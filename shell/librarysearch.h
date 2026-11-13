/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_LIBRARYSEARCH_H
#define THOUGHTS_LIBRARYSEARCH_H

#include <QLineEdit>
#include <QList>
#include <QString>
#include <QStyledItemDelegate>

class QAction;
class QListWidget;
class QTimer;
class QToolButton;

namespace Thoughts
{
// A Library PDF to index page by page. identity changes whenever the file does.
struct LibrarySearchPdf {
    QString pdfPath;
    QString title; // used when neither a Library name nor PDF metadata names it
    QString project;
    QString identity;
    QString libraryTitle = {}; // the Library's own name (project, document or display name), if any
};

// A comment, note or review card, re-indexed in full on every update.
struct LibrarySearchRecord {
    QString kind; // "comment", "note" (a to-do), "diary" or "card"
    QString ref;  // annotation or card id
    QString pdfPath;
    int page = 0;
    QString title;
    QString body;
    QString context; // shown under the title, e.g. the document name
};

struct LibrarySearchHit {
    QString kind; // "document", "page", "comment", "note", "diary", "card", "source" (a file) or "source-line"
    QString ref;
    QString pdfPath;
    int page = 0;
    QString title;
    QString context;
    QString snippet; // plain text; matched words are wrapped in \x01 … \x02
    QString sourcePath = {}; // sources: the file to open
    int line = 0;            // source-line: the 1-based line of the match
    int matchingPages = 0;   // document: the pages with the best match; page is the best of them
    int matches = 0;         // document: how many times the words occur on those pages
};

// What the search field looks through. Documents is the Library's PDFs with
// their comments, notes and review cards; Sources the projects' source files.
enum class LibrarySearchScope { Documents, Sources, All };

// A source folder of a Library project: a docs-* folder or a project's
// default folder (see SourceOwner).
struct LibrarySearchSourceFolder {
    QString owner;
    QString project;
};

// A text source file to index. identity changes whenever the file does.
struct LibrarySearchSource {
    QString path;         // absolute
    QString relativePath; // shown and matched, e.g. docs-paper/tex/main.tex
    QString project;
    QString identity;
};

struct LibrarySearchSourceUpdate {
    int files = 0;     // listed
    int read = 0;      // re-read because their identity changed
    int unchanged = 0; // re-read but with the same content (only the identity was stored)
    int removed = 0;
    int chunks = 0;    // text blocks written
    qint64 readMs = 0;
    qint64 writeMs = 0;
};

struct LibrarySearchUpdate {
    int indexedPdfs = 0;
    int removedPdfs = 0;
    int pages = 0;
    int records = 0;
    qint64 extractMs = 0;
    qint64 writeMs = 0;
};

// Full-text index (SQLite FTS5) of the Library's PDF text, comments and review
// cards. It is a cache: deleting the file only costs a rebuild. Every call opens
// its own connection, so updates can run on a worker thread while the window
// searches on the GUI thread.
class LibrarySearchIndex
{
public:
    explicit LibrarySearchIndex(QString databasePath);

    // Re-extracts PDFs whose identity changed, drops PDFs no longer listed, and
    // replaces every record of replaceKinds ("comment", "note", "card") with
    // records. PDF text is extracted in parallel; the database is written in
    // one transaction.
    bool update(const QList<LibrarySearchPdf> &pdfs, const QList<LibrarySearchRecord> &records, const QStringList &replaceKinds, LibrarySearchUpdate *summary,
                QString *error) const;
    // Re-reads source files whose identity changed and drops files no longer
    // listed. Each file is indexed by its path and, in blocks of lines, by the
    // text a reader sees: Thoughts markers and the hidden parts of agent
    // proposals are blanked, so matches and line numbers follow the file.
    bool updateSources(const QList<LibrarySearchSource> &sources, LibrarySearchSourceUpdate *summary, QString *error) const;
    QList<LibrarySearchHit> search(const QString &query, QString *error = nullptr) const;
    QList<LibrarySearchHit> search(const QString &query, LibrarySearchScope scope, QString *error = nullptr) const;
    bool isEmpty() const;
    bool hasSources() const; // whether sources were indexed at least once

    // The text source files of these folders that Sync keeps (their
    // .ThoughtIgnore rules), with identities. Reads only metadata.
    static QList<LibrarySearchSource> listSources(const QString &docsRoot, const QList<LibrarySearchSourceFolder> &folders);
    // Source files whose text is indexed (TeX, Markdown, BibTeX, styles, scripts, ...).
    static bool isTextSource(const QString &path);
    // The text of a source as a reader sees it, same length and lines.
    static QString visibleSourceText(const QString &path, const QString &text);

    // Each word of the query must occur; every word also matches as a prefix.
    static QString matchExpression(const QString &query);
    static QStringList queryWords(const QString &query);

private:
    QString m_path;
};

class LibrarySearchHitDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// The Library window's search field. Results appear in a list below it as you
// type; ↑/↓ move, Return opens, Escape closes the list (a second Escape clears).
class LibrarySearchField : public QLineEdit
{
    Q_OBJECT
public:
    explicit LibrarySearchField(QString databasePath, QWidget *parent = nullptr);
    // Shown in the result list while the first index is being built.
    void setIndexingStatus(const QString &status);
    void setSourceIndexingStatus(const QString &status);
    void refreshResults();
    LibrarySearchScope scope() const
    {
        return m_scope;
    }
    void setScope(LibrarySearchScope scope);
    static QString scopeName(LibrarySearchScope scope);
    static QString scopeKey(LibrarySearchScope scope); // "documents", "sources" or "all"
    static LibrarySearchScope scopeFromKey(const QString &key);

Q_SIGNALS:
    void hitActivated(const Thoughts::LibrarySearchHit &hit, const QString &query);
    void scopeChanged(Thoughts::LibrarySearchScope scope);

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void runSearch();
    void placeResults();
    void hideResults();
    void moveSelection(int step);
    void activateRow(int row);
    void updateScopeButton();
    QString currentStatus() const;

    LibrarySearchIndex m_index;
    LibrarySearchScope m_scope = LibrarySearchScope::Documents;
    QToolButton *m_scopeButton = nullptr;
    QList<QAction *> m_scopeActions;
    QString m_sourceIndexingStatus;
    QListWidget *m_results = nullptr;
    QTimer *m_delay = nullptr;
    QList<LibrarySearchHit> m_hits;
    QString m_indexingStatus;
};
}

#endif
