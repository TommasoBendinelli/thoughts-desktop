// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDir>

namespace Thoughts {
// The Library of this home folder when THOUGHTS_HOME is not set: ~/Thoughts/docs
// on every desktop. Not in Documents: macOS can stall opening files in that
// protected folder, and Windows may redirect it to OneDrive.
inline QString defaultThoughtsHome()
{
#ifdef Q_OS_WIN
    // Many checks compare paths as strings: always use forward slashes and
    // an upper-case drive letter, as Qt's canonical paths do.
    QString root = QDir::cleanPath(QDir::home().filePath(QStringLiteral("Thoughts/docs")));
    if (root.size() >= 2 && root.at(1) == QLatin1Char(':')) {
        root[0] = root.at(0).toUpper();
    }
    return root;
#else
    return QDir::home().filePath(QStringLiteral("Thoughts/docs"));
#endif
}

// The Thoughts documents root. THOUGHTS_HOME selects another Library, for
// example a temporary one in tests.
inline QString thoughtsHome()
{
    const QString configured = qEnvironmentVariable("THOUGHTS_HOME");
    if (configured.isEmpty()) {
        return defaultThoughtsHome();
    }
#ifdef Q_OS_WIN
    QString root = QDir::cleanPath(QDir::fromNativeSeparators(configured));
    if (root.size() >= 2 && root.at(1) == QLatin1Char(':')) {
        root[0] = root.at(0).toUpper();
    }
    return root;
#else
    return configured;
#endif
}
}
