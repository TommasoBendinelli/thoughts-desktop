// SPDX-License-Identifier: GPL-2.0-or-later
#include "managedsource.h"

#include "pyerror.h"
#include "pyfs.h"
#include "sourceinventory.h"

namespace Thoughts::Publisher
{

std::optional<bool> isManagedLibrarySource(const QString &docsRoot, const QString &path)
{
    try {
        const Py::Path root(docsRoot);
        return isManagedSourcePath(root, sourceOwners(root), path);
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
}

std::optional<QStringList> discoveredFolderSources(const QString &docsRoot, const QString &owner)
{
    try {
        // As sourceManifestPayload: paths relative to the resolved docs root.
        const Py::Path root = Py::resolve(Py::Path(docsRoot));
        QStringList result;
        for (const Json &entry : authoredSourceEntries(root, root / owner).items()) {
            result.append(entry.value(QStringLiteral("path")).toString());
        }
        return result;
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
}

}
