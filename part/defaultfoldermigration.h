// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Thoughts
{
// The client local migration of the default-folder rename (2026-10-17):
// .thoughts/projects/<hash>/documents becomes .thoughts/projects/<hash>/default
// once Fedora has renamed its copy. Sync runs it, under its own lock and the
// publisher's source-state lock, before reconciliation or publication:
//
//  1. a journal in the state directory records the folders and the phase;
//  2. each legacy folder is renamed in one step (unsynced edits, imported
//     PDFs and everything else move with it). When default/ already exists,
//     missing entries move, identical ones are dropped and a differing one
//     stops the migration with a problem that needs attention;
//  3. Rule R (SourceOwner::rewriteLegacyDefaultFolder) is applied textually
//     to every local state file and table that stores source paths, owners or
//     absolute paths; files named sha256(path) are renamed after it;
//  4. the local Project manifests get the same rewrite, so they equal
//     Fedora's migrated bytes;
//  5. rebuildable caches keyed by path are dropped;
//  6. the journal is marked done.
//
// Every step is idempotent: an interrupted migration reruns from its journal.
// Nothing here publishes a deletion of an old path.
class DefaultFolderMigration
{
public:
    struct Paths {
        QString docsRoot; // the Library
        QString stateRoot; // <application data>/thoughts-server (source-publication.json, desktop-sync.sqlite, ...)
        QString applicationDataRoot; // library-search.sqlite
        QString thoughtsDataRoot; // source-manifest.json, thoughts-server/review-query.sqlite
    };
    struct Result {
        bool success = true;
        bool migrated = false; // something was moved or rewritten
        QString error; // a readable summary when !success
        QString details;
        QJsonObject report; // per file/table rewrite counts, the final scan
    };

    explicit DefaultFolderMigration(Paths paths);

    QString journalPath() const;
    QJsonObject journal() const;
    // A journal whose phase is not "done": rerun at startup or Sync.
    bool interrupted() const;
    // Project hashes whose default folder is still .../documents on disk.
    QStringList legacyFolders() const;
    // Whether this computer's publication state still names a legacy folder.
    bool legacyStateRemains() const;
    // Fedora's Library project catalogue (GET library-projects) names a
    // default folder in its canonical spelling: Fedora has migrated.
    static bool fedoraUsesCanonicalSegment(const QJsonObject &librarySnapshot);
    // Fedora has migrated and this computer has not (or was interrupted).
    bool needed(const QJsonObject &librarySnapshot) const;

    // Steps 1-6. The caller stops its own writers of the state first (the
    // source change tracker, open stores with in-memory copies) and reloads
    // them afterwards.
    Result run();

private:
    Paths m_paths;
};

}
