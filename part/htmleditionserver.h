// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Loopback HTTP server for one HTML edition (spec "Desktop", Spike 2): serves the edition's
// bundle directory under http://127.0.0.1:<random port>/<token>/ with a strict CSP
// (script-src 'none': the edition has no scripts; the viewer is injected with runJavaScript)
// and receives the viewer's messages as POST /<token>/msg.
#include <QDir>
#include <QHash>
#include <QObject>
#include <QTcpServer>
#include <QUrl>

class QTcpSocket;

namespace Thoughts
{
class HtmlEditionServer : public QObject
{
    Q_OBJECT
public:
    explicit HtmlEditionServer(QObject *parent = nullptr);
    ~HtmlEditionServer() override;

    // Serves `bundleDir` (an extracted edition) from now on; starts listening on first use.
    bool serve(const QString &bundleDir);
    QString bundleDir() const;
    QUrl baseUrl() const; // http://127.0.0.1:<port>/<token>/
    QUrl entryUrl(const QString &entry) const;
    QUrl messageUrl() const; // http://127.0.0.1:<port>/<token>/msg
    quint16 port() const;
    static QString contentSecurityPolicy();
    int requestsServed() const
    {
        return m_requestsServed;
    }

Q_SIGNALS:
    void message(const QByteArray &body);

private:
    void onReadyRead(QTcpSocket *socket);
    void respond(QTcpSocket *socket, int code, const QByteArray &type, const QByteArray &body, bool keepAlive);

    QTcpServer m_server;
    QDir m_bundle;
    bool m_hasBundle = false;
    QString m_token;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    int m_requestsServed = 0;
};
}
