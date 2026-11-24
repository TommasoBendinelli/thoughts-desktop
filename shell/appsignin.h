// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QObject;

// Signing this computer in to a Thoughts server with a username and password, invite links, and
// the list of signed-in devices (protocol/app-sign-in.md, capability app_sign_in_v1). The server
// issues the tokens; they are kept where hand-entered device tokens are kept (thoughtsserveraddress.h).
namespace Thoughts::AppSignIn
{

QString capability();
// The secret-store account of the token that publishes source folders.
QString sourceSyncAccount();
// What the server calls this computer's system: "macos", "windows" or "linux".
QString platform();
// "This Mac's name", or the same for a PC or another computer.
QString deviceNameLabel();
// What this computer is listed as unless its owner changes it: its own name.
QString defaultDeviceName();
// A device name as sent: trimmed, without control characters, at most 64 characters; empty when
// nothing is left.
QString cleanDeviceName(const QString &name);
constexpr int maximumDeviceNameLength = 64;

// A new password, as for the web app (protocol/web-app.md).
constexpr int minimumPasswordLength = 10;
constexpr int maximumPasswordLength = 256;

// thoughts://connect?server=…&username=…[&invite=…] (section 2): what the sign-in is filled
// with. Opening one never signs in by itself.
struct ConnectLink {
    QString server;
    QString username;
    QString invite;
};
// Whether @p url is meant as a connect link (thoughts://connect…), valid or not.
bool isConnectLinkUrl(const QUrl &url);
// Its parameters, decoded; nothing for any other link or a malformed one (another scheme or
// host, no server, a parameter twice, an invite that is not URL-safe base64).
std::optional<ConnectLink> connectLinkFromUrl(const QUrl &url);

// What GET /v1/ready says about an address.
enum class ServerCheck {
    Found,
    // A Thoughts server without app_sign_in_v1.
    TooOld,
    NotFound,
    // A Thoughts server that cannot answer yet (503).
    NotReady,
};
ServerCheck serverCheckFromAnswer(int httpStatus, const QByteArray &body);
QString serverCheckText(ServerCheck check);

// What POST /v1/apps/sign-in gave this computer.
struct SignedIn {
    QString username;
    QString libraryName;
    QString appDeviceId;
    QByteArray appToken;
    QString sourceSyncDeviceId;
    QByteArray sourceSyncToken;
};
// The tokens of a 200, or nothing with *error set to what to show.
std::optional<SignedIn> signedInFromAnswer(int httpStatus, const QByteArray &body, const QByteArray &retryAfter, QString *error);

// What to show for a refused request (0: no answer), from the server's detail: its text for a
// 401 or 503, the time to wait for a 429, and @p fallback when there is nothing better.
QString errorMessage(int httpStatus, const QByteArray &body, const QByteArray &retryAfter, const QString &fallback);

// One entry of GET /v1/apps/devices.
struct Device {
    QString id;
    QString name;
    QString platform;
    QString kind;
    QString username;
    QDateTime lastSeenAt;
    bool current = false;
};
// The devices of a 200 answer, newest first as the server lists them; nothing for anything else.
std::optional<QList<Device>> devicesFromAnswer(const QByteArray &body);
// "Mac", "Windows", "Android", "Web browser"…
QString platformLabel(const QString &platform);

// Where sign-in keeps tokens and where its requests go. An unset member uses the system: the
// secret store (service ServerAddress::tokenService()) and the server's own address. Under
// QStandardPaths' test mode an unset member never reaches the secret store or the network, so a
// test cannot touch this computer's real tokens or a real server.
struct Hooks {
    std::function<bool(const QString &account, const QByteArray &token, QString *error)> writeToken;
    // The token stored for @p account that may be sent to @p address.
    std::function<QByteArray(const QString &account, const QString &address)> readToken;
    std::function<bool(const QString &account, QString *error)> removeToken;
    // The URL requests for @p address (normalized) go to.
    std::function<QUrl(const QString &address)> requestUrl;
};
bool writeToken(const Hooks &hooks, const QString &account, const QByteArray &token, QString *error);
// THOUGHTS_TOKEN for this computer's device account when set, as the rest of Thoughts does.
QByteArray readToken(const Hooks &hooks, const QString &account, const QString &address);
bool removeToken(const Hooks &hooks, const QString &account, QString *error);
// The URL of @p path ("/v1/ready") on the server at @p address; empty when the address is
// invalid or nothing may be asked (test mode without a hook).
QUrl requestUrl(const Hooks &hooks, const QString &address, const QString &path);

// After signing in: saves @p address as the server (unless THOUGHTS_SERVER_URL sets it for this
// process), stores both tokens and records that they belong to that server.
bool storeSignedIn(const Hooks &hooks, const QString &address, const SignedIn &signedIn, QString *error);
// Sign Out of This Mac: removes both tokens of this computer.
bool removeStoredTokens(const Hooks &hooks, QString *error);

// Sends one request (a token unless empty; a JSON body for POST) and answers once in @p context
// with the status (0 without an answer), the body and Retry-After. Redirects are not followed:
// a password or token goes only to the address the person sees.
using Answer = std::function<void(int httpStatus, const QByteArray &body, const QByteArray &retryAfter)>;
QNetworkReply *send(QNetworkAccessManager *network, const QByteArray &method, const QUrl &url, const QByteArray &token, const QJsonObject *body, int timeoutMs,
                    QObject *context, Answer answer);

}
