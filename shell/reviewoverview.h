/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_REVIEWOVERVIEW_H
#define THOUGHTS_REVIEWOVERVIEW_H

#include "reviewfeedbackpanel.h"
#include "reviewstore.h"
#include "reviewinterval.h"

#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QVariantMap>

#include <functional>
#include <optional>

class QAction;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class MainShellTest;
namespace Thoughts
{
class ReviewFeedbackPanel;
}

namespace Thoughts::PdfVersions
{
class Fetcher;
struct Result;
}
class CommentsOverviewTest;

namespace Thoughts
{

// Why the version of a History's block could not be got, as it says it; empty when it was.
QString pdfVersionError(const PdfVersions::Result &result);

// The History of the document @p pdfStem for its sidebar, one block per PDF version
// (ReviewFeedbackPanel): the Library's current version (@p currentPdfPath) first, with every card of
// the document and its answer there, and also the feedback given on earlier versions whose passage
// is still found in that answer (feedback "carried": true); then earlier ones newest first and last
// the entries whose version was not recorded. In each block a card's review answers and answer
// feedback follow one another, the cards in page order.
QList<ReviewFeedbackPanel::Version> documentHistoryVersions(const ReviewStore &store, const QString &pdfStem, const QString &currentPdfPath);
// Adds to @p versions what a document's History shows besides its cards: the current version
// (@p currentSha256, @p currentPdfPath) even with nothing on it, every earlier version of @p kept
// (SHA-256 and when it was published) still on this computer, and the comments on passages of
// the document (its document_feedback @p records, deleted ones left out) under the version each
// was first made on. Versions keep the order of documentHistoryVersions.
void addDocumentVersions(QList<ReviewFeedbackPanel::Version> &versions, const QList<QJsonObject> &records, const QList<QPair<QString, QDateTime>> &kept,
                         const QString &currentSha256, const QString &currentPdfPath);
// The versions of the Library PDF @p filename that this computer still has in a Library generation
// of @p docsRoot, with when each generation was made.
QList<QPair<QString, QDateTime>> keptDocumentVersions(const QString &docsRoot, const QString &filename);
// The document (pdf_stem) whose cards were reviewed or got feedback on the PDF version @p sha256,
// for the History of that version opened on its own; empty when no record names it.
QString versionPdfStem(const ReviewStore &store, const QString &sha256);
// What a reader's Cards list shows of @p card's schedule ("sidebar_schedule" of the records
// Shell::refreshSidebarReviewCards passes; AnnotationModel::reviewCardSchedule words it): its state
// ("new", "learning", "review", "never", "retired", "unpublished" or "unavailable": not in review
// now), from when it is due (due_from, only when @p active: ReviewStore::activeOf takes it), its
// due date, skip, interval, counts, last review and the tags of its last answer, labelled as the
// Review tab labels them. @p unpublished: its document is unpublished (read for inactive cards).
QJsonObject sidebarReviewSchedule(const ReviewCard &card, bool active, bool unpublished = false);

// The body of a request to the agent recall prediction service (review-phases spec, section 4)
// for @p card, whose answer reads @p answer (cut to 3000 characters) in the document @p document:
// the steps of @p recall, the card's answers in effect (the last 30, oldest first) and the newest
// answers of the other cards of @p documentCards in the 14 days before @p now (at most 20).
QJsonObject recallPredictionRequest(const ReviewCard &card, const QList<ReviewCard> &documentCards, const QString &answer, const QString &document,
                                    const ReviewTags::TagGroup &recall, const QDateTime &now);

class ReviewCardsWidget : public QWidget
{
    Q_OBJECT

    friend class ::MainShellTest;
    friend class ::CommentsOverviewTest;

public:
    using CardPresenter = std::function<bool(const ReviewCard &, bool, QString *)>;
    using CardReviewer = std::function<void(const QString &)>;

    explicit ReviewCardsWidget(const QString &docsRoot,
                               CardPresenter cardPresenter,
                               CardReviewer cardReviewer,
                               QWidget *parent = nullptr,
                               ReviewStore::ProjectionProvider projectionProvider = {});
    ~ReviewCardsWidget() override;

    void setSyncInProgress(bool running);
    void reload();
    void ensureLoaded();
    // Removes markers of deleted cards left pending, once that is safe; never during Sync.
    void retryPendingMarkerRemovals();
    int issueCount() const;
    int activeCardCount() const;
    int dueCardCount() const;
    bool libraryCountsAvailable() const { return m_libraryCountsAvailable; }
    const QHash<QString, QSet<QString>> &libraryCardIds() const { return m_libraryCardIds; }
    // Ids of the cards due now, as the review queue counts them (dueCardCount()).
    const QSet<QString> &libraryDueCardIds() const { return m_libraryDueCardIds; }
    const QList<ReviewBrowserCard> &libraryCards() const { return m_browseCards; }
    void setStateFilter(const QString &stateFilter);
    void selectCard(const QString &cardId);
    // The active project cards of the Project @p projectId (ReviewStore::activeProjectCards): as the
    // cards were last read (-1 before they are), or read now.
    int activeProjectCardCount(const QString &projectId) const;
    QList<ReviewCard> activeProjectCards(const QString &projectId);
    // Retire project cards… of a Project (ReviewStore::retireProjectCards), then reads the cards again.
    bool retireProjectCards(const QString &projectId, QString *error = nullptr);

Q_SIGNALS:
    void countsChanged(int activeCards, int dueCards);
    void openSourceRequested(const QString &sourcePath, int line);
    // History: open the PDF version @p pdfPath at @p page, marking @p regions ({page, rect}) when
    // given. @p current: the card's Library PDF; else an earlier version, read-only, as @p title.
    void pdfVersionOpenRequested(const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current);

private Q_SLOTS:
    void refreshBrowseTable();
    void refreshBrowseTypeCounts(const QString &selectedDocument);
    void sortBrowseTable();
    void browseSelectionChanged();
    void browseQuestionChanged();
    void openBrowseCard();
    void reviewBrowseCard();
    void updateBrowseHistory();
    void deleteOrRestoreBrowseCard();
    // "Retire card", or "Restore to review" on a retired card: the selected card (ReviewRetire).
    void retireOrRestoreBrowseCard();
    // Make project card, Make knowledge card or Follow the Project (@p kind: knowledge, project or
    // follow_project) for the selected card (review-card-kinds.md §5).
    void setBrowseCardKind(const QString &kind);
    // "Merge cards…": the selected cards of one document become the one the user keeps.
    void mergeBrowseCards();
    // "Compact tags": merges several tags into one, permanently and on every device.
    void compactTags();
    void dropDeletedBrowseCards(const QStringList &cardIds);
    void repairBrowseSourceLink();
    // Opens the area editor for the selected card; @p again: the "Place This Card Again" action.
    void placeBrowseCard(bool again);

private:
    void sortVisibleBrowseCards();
    void startReload();
    void applyCards(QList<ReviewBrowserCard> cards, const QStringList &errors);
    ReviewStore::ProjectionProvider snapshotProvider() const;
    void buildUi();
    void configureBrowseHeader();
    QStringList browseNames(const ReviewCard &card) const;
    QJsonArray m_browseProjects;
    void saveBrowseHeaderState();
    void showBrowseCard(const ReviewBrowserCard *item);
    // The History of @p item: review events and answer feedback in time order, with a link to the
    // PDF version each was made on. Whether feedback is still in the answer is read only while
    // the History is shown (it reads the PDF's text).
    void refreshBrowseHistory(const ReviewBrowserCard *item);
    void openHistoryVersion(int index);
    // The answer still in effect listed at @p viewportPos of the History, if any.
    QString historyAnswerAt(const QPoint &viewportPos) const;
    // The History's right-click menu: its usual actions and, on an answer still in effect,
    // "Take back this answer…".
    void showHistoryMenu(const QPoint &viewportPos);
    // Takes back the answer @p eventId of the History's card once confirmed: the retraction Undo
    // writes (ReviewStore::retractRating), so the card's schedule and tags come from its other
    // answers and every device agrees after Sync.
    void takeBackHistoryAnswer(const QString &eventId);
    const ReviewBrowserCard *selectedBrowseCard() const;
    bool saveBrowseQuestion(bool reportFailure = true);
    void updateCachedBrowseQuestion(const QString &cardId, const QString &question);
    void updateBrowseExportAction();
    QStringList browseSearchTags() const;
    // Opens the export dialog; @p preferSelection starts it on the selected cards even when only one is selected.
    void exportBrowseCards(bool preferSelection = false);
    void populateBrowseContextMenu(QMenu *menu);
    // @p format is "csv", "json" or "md"; @p fields are keys in export order.
    QByteArray cardExportData(const QList<ReviewBrowserCard> &cards, const QStringList &fields, const QString &format) const;

    bool m_syncInProgress = false;
    bool m_markerRetryRunning = false;
    QElapsedTimer m_markerRetryElapsed;
    QString m_docsRoot;
    ReviewStore::ProjectionProvider m_projectionProvider;
    QTimer *m_reloadTimer = nullptr;
    bool m_loading = false;
    bool m_mutating = false;
    bool m_reloadPending = false;
    quint64 m_revision = 0;
    QDateTime m_loadedAt;
    ReviewStore m_store;
    CardPresenter m_cardPresenter;
    CardReviewer m_cardReviewer;
    QList<ReviewBrowserCard> m_browseCards;
    QList<ReviewBrowserCard> m_visibleBrowseCards;
    bool m_libraryCountsAvailable = false;
    QHash<QString, QSet<QString>> m_libraryCardIds;
    QSet<QString> m_libraryDueCardIds;
    int m_activeCardCount = 0;
    int m_dueCardCount = 0;

    QLineEdit *m_browseSearch = nullptr;
    QComboBox *m_browseDocumentFilter = nullptr;
    QComboBox *m_browseStateFilter = nullptr;
    QComboBox *m_browseTypeFilter = nullptr; // Linked, PDF only, linking pending, or a link problem.
    QList<ReviewTags::TagGroup> m_tagGroups; // Synced tag groups: the sliders.
    QHash<QString, QString> m_tagMerges; // Synced tag merges; old names in tag: searches find merged tags.
    int m_browseSortKey = 0; // A BrowserSort: a column's, or one chosen from the header menu.
    Qt::SortOrder m_browseSortOrder = Qt::AscendingOrder;
    QLabel *m_browseCount = nullptr;
    QToolButton *m_browseActionsButton = nullptr;
    QAction *m_browseExportAction = nullptr;
    QAction *m_browseDeleteAction = nullptr;
    QAction *m_browseRetireAction = nullptr; // "Retire card", or "Restore to review" on a retired card.
    QAction *m_browseKindAction = nullptr; // "Make project card", or "Make knowledge card" on a project card.
    QAction *m_browseFollowAction = nullptr; // "Follow the Project (…)": a card with a kind of its own in a Project.
    QAction *m_browseMergeAction = nullptr; // "Merge cards…": two or more selected cards.
    QTreeWidget *m_browseTable = nullptr;
    QLabel *m_browseState = nullptr;
    QWidget *m_browseEditorPanel = nullptr;
    QPlainTextEdit *m_browseQuestion = nullptr;
    QLabel *m_browseQuestionStatus = nullptr;
    QTextBrowser *m_browseDetails = nullptr;
    QTextBrowser *m_browseHistory = nullptr;
    QWidget *m_browseHistoryPanel = nullptr;
    QPushButton *m_browseUpdateHistoryButton = nullptr;
    QPushButton *m_browseReviewButton = nullptr;
    QPushButton *m_browseEditButton = nullptr;
    QPushButton *m_browseHistoryButton = nullptr;
    QPushButton *m_browseSourceButton = nullptr;
    QPushButton *m_browseAreasButton = nullptr;
    QPushButton *m_browsePlaceAgainButton = nullptr;
    QPushButton *m_browseDeleteRestoreButton = nullptr;
    QPushButton *m_browseMergeButton = nullptr; // Beside Delete when several cards are selected.
    QTimer *m_browseQuestionSaveTimer = nullptr;
    QString m_browseQuestionCardId;
    QString m_browseQuestionSavedText;
    QString m_historyCardToReopen;
    QLabel *m_browseHistoryStatus = nullptr; // Getting a version, or why it could not be opened.
    // The History's "Open this version" links, by index (href thoughts-version:<index>).
    struct HistoryVersion {
        QString cardId;
        QString sha256;
        QString title; // "<document> — version of <time>"
        int page = 0;
        QVariantList regions; // The feedback's passage in that version; empty for review events.
        bool current = false;
    };
    QList<HistoryVersion> m_browseHistoryVersions;
    QString m_browseHistoryCardId; // The card the History shows.
    QStringList m_browseHistoryAnswers; // Per item of the History's list: the answer it can take back, or nothing.
    PdfVersions::Fetcher *m_versionFetcher = nullptr;
    quint64 m_versionRequest = 0; // A newer request (or another card) supersedes an older one.
    // feedback_ids not found in the answer of the current PDF, by card, PDF hash and records.
    QHash<QString, QSet<QString>> m_feedbackNotFound;
};

// Review › Mental Model: what the user's review cards say about them, as a Markdown document whose claims cite
// their cards (links "thoughts-card:<card id>"). It is written from the cards by an agent and kept in the
// application data folder (mental-model/mental-model.md). Refresh writes it again: it runs the bundled
// tools/mental-model/build.py, which asks the model, and shows the new document when that finishes.
class MentalModelView : public QWidget
{
    Q_OBJECT

public:
    explicit MentalModelView(QWidget *parent = nullptr);
    static QString path();
    // Reads the document again; the Review tab does this each time it shows the view.
    void reload();
    // Writes the document again from the cards (takes a minute or two), then reloads it.
    void rewrite();
    // How many cards were reviewed after a time: the page asks for a Refresh when cards were studied since it was
    // written.
    void setReviewedSince(std::function<int(const QDateTime &)> count);

Q_SIGNALS:
    // A cited card was clicked: show it in Browse cards.
    void cardRequested(const QString &cardId);
    // Refresh rewrote the Library note mental-model.md, which every agent and service reads: Sync takes it to the
    // server.
    void libraryNoteWritten();

private:
    QLabel *m_status = nullptr;
    QPushButton *m_refresh = nullptr;
    QTextBrowser *m_text = nullptr;
    QDateTime m_loaded;
    std::function<int(const QDateTime &)> m_reviewedSince;
    QPointer<QProcess> m_writer; // While Refresh writes the document.
};

class ReviewOverview : public QDialog
{
    Q_OBJECT

    friend class ::MainShellTest;

public:
    using DocumentProvider = std::function<QVariantMap()>;
    using CardPresenter = std::function<bool(const ReviewCard &, bool, QString *)>;
    using MaskClearer = std::function<void()>;
    using CardBrowserOpener = std::function<void(const QString &)>;
    using SessionHostProvider = std::function<QWidget *()>;

    explicit ReviewOverview(const QString &docsRoot,
                            DocumentProvider documentProvider,
                            CardPresenter cardPresenter,
                            MaskClearer maskClearer,
                            QWidget *parent = nullptr,
                            ReviewStore::ProjectionProvider projectionProvider = {},
                            CardBrowserOpener cardBrowserOpener = {},
                            SessionHostProvider sessionHostProvider = {},
                            bool syncInProgress = false);
    ~ReviewOverview() override;

    // How the card's ‹ › arrows count and show the answer areas of the presented card.
    void setAnswerAreaNavigation(std::function<int()> count, std::function<bool(int)> focus);
    // The answer feedback in the reader (review-card-feedback.md §6.3): @p states gives each
    // feedback_id's state as the reader finds it in the current answer, also while masked
    // (Part::reviewFeedbackStates), for Copy for agent's "(not found in the current answer)".
    void setAnswerFeedbackStates(std::function<QVariantMap()> states);
    // The card under review while its answer is masked, else empty: the History in the sidebar
    // does not open that card nor show its comments (ReviewFeedbackPanel::setMaskedCard).
    QString maskedHistoryCard() const;
    // The cards that exist whose feedback may be shared with @p card's answer (card_id -> record,
    // ReviewStore::feedbackHomeCards), for the reader presenting it (Part::setReviewFeedbackHomeCards).
    QJsonObject feedbackHomeCards(const ReviewCard &card);
    // The outline of a card's PDF (Part::reviewDocumentOutline()), for its section note.
    void setContextOutline(std::function<QVariantList(const ReviewCard &)> outline);
    // Presents the current card masked again after its PDF was reloaded (Shell::restoreReviewCard),
    // where the reader is; without it, the card presenter does.
    void setPresentationRestorer(std::function<bool(const ReviewCard &, QString *)> restorer);
    void setSyncInProgress(bool running);
    void reload();
    void reloadFromProjections();
    void reviewCard(const QString &cardId);
    void showLandingPage();
    // The agent conversations of the projections changed: the card's newest-conversation line
    // and its menu follow (card-agent-conversations spec section 4), without a reload.
    void refreshAgentConversations(const QJsonObject &projections = {});

Q_SIGNALS:
    /** "Remove from this Card": @p record is the conversation's next revision, to be queued. */
    void agentConversationChangeRequested(const QVariantMap &record);
    void cardInfoRequested(const QString &cardId);
    void openSourceRequested(const QString &sourcePath, int line);
    void reviewStarted();
    /** A review was recorded here; other windows' queues must reload. */
    void reviewRecorded();
    /** Flip: cover everything except the answer (true) or only the answer (false). */
    void reviewMaskInvertedChanged(bool inverted);
    /** The answer was revealed: outline it instead of hiding it. */
    void answerRevealed();
    /** "Open this version ›" of feedback not in the current answer (as ReviewCardsWidget's). */
    void pdfVersionOpenRequested(const QString &pdfPath, const QString &title, int page, const QVariantList &regions, bool current);
    /** The card menu's "Show history": the History of @p cardId's document, in the sidebar of the tab under review. */
    void cardHistoryRequested(const QString &cardId);
    /** maskedHistoryCard() changed. */
    void maskedHistoryCardChanged(const QString &cardId);

public Q_SLOTS:
    // The card menu's Show history and maskedHistoryCard(), as the review stands now.
    void refreshAnswerFeedback();
    // A reload of the PDF took away the mask or frame of @p cardId (Part::reviewPresentationLost):
    // the current card comes back as the bar shows it, masked or revealed, once it is placed on the
    // reloaded PDF (else when Sync's projections bring its places there).
    void restoreReviewPresentation(const QString &cardId);
    // The current card was made in an HTML attachment and its text is no longer in the page
    // (Part::reviewAnswerNotFound): the panel says so and shows the stored answer once revealed.
    void showAnswerNotFound(const QString &cardId);

public:
    QString answerNotFoundCardId() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void showBrowseCards();
    void showAttentionCards();
    void showReviewLanding();
    void refreshDueQueue();
    // Start Review: the Study entry (knowledge cards) of the whole Library, or every card of the
    // document chosen (review-card-kinds.md §7).
    void startReview();
    void revealAnswer();
    void setMaskFlipped(bool flipped);
    void showAnswerPages(const ReviewCard &card);
    void updateAnswerControls();
    void toggleMaskFlipped();
    void hideAnswer();
    void chooseTags();
    void closeTagsDialog();
    void chooseInterval();
    void recordNext();
    void stepAnswerArea(int step);
    void rateRetained();
    void chooseSkip();
    void skipWithDefaults();
    static ReviewInterval defaultSkipInterval();
    void undoSkip();
    void undoPrevious();
    void focusReview();
    void toggleReviewVisibility();
    void deleteCard();
    // Takes the card out of review for good, keeping its history; Undo writes undo_retire.
    void retireCard();
    // The card menu's Make project card, Make knowledge card and Follow the Project (@p kind:
    // knowledge, project or follow_project) for the card under review.
    void setCurrentCardKind(const QString &kind);
    void showCardInfo();
    void copyCardForAgent();
    QString currentReviewText(const ReviewCard &card) const;
    void editCurrentCard();
    void previousQuestion();
    void updateCardMenu();

private:
    // The line above Reveal with the card's newest agent conversation.
    // @p cardChanged: a new card or a re-shown bar, where the line may appear or go; otherwise
    // (the projections changed) it only follows while the knowledge is not being set.
    // @p projections: the new projections when the caller has them, else the provider's.
    void refreshCardConversations(bool cardChanged, const QJsonObject &projections = {});
    void updateConversationStatus();
    void elideConversationTitle();
    void openCardConversation(const QJsonObject &record);
    void fillAgentConversationsMenu();
    bool refreshDeferred() const;
    QString dashboardSnapshotPath() const;
    // The skipped cards' bar drafts are kept on disk, so a restart (an update) does not lose them.
    QString barDraftsPath() const;
    void loadBarDrafts();
    void saveBarDrafts() const;
    void restoreVerifiedCounts();
    void saveVerifiedCounts(int newCards, int learningCards, int reviewCards, int unavailable) const;
    void displayCounts(int newCards, int learningCards, int reviewCards, int unavailable);
    // While Sync or the source scan defers the refresh: keep the frozen dashboard usable
    // and, when only the scan defers it, retry the reload until the scan finishes.
    void applyDeferredState();
    void buildUi();
    // The document filter lists @p stems (the documents with cards), keeping the one chosen.
    void applyDocumentStems(QStringList stems);
    // The dashboard's counts from the queue loaded here, the unavailable cards as last counted; a
    // background refresh (requestQueueRefresh) brings the rest.
    void updateDueSummary();
    // The review queue, the document filter and the counts are rebuilt off the GUI thread, from
    // one reading of every card: Sync's refresh and the Library's reloads after it froze the
    // window about 3 s each when this ran on it (13 Nov). Requests within a moment, or while one
    // runs, become one; a result made for another card under review or another filter is not
    // applied (the next one is).
    struct QueueRefresh {
        QString filter; // The document filter it was made for.
        QString sessionCardId; // The card under review when it was asked for; empty outside a review.
        QStringList stems;
        QStringList projectIds; // The Projects that hold at least one active project card.
        QList<ReviewCard> due;
        int unavailable = 0;
        int cards = 0; // Read in the background, in durationMs.
        qint64 durationMs = 0;
    };
    void requestQueueRefresh();
    void startQueueRefresh();
    void applyQueueRefresh(const QueueRefresh &result);
    void displayQueueCounts(const QList<ReviewCard> &due, int unavailable);
    void showCurrentCard();
    void setSessionControlsVisible(bool visible);
    void attachSessionPanel();
    void releaseSessionHost();
    void positionSessionPanel();
    // While the reader shows a native view (the HTML view's web view), child widgets over it are
    // hidden behind it: the panel and its Hide button become frameless tool windows meanwhile
    // (host property "thoughtsNativeDocumentView").
    void applySessionPanelWindowing();
    bool sessionPanelFloats() const;
    void updateRatingLabels(const ReviewCard &card);
    void updateSessionMessage(const QString &message);
    void openCardBrowser(const QString &stateFilter);
    void rateCurrent(const QString &rating, const QDateTime &scheduledDueAt = {}, const QDateTime &reviewedAt = QDateTime::currentDateTimeUtc(), const QStringList &tags = {},
                     const QHash<QString, QString> &groupNotes = {}, const QHash<QString, ReviewContextNotes::Note> &contextNotes = {});
    // The review in two steps (review-phases spec). The before panel under the question: the
    // groups asked before the answer (sliders) and the before note, for the card shown; nothing
    // is chosen when it appears. Rebuilt when the card or the groups change.
    void rebuildBeforePanel();
    // Shows the panel editable until the answer is first shown, then as one locked line, with
    // the agent's prediction under it.
    void updateBeforePanel();
    // The answer is shown for the first time (Reveal): the before step locks, and each group
    // asked before and after starts the after step at the step predicted.
    void lockBeforeStep();
    // Whether @p tag belongs to a group asked before the answer only (Feeling).
    bool beforeOnlyTag(const QString &tag) const;
    // What an answer of the card shown records: the before-only groups' tags and the after step's
    // @p afterTags (answerTags), and the two steps with the agent's prediction (answerPhases).
    QStringList answerTags(const QStringList &afterTags) const;
    QJsonObject answerPhases(const QStringList &afterTags) const;
    // How this session started (ReviewEntry): Review now of one card, or the due cards.
    QString reviewEntry() const;
    // Asks the agent recall prediction service for @p card, once per card in a due session, in
    // the background; nothing without recall-predictor.json or a group asked before and after.
    void requestRecallPrediction(const ReviewCard &card);
    // The service's answer for @p cardId (a tag of the recall group); kept for the session.
    void applyRecallPrediction(const QString &cardId, const QJsonObject &response);
    void chooseAnswer(const QStringList &selectedTags, bool requireRevealed = true);
    void refreshBarLabels();
    void rateScoped(const QString &rating, const QDateTime &scheduledDueAt, const QStringList &tags, ReviewSkipScope scope, bool dueOnly,
                    const QHash<QString, QString> &groupNotes = {}, const QHash<QString, ReviewContextNotes::Note> &contextNotes = {});
    // The card's section, page and document notes as edited for this answer, read when first needed.
    ReviewContextNotes::Draft &contextDraft(const ReviewCard &card);
    // Whole-document cards become document notes (ReviewStore::convertWholeDocumentCards), once.
    void convertWholeDocumentCards();
    // Postpones without answering: the card keeps its tags.
    void skipCurrent(const ReviewInterval &interval, ReviewSkipScope scope, bool dueOnly);
    const QHash<QString, int> &tagUsage();
    // A card's tags changed from @p from to @p to (merged names): moves it between the counts.
    void moveTagUsage(const QStringList &from, const QStringList &to);
    void updateReviewVisibilityButton();
    void updateSessionExtras();
    bool ensureCurrentGeometry(ReviewCard *card, QString *error);
    QString currentPdfPath(const ReviewCard &card) const;
    static QString fileSha256(const QString &path);
    QVariantMap locateVisualPage(const QString &pdfPath, const QString &pageVisualSha256) const;

    bool m_syncInProgress = false;
    // Due cards (new + learning + review) the dashboard currently shows, once it shows counts.
    bool m_countsShown = false;
    int m_displayedDue = 0;
    // The two Library-wide entries (review-card-kinds.md §7): due knowledge and project cards, and
    // the Projects that hold an active project card (the Projects entry's choice).
    int m_studyDue = 0;
    int m_projectDue = 0;
    QStringList m_projectEntryIds;
    // The entry the session was started from: its kind (empty: every card of its scope) and Project.
    QString m_sessionKind;
    QString m_sessionProjectId;
    // A session of @p kind's cards (empty: all), of the Project @p projectId only when given.
    void startEntryReview(const QString &kind, const QString &projectId);
    // The cards of @p due the session's entry offers.
    QList<ReviewCard> entryCards(const QList<ReviewCard> &due) const;
    // The entries' labels and whether they can start.
    void updateEntryButtons();
    void fillProjectsMenu();
    // "Project card · Changed since you last saw it" over the card under review.
    void updateCardKindLine();
    bool m_deferredRetryScheduled = false;
    QTimer *m_queueRefreshTimer = nullptr;
    bool m_queueRefreshRunning = false;
    bool m_queueRefreshPending = false; // Asked for again while one ran.
    int m_unavailableCount = 0; // As the last background refresh counted them.
    QString m_docsRoot;
    ReviewStore m_store;
    DocumentProvider m_documentProvider;
    CardPresenter m_cardPresenter;
    std::function<bool(const ReviewCard &, QString *)> m_presentationRestorer;
    // The current card, whose presentation a reload took away until it is placed on the new PDF.
    QString m_lostPresentationCardId;
    QString m_answerNotFoundCardId;
    MaskClearer m_maskClearer;
    CardBrowserOpener m_cardBrowserOpener;
    SessionHostProvider m_sessionHostProvider;
    QList<ReviewCard> m_dueCards;
    QList<QPair<QString, QString>> m_reviewHistory;
    QHash<QString, QList<QPair<QString, QString>>> m_scopedRatingEvents;
    // Entries of m_reviewHistory that are retires (Undo writes undo_retire), by event_id.
    QSet<QString> m_retireEvents;
    QToolButton *m_cardMenuButton = nullptr;
    QAction *m_feedbackHistoryAction = nullptr; // Card menu: Show history of the current card.
    QString m_maskedHistoryCard;
    std::function<QVariantMap()> m_answerFeedbackStates;
    QSet<QString> orphanedFeedback() const;
    // m_answerFeedbackStates(), or empty.
    QVariantMap answerFeedbackStates() const;
    // The answer feedback of @p card in the review: its own records (deleted ones too with
    // @p includeDeleted) and the other cards' records found in its answer (shared: in @p states,
    // as the reader finds them), in created_at order.
    QList<QJsonObject> sessionFeedback(const ReviewCard &card, bool includeDeleted, const QVariantMap &states) const;
    // The cards whose feedback is in @p feedback of @p card and that exist (ReviewStore::feedbackHomeCards),
    // as read when the card was presented; empty without shared feedback.
    QHash<QString, QJsonObject> sessionHomeCards(const ReviewCard &card, const QList<QJsonObject> &feedback);
    QString m_homeCardsCardId;
    QHash<QString, QJsonObject> m_homeCards;
    // The open Tags window; it leaves the document behind it scrollable.
    QPointer<QDialog> m_tagsDialog;
    PdfVersions::Fetcher *m_versionFetcher = nullptr;
    QPointer<QPushButton> m_reviewVisibilityButton;
    bool m_reviewHidden = false;
    bool m_lastActionWasSkip = false;
    int m_currentReview = -1;
    bool m_sessionComplete = false;
    bool m_singleCardSession = false;
    ReviewInterval m_interval;
    ReviewSkipBatch m_pendingSkip;
    QString m_skippedCardId;
    bool m_skippedSingleCardSession = false;

    QStackedWidget *m_reviewStack = nullptr;
    QWidget *m_overviewPage = nullptr;
    QPointer<QFrame> m_sessionPage;
    QPointer<QWidget> m_sessionHost;
    QPointer<QObject> m_sessionHostGuard;
    QWidget *m_ratingPanel = nullptr; // The Tag | Interval | Next bar, shown with the answer (Reveal).
    QPushButton *m_doneButton = nullptr;
    QPushButton *m_browseCardsButton = nullptr;

    QComboBox *m_documentFilter = nullptr;
    QLabel *m_dueSummary = nullptr;
    QLabel *m_newCount = nullptr;
    QLabel *m_learningCount = nullptr;
    QLabel *m_reviewCount = nullptr;
    QFrame *m_attentionPanel = nullptr;
    QLabel *m_attentionSummary = nullptr;
    QPushButton *m_attentionButton = nullptr;
    QString m_attentionBrowserState;
    QLabel *m_reviewStatus = nullptr;
    QLabel *m_cardKindLine = nullptr;
    QPushButton *m_startProjectsButton = nullptr; // "Projects · M due", with the choice of a Project.
    QMenu *m_projectsMenu = nullptr;
    QLabel *m_reviewQuestion = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_revealButton = nullptr;
    bool m_maskFlipped = false;
    QStringList m_selectedTags;
    // The tag groups the bar's tag cell orders slider steps by; read when a card is shown.
    QList<ReviewTags::TagGroup> m_barTagGroups;
    // This answer's note per group (key "other" for Other); kept while the tag picker closes and
    // reopens, recorded on Next with the tags, cleared for the next card.
    QHash<QString, QString> m_groupNotes;
    // The section, page and document notes as edited for the card shown (for its cardId); like the
    // group notes, recorded with the answer and dropped for the next card.
    ReviewContextNotes::Draft m_contextDraft;
    std::function<QVariantList(const ReviewCard &)> m_contextOutline;
    bool m_wholeDocumentCardsConverted = false;
    QString m_barCardId; // The card the bar's tags and interval were chosen for.
    // What the bar held when a card was skipped: shown again, unsaved, when that card returns
    // (on this device, also after a restart), unless the card got a new answer meanwhile.
    struct BarDraft {
        QStringList tags;
        ReviewInterval interval;
        QHash<QString, QString> groupNotes;
        QDateTime answeredAt; // The card's last answer the draft was chosen after.
        ReviewContextNotes::Draft context = {}; // Kept only when something was typed.
        QDateTime savedAt = {}; // Drafts of cards never shown again are forgotten after a while.
    };
    QHash<QString, BarDraft> m_barDrafts;
    // The before step of the card shown (review-phases spec).
    struct BeforeStep {
        QString cardId;
        QStringList tags; // Picked in the before panel, in the groups asked before.
        QString note; // The before note, as typed.
        QDateTime answerShownAt; // When the answer was first shown; invalid until then. Set: locked.
        // A group was asked before the answer (a slider of a group asked before): only then is the
        // panel shown and the answer recorded in two steps, as on the phone.
        bool shown = false;
        // Opened again with the pencil after the answer was shown (the phone's "Back to before
        // step"), until the answer is shown again; answerShownAt keeps the first time.
        bool reopened = false;
        // The card's last answer when the step began (invalid: never answered): the answer the
        // step kept on disk belongs to (keptBeforeStep).
        QDateTime answeredAt = {};
        bool locked() const { return answerShownAt.isValid() && !reopened; }
    };
    // The before step of each card is kept on this computer until that card's answer is recorded,
    // also through a restart: a card that leaves the review before it is answered (unavailable for
    // a while, skipped, the review ended or the app quit) comes back with what was picked and
    // typed, in the same review or a later one. A step belongs to the card's last answer when it began; another
    // last answer (recorded here or on another device) makes it obsolete. A step with no pick and
    // no note keeps nothing, so clearing them clears it.
    QString beforeStepsPath() const;
    // The step kept for @p card, or an empty one; an obsolete kept step is dropped.
    BeforeStep keptBeforeStep(const ReviewCard &card) const;
    // Keeps m_before as it is now (after a pick, the note, Reveal, the pencil).
    void keepBeforeStep();
    // The cards' answers were recorded (or the cards deleted or merged away): their steps go.
    void forgetBeforeSteps(const QStringList &cardIds);
    // Each group asked before and after starts the after step at the step picked before (Reveal).
    void startAfterStepAtPrediction();
    QTimer *m_beforeNoteSaveTimer = nullptr; // Typing in the before note is kept a moment later.
    // showCurrentCard(), keeping @p kept when it is the before step of the card shown: the same
    // card shown again (after Edit), or the step carried into the card a merge made of it.
    void showCurrentCard(const BeforeStep &kept);
    // The pencil by the before line: the answer is covered again and the before rows open on the
    // picks made, until the answer is shown again (the phone's "Before: … · Tap to change").
    void reopenBeforeStep();
    // The before line as wide as the bar lets it be, on one line; its whole text is the tooltip.
    void elideBeforeSummary();
    BeforeStep m_before;
    QWidget *m_beforePanel = nullptr;
    QWidget *m_beforeGroups = nullptr; // The groups' sliders, rebuilt with the groups.
    QPlainTextEdit *m_beforeNote = nullptr;
    // Once the answer is shown, the bar's line under the question: "Recall: Partly recalled ·
    // Feeling: OK — note…" ("Before: not rated" without picks), then the pencil.
    QWidget *m_beforeSummaryRow = nullptr;
    QLabel *m_beforeSummary = nullptr;
    QString m_beforeSummaryText; // m_beforeSummary's text before it is elided.
    QToolButton *m_beforeEditButton = nullptr;
    QLabel *m_predictionLine = nullptr; // "Agent predicted: …", once locked.
    // The agent's recall predictions of this due session, by card ID: asked once per card.
    struct RecallPrediction {
        QString groupId;
        QStringList scale;
        QDateTime requestedAt;
        QDateTime arrivedAt;
        QJsonObject result; // The service's answer; empty while pending, or when it failed.
        bool pending = false;
        QPointer<QNetworkReply> reply;
    };
    QHash<QString, RecallPrediction> m_predictions;
    QNetworkAccessManager *m_predictionNetwork = nullptr;
    // Forgets the predictions of the last due session, its requests aborted.
    void clearRecallPredictions();
    QHash<QString, QString> m_tagMerges; // Those of the last card shown; tagUsage() is counted under them.
    std::optional<QHash<QString, int>> m_tagUsage; // Cards per current tag: loaded once per session, then counted locally.
    QWidget *m_answerRow = nullptr;
    QLabel *m_currentState = nullptr;
    QPushButton *m_tagField = nullptr;
    QPushButton *m_intervalField = nullptr;
    QPushButton *m_nextButton = nullptr;
    QToolButton *m_previousAreaButton = nullptr;
    QToolButton *m_nextAreaButton = nullptr;
    int m_answerArea = 0;
    std::function<int()> m_answerAreaCount;
    std::function<bool(int)> m_focusAnswerArea;

    // Agent conversations of the current card (card-agent-conversations spec section 4).
    ReviewStore::ProjectionProvider m_conversationProjections;
    QPushButton *m_conversationLine = nullptr;
    QLabel *m_conversationIcon = nullptr;
    QLabel *m_conversationTitle = nullptr;
    QLabel *m_conversationDetails = nullptr;
    QLabel *m_conversationStatus = nullptr; // Claude: working (accent) or open (grey)
    QJsonObject m_conversationShown; // the record the line shows
    QString m_conversationCardId; // the card it was chosen for
    QTimer *m_conversationStatusTimer = nullptr;
    QMenu *m_agentConversationsMenu = nullptr;

};

}

#endif
