#include "../part/verifiedartifactcache.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/desktopsyncstore.h"
#include "../part/sourcechangetracker.h"
#include "../part/docschangemonitor.h"
#include "../part/thoughtignore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <functional>
#include <QTemporaryDir>
#include <QTest>
#include <limits>
#include <QThreadPool>
#include <QTimer>

#include <thoughts/durableoutbox.h>

using namespace Thoughts;

namespace
{

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}

DesktopTrackedPath fileState(const QString &root, const QString &path, const QStringList &targets)
{
    const QString absolutePath = QDir(root).filePath(path);
    const QFileInfo info(absolutePath);
    QFile file(absolutePath);
    DesktopTrackedPath state;
    state.path = path;
    state.exists = info.isFile();
    state.sizeBytes = state.exists ? info.size() : -1;
    state.modifiedAtMs = state.exists ? info.lastModified().toMSecsSinceEpoch() : -1;
    if (state.exists && file.open(QIODevice::ReadOnly)) {
        state.sha256 = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
    }
    state.metadata.insert(QStringLiteral("kind"), QStringLiteral("file"));
    state.affectedTargets = targets;
    return state;
}

DesktopTrackedPath directoryState(const QString &root, const QString &path, const QStringList &targets)
{
    const QFileInfo info(QDir(root).filePath(path));
    DesktopTrackedPath state;
    state.path = path;
    state.exists = info.isDir();
    state.sizeBytes = -1;
    state.modifiedAtMs = state.exists ? info.lastModified().toMSecsSinceEpoch() : -1;
    state.metadata.insert(QStringLiteral("kind"), QStringLiteral("directory"));
    state.affectedTargets = targets;
    return state;
}

QJsonObject logicalOperation(const QString &id, qint64 sequence, const QString &value)
{
    return {
        {QStringLiteral("operation_id"), id},
        {QStringLiteral("device_sequence"), sequence},
        {QStringLiteral("entity_type"), QStringLiteral("logical_comment")},
        {QStringLiteral("payload"), QJsonObject {{QStringLiteral("value"), value}}},
    };
}

QJsonObject libraryOperation(const QString &type, const QString &id, const QString &filename = QStringLiteral("paper.pdf"))
{
    QJsonObject operation {{QStringLiteral("type"), type}, {QStringLiteral("request_id"), id}, {QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}};
    if (type == QLatin1String("add")) {
        operation.insert(QStringLiteral("local_path"), QStringLiteral("/tmp/%1").arg(filename));
    } else if (type == QLatin1String("rename")) {
        operation.insert(QStringLiteral("display_name"), QStringLiteral("Title %1").arg(id));
    } else if (type == QLatin1String("archive")) {
        operation.insert(QStringLiteral("archived"), !id.endsWith(QLatin1Char('0')));
    }
    return operation;
}

}

class DesktopSyncStoreTest : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    void recordStoreChange(const QString &databasePath)
    {
        m_storeChanges.append(databasePath);
    }

private:
    QStringList m_storeChanges;

private Q_SLOTS:
    void docsChangeMonitorReportsFileEventsBelowItsRoot()
    {
        QVERIFY(DocsChangeMonitor::isWithin(QStringLiteral("/docs/a/b.tex"), QStringLiteral("/docs")));
        QVERIFY(DocsChangeMonitor::isWithin(QStringLiteral("/docs"), QStringLiteral("/docs")));
        QVERIFY(!DocsChangeMonitor::isWithin(QStringLiteral("/docs-other/b.tex"), QStringLiteral("/docs")));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        // The temporary folder is reached through /var -> /private/var, so this
        // also proves that resolved event paths map back to the configured root.
        DocsChangeMonitor *monitor = DocsChangeMonitor::forRoot(temporary.path());
        QCOMPARE(DocsChangeMonitor::forRoot(temporary.path() + QStringLiteral("/")), monitor);
        QSignalSpy changed(monitor, &DocsChangeMonitor::pathsChanged);
        QTRY_VERIFY_WITH_TIMEOUT(monitor->active(), 2000);
        const QString source = temporary.filePath(QStringLiteral("docs-demo/tex/source/main.tex"));
        QVERIFY(writeBytes(source, QByteArrayLiteral("one\n")));
        QSaveFile replacement(source);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write(QByteArrayLiteral("two\n"));
        QVERIFY(replacement.commit());
        const auto reported = [&] {
            QStringList paths;
            for (const QList<QVariant> &arguments : std::as_const(changed)) {
                paths += arguments.constFirst().toStringList();
            }
            return paths;
        };
        QTRY_VERIFY_WITH_TIMEOUT(reported().contains(source), 5000);
        const QStringList paths = reported();
        for (const QString &path : paths) {
            QVERIFY2(DocsChangeMonitor::isWithin(path, temporary.path()), qPrintable(path));
        }
        // Batches are deduplicated.
        for (const QList<QVariant> &arguments : std::as_const(changed)) {
            const QStringList batch = arguments.constFirst().toStringList();
            QCOMPARE(QSet<QString>(batch.cbegin(), batch.cend()).size(), batch.size());
        }

        QSignalSpy rescan(monitor, &DocsChangeMonitor::rescanRequired);
        monitor->injectChanges({QStringLiteral("/elsewhere/file.tex")}, true);
        QTRY_COMPARE(rescan.count(), 1);
    }

    void desktopSyncCommitsAnnounceOnlyRealChanges()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString databasePath = temporary.filePath(QStringLiteral("desktop-sync.sqlite"));
        QVERIFY(connectDesktopSyncStoreChanges(this, SLOT(recordStoreChange(QString))));
        DesktopSyncOutbox outbox(databasePath, temporary.filePath(QStringLiteral("outbox.json")));
        QString error;
        QVERIFY2(outbox.ready(&error), qPrintable(error));
        QTest::qWait(50);
        m_storeChanges.clear();

        QVERIFY(!outbox.enqueue(QStringLiteral("logical_comment"), QStringLiteral("comment-one"), QStringLiteral("paper"), 0,
                                QJsonObject {{QStringLiteral("text"), QStringLiteral("hello")}}, QStringLiteral("2026-09-29T12:00:00Z"))
                     .isEmpty());
        QTRY_VERIFY(m_storeChanges.contains(databasePath));

        // Reads and already-completed one-time imports commit empty
        // transactions; they must not wake observers.
        m_storeChanges.clear();
        QVERIFY2(outbox.ready(&error), qPrintable(error));
        QCOMPARE(outbox.operations(&error).size(), 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTest::qWait(100);
        QVERIFY2(m_storeChanges.isEmpty(), qPrintable(m_storeChanges.join(QLatin1Char(','))));
    }

    void backgroundWatchesOnlyDocumentSources()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = QStringLiteral("project/tex/main.tex");
        const QString asset = QStringLiteral("project/assets/plot.png");
        const QString unused = QStringLiteral("project/notes/unused.md");
        QVERIFY(writeBytes(temporary.filePath(source), QByteArrayLiteral("source\n")));
        QVERIFY(writeBytes(temporary.filePath(asset), QByteArrayLiteral("image bytes\n")));
        QVERIFY(writeBytes(temporary.filePath(unused), QByteArrayLiteral("unused\n")));

        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), source, {QStringLiteral("paper.pdf")}),
                                            fileState(temporary.path(), asset, {QStringLiteral("paper.pdf")}),
                                            fileState(temporary.path(), unused, {})}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        tracker.setDebounceInterval(10);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);
        const QVariantMap diagnostics = tracker.diagnostics();
        QCOMPARE(diagnostics.value(QStringLiteral("tracked_paths")).toInt(), 3);
        QCOMPARE(diagnostics.value(QStringLiteral("background_sources")).toInt(), 1);
        QCOMPARE(diagnostics.value(QStringLiteral("watched_files")).toInt(), 1);
        QVERIFY(diagnostics.value(QStringLiteral("scan_ms")).toLongLong() >= 0);
        QCOMPARE(diagnostics.value(QStringLiteral("observed_paths")).toInt(), 3);

        QCOMPARE(diagnostics.value(QStringLiteral("watched_directories")).toInt(), 1);

        QSignalSpy scheduled(&tracker, &SourceChangeTracker::reconciliationScheduled);
        DocsChangeMonitor *monitor = DocsChangeMonitor::forRoot(temporary.path());
        monitor->injectChanges({temporary.filePath(asset), temporary.filePath(unused), temporary.filePath(QStringLiteral("project/assets"))});
        // Contents of a sibling rewritten in place, e.g. a build output.
        monitor->injectChanges({temporary.filePath(QStringLiteral("project/tex/main.aux"))}, false, false);
        QTest::qWait(150);
        QCOMPARE(scheduled.count(), 0);
        // A new entry beside the source (editor swap file, new chapter).
        monitor->injectChanges({temporary.filePath(QStringLiteral("project/tex/.main.tex.swp"))});
        QTRY_COMPARE_WITH_TIMEOUT(scheduled.count(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);
        scheduled.clear();
        // Renaming a directory that contains a source reports only the directory.
        monitor->injectChanges({temporary.filePath(QStringLiteral("project"))});
        QTRY_COMPARE_WITH_TIMEOUT(scheduled.count(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);

        QVERIFY(writeBytes(temporary.filePath(asset), QByteArrayLiteral("changed image bytes\n")));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QCOMPARE(store.summary(&error).dirtyPaths, QStringList {asset});
    }

    void directoryAliasesShareWatchCoverage()
    {
#ifndef Q_OS_UNIX
        QSKIP("Requires directory symbolic links");
#endif
        QTemporaryDir temporary;
        const QString original = QStringLiteral("project/overleaf/generated/tables/main.tex");
        const QString alias = QStringLiteral("project/tex/generated/tables/main.tex");
        const QStringList targets {QStringLiteral("paper.pdf")};
        QVERIFY(writeBytes(temporary.filePath(original), QByteArrayLiteral("Original.\n")));
        QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("project/tex"))));
        QVERIFY(QFile::link(temporary.filePath(QStringLiteral("project/overleaf/generated")),
                            temporary.filePath(QStringLiteral("project/tex/generated"))));
        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), original, targets),
            fileState(temporary.path(), alias, targets),
            directoryState(temporary.path(), QStringLiteral("project/overleaf/generated/tables"), targets),
            directoryState(temporary.path(), QStringLiteral("project/tex/generated/tables"), targets)}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        tracker.setDebounceInterval(10);
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);
        QVERIFY(writeBytes(temporary.filePath(alias), QByteArrayLiteral("Updated through alias.\n")));
        QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths.contains(original), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths.contains(alias), 4000);
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);
    }

    void thoughtIgnoreExcludesDiscoveryAndRetiresTrackedPath()
    {
        QTemporaryDir temporary;
        const QString main = QStringLiteral("docs-demo/tex/source/main.tex");
        const QString hidden = QStringLiteral("docs-demo/tex/source/secret.tex");
        const QString ignore = QStringLiteral("docs-demo/.ThoughtIgnore");
        const QStringList targets {QStringLiteral("demo.pdf")};
        QVERIFY(writeBytes(temporary.filePath(main), QByteArrayLiteral("Main.\n")));
        QVERIFY(writeBytes(temporary.filePath(hidden), QByteArrayLiteral("Private.\n")));
        QVERIFY(writeBytes(temporary.filePath(ignore), QByteArrayLiteral(".git/\ntex/source/secret.tex\n")));
        auto mainState = fileState(temporary.path(), main, targets);
        auto hiddenState = fileState(temporary.path(), hidden, targets);
        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({mainState, hiddenState,
            SourceChangeTracker::coverageSentinel(QStringLiteral("docs-demo/tex/source"), QStringLiteral("docs-demo"), targets)}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QVERIFY(store.summary(&error).dirtyPaths.contains(hidden));
        QVERIFY(!store.summary(&error).dirtyPaths.contains(main));
        const QString newHidden = QStringLiteral("docs-demo/tex/source/private.tex");
        QVERIFY(writeBytes(temporary.filePath(ignore), QByteArrayLiteral(".git/\ntex/source/secret.tex\ntex/source/private.tex\n")));
        QVERIFY(writeBytes(temporary.filePath(newHidden), QByteArrayLiteral("Another private file.\n")));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QVERIFY(!store.summary(&error).dirtyPaths.contains(newHidden));
    }
    void pruneForgetsOnlyIgnoredDeletedPathsWithoutPendingChanges()
    {
        QTemporaryDir temporary;
        const QString main = QStringLiteral("docs-demo/tex/source/main.tex");
        const QString secret = QStringLiteral("docs-demo/tex/source/secret.tex");
        const QString pending = QStringLiteral("docs-demo/tex/source/pending.tex");
        const QString gone = QStringLiteral("docs-demo/tex/source/gone.tex");
        const QString ignore = QStringLiteral("docs-demo/.ThoughtIgnore");
        const QStringList targets {QStringLiteral("demo.pdf")};
        QVERIFY(writeBytes(temporary.filePath(main), QByteArrayLiteral("Main.\n")));
        QVERIFY(writeBytes(temporary.filePath(secret), QByteArrayLiteral("Still on disk.\n")));
        QVERIFY(writeBytes(temporary.filePath(pending), QByteArrayLiteral("Pending.\n")));
        QVERIFY(writeBytes(temporary.filePath(gone), QByteArrayLiteral("Gone.\n")));
        QVERIFY(writeBytes(temporary.filePath(ignore), QByteArrayLiteral(".git/\ntex/source/secret.tex\ntex/source/pending.tex\n")));
        const auto deleted = [&](const QString &path) {
            auto state = fileState(temporary.path(), path, targets);
            state.exists = false;
            state.sha256.clear();
            state.sizeBytes = -1;
            state.modifiedAtMs = -1;
            return state;
        };
        const auto pendingState = deleted(pending);
        QVERIFY(QFile::remove(temporary.filePath(gone)));
        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), main, targets), deleted(secret), pendingState, deleted(gone)}, &error), qPrintable(error));
        QVERIFY2(store.recordPathChange(pending, QStringLiteral("delete"), pendingState.metadata, targets, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));

        QCOMPARE(tracker.pruneIgnoredTombstones(&error), 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QStringList remaining;
        for (const auto &path : store.baselines(&error)) remaining.append(path.path);
        remaining.sort();
        QCOMPARE(remaining, (QStringList {gone, main, pending}));
        QStringList tracked;
        for (const auto &path : tracker.trackedPaths()) tracked.append(path.path);
        QVERIFY(!tracked.contains(secret));
        QVERIFY(tracked.contains(pending));
        QVERIFY(tracked.contains(gone));
        QVERIFY(QFileInfo::exists(temporary.filePath(secret)));
    }
    void thoughtIgnoreAddedAfterConnectionIsDiscovered()
    {
        QTemporaryDir temporary;
        const QString main = QStringLiteral("docs-demo/tex/source/main.tex");
        const QString ignore = QStringLiteral("docs-demo/.ThoughtIgnore");
        QVERIFY(writeBytes(temporary.filePath(main), QByteArrayLiteral("Main.\n")));
        const QStringList targets {QStringLiteral("demo.pdf")};
        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), main, targets),
            SourceChangeTracker::coverageSentinel(QStringLiteral("docs-demo"), QStringLiteral("docs-demo"), targets)}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 8000);
        QVERIFY(writeBytes(temporary.filePath(ignore), QByteArrayLiteral(".git/\n")));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths.contains(ignore), 8000);
    }
    void htmlDiscoveryUsesBuildInputsBesideEditableSources()
    {
        QTemporaryDir temporary;
        const QString main = QStringLiteral("docs-demo/tex/source/main.tex");
        const QString html = QStringLiteral("docs-demo/tex/source/plot.html");
        const QStringList targets {QStringLiteral("demo.pdf")};
        QVERIFY(writeBytes(temporary.filePath(main), QByteArrayLiteral("main")));
        auto source = fileState(temporary.path(), main, targets);
        source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
        source.metadata.insert(QStringLiteral("project"), QStringLiteral("docs-demo"));
        source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
        DesktopSyncStore store(temporary.filePath(QStringLiteral("state.sqlite")));
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.replaceBaselines({source, SourceChangeTracker::coverageSentinel(QStringLiteral("docs-demo/tex/source"), QStringLiteral("docs-demo"), targets)}, &error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY(tracker.setTrackedPaths(store.baselines(&error), &error));
        QVERIFY(tracker.reconcileNow(&error));
        QVERIFY(writeBytes(temporary.filePath(html), QByteArrayLiteral("<!doctype html><p>Plot</p>")));
        QVERIFY(tracker.reconcileNow(&error));
        QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths.contains(html), 5000);
        const auto pending = store.freeze(&error).files;
        const auto found = std::find_if(pending.cbegin(), pending.cend(), [&](const auto &entry) { return entry.path == html; });
        QVERIFY(found != pending.cend());
        QCOMPARE(found->trackedState.metadata.value(QStringLiteral("source_kind")).toString(), QStringLiteral("build_input"));
        QCOMPARE(found->trackedState.metadata.value(QStringLiteral("role")).toString(), QStringLiteral("asset"));
    }
    void verifiedArtifactCacheRejectsSameLengthRewrite()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("artifact"));
        const QByteArray original("{\"value\":1}");
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
        QVERIFY(writeBytes(path, original));
        QByteArray first;
        QVERIFY(Thoughts::readVerifiedArtifact(path, digest, &first));
        QCOMPARE(Thoughts::sourceArtifactJson(first, digest).value(QStringLiteral("value")).toInt(), 1);
        QVERIFY(Thoughts::readVerifiedArtifact(path, digest));
        QVERIFY(writeBytes(path, QByteArray("{\"value\":2}")));
        QVERIFY(!Thoughts::readVerifiedArtifact(path, digest));
        QVERIFY(QFile::remove(path));
        QVERIFY(!Thoughts::readVerifiedArtifact(path, digest));
    }

    void startupReconciliationIsConservativelyUnknown();
    void freezeAndAcknowledgeAreGenerationSafe();
    void frozenRevertRemainsPendingAfterAcceptance();
    void baselinesWaitForAnotherWriter();
    void frozenRevertSurvivesProcessRestart();
    void directorySentinelDetectsNewFiles();
    void fileCreatedWhileClosedIsPendingOnceSettled();
    void unpublishedFileDeletedBeforeSyncLeavesNothingPending();
    void unsettledFileOnlyUnsettlesItsOwnProject();
    void directorySentinelDetectsExactRename();
    void atomicSaveDoesNotCreateDirectoryWork();
    void nestedSentinelsDoNotDuplicateOrHashUnchangedFiles();
    void coverageSentinelDetectsNewSiblingDirectoryAfterRestart();
    void acceptedBaselineReplacementRetainsCoverageObservations();
    void thoughtIgnoreIsTheProjectSourcePolicy();
    void defaultFolderIsASourceFolder();
    void legacyCoverageRepairPreservesBaselineBackedChanges();
    void asynchronousHashTimeoutKeepsEventLoopResponsive();
    void destructionCancelsInFlightReconciliation();
    void asynchronousReconciliationReschedulesNewestGeneration();
    void stableWatcherConfigurationDoesNotReschedule();
    void unresolvedDiscoveryKeepsScopeHintAndUsesSharedSources();
    void detectsClosedAtomicEditsDeletesAndSharedDependencies();
    void groupsUnresolvedPathsAsSharedSources();
    void terminalSourcePublicationRecoveryIsExactAndIdempotent();
    void terminalSourcePublicationRecoveryRejectsReclaimedSameBytes();
    void persistsFrontierAndVerifiedObjects();
    void libraryOperationsCoalesceAndAcknowledgeByGeneration();
    void legacyLibraryImportIsTransactionalIdempotentAndRecoverable();
    void legacyImportIsRecoverableTransactionalAndIdempotent();
    void outboxAdapterIsDurableOutboxCompatible();
    void deviceSequenceReconciliationIsDurableAndPreservesOperations();
    void deviceSequenceReconciliationRollsBackOnExhaustion();
};

void DesktopSyncStoreTest::startupReconciliationIsConservativelyUnknown()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("paper/main.tex"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("baseline\n")));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Unknown);
    const DesktopTrackedPath baseline = fileState(temporary.path(), QStringLiteral("paper/main.tex"), {QStringLiteral("paper.pdf")});
    QVERIFY2(store.replaceBaselines({baseline}, &error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Unknown);
    QCOMPARE(store.baselines(&error).size(), 1);

    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QSignalSpy hashed(&tracker, &SourceChangeTracker::fileHashed);
    QVERIFY(!tracker.authoritative());
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Unknown);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(tracker.authoritative());
    QCOMPARE(hashed.count(), 1);
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    QCOMPARE(store.summary(&error).pendingFileCount, 0);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(hashed.count(), 1);
    {
        DesktopSyncStore reopened(QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite")));
        QVERIFY2(reopened.open(&error), qPrintable(error));
        SourceChangeTracker restarted(temporary.path(), &reopened);
        QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
        QSignalSpy restartedHashed(&restarted, &SourceChangeTracker::fileHashed);
        QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
        QCOMPARE(restartedHashed.count(), 0);
    }
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("changed metadata and contents\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(hashed.count(), 2);
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Pending);
}

void DesktopSyncStoreTest::terminalSourcePublicationRecoveryIsExactAndIdempotent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList targets {QStringLiteral("paper.pdf")};
    const QString appliedPath = QStringLiteral("paper/applied.tex");
    const QString unrelatedPath = QStringLiteral("paper/unrelated.tex");
    const QString deletedPath = QStringLiteral("paper/deleted.tex");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(appliedPath), QByteArrayLiteral("base a\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(unrelatedPath), QByteArrayLiteral("base b\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(deletedPath), QByteArrayLiteral("base c\n")));

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    QString error;
    QString appliedSha256;
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), appliedPath, targets), fileState(temporary.path(), unrelatedPath, targets), fileState(temporary.path(), deletedPath, targets)}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

        QVERIFY(writeBytes(QDir(temporary.path()).filePath(appliedPath), QByteArrayLiteral("published a\n")));
        QVERIFY(writeBytes(QDir(temporary.path()).filePath(unrelatedPath), QByteArrayLiteral("local only b\n")));
        QVERIFY(QFile::remove(QDir(temporary.path()).filePath(deletedPath)));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        Thoughts::DesktopSyncFrontier frontier;
        frontier.metadata.insert(QStringLiteral("source_manifest"), QJsonObject {{QStringLiteral("exists"), true}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("shared_style_v1"), true}});
        QVERIFY2(store.setFrontier(frontier, &error), qPrintable(error));
        const DesktopSyncSnapshot frozen = store.freezeForPublication(QStringLiteral("11111111-1111-4111-8111-111111111111"), &error);
        QCOMPARE(frozen.files.size(), 3);
        QCOMPARE(store.freezeForPublication(QStringLiteral("77777777-7777-4777-8777-777777777777"), &error).files.size(), 3);
        const auto applied = std::find_if(frozen.files.cbegin(), frozen.files.cend(), [&appliedPath](const DesktopPendingFile &file) { return file.path == appliedPath; });
        QVERIFY(applied != frozen.files.cend());
        appliedSha256 = applied->trackedState.sha256;
    }

    const QString deletedSha = QString::fromLatin1(QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
    const QJsonObject publication {
        {QStringLiteral("publication_id"), QStringLiteral("publication-after-response-loss")},
        {QStringLiteral("trace_id"), QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("source_frontier"), 91},
        {QStringLiteral("results"),
         QJsonArray {
             QJsonObject {{QStringLiteral("outcome"), QStringLiteral("applied")}, {QStringLiteral("path"), appliedPath}, {QStringLiteral("sha256"), appliedSha256}},
             QJsonObject {{QStringLiteral("outcome"), QStringLiteral("applied")}, {QStringLiteral("path"), unrelatedPath}, {QStringLiteral("sha256"), QString(64, QLatin1Char('f'))}},
             QJsonObject {{QStringLiteral("outcome"), QStringLiteral("duplicate")}, {QStringLiteral("path"), deletedPath}, {QStringLiteral("sha256"), deletedSha}},
         }},
    };
    // Simulate process restart after Fedora atomically committed the source
    // publication but before Desktop acknowledged its frozen generation.
    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    const int recoveredCount = reopened.reconcileAppliedSourcePublication(publication, &error);
    QVERIFY2(recoveredCount == 2, qPrintable(QStringLiteral("Recovered %1: %2").arg(recoveredCount).arg(error)));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const DesktopSyncSummary recovered = reopened.summary(&error);
    QCOMPARE(recovered.status, DesktopSyncStatus::Unknown);
    QCOMPARE(recovered.pendingFileCount, 1);
    QCOMPARE(recovered.dirtyPaths, QStringList({unrelatedPath}));
    QCOMPARE(reopened.frontier(&error).sourceFrontier, 91);
    QCOMPARE(reopened.frontier(&error).metadata.value(QStringLiteral("last_recovered_source_publication_id")).toString(), QStringLiteral("publication-after-response-loss"));
    QVERIFY(!reopened.frontier(&error).metadata.contains(QStringLiteral("source_manifest")));
    QCOMPARE(reopened.reconcileAppliedSourcePublication(publication, &error), 0);

    // A later local generation with the same path but different bytes must not
    // be retired by replaying the old terminal response.
    SourceChangeTracker restarted(temporary.path(), &reopened);
    QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(appliedPath), QByteArrayLiteral("newer local a\n")));
    QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
    QCOMPARE(reopened.reconcileAppliedSourcePublication(publication, &error), 0);
    QCOMPARE(reopened.freezeForPublication(QStringLiteral("55555555-5555-4555-8555-555555555555"), &error).files.size(), 2);
    QStringList stillPending = reopened.dirtyPaths(&error);
    stillPending.sort();
    QStringList expected {appliedPath, unrelatedPath};
    expected.sort();
    QCOMPARE(stillPending, expected);

    // Returning to byte-for-byte content from an already handled publication
    // is still a new local generation. Even if a later Sync claims it, the old
    // last_publication record must never acknowledge it again.
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(appliedPath), QByteArrayLiteral("published a\n")));
    QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
    const DesktopSyncSnapshot repeatedBytes = reopened.freezeForPublication(QStringLiteral("66666666-6666-4666-8666-666666666666"), &error);
    QVERIFY(std::any_of(repeatedBytes.files.cbegin(), repeatedBytes.files.cend(), [&appliedPath](const DesktopPendingFile &file) { return file.path == appliedPath; }));
    QCOMPARE(reopened.reconcileAppliedSourcePublication(publication, &error), 0);
    stillPending = reopened.dirtyPaths(&error);
    stillPending.sort();
    QCOMPARE(stillPending, expected);
}

void DesktopSyncStoreTest::terminalSourcePublicationRecoveryRejectsReclaimedSameBytes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = QStringLiteral("paper/main.tex");
    const QString absolutePath = QDir(temporary.path()).filePath(path);
    QVERIFY(writeBytes(absolutePath, QByteArrayLiteral("Fedora base\n")));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), path, {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

    const QByteArray publishedBytes("publication P bytes\n");
    QVERIFY(writeBytes(absolutePath, publishedBytes));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    const QString publicationTrace = QStringLiteral("22222222-2222-4222-8222-222222222222");
    QCOMPARE(store.freezeForPublication(publicationTrace, &error).files.size(), 1);

    // Before Desktop receives P's response, a later local edit returns to the
    // same bytes and a different Sync claims that newer generation. A path/SHA
    // match alone would incorrectly erase this new work.
    QVERIFY(writeBytes(absolutePath, QByteArrayLiteral("intermediate local edit\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(writeBytes(absolutePath, publishedBytes));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.freezeForPublication(QStringLiteral("33333333-3333-4333-8333-333333333333"), &error).files.size(), 1);

    const QString publishedSha = QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex());
    const QJsonObject oldPublication {
        {QStringLiteral("publication_id"), QStringLiteral("publication-p")},
        {QStringLiteral("trace_id"), publicationTrace},
        {QStringLiteral("source_frontier"), 17},
        {QStringLiteral("results"), QJsonArray {QJsonObject {{QStringLiteral("outcome"), QStringLiteral("applied")}, {QStringLiteral("path"), path}, {QStringLiteral("sha256"), publishedSha}}}},
    };
    QCOMPARE(store.reconcileAppliedSourcePublication(oldPublication, &error), 0);
    QCOMPARE(store.dirtyPaths(&error), QStringList({path}));
    QCOMPARE(store.frontier(&error).sourceFrontier, -1);
}

void DesktopSyncStoreTest::freezeAndAcknowledgeAreGenerationSafe()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("base\n")));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("first edit\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    const DesktopSyncSnapshot first = store.freeze(&error);
    QCOMPARE(first.files.size(), 1);
    QVERIFY(first.files.constFirst().hasTrackedState);
    QVERIFY(first.files.constFirst().trackedState.metadata.contains(QStringLiteral("_local_ctime_ms")));
    QVERIFY(first.files.constFirst().trackedState.metadata.contains(QStringLiteral("_local_file_identity")));
    const quint64 firstGeneration = first.files.constFirst().generation;

    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.freeze(&error).files.constFirst().generation, firstGeneration);
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("second edit\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    const DesktopSyncSnapshot second = store.freeze(&error);
    QVERIFY(second.files.constFirst().generation > firstGeneration);

    QVERIFY2(store.acknowledge(first, &error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Pending);
    QCOMPARE(store.freeze(&error).files.constFirst().generation, second.files.constFirst().generation);
    QVERIFY2(store.acknowledge(second, &error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    QCOMPARE(store.baselines(&error).constFirst().sha256, second.files.constFirst().trackedState.sha256);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);

    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("third edit\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Pending);
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("second edit\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
}

void DesktopSyncStoreTest::frozenRevertRemainsPendingAfterAcceptance()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QByteArray acceptedA("accepted A\n");
    const QByteArray inFlightB("in-flight generation B\n");
    QVERIFY(writeBytes(sourcePath, acceptedA));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

    QVERIFY(writeBytes(sourcePath, inFlightB));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    const DesktopSyncSnapshot frozenB = store.freeze(&error);
    QCOMPARE(frozenB.files.size(), 1);
    const quint64 generationB = frozenB.files.constFirst().generation;
    const DesktopTrackedPath acceptedB = frozenB.files.constFirst().trackedState;

    // The user restores A while B is already frozen for publication. This is
    // still pending work relative to the server once B is accepted.
    QVERIFY(writeBytes(sourcePath, acceptedA));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    const DesktopSyncSnapshot revertedA = store.freeze(&error);
    QCOMPARE(revertedA.files.size(), 1);
    QVERIFY(revertedA.files.constFirst().generation > generationB);
    QCOMPARE(revertedA.files.constFirst().trackedState.sha256, QString::fromLatin1(QCryptographicHash::hash(acceptedA, QCryptographicHash::Sha256).toHex()));

    QVERIFY2(store.acceptBaselinesAndAcknowledge({acceptedB}, frozenB, &error), qPrintable(error));
    QCOMPARE(store.baselines(&error).constFirst().sha256, acceptedB.sha256);
    const DesktopSyncSnapshot stillPendingA = store.freeze(&error);
    QCOMPARE(stillPendingA.files.size(), 1);
    QCOMPARE(stillPendingA.files.constFirst().generation, revertedA.files.constFirst().generation);
    QCOMPARE(stillPendingA.files.constFirst().trackedState.sha256, revertedA.files.constFirst().trackedState.sha256);

    QVERIFY2(store.acceptBaselinesAndAcknowledge({stillPendingA.files.constFirst().trackedState}, stillPendingA, &error), qPrintable(error));
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
}

void DesktopSyncStoreTest::frozenRevertSurvivesProcessRestart()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QByteArray acceptedA("restart baseline A\n");
    const QByteArray inFlightB("restart in-flight generation B\n");
    QVERIFY(writeBytes(sourcePath, acceptedA));

    QString error;
    DesktopSyncSnapshot frozenB;
    DesktopTrackedPath acceptedB;
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QVERIFY(writeBytes(sourcePath, inFlightB));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        frozenB = store.freeze(&error);
        QCOMPARE(frozenB.files.size(), 1);
        acceptedB = frozenB.files.constFirst().trackedState;
    }

    // The durable claim must survive a crash/restart before local
    // reconciliation observes the revert.
    QVERIFY(writeBytes(sourcePath, acceptedA));
    {
        DesktopSyncStore reopened(databasePath);
        QVERIFY2(reopened.open(&error), qPrintable(error));
        SourceChangeTracker restarted(temporary.path(), &reopened);
        QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
        QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
        const DesktopSyncSnapshot pendingA = reopened.freeze(&error);
        QCOMPARE(pendingA.files.size(), 1);
        QVERIFY(pendingA.files.constFirst().generation > frozenB.files.constFirst().generation);

        QVERIFY2(reopened.acceptBaselinesAndAcknowledge({acceptedB}, frozenB, &error), qPrintable(error));
        QCOMPARE(reopened.baselines(&error).constFirst().sha256, acceptedB.sha256);
        QCOMPARE(reopened.freeze(&error).files.constFirst().generation, pendingA.files.constFirst().generation);
    }

    // A second restart still sees A as pending relative to accepted B.
    DesktopSyncStore finalStore(databasePath);
    QVERIFY2(finalStore.open(&error), qPrintable(error));
    const DesktopSyncSnapshot pendingA = finalStore.freeze(&error);
    QCOMPARE(pendingA.files.size(), 1);
    QCOMPARE(pendingA.files.constFirst().trackedState.sha256, QString::fromLatin1(QCryptographicHash::hash(acceptedA, QCryptographicHash::Sha256).toHex()));
    QVERIFY2(finalStore.acceptBaselinesAndAcknowledge({pendingA.files.constFirst().trackedState}, pendingA, &error), qPrintable(error));
    SourceChangeTracker finalTracker(temporary.path(), &finalStore);
    QVERIFY2(finalTracker.setTrackedPaths(finalStore.trackingPaths(&error), &error), qPrintable(error));
    QVERIFY2(finalTracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(finalStore.summary(&error).status, DesktopSyncStatus::Clean);
}

// 13 Nov: recording the baselines of the sources a Sync installed failed with
// "database is locked" while another connection wrote, and Desktop's base
// stayed behind the publisher's. A transaction that reads first cannot take
// SQLite's write lock later in WAL mode, whatever busy_timeout says; the
// baseline transactions now take it first and wait for it.
void DesktopSyncStoreTest::baselinesWaitForAnotherWriter()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("accepted\n")));
    DesktopSyncStore store(databasePath);
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("installed and edited\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

    // Another writer (the change tracker's thread, a thoughtsctl decision)
    // holds the write lock for a moment while each step starts.
    const auto whileAnotherWriterHoldsTheLock = [&databasePath](const std::function<void()> &step) {
        QSemaphore locked;
        QThread *writer = QThread::create([&] {
            {
                QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("other-writer"));
                other.setDatabaseName(databasePath);
                if (other.open()) {
                    QSqlQuery query(other);
                    query.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
                    query.exec(QStringLiteral("BEGIN IMMEDIATE"));
                    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS other_writer(x INTEGER)"));
                    query.exec(QStringLiteral("INSERT INTO other_writer VALUES(1)"));
                    locked.release();
                    QThread::msleep(300);
                    query.exec(QStringLiteral("COMMIT"));
                } else {
                    locked.release();
                }
            }
            QSqlDatabase::removeDatabase(QStringLiteral("other-writer"));
        });
        writer->start();
        locked.acquire();
        step();
        writer->wait();
        delete writer;
    };
    DesktopSyncSnapshot frozen;
    whileAnotherWriterHoldsTheLock([&] { frozen = store.freezeForPublication(QStringLiteral("11111111-1111-4111-8111-111111111111"), &error); });
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(frozen.files.size(), 1);
    bool accepted = false;
    whileAnotherWriterHoldsTheLock([&] { accepted = store.acceptBaselinesAndAcknowledge({frozen.files.constFirst().trackedState}, frozen, &error); });
    QVERIFY2(accepted, qPrintable(error));
    QCOMPARE(store.baselines(&error).constFirst().sha256, frozen.files.constFirst().trackedState.sha256);
}

void DesktopSyncStoreTest::directorySentinelDetectsNewFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources"))));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/main.tex")), QByteArrayLiteral("main source\n")));
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    DesktopTrackedPath main = fileState(temporary.path(), QStringLiteral("sources/main.tex"), {QStringLiteral("paper.pdf")});
    main.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    main.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    main.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    QVERIFY2(store.replaceBaselines({main, SourceChangeTracker::coverageSentinel(QStringLiteral("sources"), QStringLiteral("paper"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(20);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);

    QSignalSpy scheduled(&tracker, &SourceChangeTracker::reconciliationScheduled);
    QSignalSpy finished(&tracker, &SourceChangeTracker::reconciliationFinished);
    QTest::qWait(10);
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/.new.tex.swp")), QByteArrayLiteral("temporary editor bytes\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/new.tex")), QByteArrayLiteral("new source\n")));
    QTRY_VERIFY_WITH_TIMEOUT(scheduled.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths
                                 == QStringList {QStringLiteral("sources/new.tex")},
                             8000);
    const DesktopSyncSummary summary = store.summary(&error);
    QCOMPARE(summary.status, DesktopSyncStatus::Pending);
    QCOMPARE(summary.pendingFileCount, 1);
    QCOMPARE(summary.dirtyPaths, QStringList {QStringLiteral("sources/new.tex")});
    QCOMPARE(summary.dirtyTargets, QStringList {QStringLiteral("paper.pdf")});
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(store.freeze(&error).files.size() == 1, 8000);
    const DesktopSyncSnapshot snapshot = store.freeze(&error);
    QCOMPARE(snapshot.files.constFirst().action, QStringLiteral("upsert"));
    QCOMPARE(snapshot.files.constFirst().path, QStringLiteral("sources/new.tex"));
    QCOMPARE(snapshot.files.constFirst().metadata.value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
    QCOMPARE(snapshot.files.constFirst().metadata.value(QStringLiteral("source_kind")).toString(), QStringLiteral("source"));
    QCOMPARE(snapshot.files.constFirst().metadata.value(QStringLiteral("project")).toString(), QStringLiteral("paper"));
    QCOMPARE(snapshot.files.constFirst().metadata.value(QStringLiteral("role")).toString(), QStringLiteral("source"));
    QVERIFY(snapshot.files.constFirst().hasTrackedState);
    QVERIFY(!snapshot.files.constFirst().trackedState.sha256.isEmpty());
    const QList<DesktopTrackedPath> trackedPaths = tracker.trackedPaths();
    QVERIFY(std::any_of(trackedPaths.cbegin(), trackedPaths.cend(), [](const DesktopTrackedPath &tracked) { return tracked.path == QLatin1String("sources/new.tex"); }));
    QVERIFY(tracker.authoritative());
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).dirtyPaths,
             QStringList {QStringLiteral("sources/new.tex")});
}

void DesktopSyncStoreTest::fileCreatedWhileClosedIsPendingOnceSettled()
{
    // A file created while Thoughts was closed is found by the startup scan,
    // but only as a candidate until a second look finds it unchanged. Sync
    // waits for settlingCandidates() to clear, then must see it as pending.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources"))));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/main.tex")), QByteArrayLiteral("main source\n")));
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    DesktopTrackedPath main = fileState(temporary.path(), QStringLiteral("sources/main.tex"), {QStringLiteral("paper.pdf")});
    main.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    main.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    main.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    QVERIFY2(store.replaceBaselines({main, SourceChangeTracker::coverageSentinel(QStringLiteral("sources"), QStringLiteral("paper"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    {
        SourceChangeTracker first(temporary.path(), &store);
        QVERIFY2(first.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        QVERIFY2(first.reconcileNow(&error), qPrintable(error));
        QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    }
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/added.tex")), QByteArrayLiteral("added while closed\n")));
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources/support"))));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/notes.md")), QByteArrayLiteral("# Added while closed\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/support/style.css")), QByteArrayLiteral("p { margin: 0; }\n")));
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources/support/fonts"))));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/support/fonts/body.ttf")), QByteArrayLiteral("font bytes")));

    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(50);
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    QSignalSpy finished(&tracker, &SourceChangeTracker::reconciliationFinished);
    tracker.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    QVERIFY(tracker.settlingCandidates() || store.summary(&error).dirtyPaths.contains(QStringLiteral("sources/added.tex")));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.settlingCandidates() && !tracker.reconciliationInProgress(), 5000);
    // Markdown documents, stylesheets, and their fonts are found like TeX.
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.settlingCandidates() && !tracker.reconciliationInProgress(), 5000);
    QStringList dirty = store.summary(&error).dirtyPaths;
    dirty.sort();
    QCOMPARE(dirty, (QStringList {QStringLiteral("sources/added.tex"), QStringLiteral("sources/notes.md"), QStringLiteral("sources/support/fonts/body.ttf"), QStringLiteral("sources/support/style.css")}));
}

void DesktopSyncStoreTest::unpublishedFileDeletedBeforeSyncLeavesNothingPending()
{
    // A file created and deleted between two Syncs was never published, so
    // there is nothing to delete on Fedora. A pending deletion for it could
    // never be published and kept the tracker unauthoritative indefinitely,
    // which hid every review card's source location.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources"))));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/main.tex")), QByteArrayLiteral("main source\n")));
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    DesktopTrackedPath main = fileState(temporary.path(), QStringLiteral("sources/main.tex"), {QStringLiteral("paper.pdf")});
    main.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    main.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    main.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    QVERIFY2(store.replaceBaselines({main, SourceChangeTracker::coverageSentinel(QStringLiteral("sources"), QStringLiteral("paper"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(50);
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);

    const QString added = QDir(temporary.path()).filePath(QStringLiteral("sources/scratch.md"));
    QVERIFY(writeBytes(added, QByteArrayLiteral("# scratch\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.settlingCandidates() && !tracker.reconciliationInProgress(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths.contains(QStringLiteral("sources/scratch.md")), 5000);
    QVERIFY(QFile::remove(added));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.settlingCandidates() && !tracker.reconciliationInProgress(), 5000);
    QVERIFY2(!store.summary(&error).dirtyPaths.contains(QStringLiteral("sources/scratch.md")), qPrintable(store.summary(&error).dirtyPaths.join(QLatin1Char(','))));
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 5000);

    // A deletion already left pending by an earlier version is cleared too.
    QVERIFY2(store.recordPathChange(QStringLiteral("sources/old-scratch.md"), QStringLiteral("delete"),
        QJsonObject {{QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved")}}, {QStringLiteral("paper.pdf")}, &error), qPrintable(error));
    QVERIFY(store.summary(&error).dirtyPaths.contains(QStringLiteral("sources/old-scratch.md")));
    // As in the field: a Sync froze and claimed it, and its claim never cleared.
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!store.freezeForPublication(QStringLiteral("stale-claim"), &error).files.isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.reconciliationInProgress(), 5000);
    QVERIFY(!store.summary(&error).dirtyPaths.contains(QStringLiteral("sources/old-scratch.md")));
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 5000);
}

void DesktopSyncStoreTest::unsettledFileOnlyUnsettlesItsOwnProject()
{
    // A file still settling in one project must not hide the source locations
    // of every other project, as the tracker's global authority does.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QList<DesktopTrackedPath> baselines;
    for (const QString &project : {QStringLiteral("docs-a"), QStringLiteral("docs-b")}) {
        QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(project)));
        const QString main = project + QStringLiteral("/main.tex");
        QVERIFY(writeBytes(QDir(temporary.path()).filePath(main), QByteArrayLiteral("main source\n")));
        DesktopTrackedPath state = fileState(temporary.path(), main, {project + QStringLiteral(".pdf")});
        state.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
        state.metadata.insert(QStringLiteral("project"), project);
        state.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
        baselines << state << SourceChangeTracker::coverageSentinel(project, project, {project + QStringLiteral(".pdf")});
    }
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines(baselines, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY(!tracker.sourceSettled(QStringLiteral("docs-a")));  // unknown before any scan
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(tracker.authoritative());
    QVERIFY(tracker.sourceSettled(QStringLiteral("docs-a")) && tracker.sourceSettled(QStringLiteral("docs-b")));

    // A new file in docs-b waits for a stable second look.
    tracker.setDebounceInterval(60000);
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-b/new.tex")), QByteArrayLiteral("new\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(tracker.settlingCandidates());
    QVERIFY(!tracker.authoritative());
    QVERIFY(tracker.sourceSettled(QStringLiteral("docs-a")));
    QVERIFY(!tracker.sourceSettled(QStringLiteral("docs-b")));
}

void DesktopSyncStoreTest::directorySentinelDetectsExactRename()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString oldPath = QDir(temporary.path()).filePath(QStringLiteral("sources/old.tex"));
    const QString newPath = QDir(temporary.path()).filePath(QStringLiteral("sources/new.tex"));
    QVERIFY(writeBytes(oldPath, QByteArrayLiteral("renamed source\n")));

    DesktopTrackedPath source = fileState(temporary.path(), QStringLiteral("sources/old.tex"), {QStringLiteral("paper.pdf")});
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("fragment"));
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({source, SourceChangeTracker::coverageSentinel(QStringLiteral("sources"), QStringLiteral("paper"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(20);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(QFile::rename(oldPath, newPath));

    QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths == QStringList({QStringLiteral("sources/new.tex"), QStringLiteral("sources/old.tex")}), 3000);
    const DesktopSyncSnapshot snapshot = store.freeze(&error);
    QCOMPARE(snapshot.files.size(), 2);
    const auto added = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(), [](const DesktopPendingFile &file) { return file.path == QLatin1String("sources/new.tex"); });
    const auto removed = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(), [](const DesktopPendingFile &file) { return file.path == QLatin1String("sources/old.tex"); });
    QVERIFY(added != snapshot.files.cend());
    QVERIFY(removed != snapshot.files.cend());
    QCOMPARE(added->action, QStringLiteral("upsert"));
    QCOMPARE(removed->action, QStringLiteral("delete"));
    QCOMPARE(added->metadata.value(QStringLiteral("project")).toString(), QStringLiteral("paper"));
    QCOMPARE(added->metadata.value(QStringLiteral("role")).toString(), QStringLiteral("fragment"));
    QCOMPARE(added->affectedTargets, QStringList {QStringLiteral("paper.pdf")});
    QCOMPARE(added->trackedState.sha256, source.sha256);
    QVERIFY(!store.summary(&error).dirtyPaths.contains(QStringLiteral("sources")));
}

void DesktopSyncStoreTest::atomicSaveDoesNotCreateDirectoryWork()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = QDir(temporary.path()).filePath(QStringLiteral("sources/main.tex"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("before\n")));
    DesktopTrackedPath source = fileState(temporary.path(), QStringLiteral("sources/main.tex"), {QStringLiteral("paper.pdf")});
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({source, directoryState(temporary.path(), QStringLiteral("sources"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(20);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));

    QSaveFile replacement(path);
    QVERIFY(replacement.open(QIODevice::WriteOnly));
    QCOMPARE(replacement.write(QByteArrayLiteral("after\n")), qint64(6));
    QVERIFY(replacement.commit());
    QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths == QStringList {QStringLiteral("sources/main.tex")}, 3000);
    const DesktopSyncSnapshot snapshot = store.freeze(&error);
    QCOMPARE(snapshot.files.size(), 1);
    QCOMPARE(snapshot.files.constFirst().action, QStringLiteral("upsert"));
    QCOMPARE(snapshot.files.constFirst().path, QStringLiteral("sources/main.tex"));
}

void DesktopSyncStoreTest::nestedSentinelsDoNotDuplicateOrHashUnchangedFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/main.tex")), QByteArrayLiteral("main\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/fragments/part.tex")), QByteArrayLiteral("part\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("elsewhere/unrelated.bin")), QByteArrayLiteral("unrelated\n")));

    DesktopTrackedPath main = fileState(temporary.path(), QStringLiteral("sources/main.tex"), {QStringLiteral("paper.pdf")});
    main.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    main.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    main.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    DesktopTrackedPath fragment = fileState(temporary.path(), QStringLiteral("sources/fragments/part.tex"), {QStringLiteral("paper.pdf")});
    fragment.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    fragment.metadata.insert(QStringLiteral("project"), QStringLiteral("paper"));
    fragment.metadata.insert(QStringLiteral("role"), QStringLiteral("fragment"));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines(
                 {main, fragment, directoryState(temporary.path(), QStringLiteral("sources"), {QStringLiteral("paper.pdf")}), directoryState(temporary.path(), QStringLiteral("sources/fragments"), {QStringLiteral("paper.pdf")})}, &error),
             qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QSignalSpy hashed(&tracker, &SourceChangeTracker::fileHashed);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(hashed.count(), 2);
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(hashed.count(), 2);
    QCOMPARE(store.summary(&error).pendingFileCount, 0);
    const QList<DesktopTrackedPath> tracked = store.trackingPaths(&error);
    QVERIFY(std::none_of(tracked.cbegin(), tracked.cend(), [](const DesktopTrackedPath &path) { return path.path == QLatin1String("elsewhere/unrelated.bin"); }));
}

void DesktopSyncStoreTest::coverageSentinelDetectsNewSiblingDirectoryAfterRestart()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString project = QStringLiteral("docs-project");
    const QStringList targets {QStringLiteral("paper.pdf"), QStringLiteral("supplement.pdf")};
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/tex/source/main.tex")), QByteArrayLiteral("main\n")));
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("docs-project/tex/source/preexisting-empty"))));

    DesktopTrackedPath source = fileState(temporary.path(), QStringLiteral("docs-project/tex/source/main.tex"), targets);
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), project);
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    const DesktopTrackedPath coverage = SourceChangeTracker::coverageSentinel(QStringLiteral("docs-project/tex/source"), project, targets);
    QCOMPARE(coverage.metadata.value(QStringLiteral("_tracking_scope")).toString(), QStringLiteral("project"));
    QCOMPARE(coverage.metadata.value(QStringLiteral("_coverage_root")).toString(), QStringLiteral("docs-project/tex/source"));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({source, coverage}, &error), qPrintable(error));
    {
        SourceChangeTracker tracker(temporary.path(), &store);
        tracker.setDebounceInterval(10);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        tracker.reconcileAsync();
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 5000);
        QCOMPARE(store.summary(&error).pendingFileCount, 0);
        const QList<DesktopTrackedPath> persisted = store.trackingPaths(&error);
        const auto seeded = std::find_if(persisted.cbegin(), persisted.cend(), [](const DesktopTrackedPath &path) { return path.path == QLatin1String("docs-project/tex/source/preexisting-empty"); });
        QVERIFY(seeded != persisted.cend());
        QCOMPARE(seeded->metadata.value(QStringLiteral("kind")).toString(), QStringLiteral("directory"));
        QCOMPARE(seeded->metadata.value(QStringLiteral("project")).toString(), project);
        QCOMPARE(seeded->affectedTargets, targets);

        // Match the production acceptance path: replace the authoritative
        // closure with file baselines plus the registered root, while the
        // descendant directory remains an observed coverage sensor only.
        QVERIFY2(store.replaceBaselines({source, coverage}, &error), qPrintable(error));
        QCOMPARE(store.summary(&error).pendingFileCount, 0);
    }

    SourceChangeTracker restarted(temporary.path(), &store);
    restarted.setDebounceInterval(10);
    // Production replaces accepted file baselines after publication. The
    // registered root lets the tracker restore its observed descendant
    // sensors without turning those directories into pending source work.
    QVERIFY2(restarted.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    restarted.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(restarted.authoritative(), 5000);
    QCOMPARE(store.summary(&error).pendingFileCount, 0);

    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/build/ignored.plot")), QByteArrayLiteral("ignored\n")));
    QTest::qWait(300);
    QVERIFY(!store.summary(&error).dirtyPaths.contains(QStringLiteral("docs-project/build/ignored.plot")));

    const QString newPath = QStringLiteral("docs-project/tex/source/new-sibling/new.plot");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(newPath), QByteArrayLiteral("plot\n")));
    QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths == QStringList {newPath}, 6000);
    const DesktopSyncSnapshot pending = store.freeze(&error);
    QCOMPARE(pending.files.size(), 1);
    QCOMPARE(pending.files.constFirst().path, newPath);
    QCOMPARE(pending.files.constFirst().affectedTargets, targets);
    QCOMPARE(pending.files.constFirst().metadata.value(QStringLiteral("project")).toString(), project);
    QCOMPARE(pending.files.constFirst().metadata.value(QStringLiteral("_tracking_scope")).toString(), QStringLiteral("project"));
    QCOMPARE(pending.files.constFirst().metadata.value(QStringLiteral("_coverage_root")).toString(), QStringLiteral("docs-project/tex/source"));
    QCOMPARE(pending.files.constFirst().metadata.value(QStringLiteral("_tracking_resolution")).toString(), QStringLiteral("unresolved"));
    QVERIFY(!restarted.authoritative());
}

void DesktopSyncStoreTest::acceptedBaselineReplacementRetainsCoverageObservations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString project = QStringLiteral("docs-project");
    const QStringList targets {QStringLiteral("paper.pdf")};
    const QString sourcePath = QStringLiteral("docs-project/tex/source/main.tex");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(sourcePath), QByteArrayLiteral("before\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/tex/source/preexisting/seed.plot")), QByteArrayLiteral("seed\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/tex/source/third_party/reference.pdf")), QByteArrayLiteral("reference\n")));
    for (const QString &noise : {QStringLiteral("docs-project/build/main.aux"), QStringLiteral("docs-project/scripts/regenerate.py"), QStringLiteral("docs-project/output/pdf/paper.pdf")}) {
        QVERIFY(writeBytes(QDir(temporary.path()).filePath(noise), QByteArrayLiteral("noise\n")));
    }

    DesktopTrackedPath source = fileState(temporary.path(), sourcePath, targets);
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), project);
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    const DesktopTrackedPath coverage = SourceChangeTracker::coverageSentinel(QStringLiteral("docs-project/tex/source"), project, targets);
    const QList<DesktopTrackedPath> accepted {source, coverage};

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite"));
    DesktopSyncStore store(databasePath);
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines(accepted, &error), qPrintable(error));
    {
        SourceChangeTracker tracker(temporary.path(), &store);
        tracker.setDebounceInterval(5);
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        tracker.reconcileAsync();
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 6000);
        QCOMPARE(store.summary(&error).pendingFileCount, 0);
        const QStringList tracked = [&store, &error] {
            QStringList paths;
            for (const DesktopTrackedPath &path : store.trackingPaths(&error)) {
                paths.append(path.path);
            }
            return paths;
        }();
        QVERIFY(tracked.contains(QStringLiteral("docs-project/tex/source/preexisting/seed.plot")));
        QVERIFY(tracked.contains(QStringLiteral("docs-project/tex/source/third_party/reference.pdf")));
        QVERIFY(!tracked.contains(QStringLiteral("docs-project/build/main.aux")));
        QVERIFY(!tracked.contains(QStringLiteral("docs-project/scripts/regenerate.py")));
        QVERIFY(!tracked.contains(QStringLiteral("docs-project/output/pdf/paper.pdf")));

        QVERIFY(writeBytes(QDir(temporary.path()).filePath(sourcePath), QByteArrayLiteral("accepted\n")));
        QTRY_VERIFY_WITH_TIMEOUT(store.summary(&error).dirtyPaths == QStringList {sourcePath}, 4000);
        const DesktopSyncSnapshot frozen = store.freeze(&error);
        QCOMPARE(frozen.files.size(), 1);
        DesktopTrackedPath acceptedSource = frozen.files.constFirst().trackedState;
        acceptedSource.metadata = source.metadata;
        acceptedSource.affectedTargets = targets;
        QVERIFY2(store.acceptBaselinesAndAcknowledge({acceptedSource, coverage}, frozen, &error), qPrintable(error));
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        tracker.reconcileAsync();
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 6000);
        QCOMPARE(store.summary(&error).pendingFileCount, 0);
    }

    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    SourceChangeTracker restarted(temporary.path(), &reopened);
    restarted.setDebounceInterval(5);
    QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
    restarted.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(restarted.authoritative(), 6000);
    QCOMPARE(reopened.summary(&error).pendingFileCount, 0);

    for (const QString &noise : {QStringLiteral("docs-project/build/late.aux"), QStringLiteral("docs-project/scripts/late.py"), QStringLiteral("docs-project/output/pdf/late.pdf")}) {
        QVERIFY(writeBytes(QDir(temporary.path()).filePath(noise), QByteArrayLiteral("new noise\n")));
    }
    restarted.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(!restarted.reconciliationInProgress(), 6000);
    QCOMPARE(reopened.summary(&error).pendingFileCount, 0);

    const QString added = QStringLiteral("docs-project/tex/source/new-sibling/new.plot");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(added), QByteArrayLiteral("new\n")));
    QTRY_VERIFY_WITH_TIMEOUT(reopened.summary(&error).dirtyPaths == QStringList {added}, 6000);
    const DesktopSyncSnapshot pending = reopened.freeze(&error);
    QCOMPARE(pending.files.size(), 1);
    QCOMPARE(pending.files.constFirst().path, added);
    QCOMPARE(pending.files.constFirst().metadata.value(QStringLiteral("_tracking_resolution")).toString(), QStringLiteral("unresolved"));
}

void DesktopSyncStoreTest::thoughtIgnoreIsTheProjectSourcePolicy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString project = QStringLiteral("docs-project");
    const QStringList targets {QStringLiteral("paper.pdf")};
    const QString sourcePath = QStringLiteral("docs-project/tex/main.tex");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(sourcePath), QByteArrayLiteral("source\n")));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/.ThoughtIgnore")), QByteArrayLiteral("build/\n*.aux\n")));
    // Files the old name heuristics skipped are sources unless .ThoughtIgnore
    // excludes them; ignored ones never become work.
    const QStringList kept {QStringLiteral("docs-project/.ThoughtIgnore"), QStringLiteral("docs-project/build.sh"),
                            QStringLiteral("docs-project/meeting_notes.md"), QStringLiteral("docs-project/scripts/plot.py")};
    for (const QString &path : kept) {
        if (!path.endsWith(QLatin1String(".ThoughtIgnore"))) QVERIFY(writeBytes(QDir(temporary.path()).filePath(path), QByteArrayLiteral("kept\n")));
    }
    for (const QString &noise : {QStringLiteral("docs-project/build/main.pdf"), QStringLiteral("docs-project/tex/main.aux")}) {
        QVERIFY(writeBytes(QDir(temporary.path()).filePath(noise), QByteArrayLiteral("noise\n")));
    }

    DesktopTrackedPath source = fileState(temporary.path(), sourcePath, targets);
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), project);
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    const DesktopTrackedPath coverage = SourceChangeTracker::coverageSentinel(project, project, targets);

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite"));
    QString error;
    const auto trackedPaths = [&error](const DesktopSyncStore &store) {
        QStringList paths;
        for (const DesktopTrackedPath &path : store.trackingPaths(&error)) paths.append(path.path);
        return paths;
    };
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY2(store.replaceBaselines({source, coverage}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        tracker.setDebounceInterval(5);
        QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
        tracker.reconcileAsync();
        QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 6000);
        QCOMPARE(store.summary(&error).pendingFileCount, 0);
        const QStringList tracked = trackedPaths(store);
        for (const QString &path : kept) QVERIFY2(tracked.contains(path), qPrintable(path));
        QVERIFY(!tracked.contains(QStringLiteral("docs-project/build/main.pdf")));
        QVERIFY(!tracked.contains(QStringLiteral("docs-project/tex/main.aux")));
    }

    // The store keeps every observation .ThoughtIgnore allows across a
    // restart, so nothing is dropped and rediscovered as an addition.
    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    for (const QString &path : kept) QVERIFY2(trackedPaths(reopened).contains(path), qPrintable(path));
    SourceChangeTracker restarted(temporary.path(), &reopened);
    restarted.setDebounceInterval(5);
    QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
    restarted.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(restarted.authoritative(), 6000);
    QCOMPARE(reopened.summary(&error).pendingFileCount, 0);

    // A later top-level Markdown file is new source work; a LaTeX build file
    // .ThoughtIgnore excludes is not.
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("docs-project/tex/late.aux")), QByteArrayLiteral("noise\n")));
    const QString added = QStringLiteral("docs-project/notes.md");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(added), QByteArrayLiteral("notes\n")));
    QTRY_VERIFY_WITH_TIMEOUT(reopened.summary(&error).dirtyPaths == QStringList {added}, 6000);
}

void DesktopSyncStoreTest::defaultFolderIsASourceFolder()
{
    // A Project's default folder holds documents like a docs-* folder: its
    // own .ThoughtIgnore (or the built-in build/ and pdf/ rules) decides,
    // while its configuration and imported PDFs keep their own handling.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto owner = [](const char *id) {
        return QStringLiteral(".thoughts/projects/") + QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha256).toHex()) + QStringLiteral("/documents");
    };
    const QString alpha = owner("alpha");
    const QString beta = owner("beta");
    QVERIFY(isPotentialDesktopSourcePath(alpha + QStringLiteral("/notes/chapter.md")));
    QVERIFY(isPotentialDesktopSourcePath(alpha + QStringLiteral("/build.sh")));
    QVERIFY(!isPotentialDesktopSourcePath(alpha + QStringLiteral("/thoughts-project.json")));
    QVERIFY(!isPotentialDesktopSourcePath(alpha + QStringLiteral("/paper.pdf")));
    QVERIFY(isEligibleDesktopSourceDirectory(alpha + QStringLiteral("/notes")));
    // Near misses are not source folders, so the name heuristics apply.
    QVERIFY(!isPotentialDesktopSourcePath(QStringLiteral(".thoughts/projects/") + alpha.mid(19, 64).toUpper() + QStringLiteral("/documents/notes/chapter.md")));
    QVERIFY(!isPotentialDesktopSourcePath(QStringLiteral(".thoughts/projects/") + QString(63, QLatin1Char('a')) + QStringLiteral("/documents/notes/chapter.md")));

    const QStringList kept {alpha + QStringLiteral("/notes.md"), alpha + QStringLiteral("/chapter/main.tex"), beta + QStringLiteral("/notes.md")};
    const QStringList noise {alpha + QStringLiteral("/build/notes.md"), alpha + QStringLiteral("/pdf/notes.md"), alpha + QStringLiteral("/.DS_Store"),
                             alpha + QStringLiteral("/paper.pdf"), alpha + QStringLiteral("/thoughts-project.json")};
    for (const QString &path : kept + noise) QVERIFY(writeBytes(QDir(temporary.path()).filePath(path), QByteArrayLiteral("content\n")));
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite"));
    QString error;
    DesktopSyncStore store(databasePath);
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({SourceChangeTracker::coverageSentinel(alpha, alpha, {}), SourceChangeTracker::coverageSentinel(beta, beta, {})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(5);
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    tracker.reconcileAsync();
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 6000);
    QStringList tracked;
    for (const DesktopTrackedPath &path : store.trackingPaths(&error)) tracked.append(path.path);
    for (const QString &path : kept) QVERIFY2(tracked.contains(path), qPrintable(path));
    for (const QString &path : noise) QVERIFY2(!tracked.contains(path), qPrintable(path));
    // Each default folder is its own project: settling one leaves the other settled.
    QVERIFY(tracker.sourceSettled(alpha) && tracker.sourceSettled(beta));
    tracker.setDebounceInterval(60000);
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(beta + QStringLiteral("/later.md")), QByteArrayLiteral("later\n")));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(tracker.settlingCandidates());
    QVERIFY(tracker.sourceSettled(alpha));
    QVERIFY(!tracker.sourceSettled(beta));

    // The folder's own .ThoughtIgnore replaces the built-in rules.
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(alpha + QStringLiteral("/.ThoughtIgnore")), QByteArrayLiteral("notes.md\n")));
    const ThoughtIgnore own(QDir(temporary.path()).filePath(alpha));
    QVERIFY(own.excludes(QStringLiteral("notes.md")));
    QVERIFY(!own.excludes(QStringLiteral("build"), true));
    const ThoughtIgnore builtIn(QDir(temporary.path()).filePath(beta));
    QVERIFY(builtIn.excludes(QStringLiteral("build"), true) && builtIn.excludes(QStringLiteral("pdf"), true) && builtIn.excludes(QStringLiteral(".DS_Store")));
    QVERIFY(!builtIn.excludes(QStringLiteral("notes.md")));
    // docs-* folders keep their unchanged built-in rules.
    const ThoughtIgnore connected(QDir(temporary.path()).filePath(QStringLiteral("docs-paper")));
    QVERIFY(!connected.excludes(QStringLiteral("build"), true));
}

void DesktopSyncStoreTest::legacyCoverageRepairPreservesBaselineBackedChanges()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList targets {QStringLiteral("paper.pdf")};
    DesktopTrackedPath first;
    first.path = QStringLiteral("docs-project/tex/source/first.tex");
    first.exists = true;
    first.sizeBytes = 1;
    first.modifiedAtMs = 1;
    first.sha256 = QString(64, QLatin1Char('1'));
    first.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("source_kind"), QStringLiteral("source")}};
    first.affectedTargets = targets;
    DesktopTrackedPath second = first;
    second.path = QStringLiteral("docs-project/tex/source/second.tex");
    second.sha256 = QString(64, QLatin1Char('2'));
    DesktopTrackedPath root = SourceChangeTracker::coverageSentinel(QStringLiteral("docs-project"), QStringLiteral("docs-project"), targets);

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({first, second, root}, &error), qPrintable(error));
    QVERIFY2(store.reconcileStartupMetadata({first, second, root}, true, &error), qPrintable(error));

    DesktopTrackedPath changedFirst = first;
    changedFirst.sha256 = QString(64, QLatin1Char('a'));
    DesktopTrackedPath changedSecond = second;
    changedSecond.sha256 = QString(64, QLatin1Char('b'));
    QList<DesktopTrackedPath> observed {changedFirst, changedSecond, root};
    constexpr int pollutedRows = 1958;
    observed.reserve(observed.size() + pollutedRows);
    for (int index = 0; index < pollutedRows; ++index) {
        DesktopTrackedPath noise;
        noise.path = QStringLiteral("docs-project/preexisting/noise-%1.plot").arg(index);
        noise.exists = true;
        noise.sizeBytes = index + 1;
        noise.modifiedAtMs = index + 10;
        noise.sha256 = QString(64, QLatin1Char('c'));
        noise.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")},
                                      {QStringLiteral("_tracking_scope"), QStringLiteral("project")},
                                      {QStringLiteral("_coverage_root"), QStringLiteral("docs-project")},
                                      {QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved")}};
        noise.affectedTargets = targets;
        observed.append(noise);
    }
    QVERIFY2(store.reconcileStartupMetadata(observed, true, &error), qPrintable(error));
    QCOMPARE(store.summary(&error).pendingFileCount, pollutedRows + 2);

    int adopted = 0;
    QVERIFY(!store.adoptLegacyCoverageBootstrapObservations(pollutedRows - 1, &adopted, &error));
    QVERIFY(error.contains(QStringLiteral("expected")));
    QCOMPARE(adopted, 0);
    QCOMPARE(store.summary(&error).pendingFileCount, pollutedRows + 2);
    QVERIFY2(store.adoptLegacyCoverageBootstrapObservations(pollutedRows, &adopted, &error), qPrintable(error));
    QCOMPARE(adopted, pollutedRows);
    const DesktopSyncSnapshot remaining = store.freeze(&error);
    QCOMPARE(remaining.files.size(), 2);
    QCOMPARE(QStringList({remaining.files.at(0).path, remaining.files.at(1).path}), QStringList({first.path, second.path}));
    QCOMPARE(remaining.files.at(0).trackedState.sha256, changedFirst.sha256);
    QCOMPARE(remaining.files.at(1).trackedState.sha256, changedSecond.sha256);
}

void DesktopSyncStoreTest::asynchronousHashTimeoutKeepsEventLoopResponsive()
{
#ifdef Q_OS_WIN
    QSKIP("The slow hash helper is a POSIX shell script.");
#endif
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QString helperPath = QDir(temporary.path()).filePath(QStringLiteral("slow-hash-helper"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("source\n")));
    QVERIFY(writeBytes(helperPath, QByteArrayLiteral("#!/bin/sh\nsleep 5\n")));
    QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    // The database lives beside the source here; let the change stream deliver
    // its creation before the tracker starts listening, as in the application
    // where the stream is running long before any reconciliation.
    DocsChangeMonitor *monitor = DocsChangeMonitor::forRoot(temporary.path());
    QTRY_VERIFY_WITH_TIMEOUT(monitor->active(), 2000);
    QTest::qWait(500);
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setHashHelperForTests(helperPath, 60);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QSignalSpy failure(&tracker, &SourceChangeTracker::reconciliationFailed);
    bool eventLoopAdvanced = false;
    QTimer::singleShot(0, &tracker, [&eventLoopAdvanced] { eventLoopAdvanced = true; });

    QElapsedTimer elapsed;
    elapsed.start();
    tracker.reconcileAsync();
    QVERIFY2(elapsed.elapsed() < 50, "starting reconciliation blocked the UI thread");
    QTRY_VERIFY_WITH_TIMEOUT(eventLoopAdvanced, 500);
    QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 2000);
    QVERIFY2(elapsed.elapsed() < 2000, "hash timeout did not bound reconciliation");
    const QString message = failure.constFirst().constFirst().toString();
    QVERIFY(message.contains(QStringLiteral("main.tex")));
    QVERIFY(message.contains(QStringLiteral("timed out hashing")));
    QVERIFY(message.contains(QStringLiteral("retry")));
    QVERIFY(!tracker.reconciliationInProgress());
    QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Unknown);
}

void DesktopSyncStoreTest::destructionCancelsInFlightReconciliation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QString helperPath = QDir(temporary.path()).filePath(QStringLiteral("blocking-hash-helper"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("source\n")));
    QVERIFY(writeBytes(helperPath, QByteArrayLiteral("#!/bin/sh\nexec /bin/sleep 30\n")));
    QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    auto tracker = std::make_unique<SourceChangeTracker>(temporary.path(), &store);
    tracker->setHashHelperForTests(helperPath, 30000);
    QVERIFY2(tracker->setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    const int globalWorkersBefore = QThreadPool::globalInstance()->activeThreadCount();
    tracker->reconcileAsync();
    QVERIFY(tracker->reconciliationInProgress());
    QTest::qWait(100);
    QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), globalWorkersBefore);

    QElapsedTimer elapsed;
    elapsed.start();
    tracker.reset();
    // Application teardown waits for the global pool. The source tracker uses
    // its own process-lifetime pool so even an uninterruptible File Provider
    // metadata/enumeration call cannot keep QApplication alive.
    QVERIFY2(QThreadPool::globalInstance()->waitForDone(2000), "destroyed tracker left its QtConcurrent scan running");
    QVERIFY2(elapsed.elapsed() < 2000, "destroyed tracker did not cancel its in-flight hash helper promptly");
}

void DesktopSyncStoreTest::asynchronousReconciliationReschedulesNewestGeneration()
{
#ifdef Q_OS_WIN
    QSKIP("The delayed hash helper is a POSIX shell script.");
#endif
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("main.tex"));
    const QString helperPath = QDir(temporary.path()).filePath(QStringLiteral("delayed-hash-helper"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("baseline\n")));
    QVERIFY(writeBytes(helperPath, QByteArrayLiteral("#!/bin/sh\nsleep 0.15\nexec /usr/bin/shasum \"$@\"\n")));
    QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({fileState(temporary.path(), QStringLiteral("main.tex"), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(10);
    tracker.setHashHelperForTests(helperPath, 3000);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QSignalSpy finished(&tracker, &SourceChangeTracker::reconciliationFinished);
    QSignalSpy failure(&tracker, &SourceChangeTracker::reconciliationFailed);
    tracker.reconcileAsync();
    QVERIFY(tracker.reconciliationInProgress());
    QTest::qWait(30);
    const QByteArray latestBytes("latest generation\n");
    QVERIFY(writeBytes(sourcePath, latestBytes));
    tracker.scheduleReconciliation();

    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 5000);
    QCOMPARE(failure.count(), 0);
    QVERIFY(finished.count() >= 1);
    const DesktopSyncSnapshot pending = store.freeze(&error);
    QCOMPARE(pending.files.size(), 1);
    QCOMPARE(pending.files.constFirst().path, QStringLiteral("main.tex"));
    QCOMPARE(pending.files.constFirst().trackedState.sha256, QString::fromLatin1(QCryptographicHash::hash(latestBytes, QCryptographicHash::Sha256).toHex()));
}

void DesktopSyncStoreTest::stableWatcherConfigurationDoesNotReschedule()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString relativePath = QStringLiteral("docs-project/tex/source/main.tex");
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(relativePath), QByteArrayLiteral("source\n")));
    DesktopTrackedPath source = fileState(temporary.path(), relativePath, {QStringLiteral("paper.pdf")});
    source.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
    source.metadata.insert(QStringLiteral("project"), QStringLiteral("docs-project"));
    source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
    const DesktopTrackedPath discovery = SourceChangeTracker::coverageSentinel(
        QStringLiteral("docs-project/tex/source"),
        QStringLiteral("docs-project"),
        {QStringLiteral("paper.pdf")});

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({source, discovery}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(10);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);

    QSignalSpy scheduled(&tracker, &SourceChangeTracker::reconciliationScheduled);
    QSignalSpy finished(&tracker, &SourceChangeTracker::reconciliationFinished);
    QVERIFY2(tracker.setTrackedPaths(store.trackingPaths(&error), &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(tracker.authoritative(), 4000);
    QVERIFY(finished.count() >= 1);
    const int stableScheduledCount = scheduled.count();
    const int stableFinishedCount = finished.count();
    QTest::qWait(900);
    QCOMPARE(scheduled.count(), stableScheduledCount);
    QCOMPARE(finished.count(), stableFinishedCount);
}

void DesktopSyncStoreTest::unresolvedDiscoveryKeepsScopeHintAndUsesSharedSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(QDir().mkpath(QDir(temporary.path()).filePath(QStringLiteral("sources"))));
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.replaceBaselines({SourceChangeTracker::coverageSentinel(QStringLiteral("sources"), QString(), {QStringLiteral("paper.pdf")})}, &error), qPrintable(error));
    SourceChangeTracker tracker(temporary.path(), &store);
    tracker.setDebounceInterval(20);
    QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QVERIFY(writeBytes(QDir(temporary.path()).filePath(QStringLiteral("sources/new/unknown.dat")), QByteArrayLiteral("unknown input\n")));
    // A file outside recorded document dependencies enters the inventory
    // during the full check requested by Sync.
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.reconciliationInProgress(), 3000);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.reconciliationInProgress(), 3000);
    QTest::qWait(30);
    QTRY_VERIFY_WITH_TIMEOUT(!tracker.reconciliationInProgress(), 3000);
    QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
    QCOMPARE(store.summary(&error).dirtyPaths, QStringList {QStringLiteral("sources/new/unknown.dat")});
    const DesktopSyncSummary summary = store.summary(&error);
    QCOMPARE(summary.pendingFileCount, 1);
    QCOMPARE(summary.dirtyTargets, QStringList());
    QCOMPARE(summary.filesByTarget.value(QStringLiteral("Shared sources")).size(), 1);
    const DesktopPendingFile pending = store.freeze(&error).files.constFirst();
    QCOMPARE(pending.affectedTargets, QStringList {QStringLiteral("paper.pdf")});
    QCOMPARE(pending.metadata.value(QStringLiteral("_tracking_resolution")).toString(), QStringLiteral("unresolved"));
    QCOMPARE(pending.metadata.value(QStringLiteral("_affected_target_hints")).toArray(), QJsonArray {QStringLiteral("paper.pdf")});
    QVERIFY(!tracker.authoritative());
}

void DesktopSyncStoreTest::detectsClosedAtomicEditsDeletesAndSharedDependencies()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("project/main.tex"));
    const QString sharedPath = QDir(temporary.path()).filePath(QStringLiteral("project/plots/shared.png"));
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("base source\n")));
    QVERIFY(writeBytes(sharedPath, QByteArrayLiteral("base plot\n")));
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("state/desktop-sync.sqlite"));

    QString error;
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));
        DesktopTrackedPath source = fileState(temporary.path(), QStringLiteral("project/main.tex"), {QStringLiteral("paper.pdf")});
        source.metadata.insert(QStringLiteral("role"), QStringLiteral("source"));
        DesktopTrackedPath shared = fileState(temporary.path(), QStringLiteral("project/plots/shared.png"), {QStringLiteral("paper.pdf"), QStringLiteral("supplement.pdf")});
        shared.metadata.insert(QStringLiteral("role"), QStringLiteral("plot"));
        QVERIFY2(store.replaceBaselines({source, shared}, &error), qPrintable(error));
        SourceChangeTracker tracker(temporary.path(), &store);
        QVERIFY2(tracker.setTrackedPaths(store.baselines(&error), &error), qPrintable(error));
        QVERIFY2(tracker.reconcileNow(&error), qPrintable(error));
        QCOMPARE(store.summary(&error).status, DesktopSyncStatus::Clean);
    }

    // Simulate an atomic editor save and a deletion while Thoughts is closed.
    QSaveFile replacement(sharedPath);
    QVERIFY(replacement.open(QIODevice::WriteOnly));
    QCOMPARE(replacement.write(QByteArrayLiteral("updated plot\n")), qint64(13));
    QVERIFY(replacement.commit());
    QVERIFY(QFile::remove(sourcePath));

    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    SourceChangeTracker restarted(temporary.path(), &reopened);
    QVERIFY2(restarted.setTrackedPaths(reopened.trackingPaths(&error), &error), qPrintable(error));
    QSignalSpy hashed(&restarted, &SourceChangeTracker::fileHashed);
    QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
    QCOMPARE(hashed.count(), 1);
    const DesktopSyncSummary pending = reopened.summary(&error);
    QCOMPARE(pending.status, DesktopSyncStatus::Pending);
    QCOMPARE(pending.pendingFileCount, 2);
    QCOMPARE(pending.dirtyTargets, QStringList({QStringLiteral("paper.pdf"), QStringLiteral("supplement.pdf")}));
    QCOMPARE(pending.filesByTarget.value(QStringLiteral("supplement.pdf")).size(), 1);
    const QList<DesktopPendingFile> pendingFiles = reopened.pendingFilesByTarget(&error).value(QStringLiteral("paper.pdf"));
    QCOMPARE(pendingFiles.size(), 2);
    const auto deleted = std::find_if(pendingFiles.cbegin(), pendingFiles.cend(), [](const DesktopPendingFile &file) { return file.path == QLatin1String("project/main.tex"); });
    QVERIFY(deleted != pendingFiles.cend());
    QCOMPARE(deleted->action, QStringLiteral("delete"));

    // Returning both files to their accepted content clears the queue without
    // manufacturing a new publication generation.
    QVERIFY(writeBytes(sourcePath, QByteArrayLiteral("base source\n")));
    QVERIFY(writeBytes(sharedPath, QByteArrayLiteral("base plot\n")));
    QVERIFY2(restarted.reconcileNow(&error), qPrintable(error));
    QCOMPARE(reopened.summary(&error).status, DesktopSyncStatus::Clean);
    QCOMPARE(reopened.summary(&error).pendingFileCount, 0);
}

void DesktopSyncStoreTest::groupsUnresolvedPathsAsSharedSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    QVERIFY2(store.recordPathChange(QStringLiteral("project/new-unknown.sty"),
                                    QStringLiteral("upsert"),
                                    QJsonObject {{QStringLiteral("role"), QStringLiteral("source")}, {QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved")}},
                                    {QStringLiteral("paper.pdf")},
                                    &error),
             qPrintable(error));
    const DesktopSyncSummary summary = store.summary(&error);
    QCOMPARE(summary.filesByTarget.value(QStringLiteral("Shared sources")).size(), 1);
    QVERIFY(!summary.dirtyTargets.contains(QStringLiteral("Shared sources")));
    const QList<DesktopTrackedPath> tracked = store.trackingPaths(&error);
    QCOMPARE(tracked.size(), 1);
    QCOMPARE(tracked.constFirst().path, QStringLiteral("project/new-unknown.sty"));
    QCOMPARE(tracked.constFirst().metadata.value(QStringLiteral("role")).toString(), QStringLiteral("source"));
    QCOMPARE(tracked.constFirst().affectedTargets, QStringList {QStringLiteral("paper.pdf")});
}

void DesktopSyncStoreTest::persistsFrontierAndVerifiedObjects()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    QString error;
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));
        DesktopSyncFrontier frontier;
        frontier.libraryRevision = QStringLiteral("revision-4");
        frontier.sourceFrontier = 10;
        frontier.commentFrontier = 20;
        frontier.annotationFrontier = 30;
        frontier.cursor = 40;
        QVERIFY2(store.setFrontier(frontier, &error), qPrintable(error));
        DesktopVerifiedObject object;
        object.sha256 = QString(64, QLatin1Char('a'));
        object.sizeBytes = 123;
        object.kind = QStringLiteral("lookup");
        object.semanticRole = QStringLiteral("source_navigation");
        object.owningPublication = QStringLiteral("paper.pdf");
        QVERIFY2(store.upsertVerifiedObject(object, &error), qPrintable(error));
    }
    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    QCOMPARE(reopened.frontier(&error).cursor, 40);
    QCOMPARE(reopened.frontier(&error).libraryRevision, QStringLiteral("revision-4"));
    const auto object = reopened.verifiedObject(QString(64, QLatin1Char('a')), &error);
    QVERIFY(object.has_value());
    QCOMPARE(object->sizeBytes, 123);
    // Only the asked receipts, across more than one query (500 each), unknown and invalid ones left out.
    for (int index = 0; index < 1200; ++index) {
        DesktopVerifiedObject more;
        more.sha256 = QStringLiteral("%1").arg(index, 64, 16, QLatin1Char('0'));
        more.sizeBytes = index;
        more.kind = QStringLiteral("lookup");
        QVERIFY2(reopened.upsertVerifiedObject(more, &error), qPrintable(error));
    }
    QStringList asked{QString(64, QLatin1Char('a')), QString(64, QLatin1Char('f')), QStringLiteral("not-a-digest")};
    for (int index = 0; index < 1200; index += 2) asked.append(QStringLiteral("%1").arg(index, 64, 16, QLatin1Char('0')));
    asked.append(asked.constLast());
    const auto some = reopened.verifiedObjects(asked, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(some.size(), 601);
    QCOMPARE(some.value(QString(64, QLatin1Char('a'))).sizeBytes, 123);
    QCOMPARE(some.value(QStringLiteral("%1").arg(1198, 64, 16, QLatin1Char('0'))).sizeBytes, 1198);
    QVERIFY(!some.contains(QStringLiteral("%1").arg(1, 64, 16, QLatin1Char('0'))));
    QCOMPARE(reopened.verifiedObjects(&error).size(), 1201);
}

void DesktopSyncStoreTest::libraryOperationsCoalesceAndAcknowledgeByGeneration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    QString error;
    {
        DesktopSyncStore store(databasePath);
        QVERIFY2(store.open(&error), qPrintable(error));

        const DesktopLibraryEnqueueResult initialAdd = store.enqueueLibraryOperation(libraryOperation(QStringLiteral("add"), QStringLiteral("add-1")));
        QVERIFY2(initialAdd.success, qPrintable(initialAdd.error));
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("rename"), QStringLiteral("rename-1"))).success);
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("rename"), QStringLiteral("rename-2"))).success);
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("archive"), QStringLiteral("archive-1"))).success);
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("archive"), QStringLiteral("archive-2"))).success);

        QList<DesktopLibraryOperation> active = store.libraryOperations(false, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(active.size(), 3);
        QCOMPARE(active.at(0).operationId, QStringLiteral("add-1"));
        QCOMPARE(active.at(1).operationId, QStringLiteral("rename-2"));
        QCOMPARE(active.at(2).operationId, QStringLiteral("archive-2"));

        const std::optional<DesktopLibraryOperation> add = store.claimNextLibraryOperation(&error);
        QVERIFY2(add.has_value(), qPrintable(error));
        QCOMPARE(add->operationId, QStringLiteral("add-1"));
        QCOMPARE(add->state, QStringLiteral("in_flight"));

        const DesktopLibraryEnqueueResult firstRemove = store.enqueueLibraryOperation(libraryOperation(QStringLiteral("remove"), QStringLiteral("remove-1")));
        QVERIFY2(firstRemove.success && firstRemove.queued && !firstRemove.cancelledPendingAdd, qPrintable(firstRemove.error));
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("remove"), QStringLiteral("remove-2"))).success);
        active = store.libraryOperations(false, &error);
        QCOMPARE(active.size(), 2);
        QCOMPARE(active.at(0).operationId, QStringLiteral("add-1"));
        QCOMPARE(active.at(1).operationId, QStringLiteral("remove-2"));

        QVERIFY(!store.acknowledgeLibraryOperation(add->operationId, add->generation + 1, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(store.acknowledgeLibraryOperation(add->operationId, add->generation, &error), qPrintable(error));
        const std::optional<DesktopLibraryOperation> remove = store.claimNextLibraryOperation(&error);
        QVERIFY2(remove.has_value(), qPrintable(error));
        QCOMPARE(remove->operationId, QStringLiteral("remove-2"));

        // A local action created while this generation is in flight must not be
        // cleared by the older acknowledgement.
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("remove"), QStringLiteral("remove-3"))).success);
        QVERIFY2(store.acknowledgeLibraryOperation(remove->operationId, remove->generation, &error), qPrintable(error));
        active = store.libraryOperations(false, &error);
        QCOMPARE(active.size(), 1);
        QCOMPARE(active.constFirst().operationId, QStringLiteral("remove-3"));
        const std::optional<DesktopLibraryOperation> finalRemove = store.claimNextLibraryOperation(&error);
        QVERIFY(finalRemove.has_value());
        QVERIFY2(store.releaseLibraryOperation(finalRemove->operationId, finalRemove->generation, &error), qPrintable(error));
        QCOMPARE(store.libraryOperations(false, &error).constFirst().state, QStringLiteral("pending"));
        const std::optional<DesktopLibraryOperation> retriedRemove = store.claimNextLibraryOperation(&error);
        QVERIFY(retriedRemove.has_value());
        QVERIFY2(store.acknowledgeLibraryOperation(retriedRemove->operationId, retriedRemove->generation, &error), qPrintable(error));

        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("rename"), QStringLiteral("rename-race-1"), QStringLiteral("race.pdf"))).success);
        const std::optional<DesktopLibraryOperation> firstRename = store.claimNextLibraryOperation(&error);
        QVERIFY(firstRename.has_value());
        QCOMPARE(firstRename->operationId, QStringLiteral("rename-race-1"));
        QVERIFY(store.enqueueLibraryOperation(libraryOperation(QStringLiteral("rename"), QStringLiteral("rename-race-2"), QStringLiteral("race.pdf"))).success);
        QVERIFY2(store.releaseLibraryOperation(firstRename->operationId, firstRename->generation, &error), qPrintable(error));
        active = store.libraryOperations(false, &error);
        QCOMPARE(active.size(), 1);
        QCOMPARE(active.constFirst().operationId, QStringLiteral("rename-race-2"));

        const DesktopLibraryEnqueueResult addCancelled = store.enqueueLibraryOperation(libraryOperation(QStringLiteral("add"), QStringLiteral("add-cancel"), QStringLiteral("cancel.pdf")));
        QVERIFY(addCancelled.success && addCancelled.queued);
        const DesktopLibraryEnqueueResult removeCancelled = store.enqueueLibraryOperation(libraryOperation(QStringLiteral("remove"), QStringLiteral("remove-cancel"), QStringLiteral("cancel.pdf")));
        QVERIFY(removeCancelled.success);
        QVERIFY(!removeCancelled.queued);
        QVERIFY(removeCancelled.cancelledPendingAdd);
        const QList<DesktopLibraryOperation> afterCancellation = store.libraryOperations(false, &error);
        const auto cancelActive = std::find_if(afterCancellation.cbegin(), afterCancellation.cend(), [](const DesktopLibraryOperation &operation) { return operation.filename == QLatin1String("cancel.pdf"); });
        QVERIFY(cancelActive == afterCancellation.cend());
    }

    DesktopSyncStore reopened(databasePath);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    const QList<DesktopLibraryOperation> active = reopened.libraryOperations(false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(active.size(), 1);
    QCOMPARE(active.constFirst().operationId, QStringLiteral("rename-race-2"));
    const QList<DesktopLibraryOperation> history = reopened.libraryOperations(true, &error);
    QCOMPARE(history.size(), 12);
    QVERIFY(std::any_of(history.cbegin(), history.cend(), [](const DesktopLibraryOperation &operation) { return operation.operationId == QLatin1String("rename-race-1") && operation.state == QLatin1String("superseded"); }));
}

void DesktopSyncStoreTest::legacyLibraryImportIsTransactionalIdempotentAndRecoverable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString legacyPath = QDir(temporary.path()).filePath(QStringLiteral("repository.json"));
    const QJsonArray legacy {
        libraryOperation(QStringLiteral("rename"), QStringLiteral("legacy-rename-1")),
        libraryOperation(QStringLiteral("rename"), QStringLiteral("legacy-rename-2")),
        libraryOperation(QStringLiteral("archive"), QStringLiteral("legacy-archive")),
    };
    const QByteArray recoveryBytes = QJsonDocument(QJsonObject {{QStringLiteral("pending_library_mutations"), legacy}, {QStringLiteral("preserved_history"), true}}).toJson(QJsonDocument::Compact);
    QVERIFY(writeBytes(legacyPath, recoveryBytes));

    DesktopSyncStore store(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")));
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    const DesktopLegacyLibraryImportResult imported = store.importLegacyLibraryOperations(legacyPath, legacy);
    QVERIFY2(imported.success, qPrintable(imported.error));
    QVERIFY(!imported.alreadyImported);
    QCOMPARE(imported.importedOperations, 3);
    QCOMPARE(store.libraryOperations(false, &error).size(), 2);
    QCOMPARE(store.libraryOperations(true, &error).size(), 3);
    QFile recovery(legacyPath);
    QVERIFY(recovery.open(QIODevice::ReadOnly));
    QCOMPARE(recovery.readAll(), recoveryBytes);

    const DesktopLegacyLibraryImportResult repeated = store.importLegacyLibraryOperations(legacyPath, legacy);
    QVERIFY2(repeated.success, qPrintable(repeated.error));
    QVERIFY(repeated.alreadyImported);
    QCOMPARE(repeated.importedOperations, 0);
    QCOMPARE(store.libraryOperations(true, &error).size(), 3);

    QJsonArray changed = legacy;
    changed.append(libraryOperation(QStringLiteral("remove"), QStringLiteral("legacy-remove")));
    const DesktopLegacyLibraryImportResult conflicting = store.importLegacyLibraryOperations(legacyPath, changed);
    QVERIFY(!conflicting.success);
    QVERIFY(!conflicting.error.isEmpty());
    QCOMPARE(store.libraryOperations(true, &error).size(), 3);

    DesktopSyncStore transactional(QDir(temporary.path()).filePath(QStringLiteral("transactional.sqlite")));
    QVERIFY2(transactional.open(&error), qPrintable(error));
    QJsonArray malformed {libraryOperation(QStringLiteral("rename"), QStringLiteral("valid-before-error")), QJsonObject {{QStringLiteral("type"), QStringLiteral("rename")}}};
    const DesktopLegacyLibraryImportResult failed = transactional.importLegacyLibraryOperations(QStringLiteral("/tmp/broken-repository.json"), malformed);
    QVERIFY(!failed.success);
    QCOMPARE(transactional.libraryOperations(true, &error).size(), 0);
    malformed.removeLast();
    const DesktopLegacyLibraryImportResult retried = transactional.importLegacyLibraryOperations(QStringLiteral("/tmp/broken-repository.json"), malformed);
    QVERIFY2(retried.success, qPrintable(retried.error));
    QCOMPARE(transactional.libraryOperations(false, &error).size(), 1);
}

void DesktopSyncStoreTest::legacyImportIsRecoverableTransactionalAndIdempotent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    const QString legacyPath = QDir(temporary.path()).filePath(QStringLiteral("outbox.json"));
    const QJsonObject legacy {
        {QStringLiteral("next_device_sequence"), 3},
        {QStringLiteral("server_cursor"), 7},
        {QStringLiteral("operations"),
         QJsonArray {
             logicalOperation(QStringLiteral("operation-one"), 1, QStringLiteral("one")),
             logicalOperation(QStringLiteral("operation-two"), 2, QStringLiteral("two")),
         }},
    };
    QVERIFY(writeBytes(legacyPath, QJsonDocument(legacy).toJson(QJsonDocument::Compact)));
    QString recoveryPath;
    {
        DesktopSyncStore store(databasePath);
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        const DesktopLegacyImportResult imported = store.importLegacyOutbox(legacyPath);
        QVERIFY2(imported.success, qPrintable(imported.error));
        QVERIFY(!imported.alreadyImported);
        QCOMPARE(imported.importedOperations, 2);
        QCOMPARE(imported.cursor, 7);
        recoveryPath = imported.recoveryCopyPath;
        QVERIFY(QFileInfo::exists(recoveryPath));
        QVERIFY(QFileInfo::exists(legacyPath));
        QCOMPARE(store.logicalOperations(false, &error).size(), 2);
        QCOMPARE(store.frontier(&error).cursor, 7);
    }

    DesktopSyncStore reopened(databasePath);
    QString error;
    QVERIFY2(reopened.open(&error), qPrintable(error));
    const DesktopLegacyImportResult repeated = reopened.importLegacyOutbox(legacyPath);
    QVERIFY2(repeated.success, qPrintable(repeated.error));
    QVERIFY(repeated.alreadyImported);
    QCOMPARE(reopened.logicalOperations(false, &error).size(), 2);
    QVERIFY2(reopened.acknowledgeLogicalOperations({QStringLiteral("operation-one")}, &error), qPrintable(error));
    QCOMPARE(reopened.logicalOperations(false, &error).size(), 1);
    QCOMPARE(reopened.logicalOperations(true, &error).size(), 2);

    const QString conflictingPath = QDir(temporary.path()).filePath(QStringLiteral("outbox-conflict.json"));
    QJsonObject conflictingLegacy = legacy;
    conflictingLegacy.insert(QStringLiteral("operations"),
                             QJsonArray {
                                 logicalOperation(QStringLiteral("operation-one"), 1, QStringLiteral("conflicting")),
                                 logicalOperation(QStringLiteral("operation-three"), 3, QStringLiteral("three")),
                             });
    conflictingLegacy.insert(QStringLiteral("next_device_sequence"), 4);
    QVERIFY(writeBytes(conflictingPath, QJsonDocument(conflictingLegacy).toJson(QJsonDocument::Compact)));
    const DesktopLegacyImportResult conflict = reopened.importLegacyOutbox(conflictingPath);
    QVERIFY(!conflict.success);
    QVERIFY(QFileInfo::exists(conflict.recoveryCopyPath));
    QCOMPARE(reopened.logicalOperations(true, &error).size(), 2);
    QVERIFY(QFileInfo::exists(recoveryPath));
}

void DesktopSyncStoreTest::deviceSequenceReconciliationIsDurableAndPreservesOperations()
{
    QTemporaryDir temporary;
    const QString path = temporary.filePath(QStringLiteral("sync.sqlite"));
    QString error;
    const QJsonObject acknowledged = logicalOperation(QStringLiteral("accepted"), 4631, QStringLiteral("accepted"));
    const QJsonObject deletion = logicalOperation(QStringLiteral("deletion"), 4632, QStringLiteral("deleted"));
    const QJsonObject newer = logicalOperation(QStringLiteral("newer"), 4800, QStringLiteral("newer"));
    {
        DesktopSyncStore store(path);
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY(store.appendLogicalOperation(acknowledged, &error));
        QVERIFY(store.acknowledgeLogicalOperations({QStringLiteral("accepted")}, &error));
        QVERIFY(store.appendLogicalOperation(deletion, &error));
        QVERIFY(store.appendLogicalOperation(newer, &error));
        QVERIFY2(store.reconcileDeviceSequence(4791, &error), qPrintable(error));
        const auto pending = store.logicalOperations(false, &error);
        QCOMPARE(pending.size(), 2);
        auto expectedDeletion = deletion;
        expectedDeletion.insert(QStringLiteral("device_sequence"), 4801);
        QCOMPARE(pending[0].operation, expectedDeletion);
        auto expectedNewer = newer;
        expectedNewer.insert(QStringLiteral("device_sequence"), 4802);
        QCOMPARE(pending[1].operation, expectedNewer);
        QCOMPARE(store.logicalOperations(true, &error)[0].operation, acknowledged);
        QVERIFY(store.reconcileDeviceSequence(4791, &error));
        QCOMPARE(store.logicalOperations(false, &error)[0].operation, expectedDeletion);
    }
    DesktopSyncStore reopened(path);
    QVERIFY(reopened.open(&error));
    QCOMPARE(reopened.enqueueLogicalOperation({}, &error).value(QStringLiteral("device_sequence")).toInteger(), 4803);
    QVERIFY(!reopened.reconcileDeviceSequence(0, &error));
    QCOMPARE(reopened.logicalOperations(false).size(), 3);
    QVERIFY(reopened.acknowledgeLogicalOperations({QStringLiteral("deletion"), QStringLiteral("newer")}, &error));
    const auto last = reopened.logicalOperations(false, &error).constFirst();
    QVERIFY(reopened.acknowledgeLogicalOperations({last.operationId}, &error));
    QVERIFY(reopened.reconcileDeviceSequence(5000, &error));
    QCOMPARE(reopened.enqueueLogicalOperation({}, &error).value(QStringLiteral("device_sequence")).toInteger(), 5000);
}

void DesktopSyncStoreTest::deviceSequenceReconciliationRollsBackOnExhaustion()
{
    QTemporaryDir temporary;
    DesktopSyncStore store(temporary.filePath(QStringLiteral("sync.sqlite")));
    QString error;
    QVERIFY(store.open(&error));
    const auto first = store.enqueueLogicalOperation({}, &error);
    const auto second = store.enqueueLogicalOperation({}, &error);
    QVERIFY(!store.reconcileDeviceSequence(std::numeric_limits<qint64>::max() - 1, &error));
    QVERIFY(!error.isEmpty());
    const auto pending = store.logicalOperations(false);
    QCOMPARE(pending[0].operation, first);
    QCOMPARE(pending[1].operation, second);
    QCOMPARE(store.enqueueLogicalOperation({}).value(QStringLiteral("device_sequence")).toInteger(), 3);
}

void DesktopSyncStoreTest::outboxAdapterIsDurableOutboxCompatible()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite"));
    const QString legacyPath = QDir(temporary.path()).filePath(QStringLiteral("outbox.json"));
    const QJsonObject annotation = {
        {QStringLiteral("operation_id"), QStringLiteral("legacy-annotation")},
        {QStringLiteral("device_sequence"), 1},
        {QStringLiteral("entity_type"), QStringLiteral("annotation")},
        {QStringLiteral("entity_id"), QStringLiteral("annotation-one")},
        {QStringLiteral("collection"), QStringLiteral("paper")},
        {QStringLiteral("payload"), QJsonObject {{QStringLiteral("subtype"), QStringLiteral("")}}},
    };
    QJsonObject legacy {
        {QStringLiteral("next_device_sequence"), 2},
        {QStringLiteral("server_cursor"), 4},
        {QStringLiteral("operations"), QJsonArray {annotation}},
    };
    const QByteArray originalLegacy = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    QVERIFY(writeBytes(legacyPath, originalLegacy));

    QString error;
    {
        DesktopSyncOutbox outbox(databasePath, legacyPath);
        QVERIFY2(outbox.ready(&error), qPrintable(error));
        QCOMPARE(outbox.cursor(&error), 4);
        QCOMPARE(outbox.operations(&error).size(), 1);
        const AnnotationSubtypeRepairResult repair = outbox.repairBlankAnnotationSubtypes();
        QVERIFY2(repair.success, qPrintable(repair.error));
        QCOMPARE(repair.repaired, 1);
        QVERIFY(QFileInfo::exists(repair.backupPath));
        QCOMPARE(outbox.operations(&error).constFirst().value(QStringLiteral("payload")).toObject().value(QStringLiteral("subtype")).toString(), QStringLiteral("Text"));

        const QJsonObject normal =
            outbox.enqueue(QStringLiteral("logical_comment"), QStringLiteral("comment-one"), QStringLiteral("paper"), 2, QJsonObject {{QStringLiteral("text"), QStringLiteral("hello")}}, QStringLiteral("2026-08-30T12:00:00Z"));
        const QJsonObject resolution = outbox.enqueueAgentProposalResolution(QString(64, QLatin1Char('a')), QStringLiteral("accept"), QJsonArray {QJsonObject {{QStringLiteral("proposal_id"), QStringLiteral("proposal-one")}}});
        const QJsonObject undo = outbox.enqueueAgentProposalUndo(resolution.value(QStringLiteral("operation_id")).toString());
        const QByteArray resolvedSource("resolved TeX\n");
        const QJsonObject sourceResolution = outbox.enqueueSourceConflictResolution(QStringLiteral("source-conflict-one"),
                                                                                    7,
                                                                                    QString(64, QLatin1Char('b')),
                                                                                    resolvedSource,
                                                                                    QStringLiteral("2026-08-30T12:01:00Z"));
        QVERIFY(!normal.isEmpty());
        QVERIFY(!resolution.isEmpty());
        QVERIFY(!undo.isEmpty());
        QVERIFY(!sourceResolution.isEmpty());
        QCOMPARE(normal.value(QStringLiteral("device_sequence")).toInteger(), 2);
        QCOMPARE(resolution.value(QStringLiteral("device_sequence")).toInteger(), 3);
        QCOMPARE(undo.value(QStringLiteral("device_sequence")).toInteger(), 4);
        QCOMPARE(sourceResolution.value(QStringLiteral("device_sequence")).toInteger(), 5);
        QCOMPARE(sourceResolution.value(QStringLiteral("outbox_kind")).toString(), QStringLiteral("source_conflict_resolution"));
        QCOMPARE(sourceResolution.value(QStringLiteral("expected_revision")).toInteger(), 7);
        QCOMPARE(sourceResolution.value(QStringLiteral("expected_sha256")).toString(), QString(64, QLatin1Char('b')));
        QCOMPARE(QByteArray::fromBase64(sourceResolution.value(QStringLiteral("content_base64")).toString().toLatin1()), resolvedSource);
        QCOMPARE(sourceResolution.value(QStringLiteral("content_sha256")).toString(), QString::fromLatin1(QCryptographicHash::hash(resolvedSource, QCryptographicHash::Sha256).toHex()));
        QVERIFY2(outbox.advanceCursor(4, 8, &error), qPrintable(error));
        QVERIFY(!outbox.advanceCursor(4, 9, &error));
        QCOMPARE(outbox.cursor(), 8);
        QVERIFY2(outbox.acknowledge({normal.value(QStringLiteral("operation_id")).toString()}, &error), qPrintable(error));
        QCOMPARE(outbox.operations(&error).size(), 4);
    }
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::ReadOnly));
    QCOMPARE(legacyFile.readAll(), originalLegacy);

    legacy.insert(QStringLiteral("next_device_sequence"), 3);
    legacy.insert(QStringLiteral("operations"), QJsonArray {annotation, logicalOperation(QStringLiteral("late-legacy-write"), 2, QStringLiteral("late"))});
    QVERIFY(writeBytes(legacyPath, QJsonDocument(legacy).toJson(QJsonDocument::Compact)));
    DesktopSyncOutbox reopened(databasePath, legacyPath);
    QVERIFY(!reopened.ready(&error));
    QVERIFY(error.contains(QStringLiteral("one-time import")));
}

QTEST_GUILESS_MAIN(DesktopSyncStoreTest)
#include "desktopsyncstoretest.moc"
