// SPDX-License-Identifier: GPL-2.0-or-later
#include "sessionstarter.h"

#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"
#include "commentcodexlauncher.h"
#include "developerlog.h"

#include <KLocalizedString>

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

#ifndef Q_OS_WIN
#include <csignal>
#endif

namespace Thoughts::SessionStarts
{

namespace
{
// A command line Windows can start (CreateProcess allows 32767 characters) with room for the rest.
#ifdef Q_OS_WIN
constexpr int MaxPromptArgumentCharacters = 24000;
#else
constexpr int MaxPromptArgumentCharacters = 200000;
#endif

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
}

QString agentName(Agent agent)
{
    return agent == Agent::Codex ? QStringLiteral("Codex") : QStringLiteral("Claude Code");
}

PageDecision decidePage(const AgentNotes::Note &page, const QDateTime &nowUtc, const QSet<QString> &handled)
{
    PageDecision decision;
    if (!page.isPhonePage()) {
        decision.why = QStringLiteral("not a page");
        return decision;
    }
    if (page.origin.value(QLatin1String("start_session")).toBool() != true) {
        decision.why = QStringLiteral("no new session asked for");
        return decision;
    }
    decision.startSession = true;
    if (page.deleted) {
        decision.why = QStringLiteral("deleted");
        return decision;
    }
    if (handled.contains(page.id)) {
        decision.why = QStringLiteral("handled on this computer already");
        return decision;
    }
    if (!page.createdAt.isValid()) {
        decision.why = QStringLiteral("no creation time");
        return decision;
    }
    if (page.createdAt.secsTo(nowUtc) > MaxPageAgeSeconds) {
        decision.why = QStringLiteral("older than 15 minutes");
        return decision;
    }
    const QString agent = page.origin.value(QLatin1String("agent")).toString().trimmed().toLower();
    if (agent.isEmpty() || agent == QLatin1String("claude")) {
        decision.agent = Agent::Claude;
    } else if (agent == QLatin1String("codex")) {
        decision.agent = Agent::Codex;
    } else {
        decision.why = QStringLiteral("an agent this computer cannot start: %1").arg(agent.left(40));
        return decision;
    }
    decision.act = true;
    return decision;
}

Consent consentFor(AgentSessions::PhoneStart choice)
{
    switch (choice) {
    case AgentSessions::PhoneStart::Always:
        return Consent::Start;
    case AgentSessions::PhoneStart::Never:
        return Consent::Decline;
    case AgentSessions::PhoneStart::Ask:
        break;
    }
    return Consent::Ask;
}

AskOutcome outcomeOf(AskAnswer answer)
{
    switch (answer) {
    case AskAnswer::Start:
        return {true, std::nullopt};
    case AskAnswer::Always:
        return {true, AgentSessions::PhoneStart::Always};
    case AskAnswer::NotHere:
        return {false, AgentSessions::PhoneStart::Never};
    case AskAnswer::Dismissed:
        break;
    }
    return {false, std::nullopt};
}

QString sessionName(const QDateTime &local)
{
    return QStringLiteral("Study session · %1").arg(QLocale::c().toString(local, QStringLiteral("d MMM HH:mm")));
}

QString promptHeader(Agent agent, const QString &computer, const QDateTime &local, const QString &pageId)
{
    QString header = QStringLiteral("[Thoughts on %1 started this session at %2 because the owner tapped Start on their phone's "
                                    "“Start a new study session” (page %3). Their prompt for it follows.")
                         .arg(computer, QLocale::c().toString(local, QStringLiteral("HH:mm")), pageId);
    if (agent == Agent::Codex) {
        header += QStringLiteral(" You run non-interactively (codex exec): nobody answers questions here, so answer the owner with Thoughts notes.");
    }
    return header + QLatin1Char(']');
}

QStringList claudeArguments(const QString &name, const QString &prompt)
{
    return {QStringLiteral("--bg"), QStringLiteral("-n"), name, QStringLiteral("--permission-mode"), QStringLiteral("auto"), prompt};
}

QStringList codexArguments(const QString &folder)
{
    return {QStringLiteral("exec"), QStringLiteral("--skip-git-repo-check"), QStringLiteral("--json"), QStringLiteral("-C"), folder,
            QStringLiteral("--approve-for-me"), QStringLiteral("-")};
}

std::optional<Backgrounded> parseBackgrounded(const QString &output)
{
    static const QRegularExpression line(QStringLiteral("backgrounded\\s*·\\s*([0-9a-f]{6,})(?:\\s*·\\s*([^\\r\\n]*))?"));
    const QRegularExpressionMatch match = line.match(output);
    if (!match.hasMatch()) return std::nullopt;
    return Backgrounded {match.captured(1), match.captured(2).trimmed()};
}

QString sessionIdFromAgents(const QByteArray &json, const QString &shortId)
{
    if (shortId.isEmpty()) return {};
    const QJsonDocument document = QJsonDocument::fromJson(json.trimmed());
    for (const QJsonValue &value : document.array()) {
        const QJsonObject row = value.toObject();
        const QString sessionId = row.value(QLatin1String("sessionId")).toString();
        if (sessionId.isEmpty()) continue;
        if (row.value(QLatin1String("id")).toString() == shortId || sessionId.startsWith(shortId)) return sessionId;
    }
    return {};
}

std::optional<bool> claudeLoggedIn(const QByteArray &authStatusJson)
{
    const QJsonValue loggedIn = QJsonDocument::fromJson(authStatusJson.trimmed()).object().value(QLatin1String("loggedIn"));
    if (!loggedIn.isBool()) return std::nullopt;
    return loggedIn.toBool();
}

QString codexThreadId(const QByteArray &jsonLines)
{
    for (const QByteArray &line : jsonLines.split('\n')) {
        const QJsonObject event = QJsonDocument::fromJson(line.trimmed()).object();
        if (event.value(QLatin1String("type")).toString() == QLatin1String("thread.started")) {
            const QString id = event.value(QLatin1String("thread_id")).toString();
            if (!id.isEmpty()) return id;
        }
        const QJsonObject message = event.value(QLatin1String("msg")).toObject();
        if (message.value(QLatin1String("type")).toString() == QLatin1String("session_configured")) {
            const QString id = message.value(QLatin1String("session_id")).toString();
            if (!id.isEmpty()) return id;
        }
    }
    return {};
}

ClaudeFailure classifyClaudeFailure(const QString &output)
{
    const QString text = output.toLower();
    if (text.contains(QLatin1String("not trusted"))) return ClaudeFailure::Untrusted;
    if ((text.contains(QLatin1String("unknown option")) || text.contains(QLatin1String("unrecognized option"))) && text.contains(QLatin1String("--bg")))
        return ClaudeFailure::TooOld;
    static const QRegularExpression signedOut(QStringLiteral("/login|not logged in|logged out|log in|sign in|signed in|authenticat|oauth|credential"));
    if (signedOut.match(text).hasMatch()) return ClaudeFailure::SignedOut;
    return ClaudeFailure::Other;
}

QStringList claudeCandidates()
{
    QStringList candidates;
    const QDir home = QDir::home();
#ifdef Q_OS_WIN
    candidates.append(home.filePath(QStringLiteral(".local/bin/claude.exe")));
    // QStandardPaths tries PATHEXT's suffixes, as `where claude` does.
    candidates.append(QStandardPaths::findExecutable(QStringLiteral("claude")));
    candidates.append(QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("npm/claude.cmd")));
#else
    candidates.append(QStandardPaths::findExecutable(QStringLiteral("claude")));
    candidates.append({home.filePath(QStringLiteral(".local/bin/claude")),
                       QStringLiteral("/opt/homebrew/bin/claude"),
                       QStringLiteral("/usr/local/bin/claude"),
                       home.filePath(QStringLiteral(".npm-global/bin/claude"))});
#ifdef Q_OS_MACOS
    candidates.append(bundledClaude());
#endif
#endif
    candidates.removeAll(QString());
    candidates.removeDuplicates();
    return candidates;
}

QString findClaude()
{
    for (const QString &candidate : claudeCandidates()) {
        if (isExecutableFile(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

bool isBatchFile(const QString &program)
{
    return program.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive) || program.endsWith(QLatin1String(".bat"), Qt::CaseInsensitive);
}

QProcessEnvironment agentEnvironment(const QString &executable, QProcessEnvironment base)
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

QByteArray noticeImage(const QString &title, const QString &message)
{
    constexpr int width = 1200;
    constexpr int margin = 60;
    constexpr int gap = 30;
    QFont titleFont;
    titleFont.setPixelSize(44);
    titleFont.setBold(true);
    QFont bodyFont;
    bodyFont.setPixelSize(34);
    const QRect area(0, 0, width - 2 * margin, 7000);
    const QRect titleBox = QFontMetrics(titleFont).boundingRect(area, Qt::TextWordWrap, title);
    const QRect bodyBox = QFontMetrics(bodyFont).boundingRect(area, Qt::TextWordWrap, message);
    const int height = qMin(8000, margin + titleBox.height() + gap + bodyBox.height() + margin);
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor(0x1f, 0x1f, 0x1f));
    painter.setFont(titleFont);
    painter.drawText(QRect(margin, margin, area.width(), titleBox.height()), Qt::TextWordWrap, title);
    painter.setFont(bodyFont);
    painter.drawText(QRect(margin, margin + titleBox.height() + gap, area.width(), height - margin - titleBox.height() - gap - margin), Qt::TextWordWrap, message);
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

}

namespace Thoughts
{

using namespace SessionStarts;

namespace
{
constexpr int HandledLimit = 500;

QString tail(const QByteArray &bytes, int characters = 300)
{
    return QString::fromUtf8(bytes).trimmed().right(characters);
}

void writeFile(const QString &path, const QByteArray &contents)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size()) file.commit();
}

[[maybe_unused]] QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}
}

struct SessionStarter::Job {
    QString pageId;
    QString prompt;
    Agent agent = Agent::Claude;
    QDateTime startedAt;  // local
    QString name;
    QString folder;
    QString executable;
    // Claude: the short id `claude --bg` printed and the session's full id; Codex: its thread id.
    QString shortId;
    QString sessionId;
    // Codex: its process and the files of this run.
    qint64 pid = 0;
    QString runDirectory;
    QString outputPath;
    QString errorsPath;
};

SessionStarter *SessionStarter::instance()
{
    static QPointer<SessionStarter> starter;
    if (!starter) {
        auto *client = new AgentNotes::Client(QString());
        starter = new SessionStarter(client, {}, QCoreApplication::instance());
        client->setParent(starter);
    }
    return starter;
}

SessionStarter::SessionStarter(AgentNotes::Client *client, Options options, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_options(std::move(options))
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SessionStarter::poll);
}

SessionStarter::~SessionStarter() = default;

void SessionStarter::start()
{
    if (m_running) return;
    m_running = true;
    loadState();
    DeveloperLog::record(QStringLiteral("session_starter.started"), {});
    poll();
}

void SessionStarter::stop()
{
    m_running = false;
    ++m_generation;
    m_timer->stop();
}

QDateTime SessionStarter::now() const
{
    return m_options.clock ? m_options.clock().toUTC() : QDateTime::currentDateTimeUtc();
}

QString SessionStarter::folder() const
{
    return m_options.folder.isEmpty() ? AgentSessions::sessionsFolder() : m_options.folder;
}

QString SessionStarter::computer() const
{
    if (!m_options.computerName.isEmpty()) return m_options.computerName;
    const QString name = Platform::computerName();
    return name.isEmpty() ? i18n("this computer") : name;
}

AgentSessions::PhoneStart SessionStarter::choice() const
{
    return m_options.consent ? *m_options.consent : AgentSessions::phoneStartChoice();
}

void SessionStarter::setChoice(AgentSessions::PhoneStart choice)
{
    if (m_options.consent) m_options.consent = choice;
    else AgentSessions::setPhoneStartChoice(choice);
}

// ---------------------------------------------------------------- the pages feed

void SessionStarter::poll()
{
    if (!m_running || m_inFlight) return;
    // Signed in to another server (or again): its sequences are not this cursor's.
    if (m_cursorConnection != ServerAddress::connectionGeneration()) {
        m_cursorConnection = ServerAddress::connectionGeneration();
        m_cursor = -1;
    }
    m_inFlight = true;
    const quint64 generation = m_generation;
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    m_client->pages(m_cursor, PagesWaitSeconds, this, [this, generation, elapsed](const AgentNotes::Client::PagesResult &result) {
        m_inFlight = false;
        if (generation != m_generation) {
            // Stopped meanwhile, perhaps started again.
            if (m_running) m_timer->start(0);
            return;
        }
        pagesAnswered(result, elapsed->elapsed());
    });
}

void SessionStarter::pagesAnswered(const AgentNotes::Client::PagesResult &result, qint64 elapsedMs)
{
    using Failure = AgentNotes::Client::Failure;
    switch (result.failure) {
    case Failure::None: {
        m_failures = 0;
        const bool first = m_cursor < 0;
        m_cursor = qMax(m_cursor, result.cursor);
        // The first answer only says where the newest page is: nothing before it is acted on.
        if (!first) {
            for (const AgentNotes::Note &page : result.notes) consider(page);
        }
        // A server that answers at once although nothing is new is not asked again at once.
        m_timer->start(!first && result.notes.isEmpty() && elapsedMs < 1000 ? 2000 : 0);
        return;
    }
    case Failure::NotConnected:
        // Not signed in: looked at again now and then (signing in makes ensureServer() succeed).
        m_timer->start(30000);
        return;
    case Failure::Unauthorized:
        m_timer->start(60000);
        return;
    case Failure::Unsupported:
        // A server without the pages feed.
        m_timer->start(10 * 60000);
        return;
    case Failure::Unreachable:
    case Failure::Failed:
        break;
    }
    ++m_failures;
    m_timer->start(qMin(120000, 5000 << qMin(m_failures - 1, 5)));
    DeveloperLog::record(QStringLiteral("session_starter.pages_failed"), {{QStringLiteral("failures"), m_failures}}, true);
}

void SessionStarter::consider(const AgentNotes::Note &page)
{
    if (!m_stateLoaded) loadState();
    const PageDecision decision = decidePage(page, now(), m_handled);
    if (!decision.act) {
        if (decision.startSession && !m_handled.contains(page.id)) {
            remember(page.id);
            DeveloperLog::record(QStringLiteral("session_starter.skipped"), {{QStringLiteral("why"), decision.why}});
        }
        return;
    }
    remember(page.id);
    auto job = std::make_shared<Job>();
    job->pageId = page.id;
    job->prompt = page.message.trimmed();
    job->agent = decision.agent;
    DeveloperLog::record(QStringLiteral("session_starter.request"), {{QStringLiteral("agent"), agentName(job->agent)}});
    whenUnclaimed(job, [this, job] { decideConsent(job); });
}

void SessionStarter::whenUnclaimed(const JobPointer &job, std::function<void()> then)
{
    m_client->handling(job->pageId, this, [this, job, then = std::move(then)](const AgentNotes::Client::WriteResult &result) {
        const QJsonObject claim = result.json.value(QLatin1String("claim")).toObject();
        if (result.failure == AgentNotes::Client::Failure::None && !claim.isEmpty()) {
            finish(job, QStringLiteral("taken"), claim.value(QLatin1String("device_id")).toString());
            return;
        }
        // Not known to be taken (also when the server has no handling records): the claim decides.
        then();
    });
}

// ---------------------------------------------------------------- this computer's choice

void SessionStarter::decideConsent(const JobPointer &job)
{
    switch (consentFor(choice())) {
    case Consent::Start:
        launch(job);
        return;
    case Consent::Decline:
        finish(job, QStringLiteral("declined"));
        return;
    case Consent::Ask:
        m_waiting.append(job);
        askNext();
        return;
    }
}

void SessionStarter::askNext()
{
    if (m_asking || m_waiting.isEmpty()) return;
    const JobPointer job = m_waiting.takeFirst();
    m_asking = true;
    QPointer<SessionStarter> guard(this);
    auto answered = std::make_shared<bool>(false);
    auto answer = [guard, job, answered](AskAnswer given) {
        if (!guard || *answered) return;
        *answered = true;
        SessionStarter *self = guard;
        self->m_asking = false;
        const AskOutcome outcome = outcomeOf(given);
        if (outcome.newChoice) self->setChoice(*outcome.newChoice);
        if (outcome.start) {
            // Another computer may have started it while the question waited.
            self->whenUnclaimed(job, [self, job] { self->launch(job); });
        } else {
            self->finish(job, QStringLiteral("declined"));
        }
        // The others are decided again: Always or Never now applies to them too.
        const QList<JobPointer> waiting = std::exchange(self->m_waiting, {});
        for (const JobPointer &next : waiting) self->decideConsent(next);
    };
    const AskRequest request {job->pageId, job->agent, folder()};
    if (m_options.ask) m_options.ask(request, answer);
    else showQuestion(request, answer);
}

void SessionStarter::showQuestion(const AskRequest &request, std::function<void(AskAnswer)> answer)
{
    auto *box = new QMessageBox(QMessageBox::Question, i18n("Start a Study Session?"),
                                i18n("Your phone asks to start a study session on this computer with %1.", agentName(request.agent)), QMessageBox::NoButton);
    box->setObjectName(QStringLiteral("sessionStartQuestion"));
    box->setInformativeText(i18n("It runs in %1 with your own %2 sign-in and the Thoughts tools. Always starts the next ones without asking; "
                                 "Settings › Agent prompts changes this choice.",
                                 QDir::toNativeSeparators(request.folder), agentName(request.agent)));
    QPushButton *start = box->addButton(i18n("Start"), QMessageBox::AcceptRole);
    QPushButton *always = box->addButton(i18n("Always"), QMessageBox::YesRole);
    // Not the escape button: Esc must not turn sessions off for good.
    QPushButton *never = box->addButton(i18n("Not on this computer"), QMessageBox::DestructiveRole);
    start->setObjectName(QStringLiteral("sessionStartOnce"));
    always->setObjectName(QStringLiteral("sessionStartAlways"));
    never->setObjectName(QStringLiteral("sessionStartNever"));
    box->setDefaultButton(start);
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QDialog::finished, box, [box, start, always, never, answer = std::move(answer)] {
        const QAbstractButton *clicked = box->clickedButton();
        answer(clicked == start ? AskAnswer::Start : clicked == always ? AskAnswer::Always : clicked == never ? AskAnswer::NotHere : AskAnswer::Dismissed);
    });
    box->show();
    box->raise();
    box->activateWindow();
    QApplication::alert(box);
}

void SessionStarter::showUntrusted(const QString &folder, const QString &claude)
{
    if (m_options.untrusted) {
        m_options.untrusted(folder, claude);
        return;
    }
    if (m_untrustedBox) {
        m_untrustedBox->raise();
        m_untrustedBox->activateWindow();
        return;
    }
    auto *box = new QMessageBox(QMessageBox::Information, i18n("Trust the Study Sessions Folder"),
                                i18n("Claude Code must trust %1 once: open a terminal there, run claude, accept, then tap Start again.", QDir::toNativeSeparators(folder)),
                                QMessageBox::Close);
    box->setObjectName(QStringLiteral("sessionStartUntrusted"));
    box->setInformativeText(i18n("Open Terminal Here opens a terminal in that folder and starts Claude Code there, which asks whether you trust the folder."));
    QPushButton *open = box->addButton(i18n("Open Terminal Here"), QMessageBox::ActionRole);
    open->setObjectName(QStringLiteral("sessionStartOpenTerminal"));
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::buttonClicked, box, [open, folder, claude](QAbstractButton *clicked) {
        if (clicked == open) openTerminalHere(folder, claude);
    });
    m_untrustedBox = box;
    box->show();
    box->raise();
    box->activateWindow();
    QApplication::alert(box);
}

bool SessionStarter::openTerminalHere(const QString &folder, const QString &claude)
{
    QDir().mkpath(folder);
#if defined(Q_OS_MACOS)
    // A .command file opens in Terminal: it goes to the folder and starts Claude Code there.
    const QString directory = QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("session-starter"));
    QDir().mkpath(directory);
    const QString script = QDir(directory).filePath(QStringLiteral("trust-claude.command"));
    QString text = QStringLiteral("#!/bin/zsh\ncd %1 || exit 1\n").arg(shellQuote(folder));
    if (!claude.isEmpty()) {
        text += QStringLiteral("print -r -- %1\nexec %2\n")
                    .arg(shellQuote(QStringLiteral("Claude Code asks once whether you trust this folder. Accept, then type /exit and tap Start again on your phone.")),
                         shellQuote(claude));
    } else {
        text += QStringLiteral("exec /bin/zsh -l\n");
    }
    writeFile(script, text.toUtf8());
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), script});
#elif defined(Q_OS_WIN)
    QStringList arguments {QStringLiteral("/K")};
    if (!claude.isEmpty()) arguments.append(QDir::toNativeSeparators(claude));
    return Platform::startInNewConsole(QStringLiteral("cmd.exe"), arguments, agentEnvironment(claude), folder);
#else
    Q_UNUSED(claude)
    return QProcess::startDetached(QStringLiteral("x-terminal-emulator"), {}, folder);
#endif
}

// ---------------------------------------------------------------- starting the session

void SessionStarter::launch(const JobPointer &job)
{
    job->startedAt = now().toLocalTime();
    job->name = sessionName(job->startedAt);
    job->folder = folder();
    if (job->prompt.isEmpty()) {
        tellNotStarted(job, QStringLiteral("failed"), i18n("No study session started"),
                       i18n("Thoughts on %1 started no study session: the request from your phone held no prompt.", computer()));
        return;
    }
    if (!QDir().mkpath(job->folder)) {
        tellNotStarted(job, QStringLiteral("failed"), i18n("No study session started"),
                       i18n("Thoughts on %1 could not make the folder %2 for study sessions, so none started there.", computer(), QDir::toNativeSeparators(job->folder)));
        return;
    }
    if (job->agent == Agent::Codex) launchCodex(job);
    else launchClaude(job);
}

void SessionStarter::launchClaude(const JobPointer &job)
{
    if (!m_options.claudeExecutable.isEmpty()) {
        job->executable = isExecutableFile(m_options.claudeExecutable) ? m_options.claudeExecutable : QString();
    } else {
        job->executable = findClaude();
    }
    const auto notInstalled = [this, job] {
        tellNotStarted(job, QStringLiteral("not_installed"), i18n("Claude Code is not on %1", computer()),
                       i18n("Thoughts on %1 could not find Claude Code, so no study session started there. Install Claude Code on %1 (claude.com/claude-code), "
                            "sign in with your Claude subscription by running claude once, then tap Start again.",
                            computer()));
    };
    if (job->executable.isEmpty()) {
        notInstalled();
        return;
    }
    // Signed out: said so plainly (a Claude Code without `auth status` says nothing, and --bg tells).
    run(job->executable, {QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--json")}, job->folder, 30000,
        [this, job, notInstalled](const RunResult &result) {
            if (!result.started) {
                notInstalled();
                return;
            }
            if (claudeLoggedIn(result.output) == false) {
                tellNotStarted(job, QStringLiteral("signed_out"), i18n("Claude Code is signed out on %1", computer()),
                               i18n("Claude Code on %1 is not signed in, so no study session started there. On %1, open a terminal, run claude and sign in "
                                    "with /login (your Claude subscription), then tap Start again.",
                                    computer()));
                return;
            }
            startClaudeSession(job);
        });
}

void SessionStarter::startClaudeSession(const JobPointer &job)
{
    const QString header = promptHeader(Agent::Claude, computer(), job->startedAt, job->pageId);
    QString prompt = header + QStringLiteral("\n\n") + job->prompt;
    // cmd.exe parses a batch file's command line (and ends it at a line break), and Windows command
    // lines are short: the prompt then goes in a file of the folder, and the argument only names it.
    if (isBatchFile(job->executable) || prompt.size() > MaxPromptArgumentCharacters) {
        const QString name = QStringLiteral("%1-%2.md").arg(job->startedAt.toString(QStringLiteral("yyyyMMdd-HHmmss")), job->pageId.left(8));
        const QString directory = QDir(job->folder).filePath(QStringLiteral("prompts"));
        QDir().mkpath(directory);
        writeFile(QDir(directory).filePath(name), prompt.toUtf8());
        prompt = header + QStringLiteral(" The prompt is in the file prompts/%1 of this folder: read all of it and follow it.").arg(name);
    }
    run(job->executable, claudeArguments(job->name, prompt), job->folder, m_options.processTimeoutMs, [this, job](const RunResult &result) {
        const QString output = QString::fromUtf8(result.output + '\n' + result.errors).trimmed();
        const auto started = parseBackgrounded(output);
        if (result.started && !result.timedOut && result.exitCode == 0 && started) {
            job->shortId = started->shortId;
            DeveloperLog::record(QStringLiteral("session_starter.claude_started"), {{QStringLiteral("short_id"), job->shortId}});
            findClaudeSession(job, 0);
            return;
        }
        if (!result.started) {
            tellNotStarted(job, QStringLiteral("not_installed"), i18n("Claude Code did not run on %1", computer()),
                           i18n("Thoughts on %1 could not run Claude Code (%2), so no study session started there.", computer(), QDir::toNativeSeparators(job->executable)));
            return;
        }
        if (result.timedOut) {
            tellNotStarted(job, QStringLiteral("failed"), i18n("No study session started on %1", computer()),
                           i18n("Claude Code on %1 did not start the session within %2 seconds, so none started there.", computer(), m_options.processTimeoutMs / 1000));
            return;
        }
        switch (classifyClaudeFailure(output)) {
        case ClaudeFailure::Untrusted:
            tellNotStarted(job, QStringLiteral("untrusted"), i18n("Claude Code must trust a folder on %1", computer()),
                           i18n("No study session started: Claude Code on %1 does not trust %2 yet. On %1, open a terminal in that folder, run claude and accept "
                                "(Thoughts there shows a button for it), then tap Start again.",
                                computer(), QDir::toNativeSeparators(job->folder)));
            showUntrusted(job->folder, job->executable);
            return;
        case ClaudeFailure::SignedOut:
            tellNotStarted(job, QStringLiteral("signed_out"), i18n("Claude Code is signed out on %1", computer()),
                           i18n("Claude Code on %1 could not start the session because it is not signed in. On %1, run claude and sign in with /login, then tap Start again. "
                                "It said: %2",
                                computer(), output.right(300)));
            return;
        case ClaudeFailure::TooOld:
            tellNotStarted(job, QStringLiteral("failed"), i18n("Claude Code is too old on %1", computer()),
                           i18n("Claude Code at %2 on %1 cannot start background sessions. Update it (claude update), then tap Start again.", computer(),
                                QDir::toNativeSeparators(job->executable)));
            return;
        case ClaudeFailure::Other:
            break;
        }
        tellNotStarted(job, QStringLiteral("failed"), i18n("No study session started on %1", computer()),
                       i18n("Claude Code on %1 could not start the session: %2", computer(), output.isEmpty() ? i18n("it said nothing (exit code %1)", result.exitCode) : output.right(300)));
    });
}

void SessionStarter::findClaudeSession(const JobPointer &job, int attempt)
{
    run(job->executable, {QStringLiteral("agents"), QStringLiteral("--json")}, job->folder, 30000, [this, job, attempt](const RunResult &result) {
        job->sessionId = sessionIdFromAgents(result.output, job->shortId);
        if (!job->sessionId.isEmpty() || attempt + 1 >= m_options.agentsAttempts) {
            claim(job);
            return;
        }
        QTimer::singleShot(m_options.agentsIntervalMs, this, [this, job, attempt] { findClaudeSession(job, attempt + 1); });
    });
}

void SessionStarter::launchCodex(const JobPointer &job)
{
    job->executable = !m_options.codexExecutable.isEmpty() ? (isExecutableFile(m_options.codexExecutable) ? m_options.codexExecutable : QString())
                                                           : CommentCodexLauncher::findCodexExecutable();
    const auto notInstalled = [this, job] {
        tellNotStarted(job, QStringLiteral("not_installed"), i18n("Codex is not on %1", computer()),
                       i18n("Thoughts on %1 could not find Codex, so no study session started there. Install Codex on %1, sign in with codex login, then tap Start again.",
                            computer()));
    };
    if (job->executable.isEmpty()) {
        notInstalled();
        return;
    }
    run(job->executable, {QStringLiteral("login"), QStringLiteral("status")}, job->folder, 30000, [this, job, notInstalled](const RunResult &result) {
        if (!result.started) {
            notInstalled();
            return;
        }
        if (result.timedOut || result.exitCode != 0) {
            tellNotStarted(job, QStringLiteral("signed_out"), i18n("Codex is signed out on %1", computer()),
                           i18n("Codex on %1 is not signed in, so no study session started there. On %1, run codex login, then tap Start again.", computer()));
            return;
        }
        startCodexSession(job);
    });
}

void SessionStarter::startCodexSession(const JobPointer &job)
{
    // Its prompt, events and errors are files of the run's own folder: the prompt goes in on
    // standard input, so no command line (or cmd.exe, for npm's codex.cmd) holds the owner's text.
    job->runDirectory = QDir(job->folder).filePath(QStringLiteral("codex/%1-%2").arg(job->startedAt.toString(QStringLiteral("yyyyMMdd-HHmmss")), job->pageId.left(8)));
    QDir().mkpath(job->runDirectory);
    const QString promptPath = QDir(job->runDirectory).filePath(QStringLiteral("prompt.md"));
    job->outputPath = QDir(job->runDirectory).filePath(QStringLiteral("output.jsonl"));
    job->errorsPath = QDir(job->runDirectory).filePath(QStringLiteral("errors.log"));
    writeFile(promptPath, (promptHeader(Agent::Codex, computer(), job->startedAt, job->pageId) + QStringLiteral("\n\n") + job->prompt).toUtf8());
    QProcess process;
    process.setProgram(job->executable);
    process.setArguments(codexArguments(job->folder));
    process.setWorkingDirectory(job->folder);
    process.setProcessEnvironment(agentEnvironment(job->executable));
    process.setStandardInputFile(promptPath);
    process.setStandardOutputFile(job->outputPath);
    process.setStandardErrorFile(job->errorsPath);
    if (!process.startDetached(&job->pid)) {
        tellNotStarted(job, QStringLiteral("failed"), i18n("Codex did not run on %1", computer()),
                       i18n("Thoughts on %1 could not run Codex (%2), so no study session started there.", computer(), QDir::toNativeSeparators(job->executable)));
        return;
    }
    DeveloperLog::record(QStringLiteral("session_starter.codex_started"), {});
    waitForCodexThread(job, 0);
}

void SessionStarter::waitForCodexThread(const JobPointer &job, int waitedMs)
{
    QFile output(job->outputPath);
    if (output.open(QIODevice::ReadOnly)) job->sessionId = codexThreadId(output.read(1 << 20));
    if (!job->sessionId.isEmpty()) {
        claim(job);
        return;
    }
    if (!Platform::processIsRunning(QString::number(job->pid))) {
        // Read once more: it may have named its thread just before it ended.
        if (output.isOpen()) {
            output.seek(0);
            job->sessionId = codexThreadId(output.read(1 << 20));
        }
        if (job->sessionId.isEmpty()) {
            QFile errors(job->errorsPath);
            const QByteArray said = errors.open(QIODevice::ReadOnly) ? errors.readAll() : QByteArray();
            tellNotStarted(job, QStringLiteral("failed"), i18n("No study session started on %1", computer()),
                           i18n("Codex on %1 stopped before it started the session: %2", computer(), said.trimmed().isEmpty() ? i18n("it said nothing") : tail(said)));
            return;
        }
        claim(job);
        return;
    }
    if (waitedMs >= m_options.codexThreadWaitMs) {
        // Running without a thread id yet: claimed under a name of its own.
        claim(job);
        return;
    }
    QTimer::singleShot(250, this, [this, job, waitedMs] { waitForCodexThread(job, waitedMs + 250); });
}

// ---------------------------------------------------------------- claiming and telling the owner

void SessionStarter::claim(const JobPointer &job)
{
    QString conversation = job->sessionId;
    if (conversation.isEmpty()) {
        conversation = job->agent == Agent::Codex ? QString(QStringLiteral("codex:") + QUuid::createUuid().toString(QUuid::WithoutBraces))
                                                  : QString(QStringLiteral("claude:") + job->shortId);
    }
    m_client->post(QStringLiteral("/v1/agent-notes/handling/%1/claim").arg(QString::fromLatin1(QUrl::toPercentEncoding(job->pageId))),
                   {{QStringLiteral("conversation_id"), conversation}}, this, [this, job](const AgentNotes::Client::WriteResult &result) {
                       if (result.status == 409) {
                           // Another computer (or agent) took the page first: its session answers.
                           withdraw(job);
                           finish(job, QStringLiteral("taken"), result.json.value(QLatin1String("claim")).toObject().value(QLatin1String("device_id")).toString());
                           return;
                       }
                       if (result.failure != AgentNotes::Client::Failure::None) {
                           // Kept: the session runs, and nothing says another computer has the page.
                           DeveloperLog::record(QStringLiteral("session_starter.claim_failed"), {{QStringLiteral("status"), result.status}});
                       }
                       tellStarted(job);
                   });
}

void SessionStarter::withdraw(const JobPointer &job)
{
    if (job->agent == Agent::Codex) {
        if (job->pid <= 0) return;
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("taskkill"), {QStringLiteral("/PID"), QString::number(job->pid), QStringLiteral("/T"), QStringLiteral("/F")});
#else
        ::kill(pid_t(job->pid), SIGTERM);
#endif
        return;
    }
    if (job->shortId.isEmpty()) return;
    const QString executable = job->executable;
    const QString shortId = job->shortId;
    const QString directory = job->folder;
    run(executable, {QStringLiteral("stop"), shortId}, directory, 30000, [this, executable, shortId, directory](const RunResult &) {
        run(executable, {QStringLiteral("rm"), shortId}, directory, 30000, [](const RunResult &) {});
    });
}

void SessionStarter::tellStarted(const JobPointer &job)
{
    const QString time = QLocale::c().toString(job->startedAt, QStringLiteral("HH:mm"));
    const QString where = QDir::toNativeSeparators(job->folder);
    QString message;
    if (job->agent == Agent::Codex) {
        message = i18n("“%1” started on %2 at %3 with Codex (codex exec, your own sign-in), in %4. It answers your prompt with notes, but it is not kept "
                       "listening for replies yet: replies to its notes go to whatever answers replies that no session takes. Its output: %5.",
                       job->name, computer(), time, where, QDir::toNativeSeparators(job->outputPath));
        if (!job->sessionId.isEmpty()) message += QLatin1Char(' ') + i18n("To continue it on %1: codex resume %2", computer(), job->sessionId);
    } else {
        message = i18n("“%1” started on %2 at %3 with Claude Code (your own sign-in), in %4. It works from your prompt and answers with notes. "
                       "To see it on %2: claude attach %5",
                       job->name, computer(), time, where, job->shortId);
    }
    QJsonObject origin {{QStringLiteral("agent"), QStringLiteral("thoughts-desktop")},
                        {QStringLiteral("kind"), QStringLiteral("session_started")},
                        {QStringLiteral("machine"), computer()},
                        {QStringLiteral("item_id"), job->pageId},
                        {QStringLiteral("session_agent"), job->agent == Agent::Codex ? QStringLiteral("codex") : QStringLiteral("claude")},
                        {QStringLiteral("conversation_id"), job->sessionId.isEmpty() ? QJsonValue() : QJsonValue(job->sessionId)}};
    sendNote(job, i18n("Study session started"), message, origin, QStringLiteral("started"));
}

void SessionStarter::tellNotStarted(const JobPointer &job, const QString &outcome, const QString &title, const QString &message)
{
    // responder: no tool takes this note for anybody's answer (as the reply waker's notices).
    const QJsonObject origin {{QStringLiteral("agent"), QStringLiteral("thoughts-desktop")},
                              {QStringLiteral("kind"), QStringLiteral("session_not_started")},
                              {QStringLiteral("machine"), computer()},
                              {QStringLiteral("item_id"), job->pageId},
                              {QStringLiteral("reason"), outcome},
                              {QStringLiteral("responder"), true}};
    sendNote(job, title, message, origin, outcome);
}

void SessionStarter::sendNote(const JobPointer &job, const QString &title, const QString &message, const QJsonObject &origin, const QString &outcome)
{
    const QString text = message.left(AgentNotes::MaxMessageCharacters);
    const QJsonObject body {{QStringLiteral("note_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                            {QStringLiteral("title"), title.left(200)},
                            {QStringLiteral("message"), text},
                            {QStringLiteral("image"),
                             QJsonObject {{QStringLiteral("media_type"), QStringLiteral("image/png")},
                                          {QStringLiteral("data_base64"), QString::fromLatin1(noticeImage(title, text).toBase64())}}},
                            {QStringLiteral("origin"), origin}};
    m_client->post(QStringLiteral("/v1/agent-notes"), body, this, [this, job, outcome](const AgentNotes::Client::WriteResult &result) {
        finish(job, outcome, result.failure == AgentNotes::Client::Failure::None ? QString() : QStringLiteral("note not sent: %1").arg(result.status));
    });
}

void SessionStarter::finish(const JobPointer &job, const QString &outcome, const QString &detail)
{
    QJsonObject metadata {{QStringLiteral("outcome"), outcome}, {QStringLiteral("agent"), agentName(job->agent)}};
    if (!detail.isEmpty()) metadata.insert(QStringLiteral("detail"), detail.left(200));
    DeveloperLog::record(QStringLiteral("session_starter.finished"), metadata);
    Q_EMIT pageFinished(job->pageId, outcome);
}

// ---------------------------------------------------------------- processes

void SessionStarter::run(const QString &program, const QStringList &arguments, const QString &directory, int timeoutMs, std::function<void(const RunResult &)> done)
{
    struct State {
        RunResult result;
        std::function<void(const RunResult &)> done;
        bool fired = false;
    };
    auto *process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(arguments);
    if (!directory.isEmpty()) process->setWorkingDirectory(directory);
    process->setProcessEnvironment(agentEnvironment(program));
    process->setStandardInputFile(QProcess::nullDevice());
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    // Once: the timeout, a failure to start and the end can each come first.
    const auto complete = [process, state] {
        if (state->fired) return;
        state->fired = true;
        state->result.output += process->readAllStandardOutput();
        state->result.errors += process->readAllStandardError();
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
        state->result.started = false;
        complete();
    });
    connect(timer, &QTimer::timeout, this, [process, state, complete] {
        state->result.started = true;
        state->result.timedOut = true;
        process->kill();
        complete();
    });
    timer->start(timeoutMs);
    process->start();
}

// ---------------------------------------------------------------- handled pages

void SessionStarter::remember(const QString &pageId)
{
    if (pageId.isEmpty() || m_handled.contains(pageId)) return;
    m_handled.insert(pageId);
    m_handledOrder.append(pageId);
    while (m_handledOrder.size() > HandledLimit) m_handled.remove(m_handledOrder.takeFirst());
    saveState();
}

void SessionStarter::loadState()
{
    m_stateLoaded = true;
    const QString path = m_options.statePath.isEmpty() ? QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("session-starter/state.json")) : m_options.statePath;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("handled")).toArray()) {
        const QString id = value.toString();
        if (id.isEmpty() || m_handled.contains(id)) continue;
        m_handled.insert(id);
        m_handledOrder.append(id);
    }
}

void SessionStarter::saveState() const
{
    const QString path = m_options.statePath.isEmpty() ? QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("session-starter/state.json")) : m_options.statePath;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QJsonObject state {{QStringLiteral("version"), 1}, {QStringLiteral("handled"), QJsonArray::fromStringList(m_handledOrder)}};
    writeFile(path, QJsonDocument(state).toJson(QJsonDocument::Compact));
}

}
