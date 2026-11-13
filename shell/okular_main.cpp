/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2007 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Andy Goossens <andygoossens@telenet.be>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "okular_main.h"

#include "aboutdata.h"
#include "shell.h"
#include "shellutils.h"
#include <KLocalizedString>
#include <KWindowSystem>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileOpenEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>

#include "config-okular.h"
#if HAVE_X11
#include <KX11Extras>
#include <private/qtx11extras_p.h>
#endif
#if HAVE_DBUS
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#endif // HAVE_DBUS

#include <iostream>

#if HAVE_DBUS
static QString startupId()
{
    QString result;
    if (KWindowSystem::isPlatformWayland()) {
        result = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
        qunsetenv("XDG_ACTIVATION_TOKEN");
    } else if (KWindowSystem::isPlatformX11()) {
#if HAVE_X11
        result = QString::fromUtf8(QX11Info::nextStartupId());
#endif
    }

    return result;
}
#endif

static bool attachUniqueInstance(const QStringList &paths, const QString &serializedOptions)
{
#if HAVE_DBUS
    if (!ShellUtils::unique(serializedOptions) || paths.count() != 1) {
        return false;
    }

    QDBusInterface iface(QStringLiteral("org.kde.okular"), QStringLiteral("/okularshell"), QStringLiteral("org.kde.okular"));
    if (!iface.isValid()) {
        return false;
    }

    if (!ShellUtils::editorCmd(serializedOptions).isEmpty()) {
        QString message =
            i18n("You cannot set the editor command in an already running okular instance. Please disable the tabs and try again. Please note, that unique is also not supported when setting the editor command at the commandline.\n");
        std::cerr << message.toStdString();
        exit(1);
    }

    const QString page = ShellUtils::page(serializedOptions);
    iface.call(QStringLiteral("openDocument"), ShellUtils::urlFromArg(paths[0], ShellUtils::qfileExistFunc(), page).url(), serializedOptions);
    if (!ShellUtils::noRaise(serializedOptions)) {
        iface.call(QStringLiteral("tryRaise"), startupId());
    }

    return true;
#else  // HAVE_DBUS
    Q_UNUSED(paths);
    Q_UNUSED(serializedOptions);
    return false;
#endif // HAVE_DBUS
}

// Ask an existing non-unique instance to open new tabs
static bool attachExistingInstance(const QStringList &paths, const QString &serializedOptions)
{
#if HAVE_DBUS
    if (paths.count() < 1) {
        return false;
    }

    // Don't try to attach to an existing instance with --print-and-exit because that would mean
    // we're going to exit that other instance and that's just rude
    if (ShellUtils::showPrintDialogAndExit(serializedOptions)) {
        return false;
    }

    // If DBus isn't running, we can't attach to an existing instance.
    auto *sessionInterface = QDBusConnection::sessionBus().interface();
    if (!sessionInterface) {
        return false;
    }

    const QStringList services = sessionInterface->registeredServiceNames().value();

    QScopedPointer<QDBusInterface> bestService;
#if HAVE_X11
    const int desktop = KWindowSystem::isPlatformX11() ? KX11Extras::currentDesktop() : 0;
#else
    const int desktop = 0;
#endif

    // Select the first instance that isn't us (metric may change in future)
    const QString ownDbus = ShellUtils::currentProcessDbusName();
    for (const QString &service : services) {
        if (service.startsWith(ShellUtils::kPerProcessDbusPrefix) && service != ownDbus) {
            bestService.reset(new QDBusInterface(service, QStringLiteral("/okularshell"), QStringLiteral("org.kde.okular")));

            // Find a window that can handle our documents
            const QDBusReply<bool> reply = bestService->call(QStringLiteral("canOpenDocs"), (int)paths.count(), desktop);
            if (reply.isValid() && reply.value()) {
                break;
            }

            bestService.reset();
        }
    }

    if (!bestService) {
        return false;
    }

    for (const QString &arg : paths) {
        // Copy stdin to temporary file which can be opened by the existing
        // window. The temp file is automatically deleted after it has been
        // opened. Not sure if this behavior is safe on all platforms.
        QScopedPointer<QTemporaryFile> tempFile;
        QString path;
        if (arg == QLatin1String("-")) {
            tempFile.reset(new QTemporaryFile);
            QFile stdinFile;
            if (!tempFile->open() || !stdinFile.open(stdin, QIODevice::ReadOnly)) {
                return false;
            }

            const size_t bufSize = 1024 * 1024;
            QScopedPointer<char, QScopedPointerArrayDeleter<char>> buf(new char[bufSize]);
            size_t bytes;
            do {
                bytes = stdinFile.read(buf.data(), bufSize);
                tempFile->write(buf.data(), bytes);
            } while (bytes != 0);

            path = tempFile->fileName();
        } else {
            // Page only makes sense if we are opening one file
            const QString page = ShellUtils::page(serializedOptions);
            path = ShellUtils::urlFromArg(arg, ShellUtils::qfileExistFunc(), page).url();
        }

        // Returns false if it can't fit another document
        const QDBusReply<bool> reply = bestService->call(QStringLiteral("openDocument"), path, serializedOptions);
        if (!reply.isValid() || !reply.value()) {
            return false;
        }
    }

    if (!ShellUtils::editorCmd(serializedOptions).isEmpty()) {
        QString message(
            i18n("You cannot set the editor command in an already running okular instance. Please disable the tabs and try again. Please note, that unique is also not supported when setting the editor command at the commandline.\n"));
        std::cerr << message.toStdString();
        exit(1);
    }

    bestService->call(QStringLiteral("tryRaise"), startupId());

    return true;
#else  // HAVE_DBUS
    Q_UNUSED(paths);
    Q_UNUSED(serializedOptions);
    return false;
#endif // HAVE_DBUS
}

namespace Okular
{
QUrl resolvedExternalOpenUrl(const QUrl &url, const QString &docsRoot)
{
    if (url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) != 0) {
        return url;
    }

    if (!url.isValid() || url.host() != QLatin1String("current-pdfs") || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment()) {
        return {};
    }

    const QString decodedPath = QUrl::fromPercentEncoding(url.path(QUrl::FullyEncoded).toUtf8());
    if (!decodedPath.startsWith(QLatin1Char('/')) || decodedPath.count(QLatin1Char('/')) != 1) {
        return {};
    }

    const QString fileName = decodedPath.mid(1);
    if (fileName.isEmpty() || fileName.contains(QLatin1Char('\\')) || QFileInfo(fileName).fileName() != fileName || QFileInfo(fileName).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return {};
    }

    const QDir currentPdfs(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QString canonicalRoot = QFileInfo(currentPdfs.absolutePath()).canonicalFilePath();
    const QFileInfo candidate(currentPdfs.filePath(fileName));
    const QString canonicalCandidate = candidate.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !candidate.isFile() || canonicalCandidate.isEmpty() || QFileInfo(canonicalCandidate).absolutePath() != canonicalRoot) {
        return {};
    }

    return QUrl::fromLocalFile(canonicalCandidate);
}

QString reviewCardIdFromUrl(const QUrl &url)
{
    if (url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) != 0 || !url.isValid() || url.host() != QLatin1String("review-card")
        || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasQuery() || url.hasFragment()) {
        return {};
    }
    const QString path = url.path(QUrl::FullyEncoded);
    if (!path.startsWith(QLatin1Char('/')) || path.count(QLatin1Char('/')) != 1) {
        return {};
    }
    const QUuid id = QUuid::fromString(QStringView(path).mid(1));
    // Only the plain 36-character form: no braces or other spellings of the id.
    if (id.isNull() || path.size() != 37) {
        return {};
    }
    return id.toString(QUuid::WithoutBraces);
}

QJsonObject agentNotesRequestFromUrl(const QUrl &url)
{
    if (url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) != 0 || !url.isValid() || url.host() != QLatin1String("agent-notes")
        || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasFragment() || (!url.path().isEmpty() && url.path() != QLatin1String("/"))) {
        return {};
    }
    QJsonObject request;
    const QUrlQuery query(url);
    for (const auto &[key, value] : query.queryItems(QUrl::FullyDecoded)) {
        const QString field = key == QLatin1String("note") ? QStringLiteral("note_id") : key;
        if ((field != QLatin1String("document") && field != QLatin1String("project") && field != QLatin1String("note_id")) || value.isEmpty() || value.size() > 512
            || request.contains(field)) {
            return {};
        }
        request.insert(field, value);
    }
    request.insert(QStringLiteral("command"), QStringLiteral("open-agent-notes"));
    return request;
}

// Opens Agent Notes as the link asks, once the first window exists.
static void showAgentNotesWhenReady(const QJsonObject &request, int attempts = 0)
{
    const bool hasShell = std::any_of(QApplication::topLevelWidgets().cbegin(), QApplication::topLevelWidgets().cend(), [](QWidget *widget) {
        return qobject_cast<Shell *>(widget);
    });
    if (hasShell) {
        const QJsonObject result = Shell::openAgentNotesCommand(request);
        if (!result.value(QStringLiteral("accepted")).toBool()) qWarning().noquote() << "thoughts://agent-notes:" << result.value(QStringLiteral("message")).toString();
    } else if (attempts < 300) {
        QTimer::singleShot(100, qApp, [request, attempts] {
            showAgentNotesWhenReady(request, attempts + 1);
        });
    }
}

// Shows the card in the Library's Review Card tab. A link that launches
// Thoughts arrives before the first window exists; wait for it rather than
// opening a second one.
static void showReviewCardWhenReady(const QString &cardId, int attempts = 0)
{
    const bool hasShell = std::any_of(QApplication::topLevelWidgets().cbegin(), QApplication::topLevelWidgets().cend(), [](QWidget *widget) {
        return qobject_cast<Shell *>(widget);
    });
    if (hasShell) {
        Shell::showReviewCardInLibrary(cardId);
    } else if (attempts < 300) {
        QTimer::singleShot(100, qApp, [cardId, attempts] {
            showReviewCardWhenReady(cardId, attempts + 1);
        });
    }
}

bool FileOpenEventHandler::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() != QEvent::FileOpen) {
        return QObject::eventFilter(object, event);
    }

    auto *fileOpenEvent = static_cast<QFileOpenEvent *>(event);
    const QUrl requestedUrl = fileOpenEvent->url();
    const QString cardId = reviewCardIdFromUrl(requestedUrl);
    if (!cardId.isEmpty()) {
        showReviewCardWhenReady(cardId);
        return true;
    }
    const QJsonObject agentNotes = agentNotesRequestFromUrl(requestedUrl);
    if (!agentNotes.isEmpty()) {
        showAgentNotesWhenReady(agentNotes);
        return true;
    }
    const QUrl openUrl = resolvedExternalOpenUrl(requestedUrl, ShellUtils::docsRoot());
    if (requestedUrl.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) == 0 && openUrl.isEmpty()) {
        return true;
    }
    const QString serializedOptions = ShellUtils::serializeOptions(false, false, false, false, false, QString(), QString(), QString());
    bool foundShell = false;

    for (QWidget *widget : QApplication::topLevelWidgets()) {
        Shell *shell = qobject_cast<Shell *>(widget);
        if (!shell) {
            continue;
        }

        foundShell = true;
        if (!shell->openDocument(openUrl, serializedOptions)) {
            continue;
        }

        shell->show();
        shell->raise();
        shell->activateWindow();
        return true;
    }

    // During initial application startup, let Qt retain its normal handling
    // until the first Shell exists. For an already-running application whose
    // occupied windows reject another document, create a new window.
    if (!foundShell) {
        return false;
    }

    Shell *shell = new Shell(serializedOptions);
    if (!shell->isValid()) {
        delete shell;
        return false;
    }

    shell->show();
    if (!shell->openDocument(openUrl, serializedOptions)) {
        delete shell;
        return false;
    }

    shell->raise();
    shell->activateWindow();
    return true;
}

Status main(const QStringList &paths, const QString &serializedOptions)
{
    if (ShellUtils::unique(serializedOptions) && paths.count() > 1) {
        QTextStream stream(stderr);
        stream << i18n("Error: Can't open more than one document with the --unique switch") << '\n';
        return Error;
    }

    if (ShellUtils::startInPresentation(serializedOptions) && paths.count() > 1) {
        QTextStream stream(stderr);
        stream << i18n("Error: Can't open more than one document with the --presentation switch") << '\n';
        return Error;
    }

    if (ShellUtils::showPrintDialog(serializedOptions) && paths.count() > 1) {
        QTextStream stream(stderr);
        stream << i18n("Error: Can't open more than one document with the --print switch") << '\n';
        return Error;
    }

    if (!ShellUtils::page(serializedOptions).isEmpty() && paths.count() > 1) {
        QTextStream stream(stderr);
        stream << i18n("Error: Can't open more than one document with the --page switch") << '\n';
        return Error;
    }

    if (!ShellUtils::find(serializedOptions).isEmpty() && paths.count() > 1) {
        QTextStream stream(stderr);
        stream << i18n("Error: Can't open more than one document with the --find switch") << '\n';
        return Error;
    }

    // try to attach to existing session, unique or not
    if (attachUniqueInstance(paths, serializedOptions) || attachExistingInstance(paths, serializedOptions)) {
        return AttachedOtherProcess;
    }

    Shell *shell = new Shell(serializedOptions);
    if (!shell->isValid()) {
        return Error;
    }

    shell->show();
    for (int i = 0; i < paths.count();) {
        // Page only makes sense if we are opening one file
        const QString page = ShellUtils::page(serializedOptions);
        const QUrl url = ShellUtils::urlFromArg(paths[i], ShellUtils::qfileExistFunc(), page);
        if (shell->openDocument(url, serializedOptions)) {
            ++i;
        } else {
            shell = new Shell(serializedOptions);
            if (!shell->isValid()) {
                return Error;
            }
            shell->show();
        }
    }

    return Success;
}

}

/* kate: replace-tabs on; indent-width 4; */
