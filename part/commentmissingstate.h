/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_COMMENTMISSINGSTATE_H
#define THOUGHTS_COMMENTMISSINGSTATE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

// "Comment missing": a comment marker in a managed source whose comment no
// longer exists (deleted, or unknown to the comment store). The repository
// detects them during its source-comment refresh; see docs/comment-missing.md.
namespace Thoughts
{
// Local-only entry of the effective projections that lists the missing
// markers for the Issues view. It is never queued or synced.
inline QString commentMissingProjectionKey()
{
    return QStringLiteral("local_issue\ncomment_missing\nmarkers");
}

// One identity for the id forms a marker can carry: "<uuid>", "thoughts-<uuid>"
// and "thoughts-{<uuid>}" name the same comment.
inline QString commentMarkerIdentity(QString id)
{
    id = id.trimmed();
    if (id.startsWith(QLatin1String("thoughts-"))) id = id.mid(9);
    if (id.size() > 2 && id.startsWith(QLatin1Char('{')) && id.endsWith(QLatin1Char('}'))) id = id.mid(1, id.size() - 2);
    return id.toLower();
}

// Marker kinds that belong to comments; \ReviewAnchor is never reported.
inline bool isCommentMarkerKind(const QString &kind)
{
    return kind == QLatin1String("anchor") || kind == QLatin1String("highlight") || kind == QLatin1String("strikethrough") || kind == QLatin1String("markdown");
}

// Identities of every comment that exists: each non-deleted logical comment
// (its comment_id and scope.anchor_id) and each non-deleted legacy annotation
// that has no logical comment of the same id.
inline QSet<QString> liveCommentMarkerIdentities(const QJsonObject &projections)
{
    static const QString logicalPrefix = QStringLiteral("logical_comment\ncomments\n");
    QSet<QString> live;
    QSet<QString> logicalIds;
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(logicalPrefix)) continue;
        const QJsonObject record = it.value().toObject();
        const QString id = record.value(QStringLiteral("comment_id")).toString(it.key().mid(logicalPrefix.size()));
        logicalIds.insert(commentMarkerIdentity(id));
        if (record.value(QStringLiteral("deleted")).toBool()) continue;
        live.insert(commentMarkerIdentity(id));
        const QString anchor = record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString();
        if (!anchor.isEmpty()) live.insert(commentMarkerIdentity(anchor));
    }
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("annotation\n"))) continue;
        const QJsonObject record = it.value().toObject();
        const QString id = commentMarkerIdentity(it.key().section(QLatin1Char('\n'), 2));
        if (record.value(QStringLiteral("deleted")).toBool() || logicalIds.contains(id)) continue;
        live.insert(id);
        const QString anchor = record.value(QStringLiteral("anchor_id")).toString();
        if (!anchor.isEmpty()) live.insert(commentMarkerIdentity(anchor));
    }
    return live;
}

// The deleted root comment a marker belongs to: an exact anchor_id or
// comment_id match first, then the same identity in another id form.
inline QJsonObject deletedCommentForMarker(const QJsonObject &projections, const QString &markerId)
{
    static const QString logicalPrefix = QStringLiteral("logical_comment\ncomments\n");
    const QString identity = commentMarkerIdentity(markerId);
    QJsonObject similar;
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(logicalPrefix)) continue;
        const QJsonObject record = it.value().toObject();
        if (!record.value(QStringLiteral("deleted")).toBool() || !record.value(QStringLiteral("parent_id")).toString().isEmpty()) continue;
        const QString id = record.value(QStringLiteral("comment_id")).toString();
        const QString anchor = record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString();
        if (id == markerId || anchor == markerId) return record;
        if (similar.isEmpty() && (commentMarkerIdentity(id) == identity || (!anchor.isEmpty() && commentMarkerIdentity(anchor) == identity))) similar = record;
    }
    return similar;
}

// The server refuses to revive an item whose owning document was deleted.
inline bool deletedCommentBlocksRestore(const QJsonObject &record)
{
    const QJsonObject evidence = record.value(QStringLiteral("document_deletion")).toObject();
    if (!evidence.isEmpty() || record.value(QStringLiteral("deletion_reason")).toString() == QLatin1String("document_deleted")) return true;
    for (const auto &value : record.value(QStringLiteral("source_deletion")).toObject().value(QStringLiteral("sources")).toArray()) {
        if (value.toObject().value(QStringLiteral("deleted")).toBool()) return true;
    }
    return false;
}
}

#endif
