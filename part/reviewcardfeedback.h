// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVariantList>

#include <functional>
#include <optional>
#include <string>

namespace Thoughts
{
class DesktopSyncOutbox;

// Review card feedback (thoughts-server protocol/review-card-feedback.md): a passage of a card's
// revealed answer marked liked (positive) or disliked (negative), with an optional comment. One
// synced record per highlight (entity_type review_card_feedback, collection review-feedback,
// entity_id = feedback_id). The anchor, card, creation time and creating origin are fixed by the
// first write; later writes change only polarity, comment and deletion. The phone follows the same
// rules, so everything here matches the spec and the server exactly.
namespace ReviewCardFeedback
{
QString entityType();
QString collection();
// Servers that accept review_card_feedback; until one does, local records stay queued.
QString capability();
QString projectionKey(const QString &feedbackId);
QString positive();
QString negative();
// A question or another comment on a document (document-feedback.md §9), never card feedback.
QString neutral();

constexpr int contextLength = 32;     // quote.prefix / quote.suffix written by this client
constexpr int maximumComment = 4000;  // characters (code points), as the server counts them
constexpr int maximumQuote = 4000;
constexpr int maximumContext = 200;
constexpr int maximumRegions = 200;
constexpr double qualificationMargin = 0.01;

// ---------------------------------------------------------------- re-anchoring (§4)

// The comparable characters of @p text and, for each, the UTF-16 range [first, second) of the code
// point it came from.
struct Skeleton {
    std::u32string text;
    QList<QPair<int, int>> spans;
};
Skeleton skeleton(const QString &text, bool dropHyphens = false);

struct Resolution {
    QString state; // "exact", "relocated" or "orphaned"
    int start = -1; // UTF-16 offsets into the answer text, end exclusive; -1 unless relocated
    int end = -1;
};
Resolution resolve(const QString &answerText, const QString &exact, const QString &prefix = {}, const QString &suffix = {}, bool samePdf = false);
// resolve() for a record: quote and same-PDF test from its anchor.
Resolution resolve(const QJsonObject &record, const QString &answerText, const QString &displayedPdfSha256);

// ---------------------------------------------------------------- answer text (§4.1)

// One text entity of a page as the PDF text layer gives it (a character, a space, a newline),
// in reading order. @p page is 1-based, @p box normalized to the page with a top-left origin.
struct Glyph {
    int page = 0;
    QString text;
    QRectF box;
};
struct AnswerText {
    QString text;
    // For each UTF-16 unit of text, the index into glyphs it came from, or -1 (region joiners).
    QList<int> source;
    QList<Glyph> glyphs;
};
// @p regionGlyphs: for each answer region in list order, the page glyphs whose box centre lies
// inside the region. Regions are joined with "\n".
AnswerText answerText(const QList<QList<Glyph>> &regionGlyphs);
// One rectangle per text line of @p glyphs (glyph boxes merged; blank glyphs ignored), in order.
QList<QPair<int, QRectF>> lineRects(const QList<Glyph> &glyphs);
// The line rectangles of answer text [start, end).
QList<QPair<int, QRectF>> rangeRects(const AnswerText &answer, int start, int end);

// The card's answer regions (page 1-based, normalized rect) from a list of {page, rect} maps.
QList<QPair<int, QRectF>> regionsFromVariant(const QVariantList &regions);
// Whether a selection made of @p lines may receive feedback: the centre of every line lies in one
// of @p answerRegions grown by qualificationMargin (§6.2).
bool qualifies(const QList<QPair<int, QRectF>> &lines, const QList<QPair<int, QRectF>> &answerRegions);
QJsonArray regionsJson(const QList<QPair<int, QRectF>> &lines);
QList<QPair<int, QRectF>> regionsOf(const QJsonObject &record);

// ---------------------------------------------------------------- records (§2, §5)

// UTC with millisecond precision and Z.
QString timestamp(const QDateTime &now);
QJsonObject anchor(const QString &exact, const QString &prefix, const QString &suffix, const QString &documentId, const QString &pdfSha256,
                   const QJsonArray &regions, const QString &segmentId = {}, const QString &segmentContentSha256 = {});
// The comment as saved: leading and trailing whitespace trimmed, at most maximumComment characters.
QString cleanComment(const QString &comment);
QJsonObject create(const QString &cardId, const QString &polarity, const QString &comment, const QJsonObject &anchor, const QString &originId,
                   const QDateTime &now = QDateTime::currentDateTimeUtc());
// The next revision with @p polarity and @p comment; unknown fields are kept.
QJsonObject edit(const QJsonObject &record, const QString &polarity, const QString &comment, const QString &originId,
                 const QDateTime &now = QDateTime::currentDateTimeUtc());
// The tombstone (next revision, deleted, deleted_at); the full record is kept.
QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
// Undo of a delete: the tombstone's revision + 1, not deleted, without deleted_at.
QJsonObject undo(const QJsonObject &tombstone, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// Change passage: a highlight moved to another passage. The anchor is fixed by the first write
// (§2), so a new record (created) takes the new passage and the old one is deleted (tombstone),
// both in one store write.
struct Replacement {
    QJsonObject created;
    QJsonObject tombstone;
};
// Whether @p anchor is the passage @p record already has: the same exact quote on the same PDF
// bytes. Then Change passage only saves an edit.
bool samePassage(const QJsonObject &record, const QJsonObject &anchor);
// The replacement of @p current, the stored record of a card: a new record on the same card_id
// (new feedback_id, revision 1) with @p anchor, @p polarity and @p comment, and @p current's
// tombstone. Empty and @p error when @p current is missing or deleted, or the new record is not
// valid (validationError, e.g. its anchor). The caller stamps created's review_version.
std::optional<Replacement> replacePassage(const QJsonObject &current, const QString &polarity, const QString &comment, const QJsonObject &anchor,
                                          const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc(), QString *error = nullptr);
// The writes that undo @p replacement, from the records as they are stored now (@p createdNow,
// @p oldNow; empty when not known, then the replacement's own): the new record deleted (next
// revision) unless it already is, and the old one restored (next revision, not deleted) unless a
// later write already brought it back. Card and document feedback alike.
QList<QJsonObject> undoReplacement(const Replacement &replacement, const QJsonObject &createdNow, const QJsonObject &oldNow, const QString &originId,
                                   const QDateTime &now = QDateTime::currentDateTimeUtc());

// Whether the write @p incoming replaces @p stored under the server's rules: priority (revision,
// deleted, updated_at, origin_id), so a delete wins an equal revision in every arrival order and
// a deleted record comes back only with a strictly higher revision.
// The immutable fields of @p stored are kept by merge(), not tested here.
bool applies(const QJsonObject &incoming, const QJsonObject &stored);
// @p incoming applied to @p stored: the winner, with card_id, anchor, created_at and
// created_origin_id always those of @p stored when it exists.
QJsonObject merge(const QJsonObject &stored, const QJsonObject &incoming);

// Records of @p cardId among projections keyed "entity_type\ncollection\nentity_id", in
// created_at order (then feedback_id).
QList<QJsonObject> recordsOfCard(const QJsonObject &projections, const QString &cardId, bool includeDeleted = false);
void sortByCreation(QList<QJsonObject> &records);

// ---------------------------------------------------------------- shared feedback

// Feedback is shared by the cards of a document (2026-10-26: "the feedback on the document
// should be shared across cards"). A record keeps the card it was made on (its home card,
// card_id; new records still name the card under review), and every other card of the same
// document shows it too while its passage lies inside that card's answer: made on the PDF shown,
// the centre of each of its line rectangles is inside the card's answer regions (the rule for
// making feedback, qualifies()); made on another PDF, its quote is found (§4) in the card's answer
// text. Not inside: not shown on that card and never listed there as not found. An edit, delete
// or Undo from any card changes the same record. The phone follows the same rules.

// The cards that exist here (not deleted, not candidates), by card_id: the cards feedback may
// be shared from. Another card's record whose card is not among them is never shown (§5: feedback
// of a deleted or unknown card is hidden).
using HomeCards = QHash<QString, QJsonObject>;
// The document of a card's answer: its Library document IDs and its PDF stem.
struct CardDocument {
    QSet<QString> ids;
    QString pdfStem;
};
// @p card's document: @p extraIds (such as the shown PDF's own ID) and the card's primary
// pdf_document_id and document_id (empty ones left out), and its primary pdf_stem.
CardDocument documentOf(const QJsonObject &card, const QStringList &extraIds = {});
// The Library document @p record was made in: its anchor's pdf.document_id.
QString documentIdOf(const QJsonObject &record);
// Whether @p record, made on @p homeCard, was made in @p document: its anchor's document ID among
// the document's IDs; without a valid anchor ID, or a document without IDs, the home card's IDs
// (any in common); without those, the home card's PDF stem.
bool inDocument(const QJsonObject &record, const QJsonObject &homeCard, const CardDocument &document);
// Whether @p record was made on another card than @p cardId (it is shared with @p cardId).
bool isShared(const QJsonObject &record, const QString &cardId);
// The records @p cardId's answer may show among @p records (or projections): its own, and those of
// other cards among @p homeCards made in @p document; deleted ones only with @p includeDeleted. In
// created_at order.
QList<QJsonObject> recordsForCard(const QList<QJsonObject> &records, const QString &cardId, const CardDocument &document, const HomeCards &homeCards,
                                  bool includeDeleted = false);
QList<QJsonObject> recordsForCard(const QJsonObject &projections, const QString &cardId, const CardDocument &document, const HomeCards &homeCards,
                                  bool includeDeleted = false);
// Where the record of another card lies in this card's answer: made on the shown PDF
// (@p displayedPdfSha256), exact when the centre of each line rectangle is inside one of
// @p answerRegions (grown by qualificationMargin, as qualifies()), else orphaned; made on another
// PDF, resolve() in the answer text, which @p answerText gives (asked for only then; empty when it
// cannot be read, so orphaned). Orphaned = not shown on this card.
Resolution resolveShared(const QJsonObject &record, const std::function<QString()> &answerText, const QString &displayedPdfSha256,
                         const QList<QPair<int, QRectF>> &answerRegions);
// The records of other cards among @p records (every card's) that lie in @p cardId's answer, with
// their state (exact or relocated): made on one of @p homeCards in @p document, not deleted, and
// inside the answer as resolveShared() finds it. @p answerText is asked for at most once.
QHash<QString, QString> sharedStates(const QString &cardId, const CardDocument &document, const HomeCards &homeCards, const QString &displayedPdfSha256,
                                     const QList<QPair<int, QRectF>> &answerRegions, const QList<QJsonObject> &records,
                                     const std::function<QString()> &answerText);

// Empty when @p record is a valid record for the server (§2), otherwise why not.
QString validationError(const QJsonObject &record);
// The §2 rules; without @p requireCard every rule but the card_id one (document feedback,
// documentfeedback.h, shares the other fields). @p noun names the record in the message.
QString fieldValidationError(const QJsonObject &record, bool requireCard, const QString &noun);
// Queues @p record for the server (base revision = revision - 1).
bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error = nullptr);
// Queues every one of @p records in one store write (all or none).
bool enqueueAll(DesktopSyncOutbox &outbox, const QList<QJsonObject> &records, QString *error = nullptr);

// ---------------------------------------------------------------- agents (§6.5, §7)

// "Answer feedback:\n- Liked: \"…\" — comment\n…" for the non-deleted @p records, or empty. A
// record of another card than @p cardId (shared) ends with ` (from another card: "<question>")`,
// its card's question from @p homeQuestions (card_id -> question), whitespace collapsed, cut at
// 300 characters with "…"; ` (from another card)` without one.
QString agentSection(QList<QJsonObject> records, const QSet<QString> &orphaned, const QString &cardId = {}, const QHash<QString, QString> &homeQuestions = {});
// One feedback entry of a card for agents (§7). @p card is the card record. Besides the §7 fields:
// card_id, the card the feedback was made on, and shared, true when that is another card (shared
// feedback found in this card's answer).
QJsonObject agentJson(const QJsonObject &record, const QJsonObject &card, const QString &resolvedState = {});
// The feedback array of a card: non-deleted records in created_at order (deleted ones too, with
// deleted_at, when @p includeDeleted). @p states: feedback_id -> anchor_state where it is known
// (shared records found in the answer: exact or relocated).
QJsonArray agentArray(QList<QJsonObject> records, const QJsonObject &card, bool includeDeleted = false, const QHash<QString, QString> &states = {});
}
}
