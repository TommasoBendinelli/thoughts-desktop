/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_COMMENTFRAGMENTIDS_H
#define OKULAR_COMMENTFRAGMENTIDS_H

#include <QString>
#include <QStringView>

#include <optional>

namespace Thoughts
{

struct CommentFragmentId {
    QString logicalId;
    int index = 0;
};

inline QStringView commentFragmentMarker()
{
    return u"#oe-fragment-";
}

inline QString makeCommentFragmentId(const QString &logicalId, int index)
{
    return logicalId + commentFragmentMarker() + QString::number(index);
}

inline std::optional<CommentFragmentId> parseCommentFragmentId(const QString &id)
{
    const qsizetype markerPosition = id.lastIndexOf(commentFragmentMarker());
    if (markerPosition <= 0) {
        return std::nullopt;
    }

    bool ok = false;
    const int index = QStringView(id).mid(markerPosition + commentFragmentMarker().size()).toInt(&ok);
    if (!ok || index <= 0) {
        return std::nullopt;
    }

    return CommentFragmentId {
        id.left(markerPosition),
        index,
    };
}

inline QString logicalCommentId(const QString &id)
{
    const std::optional<CommentFragmentId> fragment = parseCommentFragmentId(id);
    return fragment ? fragment->logicalId : id;
}

}

#endif
