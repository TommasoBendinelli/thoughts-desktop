// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/agentsessionsettings.h"
#include "../shell/agentnotes.h"
#include "../shell/sessionstarter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <memory>

using namespace Thoughts;
using namespace Thoughts::SessionStarts;

namespace
{
QString iso(const QDateTime &time)
{
    return time.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz000Z"));
}

// A page of the pages feed as the server writes it (protocol/agent-notes.md, "Note record"): the
// phone's Start a new study session.
QJsonObject pageRecord(const QString &id, qint64 sequence, const QDateTime &created, const QString &prompt, const QJsonObject &extraOrigin = {})
{
    QJsonObject origin {{QStringLiteral("kind"), QStringLiteral("phone_page")},
                        {QStringLiteral("device"), QStringLiteral("phone")},
                        {QStringLiteral("start_session"), true},
                        {QStringLiteral("copy_for_agent"), true}};
    for (auto it = extraOrigin.constBegin(); it != extraOrigin.constEnd(); ++it) origin.insert(it.key(), it.value());
    return {{QStringLiteral("note_id"), id},
            {QStringLiteral("sequence"), double(sequence)},
            {QStringLiteral("created_at"), iso(created)},
            {QStringLiteral("sender_device_id"), QStringLiteral("phone")},
            {QStringLiteral("title"), QStringLiteral("Start a study session")},
            {QStringLiteral("message"), prompt},
            {QStringLiteral("media_type"), QStringLiteral("image/png")},
            {QStringLiteral("image_width"), 1200},
            {QStringLiteral("image_height"), 400},
            {QStringLiteral("image_bytes"), 100},
            {QStringLiteral("image_sha256"), QString(64, QLatin1Char('0'))},
            {QStringLiteral("image_url"), QStringLiteral("/v1/agent-notes/%1/image").arg(id)},
            {QStringLiteral("origin"), origin},
            {QStringLiteral("deleted"), false},
            {QStringLiteral("replies"), QJsonArray()}};
}

AgentNotes::Note page(const QJsonObject &record)
{
    return *AgentNotes::Note::fromJson(record);
}

// The Thoughts server as far as the session starter needs it: the pages feed (a request with the
// newest cursor is held until a page comes, as the server waits), handling records, claims (the
// first wins) and new notes.
class FakeServer : public QObject
{
public:
    FakeServer()
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qsizetype length = 0;
                    for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    }
                    if (buffer.size() < headerEnd + 4 + length) return;
                    const QByteArray head = buffer.left(headerEnd + 4);
                    const QByteArray body = buffer.mid(headerEnd + 4, length);
                    m_buffers.remove(socket);
                    answer(socket, head, body);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    QByteArray token = "device-token";
    qint64 cursor = 7;
    QJsonArray pages;  // oldest first
    // page id -> the claim another computer made (handling's "claim", and every later claim is 409).
    QHash<QString, QJsonObject> claims;
    // page id -> the claim is refused although handling said nobody had it (two computers racing).
    QSet<QString> claimRaces;
    QStringList requests;  // "GET /v1/agent-notes/pages?after=7&wait=25"
    QList<QJsonObject> claimBodies;
    QList<QJsonObject> notes;  // POST /v1/agent-notes bodies

    int waitingPolls() const
    {
        int count = 0;
        for (const auto &held : m_held) count += held.first ? 1 : 0;
        return count;
    }
    // A new page: the held long polls get it.
    void addPage(const QJsonObject &record)
    {
        pages.append(record);
        cursor = qMax(cursor, qint64(record.value(QStringLiteral("sequence")).toDouble()));
        const auto held = std::exchange(m_held, {});
        for (const auto &waiting : held) {
            if (waiting.first) reply(waiting.first, 200, pagesAfter(waiting.second));
        }
    }

private:
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<std::pair<QPointer<QTcpSocket>, qint64>> m_held;

    QByteArray pagesAfter(qint64 after) const
    {
        QJsonArray answered;
        qint64 last = after;
        for (const QJsonValue &value : pages) {
            const qint64 sequence = qint64(value.toObject().value(QStringLiteral("sequence")).toDouble());
            if (sequence <= after) continue;
            answered.append(value);
            last = qMax(last, sequence);
        }
        return QJsonDocument(QJsonObject {{QStringLiteral("notes"), answered}, {QStringLiteral("cursor"), double(last)}}).toJson(QJsonDocument::Compact);
    }

    void reply(QTcpSocket *socket, int code, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void answer(QTcpSocket *socket, const QByteArray &head, const QByteArray &requestBody)
    {
        const QList<QByteArray> words = head.left(head.indexOf("\r\n")).split(' ');
        const QString method = QString::fromLatin1(words.value(0));
        const QUrl target(QString::fromLatin1(words.value(1)));
        requests.append(method + QLatin1Char(' ') + target.toString());
        if (!head.contains(QByteArray("Authorization: Bearer " + token))) {
            reply(socket, 401, R"({"detail":"invalid token"})");
            return;
        }
        const QString path = target.path();
        const QJsonObject body = QJsonDocument::fromJson(requestBody).object();
        if (method == QLatin1String("GET") && path == QLatin1String("/v1/agent-notes/pages")) {
            const QUrlQuery query(target);
            if (!query.hasQueryItem(QStringLiteral("after"))) {
                reply(socket, 200, QJsonDocument(QJsonObject {{QStringLiteral("notes"), QJsonArray()}, {QStringLiteral("cursor"), double(cursor)}}).toJson(QJsonDocument::Compact));
                return;
            }
            const qint64 after = query.queryItemValue(QStringLiteral("after")).toLongLong();
            if (after >= cursor) {
                m_held.append({QPointer<QTcpSocket>(socket), after});
                return;
            }
            reply(socket, 200, pagesAfter(after));
            return;
        }
        if (method == QLatin1String("GET") && path.startsWith(QLatin1String("/v1/agent-notes/handling/"))) {
            const QString id = path.section(QLatin1Char('/'), -1);
            const QJsonValue claim = claims.contains(id) ? QJsonValue(claims.value(id)) : QJsonValue();
            reply(socket, 200,
                  QJsonDocument(QJsonObject {{QStringLiteral("item_id"), id}, {QStringLiteral("kind"), QStringLiteral("page")}, {QStringLiteral("note_id"), id},
                                             {QStringLiteral("covered_by"), QJsonArray()}, {QStringLiteral("claim"), claim}})
                      .toJson(QJsonDocument::Compact));
            return;
        }
        if (method == QLatin1String("POST") && path.startsWith(QLatin1String("/v1/agent-notes/handling/")) && path.endsWith(QLatin1String("/claim"))) {
            const QString id = path.section(QLatin1Char('/'), -2, -2);
            claimBodies.append(body);
            if (claims.contains(id) || claimRaces.contains(id)) {
                const QJsonObject other = claims.value(id, QJsonObject {{QStringLiteral("device_id"), QStringLiteral("other-computer")},
                                                                        {QStringLiteral("conversation_id"), QStringLiteral("their-session")}});
                reply(socket, 409,
                      QJsonDocument(QJsonObject {{QStringLiteral("error_code"), QStringLiteral("agent_note_claimed")}, {QStringLiteral("detail"), QStringLiteral("another agent has it")},
                                                 {QStringLiteral("item_id"), id}, {QStringLiteral("claim"), other}})
                          .toJson(QJsonDocument::Compact));
                return;
            }
            claims.insert(id, {{QStringLiteral("device_id"), QStringLiteral("this-computer")}, {QStringLiteral("conversation_id"), body.value(QStringLiteral("conversation_id"))}});
            reply(socket, 201, QJsonDocument(QJsonObject {{QStringLiteral("item_id"), id}, {QStringLiteral("claim"), claims.value(id)}}).toJson(QJsonDocument::Compact));
            return;
        }
        if (method == QLatin1String("POST") && path == QLatin1String("/v1/agent-notes")) {
            notes.append(body);
            reply(socket, 201, QJsonDocument(QJsonObject {{QStringLiteral("note"), QJsonObject {{QStringLiteral("note_id"), body.value(QStringLiteral("note_id"))}}}}).toJson(QJsonDocument::Compact));
            return;
        }
        reply(socket, 404, R"({"detail":"Not Found"})");
    }
};

// Claude Code as far as the starter uses it. Every call writes its arguments (NUL-separated) and
// folder, and appends "<pid> <first argument>" to calls. FAKE_CLAUDE_MODE: signedout, untrusted.
const char fakeClaudeScript[] = R"(#!/bin/sh
dir="$FAKE_AGENT_DIR"
printf '%s\0' "$@" > "$dir/argv-$$"
pwd -P > "$dir/cwd-$$"
echo "$$ $1" >> "$dir/calls"
case "$1" in
    auth)
        if [ "$FAKE_CLAUDE_MODE" = signedout ]; then echo '{"loggedIn": false}'; else echo '{"loggedIn": true, "authMethod": "claude.ai"}'; fi
        exit 0 ;;
    agents)
        echo '[{"pid": 1, "cwd": "/elsewhere", "kind": "interactive", "sessionId": "11111111-2222-4333-8444-555555555555", "name": "other", "status": "idle"},'
        echo ' {"pid": 2, "cwd": "/here", "kind": "background", "sessionId": "1a2b3c4d-0000-4000-8000-000000000001", "name": "Study", "status": "busy"}]'
        exit 0 ;;
    stop|rm)
        exit 0 ;;
    --bg)
        if [ "$FAKE_CLAUDE_MODE" = untrusted ]; then
            echo "Workspace not trusted. Run \`claude\` in $PWD once and accept the trust prompt, then retry." >&2
            exit 1
        fi
        echo "backgrounded · 1a2b3c4d · $3"
        exit 0 ;;
esac
exit 2
)";

// Codex: login status, and exec, which reads its prompt from standard input and names its thread.
const char fakeCodexScript[] = R"(#!/bin/sh
dir="$FAKE_AGENT_DIR"
printf '%s\0' "$@" > "$dir/argv-$$"
pwd -P > "$dir/cwd-$$"
echo "$$ $1" >> "$dir/calls"
case "$1" in
    login)
        echo "Logged in using ChatGPT"
        exit 0 ;;
    exec)
        cat > "$dir/stdin-$$"
        echo '{"type":"thread.started","thread_id":"0199aaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee"}'
        echo '{"type":"turn.started"}'
        exit 0 ;;
esac
exit 2
)";
}

class SessionStarterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void decidesWhichPagesToActOn_data();
    void decidesWhichPagesToActOn();
    void consentAndAnswers();
    void phoneStartSetting();
    void namesAndHeaders();
    void commandLines();
    void readsClaudeAnswers();
    void readsCodexThread();
    void classifiesClaudeFailures();
    void agentEnvironmentKeepsOwnSignIn();
    void noticeImageIsAPicture();

    void startsClaudeSessionFromPhonePage();
    void anotherComputerWinsTheClaim();
    void alreadyClaimedPageIsLeftAlone();
    void untrustedFolderIsExplained();
    void missingClaudeIsExplained();
    void signedOutClaudeIsExplained();
    void neverAndOldPagesStartNothing();
    void questionAlwaysThenStartsWithoutAsking();
    void questionNotHereDeclinesForGood();
    void startsCodexSession();

private:
    struct Fixture {
        std::unique_ptr<FakeServer> server;
        AgentNotes::Client *client = nullptr;
        std::unique_ptr<SessionStarter> starter;
        std::unique_ptr<QSignalSpy> finished;
    };
    Fixture start(SessionStarter::Options options);
    SessionStarter::Options options() const;
    // The calls a fake agent got, in order: {pid, first argument}.
    QList<std::pair<QString, QString>> calls() const;
    QStringList argv(const QString &pid) const;
    QString cwd(const QString &pid) const;
    QString callOf(const QString &first) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_claude;
    QString m_codex;
    QString m_agents;
};

void SessionStarterTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
#ifdef Q_OS_WIN
    // The fake claude and codex are shell scripts.
#endif
}

void SessionStarterTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    const QDir root(m_dir->path());
    m_agents = root.filePath(QStringLiteral("agent-calls"));
    QVERIFY(QDir().mkpath(m_agents));
    m_claude = root.filePath(QStringLiteral("bin/claude"));
    m_codex = root.filePath(QStringLiteral("bin/codex"));
    QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("bin"))));
    for (const auto &[path, script] : {std::pair<QString, const char *>(m_claude, fakeClaudeScript), std::pair<QString, const char *>(m_codex, fakeCodexScript)}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(script);
        file.close();
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    qputenv("FAKE_AGENT_DIR", m_agents.toUtf8());
    qunsetenv("FAKE_CLAUDE_MODE");
}

void SessionStarterTest::cleanup()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMessageBox *>(widget)) delete widget;
    }
    m_dir.reset();
}

SessionStarter::Options SessionStarterTest::options() const
{
    SessionStarter::Options options;
    options.folder = QDir(m_dir->path()).filePath(QStringLiteral("Thoughts Sessions"));
    options.statePath = QDir(m_dir->path()).filePath(QStringLiteral("state/state.json"));
    options.computerName = QStringLiteral("Test Mac");
    options.claudeExecutable = m_claude;
    options.codexExecutable = m_codex;
    options.consent = AgentSessions::PhoneStart::Always;
    options.agentsIntervalMs = 20;
    options.untrusted = [](const QString &, const QString &) {};
    return options;
}

SessionStarterTest::Fixture SessionStarterTest::start(SessionStarter::Options options)
{
    Fixture fixture;
    fixture.server = std::make_unique<FakeServer>();
    fixture.client = new AgentNotes::Client(QString());
    fixture.client->setServer(fixture.server->url(), fixture.server->token);
    fixture.starter = std::make_unique<SessionStarter>(fixture.client, std::move(options));
    fixture.client->setParent(fixture.starter.get());
    fixture.finished = std::make_unique<QSignalSpy>(fixture.starter.get(), &SessionStarter::pageFinished);
    fixture.starter->start();
    // The first answer says where the newest page is; the next request waits for a new one.
    const FakeServer *server = fixture.server.get();
    [&] { QTRY_COMPARE_WITH_TIMEOUT(server->waitingPolls(), 1, 10000); }();
    return fixture;
}

QList<std::pair<QString, QString>> SessionStarterTest::calls() const
{
    QList<std::pair<QString, QString>> result;
    QFile file(QDir(m_agents).filePath(QStringLiteral("calls")));
    if (!file.open(QIODevice::ReadOnly)) return result;
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        const QList<QByteArray> words = line.trimmed().split(' ');
        result.append({QString::fromUtf8(words.value(0)), QString::fromUtf8(words.value(1))});
    }
    return result;
}

QStringList SessionStarterTest::argv(const QString &pid) const
{
    QFile file(QDir(m_agents).filePath(QStringLiteral("argv-") + pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    if (bytes.endsWith('\0')) bytes.chop(1);
    QStringList arguments;
    for (const QByteArray &argument : bytes.split('\0')) arguments.append(QString::fromUtf8(argument));
    return arguments;
}

QString SessionStarterTest::cwd(const QString &pid) const
{
    QFile file(QDir(m_agents).filePath(QStringLiteral("cwd-") + pid));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString SessionStarterTest::callOf(const QString &first) const
{
    for (const auto &[pid, argument] : calls()) {
        if (argument == first) return pid;
    }
    return {};
}

// ---------------------------------------------------------------- pure parts

void SessionStarterTest::decidesWhichPagesToActOn_data()
{
    QTest::addColumn<QJsonObject>("record");
    QTest::addColumn<bool>("handled");
    QTest::addColumn<bool>("act");
    QTest::addColumn<bool>("startSession");
    QTest::addColumn<int>("agent");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QTest::newRow("claude by default") << pageRecord(QStringLiteral("p1"), 1, now.addSecs(-60), QStringLiteral("Study")) << false << true << true << int(Agent::Claude);
    QTest::newRow("claude named") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("agent"), QStringLiteral("claude")}}) << false << true
                                  << true << int(Agent::Claude);
    QTest::newRow("codex") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("agent"), QStringLiteral("Codex")}}) << false << true << true
                           << int(Agent::Codex);
    QTest::newRow("unknown agent") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("agent"), QStringLiteral("gemini")}}) << false
                                   << false << true << int(Agent::Claude);
    QTest::newRow("14 minutes old") << pageRecord(QStringLiteral("p1"), 1, now.addSecs(-14 * 60), QStringLiteral("Study")) << false << true << true << int(Agent::Claude);
    QTest::newRow("16 minutes old") << pageRecord(QStringLiteral("p1"), 1, now.addSecs(-16 * 60), QStringLiteral("Study")) << false << false << true << int(Agent::Claude);
    QTest::newRow("handled before") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study")) << true << false << true << int(Agent::Claude);
    QJsonObject deleted = pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"));
    deleted.insert(QStringLiteral("deleted"), true);
    QTest::newRow("deleted") << deleted << false << false << true << int(Agent::Claude);
    QTest::newRow("start_session false") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("start_session"), false}}) << false << false
                                         << false << int(Agent::Claude);
    QTest::newRow("start_session a string") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("start_session"), QStringLiteral("true")}})
                                            << false << false << false << int(Agent::Claude);
    QTest::newRow("an agent's note") << pageRecord(QStringLiteral("p1"), 1, now, QStringLiteral("Study"), {{QStringLiteral("kind"), QStringLiteral("session_started")}}) << false
                                     << false << false << int(Agent::Claude);
}

void SessionStarterTest::decidesWhichPagesToActOn()
{
    QFETCH(QJsonObject, record);
    QFETCH(bool, handled);
    QFETCH(bool, act);
    QFETCH(bool, startSession);
    QFETCH(int, agent);
    QSet<QString> seen;
    if (handled) seen.insert(QStringLiteral("p1"));
    const PageDecision decision = decidePage(page(record), QDateTime::currentDateTimeUtc(), seen);
    QCOMPARE(decision.act, act);
    QCOMPARE(decision.startSession, startSession);
    if (act) QCOMPARE(int(decision.agent), agent);
    else QVERIFY(!decision.why.isEmpty());
}

void SessionStarterTest::consentAndAnswers()
{
    QCOMPARE(consentFor(AgentSessions::PhoneStart::Ask), Consent::Ask);
    QCOMPARE(consentFor(AgentSessions::PhoneStart::Always), Consent::Start);
    QCOMPARE(consentFor(AgentSessions::PhoneStart::Never), Consent::Decline);

    QVERIFY(outcomeOf(AskAnswer::Start).start);
    QVERIFY(!outcomeOf(AskAnswer::Start).newChoice);
    QVERIFY(outcomeOf(AskAnswer::Always).start);
    QCOMPARE(*outcomeOf(AskAnswer::Always).newChoice, AgentSessions::PhoneStart::Always);
    QVERIFY(!outcomeOf(AskAnswer::NotHere).start);
    QCOMPARE(*outcomeOf(AskAnswer::NotHere).newChoice, AgentSessions::PhoneStart::Never);
    // Closing the question decides nothing for later.
    QVERIFY(!outcomeOf(AskAnswer::Dismissed).start);
    QVERIFY(!outcomeOf(AskAnswer::Dismissed).newChoice);
}

void SessionStarterTest::phoneStartSetting()
{
    using AgentSessions::PhoneStart;
    QCOMPARE(AgentSessions::phoneStartFromString(QString()), PhoneStart::Ask);
    QCOMPARE(AgentSessions::phoneStartFromString(QStringLiteral("Always")), PhoneStart::Always);
    QCOMPARE(AgentSessions::phoneStartFromString(QStringLiteral("never")), PhoneStart::Never);
    QCOMPARE(AgentSessions::phoneStartFromString(QStringLiteral("sometimes")), PhoneStart::Ask);
    for (PhoneStart choice : {PhoneStart::Ask, PhoneStart::Always, PhoneStart::Never}) {
        QCOMPARE(AgentSessions::phoneStartFromString(AgentSessions::phoneStartToString(choice)), choice);
    }
    // Saved in the app's configuration (test mode: the test's own), Ask as no entry at all.
    AgentSessions::setPhoneStartChoice(PhoneStart::Never);
    QCOMPARE(AgentSessions::phoneStartChoice(), PhoneStart::Never);
    AgentSessions::setPhoneStartChoice(PhoneStart::Always);
    QCOMPARE(AgentSessions::phoneStartChoice(), PhoneStart::Always);
    AgentSessions::setPhoneStartChoice(PhoneStart::Ask);
    QCOMPARE(AgentSessions::phoneStartChoice(), PhoneStart::Ask);
    QVERIFY(!KConfigGroup(KSharedConfig::openConfig(), AgentSessions::configGroup()).hasKey(AgentSessions::phoneStartKey()));
    QCOMPARE(AgentSessions::sessionsFolder(), QDir::home().filePath(QStringLiteral("Thoughts Sessions")));
}

void SessionStarterTest::namesAndHeaders()
{
    const QDateTime local(QDate(2026, 11, 6), QTime(9, 5));
    QCOMPARE(sessionName(local), QStringLiteral("Study session · 6 Nov 09:05"));
    const QString header = promptHeader(Agent::Claude, QStringLiteral("Alex's Mac"), local, QStringLiteral("page-1"));
    QVERIFY(header.startsWith(QLatin1Char('[')));
    QVERIFY(header.endsWith(QLatin1Char(']')));
    QVERIFY(header.contains(QStringLiteral("Alex's Mac")));
    QVERIFY(header.contains(QStringLiteral("09:05")));
    QVERIFY(header.contains(QStringLiteral("page-1")));
    QVERIFY(!header.contains(QLatin1Char('\n')));
    QVERIFY(promptHeader(Agent::Codex, QStringLiteral("Alex's Mac"), local, QStringLiteral("page-1")).contains(QStringLiteral("codex exec")));
}

void SessionStarterTest::commandLines()
{
    QCOMPARE(claudeArguments(QStringLiteral("Study session · 6 Nov 09:05"), QStringLiteral("[header]\n\nprompt")),
             QStringList({QStringLiteral("--bg"), QStringLiteral("-n"), QStringLiteral("Study session · 6 Nov 09:05"), QStringLiteral("--permission-mode"), QStringLiteral("auto"),
                          QStringLiteral("[header]\n\nprompt")}));
    QVERIFY(!claudeArguments(QStringLiteral("n"), QStringLiteral("p")).contains(QStringLiteral("bypassPermissions")));
    // A session asked for from a phone runs Codex with its automatic review in the sandbox, never without both.
    QCOMPARE(codexArguments(QStringLiteral("/Users/user/Thoughts Sessions")),
             QStringList({QStringLiteral("exec"), QStringLiteral("--skip-git-repo-check"), QStringLiteral("--json"), QStringLiteral("-C"), QStringLiteral("/Users/user/Thoughts Sessions"),
                          QStringLiteral("--approve-for-me"), QStringLiteral("-")}));
    QVERIFY(!codexArguments(QStringLiteral("/f")).join(QLatin1Char(' ')).contains(QStringLiteral("dangerously")));
    QVERIFY(isBatchFile(QStringLiteral("C:/Users/user/AppData/Roaming/npm/claude.CMD")));
    QVERIFY(isBatchFile(QStringLiteral("x.bat")));
    QVERIFY(!isBatchFile(QStringLiteral("C:/Users/user/.local/bin/claude.exe")));
    QVERIFY(!claudeCandidates().isEmpty());
}

void SessionStarterTest::readsClaudeAnswers()
{
    auto started = parseBackgrounded(QStringLiteral("Starting…\nbackgrounded · 1a2b3c4d · Study session · 26 Nov 15:04\n"));
    QVERIFY(started);
    QCOMPARE(started->shortId, QStringLiteral("1a2b3c4d"));
    QCOMPARE(started->name, QStringLiteral("Study session · 26 Nov 15:04"));
    started = parseBackgrounded(QStringLiteral("backgrounded · abcdef12"));
    QVERIFY(started);
    QCOMPARE(started->shortId, QStringLiteral("abcdef12"));
    QVERIFY(!parseBackgrounded(QStringLiteral("Workspace not trusted. Run `claude` in /x once and accept the trust prompt, then retry.")));
    QVERIFY(!parseBackgrounded(QStringLiteral("backgrounded · XYZ")));

    const QByteArray agents = R"([{"pid":1,"sessionId":"11111111-2222-4333-8444-555555555555","kind":"interactive"},
                                  {"pid":2,"id":"1a2b3c4d","sessionId":"1a2b3c4d-0000-4000-8000-000000000001","kind":"background"}])";
    QCOMPARE(sessionIdFromAgents(agents, QStringLiteral("1a2b3c4d")), QStringLiteral("1a2b3c4d-0000-4000-8000-000000000001"));
    // By the start of its id when the row has no short id.
    QCOMPARE(sessionIdFromAgents(R"([{"sessionId":"1a2b3c4d-0000-4000-8000-000000000001"}])", QStringLiteral("1a2b3c4d")),
             QStringLiteral("1a2b3c4d-0000-4000-8000-000000000001"));
    QCOMPARE(sessionIdFromAgents(agents, QStringLiteral("99999999")), QString());
    QCOMPARE(sessionIdFromAgents("not json", QStringLiteral("1a2b3c4d")), QString());
    QCOMPARE(sessionIdFromAgents(agents, QString()), QString());

    QCOMPARE(claudeLoggedIn(R"({"loggedIn": true, "authMethod": "claude.ai"})"), std::optional<bool>(true));
    QCOMPARE(claudeLoggedIn(R"({"loggedIn": false})"), std::optional<bool>(false));
    QCOMPARE(claudeLoggedIn("error: unknown command 'auth'"), std::optional<bool>());
}

void SessionStarterTest::readsCodexThread()
{
    QCOMPARE(codexThreadId("{\"type\":\"thread.started\",\"thread_id\":\"0199aaaa\"}\n{\"type\":\"turn.started\"}\n"), QStringLiteral("0199aaaa"));
    QCOMPARE(codexThreadId("{\"id\":\"0\",\"msg\":{\"type\":\"session_configured\",\"session_id\":\"older-1\"}}\n"), QStringLiteral("older-1"));
    QCOMPARE(codexThreadId("{\"type\":\"turn.started\"}\nnot json\n"), QString());
    QCOMPARE(codexThreadId(QByteArray()), QString());
}

void SessionStarterTest::classifiesClaudeFailures()
{
    QCOMPARE(classifyClaudeFailure(QStringLiteral("Workspace not trusted. Run `claude` in /Users/user/Thoughts Sessions once and accept the trust prompt, then retry.")),
             ClaudeFailure::Untrusted);
    QCOMPARE(classifyClaudeFailure(QStringLiteral("error: unknown option '--bg'")), ClaudeFailure::TooOld);
    QCOMPARE(classifyClaudeFailure(QStringLiteral("Not logged in · Please run /login")), ClaudeFailure::SignedOut);
    QCOMPARE(classifyClaudeFailure(QStringLiteral("Invalid API key · Please run /login")), ClaudeFailure::SignedOut);
    QCOMPARE(classifyClaudeFailure(QStringLiteral("Something else broke")), ClaudeFailure::Other);
}

void SessionStarterTest::agentEnvironmentKeepsOwnSignIn()
{
    QProcessEnvironment base;
    base.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    base.insert(QStringLiteral("CLAUDECODE"), QStringLiteral("1"));
    base.insert(QStringLiteral("CLAUDE_CODE_ENTRYPOINT"), QStringLiteral("cli"));
    base.insert(QStringLiteral("ANTHROPIC_API_KEY"), QStringLiteral("placeholder"));
    base.insert(QStringLiteral("CODEX_HOME"), QStringLiteral("/codex"));
    base.insert(QStringLiteral("HOME"), QStringLiteral("/Users/user"));
    const QProcessEnvironment environment = agentEnvironment(QStringLiteral("/opt/tools/bin/claude"), base);
    QVERIFY(!environment.contains(QStringLiteral("CLAUDECODE")));
    QVERIFY(!environment.contains(QStringLiteral("CLAUDE_CODE_ENTRYPOINT")));
    QVERIFY(!environment.contains(QStringLiteral("ANTHROPIC_API_KEY")));
    QCOMPARE(environment.value(QStringLiteral("CODEX_HOME")), QStringLiteral("/codex"));
#ifndef Q_OS_WIN
    const QStringList path = environment.value(QStringLiteral("PATH")).split(QLatin1Char(':'));
    QCOMPARE(path.first(), QStringLiteral("/opt/tools/bin"));
    QVERIFY(path.contains(QStringLiteral("/opt/homebrew/bin")));
    QVERIFY(path.contains(QStringLiteral("/usr/bin")));
    QCOMPARE(path.count(QStringLiteral("/usr/bin")), 1);
#endif
}

void SessionStarterTest::noticeImageIsAPicture()
{
    const QByteArray png = noticeImage(QStringLiteral("Study session started"), QString(600, QLatin1Char('w')).replace(QStringLiteral("wwwww"), QStringLiteral("word ")));
    QVERIFY(png.startsWith("\x89PNG"));
    const QImage image = QImage::fromData(png, "PNG");
    QCOMPARE(image.width(), 1200);
    QVERIFY(image.height() > 200);
    QVERIFY(image.height() <= 8000);
}

// ---------------------------------------------------------------- against a fake server

void SessionStarterTest::startsClaudeSessionFromPhonePage()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    Fixture fixture = start(options());
    FakeServer &server = *fixture.server;
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/pages")));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/pages?after=7&wait=%1").arg(SessionStarter::PagesWaitSeconds)));

    const QString prompt = QStringLiteral("Let's study chapter 3 of the MoE paper.\nAsk me questions with notes.");
    server.addPage(pageRecord(QStringLiteral("page-claude"), 8, QDateTime::currentDateTimeUtc().addSecs(-5), prompt));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(0).toString(), QStringLiteral("page-claude"));
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("started"));

    // Nobody had it; signed in; started in the background in the sessions folder; its full id read.
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/handling/page-claude")));
    QStringList order;
    for (const auto &call : calls()) order.append(call.second);
    QCOMPARE(order, QStringList({QStringLiteral("auth"), QStringLiteral("--bg"), QStringLiteral("agents")}));
    QCOMPARE(argv(callOf(QStringLiteral("auth"))), QStringList({QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--json")}));
    const QString background = callOf(QStringLiteral("--bg"));
    const QStringList arguments = argv(background);
    QCOMPARE(arguments.size(), 6);
    QCOMPARE(arguments.mid(0, 2), QStringList({QStringLiteral("--bg"), QStringLiteral("-n")}));
    QVERIFY2(arguments.at(2).startsWith(QStringLiteral("Study session · ")), qPrintable(arguments.at(2)));
    QCOMPARE(arguments.mid(3, 2), QStringList({QStringLiteral("--permission-mode"), QStringLiteral("auto")}));
    QVERIFY(arguments.at(5).startsWith(QStringLiteral("[Thoughts on Test Mac started this session")));
    QVERIFY(arguments.at(5).contains(QStringLiteral("page-claude")));
    QVERIFY(arguments.at(5).endsWith(QStringLiteral("]\n\n") + prompt));
    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("Thoughts Sessions"));
    QVERIFY(QFileInfo(folder).isDir());
    QCOMPARE(cwd(background), QFileInfo(folder).canonicalFilePath());

    // The page claimed for that session, and the owner told where it runs.
    QCOMPARE(server.claimBodies.size(), 1);
    QCOMPARE(server.claimBodies.at(0).value(QStringLiteral("conversation_id")).toString(), QStringLiteral("1a2b3c4d-0000-4000-8000-000000000001"));
    QCOMPARE(server.notes.size(), 1);
    const QJsonObject note = server.notes.at(0);
    QCOMPARE(note.value(QStringLiteral("title")).toString(), QStringLiteral("Study session started"));
    const QString message = note.value(QStringLiteral("message")).toString();
    QVERIFY2(message.contains(QStringLiteral("Test Mac")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("Claude Code")));
    QVERIFY(message.contains(QStringLiteral("claude attach 1a2b3c4d")));
    QVERIFY(message.contains(arguments.at(2)));
    const QJsonObject origin = note.value(QStringLiteral("origin")).toObject();
    QCOMPARE(origin.value(QStringLiteral("agent")).toString(), QStringLiteral("thoughts-desktop"));
    QCOMPARE(origin.value(QStringLiteral("kind")).toString(), QStringLiteral("session_started"));
    QCOMPARE(origin.value(QStringLiteral("machine")).toString(), QStringLiteral("Test Mac"));
    QCOMPARE(origin.value(QStringLiteral("item_id")).toString(), QStringLiteral("page-claude"));
    QCOMPARE(origin.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("1a2b3c4d-0000-4000-8000-000000000001"));
    QCOMPARE(note.value(QStringLiteral("image")).toObject().value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    QVERIFY(!QImage::fromData(QByteArray::fromBase64(note.value(QStringLiteral("image")).toObject().value(QStringLiteral("data_base64")).toString().toLatin1())).isNull());
    QVERIFY(!note.value(QStringLiteral("note_id")).toString().isEmpty());

    // The feed goes on from that page; the same page again (a retry of the feed) starts nothing.
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.contains(QStringLiteral("GET /v1/agent-notes/pages?after=8&wait=%1").arg(SessionStarter::PagesWaitSeconds)), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(server.waitingPolls(), 1, 10000);
    server.addPage(pageRecord(QStringLiteral("page-claude"), 9, QDateTime::currentDateTimeUtc(), prompt));
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.contains(QStringLiteral("GET /v1/agent-notes/pages?after=9&wait=%1").arg(SessionStarter::PagesWaitSeconds)), 10000);
    QTest::qWait(300);
    QCOMPARE(fixture.finished->size(), 1);
    QCOMPARE(calls().size(), 3);

    // Remembered across restarts of the app.
    QFile state(QDir(m_dir->path()).filePath(QStringLiteral("state/state.json")));
    QVERIFY(state.open(QIODevice::ReadOnly));
    QVERIFY(state.readAll().contains("page-claude"));
}

void SessionStarterTest::anotherComputerWinsTheClaim()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    Fixture fixture = start(options());
    FakeServer &server = *fixture.server;
    // Both computers saw nobody had it; the other one's claim came first.
    server.claimRaces.insert(QStringLiteral("page-race"));
    server.addPage(pageRecord(QStringLiteral("page-race"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("taken"));
    // Its own session is stopped and removed; the owner hears from the other computer only.
    QTRY_COMPARE_WITH_TIMEOUT(calls().size(), 5, 10000);
    QCOMPARE(argv(callOf(QStringLiteral("stop"))), QStringList({QStringLiteral("stop"), QStringLiteral("1a2b3c4d")}));
    QCOMPARE(argv(callOf(QStringLiteral("rm"))), QStringList({QStringLiteral("rm"), QStringLiteral("1a2b3c4d")}));
    QCOMPARE(calls().at(3).second, QStringLiteral("stop"));
    QCOMPARE(calls().at(4).second, QStringLiteral("rm"));
    QVERIFY(server.notes.isEmpty());
}

void SessionStarterTest::alreadyClaimedPageIsLeftAlone()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    Fixture fixture = start(options());
    FakeServer &server = *fixture.server;
    server.claims.insert(QStringLiteral("page-taken"),
                         {{QStringLiteral("device_id"), QStringLiteral("other-computer")}, {QStringLiteral("conversation_id"), QStringLiteral("their-session")}});
    server.addPage(pageRecord(QStringLiteral("page-taken"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("taken"));
    QVERIFY(calls().isEmpty());
    QVERIFY(server.claimBodies.isEmpty());
    QVERIFY(server.notes.isEmpty());
}

void SessionStarterTest::untrustedFolderIsExplained()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    qputenv("FAKE_CLAUDE_MODE", "untrusted");
    SessionStarter::Options settings = options();
    auto shown = std::make_shared<QStringList>();
    settings.untrusted = [shown](const QString &folder, const QString &claude) { *shown << folder << claude; };
    Fixture fixture = start(settings);
    FakeServer &server = *fixture.server;
    server.addPage(pageRecord(QStringLiteral("page-trust"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("untrusted"));
    // The computer shows how to trust the folder; the phone is told why nothing started.
    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("Thoughts Sessions"));
    QCOMPARE(*shown, QStringList({folder, m_claude}));
    QVERIFY(server.claimBodies.isEmpty());
    QCOMPARE(server.notes.size(), 1);
    const QJsonObject origin = server.notes.at(0).value(QStringLiteral("origin")).toObject();
    QCOMPARE(origin.value(QStringLiteral("kind")).toString(), QStringLiteral("session_not_started"));
    QCOMPARE(origin.value(QStringLiteral("reason")).toString(), QStringLiteral("untrusted"));
    QCOMPARE(origin.value(QStringLiteral("item_id")).toString(), QStringLiteral("page-trust"));
    QVERIFY(origin.value(QStringLiteral("responder")).toBool());
    const QString message = server.notes.at(0).value(QStringLiteral("message")).toString();
    QVERIFY2(message.contains(QDir::toNativeSeparators(folder)), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("Start again")));
}

void SessionStarterTest::missingClaudeIsExplained()
{
    SessionStarter::Options settings = options();
    settings.claudeExecutable = QDir(m_dir->path()).filePath(QStringLiteral("nowhere/claude"));
    Fixture fixture = start(settings);
    FakeServer &server = *fixture.server;
    server.addPage(pageRecord(QStringLiteral("page-missing"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("not_installed"));
    QCOMPARE(server.notes.size(), 1);
    QCOMPARE(server.notes.at(0).value(QStringLiteral("title")).toString(), QStringLiteral("Claude Code is not on Test Mac"));
    QCOMPARE(server.notes.at(0).value(QStringLiteral("origin")).toObject().value(QStringLiteral("reason")).toString(), QStringLiteral("not_installed"));
    QVERIFY(server.claimBodies.isEmpty());
}

void SessionStarterTest::signedOutClaudeIsExplained()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    qputenv("FAKE_CLAUDE_MODE", "signedout");
    Fixture fixture = start(options());
    FakeServer &server = *fixture.server;
    server.addPage(pageRecord(QStringLiteral("page-signedout"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("signed_out"));
    // Nothing was started.
    QCOMPARE(calls().size(), 1);
    QCOMPARE(calls().at(0).second, QStringLiteral("auth"));
    QCOMPARE(server.notes.size(), 1);
    QVERIFY(server.notes.at(0).value(QStringLiteral("message")).toString().contains(QStringLiteral("/login")));
}

void SessionStarterTest::neverAndOldPagesStartNothing()
{
    SessionStarter::Options settings = options();
    settings.consent = AgentSessions::PhoneStart::Never;
    Fixture fixture = start(settings);
    FakeServer &server = *fixture.server;
    // Older than 15 minutes when it arrives: not even looked at.
    server.addPage(pageRecord(QStringLiteral("page-old"), 8, QDateTime::currentDateTimeUtc().addSecs(-20 * 60), QStringLiteral("Study")));
    // Never on this computer: declined without a word (another computer may start it).
    server.addPage(pageRecord(QStringLiteral("page-never"), 9, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(0).toString(), QStringLiteral("page-never"));
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("declined"));
    QVERIFY(!server.requests.contains(QStringLiteral("GET /v1/agent-notes/handling/page-old")));
    QVERIFY(calls().isEmpty());
    QVERIFY(server.claimBodies.isEmpty());
    QVERIFY(server.notes.isEmpty());
}

void SessionStarterTest::questionAlwaysThenStartsWithoutAsking()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    SessionStarter::Options settings = options();
    settings.consent = AgentSessions::PhoneStart::Ask;
    Fixture fixture = start(settings);
    FakeServer &server = *fixture.server;
    server.addPage(pageRecord(QStringLiteral("page-ask"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    // The question: non-modal, its three answers.
    QMessageBox *question = nullptr;
    const auto findQuestion = [&question] {
        question = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QLatin1String("sessionStartQuestion") && widget->isVisible()) question = qobject_cast<QMessageBox *>(widget);
        }
        return question != nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(findQuestion(), 10000);
    QCOMPARE(question->windowModality(), Qt::NonModal);
    QCOMPARE(question->text(), QStringLiteral("Your phone asks to start a study session on this computer with Claude Code."));
    QVERIFY(question->findChild<QPushButton *>(QStringLiteral("sessionStartOnce")));
    QVERIFY(question->findChild<QPushButton *>(QStringLiteral("sessionStartNever")));
    QCOMPARE(question->findChild<QPushButton *>(QStringLiteral("sessionStartNever"))->text(), QStringLiteral("Not on this computer"));
    QVERIFY(calls().isEmpty());
    QPointer<QMessageBox> asked(question);
    question->findChild<QPushButton *>(QStringLiteral("sessionStartAlways"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("started"));
    QTRY_VERIFY(!asked);

    // Always: the next request starts without a question.
    QTRY_COMPARE_WITH_TIMEOUT(server.waitingPolls(), 1, 10000);
    server.addPage(pageRecord(QStringLiteral("page-ask-2"), 9, QDateTime::currentDateTimeUtc(), QStringLiteral("Study more")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 2, 20000);
    QCOMPARE(fixture.finished->at(1).at(1).toString(), QStringLiteral("started"));
    QVERIFY(!findQuestion());
    QCOMPARE(server.notes.size(), 2);
}

void SessionStarterTest::questionNotHereDeclinesForGood()
{
    SessionStarter::Options settings = options();
    settings.consent = AgentSessions::PhoneStart::Ask;
    auto asked = std::make_shared<int>(0);
    settings.ask = [asked](const SessionStarter::AskRequest &request, std::function<void(AskAnswer)> answer) {
        ++*asked;
        QCOMPARE(request.agent, Agent::Codex);
        QVERIFY(request.folder.endsWith(QStringLiteral("Thoughts Sessions")));
        answer(AskAnswer::NotHere);
    };
    Fixture fixture = start(settings);
    FakeServer &server = *fixture.server;
    server.addPage(pageRecord(QStringLiteral("page-no"), 8, QDateTime::currentDateTimeUtc(), QStringLiteral("Study"), {{QStringLiteral("agent"), QStringLiteral("codex")}}));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("declined"));
    QTRY_COMPARE_WITH_TIMEOUT(server.waitingPolls(), 1, 10000);
    server.addPage(pageRecord(QStringLiteral("page-no-2"), 9, QDateTime::currentDateTimeUtc(), QStringLiteral("Study")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 2, 20000);
    QCOMPARE(fixture.finished->at(1).at(1).toString(), QStringLiteral("declined"));
    QCOMPARE(*asked, 1);
    QVERIFY(calls().isEmpty());
    QVERIFY(server.notes.isEmpty());
}

void SessionStarterTest::startsCodexSession()
{
#ifdef Q_OS_WIN
    QSKIP("The fake codex is a shell script.");
#endif
    Fixture fixture = start(options());
    FakeServer &server = *fixture.server;
    const QString prompt = QStringLiteral("Quiz me on \"routing\" & $HOME's `experts`.");
    server.addPage(pageRecord(QStringLiteral("page-codex"), 8, QDateTime::currentDateTimeUtc(), prompt, {{QStringLiteral("agent"), QStringLiteral("codex")}}));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.finished->size(), 1, 20000);
    QCOMPARE(fixture.finished->at(0).at(1).toString(), QStringLiteral("started"));

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("Thoughts Sessions"));
    QStringList order;
    for (const auto &call : calls()) order.append(call.second);
    QCOMPARE(order, QStringList({QStringLiteral("login"), QStringLiteral("exec")}));
    const QString exec = callOf(QStringLiteral("exec"));
    QCOMPARE(argv(exec), QStringList({QStringLiteral("exec"), QStringLiteral("--skip-git-repo-check"), QStringLiteral("--json"), QStringLiteral("-C"), folder,
                                      QStringLiteral("--approve-for-me"), QStringLiteral("-")}));
    QCOMPARE(cwd(exec), QFileInfo(folder).canonicalFilePath());
    // The prompt went in on standard input, exactly.
    QFile input(QDir(m_agents).filePath(QStringLiteral("stdin-") + exec));
    QVERIFY(input.open(QIODevice::ReadOnly));
    const QString given = QString::fromUtf8(input.readAll());
    QVERIFY(given.startsWith(QStringLiteral("[Thoughts on Test Mac started this session")));
    QVERIFY(given.endsWith(QStringLiteral("]\n\n") + prompt));

    QCOMPARE(server.claimBodies.size(), 1);
    QCOMPARE(server.claimBodies.at(0).value(QStringLiteral("conversation_id")).toString(), QStringLiteral("0199aaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee"));
    QCOMPARE(server.notes.size(), 1);
    const QString message = server.notes.at(0).value(QStringLiteral("message")).toString();
    QVERIFY2(message.contains(QStringLiteral("with Codex")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("not kept listening for replies yet")));
    QVERIFY(message.contains(QStringLiteral("codex resume 0199aaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee")));
    const QJsonObject origin = server.notes.at(0).value(QStringLiteral("origin")).toObject();
    QCOMPARE(origin.value(QStringLiteral("kind")).toString(), QStringLiteral("session_started"));
    QCOMPARE(origin.value(QStringLiteral("session_agent")).toString(), QStringLiteral("codex"));
    QCOMPARE(origin.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("0199aaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee"));
    // Its events are kept in the run's folder.
    const QStringList runs = QDir(QDir(folder).filePath(QStringLiteral("codex"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(runs.size(), 1);
    QVERIFY(runs.at(0).endsWith(QStringLiteral("-page-cod")));
    QFile output(QDir(folder).filePath(QStringLiteral("codex/%1/output.jsonl").arg(runs.at(0))));
    QVERIFY(output.open(QIODevice::ReadOnly));
    QVERIFY(output.readAll().contains("thread.started"));
}

QTEST_MAIN(SessionStarterTest)
#include "sessionstartertest.moc"
