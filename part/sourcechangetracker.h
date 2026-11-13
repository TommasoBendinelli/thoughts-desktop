// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "desktopsyncstore.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <optional>

class QTimer;

namespace Thoughts
{

class SourceChangeTracker : public QObject
{
    Q_OBJECT

public:
    SourceChangeTracker(QString rootPath, DesktopSyncStore *store, QObject *parent = nullptr);
    ~SourceChangeTracker() override;

    // A coverage sentinel recursively discovers plausible sources beneath one
    // approved authoring root. Exact dependency parents use ordinary directory
    // descriptors and are watched only as atomic-save sensors.
    static DesktopTrackedPath coverageSentinel(QString path,
                                               QString project,
                                               QStringList affectedTargets);

    QString rootPath() const;
    bool authoritative() const;
    QList<DesktopTrackedPath> trackedPaths() const;
    int debounceInterval() const;
    bool reconciliationInProgress() const;
    // New or changed files seen once and awaiting a stable second look; the
    // tracker rescans them itself after its stability delay.
    bool settlingCandidates() const;
    // Whether the last scan settled everything under this project folder (e.g.
    // "docs-paper"). Unlike authoritative(), a file still settling or
    // unresolved in another project does not count against it.
    bool sourceSettled(const QString &project) const;
    QVariantMap diagnostics() const;

    bool setTrackedPaths(const QList<DesktopTrackedPath> &paths, QString *error = nullptr);
    // Drops "deleted" markers for paths now excluded by .ThoughtIgnore so
    // scans stop revisiting them. Returns the number removed, or -1 on error.
    int pruneIgnoredTombstones(QString *error = nullptr);
    void setDebounceInterval(int milliseconds);
    void setHashHelperForTests(QString executable, int timeoutMs);
    // Kept as a deterministic blocking seam for focused tests. Production
    // callers must request asynchronous reconciliation instead.
    bool reconcileNow(QString *error = nullptr);

public Q_SLOTS:
    void scheduleReconciliation();
    void reconcileAsync();

Q_SIGNALS:
    void reconciliationScheduled();
    void reconciliationFinished();
    void reconciliationFailed(const QString &error);
    void authoritativeChanged(bool authoritative);
    void fileHashed(const QString &path);

private:
    struct CandidateState {
        DesktopTrackedPath observed;
        qint64 firstSeenAtMs = 0;
    };

    struct ScanResult {
        bool success = false;
        QString error;
        QList<DesktopTrackedPath> observed;
        QHash<QString, CandidateState> candidateStates;
        QStringList hashedPaths;
        bool waitingForStableCandidate = false;
        bool discoveredDirectoryAdded = false;
        bool unresolved = false;
        QStringList unresolvedPaths;
        QStringList addedDirectories;
    };

    struct AsyncResult {
        quint64 generation = 0;
        bool obsolete = false;
        bool storeCommitted = false;
        qint64 scanMs = -1;
        qint64 storeMs = -1;
        ScanResult scan;
    };

    static std::optional<DesktopTrackedPath> observedPath(const QString &rootPath,
                                                           const DesktopTrackedPath &tracked,
                                                           const QString &hashExecutable,
                                                           int hashTimeoutMs,
                                                           QString *error,
                                                           QStringList *hashedPaths = nullptr,
                                                           const std::shared_ptr<std::atomic<quint64>> &requestedGeneration = {},
                                                           quint64 generation = 0);
    static ScanResult scanPaths(const QString &rootPath,
                                const QList<DesktopTrackedPath> &trackedPaths,
                                const QHash<QString, CandidateState> &candidateStates,
                                int stabilityDelayMs,
                                const QString &hashExecutable,
                                int hashTimeoutMs,
                                const std::shared_ptr<std::atomic<quint64>> &requestedGeneration = {},
                                quint64 generation = 0);
    void startAsyncReconciliation();
    void finishAsyncReconciliation(const AsyncResult &result);
    void startMetadataProbe(const QList<DesktopTrackedPath> &paths);
    void refreshBackgroundSources();
    bool configureWatcher(QString *error = nullptr);
    bool isRelevantChange(const QString &absolutePath, bool entryChanged) const;
    void setAuthoritative(bool authoritative);
    void recordSettlement(const ScanResult &scan);

    QString m_rootPath;
    QList<DesktopTrackedPath> m_backgroundSources;
    QString m_hashExecutable;
    int m_hashTimeoutMs = 30000;
    DesktopSyncStore *m_store = nullptr;
    // Absolute paths of background sources and their directories; see
    // isRelevantChange().
    QSet<QString> m_interestingPaths;
    QStringList m_sortedInterestingPaths;
    QSet<QString> m_interestingDirectories;
    qsizetype m_watchedDirectoryCount = 0;
    QTimer *m_debounceTimer = nullptr;
    QTimer *m_integrityAuditTimer = nullptr;
    QList<DesktopTrackedPath> m_trackedPaths;
    QHash<QString, CandidateState> m_candidateStates;
    // Paths that kept the last successful scan from being authoritative;
    // unknown (everything unsettled) until a scan completes.
    QStringList m_unsettledPaths;
    bool m_settlementKnown = false;
    std::shared_ptr<std::atomic<quint64>> m_requestedGeneration;
    qsizetype m_integrityAuditIndex = 0;
    bool m_reconciliationInProgress = false;
    bool m_metadataProbeInProgress = false;
    bool m_reconciliationQueued = false;
    bool m_authoritative = false;
    qint64 m_lastSetupMs = -1;
    qint64 m_firstSetupMs = -1;
    qint64 m_lastScanMs = -1;
    qint64 m_firstScanMs = -1;
    qint64 m_lastStoreMs = -1;
    qint64 m_firstStoreMs = -1;
    qint64 m_lastFinishMs = -1;
    qint64 m_lastCompletedAtMs = -1;
    int m_lastHashedCount = 0;
    int m_lastObservedCount = 0;
};

// Waits up to timeoutMs for the trackers' background scans and store
// commits (after their trackers are gone, so the jobs are obsolete). The
// default-folder rename rewrites the store only after these settle.
bool waitForSourceTrackerWork(int timeoutMs);

}
