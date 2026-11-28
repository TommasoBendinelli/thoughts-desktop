/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Linked into every autotest. Before main() runs, HOME and the macOS user
// domain point at a temporary directory, so QStandardPaths, KConfig and the
// docdata store never touch the user's real application data. On Windows,
// where those follow the known folders rather than HOME, QStandardPaths test
// mode moves them to qttest folders instead, and offscreen tests read
// Windows' own fonts in the Windows platform's default font. On macOS
// QStandardPaths ignores HOME and XDG_* (and test mode only rewrites paths
// below QDir::homePath()), but Foundation honours CFFIXED_USER_HOME.
// The directory is short-named: KIO places its worker sockets below it, and
// a Unix socket path must fit in 104 bytes.
//
// Parts and Shells use a temporary Library and an unreachable server with
// dummy credentials (so the keychain is never read) unless the environment
// already chooses them, as the end-to-end harness does. Startup aborts if a
// test could still reach the user's documents, application data or server.

#include "config-thoughts.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtGlobal>

#ifdef Q_OS_UNIX
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <QFont>
#include <QGuiApplication>
#endif

namespace
{
void putDefault(const char *name, const QByteArray &value)
{
    if (qEnvironmentVariableIsEmpty(name)) {
        qputenv(name, value);
    }
}

struct TestHome {
    TestHome()
    {
        if (!directory.isValid()) {
            qFatal("Cannot create a temporary home directory for the test");
        }
        // The end-to-end harness provides its own private home; keep it.
        QByteArray realHome;
#ifdef Q_OS_UNIX
        if (const passwd *entry = getpwuid(getuid())) {
            realHome = entry->pw_dir;
        }
#endif
        const QByteArray preset = qgetenv("CFFIXED_USER_HOME");
        const bool privatePreset = !preset.isEmpty() && preset == qgetenv("HOME") && !realHome.isEmpty()
            && preset != realHome && !preset.startsWith(QByteArray(realHome).append('/'));
        const QByteArray path = privatePreset ? preset : QFile::encodeName(directory.path());
        qputenv("HOME", path);
        qputenv("CFFIXED_USER_HOME", path);
        qputenv("XDG_DATA_HOME", QByteArray(path + "/.local/share"));
        qputenv("XDG_CONFIG_HOME", QByteArray(path + "/.config"));
        qputenv("XDG_CACHE_HOME", QByteArray(path + "/.cache"));
        QDir(directory.path()).mkpath(QStringLiteral("docs"));
        putDefault("THOUGHTS_HOME", QByteArray(path + "/docs"));
        putDefault("THOUGHTS_SOURCE_PUBLISHER", QByteArray(path + "/no-source-publisher"));
        putDefault("THOUGHTS_SERVER_URL", "http://127.0.0.1:9");
        putDefault("THOUGHTS_TOKEN", "autotest-token");
        // Tests never open windows on the user's screen (agents run them while the user works);
        // a run that needs the real screen sets QT_QPA_PLATFORM itself.
        putDefault("QT_QPA_PLATFORM", "offscreen");
        putDefault("THOUGHTS_SOURCE_TOKEN", "autotest-source-token");
#ifdef Q_OS_WIN
        // Windows keeps application data and settings in the known folders (%LOCALAPPDATA%, %APPDATA%), which
        // QStandardPaths and KConfig read whatever HOME says: without test mode a test writes the user's open tabs
        // (thoughts-open-tabsrc) and docdata. Test mode moves every writable location to its qttest folder.
        QStandardPaths::setTestModeEnabled(true);
        // A Thoughts the test starts does the same (shell/main.cpp).
        qputenv("THOUGHTS_AUTOTEST_APPDATA", "1");
        // Offscreen, Qt on Windows reads fonts only from its own lib/fonts folder, which Qt no longer ships:
        // without fonts a PDF a test draws has no text at all. Windows' own folder has them.
        putDefault("QT_QPA_FONTDIR", QFile::encodeName(QDir::fromNativeSeparators(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))) + QStringLiteral("/Fonts")));
#endif
    }

    QTemporaryDir directory {QStringLiteral("/tmp/thoughts-test-XXXXXX")};
};

const TestHome testHome;

void refuseRealUserData()
{
    QString realHome;
#ifdef Q_OS_UNIX
    if (const passwd *entry = getpwuid(getuid())) {
        realHome = QFile::decodeName(entry->pw_dir);
    }
#endif
    const auto inside = [](const QString &path, const QString &root) {
        const QString clean = QDir::cleanPath(path);
        return !root.isEmpty() && (clean == root || clean.startsWith(root + QLatin1Char('/')));
    };
    const QString docs = qEnvironmentVariable("THOUGHTS_HOME");
    if (docs.isEmpty() || inside(docs, QDir::cleanPath(realHome + QStringLiteral("/Documents")))) {
        qFatal("The test Library resolves to the user's documents: %s", qPrintable(docs));
    }
    for (const auto location : {QStandardPaths::GenericDataLocation, QStandardPaths::AppDataLocation, QStandardPaths::GenericConfigLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (inside(path, QDir::cleanPath(realHome + QStringLiteral("/Library")))) {
            qFatal("Test application data resolves to the user's Library: %s", qPrintable(path));
        }
    }
#ifdef Q_OS_WIN
    if (!QStandardPaths::isTestModeEnabled()) {
        qFatal("Test application data resolves to the user's AppData: QStandardPaths test mode is off");
    }
#endif
    // The build's own servers (config-thoughts.h) are the production ones.
    const QString serverHost = QUrl(qEnvironmentVariable("THOUGHTS_SERVER_URL")).host().toLower();
    for (const char *production : {THOUGHTS_DEFAULT_SERVER_URL, THOUGHTS_LEGACY_SERVER_URL}) {
        const QString productionHost = QUrl(QString::fromUtf8(production)).host().toLower();
        if (!productionHost.isEmpty() && serverHost == productionHost) {
            qFatal("Tests must not use the production server");
        }
    }
}

#ifdef Q_OS_WIN
// Offscreen, the default font is the first family in that folder, whatever the user installed: one with odd
// metrics splits every word a test reads back from its PDF ("Alp h a beta"). Segoe UI 9 is the Windows
// platform's own default, so the tests measure what the app shows.
void useWindowsDefaultFont()
{
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) && QGuiApplication::platformName() == QLatin1String("offscreen")) {
        QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI"), 9));
    }
}
#endif
}

Q_COREAPP_STARTUP_FUNCTION(refuseRealUserData)
#ifdef Q_OS_WIN
Q_COREAPP_STARTUP_FUNCTION(useWindowsDefaultFont)
#endif
