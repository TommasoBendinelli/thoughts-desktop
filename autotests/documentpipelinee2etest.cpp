// SPDX-License-Identifier: GPL-2.0-or-later
// End-to-end test of the document pipeline: the desktop's real Sync engine
// and source publisher against a local thoughts-server and build worker.
// tools/e2e/run-document-pipeline-e2e.py starts the server and runs this with
// a private HOME; without THOUGHTS_E2E_SERVER every test is skipped.
// The functions run in order and build on each other's Library.
#include "../part/desktopsyncstore.h"
#include "../part/fedorarepository.h"
#include "../part/libraryprojectstore.h"
#include "../part/projectconfiguration.h"
#include "../part/sourcefoldermigration.h"
#include "../part/sourcefoldernames.h"
#include "../part/sourceowner.h"
#include "../part/sourcedocumentmove.h"
#include "../shell/agentproposalparser.h"
#include "../shell/documentmanagementdialog.h"
#include "../shell/proposalauthoring.h"
#include "../shell/proposaldecisions.h"
#include "../shell/reviewdocumenttools.h"
#include "../shell/reviewstore.h"
#include "../shell/thoughtscommandprotocol.h"
#include "../shell/websiteimportdialog.h"
#include "../part/documentidentity.h"
#include "../part/thoughtsplatform.h"

#include <poppler-qt6.h>

#include <thoughts/syncclient.h>

#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

// verifySettled() and, when it failed, the end of the test function: going on would only add
// failures and Syncs that cannot finish (see sync()).
#define VERIFY_SETTLED(label)                                                                                          \
    do {                                                                                                               \
        verifySettled(label);                                                                                          \
        if (QTest::currentTestFailed()) {                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

namespace
{
constexpr int syncTimeoutMs = 600000;
// A Sync with nothing to do checks the server and the render state only.
constexpr qint64 quietSyncLimitMs = 5000;
const QStringList workPhases {QStringLiteral("uploading"), QStringLiteral("building"), QStringLiteral("installing_sources"), QStringLiteral("activating"),
                              QStringLiteral("downloading"), QStringLiteral("importing_folder")};

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject readJson(const QString &path)
{
    return QJsonDocument::fromJson(readFile(path)).object();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray pdf(const QString &title, const QString &text)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QPdfWriter writer(&buffer);
    writer.setTitle(title);
    QPainter painter(&writer);
    painter.drawText(100, 100, text);
    painter.end();
    return bytes;
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Serves the paper for the link import and redirects its landing page to it.
class PaperServer : public QTcpServer
{
public:
    QByteArray paper;
    QList<QByteArray> requests;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            const QByteArray request = socket->readAll();
            const QByteArray path = request.split(' ').value(1);
            requests.append(path);
            QByteArray response;
            if (path == "/papers/latest")
                response = "HTTP/1.1 302 Found\r\nLocation: /files/paper.pdf\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            else if (path == "/files/paper.pdf")
                response = "HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\nContent-Length: " + QByteArray::number(paper.size()) + "\r\nConnection: close\r\n\r\n" + paper;
            else
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            socket->write(response);
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
};

// Appends the desktop's Sync journal to THOUGHTS_E2E_JOURNAL for diagnosis.
class JournalWriter : public QObject
{
    Q_OBJECT
public:
    explicit JournalWriter(const QString &path)
        : m_file(path)
    {
        m_file.open(QIODevice::WriteOnly | QIODevice::Append);
    }
public Q_SLOTS:
    void journal(const QString &traceId, const QJsonObject &event)
    {
        QJsonObject line = event;
        line.insert(QStringLiteral("trace_id"), traceId);
        m_file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
        m_file.flush();
    }
    void progress(const QString &traceId, const QString &phase, qint64, qint64, const QString &message)
    {
        if (!phase.startsWith(QLatin1String("timing_")))
            journal(traceId, QJsonObject {{QStringLiteral("progress"), phase}, {QStringLiteral("message"), message}});
    }
private:
    QFile m_file;
};

// Runs a callback on each Sync progress phase (FedoraRepository's signals
// are reachable here only by name).
class ProgressHook : public QObject
{
    Q_OBJECT
public:
    std::function<void(const QString &)> onPhase;
    std::function<void(const QString &, const QString &)> onMessage;
public Q_SLOTS:
    void progress(const QString &, const QString &phase, qint64, qint64, const QString &message)
    {
        if (onPhase) onPhase(phase);
        if (onMessage) onMessage(phase, message);
    }
};

// The import dialog accepts only https links without a port. Route the test
// host to the loopback paper server; every other host goes nowhere.
class LoopbackNetwork : public QNetworkAccessManager
{
public:
    explicit LoopbackNetwork(quint16 port)
        : m_port(port)
    {
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override
    {
        QUrl url = request.url();
        const bool paperHost = url.scheme() == QLatin1String("https") && url.host() == QLatin1String("papers.e2e.invalid");
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("127.0.0.1"));
        url.setPort(paperHost ? m_port : 9);
        QNetworkRequest local(request);
        local.setUrl(url);
        return QNetworkAccessManager::createRequest(operation, local, data);
    }

private:
    quint16 m_port;
};
}

namespace Okular
{
class DocumentPipelineE2ETest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testInitialProjectsBuild();
    void testNewMarkdownDocument();
    void testNewLatexDocument();
    void testAnchorRemovedComment();
    void testProposalDecisions();
    void testNestedLatexEntryPoint();
    void testImportPdfFile();
    void testImportPdfLink();
    void testUnresolvableLinksDoNotBlockSync();
    void testMarkdownDocumentLinks();
    void testProjectFileChangedOnBothSides();
    void testEditMarkdownDocument();
    void testIncludedFlag();
    void testDeleteDocuments();
    void testDefaultFolderDocumentCheckedAfterItsFileSynced();
    void testDefaultFolderDocuments();
    void testRestoreRemovedDocument();
    // Knowledge cards and project cards: a Project's kind and a card's kind through a real Sync.
    void testReviewCardKinds();
    // Review cards on a source document are always linked (2026-10-13).
    void testLinkedReviewCards();
    // A failed PDF build does not block proposals (2026-10-14).
    void testProposalAfterFailedBuild();
    void testFailureOfReplacedRenderer();
    // Shared figures of a multi-document Project: one publisher run per
    // Project, and a deleted figure does not stay pending (2026-10-14).
    void testSharedAssetPublication();
    // A realistic Library (24 LaTeX documents, about 400 paths); timings only.
    // Runs when THOUGHTS_E2E_PERF is set.
    void testSyncSpeedRealisticLibrary();
    // Fedora renames every default folder documents/ -> default/ (with its
    // services stopped by the harness); the next Sync renames this
    // computer's, publishes an unsynced edit to the new path and keeps every
    // PDF name. The later tests then run on the renamed Library. Before the
    // Project-manifest tests: Fedora refuses to migrate an open conflict.
    void testDefaultFolderRename();
    // Fedora renames every connected folder docs-<old> -> docs-<slug>-<hex>
    // (source folder names spec, services stopped by the harness); the next
    // Sync renames this computer's folders, publishes an unsynced edit to the
    // new path, never deletes an old path, and keeps every PDF name and card.
    // Fedora refuses to migrate an open conflict, so this runs before them.
    void testSourceFolderRename();
    // Before the Project-manifest tests: the last one leaves a conflict open on purpose.
    void testMoveSourceDocument();
    void testNewProjectReceivesMovedFolder();
    void testUnpublishedProjectEditIsNotOverwritten();
    // Uses the conflict the previous test leaves open.
    void testProposalBlockedBySourceConflict();

private:
    struct Outcome {
        FedoraRepository::ManualSyncState state = FedoraRepository::ManualSyncState::Idle;
        QString message;
        qint64 elapsedMs = 0;
        QStringList phases;
        QString generation;
        // Every progress message with its time since the Sync started.
        QList<QPair<qint64, QString>> messages;
        // Source publisher runs ("Publishing local ..." messages).
        int publisherRuns() const;
        // The PDFs the publisher runs covered.
        QStringList publishedTargets() const;
        // Milliseconds from the first publication step to the render check; 0 without publication.
        qint64 publicationMs() const;
    };
    struct Project {
        QString id;
        QString owner;
        QString target;
        QStringList pdfs;
    };

    // Returns a failed Outcome at once when the test has already failed: the
    // QSignalSpy waits then return immediately (no event loop runs), so a Sync
    // would never finish and the test would stall until QTest's watchdog.
    // A Sync as the app runs it since Sync of the whole Library no longer waits for the
    // server's PDF builds (14 November 2026): the Sync, and while it left builds running,
    // the Sync that the repository announces (remoteWorkFound) when a build has ended and
    // that the shell then starts. Returns when no build is pending any more.
    Outcome sync(const QString &label);
    // One Sync, which may end while the server still builds.
    Outcome syncOnce(const QString &label);
    // Since 14 November 2026 a PDF the server could not build no longer fails the Sync
    // whose changes arrived: the Sync succeeded with the warning pdf_build_failed.
    bool buildFailed(const Outcome &outcome) const
    {
        return outcome.state == FedoraRepository::ManualSyncState::Succeeded && m_repository->manualSyncWarningCode() == QLatin1String("pdf_build_failed");
    }
    // A completed Sync must leave nothing pending, and the next one must be a
    // quick no-op that changes neither sources nor the Library. Called through
    // VERIFY_SETTLED, which ends the test function when it failed.
    void verifySettled(const QString &label);
    bool nothingPending() const;
    QString pendingSummary() const;
    QHash<QString, QString> library() const;
    // The included documents of a Project, as the Library catalogue lists them.
    QStringList catalogueFiles(const Project &project) const;
    // The document titles of a Project's published reading.
    QStringList readingTitles(const Project &project) const;
    QString generation() const;
    QJsonObject configuration(const Project &project) const;
    QJsonObject catalogueProject(const Project &project) const;
    QJsonObject declaredDocument(const Project &project, const QString &path) const;
    // @p projectId: a fixed id (e.g. a legacy "source:docs-x"); a new one when empty.
    QString createProject(const QString &name, const QString &owner, const QString &source, const QByteArray &text, const QString &projectId = {});
    // Checks or unchecks a document in Manage documents and applies it.
    QString manageDocument(const Project &project, const QString &alias, bool included);
    // The PDF filename Manage documents recorded for a source document.
    QString declaredOutput(const Project &project, const QString &path) const;
    // f) A change in one Project leaves the other's PDFs and Project file alone.
    void verifyUntouched(const Project &project, const QHash<QString, QString> &before, const QByteArray &configurationBefore);
    // Fedora's current bytes of a source path; empty when it has none.
    QByteArray serverSource(const QString &path) const;
    // Fedora's /v1/sources entry of a path, deleted ones included; empty when it has none.
    QJsonObject serverSourceEntry(const QString &path) const;
    // Asks tools/e2e/run-document-pipeline-e2e.py to act on the server
    // (THOUGHTS_E2E_CONTROL) and returns its answer.
    QJsonObject harnessRequest(const QString &action) const;
    // Fedora's render status of one Library PDF (GET /v1/render-status).
    QJsonObject renderStatus(const QString &target) const;
    // A Project created in the Library, as "Add project" creates it.
    QString addLibraryProject(const QString &name);
    // Moves a connected source folder to another Project, as the Library's Move does.
    QString moveSourceFolder(const QString &from, const QString &owner, const QString &to);
    // Why this computer's copy of a Project manifest is not Fedora's settled
    // copy: different bytes, or no recorded authoring base. Empty when settled.
    QString manifestProblem(const QString &projectId) const;
    // The state the 2026-10-12 "Apps" Project was left in: a local manifest
    // Fedora never acknowledged, no recorded base, and a source inventory
    // that predates the Project.
    void forgetProjectManifest(const QString &projectId, const QByteArray &local);
    // A Project with one connected LaTeX folder: files to write and the
    // folder-relative documents it declares. Returns its id.
    QString createLatexProject(const QString &name, const QString &owner, const QHash<QString, QByteArray> &files, const QStringList &documents);
    // Fedora's answer to a Library request (plan or operation), synchronously.
    QJsonObject libraryRequest(const QJsonObject &request, QString *error) const;
    // Uploads review-card operations as the phone; returns the server's error, empty on success.
    QString uploadAsPhone(const QList<QJsonObject> &payloads, const QList<qint64> &baseRevisions, const QList<QByteArray> &objects = {}) const;
    // The card as this computer's projections show it.
    QJsonObject cardProjection(const QString &cardId) const;
    // Syncs until @p done holds (the worker places cards after a build), at most @p syncs times.
    bool syncUntil(const QString &label, const std::function<bool()> &done, int syncs = 8);
    // The command result thoughtsctl receives for the last Sync, from the
    // operation the app records for it (Shell::updateCommentsOverviewManualSyncState).
    QJsonObject syncCommandResult(const Outcome &outcome) const;
    // Rewrites a build record of the local Fedora, which keeps its state in
    // ./state/thoughts.db, as if another server version had run the build.
    bool rewriteServerBuild(const QString &buildId, const std::function<void(QString &status, QJsonObject &progress, QJsonObject &result)> &edit) const;
    // A create-proposal request as thoughtsctl reads it, replacing @p line (1-based) of @p path.
    QJsonObject proposalRequest(const QString &path, int line, const QByteArray &replacement) const;

    FedoraRepository *m_repository = nullptr;
    QString m_appsId;
    QString m_docs;
    QUrl m_server;
    QByteArray m_token;
    Project m_alpha;
    Project m_beta;
    QString m_markdownTarget;
    QString m_latexTarget;
    QString m_removedLatexId;
    QString m_nestedTarget;
    QString m_importedFile;
    QString m_importedLink;
    QString m_brokenFragment;
};

void DocumentPipelineE2ETest::initTestCase()
{
    const QString server = qEnvironmentVariable("THOUGHTS_E2E_SERVER");
    if (server.isEmpty()) {
        QSKIP("Set THOUGHTS_E2E_SERVER; run tools/e2e/run-document-pipeline-e2e.py.");
    }
    // Isolation: a loopback server only, and every location in a private HOME.
    m_server = QUrl(server);
    QVERIFY2(m_server.scheme() == QLatin1String("http") && m_server.host() == QLatin1String("127.0.0.1") && m_server.port() > 0, qPrintable(server));
    QCOMPARE(QUrl(qEnvironmentVariable("THOUGHTS_SERVER_URL")), m_server);
    const QString home = QDir(qEnvironmentVariable("HOME")).canonicalPath();
    QVERIFY2(!home.isEmpty() && home == QDir(qEnvironmentVariable("CFFIXED_USER_HOME")).canonicalPath() && home.contains(QStringLiteral("thoughts-e2e-")),
             "HOME and CFFIXED_USER_HOME must be the harness's private home");
    m_docs = qEnvironmentVariable("THOUGHTS_E2E_DOCS");
    QVERIFY(QDir().mkpath(m_docs));
    m_docs = QDir(m_docs).canonicalPath();
    // macOS reports the same directory as /var/... or /private/var/...
    const auto normalized = [](const QString &path) {
        const QString clean = QDir::cleanPath(path);
        return clean.startsWith(QLatin1String("/private/")) ? clean.mid(8) : clean;
    };
    for (const QString &location : {QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                                    QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), m_docs}) {
        QVERIFY2(normalized(location).startsWith(normalized(home) + QLatin1Char('/')), qPrintable(QStringLiteral("%1 is outside %2").arg(location, home)));
    }
    m_token = qgetenv("THOUGHTS_TOKEN");
    QVERIFY(!m_token.isEmpty() && !qgetenv("THOUGHTS_SOURCE_TOKEN").isEmpty());
    QVERIFY(QFileInfo(qEnvironmentVariable("THOUGHTS_SOURCE_PUBLISHER")).isExecutable());

    m_repository = FedoraRepository::instance(m_docs);
    QVERIFY(m_repository);
    QCOMPARE(m_repository->m_docsRoot, m_docs);
    // ensureClient accepts only the production host; talk to the loopback server.
    m_repository->m_client = std::make_unique<Thoughts::SyncClient>(m_server, m_token, m_repository);
    if (const QString journal = qEnvironmentVariable("THOUGHTS_E2E_JOURNAL"); !journal.isEmpty()) {
        auto *writer = new JournalWriter(journal);
        writer->setParent(this);
        connect(m_repository, SIGNAL(syncJournalEvent(QString, QJsonObject)), writer, SLOT(journal(QString, QJsonObject)));
        connect(m_repository, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)), writer, SLOT(progress(QString, QString, qint64, qint64, QString)));
    }
}

DocumentPipelineE2ETest::Outcome DocumentPipelineE2ETest::sync(const QString &label)
{
    Outcome outcome = syncOnce(label);
    for (int followUp = 1; followUp <= 8 && outcome.state == FedoraRepository::ManualSyncState::Succeeded && !QTest::currentTestFailed() && !m_repository->pendingServerBuilds().isEmpty(); ++followUp) {
        QSignalSpy found(m_repository, SIGNAL(remoteWorkFound(bool, QString)));
        QElapsedTimer waited;
        waited.start();
        // The request the repository holds notices the end of the build; it then asks the
        // server's state and announces the work.
        if (!QTest::qWaitFor([&] { return !found.isEmpty() || m_repository->pendingServerBuilds().isEmpty(); }, syncTimeoutMs)) {
            outcome.state = FedoraRepository::ManualSyncState::Failed;
            outcome.message = QStringLiteral("the end of the server's build was not noticed");
            return outcome;
        }
        if (found.isEmpty()) {
            found.wait(5000);
        }
        qInfo().noquote() << "E2E sync" << label << "left a build running; its end was" << (found.isEmpty() ? "not announced" : "announced") << "after" << waited.elapsed() << "ms";
        Outcome next = syncOnce(label + QStringLiteral(" (after the build, #%1)").arg(followUp));
        for (const QString &phase : std::as_const(outcome.phases)) {
            if (!next.phases.contains(phase)) {
                next.phases.append(phase);
            }
        }
        for (auto &entry : next.messages) {
            entry.first += outcome.elapsedMs + waited.elapsed();
        }
        next.messages = outcome.messages + next.messages;
        next.elapsedMs += outcome.elapsedMs;
        outcome = next;
    }
    return outcome;
}

DocumentPipelineE2ETest::Outcome DocumentPipelineE2ETest::syncOnce(const QString &label)
{
    Outcome outcome;
    if (QTest::currentTestFailed()) {
        outcome.state = FedoraRepository::ManualSyncState::Failed;
        outcome.message = QStringLiteral("not started: the test already failed");
        qInfo().noquote() << "E2E sync" << label << "skipped: the test already failed";
        return outcome;
    }
    QSignalSpy states(m_repository, SIGNAL(manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState, QString, QString)));
    QSignalSpy progress(m_repository, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)));
    QElapsedTimer timer;
    timer.start();
    QList<QPair<qint64, QString>> messages;
    ProgressHook hook;
    hook.onMessage = [&](const QString &phase, const QString &message) { messages.append({timer.elapsed(), phase + QLatin1Char('|') + message}); };
    connect(m_repository, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)), &hook, SLOT(progress(QString, QString, qint64, qint64, QString)));
    m_repository->syncNow(uuid(), true);
    const QDeadlineTimer deadline(syncTimeoutMs);
    while (!deadline.hasExpired()) {
        outcome.state = m_repository->manualSyncState();
        if (!states.isEmpty() && (outcome.state == FedoraRepository::ManualSyncState::Succeeded || outcome.state == FedoraRepository::ManualSyncState::Failed)) {
            break;
        }
        states.wait(500);
    }
    outcome.elapsedMs = timer.elapsed();
    outcome.message = m_repository->manualSyncMessage();
    if (outcome.state == FedoraRepository::ManualSyncState::Failed) {
        outcome.message += QStringLiteral(" [%1] %2").arg(m_repository->manualSyncErrorCode(), m_repository->manualSyncDetails().right(3000));
    }
    for (const QList<QVariant> &arguments : std::as_const(progress)) {
        const QString phase = arguments.value(1).toString();
        if (!phase.startsWith(QLatin1String("timing_")) && !outcome.phases.contains(phase)) {
            outcome.phases.append(phase);
        }
    }
    for (const auto &[at, message] : std::as_const(messages)) {
        outcome.messages.append({at, message});
    }
    outcome.generation = generation();
    qInfo().noquote() << "E2E sync" << label << (outcome.state == FedoraRepository::ManualSyncState::Succeeded ? "succeeded" : "FAILED") << "in" << outcome.elapsedMs << "ms; phases:" << outcome.phases.join(QLatin1Char(','));
    return outcome;
}

int DocumentPipelineE2ETest::Outcome::publisherRuns() const
{
    return std::count_if(messages.cbegin(), messages.cend(), [](const QPair<qint64, QString> &entry) {
        return entry.second.startsWith(QLatin1String("uploading|Publishing local"));
    });
}

QStringList DocumentPipelineE2ETest::Outcome::publishedTargets() const
{
    // "Publishing local changes for a.pdf" or, for one run covering several
    // documents, "Publishing local changes for 3 documents: a.pdf, b.pdf, c.pdf".
    QStringList targets;
    const QString prefix = QStringLiteral("uploading|Publishing local changes for ");
    for (const auto &entry : messages) {
        if (!entry.second.startsWith(prefix)) continue;
        QString list = entry.second.mid(prefix.size());
        if (const qsizetype colon = list.indexOf(QStringLiteral(" documents: ")); colon >= 0) list = list.mid(colon + 12);
        targets.append(list.split(QStringLiteral(", "), Qt::SkipEmptyParts));
    }
    targets.sort();
    return targets;
}

qint64 DocumentPipelineE2ETest::Outcome::publicationMs() const
{
    qint64 start = -1;
    for (const auto &entry : messages) {
        if (start < 0 && entry.second.startsWith(QLatin1String("preparing|")) && entry.second.contains(QLatin1String("freezing"))) start = entry.first;
        if (start >= 0 && entry.second.startsWith(QLatin1String("checking|Checking whether the server needs to update any PDFs"))) return entry.first - start;
    }
    return start < 0 ? 0 : -1;
}

bool DocumentPipelineE2ETest::nothingPending() const
{
    const QJsonObject pending = m_repository->m_pendingChangesSnapshot;
    return pending.value(QStringLiteral("available")).toBool() && pending.value(QStringLiteral("total")).toInt() == 0
        && pending.value(QStringLiteral("source_state")).toString() == QLatin1String("clean")
        && Thoughts::LibraryProjectStore(m_docs).pending().isEmpty();
}

QString DocumentPipelineE2ETest::pendingSummary() const
{
    QJsonObject pending = m_repository->m_pendingChangesSnapshot;
    pending.remove(QStringLiteral("documents"));
    return QString::fromUtf8(QJsonDocument(pending).toJson(QJsonDocument::Compact)) + QStringLiteral(" project operations: ")
        + QString::fromUtf8(QJsonDocument(Thoughts::LibraryProjectStore(m_docs).pending()).toJson(QJsonDocument::Compact));
}

void DocumentPipelineE2ETest::verifySettled(const QString &label)
{
    QTRY_VERIFY2_WITH_TIMEOUT(nothingPending(), qPrintable(label + QStringLiteral(": still pending after Sync: ") + pendingSummary()), 15000);
    const QString before = generation();
    const Outcome quiet = sync(label + QStringLiteral(" (repeat)"));
    QVERIFY2(quiet.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(label + QStringLiteral(": repeated Sync failed: ") + quiet.message));
    QVERIFY2(quiet.elapsedMs < quietSyncLimitMs, qPrintable(label + QStringLiteral(": repeated Sync took %1 ms").arg(quiet.elapsedMs)));
    for (const QString &phase : workPhases) {
        QVERIFY2(!quiet.phases.contains(phase), qPrintable(label + QStringLiteral(": repeated Sync did work (%1): %2").arg(phase, quiet.phases.join(QLatin1Char(',')))));
    }
    QCOMPARE(quiet.generation, before);
    QTRY_VERIFY2_WITH_TIMEOUT(nothingPending(), qPrintable(label + QStringLiteral(": pending after the repeated Sync: ") + pendingSummary()), 15000);
}

QString DocumentPipelineE2ETest::generation() const
{
    const QFileInfo current(QDir(m_docs).filePath(QStringLiteral("current_pdfs")));
    return current.isSymLink() ? QFileInfo(current.symLinkTarget()).fileName() : QString();
}

QHash<QString, QString> DocumentPipelineE2ETest::library() const
{
    QHash<QString, QString> result;
    const QString current = QDir(m_docs).filePath(QStringLiteral("current_pdfs"));
    for (const QJsonValue &value : readJson(QDir(current).filePath(QStringLiteral(".thoughts-generation.json"))).value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        // The active file must be the published PDF.
        const QByteArray bytes = readFile(QDir(current).filePath(filename));
        result.insert(filename, bytes.startsWith("%PDF-") && sha256(bytes) == publication.value(QStringLiteral("sha256")).toString() ? sha256(bytes) : QStringLiteral("invalid"));
    }
    return result;
}

QStringList DocumentPipelineE2ETest::catalogueFiles(const Project &project) const
{
    QStringList result;
    for (const QJsonValue &value : readJson(QDir(m_docs).filePath(QStringLiteral(".thoughts/library-projects.json"))).value(QStringLiteral("projects")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")).toString() != project.id) continue;
        for (const QJsonValue &document : value.toObject().value(QStringLiteral("documents")).toArray()) {
            if (document.toObject().value(QStringLiteral("included")).toBool(true)) result.append(document.toObject().value(QStringLiteral("filename")).toString());
        }
    }
    return result;
}

QStringList DocumentPipelineE2ETest::readingTitles(const Project &project) const
{
    QStringList result;
    const QDir current(QDir(m_docs).filePath(QStringLiteral("current_pdfs")));
    for (const QString &name : current.entryList({QStringLiteral("*.thoughts-project")}, QDir::Files)) {
        const QJsonObject reading = readJson(current.filePath(name));
        if (reading.value(QStringLiteral("project_id")).toString() != project.id) continue;
        for (const QJsonValue &document : reading.value(QStringLiteral("documents")).toArray()) result.append(document.toObject().value(QStringLiteral("title")).toString());
    }
    return result;
}

QJsonObject DocumentPipelineE2ETest::configuration(const Project &project) const
{
    return Thoughts::ProjectConfiguration::read(Thoughts::ProjectConfiguration::path(m_docs, project.id));
}

QJsonObject DocumentPipelineE2ETest::catalogueProject(const Project &project) const
{
    for (const QJsonValue &value : Thoughts::LibraryProjectStore(m_docs).projects(false)) {
        if (value.toObject().value(QStringLiteral("id")).toString() == project.id) {
            return value.toObject();
        }
    }
    return {};
}

QJsonObject DocumentPipelineE2ETest::declaredDocument(const Project &project, const QString &path) const
{
    for (const QJsonValue &value : configuration(project).value(QStringLiteral("documents")).toArray()) {
        if (value.toObject().value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString() == path) {
            return value.toObject();
        }
    }
    return {};
}

QString DocumentPipelineE2ETest::createProject(const QString &name, const QString &owner, const QString &source, const QByteArray &text, const QString &projectId)
{
    const QString id = projectId.isEmpty() ? QString(QStringLiteral("new:") + uuid()) : projectId;
    QJsonObject value = Thoughts::ProjectConfiguration::empty(m_docs, id, name);
    const QString folder = uuid();
    const QString document = uuid();
    QJsonArray folders = value.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject {{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), uuid()}});
    value.insert(QStringLiteral("source_folders"), folders);
    value.insert(QStringLiteral("documents"),
                 QJsonArray {QJsonObject {{QStringLiteral("document_id"), document},
                                          {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), source}}},
                                          {QStringLiteral("format"), QStringLiteral("markdown")},
                                          {QStringLiteral("included"), true}}});
    value.insert(QStringLiteral("document_names"), QJsonObject {{document, QString(name + QStringLiteral(" notes"))}});
    if (!Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(m_docs, id), value).isEmpty() || !writeFile(QDir(m_docs).filePath(owner + QLatin1Char('/') + source), text)) {
        return {};
    }
    return id;
}

QString DocumentPipelineE2ETest::manageDocument(const Project &project, const QString &alias, bool included)
{
    const QJsonObject current = catalogueProject(project);
    if (current.isEmpty()) {
        return QStringLiteral("The Project is not in the catalogue.");
    }
    Thoughts::DocumentManagementDialog dialog(m_docs, current);
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("manageDocumentsList"));
    QListWidgetItem *row = nullptr;
    for (int i = 0; list && i < list->count(); ++i) {
        if (list->item(i)->toolTip() == alias) {
            row = list->item(i);
        }
    }
    if (!row) {
        return QStringLiteral("Manage documents does not list %1").arg(alias);
    }
    row->setCheckState(included ? Qt::Checked : Qt::Unchecked);
    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
    return dialog.result() == QDialog::Accepted ? QString() : dialog.findChildren<QLabel *>().constLast()->text();
}

QString DocumentPipelineE2ETest::declaredOutput(const Project &project, const QString &path) const
{
    const QJsonObject document = declaredDocument(project, path);
    return configuration(project).value(QStringLiteral("rendering")).toObject().value(document.value(QStringLiteral("document_id")).toString()).toObject().value(QStringLiteral("output")).toString();
}

void DocumentPipelineE2ETest::verifyUntouched(const Project &project, const QHash<QString, QString> &before, const QByteArray &configurationBefore)
{
    const QHash<QString, QString> after = library();
    for (const QString &filename : project.pdfs) {
        QVERIFY2(after.value(filename).size() == 64 && after.value(filename) == before.value(filename), qPrintable(QStringLiteral("%1 changed").arg(filename)));
    }
    QCOMPARE(readFile(Thoughts::ProjectConfiguration::path(m_docs, project.id)), configurationBefore);
}

QByteArray DocumentPipelineE2ETest::serverSource(const QString &path) const
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    const auto get = [&](const QString &endpoint) {
        QNetworkRequest request(m_server.resolved(QUrl(endpoint)));
        request.setRawHeader("Authorization", "Bearer " + qgetenv("THOUGHTS_SOURCE_TOKEN"));
        QNetworkReply *reply = network.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        reply->deleteLater();
        return body;
    };
    for (const QJsonValue &value : QJsonDocument::fromJson(get(QStringLiteral("/v1/sources"))).object().value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        if (source.value(QStringLiteral("path")).toString() != path || source.value(QStringLiteral("deleted")).toBool()) continue;
        const QByteArray bytes = get(QStringLiteral("/v1/objects/") + source.value(QStringLiteral("object_sha256")).toString());
        return sha256(bytes) == source.value(QStringLiteral("sha256")).toString() ? bytes : QByteArray();
    }
    return {};
}

QJsonObject DocumentPipelineE2ETest::serverSourceEntry(const QString &path) const
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(m_server.resolved(QUrl(QStringLiteral("/v1/sources"))));
    request.setRawHeader("Authorization", "Bearer " + qgetenv("THOUGHTS_SOURCE_TOKEN"));
    QNetworkReply *reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
    reply->deleteLater();
    for (const QJsonValue &value : QJsonDocument::fromJson(body).object().value(QStringLiteral("sources")).toArray()) {
        if (value.toObject().value(QStringLiteral("path")).toString() == path) return value.toObject();
    }
    return {};
}

QJsonObject DocumentPipelineE2ETest::harnessRequest(const QString &action) const
{
    const QDir control(qEnvironmentVariable("THOUGHTS_E2E_CONTROL"));
    QFile::remove(control.filePath(QStringLiteral("response.json")));
    if (!writeFile(control.filePath(QStringLiteral("request.json.part")), QJsonDocument(QJsonObject{{QStringLiteral("action"), action}}).toJson())
        || !QFile::rename(control.filePath(QStringLiteral("request.json.part")), control.filePath(QStringLiteral("request.json")))) {
        return {{QStringLiteral("exit"), -1}, {QStringLiteral("output"), QStringLiteral("cannot write the harness request")}};
    }
    const QDeadlineTimer deadline(600000);
    while (!deadline.hasExpired()) {
        const QJsonObject response = readJson(control.filePath(QStringLiteral("response.json")));
        if (!response.isEmpty()) return response;
        QTest::qWait(200);
    }
    return {{QStringLiteral("exit"), -1}, {QStringLiteral("output"), QStringLiteral("the harness did not answer")}};
}

QJsonObject DocumentPipelineE2ETest::renderStatus(const QString &target) const
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QUrl url = m_server.resolved(QUrl(QStringLiteral("/v1/render-status")));
    url.setQuery(QStringLiteral("target=") + target);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    QNetworkReply *reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject result = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return result;
}

QString DocumentPipelineE2ETest::addLibraryProject(const QString &name)
{
    const QJsonObject project {{QStringLiteral("id"), QString(QStringLiteral("new:") + uuid())}, {QStringLiteral("revision"), 0}};
    const QString error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("create"), project, {{QStringLiteral("name"), name}});
    return error.isEmpty() ? project.value(QStringLiteral("id")).toString() : QString();
}

QString DocumentPipelineE2ETest::moveSourceFolder(const QString &from, const QString &owner, const QString &to)
{
    QJsonObject source, destination;
    for (const QJsonValue &value : Thoughts::LibraryProjectStore(m_docs).projects(false)) {
        if (value.toObject().value(QStringLiteral("id")).toString() == from) source = value.toObject();
        if (value.toObject().value(QStringLiteral("id")).toString() == to) destination = value.toObject();
    }
    if (source.isEmpty() || destination.isEmpty()) return QStringLiteral("The Projects are not in the Library.");
    return Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("move_source"), source,
                                                          {{QStringLiteral("source_project"), owner},
                                                           {QStringLiteral("destination_project_id"), to},
                                                           {QStringLiteral("destination_revision"), destination.value(QStringLiteral("revision")).toInt()}});
}

QString DocumentPipelineE2ETest::manifestProblem(const QString &projectId) const
{
    const QString file = Thoughts::ProjectConfiguration::path(m_docs, projectId);
    const QString path = QDir(m_docs).relativeFilePath(file);
    const QByteArray remote = serverSource(path);
    const QByteArray local = readFile(file);
    QStringList problems;
    if (remote.isEmpty()) problems.append(QStringLiteral("Fedora has no manifest"));
    else if (local != remote) problems.append(QStringLiteral("local manifest %1 differs from Fedora's %2").arg(sha256(local).left(12), sha256(remote).left(12)));
    const QJsonObject state = readJson(QDir(m_repository->m_stateRoot).filePath(QStringLiteral("source-publication.json")));
    const QJsonObject base = state.value(QStringLiteral("bases")).toObject().value(path).toObject();
    if (base.isEmpty()) problems.append(QStringLiteral("no recorded base"));
    else if (base.value(QStringLiteral("sha256")).toString() != sha256(remote)) problems.append(QStringLiteral("the recorded base is not Fedora's copy"));
    return problems.join(QStringLiteral("; "));
}

void DocumentPipelineE2ETest::forgetProjectManifest(const QString &projectId, const QByteArray &local)
{
    const QString file = Thoughts::ProjectConfiguration::path(m_docs, projectId);
    const QString path = QDir(m_docs).relativeFilePath(file);
    QVERIFY(writeFile(file, local));
    const QString statePath = QDir(m_repository->m_stateRoot).filePath(QStringLiteral("source-publication.json"));
    QJsonObject state = readJson(statePath);
    for (const QString &key : {QStringLiteral("bases"), QStringLiteral("materialized_sources")}) {
        QJsonObject entries = state.value(key).toObject();
        entries.remove(path);
        state.insert(key, entries);
    }
    QVERIFY(writeFile(statePath, QJsonDocument(state).toJson()));
    // Nor does this computer's source tracker know the file.
    QString error;
    Thoughts::DesktopSyncStore store(QDir(m_repository->m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
    QVERIFY2(store.open(&error), qPrintable(error));
    QList<Thoughts::DesktopTrackedPath> baselines = store.baselines(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    baselines.erase(std::remove_if(baselines.begin(), baselines.end(), [&](const Thoughts::DesktopTrackedPath &baseline) { return baseline.path == path; }), baselines.end());
    QVERIFY2(store.replaceBaselines(baselines, &error), qPrintable(error));
    QJsonObject manifest = readJson(m_repository->m_sourceManifestPath);
    QJsonArray shared;
    for (const QJsonValue &entry : manifest.value(QStringLiteral("shared_files")).toArray())
        if (entry.toObject().value(QStringLiteral("path")).toString() != path) shared.append(entry);
    manifest.insert(QStringLiteral("shared_files"), shared);
    QVERIFY(writeFile(m_repository->m_sourceManifestPath, QJsonDocument(manifest).toJson()));
    const QString cataloguePath = QDir(m_docs).filePath(QStringLiteral(".thoughts/library-projects.json"));
    QJsonObject catalogue = readJson(cataloguePath);
    catalogue.remove(QStringLiteral("generated_manifests"));
    QVERIFY(writeFile(cataloguePath, QJsonDocument(catalogue).toJson()));
}

void DocumentPipelineE2ETest::testNewProjectReceivesMovedFolder()
{
    // 2026-10-12: the user created the Project "Apps" in the Library and moved
    // the folder of an existing Project into it. Fedora wrote both revisions of
    // Apps's manifest, but this computer kept its own first copy, recorded no
    // base for it and never managed it.
    Project gamma {createProject(QStringLiteral("Gamma"), QStringLiteral("docs-gamma"), QStringLiteral("markdown/source/notes.md"), "# Gamma\n\nMoves into Apps.\n"),
                   QStringLiteral("docs-gamma"), QStringLiteral("gamma_notes.pdf")};
    QVERIFY(!gamma.id.isEmpty());
    Outcome outcome = sync(QStringLiteral("Gamma"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(library().value(gamma.target).size(), 64);
    VERIFY_SETTLED(QStringLiteral("Gamma"));

    // a) Create a Project and move a folder into it before the next Sync.
    m_appsId = addLibraryProject(QStringLiteral("Apps"));
    QVERIFY(!m_appsId.isEmpty());
    QString error = moveSourceFolder(gamma.id, gamma.owner, m_appsId);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    outcome = sync(QStringLiteral("create Apps and move docs-gamma"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QString appsPath = QDir(m_docs).relativeFilePath(Thoughts::ProjectConfiguration::path(m_docs, m_appsId));
    const QByteArray published = serverSource(appsPath);
    QVERIFY2(published.contains("docs-gamma"), published.constData());
    QStringList problems;
    if (const QString problem = manifestProblem(m_appsId); !problem.isEmpty()) problems.append(QStringLiteral("Apps after the move: ") + problem);

    // Q2: a later global publication must leave Fedora's Apps manifest alone.
    // Any change to which PDFs the Library builds makes Sync rediscover every
    // source; losing the accepted source inventory does the same.
    QVERIFY(QFile::remove(m_repository->m_sourceManifestPath));
    outcome = sync(QStringLiteral("rediscover all sources"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    if (serverSource(appsPath) != published) problems.append(QStringLiteral("a global publication replaced Fedora's Apps manifest with:\n") + QString::fromUtf8(serverSource(appsPath)));

    // A Project file written here and a Library Project created here both
    // reach Fedora, and a Sync between creating a Project and moving a folder
    // into it settles the same way.
    const Project delta {createProject(QStringLiteral("Delta"), QStringLiteral("docs-delta"), QStringLiteral("markdown/source/notes.md"), "# Delta\n\nMoves later.\n"),
                         QStringLiteral("docs-delta"), QStringLiteral("delta_notes.pdf")};
    QVERIFY(!delta.id.isEmpty());
    const QString later = addLibraryProject(QStringLiteral("Later apps"));
    QVERIFY(!later.isEmpty());
    outcome = sync(QStringLiteral("create Delta and Later apps"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
    QVERIFY2(!serverSource(QDir(m_docs).relativeFilePath(Thoughts::ProjectConfiguration::path(m_docs, delta.id))).isEmpty(), "Delta's Project file was never published");
    error = moveSourceFolder(delta.id, delta.owner, later);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    outcome = sync(QStringLiteral("move docs-delta into Later apps"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(serverSource(QDir(m_docs).relativeFilePath(Thoughts::ProjectConfiguration::path(m_docs, later))).contains("docs-delta"));
    if (const QString problem = manifestProblem(later); !problem.isEmpty()) problems.append(QStringLiteral("Later apps: ") + problem);
    if (serverSource(appsPath) != published) problems.append(QStringLiteral("Fedora's Apps manifest changed"));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
    VERIFY_SETTLED(QStringLiteral("move docs-delta into Later apps"));
    QCOMPARE(manifestProblem(m_appsId), QString());

    // A Project created while a Sync rediscovers every source waits for its
    // queued creation: publishing this computer's copy first would make
    // Fedora refuse the creation ("This project already exists").
    QString concurrent;
    ProgressHook hook;
    hook.onPhase = [&](const QString &phase) {
        if (phase == QLatin1String("preparing") && concurrent.isEmpty()) concurrent = addLibraryProject(QStringLiteral("Concurrent"));
    };
    connect(m_repository, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)), &hook, SLOT(progress(QString, QString, qint64, qint64, QString)));
    QVERIFY(QFile::remove(m_repository->m_sourceManifestPath));
    outcome = sync(QStringLiteral("create a Project while Sync discovers sources"));
    hook.disconnect();
    m_repository->disconnect(&hook);
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(!concurrent.isEmpty());
    outcome = sync(QStringLiteral("send the Project created during the last Sync"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(manifestProblem(concurrent), QString());
    VERIFY_SETTLED(QStringLiteral("send the Project created during the last Sync"));

    // c) A computer already left in that state heals on its next Sync: the
    // stale copy is the manifest this computer wrote when it created Apps.
    forgetProjectManifest(m_appsId, QJsonDocument(Thoughts::ProjectConfiguration::empty(m_docs, m_appsId, QStringLiteral("Apps"))).toJson());
    outcome = sync(QStringLiteral("heal the stale Apps manifest"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(serverSource(appsPath), published);
    QCOMPARE(manifestProblem(m_appsId), QString());
    VERIFY_SETTLED(QStringLiteral("heal the stale Apps manifest"));

    // The state the user left it in by hand: Fedora's bytes, still no base.
    forgetProjectManifest(m_appsId, published);
    outcome = sync(QStringLiteral("adopt Fedora's Apps manifest"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(serverSource(appsPath), published);
    QCOMPARE(manifestProblem(m_appsId), QString());
    VERIFY_SETTLED(QStringLiteral("adopt Fedora's Apps manifest"));
}

QJsonObject DocumentPipelineE2ETest::syncCommandResult(const Outcome &outcome) const
{
    const bool succeeded = outcome.state == FedoraRepository::ManualSyncState::Succeeded;
    QJsonObject operation {{QStringLiteral("id"), QString(QStringLiteral("e2e-") + uuid())},
                           {QStringLiteral("status"), succeeded ? QStringLiteral("succeeded") : QStringLiteral("failed")},
                           {QStringLiteral("phase"), succeeded ? QStringLiteral("succeeded") : QStringLiteral("failed")}};
    if (!succeeded) {
        operation.insert(QStringLiteral("last_error_code"), m_repository->manualSyncErrorCode());
        operation.insert(QStringLiteral("last_error_message"), outcome.message);
    } else if (!m_repository->manualSyncWarningCode().isEmpty()) {
        // What the shell keeps with a succeeded Sync's run: the repository's warning.
        operation.insert(QStringLiteral("warning_code"), m_repository->manualSyncWarningCode());
        operation.insert(QStringLiteral("warning_message"), m_repository->manualSyncWarning());
    }
    return Thoughts::syncCommandOperationResult(operation);
}

QJsonObject DocumentPipelineE2ETest::proposalRequest(const QString &path, int line, const QByteArray &replacement) const
{
    return {{QStringLiteral("command"), QStringLiteral("create-proposal")},
            {QStringLiteral("source"), path},
            {QStringLiteral("expected_source_sha256"), sha256(readFile(path))},
            {QStringLiteral("start_line"), line},
            {QStringLiteral("end_line"), line + 1},
            {QStringLiteral("replacement"), QString::fromUtf8(replacement)},
            {QStringLiteral("explanation"), QStringLiteral("End-to-end proposal.")},
            {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")},
            {QStringLiteral("request_id"), uuid()}};
}

void DocumentPipelineE2ETest::testProposalAfterFailedBuild()
{
    // 2026-10-14: \Cref without cleveref in an \input fragment failed the
    // PDF build. Fedora named main.tex with the fragment's line, and the Sync
    // before each proposal "failed" although the sources had synced, so the
    // proposal tools refused the proposals that would fix the build.
    const QString owner = QStringLiteral("docs-brokenbuild");
    const QString fragmentRelative = QStringLiteral("tex/fragments/sections/results.tex");
    const QByteArray main = "\\documentclass{article}\n\\begin{document}\nIntroduction text.\n\n\\input{tex/fragments/sections/results.tex}\n\\end{document}\n";
    const QByteArray fragment = "Results text.\n\nSee \\Cref{tab:results} for the numbers.\n";
    const QString id = createLatexProject(QStringLiteral("Broken build"), owner,
                                          {{QStringLiteral("tex/source/paper.tex"), main}, {fragmentRelative, fragment}},
                                          {QStringLiteral("tex/source/paper.tex")});
    QVERIFY(!id.isEmpty());
    m_brokenFragment = QDir(m_docs).filePath(owner + QLatin1Char('/') + fragmentRelative);

    Outcome outcome = sync(QStringLiteral("build fails"));
    QVERIFY2(buildFailed(outcome), qPrintable(QStringLiteral("the PDF build must fail: ") + outcome.message));
    QVERIFY(m_repository->manualSyncErrorCode().isEmpty());
    QCOMPARE(serverSource(owner + QLatin1Char('/') + fragmentRelative), fragment);
    // Fedora names the fragment and the command (thoughts-server 69dfbd6 on).
    const QString details = m_repository->manualSyncWarning();
    QVERIFY2(details.contains(owner + QStringLiteral("/tex/fragments/sections/results.tex:3 failed: Undefined control sequence \\Cref")), qPrintable(details));

    // The Sync before a proposal: sources synced, so the proposal is written and the build failure is a warning.
    const QJsonObject before = syncCommandResult(outcome);
    QCOMPARE(before.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
    QVERIFY2(Thoughts::syncCommandSourcesSynced(before), QJsonDocument(before).toJson().constData());
    const QJsonObject created = Thoughts::authorProposalAfterSync(m_docs, proposalRequest(m_brokenFragment, 3, "See Table~\\ref{tab:results} for the numbers.\n"),
                                                                  QStringLiteral("thoughtsctl"), before);
    QVERIFY2(created.value(QStringLiteral("state")).toString() == QLatin1String("proposal_created"), QJsonDocument(created).toJson().constData());
    QVERIFY(!created.contains(QStringLiteral("error_code")));
    const QJsonArray warnings = created.value(QStringLiteral("warnings")).toArray();
    // The shell keeps the succeeded Sync's warning (manualSyncWarningCode, "warning_code" in
    // fedoraManualSyncState) with its run, and the command protocol passes it on.
    QCOMPARE(before.value(QStringLiteral("warning_code")).toString(), QStringLiteral("pdf_build_failed"));
    QCOMPARE(warnings.size(), 1);
    if (!warnings.isEmpty()) {
        QCOMPARE(warnings.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("pdf_build_failed"));
        QVERIFY(warnings.first().toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("PDF")));
    }
    QVERIFY(readFile(m_brokenFragment).contains("\\agentProposedEdit"));

    // The Sync after it delivers the proposal; its preview renders the fix, so the build passes.
    outcome = sync(QStringLiteral("proposal fixes the build"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(Thoughts::syncCommandSourcesSynced(syncCommandResult(outcome)));
    VERIFY_SETTLED(QStringLiteral("proposal fixes the build"));
}

void DocumentPipelineE2ETest::testFailureOfReplacedRenderer()
{
    // 2026-10-14: a server version broke every LaTeX build with a bibliography.
    // Its build failed two PDFs; the version was rolled back and one PDF was
    // rebuilt. The other kept its inputs, so nothing rebuilt it: every Sync
    // ended "PDF build failed" with the old message, which named the PDF that
    // had been rebuilt (the first failure of the shared build).
    // Two Projects, as AgentsVsNetwork and concepts were.
    const QString alphaOwner = QStringLiteral("docs-stalealpha"), betaOwner = QStringLiteral("docs-stalebeta");
    const QByteArray good = "\\documentclass{article}\n\\begin{document}\nResults text.\n\\end{document}\n";
    const QByteArray broken = "\\documentclass{article}\n\\begin{document}\nSee \\Cref{tab:results}.\n\\end{document}\n";
    const QHash<QString, QString> before = library();
    QVERIFY(!createLatexProject(QStringLiteral("Stale alpha"), alphaOwner, {{QStringLiteral("tex/source/alpha.tex"), good}}, {QStringLiteral("tex/source/alpha.tex")}).isEmpty());
    QVERIFY(!createLatexProject(QStringLiteral("Stale beta"), betaOwner, {{QStringLiteral("tex/source/beta.tex"), good}}, {QStringLiteral("tex/source/beta.tex")}).isEmpty());
    Outcome outcome = sync(QStringLiteral("stale failure: both build"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QStringList added;
    const QHash<QString, QString> built = library();
    for (auto it = built.cbegin(); it != built.cend(); ++it)
        if (!before.contains(it.key())) added.append(it.key());
    std::sort(added.begin(), added.end());
    QCOMPARE(added, (QStringList {QStringLiteral("stalealpha_alpha.pdf"), QStringLiteral("stalebeta_beta.pdf")}));
    const QHash<QString, QString> sources {{added.at(0), QDir(m_docs).filePath(alphaOwner + QStringLiteral("/tex/source/alpha.tex"))},
                                           {added.at(1), QDir(m_docs).filePath(betaOwner + QStringLiteral("/tex/source/beta.tex"))}};
    const auto buildOf = [this](const QString &pdf) { return renderStatus(pdf).value(QStringLiteral("build")).toObject(); };

    // One build fails both PDFs.
    for (const QString &source : sources) QVERIFY(writeFile(source, broken));
    outcome = sync(QStringLiteral("stale failure: both fail"));
    QVERIFY2(buildFailed(outcome), qPrintable(outcome.message));
    const QString sharedFailure = buildOf(added.at(0)).value(QStringLiteral("id")).toString();
    QCOMPARE(buildOf(added.at(1)).value(QStringLiteral("id")).toString(), sharedFailure);
    const QJsonArray sharedFailures = buildOf(added.at(0)).value(QStringLiteral("result")).toObject().value(QStringLiteral("failed_documents")).toArray();
    QCOMPARE(sharedFailures.size(), 2);
    // The PDF listed first is fixed and rebuilt, as AgentsVsNetwork was; the other keeps failing.
    const QString fixedPdf = sharedFailures.first().toObject().value(QStringLiteral("pdf_filename")).toString();
    QVERIFY(sources.contains(fixedPdf));
    const QString stuckPdf = fixedPdf == added.at(0) ? added.at(1) : added.at(0);
    const QString stuckFile = QFileInfo(sources.value(stuckPdf)).fileName();
    const QString fixedFile = QFileInfo(sources.value(fixedPdf)).fileName();
    QVERIFY(writeFile(sources.value(fixedPdf), good));
    // (This Sync ends with the successful build; the other failure shows again from the next one.)
    outcome = sync(QStringLiteral("stale failure: first PDF fixed"));
    QCOMPARE(renderStatus(fixedPdf).value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    QCOMPARE(buildOf(stuckPdf).value(QStringLiteral("id")).toString(), sharedFailure);
    outcome = sync(QStringLiteral("stale failure: other PDF still fails"));
    QVERIFY2(buildFailed(outcome), qPrintable(outcome.message));
    // The Sync names the failure of the PDF that still fails, not the fixed one's.
    QString details = m_repository->manualSyncWarning();
    QVERIFY2(details.contains(stuckFile) && !details.contains(fixedFile), qPrintable(details));

    // The renderer is replaced (a deploy or rollback). The sources are still
    // broken: Fedora builds the PDF once more and the failure stands.
    QVERIFY(rewriteServerBuild(sharedFailure, [](QString &, QJsonObject &progress, QJsonObject &) {
        progress.insert(QStringLiteral("renderer_identity"), QStringLiteral("replaced-renderer"));
    }));
    QCOMPARE(renderStatus(stuckPdf).value(QStringLiteral("status")).toString(), QStringLiteral("out_of_sync"));
    outcome = sync(QStringLiteral("stale failure: renderer replaced, sources broken"));
    QVERIFY2(buildFailed(outcome), qPrintable(outcome.message));
    details = m_repository->manualSyncWarning();
    QVERIFY2(details.contains(stuckFile), qPrintable(details));
    const QString retried = buildOf(stuckPdf).value(QStringLiteral("id")).toString();
    QVERIFY(!retried.isEmpty() && retried != sharedFailure);
    outcome = sync(QStringLiteral("stale failure: a current failure is not retried"));
    QVERIFY2(buildFailed(outcome), qPrintable(outcome.message));
    QCOMPARE(buildOf(stuckPdf).value(QStringLiteral("id")).toString(), retried);

    // Fixed, it builds. Then its build at these inputs is recorded as failed
    // by a renderer that is gone, as the rolled-back version's build was.
    QVERIFY(writeFile(sources.value(stuckPdf), good));
    outcome = sync(QStringLiteral("stale failure: other PDF fixed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QString lastGood = buildOf(stuckPdf).value(QStringLiteral("id")).toString();
    QVERIFY(!lastGood.isEmpty());
    const QString regression = QStringLiteral("compiler_failure: Source %1 failed during LaTeX compilation; latexmk exit 12")
                                   .arg(QDir(m_docs).relativeFilePath(sources.value(stuckPdf)));
    QVERIFY(rewriteServerBuild(lastGood, [&](QString &status, QJsonObject &progress, QJsonObject &result) {
        status = QStringLiteral("attention");
        progress.insert(QStringLiteral("renderer_identity"), QStringLiteral("rolled-back-renderer"));
        result.insert(QStringLiteral("attention"), true);
        result.insert(QStringLiteral("failed_documents"), QJsonArray {QJsonObject {{QStringLiteral("code"), QStringLiteral("compiler_failure")},
                                                                                  {QStringLiteral("message"), regression},
                                                                                  {QStringLiteral("pdf_filename"), stuckPdf}}});
    }));
    // Nothing changed in the sources: the next Sync rebuilds it with the
    // current renderer, which succeeds, instead of reporting the old failure.
    outcome = sync(QStringLiteral("stale failure: a gone renderer's failure"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message + QLatin1Char('\n') + m_repository->manualSyncDetails()));
    const QJsonObject status = renderStatus(stuckPdf);
    QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    QVERIFY(status.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString() != lastGood);
    VERIFY_SETTLED(QStringLiteral("stale failure: a gone renderer's failure"));
}

bool DocumentPipelineE2ETest::rewriteServerBuild(const QString &buildId, const std::function<void(QString &, QJsonObject &, QJsonObject &)> &edit) const
{
    const QString connection = QStringLiteral("e2e-server-state");
    bool written = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QDir::current().absoluteFilePath(QStringLiteral("state/thoughts.db")));
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=10000"));
        if (database.open()) {
            QSqlQuery read(database);
            read.prepare(QStringLiteral("SELECT status, progress, result FROM build_jobs WHERE id = ?"));
            read.addBindValue(buildId);
            if (read.exec() && read.next()) {
                QString status = read.value(0).toString();
                QJsonObject progress = QJsonDocument::fromJson(read.value(1).toByteArray()).object();
                QJsonObject result = QJsonDocument::fromJson(read.value(2).toByteArray()).object();
                read.finish();
                edit(status, progress, result);
                QSqlQuery write(database);
                write.prepare(QStringLiteral("UPDATE build_jobs SET status = ?, progress = ?, result = ?, updated_at = ? WHERE id = ?"));
                write.addBindValue(status);
                write.addBindValue(QString::fromUtf8(QJsonDocument(progress).toJson(QJsonDocument::Compact)));
                write.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
                // Render checks cache by the newest build update.
                write.addBindValue(QDateTime::currentDateTimeUtc().addSecs(1).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz000")));
                write.addBindValue(buildId);
                written = write.exec() && write.numRowsAffected() == 1;
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return written;
}

void DocumentPipelineE2ETest::testProposalBlockedBySourceConflict()
{
    // A Sync that leaves a source conflict open still blocks proposals.
    QVERIFY(!m_brokenFragment.isEmpty());
    const Outcome outcome = sync(QStringLiteral("conflict open"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Failed, "the conflict must still be open");
    QCOMPARE(m_repository->manualSyncErrorCode(), QStringLiteral("sync_failed"));
    const QJsonObject before = syncCommandResult(outcome);
    QVERIFY(!Thoughts::syncCommandSourcesSynced(before));
    const QByteArray original = readFile(m_brokenFragment);
    const QJsonObject refused = Thoughts::authorProposalAfterSync(m_docs, proposalRequest(m_brokenFragment, 1, "Results text, revised.\n"),
                                                                  QStringLiteral("thoughtsctl"), before);
    QCOMPARE(refused.value(QStringLiteral("error_code")).toString(), QStringLiteral("sync_before_failed"));
    QVERIFY(!refused.contains(QStringLiteral("warnings")));
    QCOMPARE(readFile(m_brokenFragment), original);
}

void DocumentPipelineE2ETest::testUnpublishedProjectEditIsNotOverwritten()
{
    // b) A local manifest without a base that is not this computer's own
    // copy (here edited by hand) must never be published over Fedora's.
    QVERIFY(!m_appsId.isEmpty());
    const QString file = Thoughts::ProjectConfiguration::path(m_docs, m_appsId);
    const QString path = QDir(m_docs).relativeFilePath(file);
    const QByteArray published = serverSource(path);
    QVERIFY(published.contains("docs-gamma"));
    QJsonObject edited = QJsonDocument::fromJson(published).object();
    edited.insert(QStringLiteral("name"), QStringLiteral("Apps, renamed by hand"));
    edited.insert(QStringLiteral("source_folders"), QJsonArray {edited.value(QStringLiteral("source_folders")).toArray().first()});
    edited.insert(QStringLiteral("documents"), QJsonArray {});
    edited.insert(QStringLiteral("document_names"), QJsonObject {});
    const QByteArray local = QJsonDocument(edited).toJson();
    forgetProjectManifest(m_appsId, local);
    const Outcome outcome = sync(QStringLiteral("unbased hand edit"));
    qInfo().noquote() << "E2E unbased hand edit:" << outcome.message.left(600);
    QCOMPARE(serverSource(path), published);
    QCOMPARE(readFile(file), local);
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Failed, "the clash must be reported");
    // Reported as a source conflict to resolve, like any other clash.
    const QJsonObject state = readJson(QDir(m_repository->m_stateRoot).filePath(QStringLiteral("source-publication.json")));
    QCOMPARE(state.value(QStringLiteral("conflicts")).toObject().value(path).toObject().value(QStringLiteral("local_sha256")).toString(), sha256(local));
}

void DocumentPipelineE2ETest::testDefaultFolderDocuments()
{
    // g) Markdown and LaTeX documents in each Project's default folder. Both
    // Projects hold notes.md there; each builds its own Library PDF.
    const QByteArray standalone = "\\documentclass{article}\n\\begin{document}\n\\section{Chapter}\nA default-folder \\LaTeX{} document.\n\\end{document}\n";
    QStringList outputs;
    QHash<QString, QString> outputsByProject;
    // The files of the documents removed above stay listed, unchecked, and
    // stay removed when Manage documents applies another change.
    for (Project *project : {&m_alpha, &m_beta}) {
        const QString owner = QDir(m_docs).relativeFilePath(QFileInfo(Thoughts::ProjectConfiguration::path(m_docs, project->id)).absolutePath());
        QVERIFY2(Thoughts::SourceOwner::isDefaultFolder(owner), qPrintable(owner));
        QVERIFY(writeFile(QDir(m_docs).filePath(owner + QStringLiteral("/notes.md")), "# Notes\n\nA default-folder Markdown document.\n"));
        QString error = manageDocument(*project, owner + QStringLiteral("/notes.md"), true);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString notes = declaredOutput(*project, QStringLiteral("notes.md"));
        QCOMPARE(notes, Thoughts::SourceOwner::defaultFolderOutput(owner + QStringLiteral("/notes.md")));
        QCOMPARE(declaredDocument(*project, QStringLiteral("notes.md")).value(QStringLiteral("source")).toObject().value(QStringLiteral("folder_id")),
                 configuration(*project).value(QStringLiteral("default_folder_id")));
        outputs.append(notes);
        outputsByProject.insert(notes, project->id);
        if (project == &m_alpha) {
            QVERIFY(writeFile(QDir(m_docs).filePath(owner + QStringLiteral("/chapter/paper.tex")), standalone));
            error = manageDocument(*project, owner + QStringLiteral("/chapter/paper.tex"), true);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const QString paper = declaredOutput(*project, QStringLiteral("chapter/paper.tex"));
            QCOMPARE(paper, Thoughts::SourceOwner::defaultFolderOutput(owner + QStringLiteral("/chapter/paper.tex")));
            outputs.append(paper);
            outputsByProject.insert(paper, project->id);
        }
    }
    QCOMPARE(QSet<QString>(outputs.cbegin(), outputs.cend()).size(), outputs.size());

    const Outcome outcome = sync(QStringLiteral("default folder documents"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QHash<QString, QString> after = library();
    for (const QString &output : std::as_const(outputs)) {
        QVERIFY2(after.value(output).size() == 64, qPrintable(output + QStringLiteral(": ") + QStringList(after.keys()).join(QLatin1Char(','))));
        Project &project = outputsByProject.value(output) == m_alpha.id ? m_alpha : m_beta;
        QVERIFY2(catalogueFiles(project).contains(output), qPrintable(output));
        project.pdfs.append(output);
    }
    VERIFY_SETTLED(QStringLiteral("default folder documents"));
}

void DocumentPipelineE2ETest::testDefaultFolderDocumentCheckedAfterItsFileSynced()
{
    // g) Like ETH: Beta's only built document is in its connected folder. A
    // default-folder file reaches Fedora in one Sync; checking it later changes
    // only the Project file, whose earlier builds tie it to the connected
    // folder alone. That Sync must still send the new document's build inputs
    // (it published project-scoped for beta_notes.pdf and failed with
    // "affected target has no exact build input").
    const QString owner = QDir(m_docs).relativeFilePath(QFileInfo(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id)).absolutePath());
    const QString source = owner + QStringLiteral("/report_comparison.tex");
    QVERIFY(writeFile(QDir(m_docs).filePath(source), "\\documentclass{article}\n\\begin{document}\nA later default-folder document.\n\\end{document}\n"));
    Outcome outcome = sync(QStringLiteral("upload undeclared default-folder file"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QString error = manageDocument(m_beta, source, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString target = declaredOutput(m_beta, QStringLiteral("report_comparison.tex"));
    QCOMPARE(target, Thoughts::SourceOwner::defaultFolderOutput(source));
    outcome = sync(QStringLiteral("check default-folder document"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(catalogueFiles(m_beta).contains(target), qPrintable(catalogueFiles(m_beta).join(QLatin1Char(','))));
    QCOMPARE(library().value(target).size(), 64);
    VERIFY_SETTLED(QStringLiteral("check default-folder document"));
}

void DocumentPipelineE2ETest::testRestoreRemovedDocument()
{
    // e) A removed document comes back under its kept identity and PDF name.
    // Later Manage documents changes left it removed; checking it builds it
    // back into Alpha.
    const QString alias = QStringLiteral("docs-alpha/tex/source/paper.tex");
    QVERIFY(declaredDocument(m_alpha, QStringLiteral("tex/source/paper.tex")).isEmpty());
    QVERIFY2(!catalogueFiles(m_alpha).contains(m_latexTarget), qPrintable(catalogueFiles(m_alpha).join(QLatin1Char(','))));
    const QString error = manageDocument(m_alpha, alias, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(declaredDocument(m_alpha, QStringLiteral("tex/source/paper.tex")).value(QStringLiteral("document_id")).toString(), m_removedLatexId);
    const Outcome outcome = sync(QStringLiteral("restore LaTeX document"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(catalogueFiles(m_alpha).contains(m_latexTarget), qPrintable(catalogueFiles(m_alpha).join(QLatin1Char(','))));
    const QString restored = library().value(m_latexTarget);
    QCOMPARE(restored.size(), 64);
    VERIFY_SETTLED(QStringLiteral("restore LaTeX document"));

    // The restored PDF is a live build target again: editing it rebuilds it.
    QVERIFY(writeFile(QDir(m_docs).filePath(alias), "\\documentclass{article}\n\\begin{document}\nThe restored paper, edited.\n\\end{document}\n"));
    const Outcome edited = sync(QStringLiteral("edit restored LaTeX document"));
    QVERIFY2(edited.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(edited.message));
    QVERIFY(library().value(m_latexTarget).size() == 64 && library().value(m_latexTarget) != restored);
    VERIFY_SETTLED(QStringLiteral("edit restored LaTeX document"));
}

void DocumentPipelineE2ETest::testAnchorRemovedComment()
{
    // g) A comment whose source marker vanishes without a Delete stays as
    // "Anchor removed"; Restore puts the marker back and the comment is linked
    // again; an explicit Delete removes the marker and the comment for good.
    // Runs right after the LaTeX document is added, on its source and PDF.
    const QString relative = QStringLiteral("docs-alpha/tex/source/paper.tex");
    const QString path = QDir(m_docs).filePath(relative);
    const QString target = m_latexTarget;
    QVERIFY2(target.endsWith(QLatin1String(".pdf")), qPrintable(target));
    Outcome outcome;
    const QString id = QStringLiteral("e2e-anchor-") + uuid().left(8);
    const QString key = QStringLiteral("logical_comment\ncomments\n") + id;
    const QByteArray original = readFile(path);
    QVERIFY2(original.contains("A new \\LaTeX{} document."), original.constData());
    const QByteArray markerText = QByteArray("\\CommentAnchor{") + id.toUtf8() + QByteArray("}{End-to-end comment}%");
    QByteArray linkedSource = original;
    linkedSource.replace("\\begin{document}\n", "\\providecommand{\\CommentAnchor}[2]{}\n\\begin{document}\n");
    linkedSource.replace(QByteArray("A new \\LaTeX{} document."), QByteArray(markerText + "\nA new \\LaTeX{} document."));
    QVERIFY(writeFile(path, linkedSource));
    const int markerLine = QString::fromUtf8(linkedSource).split(QLatin1Char('\n')).indexOf(QStringLiteral("\\CommentAnchor{%1}{End-to-end comment}%").arg(id)) + 1;
    QVERIFY(markerLine > 0);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject comment{{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id}, {QStringLiteral("thread_root_id"), id},
        {QStringLiteral("parent_id"), QJsonValue::Null}, {QStringLiteral("revision"), 1}, {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now},
        {QStringLiteral("origin_id"), QStringLiteral("thoughts-mac")}, {QStringLiteral("deleted"), false}, {QStringLiteral("contents"), QStringLiteral("End-to-end comment")},
        {QStringLiteral("author"), QStringLiteral("E2E")}, {QStringLiteral("subtype"), QStringLiteral("Text")},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QFileInfo(target).completeBaseName()}, {QStringLiteral("document_id"), target},
            {QStringLiteral("pdf_sha256"), library().value(target)}, {QStringLiteral("page_index"), 0}, {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.2, 0.2}}}},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), id},
            {QStringLiteral("source_file"), relative}, {QStringLiteral("source_line"), markerLine}, {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}}}};
    QVERIFY(m_repository->enqueueLogicalComment(comment));
    const auto durable = [&] { return m_repository->effectiveProjections(true, false).value(key).toObject(); };
    const auto durableScope = [&] { return durable().value(QStringLiteral("scope")).toObject(); };

    outcome = sync(QStringLiteral("linked comment"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(durableScope().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"), 15000);

    // The marker vanishes in an ordinary edit: the server keeps the comment as anchor removed.
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace(QByteArray(markerText + "\n"), QByteArray())));
    outcome = sync(QStringLiteral("marker removed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(durableScope().value(QStringLiteral("reason")).toString(), QStringLiteral("anchor_removed"), 15000);
    QVERIFY2(!durable().value(QStringLiteral("deleted")).toBool(), QJsonDocument(durable()).toJson().constData());
    QCOMPARE(durableScope().value(QStringLiteral("state")).toString(), QStringLiteral("needs_attention"));
    QCOMPARE(durableScope().value(QStringLiteral("source_file")).toString(), relative);
    QCOMPARE(durableScope().value(QStringLiteral("source_line")).toInt(), markerLine);
    QVERIFY(!durableScope().value(QStringLiteral("anchor_removed_at")).toString().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m_repository->effectiveProjections().value(key).toObject().value(QStringLiteral("scope")).toObject().value(QStringLiteral("reason")).toString(),
                              QStringLiteral("anchor_removed"), 15000);
    VERIFY_SETTLED(QStringLiteral("marker removed"));

    // Restore re-inserts the marker at its last known line; the next Sync relinks the comment.
    const QVariantList restored = m_repository->restoreRemovedAnchors({id});
    QCOMPARE(restored.size(), 1);
    QVERIFY2(restored.first().toMap().value(QStringLiteral("success")).toBool(), qPrintable(restored.first().toMap().value(QStringLiteral("message")).toString()));
    QVERIFY(readFile(path).contains(markerText));
    outcome = sync(QStringLiteral("anchor restored"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(durableScope().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"), 15000);
    QVERIFY(!durableScope().contains(QStringLiteral("anchor_removed_at")));

    // An explicit Delete removes the marker from the source and the comment for good.
    QJsonObject deletion = durable();
    deletion.insert(QStringLiteral("deleted"), true);
    deletion.insert(QStringLiteral("deletion_reason"), QStringLiteral("user_deleted"));
    deletion.insert(QStringLiteral("revision"), deletion.value(QStringLiteral("revision")).toInteger() + 1);
    deletion.insert(QStringLiteral("updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    deletion.insert(QStringLiteral("origin_id"), QStringLiteral("thoughts-mac"));
    QVERIFY(m_repository->enqueueLogicalComment(deletion));
    QVERIFY2(!readFile(path).contains(id.toUtf8()), readFile(path).constData());
    outcome = sync(QStringLiteral("comment deleted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_VERIFY_WITH_TIMEOUT(durable().value(QStringLiteral("deleted")).toBool(), 15000);
    QCOMPARE(durable().value(QStringLiteral("deletion_reason")).toString(), QStringLiteral("user_deleted"));
    QVERIFY(!readFile(path).contains(id.toUtf8()));
    VERIFY_SETTLED(QStringLiteral("comment deleted"));
}

void DocumentPipelineE2ETest::testProposalDecisions()
{
    // h) Agent-proposal decisions (protocol/agent-proposals.md §10). A desktop
    // Accept records a decision claim that the next publication carries and the
    // answer expires; the server retires the proposal. A wrapper removed by a
    // plain edit is "removed outside Thoughts" on a server with
    // proposal_decisions_v1: Restore Proposal brings it back as pending and
    // Dismiss retires it. An older server retires both kinds itself.
    const QString relative = QStringLiteral("docs-alpha/tex/source/paper.tex");
    const QString path = QDir(m_docs).filePath(relative);
    const QString key = QStringLiteral("proposal\nproposals\n");
    const QString database = m_repository->proposalDecisionDatabase();
    const QByteArray line("A new \\LaTeX{} document.\n");
    const QByteArray original = readFile(path);
    QVERIFY2(original.contains(line), original.constData());
    const auto wrapper = [](const QString &id, const QByteArray &before, const QByteArray &after) -> QByteArray {
        return QByteArray("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{") + id.toUtf8()
            + QByteArray("}{End-to-end proposal.\n}{") + before + QByteArray("}{") + after + QByteArray("}");
    };
    const QByteArray acceptedLine("An accepted \\LaTeX{} document.\n");
    const QByteArray proposedLine("A proposed \\LaTeX{} document.\n");
    const auto row = [&](const QString &id) { return m_repository->effectiveProjections().value(key + id).toObject(); };
    const auto sha = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    Outcome outcome;

    // Accept in Thoughts: claim written with the source, sent and expired by the Sync.
    const QString accepted = QStringLiteral("e2e-accept-") + uuid().left(8);
    QVERIFY(writeFile(path, QByteArray(original).replace(line, wrapper(accepted, line, acceptedLine))));
    outcome = sync(QStringLiteral("proposal created"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(row(accepted).value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    Thoughts::AgentProposalDecisionOptions options;
    options.claimDatabase = database;
    QString error;
    QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(m_docs, path, sha(readFile(path)), {accepted}, QStringLiteral("accept"), &error, nullptr, nullptr, nullptr, options), qPrintable(error));
    QCOMPARE(Thoughts::ProposalDecisions::storedClaims(database).size(), 1);
    outcome = sync(QStringLiteral("proposal accepted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(Thoughts::ProposalDecisions::storedClaims(database).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(row(accepted).isEmpty(), 15000); // retired (deleted rows leave the projections)
    QVERIFY(readFile(path).contains(acceptedLine));
    VERIFY_SETTLED(QStringLiteral("proposal accepted"));

    // Removed by a plain edit (the text looks edited, not decided).
    const QString removed = QStringLiteral("e2e-removed-") + uuid().left(8);
    const QByteArray acceptedSource = readFile(path);
    const QByteArray second = wrapper(removed, acceptedLine, proposedLine);
    QVERIFY(writeFile(path, QByteArray(acceptedSource).replace(acceptedLine, second)));
    outcome = sync(QStringLiteral("second proposal"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(row(removed).value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace(second, "A hand-edited \\LaTeX{} document.\n")));
    outcome = sync(QStringLiteral("proposal removed by hand"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTest::qWait(1000);
    if (row(removed).isEmpty()) {
        qInfo() << "E2E server without proposal_decisions_v1: the removal retired the proposal; Restore and Dismiss are not exercised.";
        VERIFY_SETTLED(QStringLiteral("proposal removed by hand"));
        return;
    }
    QCOMPARE(row(removed).value(QStringLiteral("status")).toString(), QStringLiteral("removed_outside"));
    // Both texts were edited: Restore refuses and leaves the source alone.
    const QByteArray handEdited = readFile(path);
    QVariantList results = m_repository->restoreRemovedProposals({removed});
    QCOMPARE(results.first().toMap().value(QStringLiteral("code")).toString(), QStringLiteral("text_changed"));
    QCOMPARE(readFile(path), handEdited);
    // With the original text back, Restore puts the wrapper there and the Sync makes it pending again.
    QVERIFY(writeFile(path, QByteArray(handEdited).replace("A hand-edited \\LaTeX{} document.\n", acceptedLine)));
    outcome = sync(QStringLiteral("original text back"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    results = m_repository->restoreRemovedProposals({removed});
    QVERIFY2(results.first().toMap().value(QStringLiteral("success")).toBool(), qPrintable(results.first().toMap().value(QStringLiteral("message")).toString()));
    QVERIFY(readFile(path).contains(removed.toUtf8()));
    outcome = sync(QStringLiteral("proposal restored"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(row(removed).value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    // Removed by hand again, then dismissed: retired everywhere, the source is untouched.
    QVERIFY(readFile(path).contains(second));
    // An edit that is neither the accepted nor the rejected text, so a server in compatible mode keeps it too.
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace(second, "An edited-again \\LaTeX{} document.\n")));
    outcome = sync(QStringLiteral("proposal removed again"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(row(removed).value(QStringLiteral("status")).toString(), QStringLiteral("removed_outside"), 15000);
    // A plain edit that leaves exactly the accepted text: a server in compatible
    // mode still retires it as accepted; in strict mode only a claim decides.
    const QString handAccepted = QStringLiteral("e2e-hand-") + uuid().left(8);
    const QByteArray third = wrapper(handAccepted, line, "A hand-accepted \\LaTeX{} document.\n");
    QVERIFY(readFile(path).contains("\\end{document}"));
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace("\\end{document}", QByteArray(third + "\\end{document}"))));
    outcome = sync(QStringLiteral("third proposal"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QTRY_COMPARE_WITH_TIMEOUT(row(handAccepted).value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace(third, "A hand-accepted \\LaTeX{} document.\n")));
    outcome = sync(QStringLiteral("third proposal accepted by hand"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const bool strict = QStringList{QStringLiteral("1"), QStringLiteral("true"), QStringLiteral("yes")}.contains(qEnvironmentVariable("THOUGHTS_E2E_PROPOSAL_DECISIONS_STRICT").toLower());
    if (strict) {
        QTRY_COMPARE_WITH_TIMEOUT(row(handAccepted).value(QStringLiteral("status")).toString(), QStringLiteral("removed_outside"), 15000);
        QCOMPARE(row(handAccepted).value(QStringLiteral("removal")).toObject().value(QStringLiteral("looks_like")).toString(), QStringLiteral("accept"));
        QVERIFY(m_repository->dismissRemovedProposals({handAccepted}).first().toMap().value(QStringLiteral("success")).toBool());
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(row(handAccepted).isEmpty(), 15000);
    }
    // In strict mode two Dismisses are now queued; each travels in its own batch.
    const QByteArray beforeDismiss = readFile(path);
    QVERIFY(m_repository->dismissRemovedProposals({removed}).first().toMap().value(QStringLiteral("success")).toBool());
    QVERIFY(row(removed).isEmpty());
    outcome = sync(QStringLiteral("proposal dismissed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(row(removed).isEmpty());
    QCOMPARE(readFile(path), beforeDismiss);
    VERIFY_SETTLED(QStringLiteral("proposal dismissed"));
}

void DocumentPipelineE2ETest::testInitialProjectsBuild()
{
    // f) Two Projects, each with a Markdown document in its own source folder.
    m_alpha = {createProject(QStringLiteral("Alpha"), QStringLiteral("docs-alpha"), QStringLiteral("markdown/source/intro.md"), "# Alpha\n\nThe first Project.\n"),
               QStringLiteral("docs-alpha"), QStringLiteral("alpha_intro.pdf")};
    m_beta = {createProject(QStringLiteral("Beta"), QStringLiteral("docs-beta"), QStringLiteral("markdown/source/notes.md"), "# Beta\n\nThe second Project.\n"),
              QStringLiteral("docs-beta"), QStringLiteral("beta_notes.pdf")};
    QVERIFY(!m_alpha.id.isEmpty() && !m_beta.id.isEmpty());
    m_alpha.pdfs = {m_alpha.target};
    m_beta.pdfs = {m_beta.target};

    const Outcome first = sync(QStringLiteral("initial"));
    QVERIFY2(first.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(first.message));
    const QHash<QString, QString> pdfs = library();
    QVERIFY2(pdfs.value(m_alpha.target).size() == 64, qPrintable(QStringList(pdfs.keys()).join(QLatin1Char(','))));
    QVERIFY2(pdfs.value(m_beta.target).size() == 64, qPrintable(QStringList(pdfs.keys()).join(QLatin1Char(','))));
    VERIFY_SETTLED(QStringLiteral("initial"));
}

void DocumentPipelineE2ETest::testNewMarkdownDocument()
{
    // a) A new Markdown file, included through Manage documents.
    const QHash<QString, QString> before = library();
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/markdown/source/methods.md")), "# Methods\n\nA new Markdown document.\n\n- one\n- two\n"));
    const QString error = manageDocument(m_alpha, QStringLiteral("docs-alpha/markdown/source/methods.md"), true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_markdownTarget = declaredOutput(m_alpha, QStringLiteral("markdown/source/methods.md"));
    QVERIFY2(m_markdownTarget.endsWith(QLatin1String(".pdf")), qPrintable(m_markdownTarget));

    const Outcome outcome = sync(QStringLiteral("new Markdown"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(library().value(m_markdownTarget).size() == 64, qPrintable(QStringList(library().keys()).join(QLatin1Char(','))));
    m_alpha.pdfs.append(m_markdownTarget);
    QVERIFY(catalogueFiles(m_alpha).contains(m_markdownTarget));
    QVERIFY2(readingTitles(m_alpha).contains(QStringLiteral("methods")), qPrintable(readingTitles(m_alpha).join(QLatin1Char(','))));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("new Markdown"));
}

void DocumentPipelineE2ETest::testNewLatexDocument()
{
    // b) A new LaTeX document in tex/source, included through Manage documents.
    const QHash<QString, QString> before = library();
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/tex/source/paper.tex")),
                      "\\documentclass{article}\n\\begin{document}\n\\section{Paper}\nA new \\LaTeX{} document.\n\\end{document}\n"));
    const QString error = manageDocument(m_alpha, QStringLiteral("docs-alpha/tex/source/paper.tex"), true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(declaredDocument(m_alpha, QStringLiteral("tex/source/paper.tex")).value(QStringLiteral("format")).toString(), QStringLiteral("latex"));

    const Outcome outcome = sync(QStringLiteral("new LaTeX"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QHash<QString, QString> after = library();
    QStringList added;
    for (auto iterator = after.cbegin(); iterator != after.cend(); ++iterator) {
        if (!before.contains(iterator.key())) {
            added.append(iterator.key());
        }
    }
    QCOMPARE(added, QStringList {QStringLiteral("alpha_paper.pdf")});
    m_latexTarget = added.constFirst();
    QCOMPARE(after.value(m_latexTarget).size(), 64);
    m_alpha.pdfs.append(m_latexTarget);
    QVERIFY(catalogueFiles(m_alpha).contains(m_latexTarget));
    QVERIFY2(readingTitles(m_alpha).contains(QStringLiteral("paper")), qPrintable(readingTitles(m_alpha).join(QLatin1Char(','))));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("new LaTeX"));
}

void DocumentPipelineE2ETest::testNestedLatexEntryPoint()
{
    // b2) An entry point outside tex/ builds from its own directory: its inputs
    // anywhere in the folder are published, and intro resolves to paper/intro.tex.
    const QHash<QString, QString> before = library();
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/paper/main.tex")),
                      "\\documentclass{article}\n\\begin{document}\n\\input{intro}\n\\input{../shared/note}\n\\end{document}\n"));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/paper/intro.tex")), "Nested introduction.\n"));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/intro.tex")), "\\errmessage{wrong introduction}\n"));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/shared/note.tex")), "Shared note.\n"));
    const QString error = manageDocument(m_alpha, QStringLiteral("docs-alpha/paper/main.tex"), true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonObject declared = declaredDocument(m_alpha, QStringLiteral("paper/main.tex"));
    QCOMPARE(declared.value(QStringLiteral("format")).toString(), QStringLiteral("latex"));

    const Outcome outcome = sync(QStringLiteral("nested LaTeX"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QHash<QString, QString> after = library();
    QStringList added;
    for (auto iterator = after.cbegin(); iterator != after.cend(); ++iterator) {
        if (!before.contains(iterator.key())) {
            added.append(iterator.key());
        }
    }
    QCOMPARE(added.size(), 1);
    m_nestedTarget = added.constFirst();
    QVERIFY2(m_nestedTarget.startsWith(QLatin1String("alpha_main_")), qPrintable(m_nestedTarget));
    QCOMPARE(after.value(m_nestedTarget).size(), 64);
    m_alpha.pdfs.append(m_nestedTarget);
    QVERIFY(catalogueFiles(m_alpha).contains(m_nestedTarget));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("nested LaTeX"));
}

void DocumentPipelineE2ETest::testImportPdfFile()
{
    // c) Import a PDF file into Beta as the import chooser does. This changes
    // Beta's Project file here and, through the Library operation, on the
    // server, with different JSON formatting: tonight's incident (d) while a
    // second Project (f) owns the alphabetically first PDFs.
    const QHash<QString, QString> before = library();
    const QByteArray alphaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id));
    const QByteArray bytes = pdf(QStringLiteral("Imported Paper"), QStringLiteral("An imported PDF"));
    QTemporaryDir downloads;
    const QString chosen = downloads.filePath(QStringLiteral("Imported Paper.pdf"));
    QVERIFY(writeFile(chosen, bytes));
    const QString error = Thoughts::LibraryProjectStore(m_docs).importPdf(catalogueProject(m_beta), chosen, QFileInfo(chosen).completeBaseName());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (const QJsonValue &value : configuration(m_beta).value(QStringLiteral("documents")).toArray()) {
        if (value.toObject().value(QStringLiteral("format")).toString() == QLatin1String("pdf")) {
            m_importedFile = value.toObject().value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString();
        }
    }
    QVERIFY(m_importedFile.startsWith(QLatin1String("Imported_Paper_")));
    QCOMPARE(Thoughts::LibraryProjectStore(m_docs).pending().size(), 1);

    const Outcome outcome = sync(QStringLiteral("import PDF file"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(library().value(m_importedFile), sha256(bytes));
    m_beta.pdfs.append(m_importedFile);
    QVERIFY(catalogueFiles(m_beta).contains(m_importedFile));
    QVERIFY2(readingTitles(m_beta).contains(QStringLiteral("Imported Paper")), qPrintable(readingTitles(m_beta).join(QLatin1Char(','))));
    // Beta's own documents may be rebuilt: its Project file is their input.
    QCOMPARE(library().value(m_beta.target).size(), 64);
    verifyUntouched(m_alpha, before, alphaBefore);
    VERIFY_SETTLED(QStringLiteral("import PDF file"));
}

void DocumentPipelineE2ETest::testImportPdfLink()
{
    // c) Import from a link: the dialog downloads through a redirect from a
    // local HTTP server, then saves the PDF as the Library does.
    PaperServer papers;
    papers.paper = pdf(QStringLiteral("Linked Paper"), QStringLiteral("A PDF from a link"));
    QVERIFY(papers.listen(QHostAddress::LocalHost));
    LoopbackNetwork network(papers.serverPort());
    const QHash<QString, QString> before = library();
    const QByteArray alphaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id));
    QString importError = QStringLiteral("not imported");
    Thoughts::WebsiteImportDialog dialog(QStringLiteral("Beta"), [&](const QString &path, const QString &name, const QUrl &link) {
        importError = Thoughts::LibraryProjectStore(m_docs).importPdf(catalogueProject(m_beta), path, name, link);
        return importError;
    }, nullptr, &network, QStringLiteral("https://papers.e2e.invalid/papers/latest"));
    dialog.show();
    QTRY_COMPARE_WITH_TIMEOUT(dialog.windowTitle(), QStringLiteral("Confirm import"), 20000);
    QCOMPARE(papers.requests, (QList<QByteArray> {"/papers/latest", "/files/paper.pdf"}));
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("websiteImportName"))->text(), QStringLiteral("Linked Paper"));
    dialog.findChild<QPushButton *>(QStringLiteral("websiteImportNext"))->click();
    QVERIFY2(importError.isEmpty(), qPrintable(importError));
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    for (const QJsonValue &value : configuration(m_beta).value(QStringLiteral("documents")).toArray()) {
        const QString path = value.toObject().value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString();
        if (path.startsWith(QLatin1String("Linked_Paper_"))) {
            m_importedLink = path;
        }
    }
    QVERIFY(!m_importedLink.isEmpty());

    const Outcome outcome = sync(QStringLiteral("import PDF link"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(library().value(m_importedLink), sha256(papers.paper));
    m_beta.pdfs.append(m_importedLink);
    QVERIFY2(readingTitles(m_beta).contains(QStringLiteral("Linked Paper")), qPrintable(readingTitles(m_beta).join(QLatin1Char(','))));
    QCOMPARE(library().value(m_importedFile), before.value(m_importedFile));
    verifyUntouched(m_alpha, before, alphaBefore);
    VERIFY_SETTLED(QStringLiteral("import PDF link"));
}

void DocumentPipelineE2ETest::testUnresolvableLinksDoNotBlockSync()
{
    // 2026-10-12: a pending proposal in docs-concepts used \PdfLink for a PDF
    // of another Project. Freezing the sources raised "explicit PdfLink target
    // is unavailable" and every Sync failed. A link that cannot resolve must
    // affect only its own document: the link stays inactive and Fedora's
    // status of that PDF says so, while every source (that one included) syncs.
    QVERIFY(!m_importedFile.isEmpty() && !m_latexTarget.isEmpty());
    const QString key = QFileInfo(m_importedFile).completeBaseName(); // a PDF of Beta
    const QString relative = QStringLiteral("docs-alpha/tex/source/paper.tex");
    const QString path = QDir(m_docs).filePath(relative);
    const QByteArray original = readFile(path);
    QVERIFY(original.contains("\\end{document}"));
    const QString betaRelative = QStringLiteral("docs-beta/markdown/source/notes.md");
    const QByteArray betaOriginal = readFile(QDir(m_docs).filePath(betaRelative));
    const QHash<QString, QString> before = library();

    // 1. The user's own edit: a \PdfLink to Beta's PDF with a missing anchor
    //    (since thoughts-server baff0ed the build finds Beta's PDF by name; the
    //    anchor keeps the link inactive), and an unrelated edit in the other
    //    Project in the same Sync.
    const QByteArray badLink = "See \\href{https://example.org}{x} and \\PdfLink{" + key.toUtf8() + "}{paper-title}{the imported paper}.\n";
    // \\PdfLink as the Library's manual_style.sty defines it.
    const QByteArray preamble = "\\usepackage{hyperref}\n\\providecommand{\\PdfLink}[3]{\\href{manual-pdflink:\\detokenize{#1}?anchor=\\detokenize{#2}}{#3}}\n";
    QVERIFY(original.contains("\\begin{document}"));
    QVERIFY(writeFile(path, QByteArray(original).replace("\\begin{document}", QByteArray(preamble + "\\begin{document}")).replace("\\end{document}", QByteArray(badLink + "\\end{document}"))));
    QVERIFY(writeFile(QDir(m_docs).filePath(betaRelative), betaOriginal + "\nEdited next to a broken link.\n"));
    Outcome outcome = sync(QStringLiteral("user edit with a broken link"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(serverSource(relative), readFile(path));
    QCOMPARE(serverSource(betaRelative), readFile(QDir(m_docs).filePath(betaRelative)));
    QVERIFY(library().value(m_latexTarget).size() == 64 && library().value(m_latexTarget) != before.value(m_latexTarget));
    QVERIFY(library().value(m_beta.target).size() == 64 && library().value(m_beta.target) != before.value(m_beta.target));
    QJsonObject status = renderStatus(m_latexTarget);
    QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    QVERIFY2(status.value(QStringLiteral("reason")).toString().contains(QStringLiteral("\\PdfLink{%1}{paper-title} stays inactive").arg(key)),
             qPrintable(status.value(QStringLiteral("reason")).toString()));
    QVERIFY2(!renderStatus(m_beta.target).value(QStringLiteral("reason")).toString().contains(QStringLiteral("inactive")),
             qPrintable(renderStatus(m_beta.target).value(QStringLiteral("reason")).toString()));
    VERIFY_SETTLED(QStringLiteral("user edit with a broken link"));

    // 2. A pending proposal written by hand whose replacement has bad links:
    //    a path instead of a PDF key (once "unsafe PdfLink target") and a
    //    Cross link to a PDF no Project has (a folder of no Project alone no
    //    longer breaks it: the build finds a Library PDF by its name).
    const QString id = QStringLiteral("e2e-badlink-") + uuid().left(8);
    const QByteArray replacement = "Proposed: \\PdfLink{docs-beta/" + key.toUtf8() + "}{document-start}{bad key} and \\CrossPdfLink{docs-nowhere/" + key.toUtf8() + "_missing}{document-start}{no such PDF}.\n";
    const QByteArray wrapper = QByteArray("\\agentProposedEdit[created=2026-10-12T14:11:00.000Z,conversation_id=7304c669-0000-4000-8000-000000000001]{") + id.toUtf8()
        + QByteArray("}{A proposal with broken links.\n}{") + badLink + QByteArray("}{") + replacement + QByteArray("}");
    QVERIFY(writeFile(path, QByteArray(readFile(path)).replace(badLink, wrapper)));
    outcome = sync(QStringLiteral("pending proposal with broken links"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(serverSource(relative), readFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(m_repository->effectiveProjections().value(QStringLiteral("proposal\nproposals\n") + id).toObject().value(QStringLiteral("status")).toString(),
                              QStringLiteral("pending"), 15000);
    VERIFY_SETTLED(QStringLiteral("pending proposal with broken links"));

    // Back to the original text: the PDF's status no longer mentions links.
    QVERIFY(writeFile(path, original));
    QVERIFY(writeFile(QDir(m_docs).filePath(betaRelative), betaOriginal));
    outcome = sync(QStringLiteral("broken links removed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    status = renderStatus(m_latexTarget);
    QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    QVERIFY2(!status.value(QStringLiteral("reason")).toString().contains(QStringLiteral("inactive")), qPrintable(status.value(QStringLiteral("reason")).toString()));
    VERIFY_SETTLED(QStringLiteral("broken links removed"));
}

void DocumentPipelineE2ETest::testMarkdownDocumentLinks()
{
    // 2026-10-19: relative Markdown links make the LaTeX document links. A
    // bare key.pdf is the \PdfLink of the Project's imported PDF (frozen as a
    // build input like LaTeX's), a path into another folder a
    // \CrossSourceLink; a link that cannot resolve stays inactive and the
    // PDF's status reports it as the Markdown link it is.
    QVERIFY(!m_importedFile.isEmpty() && !m_latexTarget.isEmpty());
    const QString key = QFileInfo(m_importedFile).completeBaseName(); // a PDF of Beta
    const QString relative = QStringLiteral("docs-beta/markdown/source/notes.md");
    const QString path = QDir(m_docs).filePath(relative);
    const QByteArray original = readFile(path);
    const QByteArray links = "\n[the paper](" + key.toUtf8() + ".pdf), [missing anchor](" + key.toUtf8() + ".pdf#paper-title), "
        "[alpha paper](../../../docs-alpha/tex/source/paper.tex) and [nowhere](../../../docs-nowhere/x.md).\n";
    QVERIFY(writeFile(path, original + links));
    Outcome outcome = sync(QStringLiteral("Markdown document links"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(serverSource(relative), readFile(path));
    QJsonObject status = renderStatus(m_beta.target);
    QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("synced"));
    const QString reason = status.value(QStringLiteral("reason")).toString();
    QVERIFY2(reason.contains(QStringLiteral("Markdown link (%1.pdf#paper-title) stays inactive").arg(key)), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("Markdown link (../../../docs-nowhere/x.md) stays inactive")), qPrintable(reason));
    QVERIFY2(!reason.contains(QStringLiteral("Markdown link (%1.pdf) ").arg(key)) && !reason.contains(QStringLiteral("docs-alpha")), qPrintable(reason));
    QVERIFY2(!reason.contains(QStringLiteral("\\PdfLink")), qPrintable(reason));
    VERIFY_SETTLED(QStringLiteral("Markdown document links"));

    QVERIFY(writeFile(path, original));
    outcome = sync(QStringLiteral("Markdown document links removed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(!renderStatus(m_beta.target).value(QStringLiteral("reason")).toString().contains(QStringLiteral("inactive")),
             qPrintable(renderStatus(m_beta.target).value(QStringLiteral("reason")).toString()));
    VERIFY_SETTLED(QStringLiteral("Markdown document links removed"));
}

void DocumentPipelineE2ETest::testProjectFileChangedOnBothSides()
{
    // d) A Project change that the server applies as a Library operation and
    // this desktop applies to its own copy: both write the same Project file.
    const QHash<QString, QString> before = library();
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    const QString error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("rename"), catalogueProject(m_alpha), {{QStringLiteral("name"), QStringLiteral("Alpha renamed")}});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QByteArray local = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id));

    const Outcome outcome = sync(QStringLiteral("Project changed on both sides"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(configuration(m_alpha).value(QStringLiteral("name")).toString(), QStringLiteral("Alpha renamed"));
    QCOMPARE(catalogueProject(m_alpha).value(QStringLiteral("name")).toString(), QStringLiteral("Alpha renamed"));
    const QByteArray settled = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id));
    qInfo().noquote() << "E2E Project file" << (settled == local ? "kept this desktop's formatting" : "adopted the server's formatting");
    QStringList names = library().keys(), namesBefore = before.keys();
    names.sort();
    namesBefore.sort();
    QCOMPARE(names, namesBefore);
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("Project changed on both sides"));
}

void DocumentPipelineE2ETest::testEditMarkdownDocument()
{
    // e) Editing a document rebuilds only its PDF.
    const QHash<QString, QString> before = library();
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    QVERIFY(writeFile(QDir(m_docs).filePath(QStringLiteral("docs-alpha/markdown/source/methods.md")), "# Methods\n\nAn edited Markdown document.\n\n- one\n- two\n- three\n"));

    const Outcome outcome = sync(QStringLiteral("edit Markdown"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QHash<QString, QString> after = library();
    QVERIFY(after.value(m_markdownTarget).size() == 64 && after.value(m_markdownTarget) != before.value(m_markdownTarget));
    QCOMPARE(after.value(m_latexTarget), before.value(m_latexTarget));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("edit Markdown"));
}

void DocumentPipelineE2ETest::testIncludedFlag()
{
    // e) Excluding a document takes it out of its Project; including it again
    // builds it back in.
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    const QHash<QString, QString> before = library();
    const QString alias = QStringLiteral("docs-alpha/markdown/source/methods.md");
    QString error = manageDocument(m_alpha, alias, false);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(declaredDocument(m_alpha, QStringLiteral("markdown/source/methods.md")).value(QStringLiteral("included")).toBool(true), false);
    Outcome outcome = sync(QStringLiteral("exclude Markdown"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(!catalogueFiles(m_alpha).contains(m_markdownTarget), qPrintable(catalogueFiles(m_alpha).join(QLatin1Char(','))));
    QVERIFY(catalogueFiles(m_alpha).contains(m_alpha.target));
    QCOMPARE(library().value(m_alpha.target).size(), 64);
    QVERIFY2(!readingTitles(m_alpha).contains(QStringLiteral("methods")), qPrintable(readingTitles(m_alpha).join(QLatin1Char(','))));
    // An unchecked document's PDF leaves the Library; Fedora keeps it, and its
    // comments, for when the document is checked again.
    QVERIFY2(!library().contains(m_markdownTarget), qPrintable(QStringList(library().keys()).join(QLatin1Char(','))));
    QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(QStringLiteral("current_pdfs/") + m_markdownTarget)));
    VERIFY_SETTLED(QStringLiteral("exclude Markdown"));

    error = manageDocument(m_alpha, alias, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    outcome = sync(QStringLiteral("include Markdown"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(library().value(m_markdownTarget).size(), 64);
    QVERIFY(catalogueFiles(m_alpha).contains(m_markdownTarget));
    QVERIFY(readingTitles(m_alpha).contains(QStringLiteral("methods")));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("include Markdown"));
}

void DocumentPipelineE2ETest::testDeleteDocuments()
{
    // e) "Remove document" takes the LaTeX document out of Alpha; its file
    // stays, as the dialog promises.
    const QByteArray betaBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, m_beta.id));
    QHash<QString, QString> before = library();
    m_removedLatexId = declaredDocument(m_alpha, QStringLiteral("tex/source/paper.tex")).value(QStringLiteral("document_id")).toString();
    QVERIFY(!m_removedLatexId.isEmpty());
    QString error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("remove_document"), catalogueProject(m_alpha),
                                                                  {{QStringLiteral("document_id"), QStringLiteral("docs-alpha/tex/source/paper.tex")}});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(declaredDocument(m_alpha, QStringLiteral("tex/source/paper.tex")).isEmpty());
    Outcome outcome = sync(QStringLiteral("remove LaTeX document"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(!catalogueFiles(m_alpha).contains(m_latexTarget), qPrintable(catalogueFiles(m_alpha).join(QLatin1Char(','))));
    QVERIFY2(!readingTitles(m_alpha).contains(QStringLiteral("paper")), qPrintable(readingTitles(m_alpha).join(QLatin1Char(','))));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("remove LaTeX document"));

    // e) Remove the nested entry point the same way.
    before = library();
    error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("remove_document"), catalogueProject(m_alpha),
                                                          {{QStringLiteral("document_id"), QStringLiteral("docs-alpha/paper/main.tex")}});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(declaredDocument(m_alpha, QStringLiteral("paper/main.tex")).isEmpty());
    outcome = sync(QStringLiteral("remove nested LaTeX document"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(!catalogueFiles(m_alpha).contains(m_nestedTarget), qPrintable(catalogueFiles(m_alpha).join(QLatin1Char(','))));
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("remove nested LaTeX document"));

    // e) Remove the Markdown document and delete its file.
    before = library();
    error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("remove_document"), catalogueProject(m_alpha),
                                                  {{QStringLiteral("document_id"), QStringLiteral("docs-alpha/markdown/source/methods.md")}});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(QFile::remove(QDir(m_docs).filePath(QStringLiteral("docs-alpha/markdown/source/methods.md"))));
    outcome = sync(QStringLiteral("delete Markdown document"));
    // Fedora still builds the PDF of a removed document whose source changed,
    // and the renderer rejects it, so every later Sync reports a failed build.
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QCOMPARE(catalogueFiles(m_alpha), QStringList {m_alpha.target});
    QCOMPARE(readingTitles(m_alpha), QStringList {QStringLiteral("Alpha notes")});
    QCOMPARE(library().value(m_alpha.target).size(), 64);
    verifyUntouched(m_beta, before, betaBefore);
    VERIFY_SETTLED(QStringLiteral("delete Markdown document"));
}

QString DocumentPipelineE2ETest::createLatexProject(const QString &name, const QString &owner, const QHash<QString, QByteArray> &files, const QStringList &documents)
{
    const QString id = QStringLiteral("new:") + uuid();
    QJsonObject value = Thoughts::ProjectConfiguration::empty(m_docs, id, name);
    const QString folder = uuid();
    QJsonArray folders = value.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject {{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), uuid()}});
    value.insert(QStringLiteral("source_folders"), folders);
    QJsonArray declared;
    QJsonObject names;
    for (const QString &path : documents) {
        const QString document = uuid();
        declared.append(QJsonObject {{QStringLiteral("document_id"), document},
                                     {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), path}}},
                                     {QStringLiteral("format"), QStringLiteral("latex")},
                                     {QStringLiteral("included"), true}});
        names.insert(document, QFileInfo(path).completeBaseName());
    }
    value.insert(QStringLiteral("documents"), declared);
    value.insert(QStringLiteral("document_names"), names);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (!writeFile(QDir(m_docs).filePath(owner + QLatin1Char('/') + it.key()), it.value())) return {};
    }
    return Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(m_docs, id), value).isEmpty() ? id : QString();
}

QJsonObject DocumentPipelineE2ETest::libraryRequest(const QJsonObject &request, QString *error) const
{
    Thoughts::SyncClient client(m_server, m_token);
    QNetworkReply *reply = client.editLibraryProject(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QJsonObject value = QJsonDocument::fromJson(reply->readAll()).object();
    if (reply->error() != QNetworkReply::NoError && error) *error = value.value(QStringLiteral("detail")).toString(reply->errorString());
    reply->deleteLater();
    return value;
}

namespace
{
// The line of text that starts with @p firstWord on a PDF page, normalized and padded slightly.
QRectF textLine(const QString &pdfPath, int page, const QString &firstWord, double padding = 2)
{
    auto document = Poppler::Document::load(pdfPath);
    if (!document || page < 1 || page > document->numPages()) return {};
    auto pdfPage = document->page(page - 1);
    const QSizeF size = pdfPage->pageSizeF();
    QRectF line;
    const auto boxes = pdfPage->textList();
    for (const auto &box : boxes) {
        if (box->text() != firstWord) continue;
        const double middle = box->boundingBox().center().y();
        for (const auto &other : boxes)
            if (qAbs(other->boundingBox().center().y() - middle) < 2) line |= other->boundingBox();
        break;
    }
    if (line.isEmpty()) return {};
    line.adjust(-padding, -padding, padding, padding);
    return QRectF(line.left() / size.width(), line.top() / size.height(), line.width() / size.width(), line.height() / size.height());
}
QJsonArray rectJson(const QRectF &rect)
{
    return QJsonArray{rect.left(), rect.top(), rect.right(), rect.bottom()};
}
}

QString DocumentPipelineE2ETest::uploadAsPhone(const QList<QJsonObject> &payloads, const QList<qint64> &baseRevisions, const QList<QByteArray> &objects) const
{
    Thoughts::SyncClient client(m_server, qgetenv("THOUGHTS_E2E_PHONE_TOKEN"));
    QEventLoop wait;
    for (const QByteArray &object : objects) {
        QNetworkReply *upload = client.uploadObject(object, QByteArrayLiteral("text/plain; charset=utf-8"));
        QObject::connect(upload, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();
        const QString error = upload->error() == QNetworkReply::NoError ? QString() : QString::fromUtf8(upload->readAll());
        upload->deleteLater();
        if (!error.isEmpty()) return error;
    }
    QNetworkReply *sequence = client.deviceSequence();
    QObject::connect(sequence, &QNetworkReply::finished, &wait, &QEventLoop::quit);
    wait.exec();
    qint64 next = QJsonDocument::fromJson(sequence->readAll()).object().value(QStringLiteral("next_device_sequence")).toInteger(1);
    sequence->deleteLater();
    QList<QJsonObject> operations;
    for (int i = 0; i < payloads.size(); ++i) {
        const QJsonObject &payload = payloads[i];
        operations.append({{QStringLiteral("operation_id"), uuid()}, {QStringLiteral("device_sequence"), next++},
            {QStringLiteral("entity_type"), QStringLiteral("review_card")}, {QStringLiteral("collection"), QStringLiteral("cards")},
            {QStringLiteral("entity_id"), payload.value(QStringLiteral("card_id"))}, {QStringLiteral("base_revision"), baseRevisions.value(i)},
            {QStringLiteral("client_timestamp"), payload.value(QStringLiteral("updated_at"))}, {QStringLiteral("payload"), payload}});
    }
    QNetworkReply *upload = client.uploadOperations(operations);
    QObject::connect(upload, &QNetworkReply::finished, &wait, &QEventLoop::quit);
    wait.exec();
    const QByteArray body = upload->readAll();
    const QString error = upload->error() == QNetworkReply::NoError ? QString() : QString::fromUtf8(body);
    upload->deleteLater();
    return error;
}

QJsonObject DocumentPipelineE2ETest::cardProjection(const QString &cardId) const
{
    return m_repository->effectiveProjections().value(QStringLiteral("review_card\ncards\n") + cardId).toObject();
}

bool DocumentPipelineE2ETest::syncUntil(const QString &label, const std::function<bool()> &done, int syncs)
{
    for (int attempt = 0; attempt < syncs; ++attempt) {
        const Outcome outcome = sync(label + QStringLiteral(" #%1").arg(attempt + 1));
        if (outcome.state != FedoraRepository::ManualSyncState::Succeeded) {
            qWarning().noquote() << "E2E" << label << "Sync failed:" << outcome.message;
            return false;
        }
        if (done()) return true;
        QTest::qWait(2000);
    }
    return done();
}

namespace
{
QByteArray figureDocument(const QString &title, const QStringList &figures, const QStringList &fragments = {})
{
    QByteArray text = "\\documentclass{article}\n\\usepackage{graphicx}\n\\begin{document}\n\\section{" + title.toUtf8() + "}\n";
    for (const QString &fragment : fragments) text += "\\input{" + fragment.toUtf8() + "}\n";
    for (const QString &figure : figures) text += "\\includegraphics[width=3cm]{" + figure.toUtf8() + "}\n";
    return text + "\\end{document}\n";
}

QStringList changedPdfs(const QHash<QString, QString> &before, const QHash<QString, QString> &after, const QStringList &among)
{
    QStringList changed;
    for (const QString &filename : among) {
        if (before.value(filename) != after.value(filename)) changed.append(filename);
    }
    changed.sort();
    return changed;
}
}

void DocumentPipelineE2ETest::testSharedAssetPublication()
{
    // 2026-10-14: every full Sync published the 23 docs-concepts PDFs one at a
    // time, about 1 s each, although nothing had changed. A deleted figure
    // stayed pending: its deletion listed every PDF of the Project (any file in
    // tex/assets is a build input of all of them), its accepted baseline only
    // the PDFs that had published it, and that mismatch kept the deletion from
    // ever being acknowledged.
    const QString owner = QStringLiteral("docs-figures");
    const QStringList names {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")};
    QHash<QString, QByteArray> files;
    QStringList documents;
    for (const QString &name : names) {
        documents.append(QStringLiteral("tex/source/%1.tex").arg(name));
        files.insert(documents.constLast(), figureDocument(name, {QStringLiteral("tex/assets/shared.pdf")}));
    }
    files.insert(QStringLiteral("tex/assets/shared.pdf"), pdf(QStringLiteral("shared"), QStringLiteral("Shared figure, first version")));
    files.insert(QStringLiteral("tex/assets/old-figure.pdf"), pdf(QStringLiteral("old"), QStringLiteral("A figure nothing uses")));
    const QString id = createLatexProject(QStringLiteral("Figures"), owner, files, documents);
    QVERIFY(!id.isEmpty());
    Outcome outcome = sync(QStringLiteral("figures: initial"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    Project figures {id, owner, QString(), {}};
    figures.pdfs = catalogueFiles(figures);
    figures.pdfs.sort();
    QCOMPARE(figures.pdfs.size(), names.size());
    for (const QString &filename : std::as_const(figures.pdfs)) QCOMPARE(library().value(filename).size(), 64);
    VERIFY_SETTLED(QStringLiteral("figures: initial"));
    const auto path = [this, &owner](const QString &relative) { return QDir(m_docs).filePath(owner + QLatin1Char('/') + relative); };
    QStringList problems;

    // A shared figure changes: every PDF of the Project, in one publisher run.
    QHash<QString, QString> before = library();
    QVERIFY(writeFile(path(QStringLiteral("tex/assets/shared.pdf")), pdf(QStringLiteral("shared"), QStringLiteral("Shared figure, second version"))));
    outcome = sync(QStringLiteral("figures: shared figure changed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    qInfo().noquote() << "E2E figures: shared figure changed:" << outcome.publisherRuns() << "publisher run(s) for" << outcome.publishedTargets().join(QLatin1Char(',')) << "publication" << outcome.publicationMs() << "ms";
    if (outcome.publishedTargets() != figures.pdfs) problems.append(QStringLiteral("shared figure: published %1").arg(outcome.publishedTargets().join(QLatin1Char(','))));
    if (outcome.publisherRuns() != 1) problems.append(QStringLiteral("shared figure: %1 publisher runs").arg(outcome.publisherRuns()));
    QCOMPARE(changedPdfs(before, library(), figures.pdfs), figures.pdfs);
    VERIFY_SETTLED(QStringLiteral("figures: shared figure changed"));

    // One document's text: one publisher run for the Project (which PDFs
    // read a source is known only to Fedora's build), and only that PDF changes.
    before = library();
    QVERIFY(writeFile(path(QStringLiteral("tex/source/two.tex")), figureDocument(QStringLiteral("two edited"), {QStringLiteral("tex/assets/shared.pdf")})));
    outcome = sync(QStringLiteral("figures: one document edited"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    qInfo().noquote() << "E2E figures: one document edited:" << outcome.publisherRuns() << "publisher run(s) for" << outcome.publishedTargets().join(QLatin1Char(','));
    QString two;
    for (const QString &filename : std::as_const(figures.pdfs)) if (filename.contains(QLatin1String("two"))) two = filename;
    QVERIFY(!two.isEmpty());
    if (outcome.publisherRuns() != 1) problems.append(QStringLiteral("one document: %1 publisher runs").arg(outcome.publisherRuns()));
    QCOMPARE(changedPdfs(before, library(), figures.pdfs), QStringList {two});
    QCOMPARE(serverSource(owner + QStringLiteral("/tex/source/two.tex")), figureDocument(QStringLiteral("two edited"), {QStringLiteral("tex/assets/shared.pdf")}));
    VERIFY_SETTLED(QStringLiteral("figures: one document edited"));

    // Two documents' texts in one Sync: one publisher run for both.
    before = library();
    QStringList edited;
    for (const QString &name : {QStringLiteral("one"), QStringLiteral("three")}) {
        QVERIFY(writeFile(path(QStringLiteral("tex/source/%1.tex").arg(name)), figureDocument(name + QStringLiteral(" edited"), {QStringLiteral("tex/assets/shared.pdf")})));
        for (const QString &filename : std::as_const(figures.pdfs)) if (filename.contains(name)) edited.append(filename);
    }
    edited.sort();
    outcome = sync(QStringLiteral("figures: two documents edited"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    qInfo().noquote() << "E2E figures: two documents edited:" << outcome.publisherRuns() << "publisher run(s) for" << outcome.publishedTargets().join(QLatin1Char(','));
    if (outcome.publisherRuns() != 1) problems.append(QStringLiteral("two documents: %1 publisher runs").arg(outcome.publisherRuns()));
    QCOMPARE(changedPdfs(before, library(), figures.pdfs), edited);
    VERIFY_SETTLED(QStringLiteral("figures: two documents edited"));

    // A figure nothing uses is deleted: the deletion does not stay pending.
    QVERIFY(QFile::remove(path(QStringLiteral("tex/assets/old-figure.pdf"))));
    outcome = sync(QStringLiteral("figures: unused figure deleted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    VERIFY_SETTLED(QStringLiteral("figures: unused figure deleted"));

    // A figure stops being used and is deleted in the same Sync.
    QVERIFY(writeFile(path(QStringLiteral("tex/assets/extra.pdf")), pdf(QStringLiteral("extra"), QStringLiteral("Extra figure"))));
    QVERIFY(writeFile(path(QStringLiteral("tex/source/one.tex")), figureDocument(QStringLiteral("one edited"), {QStringLiteral("tex/assets/shared.pdf"), QStringLiteral("tex/assets/extra.pdf")})));
    outcome = sync(QStringLiteral("figures: extra figure added"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    VERIFY_SETTLED(QStringLiteral("figures: extra figure added"));
    QVERIFY(writeFile(path(QStringLiteral("tex/source/one.tex")), figureDocument(QStringLiteral("one again"), {QStringLiteral("tex/assets/shared.pdf")})));
    QVERIFY(QFile::remove(path(QStringLiteral("tex/assets/extra.pdf"))));
    outcome = sync(QStringLiteral("figures: extra figure dropped and deleted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    VERIFY_SETTLED(QStringLiteral("figures: extra figure dropped and deleted"));

    // The state the user's Library was in: a figure's accepted baseline names
    // fewer PDFs than its pending deletion. Recreate the figure, publish it,
    // then narrow its baseline to one PDF before deleting it.
    QVERIFY(writeFile(path(QStringLiteral("tex/assets/budget.pdf")), pdf(QStringLiteral("budget"), QStringLiteral("Budget figure"))));
    outcome = sync(QStringLiteral("figures: budget figure added"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    VERIFY_SETTLED(QStringLiteral("figures: budget figure added"));
    {
        QString error;
        Thoughts::DesktopSyncStore store(QDir(m_repository->m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
        QVERIFY2(store.open(&error), qPrintable(error));
        QList<Thoughts::DesktopTrackedPath> baselines = store.baselines(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool narrowed = false;
        for (Thoughts::DesktopTrackedPath &baseline : baselines) {
            if (baseline.path != owner + QStringLiteral("/tex/assets/budget.pdf")) continue;
            QVERIFY2(baseline.affectedTargets.size() == figures.pdfs.size(), qPrintable(baseline.affectedTargets.join(QLatin1Char(','))));
            baseline.affectedTargets = {figures.pdfs.constFirst()};
            narrowed = true;
        }
        QVERIFY(narrowed);
        QVERIFY2(store.replaceBaselines(baselines, &error), qPrintable(error));
    }
    QVERIFY(QFile::remove(path(QStringLiteral("tex/assets/budget.pdf"))));
    outcome = sync(QStringLiteral("figures: budget figure deleted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    qInfo().noquote() << "E2E figures: budget figure deleted:" << outcome.publisherRuns() << "publisher run(s) for" << outcome.publishedTargets().join(QLatin1Char(','));
    VERIFY_SETTLED(QStringLiteral("figures: budget figure deleted"));

    // A proposal in one document: one publisher run, and only that PDF changes.
    before = library();
    const QString proposed = path(QStringLiteral("tex/source/three.tex"));
    const QJsonObject created = Thoughts::authorProposalAfterSync(m_docs, proposalRequest(proposed, 4, "\\section{three proposed}\n"), QStringLiteral("thoughtsctl"),
                                                                  syncCommandResult(sync(QStringLiteral("figures: before the proposal"))));
    QVERIFY2(created.value(QStringLiteral("state")).toString() == QLatin1String("proposal_created"), QJsonDocument(created).toJson().constData());
    outcome = sync(QStringLiteral("figures: proposal"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QStringList three;
    for (const QString &filename : std::as_const(figures.pdfs)) if (filename.contains(QLatin1String("three"))) three.append(filename);
    if (outcome.publisherRuns() != 1) problems.append(QStringLiteral("proposal: %1 publisher runs").arg(outcome.publisherRuns()));
    QCOMPARE(changedPdfs(before, library(), figures.pdfs), three);
    QVERIFY(serverSource(owner + QStringLiteral("/tex/source/three.tex")).contains("\\agentProposedEdit"));
    VERIFY_SETTLED(QStringLiteral("figures: proposal"));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
}

void DocumentPipelineE2ETest::testSyncSpeedRealisticLibrary()
{
    if (qEnvironmentVariableIsEmpty("THOUGHTS_E2E_PERF")) {
        QSKIP("Set THOUGHTS_E2E_PERF=1 to measure Sync on a realistic Library.");
    }
    // The shape of docs-concepts: 24 LaTeX documents, each with its own
    // fragments, 18 figures in tex/assets shared by all of them, a style and
    // a bibliography; about 400 managed paths in all.
    const QString owner = QStringLiteral("docs-perf");
    const int documentCount = qEnvironmentVariableIntValue("THOUGHTS_E2E_PERF_DOCUMENTS") > 0 ? qEnvironmentVariableIntValue("THOUGHTS_E2E_PERF_DOCUMENTS") : 24;
    QHash<QString, QByteArray> files;
    QStringList documents;
    for (int asset = 1; asset <= 18; ++asset) {
        files.insert(QStringLiteral("tex/assets/figure-%1.pdf").arg(asset), pdf(QStringLiteral("figure %1").arg(asset), QStringLiteral("Figure %1").arg(asset)));
    }
    files.insert(QStringLiteral("tex/support/perf_style.sty"), "\\ProvidesPackage{perf_style}\n\\RequirePackage{graphicx}\n");
    for (int document = 1; document <= documentCount; ++document) {
        QStringList fragments;
        for (int section = 1; section <= 15; ++section) {
            const QString fragment = QStringLiteral("tex/fragments/doc%1/section%2.tex").arg(document).arg(section);
            files.insert(fragment, QStringLiteral("\\subsection{Part %1}\nText of document %2, part %1.\n").arg(section).arg(document).toUtf8());
            fragments.append(fragment);
        }
        documents.append(QStringLiteral("tex/source/doc%1.tex").arg(document));
        files.insert(documents.constLast(), figureDocument(QStringLiteral("Document %1").arg(document), {QStringLiteral("tex/assets/figure-%1.pdf").arg(1 + document % 18)}, fragments));
    }
    QElapsedTimer setup;
    setup.start();
    const QString id = createLatexProject(QStringLiteral("Perf"), owner, files, documents);
    QVERIFY(!id.isEmpty());
    Outcome outcome = sync(QStringLiteral("perf: initial"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    Project perf {id, owner, QString(), {}};
    perf.pdfs = catalogueFiles(perf);
    QCOMPARE(perf.pdfs.size(), documentCount);
    qInfo().noquote() << "E2E perf: Library of" << files.size() << "files built in" << setup.elapsed() << "ms";
    VERIFY_SETTLED(QStringLiteral("perf: initial"));
    const auto path = [this, &owner](const QString &relative) { return QDir(m_docs).filePath(owner + QLatin1Char('/') + relative); };
    const auto report = [](const QString &label, const Outcome &outcome) {
        qInfo().noquote() << "E2E PERF" << label << ": total" << outcome.elapsedMs << "ms, publication" << outcome.publicationMs() << "ms," << outcome.publisherRuns() << "publisher run(s),"
                          << outcome.publishedTargets().size() << "document(s)";
        for (const auto &entry : outcome.messages) {
            if (entry.second.startsWith(QLatin1String("timing_desktop_publication|")) || entry.second.startsWith(QLatin1String("timing_desktop_source"))) {
                QString message = entry.second.section(QLatin1Char('|'), 1);
                message.replace(QRegularExpression(QStringLiteral("(perf_doc\\d+\\.pdf, )+")), QStringLiteral("... "));
                qInfo().noquote() << "E2E PERF   " << message;
            }
        }
    };

    for (int repeat = 1; repeat <= 3; ++repeat) {
        outcome = sync(QStringLiteral("perf: no change #%1").arg(repeat));
        QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
        report(QStringLiteral("no change #%1").arg(repeat), outcome);
    }

    QVERIFY(writeFile(path(QStringLiteral("tex/fragments/doc3/section2.tex")), "\\subsection{Part 2}\nEdited text of document 3.\n"));
    outcome = sync(QStringLiteral("perf: one fragment edited"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    report(QStringLiteral("one fragment edited"), outcome);

    QVERIFY(writeFile(path(QStringLiteral("tex/assets/figure-5.pdf")), pdf(QStringLiteral("figure 5"), QStringLiteral("Figure 5, second version"))));
    outcome = sync(QStringLiteral("perf: shared figure changed"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    report(QStringLiteral("shared figure changed"), outcome);
    outcome = sync(QStringLiteral("perf: after shared figure"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    report(QStringLiteral("after shared figure"), outcome);

    // The 2026-10-14 state: a deleted figure whose baseline names fewer PDFs.
    {
        QString error;
        Thoughts::DesktopSyncStore store(QDir(m_repository->m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
        QVERIFY2(store.open(&error), qPrintable(error));
        QList<Thoughts::DesktopTrackedPath> baselines = store.baselines(&error);
        for (Thoughts::DesktopTrackedPath &baseline : baselines) {
            if (baseline.path == owner + QStringLiteral("/tex/assets/figure-18.pdf")) baseline.affectedTargets = baseline.affectedTargets.mid(0, 6);
        }
        QVERIFY2(store.replaceBaselines(baselines, &error), qPrintable(error));
    }
    QVERIFY(QFile::remove(path(QStringLiteral("tex/assets/figure-18.pdf"))));
    QVERIFY(writeFile(path(QStringLiteral("tex/source/doc17.tex")), figureDocument(QStringLiteral("Document 17"), {QStringLiteral("tex/assets/figure-1.pdf")})));
    outcome = sync(QStringLiteral("perf: figure deleted"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    report(QStringLiteral("figure deleted"), outcome);
    for (int repeat = 1; repeat <= 2; ++repeat) {
        outcome = sync(QStringLiteral("perf: after figure deleted #%1").arg(repeat));
        QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
        report(QStringLiteral("after figure deleted #%1").arg(repeat), outcome);
    }
}

void DocumentPipelineE2ETest::testReviewCardKinds()
{
    // review-card-kinds.md: a Project's kind is a Library-project action (set_review_kind with
    // base_revision), a card's kind a review event; both reach the server through a real Sync,
    // and every answer carries the passage it was given on.
    const QString source = QStringLiteral("docs-kinds/tex/source/kinds.tex");
    const QByteArray text =
        "\\documentclass{article}\n"
        "\\providecommand{\\ReviewCardMaskStart}[2]{}\n\\providecommand{\\ReviewCardMaskEnd}[2]{}\n"
        "\\begin{document}\n"
        "A draft sentence about the method.\n\n"
        "Another sentence that stays.\n"
        "\\end{document}\n";
    const QHash<QString, QString> before = library();
    const QString created = createLatexProject(QStringLiteral("Kinds"), QStringLiteral("docs-kinds"), {{QStringLiteral("tex/source/kinds.tex"), text}},
                                               {QStringLiteral("tex/source/kinds.tex")});
    QVERIFY(!created.isEmpty());
    Outcome outcome = sync(QStringLiteral("kinds setup"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QString target;
    const QHash<QString, QString> built = library();
    for (auto it = built.cbegin(); it != built.cend(); ++it)
        if (!before.contains(it.key())) target = it.key();
    QVERIFY2(!target.isEmpty(), qPrintable(QStringList(library().keys()).join(QLatin1Char(','))));
    const auto project = [&] {
        for (const QJsonValue &value : Thoughts::LibraryProjectStore(m_docs).projects(false))
            if (value.toObject().value(QStringLiteral("name")).toString() == QLatin1String("Kinds")) return value.toObject();
        return QJsonObject();
    };
    // A server with review_card_kinds_v1 lists every Project's kind, knowledge until it is changed.
    QVERIFY2(!project().isEmpty(), "the Kinds Project is not in the catalogue");
    QCOMPARE(project().value(QStringLiteral("review_kind")).toString(), QStringLiteral("knowledge"));

    // A card from the agent tools, following its Project.
    const QString bundle = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"));
    Thoughts::ReviewStore store(m_docs, [this] { return m_repository->effectiveProjections(); }, m_repository->m_stateRoot);
    const QString path = QDir(m_docs).filePath(source);
    const QByteArray current = readFile(path);
    const int line = int(current.left(current.indexOf("A draft sentence")).count('\n')) + 1;
    const QString cardId = uuid();
    const QJsonObject request{{QStringLiteral("command"), QStringLiteral("create-review-card")}, {QStringLiteral("source"), path},
                              {QStringLiteral("expected_source_sha256"), sha256(current)}, {QStringLiteral("start_line"), line}, {QStringLiteral("end_line"), line + 1},
                              {QStringLiteral("question"), QStringLiteral("What does the draft say about the method?")},
                              {QStringLiteral("due_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {QStringLiteral("request_id"), cardId}};
    const QJsonObject card = store.sourceCardCommand(request, bundle);
    QVERIFY2(card.value(QStringLiteral("accepted")).toBool(), QJsonDocument(card).toJson().constData());
    QCOMPARE(card.value(QStringLiteral("review_kind")).toString(), QStringLiteral("knowledge"));
    QCOMPARE(card.value(QStringLiteral("review_kind_source")).toString(), QStringLiteral("project"));
    const auto storedCard = [&]() -> Thoughts::ReviewCard {
        for (const auto &candidate : store.cards())
            if (candidate.id() == cardId) return candidate;
        return {};
    };
    QVERIFY2(syncUntil(QStringLiteral("kinds card"), [&] { return !cardProjection(cardId).isEmpty() && nothingPending(); }), qPrintable(pendingSummary()));

    // Project cards: one set_review_kind action, accepted with the Project's revision.
    const QJsonObject knowledgeProject = project();
    QString error = Thoughts::LibraryProjectStore(m_docs).enqueue(QStringLiteral("set_review_kind"), knowledgeProject, {{QStringLiteral("review_kind"), QStringLiteral("project")}});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(storedCard().isProjectCard()); // at once, before Sync
    outcome = sync(QStringLiteral("project kind"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY2(Thoughts::LibraryProjectStore(m_docs).pending().isEmpty(), QJsonDocument(Thoughts::LibraryProjectStore(m_docs).pending()).toJson().constData());
    // The server's catalogue as this computer received it, as the phone reads it.
    QFile catalogueFile(QDir(m_docs).filePath(QStringLiteral(".thoughts/library-projects.json")));
    QVERIFY(catalogueFile.open(QIODevice::ReadOnly));
    bool serverKind = false;
    for (const QJsonValue &value : QJsonDocument::fromJson(catalogueFile.readAll()).object().value(QStringLiteral("projects")).toArray())
        if (value.toObject().value(QStringLiteral("name")).toString() == QLatin1String("Kinds"))
            serverKind = value.toObject().value(QStringLiteral("review_kind")).toString() == QLatin1String("project");
    QVERIFY2(serverKind, "the server's catalogue does not say Project cards");
    QVERIFY(project().value(QStringLiteral("revision")).toInt() > knowledgeProject.value(QStringLiteral("revision")).toInt());
    QVERIFY(storedCard().isProjectCard());
    QCOMPARE(storedCard().reviewKindSource, QStringLiteral("project"));
    // The manifest is not touched: nothing to publish.
    VERIFY_SETTLED(QStringLiteral("project kind"));

    // Make knowledge card: a kind event, stored by the server and read back from its projections.
    QString eventId;
    QVERIFY2(store.setCardKind(cardId, QStringLiteral("knowledge"), &error, QDateTime::currentDateTimeUtc(), &eventId), qPrintable(error));
    QVERIFY(!storedCard().isProjectCard());
    QVERIFY2(syncUntil(QStringLiteral("card kind"), [&] {
        return m_repository->effectiveProjections().value(QStringLiteral("review_event\nevents\n") + eventId).toObject().value(QStringLiteral("review_kind")).toString()
            == QLatin1String("knowledge") && nothingPending();
    }), qPrintable(pendingSummary()));
    const QJsonObject kindEvent = m_repository->effectiveProjections().value(QStringLiteral("review_event\nevents\n") + eventId).toObject();
    QCOMPARE(kindEvent.value(QStringLiteral("queue_action")).toString(), QStringLiteral("set_review_kind"));
    QCOMPARE(kindEvent.value(QStringLiteral("card_id")).toString(), cardId);
    QVERIFY(!kindEvent.contains(QStringLiteral("rating")));
    QCOMPARE(storedCard().reviewKind, QStringLiteral("knowledge"));
    QCOMPARE(storedCard().reviewKindSource, QStringLiteral("card"));
    // Follow the Project: a project card again, as its Project.
    QVERIFY2(store.setCardKind(cardId, QStringLiteral("follow_project"), &error, QDateTime::currentDateTimeUtc(), &eventId), qPrintable(error));
    QVERIFY2(syncUntil(QStringLiteral("follow project"), [&] {
        return m_repository->effectiveProjections().contains(QStringLiteral("review_event\nevents\n") + eventId) && nothingPending();
    }), qPrintable(pendingSummary()));
    QVERIFY(storedCard().isProjectCard());
    QCOMPARE(storedCard().reviewKindSource, QStringLiteral("project"));

    // An answer carries its passage; the server keeps the stamp unchanged.
    const Thoughts::ReviewCard answered = storedCard();
    const QJsonObject stamp = Thoughts::ReviewKinds::passageStamp(answered.record);
    QVERIFY2(!stamp.isEmpty(), QJsonDocument(answered.record).toJson().constData());
    const QDateTime at = QDateTime::currentDateTimeUtc();
    QString answerId;
    QVERIFY2(store.recordRating(cardId, QStringLiteral("custom_interval"), at, &error, at.addDays(60), &answerId, {}, {QStringLiteral("Correct")}, {}, {},
                                &answered.record), qPrintable(error));
    QVERIFY2(syncUntil(QStringLiteral("answer passage"), [&] {
        return m_repository->effectiveProjections().contains(QStringLiteral("review_event\nevents\n") + answerId) && nothingPending();
    }), qPrintable(pendingSummary()));
    QCOMPARE(m_repository->effectiveProjections().value(QStringLiteral("review_event\nevents\n") + answerId).toObject().value(QStringLiteral("passage")).toObject(), stamp);
    // A project card: due a week after the answer, whatever the interval.
    QCOMPARE(storedCard().schedule.dueAt, Thoughts::ReviewKinds::parseTime(at.toString(Qt::ISODateWithMs)).addDays(7));
    QVERIFY(!storedCard().passageChanged);
    VERIFY_SETTLED(QStringLiteral("card kinds"));
}

void DocumentPipelineE2ETest::testLinkedReviewCards()
{
    // 2026-10-13: "There should be no PDF-only cards if the source exists." A card on a
    // LaTeX document is always linked, one marked passage per answer area.
    const QString source = QStringLiteral("docs-cards/tex/source/cards.tex");
    const QByteArray text =
        "\\documentclass{article}\n"
        "\\providecommand{\\ReviewCardMaskStart}[2]{}\n\\providecommand{\\ReviewCardMaskEnd}[2]{}\n"
        // Room around each line: Fedora measures words by their font height, not their glyphs.
        "\\setlength{\\parskip}{14pt}\n"
        "\\begin{document}\n"
        "Alpha bravo charlie delta echo.\n\n"
        "Foxtrot golf hotel india juliet.\n\n"
        "Uniform victor whiskey xray yankee.\n\n"
        "\\begin{figure}[h]\n\\centering\n\\setlength{\\unitlength}{1mm}\n\\begin{picture}(40,20)\n\\put(0,0){\\line(1,1){20}}\n\\put(0,0){\\framebox(40,20){}}\n\\end{picture}\n\\end{figure}\n\n"
        "\\newpage\n"
        "Kilo lima mike november oscar.\n\n"
        "Papa quebec romeo sierra tango.\n\n"
        "Zulu amber bronze copper dusk.\n"
        "\\end{document}\n";
    const QHash<QString, QString> before = library();
    Project cards {createLatexProject(QStringLiteral("Cards"), QStringLiteral("docs-cards"), {{QStringLiteral("tex/source/cards.tex"), text}},
                                      {QStringLiteral("tex/source/cards.tex")}), QStringLiteral("docs-cards"), {}};
    QVERIFY(!cards.id.isEmpty());
    Outcome outcome = sync(QStringLiteral("cards setup"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QString target;
    const QHash<QString, QString> built = library();
    for (auto it = built.cbegin(); it != built.cend(); ++it)
        if (!before.contains(it.key())) target = it.key();
    QVERIFY2(!target.isEmpty(), qPrintable(QStringList(library().keys()).join(QLatin1Char(','))));
    const QString pdfPath = QDir(m_docs).filePath(QStringLiteral("current_pdfs/") + target);
    const QString bundle = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"));
    const auto document = [&] {
        const QString sha = library().value(target);
        const auto identity = Thoughts::DocumentIdentityStore(m_docs).forFile(pdfPath, sha);
        auto pdf = Poppler::Document::load(pdfPath);
        return QVariantMap{{QStringLiteral("pdf_path"), pdfPath}, {QStringLiteral("pdf_stem"), QFileInfo(target).completeBaseName()},
            {QStringLiteral("pdf_sha256"), sha}, {QStringLiteral("page_count"), pdf ? pdf->numPages() : 0},
            {QStringLiteral("document_id"), identity.value(QStringLiteral("document_id")).toString()}, {QStringLiteral("source_bundle_root"), bundle}};
    };
    QVERIFY2(!document().value(QStringLiteral("document_id")).toString().isEmpty(), "the built PDF has no document identity");
    QCOMPARE(document().value(QStringLiteral("page_count")).toInt(), 2);
    Thoughts::ReviewStore store(m_docs, [this] { return m_repository->effectiveProjections(); }, m_repository->m_stateRoot);
    const auto markerPairs = [&](const QString &id) { return int(readFile(QDir(m_docs).filePath(source)).count(QByteArray(QByteArray("\\ReviewCardMaskStart{") + id.toUtf8()))); };
    const auto segments = [](const QJsonObject &card) { return card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray(); };
    const auto placed = [&](const QString &id, int regions) {
        const QJsonObject card = cardProjection(id);
        const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
        return card.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("succeeded")
            && primary.value(QStringLiteral("pdf_sha256")).toString() == library().value(target)
            && primary.value(QStringLiteral("regions")).toArray().size() >= regions;
    };
    const auto describe = [&](const QString &id) { return QString::fromUtf8(QJsonDocument(cardProjection(id)).toJson(QJsonDocument::Compact)); };

    // a) Two areas on two pages, from the desktop: two marked passages, placed after the rebuild.
    const QString desktopCard = uuid();
    {
        const auto doc = document();
        const QList<Thoughts::ReviewArea> areas{{1, textLine(pdfPath, 1, QStringLiteral("Alpha"))}, {2, textLine(pdfPath, 2, QStringLiteral("Papa"))}};
        QVERIFY(!areas[0].rect.isEmpty() && !areas[1].rect.isEmpty());
        const auto plan = Thoughts::planReviewSourceAnchors(m_docs, bundle, pdfPath, doc.value(QStringLiteral("pdf_sha256")).toString(), areas, desktopCard);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.anchor.value(QStringLiteral("segments")).toArray().size(), 2);
        Thoughts::ReviewCard card;
        QString error;
        QVERIFY2(store.createManualCard(doc, 1, rectJson(areas[0].rect).toVariantList(), QStringLiteral("Which words open both pages?"), plan.pageVisualSha256,
                                        &card, &error, 0, {}, &plan, desktopCard), qPrintable(error));
        QCOMPARE(card.linkingState(), QStringLiteral("linked"));
        QCOMPARE(markerPairs(desktopCard), 2);
    }
    QVERIFY2(syncUntil(QStringLiteral("desktop card"), [&] { return placed(desktopCard, 2); }), qPrintable(describe(desktopCard)));
    QCOMPARE(segments(cardProjection(desktopCard)).size(), 2);
    QCOMPARE(cardProjection(desktopCard).value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
    QVERIFY(serverSource(source).contains(desktopCard.toUtf8()));

    // An edit moves the text; the rebuilt PDF places the card's two passages again.
    QVERIFY(writeFile(QDir(m_docs).filePath(source), readFile(QDir(m_docs).filePath(source)).replace("\\begin{document}\n", "\\begin{document}\nAn inserted opening sentence moves everything down.\n\n")));
    const QString editedSha = library().value(target);
    QVERIFY2(syncUntil(QStringLiteral("text moved"), [&] { return library().value(target) != editedSha && placed(desktopCard, 2); }), qPrintable(describe(desktopCard)));
    QCOMPARE(markerPairs(desktopCard), 2);

    // Its areas placed again: the markers move in one replacement, accepted by the server.
    {
        const auto doc = document();
        Thoughts::ReviewCard card;
        for (const auto &candidate : store.cards()) if (candidate.id() == desktopCard) card = candidate;
        QVERIFY(!card.id().isEmpty());
        const QList<Thoughts::ReviewArea> areas{{1, textLine(pdfPath, 1, QStringLiteral("Foxtrot"))}, {2, textLine(pdfPath, 2, QStringLiteral("Kilo"))}};
        const auto plan = Thoughts::planReviewSourceAnchors(m_docs, bundle, pdfPath, doc.value(QStringLiteral("pdf_sha256")).toString(), areas, desktopCard, true);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QString error;
        QVERIFY2(store.replaceCardAreas(card, doc.value(QStringLiteral("pdf_sha256")).toString(), QStringLiteral("Which words open both pages now?"), plan, &error), qPrintable(error));
        const QByteArray local = readFile(QDir(m_docs).filePath(source));
        QVERIFY(local.contains(QByteArray("}{0}%\nFoxtrot")) && local.contains(QByteArray("}{1}%\nKilo")) && !local.contains(QByteArray("}{0}%\nAlpha")));
    }
    const QString movedSha = library().value(target);
    QVERIFY2(syncUntil(QStringLiteral("areas placed again"), [&] { return library().value(target) != movedSha && placed(desktopCard, 2); }), qPrintable(describe(desktopCard)));
    QVERIFY(serverSource(source).contains(QByteArray("}{0}%\nFoxtrot")));
    QCOMPARE(cardProjection(desktopCard).value(QStringLiteral("primary")).toObject().value(QStringLiteral("question")).toString(), QStringLiteral("Which words open both pages now?"));

    // b) The phone creates a two-area card with its own source change.
    const QString phoneCard = uuid();
    {
        const auto doc = document();
        const QByteArray current = serverSource(source);
        QCOMPARE(current, readFile(QDir(m_docs).filePath(source)));
        const QList<QByteArray> lines = current.split('\n');
        int uniform = 0, zulu = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].startsWith("Uniform")) uniform = i + 1;
            if (lines[i].startsWith("Zulu")) zulu = i + 1;
        }
        QVERIFY(uniform && zulu);
        const auto marked = Thoughts::markReviewSourceRanges(source, current, {{uniform, uniform}, {zulu, zulu}}, phoneCard);
        QVERIFY2(marked.error.isEmpty(), qPrintable(marked.error));
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QRectF first = textLine(pdfPath, 1, QStringLiteral("Uniform")), second = textLine(pdfPath, 2, QStringLiteral("Zulu"));
        const QJsonObject payload{{QStringLiteral("schema_version"), 4}, {QStringLiteral("card_type"), QStringLiteral("visual_region")},
            {QStringLiteral("card_id"), phoneCard}, {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1},
            {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now}, {QStringLiteral("approved_at"), now},
            {QStringLiteral("origin_id"), QStringLiteral("thoughts-phone-e2e")}, {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
            {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")}}},
            {QStringLiteral("source_change"), QJsonObject{{QStringLiteral("transaction_id"), phoneCard}, {QStringLiteral("path"), source},
                {QStringLiteral("before_sha256"), sha256(current)}, {QStringLiteral("after_sha256"), sha256(marked.replacement)},
                {QStringLiteral("base_sha256"), sha256(current)}}},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("document_id"), doc.value(QStringLiteral("document_id")).toString()},
                {QStringLiteral("pdf_stem"), doc.value(QStringLiteral("pdf_stem")).toString()}, {QStringLiteral("pdf_sha256"), doc.value(QStringLiteral("pdf_sha256")).toString()},
                {QStringLiteral("page"), 1}, {QStringLiteral("rect"), rectJson(first)}, {QStringLiteral("question"), QStringLiteral("Which words did the phone mark?")},
                {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), rectJson(first)}},
                                                       QJsonObject{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), rectJson(second)}}}},
                {QStringLiteral("source_anchor"), marked.anchor}}}};
        const QString error = uploadAsPhone({payload}, {0}, {current, marked.replacement});
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    const QString phoneSha = library().value(target);
    QVERIFY2(syncUntil(QStringLiteral("phone card"), [&] { return library().value(target) != phoneSha && placed(phoneCard, 2); }), qPrintable(describe(phoneCard)));
    QCOMPARE(segments(cardProjection(phoneCard)).size(), 2);
    QCOMPARE(markerPairs(phoneCard), 2);

    // e) An area with no source text (a drawing) is refused before anything is saved.
    {
        const auto doc = document();
        const QByteArray sourceBefore = readFile(QDir(m_docs).filePath(source));
        const int cardsBefore = store.cards().size();
        // The figure is between the Uniform line and the end of page 1's text.
        const QRectF uniform = textLine(pdfPath, 1, QStringLiteral("Uniform"));
        const QRectF figure(QPointF(0.3, uniform.bottom() + 0.01), QPointF(0.7, uniform.bottom() + 0.12));
        const QString id = uuid();
        const auto plan = Thoughts::planReviewSourceAnchors(m_docs, bundle, pdfPath, doc.value(QStringLiteral("pdf_sha256")).toString(),
            {{1, uniform}, {1, figure}}, id);
        QVERIFY2(!plan.error.isEmpty(), "a drawing without text was linked");
        QCOMPARE(plan.failedArea, 1);
        qInfo().noquote() << "E2E refused figure area:" << plan.error;
        QCOMPARE(readFile(QDir(m_docs).filePath(source)), sourceBefore);
        QCOMPARE(store.cards().size(), cardsBefore);
    }
    // c) and d) PDF-only cards on the source document: the server links them.
    const QString pdfOnlyCard = uuid(), seededCard = uuid();
    {
        const auto doc = document();
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const auto pdfCard = [&](const QString &id, int page, const QString &word, const QJsonObject &link) {
            // A rectangle drawn around the line, as on the phone: whole words with room to spare.
            const QRectF rect = textLine(pdfPath, page, word, 6);
            QJsonObject primary{{QStringLiteral("document_id"), doc.value(QStringLiteral("document_id")).toString()},
                {QStringLiteral("pdf_stem"), doc.value(QStringLiteral("pdf_stem")).toString()}, {QStringLiteral("pdf_sha256"), doc.value(QStringLiteral("pdf_sha256")).toString()},
                {QStringLiteral("page"), page}, {QStringLiteral("rect"), rectJson(rect)}, {QStringLiteral("question"), QStringLiteral("Which words start this line?")},
                {QStringLiteral("page_visual_sha256"), Thoughts::fingerprintReviewPage(pdfPath, page).value(QStringLiteral("page_visual_sha256")).toString()},
                {QStringLiteral("page_visual_fingerprint_method"), QStringLiteral("poppler-rgb-96-v1")}};
            QJsonObject card{{QStringLiteral("schema_version"), 3}, {QStringLiteral("card_type"), QStringLiteral("visual_region")},
                {QStringLiteral("card_id"), id}, {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1},
                {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now}, {QStringLiteral("approved_at"), now},
                {QStringLiteral("origin_id"), QStringLiteral("thoughts-phone-e2e")}, {QStringLiteral("primary"), primary}};
            if (!link.isEmpty()) {
                primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
                card.insert(QStringLiteral("primary"), primary);
                card.insert(QStringLiteral("source_link"), link);
            }
            return card;
        };
        // c) Created without any source information; d) an older card saved as PDF-only.
        const QString error = uploadAsPhone({pdfCard(pdfOnlyCard, 1, QStringLiteral("Alpha"), {}),
                                             pdfCard(seededCard, 2, QStringLiteral("Papa"), {{QStringLiteral("state"), QStringLiteral("pdf_without_source")}})}, {0, 0});
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    const auto linked = [&](const QString &id) -> bool {
        return !segments(cardProjection(id)).isEmpty() && markerPairs(id) >= 1
            && cardProjection(id).value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("linked");
    };
    const bool convertedC = syncUntil(QStringLiteral("PDF-only conversion"), [&] { return linked(pdfOnlyCard) && linked(seededCard); }, 10);
    if (!convertedC) qWarning().noquote() << "E2E unconverted cards:" << describe(pdfOnlyCard) << describe(seededCard);
    QVERIFY2(linked(pdfOnlyCard), qPrintable(QStringLiteral("c) ") + describe(pdfOnlyCard)));
    QVERIFY2(linked(seededCard), qPrintable(QStringLiteral("d) ") + describe(seededCard)));
    const bool placedC = syncUntil(QStringLiteral("converted cards placed"), [&] { return placed(pdfOnlyCard, 1) && placed(seededCard, 1); });
    QVERIFY2(placedC, qPrintable(QStringLiteral("c) %1\nd) %2\nLibrary PDF %3, status %4")
                                     .arg(describe(pdfOnlyCard), describe(seededCard), library().value(target),
                                          QString::fromUtf8(QJsonDocument(renderStatus(target)).toJson(QJsonDocument::Compact)).left(600))));

    VERIFY_SETTLED(QStringLiteral("linked review cards"));
}

void DocumentPipelineE2ETest::testDefaultFolderRename()
{
    if (qEnvironmentVariable("THOUGHTS_E2E_CONTROL").isEmpty()) {
        QSKIP("The harness offers no server control (THOUGHTS_E2E_CONTROL).");
    }
    using Thoughts::SourceOwner;
    const auto owner = [&](const Project &project) {
        return QDir(m_docs).relativeFilePath(QFileInfo(Thoughts::ProjectConfiguration::path(m_docs, project.id)).absolutePath());
    };
    const QString alphaLegacy = owner(m_alpha);
    const QString betaLegacy = owner(m_beta);
    QVERIFY2(SourceOwner::isLegacyDefaultFolder(alphaLegacy) && SourceOwner::isLegacyDefaultFolder(betaLegacy), qPrintable(alphaLegacy + QLatin1Char(' ') + betaLegacy));
    const QString alphaCanonical = SourceOwner::rewriteLegacyDefaultFolder(alphaLegacy);
    const QString betaCanonical = SourceOwner::rewriteLegacyDefaultFolder(betaLegacy);
    const QString legacyPaper = alphaLegacy + QStringLiteral("/chapter/paper.tex");
    const QString canonicalPaper = alphaCanonical + QStringLiteral("/chapter/paper.tex");
    const QByteArray synced = readFile(QDir(m_docs).filePath(legacyPaper));
    QVERIFY(!synced.isEmpty());
    QCOMPARE(serverSource(legacyPaper), synced);
    const QHash<QString, QString> before = library();
    QStringList pdfsBefore = catalogueFiles(m_alpha) + catalogueFiles(m_beta);
    pdfsBefore.sort();
    QVERIFY(!pdfsBefore.isEmpty());
    QVERIFY(!m_importedFile.isEmpty() && QFileInfo::exists(QDir(m_docs).filePath(betaLegacy + QLatin1Char('/') + m_importedFile)));
    const auto cardStems = [&] {
        QMap<QString, QString> stems;
        const QJsonObject projections = m_repository->effectiveProjections();
        for (auto it = projections.begin(); it != projections.end(); ++it)
            if (it.key().startsWith(QLatin1String("review_card\ncards\n")))
                stems.insert(it.key(), it.value().toObject().value(QStringLiteral("pdf_stem")).toString());
        return stems;
    };
    const QMap<QString, QString> cardsBefore = cardStems();

    // Not yet published when Fedora migrates: an edit in Alpha's default
    // folder and a PDF imported into Beta (staged in its default folder).
    QByteArray edited = synced;
    edited.replace("A default-folder", "Edited before the rename: a default-folder");
    QVERIFY(edited != synced);
    QVERIFY(writeFile(QDir(m_docs).filePath(legacyPaper), edited));
    QTemporaryDir downloads;
    const QByteArray pendingPdf = pdf(QStringLiteral("Renamed Import"), QStringLiteral("Imported while the folders were renamed"));
    const QString chosen = downloads.filePath(QStringLiteral("Renamed Import.pdf"));
    QVERIFY(writeFile(chosen, pendingPdf));
    QString error = Thoughts::LibraryProjectStore(m_docs).importPdf(catalogueProject(m_beta), chosen, QFileInfo(chosen).completeBaseName());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QString pendingImport;
    for (const QJsonValue &value : Thoughts::LibraryProjectStore(m_docs).pending()) {
        if (value.toObject().value(QStringLiteral("action")).toString() == QLatin1String("import")) {
            pendingImport = value.toObject().value(QStringLiteral("filename")).toString();
            QVERIFY(value.toObject().value(QStringLiteral("local_path")).toString().contains(betaLegacy));
        }
    }
    QVERIFY(!pendingImport.isEmpty());

    // Fedora migrates with its services stopped.
    const qint64 legacyRevision = serverSourceEntry(legacyPaper).value(QStringLiteral("revision")).toInteger();
    QVERIFY(legacyRevision > 0);
    const QJsonObject migrated = harnessRequest(QStringLiteral("migrate-default-folder"));
    if (migrated.value(QStringLiteral("missing")).toBool()) {
        QSKIP("These server sources have no thoughts_server.migrate_default_folder yet.");
    }
    QVERIFY2(migrated.value(QStringLiteral("exit")).toInt() == 0, qPrintable(migrated.value(QStringLiteral("output")).toString().right(4000)));
    // Revision continuity: the new path starts where the old one ended.
    QCOMPARE(serverSourceEntry(canonicalPaper).value(QStringLiteral("revision")).toInteger(), legacyRevision);
    QCOMPARE(serverSource(canonicalPaper), synced);
    QVERIFY(serverSourceEntry(legacyPaper).value(QStringLiteral("deleted")).toBool());
    const qint64 tombstone = serverSourceEntry(legacyPaper).value(QStringLiteral("revision")).toInteger();

    // This computer's Sync renames its folders first, then publishes.
    Outcome outcome = sync(QStringLiteral("after the default-folder rename"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(alphaLegacy)) && !QFileInfo::exists(QDir(m_docs).filePath(betaLegacy)));
    QCOMPARE(owner(m_alpha), alphaCanonical);
    QCOMPARE(owner(m_beta), betaCanonical);
    QCOMPARE(readFile(QDir(m_docs).filePath(canonicalPaper)), edited);
    QCOMPARE(serverSource(canonicalPaper), edited);
    // The client never deletes an old path: it keeps the migration's tombstone.
    QVERIFY(serverSourceEntry(legacyPaper).value(QStringLiteral("deleted")).toBool());
    QCOMPARE(serverSourceEntry(legacyPaper).value(QStringLiteral("revision")).toInteger(), tombstone);
    QVERIFY(QFileInfo::exists(QDir(m_docs).filePath(betaCanonical + QLatin1Char('/') + m_importedFile)));
    // The queued import uploaded from its renamed staging copy.
    QCOMPARE(Thoughts::LibraryProjectStore(m_docs).pending().size(), 0);
    QCOMPARE(library().value(pendingImport), sha256(pendingPdf));
    m_beta.pdfs.append(pendingImport);
    // Every PDF and review card keeps its name (rule 4).
    QStringList pdfsAfter = catalogueFiles(m_alpha) + catalogueFiles(m_beta);
    pdfsAfter.removeAll(pendingImport);
    pdfsAfter.sort();
    QCOMPARE(pdfsAfter, pdfsBefore);
    const QHash<QString, QString> after = library();
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        QVERIFY2(after.contains(it.key()), qPrintable(it.key()));
    }
    QCOMPARE(cardStems(), cardsBefore);
    // Nothing here names the old folders any more.
    const QString stateRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server"));
    QVERIFY(!SourceOwner::containsLegacyDefaultFolder(readFile(QDir(stateRoot).filePath(QStringLiteral("source-publication.json")))));
    QVERIFY(!SourceOwner::containsLegacyDefaultFolder(readFile(QDir(m_docs).filePath(QStringLiteral(".thoughts/library-projects.json")))));
    QVERIFY(!SourceOwner::containsLegacyDefaultFolder(QJsonDocument(m_repository->effectiveProjections()).toJson(QJsonDocument::Compact)));
    QCOMPARE(readJson(QDir(stateRoot).filePath(QStringLiteral("default-folder-migration.json"))).value(QStringLiteral("phase")).toString(), QStringLiteral("done"));
    QCOMPARE(readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id)), serverSource(alphaCanonical + QStringLiteral("/thoughts-project.json")));
    VERIFY_SETTLED(QStringLiteral("after the default-folder rename"));

    // New default folders follow the Library.
    QCOMPARE(Thoughts::DefaultFolderLocation::librarySegment(m_docs), SourceOwner::DefaultSegment);
}

void DocumentPipelineE2ETest::testSourceFolderRename()
{
    if (qEnvironmentVariable("THOUGHTS_E2E_CONTROL").isEmpty()) {
        QSKIP("The harness offers no server control (THOUGHTS_E2E_CONTROL).");
    }
    using Thoughts::SourceFolderNames;
    const QString oldPaper = m_alpha.owner + QStringLiteral("/tex/source/paper.tex");
    const QByteArray synced = readFile(QDir(m_docs).filePath(oldPaper));
    QVERIFY2(!synced.isEmpty(), qPrintable(oldPaper));
    QCOMPARE(serverSource(oldPaper), synced);
    const QHash<QString, QString> before = library();
    QStringList pdfsBefore = catalogueFiles(m_alpha) + catalogueFiles(m_beta);
    pdfsBefore.sort();
    QVERIFY(!pdfsBefore.isEmpty());
    const auto cardStems = [&] {
        QMap<QString, QString> stems;
        const QJsonObject projections = m_repository->effectiveProjections();
        for (auto it = projections.begin(); it != projections.end(); ++it)
            if (it.key().startsWith(QLatin1String("review_card\ncards\n")))
                stems.insert(it.key(), it.value().toObject().value(QStringLiteral("pdf_stem")).toString());
        return stems;
    };
    const QMap<QString, QString> cardsBefore = cardStems();

    // A Project with a legacy id "source:docs-<folder>" (its default folder is
    // sha256 of the id): the id is an identifier, never renamed.
    const QString legacyId = QStringLiteral("source:docs-legacyid");
    const QString legacyDefault = QDir(m_docs).relativeFilePath(QFileInfo(Thoughts::ProjectConfiguration::path(m_docs, legacyId)).absolutePath());
    QCOMPARE(createProject(QStringLiteral("Legacy id"), QStringLiteral("docs-legacyid"), QStringLiteral("markdown/source/legacy.md"), "# Legacy\n\nAn old Project id.\n", legacyId),
             legacyId);
    Outcome legacyOutcome = sync(QStringLiteral("legacy-id Project"));
    QVERIFY2(legacyOutcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(legacyOutcome.message));
    QVERIFY(!serverSource(QStringLiteral("docs-legacyid/markdown/source/legacy.md")).isEmpty());

    // Not yet published when Fedora renames: an edit in Alpha's connected folder.
    QByteArray edited = synced;
    edited.append("\n% Edited before the folder rename.\n");
    QVERIFY(writeFile(QDir(m_docs).filePath(oldPaper), edited));

    // Fedora renames with its services stopped.
    const qint64 oldRevision = serverSourceEntry(oldPaper).value(QStringLiteral("revision")).toInteger();
    QVERIFY(oldRevision > 0);
    const QJsonObject migrated = harnessRequest(QStringLiteral("migrate-source-folder-names"));
    if (migrated.value(QStringLiteral("missing")).toBool()) {
        QSKIP("These server sources have no thoughts_server.migrate_source_folder_names yet.");
    }
    QVERIFY2(migrated.value(QStringLiteral("exit")).toInt() == 0, qPrintable(migrated.value(QStringLiteral("output")).toString().right(4000)));
    QVERIFY(serverSourceEntry(oldPaper).value(QStringLiteral("deleted")).toBool());
    const qint64 tombstone = serverSourceEntry(oldPaper).value(QStringLiteral("revision")).toInteger();

    // This computer's Sync renames its folders first, then publishes.
    Outcome outcome = sync(QStringLiteral("after the source folder rename"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const Thoughts::SourceFolderRenames renames = Thoughts::LibraryProjectStore::sourceFolderRenames(m_docs);
    const QString alphaNew = SourceFolderNames::newNameOf(m_alpha.owner, renames);
    const QString betaNew = SourceFolderNames::newNameOf(m_beta.owner, renames);
    QVERIFY2(alphaNew != m_alpha.owner && SourceFolderNames::isRuleNName(alphaNew), qPrintable(alphaNew));
    QVERIFY2(betaNew != m_beta.owner && SourceFolderNames::isRuleNName(betaNew), qPrintable(betaNew));
    const QString newPaper = alphaNew + QStringLiteral("/tex/source/paper.tex");
    QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(m_alpha.owner)));
    QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(m_beta.owner)));
    QCOMPARE(readFile(QDir(m_docs).filePath(newPaper)), edited);
    QCOMPARE(serverSource(newPaper), edited);
    QVERIFY(serverSourceEntry(newPaper).value(QStringLiteral("revision")).toInteger() > oldRevision);
    // The client never deletes or recreates an old path.
    QVERIFY(serverSourceEntry(oldPaper).value(QStringLiteral("deleted")).toBool());
    QCOMPARE(serverSourceEntry(oldPaper).value(QStringLiteral("revision")).toInteger(), tombstone);
    // Every PDF and review card keeps its name (rule F).
    m_alpha.owner = alphaNew;
    m_beta.owner = betaNew;
    QStringList pdfsAfter = catalogueFiles(m_alpha) + catalogueFiles(m_beta);
    pdfsAfter.sort();
    QCOMPARE(pdfsAfter, pdfsBefore);
    const QHash<QString, QString> after = library();
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        QVERIFY2(after.contains(it.key()), qPrintable(it.key()));
    }
    QCOMPARE(cardStems(), cardsBefore);
    // Nothing here names the old folders any more.
    const QString stateRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server"));
    for (const QByteArray &bytes : {readFile(QDir(stateRoot).filePath(QStringLiteral("source-publication.json"))),
                                    QJsonDocument(m_repository->effectiveProjections()).toJson(QJsonDocument::Compact)}) {
        // Rule P finds nothing left to change (free text keeps what it said).
        QCOMPARE(SourceFolderNames::rewriteJsonText(bytes, renames), bytes);
    }
    QCOMPARE(readJson(QDir(stateRoot).filePath(QString(Thoughts::SourceFolderMigration::JournalName))).value(QStringLiteral("phase")).toString(), QStringLiteral("done"));
    QCOMPARE(readFile(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id)),
             serverSource(QDir(m_docs).relativeFilePath(Thoughts::ProjectConfiguration::path(m_docs, m_alpha.id))));
    // The legacy-id Project keeps its id and default folder; its folder moved.
    const QString legacyNew = SourceFolderNames::newNameOf(QStringLiteral("docs-legacyid"), renames);
    QVERIFY2(legacyNew != QLatin1String("docs-legacyid"), qPrintable(legacyNew));
    QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(QStringLiteral("docs-legacyid"))));
    QVERIFY(QFileInfo::exists(QDir(m_docs).filePath(legacyNew + QStringLiteral("/markdown/source/legacy.md"))));
    QCOMPARE(QDir(m_docs).relativeFilePath(QFileInfo(Thoughts::ProjectConfiguration::path(m_docs, legacyId)).absolutePath()), legacyDefault);
    const QJsonObject legacyManifest = QJsonDocument::fromJson(readFile(Thoughts::ProjectConfiguration::path(m_docs, legacyId))).object();
    QCOMPARE(legacyManifest.value(QStringLiteral("id")).toString(), legacyId);
    QVERIFY(QJsonDocument(legacyManifest).toJson().contains(legacyNew.toUtf8()));
    verifySettled(QStringLiteral("after the source folder rename"));
}

void DocumentPipelineE2ETest::testMoveSourceDocument()
{
    // 2026-10-12: the user wanted concepts' sdpo.tex in the course-notes
    // Project. "Move to project…" moves one source-linked document with the
    // files it uses: an exclusive fragment and figure move, shared ones and a
    // generated input are copied, the origin's preamble pieces it needs are
    // carried, and links on both sides are rewritten. Its identity, PDF,
    // comment, review card and pending proposal stay attached.
    const QByteArray originStyle =
        "\\NeedsTeXFormat{LaTeX2e}\n\\ProvidesPackage{manual_style}\n\\RequirePackage{graphicx}\n\\RequirePackage{array}\n\\RequirePackage{hyperref}\n"
        "\\IfFileExists{manual_style_override.sty}{\\RequirePackage{manual_style_override}}{}\n"
        "\\newcommand{\\docTitle}[1]{\\section*{#1}}\n\\newcommand{\\originOnly}[1]{\\textbf{#1}}\n"
        "\\newcommand{\\SourceLabel}[1]{\\hypertarget{source-label:\\detokenize{#1}}{}}\n"
        "\\newcommand{\\SourceLink}[3]{\\href{manual-sourcelink:\\detokenize{#1}?label=\\detokenize{#2}}{#3}}\n"
        "\\newcommand{\\CrossSourceLink}[3]{\\href{manual-cross-sourcelink:\\detokenize{#1}?label=\\detokenize{#2}}{#3}}\n"
        "\\providecommand{\\CommentAnchor}[2]{}\n\\providecommand{\\ReviewCardMaskStart}[2]{}\n\\providecommand{\\ReviewCardMaskEnd}[1]{}\n"
        "\\newcolumntype{P}[1]{>{\\raggedright\\arraybackslash}p{#1}}\n";
    QByteArray destinationStyle = originStyle;
    destinationStyle.replace("\\newcommand{\\originOnly}[1]{\\textbf{#1}}\n", "");
    destinationStyle.replace("\\newcolumntype{P}[1]{>{\\raggedright\\arraybackslash}p{#1}}\n", "");
    destinationStyle.replace("\\IfFileExists{manual_style_override.sty}{\\RequirePackage{manual_style_override}}{}\n", "");
    const QString commentId = QStringLiteral("e2e-move-") + uuid().left(8);
    const QString proposalId = QStringLiteral("e2e-move-proposal-") + uuid().left(8);
    const QByteArray proposed = QByteArray("\\agentProposedEdit[created=2026-10-12T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{")
        + proposalId.toUtf8() + "}{Move proposal.\n}{A moved sentence.\n}{A proposed moved sentence.\n}";
    const QByteArray moved = "\\begin{document}\n\\docTitle{Moved}\\label{sec:moved}\\SourceLabel{moved}\n"
        "\\ReviewCardMaskStart{e2e-card}{0}%\nA reviewed passage with \\originOnly{an origin macro}; see \\Cref{sec:moved}.\n\\ReviewCardMaskEnd{e2e-card}%\n"
        "\\CommentAnchor{" + commentId.toUtf8() + "}{Moved comment}%\n"
        + proposed + "\n"
        "\\input{tex/fragments/only.tex}\n\\input{tex/fragments/shared.tex}\n\\input{tex/generated/table.tex}\n"
        "\\includegraphics[width=2cm]{tex/assets/only.pdf}\n\\includegraphics[width=2cm]{tex/assets/shared.pdf}\n"
        "See \\SourceLink{tex/source/other.tex}{other}{the other document}.\n"
        "\\begin{tabular}{P{2cm} l}a & b\\end{tabular}\n\\end{document}\n";
    const QByteArray other = "\\begin{document}\n\\docTitle{Other}\\SourceLabel{other}\n\\input{tex/fragments/shared.tex}\n"
        "\\includegraphics[width=2cm]{tex/assets/shared.pdf}\nSee \\SourceLink{tex/source/moved.tex}{moved}{the moved document}.\n\\end{document}\n";
    const QByteArray generated = "% Generated by scripts/table.py\nA generated table.\n";
    const QHash<QString, QByteArray> originFiles {
        {QStringLiteral("tex/support/manual_style.sty"), originStyle},
        {QStringLiteral("tex/support/manual_style_override.sty"), "\\ProvidesPackage{manual_style_override}\n\\RequirePackage{cleveref}\n"},
        {QStringLiteral("tex/source/moved.tex"), moved},
        {QStringLiteral("tex/source/other.tex"), other},
        {QStringLiteral("tex/fragments/only.tex"), "Only the moved document uses this fragment.\n"},
        {QStringLiteral("tex/fragments/shared.tex"), "Both documents use this fragment.\n"},
        {QStringLiteral("tex/generated/table.tex"), generated},
        {QStringLiteral("tex/assets/only.pdf"), pdf(QStringLiteral("Only"), QStringLiteral("Only figure"))},
        {QStringLiteral("tex/assets/shared.pdf"), pdf(QStringLiteral("Shared"), QStringLiteral("Shared figure"))},
        {QStringLiteral("scripts/table.py"), "print('A generated table.')\n"}};
    const QHash<QString, QByteArray> destinationFiles {
        {QStringLiteral("tex/support/manual_style.sty"), destinationStyle},
        {QStringLiteral("tex/source/existing.tex"), "\\begin{document}\n\\docTitle{Existing}\nAlready here.\n\\end{document}\n"}};
    Project origin {createLatexProject(QStringLiteral("Move origin"), QStringLiteral("docs-movefrom"), originFiles,
                                       {QStringLiteral("tex/source/moved.tex"), QStringLiteral("tex/source/other.tex")}),
                    QStringLiteral("docs-movefrom"), QStringLiteral("movefrom_moved.pdf")};
    Project destination {createLatexProject(QStringLiteral("Move destination"), QStringLiteral("docs-moveto"), destinationFiles, {QStringLiteral("tex/source/existing.tex")}),
                         QStringLiteral("docs-moveto"), QStringLiteral("moveto_existing.pdf")};
    QVERIFY(!origin.id.isEmpty() && !destination.id.isEmpty());
    const QString documentId = declaredDocument(origin, QStringLiteral("tex/source/moved.tex")).value(QStringLiteral("document_id")).toString();
    QVERIFY(!documentId.isEmpty());
    Outcome outcome = sync(QStringLiteral("move setup"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    for (const QString &pdfName : {QStringLiteral("movefrom_moved.pdf"), QStringLiteral("movefrom_other.pdf"), QStringLiteral("moveto_existing.pdf")})
        QVERIFY2(library().value(pdfName).size() == 64, qPrintable(pdfName + QStringLiteral(" was not built: ") + QStringList(library().keys()).join(QLatin1Char(','))));

    // A source-linked comment, a review card on the passage, and the pending proposal.
    const QString oldPath = QStringLiteral("docs-movefrom/tex/source/moved.tex");
    const QString newPath = QStringLiteral("docs-moveto/tex/source/moved.tex");
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const int markerLine = QString::fromUtf8(moved).split(QLatin1Char('\n')).indexOf(QStringLiteral("\\CommentAnchor{%1}{Moved comment}%").arg(commentId)) + 1;
    QVERIFY(markerLine > 0);
    QVERIFY(m_repository->enqueueLogicalComment(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), commentId}, {QStringLiteral("thread_root_id"), commentId},
        {QStringLiteral("parent_id"), QJsonValue::Null}, {QStringLiteral("revision"), 1}, {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now},
        {QStringLiteral("origin_id"), QStringLiteral("thoughts-mac")}, {QStringLiteral("deleted"), false}, {QStringLiteral("contents"), QStringLiteral("Moved comment")},
        {QStringLiteral("author"), QStringLiteral("E2E")}, {QStringLiteral("subtype"), QStringLiteral("Text")},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("movefrom_moved")}, {QStringLiteral("document_id"), documentId},
            {QStringLiteral("pdf_sha256"), library().value(QStringLiteral("movefrom_moved.pdf"))}, {QStringLiteral("page_index"), 0}, {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.2, 0.2}}}},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), commentId},
            {QStringLiteral("source_file"), oldPath}, {QStringLiteral("source_line"), markerLine}, {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}}}}));
    const QString cardId = uuid();
    const QByteArray phoneToken = qgetenv("THOUGHTS_E2E_PHONE_TOKEN");
    QVERIFY2(!phoneToken.isEmpty(), "run with tools/e2e/run-document-pipeline-e2e.py, which provides a second device");
    {
        // The phone creates the card, as review cards usually are.
        Thoughts::SyncClient client(m_server, phoneToken);
        QNetworkReply *sequence = client.deviceSequence();
        QEventLoop wait;
        QObject::connect(sequence, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();
        const qint64 next = QJsonDocument::fromJson(sequence->readAll()).object().value(QStringLiteral("next_device_sequence")).toInteger(1);
        sequence->deleteLater();
        const QJsonObject card {{QStringLiteral("operation_id"), uuid()}, {QStringLiteral("device_sequence"), next},
            {QStringLiteral("entity_type"), QStringLiteral("review_card")}, {QStringLiteral("collection"), QStringLiteral("cards")}, {QStringLiteral("entity_id"), cardId},
            {QStringLiteral("base_revision"), 0}, {QStringLiteral("client_timestamp"), now},
            {QStringLiteral("payload"), QJsonObject{{QStringLiteral("card_id"), cardId}, {QStringLiteral("revision"), 1}, {QStringLiteral("front"), QStringLiteral("What moved?")},
                {QStringLiteral("back"), QStringLiteral("The passage")}, {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now},
                {QStringLiteral("primary"), QJsonObject{{QStringLiteral("pdf_stem"), QStringLiteral("movefrom_moved")}, {QStringLiteral("document_id"), documentId},
                    {QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("segments"), QJsonArray{QJsonObject{
                        {QStringLiteral("segment_id"), QStringLiteral("e2e-card")}, {QStringLiteral("source_file"), oldPath},
                        {QStringLiteral("content_sha256"), sha256("A reviewed passage")}}}}}}}}}}};
        QNetworkReply *upload = client.uploadOperations({card});
        QObject::connect(upload, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();
        QVERIFY2(upload->error() == QNetworkReply::NoError, upload->readAll().constData());
        upload->deleteLater();
    }
    outcome = sync(QStringLiteral("move annotations"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    const QString commentKey = QStringLiteral("logical_comment\ncomments\n") + commentId;
    const auto scope = [&] { return m_repository->effectiveProjections(true, false).value(commentKey).toObject().value(QStringLiteral("scope")).toObject(); };
    QTRY_COMPARE_WITH_TIMEOUT(scope().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"), 15000);
    const auto proposal = [&] { return m_repository->effectiveProjections().value(QStringLiteral("proposal\nproposals\n") + proposalId).toObject(); };
    QTRY_COMPARE_WITH_TIMEOUT(proposal().value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    VERIFY_SETTLED(QStringLiteral("move annotations"));

    // Failure paths: a missing destination, and a name the destination already uses.
    QString error;
    libraryRequest(Thoughts::SourceDocumentMove::planRequest(catalogueProject(origin), documentId, QStringLiteral("new:missing"), {}, true), &error);
    QVERIFY2(error.contains(QStringLiteral("no longer available")), qPrintable(error));
    error.clear();
    QJsonObject clash = libraryRequest(Thoughts::SourceDocumentMove::planRequest(catalogueProject(origin), documentId, destination.id,
                                                                              QStringLiteral("tex/source/existing.tex"), true), &error).value(QStringLiteral("plan")).toObject();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(clash.value(QStringLiteral("destination")).toObject().value(QStringLiteral("collision")).toBool());
    QCOMPARE(clash.value(QStringLiteral("destination")).toObject().value(QStringLiteral("suggested_path")).toString(), QStringLiteral("tex/source/existing_2.tex"));
    {
        // Queued anyway (as an older dialog could): Fedora refuses, the Sync says so, and nothing changed.
        const QByteArray destinationBefore = readFile(Thoughts::ProjectConfiguration::path(m_docs, destination.id));
        error = Thoughts::LibraryProjectStore(m_docs).enqueue(QString(Thoughts::SourceDocumentMove::action), catalogueProject(origin),
                                                              Thoughts::SourceDocumentMove::operationValues(clash, true));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        outcome = sync(QStringLiteral("move refused"));
        QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Failed && outcome.message.contains(QStringLiteral("was cancelled")), qPrintable(outcome.message));
        QVERIFY(Thoughts::LibraryProjectStore(m_docs).pending().isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(m_docs).filePath(QStringLiteral("docs-moveto/tex/assets/only.pdf"))));
        QVERIFY(QFileInfo::exists(QDir(m_docs).filePath(oldPath)));
        QCOMPARE(readFile(Thoughts::ProjectConfiguration::path(m_docs, destination.id)), destinationBefore);
        VERIFY_SETTLED(QStringLiteral("move refused"));
    }

    // The reviewed plan.
    error.clear();
    const QJsonObject plan = libraryRequest(Thoughts::SourceDocumentMove::planRequest(catalogueProject(origin), documentId, destination.id, {}, true), &error)
                                 .value(QStringLiteral("plan")).toObject();
    QVERIFY2(error.isEmpty() && !plan.isEmpty(), qPrintable(error));
    QHash<QString, QJsonObject> files;
    for (const QJsonValue &value : plan.value(QStringLiteral("files")).toArray()) files.insert(value.toObject().value(QStringLiteral("source")).toString(), value.toObject());
    QCOMPARE(files.value(QStringLiteral("tex/fragments/only.tex")).value(QStringLiteral("action")).toString(), QStringLiteral("move"));
    QCOMPARE(files.value(QStringLiteral("tex/fragments/shared.tex")).value(QStringLiteral("action")).toString(), QStringLiteral("copy"));
    QCOMPARE(files.value(QStringLiteral("tex/assets/only.pdf")).value(QStringLiteral("transfer")).toString(), QStringLiteral("desktop"));
    QCOMPARE(files.value(QStringLiteral("tex/assets/only.pdf")).value(QStringLiteral("action")).toString(), QStringLiteral("move"));
    QCOMPARE(files.value(QStringLiteral("tex/assets/shared.pdf")).value(QStringLiteral("action")).toString(), QStringLiteral("copy"));
    QVERIFY(files.value(QStringLiteral("tex/generated/table.tex")).value(QStringLiteral("generated")).toBool());
    QCOMPARE(files.value(QStringLiteral("tex/generated/table.tex")).value(QStringLiteral("action")).toString(), QStringLiteral("copy"));
    const QString carried = plan.value(QStringLiteral("preamble")).toObject().value(QStringLiteral("carried")).toString();
    QVERIFY2(carried.contains(QStringLiteral("\\originOnly")) && carried.contains(QStringLiteral("{cleveref}")) && carried.contains(QStringLiteral("newcolumntype{P}")), qPrintable(carried));
    QVERIFY(Thoughts::SourceDocumentMove::describe(plan).contains(QStringLiteral("regenerating there no longer updates")));

    // An unpublished edit of the document holds the move back one Sync; a
    // move queued while a Sync runs waits for the next one.
    const QString oldFile = QDir(m_docs).filePath(oldPath);
    QVERIFY(writeFile(oldFile, QByteArray(readFile(oldFile)).replace("Only the moved", "Only the moved").replace("\\docTitle{Moved}", "\\docTitle{Moved and edited}")));
    {
        ProgressHook hook;
        bool queued = false;
        hook.onPhase = [&](const QString &phase) {
            if (queued || phase != QLatin1String("preparing")) return;
            queued = true;
            error = Thoughts::LibraryProjectStore(m_docs).enqueue(QString(Thoughts::SourceDocumentMove::action), catalogueProject(origin),
                                                                  Thoughts::SourceDocumentMove::operationValues(plan, true));
        };
        connect(m_repository, SIGNAL(syncProgressChanged(QString, QString, qint64, qint64, QString)), &hook, SLOT(progress(QString, QString, qint64, qint64, QString)));
        // This one Sync must leave the move queued; the Sync after its build would send it.
        outcome = syncOnce(QStringLiteral("move queued during Sync"));
        disconnect(m_repository, nullptr, &hook, nullptr);
        QVERIFY(queued);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    }
    QCOMPARE(Thoughts::LibraryProjectStore(m_docs).pending().size(), 1);
    QVERIFY(serverSource(oldPath).contains("Moved and edited"));
    const QHash<QString, QString> before = library();
    outcome = sync(QStringLiteral("move document"));
    QVERIFY2(outcome.state == FedoraRepository::ManualSyncState::Succeeded, qPrintable(outcome.message));
    QVERIFY(Thoughts::LibraryProjectStore(m_docs).pending().isEmpty());

    // Files on this computer and on Fedora.
    const auto local = [&](const QString &path) { return QFileInfo::exists(QDir(m_docs).filePath(path)); };
    QVERIFY(!local(oldPath));
    const QByteArray movedText = readFile(QDir(m_docs).filePath(newPath));
    QVERIFY2(movedText.contains("Moved and edited") && movedText.contains("thoughts:carried-preamble begin")
             && movedText.contains("\\CrossSourceLink{docs-movefrom/tex/source/other.tex}{other}") && movedText.contains(proposalId.toUtf8())
             && movedText.contains(commentId.toUtf8()), movedText.constData());
    QCOMPARE(serverSource(newPath), movedText);
    QVERIFY(serverSource(oldPath).isEmpty());
    QVERIFY(!local(QStringLiteral("docs-movefrom/tex/fragments/only.tex")) && local(QStringLiteral("docs-moveto/tex/fragments/only.tex")));
    QVERIFY(local(QStringLiteral("docs-movefrom/tex/fragments/shared.tex")) && local(QStringLiteral("docs-moveto/tex/fragments/shared.tex")));
    QVERIFY(!local(QStringLiteral("docs-movefrom/tex/assets/only.pdf")) && local(QStringLiteral("docs-moveto/tex/assets/only.pdf")));
    QVERIFY(local(QStringLiteral("docs-movefrom/tex/assets/shared.pdf")) && local(QStringLiteral("docs-moveto/tex/assets/shared.pdf")));
    QCOMPARE(readFile(QDir(m_docs).filePath(QStringLiteral("docs-moveto/tex/generated/table.tex"))), generated);
    QVERIFY(local(QStringLiteral("docs-movefrom/tex/generated/table.tex")) && local(QStringLiteral("docs-movefrom/scripts/table.py")));
    QVERIFY(!local(QStringLiteral("docs-moveto/scripts/table.py")));
    QVERIFY(readFile(QDir(m_docs).filePath(QStringLiteral("docs-movefrom/tex/source/other.tex"))).contains("\\CrossSourceLink{docs-moveto/tex/source/moved.tex}{moved}"));

    // Both Project manifests; the same identity and PDF.
    QVERIFY(declaredDocument(origin, QStringLiteral("tex/source/moved.tex")).isEmpty());
    const QJsonObject declared = declaredDocument(destination, QStringLiteral("tex/source/moved.tex"));
    QCOMPARE(declared.value(QStringLiteral("document_id")).toString(), documentId);
    QCOMPARE(declaredOutput(destination, QStringLiteral("tex/source/moved.tex")), QStringLiteral("movefrom_moved.pdf"));
    QVERIFY(manifestProblem(origin.id).isEmpty());
    QVERIFY(manifestProblem(destination.id).isEmpty());
    const QHash<QString, QString> after = library();
    QVERIFY2(after.value(QStringLiteral("movefrom_moved.pdf")).size() == 64 && after.value(QStringLiteral("movefrom_moved.pdf")) != before.value(QStringLiteral("movefrom_moved.pdf")),
             "the moved document was not rebuilt in its new Project");
    QVERIFY(after.value(QStringLiteral("movefrom_other.pdf")).size() == 64);
    // The destination folder gained inputs, so its documents may be rebuilt; they stay published.
    QVERIFY(after.value(QStringLiteral("moveto_existing.pdf")).size() == 64);
    QVERIFY(catalogueFiles(destination).contains(QStringLiteral("movefrom_moved.pdf")));
    QVERIFY(!catalogueFiles(origin).contains(QStringLiteral("movefrom_moved.pdf")));
    QVERIFY(catalogueFiles(origin).contains(QStringLiteral("movefrom_other.pdf")));
    QVERIFY2(readingTitles(destination).contains(QStringLiteral("moved")), qPrintable(readingTitles(destination).join(QLatin1Char(','))));
    QVERIFY(!readingTitles(origin).contains(QStringLiteral("moved")));

    // The comment, the card and the proposal follow the source.
    QTRY_COMPARE_WITH_TIMEOUT(scope().value(QStringLiteral("source_file")).toString(), newPath, 15000);
    QCOMPARE(scope().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"));
    QVERIFY(!m_repository->effectiveProjections(true, false).value(commentKey).toObject().value(QStringLiteral("deleted")).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(proposal().value(QStringLiteral("status")).toString(), QStringLiteral("pending"), 15000);
    const QJsonObject card = m_repository->effectiveProjections().value(QStringLiteral("review_card\ncards\n") + cardId).toObject();
    QVERIFY2(QJsonDocument(card).toJson().contains(newPath.toUtf8()) && !QJsonDocument(card).toJson().contains(oldPath.toUtf8()), QJsonDocument(card).toJson().constData());
    QCOMPARE(card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("pdf_stem")).toString(), QStringLiteral("movefrom_moved"));

    // What another device (the phone) reads: Fedora's catalogue lists the
    // document under its new Project with the same identity and PDF.
    error.clear();
    {
        Thoughts::SyncClient client(m_server, phoneToken);
        QNetworkReply *reply = client.libraryProjects();
        QEventLoop wait;
        QObject::connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();
        const QJsonArray projects = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("projects")).toArray();
        reply->deleteLater();
        QStringList where;
        for (const QJsonValue &value : projects)
            for (const QJsonValue &document : value.toObject().value(QStringLiteral("documents")).toArray())
                if (document.toObject().value(QStringLiteral("document_id")).toString() == documentId)
                    where << value.toObject().value(QStringLiteral("id")).toString() + QLatin1Char('=') + document.toObject().value(QStringLiteral("filename")).toString();
        QCOMPARE(where, QStringList {destination.id + QStringLiteral("=movefrom_moved.pdf")});
    }
    VERIFY_SETTLED(QStringLiteral("move document"));
}
}

QTEST_MAIN(Okular::DocumentPipelineE2ETest)
#include "documentpipelinee2etest.moc"
