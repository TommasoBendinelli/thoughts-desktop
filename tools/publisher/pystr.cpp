// SPDX-License-Identifier: GPL-2.0-or-later
#include "pystr.h"

#include <algorithm>

namespace Thoughts::Py
{
namespace
{
struct CaseMapping {
    char32_t codePoint;
    int length;
    char32_t mapped[3];
};
struct CodeRange {
    char32_t first;
    char32_t last;
};
#include "unicode_table.inc"

template<size_t N>
const CaseMapping *findMapping(const CaseMapping (&table)[N], char32_t codePoint)
{
    const auto end = table + N;
    const auto found = std::lower_bound(table, end, codePoint, [](const CaseMapping &entry, char32_t value) {
        return entry.codePoint < value;
    });
    return found != end && found->codePoint == codePoint ? found : nullptr;
}

template<size_t N>
bool inRanges(const CodeRange (&table)[N], char32_t codePoint)
{
    const auto end = table + N;
    const auto found = std::upper_bound(table, end, codePoint, [](char32_t value, const CodeRange &range) {
        return value < range.first;
    });
    return found != table && codePoint <= (found - 1)->last;
}

template<size_t N>
QString mapCase(const CaseMapping (&table)[N], const QString &text)
{
    std::u32string result;
    for (const char32_t codePoint : toCodePoints(text)) {
        if (const CaseMapping *mapping = findMapping(table, codePoint)) {
            result.append(mapping->mapped, mapping->length);
        } else {
            result.push_back(codePoint);
        }
    }
    return fromCodePoints(result);
}

bool isLineBreak(char32_t codePoint)
{
    switch (codePoint) {
    case U'\n':
    case U'\r':
    case 0x0B:
    case 0x0C:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x85:
    case 0x2028:
    case 0x2029:
        return true;
    default:
        return false;
    }
}

QString hex(char32_t value, int width)
{
    return QString::number(uint(value), 16).rightJustified(width, QLatin1Char('0'));
}
}

std::u32string toCodePoints(const QString &text)
{
    std::u32string result;
    result.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const char16_t unit = text.at(index).unicode();
        if (QChar::isHighSurrogate(unit) && index + 1 < text.size() && QChar::isLowSurrogate(text.at(index + 1).unicode())) {
            result.push_back(QChar::surrogateToUcs4(unit, text.at(index + 1).unicode()));
            ++index;
        } else {
            // Lone surrogates stay lone code points, as in Python.
            result.push_back(unit);
        }
    }
    return result;
}

QString fromCodePoints(const std::u32string &codePoints)
{
    QString result;
    result.reserve(qsizetype(codePoints.size()));
    for (const char32_t codePoint : codePoints) {
        if (codePoint > 0xFFFF) {
            result.append(QChar(QChar::highSurrogate(codePoint)));
            result.append(QChar(QChar::lowSurrogate(codePoint)));
        } else {
            result.append(QChar(char16_t(codePoint)));
        }
    }
    return result;
}

qsizetype codePointCount(const QString &text)
{
    return qsizetype(toCodePoints(text).size());
}

QString leftCodePoints(const QString &text, qsizetype count)
{
    const std::u32string codePoints = toCodePoints(text);
    if (qsizetype(codePoints.size()) <= count) {
        return text;
    }
    return fromCodePoints(codePoints.substr(0, size_t(count)));
}

QString casefold(const QString &text)
{
    return mapCase(caseFoldEntries, text);
}

QString lower(const QString &text)
{
    return mapCase(lowerEntries, text);
}

bool isSpace(char32_t codePoint)
{
    return inRanges(spaceRanges, codePoint);
}

bool isPrintable(char32_t codePoint)
{
    return inRanges(printableRanges, codePoint);
}

QString strip(const QString &text)
{
    const std::u32string codePoints = toCodePoints(text);
    size_t first = 0;
    size_t last = codePoints.size();
    while (first < last && isSpace(codePoints[first])) {
        ++first;
    }
    while (last > first && isSpace(codePoints[last - 1])) {
        --last;
    }
    return fromCodePoints(codePoints.substr(first, last - first));
}

QString strip(const QString &text, const QString &characters)
{
    const std::u32string set = toCodePoints(characters);
    const auto member = [&set](char32_t codePoint) {
        return set.find(codePoint) != std::u32string::npos;
    };
    const std::u32string codePoints = toCodePoints(text);
    size_t first = 0;
    size_t last = codePoints.size();
    while (first < last && member(codePoints[first])) {
        ++first;
    }
    while (last > first && member(codePoints[last - 1])) {
        --last;
    }
    return fromCodePoints(codePoints.substr(first, last - first));
}

QStringList splitLines(const QString &text)
{
    QStringList lines;
    const std::u32string codePoints = toCodePoints(text);
    size_t start = 0;
    size_t index = 0;
    while (index < codePoints.size()) {
        if (!isLineBreak(codePoints[index])) {
            ++index;
            continue;
        }
        lines.append(fromCodePoints(codePoints.substr(start, index - start)));
        if (codePoints[index] == U'\r' && index + 1 < codePoints.size() && codePoints[index + 1] == U'\n') {
            ++index;
        }
        ++index;
        start = index;
    }
    if (start < codePoints.size()) {
        lines.append(fromCodePoints(codePoints.substr(start)));
    }
    return lines;
}

QString universalNewlines(const QString &text)
{
    QString result = text;
    result.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    result.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return result;
}

QString repr(const QString &text)
{
    const QChar quote = text.contains(QLatin1Char('\'')) && !text.contains(QLatin1Char('"')) ? QLatin1Char('"') : QLatin1Char('\'');
    QString result(quote);
    for (const char32_t codePoint : toCodePoints(text)) {
        if (codePoint == quote.unicode() || codePoint == U'\\') {
            result += QLatin1Char('\\');
            result += QChar(char16_t(codePoint));
        } else if (codePoint == U'\t') {
            result += QStringLiteral("\\t");
        } else if (codePoint == U'\n') {
            result += QStringLiteral("\\n");
        } else if (codePoint == U'\r') {
            result += QStringLiteral("\\r");
        } else if (codePoint < 0x20 || codePoint == 0x7F) {
            result += QStringLiteral("\\x") + hex(codePoint, 2);
        } else if (codePoint < 0x7F) {
            result += QChar(char16_t(codePoint));
        } else if (isPrintable(codePoint)) {
            result += fromCodePoints(std::u32string(1, codePoint));
        } else if (codePoint <= 0xFF) {
            result += QStringLiteral("\\x") + hex(codePoint, 2);
        } else if (codePoint <= 0xFFFF) {
            result += QStringLiteral("\\u") + hex(codePoint, 4);
        } else {
            result += QStringLiteral("\\U") + hex(codePoint, 8);
        }
    }
    result += quote;
    return result;
}

QString posixName(const QString &path)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (auto it = parts.crbegin(); it != parts.crend(); ++it) {
        if (*it != QLatin1String(".")) {
            return *it;
        }
    }
    return {};
}

QString nameSuffix(const QString &name)
{
    qsizetype first = 0;
    while (first < name.size() && name.at(first) == QLatin1Char('.')) {
        ++first;
    }
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot >= first ? name.mid(dot) : QString();
}

QString nameStem(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot != -1) {
        const QString stem = name.left(dot);
        for (const QChar character : stem) {
            if (character != QLatin1Char('.')) {
                return stem;
            }
        }
    }
    return name;
}

QString posixSuffix(const QString &path)
{
    return nameSuffix(posixName(path));
}

QString posixStem(const QString &path)
{
    return nameStem(posixName(path));
}

bool isAsciiWordChar(QChar character)
{
    const char16_t c = character.unicode();
    return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'_' || c == u'.' || c == u'-';
}

bool isSafePdfFilename(const QString &text)
{
    if (text.size() < 5 || !text.endsWith(QLatin1String(".pdf"))) {
        return false;
    }
    const char16_t first = text.at(0).unicode();
    if (!((first >= u'A' && first <= u'Z') || (first >= u'a' && first <= u'z') || (first >= u'0' && first <= u'9'))) {
        return false;
    }
    return std::all_of(text.cbegin(), text.cend(), isAsciiWordChar);
}

bool isConnectedFolderName(const QString &text)
{
    return text.size() > 5 && text.startsWith(QLatin1String("docs-")) && std::all_of(text.cbegin() + 5, text.cend(), isAsciiWordChar);
}

bool isLowerHex(const QString &text, qsizetype length)
{
    return text.size() == length && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
               return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
           });
}

}
