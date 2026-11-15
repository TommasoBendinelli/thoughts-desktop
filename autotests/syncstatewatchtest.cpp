// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/syncstatewatch.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

using namespace Thoughts;

namespace
{
// GET /v1/health and GET /v2/sync/state as a server with sync_state_wait_v1 answers them: with
// If-None-Match naming the state it holds and wait_ms, the request is held until the state
// changes (200) or the wait is over (304).
class StateServer : public QObject
{
public:
    StateServer()
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray head = buffer.left(headerEnd);
                    m_buffers.remove(socket);
                    answer(socket, head);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    for (int index = 0; index < m_held.size(); ++index) {
                        if (m_held.at(index).socket == socket) {
                            delete m_held.at(index).timer;
                            m_held.removeAt(index);
                            ++closedWhileHeld;
                            break;
                        }
                    }
                    socket->deleteLater();
                });
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    QStringList capabilities{QStringLiteral("agent_notes_v1"), SyncStateWatch::capability()};
    QByteArray version = "\"v1\"";
    QStringList requests;  // "GET /v2/sync/state?wait_ms=400"
    QList<QByteArray> conditions;  // the If-None-Match of each state request ("" without one)
    int stateStatus = 0;  // set: every state request is answered with it at once
    bool holds = true;  // false: as a server without the capability, 304 at once
    bool answersNever = false;  // a held request is never answered (the connection is gone)
    int closedWhileHeld = 0;

    int held() const { return int(m_held.size()); }
    int stateRequests() const { return int(requests.filter(QStringLiteral("/v2/sync/state")).size()); }

    // Another device wrote: every held request is answered with the new state.
    void change(const QByteArray &newVersion)
    {
        version = newVersion;
        const QList<Held> held = m_held;
        m_held.clear();
        for (const Held &entry : held) {
            delete entry.timer;
            if (entry.socket) reply(entry.socket, 200);
        }
    }

private:
    struct Held {
        QPointer<QTcpSocket> socket;
        QTimer *timer = nullptr;
    };

    void reply(QTcpSocket *socket, int code, const QByteArray &body = {}, const QByteArray &headers = {})
    {
        QByteArray payload = body;
        QByteArray extra = headers;
        if (code == 200 && payload.isEmpty()) {
            payload = QJsonDocument(QJsonObject{{QStringLiteral("contract_version"), 2},
                                                {QStringLiteral("latest_server_sequence"), 10},
                                                {QStringLiteral("library_revision"), QString(64, QLatin1Char('a'))},
                                                {QStringLiteral("source_frontier"), 3}})
                          .toJson(QJsonDocument::Compact);
        }
        if (code == 200 || code == 304) extra += "ETag: " + version + "\r\n";
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\n" + extra
                      + "Connection: close\r\n\r\n" + payload);
        socket->disconnectFromHost();
    }

    void answer(QTcpSocket *socket, const QByteArray &head)
    {
        const QList<QByteArray> words = head.left(head.indexOf("\r\n")).split(' ');
        const QUrl target(QString::fromLatin1(words.value(1)));
        requests.append(QString::fromLatin1(words.value(0)) + QLatin1Char(' ') + target.toString());
        QByteArray condition;
        for (const QByteArray &line : head.split('\n')) {
            if (line.toLower().startsWith("if-none-match:")) condition = line.mid(14).trimmed();
        }
        if (!head.contains("Authorization: Bearer secret")) {
            reply(socket, 401, R"({"detail":"invalid token"})");
        } else if (target.path() == QLatin1String("/v1/health")) {
            reply(socket, 200, QJsonDocument(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities)}}).toJson());
        } else if (target.path() == QLatin1String("/v2/sync/state")) {
            conditions.append(condition);
            const int wait = QUrlQuery(target).queryItemValue(QStringLiteral("wait_ms")).toInt();
            if (stateStatus) {
                reply(socket, stateStatus, R"({"detail":"try later"})");
            } else if (condition != version) {
                reply(socket, 200);
            } else if (!holds || wait <= 0) {
                reply(socket, 304);
            } else {
                Held entry{socket, nullptr};
                if (!answersNever) {
                    entry.timer = new QTimer;
                    entry.timer->setSingleShot(true);
                    QPointer<QTcpSocket> guard(socket);
                    connect(entry.timer, &QTimer::timeout, this, [this, guard] {
                        for (int index = 0; index < m_held.size(); ++index) {
                            if (m_held.at(index).socket == guard) {
                                m_held.at(index).timer->deleteLater();
                                m_held.removeAt(index);
                                break;
                            }
                        }
                        if (guard) reply(guard, 304);
                    });
                    entry.timer->start(wait);
                }
                m_held.append(entry);
            }
        } else {
            reply(socket, 404, R"({"detail":"not found"})");
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<Held> m_held;
};

template<typename Predicate>
bool waitFor(Predicate predicate, int timeout = 5000)
{
    return QTest::qWaitFor(predicate, timeout);
}
}

class SyncStateWatchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void withoutAServerThePauseIsPlain();
    void withoutTheCapabilityThePauseIsPlain();
    void aChangeRunsTheCheckAtOnce();
    void anUnchangedStateEndsTheWait();
    void aFailedHeldRequestPausesPlain();
    void aServerThatDoesNotHoldGetsPlainPauses();
    void aLostAnswerEndsByTheTransferTimeout();
    void changesInARowAreSpaced();
    void stopEndsTheHeldRequest();
};

void SyncStateWatchTest::initTestCase()
{
    // As in the app's tests: only the environment names a server, and it names none here.
    QStandardPaths::setTestModeEnabled(true);
    qunsetenv("THOUGHTS_TOKEN");
    qunsetenv("THOUGHTS_SERVER_URL");
}

void SyncStateWatchTest::withoutAServerThePauseIsPlain()
{
    SyncStateWatch watch;
    watch.setTimingForTesting(300, 2000, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    QElapsedTimer clock;
    clock.start();
    watch.start();
    QCOMPARE(watch.pause(), SyncStateWatch::Pause::Plain);
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 250, qPrintable(QString::number(clock.elapsed())));
    // The next pause follows the check by itself.
    QCOMPARE(watch.pause(), SyncStateWatch::Pause::Plain);
    QVERIFY(due.wait(3000));
    watch.stop();
}

void SyncStateWatchTest::withoutTheCapabilityThePauseIsPlain()
{
    StateServer server;
    server.capabilities = {QStringLiteral("agent_notes_v1")};
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 2000, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    QElapsedTimer clock;
    clock.start();
    watch.start();
    // The first pause asks nothing: the app is starting.
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 250, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(server.requests.isEmpty());
    QVERIFY(due.wait(3000));
    QVERIFY(!watch.serverHolds());
    QVERIFY(due.wait(3000));
    // The server was asked once what it can do, and never to hold a request.
    QCOMPARE(server.requests, QStringList{QStringLiteral("GET /v1/health")});
    watch.stop();
}

void SyncStateWatchTest::aChangeRunsTheCheckAtOnce()
{
    StateServer server;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 4000, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(due.wait(3000));
    // The first request learns the state's version at once; the second is held on it.
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QVERIFY(watch.serverHolds());
    QCOMPARE(watch.pause(), SyncStateWatch::Pause::Held);
    QCOMPARE(server.requests.value(0), QStringLiteral("GET /v1/health"));
    QCOMPARE(server.requests.value(1), QStringLiteral("GET /v2/sync/state?wait_ms=4000"));
    QCOMPARE(server.requests.value(2), QStringLiteral("GET /v2/sync/state?wait_ms=4000"));
    QCOMPARE(server.conditions, (QList<QByteArray>{QByteArray(), "\"v1\""}));
    QCOMPARE(watch.stateVersion(), QByteArray("\"v1\""));
    // Longer than a plain pause: the held request is the pause.
    QTest::qWait(500);
    QCOMPARE(due.count(), 1);

    QElapsedTimer clock;
    clock.start();
    server.change("\"v2\"");
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(watch.stateVersion(), QByteArray("\"v2\""));
    // The next pause waits on the new state.
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QCOMPARE(server.conditions.last(), QByteArray("\"v2\""));
    QCOMPARE(due.count(), 2);
    watch.stop();
}

void SyncStateWatchTest::anUnchangedStateEndsTheWait()
{
    StateServer server;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 600, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QElapsedTimer clock;
    clock.start();
    // 304 after the wait: the check that ends every pause, then the next held request.
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 400, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QCOMPARE(server.conditions.last(), QByteArray("\"v1\""));
    QVERIFY(watch.serverHolds());
    watch.stop();
}

void SyncStateWatchTest::aFailedHeldRequestPausesPlain()
{
    StateServer server;
    server.stateStatus = 503;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(400, 4000, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(waitFor([&] { return server.stateRequests() == 1; }));
    QElapsedTimer clock;
    clock.start();
    QVERIFY(waitFor([&] { return watch.pause() == SyncStateWatch::Pause::Plain; }));
    QCOMPARE(due.count(), 1);
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 300, qPrintable(QString::number(clock.elapsed())));
    // The next pause tries to hold again; once the server answers, it does.
    server.stateStatus = 0;
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QCOMPARE(due.count(), 2);
    watch.stop();
}

void SyncStateWatchTest::aServerThatDoesNotHoldGetsPlainPauses()
{
    StateServer server;
    server.holds = false;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 4000, 1000, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(due.wait(3000));
    QElapsedTimer clock;
    clock.start();
    // 304 at once to a request that asked to be held: a plain pause, not a loop of requests.
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 250, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(!watch.serverHolds());
    const int asked = server.stateRequests();
    QCOMPARE(asked, 2);
    QVERIFY(due.wait(3000));
    QCOMPARE(server.stateRequests(), asked);
    QCOMPARE(watch.pause(), SyncStateWatch::Pause::Plain);
    watch.stop();
}

void SyncStateWatchTest::aLostAnswerEndsByTheTransferTimeout()
{
    StateServer server;
    server.answersNever = true;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(1500, 300, 100, 50, 300);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QCOMPARE(due.count(), 1);
    QElapsedTimer clock;
    clock.start();
    // Nothing comes back: the request ends once wait and margin are over, and the pause is
    // what is left of a plain one.
    QVERIFY(waitFor([&] { return server.closedWhileHeld == 1; }, 3000));
    QVERIFY2(clock.elapsed() >= 450, qPrintable(QString::number(clock.elapsed())));
    QVERIFY(due.wait(3000));
    QVERIFY2(clock.elapsed() >= 1200, qPrintable(QString::number(clock.elapsed())));
    watch.stop();
}

void SyncStateWatchTest::changesInARowAreSpaced()
{
    StateServer server;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 4000, 100, 600);
    QList<qint64> times;
    QElapsedTimer clock;
    clock.start();
    connect(&watch, &SyncStateWatch::checkDue, this, [&] { times.append(clock.elapsed()); });
    watch.start();
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    QCOMPARE(times.size(), 1);
    server.change("\"v2\"");
    QVERIFY(waitFor([&] { return times.size() == 2; }));
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    server.change("\"v3\"");
    QVERIFY(waitFor([&] { return times.size() == 3; }));
    QVERIFY2(times.at(2) - times.at(1) >= 550, qPrintable(QString::number(times.at(2) - times.at(1))));
    watch.stop();
}

void SyncStateWatchTest::stopEndsTheHeldRequest()
{
    StateServer server;
    SyncStateWatch watch;
    watch.setServer(server.url(), "secret");
    watch.setTimingForTesting(300, 4000, 100, 50);
    QSignalSpy due(&watch, &SyncStateWatch::checkDue);
    watch.start();
    QVERIFY(waitFor([&] { return server.held() == 1; }));
    // The app quits: the request goes with it, and nothing is due afterwards.
    watch.stop();
    QVERIFY(waitFor([&] { return server.closedWhileHeld == 1; }));
    QCOMPARE(watch.pause(), SyncStateWatch::Pause::None);
    server.change("\"v2\"");
    QTest::qWait(500);
    QCOMPARE(due.count(), 1);
    QVERIFY(!watch.isRunning());
}

QTEST_GUILESS_MAIN(SyncStateWatchTest)
#include "syncstatewatchtest.moc"
