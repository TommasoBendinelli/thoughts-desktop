// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace Thoughts
{
// One standard name for connected source folders (spec
// thoughts-source-folder-names-SPEC.md, 2026-10-27): every folder a Project
// manifest declares is called docs-<slug>-<hex>. Fedora computes the map of
// renames once (rule N) and serves it as `source_folder_renames` in
// library-projects; clients never compute it, they apply it with rule P.
// Shared with tools/publisher; the server and Android implement the same
// rules against the same test vectors.
struct SourceFolderRename {
    QString folderId;
    QString oldName;
    QString newName;
    bool operator==(const SourceFolderRename &other) const = default;
};
using SourceFolderRenames = QList<SourceFolderRename>;

struct SourceFolderNames {
    static constexpr QLatin1StringView Prefix{"docs-"};
    static constexpr QLatin1StringView ImportPrefix{"docs-import-"};
    static constexpr qsizetype SlugLimit = 32;
    static constexpr qsizetype HexLength = 6;

    // [A-Za-z0-9_.-]: the characters a folder name token is made of.
    static constexpr bool isTokenCharacter(char32_t c)
    {
        return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9') || c == U'_' || c == U'.' || c == U'-';
    }
    // ^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$, the strictest folder validator.
    static bool isStrictFolderName(const QString &name)
    {
        static const QRegularExpression pattern(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$"));
        return pattern.match(name).hasMatch();
    }
    static bool isLowerHex(QStringView value)
    {
        return !value.isEmpty() && std::all_of(value.cbegin(), value.cend(), [](QChar c) {
            return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        });
    }
    // ^docs-[a-z0-9]+(-[a-z0-9]+)*-[0-9a-f]{6,32}$, except docs-import-<32 hex>:
    // a name that follows the standard.
    static bool isRuleNName(const QString &name)
    {
        static const QRegularExpression pattern(QStringLiteral("^docs-[a-z0-9]+(?:-[a-z0-9]+)*-[0-9a-f]{6,32}$"));
        return pattern.match(name).hasMatch() && !isLegacyImportFolder(name);
    }
    // docs-import-<32 lowercase hex>: the name Import sources used before.
    static bool isLegacyImportFolder(const QString &name)
    {
        return name.size() == ImportPrefix.size() + 32 && name.startsWith(ImportPrefix) && isLowerHex(QStringView(name).mid(ImportPrefix.size()));
    }

    // ---- Rule P ------------------------------------------------------------
    // Replace each `old` of the map that is a whole token: the character
    // before is the start or not in [A-Za-z0-9_.-], the character after is the
    // end or not in that set, and it does not directly follow the literal
    // "source:" (a legacy Project id such as source:docs-concepts, amendment
    // of 2026-10-27). Case-sensitive, one pass, longest old first, output
    // never rescanned.
    template<typename Text, typename At>
    static Text rewriteTokens(const Text &text, const SourceFolderRenames &renames, At at, qsizetype *count = nullptr)
    {
        if (renames.isEmpty() || text.isEmpty()) {
            return text;
        }
        SourceFolderRenames ordered;
        for (const SourceFolderRename &rename : renames) {
            if (!rename.oldName.isEmpty() && rename.oldName != rename.newName) {
                ordered.append(rename);
            }
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const SourceFolderRename &a, const SourceFolderRename &b) {
            return a.oldName.size() > b.oldName.size();
        });
        if (ordered.isEmpty()) {
            return text;
        }
        Text result;
        qsizetype copied = 0;
        qsizetype replaced = 0;
        const qsizetype size = text.size();
        static constexpr char projectIdPrefix[] = "source:";
        static constexpr qsizetype projectIdPrefixLength = sizeof(projectIdPrefix) - 1;
        for (qsizetype i = 0; i < size; ++i) {
            if (i > 0 && isTokenCharacter(at(text, i - 1))) {
                continue;
            }
            // "source:docs-x" is a legacy Project identifier (its default
            // folder is sha256 of it): never rewritten. "project:docs-x" is.
            if (i >= projectIdPrefixLength) {
                bool projectId = true;
                for (qsizetype j = 0; j < projectIdPrefixLength && projectId; ++j) {
                    projectId = at(text, i - projectIdPrefixLength + j) == char32_t(projectIdPrefix[j]);
                }
                if (projectId) {
                    continue;
                }
            }
            for (const SourceFolderRename &rename : std::as_const(ordered)) {
                const qsizetype length = rename.oldName.size();
                if (i + length > size) {
                    continue;
                }
                bool equal = true;
                for (qsizetype j = 0; j < length && equal; ++j) {
                    equal = at(text, i + j) == char32_t(rename.oldName.at(j).unicode());
                }
                if (!equal || (i + length < size && isTokenCharacter(at(text, i + length)))) {
                    continue;
                }
                result += text.mid(copied, i - copied);
                if constexpr (std::is_same_v<Text, QByteArray>) {
                    result += rename.newName.toUtf8();
                } else {
                    result += rename.newName;
                }
                copied = i + length;
                ++replaced;
                // The replaced token ends at a boundary; continue after it.
                i = copied - 1;
                break;
            }
        }
        if (count) {
            *count += replaced;
        }
        if (!replaced) {
            return text;
        }
        result += text.mid(copied);
        return result;
    }
    static QString rewrite(const QString &text, const SourceFolderRenames &renames, qsizetype *count = nullptr)
    {
        // Names are ASCII, so comparing UTF-16 units is exact; a surrogate is never a token character.
        return rewriteTokens(text, renames, [](const QString &value, qsizetype index) -> char32_t {
            return value.at(index).unicode();
        }, count);
    }
    // The same on UTF-8 bytes: every non-ASCII byte is a boundary, as its character is.
    static QByteArray rewrite(const QByteArray &bytes, const SourceFolderRenames &renames, qsizetype *count = nullptr)
    {
        return rewriteTokens(bytes, renames, [](const QByteArray &value, qsizetype index) -> char32_t {
            return static_cast<unsigned char>(value.at(index));
        }, count);
    }
    // Whether rule P changes anything (or names any `old` at all).
    static bool mentionsOld(const QString &text, const SourceFolderRenames &renames)
    {
        qsizetype count = 0;
        rewrite(text, renames, &count);
        return count > 0;
    }
    static bool mentionsOld(const QByteArray &bytes, const SourceFolderRenames &renames)
    {
        qsizetype count = 0;
        rewrite(bytes, renames, &count);
        return count > 0;
    }

    // Free text keeps its bytes: multi-line strings and `contents` /
    // `*_contents` fields (rule P is for state and metadata).
    static bool isContentsKey(QStringView key)
    {
        return key == u"contents" || key.endsWith(u"_contents");
    }
    static bool isMultiLine(QStringView text)
    {
        return text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'));
    }
    // Rule P on a JSON value: object keys and single-line strings, never
    // inside a contents field.
    static QJsonValue rewriteJson(const QJsonValue &value, const SourceFolderRenames &renames, qsizetype *count = nullptr)
    {
        if (value.isString()) {
            const QString text = value.toString();
            return isMultiLine(text) ? value : QJsonValue(rewrite(text, renames, count));
        }
        if (value.isArray()) {
            QJsonArray array = value.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) {
                array[i] = rewriteJson(array.at(i), renames, count);
            }
            return array;
        }
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            QJsonObject result;
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                const QString key = isMultiLine(it.key()) ? it.key() : rewrite(it.key(), renames, count);
                result.insert(key, isContentsKey(it.key()) ? it.value() : rewriteJson(it.value(), renames, count));
            }
            return result;
        }
        return value;
    }
    static QJsonObject rewriteJson(const QJsonObject &value, const SourceFolderRenames &renames, qsizetype *count = nullptr)
    {
        return rewriteJson(QJsonValue(value), renames, count).toObject();
    }
    static QJsonArray rewriteJson(const QJsonArray &value, const SourceFolderRenames &renames, qsizetype *count = nullptr)
    {
        return rewriteJson(QJsonValue(value), renames, count).toArray();
    }

    // Rule P on JSON or JSON Lines text, keeping every other byte: each string
    // token is rewritten in place (keys too), except multi-line strings and
    // anything inside a contents field. Bytes outside strings never change.
    // skippedKeys are kept like contents fields (the migration keeps the map
    // itself, source_folder_renames, as Fedora sent it).
    static QByteArray rewriteJsonText(const QByteArray &bytes, const SourceFolderRenames &renames, qsizetype *count = nullptr, const QStringList &skippedKeys = {})
    {
        struct Frame {
            bool object = false;
            bool expectKey = false;
            bool skip = false;
            QString key;
        };
        QList<Frame> stack;
        QByteArray result;
        qsizetype copied = 0;
        qsizetype replaced = 0;
        const qsizetype size = bytes.size();
        const auto valueSkipped = [&stack, &skippedKeys]() {
            if (stack.isEmpty()) {
                return false;
            }
            const Frame &top = stack.constLast();
            return top.skip || (top.object && !top.expectKey && (isContentsKey(top.key) || skippedKeys.contains(top.key)));
        };
        for (qsizetype i = 0; i < size; ++i) {
            const char c = bytes.at(i);
            if (c == '"') {
                qsizetype end = i + 1;
                bool escaped = false;
                bool special = false; // an escape other than \" \\ \/
                while (end < size) {
                    const char d = bytes.at(end);
                    if (escaped) {
                        if (d != '"' && d != '\\' && d != '/') {
                            special = true;
                        }
                        escaped = false;
                    } else if (d == '\\') {
                        escaped = true;
                    } else if (d == '"') {
                        break;
                    }
                    ++end;
                }
                if (end >= size) {
                    break; // unterminated: leave the rest
                }
                const bool isKey = !stack.isEmpty() && stack.constLast().object && stack.constLast().expectKey;
                const QByteArray raw = bytes.mid(i + 1, end - i - 1);
                const bool skip = isKey ? (!stack.isEmpty() && stack.constLast().skip) : valueSkipped();
                QString decoded;
                if (special || isKey) {
                    decoded = QJsonDocument::fromJson("[\"" + raw + "\"]").array().at(0).toString();
                }
                if (isKey) {
                    stack.last().key = decoded;
                }
                if (!skip && !(special && isMultiLine(decoded))) {
                    qsizetype local = 0;
                    QByteArray rewritten;
                    if (special) {
                        // Rewrite the decoded text and encode it again the way Qt does.
                        const QString changed = rewrite(decoded, renames, &local);
                        if (local) {
                            const QByteArray encoded = QJsonDocument(QJsonArray{changed}).toJson(QJsonDocument::Compact);
                            rewritten = encoded.mid(2, encoded.size() - 4);
                        }
                    } else {
                        rewritten = rewrite(raw, renames, &local);
                    }
                    if (local) {
                        result += bytes.mid(copied, i + 1 - copied);
                        result += rewritten;
                        copied = end;
                        replaced += local;
                    }
                }
                i = end;
                continue;
            }
            switch (c) {
            case '{':
                stack.append(Frame{true, true, valueSkipped(), {}});
                break;
            case '[':
                stack.append(Frame{false, false, valueSkipped(), {}});
                break;
            case '}':
            case ']':
                if (!stack.isEmpty()) {
                    stack.removeLast();
                }
                break;
            case ':':
                if (!stack.isEmpty() && stack.constLast().object) {
                    stack.last().expectKey = false;
                }
                break;
            case ',':
                if (!stack.isEmpty() && stack.constLast().object) {
                    stack.last().expectKey = true;
                    stack.last().key.clear();
                }
                break;
            default:
                break;
            }
        }
        if (count) {
            *count += replaced;
        }
        if (!replaced) {
            return bytes;
        }
        result += bytes.mid(copied);
        return result;
    }

    // ---- Rule N ------------------------------------------------------------
    // The slug of a display name (fallback: the old folder name).
    static QString slug(const QString &displayName, const QString &fallback = {})
    {
        QString text = displayName.trimmed().isEmpty() ? fallback : displayName;
        if (text.startsWith(Prefix, Qt::CaseInsensitive)) {
            text = text.mid(Prefix.size());
        }
        // ASCII letters only: every other character becomes a separator, as in
        // Fedora's Python (no Unicode case mapping: K-sign and dotted I are not k and i).
        QString dashed;
        bool inRun = false;
        for (const QChar character : std::as_const(text)) {
            const char16_t c = character.unicode();
            if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9')) {
                dashed += character;
                inRun = false;
            } else if (c >= u'A' && c <= u'Z') {
                dashed += QChar(char16_t(c - u'A' + u'a'));
                inRun = false;
            } else if (!inRun) {
                dashed += QLatin1Char('-');
                inRun = true;
            }
        }
        const auto trim = [](QString value) {
            while (value.startsWith(QLatin1Char('-'))) {
                value.remove(0, 1);
            }
            while (value.endsWith(QLatin1Char('-'))) {
                value.chop(1);
            }
            return value;
        };
        QString result = trim(dashed);
        if (result.size() > SlugLimit) {
            const QChar next = result.at(SlugLimit);
            result.truncate(SlugLimit);
            if (next != QLatin1Char('-') && result.contains(QLatin1Char('-'))) {
                result.truncate(result.lastIndexOf(QLatin1Char('-')));
            }
            result = trim(result);
        }
        return result.isEmpty() ? QStringLiteral("folder") : result;
    }
    // The 32 lowercase hex digits a folder's name hex comes from: the import
    // hex of docs-import-<32hex>, else folder_id without dashes.
    static QString hexSource(const QString &oldName, const QString &folderId)
    {
        if (isLegacyImportFolder(oldName)) {
            return oldName.mid(ImportPrefix.size());
        }
        QString hex = folderId.toLower();
        hex.remove(QLatin1Char('-'));
        hex.remove(QLatin1Char('{'));
        hex.remove(QLatin1Char('}'));
        return hex;
    }
    // docs-<slug>-<hex>: hex starts at 6 digits and grows by 2 (to 32) while
    // `taken` reports a case-insensitive clash. Empty when every length clashes.
    static QString name(const QString &slugText, const QString &hex, const std::function<bool(const QString &)> &taken)
    {
        for (qsizetype length = HexLength; length <= std::max<qsizetype>(HexLength, hex.size()); length += 2) {
            const QString candidate = Prefix + slugText + QLatin1Char('-') + hex.left(length);
            if (!taken || !taken(candidate.toLower())) {
                return candidate;
            }
            if (length >= hex.size()) {
                break;
            }
        }
        return {};
    }
    // A new import's folder: display name = import name, hex = the operation id's.
    static QString importFolderName(const QString &importName, const QString &operationId, const std::function<bool(const QString &)> &taken)
    {
        const QUuid uuid(operationId);
        if (uuid.isNull()) {
            return {};
        }
        return name(slug(importName, QStringLiteral("folder")), uuid.toString(QUuid::Id128), taken);
    }

    // ---- the map ------------------------------------------------------------
    // library-projects `source_folder_renames`: [{folder_id, old, new}]. Entries
    // that are not two different valid folder names are ignored.
    static SourceFolderRenames fromJson(const QJsonArray &values)
    {
        SourceFolderRenames result;
        QSet<QString> olds;
        for (const QJsonValue &value : values) {
            const QJsonObject entry = value.toObject();
            SourceFolderRename rename{entry.value(QStringLiteral("folder_id")).toString(), entry.value(QStringLiteral("old")).toString(),
                                      entry.value(QStringLiteral("new")).toString()};
            if (!isFolderSegment(rename.oldName) || !isFolderSegment(rename.newName) || rename.oldName == rename.newName || olds.contains(rename.oldName)) {
                continue;
            }
            olds.insert(rename.oldName);
            result.append(rename);
        }
        return result;
    }
    static QJsonArray toJson(const SourceFolderRenames &renames)
    {
        QJsonArray result;
        for (const SourceFolderRename &rename : renames) {
            result.append(QJsonObject{{QStringLiteral("folder_id"), rename.folderId}, {QStringLiteral("old"), rename.oldName}, {QStringLiteral("new"), rename.newName}});
        }
        return result;
    }
    // ^docs-[A-Za-z0-9_.-]+$ without "." / ".." tricks: one path segment.
    static bool isFolderSegment(const QString &name)
    {
        if (name.size() <= Prefix.size() || !name.startsWith(Prefix) || name.contains(QLatin1String(".."))) {
            return false;
        }
        return std::all_of(name.cbegin(), name.cend(), [](QChar c) {
            return isTokenCharacter(c.unicode());
        });
    }
    // The old name of a renamed folder, or the name itself.
    static QString oldNameOf(const QString &folder, const SourceFolderRenames &renames)
    {
        for (const SourceFolderRename &rename : renames) {
            if (rename.newName == folder) {
                return rename.oldName;
            }
        }
        return folder;
    }
    // The Library short name PDFs of a folder are named after (rule F): the
    // short name of the folder's old name for a renamed folder, so published
    // filenames and comment sidecar stems never change.
    static QString pdfShortName(const QString &folder, const SourceFolderRenames &renames)
    {
        const QString name = oldNameOf(folder, renames);
        return name.startsWith(Prefix) ? name.mid(Prefix.size()) : name;
    }

    // The renames this Library knows, cached by file time: Fedora's map as the
    // client catalogue received it (.thoughts/library-projects.json
    // source_folder_renames), or library/source-folder-renames.json in a
    // server worktree ([{folder_id, old, new}] or {"renames": [...]}).
    static SourceFolderRenames knownRenames(const QString &docsRoot)
    {
        if (docsRoot.isEmpty()) {
            return {};
        }
        const QStringList paths{QDir(docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json")),
                                QDir(docsRoot).filePath(QStringLiteral("library/source-folder-renames.json"))};
        QString stamp;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            stamp += info.exists() ? QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char(':') + QString::number(info.size()) : QStringLiteral("-");
            stamp += QLatin1Char('|');
        }
        static QMutex mutex;
        static QHash<QString, QPair<QString, SourceFolderRenames>> cache;
        QMutexLocker locker(&mutex);
        const auto cached = cache.constFind(docsRoot);
        if (cached != cache.cend() && cached->first == stamp) {
            return cached->second;
        }
        SourceFolderRenames result;
        QSet<QString> olds;
        for (const QString &path : paths) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            const QJsonArray values = document.isArray() ? document.array()
                : document.object().contains(QStringLiteral("source_folder_renames")) ? document.object().value(QStringLiteral("source_folder_renames")).toArray()
                                                                                      : document.object().value(QStringLiteral("renames")).toArray();
            for (const SourceFolderRename &rename : fromJson(values)) {
                if (!olds.contains(rename.oldName)) {
                    olds.insert(rename.oldName);
                    result.append(rename);
                }
            }
        }
        cache.insert(docsRoot, {stamp, result});
        return result;
    }
    static QString newNameOf(const QString &folder, const SourceFolderRenames &renames)
    {
        for (const SourceFolderRename &rename : renames) {
            if (rename.oldName == folder) {
                return rename.newName;
            }
        }
        return folder;
    }
};
}
