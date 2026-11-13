// SPDX-License-Identifier: GPL-2.0-or-later
#include "frozeninputs.h"

#include "projectconfig.h"
#include "sourceowner.h"
#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "sha256.h"
#include "sourceinventory.h"
#include "thoughtignore.h"

#include <QHash>

#include <algorithm>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

const QSet<QString> buildObjectRoles{QStringLiteral("asset"), QStringLiteral("generated"), QStringLiteral("external_pdf")};

// _relative_path(value, label)
QString relativePath(const Json &value, const QString &label)
{
    if (!value.isString() || value.toString().isEmpty()) {
        Py::raiseValueError(label + QStringLiteral(" must be a non-empty relative path"));
    }
    const QString text = value.toString();
    bool safe = !text.startsWith(QLatin1Char('/')) && !text.contains(QLatin1Char('\\')) && !text.contains(QChar(0));
    for (const QString &part : text.split(QLatin1Char('/'))) {
        safe = safe && !part.isEmpty() && part != QLatin1String(".") && part != QLatin1String("..");
    }
    if (!safe) {
        Py::raiseValueError(QStringLiteral("unsafe %1: %2").arg(label, Py::repr(text)));
    }
    return text;
}

// FrozenPublicationInputs._target(value)
QString frozenTarget(const QString &value)
{
    if (Py::Path(value).name() != value || value.contains(QLatin1Char('\\')) || value.contains(QChar(0))
        || !Py::casefold(value).endsWith(QLatin1String(".pdf")) || value == QLatin1String(".pdf") || value == QLatin1String("..pdf")) {
        Py::raiseValueError(QStringLiteral("unsafe frozen build target: ") + Py::repr(value));
    }
    return value;
}

bool isManagedPdfPath(const QString &path)
{
    const QStringList parts = Py::Path(path).segments();
    return parts.size() == 5 && parts.at(0) == QLatin1String(".thoughts") && parts.at(1) == QLatin1String("projects") && Py::isLowerHex(parts.at(2), 64)
        && (parts.at(3) == QLatin1String("documents") || parts.at(3) == QLatin1String("default")) && Py::isSafePdfFilename(parts.at(4));
}

// _valid_source_path(root, path)
bool validSourcePath(const Py::Path &root, const QString &path)
{
    static const QSet<QString> textSuffixes{QStringLiteral(".md"), QStringLiteral(".markdown"), QStringLiteral(".css"), QStringLiteral(".txt")};
    static const QSet<QString> authoredExtensions{QStringLiteral(".bib"), QStringLiteral(".bst"), QStringLiteral(".cfg"),
                                                  QStringLiteral(".cls"), QStringLiteral(".def"), QStringLiteral(".ini"),
                                                  QStringLiteral(".sty"), QStringLiteral(".tex"), QStringLiteral(".txt")};
    const Py::Path resolvedRoot = Py::resolve(root);
    if (isManifestPath(path)) {
        const Py::Path candidate = root / path;
        return Py::isFile(candidate) && !Py::isSymlink(candidate) && Py::resolve(candidate).isRelativeTo(resolvedRoot);
    }
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // The owning folder: a docs-* folder or a Project's default folder.
    QString owner;
    if (!parts.isEmpty() && parts.first().startsWith(QLatin1String("docs-"))) {
        owner = parts.first();
    } else if (SourceOwner::isDefaultFolder(SourceOwner::of(path))) {
        owner = SourceOwner::of(path);
    }
    if (owner.isEmpty()) {
        return false;
    }
    const QStringList inner = path.mid(owner.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    Py::Path project;
    try {
        project = Py::resolve(root / owner, true);
    } catch (const Py::Exception &error) {
        if (Py::isOsError(error)) {
            return false;
        }
        throw;
    }
    if (!Py::isDir(project) || !project.isRelativeTo(resolvedRoot)) {
        return false;
    }
    if (inner.size() == 1 && (inner.at(0) == QLatin1String("thoughts-project.json") || inner.at(0) == QLatin1String(".ThoughtIgnore"))) {
        return true;
    }
    const QString suffix = Py::casefold(Py::posixSuffix(path));
    if (inner.size() >= 1 && textSuffixes.contains(suffix)) {
        return !parts.contains(QStringLiteral("..")) && !path.contains(QLatin1Char('\\')) && Py::resolve(root / path).isRelativeTo(project)
            && !ThoughtIgnore(project).excludes(inner.join(QLatin1Char('/')));
    }
    if (inner.size() == 2 && inner.at(0) == QLatin1String("tex") && inner.at(1) == QLatin1String("docs_order_manifest.txt")) {
        return true;
    }
    if (inner.size() >= 1 && authoredExtensions.contains(suffix)) {
        const QString relative = inner.join(QLatin1Char('/'));
        if (Py::resolve(root / path).isRelativeTo(project) && !ThoughtIgnore(project).excludes(relative) && latexFolderInput(project, relative)) {
            return true;
        }
    }
    return inner.size() >= 3 && inner.at(0) == QLatin1String("tex")
        && (inner.at(1) == QLatin1String("source") || inner.at(1) == QLatin1String("fragments") || inner.at(1) == QLatin1String("support"))
        && authoredExtensions.contains(suffix);
}
}

Json FrozenEntry::publicIdentity(const QString &kind, const std::optional<QString> &target) const
{
    Json value = Json::object({{QStringLiteral("kind"), Json(kind)}, {QStringLiteral("path"), Json(path)}, {QStringLiteral("action"), Json(action)}});
    if (target) {
        value.insert(QStringLiteral("target"), Json(*target));
    }
    if (kind == QLatin1String("source") && baseSha256) {
        value.insert(QStringLiteral("base_sha256"), Json(*baseSha256));
    }
    if (action == QLatin1String("upsert")) {
        value.insert(QStringLiteral("sha256"), sha256 ? Json(*sha256) : Json());
        value.insert(QStringLiteral("size_bytes"), sizeBytes ? Json(*sizeBytes) : Json());
        if (mediaType) {
            value.insert(QStringLiteral("media_type"), Json(*mediaType));
        }
        if (reuseVerified) {
            value.insert(QStringLiteral("reuse_verified"), Json(true));
        }
    }
    if (role) {
        value.insert(QStringLiteral("role"), Json(*role));
    }
    return value;
}

FrozenInputs FrozenInputs::load(const Py::Path &path, const Py::Path &sourceRoot)
{
    Py::Path planPath;
    Json payload;
    try {
        planPath = Py::resolve(path, true);
        Py::LoadOptions options;
        options.rejectDuplicateKeys = true;
        options.duplicateKeyMessage = QStringLiteral("duplicate frozen publication field: %1");
        payload = Py::loadsText(Py::readText(planPath), options);
    } catch (const Py::Exception &error) {
        if (Py::isOsError(error) || error.type == QLatin1String("UnicodeDecodeError") || error.type == Py::jsonDecodeErrorType) {
            Py::raiseValueError(QStringLiteral("frozen publication inputs are unavailable: ") + error.message);
        }
        throw;
    }
    const Json schema = payload.isObject() ? payload.value(QStringLiteral("schema_version")) : Json();
    const QString schemaKey = Py::hashKey(schema);
    if (schemaKey != QLatin1String("n:1") && schemaKey != QLatin1String("n:2")) {
        Py::raiseValueError(QStringLiteral("frozen publication inputs have an unsupported schema"));
    }
    const bool schemaTwo = schemaKey == QLatin1String("n:2");
    const Json declaredRoot = payload.value(QStringLiteral("source_root"));
    if (!declaredRoot.isString() || Py::resolve(Py::Path(declaredRoot.toString())) != Py::resolve(sourceRoot)) {
        Py::raiseValueError(QStringLiteral("frozen publication inputs belong to a different source root"));
    }
    const QString objectsDir = relativePath(payload.value(QStringLiteral("objects_dir")), QStringLiteral("frozen objects directory"));
    const Py::Path bundleRoot = Py::resolve(planPath.parent());
    Py::Path objectsRoot;
    try {
        objectsRoot = Py::resolve(bundleRoot / objectsDir, true);
    } catch (const Py::Exception &error) {
        if (!Py::isOsError(error)) {
            throw;
        }
        Py::raiseValueError(QStringLiteral("frozen objects directory is unavailable"));
    }
    if (!objectsRoot.isRelativeTo(bundleRoot) || !Py::isDir(objectsRoot)) {
        Py::raiseValueError(QStringLiteral("frozen objects directory escapes its publication bundle"));
    }
    QHash<QString, QByteArray> objectCache;
    const auto loadEntry = [&](const Json &raw, bool buildInput) {
        if (!raw.isObject()) {
            Py::raiseValueError(QStringLiteral("frozen publication entry must be an object"));
        }
        FrozenEntry entry;
        entry.path = relativePath(raw.value(QStringLiteral("path")), QStringLiteral("frozen publication path"));
        const QString &relative = entry.path;
        const Json action = raw.value(QStringLiteral("action"));
        Py::hashKey(action);
        if (action != Json(QStringLiteral("upsert")) && action != Json(QStringLiteral("delete"))) {
            Py::raiseValueError(QStringLiteral("frozen publication action is invalid: ") + relative);
        }
        entry.action = action.toString();
        if (buildInput && raw.contains(QStringLiteral("base_sha256"))) {
            Py::raiseValueError(QStringLiteral("frozen build input contains a source base: ") + relative);
        }
        if (!buildInput) {
            const Json base = raw.value(QStringLiteral("base_sha256"));
            if ((schemaTwo || !base.isNull()) && !(base.isString() && Py::isLowerHex(base.toString(), 64))) {
                Py::raiseValueError(QStringLiteral("frozen source base SHA-256 is invalid: ") + relative);
            }
            if (!base.isNull()) {
                entry.baseSha256 = base.toString();
            }
        }
        const Json role = buildInput ? raw.value(QStringLiteral("role")) : Json();
        const Json mediaType = buildInput && entry.action == QLatin1String("upsert") ? raw.value(QStringLiteral("media_type")) : Json();
        const Json reuse = raw.contains(QStringLiteral("reuse_verified")) ? raw.value(QStringLiteral("reuse_verified")) : Json(false);
        if (!reuse.isBool()) {
            Py::raiseValueError(QStringLiteral("frozen build-input reuse flag is invalid: ") + relative);
        }
        if (reuse.toBool() && (!buildInput || entry.action != QLatin1String("upsert"))) {
            Py::raiseValueError(QStringLiteral("verified object reuse is invalid for frozen entry: ") + relative);
        }
        if (buildInput) {
            Py::hashKey(role);
            if (!role.isString() || !buildObjectRoles.contains(role.toString())) {
                Py::raiseValueError(QStringLiteral("frozen build-input role is invalid: ") + relative);
            }
            entry.role = role.toString();
        }
        if (buildInput && entry.action == QLatin1String("upsert")) {
            const QString text = mediaType.toString();
            if (!mediaType.isString() || text.isEmpty() || Py::codePointCount(text) > 255 || text.contains(QLatin1Char('\r'))
                || text.contains(QLatin1Char('\n'))) {
                Py::raiseValueError(QStringLiteral("frozen build-input media type is invalid: ") + relative);
            }
            entry.mediaType = text;
        }
        if (entry.action == QLatin1String("delete")) {
            for (const char *key : {"sha256", "size_bytes", "media_type", "reuse_verified"}) {
                if (raw.contains(QLatin1String(key))) {
                    Py::raiseValueError(QStringLiteral("frozen deletion contains object fields: ") + relative);
                }
            }
            return entry;
        }
        const Json digest = raw.value(QStringLiteral("sha256"));
        const Json size = raw.value(QStringLiteral("size_bytes"));
        if (!digest.isString() || !Py::isLowerHex(digest.toString(), 64)) {
            Py::raiseValueError(QStringLiteral("frozen publication SHA-256 is invalid: ") + relative);
        }
        if (size.type() != Json::Type::Int || size.toInt() < 0) {
            Py::raiseValueError(QStringLiteral("frozen publication size is invalid: ") + relative);
        }
        entry.sha256 = digest.toString();
        entry.sizeBytes = size.toInt();
        if (reuse.toBool()) {
            entry.reuseVerified = true;
            return entry;
        }
        if (!objectCache.contains(*entry.sha256)) {
            Py::Path objectPath;
            try {
                objectPath = Py::resolve(objectsRoot / *entry.sha256, true);
            } catch (const Py::Exception &error) {
                if (!Py::isOsError(error)) {
                    throw;
                }
                Py::raiseValueError(QStringLiteral("frozen publication object is unavailable: ") + relative);
            }
            if (objectPath.parent() != objectsRoot || !Py::isFile(objectPath)) {
                Py::raiseValueError(QStringLiteral("frozen publication object escapes its object root: ") + relative);
            }
            QByteArray content;
            try {
                content = Py::readBytes(objectPath);
            } catch (const Py::Exception &error) {
                if (!Py::isOsError(error)) {
                    throw;
                }
                Py::raiseValueError(QStringLiteral("frozen publication object is unavailable: ") + relative);
            }
            if (content.size() != *entry.sizeBytes || sha256Hex(content) != *entry.sha256) {
                Py::raiseValueError(QStringLiteral("frozen publication object failed identity verification: ") + relative);
            }
            objectCache.insert(*entry.sha256, content);
        }
        const QByteArray content = objectCache.value(*entry.sha256);
        if (content.size() != *entry.sizeBytes) {
            Py::raiseValueError(QStringLiteral("frozen publication size disagrees for shared object: ") + relative);
        }
        if (!buildInput) {
            QString error;
            if (!Py::decodeUtf8(content, &error)) {
                Py::raise(QStringLiteral("UnicodeDecodeError"), error);
            }
            if (content.size() > maximumManagedSourceBytes) {
                Py::raiseValueError(QStringLiteral("frozen managed source exceeds the size limit: ") + relative);
            }
        }
        entry.content = content;
        return entry;
    };
    const Json rawSources = payload.value(QStringLiteral("sources"));
    const Json rawBuildInputs = payload.value(QStringLiteral("build_inputs"));
    if (!rawSources.isArray() || !rawBuildInputs.isObject()) {
        Py::raiseValueError(QStringLiteral("frozen publication inputs are incomplete"));
    }
    FrozenInputs inputs;
    QSet<QString> seenSources;
    for (const Json &raw : rawSources.items()) {
        FrozenEntry entry = loadEntry(raw, false);
        if (seenSources.contains(entry.path)) {
            Py::raiseValueError(QStringLiteral("duplicate frozen source path: ") + entry.path);
        }
        seenSources.insert(entry.path);
        inputs.sources.append(entry);
    }
    if (inputs.sources.size() > 1000) {
        Py::raiseValueError(QStringLiteral("frozen source publication exceeds the 1000-file protocol limit"));
    }
    for (const auto &[rawTarget, rawEntries] : rawBuildInputs.members()) {
        const QString target = frozenTarget(rawTarget);
        if (!rawEntries.isArray()) {
            Py::raiseValueError(QStringLiteral("frozen build inputs must be a list: ") + target);
        }
        QSet<QString> seen;
        QList<FrozenEntry> entries;
        for (const Json &raw : rawEntries.items()) {
            FrozenEntry entry = loadEntry(raw, true);
            if (seen.contains(entry.path)) {
                Py::raiseValueError(QStringLiteral("duplicate frozen build-input path for %1: %2").arg(target, entry.path));
            }
            seen.insert(entry.path);
            entries.append(entry);
        }
        if (entries.size() > 10000) {
            Py::raiseValueError(QStringLiteral("frozen build input snapshot exceeds 10000 files: ") + target);
        }
        std::stable_sort(entries.begin(), entries.end(), [](const FrozenEntry &left, const FrozenEntry &right) {
            return Py::codePointLess(left.path, right.path);
        });
        inputs.buildInputs.append({target, entries});
    }
    inputs.sourceRoot = Py::resolve(sourceRoot);
    return inputs;
}

const FrozenEntry *FrozenInputs::source(const QString &path) const
{
    for (const FrozenEntry &entry : sources) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

QStringList FrozenInputs::sourcePaths() const
{
    QStringList paths;
    for (const FrozenEntry &entry : sources) {
        paths.append(entry.path);
    }
    return paths;
}

bool FrozenInputs::hasBuildTarget(const QString &target) const
{
    return buildTargets().contains(target);
}

QStringList FrozenInputs::buildTargets() const
{
    QStringList targets;
    for (const auto &item : buildInputs) {
        targets.append(item.first);
    }
    return targets;
}

void FrozenInputs::validateSources(const QSet<QString> &manifestPaths, const QSet<QString> &recordedPaths,
                                   const std::optional<QString> &scopeProject) const
{
    for (const FrozenEntry &entry : sources) {
        const QString &path = entry.path;
        if (!manifestPaths.contains(path) && !recordedPaths.contains(path) && !validSourcePath(sourceRoot, path)) {
            if (entry.action == QLatin1String("delete")) {
                continue;
            }
            Py::raiseValueError(QStringLiteral("frozen source path is not managed: ") + path);
        }
        if (scopeProject && !path.startsWith(*scopeProject + QLatin1Char('/'))) {
            bool owned = false;
            if (isManifestPath(path) && entry.action == QLatin1String("upsert")) {
                const Json value = Py::loadsBytes(entry.content.value_or(QByteArray()));
                validateConfiguration(value);
                for (const Json &folder : value.value(QStringLiteral("source_folders")).items()) {
                    owned = owned || Py::subscript(folder, QStringLiteral("path")) == Json(*scopeProject);
                }
            }
            if (!owned) {
                Py::raiseValueError(QStringLiteral("frozen source path is outside the scoped project: ") + path);
            }
        }
    }
}

std::optional<QList<BuildInput>> FrozenInputs::buildEntries(const QString &target, const QString &project) const
{
    const QList<FrozenEntry> *entries = nullptr;
    for (const auto &item : buildInputs) {
        if (item.first == target) {
            entries = &item.second;
        }
    }
    if (!entries) {
        return std::nullopt;
    }
    QStringList texPrefixes;
    for (const Py::Path &tree : projectTexRoots(sourceRoot / project)) {
        texPrefixes.append(*tree.relativeTo(sourceRoot));
    }
    QStringList assetPrefixes;
    QStringList generatedPrefixes;
    for (const QString &prefix : std::as_const(texPrefixes)) {
        assetPrefixes << prefix + QStringLiteral("/assets/") << prefix + QStringLiteral("/source/");
        generatedPrefixes << prefix + QStringLiteral("/generated/");
    }
    assetPrefixes << project + QStringLiteral("/markdown/assets/") << project + QStringLiteral("/markdown/source/");
    const auto startsWithAny = [](const QString &path, const QStringList &prefixes) {
        return std::any_of(prefixes.cbegin(), prefixes.cend(), [&path](const QString &prefix) {
            return path.startsWith(prefix);
        });
    };
    std::optional<QSet<QString>> stylesheetLoaded;
    QList<BuildInput> output;
    for (const FrozenEntry &entry : *entries) {
        const QString role = entry.role.value_or(QString());
        if (entry.action == QLatin1String("delete")) {
            // Dropping an input names a path the target used before. After a
            // document moved to another folder (move_source_document) that path
            // is in its old folder, so it cannot match the new folder's roles.
            if (entry.path.split(QLatin1Char('/')).contains(QStringLiteral("..")) || entry.path.contains(QLatin1Char('\\'))) {
                Py::raiseValueError(QStringLiteral("frozen build-input path does not match its role: ") + entry.path);
            }
            continue;
        }
        bool valid;
        if (role == QLatin1String("external_pdf")) {
            const QStringList parts = entry.path.split(QLatin1Char('/'));
            valid = (parts.size() == 2 && parts.at(0) == QLatin1String("third_party") && Py::casefold(Py::posixSuffix(entry.path)) == QLatin1String(".pdf"))
                || isManagedPdfPath(entry.path);
        } else {
            valid = startsWithAny(entry.path, role == QLatin1String("asset") ? assetPrefixes : generatedPrefixes);
            QStringList sourcePrefixes;
            for (const QString &prefix : std::as_const(texPrefixes)) {
                sourcePrefixes << prefix + QStringLiteral("/source/");
            }
            if (startsWithAny(entry.path, sourcePrefixes)) {
                valid = valid && isLatexSourceAsset(project + QStringLiteral("/tex/source/") + Py::posixName(entry.path));
            }
            if (entry.path.startsWith(project + QStringLiteral("/markdown/source/"))) {
                valid = valid && isMarkdownAsset(entry.path);
            }
            valid = valid || (role == QLatin1String("asset") && isHtmlSourcePath(entry.path, project));
            valid = valid
                || (role == QLatin1String("asset") && entry.path.startsWith(project + QLatin1Char('/'))
                    && !entry.path.split(QLatin1Char('/')).contains(QStringLiteral("..")) && !entry.path.contains(QLatin1Char('\\'))
                    && isMarkdownAsset(entry.path));
            if (!valid && role == QLatin1String("asset") && entry.path.startsWith(project + QLatin1Char('/'))) {
                const QString relative = entry.path.mid(project.size() + 1);
                valid = latexAssetSuffixes.contains(Py::casefold(Py::posixSuffix(relative))) && latexFolderInput(sourceRoot / project, relative);
            }
            if (!valid && role == QLatin1String("asset")) {
                if (!stylesheetLoaded) {
                    stylesheetLoaded = QSet<QString>();
                    for (const Py::Path &path : stylesheetDependencies(sourceRoot / project)) {
                        stylesheetLoaded->insert(*path.relativeTo(sourceRoot));
                    }
                }
                valid = stylesheetLoaded->contains(entry.path);
            }
        }
        if (!valid) {
            Py::raiseValueError(QStringLiteral("frozen build-input path does not match its role: ") + entry.path);
        }
        if (entry.action == QLatin1String("delete")) {
            continue;
        }
        BuildInput input;
        input.path = entry.path;
        input.sha256 = entry.sha256.value_or(QString());
        input.sizeBytes = entry.sizeBytes.value_or(0);
        input.mediaType = entry.mediaType.value_or(QString());
        input.role = role;
        input.frozenContent = entry.content;
        input.reuseVerified = entry.reuseVerified;
        output.append(input);
    }
    return output;
}

Json FrozenInputs::acceptedEntries(const QSet<QString> &sourcePaths, const QSet<QString> &buildTargets) const
{
    QList<Json> accepted;
    QStringList paths(sourcePaths.cbegin(), sourcePaths.cend());
    std::sort(paths.begin(), paths.end(), Py::codePointLess);
    for (const QString &path : std::as_const(paths)) {
        if (const FrozenEntry *entry = source(path)) {
            accepted.append(entry->publicIdentity(QStringLiteral("source")));
        }
    }
    QStringList targets(buildTargets.cbegin(), buildTargets.cend());
    std::stable_sort(targets.begin(), targets.end(), [](const QString &left, const QString &right) {
        return Py::codePointLess(Py::casefold(left), Py::casefold(right));
    });
    for (const QString &target : std::as_const(targets)) {
        for (const auto &item : buildInputs) {
            if (item.first != target) {
                continue;
            }
            for (const FrozenEntry &entry : item.second) {
                accepted.append(entry.publicIdentity(QStringLiteral("build_input"), target));
            }
        }
    }
    std::stable_sort(accepted.begin(), accepted.end(), [](const Json &left, const Json &right) {
        const QString leftKind = left.value(QStringLiteral("kind")).toString();
        const QString rightKind = right.value(QStringLiteral("kind")).toString();
        if (leftKind != rightKind) {
            return Py::codePointLess(leftKind, rightKind);
        }
        const QString leftTarget = Py::casefold(left.value(QStringLiteral("target")).toString());
        const QString rightTarget = Py::casefold(right.value(QStringLiteral("target")).toString());
        if (leftTarget != rightTarget) {
            return Py::codePointLess(leftTarget, rightTarget);
        }
        return Py::codePointLess(left.value(QStringLiteral("path")).toString(), right.value(QStringLiteral("path")).toString());
    });
    return Json::array(accepted);
}

}
