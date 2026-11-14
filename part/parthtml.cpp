// SPDX-License-Identifier: GPL-2.0-or-later
// The Part's HTML view (thoughts-html-view-SPEC.md, "Desktop"): the PDF | HTML switch, the
// edition shown beside the page view, its selection menu (New review card, Comment, Read Aloud,
// Copy, Like/Dislike in a revealed answer), links, and reviews presented in the edition.
#include "part.h"

#include "core/document.h"
#include "core/htmlattachment.h"
#include "fedorarepository.h"
#include "feedbackversion.h"
#include "htmldocumentview.h"
#include "htmledition.h"
#include "htmlreader.h"
#include "pageview.h"
#include "readaloud.h"
#include "readaloudbar.h"
#include "reviewcardfeedback.h"
#include "reviewfeedbackeditor.h"
#include "reviewfeedbackoverlay.h"
#include "settings.h"
#include "thoughtsplatform.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageBox>
#include <KToggleAction>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QShortcut>
#include <QStackedWidget>
#include <QUrlQuery>
#include <QUuid>

namespace
{
QString str(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// thoughts-link:KIND?k=v&… (values percent-encoded, '/' kept) -> kind + parameters.
std::pair<QString, QHash<QString, QString>> parseThoughtsLink(const QString &href)
{
    QHash<QString, QString> params;
    if (!href.startsWith(QLatin1String("thoughts-link:"))) {
        return {QString(), params};
    }
    const QString rest = href.mid(int(qstrlen("thoughts-link:")));
    const qsizetype question = rest.indexOf(QLatin1Char('?'));
    const QString kind = question < 0 ? rest : rest.left(question);
    if (question >= 0) {
        const QStringList pairs = rest.mid(question + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts);
        for (const QString &pair : pairs) {
            const qsizetype equals = pair.indexOf(QLatin1Char('='));
            const QString key = QUrl::fromPercentEncoding((equals < 0 ? pair : pair.left(equals)).toUtf8());
            const QString value = equals < 0 ? QString() : QUrl::fromPercentEncoding(pair.mid(equals + 1).toUtf8());
            params.insert(key, value);
        }
    }
    return {kind, params};
}

// The viewer's forwardKeys name of a Qt shortcut ("s", "Escape", "Meta+Shift+u"), or empty
// when the page cannot report it. On macOS Qt's Ctrl is ⌘ (the page's Meta) unless swapped back.
bool macCommandIsControl()
{
#ifdef Q_OS_MACOS
    return !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
#else
    return false;
#endif
}

QString viewerKeyName(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QLatin1Char(char('a' + (key - Qt::Key_A))));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return QStringLiteral("Digit%1").arg(key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    switch (key) {
    case Qt::Key_Escape:
        return QStringLiteral("Escape");
    case Qt::Key_Space:
        return QStringLiteral("Space");
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("Enter");
    case Qt::Key_Left:
        return QStringLiteral("ArrowLeft");
    case Qt::Key_Right:
        return QStringLiteral("ArrowRight");
    case Qt::Key_Up:
        return QStringLiteral("ArrowUp");
    case Qt::Key_Down:
        return QStringLiteral("ArrowDown");
    default:
        return {};
    }
}

QString viewerKeySpec(const QKeyCombination &combination)
{
    const QString name = viewerKeyName(combination.key());
    if (name.isEmpty()) return {};
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    const bool swap = macCommandIsControl();
    QStringList parts;
    if (modifiers & (swap ? Qt::ControlModifier : Qt::MetaModifier)) parts << QStringLiteral("Meta");
    if (modifiers & (swap ? Qt::MetaModifier : Qt::ControlModifier)) parts << QStringLiteral("Ctrl");
    if (modifiers & Qt::AltModifier) parts << QStringLiteral("Alt");
    if (modifiers & Qt::ShiftModifier) parts << QStringLiteral("Shift");
    parts << name;
    return parts.join(QLatin1Char('+'));
}

// A forwarded key message ({key, code, meta, ctrl, alt, shift}) as a Qt key combination.
QKeyCombination keyFromViewer(const QJsonObject &message)
{
    const QString code = message.value(QStringLiteral("code")).toString();
    const QString key = message.value(QStringLiteral("key")).toString();
    int qtKey = Qt::Key_unknown;
    if (code.size() == 4 && code.startsWith(QLatin1String("Key")) && code.at(3) >= QLatin1Char('A') && code.at(3) <= QLatin1Char('Z')) {
        qtKey = Qt::Key_A + (code.at(3).unicode() - 'A');
    } else if (code.size() == 6 && code.startsWith(QLatin1String("Digit")) && code.at(5).isDigit()) {
        qtKey = Qt::Key_0 + (code.at(5).unicode() - '0');
    } else {
        for (int candidate : {int(Qt::Key_Escape), int(Qt::Key_Space), int(Qt::Key_Return), int(Qt::Key_Left), int(Qt::Key_Right), int(Qt::Key_Up), int(Qt::Key_Down)}) {
            const QString name = viewerKeyName(candidate);
            if (name == code || name == key) {
                qtKey = candidate;
                break;
            }
        }
        for (int f = 1; qtKey == Qt::Key_unknown && f <= 12; ++f) {
            if (key == QStringLiteral("F%1").arg(f)) qtKey = Qt::Key_F1 + f - 1;
        }
    }
    if (qtKey == Qt::Key_unknown) return QKeyCombination(Qt::Key_unknown);
    const bool swap = macCommandIsControl();
    Qt::KeyboardModifiers modifiers;
    if (message.value(QStringLiteral("meta")).toBool()) modifiers |= swap ? Qt::ControlModifier : Qt::MetaModifier;
    if (message.value(QStringLiteral("ctrl")).toBool()) modifiers |= swap ? Qt::MetaModifier : Qt::ControlModifier;
    if (message.value(QStringLiteral("alt")).toBool()) modifiers |= Qt::AltModifier;
    if (message.value(QStringLiteral("shift")).toBool()) modifiers |= Qt::ShiftModifier;
    return QKeyCombination(modifiers, Qt::Key(qtKey));
}

QString safePdfFilename(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"));
    return pattern.match(name).hasMatch() ? name : QString();
}
}

namespace Okular
{
void Part::setupHtmlView(QWidget *parent)
{
    m_documentStack = new QStackedWidget(parent);
    m_documentStack->setObjectName(QStringLiteral("thoughtsDocumentViews"));
    m_documentStack->addWidget(m_pageView);
    m_htmlView = new Thoughts::HtmlDocumentView(m_documentStack);
    m_documentStack->addWidget(m_htmlView);
    m_documentStack->setCurrentWidget(m_pageView);
    m_htmlStore = std::make_unique<Thoughts::HtmlEdition::Store>(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/html-editions")));
    m_htmlReader = new Thoughts::HtmlReader(m_htmlView, m_htmlStore.get(), this);

    m_htmlViewAction = new KToggleAction(QIcon::fromTheme(QStringLiteral("text-html")), i18n("HTML View"), this);
    m_htmlViewAction->setObjectName(QStringLiteral("view_html_edition"));
    m_htmlViewAction->setToolTip(i18n("Show this document as HTML (reflowing text) instead of the PDF"));
    actionCollection()->addAction(QStringLiteral("view_html_edition"), m_htmlViewAction);
    connect(m_htmlViewAction, &QAction::triggered, this, [this](bool checked) { setDocumentView(checked ? QStringLiteral("html") : QStringLiteral("pdf")); });

    // The bar above the edition: notices ("2 cards are not shown…"), text size, back to the PDF.
    m_htmlView->setTextSizeHandler([this](int delta) {
        const int size = delta == 0 ? 100 : m_htmlReader->textSize() + delta;
        m_htmlReader->setTextSize(size);
        m_htmlView->setTextSizeLabel(m_htmlReader->textSize());
    });
    m_htmlView->setTextSizeLabel(m_htmlReader->textSize());
    connect(m_htmlView, &Thoughts::HtmlDocumentView::showPdfRequested, this, [this] { setDocumentView(QStringLiteral("pdf")); });
    connect(m_htmlView, &Thoughts::HtmlDocumentView::noticeClicked, this, &Part::showHtmlNotShownMenu);

    connect(m_htmlReader, &Thoughts::HtmlReader::activeChanged, this, &Part::htmlViewActiveChanged);
    connect(m_htmlReader, &Thoughts::HtmlReader::availabilityChanged, this, &Part::updateHtmlViewAction);
    connect(m_htmlReader, &Thoughts::HtmlReader::contextMenuRequested, this, &Part::showHtmlContextMenu);
    connect(m_htmlReader, &Thoughts::HtmlReader::itemTapped, this, &Part::htmlItemTapped);
    connect(m_htmlReader, &Thoughts::HtmlReader::linkActivated, this, &Part::openHtmlLink);
    m_htmlReader->setForwardedKeysProvider([this] { return htmlForwardedKeys(); });
    connect(m_htmlReader, &Thoughts::HtmlReader::keyForwarded, this, &Part::handleHtmlKey);
    connect(m_htmlReader, &Thoughts::HtmlReader::ready, this, [this] {
        refreshHtmlComments();
        refreshHtmlFeedback();
    });
    connect(m_htmlReader, &Thoughts::HtmlReader::notShownChanged, this, [this](const QStringList &ids) {
        m_htmlView->setNotice(ids.isEmpty() ? QString()
                                            : i18np("%1 card is not shown in the HTML view; it reviews in the PDF.",
                                                    "%1 cards are not shown in the HTML view; they review in the PDF.", ids.size()),
                              ids.isEmpty() ? QString() : i18n("Show"));
    });
    connect(m_htmlReader, &Thoughts::HtmlReader::reviewUnavailable, this, [this](const QString &cardId, const QString &) {
        // The page could not mask this card: review it in the PDF (not remembered).
        if (str(m_reviewCardRecord, QStringLiteral("card_id")) != cardId) return;
        // Switching to the PDF presents it there again (htmlViewActiveChanged).
        m_htmlReviewShown = false;
        m_htmlReader->setActive(false, false);
        displayInfoMessage(i18n("Not shown in HTML view: reviewing this card in the PDF."), KMessageWidget::Information, 5000);
    });
    updateHtmlViewAction();
    setupHtmlAttachmentView();
}

bool Part::htmlViewActive() const
{
    return m_htmlReader && m_htmlReader->isActive();
}

void Part::setDocumentView(const QString &view)
{
    // A page's tab has one view: its page.
    if (!m_htmlReader || m_attachmentTab) return;
    // Choosing a view of the document leaves an HTML attachment shown in its place.
    hideHtmlAttachment();
    const bool html = view == QLatin1String("html");
    if (html == m_htmlReader->isActive()) {
        updateHtmlViewAction();
        return;
    }
    // Leaving the PDF: start the HTML view where the PDF is (and the other way round).
    if (!html) mapHtmlPositionToPdf();
    m_htmlReader->setActive(html, true);
    if (html) mapPdfPositionToHtml();
}

QVariantMap Part::htmlViewState() const
{
    if (!m_htmlReader) return {};
    const auto availability = m_htmlReader->availability();
    static const char *const kinds[] = {"ready", "building", "not_downloaded", "unavailable", "unknown"};
    return {{QStringLiteral("active"), m_htmlReader->isActive()},
            {QStringLiteral("web_view"), Thoughts::HtmlDocumentView::webViewAvailable()},
            {QStringLiteral("availability"), QString::fromLatin1(kinds[availability.kind])},
            {QStringLiteral("reason"), availability.reason},
            {QStringLiteral("message"), m_htmlView ? m_htmlView->message() : QString()},
            {QStringLiteral("edition"), m_htmlReader->shownEditionSha()},
            {QStringLiteral("ready"), m_htmlReader->isEditionShown()},
            {QStringLiteral("review_card"), m_htmlReader->reviewCardId()},
            {QStringLiteral("review_blocks"), m_htmlReader->reviewBlockCount()},
            {QStringLiteral("review_revealed"), m_htmlReader->reviewRevealed()},
            {QStringLiteral("not_shown"), m_htmlReader->notShownCardIds()}};
}

void Part::updateHtmlViewAction()
{
    if (!m_htmlViewAction || !m_htmlReader) return;
    m_htmlViewAction->setChecked(m_htmlReader->isActive());
    const auto info = m_htmlReader->libraryInfo();
    const bool document = !m_htmlReader->documentPath().isEmpty();
    m_htmlViewAction->setEnabled(document);
    const QString text = m_htmlReader->availabilityText();
    m_htmlViewAction->setToolTip(text.isEmpty() ? i18n("Show this document as HTML (reflowing text) instead of the PDF") : text);
    Q_UNUSED(info)
}

void Part::htmlViewActiveChanged(bool active)
{
    if (!m_documentStack) return;
    // An HTML attachment stays in front (e.g. a reload of the PDF); Back shows the view chosen now.
    if (m_attachmentShown) {
        if (active) refreshHtmlComments();
        updateHtmlViewAction();
        return;
    }
    const bool wasHtml = m_documentStack->currentWidget() == m_htmlView;
    m_documentStack->setCurrentWidget(active ? static_cast<QWidget *>(m_htmlView) : static_cast<QWidget *>(m_pageView.data()));
    // The review panel cannot float over a native web view (it would be hidden behind it): the
    // review overview watches this property and makes its panel a tool window meanwhile.
    widget()->setProperty("thoughtsNativeDocumentView", active && Thoughts::HtmlDocumentView::webViewAvailable());
    if (QWidget *container = widget()->findChild<QWidget *>(QStringLiteral("thoughtsPageSplitter"))) container->parentWidget()->setFocusProxy(active ? static_cast<QWidget *>(m_htmlView) : static_cast<QWidget *>(m_pageView.data()));
    if (active) refreshHtmlComments();
    // A review in progress follows the view: the review overview presents the card again (in
    // the view now shown, revealed or flipped as it was).
    const QString reviewCard = str(m_reviewCardRecord, QStringLiteral("card_id"));
    if (wasHtml != active && !reviewCard.isEmpty()) {
        if (wasHtml) {
            m_htmlReader->clearReview();
            m_htmlReviewShown = false;
        } else if (m_pageView) {
            m_pageView->clearReviewMask();
        }
        Q_EMIT reviewPresentationLost(reviewCard);
    }
    updateHtmlViewAction();
}

namespace
{
// A card's answer regions as {page (1-based), top}.
QList<std::pair<int, double>> cardRegionTops(const QJsonObject &card)
{
    QList<std::pair<int, double>> tops;
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    QJsonArray regions = primary.value(QStringLiteral("regions")).toArray();
    if (regions.isEmpty() && primary.value(QStringLiteral("page")).toInt() > 0) {
        regions.append(QJsonObject{{QStringLiteral("page"), primary.value(QStringLiteral("page"))}, {QStringLiteral("rect"), primary.value(QStringLiteral("rect"))}});
    }
    for (const QJsonValue &value : regions) {
        const QJsonObject region = value.toObject();
        const int page = region.value(QStringLiteral("page")).toInt();
        const QJsonArray rect = region.value(QStringLiteral("rect")).toArray();
        if (page > 0 && rect.size() == 4) tops.append({page, rect.at(1).toDouble()});
    }
    std::sort(tops.begin(), tops.end());
    return tops;
}
}

void Part::mapPdfPositionToHtml()
{
    if (!m_htmlReader || !m_document->isOpened()) return;
    // The first card answer on the current page, else the same fraction of the document.
    const int page = int(m_document->currentPage()) + 1;
    QString best;
    double bestTop = 2.0;
    for (const QJsonObject &card : m_htmlReader->cards()) {
        const QStringList keys = m_htmlReader->cardAnchors(card);
        if (keys.isEmpty()) continue;
        for (const auto &[cardPage, top] : cardRegionTops(card)) {
            if (cardPage == page && top < bestTop) {
                bestTop = top;
                best = keys.first();
            }
        }
    }
    if (!best.isEmpty()) {
        m_htmlReader->scrollTo(best);
        return;
    }
    const int pages = int(m_document->pages());
    const double fraction = pages > 1 ? double(page - 1) / double(pages - 1) : 0.0;
    m_htmlReader->scrollTo(QJsonObject{{QStringLiteral("scroll_fraction"), fraction}});
}

void Part::mapHtmlPositionToPdf()
{
    if (!m_htmlReader || !m_htmlReader->isEditionShown() || !m_document->isOpened()) return;
    const QJsonObject position = m_htmlReader->lastPosition();
    const QString key = str(position, QStringLiteral("anchor_key"));
    if (key.startsWith(QLatin1String("card:"))) {
        const QJsonObject card = m_htmlReader->card(key.section(QLatin1Char(':'), 1, 1));
        const auto tops = cardRegionTops(card);
        if (!tops.isEmpty() && tops.first().first <= int(m_document->pages())) {
            Okular::DocumentViewport viewport(tops.first().first - 1);
            viewport.rePos.enabled = true;
            viewport.rePos.normalizedX = 0.0;
            viewport.rePos.normalizedY = std::max(0.0, tops.first().second - 0.05);
            viewport.rePos.pos = Okular::DocumentViewport::TopLeft;
            m_document->setViewport(viewport);
            return;
        }
    }
    if (position.contains(QStringLiteral("scroll_fraction"))) {
        const int pages = int(m_document->pages());
        const double fraction = std::clamp(position.value(QStringLiteral("scroll_fraction")).toDouble(), 0.0, 1.0);
        m_document->setViewportPage(std::clamp(int(std::lround(fraction * (pages - 1))), 0, std::max(0, pages - 1)));
    }
}

void Part::refreshHtmlComments()
{
    if (!m_htmlReader || !m_htmlReader->isActive()) return;
    const QString stem = QFileInfo(localFilePath()).completeBaseName();
    QList<QJsonObject> comments;
    const QString prefix = QStringLiteral("logical_comment\ncomments\n");
    for (auto it = m_fedoraProjections.constBegin(); it != m_fedoraProjections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (str(record.value(QStringLiteral("origin")).toObject(), QStringLiteral("collection")) == stem) comments.append(record);
    }
    m_htmlReader->setComments(comments);
}

void Part::refreshHtmlFeedback()
{
    if (!m_htmlReader || !m_pageView) return;
    auto *overlay = m_pageView->reviewFeedback();
    if (!overlay->hasCard() || overlay->cardId() != m_htmlReader->reviewCardId()) {
        m_htmlReader->setFeedback({});
        return;
    }
    QList<QJsonObject> records;
    const auto items = overlay->items();
    for (const auto &item : items) {
        if (item.state != QLatin1String("orphaned") && item.state != QLatin1String("earlier")) records.append(item.record);
    }
    m_htmlReader->setFeedback(records);
}

bool Part::presentHtmlReview(const QVariantList &regions)
{
    m_reviewCardRegions = regions;
    m_htmlReviewShown = false;
    if (!m_htmlReader || m_reviewCardRecord.isEmpty()) return false;
    const bool pendingHtmlCard = Thoughts::HtmlReader::isHtmlPending(m_reviewCardRecord) && regions.isEmpty();
    // A card made in the HTML view has no place in the PDF until the next Sync links it.
    if (pendingHtmlCard && !m_htmlReader->isActive() && m_htmlReader->availability().kind == Thoughts::HtmlEdition::Availability::Ready) {
        m_htmlReader->setActive(true, false);
    }
    if (!m_htmlReader->isActive()) {
        if (pendingHtmlCard) {
            displayInfoMessage(i18n("Placed at next Sync — review it in the HTML view. This card was made in the HTML view; it gets its place in the PDF when the next Sync links it."),
                               KMessageWidget::Information, 8000);
        }
        return false;
    }
    if (m_htmlReader->presentReview(m_reviewCardRecord, false)) {
        // (PageView::clearReviewMask would also forget the card's answer feedback.)
        m_htmlReviewShown = true;
        return true;
    }
    if (pendingHtmlCard) {
        displayInfoMessage(i18n("Placed at next Sync — this card cannot be shown until the HTML edition is available."), KMessageWidget::Information, 6000);
        return false;
    }
    // Not shown in HTML view: the card reviews in the PDF.
    m_htmlReader->setActive(false, false);
    displayInfoMessage(i18n("Not shown in HTML view: reviewing this card in the PDF."), KMessageWidget::Information, 5000);
    return false;
}

void Part::showHtmlNotShownMenu()
{
    if (!m_htmlReader) return;
    QMenu menu(widget());
    menu.setToolTipsVisible(true);
    menu.addSection(i18n("Not shown in HTML view"));
    for (const QString &id : m_htmlReader->notShownCardIds()) {
        const QJsonObject card = m_htmlReader->card(id);
        const QString question = str(card.value(QStringLiteral("primary")).toObject(), QStringLiteral("question"));
        QAction *action = menu.addAction(question.isEmpty() ? id : question.left(80));
        connect(action, &QAction::triggered, this, [this, card] {
            m_htmlReader->setActive(false, false);
            const auto tops = cardRegionTops(card);
            if (!tops.isEmpty()) m_document->setViewportPage(tops.first().first - 1);
        });
    }
    menu.exec(QCursor::pos());
}

QJsonObject Part::htmlSelectionFields(const QJsonObject &selection) const
{
    QJsonObject fields{{QStringLiteral("created_in"), QStringLiteral("html")},
                       {QStringLiteral("html_edition_sha256"), m_htmlReader->shownEditionSha()},
                       {QStringLiteral("text_quote"), selection.value(QStringLiteral("text_quote"))}};
    // Markdown: the exact source bytes, while the edition's source file hash is known.
    const QJsonObject range = selection.value(QStringLiteral("source_range")).toObject();
    const auto manifest = m_htmlReader->shownManifest();
    const QString file = str(range, QStringLiteral("source_file"));
    const QString sourceSha = manifest ? manifest->sourceFiles.value(file).toString() : QString();
    if (!range.isEmpty() && !file.isEmpty() && Thoughts::HtmlEdition::isSha256(sourceSha)) {
        fields.insert(QStringLiteral("source_range"), QJsonObject{{QStringLiteral("source_file"), file},
                                                                  {QStringLiteral("source_sha256"), sourceSha.toLower()},
                                                                  {QStringLiteral("start_byte"), range.value(QStringLiteral("start_byte"))},
                                                                  {QStringLiteral("end_byte"), range.value(QStringLiteral("end_byte"))},
                                                                  {QStringLiteral("exact"), range.value(QStringLiteral("exact")).toBool(true)}});
    }
    return fields;
}

void Part::showHtmlContextMenu(const QJsonObject &message)
{
    if (!m_htmlReader || !m_htmlView) return;
    const QJsonObject selection = message.value(QStringLiteral("selection")).toObject();
    const QString text = str(selection, QStringLiteral("text")).trimmed();
    const QString reviewCard = m_htmlReader->reviewCardId();
    QMenu menu(widget());
    if (!text.isEmpty()) {
        const QJsonObject answer = selection.value(QStringLiteral("answer")).toObject();
        if (!reviewCard.isEmpty() && m_htmlReader->reviewRevealed() && str(answer, QStringLiteral("card_id")) == reviewCard) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("thumb-up")), i18n("Like"), this, [this, selection] { giveHtmlFeedback(Thoughts::ReviewCardFeedback::positive(), selection, false); })
                ->setObjectName(QStringLiteral("htmlFeedbackLike"));
            menu.addAction(QIcon::fromTheme(QStringLiteral("thumb-down")), i18n("Dislike"), this, [this, selection] { giveHtmlFeedback(Thoughts::ReviewCardFeedback::negative(), selection, false); })
                ->setObjectName(QStringLiteral("htmlFeedbackDislike"));
            menu.addAction(i18n("Feedback Comment…"), this, [this, selection] { giveHtmlFeedback(Thoughts::ReviewCardFeedback::negative(), selection, true); })
                ->setObjectName(QStringLiteral("htmlFeedbackComment"));
            menu.addSeparator();
        }
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy"), this, [this] {
            m_htmlView->call(QStringLiteral("copySelection"), {}, [](bool ok, const QJsonValue &result, const QString &) {
                if (ok && !result.toString().isEmpty()) QGuiApplication::clipboard()->setText(result.toString());
            });
        })->setObjectName(QStringLiteral("htmlCopy"));
        menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), i18n("Read Aloud"), this, [this, text] { readAloudHtmlText(text); })
            ->setObjectName(QStringLiteral("htmlReadAloud"));
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Comment…"), this, [this, selection] { createHtmlComment(selection); })
            ->setObjectName(QStringLiteral("htmlComment"));
        if (reviewCard.isEmpty()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("view-task-add")), i18n("Add Review Card…"), this, [this, selection] { requestHtmlCard(selection); })
                ->setObjectName(QStringLiteral("AddReviewCard"));
        }
    }
    const QJsonArray items = message.value(QStringLiteral("items")).toArray();
    if (!items.isEmpty()) menu.addSeparator();
    QSet<QString> seen;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString kind = str(item, QStringLiteral("kind"));
        const QString id = str(item, QStringLiteral("id"));
        if (seen.contains(kind + id)) continue;
        seen.insert(kind + id);
        const QJsonObject tap{{QStringLiteral("kind"), kind}, {QStringLiteral("id"), id}};
        if ((kind == QLatin1String("card") || kind == QLatin1String("question")) && reviewCard.isEmpty()) {
            menu.addAction(i18n("Open Review Card"), this, [this, tap] { htmlItemTapped(tap); });
        } else if (kind == QLatin1String("comment")) {
            menu.addAction(i18n("Open Comment"), this, [this, tap] { htmlItemTapped(tap); });
        } else if (kind == QLatin1String("feedback")) {
            menu.addAction(i18n("Edit Feedback…"), this, [this, tap] { htmlItemTapped(tap); });
        }
    }
    if (message.value(QStringLiteral("link")).isObject()) {
        const QJsonObject link = message.value(QStringLiteral("link")).toObject();
        menu.addAction(QIcon::fromTheme(QStringLiteral("quickopen")), i18n("Open Link"), this, [this, link] {
            QJsonObject activated = link;
            if (!activated.contains(QStringLiteral("target"))) activated.insert(QStringLiteral("target"), link.value(QStringLiteral("href")));
            openHtmlLink(activated);
        });
    }
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("application-pdf")), i18n("Show the PDF"), this, [this] { setDocumentView(QStringLiteral("pdf")); });
    // At the click (viewport CSS px = Qt logical px of the web view), else at the cursor.
    QPoint at = QCursor::pos();
    const QJsonArray point = message.value(QStringLiteral("point")).toArray();
    if (point.size() == 2 && m_htmlView->webContainer()) at = m_htmlView->webContainer()->mapToGlobal(QPoint(point.at(0).toInt(), point.at(1).toInt()));
    if (!menu.isEmpty()) menu.exec(at);
}

QStringList Part::htmlForwardedKeys() const
{
    // The page keeps everything else (arrows, Space, Page Up/Down scroll it; ⌘ shortcuts of the
    // menu bar reach the menu). S: the review's Skip (a window shortcut of the review panel).
    QStringList keys{QStringLiteral("s"), QStringLiteral("Escape")};
    if (QAction *readAloud = actionCollection()->action(QStringLiteral("read_aloud_selection"))) {
        for (const QKeySequence &sequence : readAloud->shortcuts()) {
            if (sequence.count() != 1) continue;
            const QString spec = viewerKeySpec(sequence[0]);
            if (!spec.isEmpty() && !keys.contains(spec)) keys << spec;
        }
    }
    return keys;
}

QStringList Part::attachmentForwardedKeys() const
{
    // S and Escape only: Read Aloud's bar belongs to the HTML edition.
    return {QStringLiteral("s"), QStringLiteral("Escape")};
}

void Part::handleHtmlKey(const QJsonObject &message)
{
    // From the HTML view or an HTML attachment shown in place of the document.
    if (!htmlViewActive() && !m_attachmentShown) return;
    const QKeyCombination combination = keyFromViewer(message);
    if (combination.key() == Qt::Key_unknown) return;
    // The page only asks. Which keys reach the window's shortcuts is decided here, by the list
    // the viewer was given: the viewer forwards nothing else, but an attachment's own script
    // can post any message, and would otherwise fire every shortcut of the window.
    if (!(m_attachmentShown ? attachmentForwardedKeys() : htmlForwardedKeys()).contains(viewerKeySpec(combination))) return;
    const QKeySequence sequence(combination);
    // Read Aloud reads the page's selection (the PDF's action reads the PDF's).
    if (QAction *readAloud = actionCollection()->action(QStringLiteral("read_aloud_selection"));
        !m_attachmentShown && readAloud && readAloud->shortcuts().contains(sequence)) {
        if (message.value(QStringLiteral("repeat")).toBool()) return;
        readAloudHtmlText(m_htmlReader->lastSelection().value(QStringLiteral("text")).toString());
        return;
    }
    if (combination == QKeyCombination(Qt::Key_Escape)) {
        // As in the PDF: stop reading aloud, close the search bar, leave full screen.
        if (m_htmlReadAloud && m_htmlReadAloud->active()) {
            m_htmlReadAloud->stop();
            return;
        }
        if (m_closeFindBar && m_closeFindBar->isEnabled()) {
            m_closeFindBar->trigger();
            return;
        }
        if (QWidget *window = widget()->window(); window && window != widget()) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &escape);
        }
        return;
    }
    // The window's shortcuts (the review panel's S, also while it floats as a tool window over
    // the web view). Widget-scoped shortcuts need focus in their widget, which the page has.
    if (message.value(QStringLiteral("repeat")).toBool()) return;
    QWidget *window = widget()->window();
    if (!window) return;
    for (QShortcut *shortcut : window->findChildren<QShortcut *>()) {
        if (!shortcut->isEnabled() || !shortcut->keys().contains(sequence)) continue;
        if (shortcut->context() == Qt::WidgetShortcut || shortcut->context() == Qt::WidgetWithChildrenShortcut) continue;
        auto *owner = qobject_cast<QWidget *>(shortcut->parent());
        if (!owner || !owner->isVisible()) continue;
        Q_EMIT shortcut->activated();
        return;
    }
}

void Part::readAloudHtmlText(const QString &text)
{
    if (Thoughts::ReadAloud::prepareText(text).isEmpty()) {
        displayInfoMessage(i18n("There is no text to read aloud."), KMessageWidget::Information, 2500);
        return;
    }
    if (m_pageView) m_pageView->stopReadAloud();
    if (!m_htmlReadAloud) {
        m_htmlReadAloud = new Thoughts::ReadAloud::Controller(this);
        connect(m_htmlReadAloud, &Thoughts::ReadAloud::Controller::developerEvent, this, &Part::developerEvent);
        connect(m_htmlReadAloud, &Thoughts::ReadAloud::Controller::unavailable, this, [this](const QString &) {
            displayInfoMessage(i18n("Read Aloud is unavailable: the cloud voice cannot be used and there is no system voice."), KMessageWidget::Warning, 5000);
        });
        m_htmlReadAloudBar = new ReadAloudBar(m_htmlReadAloud, m_htmlView->readAloudHost());
        connect(m_htmlReadAloud, &Thoughts::ReadAloud::Controller::stateChanged, this, [this] {
            m_htmlView->setReadAloudHostVisible(m_htmlReadAloud && m_htmlReadAloud->active(), m_htmlReadAloudBar ? m_htmlReadAloudBar->sizeHint().height() : 0);
        });
    }
    m_htmlView->setReadAloudHostVisible(true, m_htmlReadAloudBar->sizeHint().height());
    m_htmlReadAloud->start(text);
}

void Part::requestHtmlCard(const QJsonObject &selection)
{
    if (!m_htmlReader || !m_htmlReader->isEditionShown()) return;
    const auto info = m_htmlReader->libraryInfo();
    const QJsonObject quote = selection.value(QStringLiteral("text_quote")).toObject();
    if (str(quote, QStringLiteral("exact")).trimmed().isEmpty()) return;
    QVariantMap fields = htmlSelectionFields(selection).toVariantMap();
    fields.insert(QStringLiteral("pdf_path"), localFilePath());
    fields.insert(QStringLiteral("pdf_stem"), QFileInfo(localFilePath()).completeBaseName());
    fields.insert(QStringLiteral("pdf_sha256"), info.pdfSha256);
    fields.insert(QStringLiteral("document_id"), info.documentId);
    fields.insert(QStringLiteral("selected_text"), str(quote, QStringLiteral("exact")));
    m_htmlView->call(QStringLiteral("clearSelection"));
    Q_EMIT requestHtmlReviewCard(localFilePath(), fields);
}

void Part::createHtmlComment(const QJsonObject &selection)
{
    if (!m_htmlReader || !m_htmlReader->isEditionShown() || !m_fedoraRepository) return;
    const QJsonObject quote = selection.value(QStringLiteral("text_quote")).toObject();
    const QString exact = str(quote, QStringLiteral("exact"));
    if (exact.trimmed().isEmpty()) return;
    bool accepted = false;
    const QString shown = exact.size() > 120 ? exact.left(117) + QStringLiteral("…") : exact;
    const QString contents = QInputDialog::getMultiLineText(widget(), i18n("Comment"), i18n("Comment on “%1”:", shown), QString(), &accepted);
    if (!accepted) return;
    const auto info = m_htmlReader->libraryInfo();
    const QString id = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString now = isoNow();
    QJsonObject origin = htmlSelectionFields(selection);
    origin.insert(QStringLiteral("collection"), QFileInfo(localFilePath()).completeBaseName());
    origin.insert(QStringLiteral("document_id"), info.documentId);
    origin.insert(QStringLiteral("pdf_sha256"), info.pdfSha256);
    origin.insert(QStringLiteral("rect"), QJsonArray());
    origin.insert(QStringLiteral("fragments"), QJsonArray());
    const QJsonObject payload{{QStringLiteral("schema_version"), 1},
                              {QStringLiteral("comment_id"), id},
                              {QStringLiteral("thread_root_id"), id},
                              {QStringLiteral("parent_id"), QJsonValue(QJsonValue::Null)},
                              {QStringLiteral("revision"), 1},
                              {QStringLiteral("created_at"), now},
                              {QStringLiteral("updated_at"), now},
                              {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()},
                              {QStringLiteral("deleted"), false},
                              {QStringLiteral("contents"), contents},
                              {QStringLiteral("author"), Okular::Settings::identityAuthor()},
                              {QStringLiteral("subtype"), QStringLiteral("Highlight")},
                              {QStringLiteral("selected_text"), exact},
                              {QStringLiteral("color"), QJsonArray{1.0, 0.92, 0.23}},
                              {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pdf_only")}, {QStringLiteral("reason"), QStringLiteral("html_pending_server_link")}}},
                              {QStringLiteral("origin"), origin}};
    m_htmlView->call(QStringLiteral("clearSelection"));
    if (!m_fedoraRepository->enqueueLogicalComment(payload)) {
        KMessageBox::error(widget(), i18n("The comment could not be saved."), i18n("Comment"));
        return;
    }
    m_fedoraProjections = m_fedoraRepository->effectiveProjections();
    refreshHtmlComments();
    displayInfoMessage(i18n("Comment saved; its marker is placed at the next Sync."), KMessageWidget::Positive, 4000);
}

void Part::giveHtmlFeedback(const QString &polarity, const QJsonObject &selection, bool withComment)
{
    if (!m_pageView || !m_htmlReader) return;
    auto *overlay = m_pageView->reviewFeedback();
    const QJsonObject answer = selection.value(QStringLiteral("answer")).toObject();
    const QString cardId = str(answer, QStringLiteral("card_id"));
    if (!overlay->hasCard() || overlay->cardId() != cardId || cardId != m_htmlReader->reviewCardId()) return;
    const QJsonObject quote = answer.value(QStringLiteral("text_quote")).toObject();
    const QString exact = str(quote, QStringLiteral("exact")).trimmed();
    if (exact.isEmpty()) return;
    const QJsonObject card = overlay->card();
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    QString documentId = m_htmlReader->libraryInfo().documentId;
    if (documentId.isEmpty()) documentId = str(primary, QStringLiteral("pdf_document_id")).isEmpty() ? str(primary, QStringLiteral("document_id")) : str(primary, QStringLiteral("pdf_document_id"));
    const QString pdfSha = m_htmlReader->libraryInfo().pdfSha256;
    QString segmentId, segmentSha;
    const QJsonArray segments = primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    if (segments.size() == 1) {
        segmentId = str(segments.first().toObject(), QStringLiteral("segment_id"));
        segmentSha = str(segments.first().toObject(), QStringLiteral("content_sha256"));
        if (segmentId.isEmpty() || segmentId.size() > 64 || segmentSha.size() != 64 || segmentSha != segmentSha.toLower()) segmentId.clear(), segmentSha.clear();
    }
    const QJsonObject anchor = Thoughts::ReviewCardFeedback::anchor(exact.left(4000), str(quote, QStringLiteral("prefix")), str(quote, QStringLiteral("suffix")), documentId,
                                                                    pdfSha, QJsonArray(), segmentId, segmentSha);
    const QString edition = m_htmlReader->shownEditionSha();
    auto make = [cardId, anchor, edition, overlay](const QString &chosenPolarity, const QString &comment) {
        QJsonObject record = Thoughts::FeedbackVersion::stamped(Thoughts::ReviewCardFeedback::create(cardId, chosenPolarity, comment, anchor, Thoughts::Platform::reviewOriginId()),
                                                                 overlay->reviewVersion());
        record.insert(QStringLiteral("view"), QStringLiteral("html"));
        record.insert(QStringLiteral("html_edition_sha256"), edition);
        return record;
    };
    auto save = [this, overlay](const QJsonObject &record) {
        QString error = Thoughts::ReviewCardFeedback::validationError(record);
        if (error.isEmpty() && overlay->write(record, &error)) {
            m_htmlView->call(QStringLiteral("clearSelection"));
            refreshHtmlFeedback();
            Q_EMIT reviewFeedbackChanged();
            return;
        }
        KMessageBox::error(widget(), error, i18n("Answer Feedback"));
    };
    if (!withComment) {
        save(make(polarity, QString()));
        return;
    }
    QJsonObject draft = make(polarity, QString());
    draft.insert(QStringLiteral("feedback_id"), QString());
    auto *editor = new Thoughts::ReviewFeedbackEditor(draft, true, widget());
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [make, save, editor](const QString &chosen, const QString &comment) {
        save(make(chosen, comment));
        editor->close();
    });
    editor->popupAt(QRect(QCursor::pos(), QSize(1, 1)));
}

void Part::htmlItemTapped(const QJsonObject &message)
{
    const QString kind = str(message, QStringLiteral("kind"));
    const QString id = str(message, QStringLiteral("id"));
    if (id.isEmpty()) return;
    if (kind == QLatin1String("card") || kind == QLatin1String("question")) {
        if (m_htmlReader->reviewCardId().isEmpty()) Q_EMIT requestOpenReviewCard(id);
    } else if (kind == QLatin1String("comment")) {
        Q_EMIT requestCommentsOverviewForComment(localFilePath(), id, 0);
    } else if (kind == QLatin1String("feedback") && m_pageView) {
        auto *overlay = m_pageView->reviewFeedback();
        const QJsonObject record = overlay->record(id);
        if (record.isEmpty()) return;
        auto *editor = new Thoughts::ReviewFeedbackEditor(record, false, widget());
        editor->setAttribute(Qt::WA_DeleteOnClose);
        connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [this, overlay, record, editor](const QString &polarity, const QString &comment) {
            QString error;
            if (!overlay->write(Thoughts::ReviewCardFeedback::edit(record, polarity, comment, Thoughts::Platform::reviewOriginId()), &error))
                KMessageBox::error(widget(), error, i18n("Answer Feedback"));
            refreshHtmlFeedback();
            editor->close();
        });
        connect(editor, &Thoughts::ReviewFeedbackEditor::deleteRequested, this, [this, overlay, record, editor] {
            QString error;
            if (!overlay->write(Thoughts::ReviewCardFeedback::remove(record, Thoughts::Platform::reviewOriginId()), &error))
                KMessageBox::error(widget(), error, i18n("Answer Feedback"));
            refreshHtmlFeedback();
            editor->close();
        });
        editor->popupAt(QRect(QCursor::pos(), QSize(1, 1)));
    }
}

void Part::openHtmlLink(const QJsonObject &message)
{
    const QString kind = str(message, QStringLiteral("kind"));
    const QString target = str(message, QStringLiteral("target")).isEmpty() ? str(message, QStringLiteral("href")) : str(message, QStringLiteral("target"));
    if (kind == QLatin1String("web") || kind == QLatin1String("mailto")) {
        const QUrl url(target);
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("mailto")) QDesktopServices::openUrl(url);
        return;
    }
    if (kind != QLatin1String("thoughts") && !target.startsWith(QLatin1String("thoughts-link:"))) return;
    const auto [linkKind, params] = parseThoughtsLink(target);
    if (linkKind == QLatin1String("html")) {
        QUrl link;
        link.setScheme(QStringLiteral("thoughts-html"));
        link.setPath(params.value(QStringLiteral("path")));
        // Inside Thoughts: a tab of its own in the app, else in place of the edition (Back returns).
        openHtmlAttachmentLink(link);
        return;
    }
    QString filename = params.value(QStringLiteral("filename"));
    if (filename.isEmpty() && !params.value(QStringLiteral("key")).isEmpty()) filename = params.value(QStringLiteral("key")) + QStringLiteral(".pdf");
    filename = safePdfFilename(QFileInfo(filename).fileName());
    QString destination = params.value(QStringLiteral("destination"));
    const QString anchor = params.value(QStringLiteral("anchor"));
    if (destination.isEmpty() && !anchor.isEmpty() && anchor != QLatin1String("document-start")) destination = anchor;
    const QString label = params.value(QStringLiteral("label"));
    // Same document: a label of the source scrolls the edition.
    if ((filename.isEmpty() || filename == QFileInfo(localFilePath()).fileName()) && !label.isEmpty() && label != QLatin1String("document-start")) {
        m_htmlView->runScript(QStringLiteral("(function(){var e=document.getElementById(%1);if(e){e.scrollIntoView({block:'start'});}})()")
                                  .arg(QString::fromUtf8(QJsonDocument(QJsonArray{label.startsWith(QLatin1String("source-label:")) ? label : QStringLiteral("source-label:") + label}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)));
        return;
    }
    if (filename.isEmpty()) {
        displayInfoMessage(i18n("This link has no document."), KMessageWidget::Warning, 4000);
        return;
    }
    const QString path = QDir(m_docsRenderRoot).filePath(QStringLiteral("current_pdfs/") + filename);
    if (!QFileInfo::exists(path)) {
        displayInfoMessage(i18n("%1 is not on this computer. Sync to get it.", filename), KMessageWidget::Warning, 5000);
        return;
    }
    if (QFileInfo(path) == QFileInfo(localFilePath())) {
        if (!destination.isEmpty()) {
            setDocumentView(QStringLiteral("pdf"));
            navigateToDestination(destination, QString(), true);
        }
        return;
    }
    Q_EMIT requestOpenDocumentLink(QUrl::fromLocalFile(path), destination, QString());
}
}
