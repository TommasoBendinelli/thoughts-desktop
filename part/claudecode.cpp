// SPDX-License-Identifier: GPL-2.0-or-later
#include "claudecode.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace Thoughts::ClaudeCode
{

namespace
{
QList<int> versionKey(const QString &text)
{
    QList<int> key;
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[.-]")))) {
        bool ok = false;
        const int number = part.toInt(&ok);
        key.append(ok ? number : 0);
    }
    return key;
}

#ifdef Q_OS_MACOS
// The newest Claude Code the Claude app keeps: claude-code/<version>/claude.app or
// claude-code/<version>/<build>/claude.app.
QString bundledClaude()
{
    const QDir root(QDir::home().filePath(QStringLiteral("Library/Application Support/Claude/claude-code")));
    QStringList versions = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) { return versionKey(a) > versionKey(b); });
    const QString inside = QStringLiteral("claude.app/Contents/MacOS/claude");
    for (const QString &version : std::as_const(versions)) {
        const QDir versionDir(root.filePath(version));
        if (QFileInfo(versionDir.filePath(inside)).isExecutable()) return versionDir.filePath(inside);
        for (const QString &build : versionDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString candidate = QDir(versionDir.filePath(build)).filePath(inside);
            if (QFileInfo(candidate).isExecutable()) return candidate;
        }
    }
    return {};
}
#endif

bool isExecutableFile(const QString &path)
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && info.isExecutable();
}

QString tail(const QByteArray &bytes, int characters = 400)
{
    return QString::fromUtf8(bytes).trimmed().right(characters);
}
}

QStringList candidates()
{
    QStringList found;
    const QDir home = QDir::home();
#ifdef Q_OS_WIN
    found.append(home.filePath(QStringLiteral(".local/bin/claude.exe")));
    // QStandardPaths tries PATHEXT's suffixes, as `where claude` does.
    found.append(QStandardPaths::findExecutable(QStringLiteral("claude")));
    found.append(QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("npm/claude.cmd")));
#else
    found.append(QStandardPaths::findExecutable(QStringLiteral("claude")));
    found.append({home.filePath(QStringLiteral(".local/bin/claude")),
                  QStringLiteral("/opt/homebrew/bin/claude"),
                  QStringLiteral("/usr/local/bin/claude"),
                  home.filePath(QStringLiteral(".npm-global/bin/claude"))});
#ifdef Q_OS_MACOS
    found.append(bundledClaude());
#endif
#endif
    found.removeAll(QString());
    found.removeDuplicates();
    return found;
}

QString find()
{
    for (const QString &candidate : candidates()) {
        if (isExecutableFile(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

bool isBatchFile(const QString &program)
{
    return program.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive) || program.endsWith(QLatin1String(".bat"), Qt::CaseInsensitive);
}

bool safeForBatchFile(const QStringList &arguments)
{
    static const QRegularExpression special(QStringLiteral("[&|<>^%!\"\\r\\n]"));
    return std::none_of(arguments.cbegin(), arguments.cend(), [](const QString &argument) { return special.match(argument).hasMatch(); });
}

QProcessEnvironment environment(const QString &executable, QProcessEnvironment base)
{
    static const QStringList dropped {QStringLiteral("CLAUDE_CODE_"), QStringLiteral("CLAUDECODE"), QStringLiteral("CLAUDE_AGENT_SDK"),
                                      QStringLiteral("CLAUDE_PID"), QStringLiteral("CLAUDE_EFFORT"), QStringLiteral("ANTHROPIC_")};
    const QStringList keys = base.keys();
    for (const QString &key : keys) {
        if (std::any_of(dropped.cbegin(), dropped.cend(), [&key](const QString &prefix) { return key.startsWith(prefix, Qt::CaseInsensitive); }))
            base.remove(key);
    }
#ifdef Q_OS_WIN
    const QChar separator = QLatin1Char(';');
    const QStringList extra {executable.isEmpty() ? QString() : QDir::toNativeSeparators(QFileInfo(executable).absolutePath())};
#else
    const QChar separator = QLatin1Char(':');
    const QStringList extra {executable.isEmpty() ? QString() : QFileInfo(executable).absolutePath(),
                             QDir::home().filePath(QStringLiteral(".local/bin")),
                             QStringLiteral("/opt/homebrew/bin"),
                             QStringLiteral("/usr/local/bin")};
    if (!base.contains(QStringLiteral("HOME"))) base.insert(QStringLiteral("HOME"), QDir::homePath());
#endif
    QStringList path = base.value(QStringLiteral("PATH")).split(separator, Qt::SkipEmptyParts);
    for (auto it = extra.crbegin(); it != extra.crend(); ++it) {
        if (!it->isEmpty() && !path.contains(*it)) path.prepend(*it);
    }
    base.insert(QStringLiteral("PATH"), path.join(separator));
    return base;
}

std::optional<bool> loggedIn(const QByteArray &authStatusJson)
{
    const QJsonValue value = QJsonDocument::fromJson(authStatusJson.trimmed()).object().value(QLatin1String("loggedIn"));
    if (!value.isBool()) return std::nullopt;
    return value.toBool();
}

// ---------------------------------------------------------------- the installation's tools

bool BundledTools::hasTools() const
{
    return QFileInfo(python).isFile() && QFileInfo(mcpServer).isFile();
}

bool BundledTools::hasWaker() const
{
    return (QFileInfo(pythonw).isFile() || QFileInfo(python).isFile()) && QFileInfo(waker).isFile() && QFileInfo(agentNotes).isFile();
}

QString BundledTools::fingerprint() const
{
    return QDir::cleanPath(python) + QLatin1Char('\n') + QDir::cleanPath(mcpServer);
}

BundledTools bundledTools(const QString &programFolder)
{
    BundledTools tools;
    // THOUGHTS_AGENT_TOOLS names an installation folder instead (a build run from its build folder, tests).
    const QString configured = programFolder.isEmpty() ? qEnvironmentVariable("THOUGHTS_AGENT_TOOLS") : QString();
    const QString folder = !configured.isEmpty() ? QDir(configured).filePath(QStringLiteral("bin"))
        : programFolder.isEmpty()               ? QCoreApplication::applicationDirPath()
                                                : programFolder;
    // <installation>/bin/thoughts.exe: the installation is the folder above bin.
    QDir root(folder);
    if (!root.cdUp()) return tools;
    QFile file(root.filePath(QStringLiteral("tools/agent-tools.json")));
    if (!file.open(QIODevice::ReadOnly)) return tools;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    const auto inside = [&root](const QJsonValue &value) {
        const QString relative = value.toString();
        // Only files of the installation: never an absolute path or one that leaves it.
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || QDir::cleanPath(relative).startsWith(QLatin1String(".."))) return QString();
        return QDir::cleanPath(root.filePath(relative));
    };
    tools.root = root.absolutePath();
    tools.python = inside(json.value(QLatin1String("python")));
    tools.pythonw = inside(json.value(QLatin1String("pythonw")));
    if (tools.pythonw.isEmpty()) tools.pythonw = tools.python;
    tools.mcpServer = inside(json.value(QLatin1String("mcp_server")));
    if (!tools.mcpServer.isEmpty()) tools.agentNotes = QDir(QFileInfo(tools.mcpServer).absolutePath()).filePath(QStringLiteral("agent_notes.py"));
    tools.waker = inside(json.value(QLatin1String("waker")));
    tools.pythonVersion = json.value(QLatin1String("python_version")).toString();
#ifdef Q_OS_WIN
    tools.thoughtsctl = QDir(folder).filePath(QStringLiteral("thoughtsctl.exe"));
#else
    tools.thoughtsctl = QDir(folder).filePath(QStringLiteral("thoughtsctl"));
#endif
    return tools;
}

QString serverName()
{
    return QStringLiteral("thoughts");
}

QList<QPair<QString, QString>> toolEnvironment(const BundledTools &tools, const QProcessEnvironment &app)
{
    QList<QPair<QString, QString>> variables;
    if (!tools.thoughtsctl.isEmpty()) variables.append({QStringLiteral("THOUGHTSCTL"), QDir::toNativeSeparators(tools.thoughtsctl)});
    const QString library = app.value(QStringLiteral("THOUGHTS_HOME"));
    if (!library.isEmpty()) {
        // thoughtsctl reads THOUGHTS_HOME, the agent-note tools THOUGHTS_DOCS: both name the Library.
        variables.append({QStringLiteral("THOUGHTS_HOME"), QDir::toNativeSeparators(library)});
        variables.append({QStringLiteral("THOUGHTS_DOCS"), QDir::toNativeSeparators(library)});
    }
    return variables;
}

QStringList addArguments(const BundledTools &tools, const QList<QPair<QString, QString>> &variables)
{
    QStringList arguments {QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--scope"), QStringLiteral("user"), serverName()};
    for (const auto &[name, value] : variables) arguments << QStringLiteral("-e") << name + QLatin1Char('=') + value;
    arguments << QStringLiteral("--") << QDir::toNativeSeparators(tools.python) << QStringLiteral("-X") << QStringLiteral("utf8") << QStringLiteral("-B")
              << QDir::toNativeSeparators(tools.mcpServer);
    return arguments;
}

QStringList removeArguments()
{
    return {QStringLiteral("mcp"), QStringLiteral("remove"), QStringLiteral("--scope"), QStringLiteral("user"), serverName()};
}

QString describe(const Result &result)
{
    switch (result.outcome) {
    case Outcome::Done:
        return i18n("Claude Code has the Thoughts tools now. Sessions you start from now on can use them; start a running one again to give it them.");
    case Outcome::NotSignedIn:
        return i18n("Claude Code has the Thoughts tools, but it is not signed in yet. Open a terminal, run claude and sign in with your Claude account; "
                    "then your sessions can use them.");
    case Outcome::NotInstalled:
        return i18n("Claude Code is not on this computer. Install it (claude.com/claude-code), run claude once in a terminal to sign in, "
                    "then choose Set up Claude Code again.");
    case Outcome::NoTools:
        return i18n("This installation of Thoughts does not include the Thoughts tools for Claude Code.");
    case Outcome::UnusualFolder:
        return i18n("Thoughts could not set up Claude Code: it was installed with npm, and a folder name has characters that Windows' command line "
                    "would change (%1). Install Claude Code with its own installer (claude.com/claude-code), then try again.",
                    result.detail);
    case Outcome::Failed:
        break;
    }
    return result.detail.isEmpty() ? i18n("Claude Code did not take the Thoughts tools.") : i18n("Claude Code did not take the Thoughts tools. It said: %1", result.detail);
}

bool needsRegisteringAgain(const QString &registered, const BundledTools &tools)
{
    return !registered.isEmpty() && tools.hasTools() && registered != tools.fingerprint();
}

// ---------------------------------------------------------------- Set up Claude Code

Setup::Setup(QObject *parent)
    : Setup(Options(), parent)
{
}

Setup::Setup(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
}

void Setup::start()
{
    m_tools = m_options.tools ? *m_options.tools : bundledTools();
    if (!m_tools.hasTools()) {
        finish(Outcome::NoTools);
        return;
    }
    if (!m_options.claudeExecutable.isEmpty()) {
        m_claude = isExecutableFile(m_options.claudeExecutable) ? m_options.claudeExecutable : QString();
    } else {
        m_claude = find();
    }
    if (m_claude.isEmpty()) {
        finish(Outcome::NotInstalled);
        return;
    }
    const QStringList add = addArguments(m_tools, toolEnvironment(m_tools));
    if (isBatchFile(m_claude) && !safeForBatchFile(add)) {
        finish(Outcome::UnusualFolder, QDir::toNativeSeparators(m_tools.root));
        return;
    }
    // An earlier registration (another installation's, or this one's before an update) goes first;
    // there may be none, so how this ends does not matter.
    run(removeArguments(), [this, add](const Run &removed) {
        if (!removed.started) {
            finish(Outcome::NotInstalled);
            return;
        }
        run(add, [this](const Run &added) {
            if (!added.started) {
                finish(Outcome::NotInstalled);
                return;
            }
            if (added.timedOut || added.exitCode != 0) {
                finish(Outcome::Failed, added.timedOut ? i18n("nothing within %1 seconds", m_options.timeoutMs / 1000) : tail(added.output));
                return;
            }
            run({QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--json")}, [this](const Run &status) {
                // A Claude Code without `auth status` says nothing: the tools are set up anyway.
                finish(loggedIn(status.output) == false ? Outcome::NotSignedIn : Outcome::Done);
            });
        });
    });
}

void Setup::finish(Outcome outcome, const QString &detail)
{
    if (std::exchange(m_finished, true)) return;
    Q_EMIT finished(Result {outcome, m_claude, detail});
}

void Setup::run(const QStringList &arguments, std::function<void(const Run &)> done)
{
    struct State {
        Run result;
        std::function<void(const Run &)> done;
        bool fired = false;
    };
    auto *process = new QProcess(this);
    process->setProgram(m_claude);
    process->setArguments(arguments);
    process->setWorkingDirectory(QDir::homePath());
    process->setProcessEnvironment(environment(m_claude));
    process->setStandardInputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    const auto complete = [process, state] {
        if (std::exchange(state->fired, true)) return;
        state->result.output += process->readAll();
        process->deleteLater();
        const auto callback = std::move(state->done);
        callback(state->result);
    };
    connect(process, &QProcess::finished, this, [state, complete](int exitCode, QProcess::ExitStatus status) {
        state->result.started = true;
        state->result.exitCode = status == QProcess::NormalExit ? exitCode : -1;
        complete();
    });
    connect(process, &QProcess::errorOccurred, this, [state, complete](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        complete();
    });
    connect(timer, &QTimer::timeout, this, [process, state, complete] {
        state->result.started = true;
        state->result.timedOut = true;
        process->kill();
        complete();
    });
    timer->start(m_options.timeoutMs);
    process->start();
}

}
