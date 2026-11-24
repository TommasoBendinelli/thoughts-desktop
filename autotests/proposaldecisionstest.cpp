/* SPDX-License-Identifier: GPL-2.0-or-later */
// Decisions and removed proposals (protocol/agent-proposals.md §10) on the
// desktop: decision claims (write-ahead with the source, attach, expire, undo
// drop, conflict carry), thoughtsctl decide-proposal, and Restore Proposal
// against autotests/data/agent-proposals/decisions.json (a copy of the
// server's protocol/fixtures/agent-proposals/decisions.json). Also the
// publisher's frozen source that its own merge cannot join and Fedora's
// proposal-aware merge does.

#include "../part/desktopsyncstore.h"
#include "../part/projectconfiguration.h"
#include "../part/proposalhistory.h"
#include "../part/sourceowner.h"
#include "../part/thoughtsplatform.h"
#include "../shell/agentproposalparser.h"
#include "../shell/proposalauthoring.h"
#include "../shell/proposaldecisions.h"
#include "../tools/publisher/managedsource.h"
#include "../tools/publisher/sourceapi.h"
#include "../tools/publisher/sourcepublisher.h"
#include "../tools/publisher/sourcereconciliation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace Thoughts;

namespace
{
// The publisher pauses 1, 2 and 4 s before it sends an unanswered request again; these
// tests do not wait that long.
const bool shortRetryPauses = (Thoughts::Publisher::SourceApi::setRetryDelays({1, 2, 4}), true);
}

namespace
{
const QString Relative = QStringLiteral("docs-x/tex/source/a.tex");
const QString Wrapper = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{agent-1}{Clarify.\n}{Old line.\n}{New line.\n}");
const QString Base = QStringLiteral("Intro.\n") + Wrapper + QStringLiteral("Outro.\n");
const QString FixtureWrapperSha256 = QStringLiteral("252bfd0570e87e771344faa0a2033f6d46454250eaf6f11d2ea7e8644f76dd8e");
// Another agent proposal, around the line after the first one.
const QString SecondWrapper = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:05:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{agent-2}{Shorten.\n}{Outro.\n}{End.\n}");

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject fixtures()
{
    QFile file(QStringLiteral(KDESRCDIR "data/agent-proposals/decisions.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject removedRow(const QJsonObject &snapshot, int line, const QString &sourceFile = Relative)
{
    QJsonObject stored = snapshot;
    stored.insert(QStringLiteral("source_file"), sourceFile);
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("proposal_id"), snapshot.value(QStringLiteral("proposal_id"))},
            {QStringLiteral("status"), QStringLiteral("removed_outside")}, {QStringLiteral("kind"), QStringLiteral("removed_outside")},
            {QStringLiteral("snapshot"), stored}, {QStringLiteral("source_file"), sourceFile}, {QStringLiteral("wrapper_sha256"), FixtureWrapperSha256},
            {QStringLiteral("removal"), QJsonObject{{QStringLiteral("source_line"), line}, {QStringLiteral("looks_like"), QStringLiteral("accept")},
                                                    {QStringLiteral("removed_at"), QStringLiteral("2026-10-11T09:00:00.000Z")}}},
            {QStringLiteral("revision"), 4}, {QStringLiteral("deleted"), false}};
}

// A minimal Fedora source API for the publisher: sources, objects, health,
// atomic source publications and single source applies. Every publication and
// apply request body is kept.
class FakeSourceServer : public QObject
{
public:
    FakeSourceServer()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    bool capable = true;
    bool healthFails = false;
    int failNextPublications = 0;
    QList<QJsonObject> publications;
    QHash<QString, QJsonObject> heads;
    int frontier = 0;
    // POST /v1/sources/apply: Fedora's merge result, committed as the next
    // revision and answered "merged"; an operation seen before is answered
    // "duplicate" with the current head, as Fedora answers from its record.
    QByteArray applyResult;
    // Answers without the result's identity.
    bool malformedApplyAnswer = false;
    // Commits, then answers 504 as a proxy that lost Fedora's answer.
    int loseNextApplyAnswers = 0;
    // Runs while Fedora merges, before it answers.
    std::function<void()> whileApplying;
    QList<QJsonObject> applies;

    // A new revision of `file` written on Fedora (another device or an agent).
    QJsonObject commit(const QString &file, const QByteArray &content)
    {
        const QString digest = sha(content);
        m_objects.insert(digest, content);
        const QJsonObject head{{QStringLiteral("path"), file}, {QStringLiteral("revision"), heads.value(file).value(QStringLiteral("revision")).toInt() + 1},
                               {QStringLiteral("sha256"), digest}, {QStringLiteral("object_sha256"), digest}, {QStringLiteral("deleted"), false}};
        heads.insert(file, head);
        ++frontier;
        return head;
    }

private:
    void handle(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        qsizetype length = 0;
        for (const QByteArray &line : lines) {
            if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < headerEnd + 4 + length) return;
        const QByteArray body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        const QByteArray method = requestLine.value(0);
        const QString path = QString::fromLatin1(requestLine.value(1));
        int status = 200;
        QByteArray response;
        if (method == "GET" && path == QLatin1String("/v1/health") && healthFails) {
            status = 503;
            response = "{\"detail\":\"unavailable\"}";
        } else if (method == "GET" && path == QLatin1String("/v1/health")) {
            QJsonArray capabilities{QStringLiteral("agent_proposals_v1")};
            if (capable) capabilities.append(QStringLiteral("proposal_decisions_v1"));
            response = QJsonDocument(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), capabilities}}).toJson(QJsonDocument::Compact);
        } else if (method == "GET" && path == QLatin1String("/v1/sources")) {
            QJsonArray sources;
            for (const QJsonObject &head : std::as_const(heads)) sources.append(head);
            response = QJsonDocument(QJsonObject{{QStringLiteral("source_frontier"), frontier}, {QStringLiteral("sources"), sources}}).toJson(QJsonDocument::Compact);
        } else if (method == "POST" && path == QLatin1String("/v1/objects")) {
            m_objects.insert(sha(body), body);
            response = QJsonDocument(QJsonObject{{QStringLiteral("sha256"), sha(body)}}).toJson(QJsonDocument::Compact);
        } else if ((method == "GET" || method == "HEAD") && path.startsWith(QLatin1String("/v1/objects/"))) {
            const QString digest = path.mid(12);
            status = m_objects.contains(digest) ? 200 : 404;
            if (method == "GET") response = m_objects.value(digest);
        } else if (method == "POST" && path == QLatin1String("/v1/sources/apply")) {
            const QJsonObject mutation = QJsonDocument::fromJson(body).object();
            applies.append(mutation);
            const QString file = mutation.value(QStringLiteral("path")).toString();
            const QString operation = mutation.value(QStringLiteral("operation_id")).toString();
            QJsonObject result;
            if (m_appliedOperations.contains(operation)) {
                result = heads.value(file);
                result.remove(QStringLiteral("deleted"));
                result.insert(QStringLiteral("outcome"), QStringLiteral("duplicate"));
            } else {
                m_appliedOperations.insert(operation);
                result = commit(file, applyResult);
                result.remove(QStringLiteral("deleted"));
                result.insert(QStringLiteral("outcome"), QStringLiteral("merged"));
                if (malformedApplyAnswer) {
                    result.remove(QStringLiteral("sha256"));
                    result.remove(QStringLiteral("object_sha256"));
                }
            }
            result.insert(QStringLiteral("server_sequence"), frontier);
            response = QJsonDocument(result).toJson(QJsonDocument::Compact);
            if (whileApplying) whileApplying();
            if (loseNextApplyAnswers > 0) {
                --loseNextApplyAnswers;
                status = 504;
                response = "{\"detail\":\"gateway timeout\"}";
            }
        } else if (method == "POST" && path == QLatin1String("/v1/source-publications")) {
            const QJsonObject publication = QJsonDocument::fromJson(body).object();
            publications.append(publication);
            if (failNextPublications > 0) {
                --failNextPublications;
                status = 503;
                response = "{\"detail\":\"temporarily unavailable\"}";
            } else {
                QJsonArray results;
                for (const QJsonValue &value : publication.value(QStringLiteral("mutations")).toArray()) {
                    const QJsonObject mutation = value.toObject();
                    const QString file = mutation.value(QStringLiteral("path")).toString();
                    const int revision = heads.value(file).value(QStringLiteral("revision")).toInt() + 1;
                    const QString digest = mutation.value(QStringLiteral("content_sha256")).toString(sha({}));
                    const bool deleted = mutation.value(QStringLiteral("action")).toString() == QLatin1String("delete");
                    heads.insert(file, {{QStringLiteral("path"), file}, {QStringLiteral("revision"), revision}, {QStringLiteral("sha256"), digest},
                                        {QStringLiteral("object_sha256"), digest}, {QStringLiteral("deleted"), deleted}});
                    results.append(QJsonObject{{QStringLiteral("outcome"), QStringLiteral("applied")}, {QStringLiteral("path"), file}, {QStringLiteral("revision"), revision},
                                               {QStringLiteral("sha256"), digest}, {QStringLiteral("object_sha256"), digest}});
                }
                ++frontier;
                QJsonArray decisions;
                for (const QJsonValue &value : publication.value(QStringLiteral("proposal_decisions")).toArray()) {
                    const QJsonObject claim = value.toObject();
                    decisions.append(QJsonObject{{QStringLiteral("decision_id"), claim.value(QStringLiteral("decision_id"))},
                                                 {QStringLiteral("proposal_id"), claim.value(QStringLiteral("proposal_id"))}, {QStringLiteral("outcome"), QStringLiteral("matched")}});
                }
                QJsonObject result{{QStringLiteral("publication_id"), publication.value(QStringLiteral("publication_id"))},
                                   {QStringLiteral("source_frontier"), frontier}, {QStringLiteral("results"), results}};
                if (capable) result.insert(QStringLiteral("proposal_decisions"), decisions);
                response = QJsonDocument(result).toJson(QJsonDocument::Compact);
            }
        } else {
            status = 404;
            response = "{\"detail\":\"not found\"}";
        }
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Error") + "\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(method == "HEAD" ? 0 : response.size()) + "\r\nConnection: close\r\n\r\n";
        if (method != "HEAD") reply += response;
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QString, QByteArray> m_objects;
    QSet<QString> m_appliedOperations;
};

// A Project in `root` whose connected folder is `owner`.
bool connectFolder(const QString &root, const QString &owner)
{
    QJsonObject project = ProjectConfiguration::empty(root, QStringLiteral("project"), QStringLiteral("Project"));
    QJsonArray folders = project.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), owner},
                               {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    project.insert(QStringLiteral("source_folders"), folders);
    return ProjectConfiguration::save(ProjectConfiguration::path(root, QStringLiteral("project")), project).isEmpty();
}

// A Library on disk with one LaTeX source, the publisher's state and the sync database.
struct Workspace {
    QTemporaryDir directory;
    QString relative = Relative;
    QString proposalId = QStringLiteral("agent-1");
    QString root() const { return QFileInfo(directory.filePath(QStringLiteral("docs"))).canonicalFilePath(); }
    QString source() const { return QDir(root()).filePath(relative); }
    QString stateRoot() const { return directory.filePath(QStringLiteral("state")); }
    QString database() const { return QDir(stateRoot()).filePath(QStringLiteral("desktop-sync.sqlite")); }
    QString manifest() const { return directory.filePath(QStringLiteral("source-manifest.json")); }
    bool prepare(const QString &text = Base)
    {
        QDir().mkpath(directory.filePath(QStringLiteral("docs")));
        QDir().mkpath(stateRoot());
        // A Project connects the source's folder, so Sync manages it and decisions get claims.
        if (!connectFolder(root(), SourceOwner::of(relative))) return false;
        const QJsonObject manifestPayload{{QStringLiteral("schema_version"), 3},
                                          {QStringLiteral("projects"), QJsonArray{QJsonObject{{QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("path"), relative}}}}}}}};
        return write(directory.filePath(QStringLiteral("docs/")) + relative, text.toUtf8()) && write(manifest(), QJsonDocument(manifestPayload).toJson());
    }
    // One publishOnce; false when it raised.
    bool publish(FakeSourceServer &server, QString *failure = nullptr, QStringList *messages = nullptr)
    {
        return publishTo(server.url(), QStringLiteral("token"), failure, messages);
    }
    // One publishOnce of the source as Desktop freezes it when Sync starts:
    // its bytes now, against the base the publisher's state records.
    // `desktopBase`: the base Desktop's own record names, when it has not
    // caught up with the publisher's state.
    bool publishFrozen(FakeSourceServer &server, QString *failure = nullptr, QStringList *messages = nullptr, const QString &desktopBase = {})
    {
        const QByteArray content = read(source());
        Publisher::FrozenEntry entry;
        entry.path = relative;
        entry.action = QStringLiteral("upsert");
        entry.sha256 = sha(content);
        entry.sizeBytes = content.size();
        entry.content = content;
        entry.baseSha256 = state().value(QStringLiteral("bases")).toObject().value(relative).toObject().value(QStringLiteral("sha256")).toString(Publisher::emptySha256);
        if (!desktopBase.isEmpty()) entry.baseSha256 = desktopBase;
        Publisher::FrozenInputs frozen;
        frozen.sourceRoot = Py::Path(root());
        frozen.sources.append(entry);
        return publishTo(server.url(), QStringLiteral("token"), failure, messages, &frozen);
    }
    // The editable-source installer of a Sync, through Fedora's frontier.
    bool install(FakeSourceServer &server, QString *failure = nullptr)
    {
        Publisher::SourceApi api(server.url(), QStringLiteral("token"), 30);
        try {
            Publisher::SourcePublisher publisher(Py::Path(root()), Py::Path(manifest()),
                                                 Py::Path(QDir(stateRoot()).filePath(QStringLiteral("source-publication.json"))), api);
            Publisher::reconcileAuthoringSources(publisher, server.frontier);
            return true;
        } catch (const Py::Exception &error) {
            if (failure) *failure = error.line();
            return false;
        }
    }
    bool publishTo(const QString &url, const QString &token, QString *failure = nullptr, QStringList *messages = nullptr,
                   const Publisher::FrozenInputs *frozen = nullptr)
    {
        Publisher::SourceApi api(url, token, 30);
        try {
            Publisher::SourcePublisher publisher(Py::Path(root()), Py::Path(manifest()),
                                                 Py::Path(QDir(stateRoot()).filePath(QStringLiteral("source-publication.json"))), api, std::nullopt,
                                                 std::nullopt, std::nullopt, frozen, [messages](const QString &message, const Publisher::ProgressFields &) {
                                                     if (messages) messages->append(message);
                                                 });
            publisher.publishOnce();
            return true;
        } catch (const Py::Exception &error) {
            if (failure) *failure = error.line();
            return false;
        }
    }
    QJsonObject state() const
    {
        return QJsonDocument::fromJson(read(QDir(stateRoot()).filePath(QStringLiteral("source-publication.json")))).object();
    }
    bool decide(const QString &action, const QString &origin = QStringLiteral("desktop"))
    {
        AgentProposalDecisionOptions options;
        options.origin = origin;
        options.claimDatabase = database();
        QString error;
        const bool decided = resolveLiveAgentProposedEdits(root(), source(), sha(read(source())), {proposalId}, action, &error, nullptr, nullptr, nullptr, options);
        if (!decided) qWarning() << error;
        return decided;
    }
};
}

class ProposalDecisionsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testWrapperSha256();
    void testAttachFixtures();
    void testRestoreFixtures();
    void testClaimStore();
    void testLocalDecisionRecordsClaimWithSource();
    void testClaimIsForgottenWhenSourceWriteFails();
    void testWithdrawalRecordsClaim();
    void testUnmanagedSourcesRecordNoClaim();
    void testPublicationDropsClaimsNoUploadCanCarry();
    void testSyncDropsClaimsWithoutThePublisher();
    void testClaimDatabaseStaysInTheTestHome();
    void testPublicationAttachesAndExpiresClaims();
    void testUndoDropsClaimAtUpload();
    void testOlderServerExpiresClaimsWithoutSendingThem();
    void testRetriedPublicationSendsTheSameClaims();
    void testClaimsWaitWhenTheServerCannotBeAsked();
    void testConflictResolutionCarriesClaims();
    void testFedoraMergeOfAConflictingSourceIsInstalled();
    void testSourceSentAloneCarriesItsClaims();
    void testFolderAddedOnAnotherDeviceInstalls();
    void testUnansweredSourceApplyIsRepeated();
    void testEditDuringFedoraMergeKeepsItsBase();
    void testFrozenCopyOnDesktopsEarlierBase_data();
    void testFrozenCopyOnDesktopsEarlierBase();
    void testDecisionOnFedorasMergeBeforeDesktopRecordedIt_data();
    void testDecisionOnFedorasMergeBeforeDesktopRecordedIt();
    void testClaimsAgainstServer();
    void testDecideProposalCommand();
    void testRestoreRemovedProposal();
    void testDismissPayload();
};

void ProposalDecisionsTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    const QJsonObject data = fixtures();
    QCOMPARE(data.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(data.value(QStringLiteral("attach")).toArray().size(), 4);
    QCOMPARE(data.value(QStringLiteral("restore")).toArray().size(), 27);
}

void ProposalDecisionsTest::testWrapperSha256()
{
    const AgentProposalParse parse = parseAgentProposals(Relative, Base);
    QCOMPARE(parse.proposals.size(), 1);
    QCOMPARE(ProposalDecisions::wrapperSha256(Base, parse.proposals.first()), FixtureWrapperSha256);
    // Every fixture claim names the wrapper of its scenario's text.
    const QJsonObject claim = fixtures().value(QStringLiteral("attach")).toArray().first().toObject().value(QStringLiteral("claims")).toArray().first().toObject();
    QCOMPARE(claim.value(QStringLiteral("wrapper_sha256")).toString(), FixtureWrapperSha256);
    QVERIFY(ProposalDecisions::isValidClaim(claim));
}

void ProposalDecisionsTest::testAttachFixtures()
{
    for (const QJsonValue &value : fixtures().value(QStringLiteral("attach")).toArray()) {
        const QJsonObject test = value.toObject();
        QList<QJsonObject> claims;
        for (const QJsonValue &claim : test.value(QStringLiteral("claims")).toArray()) claims.append(claim.toObject());
        QHash<QString, std::optional<QString>> uploads;
        const QJsonObject upload = test.value(QStringLiteral("upload")).toObject();
        for (auto it = upload.begin(); it != upload.end(); ++it) uploads.insert(it.key(), it.value().isNull() ? std::nullopt : std::optional<QString>(it.value().toString()));
        const ProposalDecisions::ClaimSelection selection = ProposalDecisions::selectClaimsForUpload(claims, uploads);
        QStringList expectedSent, expectedDropped;
        for (const QJsonValue &id : test.value(QStringLiteral("expect")).toObject().value(QStringLiteral("sent")).toArray()) expectedSent.append(id.toString());
        for (const QJsonValue &id : test.value(QStringLiteral("expect")).toObject().value(QStringLiteral("dropped")).toArray()) expectedDropped.append(id.toString());
        const QByteArray name = test.value(QStringLiteral("id")).toString().toUtf8();
        QVERIFY2(ProposalDecisions::decisionIds(selection.sent) == expectedSent, name.constData());
        QVERIFY2(selection.dropped == expectedDropped, name.constData());
    }
    // A deletion carries its claims; one proposal travels once.
    QJsonObject claim = ProposalDecisions::newClaim(QStringLiteral("agent-1"), QStringLiteral("accept"), FixtureWrapperSha256, Relative, QStringLiteral("desktop"));
    QJsonObject again = ProposalDecisions::newClaim(QStringLiteral("agent-1"), QStringLiteral("reject"), FixtureWrapperSha256, Relative, QStringLiteral("desktop"));
    const auto selection = ProposalDecisions::selectClaimsForUpload({claim, again}, {{Relative, std::nullopt}});
    QCOMPARE(selection.sent.size(), 1);
    QCOMPARE(selection.sent.first(), claim);
}

void ProposalDecisionsTest::testRestoreFixtures()
{
    for (const QJsonValue &value : fixtures().value(QStringLiteral("restore")).toArray()) {
        const QJsonObject test = value.toObject();
        const QByteArray name = test.value(QStringLiteral("id")).toString().toUtf8();
        const QJsonValue source = test.value(QStringLiteral("source"));
        const ProposalDecisions::RestoreEdit edit = ProposalDecisions::restoreEdit(test.value(QStringLiteral("path")).toString(),
            source.isNull() ? std::nullopt : std::optional<QString>(source.toString()), test.value(QStringLiteral("snapshot")).toObject(), test.value(QStringLiteral("line")).toInt());
        const QJsonObject expect = test.value(QStringLiteral("expect")).toObject();
        if (expect.contains(QStringLiteral("output"))) {
            QVERIFY2(edit.code.isEmpty(), QByteArray(name + " refused " + edit.code.toUtf8()).constData());
            QVERIFY2(edit.output == expect.value(QStringLiteral("output")).toString(), QByteArray(name + "\n" + edit.output.toUtf8()).constData());
        } else if (expect.value(QStringLiteral("already_present")).toBool()) {
            QVERIFY2(edit.code == QLatin1String("already_present"), QByteArray(name + " " + edit.code.toUtf8()).constData());
        } else {
            QVERIFY2(edit.code == expect.value(QStringLiteral("refused")).toString(), QByteArray(name + " " + edit.code.toUtf8()).constData());
            QVERIFY(edit.output.isEmpty());
        }
    }
}

void ProposalDecisionsTest::testClaimStore()
{
    QTemporaryDir directory;
    const QString database = directory.filePath(QStringLiteral("state/desktop-sync.sqlite"));
    QString error;
    // Nothing recorded: no database is created for reading.
    QVERIFY(ProposalDecisions::storedClaims(database, &error).isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo::exists(database));
    QVERIFY(ProposalDecisions::forgetClaims(database, {QStringLiteral("x")}, &error));

    const QJsonObject first = ProposalDecisions::newClaim(QStringLiteral("agent-1"), QStringLiteral("accept"), FixtureWrapperSha256, Relative, QStringLiteral("desktop"));
    const QJsonObject other = ProposalDecisions::newClaim(QStringLiteral("agent-2"), QStringLiteral("withdraw"), FixtureWrapperSha256, QStringLiteral("docs-x/b.md"), QStringLiteral("mcp"));
    QVERIFY(ProposalDecisions::recordClaims(database, {first, other}, &error));
    QCOMPARE(ProposalDecisions::storedClaims(database), (QList<QJsonObject>{first, other}));
    QCOMPARE(ProposalDecisions::storedClaims(database, nullptr, {Relative}), QList<QJsonObject>{first});
    // Deciding the same proposal again replaces the earlier (undone) claim.
    const QJsonObject second = ProposalDecisions::newClaim(QStringLiteral("agent-1"), QStringLiteral("reject"), FixtureWrapperSha256, Relative, QStringLiteral("vscode"));
    QVERIFY(ProposalDecisions::recordClaims(database, {second}, &error));
    QCOMPARE(ProposalDecisions::storedClaims(database), (QList<QJsonObject>{other, second}));
    QVERIFY(ProposalDecisions::forgetClaims(database, {other.value(QStringLiteral("decision_id")).toString()}, &error));
    QCOMPARE(ProposalDecisions::storedClaims(database), QList<QJsonObject>{second});
    // Invalid claims are refused whole.
    QJsonObject invalid = first;
    invalid.insert(QStringLiteral("action"), QStringLiteral("maybe"));
    QVERIFY(!ProposalDecisions::recordClaims(database, {invalid}, &error));
    // The table lives next to the app's own tables, which keep working.
    DesktopSyncStore store(database);
    QVERIFY(store.open(&error));
    QCOMPARE(ProposalDecisions::storedClaims(database).size(), 1);
}

void ProposalDecisionsTest::testLocalDecisionRecordsClaimWithSource()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    QCOMPARE(read(workspace.source()), QByteArray("Intro.\nNew line.\nOutro.\n"));
    const QList<QJsonObject> claims = ProposalDecisions::storedClaims(workspace.database());
    QCOMPARE(claims.size(), 1);
    const QJsonObject claim = claims.first();
    QVERIFY(ProposalDecisions::isValidClaim(claim));
    QCOMPARE(claim.value(QStringLiteral("proposal_id")).toString(), QStringLiteral("agent-1"));
    QCOMPARE(claim.value(QStringLiteral("action")).toString(), QStringLiteral("accept"));
    QCOMPARE(claim.value(QStringLiteral("wrapper_sha256")).toString(), FixtureWrapperSha256);
    QCOMPARE(claim.value(QStringLiteral("source_file")).toString(), Relative);
    QCOMPARE(claim.value(QStringLiteral("origin")).toString(), QStringLiteral("desktop"));

    // "disregard" is a rejection; a stale wrapper version is refused before anything is written.
    QVERIFY(write(workspace.source(), Base.toUtf8()));
    AgentProposalDecisionOptions options;
    options.claimDatabase = workspace.database();
    options.expectedWrapperSha256 = sha("other");
    QString error, code;
    QVERIFY(!resolveLiveAgentProposedEdits(workspace.root(), workspace.source(), sha(Base.toUtf8()), {QStringLiteral("agent-1")}, QStringLiteral("disregard"), &error, nullptr, nullptr, &code, options));
    QCOMPARE(code, QStringLiteral("source_changed"));
    QCOMPARE(read(workspace.source()), Base.toUtf8());
    options.expectedWrapperSha256 = FixtureWrapperSha256;
    QVERIFY(resolveLiveAgentProposedEdits(workspace.root(), workspace.source(), sha(Base.toUtf8()), {QStringLiteral("agent-1")}, QStringLiteral("disregard"), &error, nullptr, nullptr, &code, options));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).first().value(QStringLiteral("action")).toString(), QStringLiteral("reject"));

    // A Library other than the Thoughts Library records nothing by default.
    QCOMPARE(ProposalDecisions::databasePath(workspace.root()), QString());
}

void ProposalDecisionsTest::testClaimIsForgottenWhenSourceWriteFails()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    const QJsonObject claim = ProposalDecisions::newClaim(QStringLiteral("agent-1"), QStringLiteral("accept"), FixtureWrapperSha256, Relative, QStringLiteral("desktop"));
    QString error;
    // The source changed: nothing is claimed and nothing is written.
    QVERIFY(!ProposalHistory::writeSource(workspace.root(), workspace.source(), "other", "after", {}, &error, {claim}, workspace.database()));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
    // The source cannot be replaced: the claim written ahead is forgotten again.
    const QString folder = QFileInfo(workspace.source()).absolutePath();
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(workspace.source(), QFileDevice::ReadOwner);
    const bool written = ProposalHistory::writeSource(workspace.root(), workspace.source(), Base.toUtf8(), "after", {}, &error, {claim}, workspace.database());
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(workspace.source(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QVERIFY(!written);
    QCOMPARE(read(workspace.source()), Base.toUtf8());
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

void ProposalDecisionsTest::testWithdrawalRecordsClaim()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    const QJsonObject request{{QStringLiteral("command"), QStringLiteral("remove-proposal")}, {QStringLiteral("source"), Relative},
                              {QStringLiteral("expected_source_sha256"), sha(Base.toUtf8())}, {QStringLiteral("proposal_id"), QStringLiteral("agent-1")},
                              {QStringLiteral("explanation"), QStringLiteral("No longer needed.")},
                              {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")},
                              {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const QJsonObject result = authorProposal(workspace.root(), request, QStringLiteral("mcp"), workspace.database());
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(read(workspace.source()), QByteArray("Intro.\nOld line.\nOutro.\n"));
    const QList<QJsonObject> claims = ProposalDecisions::storedClaims(workspace.database());
    QCOMPARE(claims.size(), 1);
    QCOMPARE(claims.first().value(QStringLiteral("action")).toString(), QStringLiteral("withdraw"));
    QCOMPARE(claims.first().value(QStringLiteral("origin")).toString(), QStringLiteral("mcp"));
    QCOMPARE(claims.first().value(QStringLiteral("wrapper_sha256")).toString(), FixtureWrapperSha256);
    // A retry is replayed and records nothing more.
    const QJsonObject replay = authorProposal(workspace.root(), request, QStringLiteral("mcp"), workspace.database());
    QVERIFY(replay.value(QStringLiteral("replayed")).toBool());
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
}

// Files Sync never uploads (tex/generated/, .ThoughtIgnore, outside a
// Project's folders) can never carry a claim: the decision is made, no claim.
void ProposalDecisionsTest::testUnmanagedSourcesRecordNoClaim()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    QVERIFY(ProposalDecisions::claimApplies(workspace.root(), Relative));
    const QString generated = QStringLiteral("docs-x/tex/generated/tables/token_selection/results.tex");
    const QString ignored = QStringLiteral("docs-x/tex/source/private/notes.tex");
    const QString unowned = QStringLiteral("note with spaces.tex");
    const QString root = workspace.root();
    for (const QString &path : {generated, ignored}) QVERIFY(write(QDir(root).filePath(path), Base.toUtf8()));
    // remove-proposal wants an ID that is unique in the Library.
    const QString unownedText = QString(Base).replace(QStringLiteral("{agent-1}"), QStringLiteral("{agent-7}"));
    QVERIFY(write(QDir(root).filePath(unowned), unownedText.toUtf8()));
    QVERIFY(write(QDir(root).filePath(QStringLiteral("docs-x/.ThoughtIgnore")), "tex/source/private/\n"));
    for (const QString &path : {generated, ignored, unowned}) QVERIFY2(!ProposalDecisions::claimApplies(root, path), qPrintable(path));

    // A local decision (the desktop's Accept).
    AgentProposalDecisionOptions options;
    options.claimDatabase = workspace.database();
    QString error;
    QVERIFY(resolveLiveAgentProposedEdits(root, QDir(root).filePath(ignored), sha(Base.toUtf8()), {QStringLiteral("agent-1")}, QStringLiteral("accept"), &error, nullptr,
                                          nullptr, nullptr, options));
    QCOMPARE(read(QDir(root).filePath(ignored)), QByteArray("Intro.\nNew line.\nOutro.\n"));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());

    // thoughtsctl decide-proposal: the file is decided, the answer says why there is no claim.
    const QJsonObject request{{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("decide-proposal")},
                              {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("source"), QDir(root).filePath(generated)},
                              {QStringLiteral("expected_source_sha256"), sha(Base.toUtf8())}, {QStringLiteral("proposal_id"), QStringLiteral("agent-1")},
                              {QStringLiteral("wrapper_sha256"), FixtureWrapperSha256}, {QStringLiteral("action"), QStringLiteral("accept")},
                              {QStringLiteral("origin"), QStringLiteral("thoughtsctl")}};
    const QJsonObject result = ProposalDecisions::decideProposal(root, request, workspace.database());
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("claim")).toString(), QStringLiteral("not_applicable"));
    QVERIFY(result.value(QStringLiteral("message")).toString().contains(QStringLiteral("no decision is recorded")));
    QCOMPARE(read(QDir(root).filePath(generated)), QByteArray("Intro.\nNew line.\nOutro.\n"));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());

    // An agent's withdrawal (remove-proposal).
    const QJsonObject removal{{QStringLiteral("command"), QStringLiteral("remove-proposal")}, {QStringLiteral("source"), unowned},
                              {QStringLiteral("expected_source_sha256"), sha(unownedText.toUtf8())}, {QStringLiteral("proposal_id"), QStringLiteral("agent-7")},
                              {QStringLiteral("explanation"), QStringLiteral("No longer needed.")},
                              {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")},
                              {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const QJsonObject removed = authorProposal(root, removal, QStringLiteral("mcp"), workspace.database());
    QVERIFY2(removed.value(QStringLiteral("accepted")).toBool(), QJsonDocument(removed).toJson().constData());
    QCOMPARE(read(QDir(root).filePath(unowned)), QByteArray("Intro.\nOld line.\nOutro.\n"));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());

    // The managed source still gets its claim.
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
}

// Claims stored before Thoughts knew better (the user's 9 accepts in a
// tex/generated/ table, the 2 test claims for "note with spaces.tex") are
// dropped by the next publication instead of waiting forever.
void ProposalDecisionsTest::testPublicationDropsClaimsNoUploadCanCarry()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QString generated = QStringLiteral("docs-x/tex/generated/tables/token_selection/epoch_comparison_results_evaluation_v2.tex");
    QVERIFY(write(QDir(workspace.root()).filePath(generated), "Table.\n"));
    QVERIFY(write(QDir(workspace.root()).filePath(QStringLiteral("docs-x/tex/source/waiting.tex")), "Waiting.\n"));
    const auto claim = [](const QString &id, const QString &path, const QString &origin = QStringLiteral("thoughtsctl")) {
        return ProposalDecisions::newClaim(id, QStringLiteral("accept"), FixtureWrapperSha256, path, origin);
    };
    const QList<QJsonObject> inapplicable{claim(QStringLiteral("agent-g1"), generated), claim(QStringLiteral("agent-g2"), generated),
                                          claim(QStringLiteral("agent-t1"), QStringLiteral("note with spaces.tex"), QStringLiteral("mcp")),
                                          claim(QStringLiteral("agent-t2"), QStringLiteral("note with spaces.tex"), QStringLiteral("mcp")),
                                          // Managed, but gone with nothing to delete on the server.
                                          claim(QStringLiteral("agent-m1"), QStringLiteral("docs-x/tex/source/never-published.tex"))};
    const QJsonObject waiting = claim(QStringLiteral("agent-w1"), QStringLiteral("docs-x/tex/source/waiting.tex"));
    QVERIFY(ProposalDecisions::recordClaims(workspace.database(), inapplicable + QList<QJsonObject>{waiting}));
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    const QJsonObject decided = ProposalDecisions::storedClaims(workspace.database(), nullptr, {Relative}).first();

    QStringList messages;
    QVERIFY2(workspace.publish(server, &failure, &messages), qPrintable(failure));
    QCOMPARE(server.publications.last().value(QStringLiteral("proposal_decisions")).toArray(), QJsonArray{decided});
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()), QList<QJsonObject>{waiting});
    const QString log = messages.join(QLatin1Char('\n'));
    QVERIFY2(log.contains(QStringLiteral("Dropped 2 proposal decision(s) for %1: Sync does not manage this file.").arg(generated)), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("Dropped 2 proposal decision(s) for note with spaces.tex: Sync does not manage this file.")), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("for docs-x/tex/source/never-published.tex: the file no longer exists and no deletion is pending.")), qPrintable(log));

    // Without any change to upload, the cleanup happens all the same.
    QVERIFY(ProposalDecisions::recordClaims(workspace.database(), {claim(QStringLiteral("agent-g3"), generated)}));
    const int publications = server.publications.size();
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.size(), publications);
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()), QList<QJsonObject>{waiting});

    // A managed file deleted locally keeps its claim: the deletion carries it.
    QVERIFY(write(workspace.source(), Base.toUtf8()));
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QVERIFY(workspace.decide(QStringLiteral("reject")));
    const QJsonObject rejected = ProposalDecisions::storedClaims(workspace.database(), nullptr, {Relative}).first();
    QVERIFY(QFile::remove(workspace.source()));
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.last().value(QStringLiteral("proposal_decisions")).toArray(), QJsonArray{rejected});
    QCOMPARE(server.publications.last().value(QStringLiteral("mutations")).toArray().first().toObject().value(QStringLiteral("action")).toString(), QStringLiteral("delete"));
}

// A Sync that publishes nothing never starts the publisher, so the app drops
// the claims no upload can carry itself, by the same rule and from the
// publisher's state: a managed file deleted locally keeps its claim while a
// Fedora base records that its deletion is still to be published.
void ProposalDecisionsTest::testSyncDropsClaimsWithoutThePublisher()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QString statePath = QDir(workspace.stateRoot()).filePath(QStringLiteral("source-publication.json"));
    QStringList messages;
    const auto drop = [&] {
        return Publisher::dropInapplicableProposalClaims(workspace.root(), workspace.manifest(), statePath, workspace.database(),
                                                         [&messages](const QString &message) { messages.append(message); });
    };
    // Nothing stored: nothing is read and no database is created.
    QCOMPARE(drop(), 0);
    QVERIFY(!QFileInfo::exists(workspace.database()));
    QVERIFY(messages.isEmpty());

    const QString generated = QStringLiteral("docs-x/tex/generated/table.tex");
    QVERIFY(write(QDir(workspace.root()).filePath(generated), "Table.\n"));
    const auto claim = [](const QString &id, const QString &path) {
        return ProposalDecisions::newClaim(id, QStringLiteral("accept"), FixtureWrapperSha256, path, QStringLiteral("thoughtsctl"));
    };
    QVERIFY(ProposalDecisions::recordClaims(workspace.database(), {claim(QStringLiteral("agent-g1"), generated), claim(QStringLiteral("agent-t1"), QStringLiteral("note with spaces.tex")),
                                                                   claim(QStringLiteral("agent-m1"), QStringLiteral("docs-x/tex/source/never-published.tex"))}));
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    QVERIFY(QFile::remove(workspace.source())); // published before: its base waits for the deletion
    const QList<QJsonObject> decided = ProposalDecisions::storedClaims(workspace.database(), nullptr, {Relative});
    QCOMPARE(decided.size(), 1);
    QCOMPARE(drop(), 3);
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()), decided);
    const QString log = messages.join(QLatin1Char('\n'));
    QVERIFY2(log.contains(QStringLiteral("Dropped 1 proposal decision(s) for %1: Sync does not manage this file.").arg(generated)), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("Dropped 1 proposal decision(s) for note with spaces.tex: Sync does not manage this file.")), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("Dropped 1 proposal decision(s) for docs-x/tex/source/never-published.tex: the file no longer exists and no deletion is pending.")),
             qPrintable(log));
    QCOMPARE(messages.size(), 3);

    // The publisher then carries the kept claim with the deletion.
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.last().value(QStringLiteral("proposal_decisions")).toArray(), QJsonArray{decided.first()});
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

// Tests and thoughtsctl under a private HOME, CFFIXED_USER_HOME or
// THOUGHTS_HOME never reach the account's real desktop-sync.sqlite.
void ProposalDecisionsTest::testClaimDatabaseStaysInTheTestHome()
{
    const QString account = Platform::accountHomeDirectory();
    const auto real = [&account](const QString &path) {
        return !account.isEmpty() && (path.startsWith(account + QStringLiteral("/Library/")) || path.startsWith(account + QStringLiteral("/.local/share/")));
    };
    QTemporaryDir library;
    const QByteArray savedRoot = qgetenv("THOUGHTS_HOME");
    qputenv("THOUGHTS_HOME", library.path().toLocal8Bit());
    const auto restoreRoot = qScopeGuard([&savedRoot] {
        if (savedRoot.isEmpty()) qunsetenv("THOUGHTS_HOME");
        else qputenv("THOUGHTS_HOME", savedRoot);
    });
    QString database = ProposalDecisions::databasePath(library.path());
    QVERIFY2(!database.isEmpty() && !real(database), qPrintable(database));

    // A private HOME alone moves the data folders (QStandardPaths reads only
    // CFFIXED_USER_HOME on macOS, and Qt's test mode then stays in the real one).
    if (!qEnvironmentVariableIsSet("CFFIXED_USER_HOME")) {
        QTemporaryDir home;
        const QString homePath = QDir::cleanPath(home.path());
        const QByteArray savedHome = qgetenv("HOME");
        qputenv("HOME", homePath.toLocal8Bit());
        const auto restoreHome = qScopeGuard([&savedHome] { qputenv("HOME", savedHome); });
        QVERIFY2(Platform::applicationDataRoot().startsWith(homePath + QLatin1Char('/')), qPrintable(Platform::applicationDataRoot()));
        QVERIFY2(Platform::thoughtsDataRoot().startsWith(homePath + QLatin1Char('/')), qPrintable(Platform::thoughtsDataRoot()));
        database = ProposalDecisions::databasePath(library.path());
        QVERIFY2(database.startsWith(homePath + QLatin1Char('/')), qPrintable(database));
    }

    // THOUGHTS_HOME alone (no private home, no test mode): a temporary
    // Library gets no claim database rather than the real one.
    QStandardPaths::setTestModeEnabled(false);
    const auto restoreTestMode = qScopeGuard([] { QStandardPaths::setTestModeEnabled(true); });
    database = ProposalDecisions::databasePath(library.path());
    if (Platform::isAccountDataRoot(Platform::applicationDataRoot())) QCOMPARE(database, QString());
    else QVERIFY2(!database.isEmpty() && !real(database), qPrintable(database));
    // decide-proposal then answers not_recorded and writes no database.
    Workspace workspace;
    QVERIFY(workspace.prepare());
    qputenv("THOUGHTS_HOME", workspace.root().toLocal8Bit());
    if (Platform::isAccountDataRoot(Platform::applicationDataRoot())) {
        const QJsonObject request{{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("decide-proposal")},
                                  {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("source"), workspace.source()},
                                  {QStringLiteral("expected_source_sha256"), sha(Base.toUtf8())}, {QStringLiteral("proposal_id"), QStringLiteral("agent-1")},
                                  {QStringLiteral("wrapper_sha256"), FixtureWrapperSha256}, {QStringLiteral("action"), QStringLiteral("accept")},
                                  {QStringLiteral("origin"), QStringLiteral("thoughtsctl")}};
        const QJsonObject result = ProposalDecisions::decideProposal(workspace.root(), request);
        QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(result.value(QStringLiteral("claim")).toString(), QStringLiteral("not_recorded"));
    }
}

void ProposalDecisionsTest::testPublicationAttachesAndExpiresClaims()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.size(), 1);
    QVERIFY(!server.publications.first().contains(QStringLiteral("proposal_decisions")));

    QVERIFY(workspace.decide(QStringLiteral("accept")));
    const QJsonObject claim = ProposalDecisions::storedClaims(workspace.database()).first();
    // A claim for a managed file this publication does not upload (not yet
    // in its manifest) waits.
    QVERIFY(write(QDir(workspace.root()).filePath(QStringLiteral("docs-x/tex/source/other.tex")), "Other.\n"));
    const QJsonObject waiting = ProposalDecisions::newClaim(QStringLiteral("agent-9"), QStringLiteral("reject"), FixtureWrapperSha256, QStringLiteral("docs-x/tex/source/other.tex"), QStringLiteral("desktop"));
    QVERIFY(ProposalDecisions::recordClaims(workspace.database(), {waiting}));
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.size(), 2);
    QCOMPARE(server.publications.last().value(QStringLiteral("proposal_decisions")).toArray(), QJsonArray{claim});
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()), QList<QJsonObject>{waiting});
    QVERIFY(!workspace.state().contains(QStringLiteral("pending_publication_decisions")));
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("sha256")).toString(), sha("Intro.\nNew line.\nOutro.\n"));
}

void ProposalDecisionsTest::testUndoDropsClaimAtUpload()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QVERIFY(workspace.publish(server));
    const QByteArray before = read(workspace.source());
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    // Undo brings the wrapper back before Sync.
    QString error;
    QVERIFY(restoreLiveAgentProposalSource(workspace.root(), workspace.source(), sha(read(workspace.source())), before, &error));
    QVERIFY(workspace.publish(server));
    QCOMPARE(server.publications.size(), 1); // nothing changed, nothing published
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());

    // An undone decision followed by another edit uploads the file without the claim.
    QVERIFY(workspace.decide(QStringLiteral("reject")));
    QVERIFY(restoreLiveAgentProposalSource(workspace.root(), workspace.source(), sha(read(workspace.source())), before, &error));
    QVERIFY(write(workspace.source(), before + "More.\n"));
    QVERIFY(workspace.publish(server));
    QCOMPARE(server.publications.size(), 2);
    QVERIFY(!server.publications.last().contains(QStringLiteral("proposal_decisions")));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

void ProposalDecisionsTest::testOlderServerExpiresClaimsWithoutSendingThem()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    server.capable = false;
    QVERIFY(workspace.publish(server));
    QVERIFY(workspace.decide(QStringLiteral("reject")));
    QVERIFY(workspace.publish(server));
    QCOMPARE(server.publications.size(), 2);
    QVERIFY(!server.publications.last().contains(QStringLiteral("proposal_decisions")));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

void ProposalDecisionsTest::testRetriedPublicationSendsTheSameClaims()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QVERIFY(workspace.publish(server));
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    // Since 14 November 2026 the publisher itself sends a publication again when the
    // server is unavailable (three times). The run fails only when the server stays
    // unavailable; the next run then sends the same request.
    server.failNextPublications = 4;
    QVERIFY(!workspace.publish(server));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    QVERIFY(workspace.state().value(QStringLiteral("pending_publication")).toObject().contains(QStringLiteral("proposal_decisions")));
    QVERIFY(workspace.publish(server));
    // The first publication, the four refused sendings and the one that arrived.
    QCOMPARE(server.publications.size(), 6);
    for (int sending = 2; sending < 6; ++sending) {
        QCOMPARE(server.publications.at(sending), server.publications.at(1));
    }
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
    QVERIFY(!workspace.state().contains(QStringLiteral("pending_publication_decisions")));
}

void ProposalDecisionsTest::testClaimsWaitWhenTheServerCannotBeAsked()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QVERIFY(workspace.publish(server));
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    server.healthFails = true;
    QVERIFY(workspace.publish(server));
    QVERIFY(!server.publications.last().contains(QStringLiteral("proposal_decisions")));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    QVERIFY(!workspace.state().contains(QStringLiteral("pending_publication_decisions")));
    // The next upload of the file carries the claim (the server matches it late).
    server.healthFails = false;
    QVERIFY(write(workspace.source(), read(workspace.source()) + "More.\n"));
    QVERIFY(workspace.publish(server));
    QCOMPARE(server.publications.last().value(QStringLiteral("proposal_decisions")).toArray().size(), 1);
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

// The publisher and the claims against a real thoughts-server with
// proposal_decisions_v1 (tools/e2e/run-proposal-decisions-server.py starts one).
// THOUGHTS_DECISIONS_SERVER_STRICT says which mode the server runs in.
void ProposalDecisionsTest::testClaimsAgainstServer()
{
    const QString url = qEnvironmentVariable("THOUGHTS_DECISIONS_SERVER");
    const QString token = qEnvironmentVariable("THOUGHTS_DECISIONS_SERVER_TOKEN");
    if (url.isEmpty() || token.isEmpty()) QSKIP("Set THOUGHTS_DECISIONS_SERVER and THOUGHTS_DECISIONS_SERVER_TOKEN (tools/e2e/run-proposal-decisions-server.py).");
    const bool strict = qEnvironmentVariable("THOUGHTS_DECISIONS_SERVER_STRICT") == QLatin1String("1");
    QNetworkAccessManager network;
    // The latest server payload of a proposal row, from the change feed.
    const auto row = [&](const QString &id) {
        QJsonObject latest;
        for (qint64 after = 0;;) {
            QNetworkRequest request(QUrl(url + QStringLiteral("/v1/changes?limit=1000&after=%1").arg(after)));
            request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            QNetworkReply *reply = network.get(request);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            const QJsonObject page = QJsonDocument::fromJson(reply->readAll()).object();
            reply->deleteLater();
            for (const QJsonValue &value : page.value(QStringLiteral("changes")).toArray()) {
                const QJsonObject change = value.toObject();
                if (change.value(QStringLiteral("entity_type")).toString() == QLatin1String("proposal") && change.value(QStringLiteral("entity_id")).toString() == id
                    && change.value(QStringLiteral("outcome")).toString() == QLatin1String("applied"))
                    latest = change.value(QStringLiteral("payload")).toObject();
            }
            const qint64 next = page.value(QStringLiteral("next_cursor")).toInteger(after);
            if (!page.value(QStringLiteral("has_more")).toBool() || next <= after) break;
            after = next;
        }
        return latest;
    };
    const auto status = [&](const QString &id) { return row(id).value(QStringLiteral("status")).toString(); };
    // A workspace with its own folder and proposal ID on the shared server.
    const auto workspaceFor = [](Workspace &workspace, QString *text) {
        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        workspace.relative = QStringLiteral("docs-%1/tex/source/a.tex").arg(suffix);
        workspace.proposalId = QStringLiteral("agent-%1").arg(suffix);
        *text = QString(Base).replace(QStringLiteral("{agent-1}"), QStringLiteral("{%1}").arg(workspace.proposalId));
        // One device token on the server: each workspace continues its device sequence.
        static qint64 nextSequence = 1;
        const QJsonObject state{{QStringLiteral("schema_version"), 1}, {QStringLiteral("next_device_sequence"), nextSequence}, {QStringLiteral("bases"), QJsonObject()}};
        nextSequence += 1000;
        return workspace.prepare(*text) && write(QDir(workspace.stateRoot()).filePath(QStringLiteral("source-publication.json")), QJsonDocument(state).toJson());
    };
    QString failure, text;

    // Accept and reject in Thoughts: matched, whatever the mode.
    for (const QString &action : {QStringLiteral("accept"), QStringLiteral("reject")}) {
        Workspace workspace;
        QVERIFY(workspaceFor(workspace, &text));
        QVERIFY2(workspace.publishTo(url, token, &failure), qPrintable(failure));
        QCOMPARE(status(workspace.proposalId), QStringLiteral("pending"));
        QVERIFY(row(workspace.proposalId).value(QStringLiteral("wrapper_sha256")).toString().size() == 64);
        QCOMPARE(row(workspace.proposalId).value(QStringLiteral("wrapper_sha256")).toString(),
                 ProposalDecisions::wrapperSha256(text, parseAgentProposals(workspace.relative, text).proposals.first()));
        QVERIFY(workspace.decide(action));
        QVERIFY2(workspace.publishTo(url, token, &failure), qPrintable(failure));
        QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
        QCOMPARE(status(workspace.proposalId), action == QLatin1String("accept") ? QStringLiteral("accepted") : QStringLiteral("rejected"));
        QCOMPARE(workspace.state().value(QStringLiteral("last_publication")).toObject().value(QStringLiteral("proposal_decisions")).toArray().first().toObject()
                     .value(QStringLiteral("outcome")).toString(), QStringLiteral("matched"));
    }
    // An agent's withdrawal.
    {
        Workspace workspace;
        QVERIFY(workspaceFor(workspace, &text));
        QVERIFY(workspace.publishTo(url, token, &failure));
        const QJsonObject request{{QStringLiteral("command"), QStringLiteral("remove-proposal")}, {QStringLiteral("source"), workspace.relative},
                                  {QStringLiteral("expected_source_sha256"), sha(text.toUtf8())}, {QStringLiteral("proposal_id"), workspace.proposalId},
                                  {QStringLiteral("explanation"), QStringLiteral("Obsolete.")},
                                  {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")},
                                  {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
        QVERIFY(authorProposal(workspace.root(), request, QStringLiteral("mcp"), workspace.database()).value(QStringLiteral("accepted")).toBool());
        QVERIFY(workspace.publishTo(url, token, &failure));
        QCOMPARE(status(workspace.proposalId), QStringLiteral("withdrawn"));
    }
    // A decision undone before Sync is dropped: the proposal stays pending.
    {
        Workspace workspace;
        QVERIFY(workspaceFor(workspace, &text));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QVERIFY(workspace.decide(QStringLiteral("accept")));
        QString error;
        QVERIFY(restoreLiveAgentProposalSource(workspace.root(), workspace.source(), sha(read(workspace.source())), text.toUtf8(), &error));
        QVERIFY(write(workspace.source(), text.toUtf8() + "More.\n"));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
        QCOMPARE(status(workspace.proposalId), QStringLiteral("pending"));
    }
    // Removed by a plain edit to exactly the accepted text, without a claim.
    {
        Workspace workspace;
        QVERIFY(workspaceFor(workspace, &text));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QVERIFY(write(workspace.source(), "Intro.\nNew line.\nOutro.\n"));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QCOMPARE(status(workspace.proposalId), strict ? QStringLiteral("removed_outside") : QStringLiteral("accepted"));
        if (strict) QCOMPARE(row(workspace.proposalId).value(QStringLiteral("removal")).toObject().value(QStringLiteral("looks_like")).toString(), QStringLiteral("accept"));
    }
    // Removed by an unrelated edit first, decided later: the claim matches late.
    {
        Workspace workspace;
        QVERIFY(workspaceFor(workspace, &text));
        QVERIFY(workspace.publishTo(url, token, &failure));
        const QString wrapperSha = ProposalDecisions::wrapperSha256(text, parseAgentProposals(workspace.relative, text).proposals.first());
        QVERIFY(write(workspace.source(), "Intro.\nSomething else.\nOutro.\n"));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QCOMPARE(status(workspace.proposalId), QStringLiteral("removed_outside"));
        QCOMPARE(row(workspace.proposalId).value(QStringLiteral("removal")).toObject().value(QStringLiteral("source_line")).toInt(), 2);
        QVERIFY(ProposalDecisions::recordClaims(workspace.database(), {ProposalDecisions::newClaim(workspace.proposalId, QStringLiteral("reject"), wrapperSha,
                                                                                                    workspace.relative, QStringLiteral("desktop"))}));
        QVERIFY(write(workspace.source(), "Intro.\nSomething else, again.\nOutro.\n"));
        QVERIFY(workspace.publishTo(url, token, &failure));
        QCOMPARE(status(workspace.proposalId), QStringLiteral("rejected"));
        QCOMPARE(workspace.state().value(QStringLiteral("last_publication")).toObject().value(QStringLiteral("proposal_decisions")).toArray().first().toObject()
                     .value(QStringLiteral("outcome")).toString(), QStringLiteral("matched_late"));
        QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
    }
}

void ProposalDecisionsTest::testConflictResolutionCarriesClaims()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    const QJsonObject claim = ProposalDecisions::storedClaims(workspace.database()).first();
    // The resolved text still removes the wrapper: the claim travels with it.
    QString error;
    const QList<QJsonObject> carried = ProposalDecisions::claimsForConflictResolution(workspace.database(), Relative, QStringLiteral("Intro.\nNew line, merged.\nOutro.\n"), &error);
    QCOMPARE(carried, QList<QJsonObject>{claim});
    DesktopSyncOutbox outbox(workspace.database());
    QVERIFY(outbox.ready(&error));
    QJsonArray decisions;
    for (const QJsonObject &value : carried) decisions.append(value);
    const QJsonObject operation = outbox.enqueueSourceConflictResolution(QStringLiteral("conflict-1"), 3, sha("current"), "Intro.\nNew line, merged.\nOutro.\n",
                                                                         QStringLiteral("2026-10-11T09:00:00.000Z"), decisions);
    QCOMPARE(operation.value(QStringLiteral("proposal_decisions")).toArray(), decisions);
    QCOMPARE(outbox.operations().first().value(QStringLiteral("proposal_decisions")).toArray(), decisions);
    // Still stored until the resolution is answered.
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    // A resolution that keeps the wrapper undoes the decision: the claim is dropped.
    QVERIFY(ProposalDecisions::claimsForConflictResolution(workspace.database(), Relative, Base, &error).isEmpty());
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
}

// The user accepted a proposal here while an agent proposed another edit
// next to it on Fedora. This computer's merge conflicts, so the frozen source
// goes to Fedora alone; Fedora's merge joins both and commits the result,
// which becomes the file and its base, so the next Sync sends nothing. (On
// 2026-11-13 the answer was refused as inconsistent and every automatic Sync
// sent the change again, each time committed as one more revision.)
void ProposalDecisionsTest::testFedoraMergeOfAConflictingSourceIsInstalled()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QByteArray accepted = "Intro.\nNew line.\nOutro.\n";
    const QByteArray merged = (QStringLiteral("Intro.\nNew line.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (Base + SecondWrapper).toUtf8());
    QVERIFY(write(workspace.source(), accepted));
    server.applyResult = merged;

    QStringList messages;
    QVERIFY2(workspace.publishFrozen(server, &failure, &messages), qPrintable(failure));
    QCOMPARE(server.applies.size(), 1);
    QCOMPARE(server.applies.first().value(QStringLiteral("content_sha256")).toString(), sha(accepted));
    QCOMPARE(server.applies.first().value(QStringLiteral("base_sha256")).toString(), sha(Base.toUtf8()));
    QCOMPARE(server.publications.size(), 1);
    QCOMPARE(read(workspace.source()), merged);
    const QJsonObject base{{QStringLiteral("revision"), 3}, {QStringLiteral("sha256"), sha(merged)}, {QStringLiteral("object_sha256"), sha(merged)}};
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject(), base);
    QVERIFY(!workspace.state().contains(QStringLiteral("conflicts")));
    QVERIFY(!workspace.state().contains(QStringLiteral("pending_source_applies")));
    const QString log = messages.join(QLatin1Char('\n'));
    QVERIFY2(log.contains(QStringLiteral("The server merged source change %1 into revision 3.").arg(Relative)), qPrintable(log));
    QVERIFY2(log.contains(QStringLiteral("1 updated")), qPrintable(log));

    // The next Syncs, frozen or not, have nothing to send.
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.applies.size(), 1);
    QCOMPARE(server.publications.size(), 1);
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(read(workspace.source()), merged);
}

// 18 Nov: proposals were accepted on this computer while agents changed
// the same file on the server, so this computer's merge conflicted and the
// source went to the server alone. Sent without its claims, the server recorded the
// accepted proposals as removed outside Thoughts.
void ProposalDecisionsTest::testSourceSentAloneCarriesItsClaims()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QByteArray merged = (QStringLiteral("Intro.\nNew line.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (Base + SecondWrapper).toUtf8());
    {
        // The app's own tables, which hold the claims too, as on a computer that has synced.
        DesktopSyncStore store(workspace.database());
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
    }
    QVERIFY(workspace.decide(QStringLiteral("accept")));
    QCOMPARE(read(workspace.source()), QByteArray("Intro.\nNew line.\nOutro.\n"));
    const QList<QJsonObject> claims = ProposalDecisions::storedClaims(workspace.database());
    QCOMPARE(claims.size(), 1);
    server.applyResult = merged;

    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(server.applies.size(), 1);
    QCOMPARE(server.applies.first().value(QStringLiteral("proposal_decisions")).toArray(), QJsonArray{claims.first()});
    // Answered: the claim expires, and no later publication sends it again.
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
    QCOMPARE(read(workspace.source()), merged);
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    for (const QJsonObject &publication : std::as_const(server.publications)) QVERIFY(!publication.contains(QStringLiteral("proposal_decisions")));
}

// A folder a Project of this Library connects, made on another device (the server's
// add_source_folder), installs here although this computer does not have it yet: the
// manifest lists only the folders found here (18 Nov: the math Project's folder never came).
void ProposalDecisionsTest::testFolderAddedOnAnotherDeviceInstalls()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QString added = QStringLiteral("docs-added/tex/source/note.tex");
    server.commit(added, "Added on the server.\n");
    // Not connected to a Project here: it stays on the server.
    QVERIFY2(workspace.install(server, &failure), qPrintable(failure));
    QVERIFY(!QFileInfo::exists(QDir(workspace.root()).filePath(added)));
    QCOMPARE(ProjectConfiguration::missingConnectedFolders(workspace.root()), QStringList());
    QVERIFY(connectFolder(workspace.root(), QStringLiteral("docs-added")));
    // What makes Sync run the installer although neither the server nor the manifest changed.
    QCOMPARE(ProjectConfiguration::missingConnectedFolders(workspace.root()), QStringList{QStringLiteral("docs-added")});
    QVERIFY2(workspace.install(server, &failure), qPrintable(failure));
    QCOMPARE(read(QDir(workspace.root()).filePath(added)), QByteArray("Added on the server.\n"));
    QCOMPARE(ProjectConfiguration::missingConnectedFolders(workspace.root()), QStringList());
}

// Fedora committed the source sent alone but its answer was lost, or came
// back without the result's identity: the retry sends the same operation,
// which Fedora answers from its record, so the change is committed once.
void ProposalDecisionsTest::testUnansweredSourceApplyIsRepeated()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QByteArray accepted = "Intro.\nNew line.\nOutro.\n";
    const QByteArray merged = (QStringLiteral("Intro.\nNew line.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (Base + SecondWrapper).toUtf8());
    QVERIFY(write(workspace.source(), accepted));
    server.applyResult = merged;
    server.loseNextApplyAnswers = 1;

    QVERIFY(!workspace.publishFrozen(server, &failure));
    QVERIFY2(failure.contains(QStringLiteral("gateway timeout")), qPrintable(failure));
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(read(workspace.source()), accepted);
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(workspace.state().value(QStringLiteral("pending_source_applies")).toObject().value(Relative).toObject(), server.applies.first());

    QStringList messages;
    QVERIFY2(workspace.publishFrozen(server, &failure, &messages), qPrintable(failure));
    QCOMPARE(server.applies.size(), 2);
    QCOMPARE(server.applies.at(1), server.applies.at(0));
    QVERIFY2(messages.join(QLatin1Char('\n')).contains(QStringLiteral("Repeating the unanswered source change %1").arg(Relative)), qPrintable(messages.join(QLatin1Char('\n'))));
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(read(workspace.source()), merged);
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("sha256")).toString(), sha(merged));
    QVERIFY(!workspace.state().contains(QStringLiteral("pending_source_applies")));

    // An answer without the result's identity is refused and nothing is installed.
    const QByteArray mine = (QStringLiteral("Intro.\nNew line, edited here.\n") + SecondWrapper).toUtf8();
    const QByteArray both = (QStringLiteral("Intro.\nNew line, edited on both sides.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (QStringLiteral("Intro.\nNew line, edited on Fedora.\n") + SecondWrapper).toUtf8());
    QVERIFY(write(workspace.source(), mine));
    server.applyResult = both;
    server.malformedApplyAnswer = true;
    QVERIFY(!workspace.publishFrozen(server, &failure));
    QVERIFY2(failure.contains(QStringLiteral("the server returned an inconsistent source merge result: ") + Relative), qPrintable(failure));
    QCOMPARE(read(workspace.source()), mine);
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("revision")).toInt(), 5);
    // The retry is answered from Fedora's record.
    server.malformedApplyAnswer = false;
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(server.applies.size(), 4);
    QCOMPARE(server.applies.at(3), server.applies.at(2));
    QCOMPARE(server.heads.value(Relative).value(QStringLiteral("revision")).toInt(), 5);
    QCOMPARE(read(workspace.source()), both);
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    QCOMPARE(server.applies.size(), 4);
    QCOMPARE(server.publications.size(), 1);
}

// The user edited the file again while Fedora merged its frozen copy: the
// newer edit stays, and so does the base it was written on, so the next Sync
// merges it with Fedora's result instead of replacing that result.
void ProposalDecisionsTest::testEditDuringFedoraMergeKeepsItsBase()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QByteArray accepted = "Intro.\nNew line.\nOutro.\n";
    const QByteArray newer = "Intro.\nNew line.\nOutro.\nMore.\n";
    const QByteArray merged = (QStringLiteral("Intro.\nNew line.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (Base + SecondWrapper).toUtf8());
    QVERIFY(write(workspace.source(), accepted));
    server.applyResult = merged;
    server.whileApplying = [&] { QVERIFY(write(workspace.source(), newer)); };

    QStringList messages;
    QVERIFY2(workspace.publishFrozen(server, &failure, &messages), qPrintable(failure));
    QCOMPARE(read(workspace.source()), newer);
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("sha256")).toString(), sha(Base.toUtf8()));
    QVERIFY2(messages.join(QLatin1Char('\n')).contains(QStringLiteral("the next Sync merges the newer edit")), qPrintable(messages.join(QLatin1Char('\n'))));

    server.whileApplying = nullptr;
    server.applyResult = merged + "More.\n";
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(server.applies.size(), 2);
    QCOMPARE(server.applies.at(1).value(QStringLiteral("base_sha256")).toString(), sha(Base.toUtf8()));
    QCOMPARE(server.applies.at(1).value(QStringLiteral("content_sha256")).toString(), sha(newer));
    QCOMPARE(read(workspace.source()), merged + "More.\n");
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("revision")).toInt(), 4);
}

void ProposalDecisionsTest::testFrozenCopyOnDesktopsEarlierBase_data()
{
    QTest::addColumn<QByteArray>("localBefore");
    QTest::addColumn<QByteArray>("editAfter");
    QTest::addColumn<bool>("onInstalledBase");
    // The installer merges Fedora's revision with a local edit; the file is edited again before Desktop records the merge.
    QTest::newRow("merged, then edited") << QByteArray("a\nb\nc\nd\ne\nf\ng\nH\n") << QByteArray("A\nb\nc\nD\ne\nf\ng\nH\n") << true;
    // The installer installs Fedora's revision; Desktop could not record it (the database was locked); then an edit.
    QTest::newRow("installed, then edited") << QByteArray() << QByteArray("A\nb\nc\nD\ne\nf\ng\nh\n") << true;
    // An editor saves its copy from before the merge over it: Fedora's "A" is missing.
    QTest::newRow("older copy saved over the merge") << QByteArray("a\nb\nc\nd\ne\nf\ng\nH\n") << QByteArray("a\nb\nc\nD\ne\nf\ng\nH\n") << false;
}

// 13 Nov: the installer moved the publisher's base to Fedora's revision while
// Desktop kept its own older base, and every frozen Sync was refused with
// "frozen Desktop source has no matching authoring or server base". A frozen
// copy on that earlier base is published against the installed base when it
// holds every change Fedora made since, and merged against the earlier base
// otherwise, so Fedora's changes are never taken back.
void ProposalDecisionsTest::testFrozenCopyOnDesktopsEarlierBase()
{
    QFETCH(QByteArray, localBefore);
    QFETCH(QByteArray, editAfter);
    QFETCH(bool, onInstalledBase);
    Workspace workspace;
    const QByteArray first = "a\nb\nc\nd\ne\nf\ng\nh\n";
    const QByteArray installed = "A\nb\nc\nd\ne\nf\ng\nh\n";
    QVERIFY(workspace.prepare(QString::fromUtf8(first)));
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    server.commit(Relative, installed);
    if (!localBefore.isEmpty()) QVERIFY(write(workspace.source(), localBefore));
    QVERIFY2(workspace.install(server, &failure), qPrintable(failure));
    QCOMPARE(workspace.state().value(QStringLiteral("bases")).toObject().value(Relative).toObject().value(QStringLiteral("sha256")).toString(), sha(installed));
    QVERIFY(write(workspace.source(), editAfter));

    // A base Desktop never had stays refused.
    QVERIFY(!workspace.publishFrozen(server, &failure, nullptr, sha("unrelated\n")));
    QVERIFY2(failure.contains(QStringLiteral("frozen Desktop source has no matching authoring or server base: ") + Relative), qPrintable(failure));
    QCOMPARE(server.publications.size(), 1);

    QStringList messages;
    QVERIFY2(workspace.publishFrozen(server, &failure, &messages, sha(first)), qPrintable(failure));
    QCOMPARE(server.publications.size(), 2);
    const QJsonObject sent = server.publications.last().value(QStringLiteral("mutations")).toArray().first().toObject();
    QCOMPARE(sent.value(QStringLiteral("content_sha256")).toString(), sha(editAfter));
    QCOMPARE(sent.value(QStringLiteral("base_sha256")).toString(), onInstalledBase ? sha(installed) : sha(first));
    QCOMPARE(sent.value(QStringLiteral("base_object_sha256")).toString(), onInstalledBase ? sha(installed) : sha(first));
    const QString log = messages.join(QLatin1Char('\n'));
    QVERIFY2(log.contains(onInstalledBase ? QStringLiteral("the file holds every change since") : QStringLiteral("merged against that earlier base")), qPrintable(log));
    // Desktop then records the published bytes: nothing more to send.
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.size(), 2);
    QVERIFY(!workspace.state().value(QStringLiteral("superseded_bases")).toObject().contains(Relative));
}

void ProposalDecisionsTest::testDecisionOnFedorasMergeBeforeDesktopRecordedIt_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<QByteArray>("savedAfter");
    QTest::addColumn<QByteArray>("expected");
    QTest::addColumn<bool>("onMergedBase");
    QTest::newRow("accepted on the merge") << QStringLiteral("accept") << QByteArray() << QByteArray("Intro.\nNew line.\nEnd.\n") << true;
    QTest::newRow("rejected on the merge") << QStringLiteral("disregard") << QByteArray() << QByteArray("Intro.\nNew line.\nOutro.\n") << true;
    // The decision was made on the merge, but an editor then saved its copy
    // from before the merge over it: agent-2's offer is missing from the file
    // and no decision explains it, so Fedora keeps the offer.
    QTest::newRow("accepted, then an older copy saved over it") << QStringLiteral("accept") << QByteArray("Intro.\nNew line.\nOutro.\n")
                                                                << QByteArray("Intro.\nNew line.\nOutro.\n") << false;
}

// A conflict seen in use: the user accepted agent-1 here
// while an agent proposed agent-2 on Fedora. Fedora merged the two and the
// publisher installed the merge, but Desktop kept its base from before the
// accept (it records the merge only once a later check finds the file
// unchanged), and the user accepted agent-2 on the installed merge before
// that. The next Sync named the old base; the file, with agent-2 decided
// where Fedora's revision holds its wrapper, clashed with that revision, so
// it went to Fedora against the old base and Fedora reported a conflict.
// The claim of the decision, made on the wrapper of the merged revision, shows
// the file was written on it: it is published against that revision.
void ProposalDecisionsTest::testDecisionOnFedorasMergeBeforeDesktopRecordedIt()
{
    QFETCH(QString, action);
    QFETCH(QByteArray, savedAfter);
    QFETCH(QByteArray, expected);
    QFETCH(bool, onMergedBase);
    Workspace workspace;
    QVERIFY(workspace.prepare());
    FakeSourceServer server;
    QString failure;
    QVERIFY2(workspace.publish(server, &failure), qPrintable(failure));
    const QByteArray accepted = "Intro.\nNew line.\nOutro.\n";
    const QByteArray merged = (QStringLiteral("Intro.\nNew line.\n") + SecondWrapper).toUtf8();
    server.commit(Relative, (Base + SecondWrapper).toUtf8());
    QVERIFY(write(workspace.source(), accepted));
    server.applyResult = merged;
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(read(workspace.source()), merged);
    QCOMPARE(server.applies.size(), 1);

    // The user decides Fedora's new proposal in Thoughts.
    workspace.proposalId = QStringLiteral("agent-2");
    QVERIFY(workspace.decide(action));
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    if (!savedAfter.isEmpty()) QVERIFY(write(workspace.source(), savedAfter));
    QCOMPARE(read(workspace.source()), expected);

    // Desktop's own record still names the base from before the first accept.
    QStringList messages;
    QVERIFY2(workspace.publishFrozen(server, &failure, &messages, sha(Base.toUtf8())), qPrintable(failure));
    const QString log = messages.join(QLatin1Char('\n'));
    if (!onMergedBase) {
        // Sent against the old base, alone or not, so Fedora merges its offer back in.
        const QJsonObject sent = server.applies.size() == 2 ? server.applies.last()
                                                            : server.publications.last().value(QStringLiteral("mutations")).toArray().first().toObject();
        QCOMPARE(sent.value(QStringLiteral("content_sha256")).toString(), sha(expected));
        QCOMPARE(sent.value(QStringLiteral("base_sha256")).toString(), sha(Base.toUtf8()));
        QVERIFY2(log.contains(QStringLiteral("merged against that earlier base")), qPrintable(log));
        return;
    }
    QCOMPARE(server.applies.size(), 1);
    QCOMPARE(server.publications.size(), 2);
    const QJsonObject sent = server.publications.last().value(QStringLiteral("mutations")).toArray().first().toObject();
    QCOMPARE(sent.value(QStringLiteral("content_sha256")).toString(), sha(expected));
    QCOMPARE(sent.value(QStringLiteral("base_sha256")).toString(), sha(merged));
    QVERIFY2(log.contains(QStringLiteral("with proposals decided here, so it is published against revision 3")), qPrintable(log));
    // The decision travels with the publication and expires with its answer.
    const QJsonArray decisions = server.publications.last().value(QStringLiteral("proposal_decisions")).toArray();
    QCOMPARE(decisions.size(), 1);
    QCOMPARE(decisions.first().toObject().value(QStringLiteral("proposal_id")).toString(), QStringLiteral("agent-2"));
    QVERIFY(ProposalDecisions::storedClaims(workspace.database()).isEmpty());
    // Nothing more to send.
    QVERIFY2(workspace.publishFrozen(server, &failure), qPrintable(failure));
    QCOMPARE(server.publications.size(), 2);
    QCOMPARE(server.applies.size(), 1);
}

void ProposalDecisionsTest::testDecideProposalCommand()
{
    Workspace workspace;
    QVERIFY(workspace.prepare());
    const QString outside = workspace.directory.filePath(QStringLiteral("docs/notes/a.tex"));
    QVERIFY(write(outside, Base.toUtf8()));
    const auto request = [&](const QString &action = QStringLiteral("accept")) {
        return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("decide-proposal")},
                           {QStringLiteral("request_id"), QStringLiteral("7c1d1f4e-58a9-4b8e-9a43-5b1b3c2d9e01")}, {QStringLiteral("source"), workspace.source()},
                           {QStringLiteral("expected_source_sha256"), sha(Base.toUtf8())}, {QStringLiteral("proposal_id"), QStringLiteral("agent-1")},
                           {QStringLiteral("wrapper_sha256"), FixtureWrapperSha256}, {QStringLiteral("action"), action}, {QStringLiteral("origin"), QStringLiteral("vscode")}};
    };
    const auto refused = [&](QJsonObject value, const QString &code) {
        const QJsonObject result = ProposalDecisions::decideProposal(workspace.root(), value, workspace.database());
        if (result.value(QStringLiteral("accepted")).toBool() || result.value(QStringLiteral("error_code")).toString() != code) {
            qWarning().noquote() << QJsonDocument(result).toJson(QJsonDocument::Compact);
            return false;
        }
        return read(workspace.source()) == Base.toUtf8() && ProposalDecisions::storedClaims(workspace.database()).isEmpty();
    };
    QJsonObject value = request(QStringLiteral("partial"));
    QVERIFY(refused(value, QStringLiteral("invalid_request")));
    value = request();
    value.insert(QStringLiteral("ids"), QJsonArray{QStringLiteral("agent-1")});
    QVERIFY(refused(value, QStringLiteral("invalid_request")));
    value = request();
    value.insert(QStringLiteral("origin"), QStringLiteral("mcp"));
    QVERIFY(refused(value, QStringLiteral("invalid_request")));
    value = request();
    value.remove(QStringLiteral("wrapper_sha256"));
    QVERIFY(refused(value, QStringLiteral("invalid_request")));
    value = request();
    value.insert(QStringLiteral("source"), Relative);
    QVERIFY(refused(value, QStringLiteral("invalid_request")));
    value = request();
    value.insert(QStringLiteral("source"), outside);
    QVERIFY(refused(value, QStringLiteral("not_in_library")));
    value = request();
    value.insert(QStringLiteral("expected_source_sha256"), sha("older"));
    QVERIFY(refused(value, QStringLiteral("source_changed")));
    value = request();
    value.insert(QStringLiteral("wrapper_sha256"), sha("older wrapper"));
    QVERIFY(refused(value, QStringLiteral("source_changed")));
    value = request();
    value.insert(QStringLiteral("proposal_id"), QStringLiteral("agent-2"));
    QVERIFY(refused(value, QStringLiteral("unknown_id")));
    // The spec code of a broken wrapper (§7).
    const QString duplicated = Base + Wrapper + QStringLiteral("\n");
    QVERIFY(write(workspace.source(), duplicated.toUtf8()));
    value = request();
    value.insert(QStringLiteral("expected_source_sha256"), sha(duplicated.toUtf8()));
    value.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonObject result = ProposalDecisions::decideProposal(workspace.root(), value, workspace.database());
    QCOMPARE(result.value(QStringLiteral("error_code")).toString(), QStringLiteral("duplicate_id"));
    QVERIFY(write(workspace.source(), Base.toUtf8()));

    result = ProposalDecisions::decideProposal(workspace.root(), request(), workspace.database());
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("accepted"));
    QCOMPARE(result.value(QStringLiteral("proposal_id")).toString(), QStringLiteral("agent-1"));
    QCOMPARE(result.value(QStringLiteral("source")).toString(), workspace.source());
    QCOMPARE(result.value(QStringLiteral("source_sha256")).toString(), sha("Intro.\nNew line.\nOutro.\n"));
    QCOMPARE(result.value(QStringLiteral("claim")).toString(), QStringLiteral("recorded"));
    QCOMPARE(result.value(QStringLiteral("sync")).toString(), QStringLiteral("pending"));
    QVERIFY(!result.value(QStringLiteral("replayed")).toBool());
    QCOMPARE(read(workspace.source()), QByteArray("Intro.\nNew line.\nOutro.\n"));
    const QList<QJsonObject> claims = ProposalDecisions::storedClaims(workspace.database());
    QCOMPARE(claims.size(), 1);
    QCOMPARE(claims.first().value(QStringLiteral("origin")).toString(), QStringLiteral("vscode"));
    QCOMPARE(claims.first().value(QStringLiteral("action")).toString(), QStringLiteral("accept"));

    // A retry returns the stored answer; the same ID with other arguments is a conflict.
    QJsonObject replay = ProposalDecisions::decideProposal(workspace.root(), request(), workspace.database());
    QVERIFY(replay.value(QStringLiteral("replayed")).toBool());
    replay.remove(QStringLiteral("replayed"));
    result.remove(QStringLiteral("replayed"));
    QCOMPARE(replay, result);
    QCOMPARE(ProposalDecisions::storedClaims(workspace.database()).size(), 1);
    result = ProposalDecisions::decideProposal(workspace.root(), request(QStringLiteral("reject")), workspace.database());
    QCOMPARE(result.value(QStringLiteral("error_code")).toString(), QStringLiteral("request_conflict"));
}

void ProposalDecisionsTest::testRestoreRemovedProposal()
{
    Workspace workspace;
    QVERIFY(workspace.prepare(QStringLiteral("Intro.\nNew line.\nOutro.\n")));
    const QJsonObject snapshot{{QStringLiteral("proposal_id"), QStringLiteral("agent-1")}, {QStringLiteral("source_file"), Relative},
                               {QStringLiteral("comment"), QStringLiteral("Clarify.\n")}, {QStringLiteral("before"), QStringLiteral("Old line.\n")},
                               {QStringLiteral("after"), QStringLiteral("New line.\n")}, {QStringLiteral("created_at"), QStringLiteral("2026-10-10T12:00:00.000Z")},
                               {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")}};
    const QJsonObject row = removedRow(snapshot, 2);
    QJsonObject result = ProposalDecisions::restoreRemovedProposal(workspace.root(), row);
    QVERIFY2(result.value(QStringLiteral("success")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("line")).toInt(), 2);
    QCOMPARE(read(workspace.source()), Base.toUtf8());
    // Again: already there, nothing written.
    result = ProposalDecisions::restoreRemovedProposal(workspace.root(), row);
    QVERIFY(result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("already_present"));
    // Both texts edited: refused with the location, the file is unchanged.
    QVERIFY(write(workspace.source(), "Intro.\nSomething else.\nOutro.\n"));
    result = ProposalDecisions::restoreRemovedProposal(workspace.root(), row);
    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("text_changed"));
    QVERIFY(result.value(QStringLiteral("message")).toString().contains(Relative + QStringLiteral(":2")));
    QCOMPARE(read(workspace.source()), QByteArray("Intro.\nSomething else.\nOutro.\n"));
    // Not a removed row (e.g. dismissed elsewhere): nothing to restore.
    QJsonObject dismissed = ProposalDecisions::dismissedPayload(row);
    QVERIFY(!ProposalDecisions::restoreRemovedProposal(workspace.root(), dismissed).value(QStringLiteral("success")).toBool());
    // A path escaping the Library is refused.
    QVERIFY(!ProposalDecisions::restoreRemovedProposal(workspace.root(), removedRow(snapshot, 2, QStringLiteral("../a.tex"))).value(QStringLiteral("success")).toBool());
    // The file is gone.
    QVERIFY(QFile::remove(workspace.source()));
    QCOMPARE(ProposalDecisions::restoreRemovedProposal(workspace.root(), row).value(QStringLiteral("code")).toString(), QStringLiteral("source_missing"));
}

void ProposalDecisionsTest::testDismissPayload()
{
    const QJsonObject row = removedRow({{QStringLiteral("proposal_id"), QStringLiteral("agent-1")}}, 3);
    QVERIFY(ProposalDecisions::isRemovedOutside(row));
    const QJsonObject payload = ProposalDecisions::dismissedPayload(row);
    QCOMPARE(payload.value(QStringLiteral("status")).toString(), QStringLiteral("dismissed"));
    QCOMPARE(payload.value(QStringLiteral("kind")).toString(), QStringLiteral("dismissed"));
    QVERIFY(payload.value(QStringLiteral("deleted")).toBool());
    QCOMPARE(payload.value(QStringLiteral("snapshot")), row.value(QStringLiteral("snapshot")));
    QVERIFY(!ProposalDecisions::isRemovedOutside(payload));
    QVERIFY(ProposalDecisions::copyableText(row).startsWith(QStringLiteral("Proposal agent-1 in ") + Relative + QStringLiteral(":3")));
}

QTEST_GUILESS_MAIN(ProposalDecisionsTest)
#include "proposaldecisionstest.moc"
