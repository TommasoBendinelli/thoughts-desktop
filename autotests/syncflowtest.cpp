// SPDX-License-Identifier: GPL-2.0-or-later
// The Sync state machine of FedoraRepository against a scripted local server:
// what a Sync does while the server is still building a PDF, how the end of
// that build starts the Sync that fetches the new PDF, and which failures are
// retried.
#include "../part/desktopsyncstore.h"
#include "../part/fedorarepository.h"
#include "../part/sourcechangetracker.h"

#include <thoughts/syncclient.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <functional>
#include <memory>

namespace
{

struct Request {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct Response {
    int status = 200;
    QByteArray body;
    // Answer only after this long (a long poll), or never when negative.
    int delayMs = 0;
    QByteArray contentType = QByteArrayLiteral("application/json");
    // Close the connection without any answer.
    bool drop = false;
    QHash<QByteArray, QByteArray> headers;
};

Response json(const QJsonObject &value, int status = 200, int delayMs = 0)
{
    Response response;
    response.status = status;
    response.body = QJsonDocument(value).toJson(QJsonDocument::Compact);
    response.delayMs = delayMs;
    return response;
}

// One request per connection, answered by the handler of the moment.
class ScriptedServer : public QObject
{
public:
    using Handler = std::function<Response(const Request &)>;

    ScriptedServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        m_listening = m_server.listen(QHostAddress::LocalHost);
    }

    bool listening() const
    {
        return m_listening;
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }

    void setHandler(Handler handler)
    {
        m_handler = std::move(handler);
    }

    const QList<Request> &requests() const
    {
        return m_requests;
    }

    int count(const QByteArray &method, const QByteArray &targetPrefix, int from = 0) const
    {
        int found = 0;
        for (qsizetype index = from; index < m_requests.size(); ++index) {
            found += m_requests.at(index).method == method && m_requests.at(index).target.startsWith(targetPrefix) ? 1 : 0;
        }
        return found;
    }

private:
    void consume(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer.append(socket->readAll());
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        Request request;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        request.method = requestLine.value(0);
        request.target = requestLine.value(1);
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const QByteArray line = lines.at(index).trimmed();
            const qsizetype separator = line.indexOf(':');
            if (separator > 0) {
                request.headers.insert(line.left(separator).trimmed().toLower(), line.mid(separator + 1).trimmed());
            }
        }
        const qsizetype length = request.headers.value("content-length").toLongLong();
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        request.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        m_requests.append(request);
        const Response response = m_handler ? m_handler(request) : json({{QStringLiteral("detail"), QStringLiteral("no handler")}}, 500);
        const QPointer<QTcpSocket> guarded(socket);
        const auto send = [guarded, response] {
            if (!guarded) {
                return;
            }
            if (!response.drop) {
                QByteArray encoded = "HTTP/1.1 " + QByteArray::number(response.status) + " Scripted\r\nContent-Type: " + response.contentType + "\r\nContent-Length: "
                    + QByteArray::number(response.body.size()) + "\r\n";
                for (auto header = response.headers.cbegin(); header != response.headers.cend(); ++header) {
                    encoded += header.key() + ": " + header.value() + "\r\n";
                }
                guarded->write(encoded + "Connection: close\r\n\r\n" + response.body);
                guarded->flush();
            }
            guarded->disconnectFromHost();
        };
        if (response.delayMs < 0) {
            return;
        }
        if (response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, this, send);
        } else {
            send();
        }
    }

    QTcpServer m_server;
    bool m_listening = false;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<Request> m_requests;
    Handler m_handler;
};

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

const QString revisionA(64, QLatin1Char('a'));
const QString revisionB(64, QLatin1Char('b'));
const QString revisionC(64, QLatin1Char('c'));

QJsonObject serverState(qint64 sequence, const QString &revision, qint64 sourceFrontier = 7)
{
    return {{QStringLiteral("contract_version"), 2}, {QStringLiteral("latest_server_sequence"), sequence}, {QStringLiteral("library_revision"), revision}, {QStringLiteral("source_frontier"), sourceFrontier}};
}

QJsonObject buildRecord(const QString &id, const QString &status, const QString &target = QStringLiteral("paper.pdf"))
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("target"), target}, {QStringLiteral("status"), status}, {QStringLiteral("trace_id"), QStringLiteral("0190a000-0000-7000-8000-00000000000b")},
            {QStringLiteral("updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}

}

namespace Okular
{

class SyncFlowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void testSyncEndsWhileTheServerBuilds();
    void testBuildEndingAfterSyncStartsTheFollowUpSync();
    void testBuildFailingAfterSyncMarksTheDocumentOnly();
    void testRenderCheckFailureIsAWarningOfASucceededSync();
    void testDocumentSyncStillWaitsForItsBuild();
    void testVerifiedPublicationNeedsNoSecondInstallerPass_data();
    void testVerifiedPublicationNeedsNoSecondInstallerPass();
    void testRemoteWorkCheckLooksAtTheFilesOffTheGuiThread();
    void testTransientFailuresAreAskedAgain();
    void testLibraryThatKeepsChangingIsPlannedAgain();
    void testFailuresHaveAKind_data();
    void testFailuresHaveAKind();
    void testMissingTraceStreamAsksAboutTheBuildAtOnce_data();
    void testMissingTraceStreamAsksAboutTheBuildAtOnce();

private:
    // A Library that is in sync with the scripted server at sequence 4,
    // Library revision aaaa… and source frontier 7.
    void prepareSyncedLibrary();
    bool runSync(const QString &traceId, int timeoutMs = 8000);
    QString failure() const;

    std::unique_ptr<QTemporaryDir> m_docs;
    std::unique_ptr<QTemporaryDir> m_state;
    std::unique_ptr<ScriptedServer> m_server;
    // The process-wide repository: only some of its members are exported, as for parttest.
    FedoraRepository *m_repository = nullptr;
};

void SyncFlowTest::init()
{
    m_docs = std::make_unique<QTemporaryDir>();
    m_state = std::make_unique<QTemporaryDir>();
    QVERIFY(m_docs->isValid() && m_state->isValid());
    m_server = std::make_unique<ScriptedServer>();
    QVERIFY(m_server->listening());
    m_repository = FedoraRepository::instance(m_docs->path());
    prepareSyncedLibrary();
}

void SyncFlowTest::cleanup()
{
    if (m_repository) {
        m_repository->m_client.reset();
        m_repository->m_sourceChangeTracker.reset();
        m_repository->m_desktopSyncStore.reset();
        m_repository->m_syncCoordinator.reset();
        m_repository->m_pendingBuildAnnouncementDue = false;
        m_repository->m_pendingBuildConfirmedAtMs = -1;
        m_repository->m_pendingBuildWatchFailures = 0;
        // Let queued timers of the finished Sync run out against no client.
        QTest::qWait(50);
    }
    m_server.reset();
    m_state.reset();
    m_docs.reset();
}

void SyncFlowTest::prepareSyncedLibrary()
{
    FedoraRepository *const repository = m_repository;
    const QString docs = m_docs->path();
    const QString state = m_state->path();
    repository->m_docsRoot = docs;
    repository->m_stateRoot = state;
    repository->m_outboxPath = QDir(state).filePath(QStringLiteral("outbox.json"));
    repository->m_outbox = std::make_unique<Thoughts::DesktopSyncOutbox>(QDir(state).filePath(QStringLiteral("desktop-sync.sqlite")), repository->m_outboxPath);
    repository->m_outboxReady = false;
    repository->m_cachedRepositoryStateValid = false;
    repository->m_syncCoordinator.reset();
    repository->m_renderStatuses.clear();
    // The pauses before a repeated request are 1, 2 and 4 s in the app.
    repository->m_retryDelaysMs = {20, 40, 80};
    repository->m_readerReplanDelayScale = 0;

    repository->m_sourceManifestPath = QDir(docs).filePath(QStringLiteral("source-manifest.json"));
    const QByteArray manifestBytes = QByteArrayLiteral("{\"projects\":[{\"files\":[{\"path\":\"source.tex\",\"role\":\"source\"}],\"project\":\"docs-paper\",\"targets\":[\"paper.pdf\"]}],\"schema_version\":3,\"shared_files\":[]}\n");
    QVERIFY(writeFile(repository->m_sourceManifestPath, manifestBytes));
    QVERIFY(QDir().mkpath(QDir(docs).filePath(QStringLiteral("current_pdfs"))));
    QVERIFY(QDir().mkpath(QDir(docs).filePath(QStringLiteral("docs-paper"))));
    const QJsonObject catalogue {{QStringLiteral("projects"),
                                  QJsonArray {QJsonObject {{QStringLiteral("id"), QStringLiteral("source:catalogue")},
                                                           {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}}}}}}}};
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral(".thoughts/library-projects.json")), QJsonDocument(catalogue).toJson()));
    // The PDF this computer has: it must survive every Sync that leaves a build running.
    QVERIFY(writeFile(QDir(docs).filePath(QStringLiteral("current_pdfs/paper.pdf")), QByteArrayLiteral("the-pdf-before-the-build")));

    const QByteArray sourceBytes = QByteArrayLiteral("source\n");
    const QString sourcePath = QDir(docs).filePath(QStringLiteral("source.tex"));
    QVERIFY(writeFile(sourcePath, sourceBytes));
    const QFileInfo sourceInfo(sourcePath);
    Thoughts::DesktopTrackedPath baseline;
    baseline.path = QStringLiteral("source.tex");
    baseline.exists = true;
    baseline.sizeBytes = sourceInfo.size();
    baseline.modifiedAtMs = sourceInfo.lastModified().toMSecsSinceEpoch();
    baseline.sha256 = QString::fromLatin1(QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex());
    baseline.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("role"), QStringLiteral("source")}};
    baseline.affectedTargets = {QStringLiteral("paper.pdf")};

    QString error;
    repository->m_desktopSyncStorePath = QDir(state).filePath(QStringLiteral("desktop-sync.sqlite"));
    repository->m_desktopSyncStore = std::make_unique<Thoughts::DesktopSyncStore>(repository->m_desktopSyncStorePath);
    QVERIFY2(repository->m_desktopSyncStore->open(&error), qPrintable(error));
    Thoughts::DesktopSyncStore *const store = repository->m_desktopSyncStore.get();
    QVERIFY2(store->replaceBaselines({baseline}, &error), qPrintable(error));
    repository->m_sourceChangeTracker = std::make_unique<Thoughts::SourceChangeTracker>(docs, store, repository);
    repository->m_sourceChangeTrackerRoot = docs;
    QVERIFY2(repository->m_sourceChangeTracker->setTrackedPaths({baseline}, &error), qPrintable(error));
    QVERIFY2(repository->m_sourceChangeTracker->reconcileNow(&error), qPrintable(error));

    Thoughts::DesktopSyncFrontier frontier;
    frontier.libraryRevision = revisionA;
    frontier.sourceFrontier = 7;
    frontier.cursor = 0;
    frontier.metadata.insert(QStringLiteral("materialized_source_frontier"), 7);
    frontier.metadata.insert(QStringLiteral("reader_cache_revision"), revisionA);
    frontier.metadata.insert(QStringLiteral("reader_cache_objects"), QJsonArray());
    const QFileInfo manifestInfo(repository->m_sourceManifestPath);
    frontier.metadata.insert(QStringLiteral("source_manifest"),
                             QJsonObject {{QStringLiteral("exists"), true},
                                          {QStringLiteral("size_bytes"), manifestInfo.size()},
                                          {QStringLiteral("modified_at_ms"), manifestInfo.lastModified().toMSecsSinceEpoch()},
                                          {QStringLiteral("metadata_changed_at_ms"), manifestInfo.metadataChangeTime().toMSecsSinceEpoch()},
                                          {QStringLiteral("created_at_ms"), manifestInfo.birthTime().toMSecsSinceEpoch()},
                                          {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex())},
                                          {QStringLiteral("project_sources_v1"), true}});
    frontier.metadata.insert(QStringLiteral("materialized_source_manifest"), frontier.metadata.value(QStringLiteral("source_manifest")));
    QVERIFY2(store->setFrontier(frontier, &error), qPrintable(error));
    QVERIFY(repository->m_outbox->advanceCursor(0, 4));

    const QJsonObject localState {{QStringLiteral("bootstrapped"), true}, {QStringLiteral("active_library_revision"), revisionA}};
    QVERIFY(writeFile(QDir(state).filePath(QStringLiteral("repository.json")), QJsonDocument(localState).toJson(QJsonDocument::Compact)));
    repository->m_cachedRepositoryStateValid = false;
    // The queued start-up work of the repository runs against these paths.
    QTest::qWait(50);
    repository->m_client = std::make_unique<Thoughts::SyncClient>(m_server->baseUrl(), QByteArrayLiteral("token"), repository);
}

bool SyncFlowTest::runSync(const QString &traceId, int timeoutMs)
{
    m_repository->syncNow(traceId);
    return QTest::qWaitFor([this] { return m_repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded || m_repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Failed; }, timeoutMs);
}

QString SyncFlowTest::failure() const
{
    return QStringLiteral("state %1, phase %2: %3 / %4")
        .arg(static_cast<int>(m_repository->m_syncCoordinator.state))
        .arg(static_cast<int>(m_repository->m_syncCoordinator.phase))
        .arg(m_repository->m_syncCoordinator.message, m_repository->m_syncCoordinator.details);
}

// The render check finds the server building paper.pdf. The Sync ends
// succeeded without asking about the build, keeps the PDF, marks the document
// as updating and records the build.
void SyncFlowTest::testSyncEndsWhileTheServerBuilds()
{
    FedoraRepository *const repository = m_repository;
    repository->m_pendingBuildWaitMs = 1000;
    QString protocolError;
    bool askedAboutBuildWhileSyncing = false;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.method == "GET" && request.target == "/v2/sync/state") return json(serverState(4, revisionA));
        if (request.method == "POST" && request.target == "/v1/render-checks") {
            return json({{QStringLiteral("status"), QStringLiteral("building")},
                         {QStringLiteral("library_revision"), 7},
                         {QStringLiteral("build"), buildRecord(QStringLiteral("build-one"), QStringLiteral("running"))},
                         {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("state"), QStringLiteral("building")}, {QStringLiteral("reason"), QStringLiteral("render inputs changed")}}}}});
        }
        // The watch of the pending build, after the Sync: still running.
        if (request.method == "GET" && request.target.startsWith("/v2/builds/build-one")) {
            askedAboutBuildWhileSyncing = askedAboutBuildWhileSyncing || repository->m_syncCoordinator.running();
            return json(buildRecord(QStringLiteral("build-one"), QStringLiteral("running")), 200, 400);
        }
        protocolError = QStringLiteral("Unexpected request %1 %2").arg(QString::fromLatin1(request.method), QString::fromLatin1(request.target));
        return json({{QStringLiteral("detail"), protocolError}}, 500);
    });
    QSignalSpy journal(repository, SIGNAL(syncJournalEvent(QString,QJsonObject)));
    QSignalSpy states(repository, SIGNAL(manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState,QString,QString)));

    QElapsedTimer clock;
    clock.start();
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000000a")), qPrintable(failure()));
    QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure() + protocolError));
    QVERIFY2(protocolError.isEmpty(), qPrintable(protocolError));
    // The Sync itself never asked about the build, and never waited for it.
    QVERIFY(!askedAboutBuildWhileSyncing);
    QCOMPARE(m_server->count("POST", "/v1/render-checks"), 1);
    QVERIFY(repository->manualSyncWarningCode().isEmpty());
    QVERIFY(repository->manualSyncErrorCode().isEmpty());

    // The build is recorded for the watch, not as a build this computer follows.
    const QJsonArray pending = repository->pendingServerBuilds();
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("build-one"));
    QCOMPARE(pending.first().toObject().value(QStringLiteral("documents")).toArray(), QJsonArray {QStringLiteral("paper.pdf")});
    const QJsonObject stored = repository->readRepositoryState();
    QVERIFY(stored.value(QStringLiteral("active_builds")).toArray().isEmpty());
    QVERIFY(stored.value(QStringLiteral("build_runs")).toObject().isEmpty());
    QVERIFY(!repository->buildActivity(QStringLiteral("paper.pdf")).value(QStringLiteral("active")).toBool());

    // The document says an update is on its way; its PDF is untouched.
    const QJsonObject status = repository->renderStatus(QStringLiteral("paper.pdf"));
    QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("building"));
    QVERIFY(status.value(QStringLiteral("reason")).toString().contains(QStringLiteral("The server is updating this PDF")));
    QCOMPARE(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("build-one"));
    QFile pdf(QDir(m_docs->path()).filePath(QStringLiteral("current_pdfs/paper.pdf")));
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    QCOMPARE(pdf.readAll(), QByteArrayLiteral("the-pdf-before-the-build"));

    // The Sync summary names the PDF still building.
    QJsonObject completion;
    for (const QList<QVariant> &event : journal) {
        const QJsonObject metadata = event.at(1).toJsonObject().value(QStringLiteral("metadata")).toObject();
        if (metadata.contains(QStringLiteral("sync_outcome"))) completion = metadata;
    }
    QCOMPARE(completion.value(QStringLiteral("sync_outcome")).toString(), QStringLiteral("changes_synced"));
    QCOMPARE(completion.value(QStringLiteral("pending_build_documents")).toArray(), QJsonArray {QStringLiteral("paper.pdf")});
    QVERIFY(completion.value(QStringLiteral("failed_documents")).toArray().isEmpty());

    // After the Sync one request follows the build in the background.
    QTRY_VERIFY_WITH_TIMEOUT(m_server->count("GET", "/v2/builds/build-one?wait_ms=1000") >= 1, 3000);
    QVERIFY(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded);

    // While the build runs, its progress changes the Library revision. That alone is no
    // work for a Sync (it would only find the build still running); a new operation is.
    const auto ask = [&](const QJsonObject &state, bool *pendingWork, QString *key) {
        m_server->setHandler([&, state](const Request &request) {
            if (request.target == "/v2/sync/state") return json(state);
            return json(buildRecord(QStringLiteral("build-one"), QStringLiteral("running")), 200, 400);
        });
        bool done = false;
        repository->checkRemoteWork([&](bool p, const QString &k) {
            done = true;
            *pendingWork = p;
            *key = k;
        });
        return QTest::qWaitFor([&] { return done; }, 3000);
    };
    bool pendingWork = true;
    QString key;
    QVERIFY(ask(serverState(4, revisionB), &pendingWork, &key));
    QVERIFY(!pendingWork);
    QCOMPARE(key, QStringLiteral("4/%1/7").arg(revisionB));
    QVERIFY(ask(serverState(5, revisionB), &pendingWork, &key));
    QVERIFY(pendingWork);
    // A watch that no longer hears from the server does not hold back Sync.
    repository->m_pendingBuildConfirmedAtMs = QDateTime::currentMSecsSinceEpoch() - 10 * 60 * 1000;
    repository->m_client = std::make_unique<Thoughts::SyncClient>(m_server->baseUrl(), QByteArrayLiteral("token"), repository);
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v2/sync/state") return json(serverState(4, revisionB));
        Response never;
        never.delayMs = -1;
        return never;
    });
    bool done = false;
    repository->evaluateRemoteWork(true, [&](bool p, const QString &) {
        done = true;
        pendingWork = p;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY(pendingWork);
}

// The build ends after the Sync. The held request notices it at once, the
// repository announces the work (as the periodic check would, up to 20 s
// later), and the Sync started for it asks for the new Library.
void SyncFlowTest::testBuildEndingAfterSyncStartsTheFollowUpSync()
{
    FedoraRepository *const repository = m_repository;
    repository->m_pendingBuildWaitMs = 5000;
    bool buildEnded = false;
    QString protocolError;
    QJsonObject planRequest;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.method == "GET" && request.target == "/v2/sync/state") return json(serverState(4, buildEnded ? revisionC : revisionA));
        if (request.method == "POST" && request.target == "/v1/render-checks") {
            if (buildEnded) {
                return json({{QStringLiteral("status"), QStringLiteral("current")}, {QStringLiteral("library_revision"), 7}, {QStringLiteral("documents"), QJsonArray()}});
            }
            return json({{QStringLiteral("status"), QStringLiteral("building")},
                         {QStringLiteral("library_revision"), 7},
                         {QStringLiteral("build"), buildRecord(QStringLiteral("build-two"), QStringLiteral("queued"))},
                         {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("state"), QStringLiteral("queued")}}}}});
        }
        if (request.method == "GET" && request.target.startsWith("/v2/builds/build-two")) {
            // The server answers the long poll when the build ends, here 600 ms later.
            buildEnded = true;
            return json(buildRecord(QStringLiteral("build-two"), QStringLiteral("succeeded")), 200, 600);
        }
        if (request.method == "POST" && request.target == "/v2/sync/plan") {
            planRequest = QJsonDocument::fromJson(request.body).object();
            return json({{QStringLiteral("detail"), QStringLiteral("the scripted server stops the follow-up Sync here")}}, 503);
        }
        protocolError = QStringLiteral("Unexpected request %1 %2").arg(QString::fromLatin1(request.method), QString::fromLatin1(request.target));
        return json({{QStringLiteral("detail"), protocolError}}, 500);
    });
    QSignalSpy found(repository, SIGNAL(remoteWorkFound(bool,QString)));

    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000000c")), qPrintable(failure()));
    QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure() + protocolError));
    QCOMPARE(repository->pendingServerBuilds().size(), 1);
    QElapsedTimer sinceSync;
    sinceSync.start();

    // The end of the build is announced well before the 20 s periodic check, once.
    QTRY_VERIFY_WITH_TIMEOUT(!found.isEmpty(), 5000);
    QVERIFY2(sinceSync.elapsed() < 4000, qPrintable(QString::number(sinceSync.elapsed())));
    QCOMPARE(found.first().at(0).toBool(), true);
    QCOMPARE(found.first().at(1).toString(), QStringLiteral("4/%1/7").arg(revisionC));
    QVERIFY(repository->pendingServerBuilds().isEmpty());
    // Until that Sync has run the document waits for Sync; it is no longer "updating".
    QCOMPARE(repository->renderStatus(QStringLiteral("paper.pdf")).value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    QCOMPARE(repository->renderStatus(QStringLiteral("paper.pdf")).value(QStringLiteral("local_state")).toString(), QStringLiteral("sync_pending"));
    QTest::qWait(300);
    QCOMPARE(found.size(), 1);
    QVERIFY2(protocolError.isEmpty(), qPrintable(protocolError));
    // The same question the periodic check asks now also says "work", under the same key.
    bool done = false;
    bool pendingWork = false;
    QString key;
    repository->checkRemoteWork([&](bool p, const QString &k) {
        done = true;
        pendingWork = p;
        key = k;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY(pendingWork);
    QCOMPARE(key, found.first().at(1).toString());

    // The Sync the shell starts for that announcement checks the PDFs (now current) and
    // goes on to download the new Library generation: it asks for a reader plan from
    // the revision this computer has.
    const int checksBefore = m_server->count("POST", "/v1/render-checks");
    QVERIFY(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000000d")));
    QCOMPARE(m_server->count("POST", "/v1/render-checks"), checksBefore + 1);
    QCOMPARE(planRequest.value(QStringLiteral("library_revision")).toString(), revisionA);
    QCOMPARE(planRequest.value(QStringLiteral("contract_version")).toInt(), 2);
    QVERIFY(repository->pendingServerBuilds().isEmpty());
    QVERIFY2(protocolError.isEmpty(), qPrintable(protocolError));
}

// The build fails after the Sync that left it running. That Sync stays
// succeeded; the document reports the failed build with the build record
// that the build window reads.
void SyncFlowTest::testBuildFailingAfterSyncMarksTheDocumentOnly()
{
    FedoraRepository *const repository = m_repository;
    repository->m_pendingBuildWaitMs = 5000;
    bool buildEnded = false;
    QJsonObject failedBuild = buildRecord(QStringLiteral("build-three"), QStringLiteral("attention"), QStringLiteral("all"));
    failedBuild.insert(QStringLiteral("result"),
                       QJsonObject {{QStringLiteral("failed_documents"), QJsonArray {QJsonObject {{QStringLiteral("pdf_filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("message"), QStringLiteral("latex_error: Undefined control sequence")}}}}});
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") return json(serverState(4, buildEnded ? revisionC : revisionA));
        if (request.target == "/v1/render-checks") {
            return json({{QStringLiteral("status"), QStringLiteral("building")},
                         {QStringLiteral("library_revision"), 7},
                         {QStringLiteral("build"), buildRecord(QStringLiteral("build-three"), QStringLiteral("running"), QStringLiteral("all"))},
                         {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("state"), QStringLiteral("building")}}}}});
        }
        if (request.target.startsWith("/v2/builds/build-three")) {
            buildEnded = true;
            return json(failedBuild, 200, 300);
        }
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QSignalSpy states(repository, SIGNAL(manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState,QString,QString)));
    QSignalSpy statuses(repository, SIGNAL(renderStatusChanged(QString,QJsonObject)));
    QSignalSpy found(repository, SIGNAL(remoteWorkFound(bool,QString)));
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000000e")), qPrintable(failure()));
    QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure()));
    const int stateChanges = states.size();

    QTRY_VERIFY_WITH_TIMEOUT(repository->renderStatus(QStringLiteral("paper.pdf")).value(QStringLiteral("status")).toString() == QLatin1String("attention"), 5000);
    const QJsonObject status = repository->renderStatus(QStringLiteral("paper.pdf"));
    QCOMPARE(status.value(QStringLiteral("reason")).toString(), QStringLiteral("latex_error: Undefined control sequence"));
    QCOMPARE(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("build-three"));
    QVERIFY(repository->pendingServerBuilds().isEmpty());
    // The finished Sync is not turned into a failed one.
    QVERIFY(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded);
    QCOMPARE(states.size(), stateChanges);
    // The server's Library changed (the document's status): one Sync is announced for it.
    QTRY_COMPARE_WITH_TIMEOUT(found.size(), 1, 3000);
}

// The render check itself answers that a PDF could not be built. The changes
// arrived, so the Sync succeeds; the failure is its warning and is listed for
// the Sync summary.
void SyncFlowTest::testRenderCheckFailureIsAWarningOfASucceededSync()
{
    FedoraRepository *const repository = m_repository;
    QJsonObject failedBuild = buildRecord(QStringLiteral("build-four"), QStringLiteral("failed"));
    failedBuild.insert(QStringLiteral("error"), QStringLiteral("latexmk exit 12"));
    int renderStateRequests = 0;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") return json(serverState(4, revisionA));
        if (request.target == "/v1/render-checks") {
            return json({{QStringLiteral("status"), QStringLiteral("attention")},
                         {QStringLiteral("library_revision"), 7},
                         {QStringLiteral("build"), failedBuild},
                         {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("state"), QStringLiteral("attention")}, {QStringLiteral("reason"), QStringLiteral("The latest build needs attention for this PDF.")}}}}});
        }
        if (request.target.startsWith("/v1/render-state")) {
            ++renderStateRequests;
            return json({{QStringLiteral("detail"), QStringLiteral("not scripted")}}, 500);
        }
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QSignalSpy journal(repository, SIGNAL(syncJournalEvent(QString,QJsonObject)));
    QSignalSpy states(repository, SIGNAL(manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState,QString,QString)));
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000000f")), qPrintable(failure()));
    QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure()));
    QVERIFY(repository->manualSyncErrorCode().isEmpty());
    QCOMPARE(repository->manualSyncWarningCode(), QStringLiteral("pdf_build_failed"));
    QVERIFY(repository->manualSyncWarning().startsWith(QStringLiteral("PDF build failed")));
    QVERIFY(repository->manualSyncWarning().contains(QStringLiteral("latexmk exit 12")));
    QVERIFY(repository->manualSyncMessage().isEmpty());
    QCOMPARE(states.last().at(0).toInt(), static_cast<int>(FedoraRepository::ManualSyncState::Succeeded));

    QJsonObject completion;
    for (const QList<QVariant> &event : journal) {
        const QJsonObject metadata = event.at(1).toJsonObject().value(QStringLiteral("metadata")).toObject();
        if (metadata.contains(QStringLiteral("sync_outcome"))) completion = metadata;
    }
    QCOMPARE(completion.value(QStringLiteral("sync_outcome")).toString(), QStringLiteral("changes_synced"));
    const QJsonArray failed = completion.value(QStringLiteral("failed_documents")).toArray();
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.first().toObject().value(QStringLiteral("filename")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(failed.first().toObject().value(QStringLiteral("build_id")).toString(), QStringLiteral("build-four"));
    // The documents' statuses are read again, so the failed one says so.
    QTRY_VERIFY_WITH_TIMEOUT(renderStateRequests >= 1, 3000);
    // A new Sync starts without the previous warning.
    QVERIFY(repository->pendingServerBuilds().isEmpty());
}

// A Sync of one PDF is an explicit request for that PDF: it still waits for
// the build, exactly as before.
void SyncFlowTest::testDocumentSyncStillWaitsForItsBuild()
{
    FedoraRepository *const repository = m_repository;
    repository->m_syncCoordinator.state = FedoraRepository::ManualSyncState::Running;
    repository->m_syncCoordinator.phase = FedoraRepository::ManualSyncPhase::RenderCheck;
    repository->m_syncCoordinator.setTarget(QStringLiteral("paper.pdf"));
    repository->m_activeTraceId = QStringLiteral("0190a000-0000-7000-8000-00000000001a");
    repository->m_syncCoordinator.renderCheckRequestId = repository->m_activeTraceId;
    repository->m_pollingBuild = true; // No request leaves this test.
    const QJsonObject response {{QStringLiteral("status"), QStringLiteral("building")},
                                {QStringLiteral("library_revision"), 7},
                                {QStringLiteral("build"), buildRecord(QStringLiteral("build-five"), QStringLiteral("queued"))},
                                {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("state"), QStringLiteral("queued")}}}}};
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(repository, "applyManualRenderCheckResponse", Qt::DirectConnection, Q_RETURN_ARG(bool, applied), Q_ARG(QJsonObject, response)));
    QVERIFY(applied);
    QCOMPARE(repository->m_syncCoordinator.phase, FedoraRepository::ManualSyncPhase::WaitBuild);
    const QJsonObject stored = repository->readRepositoryState();
    QCOMPARE(stored.value(QStringLiteral("active_builds")).toArray().size(), 1);
    QCOMPARE(stored.value(QStringLiteral("build_runs")).toObject().value(QStringLiteral("paper.pdf")).toObject().value(QStringLiteral("sync_origin")).toString(), QStringLiteral("manual_conditional"));
    QVERIFY(repository->pendingServerBuilds().isEmpty());
    repository->m_pollingBuild = false;
    repository->m_syncCoordinator.reset();
}

void SyncFlowTest::testVerifiedPublicationNeedsNoSecondInstallerPass_data()
{
    QTest::addColumn<QJsonObject>("publication");
    QTest::addColumn<bool>("manifestChanged");
    QTest::addColumn<bool>("installerSkipped");
    const auto verified = [](qint64 from, qint64 through) {
        return QJsonObject {{QStringLiteral("source_frontier"), 9}, {QStringLiteral("source_tree_verified"), QJsonObject {{QStringLiteral("from"), from}, {QStringLiteral("through"), through}}}};
    };
    QTest::newRow("only this publication changed the server") << verified(7, 9) << false << true;
    QTest::newRow("the publisher could not verify the tree") << QJsonObject {{QStringLiteral("source_frontier"), 9}} << false << false;
    QTest::newRow("verified from another frontier than the installer checked") << verified(8, 9) << false << false;
    QTest::newRow("the publication changed the manifest") << verified(7, 9) << true << false;
}

// After its own publication (source frontier 7 -> 9) the Sync asks the server's state
// again. It used to run the editable-source installer a second time there. When the
// publisher verified that only its publication changed the server's sources, the
// installer's earlier check holds through 9 and the pass is skipped; otherwise it runs.
void SyncFlowTest::testVerifiedPublicationNeedsNoSecondInstallerPass()
{
    QFETCH(QJsonObject, publication);
    QFETCH(bool, manifestChanged);
    QFETCH(bool, installerSkipped);
    FedoraRepository *const repository = m_repository;
    Thoughts::DesktopSyncStore *const store = repository->m_desktopSyncStore.get();
    QString error;
    if (manifestChanged) {
        QFile manifest(repository->m_sourceManifestPath);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QByteArray bytes = manifest.readAll();
        manifest.close();
        bytes.replace("\"shared_files\":[]", "\"shared_files\":[{\"path\":\"tools/shared.sty\",\"role\":\"shared_style\"}]");
        QVERIFY(writeFile(repository->m_sourceManifestPath, bytes));
    }
    // What the installer left at the start of the Sync, and the accepted publication.
    repository->m_syncCoordinator.authoringCheckedSourceFrontier = 7;
    bool noted = QMetaObject::invokeMethod(repository, "noteVerifiedSourcePublication", Qt::DirectConnection, Q_ARG(QJsonObject, publication));
    QVERIFY(noted);
    QCOMPARE(repository->m_syncCoordinator.authoringCheckedSourceFrontier, installerSkipped ? 9 : 7);
    Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(frontier.metadata.value(QStringLiteral("materialized_source_frontier")).toInteger(), installerSkipped ? 9 : 7);
    repository->m_syncCoordinator.reset();
    if (manifestChanged) {
        // The installer's check of the old manifest is not carried over; what the Sync
        // does next with a changed manifest is as before.
        return;
    }

    // The next look at the server (frontier 9) decides whether the installer runs. It is
    // replaced here by a program that only leaves a mark.
    frontier.sourceFrontier = 9;
    QVERIFY2(store->setFrontier(frontier, &error), qPrintable(error));
    const QString mark = QDir(m_state->path()).filePath(QStringLiteral("installer-ran"));
    const QString program = QDir(m_state->path()).filePath(QStringLiteral("installer.sh"));
    QVERIFY(writeFile(program, QByteArrayLiteral("#!/bin/sh\n: > \"") + QFile::encodeName(mark) + QByteArrayLiteral("\"\nexit 3\n")));
    QVERIFY(QFile::setPermissions(program, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    repository->m_sourceReconciliationProgram = program;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") return json(serverState(4, revisionA, 9));
        if (request.target == "/v1/render-checks") return json({{QStringLiteral("status"), QStringLiteral("current")}, {QStringLiteral("library_revision"), 9}, {QStringLiteral("documents"), QJsonArray()}});
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000003a")), qPrintable(failure()));
    repository->m_sourceReconciliationProgram.clear();
    QCOMPARE(!QFileInfo::exists(mark), installerSkipped);
    if (installerSkipped) {
        QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure()));
    }
}

// The periodic check (every 20 s) compares each cached Library object with its file. It
// reads the recorded facts in one query on the GUI thread and looks at the files on a
// worker; the answers are those of the synchronous check Sync itself uses.
void SyncFlowTest::testRemoteWorkCheckLooksAtTheFilesOffTheGuiThread()
{
    FedoraRepository *const repository = m_repository;
    Thoughts::DesktopSyncStore *const store = repository->m_desktopSyncStore.get();
    QString error;
    // Two verified objects of the active Library generation.
    QJsonArray cached;
    QStringList paths;
    for (int index = 0; index < 2; ++index) {
        const QString path = QDir(m_docs->path()).filePath(QStringLiteral("current_pdfs/cached-%1.pdf").arg(index));
        QVERIFY(writeFile(path, QByteArrayLiteral("verified-object-") + QByteArray::number(index)));
        const QFileInfo info(path);
        Thoughts::DesktopVerifiedObject object;
        object.sha256 = QString(64, QLatin1Char(index == 0 ? 'c' : 'd'));
        object.sizeBytes = info.size();
        object.kind = QStringLiteral("pdf");
        object.semanticRole = QStringLiteral("pdf");
        object.verifiedAtMs = QDateTime::currentMSecsSinceEpoch();
        object.metadata = QJsonObject {{QStringLiteral("local_path"), info.absoluteFilePath()},
                                       {QStringLiteral("size_bytes"), info.size()},
                                       {QStringLiteral("modified_at_ms"), info.lastModified().toMSecsSinceEpoch()},
                                       {QStringLiteral("metadata_changed_at_ms"), info.metadataChangeTime().toMSecsSinceEpoch()},
                                       {QStringLiteral("created_at_ms"), info.birthTime().toMSecsSinceEpoch()}};
        QVERIFY2(store->upsertVerifiedObject(object, &error), qPrintable(error));
        cached.append(object.sha256);
        paths.append(path);
    }
    Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    frontier.metadata.insert(QStringLiteral("reader_cache_objects"), cached);
    QVERIFY2(store->setFrontier(frontier, &error), qPrintable(error));

    const QJsonObject state = serverState(4, revisionA);
    m_server->setHandler([&](const Request &) { return json(state); });
    bool answered = false;
    bool pendingWork = true;
    QString key;
    const auto ask = [&] {
        answered = false;
        repository->m_readerArtifactCheckThread = nullptr;
        repository->checkRemoteWork([&](bool p, const QString &k) {
            answered = true;
            pendingWork = p;
            key = k;
        });
        return QTest::qWaitFor([&] { return answered; }, 5000);
    };

    // Everything is as recorded: no work, and the files were looked at on another thread.
    QVERIFY(repository->manualReaderArtifactsCurrent(state));
    QVERIFY(ask());
    QVERIFY(!pendingWork);
    QCOMPARE(key, QStringLiteral("4/%1/7").arg(revisionA));
    QVERIFY(repository->m_readerArtifactCheckThread != nullptr);
    QVERIFY(repository->m_readerArtifactCheckThread != QThread::currentThreadId());
    QVERIFY(!repository->m_readerArtifactCheckRunning);

    // A cached file changed: both checks say the Library is not current.
    QVERIFY(writeFile(paths.at(1), QByteArrayLiteral("changed after it was verified")));
    QVERIFY(!repository->manualReaderArtifactsCurrent(state));
    QVERIFY(ask());
    QVERIFY(pendingWork);
    QVERIFY(repository->m_readerArtifactCheckThread != QThread::currentThreadId());

    // A cached file is gone.
    QVERIFY(QFile::remove(paths.at(0)));
    QVERIFY(!repository->manualReaderArtifactsCurrent(state));
    QVERIFY(ask());
    QVERIFY(pendingWork);

    // An object without a receipt is known without looking at any file.
    cached.append(QString(64, QLatin1Char('e')));
    frontier.metadata.insert(QStringLiteral("reader_cache_objects"), cached);
    QVERIFY2(store->setFrontier(frontier, &error), qPrintable(error));
    QVERIFY(!repository->manualReaderArtifactsCurrent(state));
    QVERIFY(ask());
    QVERIFY(pendingWork);
    QVERIFY(repository->m_readerArtifactCheckThread == nullptr);

    // While a look at the files has not returned, the next check starts no second one.
    repository->m_readerArtifactCheckRunning = true;
    cached.removeLast();
    frontier.metadata.insert(QStringLiteral("reader_cache_objects"), cached);
    QVERIFY2(store->setFrontier(frontier, &error), qPrintable(error));
    QVERIFY(ask());
    QVERIFY(!pendingWork);
    QVERIFY(key.isEmpty());
    repository->m_readerArtifactCheckRunning = false;
}

// A gateway error or a lost connection on a request that changes nothing when repeated
// no longer fails the Sync: it is asked again (after 1, 2 and 4 s; shortened here).
void SyncFlowTest::testTransientFailuresAreAskedAgain()
{
    FedoraRepository *const repository = m_repository;
    repository->m_retryDelaysMs = {20, 40, 80};
    int stateRequests = 0;
    int renderChecks = 0;
    int catalogueRequests = 0;
    QStringList renderCheckIds;
    m_server->setHandler([&](const Request &request) {
        // The catalogue is the first request of the Sync: unavailable once, then "no such
        // endpoint" as in the other tests.
        if (request.target == "/v1/library/projects") return json({}, ++catalogueRequests == 1 ? 503 : 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") {
            ++stateRequests;
            if (stateRequests == 1) return json({{QStringLiteral("detail"), QStringLiteral("Bad Gateway")}}, 502);
            if (stateRequests == 2) {
                Response dropped;
                dropped.drop = true;
                return dropped;
            }
            return json(serverState(4, revisionA));
        }
        if (request.target == "/v1/render-checks") {
            ++renderChecks;
            renderCheckIds.append(QJsonDocument::fromJson(request.body).object().value(QStringLiteral("request_id")).toString());
            if (renderChecks <= 3) return json({{QStringLiteral("detail"), QStringLiteral("Service Unavailable")}}, renderChecks == 1 ? 503 : 504);
            return json({{QStringLiteral("status"), QStringLiteral("current")}, {QStringLiteral("library_revision"), 7}, {QStringLiteral("documents"), QJsonArray()}});
        }
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QSignalSpy journal(repository, SIGNAL(syncJournalEvent(QString,QJsonObject)));
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000004a")), qPrintable(failure()));
    QVERIFY2(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(failure()));
    QVERIFY(repository->manualSyncFailureKind().isEmpty());
    // Two failed state requests, then the good one; later the recheck.
    QVERIFY(stateRequests >= 4);
    // The render check was asked four times under one identity: the same check.
    QCOMPARE(renderChecks, 4);
    QCOMPARE(QSet<QString>(renderCheckIds.cbegin(), renderCheckIds.cend()).size(), 1);
    QVERIFY(!renderCheckIds.constFirst().isEmpty());
    int announced = 0;
    for (const QList<QVariant> &event : journal) {
        announced += event.at(1).toJsonObject().value(QStringLiteral("metadata")).toObject().contains(QStringLiteral("retry_attempt")) ? 1 : 0;
    }
    QCOMPARE(catalogueRequests, 2);
    // The catalogue, the 502 and the three render checks are announced; Qt itself may
    // already repeat a request whose connection closed without an answer.
    QVERIFY2(announced == 5 || announced == 6, qPrintable(QString::number(announced)));
}

// The server's Library changes between the reader plan and the Library read under it (a
// build that just ended is still publishing). The plan is requested again, three times
// before the Sync gives up, and such a failure is one a later Sync may not have.
void SyncFlowTest::testLibraryThatKeepsChangingIsPlannedAgain()
{
    FedoraRepository *const repository = m_repository;
    int plans = 0;
    int libraries = 0;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") return json(serverState(4, revisionB));
        if (request.target == "/v1/render-checks") return json({{QStringLiteral("status"), QStringLiteral("current")}, {QStringLiteral("library_revision"), 7}, {QStringLiteral("documents"), QJsonArray()}});
        if (request.target == "/v2/sync/plan") {
            ++plans;
            return json({{QStringLiteral("contract_version"), 2},
                         {QStringLiteral("source_profile"), QStringLiteral("reader-v1")},
                         {QStringLiteral("include_voice_media"), false},
                         {QStringLiteral("plan_id"), QString(64, QLatin1Char('1'))},
                         {QStringLiteral("latest_server_sequence"), 4},
                         {QStringLiteral("library_revision"), revisionB},
                         {QStringLiteral("metadata_download_bytes"), 100000},
                         {QStringLiteral("download_bytes"), 100000},
                         {QStringLiteral("objects"), QJsonArray()}});
        }
        if (request.target.startsWith("/v1/changes")) return json({{QStringLiteral("next_cursor"), 4}, {QStringLiteral("has_more"), false}, {QStringLiteral("changes"), QJsonArray()}});
        if (request.target == "/v2/library") {
            ++libraries;
            // Not the revision that was planned: the Library moved on.
            return json({{QStringLiteral("library_revision"), revisionC}, {QStringLiteral("latest_server_sequence"), 4}, {QStringLiteral("publications"), QJsonArray()}});
        }
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000005a")), qPrintable(failure()));
    QVERIFY(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Failed);
    QVERIFY2(repository->manualSyncMessage().contains(QStringLiteral("diverged from its approved reader plan")), qPrintable(failure()));
    // The first plan and three more.
    QCOMPARE(plans, 4);
    QCOMPARE(libraries, 4);
    QCOMPARE(repository->manualSyncFailureKind(), QStringLiteral("transient"));
}

void SyncFlowTest::testFailuresHaveAKind_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("requests");
    // What the server refused is asked once; a failure that may pass is asked four times.
    QTest::newRow("the server refused the request") << 400 << QStringLiteral("bug") << 1;
    QTest::newRow("the server failed on the request") << 500 << QStringLiteral("bug") << 1;
    QTest::newRow("the token is not accepted") << 401 << QStringLiteral("needs_user") << 1;
    QTest::newRow("the app is too old for the server") << 426 << QStringLiteral("needs_user") << 1;
    QTest::newRow("the server stays unavailable") << 503 << QStringLiteral("transient") << 4;
    QTest::newRow("the server never answers") << 0 << QStringLiteral("transient") << 4;
}

// A failed Sync says what kind of failure it was, so that whoever starts Syncs can
// tell "try again later" from "the user must act" and "trying again changes nothing".
void SyncFlowTest::testFailuresHaveAKind()
{
    QFETCH(int, status);
    QFETCH(QString, kind);
    QFETCH(int, requests);
    FedoraRepository *const repository = m_repository;
    repository->m_retryDelaysMs = {20, 40, 80};
    int stateRequests = 0;
    m_server->setHandler([&](const Request &request) {
        if (request.target == "/v1/library/projects") return json({}, 404);
        if (request.target == "/v1/ready") return json({{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray()}});
        if (request.target == "/v2/sync/state") {
            ++stateRequests;
            if (status == 0) {
                Response dropped;
                dropped.drop = true;
                return dropped;
            }
            return json({{QStringLiteral("detail"), QStringLiteral("scripted failure")}}, status);
        }
        return json({{QStringLiteral("detail"), QStringLiteral("unexpected")}}, 500);
    });
    QVERIFY(repository->manualSyncFailureKind().isEmpty());
    QVERIFY2(runSync(QStringLiteral("0190a000-0000-7000-8000-00000000004b")), qPrintable(failure()));
    QVERIFY(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Failed);
    QCOMPARE(repository->manualSyncFailureKind(), kind);
    if (status == 0) {
        // Qt may itself repeat a request whose connection closed without an answer.
        QVERIFY2(stateRequests >= requests && stateRequests <= 2 * requests, qPrintable(QString::number(stateRequests)));
    } else {
        QCOMPARE(stateRequests, requests);
    }
    QCOMPARE(repository->manualSyncErrorCode(), QStringLiteral("sync_failed"));
}

void SyncFlowTest::testMissingTraceStreamAsksAboutTheBuildAtOnce_data()
{
    QTest::addColumn<bool>("buildNamesTrace");
    QTest::addColumn<int>("streamRequests");
    QTest::newRow("the build names no such trace") << false << 1;
    QTest::newRow("the build names the trace") << true << 2;
}

// GET /v2/traces/{id}/stream answers 404. The build is asked about at once
// (it used to be after retries at 1, 2 and 4 s), and the stream is tried again
// only once and only if the build names that trace.
void SyncFlowTest::testMissingTraceStreamAsksAboutTheBuildAtOnce()
{
    QFETCH(bool, buildNamesTrace);
    QFETCH(int, streamRequests);
    FedoraRepository *const repository = m_repository;
    const QString trace = QStringLiteral("0190a000-0000-7000-8000-00000000002a");
    QJsonObject state = repository->readRepositoryState();
    state.insert(QStringLiteral("active_builds"), QJsonArray {QJsonObject {{QStringLiteral("id"), QStringLiteral("build-six")}, {QStringLiteral("target"), QStringLiteral("paper.pdf")}}});
    state.insert(QStringLiteral("build_runs"),
                 QJsonObject {{QStringLiteral("paper.pdf"),
                               QJsonObject {{QStringLiteral("run_id"), trace},
                                            {QStringLiteral("trace_id"), trace},
                                            {QStringLiteral("target"), QStringLiteral("paper.pdf")},
                                            {QStringLiteral("sync_scope_target"), QStringLiteral("paper.pdf")},
                                            {QStringLiteral("status"), QStringLiteral("running")},
                                            {QStringLiteral("build_id"), QStringLiteral("build-six")},
                                            {QStringLiteral("sync_origin"), QStringLiteral("manual_conditional")}}}});
    QVERIFY(repository->writeRepositoryState(state));
    repository->m_traceCapabilityKnown = true;
    repository->m_traceEventsSupported = true;
    repository->m_pollingBuild = false;
    repository->m_activeTraceId = trace;
    repository->m_syncCoordinator.state = FedoraRepository::ManualSyncState::Running;
    repository->m_syncCoordinator.phase = FedoraRepository::ManualSyncPhase::WaitBuild;
    repository->m_syncCoordinator.setTarget(QStringLiteral("paper.pdf"));

    QElapsedTimer clock;
    qint64 streamAskedAtMs = -1;
    qint64 longPollAtMs = -1;
    m_server->setHandler([&](const Request &request) {
        if (request.target.startsWith("/v2/traces/")) {
            if (streamAskedAtMs < 0) streamAskedAtMs = clock.elapsed();
            return json({{QStringLiteral("detail"), QStringLiteral("Trace not found")}}, 404);
        }
        if (request.target == "/v2/builds/build-six") {
            QJsonObject build = buildRecord(QStringLiteral("build-six"), QStringLiteral("running"));
            build.insert(QStringLiteral("trace_id"), buildNamesTrace ? trace : QStringLiteral("0190a000-0000-7000-8000-00000000002b"));
            return json(build);
        }
        if (request.target == "/v2/builds/build-six?wait_ms=30000" && longPollAtMs < 0) longPollAtMs = clock.elapsed();
        Response never;
        never.delayMs = -1;
        return never;
    });
    clock.start();
    QVERIFY(QMetaObject::invokeMethod(repository, "advanceManualSync", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(longPollAtMs >= 0, 5000);
    // No 1 s retry pause anywhere between the 404 and waiting on the build.
    QVERIFY2(longPollAtMs - streamAskedAtMs < 900, qPrintable(QStringLiteral("%1 ms").arg(longPollAtMs - streamAskedAtMs)));
    QCOMPARE(m_server->count("GET", "/v2/traces/"), streamRequests);
    QCOMPARE(m_server->count("GET", "/v2/builds/build-six"), streamRequests + 1);
    // The build was asked about directly after each 404, without a long poll.
    const QList<Request> &requests = m_server->requests();
    QCOMPARE(requests.at(0).target, QByteArray("/v2/traces/" + trace.toLatin1() + "/stream?after=0"));
    QCOMPARE(requests.at(1).target, QByteArrayLiteral("/v2/builds/build-six"));
    QCOMPARE(requests.constLast().target, QByteArrayLiteral("/v2/builds/build-six?wait_ms=30000"));
    const QJsonObject run = repository->readRepositoryState().value(QStringLiteral("build_runs")).toObject().value(QStringLiteral("paper.pdf")).toObject();
    QVERIFY(run.value(QStringLiteral("trace_stream_disabled")).toBool());
    QVERIFY(!run.contains(QStringLiteral("trace_stream_missing")));
    QCOMPARE(run.value(QStringLiteral("transient_attempts")).toInt(), 0);
    QVERIFY(repository->m_syncCoordinator.state == FedoraRepository::ManualSyncState::Running);

    repository->m_client.reset();
    repository->m_pollingBuild = false;
    repository->m_traceCapabilityKnown = false;
    repository->m_traceEventsSupported = false;
    repository->m_syncCoordinator.reset();
}

}

QTEST_MAIN(Okular::SyncFlowTest)
#include "syncflowtest.moc"
