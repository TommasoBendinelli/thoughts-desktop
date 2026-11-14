// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
namespace Thoughts {
inline constexpr auto reviewFingerprintMethod = "poppler-rgb-96-v1";
QJsonArray limitBasicCardExtent(const QJsonArray &candidates, const QString &pdfSha256);
QVariantMap prepareBasicReviewCards(const QString &pdfPath, const QString &bundleRoot = {});
bool isBasicReviewCard(const QJsonObject &card);
QString basicReviewSnapshotPath(const QString &docsRoot, const QJsonObject &card);
QString reviewCardPdfPath(const QString &docsRoot, const QJsonObject &card);
QVariantMap fingerprintReviewPage(const QString &pdfPath, int page, const QString &expectedPdfSha256 = {});
QVariantMap locateReviewPage(const QString &pdfPath, const QString &fingerprint);
struct ReviewAnchorPlan {
    QString error;
    // PDF validation errors prevent creation; source localization errors do not.
    bool invalidPdf = false;
    bool withoutSource = false;
    QString fallbackSource;
    QString path;
    QByteArray original;
    QByteArray replacement;
    QJsonObject anchor;
    QJsonObject answerAnchor;
    QJsonObject componentOrigin;
    // planReviewSourceAnchors: the requested area that could not be linked (-1 when the
    // problem is not one area's), the saved answer regions ({page, rect}) in segment order,
    // and the visual fingerprint of the first region's page.
    int failedArea = -1;
    QJsonArray regions;
    QString pageVisualSha256;
    // The planning failed only because this device lacks the PDF's verified source map (not
    // installed yet, or for another PDF version): the server can still link the card.
    bool sourceMapUnavailable = false;
    // Set by the caller on such a failed plan: save the card unmarked for the server to link
    // at the next Sync (source_link pending_server_link). Its regions are the areas.
    bool linkOnServer = false;
};
// One answer area of a card: a rectangle normalized to its PDF page. A text selection is
// saved as its bounding rectangle but never stands for a whole figure or table.
struct ReviewArea {
    int page = 0;
    QRectF rect;
    bool textSelection = false;
};
ReviewAnchorPlan planReviewSourceAnchor(const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                                      const QString &pdfSha256, int page, const QRectF &region, const QString &cardId);
// Links every area to complete lines of one editable source file and marks one segment per
// passage (areas whose lines overlap share one), numbered 0..n-1 in file order. An area that
// is a rectangle around a float (figure, table, algorithm, ...) of a TeX source marks the whole
// environment; words inside it that cannot be mapped (generated tables, drawings) are then
// tolerated. With @p replaceCardMarkers the card's own markers already in that file are removed
// first (the source with and without them must match the PDF's build), so the card moves.
// An area whose words the build's lines cannot place (its source changed since the build, or
// SyncTeX lines that do not name the words) is placed by its whole text in the current source
// (locateReviewTextInSource), or keeps the line mapping's error: never a PDF-only card.
// A PDF without editable source gives withoutSource, as for one area.
ReviewAnchorPlan planReviewSourceAnchors(const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                                         const QString &pdfSha256, const QList<ReviewArea> &areas, const QString &cardId,
                                         bool replaceCardMarkers = false);
// Where the words of an answer area (its PDF text in reading order) occur in a TeX or Markdown
// source, for when the PDF's build lines cannot place it: a build older than the source (a
// proposal accepted since), or SyncTeX lines that do not name the selected words (all the text
// of a pending proposal sits on its closing brace). The source lines from first to last, or
// first == 0 when the text is not found exactly once (ambiguous when found more than once).
// The exact text is tried first, then its letters and digits only, which allows for line-end
// hyphens and joined lines; both need at least 24 of them. Then, for math, whose PDF text does
// not spell its TeX, the prose pieces between formulas one by one: each must be found, in
// order, in one region whose other prose the area's short words explain. The lines are whole
// lines of the area and never cut display math.
struct ReviewTextPlacement {
    int first = 0;
    int last = 0;
    bool ambiguous = false;
};
ReviewTextPlacement locateReviewTextInSource(const QString &path, const QByteArray &source, const QStringList &words);
// The source without the marker lines of @p cardId (TeX or Markdown); lineMap[i] is the new
// line of old line i + 1, or 0 for a removed marker line. Markers that are not complete,
// canonical pairs give an error and leave the text untouched.
struct ReviewMarkerStrip {
    QByteArray text;
    QList<int> lineMap;
    int removed = 0;
    QString error;
};
ReviewMarkerStrip stripReviewCardMarkers(const QString &path, const QByteArray &source, const QString &cardId);
// The source without the markers of @p cardId's passages @p segmentIds, as deleting the card
// removes them (the server's delete_marked_passages without the text): each marker goes with its
// line when nothing else is on it. Empty, with *error, when the source does not hold exactly one
// start and one end marker for each of those passages and no other marker of the card.
QByteArray removeReviewCardMarkers(const QString &path, const QByteArray &source, const QString &cardId,
                                   const QSet<QString> &segmentIds, QString *error);
// Merging review cards (ReviewStore::mergeCards): the source change that gives the kept card
// every passage of the merged ones, in one TeX or Markdown file. The merged cards' markers go
// first, one card at a time, as their deletions remove them; then the kept card marks every
// passage on its own, numbered 0..n-1 in file order. Text between passages stays unmarked;
// passages that overlap become one.
struct ReviewCardMergeSource {
    QString error;
    // versions[0] is the source; versions[k] lacks the markers of mergedIds[0..k-1]. The last
    // one still holds the kept card's own markers.
    QList<QByteArray> versions;
    QByteArray replacement; // The kept card marks every passage.
    QJsonObject anchor;     // Its source_anchor (version 2).
    // For each segment of the anchor, the cards whose passages it marks (card IDs, file order).
    QList<QStringList> origins;
};
// @p segmentIds: each card's passages as its record lists them (segment IDs), the kept card's
// included. @p path is the source's Library path (it names the file in the anchor). @p cardNames
// name the cards in refusals (a card's question), by ID; a card without one is named by its ID.
// A pending agent proposal inside a passage stays as it is, byte for byte (only marker lines
// move), so deciding it afterwards gives what deciding it before would have, plus the merge's
// markers; a proposal that holds a card's marker is refused.
ReviewCardMergeSource planReviewCardMergeSource(const QString &path, const QByteArray &source, const QString &keptId,
                                                const QStringList &mergedIds, const QHash<QString, QSet<QString>> &segmentIds,
                                                const QHash<QString, QString> &cardNames = {});
// Why the markers of @p cardId in a TeX or Markdown source are not complete passages, said
// precisely (which line, what is wrong: a marker without its end or start, nested or misnumbered
// passages, a marker that shares its line or lies in a pending proposal, an empty passage, a
// mention of the card outside its markers); empty when they are. @p cardName names the card.
QString reviewCardMarkerProblem(const QString &path, const QByteArray &source, const QString &cardId, const QString &cardName = {});
// Marks each one-based inclusive line range as one segment (0..n-1 in file order) of a TeX or
// Markdown source. On failure *failedRange is the index of the range at fault, or -1.
// A new passage holds no definition (\def, \newcommand, \renewcommand, \providecommand,
// \DeclareRobustCommand), other card's marker or pending proposal. @p existingPassages: the
// ranges already are a card's passages (a merge moves their markers), so definitions in them and
// pending agent proposals they hold whole are no obstacle; a proposal held only in part still is.
ReviewAnchorPlan markReviewSourceRanges(const QString &path, const QByteArray &source, const QList<QPair<int, int>> &ranges,
                                        const QString &cardId, int *failedRange = nullptr, bool existingPassages = false);
QJsonObject reviewPdfComponentOrigin(const QString &bundleRoot, const QString &pdfPath,
                                     const QString &pdfSha256, int page);
ReviewAnchorPlan markReviewSourceLines(const QString &path, const QByteArray &source, const QList<int> &lines, const QString &cardId);
ReviewAnchorPlan markReviewSourceSelection(const QString &path, const QByteArray &source, int firstLine, int endLine, const QString &cardId);
// The card's passages as its markers in the source give them. The card exists, so definitions in
// its passages are no obstacle; @p aroundPendingProposals also reads passages that hold pending
// agent proposals whole, as a merge does.
ReviewAnchorPlan readReviewSourceAnchor(const QString &path, const QByteArray &source, const QString &cardId, bool aroundPendingProposals = false);

// What "Copy for agent" on a review card needs; the same text as on the phone.
struct ReviewCardAgentInput {
    QString cardId;
    QString question;
    QString note;
    QString pdfPath;      // The built PDF the card belongs to.
    QString pdfSha256;    // The card's PDF; answer text is read only from a PDF with this hash.
    QString documentName; // As the reader tab names it; empty for the file name.
    QJsonArray regions;   // The answer regions: {page, rect: [x0, y0, x1, y1]} normalized to the page.
    int page = 0;         // The answer page when there are no regions.
    bool wholeDocument = false;
    QStringList sourceFiles; // Sources that may hold its markers, relative to the docs root, in order.
    QString answerFeedback;  // ReviewCardFeedback::agentSection: "Answer feedback:" and one line per record.
};
// The card's and its document's placeholders for the agent prompt templates (agentprompts.h):
// the document's (agentDocumentFields), pdf_pages, card_id, question, note, answer_pages and
// answer_source (the source lines inside its mask markers). With @p withAnswerText also
// answer_text: that source text, or else the text inside its answer regions of the card's own PDF.
QHash<QString, QString> reviewCardPromptFields(const QString &docsRoot, const QJsonArray &projects, const ReviewCardAgentInput &card,
                                               bool withAnswerText = false);
// "Copy for agent" on a review card: @p templateText (blank for the default) filled in for the card.
QString reviewCardAgentDescription(const QString &docsRoot, const QJsonArray &projects, const ReviewCardAgentInput &card,
                                   const QString &templateText = QString());

// The card's answer as "Copy for agent" reads it from the source: the text between its mask
// markers in each of @p sourceFiles (relative to the docs root) that holds them. Lines are
// one-based and inclusive; @p path is absolute. @p contentCache, when given, keeps each file's
// text for the next card. Reads only.
struct ReviewCardSourceSegment { int first = 0, last = 0; QString text; };
struct ReviewCardSourcePassage { QString path; QList<ReviewCardSourceSegment> segments; };
QList<ReviewCardSourcePassage> reviewCardSourcePassages(const QString &docsRoot, const QString &cardId, const QStringList &sourceFiles,
                                                        QHash<QString, QString> *contentCache = nullptr);
// Its fallback: the text inside the card's answer regions of its own PDF (pdfPath with
// pdfSha256), one line per text line; empty for another build or without regions.
QString reviewCardPdfAnswerText(const ReviewCardAgentInput &card);

// The answer feedback of a card for agents (thoughts-server review-card-feedback.md §7,
// ReviewCardFeedback::agentArray): the records of @p cardRecord (deleted ones too with
// @p includeDeleted) and the other cards' records of its document that lie in its answer (shared,
// "shared": true, ReviewCardFeedback::sharedStates): made on the card's PDF (@p answer pdfSha256)
// with every line inside its answer regions, or with the quote found in the text of its answer
// regions of that PDF (@p answer pdfPath), else, when that PDF cannot be read, in
// @p fallbackAnswerText (the source text between its markers; asked for only then). @p records: every card's records;
// @p homeCards: the cards that exist (card_id -> record), whose feedback may be shared. The card's document: its
// primary IDs and the Library ID of the PDF file @p answer pdfPath (ReviewCardFeedback::inDocument).
QJsonArray reviewCardAgentFeedback(const QString &docsRoot, const QJsonObject &cardRecord, const ReviewCardAgentInput &answer, const QList<QJsonObject> &records,
                                   const QHash<QString, QJsonObject> &homeCards, bool includeDeleted = false,
                                   const std::function<QString()> &fallbackAnswerText = {});
}
