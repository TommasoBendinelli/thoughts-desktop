// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/claudecode.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

class QProcess;
class QTimer;

// Agents on the person's own Windows PC (27 Nov 2026): on a Mac or Linux computer a reply waker the
// person installs (the Thoughts connectors' reply-agent/thoughts_reply_waker.py) registers the
// computer's Claude Code sessions, so they appear in Send to Agent and Ask Agent, and brings them
// the pages and replies that are theirs. On Windows the app does that job itself: it runs the same
// waker (tools/waker, installed with its own Python) while "Let Claude Code sessions on this
// computer answer" is on (AgentSessions::answerHere(), on once Set up Claude Code registered the
// Thoughts tools) and this computer is signed in; starts it again when it ends (later and later
// when it keeps failing), and when the sign-in changes; stops it when the setting goes off and when
// the app quits (it closes the waker's standard input, then ends it); and ties it to the app, so it
// ends with the app even after a crash. The waker never starts sessions here: the app's
// SessionStarter does (start_sessions false). After an update that moved the tools (another
// Python), the supervisor registers them for Claude Code again.
namespace Thoughts::Wakers
{
// The waker's config.json (its DEFAULTS cover the rest): this computer's name, the sign-in through
// the Thoughts tools (agent_notes), the claude to run, no session starts, its log, and a stop when
// its standard input ends.
QJsonObject configFor(const ClaudeCode::BundledTools &tools, const QString &claude, const QString &machine);
// This computer as the waker names it on the server: letters, digits, ".", "_" and "-", at most 64
// ("machine" otherwise), as the waker would make it.
QString machineName(const QString &computerName);
// python -X utf8 -B <waker> run.
QStringList arguments(const ClaudeCode::BundledTools &tools);
// The wait before the next start after @p failures quick ends in a row (at least the first delay).
int restartDelayMs(const QList<int> &delays, int failures);
}

namespace Thoughts
{

class WakerSupervisor : public QObject
{
    Q_OBJECT
public:
    struct Options {
        // Set (tests): these tools instead of the installation's.
        std::optional<ClaudeCode::BundledTools> tools;
        // The waker's home; empty: <application data>/reply-waker.
        QString home;
        // Set (tests): this claude only, never a search.
        QString claudeExecutable;
        // This computer's name; empty: Platform::computerName().
        QString machine;
        // Whether the person wants it; unset: AgentSessions::answerHere().
        std::function<bool()> wanted;
        // Whether this computer is signed in; unset: its device token is stored for the server in use.
        std::function<bool()> signedIn;
        // Something that changes when the person signs in again (or to another server); unset:
        // ServerAddress::connectionGeneration().
        std::function<quint64()> connection;
        int checkIntervalMs = 10000;
        int stopWaitMs = 3000;
        // A run this long was healthy: the next end starts the delays again from the first.
        int healthyRunMs = 10 * 60 * 1000;
        QList<int> restartDelaysMs {5000, 15000, 60000, 5 * 60000, 10 * 60000};
        // Register the tools again after an update moved them (tests: off).
        bool registerAgain = true;
    };

    // The app's (Windows; it stops the waker when the app quits).
    static WakerSupervisor *instance();
    explicit WakerSupervisor(QObject *parent = nullptr);
    WakerSupervisor(Options options, QObject *parent);
    ~WakerSupervisor() override;

    // Checks now and every checkIntervalMs whether the waker should run, and starts or stops it.
    void start();
    // Stops checking and stops the waker (waits up to stopWaitMs for it, then ends it).
    void stop();
    // One check (start() runs it on a timer).
    void check();

    bool isRunning() const;
    // How many times the waker was started.
    int starts() const { return m_starts; }
    // Why it does not run, or how it last ended; empty while all is well.
    QString problem() const { return m_problem; }
    QString home() const;

Q_SIGNALS:
    void started();
    void ended(int exitCode);

private:
    void launch();
    void stopProcess();
    void processEnded(int exitCode, bool crashed);
    void registerAgainIfMoved();
    ClaudeCode::BundledTools tools() const;
    QString claude() const;
    bool wanted() const;
    bool signedIn() const;
    quint64 connection() const;

    Options m_options;
    QTimer *m_timer;
    QPointer<QProcess> m_process;
    bool m_running = false;
    bool m_stopping = false;
    int m_starts = 0;
    int m_failures = 0;
    qint64 m_notBefore = 0;  // ms since the supervisor's clock started
    QElapsedTimer m_clock;
    QElapsedTimer m_runTime;
    quint64 m_launchedConnection = 0;
    QString m_problem;
    bool m_registering = false;
};

}
