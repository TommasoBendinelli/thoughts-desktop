// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "documentfeedback.h"
#include "readertheme.h"
#include "reviewcardfeedback.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

/**
 * The mark of a comment on a passage, the same in the Annotations sidebar and the Library:
 * a plus for "like", a minus for "dislike", a question mark, three dots for a comment, a pencil
 * line for a drawing, and a T for a text box.
 */
namespace Thoughts::FeedbackIcon
{
inline QColor colour(const QString &polarity, bool dark)
{
    if (polarity == ReviewCardFeedback::neutral()) return dark ? QColor(0x90, 0xCA, 0xF9) : QColor(0x15, 0x65, 0xC0);
    const bool negative = polarity == ReviewCardFeedback::negative();
    if (dark) return negative ? QColor(0xEF, 0x9A, 0x9A) : QColor(0x81, 0xC7, 0x84);
    return negative ? QColor(0xC6, 0x28, 0x28) : QColor(0x2E, 0x7D, 0x32);
}

inline QIcon icon(const QString &kind)
{
    const bool liked = kind == QLatin1String("like");
    const bool disliked = kind == QLatin1String("dislike");
    const QString polarity = liked ? ReviewCardFeedback::positive() : disliked ? ReviewCardFeedback::negative() : ReviewCardFeedback::neutral();
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour(polarity, ReaderTheme::isDark(QGuiApplication::palette())));
    painter.drawEllipse(QRectF(1.5, 1.5, 13, 13));
    painter.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
    if (kind == DocumentFeedback::question()) {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(QRectF(1.5, 1.5, 13, 13), Qt::AlignCenter, QStringLiteral("?"));
    } else if (kind == DocumentFeedback::drawing()) {
        painter.drawLine(QPointF(5, 11), QPointF(11, 5));
    } else if (kind == DocumentFeedback::textBox()) {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(QRectF(1.5, 1.5, 13, 13), Qt::AlignCenter, QStringLiteral("T"));
    } else if (!liked && !disliked) {
        for (const double x : {5.0, 8.0, 11.0}) painter.drawPoint(QPointF(x, 8));
    } else {
        painter.drawLine(QPointF(5, 8), QPointF(11, 8));
        if (liked) painter.drawLine(QPointF(8, 5), QPointF(8, 11));
    }
    painter.end();
    return QIcon(pixmap);
}
}
