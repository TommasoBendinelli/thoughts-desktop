// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"
#include "pyjson.h"

#include <QHash>
#include <QString>
#include <QStringList>

// A docs folder's interactive HTML pages, kept in step with Fedora's page list
// (thoughts-server protocol/html-pages.md, capability html_pages_v1).
//
// A page is <folder>/tex/support/html/<name>.html, the folder a docs-* folder or a
// Project's default folder (.thoughts/projects/<hex>/default). Its bytes are an
// object; the folder's list is the HTML attachment manifest, which only
// Fedora writes (POST /v1/html-pages). Each Sync compares the local files, the
// folder's pages on Fedora and the version both last agreed on (kept in
// html-pages.json next to the publisher's state): a page Fedora added or
// changed is installed into the folder, a page added, changed or deleted here
// is sent to Fedora, naming the page it replaces. When both changed, Fedora's
// page is installed and the local one is kept in
// .thoughts/html-page-conflicts/<UTC time>/<path>.
namespace Thoughts::Publisher
{

class SourceApi;

struct HtmlPageSync {
    bool supported = true;
    QStringList installed;
    QStringList uploaded;
    QStringList removed;
    QStringList conflicts;
    QStringList errors;
    Py::Json toJson() const;
    bool changed() const { return !installed.isEmpty() || !uploaded.isEmpty() || !removed.isEmpty(); }
    // Whether a command's output reports it: something happened or needs attention.
    bool reportable() const { return changed() || !conflicts.isEmpty() || !errors.isEmpty(); }
};

// <folder>/tex/support/html/<name>.html with a portable file name.
bool isHtmlPagePath(const QString &path);
// The source folders (docs-* and default folders) among the owners of `paths` (managed source paths).
QStringList htmlPageFolders(const QStringList &paths);
// The folder's pages its HTML attachment manifest lists on this computer.
QStringList listedHtmlPages(const Py::Path &docsRoot, const QString &folder);
// One pass for `folders`. Never raises: a failure is reported in errors and
// leaves the agreed versions as they were, so the next Sync decides again.
// `stamp` names the conflict directory (UTC, e.g. 20261109T120000Z).
HtmlPageSync syncHtmlPages(SourceApi &api, const Py::Path &docsRoot, const Py::Path &statePath, const QStringList &folders,
                           const QString &stamp = {});

}
