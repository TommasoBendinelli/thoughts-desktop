/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _FINDBAR_H_
#define _FINDBAR_H_

#include "okularpart_export.h"

#include "core/document.h"

#include <QWidget>

class QAction;
class KLineEdit;
class QLabel;
class QTimer;
class QTreeWidget;

class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(Okular::Document *document, QWidget *parent = nullptr);
    ~FindBar() override;

    QString text() const;
    OKULARPART_EXPORT Qt::CaseSensitivity caseSensitivity() const;

    void focusAndSetCursor();
    bool maybeHide();
    void startSearch(const QString &findText);
    // A search found in the Library: starts at the first result from the 0-based @p firstPage on
    // (the first of all when there is none), and when the words are not a phrase here, matches
    // them anywhere on a page, as the Library does. Capitals in it are matched as typed.
    void startLibrarySearch(const QString &findText, int firstPage);

Q_SIGNALS:
    void onCloseButtonPressed();

public Q_SLOTS:
    void findNext();
    void findPrev();
    void resetSearch();

private:
    void caseSensitivityChanged();
    void closeSearch();
    void searchTextChanged(const QString &text);
    void startPendingSearch();
    void searchFinished(int id, Okular::Document::SearchStatus status);
    void populateResults();
    void navigateToCurrentResult();
    QString snippetForResult(const Okular::Document::SearchResult &result) const;
    QString pageLabelForResult(const Okular::Document::SearchResult &result) const;
    bool eventFilter(QObject *target, QEvent *event) override;

    Okular::Document *m_document;
    KLineEdit *m_search;
    QLabel *m_status;
    QTreeWidget *m_results;
    QTimer *m_inputDelayTimer;
    QAction *m_caseSensitiveAct;
    QList<Okular::Document::SearchResult> m_matches;
    QString m_activeQuery;
    bool m_searchRunning = false;
    bool m_restartPending = false;
    bool m_closing = false;
    int m_firstResultPage = -1;
    bool m_matchWordsIfNoPhrase = false;
    bool m_matchCaseAsTyped = false;
};

#endif
