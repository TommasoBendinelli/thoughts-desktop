// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include <optional>

// Relative Markdown links between documents, read as the LaTeX link commands
// they mean (thoughts_server.markdown_links, which the renderer uses):
//   [t](x.md#source-label:n)            path inside the folder, .md/.markdown/.tex: \SourceLink
//   [t](../../docs-o/y.md#source-label:n) path into another source folder:        \CrossSourceLink
//   [t](key.pdf#anchor)                 bare filename (always the Library PDF):  \PdfLink
//   [t](../../docs-o/key.pdf#anchor)    root of another docs-* folder:           \CrossPdfLink
// No fragment: the document start (document-start). Other paths inside the
// folder are ordinary links; other paths that leave it are invalid.
namespace Thoughts::Publisher::MarkdownLinks
{

struct DocumentLink {
    QString kind;            // SourceLink, CrossSourceLink, PdfLink, CrossPdfLink; empty when invalid
    QString target;          // the LaTeX command's first argument
    QString anchor;          // Source label name (empty: document start) or PDF anchor
    QString ignoredFragment; // a fragment that names no Source label (opens the document start)
    bool hasIgnoredFragment = false;
    QString problem;         // why the link is invalid
};

// The document link an href in folder/source means, or nullopt for an
// ordinary link (external, fragment-only, or a file inside the folder that is
// no source document). folder is the docs-root-relative source folder.
std::optional<DocumentLink> classify(const QString &href, const QString &folder, const QString &source);

// The href from folder/source for a link of the given kind (LaTeX arguments),
// e.g. markdownForm("CrossPdfLink", "docs-o/key", "a", "docs-x", "notes/n.md") = "../../docs-o/key.pdf#a".
QString markdownForm(const QString &kind, const QString &target, const QString &anchor, const QString &folder, const QString &source);

// The docs-root-relative path a relative link path from folder/source names,
// or an empty string when it leaves the Library.
QString resolvePath(const QString &folder, const QString &source, const QString &path);

struct LinkSpan {
    qsizetype start = 0; // of the destination, without angle brackets
    qsizetype end = 0;
    QString href;
    bool image = false;
    int line = 0;        // one-based
    QString text;        // the whole link as written
};

// Inline links, images and reference definitions outside code, in order.
QList<LinkSpan> linkSpans(const QString &markdown);

// The PDF keys of the bare-filename links (\PdfLink) in a Markdown source.
QStringList pdfLinkKeys(const QString &markdown);

}
