#include <QSet>
#include <QJsonArray>
/*
    SPDX-FileCopyrightText: 2004 Enrico Ros <eros.kde@email.it>
    SPDX-FileCopyrightText: 2004 Albert Astals Cid <aacid@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    With portions of code from kpdf/kpdf_pagewidget.h by:
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003 Kurt Pfeifle <kpfeifle@danka.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
// This file follows coding style described in kdebase/kicker/HACKING

#ifndef _OKULAR_PAGEVIEW_H_
#define _OKULAR_PAGEVIEW_H_

#include "config-okular.h"
#include "core/area.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/observer.h"
#include "core/view.h"
#include "pageviewutils.h"
#include <QAbstractScrollArea>
#include <QList>
#include <QPainterPath>
#include <QPair>
#include <QRectF>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "okularpart_export.h"
#include "signaturepartutils.h"

class QMenu;
class QMimeData;
class QPainter;
class QJsonObject;
class KActionCollection;
class AnnotationPopup;
struct AgentProposalSidebarEntry;
struct AgentProposalDecisionState;

namespace Thoughts
{
class DocumentFeedbackOverlay;
class ReviewFeedbackEditor;
class ReviewFeedbackOverlay;
}

namespace Okular
{
class Action;
class Document;
class DocumentViewport;
class FormField;
class FormFieldSignature;
class Annotation;
class MovieAction;
class RenditionAction;
class SignatureAnnotation;
}

class PageViewPrivate;

class QGestureEvent;
class QNativeGestureEvent;

/**
 * @short The main view. Handles zoom and continuous mode.. oh, and page
 * @short display of course :-)
 * ...
 */
class PageView : public QAbstractScrollArea, public Okular::DocumentObserver, public Okular::View
{
    Q_OBJECT

public:
    enum class ReviewMaskStyle { Hidden, SidebarHighlight, Revealed };
    void setCommentSourceConnectors(const QJsonArray &connectors);
    PageView(QWidget *parent, Okular::Document *document);
    ~PageView() override;

    bool mapGlobalPosToPagePoint(QPoint globalPos, int *pageNumber, Okular::NormalizedPoint *point) const;

    // Zoom mode ( last 4 are internally used only! )
    enum ZoomMode { ZoomFixed = 0, ZoomFitWidth = 1, ZoomFitPage = 2, ZoomFitAuto = 3, ZoomIn, ZoomOut, ZoomRefreshCurrent, ZoomActual };

    enum ClearMode { ClearAllSelection, ClearOnlyDividers };

    enum FinishSigningResult {
        Success,
        Failed,
        Cancelled,
    };

    enum class TextCopyMode {
        AsProvided,
        WithoutLineBreaks,
    };

    // create actions that interact with this widget
    void setupBaseActions(KActionCollection *ac);
    void setupViewerActions(KActionCollection *ac);
    void setupActions(KActionCollection *ac);
    void setupActionsPostGUIActivated();
    void updateActionState(bool docHasPages, bool docHasFormWidgets);

    // misc methods (from RMB menu/children)
    bool canFitPageWidth() const;
    void fitPageWidth(int page);
    // keep in sync with pageviewutils
    void displayMessage(const QString &message, const QString &details = QString(), PageViewMessage::Icon icon = PageViewMessage::Info, int duration = -1);

    // inherited from DocumentObserver
    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;
    void notifyViewportChanged(bool smoothMove) override;
    void notifyPageChanged(int pageNumber, int changedFlags) override;
    void notifyContentsCleared(int changedFlags) override;
    void notifyZoom(int factor) override;
    bool canUnloadPixmap(int pageNum) const override;
    void notifyCurrentPageChanged(int previous, int current) override;

    // inherited from View
    bool supportsCapability(ViewCapability capability) const override;
    CapabilityFlags capabilityFlags(ViewCapability capability) const override;
    QVariant capability(ViewCapability capability) const override;
    void setCapability(ViewCapability capability, const QVariant &option) override;

    std::vector<std::unique_ptr<Okular::RegularAreaRect>> textSelections(const QPoint start, const QPoint end, int &firstpage);
    std::unique_ptr<Okular::RegularAreaRect> textSelectionForItem(const PageViewItem *item, const QPoint startPoint = QPoint(), const QPoint endPoint = QPoint());

    void reparseConfig();

    KActionCollection *actionCollection() const;
    QAction *toggleFormsAction() const;

    int contentAreaWidth() const;
    int contentAreaHeight() const;
    QPoint contentAreaPosition() const;
    QPoint contentAreaPoint(const QPoint pos) const;
    QPointF contentAreaPoint(const QPointF pos) const;

    Okular::Annotation *focusedAnnotation() const;
    int focusedAnnotationPageNumber() const;

    bool areSourceLocationsShownGraphically() const;
    void setShowSourceLocationsGraphically(bool show);

    void setLastSourceLocationViewport(const Okular::DocumentViewport &vp);
    void clearLastSourceLocationViewport();

    void updateCursor();

    void highlightSignatureFormWidget(const Okular::FormFieldSignature *form);

    void showNoSigningCertificatesDialog(bool nonDateValidCerts);

    Okular::Document *document() const;

    Q_INVOKABLE void setDocsRoot(const QString &docsRoot);
    void setCommentRecordProvider(std::function<QJsonObject(const QString &)> provider);
    // PDF link overrides: @p click opens the Library document chosen for a clicked link
    // (true when it did); @p menu adds the override items to a link's context menu.
    // Both get the page that contains the link.
    using LinkOverrideClickHandler = std::function<bool(const Okular::Action *action, int page)>;
    using LinkOverrideMenuProvider = std::function<void(QMenu *menu, const Okular::Action *action, int page)>;
    void setLinkOverrideHandlers(LinkOverrideClickHandler click, LinkOverrideMenuProvider menu);
    void completeCommentThreadAction(const QString &rootId, bool success);
    QVariantMap annotationRelatedInfo(const QString &annotationId) const;
    Q_INVOKABLE bool pdfLinkAnchorDocument() const;
    Q_INVOKABLE QString createPdfLinkAnchorForSelection(const QString &anchorName);
    Q_INVOKABLE bool selectAgentProposal(const QString &proposalId);
    bool agentProposalOverlaysReady() const;
    Q_INVOKABLE void refreshAgentProposalOverlays();
    void setAgentProposalTextHighlight(int page, std::unique_ptr<Okular::RegularAreaRect> area);
    void clearAgentProposalTextHighlight();
    // Marks @p regions (1-based page, normalized rect) like a text selection and scrolls to the
    // first; nothing is stored. For a passage in an earlier version of a PDF.
    void showPassageRegions(const QList<QPair<int, QRectF>> &regions);
    void openAgentProposalWindow(const AgentProposalSidebarEntry &proposal);
    void showAgentProposalLocationUnavailable(const QString &proposalId);
    void showAgentProposalBuildRequired(const QString &proposalId);
    void setAgentProposalOperationPending(const QString &proposalId, bool pending, const QString &message = QString());
    void showAgentProposalResolutionError(const QString &proposalId, const QString &message);
    void showAgentProposalResolutionSuccess(const QString &proposalId, const QString &message);
    void refreshAgentProposalWindow(const AgentProposalSidebarEntry &proposal);
    Q_INVOKABLE QString openAgentProposalId() const;
    void suppressAgentProposal(const QString &proposalId);
    // Proposals in this PDF that are no longer in their local source (decided
    // since the build): their marks stay hidden across reloads until a
    // rebuild drops them from the PDF.
    void setDecidedAgentProposalIds(const QSet<QString> &proposalIds);
    void restoreAgentProposal(const QString &proposalId);
    // Tells the hover toolbar whether a proposal can be decided now, and why not.
    void setAgentProposalDecisionStateProvider(std::function<AgentProposalDecisionState(const QString &)> provider);
    AgentProposalDecisionState agentProposalDecisionState(const QString &proposalId) const;
    // The check mark of a proposal in viewport coordinates; empty when none is shown.
    Q_INVOKABLE QRect agentProposalMarkerRect(const QString &proposalId) const;
    void hideAgentProposalHoverBar();
    bool isPdfLinkAnchorDocument(QString *paperKey = nullptr) const;
    // The line under @p eventPos when it looks like a section heading (short, and set larger than
    // the page's text, numbered, or named by the PDF outline), normalized; empty otherwise.
    QRectF sectionHeadingLineAt(const PageViewItem *item, const QPointF &eventPos) const;
    bool createPdfLinkAnchor(const QString &anchorName, QString *latexLink = nullptr, QString *error = nullptr);

    void startSigning(Okular::SignatureAnnotation *signatureAnnotation);

public Q_SLOTS:
    void copyTextSelection(PageView::TextCopyMode mode = TextCopyMode::AsProvided) const;
    // The text of the current text selection, or empty.
    QString selectedText() const;
    void annotateTextSelection(Okular::HighlightAnnotation::HighlightType type = Okular::HighlightAnnotation::Highlight);
    void promptCreatePdfLinkAnchor();
    void selectAll();
    // Reads text aloud (cloud voice, system voice as fallback), stopping any
    // other Read Aloud in this process.
    void readAloud(const QString &text);
    // Reads the current text selection; without one, stops reading.
    void readAloudSelection();
    void stopReadAloud();
    bool readingAloud() const;

    void openAnnotationWindow(Okular::Annotation *annotation, int pageNumber);
    void promptReplyToAnnotation(Okular::Annotation *annotation, int pageNumber);
    void closeReviewAnnotationWindow();
    void openReviewAnnotationWindow(Okular::Annotation *annotation, int pageNumber);
    void setReviewSelection(Okular::Annotation *annotation, int pageNumber);
    QVariantMap reviewDocumentPayload() const;
    bool showReviewMask(int page, const QVariantList &rects);
    void setReviewCardHighlights(const QVariantList &cards);
    OKULARPART_EXPORT bool showReviewMasks(const QVariantList &regions, ReviewMaskStyle style = ReviewMaskStyle::Hidden, bool navigate = true);
    // Flip: cover every visible page except the answer regions instead of the
    // answer itself. Clearing the mask turns it off again.
    void setReviewMaskInverted(bool inverted);
    bool reviewMaskInverted() const;
    void clearReviewMask();
    // Clears the Cards list's highlight of a selected card, never a review's answer mask.
    void clearSidebarReviewHighlight();
    // Turns a hidden answer into an outlined one: the same area, now readable inside a blue box.
    void revealReviewMask();
    // A review card's answer areas in reading order (page, then top, then left), for the
    // card's ‹ › arrows; focusing one scrolls it near the top of the view.
    int reviewMaskBlockCount() const;
    bool focusReviewMaskBlock(int index);
    // Answer feedback (review-card-feedback.md): the card under review's liked and disliked
    // passages, drawn only while its answer is revealed (not while masked, nor in reading).
    OKULARPART_EXPORT Thoughts::ReviewFeedbackOverlay *reviewFeedback() const;
    OKULARPART_EXPORT bool reviewFeedbackVisible() const;
    // The card presented for review here (masked or revealed, not a sidebar highlight), else empty.
    QString reviewSessionCardId() const;
    // The text selection as one rectangle per line (1-based page, normalized), in reading order.
    OKULARPART_EXPORT QList<QPair<int, QRectF>> textSelectionLines() const;
    // What feedback is about: the text selection, or a rectangle while its menu is open.
    QList<QPair<int, QRectF>> feedbackLines() const;
    QString feedbackText() const;
    bool feedbackCovers(int page, const QPointF &centre) const;
    void setFeedbackArea(const QRect &selectionRect, const QString &text);
    OKULARPART_EXPORT bool reviewFeedbackSelectionQualifies() const;
    OKULARPART_EXPORT QJsonObject reviewFeedbackForSelection(const QString &polarity, const QString &comment, QString *error = nullptr) const;
    // Like / Dislike: the record at once, with an empty comment; the selection is cleared.
    OKULARPART_EXPORT bool addReviewFeedbackFromSelection(const QString &polarity, QString *error = nullptr);
    // Comment…: the editor for a new record (Like preselected); nothing is saved until Save.
    OKULARPART_EXPORT Thoughts::ReviewFeedbackEditor *openNewReviewFeedbackEditor();
    OKULARPART_EXPORT Thoughts::ReviewFeedbackEditor *openReviewFeedbackEditor(const QString &feedbackId);
    // Writes the tombstone and shows "Feedback deleted · Undo" for 5 s.
    OKULARPART_EXPORT bool deleteReviewFeedback(const QString &feedbackId);
    OKULARPART_EXPORT void addReviewFeedbackActions(QMenu *menu);
    // A normalized rectangle of a 1-based page in content coordinates; empty when not shown.
    OKULARPART_EXPORT QRectF reviewFeedbackRect(int page, const QRectF &rect) const;
    // Opens the editor of the highlight at @p contentPoint (the newest on top); false when none.
    OKULARPART_EXPORT bool openReviewFeedbackAt(const QPointF &contentPoint);
    // Document feedback (document-feedback.md): liked and disliked passages of the Library PDF
    // being read, drawn in normal reading on every page (not while a review card's answer is
    // masked or revealed). Any text selection gets Like / Dislike / Comment… when the PDF has a
    // known document ID and the selection does not qualify for answer feedback.
    OKULARPART_EXPORT Thoughts::DocumentFeedbackOverlay *documentFeedback() const;
    OKULARPART_EXPORT bool documentFeedbackVisible() const;
    OKULARPART_EXPORT bool reviewingCard() const;
    OKULARPART_EXPORT bool answerMasked() const;
    // ⌘Z for the user's own annotations in this view: whether the newest one can be taken back
    // (nothing of the PDF's own undo history came after it), when it was made, and taking it back
    // (a pencil stroke at a time, else the whole like, dislike, question, comment or text box).
    OKULARPART_EXPORT bool canUndoAnnotation() const;
    OKULARPART_EXPORT qint64 lastAnnotationAt() const;
    OKULARPART_EXPORT bool undoLastAnnotation();
    QPainterPath hiddenAnswerArea() const;
    OKULARPART_EXPORT bool documentFeedbackSelectionQualifies() const;
    // A question about the passage is document feedback, also inside a revealed review answer.
    OKULARPART_EXPORT bool documentQuestionSelectionQualifies() const;
    OKULARPART_EXPORT QJsonObject documentFeedbackForSelection(const QString &polarity, const QString &comment, QString *error = nullptr, bool question = false) const;
    OKULARPART_EXPORT bool addDocumentFeedbackFromSelection(const QString &polarity, QString *error = nullptr);
    OKULARPART_EXPORT Thoughts::ReviewFeedbackEditor *openNewDocumentFeedbackEditor(const QString &kind = QString());
    // At the passage, or at @p anchor (global) for feedback made in an HTML attachment.
    OKULARPART_EXPORT Thoughts::ReviewFeedbackEditor *openDocumentFeedbackEditor(const QString &feedbackId, const QRect &anchor = QRect());
    // Goes to the passage of @p feedbackId; with @p edit also opens its editor there.
    OKULARPART_EXPORT bool showDocumentFeedback(const QString &feedbackId, bool edit);
    OKULARPART_EXPORT bool deleteDocumentFeedback(const QString &feedbackId);
    OKULARPART_EXPORT bool resolveDocumentFeedback(const QString &feedbackId, bool resolved);
    OKULARPART_EXPORT bool openDocumentFeedbackAt(const QPointF &contentPoint);
    // Change passage (the feedback editor's button): moves a highlight to another passage. Only for
    // an own highlight drawn on the displayed PDF: the card's own answer feedback while its answer
    // is revealed (not one shared from another card, not one not found in the answer), or document
    // feedback of the PDF being read (not a drawing, not one made in an HTML attachment). The
    // highlight is hidden and its passage selected; a banner asks for the new passage, with Save
    // passage and Cancel. @p polarity, @p kind and @p comment are the editor's.
    OKULARPART_EXPORT bool canChangeReviewFeedbackPassage(const QString &feedbackId);
    OKULARPART_EXPORT bool canChangeDocumentFeedbackPassage(const QString &feedbackId);
    OKULARPART_EXPORT bool startPassageChange(bool documentFeedback, const QString &feedbackId, const QString &polarity, const QString &kind, const QString &comment);
    OKULARPART_EXPORT bool passageChangeActive() const;
    OKULARPART_EXPORT QString passageChangeFeedbackId() const;
    // Save passage: the text selection becomes the passage (it must qualify as for Like / Dislike;
    // otherwise a refusal and the mode stays). The same passage saves only an edit; another writes
    // the new record and the old one's tombstone in one store write, then "Passage changed · Undo".
    OKULARPART_EXPORT bool savePassageChange(QString *error = nullptr);
    // Cancel, Escape, masking the answer, another card, closing the document: nothing changes.
    OKULARPART_EXPORT void cancelPassageChange();
    void refreshOpenAnnotationRelatedInfo();
    void completeAnnotationRelatedOperation(const QString &annotationId, const QVariantMap &result);
    void completeAnnotationReplyOperation(const QString &parentAnnotationId, const QVariantMap &result);
    void reloadForms();

    void slotSelectPage();

    void slotAction(Okular::Action *action);
    void slotMouseAction(Okular::Action *action, Okular::FormField *form, Okular::Document::MouseEventType fieldMouseEventType);
    void slotFormChanged(int pageNumber);

    void externalKeyPressEvent(QKeyEvent *e);
#if HAVE_NEW_SIGNATURE_API
    PageView::FinishSigningResult finishSigning();
#endif

Q_SIGNALS:
    void reviewFeedbackChanged();
    void documentFeedbackChanged();
    // The user gave new feedback or a new comment, already in the document.
    void documentFeedbackAdded(const QString &feedbackId);
    // What canUndoAnnotation() answers may have changed.
    void annotationUndoChanged();
    // Feedback was deleted while this view is hidden. A slot that offers @p undo itself (the HTML
    // attachment in its place) sets @p taken, and this view's banner is not made.
    void feedbackUndoOffered(const std::function<bool(QString *error)> &undo, bool *taken);
    void annotationAdded(Okular::Annotation *annotation, int pageNumber);
    void annotationWindowOpening();
    void rightClick(const Okular::Page *, const QPoint);
    // A right-click menu of its own is about to open, for the Part to add to.
    void contextMenuReady(QMenu *menu);
    void mouseBackButtonClick();
    void mouseForwardButtonClick();
    void escPressed();
    void fitWindowToPage(const QSize pageViewPortSize, const QSize pageSize);
    void triggerSearch(const QString &text);
    void annotationClicked(Okular::Annotation *annotation, int pageNumber);
    void commentEditorClosing(Okular::Annotation *annotation, int pageNumber);
    void commentReplyRequested(const QString &annotationId, int page, const QString &contents);
    void commentRelatedMetadataSaveRequested(const QString &annotationId, int page, const QString &contents, const QVariantMap &metadataPatch);
    void commentThreadActionSaveRequested(const QString &rootId, bool needsAction);
    void commentRelatedChatGptUrlSaveRequested(const QString &annotationId, const QString &url, bool openAfterSave);
    void commentRelatedOpenCodexRequested(const QString &pdfPath, const QString &annotationId);
    void commentRelatedOpenChatGptRequested(const QString &url);
    void commentRelatedOpenSourceRequested(const QString &sourcePath, int line);
    void openTexSourceRequested(int pageNumber, double normalizedX, double normalizedY, const QString &selectedText);
    void commentRelatedOpenOverviewRequested(const QString &pdfPath, const QString &annotationId, int page);
    void agentProposalReviewRequested(const QString &proposalId);
    void agentProposalOverlaysReloaded();
    void agentProposalResolutionRequested(const QString &proposalId, const QString &action);
    void agentProposalOpenSourceRequested(const QString &sourcePath, int line);
    void manualReviewCardRequested(const QString &pdfPath, int page, const QVariantList &rect, bool textSelection);
    // A text selection running over several pages: one area ({page, rect}) per page, in page order.
    void manualReviewCardAreasRequested(const QString &pdfPath, const QVariantList &areas, bool textSelection);
    // Add Review Card for This Section… on a section heading (@p rect: its line, normalized, on one-based @p page).
    void sectionReviewCardRequested(const QString &pdfPath, int page, const QVariantList &rect);
    void reviewCardEditRequested();
    /**
     * Requests opening of a newly signed file
     * \param filePath path to signed file
     * \param pageNumber page to show (1-indexed)
     */
    void requestOpenNewlySignedFile(const QString &filePath, int pageNumber);
    void signingStarted();
    void signingFinished();
    // Developer-log events of Read Aloud (metadata only).
    void readAloudDeveloperEvent(const QString &event, const QJsonObject &metadata);

protected:
    bool event(QEvent *event) override;
    void changeEvent(QEvent *event) override;

    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;
    bool gestureEvent(QGestureEvent *e);
    bool nativeGestureEvent(QNativeGestureEvent *e);

    // mouse / keyboard events
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void inputMethodEvent(QInputMethodEvent *) override;
    void wheelEvent(QWheelEvent *) override;

    void paintEvent(QPaintEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;
    void continuousZoom(double delta);
    void continuousZoomEnd();
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

    bool viewportEvent(QEvent *e) override;

    void scrollContentsBy(int dx, int dy) override;

private:
    void refreshAnnotationRelatedInfo(const QString &annotationId, const QString &status = QString());
    // draw background and items on the opened qpainter
    void drawDocumentOnPainter(const QRect contentsRect, QPainter *p);
    void paintLinkIndicators(QPainter *painter, const QRect &contentsRect) const;
    void paintReviewSelection(QPainter *painter, const QRect &contentsRect) const;
    void paintReviewMask(QPainter *painter, const QRect &contentsRect) const;
    QRect reviewFeedbackGlobalRect(const QString &feedbackId) const;
    QRect feedbackGlobalRect(const QList<QPair<int, QRectF>> &rects) const;
    // "Feedback deleted · Undo" for 5 s; @p undo writes the restoring revision.
    void showFeedbackUndo(const std::function<bool(QString *error)> &undo, const QString &label = QString());
    // Ends a passage change whose highlight, card or answer is gone.
    void endPassageChangeIfStale();
    void showPassageChangeBanner();
    void paintDocumentFeedback(QPainter *painter, const QRect &contentsRect) const;
    // The pencil: a stroke on the page under the pointer, one drawing for the strokes of a page while
    // the tool stays on, and Annotations on the last drawing when it goes off.
    // Drawings show whatever "Visible markup" hides: that choice is about highlights.
    bool drawingsVisible() const;
    void beginDrawingStroke(const QPoint &eventPos);
    void extendDrawingStroke(const QPoint &eventPos);
    void finishDrawingStroke();
    // The pencil's eraser (the menu of its button): a drag removes each stroke it touches, of any
    // drawing shown, and one Undo puts back what one drag removed.
    void beginErasing(const QPoint &eventPos);
    void eraseAlong(const QPoint &eventPos);
    void finishErasing();
    // The pencil's colour and eraser, chosen in the menu of its button: one choice for every tab,
    // remembered, and the pencil goes on with it.
    void choosePencil(const QString &colour, bool eraser);
    void slotFollowPencil();
    void rememberAnnotation(const QString &feedbackId, bool stroke = false);
    int newestAnnotationStep() const;
    void stopDrawing();
    void endDrawing();
    // The Text tool: a click on the page away from text boxes places a box to type in (or ends the
    // typing or the selection going on), a click on a box selects it, a second click or a
    // double-click edits it, and a drag moves it on its page. Delete removes the selected box, Esc
    // ends what is going on and then the tool. Annotations opens on the last new box when it goes off.
    void pressTextBoxTool(const QPoint &eventPos);
    void moveTextBoxTool(const QPoint &eventPos);
    void releaseTextBoxTool(const QPoint &eventPos);
    QString textBoxAt(const QPoint &eventPos) const;
    void selectTextBox(const QString &feedbackId);
    // Opens the editor of the box @p feedbackId, or of a new box at @p eventPos when it is empty.
    bool editTextBox(const QString &feedbackId, const QPoint &eventPos = QPoint());
    void addTextBoxAt(const QPoint &eventPos);
    // Keeps what was typed: a new box, the next revision, or the box deleted when it was emptied.
    // The box that is there afterwards, or empty.
    QString finishTextBoxEditor();
    void placeTextBoxEditor();
    // Where @p text goes on @p page with its top left at @p topLeft (normalized), inside the page.
    QRectF textBoxRectAt(int page, const QPointF &topLeft, double fontSize, const QString &text) const;
    double defaultTextBoxFontSize(int page) const;
    void stopTextBoxes();
    void endTextBoxes();
    // The Text tool's button: a new box in the middle of the page that shows most, ready to type,
    // below any box already there.
    bool insertTextBox();
    // Why a text box cannot be placed now: a masked answer, or a PDF that is not a Library document.
    QString textBoxRefusal() const;
    void paintCommentSourceConnectors(QPainter *painter, const QRect &contentsRect) const;
    void reloadAgentProposalOverlays();
    void paintAgentProposalOverlays(QPainter *painter, const QRect &contentsRect) const;
    QList<QPair<QString, QRectF>> agentProposalMarkers() const;
    QString agentProposalMarkerAt(const QPoint &eventPos) const;
    void updateAgentProposalHover(const QPoint &viewportPos, Qt::MouseButtons buttons);
    void showAgentProposalHoverBar();
    // The bar of one marker at once (a press held on it, or a hover that rested).
    void showAgentProposalHoverBarFor(const QString &proposalId);
    bool agentProposalHoverBridgeContains(const QPoint &viewportPos) const;
    // Hides the toolbar when its marker moved (scroll, zoom, page change) or went away.
    void checkAgentProposalHoverAnchor();
    QString agentProposalAt(const PageViewItem *pageItem, const QPoint &eventPos) const;
    Q_INVOKABLE QPainterPath reviewSelectionPath() const;
    Q_INVOKABLE int reviewSelectionPageNumber() const;
    // update item width and height using current zoom parameters
    void updateItemSize(PageViewItem *item, int colWidth, int rowHeight);
    // return the widget placed on a certain point or 0 if clicking on empty space
    PageViewItem *pickItemOnPoint(int x, int y);
    // start / modify / clear selection rectangle
    void selectionStart(const QPoint pos, const QColor &color, bool aboveAll = false);
    void selectionClear(const ClearMode mode = ClearAllSelection);
    void addPopupNote(PageViewItem *item, const QPoint &eventPos);
    QMimeData *getTableContents() const;
    void drawTableDividers(QPainter *screenPainter);
    void guessTableDividers();
    // update either text or rectangle selection
    void updateSelection(const QPoint pos);
    // compute the zoom factor value for FitWidth and FitPage mode
    double zoomFactorFitMode(ZoomMode mode);
    // update internal zoom values and end in a slotRelayoutPages();
    void updateZoom(ZoomMode newZoomMode);
    // update the text on the label using global zoom value or current page's one
    void updateZoomText();
    // update the text enabled status of the zoom actions
    void updateZoomActionsEnabledStatus();
    // update view mode (single, facing...)
    void updateViewMode(const int nr);
    void textSelectionClear();
    // updates cursor
    void updateCursor(const QPoint p);

    void moveMagnifier(const QPoint p);
    void updateMagnifier(const QPoint p);

    int viewColumns() const;

    void center(int cx, int cy, bool smoothMove = false);
    void scrollTo(int x, int y, bool smoothMove = false);

    void toggleFormWidgets(bool on);

    void resizeContentArea(const QSize newSize);
    void updatePageStep();

    void addSearchWithinDocumentAction(QMenu *menu, const QString &searchText);
    void addWebShortcutsMenu(QMenu *menu, const QString &text);
    QMenu *createProcessLinkMenu(PageViewItem *item, const QPoint eventPos);
    bool addAnnotationActionsForPoint(AnnotationPopup &annotPopup, PageViewItem *pageItem, const QPoint eventPos, QMenu **menu);
    // used when selecting stuff, makes the view scroll as necessary to keep the mouse inside the view
    void scrollPosIntoView(const QPoint pos);
    QPoint viewportToContentArea(const Okular::DocumentViewport &vp) const;

    // called from slots to turn off trim modes mutually exclusive to id
    void updateTrimMode(int except_id);

    // handle link clicked
    bool mouseReleaseOverLink(const Okular::ObjectRect *rect) const;

    void createAnnotationsVideoWidgets(PageViewItem *item, const QList<Okular::Annotation *> &annotations);

    // Update speed of animated smooth scroll transitions
    void updateSmoothScrollAnimationSpeed();

    /*
     * returns the continuous mode value of the current document, by either:
     * - if the continuous mode action is initialized, then we return its associated value
     * - if not, then we will fallback to the default settings
     */
    bool getContinuousMode() const;

    // Zoom around the point given as zoomCenter
    // zoomCenter is given in viewport coordinates
    // newZoom is the intended new zoom level.  A value of 0.0 means: Don't change the current zoom level.
    // The zoomMode is set to newZoomMode.
    void zoomWithFixedCenter(ZoomMode newZoomMode, QPointF zoomCenter, float newZoom = 0.0);

    // don't want to expose classes in here
    void styleScrollBars();

    class PageViewPrivate *d;

private Q_SLOTS:
    // used to decouple the notifyViewportChanged calle
    void slotRealNotifyViewportChanged(bool smoothMove);
    // activated either directly or via queued connection on notifySetup
    void slotRelayoutPages();
    // activated by the resize event delay timer
    void delayedResizeEvent();
    // activated either directly or via the contentsMoving(int,int) signal
    void slotRequestVisiblePixmaps(int newValue = -1);
    // activated by the autoscroll timer (Shift+Up/Down keys)
    void slotAutoScroll();
    // activated by the dragScroll timer
    void slotDragScroll();
    // show the welcome message
    void slotShowWelcome();
    // activated by left click timer
    void slotShowMoveCursor();

    void slotHandleWebShortcutAction();
    void slotConfigureWebShortcuts();

    // connected to local actions (toolbar, menu, ..)
    void slotZoom();
    void slotZoomIn();
    void slotZoomOut();
    void slotZoomActual();
    void slotFitToWidthToggled(bool);
    void slotFitToPageToggled(bool);
    void slotAutoFitToggled(bool);
    void slotViewMode(QAction *action);
    void slotContinuousToggled();
    void slotReadingDirectionToggled(bool leftToRight);
    void slotUpdateReadingDirectionAction();
    void slotSetMouseNormal();
    void slotSetMouseZoom();
    void slotSetMouseMagnifier();
    void slotSetMouseSelect();
    void slotSetMouseTextSelect();
    void slotSetMouseDraw();
    void slotSetMouseTextBox();
    void slotSetMouseTableSelect();
    void slotFollowSelectionTool();
    void slotSignature();
    void slotAutoScrollUp();
    void slotAutoScrollDown();
    void slotScrollUp(int nSteps = 0);
    void slotScrollDown(int nSteps = 0);
    void slotRotateClockwise();
    void slotRotateCounterClockwise();
    void slotRotateOriginal();
    void slotTrimMarginsToggled(bool);
    void slotTrimToSelectionToggled(bool);
    void slotToggleForms();
    void slotRefreshPage();
#if HAVE_SPEECH
    void slotSpeakDocument();
    void slotSpeakFromCurrentPage();
    void slotSpeakCurrentPage();
    void slotStopSpeaks();
    void slotPauseResumeSpeech();
#endif
    void slotAnnotationWindowDestroyed(QObject *window);
    void slotProcessMovieAction(const Okular::MovieAction *action);
    void slotProcessRenditionAction(const Okular::RenditionAction *action);
    void slotFitWindowToPage();
};

#endif

/* kate: replace-tabs on; indent-width 4; */
