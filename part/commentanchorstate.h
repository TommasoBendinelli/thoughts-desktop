/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_COMMENTANCHORSTATE_H
#define THOUGHTS_COMMENTANCHORSTATE_H

#include <QJsonObject>
#include <QString>

namespace Thoughts
{
// A source-linked root whose marker vanished without an explicit Delete. The
// server writes the synced state; clients derive the same scope only for display.
inline bool isAnchorRemovedScope(const QJsonObject &scope)
{
    return scope.value(QStringLiteral("state")).toString() == QLatin1String("needs_attention")
        && scope.value(QStringLiteral("reason")).toString() == QLatin1String("anchor_removed");
}

inline QString anchorKindForComment(const QJsonObject &comment, const QString &sourceFile)
{
    if (sourceFile.endsWith(QLatin1String(".md")) || sourceFile.endsWith(QLatin1String(".markdown"))) return QStringLiteral("markdown");
    const QString subtype = comment.value(QStringLiteral("subtype")).toString();
    if (subtype == QLatin1String("StrikeOut")) return QStringLiteral("CommentStrikethrough");
    if (subtype == QLatin1String("Highlight") || subtype == QLatin1String("Underline") || subtype == QLatin1String("Squiggly")) return QStringLiteral("CommentHighlight");
    return QStringLiteral("CommentAnchor");
}

// The last verified location is carried over unchanged; anchor_removed_at is server-only.
inline QJsonObject anchorRemovedScope(const QJsonObject &lastLinkedScope, const QJsonObject &comment)
{
    if (isAnchorRemovedScope(lastLinkedScope)) return lastLinkedScope;
    QJsonObject scope{{QStringLiteral("state"), QStringLiteral("needs_attention")}, {QStringLiteral("reason"), QStringLiteral("anchor_removed")}};
    const QString id = lastLinkedScope.value(QStringLiteral("anchor_id")).toString(comment.value(QStringLiteral("comment_id")).toString());
    if (!id.isEmpty()) scope.insert(QStringLiteral("anchor_id"), id);
    const QString sourceFile = lastLinkedScope.value(QStringLiteral("source_file")).toString();
    if (!sourceFile.isEmpty()) scope.insert(QStringLiteral("source_file"), sourceFile);
    const int line = lastLinkedScope.value(QStringLiteral("source_line")).toInt();
    if (line > 0) scope.insert(QStringLiteral("source_line"), line);
    scope.insert(QStringLiteral("anchor_kind"), lastLinkedScope.value(QStringLiteral("anchor_kind")).toString(anchorKindForComment(comment, sourceFile)));
    scope.insert(QStringLiteral("anchor_mirror"), comment.value(QStringLiteral("contents")).toString().left(16384));
    return scope;
}

// Sidecar placement records keep their last known anchor and become orphaned.
inline bool isAnchorRemovedPlacement(const QJsonObject &record)
{
    return record.value(QStringLiteral("anchor_status")).toString() == QLatin1String("orphaned")
        && record.value(QStringLiteral("orphan_reason")).toString() == QLatin1String("anchor_removed");
}

// Deletions that were automatic in older clients/servers; they stay restorable and are never cleaned up.
inline bool isAutomaticDeletionReason(const QString &reason)
{
    return reason == QLatin1String("canonical_source_marker_removed") || reason == QLatin1String("source_anchor_removed")
        || reason == QLatin1String("document_deleted");
}
}

#endif
