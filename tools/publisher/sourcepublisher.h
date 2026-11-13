// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "frozeninputs.h"
#include "pyerror.h"
#include "sourceapi.h"
#include "sourceconflictrecord.h"

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

// The source publisher (thoughts_server.source_agent.SourcePublisher): an
// atomic, revision-checked publication of local source edits to Fedora,
// with its durable state file shared with the editable-source installer.
namespace Thoughts::Publisher
{

inline const QString emptySha256 = QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

// The optional fields of SourceProgressReporter.report.
struct ProgressFields {
    std::optional<QString> phase;
    QString state = QStringLiteral("running");
    std::optional<QString> path;
    std::optional<qint64> transferredBytes;
    std::optional<qint64> totalBytes;
    std::optional<QString> project;
    std::optional<qint64> selectedFiles;
    std::optional<QString> errorCode;
    std::optional<qint64> uploadedBytes;
    std::optional<qint64> downloadedBytes;
    std::optional<qint64> uploadTotalBytes;
    std::optional<qint64> downloadTotalBytes;
};
using ProgressCallback = std::function<void(const QString &, const ProgressFields &)>;

// SourceProgressReporter: a readable stderr line plus a THOUGHTS_SYNC_EVENT
// line per step, which Thoughts parses.
class ProgressReporter
{
public:
    ProgressReporter(const QString &target, const std::optional<Py::Path> &root);
    void report(const QString &message, const ProgressFields &fields = {});
    void failure(const Py::Exception &error);
    QString safeMessage(const QString &message) const;
    QString phase() const { return m_phase; }
    bool failureEmitted() const { return m_failureEmitted; }

private:
    QString m_target;
    std::optional<QString> m_root;
    QString m_phase = QStringLiteral("preparing");
    bool m_failureEmitted = false;
};

// _format_event_bytes
QString formatEventBytes(qint64 size);
// _stable_error_code
QString stableErrorCode(const Py::Exception &error, const QString &phase, const QString &operation = {});
// " ".join(text.split())
QString collapseWhitespace(const QString &text);

// {item["path"]: item for item in snapshot["sources"]}: Fedora's source heads.
class SourceHeads
{
public:
    explicit SourceHeads(const QList<Py::Json> &items = {});
    std::optional<Py::Json> get(const QString &path) const;
    // In first-seen order of path, each with its last item.
    QList<QPair<Py::Json, Py::Json>> items() const;

private:
    QList<QString> m_order;
    QHash<QString, QPair<Py::Json, Py::Json>> m_items;
};

// Source folder names spec: with a finished local rename (the journal
// source-folder-names-migration.json beside the state), bases recorded under
// an old folder follow the folder instead of being published as deletions;
// with an unfinished one this raises (nothing may be published).
void followSourceFolderRenames(const Py::Path &statePath, Py::Json &value);

class SourcePublisher
{
public:
    struct Report {
        qint64 unchanged = 0;
        qint64 upserted = 0;
        qint64 deleted = 0;
        Py::Json sourceFrontier = Py::Json(0);
        Py::Json publicationId = Py::Json(QString());
    };
    using LocalSources = QList<QPair<QString, std::optional<QByteArray>>>;

    SourcePublisher(const Py::Path &root, const Py::Path &manifest, const Py::Path &state, SourceApi &api,
                    const std::optional<Py::Path> &bootstrapState = std::nullopt, const std::optional<QSet<QString>> &includePaths = std::nullopt,
                    const std::optional<QString> &scopeProject = std::nullopt, const FrozenInputs *frozen = nullptr,
                    ProgressCallback progress = nullptr);

    Report publishOnce();
    qint64 adoptRemoteBases(const QSet<QString> &paths);

    // Shared with the editable-source installer.
    QStringList paths() const;
    QSet<QString> reconcilePreservedConflicts(LocalSources &local, const SourceHeads &current);
    bool activateFrozenRemoteSha(const QString &path, const QString &frozenSha256, const QByteArray &canonical, bool requireUnchanged);
    void writeState();
    void report(const QString &message, const ProgressFields &fields = {});
    // Fedora's conflict lists, read at most once per run (followSupersededConflict).
    SourceConflictListings &conflictListings();

    Py::Path root;
    Py::Path manifestPath;
    Py::Path statePath;
    SourceApi &api;
    Py::Json state;

private:
    std::optional<Py::Path> m_bootstrapStatePath;
    std::optional<QSet<QString>> m_includePaths;
    std::optional<QString> m_scopeProject;
    const FrozenInputs *m_frozen;
    ProgressCallback m_progress;
    std::optional<SourceConflictListings> m_conflictListings;

    Py::Json readState() const;
    void followSourceFolderRenames(Py::Json &value) const;
    Py::Json readManifest() const;
    LocalSources localSources();
    void checkManifestRetirement(const QString &path) const;
    void activateCleanRemote(const QString &path, const QByteArray &content);
    void activatePublicationResults(const Py::Json &result, const QHash<QString, QString> &frozenSha256ByPath);
    void recordBases(const std::optional<Py::Json> &snapshot = std::nullopt);
    qint64 pruneRetiredManifestPaths();
    Py::Json newMutation(const QString &action, const QString &path, const Py::Json &current, const std::optional<QByteArray> &content);
    // Fedora's next free sequence for this device when [error] refused a request
    // because a different operation owns its device sequence.
    std::optional<qint64> collidingSequenceNext(const Py::Exception &error);
    // Takes [count] fresh sequences, at or after both the local counter and [serverNext].
    qint64 reserveSequences(qint64 serverNext, qint64 count);
    // Sends the stored pending publication, renumbering it when Fedora proves another
    // operation owns one of its sequences (bounded; the IDs stay the same).
    Py::Json publishPending(Py::Json publication);
    Py::Json applyMutation(Py::Json mutation);
    void ensureEmptyBase(const QList<Py::Json> &mutations);
};

// adopt_review_source_bases(publisher, heads, paths)
void adoptReviewSourceBases(SourcePublisher &publisher, const SourceHeads &heads, const QSet<QString> &paths);

// sha256(bytes) as Python's helper names it.
QString sha256Of(const QByteArray &content);

}
