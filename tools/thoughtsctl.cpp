/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell/thoughtscommandprotocol.h"
#include "shell/proposalauthoring.h"
#include "shell/reviewknowledge.h"
#include "shell/reviewtagplan.h"
#include "part/pdfversions.h"
#include "part/thoughtshome.h"
#include "part/thoughtsplatform.h"
#include "part/thoughtsserveraddress.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

namespace
{
constexpr int connectTimeoutMs = 10000;
constexpr int requestTimeoutMs = 10000;
constexpr int defaultWaitSeconds = 30 * 60;

bool connectSocket(QLocalSocket *socket)
{
    socket->connectToServer(Thoughts::syncCommandSocketPath(), QIODevice::ReadWrite);
    return socket->waitForConnected(500);
}

bool launchThoughts()
{
#ifdef Q_OS_MACOS
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                   {QStringLiteral("-g"), QStringLiteral("-j"), QStringLiteral("-b"), QStringLiteral("org.tbend.thoughts")});
#elif defined(Q_OS_WIN)
    // Installed side by side; PATH usually lacks the Thoughts folder.
    return QProcess::startDetached(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("thoughts.exe")), {QStringLiteral("--noraise")});
#else
    return QProcess::startDetached(QStringLiteral("thoughts"), {QStringLiteral("--noraise")});
#endif
}

QJsonObject sendRequest(const QJsonObject &request, bool launchIfNeeded, QString *error, QString *errorCode = nullptr)
{
    QLocalSocket socket;
    if (!connectSocket(&socket)) {
        if (!launchIfNeeded || !launchThoughts()) {
            *error = QStringLiteral("Thoughts is not running and could not be launched.");
            return {};
        }
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < connectTimeoutMs) {
            QThread::msleep(100);
            socket.abort();
            if (connectSocket(&socket)) {
                break;
            }
        }
    }
    if (socket.state() != QLocalSocket::ConnectedState) {
        *error = QStringLiteral("Thoughts command service did not become available.");
        if (errorCode) *errorCode = QStringLiteral("transport_error");
        return {};
    }
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(requestTimeoutMs)) {
        *error = QStringLiteral("Could not send the command to Thoughts.");
        if (errorCode) *errorCode = QStringLiteral("transport_error");
        return {};
    }
    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (!response.contains('\n') && timer.elapsed() < requestTimeoutMs) {
        if (!socket.waitForReadyRead(qMax(1, requestTimeoutMs - int(timer.elapsed())))) {
            break;
        }
        response += socket.readAll();
    }
    const QJsonDocument document = QJsonDocument::fromJson(response.left(response.indexOf('\n')));
    if (!document.isObject()) {
        *error = response.contains('\n') ? QStringLiteral("Thoughts returned an invalid command response.") : QStringLiteral("Thoughts did not answer in time.");
        if (errorCode) *errorCode = response.contains('\n') ? QStringLiteral("transport_error") : QStringLiteral("timeout");
        return {};
    }
    return document.object();
}

QJsonObject waitForTerminal(QJsonObject result, int timeoutSeconds, QString *error, bool launchIfNeeded = true)
{
    const QString operationId = result.value(QStringLiteral("operation_id")).toString();
    if (operationId.isEmpty()) {
        *error = QStringLiteral("Thoughts accepted the command without an operation ID.");
        return {};
    }
    QElapsedTimer timer;
    timer.start();
    while (!result.value(QStringLiteral("terminal")).toBool()) {
        if (timer.elapsed() >= timeoutSeconds * 1000LL) {
            result.insert(QStringLiteral("client_timeout"), true);
            result.insert(QStringLiteral("message"), QStringLiteral("Timed out waiting; synchronization continues in Thoughts."));
            return result;
        }
        QThread::sleep(2);
        result = sendRequest(Thoughts::syncCommandStatusRequest(operationId),
                             launchIfNeeded,
                             error);
        if (result.isEmpty()) {
            return {};
        }
    }
    return result;
}

// Proposal commands Sync before writing, so the agent edits current source,
// and schedule a Sync afterwards so the proposal reaches the other devices.
// A burst of proposals shares both: the Sync before is skipped when one
// succeeded recently, and one background delivery runs once proposals stop
// arriving. THOUGHTS_PROPOSAL_SYNC=0 turns this off for isolated tests.
// Both gate on the sources only: a failed PDF build after the sources synced
// is a warning, since the proposal may be what fixes the build.
constexpr qint64 recentSyncMs = 60 * 1000;
constexpr qint64 deliveryQuietMs = 10 * 1000;

QString proposalSyncStatePath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("proposal-sync.json"));
}

QJsonObject readProposalSyncState()
{
    QFile file(proposalSyncStatePath());
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void recordProposalSyncTime(const QString &key, qint64 time = QDateTime::currentMSecsSinceEpoch())
{
    QLockFile lock(proposalSyncStatePath() + QStringLiteral(".lock"));
    if (!lock.tryLock(5000)) return;
    auto state = readProposalSyncState();
    state.insert(key, QJsonValue(time));
    QSaveFile file(proposalSyncStatePath());
    const QByteArray data = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) file.commit();
}

// Requests a full Sync, waiting out another active Sync or build first.
QJsonObject requestFullSync(bool launchIfNeeded, int timeoutSeconds, QString *error)
{
    const QJsonObject request {{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("sync")}};
    QElapsedTimer timer;
    timer.start();
    while (true) {
        const auto result = sendRequest(request, launchIfNeeded, error);
        if (result.value(QStringLiteral("error_code")).toString() != QLatin1String("busy") || timer.elapsed() >= timeoutSeconds * 1000LL) return result;
        QThread::sleep(2);
    }
}

// The server capabilities the app last read (FedoraRepository keeps them in its
// settings, those of its KAboutData identity: domain kde.org, application thoughts).
QStringList appServerCapabilities()
{
    const QSettings settings(QStringLiteral("kde.org"), QStringLiteral("thoughts"));
    return settings.value(QStringLiteral("thoughts-server/capabilities")).toStringList();
}

// move-card-passage: the card, from the review index Thoughts maintains.
Thoughts::CardMoveContext cardMoveContext(const QString &root, const QString &cardId)
{
    Thoughts::CardMoveContext context;
    context.movePairsSupported = appServerCapabilities().contains(QStringLiteral("review_card_move_pairs_v1"));
    const auto refuse = [&context](const QString &code, const QString &message) {
        context.refusal = code;
        context.refusalMessage = message;
        return context;
    };
    Thoughts::ReviewKnowledge::IndexSnapshot snapshot;
    QString error, code;
    if (!Thoughts::ReviewKnowledge::readIndex(Thoughts::ReviewKnowledge::indexPath(), &snapshot, &error, &code))
        return refuse(code.isEmpty() ? QStringLiteral("index_unavailable") : code, error);
    const auto library = Thoughts::ReviewKnowledge::libraryFromSnapshot(snapshot, root);
    const auto card = std::find_if(library.cards.cbegin(), library.cards.cend(), [&cardId](const Thoughts::ReviewCard &c) { return c.id() == cardId; });
    if (card == library.cards.cend() || !card->isApproved())
        return refuse(QStringLiteral("card_unavailable"), QStringLiteral("This card is unavailable or already deleted."));
    if (card->isWholeDocument())
        return refuse(QStringLiteral("unsupported_card"), QStringLiteral("This card's answer is a whole document, not a marked passage."));
    QSet<QString> files;
    const auto segments = card->record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    for (const auto &segment : segments) files.insert(segment.toObject().value(QStringLiteral("source_file")).toString());
    if (files.isEmpty())
        return refuse(QStringLiteral("pdf_only_card"), QStringLiteral("This card is not linked to source markers: move it with start_line and end_line selecting its new answer."));
    if (files.size() != 1 || files.contains(QString()))
        return refuse(QStringLiteral("unsupported_card"), QStringLiteral("This card's markers are in several sources; move its passages by hand."));
    context.sourceFile = *files.cbegin();
    context.question = card->question();
    return context;
}

QJsonObject authorProposalWithSync(const QString &root, const QJsonObject &request, int timeoutSeconds, QString *error)
{
    // The MCP server marks its calls, so a withdrawal is recorded as the agent's (§10.4.2).
    const QString origin = qEnvironmentVariable("THOUGHTS_REQUEST_ORIGIN") == QLatin1String("mcp") ? QStringLiteral("mcp") : QStringLiteral("thoughtsctl");
    const bool move = request.value(QStringLiteral("command")).toString() == QLatin1String("move-card-passage");
    if (qEnvironmentVariable("THOUGHTS_PROPOSAL_SYNC") == QLatin1String("0")) {
        return move ? Thoughts::authorCardMove(root, request, origin, cardMoveContext(root, request.value(QStringLiteral("card_id")).toString()))
                    : Thoughts::authorProposal(root, request, origin);
    }
    QJsonObject before;
    QString syncError;
    if (QDateTime::currentMSecsSinceEpoch() - readProposalSyncState().value(QStringLiteral("last_success_ms")).toInteger() >= recentSyncMs) {
        before = requestFullSync(true, timeoutSeconds, &syncError);
        if (before.value(QStringLiteral("accepted")).toBool() && !before.value(QStringLiteral("terminal")).toBool()) {
            before = waitForTerminal(before, timeoutSeconds, &syncError);
        }
        if (before.isEmpty() && syncError.isEmpty()) {
            syncError = QStringLiteral("Thoughts returned no Sync result.");
        }
        if (Thoughts::syncCommandSourcesSynced(before)) {
            recordProposalSyncTime(QStringLiteral("last_success_ms"));
        }
    }
    // The card is looked up after the Sync, which may have brought its markers.
    const Thoughts::CardMoveContext card = move ? cardMoveContext(root, request.value(QStringLiteral("card_id")).toString()) : Thoughts::CardMoveContext{};
    auto result = Thoughts::authorProposalAfterSync(root, request, origin, before, syncError, move ? &card : nullptr);
    if (result.isEmpty()) {
        *error = QStringLiteral("The proposal command returned no result.");
        return {};
    }
    if (!result.value(QStringLiteral("accepted")).toBool()) {
        return result;
    }
    recordProposalSyncTime(QStringLiteral("last_write_ms"));
    // Detach from our output too, or callers reading it until EOF would wait for delivery.
    QProcess delivery;
    delivery.setProgram(QCoreApplication::applicationFilePath());
    delivery.setArguments({QStringLiteral("deliver-proposals")});
    delivery.setStandardInputFile(QProcess::nullDevice());
    delivery.setStandardOutputFile(QProcess::nullDevice());
    delivery.setStandardErrorFile(QProcess::nullDevice());
    Thoughts::Platform::keepStandardStreamsPrivate();
    if (delivery.startDetached()) {
        result.insert(QStringLiteral("sync_after"), QStringLiteral("scheduled"));
        // Drop the authoring message's "not synchronized" clause; delivery is scheduled.
        result.insert(QStringLiteral("message"), QString(result.value(QStringLiteral("message")).toString().section(QLatin1Char(';'), 0, 0)
            + QStringLiteral(". A Sync will deliver it about %1 seconds after the last proposal.").arg(deliveryQuietMs / 1000)));
    } else {
        result.insert(QStringLiteral("sync_after"), QStringLiteral("not_started"));
    }
    return result;
}

// Background delivery: once no proposal has been written for a short while,
// run one Sync, and repeat if more proposals arrived during it. It never
// launches Thoughts; without a running app the next normal Sync delivers them.
int deliverProposals()
{
    QLockFile lock(proposalSyncStatePath() + QStringLiteral(".deliver"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(int(deliveryQuietMs))) return 0;
    for (int round = 0; round < 20; ++round) {
        const auto state = readProposalSyncState();
        if (state.value(QStringLiteral("delivered_through_ms")).toInteger() > state.value(QStringLiteral("last_write_ms")).toInteger()) return 0;
        for (qint64 quiet = 0; (quiet = QDateTime::currentMSecsSinceEpoch() - readProposalSyncState().value(QStringLiteral("last_write_ms")).toInteger()) < deliveryQuietMs;) {
            QThread::msleep(quint64(deliveryQuietMs - quiet));
        }
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        QString error;
        auto result = requestFullSync(false, defaultWaitSeconds, &error);
        if (!result.value(QStringLiteral("accepted")).toBool()) return 1;
        const bool reused = result.value(QStringLiteral("reused")).toBool();
        if (!result.value(QStringLiteral("terminal")).toBool()) result = waitForTerminal(result, defaultWaitSeconds, &error, false);
        // A reused Sync may have started before the latest proposal; run another.
        if (reused) continue;
        // A failed PDF build still delivered the proposals with the sources.
        if (!Thoughts::syncCommandSourcesSynced(result)) return 1;
        recordProposalSyncTime(QStringLiteral("last_success_ms"));
        recordProposalSyncTime(QStringLiteral("delivered_through_ms"), started);
    }
    return 1;
}

// One JSON object of at most 2 MiB from stdin, without a command field; empty input is {}.
bool readRequest(QJsonObject *request, bool allowEmpty)
{
    QFile input;
    input.open(stdin, QIODevice::ReadOnly);
    const QByteArray bytes = input.read(2 * 1024 * 1024 + 1);
    if (allowEmpty && bytes.trimmed().isEmpty()) {
        *request = {};
        return true;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (bytes.size() > 2 * 1024 * 1024 || parse.error != QJsonParseError::NoError || !document.isObject() || document.object().contains(QStringLiteral("command"))) return false;
    *request = document.object();
    return true;
}

void printResult(const QJsonObject &result, bool json)
{
    QTextStream output(stdout);
    if (json) {
        output << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return;
    }
    QString line = result.value(QStringLiteral("state")).toString();
    if (!line.isEmpty()) {
        line[0] = line.at(0).toUpper();
    }
    line += QStringLiteral(": ") + result.value(QStringLiteral("document")).toString(result.value(QStringLiteral("target")).toString(QStringLiteral("all documents")));
    if (result.contains(QStringLiteral("page"))) {
        line += QStringLiteral(" — page %1").arg(result.value(QStringLiteral("page")).toInt());
    }
    const QString operationId = result.value(QStringLiteral("operation_id")).toString();
    if (!operationId.isEmpty()) {
        line += QStringLiteral(" (") + operationId + QLatin1Char(')');
    }
    const QString message = result.value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) {
        line += QStringLiteral(" — ") + message;
    }
    // What a Sync that succeeded could not do, e.g. a PDF the server could not build.
    const QString warning = result.value(QStringLiteral("warning")).toString().simplified();
    if (!warning.isEmpty()) {
        line += QStringLiteral(" — ") + warning;
    }
    output << line << Qt::endl;
}
}

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    // Requests and results are JSON: pass bytes through unchanged instead of
    // translating line ends and stopping at Ctrl+Z.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("thoughtsctl"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Open PDFs, manage pending proposals, and trigger or inspect Thoughts synchronization."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("open, sync, status, create-proposal, create-proposals, revise-proposal, remove-proposal, move-card-passage, create-review-card, list-review-cards, delete-review-card, set-review-card-interval, move-review-card, merge-review-cards, create-project, import-document, remove-project, open-agent-notes, delete-comment, list-todos, add-todo, edit-todo, decide-proposal, resolve-source-conflict, link-conversation, search-review-cards, knowledge-profile, knowledge-levels, review-tags or store-secret. "
                                                                           "decide-proposal accepts or rejects one whole proposal for an editor integration acting on the user's click; agents never decide proposals. resolve-source-conflict settles a recorded source conflict like the Resolve dialog: path plus keep (fedora or local) or content_file (absolute path of a finished merge). "
                                                                           "open-agent-notes brings Agent Notes to the front with one document's or project's notes: {\"document\": id, Library filename, PDF path or name} or {\"project\": id or name}, and/or {\"note_id\"} to select a note. "
                                                                           "search-review-cards and knowledge-profile only read the review index and the Library, with or without Thoughts running. "
                                                                           "list-document-feedback lists the likes, dislikes, questions, drawings and text boxes given while reading, with the text of their pages on the PDF version they were given on (fetched from the server when not here; comments are left out). "
                                                                           "knowledge-levels prints the tag-to-knowledge-level mapping; with --init it writes the default mapping to a file you can edit. "
                                                                           "review-tags plan FILE shows what a tag reorganization (merges and the group layout) would change, read-only; "
                                                                           "review-tags apply FILE has Thoughts record it, as Compact tags and the tag picker do."));
    parser.addPositionalArgument(QStringLiteral("operation-id"), QStringLiteral("PDF path/Library filename for open; operation ID for status; https address for set-server"), QStringLiteral("[operation-id]"));
    const QCommandLineOption targetOption(QStringLiteral("target"), QStringLiteral("Exact Library PDF filename."), QStringLiteral("filename"));
    const QCommandLineOption waitOption(QStringLiteral("wait"), QStringLiteral("Wait for terminal success or failure."));
    const QCommandLineOption timeoutOption(QStringLiteral("timeout"), QStringLiteral("Wait timeout in seconds."), QStringLiteral("seconds"), QString::number(defaultWaitSeconds));
    const QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("Write one machine-readable JSON result."));
    const QCommandLineOption initOption(QStringLiteral("init"), QStringLiteral("For knowledge-levels: write the default mapping to its file (never replaces one)."));
    const QCommandLineOption pageOption(QStringLiteral("page"), QStringLiteral("One-based PDF page for open."), QStringLiteral("number"), QStringLiteral("1"));
    const QCommandLineOption serviceOption(QStringLiteral("service"), QStringLiteral("Secret store service for store-secret."), QStringLiteral("name"), QStringLiteral("thoughts-server"));
    const QCommandLineOption accountOption(QStringLiteral("account"), QStringLiteral("Secret store account for store-secret, e.g. thoughts-windows or thoughts-source-sync."), QStringLiteral("name"));
    parser.addOption(pageOption);
    parser.addOption(serviceOption);
    parser.addOption(accountOption);
    parser.addOption(targetOption);
    parser.addOption(waitOption);
    parser.addOption(timeoutOption);
    parser.addOption(jsonOption);
    parser.addOption(initOption);
    parser.process(application);

    QTextStream errors(stderr);
    const QStringList positional = parser.positionalArguments();
    const QString command = positional.value(0);
    if (command == QLatin1String("deliver-proposals")) {
        return deliverProposals();
    }
    if (command == QLatin1String("store-secret")) {
        // Reads the secret from the first line of stdin so it never appears
        // in a command line or shell history.
        const QString account = parser.value(accountOption);
        if (positional.size() != 1 || account.isEmpty()) {
            errors << "error: store-secret takes --account NAME [--service NAME] and the secret on stdin\n";
            return 2;
        }
        QFile input;
        input.open(stdin, QIODevice::ReadOnly);
        const QByteArray secret = Thoughts::Platform::cleanSecret(input.readLine(64 * 1024));
        if (secret.isEmpty()) {
            errors << "error: no secret on stdin\n";
            return 2;
        }
        QString error;
        if (!Thoughts::Platform::writeSecret(parser.value(serviceOption), account, secret, &error)) {
            errors << "error: " << error << '\n';
            return 3;
        }
        QTextStream output(stdout);
        output << "Stored " << parser.value(serviceOption) << '/' << account << '\n';
        if (parser.value(serviceOption) == Thoughts::ServerAddress::tokenService()) {
            // A server token belongs to the server that issued it: the one
            // Thoughts is set to use now. It is sent nowhere else.
            const QString address = Thoughts::ServerAddress::configured();
            if (Thoughts::ServerAddress::bindToken(account, secret, address, &error)) {
                output << "It is the token for " << Thoughts::ServerAddress::origin(address) << " and is sent only there.\n";
            } else {
                errors << "warning: " << error << " Set the address with thoughtsctl set-server, then store the token again.\n";
            }
        }
        return 0;
    }
    if (command == QLatin1String("set-server")) {
        // Saves the server address Thoughts uses with this computer's device
        // token, as Server… in the app does. Thoughts reads it at startup.
        if (positional.size() != 2) {
            errors << "error: set-server takes the server address, for example " << Thoughts::ServerAddress::defaultUrl() << '\n';
            return 2;
        }
        QString error;
        if (!Thoughts::ServerAddress::save(positional.at(1), &error)) {
            errors << "error: " << error << '\n';
            return 2;
        }
        const QString saved = Thoughts::ServerAddress::saved();
        QTextStream output(stdout);
        output << "Saved server address " << saved << '\n';
        // The tokens stored for the previous server are not sent to this one.
        for (const QString &account : {Thoughts::Platform::desktopDeviceAccount(), QStringLiteral("thoughts-source-sync")}) {
            const QString issuedFor = Thoughts::ServerAddress::tokenOrigin(account);
            if (!issuedFor.isEmpty() && issuedFor != Thoughts::ServerAddress::origin(saved)) {
                output << Thoughts::ServerAddress::tokenWithheldMessage(account, issuedFor, saved) << '\n';
            }
        }
        return 0;
    }
    if (command == QLatin1String("list-document-feedback")) {
        // Read-only, like search-review-cards: the records come from the review index. The PDF
        // versions they were given on come from this computer (the reader's cache, a Library
        // generation) or from the server, as History opens them, into a folder of this run only, so
        // the reader's cache of versions keeps what it has.
        if (positional.size() != 1 || parser.isSet(pageOption) || parser.isSet(targetOption) || parser.isSet(waitOption)
            || parser.isSet(timeoutOption) || parser.isSet(initOption)) {
            errors << "error: this command takes [--json] and one JSON request from stdin\n";
            return 2;
        }
        QJsonObject request;
        if (!readRequest(&request, true)) {
            printResult(Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Provide a JSON object of at most 2 MiB without a command field.")), parser.isSet(jsonOption));
            return 2;
        }
        Thoughts::ReviewKnowledge::Library library;
        QJsonObject result;
        if (Thoughts::ReviewKnowledge::readLibrary(Thoughts::ReviewKnowledge::indexPath(), Thoughts::thoughtsHome(), &library, &result)) {
            QTemporaryDir downloads;
            Thoughts::PdfVersions::Fetcher fetcher(Thoughts::thoughtsHome(), downloads.path());
            const auto versionPath = [&](const QString &sha256, QString *unavailable) -> QString {
                const QString cached = Thoughts::PdfVersions::cachePath(Thoughts::PdfVersions::defaultCacheRoot(), sha256);
                if (QFileInfo(cached).isFile()) return cached;
                if (!downloads.isValid()) {
                    *unavailable = QStringLiteral("No temporary folder for the download.");
                    return {};
                }
                QEventLoop loop;
                Thoughts::PdfVersions::Result fetched{{}, Thoughts::PdfVersions::Failure::Unreachable, QStringLiteral("The server did not answer within 2 minutes.")};
                QTimer::singleShot(120000, &loop, &QEventLoop::quit);
                fetcher.fetch(sha256, &loop, [&](const Thoughts::PdfVersions::Result &done) {
                    fetched = done;
                    loop.quit();
                });
                loop.exec();
                using Failure = Thoughts::PdfVersions::Failure;
                switch (fetched.failure) {
                case Failure::None: break;
                case Failure::NotConnected: *unavailable = QStringLiteral("This computer has no server address or device token."); break;
                case Failure::NotFound: *unavailable = QStringLiteral("The server no longer has this version."); break;
                default: *unavailable = fetched.detail.isEmpty() ? QStringLiteral("The version could not be fetched.") : fetched.detail; break;
                }
                return fetched.path;
            };
            result = Thoughts::ReviewKnowledge::documentFeedback(library, request, versionPath);
        }
        if (parser.isSet(jsonOption)) printResult(result, true);
        else QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        if (result.value(QStringLiteral("accepted")).toBool()) return 0;
        const int failed = Thoughts::syncCommandExitCode(result);
        return failed ? failed : 1;
    }
    // Read-only: answered here from the review index, so they work without Thoughts running.
    if (command == QLatin1String("search-review-cards") || command == QLatin1String("knowledge-profile")) {
        if (positional.size() != 1 || parser.isSet(pageOption) || parser.isSet(targetOption) || parser.isSet(waitOption)
            || parser.isSet(timeoutOption) || parser.isSet(initOption)) {
            errors << "error: this command takes [--json] and one JSON request from stdin\n";
            return 2;
        }
        QJsonObject request;
        if (!readRequest(&request, command == QLatin1String("search-review-cards"))) {
            printResult(Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Provide a JSON object of at most 2 MiB without a command field.")), parser.isSet(jsonOption));
            return 2;
        }
        const auto result = Thoughts::ReviewKnowledge::run(command, request, Thoughts::ReviewKnowledge::indexPath(), Thoughts::thoughtsHome(),
                                                           Thoughts::ReviewKnowledge::levelsPath());
        if (parser.isSet(jsonOption)) {
            printResult(result, true);
        } else if (command == QLatin1String("knowledge-profile") && result.value(QStringLiteral("accepted")).toBool()) {
            QTextStream(stdout) << result.value(QStringLiteral("summary")).toString() << Qt::endl;
        } else {
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        }
        if (result.value(QStringLiteral("accepted")).toBool()) return 0;
        const int failed = Thoughts::syncCommandExitCode(result);
        return failed ? failed : 1;
    }
    if (command == QLatin1String("review-tags")) {
        // plan reads the review index, like knowledge-profile; apply has the running app write,
        // so its in-memory review state and the outbox see the merges and the layout at once.
        const QString mode = positional.value(1);
        if (positional.size() != 3 || (mode != QLatin1String("plan") && mode != QLatin1String("apply")) || parser.isSet(pageOption)
            || parser.isSet(targetOption) || parser.isSet(waitOption) || parser.isSet(timeoutOption) || parser.isSet(initOption)) {
            errors << "error: usage: thoughtsctl review-tags plan|apply PLAN.json [--json]\n";
            return 2;
        }
        QFile file(positional.at(2));
        QJsonParseError parse;
        const QJsonDocument document = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(2 * 1024 * 1024), &parse) : QJsonDocument();
        Thoughts::ReviewTagPlan::Plan plan;
        QString error;
        QJsonObject result;
        if (!file.isOpen()) {
            result = Thoughts::ReviewKnowledge::error(QStringLiteral("invalid_request"), QStringLiteral("%1 could not be read.").arg(positional.at(2)));
        } else if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            result = Thoughts::ReviewKnowledge::error(QStringLiteral("invalid_plan"), QStringLiteral("%1 is not a JSON object: %2").arg(positional.at(2), parse.errorString()));
        } else if (!Thoughts::ReviewTagPlan::parse(document.object(), &plan, &error)) {
            result = Thoughts::ReviewKnowledge::error(QStringLiteral("invalid_plan"), error);
        } else if (mode == QLatin1String("plan")) {
            Thoughts::ReviewKnowledge::IndexSnapshot snapshot;
            QString code;
            if (!Thoughts::ReviewKnowledge::readIndex(Thoughts::ReviewKnowledge::indexPath(), &snapshot, &error, &code)) {
                result = Thoughts::ReviewKnowledge::error(code, error);
            } else {
                const auto library = Thoughts::ReviewKnowledge::libraryFromSnapshot(snapshot, Thoughts::thoughtsHome());
                result = Thoughts::ReviewTagPlan::evaluate(plan, library.cards, library.merges, library.groups);
                result.insert(QStringLiteral("indexed_at"), library.indexedAt);
                if (!library.tagRecords) result.insert(QStringLiteral("message"), QStringLiteral("This review index has no tag merges or groups; open Thoughts once to refresh it."));
            }
        } else {
            QJsonObject request{{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("review-tags-apply")},
                                {QStringLiteral("plan"), document.object()}};
            result = sendRequest(request, true, &error);
            if (result.isEmpty()) {
                errors << "error: " << error << '\n';
                return 3;
            }
        }
        if (result.value(QStringLiteral("accepted")).toBool() && !plan.levels.isEmpty()) {
            // The knowledge levels are this computer's own file, not synced: thoughtsctl updates it.
            const QString path = Thoughts::ReviewKnowledge::levelsPath();
            QStringList mergedAway;
            for (const auto &merge : plan.merges) mergedAway += merge.from;
            QString levelsNote;
            if (!QFileInfo::exists(path)) {
                levelsNote = QStringLiteral("%1 does not exist; the built-in default mapping applies.").arg(path);
            } else if (mode == QLatin1String("plan")) {
                levelsNote = QStringLiteral("apply backs up %1 and sets the plan's %2 levels in it (entries for merged tags are dropped).").arg(path).arg(plan.levels.size());
            } else {
                QString backup, message;
                levelsNote = Thoughts::ReviewKnowledge::updateLevels(path, plan.levels, mergedAway, &backup, &message)
                    ? QStringLiteral("updated %1 (previous version: %2).").arg(path, backup)
                    : QStringLiteral("NOT updated: %1").arg(message);
            }
            result.insert(QStringLiteral("knowledge_levels_file"), levelsNote);
        }
        if (parser.isSet(jsonOption)) printResult(result, true);
        else QTextStream(stdout) << Thoughts::ReviewTagPlan::formatReport(result);
        return result.value(QStringLiteral("accepted")).toBool() ? 0 : 1;
    }
    if (command == QLatin1String("knowledge-levels")) {
        if (positional.size() != 1 || parser.isSet(pageOption) || parser.isSet(targetOption) || parser.isSet(waitOption) || parser.isSet(timeoutOption)) {
            errors << "error: usage: thoughtsctl knowledge-levels [--init] [--json]\n";
            return 2;
        }
        const QString path = Thoughts::ReviewKnowledge::levelsPath();
        QString error;
        QJsonObject result;
        Thoughts::ReviewKnowledge::Levels levels;
        if (parser.isSet(initOption) && !Thoughts::ReviewKnowledge::initLevels(path, &error)) {
            result = Thoughts::ReviewKnowledge::error(QStringLiteral("levels_exist"), error);
        } else if (!Thoughts::ReviewKnowledge::loadLevels(path, &levels, &error)) {
            result = Thoughts::ReviewKnowledge::error(QStringLiteral("invalid_levels"), error);
        } else {
            result = Thoughts::ReviewKnowledge::levelsDocument(levels);
            result.insert(QStringLiteral("accepted"), true);
            result.insert(QStringLiteral("source"), levels.source);
            result.insert(QStringLiteral("path"), path);
            if (parser.isSet(initOption)) result.insert(QStringLiteral("message"), QStringLiteral("Wrote the default mapping; edit it to change the levels."));
        }
        if (parser.isSet(jsonOption)) printResult(result, true);
        else QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        return result.value(QStringLiteral("accepted")).toBool() ? 0 : 1;
    }
    // Card commands, Library project commands, to-do commands, delete-comment, link-conversation and open-agent-notes go straight to the running app, without the proposal Sync.
    const bool cardCommand = command == QLatin1String("create-review-card") || command == QLatin1String("list-review-cards") || command == QLatin1String("delete-review-card") || command == QLatin1String("set-review-card-interval")
        || command == QLatin1String("move-review-card") || command == QLatin1String("merge-review-cards") || command == QLatin1String("delete-comment") || command == QLatin1String("decide-proposal")
        || command == QLatin1String("resolve-source-conflict")
        || command == QLatin1String("list-todos") || command == QLatin1String("add-todo") || command == QLatin1String("edit-todo")
        || command == QLatin1String("link-conversation") || command == QLatin1String("create-project") || command == QLatin1String("import-document")
        || command == QLatin1String("remove-project") || command == QLatin1String("open-agent-notes");
    if (cardCommand || command == QLatin1String("create-proposal") || command == QLatin1String("create-proposals") || command == QLatin1String("revise-proposal") || command == QLatin1String("remove-proposal")
        || command == QLatin1String("move-card-passage")) {
        bool proposalTimeoutOk = false;
        const int proposalTimeout = parser.value(timeoutOption).toInt(&proposalTimeoutOk);
        if (positional.size() != 1 || parser.isSet(pageOption) || parser.isSet(targetOption) || parser.isSet(waitOption)
            || (cardCommand && parser.isSet(timeoutOption)) || !proposalTimeoutOk || proposalTimeout <= 0) {
            errors << "error: this command takes [--json] (and for proposals [--timeout SECONDS]) and one JSON request from stdin\n";
            return 2;
        }
        QFile input;
        input.open(stdin, QIODevice::ReadOnly);
        const QByteArray bytes = input.read(2 * 1024 * 1024 + 1);
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        if (bytes.size() > 2 * 1024 * 1024 || parse.error != QJsonParseError::NoError || !document.isObject() || document.object().contains(QStringLiteral("command"))) {
            printResult(Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("Provide a JSON object of at most 2 MiB without a command field.")), parser.isSet(jsonOption));
            return 2;
        }
        auto request = document.object();
        request.insert(QStringLiteral("command"), command);
        const QString root = Thoughts::thoughtsHome();
        QString error;
        if (cardCommand) request.insert(QStringLiteral("schema_version"), 1);
        QString errorCode;
        // An automatic conversation link (linked_by tool) never launches Thoughts: the MCP adds it
        // after tools such as send_note that work without the app.
        const bool launch = command != QLatin1String("link-conversation") || request.value(QStringLiteral("linked_by")).toString() == QLatin1String("agent");
        const auto result = cardCommand ? sendRequest(request, launch, &error, &errorCode) : authorProposalWithSync(root, request, proposalTimeout, &error);
        if (result.isEmpty() && command == QLatin1String("decide-proposal")) {
            // Retryable with the same request_id: the app may or may not have decided.
            QJsonObject failure = Thoughts::syncCommandError(errorCode.isEmpty() ? QStringLiteral("transport_error") : errorCode, error);
            failure.insert(QStringLiteral("retryable"), true);
            printResult(failure, parser.isSet(jsonOption));
            return 3;
        }
        if (result.isEmpty()) { errors << "error: " << error << '\n'; return 3; }
        printResult(result, parser.isSet(jsonOption));
        return Thoughts::syncCommandExitCode(result);
    }
    bool timeoutOk = false;
    const int timeoutSeconds = parser.value(timeoutOption).toInt(&timeoutOk);
    bool pageOk = false;
    const int page = parser.value(pageOption).toInt(&pageOk);
    if ((command != QLatin1String("open") && command != QLatin1String("sync") && command != QLatin1String("status"))
        || !timeoutOk || timeoutSeconds <= 0
        || (command == QLatin1String("sync") && positional.size() != 1)
        || (command == QLatin1String("status") && positional.size() != 2)
        || (command == QLatin1String("open") && (positional.size() != 2 || !pageOk || page < 1 || parser.isSet(targetOption) || parser.isSet(waitOption)))
        || (command != QLatin1String("open") && parser.isSet(pageOption))) {
        errors << "error: usage: thoughtsctl <open PDF [--page N]|sync [--target NAME.pdf]|status OPERATION_ID> [--wait] [--timeout SECONDS] [--json]\n";
        return 2;
    }

    QJsonObject request {{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), command}};
    if (command == QLatin1String("open")) {
        request.insert(QStringLiteral("document"), positional.at(1));
        request.insert(QStringLiteral("page"), page);
    } else if (command == QLatin1String("sync")) {
        const QString target = parser.value(targetOption);
        if (!Thoughts::validSyncCommandTarget(target)) {
            errors << "error: --target must be an exact PDF filename\n";
            return 2;
        }
        if (!target.isEmpty()) {
            request.insert(QStringLiteral("target"), target);
        }
    } else {
        request = Thoughts::syncCommandStatusRequest(positional.at(1));
    }

    QString error;
    QJsonObject result = sendRequest(request, true, &error);
    if (result.isEmpty()) {
        errors << "error: " << error << '\n';
        return 3;
    }
    if (parser.isSet(waitOption) && result.value(QStringLiteral("accepted")).toBool()
        && !result.value(QStringLiteral("terminal")).toBool()) {
        result = waitForTerminal(result, timeoutSeconds, &error);
        if (result.isEmpty()) {
            errors << "error: " << error << '\n';
            return 3;
        }
    }
    printResult(result, parser.isSet(jsonOption));
    return Thoughts::syncCommandExitCode(result);
}
