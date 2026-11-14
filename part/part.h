/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003-2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Andy Goossens <andygoossens@telenet.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2004 Dominique Devriese <devriese@kde.org>
    SPDX-FileCopyrightText: 2004-2007 Albert Astals Cid <aacid@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _PART_H_
#define _PART_H_

#include <config-okular.h>

#if HAVE_DBUS
#include <QDBusAbstractAdaptor> // for Q_NOREPLY
#else                           // HAVE_DBUS
#define Q_NOREPLY
#endif // HAVE_DBUS
#include <QDateTime>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <thoughts/commentservice.h>

#include <KCompressionDevice>
#include <KIO/Job>
#include <KMessageWidget>
#include <KParts/ReadWritePart>
#include <KPluginFactory>

#include "../core/document.h"
#include "../core/observer.h"
#include "../interfaces/viewerinterface.h"
#include "../kdocumentviewer.h"

#include "okularpart_export.h"
#include "docscommentmutationstore.h"
#include "docscommentsynchronizer.h"
#include "linkoverridestore.h"
#include "shell/agentproposalparser.h"

class QAction;
class QWidget;
class QPrinter;
class QMenu;

class KConfigDialog;
class KDirWatch;
class KHamburgerMenu;
class KMainWindow;
class KActionMenu;
class KToggleAction;
class KToggleFullScreenAction;
class QTemporaryFile;
class QAction;
class QJsonObject;
class QTimer;
class QSplitter;
namespace Thoughts
{
class DocsChangeMonitor;
class LiveProposalDecisionQueue;
struct LiveProposalDecisionResult;
class HtmlDocumentView;
class HtmlReader;
class HtmlAttachmentReader;
struct HtmlAttachmentTarget;
namespace HtmlEdition
{
class Store;
}
namespace ReadAloud
{
class Controller;
}
}
class QStackedWidget;
class ReadAloudBar;
template<typename T> class QFutureWatcher;
namespace KParts
{
class GUIActivateEvent;
}

class FindBar;
class ThumbnailList;
class PageSizeLabel;
class PageView;
struct AgentProposalDecisionState;
class PresentationWidget;
class Sidebar;
class TOC;
class MiniBar;
class MiniBarLogic;
class FileKeeper;
class Reviews;
namespace Okular
{
class FedoraRepository;
}
class BookmarkList;
class DrawingToolActions;
class Layers;
class SignaturePanel;

#if HAVE_PURPOSE
namespace Purpose
{
class Menu;
}
#endif

namespace Okular
{
class BrowserExtension;
class ExportFormat;

/**
 * Describes the possible embedding modes of the part
 *
 * @since 0.14 (KDE 4.8)
 */
enum EmbedMode {
    UnknownEmbedMode,
    NativeShellMode,  // embedded in the native Okular' shell
    PrintPreviewMode, // embedded to show the print preview of a document
    ViewerWidgetMode  // the part acts as a widget that can display all kinds of documents
};

/**
 * This is a "Part".  It that does all the real work in a KPart
 * application.
 *
 * @short Main Part
 * @author Wilco Greven <greven@kde.org>
 * @version 0.2
 */
class OKULARPART_EXPORT Part : public KParts::ReadWritePart, public Okular::DocumentObserver, public KDocumentViewer, public Okular::ViewerInterface
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.okular")
    Q_INTERFACES(KDocumentViewer)
    Q_INTERFACES(Okular::ViewerInterface)

    friend class PartTest;
    friend class HtmlViewTest;

public:
    // Default constructor
    /**
     * If one element of 'args' contains one of the strings "Print/Preview" or "ViewerWidget",
     * the part will be set up in the corresponding mode. Additionally, it is possible to specify
     * which config file should be used by adding a string containing "ConfigFileName=<file name>"
     * to 'args'.
     **/
    Part(QObject *parent, const QVariantList &args);

    // Destructor
    ~Part() override;

    // inherited from DocumentObserver
    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;
    void notifyViewportChanged(bool smoothMove) override;
    void notifyPageChanged(int page, int flags) override;

    bool openDocument(const QUrl &url, uint page) override;
    void startPresentation() override;
    QStringList supportedMimeTypes() const override;

    QUrl realUrl() const;

    void showSourceLocation(const QString &fileName, int line, int column, bool showGraphically = true) override;
    void clearLastShownSourceLocation() override;
    bool isWatchFileModeEnabled() const override;
    void setWatchFileModeEnabled(bool enable) final;
    bool areSourceLocationsShownGraphically() const override;
    void setShowSourceLocationsGraphically(bool show) override;
    bool openNewFilesInTabs() const override;
    QWidget *getSideContainer() const override;
    Q_INVOKABLE bool activateTabIfAlreadyOpenFile() const;
    Q_INVOKABLE bool openReviewAnnotation(const QString &annotationId, int expectedPage);
    Q_INVOKABLE QVariantMap editReviewAnnotation(const QString &annotationId, int expectedPage, const QString &contents, const QVariantMap &metadataPatch = {});
    Q_INVOKABLE QVariantMap addReviewAnnotationReply(const QString &parentAnnotationId, int expectedPage, const QString &contents);
    Q_INVOKABLE QVariantMap deleteReviewAnnotation(const QString &annotationId, int expectedPage);
    Q_INVOKABLE QVariantMap deleteReviewAnnotations(const QVariantList &targets);
    Q_INVOKABLE QVariantMap deleteComments(const QStringList &ids);
    Q_INVOKABLE QVariantMap undoCommentDeletion(int token);
    Q_INVOKABLE QVariantMap redoCommentDeletion(int token);
    Q_INVOKABLE QVariantMap updateReviewAnnotationChatGptUrl(const QString &annotationId, const QString &chatGptUrl);
    Q_INVOKABLE QVariantMap undoReviewChange();
    Q_INVOKABLE QVariantMap redoReviewChange();
    Q_INVOKABLE QVariantMap synchronizeDocsCommentsNow();
    Q_INVOKABLE QVariantMap reviewDocumentPayload() const;
    Q_INVOKABLE void exportProjectPdf(const QJsonObject &project, const QString &destination);
    Q_INVOKABLE QVariantMap reviewDocumentPayloadForPage(int page) const;
    Q_INVOKABLE QString localDocumentPath() const;
    Q_INVOKABLE void updateFromFedora(const QString &traceId = QString());
    Q_INVOKABLE void updateFromFedoraWithoutRenderCheck(const QString &traceId = QString());
    Q_INVOKABLE void updateDocumentFromFedora(const QString &target, bool publishSources, const QString &traceId = QString());
    Q_INVOKABLE QJsonObject effectiveFedoraProjections() const;
    Q_INVOKABLE void setSidebarReviewCards(const QJsonArray &cards);
    // The document's History, a "History" choice of the Sidebar menu.
    Q_INVOKABLE bool setHistorySidebarPanel(QWidget *panel);
    // Shows the sidebar on History; false when this document has none.
    Q_INVOKABLE bool showHistorySidebar();
    // The agents' notes, an "Agent Notes" choice of the Sidebar menu (for every document);
    // the choice is named by the panel's window title.
    Q_INVOKABLE bool setAgentNotesSidebarPanel(QWidget *panel);
    // Shows the sidebar on Agent Notes; false when the Shell gave none.
    Q_INVOKABLE bool showAgentNotesSidebar();
    Q_INVOKABLE bool focusSidebarReviewCard(const QString &cardId);
    Q_INVOKABLE void openNoteAttachment(const QVariantMap &attachment);
    Q_INVOKABLE bool enqueueLogicalCommentMutation(const QVariantMap &payload);
    // A comment on a passage from the Library: written, or opened (false until this document knows it).
    Q_INVOKABLE bool enqueueDocumentFeedbackRecord(const QVariantMap &record);
    Q_INVOKABLE bool openDocumentFeedback(const QString &feedbackId);
    // Queues one agent_conversation record (part/agentconversations.h); false when it is invalid or the queue is unavailable.
    Q_INVOKABLE bool enqueueAgentConversation(const QVariantMap &record);
    // Re-insert the source markers of anchor-removed comments; returns {success, results, message}.
    Q_INVOKABLE QVariantMap restoreCommentAnchors(const QStringList &ids);
    // "Comment missing" actions: action "remove" (take the markers out of their
    // sources) or "restore" (undelete or recreate their comments); returns {success, results, message}.
    Q_INVOKABLE QVariantMap resolveMissingCommentMarkers(const QString &action, const QVariantList &markers);
    // Proposals removed outside Thoughts: Restore Proposal and Dismiss; each returns {success, results, message}.
    Q_INVOKABLE QVariantMap restoreRemovedProposals(const QStringList &ids);
    Q_INVOKABLE QVariantMap dismissRemovedProposals(const QStringList &ids);
    Q_INVOKABLE bool enqueueReviewCorrespondenceReview(const QVariantMap &payload);
    Q_INVOKABLE QVariantMap openLogicalCommentLocation(const QString &commentId, int page, const QVariantMap &location);
    Q_INVOKABLE QVariantMap documentSyncStatus(const QString &target) const;
    Q_INVOKABLE QVariantMap globalPendingChanges() const;
    Q_INVOKABLE QString resolveAgentProposals(const QString &bundleId, const QString &action, const QStringList &proposalIds);
    Q_INVOKABLE void requestSourceConflictResolutionBundle(const QString &path = QString());
    Q_INVOKABLE QString queueSourceConflictResolution(const QString &conflictId,
                                                      qint64 expectedRevision,
                                                      const QString &expectedSha256,
                                                      const QByteArray &contents);
    Q_INVOKABLE bool selectAgentProposal(const QString &proposalId);
    Q_INVOKABLE QVariantMap focusAgentProposal(const QString &proposalId, const QString &sourcePath, int sourceLine);
    Q_INVOKABLE QVariantMap openAgentProposalReview(const QString &proposalId);
    Q_INVOKABLE QVariantMap openAgentProposalReview(const QString &proposalId, const QString &sourcePath, int sourceLine);
    Q_INVOKABLE QVariantMap fedoraManualSyncState() const;
    // Asks the server whether it holds work this computer has not pulled; answers with
    // fedoraRemoteWorkChecked.
    Q_INVOKABLE void checkFedoraRemoteWork();
    Q_INVOKABLE void requestFedoraBuildForTarget(const QString &target, const QString &traceId = QString());
    Q_INVOKABLE void requestFedoraRenderStatus(const QString &target);
    Q_INVOKABLE void requestFedoraRenderStateAfterExplicitOperation();
    Q_INVOKABLE void requestFedoraBuildSourceDiff(const QString &buildId);
    Q_INVOKABLE void requestFedoraBuildInputSnapshot(const QString &target);
    Q_INVOKABLE QVariantMap desktopSourceMonitoringInformation(const QString &target);
    Q_INVOKABLE void requestDesktopSourceMonitoringInformation(const QStringList &targets, QObject *receiver, quint64 requestId);
    Q_INVOKABLE void requestFedoraLibraryActivation(const QString &target);
    Q_INVOKABLE void publishNewSourceDocument(const QString &project, const QString &target, const QString &traceId = QString());
    Q_INVOKABLE QVariantMap fedoraBuildActivity(const QString &target) const;
    Q_INVOKABLE QVariantMap fedoraSourcePublicationActivity() const;
    Q_INVOKABLE QVariantMap sourceTrackingDiagnostics() const;
    Q_INVOKABLE QVariantMap fedoraRenderStatus(const QString &target);
    Q_INVOKABLE QVariantMap fedoraRenderState() const;
    Q_INVOKABLE void importLibraryPdf(const QString &requestId, const QString &sourcePath, const QString &filename);
    Q_INVOKABLE void setLibraryDisplayName(const QString &requestId, const QString &filename, const QString &expectedSha256, const QString &displayName);
    Q_INVOKABLE void removeLibraryPdf(const QString &requestId, const QString &filename, const QString &expectedSha256);
    Q_INVOKABLE void watchFedoraTrace(const QString &traceId, qint64 afterSequence = 0);
    Q_INVOKABLE void stopWatchingFedoraTrace(const QString &traceId);
    Q_INVOKABLE QString navigationViewport() const;
    Q_INVOKABLE bool navigateToDestination(const QString &destination, const QString &viewport, bool recordHistory);
    Q_INVOKABLE void setShellNavigationHistory(bool canGoBack, bool canGoForward);
    Q_INVOKABLE bool showReviewMask(int page, const QVariantList &rects);
    Q_INVOKABLE bool showReviewMasks(const QVariantList &regions);
    // As showReviewMasks(@p regions), scrolling to the answer only when @p navigate.
    Q_INVOKABLE bool showReviewMasks(const QVariantList &regions, bool navigate);
    Q_INVOKABLE void clearReviewMask();
    Q_INVOKABLE void revealReviewMask();
    Q_INVOKABLE int reviewMaskBlockCount() const;
    // The PDF outline (bookmarks) for context notes: maps {title, page (1-based, 0 without a
    // destination in this PDF), top (fraction of the page from its top), children}. Empty in a
    // project reading, whose pages are not the cards' pages.
    Q_INVOKABLE QVariantList reviewDocumentOutline() const;
    Q_INVOKABLE bool focusReviewMaskBlock(int index);
    Q_INVOKABLE void setReviewMaskInverted(bool inverted);
    // Answer feedback (review-card-feedback.md) of the card about to be presented with
    // showReviewMasks(@p regions): drawn once revealReviewMask() shows its answer.
    Q_INVOKABLE void setReviewFeedbackCard(const QJsonObject &card, const QVariantList &regions);
    // The cards other cards' feedback may be shared from (card_id -> card record of the cards that
    // exist, not deleted), given before setReviewFeedbackCard: feedback of a card not among them is
    // never shown on another card. Forgotten when the review mask is cleared.
    Q_INVOKABLE void setReviewFeedbackHomeCards(const QJsonObject &cards);
    // The card's feedback while its answer is visible, else empty: feedback_id, polarity,
    // comment, quote, state (exact, relocated, orphaned), created_at, updated_at, card_id and
    // shared; by position. With the other cards' feedback of its document found in its answer
    // (shared feedback, never orphaned).
    Q_INVOKABLE QVariantList reviewFeedbackItems();
    Q_INVOKABLE bool openReviewFeedback(const QString &feedbackId);
    // feedback_id -> exact / relocated / orphaned for the card set by setReviewFeedbackCard, for
    // Copy for agent ("not found in the current answer"), whether or not the answer is shown.
    // Other cards' feedback (shared) is listed only where it is found in this card's answer.
    Q_INVOKABLE QVariantMap reviewFeedbackStates();
    // Marks a passage ({page, rect} regions, as a card's) and scrolls to it, without storing
    // anything: the place of a History entry in the version of the PDF it was made on.
    Q_INVOKABLE bool showPassageRegions(const QVariantList &regions);

    // The HTML view (thoughts-html-view-SPEC.md): true while this document shows its HTML edition
    // (or the reason it cannot) instead of the PDF.
    Q_INVOKABLE bool htmlViewActive() const;
    // "html" or "pdf": shows that view of the document and remembers the choice for it.
    Q_INVOKABLE void setDocumentView(const QString &view);
    // State for tests and agents: active, availability, message, edition, ready, review card, blocks, not_shown.
    Q_INVOKABLE QVariantMap htmlViewState() const;

    // HTML attachments inside Thoughts (thoughts-html-attachment-cards-SPEC.md): the page a
    // thoughts-html: link of this PDF names. In the app it opens in a tab of its own next to the
    // document (requestHtmlAttachmentTab); without a shell, in place of the document (Back
    // returns). False (with a message) when it is not on this computer.
    Q_INVOKABLE bool openHtmlAttachmentLink(const QUrl &link);
    // A card made in an attachment: its page (in its tab, as for a link), at the card's page
    // state and text, without a mask.
    Q_INVOKABLE bool showHtmlAttachmentCard(const QJsonObject &card);
    // A page's tab (Shell::showHtmlAttachmentTab): this new Part opens @p pdf and shows only the
    // page @p request names ("path"), with Close (requestCloseHtmlAttachmentTab) in place of Back.
    // False when the page cannot be shown.
    Q_INVOKABLE bool openHtmlAttachmentTab(const QUrl &pdf, const QJsonObject &request);
    // In a page's tab: its page again, at the feedback ("feedback_id", "edit") or the card
    // ("card") @p request names.
    Q_INVOKABLE bool showInHtmlAttachmentTab(const QJsonObject &request);
    // A page's tab is the Part of its page alone: it no longer shows the PDF, and Close closes it.
    Q_INVOKABLE bool isHtmlAttachmentTab() const
    {
        return m_attachmentTab;
    }
    // The page's own title (its <title>, else its file name), for the tab that shows it.
    Q_INVOKABLE QString htmlAttachmentTitle() const
    {
        return m_attachmentTitle;
    }
    // The last tab of a window was a page's: its Part stays for the next document, as an
    // ordinary one.
    Q_INVOKABLE void leaveHtmlAttachmentTab();
    Q_INVOKABLE void openHtmlAttachmentInBrowser();
    // Tests without a web view (the offscreen platform): links show their page inside Thoughts
    // anyway, as in the app.
    static void setHtmlAttachmentsWithoutWebViewForTesting(bool inside);
    // State for tests and agents: shown, path, sha256, url, ready, transport, cards_supported,
    // selection, review_card, review_state, review_blocks, review_revealed, placement.
    Q_INVOKABLE QVariantMap htmlAttachmentState() const;

    void setModified(bool modified) override;

    /** Accessor for the underlying annotations model
     * \note: this is primarily meant for tests, if you need it elsewhere
     * please reconsider or at least update this comment.
     */
    QAbstractItemModel *annotationsModel() const;

public Q_SLOTS: // dbus
    Q_SCRIPTABLE Q_NOREPLY void goToPage(uint page) override;
    Q_SCRIPTABLE Q_NOREPLY void openDocument(const QString &doc);
    Q_SCRIPTABLE uint pages();
    Q_SCRIPTABLE uint currentPage();
    // The reader's current text selection, for "Copy for agent".
    Q_INVOKABLE QString selectedText() const;
    Q_SCRIPTABLE QString currentDocument();
    Q_SCRIPTABLE QString documentMetaData(const QString &metaData) const;
    Q_SCRIPTABLE void slotPreferences();
    Q_SCRIPTABLE void slotFind();
    Q_SCRIPTABLE void slotPrintPreview();
    Q_SCRIPTABLE void slotPreviousPage();
    Q_SCRIPTABLE void slotNextPage();
    Q_SCRIPTABLE void slotGotoFirst();
    Q_SCRIPTABLE void slotGotoLast();
    Q_SCRIPTABLE void slotTogglePresentation();
    Q_SCRIPTABLE void slotToggleChangeColors();
    Q_SCRIPTABLE void slotSetChangeColors(bool active);
    Q_SCRIPTABLE Q_NOREPLY void reload();
    Q_SCRIPTABLE Q_NOREPLY void enableStartWithPrint();
    Q_SCRIPTABLE Q_NOREPLY void enableExitAfterPrint();
    Q_SCRIPTABLE Q_NOREPLY void enableStartWithFind(const QString &text);
    // Opens the document search on a Library search's query, at its first result from the 1-based
    // @p page on, so it has results to step through and closing it clears the highlights.
    Q_SCRIPTABLE Q_NOREPLY void showLibrarySearch(const QString &text, int page);
    Q_SCRIPTABLE Q_NOREPLY void setEditorCmd(const QString &editorCmd);
    Q_SCRIPTABLE void slotOpenContainingFolder();
    Q_SCRIPTABLE void slotCopyAnnotation();
    Q_SCRIPTABLE void slotCopyTextSelectionOrAnnotation();
    Q_SCRIPTABLE void slotPasteAnnotation();

Q_SIGNALS:
    // The answer feedback shown for the card under review changed (or its visibility did).
    void reviewFeedbackChanged();
    // Reloading the PDF (Sync rebuilt it) took away the answer mask or frame of the card under
    // review, @p cardId, and its feedback: the review shows that card again (Shell, ReviewOverview).
    void reviewPresentationLost(const QString &cardId);
    // The card under review was made in an HTML attachment and its answer text is no longer in
    // the page: the review panel shows the stored answer instead of a mask.
    void reviewAnswerNotFound(const QString &cardId);
    void enablePrintAction(bool enable);
    void requestCommentsOverview();
    void requestOpenDocumentLink(const QUrl &url, const QString &destination, const QString &viewport);
    void navigationViewportChanged(bool historyEntry);
    void requestNavigationHistory(bool forward);
    void requestSyncOperations();
    void requestOpenCommentCodex(const QString &pdfPath, const QString &annotationId);
    void requestOpenCommentChatGpt(const QString &url);
    void requestOpenCommentSource(const QString &sourcePath, int line);
    void requestOpenSourceSelection(const QString &sourcePath, int line, const QString &selectedText);
    // @p textSelection: the area is the bounding rectangle of selected text, not a drawn rectangle.
    void requestManualReviewCard(const QString &pdfPath, int page, const QVariantList &rect, bool textSelection);
    // Several areas ({page, rect: [x0, y0, x1, y1]}, one-based pages) for one card.
    void requestManualReviewCardAreas(const QString &pdfPath, const QVariantList &areas, bool textSelection);
    // A card from a selection in the HTML view: @p fields are the card's HTML placement
    // (created_in, html_edition_sha256, text_quote, source_range?) plus pdf_path, pdf_stem,
    // pdf_sha256, document_id and selected_text. The server places its markers at the next Sync.
    void requestHtmlReviewCard(const QString &pdfPath, const QVariantMap &fields);
    // The page of an HTML attachment of this document, in a tab of its own next to the
    // document's (Shell::openHtmlAttachmentTab): @p request has its "path", and "feedback_id"
    // (with "edit") or "card" to show there.
    void requestHtmlAttachmentTab(const QJsonObject &request);
    // Close in a page's tab.
    void requestCloseHtmlAttachmentTab();
    // Feedback on the PDF chosen in a page's tab: shown in the document's tab (Shell::openTrackedFeedback).
    void requestOpenDocumentFeedback(const QString &pdfPath, const QString &feedbackId);
    void requestEditReviewCard(const QString &cardId);
    void requestOpenReviewCard(const QString &cardId);
    // Review now in the Cards list: the card alone in the Review window (Shell::reviewSidebarReviewCard).
    void requestReviewReviewCard(const QString &cardId);
    // Merge cards… in the Cards list: the cards of this document to merge into one (Shell::mergeSidebarReviewCards).
    void requestMergeReviewCards(const QStringList &cardIds);
    void requestOutlineDocumentActions(QMenu *menu, const QString &pdfPath, int page, const QString &title);
    void requestCommentsOverviewForComment(const QString &pdfPath, const QString &annotationId, int page);
    void unplacedCommentOpened(const QString &annotationId, int page);
    void libraryImportFinished(const QString &requestId, const QString &filename, const QString &path, const QString &sha256, bool success, int statusCode, const QString &message);
    void libraryDisplayNameFinished(const QString &requestId, const QString &filename, const QString &sha256, const QString &displayName, bool success, const QString &message);
    void libraryRemovalFinished(const QString &requestId, const QString &filename, const QString &sha256, bool success, const QString &message);
    void fedoraProjectionsChanged(const QJsonObject &projections, qint64 frontier);
    void fedoraManualSyncStateChanged(int state, const QString &message, const QString &details);
    void pendingChangesChanged(const QJsonObject &snapshot);
    void syncProgressChanged(const QString &traceId, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &message);
    void syncTransferProgressChanged(const QString &traceId, const QJsonObject &absoluteSnapshot);
    void syncJournalEvent(const QString &traceId, const QJsonObject &event);
    void globalSyncRequested();
    void fedoraRenderStatusChanged(const QString &target, const QJsonObject &status);
    void fedoraBuildSourceDiffReady(const QString &buildId, const QJsonObject &diff, const QString &error);
    // The answer to checkFedoraRemoteWork.
    void fedoraRemoteWorkChecked(bool pending, const QString &stateKey);
    // The same kind of answer, given without having been asked: a PDF build that a Sync left
    // running on the server has ended and the server holds a PDF this computer lacks. Kept apart
    // from the answers, so that whoever asked can tell its own answer from this news.
    void fedoraRemoteWorkFound(bool pending, const QString &stateKey);
    void fedoraBuildInputSnapshotChanged(const QString &target, const QJsonObject &snapshot, const QString &error);
    void fedoraRenderStateChanged(const QJsonObject &state);
    void agentProposalResolutionStatusChanged(const QString &operationId,
                                              const QStringList &proposalIds,
                                              const QString &action,
                                              const QString &status,
                                              const QString &message);
    void sourceConflictResolutionBundleReady(const QVariantMap &bundle, const QString &error);
    // Timing and outcome events for the developer log (ids and durations only).
    void developerEvent(const QString &event, const QJsonObject &metadata);
    void sourceConflictResolutionStatusChanged(const QString &operationId, const QString &status, const QString &message);
    void openSourceReference(const QString &absFileName, int line, int column); /* NOLINT(bugprone-derived-method-shadowing-base-method) cppcheck-suppress duplInheritedMember; see viewerinterface*/
    void viewerMenuStateChange(bool enabled);                                   /* NOLINT(bugprone-derived-method-shadowing-base-method) cppcheck-suppress duplInheritedMember */
    void enableCloseAction(bool enable);
    void mimeTypeChanged(const QMimeType &mimeType);
    void urlsDropped(const QList<QUrl> &urls);
    void fitWindowToPage(const QSize pageViewPortSize, const QSize pageSize);
    void maxRecentItemsChanged(const int);
    /**
     * Request to open a newly signed file
     * \param path file to open
     * \param pageNumber page to show (1-indexed)
     */
    void requestOpenNewlySignedFile(const QString &path, int pageNumber);

protected:
    // reimplemented from KParts::ReadWritePart
    bool openFile() override;
    bool openUrl(const QUrl &url) override;
    void guiActivateEvent(KParts::GUIActivateEvent *event) override;
    void displayInfoMessage(const QString &message, KMessageWidget::MessageType messageType = KMessageWidget::Information, int duration = -1);

public:
    bool queryClose() override;
    bool closeUrl() override;
    bool closeUrl(bool promptToSave) override;
    void setReadWrite(bool readwrite) override;
    bool saveAs(const QUrl &saveUrl) override;

protected Q_SLOTS:
    // connected to actions
    void openUrlFromDocument(const QUrl &url);
    void openUrlFromBookmarks(const QUrl &url);
    void handleDroppedUrls(const QList<QUrl> &urls);
    void slotGoToPage();
    void slotHistoryBack();
    void slotHistoryNext();
    void slotAddBookmark();
    void slotRenameBookmarkFromMenu();
    void slotRemoveBookmarkFromMenu();
    void slotRenameCurrentViewportBookmark();
    void slotPreviousBookmark();
    void slotNextBookmark();
    void slotFindNext();
    void slotFindPrev();
    bool slotSaveFileAs(bool showOkularArchiveAsDefaultFormat = false);
    void slotNewConfig();
    void slotShowMenu(const Okular::Page *page, const QPoint point);
    void slotShowTOCMenu(const Okular::DocumentViewport &vp, const QPoint point, const QString &title);
    void slotShowProperties();
    void slotShowEmbeddedFiles();
    void slotShowLeftPanel();
    // Shows the sidebar with item (Annotations or Contents), or hides it for nullptr.
    void chooseSidebarDisplay(QWidget *item);
    void updateSidebarDisplayActions();
    void slotShowBottomBar();
    void slotShowPresentation();
    void slotHidePresentation();

    /**
     * Updates the menu that is by default at the right end of the toolbar.
     * In true "simple by default" fashion, the menu only contains the most important actions
     * which are needed to use all essential Okular features. More advanced actions can be
     * discovered through a sub-menu (@see KConfigWidgets::KHamburgerMenu::setMenuBarAdvertised()).
     */
    void slotUpdateHamburgerMenu();
    void slotExportAs(QAction *);
    bool slotImportPSFile();
    void slotAboutBackend();
    void slotReload();
    void close();
    void cannotQuit();
    void slotShowFindBar();
    void slotHideFindBar();
    void slotJobStarted(KIO::Job *job);
    void slotJobFinished(KJob *job);
    void loadCancelled(const QString &reason);
    void setWindowTitleFromDocument();
    // can be connected to widget elements
    void updateViewActions();
    void updateBookmarksActions();
    void enableTOC(bool enable);
    void slotRebuildBookmarkMenu();
    void enableLayers(bool enable);
    void enableSidebarSignaturesItem(bool enable);

public Q_SLOTS:
    bool saveFile() override;
    // connected to Shell action (and browserExtension), not local one
    void slotPrint();
    void slotFileDirty(const QString &);
    bool slotAttemptReload(bool oneShot = false, const QUrl &newUrl = QUrl());
    void psTransformEnded(int, QProcess::ExitStatus);

    void errorMessage(const QString &message, int duration = 0);
    void warningMessage(const QString &message, int duration = -1);
    void noticeMessage(const QString &message, int duration = -1);

    void moveSplitter(const int sideWidgetSize);
#if HAVE_NEW_SIGNATURE_API
    void finishSigning();
#endif

private:
    friend class PartTest;
    // PDF link overrides (linkoverridestore.h): a link of this PDF opens a chosen Library document.
    struct PdfLinkContext {
        QString sourceDocumentId;
        QString sourcePdfSha256;
        Thoughts::PdfLinkOverrides::Link link;
    };
    // The overridable link behind @p action (on @p page), or nothing for links that
    // cannot be overridden (other files, Thoughts' own links, PDFs outside the Library).
    std::optional<PdfLinkContext> pdfLinkContext(const Okular::Action *action, int page) const;
    QJsonObject pdfLinkOverride(const PdfLinkContext &context) const;
    // Opens the Library document chosen for the link; false when the original link should be followed.
    bool openPdfLinkOverride(const Okular::Action *action, int page);
    void addPdfLinkOverrideActions(QMenu *menu, const Okular::Action *action, int page);
    void choosePdfLinkOverrideTarget(const PdfLinkContext &context);
    void removePdfLinkOverride(const PdfLinkContext &context);
    bool queuePdfLinkOverride(const QJsonObject &record);
    bool aboutToShowContextMenu(QMenu *menu, QAction *action, QMenu *contextMenu);
    // The sidebar panels and the Overview and Review windows, at the end of a right-click menu, so
    // they are one click away from the pointer.
    void addReadingActions(QMenu *menu);
    void showMenu(const Okular::Page *page, const QPoint point, const QString &bookmarkTitle = QString(), const Okular::DocumentViewport &vp = DocumentViewport(), bool showTOCActions = false);
    KConfigDialog *realSlotPreferences();
    /**
     * Searches the actionCollections of all KXMLGUIClients that were created by the same factory()
     * as this Part for a QAction that has both the specified name and the specified class.
     * @return an action with class @p Action and name @p actionName. nullptr if no such action is found.
     */
    template<class Action = QAction> Action *findActionInKPartHierarchy(const QString &actionName);
    /** @return the first KMainWindow among the ancestors of this part. nullptr if no KMainWindow is found. */
    KMainWindow *findMainWindow();
    bool eventFilter(QObject *watched, QEvent *event) override;
    static bool isDocumentArchiveFile(const QMimeType &mime, const QString &fileName);
    Document::OpenResult doOpenFile(const QMimeType &mime, const QString &fileNameToOpen, bool *isCompressedFile);
    bool openUrl(const QUrl &url, bool swapInsteadOfOpening);

    void setupViewerActions();
    void setViewerShortcuts();
    void setupActions();

    void setupPrint(QPrinter &printer);
    bool doPrint(QPrinter &printer);
    bool handleCompressed(QString &destpath, const QString &path, KCompressionDevice::CompressionType compressionType);
    void rebuildBookmarkMenu(bool unplugActions = true);
    void updateAboutBackendAction();
    void unsetDummyMode();
    void slotRenameBookmark(const DocumentViewport &viewport);
    void slotRemoveBookmark(const DocumentViewport &viewport);
    void resetStartArguments();
    void checkNativeSaveDataLoss(bool *out_wontSaveForms, bool *out_wontSaveAnnotations) const;
    bool exportPdfWithComments(const QUrl &destination);
    void makeExistingAnnotationsReadOnly();

    enum SaveAsFlag {
        NoSaveAsFlags = 0,      ///< No options
        SaveAsOkularArchive = 1 ///< Save as Okular Archive (.okular) instead of document's native format
    };
    Q_DECLARE_FLAGS(SaveAsFlags, SaveAsFlag)

    bool saveAs(const QUrl &saveUrl, SaveAsFlags flags);

    void setFileToWatch(const QString &filePath);
    void unsetFileToWatch();
    void recordCurrentFileRevision(bool ignoreMatchingDirtySignals = false);
    bool currentFileMatchesRecordedRevision() const;

#if HAVE_PURPOSE
    void slotShareActionFinished(const QJsonObject &output, int error, const QString &message);
#endif

    bool tryOpeningUrlWithFragmentAsName();
    bool shouldSyncDocsComments(const QUrl &documentUrl) const;
    bool isWorkspaceThirdPartyPdf(const QUrl &documentUrl) const;
    QUrl normalizedDocsLibraryUrl(const QUrl &documentUrl) const;
    int normalizeLegacyCrossPageComments();
    bool writeDocsCommentSnapshot(const QUrl &documentUrl, QJsonObject *snapshot, QString *failureReason, const QString &onlyLogicalId = QString()) const;
    bool swapSavedDocumentSilently(const QUrl &saveUrl);
    bool syncDocsComments(const QUrl &documentUrl, QString *failureReason = nullptr, const QJsonObject *commentsSnapshot = nullptr, const QString &saveTiming = QString(), qint64 elapsedBeforeSyncMs = -1);
    void handleDocsAnnotationMutation(int page, Okular::Annotation *annotation, Okular::Document::AnnotationMutation mutation);
    void discardTransientEmptyDocsComment(Okular::Annotation *annotation, int page);
    void capturePendingDocsCommentMutations();
    bool queueDocsCommentUpsert(const QString &annotationId, const QJsonObject &metadataPatch, bool markHistoryClean, QString *failureReason = nullptr);
    bool queueDocsCommentDeletions(const QStringList &annotationIds, bool markHistoryClean, QString *failureReason = nullptr);
    bool queueDocsCommentRestores(const QList<QJsonObject> &records, bool markHistoryClean, QString *failureReason = nullptr);
    bool queueDocsCommentMutations(const QList<QJsonObject> &mutations, bool markHistoryClean, QString *failureReason = nullptr);
    QJsonObject currentDocsCommentRecord(const QString &annotationId) const;
    QJsonObject currentCommentDetailsRecord(const QString &annotationId) const;
    void scheduleDocsCommentMutationFlush(int delayMs = 1000);
    void startDocsCommentMutationFlush();
    void finishDocsCommentMutationFlush();
    void startDocsPdfIdentityVerification(const QUrl &documentUrl, const QString &openedPath = QString());
    void finishDocsPdfIdentityVerification();
    void configureDocsCommentWatcher(const QUrl &documentUrl);
    void clearDocsCommentWatcher();
    void scheduleDocsCommentRefresh();
    void startDocsCommentLoad();
    void refreshDocsCommentsFromSidecar();
    void finishDocsCommentLoad();
    void configureDocsSourceWatcher(const QUrl &documentUrl, bool reconcileOnOpen = false);
    void applyDocsSourceWatchPaths(const QUrl &documentUrl, const QStringList &sourceFiles, bool replaceExisting, bool reconcileOnOpen = false, bool refreshFingerprints = false);
    void clearDocsSourceWatcher();
    void scheduleDocsSourceReconciliation(bool force = false);
    void reconcileDocsSourceChanges();
    void finishDocsSourceReconciliation();
    void focusSidebarAgentProposal(const QString &proposalId, const QString &sourcePath, int sourceLine, int attempt = 0);
    void requestAgentProposalReview(const QString &proposalId, int attempt = 0);
    void locateOpenAgentProposal();
    void startAgentProposalTextSearch(const QString &proposalId,
                                      const QString &proposalText,
                                      const QString &fallbackText,
                                      bool targetsCurrentText,
                                      const QString &sourcePath,
                                      int sourceLine);
    void finishAgentProposalTextSearch(Okular::Document::SearchStatus status);
    void resolveAgentProposalFromCard(const QString &proposalId, const QString &action);
    // Several at once: one decision for each source file, so one write and one Undo.
    void resolveAgentProposalsFromSidebar(const QStringList &proposalIds, const QString &action);
    // Whether the page's hover toolbar may accept or reject a proposal now (the
    // proposal window's rule), and why not.
    AgentProposalDecisionState agentProposalDecisionState(const QString &proposalId) const;
    void finishLiveAgentProposalDecision(const Thoughts::LiveProposalDecisionResult &result);
    void handleAgentProposalResolutionStatus(const QString &operationId, const QString &status, const QString &message);
    void undoLastAgentProposalResolution();
    void showAgentProposalStatus(const QString &message, KMessageWidget::MessageType messageType, bool undoEnabled);
    void hideAgentProposalStatus(bool undoEnabled);
    void setAgentProposalUndoEnabled(bool enabled);
    void updateUndoAction();
    void restoreAgentProposalSurfaces(const QStringList &proposalIds, bool reloadSources = true);

    /**
     * Initializes Okular::Settings.
     * Determines the config file path, and performs configuration updates not handled by kconf_update.
     *
     * @param args As passed to Part::Part().
     */
    void setupConfigSkeleton(const QVariantList &args);

    static int numberOfParts;

    QTemporaryFile *m_tempfile;

    // the document
    Okular::Document *m_document;
    bool m_shellNavigationHistory = false;
    bool m_shellCanGoBack = false;
    bool m_shellCanGoForward = false;
    QDateTime m_fileLastModified;
    QString m_temporaryLocalFile;
    bool isDocumentArchive;
    bool m_documentOpenWithPassword;
    bool m_swapInsteadOfOpening; // if set, the next open operation will replace the backing file (used when reloading just saved files)

    // main widgets
    Sidebar *m_sidebar;
    FindBar *m_findBar;
    bool m_sidebarWasVisibleBeforeSearch = true;
    KMessageWidget *m_migrationMessage;
    KMessageWidget *m_topMessage;
    KMessageWidget *m_formsMessage;
    KMessageWidget *m_infoMessage;
    KMessageWidget *m_agentProposalMessage;
    QAction *m_undoAction = nullptr;
    bool m_agentProposalUndoEnabled = false;
    KMessageWidget *m_signatureMessage;
#if HAVE_NEW_SIGNATURE_API
    KMessageWidget *m_signatureInProgressMessage;
#endif
    QPointer<ThumbnailList> m_thumbnailList;
    QPointer<PageView> m_pageView;
    QSplitter *m_pageSplitter = nullptr;
    // The HTML view beside the page view (part/parthtml.cpp).
    QStackedWidget *m_documentStack = nullptr;
    Thoughts::HtmlDocumentView *m_htmlView = nullptr;
    Thoughts::HtmlReader *m_htmlReader = nullptr;
    std::unique_ptr<Thoughts::HtmlEdition::Store> m_htmlStore;
    KToggleAction *m_htmlViewAction = nullptr;
    QJsonObject m_reviewCardRecord;
    QVariantList m_reviewCardRegions;
    bool m_htmlReviewShown = false;
    QPointer<Thoughts::ReadAloud::Controller> m_htmlReadAloud;
    QPointer<ReadAloudBar> m_htmlReadAloudBar;
    void setupHtmlView(QWidget *parent);
    void updateHtmlViewAction();
    void htmlViewActiveChanged(bool active);
    void openHtmlForCurrentDocument();
    void showHtmlContextMenu(const QJsonObject &message);
    void openHtmlLink(const QJsonObject &message);
    void htmlItemTapped(const QJsonObject &message);
    void readAloudHtmlText(const QString &text);
    // Keys the HTML page hands back (viewer forwardKeys): review Skip (S), Escape, Read Aloud
    // (an HTML attachment: S and Escape, when the page does not use them itself).
    QStringList htmlForwardedKeys() const;
    // The same for an HTML attachment: Skip and Escape only.
    QStringList attachmentForwardedKeys() const;
    void handleHtmlKey(const QJsonObject &message);
    void createHtmlComment(const QJsonObject &selection);
    void requestHtmlCard(const QJsonObject &selection);
    void giveHtmlFeedback(const QString &polarity, const QJsonObject &selection, bool withComment);
    void refreshHtmlComments();
    void refreshHtmlFeedback();
    // Document feedback (document-feedback.md) of the PDF being read: its Library document ID and
    // PDF version, and its records from the projections. Nothing for a PDF without a known
    // document ID or for a project reading.
    void refreshDocumentFeedback();
    // Opens Annotations on what was just added, unless a search or a review is using the screen.
    bool showAnnotationsForNewEntry();
    void showHtmlNotShownMenu();
    QJsonObject htmlSelectionFields(const QJsonObject &selection) const;
    bool presentHtmlReview(const QVariantList &regions);
    void mapPdfPositionToHtml();
    void mapHtmlPositionToPdf();
    // HTML attachments (part/partattachment.cpp).
    Thoughts::HtmlDocumentView *m_attachmentView = nullptr;
    Thoughts::HtmlAttachmentReader *m_attachmentReader = nullptr;
    bool m_attachmentShown = false;
    bool m_attachmentForReview = false;
    // This Part is the tab of the page m_attachmentTabPath (openHtmlAttachmentTab).
    bool m_attachmentTab = false;
    QString m_attachmentTabPath;
    QString m_attachmentTitle;
    // The page @p path shows in a tab of its own (the app) rather than in place (a Part alone, or
    // a page's tab for its own page).
    bool wantsHtmlAttachmentTab(const QString &path) const;
    bool m_attachmentReviewShown = false;
    bool m_attachmentCardsSupported = false;
    QJsonArray m_sidebarCardRecords;
    void setupHtmlAttachmentView();
    bool showHtmlAttachment(const Thoughts::HtmlAttachmentTarget &target, bool forReview, QString *error);
    void hideHtmlAttachment();
    void updateAttachmentSelectionAction();
    void requestAttachmentCard();
    void openAttachmentInBrowser();
    // A link of an attachment page that leaves it: opened only on the reader's own click.
    void openAttachmentLink(const QUrl &url, bool userGesture);
    bool m_attachmentLinkQuestion = false;
    bool presentAttachmentReview();
    void closeHtmlAttachment();
    // Like, Dislike and Comment… on the page's selection (document-feedback.md 8).
    void addAttachmentMenuActions(QMenu *menu, const QJsonObject &message);
    void showAttachmentContextMenu(const QJsonObject &message);
    QPoint attachmentClick(const QJsonObject &message) const;
    void giveAttachmentFeedback(const QString &polarity, bool withComment, const QString &kind = QString());
    void saveAttachmentFeedback(const QString &polarity, const QJsonObject &selection, bool withComment, const QString &kind);
    // The current SHA-256 of an HTML attachment (the version rule of its feedback), kept until the document changes.
    QString attachmentVersion(const QString &path);
    QHash<QString, QString> m_attachmentVersions;
    void showDocumentFeedbackList();
    // A feedback row of Annotations: the passage on the PDF or in its HTML attachment, and its
    // editor with @p edit.
    void showFeedback(const QString &feedbackId, bool edit);
    std::function<bool(QString *error)> m_attachmentFeedbackUndo;
    QTimer *m_attachmentFeedbackUndoTimer = nullptr;
    QPointer<TOC> m_toc;
    bool m_tocEnabled;
    QPointer<MiniBarLogic> m_miniBarLogic;
    QPointer<MiniBar> m_miniBar;
    QPointer<MiniBar> m_pageNumberTool;
    QPointer<QWidget> m_bottomBar;
    QPointer<PresentationWidget> m_presentationWidget;
    QPointer<PageSizeLabel> m_pageSizeLabel;
    QPointer<Reviews> m_reviewsWidget;
    struct PendingAgentProposalResolution {
        QStringList proposalIds;
        QString action;
        bool undo = false;
    };
    QHash<QString, PendingAgentProposalResolution> m_pendingAgentProposalResolutions;
    struct AgentProposalUndoState {
        bool available = false;
        bool remote = false;
        QString resolutionId;
        QStringList proposalIds;
        QString action;
        QString sourcePath;
        QString expectedSourceSha256;
        QByteArray previousContents;
        QString target;
        // The other half of a move pair decided with it, in another file (§12).
        QList<Thoughts::AgentProposalSourceChange> otherSources;
        // When it was decided, for ⌘Z against the user's newer annotations.
        qint64 at = QDateTime::currentMSecsSinceEpoch();
    };
    AgentProposalUndoState m_agentProposalUndoState;
    // Accept/Reject of proposals in local sources: the window closes at once,
    // the source write runs on this queue (one at a time, in click order).
    Thoughts::LiveProposalDecisionQueue *m_liveProposalDecisions = nullptr;
    void queueLiveAgentProposalDecision(const QStringList &proposalIds, const QString &sourcePath, const QString &sourceSha256, const QString &action);
    struct PendingLiveProposalDecision {
        QStringList proposalIds;
        QString action;
        QElapsedTimer clock;
        qint64 hiddenNs = 0;
        QString target;
    };
    QHash<quint64, PendingLiveProposalDecision> m_pendingLiveProposalDecisions;
    QPointer<BookmarkList> m_bookmarkList;
    QPointer<Layers> m_layers;
    QPointer<SignaturePanel> m_signaturePanel;

    // document watcher (and reloader) variables
    KDirWatch *m_watcher;
    QString m_watchedFilePath, m_watchedFileSymlinkTarget;
    QTimer *m_dirtyHandler;
    bool m_syncUiDeferred = false;
    bool m_syncPdfReloadPending = false;
    QUrl m_syncPdfReloadSource;
    QUrl m_syncPdfReloadTarget;
    bool m_libraryActivationWatchSuspended = false;
    QString m_libraryActivationPdfFilename;
    QString m_libraryActivationDocumentId;
    QString m_libraryActivationPdfSha256;
    QTimer *m_docsCommentRefreshTimer = nullptr;
    QTimer *m_docsSourceReconcileTimer = nullptr;
    bool m_docsCompanionActive = false;
    QPointer<Thoughts::DocsChangeMonitor> m_docsChangeMonitor;
    QTimer *m_docsCommentMutationCaptureTimer = nullptr;
    QTimer *m_docsCommentMutationFlushTimer = nullptr;
    QFutureWatcher<QJsonObject> *m_docsCommentLoadWatcher = nullptr;
    QFutureWatcher<QJsonObject> *m_docsPdfIdentityWatcher = nullptr;
    QFutureWatcher<DocsCommentMutationFlushResult> *m_docsCommentMutationFlushWatcher = nullptr;
    QFutureWatcher<QJsonObject> *m_docsSourceReconcileWatcher = nullptr;
    QString m_docsCommentPdfPath;
    QString m_docsSourcePdfPath;
    QByteArray m_docsCommentFingerprint;
    QByteArray m_docsSourceReconciledCommentFingerprint;
    QHash<QString, QByteArray> m_docsAppliedCommentFingerprints;
    QHash<QString, QJsonObject> m_docsCommentRecords;
    QHash<QString, QPair<int, Okular::Annotation *>> m_docsProjectedAnnotations;
    QHash<QString, QByteArray> m_docsSourceFingerprints;
    QJsonObject m_docsPreparedCommentLoad;
    QJsonObject m_fedoraProjections;
    bool m_hasFedoraProjectionSnapshot = false;
    bool m_applyingDocsSidecar = false;
    bool m_suppressDocsCommentMutationCapture = false;
    bool m_docsUseFedoraForNextCommentRefresh = false;
    bool m_docsCommentRefreshQueued = false;
    bool m_docsSourceReconcileForce = false;
    bool m_docsSourceReconcileQueued = false;
    bool m_docsSourceReconcileRunning = false;
    QUrl m_docsSourceReconcileDocumentUrl;
    qint64 m_fileLastSize;
    bool m_ignoreDirtySignalsForCurrentRevision;
    QUrl m_oldUrl;
    Okular::DocumentViewport m_viewportDirty;
    bool m_isReloading;
    // The card under review whose presentation a reload (still) has to give back.
    QString m_reloadLostReviewCardId;
    bool m_wasPresentationOpen;
    QWidget *m_dirtyToolboxItem;
    bool m_wasSidebarVisible;
    bool m_fileWasRemoved;
    Rotation m_dirtyPageRotation;
    QString m_docsRenderRoot;
    FedoraRepository *m_fedoraRepository = nullptr;
    bool m_manualSyncRequestedByPart = false;
    QString m_docsSyncTexExecutable;
    QString m_docsCommentSyncLogPath;
    QString m_docsSourceSyncLogPath;
    QString m_lastDocsCommentSyncError;
    QSizeF m_docsDocumentDpi;
    int m_docsSyncTexTimeoutMs;
    bool m_syncingDocsComments;
    bool m_skipDocsCommentSyncForSave = false;
    bool m_lastDocsCommentSyncSucceeded = true;
    std::unique_ptr<DocsCommentMutationStore> m_docsCommentMutationStore;
    QSet<QString> m_pendingDocsCommentMutationIds;
    QSet<QString> m_docsNewCommentIds;
    QHash<QString, bool> m_transientEmptyDocsCommentIds;
    int m_pendingDocsCommentPreHistoryIndex = -1;
    bool m_pendingDocsCommentWasClean = false;
    QString m_docsOpenedPdfPath;
    QString m_docsOpenedPdfSha256;
    bool m_docsOpenedPdfIdentityVerified = false;
    QPointer<QWidget> m_unplacedCommentPanel;
    bool openUnplacedComment(const QString &annotationId, int expectedPage);
    QList<QJsonObject> unplacedCommentThread(const QString &annotationId) const;
    QJsonObject ensureLogicalComment(const QString &annotationId);
    bool saveUnplacedComment(const QString &annotationId, const QJsonObject &patch);
    QString m_pendingReviewAnnotationId;
    int m_pendingReviewAnnotationPage = 0;
    QString m_pendingAgentProposalReviewId;
    QString m_currentAgentProposalReviewId;
    QString m_locatedAgentProposalReviewId;
    QString m_currentAgentProposalText;
    QString m_currentAgentProposalFallbackText;
    QString m_currentAgentProposalSourcePath;
    int m_currentAgentProposalSourceLine = 0;
    bool m_currentAgentProposalTargetsCurrentText = false;
    QString m_agentProposalTextSearchId;
    QString m_agentProposalTextSearchText;
    QString m_agentProposalTextSearchFallbackText;
    QString m_agentProposalTextSearchSourcePath;
    int m_agentProposalTextSearchSourceLine = 0;
    bool m_agentProposalTextSearchTargetsCurrentText = false;
    QString m_pendingAgentProposalTextSearchId;
    QString m_pendingAgentProposalTextSearchText;
    QString m_pendingAgentProposalTextSearchFallbackText;
    QString m_pendingAgentProposalTextSearchSourcePath;
    int m_pendingAgentProposalTextSearchSourceLine = 0;
    bool m_pendingAgentProposalTextSearchTargetsCurrentText = false;
    bool m_agentProposalTextSearchRunning = false;
    int m_docsCommentMutationRetryAttempt = 0;
    struct ReviewMetadataHistoryEntry {
        QString annotationId;
        QString pdfStem;
        QString previousContents;
        QString currentContents;
        QJsonObject previousMetadata;
        QJsonObject currentMetadata;
    };
    struct ReviewSidecarDeletionHistoryEntry {
        QString pdfStem;
        QList<QJsonObject> previousRecords;
        int documentUndoIndex = 0;
    };
    QList<int> m_reviewUndoIndices;
    QList<int> m_reviewRedoIndices;
    QHash<int, ReviewMetadataHistoryEntry> m_reviewMetadataHistory;
    QHash<int, ReviewSidecarDeletionHistoryEntry> m_reviewSidecarDeletionHistory;
    int m_nextReviewSidecarHistoryToken = -1;
    std::unique_ptr<Thoughts::CommentService> m_commentService;
    QList<int> m_commentUndoTokens, m_commentRedoTokens;
    QHash<int, int> m_commentHistoryDocumentIndex;
    bool m_applyingCommentStorage = false;
    QHash<QString, Thoughts::CommentRecord> m_legacyCommentStates;
    QList<Thoughts::CommentRecord> commentRecords();
    bool commitCommentChanges(const QList<Thoughts::CommentChange> &changes, QString *error);
    QVariantMap deleteLegacyReviewAnnotations(const QVariantList &targets);
    Thoughts::CommentService &commentService();


    // Remember the search history
    QStringList m_searchHistory;

    // actions
    QAction *m_gotoPage;
    QAction *m_prevPage;
    QAction *m_nextPage;
    QAction *m_beginningOfDocument;
    QAction *m_endOfDocument;
    QAction *m_historyBack;
    QAction *m_historyNext;
    QAction *m_addBookmark;
    QAction *m_renameBookmark;
    QAction *m_prevBookmark;
    QAction *m_nextBookmark;
    QAction *m_copy;
    QAction *m_copyWithoutLineBreaks;
    QAction *m_selectAll;
    QAction *m_selectCurrentPage;
    QAction *m_find;
    QAction *m_findNext;
    QAction *m_findPrev;
    QAction *m_printPreview;
    QAction *m_showProperties;
    QAction *m_showEmbeddedFiles;
    QAction *m_exportAs;
    QAction *m_exportAsText;
    QAction *m_exportAsPdfWithComments;
#if HAVE_PURPOSE
    QAction *m_share;
#endif
    QAction *m_showPresentation;
    QAction *m_openContainingFolder;
    KHamburgerMenu *m_hamburgerMenuAction;
    KToggleAction *m_showMenuBarAction;
    KToggleAction *m_showLeftPanel;
    // Toolbar "Sidebar" chooser: Hide sidebar / Annotations / Contents / History / Agent Notes. It
    // replaces the sidebar's tab strip and always mirrors show_leftpanel and
    // the sidebar's current panel.
    KActionMenu *m_sidebarDisplay = nullptr;
    QAction *m_sidebarHideAction = nullptr;
    QAction *m_sidebarAnnotationsAction = nullptr;
    QAction *m_sidebarContentsAction = nullptr;
    QAction *m_sidebarHistoryAction = nullptr;
    QPointer<QWidget> m_historyPanel;
    QAction *m_sidebarAgentNotesAction = nullptr;
    QPointer<QWidget> m_agentNotesPanel;
    KToggleAction *m_showBottomBar;
    KToggleFullScreenAction *m_showFullScreenAction;
    QAction *m_aboutBackend;
    QAction *m_reload;
    QMenu *m_exportAsMenu;
#if HAVE_PURPOSE
    Purpose::Menu *m_shareMenu;
#endif
    QAction *m_closeFindBar;
    DrawingToolActions *m_presentationDrawingActions;

    BrowserExtension *m_bExtension;

    QList<Okular::ExportFormat> m_exportFormats;
    QList<QAction *> m_bookmarkActions;
    bool m_cliPresentation;
    bool m_cliPrint;
    bool m_cliPrintAndExit;
    QString m_addBookmarkText;
    QIcon m_addBookmarkIcon;

    EmbedMode m_embedMode;

    QUrl m_realUrl;

    KXMLGUIClient *m_generatorGuiClient;
    FileKeeper *m_keeper;

    // Timer for m_infoMessage
    QTimer *m_infoTimer;

    QString m_registerDbusName;

    // String to search in document startup
    QString m_textToFindOnOpen;

    // Set when opening an url that had fragment so that if it fails opening we try adding the fragment to the filename
    // if we're opening http://localhost/foo#bar.pdf and the filename contains an # we can open it after trying to open foo fails
    QUrl m_urlWithFragment;

    // Current value of maxRecentItems to detect value change, and inform shell
    int m_maxRecentItems;

private Q_SLOTS:
    void handleDocsPathsChanged(const QStringList &paths);
    void followDocsChanges();
    void slotAccessibilityPreferences();
    void slotAnnotationPreferences();
    void openTexSource(int pageNumber, double normalizedX, double normalizedY, const QString &selectedText);
    DocsCommentSynchronizer::SourceResolutionResult resolveTexSource(int pageNumber, double normalizedX, double normalizedY, const QString &selectedText);
    void showTOCMenu(const Okular::DocumentViewport &viewport, const QPoint &point, const QString &title);
    void showTOCInformation(const Okular::DocumentViewport &viewport, const QString &title);
    void showSourceLocationFallback(const QString &sourcePath, bool generated);
    void slotHandleActivatedSourceReference(const QString &absFileName, int line, int col, bool *handled);
};

}

#endif

/* kate: replace-tabs on; indent-width 4; */
