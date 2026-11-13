/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

namespace Thoughts {
// repository.json commits a revision of these durable records. If its atomic
// write fails, the previous revision remains readable. Only changed rows are
// written; immutable proposal history is never serialized on metadata updates.
class RepositoryProjectionStore {
    QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db;
    bool ready = false;
public:
    explicit RepositoryProjectionStore(const QString &root) {
        if (!QDir().mkpath(root)) return;
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(QDir(root).filePath(QStringLiteral("repository-projections.sqlite")));
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) return;
        QSqlQuery q(db);
        ready = q.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
            && q.exec(QStringLiteral("PRAGMA synchronous=FULL"))
            && q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS revisions(id INTEGER PRIMARY KEY AUTOINCREMENT, records INTEGER NOT NULL)"))
            && q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS records(key TEXT NOT NULL, revision INTEGER NOT NULL, payload BLOB, sha256 BLOB, PRIMARY KEY(key,revision))"))
            && q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS record_revisions ON records(revision)"));
    }
    ~RepositoryProjectionStore() { db.close(); db = {}; QSqlDatabase::removeDatabase(connection); }
    RepositoryProjectionStore(const RepositoryProjectionStore &) = delete;
    RepositoryProjectionStore &operator=(const RepositoryProjectionStore &) = delete;

    bool read(qint64 revision, QJsonObject *result) {
        if (!ready || revision <= 0 || !db.transaction()) return false;
        const auto fail = [&] { db.rollback(); return false; };
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT records FROM revisions WHERE id=?")); q.addBindValue(revision);
        if (!q.exec() || !q.next()) return fail();
        const qint64 expected = q.value(0).toLongLong();
        q.prepare(QStringLiteral("SELECT r.key,r.payload,r.sha256 FROM records r JOIN (SELECT key,MAX(revision) revision FROM records WHERE revision<=? GROUP BY key) current ON r.key=current.key AND r.revision=current.revision WHERE r.payload IS NOT NULL"));
        q.addBindValue(revision);
        if (!q.exec()) return fail();
        QJsonObject values;
        while (q.next()) {
            const QByteArray raw = q.value(1).toByteArray();
            const auto doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject() || QCryptographicHash::hash(raw, QCryptographicHash::Sha256) != q.value(2).toByteArray()) return fail();
            values.insert(q.value(0).toString(), doc.object());
        }
        if (values.size() != expected || !db.commit()) return fail();
        *result = values;
        return true;
    }

    // Delete every stored revision of keys that no longer exist (retired record types)
    // and reclaim the space. Only call after a revision without these keys committed.
    bool purgeKeys(const QString &prefix) {
        if (!ready) return false;
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM records WHERE substr(key,1,?)=?"));
        q.addBindValue(prefix.size()); q.addBindValue(prefix);
        return q.exec() && q.exec(QStringLiteral("VACUUM"));
    }

    bool write(qint64 base, const QJsonObject &previous, const QJsonObject &next, qint64 *revision) {
        if (!ready) return false;
        if (base > 0 && previous == next) { *revision = base; return true; }
        if (!db.transaction()) return false;
        const auto fail = [&] { db.rollback(); return false; };
        QSqlQuery q(db);
        if (base > 0) {
            q.prepare(QStringLiteral("SELECT records FROM revisions WHERE id=?")); q.addBindValue(base);
            if (!q.exec() || !q.next() || q.value(0).toLongLong() != previous.size()) return fail();
        }
        // Future rows can only belong to an interrupted, uncommitted JSON write.
        for (const QString &table : {QStringLiteral("records"), QStringLiteral("revisions")}) {
            q.prepare(QStringLiteral("DELETE FROM ") + table + (table == QLatin1String("records") ? QStringLiteral(" WHERE revision>?") : QStringLiteral(" WHERE id>?")));
            q.addBindValue(base);
            if (!q.exec()) return fail();
        }
        q.prepare(QStringLiteral("INSERT INTO revisions(records) VALUES(?)")); q.addBindValue(next.size());
        if (!q.exec()) return fail();
        const qint64 id = q.lastInsertId().toLongLong();
        q.prepare(QStringLiteral("INSERT INTO records(key,revision,payload,sha256) VALUES(?,?,?,?)"));
        const auto put = [&](const QString &key, const QJsonValue &value) {
            const QByteArray raw = value.isUndefined() ? QByteArray() : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
            q.bindValue(0, key); q.bindValue(1, id);
            q.bindValue(2, raw); q.bindValue(3, raw.isNull() ? QByteArray() : QCryptographicHash::hash(raw, QCryptographicHash::Sha256));
            return q.exec();
        };
        for (auto it = next.constBegin(); it != next.constEnd(); ++it)
            if ((base == 0 || previous.value(it.key()) != it.value()) && !put(it.key(), it.value())) return fail();
        if (base > 0)
            for (auto it = previous.constBegin(); it != previous.constEnd(); ++it)
                if (!next.contains(it.key()) && !put(it.key(), QJsonValue(QJsonValue::Undefined))) return fail();
        if (!db.commit()) return fail();
        *revision = id;
        return true;
    }
};
}
