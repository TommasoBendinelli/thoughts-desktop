/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_COMMENTSIDEBARSTATE_H
#define THOUGHTS_COMMENTSIDEBARSTATE_H

#include "annotationproxymodels.h"
#include "commentanchorstate.h"
#include <QJsonArray>
#include <QJsonObject>

namespace Thoughts
{
// Status comes only from the canonical root. Publication inputs supply ordering,
// never evidence of successful linking or a location in the current PDF.
inline QHash<QString, AnnotationSourceOrder> commentSidebarState(const QHash<QString, QJsonObject> &comments, const QJsonArray &segments)
{
    QHash<QString, QPair<int, int>> sourceRanks;
    for (qsizetype segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const QJsonArray inputs = segments.at(segmentIndex).toObject().value(QStringLiteral("inputs")).toArray();
        for (qsizetype inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
            const QString path = inputs.at(inputIndex).toObject().value(QStringLiteral("source_file")).toString();
            if (!path.isEmpty() && !sourceRanks.contains(path)) sourceRanks.insert(path, {int(segmentIndex), int(inputIndex)});
        }
    }
    QHash<QString, AnnotationSourceOrder> result;
    for (auto it = comments.cbegin(); it != comments.cend(); ++it) {
        if (it->value(QStringLiteral("deleted")).toBool()) continue;
        const QString rootId = it->value(QStringLiteral("thread_root_id")).toString(it.key());
        const QJsonObject root = comments.value(rootId);
        if (root.isEmpty() || root.value(QStringLiteral("deleted")).toBool()) continue;
        const QJsonObject scope = root.value(QStringLiteral("scope")).toObject();
        // Anchor-removed threads are listed under Issues, not in the reader's comment order.
        if (isAnchorRemovedScope(scope)) continue;
        const QString state = scope.value(QStringLiteral("state")).toString();
        if (state != QLatin1String("source_linked") && state != QLatin1String("pdf_only") && state != QLatin1String("needs_attention")) continue;
        AnnotationSourceOrder entry;
        entry.threadRootId = rootId;
        const auto rank = sourceRanks.constFind(scope.value(QStringLiteral("source_file")).toString());
        entry.line = scope.value(QStringLiteral("source_line")).toInt();
        if (state == QLatin1String("source_linked") && rank != sourceRanks.cend() && entry.line > 0) {
            entry.hasSourcePosition = true;
            entry.sourceOrder = rank->first;
            entry.inputOrder = rank->second;
        }
        result.insert(it.key(), entry);
    }
    return result;
}
}
#endif
