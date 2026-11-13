/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "agentconversations.h"

#include <QDesktopServices>
#include <QString>
#include <QUrl>

namespace Thoughts
{
// Opens an agent conversation link. A claude.ai Claude Code link opens in the Claude app
// (claude://code/<id>) and falls back to the browser when no app handles claude://;
// codex://threads/… and everything else open as they are. False when nothing opened it.
inline bool openAgentConversationUrl(const QString &url)
{
    if (url.isEmpty()) return false;
    const QUrl app = AgentConversations::claudeAppUrl(url);
    if (app.isValid() && QDesktopServices::openUrl(app)) return true;
    return QDesktopServices::openUrl(QUrl(url));
}
}
