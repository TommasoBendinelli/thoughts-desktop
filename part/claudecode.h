// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// The person's own Claude Code on this computer, as Thoughts finds and sets it up: where claude
// is, the environment it runs with, and (where the installation brings them, i.e. the Windows
// installer) the Thoughts tools and the reply waker, with Settings › Agent prompts' "Set up
// Claude Code", which registers the tools for Claude Code. Used by the reader's Settings (part)
// and by the app (shell: the session starter, the waker's supervisor).
namespace Thoughts::ClaudeCode
{
// Where claude is looked for, in order: PATH, ~/.local/bin, /opt/homebrew/bin, /usr/local/bin,
// ~/.npm-global/bin, and on macOS the newest copy the Claude app keeps
// (~/Library/Application Support/Claude/claude-code/<version>/…/claude.app/Contents/MacOS/claude);
// on Windows %USERPROFILE%\.local\bin\claude.exe (the native installer's), PATH (as `where
// claude`) and npm's claude.cmd.
QStringList candidates();
// The first candidate that is an executable file; empty when there is none.
QString find();
// A Windows batch file (npm's claude.cmd, codex.cmd): cmd.exe parses its command line.
bool isBatchFile(const QString &program);
// Whether cmd.exe would read nothing of these arguments as its own syntax (& | < > ^ % ! " or a
// line break), so they may go to a batch file.
bool safeForBatchFile(const QStringList &arguments);
// The environment claude runs with: this app's, without what a Claude Code session that started
// the app would pass on (CLAUDECODE, CLAUDE_CODE_*, ANTHROPIC_*, …: the person's own sign-in
// counts), and with the executable's folder and the usual install folders on PATH (an app started
// from the Finder has a short PATH, and npm's claude needs node).
QProcessEnvironment environment(const QString &executable, QProcessEnvironment base = QProcessEnvironment::systemEnvironment());
// `claude auth status --json`: whether Claude Code is signed in; nothing when it did not say.
std::optional<bool> loggedIn(const QByteArray &authStatusJson);

// The Thoughts tools (tools/mcp) and the reply waker (tools/waker) with the Python that runs them,
// as an installation brings them: <installation>/tools/agent-tools.json names them, relative to
// the installation folder (the folder above the program's bin folder), e.g.
//   {"python": "python-1a2b3c4d5e6f/python.exe", "pythonw": "python-1a2b3c4d5e6f/pythonw.exe",
//    "mcp_server": "tools/mcp/server.py", "waker": "tools/waker/thoughts_reply_waker.py",
//    "python_version": "3.13.9"}
// The Windows installer has them; the macOS app does not (its people install the tools themselves).
struct BundledTools {
    QString root;  // the installation folder
    QString python;  // the console Python Claude Code starts the tools with
    QString pythonw;  // the windowless one the waker runs with (Windows; else the same as python)
    QString mcpServer;
    QString agentNotes;  // agent_notes.py beside the server: the waker's sign-in
    QString waker;
    QString thoughtsctl;  // bin/thoughtsctl(.exe), which the tools' proposal and card commands run
    QString pythonVersion;
    // The tools can be registered (python and server.py are there).
    bool hasTools() const;
    // The waker can run (a Python, the waker and agent_notes.py are there).
    bool hasWaker() const;
    // What a registration records, to notice that an update moved the tools: python and server.
    QString fingerprint() const;
};
// This installation's (programFolder: the folder of thoughts.exe; empty: the running program's, or
// the installation folder THOUGHTS_AGENT_TOOLS names, for a build run from its build folder).
BundledTools bundledTools(const QString &programFolder = {});

// The tools' name in Claude Code.
QString serverName();
// The variables the tools run with: THOUGHTSCTL, and THOUGHTS_HOME when this app reads another
// Library than the default one.
QList<QPair<QString, QString>> toolEnvironment(const BundledTools &tools, const QProcessEnvironment &app = QProcessEnvironment::systemEnvironment());
// claude mcp add --scope user thoughts -e KEY=VALUE … -- <python> -X utf8 -B <server.py>
// (UTF-8 mode: Windows would read and write the ANSI code page; -B: no bytecode beside the
// installed files). The name comes before -e, whose values would otherwise take it.
QStringList addArguments(const BundledTools &tools, const QList<QPair<QString, QString>> &variables);
// claude mcp remove --scope user thoughts: an earlier registration goes first.
QStringList removeArguments();

// What Set up Claude Code found and did.
enum class Outcome {
    Done,  // the tools are registered and Claude Code is signed in
    NotSignedIn,  // registered, but Claude Code is not signed in yet
    NotInstalled,  // no claude on this computer (or it did not run)
    NoTools,  // this installation brings no tools
    UnusualFolder,  // npm's claude.cmd, and a path cmd.exe would misread
    Failed,  // claude refused (detail: what it said)
};
struct Result {
    Outcome outcome = Outcome::Failed;
    QString claude;  // the claude it used
    QString detail;
};
// The result in plain words for Settings.
QString describe(const Result &result);
// Whether the tools need registering again (set up before, and the installation moved them since:
// an update with another Python).
bool needsRegisteringAgain(const QString &registered, const BundledTools &tools);

// Registers the tools: claude mcp remove, claude mcp add, then claude auth status. Asynchronous;
// finished() once.
class Setup : public QObject
{
    Q_OBJECT
public:
    struct Options {
        // Set (tests): this claude only, never a search; a missing file counts as not installed.
        QString claudeExecutable;
        // Set (tests): these tools instead of the installation's.
        std::optional<BundledTools> tools;
        int timeoutMs = 60000;
    };
    explicit Setup(QObject *parent = nullptr);
    Setup(Options options, QObject *parent);
    void start();

Q_SIGNALS:
    void finished(const Thoughts::ClaudeCode::Result &result);

private:
    struct Run {
        bool started = false;
        bool timedOut = false;
        int exitCode = -1;
        QByteArray output;
    };
    void run(const QStringList &arguments, std::function<void(const Run &)> done);
    void finish(Outcome outcome, const QString &detail = {});

    Options m_options;
    BundledTools m_tools;
    QString m_claude;
    bool m_finished = false;
};
}
