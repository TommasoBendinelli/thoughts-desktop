// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the desktop's own Sync engine once against a server named by
// THOUGHTS_SERVER_URL, for the end-to-end harness in tools/sync-e2e.
// It refuses to start unless every Thoughts state location resolves inside
// the private home directory the harness created, so it can never read or
// write the real Library or its synchronization state.
#include "../part/fedorarepository.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDeadlineTimer>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

#include <cstdio>

using Okular::FedoraRepository;

namespace
{
QJsonObject readJson(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

QString stateRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("thoughts-server"));
}

// The observable result of a Sync: its outcome, the active Library
// generation, and the source conflicts this device still holds.
QJsonObject report(FedoraRepository *repository, const QString &docsRoot)
{
    QJsonObject result;
    const auto state = repository->manualSyncState();
    result.insert(QStringLiteral("state"), state == FedoraRepository::ManualSyncState::Succeeded ? QStringLiteral("succeeded")
                                            : state == FedoraRepository::ManualSyncState::Failed  ? QStringLiteral("failed")
                                                                                                  : QStringLiteral("unfinished"));
    result.insert(QStringLiteral("message"), repository->manualSyncMessage());
    result.insert(QStringLiteral("details"), repository->manualSyncDetails().right(6000));
    result.insert(QStringLiteral("error_code"), repository->manualSyncErrorCode());
    const QFileInfo current(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    result.insert(QStringLiteral("generation"), current.isSymLink() ? QFileInfo(current.symLinkTarget()).fileName() : QString());
    QJsonObject pdfs;
    const QJsonObject generation = readJson(QDir(current.absoluteFilePath()).filePath(QStringLiteral(".thoughts-generation.json")));
    for (const QJsonValue &value : generation.value(QStringLiteral("publications")).toArray()) {
        pdfs.insert(value.toObject().value(QStringLiteral("filename")).toString(), value.toObject().value(QStringLiteral("sha256")));
    }
    result.insert(QStringLiteral("pdfs"), pdfs);
    QJsonObject readings;
    for (const QFileInfo &file : QDir(current.absoluteFilePath()).entryInfoList({QStringLiteral("*.thoughts-project")}, QDir::Files)) {
        QJsonArray documents;
        for (const QJsonValue &value : readJson(file.absoluteFilePath()).value(QStringLiteral("documents")).toArray()) {
            documents.append(QJsonObject {{QStringLiteral("title"), value.toObject().value(QStringLiteral("title"))},
                                          {QStringLiteral("pdf_sha256"), value.toObject().value(QStringLiteral("pdf_sha256"))}});
        }
        readings.insert(file.fileName(), documents);
    }
    result.insert(QStringLiteral("readings"), readings);
    result.insert(QStringLiteral("conflicts"), readJson(QDir(stateRoot()).filePath(QStringLiteral("source-publication.json"))).value(QStringLiteral("conflicts")));
    return result;
}

// Streams the Sync's own progress and journal to stderr for the harness report.
class Tracer : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void progress(const QString &, const QString &phase, qint64, qint64, const QString &message)
    {
        std::fprintf(stderr, "progress %s: %s\n", qPrintable(phase), qPrintable(message));
    }
    void journal(const QString &, const QJsonObject &event)
    {
        std::fprintf(stderr, "journal %s\n", QJsonDocument(event).toJson(QJsonDocument::Compact).left(1500).constData());
    }
};

// The plugin exports only the methods the app's callers use, so signals are
// observed by signature, as parttest does.
bool runSync(FedoraRepository *repository, int timeoutSeconds)
{
    QSignalSpy states(repository, SIGNAL(manualSyncStateChanged(Okular::FedoraRepository::ManualSyncState,QString,QString)));
    repository->syncNow(QUuid::createUuid().toString(QUuid::WithoutBraces), true);
    const QDeadlineTimer deadline(qint64(timeoutSeconds) * 1000);
    while (!deadline.hasExpired()) {
        const auto state = repository->manualSyncState();
        if (!states.isEmpty() && (state == FedoraRepository::ManualSyncState::Succeeded || state == FedoraRepository::ManualSyncState::Failed)) {
            return true;
        }
        states.wait(1000);
    }
    return false;
}

QJsonObject conflictBundle(FedoraRepository *repository, QString *error)
{
    QSignalSpy ready(repository, SIGNAL(sourceConflictResolutionBundleReady(QJsonObject,QString)));
    repository->requestSourceConflictResolutionBundle();
    if (ready.isEmpty() && !ready.wait(120000)) {
        *error = QStringLiteral("Timed out preparing the source conflict.");
        return {};
    }
    *error = ready.first().at(1).toString();
    return ready.first().at(0).toJsonObject();
}

int print(const QJsonObject &value)
{
    std::fputs(QJsonDocument(value).toJson(QJsonDocument::Compact).constData(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("thoughts-e2e"));
    QApplication::setApplicationName(QStringLiteral("thoughts-e2e-mac"));
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("sync, conflict, or resolve"));
    parser.addOption({QStringLiteral("docs"), QStringLiteral("Docs root"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("contents"), QStringLiteral("Resolution contents file"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("timeout"), QStringLiteral("Seconds"), QStringLiteral("seconds"), QStringLiteral("1800")});
    parser.process(application);
    const QString command = parser.positionalArguments().value(0);
    const QString docsRoot = QDir(parser.value(QStringLiteral("docs"))).absolutePath();

    // Isolation guard: every location must be under the private home.
    const QString home = QDir(qEnvironmentVariable("HOME")).canonicalPath();
    const QString fixedHome = QDir(qEnvironmentVariable("CFFIXED_USER_HOME")).canonicalPath();
    const bool privateHome = !home.isEmpty() && home == fixedHome && home.contains(QStringLiteral("thoughts-e2e"));
    const QStringList locations {QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                                 QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                                 QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), docsRoot};
    // macOS reports the same directory as /tmp/... or /private/tmp/....
    const auto normalized = [](const QString &path) {
        const QString clean = QDir::cleanPath(path);
        return clean.startsWith(QLatin1String("/private/")) ? clean.mid(8) : clean;
    };
    for (const QString &location : locations) {
        if (!privateHome || !normalized(location).startsWith(normalized(home) + QLatin1Char('/'))) {
            std::fprintf(stderr, "Refusing to run: %s is outside the harness home\n", qPrintable(location));
            return 2;
        }
    }
    if (!qEnvironmentVariable("THOUGHTS_SERVER_URL").contains(QStringLiteral(":8443"))) {
        std::fprintf(stderr, "Refusing to run against a server other than the test instance\n");
        return 2;
    }

    FedoraRepository *repository = FedoraRepository::instance(docsRoot);
    Tracer tracer;
    QObject::connect(repository, SIGNAL(syncProgressChanged(QString,QString,qint64,qint64,QString)), &tracer, SLOT(progress(QString,QString,qint64,qint64,QString)));
    QObject::connect(repository, SIGNAL(syncJournalEvent(QString,QJsonObject)), &tracer, SLOT(journal(QString,QJsonObject)));
    const int timeout = parser.value(QStringLiteral("timeout")).toInt();
    if (command == QLatin1String("sync")) {
        const bool finished = runSync(repository, timeout);
        QJsonObject result = report(repository, docsRoot);
        result.insert(QStringLiteral("finished"), finished);
        return print(result);
    }
    if (command == QLatin1String("conflict") || command == QLatin1String("resolve")) {
        QString error;
        const QJsonObject bundle = conflictBundle(repository, &error);
        if (command == QLatin1String("conflict")) {
            QJsonObject result = bundle;
            result.insert(QStringLiteral("error"), error);
            return print(result);
        }
        QFile contents(parser.value(QStringLiteral("contents")));
        if (bundle.isEmpty() || !contents.open(QIODevice::ReadOnly)) {
            return print({{QStringLiteral("state"), QStringLiteral("failed")}, {QStringLiteral("message"), error.isEmpty() ? QStringLiteral("No resolution contents") : error}});
        }
        const QString operation = repository->queueSourceConflictResolution(bundle.value(QStringLiteral("id")).toString(),
                                                                            bundle.value(QStringLiteral("expected_revision")).toInteger(),
                                                                            bundle.value(QStringLiteral("current_sha256")).toString(), contents.readAll());
        if (operation.isEmpty()) {
            return print({{QStringLiteral("state"), QStringLiteral("failed")}, {QStringLiteral("message"), QStringLiteral("The resolution was not queued")}});
        }
        // As in the app, queueing a resolution is followed by a Sync.
        const bool finished = runSync(repository, timeout);
        QJsonObject result = report(repository, docsRoot);
        result.insert(QStringLiteral("finished"), finished);
        result.insert(QStringLiteral("resolved_path"), bundle.value(QStringLiteral("path")));
        return print(result);
    }
    std::fprintf(stderr, "Unknown command\n");
    return 2;
}

#include "syncdriver.moc"
