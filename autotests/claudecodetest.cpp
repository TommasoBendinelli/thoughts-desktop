// SPDX-License-Identifier: GPL-2.0-or-later
// Claude Code on the person's own computer: the Thoughts tools an installation brings, Set up
// Claude Code against a fake claude, and the app's supervision of the reply waker (Windows) against
// a fake Python, all in a temporary folder.
#include "../part/agentsessionsettings.h"
#include "../part/claudecode.h"
#include "../shell/wakersupervisor.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Thoughts;

namespace
{
// claude as Set up Claude Code uses it. Each call appends its arguments (one line, tab-separated)
// to calls. FAKE_CLAUDE_MODE: signedout (auth status says so), refuse (mcp add fails).
const char fakeClaudeScript[] = R"(#!/bin/sh
dir="$FAKE_DIR"
line=""
for argument in "$@"; do line="$line$argument	"; done
echo "$line" >> "$dir/calls"
case "$1 $2" in
    "mcp remove")
        echo "No user-scoped MCP server found with name: thoughts" >&2
        exit 1 ;;
    "mcp add")
        if [ "$FAKE_CLAUDE_MODE" = refuse ]; then echo "Invalid environment variable format" >&2; exit 1; fi
        echo "Added stdio MCP server thoughts to user config"
        exit 0 ;;
    "auth status")
        if [ "$FAKE_CLAUDE_MODE" = signedout ]; then echo '{"loggedIn": false}'; else echo '{"loggedIn": true}'; fi
        exit 0 ;;
esac
exit 2
)";

// The Python the waker runs with: writes its arguments, its home and folder, then waits for its
// standard input to end (as the waker does with stop_on_stdin_eof) and says so.
// FAKE_WAKER_MODE: crash (ends at once with 1), busy (another waker runs: 3), deaf (ignores its
// standard input and must be ended).
const char fakePythonScript[] = R"(#!/bin/sh
dir="$FAKE_DIR"
echo "$$" >> "$dir/starts"
printf '%s\n' "$@" > "$dir/argv"
echo "$REPLY_WAKER_HOME" > "$dir/home"
pwd -P > "$dir/cwd"
case "$FAKE_WAKER_MODE" in
    crash) echo "Traceback: no Thoughts sign-in" >&2; exit 1 ;;
    busy) exit 3 ;;
    deaf) trap '' TERM; while true; do sleep 1; done ;;
esac
cat > /dev/null
echo "$$" >> "$dir/eof"
exit 0
)";

void writeExecutable(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
    file.close();
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}

void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QStringList lines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}
}

class ClaudeCodeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void findsTheInstallationsTools();
    void refusesToolsOutsideTheInstallation();
    void registrationCommandLine();
    void batchFilesAndWords();
    void settingsFollowSetUp();

    void setUpRegistersTheTools();
    void setUpSaysWhenSignedOut();
    void setUpSaysWhatClaudeSaid();
    void setUpWithoutClaudeOrTools();

    void waker_configAndNames();
    void waker_startsWithItsHomeAndStopsThroughItsInput();
    void waker_startsOnlyWhenWantedAndSignedIn();
    void waker_isStartedAgainWhenItEnds();
    void waker_anotherOneRunning();
    void waker_newSignInStartsItAgain();
    void waker_oneThatDoesNotStopIsEnded();

private:
    ClaudeCode::BundledTools makeInstallation(const QString &name = QStringLiteral("inst"));
    ClaudeCode::Result setUp(ClaudeCode::BundledTools tools, const QString &claude);
    std::unique_ptr<WakerSupervisor> supervisor(std::function<bool()> wanted = [] { return true; }, std::function<bool()> signedIn = [] { return true; },
                                                std::function<quint64()> connection = [] { return quint64(1); });

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_claude;
    QString m_python;
    QString m_fake;
};

void ClaudeCodeTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void ClaudeCodeTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    const QDir root(m_dir->path());
    m_fake = root.filePath(QStringLiteral("fake"));
    QVERIFY(QDir().mkpath(m_fake));
    m_claude = root.filePath(QStringLiteral("bin/claude"));
    writeExecutable(m_claude, fakeClaudeScript);
    qputenv("FAKE_DIR", m_fake.toUtf8());
    qunsetenv("FAKE_CLAUDE_MODE");
    qunsetenv("FAKE_WAKER_MODE");
}

void ClaudeCodeTest::cleanup()
{
    m_dir.reset();
}

ClaudeCode::BundledTools ClaudeCodeTest::makeInstallation(const QString &name)
{
    // The Windows installer's tree, with a fake Python: bin/, python-<id>/, tools/.
    const QDir root(QDir(m_dir->path()).filePath(name));
    m_python = root.filePath(QStringLiteral("python-1a2b3c4d5e6f/pythonw.exe"));
    writeExecutable(m_python, fakePythonScript);
    writeExecutable(root.filePath(QStringLiteral("python-1a2b3c4d5e6f/python.exe")), fakePythonScript);
    writeFile(root.filePath(QStringLiteral("tools/mcp/server.py")), "# the Thoughts tools\n");
    writeFile(root.filePath(QStringLiteral("tools/mcp/agent_notes.py")), "# their sign-in\n");
    writeFile(root.filePath(QStringLiteral("tools/waker/thoughts_reply_waker.py")), "# the waker\n");
    writeExecutable(root.filePath(QStringLiteral("bin/thoughtsctl")), "#!/bin/sh\n");
    const QJsonObject json {{QStringLiteral("python"), QStringLiteral("python-1a2b3c4d5e6f/python.exe")},
                            {QStringLiteral("pythonw"), QStringLiteral("python-1a2b3c4d5e6f/pythonw.exe")},
                            {QStringLiteral("mcp_server"), QStringLiteral("tools/mcp/server.py")},
                            {QStringLiteral("waker"), QStringLiteral("tools/waker/thoughts_reply_waker.py")},
                            {QStringLiteral("python_version"), QStringLiteral("3.13.9")}};
    writeFile(root.filePath(QStringLiteral("tools/agent-tools.json")), QJsonDocument(json).toJson());
    return ClaudeCode::bundledTools(root.filePath(QStringLiteral("bin")));
}

// ---------------------------------------------------------------- the installation's tools

void ClaudeCodeTest::findsTheInstallationsTools()
{
    const ClaudeCode::BundledTools tools = makeInstallation();
    const QDir root(QDir(m_dir->path()).filePath(QStringLiteral("inst")));
    QCOMPARE(tools.root, root.absolutePath());
    QCOMPARE(tools.python, root.filePath(QStringLiteral("python-1a2b3c4d5e6f/python.exe")));
    QCOMPARE(tools.pythonw, root.filePath(QStringLiteral("python-1a2b3c4d5e6f/pythonw.exe")));
    QCOMPARE(tools.mcpServer, root.filePath(QStringLiteral("tools/mcp/server.py")));
    QCOMPARE(tools.agentNotes, root.filePath(QStringLiteral("tools/mcp/agent_notes.py")));
    QCOMPARE(tools.waker, root.filePath(QStringLiteral("tools/waker/thoughts_reply_waker.py")));
    QVERIFY(tools.thoughtsctl.startsWith(root.filePath(QStringLiteral("bin/thoughtsctl"))));
    QCOMPARE(tools.pythonVersion, QStringLiteral("3.13.9"));
    QVERIFY(tools.hasTools());
    QVERIFY(tools.hasWaker());
    QVERIFY(tools.fingerprint().contains(QStringLiteral("python-1a2b3c4d5e6f")));
    // Another Python after an update: registered again; the same one: not.
    QVERIFY(!ClaudeCode::needsRegisteringAgain(tools.fingerprint(), tools));
    QVERIFY(ClaudeCode::needsRegisteringAgain(QStringLiteral("C:/old/python-000000000000/python.exe\nC:/old/tools/mcp/server.py"), tools));
    QVERIFY(!ClaudeCode::needsRegisteringAgain(QString(), tools));  // never set up: nothing to do
    // An installation without them (the macOS app): nothing.
    const ClaudeCode::BundledTools none = ClaudeCode::bundledTools(QDir(m_dir->path()).filePath(QStringLiteral("elsewhere/bin")));
    QVERIFY(!none.hasTools());
    QVERIFY(!none.hasWaker());
    QVERIFY(!ClaudeCode::needsRegisteringAgain(tools.fingerprint(), none));
}

void ClaudeCodeTest::refusesToolsOutsideTheInstallation()
{
    const QDir root(QDir(m_dir->path()).filePath(QStringLiteral("odd")));
    const QJsonObject json {{QStringLiteral("python"), QStringLiteral("/usr/bin/python3")}, {QStringLiteral("mcp_server"), QStringLiteral("../../server.py")}};
    writeFile(root.filePath(QStringLiteral("tools/agent-tools.json")), QJsonDocument(json).toJson());
    const ClaudeCode::BundledTools tools = ClaudeCode::bundledTools(root.filePath(QStringLiteral("bin")));
    QVERIFY(tools.python.isEmpty());
    QVERIFY(tools.mcpServer.isEmpty());
    QVERIFY(!tools.hasTools());
}

void ClaudeCodeTest::registrationCommandLine()
{
    const ClaudeCode::BundledTools tools = makeInstallation();
    QProcessEnvironment app;
    const auto variables = ClaudeCode::toolEnvironment(tools, app);
    QCOMPARE(variables.size(), 1);
    QCOMPARE(variables.at(0).first, QStringLiteral("THOUGHTSCTL"));
    QCOMPARE(variables.at(0).second, QDir::toNativeSeparators(tools.thoughtsctl));
    const QStringList arguments = ClaudeCode::addArguments(tools, variables);
    // The name before -e, whose values would otherwise take it; the server after --.
    QCOMPARE(arguments.mid(0, 5), QStringList({QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--scope"), QStringLiteral("user"), QStringLiteral("thoughts")}));
    QCOMPARE(arguments.mid(5, 2), QStringList({QStringLiteral("-e"), QStringLiteral("THOUGHTSCTL=") + QDir::toNativeSeparators(tools.thoughtsctl)}));
    QCOMPARE(arguments.mid(7),
             QStringList({QStringLiteral("--"), QDir::toNativeSeparators(tools.python), QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-B"),
                          QDir::toNativeSeparators(tools.mcpServer)}));
    QCOMPARE(ClaudeCode::removeArguments(), QStringList({QStringLiteral("mcp"), QStringLiteral("remove"), QStringLiteral("--scope"), QStringLiteral("user"), QStringLiteral("thoughts")}));
    // Another Library than the default one: both of its names go to the tools.
    app.insert(QStringLiteral("THOUGHTS_HOME"), QStringLiteral("/data/library"));
    const auto library = ClaudeCode::toolEnvironment(tools, app);
    QCOMPARE(library.size(), 3);
    QCOMPARE(library.at(1).first, QStringLiteral("THOUGHTS_HOME"));
    QCOMPARE(library.at(2).first, QStringLiteral("THOUGHTS_DOCS"));
}

void ClaudeCodeTest::batchFilesAndWords()
{
    QVERIFY(ClaudeCode::isBatchFile(QStringLiteral("C:/Users/alex/AppData/Roaming/npm/claude.cmd")));
    QVERIFY(!ClaudeCode::isBatchFile(QStringLiteral("C:/Users/alex/.local/bin/claude.exe")));
    QVERIFY(ClaudeCode::safeForBatchFile({QStringLiteral("C:\\Users\\alex\\AppData\\Local\\Programs\\Thoughts\\python-1a2b\\python.exe")}));
    for (const QString &odd : {QStringLiteral("C:\\Users\\R&D\\x"), QStringLiteral("100%"), QStringLiteral("a^b"), QStringLiteral("say \"hi\""), QStringLiteral("wow!")})
        QVERIFY2(!ClaudeCode::safeForBatchFile({odd}), qPrintable(odd));
    // Every outcome in plain words.
    for (ClaudeCode::Outcome outcome : {ClaudeCode::Outcome::Done, ClaudeCode::Outcome::NotSignedIn, ClaudeCode::Outcome::NotInstalled, ClaudeCode::Outcome::NoTools,
                                        ClaudeCode::Outcome::UnusualFolder, ClaudeCode::Outcome::Failed}) {
        const QString words = ClaudeCode::describe({outcome, QString(), QStringLiteral("what it said")});
        QVERIFY(words.size() > 20);
        QVERIFY2(!words.contains(QStringLiteral("MCP")) && !words.contains(QStringLiteral("stdio")), qPrintable(words));
    }
    QVERIFY(ClaudeCode::describe({ClaudeCode::Outcome::NotSignedIn, {}, {}}).contains(QStringLiteral("sign in")));
    QVERIFY(ClaudeCode::describe({ClaudeCode::Outcome::NotInstalled, {}, {}}).contains(QStringLiteral("claude.com/claude-code")));
    QVERIFY(ClaudeCode::describe({ClaudeCode::Outcome::Failed, {}, QStringLiteral("Invalid environment variable format")}).contains(QStringLiteral("Invalid environment")));
    QCOMPARE(*ClaudeCode::loggedIn(R"({"loggedIn": true})"), true);
    QVERIFY(!ClaudeCode::loggedIn("unknown command").has_value());
}

void ClaudeCodeTest::settingsFollowSetUp()
{
    AgentSessions::setRegisteredTools(QString());
    KConfigGroup(KSharedConfig::openConfig(), AgentSessions::configGroup()).deleteEntry(AgentSessions::answerHereKey());
    // Not set up: the sessions do not answer; set up: they do, until the person says otherwise.
    QVERIFY(!AgentSessions::answerHere());
    AgentSessions::setRegisteredTools(QStringLiteral("python\nserver"));
    QVERIFY(AgentSessions::answerHere());
    AgentSessions::setAnswerHere(false);
    QVERIFY(!AgentSessions::answerHere());
    QVERIFY(KConfigGroup(KSharedConfig::openConfig(), AgentSessions::configGroup()).hasKey(AgentSessions::answerHereKey()));
    AgentSessions::setAnswerHere(true);  // the default again: no entry, it follows Set up again
    QVERIFY(!KConfigGroup(KSharedConfig::openConfig(), AgentSessions::configGroup()).hasKey(AgentSessions::answerHereKey()));
    QVERIFY(AgentSessions::answerHere());
    AgentSessions::setRegisteredTools(QString());
    // This computer's id: made once, the same every time, and naming nothing.
    const QString id = AgentSessions::computerId();
    QCOMPARE(id.size(), 36);
    QCOMPARE(AgentSessions::computerId(), id);
    QCOMPARE(AgentSessions::oneComputerWhere(), QStringLiteral("one_computer"));
}

// ---------------------------------------------------------------- Set up Claude Code

ClaudeCode::Result ClaudeCodeTest::setUp(ClaudeCode::BundledTools tools, const QString &claude)
{
    ClaudeCode::Setup::Options options;
    options.tools = std::move(tools);
    options.claudeExecutable = claude;
    options.timeoutMs = 20000;
    ClaudeCode::Setup setup(options, nullptr);
    std::optional<ClaudeCode::Result> result;
    int finished = 0;
    connect(&setup, &ClaudeCode::Setup::finished, this, [&](const ClaudeCode::Result &given) {
        result = given;
        ++finished;
    });
    setup.start();
    if (!QTest::qWaitFor([&] { return result.has_value(); }, 30000)) return {ClaudeCode::Outcome::Failed, {}, QStringLiteral("no answer")};
    QTest::qWait(50);
    if (finished != 1) return {ClaudeCode::Outcome::Failed, {}, QStringLiteral("finished more than once")};
    return *result;
}

void ClaudeCodeTest::setUpRegistersTheTools()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    const ClaudeCode::BundledTools tools = makeInstallation();
    const ClaudeCode::Result result = setUp(tools, m_claude);
    QCOMPARE(int(result.outcome), int(ClaudeCode::Outcome::Done));
    QCOMPARE(result.claude, m_claude);
    // An earlier registration goes (there was none: no matter), the tools are added, the sign-in asked.
    const QStringList calls = lines(QDir(m_fake).filePath(QStringLiteral("calls")));
    QCOMPARE(calls.size(), 3);
    QCOMPARE(calls.at(0), QStringLiteral("mcp\tremove\t--scope\tuser\tthoughts\t"));
    QCOMPARE(calls.at(1), ClaudeCode::addArguments(tools, ClaudeCode::toolEnvironment(tools)).join(QLatin1Char('\t')) + QLatin1Char('\t'));
    QCOMPARE(calls.at(2), QStringLiteral("auth\tstatus\t--json\t"));
}

void ClaudeCodeTest::setUpSaysWhenSignedOut()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    qputenv("FAKE_CLAUDE_MODE", "signedout");
    const ClaudeCode::Result result = setUp(makeInstallation(), m_claude);
    QCOMPARE(int(result.outcome), int(ClaudeCode::Outcome::NotSignedIn));
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("calls"))).size(), 3);  // registered all the same
}

void ClaudeCodeTest::setUpSaysWhatClaudeSaid()
{
#ifdef Q_OS_WIN
    QSKIP("The fake claude is a shell script.");
#endif
    qputenv("FAKE_CLAUDE_MODE", "refuse");
    const ClaudeCode::Result result = setUp(makeInstallation(), m_claude);
    QCOMPARE(int(result.outcome), int(ClaudeCode::Outcome::Failed));
    QVERIFY2(result.detail.contains(QStringLiteral("Invalid environment variable format")), qPrintable(result.detail));
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("calls"))).size(), 2);  // no sign-in question after a refusal
}

void ClaudeCodeTest::setUpWithoutClaudeOrTools()
{
    const ClaudeCode::Result missing = setUp(makeInstallation(), QDir(m_dir->path()).filePath(QStringLiteral("nowhere/claude")));
    QCOMPARE(int(missing.outcome), int(ClaudeCode::Outcome::NotInstalled));
    const ClaudeCode::Result noTools = setUp(ClaudeCode::BundledTools {}, m_claude);
    QCOMPARE(int(noTools.outcome), int(ClaudeCode::Outcome::NoTools));
    QVERIFY(lines(QDir(m_fake).filePath(QStringLiteral("calls"))).isEmpty());
}

// ---------------------------------------------------------------- the waker's supervisor

std::unique_ptr<WakerSupervisor> ClaudeCodeTest::supervisor(std::function<bool()> wanted, std::function<bool()> signedIn, std::function<quint64()> connection)
{
    WakerSupervisor::Options options;
    options.tools = makeInstallation();
    options.home = QDir(m_dir->path()).filePath(QStringLiteral("app-data/reply-waker"));
    options.claudeExecutable = m_claude;
    options.machine = QStringLiteral("Alex's PC");
    options.wanted = std::move(wanted);
    options.signedIn = std::move(signedIn);
    options.connection = std::move(connection);
    options.checkIntervalMs = 100;
    options.stopWaitMs = 2000;
    options.restartDelaysMs = {200, 400};
    options.registerAgain = false;
    return std::make_unique<WakerSupervisor>(options, nullptr);
}

void ClaudeCodeTest::waker_configAndNames()
{
    QCOMPARE(Wakers::machineName(QStringLiteral("Alex's PC")), QStringLiteral("Alex-s-PC"));
    QCOMPARE(Wakers::machineName(QStringLiteral("laptop.local")), QStringLiteral("laptop"));
    QCOMPARE(Wakers::machineName(QString()), QStringLiteral("machine"));
    QCOMPARE(Wakers::machineName(QString(80, QLatin1Char('x'))).size(), 64);
    const QList<int> delays {5000, 15000, 60000};
    QCOMPARE(Wakers::restartDelayMs(delays, 0), 5000);
    QCOMPARE(Wakers::restartDelayMs(delays, 1), 5000);
    QCOMPARE(Wakers::restartDelayMs(delays, 2), 15000);
    QCOMPARE(Wakers::restartDelayMs(delays, 9), 60000);
    const ClaudeCode::BundledTools tools = makeInstallation();
    const QJsonObject config = Wakers::configFor(tools, m_claude, QStringLiteral("Alex-s-PC"));
    QCOMPARE(config.value(QStringLiteral("machine")).toString(), QStringLiteral("Alex-s-PC"));
    QCOMPARE(config.value(QStringLiteral("agent_notes")).toString(), QDir::toNativeSeparators(tools.agentNotes));
    QCOMPARE(config.value(QStringLiteral("claude")).toString(), QDir::toNativeSeparators(m_claude));
    QCOMPARE(config.value(QStringLiteral("start_sessions")), QJsonValue(false));  // the app's SessionStarter starts sessions
    QCOMPARE(config.value(QStringLiteral("stop_on_stdin_eof")), QJsonValue(true));
    QCOMPARE(config.value(QStringLiteral("log_file")).toString(), QStringLiteral("logs/waker.log"));
    QVERIFY(!config.contains(QStringLiteral("fallback_session_id")));
    QCOMPARE(Wakers::arguments(tools),
             QStringList({QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-B"), QDir::toNativeSeparators(tools.waker), QStringLiteral("run")}));
}

void ClaudeCodeTest::waker_startsWithItsHomeAndStopsThroughItsInput()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    auto waker = supervisor();
    waker->start();
    QTRY_VERIFY_WITH_TIMEOUT(waker->isRunning(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(m_fake).filePath(QStringLiteral("cwd"))), 10000);
    QCOMPARE(waker->starts(), 1);
    QVERIFY(waker->problem().isEmpty());
    const QString home = waker->home();
    const QString script = QDir::toNativeSeparators(QDir(m_dir->path()).filePath(QStringLiteral("inst/tools/waker/thoughts_reply_waker.py")));
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("argv"))), (QStringList {QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-B"), script, QStringLiteral("run")}));
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("home"))).value(0), QDir::toNativeSeparators(QDir(home).absolutePath()));
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("cwd"))).value(0), QFileInfo(home).canonicalFilePath());
    QFile config(QDir(home).filePath(QStringLiteral("config.json")));
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QJsonObject written = QJsonDocument::fromJson(config.readAll()).object();
    QCOMPARE(written.value(QStringLiteral("machine")).toString(), QStringLiteral("Alex-s-PC"));
    QCOMPARE(written.value(QStringLiteral("start_sessions")), QJsonValue(false));
    QVERIFY(QFileInfo(QDir(home).filePath(QStringLiteral("logs"))).isDir());
    // Checks while it runs start nothing more.
    QTest::qWait(400);
    QCOMPARE(waker->starts(), 1);
    // Stopped by the end of its standard input, not ended.
    waker->stop();
    QVERIFY(!waker->isRunning());
    QCOMPARE(lines(QDir(m_fake).filePath(QStringLiteral("eof"))).size(), 1);
    QTest::qWait(300);
    QCOMPARE(waker->starts(), 1);  // not started again after a stop
}

void ClaudeCodeTest::waker_startsOnlyWhenWantedAndSignedIn()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    bool wanted = false;
    bool signedIn = false;
    auto waker = supervisor([&wanted] { return wanted; }, [&signedIn] { return signedIn; });
    waker->start();
    QTest::qWait(300);
    QCOMPARE(waker->starts(), 0);
    QCOMPARE(waker->problem(), QStringLiteral("turned off"));
    wanted = true;
    QTRY_COMPARE_WITH_TIMEOUT(waker->problem(), QStringLiteral("not signed in"), 5000);
    QCOMPARE(waker->starts(), 0);
    signedIn = true;
    QTRY_VERIFY_WITH_TIMEOUT(waker->isRunning(), 10000);
    // Turned off: stopped through its input, and not started again.
    wanted = false;
    QTRY_VERIFY_WITH_TIMEOUT(!waker->isRunning(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(lines(QDir(m_fake).filePath(QStringLiteral("eof"))).size(), 1, 5000);
    QTest::qWait(300);
    QCOMPARE(waker->starts(), 1);
    waker->stop();
}

void ClaudeCodeTest::waker_isStartedAgainWhenItEnds()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    qputenv("FAKE_WAKER_MODE", "crash");
    auto waker = supervisor();
    QElapsedTimer clock;
    clock.start();
    waker->start();
    QTRY_VERIFY_WITH_TIMEOUT(waker->starts() >= 3, 10000);
    // 200 ms, then 400 ms (and 400 ms from then on) between the starts.
    QVERIFY2(clock.elapsed() >= 550, qPrintable(QString::number(clock.elapsed())));
    QTRY_VERIFY_WITH_TIMEOUT(waker->problem().contains(QStringLiteral("no Thoughts sign-in")), 5000);
    QVERIFY(waker->problem().startsWith(QStringLiteral("the waker ended (1)")));
    // Healthy again: running.
    qunsetenv("FAKE_WAKER_MODE");
    QTRY_VERIFY_WITH_TIMEOUT(waker->isRunning(), 10000);
    waker->stop();
}

void ClaudeCodeTest::waker_anotherOneRunning()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    qputenv("FAKE_WAKER_MODE", "busy");
    auto waker = supervisor();
    waker->start();
    QTRY_COMPARE_WITH_TIMEOUT(waker->problem(), QStringLiteral("another waker runs for this computer"), 10000);
    // Asked again only after a minute, not every few seconds.
    QTest::qWait(800);
    QCOMPARE(waker->starts(), 1);
    waker->stop();
}

void ClaudeCodeTest::waker_newSignInStartsItAgain()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    quint64 connection = 1;
    auto waker = supervisor([] { return true; }, [] { return true; }, [&connection] { return connection; });
    waker->start();
    QTRY_VERIFY_WITH_TIMEOUT(waker->isRunning(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(m_fake).filePath(QStringLiteral("cwd"))), 10000);
    connection = 2;
    QTRY_COMPARE_WITH_TIMEOUT(waker->starts(), 2, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(lines(QDir(m_fake).filePath(QStringLiteral("eof"))).size(), 1, 5000);  // the first one stopped
    QVERIFY(waker->isRunning());
    waker->stop();
}

void ClaudeCodeTest::waker_oneThatDoesNotStopIsEnded()
{
#ifdef Q_OS_WIN
    QSKIP("The fake Python is a shell script.");
#endif
    qputenv("FAKE_WAKER_MODE", "deaf");
    auto waker = supervisor();
    waker->start();
    QTRY_VERIFY_WITH_TIMEOUT(waker->isRunning(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(m_fake).filePath(QStringLiteral("cwd"))), 10000);
    QElapsedTimer clock;
    clock.start();
    waker->stop();
    QVERIFY(!waker->isRunning());
    QVERIFY(clock.elapsed() < 6000);
    QVERIFY(lines(QDir(m_fake).filePath(QStringLiteral("eof"))).isEmpty());
}

QTEST_MAIN(ClaudeCodeTest)
#include "claudecodetest.moc"
