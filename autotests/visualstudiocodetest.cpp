// SPDX-License-Identifier: GPL-2.0-or-later
// Where Open Source in VS Code finds VS Code (shell/visualstudiocode.h). A Mac app
// opened from the Finder has a minimal PATH, so `code` is rarely on it; the app
// itself may be anywhere LaunchServices knows, not only in /Applications.

#include "../shell/visualstudiocode.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Thoughts::VisualStudioCode;

namespace
{
const QString vscodeId = QStringLiteral("com.microsoft.VSCode");
const QString insidersId = QStringLiteral("com.microsoft.VSCodeInsiders");
const QString codiumId = QStringLiteral("com.vscodium");

// An executable (or, with executable false, a plain) file at path.
QString makeFile(const QString &path, bool executable = true)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    file.setPermissions(executable ? QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner : QFile::ReadOwner | QFile::WriteOwner);
    return QFileInfo(path).absoluteFilePath();
}

// An app bundle at bundle whose command line is Contents/Resources/app/bin/<command>;
// no command line when command is empty.
QString makeBundle(const QString &bundle, const QString &command = QStringLiteral("code"), bool executable = true)
{
    QDir().mkpath(QDir(bundle).filePath(QStringLiteral("Contents/MacOS")));
    return command.isEmpty() ? QString() : makeFile(QDir(bundle).filePath(QStringLiteral("Contents/Resources/app/bin/") + command), executable);
}

// LaunchServices as a table; asked records the identifiers asked for, in order.
std::function<QString(const QString &)> launchServices(const QHash<QString, QString> &apps, QStringList *asked = nullptr)
{
    return [apps, asked](const QString &bundleId) {
        if (asked) {
            asked->append(bundleId);
        }
        return apps.value(bundleId);
    };
}
}

class VisualStudioCodeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Each test has folders of its own.
    void init()
    {
        m_temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(m_temporary->isValid());
        m_root = QDir(m_temporary->path());
    }

    // Nothing anywhere: nothing found, so the user is told VS Code is missing.
    void nothingFound()
    {
        Places places;
        places.pathCommand = QString();
        places.commands = {m_root.filePath(QStringLiteral("usr/local/bin/code"))};
        places.applicationForBundleId = launchServices({});
        places.applicationFolders = {m_root.filePath(QStringLiteral("Applications"))};
        const Launcher launcher = find(places);
        QVERIFY(!launcher.found());
        QVERIFY(launcher.cli.isEmpty());
        QVERIFY(launcher.bundleId.isEmpty());
    }

    // `code` on PATH wins (Thoughts started from a terminal), then the fixed places,
    // as before; LaunchServices still gives the app for the fallback.
    void pathThenFixedCommands()
    {
        const QString onPath = makeFile(m_root.filePath(QStringLiteral("path/code")));
        const QString fixed = makeFile(m_root.filePath(QStringLiteral("usr/local/bin/code")));
        const QString app = m_root.filePath(QStringLiteral("Somewhere/Visual Studio Code.app"));
        makeBundle(app);
        Places places;
        places.pathCommand = onPath;
        places.commands = {m_root.filePath(QStringLiteral("missing/code")), fixed};
        places.applicationForBundleId = launchServices({{vscodeId, app}});
        Launcher launcher = find(places);
        QCOMPARE(launcher.cli, onPath);
        QCOMPARE(launcher.bundleId, vscodeId);

        places.pathCommand.clear();
        launcher = find(places);
        QCOMPARE(launcher.cli, fixed);
        QCOMPARE(launcher.bundleId, vscodeId);
    }

    // The case that failed: no `code` command anywhere, and VS Code outside
    // /Applications (here a Downloads folder). LaunchServices knows where it is.
    void appLaunchServicesKnows()
    {
        const QString app = m_root.filePath(QStringLiteral("home/Downloads/Visual Studio Code.app"));
        const QString cli = makeBundle(app);
        QStringList asked;
        Places places;
        places.commands = {m_root.filePath(QStringLiteral("usr/local/bin/code")), m_root.filePath(QStringLiteral("opt/homebrew/bin/code"))};
        places.applicationForBundleId = launchServices({{vscodeId, app}}, &asked);
        places.applicationFolders = {m_root.filePath(QStringLiteral("Applications"))};
        const Launcher launcher = find(places);
        QCOMPARE(launcher.cli, cli);
        QCOMPARE(launcher.bundleId, vscodeId);
        QCOMPARE(asked, QStringList {vscodeId});
    }

    // VS Code first, then Insiders, then VSCodium, whose command line is codium.
    void bundleIdentifierOrder()
    {
        const QString insiders = m_root.filePath(QStringLiteral("Apps/Visual Studio Code - Insiders.app"));
        const QString insidersCli = makeBundle(insiders);
        const QString codium = m_root.filePath(QStringLiteral("Apps/VSCodium.app"));
        const QString codiumCli = makeBundle(codium, QStringLiteral("codium"));
        QStringList asked;
        Places places;
        places.applicationForBundleId = launchServices({{insidersId, insiders}, {codiumId, codium}}, &asked);
        Launcher launcher = find(places);
        QCOMPARE(asked, (QStringList {vscodeId, insidersId}));
        QCOMPARE(launcher.cli, insidersCli);
        QCOMPARE(launcher.bundleId, insidersId);

        asked.clear();
        places.applicationForBundleId = launchServices({{codiumId, codium}}, &asked);
        launcher = find(places);
        QCOMPARE(asked, (QStringList {vscodeId, insidersId, codiumId}));
        QCOMPARE(launcher.cli, codiumCli);
        QCOMPARE(launcher.bundleId, codiumId);
    }

    // An app without a command line that can run is still found: macOS opens the
    // file in it by its identifier (open -b), without the line.
    void appWithoutCommandLine()
    {
        const QString app = m_root.filePath(QStringLiteral("Apps/Visual Studio Code.app"));
        makeBundle(app, QStringLiteral("code"), false);
        Places places;
        places.applicationForBundleId = launchServices({{vscodeId, app}});
        Launcher launcher = find(places);
        QVERIFY(launcher.found());
        QVERIFY(launcher.cli.isEmpty());
        QCOMPARE(launcher.bundleId, vscodeId);

        // A later app's command line is used, and the first app stays the fallback.
        const QString codium = m_root.filePath(QStringLiteral("Apps/VSCodium.app"));
        const QString codiumCli = makeBundle(codium, QStringLiteral("codium"));
        places.applicationForBundleId = launchServices({{vscodeId, app}, {codiumId, codium}});
        launcher = find(places);
        QCOMPARE(launcher.cli, codiumCli);
        QCOMPARE(launcher.bundleId, vscodeId);
    }

    // When LaunchServices knows no VS Code, the Applications folders are searched:
    // /Applications first, then ~/Applications.
    void applicationFolders()
    {
        const QString system = m_root.filePath(QStringLiteral("Applications"));
        const QString user = m_root.filePath(QStringLiteral("home/Applications"));
        const QString userCli = makeBundle(QDir(user).filePath(QStringLiteral("Visual Studio Code.app")));
        Places places;
        places.applicationForBundleId = launchServices({});
        places.applicationFolders = {system, user};
        Launcher launcher = find(places);
        QCOMPARE(launcher.cli, userCli);
        QVERIFY(launcher.bundleId.isEmpty());

        const QString systemCli = makeBundle(QDir(system).filePath(QStringLiteral("Visual Studio Code.app")));
        launcher = find(places);
        QCOMPARE(launcher.cli, systemCli);
    }

    // This computer's places keep the PATH lookup and the fixed commands of every
    // system; on macOS they add LaunchServices and both Applications folders.
    void systemPlacesOfThisComputer()
    {
        const Places places = systemPlaces();
        QCOMPARE(places.commands, (QStringList {QStringLiteral("/usr/local/bin/code"), QStringLiteral("/opt/homebrew/bin/code")}));
#ifdef Q_OS_MACOS
        QVERIFY(places.applicationForBundleId);
        QCOMPARE(places.applicationFolders, (QStringList {QStringLiteral("/Applications"), QDir::home().filePath(QStringLiteral("Applications"))}));
        QVERIFY(applicationForBundleId(QStringLiteral("org.example.thoughts.no-such-app")).isEmpty());
        // LaunchServices answers with the bundle's path (the Finder is always there).
        QVERIFY(applicationForBundleId(QStringLiteral("com.apple.finder")).endsWith(QLatin1String("/Finder.app")));
#else
        QVERIFY(!places.applicationForBundleId);
        QVERIFY(places.applicationFolders.isEmpty());
#endif
        QVERIFY(applicationForBundleId(QString()).isEmpty());
        QVERIFY(!openWithApplication(QString(), QStringLiteral("/tmp/file.tex")));
        QVERIFY(!openWithApplication(vscodeId, QStringLiteral("relative.tex")));
    }

private:
    std::unique_ptr<QTemporaryDir> m_temporary;
    QDir m_root;
};

QTEST_GUILESS_MAIN(VisualStudioCodeTest)
#include "visualstudiocodetest.moc"
