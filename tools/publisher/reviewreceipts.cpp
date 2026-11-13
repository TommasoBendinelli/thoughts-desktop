// SPDX-License-Identifier: GPL-2.0-or-later
// thoughts_server.review_source_receipts: advance authoring bases from
// acknowledged, byte-verified local card edits that Fedora already holds.

#include "sourcepublisher.h"

#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sha256.h"
#include "sourceinventory.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

struct OperationRow {
    Json operationId;
    QString encoded;
};

QList<OperationRow> acknowledgedSourceOperations(const Py::Path &database)
{
    const QString connection = QStringLiteral("review-receipts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<OperationRow> rows;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(database.str());
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            const QString detail = db.lastError().text();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
            Py::raise(QStringLiteral("sqlite3.OperationalError"), detail);
        }
        QSqlQuery query(db);
        const bool ok = query.exec(QStringLiteral(
            "SELECT operation_id, operation_json FROM desktop_logical_operations "
            "WHERE acknowledged = 1 "
            "AND json_extract(operation_json, '$.payload.source_authority') = 'client-v1' "
            "AND json_extract(operation_json, '$.payload.source_change') IS NOT NULL "
            "ORDER BY device_sequence DESC"));
        if (!ok) {
            const QString detail = query.lastError().databaseText();
            query = QSqlQuery();
            db.close();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
            Py::raise(QStringLiteral("sqlite3.OperationalError"), detail);
        }
        while (query.next()) {
            const QVariant id = query.value(0);
            Json operationId;
            if (id.isNull()) {
                operationId = Json();
            } else if (id.typeId() == QMetaType::LongLong || id.typeId() == QMetaType::Int) {
                operationId = Json(id.toLongLong());
            } else if (id.typeId() == QMetaType::Double) {
                operationId = Json(id.toDouble());
            } else {
                operationId = Json(id.toString());
            }
            rows.append({operationId, query.value(1).toString()});
        }
        query = QSqlQuery();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return rows;
}

bool verifiedObject(const Py::Path &objects, const Py::Path &root, const QString &digest)
{
    const Py::Path object = objects / digest;
    if (!Py::isFile(object) || !Py::resolve(object).isRelativeTo(root)) {
        return false;
    }
    const Py::FileStatus status = Py::status(object);
    if (status.kind == Py::FileStatus::Kind::Missing) {
        Py::raiseOsError(status.error, object.str());
    }
    return status.size <= maximumManagedSourceBytes && sha256Hex(Py::readBytes(object)) == digest;
}
}

void adoptReviewSourceBases(SourcePublisher &publisher, const SourceHeads &heads, const QSet<QString> &paths)
{
    const Py::Path database = publisher.statePath.parent() / QStringLiteral("desktop-sync.sqlite");
    if (!Py::isFile(database) || Py::truthy(publisher.state.value(QStringLiteral("pending_publication")))) {
        return;
    }
    if (!publisher.state.contains(QStringLiteral("bases"))) {
        publisher.state.insert(QStringLiteral("bases"), Json::object());
    }
    Json bases = publisher.state.value(QStringLiteral("bases"));
    QSet<QString> candidates;
    for (const QString &path : paths) {
        const std::optional<Json> head = heads.get(path);
        if (bases.contains(path) && head && !Py::truthy(Py::get(*head, QStringLiteral("deleted"), Json(false)))
            && Py::subscript(*head, QStringLiteral("sha256")) != Py::get(bases.value(path), QStringLiteral("sha256"))) {
            candidates.insert(path);
        }
    }
    if (candidates.isEmpty()) {
        return;
    }
    const QList<OperationRow> rows = acknowledgedSourceOperations(database);
    const bool conflictsPresent = publisher.state.contains(QStringLiteral("conflicts"));
    Json conflicts = conflictsPresent ? publisher.state.value(QStringLiteral("conflicts")) : Json::object();
    std::optional<QHash<QString, Json>> remoteConflicts;
    const auto isDigest = [](const Json &value) {
        return value.isString() && Py::isLowerHex(value.toString(), 64);
    };
    for (const OperationRow &row : rows) {
        const Json operation = Py::loadsText(row.encoded);
        const Json change = Py::get(Py::get(operation, QStringLiteral("payload"), Json::object()), QStringLiteral("source_change"), Json::object());
        const Json pathValue = Py::get(change, QStringLiteral("path"));
        Py::hashKey(pathValue);
        if (!pathValue.isString() || !candidates.contains(pathValue.toString())) {
            continue;
        }
        const QString path = pathValue.toString();
        const Json base = bases.value(path);
        const Json remote = *heads.get(path);
        const Json before = Py::get(change, QStringLiteral("before_sha256"));
        const Json after = Py::get(change, QStringLiteral("after_sha256"));
        const Json entityType = Py::get(operation, QStringLiteral("entity_type"));
        Py::hashKey(entityType);
        if ((entityType != Json(QStringLiteral("review_card")) && entityType != Json(QStringLiteral("review_event"))) || !isDigest(before)
            || !isDigest(after) || Py::get(change, QStringLiteral("base_sha256"), before) != Py::get(base, QStringLiteral("sha256"))
            || Py::get(remote, QStringLiteral("sha256")) != after || Py::get(remote, QStringLiteral("object_sha256")) != after) {
            continue;
        }
        const Json remoteRevision = Py::get(remote, QStringLiteral("revision"), Json(0));
        const Json baseRevision = Py::get(base, QStringLiteral("revision"), Json(0));
        if (Py::toPythonInt(remoteRevision) <= Py::toPythonInt(baseRevision)) {
            continue;
        }
        const Py::Path objects = publisher.root / QStringLiteral("reviews") / QStringLiteral("source-objects");
        if (!verifiedObject(objects, publisher.root, before.toString()) || !verifiedObject(objects, publisher.root, after.toString())) {
            continue;
        }
        const Json conflict = conflicts.value(path);
        if (Py::truthy(conflict)) {
            const Py::Path target = Py::resolve(publisher.root / path);
            if (!target.isRelativeTo(publisher.root) || !Py::isFile(target)) {
                continue;
            }
            const Py::FileStatus status = Py::status(target);
            if (status.size > maximumManagedSourceBytes || Json(sha256Hex(Py::readBytes(target))) != Py::get(conflict, QStringLiteral("local_sha256"))
                || Py::get(conflict, QStringLiteral("server_revision")) != Py::subscript(remote, QStringLiteral("revision"))) {
                continue;
            }
            if (!remoteConflicts) {
                remoteConflicts = QHash<QString, Json>();
                for (const QString &status : {QStringLiteral("unresolved"), QStringLiteral("discarded")}) {
                    for (const Json &item : publisher.api.sourceConflicts(status)) {
                        remoteConflicts->insert(Py::hashKey(Py::subscript(item, QStringLiteral("id")), "dict key"), item);
                    }
                }
            }
            const Json recorded = remoteConflicts->value(Py::hashKey(Py::get(conflict, QStringLiteral("conflict_id")), "dict key"), Json::object());
            if (Py::get(recorded, QStringLiteral("path")) != pathValue || Py::get(recorded, QStringLiteral("base_sha256")) != Py::subscript(base, QStringLiteral("sha256"))
                || Py::get(recorded, QStringLiteral("current_sha256")) != after
                || Py::get(recorded, QStringLiteral("incoming_sha256")) != Py::subscript(conflict, QStringLiteral("local_sha256"))) {
                continue;
            }
            Json result;
            try {
                result = publisher.api.discardSourceConflict(Py::pythonStr(Py::subscript(conflict, QStringLiteral("conflict_id"))),
                                                             Py::subscript(remote, QStringLiteral("revision")), after.toString());
            } catch (const Py::Exception &error) {
                if (error.type == Py::apiErrorType && error.status == 409) {
                    continue;
                }
                throw;
            }
            if (Py::get(result, QStringLiteral("id")) != Py::subscript(conflict, QStringLiteral("conflict_id"))
                || Py::get(result, QStringLiteral("status")) != Json(QStringLiteral("discarded")) || Py::get(result, QStringLiteral("path")) != pathValue
                || Py::get(result, QStringLiteral("current_sha256")) != after) {
                Py::raiseValueError(QStringLiteral("invalid acknowledged review conflict retirement: ") + path);
            }
        }
        Json receipt = Json::object({{QStringLiteral("operation_id"), row.operationId},
                                     {QStringLiteral("previous_base_sha256"), Py::subscript(base, QStringLiteral("sha256"))},
                                     {QStringLiteral("sha256"), after},
                                     {QStringLiteral("revision"), Py::subscript(remote, QStringLiteral("revision"))}});
        if (Py::truthy(conflict)) {
            receipt.insert(QStringLiteral("retired_conflict"), conflict);
            conflicts.remove(path);
            if (conflictsPresent) {
                publisher.state.insert(QStringLiteral("conflicts"), conflicts);
            }
        }
        Json receipts = publisher.state.contains(QStringLiteral("review_source_base_receipts"))
            ? publisher.state.value(QStringLiteral("review_source_base_receipts"))
            : Json::object();
        receipts.insert(path, receipt);
        publisher.state.insert(QStringLiteral("review_source_base_receipts"), receipts);
        bases.insert(path, Json::object({{QStringLiteral("revision"), Py::subscript(remote, QStringLiteral("revision"))},
                                         {QStringLiteral("sha256"), Py::subscript(remote, QStringLiteral("sha256"))},
                                         {QStringLiteral("object_sha256"), Py::subscript(remote, QStringLiteral("object_sha256"))}}));
        publisher.state.insert(QStringLiteral("bases"), bases);
        publisher.writeState();
        publisher.report(QStringLiteral("Recorded the acknowledged local card source as the shared version: ") + path);
        candidates.remove(path);
    }
}

}
