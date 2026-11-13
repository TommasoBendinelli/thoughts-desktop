// SPDX-License-Identifier: GPL-2.0-or-later
// thoughts-publish-sources publish: one atomic, revision-checked publication of
// the managed sources, then the build inputs of the selected PDFs, as the
// Python source_agent.main did.

#include "publishcommand.h"

#include "argparse.h"
#include "buildinputs.h"
#include "frozeninputs.h"
#include "htmlpages.h"
#include "manifestcommands.h"
#include "projectconfig.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sha256.h"
#include "sourceapi.h"
#include "sourceinventory.h"
#include "sourceowner.h"
#include "sourcepublisher.h"
#include "statelock.h"

#include <QMap>
#include <QUuid>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

const QSet<QString> buildObjectRoles{QStringLiteral("asset"), QStringLiteral("external_pdf"), QStringLiteral("generated")};

ProgressFields phase(const QString &name)
{
    ProgressFields fields;
    fields.phase = name;
    return fields;
}

Json entryJson(const BuildInput &entry)
{
    return Json::object({{QStringLiteral("path"), Json(entry.path)},
                         {QStringLiteral("sha256"), Json(entry.sha256)},
                         {QStringLiteral("size_bytes"), Json(entry.sizeBytes)},
                         {QStringLiteral("media_type"), Json(entry.mediaType)},
                         {QStringLiteral("role"), Json(entry.role)}});
}

// _project_from_entries
QString projectFromEntries(const QList<BuildInput> &entries)
{
    QSet<QString> owners;
    QSet<QString> defaultOwners;
    for (const BuildInput &entry : entries) {
        if (buildObjectRoles.contains(entry.role) && entry.path.startsWith(QLatin1String("docs-"))) {
            owners.insert(entry.path.section(QLatin1Char('/'), 0, 0));
        } else if (buildObjectRoles.contains(entry.role) && entry.role != QLatin1String("external_pdf")
                   && SourceOwner::isDefaultFolder(SourceOwner::of(entry.path))) {
            // A Project's imported PDFs serve every folder's builds; its
            // default folder's own assets name that folder.
            defaultOwners.insert(SourceOwner::of(entry.path));
        }
    }
    if (owners.isEmpty()) {
        owners = defaultOwners;
    }
    if (owners.size() != 1) {
        Py::raiseValueError(QStringLiteral("A document build requires its authoritative source folder"));
    }
    return *owners.cbegin();
}

void requireCanonicalRoles(const QList<BuildInput> &entries)
{
    for (const BuildInput &entry : entries) {
        if (!buildObjectRoles.contains(entry.role)) {
            Py::raiseValueError(QStringLiteral("version-three build input entries require canonical roles"));
        }
    }
}

// _build_input_snapshot_sha256
QString snapshotSha256(const QString &target, const QList<BuildInput> &entries, const QString &project, const QString &policy)
{
    requireCanonicalRoles(entries);
    return sha256Hex(buildInputSnapshotBytes(target, entries, project.isEmpty() ? projectFromEntries(entries) : project, policy));
}

// build_input_entries via _publication_build_input_entries
QList<BuildInput> publicationBuildInputEntries(const Py::Path &givenRoot, const QString &target, const std::optional<QString> &project,
                                               const FrozenInputs *frozen)
{
    if (target.contains(QLatin1Char('/')) || target.contains(QLatin1Char('\\')) || !Py::casefold(target).endsWith(QLatin1String(".pdf"))) {
        Py::raiseValueError(QStringLiteral("build target must be a PDF filename"));
    }
    const Py::Path docsRoot = Py::resolve(givenRoot);
    const auto [projectName, projectRoot] = targetProject(docsRoot, target, project, true);
    if (frozen) {
        if (const std::optional<QList<BuildInput>> entries = frozen->buildEntries(target, projectName)) {
            return *entries;
        }
    }
    QSet<QString> managed;
    const Json authored = authoredSourceEntries(docsRoot, projectRoot);
    for (const Json &entry : authored.items()) {
        managed.insert(entry.value(QStringLiteral("path")).toString());
    }
    return projectBuildInputEntries(docsRoot, projectName, projectRoot, managed);
}

void reportBuildInputSelection(ProgressReporter &reporter, const QString &target, const QString &project, const QList<BuildInput> &entries)
{
    QMap<QString, qint64> counts{{QStringLiteral("asset"), 0}, {QStringLiteral("external_pdf"), 0}, {QStringLiteral("generated"), 0}};
    qint64 total = 0;
    for (const BuildInput &entry : entries) {
        if (counts.contains(entry.role)) {
            ++counts[entry.role];
        }
        total += entry.sizeBytes;
    }
    QStringList summary;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        summary.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value()));
    }
    ProgressFields fields = phase(QStringLiteral("preparing"));
    fields.project = project;
    fields.selectedFiles = entries.size();
    fields.totalBytes = total;
    reporter.report(QStringLiteral("Selected %1 exact build object(s) for %2 (%3; %4).")
                        .arg(entries.size())
                        .arg(target, formatEventBytes(total), summary.join(QStringLiteral(", "))),
                    fields);
}

// publish_build_inputs
Json publishBuildInputs(SourceApi &api, const QString &target, const QList<BuildInput> &entries, const QString &givenProject,
                        ProgressReporter &reporter, const QString &policy, const std::optional<Json> &latestIdentity = std::nullopt)
{
    requireCanonicalRoles(entries);
    Json payloadEntries = Json::array();
    for (const BuildInput &entry : entries) {
        payloadEntries.append(entryJson(entry));
    }
    const QString project = givenProject.isEmpty() ? projectFromEntries(entries) : givenProject;
    const QString snapshot = sha256Hex(buildInputSnapshotBytes(target, entries, project, policy));
    std::optional<Json> latest;
    if (latestIdentity && Py::truthy(*latestIdentity) && Py::get(*latestIdentity, QStringLiteral("sha256")) == Json(snapshot)) {
        latest = latestIdentity;
    } else {
        latest = api.latestBuildInputs(target);
    }
    if (latest && !latest->isNull() && Py::get(*latest, QStringLiteral("sha256")) == Json(snapshot)) {
        reporter.report(QStringLiteral("The reproducible build inputs for %1 are already current.").arg(target), phase(QStringLiteral("verifying")));
        return *latest;
    }
    QSet<QString> known;
    if (latest && !latest->isNull()) {
        const Json latestEntries = Py::get(*latest, QStringLiteral("entries"));
        if (Py::truthy(latestEntries)) {
            for (const Json &item : Py::iterate(latestEntries)) {
                if (item.isObject() && item.value(QStringLiteral("sha256")).isString()) {
                    known.insert(item.value(QStringLiteral("sha256")).toString());
                }
            }
        }
    }
    QStringList digests;
    QHash<QString, QList<BuildInput>> byDigest;
    for (const BuildInput &entry : entries) {
        if (!byDigest.contains(entry.sha256)) {
            digests.append(entry.sha256);
        }
        byDigest[entry.sha256].append(entry);
    }
    for (const QString &digest : std::as_const(digests)) {
        if (known.contains(digest) || api.hasObject(digest)) {
            continue;
        }
        const QList<BuildInput> &group = byDigest[digest];
        const auto upload = std::find_if(group.cbegin(), group.cend(), [](const BuildInput &entry) {
            return !entry.reuseVerified;
        });
        if (upload == group.cend()) {
            QStringList paths;
            for (const BuildInput &entry : group) {
                paths.append(entry.path);
            }
            Py::raiseValueError(QStringLiteral("verified build input object is unavailable on the server: ") + paths.join(QStringLiteral(", ")));
        }
        std::optional<QByteArray> content = upload->frozenContent;
        if (!content && !upload->localPath.isEmpty()) {
            content = Py::readBytes(Py::Path(upload->localPath));
        }
        if (!content || sha256Hex(*content) != digest || content->size() != upload->sizeBytes) {
            Py::raiseValueError(QStringLiteral("build input changed while being staged: ") + upload->path);
        }
        ProgressFields fields = phase(QStringLiteral("uploading"));
        fields.path = upload->path;
        fields.transferredBytes = 0;
        fields.totalBytes = upload->sizeBytes;
        reporter.report(QStringLiteral("Uploading build input %1 (%2).").arg(upload->path, formatEventBytes(upload->sizeBytes)), fields);
        try {
            api.uploadObject(*content, upload->mediaType);
        } catch (const Py::Exception &error) {
            QString detail = collapseWhitespace(error.message);
            if (detail.isEmpty()) {
                detail = Py::className(error);
            }
            ProgressFields failed = fields;
            failed.state = QStringLiteral("failed");
            failed.errorCode = stableErrorCode(error, QStringLiteral("uploading"), QStringLiteral("build_input"));
            reporter.report(QStringLiteral("Uploading build input %1 failed: %2. The server build was not started.").arg(upload->path, detail), failed);
            throw;
        }
        fields.transferredBytes = upload->sizeBytes;
        reporter.report(QStringLiteral("Uploaded build input %1 (%2).").arg(upload->path, formatEventBytes(upload->sizeBytes)), fields);
    }
    const Json payload = Json::object({{QStringLiteral("schema_version"), Json(3)},
                                       {QStringLiteral("target"), Json(target)},
                                       {QStringLiteral("project"), Json(project)},
                                       {QStringLiteral("discovery_policy"), Json(policy)},
                                       {QStringLiteral("entries"), payloadEntries}});
    reporter.report(QStringLiteral("Uploading the accepted build-input inventory for %1.").arg(target), phase(QStringLiteral("uploading")));
    return api.publishBuildInputs(payload);
}

// _accepted_build_input_output
Json acceptedBuildInputOutput(const QString &target, const QString &project, const Json &snapshot, const QList<BuildInput> &staged)
{
    const Json snapshotSha = Py::get(snapshot, QStringLiteral("sha256"));
    if (!snapshotSha.isString() || !Py::isLowerHex(snapshotSha.toString(), 64)) {
        Py::raiseValueError(QStringLiteral("the server did not return a build-input snapshot identity for ") + target);
    }
    const Json acceptedTarget = Py::get(snapshot, QStringLiteral("target"));
    if (!acceptedTarget.isNull() && acceptedTarget != Json(target)) {
        Py::raiseValueError(QStringLiteral("the server returned build inputs for the wrong target: ") + target);
    }
    const Json acceptedProject = Py::get(snapshot, QStringLiteral("project"));
    if (!acceptedProject.isNull() && acceptedProject != Json(project)) {
        Py::raiseValueError(QStringLiteral("the server returned build inputs for the wrong project: ") + target);
    }
    QList<Json> accepted;
    const Json entries = Py::get(snapshot, QStringLiteral("entries"));
    if (entries.isArray()) {
        accepted = entries.items();
    } else {
        for (const BuildInput &entry : staged) {
            accepted.append(entryJson(entry));
        }
    }
    static const char *const keys[] = {"path", "sha256", "size_bytes", "media_type", "role"};
    QList<Json> publicEntries;
    for (const Json &entry : std::as_const(accepted)) {
        if (!entry.isObject()) {
            Py::raiseValueError(QStringLiteral("the server returned an incomplete build-input entry for ") + target);
        }
        Json item = Json::object();
        for (const char *key : keys) {
            if (!entry.contains(QLatin1String(key))) {
                Py::raiseValueError(QStringLiteral("the server returned an incomplete build-input entry for ") + target);
            }
            item.insert(QLatin1String(key), entry.value(QLatin1String(key)));
        }
        publicEntries.append(item);
    }
    std::stable_sort(publicEntries.begin(), publicEntries.end(), [](const Json &left, const Json &right) {
        return Py::codePointLess(left.value(QStringLiteral("path")).toString(), right.value(QStringLiteral("path")).toString());
    });
    return Json::object({{QStringLiteral("project"), Json(project)},
                         {QStringLiteral("snapshot_sha256"), snapshotSha},
                         {QStringLiteral("entries"), Json::array(publicEntries)}});
}

// _current_build_for_inputs
std::optional<Json> currentBuildForInputs(const Json &status, const QString &snapshot)
{
    const Json build = Py::get(status, QStringLiteral("build"));
    if (Py::get(status, QStringLiteral("status")) == Json(QStringLiteral("synced")) && build.isObject()
        && build.value(QStringLiteral("status")) == Json(QStringLiteral("succeeded")) && build.value(QStringLiteral("input_snapshot_sha256")) == Json(snapshot)) {
        return build;
    }
    return std::nullopt;
}

std::optional<QString> optionalValue(const ArgParse::Arguments &arguments, const QString &name)
{
    return arguments.has(name) ? std::optional<QString>(arguments.value(name)) : std::nullopt;
}

int publish(const ArgParse::Arguments &arguments, std::unique_ptr<ProgressReporter> &reporter)
{
    const Py::Path root(arguments.value(QStringLiteral("--root")));
    const Py::Path manifest(arguments.value(QStringLiteral("--manifest")));
    const Py::Path statePath(arguments.value(QStringLiteral("--state")));
    const std::optional<QString> target = optionalValue(arguments, QStringLiteral("--target"));
    const std::optional<QString> scopeTarget = optionalValue(arguments, QStringLiteral("--scope-target"));
    const std::optional<QString> project = optionalValue(arguments, QStringLiteral("--project"));
    const bool forceBuild = arguments.flag(QStringLiteral("--force-build"));
    const QString token = arguments.has(QStringLiteral("--token")) ? arguments.value(QStringLiteral("--token")) : qEnvironmentVariable("THOUGHTS_TOKEN");
    const QString targetText = target.value_or(QString());

    SourceStateLock lock(statePath);
    if (project && !project->isEmpty() && targetText.isEmpty()) {
        return ArgParse::rootError(QStringLiteral("--project requires --target"));
    }
    if (!targetText.isEmpty() && scopeTarget && !scopeTarget->isEmpty()) {
        return ArgParse::rootError(QStringLiteral("--target and --scope-target are mutually exclusive"));
    }
    if (forceBuild && targetText.isEmpty()) {
        return ArgParse::rootError(QStringLiteral("--force-build requires --target"));
    }
    // Several PDFs of one Project in one run (native extension): the sources
    // are published once, then each PDF's build inputs.
    const QStringList alsoScopeTargets = arguments.lists.value(QStringLiteral("--also-scope-target"));
    if (!alsoScopeTargets.isEmpty() && (!scopeTarget || scopeTarget->isEmpty())) {
        return ArgParse::rootError(QStringLiteral("--also-scope-target requires --scope-target"));
    }
    QStringList scopeTargets;
    if (scopeTarget && !scopeTarget->isEmpty()) {
        scopeTargets.append(*scopeTarget);
        for (const QString &extra : alsoScopeTargets) {
            if (scopeTargets.contains(extra)) {
                return ArgParse::rootError(QStringLiteral("--also-scope-target repeats a target: ") + extra);
            }
            scopeTargets.append(extra);
        }
    }
    if (token.isEmpty()) {
        return ArgParse::rootError(QStringLiteral("--token or THOUGHTS_TOKEN is required"));
    }
    SourceApi api(arguments.value(QStringLiteral("--server")), token);
    QString reporterTarget = targetText;
    if (reporterTarget.isEmpty()) {
        reporterTarget = scopeTarget.value_or(QString());
    }
    reporter = std::make_unique<ProgressReporter>(reporterTarget, root);
    ProgressReporter &progress = *reporter;
    api.setTransferProgress([&progress](qint64 uploaded, qint64 downloaded, const QString &direction) {
        ProgressFields fields = phase(direction == QLatin1String("download") ? QStringLiteral("downloading") : QStringLiteral("uploading"));
        fields.uploadedBytes = uploaded;
        fields.downloadedBytes = downloaded;
        progress.report(QStringLiteral("Transfer progress: %1 uploaded; %2 downloaded.").arg(formatEventBytes(uploaded), formatEventBytes(downloaded)), fields);
    });
    std::optional<FrozenInputs> frozen;
    if (arguments.has(QStringLiteral("--frozen-inputs"))) {
        frozen = FrozenInputs::load(Py::Path(arguments.value(QStringLiteral("--frozen-inputs"))), root);
    }
    const FrozenInputs *frozenInputs = frozen ? &*frozen : nullptr;

    progress.report(QStringLiteral("Preparing the managed source inventory."), phase(QStringLiteral("preparing")));
    qsizetype manifestCount;
    if (frozenInputs) {
        if (!Py::isFile(manifest)) {
            Py::raiseValueError(QStringLiteral("frozen source publication requires an existing managed manifest"));
        }
        manifestCount = manifestPaths(manifest).size();
    } else {
        // refresh_manifest: rewritten only when the bytes differ.
        const Json payload = sourceManifestPayload(root);
        manifestCount = manifestPayloadPathCount(payload);
        const QByteArray content = Py::dumpsFile(payload);
        if (!Py::isFile(manifest) || Py::readBytes(manifest) != content) {
            Py::atomicWrite(Py::resolve(manifest), content);
        }
    }
    progress.report(QStringLiteral("The managed source manifest contains %1 path(s).").arg(manifestCount), phase(QStringLiteral("preparing")));
    if (project && !project->isEmpty() && !frozenInputs) {
        progress.report(QStringLiteral("Ensuring project %1 is included for %2.").arg(*project, targetText), phase(QStringLiteral("preparing")));
        // ensure_project_in_manifest
        const Py::Path docsRoot = Py::resolve(root);
        validateSourceProject(docsRoot, *project, targetText, true);
        const Json payload = sourceManifestPayload(docsRoot);
        manifestPayloadPathCount(payload);
        Py::atomicWrite(Py::resolve(manifest), Py::dumpsFile(payload));
    }
    std::optional<QSet<QString>> scopedPaths;
    const QStringList requestedPaths = arguments.lists.value(QStringLiteral("--path"));
    if (!requestedPaths.isEmpty()) {
        scopedPaths = QSet<QString>(requestedPaths.cbegin(), requestedPaths.cend());
    } else if (frozenInputs) {
        const QStringList paths = frozenInputs->sourcePaths();
        scopedPaths = QSet<QString>(paths.cbegin(), paths.cend());
    }
    if (frozenInputs && scopedPaths) {
        const QStringList frozenList = frozenInputs->sourcePaths();
        QStringList unstaged;
        for (const QString &path : *scopedPaths) {
            if (!frozenList.contains(path)) {
                unstaged.append(path);
            }
        }
        if (!unstaged.isEmpty()) {
            std::sort(unstaged.begin(), unstaged.end(), Py::codePointLess);
            Py::raiseValueError(QStringLiteral("requested source paths have no frozen identity: ") + unstaged.join(QStringLiteral(", ")));
        }
    }
    // scope_target = args.scope_target or args.target
    std::optional<QString> scope = scopeTarget && !scopeTarget->isEmpty() ? scopeTarget : target;
    std::optional<QString> selectedProject;
    std::optional<Py::Path> selectedProjectRoot;
    if (scope && !scope->isEmpty()) {
        const auto [name, folder] = targetProject(Py::resolve(root), *scope, project, true);
        selectedProject = name;
        selectedProjectRoot = folder;
    }
    // The other PDFs of a multi-PDF run must belong to the same Project.
    for (qsizetype index = 1; index < scopeTargets.size(); ++index) {
        if (targetProject(Py::resolve(root), scopeTargets.at(index), project, true).first != *selectedProject) {
            Py::raiseValueError(QStringLiteral("scope targets belong to different source projects: ") + scopeTargets.at(index));
        }
    }
    if (frozenInputs) {
        QSet<QString> allowed;
        if (scope) {
            allowed.insert(*scope);
            for (const QString &extra : std::as_const(scopeTargets)) {
                allowed.insert(extra);
            }
        } else {
            for (const SourceTarget &item : registeredSourceProjectTargets(root)) {
                allowed.insert(item.target);
            }
        }
        QStringList unexpected;
        for (const QString &frozenTarget : frozenInputs->buildTargets()) {
            if (!allowed.contains(frozenTarget)) {
                unexpected.append(frozenTarget);
            }
        }
        if (!unexpected.isEmpty()) {
            std::sort(unexpected.begin(), unexpected.end(), Py::codePointLess);
            Py::raiseValueError(QStringLiteral("frozen build targets were not selected for publication: ") + unexpected.join(QStringLiteral(", ")));
        }
    }
    std::optional<QList<BuildInput>> scopedEntries;
    QMap<QString, QList<BuildInput>> extraScopedEntries;
    if (scope && !scope->isEmpty()) {
        progress.report(scopeTargets.size() > 1 ? QStringLiteral("Selecting managed sources used by %1 PDFs of %2.").arg(scopeTargets.size()).arg(*selectedProject)
                                                : QStringLiteral("Selecting managed sources used by %1.").arg(*scope),
                        phase(QStringLiteral("preparing")));
        scopedEntries = publicationBuildInputEntries(root, *scope, project, frozenInputs);
        for (qsizetype index = 1; index < scopeTargets.size(); ++index) {
            extraScopedEntries.insert(scopeTargets.at(index), publicationBuildInputEntries(root, scopeTargets.at(index), project, frozenInputs));
        }
        if (!scopedPaths) {
            QSet<QString> authored;
            const Json entries = authoredSourceEntries(Py::resolve(root), *selectedProjectRoot);
            for (const Json &entry : entries.items()) {
                authored.insert(entry.value(QStringLiteral("path")).toString());
            }
            scopedPaths = authored;
        }
        if (const std::optional<Configuration> configuration = findConfiguration(Py::resolve(root), *selectedProject)) {
            const QString configurationPath = *configuration->path.relativeTo(Py::resolve(root));
            if (!manifestPaths(manifest).contains(configurationPath)) {
                Py::raiseValueError(QStringLiteral("owning Project manifest is absent from the source inventory: ") + configurationPath);
            }
            if (!frozenInputs) {
                scopedPaths->insert(configurationPath);
            }
        }
        progress.report(QStringLiteral("Selected %1 authored source path(s) for %2.").arg(scopedPaths->size()).arg(*selectedProject),
                        phase(QStringLiteral("preparing")));
    }
    std::optional<Py::Path> bootstrap;
    if (arguments.has(QStringLiteral("--bootstrap-state"))) {
        bootstrap = Py::Path(arguments.value(QStringLiteral("--bootstrap-state")));
    }
    SourcePublisher publisher(root, manifest, statePath, api, bootstrap, scopedPaths, selectedProject, frozenInputs,
                              [&progress](const QString &message, const ProgressFields &fields) {
                                  progress.report(message, fields);
                              });
    const QStringList adopted = arguments.lists.value(QStringLiteral("--adopt-remote-base"));
    if (!adopted.isEmpty()) {
        const qint64 count = publisher.adoptRemoteBases(QSet<QString>(adopted.cbegin(), adopted.cend()));
        progress.report(QStringLiteral("Adopted %1 explicit server source base(s).").arg(count), phase(QStringLiteral("preparing")));
    }
    const SourcePublisher::Report report = publisher.publishOnce();
    // The folders' HTML pages (protocol/html-pages.md 4): a page this computer added or changed goes to
    // Fedora's list, one Fedora added is installed before the build inputs below are read.
    const HtmlPageSync pages = syncHtmlPages(api, Py::resolve(root), statePath, htmlPageFolders(publisher.paths()));
    if (pages.changed() || !pages.errors.isEmpty()) {
        progress.report(QStringLiteral("HTML pages: %1 installed, %2 sent, %3 removed, %4 need attention.")
                            .arg(pages.installed.size()).arg(pages.uploaded.size()).arg(pages.removed.size()).arg(pages.errors.size()),
                        phase(QStringLiteral("uploading")));
    }
    Json output = Json::object({{QStringLiteral("unchanged"), Json(report.unchanged)},
                                {QStringLiteral("upserted"), Json(report.upserted)},
                                {QStringLiteral("deleted"), Json(report.deleted)},
                                {QStringLiteral("source_frontier"), report.sourceFrontier},
                                {QStringLiteral("publication_id"), report.publicationId}});
    if (pages.reportable()) {
        output.insert(QStringLiteral("html_pages"), pages.toJson());
    }
    QSet<QString> processedTargets;
    if (!targetText.isEmpty()) {
        progress.report(QStringLiteral("Inspecting reproducible build inputs for %1.").arg(targetText), phase(QStringLiteral("preparing")));
        const QList<BuildInput> entries = scopedEntries ? *scopedEntries : publicationBuildInputEntries(root, targetText, project, frozenInputs);
        const QString projectName = targetProject(Py::resolve(root), targetText, project, true).first;
        reportBuildInputSelection(progress, targetText, projectName, entries);
        const QString policy = projectDiscoveryPolicy(root / projectName);
        const QString inputSnapshot = snapshotSha256(targetText, entries, projectName, policy);
        Json snapshot;
        Json checked;
        if (forceBuild) {
            snapshot = publishBuildInputs(api, targetText, entries, projectName, progress, policy);
            progress.report(QStringLiteral("Requesting the explicit server build for %1.").arg(targetText), phase(QStringLiteral("building")));
            const QString requestId = api.traceId().isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : api.traceId();
            checked = Json::object({{QStringLiteral("status"), Json(QStringLiteral("building"))},
                                    {QStringLiteral("build"),
                                     api.requestBuild(targetText, report.sourceFrontier, Py::subscript(snapshot, QStringLiteral("sha256")), requestId)}});
        } else {
            const std::optional<Json> currentBuild = currentBuildForInputs(api.renderStatus(targetText), inputSnapshot);
            if (currentBuild) {
                snapshot = Json::object({{QStringLiteral("sha256"), Json(inputSnapshot)}});
                checked = Json::object({{QStringLiteral("status"), Json(QStringLiteral("current"))}, {QStringLiteral("build"), *currentBuild}});
            } else {
                snapshot = publishBuildInputs(api, targetText, entries, projectName, progress, policy);
                progress.report(QStringLiteral("Checking whether the server needs to build %1.").arg(targetText), phase(QStringLiteral("building")));
                checked = api.requestRenderCheck(targetText);
            }
        }
        Json build = Py::get(checked, QStringLiteral("build"));
        if (Py::get(checked, QStringLiteral("status")) == Json(QStringLiteral("attention"))) {
            progress.report(QStringLiteral("Retrying the failed server build for %1.").arg(targetText), phase(QStringLiteral("building")));
            build = api.requestBuild(targetText, report.sourceFrontier, Py::subscript(snapshot, QStringLiteral("sha256")));
        }
        if (!build.isObject() || !build.value(QStringLiteral("id")).isString()) {
            Py::raiseValueError(QStringLiteral("the server's render check did not return a build identity"));
        }
        progress.report(QStringLiteral("The server accepted build %1 for %2.").arg(build.value(QStringLiteral("id")).toString(), targetText),
                        phase(QStringLiteral("building")));
        output.insert(QStringLiteral("input_snapshot_sha256"), Py::subscript(snapshot, QStringLiteral("sha256")));
        output.insert(QStringLiteral("build_id"), Py::subscript(build, QStringLiteral("id")));
        output.insert(QStringLiteral("build_status"), Py::subscript(build, QStringLiteral("status")));
        output.insert(QStringLiteral("render_check_status"), Py::subscript(checked, QStringLiteral("status")));
        output.insert(QStringLiteral("build_input_entries"),
                      Json::object({{targetText, acceptedBuildInputOutput(targetText, projectName, snapshot, entries)}}));
        processedTargets.insert(targetText);
    } else if (scopeTargets.size() > 1) {
        // One identity request for every PDF instead of one per PDF.
        const Json latestIdentities = api.latestBuildInputIdentities();
        const QString policy = projectDiscoveryPolicy(root / *selectedProject);
        Json snapshotObject = Json::object();
        Json acceptedObject = Json::object();
        QStringList ordered = scopeTargets;
        std::sort(ordered.begin(), ordered.end(), Py::codePointLess);
        for (const QString &item : std::as_const(ordered)) {
            const QList<BuildInput> &entries = item == *scopeTarget ? *scopedEntries : extraScopedEntries[item];
            reportBuildInputSelection(progress, item, *selectedProject, entries);
            const Json identity = Py::get(latestIdentities, item);
            const Json snapshot = publishBuildInputs(api, item, entries, *selectedProject, progress, policy, identity.isNull() ? std::nullopt : std::optional<Json>(identity));
            snapshotObject.insert(item, Py::subscript(snapshot, QStringLiteral("sha256")));
            acceptedObject.insert(item, acceptedBuildInputOutput(item, *selectedProject, snapshot, entries));
            processedTargets.insert(item);
        }
        output.insert(QStringLiteral("build_input_snapshots"), snapshotObject);
        output.insert(QStringLiteral("build_input_entries"), acceptedObject);
    } else if (scopeTarget && !scopeTarget->isEmpty()) {
        reportBuildInputSelection(progress, *scopeTarget, *selectedProject, *scopedEntries);
        const Json snapshot =
            publishBuildInputs(api, *scopeTarget, *scopedEntries, *selectedProject, progress, projectDiscoveryPolicy(root / *selectedProject));
        output.insert(QStringLiteral("input_snapshot_sha256"), Py::subscript(snapshot, QStringLiteral("sha256")));
        output.insert(QStringLiteral("build_input_entries"),
                      Json::object({{*scopeTarget, acceptedBuildInputOutput(*scopeTarget, *selectedProject, snapshot, *scopedEntries)}}));
        processedTargets.insert(*scopeTarget);
    } else {
        QList<SourceTarget> registered = registeredSourceProjectTargets(root);
        if (frozenInputs) {
            registered.removeIf([frozenInputs](const SourceTarget &item) {
                return !frozenInputs->hasBuildTarget(item.target);
            });
        }
        if (!registered.isEmpty()) {
            progress.report(QStringLiteral("Inspecting reproducible build inputs for %1 registered PDF(s).").arg(registered.size()),
                            phase(QStringLiteral("preparing")));
        }
        const Json latestIdentities = registered.isEmpty() ? Json::object() : api.latestBuildInputIdentities();
        QMap<QString, Json> snapshots;
        QMap<QString, Json> accepted;
        for (const SourceTarget &item : std::as_const(registered)) {
            std::optional<QList<BuildInput>> entries;
            if (frozenInputs) {
                entries = frozenInputs->buildEntries(item.target, item.project);
            }
            if (!entries) {
                entries = publicationBuildInputEntries(root, item.target, item.project, frozenInputs);
            }
            reportBuildInputSelection(progress, item.target, item.project, *entries);
            const Json identity = Py::get(latestIdentities, item.target);
            const Json snapshot = publishBuildInputs(api, item.target, *entries, item.project, progress, projectDiscoveryPolicy(root / item.project),
                                                     identity.isNull() ? std::nullopt : std::optional<Json>(identity));
            snapshots.insert(item.target, Py::subscript(snapshot, QStringLiteral("sha256")));
            accepted.insert(item.target, acceptedBuildInputOutput(item.target, item.project, snapshot, *entries));
            processedTargets.insert(item.target);
            progress.report(QStringLiteral("Prepared the reproducible build-input inventory for %1.").arg(item.target), phase(QStringLiteral("preparing")));
        }
        Json snapshotObject = Json::object();
        for (auto it = snapshots.cbegin(); it != snapshots.cend(); ++it) {
            snapshotObject.insert(it.key(), it.value());
        }
        Json acceptedObject = Json::object();
        for (auto it = accepted.cbegin(); it != accepted.cend(); ++it) {
            acceptedObject.insert(it.key(), it.value());
        }
        output.insert(QStringLiteral("build_input_snapshots"), snapshotObject);
        output.insert(QStringLiteral("build_input_entries"), acceptedObject);
    }
    if (frozenInputs) {
        QStringList unexpected;
        for (const QString &frozenTarget : frozenInputs->buildTargets()) {
            if (!processedTargets.contains(frozenTarget)) {
                unexpected.append(frozenTarget);
            }
        }
        if (!unexpected.isEmpty()) {
            std::sort(unexpected.begin(), unexpected.end(), Py::codePointLess);
            Py::raiseValueError(QStringLiteral("frozen build targets were not selected for publication: ") + unexpected.join(QStringLiteral(", ")));
        }
        output.insert(QStringLiteral("accepted_frozen_entries"), frozenInputs->acceptedEntries(scopedPaths.value_or(QSet<QString>()), processedTargets));
    }
    const auto [uploaded, downloaded] = api.transferTotals();
    output.insert(QStringLiteral("uploaded_bytes"), Json(uploaded));
    output.insert(QStringLiteral("downloaded_bytes"), Json(downloaded));
    ProgressFields totals = phase(uploaded ? QStringLiteral("uploading") : QStringLiteral("checking_server"));
    totals.uploadedBytes = uploaded;
    totals.downloadedBytes = downloaded;
    totals.uploadTotalBytes = uploaded;
    totals.downloadTotalBytes = downloaded;
    progress.report(QStringLiteral("Transfer totals: %1 uploaded; %2 downloaded.").arg(formatEventBytes(uploaded), formatEventBytes(downloaded)), totals);
    ProgressFields succeeded;
    succeeded.state = QStringLiteral("succeeded");
    progress.report(QStringLiteral("Source publication command finished successfully."), succeeded);
    Py::DumpOptions options;
    options.sortKeys = true;
    Py::printOut(Py::dumpsChecked(output, options));
    return 0;
}
}

int runPublish(const ArgParse::Arguments &arguments)
{
    std::unique_ptr<ProgressReporter> reporter;
    try {
        return publish(arguments, reporter);
    } catch (const Py::Exception &error) {
        if (reporter && !reporter->failureEmitted()) {
            reporter->failure(error);
        }
        throw;
    }
}

}
