// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringView>

namespace Thoughts
{
// The folder that owns a source path relative to the docs root: a connected
// folder docs-<name>, or a Project's default folder
// .thoughts/projects/<64 lowercase hex>/<segment>. The segment is `default`
// (canonical) or `documents` (legacy, before the 2026-10-17 rename); both are
// accepted everywhere, and one Project has exactly one of them. An owner is
// always the full path from the docs root, so two Projects' default folders
// never share one. Shared with tools/publisher; the server and Android apply
// the same rule.
struct SourceOwner {
    static constexpr QLatin1StringView DefaultSegment{"default"};
    static constexpr QLatin1StringView LegacyDefaultSegment{"documents"};
    static constexpr QLatin1StringView ProjectsPrefix{".thoughts/projects/"};
    static constexpr qsizetype HashLength = 64;
    // ".thoughts/projects/" + 64 + "/default"
    static constexpr qsizetype DefaultFolderLength = 19 + HashLength + 1 + 7;
    // ".thoughts/projects/" + 64 + "/documents"
    static constexpr qsizetype LegacyDefaultFolderLength = 19 + HashLength + 1 + 9;

    static bool isLowerHex(QStringView value)
    {
        for (const QChar character : value) {
            const char16_t c = character.unicode();
            if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) {
                return false;
            }
        }
        return true;
    }
    // ^docs-[A-Za-z0-9_.-]+$
    static bool isConnectedFolder(QStringView value)
    {
        if (value.size() <= 5 || !value.startsWith(u"docs-")) {
            return false;
        }
        for (const QChar character : value.mid(5)) {
            const char16_t c = character.unicode();
            if (!((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'_' || c == u'.' || c == u'-')) {
                return false;
            }
        }
        return true;
    }
    // ^\.thoughts/projects/[0-9a-f]{64}/(default|documents)$
    static bool isDefaultFolder(QStringView value)
    {
        if (value.size() != DefaultFolderLength && value.size() != LegacyDefaultFolderLength) {
            return false;
        }
        if (!value.startsWith(ProjectsPrefix) || value.at(19 + HashLength) != QLatin1Char('/') || !isLowerHex(value.mid(19, HashLength))) {
            return false;
        }
        const QStringView segment = value.mid(19 + HashLength + 1);
        return segment == DefaultSegment || segment == LegacyDefaultSegment;
    }
    static bool isLegacyDefaultFolder(const QString &value)
    {
        return value.size() == LegacyDefaultFolderLength && isDefaultFolder(value);
    }
    static bool isOwner(QStringView value)
    {
        return isConnectedFolder(value) || isDefaultFolder(value);
    }
    // The length of the default folder that starts path (followed by "/" or
    // the end), or 0.
    static qsizetype defaultFolderPrefixLength(QStringView path)
    {
        for (const qsizetype length : {DefaultFolderLength, LegacyDefaultFolderLength}) {
            if (path.size() >= length && isDefaultFolder(path.left(length)) && (path.size() == length || path.at(length) == QLatin1Char('/'))) {
                return length;
            }
        }
        return 0;
    }
    // The owner of a docs-root-relative path, or empty.
    static QString of(const QString &path)
    {
        const qsizetype slash = path.indexOf(QLatin1Char('/'));
        const QStringView first = slash < 0 ? QStringView(path) : QStringView(path).left(slash);
        if (isConnectedFolder(first)) {
            return first.toString();
        }
        const qsizetype length = defaultFolderPrefixLength(path);
        return length ? path.left(length) : QString();
    }
    // Everything after owner + "/", or empty without an owner.
    static QString relative(const QString &path)
    {
        const QString owner = of(path);
        return owner.isEmpty() || path.size() <= owner.size() + 1 ? QString() : path.mid(owner.size() + 1);
    }
    // The owner of an absolute or relative directory, given the docs root:
    // the directory's own path below root when that is an owner.
    static QString ofDirectory(const QString &root, const QString &directory)
    {
        if (!directory.startsWith(root + QLatin1Char('/'))) {
            return {};
        }
        const QString owner = directory.mid(root.size() + 1);
        return isOwner(owner) ? owner : QString();
    }
    // sha256(Project id) in lowercase hex: the Project's directory name.
    static QString projectHash(const QString &projectId)
    {
        return QString::fromLatin1(QCryptographicHash::hash(projectId.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    static QString defaultFolder(const QString &hash, QLatin1StringView segment)
    {
        return ProjectsPrefix + hash + QLatin1Char('/') + segment;
    }
    // A path that starts with a default folder, with that folder written in
    // its legacy spelling (.../documents); anything else unchanged.
    static QString legacySpelling(const QString &path)
    {
        if (defaultFolderPrefixLength(path) != DefaultFolderLength) {
            return path;
        }
        return path.left(19 + HashLength + 1) + LegacyDefaultSegment + path.mid(DefaultFolderLength);
    }
    // ^\.thoughts/projects/[0-9a-f]{64}/(default|documents)/thoughts-project\.json$
    static bool isManifestPath(const QString &path)
    {
        const qsizetype length = defaultFolderPrefixLength(path);
        return length && QStringView(path).mid(length) == u"/thoughts-project.json";
    }
    // The Library PDF of a LaTeX or Markdown document in a default folder
    // (spec Rule 2): document_ + 20 hex of SHA-256(owner/relative) + .pdf.
    // The owner is hashed in its legacy spelling, so renaming the folder from
    // documents to default never renames a PDF (rename spec Rule 4).
    static QString defaultFolderOutput(const QString &sourceId)
    {
        return QStringLiteral("document_")
            + QString::fromLatin1(QCryptographicHash::hash(legacySpelling(sourceId).toUtf8(), QCryptographicHash::Sha256).toHex().left(20))
            + QStringLiteral(".pdf");
    }
    // A default folder's root, given the absolute path of a directory that
    // may be one: the docs root for .../.thoughts/projects/<hex>/<segment>.
    static QString defaultFolderRoot(const QString &absoluteDirectory)
    {
        for (const qsizetype length : {DefaultFolderLength, LegacyDefaultFolderLength}) {
            if (absoluteDirectory.size() > length + 1 && absoluteDirectory.at(absoluteDirectory.size() - length - 1) == QLatin1Char('/')
                && isDefaultFolder(QStringView(absoluteDirectory).right(length))) {
                return absoluteDirectory.left(absoluteDirectory.size() - length - 1);
            }
        }
        return {};
    }

    // Rule R of the default-folder rename, the only rewrite there is:
    // \.thoughts/projects/([0-9a-f]{64})/documents(?=/|"|'|\\|$|\s)
    //   -> .thoughts/projects/\1/default
    // applied textually, so every implementation produces identical bytes.
    // It matches inside absolute and ../ paths too. Like the server's Python,
    // \s is Unicode whitespace for text (str) and ASCII whitespace for bytes.
    template<typename Text, typename At, typename Space>
    static bool legacyMatchAt(const Text &text, qsizetype index, At at, Space isSpace)
    {
        static constexpr char prefix[] = ".thoughts/projects/";
        static constexpr char segment[] = "/documents";
        const qsizetype size = text.size();
        if (index + LegacyDefaultFolderLength > size) {
            return false;
        }
        for (qsizetype i = 0; i < 19; ++i) {
            if (at(text, index + i) != char32_t(prefix[i])) {
                return false;
            }
        }
        for (qsizetype i = 19; i < 19 + HashLength; ++i) {
            const char32_t c = at(text, index + i);
            if (!((c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'f'))) {
                return false;
            }
        }
        for (qsizetype i = 0; i < 10; ++i) {
            if (at(text, index + 19 + HashLength + i) != char32_t(segment[i])) {
                return false;
            }
        }
        const qsizetype end = index + LegacyDefaultFolderLength;
        if (end == size) {
            return true;
        }
        const char32_t next = at(text, end);
        return next == U'/' || next == U'"' || next == U'\'' || next == U'\\' || isSpace(next);
    }
    // Python's str \s (str.isspace()) for the character after the match.
    static bool isRegexSpace(char32_t c)
    {
        if (c == U' ' || (c >= U'\t' && c <= U'\r') || (c >= 0x1c && c <= 0x1f)) {
            return true;
        }
        return c >= 0x80 && c <= 0x10FFFF && QChar::isSpace(c);
    }
    // Python's bytes \s: [ \t\n\r\f\v].
    static bool isAsciiRegexSpace(char32_t c)
    {
        return c == U' ' || (c >= U'\t' && c <= U'\r');
    }
    static QString rewriteLegacyDefaultFolder(const QString &text)
    {
        qsizetype from = text.indexOf(QLatin1StringView(".thoughts/projects/"));
        if (from < 0) {
            return text;
        }
        const auto at = [](const QString &value, qsizetype index) -> char32_t {
            const QChar character = value.at(index);
            if (character.isHighSurrogate() && index + 1 < value.size() && value.at(index + 1).isLowSurrogate()) {
                return QChar::surrogateToUcs4(character, value.at(index + 1));
            }
            return character.unicode();
        };
        QString result;
        qsizetype copied = 0;
        while (from >= 0) {
            if (legacyMatchAt(text, from, at, isRegexSpace)) {
                result += QStringView(text).mid(copied, from + 19 + HashLength + 1 - copied);
                result += DefaultSegment;
                copied = from + LegacyDefaultFolderLength;
                from = text.indexOf(QLatin1StringView(".thoughts/projects/"), copied);
            } else {
                from = text.indexOf(QLatin1StringView(".thoughts/projects/"), from + 1);
            }
        }
        if (copied == 0) {
            return text;
        }
        result += QStringView(text).mid(copied);
        return result;
    }
    // The same rewrite on UTF-8 bytes (JSON files, SQLite text and blobs).
    static QByteArray rewriteLegacyDefaultFolder(const QByteArray &bytes)
    {
        static const QByteArray needle(".thoughts/projects/");
        qsizetype from = bytes.indexOf(needle);
        if (from < 0) {
            return bytes;
        }
        const auto at = [](const QByteArray &value, qsizetype index) -> char32_t {
            return static_cast<unsigned char>(value.at(index));
        };
        QByteArray result;
        qsizetype copied = 0;
        while (from >= 0) {
            if (legacyMatchAt(bytes, from, at, isAsciiRegexSpace)) {
                result += QByteArrayView(bytes).mid(copied, from + 19 + HashLength + 1 - copied);
                result += QByteArrayView("default");
                copied = from + LegacyDefaultFolderLength;
                from = bytes.indexOf(needle, copied);
            } else {
                from = bytes.indexOf(needle, from + 1);
            }
        }
        if (copied == 0) {
            return bytes;
        }
        result += QByteArrayView(bytes).mid(copied);
        return result;
    }
    static bool containsLegacyDefaultFolder(const QByteArray &bytes)
    {
        return rewriteLegacyDefaultFolder(bytes) != bytes;
    }
};

// Where a Project's default folder lives on this computer (rename spec
// compatibility rules 2 and 5). A Project has exactly one default folder;
// default/ wins when both exist (the migration reports that case).
struct DefaultFolderLocation {
    static QString projectsDirectory(const QString &root)
    {
        return QDir(root).filePath(QStringLiteral(".thoughts/projects"));
    }
    // The segment the Library uses for new default folders: `default` once any
    // known Project's default folder is called that, otherwise `documents`.
    static QLatin1StringView librarySegment(const QString &root)
    {
        const QDir projects(projectsDirectory(root));
        for (const QString &name : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (name.size() == SourceOwner::HashLength && SourceOwner::isLowerHex(name)
                && QFileInfo(projects.filePath(name + QLatin1Char('/') + SourceOwner::DefaultSegment)).isDir()) {
                return SourceOwner::DefaultSegment;
            }
        }
        // Fedora's catalogue (installed by Sync) names its default folders.
        QFile catalogue(QDir(root).filePath(QStringLiteral(".thoughts/library-projects.json")));
        if (catalogue.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = catalogue.readAll();
            if (bytes.contains("/default/thoughts-project.json")) {
                const auto projectsValue = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("projects")).toArray();
                for (const auto &project : projectsValue) {
                    for (const auto &manifest : project.toObject().value(QStringLiteral("source_manifests")).toArray()) {
                        const QString path = manifest.toObject().value(QStringLiteral("path")).toString();
                        if (SourceOwner::isManifestPath(path) && SourceOwner::defaultFolderPrefixLength(path) == SourceOwner::DefaultFolderLength) {
                            return SourceOwner::DefaultSegment;
                        }
                    }
                }
            }
        }
        return SourceOwner::LegacyDefaultSegment;
    }
    // The existing default folder of a Project hash, relative to root, or empty.
    static QString existing(const QString &root, const QString &hash)
    {
        for (const QLatin1StringView segment : {SourceOwner::DefaultSegment, SourceOwner::LegacyDefaultSegment}) {
            const QString folder = SourceOwner::defaultFolder(hash, segment);
            if (QFileInfo(QDir(root).filePath(folder)).isDir()) {
                return folder;
            }
        }
        return {};
    }
    // The default folder of a Project hash relative to root: the one on disk,
    // or where a new one goes.
    static QString of(const QString &root, const QString &hash)
    {
        const QString found = existing(root, hash);
        return found.isEmpty() ? SourceOwner::defaultFolder(hash, librarySegment(root)) : found;
    }
    static QString ofProject(const QString &root, const QString &projectId)
    {
        return of(root, SourceOwner::projectHash(projectId));
    }
    // The Project manifest files under root: <hash>/default first, then
    // <hash>/documents, by directory name.
    static QStringList manifestPaths(const QString &root)
    {
        QStringList result;
        const QDir projects(projectsDirectory(root));
        for (const QString &name : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            for (const QLatin1StringView segment : {SourceOwner::DefaultSegment, SourceOwner::LegacyDefaultSegment}) {
                const QString candidate = projects.filePath(name + QLatin1Char('/') + segment + QStringLiteral("/thoughts-project.json"));
                if (QFileInfo::exists(candidate)) {
                    result.append(candidate);
                    break;
                }
            }
        }
        return result;
    }
};
}
