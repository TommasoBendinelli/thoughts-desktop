// SPDX-License-Identifier: GPL-2.0-or-later
#include "libraryswitch.h"

#include "thoughtshome.h"
#include "thoughtsplatform.h"
#include "thoughtsserveraddress.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace Thoughts::LibrarySwitch
{
namespace
{
QString canonical(const QString &address)
{
    const QString normalized = ServerAddress::normalized(address);
    return normalized.isEmpty() ? address.trimmed() : normalized;
}

std::optional<Holder> readHolder(const QString &path, const QString &key = {})
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (!key.isEmpty()) {
        object = object.value(key).toObject();
    }
    const QString server = object.value(QStringLiteral("server")).toString();
    if (server.isEmpty()) {
        return std::nullopt;
    }
    return Holder{server, object.value(QStringLiteral("username")).toString()};
}

QJsonObject holderJson(const Holder &holder)
{
    return {{QStringLiteral("server"), holder.server}, {QStringLiteral("username"), holder.username}};
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(object).toJson()) < 0 || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

// Each entry: the folder it lives in and its name there.
QList<QPair<QString, QString>> stateLocations()
{
    const QString application = Platform::applicationDataRoot();
    const QString data = Platform::thoughtsDataRoot();
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QList<QPair<QString, QString>> entries;
    for (const char *name : {"thoughts-server", "library-search.sqlite", "agent-notes", "mental-model", "outside-pdf-comments", "pdf-versions",
                             "recall-predictor.json", "review-auto-retire-notices.json", "session-starter"}) {
        entries.append({application, QString::fromLatin1(name)});
    }
    for (const char *name : {"review-before-steps", "review-skip-drafts"}) {
        entries.append({appData, QString::fromLatin1(name)});
    }
    for (const char *name : {"source-manifest.json", "source-sync-state.json"}) {
        entries.append({data, QString::fromLatin1(name)});
    }
    // The same folder can appear under two names (applicationDataRoot is thoughtsDataRoot on Windows).
    QList<QPair<QString, QString>> unique;
    for (const auto &entry : entries) {
        const QString path = QDir::cleanPath(QDir(entry.first).filePath(entry.second));
        if (std::none_of(unique.cbegin(), unique.cend(), [&path](const auto &seen) { return QDir::cleanPath(QDir(seen.first).filePath(seen.second)) == path; })) {
            unique.append(entry);
        }
    }
    return unique;
}

QString label(const Holder &holder)
{
    QString host = QUrl(holder.server).host();
    if (host.isEmpty()) {
        host = QStringLiteral("server");
    }
    const int port = QUrl(holder.server).port();
    if (port > 0) {
        host += QLatin1Char('-') + QString::number(port);
    }
    QString name = holder.username.isEmpty() ? host : host + QLatin1Char('-') + holder.username;
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return name;
}
}

QString originPath()
{
    return QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/library-origin.json"));
}

std::optional<Holder> origin()
{
    return readHolder(originPath());
}

bool recordOrigin(const Holder &holder, QString *error)
{
    return writeJson(originPath(), holderJson({canonical(holder.server), holder.username}), error);
}

bool holdsLibrary()
{
    const QDir application(Platform::applicationDataRoot());
    return QFileInfo::exists(application.filePath(QStringLiteral("thoughts-server/desktop-sync.sqlite")))
        || QFileInfo::exists(QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("source-manifest.json")));
}

bool differs(const Holder &held, const Holder &to)
{
    if (held.server.isEmpty()) {
        return false;
    }
    if (canonical(held.server) != canonical(to.server)) {
        return true;
    }
    return !held.username.isEmpty() && !to.username.isEmpty() && held.username != to.username;
}

QString pendingPath()
{
    return QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("library-switch.json"));
}

bool pending()
{
    return QFileInfo::exists(pendingPath());
}

std::optional<Holder> pendingTarget()
{
    return readHolder(pendingPath(), QStringLiteral("to"));
}

bool schedule(const Holder &from, const Holder &to, QString *error)
{
    return writeJson(pendingPath(),
                     {{QStringLiteral("from"), holderJson(from)},
                      {QStringLiteral("to"), holderJson({canonical(to.server), to.username})},
                      {QStringLiteral("requested_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}},
                     error);
}

QStringList stateEntries()
{
    QStringList names;
    for (const auto &entry : stateLocations()) {
        names.append(entry.second);
    }
    return names;
}

Result runPending()
{
    Result result;
    if (!pending()) {
        return result;
    }
    result.ran = true;
    const std::optional<Holder> from = readHolder(pendingPath(), QStringLiteral("from"));
    const std::optional<Holder> to = pendingTarget();
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString name = (from ? label(*from) : QStringLiteral("previous")) + QLatin1Char('-') + stamp;
    const QString keptIn = QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("previous-libraries/") + name);
    if (!QDir().mkpath(keptIn)) {
        result.error = QStringLiteral("The folder %1 could not be made.").arg(QDir::toNativeSeparators(keptIn));
        return result;
    }
    // Every move is a rename within one disk; any failure puts back what moved.
    QList<QPair<QString, QString>> moved;
    const auto undo = [&moved] {
        for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
            QDir().rename(it->second, it->first);
        }
    };
    const QString library = thoughtsHome();
    const QString keptLibrary = QDir::cleanPath(library) + QStringLiteral(" (") + name + QLatin1Char(')');
    if (QFileInfo(library).isDir() && !QDir(library).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (!QDir().rename(library, keptLibrary)) {
            result.error = QStringLiteral("The Library folder %1 could not be moved to %2: a file in it may be open in another program.")
                               .arg(QDir::toNativeSeparators(library), QDir::toNativeSeparators(keptLibrary));
            return result;
        }
        moved.append({library, keptLibrary});
        result.keptLibrary = keptLibrary;
    }
    for (const auto &entry : stateLocations()) {
        const QString path = QDir(entry.first).filePath(entry.second);
        if (!QFileInfo::exists(path)) {
            continue;
        }
        const QString destination = QDir(keptIn).filePath(entry.second);
        if (!QDir().rename(path, destination)) {
            undo();
            result.error = QStringLiteral("%1 could not be moved to %2: it may be open in another program.")
                               .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(destination));
            return result;
        }
        moved.append({path, destination});
    }
    QDir().mkpath(library);
    if (to && !recordOrigin(*to, &result.error)) {
        // The switch is done; the next Sync records the origin again.
        result.error.clear();
    }
    QFile::copy(pendingPath(), QDir(keptIn).filePath(QStringLiteral("library-switch.json")));
    if (!QFile::remove(pendingPath())) {
        result.error = QStringLiteral("%1 could not be removed.").arg(QDir::toNativeSeparators(pendingPath()));
        return result;
    }
    result.succeeded = true;
    result.keptIn = keptIn;
    return result;
}
}
