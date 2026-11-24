// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmlattachment.h"
#include "../part/thoughtsplatform.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QStringDecoder>
#include <QHash>
#include <QPointer>

namespace {
constexpr qint64 maximum = 100000000;
QByteArray verifiedObject(const QDir &root, const QString &digest, qint64 limit)
{
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch()) return {};
    QFile file(root.filePath(QStringLiteral("objects/") + digest));
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
    const auto bytes = file.readAll();
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == digest.toLatin1() ? bytes : QByteArray();
}
}


QString Thoughts::htmlAttachmentPath(const QUrl &link)
{
    if (link.scheme() != QLatin1String("thoughts-html") || !link.authority().isEmpty() || link.hasQuery() || link.hasFragment()) return {};
    const auto path = link.path(QUrl::FullyDecoded);
    return isHtmlSourcePath(path) ? path : QString();
}

QString Thoughts::htmlAttachmentFile(const QString &bundleRoot, const QString &pdfPath, const QString &path)
{
    if (htmlAttachmentPath(QUrl(QStringLiteral("thoughts-html:") + path)).isEmpty()) return {};
    const QDir root(bundleRoot);
    QFile pointer(root.filePath(QStringLiteral("documents/") + QFileInfo(pdfPath).fileName() + QStringLiteral(".json")));
    if (!pointer.open(QIODevice::ReadOnly) || pointer.size() > 100000) return {};
    const auto ref = QJsonDocument::fromJson(pointer.readAll()).object();
    // Local annotations can change PDF bytes while its attachment links remain valid.
    // Resolve the installed document generation, not the mutable annotated PDF hash.
    if (!QFileInfo::exists(pdfPath) || ref.value(QStringLiteral("filename")).toString() != QFileInfo(pdfPath).fileName()) return {};
    const auto manifest = QJsonDocument::fromJson(verifiedObject(root, ref.value(QStringLiteral("artifact_sha256")).toString(), 64000000)).object();
    const auto document = manifest.value(QStringLiteral("pdf")).toObject();
    if (document.value(QStringLiteral("filename")) != ref.value(QStringLiteral("filename")) || document.value(QStringLiteral("pdf_sha256")) != ref.value(QStringLiteral("pdf_sha256"))) return {};
    for (const auto &value : manifest.value(QStringLiteral("resources")).toArray()) {
        const auto resource = value.toObject();
        if (resource.value(QStringLiteral("kind")).toString() != QLatin1String("html_attachment") || resource.value(QStringLiteral("path")).toString() != path) continue;
        if (resource.value(QStringLiteral("object")) != resource.value(QStringLiteral("sha256"))) return {};
        const auto digest = resource.value(QStringLiteral("sha256")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch()) return {};
        QFile file(root.filePath(QStringLiteral("objects/") + digest));
        if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > maximum
            || file.size() != resource.value(QStringLiteral("bytes")).toInteger(-1)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        return hash.addData(&file) && hash.result().toHex() == digest.toLatin1() ? file.fileName() : QString();
    }
    return {};
}

QUrl Thoughts::serveHtmlAttachmentFile(const QString &html, QObject *owner)
{
    if (!QFileInfo(html).isFile() || QFileInfo(html).size() < 1 || QFileInfo(html).size() > maximum) return {};
    QFile source(html);
    if (!source.open(QIODevice::ReadOnly)) return {};
    QStringDecoder decoder(QStringDecoder::Utf8);
    while (!source.atEnd()) {
        const auto chunk = source.read(65536);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) return {};
        const QString decoded = decoder.decode(chunk);
        if (decoder.hasError()) return {};
    }
    // Flush a trailing partial UTF-8 sequence using an ASCII sentinel.
    const QString tail = decoder.decode(QByteArray(1, '\n'));
    if (decoder.hasError()) return {};
    auto *server = new QTcpServer(owner ? owner : QCoreApplication::instance());
    if (!server->listen(QHostAddress::LocalHost, 0)) { delete server; return {}; }
    const QByteArray route = "/" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1() + "/index.html";
    const QByteArray host = "127.0.0.1:" + QByteArray::number(server->serverPort());
    server->setMaxPendingConnections(8);
    QObject::connect(server, &QTcpServer::newConnection, server, [server, html, route, host] {
        while (auto *socket = server->nextPendingConnection()) {
            socket->setReadBufferSize(8193);
            auto *timeout = new QTimer(socket);
            timeout->setSingleShot(true);
            QObject::connect(timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
            timeout->start(60000);
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, html, route, host] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                if (request.size() > 8192) { socket->abort(); return; }
                if (!request.contains("\r\n\r\n")) { socket->setProperty("request", request); return; }
                socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                const auto lines = request.split('\n');
                const bool head = lines.first().trimmed() == "HEAD " + route + " HTTP/1.1";
                bool validHost = false;
                for (const auto &line : lines) if (line.toLower().startsWith("host:")) validHost = line.mid(5).trimmed() == host;
                const bool ok = validHost && (head || lines.first().trimmed() == "GET " + route + " HTTP/1.1");
                auto *file = new QFile(html, socket);
                if (ok && !file->open(QIODevice::ReadOnly)) { socket->abort(); return; }
                const auto size = ok ? file->size() : 9;
                QByteArray response = ok ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
                response += "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
                response += "Content-Security-Policy: sandbox allow-scripts allow-downloads; default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' data: blob:; style-src 'unsafe-inline' data:; img-src data: blob:; font-src data:; media-src data: blob:; connect-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
                response += "Content-Length: " + QByteArray::number(size) + "\r\nConnection: close\r\n\r\n";
                socket->write(response);
                if (head || !ok) {
                    if (!head) socket->write("Not found");
                    socket->disconnectFromHost();
                    return;
                }
                const auto pump = [socket, file] {
                    while (!file->atEnd() && socket->bytesToWrite() < 256 * 1024) {
                        const auto chunk = file->read(64 * 1024);
                        if (chunk.isEmpty() || socket->write(chunk) != chunk.size()) { socket->abort(); return; }
                    }
                    if (file->atEnd()) socket->disconnectFromHost();
                };
                QObject::connect(socket, &QTcpSocket::bytesWritten, socket, [pump](qint64) { pump(); });
                pump();
            });
        }
    });
    // Separate origins prevent one attachment from reading another attachment's browser state.
    if (!owner) QTimer::singleShot(12 * 60 * 60 * 1000, server, &QObject::deleteLater);
    return QUrl(QString::fromLatin1("http://" + host + route));
}

QStringList Thoughts::htmlAttachmentBundleRoots(const QString &pdfPath)
{
    QStringList roots {QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"))};
    roots.append(QDir(QFileInfo(pdfPath).absolutePath()).filePath(QStringLiteral("../.runtime/target/source-bundle")));
    return roots;
}

Thoughts::HtmlAttachmentTarget Thoughts::resolveHtmlAttachment(const QString &path, const QString &pdfPath, QString *error)
{
    HtmlAttachmentTarget target;
    if (htmlAttachmentPath(QUrl(QStringLiteral("thoughts-html:") + path)).isEmpty()) {
        if (error) *error = QStringLiteral("This HTML attachment link is invalid.");
        return target;
    }
    for (const auto &root : htmlAttachmentBundleRoots(pdfPath)) {
        target.file = htmlAttachmentFile(root, pdfPath, path);
        if (!target.file.isEmpty()) break;
    }
    if (target.file.isEmpty()) {
        if (error) *error = QStringLiteral("HTML attachment not downloaded for this PDF. Sync this document and try again.");
        return target;
    }
    target.path = path;
    target.sha256 = QFileInfo(target.file).fileName();
    return target;
}

QString Thoughts::htmlAttachmentObject(const QString &sha256, const QString &pdfPath)
{
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(sha256).hasMatch()) return {};
    for (const auto &root : htmlAttachmentBundleRoots(pdfPath)) {
        QFile file(QDir(root).filePath(QStringLiteral("objects/") + sha256));
        if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > maximum) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file) && hash.result().toHex() == sha256.toLatin1()) return file.fileName();
    }
    return {};
}

bool Thoughts::openHtmlAttachment(const QUrl &link, const QUrl &pdf, QString *error)
{
    const auto path = htmlAttachmentPath(link);
    if (path.isEmpty()) { *error = QStringLiteral("This HTML attachment link is invalid."); return false; }
    const auto target = resolveHtmlAttachment(path, pdf.toLocalFile(), error);
    if (!target.isValid()) return false;
    const auto url = serveHtmlAttachmentFile(target.file);
    if (!url.isValid() || !QDesktopServices::openUrl(url)) { *error = QStringLiteral("Could not open the default browser."); return false; }
    return true;
}

namespace
{
struct AttachmentHandler {
    QPointer<QObject> receiver;
    std::function<void(const QUrl &)> handler;
};
QHash<const QObject *, AttachmentHandler> &attachmentHandlers()
{
    static QHash<const QObject *, AttachmentHandler> handlers;
    return handlers;
}
}

void Thoughts::setHtmlAttachmentHandler(const QObject *document, QObject *receiver, std::function<void(const QUrl &)> handler)
{
    if (!document) return;
    if (!receiver || !handler) {
        attachmentHandlers().remove(document);
        return;
    }
    attachmentHandlers().insert(document, {receiver, std::move(handler)});
    // Forget the entry with either side (a later document may reuse the address).
    QObject::connect(document, &QObject::destroyed, receiver, [document] { attachmentHandlers().remove(document); });
    QObject::connect(receiver, &QObject::destroyed, [document] { attachmentHandlers().remove(document); });
}

bool Thoughts::handleHtmlAttachmentLink(const QObject *document, const QUrl &link)
{
    const auto found = attachmentHandlers().constFind(document);
    if (found == attachmentHandlers().constEnd() || !found->receiver || !found->handler) return false;
    const auto handler = found->handler;
    handler(link);
    return true;
}

namespace
{
struct WebLinkHandler {
    QPointer<QObject> receiver;
    std::function<bool(const QUrl &)> handler;
};
QHash<const QObject *, WebLinkHandler> &webLinkHandlers()
{
    static QHash<const QObject *, WebLinkHandler> handlers;
    return handlers;
}
}

void Thoughts::setWebLinkHandler(const QObject *document, QObject *receiver, std::function<bool(const QUrl &)> handler)
{
    if (!document) return;
    if (!receiver || !handler) {
        webLinkHandlers().remove(document);
        return;
    }
    webLinkHandlers().insert(document, {receiver, std::move(handler)});
    // Forget the entry with either side (a later document may reuse the address).
    QObject::connect(document, &QObject::destroyed, receiver, [document] { webLinkHandlers().remove(document); });
    QObject::connect(receiver, &QObject::destroyed, [document] { webLinkHandlers().remove(document); });
}

bool Thoughts::handleWebLink(const QObject *document, const QUrl &link)
{
    const auto found = webLinkHandlers().constFind(document);
    if (found == webLinkHandlers().constEnd() || !found->receiver || !found->handler) return false;
    const auto handler = found->handler;
    return handler(link);
}
