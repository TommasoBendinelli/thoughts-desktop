/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "findbar.h"

#include <KLineEdit>
#include <KLocalizedString>
#include <QAction>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>

#include "core/page.h"
#include "settings.h"

namespace
{
enum ResultRole {
    ResultIndexRole = Qt::UserRole + 1,
};

constexpr int SearchInputDelayMs = 250;
constexpr QRgb ResultColor = qRgb(255, 255, 64);
constexpr int MaximumSnippetLength = 120;
}

FindBar::FindBar(Okular::Document *document, QWidget *parent)
    : QWidget(parent)
    , m_document(document)
{
    setObjectName(QStringLiteral("documentSearchSidebar"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *searchLayout = new QHBoxLayout;
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(3);

    auto *closeButton = new QToolButton(this);
    closeButton->setObjectName(QStringLiteral("documentSearchClose"));
    closeButton->setIcon(QIcon::fromTheme(QStringLiteral("dialog-close")));
    closeButton->setToolTip(i18n("Close Search"));
    closeButton->setAutoRaise(true);
    searchLayout->addWidget(closeButton);

    m_search = new KLineEdit(this);
    m_search->setObjectName(QStringLiteral("documentSearchField"));
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(i18n("Search document…"));
    m_search->setToolTip(i18n("Search every occurrence in this document"));
    m_search->setAccessibleName(i18n("Search document"));
    m_search->installEventFilter(this);
    searchLayout->addWidget(m_search, 1);

    auto *optionsButton = new QToolButton(this);
    optionsButton->setObjectName(QStringLiteral("documentSearchOptions"));
    optionsButton->setAutoRaise(true);
    optionsButton->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    optionsButton->setToolTip(i18n("Search Options"));
    optionsButton->setPopupMode(QToolButton::InstantPopup);
    auto *optionsMenu = new QMenu(optionsButton);
    m_caseSensitiveAct = optionsMenu->addAction(i18n("Case sensitive"));
    m_caseSensitiveAct->setCheckable(true);
    m_caseSensitiveAct->setChecked(Okular::Settings::searchCaseSensitive());
    optionsButton->setMenu(optionsMenu);
    searchLayout->addWidget(optionsButton);

    layout->addLayout(searchLayout);

    m_status = new QLabel(i18n("Type to search"), this);
    m_status->setObjectName(QStringLiteral("documentSearchStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_results = new QTreeWidget(this);
    m_results->setObjectName(QStringLiteral("documentSearchResults"));
    m_results->setColumnCount(2);
    m_results->setHeaderLabels({i18n("Result"), i18n("Page")});
    m_results->setRootIsDecorated(false);
    m_results->setAlternatingRowColors(true);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_results, 1);

    m_inputDelayTimer = new QTimer(this);
    m_inputDelayTimer->setSingleShot(true);

    connect(closeButton, &QAbstractButton::clicked, this, &FindBar::closeSearch);
    connect(m_search, &KLineEdit::textChanged, this, &FindBar::searchTextChanged);
    connect(m_caseSensitiveAct, &QAction::toggled, this, &FindBar::caseSensitivityChanged);
    connect(m_inputDelayTimer, &QTimer::timeout, this, &FindBar::startPendingSearch);
    connect(m_results, &QTreeWidget::currentItemChanged, this, [this] { navigateToCurrentResult(); });
    connect(m_document, &Okular::Document::searchFinished, this, &FindBar::searchFinished);
}

FindBar::~FindBar() = default;

bool FindBar::eventFilter(QObject *target, QEvent *event)
{
    if (target == m_search && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            closeSearch();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Down) {
            if (keyEvent->modifiers() & Qt::ShiftModifier) {
                findPrev();
            } else {
                findNext();
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Up) {
            findPrev();
            return true;
        }
    }
    return QWidget::eventFilter(target, event);
}

QString FindBar::text() const
{
    return m_search->text();
}

Qt::CaseSensitivity FindBar::caseSensitivity() const
{
    return m_caseSensitiveAct->isChecked() || m_matchCaseAsTyped ? Qt::CaseSensitive : Qt::CaseInsensitive;
}

void FindBar::focusAndSetCursor()
{
    m_search->selectAll();
    m_search->setFocus(Qt::ShortcutFocusReason);
}

bool FindBar::maybeHide()
{
    resetSearch();
    return true;
}

void FindBar::findNext()
{
    const int count = m_results->topLevelItemCount();
    if (count == 0) {
        return;
    }

    const int current = m_results->indexOfTopLevelItem(m_results->currentItem());
    const int next = current < 0 ? 0 : (current + 1) % count;
    m_results->setCurrentItem(m_results->topLevelItem(next));
    m_results->scrollToItem(m_results->currentItem());
}

void FindBar::findPrev()
{
    const int count = m_results->topLevelItemCount();
    if (count == 0) {
        return;
    }

    const int current = m_results->indexOfTopLevelItem(m_results->currentItem());
    const int previous = current < 0 ? count - 1 : (current + count - 1) % count;
    m_results->setCurrentItem(m_results->topLevelItem(previous));
    m_results->scrollToItem(m_results->currentItem());
}

void FindBar::resetSearch()
{
    m_inputDelayTimer->stop();
    m_restartPending = false;
    m_firstResultPage = -1;
    m_matchWordsIfNoPhrase = false;
    m_matchCaseAsTyped = false;
    m_closing = m_searchRunning;
    if (m_searchRunning) {
        m_document->cancelSearch();
    } else {
        m_document->resetSearch(PART_SEARCH_ID);
    }

    m_matches.clear();
    m_results->clear();
    m_activeQuery.clear();
    m_status->setText(i18n("Type to search"));
    const QSignalBlocker blocker(m_search);
    m_search->clear();
}

void FindBar::caseSensitivityChanged()
{
    Okular::Settings::setSearchCaseSensitive(m_caseSensitiveAct->isChecked());
    Okular::Settings::self()->save();
    if (!m_search->text().isEmpty()) {
        m_inputDelayTimer->start(0);
    }
}

void FindBar::closeSearch()
{
    Q_EMIT onCloseButtonPressed();
}

void FindBar::searchTextChanged(const QString &text)
{
    m_inputDelayTimer->stop();
    m_firstResultPage = -1;
    m_matchWordsIfNoPhrase = false;
    m_matchCaseAsTyped = false;
    m_matches.clear();
    m_results->clear();

    if (text.isEmpty()) {
        Q_EMIT onCloseButtonPressed();
        return;
    }

    // A search can be reopened while cancellation from its previous close is
    // still being delivered.  The new query must win over that stale close.
    m_closing = false;
    m_status->setText(i18n("Searching…"));
    m_inputDelayTimer->start(SearchInputDelayMs);
}

void FindBar::startPendingSearch()
{
    const QString query = m_search->text();
    if (query.isEmpty()) {
        return;
    }

    if (m_searchRunning) {
        m_restartPending = true;
        m_document->cancelSearch();
        return;
    }

    m_document->resetSearch(PART_SEARCH_ID);
    m_restartPending = false;
    m_closing = false;
    m_activeQuery = query;
    m_searchRunning = true;
    m_status->setText(i18n("Searching…"));
    m_document->searchText(PART_SEARCH_ID, query, true, caseSensitivity(), Okular::Document::AllDocument, false, ResultColor);
}

void FindBar::searchFinished(int id, Okular::Document::SearchStatus status)
{
    if (id != PART_SEARCH_ID) {
        return;
    }

    m_searchRunning = false;
    if (m_closing) {
        m_document->resetSearch(PART_SEARCH_ID);
        m_closing = false;
        return;
    }

    const bool queryChanged = m_search->text() != m_activeQuery;
    const bool shouldRestart = m_restartPending || queryChanged;
    if (status == Okular::Document::SearchCancelled || shouldRestart) {
        m_document->resetSearch(PART_SEARCH_ID);
        m_restartPending = false;
        if (shouldRestart && !m_search->text().isEmpty() && !m_inputDelayTimer->isActive()) {
            m_inputDelayTimer->start(0);
        } else if (!shouldRestart) {
            m_status->setText(i18n("Search cancelled"));
        }
        return;
    }

    if (status == Okular::Document::MatchFound) {
        populateResults();
    } else if (m_matchWordsIfNoPhrase) {
        m_matchWordsIfNoPhrase = false;
        m_matchCaseAsTyped = false;
        m_document->resetSearch(PART_SEARCH_ID);
        m_searchRunning = true;
        m_document->searchText(PART_SEARCH_ID, m_activeQuery, true, caseSensitivity(), Okular::Document::GoogleAll, false, ResultColor);
    } else {
        m_matches.clear();
        m_results->clear();
        m_status->setText(i18n("No results"));
    }
}

void FindBar::populateResults()
{
    m_matches = m_document->searchResults(PART_SEARCH_ID);
    m_results->clear();

    for (int index = 0; index < m_matches.size(); ++index) {
        const auto &result = m_matches.at(index);
        auto *item = new QTreeWidgetItem(m_results, {snippetForResult(result), pageLabelForResult(result)});
        item->setData(0, ResultIndexRole, index);
        item->setToolTip(0, item->text(0));
    }

    const int count = m_results->topLevelItemCount();
    if (count == 0) {
        m_status->setText(i18n("No results"));
        return;
    }

    int first = 0;
    if (m_firstResultPage >= 0) {
        const auto match = std::find_if(m_matches.cbegin(), m_matches.cend(), [this](const auto &result) { return result.pageNumber >= m_firstResultPage; });
        first = match == m_matches.cend() ? 0 : int(std::distance(m_matches.cbegin(), match));
        m_firstResultPage = -1;
    }
    m_status->setText(i18np("%1 result", "%1 results", count));
    m_results->setCurrentItem(m_results->topLevelItem(first));
    m_results->scrollToItem(m_results->currentItem());
}

void FindBar::navigateToCurrentResult()
{
    const QTreeWidgetItem *item = m_results->currentItem();
    if (!item) {
        return;
    }

    const int index = item->data(0, ResultIndexRole).toInt();
    if (index < 0 || index >= m_matches.size()) {
        return;
    }

    const auto &result = m_matches.at(index);
    if (result.pageNumber < 0 || result.pageNumber >= static_cast<int>(m_document->pages()) || result.area.isEmpty()) {
        return;
    }

    const Okular::NormalizedRect &rect = result.area.constFirst();
    Okular::DocumentViewport viewport(result.pageNumber);
    viewport.rePos.enabled = true;
    viewport.rePos.pos = Okular::DocumentViewport::Center;
    viewport.rePos.normalizedX = (rect.left + rect.right) / 2.0;
    viewport.rePos.normalizedY = (rect.top + rect.bottom) / 2.0;
    m_document->setViewport(viewport, nullptr, true);
}

QString FindBar::snippetForResult(const Okular::Document::SearchResult &result) const
{
    if (result.pageNumber < 0 || result.pageNumber >= static_cast<int>(m_document->pages()) || result.area.isEmpty()) {
        return m_activeQuery.simplified();
    }

    const Okular::Page *page = m_document->page(result.pageNumber);
    Okular::NormalizedRect bounds = result.area.constFirst();
    for (const Okular::NormalizedRect &rect : result.area) {
        bounds |= rect;
    }

    // Only the match's own line: a taller area picks up parts of the lines around it, in the middle
    // of their words.
    const Okular::NormalizedRect context(qMax(0.0, bounds.left - 0.2), bounds.top, qMin(1.0, bounds.right + 0.2), bounds.bottom);
    Okular::RegularAreaRect contextArea;
    contextArea.append(context);
    QString snippet = page->text(&contextArea, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour).simplified();
    // The area starts and ends inside words; drop those pieces unless they hold the match.
    const auto withoutCutWord = [this](const QString &text, bool atStart) {
        const qsizetype space = atStart ? text.indexOf(QLatin1Char(' ')) : text.lastIndexOf(QLatin1Char(' '));
        const QString trimmed = space < 0 ? text : atStart ? text.mid(space + 1) : text.left(space);
        return trimmed.contains(m_activeQuery.simplified(), caseSensitivity()) ? trimmed : text;
    };
    if (context.left > 0.0) snippet = withoutCutWord(snippet, true);
    if (context.right < 1.0) snippet = withoutCutWord(snippet, false);
    if (snippet.isEmpty()) {
        snippet = m_activeQuery.simplified();
    }

    if (snippet.size() <= MaximumSnippetLength) {
        return snippet;
    }

    const QString simplifiedQuery = m_activeQuery.simplified();
    const int matchPosition = snippet.indexOf(simplifiedQuery, caseSensitivity());
    const int preferredStart = matchPosition < 0 ? 0 : matchPosition - MaximumSnippetLength / 3;
    const int start = qBound(0, preferredStart, snippet.size() - MaximumSnippetLength);
    QString shortened = snippet.mid(start, MaximumSnippetLength);
    if (start > 0) {
        shortened.prepend(QChar(0x2026));
    }
    if (start + MaximumSnippetLength < snippet.size()) {
        shortened.append(QChar(0x2026));
    }
    return shortened;
}

QString FindBar::pageLabelForResult(const Okular::Document::SearchResult &result) const
{
    const QString pageNumber = QString::number(result.pageNumber + 1);
    if (result.pageNumber < 0 || result.pageNumber >= static_cast<int>(m_document->pages())) {
        return pageNumber;
    }

    const QString label = m_document->page(result.pageNumber)->label().trimmed();
    if (label.isEmpty() || label == pageNumber) {
        return pageNumber;
    }
    return i18nc("PDF page label followed by physical page number", "%1 (%2)", label, pageNumber);
}

void FindBar::startLibrarySearch(const QString &findText, int firstPage)
{
    startSearch(findText);
    m_firstResultPage = firstPage;
    m_matchWordsIfNoPhrase = true;
    m_matchCaseAsTyped = findText != findText.toLower();
}

void FindBar::startSearch(const QString &findText)
{
    if (m_search->text() == findText) {
        if (!findText.isEmpty()) {
            m_inputDelayTimer->start(0);
        }
    } else {
        m_search->setText(findText);
    }
}

#include "moc_findbar.cpp"
