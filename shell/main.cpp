/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2007 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Andy Goossens <andygoossens@telenet.be>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell.h"
#include "developerlog.h"

#include "aboutdata.h"
#include "okular_main.h"
#include "shellutils.h"
#include "thoughtscommandprotocol.h"
#include "../part/thoughtsserveraddress.h"
#if defined(Q_OS_MACOS)
#include "agentnotesnotifier.h"
#include "macquitevent.h"
#endif
#include <KAboutData>
#include <KCrash>
#include <KIconTheme>
#include <KLocalizedString>
#include <KMessageBox>
#include <KWindowSystem>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QTextStream>
#include <QtGlobal>

#define HAVE_STYLE_MANAGER __has_include(<KStyleManager>)
#if HAVE_STYLE_MANAGER
#include <KStyleManager>
#endif

#ifdef Q_OS_WIN
// Windows starts a new process for every launch, double-clicked PDF and
// thoughts:// link. Hand the command line to a running Thoughts, which owns
// the Library and its Sync, and report whether one took it.
static bool forwardToRunningThoughts(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.connectToServer(Thoughts::syncCommandSocketPath(), QIODevice::ReadWrite);
    if (!socket.waitForConnected(300)) {
        return false;
    }
    QJsonArray forwarded;
    for (const QString &argument : arguments) {
        forwarded.append(argument.startsWith(QLatin1String("thoughts:"), Qt::CaseInsensitive) ? argument : QFileInfo(argument).absoluteFilePath());
    }
    const QJsonObject request {{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("activate")}, {QStringLiteral("arguments"), forwarded}};
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(2000)) {
        return false;
    }
    // Wait for the answer so the running window can take the foreground.
    socket.waitForReadyRead(5000);
    return true;
}
#endif

int main(int argc, char **argv)
{
    /**
     * trigger initialisation of proper icon theme
     */
#if KICONTHEMES_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    KIconTheme::initTheme();
#endif

    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents);

    QApplication app(argc, argv);
    // After the application object exists: Qt sets the platform's default then.
    Shell::applyMenuIconPolicy();

    /**
     * Install event filter to handle macOS file opening.
     * This enables double-click and "Open With" on macOS.
     */
    Okular::FileOpenEventHandler eventHandler;
    app.installEventFilter(&eventHandler);
    KLocalizedString::setApplicationDomain("okular");

#if HAVE_STYLE_MANAGER
    /**
     * trigger initialisation of proper application style
     */
    KStyleManager::initStyle();
#else
    /**
     * For Windows and macOS: use Breeze if available
     * Of all tested styles that works the best for us
     */
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QApplication::setStyle(QStringLiteral("breeze"));
#endif
#endif

    KAboutData aboutData = okularAboutData();
    KAboutData::setApplicationData(aboutData);
    QApplication::setApplicationDisplayName(i18n("Thoughts"));
    // set icon for shells which do not use desktop file metadata
#if defined(Q_OS_MACOS)
    QApplication::setWindowIcon(QIcon(QApplication::applicationDirPath() + QStringLiteral("/../Resources/thoughts.icns")));
#elif defined(Q_OS_WIN)
    QIcon windowIcon;
    for (const int size : {16, 22, 32, 48, 64, 128, 256}) {
        windowIcon.addFile(QStringLiteral(":/thoughts/icons/%1-apps-thoughts.png").arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(windowIcon);
#else
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("thoughts"), QIcon::fromTheme(QStringLiteral("okular"))));
#endif

    Thoughts::DeveloperLog developerLog(Thoughts::DeveloperLog::defaultOptions());

    KCrash::initialize();

    QCommandLineParser parser;
    // The KDE4 version accepted flags such as -unique with a single dash -> preserve compatibility
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    aboutData.setupCommandLine(&parser);

    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("p") << QStringLiteral("page"), i18n("Page of the document to be shown"), QStringLiteral("number")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("presentation"), i18n("Start the document in presentation mode")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print"), i18n("Start with print dialog")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print-and-exit"), i18n("Start with print dialog and exit after printing")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("unique"), i18n("\"Unique instance\" control")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("noraise"), i18n("Not raise window")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("find"), i18n("Find a string on the text"), QStringLiteral("string")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("editor-cmd"), i18n("Sets the external editor command"), QStringLiteral("string")));
    parser.addPositionalArgument(QStringLiteral("urls"), i18n("Documents to open. Specify '-' to read from stdin."));

    parser.process(app);
    aboutData.processCommandLine(&parser);

#ifdef Q_OS_WIN
    if (forwardToRunningThoughts(parser.positionalArguments())) {
        return 0;
    }
#endif
    QStringList thoughtsLinks;

    // Settle the server address before anything can synchronize: the first
    // run of this version keeps an installation that synchronized before on
    // the address it used (see Thoughts::ServerAddress::configured).
    Thoughts::ServerAddress::configured();

    // see if we are starting with session management
    if (app.isSessionRestored()) {
        kRestoreMainWindows<Shell>();
    } else {
        // no session.. just start up normally
        QStringList paths;
        for (int i = 0; i < parser.positionalArguments().count(); ++i) {
            // macOS delivers thoughts:// links as file-open events; Windows
            // passes them on the command line, as does anyone who starts the
            // executable with one. Replay them once a window exists.
            if (parser.positionalArguments().at(i).startsWith(QLatin1String("thoughts:"), Qt::CaseInsensitive)) {
                thoughtsLinks << parser.positionalArguments().at(i);
                continue;
            }
            paths << parser.positionalArguments().at(i);
        }
        Okular::Status status = Okular::main(paths, ShellUtils::serializeOptions(parser));
        switch (status) {
        case Okular::Error:
            return -1;
        case Okular::AttachedOtherProcess:
            return 0;
        case Okular::Success:
            // Do nothing
            break;
        }
        for (const QString &link : std::as_const(thoughtsLinks)) {
            QTimer::singleShot(0, &app, [link] {
                QFileOpenEvent event{QUrl(link)};
                QCoreApplication::sendEvent(qApp, &event);
            });
        }
    }

    if (!parser.isSet(QStringLiteral("presentation")) && !parser.isSet(QStringLiteral("print-and-exit"))) {
        Shell::showCommentsOverviewOnStartup();
    }

#if defined(Q_OS_MACOS)
    Thoughts::installMacQuitEventHandler();
    // Banners for agent notes nobody opened on the phone; set up before launch finishes, so
    // a click on one that started Thoughts still reaches it.
    Thoughts::AgentNotes::installSystemNotifications();
    // Qt reports a Dock icon click as activation, even when Thoughts is already active.
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app, [](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            Shell::showWindowOnReactivation();
        }
    });
#endif

    return app.exec();
}
/* kate: replace-tabs on; indent-width 4; */
