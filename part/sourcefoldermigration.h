// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "sourcefoldernames.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Thoughts
{
// The client side of the source folder naming change (2026-10-27): Fedora
// renamed each connected folder docs-<old> to docs-<slug>-<hex> and serves the
// map as library-projects `source_folder_renames`. Sync applies it here, under
// the Sync lock, the publisher's source-state lock and the catalogue lock,
// before reconciliation or publication (the same hook points as the
// default-folder rename):
//
//  1. a journal (source-folder-names-migration.json in the state directory)
//     records the map and the phase (moving, rewriting, done);
//  2. folders with files open in another program stop the run before anything
//     moves (Windows cannot rename them); the run is retried on the next Sync;
//  3. each folder is renamed in one step; when the new folder already exists
//     (a straggler editor recreated the old one, an interrupted run), missing
//     files move, identical ones are dropped and a differing one stops the run
//     with a problem that needs attention;
//  4. rule P (SourceFolderNames) rewrites every local state file and table
//     that records source paths or folder names: JSON keys and single-line
//     strings outside contents fields, SQLite text (a rewritten key that is
//     already taken keeps the existing row), files named sha256(path) and
//     source-conflicts paths, the local Project manifests (with their
//     generated_manifests digests);
//  5. rebuildable caches keyed by path are dropped;
//  6. a final scan, then the journal is marked done.
//
// Every step is idempotent: an interrupted run reruns from its journal. The
// publisher refuses to publish while the journal is unfinished and never
// publishes a deletion of an old path (SourcePublisher::followSourceFolderRenames).
class SourceFolderMigration
{
public:
    struct Paths {
        QString docsRoot; // the Library
        QString stateRoot; // <application data>/thoughts-server
        QString applicationDataRoot; // library-search.sqlite
        QString thoughtsDataRoot; // source-manifest.json, source-sync-state.json, thoughts-server/review-query.sqlite
    };
    struct Result {
        bool success = true;
        bool migrated = false; // something was moved or rewritten
        QString error; // a readable summary when !success
        QString details;
        QJsonObject report;
    };

    explicit SourceFolderMigration(Paths paths);

    static constexpr QLatin1StringView JournalName{"source-folder-names-migration.json"};

    QString journalPath() const;
    QJsonObject journal() const;
    // A journal whose phase is not "done": rerun at startup or Sync.
    bool interrupted() const;
    // The map the journal applies (empty without a journal).
    SourceFolderRenames journalRenames() const;
    // Fedora's map from GET library-projects.
    static SourceFolderRenames renamesFrom(const QJsonObject &librarySnapshot);
    // The entries of renames this computer still has to apply: the old folder
    // exists here (alone, or as a straggler next to the new one), or this
    // computer's publication state or catalogue still names it.
    SourceFolderRenames pending(const SourceFolderRenames &renames) const;
    // An interrupted run, or Fedora's map has entries this computer has not applied.
    bool needed(const QJsonObject &librarySnapshot) const;

    // The connected folders (docs-*) the local Project manifests declare.
    // Folders no manifest declares are never renamed and never count.
    QStringList declaredFolders() const;
    // Declared folders not named docs-<slug>-<hex> for which the map has no entry.
    QStringList unresolvedOldFolders(const SourceFolderRenames &stored) const;
    // An identity of the stored catalogue (.thoughts/library-projects.json):
    // its Projects' revisions and its rename map.
    static QString catalogueKey(const QString &docsRoot);

    // What a document Sync (which skips the Library project exchange) does:
    // apply the stored catalogue's map (a resumed or pending rename), ask
    // Fedora's catalogue (a declared folder of the old form that the stored
    // map does not cover; at most once per catalogue key, recorded in
    // *askedKey for this app run), or nothing. Full Syncs always fetch the
    // catalogue and need none of this.
    enum class DocumentSyncCheck { Nothing, ApplyStored, AskFedora };
    DocumentSyncCheck documentSyncCheck(const SourceFolderRenames &stored, const QString &catalogueKey, QString *askedKey) const;

    // Steps 1-6 for the pending entries of renames (and any the journal
    // still lists). The caller stops its own writers of the state first and
    // reloads them afterwards.
    Result run(const SourceFolderRenames &renames);

private:
    Paths m_paths;
};

}
