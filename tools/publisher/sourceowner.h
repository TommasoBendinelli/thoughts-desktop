// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../../part/sourceowner.h"
#include "pyfs.h"

#include <QPair>

// Source owners (spec Rule 1), shared with the desktop: a docs-* folder or a
// Project's default folder .thoughts/projects/<hex>/documents.
namespace Thoughts::Publisher
{

using Thoughts::SourceOwner;

// The docs root and owner of a source folder. A default folder's owner spans
// its last four segments; a connected folder's is its name.
inline QPair<Py::Path, QString> folderOwner(const Py::Path &folder)
{
    const QStringList &segments = folder.segments();
    if (segments.size() >= 4) {
        const QString owner = segments.mid(segments.size() - 4).join(QLatin1Char('/'));
        if (SourceOwner::isDefaultFolder(owner)) {
            Py::Path root = folder;
            for (int index = 0; index < 4; ++index) {
                root = root.parent();
            }
            return {root, owner};
        }
    }
    return {folder.parent(), folder.name()};
}

inline bool isDefaultFolder(const Py::Path &folder)
{
    return SourceOwner::isDefaultFolder(folderOwner(folder).second);
}

}
