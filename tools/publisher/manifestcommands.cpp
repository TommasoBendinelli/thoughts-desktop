// SPDX-License-Identifier: GPL-2.0-or-later
#include "manifestcommands.h"

#include "progress.h"
#include "projectdocuments.h"
#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sourceinventory.h"
#include "sourceowner.h"

#include <algorithm>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

bool isValidProjectName(const QString &project)
{
    return (project.startsWith(QLatin1String("docs-")) && project != QLatin1String("docs-") && !project.contains(QLatin1Char('/'))
            && !project.contains(QLatin1Char('\\')))
        || SourceOwner::isDefaultFolder(project);
}

QStringList sortedOutputs(const QList<ProjectDocument> &documents)
{
    QStringList outputs;
    for (const ProjectDocument &document : documents) {
        outputs.append(document.output);
    }
    std::sort(outputs.begin(), outputs.end(), Py::codePointLess);
    return outputs;
}

Json stringArray(const QStringList &values)
{
    Json array = Json::array();
    for (const QString &value : values) {
        array.append(Json(value));
    }
    return array;
}

// docs_root.glob("docs-*"): one listing, matching names only.
QList<Py::Path> connectedFolderCandidates(const Py::Path &docsRoot)
{
    QList<Py::Path> result;
    const std::optional<QList<Py::DirectoryEntry>> entries = Py::scanDirectory(docsRoot);
    if (!entries) {
        return result;
    }
    for (const Py::DirectoryEntry &entry : *entries) {
#ifdef Q_OS_WIN
        const bool matches = entry.name.left(5).compare(QLatin1String("docs-"), Qt::CaseInsensitive) == 0;
#else
        const bool matches = entry.name.startsWith(QLatin1String("docs-"));
#endif
        if (matches) {
            result.append(docsRoot / entry.name);
        }
    }
    return result;
}
}

QSet<QString> excludedProjects(const Py::Path &docsRoot)
{
    const Py::Path policy = docsRoot / QStringLiteral("library/source-project-policy.json");
    Progress::report("inspect_file", docsRoot, policy);
    QSet<QString> excluded;
    if (!Py::isFile(policy)) {
        return excluded;
    }
    Progress::report("read_file", docsRoot, policy);
    const Json value = Py::loadsText(Py::readText(policy));
    const Json projects = value.isObject() ? value.value(QStringLiteral("excluded_projects")) : Json();
    bool valid = value.isObject() && value.value(QStringLiteral("schema_version")) == Json(1) && projects.isArray();
    for (const Json &project : projects.items()) {
        if (!valid) {
            break;
        }
        // Unlike a project argument, "docs-" itself passes here.
        valid = project.isString() && project.toString().startsWith(QLatin1String("docs-")) && !project.toString().contains(QLatin1Char('/'))
            && !project.toString().contains(QLatin1Char('\\'));
        if (valid) {
            excluded.insert(project.toString());
        }
    }
    if (!valid) {
        Py::raiseValueError(QStringLiteral("Library source-project policy has an unsupported schema"));
    }
    return excluded;
}

Json sourceIndex(const Py::Path &docsRoot)
{
    const Py::Path path = docsRoot / QStringLiteral("current_pdfs/.thoughts-source-index.json");
    Progress::report("inspect_file", docsRoot, path);
    if (!Py::isFile(path)) {
        return Json::object({{QStringLiteral("schema_version"), Json(1)}, {QStringLiteral("pdfs"), Json::array()}});
    }
    Progress::report("read_file", docsRoot, path);
    const Json value = Py::loadsText(Py::readText(path));
    if (!value.isObject() || !value.value(QStringLiteral("pdfs")).isArray()) {
        Py::raiseValueError(QStringLiteral("source-linked PDF index has an unsupported schema"));
    }
    return value;
}

Py::Path validateSourceProject(const Py::Path &docsRoot, const QString &project, const QString &target, bool allowMissingDocumentInputs)
{
    if (!isValidProjectName(project) || Py::Path(target).name() != target || !target.endsWith(QLatin1String(".pdf"))) {
        Py::raiseValueError(QStringLiteral("Invalid source folder or document output"));
    }
    const Py::Path logicalRoot = docsRoot / project;
    Progress::report("scan_directory", docsRoot, logicalRoot);
    const Py::Path root = Py::resolve(logicalRoot);
    if (!root.isRelativeTo(docsRoot) || !Py::isDir(root)) {
        Py::raiseValueError(QStringLiteral("source project is unavailable: ") + project);
    }
    if (!configurationLocation(root)) {
        Py::raiseValueError(QStringLiteral("Missing current Project manifest for ") + project);
    }
    if (!sortedOutputs(projectDocuments(root, allowMissingDocumentInputs)).contains(target)) {
        Py::raiseValueError(QStringLiteral("Build target is not a selected document output"));
    }
    if (excludedProjects(docsRoot).contains(project)) {
        Py::raiseValueError(QStringLiteral("source project is excluded by Library source-project policy: ") + project);
    }
    return root;
}

qsizetype generateManifest(const Py::Path &docsRoot, const Py::Path &output)
{
    Progress::report("scan_directory", docsRoot, docsRoot / QStringLiteral("current_pdfs"));
    const Json payload = sourceManifestPayload(docsRoot);
    const qsizetype count = manifestPayloadPathCount(payload);
    Py::atomicWrite(Py::resolve(output), Py::dumpsFile(payload));
    return count;
}

qsizetype refreshProjectManifest(const Py::Path &givenDocsRoot, const Py::Path &manifest, const QString &projectName, const Py::Path &output)
{
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    Progress::report("read_file", docsRoot, manifest);
    const Json value = Py::loadsText(Py::readText(manifest));
    const Json projects = value.isObject() ? value.value(QStringLiteral("projects")) : Json();
    if (!value.isObject() || value.value(QStringLiteral("schema_version")) != Json(3) || !projects.isArray()) {
        Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
    }
    if (!isValidProjectName(projectName)) {
        Py::raiseValueError(QStringLiteral("project-scoped source manifest has an invalid project"));
    }
    const Py::Path projectRoot = Py::resolve(docsRoot / projectName);
    if (!projectRoot.isRelativeTo(docsRoot) || !Py::isDir(projectRoot) || !configurationLocation(projectRoot)) {
        Py::raiseValueError(QStringLiteral("source project is unavailable: ") + projectName);
    }
    Json refreshed = Json::array();
    int matched = 0;
    QSet<QString> seenProjects;
    QSet<QString> seenTargets;
    qsizetype fileCount = 0;
    for (const Json &entry : projects.items()) {
        const Json name = entry.isObject() ? entry.value(QStringLiteral("project")) : Json();
        const Json targets = entry.isObject() ? entry.value(QStringLiteral("targets")) : Json();
        const Json files = entry.isObject() ? entry.value(QStringLiteral("files")) : Json();
        bool valid = name.isString() && (name.toString().startsWith(QLatin1String("docs-")) || SourceOwner::isDefaultFolder(name.toString()))
            && !seenProjects.contains(name.toString()) && targets.isArray();
        for (const Json &target : targets.items()) {
            if (!valid) {
                break;
            }
            const QString text = target.toString();
            valid = target.isString() && Py::casefold(text).endsWith(QLatin1String(".pdf")) && !text.contains(QLatin1Char('/'))
                && !text.contains(QLatin1Char('\\')) && !seenTargets.contains(text);
        }
        if (!valid || !files.isArray()) {
            Py::raiseValueError(QStringLiteral("source manifest contains an invalid project entry"));
        }
        seenProjects.insert(name.toString());
        for (const Json &target : targets.items()) {
            seenTargets.insert(target.toString());
        }
        Json replacement = entry;
        if (name.toString() == projectName) {
            ++matched;
            replacement.insert(QStringLiteral("targets"), stringArray(sortedOutputs(projectDocuments(projectRoot, true))));
            replacement.insert(QStringLiteral("files"), authoredSourceEntries(docsRoot, projectRoot));
        }
        fileCount += replacement.value(QStringLiteral("files")).size();
        refreshed.append(replacement);
    }
    if (matched != 1) {
        Py::raiseValueError(QStringLiteral("project-scoped source manifest has no unique project"));
    }
    Json payload = value;
    payload.insert(QStringLiteral("projects"), refreshed);
    const qsizetype shared = manifestSharedFiles(payload).size();
    const qsizetype count = manifestPayloadPathCount(payload);
    if (count != shared + fileCount) {
        Py::raiseValueError(QStringLiteral("source manifest contains duplicate managed paths"));
    }
    Py::atomicWrite(Py::resolve(output), Py::dumpsFile(payload));
    return count;
}

Json migrateSourceManifest(const Py::Path &docsRoot, const Py::Path &manifest)
{
    const Json value = Py::loadsText(Py::readText(manifest));
    if (!value.isObject()) {
        Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
    }
    const Json previous = value.value(QStringLiteral("schema_version"));
    if (previous == Json(3)) {
        return Json::object({{QStringLiteral("migrated"), Json(false)},
                             {QStringLiteral("from_schema_version"), Json(3)},
                             {QStringLiteral("to_schema_version"), Json(3)},
                             {QStringLiteral("managed_paths"), Json(qint64(manifestPayloadPathCount(value)))}});
    }
    if (previous == Json(1)) {
        const Json files = value.value(QStringLiteral("files"));
        const bool valid = files.isArray() && std::all_of(files.items().cbegin(), files.items().cend(), [](const Json &path) {
                               return path.isString();
                           });
        if (!valid) {
            Py::raiseValueError(QStringLiteral("legacy source manifest has an invalid flat file list"));
        }
    } else if (previous == Json(2)) {
        const Json projects = value.value(QStringLiteral("projects"));
        const bool valid = projects.isArray() && std::all_of(projects.items().cbegin(), projects.items().cend(), [](const Json &project) {
                               return project.isObject() && project.value(QStringLiteral("project")).isString()
                                   && project.value(QStringLiteral("targets")).isArray() && project.value(QStringLiteral("files")).isArray();
                           });
        if (!valid) {
            Py::raiseValueError(QStringLiteral("legacy source manifest has an invalid project inventory"));
        }
    } else {
        Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
    }
    const Json payload = sourceManifestPayload(docsRoot);
    const qsizetype count = manifestPayloadPathCount(payload);
    Py::atomicWrite(Py::resolve(manifest), Py::dumpsFile(payload));
    return Json::object({{QStringLiteral("migrated"), Json(true)},
                         {QStringLiteral("from_schema_version"), previous},
                         {QStringLiteral("to_schema_version"), Json(3)},
                         {QStringLiteral("managed_paths"), Json(qint64(count))}});
}

Json sourceProjectCandidates(const Py::Path &givenDocsRoot)
{
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    QSet<QString> registered;
    const Json index = sourceIndex(docsRoot);
    for (const Json &entry : index.value(QStringLiteral("pdfs")).items()) {
        if (entry.isObject() && entry.value(QStringLiteral("project")).isString()) {
            registered.insert(entry.value(QStringLiteral("project")).toString());
        }
    }
    const QSet<QString> skipped = excludedProjects(docsRoot);
    QList<Py::Path> roots = connectedFolderCandidates(docsRoot);
    std::stable_sort(roots.begin(), roots.end(), [](const Py::Path &left, const Py::Path &right) {
        return Py::codePointLess(Py::casefold(left.name()), Py::casefold(right.name()));
    });
    Json candidates = Json::array();
    for (const Py::Path &root : std::as_const(roots)) {
        const QString project = root.name();
        if (registered.contains(project) || skipped.contains(project) || !Py::isDir(root) || !configurationLocation(root)) {
            continue;
        }
        for (const ProjectDocument &document : projectDocuments(root)) {
            validateSourceProject(docsRoot, project, document.output);
            candidates.append(Json::object({{QStringLiteral("project"), Json(project)}, {QStringLiteral("target"), Json(document.output)}}));
        }
    }
    return candidates;
}

}
