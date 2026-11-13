/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_TEXTLINEGEOMETRY_H
#define THOUGHTS_TEXTLINEGEOMETRY_H

#include <QList>
#include <QRectF>
#include <QSizeF>
#include <algorithm>
#include <cmath>

namespace Thoughts
{
// Display geometry only: callers retain the original word regions for source
// identity, copying, hit testing, and persistence. Call once per semantic group.
// Coordinates must have the same physical scale on both axes (points or pixels).
inline QList<QRectF> mergeTextLineRects(QList<QRectF> rects, Qt::Orientation direction = Qt::Horizontal)
{
    const auto transpose = [](const QRectF &r) { return QRectF(r.y(), r.x(), r.height(), r.width()); };
    rects.removeIf([](const QRectF &r) {
        return !r.isValid() || !std::isfinite(r.left()) || !std::isfinite(r.top()) || !std::isfinite(r.right()) || !std::isfinite(r.bottom());
    });
    if (direction == Qt::Vertical) {
        for (auto &rect : rects) rect = transpose(rect);
    }
    std::sort(rects.begin(), rects.end(), [](const QRectF &a, const QRectF &b) {
        if (a.center().y() != b.center().y()) return a.center().y() < b.center().y();
        return a.left() < b.left();
    });
    QList<QRectF> merged;
    qsizetype first = 0;
    while (first < rects.size()) {
        qsizetype end = first + 1;
        while (end < rects.size()
               && qMax(rects[first].height(), rects[end].height()) <= 3 * qMin(rects[first].height(), rects[end].height())
               && qAbs(rects[first].center().y() - rects[end].center().y()) <= qMin(rects[first].height(), rects[end].height()) * 0.65) {
            ++end;
        }
        QList<QRectF> line(rects.cbegin() + first, rects.cbegin() + end);
        std::sort(line.begin(), line.end(), [](const QRectF &a, const QRectF &b) { return a.left() < b.left(); });
        QRectF run = line.constFirst();
        qreal lastHeight = run.height();
        for (qsizetype index = 1; index < line.size(); ++index) {
            const QRectF &fragment = line[index];
            // A normal space is shorter than a line is tall. Use original
            // fragment heights so growing bounds cannot bridge a distant region.
            if (fragment.left() - run.right() <= qMax(lastHeight, fragment.height())) {
                run = run.united(fragment);
            } else {
                merged.append(run);
                run = fragment;
            }
            lastHeight = fragment.height();
        }
        merged.append(run);
        first = end;
    }
    if (direction == Qt::Vertical) {
        for (auto &rect : merged) rect = transpose(rect);
    }
    return merged;
}

inline QList<QRectF> normalizedTextLineRects(QList<QRectF> rects, const QSizeF &pageSize, Qt::Orientation direction = Qt::Horizontal)
{
    if (pageSize.isEmpty()) return rects;
    for (auto &r : rects) r = QRectF(r.x() * pageSize.width(), r.y() * pageSize.height(), r.width() * pageSize.width(), r.height() * pageSize.height());
    rects = mergeTextLineRects(std::move(rects), direction);
    for (auto &r : rects) r = QRectF(r.x() / pageSize.width(), r.y() / pageSize.height(), r.width() / pageSize.width(), r.height() / pageSize.height());
    return rects;
}
}
#endif
