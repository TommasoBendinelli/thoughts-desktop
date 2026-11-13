// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "projectdocuments.h"

// Which files of the docs-* folders Sync manages (the source manifest), as
// thoughts_server.source_agent discovers them.
namespace Thoughts::Publisher
{

constexpr qint64 maximumManagedSourceBytes = 4 * 1024 * 1024;

// _real_project_path: path is inside project with no linked part.
bool isRealProjectPath(const Py::Path &project, const Py::Path &path);
// _project_tex_roots: tex/ and each LaTeX working directory's tex/.
QList<Py::Path> projectTexRoots(const Py::Path &project);
// _latex_folder_input: a file outside the tex/ trees that an entry point
// declared there may input.
bool latexFolderInput(const Py::Path &project, const QString &relative);
// is_authored_source_path: whether discovery would list this folder-relative
// file once it exists (a file another device added).
bool isAuthoredSourcePath(const Py::Path &project, const QString &relative);
// The folders whose sources the manifest lists: each Project's connected
// folders and its default folder once it holds sources (_source_manifest_payload).
QSet<QString> sourceOwners(const Py::Path &docsRoot);
// Whether Sync manages the docs-root-relative `path` once it exists: its
// owner is one of `owners` and discovery lists it (isAuthoredSourcePath), so
// not tex/generated/, nothing .ThoughtIgnore excludes. Raises like discovery.
bool isManagedSourcePath(const Py::Path &docsRoot, const QSet<QString> &owners, const QString &path);
// _project_authored_source_entries: [{"path", "role"}] sorted by path.
Json authoredSourceEntries(const Py::Path &docsRoot, const Py::Path &project);
// _source_manifest_payload: the schema 3 manifest of every installed folder.
Json sourceManifestPayload(const Py::Path &docsRoot);
// _manifest_shared_files: validates shared_files, raising like Python.
QList<Json> manifestSharedFiles(const Json &payload);
// _manifest_payload_paths(payload): validates, then the distinct path values.
QList<Json> manifestPayloadPaths(const Json &payload);
qsizetype manifestPayloadPathCount(const Json &payload);

}
