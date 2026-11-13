// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Thoughts::AgentNotes
{
// macOS: a Notification Center banner for each note AgentNotes::Center says needs attention
// (new, and not opened on the phone within the grace period); clicking it opens Agent Notes
// on that note. Asks for permission the first time a banner is due. Only in the app bundle
// (tests and non-cocoa platforms do nothing). Call once, before the event loop starts.
void installSystemNotifications();
}
