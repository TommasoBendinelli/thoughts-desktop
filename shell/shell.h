/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Benjamin Meyer <benjamin@csh.rit.edu>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003 Luboš Luňák <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2004 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2004 Albert Astals Cid <aacid@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _OKULAR_SHELL_H_
#define _OKULAR_SHELL_H_

#include "config-okular.h"
#include <QAction>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPoint>
#include <QPointer>

#include <optional>
#include <QRect>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <kparts/mainwindow.h>
#include <kparts/readwritepart.h>
#include <functional>
#include <memory>
namespace Thoughts { class ReaderDisplayTitleStore; class SyncStateWatch; }

#if HAVE_DBUS
#include <QDBusAbstractAdaptor> // for Q_NOREPLY
#else
#define Q_NOREPLY
#endif
#include <QStackedWidget>

#include "appsignin.h"
#include "documentsyncpresentation.h"
#include "welcomescreen.h"

class Sidebar;
class ActivePartToolbarController;
class TabDropOverlay;
class QAction;
class KRecentFilesAction;
class KToggleAction;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QDomDocument;
class QDialog;
class QLabel;
class QMenu;
class QMimeData;
class QPixmap;
class QProcess;
class QProgressDialog;
class QTabWidget;
class QTimer;
class QToolButton;
class KActionCollection;
class KPluginFactory;

namespace Thoughts
{
class AgentNotesView;
class AgentNotesWindow;
namespace AgentNotes
{
struct Filter;
struct Recipient;
struct DocumentSources;
struct ProposalAuthorship;
}
class BuildTerminalLauncher;
class CommentsOverview;
class ReviewFeedbackPanel;
class ReviewOverview;
struct ReviewCard;
struct TrackedCommentTarget;
}
namespace Thoughts::PdfVersions
{
class Fetcher;
}

/**
 * This is the application "Shell".  It has a menubar and a toolbar
 * but relies on the "Part" to do all the real work.
 *
 * @short Application Shell
 * @author Wilco Greven <greven@kde.org>
 * @version 0.1
 */
class Shell : public KParts::MainWindow
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.okular")

    friend class MainShellTest;
    friend class AnnotationToolBarTest;
    friend class ThoughtsSyncCommandService;

public:
    /**
     * Constructor
     */
    explicit Shell(const QString &serializedOptions = QString());

    /**
     * Default Destructor
     */
    ~Shell() override;

    QSize sizeHint() const override;

    /**
     * Returns false if Okular component wasn't found
     **/
    bool isValid() const;

    bool openDocument(const QUrl &url, const QString &serializedOptions);

    static void showCommentsOverviewOnStartup();
    // The windows, tabs and pages open now, for the next start (see reopenLastSession).
    static void saveOpenSession();
    static void scheduleOpenSessionSave();
    // At a start without documents to open: the windows, tabs and pages open when Thoughts last
    // closed (quit, update or crash), when Configure Thoughts says so. True when one opened.
    static bool reopenLastSession();
    // At start, when no device token is stored for the configured server: Sign In over the
    // Library instead of staying unconnected. Not under THOUGHTS_TOKEN, in tests, or where the
    // system has no secret store for the tokens.
    static void offerSignInOnStartup();
    // Sign In over the Library, filled from a thoughts://connect link when one is given.
    static void showSignIn(const std::optional<Thoughts::AppSignIn::ConnectLink> &link = std::nullopt);
    // Shows a window when Thoughts is reactivated (for example from the Dock) with none visible.
    static void showWindowOnReactivation();
    // Brings the Library forward on its Review Card tab with this card selected.
    static void showReviewCardInLibrary(const QString &cardId);
    // A page link (thoughts://open/document/<id>?page=N&first=F&version=<sha256>, see
    // thoughtspagelink.h): the Library PDF at that page, with a note when the page was not
    // there or the PDF changed since the link was made; a message when it cannot be opened.
    // This computer's server's "Open in Thoughts" page of a document
    // (https://<server>/open/document/<id>?..., Thoughts::serverPageLink) opens the same way.
    // A link to a reading overlay (thoughts://open/document?document_id=<id>&overlay=<overlay_id>,
    // Thoughts::parseOverlayLink) opens the document with the Overlay tab on that overlay, shown.
    static void openPageLink(const QUrl &url);
    // File › Agent Notes: the notes agents sent to the phone, with this note selected if given.
    static void showAgentNotes(const QString &noteId = QString());
    // Agent Notes in front, showing only @p filter's notes (empty: all), @p noteId selected.
    static void showAgentNotes(const QString &noteId, const Thoughts::AgentNotes::Filter &filter);
    // open-agent-notes (thoughtsctl, the Thoughts tool, thoughts://agent-notes): {"document" (a
    // document_id, Library filename, PDF path or document name) | "project" (id or name),
    // "note_id"}: the window in front with those notes.
    static QJsonObject openAgentNotesCommand(const QJsonObject &request);
    // On macOS menus carry no icons; toolbars and buttons keep theirs. Call
    // once the application object exists.
    static void applyMenuIconPolicy();

public Q_SLOTS:
    Q_SCRIPTABLE Q_NOREPLY void tryRaise(const QString &startupId);
    Q_SCRIPTABLE bool openDocument(const QString &urlString, const QString &serializedOptions = QString());
    Q_SCRIPTABLE bool canOpenDocs(int numDocs, int desktop);

protected:
    /**
     * This method is called when it is time for the app to save its
     * properties for session management purposes.
     */
    void saveProperties(KConfigGroup &) override;

    /**
     * This method is called when this app is restored.  The KConfig
     * object points to the session management config file that was saved
     * with @ref saveProperties
     */
    void readProperties(const KConfigGroup &) override;

    void readSettings();
    void writeSettings();
    void setFullScreen(bool);

    using KParts::MainWindow::setCaption;
    void setCaption(const QString &caption) override;

    bool queryClose() override;
    void saveNewToolbarConfig() override;

    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private Q_SLOTS:
    // A page tab's Part answers requestHtmlAttachmentPicture (a null picture: none could be taken).
    void htmlAttachmentPictureReady(const QString &requestId, const QImage &picture);
    // The answer of the gateway part's checkFedoraRemoteWork (see startRemoteWorkWatch).
    void remoteWorkChecked(bool pending, const QString &stateKey);
    // A part's fedoraRemoteWorkFound: a PDF build that a Sync left running on the
    // server has ended. Every part of every window says so; one Sync starts.
    void remoteWorkAnnounced(bool pending, const QString &stateKey);
    void fileOpen();
    void openFromUrl();

    void slotUpdateFullScreen();
    void slotShowMenubar();

    void openUrl(const QUrl &url, const QString &serializedOptions = QString());
    void showOpenRecentMenu();
    void closeUrl();
    void print();
    void setPrintEnabled(bool enabled);
    void setCloseEnabled(bool enabled);
    void setTabIcon(const QMimeType &mimeType);
    void handleDroppedUrls(const QList<QUrl> &urls);
    void triggerUpdateRecentItems(const int maxItems);

    /**
     * Opens of a newly signed file
     *
     * This is separated out from the "normal" open url
     * to be allowed to have different rules for this
     * (rules could be open in tab, open in new window, replace
     * current document)
     *
     * \param filePath path to signed file
     * \param pageNumber page to show (1-indexed)
     */
    void openNewlySignedFile(const QString &path, int pageNumber);
    void openDocumentLink(const QUrl &url, const QString &destination, const QString &viewport);
    // A reader's link to this computer's server's "Open in Thoughts" page (Part::requestOpenPageLink).
    void openReaderPageLink(const QUrl &url);
    // A reader's link to a place in a Library PDF (Part::requestOpenPdfPlaceLink).
    void openReaderPdfPlaceLink(const QString &href);
    // Ask About This in a reading overlay mark's popup (Part::requestAskAboutReadingOverlayMark):
    // the mark's page as Send Page to Agent draws it, the mark's lines washed and outlined in its
    // kind's colour, titled and worded for the agent that made the overlay, in Send Page's composer.
    void askAboutReadingOverlayMark(const QJsonObject &request);
    // A reading overlay mark's note (Part::requestReadingOverlayMarkNote): the agent note fetched by
    // its id (no list has it) and its picture, given back to the Part (null when not had).
    void loadReadingOverlayMarkNote(const QString &noteId);
    // A click on that picture (Part::requestOpenAgentNote): the note in the Agent Notes window,
    // fetched first when this Mac does not have it yet.
    void openReadingOverlayMarkNote(const QString &noteId);
    // A Part started or stopped keeping an earlier version (Part::keptVersionChanged): the tabs'
    // marks, with their titles.
    void refreshTabVersionMarks();
    void recordNavigationViewport(bool historyEntry);
    void navigateHistory(bool forward);

    // Tab event handlers
    void setActiveTab(int tab);
    void closeTab(int tab);
    void activateNextTab();
    void activatePrevTab();
    void undoCloseTab();
    void moveTabData(int from, int to);

    void slotFitWindowToPage(const QSize pageViewSize, const QSize pageSize);

    void hideWelcomeScreen();
    void showWelcomeScreen();
    void refreshRecentsOnWelcomeScreen();
    void showCommentsOverview();
    void openCommentCodex(const QString &pdfPath, const QString &annotationId);
    void openCommentChatGpt(const QString &url);
    void openCommentSource(const QString &sourcePath, int line);
    void openSourceSelection(const QString &sourcePath, int line, const QString &selectedText);
    void openSourceComparison(const QString &sourcePath, int line, const QString &selectedText, const QString &originalPath = {}, const QString &failedSha256 = {});
    void showBuildSourceDiff(const QString &buildId, const QJsonObject &diff, const QString &error);
    void recordUnplacedCommentOpened(const QString &id, int page);
    void recordPartDeveloperEvent(const QString &event, const QJsonObject &metadata);
    void showCommentInOverview(const QString &pdfPath, const QString &annotationId, int page);
    void showReviewOverview();
    void createManualReviewCard(const QString &pdfPath, int page, const QVariantList &rect, bool textSelection = false);
    // One card with several answer areas ({page, rect}), e.g. a text selection over a page break.
    void createManualReviewCardAreas(const QString &pdfPath, const QVariantList &areas, bool textSelection = false);
    // Add Review Card for This Section… on a heading line (@p rect, normalized, on one-based @p page).
    void createSectionReviewCard(const QString &pdfPath, int page, const QVariantList &rect);
    void createHtmlReviewCard(const QString &pdfPath, const QVariantMap &fields);
    void editSidebarReviewCard(const QString &cardId);
    void coverSidebarReviewCardDocument(const QString &cardId);
    void openSidebarReviewCard(const QString &cardId);
    // Review now in a reader's Cards list: that card alone in the Review window.
    void reviewSidebarReviewCard(const QString &cardId);
    // Merge cards… in a reader's Cards list: those cards of its document become one.
    void mergeSidebarReviewCards(const QStringList &cardIds);
    void retireSidebarReviewCards(const QStringList &cardIds);
    void deleteSidebarReviewCards(const QStringList &cardIds);
    void addOutlineDocumentActions(QMenu *menu, const QString &pdfPath, int page, const QString &title);
    void refreshSidebarReviewCards(KParts::ReadOnlyPart *part, const QJsonObject &projections);
    // Sidebar › History of a Library PDF: its versions with their cards and answer feedback.
    void attachDocumentHistory(KParts::ReadOnlyPart *part);
    void updateDocumentHistory(KParts::ReadOnlyPart *part, Thoughts::ReviewFeedbackPanel *panel);
    // "Show history" of the card under review: the sidebar of the active tab on History, at @p cardId.
    void showCardHistory(const QString &cardId);
    // Sidebar › Agent Notes, for every document: the agents' notes beside it.
    void attachAgentNotesSidebar(KParts::ReadOnlyPart *part);
    void trackPdf();
    void renderLinkedPdf();
    void openLastPdfBuild();
    void showSyncOperations();
    void showSyncOperationLogs(const QString &operationId);
    void retrySyncOperation(const QString &operationId);
    void beginSourceConflictResolution();
    // Resolves the recorded conflict of one docs-relative source path.
    void beginSourceConflictResolutionForPath(const QString &path);
    // Lists the source files that need a merge, why, and offers to resolve each.
    void showSourceFilesNeedingMerge();
    // The window a merge dialog belongs to: this reader window while it is on screen, else the one the
    // user is working in (the Library window that asked). A hidden reader window must not come up empty
    // just to carry the dialog.
    QWidget *mergeDialogOwner();
    void sourceConflictResolutionBundleReady(const QVariantMap &bundle, const QString &error);
    void sourceConflictResolutionStatusChanged(const QString &operationId, const QString &status, const QString &message);
    void triggerFedoraSync();
    void handleFedoraRenderStatusChanged(const QString &target, const QJsonObject &status);
    void removePdfFromLibrary();
    void removePdfFromLibrary(const QString &pdfPath, const QString &filename, const QString &sha256, const QString &displayName);
    void choosePdfForLibrary();
    void chooseExternalPdfForLibrary();
    void renameUnlinkedDocument(const QString &filename, const QString &sha256, const QString &displayName);
    void handleLibraryImportFinished(const QString &requestId, const QString &filename, const QString &path, const QString &sha256, bool success, int statusCode, const QString &message);
    void handleLibraryDisplayNameFinished(const QString &requestId, const QString &filename, const QString &sha256, const QString &displayName, bool success, const QString &message);
    void handleLibraryRemovalFinished(const QString &requestId, const QString &filename, const QString &sha256, bool success, const QString &message);
    void updateCommentsOverviewFedoraProjections(const QJsonObject &projections, qint64 frontier);
    void updateCommentsOverviewPendingChanges(const QJsonObject &snapshot);
    void applyCommentsOverviewFedoraProjections();
    void applyCommentsOverviewPendingChanges();
    void updateCommentsOverviewManualSyncState(int state, const QString &message, const QString &details);
    void updateSyncOperationProgress(const QString &traceId, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &message);
    void updateSyncOperationTransferProgress(const QString &traceId, const QJsonObject &absoluteSnapshot);
    void updateSyncOperationJournalEvent(const QString &traceId, const QJsonObject &event);
    void updateCommentsOverviewRenderState(const QJsonObject &state);
    void updateCommentsOverviewAgentProposalResolution(const QString &operationId, const QStringList &proposalIds, const QString &action, const QString &status, const QString &message);
    // A reader tab saved an Accept or Reject to its source (Part::agentProposalsDecidedLocally).
    void readerProposalDecided();
    void openTrackedDocument(const QString &pdfPath);
    // Shows this window in front, restored if it was minimized.
    void presentWindow();
    // The source file (absolute) of the PDF in front, from the Library's projects; empty without one.
    QString currentSourcePath() const;
    // Edit Source in VS Code: the source of the PDF in front.
    void editCurrentSource();
    void openTrackedComment(const QString &pdfPath, const QString &annotationId, int page);
    void openTrackedFeedback(const QString &pdfPath, const QString &feedbackId);
    // A page of an HTML attachment of the sender's document (Part::requestHtmlAttachmentTab): in
    // a tab of its own next to the document's, or in the tab that already shows it.
    void openHtmlAttachmentTab(const QJsonObject &request);
    // Close in a page's tab: the tab closes and its document's tab shows again.
    void closeHtmlAttachmentTab();
    void openTrackedLogicalComment(const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location);
    void openTrackedAgentProposal(const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine);
    void editTrackedComment(const QString &pdfPath, const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch);
    void replyToTrackedComment(const QString &pdfPath, const QString &parentAnnotationId, int page, const QString &contents, const QVariantMap &logicalPayload);
    void deleteTrackedComments(const QList<Thoughts::TrackedCommentTarget> &targets);
    // The explicit Delete shared by the comment UI and `thoughtsctl delete-comment`.
    QVariantMap applyTrackedCommentDeletion(const QList<Thoughts::TrackedCommentTarget> &targets, QList<Thoughts::TrackedCommentTarget> *deleted);
    void runCommentDeletionHistory(const QList<Thoughts::TrackedCommentTarget> &targets, bool undo);
    void undoTrackedCommentChange(const QString &pdfPath);
    void redoTrackedCommentChange(const QString &pdfPath);
    void undoTrackedCommentChanges(const QList<Thoughts::TrackedCommentTarget> &targets);
    void redoTrackedCommentChanges(const QList<Thoughts::TrackedCommentTarget> &targets);

    void forgetRecentItem(QUrl const &url);

Q_SIGNALS:
    void moveSplitter(int sideWidgetSize);
    void syncOperationJournalChanged(const QString &traceId);
    void buildSourceDiffFinished(const QString &buildId);

private:
    struct TransferredTab {
        KParts::ReadWritePart *part = nullptr;
        bool printEnabled = false;
        bool closeEnabled = false;
        QString text;
        QString toolTip;
        QIcon icon;
        bool sidebarVisible = false;
    };

    Shell(KParts::ReadWritePart *initialPart, const QString &serializedOptions);
    void saveRecents();
    void setupAccel();
    void setupActions();
    void openNewTab(const QUrl &url, const QString &serializedOptions);
    void recordCurrentNavigationViewport(bool historyEntry);
    void updateNavigationHistoryActions();
    void forgetNavigationHistory(KParts::ReadWritePart *part);
    void applyOptionsToPart(QObject *part, const QString &serializedOptions);
    void connectPart(const KParts::ReadWritePart *part);
    void flushPendingSyncTransferProgress(const QString &traceId = {});
    int findTabIndex(QObject *sender) const;
    // The document's tab of @p url: never a page's tab, which has the document open too.
    int findTabIndex(const QUrl &url) const;
    // Pages of HTML attachments in tabs of their own (thoughts-html-attachment-cards-SPEC.md): a
    // second Part of the document that shows one page (Part::openHtmlAttachmentTab). They are put
    // next to their document's tab, reused for the same page, restored with the session and by
    // Undo Close Tab, and closing one shows its document's tab again.
    bool isHtmlAttachmentTab(int tab) const;
    int findHtmlAttachmentTab(const QUrl &pdf, const QString &path) const;
    // The page tab of @p request ("path", what to show) for @p pdf, made after @p besideTab
    // (-1: at the end) when there is none; null when the page cannot be shown.
    KParts::ReadWritePart *showHtmlAttachmentTab(const QUrl &pdf, const QJsonObject &request, int besideTab);
    void readRecentFilesSettings();
    void addTabContextMenuActions(QMenu *menu, int tab);
    // A page's tab menu: Open in Browser, its document, Copy page name, Copy for agent, Send to Agent
    // (no file of its own).
    void addHtmlAttachmentTabMenuActions(QMenu *menu, int tab);
    // Move Tab to New Window and Close Other Tabs, for every tab.
    void addTabWindowMenuActions(QMenu *menu, const QPointer<QWidget> &tabPage);
    // Copy for agent for @p tab: its document, source and place, then the cards reviewed and the
    // text feedback on the PDF version on screen. The tab menu and File > Copy for Agent use it.
    void copyTabForAgent(int tab);
    // What Copy for agent copies for @p tab (empty when it has no local file).
    QString agentTextOfTab(int tab);
    // Send to Agent for @p tab (its menu's Send to Agent, File > Send to Agent): at once, without a
    // dialog, exactly what Copy for agent copies, with the page on screen as Send Page to Agent draws
    // it (an HTML page's tab: the page as its web view shows it), to @p session
    // (origin.conversation_id, copy_for_agent); remembered as the last session.
    // The reader says "Sent to <session>" (or why not) in its notice; @p done hears the same.
    void sendTabToSession(int tab, const Thoughts::AgentNotes::Recipient &session, std::function<void(bool ok, const QString &notice)> done = {});
    // sendTabToSession for an HTML page's tab: @p text with the picture its Part takes of the page.
    void sendHtmlPageTabToSession(int tab, const Thoughts::AgentNotes::Recipient &session, const QString &text,
                                  const std::function<void(bool ok, const QString &notice)> &tell);
    // Pictures asked of page tabs' Parts (requestHtmlAttachmentPicture), by request id.
    QHash<QString, std::function<void(const QImage &)>> m_htmlPagePictureRequests;
    // File > Send to Last Agent (⌘⇧A): to the session sent to last; none yet: the sessions to pick
    // from, in a menu over the reader.
    void sendTabToLastSession(int tab);
    // @p menu: the sessions (AgentSessionList), a click sending @p tab's page (asked when clicked),
    // in the order of their proposals on @p tab's document.
    void fillSendToAgentMenu(QMenu *menu, std::function<int()> tab);
    // @p tab's document as Send to Agent's order reads it: its source and the other documents'.
    Thoughts::AgentNotes::DocumentSources documentSourcesOfTab(int tab) const;
    // Every proposal the record store at @p storePath ever held (pending, and decided ones as they
    // were before), with the conversation that wrote it; for a worker thread.
    static QList<Thoughts::AgentNotes::ProposalAuthorship> readProposalAuthorship(const QString &storePath);
    // Lets AgentSessionList read them in the background (once per run).
    static void installProposalLoader();
    void closeOtherTabs(QWidget *tabPage);
    bool closeTabInternal(int tab, bool promptToSave);
    void detachTab(int tab, const QPoint &globalPosition);
    QPoint newWindowPosition() const;
    TransferredTab takeTab(int tab);
    bool insertTransferredTab(TransferredTab tab, int insertionIndex);
    bool transferTabTo(Shell *destination, QWidget *page, int insertionIndex);
    void applyTransferredTabMetadata(int tab, const TransferredTab &transferredTab);
    KParts::ReadWritePart *trackedCommentPart(const QString &pdfPath, bool allowOpen, QString *error);
    // trackedCommentPart for the Library's opening of a @p kind (comment, proposal), timed.
    KParts::ReadWritePart *trackedOpenPart(const QString &kind, const QString &pdfPath, QString *error);
    void tryOpenTrackedFeedback(const QString &pdfPath, const QString &feedbackId, quint64 requestGeneration, int attempt);
    void tryOpenTrackedComment(const QString &pdfPath, const QString &annotationId, int page, quint64 requestGeneration, int attempt);
    void tryOpenTrackedLogicalComment(const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location, quint64 requestGeneration, int attempt);
    void tryOpenTrackedAgentProposal(const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine, quint64 requestGeneration, int attempt);
    void removeEmptyPlaceholderPart();
    void updateTabControls();
    QRect detachedWindowGeometry(const QPoint &globalPosition, const QSize &requestedSize) const;
    void startExternalTabDrag(QWidget *page);
    QPixmap tabDragPixmap(int tab) const;
    int tabInsertionIndex(const QPoint &globalPosition) const;
    bool handleTabDragMove(const QMimeData *mimeData, const QPoint &globalPosition);
    bool handleTabDrop(const QMimeData *mimeData, const QPoint &globalPosition);
    void showTabDropIndicator(int insertionIndex);
    void hideTabDropIndicator();
    void animateTabDrop(const QPixmap &pixmap, const QPoint &hotSpot, const QPoint &globalPosition, int tab);
    void animateWindowGeometry(const QRect &finalGeometry, const QPoint &dropPosition, bool fadeIn);
    void showCommentsOverview(bool activate);
    static QString linkedPdfRenderTarget(const QString &docsRoot, const QUrl &url);
    static bool isExternalPdfForLibrary(const QString &docsRoot, const QUrl &url);
    static QUrl webDocumentUrl(const QString &input);
    enum class PdfLibraryState {
        NoDocument,
        SourceLinked,
        SourceLinkedMissingLayout,
        UnlinkedLibrary,
        OutsideLibrary,
        UnknownLibrary,
    };
    enum class TargetSyncMode {
        Reconcile,
        PublishSources,
        SnapshotOnly,
    };
    static PdfLibraryState pdfLibraryState(const QString &docsRoot, const QUrl &url);
    static TargetSyncMode targetSyncMode(bool libraryAvailable, bool targetFound, const QString &kind);
    static QString normalizedRetrySyncTarget(const QString &storedTarget);
    static bool dispatchTargetSync(QObject *part, const QString &target, TargetSyncMode mode, const QString &traceId);
    PdfLibraryState activePdfLibraryState() const;
    PdfLibraryState quickActivePdfLibraryState() const;
    void requestPdfLibraryStateRefresh(bool force = false);
    void finishPdfLibraryStateRefresh(const QVariantMap &snapshot);
    void schedulePdfLibraryPartRefresh(const KParts::ReadWritePart *part);
    void configurePdfSourceFreshnessWatcher(const QStringList &files, const QStringList &directories);
    void handlePdfSourcePathsChanged(const QStringList &paths);
    void schedulePdfSourceFreshnessRefresh();
    static QVariantMap localSourceFreshnessForTesting(const QString &docsRoot,
                                                      const QString &project,
                                                      const QString &sourceManifestPath,
                                                      const QString &publicationStatePath,
                                                      const QString &target = QString(),
                                                      const QString &expectedInputSnapshotSha256 = QString(),
                                                      const QVariantMap &buildInputInspection = QVariantMap());
    QUrl activePdfUrl() const;
    QString activePdfSourcePath() const;
    QString activePdfSuggestedFilename() const;
    static QString pdfLinkSafeFilename(const QString &value);
    // A link's PDF, or the published copy of the same name when the linked path is missing.
    static QUrl documentLinkTarget(const QUrl &url, const QString &docsRoot);
    void beginLibraryImport(const QString &sourcePath, const QString &filename, const QPointer<KParts::ReadWritePart> &sourcePart, uint page, const QUrl &originalUrl);
    void publishSourceDocument();
    void openPendingSourcePublicationOutput();
    void openSourcePublicationLog();
    static QString libraryPdfSha256(const QString &docsRoot, const QUrl &url);
    static bool repairShellMenuLabels(QDomDocument &document);
    static bool migrateLegacyPdfLibraryActions(QDomDocument &document);
    static void migrateLegacyPdfLibraryActionConfig();
    static QString newestArchivedSidecar(const QString &docsRoot, const QString &stem);
    static bool restoreArchivedSidecar(const QString &archivedSidecar, const QString &destinationSidecar, QString *error);
    void updatePdfLibraryAction();
    // Each menu entry once: the toolbar stand-ins of the part's actions are taken out of the menus,
    // where KDE's standard layout puts some of them (Print Preview, Configure…) beside the part's own.
    void removeMenuDuplicates();
    KParts::ReadWritePart *fedoraGatewayPart() const;
    QJsonObject effectiveFedoraProjections() const;
    static bool requestFedoraBuildForTarget(QObject *gateway, const QString &target, const QString &traceId = QString());
    void refreshCommentsOverviewDocumentSyncStates();
    void updateCommentsOverviewDocumentSyncPresentations();
    void scheduleCommentsOverviewDocumentSyncPresentationUpdate();
    void startCommentsOverviewSync();
    bool hasOpenDocument() const;
    void showOverviewHome();
    static QDateTime lastSuccessfulGlobalSyncAt();
    static QJsonObject latestGlobalSyncOperationSummary();
    static QJsonObject latestDocumentSyncOperationSummary(const QString &target);
    static QJsonArray recentSyncOperationsWithHistory();
    static QJsonArray recentSyncOperationSummaries();
    static QJsonObject syncOperationWithHistory(const QString &operationId);
    QVariantMap activeFedoraBuildActivity() const;
    QVariantMap activeFedoraRenderStatus();
    void updatePdfLibraryWidget(bool sourceLinked, const QString &status = {}, const QString &reason = {}, Thoughts::DocumentSyncStatus syncState = Thoughts::DocumentSyncStatus::SyncNeeded);
    static Thoughts::CommentsOverview *commentsOverview();
    // Windows and Linux: the Library window's own menu bar (File, Settings, Help). Without a PDF open it is
    // often Thoughts' only window, and the main window's menus (Server…, Logs…, Configure, Check for Updates…,
    // About) are out of reach. The Mac has one menu bar for every window, so it is not added there.
    static void addLibraryMenuBar(Thoughts::CommentsOverview *overview);
    // Cards changed kind or were retired here: every window's review, Cards lists and the Library's cards read them again.
    static void refreshReviewCardsEverywhere();
    static Thoughts::AgentNotesWindow *agentNotesWindow();
    // The Agent Notes views (window, sidebars): a note's document is its Library PDF on this Mac
    // (else no path), titled as the reader titles it.
    static QString agentNoteDocumentPath(const QString &documentId, const QString &filename, QString *title);
    static void openAgentNoteDocument(const QString &documentId, const QString &path);
    // A link of an agent note's picture: a Library PDF at its page (and the place on it) in a
    // reader, or a web page in the browser.
    static void openAgentNoteLink(const QString &href);
    // A link to a place in a Library PDF (thoughts-pdf-link:<file>.pdf?page=N&x=&y=&rect=,
    // part/pdfplacelink.h): the PDF in the window that tracks it, at the page, with (x, y) near
    // the top and rect highlighted for a moment. False when @p href is no such link; a PDF this
    // Mac does not have yet is explained in a message.
    static bool openPdfPlaceLink(const QString &href);
    // @p part: a sidebar's reader (its page goes to the agent); none: the window, which sends
    // the page of the reader in front.
    static Thoughts::AgentNotesView *createAgentNotesView(bool sidebar, KParts::ReadOnlyPart *part = nullptr);
    // The name a page sent to an agent gives @p part's document (the reader's title for it).
    static QString readerDocumentTitle(KParts::ReadOnlyPart *part, const QString &path, const QString &documentId);
    // The Agent Notes button: a dot on its icon and the count in its tooltip while notes are unread.
    void updateAgentNotesAction(int unread);
    static Shell *shellForCommentsOverview(const QString &pdfPath = QString());
    QVariantMap currentReviewDocument() const;
    bool presentReviewCard(const Thoughts::ReviewCard &card, bool masked, QString *error);
    // The card under review again, masked, in the tab whose reload took its mask away: without
    // opening or switching tabs, moving the page or raising the window.
    bool restoreReviewCard(const Thoughts::ReviewCard &card, QString *error);
    bool showReviewCardMask(KParts::ReadWritePart *part, const Thoughts::ReviewCard &card, bool navigate, QString *error);
    void showReviewCardPdfWindow(bool restoreCardBrowser);
    // A PDF version from a review card's History: the card's Library PDF (@p current) or an
    // earlier version from the version cache, opened as a plain read-only PDF titled @p title.
    bool openPdfVersion(const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current, QString *error);
    void clearReviewMasks();
    QJsonObject handleSyncCommandRequest(const QJsonObject &request);
    QJsonObject startSyncCommand(const QString &target);
    QJsonObject deleteCommentCommand(const QJsonObject &request);
    // thoughtsctl resolve-source-conflict: the resolver's queued resolution, without its dialogs.
    QJsonObject resolveSourceConflictCommand(const QJsonObject &request);
    // What the request keeps: "keep" is fedora or local, or "content_file" names a finished merge.
    static QByteArray sourceConflictCommandContents(const QVariantMap &bundle, const QJsonObject &request, QString *error);
    // thoughtsctl list-todos, add-todo and edit-todo (todocommands.h).
    QJsonObject todoCommand(const QString &command, const QJsonObject &request);
    // thoughtsctl link-conversation: links an agent conversation to a Project (part/agentconversations.h).
    QJsonObject linkConversationCommand(const QJsonObject &request);
    QJsonObject libraryProjectCommand(const QJsonObject &request);
    // Once per installation: links the conversations of the current proposals (spec section 6.7).
    void backfillAgentConversations(const QJsonObject &projections);
    QJsonObject syncCommandStatus(const QString &operationId) const;
    bool setActiveManualSyncOperation(const QString &operationId, const QString &initiationControl);
    void clearActiveManualSyncOperation();
    bool hasProcessManualSyncOperation() const;
    static void reconcileInterruptedSyncOperationsForTesting(qint64 startedBeforeMs);

private:
    static QString sourceConflictContentsErrorForTesting(const QByteArray &contents, const QString &path, const QList<QByteArray> &versions = {});
    static QByteArray editableSourceConflictForTesting(const QByteArray &serverMerge,
                                                       const QByteArray &macContents,
                                                       const QByteArray &fedoraContents);
    bool queueSourceConflictResolution(const QVariantMap &bundle, const QByteArray &contents);
    // The durable queueing itself; returns its operation ID, or empty with @p error set.
    QString queueSourceConflictContents(const QVariantMap &bundle, const QByteArray &contents, QString *error);
    void connectSourceConflictSignals(KParts::ReadWritePart *part);
    void triggerFedoraSyncWithInitiationControl(const QString &initiationControl);
    // Signed in or out, or a server address or token saved by hand: the app's requests and the
    // repository read the server and tokens again and a Sync connects, once no Sync runs.
    static void reconnectToServer();
    static void syncAfterServerChange(int attempts = 0);
    // An agent's proposals and new PDF builds reach Fedora without this computer knowing. One
    // watch per process asks the server, through the first window: every 20 seconds, or at once
    // when a server with sync_state_wait_v1 says its state changed (SyncStateWatch).
    static void startRemoteWorkWatch();
    static Thoughts::SyncStateWatch *remoteWorkWatchForTesting();
    // The Syncs the app starts by itself (automaticsyncpolicy.h).
    // `manualSync` is the part's fedoraManualSyncState of the Sync that ended.
    void automaticSyncEnded(bool success, const QVariantMap &manualSync);
    static void refreshAutomaticSyncNotice();
    // Forgets failed automatic Syncs and their wait (tests).
    static void resetAutomaticSyncForTesting();
    // The Sync that follows a proposal decision made on this computer.
    static void proposalDecided();
    // Edit… and Cover the Whole Document… of a reader tab's Cards list.
    void editSidebarReviewCardAreas(KParts::ReadWritePart *sourcePart, const QString &cardId, bool wholeDocument);
    static void startProposalDecisionSync();
    // Forgets a pending one; `delayMs` replaces the delay after a decision (0: the real one).
    static void resetProposalDecisionSyncForTesting(int delayMs = 0);
    // The Sync that sends the changes made on this computer (localchangesync.h): the outbox
    // as a pending-changes snapshot shows it, its first state read directly, and the timer.
    static void observeLocalChanges(const QJsonObject &snapshot);
    static void seedLocalChanges(const QJsonObject &snapshot);
    static void armLocalChangeSync(qint64 atMs);
    static void startLocalChangeSync();
    // Forgets the outbox and a wanted Sync; `quietMs` and `maximumWaitMs` replace the delays
    // (0: the real ones; the others become a third of `quietMs`).
    static void resetLocalChangeSyncForTesting(int quietMs = 0, int maximumWaitMs = 0);
    // Compacts the Sync journal once, while no Sync runs and nobody uses the app.
    static void startSyncJournalUpkeep();
    // Writes what the caches hold to the developer log.
    static void recordCacheSizes();
    void checkRemoteWork();
    void reconcileOrphanedManualSync();
    void reloadAllXML();
    void materializeMainToolBar();
    void scheduleMainToolBarMaterialization();
    void scheduleMainToolBarCentering();
    void centerMainToolBarPageSelector();
    void styleMainToolBarActions();
    void styleDocumentTabs();
    void updateDocumentTitle();
    void applyReaderIcons(KActionCollection *actions);
    // The menu of the pencil's button (its colours and eraser) when @p global is on that button;
    // false when it is not, and the toolbar's own menu shows.
    bool showPencilMenu(const QPoint &global);
    void syncPdfLibraryStatusToolButton();
    void updatePdfSyncStatus();
    void schedulePdfSyncStatusUpdate();
    void showPdfSyncMenu();
    bool eventFilter(QObject *obj, QEvent *event) override;

    KPluginFactory *m_partFactory;
    KRecentFilesAction *m_recent;
    QStringList m_fileformats;
    bool m_fileformatsscanned;
    QAction *m_printAction;
    QAction *m_closeAction;
    KToggleAction *m_fullScreenAction;
    KToggleAction *m_showMenuBarAction;
    bool m_menuBarWasShown, m_toolBarWasShown;
    bool m_unique;
    QTabWidget *m_tabWidget;
    KToggleAction *m_openInTab;
    WelcomeScreen *m_welcomeScreen;
    QStackedWidget *m_centralStackedWidget;
    Sidebar *m_sidebar = nullptr;

    Thoughts::ReaderDisplayTitleStore *m_readerDisplayTitles = nullptr;
    void refreshReaderTabTitles();
    // A reader tab's icon: a dot while its Part keeps an earlier version on screen (a newer one is
    // ready), else its document type's (none where tabs are pills of their title).
    QIcon readerTabIcon(int tab) const;
    // The dot of a tab that keeps an earlier version (one icon, so tests can tell it).
    static QIcon keptVersionTabIcon();
    void selectReaderTitle(KParts::ReadWritePart *part, const QString &documentId, const QString &title, const QString &projectId = {});
    struct TabState {
        explicit TabState(KParts::ReadWritePart *p)
            : part(p)
            , printEnabled(false)
            , closeEnabled(false)
        {
        }
        KParts::ReadWritePart *part;
        bool printEnabled;
        bool closeEnabled;
    };
    QList<TabState> m_tabs;
    struct NavigationLocation {
        QPointer<KParts::ReadWritePart> part;
        QUrl url;
        QString viewport;
    };
    QList<NavigationLocation> m_navigationHistory;
    int m_navigationHistoryIndex = -1;
    bool m_restoringNavigation = false;
    QList<QUrl> m_closedTabUrls;
    // The page of each closed tab that showed one (empty for a document), as m_closedTabUrls.
    QStringList m_closedTabPages;
    QAction *m_nextTabAction;
    QAction *m_prevTabAction;
    QAction *m_undoCloseTab;
    QAction *m_pdfLibraryAction = nullptr;
    QJsonObject m_globalPendingChangesSnapshot;
    // Every open tab relays the same repository signal; one burst is applied once.
    QJsonObject m_pendingFedoraProjections;
    bool m_fedoraProjectionsUpdateScheduled = false;
    // The projections last applied, to recognise changes that touch only agent conversations.
    QJsonObject m_appliedFedoraProjections;
    bool m_agentConversationBackfillRunning = false;
    bool m_pendingChangesUpdateScheduled = false;
    QAction *m_pdfLibraryWidgetAction = nullptr;
    QPointer<QToolButton> m_pdfBuildStatusButton;
    QMenu *m_pdfLibraryMenu = nullptr;
    QAction *m_pdfSyncSummaryMenuAction = nullptr;
    QAction *m_pdfSyncDetailMenuAction = nullptr;
    QAction *m_pdfSyncNowMenuAction = nullptr;
    QAction *m_pdfSignInMenuAction = nullptr;
    // Closed by the person: no longer one of the windows reopened at the next start.
    bool m_windowClosing = false;
    QAction *m_pdfStatusMenuAction = nullptr;
    QAction *m_pdfStatusDetailMenuAction = nullptr;
    QAction *m_pdfPrimaryMenuAction = nullptr;
    QAction *m_pdfSourceConflictAction = nullptr;
    QAction *m_pdfLastBuiltMenuAction = nullptr;
    QString m_pdfBuildStatusText;
    QString m_pdfBuildStatusAccessibleName;
    QString m_pdfSyncIconName;
    QString m_pdfPrimaryMenuCommand;
    int m_fedoraManualSyncState = 0;
    // Merge cards waiting for the Sync that sends this Mac's source first (its last edits, or
    // proposal decisions): the merge dialog opens when that Sync ends.
    struct PendingCardMerge {
        QPointer<KParts::ReadWritePart> part;
        QStringList cardIds;
    };
    std::optional<PendingCardMerge> m_mergeAfterSync;
    void mergeReviewCardsIn(KParts::ReadWritePart *part, const QStringList &cardIds, bool afterSync);
    QString m_fedoraManualSyncTarget;
    QString m_fedoraManualSyncMessage;
    QString m_fedoraManualSyncDetails;
    QString m_activeManualSyncOperationId;
    QHash<QString, QJsonObject> m_pendingSyncTransferSnapshots;
    QTimer *m_syncTransferProgressTimer = nullptr;
    QString m_activeBuildOperationId;
    QString m_activeBuildOperationTarget;
    QPointer<QDialog> m_syncOperationsDialog;
    QPointer<QDialog> m_syncOperationLogDialog;
    std::unique_ptr<Thoughts::BuildTerminalLauncher> m_buildTerminalLauncher;
    QUrl m_pdfLibraryStateUrl;
    PdfLibraryState m_cachedPdfLibraryState = PdfLibraryState::UnknownLibrary;
    QString m_cachedPdfLibraryRenderTarget;
    QString m_cachedPdfLibrarySha256;
    QString m_cachedPdfLibraryDisplayName;
    QDateTime m_cachedPdfLibraryPublishedAt;
    bool m_cachedPdfLibraryGenerationCurrent = false;
    bool m_cachedPdfLibraryStale = false;
    QString m_cachedPdfLibraryBuildStatus;
    QString m_cachedPdfLibraryBuildMessage;
    QString m_cachedPdfLibraryLastBuildId;
    bool m_cachedPdfLibraryLocalFreshnessAvailable = false;
    bool m_cachedPdfLibraryLocalSourcesCurrent = false;
    QString m_cachedPdfLibraryLocalFreshnessMessage;
    QString m_cachedFedoraRenderTarget;
    QVariantMap m_cachedFedoraRenderStatus;
    QString m_pdfStatusInteractionTarget;
    QString m_rebuildAuthorizedTarget;
    QString m_pendingRebuildAfterRefreshTarget;
    QString m_pendingRebuildInitiationControl;
    bool m_pendingRebuildSourcesCurrent = false;
    bool m_pdfLibraryStateReady = false;
    bool m_overviewPresentationRefreshScheduled = false;
    bool m_pdfLibraryRefreshRunning = false;
    bool m_pdfLibraryRefreshQueued = false;
    bool m_pdfLibraryPartRefreshScheduled = false;
    quint64 m_pdfLibraryRefreshSerial = 0;
    quint64 m_pdfLibraryActiveRefreshSerial = 0;
    QUrl m_pdfLibraryActiveRefreshUrl;
    QTimer *m_pdfSourceFreshnessTimer = nullptr;
    QSet<QString> m_pdfSourceFreshnessPaths;
    QSet<QString> m_pdfSourceFreshnessDirectories;
    QAction *m_showSidebarAction = nullptr;
    QAction *m_lockSidebarAction = nullptr;
    static QPointer<Thoughts::CommentsOverview> s_commentsOverview;
    static QPointer<Thoughts::AgentNotesWindow> s_agentNotesWindow;
    static QPointer<Shell> s_commentsOverviewShell;
    QPointer<Thoughts::ReviewOverview> m_reviewOverview;
    QPointer<Thoughts::PdfVersions::Fetcher> m_historyVersionFetcher;
    bool m_manualReviewFingerprintRunning = false;
    // A card for a whole section (Add Review Card on a section heading): NotHeading when the area
    // names no section heading (an ordinary card is made of it), OnlySelection when the person
    // chose to hide only the selected text (*question: what they typed).
    enum class SectionCardOutcome { NotHeading, Handled, OnlySelection };
    SectionCardOutcome offerSectionReviewCard(QObject *sourcePart, const QVariantMap &document, int page, const QRectF &rect, bool requested, QString *question);
    bool m_sectionCardRequested = false;
    QPointer<QProgressDialog> m_sourceConflictProgress;
    // Set while resolve-source-conflict waits: the bundle goes here instead of to the resolver dialog.
    std::function<void(const QVariantMap &, const QString &)> m_sourceConflictCommandBundleHandler;
    QPointer<QWidget> m_draggedTabPage;
    std::unique_ptr<ActivePartToolbarController> m_activePartToolbar;
    QPointer<QLabel> m_documentTitle;
    QPoint m_tabDragStartPosition;
    QPoint m_tabDragHotSpot;
    bool m_tabDragStarted = false;
    bool m_mainToolBarMaterializationScheduled = false;
    bool m_mainToolBarMaterializationInProgress = false;
    bool m_mainToolBarUpdatesSuppressed = false;
    bool m_mainToolBarCenteringScheduled = false;
    bool m_pdfSyncStatusUpdateScheduled = false;
    int m_activatingTab = -1;
    QList<QPointer<QAction>> m_materializedMainToolBarLayoutActions;
    TabDropOverlay *m_tabDropOverlay = nullptr;

    bool m_isValid;
    bool m_forcePdfLibraryChooser = false;
    QString m_pendingLibraryImportId;
    QString m_pendingLibraryImportFilename;
    QString m_pendingLibraryImportSourcePath;
    QString m_pendingLibraryImportArchivedSidecar;
    QString m_pendingLibraryImportDestinationSidecar;
    QPointer<KParts::ReadWritePart> m_pendingLibraryImportPart;
    QUrl m_pendingLibraryImportOriginalUrl;
    uint m_pendingLibraryImportPage = 0;
    QString m_pendingLibraryDisplayNameId;
    QString m_pendingLibraryRemovalId;
    QString m_pendingLibraryRemovalPath;
    QString m_pendingLibraryRemovalSha256;
    QString m_pendingSourceProject;
    QString m_pendingSourceTarget;
    quint64 m_trackedCommentOpenGeneration = 0;
    // The Library's opening of a comment or proposal in progress, for the developer log:
    // since the request, and how long finding or opening its tab took.
    QElapsedTimer m_trackedOpenClock;
    qint64 m_trackedOpenTabMs = 0;
};

#endif

// vim:ts=2:sw=2:tw=78:et
