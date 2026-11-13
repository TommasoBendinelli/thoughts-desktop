// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// The source inventory's rule for one file, for code that does not use
// exceptions (the app and thoughtsctl). Shares isManagedSourcePath with the
// publisher, so a file counts exactly when Sync would publish it.
namespace Thoughts::Publisher
{

// Whether Sync manages `path` (relative to the Library `docsRoot`) as a
// source once it exists: a file of a Project's folder in one of its source
// trees, not under tex/generated/ and not excluded by .ThoughtIgnore. Empty
// when the Project configurations cannot be read.
std::optional<bool> isManagedLibrarySource(const QString &docsRoot, const QString &path);
// The docs-root-relative files discovery lists now for the source folder
// `owner` (a docs-* folder or a Project's default folder), as a new source
// manifest would list them under that folder. Empty when discovery fails.
std::optional<QStringList> discoveredFolderSources(const QString &docsRoot, const QString &owner);

// Drops the proposal decision claims stored in `database` that no
// publication can carry, by the publisher's own rule: the file is not a
// managed source (neither in the manifest at `manifestPath` nor managed by
// isManagedLibrarySource), or it is gone with no Fedora base recorded in the
// publisher state at `sourceStatePath`. A Sync calls it when it publishes
// nothing, since the publisher only checks this while it runs. Reads nothing
// but the claims when there are none. `report` gets one line per dropped path
// and the reason a check failed. Returns how many claims were dropped, or -1
// when the check failed and every claim was kept. Defined in proposalclaims.cpp.
int dropInapplicableProposalClaims(const QString &docsRoot, const QString &manifestPath, const QString &sourceStatePath, const QString &database,
                                   const std::function<void(const QString &)> &report);

// After this computer's resolution of the source conflict `supersededId` was
// answered with the successor conflict `successorId` (Fedora changed again
// meanwhile and marked the resolved one superseded, which never resolves),
// records the successor for `path` in the publisher state at
// `sourceStatePath`, under its lock: its id, `serverRevision` (the revision
// its resolution expects) and `incomingSha256` (its incoming side, the
// resolution). The working file is not touched; the publisher still accepts
// what it held (sourceconflictrecord.h). True when the record names the
// successor afterwards (also when it already did). False with `error` when no
// record of `path` names either conflict or the state cannot be read or
// written; the publisher then follows the successor at its next run. Defined
// in sourceconflictrecord.cpp.
bool recordSuccessorSourceConflict(const QString &sourceStatePath, const QString &path, const QString &supersededId, const QString &successorId,
                                   qint64 serverRevision, const QString &incomingSha256, QString *error, double lockTimeoutSeconds = 2);

}
