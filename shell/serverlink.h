// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace Thoughts
{

// The Thoughts server as the shell's own requests reach it with Qt networking (the agent notes,
// the held question whether the server has work): its address and this computer's device token.
class ServerLink
{
public:
    // A given server (tests). Otherwise ensure() reads THOUGHTS_SERVER_URL / THOUGHTS_TOKEN, else
    // the saved server address and the Keychain token. Under QStandardPaths' test mode only the
    // environment counts, so a test never reaches the real server with the real token.
    void setServer(const QUrl &baseUrl, const QByteArray &token);
    // Whether a server and a token are known. Read on first use, again while unset, and after
    // ServerAddress::connectionChanged(): the Mac may sign in (or out) while Thoughts runs.
    bool ensure();
    // The server refused the token: it is read again next time (unless it was given).
    void tokenRefused();
    // A request for a path of the server (with its query), with the token. A redirect to another
    // origin is not followed: the token is this server's.
    QNetworkRequest request(const QString &pathAndQuery, int timeoutMs, const QByteArray &userAgent) const;

private:
    QUrl m_baseUrl;
    QByteArray m_token;
    bool m_explicit = false;
    quint64 m_generation = 0;
};

// The capability names of a GET /v1/health answer ({"capabilities": [...]}).
QStringList serverCapabilities(const QByteArray &healthJson);

}
