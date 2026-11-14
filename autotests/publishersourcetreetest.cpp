/* SPDX-License-Identifier: GPL-2.0-or-later */
// The server's source tree is read once and kept between the publisher runs of a
// Sync: a kept tree is the server's while the server's source frontier is still
// that tree's (tools/publisher/sourcepublisher.cpp, currentSourceTree).
#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/pyfs.h"
#include "../tools/publisher/pyio.h"
#include "../tools/publisher/pyjson.h"
#include "../tools/publisher/pyops.h"
#include "../tools/publisher/sourceapi.h"
#include "../tools/publisher/sourcepublisher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts::Publisher;
namespace Py = Thoughts::Py;

namespace
{
// Answers GET /v1/sources and GET /v2/sync/state from `frontier` and records each request.
class TreeServer : public QObject
{
public:
    TreeServer()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    QByteArray tree() const
    {
        const QJsonObject source {{QStringLiteral("path"), QStringLiteral("docs-paper/one.tex")}, {QStringLiteral("revision"), frontier}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
                                  {QStringLiteral("object_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("deleted"), false}};
        return QJsonDocument(QJsonObject {{QStringLiteral("source_frontier"), frontier}, {QStringLiteral("sources"), QJsonArray {source}}}).toJson(QJsonDocument::Compact);
    }

    int frontier = 7;
    bool stateSupported = true;
    // The next answers are this status instead (0: the connection is closed without one).
    QList<int> failures;
    QList<QByteArray> requests;

private:
    void handle(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        if (!buffer.contains("\r\n\r\n")) {
            return;
        }
        const QByteArray method = buffer.split(' ').value(0);
        const QByteArray target = buffer.split(' ').value(1);
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        const qsizetype lengthAt = buffer.toLower().indexOf("content-length:");
        const qsizetype length = lengthAt < 0 || lengthAt > headerEnd ? 0 : buffer.mid(lengthAt + 15, buffer.indexOf('\r', lengthAt) - lengthAt - 15).trimmed().toLongLong();
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        m_buffers.remove(socket);
        requests.append(method == "GET" ? target : method + ' ' + target);
        QByteArray body;
        int status = 200;
        if (!failures.isEmpty()) {
            status = failures.takeFirst();
            if (status == 0) {
                socket->disconnectFromHost();
                return;
            }
            body = "{\"detail\":\"scripted failure\"}";
        } else if (target == "/v1/objects") {
            body = "{\"sha256\":\"" + QCryptographicHash::hash(buffer.mid(headerEnd + 4, length), QCryptographicHash::Sha256).toHex() + "\"}";
        } else if (target == "/v1/sources") {
            body = tree();
        } else if (target == "/v2/sync/state" && stateSupported) {
            body = QJsonDocument(QJsonObject {{QStringLiteral("contract_version"), 2}, {QStringLiteral("latest_server_sequence"), frontier + 3}, {QStringLiteral("library_revision"), QString(64, QLatin1Char('b'))},
                                              {QStringLiteral("source_frontier"), frontier}}).toJson(QJsonDocument::Compact);
        } else {
            status = 404;
            body = "{\"detail\":\"Not Found\"}";
        }
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Scripted\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};
}

class PublisherSourceTreeTest : public QObject
{
    Q_OBJECT

    // One publisher run: a new process reads the tree it needs.
    static Py::Json readTree(const QString &serverUrl, const QString &directory, const std::optional<qint64> &atLeast = std::nullopt)
    {
        SourceApi api(serverUrl, QStringLiteral("token"));
        SourcePublisher publisher(Py::Path(directory + QStringLiteral("/docs")), Py::Path(directory + QStringLiteral("/state/source-manifest.json")),
                                  Py::Path(directory + QStringLiteral("/state/source-publication.json")), api);
        return publisher.currentSourceTree(atLeast);
    }

private Q_SLOTS:
    void keptTreeIsUsedWhileTheFrontierStands()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = QDir(temporary.path()).canonicalPath();
        QVERIFY(QDir(directory).mkpath(QStringLiteral("docs")) && QDir(directory).mkpath(QStringLiteral("state")));
        TreeServer server;
        using Requests = QList<QByteArray>;

        // The first run reads the tree and keeps it.
        Py::Json tree = readTree(server.url(), directory);
        QCOMPARE(Py::dumpsChecked(tree), Py::dumpsChecked(Py::loadsBytes(server.tree())));
        QCOMPARE(server.requests, Requests {"/v1/sources"});
        QVERIFY(QFile::exists(directory + QStringLiteral("/state/source-tree-cache.json")));

        // The next run of the same Sync asks only for the frontier: the same tree, without
        // reading its 781 KB again.
        server.requests.clear();
        const Py::Json again = readTree(server.url(), directory);
        QCOMPARE(Py::dumpsChecked(again), Py::dumpsChecked(tree));
        QCOMPARE(server.requests, Requests {"/v2/sync/state"});
        // The installer asks for a tree at least as new as the state the app saw.
        server.requests.clear();
        QCOMPARE(Py::dumpsChecked(readTree(server.url(), directory, 7)), Py::dumpsChecked(tree));
        QCOMPARE(server.requests, Requests {"/v2/sync/state"});

        // Another device changed a source: the kept tree is not the server's any more.
        server.frontier = 8;
        server.requests.clear();
        tree = readTree(server.url(), directory);
        QCOMPARE(Py::dumpsChecked(tree), Py::dumpsChecked(Py::loadsBytes(server.tree())));
        QCOMPARE(server.requests, (Requests {"/v2/sync/state", "/v1/sources"}));
        // ... and the new one was kept.
        server.requests.clear();
        readTree(server.url(), directory);
        QCOMPARE(server.requests, Requests {"/v2/sync/state"});

        // A kept tree older than what the app already knows of is not even asked about.
        server.frontier = 9;
        server.requests.clear();
        tree = readTree(server.url(), directory, 9);
        QCOMPARE(Py::get(tree, QStringLiteral("source_frontier")), Py::Json(9));
        QCOMPARE(server.requests, Requests {"/v1/sources"});

        // A server that cannot say its frontier: the tree is read as before.
        server.stateSupported = false;
        server.requests.clear();
        readTree(server.url(), directory);
        QCOMPARE(server.requests, (Requests {"/v2/sync/state", "/v1/sources"}));
        server.stateSupported = true;

        // A tree kept for another server is not this server's.
        TreeServer other;
        other.frontier = 9;
        tree = readTree(other.url(), directory);
        QCOMPARE(other.requests, Requests {"/v1/sources"});
        server.requests.clear();
        readTree(server.url(), directory);
        QCOMPARE(server.requests, Requests {"/v1/sources"});

        // A damaged kept tree is ignored.
        QFile cache(directory + QStringLiteral("/state/source-tree-cache.json"));
        QVERIFY(cache.open(QIODevice::WriteOnly | QIODevice::Truncate));
        cache.write("{\"schema_version\":1,\"server\":");
        cache.close();
        server.requests.clear();
        tree = readTree(server.url(), directory);
        QCOMPARE(Py::get(tree, QStringLiteral("source_frontier")), Py::Json(9));
        QCOMPARE(server.requests, Requests {"/v1/sources"});
    }

    // The publisher repeats a request that changes nothing when repeated, after a failure
    // that may pass; it never repeats what the server refused, nor a request that is not
    // safe to send twice.
    void transientFailuresAreAskedAgain()
    {
        TreeServer server;
        using Requests = QList<QByteArray>;
        SourceApi::setRetryDelays({5, 10, 20});
        SourceApi api(server.url(), QStringLiteral("token"));
        const auto failure = [&api](const std::function<void()> &call) {
            try {
                call();
            } catch (const Py::Exception &error) {
                return QStringLiteral("%1 %2").arg(error.status).arg(SourceApi::transientFailure(error) ? QStringLiteral("transient") : QStringLiteral("final"));
            }
            return QStringLiteral("answered");
        };

        // 502, a closed connection and 504, then the answer.
        server.failures = {502, 0, 504};
        QCOMPARE(failure([&] { api.sourceTree(); }), QStringLiteral("answered"));
        QCOMPARE(server.requests, (Requests {"/v1/sources", "/v1/sources", "/v1/sources", "/v1/sources"}));

        // It gives up after three repetitions.
        server.requests.clear();
        server.failures = {503, 503, 503, 503};
        QCOMPARE(failure([&] { api.sourceTree(); }), QStringLiteral("503 transient"));
        QCOMPARE(server.requests.size(), 4);

        // What the server refused is not asked again.
        for (const int refused : {400, 404, 409, 422, 500}) {
            server.requests.clear();
            server.failures = {refused};
            QCOMPARE(failure([&] { api.sourceTree(); }), QStringLiteral("%1 final").arg(refused));
            QCOMPARE(server.requests.size(), 1);
        }

        // An object is stored under its digest: uploading it twice stores it once.
        server.requests.clear();
        server.failures = {503};
        QCOMPARE(failure([&] { api.uploadObject("content"); }), QStringLiteral("answered"));
        QCOMPARE(server.requests, (Requests {"POST /v1/objects", "POST /v1/objects"}));

        // A single source change and a build request are not sent twice.
        server.requests.clear();
        server.failures = {503};
        QCOMPARE(failure([&] { api.apply(Py::Json::object({{QStringLiteral("path"), Py::Json(QStringLiteral("docs-paper/one.tex"))}})); }), QStringLiteral("503 transient"));
        QCOMPARE(server.requests, Requests {"POST /v1/sources/apply"});
        server.requests.clear();
        server.failures = {502};
        QCOMPARE(failure([&] { api.requestBuild(QStringLiteral("paper.pdf"), Py::Json(7), std::nullopt); }), QStringLiteral("502 transient"));
        QCOMPARE(server.requests, Requests {"POST /v1/builds"});
        SourceApi::setRetryDelays({1000, 2000, 4000});
    }
};

QTEST_GUILESS_MAIN(PublisherSourceTreeTest)
#include "publishersourcetreetest.moc"
