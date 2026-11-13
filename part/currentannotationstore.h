/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace Thoughts {
// Current records are independent of archival history and PDF publications.
// Each operation owns its connection; callers may run on different workers.
class CurrentAnnotationStore {
    QString m_connection;
    QSqlDatabase m_database;
    QString m_error;

    bool execute(const QString &sql) {
        QSqlQuery query(m_database);
        if (query.exec(sql)) return true;
        m_error = query.lastError().text();
        return false;
    }
public:
    explicit CurrentAnnotationStore(const QString &root)
        : m_connection(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        const QString directory = QDir(root).filePath(QStringLiteral(".runtime"));
        if (!QDir().mkpath(directory)) { m_error = QStringLiteral("Cannot create current annotation store."); return; }
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
        m_database.setDatabaseName(QDir(directory).filePath(QStringLiteral("current-annotations.sqlite")));
        m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!m_database.open()) { m_error = m_database.lastError().text(); return; }
        if (!execute(QStringLiteral("PRAGMA journal_mode=WAL"))
            || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS annotations (type TEXT NOT NULL, id TEXT NOT NULL, source_file TEXT NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(type,id))"))
            || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_source ON annotations(type,source_file)"))) return;
        QHash<QString, QJsonObject> migrations;
        if (!read(QStringLiteral("migration"), &migrations)) return;
        if (!migrations.contains(QStringLiteral("proposal-resolved-pruned-v1"))) {
            // Proposal history was retired: resolved proposals are no longer kept.
            QHash<QString, QJsonObject> proposals;
            if (!read(QStringLiteral("proposal"), &proposals) || !m_database.transaction()) return;
            for (auto it = proposals.cbegin(); it != proposals.cend(); ++it)
                if (resolvedProposalKind(it.value().value(QStringLiteral("kind")).toString()) && !remove(QStringLiteral("proposal"), it.key())) {
                    m_database.rollback(); return;
                }
            if (!put(QStringLiteral("migration"), QStringLiteral("proposal-resolved-pruned-v1"), QStringLiteral(""), QJsonObject{{QStringLiteral("complete"), true}})
                || !m_database.commit()) { m_database.rollback(); return; }
        }
        if (migrations.contains(QStringLiteral("proposal-summary-v1"))) return;
        // One-time import of the old current summary, not a replay of archival history.
        QFile legacy(QDir(root).filePath(QStringLiteral("comments/.history/proposal-current-index.json")));
        if (!legacy.exists()) return;
        if (!legacy.open(QIODevice::ReadOnly)) { m_error = legacy.errorString(); return; }
        auto summary = QJsonDocument::fromJson(legacy.readAll()).object();
        const QString checksum = summary.take(QStringLiteral("index_sha256")).toString();
        if (checksum != QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(summary).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) return;
        QHash<QString, QJsonObject> current;
        if (!read(QStringLiteral("proposal"), &current) || !m_database.transaction()) return;
        const auto latest = summary.value(QStringLiteral("latest")).toObject();
        const auto dates = summary.value(QStringLiteral("dates")).toObject();
        for (auto it = latest.begin(); it != latest.end(); ++it) {
            if (current.contains(it.key())) continue;
            auto record = it.value().toObject();
            if (resolvedProposalKind(record.value(QStringLiteral("kind")).toString())) continue;
            const auto stamp = dates.value(it.key()).toObject();
            record.insert(QStringLiteral("first_seen_at"), stamp.value(QStringLiteral("first_seen")));
            record.insert(QStringLiteral("modified_at"), stamp.value(QStringLiteral("updated")));
            if (!put(QStringLiteral("proposal"), it.key(), record.value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("source_file")).toString(), record)) {
                m_database.rollback(); return;
            }
        }
        if (!put(QStringLiteral("migration"), QStringLiteral("proposal-summary-v1"), QStringLiteral(""), QJsonObject{{QStringLiteral("complete"), true}})
            || !m_database.commit()) m_database.rollback();
    }
    ~CurrentAnnotationStore() {
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase(m_connection);
    }
    CurrentAnnotationStore(const CurrentAnnotationStore &) = delete;
    CurrentAnnotationStore &operator=(const CurrentAnnotationStore &) = delete;
    QString error() const { return m_error; }
    bool read(const QString &type, QHash<QString, QJsonObject> *records) {
        if (!m_error.isEmpty()) return false;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("SELECT id,payload FROM annotations WHERE type=?"));
        query.addBindValue(type);
        if (!query.exec()) { m_error = query.lastError().text(); return false; }
        QHash<QString, QJsonObject> result;
        while (query.next()) {
            const auto value = QJsonDocument::fromJson(query.value(1).toByteArray());
            if (!value.isObject()) { m_error = QStringLiteral("Invalid current annotation record."); return false; }
            result.insert(query.value(0).toString(), value.object());
        }
        *records = result;
        return true;
    }
    static bool resolvedProposalKind(const QString &kind) {
        return kind == QLatin1String("accepted") || kind == QLatin1String("rejected") || kind == QLatin1String("deleted_from_source");
    }
    bool remove(const QString &type, const QString &id) {
        if (!m_error.isEmpty()) return false;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM annotations WHERE type=? AND id=?"));
        query.addBindValue(type); query.addBindValue(id);
        if (query.exec()) return true;
        m_error = query.lastError().text();
        return false;
    }
    bool put(const QString &type, const QString &id, const QString &source, const QJsonObject &payload) {
        if (!m_error.isEmpty()) return false;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO annotations(type,id,source_file,payload) VALUES(?,?,?,?) ON CONFLICT(type,id) DO UPDATE SET source_file=excluded.source_file,payload=excluded.payload"));
        query.addBindValue(type); query.addBindValue(id); query.addBindValue(source.isNull() ? QStringLiteral("") : source);
        query.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        if (query.exec()) return true;
        m_error = query.lastError().text();
        return false;
    }
    bool replace(const QString &type, const QJsonObject &records) {
        if (!m_error.isEmpty() || !m_database.transaction()) return false;
        QHash<QString, QJsonObject> previous;
        if (!read(type, &previous)) { m_database.rollback(); return false; }
        QSqlQuery remove(m_database);
        remove.prepare(QStringLiteral("DELETE FROM annotations WHERE type=? AND id=?"));
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (records.contains(it.key())) continue;
            remove.bindValue(0, type); remove.bindValue(1, it.key());
            if (!remove.exec()) { m_error = remove.lastError().text(); m_database.rollback(); return false; }
        }
        for (auto it = records.begin(); it != records.end(); ++it)
            if (previous.value(it.key()) != it.value().toObject()
                && !put(type, it.key(), it.value().toObject().value(QStringLiteral("source_file")).toString(), it.value().toObject())) {
                m_database.rollback(); return false;
            }
        if (m_database.commit()) return true;
        m_error = m_database.lastError().text();
        return false;
    }
    bool applyProposalEvents(const QJsonArray &events) {
        if (!m_error.isEmpty() || !m_database.transaction()) return false;
        QHash<QString, QJsonObject> records;
        if (!read(QStringLiteral("proposal"), &records)) { m_database.rollback(); return false; }
        for (const auto &value : events) {
            auto event = value.toObject();
            // Unsaved editor history is archival evidence, never current source state.
            if (event.value(QStringLiteral("source_state")).toString() == QLatin1String("editor_buffer")) continue;
            const QString id = event.value(QStringLiteral("proposal_id")).toString();
            const auto previous = records.value(id);
            const QString time = event.value(QStringLiteral("observed_at")).toString();
            if (id.isEmpty() || time.isEmpty()) { m_error = QStringLiteral("Invalid proposal current-state update."); m_database.rollback(); return false; }
            // An old Sync observation must not overwrite newer local source state.
            if (previous.value(QStringLiteral("observed_at")).toString() > time) continue;
            const QString firstSeen = previous.value(QStringLiteral("first_seen_at")).toString(time);
            event.insert(QStringLiteral("first_seen_at"), firstSeen);
            QString modified = previous.value(QStringLiteral("modified_at")).toString();
            if (event.value(QStringLiteral("kind")).toString() == QLatin1String("changed")
                && event.value(QStringLiteral("snapshot")) != previous.value(QStringLiteral("snapshot"))) modified = time;
            event.insert(QStringLiteral("modified_at"), modified);
            // A resolved proposal no longer matters: keep only open proposals.
            if (resolvedProposalKind(event.value(QStringLiteral("kind")).toString())) {
                if (!remove(QStringLiteral("proposal"), id)) { m_database.rollback(); return false; }
                records.remove(id);
                continue;
            }
            // Persist only the latest state, never authoring receipts or the event archive.
            event.remove(QStringLiteral("agent_result"));
            const QString source = event.value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("source_file")).toString();
            if (!put(QStringLiteral("proposal"), id, source, event)) { m_database.rollback(); return false; }
            records.insert(id, event);
        }
        if (m_database.commit()) return true;
        m_error = m_database.lastError().text();
        return false;
    }
};
}
