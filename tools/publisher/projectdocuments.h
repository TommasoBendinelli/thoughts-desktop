// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "projectconfig.h"

#include <QList>
#include <QSet>
#include <QString>

#include <optional>

// The documents a connected folder declares (thoughts_server.project_documents)
// and the Library PDF name each one builds.
namespace Thoughts::Publisher
{

struct ProjectDocument {
    QString source;
    QString format;
    QString identity;
    QString output;
    std::optional<QString> stylesheet;
    QString workingDirectory;
    QString documentId;
};

// The selected LaTeX and Markdown documents of project (a docs-* folder or a
// Project's default folder).
// Raises "Missing current Project manifest for <name>" without a
// configuration, and RenderError for an invalid declaration.
QList<ProjectDocument> projectDocuments(const Py::Path &project, bool allowMissingInputs = false);
// document_output(owner, entry)
QString documentOutput(const QString &owner, const Json &entry);
// component_output(project, source)
QString componentOutput(const QString &project, const QString &source);
// A LaTeX document's compiler directory relative to its folder: the declared
// working_directory, else "." (a default folder's document: its own directory).
QString latexWorkingDirectory(const QString &owner, const QString &source, const Json &options);
// The Markdown inputs and assets of a folder, sorted, honouring .ThoughtIgnore.
QList<Py::Path> markdownInputFiles(const Py::Path &project);
// LATEX_ASSET_SUFFIXES
extern const QSet<QString> latexAssetSuffixes;
// latex_folder_path(relative): no hidden or generated part.
bool latexFolderPath(const QString &relative);
// latex_folder_sources(project): a LaTeX entry point is declared outside the tex/ trees.
bool latexFolderSources(const Py::Path &project);
// latex_folder_files(project, suffixes): sorted, honouring .ThoughtIgnore.
QList<Py::Path> latexFolderFiles(const Py::Path &project, const QSet<QString> &suffixes);

}
