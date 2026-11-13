/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QScrollBar>
#include <QTimer>
#include <QTreeView>

// Revealing a row must not shift its leading text sideways to expose the
// indentation or the far edge of a wide first column.
template<typename Tree>
class RowBrowserTree : public Tree
{
public:
    using Tree::Tree;

    void scrollTo(const QModelIndex &index, QAbstractItemView::ScrollHint hint = QAbstractItemView::EnsureVisible) override
    {
        const int horizontalPosition = this->horizontalScrollBar()->value();
        Tree::scrollTo(index, hint);
        if (index.column() == 0) {
            this->horizontalScrollBar()->setValue(horizontalPosition);
        }
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        Tree::showEvent(event);
        if (m_shown) {
            return;
        }
        m_shown = true;
        this->horizontalScrollBar()->setValue(this->horizontalScrollBar()->minimum());
        // Header restoration and queued focus/selection work can run after the
        // first show, especially when a tab was populated while hidden.
        QTimer::singleShot(0, this, [this] {
            this->horizontalScrollBar()->setValue(this->horizontalScrollBar()->minimum());
        });
    }

private:
    bool m_shown = false;
};
