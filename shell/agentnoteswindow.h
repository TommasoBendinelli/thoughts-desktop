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
class QComboBox;
class QDialog;
class QLabel;
class QPlainTextEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
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

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void updateSize();

    QString m_sha256;
    QSize m_expectedSize;
    QImage m_image;
    QString m_status;
    QString m_cachedPath;
    int m_available = MaxWidth;
    bool m_loading = false;
    bool m_clickable = false;
    QPixmap m_scaled;
};

// The notes, newest first with unread ones marked, and the chosen note with its picture,
// links and the replies from the phone. Two layouts: Split (the Agent Notes window: the list
// beside the note) and Sidebar (a reader's sidebar: the list, or one note in its place).
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
    void setLibraryProvider(LibraryProvider provider);

    // Only a project's or a document's notes (empty: all). Older notes of those documents are
    // asked for too; the "Show" menu follows.
    void setFilter(const AgentNotes::Filter &filter);
    AgentNotes::Filter filter() const { return m_filter; }
    // The notes listed now, after the filter.
    int listedCount() const;

    // The phone's tools: draw on the note's picture (and reply), send a page, delete a note.
    void openComposer(const QString &noteId);
    void sendPage();
    void deleteNote(const QString &noteId);

    enum Roles { NoteIdRole = Qt::UserRole + 1, UnreadRole, MetaRole, PreviewRole };

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
    // The warning over the reply field while no agent listens for replies to the note shown.
    void updateListening();
    void showListMenu(const QPoint &position);
    void rebuildFilterMenu();
    void filterChosen(int index);

    Layout m_layout;
    QPointer<AgentNotes::Center> m_center;
    DocumentResolver m_resolveDocument;
    DocumentOpener m_openDocument;
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
    QLabel *m_listeningWarning = nullptr;
    // Asks now and then whether the note's agent still listens, while a note is on screen.
    QTimer *m_listeningTimer = nullptr;
    QPlainTextEdit *m_replyField = nullptr;
    QPushButton *m_drawButton = nullptr;
    QLabel *m_replyStatus = nullptr;
    QPushButton *m_sendReply = nullptr;
    QLabel *m_repliesHeader = nullptr;
    QWidget *m_replies = nullptr;
    QVBoxLayout *m_repliesLayout = nullptr;

    QString m_shownNoteId;
    QString m_shownDocumentId;
    QString m_shownDocumentPath;
    QString m_shownCardId;
    QUrl m_shownConversation;
    QString m_requestedNoteId;
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

}
