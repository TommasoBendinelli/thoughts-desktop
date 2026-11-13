// SPDX-License-Identifier: GPL-2.0-or-later
#include "readerchrome.h"

#include <QColor>
#include <QIcon>
#include <QString>
#include <QWidget>

namespace Thoughts::ReaderChrome
{
bool mergesTitleBar()
{
    return false;
}

int toolBarHeight()
{
    return 0;
}

int toolBarIconSize()
{
    return 18;
}

void alignWindowButtons(QWidget *)
{
}

void hideWindowTitle(QWidget *)
{
}

void zoomWindow(QWidget *)
{
}

QIcon symbolIcon(const QString &, const QColor &, const QIcon &fallback)
{
    return fallback;
}
}
