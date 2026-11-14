// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildinputs.h"
#include "markdownlinks.h"

#include "cssreferences.h"
#include "manifestcommands.h"
#include "mediatypes.h"
#include "progress.h"
#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "pyurl.h"
#include "sha256.h"
#include "sourceinventory.h"
#include "sourceowner.h"
#include "texcommands.h"
#include "thoughtignore.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMap>

#include <algorithm>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

const QString policyV1 = QStringLiteral("tex-closure-v1");
const QString policyV2 = QStringLiteral("source-closure-v2");
const QString attachmentErrorType = QStringLiteral("thoughts_server.project_attachments.ProjectAttachmentError");

const QSet<QString> excludedParts{QStringLiteral(".cache"), QStringLiteral(".git"),     QStringLiteral(".pytest_cache"),
                                  QStringLiteral(".runtime"), QStringLiteral(".venv"),  QStringLiteral("__pycache__"),
                                  QStringLiteral("build"),    QStringLiteral("current_pdfs"), QStringLiteral("node_modules"),
                                  QStringLiteral("pdf"),      QStringLiteral("tmp"),    QStringLiteral("venv")};
const QSet<QString> sensitiveNames{QStringLiteral(".ds_store"),  QStringLiteral("desktop.ini"),      QStringLiteral("ehthumbs.db"),
                                   QStringLiteral("thumbs.db"),  QStringLiteral(".env"),             QStringLiteral(".env.local"),
                                   QStringLiteral("credentials.json"), QStringLiteral("id_ed25519"), QStringLiteral("id_rsa"),
                                   QStringLiteral("secrets.json")};
const QSet<QString> sensitiveSuffixes{QStringLiteral(".key"), QStringLiteral(".p12"), QStringLiteral(".pem")};
const QSet<QString> markdownAssetSuffixes{QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
                                          QStringLiteral(".gif"), QStringLiteral(".webp"), QStringLiteral(".svg")};
constexpr qint64 maximumHtmlBytes = 100000000;

}

bool isMarkdownAsset(const QString &path)
{
    return markdownAssetSuffixes.contains(Py::lower(Py::posixSuffix(path)));
}

bool isLatexSourceAsset(const QString &path)
{
    const QStringList parts = Py::Path(path).segments();
    if (parts.size() < 4 || parts.at(1) != QLatin1String("tex") || parts.at(2) != QLatin1String("source")) {
        return false;
    }
    for (const QString &part : parts) {
        if (part.startsWith(QLatin1Char('.'))) {
            return false;
        }
    }
    return latexAssetSuffixes.contains(Py::lower(Py::nameSuffix(parts.last())));
}

namespace
{

// validate_html_file: size, then strict UTF-8 as Python's text reader
// decodes it (64 KiB chunks, an incomplete sequence carried over).
void validateHtmlFile(const Py::Path &path)
{
    const Py::FileStatus status = Py::status(path);
    if (status.kind == Py::FileStatus::Kind::Missing) {
        Py::raiseOsError(status.error, path.str());
    }
    if (!(status.size > 0 && status.size <= maximumHtmlBytes)) {
        Py::raiseValueError(QStringLiteral("HTML files must be between 1 byte and 100 MB: ") + path.name());
    }
    const QByteArray content = Py::readBytes(path);
    constexpr qsizetype chunk = 65536;
    QByteArray carry;
    for (qsizetype offset = 0; offset < content.size(); offset += chunk) {
        const QByteArray data = carry + content.mid(offset, chunk);
        const bool final = offset + chunk >= content.size();
        QString error;
        if (Py::decodeUtf8(data, &error)) {
            carry.clear();
            continue;
        }
        if (!final && error.contains(QLatin1String("unexpected end of data"))) {
            // Keep the incomplete sequence for the next chunk.
            qsizetype start = data.size() - 1;
            while (start > 0 && (uchar(data.at(start)) & 0xC0) == 0x80) {
                --start;
            }
            if (Py::decodeUtf8(data.left(start))) {
                carry = data.mid(start);
                continue;
            }
            Py::decodeUtf8(data.left(start), &error);
        }
        Py::raise(QStringLiteral("UnicodeDecodeError"), error);
    }
}

}

// load_project_attachments(project): the Project's selected imported PDFs.
QList<ProjectAttachment> projectAttachments(const Py::Path &project)
{
    Json value;
    const auto [root, owner] = folderOwner(project);
    try {
        const std::optional<Configuration> configuration = findConfiguration(root, owner);
        if (!configuration) {
            Py::raiseValueError(QStringLiteral("Missing current Project manifest for ") + owner);
        }
        value = projection(configuration->value, owner);
    } catch (const Py::Exception &error) {
        static const QSet<QString> caught{QStringLiteral("ValueError"),         QStringLiteral("UnicodeDecodeError"),
                                          QStringLiteral("UnicodeEncodeError"), QStringLiteral("FileNotFoundError"),
                                          QStringLiteral("PermissionError"),    QStringLiteral("IsADirectoryError"),
                                          QStringLiteral("NotADirectoryError"), QStringLiteral("OSError"),
                                          Py::jsonDecodeErrorType};
        if (!caught.contains(error.type)) {
            throw;
        }
        Py::raise(attachmentErrorType, QStringLiteral("project attachment manifest is unreadable: ") + error.message);
    }
    const Json rendering = value.value(QStringLiteral("rendering"));
    QList<ProjectAttachment> attachments;
    QSet<QString> seen;
    for (const Json &document : value.value(QStringLiteral("documents")).items()) {
        if (document.value(QStringLiteral("format")).toString() != QLatin1String("pdf") || !document.value(QStringLiteral("included")).toBool()) {
            continue;
        }
        const QString source = document.value(QStringLiteral("source")).toString();
        const QString filename = Py::posixName(source);
        const QString digest = rendering.value(document.value(QStringLiteral("document_id")).toString()).value(QStringLiteral("sha256")).toString();
        const QString title = Py::Path(filename).stem();
        bool titleValid = !title.isEmpty() && Py::codePointCount(title) <= 200 && title == Py::strip(title);
        for (const QChar c : title) {
            titleValid = titleValid && c.unicode() >= 32 && c.unicode() != 127;
        }
        if (!Py::isSafePdfFilename(filename) || Py::Path(filename).name() != filename || !Py::isLowerHex(digest, 64) || !titleValid) {
            Py::raise(attachmentErrorType, QStringLiteral("project attachment manifest contains an invalid entry"));
        }
        const QString folded = Py::casefold(filename);
        if (seen.contains(folded)) {
            Py::raise(attachmentErrorType, QStringLiteral("project attachment manifest contains duplicate filename: ") + filename);
        }
        seen.insert(folded);
        // uniform_manifest.input_path(project.name, source, "pdf")
        const QString path = Py::posixNormpath(owner + QLatin1Char('/') + source);
        const QStringList parts = path.split(QLatin1Char('/'));
        if (parts.size() != 5 || parts.at(0) != QLatin1String(".thoughts") || parts.at(1) != QLatin1String("projects")
            || !Py::isLowerHex(parts.at(2), 64) || (parts.at(3) != QLatin1String("documents") && parts.at(3) != QLatin1String("default")) || !Py::isSafePdfFilename(parts.at(4))) {
            Py::raiseValueError(QStringLiteral("PDF inputs must belong to a Project's managed documents directory"));
        }
        attachments.append({filename, source});
    }
    return attachments;
}

namespace
{

QList<Py::Path> explicitExternalPdfCandidates(const Py::Path &docsRoot, const Py::Path &project)
{
    QSet<QString> keys;
    const Json authored = authoredSourceEntries(docsRoot, project);
    for (const Json &entry : authored.items()) {
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QString folded = Py::casefold(path);
        const bool markdown = folded.endsWith(QLatin1String(".md")) || folded.endsWith(QLatin1String(".markdown"));
        if (!folded.endsWith(QLatin1String(".tex")) && !markdown) {
            continue;
        }
        const Py::Path source = docsRoot / path;
        Progress::report("read_file", docsRoot, source);
        // A Markdown [text](key.pdf#anchor) is the \PdfLink{key}{anchor} of LaTeX.
        const QString text = Py::readText(source);
        for (const QString &key : markdown ? MarkdownLinks::pdfLinkKeys(text) : activePdfLinkKeys(text)) {
            // A key that cannot name a third-party PDF is no build input: the
            // renderer leaves that link inactive and reports it for its
            // document, so one bad link never blocks the Sync of the Library.
            if (key.isEmpty() || Py::Path(key).name() != key || key == QLatin1String(".") || key == QLatin1String("..")) {
                continue;
            }
            keys.insert(key);
        }
    }
    QList<Py::Path> candidates;
    QSet<QString> attachmentNames;
    for (const ProjectAttachment &attachment : projectAttachments(project)) {
        attachmentNames.insert(attachment.filename);
        if (!attachment.source.isEmpty()) {
            const Py::Path candidate = Py::resolve(project / attachment.source);
            if (!std::any_of(candidates.cbegin(), candidates.cend(), [&candidate](const Py::Path &known) {
                    return known == candidate;
                })) {
                candidates.append(candidate);
            }
        }
    }
    QStringList sortedKeys(keys.cbegin(), keys.cend());
    std::sort(sortedKeys.begin(), sortedKeys.end(), Py::codePointLess);
    for (const QString &key : std::as_const(sortedKeys)) {
        if (attachmentNames.contains(key + QStringLiteral(".pdf"))) {
            continue;
        }
        const Py::Path candidate = docsRoot / QStringLiteral("third_party") / (key + QStringLiteral(".pdf"));
        Progress::report("inspect_file", docsRoot, candidate);
        // An unavailable target (another Project's PDF, a typo) is the same
        // inactive link with a build warning as in the renderer, not a reason
        // to refuse freezing every source of the Project.
        if (!Py::isFile(candidate)) {
            continue;
        }
        candidates.append(candidate);
    }
    return candidates;
}

QList<QPair<Py::Path, QString>> supplementalContentCandidates(const Py::Path &docsRoot, const Py::Path &project)
{
    QList<QPair<Py::Path, QString>> candidates;
    const QString asset = QStringLiteral("asset");
    for (const Py::Path &tree : projectTexRoots(project)) {
        const Py::Path sourceRoot = tree / QStringLiteral("source");
        for (const Py::Path &path : Py::recursiveGlob(sourceRoot)) {
            if (Py::isFile(path) && isRealProjectPath(project, path)
                && isLatexSourceAsset(project.name() + QStringLiteral("/tex/source/") + *path.relativeTo(sourceRoot))) {
                candidates.append({path, asset});
            }
        }
    }
    for (const Py::Path &tree : {project / QStringLiteral("tex"), project / QStringLiteral("markdown")}) {
        if (!Py::isDir(tree)) {
            continue;
        }
        for (const Py::Path &path : Py::recursiveGlob(tree)) {
            if (Py::isFile(path) && isHtmlSourcePath(*path.relativeTo(docsRoot), project.name())) {
                candidates.append({path, asset});
            }
        }
    }
    const Py::Path markdownAssets = project / QStringLiteral("markdown/assets");
    if (configurationLocation(project) && Py::isDir(markdownAssets)) {
        for (const Py::Path &path : Py::recursiveGlob(markdownAssets)) {
            if (Py::isFile(path)) {
                candidates.append({path, asset});
            }
        }
    }
    if (configurationLocation(project)) {
        for (const Py::Path &path : markdownInputFiles(project)) {
            if (markdownAssetSuffixes.contains(Py::lower(path.suffix()))) {
                candidates.append({path, asset});
            }
        }
        for (const Py::Path &path : stylesheetDependencies(project)) {
            candidates.append({path, asset});
        }
    }
    for (const Py::Path &texRoot : projectTexRoots(project)) {
        for (const auto &[rootName, role] : {std::pair{"assets", "asset"}, std::pair{"generated", "generated"}}) {
            const Py::Path root = texRoot / QString::fromLatin1(rootName);
            Progress::report("scan_directory", docsRoot, root);
            if (!Py::isDir(root)) {
                continue;
            }
            // The Python loop reports the folder before each step of rglob().
            for (const Py::Path &path : Py::recursiveGlob(root)) {
                Progress::report("scan_directory", docsRoot, root);
                Progress::report("inspect_file", docsRoot, path);
                if (Py::isFile(path) && isRealProjectPath(project, path)) {
                    candidates.append({path, QString::fromLatin1(role)});
                }
            }
            Progress::report("scan_directory", docsRoot, root);
        }
    }
    if (latexFolderSources(project)) {
        // Figures and data of an entry point outside the tex/ trees.
        const QList<Py::Path> texRoots = projectTexRoots(project);
        const bool defaultFolder = isDefaultFolder(project);
        for (const Py::Path &path : latexFolderFiles(project, latexAssetSuffixes)) {
            const bool inTexTree = std::any_of(texRoots.cbegin(), texRoots.cend(), [&path](const Py::Path &tree) {
                return path.isRelativeTo(tree);
            });
            // A PDF at a default folder's root is an imported Project PDF,
            // an external_pdf input only when the Project selects it.
            const bool importedPdf = defaultFolder && path.parent() == project && Py::casefold(path.suffix()) == QLatin1String(".pdf");
            if (isRealProjectPath(project, path) && !inTexTree && !importedPdf) {
                candidates.append({path, asset});
            }
        }
    }
    for (const Py::Path &path : explicitExternalPdfCandidates(docsRoot, project)) {
        candidates.append({path, QStringLiteral("external_pdf")});
    }
    return candidates;
}

Json entryObject(const BuildInput &entry)
{
    return Json::object({{QStringLiteral("path"), Json(entry.path)},
                         {QStringLiteral("sha256"), Json(entry.sha256)},
                         {QStringLiteral("size_bytes"), Json(entry.sizeBytes)},
                         {QStringLiteral("media_type"), Json(entry.mediaType)},
                         {QStringLiteral("role"), Json(entry.role)}});
}

Json stringArray(QStringList values)
{
    std::sort(values.begin(), values.end(), Py::codePointLess);
    Json array = Json::array();
    for (const QString &value : std::as_const(values)) {
        array.append(Json(value));
    }
    return array;
}

Json buildInputInventory(const Py::Path &docsRoot, const QString &target, const Py::Path &sourceManifest, const QString &projectName,
                         const Py::Path &project, const Json &authored, const QList<BuildInput> &entries)
{
    QSet<QString> managed;
    for (const Json &entry : authored.items()) {
        managed.insert(entry.value(QStringLiteral("path")).toString());
    }
    Json payloadEntries = Json::array();
    for (const BuildInput &entry : entries) {
        payloadEntries.append(entryObject(entry));
    }
    const Py::Path sourceIndexPath = docsRoot / QStringLiteral("current_pdfs/.thoughts-source-index.json");
    const Py::Path resolvedManifest = Py::resolve(sourceManifest);
    const Py::Path resolvedIndex = Py::resolve(sourceIndexPath);
    QSet<QString> watchFiles{resolvedManifest.str(), resolvedIndex.str()};
    QSet<QString> watchDirectories{resolvedManifest.parent().str(), resolvedIndex.parent().str()};
    for (const BuildInput &entry : entries) {
        const Py::Path local = Py::resolve(Py::Path(entry.localPath));
        watchFiles.insert(local.str());
        watchDirectories.insert(local.parent().str());
    }
    watchDirectories.insert((project / QStringLiteral("tex")).str());
    for (const Json &entry : authored.items()) {
        const Py::Path local = Py::resolve(docsRoot / entry.value(QStringLiteral("path")).toString());
        watchFiles.insert(local.str());
        watchDirectories.insert(local.parent().str());
    }
    watchFiles.insert(Py::resolve(project / QStringLiteral("tex/docs_order_manifest.txt")).str());
    const QString policy = projectDiscoveryPolicy(project);
    const QByteArray snapshot = buildInputSnapshotBytes(target, entries, projectName, policy);
    return Json::object({{QStringLiteral("schema_version"), Json(3)},
                         {QStringLiteral("target"), Json(target)},
                         {QStringLiteral("project"), Json(projectName)},
                         {QStringLiteral("discovery_policy"), Json(policy)},
                         {QStringLiteral("sha256"), Json(sha256Hex(snapshot))},
                         {QStringLiteral("entries"), payloadEntries},
                         {QStringLiteral("managed_files"), stringArray(QStringList(managed.cbegin(), managed.cend()))},
                         {QStringLiteral("watch_files"), stringArray(QStringList(watchFiles.cbegin(), watchFiles.cend()))},
                         {QStringLiteral("watch_directories"), stringArray(QStringList(watchDirectories.cbegin(), watchDirectories.cend()))}});
}

QSet<QString> pathSet(const Json &authored)
{
    QSet<QString> paths;
    for (const Json &entry : authored.items()) {
        paths.insert(entry.value(QStringLiteral("path")).toString());
    }
    return paths;
}
}

bool isHtmlSourcePath(const QString &path, const QString &project)
{
    const QStringList parts = path.split(QLatin1Char('/'));
    if (!Py::isConnectedFolderName(project) || parts.size() < 3 || parts.at(0) != project
        || (parts.at(1) != QLatin1String("tex") && parts.at(1) != QLatin1String("markdown"))) {
        return false;
    }
    for (const QString &part : parts) {
        if (part.isEmpty() || part.startsWith(QLatin1Char('.'))) {
            return false;
        }
    }
    for (const QChar c : path) {
        if (c.unicode() < 32 || c == QLatin1Char('\\') || c == QLatin1Char('?') || c == QLatin1Char('#') || c == QLatin1Char('%')) {
            return false;
        }
    }
    const QString suffix = Py::lower(Py::posixSuffix(path));
    return suffix == QLatin1String(".html") || suffix == QLatin1String(".htm");
}

QList<Py::Path> stylesheetDependencies(const Py::Path &project)
{
    QMap<QString, Py::Path> found;
    const Py::Path root = Py::resolve(project);
    for (const Py::Path &stylesheet : markdownInputFiles(project)) {
        if (Py::lower(stylesheet.suffix()) != QLatin1String(".css")) {
            continue;
        }
        const QString relative = *stylesheet.relativeTo(project);
        QString text;
        try {
            text = Py::readText(stylesheet);
        } catch (const Py::Exception &) {
            continue;
        }
        for (const QString &reference : stylesheetReferences(text)) {
            const Py::SplitResult parts = Py::urlsplit(Py::strip(reference));
            if (!parts.scheme.isEmpty() || !parts.netloc.isEmpty() || parts.path.isEmpty() || parts.path.startsWith(QLatin1Char('/'))) {
                continue;
            }
            const QString target = Py::posixNormpath(Py::posixJoin(Py::posixDirname(relative), Py::unquote(parts.path)));
            if (target == QLatin1String("..") || target.startsWith(QLatin1String("../"))) {
                continue;
            }
            const Py::Path path = project / target;
            if (Py::isFile(path) && !Py::isSymlink(path) && Py::resolve(path).isRelativeTo(root)) {
                found.insert(target, path);
            }
        }
    }
    QStringList targets = found.keys();
    std::sort(targets.begin(), targets.end(), Py::codePointLess);
    QList<Py::Path> paths;
    for (const QString &target : std::as_const(targets)) {
        paths.append(found.value(target));
    }
    return paths;
}

QString projectDiscoveryPolicy(const Py::Path &project)
{
    return configurationLocation(project) ? policyV2 : policyV1;
}

QByteArray buildInputSnapshotBytes(const QString &target, const QList<BuildInput> &entries, const QString &project, const QString &discoveryPolicy)
{
    QList<BuildInput> canonical = entries;
    std::stable_sort(canonical.begin(), canonical.end(), [](const BuildInput &left, const BuildInput &right) {
        return Py::codePointLess(left.path, right.path);
    });
    QSet<QString> paths;
    for (const BuildInput &entry : std::as_const(canonical)) {
        paths.insert(entry.path);
    }
    if (paths.size() != canonical.size()) {
        Py::raiseValueError(QStringLiteral("build input snapshot contains duplicate paths"));
    }
    Json array = Json::array();
    for (const BuildInput &entry : std::as_const(canonical)) {
        array.append(entryObject(entry));
    }
    const Json payload = Json::object({{QStringLiteral("schema_version"), Json(3)},
                                       {QStringLiteral("target"), Json(target)},
                                       {QStringLiteral("project"), Json(project)},
                                       {QStringLiteral("discovery_policy"), Json(discoveryPolicy)},
                                       {QStringLiteral("entries"), array}});
    Py::DumpOptions options;
    options.sortKeys = true;
    options.separators = QPair<QString, QString>(QStringLiteral(","), QStringLiteral(":"));
    return Py::dumpsChecked(payload, options) + '\n';
}

namespace
{
int registeredTargetsScopes = 0;
qint64 registeredTargetsScans = 0;
QHash<QString, QList<SourceTarget>> registeredTargetsByRoot;
}

RegisteredSourceTargetsScope::RegisteredSourceTargetsScope()
{
    ++registeredTargetsScopes;
}

RegisteredSourceTargetsScope::~RegisteredSourceTargetsScope()
{
    if (--registeredTargetsScopes == 0) {
        registeredTargetsByRoot.clear();
    }
}

qint64 RegisteredSourceTargetsScope::scans()
{
    return registeredTargetsScans;
}

QList<SourceTarget> registeredSourceProjectTargets(const Py::Path &givenDocsRoot)
{
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    if (registeredTargetsScopes > 0) {
        const auto known = registeredTargetsByRoot.constFind(docsRoot.str());
        if (known != registeredTargetsByRoot.cend()) {
            return known.value();
        }
    }
    ++registeredTargetsScans;
    const QSet<QString> skipped = excludedProjects(docsRoot);
    QSet<QString> owners;
    for (const Py::Path &path : configurationPaths(docsRoot, false)) {
        for (const QString &owner : selectedSourceFolders(readConfiguration(path))) {
            owners.insert(owner);
        }
    }
    QStringList projects;
    for (const QString &owner : std::as_const(owners)) {
        if (!skipped.contains(owner)) {
            projects.append(owner);
        }
    }
    // sorted(key=str.casefold); ties fall back to code-point order.
    std::sort(projects.begin(), projects.end(), [](const QString &left, const QString &right) {
        const QString a = Py::casefold(left);
        const QString b = Py::casefold(right);
        return a != b ? Py::codePointLess(a, b) : Py::codePointLess(left, right);
    });
    QList<SourceTarget> targets;
    for (const QString &project : std::as_const(projects)) {
        if (!Py::isDir(docsRoot / project)) {
            continue;
        }
        const QList<ProjectDocument> documents = projectDocuments(docsRoot / project, true);
        if (!documents.isEmpty()) {
            validateSourceProject(docsRoot, project, documents.first().output, true);
        }
        for (const ProjectDocument &document : documents) {
            targets.append({project, document.output});
        }
    }
    if (registeredTargetsScopes > 0) {
        registeredTargetsByRoot.insert(docsRoot.str(), targets);
    }
    return targets;
}

QPair<QString, Py::Path> targetProject(const Py::Path &docsRoot, const QString &target, const std::optional<QString> &projectOverride,
                                       bool allowMissingDocumentInputs)
{
    QStringList projects;
    for (const SourceTarget &entry : registeredSourceProjectTargets(docsRoot)) {
        if (entry.target == target) {
            projects.append(entry.project);
        }
    }
    if (projects.size() > 1) {
        Py::raiseValueError(QStringLiteral("build target has no unique source-linked project: ") + target);
    }
    if (projectOverride) {
        const Py::Path root = validateSourceProject(docsRoot, *projectOverride, target, allowMissingDocumentInputs);
        if (!projects.isEmpty() && projects.first() != *projectOverride) {
            Py::raiseValueError(QStringLiteral("build target is linked to %1, not requested project %2").arg(projects.first(), *projectOverride));
        }
        return {*projectOverride, root};
    }
    if (projects.size() != 1) {
        Py::raiseValueError(QStringLiteral("build target has no unique source-linked project: ") + target);
    }
    return {projects.first(), validateSourceProject(docsRoot, projects.first(), target, allowMissingDocumentInputs)};
}

QList<BuildInput> projectBuildInputEntries(const Py::Path &docsRoot, const QString &projectName, const Py::Path &project,
                                           const QSet<QString> &managedPaths)
{
    const ThoughtIgnore ignored(project);
    const QString owner = folderOwner(project).second;
    QList<QPair<Py::Path, QString>> candidates = supplementalContentCandidates(docsRoot, project);
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
        const QString a = left.first.str();
        const QString b = right.first.str();
        return a != b ? Py::codePointLess(a, b) : Py::codePointLess(left.second, right.second);
    });
    const Py::Path resolvedProject = Py::resolve(project);
    QMap<QString, BuildInput> entries;
    for (const auto &[candidate, role] : std::as_const(candidates)) {
        Progress::report("inspect_file", docsRoot, candidate);
        const Py::Path logical = candidate.isAbsolute() ? candidate : docsRoot / candidate;
        const std::optional<QString> relative = logical.relativeTo(docsRoot);
        if (!relative) {
            continue;
        }
        Py::Path resolved;
        try {
            resolved = Py::resolve(logical, true);
        } catch (const Py::Exception &) {
            continue;
        }
        const QStringList parts = Py::Path(*relative).segments();
        const QString path = parts.join(QLatin1Char('/'));
        if (parts.isEmpty() || managedPaths.contains(*relative)
            || (path.size() > owner.size() + 1 && path.startsWith(owner + QLatin1Char('/')) && ignored.excludes(path.mid(owner.size() + 1)))) {
            continue;
        }
        if (std::any_of(parts.cbegin(), parts.cend(), [](const QString &part) {
                return excludedParts.contains(part);
            })) {
            continue;
        }
        const QString name = Py::casefold(resolved.name());
        if (!resolved.isRelativeTo(docsRoot) || !Py::isFile(resolved) || sensitiveNames.contains(name)
            || sensitiveSuffixes.contains(Py::casefold(resolved.suffix())) || name.startsWith(QLatin1String(".env."))) {
            continue;
        }
        Progress::report("read_file", docsRoot, candidate);
        if (isHtmlSourcePath(*relative, projectName)) {
            if (!resolved.isRelativeTo(resolvedProject)) {
                Py::raiseValueError(QStringLiteral("HTML source escapes its project: ") + *relative);
            }
            validateHtmlFile(resolved);
        }
        const QByteArray content = Py::readBytes(resolved);
        entries.insert(*relative, {*relative, sha256Hex(content), content.size(), Publisher::guessMediaType(resolved.name()), role, resolved.str()});
    }
    if (entries.size() > 10000) {
        Py::raiseValueError(QStringLiteral("build input snapshot exceeds 10000 files: ") + project.name());
    }
    QStringList paths = entries.keys();
    std::sort(paths.begin(), paths.end(), Py::codePointLess);
    QList<BuildInput> result;
    for (const QString &path : std::as_const(paths)) {
        result.append(entries.value(path));
    }
    return result;
}

Json inspectBuildInputs(const Py::Path &givenDocsRoot, const QString &target, const Py::Path &sourceManifest,
                        const std::optional<QString> &projectOverride)
{
    Progress::report("scan_directory", givenDocsRoot, givenDocsRoot / QStringLiteral("current_pdfs"));
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    const auto [projectName, project] = targetProject(docsRoot, target, projectOverride, true);
    const Json authored = authoredSourceEntries(docsRoot, project);
    const QList<BuildInput> entries = projectBuildInputEntries(docsRoot, projectName, project, pathSet(authored));
    return buildInputInventory(docsRoot, target, sourceManifest, projectName, project, authored, entries);
}

Json inspectProjectBuildInputs(const Py::Path &givenDocsRoot, const QStringList &targets, const Py::Path &sourceManifest, const std::optional<QString> &projectOverride)
{
    // Every PDF of the run asks which folder builds it; the Project files do not change meanwhile.
    const RegisteredSourceTargetsScope registry;
    Progress::report("scan_directory", givenDocsRoot, givenDocsRoot / QStringLiteral("current_pdfs"));
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    std::optional<QString> selectedName;
    Py::Path selectedProject;
    Json authored;
    QList<BuildInput> entries;
    QSet<QString> seen;
    Json inventories = Json::array();
    for (const QString &target : targets) {
        if (seen.contains(target)) {
            Py::raiseValueError(QStringLiteral("duplicate build target: ") + target);
        }
        seen.insert(target);
        const auto [projectName, project] = targetProject(docsRoot, target, projectOverride, true);
        if (!selectedName) {
            selectedName = projectName;
            selectedProject = project;
            authored = authoredSourceEntries(docsRoot, project);
            entries = projectBuildInputEntries(docsRoot, projectName, project, pathSet(authored));
        } else if (projectName != *selectedName) {
            Py::raiseValueError(QStringLiteral("build targets belong to different source projects: ") + target);
        }
        inventories.append(buildInputInventory(docsRoot, target, sourceManifest, *selectedName, selectedProject, authored, entries));
    }
    return Json::object({{QStringLiteral("schema_version"), Json(1)}, {QStringLiteral("inventories"), inventories}});
}

QSet<QString> manifestPaths(const Py::Path &sourceManifest)
{
    QSet<QString> paths;
    if (!Py::isFile(sourceManifest)) {
        return paths;
    }
    const Json value = Py::loadsText(Py::readText(sourceManifest));
    const QString unsupported = QStringLiteral("source manifest has an unsupported schema");
    if (!value.isObject()) {
        Py::raiseValueError(unsupported);
    }
    if (value.value(QStringLiteral("schema_version")) != Json(3)) {
        Py::raiseValueError(QStringLiteral("normal runtime requires source-manifest schema version three; use migrate-manifest for a legacy import"));
    }
    const Json projects = value.value(QStringLiteral("projects"));
    if (!projects.isArray()) {
        Py::raiseValueError(unsupported);
    }
    for (const Json &entry : manifestSharedFiles(value)) {
        paths.insert(entry.value(QStringLiteral("path")).toString());
    }
    for (const Json &project : projects.items()) {
        const Json files = project.isObject() ? project.value(QStringLiteral("files")) : Json();
        if (!files.isArray()) {
            Py::raiseValueError(unsupported);
        }
        for (const Json &entry : files.items()) {
            if (!entry.isObject() || !entry.value(QStringLiteral("path")).isString() || !entry.value(QStringLiteral("role")).isString()) {
                Py::raiseValueError(unsupported);
            }
            const QString path = entry.value(QStringLiteral("path")).toString();
            if (paths.contains(path)) {
                Py::raiseValueError(QStringLiteral("duplicate source-manifest path: ") + path);
            }
            paths.insert(path);
        }
    }
    return paths;
}

Json inspectLibraryBuildInputs(const Py::Path &givenDocsRoot, const Py::Path &sourceManifest)
{
    const Py::Path docsRoot = Py::resolve(givenDocsRoot);
    manifestPaths(sourceManifest);
    const Json manifest = Py::loadsText(Py::readText(sourceManifest));
    QList<QPair<QString, QString>> targets;
    QSet<QString> seen;
    for (const Json &project : Py::iterate(Py::subscript(manifest, QStringLiteral("projects")))) {
        const Json name = Py::get(project, QStringLiteral("project"));
        const Json declared = Py::get(project, QStringLiteral("targets"));
        if (!name.isString() || name.toString().isEmpty() || !declared.isArray()) {
            Py::raiseValueError(QStringLiteral("source manifest has an invalid project or target list"));
        }
        for (const Json &target : declared.items()) {
            const QString text = target.toString();
            if (!target.isString() || !text.endsWith(QLatin1String(".pdf")) || text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('\\'))
                || seen.contains(text)) {
                Py::raiseValueError(QStringLiteral("source manifest has an invalid or duplicate target"));
            }
            seen.insert(text);
            targets.append({text, name.toString()});
        }
    }
    QSet<QPair<QString, QString>> authoritative;
    for (const SourceTarget &row : registeredSourceProjectTargets(docsRoot)) {
        authoritative.insert({row.target, row.project});
    }
    if (QSet<QPair<QString, QString>>(targets.cbegin(), targets.cend()) != authoritative) {
        Py::raiseValueError(QStringLiteral("source manifest targets differ from current project configuration"));
    }
    struct Scanned {
        Py::Path project;
        Json authored;
        QList<BuildInput> entries;
    };
    QHash<QString, Scanned> projects;
    Json inventories = Json::array();
    for (const auto &[target, name] : std::as_const(targets)) {
        if (!projects.contains(name)) {
            const Py::Path project = validateSourceProject(docsRoot, name, target, true);
            const Json authored = authoredSourceEntries(docsRoot, project);
            projects.insert(name, {project, authored, projectBuildInputEntries(docsRoot, name, project, pathSet(authored))});
        }
        const Scanned &scanned = projects[name];
        inventories.append(buildInputInventory(docsRoot, target, sourceManifest, name, scanned.project, scanned.authored, scanned.entries));
    }
    return Json::object({{QStringLiteral("schema_version"), Json(1)}, {QStringLiteral("inventories"), inventories}});
}

}
