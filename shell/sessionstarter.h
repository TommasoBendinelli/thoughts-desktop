// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/agentsessionsettings.h"
#include "agentnotes.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QMessageBox;
class QTimer;

// The phone's "Start a new study session" on the owner's own computer (product decision of
// 26 Nov 2026): the phone sends a page (origin.kind "phone_page", origin.start_session true,
// origin.agent "claude" or "codex", absent meaning "claude") whose message is the prompt, and
// Thoughts on a computer of the owner starts an agent session for it with the owner's own
// Claude Code or Codex sign-in and the Thoughts tools they installed, in a folder of its own
// (~/Thoughts Sessions). The flow is the reply waker's start_session (connectors
// reply-agent/thoughts_reply_waker.py, feature/start-session 77ee6aa): start the session, claim
// the page for it (the first claim wins, so of two computers only one keeps its session), and send
// the owner a note saying where it runs, or why it could not start.
//
// Send to Agent's and Ask Agent's "New session on this computer" (27 Nov 2026) sends the same kind
// of page from this computer for this computer only: origin.where "one_computer" and
// origin.computer this computer's id (AgentSessions::computerId()). Only the computer it names acts
// on it, at once (the person asked for it there), and its text (what Copy for agent copies, or the
// question) is the new session's first prompt. The reply wakers leave such a page alone (they start
// sessions only for where "computer"), and the server too (it starts those with where "server").
namespace Thoughts::SessionStarts
{
enum class Agent { Claude, Codex };
// "Claude Code", "Codex".
QString agentName(Agent agent);

// Pages older than this when they reach this computer are never acted on.
constexpr int MaxPageAgeSeconds = 15 * 60;

struct PageDecision {
    bool act = false;
    // A request for a new session at all (remembered as handled even when not acted on).
    bool startSession = false;
    Agent agent = Agent::Claude;
    // Asked for on this computer (where "one_computer" naming it): started without a question.
    bool askedHere = false;
    QString why;  // when it is not acted on
};
// Whether this computer (@p computerId) acts on a page of the pages feed: a phone page asking for a
// new session, not deleted, not handled here before, at most MaxPageAgeSeconds old, for an agent
// this computer can start, and for any of the person's computers (where "computer" or none) or for
// this one (where "one_computer", origin.computer @p computerId); never one for the server (where
// "server") or another computer. Whether another computer took it already is the server's to say
// (handling).
PageDecision decidePage(const AgentNotes::Note &page, const QDateTime &nowUtc, const QSet<QString> &handled, const QString &computerId = {});

// What this computer's choice means for a request.
enum class Consent { Start, Ask, Decline };
Consent consentFor(AgentSessions::PhoneStart choice);
// The question's answers ("Start", "Always", "Not on this computer", or closed unanswered) and
// what follows: whether the session starts and the computer's new choice.
enum class AskAnswer { Start, Always, NotHere, Dismissed };
struct AskOutcome {
    bool start = false;
    std::optional<AgentSessions::PhoneStart> newChoice;
};
AskOutcome outcomeOf(AskAnswer answer);

// "Study session · 26 Nov 15:04" (local time); "Thoughts session · 26 Nov 15:04" for one asked for
// on this computer.
QString sessionName(const QDateTime &local, bool askedHere = false);
// The line before the owner's prompt: who started the session, when, and for which page.
QString promptHeader(Agent agent, const QString &computer, const QDateTime &local, const QString &pageId, bool askedHere = false);
// claude --bg -n <name> --permission-mode auto <prompt>: a background session in auto mode (nobody
// is there to answer permission prompts; auto's classifier still checks each action). Never
// bypassPermissions.
QStringList claudeArguments(const QString &name, const QString &prompt);
// codex exec in @p folder, the prompt read from standard input ("-"), its events as JSON lines,
// with --approve-for-me (approvals by Codex's automatic review, commands in the workspace-write
// sandbox), as auto mode for Claude: nobody is there to answer, and a session asked for from a
// phone never runs without approvals or sandbox, whatever the comments' Codex setting says.
QStringList codexArguments(const QString &folder);

// What `claude --bg` prints when the session started: "backgrounded · <short id> · <name>".
struct Backgrounded {
    QString shortId;
    QString name;
};
std::optional<Backgrounded> parseBackgrounded(const QString &output);
// The full session id of the background session @p shortId in `claude agents --json`; empty
// when it is not listed (yet).
QString sessionIdFromAgents(const QByteArray &json, const QString &shortId);
// `claude auth status --json`: whether Claude Code is signed in; nothing when it did not say.
std::optional<bool> claudeLoggedIn(const QByteArray &authStatusJson);
// The thread id in `codex exec --json` output (thread.started; an older Codex's
// session_configured); empty while there is none.
QString codexThreadId(const QByteArray &jsonLines);

// Why `claude --bg` did not start a session.
enum class ClaudeFailure {
    Untrusted,  // the folder is not trusted yet ("Workspace not trusted. Run `claude` in <dir> once …")
    SignedOut,
    TooOld,  // a Claude Code without --bg
    Other,
};
ClaudeFailure classifyClaudeFailure(const QString &output);

// Where claude is looked for, in order: PATH, ~/.local/bin, /opt/homebrew/bin, /usr/local/bin,
// ~/.npm-global/bin, and on macOS the newest copy the Claude app keeps
// (~/Library/Application Support/Claude/claude-code/<version>/…/claude.app/Contents/MacOS/claude);
// on Windows %USERPROFILE%\.local\bin\claude.exe, PATH (as `where claude`) and npm's claude.cmd.
QStringList claudeCandidates();
// The first candidate that is an executable file; empty when there is none.
QString findClaude();
// A Windows batch file (npm's claude.cmd, codex.cmd): cmd.exe parses its command line, so no
// argument may hold the owner's text.
bool isBatchFile(const QString &program);
// The environment an agent runs with: this app's, without what a Claude Code session that started
// the app would pass on (CLAUDECODE, CLAUDE_CODE_*, ANTHROPIC_*, …: the owner's own sign-in counts),
// and with the executable's folder and the usual install folders on PATH (an app started from
// the Finder has a short PATH, and npm's claude needs node).
QProcessEnvironment agentEnvironment(const QString &executable, QProcessEnvironment base = QProcessEnvironment::systemEnvironment());
// A PNG with the note's title and message: the picture every agent note has.
QByteArray noticeImage(const QString &title, const QString &message);
}

namespace Thoughts
{

// One for the app (instance()), while it runs and is signed in: long-polls the pages feed
// (GET /v1/agent-notes/pages) from the newest page at start, and for each page decidePage() acts
// on that nobody claimed yet: asks or not (this computer's choice, AgentSessions::PhoneStart),
// starts the session, claims the page for it and sends the owner a note. Each page at most once
// (handled ids are kept in the state file).
class SessionStarter : public QObject
{
    Q_OBJECT
public:
    struct AskRequest {
        QString pageId;
        SessionStarts::Agent agent = SessionStarts::Agent::Claude;
        QString folder;
    };
    struct Options {
        // Where the sessions run; empty: AgentSessions::sessionsFolder(), made when needed.
        QString folder;
        // The handled pages; empty: <application data>/session-starter/state.json.
        QString statePath;
        // This computer in notes and prompts; empty: Platform::computerName().
        QString computerName;
        // This computer's id for pages meant for it only; empty: AgentSessions::computerId().
        QString computerId;
        // Set (tests): this executable only, never a search; a missing file counts as not installed.
        QString claudeExecutable;
        QString codexExecutable;
        // Set (tests): this choice instead of the setting, and the question's answers change it here.
        std::optional<AgentSessions::PhoneStart> consent;
        // Now, in UTC; unset: the system clock.
        std::function<QDateTime()> clock;
        // The question on this computer when the choice is Ask; unset: a non-modal dialog. @p answer
        // is called once.
        std::function<void(const AskRequest &request, std::function<void(SessionStarts::AskAnswer)> answer)> ask;
        // Claude Code does not trust the folder yet; unset: a dialog with Open Terminal Here.
        std::function<void(const QString &folder, const QString &claude)> untrusted;
        int processTimeoutMs = 90000;
        // `claude agents --json` is asked this often for the new session's full id.
        int agentsAttempts = 10;
        int agentsIntervalMs = 1000;
        // How long Codex may take to name its thread.
        int codexThreadWaitMs = 20000;
    };

    static SessionStarter *instance();
    SessionStarter(AgentNotes::Client *client, Options options, QObject *parent = nullptr);
    ~SessionStarter() override;

    // Starts (or keeps) the long poll; stop() ends it. Sessions being started go on.
    void start();
    void stop();
    bool isRunning() const { return m_running; }

    // How long one request of the pages feed waits (the server allows 60 s).
    static constexpr int PagesWaitSeconds = 25;

    // A terminal in @p folder that starts @p claude there, so Claude Code asks once whether it trusts
    // the folder (macOS: Terminal; Windows: a console window). False when none could be opened.
    static bool openTerminalHere(const QString &folder, const QString &claude);

Q_SIGNALS:
    // Once for each page acted on: "started", "taken" (another computer claimed it), "declined"
    // (Never, or Not on this computer), "untrusted", "not_installed", "signed_out" or "failed".
    void pageFinished(const QString &pageId, const QString &outcome);
    // A Claude Code session started for @p pageId and claimed it (before pageFinished "started"),
    // with its full id when `claude agents` named it: Send to Agent then sends to it next.
    void sessionStarted(const QString &pageId, const QString &sessionId, const QString &name);

private:
    struct Job;
    struct RunResult {
        bool started = false;
        bool timedOut = false;
        int exitCode = -1;
        QByteArray output;
        QByteArray errors;
    };
    using JobPointer = std::shared_ptr<Job>;

    void poll();
    void pagesAnswered(const AgentNotes::Client::PagesResult &result, qint64 elapsedMs);
    void consider(const AgentNotes::Note &page);
    void whenUnclaimed(const JobPointer &job, std::function<void()> then);
    void decideConsent(const JobPointer &job);
    void askNext();
    void showQuestion(const AskRequest &request, std::function<void(SessionStarts::AskAnswer)> answer);
    void showUntrusted(const QString &folder, const QString &claude);
    void launch(const JobPointer &job);
    void launchClaude(const JobPointer &job);
    void startClaudeSession(const JobPointer &job);
    void findClaudeSession(const JobPointer &job, int attempt);
    void launchCodex(const JobPointer &job);
    void startCodexSession(const JobPointer &job);
    void waitForCodexThread(const JobPointer &job, int waitedMs);
    void claim(const JobPointer &job);
    void withdraw(const JobPointer &job);
    void tellStarted(const JobPointer &job);
    void tellNotStarted(const JobPointer &job, const QString &outcome, const QString &title, const QString &message);
    void sendNote(const JobPointer &job, const QString &title, const QString &message, const QJsonObject &origin, const QString &outcome);
    void finish(const JobPointer &job, const QString &outcome, const QString &detail = {});
    void run(const QString &program, const QStringList &arguments, const QString &directory, int timeoutMs, std::function<void(const RunResult &)> done);

    AgentSessions::PhoneStart choice() const;
    void setChoice(AgentSessions::PhoneStart choice);
    QString folder() const;
    QString computer() const;
    QString computerId() const;
    QDateTime now() const;
    void remember(const QString &pageId);
    void loadState();
    void saveState() const;

    AgentNotes::Client *m_client;
    Options m_options;
    QTimer *m_timer;
    bool m_running = false;
    bool m_inFlight = false;
    quint64 m_generation = 0;
    // The feed's cursor (-1 before the first answer) and the sign-in it belongs to.
    qint64 m_cursor = -1;
    quint64 m_cursorConnection = 0;
    int m_failures = 0;
    bool m_stateLoaded = false;
    QStringList m_handledOrder;
    QSet<QString> m_handled;
    // Requests waiting for the question, which is asked for one at a time.
    QList<JobPointer> m_waiting;
    bool m_asking = false;
    QPointer<QMessageBox> m_untrustedBox;
};

}
