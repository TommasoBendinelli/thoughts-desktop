// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/agentconversations.h"
#include "../part/desktopsyncstore.h"
#include "../part/proposalprovenance.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts;

namespace
{
const QString claudeId = QStringLiteral("5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139");
const QString codexId = QStringLiteral("01900000-0000-7000-8000-000000000002");
const QString claudeUrl = QStringLiteral("https://claude.ai/code/session_01ExampleSessionBcdeFghi");
const QString project = QStringLiteral("source:docs-concepts");
const QString origin = QStringLiteral("desktop-test");
const QString device = QStringLiteral("Alexs-MacBook-Pro");

QDateTime at(const QString &text)
{
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}

AgentConversations::Touch toolTouch(const QString &title = QStringLiteral("Thoughts search on mobile"), const QString &source = QStringLiteral("app"))
{
    AgentConversations::Touch touch;
    touch.projectId = project;
    touch.agent = QStringLiteral("claude");
    touch.conversationId = claudeId;
    touch.conversationUrl = claudeUrl;
    touch.title = title;
    touch.titleSource = source;
    touch.sessionName = QStringLiteral("alex-94");
    touch.linkedBy = QStringLiteral("tool");
    touch.tool = QStringLiteral("create_proposal");
    return touch;
}

QJsonObject created(const QDateTime &now = at(QStringLiteral("2026-10-26T11:02:13.511Z")))
{
    return AgentConversations::touch({}, toolTouch(), origin, now, device).record;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class AgentConversationsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLinkIdVectors();
    void testProposalConversationLinkUsesTheStoredLinkOnly();
    void testCreate();
    void testValidation();
    void testClaudeAppUrl();
    void testTitleCleaning();
    void testTouchThrottle();
    void testTouchTitleRanks();
    void testTouchKeepsUrlAndUnknownFields();
    void testRemovalIsStickyAgainstTools();
    void testExplicitLinkRevives();
    void testRemove();
    void testRecordsFor();
    void testOnlyConversationChanges();
    void testEnqueue();
    void testResolveProject();
    void testProposalBackfill();
    void testCardEntryCreatedWithTheRecord();
    void testCardEntryOnAnExistingRecord();
    void testCardEntryThrottle();
    void testCardRemovalIsStickyExceptForTheAgent();
    void testCardEntryOnAProjectRemovedRecord();
    void testCardEntryLimit();
    void testRemoveFromCard();
    void testRecordsForCard();
};

namespace
{
const QString cardId = QStringLiteral("0fc5768d-a56c-53d0-9f37-7af64c3cf8a9");

AgentConversations::Touch cardTouch(const QString &linkedBy = QStringLiteral("prompt"))
{
    auto touch = toolTouch();
    touch.tool = QStringLiteral("prompt");
    touch.reviewCardId = cardId;
    touch.cardLinkedBy = linkedBy;
    return touch;
}

QJsonObject cardEntry(const QJsonObject &record, const QString &card = cardId)
{
    for (const auto &value : record.value(QStringLiteral("review_cards")).toArray()) {
        if (value.toObject().value(QStringLiteral("card_id")).toString() == card) return value.toObject();
    }
    return {};
}
}

void AgentConversationsTest::testProposalConversationLinkUsesTheStoredLinkOnly()
{
    // Only the link stored with a proposal opens its conversation; nothing on
    // this computer is searched for older proposals without one.
    QTemporaryDir codexHome;
    QVERIFY(codexHome.isValid());
    const QString id = QStringLiteral("019a1c2d-3e4f-7a5b-8c6d-7e8f9a0b1c2d");
    const QString day = codexHome.filePath(QStringLiteral("sessions/2026/11/12"));
    QVERIFY(QDir().mkpath(day));
    QFile rollout(QDir(day).filePath(QStringLiteral("rollout-2026-11-12T09-02-43-") + id + QStringLiteral(".jsonl")));
    QVERIFY(rollout.open(QIODevice::WriteOnly));
    rollout.close();
    const QByteArray previous = qgetenv("CODEX_HOME");
    qputenv("CODEX_HOME", codexHome.path().toUtf8());
    const QString stored = QStringLiteral("codex://threads/") + id;
    QCOMPARE(Thoughts::proposalConversationLink(stored, id), stored);
    QVERIFY(Thoughts::proposalConversationLink(QString(), id).isEmpty());
    QVERIFY(Thoughts::proposalConversationLink(QStringLiteral("https://example.com/not-a-conversation"), id).isEmpty());
    if (previous.isEmpty()) qunsetenv("CODEX_HOME"); else qputenv("CODEX_HOME", previous);
}

void AgentConversationsTest::testLinkIdVectors()
{
    QCOMPARE(AgentConversations::linkId(QStringLiteral("source:docs-concepts"), QStringLiteral("claude"), claudeId),
             QStringLiteral("ef8e44d466d11197fa75b69c419f74b574068655d7b0dd3ffeced7c341db3f4d"));
    QCOMPARE(AgentConversations::linkId(QStringLiteral("new:8b964c95-8bc0-4045-a38e-015d2172d7ca"), QStringLiteral("codex"), codexId),
             QStringLiteral("e05cfb424a6849e70f4fc8ad7eb8ba99a5c6c25da759ccf97fae0a9aadca6679"));
    QCOMPARE(AgentConversations::projectionKey(QStringLiteral("abc")), QStringLiteral("agent_conversation\nagent-conversations\nabc"));
    QCOMPARE(AgentConversations::capability(), QStringLiteral("agent_conversations_v1"));
}

void AgentConversationsTest::testCreate()
{
    const auto result = AgentConversations::touch({}, toolTouch(), origin, at(QStringLiteral("2026-10-26T11:02:13.511Z")), device);
    QVERIFY(result.written);
    QVERIFY(!result.removed);
    const QJsonObject &record = result.record;
    QCOMPARE(record.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(record.value(QStringLiteral("link_id")).toString(), QStringLiteral("ef8e44d466d11197fa75b69c419f74b574068655d7b0dd3ffeced7c341db3f4d"));
    QCOMPARE(record.value(QStringLiteral("title")).toString(), QStringLiteral("Thoughts search on mobile"));
    QCOMPARE(record.value(QStringLiteral("title_source")).toString(), QStringLiteral("app"));
    QCOMPARE(record.value(QStringLiteral("session_name")).toString(), QStringLiteral("alex-94"));
    QCOMPARE(record.value(QStringLiteral("linked_by")).toString(), QStringLiteral("tool"));
    QCOMPARE(record.value(QStringLiteral("last_tool")).toString(), QStringLiteral("create_proposal"));
    QCOMPARE(record.value(QStringLiteral("device_name")).toString(), device);
    QCOMPARE(record.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-26T11:02:13.511Z"));
    QCOMPARE(record.value(QStringLiteral("last_active_at")).toString(), QStringLiteral("2026-10-26T11:02:13.511Z"));
    QCOMPARE(record.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-26T11:02:13.511Z"));
    QCOMPARE(record.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(record.value(QStringLiteral("origin_id")).toString(), origin);
    QCOMPARE(record.value(QStringLiteral("removed")).toBool(true), false);
    QVERIFY(record.value(QStringLiteral("removed_at")).isNull());
    QCOMPARE(record.value(QStringLiteral("deleted")).toBool(true), false);
    QVERIFY2(AgentConversations::validationError(record).isEmpty(), qPrintable(AgentConversations::validationError(record)));

    // An explicit link records link_conversation; a backfill records no tool.
    auto explicitLink = toolTouch();
    explicitLink.linkedBy = QStringLiteral("agent");
    QCOMPARE(AgentConversations::touch({}, explicitLink, origin, QDateTime::currentDateTimeUtc(), device).record.value(QStringLiteral("last_tool")).toString(),
             QStringLiteral("link_conversation"));
    auto backfill = toolTouch();
    backfill.linkedBy = QStringLiteral("backfill");
    QCOMPARE(AgentConversations::touch({}, backfill, origin, QDateTime::currentDateTimeUtc(), device).record.value(QStringLiteral("last_tool")).toString(), QString());

    // Without a usable title the fallback names the conversation.
    auto untitled = toolTouch(QStringLiteral(" \n "), QStringLiteral("prompt"));
    const auto fallback = AgentConversations::touch({}, untitled, origin, QDateTime::currentDateTimeUtc(), device).record;
    QCOMPARE(fallback.value(QStringLiteral("title")).toString(), QStringLiteral("Claude conversation 5ef4cd0f"));
    QCOMPARE(fallback.value(QStringLiteral("title_source")).toString(), QStringLiteral("fallback"));
}

void AgentConversationsTest::testClaudeAppUrl()
{
    // Claude Code links open in the Claude app; its handler takes exactly (cse|session)_[A-Za-z0-9_-]+.
    QCOMPARE(AgentConversations::claudeAppUrl(QStringLiteral("https://claude.ai/code/session_01ExampleSessionCdefGhij")),
             QUrl(QStringLiteral("claude://code/session_01ExampleSessionCdefGhij")));
    QCOMPARE(AgentConversations::claudeAppUrl(QStringLiteral("https://claude.ai/code/cse_01Ab-c_9")), QUrl(QStringLiteral("claude://code/cse_01Ab-c_9")));
    const QStringList others = {
        QString(),
        QStringLiteral("codex://threads/019a0000-0000-7000-8000-000000000000"),
        QStringLiteral("https://claude.ai/code/"),
        QStringLiteral("https://claude.ai/code/session_"),
        QStringLiteral("https://claude.ai/code/other_01abc"),
        QStringLiteral("https://claude.ai/chat/session_01abc"),
        QStringLiteral("http://claude.ai/code/session_01abc"),
        QStringLiteral("https://claude.ai.evil.com/code/session_01abc"),
        QStringLiteral("https://claudeXai/code/session_01abc"),
        QStringLiteral("https://claude.ai/code/session_01abc/extra"),
        QStringLiteral("https://claude.ai/code/session_01abc?x=1"),
        QStringLiteral("https://claude.ai/code/session_01a.bc"),
    };
    for (const QString &url : others) {
        QVERIFY2(AgentConversations::claudeAppUrl(url).isEmpty(), qPrintable(url));
    }
}

void AgentConversationsTest::testValidation()
{
    const QJsonObject valid = created();
    QVERIFY(AgentConversations::validationError(valid).isEmpty());
    const auto invalid = [&valid](const QString &field, const QJsonValue &value) {
        QJsonObject record = valid;
        if (value.isUndefined()) record.remove(field);
        else record.insert(field, value);
        return !AgentConversations::validationError(record).isEmpty();
    };
    QVERIFY(invalid(QStringLiteral("link_id"), QString(64, QLatin1Char('f'))));
    QVERIFY(invalid(QStringLiteral("conversation_url"), QString(QStringLiteral("codex://threads/") + claudeId)));
    QVERIFY(invalid(QStringLiteral("conversation_url"), QString(QStringLiteral("https://claude.ai/chat/") + claudeId)));
    QVERIFY(invalid(QStringLiteral("conversation_url"), QJsonValue::Null));
    QVERIFY(invalid(QStringLiteral("removed"), true));
    QVERIFY(invalid(QStringLiteral("removed_at"), QStringLiteral("2026-10-26T12:00:00.000Z")));
    QVERIFY(invalid(QStringLiteral("removed"), QStringLiteral("false")));
    QVERIFY(invalid(QStringLiteral("title"), QStringLiteral("Two\nlines")));
    QVERIFY(invalid(QStringLiteral("title"), QStringLiteral("   ")));
    QVERIFY(invalid(QStringLiteral("title"), QString(201, QLatin1Char('x'))));
    QVERIFY(invalid(QStringLiteral("title_source"), QStringLiteral("user")));
    QVERIFY(invalid(QStringLiteral("linked_by"), QStringLiteral("user")));
    QVERIFY(invalid(QStringLiteral("last_tool"), QStringLiteral("Create-Proposal")));
    QVERIFY(invalid(QStringLiteral("agent"), QStringLiteral("gemini")));
    QVERIFY(invalid(QStringLiteral("updated_at"), QStringLiteral("2026-10-26T11:02:13")));
    QVERIFY(invalid(QStringLiteral("revision"), 0));
    QVERIFY(invalid(QStringLiteral("revision"), 1.5));
    QVERIFY(invalid(QStringLiteral("origin_id"), QString()));
    QVERIFY(invalid(QStringLiteral("session_name"), QString(101, QLatin1Char('s'))));
    QVERIFY(invalid(QStringLiteral("schema_version"), 2));
    QVERIFY(invalid(QStringLiteral("project_id"), QJsonValue::Undefined));

    // A Codex record links exactly its own thread.
    AgentConversations::Touch codex = toolTouch();
    codex.agent = QStringLiteral("codex");
    codex.conversationId = codexId;
    codex.conversationUrl = QStringLiteral("codex://threads/") + codexId;
    codex.sessionName.clear();
    const auto codexRecord = AgentConversations::touch({}, codex, origin, QDateTime::currentDateTimeUtc(), device).record;
    QVERIFY(AgentConversations::validationError(codexRecord).isEmpty());
    QVERIFY(!AgentConversations::validConversationUrl(QStringLiteral("codex"), codexId, QStringLiteral("codex://threads/") + claudeId));
    QVERIFY(AgentConversations::validConversationUrl(QStringLiteral("claude"), claudeId, QString()));

    // A removed record is valid with removed_at.
    QVERIFY(AgentConversations::validationError(AgentConversations::remove(valid, origin)).isEmpty());
}

void AgentConversationsTest::testTitleCleaning()
{
    QCOMPARE(AgentConversations::cleanTitle(QStringLiteral("  Fix\tthe\n\nsearch   bar  ")), QStringLiteral("Fix the search bar"));
    const QString longTitle = QStringLiteral("word ").repeated(60);
    const QString cleaned = AgentConversations::cleanTitle(longTitle);
    QVERIFY(cleaned.size() <= AgentConversations::titleLimit);
    QVERIFY(cleaned.endsWith(QChar(0x2026)));
    QVERIFY(cleaned.startsWith(QStringLiteral("word word")));
    QCOMPARE(AgentConversations::fallbackTitle(QStringLiteral("codex"), codexId), QStringLiteral("Codex conversation 01900000"));
}

void AgentConversationsTest::testTouchThrottle()
{
    const QJsonObject first = created(at(QStringLiteral("2026-10-26T11:00:00.000Z")));
    // Nothing changed and the last touch is recent: nothing is written.
    auto result = AgentConversations::touch(first, toolTouch(), origin, at(QStringLiteral("2026-10-26T11:05:00.000Z")), device);
    QVERIFY(!result.written);
    QCOMPARE(result.record, first);
    // 600 s later the touch records the activity.
    result = AgentConversations::touch(first, toolTouch(), QStringLiteral("other-mac"), at(QStringLiteral("2026-10-26T11:10:00.000Z")), QStringLiteral("other"));
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(result.record.value(QStringLiteral("last_active_at")).toString(), QStringLiteral("2026-10-26T11:10:00.000Z"));
    QCOMPARE(result.record.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-26T11:10:00.000Z"));
    QCOMPARE(result.record.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-26T11:00:00.000Z"));
    QCOMPARE(result.record.value(QStringLiteral("origin_id")).toString(), QStringLiteral("other-mac"));
    QCOMPARE(result.record.value(QStringLiteral("device_name")).toString(), QStringLiteral("other"));
    QCOMPARE(result.record.value(QStringLiteral("linked_by")).toString(), QStringLiteral("tool"));

    // A changed session name writes at once.
    auto renamed = toolTouch();
    renamed.sessionName = QStringLiteral("alex-95");
    result = AgentConversations::touch(first, renamed, origin, at(QStringLiteral("2026-10-26T11:01:00.000Z")), device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("session_name")).toString(), QStringLiteral("alex-95"));

    // A clock behind the stored record never moves last_active_at back, and updated_at grows.
    result = AgentConversations::touch(first, renamed, origin, at(QStringLiteral("2026-10-26T10:00:00.000Z")), device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("last_active_at")).toString(), QStringLiteral("2026-10-26T11:00:00.000Z"));
    QCOMPARE(result.record.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-26T11:00:00.001Z"));
}

void AgentConversationsTest::testTouchTitleRanks()
{
    const QDateTime soon = at(QStringLiteral("2026-10-26T11:03:00.000Z"));
    const QJsonObject prompt = AgentConversations::touch({}, toolTouch(QStringLiteral("First prompt"), QStringLiteral("prompt")), origin,
                                                         at(QStringLiteral("2026-10-26T11:02:00.000Z")), device).record;
    // A lower rank never replaces a higher one.
    auto result = AgentConversations::touch(prompt, toolTouch(QStringLiteral("Claude conversation 5ef4cd0f"), QStringLiteral("fallback")), origin, soon, device);
    QVERIFY(!result.written);
    // An equal prompt rank with another text does not replace it either.
    result = AgentConversations::touch(prompt, toolTouch(QStringLiteral("Second prompt"), QStringLiteral("prompt")), origin, soon, device);
    QVERIFY(!result.written);
    // A higher rank replaces it.
    result = AgentConversations::touch(prompt, toolTouch(QStringLiteral("App title"), QStringLiteral("app")), origin, soon, device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("title")).toString(), QStringLiteral("App title"));
    QCOMPARE(result.record.value(QStringLiteral("title_source")).toString(), QStringLiteral("app"));
    // An app title follows a rename.
    const QJsonObject app = result.record;
    result = AgentConversations::touch(app, toolTouch(QStringLiteral("Renamed"), QStringLiteral("app")), origin, soon, device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("title")).toString(), QStringLiteral("Renamed"));
    // The agent's own title wins over the app's, and the app's does not take it back.
    auto agent = toolTouch(QStringLiteral("Explains the GRPO notes"), QStringLiteral("agent"));
    agent.linkedBy = QStringLiteral("agent");
    result = AgentConversations::touch(app, agent, origin, soon, device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("title")).toString(), QStringLiteral("Explains the GRPO notes"));
    QCOMPARE(result.record.value(QStringLiteral("last_tool")).toString(), QStringLiteral("link_conversation"));
    QCOMPARE(result.record.value(QStringLiteral("linked_by")).toString(), QStringLiteral("tool"));
    result = AgentConversations::touch(result.record, toolTouch(QStringLiteral("Another app title"), QStringLiteral("app")), origin, soon, device);
    QVERIFY(!result.written);
}

void AgentConversationsTest::testTouchKeepsUrlAndUnknownFields()
{
    QJsonObject stored = created(at(QStringLiteral("2026-10-26T11:00:00.000Z")));
    stored.insert(QStringLiteral("phone_extension"), QJsonObject{{QStringLiteral("kept"), true}});
    auto withoutUrl = toolTouch();
    withoutUrl.conversationUrl.clear();
    withoutUrl.sessionName.clear();
    const auto result = AgentConversations::touch(stored, withoutUrl, origin, at(QStringLiteral("2026-10-26T12:00:00.000Z")), device);
    QVERIFY(result.written);
    QCOMPARE(result.record.value(QStringLiteral("conversation_url")).toString(), claudeUrl);
    QCOMPARE(result.record.value(QStringLiteral("session_name")).toString(), QStringLiteral("alex-94"));
    QCOMPARE(result.record.value(QStringLiteral("phone_extension")).toObject().value(QStringLiteral("kept")).toBool(), true);

    // A link appearing later is recorded at once.
    QJsonObject unlinked = AgentConversations::touch({}, withoutUrl, origin, at(QStringLiteral("2026-10-26T11:00:00.000Z")), device).record;
    QVERIFY(unlinked.value(QStringLiteral("conversation_url")).toString().isEmpty());
    const auto linked = AgentConversations::touch(unlinked, toolTouch(), origin, at(QStringLiteral("2026-10-26T11:00:30.000Z")), device);
    QVERIFY(linked.written);
    QCOMPARE(linked.record.value(QStringLiteral("conversation_url")).toString(), claudeUrl);
}

void AgentConversationsTest::testRemovalIsStickyAgainstTools()
{
    const QJsonObject removed = AgentConversations::remove(created(at(QStringLiteral("2026-10-26T11:00:00.000Z"))), origin, at(QStringLiteral("2026-10-26T11:01:00.000Z")));
    for (const QString &linkedBy : {QStringLiteral("tool"), QStringLiteral("backfill")}) {
        auto touch = toolTouch(QStringLiteral("A better title"), QStringLiteral("app"));
        touch.linkedBy = linkedBy;
        const auto result = AgentConversations::touch(removed, touch, origin, at(QStringLiteral("2026-10-27T11:00:00.000Z")), device);
        QVERIFY(!result.written);
        QVERIFY(result.removed);
        QCOMPARE(result.record, removed);
    }
}

void AgentConversationsTest::testExplicitLinkRevives()
{
    const QJsonObject removed = AgentConversations::remove(created(at(QStringLiteral("2026-10-26T11:00:00.000Z"))), origin, at(QStringLiteral("2026-10-26T11:01:00.000Z")));
    auto explicitLink = toolTouch(QStringLiteral("Back again"), QStringLiteral("agent"));
    explicitLink.linkedBy = QStringLiteral("agent");
    const auto result = AgentConversations::touch(removed, explicitLink, origin, at(QStringLiteral("2026-10-26T11:02:00.000Z")), device);
    QVERIFY(result.written);
    QVERIFY(!result.removed);
    QCOMPARE(result.record.value(QStringLiteral("removed")).toBool(true), false);
    QVERIFY(result.record.value(QStringLiteral("removed_at")).isNull());
    QCOMPARE(result.record.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(result.record.value(QStringLiteral("title")).toString(), QStringLiteral("Back again"));
    QVERIFY(AgentConversations::validationError(result.record).isEmpty());
}

void AgentConversationsTest::testRemove()
{
    QJsonObject stored = created(at(QStringLiteral("2026-10-26T11:00:00.000Z")));
    stored.insert(QStringLiteral("phone_extension"), 7);
    const QJsonObject removed = AgentConversations::remove(stored, QStringLiteral("other-mac"), at(QStringLiteral("2026-10-26T10:00:00.000Z")));
    QCOMPARE(removed.value(QStringLiteral("removed")).toBool(), true);
    QCOMPARE(removed.value(QStringLiteral("removed_at")).toString(), QStringLiteral("2026-10-26T10:00:00.000Z"));
    QCOMPARE(removed.value(QStringLiteral("revision")).toInt(), 2);
    // updated_at stays strictly after the stored one even with a clock behind.
    QCOMPARE(removed.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-26T11:00:00.001Z"));
    QCOMPARE(removed.value(QStringLiteral("last_active_at")).toString(), stored.value(QStringLiteral("last_active_at")).toString());
    QCOMPARE(removed.value(QStringLiteral("origin_id")).toString(), QStringLiteral("other-mac"));
    QCOMPARE(removed.value(QStringLiteral("phone_extension")).toInt(), 7);
    QVERIFY(AgentConversations::validationError(removed).isEmpty());
}

void AgentConversationsTest::testRecordsFor()
{
    const QJsonObject older = created(at(QStringLiteral("2026-10-26T09:00:00.000Z")));
    auto other = toolTouch(QStringLiteral("Newer"), QStringLiteral("app"));
    other.conversationId = QStringLiteral("11111111-2222-4333-8444-555555555555");
    const QJsonObject newer = AgentConversations::touch({}, other, origin, at(QStringLiteral("2026-10-26T10:00:00.000Z")), device).record;
    auto third = other;
    third.conversationId = QStringLiteral("22222222-2222-4333-8444-555555555555");
    const QJsonObject removed = AgentConversations::remove(AgentConversations::touch({}, third, origin, at(QStringLiteral("2026-10-26T11:00:00.000Z")), device).record, origin);
    auto elsewhere = other;
    elsewhere.projectId = QStringLiteral("source:docs-other");
    const QJsonObject otherProject = AgentConversations::touch({}, elsewhere, origin, at(QStringLiteral("2026-10-26T12:00:00.000Z")), device).record;
    QJsonObject deleted = AgentConversations::touch({}, [&] { auto t = other; t.conversationId = QStringLiteral("33333333-2222-4333-8444-555555555555"); return t; }(),
                                                    origin, at(QStringLiteral("2026-10-26T13:00:00.000Z")), device).record;
    deleted.insert(QStringLiteral("deleted"), true);
    QJsonObject projections{{QStringLiteral("logical_comment\ncomments\nc1"), QJsonObject{{QStringLiteral("project_id"), project}}}};
    for (const QJsonObject &record : {older, newer, removed, otherProject, deleted})
        projections.insert(AgentConversations::projectionKey(record.value(QStringLiteral("link_id")).toString()), record);
    const auto records = AgentConversations::recordsFor(projections, project);
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).value(QStringLiteral("title")).toString(), QStringLiteral("Newer"));
    QCOMPARE(records.at(1).value(QStringLiteral("link_id")).toString(), older.value(QStringLiteral("link_id")).toString());
    QVERIFY(AgentConversations::recordsFor(projections, QString()).isEmpty());
}

void AgentConversationsTest::testOnlyConversationChanges()
{
    const QJsonObject comment{{QStringLiteral("comment_id"), QStringLiteral("c1")}};
    const QJsonObject before{{QStringLiteral("logical_comment\ncomments\nc1"), comment}};
    QJsonObject after = before;
    after.insert(AgentConversations::projectionKey(QStringLiteral("x")), created());
    QVERIFY(AgentConversations::onlyConversationChanges(before, after));
    QVERIFY(AgentConversations::onlyConversationChanges(after, before));
    after.insert(QStringLiteral("logical_comment\ncomments\nc2"), comment);
    QVERIFY(!AgentConversations::onlyConversationChanges(before, after));
    QVERIFY(!AgentConversations::onlyConversationChanges(before, QJsonObject()));
}

void AgentConversationsTest::testEnqueue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    const QJsonObject first = created(at(QStringLiteral("2026-10-26T11:00:00.000Z")));
    const QJsonObject removed = AgentConversations::remove(first, origin, at(QStringLiteral("2026-10-26T11:01:00.000Z")));
    QString error;
    QVERIFY2(AgentConversations::enqueue(outbox, first, &error), qPrintable(error));
    QVERIFY2(AgentConversations::enqueue(outbox, removed, &error), qPrintable(error));
    const QList<QJsonObject> operations = outbox.operations(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 2);
    const QList<QJsonObject> payloads{first, removed};
    for (int index = 0; index < operations.size(); ++index) {
        const QJsonObject &operation = operations.at(index);
        QCOMPARE(operation.value(QStringLiteral("entity_type")).toString(), QStringLiteral("agent_conversation"));
        QCOMPARE(operation.value(QStringLiteral("collection")).toString(), QStringLiteral("agent-conversations"));
        QCOMPARE(operation.value(QStringLiteral("entity_id")).toString(), first.value(QStringLiteral("link_id")).toString());
        QCOMPARE(operation.value(QStringLiteral("base_revision")).toInteger(-1), qint64(index));
        QCOMPARE(operation.value(QStringLiteral("client_timestamp")).toString(), payloads.at(index).value(QStringLiteral("updated_at")).toString());
        QCOMPARE(operation.value(QStringLiteral("payload")).toObject(), payloads.at(index));
    }
    QJsonObject invalid = first;
    invalid.insert(QStringLiteral("link_id"), QString(64, QLatin1Char('0')));
    QVERIFY(!AgentConversations::enqueue(outbox, invalid, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(outbox.operations().size(), 2);
}

void AgentConversationsTest::testResolveProject()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docs = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral("docs-concepts/tex/grpo.tex")), "x"));
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral("docs-shared/notes.md")), "x"));
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral("current_pdfs/grpo.pdf")), "%PDF"));
    QVERIFY(writeFile(QDir(temporary.path()).filePath(QStringLiteral("outside.tex")), "x"));
    const auto sourceFile = [](const QString &id) { return QJsonObject{{QStringLiteral("id"), id}}; };
    const QJsonObject concepts{
        {QStringLiteral("id"), QStringLiteral("source:docs-concepts")},
        {QStringLiteral("name"), QStringLiteral("Concepts")},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("docs-concepts")},
                                                           {QStringLiteral("files"), QJsonArray{sourceFile(QStringLiteral("docs-concepts/tex/grpo.tex"))}}},
                                               QJsonObject{{QStringLiteral("id"), QStringLiteral("docs-shared")}, {QStringLiteral("files"), QJsonArray{}}}}},
        {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("grpo.pdf")}}}},
    };
    const QJsonObject papers{
        {QStringLiteral("id"), QStringLiteral("new:8b964c95-8bc0-4045-a38e-015d2172d7ca")},
        {QStringLiteral("name"), QStringLiteral("Papers")},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("docs-shared")}, {QStringLiteral("files"), QJsonArray{}}}}},
        {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}}}},
    };
    const QJsonObject twin{{QStringLiteral("id"), QStringLiteral("new:other")}, {QStringLiteral("name"), QStringLiteral("papers")}};
    // A Project known only from its manifest (folders not scanned yet).
    const QJsonObject manifestOnly{
        {QStringLiteral("id"), QStringLiteral("new:manifest")},
        {QStringLiteral("name"), QStringLiteral("Manifest")},
        {QStringLiteral("project_configuration"),
         QJsonObject{{QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("default/abc")}}}}}},
    };
    QCOMPARE(AgentConversations::resolveProject(QJsonArray{concepts, manifestOnly}, docs, QJsonObject{{QStringLiteral("source"), QStringLiteral("default/abc/notes.md")}})
                 .project.value(QStringLiteral("name")).toString(),
             QStringLiteral("Manifest"));
    const QJsonArray projects{concepts, papers};
    const auto resolve = [&](const QJsonArray &list, const QString &key, const QString &value) {
        return AgentConversations::resolveProject(list, docs, QJsonObject{{key, value}});
    };
    QCOMPARE(resolve(projects, QStringLiteral("project"), QStringLiteral("concepts")).project.value(QStringLiteral("id")).toString(), QStringLiteral("source:docs-concepts"));
    QCOMPARE(resolve(projects, QStringLiteral("project"), QStringLiteral("new:8b964c95-8bc0-4045-a38e-015d2172d7ca")).project.value(QStringLiteral("name")).toString(),
             QStringLiteral("Papers"));
    QCOMPARE(resolve(projects, QStringLiteral("project"), QStringLiteral("Nothing")).errorCode, QStringLiteral("project_not_found"));
    QCOMPARE(resolve(QJsonArray{concepts, papers, twin}, QStringLiteral("project"), QStringLiteral("Papers")).errorCode, QStringLiteral("ambiguous_project"));

    QCOMPARE(resolve(projects, QStringLiteral("source"), QStringLiteral("docs-concepts/tex/grpo.tex")).project.value(QStringLiteral("name")).toString(),
             QStringLiteral("Concepts"));
    QCOMPARE(resolve(projects, QStringLiteral("source"), QDir(docs).filePath(QStringLiteral("docs-concepts/tex/../tex/grpo.tex"))).project.value(QStringLiteral("name")).toString(),
             QStringLiteral("Concepts"));
    QCOMPARE(resolve(projects, QStringLiteral("source"), QStringLiteral("docs-shared/notes.md")).errorCode, QStringLiteral("ambiguous_project"));
    QCOMPARE(resolve(projects, QStringLiteral("source"), QDir(temporary.path()).filePath(QStringLiteral("outside.tex"))).errorCode, QStringLiteral("not_in_library"));
    QCOMPARE(resolve(projects, QStringLiteral("source"), QStringLiteral("docs-unknown/x.tex")).errorCode, QStringLiteral("not_in_library"));

    QCOMPARE(resolve(projects, QStringLiteral("document"), QStringLiteral("grpo.pdf")).project.value(QStringLiteral("name")).toString(), QStringLiteral("Concepts"));
    QCOMPARE(resolve(projects, QStringLiteral("document"), QDir(docs).filePath(QStringLiteral("current_pdfs/grpo.pdf"))).project.value(QStringLiteral("name")).toString(),
             QStringLiteral("Concepts"));
    QCOMPARE(resolve(projects, QStringLiteral("document"), QStringLiteral("paper.pdf")).project.value(QStringLiteral("name")).toString(), QStringLiteral("Papers"));
    QCOMPARE(resolve(projects, QStringLiteral("document"), QStringLiteral("/tmp/elsewhere.pdf")).errorCode, QStringLiteral("not_in_library"));
    QCOMPARE(resolve(projects, QStringLiteral("document"), QStringLiteral("missing.pdf")).errorCode, QStringLiteral("not_in_library"));

    QCOMPARE(AgentConversations::resolveProject(projects, docs, QJsonObject{}).errorCode, QStringLiteral("invalid_request"));
    QCOMPARE(AgentConversations::resolveProject(projects, docs, QJsonObject{{QStringLiteral("project"), QStringLiteral("Concepts")}, {QStringLiteral("document"), QStringLiteral("grpo.pdf")}})
                 .errorCode,
             QStringLiteral("invalid_request"));
}

void AgentConversationsTest::testProposalBackfill()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docs = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString codexHome = QDir(temporary.path()).filePath(QStringLiteral("codex"));
    const QString claudeHome = QDir(temporary.path()).filePath(QStringLiteral("claude"));
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral("docs-concepts/grpo.tex")), "x"));
    QVERIFY(writeFile(QDir(codexHome).filePath(QStringLiteral("session_index.jsonl")),
                      "{\"id\":\"" + codexId.toUtf8() + "\",\"thread_name\":\"Old name\"}\n{\"id\":\"" + codexId.toUtf8() + "\",\"thread_name\":\"GRPO notes\"}\n"));
    const QString transcriptId = QStringLiteral("44444444-2222-4333-8444-555555555555");
    QVERIFY(writeFile(QDir(claudeHome).filePath(QStringLiteral("projects/-Users-alex/") + transcriptId + QStringLiteral(".jsonl")), "{}\n"));
    const QJsonArray projects{QJsonObject{{QStringLiteral("id"), project},
                                          {QStringLiteral("name"), QStringLiteral("Concepts")},
                                          {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("docs-concepts")}}}}}};
    const auto proposal = [](const QString &id, const QString &conversation, const QString &url) {
        QJsonObject snapshot{{QStringLiteral("proposal_id"), id}, {QStringLiteral("source_file"), QStringLiteral("docs-concepts/grpo.tex")},
                             {QStringLiteral("conversation_id"), conversation}};
        if (!url.isEmpty()) snapshot.insert(QStringLiteral("conversation_url"), url);
        return QJsonObject{{QStringLiteral("proposal_id"), id}, {QStringLiteral("snapshot"), snapshot}};
    };
    const QJsonObject projections{
        {QStringLiteral("proposal\nproposals\np1"), proposal(QStringLiteral("p1"), codexId, QStringLiteral("codex://threads/") + codexId)},
        {QStringLiteral("proposal\nproposals\np2"), proposal(QStringLiteral("p2"), codexId, QStringLiteral("codex://threads/") + codexId)},
        {QStringLiteral("proposal\nproposals\np3"), proposal(QStringLiteral("p3"), claudeId, claudeUrl)},
        {QStringLiteral("proposal\nproposals\np4"), proposal(QStringLiteral("p4"), transcriptId, QString())},
        {QStringLiteral("proposal\nproposals\np5"), proposal(QStringLiteral("p5"), QStringLiteral("66666666-2222-4333-8444-555555555555"), QString())},
    };
    AgentConversations::BackfillLookup lookup;
    lookup.link = [](const QString &stored, const QString &) { return stored; };
    lookup.codexHome = codexHome;
    lookup.claudeHome = claudeHome;
    const auto touches = AgentConversations::proposalBackfill(projections, projects, docs, lookup);
    // One per conversation; p5 has neither a link nor a transcript here.
    QCOMPARE(touches.size(), 3);
    QHash<QString, AgentConversations::Touch> byId;
    for (const auto &touch : touches) byId.insert(touch.conversationId, touch);
    QCOMPARE(byId.value(codexId).agent, QStringLiteral("codex"));
    QCOMPARE(byId.value(codexId).title, QStringLiteral("GRPO notes"));
    QCOMPARE(byId.value(codexId).titleSource, QStringLiteral("app"));
    QCOMPARE(byId.value(claudeId).agent, QStringLiteral("claude"));
    QCOMPARE(byId.value(claudeId).conversationUrl, claudeUrl);
    QCOMPARE(byId.value(claudeId).title, QStringLiteral("Proposal conversation 5ef4cd0f"));
    QCOMPARE(byId.value(claudeId).titleSource, QStringLiteral("fallback"));
    QCOMPARE(byId.value(transcriptId).agent, QStringLiteral("claude"));
    QVERIFY(byId.value(transcriptId).conversationUrl.isEmpty());
    for (const auto &touch : touches) {
        QCOMPARE(touch.projectId, project);
        QCOMPARE(touch.linkedBy, QStringLiteral("backfill"));
        const auto record = AgentConversations::touch({}, touch, origin, QDateTime::currentDateTimeUtc(), device).record;
        QVERIFY2(AgentConversations::validationError(record).isEmpty(), qPrintable(AgentConversations::validationError(record)));
        QCOMPARE(record.value(QStringLiteral("linked_by")).toString(), QStringLiteral("backfill"));
    }
}

void AgentConversationsTest::testCardEntryCreatedWithTheRecord()
{
    const auto result = AgentConversations::touch({}, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:46:58.120Z")), device);
    QVERIFY(result.written);
    QVERIFY(result.cardWritten);
    QCOMPARE(result.record.value(QStringLiteral("review_cards")).toArray().size(), 1);
    QCOMPARE(cardEntry(result.record), QJsonObject({{QStringLiteral("card_id"), cardId},
                                                     {QStringLiteral("linked_at"), QStringLiteral("2026-10-26T17:46:58.120Z")},
                                                     {QStringLiteral("last_active_at"), QStringLiteral("2026-10-26T17:46:58.120Z")},
                                                     {QStringLiteral("linked_by"), QStringLiteral("prompt")},
                                                     {QStringLiteral("removed"), false}}));
    QVERIFY(AgentConversations::validationError(result.record).isEmpty());
    // Without a card (or with a card id that is no UUID) there is no review_cards field.
    QVERIFY(!created().contains(QStringLiteral("review_cards")));
    auto bad = cardTouch();
    bad.reviewCardId = QStringLiteral("not-a-card");
    QVERIFY(!AgentConversations::touch({}, bad, origin, at(QStringLiteral("2026-10-26T17:46:58.120Z")), device).record.contains(QStringLiteral("review_cards")));
}

void AgentConversationsTest::testCardEntryOnAnExistingRecord()
{
    // A fresh Project link (inside its throttle) still writes for a new card entry; unknown fields stay.
    QJsonObject stored = created(at(QStringLiteral("2026-10-26T17:00:00.000Z")));
    stored.insert(QStringLiteral("review_cards"), QJsonArray{QJsonObject{{QStringLiteral("card_id"), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848")},
                                                                       {QStringLiteral("linked_at"), QStringLiteral("2026-10-26T16:00:00.000Z")},
                                                                       {QStringLiteral("last_active_at"), QStringLiteral("2026-10-26T16:00:00.000Z")},
                                                                       {QStringLiteral("linked_by"), QStringLiteral("tool")},
                                                                       {QStringLiteral("removed"), false},
                                                                       {QStringLiteral("phone_extension"), 1}},
                                                           QStringLiteral("malformed")});
    const auto result = AgentConversations::touch(stored, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:01:00.000Z")), device);
    QVERIFY(result.written);
    QVERIFY(result.cardWritten);
    QCOMPARE(result.record.value(QStringLiteral("revision")).toInt(), 2);
    const QJsonArray cards = result.record.value(QStringLiteral("review_cards")).toArray();
    QCOMPARE(cards.size(), 3);
    QCOMPARE(cards.at(0).toObject().value(QStringLiteral("phone_extension")).toInt(), 1);
    QCOMPARE(cards.at(1).toString(), QStringLiteral("malformed"));
    QCOMPARE(cardEntry(result.record).value(QStringLiteral("linked_by")).toString(), QStringLiteral("prompt"));
    // Project-level fields follow the Project rule unchanged.
    QCOMPARE(result.record.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-26T17:00:00.000Z"));
    QCOMPARE(result.record.value(QStringLiteral("last_tool")).toString(), QStringLiteral("prompt"));
}

void AgentConversationsTest::testCardEntryThrottle()
{
    const QJsonObject first = AgentConversations::touch({}, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:00:00.000Z")), device).record;
    // Within 600 s nothing changes: nothing is written.
    auto again = cardTouch(QStringLiteral("tool"));
    const auto soon = AgentConversations::touch(first, again, origin, at(QStringLiteral("2026-10-26T17:09:59.000Z")), device);
    QVERIFY(!soon.written);
    QVERIFY(!soon.cardWritten);
    QCOMPARE(soon.record, first);
    // After 600 s the entry's last_active_at moves; linked_at and linked_by stay as first written.
    const auto later = AgentConversations::touch(first, again, origin, at(QStringLiteral("2026-10-26T17:10:00.000Z")), device);
    QVERIFY(later.written);
    QVERIFY(later.cardWritten);
    const QJsonObject entry = cardEntry(later.record);
    QCOMPARE(entry.value(QStringLiteral("last_active_at")).toString(), QStringLiteral("2026-10-26T17:10:00.000Z"));
    QCOMPARE(entry.value(QStringLiteral("linked_at")).toString(), QStringLiteral("2026-10-26T17:00:00.000Z"));
    QCOMPARE(entry.value(QStringLiteral("linked_by")).toString(), QStringLiteral("prompt"));
    // last_active_at never goes back (a clock behind).
    const auto behind = AgentConversations::touch(later.record, again, origin, at(QStringLiteral("2026-10-26T17:30:00.000Z")).addDays(-1), device);
    QVERIFY(!behind.cardWritten || cardEntry(behind.record).value(QStringLiteral("last_active_at")).toString() == QStringLiteral("2026-10-26T17:10:00.000Z"));
}

void AgentConversationsTest::testCardRemovalIsStickyExceptForTheAgent()
{
    const QJsonObject first = AgentConversations::touch({}, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:00:00.000Z")), device).record;
    const QJsonObject removed = AgentConversations::removeFromCard(first, cardId, origin, at(QStringLiteral("2026-10-26T17:01:00.000Z")));
    for (const QString &linkedBy : {QStringLiteral("prompt"), QStringLiteral("tool")}) {
        const auto result = AgentConversations::touch(removed, cardTouch(linkedBy), origin, at(QStringLiteral("2026-10-26T18:00:00.000Z")), device);
        QVERIFY(!result.cardWritten);
        QCOMPARE(cardEntry(result.record).value(QStringLiteral("removed")).toBool(), true);
        // The Project record still follows its own throttle (it is an hour old).
        QVERIFY(result.written);
    }
    const auto revived = AgentConversations::touch(removed, cardTouch(QStringLiteral("agent")), origin, at(QStringLiteral("2026-10-26T17:02:00.000Z")), device);
    QVERIFY(revived.written);
    QVERIFY(revived.cardWritten);
    QCOMPARE(cardEntry(revived.record).value(QStringLiteral("removed")).toBool(true), false);
    QCOMPARE(cardEntry(revived.record).value(QStringLiteral("linked_at")).toString(), QStringLiteral("2026-10-26T17:00:00.000Z"));
}

void AgentConversationsTest::testCardEntryOnAProjectRemovedRecord()
{
    const QJsonObject removed = AgentConversations::remove(created(at(QStringLiteral("2026-10-26T17:00:00.000Z"))), origin, at(QStringLiteral("2026-10-26T17:01:00.000Z")));
    // A card touch still records the card, but the Project-level removal stays.
    const auto result = AgentConversations::touch(removed, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:02:00.000Z")), device);
    QVERIFY(result.written);
    QVERIFY(result.removed);
    QVERIFY(result.cardWritten);
    QCOMPARE(result.record.value(QStringLiteral("removed")).toBool(), true);
    QCOMPARE(result.record.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(result.record.value(QStringLiteral("title")).toString(), removed.value(QStringLiteral("title")).toString());
    QVERIFY(AgentConversations::validationError(result.record).isEmpty());
    // Hidden from the Project, shown on the card.
    QJsonObject projections{{AgentConversations::projectionKey(result.record.value(QStringLiteral("link_id")).toString()), result.record}};
    QVERIFY(AgentConversations::recordsFor(projections, project).isEmpty());
    QCOMPARE(AgentConversations::recordsForCard(projections, cardId).size(), 1);
    // Again within the throttle: nothing.
    const auto again = AgentConversations::touch(result.record, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:03:00.000Z")), device);
    QVERIFY(!again.written);
    QVERIFY(again.removed);
    // A Project-only touch writes nothing, as before.
    QVERIFY(!AgentConversations::touch(result.record, toolTouch(), origin, at(QStringLiteral("2026-10-27T17:03:00.000Z")), device).written);
}

void AgentConversationsTest::testCardEntryLimit()
{
    QJsonObject stored = created(at(QStringLiteral("2026-10-26T17:00:00.000Z")));
    QJsonArray cards;
    for (int i = 0; i < AgentConversations::cardEntryLimit; ++i) {
        const QString time = QDateTime(QDate(2026, 10, 1), QTime(0, 0)).addSecs(i * 60).toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) + QLatin1Char('Z');
        cards.append(QJsonObject{{QStringLiteral("card_id"), QStringLiteral("%1-0000-4000-8000-000000000000").arg(i, 8, 16, QLatin1Char('0'))},
                                 {QStringLiteral("linked_at"), time},
                                 {QStringLiteral("last_active_at"), time},
                                 {QStringLiteral("linked_by"), QStringLiteral("tool")},
                                 {QStringLiteral("removed"), i == 150}});
    }
    stored.insert(QStringLiteral("review_cards"), cards);
    // Full: the removed entry goes first, even though others were active longer ago.
    auto result = AgentConversations::touch(stored, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:01:00.000Z")), device);
    QJsonArray after = result.record.value(QStringLiteral("review_cards")).toArray();
    QCOMPARE(after.size(), AgentConversations::cardEntryLimit);
    QVERIFY(cardEntry(result.record, QStringLiteral("00000096-0000-4000-8000-000000000000")).isEmpty());
    QVERIFY(!cardEntry(result.record, QStringLiteral("00000000-0000-4000-8000-000000000000")).isEmpty());
    QVERIFY(!cardEntry(result.record).isEmpty());
    // Then the one active longest ago.
    auto next = cardTouch();
    next.reviewCardId = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
    result = AgentConversations::touch(result.record, next, origin, at(QStringLiteral("2026-10-26T17:02:00.000Z")), device);
    QCOMPARE(result.record.value(QStringLiteral("review_cards")).toArray().size(), AgentConversations::cardEntryLimit);
    QVERIFY(cardEntry(result.record, QStringLiteral("00000000-0000-4000-8000-000000000000")).isEmpty());
    QVERIFY(!cardEntry(result.record, QStringLiteral("00000001-0000-4000-8000-000000000000")).isEmpty());
    QVERIFY(!cardEntry(result.record).isEmpty());
}

void AgentConversationsTest::testRemoveFromCard()
{
    QJsonObject stored = AgentConversations::touch({}, cardTouch(), origin, at(QStringLiteral("2026-10-26T17:00:00.000Z")), device).record;
    stored.insert(QStringLiteral("phone_extension"), 3);
    const QJsonObject removed = AgentConversations::removeFromCard(stored, cardId, QStringLiteral("other-mac"), at(QStringLiteral("2026-10-26T16:00:00.000Z")));
    QCOMPARE(cardEntry(removed).value(QStringLiteral("removed")).toBool(), true);
    QCOMPARE(removed.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(removed.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-26T17:00:00.001Z"));
    QCOMPARE(removed.value(QStringLiteral("origin_id")).toString(), QStringLiteral("other-mac"));
    QCOMPARE(removed.value(QStringLiteral("removed")).toBool(true), false);
    QCOMPARE(removed.value(QStringLiteral("last_active_at")).toString(), stored.value(QStringLiteral("last_active_at")).toString());
    QCOMPARE(removed.value(QStringLiteral("phone_extension")).toInt(), 3);
    QVERIFY(AgentConversations::validationError(removed).isEmpty());
    // No such entry, or already removed: unchanged.
    QCOMPARE(AgentConversations::removeFromCard(removed, cardId, origin), removed);
    QCOMPARE(AgentConversations::removeFromCard(stored, QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), origin), stored);
}

void AgentConversationsTest::testRecordsForCard()
{
    // Three conversations on the card: the newest card activity first, whatever the records' own dates.
    const auto make = [](const QString &conversation, const QString &projectId, const QString &time) {
        auto touch = cardTouch();
        touch.conversationId = conversation;
        touch.projectId = projectId;
        return AgentConversations::touch({}, touch, origin, at(time), device).record;
    };
    const QJsonObject older = make(QStringLiteral("11111111-2222-4333-8444-555555555555"), project, QStringLiteral("2026-10-26T10:00:00.000Z"));
    QJsonObject newer = make(QStringLiteral("22222222-2222-4333-8444-555555555555"), project, QStringLiteral("2026-10-26T09:00:00.000Z"));
    // Its card entry was touched later than the other's.
    {
        auto touch = cardTouch();
        touch.conversationId = QStringLiteral("22222222-2222-4333-8444-555555555555");
        newer = AgentConversations::touch(newer, touch, origin, at(QStringLiteral("2026-10-26T12:00:00.000Z")), device).record;
    }
    // A card moved to another Project keeps its links there: listed too.
    const QJsonObject moved = make(QStringLiteral("33333333-2222-4333-8444-555555555555"), QStringLiteral("source:docs-other"), QStringLiteral("2026-10-26T11:00:00.000Z"));
    const QJsonObject removedFromCard = AgentConversations::removeFromCard(make(QStringLiteral("44444444-2222-4333-8444-555555555555"), project, QStringLiteral("2026-10-26T13:00:00.000Z")), cardId, origin);
    QJsonObject deleted = make(QStringLiteral("55555555-2222-4333-8444-555555555555"), project, QStringLiteral("2026-10-26T14:00:00.000Z"));
    deleted.insert(QStringLiteral("deleted"), true);
    const QJsonObject projectOnly = created(at(QStringLiteral("2026-10-26T15:00:00.000Z")));
    QJsonObject malformed = make(QStringLiteral("66666666-2222-4333-8444-555555555555"), project, QStringLiteral("2026-10-26T16:00:00.000Z"));
    malformed.insert(QStringLiteral("review_cards"), QJsonArray{QJsonObject{{QStringLiteral("card_id"), cardId}, {QStringLiteral("last_active_at"), QStringLiteral("yesterday")}}});
    QJsonObject projections;
    for (const QJsonObject &record : {older, newer, moved, removedFromCard, deleted, projectOnly, malformed})
        projections.insert(AgentConversations::projectionKey(record.value(QStringLiteral("link_id")).toString()), record);
    const auto list = AgentConversations::recordsForCard(projections, cardId);
    QCOMPARE(list.size(), 3);
    QCOMPARE(list.at(0).record.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("22222222-2222-4333-8444-555555555555"));
    QCOMPARE(list.at(1).record.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("33333333-2222-4333-8444-555555555555"));
    QCOMPARE(list.at(2).record.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("11111111-2222-4333-8444-555555555555"));
    QCOMPARE(list.at(0).entry.value(QStringLiteral("card_id")).toString(), cardId);
    QVERIFY(AgentConversations::recordsForCard(projections, QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848")).isEmpty());
    QVERIFY(AgentConversations::recordsForCard(projections, QString()).isEmpty());
}

QTEST_GUILESS_MAIN(AgentConversationsTest)
#include "agentconversationstest.moc"
