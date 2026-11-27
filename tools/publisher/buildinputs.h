// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "projectdocuments.h"

#include <QSet>

// Build inputs (thoughts_server.source_agent): the binary files a folder's
// documents compile with, found by location and extension, and the snapshot
// identity Fedora recomputes to accept them.
namespace Thoughts::Publisher
{

struct BuildInput {
    QString path;
    QString sha256;
    qint64 sizeBytes = 0;
    QString mediaType;
    QString role;
    QString localPath;
    // From a frozen bundle: its verified bytes, or a promise that Fedora
    // already holds the object.
    std::optional<QByteArray> frozenContent;
    bool reuseVerified = false;
};

struct SourceTarget {
    QString project;
    QString target;
};

// One selected imported PDF of a Project: its Library filename and its path
// relative to the folder (empty for a legacy third_party/ PDF).
struct ProjectAttachment {
    QString filename;
    QString source;
};
// load_project_attachments(project): the selected imported PDFs of the
// Project that owns folder project, the PDFs its \PdfLink commands can name.
QList<ProjectAttachment> projectAttachments(const Py::Path &project);

// registered_source_project_targets: every selected document of every
// installed folder.
QList<SourceTarget> registeredSourceProjectTargets(const Py::Path &docsRoot);
// While one is alive, registeredSourceProjectTargets reads the Project files
// of a docs root once and answers from that. One publication asks it for
// every PDF it covers, twice (27 PDFs sharing a changed file: 54 scans of
// every Project of the Library, 6 to 16 s on 13 November 2026). Keep one
// alive only across code that does not change Project files; scopes nest.
class RegisteredSourceTargetsScope
{
public:
    RegisteredSourceTargetsScope();
    ~RegisteredSourceTargetsScope();
    RegisteredSourceTargetsScope(const RegisteredSourceTargetsScope &) = delete;
    RegisteredSourceTargetsScope &operator=(const RegisteredSourceTargetsScope &) = delete;
    // How often the Project files of a docs root were actually read, in this process.
    static qint64 scans();
};
// _target_project: the folder name and resolved folder that build target.
QPair<QString, Py::Path> targetProject(const Py::Path &docsRoot, const QString &target, const std::optional<QString> &projectOverride,
                                       bool allowMissingDocumentInputs);
// _project_build_input_entries, sorted by path.
QList<BuildInput> projectBuildInputEntries(const Py::Path &docsRoot, const QString &projectName, const Py::Path &project,
                                           const QSet<QString> &managedPaths);
// _build_input_snapshot_bytes for canonical entries.
QByteArray buildInputSnapshotBytes(const QString &target, const QList<BuildInput> &entries, const QString &project, const QString &discoveryPolicy);
// _project_discovery_policy
QString projectDiscoveryPolicy(const Py::Path &project);
// The files a folder's Markdown stylesheets load.
QList<Py::Path> stylesheetDependencies(const Py::Path &project);
// is_html_source_path(path, project)
bool isHtmlSourcePath(const QString &path, const QString &project);
// entries without the HTML pages target's PDF does not carry: thoughts-server makes a PDF carry the
// pages its source links (and the pages those link), and the folder's alphabetically first PDF
// all of them. So a page edit changes only the build inputs of the PDFs that carry the page.
QList<BuildInput> withCarriedPagesOnly(const Py::Path &docsRoot, const QString &projectName, const Py::Path &project,
                                       const QString &target, const QList<BuildInput> &entries);
// is_latex_source_asset(path) and is_markdown_asset(path)
bool isLatexSourceAsset(const QString &path);
bool isMarkdownAsset(const QString &path);

// inspect-build-inputs and inspect-library-build-inputs: the stdout objects.
Py::Json inspectBuildInputs(const Py::Path &docsRoot, const QString &target, const Py::Path &sourceManifest,
                            const std::optional<QString> &projectOverride);
Py::Json inspectLibraryBuildInputs(const Py::Path &docsRoot, const Py::Path &sourceManifest);
// inspect-build-inputs --also-target (native only): several PDFs of one
// Project from one scan, as {"schema_version": 1, "inventories": [...]}.
Py::Json inspectProjectBuildInputs(const Py::Path &docsRoot, const QStringList &targets, const Py::Path &sourceManifest,
                                   const std::optional<QString> &projectOverride);

// _manifest_paths: the managed paths of a live source manifest (validated).
QSet<QString> manifestPaths(const Py::Path &sourceManifest);

}
