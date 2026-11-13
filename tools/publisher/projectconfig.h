// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"
#include "pyjson.h"

#include <QList>
#include <QString>

#include <optional>

// Project configurations (thoughts_server.project_configuration): one
// thoughts-project.json per Project in its default folder under
// .thoughts/projects, binding docs-* folders and documents by UUID.
namespace Thoughts::Publisher
{

using Py::Json;
using Py::Path;

// A Project's default folder is .thoughts/projects/<sha256(id)>/default, or
// .../documents before the 2026-10-17 rename; either spelling is valid.
bool isDefaultFolderOf(const QString &location, const QString &projectId);
// The manifest path a validated configuration declares: inside its default folder.
QString manifestPathOf(const Json &value);
bool isManifestPath(const QString &path);

// The configurations under root, sorted as pathlib sorts them (or in
// directory order, as an unsorted glob yields them).
QList<Path> configurationPaths(const Path &root, bool sorted = true);
// validate(value): raises ValueError with Python's message.
void validateConfiguration(const Json &value);
// read(path): symlink and location checks, then validate.
Json readConfiguration(const Path &path);

struct Configuration {
    Path path;
    Json value;
};
// find(root, owner): the configuration that lists owner as a source folder.
std::optional<Configuration> findConfiguration(const Path &root, const QString &owner);
// project_manifest.location(project)
std::optional<Path> configurationLocation(const Path &project);
// selected_source_folders(value): folders with selected LaTeX or Markdown documents.
QStringList selectedSourceFolders(const Json &value);
// projection(value, owner): the per-folder v4 view.
Json projection(const Json &value, const QString &owner);

}
