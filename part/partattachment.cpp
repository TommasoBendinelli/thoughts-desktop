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
#include "documentfeedback.h"
#include "documentfeedbackoverlay.h"
#include "documentidentity.h"
#include "fedorarepository.h"
#include "feedbackversion.h"
#include "htmlattachmentreader.h"
#include "htmldocumentview.h"
#include "htmledition.h"
#include "htmlreader.h"
#include "pageview.h"
#include "reviewcardfeedback.h"
#include "reviewfeedbackeditor.h"
#include "side_reviews.h"
#include "thoughtsplatform.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardGuiItem>

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMenu>
#include <QMetaMethod>
#include <QRegularExpression>
#include <QSet>
#include <QStackedWidget>
#include <QTextDocumentFragment>
#include <QTimer>

namespace
{
QString str(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

const QString AttachmentCardsCapability = QStringLiteral("html_attachment_cards_v1");

bool s_withoutWebViewForTesting = false;

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
    if ((!Thoughts::HtmlDocumentView::webViewAvailable() && !s_withoutWebViewForTesting) || !m_attachmentReader) {
        // No web view in this build: the browser, as before.
        if (!Thoughts::openHtmlAttachment(link, QUrl::fromLocalFile(localFilePath()), &error)) {
            displayInfoMessage(error, KMessageWidget::Warning, 6000);
            return false;
        }
        return true;
    }
    const auto target = Thoughts::resolveHtmlAttachment(path, localFilePath(), &error);
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
    const auto target = Thoughts::resolveHtmlAttachment(str(request, QStringLiteral("path")), localFilePath(), &error);
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
    if (!m_attachmentReader->open(target, attachmentOwner(localFilePath(), m_docsRenderRoot), error)) return false;
    m_attachmentTitle = attachmentTitle(target);
    m_attachmentReader->setCards(m_sidebarCardRecords);
    m_attachmentReader->setFeedback(m_pageView->documentFeedback()->records());
    if (!m_attachmentShown) {
        m_attachmentShown = true;
        m_attachmentForReview = forReview;
        m_documentStack->setCurrentWidget(m_attachmentView);
        // The review panel floats over a native web view (see htmlViewActiveChanged).
        widget()->setProperty("thoughtsNativeDocumentView", Thoughts::HtmlDocumentView::webViewAvailable());
        if (QWidget *container = widget()->findChild<QWidget *>(QStringLiteral("thoughtsPageSplitter"))) container->parentWidget()->setFocusProxy(m_attachmentView);
    } else if (!forReview) {
        // Opened by the user: it stays when the review ends.
        m_attachmentForReview = false;
    }
    if (!m_attachmentReviewShown) m_attachmentView->setNotice(QString(), QString());
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
    return true;
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
}

void Part::closeHtmlAttachment()
{
    if (!m_attachmentReader) return;
    hideHtmlAttachment();
    m_attachmentReviewShown = false;
    m_attachmentReader->close();
}

void Part::updateAttachmentSelectionAction()
{
    if (!m_attachmentView || !m_attachmentReader) return;
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
    if (!m_attachmentReader || !m_attachmentReader->isOpen() || !m_attachmentCardsSupported || !m_attachmentReader->reviewCardId().isEmpty()) return;
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
        QDesktopServices::openUrl(url);
        return;
    }
#ifdef Q_OS_MACOS
    displayInfoMessage(i18n("The page tried to open %1 by itself. It was not opened.", url.toDisplayString(QUrl::RemoveQuery | QUrl::RemoveFragment)), KMessageWidget::Warning, 5000);
#else
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
    QUrl url = Thoughts::serveHtmlAttachmentFile(m_attachmentReader->target().file);
    // The tab shown here (the page's #hash), when it has one.
    const QString hash = str(m_attachmentReader->pageState(), QStringLiteral("hash"));
    if (url.isValid() && hash.startsWith(QLatin1Char('#')) && hash.size() > 1) url.setFragment(hash.mid(1));
    if (!url.isValid() || !QDesktopServices::openUrl(url)) displayInfoMessage(i18n("Could not open the default browser."), KMessageWidget::Warning, 5000);
}

namespace
{
// The attachment of a card or feedback: the current version at its path, else the version it was
// made on.
Thoughts::HtmlAttachmentTarget namedAttachment(const QJsonObject &ref, const QString &pdfPath, QString *error)
{
    const QString path = str(ref, QStringLiteral("path"));
    auto target = Thoughts::resolveHtmlAttachment(path, pdfPath, error);
    if (!target.isValid() && Thoughts::isHtmlSourcePath(path)) {
        const QString sha = str(ref, QStringLiteral("sha256"));
        const QString file = Thoughts::htmlAttachmentObject(sha, pdfPath);
        if (!file.isEmpty()) target = {path, file, sha};
    }
    return target;
}

Thoughts::HtmlAttachmentTarget cardAttachment(const QJsonObject &card, const QString &pdfPath, QString *error)
{
    return namedAttachment(card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("html_attachment")).toObject(), pdfPath, error);
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

void Part::saveAttachmentFeedback(const QString &polarity, const QJsonObject &selection, bool withComment, const QString &kind)
{
    auto *overlay = m_pageView->documentFeedback();
    const QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(selection, m_attachmentReader->target());
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
        m_attachmentView->call(QStringLiteral("clearSelection"));
        if (showAnnotationsForNewEntry()) m_reviewsWidget->selectFeedback(str(record, QStringLiteral("feedback_id")));
    };
    if (!withComment) {
        save(make(polarity, QString()));
        return;
    }
    // Nothing is saved until Save.
    QJsonObject draft = Thoughts::DocumentFeedback::withKind(make(polarity, QString()), kind);
    draft.insert(QStringLiteral("feedback_id"), QString());
    auto *editor = new Thoughts::ReviewFeedbackEditor(draft, true, widget(), true);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [make, save, editor](const QString &chosen, const QString &comment) {
        save(Thoughts::DocumentFeedback::withKind(make(editor->kind().isEmpty() ? chosen : Thoughts::ReviewCardFeedback::positive(), Thoughts::ReviewCardFeedback::cleanComment(comment)), editor->kind()));
        editor->close();
    });
    editor->popupAt(QRect(QCursor::pos(), QSize(1, 1)));
}

QString Part::attachmentVersion(const QString &path)
{
    const auto known = m_attachmentVersions.constFind(path);
    if (known != m_attachmentVersions.cend()) return *known;
    QString error;
    const QString sha256 = Thoughts::resolveHtmlAttachment(path, localFilePath(), &error).sha256;
    m_attachmentVersions.insert(path, sha256);
    return sha256;
}

void Part::showFeedback(const QString &feedbackId, bool edit)
{
    const QJsonObject record = m_pageView->documentFeedback()->record(feedbackId);
    if (!Thoughts::DocumentFeedback::isAttachmentFeedback(record)) {
        // A page's tab has no PDF to show: the document's tab shows the passage.
        if (m_attachmentTab) {
            Q_EMIT requestOpenDocumentFeedback(localFilePath(), feedbackId);
            return;
        }
        if (!m_attachmentReviewShown) hideHtmlAttachment();
        m_pageView->showDocumentFeedback(feedbackId, edit);
        return;
    }
    QString error;
    const auto target = namedAttachment(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject(), localFilePath(), &error);
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
    const auto target = cardAttachment(card, localFilePath(), &error);
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
    const auto target = cardAttachment(card, localFilePath(), &error);
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
            {QStringLiteral("review_revealed"), m_attachmentReader->reviewRevealed()}};
}
}
