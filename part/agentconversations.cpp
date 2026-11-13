// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentconversations.h"

#include "desktopsyncstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QFileInfo>
#include <QJsonValue>
#include <QRegularExpression>
#include <algorithm>

namespace Thoughts::AgentConversations
{
namespace
{
const QString agentKey = QStringLiteral("agent");
const QString titleKey = QStringLiteral("title");
const QString titleSourceKey = QStringLiteral("title_source");
const QString urlKey = QStringLiteral("conversation_url");
const QString sessionKey = QStringLiteral("session_name");
const QString removedKey = QStringLiteral("removed");
const QString removedAtKey = QStringLiteral("removed_at");
const QString lastActiveKey = QStringLiteral("last_active_at");
const QString updatedKey = QStringLiteral("updated_at");
const QString revisionKey = QStringLiteral("revision");

bool hasControlCharacter(const QString &value)
{
    return std::any_of(value.cbegin(), value.cend(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}

// RFC 3339 with a time zone; fractions beyond milliseconds are ignored.
QDateTime parsedTime(const QString &value)
{
    static const QRegularExpression format(QStringLiteral("^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2})(?:\\.(\\d{1,9}))?(Z|[+-]\\d{2}:\\d{2})$"));
    const auto match = format.match(value);
    if (!match.hasMatch()) return {};
    const QString fraction = QString(match.captured(2) + QStringLiteral("000")).left(3);
    const QString zone = match.captured(3) == QLatin1String("Z") ? QStringLiteral("Z") : match.captured(3);
    return QDateTime::fromString(match.captured(1) + QLatin1Char('.') + fraction + zone, Qt::ISODateWithMs);
}

QString formatted(const QDateTime &time)
{
    return time.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) + QLatin1Char('Z');
}

QString singleLine(const QString &value, int limit)
{
    QString text = value;
    for (QChar &c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) c = QLatin1Char(' ');
    }
    text = text.simplified();
    if (text.size() > limit) {
        text.truncate(limit);
        if (text.at(text.size() - 1).isHighSurrogate()) text.chop(1);
    }
    return text;
}

bool isOneOf(const QString &value, std::initializer_list<const char *> allowed)
{
    return std::any_of(allowed.begin(), allowed.end(), [&value](const char *candidate) { return value == QLatin1String(candidate); });
}

const QString cardsKey = QStringLiteral("review_cards");
const QString cardIdKey = QStringLiteral("card_id");

// The index of the (first well-formed) entry for @p cardId, or -1.
int cardEntryIndex(const QJsonArray &cards, const QString &cardId);

struct CardUpdate {
    QJsonArray cards; // the entries with this touch applied
    bool touched = false; // the card's entry is in @p cards as this touch leaves it (not a removed one)
    bool due = false; // the entry is new, revived or its last_active_at is >= 600 s old: write
};
CardUpdate applyCardTouch(const QJsonObject &stored, const QString &cardId, const QString &linkedBy, const QDateTime &now);
}

QString entityType()
{
    return QStringLiteral("agent_conversation");
}

QString collection()
{
    return QStringLiteral("agent-conversations");
}

QString capability()
{
    return QStringLiteral("agent_conversations_v1");
}

QString linkId(const QString &projectId, const QString &agent, const QString &conversationId)
{
    const QString identity = projectId + QLatin1Char('\n') + agent + QLatin1Char(':') + conversationId;
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString projectionKey(const QString &linkId)
{
    return entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n') + linkId;
}

bool isProjectionKey(const QString &key)
{
    return key.startsWith(entityType() + QLatin1Char('\n'));
}

bool validCardId(const QString &cardId)
{
    return validConversationId(cardId);
}

bool validCardEntry(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject entry = value.toObject();
    if (!validCardId(entry.value(cardIdKey).toString()) || !parsedTime(entry.value(lastActiveKey).toString()).isValid()) return false;
    return !entry.contains(removedKey) || entry.value(removedKey).isBool();
}

namespace
{
int cardEntryIndex(const QJsonArray &cards, const QString &cardId)
{
    for (qsizetype i = 0; i < cards.size(); ++i) {
        if (validCardEntry(cards.at(i)) && cards.at(i).toObject().value(cardIdKey).toString() == cardId) return int(i);
    }
    return -1;
}

CardUpdate applyCardTouch(const QJsonObject &stored, const QString &cardId, const QString &linkedBy, const QDateTime &now)
{
    CardUpdate update;
    update.cards = stored.value(cardsKey).toArray();
    const QString nowText = formatted(now);
    const int index = cardEntryIndex(update.cards, cardId);
    if (index < 0) {
        update.cards.append(QJsonObject{{cardIdKey, cardId},
                                        {QStringLiteral("linked_at"), nowText},
                                        {lastActiveKey, nowText},
                                        {QStringLiteral("linked_by"), isOneOf(linkedBy, {"prompt", "tool", "agent"}) ? linkedBy : QStringLiteral("tool")},
                                        {removedKey, false}});
        // Full: drop the entry least worth keeping, never the new one: malformed ones first,
        // then removed ones, then the one active longest ago.
        while (update.cards.size() > cardEntryLimit) {
            int drop = -1;
            int dropRank = 3;
            QDateTime dropTime;
            for (int i = 0; i < update.cards.size() - 1; ++i) {
                const QJsonValue candidate = update.cards.at(i);
                const int rank = !validCardEntry(candidate) ? 0 : candidate.toObject().value(removedKey).toBool() ? 1 : 2;
                const QDateTime time = rank ? parsedTime(candidate.toObject().value(lastActiveKey).toString()) : QDateTime();
                if (drop < 0 || rank < dropRank || (rank == dropRank && rank && time < dropTime)) {
                    drop = i;
                    dropRank = rank;
                    dropTime = time;
                }
            }
            update.cards.removeAt(drop);
        }
        update.touched = update.due = true;
        return update;
    }
    QJsonObject entry = update.cards.at(index).toObject();
    const bool wasRemoved = entry.value(removedKey).toBool();
    // Removed from the card by the user: sticky, except against an explicit link.
    if (wasRemoved && linkedBy != QLatin1String("agent")) return update;
    const QDateTime active = parsedTime(entry.value(lastActiveKey).toString());
    update.due = wasRemoved || active.msecsTo(now) >= throttleSeconds * 1000;
    if (wasRemoved) entry.insert(removedKey, false);
    if (active < now) entry.insert(lastActiveKey, nowText);
    update.cards.replace(index, entry);
    update.touched = true;
    return update;
}
}

int titleRank(const QString &titleSource)
{
    if (titleSource == QLatin1String("agent")) return 3;
    if (titleSource == QLatin1String("app")) return 2;
    if (titleSource == QLatin1String("prompt")) return 1;
    if (titleSource == QLatin1String("fallback")) return 0;
    return -1;
}

QString cleanTitle(const QString &title)
{
    QString text = singleLine(title, title.size());
    if (text.size() <= titleLimit) return text;
    // Cut at a word boundary and mark the cut.
    QString cut = text.left(titleLimit - 1);
    if (cut.at(cut.size() - 1).isHighSurrogate()) cut.chop(1);
    const int space = cut.lastIndexOf(QLatin1Char(' '));
    if (space > titleLimit / 2) cut.truncate(space);
    return cut.trimmed() + QChar(0x2026);
}

QString fallbackTitle(const QString &agent, const QString &conversationId)
{
    return (agent == QLatin1String("codex") ? QStringLiteral("Codex conversation ") : QStringLiteral("Claude conversation ")) + conversationId.left(8);
}

bool validConversationId(const QString &conversationId)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$"));
    return pattern.match(conversationId).hasMatch();
}

bool validConversationUrl(const QString &agent, const QString &conversationId, const QString &url)
{
    if (url.isEmpty()) return true;
    if (agent == QLatin1String("claude")) {
        static const QRegularExpression claude(QStringLiteral("^https://claude\\.ai/code/session_[A-Za-z0-9]{1,64}$"));
        return claude.match(url).hasMatch();
    }
    return agent == QLatin1String("codex") && url == QStringLiteral("codex://threads/") + conversationId;
}

QUrl claudeAppUrl(const QString &url)
{
    static const QRegularExpression claudeCode(QStringLiteral("^https://claude\\.ai/code/((?:session|cse)_[A-Za-z0-9_-]+)$"));
    const QRegularExpressionMatch match = claudeCode.match(url);
    return match.hasMatch() ? QUrl(QStringLiteral("claude://code/") + match.captured(1)) : QUrl();
}

QString timestamp(const QDateTime &now, const QString &notBefore)
{
    QDateTime at = now.toUTC();
    const QDateTime previous = parsedTime(notBefore);
    if (previous.isValid() && previous.toUTC() >= at) at = previous.toUTC().addMSecs(1);
    return formatted(at);
}

QString validationError(const QJsonObject &record)
{
    const QJsonValue schema = record.value(QStringLiteral("schema_version"));
    if (!schema.isDouble() || schema.toDouble() != 1) return QStringLiteral("The agent conversation has no valid schema_version.");
    const QString projectId = record.value(QStringLiteral("project_id")).toString();
    if (projectId.isEmpty() || projectId.size() > 512 || hasControlCharacter(projectId)) return QStringLiteral("The agent conversation has no valid project_id.");
    const QString agent = record.value(agentKey).toString();
    if (!isOneOf(agent, {"claude", "codex"})) return QStringLiteral("The agent conversation has no valid agent.");
    const QString conversationId = record.value(QStringLiteral("conversation_id")).toString();
    if (!validConversationId(conversationId)) return QStringLiteral("The agent conversation has no valid conversation_id.");
    if (record.value(QStringLiteral("link_id")).toString() != linkId(projectId, agent, conversationId))
        return QStringLiteral("The agent conversation's link_id does not match its Project and conversation.");
    if (!record.value(urlKey).isString() || !validConversationUrl(agent, conversationId, record.value(urlKey).toString()))
        return QStringLiteral("The agent conversation has no valid conversation_url.");
    const QString title = record.value(titleKey).toString();
    if (!record.value(titleKey).isString() || title.trimmed().isEmpty() || title.trimmed().size() > titleLimit || hasControlCharacter(title))
        return QStringLiteral("The agent conversation has no valid title.");
    if (titleRank(record.value(titleSourceKey).toString()) < 0) return QStringLiteral("The agent conversation has no valid title_source.");
    if (!record.value(sessionKey).isString() || record.value(sessionKey).toString().size() > 100)
        return QStringLiteral("The agent conversation has no valid session_name.");
    if (!isOneOf(record.value(QStringLiteral("linked_by")).toString(), {"agent", "tool", "backfill"}))
        return QStringLiteral("The agent conversation has no valid linked_by.");
    static const QRegularExpression toolPattern(QStringLiteral("^[a-z_]{0,64}$"));
    if (!record.value(QStringLiteral("last_tool")).isString() || !toolPattern.match(record.value(QStringLiteral("last_tool")).toString()).hasMatch())
        return QStringLiteral("The agent conversation has no valid last_tool.");
    if (!record.value(QStringLiteral("device_name")).isString() || record.value(QStringLiteral("device_name")).toString().size() > 128)
        return QStringLiteral("The agent conversation has no valid device_name.");
    for (const auto &field : {QStringLiteral("created_at"), lastActiveKey, updatedKey}) {
        if (!parsedTime(record.value(field).toString()).isValid()) return QStringLiteral("The agent conversation has no valid %1.").arg(field);
    }
    const QJsonValue revision = record.value(revisionKey);
    if (!revision.isDouble() || revision.toDouble() < 1 || revision.toDouble() != double(revision.toInteger()))
        return QStringLiteral("The agent conversation has no valid revision.");
    const QString origin = record.value(QStringLiteral("origin_id")).toString();
    if (origin.isEmpty() || origin.size() > 128) return QStringLiteral("The agent conversation has no valid origin_id.");
    if (!record.value(removedKey).isBool()) return QStringLiteral("The agent conversation has no valid removed flag.");
    const QJsonValue removedAt = record.value(removedAtKey);
    if (record.value(removedKey).toBool()) {
        if (!parsedTime(removedAt.toString()).isValid()) return QStringLiteral("A removed agent conversation needs removed_at.");
    } else if (!removedAt.isNull() && !removedAt.isUndefined()) {
        return QStringLiteral("removed_at is only allowed on a removed agent conversation.");
    }
    if (record.contains(QStringLiteral("deleted")) && !record.value(QStringLiteral("deleted")).isBool())
        return QStringLiteral("The agent conversation has no valid deleted flag.");
    return QString();
}

TouchResult touch(const QJsonObject &stored, const Touch &input, const QString &originId, const QDateTime &now, const QString &deviceName)
{
    QString title = cleanTitle(input.title);
    QString titleSource = input.titleSource;
    if (title.isEmpty() || titleRank(titleSource) < 0) {
        title = fallbackTitle(input.agent, input.conversationId);
        titleSource = QStringLiteral("fallback");
    }
    const QString tool = input.linkedBy == QLatin1String("agent") ? QStringLiteral("link_conversation")
        : input.linkedBy == QLatin1String("backfill")             ? QString()
                                                                  : input.tool;
    const QString device = singleLine(deviceName, 128);
    const QString session = singleLine(input.sessionName, 100);
    const QString nowText = formatted(now);
    const bool withCard = validCardId(input.reviewCardId);

    // A purged record (deleted: true) is absent for v1 clients.
    if (stored.isEmpty() || stored.value(QStringLiteral("deleted")).toBool()) {
        const qint64 revision = stored.isEmpty() ? 1 : stored.value(revisionKey).toInteger() + 1;
        QJsonObject record{
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("link_id"), linkId(input.projectId, input.agent, input.conversationId)},
            {QStringLiteral("project_id"), input.projectId},
            {agentKey, input.agent},
            {QStringLiteral("conversation_id"), input.conversationId},
            {urlKey, input.conversationUrl},
            {titleKey, title},
            {titleSourceKey, titleSource},
            {sessionKey, session},
            {QStringLiteral("linked_by"), input.linkedBy},
            {QStringLiteral("last_tool"), tool},
            {QStringLiteral("device_name"), device},
            {QStringLiteral("created_at"), nowText},
            {lastActiveKey, nowText},
            {updatedKey, stored.isEmpty() ? nowText : timestamp(now, stored.value(updatedKey).toString())},
            {revisionKey, revision},
            {QStringLiteral("origin_id"), originId},
            {removedKey, false},
            {removedAtKey, QJsonValue::Null},
            {QStringLiteral("deleted"), false},
        };
        if (withCard) record.insert(cardsKey, applyCardTouch({}, input.reviewCardId, input.cardLinkedBy, now).cards);
        return {record, true, false, withCard};
    }

    const CardUpdate card = withCard ? applyCardTouch(stored, input.reviewCardId, input.cardLinkedBy, now) : CardUpdate();
    const bool wasRemoved = stored.value(removedKey).toBool();
    // Removal is sticky against automatic matching; only an explicit link revives it. The
    // card's own entry is still kept up to date: the card list ignores the Project's flag.
    if (wasRemoved && input.linkedBy != QLatin1String("agent")) {
        if (!card.due) return {stored, false, true, false};
        QJsonObject next = stored;
        next.insert(cardsKey, card.cards);
        next.insert(revisionKey, stored.value(revisionKey).toInteger() + 1);
        next.insert(updatedKey, timestamp(now, stored.value(updatedKey).toString()));
        next.insert(QStringLiteral("origin_id"), originId);
        return {next, true, true, true};
    }

    QJsonObject next = stored;
    bool changed = false;
    if (wasRemoved) {
        next.insert(removedKey, false);
        next.insert(removedAtKey, QJsonValue::Null);
        changed = true;
    }
    const QString storedTitle = stored.value(titleKey).toString();
    const QString storedSource = stored.value(titleSourceKey).toString();
    const int rank = titleRank(titleSource);
    const int storedRank = titleRank(storedSource);
    if (rank > storedRank || (rank == storedRank && (titleSource == QLatin1String("agent") || titleSource == QLatin1String("app")) && title != storedTitle)) {
        if (title != storedTitle || titleSource != storedSource) changed = true;
        next.insert(titleKey, title);
        next.insert(titleSourceKey, titleSource);
    }
    if (!input.conversationUrl.isEmpty() && input.conversationUrl != stored.value(urlKey).toString()) {
        next.insert(urlKey, input.conversationUrl);
        changed = true;
    }
    if (!session.isEmpty() && session != stored.value(sessionKey).toString()) {
        next.insert(sessionKey, session);
        changed = true;
    }
    const QDateTime storedActive = parsedTime(stored.value(lastActiveKey).toString());
    const bool stale = !storedActive.isValid() || storedActive.msecsTo(now) >= throttleSeconds * 1000;
    if (!changed && !stale && !card.due) return {stored, false, false, false};

    if (card.touched) next.insert(cardsKey, card.cards);
    next.insert(lastActiveKey, storedActive.isValid() && storedActive > now ? stored.value(lastActiveKey).toString() : nowText);
    next.insert(QStringLiteral("last_tool"), tool);
    next.insert(QStringLiteral("device_name"), device);
    next.insert(revisionKey, stored.value(revisionKey).toInteger() + 1);
    next.insert(updatedKey, timestamp(now, stored.value(updatedKey).toString()));
    next.insert(QStringLiteral("origin_id"), originId);
    return {next, true, false, card.touched};
}

QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = record;
    next.insert(removedKey, true);
    next.insert(removedAtKey, formatted(now));
    next.insert(revisionKey, record.value(revisionKey).toInteger() + 1);
    next.insert(updatedKey, timestamp(now, record.value(updatedKey).toString()));
    next.insert(QStringLiteral("origin_id"), originId);
    return next;
}

QJsonObject removeFromCard(const QJsonObject &record, const QString &cardId, const QString &originId, const QDateTime &now)
{
    QJsonArray cards = record.value(cardsKey).toArray();
    const int index = cardEntryIndex(cards, cardId);
    if (index < 0 || cards.at(index).toObject().value(removedKey).toBool()) return record;
    QJsonObject entry = cards.at(index).toObject();
    entry.insert(removedKey, true);
    cards.replace(index, entry);
    QJsonObject next = record;
    next.insert(cardsKey, cards);
    next.insert(revisionKey, record.value(revisionKey).toInteger() + 1);
    next.insert(updatedKey, timestamp(now, record.value(updatedKey).toString()));
    next.insert(QStringLiteral("origin_id"), originId);
    return next;
}

QList<CardConversation> recordsForCard(const QJsonObject &projections, const QString &cardId)
{
    QList<CardConversation> result;
    if (!validCardId(cardId)) return result;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (record.value(QStringLiteral("deleted")).toBool() || !validConversationId(record.value(QStringLiteral("conversation_id")).toString())) continue;
        const QJsonArray cards = record.value(cardsKey).toArray();
        const int index = cardEntryIndex(cards, cardId);
        if (index < 0 || cards.at(index).toObject().value(removedKey).toBool()) continue;
        result.append({record, cards.at(index).toObject()});
    }
    std::sort(result.begin(), result.end(), [](const CardConversation &a, const CardConversation &b) {
        const QDateTime ae = parsedTime(a.entry.value(lastActiveKey).toString());
        const QDateTime be = parsedTime(b.entry.value(lastActiveKey).toString());
        if (ae != be) return ae > be;
        const QDateTime ar = parsedTime(a.record.value(lastActiveKey).toString());
        const QDateTime br = parsedTime(b.record.value(lastActiveKey).toString());
        if (ar.isValid() != br.isValid()) return ar.isValid();
        if (ar.isValid() && ar != br) return ar > br;
        return a.record.value(QStringLiteral("link_id")).toString() < b.record.value(QStringLiteral("link_id")).toString();
    });
    return result;
}

QList<QJsonObject> recordsFor(const QJsonObject &projections, const QString &projectId)
{
    QList<QJsonObject> result;
    if (projectId.isEmpty()) return result;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (record.value(QStringLiteral("project_id")).toString() != projectId || record.value(removedKey).toBool()
            || record.value(QStringLiteral("deleted")).toBool())
            continue;
        result.append(record);
    }
    std::sort(result.begin(), result.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const QDateTime at = parsedTime(a.value(lastActiveKey).toString());
        const QDateTime bt = parsedTime(b.value(lastActiveKey).toString());
        if (at.isValid() != bt.isValid()) return at.isValid();
        if (at.isValid() && at != bt) return at > bt;
        return a.value(QStringLiteral("link_id")).toString() < b.value(QStringLiteral("link_id")).toString();
    });
    return result;
}

bool onlyConversationChanges(const QJsonObject &before, const QJsonObject &after)
{
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (!isProjectionKey(it.key()) && before.value(it.key()) != it.value()) return false;
    }
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        if (!isProjectionKey(it.key()) && !after.contains(it.key())) return false;
    }
    return true;
}

bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error)
{
    const QString problem = validationError(record);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return false;
    }
    QString queueError;
    if (!outbox.ready(&queueError)
        || outbox.enqueue(entityType(), record.value(QStringLiteral("link_id")).toString(), collection(), record.value(revisionKey).toInteger() - 1, record,
                          record.value(updatedKey).toString())
               .isEmpty()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The conversation could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

namespace
{
// The canonical path, also for a file that does not exist (yet): its deepest existing
// ancestor is resolved and the rest appended.
QString canonical(const QString &path)
{
    QString existing = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString rest;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const int slash = existing.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0) return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        rest = existing.mid(slash) + rest;
        existing.truncate(slash);
    }
    const QString resolved = QFileInfo(existing).canonicalFilePath();
    return (resolved.isEmpty() ? existing : resolved) + rest;
}

ProjectResolution failure(const QString &code, const QString &message)
{
    return {QJsonObject(), code, message};
}

ProjectResolution single(const QList<QJsonObject> &matches, const QString &none)
{
    if (matches.size() > 1) return failure(QStringLiteral("ambiguous_project"), QStringLiteral("More than one Project matches; name the Project instead."));
    if (matches.isEmpty()) return failure(QStringLiteral("not_in_library"), none);
    return {matches.constFirst(), QString(), QString()};
}
}

ProjectResolution resolveProject(const QJsonArray &projects, const QString &docsRoot, const QJsonObject &request)
{
    const QStringList kinds{QStringLiteral("project"), QStringLiteral("source"), QStringLiteral("document")};
    QString kind;
    for (const QString &candidate : kinds) {
        if (!request.contains(candidate)) continue;
        if (!kind.isEmpty()) return failure(QStringLiteral("invalid_request"), QStringLiteral("Give exactly one of project, source or document."));
        kind = candidate;
    }
    const QString value = request.value(kind).toString();
    if (kind.isEmpty() || value.isEmpty() || value.contains(QChar(0)))
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Give exactly one of project, source or document."));

    if (kind == QLatin1String("project")) {
        QList<QJsonObject> named;
        for (const auto &entry : projects) {
            const QJsonObject project = entry.toObject();
            if (project.value(QStringLiteral("id")).toString() == value) return {project, QString(), QString()};
            if (project.value(QStringLiteral("name")).toString().compare(value, Qt::CaseInsensitive) == 0) named.append(project);
        }
        if (named.size() > 1) return failure(QStringLiteral("ambiguous_project"), QStringLiteral("More than one Project is named \"%1\"; use its id.").arg(value));
        if (named.isEmpty()) return failure(QStringLiteral("project_not_found"), QStringLiteral("No Library Project is named \"%1\".").arg(value));
        return {named.constFirst(), QString(), QString()};
    }

    const QString root = canonical(docsRoot);
    const auto relativeInside = [&root](const QString &path, QString *relative) {
        const QString absolute = canonical(QDir::isAbsolutePath(path) ? path : QDir(root).filePath(path));
        if (!absolute.startsWith(root + QLatin1Char('/'))) return false;
        *relative = absolute.mid(root.size() + 1);
        return true;
    };

    if (kind == QLatin1String("source")) {
        QString relative;
        if (!relativeInside(value, &relative)) return failure(QStringLiteral("not_in_library"), QStringLiteral("The source is not inside the Thoughts Library."));
        QList<QJsonObject> matches;
        for (const auto &entry : projects) {
            const QJsonObject project = entry.toObject();
            bool found = false;
            for (const auto &raw : project.value(QStringLiteral("sources")).toArray()) {
                const QJsonObject location = raw.toObject();
                const QString owner = location.value(QStringLiteral("id")).toString();
                if (!owner.isEmpty() && relative.startsWith(owner + QLatin1Char('/'))) found = true;
                for (const auto &file : location.value(QStringLiteral("files")).toArray()) {
                    if (file.toObject().value(QStringLiteral("id")).toString() == relative) found = true;
                }
            }
            for (const auto &raw : project.value(QStringLiteral("documents")).toArray()) {
                if (raw.toObject().value(QStringLiteral("source_file")).toString() == relative) found = true;
            }
            // The Project's connected folders (its manifest, including the default folder),
            // also before their files were scanned.
            QStringList owners;
            for (const auto &raw : project.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("source_folders")).toArray())
                owners.append(raw.toObject().value(QStringLiteral("path")).toString());
            for (const auto &raw : project.value(QStringLiteral("source_folders")).toArray())
                owners.append(raw.toObject().value(QStringLiteral("project")).toString());
            for (const QString &owner : std::as_const(owners)) {
                if (!owner.isEmpty() && relative.startsWith(owner + QLatin1Char('/'))) found = true;
            }
            if (found) matches.append(project);
        }
        return single(matches, QStringLiteral("The source belongs to no Library Project."));
    }

    QString filename = value;
    if (QDir::isAbsolutePath(value)) {
        QString relative;
        if (!relativeInside(value, &relative) || !relative.startsWith(QLatin1String("current_pdfs/")))
            return failure(QStringLiteral("not_in_library"), QStringLiteral("The PDF is not a Thoughts Library document."));
        filename = QFileInfo(relative).fileName();
    } else if (value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\'))) {
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Give a Library PDF filename or an absolute path."));
    }
    QList<QJsonObject> matches;
    for (const auto &entry : projects) {
        const QJsonObject project = entry.toObject();
        bool found = project.value(QStringLiteral("target")).toString() == filename;
        for (const auto &raw : project.value(QStringLiteral("documents")).toArray()) {
            if (raw.toObject().value(QStringLiteral("filename")).toString() == filename) found = true;
        }
        if (found) matches.append(project);
    }
    return single(matches, QStringLiteral("No Library Project contains %1.").arg(filename));
}

QString codexThreadName(const QString &codexHome, const QString &conversationId)
{
    QFile index(QDir(codexHome).filePath(QStringLiteral("session_index.jsonl")));
    if (!index.open(QIODevice::ReadOnly)) return {};
    QString name;
    while (!index.atEnd()) {
        const QByteArray line = index.readLine();
        if (!line.contains(conversationId.toUtf8())) continue;
        const QJsonObject entry = QJsonDocument::fromJson(line).object();
        if (entry.value(QStringLiteral("id")).toString() == conversationId && !entry.value(QStringLiteral("thread_name")).toString().trimmed().isEmpty())
            name = entry.value(QStringLiteral("thread_name")).toString();
    }
    return name;
}

QList<Touch> proposalBackfill(const QJsonObject &projections, const QJsonArray &projects, const QString &docsRoot, const BackfillLookup &lookup)
{
    QList<Touch> touches;
    QSet<QString> seen;
    const QString prefix = QStringLiteral("proposal\nproposals\n");
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject row = it.value().toObject();
        if (row.value(QStringLiteral("deleted")).toBool()) continue;
        const QJsonObject snapshot = row.value(QStringLiteral("snapshot")).toObject();
        const QString conversationId = snapshot.value(QStringLiteral("conversation_id")).toString();
        if (!validConversationId(conversationId)) continue;
        const QString stored = snapshot.value(QStringLiteral("conversation_url")).toString();
        const QString url = lookup.link ? lookup.link(stored, conversationId) : stored;
        QString agent;
        if (url.startsWith(QLatin1String("codex://"))) agent = QStringLiteral("codex");
        else if (url.startsWith(QLatin1String("https://claude.ai/"))) agent = QStringLiteral("claude");
        else if (!lookup.claudeHome.isEmpty()) {
            QDirIterator transcripts(QDir(lookup.claudeHome).filePath(QStringLiteral("projects")), {conversationId + QStringLiteral(".jsonl")}, QDir::Files,
                                     QDirIterator::Subdirectories);
            if (transcripts.hasNext()) agent = QStringLiteral("claude");
        }
        if (agent.isEmpty() || !validConversationUrl(agent, conversationId, url)) continue;
        const QString source = row.value(QStringLiteral("source_file")).toString(snapshot.value(QStringLiteral("source_file")).toString());
        if (source.isEmpty()) continue;
        const ProjectResolution resolution = resolveProject(projects, docsRoot, QJsonObject{{QStringLiteral("source"), source}});
        if (resolution.project.isEmpty()) continue;
        Touch touch;
        touch.projectId = resolution.project.value(QStringLiteral("id")).toString();
        touch.agent = agent;
        touch.conversationId = conversationId;
        touch.conversationUrl = url;
        const QString thread = agent == QLatin1String("codex") && !lookup.codexHome.isEmpty() ? codexThreadName(lookup.codexHome, conversationId) : QString();
        touch.title = thread.isEmpty() ? QStringLiteral("Proposal conversation ") + conversationId.left(8) : thread;
        touch.titleSource = thread.isEmpty() ? QStringLiteral("fallback") : QStringLiteral("app");
        touch.linkedBy = QStringLiteral("backfill");
        const QString id = linkId(touch.projectId, agent, conversationId);
        if (touch.projectId.isEmpty() || seen.contains(id)) continue;
        seen.insert(id);
        touches.append(touch);
    }
    return touches;
}
}
