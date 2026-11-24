// SPDX-License-Identifier: GPL-2.0-or-later
#include "appsignin.h"

#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"

#include <KLocalizedString>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace Thoughts::AppSignIn
{

namespace
{
constexpr qsizetype maximumAnswerBytes = 1024 * 1024;
constexpr qsizetype maximumDetailLength = 300;

// Plain text of at most a few lines: what the server says is shown as it is, never as markup.
QString plainText(QString text)
{
    for (QChar &character : text) {
        if (character.category() == QChar::Other_Control) {
            character = QLatin1Char(' ');
        }
    }
    text = text.simplified();
    if (text.size() > maximumDetailLength) {
        text = text.left(maximumDetailLength - 1) + QChar(0x2026);
    }
    return text;
}

// The "detail" of a FastAPI answer: a text, or for a validation error a list of {"msg"}.
QString detailText(const QByteArray &body)
{
    const QJsonValue detail = QJsonDocument::fromJson(body).object().value(QLatin1String("detail"));
    if (detail.isString()) {
        return plainText(detail.toString());
    }
    QStringList messages;
    for (const QJsonValue &entry : detail.toArray()) {
        const QString message = entry.toObject().value(QLatin1String("msg")).toString();
        if (!message.isEmpty()) {
            messages.append(message);
        }
    }
    return plainText(messages.join(QStringLiteral("; ")));
}

// Seconds to wait from a Retry-After header (seconds or an HTTP date); -1 when absent.
qint64 retryAfterSeconds(const QByteArray &value)
{
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }
    bool number = false;
    const qint64 seconds = trimmed.toLongLong(&number);
    if (number) {
        return seconds < 0 ? -1 : seconds;
    }
    const QDateTime when = QDateTime::fromString(QString::fromLatin1(trimmed), Qt::RFC2822Date);
    return when.isValid() ? std::max<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(when)) : -1;
}

QString waitText(qint64 seconds)
{
    if (seconds < 0) {
        return i18n("Try again later.");
    }
    if (seconds <= 90) {
        return i18np("Try again in 1 second.", "Try again in %1 seconds.", int(std::max<qint64>(1, seconds)));
    }
    return i18np("Try again in 1 minute.", "Try again in %1 minutes.", int(std::ceil(seconds / 60.0)));
}

bool testMode()
{
    return QStandardPaths::isTestModeEnabled();
}
}

QString capability()
{
    return QStringLiteral("app_sign_in_v1");
}

QString sourceSyncAccount()
{
    return QStringLiteral("thoughts-source-sync");
}

QString platform()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

QString deviceNameLabel()
{
#if defined(Q_OS_MACOS)
    return i18n("This Mac's name");
#elif defined(Q_OS_WIN)
    return i18n("This PC's name");
#else
    return i18n("This computer's name");
#endif
}

QString defaultDeviceName()
{
    const QString name = cleanDeviceName(Thoughts::Platform::computerName());
    return name.isEmpty() ? i18n("Thoughts on %1", platformLabel(platform())) : name;
}

QString cleanDeviceName(const QString &name)
{
    QString text;
    text.reserve(name.size());
    for (const QChar character : name) {
        if (character.category() != QChar::Other_Control) {
            text.append(character);
        }
    }
    text = text.trimmed();
    if (text.size() > maximumDeviceNameLength) {
        text = text.left(maximumDeviceNameLength).trimmed();
    }
    return text;
}

bool isConnectLinkUrl(const QUrl &url)
{
    return url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) == 0 && url.host() == QLatin1String("connect");
}

std::optional<ConnectLink> connectLinkFromUrl(const QUrl &url)
{
    if (!url.isValid() || !isConnectLinkUrl(url) || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QLatin1String("/"))) {
        return std::nullopt;
    }
    ConnectLink link;
    bool server = false;
    bool username = false;
    bool invite = false;
    const auto take = [](bool &seen, QString &field, const QString &value) {
        if (seen) {
            return false;
        }
        seen = true;
        field = value;
        return true;
    };
    for (const auto &[key, value] : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        bool fresh = true;
        if (key == QLatin1String("server")) {
            fresh = take(server, link.server, value.trimmed());
        } else if (key == QLatin1String("username")) {
            fresh = take(username, link.username, value.trimmed());
        } else if (key == QLatin1String("invite")) {
            fresh = take(invite, link.invite, value.trimmed());
        }
        // Other parameters are for later versions and are left alone.
        if (!fresh) {
            return std::nullopt;
        }
    }
    static const QRegularExpression inviteCode(QStringLiteral("^[A-Za-z0-9_-]+={0,2}$"));
    if (link.server.isEmpty() || link.server.size() > 2048 || link.username.size() > 64 || link.invite.size() > 512
        || (invite && !inviteCode.match(link.invite).hasMatch())) {
        return std::nullopt;
    }
    return link;
}

ServerCheck serverCheckFromAnswer(int httpStatus, const QByteArray &body)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    if (httpStatus == 200 && object.value(QLatin1String("capabilities")).isArray()) {
        const QJsonArray capabilities = object.value(QLatin1String("capabilities")).toArray();
        return capabilities.contains(capability()) ? ServerCheck::Found : ServerCheck::TooOld;
    }
    if (httpStatus == 503 && object.contains(QLatin1String("detail"))) {
        return ServerCheck::NotReady;
    }
    return ServerCheck::NotFound;
}

QString serverCheckText(ServerCheck check)
{
    switch (check) {
    case ServerCheck::Found:
        return i18n("Thoughts server found");
    case ServerCheck::TooOld:
        return i18n("This server is too old for signing in; ask for a device token.");
    case ServerCheck::NotReady:
        return i18n("The server is not ready yet. Try again in a moment.");
    case ServerCheck::NotFound:
        break;
    }
    return i18n("No Thoughts server at this address");
}

std::optional<SignedIn> signedInFromAnswer(int httpStatus, const QByteArray &body, const QByteArray &retryAfter, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (httpStatus == 404 || httpStatus == 405) {
        // No such route: a server before app_sign_in_v1.
        return fail(serverCheckText(ServerCheck::TooOld));
    }
    if (httpStatus != 200) {
        return fail(errorMessage(httpStatus, body, retryAfter, i18n("Signing in did not work.")));
    }
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const QJsonObject devices = object.value(QLatin1String("devices")).toObject();
    const QJsonObject app = devices.value(QLatin1String("app")).toObject();
    const QJsonObject sourceSync = devices.value(QLatin1String("source_sync")).toObject();
    SignedIn signedIn;
    signedIn.username = object.value(QLatin1String("username")).toString();
    signedIn.libraryName = object.value(QLatin1String("library")).toObject().value(QLatin1String("name")).toString();
    signedIn.appDeviceId = app.value(QLatin1String("device_id")).toString();
    signedIn.appToken = app.value(QLatin1String("token")).toString().toUtf8();
    signedIn.sourceSyncDeviceId = sourceSync.value(QLatin1String("device_id")).toString();
    signedIn.sourceSyncToken = sourceSync.value(QLatin1String("token")).toString().toUtf8();
    if (signedIn.appDeviceId.isEmpty() || signedIn.appToken.isEmpty() || signedIn.sourceSyncDeviceId.isEmpty() != signedIn.sourceSyncToken.isEmpty()) {
        return fail(i18n("The server's answer could not be read."));
    }
    if (error) {
        error->clear();
    }
    return signedIn;
}

QString errorMessage(int httpStatus, const QByteArray &body, const QByteArray &retryAfter, const QString &fallback)
{
    if (httpStatus <= 0) {
        return i18n("The server did not answer. Check the address and the connection.");
    }
    if (httpStatus == 429) {
        return i18n("Too many failed sign-ins.") + QLatin1Char(' ') + waitText(retryAfterSeconds(retryAfter));
    }
    const QString detail = detailText(body);
    if (httpStatus == 401) {
        return detail.isEmpty() ? i18n("Wrong username or password") : detail;
    }
    if (httpStatus == 503) {
        return detail.isEmpty() ? i18n("The server is busy. Try again in a moment.") : detail;
    }
    return detail.isEmpty() ? fallback : detail;
}

std::optional<QList<Device>> devicesFromAnswer(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.object().value(QLatin1String("devices")).isArray()) {
        return std::nullopt;
    }
    QList<Device> devices;
    for (const QJsonValue &value : document.object().value(QLatin1String("devices")).toArray()) {
        const QJsonObject entry = value.toObject();
        Device device;
        device.id = entry.value(QLatin1String("device_id")).toString();
        if (device.id.isEmpty()) {
            continue;
        }
        device.name = plainText(entry.value(QLatin1String("name")).toString());
        device.platform = entry.value(QLatin1String("platform")).toString();
        device.kind = entry.value(QLatin1String("kind")).toString();
        device.username = entry.value(QLatin1String("username")).toString();
        device.lastSeenAt = QDateTime::fromString(entry.value(QLatin1String("last_seen_at")).toString(), Qt::ISODateWithMs);
        device.current = entry.value(QLatin1String("current")).toBool();
        devices.append(device);
    }
    return devices;
}

QString platformLabel(const QString &platform)
{
    if (platform == QLatin1String("macos")) {
        return i18n("Mac");
    }
    if (platform == QLatin1String("windows")) {
        return i18n("Windows");
    }
    if (platform == QLatin1String("linux")) {
        return i18n("Linux");
    }
    if (platform == QLatin1String("android")) {
        return i18n("Android");
    }
    if (platform == QLatin1String("ios")) {
        return i18n("iPhone or iPad");
    }
    if (platform == QLatin1String("web")) {
        return i18n("Web browser");
    }
    return plainText(platform);
}

bool writeToken(const Hooks &hooks, const QString &account, const QByteArray &token, QString *error)
{
    if (hooks.writeToken) {
        return hooks.writeToken(account, token, error);
    }
    if (testMode()) {
        if (error) {
            *error = QStringLiteral("tests do not use the secret store");
        }
        return false;
    }
    return Thoughts::Platform::writeSecret(ServerAddress::tokenService(), account, token, error);
}

QByteArray readToken(const Hooks &hooks, const QString &account, const QString &address)
{
    if (hooks.readToken) {
        return hooks.readToken(account, address);
    }
    if (testMode()) {
        return {};
    }
    if (account == Thoughts::Platform::desktopDeviceAccount()) {
        const QByteArray environment = qgetenv("THOUGHTS_TOKEN").trimmed();
        if (!environment.isEmpty()) {
            return environment;
        }
    }
    return ServerAddress::storedToken(account, address);
}

bool removeToken(const Hooks &hooks, const QString &account, QString *error)
{
    if (hooks.removeToken) {
        return hooks.removeToken(account, error);
    }
    if (testMode()) {
        if (error) {
            *error = QStringLiteral("tests do not use the secret store");
        }
        return false;
    }
    return Thoughts::Platform::deleteSecret(ServerAddress::tokenService(), account, error);
}

QUrl requestUrl(const Hooks &hooks, const QString &address, const QString &path)
{
    const QString canonical = ServerAddress::normalized(address);
    if (canonical.isEmpty()) {
        return {};
    }
    QUrl url = hooks.requestUrl ? hooks.requestUrl(canonical) : (testMode() ? QUrl() : QUrl(canonical));
    if (!url.isValid() || url.isEmpty()) {
        return {};
    }
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    url.setPath(base + path);
    return url;
}

bool storeSignedIn(const Hooks &hooks, const QString &address, const SignedIn &signedIn, QString *error)
{
    const QString canonical = ServerAddress::normalized(address, error);
    if (canonical.isEmpty()) {
        return false;
    }
    if (!ServerAddress::overriddenByEnvironment() && !ServerAddress::save(canonical, error)) {
        return false;
    }
    const QString app = Thoughts::Platform::desktopDeviceAccount();
    const bool sourceSync = !signedIn.sourceSyncToken.isEmpty();
    if (!writeToken(hooks, app, signedIn.appToken, error) || (sourceSync && !writeToken(hooks, sourceSyncAccount(), signedIn.sourceSyncToken, error))) {
        return false;
    }
    return ServerAddress::bindToken(app, signedIn.appToken, canonical, error)
        && (!sourceSync || ServerAddress::bindToken(sourceSyncAccount(), signedIn.sourceSyncToken, canonical, error));
}

bool removeStoredTokens(const Hooks &hooks, QString *error)
{
    QString appError;
    QString sourceError;
    const bool app = removeToken(hooks, Thoughts::Platform::desktopDeviceAccount(), &appError);
    const bool source = removeToken(hooks, sourceSyncAccount(), &sourceError);
    if (error) {
        *error = !app ? appError : sourceError;
    }
    return app && source;
}

QNetworkReply *send(QNetworkAccessManager *network, const QByteArray &method, const QUrl &url, const QByteArray &token, const QJsonObject *body, int timeoutMs,
                    QObject *context, Answer answer)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(timeoutMs);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "Thoughts desktop");
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token);
    }
    QNetworkReply *reply = nullptr;
    if (body) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = network->sendCustomRequest(request, method, QJsonDocument(*body).toJson(QJsonDocument::Compact));
    } else {
        reply = network->sendCustomRequest(request, method);
    }
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, answer = std::move(answer)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->read(maximumAnswerBytes);
        const QByteArray retryAfter = reply->rawHeader("Retry-After");
        answer(status, data, retryAfter);
    });
    // Whether or not the context still exists.
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    return reply;
}

}
