// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <functional>

namespace Thoughts
{
class DesktopSyncOutbox;

// Agent conversations: the Claude Code and Codex conversations that worked on a
// Library Project. One synced record per (Project, conversation) (entity_type
// agent_conversation, collection agent-conversations, entity_id = link_id), last
// writer wins on revision; removal is a flag, not a tombstone. The phone reads and
// removes the same records, so everything here follows the shared format exactly
// (thoughts-server protocol/agent-conversations.md, spec sections 2 and 3).
namespace AgentConversations
{
QString entityType();
QString collection();
// Servers that accept agent_conversation; until one does, local writes stay queued.
QString capability();
// sha256_hex(utf8(project_id + "\n" + agent + ":" + conversation_id)).
QString linkId(const QString &projectId, const QString &agent, const QString &conversationId);
QString projectionKey(const QString &linkId);

// A touch writes again after this long even when nothing else changed.
constexpr qint64 throttleSeconds = 600;
constexpr int titleLimit = 200;

// agent 3 > app 2 > prompt 1 > fallback 0; -1 for anything else.
int titleRank(const QString &titleSource);
// One line: newlines and tabs become spaces, whitespace runs collapse, at most 200
// UTF-16 units (cut at a word boundary with an ellipsis when longer).
QString cleanTitle(const QString &title);
// "Claude conversation 5ef4cd0f" / "Codex conversation 01900000".
QString fallbackTitle(const QString &agent, const QString &conversationId);
bool validConversationId(const QString &conversationId);
// "" is valid; otherwise the claude.ai session link (claude) or exactly codex://threads/<id> (codex).
bool validConversationUrl(const QString &agent, const QString &conversationId, const QString &url);
// The Claude app link (claude://code/<id>) for a Claude Code conversation link
// https://claude.ai/code/<id> with <id> session_… or cse_…; an empty QUrl for anything else.
// The app's handler accepts exactly ^(cse|session)_[A-Za-z0-9_-]+$.
QUrl claudeAppUrl(const QString &url);
// The writers' timestamp format yyyy-MM-ddTHH:mm:ss.SSSZ, never at or before @p notBefore.
QString timestamp(const QDateTime &now, const QString &notBefore = QString());

// Empty when @p record is a well-formed record (spec section 2), otherwise why not.
QString validationError(const QJsonObject &record);

struct Touch {
    QString projectId;
    QString agent; // claude | codex
    QString conversationId;
    QString conversationUrl;
    QString title;
    QString titleSource; // agent | app | prompt | fallback
    QString sessionName;
    QString linkedBy; // agent | tool | backfill
    QString tool; // the MCP tool of an automatic touch
    // Also link the conversation to this review card (an entry in review_cards, card spec
    // section 1); empty for a Project-only touch.
    QString reviewCardId;
    QString cardLinkedBy; // prompt | tool | agent
};

struct TouchResult {
    QJsonObject record; // what is stored afterwards (the stored record when nothing was written)
    bool written = false;
    bool removed = false; // the Project-level removed flag of the stored record
    bool cardWritten = false; // the review card entry was added, revived or refreshed
};

// The review card entries (review_cards) of a record: at most this many, oldest dropped first.
constexpr int cardEntryLimit = 200;
// A review card id as review_cards stores it: a lowercase UUID.
bool validCardId(const QString &cardId);
// Whether @p entry is a well-formed review_cards entry (readers skip the others).
bool validCardEntry(const QJsonValue &entry);

// Spec section 3.2. @p stored is the current record (empty when there is none).
TouchResult touch(const QJsonObject &stored, const Touch &touch, const QString &originId, const QDateTime &now, const QString &deviceName);
// Spec section 3.3: the next revision of @p record, removed; unknown fields are kept.
QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());
// Card spec section 2: the next revision of @p record with its entry for @p cardId removed
// (the Project-level fields stay as they are); @p record unchanged when it has no such entry.
QJsonObject removeFromCard(const QJsonObject &record, const QString &cardId, const QString &originId,
                           const QDateTime &now = QDateTime::currentDateTimeUtc());

// Visible records (not removed, not deleted) of @p projectId among projections keyed
// "entity_type\ncollection\nentity_id", latest last_active_at first, then link_id.
QList<QJsonObject> recordsFor(const QJsonObject &projections, const QString &projectId);
// Whether a projection key belongs to an agent conversation.
bool isProjectionKey(const QString &key);

struct CardConversation {
    QJsonObject record;
    QJsonObject entry; // its review_cards entry for the card
};
// The conversations linked to review card @p cardId (card spec section 2): every non-deleted
// record, of any Project, whose entry for the card is not removed (the Project-level removed
// flag does not count), latest entry last_active_at first, then record last_active_at, then link_id.
QList<CardConversation> recordsForCard(const QJsonObject &projections, const QString &cardId);

// Whether every key that differs between @p before and @p after is an agent conversation
// (such changes refresh only the Library rows, not the whole Library).
bool onlyConversationChanges(const QJsonObject &before, const QJsonObject &after);

// Queues @p record for the server (base revision = revision - 1).
bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error = nullptr);

struct ProjectResolution {
    QJsonObject project; // empty on failure
    QString errorCode; // project_not_found | ambiguous_project | not_in_library | invalid_request
    QString message;
};
// The Project of a link-conversation request (spec section 6.6): exactly one of "project"
// (id or case-insensitive name), "source" (a path, relative to @p docsRoot or absolute
// inside it) or "document" (a Library PDF filename, or an absolute path in current_pdfs).
// @p projects is LibraryProjectStore::projects().
ProjectResolution resolveProject(const QJsonArray &projects, const QString &docsRoot, const QJsonObject &request);

// The thread name Codex shows for @p conversationId (last session_index.jsonl entry), or "".
QString codexThreadName(const QString &codexHome, const QString &conversationId);

struct BackfillLookup {
    // The proposal's conversation link: its stored link, else one this computer still finds
    // (Thoughts::proposalConversationLink).
    std::function<QString(const QString &storedUrl, const QString &conversationId)> link;
    QString codexHome; // ~/.codex or $CODEX_HOME
    QString claudeHome; // ~/.claude or $CLAUDE_CONFIG_DIR
};
// One-time backfill (spec section 6.7): a link touch (linked_by backfill) per conversation and
// Project of the current proposals in @p projections, at most one per record.
QList<Touch> proposalBackfill(const QJsonObject &projections, const QJsonArray &projects, const QString &docsRoot, const BackfillLookup &lookup);
}
}
