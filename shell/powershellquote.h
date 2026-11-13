// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QChar>
#include <QString>

namespace Thoughts
{

// Whether PowerShell reads this character as a single quote. Besides the
// ASCII apostrophe it accepts the typographic quotes U+2018, U+2019, U+201A
// and U+201B, and each of them ends a single-quoted string.
inline bool isPowerShellSingleQuote(QChar character)
{
    switch (character.unicode()) {
    case 0x0027:
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
        return true;
    default:
        return false;
    }
}

// A PowerShell single-quoted string literal for any text, such as a file
// name the server or a PDF chose. Inside such a literal nothing is expanded
// and two quote characters in a row stand for the second one, so every
// character PowerShell reads as a single quote is written twice.
inline QString powerShellQuote(const QString &value)
{
    QString quoted;
    quoted.reserve(value.size() + 2);
    quoted += QLatin1Char('\'');
    for (const QChar character : value) {
        if (isPowerShellSingleQuote(character)) {
            quoted += character;
        }
        quoted += character;
    }
    quoted += QLatin1Char('\'');
    return quoted;
}

}
