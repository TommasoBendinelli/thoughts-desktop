// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyjson.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

// conflicts[path] of source-publication.json records a source conflict this
// computer preserved on Fedora: conflict_id, local_sha256 (the conflict's
// incoming side) and server_revision (the revision Fedora's resolution
// expects). A resolution answers with a successor conflict when Fedora
// changed again meanwhile; Fedora then marks the recorded conflict
// "superseded", which never resolves, so the record must follow the successor
// or the file can never be resolved or published again.
namespace Thoughts::Publisher
{

class SourceApi;

// The record of `successorId`, which superseded the conflict `recorded`
// names: its id, the revision its resolution expects and its incoming side
// (the resolution Fedora could not apply). The superseded record is kept in
// "superseded" (oldest first). Queuing a resolution never writes the working
// file, so after a merge that differs from it the file still holds what the
// first conflict recorded; acceptedLocalDigests keeps that accepted.
Py::Json successorConflictRecord(const Py::Json &recorded, const QString &successorId, const Py::Json &serverRevision, const QString &incomingSha256);

// The digests the working file may hold without an edit made after the
// conflict: the recorded incoming side and those of the records it superseded.
QSet<QString> acceptedLocalDigests(const Py::Json &recorded);

// Fedora's conflict lists, each read at most once.
class SourceConflictListings
{
public:
    explicit SourceConflictListings(SourceApi &api);
    const QList<Py::Json> &listing(const QString &status);

private:
    SourceApi &m_api;
    QHash<QString, QList<Py::Json>> m_listings;
};

// When Fedora superseded the conflict `recorded` names (a resolution made on
// any device met a newer Fedora version), the record of the conflict that
// replaced it, following the chain to its newest conflict. Fedora's list
// names no successor, so it is the conflict of `path` whose common base is
// the superseded conflict's Fedora side. Empty when the recorded conflict is
// not superseded. Raises ValueError when the successor cannot be identified.
std::optional<Py::Json> followSupersededConflict(SourceConflictListings &listings, const QString &path, const Py::Json &recorded);

// recordSuccessorSourceConflict, for the app, is declared in managedsource.h.

}
