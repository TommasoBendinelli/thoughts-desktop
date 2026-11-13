// SPDX-License-Identifier: GPL-2.0-or-later
#include "mediatypes.h"

#include <QHash>
#include <QStringList>

namespace Thoughts::Publisher
{

namespace
{
struct MediaTypeEntry {
    const char16_t *key;
    const char16_t *value;
};
#include "mediatypes_table.inc"

QHash<QString, QString> table(const MediaTypeEntry *entries, size_t count)
{
    QHash<QString, QString> result;
    for (size_t index = 0; index < count; ++index) {
        result.insert(QString::fromUtf16(entries[index].key), QString::fromUtf16(entries[index].value));
    }
    return result;
}

const QHash<QString, QString> &types()
{
    static const auto value = table(mediaTypeEntries, std::size(mediaTypeEntries));
    return value;
}

const QHash<QString, QString> &suffixMap()
{
    static const auto value = table(suffixMapEntries, std::size(suffixMapEntries));
    return value;
}

const QHash<QString, QString> &encodings()
{
    static const auto value = table(encodingEntries, std::size(encodingEntries));
    return value;
}

// posixpath.splitext: leading dots of the last component do not start an
// extension.
QPair<QString, QString> splitext(const QString &path)
{
    const qsizetype separator = path.lastIndexOf(QLatin1Char('/'));
    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot > separator) {
        for (qsizetype index = separator + 1; index < dot; ++index) {
            if (path.at(index) != QLatin1Char('.')) {
                return {path.left(dot), path.mid(dot)};
            }
        }
    }
    return {path, QString()};
}

QString guessFileType(const QString &path)
{
    auto [base, extension] = splitext(path);
    while (suffixMap().contains(extension.toLower())) {
        std::tie(base, extension) = splitext(base + suffixMap().value(extension.toLower()));
    }
    // Encoding suffixes are case sensitive.
    if (encodings().contains(extension)) {
        std::tie(base, extension) = splitext(base);
    }
    return types().value(extension.toLower());
}

const QStringList &paramSchemes()
{
    // urllib.parse.uses_params
    static const QStringList schemes {QString(), QStringLiteral("ftp"), QStringLiteral("hdl"), QStringLiteral("prospero"), QStringLiteral("http"),
                                      QStringLiteral("imap"), QStringLiteral("https"), QStringLiteral("shttp"), QStringLiteral("rtsp"), QStringLiteral("rtspu"),
                                      QStringLiteral("sip"), QStringLiteral("sips"), QStringLiteral("mms"), QStringLiteral("sftp"), QStringLiteral("tel")};
    return schemes;
}
}

QString guessType(const QString &name)
{
    // urllib.parse.urlparse: a scheme is letters, digits, "+-." before the
    // first ":", starting with an ASCII letter.
    const qsizetype colon = name.indexOf(QLatin1Char(':'));
    QString scheme;
    QString rest = name;
    if (colon > 0 && name.at(0).isLetter() && name.at(0).unicode() < 0x80) {
        bool valid = true;
        for (qsizetype index = 0; index < colon && valid; ++index) {
            const QChar c = name.at(index);
            valid = c.unicode() < 0x80 && (c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('-') || c == QLatin1Char('.'));
        }
        if (valid) {
            scheme = name.left(colon).toLower();
            rest = name.mid(colon + 1);
        }
    }
    if (scheme.size() <= 1) {
        // guess_file_type(); macOS has no drive letters to split off.
        return guessFileType(name);
    }
    // urlsplit: netloc after "//", then fragment and query; urlparse then
    // splits ";params" off the last path segment for some schemes.
    QString path = rest;
    if (path.startsWith(QLatin1String("//"))) {
        qsizetype end = path.size();
        for (const QChar delimiter : {QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#')}) {
            const qsizetype found = path.indexOf(delimiter, 2);
            if (found >= 0 && found < end) {
                end = found;
            }
        }
        path = path.mid(end);
    }
    if (const qsizetype fragment = path.indexOf(QLatin1Char('#')); fragment >= 0) {
        path.truncate(fragment);
    }
    if (const qsizetype query = path.indexOf(QLatin1Char('?')); query >= 0) {
        path.truncate(query);
    }
    if (paramSchemes().contains(scheme) && path.contains(QLatin1Char(';'))) {
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        const qsizetype semicolon = path.indexOf(QLatin1Char(';'), slash < 0 ? 0 : slash);
        if (semicolon >= 0) {
            path.truncate(semicolon);
        }
    }
    if (scheme == QLatin1String("data")) {
        const qsizetype comma = path.indexOf(QLatin1Char(','));
        if (comma < 0) {
            return {};
        }
        const qsizetype semicolon = path.left(comma).indexOf(QLatin1Char(';'));
        QString type = semicolon >= 0 ? path.left(semicolon) : path.left(comma);
        if (type.contains(QLatin1Char('=')) || !type.contains(QLatin1Char('/'))) {
            type = QStringLiteral("text/plain");
        }
        return type;
    }
    return guessFileType(path);
}

QString guessMediaType(const QString &name)
{
    const QString type = guessType(name);
    return type.isEmpty() ? QStringLiteral("application/octet-stream") : type;
}

}
