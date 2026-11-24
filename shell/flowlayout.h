// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QWidget>

#include <algorithm>

namespace Thoughts
{

// Lays its widgets out left to right at their natural width, wrapping onto the next line (Qt's
// flow layout); a widget wider than the line is given the line's width.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int spacing = 6)
        : QLayout(parent)
        , m_spacing(spacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) delete item;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        const int height = arrange(rect, false);
        // A window sizes itself from minimum sizes, which know nothing of wrapping (one line here),
        // so a dialog could squeeze the wrapped lines under the widgets below them. Ask for them.
        QWidget *owner = parentWidget();
        if (owner && height > 0 && owner->minimumHeight() != height) owner->setMinimumHeight(height);
    }
    QSize sizeHint() const override
    {
        // As wide as the widest widget: a line fits whatever width the parent gives.
        return minimumSize();
    }
    QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem *item : m_items) size = size.expandedTo(item->minimumSize());
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }

private:
    int arrange(const QRect &rect, bool measureOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = area.x(), y = area.y(), lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            if (item->isEmpty()) continue;
            const QSize hint = item->sizeHint();
            const int width = std::max(1, std::min(hint.width(), area.width()));
            if (x > area.x() && x + width > area.right() + 1) {
                x = area.x();
                y += lineHeight + m_spacing;
                lineHeight = 0;
            }
            if (!measureOnly) item->setGeometry(QRect(QPoint(x, y), QSize(width, hint.height())));
            x += width + m_spacing;
            lineHeight = std::max(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_spacing = 6;
};

}
