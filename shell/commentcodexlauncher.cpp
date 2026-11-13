/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "commentcodexlauncher.h"
#include "powershellquote.h"
#include "../part/thoughtsplatform.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace
{

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return false;
    }
    return file.commit();
}

QString canonicalOrAbsolutePath(const QString &path)
{
    const QFileInfo info(path);
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}

bool isPathInside(const QString &path, const QString &directory)
{
    const QString canonicalPath = canonicalOrAbsolutePath(path);
    QString canonicalDirectory = canonicalOrAbsolutePath(directory);
    if (!canonicalDirectory.endsWith(QLatin1Char('/'))) {
        canonicalDirectory.append(QLatin1Char('/'));
    }
    return canonicalPath.startsWith(canonicalDirectory);
}

using Thoughts::powerShellQuote;

bool processIsRunning(const QString &pid)
{
#ifdef Q_OS_WIN
    return Thoughts::Platform::processIsRunning(pid);
#else
    return !pid.isEmpty() && QProcess::execute(QStringLiteral("/bin/kill"), {QStringLiteral("-0"), pid}) == 0;
#endif
}

QString sessionIdFromRollout(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    for (int lineNumber = 0; lineNumber < 64 && !file.atEnd(); ++lineNumber) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readLine(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("type")).toString() != QLatin1String("session_meta")) {
            continue;
        }
        return object.value(QStringLiteral("payload")).toObject().value(QStringLiteral("id")).toString();
    }
    return {};
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

CommentCodexLauncher::CommentCodexLauncher(const QString &docsRoot, Options options, QObject *parent)
    : QObject(parent)
    , m_docsRoot(canonicalOrAbsolutePath(docsRoot))
    , m_options(std::move(options))
{
}

QString CommentCodexLauncher::associationKey(const QString &pdfPath, const QString &annotationId)
{
    const QFileInfo pdfInfo(pdfPath);
    const QString canonicalPdfPath = pdfInfo.canonicalFilePath().isEmpty() ? pdfInfo.absoluteFilePath() : pdfInfo.canonicalFilePath();
    QByteArray identity = canonicalPdfPath.toUtf8();
    identity.append('\0');
    identity.append(annotationId.toUtf8());
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString CommentCodexLauncher::runtimeDirectory(const QString &pdfPath, const QString &annotationId) const
{
    return QDir(m_docsRoot).filePath(QStringLiteral(".runtime/target/okular-codex-comments/%1").arg(associationKey(pdfPath, annotationId)));
}

QString CommentCodexLauncher::resolveCodexHome() const
{
    if (!m_options.codexHome.isEmpty()) {
        return canonicalOrAbsolutePath(m_options.codexHome);
    }
    const QByteArray configuredHome = qgetenv("CODEX_HOME");
    if (!configuredHome.isEmpty()) {
        return canonicalOrAbsolutePath(QString::fromLocal8Bit(configuredHome));
    }
    return QDir::home().filePath(QStringLiteral(".codex"));
}

QString CommentCodexLauncher::resolveCodexExecutable() const
{
    QStringList candidates;
    if (!m_options.codexExecutable.isEmpty()) {
        candidates.append(m_options.codexExecutable);
    }
#ifdef Q_OS_WIN
    // npm installs a codex.cmd shim; the standalone release is codex.exe.
    candidates.append({
        QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("npm/codex.cmd")),
        QDir::home().filePath(QStringLiteral(".local/bin/codex.exe")),
    });
#endif
    candidates.append({
        QDir::home().filePath(QStringLiteral(".npm-global/bin/codex")),
        QDir::home().filePath(QStringLiteral(".local/bin/codex")),
        QStringLiteral("/opt/homebrew/bin/codex"),
        QStringLiteral("/usr/local/bin/codex"),
    });
    const QString inherited = QStandardPaths::findExecutable(QStringLiteral("codex"));
    if (!inherited.isEmpty()) {
        candidates.append(inherited);
    }
    for (const QString &candidate : std::as_const(candidates)) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

CommentCodexLauncher::SessionAssociation CommentCodexLauncher::consumeCapturedSession(const QString &directory, const QString &pdfPath, const QString &annotationId) const
{
    const QString capturedPath = readTextFile(QDir(directory).filePath(QStringLiteral("session.path")));
    if (capturedPath.isEmpty()) {
        return {};
    }
    SessionAssociation association;
    association.path = QFileInfo(capturedPath).absoluteFilePath();
    association.id = sessionIdFromRollout(association.path);
    if (association.id.isEmpty()) {
        return {};
    }
    saveAssociation(directory, pdfPath, annotationId, association);
    return association;
}

CommentCodexLauncher::SessionAssociation CommentCodexLauncher::loadAssociation(const QString &directory) const
{
    QFile file(QDir(directory).filePath(QStringLiteral("association.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != 1) {
        return {};
    }
    SessionAssociation association;
    association.id = object.value(QStringLiteral("session_id")).toString();
    association.path = object.value(QStringLiteral("session_path")).toString();
    return association;
}

CommentCodexLauncher::SessionAssociation CommentCodexLauncher::locateAssociation(SessionAssociation association) const
{
    if (association.id.isEmpty()) {
        return {};
    }
    if (!association.path.isEmpty() && QFileInfo::exists(association.path) && sessionIdFromRollout(association.path) == association.id) {
        association.archived = isPathInside(association.path, QDir(resolveCodexHome()).filePath(QStringLiteral("archived_sessions")));
        return association;
    }

    const QDir codexHome(resolveCodexHome());
    const QString fileNamePattern = QStringLiteral("*%1*.jsonl").arg(association.id);
    const QString archivedRoot = codexHome.filePath(QStringLiteral("archived_sessions"));
    QDirIterator archived(archivedRoot, {fileNamePattern}, QDir::Files, QDirIterator::Subdirectories);
    if (archived.hasNext()) {
        association.path = archived.next();
        association.archived = true;
        return association;
    }
    const QString sessionsRoot = codexHome.filePath(QStringLiteral("sessions"));
    QDirIterator active(sessionsRoot, {fileNamePattern}, QDir::Files, QDirIterator::Subdirectories);
    if (active.hasNext()) {
        association.path = active.next();
        association.archived = false;
        return association;
    }
    return {};
}

bool CommentCodexLauncher::saveAssociation(const QString &directory, const QString &pdfPath, const QString &annotationId, const SessionAssociation &association) const
{
    const QJsonObject object {
        {QStringLiteral("version"), 1},
        {QStringLiteral("pdf_path"), QFileInfo(pdfPath).absoluteFilePath()},
        {QStringLiteral("annotation_id"), annotationId},
        {QStringLiteral("session_id"), association.id},
        {QStringLiteral("session_path"), association.path},
    };
    return writeFile(QDir(directory).filePath(QStringLiteral("association.json")), QJsonDocument(object).toJson(QJsonDocument::Indented));
}

bool CommentCodexLauncher::launchTerminal(const QString &scriptPath) const
{
#ifdef Q_OS_WIN
    const QStringList arguments {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-NoExit"), QStringLiteral("-File"), QDir::toNativeSeparators(scriptPath)};
    if (m_options.startDetached) {
        return m_options.startDetached(QStringLiteral("powershell.exe"), arguments);
    }
    return Thoughts::Platform::startInNewConsole(QStringLiteral("powershell.exe"), arguments, QProcessEnvironment::systemEnvironment());
#endif
    if (m_options.startDetached) {
        return m_options.startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), scriptPath});
    }
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), scriptPath});
}

bool CommentCodexLauncher::focusTerminalTab(const QString &tty) const
{
    if (m_options.focusTerminal) {
        return m_options.focusTerminal(tty);
    }
#ifdef Q_OS_WIN
    // tty holds the conversation window's title.
    return Thoughts::Platform::focusWindowTitled(tty);
#endif
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), focusTerminalScript(), tty});
    return process.waitForStarted(1000) && process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool CommentCodexLauncher::codexBypassesApprovalsAndSandbox()
{
    if (qEnvironmentVariableIsSet("THOUGHTS_CODEX_YOLO")) {
        const QByteArray value = qgetenv("THOUGHTS_CODEX_YOLO").trimmed().toLower();
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }
    return KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("Codex")).readEntry("BypassApprovalsAndSandbox", false);
}

QString CommentCodexLauncher::terminalScript(const QString &runtimeDirectory,
                                             const QString &docsRoot,
                                             const QString &codexHome,
                                             const QString &codexExecutable,
                                             const QString &sessionId,
                                             bool unarchiveSession,
                                             bool bypassApprovalsAndSandbox)
{
    // Without --yolo Codex asks before it runs commands and keeps its sandbox.
    const QString yolo = bypassApprovalsAndSandbox ? QStringLiteral(" --yolo") : QString();
#ifdef Q_OS_WIN
    // PowerShell has no lsof to see which rollout Codex opens. Once Codex
    // exits, a new conversation's rollout is the first one created since the
    // launch whose session_meta names the docs folder; resumed conversations
    // already have theirs.
    const QString run = sessionId.isEmpty()
        ? QStringLiteral("    & %1%2 -C $DocsRoot").arg(powerShellQuote(codexExecutable), yolo)
        : QStringLiteral("    & %1%2 resume -C $DocsRoot %3").arg(powerShellQuote(codexExecutable), yolo, powerShellQuote(sessionId));
    const QString unarchiveCommand = unarchiveSession && !sessionId.isEmpty()
        ? QStringLiteral("    & %1 unarchive %2\n    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n").arg(powerShellQuote(codexExecutable), powerShellQuote(sessionId))
        : QString();
    return QStringLiteral(R"($RuntimeDirectory = %1
$CodexHome = %2
$DocsRoot = %3
$Title = %4
$Capture = %5
$ActiveTitle = Join-Path $RuntimeDirectory 'active.tty'
$ActivePid = Join-Path $RuntimeDirectory 'active.pid'
$CapturedSession = Join-Path $RuntimeDirectory 'session.path'
$Launching = Join-Path $RuntimeDirectory 'launching'
$Host.UI.RawUI.WindowTitle = $Title
[IO.File]::WriteAllText($ActiveTitle, $Title)
[IO.File]::WriteAllText($ActivePid, [string]$PID)
Remove-Item -LiteralPath $Launching -ErrorAction SilentlyContinue
$Started = (Get-Date).AddSeconds(-2)
try {
%6%7
} finally {
    if ($Capture) {
        $Folder = ($DocsRoot -replace '/', '').TrimEnd('')
        $Candidates = @(Get-ChildItem -LiteralPath (Join-Path $CodexHome 'sessions') -Recurse -Filter 'rollout-*.jsonl' -ErrorAction SilentlyContinue |
            Where-Object { $_.CreationTime -ge $Started } | Sort-Object CreationTime)
        $Chosen = $null
        foreach ($Candidate in $Candidates) {
            foreach ($Line in (Get-Content -LiteralPath $Candidate.FullName -TotalCount 64 -Encoding UTF8 -ErrorAction SilentlyContinue)) {
                try { $Record = $Line | ConvertFrom-Json } catch { continue }
                if ($Record.type -eq 'session_meta') {
                    if ($Record.payload.cwd -and (($Record.payload.cwd -replace '/', '').TrimEnd('') -ieq $Folder)) { $Chosen = $Candidate.FullName }
                    break
                }
            }
            if ($Chosen) { break }
        }
        if (-not $Chosen -and $Candidates.Count -eq 1) { $Chosen = $Candidates[0].FullName }
        if ($Chosen) { [IO.File]::WriteAllText($CapturedSession, $Chosen) }
    }
    Remove-Item -LiteralPath $ActiveTitle, $ActivePid, $Launching -ErrorAction SilentlyContinue
}
)")
        .arg(powerShellQuote(runtimeDirectory),
             powerShellQuote(codexHome),
             powerShellQuote(docsRoot),
             powerShellQuote(QStringLiteral("Thoughts comment - %1").arg(QFileInfo(runtimeDirectory).fileName().left(12))),
             sessionId.isEmpty() ? QStringLiteral("$true") : QStringLiteral("$false"),
             unarchiveCommand,
             run);
#endif
    const QString command = sessionId.isEmpty()
        ? QStringLiteral("exec %1%2 -C %3").arg(shellQuote(codexExecutable), yolo, shellQuote(docsRoot))
        : QStringLiteral("exec %1%2 resume -C %3 %4").arg(shellQuote(codexExecutable), yolo, shellQuote(docsRoot), shellQuote(sessionId));
    const QString unarchive = unarchiveSession && !sessionId.isEmpty()
        ? QStringLiteral("%1 unarchive %2 || exit $?\n").arg(shellQuote(codexExecutable), shellQuote(sessionId))
        : QString();

    return QStringLiteral(R"(#!/bin/zsh
setopt no_unset
RUNTIME_DIRECTORY=%1
CODEX_HOME_DIRECTORY=%2
ACTIVE_TTY="${RUNTIME_DIRECTORY}/active.tty"
ACTIVE_PID="${RUNTIME_DIRECTORY}/active.pid"
CAPTURED_SESSION="${RUNTIME_DIRECTORY}/session.path"
LAUNCHING="${RUNTIME_DIRECTORY}/launching"
TARGET_PID=$$
TARGET_PGID=$(/bin/ps -o pgid= -p "${TARGET_PID}" | /usr/bin/tr -d ' ')
if [[ -z "${TARGET_PGID}" ]]; then
    TARGET_PGID="${TARGET_PID}"
fi

TTY_VALUE=$(/usr/bin/tty)
print -r -- "${TTY_VALUE}" > "${ACTIVE_TTY}.tmp"
/bin/mv -f "${ACTIVE_TTY}.tmp" "${ACTIVE_TTY}"
print -r -- "${TARGET_PID}" > "${ACTIVE_PID}.tmp"
/bin/mv -f "${ACTIVE_PID}.tmp" "${ACTIVE_PID}"
/bin/rm -f "${LAUNCHING}"

(
    CAPTURED=false
    while /bin/kill -0 "${TARGET_PID}" 2>/dev/null; do
        if [[ "${CAPTURED}" == false ]]; then
            while IFS= read -r ENTRY; do
                CANDIDATE="${ENTRY#n}"
                if [[ "${CANDIDATE}" == "${CODEX_HOME_DIRECTORY}"/sessions/*/rollout-*.jsonl ]]; then
                    print -r -- "${CANDIDATE}" > "${CAPTURED_SESSION}.tmp"
                    /bin/mv -f "${CAPTURED_SESSION}.tmp" "${CAPTURED_SESSION}"
                    CAPTURED=true
                    break
                fi
            done < <(/usr/sbin/lsof -a -g "${TARGET_PGID}" -Fn 2>/dev/null)
        fi
        if [[ "${CAPTURED}" == true ]]; then
            /bin/sleep 1
        else
            /bin/sleep 0.1
        fi
    done
    /bin/rm -f "${ACTIVE_TTY}" "${ACTIVE_PID}" "${LAUNCHING}"
) >/dev/null 2>&1 &

%3%4
)")
        .arg(shellQuote(runtimeDirectory), shellQuote(codexHome), unarchive, command);
}

bool CommentCodexLauncher::openComment(const QString &pdfPath, const QString &annotationId, QString *error)
{
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    Q_UNUSED(pdfPath)
    Q_UNUSED(annotationId)
    if (error) {
        *error = i18n("Opening comment conversations is available through Terminal.app on macOS only.");
    }
    return false;
#else
    if (pdfPath.isEmpty() || annotationId.isEmpty()) {
        if (error) {
            *error = i18n("Select one valid comment before opening Codex.");
        }
        return false;
    }
    const QString codexExecutable = resolveCodexExecutable();
    if (codexExecutable.isEmpty()) {
        if (error) {
#ifdef Q_OS_WIN
            *error = i18n("The Codex executable could not be found. Install it with npm (codex.cmd in %APPDATA%\\npm) or put codex on PATH.");
#else
            *error = i18n("The Codex executable could not be found. Expected it under ~/.npm-global/bin, ~/.local/bin, /opt/homebrew/bin, or /usr/local/bin.");
#endif
        }
        return false;
    }

    const QString directory = runtimeDirectory(pdfPath, annotationId);
    if (!QDir().mkpath(directory)) {
        if (error) {
            *error = i18n("The local comment-conversation directory could not be created at %1.", QDir::toNativeSeparators(directory));
        }
        return false;
    }
    QLockFile lock(QDir(directory).filePath(QStringLiteral("launcher.lock")));
    lock.setStaleLockTime(15000);
    if (!lock.tryLock(0)) {
        return true;
    }

    SessionAssociation association = consumeCapturedSession(directory, pdfPath, annotationId);
    if (association.id.isEmpty()) {
        association = loadAssociation(directory);
    }

    const QString activeTtyPath = QDir(directory).filePath(QStringLiteral("active.tty"));
    const QString activePidPath = QDir(directory).filePath(QStringLiteral("active.pid"));
    const QString tty = readTextFile(activeTtyPath);
    const bool activeProcessRunning = processIsRunning(readTextFile(activePidPath));
    if (!activeProcessRunning) {
        QFile::remove(activeTtyPath);
        QFile::remove(activePidPath);
    } else if (!tty.isEmpty()) {
        if (focusTerminalTab(tty)) {
            return true;
        }
#ifdef Q_OS_WIN
        // A Windows Terminal tab has no window of its own to raise.
        return true;
#endif
        if (error) {
            *error = i18n("Codex is still running in Terminal, but its tab could not be focused. Allow Thoughts to control Terminal in System Settings, then try again.");
        }
        return false;
    }
    if (activeProcessRunning) {
        return true;
    }

    const QString launchingPath = QDir(directory).filePath(QStringLiteral("launching"));
    const QFileInfo launchingInfo(launchingPath);
    if (launchingInfo.exists() && launchingInfo.lastModified().msecsTo(QDateTime::currentDateTime()) < 15000) {
        return true;
    }
    QFile::remove(launchingPath);

    association = locateAssociation(association);
    if (!association.id.isEmpty()) {
        saveAssociation(directory, pdfPath, annotationId, association);
    } else {
        QFile::remove(QDir(directory).filePath(QStringLiteral("association.json")));
        QFile::remove(QDir(directory).filePath(QStringLiteral("session.path")));
    }

#ifdef Q_OS_WIN
    const QString scriptPath = QDir(directory).filePath(QStringLiteral("launch.ps1"));
    // Windows PowerShell reads a script without a byte-order mark as ANSI.
    const QByteArray scriptPrefix = QByteArrayLiteral("\xEF\xBB\xBF");
#else
    const QString scriptPath = QDir(directory).filePath(QStringLiteral("launch.command"));
    const QByteArray scriptPrefix;
#endif
    const bool bypass = m_options.bypassApprovalsAndSandbox ? *m_options.bypassApprovalsAndSandbox : codexBypassesApprovalsAndSandbox();
    const QString script = terminalScript(directory, m_docsRoot, resolveCodexHome(), codexExecutable, association.id, association.archived, bypass);
    if (!writeFile(scriptPath, scriptPrefix + script.toUtf8()) || !QFile::setPermissions(scriptPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) {
            *error = i18n("The Terminal launcher could not be written at %1.", QDir::toNativeSeparators(scriptPath));
        }
        return false;
    }
    if (!writeFile(launchingPath, QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + '\n')) {
        if (error) {
            *error = i18n("The Codex launch state could not be recorded.");
        }
        return false;
    }
    if (!launchTerminal(scriptPath)) {
        QFile::remove(launchingPath);
        if (error) {
#ifdef Q_OS_WIN
            *error = i18n("PowerShell could not be opened for this comment.");
#else
            *error = i18n("Terminal.app could not be opened for this comment.");
#endif
        }
        return false;
    }
    return true;
#endif
}

}
