// SPDX-License-Identifier: GPL-2.0-or-later
#include "todocommands.h"

#include "thoughtscommandprotocol.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>
#include <QTime>
#include <QUuid>

#include <algorithm>

namespace
{
const QString commentPrefix = QStringLiteral("logical_comment\ncomments\n");
const int maxTextLength = 20000;
const int maxWhereLength = 2000;

// A command's name for a to-do field, and the logical comment's.
struct Field {
    QString request;
    QString payload;
};
const QList<Field> fields {
    {QStringLiteral("text"), QStringLiteral("contents")},
    {QStringLiteral("hidden_until"), QStringLiteral("visible_from")},
    {QStringLiteral("due_date"), QStringLiteral("calendar_date")},
    {QStringLiteral("due_time"), QStringLiteral("calendar_time")},
    {QStringLiteral("where"), QStringLiteral("check_where")},
    {QStringLiteral("priority"), QStringLiteral("priority")},
};

QString text(const QJsonObject &payload, const QString &key)
{
    return payload.value(key).toString();
}

bool isTodo(const QJsonObject &payload)
{
    return text(payload, QStringLiteral("subtype")) == QLatin1String("Note") && !payload.value(QStringLiteral("deleted")).toBool()
        && text(payload, QStringLiteral("parent_id")).isEmpty() && text(payload, QStringLiteral("entry_kind")) != QLatin1String("diary");
}

bool hiddenOn(const QJsonObject &payload, const QDate &today)
{
    const QDate from = QDate::fromString(text(payload, QStringLiteral("visible_from")), Qt::ISODate);
    return from.isValid() && from > today;
}

QJsonObject record(const QJsonObject &payload, const QDate &today)
{
    return {{QStringLiteral("todo_id"), payload.value(QStringLiteral("comment_id"))},
            {QStringLiteral("revision"), payload.value(QStringLiteral("revision"))},
            {QStringLiteral("text"), text(payload, QStringLiteral("contents"))},
            {QStringLiteral("priority"), payload.value(QStringLiteral("priority")).toString(QStringLiteral("normal"))},
            {QStringLiteral("due_date"), text(payload, QStringLiteral("calendar_date"))},
            {QStringLiteral("due_time"), text(payload, QStringLiteral("calendar_time"))},
            {QStringLiteral("hidden_until"), text(payload, QStringLiteral("visible_from"))},
            {QStringLiteral("hidden"), hiddenOn(payload, today)},
            {QStringLiteral("where"), text(payload, QStringLiteral("check_where"))},
            {QStringLiteral("created_at"), text(payload, QStringLiteral("created_at"))},
            {QStringLiteral("updated_at"), text(payload, QStringLiteral("updated_at"))}};
}

QJsonObject done(const QString &state, const QJsonObject &payload, const QDate &today, const QString &message)
{
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
            {QStringLiteral("state"), state}, {QStringLiteral("todo"), record(payload, today)}, {QStringLiteral("message"), message}};
}

Thoughts::TodoCommands::Change invalid(const QString &message)
{
    return {Thoughts::syncCommandError(QStringLiteral("invalid_request"), message), {}};
}

QString unexpectedField(const QJsonObject &request, QStringList allowed)
{
    allowed << QStringLiteral("schema_version") << QStringLiteral("command");
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!allowed.contains(it.key())) return QStringLiteral("Unexpected field: %1").arg(it.key());
    }
    return {};
}

QStringList withFields(const QStringList &names)
{
    QStringList allowed = names;
    for (const Field &field : fields) allowed << field.request;
    return allowed;
}

bool plainDate(const QString &value)
{
    const QDate date = QDate::fromString(value, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == value;
}

// The request's to-do fields under their logical comment names. Returns what is wrong with them.
QString readFields(const QJsonObject &request, QJsonObject *values)
{
    for (const Field &field : fields) {
        if (!request.contains(field.request)) continue;
        const QJsonValue value = request.value(field.request);
        if (!value.isString()) return QStringLiteral("%1 must be text.").arg(field.request);
        values->insert(field.payload, value.toString().trimmed());
    }
    if (values->contains(QStringLiteral("contents")) && text(*values, QStringLiteral("contents")).isEmpty()) return QStringLiteral("text must not be empty.");
    if (text(*values, QStringLiteral("contents")).size() > maxTextLength) return QStringLiteral("text can have at most %1 characters.").arg(maxTextLength);
    if (text(*values, QStringLiteral("check_where")).size() > maxWhereLength) return QStringLiteral("where can have at most %1 characters.").arg(maxWhereLength);
    const QString hiddenUntil = text(*values, QStringLiteral("visible_from"));
    if (!hiddenUntil.isEmpty() && !plainDate(hiddenUntil)) return QStringLiteral("Enter hidden_until as YYYY-MM-DD.");
    const QString dueDate = text(*values, QStringLiteral("calendar_date"));
    if (!dueDate.isEmpty() && !plainDate(dueDate)) return QStringLiteral("Enter due_date as YYYY-MM-DD.");
    const QString dueTime = text(*values, QStringLiteral("calendar_time"));
    if (!dueTime.isEmpty() && !QTime::fromString(dueTime, QStringLiteral("HH:mm")).isValid()) return QStringLiteral("Enter due_time as HH:MM.");
    const QString priority = text(*values, QStringLiteral("priority"));
    if (values->contains(QStringLiteral("priority")) && priority != QLatin1String("normal") && priority != QLatin1String("high")) {
        return QStringLiteral("priority must be normal or high.");
    }
    return {};
}

bool alreadyHas(const QJsonObject &payload, const QJsonObject &values)
{
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString current = payload.value(it.key()).toString(it.key() == QLatin1String("priority") ? QStringLiteral("normal") : QString());
        if (current != it.value().toString()) return false;
    }
    return true;
}

// As on the phone: no due date means no time or zone, and a date keeps the zone it was set in.
QString applyFields(QJsonObject *payload, const QJsonObject &values, const QString &zoneId)
{
    for (auto it = values.begin(); it != values.end(); ++it) payload->insert(it.key(), it.value());
    if (text(*payload, QStringLiteral("calendar_date")).isEmpty()) {
        if (!text(values, QStringLiteral("calendar_time")).isEmpty()) return QStringLiteral("A due time needs a due date.");
        payload->insert(QStringLiteral("calendar_time"), QString());
        payload->insert(QStringLiteral("calendar_zone_id"), QString());
    } else if (text(*payload, QStringLiteral("calendar_zone_id")).isEmpty()) {
        payload->insert(QStringLiteral("calendar_zone_id"), zoneId);
    }
    return {};
}
}

QJsonObject Thoughts::TodoCommands::list(const QJsonObject &projections, const QJsonObject &request, const QDate &today)
{
    const QString unexpected = unexpectedField(request, {QStringLiteral("include_hidden")});
    if (!unexpected.isEmpty()) return syncCommandError(QStringLiteral("invalid_request"), unexpected);
    const QJsonValue includeHidden = request.value(QStringLiteral("include_hidden"));
    if (!includeHidden.isUndefined() && !includeHidden.isBool()) return syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("include_hidden must be true or false."));

    QList<QJsonObject> todos;
    int hidden = 0;
    for (auto it = projections.begin(); it != projections.end(); ++it) {
        if (!it.key().startsWith(commentPrefix)) continue;
        const QJsonObject payload = it.value().toObject();
        if (!isTodo(payload)) continue;
        if (hiddenOn(payload, today)) {
            ++hidden;
            if (!includeHidden.toBool()) continue;
        }
        todos.append(record(payload, today));
    }
    std::sort(todos.begin(), todos.end(), [](const QJsonObject &left, const QJsonObject &right) {
        const QString leftTime = text(left, QStringLiteral("created_at"));
        const QString rightTime = text(right, QStringLiteral("created_at"));
        return leftTime != rightTime ? leftTime > rightTime : text(left, QStringLiteral("todo_id")) > text(right, QStringLiteral("todo_id"));
    });
    QJsonArray listed;
    for (const QJsonObject &todo : std::as_const(todos)) listed.append(todo);
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
            {QStringLiteral("state"), QStringLiteral("listed")}, {QStringLiteral("todos"), listed}, {QStringLiteral("hidden_count"), hidden}};
}

Thoughts::TodoCommands::Change Thoughts::TodoCommands::add(const QJsonObject &projections, const QJsonObject &request, QJsonObject blank, const QString &zoneId, const QDate &today)
{
    const QString unexpected = unexpectedField(request, withFields({QStringLiteral("request_id")}));
    if (!unexpected.isEmpty()) return invalid(unexpected);
    const QString requestId = request.value(QStringLiteral("request_id")).toString();
    if (QUuid::fromString(requestId).isNull() || QUuid::fromString(requestId).toString(QUuid::WithoutBraces) != requestId) {
        return invalid(QStringLiteral("Provide a fresh UUID as request_id."));
    }
    QJsonObject values;
    const QString problem = readFields(request, &values);
    if (!problem.isEmpty()) return invalid(problem);
    if (!values.contains(QStringLiteral("contents"))) return invalid(QStringLiteral("text must not be empty."));

    // The ID comes from the request, so an uncertain call can be retried without a second to-do.
    const QString id = QStringLiteral("thoughts-") + requestId;
    const QJsonObject existing = projections.value(commentPrefix + id).toObject();
    if (!existing.isEmpty()) {
        if (!isTodo(existing) || !alreadyHas(existing, values)) {
            return {syncCommandError(QStringLiteral("request_id_reused"), QStringLiteral("This request_id already made a different to-do; use a new UUID.")), {}};
        }
        Change replay {done(QStringLiteral("added"), existing, today, QStringLiteral("This to-do was already added.")), {}};
        replay.result.insert(QStringLiteral("replayed"), true);
        return replay;
    }
    blank.insert(QStringLiteral("comment_id"), id);
    blank.insert(QStringLiteral("thread_root_id"), id);
    const QString applied = applyFields(&blank, values, zoneId);
    if (!applied.isEmpty()) return invalid(applied);
    return {done(QStringLiteral("added"), blank, today, QStringLiteral("To-do added. It shows in Thoughts now; the next Sync puts it on your other devices.")), blank};
}

Thoughts::TodoCommands::Change Thoughts::TodoCommands::edit(const QJsonObject &projections, const QJsonObject &request, const QString &originId, const QDateTime &now, const QString &zoneId, const QDate &today)
{
    const QString unexpected = unexpectedField(request, withFields({QStringLiteral("todo_id"), QStringLiteral("expected_revision")}));
    if (!unexpected.isEmpty()) return invalid(unexpected);
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    const QString id = request.value(QStringLiteral("todo_id")).toString();
    if (!idPattern.match(id).hasMatch()) return invalid(QStringLiteral("Provide the todo_id from list-todos."));
    const qint64 expected = request.value(QStringLiteral("expected_revision")).toInteger(-1);
    if (expected < 1) return invalid(QStringLiteral("Provide the to-do's revision from list-todos as expected_revision."));
    QJsonObject values;
    const QString problem = readFields(request, &values);
    if (!problem.isEmpty()) return invalid(problem);
    if (values.isEmpty()) return invalid(QStringLiteral("Give at least one field to change."));

    const QJsonObject previous = projections.value(commentPrefix + id).toObject();
    if (previous.isEmpty() || previous.value(QStringLiteral("deleted")).toBool()) {
        return {syncCommandError(QStringLiteral("todo_unavailable"), QStringLiteral("Thoughts has no to-do with this ID. List the to-dos again.")), {}};
    }
    if (!isTodo(previous)) {
        return {syncCommandError(QStringLiteral("not_a_todo"), QStringLiteral("This ID belongs to a comment or a diary entry, not a to-do.")), {}};
    }
    // First, so that retrying an edit that did go through is not mistaken for someone else's change.
    if (alreadyHas(previous, values)) {
        return {done(QStringLiteral("unchanged"), previous, today, QStringLiteral("The to-do already says this.")), {}};
    }
    const qint64 revision = previous.value(QStringLiteral("revision")).toInteger();
    if (revision != expected) {
        QJsonObject changed = syncCommandError(QStringLiteral("todo_changed"), QStringLiteral("This to-do changed since it was read. Read it again before changing it."));
        changed.insert(QStringLiteral("todo"), record(previous, today));
        return {changed, {}};
    }
    QJsonObject payload = previous;
    const QString applied = applyFields(&payload, values, zoneId);
    if (!applied.isEmpty()) return invalid(applied);
    payload.insert(QStringLiteral("revision"), revision + 1);
    payload.insert(QStringLiteral("updated_at"), now.toUTC().toString(Qt::ISODateWithMs));
    payload.insert(QStringLiteral("origin_id"), originId);
    // As the to-do editor saves it (CommentsOverview::logicalMutationPayload).
    payload.insert(QStringLiteral("project"), QJsonValue(QJsonValue::Null));
    for (const QString &key : {QStringLiteral("project_name"), QStringLiteral("locations"), QStringLiteral("source_file"), QStringLiteral("source_line"), QStringLiteral("source_sha256")}) {
        payload.remove(key);
    }
    return {done(QStringLiteral("edited"), payload, today, QStringLiteral("To-do changed. It shows in Thoughts now; the next Sync puts it on your other devices.")), payload};
}
