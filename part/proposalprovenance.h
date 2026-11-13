/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_PROPOSALPROVENANCE_H
#define THOUGHTS_PROPOSALPROVENANCE_H

#include <QRegularExpression>
#include <QString>

#include "../shell/agentproposalparser.h"

namespace Thoughts
{
// The conversation comes from the proposal's metadata only; the legacy
// "Prepared in Codex conversation" sentence was retired on 2026-10-10 (agent-proposals spec §3.3).
inline QString proposalConversationId(const QString &structured)
{
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$"));
    return uuid.match(structured).hasMatch() ? structured.toLower() : QString();
}

// The link that opens the conversation that wrote a proposal: the one stored with
// it (spec §3.6, every proposal since 2026-10-18), else none. Older proposals
// were once matched to Codex rollout files and Claude session records on this
// computer; that search walked tens of thousands of files and was dropped.
inline QString proposalConversationLink(const QString &storedUrl, const QString &conversationId)
{
    Q_UNUSED(conversationId);
    return isValidAgentProposalConversationUrl(storedUrl) ? storedUrl : QString();
}
}
#endif
