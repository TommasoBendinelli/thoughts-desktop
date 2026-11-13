// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "agentprompts.h"
#include <KConfigGroup>
#include <KSharedConfig>

// The Copy for agent template as Settings > Agent prompts saves it.
namespace Thoughts::AgentPrompts
{
// Settings, in this app's config file. A missing or blank entry means the default.
inline QString configGroup() { return QStringLiteral("AgentPrompts"); }
inline QString configKey(Kind)
{
    return QStringLiteral("CopyForAgentTemplate");
}

namespace detail
{
// Earlier Copy for agent defaults. A saved copy of one is the user's default, not an edit, so it
// follows the current default. Up to 2026-10-26 the default had no {current_review} line.
inline QString defaultAgentTemplateBeforeCurrentReview()
{
    QString text = defaultTemplate(Kind::Agent);
    text.remove(QStringLiteral("{current_review}\n"));
    return text;
}
// Up to 2026-10-22 it also asked the agent not to reveal the answer.
inline QString previousDefaultAgentTemplate()
{
    return defaultAgentTemplateBeforeCurrentReview()
        + QStringLiteral("I'm studying this card. Don't reveal, quote or hint at the answer until I have given my own answer or ask for it; "
                         "the answer pages and source lines above are for looking it up then.\n");
}
inline bool isDefault(Kind kind, const QString &text)
{
    return text.trimmed().isEmpty() || text == defaultTemplate(kind)
        || (kind == Kind::Agent && (text == previousDefaultAgentTemplate() || text == defaultAgentTemplateBeforeCurrentReview()));
}
}

inline QString savedTemplate(Kind kind)
{
    const QString saved = KConfigGroup(KSharedConfig::openConfig(), configGroup()).readEntry(configKey(kind), QString());
    return detail::isDefault(kind, saved) ? defaultTemplate(kind) : saved;
}
// Saving the default (or nothing, or an earlier default) removes the entry, so a later default applies.
inline void saveTemplate(Kind kind, const QString &text)
{
    KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    if (detail::isDefault(kind, text)) group.deleteEntry(configKey(kind));
    else group.writeEntry(configKey(kind), text);
    group.sync();
}
}
