// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyerror.h"
#include "pyfs.h"

#include <QString>

// Identifiers the server chooses and the publisher turns into file or URL
// paths. The server is not trusted to keep them harmless: a conflict id or
// an object digest such as "../../x" or "/etc/x" would name a file outside
// the publisher's state folder. Only the exact shapes the protocol defines
// are accepted; anything else stops the run with an error.
namespace Thoughts::Publisher
{

inline bool isLowerHex(QStringView text)
{
    for (const QChar character : text) {
        const char16_t unit = character.unicode();
        if (!((unit >= u'0' && unit <= u'9') || (unit >= u'a' && unit <= u'f'))) {
            return false;
        }
    }
    return true;
}

// A conflict id is a UUID in its usual form: 8-4-4-4-12 hexadecimal digits.
inline bool isConflictId(const QString &value)
{
    if (value.size() != 36) {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        const char16_t unit = value.at(index).unicode();
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (unit != u'-') {
                return false;
            }
        } else if (!((unit >= u'0' && unit <= u'9') || (unit >= u'a' && unit <= u'f') || (unit >= u'A' && unit <= u'F'))) {
            return false;
        }
    }
    return true;
}

// An object digest is a SHA-256 as 64 lower-case hexadecimal digits.
inline bool isObjectDigest(const QString &value)
{
    return value.size() == 64 && isLowerHex(value);
}

// The value itself, or a ValueError that says what was wrong. The text of an
// invalid value is not repeated in full: it may be long or misleading.
inline QString requireConflictId(const QString &value)
{
    if (!isConflictId(value)) {
        Py::raiseValueError(QStringLiteral("the server sent a conflict id that is not a UUID (%1 characters); nothing was written").arg(value.size()));
    }
    return value;
}

inline QString requireObjectDigest(const QString &value)
{
    if (!isObjectDigest(value)) {
        Py::raiseValueError(QStringLiteral("the server sent an object digest that is not 64 lower-case hexadecimal digits (%1 characters); nothing was read or written").arg(value.size()));
    }
    return value;
}

// <state folder>/source-conflicts/<conflict id>/<source path>.local: the
// local text kept when a conflict is settled with the server's version.
inline Py::Path conflictCopyPath(const Py::Path &stateFolder, const QString &conflictId, const QString &sourcePath)
{
    return (stateFolder / QStringLiteral("source-conflicts")).child(requireConflictId(conflictId)) / (sourcePath + QStringLiteral(".local"));
}

// <state folder>/source-bundle/objects/<digest>: an object Sync already holds.
inline Py::Path cachedObjectPath(const Py::Path &stateFolder, const QString &objectDigest)
{
    return (stateFolder / QStringLiteral("source-bundle") / QStringLiteral("objects")).child(requireObjectDigest(objectDigest));
}

}
