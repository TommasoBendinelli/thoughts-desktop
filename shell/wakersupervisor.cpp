// SPDX-License-Identifier: GPL-2.0-or-later
#include "wakersupervisor.h"

#include "../part/agentsessionsettings.h"
#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"
#include "developerlog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

namespace Thoughts::Wakers
{

QJsonObject configFor(const ClaudeCode::BundledTools &tools, const QString &claude, const QString &machine)
{
    return {{QStringLiteral("machine"), machine},
            {QStringLiteral("agent_notes"), QDir::toNativeSeparators(tools.agentNotes)},
            {QStringLiteral("claude"), claude.isEmpty() ? QStringLiteral("auto") : QDir::toNativeSeparators(claude)},
            // The app's SessionStarter starts the sessions asked for; the waker only wakes them.
            {QStringLiteral("start_sessions"), false},
            {QStringLiteral("log_file"), QStringLiteral("logs/waker.log")},
            {QStringLiteral("stop_on_stdin_eof"), true}};
}

QString machineName(const QString &computerName)
{
    QString name = computerName.section(QLatin1Char('.'), 0, 0);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("-"));
    name = name.left(64);
    return name.isEmpty() ? QStringLiteral("machine") : name;
}

QStringList arguments(const ClaudeCode::BundledTools &tools)
{
    // UTF-8 mode (Windows would read and write the ANSI code page); no bytecode beside the installed files.
    return {QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-B"), QDir::toNativeSeparators(tools.waker), QStringLiteral("run")};
}

int restartDelayMs(const QList<int> &delays, int failures)
{
    if (delays.isEmpty()) return 0;
    return delays.at(qBound(0, failures - 1, int(delays.size()) - 1));
}

}

namespace Thoughts
{

namespace
{
// The waker's exit code when another waker runs in its home (thoughts_reply_waker.ALREADY_RUNNING).
constexpr int AlreadyRunning = 3;
// The standard error file is started afresh when it grew past this.
constexpr qint64 MaxErrorLogBytes = 1024 * 1024;

QString lastLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (file.size() > 4096) file.seek(file.size() - 4096);
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed().right(300);
}
}

WakerSupervisor *WakerSupervisor::instance()
{
    static QPointer<WakerSupervisor> supervisor;
    if (!supervisor) {
        supervisor = new WakerSupervisor(QCoreApplication::instance());
        // Ended with the app: the waker must not outlive it.
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, supervisor, [] {
            if (supervisor) supervisor->stop();
        });
    }
    return supervisor;
}

WakerSupervisor::WakerSupervisor(QObject *parent)
    : WakerSupervisor(Options(), parent)
{
}

WakerSupervisor::WakerSupervisor(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_timer(new QTimer(this))
{
    m_clock.start();
    connect(m_timer, &QTimer::timeout, this, &WakerSupervisor::check);
}

WakerSupervisor::~WakerSupervisor()
{
    stopProcess();
}

QString WakerSupervisor::home() const
{
    return m_options.home.isEmpty() ? QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("reply-waker")) : m_options.home;
}

ClaudeCode::BundledTools WakerSupervisor::tools() const
{
    return m_options.tools ? *m_options.tools : ClaudeCode::bundledTools();
}

QString WakerSupervisor::claude() const
{
    if (!m_options.claudeExecutable.isEmpty()) return QFileInfo(m_options.claudeExecutable).isExecutable() ? m_options.claudeExecutable : QString();
    return ClaudeCode::find();
}

bool WakerSupervisor::wanted() const
{
    return m_options.wanted ? m_options.wanted() : AgentSessions::answerHere();
}

bool WakerSupervisor::signedIn() const
{
    if (m_options.signedIn) return m_options.signedIn();
    return !ServerAddress::storedToken(Platform::desktopDeviceAccount(), ServerAddress::configured()).isEmpty();
}

quint64 WakerSupervisor::connection() const
{
    return m_options.connection ? m_options.connection() : ServerAddress::connectionGeneration();
}

bool WakerSupervisor::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void WakerSupervisor::start()
{
    // Every window starts it; once is enough.
    if (m_timer->isActive()) return;
    if (m_options.registerAgain) registerAgainIfMoved();
    check();
    m_timer->start(m_options.checkIntervalMs);
}

void WakerSupervisor::stop()
{
    m_timer->stop();
    stopProcess();
}

void WakerSupervisor::check()
{
    const ClaudeCode::BundledTools installed = tools();
    QString why;
    if (!installed.hasWaker()) why = QStringLiteral("this installation has no waker");
    else if (!wanted()) why = QStringLiteral("turned off");
    else if (claude().isEmpty()) why = QStringLiteral("no Claude Code on this computer");
    else if (!signedIn()) why = QStringLiteral("not signed in");
    if (!why.isEmpty()) {
        if (isRunning()) {
            DeveloperLog::record(QStringLiteral("waker.stopping"), {{QStringLiteral("why"), why}});
            stopProcess();
        }
        m_problem = why;
        return;
    }
    if (isRunning()) {
        // Signed in again, or to another server: the waker reads the sign-in when it starts.
        if (connection() != m_launchedConnection) {
            DeveloperLog::record(QStringLiteral("waker.restarting"), {{QStringLiteral("why"), QStringLiteral("the sign-in changed")}});
            stopProcess();
            launch();
        }
        return;
    }
    if (m_clock.elapsed() < m_notBefore) return;
    launch();
}

void WakerSupervisor::launch()
{
    const ClaudeCode::BundledTools installed = tools();
    const QString claudePath = claude();
    const QDir folder(home());
    if (!QDir().mkpath(folder.filePath(QStringLiteral("logs")))) {
        m_problem = QStringLiteral("cannot make %1").arg(QDir::toNativeSeparators(folder.absolutePath()));
        return;
    }
    const QString machine = Wakers::machineName(m_options.machine.isEmpty() ? Platform::computerName() : m_options.machine);
    QSaveFile config(folder.filePath(QStringLiteral("config.json")));
    const QByteArray json = QJsonDocument(Wakers::configFor(installed, claudePath, machine)).toJson();
    if (!config.open(QIODevice::WriteOnly) || config.write(json) != json.size() || !config.commit()) {
        m_problem = QStringLiteral("cannot write the waker's settings");
        return;
    }
    const QString errors = folder.filePath(QStringLiteral("logs/stderr.log"));
    if (QFileInfo(errors).size() > MaxErrorLogBytes) {
        QFile::remove(errors + QStringLiteral(".1"));
        QFile::rename(errors, errors + QStringLiteral(".1"));
    }

    auto *process = new QProcess(this);
    process->setProgram(QFileInfo(installed.pythonw).isFile() ? installed.pythonw : installed.python);
    process->setArguments(Wakers::arguments(installed));
    QProcessEnvironment environment = ClaudeCode::environment(claudePath);
    environment.insert(QStringLiteral("REPLY_WAKER_HOME"), QDir::toNativeSeparators(folder.absolutePath()));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(folder.absolutePath());
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(errors, QIODevice::Append);
    // Standard input stays a pipe: closing it asks the waker to stop (stop_on_stdin_eof).
    connect(process, &QProcess::started, this, [this, process] {
        // Ends with the app, also after a crash (Windows).
        Platform::endWithThisProcess(process->processId());
        Q_EMIT started();
    });
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        if (process == m_process) processEnded(exitCode, status != QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_process) processEnded(-1, true);
    });
    m_process = process;
    m_stopping = false;
    m_launchedConnection = connection();
    ++m_starts;
    m_runTime.start();
    m_problem.clear();
    DeveloperLog::record(QStringLiteral("waker.started"), {{QStringLiteral("starts"), m_starts}});
    process->start();
}

void WakerSupervisor::processEnded(int exitCode, bool crashed)
{
    QProcess *process = m_process;
    m_process = nullptr;
    if (process) process->deleteLater();
    const bool asked = m_stopping;
    m_stopping = false;
    Q_EMIT ended(exitCode);
    if (asked) return;
    // It ended by itself: started again later, and later the more often it fails quickly.
    if (m_runTime.isValid() && m_runTime.elapsed() >= m_options.healthyRunMs) m_failures = 0;
    ++m_failures;
    const int delay = exitCode == AlreadyRunning ? qMax(Wakers::restartDelayMs(m_options.restartDelaysMs, m_failures), 60000)
                                                 : Wakers::restartDelayMs(m_options.restartDelaysMs, m_failures);
    m_notBefore = m_clock.elapsed() + delay;
    const QString said = lastLine(QDir(home()).filePath(QStringLiteral("logs/stderr.log")));
    m_problem = exitCode == AlreadyRunning ? QStringLiteral("another waker runs for this computer")
                                           : QStringLiteral("the waker ended (%1)%2").arg(crashed ? QStringLiteral("crashed") : QString::number(exitCode),
                                                                                           said.isEmpty() ? QString() : QStringLiteral(": ") + said);
    DeveloperLog::record(QStringLiteral("waker.ended"),
                         {{QStringLiteral("exit_code"), exitCode}, {QStringLiteral("crashed"), crashed}, {QStringLiteral("failures"), m_failures}, {QStringLiteral("next_in_ms"), delay}});
    // Started again by the next check after the delay; at once when the delay is shorter than a check.
    if (m_timer->isActive() && delay < m_options.checkIntervalMs) QTimer::singleShot(delay, this, &WakerSupervisor::check);
}

void WakerSupervisor::stopProcess()
{
    QProcess *process = m_process;
    if (!process || process->state() == QProcess::NotRunning) return;
    m_stopping = true;
    // The waker stops when its standard input ends; one that does not is ended.
    process->closeWriteChannel();
    if (!process->waitForFinished(m_options.stopWaitMs)) {
        process->kill();
        process->waitForFinished(2000);
    }
    if (m_process == process) {
        // finished() came, or will not come any more.
        m_process = nullptr;
        process->deleteLater();
    }
    m_stopping = false;
}

void WakerSupervisor::registerAgainIfMoved()
{
    const ClaudeCode::BundledTools installed = tools();
    if (m_registering || !ClaudeCode::needsRegisteringAgain(AgentSessions::registeredTools(), installed)) return;
    m_registering = true;
    ClaudeCode::Setup::Options options;
    options.claudeExecutable = m_options.claudeExecutable;
    options.tools = installed;
    auto *setup = new ClaudeCode::Setup(options, this);
    connect(setup, &ClaudeCode::Setup::finished, this, [this, setup, installed](const ClaudeCode::Result &result) {
        m_registering = false;
        setup->deleteLater();
        const bool registered = result.outcome == ClaudeCode::Outcome::Done || result.outcome == ClaudeCode::Outcome::NotSignedIn;
        if (registered) AgentSessions::setRegisteredTools(installed.fingerprint());
        DeveloperLog::record(QStringLiteral("claude_code.registered_again"), {{QStringLiteral("outcome"), int(result.outcome)}, {QStringLiteral("registered"), registered}});
    });
    setup->start();
}

}
