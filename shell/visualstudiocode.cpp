// SPDX-License-Identifier: GPL-2.0-or-later
#include "visualstudiocode.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace Thoughts::VisualStudioCode
{
namespace
{
struct KnownApp {
    const char *bundleId;
    const char *bundleName;
    // The command line inside the bundle, Contents/Resources/app/bin/<name>. VS Code
    // names it code; VSCodium renames it codium. Insiders is tried under both names
    // VS Code has used for it.
    QStringList commands;
};

const QList<KnownApp> &knownApps()
{
    static const QList<KnownApp> apps {
        {"com.microsoft.VSCode", "Visual Studio Code.app", {QStringLiteral("code")}},
        {"com.microsoft.VSCodeInsiders", "Visual Studio Code - Insiders.app", {QStringLiteral("code"), QStringLiteral("code-insiders")}},
        {"com.vscodium", "VSCodium.app", {QStringLiteral("codium"), QStringLiteral("code")}},
    };
    return apps;
}

QString runnable(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
}

QString commandInBundle(const QString &bundle, const KnownApp &app)
{
    for (const QString &name : app.commands) {
        const QString command = runnable(QDir(bundle).filePath(QStringLiteral("Contents/Resources/app/bin/") + name));
        if (!command.isEmpty()) {
            return command;
        }
    }
    return {};
}
}

Places systemPlaces()
{
    Places places;
    places.pathCommand = QStandardPaths::findExecutable(QStringLiteral("code"));
    places.commands = {QStringLiteral("/usr/local/bin/code"), QStringLiteral("/opt/homebrew/bin/code")};
#ifdef Q_OS_MACOS
    places.applicationForBundleId = applicationForBundleId;
    places.applicationFolders = {QStringLiteral("/Applications"), QDir::home().filePath(QStringLiteral("Applications"))};
#endif
    return places;
}

Launcher find(const Places &places)
{
    Launcher launcher;
    launcher.cli = runnable(places.pathCommand);
    for (const QString &command : places.commands) {
        if (!launcher.cli.isEmpty()) {
            break;
        }
        launcher.cli = runnable(command);
    }
    for (const KnownApp &app : knownApps()) {
        if (!launcher.cli.isEmpty() && !launcher.bundleId.isEmpty()) {
            break;
        }
        const QString bundle = places.applicationForBundleId ? places.applicationForBundleId(QString::fromLatin1(app.bundleId)) : QString();
        if (bundle.isEmpty()) {
            continue;
        }
        if (launcher.bundleId.isEmpty()) {
            launcher.bundleId = QString::fromLatin1(app.bundleId);
        }
        if (launcher.cli.isEmpty()) {
            launcher.cli = commandInBundle(bundle, app);
        }
    }
    for (const QString &folder : places.applicationFolders) {
        for (const KnownApp &app : knownApps()) {
            if (!launcher.cli.isEmpty()) {
                return launcher;
            }
            launcher.cli = commandInBundle(QDir(folder).filePath(QString::fromLatin1(app.bundleName)), app);
        }
    }
    return launcher;
}

Launcher find()
{
    return find(systemPlaces());
}

#ifndef Q_OS_MACOS
QString applicationForBundleId(const QString &)
{
    return {};
}
#endif

bool openWithApplication(const QString &bundleId, const QString &path)
{
#ifdef Q_OS_MACOS
    // An absolute path never starts with "-", so open cannot take it for an option.
    if (bundleId.isEmpty() || !QDir::isAbsolutePath(path)) {
        return false;
    }
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-b"), bundleId, path});
#else
    Q_UNUSED(bundleId)
    Q_UNUSED(path)
    return false;
#endif
}
}
