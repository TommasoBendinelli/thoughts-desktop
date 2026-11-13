// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_MACQUITEVENT_H
#define THOUGHTS_MACQUITEVENT_H

namespace Thoughts
{
// Answers the macOS "quit" Apple event (Dock Quit, scripts, log out) with what really
// happened. Qt makes every termination request return as canceled, even one it carries
// out, so AppKit told every sender "User canceled" while Thoughts went on to quit.
void installMacQuitEventHandler();
}

#endif
