// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourceconflictrecord.h"

#include "managedsource.h"
#include "pyerror.h"
#include "pyfs.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sourceapi.h"
#include "statelock.h"

namespace Thoughts::Publisher
{

using Py::Json;

namespace
{

bool isRevision(const Json &value)
{
    return value.type() == Json::Type::Int || value.isBool();
}

bool isDigest(const Json &value)
{
    return value.isString() && Py::isLowerHex(value.toString(), 64);
}

}

Json successorConflictRecord(const Json &recorded, const QString &successorId, const Json &serverRevision, const QString &incomingSha256)
{
    Json superseded = Py::get(recorded, QStringLiteral("superseded"), Json::array());
    if (!superseded.isArray()) {
        superseded = Json::array();
    }
    superseded.append(Json::object({{QStringLiteral("conflict_id"), Py::get(recorded, QStringLiteral("conflict_id"))},
                                    {QStringLiteral("local_sha256"), Py::get(recorded, QStringLiteral("local_sha256"))},
                                    {QStringLiteral("server_revision"), Py::get(recorded, QStringLiteral("server_revision"))}}));
    return Json::object({{QStringLiteral("conflict_id"), Json(successorId)},
                         {QStringLiteral("local_sha256"), Json(incomingSha256)},
                         {QStringLiteral("server_revision"), serverRevision},
                         {QStringLiteral("superseded"), superseded}});
}

QSet<QString> acceptedLocalDigests(const Json &recorded)
{
    QSet<QString> digests;
    const Json local = Py::get(recorded, QStringLiteral("local_sha256"));
    if (local.isString()) {
        digests.insert(local.toString());
    }
    const Json superseded = Py::get(recorded, QStringLiteral("superseded"), Json::array());
    if (superseded.isArray()) {
        for (const Json &earlier : superseded.items()) {
            const Json digest = Py::get(earlier, QStringLiteral("local_sha256"));
            if (digest.isString()) {
                digests.insert(digest.toString());
            }
        }
    }
    return digests;
}

SourceConflictListings::SourceConflictListings(SourceApi &api)
    : m_api(api)
{
}

const QList<Json> &SourceConflictListings::listing(const QString &status)
{
    auto found = m_listings.find(status);
    if (found == m_listings.end()) {
        found = m_listings.insert(status, m_api.sourceConflicts(status));
    }
    return found.value();
}

std::optional<Json> followSupersededConflict(SourceConflictListings &listings, const QString &path, const Json &recorded)
{
    const QString unidentified = QStringLiteral("source conflict was superseded on the server by a conflict this computer cannot identify; resolve it on the server: ") + path;
    Json record = recorded;
    bool followed = false;
    QSet<QString> seen;
    for (;;) {
        const Json conflictId = Py::get(record, QStringLiteral("conflict_id"));
        if (!conflictId.isString() || seen.contains(conflictId.toString())) {
            Py::raiseValueError(unidentified);
        }
        seen.insert(conflictId.toString());
        std::optional<Json> superseded;
        for (const Json &item : listings.listing(QStringLiteral("superseded"))) {
            if (Py::get(item, QStringLiteral("id")) == conflictId && Py::get(item, QStringLiteral("path")) == Json(path)) {
                superseded = item;
                break;
            }
        }
        if (!superseded) {
            break;
        }
        // The successor's common base is the superseded conflict's Fedora
        // side, and its resolution expects a later revision.
        const Json fedoraSide = Py::get(*superseded, QStringLiteral("current_sha256"));
        const qint64 revision = Py::toPythonInt(Py::get(record, QStringLiteral("server_revision"), Json(-1)));
        QList<Json> candidates;
        for (const QString &status : {QStringLiteral("unresolved"), QStringLiteral("resolved"), QStringLiteral("superseded")}) {
            for (const Json &item : listings.listing(status)) {
                const Json expected = Py::get(item, QStringLiteral("expected_revision"));
                if (Py::get(item, QStringLiteral("path")) == Json(path) && Py::get(item, QStringLiteral("id")) != conflictId && isDigest(fedoraSide)
                    && Py::get(item, QStringLiteral("base_sha256")) == fedoraSide && isRevision(expected) && Py::toPythonInt(expected) > revision) {
                    candidates.append(item);
                }
            }
        }
        if (candidates.size() > 1) {
            // Fedora lists a superseded conflict with the revision of the
            // resolution that superseded it, which is its successor's.
            const Json supersededAt = Py::get(*superseded, QStringLiteral("expected_revision"));
            QList<Json> atSupersession;
            for (const Json &candidate : std::as_const(candidates)) {
                if (Py::get(candidate, QStringLiteral("expected_revision")) == supersededAt) {
                    atSupersession.append(candidate);
                }
            }
            candidates = atSupersession;
        }
        if (candidates.size() != 1) {
            Py::raiseValueError(unidentified);
        }
        const Json &successor = candidates.first();
        const Json successorId = Py::get(successor, QStringLiteral("id"));
        const Json incoming = Py::get(successor, QStringLiteral("incoming_sha256"));
        if (!successorId.isString() || successorId.toString().isEmpty() || !isDigest(incoming)) {
            Py::raiseValueError(unidentified);
        }
        record = successorConflictRecord(record, successorId.toString(), Py::get(successor, QStringLiteral("expected_revision")), incoming.toString());
        followed = true;
    }
    return followed ? std::optional<Json>(record) : std::nullopt;
}

bool recordSuccessorSourceConflict(const QString &sourceStatePath, const QString &path, const QString &supersededId, const QString &successorId,
                                   qint64 serverRevision, const QString &incomingSha256, QString *error, double lockTimeoutSeconds)
{
    try {
        const Py::Path statePath(sourceStatePath);
        SourceStateLock lock(statePath, lockTimeoutSeconds);
        Json state = Py::loadsText(Py::readText(statePath));
        Json conflicts = Py::get(state, QStringLiteral("conflicts"), Json::object());
        const Json recorded = conflicts.isObject() ? Py::get(conflicts, path) : Json();
        const Json recordedId = Py::get(recorded, QStringLiteral("conflict_id"));
        if (recordedId == Json(successorId)) {
            return true;
        }
        if (recordedId != Json(supersededId)) {
            *error = QStringLiteral("no recorded source conflict of %1 names %2").arg(path, supersededId);
            return false;
        }
        conflicts.insert(path, successorConflictRecord(recorded, successorId, Json(serverRevision), incomingSha256));
        state.insert(QStringLiteral("conflicts"), conflicts);
        Py::atomicWrite(statePath, Py::dumpsFile(state, true));
        return true;
    } catch (const Py::Exception &failure) {
        *error = failure.line();
        return false;
    }
}

}
