#include "reviewplacement.h"
#include "thoughtsplatform.h"
#include "feedbackversion.h"
#include "reviewmaskgeometry.h"
#include "documentfeedbackoverlay.h"
#include "readingoverlaylayer.h"
#include "readingoverlaylist.h"
#include "readingoverlays.h"
#include "reviewfeedbackeditor.h"
#include "reviewfeedbackoverlay.h"
#include "../gui/textlinepath.h"
/*
    SPDX-FileCopyrightText: 2004-2005 Enrico Ros <eros.kde@email.it>
    SPDX-FileCopyrightText: 2004-2006 Albert Astals Cid <aacid@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    With portions of code from kpdf/kpdf_pagewidget.cc by:
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003 Dirk Mueller <mueller@kde.org>
    SPDX-FileCopyrightText: 2004 James Ots <kde@jamesots.com>
    SPDX-FileCopyrightText: 2011 Jiri Baum - NICTA <jiri@baum.com.au>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pageview.h"
#include "readertheme.h"

// qt/kde includes
#include <QActionGroup>
#include <QApplication>
#include <QStyleHints>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFile>
#include <QGestureEvent>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QHash>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QHideEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QDomElement>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QSet>
#include <QStyle>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
#include <QUuid>

#include <KActionCollection>
#include <KActionMenu>
#include <KConfigWatcher>
#include <KIO/CommandLauncherJob>
#include <KIO/JobUiDelegate>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSelectAction>
#include <KStandardAction>
#include <KStringHandler>
#include <KToggleAction>
#include <KUriFilter>
#include <QAction>
#include <QDebug>
#include <QIcon>

// system includes
#include <array>
#include <math.h>
#include <stdlib.h>
#include <memory>
#include <optional>

// local includes
#include "agentproposalhoverbar.h"
#include "agentproposalwindow.h"
#include "agentproposaloverlayutils.h"
#include "../shell/agentproposalparser.h"
#include "annotationpopup.h"
#include "annotwindow.h"
#include "colormodemenu.h"
#include "commentfragmentgroup.h"
#include "commentfragmentids.h"
#include "docscommentsynchronizer.h"
#include "core/annotations.h"
#include "cursorwraphelper.h"
#include "formwidgets.h"
#include "gui/debug_ui.h"
#include "gui/guiutils.h"
#include "gui/pagepainter.h"
#include "gui/priorities.h"
#include "okmenutitle.h"
#include "pageviewannotator.h"
#include "pageviewmouseannotation.h"
#include "pageviewutils.h"
#include "readaloud.h"
#include "readaloudbar.h"
#include "toggleactionmenu.h"
#if HAVE_SPEECH
#include "tts.h"
#endif
#include "core/action.h"
#include "core/audioplayer.h"
#include "core/document_p.h"
#include "core/form.h"
#include "core/generator.h"
#include "core/misc.h"
#include "core/movie.h"
#include "core/page.h"
#include "core/utils.h"
#include "core/page_p.h"
#include "core/sourcereference.h"
#include "core/tile.h"
#include "kleopatraintegration.h"
#include "magnifierview.h"
#include "settings.h"
#include "settings_core.h"
#include "signaturepartutils.h"
#include "url_utils.h"
#include "videowidget.h"

// Without this the code in DocumentPrivate::setDefaultViewMode
// that casts Okular::Generator::PageLayout to View::ViewModeModality data won't work
// Would be possibly better in document.cpp but we can't do it there since Okular::Settings doesn't exist in core
// This is a bit of a missing feature/documentation in what the QVariant in View API mean
static_assert(static_cast<int>(Okular::Generator::PageLayout::SinglePage) == Okular::Settings::EnumViewMode::Single);
static_assert(static_cast<int>(Okular::Generator::PageLayout::TwoPageLeft) == Okular::Settings::EnumViewMode::Facing);
static_assert(static_cast<int>(Okular::Generator::PageLayout::TwoPageRight) == Okular::Settings::EnumViewMode::FacingFirstCentered);

static const int pageflags = PagePainter::Accessibility | PagePainter::EnhanceLinks | PagePainter::EnhanceImages | PagePainter::Highlights | PagePainter::TextSelection | PagePainter::Annotations;

static const std::array<float, 16> kZoomValues {0.12, 0.25, 0.33, 0.50, 0.66, 0.75, 1.00, 1.25, 1.50, 2.00, 4.00, 8.00, 16.00, 25.00, 50.00, 100.00};

// This is the length of the text that will be shown when the user is searching for a specific piece of text.
static const int searchTextPreviewLength = 21;
static constexpr int popupNoteToolId = 7;

static QString normalizedCommentId(QString id)
{
    if (id.startsWith(QLatin1String("id:"))) {
        id.remove(0, 3);
    }
    return Thoughts::logicalCommentId(id);
}

static QString sidecarCommentId(const QJsonObject &record)
{
    QString id = record.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        id = record.value(QStringLiteral("comment_key")).toString(record.value(QStringLiteral("review_key")).toString());
    }
    return normalizedCommentId(id);
}

static bool hasCommentWindow(const Okular::Annotation *annotation)
{
    if (!annotation || (annotation->flags() & Okular::Annotation::Hidden)) {
        return false;
    }

    const Okular::Annotation::SubType subType = annotation->subType();
    return subType != Okular::Annotation::AWidget && subType != Okular::Annotation::AFileAttachment && subType != Okular::Annotation::AMovie && subType != Okular::Annotation::AScreen && subType != Okular::Annotation::ARichMedia;
}

static bool isDraggablePopupNote(const Okular::Annotation *annotation)
{
    if (!annotation || annotation->subType() != Okular::Annotation::AText || !annotation->canBeMoved()) {
        return false;
    }

    return static_cast<const Okular::TextAnnotation *>(annotation)->textType() == Okular::TextAnnotation::Linked;
}

static Okular::Annotation *commentAnnotationAtPoint(const PageViewItem *pageItem, const QPoint &eventPos)
{
    if (!pageItem) {
        return nullptr;
    }

    const double nX = pageItem->absToPageX(eventPos.x());
    const double nY = pageItem->absToPageY(eventPos.y());
    const QRect &itemRect = pageItem->uncroppedGeometry();
    const QList<Okular::ObjectRect *> &objectRects = pageItem->page()->objectRects();
    for (auto it = objectRects.crbegin(); it != objectRects.crend(); ++it) {
        const Okular::ObjectRect *objectRect = *it;
        if (objectRect->objectType() != Okular::ObjectRect::OAnnotation || !objectRect->contains(nX, nY, itemRect.width(), itemRect.height())) {
            continue;
        }

        Okular::Annotation *annotation = static_cast<const Okular::AnnotationObjectRect *>(objectRect)->annotation();
        if (!Okular::Settings::highlightComments() && Okular::AnnotationUtils::isCommentAnnotation(annotation)) {
            continue;
        }
        if (hasCommentWindow(annotation)) {
            return annotation;
        }
    }

    return nullptr;
}

static QString canonicalOrAbsolutePath(const QFileInfo &info)
{
    const QString canonicalPath = info.canonicalFilePath();
    return canonicalPath.isEmpty() ? info.absoluteFilePath() : canonicalPath;
}

static bool usesManagedCommentThreads(const Okular::Document *document, const QString &docsRoot)
{
    if (!document || docsRoot.isEmpty() || !document->currentDocument().isLocalFile()) {
        return false;
    }

    const QFileInfo documentInfo(document->currentDocument().toLocalFile());
    const QFileInfo documentDirectoryInfo(documentInfo.absolutePath());
    const QFileInfo currentPdfsInfo(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    return canonicalOrAbsolutePath(documentDirectoryInfo) == canonicalOrAbsolutePath(currentPdfsInfo);
}

static void collapseManagedCommentReplyAnnotations(Okular::Document *document, const QString &docsRoot)
{
    if (!usesManagedCommentThreads(document, docsRoot)) {
        return;
    }

    QSet<const Okular::Annotation *> anchoredAnnotations;
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        const Okular::Page *page = document->page(pageNumber);
        if (!page) {
            continue;
        }
        for (const Okular::ObjectRect *objectRect : page->objectRects()) {
            if (objectRect && objectRect->objectType() == Okular::ObjectRect::OAnnotation) {
                anchoredAnnotations.insert(static_cast<const Okular::AnnotationObjectRect *>(objectRect)->annotation());
            }
        }
    }

    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        const Okular::Page *page = document->page(pageNumber);
        if (!page) {
            continue;
        }
        for (Okular::Annotation *annotation : page->annotations()) {
            if (!annotation || !annotation->previousAnnotation()) {
                continue;
            }
            const Thoughts::CommentFragment root = Thoughts::commentThreadRoot(document, annotation, static_cast<int>(pageNumber));
            if (root.annotation && root.annotation != annotation && anchoredAnnotations.contains(root.annotation)
                && !(root.annotation->flags() & Okular::Annotation::Hidden)) {
                annotation->setFlags(annotation->flags() | Okular::Annotation::Hidden);
            }
        }
    }
}

static inline double normClamp(double value, double def)
{
    return (value < 0.0 || value > 1.0) ? def : value;
}

struct TableSelectionPart {
    PageViewItem *item;
    Okular::NormalizedRect rectInItem;
    Okular::NormalizedRect rectInSelection;

    TableSelectionPart(PageViewItem *item_p, const Okular::NormalizedRect &rectInItem_p, const Okular::NormalizedRect &rectInSelection_p);
};

TableSelectionPart::TableSelectionPart(PageViewItem *item_p, const Okular::NormalizedRect &rectInItem_p, const Okular::NormalizedRect &rectInSelection_p)
    : item(item_p)
    , rectInItem(rectInItem_p)
    , rectInSelection(rectInSelection_p)
{
}

// The proposal hover toolbar appears once the pointer rests on a marker this
// long, stays this long after the pointer leaves the marker and the toolbar,
// and does not appear right after the page scrolled under the pointer.
static constexpr int AgentProposalHoverShowDelayMs = 200;
static constexpr int AgentProposalHoverGraceMs = 350;
static constexpr int AgentProposalHoverScrollQuietMs = 300;
// A press on a proposal's marker this soon after a wheel or trackpad scroll stops the page; it does not accept.
static constexpr int AgentProposalPressScrollQuietMs = 100;

// structure used internally by PageView for data storage
class PageViewPrivate
{
public:
    explicit PageViewPrivate(PageView *qq);

    FormWidgetsController *formWidgetsController();
#if HAVE_SPEECH
    OkularTTS *tts();
#endif
    QString selectedText() const;

    // the document, pageviewItems and the 'visible cache'
    PageView *q;
    Okular::Document *document = nullptr;
    QString docsRoot;
    std::function<QJsonObject(const QString &)> commentRecordProvider;
    PageView::LinkOverrideClickHandler linkOverrideClick;
    PageView::LinkOverrideMenuProvider linkOverrideMenu;
    QList<Thoughts::AgentProposalOverlay> agentProposalOverlays;
    QString selectedAgentProposalId;
    QString pressedAgentProposalMarker;
    // A press on a ✓ marker accepts on release. Held still for the press-and-hold interval it
    // shows the marker's bar (Open / Accept / Reject) instead; moved past the drag distance,
    // scrolled meanwhile, or made while the page was still scrolling, it does nothing.
    QTimer *agentProposalHoldTimer = nullptr;
    bool agentProposalPressHeld = false;
    bool agentProposalPressCancelled = false;
    bool agentProposalTextHighlight = false;
    // The passage a link to a place shows (showPlace): a text selection of its own, gone with
    // the next click or when placeHighlightTimer fires.
    bool placeHighlight = false;
    QTimer *placeHighlightTimer = nullptr;
    QSet<QString> suppressedAgentProposalIds;
    QSet<QString> decidedAgentProposalIds;
    QPointer<AgentProposalWindow> agentProposalWindow;
    // The passage the open proposal window goes beside (page, normalized rect), until placed.
    struct AgentProposalPlacement {
        QString proposalId;
        int page = -1;
        QRectF rect;
    };
    std::optional<AgentProposalPlacement> agentProposalPlacement;
    quint64 agentProposalReloadGeneration = 0;
    bool agentProposalOverlaysReady = false;
    // Developer log: a burst of overlay readings, from its first start until one is delivered.
    QElapsedTimer agentProposalReloadClock;
    int agentProposalReloadsSuperseded = 0;
    // The document the places are (being) read for, whether a reading is under way, and whether
    // another one was asked for meanwhile (reloadAgentProposalOverlays(true)).
    QUrl agentProposalOverlaysUrl;
    bool agentProposalReloadRunning = false;
    bool agentProposalReloadAgain = false;
    // The proposals the PDF's source artifact says it was built with, when one matches the PDF.
    QSet<QString> builtAgentProposalIds;
    bool builtAgentProposalIdsKnown = false;
    // Open / Accept / Reject next to a hovered or held marker.
    QPointer<AgentProposalHoverBar> agentProposalHoverBar;
    QTimer *agentProposalHoverShowTimer = nullptr;
    QTimer *agentProposalHoverHideTimer = nullptr;
    QString agentProposalHoverCandidate;
    // The marker the bar belongs to, in viewport coordinates.
    QRect agentProposalHoverMarker;
    QPoint agentProposalHoverPointer;
    bool agentProposalHoverPointerValid = false;
    int agentProposalHoverPage = -1;
    QElapsedTimer agentProposalLastScroll;
    // The last wheel or trackpad scroll the page received (a press right after it stops the page).
    QElapsedTimer agentProposalLastWheel;
    std::function<AgentProposalDecisionState(const QString &)> agentProposalDecisionStateProvider;
    QList<PageViewItem *> items;
    QList<PageViewItem *> visibleItems;
    MagnifierView *magnifierView = nullptr;

    // view layout (columns in Settings), zoom and mouse
    PageView::ZoomMode zoomMode = PageView::ZoomFitWidth;
    float zoomFactor = 1.0;
    QPoint mouseGrabOffset;
    QPointF mousePressPos;
    QPointF mouseSelectPos;
    QPointF previousMouseMovePos;
    qreal mouseMidLastY = 0.0;
    bool mouseSelecting = false;
    QRect mouseSelectionRect;
    QColor mouseSelectionColor;
    bool mouseTextSelecting = false;
    // A text-tool drag that caught no text: the rectangle it covers is the selection (a figure).
    bool textDragArea = false;
    Okular::Annotation *mouseCommentCandidate = nullptr;
    int mouseCommentCandidatePage = -1;
    QSet<int> pagesWithTextSelection;
    bool mouseOnRect = false;
    int mouseMode = 0;
    // The pencil: the tool it replaced, the page (1-based, 0 when there is none) and points of the
    // stroke being drawn, and the drawings made since it went on, oldest first.
    int modeBeforeDrawing = 0;
    int strokePage = 0;
    Thoughts::DocumentFeedback::Stroke stroke;
    QStringList drawings;
    // The pencil's menu (its colours, then the eraser) and, during an eraser drag, where the pointer
    // was and each drawing it changed: as it was before, and the revision the eraser wrote last.
    QMenu *pencilMenu = nullptr;
    QList<QAction *> pencilColours;
    QAction *aPencilEraser = nullptr;
    struct Erased {
        QJsonObject before;
        int revision = 0;
    };
    bool erasing = false;
    QPointF eraserAt;
    QHash<QString, Erased> erased;
    // What the reader shows instead of the pages (setShownInstead), what the pencil does on an HTML
    // page, and whether the document has pages (updateActionState), for the tools' state.
    PageView::ShownInstead shownInstead = PageView::ShownInstead::Nothing;
    std::function<void()> htmlPagePencil;
    bool hasPages = false;
    // The user's annotations in this view, newest last, for ⌘Z: the record, whether the step was a
    // pencil stroke (taken back one at a time), the PDF's own undo position then (a later change of
    // that kind is undone first) and when. An eraser drag is one step for every drawing it changed.
    struct AnnotationStep {
        QString feedbackId;
        bool stroke = false;
        int documentHistory = 0;
        qint64 at = 0;
        QHash<QString, Erased> erased;
    };
    QList<AnnotationStep> annotationSteps;
    // The Text tool (it replaces a tool as the pencil does, in modeBeforeDrawing): the editor of the
    // box being typed, that box (empty for a new one) and where a new one starts, the page of the box
    // being typed or moved, the box under the press, the place of the box being moved, whether a
    // click just ended the typing, and the boxes made since the tool went on.
    QPointer<QTextEdit> textBoxEditor;
    QString textBoxEditing;
    QPointF textBoxStart;
    double textBoxStartFontSize = 0.0;
    int textBoxPage = 0;
    QString textBoxPressed;
    bool textBoxPressedSelected = false;
    QPoint textBoxPressPos;
    QRectF textBoxMoved;
    bool textBoxJustFinished = false;
    QStringList textBoxes;
    MouseAnnotation *mouseAnnotation = nullptr;

    // table selection
    QList<double> tableSelectionCols;
    QList<double> tableSelectionRows;
    QList<TableSelectionPart> tableSelectionParts;
    bool tableDividersGuessed = false;

    int lastSourceLocationViewportPageNumber = -1;
    double lastSourceLocationViewportNormalizedX = 0.0;
    double lastSourceLocationViewportNormalizedY = 0.0;
    int singlePageWheelAccumulatedDelta = 0;

    // for everything except PgUp/PgDn and scroll to arbitrary locations
    const int baseShortScrollDuration = 100;
    int currentShortScrollDuration = baseShortScrollDuration;
    // for PgUp/PgDn and scroll to arbitrary locations
    const int baseLongScrollDuration = baseShortScrollDuration * 2;
    int currentLongScrollDuration = baseLongScrollDuration;

    // auto scroll
    int scrollIncrement = 0;
    QTimer *autoScrollTimer = nullptr;
    // annotations
    PageViewAnnotator *annotator = nullptr;
    // text annotation dialogs list
    QSet<AnnotWindow *> m_annowindows;
    QPointer<AnnotWindow> m_reviewAnnotationWindow;
    Okular::Annotation *m_reviewSelectionAnnotation = nullptr;
    int m_reviewSelectionPageNumber = -1;
    QString m_reviewSelectionUniqueName;
    int m_reviewMaskPageNumber = -1;
    QMap<int, QList<Okular::NormalizedRect>> m_reviewMaskRects;
    QMap<int, QList<Okular::NormalizedRect>> m_reviewCardHighlightRects;
    PageView::ReviewMaskStyle m_reviewMaskStyle = PageView::ReviewMaskStyle::Hidden;
    bool m_reviewMaskInverted = false;
    // Answer feedback of the card under review (drawn only while its answer is revealed).
    Thoughts::ReviewFeedbackOverlay *reviewFeedback = nullptr;
    Thoughts::DocumentFeedbackOverlay *documentFeedback = nullptr;
    // Where document feedback's pictures are kept (Part sets it with the repository).
    Thoughts::ReviewFeedbackEditor::PictureStore feedbackPictureStore;
    // A question closed without Ask Agent (or not sent), for the next Question….
    QString unsentQuestion;
    // Agents' reading guides (reading-overlays.md): the overlays of the PDF on screen; the mark the
    // Overlay tab asked for, flashed while overlayFlashTicks counts down; the comment marker pressed.
    Thoughts::ReadingOverlayLayer *readingOverlays = nullptr;
    QList<QPair<int, QRectF>> overlayFlashRects;
    int overlayFlashTicks = 0;
    QTimer *overlayFlashTimer = nullptr;
    QStringList pressedOverlayMarker;
    QPointer<QWidget> overlayMarkPopup;
    // The pictures of marks' notes (reading-overlays.md "Mark notes") by note_id, the newest few
    // (oldest first in overlayNoteImageOrder), and the notes asked for and not answered yet.
    QHash<QString, QImage> overlayNoteImages;
    QStringList overlayNoteImageOrder;
    QSet<QString> overlayNoteRequested;
    // A rectangle whose menu is open: what its feedback is about, instead of the text selection.
    QList<QPair<int, QRectF>> feedbackAreaLines;
    QString feedbackAreaText;
    QPointer<QWidget> reviewFeedbackBanner;
    // Change passage in progress (PageView::startPassageChange).
    struct PassageChange {
        bool document = false;
        QString feedbackId;
        QString cardId;
        QString documentId;
        QString polarity;
        QString kind;
        QString comment;
    };
    std::optional<PassageChange> passageChange;
    bool passageSaving = false;
    QPointer<QWidget> passageBanner;
    QJsonArray m_commentSourceConnectors;
    // other stuff
    QTimer *delayResizeEventTimer = nullptr;
    bool dirtyLayout = false;
    bool blockViewport = false;               // prevents changes to viewport
    bool blockPixmapsRequest = false;         // prevent pixmap requests
    PageViewMessage *messageWindow = nullptr; // in pageviewutils.h
    bool m_formsVisible = false;
    FormWidgetsController *formsWidgetController = nullptr;
#if HAVE_SPEECH
    OkularTTS *m_tts = nullptr;
#endif
    QTimer *refreshTimer = nullptr;
    QSet<int> refreshPages;

    // bbox state for Trim to Selection mode
    Okular::NormalizedRect trimBoundingBox;

    // infinite resizing loop prevention
    bool verticalScrollBarVisible = false;
    bool horizontalScrollBarVisible = false;

    // drag scroll
    QPoint dragScrollVector;
    QTimer dragScrollTimer;

    // left click depress
    QTimer leftClickTimer;

    // Triple-click detection for line selection
    QElapsedTimer lastDoubleClickTime;
    QPoint lastDoubleClickPos;
    bool tripleClickDetectionEnabled = false;

    // actions
    QAction *aRotateClockwise = nullptr;
    QAction *aRotateCounterClockwise = nullptr;
    QAction *aRotateOriginal = nullptr;
    KActionMenu *aTrimMode = nullptr;
    KToggleAction *aTrimMargins = nullptr;
    KToggleAction *aReadingDirection = nullptr;
    QAction *aMouseNormal = nullptr;
    QAction *aMouseZoom = nullptr;
    QAction *aMouseSelect = nullptr;
    QAction *aMouseTextSelect = nullptr;
    QAction *aMouseTableSelect = nullptr;
    QAction *aMouseDraw = nullptr;
    QAction *aMouseTextBox = nullptr;
    QAction *aMouseMagnifier = nullptr;
    KToggleAction *aTrimToSelection = nullptr;
    QAction *aSignature = nullptr;
    KSelectAction *aZoom = nullptr;
    QAction *aZoomIn = nullptr;
    QAction *aZoomOut = nullptr;
    QAction *aZoomActual = nullptr;
    KToggleAction *aZoomFitWidth = nullptr;
    KToggleAction *aZoomFitPage = nullptr;
    KToggleAction *aZoomAutoFit = nullptr;
    KActionMenu *aViewModeMenu = nullptr;
    KToggleAction *aHighlightComments = nullptr;
    KToggleAction *aHighlightFeedback = nullptr;
    KToggleAction *aHighlightAgentEdits = nullptr;
    KToggleAction *aHighlightReviewCards = nullptr;
    KToggleAction *aHighlightReadingOverlays = nullptr;
    QActionGroup *viewModeActionGroup = nullptr;
    ColorModeMenu *aColorModeMenu = nullptr;
    KToggleAction *aViewContinuous = nullptr;
    QAction *aPrevAction = nullptr;
    KToggleAction *aToggleForms = nullptr;
    QAction *aSpeakDoc = nullptr;
    QAction *aSpeakDocFromPage = nullptr;
    QAction *aSpeakPage = nullptr;
    QAction *aSpeakStop = nullptr;
    QAction *aSpeakPauseResume = nullptr;
    QAction *aReadAloud = nullptr;
    Thoughts::ReadAloud::Controller *readAloud = nullptr;
    ReadAloudBar *readAloudBar = nullptr;
    KActionCollection *actionCollection = nullptr;
    QActionGroup *mouseModeActionGroup = nullptr;
    ToggleActionMenu *aMouseModeMenu = nullptr;
    QAction *aFitWindowToPage = nullptr;

    int setting_viewCols = 0;
    bool rtl_Mode = false;
    // Keep track of whether tablet pen is currently pressed down
    bool penDown = false;

    // Keep track of mouse over link object
    const Okular::ObjectRect *mouseOverLinkObject = nullptr;

    QScroller *scroller = nullptr;

    bool pinchZoomActive = false;
    // The remaining scroll from the previous zoom event
    QPointF remainingScroll;
    SignaturePartUtils::SigningInformation signingInfo;
#if HAVE_NEW_SIGNATURE_API
    Okular::SignatureAnnotation *signatureAnnotation = nullptr;
#endif
};

PageViewPrivate::PageViewPrivate(PageView *qq)
    : q(qq)
{
}

FormWidgetsController *PageViewPrivate::formWidgetsController()
{
    if (!formsWidgetController) {
        formsWidgetController = new FormWidgetsController(document);
        QObject::connect(formsWidgetController, &FormWidgetsController::changed, q, &PageView::slotFormChanged);
        QObject::connect(formsWidgetController, &FormWidgetsController::action, q, &PageView::slotAction);
        QObject::connect(formsWidgetController, &FormWidgetsController::mouseAction, q, &PageView::slotMouseAction);
    }

    return formsWidgetController;
}

#if HAVE_SPEECH
OkularTTS *PageViewPrivate::tts()
{
    if (!m_tts) {
        m_tts = new OkularTTS(q);
        if (aSpeakStop) {
            QObject::connect(m_tts, &OkularTTS::canPauseOrResume, aSpeakStop, &QAction::setEnabled);
        }

        if (aSpeakPauseResume) {
            QObject::connect(m_tts, &OkularTTS::canPauseOrResume, aSpeakPauseResume, &QAction::setEnabled);
        }
    }

    return m_tts;
}
#endif

/* PageView. What's in this file? -> quick overview.
 * Code weight (in rows) and meaning:
 *  160 - constructor and creating actions plus their connected slots (empty stuff)
 *  70  - DocumentObserver inherited methodes (important)
 *  550 - events: mouse, keyboard, drag
 *  170 - slotRelayoutPages: set contents of the scrollview on continuous/single modes
 *  100 - zoom: zooming pages in different ways, keeping update the toolbar actions, etc..
 *  other misc functions: only slotRequestVisiblePixmaps and pickItemOnPoint noticeable,
 * and many insignificant stuff like this comment :-)
 */
PageView::PageView(QWidget *parent, Okular::Document *document)
    : QAbstractScrollArea(parent)
    , Okular::View(QStringLiteral("PageView"))
{
    // create and initialize private storage structure
    d = new PageViewPrivate(this);
    d->document = document;
    d->mouseMode = Okular::Settings::mouseMode();
    d->mouseAnnotation = new MouseAnnotation(this, document);
    d->messageWindow = new PageViewMessage(this);
    styleScrollBars();
    d->reviewFeedback = new Thoughts::ReviewFeedbackOverlay(document, this);
    connect(d->reviewFeedback, &Thoughts::ReviewFeedbackOverlay::changed, this, [this] {
        viewport()->update();
        endPassageChangeIfStale();
        Q_EMIT reviewFeedbackChanged();
    });
    d->documentFeedback = new Thoughts::DocumentFeedbackOverlay(document, this);
    d->documentFeedback->setShownKinds(Okular::Settings::highlightComments(), Okular::Settings::highlightFeedback());
    connect(d->documentFeedback, &Thoughts::DocumentFeedbackOverlay::changed, this, [this] {
        viewport()->update();
        endPassageChangeIfStale();
        Q_EMIT documentFeedbackChanged();
    });
    d->readingOverlays = new Thoughts::ReadingOverlayLayer(this);
    // A mark made on another PDF version is found in the pages' text as feedback is, from the same glyphs.
    d->readingOverlays->setPageText([this](const QList<int> &pages) { return d->documentFeedback->pagesText(pages); });
    connect(d->readingOverlays, &Thoughts::ReadingOverlayLayer::changed, this, [this] {
        viewport()->update();
        Q_EMIT readingOverlaysChanged();
    });
    d->setting_viewCols = Okular::Settings::viewColumns();
    d->rtl_Mode = Okular::Settings::rtlReadingDirection();

    d->trimBoundingBox = Okular::NormalizedRect(); // Null box

    switch (Okular::Settings::zoomMode()) {
    case 0: {
        d->zoomFactor = 1;
        d->zoomMode = PageView::ZoomFixed;
        break;
    }
    case 1: {
        d->zoomMode = PageView::ZoomFitWidth;
        break;
    }
    case 2: {
        d->zoomMode = PageView::ZoomFitPage;
        break;
    }
    case 3: {
        d->zoomMode = PageView::ZoomFitAuto;
        break;
    }
    case 4: {
        d->zoomMode = PageView::ZoomFixed;
        d->zoomFactor = Okular::Settings::customZoomFactor() / 100.0;
        break;
    }
    }

    connect(Okular::Settings::self(), &Okular::Settings::viewContinuousChanged, this, [=, this]() {
        if (d->aViewContinuous && !d->document->isOpened()) {
            d->aViewContinuous->setChecked(Okular::Settings::viewContinuous());
        }
    });

    connect(Okular::Settings::self(), &Okular::Settings::highlightVisibilityChanged, this, [this] {
        if (d->aHighlightComments) {
            const QSignalBlocker blocker(d->aHighlightComments);
            d->aHighlightComments->setChecked(Okular::Settings::highlightComments());
        }
        if (d->aHighlightFeedback) {
            const QSignalBlocker blocker(d->aHighlightFeedback);
            d->aHighlightFeedback->setChecked(Okular::Settings::highlightFeedback());
        }
        d->documentFeedback->setShownKinds(Okular::Settings::highlightComments(), Okular::Settings::highlightFeedback());
        if (d->aHighlightAgentEdits) {
            const QSignalBlocker blocker(d->aHighlightAgentEdits);
            d->aHighlightAgentEdits->setChecked(Okular::Settings::highlightAgentEdits());
        }
        if (d->aHighlightReviewCards) {
            const QSignalBlocker blocker(d->aHighlightReviewCards);
            d->aHighlightReviewCards->setChecked(Okular::Settings::highlightReviewCards());
        }
        if (d->aHighlightReadingOverlays) {
            const QSignalBlocker blocker(d->aHighlightReadingOverlays);
            d->aHighlightReadingOverlays->setChecked(Okular::Settings::highlightReadingOverlays());
        }
        if (!Okular::Settings::highlightComments()) {
            // Comments that are no longer painted must also stop being clickable.
            d->mouseCommentCandidate = nullptr;
            d->mouseCommentCandidatePage = -1;
            if (d->mouseAnnotation && d->mouseAnnotation->isActive() && Okular::AnnotationUtils::isCommentAnnotation(d->mouseAnnotation->annotation())) {
                d->mouseAnnotation->cancel();
            }
        }
        viewport()->update();
    });

    d->delayResizeEventTimer = new QTimer(this);
    d->delayResizeEventTimer->setSingleShot(true);
    d->delayResizeEventTimer->setObjectName(QStringLiteral("delayResizeEventTimer"));
    connect(d->delayResizeEventTimer, &QTimer::timeout, this, &PageView::delayedResizeEvent);

    setFrameStyle(QFrame::NoFrame);

    setAttribute(Qt::WA_StaticContents);

    setObjectName(QStringLiteral("okular::pageView"));

    // viewport setup: setup focus, and track mouse
    viewport()->setFocusProxy(this);
    viewport()->setFocusPolicy(Qt::StrongFocus);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_NoSystemBackground);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);

    d->agentProposalHoverShowTimer = new QTimer(this);
    d->agentProposalHoverShowTimer->setSingleShot(true);
    d->agentProposalHoverShowTimer->setInterval(AgentProposalHoverShowDelayMs);
    connect(d->agentProposalHoverShowTimer, &QTimer::timeout, this, &PageView::showAgentProposalHoverBar);
    d->placeHighlightTimer = new QTimer(this);
    d->placeHighlightTimer->setSingleShot(true);
    d->placeHighlightTimer->setInterval(6000);
    connect(d->placeHighlightTimer, &QTimer::timeout, this, [this] {
        if (d->placeHighlight) textSelectionClear();
    });
    // A mark asked for from the Overlay tab blinks three times.
    d->overlayFlashTimer = new QTimer(this);
    d->overlayFlashTimer->setInterval(180);
    connect(d->overlayFlashTimer, &QTimer::timeout, this, [this] {
        if (--d->overlayFlashTicks <= 0) {
            d->overlayFlashTimer->stop();
            d->overlayFlashRects.clear();
        }
        viewport()->update();
    });
    d->agentProposalHoldTimer = new QTimer(this);
    d->agentProposalHoldTimer->setSingleShot(true);
    d->agentProposalHoldTimer->setInterval(QGuiApplication::styleHints()->mousePressAndHoldInterval());
    connect(d->agentProposalHoldTimer, &QTimer::timeout, this, [this] {
        if (d->pressedAgentProposalMarker.isEmpty() || d->agentProposalPressCancelled) {
            return;
        }
        // Held: the bar instead of an accept on release.
        d->agentProposalPressHeld = true;
        showAgentProposalHoverBarFor(d->pressedAgentProposalMarker);
    });
    d->agentProposalHoverHideTimer = new QTimer(this);
    d->agentProposalHoverHideTimer->setSingleShot(true);
    d->agentProposalHoverHideTimer->setInterval(AgentProposalHoverGraceMs);
    connect(d->agentProposalHoverHideTimer, &QTimer::timeout, this, [this] {
        if (d->agentProposalHoverBar && d->agentProposalHoverBar->pointerInside()) {
            return;
        }
        if (d->agentProposalHoverPointerValid && agentProposalHoverBridgeContains(d->agentProposalHoverPointer)) {
            return;
        }
        hideAgentProposalHoverBar();
    });

    d->scroller = QScroller::scroller(viewport());

    QScrollerProperties prop;
    prop.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.3);
    prop.setScrollMetric(QScrollerProperties::MaximumVelocity, 1);
    prop.setScrollMetric(QScrollerProperties::AcceleratingFlickMaximumTime, 0.2); // Workaround for QTBUG-88249 (non-flick gestures recognized as accelerating flick)
    prop.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    prop.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    prop.setScrollMetric(QScrollerProperties::DragStartDistance, 0.0);
    d->scroller->setScrollerProperties(prop);

    connect(d->scroller, &QScroller::stateChanged, this, [this](QScroller::State s) { slotRequestVisiblePixmaps(s); });
    // The proposal window goes beside its passage once the view stopped moving there.
    connect(d->scroller, &QScroller::stateChanged, this, [this](QScroller::State s) {
        if (s == QScroller::Inactive && d->agentProposalPlacement) QTimer::singleShot(0, this, &PageView::applyAgentProposalWindowPlacement);
    });

    // the apparently "magic" value of 20 is the same used internally in QScrollArea
    verticalScrollBar()->setCursor(Qt::ArrowCursor);
    verticalScrollBar()->setSingleStep(20);
    horizontalScrollBar()->setCursor(Qt::ArrowCursor);
    horizontalScrollBar()->setSingleStep(20);

    // make the smooth scroll animation durations respect the global animation
    // scale
    KConfigWatcher::Ptr animationSpeedWatcher = KConfigWatcher::create(KSharedConfig::openConfig());
    connect(animationSpeedWatcher.data(), &KConfigWatcher::configChanged, this, [this](const KConfigGroup &group, const QByteArrayList &names) {
        if (group.name() == QLatin1String("KDE") && names.contains(QByteArrayLiteral("AnimationDurationFactor"))) {
            PageView::updateSmoothScrollAnimationSpeed();
        }
    });

    // connect the padding of the viewport to pixmaps requests
    connect(horizontalScrollBar(), &QAbstractSlider::valueChanged, this, &PageView::slotRequestVisiblePixmaps);
    connect(verticalScrollBar(), &QAbstractSlider::valueChanged, this, &PageView::slotRequestVisiblePixmaps);

    // Keep the scroller in sync with user input on the scrollbars.
    // QAbstractSlider::sliderMoved() and sliderReleased are the intuitive signals,
    // but are only emitted when the “slider is down”, i. e. not when the user scrolls on the scrollbar.
    // QAbstractSlider::actionTriggered() is emitted in all user input cases,
    // but before the value() changes, so we need queued connection here.
    auto update_scroller = [=, this]() {
        d->scroller->scrollTo(QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()), 0); // sync scroller with scrollbar
    };
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this, update_scroller, Qt::QueuedConnection);
    connect(horizontalScrollBar(), &QAbstractSlider::actionTriggered, this, update_scroller, Qt::QueuedConnection);

    connect(&d->dragScrollTimer, &QTimer::timeout, this, &PageView::slotDragScroll);

    d->leftClickTimer.setSingleShot(true);
    connect(&d->leftClickTimer, &QTimer::timeout, this, &PageView::slotShowMoveCursor);

    // set a corner button to resize the view to the page size
    //    QPushButton * resizeButton = new QPushButton( viewport() );
    //    resizeButton->setPixmap( SmallIcon("crop") );
    //    setCornerWidget( resizeButton );
    //    resizeButton->setEnabled( false );
    // connect(...);
    setAttribute(Qt::WA_InputMethodEnabled, true);

    // Grab pinch gestures to zoom and rotate the view
    grabGesture(Qt::PinchGesture);

    d->magnifierView = new MagnifierView(document, this);
    d->magnifierView->hide();
    d->magnifierView->setGeometry(0, 0, 351, 201); // TODO: more dynamic?

    connect(document, &Okular::Document::processMovieAction, this, &PageView::slotProcessMovieAction);
    connect(document, &Okular::Document::processRenditionAction, this, &PageView::slotProcessRenditionAction);

    // schedule the welcome message
    QMetaObject::invokeMethod(this, "slotShowWelcome", Qt::QueuedConnection);
}

PageView::~PageView()
{
    delete d->readAloudBar;
    delete d->readAloud;
#if HAVE_SPEECH
    if (d->m_tts) {
        d->m_tts->stopAllSpeechs();
    }
#endif

    delete d->mouseAnnotation;

    // delete the local storage structure

    // We need to assign it to a different list otherwise slotAnnotationWindowDestroyed
    // will bite us and clear d->m_annowindows
    QSet<AnnotWindow *> annowindows = d->m_annowindows;
    d->m_annowindows.clear();
    qDeleteAll(annowindows);

    // delete all widgets
    qDeleteAll(d->items);
    delete d->formsWidgetController;
    d->document->removeObserver(this);
    delete d;
}

Okular::Annotation *PageView::focusedAnnotation() const
{
    return d->mouseAnnotation ? d->mouseAnnotation->annotation() : nullptr;
}

int PageView::focusedAnnotationPageNumber() const
{
    return d->mouseAnnotation ? d->mouseAnnotation->pageNumber() : -1;
}

bool PageView::mapGlobalPosToPagePoint(QPoint globalPos, int *pageNumber, Okular::NormalizedPoint *point) const
{
    const QPoint viewportPos = viewport()->mapFromGlobal(globalPos);
    const QPoint contentPos = contentAreaPoint(viewportPos);
    PageViewItem *pageItem = const_cast<PageView *>(this)->pickItemOnPoint(contentPos.x(), contentPos.y());
    if (!pageItem) {
        return false;
    }

    if (pageNumber) {
        *pageNumber = pageItem->pageNumber();
    }
    if (point) {
        *point = Okular::NormalizedPoint(pageItem->absToPageX(contentPos.x()), pageItem->absToPageY(contentPos.y()));
    }
    return true;
}

void PageView::setupBaseActions(KActionCollection *ac)
{
    d->actionCollection = ac;

    // Zoom actions ( higher scales takes lots of memory! )
    d->aZoom = new KSelectAction(QIcon::fromTheme(QStringLiteral("page-zoom")), i18n("Zoom"), this);
    ac->addAction(QStringLiteral("zoom_to"), d->aZoom);
    d->aZoom->setEditable(true);
    d->aZoom->setMaxComboViewCount(kZoomValues.size() + 3);
    connect(d->aZoom, &KSelectAction::actionTriggered, this, &PageView::slotZoom);
    updateZoomText();

    d->aZoomIn = KStandardAction::zoomIn(this, SLOT(slotZoomIn()), ac);

    d->aZoomOut = KStandardAction::zoomOut(this, SLOT(slotZoomOut()), ac);

    d->aZoomActual = KStandardAction::actualSize(this, &PageView::slotZoomActual, ac);
    d->aZoomActual->setText(i18n("Zoom to 100%"));
}

void PageView::setupViewerActions(KActionCollection *ac)
{
    d->actionCollection = ac;

    ac->setDefaultShortcut(d->aZoomIn, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Plus));
    ac->setDefaultShortcut(d->aZoomOut, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Minus));

    // orientation menu actions
    d->aRotateClockwise = new QAction(QIcon::fromTheme(QStringLiteral("object-rotate-right")), i18n("Rotate &Right"), this);
    d->aRotateClockwise->setIconText(i18nc("Rotate right", "Right"));
    ac->addAction(QStringLiteral("view_orientation_rotate_cw"), d->aRotateClockwise);
    d->aRotateClockwise->setEnabled(false);
    connect(d->aRotateClockwise, &QAction::triggered, this, &PageView::slotRotateClockwise);
    d->aRotateCounterClockwise = new QAction(QIcon::fromTheme(QStringLiteral("object-rotate-left")), i18n("Rotate &Left"), this);
    d->aRotateCounterClockwise->setIconText(i18nc("Rotate left", "Left"));
    ac->addAction(QStringLiteral("view_orientation_rotate_ccw"), d->aRotateCounterClockwise);
    d->aRotateCounterClockwise->setEnabled(false);
    connect(d->aRotateCounterClockwise, &QAction::triggered, this, &PageView::slotRotateCounterClockwise);
    d->aRotateOriginal = new QAction(i18n("Original Orientation"), this);
    ac->addAction(QStringLiteral("view_orientation_original"), d->aRotateOriginal);
    d->aRotateOriginal->setEnabled(false);
    connect(d->aRotateOriginal, &QAction::triggered, this, &PageView::slotRotateOriginal);

    // Trim View actions
    d->aTrimMode = new KActionMenu(i18n("&Trim View"), this);
    d->aTrimMode->setPopupMode(QToolButton::InstantPopup);
    ac->addAction(QStringLiteral("view_trim_mode"), d->aTrimMode);

    d->aTrimMargins = new KToggleAction(QIcon::fromTheme(QStringLiteral("trim-margins")), i18n("&Trim Margins"), d->aTrimMode->menu());
    d->aTrimMode->addAction(d->aTrimMargins);
    ac->addAction(QStringLiteral("view_trim_margins"), d->aTrimMargins);
    d->aTrimMargins->setData(QVariant::fromValue((int)Okular::Settings::EnumTrimMode::Margins));
    connect(d->aTrimMargins, &QAction::toggled, this, &PageView::slotTrimMarginsToggled);
    d->aTrimMargins->setChecked(Okular::Settings::trimMargins());

    d->aTrimToSelection = new KToggleAction(QIcon::fromTheme(QStringLiteral("trim-to-selection")), i18n("Trim To &Selection"), d->aTrimMode->menu());
    d->aTrimMode->addAction(d->aTrimToSelection);
    ac->addAction(QStringLiteral("view_trim_selection"), d->aTrimToSelection);
    d->aTrimToSelection->setData(QVariant::fromValue((int)Okular::Settings::EnumTrimMode::Selection));
    connect(d->aTrimToSelection, &QAction::toggled, this, &PageView::slotTrimToSelectionToggled);

    d->aZoomFitWidth = new KToggleAction(QIcon::fromTheme(QStringLiteral("zoom-fit-width")), i18n("Fit &Width"), this);
    ac->addAction(QStringLiteral("view_fit_to_width"), d->aZoomFitWidth);
    connect(d->aZoomFitWidth, &QAction::toggled, this, &PageView::slotFitToWidthToggled);

    d->aZoomFitPage = new KToggleAction(QIcon::fromTheme(QStringLiteral("zoom-fit-best")), i18n("Fit &Page"), this);
    ac->addAction(QStringLiteral("view_fit_to_page"), d->aZoomFitPage);
    connect(d->aZoomFitPage, &QAction::toggled, this, &PageView::slotFitToPageToggled);

    d->aZoomAutoFit = new KToggleAction(QIcon::fromTheme(QStringLiteral("zoom-fit-best")), i18n("&Auto Fit"), this);
    ac->addAction(QStringLiteral("view_auto_fit"), d->aZoomAutoFit);
    connect(d->aZoomAutoFit, &QAction::toggled, this, &PageView::slotAutoFitToggled);

    d->aFitWindowToPage = new QAction(QIcon::fromTheme(QStringLiteral("zoom-fit-width")), i18n("Fit Wi&ndow to Page"), this);
    d->aFitWindowToPage->setEnabled(Okular::Settings::viewMode() == (int)Okular::Settings::EnumViewMode::Single);
    ac->setDefaultShortcut(d->aFitWindowToPage, QKeySequence(Qt::CTRL | Qt::Key_J));
    ac->addAction(QStringLiteral("fit_window_to_page"), d->aFitWindowToPage);
    connect(d->aFitWindowToPage, &QAction::triggered, this, &PageView::slotFitWindowToPage);

    // View Mode action menu (Single Page, Facing Pages,...(choose), and Continuous (on/off))
    d->aViewModeMenu = new KActionMenu(QIcon::fromTheme(QStringLiteral("view-split-left-right")), i18n("&View Mode"), this);
    d->aViewModeMenu->setPopupMode(QToolButton::InstantPopup);
    ac->addAction(QStringLiteral("view_render_mode"), d->aViewModeMenu);

    d->viewModeActionGroup = new QActionGroup(this);
    auto addViewMode = [=, this](QAction *a, const QString &name, Okular::Settings::EnumViewMode::type id) {
        a->setCheckable(true);
        a->setData(int(id));
        d->aViewModeMenu->addAction(a);
        ac->addAction(name, a);
        d->viewModeActionGroup->addAction(a);
    };
    addViewMode(new QAction(QIcon::fromTheme(QStringLiteral("view-pages-single")), i18nc("@item:inmenu", "&Single Page"), this), QStringLiteral("view_render_mode_single"), Okular::Settings::EnumViewMode::Single);
    addViewMode(new QAction(QIcon::fromTheme(QStringLiteral("view-pages-facing")), i18nc("@item:inmenu", "&Facing Pages"), this), QStringLiteral("view_render_mode_facing"), Okular::Settings::EnumViewMode::Facing);
    addViewMode(new QAction(QIcon::fromTheme(QStringLiteral("view-pages-facing-first-centered")), i18nc("@item:inmenu", "Facing Pages (&Center First Page)"), this),
                QStringLiteral("view_render_mode_facing_center_first"),
                Okular::Settings::EnumViewMode::FacingFirstCentered);
    addViewMode(new QAction(QIcon::fromTheme(QStringLiteral("view-pages-overview")), i18nc("@item:inmenu", "&Overview"), this), QStringLiteral("view_render_mode_overview"), Okular::Settings::EnumViewMode::Summary);
    const QList<QAction *> viewModeActions = d->viewModeActionGroup->actions();
    for (QAction *viewModeAction : viewModeActions) {
        if (viewModeAction->data().toInt() == Okular::Settings::viewMode()) {
            viewModeAction->setChecked(true);
            break;
        }
    }
    connect(d->viewModeActionGroup, &QActionGroup::triggered, this, &PageView::slotViewMode);

    auto *highlightMenu = new KActionMenu(QIcon::fromTheme(QStringLiteral("draw-highlight")), i18n("Visible markup"), this);
    highlightMenu->setPopupMode(QToolButton::InstantPopup);
    highlightMenu->setToolTip(i18n("Choose which markup is visible in the PDF"));
    ac->addAction(QStringLiteral("view_highlights"), highlightMenu);
    d->aHighlightComments = new KToggleAction(i18n("Comments"), this);
    d->aHighlightComments->setChecked(Okular::Settings::highlightComments());
    d->aHighlightComments->setToolTip(i18n("Highlight commented passages and their comment markers (comments stay on every version)"));
    ac->addAction(QStringLiteral("highlight_comments"), d->aHighlightComments);
    highlightMenu->addAction(d->aHighlightComments);
    connect(d->aHighlightComments, &QAction::toggled, this, [](bool checked) {
        Okular::Settings::setHighlightComments(checked);
        Okular::Settings::self()->save();
    });
    // Feedback belongs to the PDF version it was given on: likes, dislikes and questions.
    d->aHighlightFeedback = new KToggleAction(i18n("Feedback"), this);
    d->aHighlightFeedback->setChecked(Okular::Settings::highlightFeedback());
    d->aHighlightFeedback->setToolTip(i18n("Highlight liked, disliked and questioned passages (feedback belongs to its version)"));
    ac->addAction(QStringLiteral("highlight_feedback"), d->aHighlightFeedback);
    highlightMenu->addAction(d->aHighlightFeedback);
    connect(d->aHighlightFeedback, &QAction::toggled, this, [](bool checked) {
        Okular::Settings::setHighlightFeedback(checked);
        Okular::Settings::self()->save();
    });
    d->aHighlightAgentEdits = new KToggleAction(i18n("Agent proposals"), this);
    d->aHighlightAgentEdits->setChecked(Okular::Settings::highlightAgentEdits());
    d->aHighlightAgentEdits->setToolTip(i18n("Show markup for all available agent proposals in the PDF"));
    ac->addAction(QStringLiteral("highlight_agent_edits"), d->aHighlightAgentEdits);
    highlightMenu->addAction(d->aHighlightAgentEdits);
    connect(d->aHighlightAgentEdits, &QAction::toggled, this, [this](bool checked) {
        Okular::Settings::setHighlightAgentEdits(checked);
        Okular::Settings::self()->save();
        hideAgentProposalHoverBar();
    });

    d->aHighlightReviewCards = new KToggleAction(i18n("Review cards"), this);
    d->aHighlightReviewCards->setChecked(Okular::Settings::highlightReviewCards());
    d->aHighlightReviewCards->setToolTip(i18n("Highlight all available review card answers in the PDF"));
    ac->addAction(QStringLiteral("highlight_review_cards"), d->aHighlightReviewCards);
    highlightMenu->addAction(d->aHighlightReviewCards);
    connect(d->aHighlightReviewCards, &QAction::toggled, this, [](bool checked) {
        Okular::Settings::setHighlightReviewCards(checked);
        Okular::Settings::self()->save();
    });
    // Agents' reading guides (reading-overlays.md §4); off hides every overlay.
    d->aHighlightReadingOverlays = new KToggleAction(i18n("Reading overlays"), this);
    d->aHighlightReadingOverlays->setChecked(Okular::Settings::highlightReadingOverlays());
    d->aHighlightReadingOverlays->setToolTip(i18n("Show agents' reading overlays: what to read, skim or skip, and their comments"));
    ac->addAction(QStringLiteral("highlight_reading_overlays"), d->aHighlightReadingOverlays);
    highlightMenu->addAction(d->aHighlightReadingOverlays);
    connect(d->aHighlightReadingOverlays, &QAction::toggled, this, [this](bool checked) {
        Okular::Settings::setHighlightReadingOverlays(checked);
        Okular::Settings::self()->save();
        viewport()->update();
    });

    // Continuous view action, add to view mode action menu.
    d->aViewModeMenu->addSeparator();
    d->aViewContinuous = new KToggleAction(QIcon::fromTheme(QStringLiteral("view-pages-continuous")), i18n("&Continuous"), this);
    d->aViewModeMenu->addAction(d->aViewContinuous);
    ac->addAction(QStringLiteral("view_continuous"), d->aViewContinuous);
    connect(d->aViewContinuous, &QAction::toggled, this, &PageView::slotContinuousToggled);
    d->aViewContinuous->setChecked(Okular::Settings::viewContinuous());

    // Reading direction toggle action. (Checked means RTL, unchecked means LTR.)
    d->aReadingDirection = new KToggleAction(QIcon::fromTheme(QStringLiteral("format-text-direction-rtl")), i18nc("@action page layout", "Use Right to Left Reading Direction"), this);
    d->aReadingDirection->setChecked(Okular::Settings::rtlReadingDirection());
    ac->addAction(QStringLiteral("rtl_page_layout"), d->aReadingDirection);
    connect(d->aReadingDirection, &QAction::toggled, this, &PageView::slotReadingDirectionToggled);
    connect(Okular::SettingsCore::self(), &Okular::SettingsCore::configChanged, this, &PageView::slotUpdateReadingDirectionAction);

    // Mouse mode actions for viewer mode
    d->mouseModeActionGroup = new QActionGroup(this);
    d->mouseModeActionGroup->setExclusive(true);
    d->aMouseNormal = new QAction(QIcon::fromTheme(QStringLiteral("transform-browse")), i18n("&Browse"), this);
    ac->addAction(QStringLiteral("mouse_drag"), d->aMouseNormal);
    connect(d->aMouseNormal, &QAction::triggered, this, &PageView::slotSetMouseNormal);
    d->aMouseNormal->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseNormal, QKeySequence(Qt::CTRL | Qt::Key_1));
    d->aMouseNormal->setActionGroup(d->mouseModeActionGroup);
    d->aMouseNormal->setChecked(Okular::Settings::mouseMode() == Okular::Settings::EnumMouseMode::Browse);

    d->aMouseZoom = new QAction(QIcon::fromTheme(QStringLiteral("page-zoom")), i18n("&Zoom"), this);
    ac->addAction(QStringLiteral("mouse_zoom"), d->aMouseZoom);
    connect(d->aMouseZoom, &QAction::triggered, this, &PageView::slotSetMouseZoom);
    d->aMouseZoom->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseZoom, QKeySequence(Qt::CTRL | Qt::Key_2));
    d->aMouseZoom->setActionGroup(d->mouseModeActionGroup);
    d->aMouseZoom->setChecked(Okular::Settings::mouseMode() == Okular::Settings::EnumMouseMode::Zoom);

    d->aColorModeMenu = new ColorModeMenu(ac, this);
}

// WARNING: 'setupViewerActions' must have been called before this method
void PageView::setupActions(KActionCollection *ac)
{
    d->actionCollection = ac;

    ac->setDefaultShortcuts(d->aZoomIn, KStandardShortcut::zoomIn());
    ac->setDefaultShortcuts(d->aZoomOut, KStandardShortcut::zoomOut());

    // Mouse-Mode actions
    d->aMouseSelect = new QAction(QIcon::fromTheme(QStringLiteral("select-rectangular")), i18n("&Rectangular Selection"), this);
    ac->addAction(QStringLiteral("mouse_select"), d->aMouseSelect);
    connect(d->aMouseSelect, &QAction::triggered, this, &PageView::slotSetMouseSelect);
    d->aMouseSelect->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseSelect, Qt::CTRL | Qt::Key_3);
    d->aMouseSelect->setActionGroup(d->mouseModeActionGroup);
    d->aMouseSelect->setToolTip(i18n("Select an area"));

    d->aMouseTextSelect = new QAction(QIcon::fromTheme(QStringLiteral("edit-select-text")), i18n("&Text Selection"), this);
    ac->addAction(QStringLiteral("mouse_textselect"), d->aMouseTextSelect);
    connect(d->aMouseTextSelect, &QAction::triggered, this, &PageView::slotSetMouseTextSelect);
    d->aMouseTextSelect->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseTextSelect, Qt::CTRL | Qt::Key_4);
    d->aMouseTextSelect->setActionGroup(d->mouseModeActionGroup);
    d->aMouseTextSelect->setToolTip(i18n("Select and highlight text"));

    d->aMouseDraw = new QAction(QIcon::fromTheme(QStringLiteral("draw-freehand")), i18n("&Draw"), this);
    ac->addAction(QStringLiteral("mouse_draw"), d->aMouseDraw);
    connect(d->aMouseDraw, &QAction::triggered, this, &PageView::slotSetMouseDraw);
    d->aMouseDraw->setCheckable(true);
    d->aMouseDraw->setActionGroup(d->mouseModeActionGroup);
    d->aMouseDraw->setToolTip(i18n("Pencil"));
    connect(d->aMouseDraw, &QAction::toggled, this, [this](bool on) {
        if (!on) endDrawing();
    });

    // The pencil's colours and its eraser: the menu of its button, which the shell shows on a
    // right-click and finds in the property "thoughtsMenu" of mouse_draw. One choice for every tab,
    // remembered. It is not an action of the collection, so Configure Toolbars does not offer it as
    // a button of its own.
    d->pencilMenu = new QMenu(this);
    d->pencilMenu->setObjectName(QStringLiteral("pencilMenu"));
    d->aMouseDraw->setProperty("thoughtsMenu", QVariant::fromValue<QObject *>(d->pencilMenu));
    auto *pencilChoices = new QActionGroup(this);
    // Several may be unchecked at once: slotFollowPencil checks the one in use.
    pencilChoices->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    // The pencil's own blue first: no colour is kept for it, so it stays lighter in dark mode.
    const struct {
        const char *key;
        QString name;
        QString colour;
    } colours[] = {
        {"blue", i18nc("@action pencil colour", "Blue"), QString()},
        {"black", i18nc("@action pencil colour", "Black"), QStringLiteral("#212121")},
        {"red", i18nc("@action pencil colour", "Red"), QStringLiteral("#d32f2f")},
        {"green", i18nc("@action pencil colour", "Green"), QStringLiteral("#2e7d32")},
        {"orange", i18nc("@action pencil colour", "Orange"), QStringLiteral("#ef6c00")},
    };
    for (const auto &choice : colours) {
        auto *action = new QAction(choice.name, pencilChoices);
        action->setObjectName(QStringLiteral("pencilColour_") + QLatin1StringView(choice.key));
        action->setCheckable(true);
        action->setData(choice.colour);
        connect(action, &QAction::triggered, this, [this, colour = choice.colour] { choosePencil(colour, false); });
        d->pencilMenu->addAction(action);
        d->pencilColours.append(action);
    }
    d->pencilMenu->addSeparator();
    d->aPencilEraser = new QAction(QIcon::fromTheme(QStringLiteral("draw-eraser")), i18nc("@action the pencil's eraser", "Eraser"), pencilChoices);
    d->aPencilEraser->setObjectName(QStringLiteral("pencilEraser"));
    d->aPencilEraser->setCheckable(true);
    d->aPencilEraser->setToolTip(i18n("Remove whole strokes"));
    connect(d->aPencilEraser, &QAction::triggered, this, [this] { choosePencil(QString(), true); });
    d->pencilMenu->addAction(d->aPencilEraser);
    // A swatch of each colour, the blue as the pencil draws it now (light or dark).
    connect(d->pencilMenu, &QMenu::aboutToShow, this, [this] {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        for (QAction *action : std::as_const(d->pencilColours)) {
            const QString colour = action->data().toString();
            QPixmap swatch(32, 32);
            swatch.setDevicePixelRatio(2.0);
            swatch.fill(Qt::transparent);
            QPainter painter(&swatch);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colour.isEmpty() ? Thoughts::ReviewFeedbackOverlay::colour(Thoughts::ReviewCardFeedback::neutral(), dark) : QColor::fromString(colour));
            painter.drawEllipse(QRectF(2.0, 2.0, 12.0, 12.0));
            painter.end();
            action->setIcon(QIcon(swatch));
        }
    });
    slotFollowPencil();
    connect(Okular::Settings::self(), &KCoreConfigSkeleton::configChanged, this, &PageView::slotFollowPencil);

    d->aMouseTextBox = new QAction(QIcon::fromTheme(QStringLiteral("draw-text")), i18n("Te&xt"), this);
    d->aMouseTextBox->setToolTip(i18n("Text box"));
    ac->addAction(QStringLiteral("mouse_text_box"), d->aMouseTextBox);
    connect(d->aMouseTextBox, &QAction::triggered, this, &PageView::slotSetMouseTextBox);
    d->aMouseTextBox->setCheckable(true);
    d->aMouseTextBox->setActionGroup(d->mouseModeActionGroup);
    connect(d->aMouseTextBox, &QAction::toggled, this, [this](bool on) {
        if (!on) endTextBoxes();
    });

    d->aMouseTableSelect = new QAction(QIcon::fromTheme(QStringLiteral("table")), i18n("T&able Selection"), this);
    ac->addAction(QStringLiteral("mouse_tableselect"), d->aMouseTableSelect);
    connect(d->aMouseTableSelect, &QAction::triggered, this, &PageView::slotSetMouseTableSelect);
    d->aMouseTableSelect->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseTableSelect, Qt::CTRL | Qt::Key_5);
    d->aMouseTableSelect->setActionGroup(d->mouseModeActionGroup);

    d->aMouseMagnifier = new QAction(QIcon::fromTheme(QStringLiteral("document-preview")), i18n("&Magnifier"), this);
    ac->addAction(QStringLiteral("mouse_magnifier"), d->aMouseMagnifier);
    connect(d->aMouseMagnifier, &QAction::triggered, this, &PageView::slotSetMouseMagnifier);
    d->aMouseMagnifier->setCheckable(true);
    ac->setDefaultShortcut(d->aMouseMagnifier, Qt::CTRL | Qt::Key_6);
    d->aMouseMagnifier->setActionGroup(d->mouseModeActionGroup);
    d->aMouseMagnifier->setChecked(Okular::Settings::mouseMode() == Okular::Settings::EnumMouseMode::Magnifier);

    // Mouse mode selection tools menu
    d->aMouseModeMenu = new ToggleActionMenu(i18nc("@action", "Selection Tools"), this);
    d->aMouseModeMenu->setPopupMode(QToolButton::MenuButtonPopup);
    d->aMouseModeMenu->addAction(d->aMouseSelect);
    d->aMouseModeMenu->addAction(d->aMouseTextSelect);
    d->aMouseModeMenu->addAction(d->aMouseTableSelect);
    connect(d->aMouseModeMenu->menu(), &QMenu::triggered, d->aMouseModeMenu, &ToggleActionMenu::setDefaultAction);
    ac->addAction(QStringLiteral("mouse_selecttools"), d->aMouseModeMenu);
    // The toolbar shows Text and Rectangular Selection as two buttons; keep this chooser for
    // compatibility, but never expose it through saved toolbar customizations.
    d->aMouseModeMenu->setVisible(false);
    connect(Okular::Settings::self(), &KCoreConfigSkeleton::configChanged, this, &PageView::slotFollowSelectionTool);

    switch (Okular::Settings::mouseMode()) {
    case Okular::Settings::EnumMouseMode::TextSelect:
        d->aMouseTextSelect->setChecked(true);
        d->aMouseModeMenu->setDefaultAction(d->aMouseTextSelect);
        break;
    case Okular::Settings::EnumMouseMode::RectSelect:
        d->aMouseSelect->setChecked(true);
        d->aMouseModeMenu->setDefaultAction(d->aMouseSelect);
        break;
    case Okular::Settings::EnumMouseMode::TableSelect:
        d->aMouseTableSelect->setChecked(true);
        d->aMouseModeMenu->setDefaultAction(d->aMouseTableSelect);
        break;
    default:
        d->aMouseModeMenu->setDefaultAction(d->aMouseTextSelect);
    }

    // Create signature action
    d->aSignature = new QAction(QIcon::fromTheme(QStringLiteral("document-edit-sign")), i18n("Digitally &Sign…"), this);
    ac->addAction(QStringLiteral("add_digital_signature"), d->aSignature);
    d->aSignature->setEnabled(false);
    d->aSignature->setVisible(false);

    // speak actions
#if HAVE_SPEECH
    d->aSpeakDoc = new QAction(QIcon::fromTheme(QStringLiteral("text-speak")), i18n("Speak Whole Document"), this);
    ac->addAction(QStringLiteral("speak_document"), d->aSpeakDoc);
    d->aSpeakDoc->setEnabled(false);
    connect(d->aSpeakDoc, &QAction::triggered, this, &PageView::slotSpeakDocument);

    d->aSpeakDocFromPage = new QAction(QIcon::fromTheme(QStringLiteral("text-speak")), i18n("Speak From Current Page"), this);
    ac->addAction(QStringLiteral("speak_from_current_page"), d->aSpeakDocFromPage);
    d->aSpeakDocFromPage->setEnabled(false);
    connect(d->aSpeakDocFromPage, &QAction::triggered, this, &PageView::slotSpeakFromCurrentPage);

    d->aSpeakPage = new QAction(QIcon::fromTheme(QStringLiteral("text-speak")), i18n("Speak Current Page"), this);
    ac->addAction(QStringLiteral("speak_current_page"), d->aSpeakPage);
    d->aSpeakPage->setEnabled(false);
    connect(d->aSpeakPage, &QAction::triggered, this, &PageView::slotSpeakCurrentPage);

    d->aSpeakStop = new QAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")), i18n("Stop Speaking"), this);
    ac->addAction(QStringLiteral("speak_stop_all"), d->aSpeakStop);
    d->aSpeakStop->setEnabled(false);
    connect(d->aSpeakStop, &QAction::triggered, this, &PageView::slotStopSpeaks);

    d->aSpeakPauseResume = new QAction(QIcon::fromTheme(QStringLiteral("media-playback-pause")), i18n("Pause/Resume Speaking"), this);
    ac->addAction(QStringLiteral("speak_pause_resume"), d->aSpeakPauseResume);
    d->aSpeakPauseResume->setEnabled(false);
    connect(d->aSpeakPauseResume, &QAction::triggered, this, &PageView::slotPauseResumeSpeech);
#else
    d->aSpeakDoc = nullptr;
    d->aSpeakPage = nullptr;
    d->aSpeakStop = nullptr;
    d->aSpeakPauseResume = nullptr;
#endif

    // Read Aloud: the selection in the cloud voice, the system voice as fallback.
    d->aReadAloud = new QAction(QIcon::fromTheme(QStringLiteral("text-speak")), i18n("Read Aloud"), this);
    ac->addAction(QStringLiteral("read_aloud_selection"), d->aReadAloud);
    ac->setDefaultShortcut(d->aReadAloud, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
    d->aReadAloud->setEnabled(false);
    connect(d->aReadAloud, &QAction::triggered, this, &PageView::readAloudSelection);

    // Other actions
    QAction *su = new QAction(i18n("Scroll Up"), this);
    ac->addAction(QStringLiteral("view_scroll_up"), su);
    connect(su, &QAction::triggered, this, &PageView::slotAutoScrollUp);
    ac->setDefaultShortcut(su, QKeySequence(Qt::SHIFT | Qt::Key_Up));
    addAction(su);

    QAction *sd = new QAction(i18n("Scroll Down"), this);
    ac->addAction(QStringLiteral("view_scroll_down"), sd);
    connect(sd, &QAction::triggered, this, &PageView::slotAutoScrollDown);
    ac->setDefaultShortcut(sd, QKeySequence(Qt::SHIFT | Qt::Key_Down));
    addAction(sd);

    QAction *spu = new QAction(i18n("Scroll Page Up"), this);
    ac->addAction(QStringLiteral("view_scroll_page_up"), spu);
    connect(spu, &QAction::triggered, this, &PageView::slotScrollUp);
    ac->setDefaultShortcut(spu, QKeySequence(Qt::SHIFT | Qt::Key_Space));
    addAction(spu);

    QAction *spd = new QAction(i18n("Scroll Page Down"), this);
    ac->addAction(QStringLiteral("view_scroll_page_down"), spd);
    connect(spd, &QAction::triggered, this, &PageView::slotScrollDown);
    ac->setDefaultShortcut(spd, QKeySequence(Qt::Key_Space));
    addAction(spd);

    d->aToggleForms = new KToggleAction(i18n("Show Forms"), this);
    ac->addAction(QStringLiteral("view_toggle_forms"), d->aToggleForms);
    d->aToggleForms->setEnabled(false);
    d->aToggleForms->setVisible(false);
    toggleFormWidgets(false);

    // Setup undo and redo actions
    QAction *kundo = KStandardAction::create(KStandardAction::Undo, d->document, SLOT(undo()), ac);
    QAction *kredo = KStandardAction::create(KStandardAction::Redo, d->document, SLOT(redo()), ac);
    connect(d->document, &Okular::Document::canUndoChanged, kundo, &QAction::setEnabled);
    connect(d->document, &Okular::Document::canRedoChanged, kredo, &QAction::setEnabled);
    kundo->setEnabled(false);
    kredo->setEnabled(false);

    d->annotator = new PageViewAnnotator(this, d->document);
    connect(d->annotator, &PageViewAnnotator::toolActive, this, [&](bool selected) {
        if (selected) {
            QAction *aMouseMode = d->mouseModeActionGroup->checkedAction();
            if (aMouseMode) {
                aMouseMode->setChecked(false);
            }
            selectionClear();
            textSelectionClear();
        } else {
            switch (d->mouseMode) {
            case Okular::Settings::EnumMouseMode::Browse:
                d->aMouseNormal->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::Zoom:
                d->aMouseZoom->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::RectSelect:
                d->aMouseSelect->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::TableSelect:
                d->aMouseTableSelect->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::Magnifier:
                d->aMouseMagnifier->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::TextSelect:
                d->aMouseTextSelect->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::Draw:
                d->aMouseDraw->setChecked(true);
                break;
            case Okular::Settings::EnumMouseMode::TextBox:
                d->aMouseTextBox->setChecked(true);
                break;
            }
        }
    });
    connect(d->annotator, &PageViewAnnotator::toolActive, d->mouseAnnotation, &MouseAnnotation::reset);
    connect(d->annotator, &PageViewAnnotator::requestOpenNewlySignedFile, this, &PageView::requestOpenNewlySignedFile);
    d->annotator->setupActions(ac);
    for (QAction *action : ac->actions()) {
        const QString actionId = action->objectName();
        if (actionId.startsWith(QLatin1String("annotation_")) || actionId == QLatin1String("mouse_toggle_annotate")) {
            action->setEnabled(false);
            action->setVisible(false);
        }
    }
}

bool PageView::canFitPageWidth() const
{
    return Okular::Settings::viewMode() != Okular::Settings::EnumViewMode::Single || d->zoomMode != ZoomFitWidth;
}

void PageView::fitPageWidth(int page)
{
    // zoom: Fit Width, columns: 1. setActions + relayout + setPage + update
    d->zoomMode = ZoomFitWidth;
    Okular::Settings::setViewMode(Okular::Settings::EnumViewMode::Single);
    d->aZoomFitWidth->setChecked(true);
    d->aZoomFitPage->setChecked(false);
    d->aZoomAutoFit->setChecked(false);
    updateViewMode(Okular::Settings::EnumViewMode::Single);
    viewport()->setUpdatesEnabled(false);
    slotRelayoutPages();
    viewport()->setUpdatesEnabled(true);
    d->document->setViewportPage(page);
    updateZoomText();
    setFocus();
}

void PageView::openAnnotationWindow(Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation) {
        return;
    }
    if (Okular::AnnotationUtils::isCommentAnnotation(annotation) && d->aHighlightComments) {
        d->aHighlightComments->setChecked(true);
    }
    const Thoughts::CommentFragment root = Thoughts::commentFragmentGroup(d->document, annotation, pageNumber).root();
    if (root.annotation) {
        annotation = root.annotation;
        pageNumber = root.page;
    }

    Q_EMIT annotationWindowOpening();

    const QPointer<QWidget> previousFocusWidget = QApplication::focusWidget();

    // find the annot window
    AnnotWindow *existWindow = nullptr;
    for (AnnotWindow *aw : std::as_const(d->m_annowindows)) {
        if (aw->annotation() == annotation) {
            existWindow = aw;
            break;
        }
    }

    if (existWindow == nullptr) {
        const QUrl documentUrl = d->document->currentDocument();
        const bool useCommentThreadUi = usesManagedCommentThreads(d->document, d->docsRoot);

        existWindow = new AnnotWindow(this, annotation, d->document, pageNumber, useCommentThreadUi);
        connect(existWindow, &AnnotWindow::threadActionSaveRequested, this, &PageView::commentThreadActionSaveRequested);
        connect(existWindow, &QObject::destroyed, this, &PageView::slotAnnotationWindowDestroyed);
        connect(existWindow, &AnnotWindow::aboutToClose, this, &PageView::commentEditorClosing);
        connect(existWindow, &AnnotWindow::replyRequested, this, [this, existWindow](const QString &contents) {
            if (!existWindow->annotation()) {
                return;
            }
            if (contents.isEmpty()) {
                promptReplyToAnnotation(existWindow->annotation(), existWindow->pageNumber());
                return;
            }
            Q_EMIT commentReplyRequested(normalizedCommentId(existWindow->annotation()->uniqueName()), existWindow->pageNumber() + 1, contents);
        });
        connect(existWindow, &AnnotWindow::relatedMetadataSaveRequested, this, [this, existWindow](const QVariantMap &metadataPatch) {
            if (!existWindow->annotation()) {
                return;
            }
            Q_EMIT commentRelatedMetadataSaveRequested(normalizedCommentId(existWindow->annotation()->uniqueName()),
                                                       existWindow->pageNumber() + 1,
                                                       existWindow->annotation()->contents(),
                                                       metadataPatch);
        });
        connect(existWindow, &AnnotWindow::relatedChatGptUrlSaveRequested, this, [this, existWindow](const QString &url, bool openAfterSave) {
            if (existWindow->annotation()) {
                Q_EMIT commentRelatedChatGptUrlSaveRequested(normalizedCommentId(existWindow->annotation()->uniqueName()), url, openAfterSave);
            }
        });
        connect(existWindow, &AnnotWindow::relatedOpenCodexRequested, this, [this, existWindow] {
            const QUrl documentUrl = d->document->currentDocument();
            if (existWindow->annotation() && documentUrl.isLocalFile()) {
                Q_EMIT commentRelatedOpenCodexRequested(QFileInfo(documentUrl.toLocalFile()).absoluteFilePath(), normalizedCommentId(existWindow->annotation()->uniqueName()));
            }
        });
        connect(existWindow, &AnnotWindow::relatedOpenChatGptRequested, this, &PageView::commentRelatedOpenChatGptRequested);
        connect(existWindow, &AnnotWindow::relatedOpenSourceRequested, this, &PageView::commentRelatedOpenSourceRequested);
        connect(existWindow, &AnnotWindow::relatedOpenOverviewRequested, this, [this, existWindow] {
            const QUrl documentUrl = d->document->currentDocument();
            if (existWindow->annotation() && documentUrl.isLocalFile()) {
                Q_EMIT commentRelatedOpenOverviewRequested(QFileInfo(documentUrl.toLocalFile()).absoluteFilePath(),
                                                           normalizedCommentId(existWindow->annotation()->uniqueName()),
                                                           existWindow->pageNumber() + 1);
            }
        });
        existWindow->setRelatedInfo(annotationRelatedInfo(annotation->uniqueName()));

        d->m_annowindows << existWindow;
    }

    existWindow->show();
    existWindow->raise();
    if (previousFocusWidget) {
        previousFocusWidget->setFocus(Qt::OtherFocusReason);
    }
}

void PageView::promptReplyToAnnotation(Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation || annotation->uniqueName().isEmpty() || annotation->contents().trimmed().isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString contents = QInputDialog::getMultiLineText(this, i18n("Reply to Comment"), i18n("Reply:"), QString(), &accepted).trimmed();
    if (!accepted || contents.isEmpty()) {
        return;
    }
    Q_EMIT commentReplyRequested(normalizedCommentId(annotation->uniqueName()), pageNumber + 1, contents);
}

void PageView::completeAnnotationReplyOperation(const QString &parentAnnotationId, const QVariantMap &result)
{
    const bool success = result.value(QStringLiteral("success")).toBool();
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (window && window->annotation() && normalizedCommentId(window->annotation()->uniqueName()) == parentAnnotationId) {
            window->completeReplyOperation(success);
        }
    }
    if (!success) {
        const QString error = result.value(QStringLiteral("error")).toString();
        KMessageBox::error(this, error.isEmpty() ? i18n("The reply could not be added.") : error, i18n("Reply to Comment"));
        return;
    }

    Okular::Annotation *parent = nullptr;
    int parentPage = -1;
    for (uint pageNumber = 0; pageNumber < d->document->pages() && !parent; ++pageNumber) {
        const Okular::Page *page = d->document->page(pageNumber);
        for (Okular::Annotation *candidate : page->annotations()) {
            if (normalizedCommentId(candidate->uniqueName()) == parentAnnotationId) {
                parent = candidate;
                parentPage = static_cast<int>(pageNumber);
                break;
            }
        }
    }
    if (parent) {
        openAnnotationWindow(parent, parentPage);
    }
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (window && window->annotation()) {
            window->updateAnnotation(window->annotation());
        }
    }
}

QVariantMap PageView::annotationRelatedInfo(const QString &annotationId) const
{
    const QString stableId = normalizedCommentId(annotationId);
    const QUrl documentUrl = d->document->currentDocument();
    if (stableId.isEmpty() || d->docsRoot.isEmpty() || !documentUrl.isLocalFile()) {
        return {};
    }

    const QFileInfo pdfInfo(documentUrl.toLocalFile());
    QJsonObject record;
    if (d->commentRecordProvider) {
        record = d->commentRecordProvider(stableId);
    } else {
        const auto records = Okular::DocsCommentSynchronizer::visibleSidecarComments(d->docsRoot, pdfInfo.completeBaseName(), nullptr, nullptr);
        for (const QJsonObject &candidate : records) {
            if (sidecarCommentId(candidate) == stableId) {
                record = candidate;
                break;
            }
        }
    }
    // The annotation itself is enough to show its type and empty optional
    // fields while its metadata is unavailable or still being synchronized.
    bool liveComment = !record.isEmpty() && !record.value(QStringLiteral("deleted")).toBool();
    for (uint page = 0; page < d->document->pages() && !liveComment; ++page) {
        for (const Okular::Annotation *annotation : d->document->page(page)->annotations()) {
            if (normalizedCommentId(annotation->uniqueName()) == stableId &&
                (annotation->subType() == Okular::Annotation::AText || annotation->subType() == Okular::Annotation::AHighlight)) {
                liveComment = true;
                break;
            }
        }
    }
    if (liveComment) {
        QString rootId = record.value(QStringLiteral("thread_root_id")).toString(stableId);
        QJsonObject rootRecord = rootId == stableId ? record : QJsonObject();
        if (rootId != stableId && d->commentRecordProvider) rootRecord = d->commentRecordProvider(rootId);
        const QJsonValue rawLocation = record.value(QStringLiteral("location"));
        QString sourcePath = record.value(QStringLiteral("tex_file")).toString().trimmed();
        if (!sourcePath.isEmpty() && QFileInfo(sourcePath).isRelative()) {
            sourcePath = QDir(d->docsRoot).absoluteFilePath(sourcePath);
        }
        if (!sourcePath.isEmpty()) {
            const QFileInfo sourceInfo(sourcePath);
            sourcePath = sourceInfo.canonicalFilePath().isEmpty() ? sourceInfo.absoluteFilePath() : sourceInfo.canonicalFilePath();
        }
        return {
            {QStringLiteral("available"), true},
            {QStringLiteral("annotationId"), stableId},
            {QStringLiteral("threadRootId"), rootId},
            {QStringLiteral("needsAction"), rootRecord.value(QStringLiteral("needs_action")).toBool()},
            {QStringLiteral("threadActionAvailable"), !rootRecord.value(QStringLiteral("logical_comment_id")).toString().isEmpty() && !rootRecord.value(QStringLiteral("deleted")).toBool()},
            {QStringLiteral("pdfPath"), pdfInfo.absoluteFilePath()},
            {QStringLiteral("priority"), record.value(QStringLiteral("priority")).toString(QStringLiteral("normal"))},
            {QStringLiteral("locationPresent"), record.contains(QStringLiteral("location")) && !rawLocation.isNull()},
            {QStringLiteral("location"), rawLocation.isObject() ? rawLocation.toObject().toVariantMap() : QVariantMap()},
            {QStringLiteral("chatGptUrl"), record.value(QStringLiteral("chatgpt_url")).toString()},
            {QStringLiteral("sourcePath"), sourcePath},
            {QStringLiteral("sourceLine"), record.value(QStringLiteral("tex_line")).toInt()},
            {QStringLiteral("linking"), (rootRecord.contains(QStringLiteral("linking")) ? rootRecord : record).value(QStringLiteral("linking")).toObject().toVariantMap()},
        };
    }
    return {};
}

void PageView::completeCommentThreadAction(const QString &rootId, bool success)
{
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (!window->annotation()) continue;
        const auto info = annotationRelatedInfo(window->annotation()->uniqueName());
        if (info.value(QStringLiteral("threadRootId")).toString() != rootId) continue;
        window->completeThreadActionOperation(success);
        window->setThreadActionInfo(info);
    }
}

void PageView::refreshAnnotationRelatedInfo(const QString &annotationId, const QString &status)
{
    const QString stableId = normalizedCommentId(annotationId);
    const QVariantMap info = annotationRelatedInfo(stableId);
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (window->annotation() && normalizedCommentId(window->annotation()->uniqueName()) == stableId) {
            window->setRelatedInfo(info, status);
        }
    }
}

void PageView::refreshOpenAnnotationRelatedInfo()
{
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (window->annotation()) {
            const auto info = annotationRelatedInfo(window->annotation()->uniqueName());
            if (window->hasUnsavedRelatedChanges()) window->setThreadActionInfo(info);
            else window->setRelatedInfo(info);
        }
    }
}

void PageView::completeAnnotationRelatedOperation(const QString &annotationId, const QVariantMap &result)
{
    const QString stableId = normalizedCommentId(annotationId);
    const bool success = result.value(QStringLiteral("success")).toBool();
    const QString message = success ? i18n("Details saved.") : result.value(QStringLiteral("error")).toString();
    if (success) {
        refreshAnnotationRelatedInfo(stableId, message);
        if (result.value(QStringLiteral("openChatGpt")).toBool()) {
            const QVariantMap info = annotationRelatedInfo(stableId);
            Q_EMIT commentRelatedOpenChatGptRequested(info.value(QStringLiteral("chatGptUrl")).toString());
        }
        return;
    }
    for (AnnotWindow *window : std::as_const(d->m_annowindows)) {
        if (window->annotation() && normalizedCommentId(window->annotation()->uniqueName()) == stableId) {
            window->completeRelatedOperation(false, message.isEmpty() ? i18n("The related information could not be saved.") : message);
        }
    }
}

void PageView::openReviewAnnotationWindow(Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation) {
        return;
    }
    const Thoughts::CommentFragment root = Thoughts::commentFragmentGroup(d->document, annotation, pageNumber).root();
    if (root.annotation) {
        annotation = root.annotation;
        pageNumber = root.page;
    }

    if (d->m_reviewAnnotationWindow && d->m_reviewAnnotationWindow->annotation() != annotation) {
        d->m_reviewAnnotationWindow->close();
    }

    openAnnotationWindow(annotation, pageNumber);
    for (AnnotWindow *aw : std::as_const(d->m_annowindows)) {
        if (aw->annotation() == annotation) {
            d->m_reviewAnnotationWindow = aw;
            break;
        }
    }
}

void PageView::closeReviewAnnotationWindow()
{
    if (d->m_reviewAnnotationWindow) {
        d->m_reviewAnnotationWindow->close();
        d->m_reviewAnnotationWindow.clear();
    }
}

void PageView::setReviewSelection(Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation || pageNumber < 0 || pageNumber >= static_cast<int>(d->document->pages())) {
        annotation = nullptr;
        pageNumber = -1;
    } else {
        const Thoughts::CommentFragment root = Thoughts::commentFragmentGroup(d->document, annotation, pageNumber).root();
        if (root.annotation) {
            annotation = root.annotation;
            pageNumber = root.page;
        }
        const Thoughts::CommentFragment threadRoot = Thoughts::commentThreadRoot(d->document, annotation, pageNumber);
        if (threadRoot.annotation) {
            annotation = threadRoot.annotation;
            pageNumber = threadRoot.page;
        }
    }

    if (d->m_reviewSelectionAnnotation == annotation && d->m_reviewSelectionPageNumber == pageNumber) {
        return;
    }

    d->m_reviewSelectionAnnotation = annotation;
    d->m_reviewSelectionPageNumber = pageNumber;
    d->m_reviewSelectionUniqueName = annotation ? annotation->uniqueName() : QString();
    viewport()->update();
}

void PageView::slotAnnotationWindowDestroyed(QObject *window)
{
    d->m_annowindows.remove(static_cast<AnnotWindow *>(window));
}

void PageView::displayMessage(const QString &message, const QString &details, PageViewMessage::Icon icon, int duration)
{
    if (!Okular::Settings::showOSD()) {
        if (icon == PageViewMessage::Error) {
            if (!details.isEmpty()) {
                KMessageBox::detailedError(this, message, details);
            } else {
                KMessageBox::error(this, message);
            }
        }
        return;
    }

    // hide messageWindow if string is empty
    if (message.isEmpty()) {
        d->messageWindow->hide();
        return;
    }

    // display message (duration is length dependent)
    if (duration == -1) {
        duration = 500 + 100 * message.length();
        if (!details.isEmpty()) {
            duration += 500 + 100 * details.length();
        }
    }
    d->messageWindow->display(message, details, icon, duration);
}

void PageView::reparseConfig()
{
    // set smooth scrolling policies
    PageView::updateSmoothScrollAnimationSpeed();

    // set the scroll bars policies
    Qt::ScrollBarPolicy scrollBarMode = Okular::Settings::showScrollBars() ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff;
    if (horizontalScrollBarPolicy() != scrollBarMode) {
        setHorizontalScrollBarPolicy(scrollBarMode);
        setVerticalScrollBarPolicy(scrollBarMode);
    }

    if (Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::Summary && ((int)Okular::Settings::viewColumns() != d->setting_viewCols)) {
        d->setting_viewCols = Okular::Settings::viewColumns();

        slotRelayoutPages();
    }

    if (Okular::Settings::rtlReadingDirection() != d->rtl_Mode) {
        d->rtl_Mode = Okular::Settings::rtlReadingDirection();
        slotRelayoutPages();
    }

    updatePageStep();

    if (d->annotator) {
        d->annotator->reparseConfig();
    }

    // Something like invert colors may have changed
    // As we don't have a way to find out the old value
    // We just update the viewport, this shouldn't be that bad
    // since it's just a repaint of pixmaps we already have
    viewport()->update();
}

KActionCollection *PageView::actionCollection() const
{
    return d->actionCollection;
}

QAction *PageView::toggleFormsAction() const
{
    return d->aToggleForms;
}

int PageView::contentAreaWidth() const
{
    return horizontalScrollBar()->maximum() + viewport()->width();
}

int PageView::contentAreaHeight() const
{
    return verticalScrollBar()->maximum() + viewport()->height();
}

QPoint PageView::contentAreaPosition() const
{
    return QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

QPoint PageView::contentAreaPoint(const QPoint pos) const
{
    return pos + contentAreaPosition();
}

QPointF PageView::contentAreaPoint(const QPointF pos) const
{
    return pos + contentAreaPosition();
}

QString PageViewPrivate::selectedText() const
{
    if (pagesWithTextSelection.isEmpty()) {
        return QString();
    }

    QString text;
    QList<int> selpages = pagesWithTextSelection.values();
    std::sort(selpages.begin(), selpages.end());
    const Okular::Page *pg = nullptr;
    if (selpages.count() == 1) {
        pg = document->page(selpages.first());
        text.append(pg->text(pg->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
    } else {
        pg = document->page(selpages.first());
        text.append(pg->text(pg->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        int end = selpages.count() - 1;
        for (int i = 1; i < end; ++i) {
            pg = document->page(selpages.at(i));
            text.append(pg->text(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
        }
        pg = document->page(selpages.last());
        text.append(pg->text(pg->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
    }

    if (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    return text;
}

QMimeData *PageView::getTableContents() const
{
    QString selText;
    QString selHtml;
    QList<double> xs = d->tableSelectionCols;
    QList<double> ys = d->tableSelectionRows;
    xs.prepend(0.0);
    xs.append(1.0);
    ys.prepend(0.0);
    ys.append(1.0);
    selHtml = QString::fromLatin1(
        "<html><head>"
        "<meta content=\"text/html; charset=utf-8\" http-equiv=\"Content-Type\">"
        "</head><body><table>");
    for (int r = 0; r + 1 < ys.length(); r++) {
        selHtml += QLatin1String("<tr>");
        for (int c = 0; c + 1 < xs.length(); c++) {
            Okular::NormalizedRect cell(xs[c], ys[r], xs[c + 1], ys[r + 1]);
            if (c) {
                selText += QLatin1Char('\t');
            }
            QString txt;
            for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
                // first, crop the cell to this part
                if (!tsp.rectInSelection.intersects(cell)) {
                    continue;
                }
                Okular::NormalizedRect cellPart = tsp.rectInSelection & cell; // intersection

                // second, convert it from table coordinates to part coordinates
                cellPart.left -= tsp.rectInSelection.left;
                cellPart.left /= (tsp.rectInSelection.right - tsp.rectInSelection.left);
                cellPart.right -= tsp.rectInSelection.left;
                cellPart.right /= (tsp.rectInSelection.right - tsp.rectInSelection.left);
                cellPart.top -= tsp.rectInSelection.top;
                cellPart.top /= (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
                cellPart.bottom -= tsp.rectInSelection.top;
                cellPart.bottom /= (tsp.rectInSelection.bottom - tsp.rectInSelection.top);

                // third, convert from part coordinates to item coordinates
                cellPart.left *= (tsp.rectInItem.right - tsp.rectInItem.left);
                cellPart.left += tsp.rectInItem.left;
                cellPart.right *= (tsp.rectInItem.right - tsp.rectInItem.left);
                cellPart.right += tsp.rectInItem.left;
                cellPart.top *= (tsp.rectInItem.bottom - tsp.rectInItem.top);
                cellPart.top += tsp.rectInItem.top;
                cellPart.bottom *= (tsp.rectInItem.bottom - tsp.rectInItem.top);
                cellPart.bottom += tsp.rectInItem.top;

                // now get the text
                Okular::RegularAreaRect rects;
                rects.append(cellPart);
                txt += tsp.item->page()->text(&rects, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
            }
            QString html = txt;
            selText += txt.replace(QLatin1Char('\n'), QLatin1Char(' '));
            html.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;")).replace(QLatin1Char('>'), QLatin1String("&gt;"));
            // Remove newlines, do not turn them into <br>, because
            // Excel interprets <br> within cell as new cell...
            html.replace(QLatin1Char('\n'), QLatin1String(" "));
            selHtml += QStringLiteral("<td>") + html + QStringLiteral("</td>");
        }
        selText += QLatin1Char('\n');
        selHtml += QLatin1String("</tr>\n");
    }
    selHtml += QLatin1String("</table></body></html>\n");

    QMimeData *md = new QMimeData();
    md->setText(selText);
    md->setHtml(selHtml);

    return md;
}

QString PageView::selectedText() const
{
    return d->selectedText();
}

void PageView::copyTextSelection(TextCopyMode mode) const
{
    switch (d->mouseMode) {
    case Okular::Settings::EnumMouseMode::Browse: {
        if (auto *annotation = d->mouseAnnotation->annotation()) {
            const QString text = annotation->contents();
            if (!text.isEmpty()) {
                QClipboard *cb = QApplication::clipboard();
                cb->setText(text, QClipboard::Clipboard);
            }
        }
    } break;

    case Okular::Settings::EnumMouseMode::TableSelect: {
        QClipboard *cb = QApplication::clipboard();
        cb->setMimeData(getTableContents(), QClipboard::Clipboard);
    } break;

    case Okular::Settings::EnumMouseMode::TextSelect: {
        QString text = d->selectedText();
        if (!text.isEmpty()) {
            QClipboard *cb = QApplication::clipboard();
            if (mode == TextCopyMode::WithoutLineBreaks) {
                // copy the text without line breaks
                cb->setText(Okular::removeLineBreaks(text), QClipboard::Clipboard);
            } else {
                // Copy the original text without any modifications
                cb->setText(text, QClipboard::Clipboard);
            }
        }
    } break;
    }
}

void PageView::annotateTextSelection(Okular::HighlightAnnotation::HighlightType type)
{
    if (d->mouseMode != Okular::Settings::EnumMouseMode::TextSelect || !d->document->isAllowed(Okular::AllowNotes)) {
        return;
    }

    QColor annotationColor(type == Okular::HighlightAnnotation::StrikeOut ? Qt::red : Qt::yellow);
    qreal annotationOpacity = 1.0;
    const QDomElement highlighterTool = d->annotator->builtinTool(type == Okular::HighlightAnnotation::StrikeOut ? 4 : 1);
    const QDomElement engineElement = highlighterTool.firstChildElement(QStringLiteral("engine"));
    const QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    const QColor configuredColor(annotationElement.attribute(QStringLiteral("color"), engineElement.attribute(QStringLiteral("color"), annotationColor.name())));
    if (configuredColor.isValid()) {
        annotationColor = configuredColor;
    }
    if (annotationElement.hasAttribute(QStringLiteral("opacity"))) {
        annotationOpacity = annotationElement.attribute(QStringLiteral("opacity"), QStringLiteral("1.0")).toDouble();
    }

    QList<int> selectedPages;
    selectedPages.reserve(d->document->pages());
    for (uint pageNumber = 0; pageNumber < d->document->pages(); ++pageNumber) {
        const Okular::Page *page = d->document->page(pageNumber);
        if (page && page->textSelection() && !page->textSelection()->isEmpty()) {
            selectedPages.append(static_cast<int>(pageNumber));
        }
    }

    Okular::Annotation *firstAnnotation = nullptr;
    int firstAnnotationPage = -1;
    const QDateTime now = QDateTime::currentDateTime();
    const QString logicalCommentId = QStringLiteral("thoughts-") + QUuid::createUuid().toString();
    QList<Okular::Document::PageAnnotation> annotations;
    int fragmentIndex = 0;

    for (const int pageNumber : std::as_const(selectedPages)) {
        const Okular::Page *page = d->document->page(pageNumber);
        const Okular::RegularAreaRect *selection = page ? page->textSelection() : nullptr;
        if (!selection || selection->isEmpty()) {
            continue;
        }

        Okular::NormalizedRect boundingRect;
        bool hasBoundingRect = false;
        auto *annotation = new Okular::HighlightAnnotation();
        annotation->setHighlightType(type);

        for (const Okular::NormalizedRect &rect : std::as_const(*selection)) {
            if (hasBoundingRect) {
                boundingRect |= rect;
            } else {
                boundingRect = rect;
                hasBoundingRect = true;
            }

            Okular::HighlightAnnotation::Quad quad;
            quad.setCapStart(false);
            quad.setCapEnd(false);
            quad.setFeather(1.0);
            quad.setPoint(Okular::NormalizedPoint(rect.left, rect.bottom), 0);
            quad.setPoint(Okular::NormalizedPoint(rect.right, rect.bottom), 1);
            quad.setPoint(Okular::NormalizedPoint(rect.right, rect.top), 2);
            quad.setPoint(Okular::NormalizedPoint(rect.left, rect.top), 3);
            annotation->highlightQuads().append(quad);
        }

        if (!hasBoundingRect) {
            delete annotation;
            continue;
        }

        annotation->setBoundingRectangle(boundingRect);
        annotation->setUniqueName(fragmentIndex == 0 ? logicalCommentId : Thoughts::makeCommentFragmentId(logicalCommentId, fragmentIndex));
        annotation->setContents(QString());
        annotation->setAuthor(Okular::Settings::identityAuthor());
        annotation->setCreationDate(now);
        annotation->setModificationDate(now);
        annotation->style().setColor(annotationColor);
        annotation->style().setOpacity(annotationOpacity);
        annotation->window().setSummary(i18n("Text Annotation"));

        annotations.append(qMakePair(pageNumber, static_cast<Okular::Annotation *>(annotation)));
        if (!firstAnnotation) {
            firstAnnotation = annotation;
            firstAnnotationPage = pageNumber;
        }
        ++fragmentIndex;
    }

    if (!firstAnnotation) {
        return;
    }

    d->document->addPageAnnotations(annotations);
    textSelectionClear();
    // Before the window opens: it keeps the focus for typing.
    Q_EMIT annotationAdded(firstAnnotation, firstAnnotationPage);
    openAnnotationWindow(firstAnnotation, firstAnnotationPage);
}

void PageView::setCommentRecordProvider(std::function<QJsonObject(const QString &)> provider)
{
    d->commentRecordProvider = std::move(provider);
}

void PageView::setLinkOverrideHandlers(LinkOverrideClickHandler click, LinkOverrideMenuProvider menu)
{
    d->linkOverrideClick = std::move(click);
    d->linkOverrideMenu = std::move(menu);
}

void PageView::setDocsRoot(const QString &docsRoot)
{
    const QFileInfo rootInfo(docsRoot);
    d->docsRoot = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath();
    collapseManagedCommentReplyAnnotations(d->document, d->docsRoot);
    viewport()->update();
}

bool PageView::selectAgentProposal(const QString &proposalId)
{
    const auto matchingOverlay = [this, &proposalId](const QString &role, bool primaryOnly) {
        return std::find_if(d->agentProposalOverlays.cbegin(), d->agentProposalOverlays.cend(), [&proposalId, &role, primaryOnly](const Thoughts::AgentProposalOverlay &overlay) {
            return overlay.id == proposalId && (role.isEmpty() || overlay.role == role) && (!primaryOnly || !overlay.duplicate);
        });
    };
    // Prefer the proposal's own occurrence over LaTeX-made copies such as contents entries.
    auto found = matchingOverlay(QStringLiteral("after"), true);
    if (found == d->agentProposalOverlays.cend()) {
        found = matchingOverlay({}, true);
    }
    if (found == d->agentProposalOverlays.cend()) {
        found = matchingOverlay(QStringLiteral("after"), false);
    }
    if (found == d->agentProposalOverlays.cend()) {
        found = matchingOverlay({}, false);
    }
    if (found == d->agentProposalOverlays.cend()) {
        // Never read yet: read now. A reading under way is not started again; it was thrown away
        // and restarted here, so a proposal opened while its PDF loaded waited for a second one.
        if (!d->agentProposalOverlaysReady && !d->agentProposalReloadRunning) {
            reloadAgentProposalOverlays();
        }
        return false;
    }
    d->selectedAgentProposalId = proposalId;
    // A jump to a page out of sight is immediate: scrolling there drew every page in between.
    const bool nearby = std::abs(static_cast<int>(d->document->currentPage()) - found->page) <= 1;
    Okular::DocumentViewport focusViewport(found->page);
    const Okular::Page *page = d->document->page(found->page);
    if (page && page->width() > 0.0 && page->height() > 0.0) {
        focusViewport.rePos.enabled = true;
        focusViewport.rePos.normalizedX = std::max(
            0.0,
            (found->normalized ? found->rect.left() : found->rect.left() / page->width()) - 0.04);
        focusViewport.rePos.normalizedY = std::max(
            0.0,
            (found->normalized ? found->rect.top() : found->rect.top() / page->height()) - 0.10);
        focusViewport.rePos.pos = Okular::DocumentViewport::TopLeft;
    }
    d->document->setViewport(focusViewport, nullptr, nearby);
    viewport()->update();
    if (page && page->width() > 0.0 && page->height() > 0.0) {
        const QRectF rect = found->normalized ? found->rect
                                              : QRectF(found->rect.left() / page->width(), found->rect.top() / page->height(), found->rect.width() / page->width(),
                                                       found->rect.height() / page->height());
        placeAgentProposalWindowNear(proposalId, found->page, rect);
    }
    return true;
}

void PageView::placeAgentProposalWindowNear(const QString &proposalId, int page, const QRectF &rect)
{
    if (!d->agentProposalWindow || d->agentProposalWindow->proposalId() != proposalId || page < 0 || !rect.isValid()) return;
    d->agentProposalPlacement = PageViewPrivate::AgentProposalPlacement{proposalId, page, rect};
    // After the view's move there (a smooth scroll ends in stateChanged) and its layout.
    if (d->scroller->state() == QScroller::Inactive) QTimer::singleShot(0, this, &PageView::applyAgentProposalWindowPlacement);
}

QRect PageView::globalPageRect(int page, const QRectF &rect) const
{
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible() || item->pageNumber() != page) continue;
        const QRect geometry = item->uncroppedGeometry();
        const QRectF onView(geometry.x() + rect.left() * geometry.width() - horizontalScrollBar()->value(),
                            geometry.y() + rect.top() * geometry.height() - verticalScrollBar()->value(), rect.width() * geometry.width(), rect.height() * geometry.height());
        return QRect(viewport()->mapToGlobal(onView.topLeft().toPoint()), onView.size().toSize().expandedTo(QSize(1, 1)));
    }
    return {};
}

void PageView::applyAgentProposalWindowPlacement()
{
    if (!d->agentProposalPlacement || d->scroller->state() != QScroller::Inactive) return;
    const auto placement = *std::exchange(d->agentProposalPlacement, std::nullopt);
    if (!d->agentProposalWindow || !d->agentProposalWindow->isVisible() || d->agentProposalWindow->proposalId() != placement.proposalId) return;
    const QRect passage = globalPageRect(placement.page, placement.rect);
    QRect page;
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (item->pageNumber() != placement.page) continue;
        page = QRect(viewport()->mapToGlobal(item->croppedGeometry().topLeft() - QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value())),
                     item->croppedGeometry().size());
    }
    // The page's width as far as it is on screen: a page wider than the view has no room beside it.
    const QRect view(viewport()->mapToGlobal(QPoint(0, 0)), viewport()->size());
    page = QRect(QPoint(std::max(page.left(), view.left()), page.top()), QPoint(std::min(page.right(), view.right()), page.bottom()));
    const QScreen *shown = screen();
    if (!passage.isValid() || !shown) return;
    d->agentProposalWindow->placeBeside(passage, page, shown->availableGeometry());
}

void PageView::placeAgentProposalWindowInCorner()
{
    const QScreen *shown = screen();
    if (!d->agentProposalWindow || !shown) return;
    d->agentProposalPlacement.reset();
    d->agentProposalWindow->placeInCorner(QRect(viewport()->mapToGlobal(QPoint(0, 0)), viewport()->size()), shown->availableGeometry());
}

bool PageView::agentProposalOverlaysReady() const
{
    return d->agentProposalOverlaysReady;
}

bool PageView::agentProposalBuiltIntoPdf(const QString &proposalId, bool *known) const
{
    const bool isKnown = d->agentProposalOverlaysReady && d->builtAgentProposalIdsKnown && d->agentProposalOverlaysUrl == d->document->currentDocument();
    if (known) {
        *known = isKnown;
    }
    return isKnown && d->builtAgentProposalIds.contains(proposalId);
}

void PageView::refreshAgentProposalOverlays()
{
    reloadAgentProposalOverlays(true);
}

void PageView::setAgentProposalTextHighlight(int page, std::unique_ptr<Okular::RegularAreaRect> area)
{
    textSelectionClear();
    if (!area || area->isEmpty() || page < 0 || page >= static_cast<int>(d->document->pages())) {
        return;
    }
    d->agentProposalTextHighlight = true;
    d->pagesWithTextSelection.insert(page);
    d->document->setPageTextSelection(page, std::move(area), QColor(255, 210, 0, 150));
}

void PageView::clearAgentProposalTextHighlight()
{
    textSelectionClear();
}

void PageView::showPassageRegions(const QList<QPair<int, QRectF>> &regions)
{
    textSelectionClear();
    const int pages = static_cast<int>(d->document->pages());
    QMap<int, Okular::RegularAreaRect> areas;
    for (const auto &[page, rect] : regions) {
        if (page < 1 || page > pages || !rect.isValid()) continue;
        areas[page - 1].append(Okular::NormalizedRect(rect.left(), rect.top(), rect.right(), rect.bottom()));
    }
    if (areas.isEmpty()) return;
    for (auto it = areas.cbegin(); it != areas.cend(); ++it) {
        d->pagesWithTextSelection.insert(it.key());
        d->document->setPageTextSelection(it.key(), std::make_unique<Okular::RegularAreaRect>(it.value()), QColor(255, 210, 0, 150));
    }
    // The first region near the top of the view, as a found proposal is shown.
    const auto &[firstPage, firstRect] = *std::find_if(regions.cbegin(), regions.cend(), [&](const auto &region) {
        return areas.contains(region.first - 1) && region.second.isValid();
    });
    Okular::DocumentViewport focus(firstPage - 1);
    focus.rePos.enabled = true;
    focus.rePos.normalizedX = std::max(0.0, firstRect.left() - 0.04);
    focus.rePos.normalizedY = std::max(0.0, firstRect.top() - 0.10);
    focus.rePos.pos = Okular::DocumentViewport::TopLeft;
    d->document->setViewport(focus, nullptr, true);
    viewport()->update();
}

void PageView::showPlace(int page, std::optional<QPointF> point, std::optional<QRectF> rect)
{
    textSelectionClear();
    const int pages = static_cast<int>(d->document->pages());
    if (page < 0 || page >= pages || d->items.size() != pages) return;
    if (rect && rect->isValid()) {
        // The passage marked as a found proposal is, until the next click or a few seconds on.
        auto area = std::make_unique<Okular::RegularAreaRect>();
        area->append(Okular::NormalizedRect(rect->left(), rect->top(), rect->right(), rect->bottom()));
        d->pagesWithTextSelection.insert(page);
        d->document->setPageTextSelection(page, std::move(area), QColor(255, 210, 0, 150));
        d->placeHighlight = true;
        d->placeHighlightTimer->start();
    }
    if (!point && rect && rect->isValid()) point = rect->topLeft();
    Okular::DocumentViewport focus(page);
    if (point) {
        // A view resized a moment ago (a window just shown) lays its pages out first: the margin
        // is in pixels of the page as it will be shown.
        if (d->delayResizeEventTimer->isActive()) delayedResizeEvent();
        else if (d->dirtyLayout) slotRelayoutPages();
        // The point a little below the top of the view: a margin of pixels, not of the page,
        // which can be many screens tall (a web article saved as one page).
        const PageViewItem *item = d->items.at(page);
        const QRect geometry = item->croppedGeometry();
        const Okular::NormalizedRect &crop = item->crop();
        const double marginY = geometry.height() > 0 ? std::min(viewport()->height() / 8.0, 80.0) / geometry.height() * (crop.bottom - crop.top) : 0.02;
        const double marginX = geometry.width() > 0 ? 24.0 / geometry.width() * (crop.right - crop.left) : 0.02;
        focus.rePos.enabled = true;
        focus.rePos.normalizedX = std::max(0.0, point->x() - marginX);
        focus.rePos.normalizedY = std::max(0.0, point->y() - marginY);
        focus.rePos.pos = Okular::DocumentViewport::TopLeft;
    }
    d->document->setViewport(focus, nullptr, false);
    viewport()->update();
}

bool PageView::placeHighlightShown() const
{
    return d->placeHighlight;
}

void PageView::setPlaceHighlightDuration(int milliseconds)
{
    d->placeHighlightTimer->setInterval(milliseconds);
}

void PageView::openAgentProposalWindow(const AgentProposalSidebarEntry &proposal)
{
    hideAgentProposalHoverBar();
    if (proposal.id.isEmpty()) {
        return;
    }
    d->selectedAgentProposalId = proposal.id;
    viewport()->update();
    const bool firstOpen = !d->agentProposalWindow;
    if (firstOpen) {
        d->agentProposalWindow = new AgentProposalWindow(this);
        connect(d->agentProposalWindow, &AgentProposalWindow::closed, this, [this] {
            d->selectedAgentProposalId.clear();
            clearAgentProposalTextHighlight();
            viewport()->update();
            if (d->agentProposalWindow) {
                Q_EMIT agentProposalWindowClosed(d->agentProposalWindow->proposalId());
            }
        });
        connect(d->agentProposalWindow, &AgentProposalWindow::resolutionRequested, this, &PageView::agentProposalResolutionRequested);
        connect(d->agentProposalWindow, &AgentProposalWindow::undoRequested, this, &PageView::agentProposalUndoRequested);
        connect(d->agentProposalWindow, &AgentProposalWindow::redoRequested, this, &PageView::agentProposalRedoRequested);
        connect(d->agentProposalWindow, &AgentProposalWindow::openSourceRequested, this, &PageView::agentProposalOpenSourceRequested);
    }
    d->agentProposalWindow->setProposal(proposal);
    // Out of the way until the passage is found (then beside it): the view's top right corner.
    d->agentProposalPlacement.reset();
    if (firstOpen || !d->agentProposalWindow->isVisible()) placeAgentProposalWindowInCorner();
    d->agentProposalWindow->show();
    d->agentProposalWindow->raise();
    d->agentProposalWindow->activateWindow();
}

void PageView::showAgentProposalLocationUnavailable(const QString &proposalId)
{
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->setLocationUnavailable(true);
        // Nowhere on the page to be beside: the view's corner, not over the middle of the page.
        placeAgentProposalWindowInCorner();
    }
}

void PageView::showAgentProposalBuildRequired(const QString &proposalId)
{
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->setPdfBuildRequired(true);
    }
}

void PageView::setAgentProposalOperationPending(const QString &proposalId, bool pending, const QString &message)
{
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->setOperationPending(pending, message);
    }
}

void PageView::showAgentProposalResolutionError(const QString &proposalId, const QString &message)
{
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->showResolutionError(message);
    }
}

void PageView::showAgentProposalResolutionSuccess(const QString &proposalId, const QString &message)
{
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->showResolutionSuccess(message);
    }
}

void PageView::refreshAgentProposalWindow(const AgentProposalSidebarEntry &proposal)
{
    if (!d->agentProposalWindow || !d->agentProposalWindow->isVisible()) {
        return;
    }
    if (proposal.id.isEmpty()) {
        d->agentProposalWindow->close();
        return;
    }
    if (d->agentProposalWindow->proposalId() == proposal.id) {
        d->agentProposalWindow->setProposal(proposal);
    }
}

QString PageView::openAgentProposalId() const
{
    return d->agentProposalWindow && d->agentProposalWindow->isVisible()
        ? d->agentProposalWindow->proposalId()
        : QString();
}

void PageView::suppressAgentProposal(const QString &proposalId)
{
    if (proposalId.isEmpty()) {
        return;
    }
    d->suppressedAgentProposalIds.insert(proposalId);
    d->agentProposalOverlays.removeIf([&proposalId](const Thoughts::AgentProposalOverlay &overlay) { return overlay.id == proposalId; });
    if (d->selectedAgentProposalId == proposalId) {
        d->selectedAgentProposalId.clear();
    }
    if (d->agentProposalWindow && d->agentProposalWindow->proposalId() == proposalId) {
        d->agentProposalWindow->close();
    }
    checkAgentProposalHoverAnchor();
    viewport()->update();
}

void PageView::setDecidedAgentProposalIds(const QSet<QString> &proposalIds)
{
    d->decidedAgentProposalIds = proposalIds;
    if (d->agentProposalOverlays.removeIf([&proposalIds](const Thoughts::AgentProposalOverlay &overlay) { return proposalIds.contains(overlay.id); }) > 0) {
        checkAgentProposalHoverAnchor();
        viewport()->update();
    }
}

void PageView::restoreAgentProposal(const QString &proposalId)
{
    if (proposalId.isEmpty()) {
        return;
    }
    d->suppressedAgentProposalIds.remove(proposalId);
    d->decidedAgentProposalIds.remove(proposalId);
    reloadAgentProposalOverlays(true);
}

bool PageView::pdfLinkAnchorDocument() const
{
    return isPdfLinkAnchorDocument();
}

QString PageView::createPdfLinkAnchorForSelection(const QString &anchorName)
{
    QString error;
    if (!createPdfLinkAnchor(anchorName, nullptr, &error)) {
        return error;
    }
    return QString();
}

QRectF PageView::sectionHeadingLineAt(const PageViewItem *item, const QPointF &eventPos) const
{
    if (!item || !item->page()) return {};
    const Okular::Page *page = item->page();
    if (!page->hasTextPage()) d->document->requestTextPage(item->pageNumber());
    if (!page->hasTextPage()) return {};
    const double x = item->absToPageX(eventPos.x()), y = item->absToPageY(eventPos.y());
    const Okular::TextEntity::List words = page->words(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
    int clicked = -1;
    QList<double> pageHeights;
    for (int i = 0; i < words.size(); ++i) {
        const Okular::NormalizedRect area = words[i].area();
        if (words[i].text().trimmed().isEmpty()) continue;
        pageHeights.append(area.bottom - area.top);
        if (clicked < 0 && x >= area.left - 0.003 && x <= area.right + 0.003 && y >= area.top && y <= area.bottom) clicked = i;
    }
    if (clicked < 0) return {};
    // The clicked word's line, as far as its words follow each other closely (one column).
    const Okular::NormalizedRect at = words[clicked].area();
    QList<int> band;
    for (int i = 0; i < words.size(); ++i) {
        const Okular::NormalizedRect area = words[i].area();
        const double centre = (area.top + area.bottom) / 2;
        if (!words[i].text().trimmed().isEmpty() && centre >= at.top && centre <= at.bottom) band.append(i);
    }
    std::sort(band.begin(), band.end(), [&words](int a, int b) { return words[a].area().left < words[b].area().left; });
    int first = int(band.indexOf(clicked)), last = first;
    while (first > 0 && words[band[first]].area().left - words[band[first - 1]].area().right < 0.03) --first;
    while (last + 1 < band.size() && words[band[last + 1]].area().left - words[band[last]].area().right < 0.03) ++last;
    QRectF line;
    QStringList text;
    QList<double> lineHeights;
    for (int k = first; k <= last; ++k) {
        const Okular::NormalizedRect area = words[band[k]].area();
        line |= QRectF(QPointF(area.left, area.top), QPointF(area.right, area.bottom));
        text.append(words[band[k]].text().simplified());
        lineHeights.append(area.bottom - area.top);
    }
    const QString lineText = text.join(QLatin1Char(' ')).simplified();
    const int wordCount = int(lineText.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
    if (!wordCount || wordCount > 15 || lineText.endsWith(QLatin1Char(',')) || lineText.endsWith(QLatin1Char(';'))) return {};
    const auto median = [](QList<double> values) {
        std::sort(values.begin(), values.end());
        return values.isEmpty() ? 0.0 : values.at(values.size() / 2);
    };
    const bool larger = median(lineHeights) >= 1.08 * median(pageHeights);
    static const QRegularExpression numbered(QStringLiteral(R"(^(\d+(\.\d+)*\.?|[A-Z](\.\d+)+\.?|[A-Z]\.|[IVXLC]+\.)\s+\p{Lu})"));
    bool outlined = false;
    if (!larger && !numbered.match(lineText).hasMatch()) {
        const auto letters = [](const QString &value) {
            QString folded;
            for (const QChar c : value) if (c.isLetterOrNumber()) folded += c.toCaseFolded();
            return folded;
        };
        const QString shown = letters(lineText);
        std::function<bool(const QDomNode &)> names = [&](const QDomNode &parent) {
            for (QDomNode node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
                const QDomElement element = node.toElement();
                if (element.isNull()) continue;
                const QString title = letters(element.tagName());
                const Okular::DocumentViewport viewport(element.attribute(QStringLiteral("Viewport")));
                if (title.size() >= 3 && (shown.contains(title) || title.contains(shown)) && (!viewport.isValid() || viewport.pageNumber == item->pageNumber())) return true;
                if (names(element)) return true;
            }
            return false;
        };
        if (const Okular::DocumentSynopsis *synopsis = d->document->documentSynopsis()) outlined = shown.size() >= 3 && names(*synopsis);
        if (!outlined) return {};
    }
    return line;
}

bool PageView::isPdfLinkAnchorDocument(QString *paperKey) const
{
    if (paperKey) {
        paperKey->clear();
    }
    const QUrl documentUrl = d->document->currentDocument();
    if (d->docsRoot.isEmpty() || !documentUrl.isLocalFile()) {
        return false;
    }

    const QFileInfo documentInfo(documentUrl.toLocalFile());
    if (documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString documentPath = documentInfo.canonicalFilePath().isEmpty() ? documentInfo.absoluteFilePath() : documentInfo.canonicalFilePath();
    const QString relativePath = QDir::fromNativeSeparators(QDir(d->docsRoot).relativeFilePath(documentPath));
    const QStringList pathParts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const bool isGlobalThirdParty = pathParts.size() == 2 && pathParts.at(0) == QLatin1String("third_party");
    if (!isGlobalThirdParty) {
        return false;
    }

    const QString key = documentInfo.completeBaseName();
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!identifier.match(key).hasMatch()) {
        return false;
    }
    if (paperKey) {
        *paperKey = key;
    }
    return true;
}

static QString latexEscapePdfLinkText(const QString &text)
{
    QString escaped;
    const QString normalized = text.simplified();
    escaped.reserve(normalized.size());
    for (const QChar character : normalized) {
        switch (character.unicode()) {
        case '\\':
            escaped += QStringLiteral("\\textbackslash{}");
            break;
        case '{':
            escaped += QStringLiteral("\\{");
            break;
        case '}':
            escaped += QStringLiteral("\\}");
            break;
        case '%':
        case '$':
        case '#':
        case '_':
        case '&':
            escaped += QLatin1Char('\\');
            escaped += character;
            break;
        case '~':
            escaped += QStringLiteral("\\textasciitilde{}");
            break;
        case '^':
            escaped += QStringLiteral("\\textasciicircum{}");
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

static QString proposedPdfLinkAnchorName(const QString &text)
{
    QString proposed;
    bool pendingHyphen = false;
    const QString lower = text.simplified().toLower();
    for (const QChar character : lower) {
        const ushort code = character.unicode();
        const bool asciiLetter = code >= 'a' && code <= 'z';
        const bool digit = code >= '0' && code <= '9';
        if (asciiLetter || digit) {
            if (pendingHyphen && !proposed.isEmpty() && proposed.size() < 48) {
                proposed += QLatin1Char('-');
            }
            pendingHyphen = false;
            if (proposed.size() < 48) {
                proposed += character;
            }
        } else if (!proposed.isEmpty()) {
            pendingHyphen = true;
        }
        if (proposed.size() >= 48) {
            break;
        }
    }
    while (proposed.endsWith(QLatin1Char('-'))) {
        proposed.chop(1);
    }
    return proposed.isEmpty() ? QStringLiteral("anchor") : proposed;
}

bool PageView::createPdfLinkAnchor(const QString &anchorName, QString *latexLink, QString *error)
{
    if (latexLink) {
        latexLink->clear();
    }
    if (error) {
        error->clear();
    }
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    QString paperKey;
    if (!isPdfLinkAnchorDocument(&paperKey)) {
        return fail(i18n("Documentation link anchors can only be created in PDFs directly inside the workspace-global third_party folder."));
    }
    if (!d->document->isAllowed(Okular::AllowNotes)) {
        return fail(i18n("This PDF does not allow annotations."));
    }
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!identifier.match(anchorName).hasMatch()) {
        return fail(i18n("Anchor names must start with a letter or digit and contain only letters, digits, dots, underscores, or hyphens."));
    }

    const QString marker = QStringLiteral("doc-anchor:") + anchorName;
    for (uint pageNumber = 0; pageNumber < d->document->pages(); ++pageNumber) {
        const Okular::Page *page = d->document->page(pageNumber);
        if (!page) {
            continue;
        }
        for (const Okular::Annotation *annotation : page->annotations()) {
            if (annotation && annotation->contents().trimmed() == marker) {
                return fail(i18n("An anchor named '%1' already exists in this PDF.", anchorName));
            }
        }
    }

    QList<int> selectedPages;
    for (uint pageNumber = 0; pageNumber < d->document->pages(); ++pageNumber) {
        const Okular::Page *page = d->document->page(pageNumber);
        if (page && page->textSelection() && !page->textSelection()->isEmpty()) {
            selectedPages.append(static_cast<int>(pageNumber));
        }
    }
    if (selectedPages.size() != 1) {
        return fail(i18n("Select text on exactly one page before creating a documentation link anchor."));
    }

    const int pageNumber = selectedPages.constFirst();
    const Okular::Page *page = d->document->page(pageNumber);
    const Okular::RegularAreaRect *selection = page ? page->textSelection() : nullptr;
    const QString selectedText = page && selection
        ? page->text(selection, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour).simplified()
        : QString();
    if (!selection || selection->isEmpty() || selectedText.isEmpty()) {
        return fail(i18n("The selected passage is empty."));
    }

    Okular::NormalizedRect boundingRect;
    bool hasBoundingRect = false;
    auto *annotation = new Okular::HighlightAnnotation();
    annotation->setHighlightType(Okular::HighlightAnnotation::Highlight);
    for (const Okular::NormalizedRect &rect : std::as_const(*selection)) {
        if (hasBoundingRect) {
            boundingRect |= rect;
        } else {
            boundingRect = rect;
            hasBoundingRect = true;
        }
        Okular::HighlightAnnotation::Quad quad;
        quad.setCapStart(false);
        quad.setCapEnd(false);
        quad.setFeather(1.0);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.bottom), 0);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.bottom), 1);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.top), 2);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.top), 3);
        annotation->highlightQuads().append(quad);
    }
    if (!hasBoundingRect) {
        delete annotation;
        return fail(i18n("The selected passage has no usable highlight geometry."));
    }

    const QDateTime now = QDateTime::currentDateTime();
    annotation->setBoundingRectangle(boundingRect);
    annotation->setUniqueName(QStringLiteral("thoughts-") + QUuid::createUuid().toString());
    annotation->setContents(marker);
    annotation->setAuthor(Okular::Settings::identityAuthor());
    annotation->setCreationDate(now);
    annotation->setModificationDate(now);
    annotation->style().setColor(QColor(Qt::yellow));
    annotation->style().setOpacity(1.0);
    annotation->window().setSummary(i18n("Documentation Link Anchor"));
    d->document->addPageAnnotation(pageNumber, annotation);
    textSelectionClear();

    const QString command = QStringLiteral("\\PdfLink{%1}{%2}{%3}").arg(paperKey, anchorName, latexEscapePdfLinkText(selectedText));
    QApplication::clipboard()->setText(command, QClipboard::Clipboard);
    if (latexLink) {
        *latexLink = command;
    }
    return true;
}

void PageView::promptCreatePdfLinkAnchor()
{
    QString proposedName = proposedPdfLinkAnchorName(d->selectedText());
    while (true) {
        bool accepted = false;
        const QString anchorName = QInputDialog::getText(
            this,
            i18n("Create PDF Link Anchor"),
            i18n("Anchor name:"),
            QLineEdit::Normal,
            proposedName,
            &accepted);
        if (!accepted) {
            return;
        }
        proposedName = anchorName;

        QString latexLink;
        QString error;
        if (createPdfLinkAnchor(anchorName, &latexLink, &error)) {
            displayMessage(
                i18n("PDF link anchor created and LaTeX link copied. Save the PDF to keep the anchor."),
                QString(),
                PageViewMessage::Info,
                5000);
            return;
        }
        KMessageBox::error(this, error, i18n("Cannot Create PDF Link Anchor"));
    }
}

void PageView::addPopupNote(PageViewItem *item, const QPoint &eventPos)
{
    if (!item || d->mouseMode != Okular::Settings::EnumMouseMode::TextSelect || !d->document->isAllowed(Okular::AllowNotes)) {
        return;
    }

    QColor annotationColor(Qt::yellow);
    qreal annotationOpacity = 1.0;
    QString annotationIcon = QStringLiteral("Comment");
    const QDomElement popupNoteTool = d->annotator->builtinTool(popupNoteToolId);
    const QDomElement engineElement = popupNoteTool.firstChildElement(QStringLiteral("engine"));
    const QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    const QColor configuredColor(annotationElement.attribute(QStringLiteral("color"), engineElement.attribute(QStringLiteral("color"), QStringLiteral("#ffff00"))));
    if (configuredColor.isValid()) {
        annotationColor = configuredColor;
    }
    if (annotationElement.hasAttribute(QStringLiteral("opacity"))) {
        annotationOpacity = annotationElement.attribute(QStringLiteral("opacity"), QStringLiteral("1.0")).toDouble();
    }
    const int pageWidth = item->uncroppedWidth();
    const int pageHeight = item->uncroppedHeight();
    if (pageWidth <= 0 || pageHeight <= 0) {
        return;
    }

    const double iconWidth = 0.03;
    const double iconHeight = qMin(1.0, iconWidth * pageWidth / pageHeight);
    Okular::NormalizedRect boundary;
    boundary.left = qBound(0.0, item->absToPageX(eventPos.x()), 1.0);
    boundary.top = qBound(0.0, item->absToPageY(eventPos.y()), 1.0);
    boundary.right = boundary.left + iconWidth;
    boundary.bottom = boundary.top + iconHeight;
    if (boundary.right > 1.0) {
        boundary.left -= boundary.right - 1.0;
        boundary.right = 1.0;
    }
    if (boundary.bottom > 1.0) {
        boundary.top -= boundary.bottom - 1.0;
        boundary.bottom = 1.0;
    }

    auto *annotation = new Okular::TextAnnotation();
    annotation->setTextType(Okular::TextAnnotation::Linked);
    annotation->setTextIcon(annotationIcon);
    annotation->setBoundingRectangle(boundary);
    annotation->setContents(QString());
    annotation->setAuthor(Okular::Settings::identityAuthor());
    const QDateTime now = QDateTime::currentDateTime();
    annotation->setCreationDate(now);
    annotation->setModificationDate(now);
    annotation->style().setColor(annotationColor);
    annotation->style().setOpacity(annotationOpacity);
    annotation->window().setSummary(i18n("Pop-up Note"));

    d->document->addPageAnnotation(item->pageNumber(), annotation);
    Q_EMIT annotationAdded(annotation, item->pageNumber());
    openAnnotationWindow(annotation, item->pageNumber());
}

void PageView::selectAll()
{
    for (const PageViewItem *item : std::as_const(d->items)) {
        std::unique_ptr<Okular::RegularAreaRect> area = textSelectionForItem(item);
        d->pagesWithTextSelection.insert(item->pageNumber());
        d->document->setPageTextSelection(item->pageNumber(), std::move(area), palette().color(QPalette::Active, QPalette::Highlight));
    }
}

void PageView::createAnnotationsVideoWidgets(PageViewItem *item, const QList<Okular::Annotation *> &annotations)
{
    qDeleteAll(item->videoWidgets());
    item->videoWidgets().clear();

    for (Okular::Annotation *a : annotations) {
        if (a->subType() == Okular::Annotation::AMovie) {
            Okular::MovieAnnotation *movieAnn = static_cast<Okular::MovieAnnotation *>(a);
            VideoWidget *vw = new VideoWidget(movieAnn, movieAnn->movie(), d->document, viewport());
            item->videoWidgets().insert(movieAnn->movie(), vw);
            vw->pageInitialized();
        } else if (a->subType() == Okular::Annotation::ARichMedia) {
            Okular::RichMediaAnnotation *richMediaAnn = static_cast<Okular::RichMediaAnnotation *>(a);
            VideoWidget *vw = new VideoWidget(richMediaAnn, richMediaAnn->movie(), d->document, viewport());
            item->videoWidgets().insert(richMediaAnn->movie(), vw);
            vw->pageInitialized();
        } else if (a->subType() == Okular::Annotation::AScreen) {
            const Okular::ScreenAnnotation *screenAnn = static_cast<Okular::ScreenAnnotation *>(a);
            Okular::Movie *movie = GuiUtils::renditionMovieFromScreenAnnotation(screenAnn);
            if (movie) {
                VideoWidget *vw = new VideoWidget(screenAnn, movie, d->document, viewport());
                item->videoWidgets().insert(movie, vw);
                vw->pageInitialized();
            }
        }
    }
}

// BEGIN DocumentObserver inherited methods
void PageView::notifySetup(const QList<Okular::Page *> &pageSet, int setupFlags)
{
    collapseManagedCommentReplyAnnotations(d->document, d->docsRoot);
    bool documentChanged = setupFlags & Okular::DocumentObserver::DocumentChanged;
    if (documentChanged) {
        stopReadAloud();
        d->m_reviewMaskPageNumber = -1;
        d->m_reviewMaskRects.clear();
        d->m_reviewCardHighlightRects.clear();
        // The card's answer and feedback were placed on the pages (and PDF version) just replaced;
        // presenting the card again sets them for the new ones.
        d->reviewFeedback->clear();
        // Document feedback is placed again on the new pages (the part sets the PDF version), and
        // so are the reading overlays' marks.
        d->documentFeedback->resetPages();
        d->readingOverlays->resetPages();
        d->overlayFlashRects.clear();
        delete d->overlayMarkPopup.data();
        cancelPassageChange();
    }
    const bool allowfillforms = d->document->isAllowed(Okular::AllowFillForms);

    // reuse current pages if nothing new
    if ((pageSet.count() == d->items.count()) && !documentChanged && !(setupFlags & Okular::DocumentObserver::NewLayoutForPages)) {
        int count = pageSet.count();
        for (int i = 0; (i < count) && !documentChanged; i++) {
            if ((int)pageSet[i]->number() != d->items[i]->pageNumber()) {
                documentChanged = true;
            } else {
                // even if the document has not changed, allowfillforms may have
                // changed, so update all fields' "canBeFilled" flag
                const QSet<FormWidgetIface *> formWidgetsList = d->items[i]->formWidgets();
                for (FormWidgetIface *w : formWidgetsList) {
                    w->setCanBeFilled(allowfillforms);
                }
            }
        }

        if (!documentChanged) {
            if (setupFlags & Okular::DocumentObserver::UrlChanged) {
                // Here with UrlChanged and no document changed it means we
                // need to update all the Annotation* and Form* otherwise
                // they still point to the old document ones, luckily the old ones are still
                // around so we can look for the new ones using unique ids, etc
                d->mouseAnnotation->updateAnnotationPointers();

                if (d->m_reviewSelectionAnnotation) {
                    Okular::Annotation *newAnnotation = nullptr;
                    if (d->m_reviewSelectionPageNumber >= 0 && d->m_reviewSelectionPageNumber < static_cast<int>(d->document->pages()) && !d->m_reviewSelectionUniqueName.isEmpty()) {
                        newAnnotation = d->document->page(d->m_reviewSelectionPageNumber)->annotation(d->m_reviewSelectionUniqueName);
                    }
                    d->m_reviewSelectionAnnotation = newAnnotation;
                    if (!newAnnotation) {
                        d->m_reviewSelectionPageNumber = -1;
                        d->m_reviewSelectionUniqueName.clear();
                    }
                    viewport()->update();
                }

                for (AnnotWindow *aw : std::as_const(d->m_annowindows)) {
                    Okular::Annotation *newA = d->document->page(aw->pageNumber())->annotation(aw->annotation()->uniqueName());
                    aw->updateAnnotation(newA);
                }

                const QRect viewportRect(horizontalScrollBar()->value(), verticalScrollBar()->value(), viewport()->width(), viewport()->height());
                for (int i = 0; i < count; i++) {
                    PageViewItem *item = d->items[i];
                    const QSet<FormWidgetIface *> fws = item->formWidgets();
                    for (FormWidgetIface *w : fws) {
                        Okular::FormField *f = Okular::PagePrivate::findEquivalentForm(d->document->page(i), w->formField());
                        if (f) {
                            w->setFormField(f);
                        } else {
                            qWarning() << "Lost form field on document save, something is wrong";
                            item->formWidgets().remove(w);
                            delete w;
                        }
                    }

                    // For the video widgets we don't really care about reusing them since they don't contain much info so just
                    // create them again
                    createAnnotationsVideoWidgets(item, pageSet[i]->annotations());
                    const QHash<const Okular::Movie *, VideoWidget *> videoWidgets = item->videoWidgets();
                    for (VideoWidget *vw : videoWidgets) {
                        const Okular::NormalizedRect r = vw->normGeometry();
                        vw->setGeometry(qRound(item->uncroppedGeometry().left() + item->uncroppedWidth() * r.left) + 1 - viewportRect.left(),
                                        qRound(item->uncroppedGeometry().top() + item->uncroppedHeight() * r.top) + 1 - viewportRect.top(),
                                        qRound(fabs(r.right - r.left) * item->uncroppedGeometry().width()),
                                        qRound(fabs(r.bottom - r.top) * item->uncroppedGeometry().height()));

                        // Workaround, otherwise the size somehow gets lost
                        vw->show();
                        vw->hide();
                    }
                }
            }

            return;
        }
    }

    if (documentChanged || pageSet.isEmpty()) {
        setReviewSelection(nullptr, -1);
    }

    // mouseAnnotation must not access our PageViewItem widgets any longer
    d->mouseAnnotation->reset();

    // delete all widgets (one for each page in pageSet)
    qDeleteAll(d->items);
    d->items.clear();
    d->visibleItems.clear();
    d->pagesWithTextSelection.clear();
    toggleFormWidgets(false);
    if (d->formsWidgetController) {
        d->formsWidgetController->dropRadioButtons();
    }

    bool haspages = !pageSet.isEmpty();
    bool hasformwidgets = false;
    // create children widgets
    for (const Okular::Page *page : pageSet) {
        PageViewItem *item = new PageViewItem(page);
        d->items.push_back(item);
#ifdef PAGEVIEW_DEBUG
        qCDebug(OkularUiDebug).nospace() << "cropped geom for " << d->items.last()->pageNumber() << " is " << d->items.last()->croppedGeometry();
#endif
        const QList<Okular::FormField *> pageFields = page->formFields();
        for (Okular::FormField *ff : pageFields) {
            FormWidgetIface *w = FormWidgetFactory::createWidget(ff, this);
            if (w) {
                w->setPageItem(item);
                w->setFormWidgetsController(d->formWidgetsController());
                w->setVisibility(false);
                w->setCanBeFilled(allowfillforms);
                item->formWidgets().insert(w);
                hasformwidgets = true;
            }
        }

        createAnnotationsVideoWidgets(item, page->annotations());
    }

    // invalidate layout so relayout/repaint will happen on next viewport change
    if (haspages) {
        // We do a delayed call to slotRelayoutPages but also set the dirtyLayout
        // because we might end up in notifyViewportChanged while slotRelayoutPages
        // has not been done and we don't want that to happen
        d->dirtyLayout = true;
        QMetaObject::invokeMethod(this, "slotRelayoutPages", Qt::QueuedConnection);
    } else {
        // update the mouse cursor when closing because we may have close through a link and
        // want the cursor to come back to the normal cursor
        updateCursor();
        // then, make the message window and scrollbars disappear, and trigger a repaint
        d->messageWindow->hide();
        resizeContentArea(QSize(0, 0));
        viewport()->update(); // when there is no change to the scrollbars, no repaint would
                              // be done and the old document would still be shown
    }

    // OSD (Message balloons) to display pages
    // A backing-file refresh may discover that some internal page references
    // need rebuilding, but it is still the same document from the user's
    // perspective. Only announce "Loaded…" for an actual document-open
    // lifecycle, never for UrlChanged-only save swaps.
    if ((setupFlags & Okular::DocumentObserver::DocumentChanged) && pageSet.count() > 0) {
        d->messageWindow->display(i18np(" Loaded a one-page document.", " Loaded a %1-page document.", pageSet.count()), QString(), PageViewMessage::Info, 4000);
    }

    updateActionState(haspages, hasformwidgets);

    // We need to assign it to a different list otherwise slotAnnotationWindowDestroyed
    // will bite us and clear d->m_annowindows
    QSet<AnnotWindow *> annowindows = d->m_annowindows;
    d->m_annowindows.clear();
    qDeleteAll(annowindows);

    if (setupFlags & (Okular::DocumentObserver::DocumentChanged | Okular::DocumentObserver::UrlChanged)) {
        hideAgentProposalHoverBar();
        d->suppressedAgentProposalIds.clear();
        if (d->agentProposalWindow) {
            d->agentProposalWindow->close();
        }
    }
    reloadAgentProposalOverlays(!(setupFlags & (Okular::DocumentObserver::DocumentChanged | Okular::DocumentObserver::UrlChanged)));
    selectionClear();
}

void PageView::updateActionState(bool haspages, bool /*hasformwidgets*/)
{
    if (d->aTrimMargins) {
        d->aTrimMargins->setEnabled(haspages);
    }

    if (d->aTrimToSelection) {
        d->aTrimToSelection->setEnabled(haspages);
    }

    if (d->aViewModeMenu) {
        d->aViewModeMenu->setEnabled(haspages);
    }

    if (d->aViewContinuous) {
        d->aViewContinuous->setEnabled(haspages);
    }

    updateZoomActionsEnabledStatus();

    if (d->aColorModeMenu) {
        d->aColorModeMenu->setEnabled(haspages);
    }

    if (d->aReadingDirection) {
        d->aReadingDirection->setEnabled(haspages);
    }

    if (d->mouseModeActionGroup) {
        d->mouseModeActionGroup->setEnabled(haspages);
    }
    if (d->aMouseModeMenu) {
        d->aMouseModeMenu->setEnabled(haspages);
    }

    if (d->aRotateClockwise) {
        d->aRotateClockwise->setEnabled(haspages);
    }
    if (d->aRotateCounterClockwise) {
        d->aRotateCounterClockwise->setEnabled(haspages);
    }
    if (d->aRotateOriginal) {
        d->aRotateOriginal->setEnabled(haspages);
    }
    if (d->aToggleForms) { // may be null if dummy mode is on
        d->aToggleForms->setChecked(false);
        d->aToggleForms->setEnabled(false);
        d->aToggleForms->setVisible(false);
        toggleFormWidgets(false);
    }
    if (d->annotator) {
        d->annotator->setToolsEnabled(false);
        d->annotator->setTextToolsEnabled(false);
    }

    if (d->aSignature) {
        d->aSignature->setEnabled(false);
        d->aSignature->setVisible(false);
    }

    if (d->aReadAloud) {
        d->aReadAloud->setEnabled(haspages);
    }
#if HAVE_SPEECH
    if (d->aSpeakDoc) {
        const bool enablettsactions = haspages ? Okular::Settings::useTTS() : false;
        d->aSpeakDoc->setEnabled(enablettsactions);
        d->aSpeakDocFromPage->setEnabled(enablettsactions);
        d->aSpeakPage->setEnabled(enablettsactions);
    }
#endif
    if (d->aMouseMagnifier) {
        d->aMouseMagnifier->setEnabled(d->document->supportsTiles());
    }
    if (d->aFitWindowToPage) {
        d->aFitWindowToPage->setEnabled(haspages && !getContinuousMode());
    }
    d->hasPages = haspages;
    // An HTML page or edition shown instead keeps the PDF's tools greyed out.
    if (d->shownInstead != ShownInstead::Nothing) applyShownInsteadToTools();
}

void PageView::setShownInstead(ShownInstead shown)
{
    if (d->shownInstead == shown) return;
    d->shownInstead = shown;
    applyShownInsteadToTools();
}

PageView::ShownInstead PageView::shownInstead() const
{
    return d->shownInstead;
}

void PageView::setHtmlPagePencil(std::function<void()> drawOnPage)
{
    d->htmlPagePencil = std::move(drawOnPage);
}

void PageView::applyShownInsteadToTools()
{
    // Before setupActions there are no tools of the reader to grey out.
    if (!d->aMouseDraw) return;
    const bool pdf = d->shownInstead == ShownInstead::Nothing;
    const QString elsewhere = i18nc("@info:tooltip a tool that does nothing on an HTML page", "Works on PDF pages");
    // Each tool as updateActionState leaves it on the PDF, with its own tip there.
    const struct {
        QAction *action;
        QString tip;
        bool enabled;
    } tools[] = {
        {d->aMouseNormal, QString(), d->hasPages},
        {d->aMouseZoom, QString(), d->hasPages},
        {d->aMouseSelect, i18n("Select an area"), d->hasPages},
        {d->aMouseTextSelect, i18n("Select and highlight text"), d->hasPages},
        {d->aMouseTableSelect, QString(), d->hasPages},
        {d->aMouseMagnifier, QString(), d->document->supportsTiles()},
        {d->aMouseTextBox, i18n("Text box"), d->hasPages},
    };
    for (const auto &tool : tools) {
        if (!tool.action) continue;
        tool.action->setEnabled(pdf && tool.enabled);
        tool.action->setToolTip(pdf ? tool.tip : elsewhere);
    }
    if (d->aMouseModeMenu) d->aMouseModeMenu->setEnabled(pdf && d->hasPages);
    // The pencil draws on a snapshot of an HTML attachment page; the HTML edition keeps no drawings.
    d->aMouseDraw->setEnabled(d->hasPages && d->shownInstead != ShownInstead::HtmlEdition);
    d->aMouseDraw->setToolTip(d->shownInstead == ShownInstead::HtmlEdition ? elsewhere
                                  : d->shownInstead == ShownInstead::HtmlPage ? i18n("Draw on This Page")
                                  : Okular::Settings::pencilEraser()          ? i18nc("@info:tooltip the pencil's button while it erases", "Eraser")
                                                                              : i18n("Pencil"));
}

QAction *PageView::mouseModeAction() const
{
    switch (d->mouseMode) {
    case Okular::Settings::EnumMouseMode::Browse:
        return d->aMouseNormal;
    case Okular::Settings::EnumMouseMode::Zoom:
        return d->aMouseZoom;
    case Okular::Settings::EnumMouseMode::RectSelect:
        return d->aMouseSelect;
    case Okular::Settings::EnumMouseMode::TableSelect:
        return d->aMouseTableSelect;
    case Okular::Settings::EnumMouseMode::Magnifier:
        return d->aMouseMagnifier;
    case Okular::Settings::EnumMouseMode::TextSelect:
        return d->aMouseTextSelect;
    case Okular::Settings::EnumMouseMode::Draw:
        return d->aMouseDraw;
    case Okular::Settings::EnumMouseMode::TextBox:
        return d->aMouseTextBox;
    default:
        return nullptr;
    }
}

void PageView::setupActionsPostGUIActivated()
{
    d->annotator->setupActionsPostGUIActivated();
}

bool PageView::areSourceLocationsShownGraphically() const
{
    return Okular::Settings::showSourceLocationsGraphically();
}

void PageView::setShowSourceLocationsGraphically(bool show)
{
    if (show == Okular::Settings::showSourceLocationsGraphically()) {
        return;
    }
    Okular::Settings::setShowSourceLocationsGraphically(show);
    viewport()->update();
}

void PageView::setLastSourceLocationViewport(const Okular::DocumentViewport &vp)
{
    if (vp.rePos.enabled) {
        d->lastSourceLocationViewportNormalizedX = normClamp(vp.rePos.normalizedX, 0.5);
        d->lastSourceLocationViewportNormalizedY = normClamp(vp.rePos.normalizedY, 0.0);
    } else {
        d->lastSourceLocationViewportNormalizedX = 0.5;
        d->lastSourceLocationViewportNormalizedY = 0.0;
    }
    d->lastSourceLocationViewportPageNumber = vp.pageNumber;
    viewport()->update();
}

void PageView::clearLastSourceLocationViewport()
{
    d->lastSourceLocationViewportPageNumber = -1;
    d->lastSourceLocationViewportNormalizedX = 0.0;
    d->lastSourceLocationViewportNormalizedY = 0.0;
    viewport()->update();
}

void PageView::notifyViewportChanged(bool smoothMove)
{
    QMetaObject::invokeMethod(this, "slotRealNotifyViewportChanged", Qt::QueuedConnection, Q_ARG(bool, smoothMove));
}

void PageView::slotRealNotifyViewportChanged(bool smoothMove)
{
    // if we are the one changing viewport, skip this notify
    if (d->blockViewport) {
        return;
    }

    // block setViewport outgoing calls
    d->blockViewport = true;

    // find PageViewItem matching the viewport description
    const Okular::DocumentViewport &vp = d->document->viewport();
    const PageViewItem *item = nullptr;
    for (const PageViewItem *tmpItem : std::as_const(d->items)) {
        if (tmpItem->pageNumber() == vp.pageNumber) {
            item = tmpItem;
            break;
        }
    }
    if (!item) {
        qCWarning(OkularUiDebug) << "viewport for page" << vp.pageNumber << "has no matching item!";
        d->blockViewport = false;
        return;
    }
#ifdef PAGEVIEW_DEBUG
    qCDebug(OkularUiDebug) << "document viewport changed";
#endif
    // relayout in "Single Pages" mode or if a relayout is pending
    d->blockPixmapsRequest = true;
    if (!getContinuousMode() || d->dirtyLayout) {
        slotRelayoutPages();
    }

    // restore viewport center or use default {x-center,v-top} alignment
    const QPoint centerCoord = viewportToContentArea(vp);

    // if smooth movement requested, setup parameters and start it
    center(centerCoord.x(), centerCoord.y(), smoothMove);

    d->blockPixmapsRequest = false;

    // request visible pixmaps in the current viewport and recompute it
    slotRequestVisiblePixmaps();

    // enable setViewport calls
    d->blockViewport = false;

    if (viewport()) {
        viewport()->update();
    }

    // A page change takes the proposal toolbar away with its marker.
    if (d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible() && d->agentProposalHoverPage != static_cast<int>(d->document->currentPage())) {
        hideAgentProposalHoverBar();
    }
    checkAgentProposalHoverAnchor();

    // since the page has moved below cursor, update it
    updateCursor();
}

void PageView::notifyPageChanged(int pageNumber, int changedFlags)
{
    // only handle pixmap / highlight changes notifies
    if (changedFlags & DocumentObserver::Bookmark) {
        return;
    }

    if (changedFlags & DocumentObserver::Annotations) {
        collapseManagedCommentReplyAnnotations(d->document, d->docsRoot);
        const QList<Okular::Annotation *> annots = d->document->page(pageNumber)->annotations();
        if (d->m_reviewSelectionPageNumber == pageNumber && !annots.contains(d->m_reviewSelectionAnnotation)) {
            setReviewSelection(nullptr, -1);
        }
        const QList<Okular::Annotation *>::ConstIterator annItEnd = annots.end();
        QSet<AnnotWindow *>::Iterator it = d->m_annowindows.begin();
        for (; it != d->m_annowindows.end();) {
            // A page notification only describes annotations on that page.
            // Background updates elsewhere must not dismiss an active editor.
            if ((*it)->pageNumber() != pageNumber) {
                ++it;
                continue;
            }
            QList<Okular::Annotation *>::ConstIterator annIt = std::find(annots.begin(), annots.end(), (*it)->annotation());
            if (annIt != annItEnd) {
                (*it)->reloadInfo();
                ++it;
            } else {
                AnnotWindow *w = *it;
                it = d->m_annowindows.erase(it);
                // Need to delete after removing from the list
                // otherwise deleting will call slotAnnotationWindowDestroyed which will mess
                // the list and the iterators
                delete w;
            }
        }

        d->mouseAnnotation->notifyAnnotationChanged(pageNumber);
    }

    if (changedFlags & DocumentObserver::BoundingBox) {
#ifdef PAGEVIEW_DEBUG
        qCDebug(OkularUiDebug) << "BoundingBox change on page" << pageNumber;
#endif
        slotRelayoutPages();
        slotRequestVisiblePixmaps(); // TODO: slotRelayoutPages() may have done this already!
        // Repaint the whole widget since layout may have changed
        viewport()->update();
        return;
    }

    // iterate over visible items: if page(pageNumber) is one of them, repaint it
    for (const PageViewItem *visibleItem : std::as_const(d->visibleItems)) {
        if (visibleItem->pageNumber() == pageNumber && visibleItem->isVisible()) {
            // update item's rectangle plus the little outline
            QRect expandedRect = visibleItem->croppedGeometry();
            // a PageViewItem is placed in the global page layout,
            // while we need to map its position in the viewport coordinates
            // (to get the correct area to repaint)
            expandedRect.translate(-contentAreaPosition());
            expandedRect.adjust(-1, -1, 3, 3);
            viewport()->update(expandedRect);

            // if we were "zoom-dragging" do not overwrite the "zoom-drag" cursor
            if (cursor().shape() != Qt::SizeVerCursor) {
                // since the page has been regenerated below cursor, update it
                updateCursor();
            }
            break;
        }
    }
}

void PageView::notifyContentsCleared(int changedFlags)
{
    // if pixmaps were cleared, re-ask them
    if (changedFlags & DocumentObserver::Pixmap) {
        QMetaObject::invokeMethod(this, "slotRequestVisiblePixmaps", Qt::QueuedConnection);
    }
}

void PageView::notifyZoom(int factor)
{
    if (factor > 0) {
        updateZoom(ZoomIn);
    } else {
        updateZoom(ZoomOut);
    }
}

bool PageView::canUnloadPixmap(int pageNumber) const
{
    if (Okular::SettingsCore::memoryLevel() == Okular::SettingsCore::EnumMemoryLevel::Low || Okular::SettingsCore::memoryLevel() == Okular::SettingsCore::EnumMemoryLevel::Normal) {
        // if the item is visible, forbid unloading
        for (const PageViewItem *visibleItem : std::as_const(d->visibleItems)) {
            if (visibleItem->pageNumber() == pageNumber) {
                return false;
            }
        }
    } else {
        // forbid unloading of the visible items, and of the previous and next
        for (const PageViewItem *visibleItem : std::as_const(d->visibleItems)) {
            if (abs(visibleItem->pageNumber() - pageNumber) <= 1) {
                return false;
            }
        }
    }
    // if hidden premit unloading
    return true;
}

void PageView::notifyCurrentPageChanged(int previous, int current)
{
    if (previous != -1) {
        PageViewItem *item = d->items.at(previous);
        if (item) {
            const QHash<const Okular::Movie *, VideoWidget *> videoWidgetsList = item->videoWidgets();
            for (VideoWidget *videoWidget : videoWidgetsList) {
                videoWidget->pageLeft();
            }
        }

        // On close, run the widget scripts, needed for running animated PDF
        const Okular::Page *page = d->document->page(previous);
        const QList<Okular::Annotation *> annotations = page->annotations();
        for (Okular::Annotation *annotation : annotations) {
            if (annotation->subType() == Okular::Annotation::AWidget) {
                Okular::WidgetAnnotation *widgetAnnotation = static_cast<Okular::WidgetAnnotation *>(annotation);
                d->document->processAction(widgetAnnotation->additionalAction(Okular::Annotation::PageClosing));
            }
        }
    }

    if (current != -1) {
        PageViewItem *item = d->items.at(current);
        if (item) {
            const QHash<const Okular::Movie *, VideoWidget *> videoWidgetsList = item->videoWidgets();
            for (VideoWidget *videoWidget : videoWidgetsList) {
                videoWidget->pageEntered();
            }
        }

        // update zoom text and factor if in a ZoomFit/* zoom mode
        if (d->zoomMode != ZoomFixed) {
            updateZoomText();
        }

        // Opening any widget scripts, needed for running animated PDF
        const Okular::Page *page = d->document->page(current);
        const QList<Okular::Annotation *> annotations = page->annotations();
        for (Okular::Annotation *annotation : annotations) {
            if (annotation->subType() == Okular::Annotation::AWidget) {
                Okular::WidgetAnnotation *widgetAnnotation = static_cast<Okular::WidgetAnnotation *>(annotation);
                d->document->processAction(widgetAnnotation->additionalAction(Okular::Annotation::PageOpening));
            }
        }
    }

    // if the view is paged (or not continuous) and there is a selected annotation,
    // we call reset to avoid creating an artifact in the next page.
    if (!getContinuousMode()) {
        if (d->mouseAnnotation && d->mouseAnnotation->isFocused()) {
            d->mouseAnnotation->reset();
        }
    }
}

// END DocumentObserver inherited methods

// BEGIN View inherited methods
bool PageView::supportsCapability(ViewCapability capability) const
{
    switch (capability) {
    case Zoom:
    case ZoomModality:
    case Continuous:
    case ViewModeModality:
    case TrimMargins:
        return true;
    }
    return false;
}

Okular::View::CapabilityFlags PageView::capabilityFlags(ViewCapability capability) const
{
    switch (capability) {
    case Zoom:
    case ZoomModality:
    case Continuous:
    case ViewModeModality:
    case TrimMargins:
        return CapabilityRead | CapabilityWrite | CapabilitySerializable;
    }
    return NoFlag;
}

QVariant PageView::capability(ViewCapability capability) const
{
    switch (capability) {
    case Zoom:
        return d->zoomFactor;
    case ZoomModality:
        return d->zoomMode;
    case Continuous:
        return getContinuousMode();
    case ViewModeModality: {
        if (d->viewModeActionGroup) {
            const QList<QAction *> actions = d->viewModeActionGroup->actions();
            for (const QAction *action : actions) {
                if (action->isChecked()) {
                    return action->data();
                }
            }
        }
        return QVariant();
    }
    case TrimMargins:
        return d->aTrimMargins ? d->aTrimMargins->isChecked() : false;
    }
    return QVariant();
}

void PageView::setCapability(ViewCapability capability, const QVariant &option)
{
    switch (capability) {
    case Zoom: {
        bool ok = true;
        double factor = option.toDouble(&ok);
        if (ok && factor > 0.0) {
            d->zoomFactor = static_cast<float>(factor);
            updateZoom(ZoomRefreshCurrent);
        }
        break;
    }
    case ZoomModality: {
        bool ok = true;
        int mode = option.toInt(&ok);
        if (ok) {
            if (mode >= 0 && mode < 3) {
                updateZoom((ZoomMode)mode);
            }
        }
        break;
    }
    case ViewModeModality: {
        bool ok = true;
        int mode = option.toInt(&ok);
        if (ok) {
            if (mode >= 0 && mode < Okular::Settings::EnumViewMode::COUNT) {
                updateViewMode(mode);
            }
        }
        break;
    }
    case Continuous: {
        bool mode = option.toBool();
        d->aViewContinuous->setChecked(mode);
        break;
    }
    case TrimMargins: {
        bool value = option.toBool();
        d->aTrimMargins->setChecked(value);
        slotTrimMarginsToggled(value);
        break;
    }
    }
}

// END View inherited methods

// BEGIN widget events
bool PageView::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture) {
        return gestureEvent(static_cast<QGestureEvent *>(event));
    } else if (event->type() == QEvent::NativeGesture) {
        return nativeGestureEvent(static_cast<QNativeGestureEvent *>(event));
    } else if (event->type() == QEvent::ShortcutOverride && d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Undo) || (key->key() == Qt::Key_Z && key->modifiers() == Qt::MetaModifier) || key->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
    } else if (event->type() == QEvent::ShortcutOverride && d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool selected = !d->documentFeedback->selectedTextBox().isEmpty();
        if (key->key() == Qt::Key_Escape
            || (selected && (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter))) {
            event->accept();
            return true;
        }
    }

    // do not stop the event
    return QAbstractScrollArea::event(event);
}

bool PageView::gestureEvent(QGestureEvent *event)
{
    QPinchGesture *pinch = static_cast<QPinchGesture *>(event->gesture(Qt::PinchGesture));

    if (pinch) {
        // Viewport zoom level at the moment where the pinch gesture starts.
        // The viewport zoom level _during_ the gesture will be this value
        // times the relative zoom reported by QGestureEvent.
        static qreal vanillaZoom = d->zoomFactor;

        if (pinch->state() == Qt::GestureStarted) {
            vanillaZoom = d->zoomFactor;
            d->pinchZoomActive = true;
            d->scroller->handleInput(QScroller::InputRelease, QPointF());
            d->scroller->stop();
        }

        const QPinchGesture::ChangeFlags changeFlags = pinch->changeFlags();

        // Zoom
        if (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            zoomWithFixedCenter(ZoomRefreshCurrent, mapFromGlobal(pinch->centerPoint().toPoint()), vanillaZoom * pinch->totalScaleFactor());
        }

        // Count the number of 90-degree rotations we did since the start of the pinch gesture.
        // Otherwise a pinch turned to 90 degrees and held there will rotate the page again and again.
        static int rotations = 0;

        if (changeFlags & QPinchGesture::RotationAngleChanged) {
            // Rotation angle relative to the accumulated page rotations triggered by the current pinch
            // We actually turn at 80 degrees rather than at 90 degrees.  That's less strain on the hands.
            const qreal relativeAngle = pinch->rotationAngle() - rotations * 90;
            if (relativeAngle > 80) {
                slotRotateClockwise();
                rotations++;
            }
            if (relativeAngle < -80) {
                slotRotateCounterClockwise();
                rotations--;
            }
        }

        if (pinch->state() == Qt::GestureFinished || pinch->state() == Qt::GestureCanceled) {
            rotations = 0;
            d->pinchZoomActive = false;
            d->remainingScroll = QPointF(0.0, 0.0);
        }

        return true;
    }

    return false;
}

bool PageView::nativeGestureEvent(QNativeGestureEvent *event)
{
    if (event->gestureType() == Qt::BeginNativeGesture) {
        d->pinchZoomActive = true;
        d->scroller->handleInput(QScroller::InputRelease, QPointF());
        d->scroller->stop();
        return true;
    }
    if (event->gestureType() == Qt::ZoomNativeGesture) {
        zoomWithFixedCenter(ZoomRefreshCurrent, mapFromGlobal(QCursor::pos()), d->zoomFactor * (1 + event->value()));
        return true;
    }
    if (event->gestureType() == Qt::EndNativeGesture) {
        d->pinchZoomActive = false;
        d->remainingScroll = QPointF(0.0, 0.0);
        return true;
    }
    return false;
}

void PageView::paintEvent(QPaintEvent *pe)
{
    const QPoint areaPos = contentAreaPosition();
    // create the rect into contents from the clipped screen rect
    QRect viewportRect = viewport()->rect();
    viewportRect.translate(areaPos);
    QRect contentsRect = pe->rect().translated(areaPos).intersected(viewportRect);
    if (!contentsRect.isValid()) {
        return;
    }

#ifdef PAGEVIEW_DEBUG
    qCDebug(OkularUiDebug) << "paintevent" << contentsRect;
#endif

    // create the screen painter. a pixel painted at contentsX,contentsY
    // appears to the top-left corner of the scrollview.
    QPainter screenPainter(viewport());
    // translate to simulate the scrolled content widget
    screenPainter.translate(-areaPos);

    // selectionRect is the normalized mouse selection rect
    QRect selectionRect = d->mouseSelectionRect;
    if (!selectionRect.isNull()) {
        selectionRect = selectionRect.normalized();
    }
    // selectionRectInternal without the border
    QRect selectionRectInternal = selectionRect;
    selectionRectInternal.adjust(1, 1, -1, -1);
    // color for blending
    QColor selBlendColor = (selectionRect.width() > 8 || selectionRect.height() > 8) ? d->mouseSelectionColor : Qt::red;

    // subdivide region into rects
    QRegion rgn = pe->region();
    // preprocess rects area to see if it worths or not using subdivision
    uint summedArea = 0;
    for (const QRect &r : rgn) {
        summedArea += r.width() * r.height();
    }
    // very elementary check: SUMj(Region[j].area) is less than boundingRect.area
    const bool useSubdivision = summedArea < (0.6 * contentsRect.width() * contentsRect.height());
    if (!useSubdivision) {
        rgn = contentsRect;
    }

    // iterate over the rects (only one loop if not using subdivision)
    for (const QRect &r : rgn) {
        if (useSubdivision) {
            // set 'contentsRect' to a part of the sub-divided region
            contentsRect = r.translated(areaPos).intersected(viewportRect);
            if (!contentsRect.isValid()) {
                continue;
            }
        }
#ifdef PAGEVIEW_DEBUG
        qCDebug(OkularUiDebug) << contentsRect;
#endif

        // note: this check will take care of all things requiring alpha blending (not only selection)
        bool wantCompositing = !selectionRect.isNull() && contentsRect.intersects(selectionRect);
        // also alpha-blend when there is a table selection...
        wantCompositing |= !d->tableSelectionParts.isEmpty();

        if (wantCompositing && Okular::Settings::enableCompositing()) {
            // create pixmap and open a painter over it (contents{left,top} becomes pixmap {0,0})
            QPixmap doubleBuffer(contentsRect.size() * devicePixelRatioF());
            doubleBuffer.setDevicePixelRatio(devicePixelRatioF());
            QPainter pixmapPainter(&doubleBuffer);

            pixmapPainter.translate(-contentsRect.left(), -contentsRect.top());

            // 1) Layer 0: paint items and clear bg on unpainted rects
            drawDocumentOnPainter(contentsRect, &pixmapPainter);
            paintReadingOverlays(&pixmapPainter, contentsRect);
            paintLinkIndicators(&pixmapPainter, contentsRect);
            // 2a) Layer 1a: paint (blend) transparent selection (rectangle)
            if (!selectionRect.isNull() && selectionRect.intersects(contentsRect) && !selectionRectInternal.contains(contentsRect)) {
                QRect blendRect = selectionRectInternal.intersected(contentsRect);
                // skip rectangles covered by the selection's border
                if (blendRect.isValid()) {
                    // grab current pixmap into a new one to colorize contents
                    QPixmap blendedPixmap(blendRect.width() * devicePixelRatioF(), blendRect.height() * devicePixelRatioF());
                    blendedPixmap.setDevicePixelRatio(devicePixelRatioF());
                    QPainter p(&blendedPixmap);

                    p.drawPixmap(0,
                                 0,
                                 doubleBuffer,
                                 (blendRect.left() - contentsRect.left()) * devicePixelRatioF(),
                                 (blendRect.top() - contentsRect.top()) * devicePixelRatioF(),
                                 blendRect.width() * devicePixelRatioF(),
                                 blendRect.height() * devicePixelRatioF());

                    QColor blCol = selBlendColor.darker(140);
                    blCol.setAlphaF(0.2);
                    p.fillRect(blendedPixmap.rect(), blCol);
                    p.end();
                    // copy the blended pixmap back to its place
                    pixmapPainter.drawPixmap(blendRect.left(), blendRect.top(), blendedPixmap);
                }
                // draw border (red if the selection is too small)
                pixmapPainter.setPen(selBlendColor);
                pixmapPainter.drawRect(selectionRect.adjusted(0, 0, -1, -1));
            }
            // 2b) Layer 1b: paint (blend) transparent selection (table)
            for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
                QRect selectionPartRect = tsp.rectInItem.geometry(tsp.item->uncroppedWidth(), tsp.item->uncroppedHeight());
                selectionPartRect.translate(tsp.item->uncroppedGeometry().topLeft());
                QRect selectionPartRectInternal = selectionPartRect;
                selectionPartRectInternal.adjust(1, 1, -1, -1);
                if (!selectionPartRect.isNull() && selectionPartRect.intersects(contentsRect) && !selectionPartRectInternal.contains(contentsRect)) {
                    QRect blendRect = selectionPartRectInternal.intersected(contentsRect);
                    // skip rectangles covered by the selection's border
                    if (blendRect.isValid()) {
                        // grab current pixmap into a new one to colorize contents
                        QPixmap blendedPixmap(blendRect.width() * devicePixelRatioF(), blendRect.height() * devicePixelRatioF());
                        blendedPixmap.setDevicePixelRatio(devicePixelRatioF());
                        QPainter p(&blendedPixmap);
                        p.drawPixmap(0,
                                     0,
                                     doubleBuffer,
                                     (blendRect.left() - contentsRect.left()) * devicePixelRatioF(),
                                     (blendRect.top() - contentsRect.top()) * devicePixelRatioF(),
                                     blendRect.width() * devicePixelRatioF(),
                                     blendRect.height() * devicePixelRatioF());

                        QColor blCol = d->mouseSelectionColor.darker(140);
                        blCol.setAlphaF(0.2);
                        p.fillRect(blendedPixmap.rect(), blCol);
                        p.end();
                        // copy the blended pixmap back to its place
                        pixmapPainter.drawPixmap(blendRect.left(), blendRect.top(), blendedPixmap);
                    }
                    // draw border (red if the selection is too small)
                    pixmapPainter.setPen(d->mouseSelectionColor);
                    pixmapPainter.drawRect(selectionPartRect.adjusted(0, 0, -1, -1));
                }
            }
            drawTableDividers(&pixmapPainter);
            // 3a) Layer 1: give annotator painting control
            if (d->annotator && d->annotator->routePaints(contentsRect)) {
                d->annotator->routePaint(&pixmapPainter, contentsRect);
            }
            // 3b) Layer 1: give mouseAnnotation painting control
            d->mouseAnnotation->routePaint(&pixmapPainter, contentsRect);
            paintReviewSelection(&pixmapPainter, contentsRect);
            paintReviewMask(&pixmapPainter, contentsRect);
            paintAgentProposalOverlays(&pixmapPainter, contentsRect);
            paintCommentSourceConnectors(&pixmapPainter, contentsRect);

            // 4) Layer 2: overlays
            if (Okular::Settings::debugDrawBoundaries()) {
                pixmapPainter.setPen(Qt::blue);
                pixmapPainter.drawRect(contentsRect);
            }

            // finish painting and draw contents
            pixmapPainter.end();
            screenPainter.drawPixmap(contentsRect.left(), contentsRect.top(), doubleBuffer);
        } else {
            // 1) Layer 0: paint items and clear bg on unpainted rects
            drawDocumentOnPainter(contentsRect, &screenPainter);
            paintReadingOverlays(&screenPainter, contentsRect);
            paintLinkIndicators(&screenPainter, contentsRect);
            // 2a) Layer 1a: paint opaque selection (rectangle)
            if (!selectionRect.isNull() && selectionRect.intersects(contentsRect) && !selectionRectInternal.contains(contentsRect)) {
                screenPainter.setPen(palette().color(QPalette::Active, QPalette::Highlight).darker(110));
                screenPainter.drawRect(selectionRect);
            }
            // 2b) Layer 1b: paint opaque selection (table)
            for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
                QRect selectionPartRect = tsp.rectInItem.geometry(tsp.item->uncroppedWidth(), tsp.item->uncroppedHeight());
                selectionPartRect.translate(tsp.item->uncroppedGeometry().topLeft());
                QRect selectionPartRectInternal = selectionPartRect;
                selectionPartRectInternal.adjust(1, 1, -1, -1);
                if (!selectionPartRect.isNull() && selectionPartRect.intersects(contentsRect) && !selectionPartRectInternal.contains(contentsRect)) {
                    screenPainter.setPen(palette().color(QPalette::Active, QPalette::Highlight).darker(110));
                    screenPainter.drawRect(selectionPartRect);
                }
            }
            drawTableDividers(&screenPainter);
            // 3a) Layer 1: give annotator painting control
            if (d->annotator && d->annotator->routePaints(contentsRect)) {
                d->annotator->routePaint(&screenPainter, contentsRect);
            }
            // 3b) Layer 1: give mouseAnnotation painting control
            d->mouseAnnotation->routePaint(&screenPainter, contentsRect);
            paintReviewSelection(&screenPainter, contentsRect);
            paintReviewMask(&screenPainter, contentsRect);
            paintAgentProposalOverlays(&screenPainter, contentsRect);
            paintCommentSourceConnectors(&screenPainter, contentsRect);

            // 4) Layer 2: overlays
            if (Okular::Settings::debugDrawBoundaries()) {
                screenPainter.setPen(Qt::red);
                screenPainter.drawRect(contentsRect);
            }
        }
    }
}

void PageView::paintLinkIndicators(QPainter *painter, const QRect &contentsRect) const
{
    QColor linkColor = palette().color(QPalette::Active, QPalette::Link);
    if (!linkColor.isValid()) {
        linkColor = QColor(0, 102, 204);
    }

    for (const PageViewItem *pageItem : std::as_const(d->items)) {
        if (!pageItem->isVisible() || !pageItem->croppedGeometry().intersects(contentsRect)) {
            continue;
        }

        for (const Okular::ObjectRect *objectRect : pageItem->page()->objectRects()) {
            if (!objectRect || objectRect->objectType() != Okular::ObjectRect::Action) {
                continue;
            }

            QRect indicatorRect = objectRect->boundingRect(pageItem->uncroppedWidth(), pageItem->uncroppedHeight());
            indicatorRect.translate(pageItem->uncroppedGeometry().topLeft());
            indicatorRect = indicatorRect.intersected(pageItem->croppedGeometry());
            if (indicatorRect.isValid() && indicatorRect.intersects(contentsRect)) {
                paintPdfLinkIndicator(painter, indicatorRect, linkColor, objectRect == d->mouseOverLinkObject);
            }
        }
    }
}

QPainterPath PageView::reviewSelectionPath() const
{
    QPainterPath path;
    QMap<int, QList<QPolygonF>> pageQuads;
    if (!d->m_reviewSelectionAnnotation || d->m_reviewSelectionPageNumber < 0) {
        return path;
    }

    const Thoughts::CommentFragmentGroup group =
        Thoughts::commentFragmentGroup(d->document, d->m_reviewSelectionAnnotation, d->m_reviewSelectionPageNumber);
    for (const Thoughts::CommentFragment &fragment : group.fragments) {
        const PageViewItem *selectedItem = nullptr;
        for (const PageViewItem *item : std::as_const(d->items)) {
            if (item->pageNumber() == fragment.page) {
                selectedItem = item;
                break;
            }
        }
        if (!selectedItem || !selectedItem->isVisible() || !fragment.annotation) {
            continue;
        }

        QPainterPath fragmentPath;
        const QPoint pageOrigin = selectedItem->uncroppedGeometry().topLeft();
        if (fragment.annotation->subType() == Okular::Annotation::AHighlight) {
            const auto *highlight = static_cast<const Okular::HighlightAnnotation *>(fragment.annotation);
            for (const Okular::HighlightAnnotation::Quad &quad : highlight->highlightQuads()) {
                QPolygonF polygon;
                for (int pointIndex = 0; pointIndex < 4; ++pointIndex) {
                    const Okular::NormalizedPoint point = quad.transformedPoint(pointIndex);
                    polygon.append(QPointF(pageOrigin.x() + point.x * selectedItem->uncroppedWidth(), pageOrigin.y() + point.y * selectedItem->uncroppedHeight()));
                }
                pageQuads[fragment.page].append(polygon);
            }
        }

        if (fragment.annotation->subType() != Okular::Annotation::AHighlight
            || static_cast<const Okular::HighlightAnnotation *>(fragment.annotation)->highlightQuads().isEmpty()) {
            QRect annotationRect = Okular::AnnotationUtils::annotationGeometry(fragment.annotation, selectedItem->uncroppedWidth(), selectedItem->uncroppedHeight());
            annotationRect.translate(pageOrigin);
            QRectF outlineRect(annotationRect);
            outlineRect.adjust(-2.0, -2.0, 2.0, 2.0);
            if (outlineRect.isValid()) {
                fragmentPath.addRoundedRect(outlineRect, 3.0, 3.0);
            }
        }
        path.addPath(fragmentPath);
    }
    for (const auto &quads : pageQuads) path.addPath(Thoughts::textLinePath(quads));
    path.setFillRule(Qt::WindingFill);
    return path;
}

int PageView::reviewSelectionPageNumber() const
{
    return d->m_reviewSelectionPageNumber;
}

void PageView::paintReviewSelection(QPainter *painter, const QRect &contentsRect) const
{
    if (!Okular::Settings::highlightComments()) {
        return;
    }
    const QPainterPath path = reviewSelectionPath();
    if (path.isEmpty() || !path.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0).intersects(contentsRect)) {
        return;
    }

    QColor accent = palette().color(QPalette::Active, QPalette::Highlight);
    QColor fill = accent;
    fill.setAlphaF(0.18);

    QPen outline(accent, 2.0);
    outline.setCosmetic(true);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(outline);
    painter->setBrush(fill);
    painter->drawPath(path);
    painter->restore();
}

void PageView::setCommentSourceConnectors(const QJsonArray &connectors)
{
    d->m_commentSourceConnectors = connectors;
    viewport()->update();
}

void PageView::paintCommentSourceConnectors(QPainter *painter, const QRect &contentsRect) const
{
    if (!Okular::Settings::highlightComments()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(QColor(130, 120, 20), 1.0));
    painter->setBrush(QColor(130, 120, 20));
    for (const QJsonValue &value : d->m_commentSourceConnectors) {
        const QJsonObject connector = value.toObject();
        for (const PageViewItem *item : std::as_const(d->items)) {
            if (!item->isVisible() || item->pageNumber() != connector.value(QStringLiteral("page")).toInt()) continue;
            const QJsonArray source = connector.value(QStringLiteral("source")).toArray(), display = connector.value(QStringLiteral("display")).toArray();
            if (source.size() != 2 || display.size() != 2) continue;
            const QPoint origin = item->uncroppedGeometry().topLeft();
            const auto point = [&](const QJsonArray &p) { return QPointF(origin.x() + p[0].toDouble()*item->uncroppedWidth(), origin.y() + p[1].toDouble()*item->uncroppedHeight()); };
            const QPointF from = point(source), to = point(display);
            if (QRectF(from, to).normalized().adjusted(-3,-3,3,3).intersects(contentsRect)) {
                painter->drawLine(from, to);
                painter->drawEllipse(from, 2.0, 2.0);
            }
        }
    }
    painter->restore();
}

// As on the phone, the mask looks like blank paper: the colour pages are shown
// in, so it hides the answer without drawing attention to itself.
static QColor reviewMaskColor()
{
    if (!Okular::SettingsCore::changeColors()) return Qt::white;
    switch (Okular::SettingsCore::renderMode()) {
    case Okular::SettingsCore::EnumRenderMode::Paper:
        return Okular::SettingsCore::paperColor();
    case Okular::SettingsCore::EnumRenderMode::Recolor:
        return Okular::Settings::recolorBackground();
    case Okular::SettingsCore::EnumRenderMode::Inverted:
    case Okular::SettingsCore::EnumRenderMode::InvertLightness:
    case Okular::SettingsCore::EnumRenderMode::InvertLuma:
    case Okular::SettingsCore::EnumRenderMode::InvertLumaSymmetric:
        return Qt::black;
    default:
        return Qt::white;
    }
}

void PageView::paintReviewMask(QPainter *painter, const QRect &contentsRect) const
{
    const auto regionPath = [&](const QMap<int, QList<Okular::NormalizedRect>> &pages) {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        for (const PageViewItem *item : std::as_const(d->items)) {
            if (!item->isVisible()) continue;
            const auto regions = pages.constFind(item->pageNumber());
            if (regions == pages.cend()) continue;
            const QPoint origin = item->uncroppedGeometry().topLeft();
            for (const Okular::NormalizedRect &normalized : regions.value()) {
                QRectF rect(origin.x() + normalized.left * item->uncroppedWidth(),
                            origin.y() + normalized.top * item->uncroppedHeight(),
                            (normalized.right - normalized.left) * item->uncroppedWidth(),
                            (normalized.bottom - normalized.top) * item->uncroppedHeight());
                rect.adjust(-2.0, -1.5, 2.0, 1.5);
                if (rect.intersects(contentsRect)) path.addRect(rect);
            }
        }
        return path;
    };
    const QPainterPath selected = d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight
        ? regionPath(d->m_reviewMaskRects) : QPainterPath();
    const auto paintPath = [&](const QPainterPath &path, const QColor &accent, int alpha, bool excludeSelected) {
        if (path.isEmpty()) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (excludeSelected && !selected.isEmpty()) {
            // Do not stack the ordinary highlight underneath the selected color.
            QRegion clip(contentsRect);
            for (const QPolygonF &rect : selected.toSubpathPolygons()) {
                clip -= rect.boundingRect().adjusted(-1, -1, 1, 1).toAlignedRect();
            }
            painter->setClipRegion(clip, Qt::IntersectClip);
        }
        painter->setPen(QPen(accent, 1));
        painter->setBrush(QColor(accent.red(), accent.green(), accent.blue(), alpha));
        painter->drawPath(path);
        painter->restore();
    };
    if (Okular::Settings::highlightReviewCards()) {
        paintPath(regionPath(d->m_reviewCardHighlightRects), palette().color(QPalette::Highlight), 24, true);
    }
    // Sidebar selection stays visible and uses one color regardless of the global toggle.
    paintPath(selected, QColor(25, 95, 190), 64, false);
    // Answer concealment in a review session is independent of highlight preferences.
    if (d->m_reviewMaskStyle == ReviewMaskStyle::Hidden && !d->m_reviewMaskRects.isEmpty()) {
        QPainterPath mask = regionPath(d->m_reviewMaskRects);
        if (d->m_reviewMaskInverted) {
            // Flipped: the answer stays readable and its context is hidden.
            QPainterPath pages;
            for (const PageViewItem *item : std::as_const(d->items)) {
                if (item->isVisible() && item->uncroppedGeometry().intersects(contentsRect)) pages.addRect(item->uncroppedGeometry());
            }
            mask = pages.subtracted(mask);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(reviewMaskColor());
        painter->drawPath(mask);
        painter->restore();
    }
    // A revealed answer keeps the exact area that was hidden, framed so it is easy to find.
    if (d->m_reviewMaskStyle == ReviewMaskStyle::Revealed && !d->m_reviewMaskRects.isEmpty()) {
        QPainterPath frame;
        frame.setFillRule(Qt::WindingFill);
        for (const QPolygonF &block : regionPath(d->m_reviewMaskRects).toSubpathPolygons()) {
            frame.addRect(block.boundingRect().adjusted(-3.0, -3.0, 3.0, 3.0));
        }
        const QColor blue(25, 95, 190);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(blue, 2));
        painter->setBrush(QColor(blue.red(), blue.green(), blue.blue(), 18));
        painter->drawPath(frame.simplified());
        painter->restore();
    }
    // The card's answer feedback, only while its answer is visible.
    if (reviewFeedbackVisible()) {
        d->reviewFeedback->paint(painter, [this, &contentsRect](int page, const QRectF &rect) {
            const QRectF shown = reviewFeedbackRect(page, rect);
            return shown.intersects(contentsRect) ? shown : QRectF();
        }, palette().color(QPalette::Window).lightness() < 128);
    }
    // While the answer is masked, the user's own annotations show everywhere but over it.
    const QPainterPath hidden = hiddenAnswerArea();
    if (hidden.isEmpty()) {
        paintDocumentFeedback(painter, contentsRect);
    } else {
        QPainterPath open;
        open.addRect(contentsRect);
        painter->save();
        painter->setClipPath(open.subtracted(hidden), Qt::IntersectClip);
        paintDocumentFeedback(painter, contentsRect);
        painter->restore();
    }
}

QPainterPath PageView::hiddenAnswerArea() const
{
    // What a masked review answer's mask covers (the flipped mask: everything but the answer), in
    // content coordinates; empty when no answer is masked. Nothing of the user's shows there.
    if (!answerMasked()) return {};
    QPainterPath area;
    area.setFillRule(Qt::WindingFill);
    QPainterPath pages;
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible()) continue;
        const QRect geometry = item->uncroppedGeometry();
        pages.addRect(geometry);
        for (const Okular::NormalizedRect &normalized : d->m_reviewMaskRects.value(item->pageNumber())) {
            const QRectF rect(geometry.left() + normalized.left * item->uncroppedWidth(), geometry.top() + normalized.top * item->uncroppedHeight(),
                              (normalized.right - normalized.left) * item->uncroppedWidth(), (normalized.bottom - normalized.top) * item->uncroppedHeight());
            area.addRect(rect.adjusted(-2.0, -1.5, 2.0, 1.5));
        }
    }
    return d->m_reviewMaskInverted ? pages.subtracted(area) : area;
}

void PageView::paintDocumentFeedback(QPainter *painter, const QRect &contentsRect) const
{
    const auto map = [this, &contentsRect](int page, const QRectF &rect) {
        const QRectF shown = reviewFeedbackRect(page, rect);
        return shown.intersects(contentsRect) ? shown : QRectF();
    };
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    if (drawingsVisible() && !d->documentFeedback->records().isEmpty()) {
        QSet<int> pages;
        for (const PageViewItem *item : std::as_const(d->items)) {
            if (item->isVisible() && item->uncroppedGeometry().intersects(contentsRect)) pages.insert(item->pageNumber() + 1);
        }
        // The switches as they are now, also when a setting changed without being saved.
        d->documentFeedback->setShownKinds(Okular::Settings::highlightComments(), Okular::Settings::highlightFeedback());
        d->documentFeedback->paint(painter, pages, map, dark, false, d->mouseMode == Okular::Settings::EnumMouseMode::TextBox);
    }
    if (d->strokePage > 0) Thoughts::ReviewFeedbackOverlay::paintStrokes(painter, d->strokePage, {d->stroke}, map, dark);
    // The box being moved, where it is going.
    if (!d->textBoxMoved.isEmpty()) {
        const QJsonObject moving = d->documentFeedback->record(d->textBoxPressed);
        Thoughts::ReviewFeedbackOverlay::paintTextBox(painter, d->textBoxPage, d->textBoxMoved, Thoughts::DocumentFeedback::textBoxFontSize(moving),
                                                      moving.value(QStringLiteral("comment")).toString(), map, dark, 2);
    }
    // The box being typed in: its editor shows the text, the frame shows it is a box.
    if (d->textBoxEditor && d->textBoxEditor->isVisible()) {
        QColor accent = Thoughts::ReviewFeedbackOverlay::colour(Thoughts::ReviewCardFeedback::neutral(), dark);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(accent, 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(d->textBoxEditor->geometry().translated(contentAreaPosition())).adjusted(-3.0, -2.0, 3.0, 2.0), 3.0, 3.0);
        painter->restore();
    }
}

bool PageView::drawingsVisible() const
{
    // Also during a review: only what lies over a masked answer stays hidden (hiddenAnswerArea).
    return d->documentFeedback->hasDocument();
}

namespace
{
// How far from the pointer the eraser reaches, on screen at every zoom.
constexpr double eraserRadius = 8.0;

// The pencil's colour now, "#rrggbb", or empty for its own blue.
QString pencilColour()
{
    return Thoughts::DocumentFeedback::strokeColour(Okular::Settings::pencilColor());
}

// A ring as wide as the eraser reaches, dark with a light edge to show on any page.
QCursor eraserCursor()
{
    static const QCursor cursor = [] {
        const int side = int(std::ceil(eraserRadius * 2.0)) + 4;
        QPixmap pixmap(QSize(side, side) * 2);
        pixmap.setDevicePixelRatio(2.0);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        const QPointF centre(side / 2.0, side / 2.0);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 3.0));
        painter.drawEllipse(centre, eraserRadius, eraserRadius);
        painter.setPen(QPen(QColor(0, 0, 0, 200), 1.2));
        painter.drawEllipse(centre, eraserRadius, eraserRadius);
        painter.end();
        return QCursor(pixmap, side / 2, side / 2);
    }();
    return cursor;
}
}

void PageView::beginDrawingStroke(const QPoint &eventPos)
{
    const PageViewItem *item = pickItemOnPoint(eventPos.x(), eventPos.y());
    if (!item) return;
    if (!drawingsVisible()) {
        d->messageWindow->displayInMiddle(i18n("Drawings are kept with Library documents, and this PDF is not one."), PageViewMessage::Warning, 4000);
        return;
    }
    if (hiddenAnswerArea().contains(QPointF(eventPos))) {
        d->messageWindow->displayInMiddle(i18n("Reveal the answer to draw over it."), PageViewMessage::Warning, 4000);
        return;
    }
    d->strokePage = item->pageNumber() + 1;
    d->stroke = {{}, pencilColour()};
    extendDrawingStroke(eventPos);
}

void PageView::extendDrawingStroke(const QPoint &eventPos)
{
    const QRectF page = reviewFeedbackRect(d->strokePage, QRectF(0.0, 0.0, 1.0, 1.0));
    if (page.isEmpty()) return;
    // It stays on its page, however far the pointer goes.
    const QPointF point(std::clamp((eventPos.x() - page.left()) / page.width(), 0.0, 1.0), std::clamp((eventPos.y() - page.top()) / page.height(), 0.0, 1.0));
    QList<QPointF> &points = d->stroke.points;
    const QPointF last = points.isEmpty() ? point : points.constLast();
    if (!points.isEmpty()) {
        // Movement under a pixel and a half adds size to the record, not shape to the line.
        if (std::hypot((point.x() - last.x()) * page.width(), (point.y() - last.y()) * page.height()) < 1.5) return;
        if (points.size() >= Thoughts::DocumentFeedback::maximumStrokePoints) return;
    }
    points.append(point);
    const auto shown = [&page](const QPointF &at) { return QPointF(page.left() + at.x() * page.width(), page.top() + at.y() * page.height()); };
    viewport()->update(QRectF(shown(last), shown(point)).normalized().adjusted(-8.0, -8.0, 8.0, 8.0).toAlignedRect().translated(-contentAreaPosition()));
}

void PageView::finishDrawingStroke()
{
    const int page = std::exchange(d->strokePage, 0);
    const Thoughts::DocumentFeedback::Stroke stroke = std::exchange(d->stroke, {});
    if (page == 0 || stroke.points.isEmpty()) return;
    QJsonObject current = d->drawings.isEmpty() ? QJsonObject() : d->documentFeedback->record(d->drawings.constLast());
    QList<Thoughts::DocumentFeedback::Stroke> strokes = Thoughts::DocumentFeedback::strokesOf(current);
    const auto regions = Thoughts::ReviewCardFeedback::regionsOf(current);
    if (current.value(QStringLiteral("deleted")).toBool() || regions.isEmpty() || regions.constFirst().first != page || strokes.size() >= Thoughts::DocumentFeedback::maximumStrokes) {
        current = {};
        strokes.clear();
    }
    strokes.append(stroke);
    const QString origin = Thoughts::Platform::reviewOriginId();
    const QJsonObject record = Thoughts::FeedbackVersion::stamped(current.isEmpty()
        ? Thoughts::DocumentFeedback::createDrawing(page, strokes, d->documentFeedback->documentId(), d->documentFeedback->pdfSha256(), origin)
        : Thoughts::DocumentFeedback::withStrokes(current, strokes, origin), d->documentFeedback->reviewVersion());
    QString error;
    if (!d->documentFeedback->write(record, &error)) {
        d->messageWindow->display(i18n("The drawing could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
        viewport()->update();
        return;
    }
    if (current.isEmpty()) d->drawings.append(record.value(QStringLiteral("feedback_id")).toString());
    rememberAnnotation(record.value(QStringLiteral("feedback_id")).toString(), true);
}

void PageView::beginErasing(const QPoint &eventPos)
{
    d->erasing = true;
    d->erased.clear();
    d->eraserAt = eventPos;
    eraseAlong(eventPos);
}

void PageView::eraseAlong(const QPoint &eventPos)
{
    if (!d->erasing) return;
    const QPointF from = std::exchange(d->eraserAt, QPointF(eventPos));
    const QPointF to = eventPos;
    // Never what a masked answer hides.
    if (!drawingsVisible() || hiddenAnswerArea().contains(to)) return;
    const auto map = [this](int page, const QRectF &rect) { return reviewFeedbackRect(page, rect); };
    QSet<int> pages;
    for (const QPointF &at : {from, to}) {
        if (const PageViewItem *item = pickItemOnPoint(qRound(at.x()), qRound(at.y()))) pages.insert(item->pageNumber() + 1);
    }
    const QString origin = Thoughts::Platform::reviewOriginId();
    QList<QJsonObject> records;
    QHash<QString, QJsonObject> before;
    for (const int page : std::as_const(pages)) {
        const QHash<QString, QList<int>> touched = d->documentFeedback->strokesNear(page, from, to, eraserRadius, map);
        for (auto it = touched.cbegin(); it != touched.cend(); ++it) {
            const QJsonObject current = d->documentFeedback->record(it.key());
            QList<Thoughts::DocumentFeedback::Stroke> strokes = Thoughts::DocumentFeedback::strokesOf(current);
            for (auto index = it->crbegin(); index != it->crend(); ++index) strokes.removeAt(*index);
            // Its last stroke takes the drawing with it.
            records.append(strokes.isEmpty() ? Thoughts::DocumentFeedback::remove(current, origin)
                                             : Thoughts::FeedbackVersion::stamped(Thoughts::DocumentFeedback::withStrokes(current, strokes, origin),
                                                                                  d->documentFeedback->reviewVersion()));
            before.insert(it.key(), current);
        }
    }
    if (records.isEmpty()) return;
    QString error;
    if (!d->documentFeedback->writeAll(records, &error)) {
        d->messageWindow->display(i18n("The drawing could not be changed: %1", error), QString(), PageViewMessage::Error, 5000);
        viewport()->update();
        return;
    }
    for (const QJsonObject &record : std::as_const(records)) {
        const QString id = record.value(QStringLiteral("feedback_id")).toString();
        // As it was before the drag began.
        if (!d->erased.contains(id)) d->erased.insert(id, {before.value(id), 0});
        d->erased[id].revision = record.value(QStringLiteral("revision")).toInt();
        if (record.value(QStringLiteral("deleted")).toBool()) d->drawings.removeAll(id);
    }
    viewport()->update();
}

void PageView::finishErasing()
{
    QHash<QString, PageViewPrivate::Erased> erased = std::exchange(d->erased, {});
    if (!std::exchange(d->erasing, false) || erased.isEmpty()) return;
    const QString first = erased.cbegin().key();
    d->annotationSteps.append({first, false, d->document->undoHistoryIndex(), QDateTime::currentMSecsSinceEpoch(), std::move(erased)});
    if (d->annotationSteps.size() > 500) d->annotationSteps.removeFirst();
    Q_EMIT annotationUndoChanged();
}

void PageView::rememberAnnotation(const QString &feedbackId, bool stroke)
{
    if (feedbackId.isEmpty()) return;
    d->annotationSteps.append({feedbackId, stroke, d->document->undoHistoryIndex(), QDateTime::currentMSecsSinceEpoch(), {}});
    // A long session keeps the last few hundred.
    if (d->annotationSteps.size() > 500) d->annotationSteps.removeFirst();
    Q_EMIT annotationUndoChanged();
}

int PageView::newestAnnotationStep() const
{
    for (qsizetype index = d->annotationSteps.size() - 1; index >= 0; --index) {
        const auto &step = d->annotationSteps.at(index);
        if (!step.erased.isEmpty()) {
            // While a drawing is still as the eraser left it.
            for (auto it = step.erased.cbegin(); it != step.erased.cend(); ++it) {
                if (d->documentFeedback->record(it.key()).value(QStringLiteral("revision")).toInt() == it->revision) return int(index);
            }
            continue;
        }
        const QJsonObject record = d->documentFeedback->record(step.feedbackId);
        // Gone some other way (deleted, or the document changed): nothing to take back.
        if (record.isEmpty() || record.value(QStringLiteral("deleted")).toBool()) continue;
        if (step.stroke && Thoughts::DocumentFeedback::strokesOf(record).isEmpty()) continue;
        return int(index);
    }
    return -1;
}

bool PageView::canUndoAnnotation() const
{
    const int index = newestAnnotationStep();
    return index >= 0 && d->annotationSteps.at(index).documentHistory == d->document->undoHistoryIndex();
}

qint64 PageView::lastAnnotationAt() const
{
    const int index = newestAnnotationStep();
    return index < 0 ? 0 : d->annotationSteps.at(index).at;
}

bool PageView::undoLastAnnotation()
{
    if (!canUndoAnnotation()) return false;
    const int index = newestAnnotationStep();
    const auto step = d->annotationSteps.at(index);
    d->annotationSteps.erase(d->annotationSteps.begin() + index, d->annotationSteps.end());
    const QString origin = Thoughts::Platform::reviewOriginId();
    if (!step.erased.isEmpty()) {
        // The strokes an eraser drag removed come back, also a drawing it deleted (an edit undeletes).
        QList<QJsonObject> restored;
        for (auto it = step.erased.cbegin(); it != step.erased.cend(); ++it) {
            const QJsonObject current = d->documentFeedback->record(it.key());
            if (current.value(QStringLiteral("revision")).toInt() != it->revision) continue;
            restored.append(Thoughts::FeedbackVersion::stamped(
                Thoughts::DocumentFeedback::withStrokes(current, Thoughts::DocumentFeedback::strokesOf(it->before), origin), d->documentFeedback->reviewVersion()));
        }
        QString error;
        if (!d->documentFeedback->writeAll(restored, &error)) {
            d->messageWindow->display(i18n("It could not be taken back: %1", error), QString(), PageViewMessage::Error, 5000);
            Q_EMIT annotationUndoChanged();
            return false;
        }
        viewport()->update();
        Q_EMIT annotationUndoChanged();
        return true;
    }
    const QJsonObject current = d->documentFeedback->record(step.feedbackId);
    QList<Thoughts::DocumentFeedback::Stroke> strokes = Thoughts::DocumentFeedback::strokesOf(current);
    if (step.stroke && !strokes.isEmpty()) strokes.removeLast();
    const bool whole = !step.stroke || strokes.isEmpty();
    QString error;
    if (!d->documentFeedback->write(whole ? Thoughts::DocumentFeedback::remove(current, origin)
                                          : Thoughts::FeedbackVersion::stamped(Thoughts::DocumentFeedback::withStrokes(current, strokes, origin),
                                                                               d->documentFeedback->reviewVersion()),
                                    &error)) {
        d->messageWindow->display(i18n("It could not be taken back: %1", error), QString(), PageViewMessage::Error, 5000);
        Q_EMIT annotationUndoChanged();
        return false;
    }
    if (whole) {
        d->drawings.removeAll(step.feedbackId);
        d->textBoxes.removeAll(step.feedbackId);
        if (d->documentFeedback->selectedTextBox() == step.feedbackId) selectTextBox(QString());
    }
    viewport()->update();
    Q_EMIT annotationUndoChanged();
    return true;
}

void PageView::stopDrawing()
{
    (d->modeBeforeDrawing == Okular::Settings::EnumMouseMode::RectSelect ? d->aMouseSelect : d->aMouseTextSelect)->trigger();
}

void PageView::endDrawing()
{
    finishErasing();
    finishDrawingStroke();
    const QString last = d->drawings.isEmpty() ? QString() : d->drawings.constLast();
    d->drawings.clear();
    // Here and not at the first stroke: the panel changes the size of the page being drawn on.
    if (!last.isEmpty() && !d->documentFeedback->record(last).isEmpty()) Q_EMIT documentFeedbackAdded(last);
}

namespace
{
// The text of a text box while it is typed, where it goes on the page. Esc ends it, and so does the
// focus going elsewhere (a click on the page or another field), but not a popup menu or another
// application taking it for a while.
class TextBoxEditor : public QTextEdit
{
public:
    using QTextEdit::QTextEdit;
    std::function<void(bool byFocus)> finished;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
        return QTextEdit::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            if (finished) finished(false);
            return;
        }
        QTextEdit::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        QTextEdit::focusOutEvent(event);
        if (event->reason() != Qt::PopupFocusReason && event->reason() != Qt::ActiveWindowFocusReason && finished) finished(true);
    }
};
}

void PageView::slotSetMouseTextBox()
{
    // Nowhere to keep a text box: the tool in use stays, and the reason shows in the middle of the
    // view, where the controls of a review do not cover it.
    if (!drawingsVisible()) {
        QAction *inUse = d->mouseMode == Okular::Settings::EnumMouseMode::RectSelect ? d->aMouseSelect
            : d->mouseMode == Okular::Settings::EnumMouseMode::Draw                 ? d->aMouseDraw
            : d->mouseMode == Okular::Settings::EnumMouseMode::TextSelect           ? d->aMouseTextSelect
                                                                                     : nullptr;
        if (inUse) inUse->setChecked(true);
        else stopTextBoxes();
        d->messageWindow->displayInMiddle(textBoxRefusal(), PageViewMessage::Warning, 4000);
        return;
    }
    if (d->mouseMode != Okular::Settings::EnumMouseMode::TextBox) {
        // From the pencil, the tool both replaced.
        if (d->mouseMode != Okular::Settings::EnumMouseMode::Draw) d->modeBeforeDrawing = d->mouseMode;
        selectionClear();
        textSelectionClear();
        // Not saved, as for the pencil: the next start opens with a selection tool.
        d->mouseMode = Okular::Settings::EnumMouseMode::TextBox;
        d->messageWindow->hide();
        updateCursor();
        if (d->annotator) {
            d->annotator->detachAnnotation();
        }
    }
    // As in Preview: each press puts a new box on the page in view, ready to type.
    insertTextBox();
    viewport()->update();
}

void PageView::addTextBoxAt(const QPoint &eventPos)
{
    // "Add Feedback" in the menu of a click on the page: a text box there, with the Text tool on.
    if (!drawingsVisible() || hiddenAnswerArea().contains(QPointF(eventPos))) return;
    if (d->mouseMode != Okular::Settings::EnumMouseMode::TextBox) {
        if (d->mouseMode != Okular::Settings::EnumMouseMode::Draw) d->modeBeforeDrawing = d->mouseMode;
        selectionClear();
        textSelectionClear();
        d->mouseMode = Okular::Settings::EnumMouseMode::TextBox;
        d->aMouseTextBox->setChecked(true);
        updateCursor();
        if (d->annotator) {
            d->annotator->detachAnnotation();
        }
    }
    editTextBox(QString(), eventPos);
    viewport()->update();
}

QString PageView::textBoxRefusal() const
{
    return i18n("Text boxes are kept with Library documents, and this PDF is not one.");
}

bool PageView::insertTextBox()
{
    // The box being typed is kept first, so that the new one goes below it.
    finishTextBoxEditor();
    // The page that shows most, and the middle of what shows of it.
    const QRect visible(contentAreaPosition(), viewport()->size());
    const PageViewItem *page = nullptr;
    QRect shown;
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible()) continue;
        const QRect part = item->croppedGeometry().intersected(visible);
        if (!part.isEmpty() && (!page || part.width() * part.height() > shown.width() * shown.height())) {
            page = item;
            shown = part;
        }
    }
    if (!page) return false;
    // The box starts a little left of the middle and grows to the right as it is typed; boxes already
    // where its first line would go move it down below them, so that they do not cover each other.
    const int number = page->pageNumber() + 1;
    const QRect whole = page->croppedGeometry();
    const double line = defaultTextBoxFontSize(number) * whole.height() * 1.4;
    QList<QRectF> boxes;
    for (const QJsonObject &record : d->documentFeedback->records()) {
        if (!Thoughts::DocumentFeedback::isTextBox(record) || record.value(QStringLiteral("deleted")).toBool()) continue;
        const auto rects = d->documentFeedback->rectsOf(record.value(QStringLiteral("feedback_id")).toString());
        if (!rects.isEmpty() && rects.constFirst().first == number) boxes.append(reviewFeedbackRect(number, rects.constFirst().second).adjusted(-4.0, -4.0, 4.0, 4.0));
    }
    QPoint at(shown.center().x() - whole.width() / 8, shown.center().y());
    for (int tries = 0; tries < 50 && at.y() + line < shown.bottom(); ++tries) {
        const QRectF firstLine(at.x(), at.y() - line / 2.0, whole.width() / 4.0, line);
        const auto covered = std::find_if(boxes.cbegin(), boxes.cend(), [&firstLine](const QRectF &box) { return box.intersects(firstLine); });
        if (covered == boxes.cend()) break;
        at.setY(int(std::ceil(covered->bottom() + line / 2.0)) + 1);
    }
    if (hiddenAnswerArea().contains(QPointF(at))) {
        d->messageWindow->displayInMiddle(i18n("Reveal the answer to type over it."), PageViewMessage::Warning, 4000);
        return false;
    }
    return editTextBox(QString(), at);
}

void PageView::stopTextBoxes()
{
    (d->modeBeforeDrawing == Okular::Settings::EnumMouseMode::RectSelect ? d->aMouseSelect : d->aMouseTextSelect)->trigger();
}

void PageView::endTextBoxes()
{
    finishTextBoxEditor();
    d->textBoxPressed.clear();
    d->textBoxMoved = QRectF();
    d->documentFeedback->setTextBoxInHand(QString());
    selectTextBox(QString());
    const QString last = d->textBoxes.isEmpty() ? QString() : d->textBoxes.constLast();
    d->textBoxes.clear();
    // As for the pencil: here and not at the first box, because the panel changes the size of the page.
    const QJsonObject record = last.isEmpty() ? QJsonObject() : d->documentFeedback->record(last);
    if (!record.isEmpty() && !record.value(QStringLiteral("deleted")).toBool()) Q_EMIT documentFeedbackAdded(last);
}

QString PageView::textBoxAt(const QPoint &eventPos) const
{
    if (!drawingsVisible() || hiddenAnswerArea().contains(QPointF(eventPos))) return {};
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible() || !item->croppedGeometry().adjusted(-4, -4, 4, 4).contains(eventPos)) continue;
        const QString box = d->documentFeedback->textBoxAt(item->pageNumber() + 1, eventPos, [this](int page, const QRectF &rect) { return reviewFeedbackRect(page, rect); });
        if (!box.isEmpty()) return box;
    }
    return {};
}

void PageView::selectTextBox(const QString &feedbackId)
{
    if (d->documentFeedback->selectedTextBox() == feedbackId) return;
    d->documentFeedback->setSelectedTextBox(feedbackId);
    viewport()->update();
}

void PageView::pressTextBoxTool(const QPoint &eventPos)
{
    // A click away from the box being typed ends the typing, and places nothing.
    const bool ended = std::exchange(d->textBoxJustFinished, false) || d->textBoxEditor;
    finishTextBoxEditor();
    d->textBoxJustFinished = false;
    d->textBoxPressed.clear();
    d->textBoxMoved = QRectF();
    const PageViewItem *item = pickItemOnPoint(eventPos.x(), eventPos.y());
    const QString box = textBoxAt(eventPos);
    if (!box.isEmpty()) {
        // Selected at once; a drag moves it, a click on it once selected edits it.
        d->textBoxPressedSelected = !ended && d->documentFeedback->selectedTextBox() == box;
        d->textBoxPressed = box;
        d->textBoxPressPos = eventPos;
        d->textBoxPage = d->documentFeedback->rectsOf(box).constFirst().first;
        selectTextBox(box);
        return;
    }
    // Away from the boxes: the end of what was going on, or else a new box.
    const bool selected = !d->documentFeedback->selectedTextBox().isEmpty();
    selectTextBox(QString());
    if (!item || ended || selected) return;
    if (!drawingsVisible()) {
        d->messageWindow->displayInMiddle(textBoxRefusal(), PageViewMessage::Warning, 4000);
        return;
    }
    if (hiddenAnswerArea().contains(QPointF(eventPos))) {
        d->messageWindow->displayInMiddle(i18n("Reveal the answer to type over it."), PageViewMessage::Warning, 4000);
        return;
    }
    editTextBox(QString(), eventPos);
}

void PageView::moveTextBoxTool(const QPoint &eventPos)
{
    const QJsonObject record = d->documentFeedback->record(d->textBoxPressed);
    const QRectF page = reviewFeedbackRect(d->textBoxPage, QRectF(0.0, 0.0, 1.0, 1.0));
    if (record.isEmpty() || page.isEmpty()) return;
    if (d->textBoxMoved.isEmpty() && (eventPos - d->textBoxPressPos).manhattanLength() < QApplication::startDragDistance()) return;
    const QRectF from = Thoughts::DocumentFeedback::textBoxRect(record);
    // It stays on its page, however far the pointer goes.
    QRectF to = from.translated((eventPos.x() - d->textBoxPressPos.x()) / page.width(), (eventPos.y() - d->textBoxPressPos.y()) / page.height());
    to.moveLeft(std::clamp(to.left(), 0.0, 1.0 - to.width()));
    to.moveTop(std::clamp(to.top(), 0.0, 1.0 - to.height()));
    d->textBoxMoved = to;
    d->documentFeedback->setTextBoxInHand(d->textBoxPressed);
    viewport()->update();
    updateCursor(eventPos);
}

void PageView::releaseTextBoxTool(const QPoint &eventPos)
{
    const QString pressed = std::exchange(d->textBoxPressed, QString());
    const QRectF moved = std::exchange(d->textBoxMoved, QRectF());
    const bool wasSelected = std::exchange(d->textBoxPressedSelected, false);
    if (pressed.isEmpty()) return;
    d->documentFeedback->setTextBoxInHand(QString());
    viewport()->update();
    const QJsonObject current = d->documentFeedback->record(pressed);
    if (current.isEmpty() || current.value(QStringLiteral("deleted")).toBool()) return;
    if (!moved.isEmpty()) {
        QString error;
        const QJsonObject record = Thoughts::FeedbackVersion::stamped(
            Thoughts::DocumentFeedback::withTextBox(current, moved, Thoughts::DocumentFeedback::textBoxFontSize(current), current.value(QStringLiteral("comment")).toString(),
                                                    Thoughts::Platform::reviewOriginId()),
            d->documentFeedback->reviewVersion());
        if (!d->documentFeedback->write(record, &error)) d->messageWindow->display(i18n("The text box could not be moved: %1", error), QString(), PageViewMessage::Error, 5000);
        updateCursor(eventPos);
        return;
    }
    if (wasSelected) editTextBox(pressed);
}

double PageView::defaultTextBoxFontSize(int page) const
{
    // 12 points, the size of a document's text, as a fraction of the page's height.
    const Okular::Page *shown = d->document->page(page - 1);
    const QSizeF dpi = Okular::Utils::realDpi(window() ? window()->windowHandle() : nullptr);
    if (!shown || shown->height() <= 0.0 || dpi.height() <= 0.0) return 0.015;
    return std::clamp(12.0 / (shown->height() / dpi.height() * 72.0), 0.008, 0.06);
}

QRectF PageView::textBoxRectAt(int page, const QPointF &topLeft, double fontSize, const QString &text) const
{
    const QRectF shown = reviewFeedbackRect(page, QRectF(0.0, 0.0, 1.0, 1.0));
    const Okular::Page *okularPage = d->document->page(page - 1);
    const double aspect = !shown.isEmpty() ? shown.width() / shown.height() : okularPage && okularPage->height() > 0.0 ? okularPage->width() / okularPage->height() : 0.75;
    // From where it starts to the right edge of the page, where its lines wrap.
    QRectF rect(topLeft, Thoughts::ReviewFeedbackOverlay::textBoxSize(text, fontSize, 1.0 - topLeft.x(), aspect));
    if (rect.bottom() > 1.0) rect.moveTop(std::max(0.0, 1.0 - rect.height()));
    return rect;
}

bool PageView::editTextBox(const QString &feedbackId, const QPoint &eventPos)
{
    finishTextBoxEditor();
    QString text;
    if (feedbackId.isEmpty()) {
        const PageViewItem *item = pickItemOnPoint(eventPos.x(), eventPos.y());
        if (!item || !drawingsVisible()) return false;
        d->textBoxPage = item->pageNumber() + 1;
        const QRectF page = reviewFeedbackRect(d->textBoxPage, QRectF(0.0, 0.0, 1.0, 1.0));
        if (page.isEmpty()) return false;
        d->textBoxStartFontSize = defaultTextBoxFontSize(d->textBoxPage);
        // The first line where the click was, with room for a few words to its right.
        d->textBoxStart = QPointF(std::clamp((eventPos.x() - page.left()) / page.width(), 0.0, 0.85),
                                  std::clamp((eventPos.y() - page.top()) / page.height() - d->textBoxStartFontSize * 0.6, 0.0, 1.0 - d->textBoxStartFontSize * 1.3));
    } else {
        const QJsonObject record = d->documentFeedback->record(feedbackId);
        const auto rects = d->documentFeedback->rectsOf(feedbackId);
        if (!Thoughts::DocumentFeedback::isTextBox(record) || rects.isEmpty()) return false;
        d->textBoxPage = rects.constFirst().first;
        d->textBoxStart = Thoughts::DocumentFeedback::textBoxRect(record).topLeft();
        d->textBoxStartFontSize = Thoughts::DocumentFeedback::textBoxFontSize(record);
        text = record.value(QStringLiteral("comment")).toString();
    }
    d->textBoxEditing = feedbackId;
    selectTextBox(QString());

    auto *editor = new TextBoxEditor(viewport());
    editor->setObjectName(QStringLiteral("textBoxEditor"));
    editor->setAcceptRichText(false);
    editor->setFrameShape(QFrame::NoFrame);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    editor->setContentsMargins(0, 0, 0, 0);
    editor->document()->setDocumentMargin(0);
    // The page shows through: what is typed is where it will stay.
    QPalette colours = editor->palette();
    colours.setColor(QPalette::Base, Qt::transparent);
    colours.setColor(QPalette::Text, Thoughts::ReviewFeedbackOverlay::colour(Thoughts::ReviewCardFeedback::neutral(), palette().color(QPalette::Window).lightness() < 128));
    editor->setPalette(colours);
    editor->viewport()->setAutoFillBackground(false);
    editor->setPlainText(text);
    editor->moveCursor(QTextCursor::End);
    editor->finished = [this, editor](bool byFocus) {
        // An editor already put away (its box kept as the next one opened) loses the focus as it
        // hides: that is no click ending the typing.
        if (d->textBoxEditor != editor) return;
        if (byFocus) {
            // The click that took the focus ends the typing; it does not also place a box.
            d->textBoxJustFinished = true;
            QTimer::singleShot(0, this, [this] { d->textBoxJustFinished = false; });
        }
        const QString kept = finishTextBoxEditor();
        // Esc leaves the box selected, so that Delete removes it and Esc again ends the selection.
        if (!byFocus) {
            selectTextBox(kept);
            setFocus(Qt::OtherFocusReason);
        }
    };
    connect(editor, &QTextEdit::textChanged, this, &PageView::placeTextBoxEditor);
    d->textBoxEditor = editor;
    d->documentFeedback->setTextBoxInHand(feedbackId);
    placeTextBoxEditor();
    editor->show();
    editor->setFocus(Qt::MouseFocusReason);
    viewport()->update();
    return true;
}

void PageView::placeTextBoxEditor()
{
    QTextEdit *editor = d->textBoxEditor;
    if (!editor) return;
    const QRectF page = reviewFeedbackRect(d->textBoxPage, QRectF(0.0, 0.0, 1.0, 1.0));
    if (page.isEmpty()) return;
    const QFont font = Thoughts::ReviewFeedbackOverlay::textBoxFont(d->textBoxStartFontSize * page.height());
    if (editor->font() != font) editor->setFont(font);
    const QPointF topLeft(page.left() + d->textBoxStart.x() * page.width(), page.top() + d->textBoxStart.y() * page.height());
    // As wide as its longest line and a little more for the caret, up to the right edge of the page.
    const QFontMetricsF metrics(font);
    double widest = 0.0;
    for (const QString &line : editor->toPlainText().split(QLatin1Char('\n'))) widest = std::max(widest, metrics.horizontalAdvance(line));
    const double available = std::max(1.0, page.right() - topLeft.x());
    const double width = std::clamp(widest + metrics.averageCharWidth() * 1.5, std::min(metrics.averageCharWidth() * 6.0, available), available);
    editor->document()->setTextWidth(width);
    const double height = std::max(editor->document()->size().height(), metrics.lineSpacing());
    const QRect geometry = QRectF(topLeft - QPointF(contentAreaPosition()), QSizeF(width, height)).toAlignedRect();
    if (editor->geometry() != geometry) {
        editor->setGeometry(geometry);
        viewport()->update();
    }
}

QString PageView::finishTextBoxEditor()
{
    QPointer<QTextEdit> editor = std::exchange(d->textBoxEditor, nullptr);
    if (!editor) return {};
    const bool focused = editor->hasFocus();
    const QString text = Thoughts::ReviewCardFeedback::cleanComment(editor->toPlainText());
    const QString id = std::exchange(d->textBoxEditing, QString());
    d->documentFeedback->setTextBoxInHand(QString());
    editor->hide();
    editor->deleteLater();
    if (focused) setFocus(Qt::OtherFocusReason);
    viewport()->update();
    // The document went away while it was typed.
    if (!drawingsVisible()) return {};
    const QJsonObject current = id.isEmpty() ? QJsonObject() : d->documentFeedback->record(id);
    if (!id.isEmpty() && (current.isEmpty() || current.value(QStringLiteral("deleted")).toBool())) return {};
    // A box emptied is a box deleted, with Undo; a new one with nothing typed is nothing.
    if (text.isEmpty()) {
        if (!current.isEmpty()) deleteDocumentFeedback(id);
        return {};
    }
    if (!current.isEmpty() && text == current.value(QStringLiteral("comment")).toString()) return id;
    const QRectF rect = textBoxRectAt(d->textBoxPage, d->textBoxStart, d->textBoxStartFontSize, text);
    const QString origin = Thoughts::Platform::reviewOriginId();
    const QJsonObject record = Thoughts::FeedbackVersion::stamped(
        current.isEmpty() ? Thoughts::DocumentFeedback::createTextBox(d->textBoxPage, rect, d->textBoxStartFontSize, text, d->documentFeedback->documentId(),
                                                                      d->documentFeedback->pdfSha256(), origin)
                          : Thoughts::DocumentFeedback::withTextBox(current, rect, d->textBoxStartFontSize, text, origin),
        d->documentFeedback->reviewVersion());
    QString error;
    if (!d->documentFeedback->write(record, &error)) {
        d->messageWindow->display(i18n("The text could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
        return id;
    }
    if (current.isEmpty()) {
        d->textBoxes.append(record.value(QStringLiteral("feedback_id")).toString());
        rememberAnnotation(record.value(QStringLiteral("feedback_id")).toString());
    }
    return record.value(QStringLiteral("feedback_id")).toString();
}

QRectF PageView::reviewFeedbackRect(int page, const QRectF &rect) const
{
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible() || item->pageNumber() != page - 1) continue;
        const QPoint origin = item->uncroppedGeometry().topLeft();
        return QRectF(origin.x() + rect.left() * item->uncroppedWidth(), origin.y() + rect.top() * item->uncroppedHeight(),
                      rect.width() * item->uncroppedWidth(), rect.height() * item->uncroppedHeight());
    }
    return {};
}

Thoughts::ReviewFeedbackOverlay *PageView::reviewFeedback() const
{
    return d->reviewFeedback;
}

QString PageView::reviewSessionCardId() const
{
    if (d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight || d->m_reviewMaskRects.isEmpty()) return {};
    return d->reviewFeedback->cardId();
}

bool PageView::reviewFeedbackVisible() const
{
    return d->reviewFeedback->hasCard() && d->m_reviewMaskStyle == ReviewMaskStyle::Revealed && !d->m_reviewMaskRects.isEmpty();
}

QList<QPair<int, QRectF>> PageView::textSelectionLines() const
{
    QList<QPair<int, QRectF>> lines;
    for (uint pageNumber = 0; pageNumber < d->document->pages(); ++pageNumber) {
        const Okular::Page *page = d->document->page(pageNumber);
        if (!page || !page->textSelection() || page->textSelection()->isEmpty()) continue;
        QList<QRectF> rects;
        for (const Okular::NormalizedRect &rect : std::as_const(*page->textSelection())) rects.append(QRectF(QPointF(rect.left, rect.top), QPointF(rect.right, rect.bottom)));
        QList<QRectF> merged = Thoughts::normalizedTextLineRects(std::move(rects), QSizeF(page->width(), page->height()));
        std::sort(merged.begin(), merged.end(), [](const QRectF &a, const QRectF &b) { return a.top() != b.top() ? a.top() < b.top() : a.left() < b.left(); });
        for (const QRectF &line : std::as_const(merged)) lines.append({int(pageNumber) + 1, line});
    }
    return lines;
}

// The selected text of every page with a text selection, in page order.
static QString feedbackSelectionText(const Okular::Document *document)
{
    QString text;
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        const Okular::Page *page = document->page(pageNumber);
        if (page && page->textSelection() && !page->textSelection()->isEmpty())
            text.append(page->text(page->textSelection(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour));
    }
    return text;
}

QList<QPair<int, QRectF>> PageView::feedbackLines() const
{
    return d->feedbackAreaLines.isEmpty() ? textSelectionLines() : d->feedbackAreaLines;
}

QString PageView::feedbackText() const
{
    return d->feedbackAreaLines.isEmpty() ? feedbackSelectionText(d->document) : d->feedbackAreaText;
}

bool PageView::feedbackCovers(int page, const QPointF &centre) const
{
    if (d->feedbackAreaLines.isEmpty()) {
        const Okular::Page *pdfPage = page >= 1 && page <= int(d->document->pages()) ? d->document->page(page - 1) : nullptr;
        return pdfPage && pdfPage->textSelection() && pdfPage->textSelection()->contains(centre.x(), centre.y());
    }
    return std::any_of(d->feedbackAreaLines.cbegin(), d->feedbackAreaLines.cend(), [page, centre](const auto &line) { return line.first == page && line.second.contains(centre); });
}

void PageView::updateTextDragArea(const QPoint pos)
{
    // Large enough that a drag started in a margin beside a line reaches its text first.
    constexpr int minimumSide = 40;
    const QRect drag = QRect(d->mouseSelectPos.toPoint(), pos).normalized();
    bool area = drag.width() >= minimumSide && drag.height() >= minimumSide;
    for (int page : std::as_const(d->pagesWithTextSelection)) {
        const Okular::Page *selected = area && page >= 0 && page < int(d->document->pages()) ? d->document->page(page) : nullptr;
        if (selected && selected->textSelection() && !selected->textSelection()->isEmpty()) {
            area = false;
        }
    }
    if (!area && !d->textDragArea) {
        return;
    }
    const QRect before = d->mouseSelectionRect.normalized();
    d->textDragArea = area;
    d->mouseSelectionRect = area ? drag : QRect();
    if (area) {
        d->mouseSelectionColor = palette().color(QPalette::Active, QPalette::Highlight);
    }
    viewport()->update((before | d->mouseSelectionRect).translated(-contentAreaPosition()).adjusted(-2, -2, 2, 2));
}

void PageView::showAreaFeedbackMenu(const QRect &selectionRect, const QPoint globalPos)
{
    // The text inside the area, if any, is its quote; a figure drawn without text gets "Area on page N".
    QString text;
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible() || !selectionRect.intersects(item->croppedGeometry())) {
            continue;
        }
        const Okular::Page *okularPage = item->page();
        if (!okularPage->hasTextPage()) {
            continue;
        }
        QRect relative = selectionRect.intersected(item->croppedGeometry());
        relative.translate(-item->uncroppedGeometry().topLeft());
        Okular::RegularAreaRect rects;
        rects.append(Okular::NormalizedRect(relative, item->uncroppedWidth(), item->uncroppedHeight()));
        text += okularPage->text(&rects);
    }
    setFeedbackArea(selectionRect, text);
    const auto clearArea = qScopeGuard([this] {
        d->feedbackAreaLines.clear();
        selectionClear();
    });
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("AreaFeedbackMenu"));
    addReviewFeedbackActions(&menu);
    if (menu.actions().isEmpty()) {
        return;
    }
    menu.exec(globalPos);
}

void PageView::setFeedbackArea(const QRect &selectionRect, const QString &text)
{
    d->feedbackAreaLines.clear();
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (!item->isVisible() || !selectionRect.intersects(item->croppedGeometry())) {
            continue;
        }
        const QRect relative = selectionRect.intersected(item->croppedGeometry()).translated(-item->uncroppedGeometry().topLeft());
        const Okular::NormalizedRect rect(relative, item->uncroppedWidth(), item->uncroppedHeight());
        d->feedbackAreaLines.append({item->pageNumber() + 1, QRectF(QPointF(rect.left, rect.top), QPointF(rect.right, rect.bottom))});
    }
    // A figure may have no text of its own, and a record needs a quote; the region is its anchor.
    d->feedbackAreaText = text.trimmed().isEmpty() && !d->feedbackAreaLines.isEmpty() ? i18n("Area on page %1", d->feedbackAreaLines.constFirst().first) : text;
}

// Clears every page's text selection, also one set without a drag.
static void clearFeedbackSelection(Okular::Document *document)
{
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        const Okular::Page *page = document->page(pageNumber);
        if (page && page->textSelection()) document->setPageTextSelection(pageNumber, nullptr, QColor());
    }
}

bool PageView::reviewFeedbackSelectionQualifies() const
{
    return reviewFeedbackVisible() && !feedbackText().trimmed().isEmpty() && d->reviewFeedback->qualifies(feedbackLines());
}

QJsonObject PageView::reviewFeedbackForSelection(const QString &polarity, const QString &comment, QString *error) const
{
    if (!reviewFeedbackSelectionQualifies()) {
        if (error) *error = i18n("Select a passage inside the revealed answer.");
        return {};
    }
    return d->reviewFeedback->newRecord(polarity, comment, feedbackLines(), feedbackText(), [this](int page, const QPointF &centre) { return feedbackCovers(page, centre); }, error);
}

bool PageView::addReviewFeedbackFromSelection(const QString &polarity, QString *error)
{
    QString problem;
    const QJsonObject record = reviewFeedbackForSelection(polarity, QString(), &problem);
    if (record.isEmpty() || !d->reviewFeedback->write(record, &problem)) {
        if (error) *error = problem;
        d->messageWindow->display(i18n("The feedback could not be saved: %1", problem), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    textSelectionClear();
    clearFeedbackSelection(d->document);
    return true;
}

QRect PageView::reviewFeedbackGlobalRect(const QString &feedbackId) const
{
    return feedbackGlobalRect(d->reviewFeedback->rectsOf(feedbackId));
}

QRect PageView::feedbackGlobalRect(const QList<QPair<int, QRectF>> &rects) const
{
    QRectF united;
    for (const auto &[page, rect] : rects) {
        const QRectF shown = reviewFeedbackRect(page, rect);
        if (!shown.isEmpty()) united = united.isEmpty() ? shown : united.united(shown);
    }
    if (united.isEmpty()) return QRect(viewport()->mapToGlobal(viewport()->rect().center()), QSize(1, 1));
    const QRect local = united.toAlignedRect().translated(-contentAreaPosition());
    return QRect(viewport()->mapToGlobal(local.topLeft()), local.size());
}

Thoughts::ReviewFeedbackEditor *PageView::openReviewFeedbackEditor(const QString &feedbackId)
{
    const QJsonObject record = d->reviewFeedback->record(feedbackId);
    if (record.isEmpty() || record.value(QStringLiteral("deleted")).toBool()) return nullptr;
    auto *editor = new Thoughts::ReviewFeedbackEditor(record, false, this);
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [this, feedbackId](const QString &polarity, const QString &comment) {
        const QJsonObject current = d->reviewFeedback->record(feedbackId);
        QString error;
        if (current.isEmpty() || !d->reviewFeedback->write(Thoughts::FeedbackVersion::stamped(Thoughts::ReviewCardFeedback::edit(current, polarity, comment, Thoughts::Platform::reviewOriginId()),
                                                                                        d->reviewFeedback->reviewVersion()), &error))
            d->messageWindow->display(i18n("The feedback could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
    });
    connect(editor, &Thoughts::ReviewFeedbackEditor::deleteRequested, this, [this, feedbackId] { deleteReviewFeedback(feedbackId); });
    editor->setChangePassageAvailable(canChangeReviewFeedbackPassage(feedbackId));
    connect(editor, &Thoughts::ReviewFeedbackEditor::changePassageRequested, this, [this, feedbackId](const QString &polarity, const QString &kind, const QString &comment) {
        startPassageChange(false, feedbackId, polarity, kind, comment);
    });
    editor->popupAt(reviewFeedbackGlobalRect(feedbackId));
    return editor;
}

Thoughts::ReviewFeedbackEditor *PageView::openNewReviewFeedbackEditor()
{
    QString error;
    const QJsonObject draft = reviewFeedbackForSelection(Thoughts::ReviewCardFeedback::positive(), QString(), &error);
    if (draft.isEmpty()) {
        d->messageWindow->display(error, QString(), PageViewMessage::Warning, 4000);
        return nullptr;
    }
    // Nothing is saved until Save.
    QRect anchor;
    for (const auto &[page, rect] : feedbackLines()) {
        const QRect shown = reviewFeedbackRect(page, rect).toAlignedRect();
        anchor = anchor.isEmpty() ? shown : anchor.united(shown);
    }
    anchor.translate(-contentAreaPosition());
    anchor = QRect(viewport()->mapToGlobal(anchor.topLeft()), anchor.size());
    auto *editor = new Thoughts::ReviewFeedbackEditor(draft, true, this);
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [this, draft](const QString &polarity, const QString &comment) {
        QJsonObject record = draft;
        record.insert(QStringLiteral("polarity"), polarity);
        record.insert(QStringLiteral("comment"), Thoughts::ReviewCardFeedback::cleanComment(comment));
        // Created when saved.
        const QString now = Thoughts::ReviewCardFeedback::timestamp(QDateTime::currentDateTimeUtc());
        record.insert(QStringLiteral("created_at"), now);
        record.insert(QStringLiteral("updated_at"), now);
        QString error;
        if (!d->reviewFeedback->write(record, &error)) {
            d->messageWindow->display(i18n("The feedback could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
            return;
        }
        textSelectionClear();
        clearFeedbackSelection(d->document);
    });
    editor->popupAt(anchor);
    return editor;
}

bool PageView::deleteReviewFeedback(const QString &feedbackId)
{
    const QJsonObject current = d->reviewFeedback->record(feedbackId);
    if (current.isEmpty() || current.value(QStringLiteral("deleted")).toBool()) return false;
    const QJsonObject tombstone = Thoughts::ReviewCardFeedback::remove(current, Thoughts::Platform::reviewOriginId());
    QString error;
    if (!d->reviewFeedback->write(tombstone, &error)) {
        d->messageWindow->display(i18n("The feedback could not be deleted: %1", error), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    showFeedbackUndo([this, tombstone](QString *error) {
        const QJsonObject latest = d->reviewFeedback->record(tombstone.value(QStringLiteral("feedback_id")).toString());
        return d->reviewFeedback->write(Thoughts::ReviewCardFeedback::undo(latest.isEmpty() ? tombstone : latest, Thoughts::Platform::reviewOriginId()), error);
    });
    return true;
}

bool PageView::canChangeReviewFeedbackPassage(const QString &feedbackId)
{
    if (!reviewFeedbackVisible()) return false;
    const QJsonObject record = d->reviewFeedback->record(feedbackId);
    // The card's own highlight (not shared from another card), drawn on the displayed PDF.
    return !record.isEmpty() && !record.value(QStringLiteral("deleted")).toBool() && !Thoughts::ReviewCardFeedback::isShared(record, d->reviewFeedback->cardId())
        && !d->reviewFeedback->rectsOf(feedbackId).isEmpty();
}

bool PageView::canChangeDocumentFeedbackPassage(const QString &feedbackId)
{
    if (!documentFeedbackVisible()) return false;
    const QJsonObject record = d->documentFeedback->record(feedbackId);
    return !record.isEmpty() && !record.value(QStringLiteral("deleted")).toBool() && !Thoughts::DocumentFeedback::isPageMark(record)
        && !Thoughts::DocumentFeedback::isAttachmentFeedback(record) && !d->documentFeedback->rectsOf(feedbackId).isEmpty();
}

bool PageView::startPassageChange(bool documentFeedback, const QString &feedbackId, const QString &polarity, const QString &kind, const QString &comment)
{
    if (documentFeedback ? !canChangeDocumentFeedbackPassage(feedbackId) : !canChangeReviewFeedbackPassage(feedbackId)) return false;
    cancelPassageChange();
    const auto rects = documentFeedback ? d->documentFeedback->rectsOf(feedbackId) : d->reviewFeedback->rectsOf(feedbackId);
    PageViewPrivate::PassageChange change;
    change.document = documentFeedback;
    change.feedbackId = feedbackId;
    change.cardId = documentFeedback ? QString() : d->reviewFeedback->cardId();
    change.documentId = documentFeedback ? d->documentFeedback->documentId() : QString();
    change.polarity = polarity;
    change.kind = kind;
    change.comment = comment;
    d->passageChange = change;
    // The current passage, selected: the user drags a new selection, or saves this one.
    textSelectionClear();
    clearFeedbackSelection(d->document);
    QMap<int, Okular::RegularAreaRect> areas;
    for (const auto &[page, rect] : rects) {
        if (page < 1 || page > int(d->document->pages())) continue;
        areas[page - 1].appendShape(Okular::NormalizedRect(rect.left(), rect.top(), rect.right(), rect.bottom()));
    }
    for (auto it = areas.cbegin(); it != areas.cend(); ++it)
        d->document->setPageTextSelection(it.key(), std::make_unique<Okular::RegularAreaRect>(it.value()), palette().color(QPalette::Active, QPalette::Highlight));
    if (documentFeedback) d->documentFeedback->setHiddenFeedback(feedbackId);
    else d->reviewFeedback->setHiddenFeedback(feedbackId);
    showPassageChangeBanner();
    viewport()->update();
    return true;
}

bool PageView::passageChangeActive() const
{
    return d->passageChange.has_value();
}

QString PageView::passageChangeFeedbackId() const
{
    return d->passageChange ? d->passageChange->feedbackId : QString();
}

void PageView::showPassageChangeBanner()
{
    if (d->passageBanner) d->passageBanner->deleteLater();
    auto *banner = new QFrame(viewport());
    banner->setObjectName(QStringLiteral("feedbackPassageBanner"));
    banner->setAttribute(Qt::WA_StyledBackground, true);
    banner->setStyleSheet(QStringLiteral("QFrame#feedbackPassageBanner { background: palette(tool-tip-base); border: 1px solid palette(mid); border-radius: 8px; }"));
    auto *layout = new QHBoxLayout(banner);
    layout->setContentsMargins(12, 6, 8, 6);
    layout->setSpacing(8);
    auto *label = new QLabel(i18n("Select the new passage"), banner);
    label->setObjectName(QStringLiteral("feedbackPassageLabel"));
    layout->addWidget(label);
    auto *save = new QPushButton(i18n("Save passage"), banner);
    save->setObjectName(QStringLiteral("feedbackPassageSave"));
    save->setAutoDefault(false);
    layout->addWidget(save);
    auto *cancel = new QPushButton(i18n("Cancel"), banner);
    cancel->setObjectName(QStringLiteral("feedbackPassageCancel"));
    cancel->setFlat(true);
    cancel->setAutoDefault(false);
    layout->addWidget(cancel);
    connect(save, &QPushButton::clicked, this, [this] { savePassageChange(); });
    connect(cancel, &QPushButton::clicked, this, [this] { cancelPassageChange(); });
    banner->adjustSize();
    banner->move((viewport()->width() - banner->width()) / 2, std::max(0, viewport()->height() - banner->height() - 24));
    banner->show();
    banner->raise();
    d->passageBanner = banner;
}

void PageView::cancelPassageChange()
{
    if (!d->passageChange) return;
    const bool documentFeedback = d->passageChange->document;
    d->passageChange.reset();
    if (d->passageBanner) d->passageBanner->deleteLater();
    // Drawn again; the preselected passage goes.
    if (documentFeedback) d->documentFeedback->setHiddenFeedback(QString());
    else d->reviewFeedback->setHiddenFeedback(QString());
    textSelectionClear();
    clearFeedbackSelection(d->document);
    viewport()->update();
}

void PageView::endPassageChangeIfStale()
{
    if (!d->passageChange || d->passageSaving) return;
    const auto &change = *d->passageChange;
    bool stale;
    if (change.document) {
        const QJsonObject record = d->documentFeedback->record(change.feedbackId);
        stale = !documentFeedbackVisible() || d->documentFeedback->documentId() != change.documentId || record.isEmpty() || record.value(QStringLiteral("deleted")).toBool();
    } else {
        const QJsonObject record = d->reviewFeedback->record(change.feedbackId);
        stale = !reviewFeedbackVisible() || d->reviewFeedback->cardId() != change.cardId || record.isEmpty() || record.value(QStringLiteral("deleted")).toBool();
    }
    if (stale) cancelPassageChange();
}

bool PageView::savePassageChange(QString *error)
{
    if (!d->passageChange) return false;
    const PageViewPrivate::PassageChange change = *d->passageChange;
    const auto refuse = [this, error](const QString &message) {
        if (error) *error = message;
        d->messageWindow->display(message, QString(), PageViewMessage::Warning, 4000);
        return false;
    };
    // The selection must qualify as for Like / Dislike; otherwise the mode stays.
    const bool qualifies = change.document ? documentFeedbackSelectionQualifies() : reviewFeedbackSelectionQualifies();
    if (!qualifies) return refuse(change.document ? i18n("Select a passage of this PDF.") : i18n("Select a passage inside the revealed answer."));
    QString problem;
    // The new anchor exactly as Like / Dislike build it (only the anchor is taken from it).
    const QString positive = Thoughts::ReviewCardFeedback::positive();
    const QJsonObject draft = change.document ? documentFeedbackForSelection(positive, QString(), &problem) : reviewFeedbackForSelection(positive, QString(), &problem);
    if (draft.isEmpty()) return refuse(problem);
    const QJsonObject anchor = draft.value(QStringLiteral("anchor")).toObject();
    const QJsonObject current = change.document ? d->documentFeedback->record(change.feedbackId) : d->reviewFeedback->record(change.feedbackId);
    const QString origin = Thoughts::Platform::reviewOriginId();
    const QJsonObject version = change.document ? d->documentFeedback->reviewVersion() : d->reviewFeedback->reviewVersion();
    d->passageSaving = true;
    bool saved = false;
    std::optional<Thoughts::ReviewCardFeedback::Replacement> replacement;
    if (Thoughts::ReviewCardFeedback::samePassage(current, anchor)) {
        // The same passage: only an edited polarity, kind or comment.
        const bool edited = current.value(QStringLiteral("polarity")).toString() != change.polarity || current.value(QStringLiteral("comment")).toString() != change.comment
            || (change.document && Thoughts::DocumentFeedback::kindOf(current) != change.kind);
        if (!edited) {
            saved = true;
        } else if (change.document) {
            saved = d->documentFeedback->write(Thoughts::FeedbackVersion::stamped(Thoughts::DocumentFeedback::withKind(Thoughts::DocumentFeedback::edit(current, change.polarity, change.comment, origin), change.kind), version), &problem);
        } else {
            saved = d->reviewFeedback->write(Thoughts::FeedbackVersion::stamped(Thoughts::ReviewCardFeedback::edit(current, change.polarity, change.comment, origin), version), &problem);
        }
    } else {
        replacement = change.document ? Thoughts::DocumentFeedback::replacePassage(current, change.polarity, change.kind, change.comment, anchor, origin, QDateTime::currentDateTimeUtc(), &problem)
                                      : Thoughts::ReviewCardFeedback::replacePassage(current, change.polarity, change.comment, anchor, origin, QDateTime::currentDateTimeUtc(), &problem);
        if (replacement) {
            // Stamped as on create (FeedbackVersion).
            replacement->created = Thoughts::FeedbackVersion::stamped(replacement->created, version);
            const QList<QJsonObject> writes{replacement->created, replacement->tombstone};
            saved = change.document ? d->documentFeedback->writeAll(writes, &problem) : d->reviewFeedback->writeAll(writes, &problem);
        }
    }
    d->passageSaving = false;
    if (!saved) {
        if (error) *error = problem;
        d->messageWindow->display(i18n("The passage could not be changed: %1", problem), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    cancelPassageChange();
    if (replacement) {
        const Thoughts::ReviewCardFeedback::Replacement done = *replacement;
        const bool documentFeedback = change.document;
        showFeedbackUndo(
            [this, done, documentFeedback](QString *error) {
                const QString createdId = done.created.value(QStringLiteral("feedback_id")).toString();
                const QString oldId = done.tombstone.value(QStringLiteral("feedback_id")).toString();
                const QJsonObject createdNow = documentFeedback ? d->documentFeedback->record(createdId) : d->reviewFeedback->record(createdId);
                const QJsonObject oldNow = documentFeedback ? d->documentFeedback->record(oldId) : d->reviewFeedback->record(oldId);
                const QList<QJsonObject> writes = Thoughts::ReviewCardFeedback::undoReplacement(done, createdNow, oldNow, Thoughts::Platform::reviewOriginId());
                return documentFeedback ? d->documentFeedback->writeAll(writes, error) : d->reviewFeedback->writeAll(writes, error);
            },
            i18n("Passage changed"));
        if (documentFeedback) Q_EMIT documentFeedbackAdded(done.created.value(QStringLiteral("feedback_id")).toString());
    }
    return true;
}

// "Feedback deleted · Undo" for 5 s; Undo writes the tombstone's revision + 1, not deleted.
bool PageView::openReviewFeedbackAt(const QPointF &contentPoint)
{
    if (!reviewFeedbackVisible()) return false;
    const QString hit = d->reviewFeedback->hitTest(contentPoint, [this](int page, const QRectF &rect) { return reviewFeedbackRect(page, rect); });
    return !hit.isEmpty() && openReviewFeedbackEditor(hit);
}

Thoughts::DocumentFeedbackOverlay *PageView::documentFeedback() const
{
    return d->documentFeedback;
}

bool PageView::documentFeedbackVisible() const
{
    return d->documentFeedback->hasDocument() && (Okular::Settings::highlightComments() || Okular::Settings::highlightFeedback());
}

Thoughts::ReadingOverlayLayer *PageView::readingOverlays() const
{
    return d->readingOverlays;
}

bool PageView::readingOverlaysVisible() const
{
    return d->readingOverlays->hasDocument() && Okular::Settings::highlightReadingOverlays();
}

void PageView::paintReadingOverlays(QPainter *painter, const QRect &contentsRect) const
{
    const auto map = [this, &contentsRect](int page, const QRectF &rect) {
        const QRectF shown = reviewFeedbackRect(page, rect);
        return shown.intersects(contentsRect) ? shown : QRectF();
    };
    QSet<int> pages;
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (item->isVisible() && item->uncroppedGeometry().intersects(contentsRect)) pages.insert(item->pageNumber() + 1);
    }
    if (pages.isEmpty()) return;
    painter->save();
    // Nothing of an agent's shows over a masked review answer either.
    const QPainterPath hidden = hiddenAnswerArea();
    if (!hidden.isEmpty()) {
        QPainterPath open;
        open.addRect(contentsRect);
        painter->setClipPath(open.subtracted(hidden), Qt::IntersectClip);
    }
    // The washes are on the page image, under the user's markup (drawDocumentOnPainter); the
    // comment markers in the margin over everything.
    if (readingOverlaysVisible()) d->readingOverlays->paintMarkers(painter, pages, map);
    // The mark the Overlay tab asked for, on while the count is even.
    if (!d->overlayFlashRects.isEmpty() && d->overlayFlashTicks % 2 == 0) {
        const QColor blue(25, 95, 190);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(blue, 2.0));
        painter->setBrush(QColor(blue.red(), blue.green(), blue.blue(), 40));
        for (const auto &[page, rect] : std::as_const(d->overlayFlashRects)) {
            const QRectF shown = map(page, rect);
            if (!shown.isEmpty()) painter->drawRoundedRect(shown.adjusted(-2.0, -1.5, 2.0, 1.5), 2.0, 2.0);
        }
    }
    painter->restore();
}

QStringList PageView::readingOverlayMarkAt(const QPointF &contentPoint, bool *marker)
{
    if (marker) *marker = false;
    if (!readingOverlaysVisible() || hiddenAnswerArea().contains(contentPoint)) return {};
    const PageViewItem *item = pickItemOnPoint(int(contentPoint.x()), int(contentPoint.y()));
    if (!item) return {};
    const Thoughts::ReadingOverlayLayer::Hit hit = d->readingOverlays->hitTest(item->pageNumber() + 1, contentPoint, [this](int page, const QRectF &rect) {
        return reviewFeedbackRect(page, rect);
    });
    if (hit.isEmpty()) return {};
    if (marker) *marker = hit.marker;
    return {hit.overlayId, hit.markId};
}

bool PageView::flashReadingOverlayMark(const QString &overlayId, const QString &markId)
{
    const Thoughts::ReadingOverlayLayer::Mark mark = d->readingOverlays->mark(overlayId, markId);
    if (mark.rects.isEmpty()) {
        d->messageWindow->display(i18n("This passage is not in this version of the PDF."), QString(), PageViewMessage::Warning, 4000);
        return false;
    }
    const auto &[page, rect] = mark.rects.constFirst();
    Okular::DocumentViewport viewport(page - 1);
    viewport.rePos.enabled = true;
    viewport.rePos.normalizedX = rect.center().x();
    viewport.rePos.normalizedY = rect.center().y();
    viewport.rePos.pos = Okular::DocumentViewport::Center;
    d->document->setViewport(viewport, nullptr, true);
    d->overlayFlashRects = mark.rects;
    d->overlayFlashTicks = 6;
    d->overlayFlashTimer->start();
    this->viewport()->update();
    return true;
}

bool PageView::readingOverlayFlashShown() const
{
    return !d->overlayFlashRects.isEmpty();
}

namespace
{
// A label that does something when clicked: the byline of a reading overlay mark's popup.
class PopupLinkLabel : public QLabel
{
public:
    PopupLinkLabel(std::function<void()> clicked, QWidget *parent)
        : QLabel(parent)
        , m_clicked(std::move(clicked))
    {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && m_clicked) {
            event->accept();
            // Last: it may close the popup this label is in.
            const auto clicked = m_clicked;
            clicked();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    std::function<void()> m_clicked;
};
}

QWidget *PageView::openReadingOverlayMarkPopup(const QString &overlayId, const QString &markId)
{
    delete d->overlayMarkPopup.data();
    const QJsonObject record = d->readingOverlays->record(overlayId);
    const QJsonObject mark = Thoughts::ReadingOverlays::markOf(record, markId);
    if (mark.isEmpty() || Thoughts::ReadingOverlays::isDeleted(record)) return nullptr;
    auto *popup = new QFrame(this, Qt::Popup);
    popup->setObjectName(QStringLiteral("readingOverlayMarkPopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    const QString kind = Thoughts::ReadingOverlays::kindOf(mark);
    const QString kindName = kind == Thoughts::ReadingOverlays::read() ? i18nc("@label reading overlay mark", "Read carefully")
        : kind == Thoughts::ReadingOverlays::skim()                    ? i18nc("@label reading overlay mark", "Skim")
        : kind == Thoughts::ReadingOverlays::skip()                    ? i18nc("@label reading overlay mark", "You can skip this")
                                                                       : i18nc("@label reading overlay mark", "Note");
    const QString noteId = Thoughts::ReadingOverlays::noteIdOf(mark);
    popup->setProperty("overlayId", overlayId);
    popup->setProperty("markId", markId);
    popup->setProperty("noteId", noteId);
    auto *heading = new QLabel(popup);
    heading->setObjectName(QStringLiteral("readingOverlayMarkKind"));
    heading->setTextFormat(Qt::RichText);
    heading->setText(QStringLiteral("<span style=\"color:%1\">\u25A0</span> <b>%2</b>")
                         .arg(Thoughts::ReadingOverlayLayer::kindColour(kind).name(), kindName.toHtmlEscaped()));
    layout->addWidget(heading);
    // The comment; a mark with a note shows the note's picture here instead, once it is here.
    const QString commentText = Thoughts::ReadingOverlays::commentOf(mark);
    if (!commentText.isEmpty()) {
        auto *comment = new QLabel(Thoughts::breakableText(commentText), popup);
        comment->setObjectName(QStringLiteral("readingOverlayMarkComment"));
        comment->setTextFormat(Qt::PlainText);
        comment->setWordWrap(true);
        comment->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(comment);
    }
    // The start of the passage, as the Overlay tab lists it.
    auto *quote = new QLabel(QStringLiteral("“%1”").arg(Thoughts::breakableText(Thoughts::ReadingOverlays::quoteStart(mark))), popup);
    quote->setObjectName(QStringLiteral("readingOverlayMarkQuote"));
    quote->setTextFormat(Qt::PlainText);
    quote->setWordWrap(true);
    QFont italic = quote->font();
    italic.setItalic(true);
    quote->setFont(italic);
    QPalette secondary = quote->palette();
    secondary.setColor(QPalette::WindowText, secondary.color(QPalette::PlaceholderText));
    quote->setPalette(secondary);
    layout->addWidget(quote);
    // The byline: who made the overlay and which one; a click shows the mark in the Overlay tab.
    const QString label = Thoughts::ReadingOverlays::authorLabel(record);
    const QString title = record.value(QStringLiteral("title")).toString();
    auto *author = new PopupLinkLabel(
        [this, popup, overlayId, markId] {
            popup->close();
            Q_EMIT readingOverlayMarkRevealRequested(overlayId, markId);
        },
        popup);
    author->setText(Thoughts::breakableText(label.isEmpty() ? title : i18nc("@info agent label · overlay title", "%1 · %2", label, title)));
    author->setObjectName(QStringLiteral("readingOverlayMarkAuthor"));
    author->setTextFormat(Qt::PlainText);
    author->setWordWrap(true);
    author->setToolTip(i18n("Show this mark in the Overlay tab"));
    QPalette link = author->palette();
    link.setColor(QPalette::WindowText, link.color(QPalette::Link));
    author->setPalette(link);
    layout->addWidget(author);
    // A question for the agent that made the overlay, and the page goes to it.
    auto *question = new QLineEdit(popup);
    question->setObjectName(QStringLiteral("readingOverlayMarkQuestion"));
    question->setPlaceholderText(i18n("Your question (optional)"));
    question->setMaxLength(2000);
    question->setClearButtonEnabled(true);
    layout->addSpacing(2);
    layout->addWidget(question);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *ask = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), i18n("Ask About This"), popup);
    ask->setObjectName(QStringLiteral("readingOverlayAskAboutThis"));
    ask->setDefault(true);
    ask->setToolTip(Thoughts::ReadingOverlays::conversationId(record).isEmpty()
                        ? i18n("Send this page, with the passage marked, to an agent with your question. You see it before it goes.")
                        : i18n("Send this page, with the passage marked, to the conversation that made this overlay, with your question. You see it before it goes."));
    buttons->addWidget(ask);
    layout->addLayout(buttons);
    for (QWidget *widget : popup->findChildren<QLabel *>()) widget->setMaximumWidth(380);
    popup->setMinimumWidth(300);
    connect(ask, &QPushButton::clicked, this, [this, popup, question, overlayId, markId] {
        const QString asked = question->text();
        popup->close();
        Q_EMIT readingOverlayQuestionAsked(overlayId, markId, asked);
    });
    connect(question, &QLineEdit::returnPressed, ask, &QPushButton::click);
    d->overlayMarkPopup = popup;
    const auto image = noteId.isEmpty() ? d->overlayNoteImages.constEnd() : d->overlayNoteImages.constFind(noteId);
    if (image != d->overlayNoteImages.constEnd()) showReadingOverlayNoteImage(popup, *image);
    placeReadingOverlayMarkPopup(popup, overlayId, markId);
    popup->show();
    question->setFocus(Qt::PopupFocusReason);
    // The note's picture, once: the comment shows until it is here (setReadingOverlayNoteImage).
    if (!noteId.isEmpty() && image == d->overlayNoteImages.constEnd() && !d->overlayNoteRequested.contains(noteId)) {
        d->overlayNoteRequested.insert(noteId);
        Q_EMIT readingOverlayNoteRequested(noteId);
    }
    return popup;
}

void PageView::placeReadingOverlayMarkPopup(QWidget *popup, const QString &overlayId, const QString &markId)
{
    // Below its marker, or below the mark's first line when the marker is not on screen.
    const auto map = [this](int page, const QRectF &rect) { return reviewFeedbackRect(page, rect); };
    QRectF anchor = d->readingOverlays->markerRect(overlayId, markId, map);
    if (anchor.isEmpty()) {
        const auto rects = d->readingOverlays->mark(overlayId, markId).rects;
        if (!rects.isEmpty()) anchor = map(rects.constFirst().first, rects.constFirst().second);
    }
    popup->adjustSize();
    const QPoint below = anchor.isEmpty() ? QCursor::pos() : viewport()->mapToGlobal((anchor.bottomRight() - QPointF(contentAreaPosition())).toPoint() + QPoint(0, 4));
    QPoint at(below.x() - popup->width(), below.y());
    if (const QScreen *screen = QGuiApplication::screenAt(below)) {
        const QRect available = screen->availableGeometry();
        at.setX(std::clamp(at.x(), available.left(), std::max(available.left(), available.right() - popup->width())));
        if (at.y() + popup->height() > available.bottom()) at.setY(std::max(available.top(), below.y() - popup->height() - int(anchor.height()) - 8));
    }
    popup->move(at);
}

void PageView::showReadingOverlayNoteImage(QWidget *popup, const QImage &image)
{
    auto *layout = qobject_cast<QVBoxLayout *>(popup->layout());
    if (!layout || image.isNull() || image.width() <= 0 || popup->findChild<QScrollArea *>(QStringLiteral("readingOverlayMarkNoteScroll"))) return;
    const QString noteId = popup->property("noteId").toString();
    // As wide as reading wants (on a narrow screen, what it has); at most 60 % of the screen tall,
    // scrolling beyond.
    const QScreen *screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QMargins margins = layout->contentsMargins();
    const int popupWidth = std::min(readingOverlayNotePopupWidth, std::max(260, available.width() - 32));
    const int width = popupWidth - margins.left() - margins.right();
    const int maximumHeight = std::max(120, int(available.height() * 0.6));
    auto *scroll = new QScrollArea(popup);
    scroll->setObjectName(QStringLiteral("readingOverlayMarkNoteScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    int shownWidth = width;
    int shownHeight = qRound(double(image.height()) * shownWidth / image.width());
    if (shownHeight > maximumHeight) {
        // Taller than the room: narrower by the scroll bar, which then shows.
        shownWidth = width - style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll->verticalScrollBar());
        shownHeight = qRound(double(image.height()) * shownWidth / image.width());
    }
    auto *picture = new PopupLinkLabel(
        [this, popup, noteId] {
            popup->close();
            Q_EMIT readingOverlayNoteOpenRequested(noteId);
        },
        scroll);
    picture->setObjectName(QStringLiteral("readingOverlayMarkNote"));
    picture->setToolTip(i18n("Open this note in Agent Notes: zoom in, draw on it, reply to the agent"));
    const qreal ratio = std::max<qreal>(1.0, popup->devicePixelRatioF());
    QPixmap pixmap = QPixmap::fromImage(image.scaled(QSize(qRound(shownWidth * ratio), qRound(shownHeight * ratio)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(ratio);
    picture->setPixmap(pixmap);
    picture->setFixedSize(shownWidth, shownHeight);
    scroll->setWidget(picture);
    scroll->setFixedSize(width, std::min(shownHeight, maximumHeight));
    // In place of the comment, right under the kind.
    int index = layout->indexOf(popup->findChild<QLabel *>(QStringLiteral("readingOverlayMarkKind"))) + 1;
    if (QLabel *comment = popup->findChild<QLabel *>(QStringLiteral("readingOverlayMarkComment"))) {
        index = layout->indexOf(comment);
        delete comment;
    }
    layout->insertWidget(index, scroll);
    for (QLabel *label : popup->findChildren<QLabel *>()) {
        if (label != picture) label->setMaximumWidth(width);
    }
    popup->setFixedWidth(popupWidth);
    if (popup->isVisible()) {
        // Measured with the picture: shown now, not once the event loop gets to it.
        scroll->show();
        placeReadingOverlayMarkPopup(popup, popup->property("overlayId").toString(), popup->property("markId").toString());
    }
}

void PageView::setReadingOverlayNoteImage(const QString &noteId, const QImage &image)
{
    d->overlayNoteRequested.remove(noteId);
    // Not had: the comment stays, and the next popup asks again.
    if (noteId.isEmpty() || image.isNull()) return;
    if (!d->overlayNoteImages.contains(noteId)) {
        d->overlayNoteImageOrder.append(noteId);
        while (d->overlayNoteImageOrder.size() > 8) d->overlayNoteImages.remove(d->overlayNoteImageOrder.takeFirst());
    }
    d->overlayNoteImages.insert(noteId, image);
    QWidget *popup = d->overlayMarkPopup.data();
    if (popup && popup->isVisible() && popup->property("noteId").toString() == noteId) showReadingOverlayNoteImage(popup, image);
}

bool PageView::answerMasked() const
{
    // While a review card's answer is masked, nothing over it may give it away (hiddenAnswerArea):
    // the user's drawings, text boxes and highlights show elsewhere, and the pencil and the Text
    // tool work elsewhere. Once it is revealed they show with the card's own answer feedback.
    return !d->m_reviewMaskRects.isEmpty() && d->m_reviewMaskStyle == ReviewMaskStyle::Hidden;
}

bool PageView::reviewingCard() const
{
    // A review card's answer is masked or revealed (its answer feedback is shown then); a sidebar
    // highlight of a card is reading.
    return !d->m_reviewMaskRects.isEmpty() && (d->m_reviewMaskStyle == ReviewMaskStyle::Hidden || d->m_reviewMaskStyle == ReviewMaskStyle::Revealed);
}

bool PageView::documentFeedbackSelectionQualifies() const
{
    // Card feedback takes precedence: one selection never creates both.
    return !reviewFeedbackSelectionQualifies() && documentQuestionSelectionQualifies();
}

bool PageView::documentQuestionSelectionQualifies() const
{
    // Card feedback has no questions, so inside a revealed review answer a question about the
    // passage is still the document's (as Like, Dislike and Comment… are the card's).
    if (!d->documentFeedback->hasDocument()) return false;
    const auto lines = feedbackLines();
    return !lines.isEmpty() && lines.size() <= Thoughts::ReviewCardFeedback::maximumRegions && !feedbackText().trimmed().isEmpty();
}

QJsonObject PageView::documentFeedbackForSelection(const QString &polarity, const QString &comment, QString *error, bool question) const
{
    if (!(question ? documentQuestionSelectionQualifies() : documentFeedbackSelectionQualifies())) {
        if (error) *error = i18n("Select a passage of a Library PDF.");
        return {};
    }
    return d->documentFeedback->newRecord(polarity, comment, feedbackLines(), feedbackText(), [this](int page, const QPointF &centre) { return feedbackCovers(page, centre); }, error);
}

bool PageView::addDocumentFeedbackFromSelection(const QString &polarity, QString *error)
{
    QString problem;
    const QJsonObject record = documentFeedbackForSelection(polarity, QString(), &problem);
    if (record.isEmpty() || !d->documentFeedback->write(record, &problem)) {
        if (error) *error = problem;
        d->messageWindow->display(i18n("The feedback could not be saved: %1", problem), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    textSelectionClear();
    clearFeedbackSelection(d->document);
    rememberAnnotation(record.value(QStringLiteral("feedback_id")).toString());
    Q_EMIT documentFeedbackAdded(record.value(QStringLiteral("feedback_id")).toString());
    return true;
}

bool PageView::showDocumentFeedback(const QString &feedbackId, bool edit)
{
    const auto rects = d->documentFeedback->rectsOf(feedbackId);
    if (rects.isEmpty()) {
        d->messageWindow->display(i18n("This passage is not in this version of the PDF."), QString(), PageViewMessage::Warning, 4000);
    } else {
        Okular::DocumentViewport viewport(rects.first().first - 1);
        viewport.rePos.enabled = true;
        viewport.rePos.normalizedX = rects.first().second.center().x();
        viewport.rePos.normalizedY = rects.first().second.center().y();
        viewport.rePos.pos = Okular::DocumentViewport::Center;
        // Straight there for the editor, which opens at the passage's place on screen.
        d->document->setViewport(viewport, nullptr, !edit);
    }
    return edit ? openDocumentFeedbackEditor(feedbackId) != nullptr : !rects.isEmpty();
}

void PageView::setFeedbackPictureStore(const Thoughts::ReviewFeedbackEditor::PictureStore &store)
{
    d->feedbackPictureStore = store;
}

Thoughts::ReviewFeedbackEditor::PictureStore PageView::feedbackPictureStore() const
{
    return d->feedbackPictureStore;
}

void PageView::setUnsentQuestion(const QString &text)
{
    d->unsentQuestion = text;
}

QString PageView::unsentQuestion() const
{
    return d->unsentQuestion;
}

QJsonObject PageView::questionRequest(const QString &question, const QString &quote, const QList<QPair<int, QRectF>> &lines, bool html)
{
    QJsonObject request{{QStringLiteral("question"), question}, {QStringLiteral("quote"), quote.simplified()}, {QStringLiteral("html"), html}};
    if (html || lines.isEmpty()) return request;
    // The passage's first page, with its lines there (the page the agent is shown).
    const int page = lines.constFirst().first;
    QJsonArray regions;
    for (const auto &[linePage, rect] : lines) {
        if (linePage == page) regions.append(QJsonArray{rect.left(), rect.top(), rect.right(), rect.bottom()});
    }
    request.insert(QStringLiteral("page"), page);
    request.insert(QStringLiteral("regions"), regions);
    return request;
}

Thoughts::ReviewFeedbackEditor *PageView::openDocumentFeedbackEditor(const QString &feedbackId, const QRect &anchor)
{
    const QJsonObject record = d->documentFeedback->record(feedbackId);
    if (record.isEmpty() || record.value(QStringLiteral("deleted")).toBool()) return nullptr;
    auto *editor = new Thoughts::ReviewFeedbackEditor(record, false, this, true);
    editor->enablePictures(d->feedbackPictureStore);
    // A question kept before questions were asked: Ask Agent sends it, then it goes (Undo brings it back).
    editor->setAskAvailable(true);
    connect(editor, &Thoughts::ReviewFeedbackEditor::askRequested, this, [this, feedbackId, record, anchor](const QString &question) {
        const QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString();
        Q_EMIT documentQuestionAsked(questionRequest(question, quote, d->documentFeedback->rectsOf(feedbackId), anchor.isValid()));
        deleteDocumentFeedback(feedbackId);
    });
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [this, feedbackId, editor](const QString &polarity, const QString &comment) {
        const QJsonObject current = d->documentFeedback->record(feedbackId);
        QString error;
        // A text box keeps its place and font, and its box fits the new text; the pictures are the editor's.
        const auto nextText = [&]() {
            if (!Thoughts::DocumentFeedback::isTextBox(current) || d->documentFeedback->rectsOf(feedbackId).isEmpty())
                return Thoughts::DocumentFeedback::withKind(Thoughts::DocumentFeedback::edit(current, polarity, comment, Thoughts::Platform::reviewOriginId()), editor->kind());
            const double fontSize = Thoughts::DocumentFeedback::textBoxFontSize(current);
            const QString text = Thoughts::ReviewCardFeedback::cleanComment(comment);
            return Thoughts::DocumentFeedback::withTextBox(current, textBoxRectAt(d->documentFeedback->rectsOf(feedbackId).constFirst().first, Thoughts::DocumentFeedback::textBoxRect(current).topLeft(), fontSize, text),
                                                           fontSize, text, Thoughts::Platform::reviewOriginId());
        };
        const auto next = [&]() { return Thoughts::DocumentFeedback::withPictures(nextText(), editor->pictures()); };
        if (current.isEmpty() || !d->documentFeedback->write(Thoughts::FeedbackVersion::stamped(next(), d->documentFeedback->reviewVersion()), &error))
            d->messageWindow->display(i18n("The feedback could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
    });
    connect(editor, &Thoughts::ReviewFeedbackEditor::deleteRequested, this, [this, feedbackId] { deleteDocumentFeedback(feedbackId); });
    connect(editor, &Thoughts::ReviewFeedbackEditor::resolveRequested, this, [this, feedbackId, record] { resolveDocumentFeedback(feedbackId, !Thoughts::DocumentFeedback::isResolved(record)); });
    // Not for feedback made in an HTML attachment (its editor opens at @p anchor).
    editor->setChangePassageAvailable(!anchor.isValid() && canChangeDocumentFeedbackPassage(feedbackId));
    connect(editor, &Thoughts::ReviewFeedbackEditor::changePassageRequested, this, [this, feedbackId](const QString &polarity, const QString &kind, const QString &comment) {
        startPassageChange(true, feedbackId, polarity, kind, comment);
    });
    editor->popupAt(anchor.isValid() ? anchor : visiblePart(feedbackGlobalRect(d->documentFeedback->rectsOf(feedbackId))));
    return editor;
}

QRect PageView::visiblePart(const QRect &global) const
{
    // Beside what of the passage is on screen: a long one reaches far beyond the window.
    const QRect shown(viewport()->mapToGlobal(QPoint(0, 0)), viewport()->size());
    return global.intersects(shown) ? global.intersected(shown) : global;
}

Thoughts::ReviewFeedbackEditor *PageView::openNewDocumentFeedbackEditor(const QString &kind)
{
    QString error;
    const QJsonObject draft = documentFeedbackForSelection(Thoughts::ReviewCardFeedback::positive(), QString(), &error,
                                                           kind == Thoughts::DocumentFeedback::question());
    if (draft.isEmpty()) {
        d->messageWindow->display(error, QString(), PageViewMessage::Warning, 4000);
        return nullptr;
    }
    // Nothing is saved until Save; a question never is (Ask Agent sends it). A question closed
    // without asking comes back here.
    QJsonObject shown = Thoughts::DocumentFeedback::withKind(draft, kind);
    if (kind == Thoughts::DocumentFeedback::question() && !d->unsentQuestion.isEmpty()) shown.insert(QStringLiteral("comment"), d->unsentQuestion);
    auto *editor = new Thoughts::ReviewFeedbackEditor(shown, true, this, true);
    editor->enablePictures(d->feedbackPictureStore);
    editor->setAskAvailable(true);
    const QString quote = feedbackText();
    const QList<QPair<int, QRectF>> lines = feedbackLines();
    connect(editor, &Thoughts::ReviewFeedbackEditor::askRequested, this, [this, quote, lines](const QString &question) {
        d->unsentQuestion.clear();
        textSelectionClear();
        clearFeedbackSelection(d->document);
        Q_EMIT documentQuestionAsked(questionRequest(question, quote, lines, false));
    });
    connect(editor, &Thoughts::ReviewFeedbackEditor::unsentQuestion, this, [this](const QString &text) { d->unsentQuestion = text; });
    connect(editor, &Thoughts::ReviewFeedbackEditor::saveRequested, this, [this, draft, editor](const QString &polarity, const QString &comment) {
        QJsonObject record = Thoughts::DocumentFeedback::withPictures(Thoughts::DocumentFeedback::withKind(draft, editor->kind()), editor->pictures());
        record.insert(QStringLiteral("polarity"), polarity);
        record.insert(QStringLiteral("comment"), Thoughts::ReviewCardFeedback::cleanComment(comment));
        // Created when saved.
        const QString now = Thoughts::ReviewCardFeedback::timestamp(QDateTime::currentDateTimeUtc());
        record.insert(QStringLiteral("created_at"), now);
        record.insert(QStringLiteral("updated_at"), now);
        QString error;
        if (!d->documentFeedback->write(record, &error)) {
            d->messageWindow->display(i18n("The feedback could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
            return;
        }
        textSelectionClear();
        clearFeedbackSelection(d->document);
        rememberAnnotation(record.value(QStringLiteral("feedback_id")).toString());
        Q_EMIT documentFeedbackAdded(record.value(QStringLiteral("feedback_id")).toString());
    });
    editor->popupAt(feedbackGlobalRect(feedbackLines()));
    return editor;
}

bool PageView::resolveDocumentFeedback(const QString &feedbackId, bool resolved)
{
    const QJsonObject current = d->documentFeedback->record(feedbackId);
    if (current.isEmpty() || !Thoughts::DocumentFeedback::canResolve(current)) return false;
    QString error;
    if (!d->documentFeedback->write(Thoughts::DocumentFeedback::resolve(current, resolved, Thoughts::Platform::reviewOriginId()), &error)) {
        d->messageWindow->display(i18n("The feedback could not be saved: %1", error), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    return true;
}

bool PageView::deleteDocumentFeedback(const QString &feedbackId)
{
    const QJsonObject current = d->documentFeedback->record(feedbackId);
    if (current.isEmpty() || current.value(QStringLiteral("deleted")).toBool()) return false;
    const QJsonObject tombstone = Thoughts::DocumentFeedback::remove(current, Thoughts::Platform::reviewOriginId());
    QString error;
    if (!d->documentFeedback->write(tombstone, &error)) {
        d->messageWindow->display(i18n("The feedback could not be deleted: %1", error), QString(), PageViewMessage::Error, 5000);
        return false;
    }
    showFeedbackUndo([this, tombstone](QString *error) {
        const QJsonObject latest = d->documentFeedback->record(tombstone.value(QStringLiteral("feedback_id")).toString());
        return d->documentFeedback->write(Thoughts::DocumentFeedback::undo(latest.isEmpty() ? tombstone : latest, Thoughts::Platform::reviewOriginId()), error);
    });
    return true;
}

bool PageView::openDocumentFeedbackAt(const QPointF &contentPoint)
{
    if (!drawingsVisible() || d->documentFeedback->records().isEmpty() || hiddenAnswerArea().contains(contentPoint)) return false;
    d->documentFeedback->setShownKinds(Okular::Settings::highlightComments(), Okular::Settings::highlightFeedback());
    const PageViewItem *item = pickItemOnPoint(int(contentPoint.x()), int(contentPoint.y()));
    if (!item) return false;
    const QString hit = d->documentFeedback->hitTest(item->pageNumber() + 1, contentPoint, [this](int page, const QRectF &rect) { return reviewFeedbackRect(page, rect); });
    return !hit.isEmpty() && openDocumentFeedbackEditor(hit);
}

// Like, Dislike and Comment… lead the selection menu while a review answer is visible and the
// selection lies inside it (§6.2); otherwise, on a Library PDF with a known document ID, they
// give document feedback (document-feedback.md §5.1). One selection never creates both.
void PageView::addReviewFeedbackActions(QMenu *menu)
{
    if (d->passageChange) {
        // Choosing the new passage: only Save passage and Cancel.
        QAction *save = menu->addAction(i18n("Save passage"));
        save->setObjectName(QStringLiteral("FeedbackSavePassage"));
        connect(save, &QAction::triggered, this, [this] { savePassageChange(); });
        QAction *cancel = menu->addAction(i18n("Cancel"));
        cancel->setObjectName(QStringLiteral("FeedbackCancelPassage"));
        connect(cancel, &QAction::triggered, this, [this] { cancelPassageChange(); });
        menu->addSeparator();
        return;
    }
    if (!reviewFeedbackSelectionQualifies()) {
        if (!documentFeedbackSelectionQualifies()) return;
        QAction *like = menu->addAction(i18n("Like"));
        like->setObjectName(QStringLiteral("DocumentFeedbackLike"));
        connect(like, &QAction::triggered, this, [this] { addDocumentFeedbackFromSelection(Thoughts::ReviewCardFeedback::positive()); });
        QAction *dislike = menu->addAction(i18n("Dislike"));
        dislike->setObjectName(QStringLiteral("DocumentFeedbackDislike"));
        connect(dislike, &QAction::triggered, this, [this] { addDocumentFeedbackFromSelection(Thoughts::ReviewCardFeedback::negative()); });
        QAction *question = menu->addAction(i18n("Question…"));
        question->setObjectName(QStringLiteral("DocumentFeedbackQuestion"));
        connect(question, &QAction::triggered, this, [this] { openNewDocumentFeedbackEditor(Thoughts::DocumentFeedback::question()); });
        QAction *comment = menu->addAction(i18n("Comment…"));
        comment->setObjectName(QStringLiteral("DocumentFeedbackComment"));
        connect(comment, &QAction::triggered, this, [this] { openNewDocumentFeedbackEditor(Thoughts::DocumentFeedback::other()); });
        menu->addSeparator();
        return;
    }
    QAction *like = menu->addAction(i18n("Like"));
    like->setObjectName(QStringLiteral("ReviewFeedbackLike"));
    connect(like, &QAction::triggered, this, [this] { addReviewFeedbackFromSelection(Thoughts::ReviewCardFeedback::positive()); });
    QAction *dislike = menu->addAction(i18n("Dislike"));
    dislike->setObjectName(QStringLiteral("ReviewFeedbackDislike"));
    connect(dislike, &QAction::triggered, this, [this] { addReviewFeedbackFromSelection(Thoughts::ReviewCardFeedback::negative()); });
    if (documentQuestionSelectionQualifies()) {
        // A question about the passage, as outside the review: it stays with the document.
        QAction *question = menu->addAction(i18n("Question…"));
        question->setObjectName(QStringLiteral("DocumentFeedbackQuestion"));
        connect(question, &QAction::triggered, this, [this] { openNewDocumentFeedbackEditor(Thoughts::DocumentFeedback::question()); });
    }
    QAction *comment = menu->addAction(i18n("Comment…"));
    comment->setObjectName(QStringLiteral("ReviewFeedbackComment"));
    connect(comment, &QAction::triggered, this, [this] { openNewReviewFeedbackEditor(); });
    menu->addSeparator();
}

void PageView::showFeedbackUndo(const std::function<bool(QString *error)> &write, const QString &labelText)
{
    if (!isVisible()) {
        bool taken = false;
        Q_EMIT feedbackUndoOffered(write, &taken);
        if (taken) return;
    }
    if (d->reviewFeedbackBanner) d->reviewFeedbackBanner->deleteLater();
    auto *banner = new QFrame(viewport());
    banner->setObjectName(QStringLiteral("reviewFeedbackUndoBanner"));
    banner->setAttribute(Qt::WA_StyledBackground, true);
    banner->setStyleSheet(QStringLiteral("QFrame#reviewFeedbackUndoBanner { background: palette(tool-tip-base); border: 1px solid palette(mid); border-radius: 8px; }"));
    auto *layout = new QHBoxLayout(banner);
    layout->setContentsMargins(12, 6, 8, 6);
    layout->setSpacing(8);
    auto *label = new QLabel(labelText.isEmpty() ? i18n("Feedback deleted") : labelText, banner);
    label->setObjectName(QStringLiteral("reviewFeedbackUndoLabel"));
    layout->addWidget(label);
    auto *separator = new QLabel(QStringLiteral("·"), banner);
    layout->addWidget(separator);
    auto *undo = new QPushButton(i18n("Undo"), banner);
    undo->setObjectName(QStringLiteral("reviewFeedbackUndoButton"));
    undo->setFlat(true);
    undo->setAutoDefault(false);
    layout->addWidget(undo);
    connect(undo, &QPushButton::clicked, this, [this, write, banner] {
        QString error;
        if (!write(&error))
            d->messageWindow->display(i18n("The feedback could not be restored: %1", error), QString(), PageViewMessage::Error, 5000);
        banner->deleteLater();
    });
    banner->adjustSize();
    banner->move((viewport()->width() - banner->width()) / 2, std::max(0, viewport()->height() - banner->height() - 24));
    banner->show();
    banner->raise();
    d->reviewFeedbackBanner = banner;
    QTimer::singleShot(5000, banner, &QObject::deleteLater);
}

void PageView::reloadAgentProposalOverlays(bool keepCurrent)
{
    const QUrl documentUrl = d->document->currentDocument();
    const bool sameDocument = keepCurrent && documentUrl == d->agentProposalOverlaysUrl;
    if (sameDocument && d->agentProposalReloadRunning) {
        d->agentProposalReloadAgain = true;
        return;
    }
    if (d->agentProposalReloadClock.isValid()) {
        ++d->agentProposalReloadsSuperseded;
    } else {
        d->agentProposalReloadClock.start();
        d->agentProposalReloadsSuperseded = 0;
    }
    d->agentProposalOverlaysUrl = documentUrl;
    d->agentProposalReloadAgain = false;
    if (!sameDocument || !d->agentProposalOverlaysReady) {
        d->agentProposalOverlays.clear();
        d->selectedAgentProposalId.clear();
        d->agentProposalOverlaysReady = false;
        d->builtAgentProposalIds.clear();
        d->builtAgentProposalIdsKnown = false;
    }
    if (d->docsRoot.isEmpty() || !documentUrl.isLocalFile()) {
        d->agentProposalOverlays.clear();
        d->agentProposalOverlaysReady = true;
        d->agentProposalReloadRunning = false;
        d->agentProposalReloadClock.invalidate();
        Q_EMIT agentProposalOverlaysReloaded();
        return;
    }
    d->agentProposalReloadRunning = true;
    viewport()->update();

    const QString docsRoot = d->docsRoot;
    // The bytes on screen (an earlier version kept while a newer one is ready: its own file).
    const QString openedPath = d->document->metaData(QStringLiteral("OpenedFilePath")).toString();
    const QString pdfPath = openedPath.isEmpty() ? documentUrl.toLocalFile() : openedPath;
    const int pageCount = static_cast<int>(d->document->pages());
    const quint64 generation = ++d->agentProposalReloadGeneration;
    const QPointer<PageView> guard(this);
    QThreadPool::globalInstance()->start([guard, documentUrl, docsRoot, pdfPath, pageCount, generation] {
        QList<Thoughts::AgentProposalOverlay> overlays;
        QSet<QString> builtIds;
        bool builtIdsKnown = false;
        const QFileInfo pdfInfo(pdfPath);
        QFile pdfFile(pdfInfo.absoluteFilePath());
        if (pdfFile.open(QIODevice::ReadOnly)) {
            QCryptographicHash pdfHash(QCryptographicHash::Sha256);
            if (pdfHash.addData(&pdfFile)) {
                // Sync may replace these files meanwhile, which Windows refuses
                // while they are open; everything below works on copies.
                pdfFile.close();
                const QString pdfSha256 = QString::fromLatin1(pdfHash.result().toHex());
                QDir bundleRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")));
                if (!QFileInfo::exists(bundleRoot.filePath(QStringLiteral("documents")))) {
                    bundleRoot = QDir(QDir(docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
                }
                const QSet<QString> locallyResolved = Thoughts::locallyResolvedAgentProposalIds(bundleRoot.absolutePath());
                QFile pointerFile(bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(pdfInfo.fileName())));
                if (pointerFile.open(QIODevice::ReadOnly)) {
                    const QJsonObject pointer = QJsonDocument::fromJson(pointerFile.readAll()).object();
                    pointerFile.close();
                    const QString artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
                    if (pointer.value(QStringLiteral("schema_version")).toInt() == 1 && artifactSha256.size() == 64) {
                        QFile manifestFile(bundleRoot.filePath(QStringLiteral("objects/%1").arg(artifactSha256)));
                        if (manifestFile.open(QIODevice::ReadOnly)) {
                            const QByteArray manifestContents = manifestFile.readAll();
                            manifestFile.close();
                            if (QCryptographicHash::hash(manifestContents, QCryptographicHash::Sha256).toHex() == artifactSha256.toLatin1()) {
                                const QJsonObject manifest = QJsonDocument::fromJson(manifestContents).object();
                                const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
                                const bool pdfMatches = manifest.value(QStringLiteral("kind")).toString() == QLatin1String("thoughts-document-source-artifact")
                                    && pdf.value(QStringLiteral("filename")).toString().compare(pdfInfo.fileName(), Qt::CaseInsensitive) == 0
                                    && pdf.value(QStringLiteral("pdf_sha256")).toString() == pdfSha256;
                                if (pdfMatches) {
                                    QHash<QString, QJsonObject> proposalsById;
                                    for (const QJsonValue &value : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
                                        const QJsonObject proposal = value.toObject();
                                        proposalsById.insert(proposal.value(QStringLiteral("id")).toString(), proposal);
                                        builtIds.insert(proposal.value(QStringLiteral("id")).toString());
                                    }
                                    builtIdsKnown = manifest.value(QStringLiteral("agent_proposals")).isArray();
                                    const bool hasV2Correspondence = manifest.value(QStringLiteral("review_correspondence_schema_version")).toInt() == 2;
                                    if (hasV2Correspondence) {
                                        for (const QJsonValue &value : manifest.value(QStringLiteral("review_correspondences")).toArray()) {
                                            const QJsonObject correspondence = value.toObject();
                                            if (correspondence.value(QStringLiteral("kind")).toString() != QLatin1String("agent_edit")) {
                                                continue;
                                            }
                                            const QString id = correspondence.value(QStringLiteral("item_id")).toString();
                                            const QJsonObject proposal = proposalsById.value(id);
                                            if (id.isEmpty() || locallyResolved.contains(id) || proposal.value(QStringLiteral("suppressed")).toBool()) {
                                                continue;
                                            }
                                            if (manifest.value(QStringLiteral("review_placement_model_version")).toInt() >= 3
                                                && !Thoughts::validCompilerReviewCorrespondence(correspondence, pdfInfo.fileName(), pdfSha256, pageCount)) {
                                                continue;
                                            }
                                            for (const QJsonValue &locationValue : correspondence.value(QStringLiteral("locations")).toArray()) {
                                                const QJsonObject location = locationValue.toObject();
                                                if (location.value(QStringLiteral("pdf_filename")).toString().compare(pdfInfo.fileName(), Qt::CaseInsensitive) != 0
                                                    || location.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256) {
                                                    continue;
                                                }
                                                if (location.value(QStringLiteral("proof")).toObject().value(QStringLiteral("resolver_version")).toInt() >= 3
                                                    && !Thoughts::validCompilerReviewLocation(location, correspondence.value(QStringLiteral("review_key")).toString(), pdfInfo.fileName(), pdfSha256, pageCount, correspondence)) {
                                                    continue;
                                                }
                                                const int page = location.value(QStringLiteral("page_index")).toInt(-1);
                                                const QString role = location.value(QStringLiteral("role")).toString(QStringLiteral("after"));
                                                const bool duplicate = location.value(QStringLiteral("duplicate")).toBool();
                                                if (page < 0 || page >= pageCount) {
                                                    continue;
                                                }
                                                for (const QJsonValue &regionValue : location.value(QStringLiteral("regions")).toArray()) {
                                                    const QJsonArray region = regionValue.toArray();
                                                    if (region.size() != 4) {
                                                        continue;
                                                    }
                                                    const QRectF rect(region.at(0).toDouble(),
                                                                      region.at(1).toDouble(),
                                                                      region.at(2).toDouble() - region.at(0).toDouble(),
                                                                      region.at(3).toDouble() - region.at(1).toDouble());
                                                    if (rect.isValid()) {
                                                        overlays.append({id,
                                                                         role,
                                                                         page,
                                                                         rect,
                                                                         Thoughts::AgentProposalOverlayStyle::Fragment,
                                                                         true,
                                                                         duplicate});
                                                    }
                                                }
                                                const QJsonArray point = location.value(QStringLiteral("point")).toArray();
                                                if (point.size() == 2) {
                                                    const QPointF center(point.at(0).toDouble(), point.at(1).toDouble());
                                                    overlays.append({id,
                                                                     role,
                                                                     page,
                                                                     QRectF(center.x() - 0.004, center.y() - 0.004, 0.008, 0.008),
                                                                     Thoughts::AgentProposalOverlayStyle::Region,
                                                                     true,
                                                                     duplicate});
                                                }
                                            }
                                        }
                                    } else {
                                        for (const QJsonValue &value : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
                                        const QJsonObject proposal = value.toObject();
                                        if (locallyResolved.contains(proposal.value(QStringLiteral("id")).toString()) || proposal.value(QStringLiteral("suppressed")).toBool()) {
                                            continue;
                                        }
                                        const QString id = proposal.value(QStringLiteral("id")).toString();
                                        const QJsonObject placement = proposal.value(QStringLiteral("placement")).toObject();
                                        const auto appendFragments = [&](const QJsonArray &fragments, const QString &role) {
                                            QString preview = proposal.value(role).toObject().value(QStringLiteral("preview")).toString();
                                            if (preview.isEmpty()) {
                                                preview = proposal.value(QStringLiteral("body_preview")).toString();
                                            }
                                            overlays.append(Thoughts::agentProposalOverlays(id, role, preview, fragments, pdfInfo.fileName(), pageCount));
                                        };
                                        if (placement.value(QStringLiteral("regions")).isArray()) {
                                            for (const QJsonValue &regionValue : placement.value(QStringLiteral("regions")).toArray()) {
                                                const QJsonObject region = regionValue.toObject();
                                                appendFragments(region.value(QStringLiteral("quad_fragments")).toArray(), region.value(QStringLiteral("role")).toString());
                                            }
                                        } else {
                                            appendFragments(placement.value(QStringLiteral("quad_fragments")).toArray(), QStringLiteral("before"));
                                        }
                                    }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, documentUrl, generation, overlays = std::move(overlays), builtIds = std::move(builtIds), builtIdsKnown]() mutable {
                if (!guard || guard->d->agentProposalReloadGeneration != generation || guard->d->document->currentDocument() != documentUrl) {
                    return;
                }
                guard->d->agentProposalReloadRunning = false;
                guard->d->builtAgentProposalIds = std::move(builtIds);
                guard->d->builtAgentProposalIdsKnown = builtIdsKnown;
                overlays.removeIf([guard](const Thoughts::AgentProposalOverlay &overlay) {
                    return guard->d->suppressedAgentProposalIds.contains(overlay.id) || guard->d->decidedAgentProposalIds.contains(overlay.id);
                });
                QList<QSizeF> pageSizes;
                for (uint page = 0; page < guard->d->document->pages(); ++page) {
                    const Okular::Page *pdfPage = guard->d->document->page(page);
                    pageSizes.append(QSizeF(pdfPage->width(), pdfPage->height()));
                }
                guard->d->agentProposalOverlays = Thoughts::mergeAgentProposalOverlayLines(std::move(overlays), pageSizes);
                guard->d->agentProposalOverlaysReady = true;
                if (guard->d->agentProposalReloadClock.isValid()) {
                    Q_EMIT guard->agentProposalOverlaysTimed(guard->d->agentProposalReloadClock.elapsed(), int(guard->d->agentProposalOverlays.size()),
                                                             guard->d->agentProposalReloadsSuperseded);
                    guard->d->agentProposalReloadClock.invalidate();
                }
                // A toolbar stays only if its marker is still where it was.
                guard->checkAgentProposalHoverAnchor();
                guard->viewport()->update();
                Q_EMIT guard->agentProposalOverlaysReloaded();
                if (guard && std::exchange(guard->d->agentProposalReloadAgain, false)) {
                    guard->reloadAgentProposalOverlays(true);
                }
            },
            Qt::QueuedConnection);
    });
}

void PageView::paintAgentProposalOverlays(QPainter *painter, const QRect &contentsRect) const
{
    if (!Okular::Settings::highlightAgentEdits() || d->agentProposalOverlays.isEmpty()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (const Thoughts::AgentProposalOverlay &overlay : std::as_const(d->agentProposalOverlays)) {
        if (d->suppressedAgentProposalIds.contains(overlay.id)) {
            continue;
        }
        const PageViewItem *pageItem = overlay.page >= 0 && overlay.page < d->items.size() ? d->items.at(overlay.page) : nullptr;
        if (!pageItem || !pageItem->isVisible() || pageItem->page()->width() <= 0 || pageItem->page()->height() <= 0) {
            continue;
        }
        const QPoint origin = pageItem->uncroppedGeometry().topLeft();
        const QRectF normalizedRect = overlay.normalized
            ? overlay.rect
            : QRectF(overlay.rect.left() / pageItem->page()->width(),
                     overlay.rect.top() / pageItem->page()->height(),
                     overlay.rect.width() / pageItem->page()->width(),
                     overlay.rect.height() / pageItem->page()->height());
        const QRectF rect(origin.x() + normalizedRect.left() * pageItem->uncroppedWidth(),
                          origin.y() + normalizedRect.top() * pageItem->uncroppedHeight(),
                          normalizedRect.width() * pageItem->uncroppedWidth(),
                          normalizedRect.height() * pageItem->uncroppedHeight());
        if (!rect.intersects(contentsRect)) {
            continue;
        }
        const bool after = overlay.role == QLatin1String("after");
        if (overlay.style == Thoughts::AgentProposalOverlayStyle::Region) {
            QRectF regionRect = rect.adjusted(-3.0, -3.0, 3.0, 3.0);
            QColor regionColor = after ? QColor(35, 155, 75) : QColor(205, 35, 35);
            QColor regionFill = regionColor;
            regionFill.setAlpha(18);
            regionColor.setAlpha(220);
            QPen regionPen(regionColor, 2.4);
            regionPen.setCosmetic(true);
            if (!after) {
                regionPen.setStyle(Qt::DashLine);
            }
            painter->setPen(regionPen);
            painter->setBrush(regionFill);
            painter->drawRoundedRect(regionRect, 5.0, 5.0);
            continue;
        }
        painter->fillRect(rect, after ? QColor(35, 175, 80, 72) : QColor(220, 35, 35, 72));
        if (!after) {
            QPen strikePen(QColor(210, 25, 25), 2.4);
            strikePen.setCosmetic(true);
            painter->setPen(strikePen);
            painter->drawLine(QPointF(rect.left(), rect.center().y()), QPointF(rect.right(), rect.center().y()));
        }
    }
    const QString hoveredMarker = d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible() ? d->agentProposalHoverBar->proposalId() : QString();
    for (const auto &marker : agentProposalMarkers()) {
        if (!marker.second.intersects(contentsRect)) continue;
        // The marker whose toolbar is open stands out from its neighbours.
        const bool hovered = marker.first == hoveredMarker;
        painter->setPen(QPen(palette().color(QPalette::Highlight), hovered ? 2.0 : 1.0));
        QColor fill = palette().color(QPalette::Base);
        if (hovered) {
            QColor tint = palette().color(QPalette::Highlight);
            fill = QColor::fromRgbF(fill.redF() * 0.8 + tint.redF() * 0.2, fill.greenF() * 0.8 + tint.greenF() * 0.2, fill.blueF() * 0.8 + tint.blueF() * 0.2);
        }
        painter->setBrush(fill);
        painter->drawRoundedRect(marker.second, 4, 4);
        painter->save();
        painter->translate(marker.second.topLeft());
        // A check mark in the 20 px box, in the green of Accept: a click accepts.
        painter->setPen(QPen(QColor(35, 155, 75), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath check;
        check.moveTo(5, 10.5);
        check.lineTo(8.5, 14);
        check.lineTo(15, 6.5);
        painter->drawPath(check);
        painter->restore();
    }
    painter->restore();
}

QList<QPair<QString, QRectF>> PageView::agentProposalMarkers() const
{
    QList<QPair<QString, QRectF>> markers;
    if (!Okular::Settings::highlightAgentEdits()) return markers;
    // Select a stable document-wide anchor before applying layout visibility:
    // the proposal's earliest own occurrence. LaTeX-made copies (contents
    // entries, running heads) keep their highlight but anchor the marker only
    // when the proposal has no other fragment.
    QList<QSizeF> pageSizes;
    for (const PageViewItem *item : std::as_const(d->items)) {
        pageSizes.append(QSizeF(item->page()->width(), item->page()->height()));
    }
    QList<const Thoughts::AgentProposalOverlay *> firstFragments;
    const auto anchors = Thoughts::agentProposalMarkerAnchors(d->agentProposalOverlays, pageSizes);
    for (const qsizetype index : anchors) {
        firstFragments.append(&d->agentProposalOverlays.at(index));
    }
    QMap<QPair<int, QString>, QPointF> starts;
    for (const auto *fragment : std::as_const(firstFragments)) {
        const auto &overlay = *fragment;
        if (d->suppressedAgentProposalIds.contains(overlay.id) || overlay.page < 0 || overlay.page >= d->items.size()) continue;
        const auto *item = d->items.at(overlay.page);
        if (!item->isVisible() || item->page()->width() <= 0 || item->page()->height() <= 0) continue;
        const QPoint origin = item->uncroppedGeometry().topLeft();
        const qreal scaleX = item->uncroppedWidth() / (overlay.normalized ? 1.0 : item->page()->width());
        const qreal scaleY = item->uncroppedHeight() / (overlay.normalized ? 1.0 : item->page()->height());
        const QPointF start(origin.x() + overlay.rect.left() * scaleX, origin.y() + overlay.rect.top() * scaleY);
        if (!QRectF(start, QSizeF(overlay.rect.width() * scaleX, overlay.rect.height() * scaleY)).intersects(item->croppedGeometry())) continue;
        const auto key = qMakePair(overlay.page, overlay.id);
        const auto previous = starts.constFind(key);
        if (previous == starts.cend() || start.y() < previous->y() || (start.y() == previous->y() && start.x() < previous->x())) starts.insert(key, start);
    }
    // One marker per proposal, shared by all pages and before/after fragments.
    constexpr qreal size = 20;
    for (auto start = starts.cbegin(); start != starts.cend(); ++start) {
        const QRect page = d->items.at(start.key().first)->croppedGeometry();
        if (page.width() < size + 4 || page.height() < size + 4) continue;
        QRectF marker(qBound(qreal(page.left() + 2), start->x(), qreal(page.right() - size - 2)),
                      qMax(qreal(page.top() + 2), start->y() - size - 4), size, size);
        for (const auto &placed : std::as_const(markers)) {
            if (marker.adjusted(-2, -2, 2, 2).intersects(placed.second) && placed.second.right() + size + 4 < page.right()) {
                marker.moveLeft(placed.second.right() + 4);
            }
        }
        markers.append(qMakePair(start.key().second, marker));
    }
    return markers;
}

QString PageView::agentProposalMarkerAt(const QPoint &eventPos) const
{
    for (const auto &marker : agentProposalMarkers()) {
        if (marker.second.contains(eventPos)) return marker.first;
    }
    return {};
}

QRect PageView::agentProposalMarkerRect(const QString &proposalId) const
{
    for (const auto &marker : agentProposalMarkers()) {
        if (marker.first == proposalId) return marker.second.toAlignedRect().translated(-contentAreaPosition());
    }
    return {};
}

void PageView::setAgentProposalDecisionStateProvider(std::function<AgentProposalDecisionState(const QString &)> provider)
{
    d->agentProposalDecisionStateProvider = std::move(provider);
}

AgentProposalDecisionState PageView::agentProposalDecisionState(const QString &proposalId) const
{
    if (d->agentProposalDecisionStateProvider) return d->agentProposalDecisionStateProvider(proposalId);
    AgentProposalDecisionState state;
    state.reason = i18n("This proposal cannot be decided here. Open it to see why.");
    return state;
}

void PageView::updateAgentProposalHover(const QPoint &viewportPos, Qt::MouseButtons buttons)
{
    d->agentProposalHoverPointer = viewportPos;
    d->agentProposalHoverPointerValid = true;
    const bool visible = d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible();
    if (buttons != Qt::NoButton || (d->annotator && d->annotator->active()) || d->items.isEmpty()) {
        // Dragging, selecting or annotating: no toolbar.
        d->agentProposalHoverShowTimer->stop();
        d->agentProposalHoverCandidate.clear();
        if (visible && buttons != Qt::NoButton) {
            hideAgentProposalHoverBar();
        }
        return;
    }
    const QString proposalId = agentProposalMarkerAt(contentAreaPoint(viewportPos));
    if (!proposalId.isEmpty()) {
        if (visible && d->agentProposalHoverBar->proposalId() == proposalId) {
            d->agentProposalHoverHideTimer->stop();
            return;
        }
        // Another marker (proposals close together): its own toolbar.
        if (visible) {
            hideAgentProposalHoverBar();
        }
        if (d->agentProposalHoverCandidate != proposalId || !d->agentProposalHoverShowTimer->isActive()) {
            d->agentProposalHoverCandidate = proposalId;
            d->agentProposalHoverShowTimer->start();
        }
        return;
    }
    d->agentProposalHoverCandidate.clear();
    d->agentProposalHoverShowTimer->stop();
    if (!visible) {
        return;
    }
    if (agentProposalHoverBridgeContains(viewportPos)) {
        d->agentProposalHoverHideTimer->stop();
    } else if (!d->agentProposalHoverHideTimer->isActive()) {
        d->agentProposalHoverHideTimer->start();
    }
}

void PageView::showAgentProposalHoverBar()
{
    const QString proposalId = std::exchange(d->agentProposalHoverCandidate, {});
    if (proposalId.isEmpty() || !d->agentProposalHoverPointerValid || (d->annotator && d->annotator->active())
        || QApplication::mouseButtons() != Qt::NoButton) {
        return;
    }
    // The page scrolled under a resting pointer: not a hover the user meant.
    if (d->agentProposalLastScroll.isValid() && d->agentProposalLastScroll.elapsed() < AgentProposalHoverScrollQuietMs) {
        return;
    }
    if (agentProposalMarkerAt(contentAreaPoint(d->agentProposalHoverPointer)) != proposalId) {
        return;
    }
    showAgentProposalHoverBarFor(proposalId);
}

void PageView::showAgentProposalHoverBarFor(const QString &proposalId)
{
    d->agentProposalHoverShowTimer->stop();
    d->agentProposalHoverCandidate.clear();
    if (d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible() && d->agentProposalHoverBar->proposalId() != proposalId) {
        hideAgentProposalHoverBar();
    }
    const QRect marker = agentProposalMarkerRect(proposalId);
    if (marker.isEmpty()) {
        return;
    }
    if (!d->agentProposalHoverBar) {
        d->agentProposalHoverBar = new AgentProposalHoverBar(viewport());
        connect(d->agentProposalHoverBar, &AgentProposalHoverBar::openRequested, this, [this](const QString &id) {
            hideAgentProposalHoverBar();
            d->selectedAgentProposalId = id;
            viewport()->update();
            Q_EMIT agentProposalReviewRequested(id);
        });
        connect(d->agentProposalHoverBar, &AgentProposalHoverBar::resolutionRequested, this, [this](const QString &id, const QString &action) {
            // The same decision as the proposal window's buttons.
            hideAgentProposalHoverBar();
            Q_EMIT agentProposalResolutionRequested(id, action);
        });
        connect(d->agentProposalHoverBar, &AgentProposalHoverBar::pointerEntered, this, [this] { d->agentProposalHoverHideTimer->stop(); });
        connect(d->agentProposalHoverBar, &AgentProposalHoverBar::pointerLeft, this, [this] {
            if (d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible()) {
                d->agentProposalHoverHideTimer->start();
            }
        });
    }
    const AgentProposalDecisionState state = agentProposalDecisionState(proposalId);
    AgentProposalHoverBar *bar = d->agentProposalHoverBar;
    bar->setProposal(proposalId, state);
    const QSize size = bar->sizeHint();
    // Just above the marker, left edges aligned, so the marker row and the
    // proposal's own text stay visible; below it when there is no room above.
    constexpr int gap = 2;
    int y = marker.top() - gap - size.height();
    if (y < 0) {
        y = marker.bottom() + 1 + gap;
    }
    const int x = qBound(0, marker.left(), qMax(0, viewport()->width() - size.width()));
    bar->setGeometry(QRect(QPoint(x, y), size));
    d->agentProposalHoverMarker = marker;
    d->agentProposalHoverPage = static_cast<int>(d->document->currentPage());
    d->agentProposalHoverHideTimer->stop();
    bar->show();
    bar->raise();
    viewport()->update(marker.adjusted(-2, -2, 2, 2));
}

bool PageView::agentProposalHoverBridgeContains(const QPoint &viewportPos) const
{
    if (!d->agentProposalHoverBar || !d->agentProposalHoverBar->isVisible()) {
        return false;
    }
    // The marker, the toolbar and the strip between them, with a little slack.
    const QRect bar = d->agentProposalHoverBar->geometry();
    const QRect marker = d->agentProposalHoverMarker;
    const int top = qMin(marker.top(), bar.top());
    const int bottom = qMax(marker.bottom(), bar.bottom());
    const QRect strip(marker.left(), top, marker.width(), bottom - top + 1);
    constexpr int slack = 4;
    return bar.adjusted(-slack, -slack, slack, slack).contains(viewportPos) || strip.adjusted(-slack, -slack, slack, slack).contains(viewportPos);
}

void PageView::hideAgentProposalHoverBar()
{
    if (!d->agentProposalHoverShowTimer) {
        return;
    }
    d->agentProposalHoverShowTimer->stop();
    d->agentProposalHoverHideTimer->stop();
    d->agentProposalHoverCandidate.clear();
    if (d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible()) {
        if (d->agentProposalHoverBar->hasFocus() || d->agentProposalHoverBar->isAncestorOf(QApplication::focusWidget())) {
            setFocus();
        }
        d->agentProposalHoverBar->hide();
        viewport()->update(d->agentProposalHoverMarker.adjusted(-2, -2, 2, 2));
    }
    d->agentProposalHoverMarker = QRect();
}

void PageView::checkAgentProposalHoverAnchor()
{
    if (!d->agentProposalHoverBar || !d->agentProposalHoverBar->isVisible()) {
        return;
    }
    if (agentProposalMarkerRect(d->agentProposalHoverBar->proposalId()) != d->agentProposalHoverMarker) {
        hideAgentProposalHoverBar();
    }
}

QString PageView::agentProposalAt(const PageViewItem *pageItem, const QPoint &eventPos) const
{
    if (!Okular::Settings::highlightAgentEdits()) {
        return {};
    }
    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextSelect) {
        return {};
    }
    if (!pageItem || pageItem->page()->width() <= 0 || pageItem->page()->height() <= 0) {
        return {};
    }
    for (const Thoughts::AgentProposalOverlay &overlay : std::as_const(d->agentProposalOverlays)) {
        if (overlay.page != pageItem->pageNumber() || d->suppressedAgentProposalIds.contains(overlay.id)) {
            continue;
        }
        const QPoint origin = pageItem->uncroppedGeometry().topLeft();
        const QRectF normalizedRect = overlay.normalized
            ? overlay.rect
            : QRectF(overlay.rect.left() / pageItem->page()->width(),
                     overlay.rect.top() / pageItem->page()->height(),
                     overlay.rect.width() / pageItem->page()->width(),
                     overlay.rect.height() / pageItem->page()->height());
        QRectF overlayRect(origin.x() + normalizedRect.left() * pageItem->uncroppedWidth(),
                           origin.y() + normalizedRect.top() * pageItem->uncroppedHeight(),
                           normalizedRect.width() * pageItem->uncroppedWidth(),
                           normalizedRect.height() * pageItem->uncroppedHeight());
        overlayRect.adjust(-4.0, -4.0, 4.0, 4.0);
        if (overlayRect.contains(eventPos)) {
            return overlay.id;
        }
    }
    return {};
}

QVariantMap PageView::reviewDocumentPayload(const QString &shownPath) const
{
    QVariantMap result;
    const QUrl url = d->document->currentDocument();
    if (!url.isLocalFile()) {
        result.insert(QStringLiteral("error"), i18n("Open a local PDF before generating review cards."));
        return result;
    }
    const QFileInfo info(shownPath.isEmpty() ? url.toLocalFile() : shownPath);
    if (info.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0 || d->document->pages() == 0) {
        result.insert(QStringLiteral("error"), i18n("The current document is not a readable PDF."));
        return result;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        result.insert(QStringLiteral("error"), i18n("The PDF fingerprint could not be read."));
        return result;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        result.insert(QStringLiteral("error"), i18n("The PDF fingerprint could not be calculated."));
        return result;
    }
    result.insert(QStringLiteral("pdf_path"), info.absoluteFilePath());
    result.insert(QStringLiteral("pdf_stem"), info.completeBaseName());
    result.insert(QStringLiteral("pdf_sha256"), QString::fromLatin1(hash.result().toHex()));
    result.insert(QStringLiteral("page_count"), static_cast<int>(d->document->pages()));
    return result;
}

bool PageView::showReviewMask(int page, const QVariantList &rects)
{
    QVariantList regions;
    for (const QVariant &rect : rects) {
        regions.append(QVariantMap{{QStringLiteral("page"), page}, {QStringLiteral("rect"), rect}});
    }
    return showReviewMasks(regions);
}

void PageView::setReviewCardHighlights(const QVariantList &cards)
{
    d->m_reviewCardHighlightRects.clear();
    for (const QVariant &card : cards) {
        QMap<int, QList<QRectF>> pages;
        for (const QVariant &value : card.toList()) {
            const QVariantMap region = value.toMap();
            const int page = region.value(QStringLiteral("page")).toInt() - 1;
            const QJsonArray rect = QJsonArray::fromVariantList(region.value(QStringLiteral("rect")).toList());
            if (page < 0 || page >= static_cast<int>(d->document->pages())
                || !Thoughts::validNormalizedReviewCoordinates(rect, 4)) continue;
            pages[page].append(QRectF(QPointF(rect.at(0).toDouble(), rect.at(1).toDouble()), QPointF(rect.at(2).toDouble(), rect.at(3).toDouble())));
        }
        // Join word fragments within each card, preserving gaps between different answers.
        for (auto page = pages.begin(); page != pages.end(); ++page) {
            const auto *pdfPage = d->document->page(page.key());
            for (const auto &r : Thoughts::normalizedTextLineRects(std::move(page.value()), QSizeF(pdfPage->width(), pdfPage->height()))) {
                d->m_reviewCardHighlightRects[page.key()].append(Okular::NormalizedRect(r.left(), r.top(), r.right(), r.bottom()));
            }
        }
    }
    viewport()->update();
}

bool PageView::showReviewMasks(const QVariantList &regions, ReviewMaskStyle style, bool navigate)
{
    QMap<int, QList<Okular::NormalizedRect>> validated;
    if (regions.isEmpty()) return false;
    for (const QVariant &value : regions) {
        const QVariantMap region = value.toMap();
        const int page = region.value(QStringLiteral("page")).toInt() - 1;
        const QJsonArray rect = QJsonArray::fromVariantList(region.value(QStringLiteral("rect")).toList());
        if (page < 0 || page >= static_cast<int>(d->document->pages())
            || !Thoughts::validNormalizedReviewCoordinates(rect, 4)) return false;
        validated[page].append(Okular::NormalizedRect(rect.at(0).toDouble(), rect.at(1).toDouble(), rect.at(2).toDouble(), rect.at(3).toDouble()));
    }
    for (auto page = validated.begin(); page != validated.end(); ++page) {
        const auto *pdfPage = d->document->page(page.key());
        if (style == ReviewMaskStyle::Hidden) {
            // The page's own text bounds how far a table answer's mask grows.
            if (!pdfPage->hasTextPage()) d->document->requestTextPage(page.key());
            QList<Okular::NormalizedRect> pageWords;
            for (const Okular::TextEntity &word : pdfPage->words(nullptr, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour)) pageWords.append(word.area());
            page.value() = Thoughts::continuousReviewMaskBlocks(page.value(), pdfPage->height() / pdfPage->width(), pageWords);
        } else {
            QList<QRectF> rects;
            for (const auto &r : page.value()) rects.append(QRectF(r.left, r.top, r.right - r.left, r.bottom - r.top));
            page.value().clear();
            for (const auto &r : Thoughts::normalizedTextLineRects(std::move(rects), QSizeF(pdfPage->width(), pdfPage->height()))) {
                page.value().append(Okular::NormalizedRect(r.left(), r.top(), r.right(), r.bottom()));
            }
        }
    }
    d->m_reviewMaskRects = validated;
    d->m_reviewMaskStyle = style;
    d->m_reviewMaskPageNumber = validated.firstKey();
    Okular::DocumentViewport reviewViewport(d->m_reviewMaskPageNumber);
    reviewViewport.rePos.enabled = true;
    reviewViewport.rePos.normalizedY = std::max(0.0, validated.first().constFirst().top - 0.12);
    reviewViewport.rePos.pos = Okular::DocumentViewport::TopLeft;
    if (navigate) d->document->setViewport(reviewViewport);
    viewport()->update();
    endPassageChangeIfStale();
    return true;
}

void PageView::clearSidebarReviewHighlight()
{
    if (d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight) clearReviewMask();
}

void PageView::clearReviewMask()
{
    // No card is under review any more: its feedback is not shown elsewhere.
    d->reviewFeedback->clear();
    if (d->m_reviewMaskPageNumber < 0 && d->m_reviewMaskRects.isEmpty()) {
        d->m_reviewMaskInverted = false;
        return;
    }
    d->m_reviewMaskPageNumber = -1;
    d->m_reviewMaskRects.clear();
    d->m_reviewMaskInverted = false;
    viewport()->update();
    endPassageChangeIfStale();
}

void PageView::revealReviewMask()
{
    if (d->m_reviewMaskStyle != ReviewMaskStyle::Hidden || d->m_reviewMaskRects.isEmpty()) return;
    d->m_reviewMaskStyle = ReviewMaskStyle::Revealed;
    d->m_reviewMaskInverted = false;
    viewport()->update();
}

namespace
{
QList<QPair<int, Okular::NormalizedRect>> reviewMaskBlocks(const QMap<int, QList<Okular::NormalizedRect>> &rects)
{
    QList<QPair<int, Okular::NormalizedRect>> blocks;
    for (auto page = rects.cbegin(); page != rects.cend(); ++page) {
        auto sorted = page.value();
        std::sort(sorted.begin(), sorted.end(), [](const Okular::NormalizedRect &a, const Okular::NormalizedRect &b) {
            return a.top != b.top ? a.top < b.top : a.left < b.left;
        });
        for (const auto &rect : std::as_const(sorted)) blocks.append({page.key(), rect});
    }
    return blocks;
}
}

int PageView::reviewMaskBlockCount() const
{
    // A sidebar highlight is not a card under review.
    if (d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight) return 0;
    return reviewMaskBlocks(d->m_reviewMaskRects).size();
}

bool PageView::focusReviewMaskBlock(int index)
{
    if (d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight) return false;
    const auto blocks = reviewMaskBlocks(d->m_reviewMaskRects);
    if (index < 0 || index >= blocks.size()) return false;
    Okular::DocumentViewport viewport(blocks.at(index).first);
    viewport.rePos.enabled = true;
    viewport.rePos.normalizedY = std::max(0.0, blocks.at(index).second.top - 0.12);
    viewport.rePos.pos = Okular::DocumentViewport::TopLeft;
    d->document->setViewport(viewport);
    return true;
}

void PageView::setReviewMaskInverted(bool inverted)
{
    if (d->m_reviewMaskInverted == inverted) return;
    d->m_reviewMaskInverted = inverted;
    viewport()->update();
}

bool PageView::reviewMaskInverted() const
{
    return d->m_reviewMaskInverted;
}

void PageView::drawTableDividers(QPainter *screenPainter)
{
    if (!d->tableSelectionParts.isEmpty()) {
        screenPainter->setPen(d->mouseSelectionColor.darker());
        if (d->tableDividersGuessed) {
            QPen p = screenPainter->pen();
            p.setStyle(Qt::DashLine);
            screenPainter->setPen(p);
        }
        for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
            QRect selectionPartRect = tsp.rectInItem.geometry(tsp.item->uncroppedWidth(), tsp.item->uncroppedHeight());
            selectionPartRect.translate(tsp.item->uncroppedGeometry().topLeft());
            QRect selectionPartRectInternal = selectionPartRect;
            selectionPartRectInternal.adjust(1, 1, -1, -1);
            for (double col : std::as_const(d->tableSelectionCols)) {
                if (col >= tsp.rectInSelection.left && col <= tsp.rectInSelection.right) {
                    col = (col - tsp.rectInSelection.left) / (tsp.rectInSelection.right - tsp.rectInSelection.left);
                    const int x = selectionPartRect.left() + col * selectionPartRect.width() + 0.5;
                    screenPainter->drawLine(x, selectionPartRectInternal.top(), x, selectionPartRectInternal.top() + selectionPartRectInternal.height());
                }
            }
            for (double row : std::as_const(d->tableSelectionRows)) {
                if (row >= tsp.rectInSelection.top && row <= tsp.rectInSelection.bottom) {
                    row = (row - tsp.rectInSelection.top) / (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
                    const int y = selectionPartRect.top() + row * selectionPartRect.height() + 0.5;
                    screenPainter->drawLine(selectionPartRectInternal.left(), y, selectionPartRectInternal.left() + selectionPartRectInternal.width(), y);
                }
            }
        }
    }
}

namespace
{
// One Read Aloud at a time in the whole app, whichever tab started it.
QPointer<Thoughts::ReadAloud::Controller> s_activeReadAloud;
bool s_cloudFallbackNoticeShown = false;
bool s_addKeyHintShown = false;
}

void PageView::readAloud(const QString &text)
{
    if (Thoughts::ReadAloud::prepareText(text).isEmpty()) {
        d->messageWindow->display(i18n("There is no text to read aloud."), QString(), PageViewMessage::Info, 2500);
        return;
    }
    if (s_activeReadAloud && s_activeReadAloud != d->readAloud) {
        s_activeReadAloud->stop();
    }
    if (!d->readAloud) {
        d->readAloud = new Thoughts::ReadAloud::Controller(this);
        d->readAloudBar = new ReadAloudBar(d->readAloud, this);
        connect(d->readAloud, &Thoughts::ReadAloud::Controller::developerEvent, this, &PageView::readAloudDeveloperEvent);
        // Once per session: without a key, where to add one; a refused key, where
        // to check it; any other failure, that the system voice took over.
        connect(d->readAloud, &Thoughts::ReadAloud::Controller::fallingBack, this, [this](const QString &reason) {
            if (reason == QLatin1String("no_key")) {
                if (!std::exchange(s_addKeyHintShown, true)) {
                    d->messageWindow->display(i18n("Reading with the system voice. For the cloud voice, add an OpenRouter key in Thoughts > Preferences > Read Aloud."),
                                              QString(), PageViewMessage::Info, 6000);
                }
            } else if (reason == QLatin1String("http_401") || reason == QLatin1String("http_403")) {
                if (!std::exchange(s_addKeyHintShown, true)) {
                    d->messageWindow->display(i18n("OpenRouter refused the key — using the system voice. Check the key in Thoughts > Preferences > Read Aloud."),
                                              QString(), PageViewMessage::Warning, 6000);
                }
            } else if (!std::exchange(s_cloudFallbackNoticeShown, true)) {
                d->messageWindow->display(i18n("Cloud voice unavailable — using the system voice."), QString(), PageViewMessage::Info, 4000);
            }
        });
        connect(d->readAloud, &Thoughts::ReadAloud::Controller::unavailable, this, [this](const QString &) {
            d->messageWindow->display(i18n("Read Aloud is unavailable: the cloud voice cannot be used and there is no system voice."), QString(), PageViewMessage::Warning, 5000);
        });
    }
    s_activeReadAloud = d->readAloud;
    d->readAloud->start(text);
}

void PageView::readAloudSelection()
{
    const QString text = d->selectedText();
    if (text.trimmed().isEmpty()) {
        if (readingAloud()) {
            stopReadAloud();
        } else {
            d->messageWindow->display(i18n("Select text to read it aloud."), QString(), PageViewMessage::Info, 2500);
        }
        return;
    }
    readAloud(text);
}

void PageView::stopReadAloud()
{
    if (d->readAloud) {
        d->readAloud->stop();
    }
}

bool PageView::readingAloud() const
{
    return d->readAloud && d->readAloud->active();
}

void PageView::hideEvent(QHideEvent *event)
{
    // Switching to another tab hides this view inside a visible window: stop.
    // Hiding the whole window (the Library is in use) or minimizing keeps reading.
    if (!event->spontaneous() && window()->isVisible() && readingAloud()) {
        stopReadAloud();
    }
    QAbstractScrollArea::hideEvent(event);
}

void PageView::resizeEvent(QResizeEvent *e)
{
    if (d->items.isEmpty()) {
        resizeContentArea(e->size());
        return;
    }

    if ((d->zoomMode == ZoomFitWidth || d->zoomMode == ZoomFitAuto) && !verticalScrollBar()->isVisible() && qAbs(e->oldSize().height() - e->size().height()) < verticalScrollBar()->width() && d->verticalScrollBarVisible) {
        // this saves us from infinite resizing loop because of scrollbars appearing and disappearing
        // see bug 160628 for more info
        // TODO looks are still a bit ugly because things are left uncentered
        // but better a bit ugly than unusable
        d->verticalScrollBarVisible = false;
        resizeContentArea(e->size());
        return;
    } else if (d->zoomMode == ZoomFitAuto && !horizontalScrollBar()->isVisible() && qAbs(e->oldSize().width() - e->size().width()) < horizontalScrollBar()->height() && d->horizontalScrollBarVisible) {
        // this saves us from infinite resizing loop because of scrollbars appearing and disappearing
        // TODO looks are still a bit ugly because things are left uncentered
        // but better a bit ugly than unusable
        d->horizontalScrollBarVisible = false;
        resizeContentArea(e->size());
        return;
    }

    if (d->pinchZoomActive) {
        // if we make a continuous zooming with pinch gesture or mouse, we call delayedResizeEvent() directly.
        delayedResizeEvent();
    } else {
        // start a timer that will refresh the pixmap after 0.2s
        d->delayResizeEventTimer->start(200);
    }

    d->verticalScrollBarVisible = verticalScrollBar()->isVisible();
    d->horizontalScrollBarVisible = horizontalScrollBar()->isVisible();
}

void PageView::keyPressEvent(QKeyEvent *e)
{
    if (d->passageChange && e->key() == Qt::Key_Escape) {
        cancelPassageChange();
        e->accept();
        return;
    }
    if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        if (e->key() == Qt::Key_Escape) {
            stopDrawing();
            e->accept();
            return;
        }
        if (e->matches(QKeySequence::Undo) || (e->key() == Qt::Key_Z && e->modifiers() == Qt::MetaModifier)) {
            undoLastAnnotation();
            e->accept();
            return;
        }
    }
    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        const QString selected = d->documentFeedback->selectedTextBox();
        if (e->key() == Qt::Key_Escape) {
            if (selected.isEmpty()) stopTextBoxes();
            else selectTextBox(QString());
            e->accept();
            return;
        }
        if (!selected.isEmpty() && (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)) {
            selectTextBox(QString());
            deleteDocumentFeedback(selected);
            e->accept();
            return;
        }
        if (!selected.isEmpty() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
            editTextBox(selected);
            e->accept();
            return;
        }
    }

    // Ignore ESC key press to send to shell.cpp
    if (e->key() != Qt::Key_Escape) {
        e->accept();
    } else {
        e->ignore();
    }

    // if performing a selection or dyn zooming, disable keys handling
    if ((d->mouseSelecting && e->key() != Qt::Key_Escape) || (QApplication::mouseButtons() & Qt::MiddleButton)) {
        return;
    }

    // move/scroll page by using keys
    // When the shift key is held down, scroll ten times faster
    int stepsize = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    switch (e->key()) {
    case Qt::Key_J:
    case Qt::Key_Down:
        slotScrollDown(stepsize /* go down 1 step */);
        break;

    case Qt::Key_PageDown:
        slotScrollDown();
        break;

    case Qt::Key_K:
    case Qt::Key_Up:
        slotScrollUp(stepsize /* go up 1 step */);
        break;

    case Qt::Key_PageUp:
    case Qt::Key_Backspace:
        slotScrollUp();
        break;

    case Qt::Key_Left:
        if (e->modifiers() & Qt::ShiftModifier) {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(-stepsize * horizontalScrollBar()->singleStep(), 0), d->currentShortScrollDuration);
        } else {
            const int nextPage = d->document->currentPage() - viewColumns();
            d->document->setViewportPage(nextPage);
        }
        break;
    case Qt::Key_Right:
        if (e->modifiers() & Qt::ShiftModifier) {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(stepsize * horizontalScrollBar()->singleStep(), 0), d->currentShortScrollDuration);
        } else {
            const int nextPage = d->document->currentPage() + viewColumns();
            d->document->setViewportPage(nextPage);
        }
        break;
    case Qt::Key_H:
        if (horizontalScrollBar()->maximum() == 0) {
            const int nextPage = d->document->currentPage() - viewColumns();
            d->document->setViewportPage(nextPage);
        } else {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(-stepsize * horizontalScrollBar()->singleStep(), 0), d->currentShortScrollDuration);
        }
        break;
    case Qt::Key_L:
        if (horizontalScrollBar()->maximum() == 0) {
            const int nextPage = d->document->currentPage() + viewColumns();
            d->document->setViewportPage(nextPage);
        } else {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(stepsize * horizontalScrollBar()->singleStep(), 0), d->currentShortScrollDuration);
        }
        break;
    case Qt::Key_Escape:
        Q_EMIT escPressed();
        stopReadAloud();
        selectionClear(d->tableDividersGuessed ? ClearOnlyDividers : ClearAllSelection);
        textSelectionClear();
        d->mousePressPos = QPointF();
        if (d->aPrevAction) {
            d->aPrevAction->trigger();
            d->aPrevAction = nullptr;
        }
        d->mouseAnnotation->routeKeyPressEvent(e);
        break;
    case Qt::Key_Delete:
        d->mouseAnnotation->routeKeyPressEvent(e);
        break;
    case Qt::Key_Shift:
    case Qt::Key_Control:
        if (d->autoScrollTimer) {
            if (d->autoScrollTimer->isActive()) {
                d->autoScrollTimer->stop();
            } else {
                slotAutoScroll();
            }
            return;
        }
        // fallthrough
    default:
        e->ignore();
        return;
    }
    // if a known key has been pressed, stop scrolling the page
    if (d->autoScrollTimer) {
        d->scrollIncrement = 0;
        d->autoScrollTimer->stop();
    }
}

void PageView::keyReleaseEvent(QKeyEvent *e)
{
    e->accept();

    if (d->annotator && d->annotator->active()) {
        if (d->annotator->routeKeyEvent(e)) {
            return;
        }
    }

    if (e->key() == Qt::Key_Escape && d->autoScrollTimer) {
        d->scrollIncrement = 0;
        d->autoScrollTimer->stop();
    }

    if (e->key() == Qt::Key_Control) {
        continuousZoomEnd();
    }
}

void PageView::inputMethodEvent(QInputMethodEvent *e)
{
    Q_UNUSED(e)
}

void PageView::tabletEvent(QTabletEvent *e)
{
    // Ignore tablet events that we don't care about
    if (!(e->type() == QEvent::TabletPress || e->type() == QEvent::TabletRelease || e->type() == QEvent::TabletMove)) {
        e->ignore();
        return;
    }

    // Determine pen state
    bool penReleased = false;
    if (e->type() == QEvent::TabletPress) {
        d->penDown = true;
    }
    if (e->type() == QEvent::TabletRelease) {
        d->penDown = false;
        penReleased = true;
    }

    // If we're editing an annotation and the tablet pen is either down or just released
    // then dispatch event to annotator
    if (d->annotator && d->annotator->active() && (d->penDown || penReleased)) {
        // accept the event, otherwise it comes back as a mouse event
        e->accept();

        const QPointF eventPos = contentAreaPoint(e->position());
        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        const QPoint localOriginInGlobal = mapToGlobal(QPoint(0, 0));

        // routeTabletEvent will accept or ignore event as appropriate
        d->annotator->routeTabletEvent(e, pageItem, localOriginInGlobal);
    } else {
        e->ignore();
    }
}

void PageView::continuousZoom(double delta)
{
    if (delta) {
        d->zoomFactor *= (1.0 + (delta / 500.0));
        d->blockPixmapsRequest = true;
        updateZoom(ZoomRefreshCurrent);
        d->blockPixmapsRequest = false;
        viewport()->update();
    }
}

void PageView::continuousZoomEnd()
{
    // request pixmaps since it was disabled during drag
    slotRequestVisiblePixmaps();

    // the cursor may now be over a link.. update it
    updateCursor();
}

void PageView::mouseMoveEvent(QMouseEvent *e)
{
    d->previousMouseMovePos = e->globalPosition();
    if (!d->pressedAgentProposalMarker.isEmpty()) {
        // A drag that starts on a marker neither accepts it nor scrolls the page.
        if (!d->agentProposalPressCancelled && (d->mousePressPos - e->globalPosition()).manhattanLength() >= QApplication::startDragDistance()) {
            d->agentProposalPressCancelled = true;
            d->agentProposalHoldTimer->stop();
        }
        // The bar a hold opened stays while the button is down.
        if (!d->agentProposalPressHeld) {
            updateAgentProposalHover(e->pos(), e->buttons());
        }
        e->accept();
        return;
    }
    updateAgentProposalHover(e->pos(), e->buttons());

    // don't perform any mouse action when no document is shown
    if (d->items.isEmpty()) {
        return;
    }

    // if holding mouse mid button, perform zoom
    if (e->buttons() & Qt::MiddleButton) {
        int deltaY = d->mouseMidLastY - e->globalPosition().y();
        d->mouseMidLastY = e->globalPosition().y();

        const float upperZoomLimit = d->document->supportsTiles() ? 99.99 : 3.99;

        // Wrap mouse cursor
        if (Okular::Settings::dragBeyondScreenEdges()) {
            Qt::Edges wrapEdges;
            wrapEdges.setFlag(Qt::TopEdge, d->zoomFactor < upperZoomLimit);
            wrapEdges.setFlag(Qt::BottomEdge, d->zoomFactor > 0.101);

            deltaY += CursorWrapHelper::wrapCursor(e->globalPosition().toPoint(), wrapEdges).y();
        }

        // update zoom level, perform zoom and redraw
        continuousZoom(deltaY);
        return;
    }

    const QPoint eventPos = contentAreaPoint(e->pos());



    // if we're editing an annotation, dispatch event to it
    if (d->annotator && d->annotator->active()) {
        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        updateCursor(eventPos);
        d->annotator->routeMouseEvent(e, pageItem);
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        if (d->erasing) eraseAlong(eventPos);
        else if (d->strokePage > 0) extendDrawingStroke(eventPos);
        else updateCursor(eventPos);
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        if (!d->textBoxPressed.isEmpty() && (e->buttons() & Qt::LeftButton)) moveTextBoxTool(eventPos);
        else updateCursor(eventPos);
        return;
    }

    bool leftButton = (e->buttons() == Qt::LeftButton);
    bool rightButton = (e->buttons() == Qt::RightButton);

    PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
    switch (d->mouseMode) {
    case Okular::Settings::EnumMouseMode::Browse: {
        if (leftButton) {
            d->leftClickTimer.stop();
            if (d->mouseAnnotation->isActive()) {
                // if left button pressed and annotation is focused, forward move event
                d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, leftButton);
            }
            // drag page
            else {
                if (d->scroller->state() == QScroller::Inactive || d->scroller->state() == QScroller::Scrolling) {
                    d->mouseGrabOffset = QPoint(0, 0);

                    if (!d->pinchZoomActive) {
                        d->scroller->handleInput(QScroller::InputPress, e->pos(), e->timestamp() - 1);
                    }
                }

                setCursor(Qt::ClosedHandCursor);

                // Wrap mouse cursor
                if (Okular::Settings::dragBeyondScreenEdges()) {
                    Qt::Edges wrapEdges;
                    wrapEdges.setFlag(Qt::TopEdge, verticalScrollBar()->value() < verticalScrollBar()->maximum());
                    wrapEdges.setFlag(Qt::BottomEdge, verticalScrollBar()->value() > verticalScrollBar()->minimum());
                    wrapEdges.setFlag(Qt::LeftEdge, horizontalScrollBar()->value() < horizontalScrollBar()->maximum());
                    wrapEdges.setFlag(Qt::RightEdge, horizontalScrollBar()->value() > horizontalScrollBar()->minimum());

                    d->mouseGrabOffset -= CursorWrapHelper::wrapCursor(e->pos(), wrapEdges);
                }

                if (!d->pinchZoomActive) {
                    d->scroller->handleInput(QScroller::InputMove, e->pos() + d->mouseGrabOffset, e->timestamp());
                }
            }
        } else if (rightButton && !d->mousePressPos.isNull() && d->aMouseSelect) {
            // if mouse moves 5 px away from the press point, switch to 'selection'
            qreal deltaX = d->mousePressPos.x() - e->globalPosition().x(), deltaY = d->mousePressPos.y() - e->globalPosition().y();
            if (deltaX > 5 || deltaX < -5 || deltaY > 5 || deltaY < -5) {
                d->aPrevAction = d->aMouseNormal;
                d->aMouseSelect->trigger();
                QPoint newPos = eventPos + QPoint(deltaX, deltaY);
                selectionStart(newPos, palette().color(QPalette::Active, QPalette::Highlight).lighter(120), false);
                updateSelection(eventPos);
                break;
            }
        } else {
            /* Forward move events which are still not yet consumed by "mouse grab" or aMouseSelect */
            d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, leftButton);
            updateCursor();
        }
    } break;

    case Okular::Settings::EnumMouseMode::Zoom:
    case Okular::Settings::EnumMouseMode::RectSelect:
    case Okular::Settings::EnumMouseMode::TableSelect:
    case Okular::Settings::EnumMouseMode::TrimSelect:
        // set second corner of selection
        if (d->mouseSelecting) {
            updateSelection(eventPos);
            d->mouseOverLinkObject = nullptr;
        }
        d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, leftButton);
        updateCursor();
        break;

    case Okular::Settings::EnumMouseMode::Magnifier:
        if (e->buttons()) // if any button is pressed at all
        {
            moveMagnifier(e->pos());
            updateMagnifier(eventPos);
        }
        break;

    case Okular::Settings::EnumMouseMode::TextSelect:
        // A window opened during a click can consume its release. Hovering
        // must never start or extend a drag from a stale press position.
        if (!leftButton) {
            d->mouseTextSelecting = false;
            if (d->textDragArea) {
                d->textDragArea = false;
                selectionClear();
            }
            if (e->buttons() == Qt::NoButton) {
                d->mousePressPos = QPointF();
            }
            d->mouseCommentCandidate = nullptr;
            d->mouseCommentCandidatePage = -1;
            d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, false);
            updateCursor();
            break;
        }
        if (d->mouseCommentCandidate) {
            if (leftButton) {
                if ((eventPos - d->mouseSelectPos).manhattanLength() < QApplication::startDragDistance()) {
                    d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, false);
                    updateCursor();
                    break;
                }

                if (isDraggablePopupNote(d->mouseCommentCandidate)) {
                    d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, true);
                    updateCursor();
                    break;
                }
            }

            d->mouseCommentCandidate = nullptr;
            d->mouseCommentCandidatePage = -1;
            textSelectionClear();
        }

        // if mouse moves 5 px away from the press point and the document supports text extraction, do 'textselection'
        if (!d->mouseTextSelecting && !d->mousePressPos.isNull() && d->document->supportsSearching() && ((eventPos - d->mouseSelectPos).manhattanLength() > 5)) {
            d->mouseTextSelecting = true;
        }
        d->mouseAnnotation->routeMouseMoveEvent(pageItem, eventPos, leftButton);
        updateSelection(eventPos);
        if (d->mouseTextSelecting) {
            updateTextDragArea(eventPos);
        }
        updateCursor();
        break;
    }
}

void PageView::mousePressEvent(QMouseEvent *e)
{
    hideAgentProposalHoverBar();
    // don't perform any mouse action when no document is shown
    if (d->items.isEmpty()) {
        return;
    }
    // A linked passage stays marked until the next click.
    if (d->placeHighlight) {
        textSelectionClear();
    }

    // if performing a selection or dyn zooming, disable mouse press
    if (d->mouseSelecting || (e->button() != Qt::MiddleButton && (e->buttons() & Qt::MiddleButton))) {
        return;
    }

    // if the page is scrolling, stop it
    if (d->autoScrollTimer) {
        d->scrollIncrement = 0;
        d->autoScrollTimer->stop();
    }

    // if pressing mid mouse button while not doing other things, begin 'continuous zoom' mode
    if (e->button() == Qt::MiddleButton) {
        d->mouseMidLastY = e->globalPosition().y();
        setCursor(Qt::SizeVerCursor);
        CursorWrapHelper::startDrag();
        return;
    }

    const QPoint eventPos = contentAreaPoint(e->pos());

    // if we're editing an annotation, dispatch event to it
    if (d->annotator && d->annotator->active()) {
        d->scroller->stop();
        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        d->annotator->routeMouseEvent(e, pageItem);
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        if (e->button() == Qt::LeftButton) {
            if (Okular::Settings::pencilEraser()) beginErasing(eventPos);
            else beginDrawingStroke(eventPos);
        }
        e->accept();
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        if (e->button() == Qt::LeftButton) pressTextBoxTool(eventPos);
        e->accept();
        return;
    }

    d->pressedAgentProposalMarker.clear();
    d->agentProposalHoldTimer->stop();
    d->pressedOverlayMarker.clear();
    if (e->button() == Qt::LeftButton && agentProposalMarkerAt(eventPos).isEmpty()) {
        // A press on a reading overlay's comment marker opens its popup on release.
        bool marker = false;
        const QStringList mark = readingOverlayMarkAt(eventPos, &marker);
        if (marker) {
            d->pressedOverlayMarker = mark;
            d->mousePressPos = e->globalPosition();
            d->scroller->stop();
            e->accept();
            return;
        }
    }
    if (e->button() == Qt::LeftButton) {
        d->pressedAgentProposalMarker = agentProposalMarkerAt(eventPos);
        if (!d->pressedAgentProposalMarker.isEmpty()) {
            d->mousePressPos = e->globalPosition();
            d->agentProposalPressHeld = false;
            // A press that stops a page still scrolling (a fling coasting, a trackpad's momentum
            // still arriving as wheel events) is not a click on the marker it lands on.
            d->agentProposalPressCancelled = d->scroller->state() == QScroller::Scrolling
                || (d->agentProposalLastWheel.isValid() && d->agentProposalLastWheel.elapsed() < AgentProposalPressScrollQuietMs);
            d->scroller->stop();
            if (!d->agentProposalPressCancelled) {
                d->agentProposalHoldTimer->start();
            }
            e->accept();
            return;
        }
    }

    // trigger history navigation for additional mouse buttons
    if (e->button() == Qt::XButton1) {
        Q_EMIT mouseBackButtonClick();
        return;
    }
    if (e->button() == Qt::XButton2) {
        Q_EMIT mouseForwardButtonClick();
        return;
    }

    // update press / 'start drag' mouse position
    d->mousePressPos = e->globalPosition();
    d->mouseCommentCandidate = nullptr;
    d->mouseCommentCandidatePage = -1;
    CursorWrapHelper::startDrag();

    // handle mode dependent mouse press actions
    bool leftButton = e->button() == Qt::LeftButton, rightButton = e->button() == Qt::RightButton;

    //   Not sure we should erase the selection when clicking with left.
    if (d->mouseMode != Okular::Settings::EnumMouseMode::TextSelect) {
        textSelectionClear();
    }

    switch (d->mouseMode) {
    case Okular::Settings::EnumMouseMode::Browse: // drag start / click / link following
    {
        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        if (leftButton) {
            if (pageItem) {
                d->mouseCommentCandidate = commentAnnotationAtPoint(pageItem, eventPos);
                d->mouseCommentCandidatePage = d->mouseCommentCandidate ? pageItem->pageNumber() : -1;
                d->mouseAnnotation->routeMousePressEvent(pageItem, eventPos, isDraggablePopupNote(d->mouseCommentCandidate) ? d->mouseCommentCandidate : nullptr);
            }

            if (!d->mouseOnRect) {
                d->mouseGrabOffset = QPoint(0, 0);
                if (!d->pinchZoomActive) {
                    d->scroller->handleInput(QScroller::InputPress, e->pos(), e->timestamp());
                }
                d->leftClickTimer.start(QApplication::doubleClickInterval() + 10);
            }
        }
    } break;

    case Okular::Settings::EnumMouseMode::Zoom: // set first corner of the zoom rect
        if (leftButton) {
            selectionStart(eventPos, palette().color(QPalette::Active, QPalette::Highlight), false);
        } else if (rightButton) {
            updateZoom(ZoomOut);
        }
        break;

    case Okular::Settings::EnumMouseMode::Magnifier:
        moveMagnifier(e->pos());
        d->magnifierView->show();
        updateMagnifier(eventPos);
        break;

    case Okular::Settings::EnumMouseMode::RectSelect: // set first corner of the selection rect
    case Okular::Settings::EnumMouseMode::TrimSelect:
        if (leftButton) {
            selectionStart(eventPos, palette().color(QPalette::Active, QPalette::Highlight).lighter(120), false);
        }
        break;
    case Okular::Settings::EnumMouseMode::TableSelect:
        if (leftButton) {
            if (d->tableSelectionParts.isEmpty()) {
                selectionStart(eventPos, palette().color(QPalette::Active, QPalette::Highlight).lighter(120), false);
            } else {
                QRect updatedRect;
                for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
                    QRect selectionPartRect = tsp.rectInItem.geometry(tsp.item->uncroppedWidth(), tsp.item->uncroppedHeight());
                    selectionPartRect.translate(tsp.item->uncroppedGeometry().topLeft());

                    // This will update the whole table rather than just the added/removed divider
                    // (which can span more than one part).
                    updatedRect = updatedRect.united(selectionPartRect);

                    if (!selectionPartRect.contains(eventPos)) {
                        continue;
                    }

                    // At this point it's clear we're either adding or removing a divider manually, so obviously the user is happy with the guess (if any).
                    d->tableDividersGuessed = false;

                    // There's probably a neat trick to finding which edge it's closest to,
                    // but this way has the advantage of simplicity.
                    const int fromLeft = abs(selectionPartRect.left() - eventPos.x());
                    const int fromRight = abs(selectionPartRect.left() + selectionPartRect.width() - eventPos.x());
                    const int fromTop = abs(selectionPartRect.top() - eventPos.y());
                    const int fromBottom = abs(selectionPartRect.top() + selectionPartRect.height() - eventPos.y());
                    const int colScore = fromTop < fromBottom ? fromTop : fromBottom;
                    const int rowScore = fromLeft < fromRight ? fromLeft : fromRight;

                    if (colScore < rowScore) {
                        bool deleted = false;
                        for (int i = 0; i < d->tableSelectionCols.length(); i++) {
                            const double col = (d->tableSelectionCols[i] - tsp.rectInSelection.left) / (tsp.rectInSelection.right - tsp.rectInSelection.left);
                            const int colX = selectionPartRect.left() + col * selectionPartRect.width() + 0.5;
                            if (abs(colX - eventPos.x()) <= 3) {
                                d->tableSelectionCols.removeAt(i);
                                deleted = true;

                                break;
                            }
                        }
                        if (!deleted) {
                            double col = eventPos.x() - selectionPartRect.left();
                            col /= selectionPartRect.width(); // at this point, it's normalised within the part
                            col *= (tsp.rectInSelection.right - tsp.rectInSelection.left);
                            col += tsp.rectInSelection.left; // at this point, it's normalised within the whole table

                            d->tableSelectionCols.append(col);
                            std::sort(d->tableSelectionCols.begin(), d->tableSelectionCols.end());
                        }
                    } else {
                        bool deleted = false;
                        for (int i = 0; i < d->tableSelectionRows.length(); i++) {
                            const double row = (d->tableSelectionRows[i] - tsp.rectInSelection.top) / (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
                            const int rowY = selectionPartRect.top() + row * selectionPartRect.height() + 0.5;
                            if (abs(rowY - eventPos.y()) <= 3) {
                                d->tableSelectionRows.removeAt(i);
                                deleted = true;

                                break;
                            }
                        }
                        if (!deleted) {
                            double row = eventPos.y() - selectionPartRect.top();
                            row /= selectionPartRect.height(); // at this point, it's normalised within the part
                            row *= (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
                            row += tsp.rectInSelection.top; // at this point, it's normalised within the whole table

                            d->tableSelectionRows.append(row);
                            std::sort(d->tableSelectionRows.begin(), d->tableSelectionRows.end());
                        }
                    }
                }
                updatedRect.translate(-contentAreaPosition());
                viewport()->update(updatedRect);
            }
        } else if (rightButton && !d->tableSelectionParts.isEmpty()) {
            QMenu menu(this);
            QAction *copyToClipboard = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Table Contents to Clipboard"));

            const bool copyAllowed = d->document->isAllowed(Okular::AllowCopy);

            if (!copyAllowed) {
                copyToClipboard->setEnabled(false);
                copyToClipboard->setText(i18n("Copy forbidden by DRM"));
            }

            const QAction *choice = menu.exec(e->globalPosition().toPoint());
            if (choice == copyToClipboard) {
                copyTextSelection(TextCopyMode::AsProvided);
            }
        }
        break;
    case Okular::Settings::EnumMouseMode::TextSelect:
        d->mouseSelectPos = eventPos;
        if (d->textDragArea) {
            d->textDragArea = false;
            selectionClear();
        }
        if (leftButton) {
            PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
            if (Okular::Annotation *annotation = commentAnnotationAtPoint(pageItem, eventPos)) {
                d->mouseCommentCandidate = annotation;
                d->mouseCommentCandidatePage = pageItem->pageNumber();
                d->tripleClickDetectionEnabled = false;
                if (isDraggablePopupNote(annotation)) {
                    d->mouseAnnotation->routeMousePressEvent(pageItem, eventPos, annotation);
                }
                break;
            }

            // Check for triple-click: single click shortly after double-click at same position
            const bool isTripleClick = d->tripleClickDetectionEnabled && d->lastDoubleClickTime.isValid() && d->lastDoubleClickTime.elapsed() < QApplication::doubleClickInterval() &&
                (eventPos - d->lastDoubleClickPos).manhattanLength() < QApplication::startDragDistance();

            if (isTripleClick) {
                // TRIPLE-CLICK: Select entire line
                PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
                if (pageItem) {
                    double nX = pageItem->absToPageX(eventPos.x());
                    double nY = pageItem->absToPageY(eventPos.y());

                    textSelectionClear();
                    std::unique_ptr<Okular::RegularAreaRect> lineRect = pageItem->page()->lineAt(Okular::NormalizedPoint(nX, nY));
                    if (lineRect) {
                        d->document->setPageTextSelection(pageItem->pageNumber(), std::move(lineRect), palette().color(QPalette::Active, QPalette::Highlight));
                        d->pagesWithTextSelection << pageItem->pageNumber();
                        if (d->document->isAllowed(Okular::AllowCopy)) {
                            const QString text = d->selectedText();
                            if (!text.isEmpty()) {
                                QClipboard *cb = QApplication::clipboard();
                                if (cb->supportsSelection()) {
                                    cb->setText(text, QClipboard::Selection);
                                }
                            }
                        }
                    }
                }
                // Update timestamp for potential 4th click detection
                d->lastDoubleClickTime.start();
                d->lastDoubleClickPos = eventPos;
            } else {
                // Regular single click - clear selection and disable triple-click detection
                textSelectionClear();
                d->tripleClickDetectionEnabled = false;
            }
        } else if (!rightButton) {
            textSelectionClear();
        }
        break;
    }
}

void PageView::mouseReleaseEvent(QMouseEvent *e)
{
    // stop the drag scrolling
    d->dragScrollTimer.stop();

    d->leftClickTimer.stop();

    const bool leftButton = e->button() == Qt::LeftButton;
    const bool rightButton = e->button() == Qt::RightButton;

    if (d->mouseAnnotation->isActive() && leftButton) {
        // Just finished to move the annotation
        d->mouseAnnotation->routeMouseReleaseEvent();
    }

    // don't perform any mouse action when no document is shown..
    if (d->items.isEmpty()) {
        // ..except for right Clicks (emitted even it viewport is empty)
        if (e->button() == Qt::RightButton) {
            Q_EMIT rightClick(nullptr, e->globalPosition().toPoint());
        }
        return;
    }

    const QPoint eventPos = contentAreaPoint(e->pos());

    if (rightButton && d->m_reviewMaskStyle == ReviewMaskStyle::SidebarHighlight && !(d->annotator && d->annotator->active())) {
        const auto *item = pickItemOnPoint(eventPos.x(), eventPos.y());
        if (item) {
            const auto origin = item->uncroppedGeometry().topLeft();
            const double x = double(eventPos.x() - origin.x()) / item->uncroppedWidth();
            const double y = double(eventPos.y() - origin.y()) / item->uncroppedHeight();
            for (const auto &box : d->m_reviewMaskRects.value(item->pageNumber())) {
                if (x < box.left || x > box.right || y < box.top || y > box.bottom) continue;
                QMenu menu(this);
                auto *edit = menu.addAction(i18n("Edit Review Card…"));
                edit->setObjectName(QStringLiteral("editHighlightedReviewCard"));
                if (menu.exec(e->globalPosition().toPoint()) == edit) Q_EMIT reviewCardEditRequested();
                e->accept();
                return;
            }
        }
    }

    // handle mode independent mid bottom zoom
    if (e->button() == Qt::MiddleButton) {
        continuousZoomEnd();
        return;
    }

    // if we're editing an annotation, dispatch event to it
    if (d->annotator && d->annotator->active()) {
        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        d->annotator->routeMouseEvent(e, pageItem);
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        if (e->button() == Qt::LeftButton) {
            finishErasing();
            finishDrawingStroke();
        }
        e->accept();
        return;
    }

    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        if (e->button() == Qt::LeftButton) releaseTextBoxTool(eventPos);
        e->accept();
        return;
    }

    if (leftButton && !d->pressedOverlayMarker.isEmpty()) {
        const QStringList pressed = std::exchange(d->pressedOverlayMarker, {});
        const QPointF pressPos = std::exchange(d->mousePressPos, QPointF());
        bool marker = false;
        if (readingOverlayMarkAt(eventPos, &marker) == pressed && marker && (pressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance())
            openReadingOverlayMarkPopup(pressed.at(0), pressed.at(1));
        e->accept();
        return;
    }

    if (leftButton && !d->pressedAgentProposalMarker.isEmpty()) {
        const QString proposalId = std::exchange(d->pressedAgentProposalMarker, {});
        const QPointF pressPos = std::exchange(d->mousePressPos, QPointF());
        d->agentProposalHoldTimer->stop();
        const bool held = std::exchange(d->agentProposalPressHeld, false);
        const bool cancelled = std::exchange(d->agentProposalPressCancelled, false);
        d->mouseTextSelecting = false;
        d->mouseCommentCandidate = nullptr;
        d->mouseCommentCandidatePage = -1;
        if (!held && !cancelled && agentProposalMarkerAt(eventPos) == proposalId
            && (pressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance()) {
            // A click on the ✓ accepts a proposal that can be decided here;
            // holding it, or resting the pointer on it, shows Open, Accept and
            // Reject. One that cannot be decided opens its window, which says why.
            if (agentProposalDecisionState(proposalId).decidable) {
                hideAgentProposalHoverBar();
                Q_EMIT agentProposalResolutionRequested(proposalId, QStringLiteral("accept"));
            } else {
                d->selectedAgentProposalId = proposalId;
                viewport()->update();
                Q_EMIT agentProposalReviewRequested(proposalId);
            }
        }
        e->accept();
        return;
    }

    Okular::Annotation *clickedCommentAnnotation = nullptr;
    int clickedCommentAnnotationPage = -1;
    if (leftButton) {
        const PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        const QPointF pressPos = contentAreaPoint(mapFromGlobal(d->mousePressPos));
        const PageViewItem *pageItemPressPos = pickItemOnPoint(pressPos.x(), pressPos.y());
        const bool isClick = pageItem && pageItem == pageItemPressPos && ((d->mousePressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance());

        if (isClick) {
            const QString proposalId = agentProposalAt(pageItem, eventPos);
            if (!proposalId.isEmpty()) {
                d->selectedAgentProposalId = proposalId;
                viewport()->update();
                e->accept();
                return;
            }
            Okular::Annotation *annotation = commentAnnotationAtPoint(pageItem, eventPos);
            if (annotation == d->mouseCommentCandidate && pageItem->pageNumber() == d->mouseCommentCandidatePage) {
                clickedCommentAnnotation = annotation;
                clickedCommentAnnotationPage = pageItem->pageNumber();
            }

            const double nX = pageItem->absToPageX(eventPos.x());
            const double nY = pageItem->absToPageY(eventPos.y());
            const QRect &itemRect = pageItem->uncroppedGeometry();
            const Okular::ObjectRect *orect = pageItem->page()->objectRect(Okular::ObjectRect::OAnnotation, nX, nY, itemRect.width(), itemRect.height());
            if (!orect) {
                closeReviewAnnotationWindow();
            }
        }
    }

    if (clickedCommentAnnotation) {
        Q_EMIT annotationClicked(clickedCommentAnnotation, clickedCommentAnnotationPage);
    }

    switch (d->mouseMode) {
    case Okular::Settings::EnumMouseMode::Browse: {
        if (!d->pinchZoomActive) {
            d->scroller->handleInput(QScroller::InputRelease, e->pos() + d->mouseGrabOffset, e->timestamp());
        }

        // return the cursor to its normal state after dragging
        if (cursor().shape() == Qt::ClosedHandCursor) {
            updateCursor(eventPos);
        }

        PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        const QPointF pressPos = contentAreaPoint(mapFromGlobal(d->mousePressPos));
        const PageViewItem *pageItemPressPos = pickItemOnPoint(pressPos.x(), pressPos.y());

        // if the mouse has not moved since the press, that's a -click-
        if (leftButton && pageItem && pageItem == pageItemPressPos && ((d->mousePressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance())
            && (openReviewFeedbackAt(eventPos) || openDocumentFeedbackAt(eventPos))) {
            break;
        }
        if (leftButton && pageItem && pageItem == pageItemPressPos && ((d->mousePressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance())) {
            if (!mouseReleaseOverLink(d->mouseOverLinkObject) && (e->modifiers() == Qt::ShiftModifier)) {
                const double nX = pageItem->absToPageX(eventPos.x());
                const double nY = pageItem->absToPageY(eventPos.y());
                const Okular::ObjectRect *rect;
                // TODO: find a better way to activate the source reference "links"
                // for the moment they are activated with Shift + left click
                // Search the nearest source reference.
                rect = pageItem->page()->objectRect(Okular::ObjectRect::SourceRef, nX, nY, pageItem->uncroppedWidth(), pageItem->uncroppedHeight());
                if (!rect) {
                    static const double s_minDistance = 0.025; // FIXME?: empirical value?
                    double distance = 0.0;
                    rect = pageItem->page()->nearestObjectRect(Okular::ObjectRect::SourceRef, nX, nY, pageItem->uncroppedWidth(), pageItem->uncroppedHeight(), &distance);
                    // distance is distanceSqr, adapt it to a normalized value
                    distance = distance / (pow(pageItem->uncroppedWidth(), 2) + pow(pageItem->uncroppedHeight(), 2));
                    if (rect && (distance > s_minDistance)) {
                        rect = nullptr;
                    }
                }
                if (rect) {
                    if (const Okular::SourceReference *ref = static_cast<const Okular::SourceReference *>(rect->object())) {
                        d->document->processSourceReference(*ref);
                    }
                } else {
                    if (const Okular::SourceReference *ref = d->document->dynamicSourceReference(pageItem->pageNumber(), nX * pageItem->page()->width(), nY * pageItem->page()->height())) {
                        d->document->processSourceReference(*ref);
                        delete ref;
                    }
                }
            }
        } else if (rightButton && !d->mouseAnnotation->isModified()) {
            if (pageItem && pageItem == pageItemPressPos && ((d->mousePressPos - e->globalPosition()).manhattanLength() < QApplication::startDragDistance())) {
                QMenu *menu = createProcessLinkMenu(pageItem, eventPos);

                const QRect &itemRect = pageItem->uncroppedGeometry();
                const double nX = pageItem->absToPageX(eventPos.x());
                const double nY = pageItem->absToPageY(eventPos.y());

                AnnotationPopup annotPopup(d->document, AnnotationPopup::MultiAnnotationMode, this);
                // Do not move annotPopup inside the if, it needs to live until menu->exec()
                addAnnotationActionsForPoint(annotPopup, pageItem, eventPos, &menu);

                if (menu) {
                    menu->exec(e->globalPosition().toPoint());
                    menu->deleteLater();
                } else {
                    // a link can move us to another page or even to another document, there's no point in trying to
                    //  process the click on the image once we have processes the click on the link
                    const Okular::ObjectRect *rect = pageItem->page()->objectRect(Okular::ObjectRect::Image, nX, nY, itemRect.width(), itemRect.height());
                    if (rect) {
                        // handle right click over a image
                    } else {
                        // right click (if not within 5 px of the press point, the mode
                        // had been already changed to 'Selection' instead of 'Normal')
                        Q_EMIT rightClick(pageItem->page(), e->globalPosition().toPoint());
                    }
                }
            } else {
                // right click (if not within 5 px of the press point, the mode
                // had been already changed to 'Selection' instead of 'Normal')
                Q_EMIT rightClick(pageItem ? pageItem->page() : nullptr, e->globalPosition().toPoint());
            }
        }
    } break;

    case Okular::Settings::EnumMouseMode::Zoom:
        // if a selection rect has been defined, zoom into it
        if (leftButton && d->mouseSelecting) {
            QRect selRect = d->mouseSelectionRect.normalized();
            if (selRect.width() <= 8 && selRect.height() <= 8) {
                selectionClear();
                break;
            }

            // find out new zoom ratio and normalized view center (relative to the contentsRect)
            double zoom = qMin((double)viewport()->width() / (double)selRect.width(), (double)viewport()->height() / (double)selRect.height());
            double nX = (double)(selRect.left() + selRect.right()) / (2.0 * (double)contentAreaWidth());
            double nY = (double)(selRect.top() + selRect.bottom()) / (2.0 * (double)contentAreaHeight());

            const float upperZoomLimit = d->document->supportsTiles() ? 100.0 : 4.0;
            if (d->zoomFactor <= upperZoomLimit || zoom <= 1.0) {
                d->zoomFactor *= zoom;
                viewport()->setUpdatesEnabled(false);
                updateZoom(ZoomRefreshCurrent);
                viewport()->setUpdatesEnabled(true);
            }

            // recenter view and update the viewport
            center((int)(nX * contentAreaWidth()), (int)(nY * contentAreaHeight()));
            viewport()->update();

            // hide message box and delete overlay window
            selectionClear();
        }
        break;

    case Okular::Settings::EnumMouseMode::Magnifier:
        d->magnifierView->hide();
        break;

    case Okular::Settings::EnumMouseMode::TrimSelect: {
        // if it is a left release checks if is over a previous link press
        if (leftButton && mouseReleaseOverLink(d->mouseOverLinkObject)) {
            selectionClear();
            break;
        }

        // if mouse is released and selection is null this is a rightClick
        if (rightButton && !d->mouseSelecting) {
            break;
        }
        const PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
        // ensure end point rests within a page, or ignore
        if (!pageItem) {
            break;
        }
        QRect selectionRect = d->mouseSelectionRect.normalized();

        double nLeft = pageItem->absToPageX(selectionRect.left());
        double nRight = pageItem->absToPageX(selectionRect.right());
        double nTop = pageItem->absToPageY(selectionRect.top());
        double nBottom = pageItem->absToPageY(selectionRect.bottom());
        if (nLeft < 0) {
            nLeft = 0;
        }
        if (nTop < 0) {
            nTop = 0;
        }
        if (nRight > 1) {
            nRight = 1;
        }
        if (nBottom > 1) {
            nBottom = 1;
        }
        d->trimBoundingBox = Okular::NormalizedRect(nLeft, nTop, nRight, nBottom);

        // Trim Selection successfully done, hide prompt
        d->messageWindow->hide();

        // clear widget selection and invalidate rect
        selectionClear();

        // When Trim selection bbox interaction is over, we should switch to another mousemode.
        if (d->aPrevAction) {
            d->aPrevAction->trigger();
            d->aPrevAction = nullptr;
        } else {
            d->aMouseNormal->trigger();
        }

        // with d->trimBoundingBox defined, redraw for trim to take visual effect
        if (d->document->pages() > 0) {
            slotRelayoutPages();
            slotRequestVisiblePixmaps(); // TODO: slotRelayoutPages() may have done this already!
        }

        break;
    }
    case Okular::Settings::EnumMouseMode::RectSelect: {
        // if it is a left release checks if is over a previous link press
        if (leftButton && mouseReleaseOverLink(d->mouseOverLinkObject)) {
            selectionClear();
            break;
        }

        // if mouse is released and selection is null this is a rightClick
        if (rightButton && !d->mouseSelecting) {
            PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
            if (pageItem) {
                QMenu *menu = nullptr;
                AnnotationPopup annotPopup(d->document, AnnotationPopup::MultiAnnotationMode, this);
                if (addAnnotationActionsForPoint(annotPopup, pageItem, eventPos, &menu)) {
                    menu->exec(e->globalPosition().toPoint());
                    menu->deleteLater();
                    break;
                }
            }
            Q_EMIT rightClick(pageItem ? pageItem->page() : nullptr, e->globalPosition().toPoint());
            break;
        }

        // if a selection is defined, display a popup
        if ((!leftButton && !d->aPrevAction) || (!rightButton && d->aPrevAction) || !d->mouseSelecting) {
            break;
        }

        QRect selectionRect = d->mouseSelectionRect.normalized();
        if (selectionRect.width() <= 8 && selectionRect.height() <= 8) {
            selectionClear();
            if (d->aPrevAction) {
                d->aPrevAction->trigger();
                d->aPrevAction = nullptr;
            }
            // A click: what is under it opens, as with the text tool.
            if (leftButton && !openReviewFeedbackAt(eventPos)) openDocumentFeedbackAt(eventPos);
            break;
        }

        // if we support text generation
        QString selectedText;
        if (d->document->supportsSearching()) {
            // grab text in selection by extracting it from all intersected pages
            const Okular::Page *okularPage = nullptr;
            for (const PageViewItem *item : std::as_const(d->items)) {
                if (!item->isVisible()) {
                    continue;
                }

                const QRect &itemRect = item->croppedGeometry();
                if (selectionRect.intersects(itemRect)) {
                    // request the textpage if there isn't one
                    okularPage = item->page();
                    qCDebug(OkularUiDebug) << "checking if page" << item->pageNumber() << "has text:" << okularPage->hasTextPage();
                    if (!okularPage->hasTextPage()) {
                        d->document->requestTextPage(okularPage->number());
                    }
                    // grab text in the rect that intersects itemRect
                    QRect relativeRect = selectionRect.intersected(itemRect);
                    relativeRect.translate(-item->uncroppedGeometry().topLeft());
                    Okular::RegularAreaRect rects;
                    rects.append(Okular::NormalizedRect(relativeRect, item->uncroppedWidth(), item->uncroppedHeight()));
                    selectedText += okularPage->text(&rects);
                }
            }
        }

        // popup that ask to copy:text and copy/save:image
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("PopupMenu"));
        QAction *textToClipboard = nullptr;
        QAction *textWithoutLineBreaksToClipboard = nullptr;
        const QAction *readAloudText = nullptr;
        const QAction *imageToClipboard = nullptr;
        const QAction *imageToFile = nullptr;
        QAction *addReviewCard = nullptr;
        int reviewCardPage = 0;
        QVariantList reviewCardRect;
        // Reading first: what a rectangle is for is saying something about a figure or a passage.
        setFeedbackArea(selectionRect, selectedText);
        const auto clearFeedbackArea = qScopeGuard([this] { d->feedbackAreaLines.clear(); });
        addReviewFeedbackActions(&menu);

        const QUrl documentUrl = d->document->currentDocument();
        const QFileInfo documentInfo(documentUrl.toLocalFile());
        const QString currentPdfsPath = QDir::cleanPath(QDir(d->docsRoot).filePath(QStringLiteral("current_pdfs")));
        const QFileInfo documentDirectoryInfo(documentInfo.absolutePath());
        const QFileInfo currentPdfsInfo(currentPdfsPath);
        const QString documentDirectoryPath = documentDirectoryInfo.canonicalFilePath().isEmpty() ? documentDirectoryInfo.absoluteFilePath() : documentDirectoryInfo.canonicalFilePath();
        const QString canonicalCurrentPdfsPath = currentPdfsInfo.canonicalFilePath().isEmpty() ? currentPdfsInfo.absoluteFilePath() : currentPdfsInfo.canonicalFilePath();
        const bool isTrackedPdf = !d->docsRoot.isEmpty() && documentUrl.isLocalFile() && (documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0 || documentInfo.suffix() == QLatin1String("thoughts-project")) && documentDirectoryPath == canonicalCurrentPdfsPath;
        if (isTrackedPdf) {
            const PageViewItem *selectedPageItem = nullptr;
            for (const PageViewItem *item : std::as_const(d->items)) {
                if (!item->isVisible() || !item->uncroppedGeometry().contains(selectionRect)) {
                    continue;
                }
                if (selectedPageItem) {
                    selectedPageItem = nullptr;
                    break;
                }
                selectedPageItem = item;
            }
            if (selectedPageItem) {
                QRect relativeRect = selectionRect;
                relativeRect.translate(-selectedPageItem->uncroppedGeometry().topLeft());
                const Okular::NormalizedRect normalizedRect(relativeRect, selectedPageItem->uncroppedWidth(), selectedPageItem->uncroppedHeight());
                const bool validBounds =
                    normalizedRect.left >= 0.0 && normalizedRect.top >= 0.0 && normalizedRect.right <= 1.0 && normalizedRect.bottom <= 1.0 && normalizedRect.left < normalizedRect.right && normalizedRect.top < normalizedRect.bottom;
                if (validBounds) {
                    reviewCardPage = selectedPageItem->pageNumber() + 1;
                    reviewCardRect = {normalizedRect.left, normalizedRect.top, normalizedRect.right, normalizedRect.bottom};
                    addReviewCard = menu.addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")), i18n("Add Review Card…"));
                    addReviewCard->setObjectName(QStringLiteral("AddReviewCard"));
                    menu.addSeparator();
                }
            }
        }
        if (d->document->supportsSearching() && !selectedText.isEmpty()) {
            textToClipboard = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Text"));
            textToClipboard->setObjectName(QStringLiteral("CopyTextToClipboard"));
        }
        imageToClipboard = menu.addAction(QIcon::fromTheme(QStringLiteral("image-x-generic")), i18n("Copy Image"));
        imageToFile = menu.addAction(QIcon::fromTheme(QStringLiteral("document-save")), i18n("Save Image…"));
        if (textToClipboard) {
            QMenu *more = menu.addMenu(i18n("More"));
            textWithoutLineBreaksToClipboard = more->addAction(QIcon::fromTheme(QStringLiteral("edit-copy-without-line-breaks")), i18n("Copy Text (Without line breaks)"));
            textWithoutLineBreaksToClipboard->setObjectName(QStringLiteral("CopyTextWithoutLineBreaksToClipboard"));
            bool copyAllowed = d->document->isAllowed(Okular::AllowCopy);
            if (!copyAllowed) {
                textToClipboard->setEnabled(false);
                textToClipboard->setText(i18n("Copy forbidden by DRM"));
                textWithoutLineBreaksToClipboard->setEnabled(false);
                textWithoutLineBreaksToClipboard->setText(i18n("Copy forbidden by DRM"));
            }
            QAction *readAloudAction = more->addAction(QIcon::fromTheme(QStringLiteral("text-speak")), i18n("Read Aloud"));
            readAloudAction->setObjectName(QStringLiteral("ReadAloudText"));
            readAloudText = readAloudAction;
            if (copyAllowed) {
                addSearchWithinDocumentAction(more, selectedText);
                addWebShortcutsMenu(more, selectedText);
            }
        }
        const QAction *choice = menu.exec(e->globalPosition().toPoint());
        // check if the user really selected an action
        if (choice) {
            // IMAGE operation chosen
            if (choice == addReviewCard) {
                Q_EMIT manualReviewCardRequested(documentInfo.absoluteFilePath(), reviewCardPage, reviewCardRect, false);
            } else if (choice == imageToClipboard || choice == imageToFile) {
                // renders page into a pixmap
                QPixmap copyPix(selectionRect.width(), selectionRect.height());
                QPainter copyPainter(&copyPix);
                copyPainter.translate(-selectionRect.left(), -selectionRect.top());
                drawDocumentOnPainter(selectionRect, &copyPainter);
                copyPainter.end();

                if (choice == imageToClipboard) {
                    // [2] copy pixmap to clipboard
                    QClipboard *cb = QApplication::clipboard();
                    cb->setPixmap(copyPix, QClipboard::Clipboard);
                    if (cb->supportsSelection()) {
                        cb->setPixmap(copyPix, QClipboard::Selection);
                    }
                    d->messageWindow->display(i18n("Image [%1 × %2] copied to clipboard.", copyPix.width(), copyPix.height()));
                } else if (choice == imageToFile) {
                    // [3] save pixmap to file
                    QString fileName = QFileDialog::getSaveFileName(this, i18n("Save file"), QString(), i18n("Images (*.png *.jpeg)"));
                    if (fileName.isEmpty()) {
                        d->messageWindow->display(i18n("File not saved."), QString(), PageViewMessage::Warning);
                    } else {
                        QMimeDatabase db;
                        QMimeType mime = db.mimeTypeForUrl(QUrl::fromLocalFile(fileName));
                        QString type;
                        if (!mime.isDefault()) {
                            type = QStringLiteral("PNG");
                        } else {
                            type = mime.name().section(QLatin1Char('/'), -1).toUpper();
                        }
                        copyPix.save(fileName, qPrintable(type));
                        d->messageWindow->display(i18n("Image [%1 × %2] saved to %3 file.", copyPix.width(), copyPix.height(), type));
                    }
                }
            }
            // TEXT operation chosen
            else {
                if (choice == textToClipboard) {
                    // [1.1] copy text to clipboard
                    QClipboard *cb = QApplication::clipboard();
                    cb->setText(selectedText, QClipboard::Clipboard);
                    if (cb->supportsSelection()) {
                        cb->setText(selectedText, QClipboard::Selection);
                    }
                } else if (choice == textWithoutLineBreaksToClipboard) {
                    // [1.2] copy text without line breaks to clipboard
                    QClipboard *cb = QApplication::clipboard();
                    cb->setText(Okular::removeLineBreaks(selectedText), QClipboard::Clipboard);
                    if (cb->supportsSelection()) {
                        cb->setText(Okular::removeLineBreaks(selectedText), QClipboard::Selection);
                    }
                }
                else if (choice == readAloudText) {
                    // [2] read the selection aloud
                    readAloud(selectedText);
                }
            }
        }
        // clear widget selection and invalidate rect
        selectionClear();

        // restore previous action if came from it using right button
        if (d->aPrevAction) {
            d->aPrevAction->trigger();
            d->aPrevAction = nullptr;
        }
    } break;

    case Okular::Settings::EnumMouseMode::TableSelect: {
        // if it is a left release checks if is over a previous link press
        if (leftButton && mouseReleaseOverLink(d->mouseOverLinkObject)) {
            selectionClear();
            break;
        }

        // if mouse is released and selection is null this is a rightClick
        if (rightButton && !d->mouseSelecting) {
            PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
            if (pageItem) {
                QMenu *menu = nullptr;
                AnnotationPopup annotPopup(d->document, AnnotationPopup::MultiAnnotationMode, this);
                if (addAnnotationActionsForPoint(annotPopup, pageItem, eventPos, &menu)) {
                    menu->exec(e->globalPosition().toPoint());
                    menu->deleteLater();
                    break;
                }
            }
            Q_EMIT rightClick(pageItem ? pageItem->page() : nullptr, e->globalPosition().toPoint());
            break;
        }

        QRect selectionRect = d->mouseSelectionRect.normalized();
        if (selectionRect.width() <= 8 && selectionRect.height() <= 8 && d->tableSelectionParts.isEmpty()) {
            selectionClear();
            if (d->aPrevAction) {
                d->aPrevAction->trigger();
                d->aPrevAction = nullptr;
            }
            break;
        }

        if (d->mouseSelecting) {
            // break up the selection into page-relative pieces
            d->tableSelectionParts.clear();
            const Okular::Page *okularPage = nullptr;
            for (PageViewItem *item : std::as_const(d->items)) {
                if (!item->isVisible()) {
                    continue;
                }

                const QRect &itemRect = item->croppedGeometry();
                if (selectionRect.intersects(itemRect)) {
                    // request the textpage if there isn't one
                    okularPage = item->page();
                    qCDebug(OkularUiDebug) << "checking if page" << item->pageNumber() << "has text:" << okularPage->hasTextPage();
                    if (!okularPage->hasTextPage()) {
                        d->document->requestTextPage(okularPage->number());
                    }
                    // grab text in the rect that intersects itemRect
                    QRect rectInItem = selectionRect.intersected(itemRect);
                    rectInItem.translate(-item->uncroppedGeometry().topLeft());
                    QRect rectInSelection = selectionRect.intersected(itemRect);
                    rectInSelection.translate(-selectionRect.topLeft());
                    d->tableSelectionParts.append(
                        TableSelectionPart(item, Okular::NormalizedRect(rectInItem, item->uncroppedWidth(), item->uncroppedHeight()), Okular::NormalizedRect(rectInSelection, selectionRect.width(), selectionRect.height())));
                }
            }

            QRect updatedRect = d->mouseSelectionRect.normalized().adjusted(0, 0, 1, 1);
            updatedRect.translate(-contentAreaPosition());
            d->mouseSelecting = false;
            d->mouseSelectionRect.setCoords(0, 0, 0, 0);
            d->tableSelectionCols.clear();
            d->tableSelectionRows.clear();
            guessTableDividers();
            viewport()->update(updatedRect);
        }

        if (!d->document->isAllowed(Okular::AllowCopy)) {
            d->messageWindow->display(i18n("Copy forbidden by DRM"), QString(), PageViewMessage::Info, -1);
            break;
        }

        QClipboard *cb = QApplication::clipboard();
        if (cb->supportsSelection()) {
            cb->setMimeData(getTableContents(), QClipboard::Selection);
        }

    } break;

    case Okular::Settings::EnumMouseMode::TextSelect:
        if (leftButton && d->mouseCommentCandidate) {
            if (clickedCommentAnnotation) {
                openAnnotationWindow(clickedCommentAnnotation, clickedCommentAnnotationPage);
            }
            break;
        }

        // if it is a left release checks if is over a previous link press
        if (leftButton && mouseReleaseOverLink(d->mouseOverLinkObject)) {
            selectionClear();
            break;
        }

        // A click on answer or document feedback opens its editor.
        if (leftButton && !d->mouseTextSelecting && (openReviewFeedbackAt(eventPos) || openDocumentFeedbackAt(eventPos))) break;

        if (d->mouseTextSelecting && d->textDragArea && leftButton) {
            // The drag caught no text: feedback on the area, as with the rectangle tool.
            d->mouseTextSelecting = false;
            d->textDragArea = false;
            showAreaFeedbackMenu(d->mouseSelectionRect.normalized(), e->globalPosition().toPoint());
            break;
        }
        if (d->mouseTextSelecting) {
            d->mouseTextSelecting = false;
            //                    textSelectionClear();
            if (d->document->isAllowed(Okular::AllowCopy)) {
                const QString text = d->selectedText();
                if (!text.isEmpty()) {
                    QClipboard *cb = QApplication::clipboard();
                    if (cb->supportsSelection()) {
                        cb->setText(text, QClipboard::Selection);
                    }
                }
            }
        } else if (!d->mousePressPos.isNull() && rightButton) {
            PageViewItem *item = pickItemOnPoint(eventPos.x(), eventPos.y());
            // if there is text selected in the page
            if (item) {
                if (item->page()->textSelection() && !d->selectedText().isEmpty()) {
                    QMenu *menu = new QMenu(this);
                    menu->setObjectName(QStringLiteral("PopupMenu"));
                    addReviewFeedbackActions(menu);

                    QAction *copyText = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Text"));
                    copyText->setObjectName(QStringLiteral("CopyTextToClipboard"));
                    copyText->setEnabled(d->document->isAllowed(Okular::AllowCopy));

                    // The shared action shows its shortcut; it reads the selection when triggered.
                    menu->addAction(d->aReadAloud);

                    QAction *openTexSource = menu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open source"));
                    openTexSource->setObjectName(QStringLiteral("OpenTexSource"));

                    QAction *highlightText = menu->addAction(QIcon::fromTheme(QStringLiteral("draw-highlight")), i18n("Highlight Text"));
                    highlightText->setObjectName(QStringLiteral("HighlightText"));
                    highlightText->setEnabled(d->document->isAllowed(Okular::AllowNotes));

                    QAction *strikethroughText = menu->addAction(QIcon::fromTheme(QStringLiteral("format-text-strikethrough")), i18n("Strikethrough"));
                    strikethroughText->setObjectName(QStringLiteral("StrikethroughText"));
                    strikethroughText->setEnabled(d->document->isAllowed(Okular::AllowNotes));

                    QAction *createPdfLinkAnchor = nullptr;
                    if (isPdfLinkAnchorDocument()) {
                        createPdfLinkAnchor = menu->addAction(QIcon::fromTheme(QStringLiteral("insert-link")), i18n("Create PDF Link Anchor…"));
                        createPdfLinkAnchor->setObjectName(QStringLiteral("CreatePdfLinkAnchor"));
                        createPdfLinkAnchor->setEnabled(d->document->isAllowed(Okular::AllowNotes));
                    }

                    QAction *addReviewCard = nullptr;
                    QVariantList reviewCardRect;
                    const QUrl documentUrl = d->document->currentDocument();
                    const QFileInfo documentInfo(documentUrl.toLocalFile());
                    const QString currentPdfsPath = QDir::cleanPath(QDir(d->docsRoot).filePath(QStringLiteral("current_pdfs")));
                    const QFileInfo documentDirectoryInfo(documentInfo.absolutePath());
                    const QFileInfo currentPdfsInfo(currentPdfsPath);
                    const QString documentDirectoryPath = documentDirectoryInfo.canonicalFilePath().isEmpty() ? documentDirectoryInfo.absoluteFilePath() : documentDirectoryInfo.canonicalFilePath();
                    const QString canonicalCurrentPdfsPath = currentPdfsInfo.canonicalFilePath().isEmpty() ? currentPdfsInfo.absoluteFilePath() : currentPdfsInfo.canonicalFilePath();
                    const bool isTrackedPdf = !d->docsRoot.isEmpty() && documentUrl.isLocalFile() && documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0 && documentDirectoryPath == canonicalCurrentPdfsPath;
                    // One answer area per page the selection covers: its bounding rectangle there.
                    // Text running over a page break gives one card with an area on each page.
                    QVariantList reviewCardAreas;
                    bool validSelection = isTrackedPdf;
                    for (uint pageNumber = 0; validSelection && pageNumber < d->document->pages(); ++pageNumber) {
                        const Okular::Page *selectedPage = d->document->page(pageNumber);
                        if (!selectedPage || !selectedPage->textSelection() || selectedPage->textSelection()->isEmpty()) {
                            continue;
                        }
                        Okular::NormalizedRect normalizedRect;
                        bool hasNormalizedRect = false;
                        for (const Okular::NormalizedRect &selectionRect : std::as_const(*selectedPage->textSelection())) {
                            if (hasNormalizedRect) {
                                normalizedRect |= selectionRect;
                            } else {
                                normalizedRect = selectionRect;
                                hasNormalizedRect = true;
                            }
                        }
                        validSelection = hasNormalizedRect && normalizedRect.left >= 0.0 && normalizedRect.top >= 0.0 && normalizedRect.right <= 1.0 && normalizedRect.bottom <= 1.0 && normalizedRect.left < normalizedRect.right
                            && normalizedRect.top < normalizedRect.bottom;
                        reviewCardAreas.append(QVariantMap{{QStringLiteral("page"), int(pageNumber) + 1},
                                                           {QStringLiteral("rect"), QVariantList{normalizedRect.left, normalizedRect.top, normalizedRect.right, normalizedRect.bottom}}});
                    }
                    if (validSelection && !reviewCardAreas.isEmpty()) {
                        reviewCardRect = reviewCardAreas.constFirst().toMap().value(QStringLiteral("rect")).toList();
                        addReviewCard = menu->addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")), i18n("Add Review Card…"));
                        addReviewCard->setObjectName(QStringLiteral("AddReviewCard"));
                        if (reviewCardAreas.size() > 1) {
                            addReviewCard->setToolTip(i18np("One card with an answer area on each of the %1 page", "One card with an answer area on each of the %1 pages", reviewCardAreas.size()));
                        }
                    }

                    // What Open source needs, read before the menu opens: while it is open a Sync that
                    // brings a new PDF reloads the document, and the page under it is gone when it
                    // returns (18 Nov crash in Page::textSelection after Open source).
                    const Okular::RegularAreaRect *sourceSelection = item->page() ? item->page()->textSelection() : nullptr;
                    const bool sourceSelected = sourceSelection && !sourceSelection->isEmpty();
                    const int sourcePageNumber = item->pageNumber() + 1;
                    const Okular::NormalizedPoint sourcePoint = sourceSelected ? sourceSelection->constFirst().center() : Okular::NormalizedPoint();
                    const QString sourceText = d->selectedText();
                    openTexSource->setEnabled(sourceSelected);

                    Q_EMIT contextMenuReady(menu);
                    const QAction *choice = menu->exec(e->globalPosition().toPoint());
                    if (choice == copyText) {
                        copyTextSelection(TextCopyMode::AsProvided);
                    } else if (choice == openTexSource) {
                        Q_EMIT openTexSourceRequested(sourcePageNumber, sourcePoint.x, sourcePoint.y, sourceText);
                    } else if (choice == strikethroughText) {
                        annotateTextSelection(Okular::HighlightAnnotation::StrikeOut);
                    } else if (choice == highlightText) {
                        annotateTextSelection();
                    } else if (createPdfLinkAnchor && choice == createPdfLinkAnchor) {
                        promptCreatePdfLinkAnchor();
                    } else if (addReviewCard && choice == addReviewCard) {
                        if (reviewCardAreas.size() > 1) {
                            Q_EMIT manualReviewCardAreasRequested(documentInfo.absoluteFilePath(), reviewCardAreas, true);
                        } else {
                            Q_EMIT manualReviewCardRequested(documentInfo.absoluteFilePath(), reviewCardAreas.constFirst().toMap().value(QStringLiteral("page")).toInt(), reviewCardRect, true);
                        }
                    }

                    menu->deleteLater();
                    break;
                }

                QMenu *menu = createProcessLinkMenu(item, eventPos);
                AnnotationPopup annotPopup(d->document, AnnotationPopup::MultiAnnotationMode, this);
                // Do not move annotPopup inside a conditional, it needs to live until menu->exec()
                addAnnotationActionsForPoint(annotPopup, item, eventPos, &menu);

                QAction *addPopupNoteAction = nullptr;
                QAction *addFeedbackAction = nullptr;
                QAction *addSectionCardAction = nullptr;
                QRectF sectionHeading;
                if (!menu) {
                    menu = new QMenu(this);
                    // A comment stays on every version of the PDF; feedback (here a text box at the
                    // click) belongs to the version it was given on.
                    addPopupNoteAction = menu->addAction(QIcon::fromTheme(QStringLiteral("comment")), i18n("Add Comment"));
                    addPopupNoteAction->setObjectName(QStringLiteral("AddPopupNote"));
                    addPopupNoteAction->setEnabled(d->document->isAllowed(Okular::AllowNotes));
                    if (drawingsVisible() && !hiddenAnswerArea().contains(QPointF(eventPos))) {
                        addFeedbackAction = menu->addAction(QIcon::fromTheme(QStringLiteral("draw-text")), i18n("Add Feedback"));
                        addFeedbackAction->setObjectName(QStringLiteral("AddFeedbackTextBox"));
                    }
                    // A section heading of a Library PDF: a card that hides the whole section.
                    const QUrl documentUrl = d->document->currentDocument();
                    const QFileInfo documentInfo(documentUrl.toLocalFile());
                    const QFileInfo currentPdfs(QDir(d->docsRoot).filePath(QStringLiteral("current_pdfs")));
                    const QFileInfo documentDirectory(documentInfo.absolutePath());
                    const bool libraryPdf = !d->docsRoot.isEmpty() && documentUrl.isLocalFile() && documentInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0
                        && (documentDirectory.canonicalFilePath().isEmpty() ? documentDirectory.absoluteFilePath() : documentDirectory.canonicalFilePath())
                            == (currentPdfs.canonicalFilePath().isEmpty() ? currentPdfs.absoluteFilePath() : currentPdfs.canonicalFilePath());
                    if (libraryPdf && !hiddenAnswerArea().contains(QPointF(eventPos))) sectionHeading = sectionHeadingLineAt(item, eventPos);
                    if (!sectionHeading.isEmpty()) {
                        menu->addSeparator();
                        addSectionCardAction = menu->addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")), i18n("Add Review Card for This Section…"));
                        addSectionCardAction->setObjectName(QStringLiteral("AddSectionReviewCard"));
                        addSectionCardAction->setToolTip(i18n("The card hides the section's text below this heading, up to the next heading of the same or a higher level."));
                    }
                }

                if (menu) {
                    menu->setObjectName(QStringLiteral("PopupMenu"));
                    Q_EMIT contextMenuReady(menu);
                    const QAction *choice = menu->exec(e->globalPosition().toPoint());
                    if (choice == addPopupNoteAction) {
                        addPopupNote(item, eventPos);
                    } else if (choice && choice == addFeedbackAction) {
                        addTextBoxAt(eventPos);
                    } else if (choice && choice == addSectionCardAction) {
                        Q_EMIT sectionReviewCardRequested(QFileInfo(d->document->currentDocument().toLocalFile()).absoluteFilePath(), item->pageNumber() + 1,
                                                          {sectionHeading.left(), sectionHeading.top(), sectionHeading.right(), sectionHeading.bottom()});
                    }
                    menu->deleteLater();
                }
            }
        }
        break;
    }

    // reset mouse press / 'drag start' position
    d->mousePressPos = QPointF();
    d->mouseCommentCandidate = nullptr;
    d->mouseCommentCandidatePage = -1;
}

void PageView::guessTableDividers()
{
    QList<QPair<double, int>> colTicks, rowTicks, colSelectionTicks, rowSelectionTicks;

    for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
        // add ticks for the edges of this area...
        colSelectionTicks.append(qMakePair(tsp.rectInSelection.left, +1));
        colSelectionTicks.append(qMakePair(tsp.rectInSelection.right, -1));
        rowSelectionTicks.append(qMakePair(tsp.rectInSelection.top, +1));
        rowSelectionTicks.append(qMakePair(tsp.rectInSelection.bottom, -1));

        // get the words in this part
        Okular::RegularAreaRect rects;
        rects.append(tsp.rectInItem);
        const Okular::TextEntity::List words = tsp.item->page()->words(&rects, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);

        for (const Okular::TextEntity &te : words) {
            if (te.text().isEmpty()) {
                continue;
            }

            Okular::NormalizedRect wordArea = te.area();

            // convert it from item coordinates to part coordinates
            wordArea.left -= tsp.rectInItem.left;
            wordArea.left /= (tsp.rectInItem.right - tsp.rectInItem.left);
            wordArea.right -= tsp.rectInItem.left;
            wordArea.right /= (tsp.rectInItem.right - tsp.rectInItem.left);
            wordArea.top -= tsp.rectInItem.top;
            wordArea.top /= (tsp.rectInItem.bottom - tsp.rectInItem.top);
            wordArea.bottom -= tsp.rectInItem.top;
            wordArea.bottom /= (tsp.rectInItem.bottom - tsp.rectInItem.top);

            // convert from part coordinates to table coordinates
            wordArea.left *= (tsp.rectInSelection.right - tsp.rectInSelection.left);
            wordArea.left += tsp.rectInSelection.left;
            wordArea.right *= (tsp.rectInSelection.right - tsp.rectInSelection.left);
            wordArea.right += tsp.rectInSelection.left;
            wordArea.top *= (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
            wordArea.top += tsp.rectInSelection.top;
            wordArea.bottom *= (tsp.rectInSelection.bottom - tsp.rectInSelection.top);
            wordArea.bottom += tsp.rectInSelection.top;

            // add to the ticks arrays...
            colTicks.append(qMakePair(wordArea.left, +1));
            colTicks.append(qMakePair(wordArea.right, -1));
            rowTicks.append(qMakePair(wordArea.top, +1));
            rowTicks.append(qMakePair(wordArea.bottom, -1));
        }
    }

    int tally = 0;

    std::sort(colSelectionTicks.begin(), colSelectionTicks.end());
    std::sort(rowSelectionTicks.begin(), rowSelectionTicks.end());

    for (int i = 0; i < colSelectionTicks.length(); ++i) {
        tally += colSelectionTicks[i].second;
        if (tally == 0 && i + 1 < colSelectionTicks.length() && colSelectionTicks[i + 1].first != colSelectionTicks[i].first) {
            colTicks.append(qMakePair(colSelectionTicks[i].first, +1));
            colTicks.append(qMakePair(colSelectionTicks[i + 1].first, -1));
        }
    }
    Q_ASSERT(tally == 0);

    for (int i = 0; i < rowSelectionTicks.length(); ++i) {
        tally += rowSelectionTicks[i].second;
        if (tally == 0 && i + 1 < rowSelectionTicks.length() && rowSelectionTicks[i + 1].first != rowSelectionTicks[i].first) {
            rowTicks.append(qMakePair(rowSelectionTicks[i].first, +1));
            rowTicks.append(qMakePair(rowSelectionTicks[i + 1].first, -1));
        }
    }
    Q_ASSERT(tally == 0);

    std::sort(colTicks.begin(), colTicks.end());
    std::sort(rowTicks.begin(), rowTicks.end());

    for (int i = 0; i < colTicks.length(); ++i) {
        tally += colTicks[i].second;
        if (tally == 0 && i + 1 < colTicks.length() && colTicks[i + 1].first != colTicks[i].first) {
            d->tableSelectionCols.append((colTicks[i].first + colTicks[i + 1].first) / 2);
            d->tableDividersGuessed = true;
        }
    }
    Q_ASSERT(tally == 0);

    for (int i = 0; i < rowTicks.length(); ++i) {
        tally += rowTicks[i].second;
        if (tally == 0 && i + 1 < rowTicks.length() && rowTicks[i + 1].first != rowTicks[i].first) {
            d->tableSelectionRows.append((rowTicks[i].first + rowTicks[i + 1].first) / 2);
            d->tableDividersGuessed = true;
        }
    }
    Q_ASSERT(tally == 0);
}

void PageView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // The Text tool: a double-click on a box edits it, and nothing else happens with it.
    if (d->mouseMode == Okular::Settings::EnumMouseMode::TextBox) {
        if (e->button() == Qt::LeftButton) {
            const QString box = textBoxAt(contentAreaPoint(e->pos()));
            // The release that follows has nothing more to do.
            d->textBoxPressed.clear();
            d->textBoxPressedSelected = false;
            d->textBoxMoved = QRectF();
            if (!box.isEmpty()) editTextBox(box);
        }
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        const QPoint eventPos = contentAreaPoint(e->pos());
        const PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());

        if (pageItem) {
            const QString proposalId = agentProposalAt(pageItem, eventPos);
            if (!proposalId.isEmpty()) {
                d->selectedAgentProposalId = proposalId;
                viewport()->update();
                Q_EMIT agentProposalReviewRequested(proposalId);
                e->accept();
                return;
            }
            // find out normalized mouse coords inside current item
            double nX = pageItem->absToPageX(eventPos.x());
            double nY = pageItem->absToPageY(eventPos.y());
            Okular::Annotation *ann = commentAnnotationAtPoint(pageItem, eventPos);

            if (d->mouseMode == Okular::Settings::EnumMouseMode::TextSelect) {
                if (hasCommentWindow(ann)) {
                    d->mouseCommentCandidate = nullptr;
                    d->mouseCommentCandidatePage = -1;
                    Q_EMIT annotationClicked(ann, pageItem->pageNumber());
                    openAnnotationWindow(ann, pageItem->pageNumber());
                    return;
                }

                // Check for quad-click: double-click shortly after triple-click at same position
                const bool isQuadClick = d->tripleClickDetectionEnabled && d->lastDoubleClickTime.isValid() && d->lastDoubleClickTime.elapsed() < QApplication::doubleClickInterval() &&
                    (eventPos - d->lastDoubleClickPos).manhattanLength() < QApplication::startDragDistance();

                if (isQuadClick) {
                    // QUAD-CLICK: Clear selection and reset
                    textSelectionClear();
                    d->tripleClickDetectionEnabled = false;
                    return;
                }

                // DOUBLE-CLICK: Select word
                textSelectionClear();

                std::unique_ptr<Okular::RegularAreaRect> wordRect = pageItem->page()->wordAt(Okular::NormalizedPoint(nX, nY));
                if (wordRect) {
                    // TODO words with hyphens across pages
                    d->document->setPageTextSelection(pageItem->pageNumber(), std::move(wordRect), palette().color(QPalette::Active, QPalette::Highlight));
                    d->pagesWithTextSelection << pageItem->pageNumber();
                    if (d->document->isAllowed(Okular::AllowCopy)) {
                        const QString text = d->selectedText();
                        if (!text.isEmpty()) {
                            QClipboard *cb = QApplication::clipboard();
                            if (cb->supportsSelection()) {
                                cb->setText(text, QClipboard::Selection);
                            }
                        }
                    }

                    // Enable triple-click detection after successful word selection
                    d->tripleClickDetectionEnabled = true;
                    d->lastDoubleClickTime.start();
                    d->lastDoubleClickPos = eventPos;
                    return;
                }
            }

            if (ann && ann->subType() != Okular::Annotation::AWidget) {
                Q_EMIT annotationClicked(ann, pageItem->pageNumber());
                openAnnotationWindow(ann, pageItem->pageNumber());
            }
        }
    }
}

void PageView::wheelEvent(QWheelEvent *e)
{
    d->agentProposalLastWheel.start();
    if (!d->document->isOpened()) {
        QAbstractScrollArea::wheelEvent(e);
        return;
    }

    int delta = e->angleDelta().y(), vScroll = verticalScrollBar()->value();
    e->accept();

    if ((e->modifiers() & Qt::ControlModifier) == Qt::ControlModifier) {
        // Ctrl key is pressed, perform zoom instead of scroll

        // If left click is pressed while doing Ctrl+Wheel, we must temporarily fake a release else
        // QScroller will ignore scrollTo calls which are are required to properly center
        // the view on the mouse pointer instead of the middle of the viewport.
        bool isDragging = d->scroller->state() == QScroller::Pressed || d->scroller->state() == QScroller::Dragging;

        if (isDragging) {
            d->scroller->handleInput(QScroller::InputRelease, e->position(), e->timestamp() - 1);
        }

        float newZoom = d->zoomFactor * (1.0 + (delta / 500.0));
        zoomWithFixedCenter(ZoomRefreshCurrent, e->position(), newZoom);

        if (isDragging) {
            d->scroller->handleInput(QScroller::InputPress, e->position(), e->timestamp());
        }

        // remainingScroll is tracking the distance between where we wanted to zoom in and the real center.
        // This error can exist because of trying to zoom out near the border of a page.
        // With Ctrl + wheel, there is no start or end of zooming, so always reset remaningScroll.
        d->remainingScroll = QPointF(0.0, 0.0);
        return;
    }

    // Perform scroll
    if (!getContinuousMode() && vScroll == verticalScrollBar()->maximum() && delta < 0) {
        d->singlePageWheelAccumulatedDelta += delta;
        // go to next page
        if (d->singlePageWheelAccumulatedDelta <= -QWheelEvent::DefaultDeltasPerStep) {
            d->singlePageWheelAccumulatedDelta = 0;
            if ((int)d->document->currentPage() < d->items.count() - 1) {
                // more optimized than document->setNextPage and then move view to top
                Okular::DocumentViewport newViewport = d->document->viewport();
                newViewport.pageNumber += viewColumns();
                if (newViewport.pageNumber >= (int)d->items.count()) {
                    newViewport.pageNumber = d->items.count() - 1;
                }
                newViewport.rePos.enabled = true;
                newViewport.rePos.normalizedY = 0.0;
                d->document->setViewport(newViewport);
                d->scroller->scrollTo(QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()), 0); // sync scroller with scrollbar
            }
        }
    } else if (!getContinuousMode() && vScroll == verticalScrollBar()->minimum() && delta > 0) {
        d->singlePageWheelAccumulatedDelta += delta;
        // go to prev page
        if (d->singlePageWheelAccumulatedDelta >= QWheelEvent::DefaultDeltasPerStep) {
            d->singlePageWheelAccumulatedDelta = 0;
            if (d->document->currentPage() > 0) {
                // more optimized than document->setPrevPage and then move view to bottom
                Okular::DocumentViewport newViewport = d->document->viewport();
                newViewport.pageNumber -= viewColumns();
                if (newViewport.pageNumber < 0) {
                    newViewport.pageNumber = 0;
                }
                newViewport.rePos.enabled = true;
                newViewport.rePos.normalizedY = 1.0;
                d->document->setViewport(newViewport);
                d->scroller->scrollTo(QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()), 0); // sync scroller with scrollbar
            }
        }
    } else {
        d->singlePageWheelAccumulatedDelta = 0;
        // When the shift key is held down, scroll ten times faster
        int multiplier = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;

        if (delta != 0 && delta % QWheelEvent::DefaultDeltasPerStep == 0) {
            // number of scroll wheel steps Qt gives to us at the same time
            int count = abs(delta / QWheelEvent::DefaultDeltasPerStep) * multiplier;
            if (delta < 0) {
                slotScrollDown(count);
            } else {
                slotScrollUp(count);
            }
        } else {
            d->scroller->scrollTo(d->scroller->finalPosition() - e->angleDelta() * multiplier, 0);
        }
    }
}

bool PageView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::Leave) {
        // Out of the page view (not into the toolbar, a child of the viewport).
        d->agentProposalHoverPointerValid = false;
        d->agentProposalHoverShowTimer->stop();
        d->agentProposalHoverCandidate.clear();
        if (d->agentProposalHoverBar && d->agentProposalHoverBar->isVisible() && !d->agentProposalHoverBar->pointerInside()) {
            d->agentProposalHoverHideTimer->start();
        }
    }
    if (e->type() == QEvent::ToolTip
        // Show tool tips only for those modes that change the cursor
        // to a hand when hovering over the link.
        && (d->mouseMode == Okular::Settings::EnumMouseMode::Browse || d->mouseMode == Okular::Settings::EnumMouseMode::RectSelect || d->mouseMode == Okular::Settings::EnumMouseMode::TextSelect ||
            d->mouseMode == Okular::Settings::EnumMouseMode::TrimSelect)) {
        QHelpEvent *he = static_cast<QHelpEvent *>(e);
        if (!(d->annotator && d->annotator->active()) && !agentProposalMarkerAt(contentAreaPoint(he->pos())).isEmpty()) {
            const QString markerId = agentProposalMarkerAt(contentAreaPoint(he->pos()));
            QToolTip::showText(he->globalPos(),
                               agentProposalDecisionState(markerId).decidable ? i18nc("@info:tooltip on a proposal's check mark", "Click to accept · hold for more")
                                                                              : i18nc("@info:tooltip on a proposal's check mark", "Click to open · hold for more"),
                               viewport());
            e->accept();
            return true;
        }
        bool overlayMarker = false;
        const QStringList overlayMark = (d->annotator && d->annotator->active()) || d->mouseAnnotation->isMouseOver()
            ? QStringList() : readingOverlayMarkAt(contentAreaPoint(he->pos()), &overlayMarker);
        const QString overlayComment = overlayMark.isEmpty()
            ? QString() : Thoughts::ReadingOverlays::commentOf(Thoughts::ReadingOverlays::markOf(d->readingOverlays->record(overlayMark.at(0)), overlayMark.at(1)));
        if (overlayMarker && !overlayComment.isEmpty()) {
            // The agent's comment on the mark (reading-overlays.md §4).
            QToolTip::showText(he->globalPos(), overlayComment, viewport());
            e->accept();
            return true;
        }
        if (d->mouseAnnotation->isMouseOver()) {
            d->mouseAnnotation->routeTooltipEvent(he);
        } else {
            const QPoint eventPos = contentAreaPoint(he->pos());
            const PageViewItem *pageItem = pickItemOnPoint(eventPos.x(), eventPos.y());
            const Okular::ObjectRect *rect = nullptr;
            const Okular::Action *link = nullptr;
            if (pageItem) {
                double nX = pageItem->absToPageX(eventPos.x());
                double nY = pageItem->absToPageY(eventPos.y());
                rect = pageItem->page()->objectRect(Okular::ObjectRect::Action, nX, nY, pageItem->uncroppedWidth(), pageItem->uncroppedHeight());
                if (rect) {
                    link = static_cast<const Okular::Action *>(rect->object());
                }
            }

            if (link) {
                QRect r = rect->boundingRect(pageItem->uncroppedWidth(), pageItem->uncroppedHeight());
                r.translate(pageItem->uncroppedGeometry().topLeft());
                r.translate(-contentAreaPosition());
                QString tip = link->actionTip();
                if (!tip.isEmpty()) {
                    QToolTip::showText(he->globalPos(), tip, viewport(), r);
                }
            } else if (!overlayComment.isEmpty()) {
                // Over a wash: its comment too.
                QToolTip::showText(he->globalPos(), overlayComment, viewport());
            }
        }
        e->accept();
        return true;
    } else {
        // do not stop the event
        return QAbstractScrollArea::viewportEvent(e);
    }
}

void PageView::scrollContentsBy(int dx, int dy)
{
    // The page moved under the pointer: no proposal toolbar now, and a press on a marker
    // accepts nothing.
    d->agentProposalLastScroll.start();
    if (!d->pressedAgentProposalMarker.isEmpty()) {
        d->agentProposalPressCancelled = true;
        d->agentProposalHoldTimer->stop();
    }
    hideAgentProposalHoverBar();
    const QRect r = viewport()->rect();
    viewport()->scroll(dx, dy, r);
    // HACK manually repaint the damaged regions, as it seems some updates are missed
    // thus leaving artifacts around
    QRegion rgn(r);
    rgn -= rgn & r.translated(dx, dy);

    for (const QRect &rect : rgn) {
        viewport()->update(rect);
    }

    placeTextBoxEditor();
    updateCursor();
}
// END widget events

std::vector<std::unique_ptr<Okular::RegularAreaRect>> PageView::textSelections(const QPoint start, const QPoint end, int &firstpage)
{
    firstpage = -1;
    std::vector<std::unique_ptr<Okular::RegularAreaRect>> ret;
    QSet<int> affectedItemsSet;
    QRect selectionRect = QRect::span(start, end);
    for (const PageViewItem *item : std::as_const(d->items)) {
        if (item->isVisible() && selectionRect.intersects(item->croppedGeometry())) {
            affectedItemsSet.insert(item->pageNumber());
        }
    }
#ifdef PAGEVIEW_DEBUG
    qCDebug(OkularUiDebug) << ">>>> item selected by mouse:" << affectedItemsSet.count();
#endif

    if (!affectedItemsSet.isEmpty()) {
        // is the mouse drag line the ne-sw diagonal of the selection rect?
        bool direction_ne_sw = start == selectionRect.topRight() || start == selectionRect.bottomLeft();

        int tmpmin = d->document->pages();
        int tmpmax = 0;
        for (const int p : std::as_const(affectedItemsSet)) {
            if (p < tmpmin) {
                tmpmin = p;
            }
            if (p > tmpmax) {
                tmpmax = p;
            }
        }

        const PageViewItem *a = pickItemOnPoint((int)(direction_ne_sw ? selectionRect.right() : selectionRect.left()), (int)selectionRect.top());
        int min = a && (a->pageNumber() != tmpmax) ? a->pageNumber() : tmpmin;
        const PageViewItem *b = pickItemOnPoint((int)(direction_ne_sw ? selectionRect.left() : selectionRect.right()), (int)selectionRect.bottom());
        int max = b && (b->pageNumber() != tmpmin) ? b->pageNumber() : tmpmax;

        QList<int> affectedItemsIds;
        for (int i = min; i <= max; ++i) {
            affectedItemsIds.append(i);
        }
#ifdef PAGEVIEW_DEBUG
        qCDebug(OkularUiDebug) << ">>>> pages:" << affectedItemsIds;
#endif
        firstpage = affectedItemsIds.first();

        if (affectedItemsIds.count() == 1) {
            const PageViewItem *item = d->items[affectedItemsIds.first()];
            selectionRect.translate(-item->uncroppedGeometry().topLeft());
            ret.push_back(textSelectionForItem(item, direction_ne_sw ? selectionRect.topRight() : selectionRect.topLeft(), direction_ne_sw ? selectionRect.bottomLeft() : selectionRect.bottomRight()));
        } else if (affectedItemsIds.count() > 1) {
            // first item
            const PageViewItem *first = d->items[affectedItemsIds.first()];
            QRect geom = first->croppedGeometry().intersected(selectionRect).translated(-first->uncroppedGeometry().topLeft());
            ret.push_back(textSelectionForItem(first, selectionRect.bottom() > geom.height() ? (direction_ne_sw ? geom.topRight() : geom.topLeft()) : (direction_ne_sw ? geom.bottomRight() : geom.bottomLeft()), QPoint()));
            // last item
            const PageViewItem *last = d->items[affectedItemsIds.last()];
            geom = last->croppedGeometry().intersected(selectionRect).translated(-last->uncroppedGeometry().topLeft());
            // the last item needs to appended at last...
            std::unique_ptr<Okular::RegularAreaRect> lastArea =
                textSelectionForItem(last, QPoint(), selectionRect.bottom() > geom.height() ? (direction_ne_sw ? geom.bottomLeft() : geom.bottomRight()) : (direction_ne_sw ? geom.topLeft() : geom.topRight()));
            affectedItemsIds.removeFirst();
            affectedItemsIds.removeLast();
            // item between the two above
            for (const int page : std::as_const(affectedItemsIds)) {
                ret.push_back(textSelectionForItem(d->items[page]));
            }
            ret.push_back(std::move(lastArea));
        }
    }
    return ret;
}

void PageView::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        styleScrollBars();
    }
}

void PageView::styleScrollBars()
{
    // Thin neutral bars on the canvas, the same colour as what they scroll over.
    const QString style = Thoughts::ReaderTheme::scrollBars(palette(), Thoughts::ReaderTheme::canvas(palette()));
    verticalScrollBar()->setStyleSheet(style);
    horizontalScrollBar()->setStyleSheet(style);
}

void PageView::drawDocumentOnPainter(const QRect contentsRect, QPainter *p)
{
    QColor backColor;

    if (Okular::Settings::useCustomBackgroundColor()) {
        backColor = Okular::Settings::backgroundColor();
    } else {
        backColor = Thoughts::ReaderTheme::canvas(palette());
    }

    // create a region from which we'll subtract painted rects
    QRegion remainingArea(contentsRect);

    // Agents' reading overlays (reading-overlays.md §4): their washes on the page image, under the
    // text selection, highlights and annotations, as on no other painting of the pages.
    const PagePainter::UnderlayScope readingOverlays(readingOverlaysVisible() ? PagePainter::Underlay([this](QPainter *painter, const Okular::Page *page,
                                                                                                            const std::function<QRectF(const QRectF &)> &map) {
        d->readingOverlays->paintWashes(painter, int(page->number()) + 1, map);
    })
                                                                                                      : PagePainter::Underlay());

    // This loop draws the actual pages
    // iterate over all items painting the ones intersecting contentsRect
    for (const PageViewItem *item : std::as_const(d->items)) {
        // check if a piece of the page intersects the contents rect
        if (!item->isVisible() || !item->croppedGeometry().intersects(contentsRect)) {
            continue;
        }

        // get item and item's outline geometries
        const QRect itemGeometry = item->croppedGeometry();

        // move the painter to the top-left corner of the real page
        p->save();
        p->translate(itemGeometry.left(), itemGeometry.top());

        // draw the page using the PagePainter with all flags active
        if (contentsRect.intersects(itemGeometry)) {
            Okular::NormalizedPoint *viewPortPoint = nullptr;
            Okular::NormalizedPoint point(d->lastSourceLocationViewportNormalizedX, d->lastSourceLocationViewportNormalizedY);
            if (Okular::Settings::showSourceLocationsGraphically() && item->pageNumber() == d->lastSourceLocationViewportPageNumber) {
                viewPortPoint = &point;
            }
            const QRect pixmapRect = contentsRect.intersected(itemGeometry).translated(-item->croppedGeometry().topLeft());
            int visibleFlags = pageflags;
            if (!Okular::Settings::highlightComments()) visibleFlags |= PagePainter::HideComments;
            if (d->agentProposalTextHighlight && !Okular::Settings::highlightAgentEdits()) visibleFlags &= ~PagePainter::TextSelection;
            PagePainter::paintCroppedPageOnPainter(p, item->page(), this, visibleFlags, item->uncroppedWidth(), item->uncroppedHeight(), pixmapRect, item->crop(), viewPortPoint);
        }

        // remove painted area from 'remainingArea' and restore painter
        remainingArea -= itemGeometry;
        p->restore();
    }

    // fill the visible area around the page with the background color
    for (const QRect &backRect : remainingArea) {
        p->fillRect(backRect, backColor);
    }

    // take outline and shadow into account when testing whether a repaint is necessary
    const qreal dpr = devicePixelRatioF();
    const QRect checkRect = contentsRect.adjusted(-3, -3, 1, 1);

    // Method to linearly interpolate between black (=(0,0,0), omitted) and the background color
    auto interpolateColor = [&backColor](double t) { return QColor(t * backColor.red(), t * backColor.green(), t * backColor.blue()); };

    // width of the shadow in device pixels
    static const int shadowWidth = 2 * dpr;

    // iterate over all items painting a faint outline and a simple bottom/right gradient
    for (const PageViewItem *item : std::as_const(d->items)) {
        // check if a piece of the page intersects the contents rect
        if (!item->isVisible() || !item->croppedGeometry().intersects(checkRect)) {
            continue;
        }

        // get item and item's outline geometries
        const QRect itemGeometry = item->croppedGeometry();

        // move the painter to the top-left corner of the real page
        p->save();
        p->translate(itemGeometry.left(), itemGeometry.top());

        // draw the page outline (faint border and bottom-right shadow)
        if (!itemGeometry.contains(contentsRect)) {
            const int itemWidth = itemGeometry.width();
            const int itemHeight = itemGeometry.height();
            // draw simple outline
            QPen pen(backColor.darker(125));
            pen.setWidth(0);
            p->setPen(pen);

            const QRectF outline(-1.0 / dpr, -1.0 / dpr, itemWidth + 1.0 / dpr, itemHeight + 1.0 / dpr);
            p->drawRect(outline);

            // draw bottom/right gradient
            for (int i = 1; i <= shadowWidth; i++) {
                pen.setColor(interpolateColor(0.75 + 0.25 * double(i) / (shadowWidth + 1)));
                p->setPen(pen);
                const QPointF left((i - 1) / dpr, itemHeight + i / dpr);
                const QPointF up(itemWidth + i / dpr, (i - 1) / dpr);
                const QPointF corner(itemWidth + i / dpr, itemHeight + i / dpr);
                p->drawLine(left, corner);
                p->drawLine(up, corner);
            }
        }

        p->restore();
    }
}

void PageView::updateItemSize(PageViewItem *item, int colWidth, int rowHeight)
{
    const Okular::Page *okularPage = item->page();
    double width = okularPage->width(), height = okularPage->height(), zoom = d->zoomFactor;
    Okular::NormalizedRect crop(0., 0., 1., 1.);

    // Handle cropping, due to either "Trim Margin" or "Trim to Selection" cases
    if ((Okular::Settings::trimMargins() && okularPage->isBoundingBoxKnown() && !okularPage->boundingBox().isNull()) || (d->aTrimToSelection && d->aTrimToSelection->isChecked() && !d->trimBoundingBox.isNull())) {
        crop = Okular::Settings::trimMargins() ? okularPage->boundingBox() : d->trimBoundingBox;

        // Rotate the bounding box
        for (int i = okularPage->rotation(); i > 0; --i) {
            Okular::NormalizedRect rot = crop;
            crop.left = 1 - rot.bottom;
            crop.top = rot.left;
            crop.right = 1 - rot.top;
            crop.bottom = rot.right;
        }

        // Expand the crop slightly beyond the bounding box (for Trim Margins only)
        if (Okular::Settings::trimMargins()) {
            static const double cropExpandRatio = 0.04;
            const double cropExpand = cropExpandRatio * ((crop.right - crop.left) + (crop.bottom - crop.top)) / 2;
            crop = Okular::NormalizedRect(crop.left - cropExpand, crop.top - cropExpand, crop.right + cropExpand, crop.bottom + cropExpand) & Okular::NormalizedRect(0, 0, 1, 1);
        }

        // We currently generate a larger image and then crop it, so if the
        // crop rect is very small the generated image is huge. Hence, we shouldn't
        // let the crop rect become too small.
        static double minCropRatio;
        if (Okular::Settings::trimMargins()) {
            // Make sure we crop by at most 50% in either dimension:
            minCropRatio = 0.5;
        } else {
            // Looser Constraint for "Trim Selection"
            minCropRatio = 0.20;
        }
        if ((crop.right - crop.left) < minCropRatio) {
            const double newLeft = (crop.left + crop.right) / 2 - minCropRatio / 2;
            crop.left = qMax(0.0, qMin(1.0 - minCropRatio, newLeft));
            crop.right = crop.left + minCropRatio;
        }
        if ((crop.bottom - crop.top) < minCropRatio) {
            const double newTop = (crop.top + crop.bottom) / 2 - minCropRatio / 2;
            crop.top = qMax(0.0, qMin(1.0 - minCropRatio, newTop));
            crop.bottom = crop.top + minCropRatio;
        }

        width *= (crop.right - crop.left);
        height *= (crop.bottom - crop.top);
#ifdef PAGEVIEW_DEBUG
        qCDebug(OkularUiDebug) << "Cropped page" << okularPage->number() << "to" << crop << "width" << width << "height" << height << "by bbox" << okularPage->boundingBox();
#endif
    }

    if (d->zoomMode == ZoomFixed) {
        width *= zoom;
        height *= zoom;
        item->setWHZC((int)width, (int)height, d->zoomFactor, crop);
    } else if (d->zoomMode == ZoomFitWidth) {
        height = (height / width) * colWidth;
        zoom = (double)colWidth / width;
        item->setWHZC(colWidth, (int)height, zoom, crop);
        if ((uint)item->pageNumber() == d->document->currentPage()) {
            d->zoomFactor = zoom;
        }
    } else if (d->zoomMode == ZoomFitPage) {
        const double scaleW = (double)colWidth / (double)width;
        const double scaleH = (double)rowHeight / (double)height;
        zoom = qMin(scaleW, scaleH);
        item->setWHZC((int)(zoom * width), (int)(zoom * height), zoom, crop);
        if ((uint)item->pageNumber() == d->document->currentPage()) {
            d->zoomFactor = zoom;
        }
    } else if (d->zoomMode == ZoomFitAuto) {
        const double aspectRatioRelation = 1.25; // relation between aspect ratios for "auto fit"
        const double uiAspect = (double)rowHeight / (double)colWidth;
        const double pageAspect = (double)height / (double)width;
        const double rel = uiAspect / pageAspect;

        if (!getContinuousMode() && rel > aspectRatioRelation) {
            // UI space is relatively much higher than the page
            zoom = (double)rowHeight / (double)height;
        } else if (rel < 1.0 / aspectRatioRelation) {
            // UI space is relatively much wider than the page in relation
            zoom = (double)colWidth / (double)width;
        } else {
            // aspect ratios of page and UI space are very similar
            const double scaleW = (double)colWidth / (double)width;
            const double scaleH = (double)rowHeight / (double)height;
            zoom = qMin(scaleW, scaleH);
        }
        item->setWHZC((int)(zoom * width), (int)(zoom * height), zoom, crop);
        if ((uint)item->pageNumber() == d->document->currentPage()) {
            d->zoomFactor = zoom;
        }
    }
#ifndef NDEBUG
    else {
        qCDebug(OkularUiDebug) << "calling updateItemSize with unrecognized d->zoomMode!";
    }
#endif
}

PageViewItem *PageView::pickItemOnPoint(int x, int y)
{
    PageViewItem *item = nullptr;
    for (PageViewItem *i : std::as_const(d->visibleItems)) {
        const QRect &r = i->croppedGeometry();
        if (x < r.right() && x > r.left() && y < r.bottom()) {
            if (y > r.top()) {
                item = i;
            }
            break;
        }
    }
    return item;
}

void PageView::textSelectionClear()
{
    d->agentProposalTextHighlight = false;
    d->placeHighlight = false;
    // something to clear
    if (!d->pagesWithTextSelection.isEmpty()) {
        for (const int page : std::as_const(d->pagesWithTextSelection)) {
            d->document->setPageTextSelection(page, nullptr, QColor());
        }
        d->pagesWithTextSelection.clear();
    }
}

void PageView::selectionStart(const QPoint pos, const QColor &color, bool /*aboveAll*/)
{
    selectionClear();
    d->mouseSelecting = true;
    d->mouseSelectionRect.setRect(pos.x(), pos.y(), 1, 1);
    d->mouseSelectionColor = color;
    // ensures page doesn't scroll
    if (d->autoScrollTimer) {
        d->scrollIncrement = 0;
        d->autoScrollTimer->stop();
    }
}

void PageView::scrollPosIntoView(const QPoint pos)
{
    // this number slows the speed of the page by its value, chosen not to be too fast or too slow, the actual speed is determined from the mouse position, not critical
    const int damping = 6;

    if (pos.x() < horizontalScrollBar()->value()) {
        d->dragScrollVector.setX((pos.x() - horizontalScrollBar()->value()) / damping);
    } else if (horizontalScrollBar()->value() + viewport()->width() < pos.x()) {
        d->dragScrollVector.setX((pos.x() - horizontalScrollBar()->value() - viewport()->width()) / damping);
    } else {
        d->dragScrollVector.setX(0);
    }

    if (pos.y() < verticalScrollBar()->value()) {
        d->dragScrollVector.setY((pos.y() - verticalScrollBar()->value()) / damping);
    } else if (verticalScrollBar()->value() + viewport()->height() < pos.y()) {
        d->dragScrollVector.setY((pos.y() - verticalScrollBar()->value() - viewport()->height()) / damping);
    } else {
        d->dragScrollVector.setY(0);
    }

    if (d->dragScrollVector != QPoint(0, 0)) {
        if (!d->dragScrollTimer.isActive()) {
            d->dragScrollTimer.start(1000 / 60); // 60 fps
        }
    } else {
        d->dragScrollTimer.stop();
    }
}

QPoint PageView::viewportToContentArea(const Okular::DocumentViewport &vp) const
{
    Q_ASSERT(vp.pageNumber >= 0);

    const QRect &r = d->items[vp.pageNumber]->croppedGeometry();
    QPoint c {r.left(), r.top()};

    if (vp.rePos.enabled) {
        // Convert the coordinates of vp to normalized coordinates on the cropped page.
        // This is a no-op if the page isn't cropped.
        const Okular::NormalizedRect &crop = d->items[vp.pageNumber]->crop();
        const double normalized_on_crop_x = (vp.rePos.normalizedX - crop.left) / (crop.right - crop.left);
        const double normalized_on_crop_y = (vp.rePos.normalizedY - crop.top) / (crop.bottom - crop.top);

        if (vp.rePos.pos == Okular::DocumentViewport::Center) {
            c.rx() += qRound(normClamp(normalized_on_crop_x, 0.5) * (double)r.width());
            c.ry() += qRound(normClamp(normalized_on_crop_y, 0.0) * (double)r.height());
        } else {
            // TopLeft
            c.rx() += qRound(normClamp(normalized_on_crop_x, 0.0) * (double)r.width() + viewport()->width() / 2.0);
            c.ry() += qRound(normClamp(normalized_on_crop_y, 0.0) * (double)r.height() + viewport()->height() / 2.0);
        }
    } else {
        // exact repositioning disabled, align page top margin with viewport top border by default
        c.rx() += r.width() / 2;
        c.ry() += viewport()->height() / 2 - 10;
    }
    return c;
}

void PageView::updateSelection(const QPoint pos)
{
    if (d->mouseSelecting) {
        scrollPosIntoView(pos);
        // update the selection rect
        QRect updateRect = d->mouseSelectionRect;
        d->mouseSelectionRect.setBottomLeft(pos);
        updateRect |= d->mouseSelectionRect;
        updateRect.translate(-contentAreaPosition());
        viewport()->update(updateRect.adjusted(-1, -2, 2, 1));
    } else if (d->mouseTextSelecting) {
        scrollPosIntoView(pos);
        int first = -1;
        std::vector<std::unique_ptr<Okular::RegularAreaRect>> selections = textSelections(pos, d->mouseSelectPos.toPoint(), first);
        QSet<int> pagesWithSelectionSet;
        for (size_t i = 0; i < selections.size(); ++i) {
            pagesWithSelectionSet.insert(i + first);
        }

        const QSet<int> noMoreSelectedPages = d->pagesWithTextSelection - pagesWithSelectionSet;
        // clear the selection from pages not selected anymore
        for (int p : noMoreSelectedPages) {
            d->document->setPageTextSelection(p, nullptr, QColor());
        }
        // set the new selection for the selected pages
        for (int p : std::as_const(pagesWithSelectionSet)) {
            d->document->setPageTextSelection(p, std::move(selections[p - first]), palette().color(QPalette::Active, QPalette::Highlight));
        }
        d->pagesWithTextSelection = pagesWithSelectionSet;
    }
}

static Okular::NormalizedPoint rotateInNormRect(const QPoint rotated, const QRect rect, Okular::Rotation rotation)
{
    Okular::NormalizedPoint ret;

    switch (rotation) {
    case Okular::Rotation0:
        ret = Okular::NormalizedPoint(rotated.x(), rotated.y(), rect.width(), rect.height());
        break;
    case Okular::Rotation90:
        ret = Okular::NormalizedPoint(rotated.y(), rect.width() - rotated.x(), rect.height(), rect.width());
        break;
    case Okular::Rotation180:
        ret = Okular::NormalizedPoint(rect.width() - rotated.x(), rect.height() - rotated.y(), rect.width(), rect.height());
        break;
    case Okular::Rotation270:
        ret = Okular::NormalizedPoint(rect.height() - rotated.y(), rotated.x(), rect.height(), rect.width());
        break;
    }

    return ret;
}

std::unique_ptr<Okular::RegularAreaRect> PageView::textSelectionForItem(const PageViewItem *item, const QPoint startPoint, const QPoint endPoint)
{
    const QRect &geometry = item->uncroppedGeometry();
    Okular::NormalizedPoint startCursor(0.0, 0.0);
    if (!startPoint.isNull()) {
        startCursor = rotateInNormRect(startPoint, geometry, item->page()->rotation());
    }
    Okular::NormalizedPoint endCursor(1.0, 1.0);
    if (!endPoint.isNull()) {
        endCursor = rotateInNormRect(endPoint, geometry, item->page()->rotation());
    }
    Okular::TextSelection mouseTextSelectionInfo(startCursor, endCursor);

    const Okular::Page *okularPage = item->page();

    if (!okularPage->hasTextPage()) {
        d->document->requestTextPage(okularPage->number());
    }

    std::unique_ptr<Okular::RegularAreaRect> selectionArea = okularPage->textArea(mouseTextSelectionInfo);
#ifdef PAGEVIEW_DEBUG
    qCDebug(OkularUiDebug).nospace() << "text areas (" << okularPage->number() << "): " << (selectionArea ? QString::number(selectionArea->count()) : QStringLiteral("(none)"));
#endif
    return selectionArea;
}

void PageView::selectionClear(const ClearMode mode)
{
    QRect updatedRect = d->mouseSelectionRect.normalized().adjusted(-2, -2, 2, 2);
    d->mouseSelecting = false;
    d->mouseSelectionRect.setCoords(0, 0, 0, 0);
    d->tableSelectionCols.clear();
    d->tableSelectionRows.clear();
    d->tableDividersGuessed = false;
    for (const TableSelectionPart &tsp : std::as_const(d->tableSelectionParts)) {
        QRect selectionPartRect = tsp.rectInItem.geometry(tsp.item->uncroppedWidth(), tsp.item->uncroppedHeight());
        selectionPartRect.translate(tsp.item->uncroppedGeometry().topLeft());
        // should check whether this is on-screen here?
        updatedRect = updatedRect.united(selectionPartRect);
    }
    if (mode != ClearOnlyDividers) {
        d->tableSelectionParts.clear();
    }
    d->tableSelectionParts.clear();
    updatedRect.translate(-contentAreaPosition());
    viewport()->update(updatedRect);
}

// const to be used for both zoomFactorFitMode function and slotRelayoutPages.
static const int kcolWidthMargin = 6;
static const int krowHeightMargin = 12;

double PageView::zoomFactorFitMode(ZoomMode mode)
{
    const int pageCount = d->items.count();
    if (pageCount == 0) {
        return 0;
    }
    const bool facingCentered = Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::FacingFirstCentered || (Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::Facing && pageCount == 1);
    const bool overrideCentering = facingCentered && pageCount < 3;
    const int nCols = overrideCentering ? 1 : viewColumns();
    const int colWidth = viewport()->width() / nCols - kcolWidthMargin;
    const double rowHeight = viewport()->height() - krowHeightMargin;
    const PageViewItem *currentItem = d->items[qMax(0, (int)d->document->currentPage())];
    // prevent segmentation fault when opening a new document;
    if (!currentItem) {
        return 0;
    }

    // We need the real width/height of the cropped page.
    const Okular::Page *okularPage = currentItem->page();
    const double width = okularPage->width() * currentItem->crop().width();
    const double height = okularPage->height() * currentItem->crop().height();

    if (mode == ZoomFitWidth) {
        return (double)colWidth / width;
    }
    if (mode == ZoomFitPage) {
        const double scaleW = (double)colWidth / (double)width;
        const double scaleH = (double)rowHeight / (double)height;
        return qMin(scaleW, scaleH);
    }
    return 0;
}

static double parseZoomString(QString z)
{
    // kdelibs4 sometimes adds accelerators to actions' text directly :(
    z.remove(QLatin1Char('&'));
    z.remove(QLatin1Char('%'));
    return QLocale().toDouble(z) / 100.0;
}

static QString makePrettyZoomString(double value)
{
    // we do not need to display 2-digit precision
    QString localValue(QLocale().toString(value * 100.0, 'f', 1));
    localValue.remove(QLocale().decimalPoint() + QLatin1Char('0'));
    // remove a trailing zero in numbers like 66.70
    if (localValue.right(1) == QLatin1String("0") && localValue.indexOf(QLocale().decimalPoint()) > -1) {
        localValue.chop(1);
    }
    return localValue;
}

void PageView::updateZoom(ZoomMode newZoomMode)
{
    if (newZoomMode == ZoomFixed) {
        if (d->aZoom->currentItem() == 0) {
            newZoomMode = ZoomFitWidth;
        } else if (d->aZoom->currentItem() == 1) {
            newZoomMode = ZoomFitPage;
        } else if (d->aZoom->currentItem() == 2) {
            newZoomMode = ZoomFitAuto;
        }
    }

    float newFactor = d->zoomFactor;
    const QAction *checkedZoomAction = nullptr;
    switch (newZoomMode) {
    case ZoomFixed: { // ZoomFixed case
        newFactor = parseZoomString(d->aZoom->currentText());
    } break;
    case ZoomIn:
    case ZoomOut: {
        const float zoomFactorFitWidth = zoomFactorFitMode(ZoomFitWidth);
        const float zoomFactorFitPage = zoomFactorFitMode(ZoomFitPage);

        QList<float> zoomValue(kZoomValues.size());

        std::copy(kZoomValues.begin(), kZoomValues.end(), zoomValue.begin());
        zoomValue.append(zoomFactorFitWidth);
        zoomValue.append(zoomFactorFitPage);
        std::sort(zoomValue.begin(), zoomValue.end());

        QList<float>::iterator i;
        if (newZoomMode == ZoomOut) {
            if (newFactor <= zoomValue.first()) {
                return;
            }
            i = std::lower_bound(zoomValue.begin(), zoomValue.end(), newFactor) - 1;
        } else {
            if (newFactor >= zoomValue.last()) {
                return;
            }
            i = std::upper_bound(zoomValue.begin(), zoomValue.end(), newFactor);
        }
        const float tmpFactor = *i;
        if (tmpFactor == zoomFactorFitWidth) {
            newZoomMode = ZoomFitWidth;
            checkedZoomAction = d->aZoomFitWidth;
        } else if (tmpFactor == zoomFactorFitPage) {
            newZoomMode = ZoomFitPage;
            checkedZoomAction = d->aZoomFitPage;
        } else {
            newFactor = tmpFactor;
            newZoomMode = ZoomFixed;
        }
    } break;
    case ZoomActual:
        newZoomMode = ZoomFixed;
        newFactor = 1.0;
        break;
    case ZoomFitWidth:
        checkedZoomAction = d->aZoomFitWidth;
        break;
    case ZoomFitPage:
        checkedZoomAction = d->aZoomFitPage;
        break;
    case ZoomFitAuto:
        checkedZoomAction = d->aZoomAutoFit;
        break;
    case ZoomRefreshCurrent:
        newZoomMode = ZoomFixed;
        d->zoomFactor = -1;
        break;
    }
    const float upperZoomLimit = d->document->supportsTiles() ? 100.0 : 4.0;
    if (newFactor > upperZoomLimit) {
        newFactor = upperZoomLimit;
    }
    if (newFactor < kZoomValues[0]) {
        newFactor = kZoomValues[0];
    }

    if (newZoomMode != d->zoomMode || (newZoomMode == ZoomFixed && newFactor != d->zoomFactor)) {
        // rebuild layout and update the whole viewport
        d->zoomMode = newZoomMode;
        d->zoomFactor = newFactor;
        // be sure to block updates to document's viewport
        bool prevState = d->blockViewport;
        d->blockViewport = true;
        slotRelayoutPages();
        d->blockViewport = prevState;
        // request pixmaps
        slotRequestVisiblePixmaps();
        // update zoom text
        updateZoomText();
        // update actions checked state
        if (d->aZoomFitWidth) {
            d->aZoomFitWidth->setChecked(checkedZoomAction == d->aZoomFitWidth);
            d->aZoomFitPage->setChecked(checkedZoomAction == d->aZoomFitPage);
            d->aZoomAutoFit->setChecked(checkedZoomAction == d->aZoomAutoFit);
        }
    } else if (newZoomMode == ZoomFixed && newFactor == d->zoomFactor) {
        updateZoomText();
    }

    updateZoomActionsEnabledStatus();
}

void PageView::updateZoomActionsEnabledStatus()
{
    const float upperZoomLimit = d->document->supportsTiles() ? kZoomValues.back() : 4.0;
    const bool hasPages = d->document && d->document->pages() > 0;

    if (d->aZoomFitWidth) {
        d->aZoomFitWidth->setEnabled(hasPages);
    }
    if (d->aZoomFitPage) {
        d->aZoomFitPage->setEnabled(hasPages);
    }
    if (d->aZoomAutoFit) {
        d->aZoomAutoFit->setEnabled(hasPages);
    }
    if (d->aZoom) {
        d->aZoom->selectableActionGroup()->setEnabled(hasPages);
        d->aZoom->setEnabled(hasPages);
    }
    if (d->aZoomIn) {
        d->aZoomIn->setEnabled(hasPages && d->zoomFactor < upperZoomLimit - 0.001);
    }
    if (d->aZoomOut) {
        d->aZoomOut->setEnabled(hasPages && d->zoomFactor > (kZoomValues[0] + 0.001));
    }
    if (d->aZoomActual) {
        d->aZoomActual->setEnabled(hasPages && d->zoomFactor != 1.0);
    }
}

void PageView::updateZoomText()
{
    // use current page zoom as zoomFactor if in ZoomFit/* mode
    if (d->zoomMode != ZoomFixed && d->items.count() > 0) {
        d->zoomFactor = d->items[qMax(0, (int)d->document->currentPage())]->zoomFactor();
    }
    float newFactor = d->zoomFactor;
    d->aZoom->removeAllActions();

    // add items that describe fit actions
    QStringList translated;
    translated << i18n("Fit Width") << i18n("Fit Page") << i18n("Auto Fit");

    // add percent items
    int idx = 0, selIdx = 3;
    bool inserted = false; // use: "d->zoomMode != ZoomFixed" to hide Fit/* zoom ratio
    int zoomValueCount = 11;
    if (d->document->supportsTiles()) {
        zoomValueCount = kZoomValues.size();
    }
    while (idx < zoomValueCount || !inserted) {
        float value = idx < zoomValueCount ? kZoomValues[idx] : newFactor;
        if (!inserted && newFactor < (value - 0.0001)) {
            value = newFactor;
        } else {
            idx++;
        }
        if (value > (newFactor - 0.0001) && value < (newFactor + 0.0001)) {
            inserted = true;
        }
        if (!inserted) {
            selIdx++;
        }
        const QString localizedValue = makePrettyZoomString(value);
        const QString i18nZoomName = i18nc("Zoom percentage value %1 will be replaced by the actual zoom factor value, so make sure you include it in your translation in order to not to break anything", "%1%", localizedValue);
        if (makePrettyZoomString(parseZoomString(i18nZoomName)) == localizedValue) {
            translated << i18nZoomName;
        } else {
            qWarning() << "Wrong translation of zoom percentage. Please file a bug";
            translated << QStringLiteral("%1%").arg(localizedValue);
        }
    }
    d->aZoom->setItems(translated);

    // select current item in list
    if (d->zoomMode == ZoomFitWidth) {
        selIdx = 0;
    } else if (d->zoomMode == ZoomFitPage) {
        selIdx = 1;
    } else if (d->zoomMode == ZoomFitAuto) {
        selIdx = 2;
    }
    // we have to temporarily enable the actions as otherwise we can't set a new current item
    d->aZoom->setEnabled(true);
    d->aZoom->selectableActionGroup()->setEnabled(true);
    d->aZoom->setCurrentItem(selIdx);
    d->aZoom->setEnabled(d->items.size() > 0);
    d->aZoom->selectableActionGroup()->setEnabled(d->items.size() > 0);
}

void PageView::updateViewMode(const int nr)
{
    const QList<QAction *> actions = d->viewModeActionGroup->actions();
    for (QAction *action : actions) {
        QVariant mode_id = action->data();
        if (mode_id.toInt() == nr) {
            action->trigger();
        }
    }
}

void PageView::updateCursor()
{
    const QPoint p = contentAreaPosition() + viewport()->mapFromGlobal(QCursor::pos());
    updateCursor(p);
}

void PageView::updateCursor(const QPoint p)
{
    const Okular::ObjectRect *previousMouseOverLinkObject = d->mouseOverLinkObject;
    const auto repaintLinkHoverChange = qScopeGuard([this, previousMouseOverLinkObject] {
        if (previousMouseOverLinkObject != d->mouseOverLinkObject) {
            viewport()->update();
        }
    });

    // reset mouse over link it will be re-set if that still valid
    d->mouseOverLinkObject = nullptr;

    // detect the underlaying page (if present)
    const PageViewItem *pageItem = pickItemOnPoint(p.x(), p.y());
    QScroller::State scrollerState = d->scroller->state();

    if (d->annotator && d->annotator->active()) {
        if (pageItem || d->annotator->annotating()) {
            setCursor(d->annotator->cursor());
        } else {
            setCursor(Qt::ForbiddenCursor);
        }
    } else if (scrollerState == QScroller::Pressed || scrollerState == QScroller::Dragging) {
        setCursor(Qt::ClosedHandCursor);
    } else if (!agentProposalMarkerAt(p).isEmpty()) {
        setCursor(Qt::PointingHandCursor);
    } else if (bool marker = false; !readingOverlayMarkAt(p, &marker).isEmpty() && marker) {
        setCursor(Qt::PointingHandCursor);
    } else if (pageItem) {
        double nX = pageItem->absToPageX(p.x());
        double nY = pageItem->absToPageY(p.y());
        Qt::CursorShape cursorShapeFallback;

        // if over a ObjectRect (of type Link) change cursor to hand
        switch (d->mouseMode) {
        case Okular::Settings::EnumMouseMode::TextSelect:
            if (d->mouseTextSelecting) {
                setCursor(Qt::IBeamCursor);
                return;
            }
            if (Okular::Annotation *annotation = commentAnnotationAtPoint(pageItem, p)) {
                setCursor(isDraggablePopupNote(annotation) ? GuiUtils::moveCursorShape(d->mouseAnnotation->isMoved()) : Qt::PointingHandCursor);
                return;
            }
            cursorShapeFallback = Qt::IBeamCursor;
            break;
        case Okular::Settings::EnumMouseMode::Magnifier:
            setCursor(Qt::CrossCursor);
            return;
        case Okular::Settings::EnumMouseMode::Draw:
            setCursor(Okular::Settings::pencilEraser() ? eraserCursor() : QCursor(Qt::CrossCursor));
            return;
        case Okular::Settings::EnumMouseMode::TextBox:
            setCursor(!d->textBoxMoved.isEmpty() ? Qt::ClosedHandCursor : !textBoxAt(p).isEmpty() ? Qt::OpenHandCursor : Qt::IBeamCursor);
            return;
        case Okular::Settings::EnumMouseMode::RectSelect:
        case Okular::Settings::EnumMouseMode::TrimSelect:
            if (d->mouseSelecting) {
                setCursor(Qt::CrossCursor);
                return;
            }
            cursorShapeFallback = Qt::CrossCursor;
            break;
        case Okular::Settings::EnumMouseMode::Browse:
            d->mouseOnRect = false;
            if (d->mouseAnnotation->isMouseOver()) {
                d->mouseOnRect = true;
                setCursor(d->mouseAnnotation->cursor());
                return;
            } else {
                cursorShapeFallback = Qt::OpenHandCursor;
            }
            break;
        default:
            setCursor(Qt::ArrowCursor);
            return;
        }

        const Okular::ObjectRect *linkobj = pageItem->page()->objectRect(Okular::ObjectRect::Action, nX, nY, pageItem->uncroppedWidth(), pageItem->uncroppedHeight());
        if (linkobj) {
            d->mouseOverLinkObject = linkobj;
            d->mouseOnRect = true;
            setCursor(Qt::PointingHandCursor);
        } else {
            setCursor(cursorShapeFallback);
        }
    } else {
        // if there's no page over the cursor and we were showing the pointingHandCursor
        // go back to the normal one
        d->mouseOnRect = false;
        setCursor(Qt::ArrowCursor);
    }
}

void PageView::reloadForms()
{
    if (d->m_formsVisible) {
        for (PageViewItem *item : std::as_const(d->visibleItems)) {
            item->reloadFormWidgetsState();
        }
    }
}

void PageView::moveMagnifier(const QPoint p) // non scaled point
{
    const int w = d->magnifierView->width() * 0.5;
    const int h = d->magnifierView->height() * 0.5;

    int x = p.x() - w;
    int y = p.y() - h;

    const int max_x = viewport()->width();
    const int max_y = viewport()->height();

    QPoint scroll(0, 0);

    if (x < 0) {
        if (horizontalScrollBar()->value() > 0) {
            scroll.setX(x - w);
        }
        x = 0;
    }

    if (y < 0) {
        if (verticalScrollBar()->value() > 0) {
            scroll.setY(y - h);
        }
        y = 0;
    }

    if (p.x() + w > max_x) {
        if (horizontalScrollBar()->value() < horizontalScrollBar()->maximum()) {
            scroll.setX(p.x() + 2 * w - max_x);
        }
        x = max_x - d->magnifierView->width() - 1;
    }

    if (p.y() + h > max_y) {
        if (verticalScrollBar()->value() < verticalScrollBar()->maximum()) {
            scroll.setY(p.y() + 2 * h - max_y);
        }
        y = max_y - d->magnifierView->height() - 1;
    }

    if (!scroll.isNull()) {
        scrollPosIntoView(contentAreaPoint(p + scroll));
    }

    d->magnifierView->doMove(x, y);
}

void PageView::updateMagnifier(const QPoint p) // scaled point
{
    /* translate mouse coordinates to page coordinates and inform the magnifier of the situation */
    const PageViewItem *item = pickItemOnPoint(p.x(), p.y());
    if (item) {
        Okular::NormalizedPoint np(item->absToPageX(p.x()), item->absToPageY(p.y()));
        d->magnifierView->updateView(np, item->page());
    }
}

int PageView::viewColumns() const
{
    int vm = Okular::Settings::viewMode();
    if (vm == Okular::Settings::EnumViewMode::Single) {
        return 1;
    } else if (vm == Okular::Settings::EnumViewMode::Facing || vm == Okular::Settings::EnumViewMode::FacingFirstCentered) {
        return 2;
    } else if (vm == Okular::Settings::EnumViewMode::Summary && d->document->pages() < Okular::Settings::viewColumns()) {
        return d->document->pages();
    } else {
        return Okular::Settings::viewColumns();
    }
}

void PageView::center(int cx, int cy, bool smoothMove)
{
    scrollTo(cx - viewport()->width() / 2, cy - viewport()->height() / 2, smoothMove);
}

void PageView::scrollTo(int x, int y, bool smoothMove)
{
    bool prevState = d->blockPixmapsRequest;

    int newValue = -1;
    if (x != horizontalScrollBar()->value() || y != verticalScrollBar()->value()) {
        newValue = 1; // Pretend this call is the result of a scrollbar event
    }

    d->blockPixmapsRequest = true;

    if (smoothMove) {
        d->scroller->scrollTo(QPoint(x, y), d->currentLongScrollDuration);
    } else {
        d->scroller->scrollTo(QPoint(x, y), 0);
    }

    d->blockPixmapsRequest = prevState;

    slotRequestVisiblePixmaps(newValue);
}

void PageView::toggleFormWidgets(bool on)
{
    bool somehadfocus = false;
    for (PageViewItem *item : std::as_const(d->items)) {
        const bool hadfocus = item->setFormWidgetsVisible(on);
        somehadfocus = somehadfocus || hadfocus;
    }
    if (somehadfocus) {
        setFocus();
    }
    d->m_formsVisible = on;
}

void PageView::resizeContentArea(const QSize newSize)
{
    const QSize vs = viewport()->size();
    int hRange = newSize.width() - vs.width();
    int vRange = newSize.height() - vs.height();
    if (horizontalScrollBar()->isVisible() && hRange == verticalScrollBar()->width() && verticalScrollBar()->isVisible() && vRange == horizontalScrollBar()->height() && Okular::Settings::showScrollBars()) {
        hRange = 0;
        vRange = 0;
    }
    horizontalScrollBar()->setRange(0, hRange);
    verticalScrollBar()->setRange(0, vRange);
    updatePageStep();
}

void PageView::updatePageStep()
{
    const QSize vs = viewport()->size();
    horizontalScrollBar()->setPageStep(vs.width());
    verticalScrollBar()->setPageStep(vs.height() * (100 - Okular::Settings::scrollOverlap()) / 100);
}

void PageView::addWebShortcutsMenu(QMenu *menu, const QString &text)
{
    if (text.isEmpty()) {
        return;
    }

    QString searchText = text;
    searchText = searchText.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' ')).simplified();

    if (searchText.isEmpty()) {
        return;
    }

    KUriFilterData filterData(searchText);

    filterData.setSearchFilteringOptions(KUriFilterData::RetrievePreferredSearchProvidersOnly);

    if (KUriFilter::self()->filterSearchUri(filterData, KUriFilter::NormalTextFilter)) {
        const QStringList searchProviders = filterData.preferredSearchProviders();

        if (!searchProviders.isEmpty()) {
            QMenu *webShortcutsMenu = new QMenu(menu);
            webShortcutsMenu->setIcon(QIcon::fromTheme(QStringLiteral("preferences-web-browser-shortcuts")));

            const QString squeezedText = KStringHandler::rsqueeze(searchText, searchTextPreviewLength);
            webShortcutsMenu->setTitle(i18n("Search for '%1' with", squeezedText));

            QAction *action = nullptr;

            for (const QString &searchProvider : searchProviders) {
                action = new QAction(searchProvider, webShortcutsMenu);
                action->setIcon(QIcon::fromTheme(filterData.iconNameForPreferredSearchProvider(searchProvider)));
                action->setData(filterData.queryForPreferredSearchProvider(searchProvider));
                connect(action, &QAction::triggered, this, &PageView::slotHandleWebShortcutAction);
                webShortcutsMenu->addAction(action);
            }

            webShortcutsMenu->addSeparator();

            action = new QAction(i18n("Configure Web Shortcuts…"), webShortcutsMenu);
            action->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
            connect(action, &QAction::triggered, this, &PageView::slotConfigureWebShortcuts);
            webShortcutsMenu->addAction(action);

            menu->addMenu(webShortcutsMenu);
        }
    }
}

QMenu *PageView::createProcessLinkMenu(PageViewItem *item, const QPoint eventPos)
{
    // check if the right-click was over a link
    const double nX = item->absToPageX(eventPos.x());
    const double nY = item->absToPageY(eventPos.y());
    const Okular::ObjectRect *rect = item->page()->objectRect(Okular::ObjectRect::Action, nX, nY, item->uncroppedWidth(), item->uncroppedHeight());
    if (rect) {
        const Okular::Action *link = static_cast<const Okular::Action *>(rect->object());

        if (!link) {
            return nullptr;
        }

        QMenu *menu = new QMenu(this);

        // creating the menu and its actions
        QAction *processLink = menu->addAction(i18n("Follow This Link"));
        processLink->setObjectName(QStringLiteral("ProcessLinkAction"));
        if (link->actionType() == Okular::Action::Sound) {
            processLink->setText(i18n("Play this Sound"));
            if (Okular::AudioPlayer::instance()->state() == Okular::AudioPlayer::PlayingState) {
                QAction *actStopSound = menu->addAction(i18n("Stop Sound"));
                connect(actStopSound, &QAction::triggered, []() { Okular::AudioPlayer::instance()->stopPlaybacks(); });
            }
        }

        if (dynamic_cast<const Okular::BrowseAction *>(link)) {
            QAction *actCopyLinkLocation = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Link Address"));
            actCopyLinkLocation->setObjectName(QStringLiteral("CopyLinkLocationAction"));
            connect(actCopyLinkLocation, &QAction::triggered, menu, [link]() {
                const Okular::BrowseAction *browseLink = static_cast<const Okular::BrowseAction *>(link);
                QClipboard *cb = QApplication::clipboard();
                cb->setText(browseLink->url().toDisplayString(), QClipboard::Clipboard);
                if (cb->supportsSelection()) {
                    cb->setText(browseLink->url().toDisplayString(), QClipboard::Selection);
                }
            });
        }

        const int page = item->pageNumber();
        connect(processLink, &QAction::triggered, this, [this, link, page]() {
            if (!d->linkOverrideClick || !d->linkOverrideClick(link, page)) {
                d->document->processAction(link);
            }
        });
        if (d->linkOverrideMenu) {
            d->linkOverrideMenu(menu, link, page);
        }
        return menu;
    }
    return nullptr;
}

bool PageView::addAnnotationActionsForPoint(AnnotationPopup &annotPopup, PageViewItem *pageItem, const QPoint eventPos, QMenu **menu)
{
    if (!pageItem) {
        return false;
    }

    const QRect &itemRect = pageItem->uncroppedGeometry();
    const double nX = pageItem->absToPageX(eventPos.x());
    const double nY = pageItem->absToPageY(eventPos.y());
    const QList<const Okular::ObjectRect *> annotRects = pageItem->page()->objectRects(Okular::ObjectRect::OAnnotation, nX, nY, itemRect.width(), itemRect.height());
    if (annotRects.isEmpty()) {
        return false;
    }

    bool hasAnnotation = false;
    for (const Okular::ObjectRect *annotRect : annotRects) {
        Okular::Annotation *ann = static_cast<const Okular::AnnotationObjectRect *>(annotRect)->annotation();
        if (ann && !(ann->flags() & Okular::Annotation::Hidden) && (ann->subType() != Okular::Annotation::AWidget)
            && (Okular::Settings::highlightComments() || !Okular::AnnotationUtils::isCommentAnnotation(ann))) {
            annotPopup.addAnnotation(ann, pageItem->pageNumber());
            hasAnnotation = true;
        }
    }

    if (!hasAnnotation) {
        return false;
    }

    connect(&annotPopup, &AnnotationPopup::openAnnotationWindow, this, &PageView::openAnnotationWindow);
    connect(&annotPopup, &AnnotationPopup::replyToAnnotation, this, &PageView::promptReplyToAnnotation);

    if (!*menu) {
        *menu = new QMenu(this);
    }
    annotPopup.addActionsToMenu(*menu);
    return true;
}

void PageView::addSearchWithinDocumentAction(QMenu *menu, const QString &searchText)
{
    const QString squeezedText = KStringHandler::rsqueeze(searchText, searchTextPreviewLength);
    QAction *action = new QAction(i18n("Search for '%1' in this document", squeezedText.simplified()), menu);
    action->setIcon(QIcon::fromTheme(QStringLiteral("document-preview")));
    connect(action, &QAction::triggered, this, [this, searchText] { Q_EMIT triggerSearch(searchText); });
    menu->addAction(action);
}

void PageView::updateSmoothScrollAnimationSpeed()
{
    // If it's turned off in Okular's own settings, don't bother to look at the
    // global settings
    if (!Okular::Settings::smoothScrolling()) {
        d->currentShortScrollDuration = 0;
        d->currentLongScrollDuration = 0;
        return;
    }

    // If we are using smooth scrolling, scale the speed of the animated
    // transitions according to the global animation speed setting
    KConfigGroup kdeglobalsConfig = KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("KDE"));
    const qreal globalAnimationScale = qMax(0.0, kdeglobalsConfig.readEntry("AnimationDurationFactor", 1.0));
    d->currentShortScrollDuration = d->baseShortScrollDuration * globalAnimationScale;
    d->currentLongScrollDuration = d->baseLongScrollDuration * globalAnimationScale;
}

bool PageView::getContinuousMode() const
{
    return d->aViewContinuous ? d->aViewContinuous->isChecked() : Okular::Settings::viewContinuous();
}

void PageView::zoomWithFixedCenter(PageView::ZoomMode newZoomMode, QPointF zoomCenter, float newZoom)
{
    const Okular::DocumentViewport &vp = d->document->viewport();
    Q_ASSERT(vp.pageNumber >= 0);

    // determine the page below zoom center
    const QPoint contentPos = contentAreaPoint(zoomCenter.toPoint());
    const PageViewItem *page = pickItemOnPoint(contentPos.x(), contentPos.y());
    const int hScrollBarMaximum = horizontalScrollBar()->maximum();
    const int vScrollBarMaximum = verticalScrollBar()->maximum();

    // if the zoom center is not over a page, use viewport page number
    if (!page) {
        page = d->items[vp.pageNumber];
    }

    const QRect beginGeometry = page->croppedGeometry();

    QPoint offset {beginGeometry.left(), beginGeometry.top()};

    const QPointF oldScroll = contentAreaPosition() - offset;

    d->blockPixmapsRequest = true;
    if (newZoom) {
        d->zoomFactor = newZoom;
    }

    updateZoom(newZoomMode);
    d->blockPixmapsRequest = false;

    const QRect afterGeometry = page->croppedGeometry();
    const double vpZoomY = (double)afterGeometry.height() / (double)beginGeometry.height();
    const double vpZoomX = (double)afterGeometry.width() / (double)beginGeometry.width();

    QPointF newScroll;
    // The calculation for newScroll is taken from Gwenview class Abstractimageview::setZoom
    newScroll.setY(vpZoomY * (oldScroll.y() + zoomCenter.y()) - (zoomCenter.y()));
    newScroll.setX(vpZoomX * (oldScroll.x() + zoomCenter.x()) - (zoomCenter.x()));

    // add the remaining scroll from the previous zoom event
    newScroll.setY(newScroll.y() + d->remainingScroll.y() * vpZoomY);
    newScroll.setX(newScroll.x() + d->remainingScroll.x() * vpZoomX);

    // adjust newScroll to the new margins after zooming
    offset = QPoint {afterGeometry.left(), afterGeometry.top()};
    newScroll += offset;

    // adjust newScroll for appear and disappear of the scrollbars
    if (Okular::Settings::showScrollBars()) {
        if (hScrollBarMaximum == 0 && horizontalScrollBar()->maximum() > 0) {
            newScroll.setY(newScroll.y() - (horizontalScrollBar()->height() / 2.0));
        }

        if (hScrollBarMaximum > 0 && horizontalScrollBar()->maximum() == 0) {
            newScroll.setY(newScroll.y() + (horizontalScrollBar()->height() / 2.0));
        }

        if (vScrollBarMaximum == 0 && verticalScrollBar()->maximum() > 0) {
            newScroll.setX(newScroll.x() - (verticalScrollBar()->width() / 2.0));
        }

        if (vScrollBarMaximum > 0 && verticalScrollBar()->maximum() == 0) {
            newScroll.setX(newScroll.x() + (verticalScrollBar()->width() / 2.0));
        }
    }

    const int newScrollX = std::round(newScroll.x());
    const int newScrollY = std::round(newScroll.y());
    scrollTo(newScrollX, newScrollY, false);

    viewport()->setUpdatesEnabled(true);
    viewport()->update();

    // test if target scroll position was reached, if not save
    // the difference in d->remainingScroll for later use
    const QPointF diffF = newScroll - contentAreaPosition();
    if (abs(diffF.x()) < 0.5 && abs(diffF.y()) < 0.5) {
        // scroll target reached set d->remainingScroll to 0.0
        d->remainingScroll = QPointF(0.0, 0.0);
    } else {
        d->remainingScroll = diffF;
    }
}

// BEGIN private SLOTS
void PageView::slotRelayoutPages()
// called by: notifySetup, viewportResizeEvent, slotViewMode, slotContinuousToggled, updateZoom
{
    // set an empty container if we have no pages
    const int pageCount = d->items.count();
    if (pageCount < 1) {
        return;
    }

    int viewportWidth = viewport()->width(), viewportHeight = viewport()->height(), fullWidth = 0, fullHeight = 0;

    // handle the 'center first page in row' stuff
    const bool facing = Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::Facing && pageCount > 1;
    const bool facingCentered = Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::FacingFirstCentered || (Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::Facing && pageCount == 1);
    const bool overrideCentering = facingCentered && pageCount < 3;
    const bool centerFirstPage = facingCentered && !overrideCentering;
    const bool facingPages = facing || centerFirstPage;
    const bool centerLastPage = centerFirstPage && pageCount % 2 == 0;
    const bool continuousView = getContinuousMode();
    const int nCols = overrideCentering ? 1 : viewColumns();
    const bool singlePageViewMode = Okular::Settings::viewMode() == Okular::Settings::EnumViewMode::Single;

    if (d->aFitWindowToPage) {
        d->aFitWindowToPage->setEnabled(!continuousView && singlePageViewMode);
    }

    // set all items geometry and resize contents. handle 'continuous' and 'single' modes separately

    const PageViewItem *currentItem = d->items[qMax(0, (int)d->document->currentPage())];

    // Here we find out column's width and row's height to compute a table
    // so we can place widgets 'centered in virtual cells'.
    const int nRows = (int)ceil((float)(centerFirstPage ? (pageCount + nCols - 1) : pageCount) / (float)nCols);

    int *colWidth = new int[nCols], *rowHeight = new int[nRows], cIdx = 0, rIdx = 0;
    for (int i = 0; i < nCols; i++) {
        colWidth[i] = viewportWidth / nCols;
    }
    for (int i = 0; i < nRows; i++) {
        rowHeight[i] = 0;
    }
    // handle the 'centering on first row' stuff
    if (centerFirstPage) {
        cIdx += nCols - 1;
    }

    // 1) find the maximum columns width and rows height for a grid in
    // which each page must well-fit inside a cell
    for (PageViewItem *item : std::as_const(d->items)) {
        // update internal page size (leaving a little margin in case of Fit* modes)
        updateItemSize(item, colWidth[cIdx] - kcolWidthMargin, viewportHeight - krowHeightMargin);
        // find row's maximum height and column's max width
        if (item->croppedWidth() + kcolWidthMargin > colWidth[cIdx]) {
            colWidth[cIdx] = item->croppedWidth() + kcolWidthMargin;
        }
        if (item->croppedHeight() + krowHeightMargin > rowHeight[rIdx]) {
            rowHeight[rIdx] = item->croppedHeight() + krowHeightMargin;
        }
        // handle the 'centering on first row' stuff
        // update col/row indices
        if (++cIdx == nCols) {
            cIdx = 0;
            rIdx++;
        }
    }

    const int pageRowIdx = ((centerFirstPage ? nCols - 1 : 0) + currentItem->pageNumber()) / nCols;

    // 2) compute full size
    for (int i = 0; i < nCols; i++) {
        fullWidth += colWidth[i];
    }
    if (continuousView) {
        for (int i = 0; i < nRows; i++) {
            fullHeight += rowHeight[i];
        }
    } else {
        fullHeight = rowHeight[pageRowIdx];
    }

    // 3) arrange widgets inside cells (and refine fullHeight if needed)
    int insertX = 0, insertY = fullHeight < viewportHeight ? (viewportHeight - fullHeight) / 2 : 0;
    const int origInsertY = insertY;
    cIdx = 0;
    rIdx = 0;
    if (centerFirstPage) {
        cIdx += nCols - 1;
        for (int i = 0; i < cIdx; ++i) {
            insertX += colWidth[i];
        }
    }
    for (PageViewItem *item : std::as_const(d->items)) {
        int cWidth = colWidth[cIdx], rHeight = rowHeight[rIdx];
        if (continuousView || rIdx == pageRowIdx) {
            const bool reallyDoCenterFirst = item->pageNumber() == 0 && centerFirstPage;
            const bool reallyDoCenterLast = item->pageNumber() == pageCount - 1 && centerLastPage;
            int actualX = 0;
            if (reallyDoCenterFirst || reallyDoCenterLast) {
                // page is centered across entire viewport
                actualX = (fullWidth - item->croppedWidth()) / 2;
            } else if (facingPages) {
                if (Okular::Settings::rtlReadingDirection()) {
                    // RTL reading mode
                    actualX = ((centerFirstPage && item->pageNumber() % 2 == 0) || (!centerFirstPage && item->pageNumber() % 2 == 1)) ? (fullWidth / 2) - item->croppedWidth() - 1 : (fullWidth / 2) + 1;
                } else {
                    // page edges 'touch' the center of the viewport
                    actualX = ((centerFirstPage && item->pageNumber() % 2 == 1) || (!centerFirstPage && item->pageNumber() % 2 == 0)) ? (fullWidth / 2) - item->croppedWidth() - 1 : (fullWidth / 2) + 1;
                }
            } else {
                // page is centered within its virtual column
                // actualX = insertX + (cWidth - item->croppedWidth()) / 2;
                if (Okular::Settings::rtlReadingDirection()) {
                    actualX = fullWidth - insertX - cWidth + ((cWidth - item->croppedWidth()) / 2);
                } else {
                    actualX = insertX + (cWidth - item->croppedWidth()) / 2;
                }
            }
            item->moveTo(actualX, (continuousView ? insertY : origInsertY) + (rHeight - item->croppedHeight()) / 2);
            item->setVisible(true);
        } else {
            item->moveTo(0, 0);
            item->setVisible(false);
        }
        item->setFormWidgetsVisible(d->m_formsVisible);
        // advance col/row index
        insertX += cWidth;
        if (++cIdx == nCols) {
            cIdx = 0;
            rIdx++;
            insertX = 0;
            insertY += rHeight;
        }
#ifdef PAGEVIEW_DEBUG
        qWarning() << "updating size for pageno" << item->pageNumber() << "cropped" << item->croppedGeometry() << "uncropped" << item->uncroppedGeometry();
#endif
    }

    delete[] colWidth;
    delete[] rowHeight;

    // 3) reset dirty state
    d->dirtyLayout = false;

    // 4) update scrollview's contents size and recenter view
    bool wasUpdatesEnabled = viewport()->updatesEnabled();
    if (fullWidth != contentAreaWidth() || fullHeight != contentAreaHeight()) {
        const Okular::DocumentViewport vp = d->document->viewport();
        // disable updates and resize the viewportContents
        if (wasUpdatesEnabled) {
            viewport()->setUpdatesEnabled(false);
        }
        resizeContentArea(QSize(fullWidth, fullHeight));
        // restore previous viewport if defined and updates enabled
        if (wasUpdatesEnabled && !d->pinchZoomActive) {
            if (vp.pageNumber >= 0) {
                int prevX = horizontalScrollBar()->value(), prevY = verticalScrollBar()->value();

                const QPoint centerPos = viewportToContentArea(vp);
                center(centerPos.x(), centerPos.y());

                // center() usually moves the viewport, that requests pixmaps too.
                // if that doesn't happen we have to request them by hand
                if (prevX == horizontalScrollBar()->value() && prevY == verticalScrollBar()->value()) {
                    slotRequestVisiblePixmaps();
                }
            }
            // or else go to center page
            else {
                center(fullWidth / 2, 0);
            }
            viewport()->setUpdatesEnabled(true);
        }
    } else {
        slotRequestVisiblePixmaps();
    }

    // 5) update the whole viewport if updated enabled
    if (wasUpdatesEnabled && !d->pinchZoomActive) {
        viewport()->update();
    }

    // Zoom or a layout change moved the marker the toolbar belongs to.
    checkAgentProposalHoverAnchor();
    placeTextBoxEditor();
}

void PageView::delayedResizeEvent()
{
    // If we already got here we don't need to execute the timer slot again
    d->delayResizeEventTimer->stop();
    slotRelayoutPages();
    slotRequestVisiblePixmaps();
}

static void slotRequestPreloadPixmap(PageView *pageView, const PageViewItem *i, const QRect expandedViewportRect, QList<Okular::PixmapRequest *> *requestedPixmaps)
{
    Okular::NormalizedRect preRenderRegion;
    const QRect intersectionRect = expandedViewportRect.intersected(i->croppedGeometry());
    if (!intersectionRect.isEmpty()) {
        preRenderRegion = Okular::NormalizedRect(intersectionRect.translated(-i->uncroppedGeometry().topLeft()), i->uncroppedWidth(), i->uncroppedHeight());
    }

    // request the pixmap if not already present
    if (!i->page()->hasPixmap(pageView, i->uncroppedWidth(), i->uncroppedHeight(), preRenderRegion) && i->uncroppedWidth() > 0) {
        Okular::PixmapRequest::PixmapRequestFeatures requestFeatures = Okular::PixmapRequest::Preload;
        requestFeatures |= Okular::PixmapRequest::Asynchronous;
        const bool pageHasTilesManager = i->page()->hasTilesManager(pageView);
        if (pageHasTilesManager && !preRenderRegion.isNull()) {
            Okular::PixmapRequest *p = new Okular::PixmapRequest(pageView, i->pageNumber(), i->uncroppedWidth(), i->uncroppedHeight(), pageView->devicePixelRatioF(), PAGEVIEW_PRELOAD_PRIO, requestFeatures);
            requestedPixmaps->push_back(p);

            p->setNormalizedRect(preRenderRegion);
            p->setTile(true);
        } else if (!pageHasTilesManager) {
            Okular::PixmapRequest *p = new Okular::PixmapRequest(pageView, i->pageNumber(), i->uncroppedWidth(), i->uncroppedHeight(), pageView->devicePixelRatioF(), PAGEVIEW_PRELOAD_PRIO, requestFeatures);
            requestedPixmaps->push_back(p);
            p->setNormalizedRect(preRenderRegion);
        }
    }
}

void PageView::slotRequestVisiblePixmaps(int newValue)
{
    // if requests are blocked (because raised by an unwanted event), exit
    if (d->blockPixmapsRequest) {
        return;
    }

    // precalc view limits for intersecting with page coords inside the loop
    const bool isEvent = newValue != -1 && !d->blockViewport;
    const QRectF viewportRect(horizontalScrollBar()->value(), verticalScrollBar()->value(), viewport()->width(), viewport()->height());
    const QRectF viewportRectAtZeroZero(0, 0, viewport()->width(), viewport()->height());

    // some variables used to determine the viewport
    int nearPageNumber = -1;
    const double viewportCenterX = (viewportRect.left() + viewportRect.right()) / 2.0;
    const double viewportCenterY = (viewportRect.top() + viewportRect.bottom()) / 2.0;
    double focusedX = 0.5, focusedY = 0.0, minDistance = -1.0;
    // Margin (in pixels) around the viewport to preload
    const int pixelsToExpand = 512;

    // iterate over all items
    d->visibleItems.clear();
    QList<Okular::PixmapRequest *> requestedPixmaps;
    QList<Okular::VisiblePageRect *> visibleRects;
    for (PageViewItem *i : std::as_const(d->items)) {
        const QSet<FormWidgetIface *> formWidgetsList = i->formWidgets();
        for (FormWidgetIface *fwi : formWidgetsList) {
            Okular::NormalizedRect r = fwi->rect();
            fwi->moveTo(qRound(i->uncroppedGeometry().left() + i->uncroppedWidth() * r.left) + 1 - viewportRect.left(), qRound(i->uncroppedGeometry().top() + i->uncroppedHeight() * r.top) + 1 - viewportRect.top());
        }
        const QHash<const Okular::Movie *, VideoWidget *> videoWidgets = i->videoWidgets();
        for (VideoWidget *vw : videoWidgets) {
            const Okular::NormalizedRect r = vw->normGeometry();
            vw->move(qRound(i->uncroppedGeometry().left() + i->uncroppedWidth() * r.left) + 1 - viewportRect.left(), qRound(i->uncroppedGeometry().top() + i->uncroppedHeight() * r.top) + 1 - viewportRect.top());

            if (vw->isPlaying() && viewportRectAtZeroZero.intersected(vw->geometry()).isEmpty()) {
                vw->stop();
                vw->pageLeft();
            }
        }

        if (!i->isVisible()) {
            continue;
        }
#ifdef PAGEVIEW_DEBUG
        qWarning() << "checking page" << i->pageNumber();
        qWarning().nospace() << "viewportRect is " << viewportRect << ", page item is " << i->croppedGeometry() << " intersect : " << viewportRect.intersects(i->croppedGeometry());
#endif
        // if the item doesn't intersect the viewport, skip it
        QRectF intersectionRect = viewportRect.intersected(i->croppedGeometry());
        if (intersectionRect.isEmpty()) {
            continue;
        }

        // add the item to the 'visible list'
        d->visibleItems.push_back(i);

        intersectionRect.translate(-i->uncroppedGeometry().topLeft());
        const Okular::NormalizedRect normRect(intersectionRect.left() / i->uncroppedWidth(), intersectionRect.top() / i->uncroppedHeight(), intersectionRect.right() / i->uncroppedWidth(), intersectionRect.bottom() / i->uncroppedHeight());

        Okular::VisiblePageRect *vItem = new Okular::VisiblePageRect(i->pageNumber(), normRect);
        visibleRects.push_back(vItem);
#ifdef PAGEVIEW_DEBUG
        qWarning() << "checking for pixmap for page" << i->pageNumber() << "=" << i->page()->hasPixmap(this, i->uncroppedWidth(), i->uncroppedHeight());
        qWarning() << "checking for text for page" << i->pageNumber() << "=" << i->page()->hasTextPage();
#endif

        Okular::NormalizedRect expandedVisibleRect = vItem->rect;
        if (i->page()->hasTilesManager(this) && Okular::Settings::memoryLevel() != Okular::Settings::EnumMemoryLevel::Low) {
            double rectMargin = pixelsToExpand / (double)i->uncroppedHeight();
            expandedVisibleRect.left = qMax(0.0, vItem->rect.left - rectMargin);
            expandedVisibleRect.top = qMax(0.0, vItem->rect.top - rectMargin);
            expandedVisibleRect.right = qMin(1.0, vItem->rect.right + rectMargin);
            expandedVisibleRect.bottom = qMin(1.0, vItem->rect.bottom + rectMargin);
        }

        // if the item has not the right pixmap, add a request for it
        if (!i->page()->hasPixmap(this, i->uncroppedWidth(), i->uncroppedHeight(), expandedVisibleRect)) {
#ifdef PAGEVIEW_DEBUG
            qWarning() << "rerequesting visible pixmaps for page" << i->pageNumber() << "!";
#endif
            Okular::PixmapRequest *p = new Okular::PixmapRequest(this, i->pageNumber(), i->uncroppedWidth(), i->uncroppedHeight(), devicePixelRatioF(), PAGEVIEW_PRIO, Okular::PixmapRequest::Asynchronous);
            requestedPixmaps.push_back(p);

            if (i->page()->hasTilesManager(this)) {
                p->setNormalizedRect(expandedVisibleRect);
                p->setTile(true);
            } else {
                p->setNormalizedRect(vItem->rect);
            }
        }

        // look for the item closest to viewport center and the relative
        // position between the item and the viewport center
        if (isEvent) {
            const QRect &geometry = i->croppedGeometry();
            // compute distance between item center and viewport center (slightly moved left)
            const double distance = hypot((geometry.left() + geometry.right()) / 2.0 - (viewportCenterX - 4), (geometry.top() + geometry.bottom()) / 2.0 - viewportCenterY);
            if (distance >= minDistance && nearPageNumber != -1) {
                continue;
            }
            nearPageNumber = i->pageNumber();
            minDistance = distance;
            if (geometry.height() > 0 && geometry.width() > 0) {
                // Compute normalized coordinates w.r.t. cropped page
                focusedX = (viewportCenterX - (double)geometry.left()) / (double)geometry.width();
                focusedY = (viewportCenterY - (double)geometry.top()) / (double)geometry.height();
                // Convert to normalized coordinates w.r.t. full page (no-op if not cropped)
                focusedX = i->crop().left + focusedX * i->crop().width();
                focusedY = i->crop().top + focusedY * i->crop().height();
            }
        }
    }

    // if preloading is enabled, add the pages before and after in preloading
    if (!d->visibleItems.isEmpty() && Okular::SettingsCore::memoryLevel() != Okular::SettingsCore::EnumMemoryLevel::Low) {
        // as the requests are done in the order as they appear in the list,
        // request first the next page and then the previous

        int pagesToPreload = viewColumns();

        // if the greedy option is set, preload all pages
        if (Okular::SettingsCore::memoryLevel() == Okular::SettingsCore::EnumMemoryLevel::Greedy) {
            pagesToPreload = d->items.count();
        }

        const QRectF adjustedViewportRect = viewportRect.adjusted(0, -pixelsToExpand, 0, pixelsToExpand);
        const QRect expandedViewportRect(adjustedViewportRect.x(), adjustedViewportRect.y(), adjustedViewportRect.width(), adjustedViewportRect.height());

        for (int j = 1; j <= pagesToPreload; j++) {
            // add the page after the 'visible series' in preload
            const int tailRequest = d->visibleItems.last()->pageNumber() + j;
            if (tailRequest < (int)d->items.count()) {
                slotRequestPreloadPixmap(this, d->items[tailRequest], expandedViewportRect, &requestedPixmaps);
            }

            // add the page before the 'visible series' in preload
            const int headRequest = d->visibleItems.first()->pageNumber() - j;
            if (headRequest >= 0) {
                slotRequestPreloadPixmap(this, d->items[headRequest], expandedViewportRect, &requestedPixmaps);
            }

            // stop if we've already reached both ends of the document
            if (headRequest < 0 && tailRequest >= (int)d->items.count()) {
                break;
            }
        }
    }

    // send requests to the document
    if (!requestedPixmaps.isEmpty()) {
        d->document->requestPixmaps(requestedPixmaps);
    }
    // if this functions was invoked by viewport events, send update to document
    if (isEvent && nearPageNumber != -1) {
        // determine the document viewport
        Okular::DocumentViewport newViewport(nearPageNumber);
        newViewport.rePos.enabled = true;
        newViewport.rePos.normalizedX = focusedX;
        newViewport.rePos.normalizedY = focusedY;
        // set the viewport to other observers
        // do not update history if the viewport is autoscrolling
        d->document->setViewport(newViewport, this, false, d->scroller->state() != QScroller::Scrolling);
    }
    d->document->setVisiblePageRects(visibleRects, this);
}

void PageView::slotAutoScroll()
{
    // the first time create the timer
    if (!d->autoScrollTimer) {
        d->autoScrollTimer = new QTimer(this);
        d->autoScrollTimer->setSingleShot(true);
        connect(d->autoScrollTimer, &QTimer::timeout, this, &PageView::slotAutoScroll);
    }

    // if scrollIncrement is zero, stop the timer
    if (!d->scrollIncrement) {
        d->autoScrollTimer->stop();
        return;
    }

    // compute delay between timer ticks and scroll amount per tick
    int index = abs(d->scrollIncrement) - 1; // 0..9
    const int scrollDelay[10] = {200, 100, 50, 30, 20, 30, 25, 20, 30, 20};
    const int scrollOffset[10] = {1, 1, 1, 1, 1, 2, 2, 2, 4, 4};
    d->autoScrollTimer->start(scrollDelay[index]);
    int delta = d->scrollIncrement > 0 ? scrollOffset[index] : -scrollOffset[index];
    d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(0, delta), scrollDelay[index]);
}

void PageView::slotDragScroll()
{
    scrollTo(horizontalScrollBar()->value() + d->dragScrollVector.x(), verticalScrollBar()->value() + d->dragScrollVector.y());
    QPoint p = contentAreaPosition() + viewport()->mapFromGlobal(QCursor::pos());
    updateSelection(p);
}

void PageView::slotShowWelcome()
{
    // show initial welcome text
    d->messageWindow->display(i18n("Welcome"), QString(), PageViewMessage::Info, 2000);
}

void PageView::slotShowMoveCursor()
{
    setCursor(GuiUtils::moveCursorShape(true));
}

void PageView::slotHandleWebShortcutAction()
{
    QAction *action = qobject_cast<QAction *>(sender());

    if (action) {
        KUriFilterData filterData(action->data().toString());

        if (KUriFilter::self()->filterSearchUri(filterData, KUriFilter::WebShortcutFilter)) {
            QDesktopServices::openUrl(filterData.uri());
        }
    }
}

void PageView::slotConfigureWebShortcuts()
{
    auto *job = new KIO::CommandLauncherJob(QStringLiteral("kcmshell6"), QStringList() << QStringLiteral("webshortcuts"));
    job->start();
}

void PageView::slotZoom()
{
    if (!d->aZoom->selectableActionGroup()->isEnabled()) {
        return;
    }

    setFocus();
    updateZoom(ZoomFixed);
}

void PageView::slotZoomIn()
{
    updateZoom(ZoomIn);
}

void PageView::slotZoomOut()
{
    updateZoom(ZoomOut);
}

void PageView::slotZoomActual()
{
    updateZoom(ZoomActual);
}

void PageView::slotFitToWidthToggled(bool on)
{
    if (on) {
        updateZoom(ZoomFitWidth);
    }
}

void PageView::slotFitToPageToggled(bool on)
{
    if (on) {
        updateZoom(ZoomFitPage);
    }
}

void PageView::slotAutoFitToggled(bool on)
{
    if (on) {
        updateZoom(ZoomFitAuto);
    }
}

void PageView::slotViewMode(QAction *action)
{
    const int nr = action->data().toInt();
    if ((int)Okular::Settings::viewMode() != nr) {
        Okular::Settings::setViewMode(nr);
        Okular::Settings::self()->save();
        if (d->document->pages() > 0) {
            slotRelayoutPages();
        }
    }
}

void PageView::slotContinuousToggled()
{
    if (d->document->pages() > 0) {
        slotRelayoutPages();
    }
}

void PageView::slotReadingDirectionToggled(bool leftToRight)
{
    Okular::Settings::setRtlReadingDirection(leftToRight);
    Okular::Settings::self()->save();
}

void PageView::slotUpdateReadingDirectionAction()
{
    d->aReadingDirection->setChecked(Okular::Settings::rtlReadingDirection());
}

void PageView::slotSetMouseNormal()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::Browse;
    Okular::Settings::setMouseMode(d->mouseMode);
    // hide the messageWindow
    d->messageWindow->hide();
    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotSetMouseZoom()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::Zoom;
    Okular::Settings::setMouseMode(d->mouseMode);
    // change the text in messageWindow (and show it if hidden)
    d->messageWindow->display(i18n("Select zooming area. Right-click to zoom out."), QString(), PageViewMessage::Info, -1);
    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotSetMouseMagnifier()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::Magnifier;
    Okular::Settings::setMouseMode(d->mouseMode);
    d->messageWindow->display(i18n("Click to see the magnified view."), QString());

    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotSetMouseSelect()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::RectSelect;
    Okular::Settings::setMouseMode(d->mouseMode);
    // The toolbar's icons and tooltips say what each tool is for: no banner (the user's choice, 10 November).
    d->messageWindow->hide();
    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotSetMouseTextSelect()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::TextSelect;
    Okular::Settings::setMouseMode(d->mouseMode);
    d->messageWindow->hide();
    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotSetMouseDraw()
{
    // On an HTML page the pencil draws on a snapshot of the page; the PDF keeps the tool it had.
    if (d->shownInstead == ShownInstead::HtmlPage && d->htmlPagePencil) {
        if (QAction *inUse = mouseModeAction()) inUse->setChecked(true);
        else d->aMouseDraw->setChecked(false);
        d->htmlPagePencil();
        return;
    }
    // The button again: back to the tool it replaced.
    if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) {
        stopDrawing();
        return;
    }
    // From the Text tool, the tool both replaced.
    if (d->mouseMode != Okular::Settings::EnumMouseMode::TextBox) d->modeBeforeDrawing = d->mouseMode;
    selectionClear();
    textSelectionClear();
    // Not saved: the next start opens with a selection tool, so a stray click never draws.
    d->mouseMode = Okular::Settings::EnumMouseMode::Draw;
    d->messageWindow->hide();
    updateCursor();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::choosePencil(const QString &colour, bool eraser)
{
    if (!eraser) Okular::Settings::setPencilColor(colour);
    Okular::Settings::setPencilEraser(eraser);
    // Every tab follows (configChanged).
    Okular::Settings::self()->save();
    slotFollowPencil();
    if (d->mouseMode != Okular::Settings::EnumMouseMode::Draw) d->aMouseDraw->trigger();
}

void PageView::slotFollowPencil()
{
    const bool eraser = Okular::Settings::pencilEraser();
    const QString colour = pencilColour();
    for (QAction *action : std::as_const(d->pencilColours)) action->setChecked(!eraser && action->data().toString() == colour);
    d->aPencilEraser->setChecked(eraser);
    // The button shows which it is; its data tells the shell, which gives it its own icon.
    const QString tool = eraser ? QStringLiteral("eraser") : QStringLiteral("pencil");
    if (d->aMouseDraw->data().toString() != tool) {
        d->aMouseDraw->setData(tool);
        d->aMouseDraw->setIcon(QIcon::fromTheme(eraser ? QStringLiteral("draw-eraser") : QStringLiteral("draw-freehand")));
        d->aMouseDraw->setToolTip(eraser ? i18nc("@info:tooltip the pencil's button while it erases", "Eraser") : i18n("Pencil"));
        if (d->mouseMode == Okular::Settings::EnumMouseMode::Draw) updateCursor();
    }
    // On an HTML page or edition the pencil's tip says what it does there.
    if (d->shownInstead != ShownInstead::Nothing) applyShownInsteadToTools();
}

void PageView::slotSetMouseTableSelect()
{
    selectionClear();
    textSelectionClear();
    d->mouseMode = Okular::Settings::EnumMouseMode::TableSelect;
    Okular::Settings::setMouseMode(d->mouseMode);
    // change the text in messageWindow (and show it if hidden)
    d->messageWindow->display(i18n("Draw a rectangle around the table, then click near edges to divide up; press Esc to clear."), QString(), PageViewMessage::Info, -1);
    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::slotFollowSelectionTool()
{
    // The selection tool is one choice for the whole app: one picked in a tab applies to every
    // tab. Other modes (Browse, Zoom, trimming) stay the tab's own.
    const auto isSelectionTool = [](int mode) { return mode == Okular::Settings::EnumMouseMode::TextSelect || mode == Okular::Settings::EnumMouseMode::RectSelect; };
    const int mode = Okular::Settings::mouseMode();
    if (mode == d->mouseMode || !isSelectionTool(mode) || !isSelectionTool(d->mouseMode)) {
        return;
    }
    selectionClear();
    textSelectionClear();
    d->mouseMode = mode;
    (mode == Okular::Settings::EnumMouseMode::RectSelect ? d->aMouseSelect : d->aMouseTextSelect)->setChecked(true);
    updateCursor();
    if (d->annotator) {
        d->annotator->detachAnnotation();
    }
}

void PageView::showNoSigningCertificatesDialog(bool nonDateValidCerts)
{
    Okular::KleopatraIntegration kleo(document());
    if (nonDateValidCerts) {
        if (kleo.kleopatraIntegrationActive()) {
            auto result = KMessageBox::questionTwoActions(this,
                                                          i18n("All your signing certificates are invalid. In case signing worked before, please check their validity dates."),
                                                          i18n("No valid certificates"),
                                                          KGuiItem(i18n("Open Certificate Manager")),
                                                          KStandardGuiItem::cancel());
            if (result == KMessageBox::PrimaryAction) {
                kleo.launchKleopatra(window()->windowHandle());
            }
        } else {
            KMessageBox::information(this, i18n("All your signing certificates are invalid. In case signing worked before, please check their validity dates."));
        }
    } else {
        if (kleo.kleopatraIntegrationActive()) {
            auto result = KMessageBox::questionTwoActions(this,
                                                          i18n("There are no available signing certificates.<br/>Launch Certificate Manager to import or manage certificates<br/>For more information, please see the section about <a "
                                                               "href=\"%1\">Adding Digital Signatures</a> in the manual.",
                                                               QStringLiteral("help:/okular/signatures.html#adding_digital_signatures")),
                                                          i18n("No certificates found"),
                                                          KGuiItem(i18n("Open Certificate Manager")),
                                                          KStandardGuiItem::cancel(),
                                                          QString {},
                                                          KMessageBox::Notify | KMessageBox::AllowLink);
            if (result == KMessageBox::PrimaryAction) {
                kleo.launchKleopatra(window()->windowHandle());
            }
        } else {
            KMessageBox::information(this,
                                     i18n("There are no available signing certificates.<br/>For more information, please see the section about <a href=\"%1\">Adding Digital Signatures</a> in the manual.",
                                          QStringLiteral("help:/okular/signatures.html#adding_digital_signatures")),
                                     i18n("No certificates found"),
                                     QString(),
                                     KMessageBox::Notify | KMessageBox::AllowLink);
        }
    }
}

Okular::Document *PageView::document() const
{
    return d->document;
}

#if HAVE_NEW_SIGNATURE_API
void PageView::startSigning(Okular::SignatureAnnotation *signatureAnnotation)
{
    d->signatureAnnotation = signatureAnnotation;
    Q_EMIT signingStarted();
}
#endif

void PageView::slotSignature()
{
    if (!d->document->isHistoryClean()) {
        KMessageBox::information(this, i18n("You have unsaved changes. Please save the document before signing it."));
        return;
    }

    const Okular::CertificateStore *certStore = d->document->certificateStore();
    bool userCancelled, nonDateValidCerts;
    const QList<Okular::CertificateInfo> &certs = certStore->signingCertificatesForNow(&userCancelled, &nonDateValidCerts);
    if (userCancelled) {
        return;
    }

    if (certs.isEmpty()) {
        showNoSigningCertificatesDialog(nonDateValidCerts);
        return;
    }

    auto signInfo = SignaturePartUtils::getCertificateAndPasswordForSigning(this, d->document, SignaturePartUtils::SigningInformationOption::BackgroundImage);
    if (!signInfo) {
        return;
    }

    d->signingInfo = std::move(*signInfo);

    d->messageWindow->display(i18n("Draw a rectangle to insert the signature field"), QString(), PageViewMessage::Info, -1);

    d->annotator->startSigning(&d->signingInfo);

    actionCollection()->action(QStringLiteral("add_digital_signature"))->setEnabled(false);

    // force an update of the cursor
    updateCursor();
    Okular::Settings::self()->save();
}

void PageView::slotAutoScrollUp()
{
    if (d->scrollIncrement < -9) {
        return;
    }
    d->scrollIncrement--;
    slotAutoScroll();
    setFocus();
}

void PageView::slotAutoScrollDown()
{
    if (d->scrollIncrement > 9) {
        return;
    }
    d->scrollIncrement++;
    slotAutoScroll();
    setFocus();
}

void PageView::slotScrollUp(int nSteps)
{
    if (verticalScrollBar()->value() > verticalScrollBar()->minimum()) {
        if (nSteps) {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(0, -100 * nSteps), d->currentShortScrollDuration);
        } else {
            if (d->scroller->finalPosition().y() > verticalScrollBar()->minimum()) {
                d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(0, -(1 - Okular::Settings::scrollOverlap() / 100.0) * viewport()->height()), d->currentLongScrollDuration);
            }
        }
    } else if (!getContinuousMode() && d->document->currentPage() > 0) {
        // Since we are in single page mode and at the top of the page, go to previous page.
        // setViewport() is more optimized than document->setPrevPage and then move view to bottom.
        Okular::DocumentViewport newViewport = d->document->viewport();
        newViewport.pageNumber -= viewColumns();
        if (newViewport.pageNumber < 0) {
            newViewport.pageNumber = 0;
        }
        newViewport.rePos.enabled = true;
        newViewport.rePos.normalizedY = 1.0;
        d->document->setViewport(newViewport);
    }
}

void PageView::slotScrollDown(int nSteps)
{
    if (verticalScrollBar()->value() < verticalScrollBar()->maximum()) {
        if (nSteps) {
            d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(0, 100 * nSteps), d->currentShortScrollDuration);
        } else {
            if (d->scroller->finalPosition().y() < verticalScrollBar()->maximum()) {
                d->scroller->scrollTo(d->scroller->finalPosition() + QPoint(0, (1 - Okular::Settings::scrollOverlap() / 100.0) * viewport()->height()), d->currentLongScrollDuration);
            }
        }
    } else if (!getContinuousMode() && (int)d->document->currentPage() < d->items.count() - 1) {
        // Since we are in single page mode and at the bottom of the page, go to next page.
        // setViewport() is more optimized than document->setNextPage and then move view to top
        Okular::DocumentViewport newViewport = d->document->viewport();
        newViewport.pageNumber += viewColumns();
        if (newViewport.pageNumber >= (int)d->items.count()) {
            newViewport.pageNumber = d->items.count() - 1;
        }
        newViewport.rePos.enabled = true;
        newViewport.rePos.normalizedY = 0.0;
        d->document->setViewport(newViewport);
    }
}

void PageView::slotRotateClockwise()
{
    int id = ((int)d->document->rotation() + 1) % 4;
    d->document->setRotation(id);
}

void PageView::slotRotateCounterClockwise()
{
    int id = ((int)d->document->rotation() + 3) % 4;
    d->document->setRotation(id);
}

void PageView::slotRotateOriginal()
{
    d->document->setRotation(0);
}

// Enforce mutual-exclusion between trim modes
// Each mode is uniquely identified by a single value
// From Okular::Settings::EnumTrimMode
void PageView::updateTrimMode(int except_id)
{
    const QList<QAction *> trimModeActions = d->aTrimMode->menu()->actions();
    for (QAction *trimModeAction : trimModeActions) {
        if (trimModeAction->data().toInt() != except_id) {
            trimModeAction->setChecked(false);
        }
    }
}

bool PageView::mouseReleaseOverLink(const Okular::ObjectRect *rect) const
{
    if (rect) {
        // handle click over a link
        const Okular::Action *action = static_cast<const Okular::Action *>(rect->object());
        if (d->linkOverrideClick) {
            // The page that holds the link tells a combined Project PDF which document it is in.
            int page = -1;
            for (const PageViewItem *item : std::as_const(d->visibleItems)) {
                if (item->page()->objectRects().contains(const_cast<Okular::ObjectRect *>(rect))) {
                    page = item->pageNumber();
                    break;
                }
            }
            if (d->linkOverrideClick(action, page)) {
                return true;
            }
        }
        d->document->processAction(action);
        return true;
    }
    return false;
}

void PageView::slotTrimMarginsToggled(bool on)
{
    if (on) { // Turn off any other Trim modes
        updateTrimMode(d->aTrimMargins->data().toInt());
    }

    if (Okular::Settings::trimMargins() != on) {
        Okular::Settings::setTrimMargins(on);
        Okular::Settings::self()->save();
        if (d->document->pages() > 0) {
            slotRelayoutPages();
            slotRequestVisiblePixmaps(); // TODO: slotRelayoutPages() may have done this already!
        }
    }
}

void PageView::slotTrimToSelectionToggled(bool on)
{
    if (on) { // Turn off any other Trim modes
        updateTrimMode(d->aTrimToSelection->data().toInt());

        if (!d->aPrevAction) {
            d->aPrevAction = d->aMouseTextSelect->isChecked() ? d->aMouseTextSelect : d->aMouseNormal;
        }
        // Change the mouse mode
        d->mouseMode = Okular::Settings::EnumMouseMode::TrimSelect;
        d->aMouseTextSelect->setChecked(false);
        d->aMouseNormal->setChecked(false);

        // change the text in messageWindow (and show it if hidden)
        d->messageWindow->display(i18n("Draw a rectangle around the page area you wish to keep visible"), QString(), PageViewMessage::Info, -1);
        // force an update of the cursor
        updateCursor();
    } else {
        // toggled off while making selection
        if (Okular::Settings::EnumMouseMode::TrimSelect == d->mouseMode) {
            // clear widget selection and invalidate rect
            selectionClear();

            // When Trim selection bbox interaction is over, we should switch to another mousemode.
            if (d->aPrevAction) {
                d->aPrevAction->trigger();
                d->aPrevAction = nullptr;
            } else {
                d->aMouseNormal->trigger();
            }
        }

        d->trimBoundingBox = Okular::NormalizedRect(); // invalidate box
        if (d->document->pages() > 0) {
            slotRelayoutPages();
            slotRequestVisiblePixmaps(); // TODO: slotRelayoutPages() may have done this already!
        }
    }
}

void PageView::slotToggleForms()
{
    toggleFormWidgets(!d->m_formsVisible);
}

void PageView::slotFormChanged(int pageNumber)
{
    if (!d->refreshTimer) {
        d->refreshTimer = new QTimer(this);
        d->refreshTimer->setSingleShot(true);
        connect(d->refreshTimer, &QTimer::timeout, this, &PageView::slotRefreshPage);
    }
    d->refreshPages << pageNumber;
    int delay = 0;
    if (d->m_formsVisible) {
        delay = 1000;
    }
    d->refreshTimer->start(delay);
}

void PageView::slotRefreshPage()
{
    for (int req : std::as_const(d->refreshPages)) {
        QTimer::singleShot(0, this, [this, req] { d->document->refreshPixmaps(req); });
    }
    d->refreshPages.clear();
}

#if HAVE_SPEECH
void PageView::slotSpeakDocument()
{
    QString text;
    for (const PageViewItem *item : std::as_const(d->items)) {
        std::unique_ptr<Okular::RegularAreaRect> area = textSelectionForItem(item);
        text.append(item->page()->text(area.get()));
        text.append(QLatin1Char('\n'));
    }

    d->tts()->say(text);
}

void PageView::slotSpeakFromCurrentPage()
{
    const int currentPage = d->document->viewport().pageNumber;

    QString text;
    QList<PageViewItem *>::const_iterator dIt = d->items.constBegin(), dEnd = d->items.constEnd();

    for (dIt += currentPage; dIt != dEnd; ++dIt) {
        std::unique_ptr<Okular::RegularAreaRect> area = textSelectionForItem(*dIt);
        text.append((*dIt)->page()->text(area.get()));
        text.append(QLatin1Char('\n'));
    }

    d->tts()->say(text);
}

void PageView::slotSpeakCurrentPage()
{
    const int currentPage = d->document->viewport().pageNumber;

    const PageViewItem *item = d->items.at(currentPage);
    std::unique_ptr<Okular::RegularAreaRect> area = textSelectionForItem(item);
    const QString text = item->page()->text(area.get());

    d->tts()->say(text);
}

void PageView::slotStopSpeaks()
{
    if (!d->m_tts) {
        return;
    }

    d->m_tts->stopAllSpeechs();
}

void PageView::slotPauseResumeSpeech()
{
    if (!d->m_tts) {
        return;
    }

    d->m_tts->pauseResumeSpeech();
}

#endif

void PageView::slotAction(Okular::Action *action)
{
    d->document->processAction(action);
}

void PageView::slotMouseAction(Okular::Action *action, Okular::FormField *form, Okular::Document::MouseEventType fieldMouseEventType)
{
    if (form && action->actionType() == Okular::Action::Script) {
        d->document->processFormMouseScriptAction(action, form, fieldMouseEventType);
    } else {
        d->document->processAction(action);
    }
}

void PageView::externalKeyPressEvent(QKeyEvent *e)
{
    keyPressEvent(e);
}

#if HAVE_NEW_SIGNATURE_API
PageView::FinishSigningResult PageView::finishSigning()
{
    const QString newFilePath = SignaturePartUtils::getFileNameForNewSignedFile(this, d->document);

    if (newFilePath.isEmpty()) {
        return Cancelled;
    }

    Okular::NewSignatureData data;
    data.setCertNickname(d->signingInfo.certificate->nickName());
    data.setCertSubjectCommonName(d->signingInfo.certificate->subjectInfo(Okular::CertificateInfo::CommonNameOrEmail, Okular::CertificateInfo::EmptyString::TranslatedNotAvailable));
    data.setPassword(d->signingInfo.certificatePassword);
    data.setDocumentPassword(d->signingInfo.documentPassword);
    data.setReason(d->signingInfo.reason);
    data.setLocation(d->signingInfo.location);
    data.setLeftFontSize(d->signatureAnnotation->leftFontSize());
    data.setFontSize(d->signatureAnnotation->fontSize());

    std::pair<Okular::SigningResult, QString> result = d->signatureAnnotation->sign(data, newFilePath);
    switch (result.first) {
    case Okular::SigningSuccess: {
        Q_EMIT requestOpenNewlySignedFile(newFilePath, d->signatureAnnotation->page() + 1);
        return Success;
    }
    case Okular::FieldAlreadySigned: // We should not end up here
    case Okular::KeyMissing:         // unless the user modified the key store after opening the dialog, this should not happen
    case Okular::InternalSigningError:
        KMessageBox::detailedError(this, errorString(result.first, static_cast<int>(result.first)), result.second);
        return Failed;
    case Okular::GenericSigningError:
        KMessageBox::detailedError(this, errorString(result.first, newFilePath), result.second);
        return Failed;
    case Okular::UserCancelled:
        return Cancelled;
    case Okular::BadPassphrase:
        KMessageBox::error(this, errorString(result.first, {}));
        return Cancelled;
    case Okular::SignatureWriteFailed:
        KMessageBox::detailedError(this, errorString(result.first, newFilePath), result.second);
        return Failed;
    }
    // We should not end here
    Q_ASSERT(false);
    return Failed;
}
#endif

void PageView::slotProcessMovieAction(const Okular::MovieAction *action)
{
    const Okular::MovieAnnotation *movieAnnotation = action->annotation();
    if (!movieAnnotation) {
        return;
    }

    const Okular::Movie *movie = movieAnnotation->movie();
    if (!movie) {
        return;
    }

    const int currentPage = d->document->viewport().pageNumber;

    PageViewItem *item = d->items.at(currentPage);
    if (!item) {
        return;
    }

    VideoWidget *vw = item->videoWidgets().value(movie);
    if (!vw) {
        return;
    }

    vw->show();

    switch (action->operation()) {
    case Okular::MovieAction::Play:
        vw->stop();
        vw->play();
        break;
    case Okular::MovieAction::Stop:
        vw->stop();
        break;
    case Okular::MovieAction::Pause:
        vw->pause();
        break;
    case Okular::MovieAction::Resume:
        vw->play();
        break;
    };
}

void PageView::slotProcessRenditionAction(const Okular::RenditionAction *action)
{
    const Okular::Movie *movie = action->movie();
    if (!movie) {
        return;
    }

    const int currentPage = d->document->viewport().pageNumber;

    PageViewItem *item = d->items.at(currentPage);
    if (!item) {
        return;
    }

    VideoWidget *vw = item->videoWidgets().value(movie);
    if (!vw) {
        return;
    }

    if (action->operation() == Okular::RenditionAction::None) {
        return;
    }

    vw->show();

    switch (action->operation()) {
    case Okular::RenditionAction::Play:
        vw->stop();
        vw->play();
        break;
    case Okular::RenditionAction::Stop:
        vw->stop();
        break;
    case Okular::RenditionAction::Pause:
        vw->pause();
        break;
    case Okular::RenditionAction::Resume:
        vw->play();
        break;
    default:
        return;
    };
}

void PageView::slotFitWindowToPage()
{
    const PageViewItem *currentPageItem = nullptr;
    QSize viewportSize = viewport()->size();
    for (const PageViewItem *pageItem : std::as_const(d->items)) {
        if (pageItem->isVisible()) {
            currentPageItem = pageItem;
            break;
        }
    }

    if (!currentPageItem) {
        return;
    }

    const QSize pageSize = QSize(currentPageItem->uncroppedWidth() + kcolWidthMargin, currentPageItem->uncroppedHeight() + krowHeightMargin);
    if (verticalScrollBar()->isVisible()) {
        viewportSize.setWidth(viewportSize.width() + verticalScrollBar()->width());
    }
    if (horizontalScrollBar()->isVisible()) {
        viewportSize.setHeight(viewportSize.height() + horizontalScrollBar()->height());
    }
    Q_EMIT fitWindowToPage(viewportSize, pageSize);
}

void PageView::slotSelectPage()
{
    textSelectionClear();
    const int currentPage = d->document->viewport().pageNumber;
    const PageViewItem *item = d->items.at(currentPage);

    if (item) {
        std::unique_ptr<Okular::RegularAreaRect> area = textSelectionForItem(item);
        d->pagesWithTextSelection.insert(currentPage);
        d->document->setPageTextSelection(currentPage, std::move(area), palette().color(QPalette::Active, QPalette::Highlight));
    }
}

void PageView::highlightSignatureFormWidget(const Okular::FormFieldSignature *form)
{
    QList<PageViewItem *>::const_iterator dIt = d->items.constBegin(), dEnd = d->items.constEnd();
    for (; dIt != dEnd; ++dIt) {
        const QSet<FormWidgetIface *> fwi = (*dIt)->formWidgets();
        for (FormWidgetIface *fw : fwi) {
            if (fw->formField() == form) {
                SignatureEdit *widget = static_cast<SignatureEdit *>(fw);
                widget->setDummyMode(true);
                QTimer::singleShot(250, this, [=] { widget->setDummyMode(false); });
                return;
            }
        }
    }
}

// END private SLOTS

/* kate: replace-tabs on; indent-width 4; */
