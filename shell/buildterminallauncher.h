// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace Thoughts
{

class BuildTerminalLauncher
{
public:
    struct Options {
        std::function<bool(const QString &, const QStringList &)> startDetached;
        std::function<bool(const QString &)> focusTerminal;
        std::function<bool(const QString &)> processRunning;
        // Whether the stored source-publisher token may be sent to the server
        // at this address; if not, *issuedFor names the server it belongs to
        // (empty when no token is stored). Unset: the secret store and
        // Thoughts::ServerAddress decide.
        std::function<bool(const QString &serverUrl, QString *issuedFor)> tokenMayBeSentTo;
    };

    explicit BuildTerminalLauncher(Options options = {});

    bool open(const QString &runDirectory, const QString &watcherExecutable, const QString &serverUrl, QString *error = nullptr) const;
    bool openBuild(const QString &buildId, const QString &target, const QString &watcherExecutable, const QString &serverUrl, QString *error = nullptr) const;
    bool openLog(const QString &runDirectory, const QString &logPath, QString *error = nullptr) const;

    static QString terminalScript(const QString &runDirectory, const QString &watcherExecutable, const QString &serverUrl);
    static QString buildTerminalScript(const QString &runtimeDirectory, const QString &buildId, const QString &target, const QString &watcherExecutable, const QString &serverUrl);
    static QString logTerminalScript(const QString &runDirectory, const QString &logPath);
    // The PowerShell script buildTerminalScript() returns on Windows, built
    // on every system so its quoting can be tested anywhere.
    static QString windowsBuildTerminalScript(const QString &runtimeDirectory, const QString &buildId, const QString &target, const QString &watcherExecutable, const QString &serverUrl);

private:
    bool openRuntime(const QString &runtimeDirectory, const QString &script, const QString &watcherExecutable, const QString &serverUrl, QString *error) const;
    bool launchTerminal(const QString &scriptPath) const;
    bool focusTerminalTab(const QString &tty) const;
    bool isProcessRunning(const QString &pid) const;

    Options m_options;
};

}
