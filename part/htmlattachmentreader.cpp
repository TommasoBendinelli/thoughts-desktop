// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmlattachmentreader.h"

#include "documentfeedback.h"
#include "htmldocumentview.h"
#include "reviewcardfeedback.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace Thoughts
{
namespace
{
QString str(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

QJsonObject primaryOf(const QJsonObject &card)
{
    return card.value(QStringLiteral("primary")).toObject();
}

bool validHash(const QString &hash)
{
    if (hash.size() > 512 || (!hash.isEmpty() && !hash.startsWith(QLatin1Char('#')))) {
        return false;
    }
    return std::none_of(hash.cbegin(), hash.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
}
}

HtmlAttachmentReader::HtmlAttachmentReader(HtmlDocumentView *view, QObject *parent)
    : QObject(parent)
    , m_view(view)
{
    connect(m_view, &HtmlDocumentView::ready, this, [this](const QJsonObject &) {
        if (!m_view->isAttachment() || !isOpen()) {
            return;
        }
        // Also after the page reloaded itself: a new document, everything is placed again.
        m_ready = true;
        pushItems();
        if (!m_reviewCardId.isEmpty()) {
            presentNow();
        }
        const auto pending = std::exchange(m_afterReady, {});
        for (const auto &run : pending) {
            run();
        }
        Q_EMIT ready();
    });
    connect(m_view, &HtmlDocumentView::viewerMessage, this, [this](const QJsonObject &message) {
        if (m_view->isAttachment()) {
            onViewerMessage(message);
        }
    });
    connect(m_view, &HtmlDocumentView::loadFailed, this, [this](const QString &error) {
        if (!isOpen()) {
            return;
        }
        m_ready = false;
        m_loadError = error;
        m_view->showMessage(tr("The HTML attachment could not be shown: %1").arg(error));
        Q_EMIT loadFailed(error);
        // A review waiting for the page shows the stored answer instead.
        if (!m_reviewCardId.isEmpty() && m_reviewState != QLatin1String("not_found")) {
            m_reviewState = QStringLiteral("not_found");
            Q_EMIT reviewMaskChanged();
            Q_EMIT answerNotFound(m_reviewCardId);
        }
    });
}

QJsonObject HtmlAttachmentReader::loadOptions() const
{
    // The page keeps its look and its own context menu (Copy is the web view's).
    return {{QStringLiteral("mode"), QStringLiteral("attachment")},
            {QStringLiteral("contextMenu"), QStringLiteral("native")},
            // Except on selected text and feedback, where Like, Dislike and Comment… are.
            {QStringLiteral("selectionMenu"), QStringLiteral("app")},
            {QStringLiteral("forwardModifierKeys"), false},
            {QStringLiteral("forwardKeys"), QJsonArray::fromStringList(m_forwardedKeys ? m_forwardedKeys() : QStringList())}};
}

bool HtmlAttachmentReader::open(const HtmlAttachmentTarget &target, const Owner &owner, QString *error)
{
    if (!target.isValid() || !m_view) {
        if (error) *error = tr("The HTML attachment is not on this computer.");
        return false;
    }
    m_owner = owner;
    if (!m_live && m_target.file == target.file && m_target.path == target.path && m_url.isValid() && m_view->isAttachment()) {
        return true;
    }
    m_live = false;
    ++m_generation;
    m_ready = false;
    m_loadError.clear();
    m_afterReady.clear();
    m_placement = {};
    m_lastSelection = {};
    m_pageState = {};
    // One loopback origin per shown page: pages cannot read each other's browser state.
    delete m_serverOwner;
    m_serverOwner = new QObject(this);
    m_url = serveHtmlAttachmentFile(target.file, m_serverOwner);
    if (!m_url.isValid() || m_url.isEmpty()) {
        m_target = {};
        m_url = {};
        if (error) *error = tr("This HTML page could not be opened: it must be a UTF-8 HTML file no larger than 100 MB.");
        return false;
    }
    m_target = target;
    m_view->loadAttachment(m_url, loadOptions());
    return true;
}

bool HtmlAttachmentReader::openLive(const HtmlAttachmentTarget &target, const Owner &owner, const QUrl &address, bool load)
{
    if (!target.isValid() || !address.isValid() || !m_view) {
        return false;
    }
    m_owner = owner;
    if (m_live && m_target.path == target.path && m_url == address && (m_view->isLive() || !load)) {
        m_target = target;
        return true;
    }
    ++m_generation;
    m_ready = false;
    m_loadError.clear();
    m_afterReady.clear();
    m_placement = {};
    m_lastSelection = {};
    m_pageState = {};
    // No loopback server: the page is at its own address.
    delete m_serverOwner;
    m_serverOwner = nullptr;
    m_live = true;
    m_target = target;
    m_url = address;
    if (load) {
        m_view->loadLive(address);
    } else {
        m_view->unload();
    }
    return true;
}

void HtmlAttachmentReader::loadLiveNow()
{
    if (m_live && m_view && m_url.isValid()) {
        m_view->loadLive(m_url);
    }
}

void HtmlAttachmentReader::close()
{
    ++m_generation;
    m_live = false;
    m_ready = false;
    m_afterReady.clear();
    m_target = {};
    m_url = {};
    m_loadError.clear();
    m_placement = {};
    m_lastSelection = {};
    m_pageState = {};
    m_reviewCard = {};
    m_reviewCardId.clear();
    m_reviewState.clear();
    m_reviewBlocks = 0;
    if (m_view) {
        m_view->unload();
    }
    delete m_serverOwner;
    m_serverOwner = nullptr;
}

namespace
{
QString resourceText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
}

QJsonObject HtmlAttachmentReader::liveSelectionArguments()
{
    return {{QStringLiteral("context"), ReviewCardFeedback::contextLength}, {QStringLiteral("longest"), ReviewCardFeedback::maximumQuote}};
}

QJsonObject HtmlAttachmentReader::liveSelection(const QString &scriptResult)
{
    const QJsonObject answer = QJsonDocument::fromJson(scriptResult.toUtf8()).object();
    // The page's text, never taken beyond what a quote may hold (attachmentAnchor cuts it exactly);
    // a prefix keeps its end, the text right before the selection.
    const auto text = [&answer](const QString &key, qsizetype longest, bool end = false) {
        const QString value = answer.value(key).toString();
        return value.size() <= longest ? value : end ? value.right(longest) : value.left(longest);
    };
    const QString exact = text(QStringLiteral("exact"), ReviewCardFeedback::maximumQuote + 1).trimmed();
    if (exact.isEmpty()) {
        return {};
    }
    QString hash = text(QStringLiteral("hash"), 513);
    if (!validHash(hash)) {
        hash.clear();
    }
    return {{QStringLiteral("type"), QStringLiteral("selection")},
            {QStringLiteral("text"), exact},
            {QStringLiteral("text_quote"),
             QJsonObject{{QStringLiteral("exact"), exact},
                         {QStringLiteral("prefix"), text(QStringLiteral("prefix"), 4 * ReviewCardFeedback::contextLength, true)},
                         {QStringLiteral("suffix"), text(QStringLiteral("suffix"), 4 * ReviewCardFeedback::contextLength)}}},
            {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), hash}}}};
}

void HtmlAttachmentReader::readLiveSelection(std::function<void(const QJsonObject &)> done)
{
    if (!m_live || !m_view) {
        done({});
        return;
    }
    // The viewer's text index (the same quotes as on embedded pages), then the selection's reader:
    // both only in the isolated world, where the page cannot reach them.
    static const QString script =
        resourceText(QStringLiteral(":/thoughts/html-viewer/text.js")) + QLatin1Char('\n') + resourceText(QStringLiteral(":/thoughts/html-live/selection.js"));
    m_view->callLive(script, liveSelectionArguments(), [done](bool ok, const QString &result, const QString &error) {
        if (!ok) {
            qWarning().noquote() << "Live page: the selection could not be read:" << error;
        }
        done(ok ? liveSelection(result) : QJsonObject());
    });
}

void HtmlAttachmentReader::whenReady(std::function<void()> run)
{
    if (m_ready) {
        run();
    } else {
        m_afterReady.append(std::move(run));
    }
}

bool HtmlAttachmentReader::isCardOf(const QJsonObject &card, const QString &path)
{
    return isHtmlAttachmentCard(card) && !path.isEmpty()
        && str(primaryOf(card).value(QStringLiteral("html_attachment")).toObject(), QStringLiteral("path")) == path;
}

QJsonArray HtmlAttachmentReader::viewerCards(const QList<QJsonObject> &cards, const QString &reviewCardId)
{
    QJsonArray out;
    QSet<QString> seen;
    for (const QJsonObject &card : cards) {
        const QString id = str(card, QStringLiteral("card_id"));
        const QJsonObject quote = primaryOf(card).value(QStringLiteral("text_quote")).toObject();
        if (id.isEmpty() || seen.contains(id) || str(quote, QStringLiteral("exact")).trimmed().isEmpty()) {
            continue;
        }
        seen.insert(id);
        // No markers in an attachment: placed by the quote alone.
        QJsonObject item{{QStringLiteral("id"), id}, {QStringLiteral("anchors"), QJsonArray()}, {QStringLiteral("text_quote"), quote}};
        if (id == reviewCardId) {
            item.insert(QStringLiteral("selected"), true);
        }
        out.append(item);
    }
    return out;
}

QJsonObject HtmlAttachmentReader::newCardFields(const QJsonObject &selection, const HtmlAttachmentTarget &target)
{
    const QJsonObject quote = selection.value(QStringLiteral("text_quote")).toObject();
    const QString exact = str(quote, QStringLiteral("exact"));
    if (exact.trimmed().isEmpty() || !target.isValid() || target.path.isEmpty() || target.sha256.size() != 64) {
        return {};
    }
    QString hash = str(selection.value(QStringLiteral("page_state")).toObject(), QStringLiteral("hash"));
    if (!validHash(hash)) {
        hash.clear();
    }
    return {{QStringLiteral("created_in"), QStringLiteral("html_attachment")},
            {QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), target.path}, {QStringLiteral("sha256"), target.sha256}}},
            {QStringLiteral("text_quote"),
             QJsonObject{{QStringLiteral("exact"), exact}, {QStringLiteral("prefix"), str(quote, QStringLiteral("prefix"))}, {QStringLiteral("suffix"), str(quote, QStringLiteral("suffix"))}}},
            {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), hash}}}};
}

QString HtmlAttachmentReader::storedAnswer(const QJsonObject &card)
{
    const QJsonObject primary = primaryOf(card);
    const QString selected = str(primary, QStringLiteral("selected_text"));
    return selected.isEmpty() ? str(primary.value(QStringLiteral("text_quote")).toObject(), QStringLiteral("exact")) : selected;
}

void HtmlAttachmentReader::setCards(const QJsonArray &cards)
{
    m_cards.clear();
    for (const QJsonValue &value : cards) {
        if (value.isObject() && isHtmlAttachmentCard(value.toObject())) {
            m_cards.append(value.toObject());
        }
    }
    pushItems();
}

QList<QJsonObject> HtmlAttachmentReader::pageCards() const
{
    QList<QJsonObject> out;
    for (const QJsonObject &card : m_cards) {
        if (isCardOf(card, m_target.path)) {
            out.append(card);
        }
    }
    return out;
}

void HtmlAttachmentReader::setFeedback(const QList<QJsonObject> &records)
{
    if (m_feedback == records) {
        return;
    }
    m_feedback = records;
    pushItems();
}

QList<QJsonObject> HtmlAttachmentReader::pageFeedback() const
{
    QList<QJsonObject> out;
    for (const QJsonObject &record : m_feedback) {
        // A drawing is on its snapshot, never on the live page (its quote "Drawing" is no passage).
        if (DocumentFeedback::isAttachmentFeedback(record) && !DocumentFeedback::isPageMark(record) && !record.value(QStringLiteral("deleted")).toBool()
            && str(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject(), QStringLiteral("path")) == m_target.path) {
            out.append(record);
        }
    }
    return out;
}

void HtmlAttachmentReader::pushItems()
{
    if (!m_ready || !m_view) {
        return;
    }
    QList<QJsonObject> cards = pageCards();
    // The card under review is placed even when it is not among the sidebar's cards.
    if (!m_reviewCard.isEmpty()) {
        cards.append(m_reviewCard);
    }
    QJsonArray feedback;
    for (const QJsonObject &record : pageFeedback()) {
        const QJsonObject quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject();
        feedback.append(QJsonObject{{QStringLiteral("id"), str(record, QStringLiteral("feedback_id"))}, {QStringLiteral("polarity"), str(record, QStringLiteral("polarity"))}, {QStringLiteral("text_quote"), quote}});
    }
    // As on the PDF: not while a card's answer is under review.
    const QString showFeedback = m_reviewCardId.isEmpty() ? QStringLiteral("always") : QStringLiteral("never");
    const QJsonArray args{viewerCards(cards, m_reviewCardId), QJsonArray(), feedback,
                          QJsonObject{{QStringLiteral("highlight_cards"), true}, {QStringLiteral("show_feedback"), showFeedback}}};
    m_view->call(QStringLiteral("setItems"), args, [this, generation = m_generation](bool ok, const QJsonValue &result, const QString &error) {
        if (generation != m_generation) {
            return;
        }
        if (!ok) {
            qWarning().noquote() << "HTML attachment: setItems failed:" << error;
            return;
        }
        m_placement = result.toObject();
        Q_EMIT placementChanged(m_placement);
    });
}

bool HtmlAttachmentReader::presentReview(const QJsonObject &card, bool inverted)
{
    if (!isHtmlAttachmentCard(card) || !isOpen()) {
        return false;
    }
    m_reviewCard = card;
    m_reviewCardId = str(card, QStringLiteral("card_id"));
    m_reviewInverted = inverted;
    m_reviewRevealed = false;
    m_reviewBlocks = 0;
    m_reviewState = QStringLiteral("pending");
    // A live page has no viewer to mask its text: review cards need an embedded page.
    if (!m_loadError.isEmpty() || m_live) {
        // The page cannot be shown (no web view): the stored answer, after the caller is done.
        m_reviewState = QStringLiteral("not_found");
        const QString cardId = m_reviewCardId;
        QTimer::singleShot(0, this, [this, cardId] {
            if (cardId == m_reviewCardId) Q_EMIT answerNotFound(cardId);
        });
    } else if (m_ready) {
        presentNow();
    }
    // (Not ready yet: the ready handler presents it.)
    Q_EMIT reviewMaskChanged();
    return true;
}

void HtmlAttachmentReader::presentNow()
{
    if (m_reviewCardId.isEmpty() || !m_view) {
        return;
    }
    const QString cardId = m_reviewCardId;
    const QJsonObject primary = primaryOf(m_reviewCard);
    pushItems();
    // The page state of the selection (its tab), then the mask as soon as the text is there.
    QString hash = str(primary.value(QStringLiteral("page_state")).toObject(), QStringLiteral("hash"));
    if (!validHash(hash)) {
        hash.clear();
    }
    m_view->call(QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), hash}}});
    const QJsonObject options{{QStringLiteral("inverted"), m_reviewInverted},
                              {QStringLiteral("scroll"), true},
                              {QStringLiteral("text_quote"), primary.value(QStringLiteral("text_quote"))}};
    m_view->call(QStringLiteral("presentReview"), QJsonArray{cardId, options}, [this, cardId, generation = m_generation](bool ok, const QJsonValue &result, const QString &error) {
        if (generation != m_generation || cardId != m_reviewCardId) {
            return;
        }
        if (!ok) {
            // The viewer cannot mask it: the review shows the stored answer instead.
            qWarning().noquote() << "HTML attachment: presentReview failed:" << error;
            m_reviewState = QStringLiteral("not_found");
            m_reviewBlocks = 0;
            Q_EMIT reviewMaskChanged();
            Q_EMIT answerNotFound(cardId);
            return;
        }
        const QJsonObject reply = result.toObject();
        m_reviewBlocks = reply.value(QStringLiteral("blocks")).toInt();
        if (m_reviewState != QLatin1String("not_found")) {
            m_reviewState = reply.value(QStringLiteral("pending")).toBool() ? QStringLiteral("pending") : QStringLiteral("hidden");
        }
        if (m_reviewRevealed) {
            reveal();
        }
        Q_EMIT reviewMaskChanged();
    });
}

void HtmlAttachmentReader::reveal()
{
    if (m_reviewCardId.isEmpty()) {
        return;
    }
    m_reviewRevealed = true;
    if (m_reviewState == QLatin1String("not_found") || !m_ready) {
        Q_EMIT reviewMaskChanged();
        return;
    }
    const QString cardId = m_reviewCardId;
    m_view->call(QStringLiteral("reveal"), {}, [this, cardId](bool ok, const QJsonValue &result, const QString &) {
        if (!ok || cardId != m_reviewCardId) {
            return;
        }
        m_reviewBlocks = result.toObject().value(QStringLiteral("blocks")).toInt(m_reviewBlocks);
        if (m_reviewState != QLatin1String("not_found")) {
            m_reviewState = QStringLiteral("revealed");
        }
        Q_EMIT reviewMaskChanged();
    });
}

void HtmlAttachmentReader::setReviewInverted(bool inverted)
{
    m_reviewInverted = inverted;
    if (m_ready && !m_reviewCardId.isEmpty() && m_reviewState != QLatin1String("not_found")) {
        m_view->call(QStringLiteral("setReviewInverted"), QJsonArray{inverted});
    }
}

void HtmlAttachmentReader::clearReview()
{
    const bool had = !m_reviewCardId.isEmpty();
    m_reviewCardId.clear();
    m_reviewCard = {};
    m_reviewState.clear();
    m_reviewRevealed = false;
    m_reviewInverted = false;
    m_reviewBlocks = 0;
    if (m_ready && had) {
        m_view->call(QStringLiteral("clearReview"));
        pushItems();
    }
    if (had) {
        Q_EMIT reviewMaskChanged();
    }
}

bool HtmlAttachmentReader::focusReviewBlock(int index)
{
    if (!m_ready || m_reviewCardId.isEmpty() || index < 0 || index >= m_reviewBlocks) {
        return false;
    }
    m_view->call(QStringLiteral("focusReviewMaskBlock"), QJsonArray{index});
    return true;
}

void HtmlAttachmentReader::showCard(const QJsonObject &card)
{
    if (!isCardOf(card, m_target.path)) {
        return;
    }
    showPlace(str(primaryOf(card).value(QStringLiteral("page_state")).toObject(), QStringLiteral("hash")), str(card, QStringLiteral("card_id")), QStringLiteral("card"));
}

void HtmlAttachmentReader::showFeedback(const QJsonObject &record)
{
    showPlace(str(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("page_state")).toObject(), QStringLiteral("hash")),
              str(record, QStringLiteral("feedback_id")), QStringLiteral("feedback"));
}

void HtmlAttachmentReader::showPlace(const QString &hash, const QString &id, const QString &kind)
{
    // A live page has no viewer to find the passage in: its feedback is only listed
    // (html-live-pages.md §2.1).
    if (m_live) {
        return;
    }
    const QString state = validHash(hash) ? hash : QString();
    whenReady([this, id, kind, state, generation = m_generation] {
        if (generation != m_generation) {
            return;
        }
        m_view->call(QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), state}}});
        // After the page re-rendered its tab (the viewer indexes pending DOM changes first).
        QTimer::singleShot(400, this, [this, id, kind, generation] {
            if (generation == m_generation && m_view) {
                m_view->call(QStringLiteral("scrollTo"), QJsonArray{QJsonObject{{QStringLiteral("item_id"), id}, {QStringLiteral("kind"), kind}}});
            }
        });
    });
}

void HtmlAttachmentReader::restorePosition(const QJsonObject &position)
{
    if (position.isEmpty()) {
        return;
    }
    const QString hash = str(position.value(QStringLiteral("page_state")).toObject(), QStringLiteral("hash"));
    const QJsonValue scrollY = position.value(QStringLiteral("scroll_y"));
    whenReady([this, hash, scrollY, generation = m_generation] {
        if (generation != m_generation) {
            return;
        }
        if (validHash(hash) && !hash.isEmpty()) {
            m_view->call(QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), hash}}});
        }
        // After the page re-rendered its tab, as showPlace does.
        QTimer::singleShot(hash.isEmpty() ? 0 : 400, this, [this, scrollY, generation] {
            if (generation == m_generation && m_view && scrollY.isDouble()) {
                m_view->call(QStringLiteral("scrollTo"), QJsonArray{QJsonObject{{QStringLiteral("scroll_y"), scrollY}}});
            }
        });
    });
}

void HtmlAttachmentReader::onViewerMessage(const QJsonObject &message)
{
    const QString type = str(message, QStringLiteral("type"));
    if (message.value(QStringLiteral("page_state")).isObject()) {
        m_pageState = message.value(QStringLiteral("page_state")).toObject();
    }
    if (type == QLatin1String("selection")) {
        // Only a hint (the page's own scripts can post too): a card needs the user's action.
        m_lastSelection = message.value(QStringLiteral("empty")).toBool() ? QJsonObject() : message;
        Q_EMIT selectionChanged(m_lastSelection);
    } else if (type == QLatin1String("itemTapped")) {
        const QString kind = str(message, QStringLiteral("kind"));
        if (kind == QLatin1String("card") || kind == QLatin1String("feedback")) {
            Q_EMIT itemTapped(message);
        }
    } else if (type == QLatin1String("contextmenu")) {
        if (message.value(QStringLiteral("app_menu")).toBool()) {
            Q_EMIT contextMenuRequested(message);
        }
    } else if (type == QLatin1String("placement")) {
        m_placement = message;
        Q_EMIT placementChanged(m_placement);
    } else if (type == QLatin1String("reviewMask")) {
        const QString cardId = str(message, QStringLiteral("card_id"));
        if (cardId.isEmpty() || cardId != m_reviewCardId) {
            return;
        }
        const QString state = str(message, QStringLiteral("state"));
        m_reviewBlocks = message.value(QStringLiteral("blocks")).toInt();
        if (state == QLatin1String("not_found")) {
            const bool first = m_reviewState != QLatin1String("not_found");
            m_reviewState = state;
            Q_EMIT reviewMaskChanged();
            if (first) {
                Q_EMIT answerNotFound(cardId);
            }
            return;
        }
        if (m_reviewState != QLatin1String("not_found") && (state == QLatin1String("hidden") || state == QLatin1String("revealed") || state == QLatin1String("pending"))) {
            m_reviewState = state;
        }
        Q_EMIT reviewMaskChanged();
    } else if (type == QLatin1String("key")) {
        if (message.value(QStringLiteral("forwarded")).toBool()) {
            Q_EMIT keyForwarded(message);
        }
    } else if (type == QLatin1String("error")) {
        qWarning().noquote() << "HTML attachment viewer:" << str(message, QStringLiteral("where")) << str(message, QStringLiteral("message"));
    }
}
}
