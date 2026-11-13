// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/thoughtscommandprotocol.h"
#include "../shell/todocommands.h"

#include <QJsonArray>
#include <QTest>

using namespace Thoughts;

namespace
{
const QString prefix = QStringLiteral("logical_comment\ncomments\n");
const QDate today(2026, 11, 4);
const QString zone = QStringLiteral("Europe/Zurich");
const QDateTime now(today, QTime(12, 0), QTimeZone::UTC);
const QString requestId = QStringLiteral("0b0b0b0b-0000-4000-8000-000000000001");
const QString newId = QStringLiteral("thoughts-") + requestId;

QJsonObject note(const QString &id, const QString &contents, const QString &createdAt, const QJsonObject &extra = {})
{
    QJsonObject payload {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id}, {QStringLiteral("thread_root_id"), id},
                         {QStringLiteral("parent_id"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("revision"), 3}, {QStringLiteral("created_at"), createdAt},
                         {QStringLiteral("updated_at"), createdAt}, {QStringLiteral("origin_id"), QStringLiteral("phone")}, {QStringLiteral("deleted"), false},
                         {QStringLiteral("contents"), contents}, {QStringLiteral("author"), QStringLiteral("alex")}, {QStringLiteral("subtype"), QStringLiteral("Note")},
                         {QStringLiteral("priority"), QStringLiteral("normal")}, {QStringLiteral("origin"), QJsonValue(QJsonValue::Null)},
                         {QStringLiteral("project"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it) payload.insert(it.key(), it.value());
    return payload;
}

QJsonObject library()
{
    return {
        {prefix + QStringLiteral("older"), note(QStringLiteral("older"), QStringLiteral("Older"), QStringLiteral("2026-11-01T08:00:00.000Z"))},
        {prefix + QStringLiteral("newer"), note(QStringLiteral("newer"), QStringLiteral("Newer"), QStringLiteral("2026-11-03T08:00:00.000Z"),
            {{QStringLiteral("calendar_date"), QStringLiteral("2026-11-20")}, {QStringLiteral("calendar_time"), QStringLiteral("09:30")},
             {QStringLiteral("calendar_zone_id"), QStringLiteral("America/New_York")}, {QStringLiteral("check_where"), QStringLiteral("Kitchen")},
             {QStringLiteral("location"), QJsonObject {{QStringLiteral("label"), QStringLiteral("Home")}}}})},
        {prefix + QStringLiteral("later"), note(QStringLiteral("later"), QStringLiteral("Later"), QStringLiteral("2026-11-02T08:00:00.000Z"),
            {{QStringLiteral("visible_from"), QStringLiteral("2026-11-05")}})},
        {prefix + QStringLiteral("today"), note(QStringLiteral("today"), QStringLiteral("Shown from today"), QStringLiteral("2026-10-30T08:00:00.000Z"),
            {{QStringLiteral("visible_from"), QStringLiteral("2026-11-04")}})},
        {prefix + QStringLiteral("gone"), note(QStringLiteral("gone"), QStringLiteral("Deleted"), QStringLiteral("2026-11-03T09:00:00.000Z"), {{QStringLiteral("deleted"), true}})},
        {prefix + QStringLiteral("diary"), note(QStringLiteral("diary"), QStringLiteral("Dear diary"), QStringLiteral("2026-11-03T10:00:00.000Z"),
            {{QStringLiteral("entry_kind"), QStringLiteral("diary")}})},
        {prefix + QStringLiteral("comment"), note(QStringLiteral("comment"), QStringLiteral("A comment"), QStringLiteral("2026-11-03T11:00:00.000Z"),
            {{QStringLiteral("subtype"), QStringLiteral("Text")}})},
        {QStringLiteral("review_card\ncards\none"), QJsonObject {{QStringLiteral("subtype"), QStringLiteral("Note")}}},
    };
}

QJsonObject blank()
{
    return note(QStringLiteral("thoughts-placeholder"), QString(), QStringLiteral("2026-11-04T12:00:00.000Z"), {{QStringLiteral("revision"), 1}, {QStringLiteral("origin_id"), QStringLiteral("mac")}});
}

QJsonObject addRequest(const QJsonObject &extra = {})
{
    QJsonObject request {{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("add-todo")},
                         {QStringLiteral("request_id"), requestId}, {QStringLiteral("text"), QStringLiteral("  Call the bank  ")}};
    for (auto it = extra.begin(); it != extra.end(); ++it) request.insert(it.key(), it.value());
    return request;
}

QJsonObject editRequest(const QString &id, const QJsonObject &extra)
{
    QJsonObject request {{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("edit-todo")},
                         {QStringLiteral("todo_id"), id}, {QStringLiteral("expected_revision"), 3}};
    for (auto it = extra.begin(); it != extra.end(); ++it) request.insert(it.key(), it.value());
    return request;
}

TodoCommands::Change added(const QJsonObject &projections, const QJsonObject &request)
{
    return TodoCommands::add(projections, request, blank(), zone, today);
}

TodoCommands::Change edited(const QJsonObject &request)
{
    return TodoCommands::edit(library(), request, QStringLiteral("mac"), now, zone, today);
}
}

class TodoCommandsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testListsOnlyTheToDosTab();
    void testAddKeepsWhatItWasGiven();
    void testAddRefusesBadInput_data();
    void testAddRefusesBadInput();
    void testRetriedAddMakesOneToDo();
    void testEditChangesOnlyWhatItWasGiven();
    void testDueDateRulesFollowThePhone();
    void testEditRefusesWhatIsNotAToDo();
    void testEditRefusesAStaleCopy();
    void testRepeatedEditQueuesNothing();
};

void TodoCommandsTest::testListsOnlyTheToDosTab()
{
    QJsonObject result = TodoCommands::list(library(), {{QStringLiteral("command"), QStringLiteral("list-todos")}}, today);
    QVERIFY(result.value(QStringLiteral("accepted")).toBool());
    QJsonArray todos = result.value(QStringLiteral("todos")).toArray();
    QCOMPARE(todos.size(), 3);
    // Newest first; a to-do hidden until today is shown.
    QCOMPARE(todos.at(0).toObject().value(QStringLiteral("todo_id")).toString(), QStringLiteral("newer"));
    QCOMPARE(todos.at(1).toObject().value(QStringLiteral("todo_id")).toString(), QStringLiteral("older"));
    QCOMPARE(todos.at(2).toObject().value(QStringLiteral("todo_id")).toString(), QStringLiteral("today"));
    QCOMPARE(result.value(QStringLiteral("hidden_count")).toInt(), 1);
    const QJsonObject newer = todos.at(0).toObject();
    QCOMPARE(newer.value(QStringLiteral("text")).toString(), QStringLiteral("Newer"));
    QCOMPARE(newer.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(newer.value(QStringLiteral("due_date")).toString(), QStringLiteral("2026-11-20"));
    QCOMPARE(newer.value(QStringLiteral("due_time")).toString(), QStringLiteral("09:30"));
    QCOMPARE(newer.value(QStringLiteral("where")).toString(), QStringLiteral("Kitchen"));
    QCOMPARE(newer.value(QStringLiteral("priority")).toString(), QStringLiteral("normal"));
    QVERIFY(!newer.value(QStringLiteral("hidden")).toBool());

    result = TodoCommands::list(library(), {{QStringLiteral("include_hidden"), true}}, today);
    todos = result.value(QStringLiteral("todos")).toArray();
    QCOMPARE(todos.size(), 4);
    QCOMPARE(todos.at(1).toObject().value(QStringLiteral("todo_id")).toString(), QStringLiteral("later"));
    QVERIFY(todos.at(1).toObject().value(QStringLiteral("hidden")).toBool());
    QCOMPARE(todos.at(1).toObject().value(QStringLiteral("hidden_until")).toString(), QStringLiteral("2026-11-05"));

    QCOMPARE(TodoCommands::list(library(), {{QStringLiteral("include_hidden"), QStringLiteral("yes")}}, today).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(TodoCommands::list(library(), {{QStringLiteral("limit"), 1}}, today).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
}

void TodoCommandsTest::testAddKeepsWhatItWasGiven()
{
    TodoCommands::Change change = added(library(), addRequest());
    QCOMPARE(change.result.value(QStringLiteral("state")).toString(), QStringLiteral("added"));
    QVERIFY(change.result.value(QStringLiteral("accepted")).toBool());
    QCOMPARE(change.payload.value(QStringLiteral("comment_id")).toString(), newId);
    QCOMPARE(change.payload.value(QStringLiteral("thread_root_id")).toString(), newId);
    QCOMPARE(change.payload.value(QStringLiteral("contents")).toString(), QStringLiteral("Call the bank"));
    QCOMPARE(change.payload.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(change.payload.value(QStringLiteral("subtype")).toString(), QStringLiteral("Note"));
    QVERIFY(!change.payload.contains(QStringLiteral("entry_kind")));
    QVERIFY(change.payload.value(QStringLiteral("calendar_zone_id")).toString().isEmpty());
    QCOMPARE(change.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("todo_id")).toString(), newId);

    change = added(library(), addRequest({{QStringLiteral("hidden_until"), QStringLiteral("2026-12-01")}, {QStringLiteral("due_date"), QStringLiteral("2026-12-03")},
                                          {QStringLiteral("due_time"), QStringLiteral("14:30")}, {QStringLiteral("priority"), QStringLiteral("high")}, {QStringLiteral("where"), QStringLiteral(" Bank ")}}));
    QCOMPARE(change.payload.value(QStringLiteral("visible_from")).toString(), QStringLiteral("2026-12-01"));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_date")).toString(), QStringLiteral("2026-12-03"));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_time")).toString(), QStringLiteral("14:30"));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_zone_id")).toString(), zone);
    QCOMPARE(change.payload.value(QStringLiteral("priority")).toString(), QStringLiteral("high"));
    QCOMPARE(change.payload.value(QStringLiteral("check_where")).toString(), QStringLiteral("Bank"));
    QVERIFY(change.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("hidden")).toBool());
}

void TodoCommandsTest::testAddRefusesBadInput_data()
{
    QTest::addColumn<QJsonObject>("extra");
    QTest::addColumn<QString>("message");
    QTest::newRow("no request id") << QJsonObject {{QStringLiteral("request_id"), QString()}} << QStringLiteral("request_id");
    QTest::newRow("id with braces") << QJsonObject {{QStringLiteral("request_id"), QString(QStringLiteral("{") + requestId + QStringLiteral("}"))}} << QStringLiteral("request_id");
    QTest::newRow("blank text") << QJsonObject {{QStringLiteral("text"), QStringLiteral("   ")}} << QStringLiteral("text");
    QTest::newRow("text is a number") << QJsonObject {{QStringLiteral("text"), 7}} << QStringLiteral("text");
    QTest::newRow("long text") << QJsonObject {{QStringLiteral("text"), QString(20001, QLatin1Char('x'))}} << QStringLiteral("20000");
    QTest::newRow("long place") << QJsonObject {{QStringLiteral("where"), QString(2001, QLatin1Char('x'))}} << QStringLiteral("2000");
    QTest::newRow("date not real") << QJsonObject {{QStringLiteral("hidden_until"), QStringLiteral("2026-02-30")}} << QStringLiteral("hidden_until");
    QTest::newRow("date with time") << QJsonObject {{QStringLiteral("due_date"), QStringLiteral("2026-12-01T10:00:00")}} << QStringLiteral("due_date");
    QTest::newRow("time not real") << QJsonObject {{QStringLiteral("due_date"), QStringLiteral("2026-12-01")}, {QStringLiteral("due_time"), QStringLiteral("25:00")}} << QStringLiteral("due_time");
    QTest::newRow("time without date") << QJsonObject {{QStringLiteral("due_time"), QStringLiteral("10:00")}} << QStringLiteral("needs a due date");
    QTest::newRow("priority") << QJsonObject {{QStringLiteral("priority"), QStringLiteral("urgent")}} << QStringLiteral("priority");
    QTest::newRow("unknown field") << QJsonObject {{QStringLiteral("deleted"), true}} << QStringLiteral("Unexpected field: deleted");
}

void TodoCommandsTest::testAddRefusesBadInput()
{
    QFETCH(QJsonObject, extra);
    QFETCH(QString, message);
    const TodoCommands::Change change = added(library(), addRequest(extra));
    QCOMPARE(change.result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QVERIFY2(change.result.value(QStringLiteral("message")).toString().contains(message), qPrintable(change.result.value(QStringLiteral("message")).toString()));
    QVERIFY(change.payload.isEmpty());
    QCOMPARE(syncCommandExitCode(change.result), 2);
}

void TodoCommandsTest::testRetriedAddMakesOneToDo()
{
    const TodoCommands::Change first = added(library(), addRequest());
    QJsonObject projections = library();
    projections.insert(prefix + newId, first.payload);

    const TodoCommands::Change retry = added(projections, addRequest());
    QVERIFY(retry.payload.isEmpty());
    QVERIFY(retry.result.value(QStringLiteral("accepted")).toBool());
    QVERIFY(retry.result.value(QStringLiteral("replayed")).toBool());
    QCOMPARE(retry.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("todo_id")).toString(), newId);

    const TodoCommands::Change reused = added(projections, addRequest({{QStringLiteral("text"), QStringLiteral("Something else")}}));
    QVERIFY(reused.payload.isEmpty());
    QCOMPARE(reused.result.value(QStringLiteral("error_code")).toString(), QStringLiteral("request_id_reused"));

    QJsonObject removed = first.payload;
    removed.insert(QStringLiteral("deleted"), true);
    projections.insert(prefix + newId, removed);
    QCOMPARE(added(projections, addRequest()).result.value(QStringLiteral("error_code")).toString(), QStringLiteral("request_id_reused"));
}

void TodoCommandsTest::testEditChangesOnlyWhatItWasGiven()
{
    const TodoCommands::Change change = edited(editRequest(QStringLiteral("newer"), {{QStringLiteral("text"), QStringLiteral("Newer, reworded")}}));
    QCOMPARE(change.result.value(QStringLiteral("state")).toString(), QStringLiteral("edited"));
    QCOMPARE(change.payload.value(QStringLiteral("contents")).toString(), QStringLiteral("Newer, reworded"));
    QCOMPARE(change.payload.value(QStringLiteral("revision")).toInt(), 4);
    QCOMPARE(change.payload.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-11-04T12:00:00.000Z"));
    QCOMPARE(change.payload.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac"));
    QCOMPARE(change.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("revision")).toInt(), 4);

    // Everything else stays, including what agents cannot see.
    QJsonObject expected = library().value(prefix + QStringLiteral("newer")).toObject();
    for (const QString &key : {QStringLiteral("contents"), QStringLiteral("revision"), QStringLiteral("updated_at"), QStringLiteral("origin_id")}) {
        expected.insert(key, change.payload.value(key));
    }
    QCOMPARE(change.payload, expected);
}

void TodoCommandsTest::testDueDateRulesFollowThePhone()
{
    // A new date keeps the zone and time it had; a to-do with neither gets this Mac's zone.
    TodoCommands::Change change = edited(editRequest(QStringLiteral("newer"), {{QStringLiteral("due_date"), QStringLiteral("2026-11-21")}}));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_time")).toString(), QStringLiteral("09:30"));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_zone_id")).toString(), QStringLiteral("America/New_York"));
    change = edited(editRequest(QStringLiteral("older"), {{QStringLiteral("due_date"), QStringLiteral("2026-11-21")}}));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_zone_id")).toString(), zone);

    // Removing the time keeps the date; removing the date removes all three.
    change = edited(editRequest(QStringLiteral("newer"), {{QStringLiteral("due_time"), QString()}}));
    QCOMPARE(change.payload.value(QStringLiteral("calendar_date")).toString(), QStringLiteral("2026-11-20"));
    QVERIFY(change.payload.value(QStringLiteral("calendar_time")).toString().isEmpty());
    change = edited(editRequest(QStringLiteral("newer"), {{QStringLiteral("due_date"), QString()}}));
    for (const QString &key : {QStringLiteral("calendar_date"), QStringLiteral("calendar_time"), QStringLiteral("calendar_zone_id")}) {
        QVERIFY(change.payload.value(key).toString().isEmpty());
    }

    // Empty text clears hidden_until too; a time on a to-do without a date is refused.
    change = edited(editRequest(QStringLiteral("later"), {{QStringLiteral("hidden_until"), QString()}}));
    QVERIFY(change.payload.value(QStringLiteral("visible_from")).toString().isEmpty());
    QVERIFY(!change.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("hidden")).toBool());
    change = edited(editRequest(QStringLiteral("older"), {{QStringLiteral("due_time"), QStringLiteral("10:00")}}));
    QCOMPARE(change.result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QVERIFY(change.payload.isEmpty());
}

void TodoCommandsTest::testEditRefusesWhatIsNotAToDo()
{
    const QJsonObject text {{QStringLiteral("text"), QStringLiteral("New")}};
    const auto refused = [&](const QString &id, const QString &code) {
        const TodoCommands::Change change = edited(editRequest(id, text));
        return change.payload.isEmpty() && change.result.value(QStringLiteral("error_code")).toString() == code;
    };
    QVERIFY(refused(QStringLiteral("missing"), QStringLiteral("todo_unavailable")));
    QVERIFY(refused(QStringLiteral("gone"), QStringLiteral("todo_unavailable")));
    QVERIFY(refused(QStringLiteral("diary"), QStringLiteral("not_a_todo")));
    QVERIFY(refused(QStringLiteral("comment"), QStringLiteral("not_a_todo")));
    QVERIFY(refused(QStringLiteral("a}b"), QStringLiteral("invalid_request")));

    QCOMPARE(edited(editRequest(QStringLiteral("older"), {})).result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(edited(editRequest(QStringLiteral("older"), {{QStringLiteral("text"), QStringLiteral("  ")}})).result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(edited(editRequest(QStringLiteral("older"), {{QStringLiteral("deleted"), true}})).result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QJsonObject withoutRevision = editRequest(QStringLiteral("older"), text);
    withoutRevision.remove(QStringLiteral("expected_revision"));
    QCOMPARE(edited(withoutRevision).result.value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
}

void TodoCommandsTest::testEditRefusesAStaleCopy()
{
    const TodoCommands::Change change = edited(editRequest(QStringLiteral("older"), {{QStringLiteral("expected_revision"), 2}, {QStringLiteral("text"), QStringLiteral("New")}}));
    QVERIFY(change.payload.isEmpty());
    QCOMPARE(change.result.value(QStringLiteral("error_code")).toString(), QStringLiteral("todo_changed"));
    const QJsonObject current = change.result.value(QStringLiteral("todo")).toObject();
    QCOMPARE(current.value(QStringLiteral("text")).toString(), QStringLiteral("Older"));
    QCOMPARE(current.value(QStringLiteral("revision")).toInt(), 3);
}

void TodoCommandsTest::testRepeatedEditQueuesNothing()
{
    // A retry after a lost answer finds its own change, whatever revision it remembers.
    const TodoCommands::Change change = edited(editRequest(QStringLiteral("older"), {{QStringLiteral("expected_revision"), 2}, {QStringLiteral("text"), QStringLiteral("Older")}, {QStringLiteral("priority"), QStringLiteral("normal")}}));
    QVERIFY(change.payload.isEmpty());
    QVERIFY(change.result.value(QStringLiteral("accepted")).toBool());
    QCOMPARE(change.result.value(QStringLiteral("state")).toString(), QStringLiteral("unchanged"));
    QCOMPARE(change.result.value(QStringLiteral("todo")).toObject().value(QStringLiteral("revision")).toInt(), 3);
}

QTEST_GUILESS_MAIN(TodoCommandsTest)
#include "todocommandstest.moc"
