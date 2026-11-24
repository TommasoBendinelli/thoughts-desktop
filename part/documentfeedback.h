// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "reviewcardfeedback.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace Thoughts
{
class DesktopSyncOutbox;

// Document feedback (thoughts-server protocol/document-feedback.md): a passage of a Library PDF
// marked liked or disliked while reading, with an optional comment. It is review card feedback
// (reviewcardfeedback.h) without a card: one document_feedback record per highlight (collection
// document-feedback, entity_id = feedback_id) that belongs to the document
// (anchor.pdf.document_id) and to the exact PDF bytes that were displayed (anchor.pdf.pdf_sha256).
// The record shape, priority, sticky delete and Undo are those of card feedback; only the rules
// that differ live here. The phone and the server follow the same rules.
namespace DocumentFeedback
{
QString entityType();
QString collection();
// Servers that accept document_feedback; until one does, local records stay queued.
QString capability();
// Servers that also accept records made in an HTML attachment (§8). An older server refuses the
// whole batch, so these stay queued until one does.
QString attachmentCapability();
// Servers that also accept a question or another comment (§9); the same goes for them.
QString kindsCapability();
// Servers that also accept a drawing (§10); the same goes for them.
QString drawingCapability();
// Servers that also accept a text box (§11); the same goes for them.
QString textCapability();
// Servers that also accept a drawing in an HTML attachment, made on a snapshot of the page (§10.1);
// the same goes for them.
QString htmlDrawingCapability();
// Servers that also accept pictures on a comment (§12); the same goes for records with pictures.
QString picturesCapability();
QString projectionKey(const QString &feedbackId);

// How many pages around the anchor's pages a quote is searched on another PDF version (§4).
constexpr int pageWindow = 3;
// Page furniture (a page number, a running head) is a line within furnitureBand of the page's foot
// or head, set apart from the rest of the page's text by more than furnitureGap times its height
// (the server's rule for reading overlays, reading_overlays.FURNITURE_BAND / FURNITURE_GAP).
constexpr double furnitureBand = 0.08;
constexpr double furnitureGap = 1.5;

// ---------------------------------------------------------------- records (§1, §3)

// A passage of an HTML attachment (§8): view "html_attachment", no PDF regions; its quote and page
// state are the anchor, and it is never drawn on the PDF.
QString attachmentView();
bool isAttachmentFeedback(const QJsonObject &record);

// A comment on a passage is a like, a dislike, a question or something else (§9). The last two have
// polarity neutral and a kind, and resolved_at closes them.
QString question();
QString other();
// question(), other(), drawing(), textBox(), or empty for a like or a dislike.
QString kindOf(const QJsonObject &record);
bool isResolved(const QJsonObject &record);
// A question or another comment: what can be resolved.
bool canResolve(const QJsonObject &record);
// @p record as @p kind: neutral with it; without one it no longer has a kind or a resolution.
QJsonObject withKind(QJsonObject record, const QString &kind);
// The next revision of a question or another comment, closed or opened again.
QJsonObject resolve(const QJsonObject &record, bool resolved, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
// Whether @p record shows now: a like, a dislike or an open question on the version it was made on
// (the displayed PDF, or in an HTML attachment the page's @p attachmentSha256), a comment on every
// version until it is resolved.
// @p attachmentSha256: the page's version as attachmentVersionOf gives it (a live page's load included).
bool shownOn(const QJsonObject &record, const QString &displayedPdfSha256, const QString &attachmentSha256 = QString());
// A page's version for shownOn: its sha256, and for a live page the load shown or given on
// (anchor.live.loaded_at, html-live-pages.md §2.1): after a Reload the feedback is from an earlier version.
QString attachmentVersionOf(const QString &pageSha256, const QString &liveLoadedAt = QString());

// A drawing on a page of the PDF (§10): pencil strokes, in normalized page coordinates, in the field
// "drawing". Its anchor names the page and never changes, because the server keeps anchors as they
// were first written and a drawing grows. It belongs to the PDF it was drawn on, like a like.
QString drawing();
bool isDrawing(const QJsonObject &record);
// One stroke: its points, and the colour of the pencil that drew it ("color", "#rrggbb"). Without
// one (empty) it is the pencil's own blue, lighter in dark mode, as clients that know no colours
// draw every stroke.
struct Stroke {
    QList<QPointF> points;
    QString colour;
};
// @p text as a stroke's colour: "#rrggbb" in lower case, or empty when it is not one.
QString strokeColour(const QString &text);
constexpr int maximumStrokes = 200;
constexpr int maximumStrokePoints = 5000;
QList<Stroke> strokesOf(const QJsonObject &record);
// Whether @p stroke, on a page shown at @p page, passes within @p radius of the segment from @p from
// to @p to (all in the same coordinates: the eraser's, on screen).
bool strokeNear(const Stroke &stroke, const QRectF &page, const QPointF &from, const QPointF &to, double radius);
// The rectangle around the strokes of a drawing, slightly larger, on its page.
QRectF strokeBounds(const QJsonObject &record);
// A first write of a drawing on @p page (1-based) of the PDF @p pdfSha256.
QJsonObject createDrawing(int page, const QList<Stroke> &strokes, const QString &documentId, const QString &pdfSha256, const QString &originId,
                          const QDateTime &now = QDateTime::currentDateTimeUtc());
// The next revision of a drawing, with @p strokes; the rest of its "drawing" (a snapshot, fields
// another client added) stays.
QJsonObject withStrokes(const QJsonObject &record, const QList<Stroke> &strokes, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// A drawing in an HTML attachment (§10.1): an interactive page has no fixed layout, so it is drawn
// on a snapshot, a picture of the page as the user saw it, uploaded as an object before the record
// ("drawing.snapshot": object_sha256, media_type, width and height in pixels). Its strokes are
// normalized to the snapshot, its anchor is that of §8 with the quote "Drawing", and it is shown as
// its snapshot with the strokes on top, never on the live page or on the PDF.
bool isAttachmentDrawing(const QJsonObject &record);
constexpr int maximumSnapshotSide = 20000;
// drawing.snapshot of @p record (empty without one), and its object.
QJsonObject snapshotOf(const QJsonObject &record);
QString snapshotSha256(const QJsonObject &record);
QJsonObject snapshotJson(const QString &objectSha256, const QString &mediaType, int width, int height);
// A first write of a drawing on @p snapshot of the page @p placement names
// (HtmlAttachmentReader::newCardFields: html_attachment and page_state) in the document @p documentId,
// whose PDF @p pdfSha256 linked the page.
QJsonObject createAttachmentDrawing(const QJsonObject &placement, const QJsonObject &snapshot, const QList<Stroke> &strokes, const QString &documentId,
                                    const QString &pdfSha256, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// Pictures on a comment (§12): a Like, Dislike, Question, other comment or text box may carry up to
// maximumPictures pictures ("pictures": object_sha256, media_type, width and height, as a
// snapshot), objects uploaded before the record; never a drawing. An edit may add or remove them.
// A picture is at most maximumPictureSide pixels on its longer side: JPEG for a photo, PNG for a
// screenshot.
constexpr int maximumPictures = 8;
constexpr int maximumPictureSide = 2048;
// The record's pictures, oldest first (the valid ones; none for a drawing).
QJsonArray picturesOf(const QJsonObject &record);
QStringList pictureSha256s(const QJsonObject &record);
bool hasPictures(const QJsonObject &record);
// @p record with @p pictures (no field when there are none).
QJsonObject withPictures(const QJsonObject &record, const QJsonArray &pictures);
// " [2 pictures: the Thoughts document_feedback tool shows them]" for Copy for agent, empty
// without pictures (the phone writes the same words).
QString agentPicturesNote(const QJsonObject &record);

// A text box on a page of the PDF (§11): text typed where the user clicked, of kind "text". The text
// is the comment, so every list of comments shows it; the field "text_box" says where it is: its
// rect, in normalized page coordinates, whose width the text wraps at, and its font size, as a
// fraction of the page's height. Its anchor names the page, as a drawing's does, because the server
// keeps anchors as first written and a text box moves; it belongs to the PDF it was typed on.
QString textBox();
bool isTextBox(const QJsonObject &record);
constexpr double minimumFontSize = 0.004;
constexpr double maximumFontSize = 0.2;
// The rect of a text box (normalized, left/top/width/height), empty when it has none.
QRectF textBoxRect(const QJsonObject &record);
double textBoxFontSize(const QJsonObject &record);
// A first write of a text box on @p page (1-based) of the PDF @p pdfSha256.
QJsonObject createTextBox(int page, const QRectF &rect, double fontSize, const QString &text, const QString &documentId, const QString &pdfSha256,
                          const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
// The next revision of a text box, at @p rect with @p text.
QJsonObject withTextBox(const QJsonObject &record, const QRectF &rect, double fontSize, const QString &text, const QString &originId,
                        const QDateTime &now = QDateTime::currentDateTimeUtc());

// A drawing or a text box: made on its page of one PDF (or, a drawing, on a snapshot of an HTML
// attachment), not on a passage. It has no quote to look for, never changes passage, is never
// resolved, and shows whatever Visible markup says.
bool isPageMark(const QJsonObject &record);
// Where a page mark is on its page of the PDF: the strokes of a drawing, the rect of a text box.
// Empty for a drawing in an HTML attachment, which is on no page of the PDF.
QRectF pageMarkBounds(const QJsonObject &record);

// The anchor of such a record from HtmlAttachmentReader::newCardFields, in the document it belongs to.
QJsonObject attachmentAnchor(const QJsonObject &placement, const QString &documentId, const QString &pdfSha256);

// A first write (revision 1, created_at = updated_at = @p now); never a card_id.
QJsonObject create(const QString &polarity, const QString &comment, const QJsonObject &anchor, const QString &originId,
                   const QDateTime &now = QDateTime::currentDateTimeUtc());
// The next revision with @p polarity and @p comment, the tombstone and the Undo of a delete, as
// for card feedback (ReviewCardFeedback::edit / remove / undo); unknown fields are kept.
QJsonObject edit(const QJsonObject &record, const QString &polarity, const QString &comment, const QString &originId,
                 const QDateTime &now = QDateTime::currentDateTimeUtc());
QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
QJsonObject undo(const QJsonObject &tombstone, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// Whether the write @p incoming replaces @p stored: the card feedback priority (revision,
// deleted, updated_at, origin_id), so a delete is sticky in every arrival order.
bool applies(const QJsonObject &incoming, const QJsonObject &stored);
// @p incoming applied to @p stored: the winner, with anchor, created_at, created_origin_id and
// view always those of @p stored when it exists (a field the first write left out stays out).
QJsonObject merge(const QJsonObject &stored, const QJsonObject &incoming);

// Empty when @p record is a valid document_feedback record for the server (§1), otherwise why
// not: the card feedback rules for the shared fields, no card_id, view absent or "pdf", and
// anchor.pdf with 1-200 regions (or an HTML attachment without regions, §8); a drawing has a
// snapshot in an HTML attachment and none on the PDF (§10.1).
QString validationError(const QJsonObject &record);
// Queues @p record for the server (base revision = revision - 1).
bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error = nullptr);
// Queues every one of @p records in one store write (all or none).
bool enqueueAll(DesktopSyncOutbox &outbox, const QList<QJsonObject> &records, QString *error = nullptr);
// Change passage (ReviewCardFeedback::replacePassage) for document feedback: the new record has
// no card_id, takes @p polarity, @p kind and @p comment, and keeps @p current's resolved_at when it
// has a kind. Drawings never change passage. Undo with ReviewCardFeedback::undoReplacement.
std::optional<ReviewCardFeedback::Replacement> replacePassage(const QJsonObject &current, const QString &polarity, const QString &kind, const QString &comment,
                                                              const QJsonObject &anchor, const QString &originId,
                                                              const QDateTime &now = QDateTime::currentDateTimeUtc(), QString *error = nullptr);

// The Library document @p record belongs to: anchor.pdf.document_id.
QString documentIdOf(const QJsonObject &record);
// The records of @p documentId among @p records (or projections keyed
// "entity_type\ncollection\nentity_id"), in created_at order (then feedback_id); deleted ones only
// with @p includeDeleted.
QList<QJsonObject> recordsOfDocument(const QList<QJsonObject> &records, const QString &documentId, bool includeDeleted = false);
QList<QJsonObject> recordsOfDocument(const QJsonObject &projections, const QString &documentId, bool includeDeleted = false);

// ---------------------------------------------------------------- finding the passage (§4)

// The pages of the displayed PDF searched for a quote, 1-based and ascending: the anchor's own
// pages, or with @p widened those plus up to pageWindow pages before and after each. Pages
// outside 1..@p pageCount are left out.
QList<int> searchPages(const QList<int> &anchorPages, int pageCount, bool widened);

struct Placement {
    QString state;                      // "exact", "relocated" or "orphaned"
    QList<QPair<int, QRectF>> rects;    // 1-based page, normalized; empty when orphaned
};
// Where @p record is drawn on the displayed PDF (@p displayedPdfSha256, @p pageCount pages): its
// own regions when it was made on these bytes; otherwise its quote searched (the card feedback
// skeleton match, hyphen retry and context scoring) in the text of its anchor's pages, then of
// the pages around them. @p pageText gives the text of whole pages in order, joined with "\n"
// (ReviewCardFeedback::answerText over every glyph of each page); it is only asked for when a
// search is needed. Never written back.
// A quote not found in the pages' text is searched once more without the page furniture between
// them (withoutPageFurniture), so a passage that runs on across a page break is found.
Placement place(const QJsonObject &record, const QString &displayedPdfSha256, int pageCount,
                const std::function<ReviewCardFeedback::AnswerText(const QList<int> &pages)> &pageText);

// @p text (whole pages, as place's pageText gives it) without the characters of the page furniture
// between its pages: the foot of each page but the last and the head of each but the first. The
// glyphs stay as they are, so the result's source still points at them. @p text itself when there
// is none.
ReviewCardFeedback::AnswerText withoutPageFurniture(const ReviewCardFeedback::AnswerText &text);

// quote.prefix / quote.suffix of a selection: up to ReviewCardFeedback::contextLength characters
// of @p text right before and after the glyphs for which @p selected (1-based page, normalized
// glyph centre) holds, without the selection's own leading and trailing whitespace. Empty when
// nothing is selected in @p text.
QPair<QString, QString> context(const ReviewCardFeedback::AnswerText &text, const std::function<bool(int, const QPointF &)> &selected);
}
}
