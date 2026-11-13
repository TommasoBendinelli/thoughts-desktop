// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildterminallauncher.h"
#include "powershellquote.h"
#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <utility>

namespace
{

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

using Thoughts::powerShellQuote;

// The Windows viewers are PowerShell scripts run in their own console. Like
// the zsh scripts, they record their process ID and window title (in place of
// the Terminal tty) so a second click focuses the open window, and remove
// both when the viewer ends. The token arrives in the environment.
QString powerShellViewer(const QString &runDirectory, const QString &title, const QString &body, const QString &finished)
{
    return QStringLiteral(R"($RunDirectory = %1
$Title = %2
$ActiveTitle = Join-Path $RunDirectory 'active.tty'
$ActivePid = Join-Path $RunDirectory 'active.pid'
$Launching = Join-Path $RunDirectory 'launching'
$Host.UI.RawUI.WindowTitle = $Title
[IO.File]::WriteAllText($ActiveTitle, $Title)
[IO.File]::WriteAllText($ActivePid, [string]$PID)
Remove-Item -LiteralPath $Launching -ErrorAction SilentlyContinue
$ExitCode = 0
try {
%3
} finally {
    Remove-Item -LiteralPath $ActiveTitle, $ActivePid, $Launching -ErrorAction SilentlyContinue
    Remove-Item Env:THOUGHTS_TOKEN -ErrorAction SilentlyContinue
}
Write-Host "%4 (exit $ExitCode)."
)")
        .arg(powerShellQuote(runDirectory), powerShellQuote(title), body, finished);
}

QString powerShellWatcher(const QString &watcherArguments)
{
    return QStringLiteral(R"(    if (-not $env:THOUGHTS_TOKEN) {
        Write-Host 'Could not read the source-publisher credential for the Thoughts server from Windows Credential Manager (thoughts-server/thoughts-source-sync).'
        $ExitCode = 1
    } else {
        & %1
        $ExitCode = $LASTEXITCODE
    })")
        .arg(watcherArguments);
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

QString focusTerminalScript()
{
    return QStringLiteral(R"(on run argv
set targetTTY to item 1 of argv
tell application "Terminal"
    repeat with terminalWindow in windows
        repeat with terminalTab in tabs of terminalWindow
            if tty of terminalTab is targetTTY then
                set selected tab of terminalWindow to terminalTab
                set index of terminalWindow to 1
                activate
                return "found"
            end if
        end repeat
    end repeat
end tell
error "Terminal tab not found" number 1
end run)");
}

}

namespace Thoughts
{

BuildTerminalLauncher::BuildTerminalLauncher(Options options)
    : m_options(std::move(options))
{
}

bool BuildTerminalLauncher::launchTerminal(const QString &scriptPath) const
{
#ifdef Q_OS_WIN
    const QStringList arguments {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-NoExit"), QStringLiteral("-File"), QDir::toNativeSeparators(scriptPath)};
    if (m_options.startDetached) {
        return m_options.startDetached(QStringLiteral("powershell.exe"), arguments);
    }
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The source-publisher token reaches only the watcher, never the script file.
    // openRuntime() already checked that this token belongs to the viewer's server.
    const QByteArray token = Thoughts::Platform::readSecret(Thoughts::ServerAddress::tokenService(), QStringLiteral("thoughts-source-sync"));
    if (!token.isEmpty()) {
        environment.insert(QStringLiteral("THOUGHTS_TOKEN"), QString::fromUtf8(token));
    }
    return Thoughts::Platform::startInNewConsole(QStringLiteral("powershell.exe"), arguments, environment);
#endif
    if (m_options.startDetached) {
        return m_options.startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), scriptPath});
    }
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), scriptPath});
}

bool BuildTerminalLauncher::focusTerminalTab(const QString &tty) const
{
    if (m_options.focusTerminal) {
        return m_options.focusTerminal(tty);
    }
#ifdef Q_OS_WIN
    // tty holds the viewer's window title.
    return Thoughts::Platform::focusWindowTitled(tty);
#endif
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), focusTerminalScript(), tty});
    return process.waitForStarted(1000) && process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool BuildTerminalLauncher::isProcessRunning(const QString &pid) const
{
    if (m_options.processRunning) {
        return m_options.processRunning(pid);
    }
#ifdef Q_OS_WIN
    return Thoughts::Platform::processIsRunning(pid);
#endif
    return !pid.isEmpty() && QProcess::execute(QStringLiteral("/bin/kill"), {QStringLiteral("-0"), pid}) == 0;
}

QString BuildTerminalLauncher::terminalScript(const QString &runDirectory, const QString &watcherExecutable, const QString &serverUrl)
{
#ifdef Q_OS_WIN
    return powerShellViewer(runDirectory,
                            QStringLiteral("Thoughts build output - %1").arg(QFileInfo(runDirectory).fileName()),
                            powerShellWatcher(QStringLiteral("%1 --server %2 --run-directory $RunDirectory").arg(powerShellQuote(watcherExecutable), powerShellQuote(serverUrl))),
                            QStringLiteral("Build output viewer finished"));
#endif
    return QStringLiteral(R"(#!/bin/zsh
setopt no_unset
RUN_DIRECTORY=%1
ACTIVE_TTY="${RUN_DIRECTORY}/active.tty"
ACTIVE_PID="${RUN_DIRECTORY}/active.pid"
LAUNCHING="${RUN_DIRECTORY}/launching"

cleanup() {
    /bin/rm -f "${ACTIVE_TTY}" "${ACTIVE_PID}" "${LAUNCHING}"
}
trap cleanup EXIT INT TERM HUP

TTY_VALUE=$(/usr/bin/tty)
print -r -- "${TTY_VALUE}" > "${ACTIVE_TTY}.tmp"
/bin/mv -f "${ACTIVE_TTY}.tmp" "${ACTIVE_TTY}"
print -r -- "$$" > "${ACTIVE_PID}.tmp"
/bin/mv -f "${ACTIVE_PID}.tmp" "${ACTIVE_PID}"
/bin/rm -f "${LAUNCHING}"

TOKEN=$(/usr/bin/security find-generic-password -s thoughts-server -a thoughts-source-sync -w)
KEYCHAIN_EXIT=$?
if [[ ${KEYCHAIN_EXIT} -ne 0 || -z "${TOKEN}" ]]; then
    print -r -- "Could not read the source-publisher credential for the Thoughts server from macOS Keychain."
    EXIT_CODE=1
else
    export THOUGHTS_TOKEN="${TOKEN}"
    unset TOKEN
    %2 --server %3 --run-directory "${RUN_DIRECTORY}"
    EXIT_CODE=$?
    unset THOUGHTS_TOKEN
fi

cleanup
trap - EXIT INT TERM HUP
print -r -- "Build output viewer finished (exit ${EXIT_CODE})."
exec "${SHELL:-/bin/zsh}" -l
)")
        .arg(shellQuote(runDirectory), shellQuote(watcherExecutable), shellQuote(serverUrl));
}

QString BuildTerminalLauncher::windowsBuildTerminalScript(const QString &runtimeDirectory,
                                                           const QString &buildId,
                                                           const QString &target,
                                                           const QString &watcherExecutable,
                                                           const QString &serverUrl)
{
    // The target is a PDF file name, which Thoughts does not choose: every
    // value reaches PowerShell as a quoted literal.
    return powerShellViewer(runtimeDirectory,
                            QStringLiteral("Thoughts build output - %1").arg(target),
                            powerShellWatcher(QStringLiteral("%1 --server %2 --build-id %3 --target %4")
                                                  .arg(powerShellQuote(watcherExecutable), powerShellQuote(serverUrl), powerShellQuote(buildId), powerShellQuote(target))),
                            QStringLiteral("Build output viewer finished"));
}

QString BuildTerminalLauncher::buildTerminalScript(const QString &runtimeDirectory,
                                                    const QString &buildId,
                                                    const QString &target,
                                                    const QString &watcherExecutable,
                                                    const QString &serverUrl)
{
#ifdef Q_OS_WIN
    return windowsBuildTerminalScript(runtimeDirectory, buildId, target, watcherExecutable, serverUrl);
#endif
    return QStringLiteral(R"(#!/bin/zsh
setopt no_unset
RUN_DIRECTORY=%1
ACTIVE_TTY="${RUN_DIRECTORY}/active.tty"
ACTIVE_PID="${RUN_DIRECTORY}/active.pid"
LAUNCHING="${RUN_DIRECTORY}/launching"

cleanup() {
    /bin/rm -f "${ACTIVE_TTY}" "${ACTIVE_PID}" "${LAUNCHING}"
}
trap cleanup EXIT INT TERM HUP

TTY_VALUE=$(/usr/bin/tty)
print -r -- "${TTY_VALUE}" > "${ACTIVE_TTY}.tmp"
/bin/mv -f "${ACTIVE_TTY}.tmp" "${ACTIVE_TTY}"
print -r -- "$$" > "${ACTIVE_PID}.tmp"
/bin/mv -f "${ACTIVE_PID}.tmp" "${ACTIVE_PID}"
/bin/rm -f "${LAUNCHING}"

TOKEN=$(/usr/bin/security find-generic-password -s thoughts-server -a thoughts-source-sync -w)
KEYCHAIN_EXIT=$?
if [[ ${KEYCHAIN_EXIT} -ne 0 || -z "${TOKEN}" ]]; then
    print -r -- "Could not read the source-publisher credential for the Thoughts server from macOS Keychain."
    EXIT_CODE=1
else
    export THOUGHTS_TOKEN="${TOKEN}"
    unset TOKEN
    %2 --server %3 --build-id %4 --target %5
    EXIT_CODE=$?
    unset THOUGHTS_TOKEN
fi

cleanup
trap - EXIT INT TERM HUP
print -r -- "Build output viewer finished (exit ${EXIT_CODE})."
exec "${SHELL:-/bin/zsh}" -l
)")
        .arg(shellQuote(runtimeDirectory), shellQuote(watcherExecutable), shellQuote(serverUrl), shellQuote(buildId), shellQuote(target));
}

QString BuildTerminalLauncher::logTerminalScript(const QString &runDirectory, const QString &logPath)
{
#ifdef Q_OS_WIN
    return powerShellViewer(runDirectory,
                            QStringLiteral("Thoughts Sync log - %1").arg(QFileInfo(runDirectory).fileName()),
                            QStringLiteral("    Get-Content -LiteralPath %1 -Tail 200 -Wait -Encoding UTF8").arg(powerShellQuote(logPath)),
                            QStringLiteral("Sync log viewer finished"));
#endif
    return QStringLiteral(R"(#!/bin/zsh
setopt no_unset
RUN_DIRECTORY=%1
LOG_PATH=%2
ACTIVE_TTY="${RUN_DIRECTORY}/active.tty"
ACTIVE_PID="${RUN_DIRECTORY}/active.pid"
LAUNCHING="${RUN_DIRECTORY}/launching"

cleanup() {
    /bin/rm -f "${ACTIVE_TTY}" "${ACTIVE_PID}" "${LAUNCHING}"
}
trap cleanup EXIT INT TERM HUP

TTY_VALUE=$(/usr/bin/tty)
print -r -- "${TTY_VALUE}" > "${ACTIVE_TTY}.tmp"
/bin/mv -f "${ACTIVE_TTY}.tmp" "${ACTIVE_TTY}"
print -r -- "$$" > "${ACTIVE_PID}.tmp"
/bin/mv -f "${ACTIVE_PID}.tmp" "${ACTIVE_PID}"
/bin/rm -f "${LAUNCHING}"

/usr/bin/tail -n 200 -f "${LOG_PATH}"
EXIT_CODE=$?

cleanup
trap - EXIT INT TERM HUP
print -r -- "Sync log viewer finished (exit ${EXIT_CODE})."
exec "${SHELL:-/bin/zsh}" -l
)")
        .arg(shellQuote(runDirectory), shellQuote(logPath));
}

bool BuildTerminalLauncher::openRuntime(const QString &runDirectory,
                                        const QString &script,
                                        const QString &watcherExecutable,
                                        const QString &serverUrl,
                                        QString *error) const
{
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    Q_UNUSED(runDirectory)
    Q_UNUSED(script)
    Q_UNUSED(watcherExecutable)
    Q_UNUSED(serverUrl)
    if (error) {
        *error = i18n("The server build output is available through Terminal.app on macOS only.");
    }
    return false;
#else
    const bool localLog = watcherExecutable.isEmpty() && serverUrl.isEmpty();
    if (runDirectory.isEmpty() || !QFileInfo(runDirectory).isDir()) {
        if (error) {
            *error = i18n("The local build-output directory is unavailable.");
        }
        return false;
    }
    if (!localLog && !QFileInfo(watcherExecutable).isExecutable()) {
        if (error) {
            *error = i18n("The build-output viewer installed with Thoughts is missing; reinstall Thoughts.");
        }
        return false;
    }
    if (!localLog && Thoughts::ServerAddress::normalized(serverUrl).isEmpty()) {
        if (error) {
            *error = i18n("The Thoughts server URL is invalid.");
        }
        return false;
    }
    if (!localLog) {
        // The viewer sends the stored source-publisher token to this server:
        // only to the one that issued it. With no token stored at all the
        // viewer says so itself.
        const QString account = QStringLiteral("thoughts-source-sync");
        QString issuedFor;
        const bool allowed = m_options.tokenMayBeSentTo ? m_options.tokenMayBeSentTo(serverUrl, &issuedFor)
                                                        : !Thoughts::ServerAddress::storedToken(account, serverUrl, &issuedFor).isEmpty();
        if (!allowed && !issuedFor.isEmpty()) {
            if (error) {
                *error = Thoughts::ServerAddress::tokenWithheldMessage(account, issuedFor, serverUrl);
            }
            return false;
        }
    }

    QLockFile lock(QDir(runDirectory).filePath(QStringLiteral("terminal-launcher.lock")));
    lock.setStaleLockTime(15000);
    if (!lock.tryLock(0)) {
        return true;
    }

    const QString activeTtyPath = QDir(runDirectory).filePath(QStringLiteral("active.tty"));
    const QString activePidPath = QDir(runDirectory).filePath(QStringLiteral("active.pid"));
    const QString pid = readTextFile(activePidPath);
    if (isProcessRunning(pid)) {
        const QString tty = readTextFile(activeTtyPath);
        if (tty.isEmpty() || focusTerminalTab(tty)) {
            return true;
        }
#ifdef Q_OS_WIN
        // Windows Terminal tabs have no window of their own to raise; the
        // viewer is open, so don't start another.
        return true;
#else
        if (error) {
            *error = i18n("The build output is open in Terminal, but its tab could not be focused. Allow Thoughts to control Terminal in System Settings, then try again.");
        }
        return false;
#endif
    }
    QFile::remove(activeTtyPath);
    QFile::remove(activePidPath);

    const QString launchingPath = QDir(runDirectory).filePath(QStringLiteral("launching"));
    const QFileInfo launchingInfo(launchingPath);
    if (launchingInfo.exists() && launchingInfo.lastModified().msecsTo(QDateTime::currentDateTime()) < 15000) {
        return true;
    }
    QFile::remove(launchingPath);

#ifdef Q_OS_WIN
    const QString scriptPath = QDir(runDirectory).filePath(QStringLiteral("build-output.ps1"));
#else
    const QString scriptPath = QDir(runDirectory).filePath(QStringLiteral("build-output.command"));
#endif
#ifdef Q_OS_WIN
    // Windows PowerShell reads a script without a byte-order mark as ANSI.
    const QByteArray scriptBytes = QByteArrayLiteral("\xEF\xBB\xBF") + script.toUtf8();
#else
    const QByteArray scriptBytes = script.toUtf8();
#endif
    if (!writeFile(scriptPath, scriptBytes)
        || !QFile::setPermissions(scriptPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) {
            *error = i18n("The Terminal launcher could not be written at %1.", QDir::toNativeSeparators(scriptPath));
        }
        return false;
    }
    if (!writeFile(launchingPath, QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + '\n')) {
        if (error) {
            *error = i18n("The Terminal launch state could not be recorded.");
        }
        return false;
    }
    if (!launchTerminal(scriptPath)) {
        QFile::remove(launchingPath);
        if (error) {
#ifdef Q_OS_WIN
            *error = i18n("PowerShell could not be opened for this server build.");
#else
            *error = i18n("Terminal.app could not be opened for this server build.");
#endif
        }
        return false;
    }
    return true;
#endif
}

bool BuildTerminalLauncher::open(const QString &runDirectory, const QString &watcherExecutable, const QString &serverUrl, QString *error) const
{
    if (!QFileInfo::exists(QDir(runDirectory).filePath(QStringLiteral("request.json")))) {
        if (error) {
            *error = i18n("The local build run is unavailable.");
        }
        return false;
    }
    return openRuntime(runDirectory, terminalScript(runDirectory, watcherExecutable, serverUrl), watcherExecutable, serverUrl, error);
}

bool BuildTerminalLauncher::openBuild(const QString &buildId,
                                      const QString &target,
                                      const QString &watcherExecutable,
                                      const QString &serverUrl,
                                      QString *error) const
{
    static const QRegularExpression safeId(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
    if (!safeId.match(buildId).hasMatch() || target.isEmpty() || target.size() > 255 || target.contains(QLatin1Char('/')) || target.contains(QLatin1Char('\\'))
        || !target.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        if (error) {
            *error = i18n("The server build identity is invalid.");
        }
        return false;
    }
    const QString runtimeDirectory = QDir(Thoughts::Platform::applicationDataRoot())
                                         .filePath(QStringLiteral("thoughts-server/build-output/%1").arg(buildId));
    if (!QDir().mkpath(runtimeDirectory)) {
        if (error) {
            *error = i18n("The local build-output directory could not be created.");
        }
        return false;
    }
    return openRuntime(runtimeDirectory,
                       buildTerminalScript(runtimeDirectory, buildId, target, watcherExecutable, serverUrl),
                       watcherExecutable,
                       serverUrl,
                       error);
}

bool BuildTerminalLauncher::openLog(const QString &runDirectory, const QString &logPath, QString *error) const
{
    const QFileInfo log(logPath);
    if (runDirectory.isEmpty() || !QFileInfo(runDirectory).isDir() || !log.isFile() || log.absolutePath() != QFileInfo(runDirectory).absoluteFilePath()) {
        if (error) {
            *error = i18n("The local Sync log is unavailable.");
        }
        return false;
    }
    return openRuntime(runDirectory, logTerminalScript(runDirectory, log.absoluteFilePath()), QString(), QString(), error);
}

}
