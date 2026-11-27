// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

namespace Thoughts
{
// Who wrote a comment, as the apps show it. A comment copied from the other person's Library
// of a shared Project carries shared_author {username}, set by the server (project-links.md
// §15); "author" is free text each device fills in and may be a computer's account name.
inline QString commentAuthor(const QJsonObject &record)
{
    const QString shared = record.value(QStringLiteral("shared_author")).toObject().value(QStringLiteral("username")).toString().trimmed();
    return shared.isEmpty() ? record.value(QStringLiteral("author")).toString() : shared;
}
}
