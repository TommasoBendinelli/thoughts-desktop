// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

/**
 * The surfaces of the reader window, shared by the toolbar, the sidebar and the page view so they
 * read as one calm surface in light and in dark.
 */
namespace Thoughts::ReaderTheme
{
inline bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

// The toolbar and the sidebar.
inline QColor chrome(const QPalette &palette)
{
    return isDark(palette) ? palette.color(QPalette::Window) : QColor(0xf6, 0xf6, 0xf6);
}

// Behind the pages, and the tab strip above them.
inline QColor canvas(const QPalette &palette)
{
    return isDark(palette) ? palette.color(QPalette::Window).darker(140) : QColor(0xe6, 0xe6, 0xe6);
}

// A wash over any surface, as a style sheet colour: 13 on hover, 26 when pressed or chosen, 30 for
// a hairline.
inline QString wash(const QPalette &palette, int alpha)
{
    return QStringLiteral("rgba(%1, %1, %1, %2)").arg(isDark(palette) ? 255 : 0).arg(alpha);
}

// Thin neutral scroll bars on @p surface, as a style sheet; @p scope limits them to a selector.
inline QString scrollBars(const QPalette &palette, const QColor &surface, const QString &scope = QString())
{
    return QStringLiteral(
               "%1QScrollBar { background: %2; border: 0; }"
               "%1QScrollBar:vertical { width: 12px; }"
               "%1QScrollBar:horizontal { height: 12px; }"
               "%1QScrollBar::handle { background: %3; border-radius: 3px; margin: 3px; }"
               "%1QScrollBar::handle:vertical { min-height: 32px; }"
               "%1QScrollBar::handle:horizontal { min-width: 32px; }"
               "%1QScrollBar::handle:hover { background: %4; }"
               "%1QScrollBar::add-line, %1QScrollBar::sub-line { width: 0; height: 0; }"
               "%1QScrollBar::add-page, %1QScrollBar::sub-page { background: transparent; }")
        .arg(scope, surface.name(), wash(palette, 70), wash(palette, 120));
}
}
