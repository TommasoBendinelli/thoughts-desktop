// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

namespace Thoughts
{
// The PDF version a piece of feedback was given on (thoughts-feedback-version-stamps-SPEC.md).
// Every user feedback record the desktop creates or edits about a document (review events, review
// cards, page exclusions, comments and replies, answer feedback) carries the same top-level field
//   "review_version": {"pdf_sha256": "<64 hex>", "source_built": true|false}
// pdf_sha256 is the SHA-256 of the PDF bytes on screen, which for a Library PDF is the server's PDF
// object SHA-256; source_built says whether a .tex/.md source builds that PDF. The server keeps
// every stamped PDF version. The field is left out when the PDF is not here, and deletions and
// undos are not stamped.
namespace FeedbackVersion
{
QString field();

// SHA-256 (lower-case hex) of the file's bytes, kept per file identity (path, size, times, inode;
// at most 4096 entries), so a PDF is hashed once per version. Empty when unreadable or when the
// file changed while it was read.
QString cachedFileSha256(const QString &path);

// {"pdf_sha256", "source_built"}, or empty when @p pdfSha256 is not a lower-case SHA-256.
QJsonObject make(const QString &pdfSha256, bool sourceBuilt);

// Whether the Library index lists the file named @p filename as built from a source ("source_linked").
bool librarySourceBuilt(const QString &docsRoot, const QString &filename);

// The stamp for the PDF at @p pdfPath under @p docsRoot: @p knownSha256 when it is valid (the hash
// of the PDF the reader opened), else the cached hash of the file. source_built is
// @p sourceBuiltHint or the Library index's kind of the file. Empty when the PDF is not here.
QJsonObject forPdf(const QString &docsRoot, const QString &pdfPath, bool sourceBuiltHint = false, const QString &knownSha256 = {});

// The PDF of the Library document @p documentId (its current PDF, else this computer's copy), or
// current_pdfs/<pdfStem>.pdf. Empty when neither file exists.
QString documentPdfPath(const QString &docsRoot, const QString &documentId, const QString &pdfStem = {});

// @p record with @p version as its review_version. An empty @p version (PDF not here) removes the
// field: an edit's stamp replaces the record's earlier one, and an unknown version is left out.
QJsonObject stamped(QJsonObject record, const QJsonObject &version);
}
}
