// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "sourcefoldernames.h"
#include "okularpart_export.h"
#include "thoughtshome.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

namespace Thoughts
{
class DesktopSyncStore;
class DesktopSyncOutbox;
enum class DesktopSyncStatus;
struct DesktopSyncSnapshot;
struct DesktopTrackedPath;
struct FrozenPublicationBundle;
class SourceChangeTracker;
class SyncTransferLedger;
class SyncClient;
class TraceSseParser;
}

class QNetworkReply;
class QProcessEnvironment;
class QTimer;
class QThreadPool;

namespace Okular
{

namespace Internal
{
enum class SourcePublicationMode {
    None,
    Scoped,
    ProjectScoped,
    Global,
};

struct SourcePublicationDecision {
    SourcePublicationMode mode = SourcePublicationMode::None;
    QStringList targets;
    QHash<QString, QString> projectsByTarget;
    // A global publication normally consumes the complete frozen snapshot.
    // A targeted Sync that also contains a workspace-wide source may select
    // only that target's paths; unrelated pending paths remain durable for a
    // later explicit Sync.
    QStringList selectedPaths;
};

struct OperationBatchDisposition {
    QSet<QString> acknowledged;
    QSet<QString> superseded;
    QSet<QString> logicalConflicts;
    // Refused and dropped: a Dismiss of a proposal that is no longer removed outside Thoughts.
    QSet<QString> refused;
    QString error;
};

struct LogicalCommentMutationDecision {
    QJsonObject payload;
    qint64 baseRevision = -1;
    bool changed = false;
    bool suppressPlacement = false;
    QString error;
};

OKULARPART_EXPORT OperationBatchDisposition classifyOperationResults(const QHash<QString, QString> &expectedEntityTypes, const QJsonArray &results, const QSet<QString> &replyDeletions = {});
OKULARPART_EXPORT LogicalCommentMutationDecision logicalCommentMutationForPlacement(const QJsonObject &logicalPayload, const QJsonObject &placementPayload);
OKULARPART_EXPORT QJsonObject logicalCommentForNewPlacement(const QJsonObject &placement, const QString &collection, const QJsonObject &parent = {});

OKULARPART_EXPORT bool runLocalSourceTool(const QString &executable, const QStringList &arguments, QByteArray *standardOutput, QString *error, int timeoutMs = 120000, int inactivityTimeoutMs = 0);
OKULARPART_EXPORT QProcessEnvironment sourcePublisherProcessEnvironment(const QByteArray &token, const QString &publisherTraceId = {});
OKULARPART_EXPORT QString sourcePublicationTransferId(const QString &target, quint64 sourceGeneration);
OKULARPART_EXPORT bool installAcceptedSourceManifest(const QString &acceptedPath, const QString &manifestPath);
OKULARPART_EXPORT QList<QJsonObject> takeSourcePublisherEvents(QByteArray &buffer, const QByteArray &incoming, bool flush);
OKULARPART_EXPORT QJsonObject publicationIdentityForBuild(const QString &target, const QJsonObject &build, const QJsonObject &currentPublication);
OKULARPART_EXPORT QString operationTraceIdForBuildRun(const QJsonObject &run, const QString &fallback);
OKULARPART_EXPORT QString authoritativeBuildStatus(const QJsonObject &build);
OKULARPART_EXPORT bool sourceManifestIsProjectScoped(const QByteArray &contents);
OKULARPART_EXPORT SourcePublicationDecision sourcePublicationDecision(bool trackerAvailable,
                                                                      bool trackerAuthoritative,
                                                                      bool manifestMatches,
                                                                      Thoughts::DesktopSyncStatus status,
                                                                      const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                      const QString &requestedTarget,
                                                                      const QHash<QString, QString> &knownProjectsByTarget = {});
// Whether discovery now lists a source of a Project's default folder (only
// `onlyOwner`'s, when given) that the source manifest at `manifestPath` lacks:
// a note's source that arrived from another device after the manifest was made.
OKULARPART_EXPORT bool sourceManifestMissesDefaultFolderSources(const QString &docsRoot, const QString &manifestPath, const QString &onlyOwner = QString());
// Whether a pending Project file declares a checked default-folder document
// whose Library PDF does not exist yet. Only a global publication sends that
// document's build inputs; a project-scoped one for another document does not.
OKULARPART_EXPORT bool pendingProjectFileDeclaresUnbuiltDefaultFolderDocument(const QString &docsRoot,
                                                                               const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                               const QSet<QString> &publishedFilenames);
OKULARPART_EXPORT Thoughts::DesktopSyncSnapshot selectedSourcePublicationSnapshot(const Thoughts::DesktopSyncSnapshot &snapshot, const SourcePublicationDecision &decision);
OKULARPART_EXPORT SourcePublicationDecision sourcePublicationWithoutBlockedTargets(SourcePublicationDecision decision,
                                                                                    const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                                    const QSet<QString> &blockedTargets,
                                                                                    const QHash<QString, QString> &knownProjectsByTarget,
                                                                                    const QString &requestedTarget);
OKULARPART_EXPORT Thoughts::FrozenPublicationBundle discoverProjectTargetFrozenPublication(const QString &executable,
                                                                                           const QString &docsRoot,
                                                                                           const QString &stateRoot,
                                                                                           const QString &manifestPath,
                                                                                           const QString &project,
                                                                                           const QString &target,
                                                                                           const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                                           const QList<Thoughts::DesktopTrackedPath> &baselines);
// The tracking metadata of a source Fedora installed at a path this
// computer did not track yet (another device's new file).
OKULARPART_EXPORT QJsonObject installedNewSourceMetadata(const QString &path, const QList<Thoughts::DesktopTrackedPath> &baselines);
OKULARPART_EXPORT QList<Thoughts::DesktopTrackedPath> acceptedDesktopSyncBaselinesForScope(const QString &docsRoot,
                                                                                           const QString &manifestPath,
                                                                                           const QString &sourceStatePath,
                                                                                           const QJsonObject &publication,
                                                                                           const QString &scopeTarget,
                                                                                           const QList<Thoughts::DesktopTrackedPath> &previousBaselines,
                                                                                           QString *error = nullptr);
OKULARPART_EXPORT bool verifyReaderObjectWithCache(Thoughts::DesktopSyncStore *store,
                                                   const QString &path,
                                                   const QString &sha256,
                                                   qint64 expectedSize,
                                                   const QString &kind,
                                                   const QString &semanticRole = QString(),
                                                   const QString &owningPublication = QString(),
                                                   bool *contentRead = nullptr,
                                                   QString *error = nullptr);
OKULARPART_EXPORT QByteArray decodeZstdPublicationDelta(const QByteArray &base, const QByteArray &patch, qint64 expectedSize);
OKULARPART_EXPORT QByteArray reconstructVerifiedPdfDelta(const QString &docsRoot,
                                                         const QString &filename,
                                                         const QString &baseSha256,
                                                         const QByteArray &patch,
                                                         const QString &resultSha256,
                                                         qint64 resultSize);
OKULARPART_EXPORT bool readerPdfDeltaPlanIsUsable(const QJsonObject &plan, const QJsonObject &publication);
}

class FedoraRepository : public QObject
{
    Q_OBJECT

public:
    enum class ManualSyncState {
        Idle,
        Running,
        Succeeded,
        Failed,
    };
    Q_ENUM(ManualSyncState)

    OKULARPART_EXPORT static FedoraRepository *instance(const QString &docsRoot = QString());
    OKULARPART_EXPORT static void exportProjectPdf(const QString &docsRoot, const QJsonObject &project, const QString &destination,
        QObject *owner, std::function<void(const QString &)> progress, std::function<void(bool, const QString &)> completed);
    OKULARPART_EXPORT static void openNoteAttachment(const QString &docsRoot, const QJsonObject &attachment, QObject *owner, std::function<void(const QString &)> failed);
    ~FedoraRepository() override;

    void captureDocument(const QString &pdfStem);
    OKULARPART_EXPORT bool captureCommentRecords(const QString &docsRoot, const QString &pdfStem, const QList<QJsonObject> &records, const QString &verifiedFallbackPdfSha256 = QString(), const QSet<QString> &createdCommentIds = {});
    OKULARPART_EXPORT void syncNow(const QString &traceId = QString(), bool conditionalRenderCheck = true);
    void syncDocument(const QString &target, bool publishSources, const QString &traceId = QString());
    OKULARPART_EXPORT ManualSyncState manualSyncState() const;
    QString manualSyncTarget() const;
    QString manualSyncOperationId() const;
    OKULARPART_EXPORT QString manualSyncMessage() const;
    OKULARPART_EXPORT QString manualSyncDetails() const;
    OKULARPART_EXPORT QString manualSyncErrorCode() const;
    OKULARPART_EXPORT QJsonObject effectiveProjections(bool includeSourceJournals = true, bool includeSourceSnapshot = true) const;
    OKULARPART_EXPORT bool hasProjectionSnapshot() const;
    OKULARPART_EXPORT bool enqueueLogicalComment(const QJsonObject &payload);
    bool enqueueLogicalComments(const QList<QJsonObject> &payloads);
    bool enqueueReviewCorrespondenceReview(const QJsonObject &payload);
    // Queues one pdf_link_override record (linkoverridestore.h) and publishes the new projections.
    OKULARPART_EXPORT bool enqueuePdfLinkOverride(const QJsonObject &record, QString *error = nullptr);
    // Queues one review_card_feedback record (reviewcardfeedback.h) and publishes the new projections.
    OKULARPART_EXPORT bool enqueueReviewCardFeedback(const QJsonObject &record, QString *error = nullptr);
    // Queues one document_feedback record (documentfeedback.h) and publishes the new projections.
    OKULARPART_EXPORT bool enqueueDocumentFeedback(const QJsonObject &record, QString *error = nullptr);
    // Several feedback records in one store write (Change passage: the new record and the old one's tombstone).
    OKULARPART_EXPORT bool enqueueReviewCardFeedbackBatch(const QList<QJsonObject> &records, QString *error = nullptr);
    OKULARPART_EXPORT bool enqueueDocumentFeedbackBatch(const QList<QJsonObject> &records, QString *error = nullptr);
    // Queues one agent_conversation record (agentconversations.h) and publishes the new projections.
    OKULARPART_EXPORT bool enqueueAgentConversation(const QJsonObject &record, QString *error = nullptr);
    // Re-insert the markers of anchor-removed comments (a pure source edit; no
    // logical operation is queued). Each result has id, success, message, placement.
    OKULARPART_EXPORT QVariantList restoreRemovedAnchors(const QStringList &ids);
    // "Comment missing" (docs/comment-missing.md): each marker is {anchor_id, source_file}
    // as listed by the Issues overlay. Remove takes the marker out of its source
    // (verified, rolled back on failure); Restore undeletes the deleted comment or
    // creates one with the marker's id. Each result has anchor_id, success, message.
    OKULARPART_EXPORT QVariantList removeMissingCommentMarkers(const QVariantList &markers);
    OKULARPART_EXPORT QVariantList restoreMissingComments(const QVariantList &markers, const QString &author);
    // Proposals removed outside Thoughts (protocol/agent-proposals.md §10.6).
    // Restore puts the wrapper back with a local source edit (no operation);
    // Dismiss queues the retirement of the server row. Each result has id,
    // success, code, message.
    OKULARPART_EXPORT QVariantList restoreRemovedProposals(const QStringList &ids);
    OKULARPART_EXPORT QVariantList dismissRemovedProposals(const QStringList &ids);
    // desktop-sync.sqlite, which holds the decision claims too.
    OKULARPART_EXPORT QString proposalDecisionDatabase() const;
    QJsonObject pendingChanges() const;
    QJsonObject pendingDocumentChanges(const QString &target) const;
    void publishSources(const QString &buildTarget = QString(), const QString &traceId = QString());
    void publishNewSourceDocument(const QString &project, const QString &buildTarget, const QString &traceId = QString());
    QJsonObject buildActivity(const QString &target) const;
    QJsonObject sourcePublicationActivity() const;
    QVariantMap sourceTrackingDiagnostics() const;
    OKULARPART_EXPORT QJsonObject renderStatus(const QString &target);
    // The status of a source-linked publication that a complete Fedora PDF
    // status (render-state) observed after its publication leaves out: Fedora
    // neither builds nor tracks it (its document is unchecked in its Project),
    // so it is no build target. Empty when render-state cannot tell.
    OKULARPART_EXPORT static QJsonObject renderStatusOutsideRenderState(const QJsonObject &renderState, const QJsonObject &publication);
    void refreshRenderStatus(const QString &target);
    void requestBuildSourceDiff(const QString &buildId);
    void refreshLatestBuildInputs(const QString &target);
    OKULARPART_EXPORT QJsonObject sourceMonitoringInformation(const QString &target);
    OKULARPART_EXPORT void requestSourceMonitoringInformation(const QStringList &targets, std::function<void(const QVariantMap &)> completion);
    void activatePublishedPdf(const QString &target);
    QJsonObject renderState() const;
    void refreshRenderStateAfterExplicitOperation();
    QJsonObject agentProposalBundle() const;
    QString resolveAgentProposals(const QString &bundleId, const QString &action, const QStringList &proposalIds);
    QString undoAgentProposalResolution(const QString &operationId);
    // An empty path prepares the first recorded conflict.
    OKULARPART_EXPORT void requestSourceConflictResolutionBundle(const QString &path = QString());
    OKULARPART_EXPORT QString queueSourceConflictResolution(const QString &conflictId,
                                          qint64 expectedRevision,
                                          const QString &expectedSha256,
                                          const QByteArray &contents);
    void importLibraryPdf(const QString &requestId, const QString &sourcePath, const QString &filename);
    void setLibraryDisplayName(const QString &requestId, const QString &filename, const QString &expectedSha256, const QString &displayName);
    void removeLibraryPdf(const QString &requestId, const QString &filename, const QString &expectedSha256);
    void watchTrace(const QString &traceId, qint64 afterSequence = 0);
    void stopWatchingTrace(const QString &traceId);
    // The server's capabilities (GET /v1/ready): asked once per run, remembered for offline use.
    // @p done runs (in @p context) with the fresh list, or the last known one when the server
    // cannot be asked.
    OKULARPART_EXPORT void queryServerCapabilities(QObject *context, std::function<void(const QStringList &capabilities)> done);
    OKULARPART_EXPORT QStringList knownServerCapabilities() const;
    // Whether the server holds work this computer has not pulled yet (an agent's proposals, a new
    // PDF build, records from another device), from one GET /v2/sync/state; changes nothing locally.
    // @p done gets that answer and a key naming the server state it was computed for.
    OKULARPART_EXPORT void checkRemoteWork(std::function<void(bool pending, const QString &stateKey)> done);

Q_SIGNALS:
    void projectionsChanged(const QJsonObject &projections, qint64 frontier);
    void libraryChanged(const QJsonObject &library);
    void libraryGenerationWillActivate();
    void libraryGenerationActivated(const QStringList &changedPdfs, bool success);
    void pdfProjected(const QString &filename, const QString &path, const QString &sha256);
    void libraryImportFinished(const QString &requestId, const QString &filename, const QString &path, const QString &sha256, bool success, int statusCode, const QString &message);
    void libraryDisplayNameFinished(const QString &requestId, const QString &filename, const QString &sha256, const QString &displayName, bool success, const QString &message);
    void libraryRemovalFinished(const QString &requestId, const QString &filename, const QString &sha256, bool success, const QString &message);
    void manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState state, const QString &message, const QString &details);
    void syncProgressChanged(const QString &traceId, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &message);
    void syncTransferProgressChanged(const QString &traceId, const QJsonObject &absoluteSnapshot);
    void syncJournalEvent(const QString &traceId, const QJsonObject &event);
    void connectionStateChanged(bool online, const QString &message);
    void renderStatusChanged(const QString &target, const QJsonObject &status);
    void buildSourceDiffReady(const QString &buildId, const QJsonObject &diff, const QString &error);
    void buildInputSnapshotChanged(const QString &target, const QJsonObject &snapshot, const QString &error);
    void renderStateChanged(const QJsonObject &state);
    void agentProposalResolutionStatusChanged(const QString &operationId, const QString &status, const QString &message);
    void sourceConflictResolutionBundleReady(const QJsonObject &bundle, const QString &error);
    void sourceConflictResolutionStatusChanged(const QString &operationId, const QString &status, const QString &message);
    void pendingChangesChanged(const QJsonObject &snapshot);

private Q_SLOTS:
    void handleDesktopSyncStoreChanged(const QString &databasePath);

private:
    friend class PartTest;
    friend class DocumentPipelineE2ETest;
    enum class ManualSyncPhase {
        None,
        ServerState,
        LocalReconciliation,
        AuthoringReconciliation,
        SourcePublication,
        DrainOutbox,
        LibraryMutations,
        RemoteChanges,
        ProjectionSnapshot,
        ChangePages,
        RenderCheck,
        WaitBuild,
        Library,
        FinalChangePages,
        DocumentSnapshot,
    };

    struct OKULARPART_EXPORT SyncCoordinator {
        enum class Scope {
            Global,
            Document,
        };

        enum class ServerStatePurpose {
            Initial,
            ReaderRecheck,
        };

        enum class LibraryRefreshState {
            NotRequested,
            Waiting,
            Ready,
        };

        enum class PostLocalWorkAction {
            ConditionalRenderCheck,
            ReaderStateRecheck,
        };

        ManualSyncState state = ManualSyncState::Idle;
        ManualSyncPhase phase = ManualSyncPhase::None;
        Scope scope = Scope::Global;
        QString target;
        bool conditionalRenderCheck = true;
        QString message;
        QString details;
        QString errorCode;
        QString buildFailure;
        QString renderCheckRequestId;
        QString sourcePublicationTraceId;
        ServerStatePurpose serverStatePurpose = ServerStatePurpose::Initial;
        LibraryRefreshState libraryRefreshState = LibraryRefreshState::NotRequested;
        PostLocalWorkAction postLocalWorkAction = PostLocalWorkAction::ConditionalRenderCheck;
        qint64 libraryLatestServerSequence = -1;
        qint64 remoteLatestServerSequence = -1;
        qint64 authoringCheckedSourceFrontier = -1;
        QStringList blockedAuthoringPaths;
        QStringList pendingAuthoringPaths;
        QStringList providerErrors;
        qint64 publishedSourceFrontier = -1;
        qint64 renderCheckedSourceFrontier = -1;
        QJsonObject serverState;
        std::unique_ptr<Thoughts::DesktopSyncSnapshot> sourceSnapshot;
        std::unique_ptr<Thoughts::FrozenPublicationBundle> frozenPublication;
        Internal::SourcePublicationMode sourcePublicationMode = Internal::SourcePublicationMode::None;
        QStringList sourcePublicationTargets;
        QHash<QString, QString> sourcePublicationProjectsByTarget;
        QStringList sourcePublicationSelectedPaths;
        QSet<QString> completedSourcePublicationTargets;
        // The PDFs of the publisher run being prepared or running: the first
        // target and the following ones of the same Project.
        QStringList sourcePublicationBatch;
        // Source publication totals for the Sync log.
        QElapsedTimer sourcePublicationClock;
        int sourcePublicationRuns = 0;
        int sourcePublicationDocuments = 0;
        bool requiresGlobalRenderCheck = false;
        qint64 localReconciliationStartedAtMs = -1;
        bool localFullReconciliationStarted = false;
        // Stored proposal decision claims were checked against the managed sources.
        bool proposalClaimsChecked = false;
        // Each re-entry repeats discovery and publication; a Sync that keeps
        // re-entering is not converging and must stop rather than spin.
        int discoveryReentries = 0;
        // The Desktop journal operation this Sync runs for.
        QString operationId;

        bool isDocument() const
        {
            return scope == Scope::Document;
        }

        bool running() const
        {
            return state == ManualSyncState::Running;
        }

        bool lifecycleIsValid() const
        {
            return running() == (phase != ManualSyncPhase::None);
        }

        bool hasSourcePublication() const
        {
            return sourcePublicationMode == Internal::SourcePublicationMode::Global || !sourcePublicationTargets.isEmpty();
        }

        bool publishesAllSources() const
        {
            return sourcePublicationMode == Internal::SourcePublicationMode::Global;
        }

        QString conditionalRenderTarget() const
        {
            return isDocument() && !requiresGlobalRenderCheck ? target : QStringLiteral("all");
        }

        void setTarget(const QString &filename)
        {
            target = filename;
            scope = filename.isEmpty() ? Scope::Global : Scope::Document;
        }

        // Defined where the snapshot types are complete: an exported class
        // instantiates these in every file that includes this header.
        SyncCoordinator();
        ~SyncCoordinator();
        SyncCoordinator(SyncCoordinator &&) noexcept;
        SyncCoordinator &operator=(SyncCoordinator &&) noexcept;

        void begin(const QString &filename, bool checkRenderedInputs, const QString &traceId);
        void fail(const QString &failureMessage, const QString &failureDetails);
        void succeed();
        void reset();
        void setSourcePublication(const Internal::SourcePublicationDecision &decision);
        void promoteSourcePublicationToGlobal();
        void completeSourcePublication(const QString &publishedTarget);
    };

    struct PreparedLibraryGeneration {
        bool success = false;
        bool alreadyCurrent = false;
        bool reuseExisting = false;
        QJsonObject generation;
        QJsonArray pendingObjects;
        QString error;
    };

    struct ActivatedLibraryGeneration {
        bool success = false;
        QJsonObject generation;
        QString error;
    };

    explicit FedoraRepository(QString docsRoot, QObject *parent = nullptr);
    OKULARPART_EXPORT QJsonObject readRepositoryState() const;
    OKULARPART_EXPORT bool writeRepositoryState(const QJsonObject &state) const;
    QJsonObject withRenderStatusSnapshot(QJsonObject state) const;
    bool writeRenderStatusSnapshot(const QJsonObject &state) const;
    bool writeLibraryIndex(const QJsonObject &library, const QString &directory = QString()) const;
    Q_INVOKABLE bool writeLibraryGenerationMetadata(const QJsonObject &library, const QString &directory) const;
    bool patchLibraryIndexEntry(const QString &filename, const QString &sha256, const QString &kind, bool remove) const;
    bool patchLibraryDisplayName(const QString &filename, const QString &sha256, const QString &displayName) const;
    bool enqueueLibraryMutation(const QJsonObject &mutation);
    QByteArray bearerToken();
    QByteArray sourcePublisherToken();
    bool ensureClient();
    static void exportProjectRevision(FedoraRepository *repository, const QString &id, const QString &revision, const QJsonArray &documentIds, const QString &destination,
        QObject *owner, std::function<void(const QString &)> progress, std::function<void(bool, const QString &)> completed);
    Q_INVOKABLE QJsonObject readerSyncPlanRequest(const QString &target = QString());
    void requestReaderSyncPlan(const QString &target, std::function<void(const QJsonObject &, const QString &)> completion);
    Q_INVOKABLE bool readerPlanAllowsObject(const QJsonObject &plan, const QString &sha256, const QString &kind, const QString &role, const QString &publication = QString(), qint64 expectedSize = -1) const;
    Q_INVOKABLE qint64 readerPlanObjectSize(const QJsonObject &plan, const QString &sha256) const;
    void initializeReaderPlanTransfers(const QJsonObject &plan);
    bool accountReaderMetadataBytes(qint64 bytes, bool complete = false);
    void accountReaderObjectProgress(const QString &sha256, qint64 bytes, bool complete = false, bool rollback = false);
    bool planManualUpload(const QString &itemId, qint64 bytes);
    void accountManualUploadProgress(const QString &itemId, qint64 bytes, bool complete = false, bool rollback = false);
    bool updateManualTransferTotals(const QString &itemId, qint64 uploadedBytes, qint64 downloadedBytes);
    bool commitManualTransferTotals(const QString &itemId, qint64 uploadedBytes, qint64 downloadedBytes);
    void rollbackManualTransferTotals(const QString &itemId);
    void emitReaderTransferSnapshot(bool flush = false);
    void beginManualReaderStateRecheck();
    bool manualReaderStateCanFinish(const QJsonObject &serverState);
    qint64 manualExpectedSourceFrontier(QString *error);
    void startGlobalReaderMetadata();
    void finishGlobalReaderMetadata(const QJsonObject &library);
    void processGlobalReaderLibrary(const QJsonObject &library);
    void verifyReaderPlanState(const QJsonObject &plan, std::function<void(bool, const QString &)> completion);
    void supersedeGlobalReaderPlan(const QString &message);
    void supersedeTargetReaderPlan(const QString &target, const QJsonObject &publication, const QString &message);
    void retryGlobalReaderPlanWithoutPdfDelta(const QString &message);
    void retryTargetReaderPlanWithoutPdfDelta(const QString &target, const QJsonObject &publication, const QString &message);
    bool recordVerifiedReaderObject(const QString &path, const QString &sha256, qint64 size, const QString &kind, const QString &role = QString(), const QString &publication = QString());
    bool recordActivatedReaderGenerationObjects(const QJsonObject &library);
    Q_INVOKABLE void markGlobalReaderCacheCurrent();
    void pullLibraryWithReaderPlan();
    void applyTraceId(const QString &traceId);
    void emitSyncProgress(const QString &phase, qint64 transferredBytes = 0, qint64 totalBytes = 0, const QString &message = QString());
    bool ensureOutboxReady(QString *details = nullptr);
    void refreshLibrary();
    Q_INVOKABLE void refreshAgentProposalBundle(const QJsonObject &library);
    void retireAgentProposalBundleDownloads();
    void finishAgentProposalBundleDownloadDrain();
    Q_INVOKABLE bool activateReaderSourceArtifactsForActiveLibrary(const QJsonObject &library);
    void projectNextPdf();
    PreparedLibraryGeneration prepareLibraryGeneration(const QJsonObject &library);
    Q_INVOKABLE QJsonObject prepareLibraryGenerationResult(const QJsonObject &library);
    Q_INVOKABLE void startPendingLibraryActivation();
    ActivatedLibraryGeneration activateLibraryGeneration(const QJsonObject &generation) const;
    Q_INVOKABLE bool libraryGenerationMatches(const QString &directory, const QJsonObject &generation) const;
    Q_INVOKABLE int garbageCollectLibraryGenerations();
    // Removes superseded Library generations using only the given state and
    // pure file checks, so it may run on a detached thread.
    OKULARPART_EXPORT static int collectObsoleteLibraryGenerations(const QString &docsRoot, const QJsonObject &state, qint64 minimumAgeSeconds);
    void scheduleLibraryGenerationCleanup(int delayMs);
    void emitSyncTiming(const QString &key, const QString &message);
    Q_INVOKABLE void reconcileInterruptedWorkOnStartup();
    static bool directoryMatchesLibraryGeneration(const QString &directory, const QJsonObject &generation, QString *error);
    static bool validateSourceIndex(const QByteArray &contents, const QJsonObject &library, QString *error);
    OKULARPART_EXPORT void finalizeLibraryGeneration(const QJsonObject &generation);
    void startPendingSourcePublication();
    void startNextDeferredTargetedBuild();
    void startManualAuthoringReconciliation(qint64 through);
    QStringList sourcePublicationArguments(const QString &target, const QString &project = QString()) const;
    QString buildRunDirectory(const QString &runId) const;
    Q_INVOKABLE bool applyChangePage(const QJsonObject &page, qint64 expectedCursor);
    Q_INVOKABLE bool applyProjectionSnapshot(const QJsonObject &snapshot);
    Q_INVOKABLE void retireUnusedRepositoryState();
    Q_INVOKABLE bool retainAcknowledgedProjections(const QJsonArray &operations);
    // What the server supports is asked again at the start of every Sync (one GET /v1/ready sets
    // every flag); this forgets the last answer.
    OKULARPART_EXPORT void forgetServerSupport();
    void beginManualSync(const QString &target, bool conditionalRenderCheck, const QString &traceId);
    void continueManualSyncAfterOverleaf();
    void synchronizeLibraryProjects();
    // The default-folder rename (documents/ -> default/): runs the local
    // migration when Fedora's catalogue shows it migrated, or when a previous
    // run was interrupted; reloads this repository's in-memory state after it.
    bool migrateDefaultFoldersIfNeeded(const QJsonObject &librarySnapshot, QString *error, QString *details);
    bool runDefaultFolderMigration(QString *error, QString *details);
    // A document Sync skips the Library project exchange; while this computer
    // still has a legacy default folder it asks Fedora's catalogue first.
    void checkDefaultFolderMigrationForDocumentSync();
    // The source folder naming change (docs-<old> -> docs-<slug>-<hex>):
    // applies Fedora's library-projects source_folder_renames when this
    // computer still has an old folder or state naming one, or resumes an
    // interrupted run; reloads the in-memory state after it.
    bool migrateSourceFoldersIfNeeded(const Thoughts::SourceFolderRenames &renames, QString *error, QString *details);
    bool runSourceFolderMigration(const Thoughts::SourceFolderRenames &renames, QString *error, QString *details);
    bool m_overleafSyncRunning = false;
    QString m_sourceReconciliationProgram;
    void scheduleManualSyncAdvance(int delayMs = 0);
    Q_INVOKABLE void wakeManualSyncAfterBackgroundActivity();
    Q_INVOKABLE void advanceManualSync();
    void checkManualServerState();
    Q_INVOKABLE bool applyManualServerState(const QJsonObject &serverState);
    OKULARPART_EXPORT void resumeManualSyncAfterSourceReconciliation();
    bool manualReaderArtifactsCurrent(const QJsonObject &serverState);
    bool canPullPlanlessRemoteChanges(const QJsonObject &serverState);
    void pullPlanlessManualChangePage();
    void uploadNextManualBatch(const QList<QJsonObject> &operations);
    // Holds back review tag groups, PDF link overrides and answer feedback until the server
    // advertises review_tag_groups_v1 / pdf_link_overrides_v1 / review_card_feedback_v1 / document_feedback_v1 / agent_conversations_v1. Returns true while the capabilities
    // are being asked for; the Sync then continues by itself.
    bool holdUnsupportedOperations(QList<QJsonObject> &operations);
    void recoverManualDeviceSequence();
    Q_INVOKABLE void processNextManualLibraryMutation();
    void uploadSpecialOutboxOperation(const QJsonObject &operation, bool manual);
    void pollAgentProposalResolution(const QString &operationId, bool manual, int attempt = 0);
    Q_INVOKABLE bool persistAppliedAgentProposalOperation(const QJsonObject &operation);
    void pullManualProjectionSnapshot();
    void pullManualChangePage();
    void startManualRenderCheck();
    Q_INVOKABLE bool applyManualRenderCheckResponse(const QJsonObject &response);
    void resumeManualSyncAfterRenderCheck();
    Q_INVOKABLE bool resumeManualSyncAfterConditionalBuild(const QJsonObject &run);
    bool manualConditionalBuildOwnsCurrentSync(const QJsonObject &run) const;
    Q_INVOKABLE bool applyManualConditionalBuildTerminalResult(const QJsonObject &run, const QString &target, const QJsonObject &build, bool succeeded, const QString &message, bool journaled);
    void startManualSourcePublication();
    // The PDFs the next publisher run covers: the first pending target and,
    // unless THOUGHTS_SOURCE_PUBLICATION_BATCH=0, every other pending target
    // of its Project. Empty for a global publication.
    QStringList nextSourcePublicationBatch() const;
    // "source publication: N document(s) in M publisher run(s), T ms" once
    // the publication step of a Sync is complete.
    void reportSourcePublicationTotals();
    void pullDocumentSnapshot();
    Q_INVOKABLE bool applyDocumentSnapshot(const QJsonObject &snapshot);
    OKULARPART_EXPORT QList<QJsonObject> documentOperations(const QString &target, const QList<QJsonObject> &operations) const;
    void failManualSync(const QString &message, const QString &details = QString());
    void deferManualBuildFailure(const QJsonObject &build, const QString &message);
    Q_INVOKABLE void completeManualSync(bool skipRenderStateRefresh = false);
    Thoughts::DesktopSyncStore *desktopSyncStore(QString *error = nullptr);
    OKULARPART_EXPORT bool sourceManifestMatchesAccepted(Thoughts::DesktopSyncStore *store, bool *matches, QString *error = nullptr);
    bool recordAcceptedSourceManifest(Thoughts::DesktopSyncStore *store, bool requirePreviousIdentity, QString *error = nullptr);
    Q_INVOKABLE bool acceptDesktopSourcePublication(const QJsonObject &publication, const QString &scopeTarget = QString(), QString *error = nullptr);
    // One scoped publication covering several PDFs; empty targets: global.
    Q_INVOKABLE bool acceptDesktopSourcePublicationForTargets(const QJsonObject &publication, const QStringList &scopeTargets, QString *error = nullptr);
    void persistManualSyncFrontier();
    void dropInapplicableProposalClaims();
    void emitEffectiveProjections();
    void pollBuilds();
    Q_INVOKABLE QJsonObject preferredActiveBuildForPolling(const QJsonArray &builds, const QJsonObject &runs) const;
    Q_INVOKABLE void startBuildTraceStream(const QString &buildId, const QString &target, const QString &streamTraceId, const QString &operationTraceId, qint64 afterSequence);
    void activateTargetedBuildArtifact(const QString &target, const QJsonObject &build, bool fullLibraryActivation = false);
    void startTargetedPublicationActivation(const QString &target, const QJsonObject &publication);
    void continueTargetedSourceArtifactDownload();
    void downloadTargetedPublicationPdf();
    PreparedLibraryGeneration prepareTargetedLibraryGeneration(const QString &target, const QJsonObject &publication, const QByteArray &pdfContents);
    Q_INVOKABLE QJsonObject prepareTargetedLibraryGenerationResult(const QString &target, const QJsonObject &publication, const QByteArray &pdfContents);
    Q_INVOKABLE void failTargetedBuildActivation(const QString &message, const QString &target = QString());
    Q_INVOKABLE QString localPublicationState(const QString &target, const QJsonObject &publication) const;
    void setLiveRenderStatus(const QString &target, const QJsonObject &status);
    void storeRenderStatus(const QString &target, const QJsonObject &status);
    Q_INVOKABLE void completeLibraryBuildActivationIfReady();
    void applyCompletedLibraryBuildActivations(const QHash<QString, QJsonObject> &completedRuns, const QString &inspectionError = QString());
    void schedulePendingChangesRefresh();
    Q_INVOKABLE void refreshPendingChanges();
    QJsonObject calculatePendingChanges() const;
    void scheduleSourceCommentRefresh();
    OKULARPART_EXPORT void refreshSourceComments();
    QSet<QString> trackedCommentSourceFiles() const;
    void cleanupDeletedCommentAnchors(const QJsonArray &candidates);
    bool affectsPendingChanges(const QString &path) const;
    static QJsonObject reviewProjections(const QJsonObject &projections);

    QString m_docsRoot;
    // Only this docs root is backed by Fedora; tests use a temporary one.
    QString m_fedoraDocsRoot = Thoughts::thoughtsHome();
    QString m_stateRoot;
    QString m_sourceManifestPath;
    mutable QJsonObject m_cachedRenderStatusSnapshot;
    mutable QString m_cachedRenderStatusIdentity;
    mutable QString m_cachedRenderStatusPath;
    mutable QJsonObject m_cachedRepositoryState;
    mutable QString m_cachedRepositoryStatePath;
    mutable qint64 m_cachedRepositoryStateSize = -1;
    mutable qint64 m_cachedRepositoryStateModifiedMs = -1;
    mutable qint64 m_cachedRepositoryStateBirthMs = -1;
    mutable bool m_cachedRepositoryStateValid = false;
    QString m_outboxPath;
    std::unique_ptr<Thoughts::DesktopSyncOutbox> m_outbox;
    std::unique_ptr<Thoughts::DesktopSyncStore> m_desktopSyncStore;
    QString m_desktopSyncStorePath;
    std::unique_ptr<Thoughts::SourceChangeTracker> m_sourceChangeTracker;
    QString m_sourceChangeTrackerRoot;
    qint64 m_sourceTrackingLoadMs = -1;
    QTimer *m_pendingChangesTimer = nullptr;
    // The tracker has reached an authoritative result since it last failed.
    bool m_sourceTrackingSettled = false;
    QJsonObject m_pendingChangesSnapshot;
    QJsonObject m_reviewProjections;
    QElapsedTimer m_publicationTimer;
    // Times the steps of a Sync's "Downloading updated PDFs" phase, which had no breakdown.
    QElapsedTimer m_readerPhaseClock;
    void emitReaderStepTiming(const QString &step);
    bool m_reviewProjectionsKnown = false;
    QJsonObject m_sourceCommentSnapshot;
    QJsonObject m_sourceCommentFileIndex;
    // Markers whose comment no longer exists, from the last complete source refresh.
    QJsonArray m_missingCommentMarkers;
    QTimer *m_sourceCommentTimer = nullptr;
    quint64 m_sourceCommentGeneration = 0;
    bool m_sourceCommentRunning = false;
    std::unique_ptr<Thoughts::SyncClient> m_client;
    QStringList m_serverCapabilities;
    bool m_serverCapabilitiesFresh = false;
    std::unique_ptr<Thoughts::SyncTransferLedger> m_syncTransferLedger;
    QByteArray m_cachedToken;
    QByteArray m_cachedSourcePublisherToken;
    // Why a stored token was not used: it was issued for another server than the configured one.
    QString m_tokenWithheld;
    QString m_sourcePublisherTokenWithheld;
    QString m_activeTraceId;
    bool m_uploading = false;
    int m_deviceSequenceRecoveryAttempts = 0;
    bool m_pulling = false;
    bool m_libraryPulling = false;
    bool m_projectSyncReady = false;
    bool m_projectSyncRunning = false;
    bool m_defaultFolderMigrationChecked = false;
    // The stored catalogue (SourceFolderMigration::catalogueKey) for which a
    // document Sync already asked Fedora about old folder names in this run.
    QString m_folderNamesAskedCatalogue;
    bool m_projectSyncDrainingOutbox = false;
    bool m_readerPlanPulling = false;
    bool m_agentProposalBundlePulling = false;
    bool m_agentProposalBundleDraining = false;
    bool m_agentProposalBundleRestartScheduled = false;
    quint64 m_agentProposalBundleDownloadGeneration = 0;
    QSet<QNetworkReply *> m_agentProposalBundleReplies;
    QJsonObject m_activeAgentProposalBundleLibrary;
    QString m_activeAgentProposalBundlePlanId;
    QJsonObject m_pendingAgentProposalBundleLibrary;
    QString m_pendingAgentProposalBundlePlanId;
    bool m_libraryPreparing = false;
    bool m_libraryActivating = false;
    bool m_projectingPdf = false;
    QJsonObject m_libraryReaderPlan;
    qint64 m_libraryReaderMetadataBytes = 0;
    QString m_activeReaderTransferPlanId;
    QJsonObject m_targetReaderPlan;
    QString m_targetReaderPlanTarget;
    QString m_targetReaderPlanPublicationIdentity;
    int m_libraryReaderPlanRetries = 0;
    int m_libraryReaderObjectRetries = 0;
    int m_targetReaderPlanRetries = 0;
    bool m_libraryPdfDeltaFallbackRequested = false;
    bool m_targetPdfDeltaFallbackRequested = false;
    bool m_importingLibrary = false;
    bool m_renamingLibrary = false;
    bool m_removingLibrary = false;
    bool m_publishingSources = false;
    bool m_pollingBuild = false;
    bool m_traceCapabilityKnown = false;
    bool m_clientReviewSourcesReady = false;
    // Whether the server moves an existing card's markers (client_review_source_replacement_v1).
    bool m_clientReviewSourceReplacementReady = false;
    bool m_clientReviewCardMoveReady = false;
    // Whether this Sync's server accepts review_tag_groups: -1 not asked yet, 0 no, 1 yes.
    int m_reviewTagGroupsSupport = -1;
    // Whether this Sync's server accepts pdf_link_override (pdf_link_overrides_v1): -1 not asked yet, 0 no, 1 yes.
    int m_pdfLinkOverridesSupport = -1;
    // Whether this Sync's server accepts review_card_feedback (review_card_feedback_v1): -1 not asked yet, 0 no, 1 yes.
    int m_reviewCardFeedbackSupport = -1;
    // Whether this Sync's server accepts document_feedback (document_feedback_v1): -1 not asked yet, 0 no, 1 yes.
    int m_documentFeedbackSupport = -1;
    int m_documentFeedbackAttachmentSupport = -1;
    int m_documentFeedbackKindsSupport = -1;
    int m_documentFeedbackDrawingSupport = -1;
    int m_documentFeedbackTextSupport = -1;
    // Whether this Sync's server accepts agent_conversation (agent_conversations_v1): -1 not asked yet, 0 no, 1 yes.
    int m_agentConversationsSupport = -1;
    // Tag groups shown as sliders (a group with "scale"): review_tag_scales_v1.
    int m_reviewTagScaleSupport = -1;
    // Tag groups under a section header (a group with "section"): review_tag_sections_v1.
    int m_reviewTagSectionSupport = -1;
    // Tag groups asked before the answer (a group with "phase"): review_tag_phases_v1.
    int m_reviewTagPhaseSupport = -1;
    // Whether the server reads decision claims (proposal_decisions_v1): -1 unknown.
    int m_proposalDecisionsSupport = -1;
    QSet<QString> m_uploadedReviewSourceObjects;
    bool m_traceEventsSupported = false;
    std::unique_ptr<Thoughts::TraceSseParser> m_buildTraceParser;
    QPointer<QNetworkReply> m_buildTraceReply;
    QSet<QString> m_terminalTraceBuilds;
    QHash<QString, QPointer<QNetworkReply>> m_explicitTraceReplies;
    QHash<QString, int> m_explicitTraceAttempts;
    QSet<QString> m_stoppedTraceWatches;
    bool m_libraryMutationRunning = false;
    // Shared with detached cleanup workers, which may outlive this object.
    std::shared_ptr<std::atomic_bool> m_libraryGarbageCollecting = std::make_shared<std::atomic_bool>(false);
    QString m_targetedBuildTarget;
    QString m_pendingLibraryBuildActivationTarget;
    QJsonObject m_targetActivationPublication;
    QStringList m_targetSourceDownloadQueue;
    QSet<QString> m_targetSourceDownloadsInFlight;
    QHash<QString, int> m_targetDownloadAttempts;
    int m_targetPdfDownloadAttempts = 0;
    bool m_targetPdfDownloading = false;
    QHash<QString, QJsonObject> m_renderStatuses;
    QSet<QString> m_restoredTransientRenderStatuses;
    bool m_libraryActivationCompletionChecking = false;
    bool m_libraryActivationCompletionRecheck = false;
    QSet<QString> m_renderStatusRequests;
    QSet<QString> m_buildInputSnapshotRequests;
    bool m_sourceConflictBundleRequestRunning = false;
    QJsonObject m_renderState;
    bool m_renderStateRequestRunning = false;
    SyncCoordinator m_syncCoordinator;
    bool m_outboxReady = false;
};

}
