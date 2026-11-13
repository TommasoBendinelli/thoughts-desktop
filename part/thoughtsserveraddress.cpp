// SPDX-License-Identifier: GPL-2.0-or-later
#include "thoughtsserveraddress.h"

#include "thoughtsplatform.h"

#include "config-thoughts.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace Thoughts::ServerAddress
{

namespace
{
constexpr auto addressKey = "server_url";
constexpr auto tokenOriginsKey = "token_origins";
// The origin of tokens stored before their account got an entry of its own.
constexpr auto everyAccount = "*";
constexpr auto originKey = "origin";
constexpr auto fingerprintKey = "token_fingerprint";
constexpr qint64 maximumStoreBytes = 64 * 1024;

QJsonObject readStore(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumStoreBytes) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeStore(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Cannot create %1.").arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        }
        return false;
    }
    if (!Thoughts::Platform::replaceFileContents(path, QJsonDocument(object).toJson(QJsonDocument::Indented))) {
        if (error) {
            *error = QStringLiteral("Cannot save %1.").arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    return true;
}

// Enough of a token's hash to tell that another token was stored; not the token.
QString tokenFingerprint(const QByteArray &token)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("thoughts-token-binding-v1\0", 26));
    hash.addData(token);
    return QString::fromLatin1(hash.result().toHex().left(32));
}

QString recordedOrigin(const QJsonObject &origins, const QString &account)
{
    return origin(origins.value(account).toObject().value(QLatin1String(originKey)).toString());
}
}

QString defaultUrl()
{
    return QStringLiteral(THOUGHTS_DEFAULT_SERVER_URL);
}

QString legacyUrl()
{
    return QStringLiteral(THOUGHTS_LEGACY_SERVER_URL);
}

QString normalized(const QString &address, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return QString();
    };
    const QString trimmed = address.trimmed();
    if (trimmed.isEmpty()) {
        return fail(QStringLiteral("Enter the server address, for example %1.").arg(defaultUrl()));
    }
    QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative()) {
        return fail(QStringLiteral("Enter a complete address, for example %1.").arg(defaultUrl()));
    }
    if (url.scheme() != QLatin1String("https")) {
        return fail(QStringLiteral("The server address must start with https://."));
    }
    if (url.host().isEmpty()) {
        return fail(QStringLiteral("The server address needs a host name."));
    }
    if (!url.userInfo().isEmpty() || trimmed.section(QLatin1String("://"), 1).section(QLatin1Char('/'), 0, 0).contains(QLatin1Char('@'))) {
        return fail(QStringLiteral("The server address must not contain a user name or password."));
    }
    if (url.hasQuery() || url.hasFragment()) {
        return fail(QStringLiteral("The server address must not contain ? or #."));
    }
    QString path = url.path(QUrl::FullyEncoded);
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path, QUrl::TolerantMode);
    if (error) {
        error->clear();
    }
    return url.toString(QUrl::FullyEncoded);
}

QString storePath()
{
    return QDir(Thoughts::Platform::thoughtsDataRoot()).filePath(QStringLiteral("server.json"));
}

QString saved(bool *present, const QString &path)
{
    if (present) {
        *present = QFile::exists(path);
    }
    return normalized(readStore(path).value(QLatin1String(addressKey)).toString());
}

namespace
{
// existingInstallation() for any program: thoughtsctl has an application data
// folder of its own, while the Sync state lies in the app's.
bool synchronizedBefore()
{
    if (existingInstallation()) {
        return true;
    }
    const QString data = Thoughts::Platform::thoughtsDataRoot();
    for (const QString &root : {data, QDir(QFileInfo(data).absolutePath()).filePath(QStringLiteral("thoughts"))}) {
        if (QFileInfo::exists(QDir(root).filePath(QStringLiteral("thoughts-server/repository.json")))) {
            return true;
        }
    }
    return false;
}
}

bool save(const QString &address, QString *error, const QString &path)
{
    return save(address, error, path, [] { return synchronizedBefore(); });
}

bool save(const QString &address, QString *error, const QString &path, const std::function<bool()> &existingInstallation)
{
    const QString canonical = normalized(address, error);
    if (canonical.isEmpty()) {
        return false;
    }
    const QJsonObject previous = readStore(path);
    QJsonObject origins = previous.value(QLatin1String(tokenOriginsKey)).toObject();
    // On a computer that already synchronized, the tokens stored so far were
    // issued by the server used so far: record that before the address
    // changes, so they are never sent to the new one. A computer that never
    // synchronized is still being set up, and its token is bound when it is
    // first used.
    const QString previousOrigin = origin(previous.value(QLatin1String(addressKey)).toString());
    if (!previousOrigin.isEmpty() && previousOrigin != origin(canonical) && recordedOrigin(origins, QLatin1String(everyAccount)).isEmpty() && existingInstallation
        && existingInstallation()) {
        origins.insert(QLatin1String(everyAccount), QJsonObject {{QLatin1String(originKey), previousOrigin}});
    }
    QJsonObject object {{QStringLiteral("schema_version"), 1}, {QLatin1String(addressKey), canonical}};
    if (!origins.isEmpty()) {
        object.insert(QLatin1String(tokenOriginsKey), origins);
    }
    return writeStore(path, object, error);
}

QString firstRunUrl(bool existingInstallation)
{
    return firstRunUrl(existingInstallation, defaultUrl(), legacyUrl());
}

QString firstRunUrl(bool existingInstallation, const QString &defaultAddress, const QString &legacyAddress)
{
    return existingInstallation && !legacyAddress.isEmpty() ? legacyAddress : defaultAddress;
}

bool existingInstallation()
{
    // Sync state, not the device token: a new installation stores its token
    // before its first start, while every earlier version that synchronized
    // left this file behind.
    const QDir stateRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server")));
    return QFileInfo::exists(stateRoot.filePath(QStringLiteral("repository.json")));
}

bool overriddenByEnvironment()
{
    return qEnvironmentVariableIsSet("THOUGHTS_SERVER_URL");
}

QString configured()
{
    return configured([] { return existingInstallation(); }, storePath());
}

namespace
{
// The saved address; the first time nothing is saved, firstRunUrl() is saved.
QString savedOrFirstRun(const std::function<bool()> &existingInstallation, const QString &path, const QString &defaultAddress, const QString &legacyAddress)
{
    bool present = false;
    const QString address = saved(&present, path);
    if (present) {
        // Something unreadable was saved: report it rather than replacing
        // the owner's choice.
        return address;
    }
    // Without a legacy address there is nothing to keep, so the installation
    // is not even looked at.
    const QString initial = firstRunUrl(!legacyAddress.isEmpty() && existingInstallation && existingInstallation(), defaultAddress, legacyAddress);
    // If saving fails the same choice is made again next time.
    save(initial, nullptr, path);
    return initial;
}
}

QString configured(const std::function<bool()> &existingInstallation, const QString &path)
{
    return configured(existingInstallation, path, defaultUrl(), legacyUrl());
}

QString configured(const std::function<bool()> &existingInstallation, const QString &path, const QString &defaultAddress, const QString &legacyAddress)
{
    if (overriddenByEnvironment()) {
        return qEnvironmentVariable("THOUGHTS_SERVER_URL");
    }
    return savedOrFirstRun(existingInstallation, path, defaultAddress, legacyAddress);
}

QString tokenService()
{
    return QStringLiteral("thoughts-server");
}

QString origin(const QString &address)
{
    const QUrl url(normalized(address));
    if (!url.isValid() || url.host().isEmpty()) {
        return {};
    }
    QString text = url.scheme() + QStringLiteral("://") + url.host(QUrl::FullyEncoded);
    // normalized() accepts https only, whose default port is 443.
    if (url.port(443) != 443) {
        text += QLatin1Char(':') + QString::number(url.port());
    }
    return text;
}

QString tokenOrigin(const QString &account, const QString &path)
{
    const QJsonObject origins = readStore(path).value(QLatin1String(tokenOriginsKey)).toObject();
    const QString own = recordedOrigin(origins, account);
    return own.isEmpty() ? recordedOrigin(origins, QLatin1String(everyAccount)) : own;
}

bool bindToken(const QString &account, const QByteArray &token, const QString &address, QString *error, const QString &path)
{
    const QString issuer = origin(address);
    if (issuer.isEmpty() || account.isEmpty() || account == QLatin1String(everyAccount) || token.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The token cannot be recorded for the server address %1.").arg(address);
        }
        return false;
    }
    QJsonObject object = readStore(path);
    QJsonObject origins = object.value(QLatin1String(tokenOriginsKey)).toObject();
    origins.insert(account, QJsonObject {{QLatin1String(originKey), issuer}, {QLatin1String(fingerprintKey), tokenFingerprint(token)}});
    object.insert(QLatin1String(tokenOriginsKey), origins);
    if (!object.contains(QStringLiteral("schema_version"))) {
        object.insert(QStringLiteral("schema_version"), 1);
    }
    return writeStore(path, object, error);
}

bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor)
{
    return tokenMayBeSentTo(account, token, address, issuedFor, [] { return existingInstallation(); }, storePath());
}

bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor, const std::function<bool()> &existingInstallation, const QString &path)
{
    return tokenMayBeSentTo(account, token, address, issuedFor, existingInstallation, path, defaultUrl(), legacyUrl());
}

bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor, const std::function<bool()> &existingInstallation, const QString &path,
                      const QString &defaultAddress, const QString &legacyAddress)
{
    if (issuedFor) {
        issuedFor->clear();
    }
    const QString target = origin(address);
    if (token.isEmpty() || target.isEmpty() || account.isEmpty() || account == QLatin1String(everyAccount)) {
        return false;
    }
    const QString fingerprint = tokenFingerprint(token);
    QJsonObject origins = readStore(path).value(QLatin1String(tokenOriginsKey)).toObject();
    const QJsonObject entry = origins.value(account).toObject();
    QString bound = recordedOrigin(origins, account);
    if (bound.isEmpty() || entry.value(QLatin1String(fingerprintKey)).toString() != fingerprint) {
        if (bound.isEmpty()) {
            // The first time this account's token is seen: it belongs to the
            // server whose address was saved when tokens were first bound
            // (before any later change of address, see save()).
            bound = recordedOrigin(origins, QLatin1String(everyAccount));
        } else {
            // Another token was stored since, for the server in use then.
            bound.clear();
        }
        if (bound.isEmpty()) {
            bound = origin(savedOrFirstRun(existingInstallation, path, defaultAddress, legacyAddress));
        }
        if (bound.isEmpty()) {
            // The saved address is unusable: nothing to bind the token to.
            return false;
        }
        // Read again: savedOrFirstRun() may just have written the file.
        QJsonObject object = readStore(path);
        origins = object.value(QLatin1String(tokenOriginsKey)).toObject();
        if (recordedOrigin(origins, QLatin1String(everyAccount)).isEmpty()) {
            origins.insert(QLatin1String(everyAccount), QJsonObject {{QLatin1String(originKey), bound}});
        }
        origins.insert(account, QJsonObject {{QLatin1String(originKey), bound}, {QLatin1String(fingerprintKey), fingerprint}});
        object.insert(QLatin1String(tokenOriginsKey), origins);
        // If this cannot be saved, the same decision is made again next time.
        writeStore(path, object, nullptr);
    }
    if (issuedFor) {
        *issuedFor = bound;
    }
    return bound == target;
}

QByteArray storedToken(const QString &account, const QString &address, QString *issuedFor)
{
    if (issuedFor) {
        issuedFor->clear();
    }
    const QByteArray token = Thoughts::Platform::readSecret(tokenService(), account);
    const bool allowed = tokenMayBeSentTo(account, token, address, issuedFor);
    // Bind this computer's other server token at the same moment, so a later
    // change of address finds both recorded.
    const QJsonObject origins = readStore(storePath()).value(QLatin1String(tokenOriginsKey)).toObject();
    for (const QString &other : {Thoughts::Platform::desktopDeviceAccount(), QStringLiteral("thoughts-source-sync")}) {
        if (other != account && !token.isEmpty() && recordedOrigin(origins, other).isEmpty()) {
            tokenMayBeSentTo(other, Thoughts::Platform::readSecret(tokenService(), other), address);
        }
    }
    return allowed ? token : QByteArray();
}

QString tokenWithheldMessage(const QString &account, const QString &issuedFor, const QString &address)
{
    const QString target = origin(address);
    QString message = QStringLiteral("The token stored on this computer (%1) was issued for %2 and is not sent to %3. "
                                     "Store the token of %3 with: thoughtsctl store-secret --account %1 (it reads the token from standard input)")
                          .arg(account, issuedFor, target.isEmpty() ? address : target);
    // Server… asks for this computer's device token only.
    message += account == Thoughts::Platform::desktopDeviceAccount() ? QStringLiteral(", or enter it in Server… (Thoughts menu on macOS, File menu elsewhere).") : QStringLiteral(".");
    return message;
}

}
