/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_TEXTLINEPATH_H
#define THOUGHTS_TEXTLINEPATH_H

#include "textlinegeometry.h"
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>
#include <map>

namespace Thoughts
{
// Align rectangular quads with their text baseline before sharing the same
// line grouping as rectangles. Preserve skewed/non-rectangular regions exactly.
inline QList<QPolygonF> textLineQuads(const QList<QPolygonF> &quads, bool preserveEdge = false)
{
    struct Group { QTransform fromLine; bool bottomFirst; QList<QRectF> rects; };
    std::map<std::pair<qint64, bool>, Group> groups;
    QList<QPolygonF> result;
    for (const auto &quad : quads) {
        if (quad.size() != 4) { result.append(quad); continue; }
        const QPointF edge = quad[1] - quad[0];
        qreal angle = std::atan2(edge.y(), edge.x()) * 180.0 / std::acos(-1.0);
        if (angle < 0) angle += 180.0;
        if (angle >= 180.0) angle -= 180.0;
        QTransform fromLine;
        fromLine.rotate(angle);
        const auto aligned = fromLine.inverted().map(quad);
        const QRectF bounds = aligned.boundingRect();
        const qreal tolerance = qMax(bounds.width(), bounds.height()) * 1e-6;
        if (!bounds.isValid() || qAbs(aligned[0].y() - aligned[1].y()) > tolerance
            || qAbs(aligned[2].y() - aligned[3].y()) > tolerance
            || qAbs(aligned[0].x() - aligned[3].x()) > tolerance
            || qAbs(aligned[1].x() - aligned[2].x()) > tolerance) {
            result.append(quad);
            continue;
        }
        const bool bottomFirst = preserveEdge && aligned[0].y() > aligned[3].y();
        const auto key = std::make_pair(qRound64(angle * 1e5), bottomFirst);
        auto [it, inserted] = groups.try_emplace(key, Group{fromLine, bottomFirst, {}});
        // Use the group's transform so almost-equal angles share one coordinate frame.
        it->second.rects.append(it->second.fromLine.inverted().map(quad).boundingRect());
    }
    for (auto &[key, group] : groups) {
        for (const auto &rect : mergeTextLineRects(std::move(group.rects))) {
            const qreal firstY = group.bottomFirst ? rect.bottom() : rect.top();
            const qreal lastY = group.bottomFirst ? rect.top() : rect.bottom();
            result.append(group.fromLine.map(QPolygonF{{rect.left(), firstY}, {rect.right(), firstY}, {rect.right(), lastY}, {rect.left(), lastY}}));
        }
    }
    return result;
}

inline QPainterPath textLinePath(const QList<QPolygonF> &quads)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (auto polygon : textLineQuads(quads)) {
        double area = 0;
        for (int i = 0; i < polygon.size(); ++i) {
            const auto &a = polygon[i];
            const auto &b = polygon[(i + 1) % polygon.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        if (area < 0) std::reverse(polygon.begin(), polygon.end());
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    return path.simplified();
}
}
#endif
