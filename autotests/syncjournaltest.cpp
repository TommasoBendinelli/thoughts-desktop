/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/syncjournal.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using Thoughts::SyncJournal;
using Thoughts::SyncOperationState;

namespace
{
// A journal file looked at, or prepared, through a connection of the test's own.
class RawJournal
{
public:
    explicit RawJournal(const QString &path)
    {
        static int serial = 0;
        m_name = QStringLiteral("syncjournaltest-%1").arg(++serial);
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_database.setDatabaseName(path);
        m_open = m_database.open();
        exec(QStringLiteral("PRAGMA busy_timeout=10000"));
    }
    ~RawJournal()
    {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }
    bool isOpen() const
    {
        return m_open;
    }
    bool exec(const QString &statement)
    {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            qWarning() << statement.left(120) << query.lastError().text();
            return false;
        }
        return true;
    }
    QVariant value(const QString &statement)
    {
        QSqlQuery query(m_database);
        return query.exec(statement) && query.next() ? query.value(0) : QVariant();
    }
    QSqlDatabase &database()
    {
        return m_database;
    }

private:
    QString m_name;
    QSqlDatabase m_database;
    bool m_open = false;
};

// The journal as builds before this one created it: free pages stay in the
// file (auto_vacuum off) and no index finds a run's error events.
bool createEarlierJournal(RawJournal &raw)
{
    const QStringList statements {
        QStringLiteral("PRAGMA auto_vacuum=NONE"),
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("CREATE TABLE sync_runs (id TEXT PRIMARY KEY, trace_id TEXT NOT NULL UNIQUE, retry_of TEXT, kind TEXT NOT NULL, target TEXT, status TEXT NOT NULL, phase TEXT NOT NULL, created_at_ms INTEGER NOT NULL, started_at_ms "
                       "INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, completed_at_ms INTEGER, duration_ms INTEGER, build_id TEXT, payload_json TEXT NOT NULL, server_state TEXT NOT NULL DEFAULT '', server_terminal INTEGER NOT NULL DEFAULT 0, "
                       "last_server_sequence INTEGER NOT NULL DEFAULT 0, stream_state TEXT NOT NULL DEFAULT 'offline', transfer_metrics_recorded INTEGER NOT NULL DEFAULT 0, uploaded_completed_bytes INTEGER NOT NULL DEFAULT 0, "
                       "uploaded_total_bytes INTEGER NOT NULL DEFAULT 0, downloaded_completed_bytes INTEGER NOT NULL DEFAULT 0, downloaded_total_bytes INTEGER NOT NULL DEFAULT 0, initiation_control TEXT NOT NULL DEFAULT 'unknown')"),
        QStringLiteral("CREATE INDEX sync_runs_state_updated ON sync_runs(status, updated_at_ms)"),
        QStringLiteral("CREATE INDEX sync_runs_target_updated ON sync_runs(target, updated_at_ms)"),
        QStringLiteral("CREATE INDEX sync_runs_global_success ON sync_runs(kind, phase, target, completed_at_ms, updated_at_ms)"),
        QStringLiteral("CREATE INDEX sync_runs_kind_updated ON sync_runs(kind, updated_at_ms)"),
        QStringLiteral("CREATE TABLE sync_steps (run_id TEXT NOT NULL, ordinal INTEGER NOT NULL, phase TEXT NOT NULL, state TEXT NOT NULL, attempt INTEGER NOT NULL DEFAULT 0, request_id TEXT, started_at_ms INTEGER NOT NULL, updated_at_ms "
                       "INTEGER NOT NULL, deadline_at_ms INTEGER, facts_json TEXT, PRIMARY KEY(run_id, ordinal))"),
        QStringLiteral("CREATE TABLE sync_events (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL, at_ms INTEGER NOT NULL, phase TEXT NOT NULL, state TEXT NOT NULL, attempt INTEGER NOT NULL DEFAULT 0, message TEXT NOT NULL, "
                       "duration_ms INTEGER, error_code TEXT, origin TEXT NOT NULL DEFAULT 'desktop', server_sequence INTEGER, server_timestamp_ms INTEGER, received_at_ms INTEGER, component TEXT NOT NULL DEFAULT 'desktop', build_id TEXT, "
                       "completed INTEGER, total INTEGER, bytes_completed INTEGER, bytes_total INTEGER, metadata_json TEXT NOT NULL DEFAULT '{}', terminal INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX sync_events_run_time ON sync_events(run_id, at_ms)"),
        QStringLiteral("CREATE UNIQUE INDEX sync_events_server_sequence ON sync_events(run_id, origin, server_sequence)"),
        QStringLiteral("CREATE TABLE sync_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("INSERT INTO sync_metadata(key, value) VALUES('legacy_json_imported', '1'), ('embedded_events_extracted_v1', '1')"),
    };
    for (const QString &statement : statements) {
        if (!raw.exec(statement)) {
            return false;
        }
    }
    return true;
}

// `runs` Syncs, the oldest `days` ago and the newest now, each with its row
// and, where `eventsExpression` (SQL over the run number r) says so, events.
bool seedRuns(RawJournal &raw, int runs, int days, const QString &eventsExpression, int largestRun)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 span = qint64(days) * 24 * 60 * 60 * 1000;
    const qint64 first = now - span;
    const qint64 interval = span / qMax(1, runs);
    if (!raw.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return false;
    }
    bool ok = raw.exec(QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS seed_numbers(i INTEGER PRIMARY KEY)")) && raw.exec(QStringLiteral("DELETE FROM seed_numbers"))
        && raw.exec(QStringLiteral("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < %1) INSERT INTO seed_numbers SELECT i FROM n").arg(qMax(runs, largestRun)));
    // One run in three failed, as in a journal that saw a bad day.
    ok = ok && raw.exec(QStringLiteral(
        "INSERT INTO sync_runs(id, trace_id, kind, target, status, phase, created_at_ms, started_at_ms, updated_at_ms, completed_at_ms, duration_ms, payload_json, initiation_control) "
        "SELECT ('0190a000-0000-7000-8000-' || substr('000000000000' || i, -12)), ('0190a000-0000-7000-8000-' || substr('000000000000' || i, -12)), 'sync', '', CASE WHEN i % 3 = 0 THEN 'failed' ELSE 'succeeded' END, CASE WHEN i % 3 = 0 THEN 'failed' ELSE 'succeeded' END, %1 + i * %2, %1 + i * %2, %1 + i * %2 + 30000, "
        "%1 + i * %2 + 30000, 30000, json_object('id', ('0190a000-0000-7000-8000-' || substr('000000000000' || i, -12)), 'trace_id', ('0190a000-0000-7000-8000-' || substr('000000000000' || i, -12)), 'kind', 'sync', 'target', '', 'created_at_ms', %1 + i * %2, 'started_at_ms', %1 + i * %2, 'updated_at_ms', "
        "%1 + i * %2 + 30000, 'completed_at_ms', %1 + i * %2 + 30000, 'duration_ms', 30000, 'initiation_control', 'pdf_toolbar_sync', 'transfer_metrics_recorded', json('true'), 'uploaded_completed_bytes', 4096, 'uploaded_total_bytes', "
        "4096, 'downloaded_completed_bytes', 8192, 'downloaded_total_bytes', 8192, 'last_error_message', CASE WHEN i % 3 = 0 THEN 'The server could not be reached.' ELSE '' END), 'pdf_toolbar_sync' FROM seed_numbers WHERE i < %3")
            .arg(first).arg(interval).arg(runs));
    ok = ok && raw.exec(QStringLiteral("INSERT INTO sync_steps(run_id, ordinal, phase, state, started_at_ms, updated_at_ms) SELECT ('0190a000-0000-7000-8000-' || substr('000000000000' || i, -12)), 0, 'queued', 'running', %1 + i * %2, %1 + i * %2 FROM seed_numbers WHERE i < %3")
                            .arg(first).arg(interval).arg(runs));
    if (ok && !eventsExpression.isEmpty()) {
        // A third of the events came from the server, with its sequence and time.
        ok = raw.exec(QStringLiteral(
            "INSERT INTO sync_events(run_id, at_ms, phase, state, message, error_code, origin, server_sequence, server_timestamp_ms, received_at_ms, component, metadata_json) "
            "SELECT ('0190a000-0000-7000-8000-' || substr('000000000000' || run.i, -12)), %1 + run.i * %2 + event.i * 2, CASE event.i % 4 WHEN 0 THEN 'preparing' WHEN 1 THEN 'verifying' WHEN 2 THEN 'uploading' ELSE 'checking_server' END, 'running', "
            "printf('Preparing source %d of the managed bundle for the server (%d)', event.i, run.i), CASE WHEN event.i = 3 AND run.i % 97 = 0 THEN 'invalid_source_publication' WHEN event.i = 7 AND run.i % 3 = 0 THEN 'sync_failed' ELSE '' END, "
            "CASE WHEN event.i % 3 = 0 THEN 'server' ELSE 'desktop' END, CASE WHEN event.i % 3 = 0 THEN event.i + 1 END, CASE WHEN event.i % 3 = 0 THEN %1 + run.i * %2 + event.i * 2 END, "
            "CASE WHEN event.i % 3 = 0 THEN %1 + run.i * %2 + event.i * 2 + 40 END, CASE WHEN event.i % 3 = 0 THEN 'renderer' ELSE 'desktop' END, printf('{\"target\":\"paper-%d.pdf\"}', run.i % 40) "
            "FROM (SELECT i, (%3) AS events FROM (SELECT i, i AS r FROM seed_numbers WHERE i < %4)) AS run JOIN seed_numbers AS event ON event.i < run.events")
                          .arg(first).arg(interval).arg(eventsExpression).arg(runs));
    }
    if (!ok) {
        raw.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    return raw.exec(QStringLiteral("COMMIT"));
}

// The id of the seeded run `number`, as long as a real one.
QString runId(int number)
{
    return QStringLiteral("0190a000-0000-7000-8000-%1").arg(number, 12, 10, QLatin1Char('0'));
}

SyncJournal::Policy quietPolicy()
{
    SyncJournal::Policy policy;
    // Retention runs when a test asks for it.
    policy.maintenanceStartDelayMs = 24 * 60 * 60 * 1000;
    policy.maintenanceStepPauseMs = 0;
    return policy;
}

SyncJournal::Page pageOf(SyncJournal &journal, const SyncJournal::PageRequest &request)
{
    SyncJournal::Page result;
    bool arrived = false;
    QObject context;
    journal.fetchPage(&context, request, [&](const SyncJournal::Page &page) {
        result = page;
        arrived = true;
    });
    if (!QTest::qWaitFor([&arrived] { return arrived; }, 30000)) {
        qWarning() << "The journal did not answer";
    }
    return result;
}

SyncJournal::History historyOf(SyncJournal &journal, const QString &id, qint64 afterRow)
{
    SyncJournal::History result;
    bool arrived = false;
    QObject context;
    journal.fetchHistory(&context, id, afterRow, [&](const SyncJournal::History &history) {
        result = history;
        arrived = true;
    });
    if (!QTest::qWaitFor([&arrived] { return arrived; }, 30000)) {
        qWarning() << "The journal did not answer";
    }
    return result;
}
}

class SyncJournalTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testNewJournalReturnsFreePages();
    void testWritesOfABurstShareOneTransaction();
    void testStartAndEndOfARunAreCommittedAtOnce();
    void testAFailedWriteLeavesTheBurstIntact();
    void testInterruptedRunsAreReconciled();
    void testAShutDownJournalAnswersEmpty();
    void testPagesAreBoundedAndContinue();
    void testHistoryRefreshReadsOnlyNewEvents();
    void testLookupsUseTheirIndexes();
    void testRetentionKeepsTheLongerOfDaysAndCount();
    void testEventsWithoutARunAreRemovedOnce();
    void testRealisticJournalStaysResponsive();
    void testCopyOfAnExistingJournal();
};

void SyncJournalTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void SyncJournalTest::testNewJournalReturnsFreePages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const SyncJournal::Statistics statistics = journal.statistics();
    QCOMPARE(statistics.autoVacuum, 2);
    QVERIFY(statistics.errorIndex);
    journal.runMaintenanceNow();
    QVERIFY(!journal.compactionPending());
    QVERIFY(journal.latestGlobalSummary().isEmpty());
    QVERIFY(!journal.lastSuccessfulGlobalSyncAt().isValid());
}

void SyncJournalTest::testWritesOfABurstShareOneTransaction()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const QString id = journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("pdf_toolbar_sync"));
    QVERIFY(!id.isEmpty());
    journal.flush();
    const SyncJournal::Statistics before = journal.statistics();

    // A Sync reports hundreds of events in a row (the ninth decile was 565).
    constexpr int events = 565;
    QElapsedTimer queueing;
    queueing.start();
    for (int index = 0; index < events; ++index) {
        journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("preparing")}, {QStringLiteral("message"), QStringLiteral("Preparing source %1").arg(index)}, {QStringLiteral("at_ms"), 1000 + index}});
    }
    const qint64 queueingMs = queueing.elapsed();
    // What was queued is visible to the next read, committed or not.
    const QJsonObject running = journal.operationWithHistory(id);
    QCOMPARE(running.value(QStringLiteral("events")).toArray().size(), events + 1);
    journal.flush();
    const SyncJournal::Statistics after = journal.statistics();
    QCOMPARE(after.writes - before.writes, qint64(events));
    qInfo() << events << "journal events queued in" << queueingMs << "ms and stored in" << after.commits - before.commits << "transactions";
    QVERIFY2(after.commits - before.commits <= 8, "A burst of events must not be one transaction per event");
    QVERIFY2(queueingMs < 500, "Queueing events must not wait for the disk");

    // In the order they were reported.
    const QJsonArray stored = journal.operationWithHistory(id).value(QStringLiteral("events")).toArray();
    QCOMPARE(stored.size(), events + 1);
    for (int index = 0; index < events; ++index) {
        QCOMPARE(stored.at(index).toObject().value(QStringLiteral("message")).toString(), QStringLiteral("Preparing source %1").arg(index));
    }
    RawJournal raw(journal.path());
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events")).toInt(), events + 1);
}

void SyncJournalTest::testStartAndEndOfARunAreCommittedAtOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal::Policy policy = quietPolicy();
    // A burst would wait this long: the start and the end of a run must not.
    policy.burstCoalesceMs = 60 * 1000;
    policy.burstMaximumMs = 60 * 1000;
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, policy);
    const QString id = journal.beginOperation(QStringLiteral("sync"), QStringLiteral("paper.pdf"), QStringLiteral("queued"), QStringLiteral("command_api"));
    QVERIFY(!id.isEmpty());
    RawJournal raw(journal.path());
    QTRY_COMPARE_WITH_TIMEOUT(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_runs WHERE status = 'running'")).toInt(), 1, 5000);

    journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("uploading")}, {QStringLiteral("message"), QStringLiteral("Publishing sources")}});
    journal.updateOperation(id, SyncOperationState::Failed, QStringLiteral("failed"), QStringLiteral("The server could not be reached."), QString(), QStringLiteral("network_timeout"));
    // The read is answered from the open transaction, the other connection after its commit.
    const QJsonObject summary = journal.operationSummary(id);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QCOMPARE(summary.value(QStringLiteral("last_error_code")).toString(), QStringLiteral("network_timeout"));
    QVERIFY(!summary.contains(QStringLiteral("warning_code")));
    QTRY_COMPARE_WITH_TIMEOUT(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_runs WHERE status = 'failed' AND completed_at_ms IS NOT NULL")).toInt(), 1, 5000);
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE message = 'Publishing sources'")).toInt(), 1);
    QCOMPARE(journal.latestDocumentSummary(QStringLiteral("paper.pdf")).value(QStringLiteral("id")).toString(), id);
    QVERIFY(journal.runningSummaries().isEmpty());
}

void SyncJournalTest::testAFailedWriteLeavesTheBurstIntact()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const QString id = journal.beginOperation(QStringLiteral("build"), QStringLiteral("paper.pdf"), QStringLiteral("waiting_source_publication"), QStringLiteral("pdf_rebuild_action"));
    QVERIFY(!id.isEmpty());
    QCOMPARE(journal.runningBuildOperationIdForTarget(QStringLiteral("paper.pdf")), id);
    journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("building")}, {QStringLiteral("message"), QStringLiteral("First")}});
    // Neither names a run of this journal.
    journal.appendEvent(QStringLiteral("run-unknown"), QJsonObject {{QStringLiteral("phase"), QStringLiteral("building")}, {QStringLiteral("message"), QStringLiteral("Lost")}});
    journal.updateTransferProgress(QStringLiteral("run-unknown"), QJsonObject {{QStringLiteral("transfer_metrics_recorded"), true}, {QStringLiteral("uploaded_completed_bytes"), 1}, {QStringLiteral("uploaded_total_bytes"), 2},
                                                                              {QStringLiteral("downloaded_completed_bytes"), 0}, {QStringLiteral("downloaded_total_bytes"), 0}});
    journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("building")}, {QStringLiteral("message"), QStringLiteral("Second")}});
    journal.updateTransferProgress(id, QJsonObject {{QStringLiteral("transfer_metrics_recorded"), true}, {QStringLiteral("uploaded_completed_bytes"), 10}, {QStringLiteral("uploaded_total_bytes"), 20},
                                                    {QStringLiteral("downloaded_completed_bytes"), 30}, {QStringLiteral("downloaded_total_bytes"), 40}});
    // The Sync's changes arrived but a PDF could not be built: it succeeds with a warning.
    journal.updateOperation(id, SyncOperationState::Succeeded, QStringLiteral("succeeded"), QStringLiteral("Completed"), QStringLiteral("build-a"), QString(), QStringLiteral("pdf_build_failed"),
                            QStringLiteral("PDF build failed\npaper.pdf: latex_error"));
    const QJsonObject warned = journal.operationSummary(id);
    QCOMPARE(warned.value(QStringLiteral("status")).toString(), QStringLiteral("succeeded"));
    QCOMPARE(warned.value(QStringLiteral("warning_code")).toString(), QStringLiteral("pdf_build_failed"));
    QCOMPARE(warned.value(QStringLiteral("warning_message")).toString(), QStringLiteral("PDF build failed\npaper.pdf: latex_error"));
    QVERIFY(!warned.contains(QStringLiteral("last_error_code")));
    journal.flush();
    RawJournal raw(journal.path());
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE message IN ('First', 'Second')")).toInt(), 2);
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE message = 'Lost'")).toInt(), 0);
    QCOMPARE(raw.value(QStringLiteral("SELECT uploaded_total_bytes + downloaded_total_bytes FROM sync_runs")).toInt(), 60);
    QCOMPARE(raw.value(QStringLiteral("SELECT status || '/' || build_id FROM sync_runs")).toString(), QStringLiteral("succeeded/build-a"));
    QVERIFY(journal.runningBuildOperationIdForTarget(QStringLiteral("paper.pdf")).isEmpty());
}

void SyncJournalTest::testInterruptedRunsAreReconciled()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const QString earlier = journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("remote_change"));
    QCOMPARE(journal.runningSummaries().size(), 1);
    QTest::qWait(5);
    const qint64 restartedAt = QDateTime::currentMSecsSinceEpoch();
    QTest::qWait(5);
    const QString current = journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("remote_change"));
    journal.reconcileInterrupted(restartedAt);
    QCOMPARE(journal.operationSummary(earlier).value(QStringLiteral("status")).toString(), QStringLiteral("interrupted"));
    QCOMPARE(journal.operationSummary(earlier).value(QStringLiteral("last_error_code")).toString(), QStringLiteral("desktop_restarted"));
    QCOMPARE(journal.operationSummary(current).value(QStringLiteral("status")).toString(), QStringLiteral("running"));
    QCOMPARE(journal.runningSummaries().size(), 1);
}

// The application shuts the journal down before it deletes the objects that
// report to it: what they still say is dropped, and nothing waits.
void SyncJournalTest::testAShutDownJournalAnswersEmpty()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const QString id = journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("pdf_toolbar_sync"));
    journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("uploading")}, {QStringLiteral("message"), QStringLiteral("Stored before the end")}});
    journal.shutDown();
    {
        RawJournal raw(journal.path());
        QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE message = 'Stored before the end'")).toInt(), 1);
    }
    journal.appendEvent(id, QJsonObject {{QStringLiteral("phase"), QStringLiteral("uploading")}, {QStringLiteral("message"), QStringLiteral("Too late")}});
    journal.updateOperation(id, SyncOperationState::Succeeded, QStringLiteral("succeeded"), QStringLiteral("Completed"));
    QVERIFY(journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("pdf_toolbar_sync")).isEmpty());
    QVERIFY(journal.latestGlobalSummary().isEmpty());
    QVERIFY(journal.runningSummaries().isEmpty());
    journal.flush();
    journal.compact();
    QVERIFY(!journal.compactionRunning());
    bool answered = false;
    QObject context;
    QVERIFY(!journal.fetchPage(&context, SyncJournal::PageRequest(), [&answered](const SyncJournal::Page &) { answered = true; }, 1000));
    QVERIFY(!answered);
    RawJournal raw(journal.path());
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE message = 'Too late'")).toInt(), 0);
    QCOMPARE(raw.value(QStringLiteral("SELECT status FROM sync_runs")).toString(), QStringLiteral("running"));
}

void SyncJournalTest::testPagesAreBoundedAndContinue()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
    {
        RawJournal raw(path);
        QVERIFY(createEarlierJournal(raw));
        QVERIFY(seedRuns(raw, 250, 20, QStringLiteral("12"), 12));
    }
    SyncJournal journal(path, nullptr, quietPolicy());
    SyncJournal::PageRequest request;
    request.named = {runId(3), runId(249), QStringLiteral("run-missing")};
    // With a wait, the page is there when the call returns.
    SyncJournal::Page first;
    QObject context;
    QVERIFY(journal.fetchPage(&context, request, [&first](const SyncJournal::Page &page) { first = page; }, 30000));
    QCOMPARE(first.operations.size(), SyncJournal::LogsPageSize);
    QVERIFY(first.hasOlder);
    QCOMPARE(first.operations.constFirst().value(QStringLiteral("id")).toString(), runId(249));
    QCOMPARE(first.operations.constLast().value(QStringLiteral("id")).toString(), runId(150));
    QCOMPARE(first.oldestId, runId(150));
    // A named run outside the page comes with it; one inside is not repeated.
    QCOMPARE(first.named.size(), 1);
    QCOMPARE(first.named.constFirst().value(QStringLiteral("id")).toString(), runId(3));
    for (const QJsonObject &operation : std::as_const(first.operations)) {
        QVERIFY(!operation.contains(QStringLiteral("events")));
        const int number = operation.value(QStringLiteral("id")).toString().right(6).toInt();
        QCOMPARE(operation.value(QStringLiteral("needs_source_conflict_resolution")).toBool(), number % 97 == 0);
        QCOMPARE(operation.value(QStringLiteral("status")).toString(), number % 3 == 0 ? QStringLiteral("failed") : QStringLiteral("succeeded"));
    }

    request = {};
    request.beforeCreatedAtMs = first.oldestCreatedAtMs;
    request.beforeId = first.oldestId;
    const SyncJournal::Page second = pageOf(journal, request);
    QCOMPARE(second.operations.size(), SyncJournal::LogsPageSize);
    QVERIFY(second.hasOlder);
    QCOMPARE(second.operations.constFirst().value(QStringLiteral("id")).toString(), runId(149));
    request.beforeCreatedAtMs = second.oldestCreatedAtMs;
    request.beforeId = second.oldestId;
    const SyncJournal::Page third = pageOf(journal, request);
    QCOMPARE(third.operations.size(), 50);
    QVERIFY(!third.hasOlder);
    QCOMPARE(third.operations.constLast().value(QStringLiteral("id")).toString(), runId(0));

    // The compatibility list still returns up to 500 runs with the same summaries.
    const QJsonArray recent = journal.recentOperations(false);
    QCOMPARE(recent.size(), 250);
    QCOMPARE(recent.first().toObject(), first.operations.constFirst());

    // A context that is gone gets no answer.
    bool answered = false;
    {
        QObject gone;
        journal.fetchPage(&gone, SyncJournal::PageRequest(), [&answered](const SyncJournal::Page &) { answered = true; });
    }
    journal.flush();
    QTest::qWait(50);
    QVERIFY(!answered);
}

void SyncJournalTest::testHistoryRefreshReadsOnlyNewEvents()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
    {
        RawJournal raw(path);
        QVERIFY(createEarlierJournal(raw));
        QVERIFY(seedRuns(raw, 3, 1, QStringLiteral("400"), 400));
        // Server events that arrive late carry earlier times, and several share one.
        QVERIFY(raw.exec(QStringLiteral("UPDATE sync_events SET at_ms = at_ms - (id % 7) * 3, server_timestamp_ms = CASE WHEN origin = 'server' THEN at_ms - (id % 5) * 2 END WHERE run_id = '0190a000-0000-7000-8000-000000000001'")));
    }
    SyncJournal journal(path, nullptr, quietPolicy());
    const QString id = runId(1);
    const QJsonObject full = journal.operationWithHistory(id);
    QCOMPARE(full.value(QStringLiteral("events")).toArray().size(), 400);

    SyncJournal::History history = historyOf(journal, id, 0);
    QVERIFY(history.found);
    QCOMPARE(history.events.size(), 400);
    QCOMPARE(SyncJournal::operationFromHistory(history.operation, history.events), full);

    // New events, one of them older than what is shown.
    qint64 lastRow = 0;
    for (const SyncJournal::Event &event : std::as_const(history.events)) {
        lastRow = qMax(lastRow, event.row);
    }
    {
        RawJournal raw(path);
        QVERIFY(raw.exec(QStringLiteral("INSERT INTO sync_events(run_id, at_ms, phase, state, message, origin) SELECT '0190a000-0000-7000-8000-000000000001', MAX(at_ms) + 10, 'verifying', 'running', 'Verified', 'desktop' FROM sync_events WHERE run_id = '0190a000-0000-7000-8000-000000000001'")));
        QVERIFY(raw.exec(QStringLiteral("INSERT INTO sync_events(run_id, at_ms, phase, state, message, origin, server_sequence, server_timestamp_ms) SELECT '0190a000-0000-7000-8000-000000000001', MIN(at_ms) + 5, 'building', 'running', 'Built earlier', 'server', 9000, "
                                        "MIN(at_ms) + 5 FROM sync_events WHERE run_id = '0190a000-0000-7000-8000-000000000001'")));
    }
    const SyncJournal::History added = historyOf(journal, id, lastRow);
    QVERIFY(added.found);
    QCOMPARE(added.events.size(), 2);
    QList<SyncJournal::Event> all = history.events;
    all.append(added.events);
    const QJsonObject refreshed = SyncJournal::operationFromHistory(added.operation, all);
    QCOMPARE(refreshed, journal.operationWithHistory(id));
    QCOMPARE(refreshed.value(QStringLiteral("events")).toArray().size(), 402);

    QVERIFY(!historyOf(journal, QStringLiteral("run-missing"), 0).found);
    QVERIFY(journal.operationWithHistory(QString()).isEmpty());
}

void SyncJournalTest::testLookupsUseTheirIndexes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SyncJournal journal(directory.filePath(QStringLiteral("sync-operations.sqlite")), nullptr, quietPolicy());
    const auto planOf = [&journal](const QString &statement) { return journal.queryPlan(statement).join(QLatin1Char('\n')); };
    // Which runs reported a source conflict (the Logs list).
    QVERIFY2(planOf(QStringLiteral("SELECT DISTINCT run_id FROM sync_events WHERE run_id IN ('a', 'b') AND error_code = 'invalid_source_publication' AND error_code != ''")).contains(QStringLiteral("sync_events_run_error")),
             qPrintable(planOf(QStringLiteral("SELECT DISTINCT run_id FROM sync_events WHERE run_id IN ('a', 'b') AND error_code = 'invalid_source_publication' AND error_code != ''"))));
    // Why a run failed (recorded when it ends).
    QVERIFY(planOf(QStringLiteral("SELECT message, error_code FROM sync_events WHERE run_id = 'a' AND error_code IS NOT NULL AND error_code != '' ORDER BY COALESCE(server_timestamp_ms, at_ms) DESC, id DESC LIMIT 1"))
                .contains(QStringLiteral("sync_events_run_error")));
    // A page of the Logs list, and the runs retention removes.
    const QString page = planOf(QStringLiteral("SELECT id FROM sync_runs WHERE created_at_ms < 5 OR (created_at_ms = 5 AND id < 'a') ORDER BY created_at_ms DESC, id DESC LIMIT 101"));
    QVERIFY2(page.contains(QStringLiteral("sync_runs_created")) && !page.contains(QStringLiteral("TEMP B-TREE")), qPrintable(page));
    QVERIFY(planOf(QStringLiteral("SELECT id FROM sync_runs WHERE updated_at_ms < 5 ORDER BY updated_at_ms LIMIT 1")).contains(QStringLiteral("sync_runs_updated")));
}

void SyncJournalTest::testRetentionKeepsTheLongerOfDaysAndCount()
{
    // More runs than are always kept, none older than the retention period: all stay.
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
        {
            RawJournal raw(path);
            QVERIFY(createEarlierJournal(raw));
            QVERIFY(seedRuns(raw, 2600, 10, QStringLiteral("2"), 2));
        }
        SyncJournal journal(path, nullptr, quietPolicy());
        journal.runMaintenanceNow();
        QCOMPARE(journal.statistics().runs, qint64(2600));
    }
    // Old runs, but fewer than are always kept: all stay.
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
        {
            RawJournal raw(path);
            QVERIFY(createEarlierJournal(raw));
            QVERIFY(seedRuns(raw, 300, 200, QStringLiteral("2"), 2));
        }
        SyncJournal journal(path, nullptr, quietPolicy());
        journal.runMaintenanceNow();
        QCOMPARE(journal.statistics().runs, qint64(300));
    }
    // Both: the newest that are always kept stay, although some are older than the period.
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
        {
            RawJournal raw(path);
            QVERIFY(createEarlierJournal(raw));
            // 2,600 runs over 60 days: 1,300 in the last 30.
            QVERIFY(seedRuns(raw, 2600, 60, QStringLiteral("3"), 3));
        }
        SyncJournal journal(path, nullptr, quietPolicy());
        journal.runMaintenanceNow();
        const SyncJournal::Statistics statistics = journal.statistics();
        QCOMPARE(statistics.runs, qint64(SyncJournal::RetentionMinimumOperations));
        QCOMPARE(statistics.events, qint64(SyncJournal::RetentionMinimumOperations) * 3);
        RawJournal raw(path);
        QCOMPARE(raw.value(QStringLiteral("SELECT MIN(id) FROM sync_runs")).toString(), runId(600));
        QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_steps")).toInt(), SyncJournal::RetentionMinimumOperations);
        QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_events WHERE run_id < '0190a000-0000-7000-8000-000000000600'")).toInt(), 0);
    }
    // A shorter rule: the period decides once fewer runs are always kept.
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
        {
            RawJournal raw(path);
            QVERIFY(createEarlierJournal(raw));
            QVERIFY(seedRuns(raw, 600, 60, QStringLiteral("3"), 3));
        }
        SyncJournal::Policy policy = quietPolicy();
        policy.retentionMinimumOperations = 100;
        SyncJournal journal(path, nullptr, policy);
        journal.runMaintenanceNow();
        // 300 of the 600 are younger than 30 days (give or take the one on the boundary).
        const qint64 kept = journal.statistics().runs;
        QVERIFY2(kept >= 299 && kept <= 301, qPrintable(QString::number(kept)));
    }
}

void SyncJournalTest::testEventsWithoutARunAreRemovedOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
    {
        RawJournal raw(path);
        QVERIFY(createEarlierJournal(raw));
        QVERIFY(seedRuns(raw, 20, 2, QStringLiteral("50"), 50));
        // What an interrupted clean-up of an earlier build left behind.
        QVERIFY(raw.exec(QStringLiteral("DELETE FROM sync_runs WHERE id IN ('0190a000-0000-7000-8000-000000000004', '0190a000-0000-7000-8000-000000000011')")));
    }
    SyncJournal::Policy policy = quietPolicy();
    policy.orphanRowsPerStep = 64;
    SyncJournal journal(path, nullptr, policy);
    QCOMPARE(journal.statistics().events, qint64(1000));
    journal.runMaintenanceNow();
    QCOMPARE(journal.statistics().events, qint64(900));
    RawJournal raw(path);
    QCOMPARE(raw.value(QStringLiteral("SELECT COUNT(*) FROM sync_steps")).toInt(), 18);
    QCOMPARE(raw.value(QStringLiteral("SELECT value FROM sync_metadata WHERE key = 'orphan_events_swept_v1'")).toString(), QStringLiteral("1"));
}

// A journal the size of one that grew for three months: 3,500 Syncs and about
// 1.3 million events, a few hundred megabytes, in the format earlier builds
// wrote. THOUGHTS_SYNCJOURNAL_TEST_PERCENT scales it down for a quick run.
void SyncJournalTest::testRealisticJournalStaysResponsive()
{
    bool scaled = false;
    const int percent = qBound(1, qEnvironmentVariableIntValue("THOUGHTS_SYNCJOURNAL_TEST_PERCENT", &scaled), 100);
    const int scale = scaled ? percent : 100;
    const int runs = 3500;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sync-operations.sqlite"));
    QElapsedTimer timer;
    timer.start();
    {
        RawJournal raw(path);
        QVERIFY(createEarlierJournal(raw));
        // Most Syncs record a few dozen events, one in ten several hundred, and
        // a few record many thousands.
        const QString events = QStringLiteral("(CASE WHEN r % 150 = 0 THEN 14000 WHEN r % 17 = 0 THEN 2800 WHEN r % 10 = 3 THEN 565 ELSE 26 + r % 40 END) * %1 / 100").arg(scale);
        QVERIFY(seedRuns(raw, runs, 80, events, 14000));
        QVERIFY(raw.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")));
    }
    const qint64 generatedMs = timer.restart();

    SyncJournal::Policy policy;
    policy.maintenanceStartDelayMs = 300;
    SyncJournal journal(path, nullptr, policy);
    // The first answers come without any maintenance: the file is used as it is.
    const QJsonObject latest = journal.latestGlobalSummary();
    const qint64 firstAnswerMs = timer.restart();
    QCOMPARE(latest.value(QStringLiteral("id")).toString(), runId(runs - 1));
    QVERIFY(journal.lastSuccessfulGlobalSyncAt().isValid());
    SyncJournal::Statistics before = journal.statistics();
    qInfo() << "Generated journal:" << before.runs << "runs," << before.events << "events," << before.fileBytes / (1024 * 1024) << "MiB in" << generatedMs << "ms; first answer after" << firstAnswerMs << "ms";
    QCOMPARE(before.runs, qint64(runs));
    QCOMPARE(before.autoVacuum, 0);
    QVERIFY(scale < 100 || !before.errorIndex);
    QVERIFY2(firstAnswerMs < 2000, "Opening a large journal must not rebuild or scan it");

    // The Logs window: one page, not every run and not every event.
    timer.restart();
    const SyncJournal::Page page = pageOf(journal, SyncJournal::PageRequest());
    const qint64 pageMs = timer.restart();
    QCOMPARE(page.operations.size(), SyncJournal::LogsPageSize);
    QVERIFY(page.hasOlder);
    const SyncJournal::History history = historyOf(journal, runId(3450), 0);
    const qint64 historyMs = timer.restart();
    QCOMPARE(history.events.size(), 14000 * scale / 100);
    qInfo() << "Logs page of" << page.operations.size() << "runs:" << pageMs << "ms; history of a run with" << history.events.size() << "events:" << historyMs << "ms";
    QVERIFY(pageMs < 1000);

    // Retention runs by itself, in steps, while reads and writes go on: nothing waits long for it.
    const QString live = journal.beginOperation(QStringLiteral("sync"), QString(), QStringLiteral("queued"), QStringLiteral("remote_change"));
    QVERIFY(!live.isEmpty());
    qint64 slowestAnswerMs = 0;
    int answers = 0;
    int round = 0;
    QElapsedTimer retention;
    retention.start();
    QElapsedTimer answer;
    while (journal.statistics().runs > SyncJournal::RetentionMinimumOperations && retention.elapsed() < 180000) {
        // A little Sync activity, stored as one burst ...
        for (int index = 0; index < 20; ++index) {
            journal.appendEvent(live, QJsonObject {{QStringLiteral("phase"), QStringLiteral("uploading")}, {QStringLiteral("message"), QStringLiteral("Uploading %1/%2").arg(round).arg(index)}});
        }
        journal.flush();
        ++round;
        // ... then quiet, which retention uses, while the status area keeps asking.
        QElapsedTimer quiet;
        quiet.start();
        while (quiet.elapsed() < 200) {
            answer.start();
            QCOMPARE(journal.latestGlobalSummary().value(QStringLiteral("id")).toString(), live);
            slowestAnswerMs = qMax(slowestAnswerMs, answer.elapsed());
            ++answers;
            QTest::qWait(3);
        }
    }
    const qint64 retentionMs = retention.elapsed();
    journal.updateOperation(live, SyncOperationState::Succeeded, QStringLiteral("succeeded"), QStringLiteral("Completed"));
    // The run just made is the newest; the others kept are the newest seeded ones.
    QTRY_VERIFY_WITH_TIMEOUT(journal.compactionPending(), 60000);
    SyncJournal::Statistics pruned = journal.statistics();
    qInfo() << "Retention removed" << before.runs - pruned.runs + 1 << "runs and" << before.events - pruned.events << "events in" << retentionMs << "ms in the background;" << answers
            << "reads meanwhile, the slowest" << slowestAnswerMs << "ms; the file is still" << pruned.fileBytes / (1024 * 1024) << "MiB with" << pruned.freePages * pruned.pageSize / (1024 * 1024) << "MiB free inside";
    QCOMPARE(pruned.runs, qint64(SyncJournal::RetentionMinimumOperations));
    QVERIFY(pruned.events < before.events);
    QVERIFY2(slowestAnswerMs < 500, "A read must not wait for retention");
    // Without the rewrite the freed pages stay in the file.
    QCOMPARE(pruned.autoVacuum, 0);
    QVERIFY(pruned.fileBytes >= before.fileBytes);

    // The one rewrite. The status area is answered from memory meanwhile.
    QSignalSpy compacted(&journal, &SyncJournal::compactionFinished);
    timer.restart();
    journal.compact();
    QVERIFY(journal.compactionRunning());
    answer.start();
    QCOMPARE(journal.latestGlobalSummary().value(QStringLiteral("id")).toString(), live);
    QVERIFY(journal.lastSuccessfulGlobalSyncAt().isValid());
    const qint64 answerDuringCompactionMs = answer.elapsed();
    QVERIFY(compacted.wait(300000));
    const qint64 compactionMs = timer.elapsed();
    QVERIFY(!journal.compactionRunning());
    QVERIFY(!journal.compactionPending());
    const SyncJournal::Statistics compactedStatistics = journal.statistics();
    qInfo() << "Compaction took" << compactionMs << "ms on the journal's thread (status answered in" << answerDuringCompactionMs << "ms meanwhile):" << before.fileBytes / (1024 * 1024) << "MiB ->"
            << compactedStatistics.fileBytes / (1024 * 1024) << "MiB";
    QCOMPARE(compactedStatistics.autoVacuum, 2);
    QVERIFY(compactedStatistics.errorIndex);
    QCOMPARE(compactedStatistics.runs, pruned.runs);
    QCOMPARE(compactedStatistics.events, pruned.events);
    QVERIFY(compactedStatistics.fileBytes < before.fileBytes);
    QVERIFY2(answerDuringCompactionMs < 100, "The status area must not wait for the compaction");
    QCOMPARE(journal.latestGlobalSummary().value(QStringLiteral("status")).toString(), QStringLiteral("succeeded"));

    // From now on retention shrinks the file by itself. A second journal on the
    // file with a shorter rule keeps the runs of the last 30 days only.
    SyncJournal::Policy shorter = quietPolicy();
    shorter.retentionMinimumOperations = 500;
    SyncJournal second(path, nullptr, shorter);
    second.runMaintenanceNow();
    const SyncJournal::Statistics later = second.statistics();
    qInfo() << "A later retention pass:" << compactedStatistics.fileBytes / (1024 * 1024) << "MiB ->" << later.fileBytes / (1024 * 1024) << "MiB," << later.runs << "runs kept";
    QVERIFY2(later.runs > 1290 && later.runs < 1340, qPrintable(QString::number(later.runs)));
    QVERIFY(later.fileBytes < compactedStatistics.fileBytes);
    QVERIFY(later.freePages < shorter.reclaimPagesPerStep);
}

// What this build does to a journal that an earlier build wrote, measured on a
// copy of one: THOUGHTS_SYNCJOURNAL_TEST_COPY names the copy, which is changed.
void SyncJournalTest::testCopyOfAnExistingJournal()
{
    const QString path = qEnvironmentVariable("THOUGHTS_SYNCJOURNAL_TEST_COPY");
    if (path.isEmpty()) {
        QSKIP("Set THOUGHTS_SYNCJOURNAL_TEST_COPY to a copy of a Sync journal to measure it.");
    }
    QVERIFY(QFileInfo(path).isFile());
    QElapsedTimer timer;
    timer.start();
    SyncJournal journal(path, nullptr, quietPolicy());
    const QJsonObject latest = journal.latestGlobalSummary();
    const qint64 firstAnswerMs = timer.restart();
    const SyncJournal::Statistics before = journal.statistics();
    qInfo() << "Existing journal:" << before.runs << "runs," << before.events << "events," << before.fileBytes / (1024 * 1024) << "MiB, auto_vacuum" << before.autoVacuum << "; first answer after" << firstAnswerMs << "ms";
    QVERIFY(!latest.isEmpty());
    timer.restart();
    const SyncJournal::Page page = pageOf(journal, SyncJournal::PageRequest());
    const qint64 pageMs = timer.restart();
    const QJsonArray recent = journal.recentOperations(false);
    const qint64 recentMs = timer.restart();
    qInfo() << "Logs page of" << page.operations.size() << "runs:" << pageMs << "ms; the 500 runs earlier builds listed:" << recentMs << "ms";
    QVERIFY(!page.operations.isEmpty());
    QVERIFY(!recent.isEmpty());

    timer.restart();
    journal.runMaintenanceNow();
    const qint64 retentionMs = timer.restart();
    const SyncJournal::Statistics pruned = journal.statistics();
    qInfo() << "Retention:" << before.runs - pruned.runs << "runs and" << before.events - pruned.events << "events removed in" << retentionMs << "ms of work (spread out in the app);" << pruned.runs << "runs and"
            << pruned.events << "events kept";
    QVERIFY(pruned.runs <= before.runs);
    if (journal.compactionPending()) {
        QSignalSpy compacted(&journal, &SyncJournal::compactionFinished);
        timer.restart();
        journal.compact();
        QVERIFY(compacted.wait(600000));
        const qint64 compactionMs = timer.elapsed();
        const SyncJournal::Statistics after = journal.statistics();
        qInfo() << "Compaction:" << compactionMs << "ms;" << before.fileBytes / (1024 * 1024) << "MiB ->" << after.fileBytes / (1024 * 1024) << "MiB";
        QCOMPARE(after.autoVacuum, 2);
        QVERIFY(after.errorIndex);
        QCOMPARE(after.runs, pruned.runs);
        QCOMPARE(after.events, pruned.events);
    }
    QCOMPARE(journal.latestGlobalSummary(), latest);
}

QTEST_MAIN(SyncJournalTest)
#include "syncjournaltest.moc"
