// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCursor>

namespace Okular
{

// macOS has no native busy cursor, so Qt draws one from a pixmap. Qt 6.11
// can hand CGImageCreate no colour space while converting it and crash
// (seen on 2026-09-28 and 2026-10-01 while opening a PDF). Keep the arrow on
// macOS; elsewhere use the requested busy shape.
inline QCursor busyCursor(Qt::CursorShape shape = Qt::WaitCursor)
{
#ifdef Q_OS_MACOS
    Q_UNUSED(shape)
    return QCursor(Qt::ArrowCursor);
#else
    return QCursor(shape);
#endif
}

}
