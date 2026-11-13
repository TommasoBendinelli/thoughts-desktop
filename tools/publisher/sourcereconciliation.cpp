// SPDX-License-Identifier: GPL-2.0-or-later
// The editable-source installer (thoughts_server.source_reconciliation):
// install remote authoring changes into the local docs folders, verifying
// every byte, and report which paths still need publication or attention.

#include "sourcereconciliation.h"

#include "htmlpages.h"

#include "argparse.h"
#include "projectconfig.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "serverids.h"
#include "sha256.h"
#include "sourceapi.h"
#include "sourceinventory.h"
#include "sourceowner.h"
#include "sourcemerge.h"
#include "sourcepublisher.h"
#include "statelock.h"

#include <QElapsedTimer>
#include <QUuid>

#include <chrono>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;
constexpr int rulesVersion = 1;

struct Decision {
    QString action;
    QString reason;
};

// sync_core.sourceDecision for boolean facts.
Decision sourceDecision(bool sameContents, bool baseKnown, bool localExists, bool absenceIsDeletion, bool localMatchesBase, bool remoteMatchesBase)
{
    if (sameContents) {
        return {QStringLiteral("acknowledge"), QStringLiteral("local_matches_remote")};
    }
    if (!localExists && !absenceIsDeletion) {
        return {QStringLiteral("install_remote"), QStringLiteral("restore_evicted_source")};
    }
    if (!baseKnown && !localExists) {
        return {QStringLiteral("install_remote"), QStringLiteral("new_remote_source")};
    }
    if (!baseKnown) {
        return {QStringLiteral("needs_attention"), QStringLiteral("missing_merge_base")};
    }
    if (localMatchesBase) {
        return {QStringLiteral("install_remote"), QStringLiteral("remote_only_change")};
    }
    if (remoteMatchesBase) {
        return {QStringLiteral("publish_local"), QStringLiteral("local_only_change")};
    }
    return {QStringLiteral("merge"), QStringLiteral("both_sides_changed")};
}

struct Identity {
    quint64 device;
    quint64 inode;
    qint64 size;
    qint64 modified;
    qint64 changed;
    bool operator==(const Identity &) const = default;
};

// _identity(path): stat() identity, or nothing for a missing file.
std::optional<Identity> identityOf(const Py::Path &path)
{
    const Py::FileStatus status = Py::status(path);
    if (status.kind == Py::FileStatus::Kind::Missing) {
        if (status.error == ENOENT) {
            return std::nullopt;
        }
        Py::raiseOsError(status.error, path.str());
    }
    return Identity{status.device, status.inode, status.size, status.modifiedNs, status.changedNs};
}

std::optional<QString> shaOf(const std::optional<QByteArray> &content)
{
    return content ? std::optional<QString>(sha256Hex(*content)) : std::nullopt;
}

Json optionalString(const std::optional<QString> &value)
{
    return value ? Json(*value) : Json();
}

// _layout_only_change: a Project manifest that only differs from its base in
// JSON layout is unchanged.
bool layoutOnlyChange(SourceApi &api, const QString &path, const std::optional<QByteArray> &local, const std::optional<Json> &base)
{
    if (!local || !base || Py::truthy(Py::get(*base, QStringLiteral("deleted"), Json(false))) || !path.endsWith(QLatin1String("/thoughts-project.json"))) {
        return false;
    }
    const QByteArray baseContent = api.download(Py::pythonStr(Py::subscript(*base, QStringLiteral("object_sha256"))));
    if (Json(sha256Hex(baseContent)) != Py::subscript(*base, QStringLiteral("sha256"))) {
        Py::raiseValueError(QStringLiteral("source base failed identity verification"));
    }
    const std::optional<Json> localValue = Py::loads(*local);
    const std::optional<Json> baseValue = Py::loads(baseContent);
    return localValue && baseValue && *localValue == *baseValue;
}

// The sha of a JSON value as Python compares it: a str, or None.
std::optional<QString> remoteSha(const Json &remote)
{
    if (Py::truthy(Py::get(remote, QStringLiteral("deleted"), Json(false)))) {
        return std::nullopt;
    }
    return Py::pythonStr(Py::subscript(remote, QStringLiteral("sha256")));
}

Json decisionJson(const QString &path, const QString &action, const QString &reason)
{
    return Json::object({{QStringLiteral("path"), Json(path)}, {QStringLiteral("action"), Json(action)}, {QStringLiteral("reason"), Json(reason)}});
}

// _validate_source_content
void validateSourceContent(const QByteArray &content)
{
    if (content.size() > maximumManagedSourceBytes) {
        Py::raise(Py::sourceApplyErrorType, QStringLiteral("source content exceeds the 4 MiB limit"));
    }
    if (content.contains('\0')) {
        Py::raise(Py::sourceApplyErrorType, QStringLiteral("source content contains a NUL byte"));
    }
    if (!Py::decodeUtf8(content)) {
        Py::raise(Py::sourceApplyErrorType, QStringLiteral("source content is not valid UTF-8"));
    }
}

bool hasConflictMarkers(const QByteArray &content)
{
    for (const QByteArray &line : content.split('\n')) {
        if (line.startsWith("<<<<<<< ") || line.startsWith("======= ") || line.startsWith(">>>>>>> ")) {
            return true;
        }
    }
    return false;
}

void unlinkFile(const Py::Path &path)
{
#ifdef Q_OS_WIN
    if (!DeleteFileW(path.str().toStdWString().c_str())) {
        const DWORD error = GetLastError();
        Py::raiseOsError(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES, path.str());
    }
#else
    if (::unlink(Py::encodeFileName(path.str()).constData()) != 0) {
        Py::raiseOsError(errno, path.str());
    }
#endif
}

bool caughtInMerge(const Py::Exception &error)
{
    return Py::isOsError(error) || Py::isValueError(error) || error.type == Py::apiErrorType || error.type == Py::sourceInfrastructureErrorType;
}

bool caughtInInstall(const Py::Exception &error)
{
    return Py::isOsError(error) || Py::isValueError(error) || error.type == Py::apiErrorType;
}

struct Candidate {
    QString path;
    Py::Path destination;
    std::optional<Identity> identity;
    Json remote;
    QString action;
};

Json reconcile(SourcePublisher &publisher, qint64 through)
{
    QElapsedTimer clock;
    clock.start();
    const QString manifestSha256 = sha256Hex(Py::readBytes(publisher.manifestPath));
    const Json snapshot = publisher.api.sourceTree();
    const qint64 observed = Py::toPythonInt(Py::get(snapshot, QStringLiteral("source_frontier"), Json(-1)));
    if (observed < through) {
        Py::raiseValueError(QStringLiteral("source snapshot predates the requested server checkpoint"));
    }
    const SourceHeads heads(Py::iterate(Py::subscript(snapshot, QStringLiteral("sources"))));
    {
        const QStringList managed = publisher.paths();
        adoptReviewSourceBases(publisher, heads, QSet<QString>(managed.cbegin(), managed.cend()));
    }
    if (!publisher.state.contains(QStringLiteral("bases"))) {
        publisher.state.insert(QStringLiteral("bases"), Json::object());
    }
    if (!publisher.state.contains(QStringLiteral("materialized_sources"))) {
        publisher.state.insert(QStringLiteral("materialized_sources"), Json::object());
    }
    Json bases = publisher.state.value(QStringLiteral("bases"));
    Json receipts = publisher.state.value(QStringLiteral("materialized_sources"));
    const auto storeState = [&] {
        publisher.state.insert(QStringLiteral("bases"), bases);
        publisher.state.insert(QStringLiteral("materialized_sources"), receipts);
    };
    QList<Candidate> candidates;
    QStringList blocked;
    QStringList pending;
    QList<Json> verified;
    QList<Json> decisions;
    std::optional<QSet<QString>> resolvedIds;
    qint64 hashes = 0;
    qint64 installedCount = 0;
    qint64 reusedObjects = 0;
    const Py::Path &root = publisher.root;
    for (const QString &path : publisher.paths()) {
        const std::optional<Json> remoteValue = heads.get(path);
        if (!remoteValue) {
            continue;
        }
        const Json remote = *remoteValue;
        std::optional<Json> base = bases.contains(path) ? std::optional<Json>(bases.value(path)) : std::nullopt;
        const std::optional<QString> remoteDigest = remoteSha(remote);
        const Json receipt = receipts.contains(path) ? receipts.value(path) : Json::object();
        const Json conflicts = Py::get(publisher.state, QStringLiteral("conflicts"), Json::object());
        if (conflicts.isObject() && conflicts.contains(path)) {
            Json conflict = conflicts.value(path);
            if (!resolvedIds) {
                resolvedIds = publisher.api.resolvedConflictIds();
            }
            if (!resolvedIds->contains(Py::hashKey(Py::get(conflict, QStringLiteral("conflict_id"))))
                && Py::toPythonInt(Py::get(remote, QStringLiteral("revision"), Json(-1))) > Py::toPythonInt(Py::get(conflict, QStringLiteral("server_revision"), Json(-1)))) {
                // Only a conflict whose file changed on Fedora can have been
                // superseded (sourceconflictrecord.h); its record follows the successor.
                std::optional<Json> successor;
                try {
                    successor = followSupersededConflict(publisher.conflictListings(), path, conflict);
                } catch (const Py::Exception &error) {
                    if (!Py::isValueError(error)) {
                        throw;
                    }
                    blocked.append(path);
                    decisions.append(decisionJson(path, QStringLiteral("needs_attention"), error.message));
                    continue;
                }
                if (successor) {
                    // Written with the rest of this reconciliation's state.
                    conflict = *successor;
                    Json recorded = Py::get(publisher.state, QStringLiteral("conflicts"), Json::object());
                    recorded.insert(path, conflict);
                    publisher.state.insert(QStringLiteral("conflicts"), recorded);
                }
            }
            if (!resolvedIds->contains(Py::hashKey(Py::get(conflict, QStringLiteral("conflict_id"))))) {
                blocked.append(path);
                decisions.append(decisionJson(path, QStringLiteral("needs_attention"), QStringLiteral("unresolved_source_conflict")));
                continue;
            }
            const Py::Path target = Py::resolve(root / path);
            if (!target.isRelativeTo(root)) {
                Py::raiseValueError(QStringLiteral("conflicted source escapes the authoring root"));
            }
            const std::optional<QByteArray> local = Py::isFile(target) ? std::optional<QByteArray>(Py::readBytes(target)) : std::nullopt;
            storeState();
            try {
                SourcePublisher::LocalSources single{{path, local}};
                publisher.reconcilePreservedConflicts(single, heads);
            } catch (const Py::Exception &error) {
                if (!Py::isValueError(error)) {
                    throw;
                }
                bases = publisher.state.value(QStringLiteral("bases"));
                blocked.append(path);
                decisions.append(decisionJson(path, QStringLiteral("needs_attention"), error.message));
                continue;
            }
            bases = publisher.state.value(QStringLiteral("bases"));
            const std::optional<QByteArray> after = Py::isFile(target) ? std::optional<QByteArray>(Py::readBytes(target)) : std::nullopt;
            installedCount += local != after ? 1 : 0;
            base = bases.contains(path) ? std::optional<Json>(bases.value(path)) : std::nullopt;
        }
        if (Py::get(receipt, QStringLiteral("revision")) == Py::subscript(remote, QStringLiteral("revision"))
            && Py::get(receipt, QStringLiteral("sha256")) == optionalString(remoteDigest)) {
            continue;
        }
        const Py::Path destination = Py::resolve(root / path);
        if (!destination.isRelativeTo(root)) {
            blocked.append(path);
            decisions.append(decisionJson(path, QStringLiteral("needs_attention"), QStringLiteral("external_source_owner")));
            continue;
        }
        const std::optional<Identity> identity = identityOf(destination);
        const std::optional<QByteArray> local = identity ? std::optional<QByteArray>(Py::readBytes(destination)) : std::nullopt;
        if (identity != identityOf(destination)) {
            blocked.append(path);
            decisions.append(decisionJson(path, QStringLiteral("needs_attention"), QStringLiteral("edited_during_inspection")));
            continue;
        }
        hashes += local ? 1 : 0;
        const std::optional<QString> localSha = shaOf(local);
        std::optional<QString> baseSha;
        if (base) {
            baseSha = Py::truthy(Py::get(*base, QStringLiteral("deleted"))) ? std::nullopt
                                                                             : std::optional<QString>(Py::pythonStr(Py::subscript(*base, QStringLiteral("sha256"))));
        }
        bool localMatchesBase = base && localSha == baseSha;
        if (!localMatchesBase && localSha != remoteDigest) {
            // A rewritten but equivalent manifest must not block a remote change or deletion.
            localMatchesBase = layoutOnlyChange(publisher.api, path, local, base);
        }
        Decision decision = sourceDecision(localSha == remoteDigest, base.has_value(), local.has_value(), true, localMatchesBase,
                                           base && remoteDigest == baseSha);
        if (decision.reason == QLatin1String("missing_merge_base") && remoteDigest && isManifestPath(path)) {
            // A Project file without a base that means the same as Fedora's
            // (this computer's own copy in Qt's layout) takes Fedora's bytes,
            // as the publisher does; any other difference still needs attention.
            const QByteArray remoteContent = publisher.api.download(Py::pythonStr(Py::subscript(remote, QStringLiteral("object_sha256"))));
            if (std::optional<QString>(sha256Hex(remoteContent)) != remoteDigest) {
                Py::raiseValueError(QStringLiteral("remote source failed identity verification"));
            }
            const std::optional<QByteArray> meaning = semanticIdentity(path, *local);
            if (meaning && meaning == semanticIdentity(path, remoteContent)) {
                decision = {QStringLiteral("install_remote"), QStringLiteral("equivalent_change")};
            }
        }
        decisions.append(decisionJson(path, decision.action, decision.reason));
        if (decision.action == QLatin1String("merge")) {
            if (!local || Py::truthy(Py::get(remote, QStringLiteral("deleted"), Json(false))) || Py::truthy(Py::get(*base, QStringLiteral("deleted"), Json(false)))) {
                pending.append(path);
                continue;
            }
            try {
                const QByteArray baseContent = publisher.api.download(Py::pythonStr(Py::subscript(*base, QStringLiteral("object_sha256"))));
                const QByteArray remoteContent = publisher.api.download(Py::pythonStr(Py::subscript(remote, QStringLiteral("object_sha256"))));
                if (Json(sha256Hex(baseContent)) != Py::subscript(*base, QStringLiteral("sha256")) || std::optional<QString>(sha256Hex(remoteContent)) != remoteDigest) {
                    Py::raiseValueError(QStringLiteral("source merge input failed identity verification"));
                }
                const auto [merged, conflictCount] = mergeSources(baseContent, remoteContent, *local, path);
                if (conflictCount) {
                    pending.append(path);
                    continue;
                }
                validateSourceContent(merged);
                if (hasConflictMarkers(merged)) {
                    Py::raiseValueError(QStringLiteral("merged source contains conflict markers"));
                }
                if (identityOf(destination) != identity || sha256Hex(Py::readBytes(destination)) != *localSha) {
                    Py::raiseValueError(QStringLiteral("edited_during_source_merge"));
                }
                const Py::Path recovery = publisher.statePath.parent() / QStringLiteral("source-merge-recoveries") / sha256Hex(path.toUtf8());
                Py::atomicWrite(withSuffix(recovery, QStringLiteral(".local")), *local);
                const Json record = Json::object({{QStringLiteral("path"), Json(path)},
                                                  {QStringLiteral("base_sha256"), Py::subscript(*base, QStringLiteral("sha256"))},
                                                  {QStringLiteral("remote_sha256"), optionalString(remoteDigest)},
                                                  {QStringLiteral("local_sha256"), optionalString(localSha)},
                                                  {QStringLiteral("merged_sha256"), Json(sha256Hex(merged))}});
                Py::atomicWrite(withSuffix(recovery, QStringLiteral(".json")), Py::dumpsChecked(record));
                Py::atomicWrite(destination, merged);
                if (sha256Hex(Py::readBytes(destination)) != sha256Hex(merged)) {
                    Py::raiseValueError(QStringLiteral("merged source changed before verification"));
                }
                const Json newReceipt = Json::object({{QStringLiteral("revision"), Py::subscript(remote, QStringLiteral("revision"))},
                                                      {QStringLiteral("sha256"), optionalString(remoteDigest)},
                                                      {QStringLiteral("object_sha256"), Py::subscript(remote, QStringLiteral("object_sha256"))},
                                                      {QStringLiteral("action"), Json(QStringLiteral("auto_merge"))},
                                                      {QStringLiteral("rules_version"), Json(rulesVersion)}});
                receipts.insert(path, newReceipt);
                bases.insert(path, Json::object({{QStringLiteral("revision"), Py::subscript(remote, QStringLiteral("revision"))},
                                                 {QStringLiteral("sha256"), Py::subscript(remote, QStringLiteral("sha256"))},
                                                 {QStringLiteral("object_sha256"), Py::subscript(remote, QStringLiteral("object_sha256"))}}));
                ++installedCount;
                Json entry = Json::object({{QStringLiteral("path"), Json(path)}});
                for (const auto &[key, value] : newReceipt.members()) {
                    entry.insert(key, value);
                }
                entry.insert(QStringLiteral("exists"), Json(true));
                entry.insert(QStringLiteral("local_sha256"), Json(sha256Hex(merged)));
                verified.append(entry);
                if (merged == remoteContent) {
                    decisions.last() = decisionJson(path, QStringLiteral("install_remote"), QStringLiteral("equivalent_change"));
                    continue;
                }
                pending.append(path);
                decisions.last() = decisionJson(path, QStringLiteral("auto_merge"), QStringLiteral("clean_three_way_merge"));
            } catch (const Py::Exception &error) {
                if (!caughtInMerge(error)) {
                    throw;
                }
                blocked.append(path);
                decisions.append(decisionJson(path, QStringLiteral("needs_attention"), error.message));
            }
            continue;
        }
        if (decision.action == QLatin1String("publish_local")) {
            pending.append(path);
            continue;
        }
        if (decision.action == QLatin1String("needs_attention")) {
            blocked.append(path);
            continue;
        }
        candidates.append({path, destination, identity, remote, decision.action});
    }

    // Files another device added to an installed folder.
    const QStringList managedList = publisher.paths();
    const QSet<QString> managed(managedList.cbegin(), managedList.cend());
    QSet<QString> installedOwners;
    for (const Json &project : Py::iterate(Py::get(Py::loadsText(Py::readText(publisher.manifestPath)), QStringLiteral("projects"), Json::array()))) {
        installedOwners.insert(Py::hashKey(Py::get(project, QStringLiteral("project"))));
    }
    QList<QPair<Json, Json>> headItems = heads.items();
    std::stable_sort(headItems.begin(), headItems.end(), [](const QPair<Json, Json> &left, const QPair<Json, Json> &right) {
        return Py::codePointLess(left.first.toString(), right.first.toString());
    });
    for (const auto &[pathValue, remote] : std::as_const(headItems)) {
        const QString path = pathValue.toString();
        if (managed.contains(path) || bases.contains(path) || Py::truthy(Py::get(remote, QStringLiteral("deleted"), Json(false)))) {
            continue;
        }
        const qsizetype slash = path.indexOf(QLatin1Char('/'));
        // A Project's default folder spans four segments; docs-* folders one.
        const QString defaultOwner = SourceOwner::isDefaultFolder(SourceOwner::of(path)) ? SourceOwner::of(path) : QString();
        const QString owner = !defaultOwner.isEmpty() ? defaultOwner : (slash < 0 ? path : path.left(slash));
        const QString relative = !defaultOwner.isEmpty() ? SourceOwner::relative(path) : (slash < 0 ? QString() : path.mid(slash + 1));
        if (!installedOwners.contains(Py::hashKey(Json(owner))) || !isAuthoredSourcePath(root / owner, relative)) {
            continue;
        }
        const Py::Path destination = Py::resolve(root / path);
        if (!destination.isRelativeTo(root) || Py::linkExists(destination)) {
            continue;
        }
        decisions.append(decisionJson(path, QStringLiteral("install_remote"), QStringLiteral("added_on_another_device")));
        candidates.append({path, destination, std::nullopt, remote, QStringLiteral("install_remote")});
    }

    for (const Candidate &candidate : std::as_const(candidates)) {
        try {
            std::optional<QByteArray> content;
            bool reused = false;
            const bool remoteDeleted = Py::truthy(Py::get(candidate.remote, QStringLiteral("deleted"), Json(false)));
            if (candidate.action == QLatin1String("install_remote") && !remoteDeleted) {
                const QString objectSha = Py::pythonStr(Py::subscript(candidate.remote, QStringLiteral("object_sha256")));
                const Py::Path cached = cachedObjectPath(publisher.statePath.parent(), objectSha);
                if (Py::isFile(cached) && Py::status(cached).size <= maximumManagedSourceBytes) {
                    content = Py::readBytes(cached);
                }
                const Json expected = Py::subscript(candidate.remote, QStringLiteral("sha256"));
                reused = content && Json(sha256Hex(*content)) == expected;
                if (!reused) {
                    content = publisher.api.download(objectSha);
                }
                if (content->size() > maximumManagedSourceBytes) {
                    Py::raiseValueError(QStringLiteral("remote source exceeds the managed source size limit"));
                }
                if (Json(sha256Hex(*content)) != expected) {
                    Py::raiseValueError(QStringLiteral("remote source failed content identity verification"));
                }
                QString error;
                if (!Py::decodeUtf8(*content, &error)) {
                    Py::raise(QStringLiteral("UnicodeDecodeError"), error);
                }
            }
            reusedObjects += reused ? 1 : 0;
            if (Py::resolve(root / candidate.path) != candidate.destination || identityOf(candidate.destination) != candidate.identity) {
                Py::raiseValueError(QStringLiteral("edited_during_download"));
            }
            if (candidate.action == QLatin1String("install_remote")) {
                if (remoteDeleted) {
                    if (Py::exists(candidate.destination)) {
                        unlinkFile(candidate.destination);
                    }
                } else {
                    Py::atomicWrite(candidate.destination, *content);
                }
            }
            const std::optional<QByteArray> installed =
                Py::isFile(candidate.destination) ? std::optional<QByteArray>(Py::readBytes(candidate.destination)) : std::nullopt;
            const std::optional<QString> installedSha = shaOf(installed);
            const std::optional<QString> expectedSha = remoteSha(candidate.remote);
            if (Py::resolve(root / candidate.path) != candidate.destination || installedSha != expectedSha) {
                Py::raiseValueError(QStringLiteral("edited_before_acknowledgement"));
            }
            const Json newReceipt = Json::object({{QStringLiteral("revision"), Py::subscript(candidate.remote, QStringLiteral("revision"))},
                                                  {QStringLiteral("sha256"), optionalString(expectedSha)},
                                                  {QStringLiteral("object_sha256"), Py::get(candidate.remote, QStringLiteral("object_sha256"))},
                                                  {QStringLiteral("action"), Json(candidate.action)},
                                                  {QStringLiteral("rules_version"), Json(rulesVersion)}});
            receipts.insert(candidate.path, newReceipt);
            Json newBase = Json::object({{QStringLiteral("revision"), Py::subscript(candidate.remote, QStringLiteral("revision"))},
                                         {QStringLiteral("sha256"), Py::subscript(candidate.remote, QStringLiteral("sha256"))},
                                         {QStringLiteral("object_sha256"), Py::subscript(candidate.remote, QStringLiteral("object_sha256"))}});
            if (remoteDeleted) {
                newBase.insert(QStringLiteral("deleted"), Json(true));
            }
            bases.insert(candidate.path, newBase);
            installedCount += candidate.action == QLatin1String("install_remote") ? 1 : 0;
            Json entry = Json::object({{QStringLiteral("path"), Json(candidate.path)}});
            for (const auto &[key, value] : newReceipt.members()) {
                entry.insert(key, value);
            }
            entry.insert(QStringLiteral("exists"), Json(installed.has_value()));
            verified.append(entry);
        } catch (const Py::Exception &error) {
            if (!caughtInInstall(error)) {
                throw;
            }
            blocked.append(candidate.path);
            decisions.append(decisionJson(candidate.path, QStringLiteral("needs_attention"), error.message));
        }
    }
    if (sha256Hex(Py::readBytes(publisher.manifestPath)) != manifestSha256) {
        Py::raiseValueError(QStringLiteral("managed source inventory changed during reconciliation"));
    }
    const bool complete = blocked.isEmpty() && pending.isEmpty();
    storeState();
    if (complete) {
        publisher.state.insert(QStringLiteral("materialized_source_frontier"), Json(observed));
    }
    const auto stringArray = [](const QStringList &values) {
        Json array = Json::array();
        for (const QString &value : values) {
            array.append(Json(value));
        }
        return array;
    };
    publisher.state.insert(QStringLiteral("authoring_reconciliation"),
                           Json::object({{QStringLiteral("checked_frontier"), Json(observed)},
                                         {QStringLiteral("blocked_paths"), stringArray(blocked)},
                                         {QStringLiteral("pending_paths"), stringArray(pending)}}));
    publisher.writeState();
    // The folders' HTML pages Fedora added or changed (protocol/html-pages.md 4): its list just arrived.
    const HtmlPageSync pages = syncHtmlPages(publisher.api, publisher.root, publisher.statePath, htmlPageFolders(publisher.paths()));
    // Python's round(): half to even.
    const double elapsed = double(clock.nsecsElapsed()) / 1000000.0;
    const qint64 duration = qint64(std::nearbyint(elapsed));
    Json report = Json::object({{QStringLiteral("schema_version"), Json(1)},
                         {QStringLiteral("rules_version"), Json(rulesVersion)},
                         {QStringLiteral("checked_frontier"), Json(observed)},
                         {QStringLiteral("manifest_sha256"), Json(manifestSha256)},
                         {QStringLiteral("complete"), Json(complete)},
                         {QStringLiteral("verified"), Json::array(verified)},
                         {QStringLiteral("blocked_paths"), stringArray(blocked)},
                         {QStringLiteral("pending_paths"), stringArray(pending)},
                         {QStringLiteral("decisions"), Json::array(decisions)},
                         {QStringLiteral("hashed_files"), Json(hashes)},
                         {QStringLiteral("downloaded_bytes"), Json(publisher.api.transferTotals().second)},
                         {QStringLiteral("reused_objects"), Json(reusedObjects)},
                         {QStringLiteral("installed_files"), Json(installedCount)},
                         {QStringLiteral("duration_ms"), Json(duration)}});
    if (pages.reportable()) {
        report.insert(QStringLiteral("html_pages"), pages.toJson());
    }
    return report;
}
}

int runReconcile(const ArgParse::Arguments &arguments)
{
    const QString token = qEnvironmentVariable("THOUGHTS_TOKEN");
    if (token.isEmpty()) {
        return ArgParse::programError(ArgParse::reconcileProgram(), QStringLiteral("THOUGHTS_TOKEN is required"));
    }
    const Py::Path state(arguments.value(QStringLiteral("--state")));
    SourceStateLock lock(state);
    SourceApi api(arguments.value(QStringLiteral("--server")), token);
    std::optional<Py::Path> bootstrap;
    if (arguments.has(QStringLiteral("--bootstrap-state"))) {
        bootstrap = Py::Path(arguments.value(QStringLiteral("--bootstrap-state")));
    }
    SourcePublisher publisher(Py::Path(arguments.value(QStringLiteral("--root"))), Py::Path(arguments.value(QStringLiteral("--manifest"))), state, api,
                              bootstrap);
    Json result = reconcile(publisher, arguments.value(QStringLiteral("--through")).toLongLong());
    const QString trace = canonicalUuid(qEnvironmentVariable("THOUGHTS_TRACE_ID")).value_or(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const qint64 nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const Py::Path reportPath = state.parent() / QStringLiteral("source-reconciliation-reports") / QStringLiteral("%1-%2.json").arg(trace).arg(nanoseconds);
    result.insert(QStringLiteral("report_path"), Json(reportPath.str()));
    result.insert(QStringLiteral("trace_id"), Json(trace));
    Py::makeDirectories(reportPath.parent());
    const QByteArray content = Py::dumpsFile(result);
    Py::atomicWrite(reportPath, content);
    Py::atomicWrite(state.parent() / QStringLiteral("authoring-reconciliation.json"), content);
    Py::printOut(Py::dumpsChecked(result));
    return 0;
}

}
