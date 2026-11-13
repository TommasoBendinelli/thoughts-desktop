// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"
#include "pyjson.h"

#include <QSet>
#include <QString>

// The source-manifest subcommands of thoughts-publish-sources. Each raises
// Py::Exception where the Python publisher raised.
namespace Thoughts::Publisher
{

// generate-manifest: writes the manifest of every installed folder; returns
// the number of managed paths.
qsizetype generateManifest(const Py::Path &docsRoot, const Py::Path &output);
// refresh-project-manifest: re-inventories one project of an existing
// manifest, keeping everything else as it was.
qsizetype refreshProjectManifest(const Py::Path &docsRoot, const Py::Path &manifest, const QString &project, const Py::Path &output);
// migrate-manifest: the summary object it prints.
Py::Json migrateSourceManifest(const Py::Path &docsRoot, const Py::Path &manifest);
// list-candidates: [{"project", "target"}] of unregistered folders.
Py::Json sourceProjectCandidates(const Py::Path &docsRoot);

// Library policy and the PDF index, shared with the publication commands.
QSet<QString> excludedProjects(const Py::Path &docsRoot);
Py::Json sourceIndex(const Py::Path &docsRoot);
// _validate_source_project: returns the resolved folder.
Py::Path validateSourceProject(const Py::Path &docsRoot, const QString &project, const QString &target, bool allowMissingDocumentInputs = false);

}
