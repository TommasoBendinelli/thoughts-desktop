/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_COMMENTSOVERVIEW_H
#define THOUGHTS_COMMENTSOVERVIEW_H

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QVariantMap>

#include <functional>
#include <optional>

#include "agentproposalparser.h"

class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QFormLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QTreeWidgetItem;
class QModelIndex;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QShowEvent;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QTreeWidget;
class QTimer;
class QThreadPool;
class QTabWidget;
class QWidget;
class CommentsOverviewTest;

namespace Thoughts
{

struct ReviewCard;
class ReviewCardsWidget;
class MentalModelView;
class LibrarySearchField;
struct LibrarySearchHit;

struct TrackedCommentTarget {
    QString pdfPath;
    QString annotationId;
    int page = 0;
    int fallbackRow = -1;
    int serviceToken = 0;
    QPointer<QObject> serviceOwner = {};
};

struct TrackedComment {
    QString documentStem;
    QString pdfPath;
    QString id;
    QString parentId;
    QString threadRootId;
    QString contents;
    QString selectedText;
    QString author;
    QString sourceFile;
    QString annotationPdfSha256;
    QString anchorStatus;
    QString priorityRaw;
    QString chatGptUrl;
    QString bundleId;
    QString proposalKind;
    QString proposalBeforePreview;
    QString proposalAfterPreview;
    QString proposalSourceSha256;
    // A broken wrapper in the source (spec issue code): listed, but it cannot be decided.
    QString proposalIssue;
    // A proposal removed outside Thoughts: its server row (protocol/agent-proposals.md §10.3).
    QJsonObject removedProposal;
    // A marker whose comment no longer exists ("Comment missing"): its overlay entry.
    QJsonObject missingMarker;
    QJsonObject location;
    QJsonObject logicalPayload;
    QJsonObject correspondence;
    QJsonArray pdfLocations;
    int sourceLine = 0;
    int page = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    QDateTime displayDate;
    QDateTime rootSortDate;
    QDateTime modifiedDate;
    bool dateIsFirstSeen = false;
    int replyOrder = 0;
    bool reply = false;
    bool pdfAvailable = false;
    bool locationPresent = false;
    bool locationValid = false;
    bool agentProposal = false;
    bool proposalSuppressed = false;
    bool liveSourceProposal = false;
    bool sourceProjected = false;
    bool sourceOnly = false;
    bool logicalComment = false;
    // A comment on a passage (document-feedback.md section 9), listed read-only: "like", "dislike",
    // "question" or "other", its record, whether the version rule hides it, the version it was made
    // on and the current version of that PDF or HTML page.
    QString feedbackKind;
    QJsonObject feedbackRecord;
    bool feedbackEarlier = false;
    QString feedbackSnapshot;
    QString feedbackCurrentSnapshot;

    bool operator==(const TrackedComment &) const = default;
};

struct TrackedCommentDocument {
    enum State {
        SourceLinked,
        PdfOnly,
    };

    QString stem;
    QString displayName;
    QString pdfPath;
    QString libraryFilename;
    QString libraryKind;
    QString librarySha256;
    QString libraryProject;
    QString libraryProjectId;
    bool sourceProposalOnly = false;
    bool sourceBuildDetached = false;
    // The publication's document is unchecked in its Project: Fedora keeps the
    // PDF and its comments but neither builds nor lists it, so it is no build
    // target and the Library does not show it.
    bool sourceUnselected = false;
    QString libraryDocumentId;
    QDateTime libraryPublishedAt;
    QDateTime libraryBuiltAt;
    qint64 libraryPageCount = -1;
    QString libraryLatestBuildMessage;
    // The source file the latest failed build of this PDF names, if any.
    QString libraryLatestBuildSource;
    QString error;
    QList<TrackedComment> comments;
    QList<QPair<QString, QString>> standaloneSources;
    QJsonArray librarySegments;
    // Prepared by the catalogue worker; navigation rendering never reads settings.
    QString navigationProjectId;
    QString navigationProjectName;
    QString navigationProjectTarget;
    QJsonObject navigationOptions;
    QHash<QString, QString> navigationSourcePaths;
    QSet<QString> suppressedCommentIds;
    qint64 pdfSizeBytes = -1;
    qint64 verifiedBuildInputBytes = -1;
    qint64 verifiedBuildInputCount = -1;
    State state = PdfOnly;
    bool pdfAvailable = false;
    bool libraryGenerationCurrent = false;
    bool libraryPdfStale = false;
    bool verifiedBuildInputsAvailable = false;
    int orphanedCommentCount = 0;

    bool operator==(const TrackedCommentDocument &) const = default;
};

struct OrphanArchiveResult {
    int archived = 0;
    QStringList errors;
};

struct ProjectAttachment {
    QString filename;
    QString sha256;
    QString title;
    QString placement;

    bool operator==(const ProjectAttachment &) const = default;
};

struct ProjectAttachmentMutationResult {
    bool success = false;
    bool changed = false;
    QString error;
};

QList<TrackedCommentDocument> loadTrackedCommentDocuments(const QString &docsRoot,
                                                          const QJsonObject &fedoraProjections = {},
                                                          bool *liveSourcesValid = nullptr,
                                                          const QString &onlyFilename = {},
                                                          bool discoverIncludedSources = true);
QSet<QString> pendingLibraryRemovalFilenames(const QJsonObject &pendingChanges);
OrphanArchiveResult archiveOrphanCommentSidecars(const QString &docsRoot);
QList<ProjectAttachment> loadProjectAttachments(const QString &docsRoot,
                                                const QString &project,
                                                QString *error = nullptr);
ProjectAttachmentMutationResult setProjectAttachment(const QString &docsRoot,
                                                     const QString &project,
                                                     const QString &pdfPath,
                                                     const QString &filename,
                                                     const QString &sha256,
                                                     const QString &title,
                                                     bool attached);

class CommentsOverview : public QDialog
{
    Q_OBJECT
    friend class ::CommentsOverviewTest;

public:
    // The Agent notes button beside Sync shows how many notes are unread.
    void setAgentNotesUnread(int unread);
    // A new to-do; diary entries and thoughtsctl add-todo start from it too.
    static QJsonObject standaloneNotePayload(const QString &contents, const QDateTime &createdAt);

    using ReviewCardPresenter = std::function<bool(const ReviewCard &, bool, QString *)>;
    using ReviewCardReviewer = std::function<void(const QString &)>;
    using ProjectionProvider = std::function<QJsonObject()>;
    // Stores the device token typed in Server… for an account of the secret
    // store; unset, the system's secret store. Tests replace it.
    using ServerTokenWriter = std::function<bool(const QString &account, const QByteArray &token, QString *error)>;
    // Asks Fedora what moving a source document would do; replies with the plan or an error.
    using SourceMovePlanner = std::function<void(const QJsonObject &request, QObject *context,
                                                 std::function<void(const QJsonObject &plan, const QString &error)> reply)>;

    enum Operation {
        EditOperation,
        DeleteOperation,
        UndoOperation,
        RedoOperation,
        CreateOperation,
    };
    Q_ENUM(Operation)

    explicit CommentsOverview(const QString &docsRoot,
                              QWidget *parent = nullptr,
                              ReviewCardPresenter reviewCardPresenter = {},
                              ReviewCardReviewer reviewCardReviewer = {},
                              ProjectionProvider projectionProvider = {});
    ~CommentsOverview() override;

    int documentCount() const;
    int commentCount() const;
    QString selectedDocument() const;
    void reload();
    bool addOutlineDocumentActions(QMenu *menu, const QString &pdfPath, int page, const QString &outlineTitle);
    void requestReload(const QString &reason = {});
    bool revealComment(const QString &pdfPath, const QString &annotationId);
    void completeReply(const QString &pdfPath, const QString &parentAnnotationId, const QVariantMap &result);
    void completeOperation(Operation operation, const QString &pdfPath, const QString &annotationId, const QVariantMap &result);
    // Result of commentsRestoreAnchorRequested: {success, message, results}.
    void completeAnchorRestore(const QVariantMap &result);
    // Result of missingCommentMarkersResolveRequested: {success, message, results}.
    void completeMissingCommentResolution(const QVariantMap &result);
    // Results of proposalsRestoreRequested / proposalsDismissRequested: {success, message, results}.
    void completeRemovedProposalRestore(const QVariantMap &result);
    void completeRemovedProposalDismiss(const QVariantMap &result);
    void completeChatGptUrlUpdate(const QString &pdfPath, const QString &annotationId, const QVariantMap &result);
    void completeBatchDelete(const QList<TrackedCommentTarget> &targets, const QVariantMap &result);
    void completeBatchHistoryOperation(Operation operation, const QList<TrackedCommentTarget> &targets, const QVariantMap &result);
    void setFedoraProjections(const QJsonObject &projections);
    void setPdfSyncState(const QJsonObject &state);
    void showServerDetails();
    void showSummary();
    void reloadReviewCards();
    // An agent created a Project or imported a PDF through the app (create-project, import-document).
    void libraryProjectsChangedByCommand();
    int commentIssueCount() const;
    int reviewCardIssueCount() const;
    void showCommentIssues();
    void showReviewCardsState(const QString &stateFilter);
    void showReviewCardInfo(const QString &cardId);
    bool sourceMonitoringRefreshNeeded(const QString &pdfPath) const;
    QVariantList documentSyncDescriptors() const;
    void setSyncSnapshot(const QJsonObject &presentations,
                         const QJsonObject &aggregate,
                         const QJsonObject &pendingChanges,
                         const QDateTime &lastSuccessfulSyncAt,
                         const QJsonObject &latestSyncOperation = QJsonObject());
    // What to say while automatic Syncs wait after failures ("" when none waits).
    void setAutomaticSyncNotice(const QString &notice);
    bool syncOperationRunning() const;
    void syncStarted(const QDateTime &startedAt = QDateTime());
    void syncFinished(bool success, const QString &message, const QString &details = QString(), const QString &errorCode = QString(), const QString &operationId = QString());
    void completeAgentProposalResolution(const QStringList &proposalIds, bool queued, const QString &error = QString());
    void finishAgentProposalResolution(const QStringList &proposalIds,
                                       const QString &action,
                                       const QString &status,
                                       const QString &message = QString());
    void applySourceMonitoringInformation(const QString &pdfPath,
                                          const QVariantMap &information);
    Q_INVOKABLE void applySourceMonitoringBatch(quint64 requestId, const QVariantMap &information);

Q_SIGNALS:
    void noteAttachmentOpenRequested(const QVariantMap &attachment);
    void sourceMonitoringBatchRequested(quint64 requestId, const QStringList &targets);
    void documentActivated(const QString &pdfPath);
    void documentPageActivated(const QString &pdfPath, int page, const QString &documentId = {}, const QString &displayTitle = {}, const QString &projectId = {});
    // A Library search result: open the PDF, mark the query's words, and go to page (if > 0).
    void librarySearchPageActivated(const QString &pdfPath, int page, const QString &query);
    void commentActivated(const QString &pdfPath, const QString &annotationId, int page);
    void documentFeedbackActivated(const QString &pdfPath, const QString &feedbackId);
    void documentFeedbackWriteRequested(const QVariantMap &record);
    void logicalCommentLocationActivated(const QString &pdfPath, const QString &commentId, int page, const QVariantMap &location);
    void agentProposalActivated(const QString &pdfPath, const QString &proposalId, const QString &sourcePath, int sourceLine);
    void logicalCommentMutationRequested(const QVariantMap &payload, Operation operation, const QString &pdfPath, const QString &commentId);
    void logicalCommentUrlMutationRequested(const QVariantMap &payload, const QString &pdfPath, const QString &commentId);
    void commentEditRequested(const QString &pdfPath, const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch);
    void commentReplyRequested(const QString &pdfPath, const QString &parentAnnotationId, int page, const QString &contents, const QVariantMap &logicalPayload);
    void commentsDeleteRequested(const QList<TrackedCommentTarget> &targets);
    void commentsRestoreAnchorRequested(const QStringList &commentIds);
    // action "remove" or "restore"; each marker is its overlay entry (anchor_id, source_file, line, ...).
    void missingCommentMarkersResolveRequested(const QString &action, const QVariantList &markers);
    void proposalsRestoreRequested(const QStringList &proposalIds);
    void proposalsDismissRequested(const QStringList &proposalIds);
    void commentUndoRequested(const QString &pdfPath);
    void commentRedoRequested(const QString &pdfPath);
    void commentsUndoRequested(const QList<TrackedCommentTarget> &targets);
    void commentsRedoRequested(const QList<TrackedCommentTarget> &targets);
    void openCodexRequested(const QString &pdfPath, const QString &annotationId);
    void openSourceRequested(const QString &sourcePath, int line);
    // From a review card's History (ReviewCardsWidget::pdfVersionOpenRequested).
    void pdfVersionOpenRequested(const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current);
    void commentChatGptUrlUpdateRequested(const QString &pdfPath, const QString &annotationId, const QString &url);
    void libraryDisplayNameRequested(const QString &filename, const QString &sha256, const QString &displayName);
    void libraryRemovalRequested(const QString &pdfPath, const QString &filename, const QString &sha256, const QString &displayName);
    void projectAttachmentChangedLocally(const QString &target);
    void projectExportRequested(const QJsonObject &project, const QString &destination);
    void trackPdfRequested();
    void syncRequested();
    void reviewOverviewRequested();
    void agentNotesRequested();
    void sourceMonitoringInformationRequested(const QString &pdfPath);
    void syncOperationsRequested();
    void syncOperationLogsRequested(const QString &operationId);
    void sourceMergeRequested();
    void documentsChanged();
    void agentProposalsResolveRequested(const QString &bundleId, const QString &action, const QStringList &proposalIds);
    // The user accepted or rejected proposals here, or undid that, and the
    // sources on this computer changed: the app sends it by itself shortly.
    void agentProposalsDecidedLocally();
    void reviewCorrespondenceReviewRequested(const QVariantMap &payload);
    // "Remove from Project" on an agent conversation row: @p record is the removed next revision to queue.
    void agentConversationRemoveRequested(const QVariantMap &record);

protected:
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;

private Q_SLOTS:
    void handleDocsPathsChanged(const QStringList &paths);
    void applyFilters();
    void activateDocument(QTreeWidgetItem *item);
    void traceInteraction(const QString &event, const QModelIndex &index = QModelIndex());
    void activateComment(const QModelIndex &index);
    void scheduleReload(const QString &path = QString());
    void processScheduledReloads();
    void selectedCommentChanged(const QModelIndex &current, const QModelIndex &previous);
    void saveSelectedComment();
    void createStandaloneNote();
    void createDiaryEntry();
    void deleteSelectedDiaryEntries();
    void replyToSelectedComment();
    void deleteSelectedComment();
    void restoreSelectedAnchors();
    void removeSelectedMissingMarkers();
    void restoreSelectedMissingComments();
    void restoreSelectedRemovedProposals();
    void dismissSelectedRemovedProposals();
    void copySelectedRemovedProposals();
    void showCommentsContextMenu(const QPoint &position);
    void showDocumentsContextMenu(const QPoint &position);
    // Source file actions from the Library tree; each returns why it failed,
    // or an empty string. The menu asks and confirms before calling them.
    static QString currentFolderBuildError(const QJsonObject &project, const QString &owner, const QString &error);
    QString renameSourceFile(const QString &path, const QString &requestedName);
    QString deleteSourceFile(const QString &path);
    QString createSourceFile(const QString &folder, const QString &requestedName, const QString &extension, QString *created = nullptr);
    // Opens the conversation link of an Agent conversations row; nothing happens without a link.
    void openAgentConversation(const QJsonObject &record);
    void removeAgentConversation(const QJsonObject &record);
    void showLibraryContextMenu(const QPoint &position);
    // The Library header's menu: a checkbox per column (Project always shown) and Restore default columns.
    void showLibraryHeaderMenu(const QPoint &position);
    void showLibraryItemContextMenu(QTreeWidgetItem *item, QTreeWidget *view, const QPoint &position, bool allowOrder);
    void deleteDocumentComponent(const QString &pdfPath, const QString &componentId, const QString &title);
    void addComponentManagementActions(QMenu *menu, const TrackedCommentDocument &document, const QString &componentId, const QString &title, int page);
    void finishDocumentRemoval();
    void refreshDocumentNavigation();
    void manageDocuments(const QJsonObject &project);
    void showDocumentOrder(const QString &pdfPath, const QString &selectedId);
    void createLibraryProject();
    QString saveWebsiteImport(const QJsonObject &project, const QString &projectName, const QString &target, const QString &path, const QString &name, const QUrl &link = {});
    void importLibraryWebsite(const QJsonObject &project, const QString &projectName, const QString &target, const QString &link = {});
    void moveLibraryDocument(const QJsonObject &project, const QJsonObject &document);
    void moveLibrarySourceFolder(const QJsonObject &project, const QString &owner, const QString &name);
    void moveLibrarySourceDocument(const QJsonObject &project, const QJsonObject &document);
    void setSourceMovePlanner(SourceMovePlanner planner) { m_sourceMovePlanner = std::move(planner); }
    void setServerTokenWriter(ServerTokenWriter writer) { m_serverTokenWriter = std::move(writer); }
    void editLibraryProject(const QJsonObject &project, const QString &action, const QJsonObject &document = {});
    void showDocumentInformation(const QString &pdfPath);
    void undoOverviewChange();
    void redoOverviewChange();
    void showFedoraDetails();
    void showReviewCards();
    void acceptSelectedAgentProposals();
    void disregardSelectedAgentProposals();
    void confirmCurrentCorrespondence();
    void flagCurrentCorrespondence();
    void recomputeCurrentCorrespondence();

private:
    SourceMovePlanner m_sourceMovePlanner;
    ServerTokenWriter m_serverTokenWriter;
    void requestSourceMovePlan(const QJsonObject &request, QObject *context,
                               std::function<void(const QJsonObject &plan, const QString &error)> reply);
    struct CommentState {
        QString contents;
        QString priority;
        QString dueDate;
        QString dueTime;
        QString dueZone;
        QString visibleFrom;
        QJsonObject location;
        bool locationPresent = false;
    };

    struct HistoryEntry {
        QString pdfPath;
        QString annotationId;
        Operation operation = EditOperation;
        int page = 0;
        int fallbackRow = -1;
        QList<TrackedCommentTarget> batchTargets;
        std::optional<CommentState> previousState;
        std::optional<CommentState> currentState;
        QJsonObject logicalPayload;
        // A decision on proposals in their local sources, one entry per file
        // decided, in the order written. Undo writes every file back.
        struct ProposalSource {
            QString path;
            QString updatedSha256;
            QByteArray previousContents;
            // Other halves of moves (§12) decided with it, in other files.
            QList<AgentProposalSourceChange> otherSources;
        };
        QList<ProposalSource> proposalSources;
        QStringList proposalIds;
        QString proposalAction;
    };

    struct PendingMutation {
        enum Kind {
            Comment,
            ChatGptUrl,
        };

        Kind kind = Comment;
        QString pdfPath;
        QString annotationId;
        QString contents;
        QVariantMap metadata;
        QString chatGptUrl;
        quint64 generation = 0;
    };

    bool resolveDirtyEditor();
    QList<TrackedCommentTarget> selectedCommentTargets() const;
    // Logical ids when every selected row is an anchor-removed comment, otherwise empty.
    QStringList selectedAnchorRemovedIds() const;
    // Overlay entries when every selected row is a "Comment missing" marker, otherwise empty.
    QVariantList selectedMissingCommentMarkers() const;
    // The proposals removed outside Thoughts among the selected rows.
    QStringList selectedRemovedProposalIds() const;
    // The selected rows Accept and Reject act on: pending proposals that are
    // neither broken nor removed. The live-source ones when there are any,
    // otherwise the artifact-only ones of one bundle (the current row's, or
    // the first one's). Every other selected row is left out.
    QModelIndexList decidableSelectedProposalRows(QString *bundleId = nullptr) const;
    QStringList selectedAgentProposalIds(QString *bundleId = nullptr) const;
    bool selectedAgentProposalsAreLive() const;
    bool resolveLiveAgentProposals(const QString &action, QString *error);
    void undoLiveAgentProposalDecision();
    void resolveSelectedAgentProposals(const QString &action);
    void restoreCommentSelection(const QList<TrackedCommentTarget> &targets, const QString &currentPdfPath = QString(), const QString &currentAnnotationId = QString(), bool loadCurrentComment = true);
    void updateSelectionUi();
    void loadComment(const QModelIndex &index);
    void clearComment();
    void selectComment(const QString &pdfPath, const QString &annotationId, int fallbackRow = -1);
    void removeHistoryForPdf(const QString &pdfPath);
    void removeUndoHistoryForPdf(const QString &pdfPath);
    void removeRedoHistoryForPdf(const QString &pdfPath);
    void setOperationPending(bool pending);
    void updateEditorActions();
    void submitCurrentCorrespondenceReview(const QString &action);
    bool currentSourceTarget(QString *sourcePath, int *line, QString *unavailableReason = nullptr) const;
    void updateEditorDirtyState();
    void applyEditorToCurrentRow();
    void applyLogicalPayloadToCurrentRow(const QJsonObject &payload);
    CommentState currentEditorState() const;
    void applyCommentState(const QString &pdfPath, const QString &annotationId, const CommentState &state, bool loadEditor);
    void applyChatGptUrlToCurrentRow(const QString &url);
    bool saveChatGptUrl(bool openAfterSave = false);
    void openChatGptUrl();
    void removeTargetsFromModel(const QList<TrackedCommentTarget> &targets);
    QVariantMap currentMetadataPatch(QString *error = nullptr) const;
    QJsonObject logicalMutationPayload(QJsonObject payload, const CommentState &state, bool deleted) const;
    QJsonObject logicalReplyPayload(const QJsonObject &parentPayload, const QString &contents) const;
    void populateDocumentList(const QString &selectedStem);
    struct DocumentRowPresentation {
        QString toolTip;
        QString iconName;
    };
    DocumentRowPresentation documentRowPresentation(const TrackedCommentDocument &document, bool hasComponents) const;
    void refreshDocumentListSyncPresentations();
    void populateComments();
    QList<QStandardItem *> makeCommentRow(const TrackedCommentDocument &document, const TrackedComment &comment) const;
    void startDocumentReload(const QString &stem);
    void finishDocumentReload(const QString &stem, const std::optional<TrackedCommentDocument> &document);
    void startFullReload(bool discoverIncludedSources = false, const QString &reason = {});
    void finishFullReload(const QList<TrackedCommentDocument> &documents, bool liveSourcesValid = true, std::optional<quint64> generation = std::nullopt,
                          const QByteArray &catalogBaseline = {}, const QMap<QString, QString> &settingsIdentity = {});
    void enqueueMutation(PendingMutation mutation);
    void startNextMutation();
    void finishMutation(const PendingMutation &mutation, const QVariantMap &result);
    bool hasPendingMutationForPath(const QString &path) const;
    bool shouldIgnoreLocalWrite(const QString &path);
    void flushQueuedMutations();
    void refreshWatchPaths();
    void requestCatalogCheck(bool inventory);
    void refreshFileSnapshots();
    void restoreWindowState();
    void saveWindowState();
    // The Library columns' visibility and order, saved whenever the user changes them.
    void restoreLibraryColumns();
    void applyLibraryColumnDefaults();
    void setLibraryColumnVisible(int column, bool visible);
    void saveLibraryColumns();
    void updateCommentSummary();
    void updateDashboardSummary(bool refreshLibrary = true);
    void updateSyncBuildSummary();
    void updateReviewCardsButton(int activeCards, int dueCards);
    // Rebuilds the Library tree now. Background changes call
    // scheduleLibraryTreeUpdate() instead, which coalesces a burst of them
    // into one rebuild and waits while the tree is not on screen.
    void updateLibraryTree();
    void scheduleLibraryTreeUpdate(const QString &reason);
    // Library file name -> why its latest PDF build failed or needs attention.
    QHash<QString, QString> libraryBuildWarnings() const;
    void showLibraryBuildFailure(const QString &name, const QString &message);
    void flushLibraryTreeUpdate();
    bool libraryTreeOnScreen() const;
    bool libraryReadsPending() const;
    // The pass functions work on the given detached project items, or on the
    // tree's own, so a rebuild can fill its rows before they reach the view.
    void updateLibrarySizes(const QList<QTreeWidgetItem *> *detached = nullptr);
    void requestLibraryModificationDates();
    void applyLibraryModificationDates(const QList<QTreeWidgetItem *> *detached = nullptr);
    void applyLibraryReviewCounts(const QList<QTreeWidgetItem *> *detached = nullptr);
    QList<QTreeWidgetItem *> libraryProjectItems(const QList<QTreeWidgetItem *> *detached) const;
    QTimer *m_libraryTreeTimer = nullptr;
    bool m_libraryTreeDirty = false;
    QStringList m_libraryTreeReasons;
    QElapsedTimer m_libraryTreeDirtySince;
    QTreeWidget *libraryRefreshTree() const;
    void publishSyncLibraryRefresh();
    QTreeWidget *m_syncLibraryTree = nullptr;
    QString m_syncLibrarySummary;
    void requestLibraryMonitoringInformation(bool force = false);
    void startLibraryMonitoringRefresh();
    void updateIdleSyncPresentation();
    void scheduleLibrarySearchUpdate();
    void startLibrarySearchUpdate();
    // Source files are indexed only while the search looks through them.
    void scheduleLibrarySourceSearchUpdate(bool immediately = false);
    void startLibrarySourceSearchUpdate();
    void openLibrarySearchHit(const LibrarySearchHit &hit, const QString &query);
    void updateSyncElapsedTimestamp();
    void beginSyncElapsedPresentation(const QDateTime &startedAt);
    void restoreIdlePdfSyncPresentation();

    QString m_docsRoot;
    QList<TrackedCommentDocument> m_documents;
    // Whether a complete Library load has been shown; until then counts would read as zero.
    bool m_documentsLoaded = false;
    QHash<QString, TrackedComment> m_renderedComments;
    QHash<QString, TrackedCommentDocument> m_renderedDocumentMetadata;
    QTimer *m_reloadTimer = nullptr;
    QTimer *m_autoSaveTimer = nullptr;
    QTimer *m_persistenceRetryTimer = nullptr;
    QTimer *m_catalogPollTimer = nullptr;
    bool m_catalogSignatureCheckRunning = false;
    bool m_catalogCheckRequested = false;
    // Canonical source path -> Library PDF filenames published from it.
    QHash<QString, QStringList> m_documentsBySource;
    bool m_documentsBySourceKnown = false;
    bool m_libraryInventoryDirty = true;
    QString m_projectStoreIdentity;
    bool m_initialProposalDiscoveryDone = false;
    QTimer *m_syncElapsedTimer = nullptr;
    QThreadPool *m_persistencePool = nullptr;
    QThreadPool *m_catalogPool = nullptr;
    // The header's Library search: its index is updated on a worker after
    // documents or review cards reload.
    LibrarySearchField *m_librarySearch = nullptr;
    QThreadPool *m_librarySearchPool = nullptr;
    QTimer *m_librarySearchTimer = nullptr;
    QString m_librarySearchIndexPath;
    bool m_librarySearchRunning = false;
    bool m_librarySearchPending = false;
    bool m_librarySearchDocumentsKnown = false;
    bool m_librarySearchCardsKnown = false;
    QTimer *m_librarySourceSearchTimer = nullptr;
    bool m_librarySourceSearchRunning = false;
    bool m_librarySourceSearchDirty = true;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_summaryPage = nullptr;
    QWidget *m_commentsPage = nullptr;
    QWidget *m_notesPage = nullptr;
    // Diary tab: notes with entry_kind "diary", newest first under day headings.
    QWidget *m_diaryPage = nullptr;
    QPlainTextEdit *m_diaryInput = nullptr;
    QTreeWidget *m_diaryList = nullptr;
    QLabel *m_diarySummary = nullptr;
    QString m_pendingDiaryCreateId;
    QSet<QString> m_pendingDiaryEditIds;
    QSet<QString> m_pendingDiaryDeleteIds;
    void populateDiary();
    // The comments on passages come from the projections: rebuilt whenever the rows are.
    void mergeDocumentFeedback();
    QList<QJsonObject> selectedFeedbackRecords() const;
    void resolveSelectedFeedback();
    void editDiaryEntry(const QString &id);
    bool saveDiaryEntryText(const QString &id, const QString &contents);
    struct BrowserState {
        bool notes = false;
        QWidget *widget = nullptr;
        QLineEdit *search = nullptr;
        QComboBox *typeFilter = nullptr;
        QTreeWidget *documentList = nullptr;
        QTreeView *table = nullptr;
        QSortFilterProxyModel *proxy = nullptr;
        QLabel *summary = nullptr;
        QSplitter *splitter = nullptr;
        QSplitter *contentSplitter = nullptr;
        QList<int> contentSizes;
    };
    BrowserState m_commentsBrowser;
    BrowserState m_notesBrowser;
    void activateBrowser(BrowserState &browser);
    void applyBrowserColumns(BrowserState &browser);
    void applyBrowserFilters(BrowserState &browser);
    void resetBrowserFilters();
    void mirrorDocumentNavigation();
    bool m_notesActive = false;
    // Review: Browse cards and Mental Model, as on the phone.
    QTabWidget *m_reviewPage = nullptr;
    ReviewCardsWidget *m_reviewCardsWidget = nullptr;
    MentalModelView *m_mentalModelView = nullptr;
    QLabel *m_librarySummary = nullptr;
    QLabel *m_librarySelectionDetails = nullptr;
    QTreeWidget *m_libraryTree = nullptr;
    QLabel *m_syncBuildSummary = nullptr;
    QProgressBar *m_fedoraStorageProgress = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_typeFilter = nullptr;
    QPushButton *m_reviewCardsButton = nullptr;
    QPushButton *m_agentNotesButton = nullptr;
    QPushButton *m_syncButton = nullptr;
    QPushButton *m_fedoraDetailsButton = nullptr;
    QLabel *m_pdfSyncStatus = nullptr;
    QLabel *m_pdfSyncTimestamp = nullptr;
    QLabel *m_syncLogsLink = nullptr;
    bool m_syncLinkShowsMerge = false;
    QLabel *m_fedoraStorage = nullptr;
    QLabel *m_fedoraStorageCleanup = nullptr;
    QJsonObject m_fedoraServerSummary;
    QHash<QString, QVariantMap> m_sourceMonitoringInformation;
    QSet<QString> m_sourceMonitoringRequests;
    QHash<QString, QStringList> m_libraryMonitoringIdentities;
    QHash<QString, QStringList> m_libraryMonitoringRequestIdentities;
    quint64 m_libraryMonitoringRequestId = 0;
    bool m_libraryMonitoringScheduled = false;
    bool m_libraryMonitoringRunning = false;
    bool m_libraryMonitoringForce = false;
    bool m_libraryMonitoringRefreshAgain = false;
    bool m_applyingLibraryMonitoringBatch = false;
    QHash<QString, QDateTime> m_libraryModificationDates;
    bool m_libraryModificationRequestRunning = false;
    bool m_libraryModificationRequestPending = false;
    qint64 m_fedoraStorageTotalBytes = -1;
    qint64 m_fedoraStorageUsedBytes = -1;
    qint64 m_fedoraStorageAvailableBytes = -1;
    QDateTime m_syncStartedAt;
    QDateTime m_lastSuccessfulSyncAt;
    int m_pendingSyncFileCount = -1;
    bool m_syncSnapshotLoaded = false;
    QJsonObject m_pendingChanges;
    QJsonObject m_latestSyncOperation;
    QString m_idlePdfSyncStatusText;
    QString m_idlePdfSyncStatusDetails;
    QString m_idlePdfSyncTimestampText;
    QString m_automaticSyncNotice;
    QString m_idlePdfSyncTimestampDetails;
    struct PendingDocumentRemoval {
        QString projectRoot;
        QString componentId;
        QString target;
        QList<TrackedCommentTarget> legacyTargets;
    };
    std::optional<PendingDocumentRemoval> m_pendingDocumentRemoval;
    QTreeWidget *m_documentList = nullptr;
    QTreeView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_commentMetadata = nullptr;
    QLabel *m_selectedText = nullptr;
    QPlainTextEdit *m_proposalPreview = nullptr;
    QWidget *m_noteDateFields = nullptr;
    QLineEdit *m_noteDueDate = nullptr;
    QString m_originalDueDate;
    QLineEdit *m_noteVisibleFrom = nullptr;
    QString m_originalVisibleFrom;
    QCheckBox *m_showHiddenTodos = nullptr;
    QCheckBox *m_showEarlierFeedback = nullptr;
    QFormLayout *m_metadataEditorLayout = nullptr;
    QCheckBox *m_locationEnabled = nullptr;
    QLineEdit *m_locationLabel = nullptr;
    QDoubleSpinBox *m_locationLatitude = nullptr;
    QDoubleSpinBox *m_locationLongitude = nullptr;
    QWidget *m_locationCoordinates = nullptr;
    QPlainTextEdit *m_commentEditor = nullptr;
    QWidget *m_noteAttachments = nullptr;
    QPlainTextEdit *m_correspondenceEvidence = nullptr;
    QPushButton *m_saveCommentButton = nullptr;
    QPushButton *m_addNoteButton = nullptr;
    QPushButton *m_replyCommentButton = nullptr;
    QPushButton *m_deleteCommentButton = nullptr;
    QPushButton *m_resolveFeedbackButton = nullptr;
    QPushButton *m_restoreAnchorButton = nullptr;
    QPushButton *m_removeMissingMarkerButton = nullptr;
    QPushButton *m_restoreMissingCommentButton = nullptr;
    QPushButton *m_restoreProposalButton = nullptr;
    QPushButton *m_dismissProposalButton = nullptr;
    QPushButton *m_copyProposalButton = nullptr;
    QPushButton *m_acceptProposalButton = nullptr;
    QPushButton *m_disregardProposalButton = nullptr;
    QPushButton *m_openCodexButton = nullptr;
    QLineEdit *m_chatGptUrlEditor = nullptr;
    QPushButton *m_openChatGptButton = nullptr;
    QWidget *m_assistantActions = nullptr;
    QPushButton *m_openSourceButton = nullptr;
    QPushButton *m_undoButton = nullptr;
    QPushButton *m_redoButton = nullptr;
    QPushButton *m_confirmCorrespondenceButton = nullptr;
    QPushButton *m_flagCorrespondenceButton = nullptr;
    QPushButton *m_recomputeCorrespondenceButton = nullptr;
    QPushButton *m_trackPdfButton = nullptr;
    QSplitter *m_splitter = nullptr;
    QSplitter *m_contentSplitter = nullptr;
    QWidget *m_details = nullptr;
    QPersistentModelIndex m_currentComment;
    QString m_currentPdfPath;
    QString m_currentAnnotationId;
    QString m_originalContents;
    QString m_originalPriority;
    QString m_originalChatGptUrl;
    QString m_pendingChatGptUrl;
    QJsonObject m_originalLocation;
    bool m_originalLocationPresent = false;
    bool m_locationTouched = false;
    QList<HistoryEntry> m_undoHistory;
    QList<HistoryEntry> m_redoHistory;
    bool m_restoredWindowState = false;
    // Set while code moves Library columns, so only the user's moves are saved.
    bool m_libraryColumnsUpdating = false;
    bool m_editorDirty = false;
    bool m_chatGptUrlDirty = false;
    bool m_openChatGptAfterSave = false;
    bool m_operationPending = false;
    bool m_restoreEditorFocusAfterOperation = false;
    bool m_restoreEditorStateAfterOperation = false;
    int m_editorCursorPositionBeforeOperation = 0;
    int m_editorCursorAnchorBeforeOperation = 0;
    int m_editorVerticalScrollBeforeOperation = 0;
    int m_editorHorizontalScrollBeforeOperation = 0;
    bool m_changingSelection = false;
    bool m_updatingCommentModel = false;
    bool m_reloadDeferred = false;
    bool m_fullReloadScheduled = false;
    bool m_discoveryReloadScheduled = false;
    // A catalogue change seen while a full reload ran (startFullReload, "catalog_changed"): the
    // signature it saw. When the running reload started from that same catalogue it already reads
    // it, so no second reload follows (after Sync the first check always saw the files Sync wrote
    // just before the reload, and a second full reload of 3 to 12 s followed every time, 13 Nov).
    bool m_catalogReloadScheduled = false;
    QByteArray m_scheduledCatalogSignature;
    bool m_fullReloadRunning = false;
    QElapsedTimer m_fullReloadElapsed;
    int m_fullReloadTransientRetries = 0;
    bool m_shuttingDown = false;
    QString m_pendingRevealPdfPath;
    // Shown again once the reload a change asked for is in, e.g. after Undo.
    QString m_summaryAfterReload;
    // Until when m_summaryAfterReload may still be shown: an outcome does not come back long after.
    QDeadlineTimer m_summaryAfterReloadDeadline;
    QString m_pendingRevealAnnotationId;
    int m_pendingRevealFallbackRow = -1;
    bool m_mutationRunning = false;
    quint64 m_nextMutationGeneration = 0;
    QSet<QString> m_scheduledDocumentReloads;
    QSet<QString> m_runningDocumentReloads;
    QSet<QString> m_obsoleteDocumentReloads;
    QSet<QString> m_reloadAgainDocuments;
    QSet<QString> m_queuedAgentProposalIds;
    QByteArray m_catalogSignature;
    QHash<QString, QJsonObject> m_librarySourceInventory;
    QHash<QString, QByteArray> m_knownFileSignatures;
    QHash<QString, QByteArray> m_ignoredLocalHashes;
    QHash<QString, PendingMutation> m_queuedMutations;
    QStringList m_mutationOrder;
    QJsonObject m_fedoraProjections;
    QJsonObject m_reviewSourceLocationIndex;
    quint64 m_commentGeneration = 0;
    QJsonObject m_documentSyncPresentations;
    QJsonObject m_aggregateSyncPresentation;
    QDateTime m_pdfStatusCheckedAt;
    QDateTime m_pdfLastSuccessfulBuildAt;
    QString m_pdfStatusRefreshError;
    bool m_syncOperationRunning = false;
    bool m_syncContentDeferred = false;
    bool m_syncRefreshPending = false;
    std::optional<PendingMutation> m_runningMutation;
    QList<TrackedCommentTarget> m_pendingSelection;
    QString m_pendingCurrentPdfPath;
    QString m_pendingCurrentAnnotationId;
};

}

#endif
