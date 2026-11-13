// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QString>

namespace Thoughts {

// Library refresh reads filesystem metadata only. Publication dates for PDFs
// are supplied by the Library catalogue, not inferred from PDF contents.
inline QHash<QString, QDateTime> libraryModificationDates(const QString &root, const QHash<QString, bool> &paths,
                                                         bool *incomplete = nullptr, int budgetMs = 5000)
{
    Q_UNUSED(root)
    Q_UNUSED(budgetMs)
    if (incomplete) *incomplete = false;
    QHash<QString, QDateTime> result;
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        if (it.value()) continue;
        const QFileInfo info(it.key());
        if (info.isFile()) result.insert(it.key(), info.lastModified());
    }
    return result;
}
}
