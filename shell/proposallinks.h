/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_PROPOSALLINKS_H
#define THOUGHTS_PROPOSALLINKS_H
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Thoughts {
// A link a proposal introduces that the Fedora renderer would leave inactive
// (thoughts_server.renderer_tools.doc_links): a \PdfLink or \CrossPdfLink to
// a PDF no Project selects (the build finds a Library PDF by its filename, in
// any Project), a Source Cross link with a wrong folder token, a missing
// doc-anchor: annotation or Source label, a dead Markdown link, or
// a retired \TexLink / \CrossTexLink (reason retired_link, with the
// \SourceLink / \CrossSourceLink that replaces it as suggestion).
struct ProposalLinkIssue {
    QString reason;     // stable code, e.g. pdf_not_selected
    QString command;    // PdfLink, CrossPdfLink, ..., or markdown_link
    QString text;       // the link as written
    int line = 0;       // one-based line within the proposed text
    QString message;    // what is wrong and how to fix it
    QString suggestion; // the corrected link, when one is known
    QJsonObject toJson() const;
};

// The links in replacement that are new relative to original and do not
// resolve against the Library at docsRoot, for a proposal in sourcePath (an
// absolute path below docsRoot). updatedSource is the source as it would be
// written, so a label the same request adds counts. Links already present in
// original are never reported: a proposal does not answer for the old text.
// A source outside every Project is not built, so its links are not checked.
QList<ProposalLinkIssue> introducedLinkIssues(const QString &docsRoot, const QString &sourcePath, const QString &original, const QString &replacement,
                                              const QString &updatedSource);
}
#endif
