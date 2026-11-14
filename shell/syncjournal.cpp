/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "syncjournal.h"

#include "../part/thoughtsplatform.h"
#include "developerlog.h"
#include "syncstatuslabels.h"
#include "thoughtscommandprotocol.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <deque>
#include <tuple>

namespace Thoughts
{
namespace
{
// One write's changes inside the worker's burst transaction: a write that fails
// takes back only what it changed itself. Outside a burst it is a transaction.
class WriteScope
{
public:
    explicit WriteScope(QSqlDatabase &database)
        : m_database(database)
    {
        m_open = database.isValid() && database.isOpen() && QSqlQuery(database).exec(QStringLiteral("SAVEPOINT journal_write"));
    }
    ~WriteScope()
    {
        rollback();
    }
    bool isOpen() const
    {
        return m_open;
    }
    bool commit()
    {
        if (!m_open) {
            return false;
        }
        m_open = false;
        return QSqlQuery(m_database).exec(QStringLiteral("RELEASE journal_write"));
    }
    void rollback()
    {
        if (!m_open) {
            return;
        }
        m_open = false;
        QSqlQuery query(m_database);
        query.exec(QStringLiteral("ROLLBACK TO journal_write"));
        query.exec(QStringLiteral("RELEASE journal_write"));
    }

private:
    QSqlDatabase &m_database;
    bool m_open = false;
};

// The words the journal stores and compares, translated once on the thread
// that creates the first journal: the worker thread never asks the translation
// catalogue, which must first be used on the application's own thread.
struct JournalLabels {
    QString building = i18n("Building…");
    QString waitingToPublish = i18n("Waiting to publish sources…");
    QString buildFailed = i18n("Build failed");
    QString operationFailed = i18n("Operation failed");
    QString syncFailed = SyncStatusLabels::syncFailed();
    QString syncing = SyncStatusLabels::syncing();
    QString interrupted = i18n("Interrupted when Thoughts restarted");
    QString buildStarted = i18n("Build started");
    QString buildRetryStarted = i18n("Build retry started");
    QString syncStarted = i18n("Sync started");
    QString syncRetryStarted = i18n("Sync retry started");
};

const JournalLabels &labels()
{
    static const JournalLabels instance;
    return instance;
}

bool usable(const QSqlDatabase &database)
{
    return database.isValid() && database.isOpen();
}

QString legacySyncOperationsPath(const QString &journalPath)
{
    return QFileInfo(journalPath).absoluteDir().filePath(QStringLiteral("sync-operations.json"));
}

QString normalizedSyncInitiationControl(const QString &control)
{
    static const QRegularExpression validControl(QStringLiteral("^[a-z0-9_]{1,64}$"));
    return validControl.match(control).hasMatch() ? control : QStringLiteral("unknown");
}

QString legacySyncOperationPhase(const QString &status, const QString &event = {})
{
    const QString detail = event.toLower();
    const SyncOperationState lifecycle = syncOperationState(QJsonObject {{QStringLiteral("status"), status}});
    if (lifecycle == SyncOperationState::Succeeded) {
        return QStringLiteral("succeeded");
    }
    if (lifecycle == SyncOperationState::Failed || lifecycle == SyncOperationState::Interrupted || lifecycle == SyncOperationState::Cancelled) {
        return detail.contains(QStringLiteral("interrupt")) ? QStringLiteral("interrupted") : QStringLiteral("failed");
    }
    if (status == labels().building || status == QLatin1String("Building…") || detail.contains(QStringLiteral("build"))) {
        return QStringLiteral("building");
    }
    if (status == labels().waitingToPublish || detail.contains(QStringLiteral("publish")) || detail.contains(QStringLiteral("upload"))) {
        return QStringLiteral("uploading");
    }
    if (detail.contains(QStringLiteral("prepar")) || detail.contains(QStringLiteral("preflight"))) {
        return QStringLiteral("preflight");
    }
    if (detail.contains(QStringLiteral("verify"))) {
        return QStringLiteral("verifying");
    }
    if (detail.contains(QStringLiteral("activat")) || detail.contains(QStringLiteral("install"))) {
        return QStringLiteral("activating");
    }
    if (detail.contains(QStringLiteral("download")) || detail.contains(QStringLiteral("transfer")) || detail.contains(QStringLiteral("artifact"))) {
        return QStringLiteral("downloading");
    }
    if (detail.contains(QStringLiteral("network"))) {
        return QStringLiteral("waiting_network");
    }
    return QStringLiteral("queued");
}

QJsonObject normalizedStoredSyncOperation(QJsonObject operation)
{
    const SyncOperationState lifecycle = syncOperationState(operation);
    if (lifecycle == SyncOperationState::Unknown) {
        return operation;
    }
    operation.insert(QStringLiteral("status"), syncOperationStateId(lifecycle));
    if (lifecycle != SyncOperationState::Running) {
        operation.insert(QStringLiteral("phase"), syncOperationStateId(lifecycle));
    }
    return operation;
}

int syncOperationPhaseOrdinal(const QString &phase)
{
    static const QStringList phases {QStringLiteral("queued"),
                                     QStringLiteral("checking_server"),
                                     QStringLiteral("preflight"),
                                     QStringLiteral("uploading"),
                                     QStringLiteral("waiting_network"),
                                     QStringLiteral("waiting_user"),
                                     QStringLiteral("building"),
                                     QStringLiteral("downloading"),
                                     QStringLiteral("verifying"),
                                     QStringLiteral("activating")};
    const int index = phases.indexOf(phase);
    return index < 0 ? phases.size() : index;
}

QJsonObject syncOperationPayloadWithoutEvents(QJsonObject operation)
{
    operation.remove(QStringLiteral("events"));
    return operation;
}

bool insertEmbeddedSyncOperationEvent(QSqlDatabase &database, const QString &runId, const QJsonObject &operation, const QJsonObject &embeddedEvent)
{
    const QString message = embeddedEvent.value(QStringLiteral("message")).toString();
    const QString phase = embeddedEvent.value(QStringLiteral("phase")).toString(legacySyncOperationPhase(embeddedEvent.value(QStringLiteral("state")).toString(), message)).left(64);
    QJsonObject eventLifecycle {{QStringLiteral("status"), embeddedEvent.value(QStringLiteral("state")).toString()}, {QStringLiteral("phase"), phase}};
    SyncOperationState lifecycle = syncOperationState(eventLifecycle);
    if (lifecycle == SyncOperationState::Unknown) {
        lifecycle = SyncOperationState::Running;
    }
    const QString state = syncOperationStateId(lifecycle);
    const QString origin = embeddedEvent.value(QStringLiteral("origin")).toString() == QLatin1String("server") ? QStringLiteral("server") : QStringLiteral("desktop");
    const qint64 sequence = qMax<qint64>(embeddedEvent.value(QStringLiteral("server_sequence")).toVariant().toLongLong(), embeddedEvent.value(QStringLiteral("sequence")).toVariant().toLongLong());
    const qint64 fallbackAt = operation.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong();
    const qint64 embeddedAt = embeddedEvent.value(QStringLiteral("at_ms")).toVariant().toLongLong();
    const qint64 atMs = embeddedAt > 0 ? embeddedAt : fallbackAt;
    const QString errorCode = embeddedEvent.value(QStringLiteral("error_code")).toString().left(128);

    // Older Desktop builds wrote every local event both here and inside
    // payload_json.  Check the complete stable identity so extracting an old
    // payload is idempotent even when its matching journal row already exists.
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral("SELECT 1 FROM sync_events WHERE run_id = ? AND origin = ? AND at_ms = ? AND phase = ? AND message = ? AND COALESCE(server_sequence, -1) = ? LIMIT 1"));
    existing.addBindValue(runId);
    existing.addBindValue(origin);
    existing.addBindValue(atMs);
    existing.addBindValue(phase);
    existing.addBindValue(message.left(origin == QLatin1String("server") ? 4000 : 600));
    existing.addBindValue(sequence > 0 ? sequence : -1);
    if (!existing.exec()) {
        return false;
    }
    if (existing.next()) {
        return true;
    }

    QJsonObject metadata = embeddedEvent.value(QStringLiteral("metadata")).toObject();
    for (const QString &field : {QStringLiteral("path"), QStringLiteral("target"), QStringLiteral("build_target")}) {
        if (embeddedEvent.contains(field)) {
            metadata.insert(field, embeddedEvent.value(field));
        }
    }
    const QVariant bytesCompleted = embeddedEvent.contains(QStringLiteral("bytes_completed")) ? embeddedEvent.value(QStringLiteral("bytes_completed")).toVariant() : embeddedEvent.value(QStringLiteral("bytes_transferred")).toVariant();
    const QVariant bytesTotal = embeddedEvent.contains(QStringLiteral("bytes_total")) ? embeddedEvent.value(QStringLiteral("bytes_total")).toVariant() : embeddedEvent.value(QStringLiteral("total_bytes")).toVariant();
    const qint64 serverTimestampMs = embeddedEvent.value(QStringLiteral("server_timestamp_ms")).toVariant().toLongLong();
    const qint64 receivedAtMs = embeddedEvent.value(QStringLiteral("received_at_ms")).toVariant().toLongLong();
    QSqlQuery insert(database);
    insert.prepare(
        QStringLiteral("INSERT INTO sync_events(run_id, at_ms, phase, state, attempt, message, duration_ms, error_code, origin, server_sequence, server_timestamp_ms, received_at_ms, component, build_id, completed, total, bytes_completed, "
                       "bytes_total, metadata_json, terminal) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(runId);
    insert.addBindValue(atMs);
    insert.addBindValue(phase);
    insert.addBindValue(state);
    insert.addBindValue(embeddedEvent.value(QStringLiteral("attempt")).toInt(operation.value(QStringLiteral("attempt")).toInt()));
    insert.addBindValue(message.left(origin == QLatin1String("server") ? 4000 : 600));
    insert.addBindValue(embeddedEvent.value(QStringLiteral("duration_ms")).toVariant());
    insert.addBindValue(errorCode);
    insert.addBindValue(origin);
    insert.addBindValue(sequence > 0 ? QVariant(sequence) : QVariant());
    insert.addBindValue(origin == QLatin1String("server") ? (serverTimestampMs > 0 ? serverTimestampMs : atMs) : QVariant());
    insert.addBindValue(origin == QLatin1String("server") ? (receivedAtMs > 0 ? receivedAtMs : atMs) : QVariant());
    insert.addBindValue(embeddedEvent.value(QStringLiteral("component")).toString(origin).left(64));
    insert.addBindValue(embeddedEvent.value(QStringLiteral("build_id")).toString().left(128));
    insert.addBindValue(embeddedEvent.value(QStringLiteral("completed")).toVariant());
    insert.addBindValue(embeddedEvent.value(QStringLiteral("total")).toVariant());
    insert.addBindValue(bytesCompleted);
    insert.addBindValue(bytesTotal);
    insert.addBindValue(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    insert.addBindValue(embeddedEvent.value(QStringLiteral("terminal")).toBool());
    return insert.exec();
}

bool normalizeEmbeddedSyncOperationEvents(QSqlDatabase &database)
{
    WriteScope scope(database);
    if (!scope.isOpen()) {
        return false;
    }
    QList<QPair<QString, QJsonObject>> embeddedPayloads;
    {
        QSqlQuery select(database);
        if (!select.exec(QStringLiteral("SELECT id, payload_json FROM sync_runs WHERE payload_json LIKE '%\"events\"%'"))) {
            return false;
        }
        while (select.next()) {
            const QJsonDocument document = QJsonDocument::fromJson(select.value(1).toString().toUtf8());
            if (document.isObject() && document.object().contains(QStringLiteral("events"))) {
                embeddedPayloads.append({select.value(0).toString(), document.object()});
            }
        }
    }

    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE sync_runs SET payload_json = ? WHERE id = ?"));
    for (const auto &[runId, embeddedPayload] : std::as_const(embeddedPayloads)) {
        for (const QJsonValue &eventValue : embeddedPayload.value(QStringLiteral("events")).toArray()) {
            if (eventValue.isObject() && !insertEmbeddedSyncOperationEvent(database, runId, embeddedPayload, eventValue.toObject())) {
                return false;
            }
        }
        const QJsonObject compactPayload = syncOperationPayloadWithoutEvents(embeddedPayload);
        update.bindValue(0, QString::fromUtf8(QJsonDocument(compactPayload).toJson(QJsonDocument::Compact)));
        update.bindValue(1, runId);
        if (!update.exec() || update.numRowsAffected() != 1) {
            return false;
        }
    }
    update.finish();
    return scope.commit();
}

bool hasIndex(QSqlDatabase &database, const QString &name)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?"));
    query.addBindValue(name);
    return query.exec() && query.next();
}

// Finds the events that carry an error code without reading every event of a
// run. Statements name its condition (error_code != '') so SQLite may use it.
const QString eventErrorIndexName = QStringLiteral("sync_events_run_error");

bool createEventErrorIndex(QSqlDatabase &database)
{
    QSqlQuery query(database);
    return query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS sync_events_run_error ON sync_events(run_id, error_code) WHERE error_code != ''"));
}

int pragmaInt(QSqlDatabase &database, const QString &name)
{
    QSqlQuery query(database);
    return query.exec(QStringLiteral("PRAGMA %1").arg(name)) && query.next() ? query.value(0).toInt() : 0;
}

qint64 scalar(QSqlDatabase &database, const QString &statement)
{
    QSqlQuery query(database);
    return query.exec(statement) && query.next() ? query.value(0).toLongLong() : 0;
}

QString metadataValue(QSqlDatabase &database, const QString &key)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM sync_metadata WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool setMetadataValue(QSqlDatabase &database, const QString &key, const QString &value)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_metadata(key, value) VALUES(?, ?)"));
    query.addBindValue(key);
    query.addBindValue(value);
    return query.exec();
}

bool openJournal(QSqlDatabase &database, const QString &path, const QString &connectionName, qint64 deferredIndexEvents)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(info.absoluteFilePath());
    if (!database.open()) {
        qWarning() << "Could not open the Thoughts synchronization journal:" << database.lastError().text();
        return false;
    }
    QSqlQuery query(database);
    // A journal that does not exist yet can return its free pages from the start.
    query.exec(QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL"));
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA synchronous=FULL"));
    query.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    const QStringList schema {
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_runs (id TEXT PRIMARY KEY, trace_id TEXT NOT NULL UNIQUE, retry_of TEXT, kind TEXT NOT NULL, target TEXT, status TEXT NOT NULL, phase TEXT NOT NULL, created_at_ms INTEGER NOT NULL, "
                       "started_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, completed_at_ms INTEGER, duration_ms INTEGER, build_id TEXT, payload_json TEXT NOT NULL, server_state TEXT NOT NULL DEFAULT '', server_terminal "
                       "INTEGER NOT NULL DEFAULT 0, last_server_sequence INTEGER NOT NULL DEFAULT 0, stream_state TEXT NOT NULL DEFAULT 'offline', transfer_metrics_recorded INTEGER NOT NULL DEFAULT 0, uploaded_completed_bytes INTEGER NOT "
                       "NULL DEFAULT 0, uploaded_total_bytes INTEGER NOT NULL DEFAULT 0, downloaded_completed_bytes INTEGER NOT NULL DEFAULT 0, downloaded_total_bytes INTEGER NOT NULL DEFAULT 0, initiation_control TEXT NOT NULL DEFAULT 'unknown')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_state_updated ON sync_runs(status, updated_at_ms)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_target_updated ON sync_runs(target, updated_at_ms)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_global_success ON sync_runs(kind, phase, target, completed_at_ms, updated_at_ms)"),
        // The status bar asks for the newest Sync on every progress event.
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_kind_updated ON sync_runs(kind, updated_at_ms)"),
        // The Logs window reads the newest runs page by page; retention reads the oldest.
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_created ON sync_runs(created_at_ms, id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_runs_updated ON sync_runs(updated_at_ms)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_steps (run_id TEXT NOT NULL, ordinal INTEGER NOT NULL, phase TEXT NOT NULL, state TEXT NOT NULL, attempt INTEGER NOT NULL DEFAULT 0, request_id TEXT, started_at_ms INTEGER NOT NULL, "
                       "updated_at_ms INTEGER NOT NULL, deadline_at_ms INTEGER, facts_json TEXT, PRIMARY KEY(run_id, ordinal))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_events (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL, at_ms INTEGER NOT NULL, phase TEXT NOT NULL, state TEXT NOT NULL, attempt INTEGER NOT NULL DEFAULT 0, message TEXT "
                       "NOT NULL, duration_ms INTEGER, error_code TEXT, origin TEXT NOT NULL DEFAULT 'desktop', server_sequence INTEGER, server_timestamp_ms INTEGER, received_at_ms INTEGER, component TEXT NOT NULL DEFAULT 'desktop', "
                       "build_id TEXT, completed INTEGER, total INTEGER, bytes_completed INTEGER, bytes_total INTEGER, metadata_json TEXT NOT NULL DEFAULT '{}', terminal INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sync_events_run_time ON sync_events(run_id, at_ms)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
    };
    for (const QString &statement : schema) {
        if (!query.exec(statement)) {
            qWarning() << "Could not initialize the Thoughts synchronization journal:" << query.lastError().text();
            return false;
        }
    }
    const auto ensureColumn = [&database](const QString &table, const QString &column, const QString &definition) {
        QSqlQuery columns(database);
        if (!columns.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            return false;
        }
        while (columns.next()) {
            if (columns.value(1).toString() == column) {
                return true;
            }
        }
        QSqlQuery alter(database);
        return alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition));
    };
    const QList<std::tuple<QString, QString, QString>> migrations {
        {QStringLiteral("sync_runs"), QStringLiteral("server_state"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("sync_runs"), QStringLiteral("server_terminal"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("last_server_sequence"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("stream_state"), QStringLiteral("TEXT NOT NULL DEFAULT 'offline'")},
        {QStringLiteral("sync_runs"), QStringLiteral("transfer_metrics_recorded"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("uploaded_completed_bytes"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("uploaded_total_bytes"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("downloaded_completed_bytes"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("downloaded_total_bytes"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("sync_runs"), QStringLiteral("initiation_control"), QStringLiteral("TEXT NOT NULL DEFAULT 'unknown'")},
        {QStringLiteral("sync_events"), QStringLiteral("origin"), QStringLiteral("TEXT NOT NULL DEFAULT 'desktop'")},
        {QStringLiteral("sync_events"), QStringLiteral("server_sequence"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("server_timestamp_ms"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("received_at_ms"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("component"), QStringLiteral("TEXT NOT NULL DEFAULT 'desktop'")},
        {QStringLiteral("sync_events"), QStringLiteral("build_id"), QStringLiteral("TEXT")},
        {QStringLiteral("sync_events"), QStringLiteral("completed"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("total"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("bytes_completed"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("bytes_total"), QStringLiteral("INTEGER")},
        {QStringLiteral("sync_events"), QStringLiteral("metadata_json"), QStringLiteral("TEXT NOT NULL DEFAULT '{}'")},
        {QStringLiteral("sync_events"), QStringLiteral("terminal"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
    };
    for (const auto &[table, column, definition] : migrations) {
        if (!ensureColumn(table, column, definition)) {
            qWarning() << "Could not migrate the Thoughts synchronization journal column" << table << column;
            return false;
        }
    }
    query.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS sync_events_server_sequence ON sync_events(run_id, origin, server_sequence)"));
    // Indexing a large journal's events takes about a second: compact() does it
    // while nobody waits. Until then the statements read a run's events instead.
    if (hasIndex(database, eventErrorIndexName) || scalar(database, QStringLiteral("SELECT COUNT(*) FROM (SELECT 1 FROM sync_events LIMIT %1)").arg(deferredIndexEvents + 1)) <= deferredIndexEvents) {
        createEventErrorIndex(database);
    }
    if (metadataValue(database, QStringLiteral("embedded_events_extracted_v1")).isEmpty()) {
        if (!normalizeEmbeddedSyncOperationEvents(database)) {
            qWarning() << "Could not migrate embedded Thoughts synchronization events";
            return false;
        }
        if (!setMetadataValue(database, QStringLiteral("embedded_events_extracted_v1"), QStringLiteral("1"))) {
            qWarning() << "Could not record the embedded Thoughts synchronization event migration";
            return false;
        }
    }
    return true;
}

QJsonArray readLegacySyncOperations(const QString &journalPath)
{
    QFile file(legacySyncOperationsPath(journalPath));
    QJsonArray kept;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isArray()) {
            const qint64 cutoff = QDateTime::currentDateTimeUtc().addDays(-90).toMSecsSinceEpoch();
            for (const QJsonValue &value : document.array()) {
                const QJsonObject operation = value.toObject();
                if (!operation.value(QStringLiteral("id")).toString().isEmpty() && operation.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong() >= cutoff) {
                    kept.append(operation);
                    if (kept.size() == 500) {
                        break;
                    }
                }
            }
        }
    }
    return kept;
}

bool writeSyncOperations(QSqlDatabase &database, const QJsonArray &operations)
{
    WriteScope scope(database);
    if (!scope.isOpen()) {
        qWarning() << "Could not start a Thoughts synchronization journal transaction:" << database.lastError().text();
        return false;
    }
    QSqlQuery insertRun(database);
    insertRun.prepare(QStringLiteral(
        "INSERT INTO sync_runs(id, trace_id, retry_of, kind, target, status, phase, created_at_ms, started_at_ms, updated_at_ms, completed_at_ms, duration_ms, build_id, payload_json, server_state, server_terminal, last_server_sequence, "
        "stream_state, transfer_metrics_recorded, uploaded_completed_bytes, uploaded_total_bytes, downloaded_completed_bytes, downloaded_total_bytes, initiation_control) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON "
        "CONFLICT(id) DO UPDATE SET trace_id=excluded.trace_id, retry_of=excluded.retry_of, kind=excluded.kind, target=excluded.target, status=excluded.status, phase=excluded.phase, created_at_ms=excluded.created_at_ms, "
        "started_at_ms=excluded.started_at_ms, updated_at_ms=MAX(sync_runs.updated_at_ms, excluded.updated_at_ms), completed_at_ms=excluded.completed_at_ms, duration_ms=excluded.duration_ms, build_id=COALESCE(NULLIF(excluded.build_id, "
        "''), sync_runs.build_id), payload_json=excluded.payload_json, transfer_metrics_recorded=excluded.transfer_metrics_recorded, uploaded_completed_bytes=excluded.uploaded_completed_bytes, "
        "uploaded_total_bytes=excluded.uploaded_total_bytes, downloaded_completed_bytes=excluded.downloaded_completed_bytes, downloaded_total_bytes=excluded.downloaded_total_bytes, initiation_control=excluded.initiation_control"));
    QSqlQuery insertStep(database);
    insertStep.prepare(QStringLiteral("INSERT INTO sync_steps(run_id, ordinal, phase, state, attempt, started_at_ms, updated_at_ms) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    QSqlQuery clearEvents(database);
    clearEvents.prepare(QStringLiteral("DELETE FROM sync_events WHERE run_id = ?"));
    QSqlQuery clearSteps(database);
    clearSteps.prepare(QStringLiteral("DELETE FROM sync_steps WHERE run_id = ?"));
    for (const QJsonValue &value : operations) {
        QJsonObject operation = normalizedStoredSyncOperation(value.toObject());
        const QString id = operation.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        const QString status = operation.value(QStringLiteral("status")).toString();
        const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
        const QString lastEvent = events.isEmpty() ? QString() : events.at(events.size() - 1).toObject().value(QStringLiteral("message")).toString();
        const QString phase = operation.value(QStringLiteral("phase")).toString(legacySyncOperationPhase(status, lastEvent));
        operation.insert(QStringLiteral("trace_id"), id);
        operation.insert(QStringLiteral("status"), status);
        operation.insert(QStringLiteral("phase"), phase);
        const bool transferMetricsRecorded = operation.value(QStringLiteral("transfer_metrics_recorded")).toBool();
        const qint64 uploadedCompleted = transferMetricsRecorded ? qMax<qint64>(0, operation.value(QStringLiteral("uploaded_completed_bytes")).toVariant().toLongLong()) : 0;
        const qint64 uploadedTotal = transferMetricsRecorded ? qMax(uploadedCompleted, operation.value(QStringLiteral("uploaded_total_bytes")).toVariant().toLongLong()) : 0;
        const qint64 downloadedCompleted = transferMetricsRecorded ? qMax<qint64>(0, operation.value(QStringLiteral("downloaded_completed_bytes")).toVariant().toLongLong()) : 0;
        const qint64 downloadedTotal = transferMetricsRecorded ? qMax(downloadedCompleted, operation.value(QStringLiteral("downloaded_total_bytes")).toVariant().toLongLong()) : 0;
        operation.insert(QStringLiteral("transfer_metrics_recorded"), transferMetricsRecorded);
        operation.insert(QStringLiteral("uploaded_completed_bytes"), uploadedCompleted);
        operation.insert(QStringLiteral("uploaded_total_bytes"), uploadedTotal);
        operation.insert(QStringLiteral("downloaded_completed_bytes"), downloadedCompleted);
        operation.insert(QStringLiteral("downloaded_total_bytes"), downloadedTotal);
        const QString initiationControl = normalizedSyncInitiationControl(operation.value(QStringLiteral("initiation_control")).toString());
        operation.insert(QStringLiteral("initiation_control"), initiationControl);
        if (transferMetricsRecorded) {
            operation.insert(QStringLiteral("bytes_transferred"), uploadedCompleted + downloadedCompleted);
            operation.insert(QStringLiteral("total_bytes"), uploadedTotal + downloadedTotal);
        }
        insertRun.bindValue(0, id);
        insertRun.bindValue(1, id);
        insertRun.bindValue(2, operation.value(QStringLiteral("retry_of")).toString());
        insertRun.bindValue(3, operation.value(QStringLiteral("kind")).toString());
        insertRun.bindValue(4, operation.value(QStringLiteral("target")).toString());
        insertRun.bindValue(5, status);
        insertRun.bindValue(6, phase);
        insertRun.bindValue(7, operation.value(QStringLiteral("created_at_ms")).toVariant());
        insertRun.bindValue(8, operation.value(QStringLiteral("started_at_ms")).toVariant());
        insertRun.bindValue(9, operation.value(QStringLiteral("updated_at_ms")).toVariant());
        insertRun.bindValue(10, operation.value(QStringLiteral("completed_at_ms")).toVariant());
        insertRun.bindValue(11, operation.value(QStringLiteral("duration_ms")).toVariant());
        insertRun.bindValue(12, operation.value(QStringLiteral("build_id")).toString());
        insertRun.bindValue(13, QString::fromUtf8(QJsonDocument(syncOperationPayloadWithoutEvents(operation)).toJson(QJsonDocument::Compact)));
        insertRun.bindValue(14, operation.value(QStringLiteral("server_state")).toString(QStringLiteral("")));
        insertRun.bindValue(15, operation.value(QStringLiteral("server_terminal")).toBool());
        insertRun.bindValue(16, operation.value(QStringLiteral("last_server_sequence")).toVariant().toLongLong());
        insertRun.bindValue(17, operation.value(QStringLiteral("stream_state")).toString(QStringLiteral("offline")));
        insertRun.bindValue(18, transferMetricsRecorded);
        insertRun.bindValue(19, uploadedCompleted);
        insertRun.bindValue(20, uploadedTotal);
        insertRun.bindValue(21, downloadedCompleted);
        insertRun.bindValue(22, downloadedTotal);
        insertRun.bindValue(23, initiationControl);
        if (!insertRun.exec()) {
            qWarning() << "Could not write Thoughts synchronization run" << id << insertRun.lastError().text();
            return false;
        }
        clearEvents.bindValue(0, id);
        clearSteps.bindValue(0, id);
        if (!clearEvents.exec() || !clearSteps.exec()) {
            qWarning() << "Could not replace Thoughts synchronization run details" << id << database.lastError().text();
            return false;
        }
        for (const QJsonValue &eventValue : events) {
            if (!eventValue.isObject() || !insertEmbeddedSyncOperationEvent(database, id, operation, eventValue.toObject())) {
                qWarning() << "Could not write a Thoughts synchronization event for" << id;
                return false;
            }
        }
        insertStep.bindValue(0, id);
        insertStep.bindValue(1, syncOperationPhaseOrdinal(phase));
        insertStep.bindValue(2, phase);
        insertStep.bindValue(3, status);
        insertStep.bindValue(4, operation.value(QStringLiteral("attempt")).toInt());
        insertStep.bindValue(5, operation.value(QStringLiteral("started_at_ms")).toVariant());
        insertStep.bindValue(6, operation.value(QStringLiteral("updated_at_ms")).toVariant());
        if (!insertStep.exec()) {
            qWarning() << "Could not write a Thoughts synchronization step for" << id << insertStep.lastError().text();
            return false;
        }
    }
    insertRun.finish();
    insertStep.finish();
    clearEvents.finish();
    clearSteps.finish();
    if (!scope.commit()) {
        qWarning() << "Could not commit the Thoughts synchronization journal:" << database.lastError().text();
        return false;
    }
    return true;
}

// The columns every list of runs reads, in the order operationFromRunRow expects.
const QString runListColumns = QStringLiteral("id, payload_json, status, phase, server_state, server_terminal, last_server_sequence, stream_state, build_id, transfer_metrics_recorded, uploaded_completed_bytes, uploaded_total_bytes, "
                                              "downloaded_completed_bytes, downloaded_total_bytes, initiation_control, created_at_ms");

// A run as the Logs window lists it, without its events. Empty when the row's payload is unreadable.
QJsonObject operationFromRunRow(const QSqlQuery &query)
{
    const QJsonDocument document = QJsonDocument::fromJson(query.value(1).toString().toUtf8());
    if (!document.isObject()) {
        return {};
    }
    QJsonObject operation = document.object();
    const QString operationId = query.value(0).toString();
    operation.insert(QStringLiteral("id"), operationId);
    operation.insert(QStringLiteral("trace_id"), operation.value(QStringLiteral("trace_id")).toString(operationId));
    operation.insert(QStringLiteral("status"), query.value(2).toString());
    operation.insert(QStringLiteral("phase"), query.value(3).toString());
    operation.insert(QStringLiteral("server_state"), query.value(4).toString());
    operation.insert(QStringLiteral("server_terminal"), query.value(5).toBool());
    operation.insert(QStringLiteral("last_server_sequence"), query.value(6).toLongLong());
    operation.insert(QStringLiteral("stream_state"), query.value(7).toString());
    if (!query.value(8).toString().isEmpty()) {
        operation.insert(QStringLiteral("build_id"), query.value(8).toString());
    }
    operation.insert(QStringLiteral("transfer_metrics_recorded"), query.value(9).toBool());
    operation.insert(QStringLiteral("uploaded_completed_bytes"), query.value(10).toLongLong());
    operation.insert(QStringLiteral("uploaded_total_bytes"), query.value(11).toLongLong());
    operation.insert(QStringLiteral("downloaded_completed_bytes"), query.value(12).toLongLong());
    operation.insert(QStringLiteral("downloaded_total_bytes"), query.value(13).toLongLong());
    operation.insert(QStringLiteral("initiation_control"), normalizedSyncInitiationControl(query.value(14).toString()));
    operation.insert(QStringLiteral("needs_source_conflict_resolution"), false);
    operation.remove(QStringLiteral("events"));
    return operation;
}

// Marks the runs that reported a source conflict. The runs are chosen first and
// only theirs are looked up: asking per row of an unsorted list read every
// event of the journal.
void markSourceConflicts(QSqlDatabase &database, QList<QJsonObject> &operations)
{
    // SQLite accepts 999 parameters in older builds.
    constexpr qsizetype chunkSize = 500;
    QSet<QString> conflicted;
    for (qsizetype offset = 0; offset < operations.size(); offset += chunkSize) {
        const qsizetype count = qMin(chunkSize, operations.size() - offset);
        QStringList placeholders;
        placeholders.fill(QStringLiteral("?"), count);
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT DISTINCT run_id FROM sync_events WHERE run_id IN (%1) AND error_code = 'invalid_source_publication' AND error_code != ''").arg(placeholders.join(QLatin1Char(','))));
        for (qsizetype index = 0; index < count; ++index) {
            query.addBindValue(operations.at(offset + index).value(QStringLiteral("id")).toString());
        }
        if (query.exec()) {
            while (query.next()) {
                conflicted.insert(query.value(0).toString());
            }
        }
    }
    if (conflicted.isEmpty()) {
        return;
    }
    for (QJsonObject &operation : operations) {
        if (conflicted.contains(operation.value(QStringLiteral("id")).toString())) {
            operation.insert(QStringLiteral("needs_source_conflict_resolution"), true);
        }
    }
}

const QString eventColumns = QStringLiteral("run_id, at_ms, phase, state, attempt, message, duration_ms, error_code, origin, server_sequence, server_timestamp_ms, received_at_ms, component, build_id, completed, total, "
                                            "bytes_completed, bytes_total, metadata_json, terminal");

QJsonObject eventFromRow(const QSqlQuery &journalEvents)
{
    const bool serverOrigin = journalEvents.value(8).toString() == QLatin1String("server");
    const qint64 serverTimestamp = journalEvents.value(10).toLongLong();
    QJsonObject event {{QStringLiteral("at_ms"), serverOrigin && serverTimestamp > 0 ? serverTimestamp : journalEvents.value(1).toLongLong()},
                       {QStringLiteral("phase"), journalEvents.value(2).toString()},
                       {QStringLiteral("state"), journalEvents.value(3).toString()},
                       {QStringLiteral("attempt"), journalEvents.value(4).toInt()},
                       {QStringLiteral("message"), journalEvents.value(5).toString()},
                       {QStringLiteral("duration_ms"), QJsonValue::fromVariant(journalEvents.value(6))},
                       {QStringLiteral("error_code"), journalEvents.value(7).toString()},
                       {QStringLiteral("origin"), journalEvents.value(8).toString()},
                       {QStringLiteral("server_sequence"), journalEvents.value(9).toLongLong()},
                       {QStringLiteral("received_at_ms"), journalEvents.value(11).toLongLong()},
                       {QStringLiteral("component"), journalEvents.value(12).toString()},
                       {QStringLiteral("build_id"), journalEvents.value(13).toString()},
                       {QStringLiteral("completed"), QJsonValue::fromVariant(journalEvents.value(14))},
                       {QStringLiteral("total"), QJsonValue::fromVariant(journalEvents.value(15))},
                       {QStringLiteral("bytes_transferred"), QJsonValue::fromVariant(journalEvents.value(16))},
                       {QStringLiteral("total_bytes"), QJsonValue::fromVariant(journalEvents.value(17))},
                       {QStringLiteral("terminal"), journalEvents.value(19).toBool()}};
    const QJsonDocument metadata = QJsonDocument::fromJson(journalEvents.value(18).toString().toUtf8());
    if (metadata.isObject()) {
        event.insert(QStringLiteral("metadata"), metadata.object());
    }
    return event;
}

// Events in the order the Logs window shows them: by time, then by the
// server's sequence; events equal in both keep the order they were read in.
QJsonArray orderedEvents(const QJsonArray &events)
{
    QList<QJsonObject> ordered;
    ordered.reserve(events.size());
    for (const QJsonValue &event : events) {
        ordered.append(event.toObject());
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const qint64 atA = a.value(QStringLiteral("at_ms")).toVariant().toLongLong();
        const qint64 atB = b.value(QStringLiteral("at_ms")).toVariant().toLongLong();
        if (atA != atB) {
            return atA < atB;
        }
        return a.value(QStringLiteral("server_sequence")).toVariant().toLongLong() < b.value(QStringLiteral("server_sequence")).toVariant().toLongLong();
    });
    QJsonArray result;
    for (const QJsonObject &event : std::as_const(ordered)) {
        result.append(event);
    }
    return result;
}

QJsonArray readSyncOperations(QSqlDatabase &database, const QString &journalPath, const QString &requestedOperationId, bool includeHistory)
{
    if (!usable(database)) {
        return {};
    }
    const qint64 cutoff = QDateTime::currentDateTimeUtc().addDays(-90).toMSecsSinceEpoch();
    QList<QJsonObject> storedOperations;
    QStringList operationIds;
    {
        QSqlQuery query(database);
        const QString selection = requestedOperationId.isEmpty() ? QStringLiteral("updated_at_ms >= ? ORDER BY created_at_ms DESC LIMIT 500") : QStringLiteral("id = ? OR trace_id = ?");
        query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE %2").arg(runListColumns, selection));
        if (requestedOperationId.isEmpty()) {
            query.addBindValue(cutoff);
        } else {
            query.addBindValue(requestedOperationId);
            query.addBindValue(requestedOperationId);
        }
        if (query.exec()) {
            while (query.next()) {
                const QJsonObject operation = operationFromRunRow(query);
                if (!operation.isEmpty()) {
                    operationIds.append(operation.value(QStringLiteral("id")).toString());
                    storedOperations.append(operation);
                }
            }
        }
    }
    markSourceConflicts(database, storedOperations);

    // The list needs only summaries. The detail window reads the selected run,
    // so live trace updates never deserialize every other run in the journal.
    QHash<QString, QJsonArray> eventsByRun;
    if (includeHistory && !operationIds.isEmpty()) {
        constexpr qsizetype chunkSize = 500;
        for (qsizetype offset = 0; offset < operationIds.size(); offset += chunkSize) {
            const qsizetype count = qMin(chunkSize, operationIds.size() - offset);
            QStringList placeholders;
            placeholders.fill(QStringLiteral("?"), count);
            QSqlQuery journalEvents(database);
            journalEvents.prepare(QStringLiteral("SELECT %1 FROM sync_events WHERE run_id IN (%2) ORDER BY run_id, COALESCE(server_timestamp_ms, at_ms), id").arg(eventColumns, placeholders.join(QLatin1Char(','))));
            for (qsizetype index = 0; index < count; ++index) {
                journalEvents.addBindValue(operationIds.at(offset + index));
            }
            if (journalEvents.exec()) {
                while (journalEvents.next()) {
                    eventsByRun[journalEvents.value(0).toString()].append(eventFromRow(journalEvents));
                }
            }
        }
    }

    QJsonArray operations;
    for (QJsonObject operation : std::as_const(storedOperations)) {
        if (!includeHistory) {
            operations.append(normalizedStoredSyncOperation(operation));
            continue;
        }
        operation.insert(QStringLiteral("events"), orderedEvents(eventsByRun.value(operation.value(QStringLiteral("id")).toString())));
        operations.append(normalizedStoredSyncOperation(operation));
    }
    if (metadataValue(database, QStringLiteral("legacy_json_imported")).isEmpty()) {
        if (operations.isEmpty() && requestedOperationId.isEmpty()) {
            const QJsonArray legacy = readLegacySyncOperations(journalPath);
            if (!legacy.isEmpty() && writeSyncOperations(database, legacy)) {
                setMetadataValue(database, QStringLiteral("legacy_json_imported"), QStringLiteral("1"));
                return readSyncOperations(database, journalPath, requestedOperationId, includeHistory);
            }
        }
        setMetadataValue(database, QStringLiteral("legacy_json_imported"), QStringLiteral("1"));
    }
    return operations;
}

SyncJournal::Page readOperationPage(QSqlDatabase &database, const SyncJournal::PageRequest &request)
{
    SyncJournal::Page page;
    if (!usable(database)) {
        return page;
    }
    const int limit = qBound(1, request.limit, 1000);
    QSet<QString> listed;
    {
        QSqlQuery query(database);
        const bool first = request.beforeId.isEmpty() && request.beforeCreatedAtMs <= 0;
        query.prepare(QStringLiteral("SELECT %1 FROM sync_runs %2 ORDER BY created_at_ms DESC, id DESC LIMIT ?")
                          .arg(runListColumns, first ? QString() : QStringLiteral("WHERE created_at_ms < ? OR (created_at_ms = ? AND id < ?)")));
        if (!first) {
            query.addBindValue(request.beforeCreatedAtMs);
            query.addBindValue(request.beforeCreatedAtMs);
            query.addBindValue(request.beforeId);
        }
        // One more than asked for says whether an older page exists.
        query.addBindValue(limit + 1);
        if (!query.exec()) {
            return page;
        }
        int rows = 0;
        while (query.next()) {
            if (++rows > limit) {
                page.hasOlder = true;
                break;
            }
            page.oldestCreatedAtMs = query.value(15).toLongLong();
            page.oldestId = query.value(0).toString();
            const QJsonObject operation = operationFromRunRow(query);
            if (!operation.isEmpty()) {
                listed.insert(page.oldestId);
                page.operations.append(operation);
            }
        }
    }
    for (const QString &name : request.named) {
        if (name.isEmpty() || listed.contains(name)) {
            continue;
        }
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE id = ? OR trace_id = ? LIMIT 1").arg(runListColumns));
        query.addBindValue(name);
        query.addBindValue(name);
        if (query.exec() && query.next() && !listed.contains(query.value(0).toString())) {
            const QJsonObject operation = operationFromRunRow(query);
            if (!operation.isEmpty()) {
                listed.insert(query.value(0).toString());
                page.named.append(operation);
            }
        }
    }
    markSourceConflicts(database, page.operations);
    markSourceConflicts(database, page.named);
    for (QJsonObject &operation : page.operations) {
        operation = normalizedStoredSyncOperation(operation);
    }
    for (QJsonObject &operation : page.named) {
        operation = normalizedStoredSyncOperation(operation);
    }
    return page;
}

SyncJournal::History readOperationHistory(QSqlDatabase &database, const QString &operationId, qint64 afterEventRow)
{
    SyncJournal::History history;
    if (!usable(database) || operationId.isEmpty()) {
        return history;
    }
    QList<QJsonObject> operations;
    {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE id = ? OR trace_id = ? LIMIT 1").arg(runListColumns));
        query.addBindValue(operationId);
        query.addBindValue(operationId);
        if (!query.exec() || !query.next()) {
            return history;
        }
        const QJsonObject operation = operationFromRunRow(query);
        if (operation.isEmpty()) {
            return history;
        }
        operations.append(operation);
    }
    markSourceConflicts(database, operations);
    history.found = true;
    history.operation = operations.constFirst();
    QSqlQuery events(database);
    events.prepare(QStringLiteral("SELECT %1, id, COALESCE(server_timestamp_ms, at_ms) FROM sync_events WHERE run_id = ? AND id > ? ORDER BY id").arg(eventColumns));
    events.addBindValue(history.operation.value(QStringLiteral("id")).toString());
    events.addBindValue(afterEventRow);
    if (events.exec()) {
        while (events.next()) {
            SyncJournal::Event event;
            event.row = events.value(20).toLongLong();
            event.orderMs = events.value(21).toLongLong();
            event.json = eventFromRow(events);
            history.events.append(event);
        }
    }
    return history;
}

const QString runSummaryColumns = QStringLiteral("id, trace_id, retry_of, kind, target, status, phase, created_at_ms, started_at_ms, updated_at_ms, completed_at_ms, duration_ms, build_id, payload_json, server_state, server_terminal, "
                                                 "last_server_sequence, stream_state, transfer_metrics_recorded, uploaded_completed_bytes, uploaded_total_bytes, downloaded_completed_bytes, downloaded_total_bytes, initiation_control");

QJsonObject syncOperationSummaryFromCurrentRow(const QSqlQuery &query)
{
    const QJsonDocument payload = QJsonDocument::fromJson(query.value(13).toString().toUtf8());
    // A restored older journal may still embed its history in the payload.
    QJsonObject operation = payload.isObject() ? syncOperationPayloadWithoutEvents(payload.object()) : QJsonObject();
    operation.insert(QStringLiteral("id"), query.value(0).toString());
    operation.insert(QStringLiteral("trace_id"), query.value(1).toString());
    operation.insert(QStringLiteral("retry_of"), query.value(2).toString());
    operation.insert(QStringLiteral("kind"), query.value(3).toString());
    operation.insert(QStringLiteral("target"), query.value(4).toString());
    operation.insert(QStringLiteral("status"), query.value(5).toString());
    operation.insert(QStringLiteral("phase"), query.value(6).toString());
    operation.insert(QStringLiteral("created_at_ms"), query.value(7).toLongLong());
    operation.insert(QStringLiteral("started_at_ms"), query.value(8).toLongLong());
    operation.insert(QStringLiteral("updated_at_ms"), query.value(9).toLongLong());
    if (!query.value(10).isNull()) {
        operation.insert(QStringLiteral("completed_at_ms"), query.value(10).toLongLong());
    }
    if (!query.value(11).isNull()) {
        operation.insert(QStringLiteral("duration_ms"), query.value(11).toLongLong());
    }
    if (!query.value(12).toString().isEmpty()) {
        operation.insert(QStringLiteral("build_id"), query.value(12).toString());
    }
    operation.insert(QStringLiteral("server_state"), query.value(14).toString());
    operation.insert(QStringLiteral("server_terminal"), query.value(15).toBool());
    operation.insert(QStringLiteral("last_server_sequence"), query.value(16).toLongLong());
    operation.insert(QStringLiteral("stream_state"), query.value(17).toString());
    operation.insert(QStringLiteral("transfer_metrics_recorded"), query.value(18).toBool());
    operation.insert(QStringLiteral("uploaded_completed_bytes"), query.value(19).toLongLong());
    operation.insert(QStringLiteral("uploaded_total_bytes"), query.value(20).toLongLong());
    operation.insert(QStringLiteral("downloaded_completed_bytes"), query.value(21).toLongLong());
    operation.insert(QStringLiteral("downloaded_total_bytes"), query.value(22).toLongLong());
    operation.insert(QStringLiteral("initiation_control"), normalizedSyncInitiationControl(query.value(23).toString()));
    return normalizedStoredSyncOperation(operation);
}

QJsonObject readSyncOperationSummary(QSqlDatabase &database, const QString &operationId)
{
    if (operationId.isEmpty() || !usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE id = ? OR trace_id = ? LIMIT 1").arg(runSummaryColumns));
    query.addBindValue(operationId);
    query.addBindValue(operationId);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return syncOperationSummaryFromCurrentRow(query);
}

QList<QJsonObject> runningSyncOperationSummaries(QSqlDatabase &database)
{
    if (!usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id, payload_json, status, phase, kind, target FROM sync_runs WHERE completed_at_ms IS NULL AND updated_at_ms >= ? ORDER BY updated_at_ms DESC"));
    query.addBindValue(QDateTime::currentDateTimeUtc().addDays(-90).toMSecsSinceEpoch());
    if (!query.exec()) {
        return {};
    }
    QList<QJsonObject> operations;
    while (query.next()) {
        const QJsonDocument payload = QJsonDocument::fromJson(query.value(1).toString().toUtf8());
        QJsonObject operation = payload.isObject() ? payload.object() : QJsonObject();
        operation.insert(QStringLiteral("id"), query.value(0).toString());
        operation.insert(QStringLiteral("status"), query.value(2).toString());
        operation.insert(QStringLiteral("phase"), query.value(3).toString());
        operation.insert(QStringLiteral("kind"), query.value(4).toString());
        operation.insert(QStringLiteral("target"), query.value(5).toString());
        if (syncOperationIsRunning(operation)) {
            operations.append(operation);
        }
    }
    return operations;
}

QDateTime readLastSuccessfulGlobalSyncAt(QSqlDatabase &database)
{
    if (!usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT completed_at_ms, updated_at_ms FROM sync_runs WHERE kind = 'sync' AND phase = 'succeeded' AND (target IS NULL OR target = '' OR target = 'All documents') ORDER BY COALESCE(completed_at_ms, updated_at_ms) DESC LIMIT 1"));
    if (!query.exec() || !query.next()) {
        return {};
    }
    const qint64 completedMs = query.value(0).toLongLong();
    const qint64 updatedMs = query.value(1).toLongLong();
    const qint64 latestMs = completedMs > 0 ? completedMs : updatedMs;
    return latestMs > 0 ? QDateTime::fromMSecsSinceEpoch(latestMs, Qt::UTC) : QDateTime();
}

QJsonObject readLatestGlobalSyncOperationSummary(QSqlDatabase &database)
{
    if (!usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE kind = 'sync' AND (target IS NULL OR target = '' OR target = 'All documents') ORDER BY updated_at_ms DESC LIMIT 1").arg(runSummaryColumns));
    return query.exec() && query.next() ? syncOperationSummaryFromCurrentRow(query) : QJsonObject();
}

QJsonObject readLatestDocumentSyncOperationSummary(QSqlDatabase &database, const QString &target)
{
    if (!usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM sync_runs WHERE kind = 'sync' AND (target = ? OR target IS NULL OR target = '' OR target = 'All documents') ORDER BY updated_at_ms DESC LIMIT 1").arg(runSummaryColumns));
    query.addBindValue(target);
    return query.exec() && query.next() ? syncOperationSummaryFromCurrentRow(query) : QJsonObject();
}

QString readRunningBuildOperationIdForTarget(QSqlDatabase &database, const QString &target)
{
    if (!usable(database)) {
        return {};
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id, payload_json, status, phase FROM sync_runs WHERE kind = 'build' AND target = ? AND completed_at_ms IS NULL ORDER BY updated_at_ms DESC"));
    query.addBindValue(target);
    if (!query.exec()) {
        return {};
    }
    while (query.next()) {
        QJsonObject operation = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
        operation.insert(QStringLiteral("status"), query.value(2).toString());
        operation.insert(QStringLiteral("phase"), query.value(3).toString());
        if (syncOperationIsRunning(operation)) {
            return query.value(0).toString();
        }
    }
    return {};
}

QString syncOperationStartedMessage(const QString &kind, bool retry)
{
    if (kind == QLatin1String("build")) {
        return retry ? labels().buildRetryStarted : labels().buildStarted;
    }
    return retry ? labels().syncRetryStarted : labels().syncStarted;
}

bool insertLocalSyncOperationEvent(QSqlDatabase &database, const QString &runId, const QJsonObject &event, int defaultAttempt = 0)
{
    QJsonObject metadata = event.value(QStringLiteral("metadata")).toObject();
    for (const QString &field : {QStringLiteral("path"), QStringLiteral("target")}) {
        if (event.contains(field)) {
            metadata.insert(field, event.value(field));
        }
    }
    QSqlQuery insert(database);
    insert.prepare(
        QStringLiteral("INSERT INTO sync_events(run_id, at_ms, phase, state, attempt, message, duration_ms, error_code, origin, component, build_id, completed, total, bytes_completed, bytes_total, metadata_json, terminal) VALUES(?, ?, ?, "
                       "?, ?, ?, ?, ?, 'desktop', ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(runId);
    insert.addBindValue(event.value(QStringLiteral("at_ms")).toVariant());
    insert.addBindValue(event.value(QStringLiteral("phase")).toString().left(64));
    insert.addBindValue(event.value(QStringLiteral("state")).toString(QStringLiteral("running")).left(32));
    insert.addBindValue(event.value(QStringLiteral("attempt")).toInt(defaultAttempt));
    insert.addBindValue(event.value(QStringLiteral("message")).toString().left(600));
    insert.addBindValue(event.value(QStringLiteral("duration_ms")).toVariant());
    insert.addBindValue(event.value(QStringLiteral("error_code")).toString().left(64));
    insert.addBindValue(event.value(QStringLiteral("component")).toString(QStringLiteral("desktop")).left(64));
    insert.addBindValue(event.value(QStringLiteral("build_id")).toString().left(128));
    insert.addBindValue(event.value(QStringLiteral("completed")).toVariant());
    insert.addBindValue(event.value(QStringLiteral("total")).toVariant());
    insert.addBindValue(event.value(QStringLiteral("bytes_transferred")).toVariant());
    insert.addBindValue(event.value(QStringLiteral("total_bytes")).toVariant());
    insert.addBindValue(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
    insert.addBindValue(event.value(QStringLiteral("terminal")).toBool());
    return insert.exec();
}

bool upsertSyncOperationStep(QSqlDatabase &database, const QString &runId, const QString &phase, const QString &state, int attempt, qint64 startedAt, qint64 updatedAt)
{
    QSqlQuery step(database);
    step.prepare(
        QStringLiteral("INSERT INTO sync_steps(run_id, ordinal, phase, state, attempt, started_at_ms, updated_at_ms) VALUES(?, ?, ?, ?, ?, ?, ?) ON CONFLICT(run_id, ordinal) DO UPDATE SET phase=excluded.phase, state=excluded.state, "
                       "attempt=excluded.attempt, updated_at_ms=excluded.updated_at_ms"));
    step.addBindValue(runId);
    step.addBindValue(syncOperationPhaseOrdinal(phase));
    step.addBindValue(phase);
    step.addBindValue(state);
    step.addBindValue(attempt);
    step.addBindValue(startedAt);
    step.addBindValue(updatedAt);
    return step.exec();
}

// Old runs are not removed here any more: that read every event of the journal
// on the first Sync of a process. Background retention removes them.
QString beginSyncOperation(QSqlDatabase &database, const QString &kind, const QString &target, const QString &phase, const QString &initiationControl, const QString &retryOf)
{
    WriteScope scope(database);
    if (!scope.isOpen()) {
        return {};
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString storedPhase = phase.isEmpty() ? QStringLiteral("queued") : phase.left(64);
    const QString storedInitiationControl = normalizedSyncInitiationControl(initiationControl);
    const QJsonObject startedEvent {
        {QStringLiteral("at_ms"), now},
        {QStringLiteral("phase"), QStringLiteral("queued")},
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("message"), syncOperationStartedMessage(kind, !retryOf.isEmpty())},
        {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("initiation_control"), storedInitiationControl}}}};
    QJsonObject operation {{QStringLiteral("id"), id},
                           {QStringLiteral("trace_id"), id},
                           {QStringLiteral("kind"), kind},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("initiation_control"), storedInitiationControl},
                           {QStringLiteral("status"), QStringLiteral("running")},
                           {QStringLiteral("phase"), storedPhase},
                           {QStringLiteral("created_at_ms"), now},
                           {QStringLiteral("started_at_ms"), now},
                           {QStringLiteral("updated_at_ms"), now},
                           {QStringLiteral("server_state"), QStringLiteral("")},
                           {QStringLiteral("server_terminal"), false},
                           {QStringLiteral("last_server_sequence"), 0},
                           {QStringLiteral("stream_state"), QStringLiteral("offline")},
                           {QStringLiteral("transfer_metrics_recorded"), true},
                           {QStringLiteral("uploaded_completed_bytes"), 0},
                           {QStringLiteral("uploaded_total_bytes"), 0},
                           {QStringLiteral("downloaded_completed_bytes"), 0},
                           {QStringLiteral("downloaded_total_bytes"), 0},
                           {QStringLiteral("bytes_transferred"), 0},
                           {QStringLiteral("total_bytes"), 0}};
    if (!retryOf.isEmpty()) {
        operation.insert(QStringLiteral("retry_of"), retryOf);
    }
    {
        QSqlQuery insert(database);
        insert.prepare(
            QStringLiteral("INSERT INTO sync_runs(id, trace_id, retry_of, kind, target, status, phase, created_at_ms, started_at_ms, updated_at_ms, completed_at_ms, duration_ms, build_id, payload_json, server_state, server_terminal, "
                           "last_server_sequence, stream_state, transfer_metrics_recorded, uploaded_completed_bytes, uploaded_total_bytes, downloaded_completed_bytes, downloaded_total_bytes, initiation_control) VALUES(?, ?, ?, ?, ?, 'running', ?, ?, ?, ?, NULL, "
                           "NULL, NULL, ?, '', 0, 0, 'offline', 1, 0, 0, 0, 0, ?)"));
        insert.addBindValue(id);
        insert.addBindValue(id);
        insert.addBindValue(retryOf);
        insert.addBindValue(kind);
        insert.addBindValue(target);
        insert.addBindValue(storedPhase);
        insert.addBindValue(now);
        insert.addBindValue(now);
        insert.addBindValue(now);
        insert.addBindValue(QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact)));
        insert.addBindValue(storedInitiationControl);
        if (!insert.exec() || !insertLocalSyncOperationEvent(database, id, startedEvent) || !upsertSyncOperationStep(database, id, storedPhase, QStringLiteral("running"), 0, now, now)) {
            return {};
        }
    }
    if (!scope.commit()) {
        return {};
    }
    DeveloperLog::record(QStringLiteral("operation.started"), {{QStringLiteral("operation_id"), id}, {QStringLiteral("kind"), kind}, {QStringLiteral("phase"), storedPhase}});
    return id;
}

void updateSyncOperation(QSqlDatabase &database, const QString &id, SyncOperationState state, const QString &phase, const QString &event, const QString &buildId, const QString &errorCode,
                         const QString &warningCode = {}, const QString &warning = {})
{
    if (id.isEmpty()) {
        return;
    }
    WriteScope scope(database);
    if (!scope.isOpen()) {
        return;
    }
    QJsonObject operation;
    QVariant storedCompletedAt;
    QVariant storedDurationValue;
    {
        QSqlQuery select(database);
        select.prepare(QStringLiteral("SELECT payload_json, status, phase, created_at_ms, started_at_ms, completed_at_ms, duration_ms, kind, build_id FROM sync_runs WHERE id = ? OR trace_id = ? LIMIT 1"));
        select.addBindValue(id);
        select.addBindValue(id);
        if (!select.exec() || !select.next()) {
            return;
        }
        const QJsonDocument payload = QJsonDocument::fromJson(select.value(0).toString().toUtf8());
        if (!payload.isObject()) {
            return;
        }
        operation = syncOperationPayloadWithoutEvents(payload.object());
        operation.insert(QStringLiteral("id"), operation.value(QStringLiteral("id")).toString(id));
        operation.insert(QStringLiteral("trace_id"), operation.value(QStringLiteral("trace_id")).toString(id));
        operation.insert(QStringLiteral("status"), select.value(1).toString());
        operation.insert(QStringLiteral("phase"), select.value(2).toString());
        operation.insert(QStringLiteral("created_at_ms"), select.value(3).toLongLong());
        operation.insert(QStringLiteral("started_at_ms"), select.value(4).toLongLong());
        operation.insert(QStringLiteral("kind"), select.value(7).toString());
        if (!select.value(8).toString().isEmpty()) {
            operation.insert(QStringLiteral("build_id"), select.value(8).toString());
        }
        storedCompletedAt = select.value(5);
        storedDurationValue = select.value(6);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString storedPhase = phase.left(64);
    const QString storedState = syncOperationStateId(state);
    const bool terminal = state == SyncOperationState::Succeeded || state == SyncOperationState::Failed || state == SyncOperationState::Interrupted || state == SyncOperationState::Cancelled;
    const bool failed = state == SyncOperationState::Failed || state == SyncOperationState::Interrupted || state == SyncOperationState::Cancelled;
    const qint64 startedAt = qMax<qint64>(0, operation.value(QStringLiteral("started_at_ms")).toVariant().toLongLong());
    const qint64 durationMs = terminal ? qMax<qint64>(0, now - startedAt) : storedDurationValue.toLongLong();
    const QVariant completedAt = terminal ? QVariant(now) : storedCompletedAt;
    const QVariant storedDuration = terminal ? QVariant(durationMs) : storedDurationValue;
    const QString runId = operation.value(QStringLiteral("id")).toString(id);

    QString safeEvent = event.left(600);
    QString resolvedErrorCode = errorCode.left(64);
    if (failed && resolvedErrorCode.isEmpty()) {
        QSqlQuery previousFailure(database);
        previousFailure.prepare(QStringLiteral("SELECT message, error_code FROM sync_events WHERE run_id = ? AND error_code IS NOT NULL AND error_code != '' ORDER BY COALESCE(server_timestamp_ms, at_ms) DESC, id DESC LIMIT 1"));
        previousFailure.addBindValue(runId);
        if (previousFailure.exec() && previousFailure.next()) {
            resolvedErrorCode = previousFailure.value(1).toString().left(64);
            if (safeEvent == labels().buildFailed || safeEvent == labels().syncFailed || safeEvent == labels().operationFailed) {
                safeEvent = previousFailure.value(0).toString().left(600);
            }
        }
        if (resolvedErrorCode.isEmpty()) {
            resolvedErrorCode = operation.value(QStringLiteral("kind")).toString() == QLatin1String("build") ? QStringLiteral("build_failed") : QStringLiteral("sync_failed");
        }
    }

    const SyncOperationState previousState = syncOperationState(operation);
    QJsonObject previousEvent;
    {
        QSqlQuery previousEventQuery(database);
        previousEventQuery.prepare(QStringLiteral("SELECT phase, state, message, error_code FROM sync_events WHERE run_id = ? AND origin = 'desktop' ORDER BY at_ms DESC, id DESC LIMIT 1"));
        previousEventQuery.addBindValue(runId);
        if (previousEventQuery.exec() && previousEventQuery.next()) {
            previousEvent.insert(QStringLiteral("phase"), previousEventQuery.value(0).toString());
            previousEvent.insert(QStringLiteral("state"), previousEventQuery.value(1).toString());
            previousEvent.insert(QStringLiteral("message"), previousEventQuery.value(2).toString());
            if (!previousEventQuery.value(3).toString().isEmpty()) {
                previousEvent.insert(QStringLiteral("error_code"), previousEventQuery.value(3).toString());
            }
        }
    }
    const bool unchangedProgress = (safeEvent == labels().syncing || safeEvent == labels().waitingToPublish || safeEvent == labels().building) && state == previousState;
    const bool duplicateEvent = previousEvent.value(QStringLiteral("phase")).toString() == storedPhase && previousEvent.value(QStringLiteral("state")).toString() == storedState &&
        previousEvent.value(QStringLiteral("message")).toString() == safeEvent && previousEvent.value(QStringLiteral("error_code")).toString() == resolvedErrorCode;
    const bool appendEvent = !safeEvent.isEmpty() && !duplicateEvent && !unchangedProgress;
    QJsonObject eventObject;
    if (appendEvent) {
        eventObject = {{QStringLiteral("at_ms"), now}, {QStringLiteral("phase"), storedPhase}, {QStringLiteral("state"), storedState}, {QStringLiteral("message"), safeEvent}, {QStringLiteral("terminal"), terminal}};
        if (terminal) {
            eventObject.insert(QStringLiteral("duration_ms"), durationMs);
        }
        if (!resolvedErrorCode.isEmpty()) {
            eventObject.insert(QStringLiteral("error_code"), resolvedErrorCode);
        }
        if (!buildId.isEmpty()) {
            eventObject.insert(QStringLiteral("build_id"), buildId.left(128));
        }
    }
    operation.insert(QStringLiteral("status"), storedState);
    operation.insert(QStringLiteral("phase"), storedPhase);
    operation.insert(QStringLiteral("updated_at_ms"), now);
    if (terminal) {
        operation.insert(QStringLiteral("completed_at_ms"), now);
        operation.insert(QStringLiteral("duration_ms"), durationMs);
    }
    if (failed) {
        operation.insert(QStringLiteral("last_error_message"), safeEvent);
        operation.insert(QStringLiteral("last_error_code"), resolvedErrorCode);
    }
    // What a Sync that succeeded still has to say; a run that ends otherwise has none.
    if (state == SyncOperationState::Succeeded && !warningCode.isEmpty()) {
        operation.insert(QStringLiteral("warning_code"), warningCode.left(64));
        operation.insert(QStringLiteral("warning_message"), warning.left(4000));
    } else if (terminal) {
        operation.remove(QStringLiteral("warning_code"));
        operation.remove(QStringLiteral("warning_message"));
    }
    if (!buildId.isEmpty()) {
        operation.insert(QStringLiteral("build_id"), buildId.left(128));
    }

    {
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE sync_runs SET status = ?, phase = ?, updated_at_ms = ?, completed_at_ms = ?, duration_ms = ?, build_id = COALESCE(NULLIF(?, ''), build_id), payload_json = ? WHERE id = ?"));
        update.addBindValue(storedState);
        update.addBindValue(storedPhase);
        update.addBindValue(now);
        update.addBindValue(completedAt);
        update.addBindValue(storedDuration);
        update.addBindValue(buildId.left(128));
        update.addBindValue(QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact)));
        update.addBindValue(runId);
        if (!update.exec() || update.numRowsAffected() != 1 || (appendEvent && !insertLocalSyncOperationEvent(database, runId, eventObject, operation.value(QStringLiteral("attempt")).toInt())) ||
            !upsertSyncOperationStep(database, runId, storedPhase, storedState, operation.value(QStringLiteral("attempt")).toInt(), startedAt, now)) {
            return;
        }
    }
    if (scope.commit() && appendEvent) {
        DeveloperLog::record(QStringLiteral("operation.updated"), {{QStringLiteral("operation_id"), id}, {QStringLiteral("phase"), storedPhase},
            {QStringLiteral("state"), storedState}, {QStringLiteral("duration_ms"), durationMs}});
    }
}

void reconcileInterruptedSyncOperations(QSqlDatabase &database, qint64 startedBeforeMs)
{
    if (!usable(database)) {
        return;
    }
    QStringList runningIds;
    {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT id, payload_json, status, phase FROM sync_runs WHERE completed_at_ms IS NULL AND updated_at_ms >= ? AND started_at_ms < ?"));
        query.addBindValue(QDateTime::currentDateTimeUtc().addDays(-90).toMSecsSinceEpoch());
        query.addBindValue(startedBeforeMs);
        if (!query.exec()) {
            return;
        }
        while (query.next()) {
            QJsonObject operation = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
            operation.insert(QStringLiteral("status"), query.value(2).toString());
            operation.insert(QStringLiteral("phase"), query.value(3).toString());
            if (syncOperationIsRunning(operation)) {
                runningIds.append(query.value(0).toString());
            }
        }
    }
    for (const QString &id : std::as_const(runningIds)) {
        updateSyncOperation(database, id, SyncOperationState::Interrupted, QStringLiteral("interrupted"), labels().interrupted, QString(), QStringLiteral("desktop_restarted"));
    }
}

bool persistServerSyncOperationEvent(QSqlDatabase &database, const QString &id, const QJsonObject &incomingEvent)
{
    WriteScope scope(database);
    if (id.isEmpty() || !scope.isOpen()) {
        return false;
    }
    const QString streamState = incomingEvent.value(QStringLiteral("stream_state")).toString();
    const qint64 sequence = incomingEvent.value(QStringLiteral("sequence")).toVariant().toLongLong();
    if (sequence <= 0) {
        if (!streamState.isEmpty()) {
            QJsonObject compactPayload;
            {
                QSqlQuery payload(database);
                payload.prepare(QStringLiteral("SELECT payload_json FROM sync_runs WHERE id = ?"));
                payload.addBindValue(id);
                if (!payload.exec() || !payload.next()) {
                    return false;
                }
                const QJsonDocument document = QJsonDocument::fromJson(payload.value(0).toString().toUtf8());
                compactPayload = document.isObject() ? syncOperationPayloadWithoutEvents(document.object()) : QJsonObject();
            }
            QSqlQuery update(database);
            update.prepare(QStringLiteral("UPDATE sync_runs SET stream_state = ?, payload_json = ? WHERE id = ?"));
            update.addBindValue(streamState.left(32));
            update.addBindValue(QString::fromUtf8(QJsonDocument(compactPayload).toJson(QJsonDocument::Compact)));
            update.addBindValue(id);
            if (!update.exec()) {
                return false;
            }
        }
        return scope.commit();
    }
    QDateTime occurredAt = QDateTime::fromString(incomingEvent.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!occurredAt.isValid()) {
        occurredAt = QDateTime::fromString(incomingEvent.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
    }
    const qint64 receivedAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 atMs = occurredAt.isValid() ? occurredAt.toUTC().toMSecsSinceEpoch() : receivedAt;
    const QJsonObject metadata = incomingEvent.value(QStringLiteral("metadata")).toObject();
    QString payloadJson;
    {
        QSqlQuery payloadQuery(database);
        payloadQuery.prepare(QStringLiteral("SELECT payload_json FROM sync_runs WHERE id = ?"));
        payloadQuery.addBindValue(id);
        if (!payloadQuery.exec() || !payloadQuery.next()) {
            return false;
        }
        const QJsonDocument payloadDocument = QJsonDocument::fromJson(payloadQuery.value(0).toString().toUtf8());
        if (!payloadDocument.isObject()) {
            return false;
        }
        QJsonObject operationPayload = syncOperationPayloadWithoutEvents(payloadDocument.object());
        QJsonObject eventIdentity;
        mergeSyncEventIdentity(operationPayload, eventIdentity, incomingEvent);
        payloadJson = QString::fromUtf8(QJsonDocument(operationPayload).toJson(QJsonDocument::Compact));
    }
    {
        QSqlQuery insert(database);
        insert.prepare(
            QStringLiteral("INSERT OR IGNORE INTO sync_events(run_id, at_ms, phase, state, attempt, message, error_code, origin, server_sequence, server_timestamp_ms, received_at_ms, component, build_id, completed, total, bytes_completed, "
                           "bytes_total, metadata_json, terminal) VALUES(?, ?, ?, ?, 0, ?, ?, 'server', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        insert.addBindValue(id);
        insert.addBindValue(atMs);
        insert.addBindValue(incomingEvent.value(QStringLiteral("phase")).toString().left(64));
        insert.addBindValue(incomingEvent.value(QStringLiteral("state")).toString().left(32));
        insert.addBindValue(incomingEvent.value(QStringLiteral("message")).toString().simplified().left(4000));
        insert.addBindValue(incomingEvent.value(QStringLiteral("error_code")).toString().left(128));
        insert.addBindValue(sequence);
        insert.addBindValue(atMs);
        insert.addBindValue(receivedAt);
        insert.addBindValue(incomingEvent.value(QStringLiteral("component")).toString().left(64));
        insert.addBindValue(incomingEvent.value(QStringLiteral("build_id")).toString().left(128));
        insert.addBindValue(incomingEvent.value(QStringLiteral("completed")).toVariant());
        insert.addBindValue(incomingEvent.value(QStringLiteral("total")).toVariant());
        insert.addBindValue(incomingEvent.value(QStringLiteral("bytes_completed")).toVariant());
        insert.addBindValue(incomingEvent.value(QStringLiteral("bytes_total")).toVariant());
        insert.addBindValue(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)));
        insert.addBindValue(incomingEvent.value(QStringLiteral("terminal")).toBool());
        if (!insert.exec()) {
            return false;
        }
    }
    {
        QSqlQuery update(database);
        update.prepare(
            QStringLiteral("UPDATE sync_runs SET server_state = ?, server_terminal = ?, last_server_sequence = ?, stream_state = ?, updated_at_ms = MAX(updated_at_ms, ?), build_id = COALESCE(NULLIF(?, ''), build_id), payload_json = ? WHERE id "
                           "= ? AND last_server_sequence < ?"));
        update.addBindValue(incomingEvent.value(QStringLiteral("state")).toString().left(32));
        update.addBindValue(incomingEvent.value(QStringLiteral("terminal")).toBool());
        update.addBindValue(sequence);
        update.addBindValue(streamState.isEmpty() ? QStringLiteral("live") : streamState.left(32));
        update.addBindValue(receivedAt);
        update.addBindValue(incomingEvent.value(QStringLiteral("build_id")).toString().left(128));
        update.addBindValue(payloadJson);
        update.addBindValue(id);
        update.addBindValue(sequence);
        if (!update.exec()) {
            return false;
        }
    }
    return scope.commit();
}

bool updateStoredSyncOperationTransferProgress(QSqlDatabase &database, const QString &traceId, const QJsonObject &incomingSnapshot)
{
    QJsonObject snapshot;
    if (traceId.isEmpty() || !normalizedSyncTransferSnapshot(incomingSnapshot, &snapshot)) {
        return false;
    }
    WriteScope scope(database);
    if (!scope.isOpen()) {
        return false;
    }
    QString operationId;
    QJsonObject operation;
    {
        QSqlQuery select(database);
        select.prepare(QStringLiteral("SELECT id, payload_json FROM sync_runs WHERE trace_id = ? OR id = ? LIMIT 1"));
        select.addBindValue(traceId);
        select.addBindValue(traceId);
        if (!select.exec() || !select.next()) {
            return false;
        }
        operationId = select.value(0).toString();
        const QJsonDocument document = QJsonDocument::fromJson(select.value(1).toString().toUtf8());
        operation = document.isObject() ? syncOperationPayloadWithoutEvents(document.object()) : QJsonObject();
    }
    const qint64 uploadedCompleted = snapshot.value(QStringLiteral("uploaded_completed_bytes")).toVariant().toLongLong();
    const qint64 uploadedTotal = snapshot.value(QStringLiteral("uploaded_total_bytes")).toVariant().toLongLong();
    const qint64 downloadedCompleted = snapshot.value(QStringLiteral("downloaded_completed_bytes")).toVariant().toLongLong();
    const qint64 downloadedTotal = snapshot.value(QStringLiteral("downloaded_total_bytes")).toVariant().toLongLong();
    operation.insert(QStringLiteral("transfer_metrics_recorded"), true);
    operation.insert(QStringLiteral("uploaded_completed_bytes"), uploadedCompleted);
    operation.insert(QStringLiteral("uploaded_total_bytes"), uploadedTotal);
    operation.insert(QStringLiteral("downloaded_completed_bytes"), downloadedCompleted);
    operation.insert(QStringLiteral("downloaded_total_bytes"), downloadedTotal);
    operation.insert(QStringLiteral("bytes_transferred"), uploadedCompleted + downloadedCompleted);
    operation.insert(QStringLiteral("total_bytes"), uploadedTotal + downloadedTotal);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    operation.insert(QStringLiteral("updated_at_ms"), now);

    {
        QSqlQuery update(database);
        update.prepare(
            QStringLiteral("UPDATE sync_runs SET transfer_metrics_recorded = 1, uploaded_completed_bytes = ?, uploaded_total_bytes = ?, downloaded_completed_bytes = ?, downloaded_total_bytes = ?, updated_at_ms = MAX(updated_at_ms, ?), "
                           "payload_json = ? WHERE id = ?"));
        update.addBindValue(uploadedCompleted);
        update.addBindValue(uploadedTotal);
        update.addBindValue(downloadedCompleted);
        update.addBindValue(downloadedTotal);
        update.addBindValue(now);
        update.addBindValue(QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact)));
        update.addBindValue(operationId);
        if (!update.exec() || update.numRowsAffected() != 1) {
            return false;
        }
    }
    return scope.commit();
}

void appendStoredSyncOperationEvent(QSqlDatabase &database, const QString &id, const QJsonObject &incomingEvent)
{
    if (id.isEmpty()) {
        return;
    }
    if (incomingEvent.value(QStringLiteral("origin")).toString() == QLatin1String("server") || incomingEvent.value(QStringLiteral("sequence")).toVariant().toLongLong() > 0 || incomingEvent.contains(QStringLiteral("stream_state"))) {
        persistServerSyncOperationEvent(database, id, incomingEvent);
        return;
    }
    WriteScope scope(database);
    if (!scope.isOpen()) {
        return;
    }
    QJsonObject operation;
    {
        QSqlQuery select(database);
        select.prepare(QStringLiteral("SELECT payload_json, status, phase, transfer_metrics_recorded FROM sync_runs WHERE id = ? OR trace_id = ? LIMIT 1"));
        select.addBindValue(id);
        select.addBindValue(id);
        if (!select.exec() || !select.next()) {
            return;
        }
        const QJsonDocument payloadDocument = QJsonDocument::fromJson(select.value(0).toString().toUtf8());
        if (!payloadDocument.isObject()) {
            return;
        }
        operation = syncOperationPayloadWithoutEvents(payloadDocument.object());
        operation.insert(QStringLiteral("id"), operation.value(QStringLiteral("id")).toString(id));
        operation.insert(QStringLiteral("trace_id"), operation.value(QStringLiteral("trace_id")).toString(id));
        operation.insert(QStringLiteral("status"), select.value(1).toString());
        operation.insert(QStringLiteral("phase"), select.value(2).toString());
        operation.insert(QStringLiteral("transfer_metrics_recorded"), select.value(3).toBool());
    }
    if (!syncOperationIsRunning(operation)) {
        return;
    }
    const QString runId = operation.value(QStringLiteral("id")).toString(id);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString message = incomingEvent.value(QStringLiteral("message")).toString().trimmed().left(600);
    const QString phase = incomingEvent.value(QStringLiteral("phase")).toString().left(64);
    const QString state = incomingEvent.value(QStringLiteral("state")).toString(QStringLiteral("running")).left(32);
    const QString errorCode = incomingEvent.value(QStringLiteral("error_code")).toString().left(64);
    const qint64 atMs = incomingEvent.value(QStringLiteral("at_ms")).toVariant().toLongLong();
    QJsonObject eventObject {{QStringLiteral("at_ms"), atMs > 0 ? atMs : now},
                             {QStringLiteral("phase"), phase.isEmpty() ? legacySyncOperationPhase(operation.value(QStringLiteral("status")).toString(), message) : phase},
                             {QStringLiteral("state"), state},
                             {QStringLiteral("message"), message}};
    if (!errorCode.isEmpty()) {
        eventObject.insert(QStringLiteral("error_code"), errorCode);
    }
    mergeSyncEventIdentity(operation, eventObject, incomingEvent);
    if (incomingEvent.value(QStringLiteral("metadata")).isObject()) {
        eventObject.insert(QStringLiteral("metadata"), incomingEvent.value(QStringLiteral("metadata")));
        const QString outcome = incomingEvent.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("sync_outcome")).toString();
        if (outcome == QLatin1String("changes_synced") || outcome == QLatin1String("available_updates_synced"))
            operation.insert(QStringLiteral("sync_outcome"), outcome);
    }
    if (incomingEvent.contains(QStringLiteral("component"))) {
        eventObject.insert(QStringLiteral("component"), incomingEvent.value(QStringLiteral("component")).toString().left(64));
    }
    for (const QString &field : {QStringLiteral("path"), QStringLiteral("target")}) {
        const QString value = incomingEvent.value(field).toString();
        if (!value.isEmpty()) {
            eventObject.insert(field, value.left(512));
        }
    }
    for (const QString &field : {QStringLiteral("bytes_transferred"), QStringLiteral("total_bytes")}) {
        const qint64 value = incomingEvent.value(field).toVariant().toLongLong();
        if (incomingEvent.contains(field) && value >= 0) {
            eventObject.insert(field, value);
            if (!operation.value(QStringLiteral("transfer_metrics_recorded")).toBool()) {
                operation.insert(field, value);
            }
        }
    }
    for (const QString &field : {QStringLiteral("attempt"), QStringLiteral("completed"), QStringLiteral("total"), QStringLiteral("duration_ms")}) {
        if (incomingEvent.contains(field)) {
            eventObject.insert(field, incomingEvent.value(field));
        }
    }
    if (!phase.isEmpty()) {
        operation.insert(QStringLiteral("phase"), phase);
    }
    operation.insert(QStringLiteral("updated_at_ms"), now);
    QJsonObject previous;
    {
        QSqlQuery previousEvent(database);
        previousEvent.prepare(QStringLiteral("SELECT phase, state, message, error_code, bytes_completed, bytes_total FROM sync_events WHERE run_id = ? AND origin = 'desktop' ORDER BY at_ms DESC, id DESC LIMIT 1"));
        previousEvent.addBindValue(runId);
        if (previousEvent.exec() && previousEvent.next()) {
            previous.insert(QStringLiteral("phase"), previousEvent.value(0).toString());
            previous.insert(QStringLiteral("state"), previousEvent.value(1).toString());
            previous.insert(QStringLiteral("message"), previousEvent.value(2).toString());
            if (!previousEvent.value(3).toString().isEmpty()) {
                previous.insert(QStringLiteral("error_code"), previousEvent.value(3).toString());
            }
            if (!previousEvent.value(4).isNull()) {
                previous.insert(QStringLiteral("bytes_transferred"), QJsonValue::fromVariant(previousEvent.value(4)));
            }
            if (!previousEvent.value(5).isNull()) {
                previous.insert(QStringLiteral("total_bytes"), QJsonValue::fromVariant(previousEvent.value(5)));
            }
        }
    }
    const bool duplicate = previous.value(QStringLiteral("phase")) == eventObject.value(QStringLiteral("phase")) && previous.value(QStringLiteral("state")) == eventObject.value(QStringLiteral("state")) &&
        previous.value(QStringLiteral("message")) == eventObject.value(QStringLiteral("message")) && previous.value(QStringLiteral("error_code")) == eventObject.value(QStringLiteral("error_code")) &&
        previous.value(QStringLiteral("bytes_transferred")) == eventObject.value(QStringLiteral("bytes_transferred")) && previous.value(QStringLiteral("total_bytes")) == eventObject.value(QStringLiteral("total_bytes"));
    const bool appendEvent = !message.isEmpty() && !duplicate;
    if (state == QLatin1String("failed") || !errorCode.isEmpty()) {
        operation.insert(QStringLiteral("last_error_message"), message);
        operation.insert(QStringLiteral("last_error_code"), errorCode.isEmpty() ? QStringLiteral("sync_failed") : errorCode);
    }
    if (appendEvent && !insertLocalSyncOperationEvent(database, runId, eventObject, incomingEvent.value(QStringLiteral("attempt")).toInt(operation.value(QStringLiteral("attempt")).toInt()))) {
        return;
    }
    {
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE sync_runs SET phase = CASE WHEN ? = '' THEN phase ELSE ? END, updated_at_ms = MAX(updated_at_ms, ?), payload_json = ? WHERE id = ?"));
        update.addBindValue(phase);
        update.addBindValue(phase);
        update.addBindValue(now);
        update.addBindValue(QString::fromUtf8(QJsonDocument(syncOperationPayloadWithoutEvents(operation)).toJson(QJsonDocument::Compact)));
        update.addBindValue(runId);
        if (!update.exec() || update.numRowsAffected() != 1) {
            return;
        }
    }
    scope.commit();
}

} // namespace

bool normalizedSyncTransferSnapshot(const QJsonObject &incoming, QJsonObject *normalized)
{
    if (!normalized || incoming.value(QStringLiteral("schema_version")).toInt(1) != 1 || !incoming.value(QStringLiteral("transfer_metrics_recorded")).toBool()) {
        return false;
    }
    static const QStringList fields {QStringLiteral("uploaded_completed_bytes"), QStringLiteral("uploaded_total_bytes"), QStringLiteral("downloaded_completed_bytes"), QStringLiteral("downloaded_total_bytes")};
    QJsonObject result {{QStringLiteral("schema_version"), 1}, {QStringLiteral("transfer_metrics_recorded"), true}};
    for (const QString &field : fields) {
        if (!incoming.contains(field)) {
            return false;
        }
        const qint64 value = incoming.value(field).toVariant().toLongLong();
        if (value < 0) {
            return false;
        }
        result.insert(field, value);
    }
    if (result.value(QStringLiteral("uploaded_completed_bytes")).toVariant().toLongLong() > result.value(QStringLiteral("uploaded_total_bytes")).toVariant().toLongLong() ||
        result.value(QStringLiteral("downloaded_completed_bytes")).toVariant().toLongLong() > result.value(QStringLiteral("downloaded_total_bytes")).toVariant().toLongLong()) {
        return false;
    }
    result.insert(QStringLiteral("flush"), incoming.value(QStringLiteral("flush")).toBool());
    *normalized = result;
    return true;
}

// ---------------------------------------------------------------- worker

class SyncJournal::Worker : public QThread
{
public:
    Worker(SyncJournal *journal, const QString &path, const Policy &policy)
        : m_journal(journal)
        , m_path(path)
        , m_policy(policy)
    {
        static std::atomic<int> serial {0};
        const int number = serial.fetch_add(1);
        // The first journal of a process is the application's own.
        m_connectionName = number == 0 ? QStringLiteral("thoughts-sync-operations") : QStringLiteral("thoughts-sync-operations-%1").arg(number);
    }

    // False once the journal was shut down: the task is dropped.
    bool enqueue(TaskKind kind, Task task)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return false;
            }
            m_queue.push_back({kind, std::move(task)});
        }
        m_wake.notify_one();
        return true;
    }

    // Stores what is queued, closes the journal and ends the thread.
    void stop()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        wait();
    }

    const QString &path() const
    {
        return m_path;
    }

    std::atomic<bool> compactionPending {false};
    std::atomic<bool> compactionRunning {false};
    std::atomic<qint64> commits {0};
    std::atomic<qint64> writes {0};

protected:
    void run() override;

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        TaskKind kind;
        Task task;
    };

    void ensureOpen(QSqlDatabase &database);
    void beginBurst(QSqlDatabase &database);
    void commitBurst(QSqlDatabase &database);
    bool maintenanceStep(QSqlDatabase &database);
    bool pruneStep(QSqlDatabase &database);
    bool orphanStep(QSqlDatabase &database);
    bool reclaimStep(QSqlDatabase &database);
    void compact(QSqlDatabase &database);
    void updateCompactionPending(QSqlDatabase &database);

    SyncJournal *const m_journal;
    const QString m_path;
    const Policy m_policy;
    QString m_connectionName;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Entry> m_queue;
    bool m_stopping = false;

    // Worker thread only.
    bool m_opened = false;
    Clock::time_point m_nextOpenAttempt;
    bool m_burst = false;
    Clock::time_point m_burstStarted;
    Clock::time_point m_lastWrite;
    int m_burstWrites = 0;
    // The runs older than this are removed; found once per retention pass.
    std::optional<qint64> m_pruneBefore;
    bool m_orphansSwept = false;
};

void SyncJournal::Worker::ensureOpen(QSqlDatabase &database)
{
    if (m_opened || Clock::now() < m_nextOpenAttempt) {
        return;
    }
    m_opened = openJournal(database, m_path, m_connectionName, m_policy.deferredIndexEvents);
    if (!m_opened) {
        // Tasks answer as an empty journal would; look again in a while.
        if (database.isValid()) {
            database.close();
        }
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_nextOpenAttempt = Clock::now() + std::chrono::seconds(5);
    }
}

void SyncJournal::Worker::beginBurst(QSqlDatabase &database)
{
    if (m_burst || !usable(database)) {
        return;
    }
    // IMMEDIATE: a write that follows a read in the same transaction must not
    // find that another connection wrote in between.
    if (QSqlQuery(database).exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        m_burst = true;
        m_burstStarted = Clock::now();
        m_burstWrites = 0;
    }
}

void SyncJournal::Worker::commitBurst(QSqlDatabase &database)
{
    if (!m_burst) {
        return;
    }
    m_burst = false;
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("COMMIT"))) {
        commits.fetch_add(1);
    } else {
        qWarning() << "Could not commit the Thoughts synchronization journal:" << query.lastError().text();
        query.exec(QStringLiteral("ROLLBACK"));
    }
}

void SyncJournal::Worker::run()
{
    {
        QSqlDatabase database;
        m_nextOpenAttempt = Clock::now();
        Clock::time_point nextMaintenance = Clock::now() + std::chrono::milliseconds(m_policy.maintenanceStartDelayMs);
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            if (m_queue.empty()) {
                if (m_burst) {
                    const Clock::time_point deadline = std::min(m_lastWrite + std::chrono::milliseconds(m_policy.burstCoalesceMs), m_burstStarted + std::chrono::milliseconds(m_policy.burstMaximumMs));
                    if (!m_stopping && Clock::now() < deadline) {
                        m_wake.wait_until(lock, deadline);
                        continue;
                    }
                    lock.unlock();
                    commitBurst(database);
                    lock.lock();
                    continue;
                }
                if (m_stopping) {
                    break;
                }
                if (Clock::now() >= nextMaintenance) {
                    lock.unlock();
                    ensureOpen(database);
                    const bool more = maintenanceStep(database);
                    nextMaintenance = Clock::now() + std::chrono::milliseconds(more ? m_policy.maintenanceStepPauseMs : m_policy.maintenanceIntervalMs);
                    lock.lock();
                    continue;
                }
                m_wake.wait_until(lock, nextMaintenance);
                continue;
            }
            Entry entry = std::move(m_queue.front());
            m_queue.pop_front();
            lock.unlock();
            ensureOpen(database);
            switch (entry.kind) {
            case TaskKind::Read:
                entry.task(database);
                break;
            case TaskKind::Write:
            case TaskKind::DurableWrite:
                beginBurst(database);
                entry.task(database);
                writes.fetch_add(1);
                ++m_burstWrites;
                m_lastWrite = Clock::now();
                if (entry.kind == TaskKind::DurableWrite || m_burstWrites >= m_policy.burstMaximumWrites || m_lastWrite - m_burstStarted >= std::chrono::milliseconds(m_policy.burstMaximumMs)) {
                    commitBurst(database);
                }
                break;
            case TaskKind::Flush:
                commitBurst(database);
                entry.task(database);
                break;
            case TaskKind::Compact:
                commitBurst(database);
                compact(database);
                entry.task(database);
                break;
            case TaskKind::Maintenance:
                commitBurst(database);
                while (maintenanceStep(database)) { }
                entry.task(database);
                break;
            }
            // Destroy what the task captured before taking the lock again.
            entry.task = nullptr;
            lock.lock();
        }
        lock.unlock();
        commitBurst(database);
        if (database.isValid()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

// One small piece of background work; true while more is left. Only runs while
// nothing is queued and no burst is open, each piece in its own transaction.
bool SyncJournal::Worker::maintenanceStep(QSqlDatabase &database)
{
    if (!usable(database)) {
        return false;
    }
    if (pruneStep(database) || orphanStep(database) || reclaimStep(database)) {
        return true;
    }
    m_pruneBefore.reset();
    updateCompactionPending(database);
    return false;
}

bool SyncJournal::Worker::pruneStep(QSqlDatabase &database)
{
    if (!m_pruneBefore) {
        // A run goes when it is older than the retention period and is not
        // among the newest runs that are always kept.
        qint64 before = QDateTime::currentDateTimeUtc().addDays(-m_policy.retentionDays).toMSecsSinceEpoch();
        QSqlQuery newest(database);
        newest.prepare(QStringLiteral("SELECT updated_at_ms FROM sync_runs ORDER BY updated_at_ms DESC LIMIT 1 OFFSET ?"));
        newest.addBindValue(qMax(0, m_policy.retentionMinimumOperations - 1));
        if (!newest.exec()) {
            return false;
        }
        if (!newest.next()) {
            // Fewer runs than are always kept.
            m_pruneBefore = 0;
            return false;
        }
        m_pruneBefore = qMin(before, newest.value(0).toLongLong());
    }
    if (*m_pruneBefore <= 0) {
        return false;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return false;
    }
    // A run's events go first, a bounded number per step, and its row last: an
    // interrupted removal leaves a run with fewer events, never events without
    // a run.
    int budget = m_policy.pruneEventsPerStep;
    int removedRuns = 0;
    bool ok = true;
    bool more = false;
    while (ok && budget > 0 && removedRuns < 64) {
        QString runId;
        {
            QSqlQuery oldest(database);
            oldest.prepare(QStringLiteral("SELECT id FROM sync_runs WHERE updated_at_ms < ? ORDER BY updated_at_ms LIMIT 1"));
            oldest.addBindValue(*m_pruneBefore);
            ok = oldest.exec();
            if (!ok || !oldest.next()) {
                break;
            }
            runId = oldest.value(0).toString();
        }
        more = true;
        QSqlQuery events(database);
        events.prepare(QStringLiteral("DELETE FROM sync_events WHERE id IN (SELECT id FROM sync_events WHERE run_id = ? LIMIT ?)"));
        events.addBindValue(runId);
        events.addBindValue(budget);
        ok = events.exec();
        const int removedEvents = ok ? events.numRowsAffected() : 0;
        if (ok && removedEvents < budget) {
            QSqlQuery steps(database);
            steps.prepare(QStringLiteral("DELETE FROM sync_steps WHERE run_id = ?"));
            steps.addBindValue(runId);
            QSqlQuery run(database);
            run.prepare(QStringLiteral("DELETE FROM sync_runs WHERE id = ?"));
            run.addBindValue(runId);
            ok = steps.exec() && run.exec();
            ++removedRuns;
        }
        // A run without events still costs a row.
        budget -= qMax(1, removedEvents);
    }
    if (!ok || !query.exec(QStringLiteral("COMMIT"))) {
        query.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    commits.fetch_add(1);
    return more;
}

// Earlier builds removed runs first and their events afterwards; a crash in
// between left events without a run. Look through the events once, a range of
// rows at a time.
bool SyncJournal::Worker::orphanStep(QSqlDatabase &database)
{
    if (m_orphansSwept) {
        return false;
    }
    static const QString doneKey = QStringLiteral("orphan_events_swept_v1");
    static const QString cursorKey = QStringLiteral("orphan_events_cursor");
    if (!metadataValue(database, doneKey).isEmpty()) {
        m_orphansSwept = true;
        return false;
    }
    // Rows are numbered once and never reused: start where the events start.
    const qint64 cursor = qMax(metadataValue(database, cursorKey).toLongLong(), scalar(database, QStringLiteral("SELECT MIN(id) FROM sync_events")) - 1);
    const qint64 last = scalar(database, QStringLiteral("SELECT MAX(id) FROM sync_events"));
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return false;
    }
    bool ok = true;
    if (cursor >= last) {
        ok = query.exec(QStringLiteral("DELETE FROM sync_steps WHERE NOT EXISTS (SELECT 1 FROM sync_runs WHERE sync_runs.id = sync_steps.run_id)")) && setMetadataValue(database, doneKey, QStringLiteral("1"));
        m_orphansSwept = ok;
    } else {
        const qint64 upTo = cursor + m_policy.orphanRowsPerStep;
        QSqlQuery remove(database);
        remove.prepare(QStringLiteral("DELETE FROM sync_events WHERE id > ? AND id <= ? AND NOT EXISTS (SELECT 1 FROM sync_runs WHERE sync_runs.id = sync_events.run_id)"));
        remove.addBindValue(cursor);
        remove.addBindValue(upTo);
        ok = remove.exec() && setMetadataValue(database, cursorKey, QString::number(upTo));
    }
    if (!ok || !query.exec(QStringLiteral("COMMIT"))) {
        query.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    commits.fetch_add(1);
    return !m_orphansSwept;
}

// Gives the pages that retention freed back to the file system, a few at a time.
bool SyncJournal::Worker::reclaimStep(QSqlDatabase &database)
{
    if (pragmaInt(database, QStringLiteral("auto_vacuum")) != 2 || pragmaInt(database, QStringLiteral("freelist_count")) < m_policy.reclaimPagesPerStep) {
        return false;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA incremental_vacuum(%1)").arg(m_policy.reclaimPagesPerStep))) {
        return false;
    }
    while (query.next()) { }
    return true;
}

void SyncJournal::Worker::updateCompactionPending(QSqlDatabase &database)
{
    compactionPending.store(usable(database) && (pragmaInt(database, QStringLiteral("auto_vacuum")) != 2 || !hasIndex(database, eventErrorIndexName)));
}

void SyncJournal::Worker::compact(QSqlDatabase &database)
{
    compactionRunning.store(true);
    if (usable(database)) {
        if (pragmaInt(database, QStringLiteral("auto_vacuum")) != 2) {
            // The rewrite needs room for a second copy and for its log.
            const qint64 size = QFileInfo(m_path).size();
            const QStorageInfo storage(QFileInfo(m_path).absolutePath());
            if (!storage.isValid() || storage.bytesAvailable() > 3 * size) {
                QSqlQuery query(database);
                if (!query.exec(QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL")) || !query.exec(QStringLiteral("VACUUM"))) {
                    qWarning() << "Could not compact the Thoughts synchronization journal:" << query.lastError().text();
                }
                query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
            }
        }
        if (!hasIndex(database, eventErrorIndexName)) {
            createEventErrorIndex(database);
        }
        updateCompactionPending(database);
    }
    compactionRunning.store(false);
    QMetaObject::invokeMethod(m_journal, &SyncJournal::compactionFinished, Qt::QueuedConnection);
}

// ---------------------------------------------------------------- journal

QString SyncJournal::defaultPath()
{
    return QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/sync-operations.sqlite"));
}

namespace
{
QPointer<SyncJournal> s_journal;

// The journal object stays: what is destroyed after it (the Sync it recorded
// belongs to an object the application deletes last) may still report to it.
void shutDownJournal()
{
    if (s_journal) {
        s_journal->shutDown();
    }
}
}

SyncJournal *SyncJournal::instance()
{
    if (!s_journal) {
        // The worker must stop before Qt closes every database connection when
        // the application ends. Clean-up routines run last-registered first, so
        // let Qt's SQL module register its own before this one.
        QSqlDatabase::drivers();
        s_journal = new SyncJournal(defaultPath());
        qAddPostRoutine(shutDownJournal);
    }
    return s_journal;
}

SyncJournal::SyncJournal(const QString &path, QObject *parent, const Policy &policy)
    : QObject(parent)
    , d(new Worker(this, path, policy))
{
    labels();
    d->start();
}

SyncJournal::~SyncJournal()
{
    d->stop();
}

void SyncJournal::shutDown()
{
    d->stop();
}

QString SyncJournal::path() const
{
    return d->path();
}

bool SyncJournal::enqueue(TaskKind kind, Task task)
{
    return d->enqueue(kind, std::move(task));
}

void SyncJournal::runAndWait(TaskKind kind, Task task)
{
    Q_ASSERT(QThread::currentThread() != d.get());
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    const bool queued = d->enqueue(kind, [&](QSqlDatabase &database) {
        task(database);
        // Notified under the lock: the waiting thread owns both objects and
        // may destroy them as soon as it can take the lock.
        const std::lock_guard<std::mutex> lock(mutex);
        done = true;
        finished.notify_one();
    });
    if (!queued) {
        // Shut down: the caller gets what an empty journal answers.
        return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    finished.wait(lock, [&done] { return done; });
}

QString SyncJournal::beginOperation(const QString &kind, const QString &target, const QString &phase, const QString &initiationControl, const QString &retryOf)
{
    // Durable: after a crash the next start finds the run and marks it interrupted.
    return call<QString>(TaskKind::DurableWrite, [=](QSqlDatabase &database) { return beginSyncOperation(database, kind, target, phase, initiationControl, retryOf); });
}

void SyncJournal::updateOperation(const QString &id, SyncOperationState state, const QString &phase, const QString &event, const QString &buildId, const QString &errorCode,
                                  const QString &warningCode, const QString &warning)
{
    if (id.isEmpty()) {
        return;
    }
    // How a run ended is committed at once; progress waits for its burst.
    const bool terminal = state == SyncOperationState::Succeeded || state == SyncOperationState::Failed || state == SyncOperationState::Interrupted || state == SyncOperationState::Cancelled;
    enqueue(terminal ? TaskKind::DurableWrite : TaskKind::Write, [=](QSqlDatabase &database) { updateSyncOperation(database, id, state, phase, event, buildId, errorCode, warningCode, warning); });
}

void SyncJournal::appendEvent(const QString &id, const QJsonObject &event)
{
    if (id.isEmpty()) {
        return;
    }
    enqueue(TaskKind::Write, [=](QSqlDatabase &database) { appendStoredSyncOperationEvent(database, id, event); });
}

void SyncJournal::appendProgress(const QString &id, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &event)
{
    if (id.isEmpty() || phase.isEmpty()) {
        return;
    }
    appendEvent(id, QJsonObject {{QStringLiteral("at_ms"), QDateTime::currentMSecsSinceEpoch()},
                                 {QStringLiteral("phase"), phase.left(64)},
                                 {QStringLiteral("state"), QStringLiteral("running")},
                                 {QStringLiteral("message"), event.left(600)},
                                 {QStringLiteral("bytes_transferred"), qMax<qint64>(0, transferredBytes)},
                                 {QStringLiteral("total_bytes"), qMax<qint64>(0, totalBytes)}});
}

void SyncJournal::updateTransferProgress(const QString &traceId, const QJsonObject &absoluteSnapshot)
{
    enqueue(TaskKind::Write, [=](QSqlDatabase &database) {
        if (!updateStoredSyncOperationTransferProgress(database, traceId, absoluteSnapshot)) {
            qWarning() << "Could not persist directional transfer progress for" << traceId;
        }
    });
}

void SyncJournal::reconcileInterrupted(qint64 startedBeforeMs)
{
    enqueue(TaskKind::DurableWrite, [=](QSqlDatabase &database) { reconcileInterruptedSyncOperations(database, startedBeforeMs); });
}

void SyncJournal::flush()
{
    runAndWait(TaskKind::Flush, [](QSqlDatabase &) { });
}

QJsonObject SyncJournal::latestGlobalSummary()
{
    if (d->compactionRunning.load()) {
        return m_lastLatestGlobalSummary;
    }
    m_lastLatestGlobalSummary = call<QJsonObject>(TaskKind::Read, [](QSqlDatabase &database) { return readLatestGlobalSyncOperationSummary(database); });
    return m_lastLatestGlobalSummary;
}

QDateTime SyncJournal::lastSuccessfulGlobalSyncAt()
{
    if (d->compactionRunning.load()) {
        return m_lastSuccessfulGlobalSyncAt;
    }
    m_lastSuccessfulGlobalSyncAt = call<QDateTime>(TaskKind::Read, [](QSqlDatabase &database) { return readLastSuccessfulGlobalSyncAt(database); });
    return m_lastSuccessfulGlobalSyncAt;
}

QJsonObject SyncJournal::latestDocumentSummary(const QString &target)
{
    return call<QJsonObject>(TaskKind::Read, [=](QSqlDatabase &database) { return readLatestDocumentSyncOperationSummary(database, target); });
}

QJsonObject SyncJournal::operationSummary(const QString &operationId)
{
    if (operationId.isEmpty()) {
        return {};
    }
    return call<QJsonObject>(TaskKind::Read, [=](QSqlDatabase &database) { return readSyncOperationSummary(database, operationId); });
}

QList<QJsonObject> SyncJournal::runningSummaries()
{
    return call<QList<QJsonObject>>(TaskKind::Read, [](QSqlDatabase &database) { return runningSyncOperationSummaries(database); });
}

QString SyncJournal::runningBuildOperationIdForTarget(const QString &target)
{
    return call<QString>(TaskKind::Read, [=](QSqlDatabase &database) { return readRunningBuildOperationIdForTarget(database, target); });
}

QJsonArray SyncJournal::recentOperations(bool includeHistory)
{
    const QString journalPath = path();
    if (!includeHistory) {
        return call<QJsonArray>(TaskKind::Read, [=](QSqlDatabase &database) { return readSyncOperations(database, journalPath, {}, false); });
    }
    // Compatibility/export callers may restore an older database after the
    // schema migration: take the history out of its payloads first.
    return call<QJsonArray>(TaskKind::Write, [=](QSqlDatabase &database) {
        if (!normalizeEmbeddedSyncOperationEvents(database)) {
            qWarning() << "Could not normalize embedded synchronization history";
        }
        return readSyncOperations(database, journalPath, {}, true);
    });
}

QJsonObject SyncJournal::operationWithHistory(const QString &operationId)
{
    if (operationId.isEmpty()) {
        return {};
    }
    const QString journalPath = path();
    const QJsonArray operations = call<QJsonArray>(TaskKind::Read, [=](QSqlDatabase &database) { return readSyncOperations(database, journalPath, operationId, true); });
    return operations.isEmpty() ? QJsonObject() : operations.first().toObject();
}

bool SyncJournal::fetchPage(QObject *context, const PageRequest &request, std::function<void(const Page &)> done, int waitMs)
{
    return fetch<Page>(context, [request](QSqlDatabase &database) { return readOperationPage(database, request); }, std::move(done), waitMs);
}

bool SyncJournal::fetchHistory(QObject *context, const QString &operationId, qint64 afterEventRow, std::function<void(const History &)> done, int waitMs)
{
    return fetch<History>(context, [operationId, afterEventRow](QSqlDatabase &database) { return readOperationHistory(database, operationId, afterEventRow); }, std::move(done), waitMs);
}

QJsonObject SyncJournal::operationFromHistory(const QJsonObject &operation, QList<Event> events)
{
    // As one statement reads them: by time, then by row.
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.orderMs != b.orderMs ? a.orderMs < b.orderMs : a.row < b.row; });
    QJsonArray array;
    for (const Event &event : std::as_const(events)) {
        array.append(event.json);
    }
    QJsonObject result = operation;
    result.insert(QStringLiteral("events"), orderedEvents(array));
    return normalizedStoredSyncOperation(result);
}

bool SyncJournal::compactionPending() const
{
    return d->compactionPending.load();
}

bool SyncJournal::compactionRunning() const
{
    return d->compactionRunning.load();
}

void SyncJournal::compact()
{
    // Set here already: what the status area asks next must not wait behind it.
    d->compactionRunning.store(true);
    if (!enqueue(TaskKind::Compact, [](QSqlDatabase &) { })) {
        d->compactionRunning.store(false);
    }
}

SyncJournal::Statistics SyncJournal::statistics()
{
    const QString journalPath = path();
    Statistics result = call<Statistics>(TaskKind::Flush, [journalPath](QSqlDatabase &database) {
        Statistics statistics;
        if (!usable(database)) {
            return statistics;
        }
        statistics.runs = scalar(database, QStringLiteral("SELECT COUNT(*) FROM sync_runs"));
        statistics.events = scalar(database, QStringLiteral("SELECT COUNT(*) FROM sync_events"));
        statistics.freePages = pragmaInt(database, QStringLiteral("freelist_count"));
        statistics.pageSize = pragmaInt(database, QStringLiteral("page_size"));
        statistics.autoVacuum = pragmaInt(database, QStringLiteral("auto_vacuum"));
        statistics.errorIndex = hasIndex(database, eventErrorIndexName);
        QSqlQuery(database).exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
        statistics.fileBytes = QFileInfo(journalPath).size();
        return statistics;
    });
    result.commits = d->commits.load();
    result.writes = d->writes.load();
    return result;
}

void SyncJournal::runMaintenanceNow()
{
    runAndWait(TaskKind::Maintenance, [](QSqlDatabase &) { });
}

QStringList SyncJournal::queryPlan(const QString &statement)
{
    return call<QStringList>(TaskKind::Read, [=](QSqlDatabase &database) {
        QStringList plan;
        QSqlQuery query(database);
        if (query.exec(QStringLiteral("EXPLAIN QUERY PLAN ") + statement)) {
            while (query.next()) {
                plan.append(query.value(3).toString());
            }
        }
        return plan;
    });
}

}
