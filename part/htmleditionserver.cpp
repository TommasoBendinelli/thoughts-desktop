// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmleditionserver.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QTcpSocket>
#include <QUuid>

namespace Thoughts
{
namespace
{
constexpr qsizetype MaxHeaderBytes = 16 * 1024;
constexpr qsizetype MaxMessageBytes = 4 * 1024 * 1024;

QByteArray mimeTypeFor(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    // The edition's own types first: QMimeDatabase guesses from content for unknown suffixes.
    if (suffix == QLatin1String("html") || suffix == QLatin1String("htm")) {
        return "text/html";
    }
    if (suffix == QLatin1String("css")) {
        return "text/css";
    }
    if (suffix == QLatin1String("svg")) {
        return "image/svg+xml";
    }
    if (suffix == QLatin1String("png")) {
        return "image/png";
    }
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        return "image/jpeg";
    }
    if (suffix == QLatin1String("woff2")) {
        return "font/woff2";
    }
    return QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().toLatin1();
}
}

HtmlEditionServer::HtmlEditionServer(QObject *parent)
    : QObject(parent)
    , m_token(QUuid::createUuid().toString(QUuid::Id128))
{
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                m_buffers.remove(socket);
                socket->deleteLater();
            });
        }
    });
}

HtmlEditionServer::~HtmlEditionServer()
{
    // Sockets are children of m_server; delete them before m_buffers goes away.
    const auto sockets = m_server.findChildren<QTcpSocket *>();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        delete socket;
    }
}

QString HtmlEditionServer::contentSecurityPolicy()
{
    // No network, no frames, no plugins. 'unsafe-inline' for styles because converters emit style
    // attributes; script-src 'none' because the edition has no scripts (runJavaScript still works).
    return QStringLiteral(
        "default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
        "font-src 'self' data:; connect-src 'self'; script-src 'none'; "
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
}

bool HtmlEditionServer::serve(const QString &bundleDir)
{
    if (!m_server.isListening() && !m_server.listen(QHostAddress::LocalHost, 0)) {
        return false;
    }
    m_bundle = QDir(bundleDir);
    m_hasBundle = !bundleDir.isEmpty() && m_bundle.exists();
    return m_hasBundle;
}

QString HtmlEditionServer::bundleDir() const
{
    return m_hasBundle ? m_bundle.absolutePath() : QString();
}

quint16 HtmlEditionServer::port() const
{
    return m_server.serverPort();
}

QUrl HtmlEditionServer::baseUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1/%2/").arg(m_server.serverPort()).arg(m_token));
}

QUrl HtmlEditionServer::entryUrl(const QString &entry) const
{
    QUrl relative;
    relative.setPath(entry.isEmpty() ? QStringLiteral("index.html") : entry);
    return baseUrl().resolved(relative);
}

QUrl HtmlEditionServer::messageUrl() const
{
    return baseUrl().resolved(QUrl(QStringLiteral("msg")));
}

void HtmlEditionServer::onReadyRead(QTcpSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    for (;;) {
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (buffer.size() > MaxHeaderBytes) {
                socket->abort();
            }
            return;
        }
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        qsizetype contentLength = 0;
        bool keepAlive = true;
        QByteArray host;
        QByteArray origin;
        for (const QByteArray &line : lines.mid(1)) {
            const qsizetype colon = line.indexOf(':');
            if (colon < 0) {
                continue;
            }
            const QByteArray key = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (key == "content-length") {
                contentLength = value.toLongLong();
            } else if (key == "connection" && value.toLower() == "close") {
                keepAlive = false;
            } else if (key == "host") {
                host = value;
            } else if (key == "origin") {
                origin = value;
            }
        }
        if (contentLength < 0 || contentLength > MaxMessageBytes) {
            socket->abort();
            return;
        }
        if (buffer.size() < headerEnd + 4 + contentLength) {
            return;
        }
        const QByteArray body = buffer.mid(headerEnd + 4, contentLength);
        buffer.remove(0, headerEnd + 4 + contentLength);
        ++m_requestsServed;
        const QByteArray method = request.value(0);
        const QByteArray target = request.value(1);
        const QByteArray self = QByteArrayLiteral("127.0.0.1:") + QByteArray::number(m_server.serverPort());
        // DNS-rebinding guard (as core/htmlattachment.cpp) and no cross-origin posts.
        if (host != self || (!origin.isEmpty() && origin != QByteArrayLiteral("http://") + self)) {
            respond(socket, 403, "text/plain", "forbidden", false);
            continue;
        }
        const QByteArray prefix = "/" + m_token.toLatin1() + "/";
        if (!target.startsWith(prefix)) {
            respond(socket, 404, "text/plain", "not found", keepAlive);
            continue;
        }
        const QString relative = QUrl::fromPercentEncoding(target.mid(prefix.size()).split('?').value(0).split('#').value(0));
        if (method == "POST" && relative == QLatin1String("msg")) {
            respond(socket, 204, "text/plain", QByteArray(), keepAlive);
            Q_EMIT message(body);
            continue;
        }
        if (method != "GET" && method != "HEAD") {
            respond(socket, 405, "text/plain", "method not allowed", keepAlive);
            continue;
        }
        if (!m_hasBundle) {
            respond(socket, 404, "text/plain", "no edition", keepAlive);
            continue;
        }
        const QString path = QDir::cleanPath(m_bundle.absoluteFilePath(relative.isEmpty() ? QStringLiteral("index.html") : relative));
        if (!path.startsWith(m_bundle.absolutePath() + QLatin1Char('/'))) {
            respond(socket, 404, "text/plain", "not found", keepAlive);
            continue;
        }
        QFile file(path);
        if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
            respond(socket, 404, "text/plain", "not found", keepAlive);
            continue;
        }
        respond(socket, 200, mimeTypeFor(path), method == "HEAD" ? QByteArray() : file.readAll(), keepAlive);
    }
}

void HtmlEditionServer::respond(QTcpSocket *socket, int code, const QByteArray &type, const QByteArray &body, bool keepAlive)
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(code) + (code < 300 ? " OK" : " Error") + "\r\n";
    response += "Content-Type: " + type + (type.startsWith("text/") || type == "image/svg+xml" ? "; charset=utf-8" : "") + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    response += "Content-Security-Policy: " + contentSecurityPolicy().toLatin1() + "\r\n";
    response += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    response += "\r\n";
    socket->write(response + body);
    if (!keepAlive) {
        socket->disconnectFromHost();
    }
}
}
