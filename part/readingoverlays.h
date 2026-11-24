// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Thoughts
{
class DesktopSyncOutbox;

// Reading overlays (thoughts-server protocol/reading-overlays.md): an agent's reading guide over a
// Library PDF. One reading_overlay record (collection reading-overlays, entity_id = overlay_id)
// belongs to a document (document.document_id) and to the PDF bytes its marks were placed on
// (document.pdf_sha256). Each mark has a kind (read, skim, skip, note), a comment and the anchor of
// document feedback (a quote and one rectangle per line), so it is found again on another PDF
// version exactly as feedback is (DocumentFeedback::place). The overlay is the agent's, not the
// user's: the user only hides it on this computer, deletes it, or deletes single marks, and those
// two are revisions (§2) that wait in the queue until the server accepts reading_overlay records.
namespace ReadingOverlays
{
QString entityType();
QString collection();
// Servers that accept reading_overlay writes; until one does, the user's deletions stay queued.
QString capability();
QString projectionKey(const QString &overlayId);

// The kinds of mark (§1): read carefully, read quickly, can be left out, only a remark.
QString read();
QString skim();
QString skip();
QString note();
bool isKind(const QString &kind);

constexpr int maximumMarks = 500;
// How much of a quote the Overlay tab lists (§4).
constexpr int listedQuoteLength = 100;

QString overlayIdOf(const QJsonObject &record);
// The Library document of @p record: document.document_id.
QString documentIdOf(const QJsonObject &record);
// The PDF the marks were placed on: document.pdf_sha256.
QString pdfSha256Of(const QJsonObject &record);
bool isDeleted(const QJsonObject &record);

// The marks in the record's (reading) order.
QList<QJsonObject> marksOf(const QJsonObject &record);
QJsonObject markOf(const QJsonObject &record, const QString &markId);
QString markIdOf(const QJsonObject &mark);
// The mark's kind, or note when it is none of the four (a later kind this client does not know
// is shown as a remark: its comment, no advice).
QString kindOf(const QJsonObject &mark);
QString commentOf(const QJsonObject &mark);
// The agent note that explains the mark at length (note_id, reading-overlays.md "Mark notes";
// formulas rendered, a picture): its id when it is a UUID, else empty. The popup shows its picture
// in place of the comment.
QString noteIdOf(const QJsonObject &mark);
QString quoteOf(const QJsonObject &mark);
// The first @p length characters (code points) of the mark's quote, with an ellipsis when cut.
QString quoteStart(const QJsonObject &mark, int length = listedQuoteLength);
// The mark's passage as DocumentFeedback::place reads one: a record with the mark's anchor.
QJsonObject passageOf(const QJsonObject &mark);

// The name the user knows the agent by: author.label, else author.agent, else empty.
QString authorLabel(const QJsonObject &record);
QString conversationUrl(const QJsonObject &record);
QString conversationId(const QJsonObject &record);

// ---------------------------------------------------------------- Ask About This (§4)

// How much of the quote the title and the message of a question carry.
constexpr int questionTitleQuoteLength = 60;
constexpr int questionQuoteLength = 300;
// The question asked when the user typed none.
QString defaultQuestion();

// Ask About This sends the mark's page to the session that made the overlay (agent-notes.md,
// "Pages sent to one session"): its title and message (English, for the agent, word for word as
// the spec has them), and what its origin has besides Send Page's: conversation_id (the overlay's
// author.conversation_id; left out when it has none) and reading_overlay {overlay_id, mark_id,
// kind}.
struct Question {
    QString title;
    QString message;
    QJsonObject origin;
};
// @p documentName and @p page (1-based) as Send Page names the page; @p asked the user's question
// (empty: defaultQuestion()).
Question question(const QJsonObject &record, const QJsonObject &mark, const QString &documentName, int page, const QString &asked);

// ---------------------------------------------------------------- the user's edits (§2)

// Delete Mark: the next revision without @p markId among the marks; deleted (the overlay's
// tombstone) when it was the last one. Empty when @p record is deleted or has no such mark.
QJsonObject withoutMark(const QJsonObject &record, const QString &markId, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
// Delete Overlay: the next revision, deleted. The rest of the record is kept.
QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// Whether the write @p incoming replaces @p stored: the priority of document feedback (revision,
// deleted, updated_at, origin_id), so a delete is sticky in every arrival order.
bool applies(const QJsonObject &incoming, const QJsonObject &stored);
// @p incoming applied to @p stored: the winner, with overlay_id, document.document_id, author,
// created_at and created_origin_id always those of @p stored when it exists (they never change, §1;
// the document's pdf_sha256 and filename do, when the agent replaces the overlay on a newer PDF).
QJsonObject merge(const QJsonObject &stored, const QJsonObject &incoming);

// Empty when @p record can be queued as the user's revision of an overlay, otherwise why not: a
// valid overlay_id and document, a title, a revision after the first, updated_at, origin_id, unless
// it is deleted 1-500 marks, and mark_ids that are valid and unique. Anchors are the server's and
// are not checked here.
QString validationError(const QJsonObject &record);
// Queues @p record for the server (base revision = revision - 1).
bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error = nullptr);

// The overlays of @p documentId among @p records (or projections keyed
// "entity_type\ncollection\nentity_id"), newest first (created_at, then overlay_id); deleted ones
// only with @p includeDeleted.
QList<QJsonObject> recordsOfDocument(const QList<QJsonObject> &records, const QString &documentId, bool includeDeleted = false);
QList<QJsonObject> recordsOfDocument(const QJsonObject &projections, const QString &documentId, bool includeDeleted = false);
}
}
