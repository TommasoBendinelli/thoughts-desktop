// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_COMMENTICON_H
#define THOUGHTS_COMMENTICON_H

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace Thoughts
{
// Same geometry and shadow as Android's TextCommentMarkerGeometry.centeredBubble
// and PDFView.buildCommentMarkerPath: body 80%, tail 20/27.5/47.5%, radius 4/20.
inline QPainterPath standaloneCommentPath(const QRectF &bounds)
{
    const qreal x = bounds.left(), y = bounds.top(), s = bounds.width();
    const qreal r = s * 0.2, bottom = y + s * 0.8;
    QPainterPath path;
    path.moveTo(x + r, y);
    path.lineTo(x + s - r, y);
    path.quadTo(x + s, y, x + s, y + r);
    path.lineTo(x + s, bottom - r);
    path.quadTo(x + s, bottom, x + s - r, bottom);
    path.lineTo(x + s * 0.475, bottom);
    path.lineTo(x + s * 0.275, y + s);
    path.lineTo(x + s * 0.2, bottom);
    path.quadTo(x, bottom, x, bottom - r);
    path.lineTo(x, y + r);
    path.quadTo(x, y, x + r, y);
    path.closeSubpath();
    return path;
}

inline void paintStandaloneComment(QPainter &painter, const QRectF &bounds, qreal opacity = 1.0)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(painter.opacity() * opacity);
    const QColor shadow(0, 0, 0, 48);
    painter.setPen(QPen(shadow, bounds.width() * 0.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(shadow);
    painter.drawPath(standaloneCommentPath(bounds.translated(0, bounds.width() * 0.05)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 0));
    painter.drawPath(standaloneCommentPath(bounds));
    painter.restore();
}

inline QIcon standaloneCommentIcon()
{
    static const QIcon icon = [] {
        QIcon result;
        for (const int size : {16, 20, 24, 32, 48, 64, 128}) {
            QPixmap pixmap(size, size);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            paintStandaloneComment(painter, QRectF(size * 0.08, size * 0.04, size * 0.84, size * 0.84));
            painter.end();
            result.addPixmap(pixmap);
        }
        return result;
    }();
    return icon;
}
}
#endif
