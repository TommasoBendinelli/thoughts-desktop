// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmlreader.h"

#include "htmldocumentview.h"
#include "core/htmlattachment.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace Thoughts
{
namespace
{
constexpr int MinimumTextSize = 50;
constexpr int MaximumTextSize = 200;
// While the shown version's edition is being built, the server is asked again after 20 s,
// doubling to 10 min, at most 8 times (about 40 min); the Library listing's state can be older.
constexpr int FirstPollMs = 20 * 1000;
constexpr int LongestPollMs = 10 * 60 * 1000;
constexpr int MaximumPolls = 8;

QString str(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

QStringList stringList(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString() && !item.toString().isEmpty() && !out.contains(item.toString())) {
            out.append(item.toString());
        }
    }
    return out;
}
}

HtmlReader::HtmlReader(HtmlDocumentView *view, HtmlEdition::Store *store, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_store(store)
    , m_fetcher(new HtmlEdition::Fetcher(store, this))
    , m_pollTimer(new QTimer(this))
{
    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        if (!m_active || m_availability.kind != HtmlEdition::Availability::Building || m_checking) {
            return;
        }
        ++m_polls;
        m_checking = true;
        m_fetcher->checkState(m_info.pdfSha256, m_info.filename, this, [this, generation = m_generation](std::optional<HtmlEdition::State>, const QString &) {
            m_checking = false;
            if (generation == m_generation) {
                updateAvailability(true);
                showCurrent();
            }
        });
    });
    connect(m_view, &HtmlDocumentView::ready, this, [this](const QJsonObject &) {
        m_ready = true;
        pushItems();
        const auto pending = std::exchange(m_afterReady, {});
        for (const auto &run : pending) {
            run();
        }
        Q_EMIT ready();
    });
    connect(m_view, &HtmlDocumentView::viewerMessage, this, &HtmlReader::onViewerMessage);
    connect(m_view, &HtmlDocumentView::loadFailed, this, [this](const QString &error) {
        m_ready = false;
        m_shownEdition.clear();
        m_lastError = error;
        if (m_active) {
            m_view->showMessage(tr("HTML not available: %1").arg(error));
        }
        Q_EMIT availabilityChanged();
    });
}

void HtmlReader::setDocument(const QString &pdfPath)
{
    ++m_generation;
    const bool samePath = pdfPath == m_pdfPath;
    m_pdfPath = pdfPath;
    m_info = pdfPath.isEmpty() ? HtmlEdition::LibraryInfo() : HtmlEdition::libraryInfo(pdfPath);
    m_lastError.clear();
    m_polls = 0;
    m_pollTimer->stop();
    if (!samePath) {
        m_cards.clear();
        m_comments.clear();
        m_feedback.clear();
        m_unplacedCards.clear();
        m_lastPosition = {};
        m_lastSelection = {};
    }
    if (m_info.listed && m_info.inLibrary && HtmlEdition::isSha256(m_info.pdfSha256)) {
        m_store->adoptListed(m_info.pdfSha256, m_info.filename, *m_info.listed);
    }
    const bool wantHtml = !pdfPath.isEmpty() && prefersHtml();
    if (wantHtml != m_active || wantHtml) {
        m_active = wantHtml;
        updateAvailability(wantHtml);
        showCurrent();
        Q_EMIT activeChanged(m_active);
    } else {
        updateAvailability(false);
    }
}

void HtmlReader::refreshLibraryState()
{
    if (m_pdfPath.isEmpty()) {
        return;
    }
    m_info = HtmlEdition::libraryInfo(m_pdfPath);
    if (m_info.listed && m_info.inLibrary && HtmlEdition::isSha256(m_info.pdfSha256)) {
        m_store->adoptListed(m_info.pdfSha256, m_info.filename, *m_info.listed);
    }
    // A pending edition is asked for again: it changes state without a new PDF or Library
    // generation, so the listing (even a freshly synced one) can still say "pending".
    if (m_active && m_store->availability(m_info.pdfSha256, m_info.sourceLinked).kind == HtmlEdition::Availability::Building && !m_checking) {
        m_checking = true;
        m_fetcher->checkState(m_info.pdfSha256, m_info.filename, this, [this, generation = m_generation](std::optional<HtmlEdition::State>, const QString &) {
            m_checking = false;
            if (generation == m_generation) {
                updateAvailability(true);
                showCurrent();
            }
        });
        return;
    }
    updateAvailability(m_active);
    showCurrent();
}

QString HtmlReader::preferenceKey() const
{
    const QString identity = m_info.documentId.isEmpty() ? QFileInfo(m_pdfPath).fileName() : m_info.documentId;
    return QStringLiteral("html-view/") + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex()) + QStringLiteral("/mode");
}

bool HtmlReader::prefersHtml() const
{
    return !m_pdfPath.isEmpty() && QSettings().value(preferenceKey()).toString() == QLatin1String("html");
}

void HtmlReader::setActive(bool html, bool remember)
{
    if (remember && !m_pdfPath.isEmpty()) {
        QSettings settings;
        if (html) {
            settings.setValue(preferenceKey(), QStringLiteral("html"));
        } else {
            settings.remove(preferenceKey());
        }
    }
    const bool changed = html != m_active;
    m_active = html;
    if (html) {
        updateAvailability(true);
        showCurrent();
    }
    if (changed) {
        Q_EMIT activeChanged(m_active);
    }
}

void HtmlReader::updateAvailability(bool askServer)
{
    HtmlEdition::Availability next;
    if (m_pdfPath.isEmpty()) {
        next.kind = HtmlEdition::Availability::Unavailable;
        next.reason = tr("no document is open");
    } else if (m_info.projectReading) {
        next.kind = HtmlEdition::Availability::Unavailable;
        next.reason = tr("a project reading combines several documents");
    } else if (!m_info.inLibrary) {
        next.kind = HtmlEdition::Availability::Unavailable;
        next.reason = tr("this PDF is not in the Library");
    } else {
        next = m_store->availability(m_info.pdfSha256, m_info.sourceLinked);
        if (next.kind == HtmlEdition::Availability::Unavailable && !m_info.sourceLinked) {
            next.reason = tr("imported PDFs have no HTML edition");
        }
    }
    m_availability = next;
    if (m_active && next.kind == HtmlEdition::Availability::Building) {
        if (!m_pollTimer->isActive() && m_polls < MaximumPolls) {
            m_pollTimer->start(std::min(LongestPollMs, FirstPollMs << std::min(m_polls, 10)));
        }
    } else {
        m_pollTimer->stop();
        m_polls = 0;
    }
    if (askServer && next.kind == HtmlEdition::Availability::Unknown && !m_checking) {
        m_checking = true;
        m_fetcher->checkState(m_info.pdfSha256, m_info.filename, this, [this, generation = m_generation](std::optional<HtmlEdition::State> state, const QString &error) {
            m_checking = false;
            if (generation != m_generation) {
                return;
            }
            if (!state) {
                m_lastError = error;
                m_availability.kind = HtmlEdition::Availability::Unavailable;
                m_availability.reason = tr("the server could not be asked (%1)").arg(error);
                showCurrent();
                Q_EMIT availabilityChanged();
                return;
            }
            updateAvailability(true);
            showCurrent();
        });
    }
    if (askServer && next.kind == HtmlEdition::Availability::NotDownloaded && !m_downloading) {
        m_downloading = true;
        m_fetcher->install(next.editionSha, this, [this, generation = m_generation](bool ok, const QString &error) {
            m_downloading = false;
            if (generation != m_generation) {
                return;
            }
            if (!ok) {
                m_availability.kind = HtmlEdition::Availability::Unavailable;
                m_availability.reason = tr("the HTML edition could not be downloaded (%1)").arg(error);
                showCurrent();
                Q_EMIT availabilityChanged();
                return;
            }
            updateAvailability(false);
            showCurrent();
        });
    }
    Q_EMIT availabilityChanged();
}

QString HtmlReader::availabilityText() const
{
    switch (m_availability.kind) {
    case HtmlEdition::Availability::Ready:
        return QString();
    case HtmlEdition::Availability::Building:
        return tr("HTML is being built. The PDF is not affected; the HTML view opens once the server has finished it.");
    case HtmlEdition::Availability::NotDownloaded:
        return m_downloading ? tr("Downloading the HTML edition…") : tr("The HTML edition is ready on the server.");
    case HtmlEdition::Availability::Unknown:
        return m_checking ? tr("Checking for an HTML edition…") : tr("No HTML edition is known for this version yet.");
    case HtmlEdition::Availability::Unavailable:
        break;
    }
    return tr("HTML not available: %1").arg(m_availability.reason);
}

void HtmlReader::showCurrent()
{
    if (!m_active || !m_view) {
        return;
    }
    if (m_availability.kind == HtmlEdition::Availability::Ready) {
        if (m_shownEdition != m_availability.editionSha) {
            loadShownEdition(m_availability.editionSha);
        } else if (!m_view->message().isEmpty() && m_manifest) {
            // A message replaced the page (e.g. while a newer version was checked): load again.
            loadShownEdition(m_availability.editionSha);
        }
        return;
    }
    m_shownEdition.clear();
    m_manifest.reset();
    m_ready = false;
    m_view->showMessage(availabilityText());
}

void HtmlReader::loadShownEdition(const QString &editionSha)
{
    m_manifest = m_store->manifest(editionSha);
    if (!m_manifest) {
        m_view->showMessage(tr("HTML not available: the downloaded edition is damaged"));
        return;
    }
    m_shownEdition = editionSha;
    m_ready = false;
    m_unplacedCards.clear();
    m_view->loadEdition(m_store->editionDir(editionSha), m_manifest->entry, loadOptions());
}

QJsonObject HtmlReader::loadOptions() const
{
    return {{QStringLiteral("theme"), m_view ? m_view->currentTheme() : QStringLiteral("system")},
            {QStringLiteral("textSize"), textSize()},
            {QStringLiteral("contextMenu"), QStringLiteral("app")},
            {QStringLiteral("forwardModifierKeys"), false},
            {QStringLiteral("forwardKeys"), QJsonArray::fromStringList(m_forwardedKeys ? m_forwardedKeys() : QStringList())}};
}

int HtmlReader::textSize() const
{
    return std::clamp(QSettings().value(QStringLiteral("html-view/text-size"), 100).toInt(), MinimumTextSize, MaximumTextSize);
}

void HtmlReader::setTextSize(int percent)
{
    percent = std::clamp(percent, MinimumTextSize, MaximumTextSize);
    QSettings().setValue(QStringLiteral("html-view/text-size"), percent);
    if (m_ready) {
        m_view->call(QStringLiteral("setTextSize"), QJsonArray{percent});
    }
}

void HtmlReader::setCards(const QJsonArray &cards)
{
    m_cards.clear();
    for (const QJsonValue &value : cards) {
        // Cards made in HTML attachments live in their page, not in the edition.
        if (value.isObject() && !isHtmlAttachmentCard(value.toObject())) {
            m_cards.append(value.toObject());
        }
    }
    pushItems();
}

void HtmlReader::setComments(const QList<QJsonObject> &comments)
{
    m_comments = comments;
    pushItems();
}

void HtmlReader::setFeedback(const QList<QJsonObject> &records)
{
    m_feedback = records;
    pushItems();
}

QJsonObject HtmlReader::card(const QString &cardId) const
{
    for (const QJsonObject &record : m_cards) {
        if (str(record, QStringLiteral("card_id")) == cardId) {
            return record;
        }
    }
    return cardId == m_reviewCardId ? m_reviewCard : QJsonObject();
}

QStringList HtmlReader::notShownCardIds() const
{
    QStringList ids(m_unplacedCards.begin(), m_unplacedCards.end());
    ids.sort();
    return ids;
}

bool HtmlReader::isHtmlPending(const QJsonObject &card)
{
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    return str(primary, QStringLiteral("created_in")) == QLatin1String("html") && !primary.contains(QStringLiteral("source_anchor"))
        && !primary.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString().isEmpty();
}

QStringList HtmlReader::cardAnchors(const QJsonObject &card) const
{
    const QString id = str(card, QStringLiteral("card_id"));
    if (m_manifest) {
        const QStringList listed = m_manifest->cardAnchors(id);
        if (!listed.isEmpty()) {
            return listed;
        }
    }
    QStringList keys;
    const QJsonArray segments = card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue &value : segments) {
        const QString segment = str(value.toObject(), QStringLiteral("segment_id"));
        const QString key = QStringLiteral("card:%1:%2").arg(id, segment);
        if (!segment.isEmpty() && !keys.contains(key)) {
            keys.append(key);
        }
    }
    return keys;
}

QStringList HtmlReader::cardQuestionAnchors(const QJsonObject &card) const
{
    return m_manifest ? m_manifest->cardQuestionAnchors(str(card, QStringLiteral("card_id"))) : QStringList();
}

bool HtmlReader::canPresent(const QJsonObject &card) const
{
    if (!m_active || m_shownEdition.isEmpty() || !m_manifest) {
        return false;
    }
    const QString id = str(card, QStringLiteral("card_id"));
    if (m_unplacedCards.contains(id)) {
        return false;
    }
    const QStringList keys = cardAnchors(card);
    if (keys.isEmpty()) {
        return isHtmlPending(card);
    }
    if (m_manifest->anchors.isEmpty()) {
        return true;
    }
    return std::any_of(keys.cbegin(), keys.cend(), [this](const QString &key) { return m_manifest->anchors.contains(key); });
}

QJsonArray HtmlReader::viewerCards() const
{
    QJsonArray out;
    bool reviewed = false;
    auto entry = [this](const QJsonObject &record) {
        QJsonObject item{{QStringLiteral("id"), str(record, QStringLiteral("card_id"))}, {QStringLiteral("anchors"), QJsonArray::fromStringList(cardAnchors(record))}};
        const QStringList question = cardQuestionAnchors(record);
        if (!question.isEmpty()) {
            item.insert(QStringLiteral("question_anchors"), QJsonArray::fromStringList(question));
        }
        if (isHtmlPending(record)) {
            item.insert(QStringLiteral("text_quote"), record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("text_quote")));
        }
        if (!m_reviewCardId.isEmpty() && str(record, QStringLiteral("card_id")) == m_reviewCardId) {
            item.insert(QStringLiteral("selected"), true);
        }
        return item;
    };
    for (const QJsonObject &record : m_cards) {
        reviewed |= str(record, QStringLiteral("card_id")) == m_reviewCardId;
        out.append(entry(record));
    }
    if (!reviewed && !m_reviewCard.isEmpty()) {
        out.append(entry(m_reviewCard));
    }
    return out;
}

QJsonArray HtmlReader::viewerComments() const
{
    QJsonArray out;
    const QString stem = QFileInfo(m_pdfPath).completeBaseName();
    for (const QJsonObject &record : m_comments) {
        if (record.value(QStringLiteral("deleted")).toBool() || !str(record, QStringLiteral("parent_id")).isEmpty()) {
            continue;
        }
        const QJsonObject origin = record.value(QStringLiteral("origin")).toObject();
        if (str(origin, QStringLiteral("collection")) != stem) {
            continue;
        }
        const QString id = str(record, QStringLiteral("comment_id"));
        const QJsonObject scope = record.value(QStringLiteral("scope")).toObject();
        QStringList anchors = m_manifest ? m_manifest->commentAnchors(id) : QStringList();
        if (anchors.isEmpty() && str(scope, QStringLiteral("state")) == QLatin1String("source_linked")) {
            anchors.append(QStringLiteral("comment:") + (str(scope, QStringLiteral("anchor_id")).isEmpty() ? id : str(scope, QStringLiteral("anchor_id"))));
        }
        QJsonObject item{{QStringLiteral("id"), id}, {QStringLiteral("anchors"), QJsonArray::fromStringList(anchors)}};
        if (!str(record, QStringLiteral("selected_text")).isEmpty()) {
            item.insert(QStringLiteral("selected_text"), str(record, QStringLiteral("selected_text")));
        }
        if (origin.value(QStringLiteral("text_quote")).isObject()) {
            item.insert(QStringLiteral("text_quote"), origin.value(QStringLiteral("text_quote")));
        }
        if (str(record, QStringLiteral("subtype")).compare(QLatin1String("StrikeOut"), Qt::CaseInsensitive) == 0) {
            item.insert(QStringLiteral("strike"), true);
        }
        out.append(item);
    }
    return out;
}

QJsonArray HtmlReader::viewerFeedback() const
{
    QJsonArray out;
    for (const QJsonObject &record : m_feedback) {
        if (record.value(QStringLiteral("deleted")).toBool()) {
            continue;
        }
        const QJsonObject quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject();
        if (str(quote, QStringLiteral("exact")).isEmpty()) {
            continue;
        }
        out.append(QJsonObject{{QStringLiteral("id"), str(record, QStringLiteral("feedback_id"))},
                               {QStringLiteral("card_id"), m_reviewCardId.isEmpty() ? str(record, QStringLiteral("card_id")) : m_reviewCardId},
                               {QStringLiteral("polarity"), str(record, QStringLiteral("polarity"))},
                               {QStringLiteral("text_quote"), quote}});
    }
    return out;
}

void HtmlReader::pushItems()
{
    if (!m_ready || !m_view) {
        return;
    }
    const QJsonArray args{viewerCards(), viewerComments(), viewerFeedback(), QJsonObject{{QStringLiteral("show_feedback"), QStringLiteral("revealed")}}};
    m_view->call(QStringLiteral("setItems"), args, [this, generation = m_generation](bool ok, const QJsonValue &result, const QString &error) {
        if (generation != m_generation) {
            return;
        }
        if (!ok) {
            qWarning().noquote() << "HTML view: setItems failed:" << error;
            return;
        }
        const QJsonObject report = result.toObject();
        QSet<QString> unplaced;
        for (const QJsonValue &value : report.value(QStringLiteral("cards")).toObject().value(QStringLiteral("unplaced")).toArray()) {
            unplaced.insert(str(value.toObject(), QStringLiteral("id")));
        }
        const bool changed = unplaced != m_unplacedCards;
        m_unplacedCards = unplaced;
        if (changed) {
            Q_EMIT notShownChanged(notShownCardIds());
        }
        Q_EMIT itemsPlaced(report);
    });
}

bool HtmlReader::presentReview(const QJsonObject &card, bool inverted)
{
    if (!canPresent(card)) {
        return false;
    }
    m_reviewCard = card;
    m_reviewCardId = str(card, QStringLiteral("card_id"));
    m_reviewInverted = inverted;
    m_reviewRevealed = false;
    m_reviewBlocks = 0;
    const QString cardId = m_reviewCardId;
    auto present = [this, cardId, card, generation = m_generation] {
        if (generation != m_generation || cardId != m_reviewCardId) {
            return;
        }
        pushItems();
        // The card is among the items just pushed (anchors, or its quote while unlinked).
        const QJsonObject options{{QStringLiteral("inverted"), m_reviewInverted}, {QStringLiteral("scroll"), true}};
        m_view->call(QStringLiteral("presentReview"), QJsonArray{cardId, options}, [this, cardId, generation](bool ok, const QJsonValue &result, const QString &error) {
            if (generation != m_generation || cardId != m_reviewCardId) {
                return;
            }
            if (!ok) {
                m_reviewCardId.clear();
                m_reviewCard = {};
                m_reviewBlocks = 0;
                Q_EMIT reviewUnavailable(cardId, error);
                return;
            }
            m_reviewBlocks = result.toObject().value(QStringLiteral("blocks")).toInt();
            if (m_reviewRevealed) {
                m_view->call(QStringLiteral("reveal"));
            }
            Q_EMIT reviewMaskChanged();
        });
    };
    if (m_ready) {
        present();
    } else {
        m_afterReady.append(present);
    }
    return true;
}

void HtmlReader::reveal()
{
    if (m_reviewCardId.isEmpty()) {
        return;
    }
    m_reviewRevealed = true;
    if (m_ready) {
        m_view->call(QStringLiteral("reveal"), {}, [this](bool ok, const QJsonValue &result, const QString &) {
            if (ok) {
                m_reviewBlocks = result.toObject().value(QStringLiteral("blocks")).toInt(m_reviewBlocks);
                Q_EMIT reviewMaskChanged();
            }
        });
    }
}

void HtmlReader::setReviewInverted(bool inverted)
{
    m_reviewInverted = inverted;
    if (m_ready && !m_reviewCardId.isEmpty()) {
        m_view->call(QStringLiteral("setReviewInverted"), QJsonArray{inverted});
    }
}

void HtmlReader::clearReview()
{
    const bool had = !m_reviewCardId.isEmpty();
    m_reviewCardId.clear();
    m_reviewCard = {};
    m_reviewRevealed = false;
    m_reviewInverted = false;
    m_reviewBlocks = 0;
    m_feedback.clear();
    if (m_ready && had) {
        m_view->call(QStringLiteral("clearReview"));
        pushItems();
    }
}

bool HtmlReader::focusReviewBlock(int index)
{
    if (!m_ready || m_reviewCardId.isEmpty() || index < 0 || index >= m_reviewBlocks) {
        return false;
    }
    m_view->call(QStringLiteral("focusReviewMaskBlock"), QJsonArray{index});
    return true;
}

void HtmlReader::scrollTo(const QJsonValue &target)
{
    auto run = [this, target] { m_view->call(QStringLiteral("scrollTo"), QJsonArray{target}); };
    if (m_ready) {
        run();
    } else {
        m_afterReady.append(run);
    }
}

void HtmlReader::onViewerMessage(const QJsonObject &message)
{
    const QString type = str(message, QStringLiteral("type"));
    if (type == QLatin1String("selection")) {
        m_lastSelection = message.value(QStringLiteral("empty")).toBool() ? QJsonObject() : message;
        Q_EMIT selectionChanged(m_lastSelection);
    } else if (type == QLatin1String("contextmenu")) {
        Q_EMIT contextMenuRequested(message);
    } else if (type == QLatin1String("itemTapped")) {
        Q_EMIT itemTapped(message);
    } else if (type == QLatin1String("link")) {
        if (str(message, QStringLiteral("kind")) != QLatin1String("internal")) {
            Q_EMIT linkActivated(message);
        }
    } else if (type == QLatin1String("position")) {
        m_lastPosition = message;
    } else if (type == QLatin1String("reviewMask")) {
        if (str(message, QStringLiteral("card_id")) == m_reviewCardId && !m_reviewCardId.isEmpty()) {
            m_reviewBlocks = message.value(QStringLiteral("blocks")).toInt();
            Q_EMIT reviewMaskChanged();
        }
    } else if (type == QLatin1String("key")) {
        if (message.value(QStringLiteral("forwarded")).toBool()) {
            Q_EMIT keyForwarded(message);
        }
    } else if (type == QLatin1String("error")) {
        qWarning().noquote() << "HTML viewer:" << str(message, QStringLiteral("where")) << str(message, QStringLiteral("message"));
    }
}
}
