// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "agentnotes.h"

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

class QAbstractButton;
class QAction;
class QComboBox;
class QDialog;
class QLabel;
class QPlainTextEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

class AgentNotesTest;
class MainShellTest;

namespace Thoughts
{

// A note's or reply's picture at the width of its view (at most MaxWidth), crisp on Retina.
class AgentNoteImageView : public QWidget
{
    Q_OBJECT
public:
    explicit AgentNoteImageView(QWidget *parent = nullptr);

    static constexpr int MaxWidth = 760;

    void setSource(const QString &sha256, const QSize &expectedSize);
    QString sha256() const { return m_sha256; }
    void setImage(const QImage &image);
    void setFailure(const QString &message);
    QImage image() const { return m_image; }
    QString statusText() const { return m_status; }
    bool failed() const { return m_image.isNull() && !m_loading && !m_sha256.isEmpty(); }
    void setCachedPath(const QString &path) { m_cachedPath = path; }
    // Fits the picture into @p available pixels of width.
    void fitWidth(int available);
    // A click on the picture emits clicked() (the sidebar's way to see it large).
    void setClickable(bool clickable);
    // The note's sources on the picture: a click on one emits linkActivated(href) instead of
    // clicked(), and the pointer and a tooltip show where it goes.
    void setLinks(const QList<AgentNotes::NoteLink> &links);
    QList<AgentNotes::NoteLink> links() const { return m_links; }
    // The reply's drawing over the picture (null: none). Kept alive by the caller.
    void setDrawing(const AgentNotes::AnnotationDocument *drawing);
    // The link under @p position (widget pixels), or -1.
    int linkAt(const QPointF &position) const;

Q_SIGNALS:
    void clicked();
    void linkActivated(const QString &href);

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void updateSize();
    void updateCursor(int link);

    QString m_sha256;
    QSize m_expectedSize;
    QImage m_image;
    QString m_status;
    QString m_cachedPath;
    int m_available = MaxWidth;
    bool m_loading = false;
    bool m_clickable = false;
    QPixmap m_scaled;
    QList<AgentNotes::NoteLink> m_links;
    const AgentNotes::AnnotationDocument *m_drawing = nullptr;
    int m_hoveredLink = -1;
};

// The notes, newest first with unread ones marked, and the chosen note with its picture,
// links and the replies from the phone. Two layouts: Split (the Agent Notes window: the list
// beside the note) and Sidebar (a reader's sidebar: the list, or one note in its place).
// Above an agent's note, always in view, one line says which agent listens for a reply, and
// "Change" lets the user choose who answers.
class AgentNotesView : public QWidget
{
    Q_OBJECT
    friend class ::AgentNotesTest;
    friend class ::MainShellTest;

public:
    struct DocumentLink {
        QString path;  // empty: not on this Mac
        QString title;
    };
    using DocumentResolver = std::function<DocumentLink(const QString &documentId, const QString &filename)>;
    using DocumentOpener = std::function<void(const QString &documentId, const QString &path)>;
    using CardOpener = std::function<void(const QString &cardId)>;
    using NoteOpener = std::function<void(const QString &noteId)>;
    // The page on screen in the reader, to send to the agent: what it is, and how to draw it.
    struct PageCapture {
        QString documentName;
        QString documentId;
        QString documentFilename;
        QString reviewCardId;
        int page = 0;  // 1-based
        int pages = 0;
        std::function<QImage(int width)> render;
    };
    using PageProvider = std::function<std::optional<PageCapture>()>;
    // The Library's documents by project, for the filter (the Shell reads the catalogue).
    using LibraryProvider = std::function<QList<AgentNotes::LibraryDocument>()>;

    enum class Layout { Split, Sidebar };

    AgentNotesView(Layout layout, AgentNotes::Center *center, DocumentResolver resolveDocument, DocumentOpener openDocument, CardOpener openCard,
                   QWidget *parent = nullptr, Qt::WindowFlags flags = {});
    ~AgentNotesView() override;

    Layout viewLayout() const { return m_layout; }
    // Split: selects the note. Sidebar: shows it in place of the list. Waits for the list
    // when the note is not in it yet.
    void selectNote(const QString &noteId);
    // The note selected in the list (Split: the one shown).
    QString currentNoteId() const;
    // The note on screen: Split, the selected one; Sidebar, the one shown in place of the
    // list (empty while the list shows).
    QString shownNoteId() const;
    // Sidebar: back from a note to the list.
    void showList();
    // Sidebar: "Open Large" and a click on a picture show the note in the Agent Notes window.
    void setLargeOpener(NoteOpener openLarge) { m_openLarge = std::move(openLarge); }
    // "Send Page to Agent…": the page it sends (none: the button is hidden).
    void setPageProvider(PageProvider provider);
    // Opens a link of a note's picture (a Library PDF page or a web page).
    using LinkOpener = std::function<void(const QString &href)>;
    void setLinkOpener(LinkOpener open) { m_openLink = std::move(open); }
    // A link of a note's picture was clicked.
    void openLink(const QString &href);
    void setLibraryProvider(LibraryProvider provider);

    // Only a project's or a document's notes (empty: all). Older notes of those documents are
    // asked for too; the "Show" menu follows.
    void setFilter(const AgentNotes::Filter &filter);
    AgentNotes::Filter filter() const { return m_filter; }
    // The notes listed now, after the filter.
    int listedCount() const;
    // The window's notes list is shown (Hide Sidebar, ⌘B); always for the sidebar's own list.
    bool listShown() const;

    // The phone's tools: draw on the note's picture (and reply), send a page, delete a note.
    void openComposer(const QString &noteId);
    void sendPage();
    void deleteNote(const QString &noteId);

    enum Roles { NoteIdRole = Qt::UserRole + 1, UnreadRole, MetaRole, PreviewRole };

    // How often a view asks who listens for replies to the note it shows.
    static constexpr int ListeningRefreshMs = 60 * 1000;
    // How long the line says that a choice was not saved.
    static constexpr int AnswerFailureShownMs = 12 * 1000;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *createDetail(QWidget *parent);
    void rebuildList();
    void updateUnreadMarks();
    void updateStatus();
    void currentChanged(QListWidgetItem *current);
    void openInPlace(const QString &noteId);
    void markSeenSoon(const AgentNotes::Note &note);
    void showDetail(const AgentNotes::Note &note, bool sameNote);
    void showPlaceholder(const QString &text);
    void rebuildReplies(const AgentNotes::Note &note);
    void loadImage(AgentNoteImageView *view, const QString &url, const QString &sha256, const QString &mediaType);
    void fitImages();
    bool readsNow() const;
    void openLarge();
    void updateReplyBox();
    // Highlight, Pen and Erase at the top of a note: draw on the note in place (None: read it). The
    // drawing is the reply's, as Draw on the Picture… makes it, and stays shown over the note.
    void setDrawTool(int tool);
    void syncDrawing();
    QWidget *createAnswerLine(QWidget *parent);
    // The line above the note shown: whether an agent listens for a reply, which one, and
    // "Change" (AgentNotes::answerLine). No line for a page the user sent, nor while the server
    // has not said or cannot tell.
    void updateAnswerLine();
    // The Sources choice, or in its place the conversation the reply continues.
    void updateSourcesChoice();
    // The window's notes list beside the open note (Split), hidden or shown; @p remember saves it.
    void setListShown(bool shown, bool remember);
    // The agent's note on screen whose line is kept up to date; empty when there is none.
    QString answerNoteId() const;
    // The list "Change" opens: Automatic and the agents that could answer now, the current
    // choice checked. The caller owns it; null when the server said nothing about the note.
    QMenu *createAnswerMenu();
    void showAnswerMenu();
    // Sends the choice; the line follows the server's answer, and after a failure it says so
    // and the choice stays as it was.
    void chooseAnswerer(const QString &noteId, const std::optional<AgentNotes::Agent> &agent);
    void showAnswerFailure(const QString &noteId, const QString &text);
    void showListMenu(const QPoint &position);
    void rebuildFilterMenu();
    void filterChosen(int index);

    Layout m_layout;
    QPointer<AgentNotes::Center> m_center;
    DocumentResolver m_resolveDocument;
    DocumentOpener m_openDocument;
    LinkOpener m_openLink;
    CardOpener m_openCard;
    NoteOpener m_openLarge;
    PageProvider m_pageProvider;
    LibraryProvider m_libraryProvider;
    AgentNotes::Filter m_filter;
    QComboBox *m_filterBox = nullptr;
    bool m_settingFilterBox = false;
    // Asks before deleting (a test answers by itself).
    std::function<bool(const QString &title)> m_confirmDelete;
    QHash<QString, QPointer<QDialog>> m_composers;
    bool m_settingReply = false;

    QLabel *m_status = nullptr;
    QAbstractButton *m_markAllRead = nullptr;
    // Hide Sidebar / Show Sidebar (⌘B) in the window: its notes list.
    QAction *m_toggleList = nullptr;
    QAbstractButton *m_refresh = nullptr;
    QAbstractButton *m_sendPage = nullptr;
    QListWidget *m_list = nullptr;
    // Split: the placeholder or the note, beside the list. Sidebar: the list or the
    // placeholder (m_listStack) on the list page, the note on the note page (m_pages).
    QStackedWidget *m_detailStack = nullptr;
    QStackedWidget *m_listStack = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_listPage = nullptr;
    QWidget *m_notePage = nullptr;
    QLabel *m_placeholder = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_detail = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QPushButton *m_openDocumentButton = nullptr;
    QPushButton *m_openCardButton = nullptr;
    QPushButton *m_openConversationButton = nullptr;
    QLabel *m_question = nullptr;
    QLabel *m_message = nullptr;
    AgentNoteImageView *m_image = nullptr;
    QWidget *m_replyBox = nullptr;
    // Split layout: the reply box under the note, always in view.
    QWidget *m_replyDock = nullptr;
    QWidget *m_drawTools = nullptr;
    QToolButton *m_highlightTool = nullptr;
    QToolButton *m_penTool = nullptr;
    QToolButton *m_eraseTool = nullptr;
    QToolButton *m_undoTool = nullptr;
    QToolButton *m_numberTool = nullptr;
    QToolButton *m_redoTool = nullptr;
    QToolButton *m_clearTool = nullptr;
    class AgentNoteCanvas *m_canvas = nullptr;
    // The draft drawn on in place or shown over the note: kept alive while it is.
    std::shared_ptr<AgentNotes::Center::Draft> m_drawnDraft;
    // Split: the line that says who listens, over the note (m_scroll).
    QWidget *m_notePane = nullptr;
    QWidget *m_answerLine = nullptr;
    QLabel *m_answerMark = nullptr;
    QLabel *m_answerText = nullptr;
    QToolButton *m_answerChange = nullptr;
    // What to tell the user after a choice that was not saved, for a while.
    QLabel *m_answerFailure = nullptr;
    QTimer *m_answerFailureTimer = nullptr;
    QString m_answerFailureNoteId;
    // Asks every minute who listens, while an agent's note is on screen and Thoughts is in front.
    QTimer *m_listeningTimer = nullptr;
    QPlainTextEdit *m_replyField = nullptr;
    QLabel *m_replyStatus = nullptr;
    QPushButton *m_sendReply = nullptr;
    // What the agent may use when it answers (AgentNotes::ReplySources), sent with every reply.
    QComboBox *m_sourcesBox = nullptr;
    QLabel *m_sourcesLabel = nullptr;
    QLabel *m_continuesConversation = nullptr;
    QLabel *m_repliesHeader = nullptr;
    QWidget *m_replies = nullptr;
    QVBoxLayout *m_repliesLayout = nullptr;

    QString m_shownNoteId;
    QString m_shownDocumentId;
    QString m_shownDocumentPath;
    QString m_shownCardId;
    QUrl m_shownConversation;
    QString m_requestedNoteId;
    // Split: the note shown is one the list does not hold (a reading overlay mark's note, opened
    // from its mark), shown with nothing selected in the list until the user picks a note there.
    bool m_unlistedShown = false;
};

// File › Agent Notes: its own window, the list beside the note.
class AgentNotesWindow : public AgentNotesView
{
    Q_OBJECT
public:
    AgentNotesWindow(AgentNotes::Center *center, DocumentResolver resolveDocument, DocumentOpener openDocument, CardOpener openCard,
                     QWidget *parent = nullptr);

    // Selects the note (once the list holds it) and brings the window forward.
    void showNote(const QString &noteId);
};

// Send to Agent's sessions (a tab's menu, File › Send to Agent): the sessions reply wakers can wake
// (AgentNotes::recipients), as last fetched, kept in the app's settings so a menu lists them at
// once, and fetched again in the background whenever a menu shows them. Also the session the user
// sent to last.
class AgentSessionList : public QObject
{
    Q_OBJECT
public:
    explicit AgentSessionList(AgentNotes::Center *center, QObject *parent = nullptr);
    // The app's, on AgentNotes::Center::instance().
    static AgentSessionList *instance();

    QList<AgentNotes::Recipient> sessions() const { return m_sessions; }
    bool isRefreshing() const { return m_refreshing; }
    // Why the last refresh did not bring the list (empty when it did).
    QString error() const { return m_error; }
    // Asks the server again (GET /v1/agent-notes/wakers) unless a request is on its way; changed()
    // follows the answer. A failure keeps the sessions known.
    void refresh();

    // The session sent to last ("" before any), with its label as it was then.
    QString lastSession() const;
    QString lastSessionLabel() const;
    void setLastSession(const AgentNotes::Recipient &session);
    std::optional<AgentNotes::Recipient> session(const QString &sessionId) const;

    // The proposals the sessions wrote (pending and decided, every version), for the order: the
    // sessions with the most on the document first (AgentNotes::sessionMenu). Set by the loader,
    // which refresh() calls to read them again in the background.
    QList<AgentNotes::ProposalAuthorship> proposals() const { return m_proposals; }
    void setProposals(const QList<AgentNotes::ProposalAuthorship> &proposals);
    void setProposalLoader(std::function<void()> load) { m_loadProposals = std::move(load); }

    // @p menu lists the sessions as AgentNotes::sessionMenu orders them for @p document's (asked for
    // each time the menu is filled): at most MenuSessions, each with its proposals ("— 12 here",
    // "— 40 in all"), the one sent to last checked; the rest under More Sessions. A click calls
    // @p send. Without any, one disabled entry says why (listing, none to wake, or the failure).
    // Each time it opens it asks again in the background (and now too, with @p refreshNow) and
    // lists what comes while it is open.
    using DocumentProvider = std::function<AgentNotes::DocumentSources()>;
    void populate(QMenu *menu, std::function<void(const AgentNotes::Recipient &)> send, bool refreshNow = true, DocumentProvider document = {});

Q_SIGNALS:
    void changed();

private:
    // @p menu's entries as the list is now.
    void fill(QMenu *menu, const std::function<void(const AgentNotes::Recipient &)> &send, const DocumentProvider &document);

    QPointer<AgentNotes::Center> m_center;
    QList<AgentNotes::Recipient> m_sessions;
    bool m_refreshing = false;
    // The server answered this run (an empty list then means none can be woken).
    bool m_answered = false;
    QString m_error;
    QList<AgentNotes::ProposalAuthorship> m_proposals;
    std::function<void()> m_loadProposals;
};

}
