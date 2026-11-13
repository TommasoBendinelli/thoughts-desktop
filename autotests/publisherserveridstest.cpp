/* SPDX-License-Identifier: GPL-2.0-or-later */
// What the publisher accepts from the server before it becomes part of a
// file or URL path (tools/publisher/serverids.h), and what its API client
// sends when the server redirects it (tools/publisher/sourceapi.cpp).
#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/pyfs.h"
#include "../tools/publisher/serverids.h"
#include "../tools/publisher/sourceapi.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <functional>
#include <utility>

using namespace Thoughts::Publisher;
namespace Py = Thoughts::Py;

namespace
{
const QString validId = QStringLiteral("0b0e6b0a-6c1d-4f6e-9d55-0f6a3c1d2e4f");
const QString validDigest = QString(64, QLatin1Char('a'));

// Whether `call` raises Python's ValueError, as the publisher reports bad input.
bool raisesValueError(const std::function<void()> &call)
{
    try {
        call();
    } catch (const Py::Exception &error) {
        return error.type == QLatin1String("ValueError");
    }
    return false;
}

// A server that records each request and answers what `respond` returns.
class RecordingServer : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray target;
        QHash<QByteArray, QByteArray> headers; // lower-case names
    };

    RecordingServer()
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

    QList<Request> requests;
    // Returns the complete response head and body.
    std::function<QByteArray(const Request &)> respond;

private:
    void handle(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        Request request {requestLine.value(0), requestLine.value(1), {}};
        for (const QByteArray &line : lines.mid(1)) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
            }
        }
        if (buffer.size() < headerEnd + 4 + request.headers.value("content-length").toLongLong()) {
            return;
        }
        m_buffers.remove(socket);
        requests.append(request);
        socket->write(respond ? respond(request) : QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QStringList hostileValues()
{
    return {
        QString(),
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral("../../../etc/passwd"),
        QStringLiteral("/etc/passwd"),
        QStringLiteral("C:\\Windows\\system.ini"),
        QStringLiteral("a/b"),
        QStringLiteral("conflict-1"),
        QStringLiteral("None"),
        QString(validId + QStringLiteral("/..")),
        QString(validId + QLatin1Char('\n')),
        QString(QLatin1Char(' ') + validId),
        QString(validDigest + QStringLiteral("/..")),
        QString(validDigest + QLatin1Char('0')),
        validDigest.left(63),
        QString(64, QLatin1Char('A')),
        QString(QString(63, QLatin1Char('a')) + QLatin1Char('g')),
        QString(QString(62, QLatin1Char('a')) + QStringLiteral("..")),
        // 36 and 64 characters long, but not the right ones.
        QStringLiteral("../../../../../../../../../../../x/y"),
        QStringLiteral("0b0e6b0a/6c1d/4f6e/9d55/0f6a3c1d2e4f"),
        QStringLiteral("0b0e6b0a-6c1d-4f6e-9d55-0f6a3c1d2e4g"),
        QStringLiteral("0b0e6b0a6c1d-4f6e-9d55-0f6a3c1d2e4f-"),
        QStringLiteral("\u0660b0e6b0a-6c1d-4f6e-9d55-0f6a3c1d2e4f"),
    };
}
}

class PublisherServerIdsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void onlyUuidsAreConflictIds()
    {
        QVERIFY(isConflictId(validId));
        QVERIFY(isConflictId(validId.toUpper()));
        QVERIFY(isConflictId(QStringLiteral("00000000-0000-0000-0000-000000000000")));
        for (const QString &value : hostileValues()) {
            QVERIFY2(!isConflictId(value), qPrintable(value));
            QVERIFY2(raisesValueError([&] { requireConflictId(value); }), qPrintable(value));
        }
        QVERIFY(!isConflictId(validDigest));
        QCOMPARE(requireConflictId(validId), validId);
    }

    void onlyLowerCaseSha256AreObjectDigests()
    {
        QVERIFY(isObjectDigest(validDigest));
        QVERIFY(isObjectDigest(QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
        for (const QString &value : hostileValues()) {
            QVERIFY2(!isObjectDigest(value), qPrintable(value));
            QVERIFY2(raisesValueError([&] { requireObjectDigest(value); }), qPrintable(value));
        }
        QVERIFY(!isObjectDigest(validId));
        QCOMPARE(requireObjectDigest(validDigest), validDigest);
    }

    void pathsStayInsideTheStateFolder()
    {
        const Py::Path state(QStringLiteral("/data/thoughts-server"));
        QCOMPARE(conflictCopyPath(state, validId, QStringLiteral("docs-a/tex/intro.tex")).posix(),
                 QStringLiteral("/data/thoughts-server/source-conflicts/%1/docs-a/tex/intro.tex.local").arg(validId));
        QCOMPARE(cachedObjectPath(state, validDigest).posix(), QStringLiteral("/data/thoughts-server/source-bundle/objects/") + validDigest);
        for (const QString &value : hostileValues()) {
            QVERIFY2(raisesValueError([&] { conflictCopyPath(state, value, QStringLiteral("docs-a/tex/intro.tex")); }), qPrintable(value));
            QVERIFY2(raisesValueError([&] { cachedObjectPath(state, value); }), qPrintable(value));
        }
        // A UUID is no digest and a digest no conflict id.
        QVERIFY(raisesValueError([&] { conflictCopyPath(state, validDigest, QStringLiteral("a.tex")); }));
        QVERIFY(raisesValueError([&] { cachedObjectPath(state, validId); }));
    }

    void childIsOneNameInsideThePath()
    {
        const Py::Path base(QStringLiteral("/data/objects"));
        QCOMPARE(base.child(QStringLiteral("name.txt")).posix(), QStringLiteral("/data/objects/name.txt"));
        // operator/ follows pathlib: an absolute path replaces the base, and ".." stays.
        QCOMPARE((base / QStringLiteral("/etc/passwd")).posix(), QStringLiteral("/etc/passwd"));
        QCOMPARE((base / QStringLiteral("../x")).posix(), QStringLiteral("/data/objects/../x"));
        for (const QString &name : {QString(), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("/etc/passwd"), QStringLiteral("../x"), QStringLiteral("a/b"),
                                    QStringLiteral("a\\b"), QStringLiteral("C:x"), QString(QStringLiteral("a") + QChar(0) + QStringLiteral("b"))}) {
            QVERIFY2(raisesValueError([&] { base.child(name); }), qPrintable(name));
        }
    }

    void apiRefusesMalformedIdentifiersBeforeAnyRequest()
    {
        RecordingServer server;
        SourceApi api(server.url(), QStringLiteral("secret-token"), 5);
        for (const QString &value : hostileValues()) {
            QVERIFY2(raisesValueError([&] { api.download(value); }), qPrintable(value));
            QVERIFY2(raisesValueError([&] { api.hasObject(value); }), qPrintable(value));
            QVERIFY2(raisesValueError([&] { api.discardSourceConflict(value, Py::Json(1), validDigest); }), qPrintable(value));
        }
        QCOMPARE(server.requests.size(), 0);

        // Well-formed ones reach the server as before.
        server.respond = [](const RecordingServer::Request &) {
            return QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        };
        QVERIFY(!api.hasObject(validDigest));
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests.constLast().target, QByteArray("/v1/objects/") + validDigest.toLatin1());
    }

    void redirectToAnotherOriginGetsNoToken()
    {
        RecordingServer server;
        RecordingServer other;
        const QByteArray body = "[]";
        const auto answer = [body](const RecordingServer::Request &) -> QByteArray {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ") + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        };
        const auto redirect = [](const QString &location) {
            return [location](const RecordingServer::Request &) -> QByteArray {
                return QByteArray("HTTP/1.1 302 Found\r\nLocation: ") + location.toLatin1() + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            };
        };
        SourceApi api(server.url(), QStringLiteral("secret-token"), 5);

        // Within the server the token travels with the redirect, as before.
        server.respond = [&](const RecordingServer::Request &request) -> QByteArray {
            return request.target == "/v1/conflicts?status=resolved" ? redirect(QStringLiteral("/moved"))(request) : answer(request);
        };
        QVERIFY(api.resolvedConflictIds().isEmpty());
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.requests.at(1).target, QByteArray("/moved"));
        QCOMPARE(server.requests.at(1).headers.value("authorization"), QByteArray("Bearer secret-token"));

        // Another origin (here another port) is followed without it.
        server.requests.clear();
        server.respond = redirect(other.url() + QStringLiteral("/elsewhere"));
        other.respond = answer;
        QVERIFY(api.resolvedConflictIds().isEmpty());
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests.first().headers.value("authorization"), QByteArray("Bearer secret-token"));
        QCOMPARE(other.requests.size(), 1);
        QCOMPARE(other.requests.first().target, QByteArray("/elsewhere"));
        QVERIFY(!other.requests.first().headers.contains("authorization"));
        for (const QByteArray &value : other.requests.first().headers) {
            QVERIFY(!value.contains("secret-token"));
        }

        // Nor does it come back when that origin redirects to the first one again.
        server.requests.clear();
        other.requests.clear();
        bool first = true;
        server.respond = [&](const RecordingServer::Request &request) -> QByteArray {
            if (std::exchange(first, false)) {
                return redirect(other.url() + QStringLiteral("/elsewhere"))(request);
            }
            return answer(request);
        };
        other.respond = redirect(server.url() + QStringLiteral("/back"));
        QVERIFY(api.resolvedConflictIds().isEmpty());
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.requests.at(1).target, QByteArray("/back"));
        QVERIFY(!server.requests.at(1).headers.contains("authorization"));
        QVERIFY(!other.requests.first().headers.contains("authorization"));
    }
};

QTEST_GUILESS_MAIN(PublisherServerIdsTest)
#include "publisherserveridstest.moc"
