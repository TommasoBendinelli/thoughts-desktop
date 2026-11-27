// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"
#include "../part/libraryswitch.h"
#include "../shell/appsignin.h"
#include "../shell/signindialog.h"
#include "../shell/projectsharedialog.h"
#include "../shell/problemreportdialog.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include <QTcpSocket>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
#include <QImage>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>

using namespace Thoughts;

namespace
{
// A Thoughts server of protocol/app-sign-in.md, as far as signing in needs it.
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
                    if (headerEnd < 0) {
                        return;
                    }
                    qsizetype length = 0;
                    for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.mid(15).trimmed().toLongLong();
                        }
                    }
                    if (buffer.size() < headerEnd + 4 + length) {
                        return;
                    }
                    answer(socket, buffer.left(headerEnd + 4), buffer.mid(headerEnd + 4, length));
                    m_buffers.remove(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }

    // GET /v1/ready
    int readyStatus = 200;
    QStringList capabilities {QStringLiteral("sync_state_v2"), QStringLiteral("app_sign_in_v1")};
    // Where a person without a login asks for one (section 5); empty: null.
    QString accessContact;
    // POST /v1/apps/sign-in: a password, or an invite with which a new password is chosen.
    QString username = QStringLiteral("alex");
    QString password = QStringLiteral("correct horse battery");
    QString invite;
    int signInStatus = 0;  // set: the answer, whatever is sent
    QByteArray signInBody;
    QByteArray retryAfter;
    // GET /v1/apps/devices and sign-out, for this token.
    QByteArray token = "app-token";
    QJsonArray devices;
    // Share… of a project (project-links.md 14): what GET /v1/project-shares answers, and the answer of
    // POST /v1/project-shares (the body is recorded in bodies).
    QJsonObject shares;
    QJsonObject shareAnswer;
    QStringList requests;  // "GET /v1/ready"
    QList<QJsonObject> bodies;  // of the POSTs, in order
    // POST /v1/problem-reports (problem-reports.md 2).
    int problemReportStatus = 201;
    QByteArray problemReportAnswer = R"({"report_id":"20261126T000000Z-00000000","redactions":0})";
    QList<QByteArray> authorizations;  // of every request
    QString lastQuery;  // of the last GET /v1/project-shares
    QJsonObject links;  // GET /v1/project-links (404 while empty: a server without links)

private:
    void reply(QTcpSocket *socket, int code, const QByteArray &body, const QByteArray &headers = {})
    {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + headers
                      + "Connection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void answer(QTcpSocket *socket, const QByteArray &head, const QByteArray &requestBody)
    {
        const QList<QByteArray> words = head.left(head.indexOf("\r\n")).split(' ');
        const QString method = QString::fromLatin1(words.value(0));
        const QString path = QUrl(QString::fromLatin1(words.value(1))).path();
        requests.append(method + QLatin1Char(' ') + path);
        QByteArray authorization;
        for (const QByteArray &line : head.split('\n')) {
            if (line.toLower().startsWith("authorization:")) {
                authorization = line.mid(14).trimmed();
            }
        }
        authorizations.append(authorization);
        const QJsonObject body = QJsonDocument::fromJson(requestBody).object();
        if (method == QLatin1String("POST")) {
            bodies.append(body);
        }
        const bool authorized = authorization == "Bearer " + token;
        if (method == QLatin1String("GET") && path == QLatin1String("/v1/ready")) {
            if (readyStatus != 200) {
                reply(socket, readyStatus, readyStatus == 503 ? R"({"detail":"Object store is unavailable"})" : R"({"detail":"Not Found"})");
                return;
            }
            reply(socket, 200, QJsonDocument(QJsonObject {{QStringLiteral("status"), QStringLiteral("ready")},
                                                          {QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities)},
                                                          {QStringLiteral("access_contact"), accessContact.isEmpty() ? QJsonValue() : QJsonValue(accessContact)}})
                                  .toJson());
            return;
        }
        if (method == QLatin1String("POST") && path == QLatin1String("/v1/apps/sign-in")) {
            if (signInStatus) {
                reply(socket, signInStatus, signInBody, retryAfter.isEmpty() ? QByteArray() : "Retry-After: " + retryAfter + "\r\n");
                return;
            }
            const bool withInvite = body.contains(QStringLiteral("invite"));
            if (withInvite && body.value(QStringLiteral("invite")).toString() != invite) {
                reply(socket, 401, R"({"detail":"This invite does not work. It may have expired or been used already; ask for a new one."})");
                return;
            }
            if (body.value(QStringLiteral("username")).toString() != username || (!withInvite && body.value(QStringLiteral("password")).toString() != password)) {
                reply(socket, 401, R"({"detail":"Wrong username or password"})");
                return;
            }
            if (withInvite) {
                password = body.value(QStringLiteral("new_password")).toString();
            }
            const QJsonObject answer {
                {QStringLiteral("username"), username},
                {QStringLiteral("library"), QJsonObject {{QStringLiteral("id"), QStringLiteral("main")}, {QStringLiteral("name"), QStringLiteral("main")}}},
                {QStringLiteral("devices"),
                 QJsonObject {{QStringLiteral("app"), QJsonObject {{QStringLiteral("device_id"), QStringLiteral("macos-1a2b3c4d5e6f7a8b")}, {QStringLiteral("token"), QString::fromLatin1(token)}}},
                              {QStringLiteral("source_sync"),
                               QJsonObject {{QStringLiteral("device_id"), QStringLiteral("macos-sync-9c8d7e6f5a4b3c2d")}, {QStringLiteral("token"), QStringLiteral("sync-token")}}}}}};
            reply(socket, 200, QJsonDocument(answer).toJson(), "Cache-Control: no-store\r\n");
            return;
        }
        if (!authorized) {
            reply(socket, 401, R"({"detail":"invalid token"})");
            return;
        }
        if (method == QLatin1String("GET") && path == QLatin1String("/v1/project-links")) {
            if (links.isEmpty()) reply(socket, 404, R"({"detail":"Not Found"})");
            else reply(socket, 200, QJsonDocument(links).toJson());
            return;
        }
        if (method == QLatin1String("GET") && path == QLatin1String("/v1/project-shares")) {
            lastQuery = QUrl(QString::fromLatin1(words.value(1))).query(QUrl::FullyDecoded);
            reply(socket, 200, QJsonDocument(shares).toJson());
            return;
        }
        if (method == QLatin1String("POST") && path == QLatin1String("/v1/problem-reports")) {
            if (!authorized) {
                reply(socket, 401, R"({"detail":"Invalid device token"})");
                return;
            }
            reply(socket, problemReportStatus, problemReportAnswer);
            return;
        }
        if (method == QLatin1String("POST") && path == QLatin1String("/v1/project-shares")) {
            reply(socket, 200, QJsonDocument(shareAnswer).toJson());
            return;
        }
        if (method == QLatin1String("DELETE") && (path.startsWith(QLatin1String("/v1/project-shares/")) || path.startsWith(QLatin1String("/v1/project-invitations/")))) {
            reply(socket, 200, R"({"state":"stopped"})");
            return;
        }
        if (method == QLatin1String("GET") && path == QLatin1String("/v1/apps/devices")) {
            reply(socket, 200, QJsonDocument(QJsonObject {{QStringLiteral("devices"), devices}}).toJson());
            return;
        }
        static const QRegularExpression signOut(QStringLiteral("^/v1/apps/devices/([^/]+)/sign-out$"));
        const QRegularExpressionMatch match = signOut.match(path);
        if (method == QLatin1String("POST") && match.hasMatch()) {
            for (int index = 0; index < devices.size(); ++index) {
                if (devices.at(index).toObject().value(QStringLiteral("device_id")).toString() == match.captured(1)) {
                    devices.removeAt(index);
                    reply(socket, 204, {});
                    return;
                }
            }
            reply(socket, 404, R"({"detail":"No such device"})");
            return;
        }
        reply(socket, 404, R"({"detail":"Not Found"})");
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QJsonObject device(const QString &id, const QString &name, const QString &platform, bool current, const QString &lastSeen = {})
{
    return {{QStringLiteral("device_id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("platform"), platform},
            {QStringLiteral("kind"), platform == QLatin1String("web") ? QStringLiteral("web") : QStringLiteral("app")},
            {QStringLiteral("username"), QStringLiteral("alex")},
            {QStringLiteral("created_at"), QStringLiteral("2026-11-18T10:00:00Z")},
            {QStringLiteral("last_seen_at"), lastSeen.isEmpty() ? QJsonValue() : QJsonValue(lastSeen)},
            {QStringLiteral("signed_out_at"), QJsonValue()},
            {QStringLiteral("current"), current}};
}

// A secret store in memory.
struct Secrets {
    QHash<QString, QByteArray> tokens;
    bool writable = true;

    AppSignIn::Hooks hooks(const FakeServer &server)
    {
        AppSignIn::Hooks hooks;
        hooks.writeToken = [this](const QString &account, const QByteArray &token, QString *error) {
            if (!writable) {
                *error = QStringLiteral("the secret store is locked");
                return false;
            }
            tokens.insert(account, token);
            return true;
        };
        hooks.readToken = [this](const QString &account, const QString &) {
            return tokens.value(account);
        };
        hooks.removeToken = [this](const QString &account, QString *) {
            tokens.remove(account);
            return true;
        };
        const QUrl url = server.url();
        hooks.requestUrl = [url](const QString &) {
            return url;
        };
        return hooks;
    }
};

template<typename T>
T *child(QWidget &parent, const QString &name)
{
    return parent.findChild<T *>(name);
}
}

class AppSignInTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void connectLinks();
    void connectLinks_data();
    void serverChecks();
    void accessRequests();
    void projectShareDialog();
    void projectShareEnded();
    void problemReports();
    void signInAnswers();
    void devicesAnswer();
    void deviceNames();
    void nothingLeavesInTestModeWithoutHooks();
    void storingRecordsBothTokensForTheServer();
    void dialogChecksTheAddressAsTyped();
    void dialogNeedsEverything();
    void dialogOffersToRequestAccess();
    void signInStoresBothTokens();
    void signInToAnotherServerSetsTheLibraryAside();
    void wrongPasswordIsShown();
    void unstoredTokensAreSignedOutAgain();
    void inviteChoosesAPassword();
    void linkFillsTheDialog();
    void serverDialogListsDevices();
    void signOutAnotherDevice();
    void signOutOfThisComputer();
    void oldServerHasNoDevices();

private:
    QString m_store;
};

void AppSignInTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_store = ServerAddress::storePath();
}

void AppSignInTest::init()
{
    qunsetenv("THOUGHTS_SERVER_URL");
    qunsetenv("THOUGHTS_TOKEN");
    QFile::remove(m_store);
}

void AppSignInTest::cleanup()
{
    QFile::remove(m_store);
}

void AppSignInTest::connectLinks_data()
{
    QTest::addColumn<QString>("link");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("server");
    QTest::addColumn<QString>("username");
    QTest::addColumn<QString>("invite");

    QTest::newRow("password") << QStringLiteral("thoughts://connect?server=https%3A%2F%2Fthoughts.example.org&username=alex") << true
                              << QStringLiteral("https://thoughts.example.org") << QStringLiteral("alex") << QString();
    QTest::newRow("invite") << QStringLiteral("thoughts://connect?server=https%3A%2F%2Fthoughts.example.org&username=alex&invite=Ab-_9xYz0123456789abcdefABCDEF0123456789abc")
                            << true << QStringLiteral("https://thoughts.example.org") << QStringLiteral("alex")
                            << QStringLiteral("Ab-_9xYz0123456789abcdefABCDEF0123456789abc");
    QTest::newRow("slash path") << QStringLiteral("thoughts://connect/?server=https://thoughts.example.org/base&username=alex") << true
                                << QStringLiteral("https://thoughts.example.org/base") << QStringLiteral("alex") << QString();
    QTest::newRow("unknown parameter kept out") << QStringLiteral("thoughts://connect?server=https://thoughts.example.org&username=alex&theme=dark") << true
                                                << QStringLiteral("https://thoughts.example.org") << QStringLiteral("alex") << QString();
    // Shown as typed: the dialog says what is wrong with an http address.
    QTest::newRow("http server shown") << QStringLiteral("thoughts://connect?server=http://thoughts.example.org&username=alex") << true
                                       << QStringLiteral("http://thoughts.example.org") << QStringLiteral("alex") << QString();
    QTest::newRow("other scheme") << QStringLiteral("https://connect?server=https://thoughts.example.org&username=alex") << false << QString() << QString() << QString();
    QTest::newRow("other host") << QStringLiteral("thoughts://open?server=https://thoughts.example.org&username=alex") << false << QString() << QString() << QString();
    QTest::newRow("no server") << QStringLiteral("thoughts://connect?username=alex") << false << QString() << QString() << QString();
    QTest::newRow("server twice") << QStringLiteral("thoughts://connect?server=https://a.example.org&server=https://b.example.org&username=alex") << false << QString()
                                  << QString() << QString();
    QTest::newRow("fragment") << QStringLiteral("thoughts://connect?server=https://thoughts.example.org&username=alex#invite=x") << false << QString() << QString()
                              << QString();
    QTest::newRow("port") << QStringLiteral("thoughts://connect:8443?server=https://thoughts.example.org") << false << QString() << QString() << QString();
    QTest::newRow("user info") << QStringLiteral("thoughts://someone@connect?server=https://thoughts.example.org") << false << QString() << QString() << QString();
    QTest::newRow("path") << QStringLiteral("thoughts://connect/elsewhere?server=https://thoughts.example.org") << false << QString() << QString() << QString();
    QTest::newRow("invite not base64") << QStringLiteral("thoughts://connect?server=https://thoughts.example.org&username=alex&invite=a%20b") << false << QString()
                                       << QString() << QString();
}

void AppSignInTest::connectLinks()
{
    QFETCH(QString, link);
    QFETCH(bool, valid);
    const QUrl url(link);
    const std::optional<AppSignIn::ConnectLink> parsed = AppSignIn::connectLinkFromUrl(url);
    QCOMPARE(parsed.has_value(), valid);
    if (valid) {
        QVERIFY(AppSignIn::isConnectLinkUrl(url));
        QFETCH(QString, server);
        QFETCH(QString, username);
        QFETCH(QString, invite);
        QCOMPARE(parsed->server, server);
        QCOMPARE(parsed->username, username);
        QCOMPARE(parsed->invite, invite);
    }
    QVERIFY(AppSignIn::isConnectLinkUrl(QUrl(QStringLiteral("THOUGHTS://connect?x=1"))));
    QVERIFY(!AppSignIn::isConnectLinkUrl(QUrl(QStringLiteral("thoughts://open/document/x"))));
}

void AppSignInTest::accessRequests()
{
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"status":"ready","access_contact":"owner@example.org"})"), QStringLiteral("owner@example.org"));
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"status":"ready","access_contact":null})"), QString());
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"status":"ready"})"), QString());
    QCOMPARE(AppSignIn::accessContactFromAnswer(404, R"({"access_contact":"owner@example.org"})"), QString());
    // Only a plain address: nothing a mailto: link would read as more recipients or fields.
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"access_contact":"owner@example.org?cc=x@example.org"})"), QString());
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"access_contact":"Owner <owner@example.org>"})"), QString());
    QCOMPARE(AppSignIn::accessContactFromAnswer(200, R"({"access_contact":"a@b.org,c@d.org"})"), QString());

    const QUrl url = AppSignIn::accessRequestUrl(QStringLiteral("owner@example.org"), QStringLiteral("https://thoughts.example.org"), QStringLiteral(" sam "));
    QCOMPARE(url.scheme(), QStringLiteral("mailto"));
    QCOMPARE(url.path(), QStringLiteral("owner@example.org"));
    const QUrlQuery query(url);
    QCOMPARE(query.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded), QStringLiteral("Thoughts access request"));
    QCOMPARE(query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded),
             QStringLiteral("Hello,\n\nI would like to use the Thoughts server at https://thoughts.example.org. Could you send me an invite?\n\n"
                            "Username I would like: sam\nMy name: \n"));
    // Spaces are %20: a + would reach the message as a plus.
    QVERIFY(!url.toEncoded().contains('+'));
}

void AppSignInTest::serverChecks()
{
    QCOMPARE(AppSignIn::serverCheckFromAnswer(200, R"({"status":"ready","capabilities":["app_sign_in_v1"]})"), AppSignIn::ServerCheck::Found);
    QCOMPARE(AppSignIn::serverCheckFromAnswer(200, R"({"status":"ready","capabilities":["sync_state_v2"]})"), AppSignIn::ServerCheck::TooOld);
    QCOMPARE(AppSignIn::serverCheckFromAnswer(503, R"({"detail":"Object store is unavailable"})"), AppSignIn::ServerCheck::NotReady);
    QCOMPARE(AppSignIn::serverCheckFromAnswer(404, R"({"detail":"Not Found"})"), AppSignIn::ServerCheck::NotFound);
    QCOMPARE(AppSignIn::serverCheckFromAnswer(200, "<html>a web page</html>"), AppSignIn::ServerCheck::NotFound);
    QCOMPARE(AppSignIn::serverCheckFromAnswer(0, {}), AppSignIn::ServerCheck::NotFound);
    QCOMPARE(AppSignIn::serverCheckText(AppSignIn::ServerCheck::Found), QStringLiteral("Thoughts server found"));
    QCOMPARE(AppSignIn::serverCheckText(AppSignIn::ServerCheck::NotFound), QStringLiteral("No Thoughts server at this address"));
    QCOMPARE(AppSignIn::serverCheckText(AppSignIn::ServerCheck::TooOld), QStringLiteral("This server is too old for signing in; ask for a device token."));
}

void AppSignInTest::signInAnswers()
{
    QString error;
    const QByteArray good = R"({"username":"alex","library":{"id":"main","name":"Main"},"devices":{"app":{"device_id":"macos-1a2b3c4d5e6f7a8b","token":"t1"},)"
                            R"("source_sync":{"device_id":"macos-sync-9c8d7e6f5a4b3c2d","token":"t2"}}})";
    const std::optional<AppSignIn::SignedIn> signedIn = AppSignIn::signedInFromAnswer(200, good, {}, &error);
    QVERIFY(signedIn);
    QVERIFY(error.isEmpty());
    QCOMPARE(signedIn->username, QStringLiteral("alex"));
    QCOMPARE(signedIn->libraryName, QStringLiteral("Main"));
    QCOMPARE(signedIn->appDeviceId, QStringLiteral("macos-1a2b3c4d5e6f7a8b"));
    QCOMPARE(signedIn->appToken, QByteArray("t1"));
    QCOMPARE(signedIn->sourceSyncDeviceId, QStringLiteral("macos-sync-9c8d7e6f5a4b3c2d"));
    QCOMPARE(signedIn->sourceSyncToken, QByteArray("t2"));

    // A phone-style answer (no source_sync) is still a sign-in.
    QVERIFY(AppSignIn::signedInFromAnswer(200, R"({"username":"alex","devices":{"app":{"device_id":"macos-1","token":"t1"}}})", {}, &error));
    QVERIFY(!AppSignIn::signedInFromAnswer(200, R"({"username":"alex","devices":{"app":{"device_id":"macos-1"}}})", {}, &error));
    QCOMPARE(error, QStringLiteral("The server's answer could not be read."));

    QVERIFY(!AppSignIn::signedInFromAnswer(401, R"({"detail":"Wrong username or password"})", {}, &error));
    QCOMPARE(error, QStringLiteral("Wrong username or password"));
    QVERIFY(!AppSignIn::signedInFromAnswer(401, R"({"detail":"This invite does not work. It may have expired or been used already; ask for a new one."})", {}, &error));
    QCOMPARE(error, QStringLiteral("This invite does not work. It may have expired or been used already; ask for a new one."));
    QVERIFY(!AppSignIn::signedInFromAnswer(429, R"({"detail":"Too many failed authentication attempts"})", "900", &error));
    QCOMPARE(error, QStringLiteral("Too many failed sign-ins. Try again in 15 minutes."));
    QVERIFY(!AppSignIn::signedInFromAnswer(429, R"({"detail":"Too many failed authentication attempts"})", "30", &error));
    QCOMPARE(error, QStringLiteral("Too many failed sign-ins. Try again in 30 seconds."));
    QVERIFY(!AppSignIn::signedInFromAnswer(429, {}, {}, &error));
    QCOMPARE(error, QStringLiteral("Too many failed sign-ins. Try again later."));
    QVERIFY(!AppSignIn::signedInFromAnswer(503, R"({"detail":"Too many sign-ins at once; try again shortly"})", "5", &error));
    QCOMPARE(error, QStringLiteral("Too many sign-ins at once; try again shortly"));
    QVERIFY(!AppSignIn::signedInFromAnswer(422, R"({"detail":[{"loc":["body","device_name"],"msg":"String should have at most 64 characters"}]})", {}, &error));
    QCOMPARE(error, QStringLiteral("String should have at most 64 characters"));
    QVERIFY(!AppSignIn::signedInFromAnswer(404, R"({"detail":"Not Found"})", {}, &error));
    QCOMPARE(error, QStringLiteral("This server is too old for signing in; ask for a device token."));
    QVERIFY(!AppSignIn::signedInFromAnswer(0, {}, {}, &error));
    QCOMPARE(error, QStringLiteral("The server did not answer. Check the address and the connection."));
    // The detail is shown as plain text of one line.
    QVERIFY(!AppSignIn::signedInFromAnswer(401, "{\"detail\":\"<b>bold</b>\\nsecond\"}", {}, &error));
    QCOMPARE(error, QStringLiteral("<b>bold</b> second"));
}

void AppSignInTest::devicesAnswer()
{
    QJsonArray list {device(QStringLiteral("macos-1"), QStringLiteral("Alex's MacBook Pro"), QStringLiteral("macos"), true, QStringLiteral("2026-11-18T17:11:00Z")),
                     device(QStringLiteral("web-2"), QStringLiteral("Web browser (alex)"), QStringLiteral("web"), false),
                     QJsonObject {{QStringLiteral("name"), QStringLiteral("no id")}}};
    const std::optional<QList<AppSignIn::Device>> devices = AppSignIn::devicesFromAnswer(QJsonDocument(QJsonObject {{QStringLiteral("devices"), list}}).toJson());
    QVERIFY(devices);
    QCOMPARE(devices->size(), 2);
    QCOMPARE(devices->at(0).id, QStringLiteral("macos-1"));
    QCOMPARE(devices->at(0).name, QStringLiteral("Alex's MacBook Pro"));
    QCOMPARE(devices->at(0).platform, QStringLiteral("macos"));
    QCOMPARE(devices->at(0).kind, QStringLiteral("app"));
    QCOMPARE(devices->at(0).username, QStringLiteral("alex"));
    QVERIFY(devices->at(0).current);
    QCOMPARE(devices->at(0).lastSeenAt, QDateTime(QDate(2026, 11, 18), QTime(17, 11), QTimeZone::UTC));
    QVERIFY(!devices->at(1).current);
    QVERIFY(!devices->at(1).lastSeenAt.isValid());
    QCOMPARE(AppSignIn::platformLabel(devices->at(1).platform), QStringLiteral("Web browser"));
    QVERIFY(!AppSignIn::devicesFromAnswer("not json"));
    QVERIFY(!AppSignIn::devicesFromAnswer(R"({"devices":{}})"));
    QCOMPARE(AppSignIn::devicesFromAnswer(R"({"devices":[]})")->size(), 0);
}

void AppSignInTest::deviceNames()
{
    QCOMPARE(AppSignIn::cleanDeviceName(QStringLiteral("  Alex's MacBook Pro \n")), QStringLiteral("Alex's MacBook Pro"));
    QCOMPARE(AppSignIn::cleanDeviceName(QStringLiteral("a\tb\x01" "c")), QStringLiteral("abc"));
    QCOMPARE(AppSignIn::cleanDeviceName(QString(80, QLatin1Char('x'))).size(), AppSignIn::maximumDeviceNameLength);
    QVERIFY(AppSignIn::cleanDeviceName(QStringLiteral(" \n ")).isEmpty());
    QVERIFY(!AppSignIn::defaultDeviceName().isEmpty());
    QCOMPARE(AppSignIn::defaultDeviceName(), AppSignIn::cleanDeviceName(AppSignIn::defaultDeviceName()));
}

void AppSignInTest::nothingLeavesInTestModeWithoutHooks()
{
    // No hooks under test mode: no request to a real server and no secret store.
    const AppSignIn::Hooks none;
    QVERIFY(AppSignIn::requestUrl(none, QStringLiteral("https://thoughts.example.org"), QStringLiteral("/v1/ready")).isEmpty());
    QVERIFY(AppSignIn::readToken(none, Platform::desktopDeviceAccount(), QStringLiteral("https://thoughts.example.org")).isEmpty());
    QString error;
    QVERIFY(!AppSignIn::writeToken(none, Platform::desktopDeviceAccount(), "token", &error));
    QVERIFY(!AppSignIn::removeToken(none, Platform::desktopDeviceAccount(), &error));
    // With a hook, the path is joined to the base the hook gives.
    AppSignIn::Hooks hooks;
    hooks.requestUrl = [](const QString &address) {
        return QUrl(address == QLatin1String("https://thoughts.example.org/base") ? QStringLiteral("http://127.0.0.1:1/base/") : QString());
    };
    QCOMPARE(AppSignIn::requestUrl(hooks, QStringLiteral("https://Thoughts.example.org/base/"), QStringLiteral("/v1/ready")), QUrl(QStringLiteral("http://127.0.0.1:1/base/v1/ready")));
    QVERIFY(AppSignIn::requestUrl(hooks, QStringLiteral("http://thoughts.example.org"), QStringLiteral("/v1/ready")).isEmpty());
}

void AppSignInTest::storingRecordsBothTokensForTheServer()
{
    FakeServer server;
    Secrets secrets;
    const AppSignIn::Hooks hooks = secrets.hooks(server);
    QVERIFY(ServerAddress::save(QStringLiteral("https://old.example.org")));
    AppSignIn::SignedIn signedIn;
    signedIn.appDeviceId = QStringLiteral("macos-1");
    signedIn.appToken = "app-token";
    signedIn.sourceSyncDeviceId = QStringLiteral("macos-sync-1");
    signedIn.sourceSyncToken = "sync-token";
    QString error;
    QVERIFY2(AppSignIn::storeSignedIn(hooks, QStringLiteral("https://Thoughts.example.org/"), signedIn, &error), qPrintable(error));
    QCOMPARE(ServerAddress::saved(), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(secrets.tokens.value(Platform::desktopDeviceAccount()), QByteArray("app-token"));
    QCOMPARE(secrets.tokens.value(AppSignIn::sourceSyncAccount()), QByteArray("sync-token"));
    QCOMPARE(ServerAddress::tokenOrigin(Platform::desktopDeviceAccount()), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(ServerAddress::tokenOrigin(AppSignIn::sourceSyncAccount()), QStringLiteral("https://thoughts.example.org"));
    QVERIFY(ServerAddress::tokenMayBeSentTo(Platform::desktopDeviceAccount(), "app-token", QStringLiteral("https://thoughts.example.org"), nullptr, {}, m_store));
    QVERIFY(!ServerAddress::tokenMayBeSentTo(Platform::desktopDeviceAccount(), "app-token", QStringLiteral("https://old.example.org"), nullptr, {}, m_store));

    QVERIFY(AppSignIn::removeStoredTokens(hooks, &error));
    QVERIFY(secrets.tokens.isEmpty());

    // THOUGHTS_SERVER_URL sets the server: the saved address stays.
    qputenv("THOUGHTS_SERVER_URL", "https://session.example.org");
    const auto restore = qScopeGuard([] {
        qunsetenv("THOUGHTS_SERVER_URL");
    });
    QVERIFY(AppSignIn::storeSignedIn(hooks, QStringLiteral("https://session.example.org"), signedIn, &error));
    QCOMPARE(ServerAddress::saved(), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(ServerAddress::tokenOrigin(Platform::desktopDeviceAccount()), QStringLiteral("https://session.example.org"));
}

void AppSignInTest::dialogChecksTheAddressAsTyped()
{
    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    dialog.show();
    auto *address = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"));
    auto *status = child<QLabel>(dialog, QStringLiteral("thoughtsSignInServerStatus"));
    QVERIFY(address && status);
    // Filled with the configured server, which a new computer starts with.
    QCOMPARE(address->text(), ServerAddress::defaultUrl());
    QTRY_COMPARE(status->text(), QStringLiteral("Thoughts server found"));
    QCOMPARE(server.requests.count(QStringLiteral("GET /v1/ready")), 1);

    // Typing waits for a pause, then asks once.
    server.capabilities = {QStringLiteral("sync_state_v2")};
    address->clear();
    QTest::keyClicks(address, QStringLiteral("old.example.org"));
    QTRY_COMPARE(status->text(), QStringLiteral("This server is too old for signing in; ask for a device token."));
    QCOMPARE(server.requests.count(QStringLiteral("GET /v1/ready")), 2);

    server.readyStatus = 404;
    address->clear();
    QTest::keyClicks(address, QStringLiteral("https://elsewhere.example.org"));
    QTRY_COMPARE(status->text(), QStringLiteral("No Thoughts server at this address"));

    // An address Thoughts refuses is explained without asking anyone.
    const int asked = server.requests.size();
    address->clear();
    QTest::keyClicks(address, QStringLiteral("http://thoughts.example.org"));
    QTRY_COMPARE(status->text(), QStringLiteral("The server address must start with https://."));
    QTest::qWait(600);
    QCOMPARE(server.requests.size(), asked);
}

void AppSignInTest::dialogOffersToRequestAccess()
{
    FakeServer server;
    server.accessContact = QStringLiteral("owner@example.org");
    Secrets secrets;
    QList<QUrl> opened;
    AppSignIn::Hooks hooks = secrets.hooks(server);
    hooks.openUrl = [&opened](const QUrl &url) {
        opened.append(url);
    };
    SignInDialog dialog(hooks);
    dialog.show();
    auto *status = child<QLabel>(dialog, QStringLiteral("thoughtsSignInServerStatus"));
    auto *access = child<QLabel>(dialog, QStringLiteral("thoughtsSignInAccess"));
    QVERIFY(status && access);
    QTRY_COMPARE(status->text(), QStringLiteral("Thoughts server found"));
    QTRY_VERIFY(access->isVisible());
    QVERIFY(access->text().contains(QLatin1String("Request access")));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername")), QStringLiteral("sam"));
    Q_EMIT access->linkActivated(QStringLiteral("request"));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().scheme(), QStringLiteral("mailto"));
    QCOMPARE(opened.first().path(), QStringLiteral("owner@example.org"));
    const QString body = QUrlQuery(opened.first()).queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
    QVERIFY2(body.contains(QStringLiteral("Thoughts server at %1.").arg(ServerAddress::defaultUrl())), qPrintable(body));
    QVERIFY2(body.contains(QLatin1String("Username I would like: sam\n")), qPrintable(body));

    // A server that names nobody: the person is told whom to ask, and nothing opens.
    server.accessContact.clear();
    auto *address = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"));
    address->clear();
    QTest::keyClicks(address, QStringLiteral("other.example.org"));
    QTRY_COMPARE(access->text(), QStringLiteral("No account? Ask the person who runs this server for an invite."));
    Q_EMIT access->linkActivated(QStringLiteral("request"));
    QCOMPARE(opened.size(), 1);

    // No server, nobody to ask.
    server.readyStatus = 404;
    address->clear();
    QTest::keyClicks(address, QStringLiteral("gone.example.org"));
    QTRY_COMPARE(status->text(), QStringLiteral("No Thoughts server at this address"));
    QVERIFY(!access->isVisible());

    // An invite is a login already.
    server.readyStatus = 200;
    server.accessContact = QStringLiteral("owner@example.org");
    dialog.fill({ServerAddress::defaultUrl(), QStringLiteral("sam"), QStringLiteral("aW52aXRl")});
    QTRY_COMPARE(status->text(), QStringLiteral("Thoughts server found"));
    QTest::qWait(50);
    QVERIFY(!access->isVisible());
}

void AppSignInTest::dialogNeedsEverything()
{
    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    dialog.show();
    auto *status = child<QLabel>(dialog, QStringLiteral("thoughtsSignInServerStatus"));
    auto *username = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername"));
    auto *password = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword"));
    auto *deviceName = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInDeviceName"));
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    QVERIFY(status && username && password && deviceName && signIn);
    QTRY_COMPARE(status->text(), QStringLiteral("Thoughts server found"));
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QVERIFY(!child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInNewPassword"))->isVisible());
    // The computer's own name, which the person may change.
    QCOMPARE(deviceName->text(), AppSignIn::defaultDeviceName());
    QVERIFY(!signIn->isEnabled());
    QTest::keyClicks(username, QStringLiteral("alex"));
    QVERIFY(!signIn->isEnabled());
    QTest::keyClicks(password, QStringLiteral("x"));
    QVERIFY(signIn->isEnabled());
    deviceName->setText(QStringLiteral("   "));
    QVERIFY(!signIn->isEnabled());
    deviceName->setText(QStringLiteral("Alex's Mac"));
    QVERIFY(signIn->isEnabled());
    // Nothing is asked of a server that is not there.
    server.readyStatus = 404;
    auto *address = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"));
    address->clear();
    QTest::keyClicks(address, QStringLiteral("https://elsewhere.example.org"));
    QTRY_COMPARE(status->text(), QStringLiteral("No Thoughts server at this address"));
    QVERIFY(!signIn->isEnabled());
    signIn->click();
    QCOMPARE(server.requests.count(QStringLiteral("POST /v1/apps/sign-in")), 0);
}

void AppSignInTest::signInStoresBothTokens()
{
    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    dialog.show();
    auto *address = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"));
    address->setText(QStringLiteral("https://thoughts.example.org"));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername")), QStringLiteral(" Alex "));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword")), server.password);
    child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInDeviceName"))->setText(QStringLiteral("Alex's MacBook Pro"));
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    QTRY_VERIFY(signIn->isEnabled());
    signIn->click();
    QTRY_COMPARE(signedIn.size(), 1);
    QCOMPARE(signedIn.at(0).at(0).toString(), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(signedIn.at(0).at(1).toString(), QStringLiteral("alex"));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));

    const QJsonObject body = server.bodies.constLast();
    QCOMPARE(body.value(QStringLiteral("username")).toString(), QStringLiteral("alex"));
    QCOMPARE(body.value(QStringLiteral("password")).toString(), server.password);
    QCOMPARE(body.value(QStringLiteral("device_name")).toString(), QStringLiteral("Alex's MacBook Pro"));
    QCOMPARE(body.value(QStringLiteral("platform")).toString(), AppSignIn::platform());
    QVERIFY(!body.contains(QStringLiteral("invite")));
    // No token goes with the sign-in itself.
    QVERIFY(server.authorizations.constLast().isEmpty());

    QCOMPARE(secrets.tokens.value(Platform::desktopDeviceAccount()), QByteArray("app-token"));
    QCOMPARE(secrets.tokens.value(AppSignIn::sourceSyncAccount()), QByteArray("sync-token"));
    QCOMPARE(ServerAddress::saved(), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(ServerAddress::tokenOrigin(Platform::desktopDeviceAccount()), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(ServerAddress::tokenOrigin(AppSignIn::sourceSyncAccount()), QStringLiteral("https://thoughts.example.org"));
    // The password is not kept.
    QVERIFY(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword"))->text().isEmpty());
}

void AppSignInTest::signInToAnotherServerSetsTheLibraryAside()
{
    // A computer synchronized with one server signs in to another (26 Nov 2026: every Sync then
    // stopped with "sources on the server kept changing"): the next start keeps the Library
    // aside and begins with an empty one; Sync waits until then.
    QVERIFY(QStandardPaths::isTestModeEnabled());
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString library = home.filePath(QStringLiteral("docs"));
    qputenv("THOUGHTS_HOME", library.toUtf8());
    const auto restore = qScopeGuard([] { qunsetenv("THOUGHTS_HOME"); });
    QFile::remove(LibrarySwitch::pendingPath());
    const QDir application(Platform::applicationDataRoot());
    QVERIFY(QDir(application.filePath(QStringLiteral("thoughts-server"))).removeRecursively());
    QVERIFY(QDir().mkpath(application.filePath(QStringLiteral("thoughts-server"))));
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(application.filePath(QStringLiteral("thoughts-server/desktop-sync.sqlite")), "old checkpoint"));
    const QString manifest = QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("source-manifest.json"));
    QVERIFY(write(manifest, "{}"));
    QVERIFY(write(QDir(library).filePath(QStringLiteral("docs-notes/note.tex")), "old note"));
    QVERIFY(ServerAddress::save(QStringLiteral("https://old.example.org")));
    QVERIFY(LibrarySwitch::holdsLibrary());

    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    dialog.show();
    child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"))->setText(QStringLiteral("https://thoughts.example.org"));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername")), QStringLiteral("alex"));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword")), server.password);
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    QTRY_VERIFY(signIn->isEnabled());
    signIn->click();
    QTRY_COMPARE(signedIn.size(), 1);
    QVERIFY(LibrarySwitch::pending());
    QCOMPARE(LibrarySwitch::pendingTarget()->server, QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(LibrarySwitch::pendingTarget()->username, QStringLiteral("alex"));

    const LibrarySwitch::Result switched = LibrarySwitch::runPending();
    QVERIFY2(switched.succeeded, qPrintable(switched.error));
    const auto cleanupKept = qScopeGuard([&switched] {
        QDir(switched.keptIn).removeRecursively();
        QDir(switched.keptLibrary).removeRecursively();
    });
    QVERIFY(!LibrarySwitch::pending());
    QVERIFY(switched.keptIn.contains(QStringLiteral("old.example.org")));
    QCOMPARE(QFile(QDir(switched.keptIn).filePath(QStringLiteral("thoughts-server/desktop-sync.sqlite"))).size(), qint64(14));
    QVERIFY(QFileInfo::exists(QDir(switched.keptIn).filePath(QStringLiteral("source-manifest.json"))));
    QVERIFY(QFileInfo::exists(QDir(switched.keptLibrary).filePath(QStringLiteral("docs-notes/note.tex"))));
    QVERIFY(!QFileInfo::exists(manifest));
    QVERIFY(QDir(library).exists());
    QVERIFY(QDir(library).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot));
    QVERIFY(!LibrarySwitch::holdsLibrary());
    QCOMPARE(LibrarySwitch::origin()->server, QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(LibrarySwitch::origin()->username, QStringLiteral("alex"));

    // The same server and account again keeps the Library; another account there does not.
    const LibrarySwitch::Holder held{QStringLiteral("https://thoughts.example.org"), QStringLiteral("alex")};
    QVERIFY(!LibrarySwitch::differs(held, {QStringLiteral("https://thoughts.example.org/"), QStringLiteral("alex")}));
    QVERIFY(LibrarySwitch::differs(held, {QStringLiteral("https://thoughts.example.org"), QStringLiteral("sam")}));
    QVERIFY(!LibrarySwitch::differs({QStringLiteral("https://thoughts.example.org"), {}}, {QStringLiteral("https://thoughts.example.org"), QStringLiteral("sam")}));
    QVERIFY(LibrarySwitch::differs(held, {QStringLiteral("https://other.example.org"), QStringLiteral("alex")}));
    QFile::remove(LibrarySwitch::originPath());
}

void AppSignInTest::wrongPasswordIsShown()
{
    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    dialog.show();
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername")), QStringLiteral("alex"));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword")), QStringLiteral("not the password"));
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    auto *error = child<QLabel>(dialog, QStringLiteral("thoughtsSignInError"));
    QTRY_VERIFY(signIn->isEnabled());
    signIn->click();
    QCOMPARE(error->text(), QStringLiteral("Signing in…"));
    QVERIFY(!signIn->isEnabled());
    QTRY_COMPARE(error->text(), QStringLiteral("Wrong username or password"));
    QVERIFY(error->isVisible());
    QVERIFY(signIn->isEnabled());
    QVERIFY(dialog.isVisible());
    QCOMPARE(signedIn.size(), 0);
    QVERIFY(secrets.tokens.isEmpty());

    // Too many tries: when to try again.
    server.signInStatus = 429;
    server.signInBody = R"({"detail":"Too many failed authentication attempts"})";
    server.retryAfter = "600";
    signIn->click();
    QTRY_COMPARE(error->text(), QStringLiteral("Too many failed sign-ins. Try again in 10 minutes."));
    server.signInStatus = 503;
    server.signInBody = R"({"detail":"Too many sign-ins at once; try again shortly"})";
    server.retryAfter = "5";
    signIn->click();
    QTRY_COMPARE(error->text(), QStringLiteral("Too many sign-ins at once; try again shortly"));
    QVERIFY(secrets.tokens.isEmpty());
}

void AppSignInTest::unstoredTokensAreSignedOutAgain()
{
    FakeServer server;
    server.devices = {device(QStringLiteral("macos-1a2b3c4d5e6f7a8b"), QStringLiteral("Alex's Mac"), QStringLiteral("macos"), true)};
    Secrets secrets;
    secrets.writable = false;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    dialog.show();
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername")), QStringLiteral("alex"));
    QTest::keyClicks(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword")), server.password);
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    QTRY_VERIFY(signIn->isEnabled());
    signIn->click();
    auto *error = child<QLabel>(dialog, QStringLiteral("thoughtsSignInError"));
    QTRY_COMPARE(error->text(), QStringLiteral("Signed in, but the tokens could not be stored: the secret store is locked"));
    // The device the server just issued is ended again with its own token.
    QTRY_COMPARE(server.requests.constLast(), QStringLiteral("POST /v1/apps/devices/macos-1a2b3c4d5e6f7a8b/sign-out"));
    QCOMPARE(server.authorizations.constLast(), QByteArray("Bearer app-token"));
    QCOMPARE(signedIn.size(), 0);
    QVERIFY(dialog.isVisible());
}

void AppSignInTest::inviteChoosesAPassword()
{
    FakeServer server;
    server.invite = QStringLiteral("Ab-_9xYz0123456789abcdefABCDEF0123456789abc");
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    dialog.fill({QStringLiteral("https://thoughts.example.org"), QStringLiteral("alex"), server.invite});
    dialog.show();
    QVERIFY(dialog.inviteMode());
    auto *password = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInPassword"));
    auto *newPassword = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInNewPassword"));
    auto *again = child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInNewPasswordAgain"));
    auto *hint = child<QLabel>(dialog, QStringLiteral("thoughtsSignInPasswordHint"));
    auto *signIn = child<QPushButton>(dialog, QStringLiteral("thoughtsSignInButton"));
    QVERIFY(!password->isVisible());
    QVERIFY(newPassword->isVisible() && again->isVisible());
    QCOMPARE(newPassword->echoMode(), QLineEdit::Password);
    QCOMPARE(again->echoMode(), QLineEdit::Password);
    QCOMPARE(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername"))->text(), QStringLiteral("alex"));
    QTRY_COMPARE(child<QLabel>(dialog, QStringLiteral("thoughtsSignInServerStatus"))->text(), QStringLiteral("Thoughts server found"));
    QCOMPARE(hint->text(), QStringLiteral("At least 10 characters."));

    QTest::keyClicks(newPassword, QStringLiteral("short"));
    QTest::keyClicks(again, QStringLiteral("short"));
    QCOMPARE(hint->text(), QStringLiteral("At least 10 characters."));
    QVERIFY(!signIn->isEnabled());
    newPassword->setText(QStringLiteral("a long new password"));
    again->setText(QStringLiteral("a long new passwore"));
    QCOMPARE(hint->text(), QStringLiteral("The passwords do not match."));
    QVERIFY(!signIn->isEnabled());
    again->setText(QStringLiteral("a long new password"));
    QVERIFY(hint->text().isEmpty());
    QVERIFY(signIn->isEnabled());
    signIn->click();
    QTRY_COMPARE(signedIn.size(), 1);
    const QJsonObject body = server.bodies.constLast();
    QCOMPARE(body.value(QStringLiteral("invite")).toString(), server.invite);
    QCOMPARE(body.value(QStringLiteral("new_password")).toString(), QStringLiteral("a long new password"));
    QVERIFY(!body.contains(QStringLiteral("password")));
    QCOMPARE(server.password, QStringLiteral("a long new password"));
    QCOMPARE(secrets.tokens.value(Platform::desktopDeviceAccount()), QByteArray("app-token"));

    // A used invite says so.
    SignInDialog second(secrets.hooks(server));
    second.fill({QStringLiteral("https://thoughts.example.org"), QStringLiteral("alex"), QStringLiteral("used")});
    second.show();
    child<QLineEdit>(second, QStringLiteral("thoughtsSignInNewPassword"))->setText(QStringLiteral("another long password"));
    child<QLineEdit>(second, QStringLiteral("thoughtsSignInNewPasswordAgain"))->setText(QStringLiteral("another long password"));
    auto *secondSignIn = child<QPushButton>(second, QStringLiteral("thoughtsSignInButton"));
    QTRY_VERIFY(secondSignIn->isEnabled());
    secondSignIn->click();
    QTRY_COMPARE(child<QLabel>(second, QStringLiteral("thoughtsSignInError"))->text(),
                 QStringLiteral("This invite does not work. It may have expired or been used already; ask for a new one."));
}

void AppSignInTest::linkFillsTheDialog()
{
    FakeServer server;
    Secrets secrets;
    SignInDialog dialog(secrets.hooks(server));
    QSignalSpy signedIn(&dialog, &SignInDialog::signedIn);
    const std::optional<AppSignIn::ConnectLink> link =
        AppSignIn::connectLinkFromUrl(QUrl(QStringLiteral("thoughts://connect?server=https%3A%2F%2Fthoughts.example.org&username=alex")));
    QVERIFY(link);
    dialog.fill(*link);
    dialog.show();
    // The server is in plain sight, and nothing is sent but the check.
    QCOMPARE(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInServer"))->text(), QStringLiteral("https://thoughts.example.org"));
    QVERIFY(child<QLabel>(dialog, QStringLiteral("thoughtsSignInIntro"))->isVisible());
    QCOMPARE(child<QLineEdit>(dialog, QStringLiteral("thoughtsSignInUsername"))->text(), QStringLiteral("alex"));
    QVERIFY(!dialog.inviteMode());
    QTRY_COMPARE(child<QLabel>(dialog, QStringLiteral("thoughtsSignInServerStatus"))->text(), QStringLiteral("Thoughts server found"));
    QTest::qWait(200);
    QCOMPARE(server.requests, QStringList {QStringLiteral("GET /v1/ready")});
    QCOMPARE(signedIn.size(), 0);
    QVERIFY(secrets.tokens.isEmpty());
}

// Share… of a Library project: the people and invitations the server lists, a share by username, and an
// invitation whose link is copied (the server sends no email).
void AppSignInTest::problemReports()
{
    using namespace Thoughts::ProblemReports;
    // A large window is scaled to 2560 pixels; a plain one stays a PNG.
    QImage wide(5120, 1000, QImage::Format_RGB32);
    wide.fill(Qt::white);
    const QByteArray encoded = encodedPicture(wide);
    QVERIFY(encoded.startsWith("\x89PNG"));
    QCOMPARE(QImage::fromData(encoded).size(), QSize(2560, 500));
    QVERIFY(encodedPicture(QImage()).isEmpty());
    // Logs keep their newest part, from the start of a line, within MaxLogBytes together.
    QString lines;
    for (int line = 0; line < 200; ++line) lines += QStringLiteral("line %1\n").arg(line);
    const QList<Log> kept = fitted({{QStringLiteral("big"), QString(MaxLogBytes - 100, QLatin1Char('x'))}, {QStringLiteral("lines"), lines}, {QStringLiteral("after"), QStringLiteral("left out")}});
    QCOMPARE(kept.size(), 2);
    QVERIFY(kept.at(1).text.endsWith(QStringLiteral("line 199\n")));
    QVERIFY(kept.at(1).text.startsWith(QStringLiteral("line ")));
    QVERIFY(kept.at(1).text.toUtf8().size() <= 100);
    QCOMPARE(failure(404, {}), QStringLiteral("This Thoughts server cannot take problem reports yet."));
    QCOMPARE(failure(429, R"({"detail":"This device sent 30 reports today. Try again tomorrow."})"), QStringLiteral("This device sent 30 reports today. Try again tomorrow."));

    FakeServer server;
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    QImage window(400, 300, QImage::Format_RGB32);
    window.fill(Qt::red);
    const auto logs = [] { return QList<Log> {{QStringLiteral("developer.jsonl"), QStringLiteral("{\"event\":\"sync.failed\"}\n")}}; };
    {
        ProblemReportDialog dialog(window, logs, secrets.hooks(server));
        dialog.show();
        // Without words nothing is sent.
        child<QPushButton>(dialog, QStringLiteral("problemReportSend"))->click();
        QVERIFY(!child<QLabel>(dialog, QStringLiteral("problemReportStatus"))->text().isEmpty());
        QCOMPARE(server.bodies.size(), 0);
        child<QPlainTextEdit>(dialog, QStringLiteral("problemReportDescription"))->setPlainText(QStringLiteral("  Sync never ends.\n"));
        child<QPushButton>(dialog, QStringLiteral("problemReportSend"))->click();
        QTRY_COMPARE(server.bodies.size(), 1);
        QTRY_VERIFY(!dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    const QJsonObject sent = server.bodies.constLast();
    QCOMPARE(server.requests.constLast(), QStringLiteral("POST /v1/problem-reports"));
    QCOMPARE(server.authorizations.constLast(), QByteArray("Bearer app-token"));
    QCOMPARE(sent.value(QStringLiteral("description")).toString(), QStringLiteral("Sync never ends."));
    QCOMPARE(sent.value(QStringLiteral("kind")).toString(), QStringLiteral("problem"));
    QCOMPARE(sent.value(QStringLiteral("app")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("thoughts-desktop"));
    QCOMPARE(sent.value(QStringLiteral("app")).toObject().value(QStringLiteral("platform")).toString(), AppSignIn::platform());
    const QImage picture = QImage::fromData(QByteArray::fromBase64(sent.value(QStringLiteral("screenshot")).toObject().value(QStringLiteral("data")).toString().toLatin1()));
    QCOMPARE(picture.size(), QSize(400, 300));
    QCOMPARE(sent.value(QStringLiteral("logs")).toArray().size(), 1);
    QCOMPARE(sent.value(QStringLiteral("logs")).toArray().at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("developer.jsonl"));
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (top->objectName() == QLatin1String("problemReportThanks")) top->close();
    }

    // The picture and the logs can be left out; a refusal is shown in the server's words.
    server.bodies.clear();
    server.problemReportStatus = 429;
    server.problemReportAnswer = R"({"detail":"This device sent 30 reports today. Try again tomorrow.","error_code":"too_many_reports"})";
    ProblemReportDialog dialog(window, logs, secrets.hooks(server));
    dialog.show();
    child<QComboBox>(dialog, QStringLiteral("problemReportKind"))->setCurrentIndex(1);
    child<QCheckBox>(dialog, QStringLiteral("problemReportIncludePicture"))->setChecked(false);
    child<QCheckBox>(dialog, QStringLiteral("problemReportIncludeLogs"))->setChecked(false);
    child<QPlainTextEdit>(dialog, QStringLiteral("problemReportDescription"))->setPlainText(QStringLiteral("Show what changed since yesterday"));
    child<QPushButton>(dialog, QStringLiteral("problemReportSend"))->click();
    QTRY_COMPARE(child<QLabel>(dialog, QStringLiteral("problemReportStatus"))->text(), QStringLiteral("This device sent 30 reports today. Try again tomorrow."));
    QVERIFY(dialog.isVisible());
    QVERIFY(child<QPushButton>(dialog, QStringLiteral("problemReportSend"))->isEnabled());
    QCOMPARE(server.bodies.constLast().value(QStringLiteral("kind")).toString(), QStringLiteral("idea"));
    QVERIFY(server.bodies.constLast().value(QStringLiteral("screenshot")).isNull());
    QCOMPARE(server.bodies.constLast().value(QStringLiteral("logs")).toArray().size(), 0);
}

void AppSignInTest::projectShareDialog()
{
    using namespace Thoughts::ProjectShares;
    QVERIFY(target(QStringLiteral("  Sam ")) && !target(QStringLiteral("  Sam "))->email && target(QStringLiteral("  Sam "))->value == QStringLiteral("sam"));
    QVERIFY(target(QStringLiteral("d@example.org"))->email);
    QVERIFY(!target(QStringLiteral("not an address@")) && !target(QStringLiteral("x")));
    QCOMPARE(refusal(404, QJsonObject {{QStringLiteral("error_code"), QStringLiteral("project_link_unknown_member")}}),
             QStringLiteral("No Thoughts account has this username. To invite someone new, type their email address."));

    FakeServer server;
    server.shares = QJsonObject {{QStringLiteral("name"), QStringLiteral("Notes")},
                                 {QStringLiteral("shares"), QJsonArray {QJsonObject {{QStringLiteral("link_id"), QStringLiteral("link-1")}, {QStringLiteral("username"), QStringLiteral("laura")}}}},
                                 {QStringLiteral("invitations"), QJsonArray {QJsonObject {{QStringLiteral("invitation_id"), QStringLiteral("inv-1")}, {QStringLiteral("email"), QStringLiteral("b@example.org")}}}},
                                 {QStringLiteral("spare_libraries"), 1000},
                                 {QStringLiteral("people"), QJsonArray {QJsonObject {{QStringLiteral("username"), QStringLiteral("sam")}, {QStringLiteral("email"), QStringLiteral("sam@example.org")}}}}};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    ProjectShareDialog dialog(QJsonObject {{QStringLiteral("id"), QStringLiteral("new:f5cb")}, {QStringLiteral("name"), QStringLiteral("Notes")}}, secrets.hooks(server));
    dialog.show();
    QTRY_COMPARE(dialog.findChildren<QWidget *>(QStringLiteral("projectShareRow")).size(), 1);
    QCOMPARE(dialog.findChildren<QWidget *>(QStringLiteral("projectShareInvitationRow")).size(), 1);
    QCOMPARE(server.lastQuery, QStringLiteral("project_id=new:f5cb"));
    QCOMPARE(server.authorizations.constLast(), QByteArray("Bearer app-token"));
    QCOMPARE(child<QLabel>(dialog, QStringLiteral("projectShareStatus"))->text(), QStringLiteral("Shared with:"));

    // Someone shared with before: one tap.
    server.shareAnswer = QJsonObject {{QStringLiteral("state"), QStringLiteral("shared")}, {QStringLiteral("username"), QStringLiteral("sam")}};
    auto *known = dialog.findChild<QPushButton *>(QStringLiteral("projectShareKnownPerson"));
    QVERIFY(known && known->text() == QStringLiteral("sam"));
    known->click();
    QTRY_COMPARE(server.bodies.size(), 1);
    QCOMPARE(server.bodies.constLast().value(QStringLiteral("username")).toString(), QStringLiteral("sam"));
    server.bodies.clear();

    // A username: shared at once.
    server.shareAnswer = QJsonObject {{QStringLiteral("state"), QStringLiteral("shared")}, {QStringLiteral("username"), QStringLiteral("sam")}};
    child<QLineEdit>(dialog, QStringLiteral("projectShareInput"))->setText(QStringLiteral("Sam"));
    child<QPushButton>(dialog, QStringLiteral("projectShareButton"))->click();
    QTRY_COMPARE(server.bodies.size(), 1);
    QCOMPARE(server.bodies.constLast(), (QJsonObject {{QStringLiteral("project_id"), QStringLiteral("new:f5cb")}, {QStringLiteral("username"), QStringLiteral("sam")}}));

    // An email address: the invitation's link goes to the clipboard.
    server.shareAnswer = QJsonObject {{QStringLiteral("state"), QStringLiteral("invited")},
                                      {QStringLiteral("invite_url"), QStringLiteral("https://thoughts.example.org/app/join.html#secret")},
                                      {QStringLiteral("email"), QJsonObject {{QStringLiteral("to"), QStringLiteral("c@example.org")}, {QStringLiteral("subject"), QStringLiteral("S")}, {QStringLiteral("body"), QStringLiteral("B")}}}};
    bool copied = false;
    QTimer poll;
    connect(&poll, &QTimer::timeout, &dialog, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QLatin1String("projectShareSendInvitation") && widget->isVisible()) {
                if (auto *copy = widget->findChild<QPushButton *>(QStringLiteral("projectShareCopyLink"))) {
                    copy->click();
                    copied = true;
                }
            }
        }
    });
    poll.start(20);
    child<QLineEdit>(dialog, QStringLiteral("projectShareInput"))->setText(QStringLiteral("c@example.org"));
    child<QPushButton>(dialog, QStringLiteral("projectShareButton"))->click();
    QTRY_VERIFY(copied);
    poll.stop();
    QCOMPARE(server.bodies.constLast().value(QStringLiteral("email")).toString(), QStringLiteral("c@example.org"));
    QTRY_COMPARE(QApplication::clipboard()->text(), QStringLiteral("https://thoughts.example.org/app/join.html#secret"));

    // Withdraw: the invitation by its id.
    dialog.findChild<QPushButton *>(QStringLiteral("projectShareWithdraw"))->click();
    QTRY_VERIFY(server.requests.contains(QStringLiteral("DELETE /v1/project-invitations/inv-1")));
}

void AppSignInTest::projectShareEnded()
{
    using namespace Thoughts::ProjectShares;
    // A share that ended says so, on both sides; a pending invitation is not a share (26 Nov 2026: the owner read
    // "Shared with: <address> · invited" as shared while the member edited a copy that no longer followed).
    const QLocale english(QLocale::English);
    const int year = QDate::currentDate().year();
    const QDateTime ended(QDate(year, 11, 24), QTime(12, 0), QTimeZone::UTC);
    const QString day = english.toString(ended.toLocalTime().date(), QStringLiteral("d MMM"));
    const auto link = [](const QString &id, const QString &project, const QString &role, const QString &status, const QString &peer,
                         const QString &created, const QString &endedAt, const QString &reason) {
        QJsonObject value {{QStringLiteral("link_id"), id}, {QStringLiteral("project_id"), project}, {QStringLiteral("role"), role},
                           {QStringLiteral("status"), status}, {QStringLiteral("peer_username"), peer}, {QStringLiteral("created_at"), created}};
        value.insert(QStringLiteral("ended_at"), endedAt.isEmpty() ? QJsonValue() : QJsonValue(endedAt));
        value.insert(QStringLiteral("end_reason"), reason.isEmpty() ? QJsonValue() : QJsonValue(reason));
        return value;
    };
    const QString endedAt = ended.toString(Qt::ISODateWithMs);
    const QJsonObject answer {{QStringLiteral("links"), QJsonArray {
        link(QStringLiteral("l1"), QStringLiteral("new:field-notes"), QStringLiteral("owner"), QStringLiteral("ended"), QStringLiteral("alex"), QStringLiteral("2026-11-24T10:00:00Z"), endedAt, QStringLiteral("revoke")),
        link(QStringLiteral("l2"), QStringLiteral("new:other"), QStringLiteral("owner"), QStringLiteral("ended"), QStringLiteral("alex"), QStringLiteral("2026-11-20T10:00:00Z"), QStringLiteral("2026-11-21T10:00:00Z"), QStringLiteral("revoke")),
        link(QStringLiteral("l3"), QStringLiteral("new:other"), QStringLiteral("owner"), QStringLiteral("active"), QStringLiteral("sam"), QStringLiteral("2026-11-22T10:00:00Z"), QString(), QString()),
        link(QStringLiteral("l4"), QStringLiteral("new:theirs"), QStringLiteral("member"), QStringLiteral("frozen"), QStringLiteral("alex"), QStringLiteral("2026-11-20T10:00:00Z"), endedAt, QStringLiteral("revoke")),
        link(QStringLiteral("l5"), QStringLiteral("new:left"), QStringLiteral("owner"), QStringLiteral("ended"), QStringLiteral("sam"), QStringLiteral("2026-11-20T10:00:00Z"), endedAt, QStringLiteral("leave"))}}};
    const auto states = linkStates(answer);
    QCOMPARE(states.size(), 4);
    QCOMPARE(states.value(QStringLiteral("new:other")).linkId, QStringLiteral("l3")); // a live link wins over an ended one
    QCOMPARE(linkSentence(states.value(QStringLiteral("new:other")), english), QStringLiteral("Shared with sam."));
    QCOMPARE(linkSentence(states.value(QStringLiteral("new:field-notes")), english),
             QStringLiteral("Sharing with alex ended on %1. Changes either of you make now stay on your own side.").arg(day));
    QCOMPARE(linkSentence(states.value(QStringLiteral("new:theirs")), english),
             QStringLiteral("alex stopped sharing this project on %1. This is your own copy now: your changes stay here and no longer reach alex.").arg(day));
    QCOMPARE(linkSentence(states.value(QStringLiteral("new:left")), english),
             QStringLiteral("sam left this project on %1. Changes either of you make now stay on your own side.").arg(day));
    const Invitation invitation {QStringLiteral("inv-1"), QStringLiteral("Alex@example.org"), ended};
    QCOMPARE(invitationLabel(invitation, {{QStringLiteral("sam"), QStringLiteral("sam@example.org")}}, english),
             QStringLiteral("Alex@example.org · invited %1, not accepted yet").arg(day));
    QCOMPARE(invitationLabel(invitation, {{QStringLiteral("alex"), QStringLiteral("alex@example.org")}}, english),
             QStringLiteral("Alex@example.org · invited %1, not accepted yet · already has the account alex: share with it instead").arg(day));

    // The owner's dialog: not shared, the old invitation waiting, one tap to share again.
    FakeServer server;
    server.links = answer;
    server.shares = QJsonObject {{QStringLiteral("name"), QStringLiteral("Field notes")}, {QStringLiteral("shares"), QJsonArray()},
                                 {QStringLiteral("invitations"), QJsonArray {QJsonObject {{QStringLiteral("invitation_id"), QStringLiteral("inv-1")},
                                                                                          {QStringLiteral("email"), QStringLiteral("alex@example.org")},
                                                                                          {QStringLiteral("created_at"), endedAt}}}},
                                 {QStringLiteral("spare_libraries"), 1000},
                                 {QStringLiteral("people"), QJsonArray {QJsonObject {{QStringLiteral("username"), QStringLiteral("alex")}, {QStringLiteral("email"), QStringLiteral("alex@example.org")}}}}};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    ProjectShareDialog owner(QJsonObject {{QStringLiteral("id"), QStringLiteral("new:field-notes")}, {QStringLiteral("name"), QStringLiteral("Field notes")}}, secrets.hooks(server));
    owner.show();
    QTRY_COMPARE(owner.findChildren<QWidget *>(QStringLiteral("projectShareInvitationRow")).size(), 1);
    const QString status = child<QLabel>(owner, QStringLiteral("projectShareStatus"))->text();
    QVERIFY2(status.contains(QStringLiteral("Sharing with alex ended on")), qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("Tap alex below to share again: what either of you changed since then comes over.")), qPrintable(status));
    QVERIFY2(status.endsWith(QStringLiteral("Not shared yet. Invitation waiting:")), qPrintable(status));
    QVERIFY(!status.contains(QStringLiteral("Shared with:")));
    const QWidget *row = owner.findChild<QWidget *>(QStringLiteral("projectShareInvitationRow"));
    QVERIFY(row->findChild<QLabel *>()->text().contains(QStringLiteral("already has the account alex: share with it instead")));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/project-links")));

    // The member's frozen copy: its own project now.
    server.shares = QJsonObject {{QStringLiteral("name"), QStringLiteral("Theirs")}, {QStringLiteral("shares"), QJsonArray()},
                                 {QStringLiteral("invitations"), QJsonArray()}, {QStringLiteral("people"), QJsonArray()}};
    ProjectShareDialog member(QJsonObject {{QStringLiteral("id"), QStringLiteral("new:theirs")}, {QStringLiteral("name"), QStringLiteral("Theirs")}}, secrets.hooks(server));
    member.show();
    QTRY_VERIFY(child<QLabel>(member, QStringLiteral("projectShareStatus"))->text().contains(QStringLiteral("alex stopped sharing this project on")));
    QVERIFY(!child<QLabel>(member, QStringLiteral("projectShareStatus"))->text().contains(QStringLiteral("Only you have this project.")));
}

void AppSignInTest::serverDialogListsDevices()
{
    FakeServer server;
    server.devices = {device(QStringLiteral("macos-1a2b3c4d5e6f7a8b"), QStringLiteral("Alex's MacBook Pro"), QStringLiteral("macos"), true, QStringLiteral("2026-11-18T17:11:00Z")),
                      device(QStringLiteral("android-0123456789abcdef"), QStringLiteral("Alex's Pixel"), QStringLiteral("android"), false),
                      device(QStringLiteral("web-0123456789abcdef"), QStringLiteral("Web browser (alex)"), QStringLiteral("web"), false)};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    QWidget window;
    auto *section = new ServerAccountSection(secrets.hooks(server), &window);
    window.show();
    auto *address = child<QLabel>(window, QStringLiteral("thoughtsServerAccountAddress"));
    auto *state = child<QLabel>(window, QStringLiteral("thoughtsServerAccountState"));
    auto *signIn = child<QPushButton>(window, QStringLiteral("thoughtsServerAccountSignIn"));
    auto *devices = child<QWidget>(window, QStringLiteral("thoughtsServerAccountDevices"));
    QVERIFY(address && state && signIn && devices);
    QVERIFY(section->signedIn());
    QCOMPARE(address->text(), QStringLiteral("Server: https://thoughts.example.org"));
    QCOMPARE(signIn->text(), QStringLiteral("Sign In to Another Server…"));
    QTRY_VERIFY(devices->isVisible());
    QCOMPARE(state->text(), QStringLiteral("Signed in as alex."));
    QCOMPARE(server.authorizations.constLast(), QByteArray("Bearer app-token"));
    const QList<QWidget *> rows = devices->findChildren<QWidget *>(QStringLiteral("thoughtsServerAccountDevice"));
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0)->property("deviceId").toString(), QStringLiteral("macos-1a2b3c4d5e6f7a8b"));
    const QString thisOne = rows.at(0)->findChild<QLabel *>(QStringLiteral("thoughtsServerAccountDeviceText"))->text();
    QVERIFY2(thisOne.startsWith(QStringLiteral("Alex's MacBook Pro\n")), qPrintable(thisOne));
    QVERIFY2(thisOne.contains(QStringLiteral("This ")), qPrintable(thisOne));
    QVERIFY2(thisOne.contains(QStringLiteral("last seen")), qPrintable(thisOne));
    // This computer signs out with its own button, the others with theirs.
    QVERIFY(!rows.at(0)->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountDeviceSignOut")));
    QVERIFY(rows.at(1)->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountDeviceSignOut")));
    QVERIFY(rows.at(1)->findChild<QLabel *>(QStringLiteral("thoughtsServerAccountDeviceText"))->text().contains(QStringLiteral("Android")));
    QVERIFY(rows.at(2)->findChild<QLabel *>(QStringLiteral("thoughtsServerAccountDeviceText"))->text().contains(QStringLiteral("Web browser")));
    QVERIFY(child<QPushButton>(window, QStringLiteral("thoughtsServerAccountSignOutThisComputer"))->isVisible());

    QSignalSpy signInRequested(section, &ServerAccountSection::signInRequested);
    signIn->click();
    QCOMPARE(signInRequested.size(), 1);

    // Not signed in: no devices, Sign In….
    secrets.tokens.clear();
    section->refresh();
    QCOMPARE(state->text(), QStringLiteral("Not signed in."));
    QCOMPARE(signIn->text(), QStringLiteral("Sign In…"));
    QVERIFY(!devices->isVisible());
    QTest::qWait(300);
    QVERIFY(!devices->isVisible());
}

void AppSignInTest::signOutAnotherDevice()
{
    FakeServer server;
    server.devices = {device(QStringLiteral("macos-1a2b3c4d5e6f7a8b"), QStringLiteral("Alex's MacBook Pro"), QStringLiteral("macos"), true),
                      device(QStringLiteral("android-0123456789abcdef"), QStringLiteral("Alex's Pixel"), QStringLiteral("android"), false)};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    QWidget window;
    auto *section = new ServerAccountSection(secrets.hooks(server), &window);
    QSignalSpy signedOut(section, &ServerAccountSection::signedOut);
    window.show();
    auto *devices = child<QWidget>(window, QStringLiteral("thoughtsServerAccountDevices"));
    QTRY_COMPARE(devices->findChildren<QWidget *>(QStringLiteral("thoughtsServerAccountDevice")).size(), 2);
    QPushButton *phone = devices->findChildren<QWidget *>(QStringLiteral("thoughtsServerAccountDevice"))
                             .at(1)
                             ->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountDeviceSignOut"));
    QVERIFY(phone);
    phone->click();
    // Asked first; Cancel changes nothing.
    auto *box = section->findChild<QMessageBox *>(QStringLiteral("thoughtsServerAccountSignOutConfirm"));
    QVERIFY(box);
    QVERIFY(box->text().contains(QStringLiteral("Alex's Pixel")));
    box->button(QMessageBox::Cancel)->click();
    QTest::qWait(100);
    QCOMPARE(server.requests.count(QStringLiteral("POST /v1/apps/devices/android-0123456789abcdef/sign-out")), 0);

    devices->findChildren<QWidget *>(QStringLiteral("thoughtsServerAccountDevice")).at(1)->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountDeviceSignOut"))->click();
    box = section->findChild<QMessageBox *>(QStringLiteral("thoughtsServerAccountSignOutConfirm"));
    QVERIFY(box);
    box->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountSignOutConfirmButton"))->click();
    QTRY_COMPARE(server.requests.count(QStringLiteral("POST /v1/apps/devices/android-0123456789abcdef/sign-out")), 1);
    QTRY_COMPARE(devices->findChildren<QWidget *>(QStringLiteral("thoughtsServerAccountDevice")).size(), 1);
    QCOMPARE(signedOut.size(), 0);
    QCOMPARE(secrets.tokens.value(Platform::desktopDeviceAccount()), QByteArray("app-token"));
}

void AppSignInTest::signOutOfThisComputer()
{
    FakeServer server;
    server.devices = {device(QStringLiteral("macos-1a2b3c4d5e6f7a8b"), QStringLiteral("Alex's MacBook Pro"), QStringLiteral("macos"), true)};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    secrets.tokens.insert(AppSignIn::sourceSyncAccount(), "sync-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    QWidget window;
    auto *section = new ServerAccountSection(secrets.hooks(server), &window);
    QSignalSpy signedOut(section, &ServerAccountSection::signedOut);
    window.show();
    auto *signOut = child<QPushButton>(window, QStringLiteral("thoughtsServerAccountSignOutThisComputer"));
    QTRY_VERIFY(signOut->isVisible());
    signOut->click();
    auto *box = section->findChild<QMessageBox *>(QStringLiteral("thoughtsServerAccountSignOutConfirm"));
    QVERIFY(box);
    QVERIFY2(box->text().contains(QStringLiteral("https://thoughts.example.org")), qPrintable(box->text()));
    box->findChild<QPushButton *>(QStringLiteral("thoughtsServerAccountSignOutConfirmButton"))->click();
    QTRY_COMPARE(signedOut.size(), 1);
    QCOMPARE(server.requests.count(QStringLiteral("POST /v1/apps/devices/macos-1a2b3c4d5e6f7a8b/sign-out")), 1);
    QVERIFY(secrets.tokens.isEmpty());
    QCOMPARE(child<QLabel>(window, QStringLiteral("thoughtsServerAccountState"))->text(), QStringLiteral("Not signed in."));
    QCOMPARE(child<QPushButton>(window, QStringLiteral("thoughtsServerAccountSignIn"))->text(), QStringLiteral("Sign In…"));
}

void AppSignInTest::oldServerHasNoDevices()
{
    FakeServer server;
    server.capabilities = {QStringLiteral("sync_state_v2")};
    Secrets secrets;
    secrets.tokens.insert(Platform::desktopDeviceAccount(), "app-token");
    QVERIFY(ServerAddress::save(QStringLiteral("https://thoughts.example.org")));
    QWidget window;
    auto *section = new ServerAccountSection(secrets.hooks(server), &window);
    QSignalSpy checked(section, &ServerAccountSection::serverChecked);
    window.show();
    QTRY_COMPARE(checked.size(), 1);
    QCOMPARE(checked.at(0).at(0).value<AppSignIn::ServerCheck>(), AppSignIn::ServerCheck::TooOld);
    QTest::qWait(200);
    QVERIFY(!child<QWidget>(window, QStringLiteral("thoughtsServerAccountDevices"))->isVisible());
    QCOMPARE(server.requests.count(QStringLiteral("GET /v1/apps/devices")), 0);
    // A token entered by hand still connects: it is "signed in" without a list.
    QCOMPARE(child<QLabel>(window, QStringLiteral("thoughtsServerAccountState"))->text(), QStringLiteral("Signed in."));

    // Without a token: a device token is the way in.
    secrets.tokens.clear();
    section->refresh();
    QTRY_COMPARE(child<QLabel>(window, QStringLiteral("thoughtsServerAccountState"))->text(), QStringLiteral("This server is too old for signing in; ask for a device token."));
}

QTEST_MAIN(AppSignInTest)
#include "appsignintest.moc"
