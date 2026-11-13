// SPDX-License-Identifier: GPL-2.0-or-later
#include "projectconfig.h"
#include "sourceowner.h"

#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSet>

namespace Thoughts::Publisher
{
namespace
{
const QString configurationFileName = QStringLiteral("thoughts-project.json");

// Python's `value in <dict or set>`: unhashable keys raise TypeError.
void requireHashable(const Json &value, const char *container)
{
    Py::hashKey(value, container);
}

QString sha256Hex(const QString &text)
{
    const std::u32string codePoints = Py::toCodePoints(text);
    for (size_t index = 0; index < codePoints.size(); ++index) {
        if (codePoints[index] >= 0xD800 && codePoints[index] <= 0xDFFF) {
            Py::raise(QStringLiteral("UnicodeEncodeError"),
                      QStringLiteral("'utf-8' codec can't encode character '\\u%1' in position %2: surrogates not allowed")
                          .arg(uint(codePoints[index]), 4, 16, QLatin1Char('0'))
                          .arg(index));
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool isHexDigit(QChar c)
{
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

// int(text, 16) succeeds (ASCII digits, optional sign, 0x prefix, single
// underscores between digits, surrounding whitespace).
bool parsesAsHexInteger(const QString &input)
{
    QString text = Py::strip(input);
    if (text.startsWith(QLatin1Char('+')) || text.startsWith(QLatin1Char('-'))) {
        text = text.mid(1);
    }
    bool prefixed = false;
    if (text.startsWith(QLatin1String("0x")) || text.startsWith(QLatin1String("0X"))) {
        text = text.mid(2);
        prefixed = true;
    }
    if (text.isEmpty()) {
        return false;
    }
    bool previousDigit = prefixed;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar c = text.at(index);
        if (c == QLatin1Char('_')) {
            if (!previousDigit || index + 1 >= text.size()) {
                return false;
            }
            previousDigit = false;
            continue;
        }
        if (!isHexDigit(c)) {
            return false;
        }
        previousDigit = true;
    }
    return true;
}

// _uuid(value, document=...)
QString permanentUuid(const Json &value, bool document = false)
{
    if (!value.isString()) {
        Py::raiseValueError(QStringLiteral("A permanent UUID is required"));
    }
    const QString text = value.toString();
    QString hex = text;
    hex.replace(QStringLiteral("urn:"), QString()).replace(QStringLiteral("uuid:"), QString());
    hex = Py::strip(hex, QStringLiteral("{}"));
    hex.remove(QLatin1Char('-'));
    if (Py::codePointCount(hex) != 32) {
        Py::raiseValueError(QStringLiteral("badly formed hexadecimal UUID string"));
    }
    if (!parsesAsHexInteger(hex)) {
        Py::raiseValueError(QStringLiteral("invalid literal for int() with base 16: ") + Py::repr(hex));
    }
    // str(UUID(text)) is the lowercase 8-4-4-4-12 form.
    bool canonical = text.size() == 36;
    for (qsizetype index = 0; canonical && index < 36; ++index) {
        const QChar c = text.at(index);
        canonical = (index == 8 || index == 13 || index == 18 || index == 23) ? c == QLatin1Char('-')
                                                                                : (isHexDigit(c) && !(c >= QLatin1Char('A') && c <= QLatin1Char('F')));
    }
    if (!canonical) {
        Py::raiseValueError(QStringLiteral("Invalid permanent UUID"));
    }
    if (document) {
        const QChar variant = text.at(19);
        const bool rfc4122 = variant == QLatin1Char('8') || variant == QLatin1Char('9') || variant == QLatin1Char('a') || variant == QLatin1Char('b');
        if (!rfc4122 || text.at(14) != QLatin1Char('4')) {
            Py::raiseValueError(QStringLiteral("Invalid permanent UUID"));
        }
    }
    return text;
}

// _relative(value)
QString safeRelative(const Json &value)
{
    const auto fail = [] {
        Py::raiseValueError(QStringLiteral("Expected a safe folder-relative path"));
    };
    if (!value.isString()) {
        fail();
    }
    const QString text = value.toString();
    if (text.isEmpty() || text == QLatin1String(".") || text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char('\\'))) {
        fail();
    }
    for (const QString &segment : text.split(QLatin1Char('/'))) {
        // posixpath.normpath(text) == text, and no ".." part.
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            fail();
        }
    }
    for (const QChar c : text) {
        if (c.unicode() < 32 || c.unicode() == 127) {
            fail();
        }
    }
    return text;
}

QSet<QString> keySet(const Json &object)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend());
}

bool equalsInt(const Json &value, int expected)
{
    return value == Json(expected);
}

// posixpath.relpath for two relative paths.
QString relativePosixPath(const QString &path, const QString &start)
{
    const auto normalized = [](const QString &text) {
        QStringList parts;
        for (const QString &segment : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (segment == QLatin1String(".")) {
                continue;
            }
            if (segment == QLatin1String("..") && !parts.isEmpty() && parts.last() != QLatin1String("..")) {
                parts.removeLast();
            } else {
                parts.append(segment);
            }
        }
        return parts;
    };
    const QStringList pathParts = normalized(path);
    const QStringList startParts = normalized(start);
    qsizetype common = 0;
    while (common < pathParts.size() && common < startParts.size() && pathParts.at(common) == startParts.at(common)) {
        ++common;
    }
    QStringList result;
    for (qsizetype index = common; index < startParts.size(); ++index) {
        result.append(QStringLiteral(".."));
    }
    result.append(pathParts.mid(common));
    return result.isEmpty() ? QStringLiteral(".") : result.join(QLatin1Char('/'));
}

struct Identity {
    bool present = false;
    quint64 device = 0;
    quint64 inode = 0;
    qint64 size = 0;
    qint64 modified = 0;
    qint64 changed = 0;
    bool operator==(const Identity &) const = default;
};

Identity identityOf(const Py::FileStatus &status)
{
    if (status.kind == Py::FileStatus::Kind::Missing) {
        return {};
    }
    return {true, status.device, status.inode, status.size, status.modifiedNs, status.changedNs};
}

struct CachedRead {
    Identity identity;
    Json value;
};
struct CachedOwners {
    QList<QPair<QString, Identity>> identity;
    QHash<QString, QList<Path>> owners;
};
QHash<QString, CachedRead> readCache;
QHash<QString, CachedOwners> ownersCache;
}

bool isDefaultFolderOf(const QString &location, const QString &projectId)
{
    // Either spelling, default (canonical) or documents (legacy).
    return SourceOwner::isDefaultFolder(location) && location.mid(SourceOwner::ProjectsPrefix.size(), SourceOwner::HashLength) == sha256Hex(projectId);
}

QString manifestPathOf(const Json &value)
{
    const Json defaultId = value.value(QStringLiteral("default_folder_id"));
    for (const Json &folder : value.value(QStringLiteral("source_folders")).items()) {
        if (folder.value(QStringLiteral("folder_id")) == defaultId && folder.value(QStringLiteral("path")).isString()) {
            return folder.value(QStringLiteral("path")).toString() + QLatin1Char('/') + configurationFileName;
        }
    }
    return {};
}

bool isManifestPath(const QString &path)
{
    return SourceOwner::isManifestPath(path);
}

QList<Path> configurationPaths(const Path &root, bool sorted)
{
    const Path base = root / QStringLiteral(".thoughts/projects");
    QList<Path> paths;
    const std::optional<QList<Py::DirectoryEntry>> entries = Py::scanDirectory(base);
    if (!entries) {
        return paths;
    }
    for (const Py::DirectoryEntry &entry : *entries) {
        if (!entry.isDirectory) {
            continue;
        }
        // <hash>/default before <hash>/documents: a Project has one default folder.
        for (const QLatin1StringView segment : {SourceOwner::DefaultSegment, SourceOwner::LegacyDefaultSegment}) {
            const Path candidate = base / entry.name / QString(segment) / configurationFileName;
            if (Py::linkExists(candidate)) {
                paths.append(candidate);
                break;
            }
        }
    }
    if (sorted) {
        std::stable_sort(paths.begin(), paths.end(), Path::less);
    }
    return paths;
}

QStringList selectedSourceFolders(const Json &value)
{
    QSet<QString> selected;
    for (const Json &document : value.value(QStringLiteral("documents")).items()) {
        const QString format = document.value(QStringLiteral("format")).toString();
        if (document.value(QStringLiteral("included")).toBool() && (format == QLatin1String("latex") || format == QLatin1String("markdown"))) {
            selected.insert(document.value(QStringLiteral("source")).value(QStringLiteral("folder_id")).toString());
        }
    }
    QStringList folders;
    for (const Json &folder : value.value(QStringLiteral("source_folders")).items()) {
        if (selected.contains(folder.value(QStringLiteral("folder_id")).toString())) {
            folders.append(folder.value(QStringLiteral("path")).toString());
        }
    }
    return folders;
}

void validateConfiguration(const Json &value)
{
    static const QSet<QString> topKeys{QStringLiteral("schema_version"), QStringLiteral("kind"),   QStringLiteral("id"),
                                       QStringLiteral("name"),           QStringLiteral("default_folder_id"),
                                       QStringLiteral("source_folders"), QStringLiteral("documents"),
                                       QStringLiteral("rendering"),      QStringLiteral("document_names")};
    if (!value.isObject() || !equalsInt(value.value(QStringLiteral("schema_version")), 5)
        || value.value(QStringLiteral("kind")) != Json(QStringLiteral("thoughts-project")) || !(keySet(value) - topKeys).isEmpty()
        || !value.value(QStringLiteral("id")).isString() || value.value(QStringLiteral("id")).toString().isEmpty()
        || !value.value(QStringLiteral("name")).isString() || !value.value(QStringLiteral("source_folders")).isArray()
        || !value.value(QStringLiteral("documents")).isArray()) {
        Py::raiseValueError(QStringLiteral("Invalid Project configuration"));
    }
    const QString projectId = value.value(QStringLiteral("id")).toString();
    QHash<QString, Json> folders;
    QSet<QString> locations;
    QSet<QString> aggregateIds;
    const QString defaultId = permanentUuid(value.value(QStringLiteral("default_folder_id")));
    static const QSet<QString> folderKeys{QStringLiteral("folder_id"), QStringLiteral("path"), QStringLiteral("document_id")};
    for (const Json &folder : value.value(QStringLiteral("source_folders")).items()) {
        if (!folder.isObject() || keySet(folder) != folderKeys) {
            Py::raiseValueError(QStringLiteral("Invalid source-folder binding"));
        }
        const QString folderId = permanentUuid(folder.value(QStringLiteral("folder_id")));
        const QString aggregate = permanentUuid(folder.value(QStringLiteral("document_id")), folderId != defaultId);
        const QString location = safeRelative(folder.value(QStringLiteral("path")));
        if (folders.contains(folderId) || locations.contains(Py::casefold(location)) || aggregateIds.contains(aggregate)) {
            Py::raiseValueError(QStringLiteral("Duplicate source-folder identity or location"));
        }
        if (folderId == defaultId) {
            if (!isDefaultFolderOf(location, projectId)) {
                Py::raiseValueError(QStringLiteral("Default source folder belongs to another Project"));
            }
        } else if (!Py::isConnectedFolderName(location)) {
            Py::raiseValueError(QStringLiteral("Invalid connected source-folder location"));
        }
        folders.insert(folderId, folder);
        locations.insert(Py::casefold(location));
        aggregateIds.insert(aggregate);
    }
    if (!folders.contains(defaultId)) {
        Py::raiseValueError(QStringLiteral("Missing default source folder"));
    }
    QSet<QString> identifiers = aggregateIds;
    QSet<QPair<QString, QString>> sources;
    QList<QPair<QString, Json>> byId;
    QHash<QString, Json> documentsById;
    static const QSet<QString> documentKeys{QStringLiteral("document_id"), QStringLiteral("source"), QStringLiteral("format"),
                                            QStringLiteral("included")};
    static const QSet<QString> sourceKeys{QStringLiteral("folder_id"), QStringLiteral("path")};
    for (const Json &document : value.value(QStringLiteral("documents")).items()) {
        if (!document.isObject() || keySet(document) != documentKeys) {
            Py::raiseValueError(QStringLiteral("Every document requires exactly document_id, source, format, and included"));
        }
        const QString documentId = permanentUuid(document.value(QStringLiteral("document_id")), true);
        if (identifiers.contains(documentId)) {
            Py::raiseValueError(QStringLiteral("Duplicate permanent document identity"));
        }
        identifiers.insert(documentId);
        const Json source = document.value(QStringLiteral("source"));
        if (!source.isObject() || keySet(source) != sourceKeys) {
            Py::raiseValueError(QStringLiteral("Document references an unknown source folder"));
        }
        const Json sourceFolder = source.value(QStringLiteral("folder_id"));
        requireHashable(sourceFolder, "dict key");
        if (!sourceFolder.isString() || !folders.contains(sourceFolder.toString())) {
            Py::raiseValueError(QStringLiteral("Document references an unknown source folder"));
        }
        const QString path = safeRelative(source.value(QStringLiteral("path")));
        const Json kind = document.value(QStringLiteral("format"));
        requireHashable(kind, "set element");
        const QString format = kind.isString() ? kind.toString() : QString();
        if ((format != QLatin1String("latex") && format != QLatin1String("markdown") && format != QLatin1String("pdf"))
            || !document.value(QStringLiteral("included")).isBool()) {
            Py::raiseValueError(QStringLiteral("Invalid document format or inclusion"));
        }
        const QString suffix = Py::lower(Py::posixSuffix(path));
        if (format == QLatin1String("pdf")) {
            if (sourceFolder.toString() != defaultId || !Py::isSafePdfFilename(path)) {
                Py::raiseValueError(QStringLiteral("Imported PDF must belong to the Project's default source folder"));
            }
        } else if (format == QLatin1String("latex") ? suffix != QLatin1String(".tex")
                                                    : (suffix != QLatin1String(".md") && suffix != QLatin1String(".markdown"))) {
            Py::raiseValueError(QStringLiteral("Invalid editable document source"));
        }
        const QPair<QString, QString> key{sourceFolder.toString(), Py::casefold(path)};
        if (sources.contains(key)) {
            Py::raiseValueError(QStringLiteral("Duplicate document source"));
        }
        sources.insert(key);
        byId.append({documentId, document});
        documentsById.insert(documentId, document);
    }
    const Json rendering = value.contains(QStringLiteral("rendering")) ? value.value(QStringLiteral("rendering")) : Json::object();
    if (!rendering.isObject()) {
        Py::raiseValueError(QStringLiteral("Invalid rendering metadata"));
    }
    static const QSet<QString> buildFields{QStringLiteral("output"), QStringLiteral("stylesheet"), QStringLiteral("working_directory"),
                                           QStringLiteral("sha256")};
    for (const auto &[key, options] : rendering.members()) {
        bool valid = documentsById.contains(key) && options.isObject() && (keySet(options) - buildFields).isEmpty();
        if (valid) {
            for (const auto &option : options.members()) {
                if (!option.second.isString() || option.second.toString().isEmpty()) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            Py::raiseValueError(QStringLiteral("Invalid rendering metadata"));
        }
        const QString kind = documentsById.value(key).value(QStringLiteral("format")).toString();
        if (kind == QLatin1String("pdf") && keySet(options) != QSet<QString>{QStringLiteral("sha256")}) {
            Py::raiseValueError(QStringLiteral("PDF rendering metadata must contain its fingerprint"));
        }
        if (options.contains(QStringLiteral("sha256"))
            && (kind != QLatin1String("pdf") || !Py::isLowerHex(options.value(QStringLiteral("sha256")).toString(), 64))) {
            Py::raiseValueError(QStringLiteral("Invalid PDF fingerprint"));
        }
        if (options.contains(QStringLiteral("working_directory"))) {
            if (kind != QLatin1String("latex")) {
                Py::raiseValueError(QStringLiteral("Invalid compiler working directory"));
            }
            if (options.value(QStringLiteral("working_directory")).toString() != QLatin1String(".")) {
                safeRelative(options.value(QStringLiteral("working_directory")));
            }
        }
    }
    for (const auto &[key, document] : byId) {
        if (document.value(QStringLiteral("format")).toString() == QLatin1String("pdf")
            && !rendering.value(key).contains(QStringLiteral("sha256"))) {
            Py::raiseValueError(QStringLiteral("Missing PDF fingerprint"));
        }
    }
    const Json names = value.contains(QStringLiteral("document_names")) ? value.value(QStringLiteral("document_names")) : Json::object();
    bool namesValid = names.isObject();
    if (namesValid) {
        for (const auto &[key, name] : names.members()) {
            if (!documentsById.contains(key) || !name.isString()) {
                namesValid = false;
                break;
            }
        }
    }
    if (!namesValid) {
        Py::raiseValueError(QStringLiteral("Invalid document names"));
    }
}

Json readConfiguration(const Path &path)
{
    if (Py::isSymlink(path)) {
        Py::raiseValueError(QStringLiteral("Project configuration must not be a symlink"));
    }
    const Py::FileStatus before = Py::status(path);
    if (before.kind == Py::FileStatus::Kind::Missing) {
        Py::raiseOsError(before.error, path.str());
    }
    const Identity identity = identityOf(before);
    const QString key = path.posix();
    const auto cached = readCache.constFind(key);
    if (cached != readCache.constEnd() && cached->identity == identity) {
        return cached->value;
    }
    Json value = Py::loadsBytes(Py::readBytes(path));
    validateConfiguration(value);
    if (!path.posix().endsWith(QLatin1Char('/') + manifestPathOf(value))) {
        Py::raiseValueError(QStringLiteral("Project configuration is stored outside its default folder"));
    }
    if (identityOf(Py::status(path)) == identity) {
        readCache.insert(key, {identity, value});
    }
    return value;
}

std::optional<Configuration> findConfiguration(const Path &root, const QString &owner)
{
    const QList<Path> paths = configurationPaths(root);
    QList<QPair<QString, Identity>> identity;
    for (const Path &path : paths) {
        identity.append({path.posix(), identityOf(Py::linkStatus(path))});
    }
    const QString cacheKey = root.posix();
    auto cached = ownersCache.find(cacheKey);
    if (cached == ownersCache.end() || cached->identity != identity) {
        QHash<QString, QList<Path>> owners;
        const Path resolvedRoot = Py::resolve(root);
        for (const Path &path : paths) {
            if (!Py::resolve(path).isRelativeTo(resolvedRoot)) {
                Py::raiseValueError(QStringLiteral("Project configuration escapes the workspace"));
            }
            const Json configuration = readConfiguration(path);
            for (const Json &folder : configuration.value(QStringLiteral("source_folders")).items()) {
                owners[folder.value(QStringLiteral("path")).toString()].append(path);
            }
        }
        cached = ownersCache.insert(cacheKey, {identity, owners});
    }
    const QList<Path> matches = cached->owners.value(owner);
    if (matches.size() > 1) {
        Py::raiseValueError(QStringLiteral("Source folder belongs to multiple Projects"));
    }
    if (matches.isEmpty()) {
        return std::nullopt;
    }
    return Configuration{matches.first(), readConfiguration(matches.first())};
}

std::optional<Path> configurationLocation(const Path &project)
{
    const auto [root, owner] = folderOwner(project);
    const std::optional<Configuration> configuration = findConfiguration(root, owner);
    if (!configuration) {
        return std::nullopt;
    }
    return configuration->path;
}

Json projection(const Json &value, const QString &owner)
{
    validateConfiguration(value);
    std::optional<Json> folder;
    for (const Json &candidate : value.value(QStringLiteral("source_folders")).items()) {
        if (candidate.value(QStringLiteral("path")).toString() == owner) {
            folder = candidate;
            break;
        }
    }
    if (!folder) {
        Py::raiseValueError(QStringLiteral("Source folder does not belong to this Project"));
    }
    const QString folderId = folder->value(QStringLiteral("folder_id")).toString();
    const Json allRendering = value.contains(QStringLiteral("rendering")) ? value.value(QStringLiteral("rendering")) : Json::object();
    Json documents = Json::array();
    Json rendering = Json::object();
    for (const Json &original : value.value(QStringLiteral("documents")).items()) {
        const Json source = original.value(QStringLiteral("source"));
        const bool pdf = original.value(QStringLiteral("format")).toString() == QLatin1String("pdf");
        if (source.value(QStringLiteral("folder_id")).toString() != folderId && !pdf) {
            continue;
        }
        Json entry = original;
        if (pdf) {
            QString sourcePath;
            for (const Json &candidate : value.value(QStringLiteral("source_folders")).items()) {
                if (candidate.value(QStringLiteral("folder_id")) == source.value(QStringLiteral("folder_id"))) {
                    sourcePath = candidate.value(QStringLiteral("path")).toString() + QLatin1Char('/') + source.value(QStringLiteral("path")).toString();
                    break;
                }
            }
            entry.insert(QStringLiteral("source"), relativePosixPath(sourcePath, owner));
        } else {
            entry.insert(QStringLiteral("source"), source.value(QStringLiteral("path")));
        }
        documents.append(entry);
        const QString documentId = entry.value(QStringLiteral("document_id")).toString();
        if (allRendering.contains(documentId)) {
            rendering.insert(documentId, allRendering.value(documentId));
        }
    }
    Json result = Json::object({{QStringLiteral("schema_version"), Json(4)},
                                {QStringLiteral("document_id"), folder->value(QStringLiteral("document_id"))},
                                {QStringLiteral("documents"), documents}});
    if (rendering.size() > 0) {
        result.insert(QStringLiteral("rendering"), rendering);
    }
    return result;
}

}
