// SPDX-License-Identifier: GPL-2.0-or-later
// The Part's HTML attachments (thoughts-html-attachment-cards-SPEC.md, "Desktop"): a thoughts-html:
// link of the PDF (or of its HTML edition) shows the page inside the reader. In the app the page
// opens in a tab of its own next to the document (a second Part of the same PDF that shows the
// page alone, with Close and Open in Browser; the document stays where it was), as do a page's
// card or feedback opened from elsewhere; a Part alone shows it in place of the document, with
// Back. A selection in the page can become a review card (when the server has
// html_attachment_cards_v1); the owner's cards of the page are highlighted; reviews of such cards
// open the page in the document's own tab, restore its #hash, mask the text, or say it is not found.
#include "part.h"

#include "core/document.h"
#include "core/htmlattachment.h"
#include "core/htmllivepage.h"
#include "documentfeedback.h"
#include "documentfeedbackoverlay.h"
#include "documentidentity.h"
#include "fedorarepository.h"
#include "feedbackversion.h"
#include "htmlattachmentreader.h"
#include "htmldocumentview.h"
#include "htmledition.h"
#include "htmllivewebview.h"
#include "htmlreader.h"
#include "pageview.h"
#include "reviewcardfeedback.h"
#include "reviewfeedbackeditor.h"
#include "settings.h"
#include "side_reviews.h"
#include "snapshotdrawingdialog.h"
#include "thoughtsplatform.h"

#include <KDirWatch>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMenu>
#include <QMetaMethod>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QStackedWidget>
#include <QTextDocumentFragment>
#include <QTimer>

#include <algorithm>

namespace
{
QString str(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

const QString AttachmentCardsCapability = QStringLiteral("html_attachment_cards_v1");

bool s_withoutWebViewForTesting = false;
int s_livePageAnswerForTesting = -1;
QImage s_livePagePictureForTesting;

const QString LiveConsent = QStringLiteral("consent");
const QString LiveLoading = QStringLiteral("loading");
const QString LiveLoaded = QStringLiteral("loaded");
const QString LiveUnreachable = QStringLiteral("unreachable");
const QString LiveUnavailable = QStringLiteral("unavailable");

// The page at @p path: the version the PDF's source bundle carries, or what this computer wrote
// over it since (a snapshot, a stub), until the bundle has that.
Thoughts::HtmlAttachmentTarget resolvedPage(const QString &path, const QString &pdfPath, const QString &docsRoot, QString *error)
{
    QString builtError;
    const auto built = Thoughts::resolveHtmlAttachment(path, pdfPath, &builtError);
    const auto written = Thoughts::isHtmlSourcePath(path) ? Thoughts::LivePage::writtenPage(path, built, docsRoot) : Thoughts::HtmlAttachmentTarget();
    if (written.isValid()) {
        if (error) error->clear();
        return written;
    }
    if (error) *error = builtError;
    return built;
}

QByteArray readPage(const QString &file, qint64 limit)
{
    QFile page(file);
    return page.open(QIODevice::ReadOnly) ? page.read(limit) : QByteArray();
}

// What a page's tab is called: the page's <title>, else its file name without .html.
QString attachmentTitle(const Thoughts::HtmlAttachmentTarget &target)
{
    QFile file(target.file);
    if (file.open(QIODevice::ReadOnly)) {
        static const QRegularExpression title(QStringLiteral("<title[^>]*>([^<]*)</title>"), QRegularExpression::CaseInsensitiveOption);
        const auto match = title.match(QString::fromUtf8(file.read(256 * 1024)));
        const QString text = match.hasMatch() ? QTextDocumentFragment::fromHtml(match.captured(1)).toPlainText().simplified() : QString();
        if (!text.isEmpty()) return text;
    }
    return QFileInfo(target.path).completeBaseName();
}
}

namespace Okular
{
void Part::setupHtmlAttachmentView()
{
    m_attachmentView = new Thoughts::HtmlDocumentView(m_documentStack);
    m_attachmentView->setObjectName(QStringLiteral("thoughtsHtmlAttachmentView"));
    m_attachmentView->setAttachmentBar(i18n("Back"), i18n("Back to the document"));
    m_attachmentView->setSelectionAction(i18n("New Review Card…"), true, false);
    m_documentStack->addWidget(m_attachmentView);
    m_attachmentReader = new Thoughts::HtmlAttachmentReader(m_attachmentView, this);

    // Back, or Close in a page's tab.
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::showPdfRequested, this, [this] {
        if (m_attachmentTab) Q_EMIT requestCloseHtmlAttachmentTab();
        else hideHtmlAttachment();
    });
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::openInBrowserRequested, this, &Part::openAttachmentInBrowser);
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::selectionActionClicked, this, &Part::requestAttachmentCard);
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::externalLinkRequested, this, &Part::openAttachmentLink);
    // A live page: its bar (Reload, Take Snapshot), its load, and the questions in its place.
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::reloadRequested, this, &Part::reloadLivePage);
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::takeSnapshotRequested, this, &Part::takeLiveSnapshot);
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::liveLoaded, this, [this](const QUrl &) {
        if (!m_attachmentReader->isLive()) return;
        m_liveState = LiveLoaded;
        // Each load (opening, Reload, a navigation of the page) is a version of the live page: the
        // feedback given on the previous one is now from an earlier version (html-live-pages.md §2.1).
        m_liveLoadedAt = Thoughts::ReviewCardFeedback::timestamp(QDateTime::currentDateTimeUtc());
        m_attachmentVersions.clear();
        refreshDocumentFeedback();
        updateLivePageBar();
    });
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::liveLoadFailed, this, [this](const QString &error) {
        if (!m_attachmentReader->isLive()) return;
        qWarning().noquote() << "Live page" << m_attachmentReader->liveAddress().toDisplayString() << "could not be loaded:" << error;
        m_liveState = LiveUnreachable;
        showLiveMessage();
        updateLivePageBar();
    });
    // Its selection's menu is the reader's (html-live-pages.md 2.1).
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::liveSelectionMenuRequested, this, &Part::requestLiveSelectionMenu);
    // The live page's own menu of selected text (one menu, 23 Nov 2026): its items, then the selection is read.
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::liveSelectionChoice, this, &Part::chooseLiveSelection);
    m_attachmentView->setLiveSelectionFeedbackProvider([this] { return liveSelectionFeedbackOffered(); });
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::messageActionClicked, this, &Part::liveMessageAction);
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::messageCancelClicked, this, &Part::liveMessageCancel);
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::selectionChanged, this, &Part::updateAttachmentSelectionAction);
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::reviewMaskChanged, this, &Part::updateAttachmentSelectionAction);
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::itemTapped, this, [this](const QJsonObject &message) {
        const QString id = str(message, QStringLiteral("id"));
        if (!m_attachmentReader->reviewCardId().isEmpty() || id.isEmpty()) return;
        if (str(message, QStringLiteral("kind")) == QLatin1String("feedback")) m_pageView->openDocumentFeedbackEditor(id, QRect(attachmentClick(message), QSize(1, 1)));
        else Q_EMIT requestOpenReviewCard(id);
    });
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::contextMenuRequested, this, &Part::showAttachmentContextMenu);
    // The page view's Undo banner is behind this page: its notice bar offers Undo instead.
    m_attachmentFeedbackUndoTimer = new QTimer(this);
    m_attachmentFeedbackUndoTimer->setSingleShot(true);
    m_attachmentFeedbackUndoTimer->setInterval(5000);
    const auto endUndo = [this] {
        m_attachmentFeedbackUndo = {};
        m_attachmentFeedbackUndoTimer->stop();
        m_attachmentView->setNotice(QString(), QString());
    };
    connect(m_attachmentFeedbackUndoTimer, &QTimer::timeout, this, endUndo);
    connect(m_pageView.data(), &PageView::feedbackUndoOffered, this, [this](const std::function<bool(QString *error)> &undo, bool *taken) {
        if (!m_attachmentShown) return;
        *taken = true;
        m_attachmentFeedbackUndo = undo;
        m_attachmentView->setNotice(i18n("Feedback deleted"), i18n("Undo"));
        m_attachmentFeedbackUndoTimer->start();
    });
    connect(m_attachmentView, &Thoughts::HtmlDocumentView::noticeClicked, this, [this, endUndo] {
        const auto undo = m_attachmentFeedbackUndo;
        if (!undo) return;
        endUndo();
        QString error;
        if (!undo(&error)) displayInfoMessage(i18n("The feedback could not be restored: %1", error), KMessageWidget::Warning, 5000);
    });
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::answerNotFound, this, [this](const QString &cardId) {
        if (str(m_reviewCardRecord, QStringLiteral("card_id")) != cardId || !m_attachmentReviewShown) return;
        // Nothing is lost: the review panel shows the stored answer text instead of a mask.
        m_attachmentView->setNotice(i18n("Answer not found in the current page."), QString());
        Q_EMIT reviewAnswerNotFound(cardId);
    });
    // The review's Skip (S) and Escape reach the app unless the page uses them itself (the viewer
    // asks after the page's own key listeners). Read Aloud stays the edition's: its bar lives there.
    m_attachmentReader->setForwardedKeysProvider([this] { return attachmentForwardedKeys(); });
    connect(m_attachmentReader, &Thoughts::HtmlAttachmentReader::keyForwarded, this, [this](const QJsonObject &message) {
        if (m_attachmentShown) handleHtmlKey(message);
    });
    // Links of the PDF (core) open the page here instead of the browser.
    Thoughts::setHtmlAttachmentHandler(m_document, this, [this](const QUrl &link) { openHtmlAttachmentLink(link); });
    // The pencil on the page draws on a snapshot of it (document-feedback.md 10.1).
    m_pageView->setHtmlPagePencil([this] { drawOnAttachmentPage(); });
    // A newer version of the shown page (Sync): shown in its place, once the writes settled.
    m_attachmentWatcher = new KDirWatch(this);
    m_attachmentFollowTimer = new QTimer(this);
    m_attachmentFollowTimer->setSingleShot(true);
    m_attachmentFollowTimer->setInterval(750);
    connect(m_attachmentFollowTimer, &QTimer::timeout, this, &Part::followHtmlAttachmentSource);
    for (const auto signal : {&KDirWatch::dirty, &KDirWatch::created, &KDirWatch::deleted})
        connect(m_attachmentWatcher, signal, m_attachmentFollowTimer, qOverload<>(&QTimer::start));
}

void Part::watchHtmlAttachmentSource()
{
    if (!m_attachmentWatcher) return;
    // The pointers of this PDF's source bundles, which say which version of the page it links.
    QStringList files;
    if (m_attachmentReader->isOpen() && url().isLocalFile()) {
        const QString pointer = QStringLiteral("documents/") + QFileInfo(localFilePath()).fileName() + QStringLiteral(".json");
        for (const QString &root : Thoughts::htmlAttachmentBundleRoots(localFilePath())) files.append(QDir::cleanPath(QDir(root).filePath(pointer)));
    }
    if (files == m_attachmentWatchedFiles) return;
    for (const QString &file : std::as_const(m_attachmentWatchedFiles)) m_attachmentWatcher->removeFile(file);
    m_attachmentWatchedFiles = files;
    for (const QString &file : std::as_const(m_attachmentWatchedFiles)) m_attachmentWatcher->addFile(file);
    if (files.isEmpty()) {
        m_attachmentFollowTimer->stop();
        m_attachmentFollowPending = false;
    }
}

void Part::followHtmlAttachmentSource()
{
    if (!m_attachmentReader || !m_attachmentShown || !m_attachmentReader->isOpen()) return;
    // A card presented on the page keeps it until the review moves on (clearReviewMask).
    if (m_attachmentReviewShown || !m_attachmentReader->reviewCardId().isEmpty()) {
        m_attachmentFollowPending = true;
        return;
    }
    m_attachmentFollowPending = false;
    const auto shown = m_attachmentReader->target();
    QString error;
    const auto newest = resolveHtmlPage(shown.path, &error);
    // No longer in the bundle (it stays as it is), or the version shown (also from another copy).
    if (!newest.isValid() || (newest.sha256.isEmpty() ? newest.file == shown.file : newest.sha256 == shown.sha256)) return;
    const int generation = ++m_attachmentFollowGeneration;
    QPointer<Part> self(this);
    const auto replace = [self, generation, newest](const QJsonObject &position) {
        if (!self || generation != self->m_attachmentFollowGeneration || !self->m_attachmentShown || !self->m_attachmentReader->isOpen()) return;
        self->m_attachmentVersions.remove(newest.path);
        QString error;
        if (!self->showHtmlAttachment(newest, self->m_attachmentForReview, &error)) {
            qWarning().noquote() << "HTML page" << newest.path << "could not show its newest version:" << error;
            return;
        }
        if (!self->m_attachmentReader->isLive()) self->m_attachmentReader->restorePosition(position);
        const QString notice = i18n("This page was updated to its newest version.");
        self->m_attachmentView->setNotice(notice, QString());
        QTimer::singleShot(5000, self, [self, notice] {
            if (self && self->m_attachmentView->notice() == notice) self->m_attachmentView->setNotice(QString(), QString());
        });
    };
    // Where the reader is in the page now (a live page, or one not loaded yet, starts at its top).
    if (m_attachmentReader->isLive() || !m_attachmentReader->isReady()) {
        replace({});
        return;
    }
    m_attachmentView->call(QStringLiteral("getPosition"), {}, [replace](bool ok, const QJsonValue &result, const QString &) {
        replace(ok ? result.toObject() : QJsonObject());
    });
}

namespace
{
Thoughts::HtmlAttachmentReader::Owner attachmentOwner(const QString &pdfPath, const QString &docsRoot)
{
    Thoughts::HtmlAttachmentReader::Owner owner;
    owner.pdfPath = pdfPath;
    owner.pdfStem = QFileInfo(pdfPath).completeBaseName();
    // The Library's PDF (the generation the link is in), not the bytes of a locally annotated copy.
    const auto info = Thoughts::HtmlEdition::libraryInfo(pdfPath);
    owner.pdfSha256 = info.pdfSha256.toLower();
    owner.documentId = info.documentId;
    if (owner.documentId.isEmpty()) owner.documentId = Thoughts::DocumentIdentityStore(docsRoot).forFile(pdfPath).value(QStringLiteral("document_id")).toString();
    if (!Thoughts::HtmlEdition::isSha256(owner.pdfSha256)) {
        QFile file(pdfPath);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (file.open(QIODevice::ReadOnly) && hash.addData(&file)) owner.pdfSha256 = QString::fromLatin1(hash.result().toHex());
    }
    return owner;
}
}

void Part::setHtmlAttachmentsWithoutWebViewForTesting(bool inside)
{
    s_withoutWebViewForTesting = inside;
}

void Part::setLivePageAnswerForTesting(int answer)
{
    s_livePageAnswerForTesting = answer;
}

void Part::setLivePagePictureForTesting(const QImage &picture)
{
    s_livePagePictureForTesting = picture;
}

Thoughts::HtmlAttachmentTarget Part::resolveHtmlPage(const QString &path, QString *error) const
{
    return resolvedPage(path, localFilePath(), m_docsRenderRoot, error);
}

bool Part::wantsHtmlAttachmentTab(const QString &path) const
{
    // The app (the shell takes the request): a tab of its own. A page's tab shows its own page;
    // another page of the document gets a tab of its own too.
    return m_attachmentReader && (!m_attachmentTab || path != m_attachmentTabPath)
        && isSignalConnected(QMetaMethod::fromSignal(&Part::requestHtmlAttachmentTab));
}

bool Part::openHtmlAttachmentLink(const QUrl &link)
{
    const QString path = Thoughts::htmlAttachmentPath(link);
    if (path.isEmpty()) {
        displayInfoMessage(i18n("This HTML attachment link is invalid."), KMessageWidget::Warning, 5000);
        return false;
    }
    QString error;
    const bool webView = Thoughts::HtmlDocumentView::webViewAvailable() || s_withoutWebViewForTesting;
    // A live page needs only a web view of its own (HtmlLiveWebView): Windows has that without Qt WebView.
    const auto live = m_attachmentReader && !webView && Thoughts::HtmlLiveWebView::available() ? resolveHtmlPage(path, &error) : Thoughts::HtmlAttachmentTarget();
    const bool liveOnly = live.isValid() && Thoughts::LivePage::stubAddressOfFile(live.file).isValid();
    if ((!webView && !liveOnly) || !m_attachmentReader) {
        // No web view in this build: the browser, as before.
        if (!Thoughts::openHtmlAttachment(link, QUrl::fromLocalFile(localFilePath()), &error)) {
            displayInfoMessage(error, KMessageWidget::Warning, 6000);
            return false;
        }
        return true;
    }
    const auto target = liveOnly ? live : resolveHtmlPage(path, &error);
    if (!target.isValid()) {
        displayInfoMessage(error, KMessageWidget::Warning, 6000);
        return false;
    }
    // In the app the page gets a tab of its own; the document stays where it was in this one.
    if (wantsHtmlAttachmentTab(target.path)) {
        Q_EMIT requestHtmlAttachmentTab({{QStringLiteral("path"), target.path}});
        return true;
    }
    if (!showHtmlAttachment(target, false, &error)) {
        displayInfoMessage(error, KMessageWidget::Warning, 6000);
        return false;
    }
    return true;
}

bool Part::openHtmlAttachmentTab(const QUrl &pdf, const QJsonObject &request)
{
    if (!m_attachmentReader || m_document->isOpened() || str(request, QStringLiteral("path")).isEmpty()) return false;
    // Before the PDF opens: a page's tab never loads the document's HTML edition.
    m_attachmentTab = true;
    m_attachmentTabPath = str(request, QStringLiteral("path"));
    m_attachmentView->setAttachmentBar(i18n("Close"), i18n("Close this page and go back to the document"));
    if (!openUrl(pdf) || !showInHtmlAttachmentTab(request)) {
        leaveHtmlAttachmentTab();
        return false;
    }
    return true;
}

bool Part::showInHtmlAttachmentTab(const QJsonObject &request)
{
    if (!m_attachmentTab || !m_document->isOpened()) return false;
    const QJsonObject card = request.value(QStringLiteral("card")).toObject();
    if (!card.isEmpty()) return showHtmlAttachmentCard(card);
    const QString feedbackId = str(request, QStringLiteral("feedback_id"));
    if (!feedbackId.isEmpty()) {
        // A tab just opened reads its document's feedback a moment later (openFile).
        if (m_pageView->documentFeedback()->record(feedbackId).isEmpty()) refreshDocumentFeedback();
        if (!m_pageView->documentFeedback()->record(feedbackId).isEmpty()) {
            showFeedback(feedbackId, request.value(QStringLiteral("edit")).toBool());
            return m_attachmentShown;
        }
    }
    QString error;
    const auto target = resolveHtmlPage(str(request, QStringLiteral("path")), &error);
    if (!target.isValid() || !showHtmlAttachment(target, false, &error)) {
        displayInfoMessage(error, KMessageWidget::Warning, 6000);
        return false;
    }
    return true;
}

void Part::leaveHtmlAttachmentTab()
{
    if (!m_attachmentTab) return;
    m_attachmentTab = false;
    m_attachmentTabPath.clear();
    m_attachmentView->setAttachmentBar(i18n("Back"), i18n("Back to the document"));
    closeHtmlAttachment();
    if (m_htmlReader && m_document->isOpened() && url().isLocalFile()) m_htmlReader->setDocument(localFilePath());
}

void Part::openHtmlAttachmentInBrowser()
{
    openAttachmentInBrowser();
}

bool Part::showHtmlAttachment(const Thoughts::HtmlAttachmentTarget &target, bool forReview, QString *error)
{
    if (!m_attachmentReader || !m_documentStack) return false;
    const auto owner = attachmentOwner(localFilePath(), m_docsRenderRoot);
    // A live page's stub: its address instead, once the user agreed to its origin on this computer.
    const QUrl live = target.isValid() ? Thoughts::LivePage::stubAddressOfFile(target.file) : QUrl();
    if (live.isValid()) {
        const bool canShow = Thoughts::HtmlLiveWebView::available() || s_withoutWebViewForTesting;
        const bool allowed = Thoughts::LivePage::originAllowed(Thoughts::LivePage::origin(live));
        const bool same = m_attachmentReader->isLive() && m_attachmentReader->target().path == target.path && m_attachmentReader->liveAddress() == live;
        // Before the view loads: a load that fails at once reports it.
        if (!same || (m_liveState == LiveConsent && allowed)) m_liveState = !canShow ? LiveUnavailable : allowed ? LiveLoading : LiveConsent;
        if (!m_attachmentReader->openLive(target, owner, live, canShow && allowed)) {
            if (error) *error = i18n("This live page could not be opened.");
            return false;
        }
        if (m_liveState == LiveConsent || m_liveState == LiveUnavailable) showLiveMessage();
    } else {
        if (!m_attachmentReader->open(target, owner, error)) return false;
        m_liveState.clear();
    }
    m_attachmentTitle = attachmentTitle(target);
    m_attachmentReader->setCards(m_sidebarCardRecords);
    m_attachmentReader->setFeedback(m_pageView->documentFeedback()->records());
    if (!m_attachmentShown) {
        m_attachmentShown = true;
        m_attachmentForReview = forReview;
        m_documentStack->setCurrentWidget(m_attachmentView);
        // The review panel floats over a native web view (see htmlViewActiveChanged); a live page's
        // is one too.
        widget()->setProperty("thoughtsNativeDocumentView", Thoughts::HtmlDocumentView::webViewAvailable() || (live.isValid() && Thoughts::HtmlLiveWebView::available()));
        if (QWidget *container = widget()->findChild<QWidget *>(QStringLiteral("thoughtsPageSplitter"))) container->parentWidget()->setFocusProxy(m_attachmentView);
    } else if (!forReview) {
        // Opened by the user: it stays when the review ends.
        m_attachmentForReview = false;
    }
    if (!m_attachmentReviewShown) m_attachmentView->setNotice(QString(), QString());
    updateToolsForShownView();
    // New cards only where the server keeps them (asked once per run, last answer offline).
    if (m_fedoraRepository) {
        m_attachmentCardsSupported = m_fedoraRepository->knownServerCapabilities().contains(AttachmentCardsCapability);
        QPointer<Part> self(this);
        m_fedoraRepository->queryServerCapabilities(this, [self](const QStringList &capabilities) {
            if (!self) return;
            self->m_attachmentCardsSupported = capabilities.contains(AttachmentCardsCapability);
            self->updateAttachmentSelectionAction();
        });
    }
    updateAttachmentSelectionAction();
    updateLivePageBar();
    watchHtmlAttachmentSource();
    return true;
}

void Part::showLiveMessage()
{
    if (!m_attachmentReader->isLive()) return;
    const QUrl address = m_attachmentReader->liveAddress();
    if (m_liveState == LiveConsent) {
        // Ask once per origin (html-live-pages.md §2); Open is remembered for it on this computer.
        m_attachmentView->showMessage(i18n("This page loads %1 from the network. Open it?", QUrl(Thoughts::LivePage::origin(address)).toDisplayString()), i18n("Open"),
                                      i18n("Cancel"));
    } else if (m_liveState == LiveUnreachable) {
        m_attachmentView->showMessage(i18n("This live page is at %1, which this device cannot reach.", address.toDisplayString()), i18n("Retry"));
    } else if (m_liveState == LiveUnavailable) {
        m_attachmentView->showMessage(i18n("This live page is at %1. Thoughts cannot show live pages on this computer.", address.toDisplayString()), i18n("Open in Browser"));
    }
}

void Part::liveMessageAction()
{
    if (!m_attachmentReader || !m_attachmentReader->isLive()) return;
    if (m_liveState == LiveConsent) {
        Thoughts::LivePage::allowOrigin(Thoughts::LivePage::origin(m_attachmentReader->liveAddress()));
        m_liveState = LiveLoading;
        updateLivePageBar();
        m_attachmentReader->loadLiveNow();
    } else if (m_liveState == LiveUnreachable) {
        reloadLivePage();
    } else if (m_liveState == LiveUnavailable) {
        openAttachmentInBrowser();
    }
}

void Part::liveMessageCancel()
{
    // Not opened: the page's tab closes, a page in place of the document goes back to it.
    if (!m_attachmentReader || !m_attachmentReader->isLive() || m_liveState != LiveConsent) return;
    if (m_attachmentTab) Q_EMIT requestCloseHtmlAttachmentTab();
    else hideHtmlAttachment();
}

void Part::reloadLivePage()
{
    if (!m_attachmentReader || !m_attachmentReader->isLive() || m_liveState == LiveConsent) return;
    if (m_liveState == LiveUnavailable) {
        showLiveMessage();
        return;
    }
    m_liveState = LiveLoading;
    updateLivePageBar();
    m_attachmentView->reloadLive();
}

void Part::updateLivePageBar()
{
    if (!m_attachmentView) return;
    const bool live = m_attachmentReader && m_attachmentReader->isLive();
    m_attachmentView->setLiveBar(live ? i18nc("@info the bar of a live HTML page", "Live \u00b7 %1", m_attachmentReader->liveAddress().toDisplayString()) : QString());
    if (!live) return;
    // Saved as the page's file in its folder, which Sync publishes: only where that folder is.
    const QString folder = Thoughts::htmlSourceFolder(m_attachmentReader->target().path);
    const bool here = !folder.isEmpty() && QFileInfo(QDir(m_docsRenderRoot).filePath(folder)).isDir();
    const bool loaded = m_liveState == LiveLoaded;
    const QString tip = m_liveSnapshotBusy ? i18n("Taking a snapshot\u2026")
        : !loaded                          ? i18n("Works once the page is shown.")
        : !here                            ? i18n("This page's folder is not on this computer.")
                                           : i18n("Replace the live page with a snapshot of what it shows now (an embedded page)");
    m_attachmentView->setTakeSnapshotAction(loaded && here && !m_liveSnapshotBusy, tip);
}

void Part::hideHtmlAttachment()
{
    // A page's tab shows its page alone (Close closes the tab).
    if (!m_attachmentShown || !m_documentStack || m_attachmentTab) return;
    m_attachmentShown = false;
    m_attachmentForReview = false;
    const bool html = m_htmlReader && m_htmlReader->isActive();
    m_documentStack->setCurrentWidget(html ? static_cast<QWidget *>(m_htmlView) : static_cast<QWidget *>(m_pageView.data()));
    widget()->setProperty("thoughtsNativeDocumentView", html && Thoughts::HtmlDocumentView::webViewAvailable());
    if (QWidget *container = widget()->findChild<QWidget *>(QStringLiteral("thoughtsPageSplitter")))
        container->parentWidget()->setFocusProxy(html ? static_cast<QWidget *>(m_htmlView) : static_cast<QWidget *>(m_pageView.data()));
    updateToolsForShownView();
}

void Part::updateToolsForShownView()
{
    if (!m_pageView) return;
    m_pageView->setShownInstead(m_attachmentShown ? PageView::ShownInstead::HtmlPage
                                    : htmlViewActive() ? PageView::ShownInstead::HtmlEdition
                                                       : PageView::ShownInstead::Nothing);
    // The newer-version bar is about the PDF: it shows over the page view only.
    updateNewerVersionBar();
}

void Part::closeHtmlAttachment()
{
    if (!m_attachmentReader) return;
    hideHtmlAttachment();
    m_attachmentReviewShown = false;
    m_attachmentReader->close();
    m_liveState.clear();
    updateLivePageBar();
    watchHtmlAttachmentSource();
}

void Part::updateAttachmentSelectionAction()
{
    if (!m_attachmentView || !m_attachmentReader) return;
    // A live page has none of the page bridge: cards need an embedded page (its feedback is the
    // reader's own menu, requestLiveSelectionMenu).
    if (m_attachmentReader->isLive()) {
        m_attachmentView->setSelectionAction(i18n("New Review Card…"), true, false, i18n("Works on embedded pages"));
        return;
    }
    const QJsonObject quote = m_attachmentReader->lastSelection().value(QStringLiteral("text_quote")).toObject();
    const bool selected = !str(quote, QStringLiteral("exact")).trimmed().isEmpty();
    const bool reviewing = !m_attachmentReader->reviewCardId().isEmpty();
    const QString tip = !m_attachmentCardsSupported ? i18n("The server does not keep review cards made in HTML pages yet.")
        : reviewing                                 ? i18n("Not during a review.")
        : !selected                                 ? i18n("Select the answer text in the page first.")
                                                    : i18n("Make a review card whose answer is the selected text.");
    m_attachmentView->setSelectionAction(i18n("New Review Card…"), true, m_attachmentCardsSupported && selected && !reviewing, tip);
}

void Part::requestAttachmentCard()
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || m_attachmentReader->isLive() || !m_attachmentCardsSupported || !m_attachmentReader->reviewCardId().isEmpty()) return;
    // The page's current selection (the last message is only a hint and may be stale).
    QPointer<Part> self(this);
    m_attachmentView->call(QStringLiteral("getSelection"), {}, [self](bool ok, const QJsonValue &result, const QString &) {
        if (!self || !self->m_attachmentReader->isOpen()) return;
        const QJsonObject selection = ok && result.isObject() ? result.toObject() : self->m_attachmentReader->lastSelection();
        const QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(selection, self->m_attachmentReader->target());
        if (fields.isEmpty()) {
            self->displayInfoMessage(i18n("Select the answer text in the page first."), KMessageWidget::Information, 3000);
            return;
        }
        const auto owner = self->m_attachmentReader->owner();
        QVariantMap map = fields.toVariantMap();
        map.insert(QStringLiteral("pdf_path"), owner.pdfPath);
        map.insert(QStringLiteral("pdf_stem"), owner.pdfStem);
        map.insert(QStringLiteral("pdf_sha256"), owner.pdfSha256);
        map.insert(QStringLiteral("document_id"), owner.documentId);
        map.insert(QStringLiteral("selected_text"), fields.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString());
        self->m_attachmentView->call(QStringLiteral("clearSelection"));
        Q_EMIT self->requestHtmlReviewCard(owner.pdfPath, map);
    });
}

void Part::openAttachmentLink(const QUrl &url, bool userGesture)
{
    // An attachment is a page an agent wrote. A link the reader clicked opens in the browser;
    // an address the page went to by itself does not, or the page could send what it shows to
    // any site just by being opened.
    if (userGesture) {
        // This computer's server's page of a Library document opens the document here.
        if (!openServerPageLink(url)) QDesktopServices::openUrl(url);
        return;
    }
#ifdef Q_OS_MACOS
    const bool clicksSeen = true;
#else
    // A live page's own web view sees them too (WebView2 on Windows).
    const bool clicksSeen = m_attachmentReader && m_attachmentReader->isLive() && Thoughts::HtmlLiveWebView::available();
#endif
    if (clicksSeen) {
        displayInfoMessage(i18n("The page tried to open %1 by itself. It was not opened.", url.toDisplayString(QUrl::RemoveQuery | QUrl::RemoveFragment)), KMessageWidget::Warning, 5000);
        return;
    }
#ifndef Q_OS_MACOS
    // Thoughts cannot see the page's clicks here: the reader decides, one question at a time.
    if (m_attachmentLinkQuestion) {
        return;
    }
    m_attachmentLinkQuestion = true;
    const auto answer = KMessageBox::questionTwoActions(widget(), i18n("The page wants to open this address in your browser:\n%1", url.toDisplayString()), i18n("Open Link"),
                                                        KGuiItem(i18n("Open")), KStandardGuiItem::cancel());
    m_attachmentLinkQuestion = false;
    if (answer == KMessageBox::PrimaryAction) {
        QDesktopServices::openUrl(url);
    }
#endif
}

void Part::openAttachmentInBrowser()
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen()) return;
    // A live page is at its address.
    if (m_attachmentReader->isLive()) {
        if (!QDesktopServices::openUrl(m_attachmentReader->liveAddress())) displayInfoMessage(i18n("Could not open the default browser."), KMessageWidget::Warning, 5000);
        return;
    }
    QUrl url = Thoughts::serveHtmlAttachmentFile(m_attachmentReader->target().file);
    // The tab shown here (the page's #hash), when it has one.
    const QString hash = str(m_attachmentReader->pageState(), QStringLiteral("hash"));
    if (url.isValid() && hash.startsWith(QLatin1Char('#')) && hash.size() > 1) url.setFragment(hash.mid(1));
    if (!url.isValid() || !QDesktopServices::openUrl(url)) displayInfoMessage(i18n("Could not open the default browser."), KMessageWidget::Warning, 5000);
}

namespace
{
// The attachment of a card or feedback: the current version at its path, else the version it was
// made on. Also that version when the page is live now: cards and feedback need an embedded page.
Thoughts::HtmlAttachmentTarget namedAttachment(const QJsonObject &ref, const QString &pdfPath, const QString &docsRoot, QString *error)
{
    const QString path = str(ref, QStringLiteral("path"));
    auto target = resolvedPage(path, pdfPath, docsRoot, error);
    const QString sha = str(ref, QStringLiteral("sha256"));
    const bool liveNow = target.isValid() && target.sha256 != sha && Thoughts::LivePage::stubAddressOfFile(target.file).isValid();
    if ((!target.isValid() || liveNow) && Thoughts::isHtmlSourcePath(path)) {
        const QString file = Thoughts::htmlAttachmentObject(sha, pdfPath);
        if (!file.isEmpty()) target = {path, file, sha};
    }
    return target;
}

Thoughts::HtmlAttachmentTarget cardAttachment(const QJsonObject &card, const QString &pdfPath, const QString &docsRoot, QString *error)
{
    return namedAttachment(card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("html_attachment")).toObject(), pdfPath, docsRoot, error);
}
}

void Part::addAttachmentMenuActions(QMenu *menu, const QJsonObject &message)
{
    const QJsonObject quote = message.value(QStringLiteral("selection")).toObject().value(QStringLiteral("text_quote")).toObject();
    const bool reviewing = !m_attachmentReader->reviewCardId().isEmpty();
    if (!str(quote, QStringLiteral("exact")).trimmed().isEmpty()) {
        if (!reviewing && m_pageView->documentFeedback()->hasDocument()) {
            const QString liked = Thoughts::ReviewCardFeedback::positive();
            const QString disliked = Thoughts::ReviewCardFeedback::negative();
            menu->addAction(i18n("Like"), this, [this, liked] { giveAttachmentFeedback(liked, false); })->setObjectName(QStringLiteral("attachmentFeedbackLike"));
            menu->addAction(i18n("Dislike"), this, [this, disliked] { giveAttachmentFeedback(disliked, false); })->setObjectName(QStringLiteral("attachmentFeedbackDislike"));
            menu->addAction(i18n("Question…"), this, [this, liked] { giveAttachmentFeedback(liked, true, Thoughts::DocumentFeedback::question()); })->setObjectName(QStringLiteral("attachmentFeedbackQuestion"));
            menu->addAction(i18n("Comment…"), this, [this, liked] { giveAttachmentFeedback(liked, true, Thoughts::DocumentFeedback::other()); })->setObjectName(QStringLiteral("attachmentFeedbackComment"));
            menu->addSeparator();
        }
        if (!reviewing && m_attachmentCardsSupported) {
            menu->addAction(i18n("New Review Card…"), this, &Part::requestAttachmentCard)->setObjectName(QStringLiteral("attachmentNewCard"));
        }
        menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy"), this, [this] {
            m_attachmentView->call(QStringLiteral("copySelection"), {}, [](bool ok, const QJsonValue &result, const QString &) {
                if (ok && !result.toString().isEmpty()) QGuiApplication::clipboard()->setText(result.toString());
            });
        })->setObjectName(QStringLiteral("attachmentCopy"));
    }
    if (reviewing) return;
    QSet<QString> seen;
    const QJsonArray items = message.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        const QString id = str(value.toObject(), QStringLiteral("id"));
        if (str(value.toObject(), QStringLiteral("kind")) != QLatin1String("feedback") || seen.contains(id)) continue;
        seen.insert(id);
        if (seen.size() == 1 && !menu->isEmpty()) menu->addSeparator();
        const QPoint at = attachmentClick(message);
        menu->addAction(i18n("Edit Feedback…"), this, [this, id, at] { m_pageView->openDocumentFeedbackEditor(id, QRect(at, QSize(1, 1))); })
            ->setObjectName(QStringLiteral("attachmentEditFeedback"));
    }
}

void Part::showAttachmentContextMenu(const QJsonObject &message)
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen()) return;
    QMenu menu(widget());
    addAttachmentMenuActions(&menu, message);
    if (!menu.isEmpty()) menu.exec(attachmentClick(message));
}

QPoint Part::attachmentClick(const QJsonObject &message) const
{
    // At the click (viewport CSS px = Qt logical px of the web view), else at the cursor.
    const QJsonArray point = message.value(QStringLiteral("point")).toArray();
    if (point.size() != 2 || !m_attachmentView->webContainer()) return QCursor::pos();
    return m_attachmentView->webContainer()->mapToGlobal(QPoint(point.at(0).toInt(), point.at(1).toInt()));
}

void Part::giveAttachmentFeedback(const QString &polarity, bool withComment, const QString &kind)
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || !m_attachmentReader->reviewCardId().isEmpty()) return;
    // The page's current selection (the last message is only a hint and may be stale).
    QPointer<Part> self(this);
    m_attachmentView->call(QStringLiteral("getSelection"), {}, [self, polarity, withComment, kind](bool ok, const QJsonValue &result, const QString &) {
        if (!self || !self->m_attachmentReader->isOpen()) return;
        self->saveAttachmentFeedback(polarity, ok && result.isObject() ? result.toObject() : self->m_attachmentReader->lastSelection(), withComment, kind);
    });
}

void Part::saveAttachmentFeedback(const QString &polarity, const QJsonObject &selection, bool withComment, const QString &kind, const QJsonArray &pictures)
{
    auto *overlay = m_pageView->documentFeedback();
    QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(selection, m_attachmentReader->target());
    // A live page's load (saveLiveFeedback), which the anchor keeps (html-live-pages.md §2.1).
    if (!fields.isEmpty() && selection.value(QStringLiteral("live")).isObject()) fields.insert(QStringLiteral("live"), selection.value(QStringLiteral("live")));
    if (fields.isEmpty() || !overlay->hasDocument()) {
        displayInfoMessage(fields.isEmpty() ? i18n("Select the text in the page first.") : i18n("Feedback needs a Library document."), KMessageWidget::Information, 3000);
        return;
    }
    const QJsonObject anchor = Thoughts::DocumentFeedback::attachmentAnchor(fields, overlay->documentId(), overlay->pdfSha256());
    auto make = [anchor, overlay](const QString &chosenPolarity, const QString &comment) {
        QJsonObject record = Thoughts::FeedbackVersion::stamped(Thoughts::DocumentFeedback::create(chosenPolarity, comment, anchor, Thoughts::Platform::reviewOriginId()),
                                                                overlay->reviewVersion());
        record.insert(QStringLiteral("view"), Thoughts::DocumentFeedback::attachmentView());
        return record;
    };
    auto save = [this, overlay](const QJsonObject &record) {
        QString error = Thoughts::DocumentFeedback::validationError(record);
        if (!error.isEmpty() || !overlay->write(record, &error)) {
            displayInfoMessage(i18n("The feedback could not be saved: %1", error), KMessageWidget::Error, 5000);
            return;
        }
        clearAttachmentSelection();
        if (showAnnotationsForNewEntry()) m_reviewsWidget->selectFeedback(str(record, QStringLiteral("feedback_id")));
    };
    if (!withComment) {
        save(Thoughts::DocumentFeedback::withPictures(make(polarity, QString()), pictures));
        return;
    }
    // Nothing is saved until Save; the pictures are in the editor, to keep or remove.
    QJsonObject draft = Thoughts::DocumentFeedback::withPictures(Thoughts::DocumentFeedback::withKind(make(polarity, QString()), kind), pictures);
    draft.insert(QStringLiteral("feedback_id"), QString());
    auto *editor = new Thoughts::ReviewFeedbackEditor(draft, true, widget(), true);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->enablePictures(m_pageView->feedbackPictureStore());
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [make, save, editor](const QString &chosen, const QString &comment) {
        save(Thoughts::DocumentFeedback::withPictures(
            Thoughts::DocumentFeedback::withKind(make(editor->kind().isEmpty() ? chosen : Thoughts::ReviewCardFeedback::positive(), Thoughts::ReviewCardFeedback::cleanComment(comment)), editor->kind()),
            editor->pictures()));
        editor->close();
    });
    editor->popupAt(QRect(QCursor::pos(), QSize(1, 1)));
}

void Part::clearAttachmentSelection()
{
    if (!m_attachmentReader->isLive()) {
        m_attachmentView->call(QStringLiteral("clearSelection"));
        return;
    }
    // As the reader's click elsewhere would (the page sees only that its selection went).
    m_attachmentView->callLive(QStringLiteral("const selection = window.getSelection(); if (selection) selection.removeAllRanges(); return '';"), {},
                               [](bool, const QString &, const QString &) {});
}

bool Part::showsLivePage(const Thoughts::HtmlAttachmentTarget &target, const QUrl &address) const
{
    return m_attachmentShown && m_attachmentReader->isLive() && m_attachmentReader->liveAddress() == address && m_attachmentReader->target().path == target.path
        && m_attachmentReader->target().sha256 == target.sha256;
}

void Part::requestLiveSelectionMenu()
{
    if (!m_attachmentReader || !m_attachmentReader->isLive() || !m_attachmentShown) return;
    // Where the reader clicked: the menu waits for the selection, a moment.
    const QPoint at = QCursor::pos();
    const auto target = m_attachmentReader->target();
    const QUrl address = m_attachmentReader->liveAddress();
    QPointer<Part> self(this);
    m_attachmentReader->readLiveSelection([self, at, target, address](const QJsonObject &selection) {
        if (!self || !self->showsLivePage(target, address)) return;
        QMenu menu(self->widget());
        self->addLiveSelectionMenuActions(&menu, selection);
        menu.exec(at);
    });
}

bool Part::liveSelectionFeedbackOffered() const
{
    // As on an embedded page: feedback needs a Library document, and none during a review.
    return m_attachmentReader && m_attachmentReader->isLive() && m_attachmentShown && m_attachmentReader->reviewCardId().isEmpty()
        && m_pageView->documentFeedback()->hasDocument();
}

void Part::chooseLiveSelection(const QString &choice)
{
    if (!liveSelectionFeedbackOffered()) return;
    const auto target = m_attachmentReader->target();
    const QUrl address = m_attachmentReader->liveAddress();
    QPointer<Part> self(this);
    m_attachmentReader->readLiveSelection([self, choice, target, address](const QJsonObject &selection) {
        if (!self || !self->showsLivePage(target, address)) return;
        const QJsonObject quote = selection.value(QStringLiteral("text_quote")).toObject();
        if (str(quote, QStringLiteral("exact")).trimmed().isEmpty()) {
            self->m_attachmentView->setNotice(i18n("The selected text could not be read. Select it again."), QString());
            return;
        }
        const QString liked = Thoughts::ReviewCardFeedback::positive();
        if (choice == QLatin1String("like")) self->giveLiveFeedback(liked, false, QString(), selection);
        else if (choice == QLatin1String("dislike")) self->giveLiveFeedback(Thoughts::ReviewCardFeedback::negative(), false, QString(), selection);
        else if (choice == QLatin1String("question")) self->giveLiveFeedback(liked, true, Thoughts::DocumentFeedback::question(), selection);
        else if (choice == QLatin1String("comment")) self->giveLiveFeedback(liked, true, Thoughts::DocumentFeedback::other(), selection);
    });
}

void Part::addLiveSelectionMenuActions(QMenu *menu, const QJsonObject &selection)
{
    const QJsonObject quote = selection.value(QStringLiteral("text_quote")).toObject();
    // As on an embedded page: feedback needs a Library document, and none during a review. A
    // selection that cannot be read (a form field, a frame) has Copy alone.
    if (!str(quote, QStringLiteral("exact")).trimmed().isEmpty() && m_attachmentReader->reviewCardId().isEmpty() && m_pageView->documentFeedback()->hasDocument()) {
        const QString liked = Thoughts::ReviewCardFeedback::positive();
        const QString disliked = Thoughts::ReviewCardFeedback::negative();
        menu->addAction(i18n("Like"), this, [this, liked, selection] { giveLiveFeedback(liked, false, QString(), selection); })
            ->setObjectName(QStringLiteral("attachmentFeedbackLike"));
        menu->addAction(i18n("Dislike"), this, [this, disliked, selection] { giveLiveFeedback(disliked, false, QString(), selection); })
            ->setObjectName(QStringLiteral("attachmentFeedbackDislike"));
        menu->addAction(i18n("Question…"), this, [this, liked, selection] { giveLiveFeedback(liked, true, Thoughts::DocumentFeedback::question(), selection); })
            ->setObjectName(QStringLiteral("attachmentFeedbackQuestion"));
        menu->addAction(i18n("Comment…"), this, [this, liked, selection] { giveLiveFeedback(liked, true, Thoughts::DocumentFeedback::other(), selection); })
            ->setObjectName(QStringLiteral("attachmentFeedbackComment"));
        menu->addSeparator();
    }
    // The web view's own Copy, as its menu had it.
    menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy"), this, [this] { m_attachmentView->copyLiveSelection(); })
        ->setObjectName(QStringLiteral("attachmentCopy"));
}

void Part::giveLiveFeedback(const QString &polarity, bool withComment, const QString &kind, const QJsonObject &selection)
{
    if (!m_attachmentReader || !m_attachmentReader->isLive() || !m_attachmentReader->reviewCardId().isEmpty()) return;
    const auto target = m_attachmentReader->target();
    const QUrl address = m_attachmentReader->liveAddress();
    QPointer<Part> self(this);
    const auto save = [self, polarity, withComment, kind, selection, target, address](const QImage &shown) {
        if (!self || !self->showsLivePage(target, address)) return;
        self->saveLiveFeedback(polarity, selection, withComment, kind, shown);
    };
    // The view as it is now, the passage still selected (html-live-pages.md 2.1): a live page
    // changes. Not taken where the server keeps no pictures.
    if (!livePagePicturesSupported()) save(QImage());
    else if (!s_livePagePictureForTesting.isNull()) save(s_livePagePictureForTesting);
    else m_attachmentView->snapshotPage(save);
}

void Part::saveLiveFeedback(const QString &polarity, const QJsonObject &selection, bool withComment, const QString &kind, const QImage &shown)
{
    if (!m_attachmentReader || !m_attachmentReader->isLive() || !m_attachmentReader->reviewCardId().isEmpty()) return;
    QJsonArray pictures;
    if (livePagePicturesSupported()) {
        QString error = i18n("The web view gave no picture.");
        const QJsonObject picture = shown.isNull() ? QJsonObject() : keepLivePagePicture(shown, &error);
        // The feedback is kept anyway, without its picture.
        if (picture.isEmpty()) qWarning().noquote() << "Live page feedback without its picture:" << error;
        else pictures.append(picture);
    }
    // The load it was given on: after a Reload it is from an earlier version (html-live-pages.md §2.1).
    QJsonObject placed = selection;
    if (!m_liveLoadedAt.isEmpty())
        placed.insert(QStringLiteral("live"), QJsonObject{{QStringLiteral("address"), m_attachmentReader->liveAddress().toString()},
                                                         {QStringLiteral("loaded_at"), m_liveLoadedAt}});
    saveAttachmentFeedback(polarity, placed, withComment, kind, pictures);
}

bool Part::livePagePicturesSupported() const
{
    // The server's last answer (showHtmlAttachment asks it again); without it the record goes without.
    return m_fedoraRepository && m_fedoraRepository->knownServerCapabilities().contains(Thoughts::DocumentFeedback::picturesCapability());
}

QJsonObject Part::keepLivePagePicture(const QImage &shown, QString *error)
{
    // Opaque as on screen (JPEG has no transparency), in the snapshot's own pixels.
    QImage source = shown;
    source.setDevicePixelRatio(1.0);
    QImage flat(source.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, source);
    }
    QString mediaType;
    const QByteArray bytes = Thoughts::ReviewFeedbackEditor::encodedPicture(flat, false, &mediaType);
    QImage kept;
    kept.loadFromData(bytes);
    if (bytes.isEmpty() || kept.isNull()) {
        if (error) *error = i18n("The picture could not be written.");
        return {};
    }
    const auto store = m_pageView->feedbackPictureStore();
    const QString sha256 = store.keep ? store.keep(bytes, error) : QString();
    if (sha256.isEmpty()) return {};
    return Thoughts::DocumentFeedback::snapshotJson(sha256, mediaType, kept.width(), kept.height());
}

bool Part::requestHtmlAttachmentPicture(const QString &requestId)
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || !m_attachmentShown) return false;
    QPointer<Part> self(this);
    const auto give = [self, requestId](const QImage &picture) {
        if (self) Q_EMIT self->htmlAttachmentPictureReady(requestId, picture);
    };
    // Tests (no web view): the picture they chose, given later as the web view's is.
    if (!s_livePagePictureForTesting.isNull()) {
        QTimer::singleShot(0, this, [give] { give(s_livePagePictureForTesting); });
        return true;
    }
    if (m_attachmentReader->isLive() ? m_liveState != LiveLoaded : !m_attachmentReader->isReady()) return false;
    m_attachmentView->snapshotPage(give);
    return true;
}

void Part::drawOnAttachmentPage()
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || !m_attachmentShown) return;
    if (!m_pageView->documentFeedback()->hasDocument()) {
        displayInfoMessage(i18n("Drawings are kept with Library documents, and this page's PDF is not one."), KMessageWidget::Information, 4000);
        return;
    }
    if (!m_attachmentReader->reviewCardId().isEmpty()) {
        displayInfoMessage(i18n("Not during a review."), KMessageWidget::Information, 3000);
        return;
    }
    const bool live = m_attachmentReader->isLive();
    if (live ? m_liveState != LiveLoaded : !m_attachmentReader->isReady()) {
        displayInfoMessage(live && m_liveState != LiveLoading ? i18n("The live page is not shown.") : i18n("The page is still loading."), KMessageWidget::Information, 3000);
        return;
    }
    QPointer<Part> self(this);
    const auto target = m_attachmentReader->target();
    // The page as the user sees it, with the page state it was in.
    const auto take = [self, target](const QJsonObject &state) {
        self->m_attachmentView->snapshotPage([self, target, state](const QImage &image) {
            if (!self || !self->m_attachmentShown || self->m_attachmentReader->target().sha256 != target.sha256) return;
            if (image.isNull()) {
                self->displayInfoMessage(i18n("Could not take a picture of this page."), KMessageWidget::Warning, 5000);
                return;
            }
            QImage shown = image;
            // At the size it has on screen (the snapshot has the screen's pixels).
            const QWidget *container = self->m_attachmentView->webContainer();
            if (container && container->width() > 0 && shown.width() > container->width()) shown.setDevicePixelRatio(qreal(shown.width()) / container->width());
            self->openNewAttachmentDrawing(shown, target, state);
        });
    };
    // A live page has no viewer to ask for its state (its own address is its place).
    if (live) {
        take(QJsonObject());
        return;
    }
    // The page state now (its tab), then the picture.
    m_attachmentView->call(QStringLiteral("getPageState"), {}, [self, target, take](bool ok, const QJsonValue &result, const QString &) {
        if (!self || !self->m_attachmentShown || self->m_attachmentReader->target().sha256 != target.sha256) return;
        take(ok && result.isObject() ? result.toObject() : self->m_attachmentReader->pageState());
    });
}

Thoughts::SnapshotDrawingDialog *Part::newSnapshotDrawingDialog(const QImage &snapshot, const QJsonObject &record)
{
    // One drawing at a time (the dialog is modal).
    if (m_snapshotDrawing) m_snapshotDrawing->close();
    // The pencil's colour as chosen last; it starts drawing, not erasing.
    auto *dialog = new Thoughts::SnapshotDrawingDialog(snapshot, Thoughts::DocumentFeedback::strokesOf(record), !record.isEmpty(), Okular::Settings::pencilColor(), false, widget());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    // A colour chosen here is the pencil's everywhere, as in the menu of its button.
    connect(dialog, &Thoughts::SnapshotDrawingDialog::pencilChosen, this, [](const QString &colour, bool eraser) {
        if (eraser) return;
        Okular::Settings::setPencilColor(colour);
        Okular::Settings::setPencilEraser(false);
        Okular::Settings::self()->save();
    });
    m_snapshotDrawing = dialog;
    return dialog;
}

void Part::openNewAttachmentDrawing(QImage snapshot, const Thoughts::HtmlAttachmentTarget &target, const QJsonObject &pageState)
{
    auto *overlay = m_pageView->documentFeedback();
    // Its anchor is that of feedback in the page (§8), with the page state the snapshot shows.
    const QJsonObject placement = Thoughts::HtmlAttachmentReader::newCardFields(
        {{QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("Drawing")}}}, {QStringLiteral("page_state"), pageState}}, target);
    if (placement.isEmpty() || !overlay->hasDocument()) {
        displayInfoMessage(i18n("Drawings are kept with Library documents, and this page's PDF is not one."), KMessageWidget::Information, 4000);
        return;
    }
    // A picture of a screen; the server keeps one up to 20000 pixels on a side.
    constexpr int largest = 8192;
    if (std::max(snapshot.width(), snapshot.height()) > largest) {
        const qreal ratio = snapshot.devicePixelRatio() * largest / std::max(snapshot.width(), snapshot.height());
        snapshot = snapshot.scaled(QSize(largest, largest), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        snapshot.setDevicePixelRatio(ratio);
    }
    auto *dialog = newSnapshotDrawingDialog(snapshot, {});
    connect(dialog, &Thoughts::SnapshotDrawingDialog::saveRequested, this, [this, dialog, snapshot, placement](const QList<Thoughts::DocumentFeedback::Stroke> &strokes) {
        auto *overlay = m_pageView->documentFeedback();
        // The picture first: it is kept here and uploaded before the record (FedoraRepository).
        QByteArray png;
        QBuffer buffer(&png);
        QString error;
        const QString sha256 = buffer.open(QIODevice::WriteOnly) && snapshot.save(&buffer, "PNG") && m_fedoraRepository
            ? m_fedoraRepository->keepFeedbackSnapshot(png, &error) : QString();
        if (sha256.isEmpty()) {
            displayInfoMessage(i18n("The drawing could not be saved: %1", error.isEmpty() ? i18n("The picture of the page could not be kept.") : error), KMessageWidget::Error, 5000);
            return;
        }
        const QJsonObject record = Thoughts::FeedbackVersion::stamped(
            Thoughts::DocumentFeedback::createAttachmentDrawing(placement, Thoughts::DocumentFeedback::snapshotJson(sha256, QStringLiteral("image/png"), snapshot.width(), snapshot.height()),
                                                                strokes, overlay->documentId(), overlay->pdfSha256(), Thoughts::Platform::reviewOriginId()),
            overlay->reviewVersion());
        error = Thoughts::DocumentFeedback::validationError(record);
        if (!error.isEmpty() || !overlay->write(record, &error)) {
            displayInfoMessage(i18n("The drawing could not be saved: %1", error), KMessageWidget::Error, 5000);
            return;
        }
        dialog->accept();
        if (showAnnotationsForNewEntry()) m_reviewsWidget->selectFeedback(str(record, QStringLiteral("feedback_id")));
    });
    dialog->show();
}

void Part::openAttachmentDrawing(const QString &feedbackId)
{
    const QJsonObject record = m_pageView->documentFeedback()->record(feedbackId);
    if (!Thoughts::DocumentFeedback::isAttachmentDrawing(record) || record.value(QStringLiteral("deleted")).toBool()) return;
    const QString sha256 = Thoughts::DocumentFeedback::snapshotSha256(record);
    QPointer<Part> self(this);
    const auto open = [self, feedbackId, sha256](bool found) {
        if (!self) return;
        const QJsonObject current = self->m_pageView->documentFeedback()->record(feedbackId);
        if (current.isEmpty() || current.value(QStringLiteral("deleted")).toBool()) return;
        QImage snapshot;
        if (found && self->m_fedoraRepository) snapshot.loadFromData(self->m_fedoraRepository->feedbackSnapshot(sha256));
        if (snapshot.isNull()) {
            self->displayInfoMessage(i18n("The picture of this drawing is not on this computer yet. Sync, then open it again."), KMessageWidget::Warning, 5000);
            return;
        }
        // The screen's pixels, as when it was taken.
        snapshot.setDevicePixelRatio(std::max<qreal>(1.0, self->widget()->devicePixelRatioF()));
        auto *dialog = self->newSnapshotDrawingDialog(snapshot, current);
        Part *part = self.data();
        connect(dialog, &Thoughts::SnapshotDrawingDialog::saveRequested, part, [part, dialog, feedbackId](const QList<Thoughts::DocumentFeedback::Stroke> &strokes) {
            auto *overlay = part->m_pageView->documentFeedback();
            const QJsonObject latest = overlay->record(feedbackId);
            QString error;
            if (latest.isEmpty() || latest.value(QStringLiteral("deleted")).toBool()) error = i18n("This drawing was deleted.");
            // The next revision: its anchor and snapshot as first written, these strokes.
            else if (!overlay->write(Thoughts::FeedbackVersion::stamped(Thoughts::DocumentFeedback::withStrokes(latest, strokes, Thoughts::Platform::reviewOriginId()), overlay->reviewVersion()),
                                     &error)
                     && error.isEmpty())
                error = i18n("It could not be written.");
            if (!error.isEmpty()) {
                part->displayInfoMessage(i18n("The drawing could not be saved: %1", error), KMessageWidget::Error, 5000);
                return;
            }
            dialog->accept();
        });
        // As other feedback is deleted: the tombstone, and Undo for a few seconds.
        connect(dialog, &Thoughts::SnapshotDrawingDialog::deleteRequested, part, [part, dialog, feedbackId] {
            if (part->m_pageView->deleteDocumentFeedback(feedbackId)) dialog->accept();
        });
        dialog->show();
    };
    // A drawing made on another computer: its picture comes from the server first.
    if (!m_fedoraRepository) open(false);
    else m_fedoraRepository->fetchFeedbackSnapshot(sha256, this, open);
}

QString Part::attachmentVersion(const QString &path)
{
    // The live page shown is the version of its current load (DocumentFeedback::shownOn).
    const QString load = m_attachmentShown && m_attachmentReader && m_attachmentReader->isLive() && m_attachmentReader->target().path == path
        ? m_liveLoadedAt : QString();
    const auto known = m_attachmentVersions.constFind(path);
    QString sha256;
    if (known != m_attachmentVersions.cend()) {
        sha256 = *known;
    } else {
        QString error;
        sha256 = resolveHtmlPage(path, &error).sha256;
        m_attachmentVersions.insert(path, sha256);
    }
    return Thoughts::DocumentFeedback::attachmentVersionOf(sha256, load);
}

void Part::showFeedback(const QString &feedbackId, bool edit)
{
    const QJsonObject record = m_pageView->documentFeedback()->record(feedbackId);
    // A drawing in a page is on its snapshot, never on the live page: the snapshot opens, wherever this is.
    if (Thoughts::DocumentFeedback::isAttachmentDrawing(record)) {
        openAttachmentDrawing(feedbackId);
        return;
    }
    if (!Thoughts::DocumentFeedback::isAttachmentFeedback(record)) {
        // A page's tab has no PDF to show: the document's tab shows the passage.
        if (m_attachmentTab) {
            Q_EMIT requestOpenDocumentFeedback(localFilePath(), feedbackId);
            return;
        }
        if (!m_attachmentReviewShown) hideHtmlAttachment();
        // Asked for: its passage shows, also when its kind's markup was off.
        showMarkupOfFeedback(feedbackId);
        m_pageView->showDocumentFeedback(feedbackId, edit);
        return;
    }
    QString error;
    const auto target = namedAttachment(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject(), localFilePath(), m_docsRenderRoot, &error);
    // Not during a review of this document (its page is in this tab): the page's own tab.
    if (target.isValid() && !m_attachmentReviewShown && wantsHtmlAttachmentTab(target.path)) {
        Q_EMIT requestHtmlAttachmentTab({{QStringLiteral("path"), target.path}, {QStringLiteral("feedback_id"), feedbackId}, {QStringLiteral("edit"), edit}});
        return;
    }
    if (!target.isValid() || !showHtmlAttachment(target, false, &error)) {
        displayInfoMessage(error.isEmpty() ? i18n("The HTML attachment of this feedback is not on this computer.") : error, KMessageWidget::Warning, 6000);
    } else {
        m_attachmentReader->showFeedback(record);
    }
    if (edit) m_pageView->openDocumentFeedbackEditor(feedbackId, QRect(QCursor::pos(), QSize(1, 1)));
}

bool Part::presentAttachmentReview()
{
    if (!m_attachmentReader) return false;
    const QJsonObject card = m_reviewCardRecord;
    QString error;
    const auto target = cardAttachment(card, localFilePath(), m_docsRenderRoot, &error);
    if (!target.isValid() || !showHtmlAttachment(target, true, &error)) {
        displayInfoMessage(error.isEmpty() ? i18n("The HTML attachment of this card is not on this computer.") : error, KMessageWidget::Warning, 6000);
        return false;
    }
    m_attachmentView->setNotice(QString(), QString());
    m_attachmentReviewShown = m_attachmentReader->presentReview(card, false);
    updateAttachmentSelectionAction();
    return m_attachmentReviewShown;
}

bool Part::showHtmlAttachmentCard(const QJsonObject &card)
{
    if (!Thoughts::isHtmlAttachmentCard(card) || !m_attachmentReader) return false;
    QString error;
    const auto target = cardAttachment(card, localFilePath(), m_docsRenderRoot, &error);
    // Opening a card is reading, not a review: its page's own tab, as for a link.
    if (target.isValid() && wantsHtmlAttachmentTab(target.path)) {
        Q_EMIT requestHtmlAttachmentTab({{QStringLiteral("path"), target.path}, {QStringLiteral("card"), card}});
        return true;
    }
    if (!target.isValid() || !showHtmlAttachment(target, false, &error)) {
        displayInfoMessage(error.isEmpty() ? i18n("The HTML attachment of this card is not on this computer.") : error, KMessageWidget::Warning, 6000);
        return false;
    }
    m_attachmentReader->showCard(card);
    return true;
}

QVariantMap Part::htmlAttachmentState() const
{
    if (!m_attachmentReader) return {};
    const auto &target = m_attachmentReader->target();
    return {{QStringLiteral("shown"), m_attachmentShown},
            {QStringLiteral("for_review"), m_attachmentForReview},
            {QStringLiteral("tab"), m_attachmentTab},
            {QStringLiteral("title"), m_attachmentTitle},
            {QStringLiteral("path"), target.path},
            {QStringLiteral("sha256"), target.sha256},
            {QStringLiteral("url"), m_attachmentReader->pageUrl().toString()},
            {QStringLiteral("ready"), m_attachmentReader->isReady()},
            {QStringLiteral("transport"), m_attachmentView->transportKind()},
            {QStringLiteral("message"), m_attachmentView->message()},
            {QStringLiteral("notice"), m_attachmentView->notice()},
            {QStringLiteral("cards_supported"), m_attachmentCardsSupported},
            {QStringLiteral("new_card_enabled"), m_attachmentView->selectionActionEnabled()},
            {QStringLiteral("selection"), m_attachmentReader->lastSelection().toVariantMap()},
            {QStringLiteral("page_state"), m_attachmentReader->pageState().toVariantMap()},
            {QStringLiteral("page_cards"), int(m_attachmentReader->pageCards().size())},
            {QStringLiteral("placement"), m_attachmentReader->placement().toVariantMap()},
            {QStringLiteral("review_card"), m_attachmentReader->reviewCardId()},
            {QStringLiteral("review_state"), m_attachmentReader->reviewState()},
            {QStringLiteral("review_blocks"), m_attachmentReader->reviewBlockCount()},
            {QStringLiteral("review_revealed"), m_attachmentReader->reviewRevealed()},
            {QStringLiteral("new_card_tip"), m_attachmentView->selectionActionToolTip()},
            {QStringLiteral("live"), m_attachmentReader->isLive()},
            {QStringLiteral("live_url"), m_attachmentReader->liveAddress().toString()},
            {QStringLiteral("live_state"), m_liveState},
            {QStringLiteral("live_bar"), m_attachmentView->liveBarText()},
            {QStringLiteral("snapshot_enabled"), m_attachmentView->takeSnapshotEnabled()},
            {QStringLiteral("snapshot_tip"), m_attachmentView->takeSnapshotToolTip()},
            {QStringLiteral("message_action"), m_attachmentView->messageActionText()},
            {QStringLiteral("snapshot_of"),
             m_attachmentReader->isOpen() && !m_attachmentReader->isLive() ? Thoughts::LivePage::snapshotSourceOfFile(target.file).toString() : QString()}};
}

void Part::takeLiveSnapshot()
{
    if (!m_attachmentReader || !m_attachmentReader->isLive() || m_liveState != LiveLoaded || m_liveSnapshotBusy) return;
    QFile resource(QStringLiteral(":/thoughts/html-live/snapshot.js"));
    if (!resource.open(QIODevice::ReadOnly)) return;
    const QString script = QString::fromUtf8(resource.readAll());
    m_liveSnapshotBusy = true;
    updateLivePageBar();
    m_attachmentView->setNotice(i18n("Taking a snapshot\u2026"), QString());
    QPointer<Part> self(this);
    const QString path = m_attachmentReader->target().path;
    const QUrl address = m_attachmentReader->liveAddress();
    // Room for the page itself within the 100 MB of a page.
    const QJsonObject args{{QStringLiteral("limit"), 90 * 1000 * 1000}};
    m_attachmentView->callLive(script, args, [self, path, address](bool ok, const QString &result, const QString &error) {
        if (!self) return;
        self->m_liveSnapshotBusy = false;
        self->m_attachmentView->setNotice(QString(), QString());
        self->updateLivePageBar();
        // Another page (or none) meanwhile: nothing to replace.
        if (!self->m_attachmentReader->isLive() || self->m_attachmentReader->target().path != path || self->m_attachmentReader->liveAddress() != address) return;
        QString why = ok ? QString() : error;
        if ((!ok || !self->saveLiveSnapshot(result, &why)) && !why.isEmpty())
            self->displayInfoMessage(i18n("The snapshot could not be taken: %1", why), KMessageWidget::Warning, 6000);
    });
}

bool Part::saveLiveSnapshot(const QString &serialized, QString *error)
{
    if (!m_attachmentReader || !m_attachmentReader->isLive()) {
        if (error) *error = i18n("No live page is shown.");
        return false;
    }
    const QJsonObject result = QJsonDocument::fromJson(serialized.toUtf8()).object();
    const QString html = result.value(QStringLiteral("html")).toString();
    if (html.trimmed().isEmpty()) {
        if (error) *error = i18n("The page gave nothing to keep.");
        return false;
    }
    const QUrl address = m_attachmentReader->liveAddress();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime local = now.toLocalTime();
    const QString shown = QLocale().toString(local.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(local.time(), QLocale::ShortFormat);
    // The page keeps its name (the stub's title) in lists and tabs.
    const QByteArray bytes = Thoughts::LivePage::finishSnapshot(html, address, m_attachmentTitle, now, shown, error);
    if (bytes.isEmpty()) return false;
    const bool replace = s_livePageAnswerForTesting >= 0
        ? s_livePageAnswerForTesting == 1
        : KMessageBox::questionTwoActions(widget(), i18n("Replace the live page with this snapshot? The link stays the same."), i18n("Take Snapshot"),
                                          KGuiItem(i18n("Replace")), KStandardGuiItem::cancel())
            == KMessageBox::PrimaryAction;
    if (!replace) {
        if (error) error->clear();
        return false;
    }
    return replaceHtmlPage(bytes, error);
}

bool Part::goLiveAgain()
{
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || m_attachmentReader->isLive()) return false;
    const QUrl address = Thoughts::LivePage::snapshotSource(readPage(m_attachmentReader->target().file, 256 * 1024));
    if (!address.isValid()) return false;
    const QByteArray stub = Thoughts::LivePage::stubHtml(address, m_attachmentTitle);
    if (stub.isEmpty()) return false;
    const bool replace = s_livePageAnswerForTesting >= 0
        ? s_livePageAnswerForTesting == 1
        : KMessageBox::questionTwoActions(widget(), i18n("Replace this snapshot with the live page at %1? The link stays the same.", address.toDisplayString()),
                                          i18n("Go Live Again"), KGuiItem(i18n("Go Live")), KStandardGuiItem::cancel())
            == KMessageBox::PrimaryAction;
    if (!replace) return false;
    QString error;
    if (!replaceHtmlPage(stub, &error)) {
        displayInfoMessage(i18n("The page could not go live again: %1", error), KMessageWidget::Warning, 6000);
        return false;
    }
    return true;
}

bool Part::replaceHtmlPage(const QByteArray &bytes, QString *error)
{
    const QString path = m_attachmentReader->target().path;
    // The page's file in its folder, as any page is replaced on this computer; Sync publishes it
    // (html-pages.md §4) and the PDFs that link it rebuild.
    if (!Thoughts::LivePage::writeLibraryPage(m_docsRenderRoot, path, bytes, error)) return false;
    // Shown here until the PDF's source bundle carries it.
    QString kept;
    if (!Thoughts::LivePage::rememberWrite(path, bytes, &kept)) qWarning().noquote() << "HTML page:" << kept;
    m_attachmentVersions.remove(path);
    Q_EMIT globalSyncRequested();
    // The page as it is now: embedded after a snapshot, live again after Go Live Again.
    QString shownError;
    const auto page = resolveHtmlPage(path, &shownError);
    if (!page.isValid() || !showHtmlAttachment(page, false, &shownError)) {
        displayInfoMessage(i18n("The page was saved; it shows after the next Sync. %1", shownError), KMessageWidget::Information, 6000);
        return true;
    }
    displayInfoMessage(i18n("The page was saved. Sync sends it to the server."), KMessageWidget::Positive, 4000);
    return true;
}
}
