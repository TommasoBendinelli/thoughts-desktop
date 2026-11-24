/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWCARDEDITOR_H
#define THOUGHTS_REVIEWCARDEDITOR_H

#include "reviewdocumenttools.h"
#include "reviewstore.h"
#include <QDialog>
#include <functional>
#include <memory>

class QComboBox;

namespace Thoughts
{
// The card's kind as Edit Review Card offers it (review-card-kinds.md §5): Knowledge card, Project
// card, and As its Project (…) when a Project holds the card; the card's own choice selected.
void fillReviewCardKindChoice(QComboBox *choice, const ReviewCard &card);
// The kind @p choice (filled by fillReviewCardKindChoice) says when the user changed it
// (ReviewKinds::knowledge(), project() or followProject()), else empty.
QString chosenReviewCardKind(const QComboBox *choice);

// How the editor starts. By default it edits the card's own regions on the card's PDF.
struct ReviewCardEditorOptions {
    QString title;      // Window title; "Edit Review Card" by default.
    QString saveText;   // The Save button's text.
    QString scopeText;  // The note under the page; "Area edits apply to this PDF version." by default.
    QString pdfSha256;  // The PDF the areas are drawn on; the card's PDF by default.
    bool useAreas = false;
    QVariantList areas; // With useAreas: the exact starting areas ({page, rect}), shown unmerged.
    int page = 0;       // The page shown first; the card's page by default.
    bool offerKind = false; // A Kind row under the question (fillReviewCardKindChoice); kind() says what was chosen.
};

class ReviewCardEditor : public QDialog
{
    Q_OBJECT
public:
    using SaveHandler = std::function<bool(const QVariantList &, const QString &, int, QString *)>;
    ReviewCardEditor(const ReviewCard &card, const QString &pdfPath, SaveHandler save, QWidget *parent = nullptr);
    ReviewCardEditor(const ReviewCard &card, const QString &pdfPath, SaveHandler save, const ReviewCardEditorOptions &options, QWidget *parent = nullptr);
    ~ReviewCardEditor() override;
    // The card's regions while no area was changed, else the areas as drawn.
    QVariantList regions() const;
    // The areas as drawn, always.
    QVariantList areas() const;
    QString question() const;
    // With options.offerKind: the kind chosen when it differs from the card's (chosenReviewCardKind), else empty.
    QString kind() const;
    // Shows the page of area @p index and selects it.
    void showArea(int index);
    void setError(const QString &error);
private:
    struct Private;
    std::unique_ptr<Private> d;
};

// Areas ({page, rect: [x0, y0, x1, y1]}) as planner input. An area equal to @p textSelection
// keeps its text-selection kind.
QList<ReviewArea> reviewAreasFromRegions(const QVariantList &regions, const ReviewArea &textSelection = {});
// As above, keeping the kind of every area equal to one of @p textSelections.
QList<ReviewArea> reviewAreasFromRegions(const QVariantList &regions, const QList<ReviewArea> &textSelections);
// "Area 2 of 3 (page 5) can't be linked to source text: <reason>"; for one area "The area on page 5 …".
QString reviewAreaFailureText(const QList<ReviewArea> &areas, int failedArea, const QString &reason);

enum class ReviewAreaFailureChoice { AdjustAreas, PlaceByHand, LinkAtNextSync, Cancel };
// Asks what to do with areas that cannot be linked: Adjust Areas or Cancel, with
// @p allowByHand also Save and Place Markers by Hand, and with @p allowServerLink (this device
// lacks the PDF's source map) also Save; Link at Next Sync.
ReviewAreaFailureChoice askAboutUnlinkedArea(QWidget *parent, const QString &text, bool allowByHand, bool allowServerLink = false);

// Runs @p work (planReviewSourceAnchors) in the background behind a progress dialog. False when
// the user cancelled it.
bool planReviewAreasWithProgress(QWidget *parent, const std::function<ReviewAnchorPlan()> &work, ReviewAnchorPlan *plan);

// The new card's areas, chosen in the area editor and linked to the source.
struct NewReviewCardAreas {
    QString question;
    ReviewAnchorPlan plan;   // Linked (or withoutSource); with placeByHand, the failed plan of the one area;
                             // with plan.linkOnServer, the failed plan with the areas as regions.
    QList<ReviewArea> areas;
    bool placeByHand = false;
};
// @p plan, failed for want of a local source map, marked to save the card unmarked with @p areas
// as its regions; the server links it at the next Sync.
ReviewAnchorPlan reviewPlanForServerLink(ReviewAnchorPlan plan, const QList<ReviewArea> &areas, const QString &pdfPath, const QString &pdfSha256);
// Opens the area editor for a new card, starting with @p first. Each Save maps all areas to the
// source; an area that cannot be linked is reported and the editor stays open. False when cancelled.
bool chooseNewReviewCardAreas(QWidget *parent, const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                              const QString &pdfSha256, const QString &cardId, const ReviewArea &first, const QString &question,
                              NewReviewCardAreas *result);
// Starting with several areas, e.g. a text selection over a page break (one area per page).
bool chooseNewReviewCardAreas(QWidget *parent, const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                              const QString &pdfSha256, const QString &cardId, const QList<ReviewArea> &initial, const QString &question,
                              NewReviewCardAreas *result);

// Edits the areas of an existing card on @p pdfPath, a current PDF of its document. A card on a
// document with editable source gets its markers placed again (one segment per area); a card on
// a PDF without source keeps PDF-only areas. With @p placeAgain the editor is the "Place This
// Card Again" action: it always links the areas, even when none changed. With @p wholeDocument it
// is "Cover the Whole Document": it opens with wholeDocumentReviewAreas. With @p offerKind it also
// offers the card's kind (the Cards list's Edit…) and records a kind chosen once saved. True when saved.
bool editReviewCardAreas(QWidget *parent, ReviewStore &store, const QString &docsRoot, const ReviewCard &card,
                         const QString &pdfPath, bool placeAgain, bool wholeDocument = false, bool offerKind = false);
// The default source bundle of this device.
QString defaultReviewSourceBundleRoot();
// Whether a card can be placed again from areas: not a whole-document or basic card.
bool canPlaceReviewCardByAreas(const ReviewCard &card);
// Whether the server asked for this card to be placed again: a source link that failed, or a
// placement whose markers are missing, ambiguous, or cannot be found.
bool reviewCardNeedsPlacingAgain(const ReviewCard &card);
}
#endif
