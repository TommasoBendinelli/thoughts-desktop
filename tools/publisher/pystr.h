// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <string>

// Python's str methods where Qt differs: Python works on code points, folds
// case fully (ß is ss), and its whitespace, line breaks and repr() follow
// their own tables. The publisher compares, sorts and reports names exactly
// as the Python publisher did, so it uses these instead of QString's.
namespace Thoughts::Py
{

std::u32string toCodePoints(const QString &text);
QString fromCodePoints(const std::u32string &codePoints);
// len(text)
qsizetype codePointCount(const QString &text);
// text[:count]
QString leftCodePoints(const QString &text, qsizetype count);

QString casefold(const QString &text);
QString lower(const QString &text);
bool isSpace(char32_t codePoint);
bool isPrintable(char32_t codePoint);

// str.strip() and str.strip(characters)
QString strip(const QString &text);
QString strip(const QString &text, const QString &characters);
// str.splitlines()
QStringList splitLines(const QString &text);
// Universal newlines as open(..., "r") reads text: \r\n and \r become \n.
QString universalNewlines(const QString &text);
// repr(str)
QString repr(const QString &text);

// PurePosixPath(path).name / .suffix / .stem (Python 3.14 rules).
QString posixName(const QString &path);
QString posixSuffix(const QString &path);
QString posixStem(const QString &path);
// The suffix of a bare file name (no directories).
QString nameSuffix(const QString &name);
QString nameStem(const QString &name);

// re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*\.pdf", text) and friends.
bool isAsciiWordChar(QChar character); // [A-Za-z0-9_.-]
bool isSafePdfFilename(const QString &text);
bool isConnectedFolderName(const QString &text); // docs-[A-Za-z0-9_.-]+
bool isLowerHex(const QString &text, qsizetype length);

}
