// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QColor;
class QIcon;
class QString;
class QWidget;

/**
 * What the reader window asks of the platform. On macOS the toolbar runs under the window buttons,
 * so one bar holds the title and the tools, and toolbar icons are SF Symbols; elsewhere the window
 * keeps its title bar and its icon theme.
 */
namespace Thoughts::ReaderChrome
{
bool mergesTitleBar();
// The bar as tall as Safari's where it runs under the window buttons, and its icons.
int toolBarHeight();
int toolBarIconSize();
// Keeps the window buttons centred in a bar of toolBarHeight(); AppKit puts them back at the top of
// its own title bar whenever it lays the window out.
void alignWindowButtons(QWidget *window);
// The toolbar shows the title instead. It stays the window's title for the Window menu.
void hideWindowTitle(QWidget *window);
// What double-clicking a title bar does, by the system setting.
void zoomWindow(QWidget *window);
// The symbol in @p tint, or @p fallback when the system has no such symbol.
QIcon symbolIcon(const QString &symbol, const QColor &tint, const QIcon &fallback);
}
