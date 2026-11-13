// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourceinventory.h"

#include "progress.h"
#include "pyerror.h"
#include "pyops.h"
#include "pystr.h"
#include "sourceowner.h"
#include "thoughtignore.h"

#include <QMap>
#include <QSet>

#include <algorithm>

namespace Thoughts::Publisher
{
namespace
{
const QSet<QString> authoredSourceExtensions{QStringLiteral(".bib"), QStringLiteral(".bst"), QStringLiteral(".cfg"),
                                             QStringLiteral(".cls"), QStringLiteral(".def"), QStringLiteral(".ini"),
                                             QStringLiteral(".sty"), QStringLiteral(".tex"), QStringLiteral(".txt")};
const QSet<QString> markdownTextSuffixes{QStringLiteral(".md"), QStringLiteral(".markdown"), QStringLiteral(".css"), QStringLiteral(".txt")};
const QSet<QString> markdownAssetSuffixes{QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
                                          QStringLiteral(".gif"), QStringLiteral(".webp"), QStringLiteral(".svg")};
const QSet<QString> legacySettings{QStringLiteral("tex/support/thoughts_document_order.txt"),
                                   QStringLiteral("tex/support/thoughts_document_options.txt"),
                                   QStringLiteral("tex/support/thoughts_project_attachments.txt"),
                                   QStringLiteral("tex/docs_order_manifest.txt")};

bool containsPath(const QList<Py::Path> &paths, const Py::Path &path)
{
    return std::any_of(paths.cbegin(), paths.cend(), [&path](const Py::Path &candidate) {
        return candidate == path;
    });
}

// Whether a default folder holds anything besides its configuration and
// imported PDFs, decided from one listing without reporting a scan.
bool defaultFolderHasSources(const Py::Path &folder)
{
    const std::optional<QList<Py::DirectoryEntry>> entries = Py::scanDirectory(folder);
    if (!entries) {
        return false;
    }
    for (const Py::DirectoryEntry &entry : *entries) {
        if (!entry.isDirectory && (entry.name == QLatin1String("thoughts-project.json") || Py::casefold(Py::nameSuffix(entry.name)) == QLatin1String(".pdf"))) {
            continue;
        }
        return true;
    }
    return false;
}

bool isUtf8Text(const Py::Path &path)
{
    try {
        return Py::decodeUtf8(Py::readBytes(path)).has_value();
    } catch (const Py::Exception &) {
        return false;
    }
}
}

bool isRealProjectPath(const Py::Path &project, const Py::Path &path)
{
    try {
        const std::optional<QString> relative = path.relativeTo(project);
        if (!relative || Py::isSymlink(project) || !Py::resolve(path).isRelativeTo(Py::resolve(project))) {
            return false;
        }
        const QStringList parts = Py::Path(*relative).segments();
        Py::Path prefix = project;
        for (const QString &part : parts) {
            prefix = prefix / part;
            if (Py::isSymlink(prefix)) {
                return false;
            }
        }
        return true;
    } catch (const Py::Exception &) {
        return false;
    }
}

QList<Py::Path> projectTexRoots(const Py::Path &project)
{
    QList<Py::Path> roots{project / QStringLiteral("tex")};
    const auto [root, owner] = folderOwner(project);
    if (const std::optional<Configuration> configuration = findConfiguration(root, owner)) {
        const Json value = projection(configuration->value, owner);
        for (const Json &document : value.value(QStringLiteral("documents")).items()) {
            if (document.value(QStringLiteral("format")).toString() != QLatin1String("latex")) {
                continue;
            }
            const Json options = value.value(QStringLiteral("rendering")).value(document.value(QStringLiteral("document_id")).toString());
            const QString directory = latexWorkingDirectory(owner, document.value(QStringLiteral("source")).toString(), options);
            const Py::Path root = project / directory / QStringLiteral("tex");
            if (!containsPath(roots, root)) {
                roots.append(root);
            }
        }
    }
    QList<Py::Path> result;
    for (const Py::Path &root : std::as_const(roots)) {
        if (isRealProjectPath(project, root)) {
            result.append(root);
        }
    }
    std::stable_sort(result.begin(), result.end(), Py::Path::less);
    return result;
}

bool latexFolderInput(const Py::Path &project, const QString &relative)
{
    if (!latexFolderPath(relative)) {
        return false;
    }
    for (const Py::Path &tree : projectTexRoots(project)) {
        const QString root = *tree.relativeTo(project);
        if (relative == root || relative.startsWith(root + QLatin1Char('/'))) {
            return false;
        }
    }
    return latexFolderSources(project);
}

bool isAuthoredSourcePath(const Py::Path &project, const QString &relative)
{
    if (relative.isEmpty() || relative.contains(QLatin1Char('\\')) || relative.startsWith(QLatin1Char('/')) || legacySettings.contains(relative)) {
        return false;
    }
    const QStringList parts = relative.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            return false;
        }
    }
    const ThoughtIgnore ignored(project);
    if (ignored.excludes(relative)) {
        return false;
    }
    for (qsizetype index = 1; index < parts.size(); ++index) {
        if (ignored.excludes(parts.mid(0, index).join(QLatin1Char('/')), true)) {
            return false;
        }
    }
    if (relative == QLatin1String(".ThoughtIgnore") || relative == QLatin1String("tex/docs_order_manifest.txt")) {
        return true;
    }
    const QString suffix = Py::casefold(Py::posixSuffix(relative));
    // Discovery lists a default folder's Markdown documents and their text
    // inputs anywhere in it.
    if (isDefaultFolder(project) && markdownTextSuffixes.contains(suffix)) {
        return true;
    }
    QList<QStringList> trees{{QStringLiteral("markdown")}};
    for (const Py::Path &root : projectTexRoots(project)) {
        trees.append(root.relativeTo(project)->split(QLatin1Char('/')));
    }
    static const QStringList roots{QStringLiteral("source"), QStringLiteral("fragments"), QStringLiteral("support")};
    for (const QStringList &tree : std::as_const(trees)) {
        const QStringList inner = parts.mid(tree.size());
        if (parts.mid(0, tree.size()) == tree && inner.size() >= 2 && roots.contains(inner.first())) {
            return authoredSourceExtensions.contains(suffix) || markdownTextSuffixes.contains(suffix);
        }
    }
    return authoredSourceExtensions.contains(suffix) && latexFolderInput(project, relative);
}

Json authoredSourceEntries(const Py::Path &docsRoot, const Py::Path &project)
{
    const ThoughtIgnore ignored(project);
    QList<QPair<Py::Path, QString>> candidates{{project / QStringLiteral("tex/docs_order_manifest.txt"), QStringLiteral("render_manifest")},
                                               {project / QStringLiteral(".ThoughtIgnore"), QStringLiteral("source")}};
    const Py::Path explicitConfiguration = project / QStringLiteral("thoughts-project.json");
    bool explicitListed = false;
    // A default folder's thoughts-project.json is the Project configuration,
    // which the manifest lists once as a shared file.
    const bool defaultFolder = isDefaultFolder(project);
    if (Py::isFile(explicitConfiguration) && !defaultFolder) {
        const std::optional<Py::Path> location = configurationLocation(project);
        if (location && *location == explicitConfiguration) {
            candidates.append({explicitConfiguration, QStringLiteral("render_manifest")});
            explicitListed = true;
        }
    }
    QList<Py::Path> trees{project / QStringLiteral("markdown")};
    trees.append(projectTexRoots(project));
    static const QStringList rootNames{QStringLiteral("fragments"), QStringLiteral("source"), QStringLiteral("support")};
    for (const Py::Path &tree : std::as_const(trees)) {
        for (const QString &rootName : rootNames) {
            const Py::Path sourceRoot = tree / rootName;
            Progress::report("scan_directory", docsRoot, sourceRoot);
            if (!Py::isDir(sourceRoot) || !isRealProjectPath(project, sourceRoot) || ignored.excludes(*sourceRoot.relativeTo(project), true)) {
                continue;
            }
            const QString role = rootName == QLatin1String("source") ? QStringLiteral("source")
                                                                    : (rootName == QLatin1String("fragments") ? QStringLiteral("fragment") : rootName);
            Py::walk(sourceRoot, [&](Py::WalkStep &step) {
                Progress::report("scan_directory", docsRoot, step.directory);
                QStringList kept;
                for (const QString &name : std::as_const(step.subdirectories)) {
                    const Py::Path child = step.directory / name;
                    if (!Py::isSymlink(child) && !ignored.excludes(*child.relativeTo(project), true)) {
                        kept.append(name);
                    }
                }
                step.subdirectories = kept;
                for (const QString &name : std::as_const(step.files)) {
                    const Py::Path path = step.directory / name;
                    Progress::report("inspect_file", docsRoot, path);
                    if (Py::isFile(path) && !Py::isSymlink(path) && !ignored.excludes(*path.relativeTo(project))) {
                        candidates.append({path, role});
                    }
                }
            });
        }
    }
    QList<Py::Path> known;
    for (const auto &candidate : std::as_const(candidates)) {
        known.append(candidate.first);
    }
    for (const Py::Path &path : markdownInputFiles(project)) {
        if (!markdownAssetSuffixes.contains(Py::lower(path.suffix())) && !containsPath(known, path)) {
            candidates.append({path, QStringLiteral("source")});
        }
    }
    if (latexFolderSources(project)) {
        // An entry point outside the tex/ trees may input any file of its folder.
        known.clear();
        for (const auto &candidate : std::as_const(candidates)) {
            known.append(candidate.first);
        }
        const QList<Py::Path> texRoots = projectTexRoots(project);
        for (const Py::Path &path : latexFolderFiles(project, authoredSourceExtensions)) {
            const bool inTexTree = std::any_of(texRoots.cbegin(), texRoots.cend(), [&path](const Py::Path &tree) {
                return path.isRelativeTo(tree);
            });
            if (!containsPath(known, path) && !inTexTree) {
                candidates.append({path, QStringLiteral("source")});
            }
        }
    }
    const Py::Path resolvedProject = Py::resolve(project);
    QMap<QString, QString> roles;
    for (const auto &[candidate, role] : std::as_const(candidates)) {
        const QString projectRelative = *candidate.relativeTo(project);
        if (legacySettings.contains(projectRelative) || ignored.excludes(projectRelative)
            || (defaultFolder && projectRelative == QLatin1String("thoughts-project.json"))) {
            continue;
        }
        Progress::report("inspect_file", docsRoot, candidate);
        const QString suffix = Py::casefold(candidate.suffix());
        bool allowed;
        if (candidate.name() == QLatin1String(".ThoughtIgnore")) {
            allowed = suffix.isEmpty();
        } else if (explicitListed && candidate == explicitConfiguration) {
            allowed = suffix == QLatin1String(".json");
        } else if (markdownTextSuffixes.contains(suffix)) {
            allowed = true;
        } else {
            allowed = authoredSourceExtensions.contains(suffix);
        }
        if (!Py::isFile(candidate) || !allowed) {
            continue;
        }
        Py::Path resolved;
        std::optional<QString> relative;
        try {
            resolved = Py::resolve(candidate, true);
            relative = candidate.relativeTo(docsRoot);
        } catch (const Py::Exception &) {
            continue;
        }
        if (!relative || !resolved.isRelativeTo(resolvedProject)) {
            continue;
        }
        const Py::FileStatus status = Py::status(resolved);
        if (status.kind == Py::FileStatus::Kind::Missing) {
            Py::raiseOsError(status.error, resolved.str());
        }
        if (status.size > maximumManagedSourceBytes) {
            continue;
        }
        Progress::report("read_file", docsRoot, candidate);
        if (!isUtf8Text(resolved)) {
            continue;
        }
        roles.insert(*relative, role);
    }
    // sorted() by code point; QMap orders by UTF-16 unit.
    QStringList paths = roles.keys();
    std::sort(paths.begin(), paths.end(), Py::codePointLess);
    Json entries = Json::array();
    for (const QString &path : std::as_const(paths)) {
        entries.append(Json::object({{QStringLiteral("path"), Json(path)}, {QStringLiteral("role"), Json(roles.value(path))}}));
    }
    return entries;
}

namespace
{
QSet<QString> activeSourceOwners(const Py::Path &docsRoot, const QList<Configuration> &configurations)
{
    QSet<QString> activeOwners;
    for (const Configuration &configuration : configurations) {
        const Json defaultId = configuration.value.value(QStringLiteral("default_folder_id"));
        const QStringList selected = selectedSourceFolders(configuration.value);
        for (const Json &folder : configuration.value.value(QStringLiteral("source_folders")).items()) {
            const Json path = folder.value(QStringLiteral("path"));
            if (folder.value(QStringLiteral("folder_id")) != defaultId && path.isString() && Py::isConnectedFolderName(path.toString())) {
                activeOwners.insert(path.toString());
            }
            // The default folder holds Markdown and LaTeX documents too. One
            // the Project declares a document in is active before its file
            // exists here: a note another device added to a folder of
            // imported PDFs installs only into an active folder.
            if (folder.value(QStringLiteral("folder_id")) == defaultId && path.isString() && SourceOwner::isDefaultFolder(path.toString())
                && (selected.contains(path.toString()) || defaultFolderHasSources(docsRoot / path.toString()))) {
                activeOwners.insert(path.toString());
            }
        }
    }
    return activeOwners;
}

QList<Configuration> readConfigurations(const Py::Path &docsRoot)
{
    QList<Configuration> configurations;
    for (const Py::Path &path : configurationPaths(docsRoot)) {
        configurations.append({path, readConfiguration(path)});
    }
    return configurations;
}
}

QSet<QString> sourceOwners(const Py::Path &docsRoot)
{
    const Py::Path root = Py::resolve(docsRoot);
    return activeSourceOwners(root, readConfigurations(root));
}

bool isManagedSourcePath(const Py::Path &docsRoot, const QSet<QString> &owners, const QString &path)
{
    const QString owner = SourceOwner::of(path);
    return !owner.isEmpty() && owners.contains(owner) && isAuthoredSourcePath(docsRoot / owner, SourceOwner::relative(path));
}

Json sourceManifestPayload(const Py::Path &givenDocsRoot)
{
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    const QList<Configuration> configurations = readConfigurations(docsRoot);
    const QSet<QString> activeOwners = activeSourceOwners(docsRoot, configurations);
    QStringList owners(activeOwners.cbegin(), activeOwners.cend());
    std::sort(owners.begin(), owners.end(), Py::codePointLess);
    QList<QPair<QString, QStringList>> targetsByProject;
    for (const QString &owner : std::as_const(owners)) {
        QSet<QString> outputs;
        for (const ProjectDocument &document : projectDocuments(docsRoot / owner, true)) {
            outputs.insert(document.output);
        }
        QStringList targets(outputs.cbegin(), outputs.cend());
        std::sort(targets.begin(), targets.end(), Py::codePointLess);
        targetsByProject.append({owner, targets});
    }
    Json projects = Json::array();
    for (const auto &[name, targets] : std::as_const(targetsByProject)) {
        const Py::Path project = docsRoot / name;
        Progress::report("scan_directory", docsRoot, project);
        if (!Py::isDir(project)) {
            continue;
        }
        Json targetList = Json::array();
        for (const QString &target : targets) {
            targetList.append(Json(target));
        }
        const Json files = authoredSourceEntries(docsRoot, project);
        // A default folder holding only its configuration and imported PDFs
        // has nothing of its own to publish.
        if (SourceOwner::isDefaultFolder(name) && targets.isEmpty() && files.size() == 0) {
            continue;
        }
        projects.append(Json::object({{QStringLiteral("project"), Json(name)},
                                      {QStringLiteral("targets"), targetList},
                                      {QStringLiteral("files"), files}}));
    }
    Json payload = Json::object({{QStringLiteral("schema_version"), Json(3)}, {QStringLiteral("projects"), projects}});
    Json shared = Json::array();
    for (const Configuration &configuration : std::as_const(configurations)) {
        shared.append(Json::object({{QStringLiteral("path"), Json(*configuration.path.relativeTo(docsRoot))},
                                    {QStringLiteral("role"), Json(QStringLiteral("render_manifest"))}}));
    }
    if (shared.size() > 0) {
        payload.insert(QStringLiteral("shared_files"), shared);
    }
    return payload;
}

QList<Json> manifestSharedFiles(const Json &payload)
{
    const Json entries = Py::get(payload, QStringLiteral("shared_files"), Json::array());
    const auto fail = [] {
        Py::raiseValueError(QStringLiteral("source manifest contains retired workspace sources or unmanaged shared sources; regenerate it"));
    };
    if (!entries.isArray()) {
        fail();
    }
    for (const Json &entry : entries.items()) {
        if (!entry.isObject()) {
            fail();
        }
        const QString path = Py::requireString(entry.contains(QStringLiteral("path")) ? entry.value(QStringLiteral("path")) : Json(QString()));
        if (!isManifestPath(path) || entry.value(QStringLiteral("role")) != Json(QStringLiteral("render_manifest"))) {
            fail();
        }
    }
    return entries.items();
}

QList<Json> manifestPayloadPaths(const Json &payload)
{
    if (Py::get(payload, QStringLiteral("schema_version")) != Json(3)) {
        Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
    }
    QList<Json> values;
    QSet<QString> seen;
    const auto add = [&values, &seen](const Json &path) {
        const QString key = Py::hashKey(path);
        if (!seen.contains(key)) {
            seen.insert(key);
            values.append(path);
        }
    };
    for (const Json &entry : manifestSharedFiles(payload)) {
        add(entry.value(QStringLiteral("path")));
    }
    for (const Json &project : Py::iterate(Py::get(payload, QStringLiteral("projects"), Json::array()))) {
        for (const Json &entry : Py::iterate(Py::get(project, QStringLiteral("files"), Json::array()))) {
            add(Py::subscript(entry, QStringLiteral("path")));
        }
    }
    return values;
}

qsizetype manifestPayloadPathCount(const Json &payload)
{
    return manifestPayloadPaths(payload).size();
}

}
