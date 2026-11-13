// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace Thoughts
{

struct AnnotationSubtypeRepairResult;

// Returns whether an otherwise unknown file can plausibly participate in a
// document source/dependency closure.  Exact manifest paths are always tracked;
// this filter is only for files discovered by directory coverage sensors.
bool isPotentialDesktopSourcePath(const QString &path);
// Directory observations are retained only when every path component is safe
// for source discovery. Exact accepted files may still live beneath an
// excluded directory, but that directory never becomes recursive coverage.
bool isEligibleDesktopSourceDirectory(const QString &path);

enum class DesktopSyncStatus {
    Unknown,
    Clean,
    Pending,
};

struct DesktopTrackedPath {
    QString path;
    bool exists = true;
    qint64 sizeBytes = -1;
    qint64 modifiedAtMs = -1;
    QString sha256;
    QJsonObject metadata;
    QStringList affectedTargets;
};

struct DesktopPendingFile {
    QString path;
    quint64 generation = 0;
    QString action;
    QJsonObject metadata;
    QStringList affectedTargets;
    bool hasTrackedState = false;
    DesktopTrackedPath trackedState;
};

struct DesktopSyncSnapshot {
    quint64 throughGeneration = 0;
    QList<DesktopPendingFile> files;

    bool isEmpty() const;
};

struct DesktopSyncSummary {
    DesktopSyncStatus status = DesktopSyncStatus::Unknown;
    int pendingFileCount = 0;
    QStringList dirtyPaths;
    QStringList dirtyTargets;
    QHash<QString, QList<DesktopPendingFile>> filesByTarget;
};

struct DesktopSyncFrontier {
    QString libraryRevision;
    qint64 sourceFrontier = -1;
    qint64 commentFrontier = -1;
    qint64 annotationFrontier = -1;
    qint64 cursor = -1;
    QJsonObject metadata;
};

struct DesktopVerifiedObject {
    QString sha256;
    qint64 sizeBytes = -1;
    QString kind;
    QString semanticRole;
    QString owningPublication;
    qint64 verifiedAtMs = 0;
    QJsonObject metadata;
};

struct DesktopLogicalOperation {
    QString operationId;
    qint64 deviceSequence = -1;
    QJsonObject operation;
    bool acknowledged = false;
    qint64 createdAtMs = 0;
    qint64 acknowledgedAtMs = 0;
};

struct DesktopLibraryOperation {
    QString operationId;
    quint64 generation = 0;
    QString type;
    QString filename;
    QJsonObject mutation;
    QString state;
    qint64 createdAtMs = 0;
    qint64 completedAtMs = 0;

    bool active() const;
};

struct DesktopLibraryEnqueueResult {
    bool success = false;
    bool queued = false;
    bool cancelledPendingAdd = false;
    DesktopLibraryOperation operation;
    QString error;
};

struct DesktopLegacyLibraryImportResult {
    bool success = false;
    bool alreadyImported = false;
    int importedOperations = 0;
    QString error;
};

struct DesktopLegacyImportResult {
    bool success = false;
    bool alreadyImported = false;
    int importedOperations = 0;
    qint64 cursor = -1;
    QString recoveryCopyPath;
    QString error;
};

class DesktopSyncStore
{
public:
    explicit DesktopSyncStore(QString databasePath);
    ~DesktopSyncStore();

    DesktopSyncStore(const DesktopSyncStore &) = delete;
    DesktopSyncStore &operator=(const DesktopSyncStore &) = delete;

    QString databasePath() const;
    bool open(QString *error = nullptr);
    bool initialized() const;

    // A worker-owned, read-only connection; both lists come from one SQLite snapshot.
    static bool readMonitoringSnapshot(const QString &databasePath, QList<DesktopTrackedPath> *baselines,
                                       DesktopSyncSnapshot *pending, QString *error);

    DesktopSyncSummary summary(QString *error = nullptr) const;
    QStringList dirtyPaths(QString *error = nullptr) const;
    QStringList dirtyTargets(QString *error = nullptr) const;
    QHash<QString, QList<DesktopPendingFile>> pendingFilesByTarget(QString *error = nullptr) const;

    bool markReconciliationRequired(QString *error = nullptr);
    QList<DesktopTrackedPath> baselines(QString *error = nullptr) const;
    QList<DesktopTrackedPath> trackingPaths(QString *error = nullptr) const;
    bool replaceBaselines(const QList<DesktopTrackedPath> &baselines, QString *error = nullptr);
    // Removes accepted and observed "deleted" markers for the given paths.
    // Rows that still exist or have pending changes are kept.
    bool removeTombstones(const QStringList &paths, int *removed = nullptr, QString *error = nullptr);
    bool reconcileStartupMetadata(const QList<DesktopTrackedPath> &observedPaths, bool observationBootstrapComplete, QString *error = nullptr);
    bool reconcileStartupMetadata(const QList<DesktopTrackedPath> &observedPaths, QString *error = nullptr)
    {
        return reconcileStartupMetadata(observedPaths, true, error);
    }
    // Explicit recovery for the pre-contract bootstrap bug. It adopts only
    // unclaimed, no-baseline pending generations whose exact tracked state is
    // already the durable observation. Callers must audit/backup the store
    // before invoking this one-time repair.
    bool adoptLegacyCoverageBootstrapObservations(int expectedCount, int *adoptedCount, QString *error = nullptr);

    bool recordPathChange(const QString &path, const QString &action, const QJsonObject &metadata, const QStringList &affectedTargets, QString *error = nullptr);
    DesktopSyncSnapshot freeze(QString *error = nullptr) const;
    DesktopSyncSnapshot freezeForPublication(const QString &claimId, QString *error = nullptr) const;
    bool acknowledge(const DesktopSyncSnapshot &snapshot, QString *error = nullptr);
    bool acceptBaselinesAndAcknowledge(const QList<DesktopTrackedPath> &baselines, const DesktopSyncSnapshot &snapshot, QString *error = nullptr, const QString &publicationId = QString());
    // Reconcile a terminal source-agent publication after response loss or a
    // process restart. Only exact pending SHA/tombstone matches are accepted;
    // unrelated and newer local generations remain pending.
    int reconcileAppliedSourcePublication(const QJsonObject &publication, QString *error = nullptr);

    DesktopSyncFrontier frontier(QString *error = nullptr) const;
    bool setFrontier(const DesktopSyncFrontier &frontier, QString *error = nullptr);

    std::optional<DesktopVerifiedObject> verifiedObject(const QString &sha256, QString *error = nullptr) const;
    // All receipts in one query; per-object lookups cost ~0.2 ms each for thousands of objects.
    QHash<QString, DesktopVerifiedObject> verifiedObjects(QString *error = nullptr) const;
    bool upsertVerifiedObject(const DesktopVerifiedObject &object, QString *error = nullptr);

    QList<DesktopLogicalOperation> logicalOperations(bool includeAcknowledged = false, QString *error = nullptr) const;
    QJsonObject enqueueLogicalOperation(QJsonObject operation, QString *error = nullptr);
    QList<QJsonObject> enqueueLogicalOperations(QList<QJsonObject> operations, QString *error = nullptr);
    bool reconcileDeviceSequence(qint64 remoteNext, QString *error = nullptr);
    bool appendLogicalOperation(const QJsonObject &operation, QString *error = nullptr);
    bool acknowledgeLogicalOperations(const QStringList &operationIds, QString *error = nullptr);
    QList<DesktopLibraryOperation> libraryOperations(bool includeHistory = false, QString *error = nullptr) const;
    DesktopLibraryEnqueueResult enqueueLibraryOperation(const QJsonObject &mutation);
    std::optional<DesktopLibraryOperation> claimNextLibraryOperation(QString *error = nullptr);
    bool acknowledgeLibraryOperation(const QString &operationId, quint64 generation, QString *error = nullptr);
    bool releaseLibraryOperation(const QString &operationId, quint64 generation, QString *error = nullptr);
    DesktopLegacyLibraryImportResult importLegacyLibraryOperations(const QString &legacyStatePath, const QJsonArray &operations);
    bool advanceCursor(qint64 expectedPrevious, qint64 next, QString *error = nullptr);
    AnnotationSubtypeRepairResult repairBlankAnnotationSubtypes();
    DesktopLegacyImportResult importLegacyOutbox(const QString &legacyPath);

private:
    friend class DesktopSyncOutbox;

    QString m_databasePath;
    QString m_connectionName;
};

class DesktopSyncOutbox
{
public:
    explicit DesktopSyncOutbox(QString databasePath, QString legacyPath = {});

    bool ready(QString *error = nullptr) const;
    QList<QJsonObject> enqueueBatch(QList<QJsonObject> operations, QString *error = nullptr);
    QJsonObject enqueue(QString entityType, QString entityId, QString collection, qint64 baseRevision, QJsonObject payload, QString clientTimestamp);
    QJsonObject enqueueAgentProposalResolution(QString artifactSha256, QString action, QJsonArray proposals);
    QJsonObject enqueueAgentProposalUndo(QString operationId);
    // `proposalDecisions`: the decision claims (protocol/agent-proposals.md §10.4.2)
    // a conflict held back, stored with the resolution so a retry sends the same.
    QJsonObject enqueueSourceConflictResolution(QString conflictId,
                                                qint64 expectedRevision,
                                                QString expectedSha256,
                                                QByteArray contents,
                                                QString clientTimestamp,
                                                QJsonArray proposalDecisions = {});
    QList<QJsonObject> operations(QString *error = nullptr) const;
    bool reconcileDeviceSequence(qint64 remoteNext, QString *error = nullptr);
    AnnotationSubtypeRepairResult repairBlankAnnotationSubtypes();
    bool acknowledge(const QList<QString> &operationIds, QString *error = nullptr);
    qint64 cursor(QString *error = nullptr) const;
    bool advanceCursor(qint64 expectedPrevious, qint64 next, QString *error = nullptr);

    QString databasePath() const;
    QString legacyPath() const;

private:
    bool prepare(QString *error) const;

    mutable DesktopSyncStore m_store;
    QString m_legacyPath;
    mutable bool m_ready = false;
};

}

Q_DECLARE_METATYPE(Thoughts::DesktopSyncStatus)
