// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

/**
 * list-todos, add-todo and edit-todo: to-dos for agents (thoughtsctl and the Mac Thoughts tools).
 *
 * A to-do is a logical comment of subtype "Note" that is not a diary entry. These functions only
 * read the projections and work out the change; Shell::todoCommand queues it as the Add To-do
 * button and the to-do editor do, so it shows at once and the next Sync publishes it.
 */
namespace Thoughts::TodoCommands
{
struct Change {
    QJsonObject result; // The command's answer.
    QJsonObject payload; // The logical comment to queue; empty when there is nothing to queue.
};

QJsonObject list(const QJsonObject &projections, const QJsonObject &request, const QDate &today);

/** @p blank is an empty new to-do (CommentsOverview::standaloneNotePayload). */
Change add(const QJsonObject &projections, const QJsonObject &request, QJsonObject blank, const QString &zoneId, const QDate &today);

Change edit(const QJsonObject &projections, const QJsonObject &request, const QString &originId, const QDateTime &now, const QString &zoneId, const QDate &today);
}
