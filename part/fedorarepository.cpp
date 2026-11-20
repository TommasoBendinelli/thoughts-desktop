#include "projectreadingcontext.h"
#include <QSettings>
#include "projectreadinggeneration.h"
#include "documentidentity.h"
#include "sourceannotationindex.h"
#include "docscommentsynchronizer.h"
#include "commentanchorstate.h"
#include "commentmissingstate.h"
#include <QDesktopServices>
#include "proposalhistory.h"
#include "currentannotationindex.h"
#include "repositoryprojectionstore.h"
#include "verifiedartifactcache.h"
#include "libraryprojectstore.h"
#include "sourcedocumentmove.h"
#include "docschangemonitor.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fedorarepository.h"
#include "desktopsyncstore.h"
#include "frozenpublicationbundle.h"
#include "agentconversations.h"
#include "linkoverridestore.h"
#include "noteattachment.h"
#include "reviewcardfeedback.h"
#include "documentfeedback.h"
#include "feedbackversion.h"
#include "thoughtsplatform.h"
#include "thoughtsserveraddress.h"
#include "sourcechangetracker.h"
#include "thoughtignore.h"
#include "core/htmlattachment.h"
#include "synctransferledger.h"
#include "../shell/syncstatuslabels.h"
#include "../shell/proposaldecisions.h"
#include "../tools/publisher/managedsource.h"

#include <thoughts/commentservice.h>
#include <thoughts/durableoutbox.h>
#include <thoughts/syncclient.h>
#include <thoughts/synccore.h>

#include "defaultfoldermigration.h"
#include "sourcefoldermigration.h"
#include "sourcefoldernames.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#ifdef THOUGHTS_HAVE_POPPLER_QT6
#include <poppler-qt6.h>
#endif
#ifdef THOUGHTS_HAVE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <mutex>
#include <thread>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/clonefile.h>
#include <sys/stdio.h>
#include <sys/xattr.h>
#endif

QJsonObject Okular::Internal::logicalCommentForNewPlacement(const QJsonObject &placement, const QString &collection, const QJsonObject &parent)
{
    const QString id = placement.value(QStringLiteral("id")).toString();
    const QString parentId = placement.value(QStringLiteral("in_reply_to")).toString();
    if (id.isEmpty() || (!parentId.isEmpty() && parent.value(QStringLiteral("comment_id")).toString() != parentId)) return {};
    const QString timestamp = placement.value(QStringLiteral("updated_at")).toString(placement.value(QStringLiteral("modified")).toString());
    QJsonObject raw;
    for (const QString &key : {QStringLiteral("page"), QStringLiteral("rect"), QStringLiteral("quad_points"), QStringLiteral("fragments")}) {
        if (placement.contains(key)) raw.insert(key, placement.value(key));
    }
    QJsonObject result {
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id},
        {QStringLiteral("thread_root_id"), parentId.isEmpty() ? id : parent.value(QStringLiteral("thread_root_id")).toString(parentId)},
        {QStringLiteral("parent_id"), parentId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parentId)},
        {QStringLiteral("revision"), 1}, {QStringLiteral("created_at"), placement.value(QStringLiteral("created")).toString(timestamp)},
        {QStringLiteral("updated_at"), timestamp}, {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()},
        {QStringLiteral("deleted"), placement.value(QStringLiteral("deleted")).toBool()},
        {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("pdf_only")}, {QStringLiteral("reason"), QStringLiteral("awaiting_unique_source_anchor")}}},
        {QStringLiteral("origin"), QJsonObject {
            {QStringLiteral("collection"), collection}, {QStringLiteral("document_id"), placement.value(QStringLiteral("document_id")).toString()},
            {QStringLiteral("pdf_sha256"), placement.value(QStringLiteral("pdf_sha256")).toString(placement.value(QStringLiteral("annotation_pdf_sha256")).toString())},
            {QStringLiteral("page_index"), std::max(0, placement.value(QStringLiteral("page")).toInt(1) - 1)},
            {QStringLiteral("rect"), QJsonArray()}, {QStringLiteral("fragments"), QJsonArray()},
            {QStringLiteral("geometry_format"), QStringLiteral("pdf_points_bottom_left")}, {QStringLiteral("raw_pdf_geometry"), raw}}},
    };
    for (const QString &key : {QStringLiteral("contents"), QStringLiteral("author"), QStringLiteral("subtype"), QStringLiteral("selected_text"), QStringLiteral("color"), QStringLiteral("priority"), QStringLiteral("location"), QStringLiteral("chatgpt_url")}) {
        if (placement.contains(key)) result.insert(key, placement.value(key));
    }
    if (!parentId.isEmpty()) result.insert(QStringLiteral("origin"), parent.value(QStringLiteral("origin")));
    return result;
}

Okular::Internal::LogicalCommentMutationDecision Okular::Internal::logicalCommentMutationForPlacement(const QJsonObject &logicalPayload, const QJsonObject &placementPayload)
{
    LogicalCommentMutationDecision decision;
    if (logicalPayload.isEmpty()) {
        return decision;
    }
    decision.payload = logicalPayload;
    // Replaying a PDF placement cannot undo a canonical deletion.
    // Explicit Undo writes through enqueueLogicalComments instead.
    if (logicalPayload.value(QStringLiteral("deleted")).toBool()) {
        decision.suppressPlacement = true;
        return decision;
    }
    if (placementPayload.value(QStringLiteral("source_anchor_deletion")).toBool() && placementPayload.value(QStringLiteral("deleted")).toBool() && !logicalPayload.value(QStringLiteral("deleted")).toBool()) {
        decision.suppressPlacement = true;
        return decision;
    }
    const bool deletionStateChanged = logicalPayload.value(QStringLiteral("deleted")).toBool() != placementPayload.value(QStringLiteral("deleted")).toBool();
    bool logicalChanged = deletionStateChanged;
    static const QStringList logicalFields {
        QStringLiteral("contents"),
        QStringLiteral("author"),
        QStringLiteral("subtype"),
        QStringLiteral("selected_text"),
        QStringLiteral("color"),
        QStringLiteral("priority"),
        QStringLiteral("location"),
        QStringLiteral("chatgpt_url"),
    };
    if (!deletionStateChanged) {
        for (const QString &field : logicalFields) {
            if (!placementPayload.contains(field)) {
                continue;
            }
            QJsonValue value = placementPayload.value(field);
            if (field == QLatin1String("selected_text") && value.isNull()) value = QString();
            if (decision.payload.value(field) != value) {
                decision.payload.insert(field, value);
                logicalChanged = true;
            }
        }
    }
    if (!logicalChanged) {
        decision.suppressPlacement = true;
        return decision;
    }
    decision.baseRevision = logicalPayload.value(QStringLiteral("revision")).toInteger();
    if (decision.baseRevision < 1) {
        decision.error = QStringLiteral("The canonical logical comment has an invalid server revision.");
        return decision;
    }
    decision.payload.insert(QStringLiteral("revision"), decision.baseRevision + 1);
    decision.payload.insert(QStringLiteral("updated_at"), placementPayload.value(QStringLiteral("updated_at")));
    decision.payload.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
    decision.payload.insert(QStringLiteral("deleted"), placementPayload.value(QStringLiteral("deleted")).toBool());
    // Deleting the PDF placement is an explicit user deletion.
    if (decision.payload.value(QStringLiteral("deleted")).toBool()) decision.payload.insert(QStringLiteral("deletion_reason"), Thoughts::CommentService::userDeletionReason());
    else decision.payload.remove(QStringLiteral("deletion_reason"));
    decision.changed = true;
    decision.suppressPlacement = true;
    return decision;
}

Okular::Internal::OperationBatchDisposition Okular::Internal::classifyOperationResults(const QHash<QString, QString> &expectedEntityTypes, const QJsonArray &results, const QSet<QString> &replyDeletions)
{
    OperationBatchDisposition disposition;
    QSet<QString> observed;
    for (const QJsonValue &value : results) {
        const QJsonObject result = value.toObject();
        const QString operationId = result.value(QStringLiteral("operation_id")).toString();
        const QString outcome = result.value(QStringLiteral("outcome")).toString();
        if (operationId.isEmpty() || !expectedEntityTypes.contains(operationId) || observed.contains(operationId)) {
            disposition.error = QStringLiteral("The server returned an invalid local change acknowledgement.");
            return disposition;
        }
        observed.insert(operationId);
        if (outcome == QLatin1String("applied") || outcome == QLatin1String("duplicate")) {
            disposition.acknowledged.insert(operationId);
        } else if (outcome == QLatin1String("conflict") && expectedEntityTypes.value(operationId) == QLatin1String("proposal")) {
            // §10.6: the server accepts a proposal operation only as the Dismiss of a
            // removed_outside row; after a Restore elsewhere it is dropped.
            disposition.refused.insert(operationId);
            disposition.acknowledged.insert(operationId);
        } else if (outcome == QLatin1String("superseded")) {
            disposition.superseded.insert(operationId);
            if (expectedEntityTypes.value(operationId) == QLatin1String("logical_comment") && !replyDeletions.contains(operationId)) {
                disposition.logicalConflicts.insert(operationId);
            } else {
                disposition.acknowledged.insert(operationId);
            }
        } else {
            disposition.error = QStringLiteral("The server returned an unknown local change outcome.");
            return disposition;
        }
    }
    if (observed != QSet<QString>(expectedEntityTypes.keyBegin(), expectedEntityTypes.keyEnd())) {
        disposition.error = QStringLiteral("The server did not return an outcome for the complete local change batch.");
    }
    return disposition;
}

bool Okular::Internal::runLocalSourceTool(const QString &executable, const QStringList &arguments, QByteArray *standardOutput, QString *error, int timeoutMs, int inactivityTimeoutMs)
{
    if (standardOutput) {
        standardOutput->clear();
    }
    if (error) {
        error->clear();
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);

    QByteArray collectedOutput;
    QByteArray collectedDiagnostics;
    QByteArray pendingDiagnostics;
    QString lastProgressOperation;
    QString lastProgressPath;
    QElapsedTimer progressTimer;
    constexpr char progressPrefix[] = "THOUGHTS_LOCAL_SOURCE_PROGRESS ";
    const auto consumeDiagnosticLine = [&](QByteArray line) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.startsWith(progressPrefix)) {
            const QJsonDocument progress = QJsonDocument::fromJson(line.mid(sizeof(progressPrefix) - 1));
            const QJsonObject event = progress.object();
            const QString operation = event.value(QStringLiteral("operation")).toString();
            const QString path = event.value(QStringLiteral("path")).toString();
            const QStringList parts = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
            const bool safePath = !path.isEmpty() && path.size() <= 512 && !QDir::isAbsolutePath(path) && !path.contains(QLatin1Char('\\')) && !path.contains(QLatin1Char('\r')) && !path.contains(QLatin1Char('\n')) &&
                !parts.contains(QString()) && !parts.contains(QStringLiteral(".."));
            const bool safeOperation = operation == QLatin1String("scan_directory") || operation == QLatin1String("inspect_file") || operation == QLatin1String("read_file");
            if (progress.isObject() && event.value(QStringLiteral("schema_version")).toInt() == 1 && event.value(QStringLiteral("event")).toString() == QLatin1String("local_source_access") && safeOperation && safePath) {
                lastProgressOperation = operation;
                lastProgressPath = path;
                progressTimer.restart();
                return;
            }
        }
        collectedDiagnostics.append(line);
        collectedDiagnostics.append('\n');
    };
    const auto consumeDiagnostics = [&](const QByteArray &incoming, bool flush) {
        pendingDiagnostics.append(incoming);
        qsizetype newline = -1;
        while ((newline = pendingDiagnostics.indexOf('\n')) >= 0) {
            consumeDiagnosticLine(pendingDiagnostics.first(newline));
            pendingDiagnostics.remove(0, newline + 1);
        }
        if (flush && !pendingDiagnostics.isEmpty()) {
            consumeDiagnosticLine(std::exchange(pendingDiagnostics, QByteArray()));
        }
    };
    const auto drainChannels = [&] {
        collectedOutput.append(process.readAllStandardOutput());
        consumeDiagnostics(process.readAllStandardError(), false);
    };
    const auto diagnosticText = [&] { return QString::fromUtf8(collectedDiagnostics).trimmed(); };
    const auto drainUntilStopped = [&](int waitMs) {
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (process.state() != QProcess::NotRunning) {
            drainChannels();
            const qint64 remaining = qMax<qint64>(0, waitMs) - waitTimer.elapsed();
            if (remaining <= 0) {
                drainChannels();
                return false;
            }
            process.waitForReadyRead(static_cast<int>(qMin<qint64>(remaining, 100)));
        }
        drainChannels();
        return true;
    };

    process.start();
    if (!process.waitForStarted(5000)) {
        drainChannels();
        if (standardOutput) {
            *standardOutput = collectedOutput;
        }
        if (error) {
            *error = diagnosticText();
            if (error->isEmpty()) {
                *error = process.errorString().trimmed();
            }
            if (error->isEmpty()) {
                *error = QStringLiteral("The local source inventory command could not be started.");
            }
        }
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    progressTimer.start();
    bool timedOut = false;
    bool inactive = false;
    while (process.state() != QProcess::NotRunning) {
        drainChannels();
        const qint64 totalRemaining = qMax<qint64>(0, timeoutMs) - timer.elapsed();
        if (totalRemaining <= 0) {
            timedOut = true;
            break;
        }
        qint64 remaining = totalRemaining;
        if (inactivityTimeoutMs > 0) {
            const qint64 progressRemaining = qMax<qint64>(0, inactivityTimeoutMs) - progressTimer.elapsed();
            if (progressRemaining <= 0) {
                inactive = true;
                break;
            }
            remaining = qMin(remaining, progressRemaining);
        }
        process.waitForReadyRead(static_cast<int>(qMin<qint64>(remaining, 100)));
    }
    drainChannels();

    if (timedOut || inactive) {
        process.terminate();
        if (!drainUntilStopped(1000)) {
            process.kill();
            drainUntilStopped(1000);
        }
    }
    consumeDiagnostics(process.readAllStandardError(), true);

    if (standardOutput) {
        *standardOutput = collectedOutput;
    }
    if (timedOut || inactive || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = diagnosticText();
            if (inactive) {
                const QString activity =
                    lastProgressOperation == QLatin1String("scan_directory") ? QStringLiteral("scanning") : (lastProgressOperation == QLatin1String("inspect_file") ? QStringLiteral("checking") : QStringLiteral("reading"));
                const QString stalledMessage = lastProgressPath.isEmpty()
                    ? QStringLiteral("Local source discovery stopped responding for %1 seconds. Make sure the Library folder is available locally, then choose Retry.").arg(qMax(1, inactivityTimeoutMs / 1000))
                    : QStringLiteral("Local source data stopped responding while %1 %2. Make sure the Library folder is available locally, then choose Retry.").arg(activity, lastProgressPath);
                *error = error->isEmpty() ? stalledMessage : *error + QLatin1Char('\n') + stalledMessage;
            } else if (timedOut) {
                const QString timeoutMessage = QStringLiteral("The local source inventory command timed out after %1 ms.").arg(qMax(0, timeoutMs));
                *error = error->isEmpty() ? timeoutMessage : *error + QLatin1Char('\n') + timeoutMessage;
            } else if (error->isEmpty()) {
                *error = process.errorString().trimmed();
            }
            if (error->isEmpty()) {
                *error = QStringLiteral("The local source inventory command did not complete.");
            }
        }
        return false;
    }
    return true;
}

bool Okular::Internal::installAcceptedSourceManifest(const QString &acceptedPath, const QString &manifestPath)
{
    QFile accepted(acceptedPath);
    if (!accepted.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray contents = accepted.readAll();
    // Rewriting identical bytes changes the file's identity, which records
    // that its editable sources were checked, so the next Sync would check
    // them all again.
    QFile current(manifestPath);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == contents) {
        return true;
    }
    // Windows cannot replace a file that is still open.
    current.close();
    // A new installation has no Thoughts data folder until this first write.
    QDir().mkpath(QFileInfo(manifestPath).absolutePath());
    QSaveFile output(manifestPath);
    output.setDirectWriteFallback(false);
    return output.open(QIODevice::WriteOnly) && output.write(contents) == contents.size() && output.commit();
}

QJsonObject Okular::Internal::installedNewSourceMetadata(const QString &path, const QList<Thoughts::DesktopTrackedPath> &baselines)
{
    // A source another device created is published already, so it is
    // tracked from here on. Without kind and source_kind the next local edit
    // of it could never be frozen for publication. Project and role are
    // those its folder's other sources share; a successful publication
    // replaces them with the manifest's.
    QJsonObject metadata {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("source_kind"), QStringLiteral("source")}};
    const QString parent = QFileInfo(path).path();
    for (const QString &key : {QStringLiteral("project"), QStringLiteral("role")}) {
        QString shared;
        bool agreed = true;
        for (const Thoughts::DesktopTrackedPath &baseline : baselines) {
            if (QFileInfo(baseline.path).path() != parent || baseline.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file")
                || baseline.metadata.value(QStringLiteral("source_kind")).toString() != QLatin1String("source")) {
                continue;
            }
            const QString value = baseline.metadata.value(key).toString();
            if (value.isEmpty() || (!shared.isEmpty() && value != shared)) {
                agreed = false;
                break;
            }
            shared = value;
        }
        if (agreed && !shared.isEmpty()) {
            metadata.insert(key, shared);
        }
    }
    return metadata;
}

QString Okular::Internal::sourcePublicationTransferId(const QString &target, quint64 sourceGeneration)
{
    // Each subprocess starts new cumulative byte counters, even when it
    // publishes the same target and source generation again during this Sync.
    return QStringLiteral("source-publication:%1:%2:%3")
        .arg(target.isEmpty() ? QStringLiteral("all") : target,
             QString::number(sourceGeneration), QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QProcessEnvironment Okular::Internal::sourcePublisherProcessEnvironment(const QByteArray &token, const QString &publisherTraceId)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("THOUGHTS_TOKEN"), QString::fromUtf8(token));
    // The source-publisher credential belongs to a different Fedora device
    // from Thoughts Desktop. Reusing the Desktop operation trace would let the
    // publisher claim that trace and make the Desktop continuation fail with
    // "trace ID belongs to another device". Publisher progress is already
    // copied into the Desktop journal through its structured stderr stream.
    // Always provide an explicit, publisher-owned trace instead of merely
    // removing a possibly inherited Desktop trace.  This keeps the boundary
    // correct even when a launcher or wrapper adds THOUGHTS_TRACE_ID again.
    const QUuid requestedTrace(publisherTraceId);
    environment.insert(QStringLiteral("THOUGHTS_TRACE_ID"), requestedTrace.isNull() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : requestedTrace.toString(QUuid::WithoutBraces));
    return environment;
}

namespace
{
// The publisher installed with Thoughts. THOUGHTS_SOURCE_PUBLISHER lets tests
// choose a specific build, or none, so they never run an installed one.
QString sourcePublisherExecutable()
{
    const QString configured = qEnvironmentVariable("THOUGHTS_SOURCE_PUBLISHER");
    return configured.isEmpty() ? Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-publish-sources")) : configured;
}
constexpr char sourcePublisherEventPrefix[] = "THOUGHTS_SYNC_EVENT ";
constexpr int localSourceInactivityTimeoutMs = 15000;
constexpr qint64 manualLocalReconciliationTimeoutMs = 60000;

bool safeComponent(const QString &value)
{
    return !value.isEmpty() && value.size() <= 512 && !value.contains(QLatin1Char('/')) && !value.contains(QLatin1Char('\\'));
}

bool safePdfFilename(const QString &value)
{
    return safeComponent(value) && value.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
}

bool safeSourceProject(const QString &value)
{
    return safeComponent(value) && value.startsWith(QLatin1String("docs-")) && value.size() > 5;
}

// A folder that builds documents: a docs-* folder or a Project's default folder.
bool safeSourceOwner(const QString &value)
{
    return safeSourceProject(value) || Thoughts::SourceOwner::isDefaultFolder(value);
}

bool validSha256(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

struct PdfDeltaPlan {
    QString baseSha256;
    QString patchSha256;
    qint64 patchSize = -1;
    QString resultSha256;
    qint64 resultSize = -1;
};

QJsonObject publicationDelta(const QJsonObject &publication)
{
    const QJsonObject metadataDelta = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("delta")).toObject();
    return metadataDelta.isEmpty() ? publication.value(QStringLiteral("delta")).toObject() : metadataDelta;
}

QString readerPublicationIdentity(const QJsonObject &publication)
{
    return publication.value(QStringLiteral("sha256")).toString() + QLatin1Char('\n') + publication.value(QStringLiteral("source_artifact_sha256")).toString() + QLatin1Char('\n') +
        QString::number(publication.value(QStringLiteral("size_bytes")).toInteger(-1)) + QLatin1Char('\n') +
        QString::fromLatin1(QJsonDocument(publicationDelta(publication)).toJson(QJsonDocument::Compact).toBase64());
}

bool plannedObjectHasRole(const QJsonObject &object, const QString &role)
{
    const QJsonArray roles = object.value(QStringLiteral("roles")).toArray();
    return std::any_of(roles.cbegin(), roles.cend(), [&role](const QJsonValue &value) { return value.toString() == role; });
}

bool plannedObjectHasPublication(const QJsonObject &object, const QString &filename)
{
    const QJsonArray publications = object.value(QStringLiteral("publications")).toArray();
    return std::any_of(publications.cbegin(), publications.cend(), [&filename](const QJsonValue &value) { return value.toString() == filename; });
}

bool readerPlanHasPdfDelta(const QJsonObject &plan, const QString &filename)
{
    const QJsonArray objects = plan.value(QStringLiteral("objects")).toArray();
    return std::any_of(objects.cbegin(), objects.cend(), [&filename](const QJsonValue &value) {
        const QJsonObject object = value.toObject();
        return object.value(QStringLiteral("kind")).toString() == QLatin1String("pdf") && plannedObjectHasRole(object, QStringLiteral("pdf_delta")) && plannedObjectHasPublication(object, filename);
    });
}

std::optional<PdfDeltaPlan> pdfDeltaPlanForPublication(const QJsonObject &plan, const QJsonObject &publication)
{
    const QString filename = publication.value(QStringLiteral("filename")).toString();
    const QString resultSha256 = publication.value(QStringLiteral("sha256")).toString();
    const qint64 resultSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
    const QJsonObject delta = publicationDelta(publication);
    const QString baseSha256 = delta.value(QStringLiteral("base_sha256")).toString();
    const QString patchSha256 = delta.value(QStringLiteral("object_sha256")).toString();
    const qint64 patchSize = delta.value(QStringLiteral("size_bytes")).toInteger(-1);
    const qint64 declaredResultSize = delta.value(QStringLiteral("result_size_bytes")).toInteger(-1);
    if (!plan.value(QStringLiteral("_desktop_pdf_delta_requested")).toBool() || !plan.value(QStringLiteral("_desktop_pdf_delta_bases")).toArray().contains(QJsonValue(baseSha256)) || !safePdfFilename(filename) ||
        delta.value(QStringLiteral("format")).toString() != QLatin1String("zstd-patch-v1") || !validSha256(baseSha256) || !validSha256(patchSha256) || !validSha256(resultSha256) ||
        patchSize <= 0 || resultSize <= 0 || patchSize >= resultSize || patchSize > std::numeric_limits<int>::max() || resultSize > std::numeric_limits<int>::max() ||
        delta.value(QStringLiteral("result_sha256")).toString() != resultSha256 || declaredResultSize != resultSize) {
        return std::nullopt;
    }

    std::optional<QJsonObject> plannedDelta;
    bool plannedFullPdf = false;
    for (const QJsonValue &value : plan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("kind")).toString() != QLatin1String("pdf") || !plannedObjectHasPublication(object, filename)) {
            continue;
        }
        if (plannedObjectHasRole(object, QStringLiteral("pdf"))) {
            if (plannedFullPdf) {
                return std::nullopt;
            }
            plannedFullPdf = true;
        }
        if (plannedObjectHasRole(object, QStringLiteral("pdf_delta"))) {
            if (plannedDelta) {
                return std::nullopt;
            }
            plannedDelta = object;
        }
    }
    if (plannedFullPdf || !plannedDelta || plannedDelta->value(QStringLiteral("sha256")).toString() != patchSha256 || plannedDelta->value(QStringLiteral("size_bytes")).toInteger(-1) != patchSize) {
        return std::nullopt;
    }
    return PdfDeltaPlan {baseSha256, patchSha256, patchSize, resultSha256, resultSize};
}

QJsonObject publicationForFilename(const QJsonObject &library, const QString &filename)
{
    QJsonObject result;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        if (publication.value(QStringLiteral("filename")).toString() != filename) {
            continue;
        }
        if (!result.isEmpty()) {
            return {};
        }
        result = publication;
    }
    return result;
}

QByteArray verifiedActivePdfBase(const QString &docsRoot, const QString &filename, const QString &baseSha256)
{
    const QDir current(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    QFile manifestFile(current.filePath(QStringLiteral(".thoughts-generation.json")));
    const QJsonDocument manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()) : QJsonDocument();
    if (!manifest.isObject() || !validSha256(baseSha256)) {
        return {};
    }
    qint64 baseSize = -1;
    for (const QJsonValue &value : manifest.object().value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        if (publication.value(QStringLiteral("filename")).toString() != filename || publication.value(QStringLiteral("sha256")).toString() != baseSha256) {
            continue;
        }
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (!safePdfFilename(filename) || size < 0 || baseSize >= 0) {
            return {};
        }
        baseSize = size;
    }
    QFile base(current.filePath(filename));
    if (baseSize < 0 || !base.open(QIODevice::ReadOnly) || base.size() != baseSize) {
        return {};
    }
    const QByteArray contents = base.readAll();
    return contents.size() == baseSize && Thoughts::SyncClient::verifyObject(contents, baseSha256.toLatin1()) ? contents : QByteArray();
}

bool pathsReferToSameLocation(const QString &left, const QString &right)
{
    const QFileInfo leftInfo(left);
    const QFileInfo rightInfo(right);
    const QString leftCanonical = leftInfo.canonicalFilePath();
    const QString rightCanonical = rightInfo.canonicalFilePath();
    if (!leftCanonical.isEmpty() && !rightCanonical.isEmpty()) {
        return QDir::cleanPath(leftCanonical) == QDir::cleanPath(rightCanonical);
    }
    const QString leftLinked = Thoughts::Platform::directoryLinkTarget(leftInfo);
    const QString rightLinked = Thoughts::Platform::directoryLinkTarget(rightInfo);
    const QString leftTarget = leftLinked.isEmpty() ? leftInfo.absoluteFilePath() : leftLinked;
    const QString rightTarget = rightLinked.isEmpty() ? rightInfo.absoluteFilePath() : rightLinked;
    return QDir::cleanPath(leftTarget) == QDir::cleanPath(rightTarget);
}

QByteArray compactJson(const QJsonObject &value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

QString transferItemId(const QString &prefix, const QByteArray &body)
{
    return QStringLiteral("%1:%2").arg(prefix, QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex()));
}

QJsonObject discoverScopedBuildInputInventory(const QString &executable, const QString &docsRoot, const QString &manifestPath, const QString &target, QString *error)
{
    QFile manifestFile(manifestPath);
    if (!safePdfFilename(target) || !manifestFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("The scoped source manifest is unavailable.");
        }
        return {};
    }
    const QJsonDocument manifest = QJsonDocument::fromJson(manifestFile.readAll());
    const QJsonArray projects = manifest.object().value(QStringLiteral("projects")).toArray();
    if (!manifest.isObject() || !manifest.object().value(QStringLiteral("projects")).isArray()) {
        if (error) {
            *error = QStringLiteral("The scoped source manifest is malformed.");
        }
        return {};
    }

    QString projectName;
    for (const QJsonValue &value : projects) {
        const QJsonObject project = value.toObject();
        const QJsonArray targets = project.value(QStringLiteral("targets")).toArray();
        if (project.value(QStringLiteral("project")).toString().isEmpty() || !project.value(QStringLiteral("targets")).isArray()) {
            if (error) {
                *error = QStringLiteral("The scoped source project is malformed.");
            }
            return {};
        }
        if (!targets.contains(QJsonValue(target))) {
            continue;
        }
        if (!projectName.isEmpty()) {
            if (error) {
                *error = QStringLiteral("The PDF target belongs to more than one source project.");
            }
            return {};
        }
        projectName = project.value(QStringLiteral("project")).toString();
    }
    if (projectName.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The PDF target is not registered in the source manifest.");
        }
        return {};
    }

    QByteArray output;
    if (!Okular::Internal::runLocalSourceTool(executable,
                                              {QStringLiteral("inspect-build-inputs"),
                                               QStringLiteral("--root"),
                                               docsRoot,
                                               QStringLiteral("--manifest"),
                                               manifestPath,
                                               QStringLiteral("--target"),
                                               target,
                                               QStringLiteral("--project"),
                                               projectName,
                                               QStringLiteral("--desktop-progress-jsonl")},
                                              &output,
                                              error,
                                              120000,
                                              localSourceInactivityTimeoutMs)) {
        return {};
    }
    const QJsonDocument inventory = QJsonDocument::fromJson(output);
    if (!inventory.isObject() || inventory.object().value(QStringLiteral("target")).toString() != target || inventory.object().value(QStringLiteral("project")).toString() != projectName ||
        !inventory.object().value(QStringLiteral("entries")).isArray()) {
        if (error) {
            *error = QStringLiteral("The scoped build-input inventory is malformed.");
        }
        return {};
    }
    if (error) {
        error->clear();
    }
    return inventory.object();
}

QHash<QString, QString> sourceManifestProjectsByTarget(const QString &manifestPath);

// inspect-build-inputs for several PDFs of one Project: one publisher run
// scans the Project once (--also-target). One PDF uses the plain command.
QList<QPair<QString, QJsonObject>> discoverScopedBuildInputInventories(const QString &executable, const QString &docsRoot, const QString &manifestPath, const QStringList &targets, QString *error)
{
    QList<QPair<QString, QJsonObject>> result;
    if (targets.size() == 1) {
        const QJsonObject inventory = discoverScopedBuildInputInventory(executable, docsRoot, manifestPath, targets.constFirst(), error);
        if (!inventory.isEmpty()) {
            result.append({targets.constFirst(), inventory});
        }
        return result;
    }
    const QHash<QString, QString> projects = sourceManifestProjectsByTarget(manifestPath);
    const QString projectName = targets.isEmpty() ? QString() : projects.value(targets.constFirst());
    if (projectName.isEmpty() || std::any_of(targets.cbegin(), targets.cend(), [&](const QString &target) { return !safePdfFilename(target) || projects.value(target) != projectName; })) {
        if (error) {
            *error = QStringLiteral("The PDF targets are not registered in one source project of the source manifest.");
        }
        return {};
    }
    QStringList arguments {QStringLiteral("inspect-build-inputs"), QStringLiteral("--root"), docsRoot, QStringLiteral("--manifest"), manifestPath, QStringLiteral("--target"), targets.constFirst()};
    for (qsizetype index = 1; index < targets.size(); ++index) {
        arguments.append({QStringLiteral("--also-target"), targets.at(index)});
    }
    arguments.append({QStringLiteral("--project"), projectName, QStringLiteral("--desktop-progress-jsonl")});
    QByteArray output;
    if (!Okular::Internal::runLocalSourceTool(executable, arguments, &output, error, 120000, localSourceInactivityTimeoutMs)) {
        return {};
    }
    const QJsonArray inventories = QJsonDocument::fromJson(output).object().value(QStringLiteral("inventories")).toArray();
    if (inventories.size() != targets.size()) {
        if (error) {
            *error = QStringLiteral("The scoped build-input inventories are malformed.");
        }
        return {};
    }
    for (qsizetype index = 0; index < targets.size(); ++index) {
        const QJsonObject inventory = inventories.at(index).toObject();
        if (inventory.value(QStringLiteral("target")).toString() != targets.at(index) || inventory.value(QStringLiteral("project")).toString() != projectName ||
            !inventory.value(QStringLiteral("entries")).isArray()) {
            if (error) {
                *error = QStringLiteral("The scoped build-input inventories are malformed.");
            }
            return {};
        }
        result.append({targets.at(index), inventory});
    }
    if (error) {
        error->clear();
    }
    return result;
}

Thoughts::FrozenPublicationBundle discoverProjectTargetsFrozenPublicationImpl(const QString &executable,
                                                                              const QString &docsRoot,
                                                                              const QString &stateRoot,
                                                                              const QString &manifestPath,
                                                                              const QString &project,
                                                                              const QStringList &targets,
                                                                              const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                              const QList<Thoughts::DesktopTrackedPath> &baselines)
{
    Thoughts::FrozenPublicationBundle result;
    result.target = targets.isEmpty() ? QString() : targets.constFirst();
    if (!safeSourceProject(project) || targets.isEmpty() || std::any_of(targets.cbegin(), targets.cend(), [](const QString &target) { return !safePdfFilename(target); })) {
        result.disposition = Thoughts::FrozenPublicationDisposition::RequiresLiveScan;
        result.error = QStringLiteral("The affected source project or PDF target is ambiguous.");
        return result;
    }
    const QString discoveryRoot = QDir(stateRoot).filePath(QStringLiteral("frozen-publications"));
    if (!QDir().mkpath(discoveryRoot)) {
        result.error = QStringLiteral("The project source discovery directory could not be prepared.");
        return result;
    }
    const QString refreshedManifest = QDir(discoveryRoot).filePath(QStringLiteral("source-manifest-project-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const auto removeRefreshedManifest = qScopeGuard([refreshedManifest] { QFile::remove(refreshedManifest); });
    QString error;
    if (!Okular::Internal::runLocalSourceTool(executable,
                                              {QStringLiteral("refresh-project-manifest"),
                                               QStringLiteral("--docs-root"),
                                               docsRoot,
                                               QStringLiteral("--manifest"),
                                               manifestPath,
                                               QStringLiteral("--project"),
                                               project,
                                               QStringLiteral("--output"),
                                               refreshedManifest,
                                               QStringLiteral("--desktop-progress-jsonl")},
                                              nullptr,
                                              &error,
                                              120000,
                                              localSourceInactivityTimeoutMs)) {
        result.error = error;
        return result;
    }
    // A Project change that adds or removes a document changes which PDFs
    // need build inputs. Only a global publication registers all of them.
    const auto projectTargets = [&project](const QString &path) {
        QStringList targets;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return targets;
        for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("projects")).toArray()) {
            if (value.toObject().value(QStringLiteral("project")).toString() != project) continue;
            for (const QJsonValue &targetValue : value.toObject().value(QStringLiteral("targets")).toArray()) targets.append(targetValue.toString());
        }
        targets.sort();
        return targets;
    };
    if (projectTargets(refreshedManifest) != projectTargets(manifestPath)) {
        result.disposition = Thoughts::FrozenPublicationDisposition::RequiresLiveScan;
        result.error = QStringLiteral("The Project's documents changed.");
        return result;
    }
    const QList<QPair<QString, QJsonObject>> inventories = discoverScopedBuildInputInventories(executable, docsRoot, refreshedManifest, targets, &error);
    if (inventories.isEmpty()) {
        result.error = error;
        return result;
    }
    for (const auto &[target, inventory] : inventories) {
        if (inventory.value(QStringLiteral("project")).toString() != project) {
            result.disposition = Thoughts::FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("The refreshed target belongs to a different source project.");
            return result;
        }
    }
    return Thoughts::FrozenPublicationStager::stageProjectTargetsWithFreshInventories(docsRoot, stateRoot, project, refreshedManifest, inventories, snapshot, baselines);
}

Thoughts::FrozenPublicationBundle discoverProjectTargetFrozenPublicationImpl(const QString &executable,
                                                                             const QString &docsRoot,
                                                                             const QString &stateRoot,
                                                                             const QString &manifestPath,
                                                                             const QString &project,
                                                                             const QString &target,
                                                                             const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                             const QList<Thoughts::DesktopTrackedPath> &baselines)
{
    return discoverProjectTargetsFrozenPublicationImpl(executable, docsRoot, stateRoot, manifestPath, project, {target}, snapshot, baselines);
}

// The Project files of Projects whose creation is still queued for Fedora.
// Fedora creates them from the queued operation; publishing this computer's
// copy first would make that operation fail ("This project already exists").
QSet<QString> pendingProjectCreationManifests(const QString &docsRoot, const QJsonArray &pending)
{
    QSet<QString> result;
    for (const QJsonValue &value : pending) {
        const QJsonObject operation = value.toObject();
        if (operation.value(QStringLiteral("action")).toString() == QLatin1String("create"))
            result.insert(QDir(docsRoot).relativeFilePath(Thoughts::ProjectConfiguration::path(docsRoot, operation.value(QStringLiteral("project_id")).toString())));
    }
    return result;
}

Thoughts::FrozenPublicationBundle
discoverGlobalFrozenPublication(const QString &executable, const QString &docsRoot, const QString &stateRoot, const Thoughts::DesktopSyncSnapshot &snapshot, const QList<Thoughts::DesktopTrackedPath> &acceptedBaselines)
{
    Thoughts::FrozenPublicationBundle result;
    QString error;
    const QString discoveryRoot = QDir(stateRoot).filePath(QStringLiteral("frozen-publications"));
    if (!QDir().mkpath(discoveryRoot)) {
        result.error = QStringLiteral("The source discovery directory could not be prepared.");
        return result;
    }
    const QString discoveredManifest = QDir(discoveryRoot).filePath(QStringLiteral("source-manifest-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const auto removeDiscoveredManifest = qScopeGuard([discoveredManifest] { QFile::remove(discoveredManifest); });
    if (!Okular::Internal::runLocalSourceTool(executable,
                                              {QStringLiteral("generate-manifest"), QStringLiteral("--docs-root"), docsRoot, QStringLiteral("--output"), discoveredManifest, QStringLiteral("--desktop-progress-jsonl")},
                                              nullptr,
                                              &error,
                                              120000,
                                              localSourceInactivityTimeoutMs)) {
        result.error = error;
        return result;
    }
    QFile manifestFile(discoveredManifest);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("The discovered source manifest could not be opened.");
        return result;
    }
    QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll());
    manifestFile.close();
    if (const QSet<QString> queued = pendingProjectCreationManifests(docsRoot, Thoughts::LibraryProjectStore(docsRoot).pending()); !queued.isEmpty() && document.isObject()) {
        // A Project created while this Sync runs waits for its queued creation.
        QJsonObject value = document.object();
        QJsonArray shared;
        for (const QJsonValue &entry : value.value(QStringLiteral("shared_files")).toArray())
            if (!queued.contains(entry.toObject().value(QStringLiteral("path")).toString())) shared.append(entry);
        if (shared.size() != value.value(QStringLiteral("shared_files")).toArray().size()) {
            value.insert(QStringLiteral("shared_files"), shared);
            document = QJsonDocument(value);
            QSaveFile rewritten(discoveredManifest);
            const QByteArray bytes = document.toJson(QJsonDocument::Indented);
            if (!rewritten.open(QIODevice::WriteOnly) || rewritten.write(bytes) != bytes.size() || !rewritten.commit()) {
                result.error = QStringLiteral("The discovered source manifest could not be saved.");
                return result;
            }
        }
    }
    const QJsonArray projects = document.object().value(QStringLiteral("projects")).toArray();
    // No projects is valid: this device may not have installed any yet.
    if (!document.isObject() || !document.object().value(QStringLiteral("projects")).isArray()) {
        result.error = QStringLiteral("The discovered source manifest is malformed.");
        return result;
    }
    QHash<QString, QString> expectedProjects;
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject project = projectValue.toObject();
        const QString projectName = project.value(QStringLiteral("project")).toString();
        if (projectName.isEmpty() || !project.value(QStringLiteral("targets")).isArray()) {
            result.error = QStringLiteral("The discovered source project is malformed.");
            return result;
        }
        for (const QJsonValue &targetValue : project.value(QStringLiteral("targets")).toArray()) {
            const QString target = targetValue.toString();
            if (!safePdfFilename(target) || expectedProjects.contains(target)) {
                result.error = QStringLiteral("The discovered source target is malformed or duplicated.");
                return result;
            }
            expectedProjects.insert(target, projectName);
        }
    }
    // Run every inspection in one helper process. Each target still performs
    // fresh discovery and hashing before its immutable publication is staged.
    QByteArray output;
    if (!Okular::Internal::runLocalSourceTool(executable,
                                              {QStringLiteral("inspect-library-build-inputs"),
                                               QStringLiteral("--root"), docsRoot,
                                               QStringLiteral("--manifest"), discoveredManifest,
                                               QStringLiteral("--desktop-progress-jsonl")},
                                              &output, &error, 120000, localSourceInactivityTimeoutMs)) {
        result.error = error;
        return result;
    }
    const QJsonDocument batch = QJsonDocument::fromJson(output);
    const QJsonArray inventories = batch.object().value(QStringLiteral("inventories")).toArray();
    if (!batch.isObject() || batch.object().value(QStringLiteral("schema_version")).toInt() != 1 ||
        !batch.object().value(QStringLiteral("inventories")).isArray() || inventories.size() != expectedProjects.size()) {
        result.error = QStringLiteral("The discovered build-input inventories are incomplete.");
        return result;
    }
    for (const QJsonValue &value : inventories) {
        const QJsonObject inventory = value.toObject();
        const QString target = inventory.value(QStringLiteral("target")).toString();
        if (!expectedProjects.contains(target) || expectedProjects.value(target) != inventory.value(QStringLiteral("project")).toString() ||
            !inventory.value(QStringLiteral("entries")).isArray()) {
            result.error = QStringLiteral("The discovered build-input inventory is malformed or duplicated.");
            return result;
        }
        expectedProjects.remove(target);
    }
    return Thoughts::FrozenPublicationStager::stageGlobal(docsRoot, stateRoot, discoveredManifest, inventories, snapshot, acceptedBaselines);
}

QJsonObject sourceManifestFileIdentity(const QString &path, bool includeHash, QString *error)
{
    if (error) {
        error->clear();
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return QJsonObject {{QStringLiteral("exists"), false}};
    }
    QJsonObject identity {{QStringLiteral("exists"), true},
                          {QStringLiteral("size_bytes"), info.size()},
                          {QStringLiteral("modified_at_ms"), info.lastModified().toMSecsSinceEpoch()},
                          {QStringLiteral("metadata_changed_at_ms"), info.metadataChangeTime().toMSecsSinceEpoch()},
                          {QStringLiteral("created_at_ms"), info.birthTime().toMSecsSinceEpoch()}};
    QString fileIdentity = Thoughts::Platform::physicalFileIdentity(path);
    if (fileIdentity.isEmpty()) {
        fileIdentity = QStringLiteral("%1:%2").arg(info.canonicalFilePath()).arg(info.birthTime().toMSecsSinceEpoch());
    }
    identity.insert(QStringLiteral("file_identity"), fileIdentity);
    if (!includeHash) {
        return identity;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("The managed source manifest could not be read: %1").arg(file.errorString());
        }
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error) {
            *error = QStringLiteral("The managed source manifest could not be hashed.");
        }
        return {};
    }
    identity.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    return identity;
}

bool sourceManifestIsProjectScopedImpl(const QByteArray &contents)
{
    const QJsonDocument document = QJsonDocument::fromJson(contents);
    const QJsonObject manifest = document.object();
    if (!document.isObject() || manifest.value(QStringLiteral("schema_version")).toInt() != 3
        || !manifest.value(QStringLiteral("projects")).isArray()) return false;
    if (manifest.contains(QStringLiteral("shared_files")) && !manifest.value(QStringLiteral("shared_files")).isArray()) return false;
    for (const auto &raw : manifest.value(QStringLiteral("shared_files")).toArray()) {
        const auto entry = raw.toObject();
        if (!QRegularExpression(QStringLiteral("^\\.thoughts/projects/[a-f0-9]{64}/(default|documents)/thoughts-project\\.json$")).match(entry.value(QStringLiteral("path")).toString()).hasMatch()
            || entry.value(QStringLiteral("role")).toString() != QLatin1String("render_manifest")) return false;
    }
    return true;
}

bool sourceManifestPathIsProjectScoped(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("The managed source manifest could not be read: %1").arg(file.errorString());
        }
        return false;
    }
    return sourceManifestIsProjectScopedImpl(file.readAll());
}

bool sameSourceManifestFileMetadata(const QJsonObject &left, const QJsonObject &right)
{
    return left.value(QStringLiteral("exists")) == right.value(QStringLiteral("exists")) && left.value(QStringLiteral("size_bytes")) == right.value(QStringLiteral("size_bytes")) &&
        left.value(QStringLiteral("modified_at_ms")) == right.value(QStringLiteral("modified_at_ms")) && left.value(QStringLiteral("metadata_changed_at_ms")) == right.value(QStringLiteral("metadata_changed_at_ms")) &&
        left.value(QStringLiteral("created_at_ms")) == right.value(QStringLiteral("created_at_ms"));
}

QHash<QString, QString> sourceManifestProjectsByTarget(const QString &manifestPath)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonArray projects = document.object().value(QStringLiteral("projects")).toArray();
    if (!document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 3 || !document.object().value(QStringLiteral("projects")).isArray()) {
        return {};
    }
    QHash<QString, QString> result;
    for (const QJsonValue &value : projects) {
        const QJsonObject project = value.toObject();
        const QString name = project.value(QStringLiteral("project")).toString();
        const QJsonArray targets = project.value(QStringLiteral("targets")).toArray();
        if (!safeSourceOwner(name) || !project.value(QStringLiteral("targets")).isArray()) {
            return {};
        }
        for (const QJsonValue &targetValue : targets) {
            const QString target = targetValue.toString();
            if (!safePdfFilename(target) || result.contains(target)) {
                return {};
            }
            result.insert(target, name);
        }
    }
    return result;
}

// True only for a well-formed source manifest that manages nothing (no
// Project source, no Project file) and a publication that accepted no build
// inputs: the state of a Library before its first Project.
bool sourceManifestHasNoCoverage(const QString &manifestPath, const QJsonObject &publication)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject manifest = document.object();
    return document.isObject() && manifest.value(QStringLiteral("schema_version")).toInt() == 3
        && manifest.value(QStringLiteral("projects")).isArray() && manifest.value(QStringLiteral("projects")).toArray().isEmpty()
        && (!manifest.contains(QStringLiteral("shared_files"))
            || (manifest.value(QStringLiteral("shared_files")).isArray() && manifest.value(QStringLiteral("shared_files")).toArray().isEmpty()))
        && publication.value(QStringLiteral("build_input_entries")).isObject() && publication.value(QStringLiteral("build_input_entries")).toObject().isEmpty();
}

// The Project files (render manifests) the accepted source inventory manages.
QSet<QString> sourceManifestSharedPaths(const QString &manifestPath)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QSet<QString> result;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("shared_files")).toArray()) {
        result.insert(value.toObject().value(QStringLiteral("path")).toString());
    }
    return result;
}

// Whether discovery now lists a source of a Project's default folder (only
// `onlyOwner`'s, when given) that the accepted manifest lacks. A note's source
// can arrive from another device after this Mac made its manifest, e.g. one an
// agent wrote on Fedora; until the manifest lists it, the note's cards and
// comments have no source location. Discovery makes the manifest with the
// same rule, so after one more discovery this is false again.
// The stat data (path, size, modification time) of every file under the given
// folders, and of the manifest: while it is unchanged, discovery would list
// the same files, so a check's answer can be reused without reading any file.
QByteArray defaultFolderSourcesSignature(const QString &docsRoot, const QString &manifestPath, const QStringList &owners)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const QFileInfo &info) {
        hash.addData(info.absoluteFilePath().toUtf8());
        hash.addData(QByteArray(QByteArray::number(info.size()) + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + '\n'));
    };
    add(QFileInfo(manifestPath));
    for (const QString &owner : owners) {
        QDirIterator files(QDir(docsRoot).filePath(owner), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QList<QFileInfo> infos;
        while (files.hasNext()) {
            files.next();
            infos.append(files.fileInfo());
        }
        std::sort(infos.begin(), infos.end(), [](const QFileInfo &a, const QFileInfo &b) { return a.absoluteFilePath() < b.absoluteFilePath(); });
        for (const QFileInfo &info : std::as_const(infos)) add(info);
    }
    return hash.result();
}

bool manifestMissesDefaultFolderSourcesUncached(const QString &docsRoot, const QJsonArray &projects, const QStringList &owners)
{
    for (const QJsonValue &value : projects) {
        const QJsonObject project = value.toObject();
        const QString owner = project.value(QStringLiteral("project")).toString();
        if (!owners.contains(owner)) {
            continue;
        }
        QSet<QString> listed;
        for (const QJsonValue &entry : project.value(QStringLiteral("files")).toArray()) {
            listed.insert(entry.toObject().value(QStringLiteral("path")).toString());
        }
        const std::optional<QStringList> discovered = Thoughts::Publisher::discoveredFolderSources(docsRoot, owner);
        if (!discovered) {
            continue;
        }
        for (const QString &path : *discovered) {
            if (!listed.contains(path)) {
                return true;
            }
        }
    }
    return false;
}

bool manifestMissesDefaultFolderSources(const QString &docsRoot, const QString &manifestPath, const QString &onlyOwner = QString())
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonArray projects = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("projects")).toArray();
    QStringList owners;
    for (const QJsonValue &value : projects) {
        const QString owner = value.toObject().value(QStringLiteral("project")).toString();
        if (Thoughts::SourceOwner::isDefaultFolder(owner) && (onlyOwner.isEmpty() || owner == onlyOwner)) owners.append(owner);
    }
    if (owners.isEmpty()) {
        return false;
    }
    // Pending-change refreshes ask this often, on the event loop, and
    // discovery reads every source file of the folders: reuse the answer while
    // neither the manifest nor any file of these folders changed.
    static QMutex cacheMutex;
    static QHash<QString, QPair<QByteArray, bool>> cache;
    const QString key = docsRoot + QLatin1Char('\n') + manifestPath + QLatin1Char('\n') + onlyOwner;
    const QByteArray signature = defaultFolderSourcesSignature(docsRoot, manifestPath, owners);
    {
        QMutexLocker lock(&cacheMutex);
        if (const auto found = cache.constFind(key); found != cache.cend() && found->first == signature) {
            return found->second;
        }
    }
    const bool misses = manifestMissesDefaultFolderSourcesUncached(docsRoot, projects, owners);
    QMutexLocker lock(&cacheMutex);
    cache.insert(key, {signature, misses});
    return misses;
}

// One Sync may re-enter discovery a few times as other devices publish; more
// means it is not converging.
constexpr int maximumDiscoveryReentries = 5;

bool sourceInventoryCoversLibrary(const QString &docsRoot, const QString &manifestPath, const QString &requestedTarget = QString())
{
    QFile file(QDir(docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json")));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject catalogue = QJsonDocument::fromJson(file.readAll()).object();
    if (!catalogue.value(QStringLiteral("projects")).isArray()) return false;
    QHash<QString, QString> projects;
    for (const QJsonValue &value : catalogue.value(QStringLiteral("projects")).toArray()) {
        for (const QJsonValue &documentValue : value.toObject().value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = documentValue.toObject();
            const QString owner = document.value(QStringLiteral("source_project")).toString();
            if (owner.isEmpty() || !document.value(QStringLiteral("included")).toBool(true)) continue;
            const QString target = document.value(QStringLiteral("filename")).toString();
            if (!safeSourceOwner(owner) || !safePdfFilename(target) || projects.contains(target)) return false;
            // A Project created on another device has no source folder here
            // until one is added; this device publishes nothing for it, and
            // its PDFs still arrive through the Library.
            if (!QFileInfo(QDir(docsRoot).filePath(owner)).isDir()) continue;
            projects.insert(target, owner);
        }
    }
    // Build authority is the current document catalogue, never the installed
    // source index: that index can still describe a previous reading model.
    const auto inventory = sourceManifestProjectsByTarget(manifestPath);
    if (!requestedTarget.isEmpty()) {
        return projects.contains(requestedTarget) && inventory.value(requestedTarget) == projects.value(requestedTarget)
            && !manifestMissesDefaultFolderSources(docsRoot, manifestPath, projects.value(requestedTarget));
    }
    if (!sourceManifestPathIsProjectScoped(manifestPath, nullptr) || projects != inventory
        || manifestMissesDefaultFolderSources(docsRoot, manifestPath)) {
        return false;
    }
    // Every Project file must be managed, or Sync neither installs Fedora's
    // revisions of it nor records a base. A Project created or connected
    // here changes no PDF target (an empty Project, or a moved folder), so
    // the targets above alone cannot notice it. A Project whose creation is
    // still queued has no Fedora copy yet; the queued operation creates it.
    const QSet<QString> pendingCreation = pendingProjectCreationManifests(docsRoot, catalogue.value(QStringLiteral("pending")).toArray());
    const QSet<QString> managed = sourceManifestSharedPaths(manifestPath);
    const QDir directory(QDir(docsRoot).filePath(QStringLiteral(".thoughts/projects")));
    for (const QString &name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        for (const auto segment : {Thoughts::SourceOwner::DefaultSegment, Thoughts::SourceOwner::LegacyDefaultSegment}) {
            const QString relative = QStringLiteral(".thoughts/projects/") + name + QLatin1Char('/') + segment + QStringLiteral("/thoughts-project.json");
            const QFileInfo file(QDir(docsRoot).filePath(relative));
            if (file.isFile() && !file.isSymLink() && !managed.contains(relative) && !pendingCreation.contains(relative)) {
                return false;
            }
        }
    }
    return true;
}

QJsonObject parseSourcePublisherEvent(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (!trimmed.startsWith(sourcePublisherEventPrefix)) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.mid(sizeof(sourcePublisherEventPrefix) - 1), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const QJsonObject incoming = document.object();
    const QString phase = incoming.value(QStringLiteral("phase")).toString();
    const QString state = incoming.value(QStringLiteral("state")).toString();
    QString message = incoming.value(QStringLiteral("message")).toString().trimmed();
    static const QSet<QString> phases {QStringLiteral("queued"),
                                       QStringLiteral("checking_server"),
                                       QStringLiteral("preparing"),
                                       QStringLiteral("uploading"),
                                       QStringLiteral("waiting_network"),
                                       QStringLiteral("waiting_user"),
                                       QStringLiteral("building"),
                                       QStringLiteral("downloading"),
                                       QStringLiteral("verifying"),
                                       QStringLiteral("activating"),
                                       QStringLiteral("succeeded"),
                                       QStringLiteral("failed")};
    static const QSet<QString> states {QStringLiteral("running"), QStringLiteral("waiting"), QStringLiteral("succeeded"), QStringLiteral("failed")};
    if (incoming.value(QStringLiteral("schema_version")).toInt() != 1 || incoming.value(QStringLiteral("type")).toString() != QLatin1String("thoughts_sync_event") || !phases.contains(phase) || !states.contains(state) || message.isEmpty()) {
        return {};
    }
    const QString homePath = QDir::homePath();
    if (!homePath.isEmpty()) {
        message.replace(homePath, QStringLiteral("~"));
    }
    QDateTime at = QDateTime::fromString(incoming.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs);
    if (!at.isValid()) {
        at = QDateTime::currentDateTime();
    }
    QJsonObject event {{QStringLiteral("at_ms"), at.toMSecsSinceEpoch()}, {QStringLiteral("phase"), phase}, {QStringLiteral("state"), state}, {QStringLiteral("message"), message.left(600)}};
    const QString target = incoming.value(QStringLiteral("target")).toString();
    if (safePdfFilename(target)) {
        event.insert(QStringLiteral("target"), target);
    }
    const QString path = incoming.value(QStringLiteral("path")).toString();
    if (!path.isEmpty() && path.size() <= 512 && !QDir::isAbsolutePath(path) && !path.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
        event.insert(QStringLiteral("path"), path);
    }
    const qint64 transferredBytes = incoming.value(QStringLiteral("bytes_transferred")).toInteger(-1);
    const qint64 totalBytes = incoming.value(QStringLiteral("total_bytes")).toInteger(-1);
    if (transferredBytes >= 0) {
        event.insert(QStringLiteral("bytes_transferred"), transferredBytes);
    }
    if (totalBytes >= 0) {
        event.insert(QStringLiteral("total_bytes"), totalBytes);
    }
    for (const QString &field : {QStringLiteral("uploaded_bytes"), QStringLiteral("downloaded_bytes"), QStringLiteral("upload_total_bytes"), QStringLiteral("download_total_bytes")}) {
        const qint64 value = incoming.value(field).toInteger(-1);
        if (value >= 0) {
            event.insert(field, value);
        }
    }
    const QString errorCode = incoming.value(QStringLiteral("error_code")).toString();
    static const QRegularExpression errorCodePattern(QStringLiteral("^[a-z0-9_]{1,64}$"));
    if (errorCodePattern.match(errorCode).hasMatch()) {
        event.insert(QStringLiteral("error_code"), errorCode);
    }
    return event;
}

QList<QJsonObject> takeTraceEvents(Thoughts::TraceSseParser &parser, const QByteArray &incoming, bool flush)
{
    QList<QJsonObject> records;
    QList<Thoughts::TraceSseFrame> frames = parser.feed(incoming);
    if (flush) {
        frames.append(parser.finish());
    }
    for (const Thoughts::TraceSseFrame &frame : std::as_const(frames)) {
        if (frame.event != QByteArrayLiteral("trace") || frame.data.isEmpty()) {
            continue;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(frame.data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        QJsonObject record = document.object();
        if (record.value(QStringLiteral("schema_version")).toInt() != 1 || record.value(QStringLiteral("sequence")).toVariant().toLongLong() <= 0) {
            continue;
        }
        record.insert(QStringLiteral("origin"), QStringLiteral("server"));
        records.append(record);
    }
    return records;
}

QString effectiveArtifactRevision(const QJsonObject &library)
{
    const QString artifactRevision = library.value(QStringLiteral("artifact_revision")).toString();
    return validSha256(artifactRevision) ? artifactRevision : library.value(QStringLiteral("library_revision")).toString();
}

QByteArray fileSha256(const QString &path);

bool safeTrackedPath(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    return !clean.isEmpty() && clean != QLatin1String(".") && !QDir::isAbsolutePath(clean) && clean != QLatin1String("..") && !clean.startsWith(QLatin1String("../"));
}

QJsonObject desktopSyncSemanticMetadata(QJsonObject metadata)
{
    metadata.remove(QStringLiteral("_local_ctime_ms"));
    metadata.remove(QStringLiteral("_local_file_identity"));
    if (metadata.value(QStringLiteral("kind")).toString() == QLatin1String("file")) {
        // A file found by folder discovery inherits the folder's monitoring
        // bookkeeping. It records how the file was found, not what it is;
        // comparing it made a new file's accepted baseline "different", so a
        // document-scoped publication dropped it and the file stayed pending,
        // and deleting or renaming it later could not be published.
        metadata.remove(QStringLiteral("_coverage_root"));
        metadata.remove(QStringLiteral("_monitoring_mode"));
        metadata.remove(QStringLiteral("_tracking_scope"));
    }
    return metadata;
}

QStringList normalizedDesktopSyncTargets(QStringList targets)
{
    targets.removeAll(QString());
    targets.removeDuplicates();
    std::sort(targets.begin(), targets.end());
    return targets;
}

bool sameDesktopSyncDescriptor(const Thoughts::DesktopTrackedPath &left, const Thoughts::DesktopTrackedPath &right)
{
    return left.path == right.path && desktopSyncSemanticMetadata(left.metadata) == desktopSyncSemanticMetadata(right.metadata) && normalizedDesktopSyncTargets(left.affectedTargets) == normalizedDesktopSyncTargets(right.affectedTargets);
}

QList<Thoughts::DesktopTrackedPath> desktopMonitoringBaselines(
    const QString &docsRoot,
    const QList<Thoughts::DesktopTrackedPath> &acceptedPaths)
{
    QHash<QString, Thoughts::DesktopTrackedPath> result;
    QHash<QString, QStringList> targetsByProject;
    const auto mergeDirectory = [&result, &docsRoot](QString path,
                                                      QString mode,
                                                      const QString &project,
                                                      QStringList targets) {
        path = QDir::cleanPath(path);
        if (!safeTrackedPath(path)) {
            return;
        }
        Thoughts::DesktopTrackedPath directory = result.value(path);
        if (!directory.path.isEmpty()
            && directory.metadata.value(QStringLiteral("kind")).toString()
                != QLatin1String("directory")) {
            return;
        }
        directory.path = path;
        directory.exists = QFileInfo(QDir(docsRoot).filePath(path)).isDir();
        directory.sizeBytes = -1;
        directory.modifiedAtMs = directory.exists
            ? QFileInfo(QDir(docsRoot).filePath(path)).lastModified().toMSecsSinceEpoch()
            : -1;
        const QString previousMode = directory.metadata.value(QStringLiteral("_monitoring_mode")).toString();
        if (mode == QLatin1String("discovery") || previousMode.isEmpty()) {
            directory.metadata = QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("directory")},
                {QStringLiteral("_monitoring_mode"), mode},
            };
            if (!project.isEmpty()) {
                directory.metadata.insert(QStringLiteral("project"), project);
            }
            if (mode == QLatin1String("discovery")) {
                directory.metadata.insert(QStringLiteral("_tracking_scope"),
                                          project.isEmpty() ? QStringLiteral("global")
                                                            : QStringLiteral("project"));
                directory.metadata.insert(QStringLiteral("_coverage_root"), path);
            }
        }
        directory.affectedTargets.append(targets);
        directory.affectedTargets = normalizedDesktopSyncTargets(directory.affectedTargets);
        result.insert(path, directory);
    };

    for (Thoughts::DesktopTrackedPath path : acceptedPaths) {
        if (path.metadata.value(QStringLiteral("kind")).toString()
                != QLatin1String("file")) {
            continue;
        }
        const QString sourceKind = path.metadata.value(QStringLiteral("source_kind")).toString();
        if (sourceKind == QLatin1String("ignored")) {
            result.insert(path.path, path);
            continue;
        }
        if (sourceKind != QLatin1String("source")
            && sourceKind != QLatin1String("build_input")) {
            continue;
        }
        result.insert(path.path, path);
        const QString project = path.metadata.value(QStringLiteral("project")).toString();
        if (!project.isEmpty()) {
            QStringList targets = targetsByProject.value(project);
            targets.append(path.affectedTargets);
            targetsByProject.insert(project, normalizedDesktopSyncTargets(targets));

        } else if (Thoughts::SourceOwner::relative(path.path) == QLatin1String("thoughts-project.json")
                   && Thoughts::SourceOwner::isDefaultFolder(Thoughts::SourceOwner::of(path.path))) {
            // A Project's configuration keeps its default folder under
            // discovery, so the folder's first document is found too. The
            // configuration's targets belong to every folder, not this one.
            const QString folder = Thoughts::SourceOwner::of(path.path);
            targetsByProject.insert(folder, targetsByProject.value(folder));
        }
        mergeDirectory(QFileInfo(path.path).path(),
                       QStringLiteral("parent"),
                       project,
                       path.affectedTargets);
    }

    static const QStringList authoringRoots {
        QStringLiteral("source"),
        QStringLiteral("fragments"),
        QStringLiteral("assets"),
        QStringLiteral("generated"),
        QStringLiteral("support"),
    };
    for (auto iterator = targetsByProject.cbegin(); iterator != targetsByProject.cend(); ++iterator) {
        mergeDirectory(iterator.key(),
                       QStringLiteral("discovery"),
                       iterator.key(),
                       iterator.value());
        for (const QString &tree : {QStringLiteral("tex"), QStringLiteral("markdown")}) {
            const QString sourceRoot = iterator.key() + QLatin1Char('/') + tree;
            mergeDirectory(sourceRoot,
                           QStringLiteral("parent"),
                           iterator.key(),
                           iterator.value());
            for (const QString &authoringRoot : authoringRoots) {
                mergeDirectory(sourceRoot + QLatin1Char('/') + authoringRoot,
                               QStringLiteral("discovery"),
                               iterator.key(),
                               iterator.value());
            }
        }
    }

    QList<Thoughts::DesktopTrackedPath> baselines = result.values();
    std::sort(baselines.begin(), baselines.end(), [](const auto &left, const auto &right) {
        return left.path < right.path;
    });
    return baselines;
}

// scopeTargets: the PDFs a scoped publication covered; empty for a global one.
QList<Thoughts::DesktopTrackedPath> acceptedDesktopSyncBaselines(const QString &docsRoot,
                                                                 const QString &manifestPath,
                                                                 const QString &sourceStatePath,
                                                                 const QJsonObject &publication,
                                                                 const QStringList &scopeTargets,
                                                                 const QList<Thoughts::DesktopTrackedPath> &previousBaselines,
                                                                 QString *error)
{
    const bool scoped = !scopeTargets.isEmpty();
    const auto fail = [error](const QString &message) -> QList<Thoughts::DesktopTrackedPath> {
        if (error) {
            *error = message;
        }
        return {};
    };
    QFile manifestFile(manifestPath);
    QFile sourceStateFile(sourceStatePath);
    if (!manifestFile.open(QIODevice::ReadOnly) || !sourceStateFile.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("The accepted source coverage could not be opened."));
    }
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll());
    const QJsonDocument sourceStateDocument = QJsonDocument::fromJson(sourceStateFile.readAll());
    const QJsonObject manifestObject = manifestDocument.object();
    const int manifestSchema = manifestObject.value(QStringLiteral("schema_version")).toInt();
    if (!manifestDocument.isObject() || manifestSchema != 3 || !manifestObject.value(QStringLiteral("projects")).isArray() ||
        (manifestObject.contains(QStringLiteral("shared_files")) && !manifestObject.value(QStringLiteral("shared_files")).isArray()) || !sourceStateDocument.isObject() ||
        !sourceStateDocument.object().value(QStringLiteral("bases")).isObject() || !publication.value(QStringLiteral("build_input_entries")).isObject()) {
        return fail(QStringLiteral("The source publisher returned incomplete tracking coverage."));
    }

    struct ManifestPath {
        QJsonObject metadata;
        QStringList targets;
    };
    QHash<QString, ManifestPath> manifestPaths;
    QSet<QString> expectedTargets;
    QHash<QString, QStringList> targetsByProject;
    for (const QJsonValue &projectValue : manifestObject.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = projectValue.toObject();
        const QString projectName = project.value(QStringLiteral("project")).toString();
        const QJsonArray targetValues = project.value(QStringLiteral("targets")).toArray();
        const QJsonArray fileValues = project.value(QStringLiteral("files")).toArray();
        if (!safeTrackedPath(projectName) || !project.value(QStringLiteral("targets")).isArray() || !project.value(QStringLiteral("files")).isArray()) {
            return fail(QStringLiteral("The managed source manifest is malformed."));
        }
        QStringList targets;
        for (const QJsonValue &targetValue : targetValues) {
            const QString target = targetValue.toString();
            if (!safePdfFilename(target)) {
                return fail(QStringLiteral("The managed source manifest contains an invalid PDF target."));
            }
            targets.append(target);
            expectedTargets.insert(target);
        }
        targets.removeDuplicates();
        targetsByProject[projectName].append(targets);
        for (const QJsonValue &fileValue : fileValues) {
            const QJsonObject file = fileValue.toObject();
            const QString path = QDir::cleanPath(file.value(QStringLiteral("path")).toString());
            const QString role = file.value(QStringLiteral("role")).toString();
            if (!safeTrackedPath(path) || role.isEmpty()) {
                return fail(QStringLiteral("The managed source manifest contains an invalid path."));
            }
            ManifestPath descriptor = manifestPaths.value(path);
            if (descriptor.metadata.isEmpty()) {
                descriptor.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("source_kind"), QStringLiteral("source")}, {QStringLiteral("project"), projectName}, {QStringLiteral("role"), role}};
            }
            descriptor.targets.append(targets);
            descriptor.targets.removeDuplicates();
            manifestPaths.insert(path, descriptor);
        }
    }
    for (const auto &raw : manifestObject.value(QStringLiteral("shared_files")).toArray()) {
        const auto entry = raw.toObject(); const auto path = entry.value(QStringLiteral("path")).toString();
        if (!QRegularExpression(QStringLiteral("^\\.thoughts/projects/[a-f0-9]{64}/(default|documents)/thoughts-project\\.json$")).match(path).hasMatch())
            return fail(QStringLiteral("Invalid Project configuration path."));
        ManifestPath descriptor;
        descriptor.metadata = {{QStringLiteral("kind"), QStringLiteral("file")}, {QStringLiteral("source_kind"), QStringLiteral("source")},
            {QStringLiteral("role"), QStringLiteral("render_manifest")}};
        // A Project file affects only its own folders' PDFs. Listing every
        // PDF scoped its publication to an unrelated Project, which Fedora
        // rejects. An unreadable file has no targets, forcing a global Sync.
        const QJsonObject configuration = Thoughts::ProjectConfiguration::read(QDir(docsRoot).filePath(path));
        for (const QJsonValue &folder : configuration.value(QStringLiteral("source_folders")).toArray()) {
            descriptor.targets.append(targetsByProject.value(folder.toObject().value(QStringLiteral("path")).toString()));
        }
        descriptor.targets.removeDuplicates();
        manifestPaths.insert(path, descriptor);
    }
    // Only Project files and no targets is valid until this device installs
    // a Project's source folder.
    if (manifestPaths.isEmpty()) {
        return fail(QStringLiteral("The managed source manifest has no tracked coverage."));
    }

    QHash<QString, Thoughts::DesktopTrackedPath> baselines;
    const QJsonObject bases = sourceStateDocument.object().value(QStringLiteral("bases")).toObject();
    for (auto iterator = manifestPaths.cbegin(); iterator != manifestPaths.cend(); ++iterator) {
        const QJsonObject accepted = bases.value(iterator.key()).toObject();
        const QString sha256 = accepted.value(QStringLiteral("sha256")).toString();
        if (!validSha256(sha256)) {
            // Newly discovered files in another project have not necessarily
            // been published yet. They must remain pending, rather than block
            // this target or become accepted from unacknowledged local bytes.
            if (scoped && std::none_of(scopeTargets.cbegin(), scopeTargets.cend(), [&iterator](const QString &target) { return iterator.value().targets.contains(target); })) {
                continue;
            }
            return fail(QStringLiteral("The accepted source coverage is missing %1.").arg(iterator.key()));
        }
        Thoughts::DesktopTrackedPath tracked;
        tracked.path = iterator.key();
        tracked.exists = true;
        tracked.sha256 = sha256;
        tracked.metadata = iterator.value().metadata;
        tracked.affectedTargets = iterator.value().targets;
        baselines.insert(tracked.path, tracked);
    }

    if (scoped) {
        for (const QString &scopeTarget : scopeTargets) {
            if (!safePdfFilename(scopeTarget) || !expectedTargets.contains(scopeTarget)) {
                return fail(QStringLiteral("The scoped source publication target is invalid."));
            }
        }

        // A scoped publication replaces only this target's dependency
        // closure. Keep every other target from the last accepted baseline;
        // otherwise publishing one PDF could silently forget a shared asset
        // which another PDF still depends on.
        for (Thoughts::DesktopTrackedPath previous : previousBaselines) {
            if (!previous.exists || previous.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory") || previous.metadata.value(QStringLiteral("source_kind")).toString() != QLatin1String("build_input")) {
                continue;
            }
            for (const QString &scopeTarget : scopeTargets) {
                previous.affectedTargets.removeAll(scopeTarget);
            }
            previous.affectedTargets = normalizedDesktopSyncTargets(previous.affectedTargets);
            if (previous.affectedTargets.isEmpty()) {
                continue;
            }
            Thoughts::DesktopTrackedPath retained = baselines.value(previous.path);
            if (retained.path.isEmpty()) {
                retained = previous;
            } else {
                retained.affectedTargets.append(previous.affectedTargets);
                retained.affectedTargets = normalizedDesktopSyncTargets(retained.affectedTargets);
            }
            baselines.insert(retained.path, retained);
        }
    }

    const QJsonObject buildInputs = publication.value(QStringLiteral("build_input_entries")).toObject();
    QSet<QString> coveredTargets;
    for (auto iterator = buildInputs.begin(); iterator != buildInputs.end(); ++iterator) {
        const QString target = iterator.key();
        const QJsonObject accepted = iterator.value().toObject();
        const QString project = accepted.value(QStringLiteral("project")).toString();
        const QString snapshotSha256 = accepted.value(QStringLiteral("snapshot_sha256")).toString();
        if (!expectedTargets.contains(target) || (scoped && !scopeTargets.contains(target)) || project.isEmpty() || !validSha256(snapshotSha256) || !accepted.value(QStringLiteral("entries")).isArray()) {
            return fail(QStringLiteral("The accepted build-input coverage is malformed."));
        }
        coveredTargets.insert(target);
        for (const QJsonValue &entryValue : accepted.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject entry = entryValue.toObject();
            const QString path = QDir::cleanPath(entry.value(QStringLiteral("path")).toString());
            const QString sha256 = entry.value(QStringLiteral("sha256")).toString();
            const qint64 size = entry.value(QStringLiteral("size_bytes")).toInteger(-1);
            const QString role = entry.value(QStringLiteral("role")).toString();
            const QString mediaType = entry.value(QStringLiteral("media_type")).toString();
            if (!safeTrackedPath(path) || !validSha256(sha256) || size < 0 || role.isEmpty() || mediaType.isEmpty()) {
                return fail(QStringLiteral("The accepted build-input coverage contains an invalid entry."));
            }
            Thoughts::DesktopTrackedPath tracked = baselines.value(path);
            if (!tracked.sha256.isEmpty() && tracked.sha256 != sha256) {
                const bool scopedReplacement = scoped && tracked.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("build_input");
                if (!scopedReplacement) {
                    return fail(QStringLiteral("The accepted source and build-input identities disagree for %1.").arg(path));
                }
                // The old identity may still be retained for another target
                // during the first half of a shared-dependency cutover.  Use
                // the scoped server result as the temporary candidate; the
                // generation-safe acknowledgement below restores the old
                // baseline until every affected target has accepted these
                // exact frozen bytes.
                tracked.sha256 = sha256;
                tracked.sizeBytes = size;
                tracked.exists = true;
                tracked.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")},
                                                {QStringLiteral("source_kind"), QStringLiteral("build_input")},
                                                {QStringLiteral("project"), project},
                                                {QStringLiteral("role"), role},
                                                {QStringLiteral("media_type"), mediaType}};
            }
            if (tracked.path.isEmpty()) {
                tracked.path = path;
                tracked.exists = true;
                tracked.sha256 = sha256;
                tracked.sizeBytes = size;
                tracked.metadata = QJsonObject {{QStringLiteral("kind"), QStringLiteral("file")},
                                                {QStringLiteral("source_kind"), QStringLiteral("build_input")},
                                                {QStringLiteral("project"), project},
                                                {QStringLiteral("role"), role},
                                                {QStringLiteral("media_type"), mediaType}};
            }
            tracked.affectedTargets.append(target);
            tracked.affectedTargets = normalizedDesktopSyncTargets(tracked.affectedTargets);
            baselines.insert(path, tracked);
        }
    }
    const QSet<QString> requiredTargets = scoped ? QSet<QString>(scopeTargets.cbegin(), scopeTargets.cend()) : expectedTargets;
    if (coveredTargets != requiredTargets) {
        return fail(QStringLiteral("The source publisher did not return complete build-input coverage."));
    }

    const QList<Thoughts::DesktopTrackedPath> result =
        desktopMonitoringBaselines(docsRoot, baselines.values());
    if (error) {
        error->clear();
    }
    return result;
}

QHash<QString, QPair<QString, qint64>> publicationArtifactIdentities(const QJsonArray &publications, bool *valid = nullptr)
{
    QHash<QString, QPair<QString, qint64>> identities;
    bool complete = true;
    for (const QJsonValue &value : publications) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (!safePdfFilename(filename) || !validSha256(digest) || size < 0 || identities.contains(filename)) {
            complete = false;
            break;
        }
        identities.insert(filename, qMakePair(digest, size));
    }
    if (valid) {
        *valid = complete && identities.size() == publications.size();
    }
    return identities;
}

bool activeBuildStatus(const QString &status)
{
    return status == QLatin1String("preparing") || status == QLatin1String("publishing") || status == QLatin1String("queued") || status == QLatin1String("running") || status == QLatin1String("activating");
}

QJsonArray deferredTargetedBuilds(const QJsonObject &state)
{
    QJsonArray targets;
    QSet<QString> seen;
    for (const QJsonValue &value : state.value(QStringLiteral("deferred_targeted_builds")).toArray()) {
        const QString target = value.toString();
        if (safePdfFilename(target) && !seen.contains(target)) {
            targets.append(target);
            seen.insert(target);
        }
    }
    return targets;
}

void storeDeferredTargetedBuilds(QJsonObject &state, const QJsonArray &targets)
{
    if (targets.isEmpty()) {
        state.remove(QStringLiteral("deferred_targeted_builds"));
    } else {
        state.insert(QStringLiteral("deferred_targeted_builds"), targets);
    }
}

QByteArray fileSha256(const QString &path)
{
    // Status refreshes check every Library PDF, often twice. Rehash a file only
    // when its identity (inode, size, mtime, ctime) changed since it was hashed.
    static QMutex mutex;
    static QHash<QString, QByteArray> digests;
    const QString identity = Thoughts::artifactFileIdentity(path);
    if (!identity.isEmpty()) {
        QMutexLocker lock(&mutex);
        if (const auto found = digests.constFind(identity); found != digests.constEnd()) return *found;
    }
    const QByteArray digest = Thoughts::artifactFileSha256(path);
    if (!identity.isEmpty() && !digest.isEmpty() && Thoughts::artifactFileIdentity(path) == identity) {
        QMutexLocker lock(&mutex);
        if (digests.size() >= 16384) digests.clear();
        digests.insert(identity, digest);
    }
    return digest;
}

// Parse a small state file once per version instead of once per caller.
QJsonDocument cachedJsonFile(const QString &path)
{
    static QMutex mutex;
    static QHash<QString, QPair<QString, QJsonDocument>> documents;
    const QString identity = Thoughts::artifactFileIdentity(path);
    if (identity.isEmpty()) return {};
    {
        QMutexLocker lock(&mutex);
        if (const auto found = documents.constFind(path); found != documents.constEnd() && found->first == identity) return found->second;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (Thoughts::artifactFileIdentity(path) == identity) {
        QMutexLocker lock(&mutex);
        if (documents.size() >= 64) documents.clear();
        documents.insert(path, qMakePair(identity, document));
    }
    return document;
}

QJsonObject readerFileMetadata(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {};
    }
    return {{QStringLiteral("local_path"), info.absoluteFilePath()},
            {QStringLiteral("size_bytes"), info.size()},
            {QStringLiteral("modified_at_ms"), info.lastModified().toMSecsSinceEpoch()},
            {QStringLiteral("metadata_changed_at_ms"), info.metadataChangeTime().toMSecsSinceEpoch()},
            {QStringLiteral("created_at_ms"), info.birthTime().toMSecsSinceEpoch()}};
}

bool sameReaderFileMetadata(const QJsonObject &left, const QJsonObject &right)
{
    static const QStringList fields {QStringLiteral("local_path"), QStringLiteral("size_bytes"), QStringLiteral("modified_at_ms"), QStringLiteral("metadata_changed_at_ms"), QStringLiteral("created_at_ms")};
    return std::all_of(fields.cbegin(), fields.cend(), [&left, &right](const QString &field) { return left.value(field) == right.value(field); });
}

QJsonObject currentPublicationForTarget(const QJsonObject &state, const QString &target)
{
    for (const QJsonValue &value : state.value(QStringLiteral("library")).toObject().value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        if (publication.value(QStringLiteral("filename")).toString() == target) {
            return publication;
        }
    }
    const QJsonObject publication = state.value(QStringLiteral("render_statuses")).toObject().value(target).toObject().value(QStringLiteral("publication")).toObject();
    return publication.value(QStringLiteral("filename")).toString() == target ? publication : QJsonObject();
}

QJsonObject publicationIdentityForBuild(const QString &target, const QJsonObject &build, const QJsonObject &currentPublication)
{
    if (!safePdfFilename(target)) {
        return {};
    }
    for (const QJsonValue &value : build.value(QStringLiteral("result")).toObject().value(QStringLiteral("artifacts")).toArray()) {
        const QJsonObject artifact = value.toObject();
        const QString digest = artifact.value(QStringLiteral("sha256")).toString();
        const qint64 size = artifact.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (artifact.value(QStringLiteral("filename")).toString() != target || artifact.value(QStringLiteral("media_type")).toString() != QLatin1String("application/pdf") || !validSha256(digest) || size < 0) {
            continue;
        }
        QJsonObject publication {{QStringLiteral("filename"), target},
                                 {QStringLiteral("sha256"), digest},
                                 {QStringLiteral("size_bytes"), size},
                                 {QStringLiteral("build_id"), artifact.value(QStringLiteral("build_id")).toString(build.value(QStringLiteral("id")).toString())},
                                 {QStringLiteral("source_frontier"), build.value(QStringLiteral("source_frontier"))},
                                 {QStringLiteral("source_artifact_sha256"), artifact.value(QStringLiteral("source_artifact_sha256"))}};
        if (artifact.value(QStringLiteral("source_resources")).isArray()) {
            publication.insert(QStringLiteral("source_resources"), artifact.value(QStringLiteral("source_resources")));
        }
        const QJsonObject delta = artifact.value(QStringLiteral("delta")).toObject();
        if (!delta.isEmpty()) {
            publication.insert(QStringLiteral("delta"), delta);
        }
        return publication;
    }
    for (const QJsonValue &value : build.value(QStringLiteral("result")).toObject().value(QStringLiteral("outcomes")).toArray()) {
        const QJsonObject outcome = value.toObject();
        const QString digest = outcome.value(QStringLiteral("sha256")).toString();
        const QString sourceArtifact = outcome.value(QStringLiteral("source_artifact_sha256")).toString();
        const qint64 size = currentPublication.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (outcome.value(QStringLiteral("filename")).toString() != target || outcome.value(QStringLiteral("status")).toString() != QLatin1String("unchanged") || currentPublication.value(QStringLiteral("filename")).toString() != target ||
            currentPublication.value(QStringLiteral("sha256")).toString() != digest || !validSha256(digest) || !validSha256(sourceArtifact) || size < 0) {
            continue;
        }
        QJsonObject publication = currentPublication;
        publication.insert(QStringLiteral("filename"), target);
        publication.insert(QStringLiteral("sha256"), digest);
        publication.insert(QStringLiteral("size_bytes"), size);
        publication.insert(QStringLiteral("build_id"), build.value(QStringLiteral("id")));
        publication.insert(QStringLiteral("source_frontier"), build.value(QStringLiteral("source_frontier")));
        publication.insert(QStringLiteral("source_artifact_sha256"), sourceArtifact);
        if (outcome.value(QStringLiteral("source_resources")).isArray()) {
            publication.insert(QStringLiteral("source_resources"), outcome.value(QStringLiteral("source_resources")));
        }
        if (outcome.contains(QStringLiteral("placement_manifest_sha256"))) {
            publication.insert(QStringLiteral("placement_manifest_sha256"), outcome.value(QStringLiteral("placement_manifest_sha256")));
        }
        const QJsonObject delta = outcome.value(QStringLiteral("delta")).toObject();
        if (!delta.isEmpty()) {
            publication.insert(QStringLiteral("delta"), delta);
        } else {
            publication.remove(QStringLiteral("delta"));
        }
        return publication;
    }
    return {};
}

QJsonObject publicationWithBuildSourceArtifact(QJsonObject publication, const QString &target, const QJsonObject &build)
{
    const QJsonObject buildPublication = publicationIdentityForBuild(target, build, publication);
    if (buildPublication.isEmpty()) {
        return publication;
    }
    for (const QString &key :
         {QStringLiteral("filename"), QStringLiteral("sha256"), QStringLiteral("size_bytes"), QStringLiteral("build_id"), QStringLiteral("source_frontier"), QStringLiteral("source_artifact_sha256"), QStringLiteral("source_resources")}) {
        if (buildPublication.contains(key)) {
            publication.insert(key, buildPublication.value(key));
        }
    }
    if (!buildPublication.value(QStringLiteral("delta")).toObject().isEmpty()) {
        publication.insert(QStringLiteral("delta"), buildPublication.value(QStringLiteral("delta")));
    }
    return publication;
}

QString payloadTimestamp(const QJsonObject &payload)
{
    for (const QString &key : {QStringLiteral("updated_at"), QStringLiteral("deleted_at"), QStringLiteral("modified"), QStringLiteral("created"), QStringLiteral("reviewed_at")}) {
        const QString value = payload.value(key).toString();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QByteArray fingerprint(const QJsonObject &payload)
{
    return QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
}

bool writeJsonFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    // Rewriting identical state (e.g. every document pointer on each activation)
    // only costs I/O and wakes file watchers.
    if (QFile existing(path); !contents.isEmpty() && existing.size() == contents.size() && existing.open(QIODevice::ReadOnly) && existing.readAll() == contents) {
        return true;
    }
    for (int attempt = 0;; ++attempt) {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit()) {
            return true;
        }
#ifdef Q_OS_WIN
        // Windows refuses to replace a file another reader, the search
        // indexer or antivirus holds open; those holds are brief.
        if (attempt < 20) {
            QThread::msleep(25);
            continue;
        }
#endif
        return false;
    }
}

QString buildTraceCursorPath(const QString &stateRoot, const QString &traceId)
{
    const QByteArray name = QCryptographicHash::hash(traceId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(stateRoot).filePath(QStringLiteral("build-trace-cursors/%1.json").arg(QString::fromLatin1(name)));
}

QJsonObject readBuildTraceCursor(const QString &stateRoot, const QString &traceId, const QString &buildId, const QString &target)
{
    QFile file(buildTraceCursorPath(stateRoot, traceId));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject cursor = QJsonDocument::fromJson(file.readAll()).object();
    if (cursor.value(QStringLiteral("trace_id")).toString() != traceId
        || cursor.value(QStringLiteral("build_id")).toString() != buildId
        || cursor.value(QStringLiteral("target")).toString() != target
        || cursor.value(QStringLiteral("last_server_sequence")).toVariant().toLongLong() < 0) return {};
    return cursor;
}

QJsonObject documentSourceArtifactPointer(const QJsonObject &publication)
{
    return {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact-pointer")},
            {QStringLiteral("filename"), publication.value(QStringLiteral("filename"))},
            {QStringLiteral("artifact_sha256"), publication.value(QStringLiteral("source_artifact_sha256"))},
            {QStringLiteral("pdf_sha256"), publication.value(QStringLiteral("sha256"))}};
}

bool documentSourceArtifactMatchesPublication(const QByteArray &contents, const QJsonObject &publication)
{
    const QString filename = publication.value(QStringLiteral("filename")).toString();
    const QString artifactSha256 = publication.value(QStringLiteral("source_artifact_sha256")).toString();
    const QString pdfSha256 = publication.value(QStringLiteral("sha256")).toString();
    if (!Thoughts::SyncClient::verifyObject(contents, artifactSha256.toLatin1())) return false;
    const QJsonObject manifest = Thoughts::sourceArtifactJson(contents, artifactSha256);
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    return safePdfFilename(filename) && validSha256(artifactSha256) && validSha256(pdfSha256) &&
        manifest.value(QStringLiteral("kind")).toString() == QLatin1String("thoughts-document-source-artifact") && manifest.value(QStringLiteral("filename")).toString() == filename &&
        pdf.value(QStringLiteral("filename")).toString() == filename && pdf.value(QStringLiteral("pdf_sha256")).toString() == pdfSha256;
}

bool safeSourceArtifactPath(const QString &value)
{
    const bool markdown = value.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || value.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
    if (value.isEmpty() || (!value.endsWith(QLatin1String(".tex")) && !markdown) || value.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]"))) || value.startsWith(QLatin1Char('/')) || value.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QStringList parts = value.split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString &part) { return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."); });
}

bool safeDocumentStyleResource(const QJsonObject &resource, bool artifactShape)
{
    const QString kind = resource.value(QStringLiteral("kind")).toString();
    const QString path = QDir::cleanPath(resource.value(QStringLiteral("path")).toString());
    const QString sha256 = resource.value(QStringLiteral("sha256")).toString();
    const QString object = resource.value(artifactShape ? QStringLiteral("object") : QStringLiteral("object_sha256")).toString();
    const qint64 bytes = resource.value(artifactShape ? QStringLiteral("bytes") : QStringLiteral("size_bytes")).toInteger(-1);
    static const QRegularExpression projectOverride(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*/tex/support/manual_style_override\\.sty$"));
    const QRegularExpression projectStyle(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*/tex/support/manual_style\\.sty$"));
    const bool pathMatchesKind = kind == QLatin1String("project_style") ? projectStyle.match(path).hasMatch() : kind == QLatin1String("project_style_override") && projectOverride.match(path).hasMatch();
    const bool html = kind == QLatin1String("html_attachment") && Thoughts::isHtmlSourcePath(path) && bytes > 0 && bytes <= 100000000;
    return (pathMatchesKind || html) && resource.value(QStringLiteral("path")).toString() == path && validSha256(sha256) && object == sha256 && bytes >= 0 && (html || bytes <= 4 * 1024 * 1024);
}

QHash<QString, QJsonObject> normalizedDocumentStyleResources(const QJsonArray &resources, bool artifactShape, bool *valid)
{
    if (valid) {
        *valid = false;
    }
    QHash<QString, QJsonObject> normalized;
    for (const QJsonValue &value : resources) {
        const QJsonObject resource = value.toObject();
        if (!safeDocumentStyleResource(resource, artifactShape)) {
            return {};
        }
        const QString kind = resource.value(QStringLiteral("kind")).toString();
        const QString key = kind == QLatin1String("html_attachment") ? resource.value(QStringLiteral("path")).toString() : kind;
        if (normalized.contains(key)) {
            return {};
        }
        normalized.insert(key,
                          QJsonObject {{QStringLiteral("kind"), kind},
                                       {QStringLiteral("path"), resource.value(QStringLiteral("path"))},
                                       {QStringLiteral("sha256"), resource.value(QStringLiteral("sha256"))},
                                       {QStringLiteral("object"), resource.value(artifactShape ? QStringLiteral("object") : QStringLiteral("object_sha256"))},
                                       {QStringLiteral("bytes"), resource.value(artifactShape ? QStringLiteral("bytes") : QStringLiteral("size_bytes"))}});
    }
    if (valid) {
        *valid = true;
    }
    return normalized;
}

bool verifiedSourceBundleObject(const QDir &bundleRoot, const QString &digest, QByteArray *contents = nullptr)
{
    return validSha256(digest) && Thoughts::readVerifiedArtifact(bundleRoot.filePath(QStringLiteral("objects/%1").arg(digest)), digest, contents);
}

struct StreamedBundleObject {
    QSaveFile file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 size = 0;
    qint64 expected;
    bool valid;
    StreamedBundleObject(const QString &path, qint64 limit) : file(path), expected(limit) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        file.setDirectWriteFallback(false);
        valid = expected >= 0 && file.open(QIODevice::WriteOnly);
    }
    void drain(QNetworkReply *reply) {
        while (reply->bytesAvailable()) {
            const auto chunk = reply->read(64 * 1024);
            size += chunk.size();
            if (!valid || size > expected || file.write(chunk) != chunk.size()) { valid = false; reply->abort(); return; }
            hash.addData(chunk);
        }
    }
    bool finish(QNetworkReply *reply, const QString &digest) {
        drain(reply);
        return valid && reply->error() == QNetworkReply::NoError && size == expected
            && hash.result().toHex() == digest.toLatin1() && file.commit();
    }
};

QSharedPointer<StreamedBundleObject> streamBundleObject(QNetworkReply *reply, const QDir &root, const QString &digest, qint64 size)
{
    auto stream = QSharedPointer<StreamedBundleObject>::create(root.filePath(QStringLiteral("objects/") + digest), size);
    reply->setReadBufferSize(256 * 1024);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, stream] { stream->drain(reply); });
    return stream;
}

bool cacheSourceBundleObject(const QDir &bundleRoot, const QString &digest, const QByteArray &contents)
{
    return validSha256(digest) && Thoughts::SyncClient::verifyObject(contents, digest.toLatin1()) && writeJsonFile(bundleRoot.filePath(QStringLiteral("objects/%1").arg(digest)), contents);
}

bool requiredDocumentSourceArtifactObjects(const QByteArray &contents, const QJsonObject &publication, QSet<QString> *objects, QString *error)
{
    if (!documentSourceArtifactMatchesPublication(contents, publication)) {
        if (error) {
            *error = QStringLiteral("The document source-artifact manifest does not match its PDF publication.");
        }
        return false;
    }
    const QJsonObject manifest = Thoughts::sourceArtifactJson(contents, publication.value(QStringLiteral("source_artifact_sha256")).toString());
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    const int pageCount = pdf.value(QStringLiteral("page_count")).toInt();
    const QJsonArray segments = pdf.value(QStringLiteral("segments")).toArray();
    if (pageCount < 1 || segments.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The document source artifact has no verified page layout.");
        }
        return false;
    }

    QHash<QString, QPair<QString, QString>> sources;
    for (const QJsonValue &value : manifest.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        const QString path = source.value(QStringLiteral("path")).toString();
        const QString digest = source.value(QStringLiteral("sha256")).toString();
        const QString object = source.value(QStringLiteral("object")).toString();
        if (!safeSourceArtifactPath(path) || !validSha256(digest) || object != digest || sources.contains(path)) {
            if (error) {
                *error = QStringLiteral("The document source artifact contains an invalid source snapshot.");
            }
            return false;
        }
        sources.insert(path, qMakePair(digest, object));
        objects->insert(object);
    }

    // Metadata-only source updates retain the exact snapshots used to render
    // the PDF. Its input maps refer to those placement snapshots, while the
    // editable source snapshots can already have newer hashes.
    auto placementSources = sources;
    if (manifest.contains(QStringLiteral("placement_sources"))) {
        placementSources.clear();
        if (!manifest.value(QStringLiteral("placement_sources")).isArray()) {
            if (error) *error = QStringLiteral("The document source artifact has an invalid placement source map.");
            return false;
        }
        for (const QJsonValue &value : manifest.value(QStringLiteral("placement_sources")).toArray()) {
            const QJsonObject source = value.toObject();
            const QString path = source.value(QStringLiteral("path")).toString();
            const QString digest = source.value(QStringLiteral("sha256")).toString();
            const QString object = source.value(QStringLiteral("object")).toString();
            if (!sources.contains(path) || placementSources.contains(path) || !validSha256(digest) || object != digest) {
                if (error) *error = QStringLiteral("The document source artifact has an invalid placement source map.");
                return false;
            }
            placementSources.insert(path, qMakePair(digest, object));
            objects->insert(object);
        }
        if (placementSources.size() != sources.size()) {
            if (error) *error = QStringLiteral("The document source artifact has an incomplete placement source map.");
            return false;
        }
    }

    const bool artifactResourcesDeclared = manifest.contains(QStringLiteral("resources"));
    if (artifactResourcesDeclared && !manifest.value(QStringLiteral("resources")).isArray()) {
        if (error) {
            *error = QStringLiteral("The document source artifact contains malformed style resources.");
        }
        return false;
    }
    bool artifactResourcesValid = false;
    const QHash<QString, QJsonObject> artifactResources = normalizedDocumentStyleResources(manifest.value(QStringLiteral("resources")).toArray(), true, &artifactResourcesValid);
    if (artifactResourcesDeclared && !artifactResourcesValid) {
        if (error) {
            *error = QStringLiteral("The document source artifact contains invalid style resources.");
        }
        return false;
    }
    for (const QJsonObject &resource : artifactResources) {
        objects->insert(resource.value(QStringLiteral("object")).toString());
    }
    const bool publicationResourcesDeclared = publication.contains(QStringLiteral("source_resources"));
    if (publicationResourcesDeclared && !publication.value(QStringLiteral("source_resources")).isArray()) {
        if (error) {
            *error = QStringLiteral("The Library publication contains malformed style-resource identities.");
        }
        return false;
    }
    bool publicationResourcesValid = false;
    const QHash<QString, QJsonObject> publicationResources = normalizedDocumentStyleResources(publication.value(QStringLiteral("source_resources")).toArray(), false, &publicationResourcesValid);
    if (publicationResourcesDeclared && (!publicationResourcesValid || !artifactResourcesDeclared || publicationResources != artifactResources)) {
        if (error) {
            *error = QStringLiteral("The document source artifact does not match its published style-resource identities.");
        }
        return false;
    }

    int expectedStart = 1;
    for (const QJsonValue &value : segments) {
        const QJsonObject segment = value.toObject();
        const int start = segment.value(QStringLiteral("start_page")).toInt();
        const int count = segment.value(QStringLiteral("page_count")).toInt();
        if (start != expectedStart || count < 1) {
            if (error) {
                *error = QStringLiteral("The document source artifact has a discontinuous page layout.");
            }
            return false;
        }
        expectedStart += count;
        if (!segment.value(QStringLiteral("source_available")).toBool()) {
            continue;
        }
        if (segment.value(QStringLiteral("source_format")).toString() == QLatin1String("markdown")) {
            const QString evidence = segment.value(QStringLiteral("renderer_evidence_object")).toString();
            if (!validSha256(evidence) || evidence != segment.value(QStringLiteral("renderer_evidence_sha256")).toString()
                || segment.value(QStringLiteral("lookup_format")).toString() != QLatin1String("thoughts-markdown-source-map-v1")
                || manifest.value(QStringLiteral("review_placement_model_version")).toInt() != 4) {
                if (error) *error = QStringLiteral("The Markdown source segment has invalid renderer evidence.");
                return false;
            }
            objects->insert(evidence);
        }
        const QJsonArray inputs = segment.value(QStringLiteral("inputs")).toArray();
        const QString lookupObject = segment.value(QStringLiteral("lookup_object")).toString();
        const QString lookupSha256 = segment.value(QStringLiteral("lookup_sha256")).toString();
        const QString synctexObject = segment.value(QStringLiteral("synctex_object")).toString();
        const QString synctexSha256 = segment.value(QStringLiteral("synctex_sha256")).toString();
        if (inputs.isEmpty() || (!validSha256(lookupObject) && !validSha256(synctexObject)) || (validSha256(lookupObject) && lookupObject != lookupSha256) || (validSha256(synctexObject) && synctexObject != synctexSha256)) {
            if (error) {
                *error = QStringLiteral("The document source artifact has an invalid SyncTeX segment.");
            }
            return false;
        }
        if (validSha256(lookupObject)) {
            objects->insert(lookupObject);
        }
        // The Library plan includes both formats. Retain both even though
        // Desktop's source navigation prefers the precomputed reverse lookup.
        if (validSha256(synctexObject)) {
            objects->insert(synctexObject);
        }
        for (const QJsonValue &inputValue : inputs) {
            const QJsonObject input = inputValue.toObject();
            const QString sourceFile = input.value(QStringLiteral("source_file")).toString();
            const QString sourceSha256 = input.value(QStringLiteral("source_sha256")).toString();
            if (!placementSources.contains(sourceFile) || placementSources.value(sourceFile).first != sourceSha256 || input.value(QStringLiteral("synctex_path")).toString().isEmpty()) {
                if (error) {
                    *error = QStringLiteral("The document source artifact has an invalid source input map.");
                }
                return false;
            }
        }
    }
    if (expectedStart - 1 != pageCount) {
        if (error) {
            *error = QStringLiteral("The document source artifact page count does not match its layout.");
        }
        return false;
    }
    for (const QJsonValue &value : manifest.value(QStringLiteral("proposal_objects")).toArray()) {
        const QString object = value.toObject().value(QStringLiteral("object")).toString();
        if (!validSha256(object)) {
            if (error) {
                *error = QStringLiteral("The document source artifact contains an invalid proposal object.");
            }
            return false;
        }
    }
    return true;
}

// Declared style resources only add a check that they equal the artifact's own;
// the required objects are the same. A receipt verified with declared resources
// therefore also covers a caller that declares none (render-status publications
// omit them), which previously rewrote every receipt back and forth each Sync.
bool receiptCoversPublication(QJsonObject receipt, const QJsonObject &expected)
{
    if (receipt == expected) return true;
    if (expected.contains(QStringLiteral("source_resources"))) return false;
    receipt.remove(QStringLiteral("source_resources"));
    return receipt == expected;
}

bool sourceArtifactRequirements(const QDir &bundleRoot, const QJsonObject &publication, QSet<QString> *required, QString *error)
{
    const QString digest = publication.value(QStringLiteral("source_artifact_sha256")).toString();
    if (!validSha256(digest)) return false;
    const QString objectPath = bundleRoot.filePath(QStringLiteral("objects/%1").arg(digest));
    const QString identity = Thoughts::artifactFileIdentity(objectPath);
    const QString receiptPath = bundleRoot.filePath(QStringLiteral("verified-requirements/%1.json").arg(digest));
    QJsonObject expected = documentSourceArtifactPointer(publication);
    if (publication.contains(QStringLiteral("source_resources"))) expected.insert(QStringLiteral("source_resources"), publication.value(QStringLiteral("source_resources")));
    // Every status refresh, reader plan and activation asks this for each document.
    // Remember a validated receipt until the object, the receipt or the request changes.
    static QMutex memoMutex;
    static QHash<QString, QSet<QString>> memo;
    const QString memoKey = identity + QLatin1Char('\n') + QString::fromUtf8(QJsonDocument(expected).toJson(QJsonDocument::Compact));
    const auto remember = [&](const QSet<QString> &objects) {
        const QString receiptIdentity = Thoughts::artifactFileIdentity(receiptPath);
        if (identity.isEmpty() || receiptIdentity.isEmpty()) return;
        QMutexLocker lock(&memoMutex);
        if (memo.size() >= 8192) memo.clear();
        memo.insert(memoKey + QLatin1Char('\n') + receiptIdentity, objects);
    };
    if (!identity.isEmpty()) {
        const QString receiptIdentity = Thoughts::artifactFileIdentity(receiptPath);
        QMutexLocker lock(&memoMutex);
        if (const auto found = memo.constFind(memoKey + QLatin1Char('\n') + receiptIdentity); !receiptIdentity.isEmpty() && found != memo.constEnd()) {
            *required = *found;
            return true;
        }
    }
    QFile receiptFile(receiptPath);
    if (!identity.isEmpty() && receiptFile.open(QIODevice::ReadOnly) && receiptFile.size() < 1024 * 1024) {
        const QJsonObject receipt = QJsonDocument::fromJson(receiptFile.readAll()).object();
        const QJsonObject payload = receipt.value(QStringLiteral("payload")).toObject();
        const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        if (payload.value(QStringLiteral("schema_version")).toInt() == 2 && payload.value(QStringLiteral("identity")).toString() == identity
            && receiptCoversPublication(payload.value(QStringLiteral("publication")).toObject(), expected)
            && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == receipt.value(QStringLiteral("sha256")).toString().toLatin1()
            && payload.value(QStringLiteral("objects")).isArray()) {
            QSet<QString> objects;
            bool valid = true;
            for (const auto &value : payload.value(QStringLiteral("objects")).toArray()) {
                valid &= validSha256(value.toString());
                objects.insert(value.toString());
            }
            if (valid) { *required = objects; remember(objects); return true; }
        }
    }
    receiptFile.close();
    QByteArray contents;
    if (!verifiedSourceBundleObject(bundleRoot, digest, &contents) || !requiredDocumentSourceArtifactObjects(contents, publication, required, error)) return false;
    QStringList sorted = required->values();
    std::sort(sorted.begin(), sorted.end());
    const QJsonObject payload {{QStringLiteral("schema_version"), 2}, {QStringLiteral("identity"), identity},
        {QStringLiteral("publication"), expected}, {QStringLiteral("objects"), QJsonArray::fromStringList(sorted)}};
    const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QDir().mkpath(QFileInfo(receiptPath).absolutePath());
    if (writeJsonFile(receiptPath, QJsonDocument(QJsonObject {{QStringLiteral("payload"), payload},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact)))
        remember(*required);
    return true;
}

bool completeDocumentSourceArtifact(const QDir &bundleRoot, const QJsonObject &publication, QSet<QString> *missing = nullptr, QString *error = nullptr)
{
    const QString artifactSha256 = publication.value(QStringLiteral("source_artifact_sha256")).toString();
    QSet<QString> required;
    if (!sourceArtifactRequirements(bundleRoot, publication, &required, error)) {
        if (missing && (!error || error->isEmpty())) missing->insert(artifactSha256);
        return false;
    }
    bool complete = true;
    for (const QString &object : std::as_const(required)) {
        if (!verifiedSourceBundleObject(bundleRoot, object)) {
            complete = false;
            if (missing) {
                missing->insert(object);
            }
        }
    }
    return complete;
}

bool activateDocumentSourceArtifact(const QDir &bundleRoot, const QJsonObject &publication)
{
    return completeDocumentSourceArtifact(bundleRoot, publication) &&
        writeJsonFile(bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(publication.value(QStringLiteral("filename")).toString())), QJsonDocument(documentSourceArtifactPointer(publication)).toJson(QJsonDocument::Compact));
}

bool cachedDocumentSourceArtifactMatches(const QDir &bundleRoot, const QJsonObject &publication)
{
    return completeDocumentSourceArtifact(bundleRoot, publication);
}

bool writeActiveSourceArtifactIndex(const QDir &bundleRoot, const QJsonObject &library, const QHash<QString, QJsonObject> &publications)
{
    QStringList filenames = publications.keys();
    std::sort(filenames.begin(), filenames.end());
    QJsonArray documents;
    for (const QString &filename : std::as_const(filenames)) {
        const QJsonObject publication = publications.value(filename);
        documents.append(QJsonObject {
            {QStringLiteral("filename"), filename},
            {QStringLiteral("pdf_sha256"), publication.value(QStringLiteral("sha256"))},
            {QStringLiteral("artifact_sha256"), publication.value(QStringLiteral("source_artifact_sha256"))},
        });
    }
    const QJsonObject index {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("kind"), QStringLiteral("thoughts-desktop-source-artifact-set")},
        {QStringLiteral("library_revision"), library.value(QStringLiteral("library_revision"))},
        {QStringLiteral("artifact_revision"), effectiveArtifactRevision(library)},
        {QStringLiteral("documents"), documents},
    };
    return writeJsonFile(bundleRoot.filePath(QStringLiteral("active-index.json")), QJsonDocument(index).toJson(QJsonDocument::Compact) + '\n');
}

QHash<QString, QJsonObject> readerSourcePublications(const QJsonObject &library)
{
    QHash<QString, QJsonObject> publications;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString artifactSha256 = publication.value(QStringLiteral("source_artifact_sha256")).toString();
        const QString pdfSha256 = publication.value(QStringLiteral("sha256")).toString();
        if (safePdfFilename(filename) && validSha256(artifactSha256) && validSha256(pdfSha256)) {
            publications.insert(filename, publication);
        }
    }
    return publications;
}

void pruneInactiveReaderSourceObjects(const QDir &bundleRoot, const QList<QJsonObject> &publications, QSet<QString> retained)
{
    for (const QJsonObject &publication : publications) {
        const QString artifact = publication.value(QStringLiteral("source_artifact_sha256")).toString();
        retained.insert(artifact);
        QSet<QString> required;
        // An unreadable live closure cannot justify deleting any cached objects.
        if (!sourceArtifactRequirements(bundleRoot, publication, &required, nullptr)) return;
        retained.unite(required);
    }
    const QDir objects(bundleRoot.filePath(QStringLiteral("objects")));
    for (const QFileInfo &object : objects.entryInfoList(QDir::Files, QDir::Name)) {
        if (validSha256(object.fileName()) && !retained.contains(object.fileName()) && verifiedSourceBundleObject(bundleRoot, object.fileName())) {
            QFile::remove(object.absoluteFilePath());
        }
    }
}

bool activeDocumentSourceArtifactMatches(const QString &docsRoot, const QString &stateRoot, const QJsonObject &publication)
{
    const QString filename = publication.value(QStringLiteral("filename")).toString();
    const QDir bundleRoot(QDir(stateRoot).filePath(QStringLiteral("source-bundle")));
    const QString generationPointer = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-artifacts/%1.json").arg(filename));
    QFile pointer(QFileInfo::exists(generationPointer) ? generationPointer : bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(filename)));
    const QJsonObject pointerObject = pointer.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(pointer.readAll()).object() : QJsonObject();
    if (pointerObject != documentSourceArtifactPointer(publication)) {
        return false;
    }
    const QString currentPdf = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename));
    return fileSha256(currentPdf) == publication.value(QStringLiteral("sha256")).toString().toLatin1() && cachedDocumentSourceArtifactMatches(bundleRoot, publication);
}

struct LibraryActivationInspection {
    QHash<QString, QJsonObject> completedRuns;
    QString error;
};

bool boundedJsonFile(const QString &path, QJsonObject *object, QString *error)
{
    if (object) {
        *object = {};
    }
    if (!QFileInfo(path).isFile()) {
        return false;
    }
    constexpr int maximumBytes = 8 * 1024 * 1024;
    QByteArray contents;
    QString helperError;
#ifndef Q_OS_MACOS
    // A separate reader process only guards against macOS privacy holds on
    // the Documents folder.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Could not read %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    contents = file.read(maximumBytes + 1);
#else
    if (!Okular::Internal::runLocalSourceTool(QStringLiteral("/usr/bin/head"), {QStringLiteral("-c"), QString::number(maximumBytes + 1), QStringLiteral("--"), path}, &contents, &helperError, 5000)) {
        if (error) {
            *error = QStringLiteral("Timed out or could not read %1. Make the Library folder available locally, then retry.%2").arg(path, helperError.isEmpty() ? QString() : QLatin1Char('\n') + helperError);
        }
        return false;
    }
#endif
    const QJsonDocument document = contents.size() <= maximumBytes ? QJsonDocument::fromJson(contents) : QJsonDocument();
    if (!document.isObject()) {
        return false;
    }
    if (object) {
        *object = document.object();
    }
    return true;
}

bool boundedSha256(const QString &path, QString *sha256, QString *error)
{
    if (sha256) {
        sha256->clear();
    }
    if (!QFileInfo(path).isFile()) {
        return false;
    }
    QByteArray output;
    QString helperError;
#ifndef Q_OS_MACOS
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        if (error) {
            *error = QStringLiteral("Could not verify %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    output = hash.result().toHex();
#else
    if (!Okular::Internal::runLocalSourceTool(QStringLiteral("/usr/bin/shasum"), {QStringLiteral("-a"), QStringLiteral("256"), QStringLiteral("--"), path}, &output, &helperError, 10000)) {
        if (error) {
            *error = QStringLiteral("Timed out or could not verify %1. Make the Library folder available locally, then retry.%2").arg(path, helperError.isEmpty() ? QString() : QLatin1Char('\n') + helperError);
        }
        return false;
    }
#endif
    static const QRegularExpression digestPattern(QStringLiteral("^([0-9a-fA-F]{64})(?:\\s|$)"));
    const QRegularExpressionMatch match = digestPattern.match(QString::fromLatin1(output));
    if (!match.hasMatch()) {
        if (error) {
            *error = QStringLiteral("The SHA-256 helper returned malformed output for %1.").arg(path);
        }
        return false;
    }
    if (sha256) {
        *sha256 = match.captured(1).toLower();
    }
    return true;
}

bool boundedLocalPublicationIsCurrent(const QString &docsRoot, const QString &stateRoot, const QString &target, const QJsonObject &publication, const QJsonObject &index, const QJsonObject &manifest, QString *error)
{
    const QString digest = publication.value(QStringLiteral("sha256")).toString();
    const qint64 expectedSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
    const QString buildId = publication.value(QStringLiteral("build_id")).toString();
    const QJsonValue sourceFrontier = publication.value(QStringLiteral("source_frontier"));
    if (!safePdfFilename(target) || publication.value(QStringLiteral("filename")).toString() != target || !validSha256(digest) || !validSha256(publication.value(QStringLiteral("source_artifact_sha256")).toString()) || expectedSize < 0) {
        return false;
    }
    const QDir current(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QString pdfPath = current.filePath(target);
    const QFileInfo pdf(pdfPath);
    QString actualDigest;
    if (!pdf.isFile() || pdf.size() != expectedSize || !boundedSha256(pdfPath, &actualDigest, error) || actualDigest != digest) {
        return false;
    }
    const auto publicationMatches = [&](const QJsonObject &entry, const QString &digestKey) {
        return entry.value(QStringLiteral("filename")).toString() == target && entry.value(digestKey).toString() == digest && (buildId.isEmpty() || entry.value(QStringLiteral("build_id")).toString() == buildId) &&
            (sourceFrontier.isUndefined() || sourceFrontier.isNull() || entry.value(QStringLiteral("source_frontier")) == sourceFrontier);
    };
    const QJsonArray indexedPdfs = index.value(QStringLiteral("pdfs")).toArray();
    const QJsonArray manifestPublications = manifest.value(QStringLiteral("publications")).toArray();
    const bool indexMatches = std::any_of(indexedPdfs.cbegin(), indexedPdfs.cend(), [&](const QJsonValue &value) { return publicationMatches(value.toObject(), QStringLiteral("pdf_sha256")); });
    const bool manifestMatches = std::any_of(manifestPublications.cbegin(), manifestPublications.cend(), [&](const QJsonValue &value) {
        const QJsonObject entry = value.toObject();
        return publicationMatches(entry, QStringLiteral("sha256")) && entry.value(QStringLiteral("size_bytes")).toInteger(-1) == expectedSize;
    });
    if (!indexMatches || !manifestMatches) {
        return false;
    }
    const QDir bundleRoot(QDir(stateRoot).filePath(QStringLiteral("source-bundle")));
    const QString generationPointer = current.filePath(QStringLiteral(".thoughts-source-artifacts/%1.json").arg(target));
    const QString pointerPath = QFileInfo(generationPointer).isFile() ? generationPointer : bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(target));
    QJsonObject pointer;
    return boundedJsonFile(pointerPath, &pointer, error) && pointer == documentSourceArtifactPointer(publication) && cachedDocumentSourceArtifactMatches(bundleRoot, publication);
}

LibraryActivationInspection inspectCompletedLibraryBuildActivations(const QString &docsRoot, const QString &stateRoot, const QJsonObject &runs)
{
    LibraryActivationInspection result;
    const QDir current(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    QJsonObject manifest;
    QJsonObject index;
    if (!boundedJsonFile(current.filePath(QStringLiteral(".thoughts-generation.json")), &manifest, &result.error) || !boundedJsonFile(current.filePath(QStringLiteral(".thoughts-library-index.json")), &index, &result.error)) {
        return result;
    }
    const QJsonArray activePublications = manifest.value(QStringLiteral("publications")).toArray();
    for (auto iterator = runs.begin(); iterator != runs.end(); ++iterator) {
        QJsonObject run = iterator.value().toObject();
        const QString status = run.value(QStringLiteral("status")).toString();
        if (status != QLatin1String("activating") && status != QLatin1String("activation_failed") && status != QLatin1String("interrupted")) {
            continue;
        }
        QJsonObject publication = run.value(QStringLiteral("publication")).toObject();
        if (publication.isEmpty() && status == QLatin1String("activating")) {
            const QJsonObject build = run.value(QStringLiteral("build")).toObject();
            QJsonObject activePublication;
            for (const QJsonValue &value : activePublications) {
                if (value.toObject().value(QStringLiteral("filename")).toString() == iterator.key()) {
                    activePublication = value.toObject();
                    break;
                }
            }
            const QJsonArray outcomes = build.value(QStringLiteral("result")).toObject().value(QStringLiteral("outcomes")).toArray();
            const bool outcomeMatches = std::any_of(outcomes.cbegin(), outcomes.cend(), [&](const QJsonValue &value) {
                const QJsonObject outcome = value.toObject();
                const QString outcomeStatus = outcome.value(QStringLiteral("status")).toString();
                return outcome.value(QStringLiteral("filename")).toString() == iterator.key() && outcome.value(QStringLiteral("sha256")).toString() == activePublication.value(QStringLiteral("sha256")).toString() &&
                    (outcomeStatus == QLatin1String("published") || outcomeStatus == QLatin1String("unchanged"));
            });
            if (build.value(QStringLiteral("status")).toString() == QLatin1String("succeeded") && outcomeMatches) {
                publication = activePublication;
                run.insert(QStringLiteral("publication"), publication);
            }
        }
        QString publicationError;
        if (boundedLocalPublicationIsCurrent(docsRoot, stateRoot, iterator.key(), publication, index, manifest, &publicationError)) {
            result.completedRuns.insert(iterator.key(), run);
        } else if (!publicationError.isEmpty() && result.error.isEmpty()) {
            result.error = publicationError;
        }
    }
    return result;
}

bool appendLogFile(const QString &path, const QByteArray &contents)
{
    if (contents.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        return contents.isEmpty();
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(contents) == contents.size() && file.flush();
}

bool writeSubmissionStatus(const QString &directory, const QString &state, const QString &message = QString())
{
    const QJsonObject status {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("state"), state},
        {QStringLiteral("message"), message},
    };
    const QString path = QDir(directory).filePath(QStringLiteral("submission-status.json"));
    const bool written = writeJsonFile(path, QJsonDocument(status).toJson(QJsonDocument::Compact) + '\n');
    if (written) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return written;
}

bool linkOrCopyFile(const QString &source, const QString &destination)
{
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        return false;
    }
    QFile::remove(destination);
    // An APFS clone or a hard link shares the unchanged bytes with the
    // previous generation instead of storing another full copy of every PDF
    // for each revision.
    if (Thoughts::Platform::cloneOrLinkFile(source, destination)) {
        return true;
    }
    return QFile::copy(source, destination);
}

bool preserveMacDocumentAccess(const QString &source, const QString &destination)
{
#ifdef Q_OS_MACOS
    constexpr auto attribute = "com.apple.macl";
    const QByteArray encodedSource = QFile::encodeName(source);
    const QByteArray encodedDestination = QFile::encodeName(destination);
    const ssize_t size = ::getxattr(encodedSource.constData(), attribute, nullptr, 0, 0, 0);
    if (size < 0) {
        return errno == ENOATTR || errno == ENOENT;
    }
    QByteArray value(size, Qt::Uninitialized);
    if (::getxattr(encodedSource.constData(), attribute, value.data(), value.size(), 0, 0) != size) {
        return false;
    }
    return ::setxattr(encodedDestination.constData(), attribute, value.constData(), value.size(), 0, 0) == 0;
#else
    Q_UNUSED(source)
    Q_UNUSED(destination)
    return true;
#endif
}

bool swapDirectories(const QString &current, const QString &staging)
{
#ifdef Q_OS_MACOS
    const QByteArray encodedCurrent = QFile::encodeName(current);
    const QByteArray encodedStaging = QFile::encodeName(staging);
    return renamex_np(encodedCurrent.constData(), encodedStaging.constData(), RENAME_SWAP) == 0;
#else
    // A new Library has no generation yet: its first one becomes current.
    if (!QFileInfo::exists(current) && !QFileInfo(current).isSymLink()) {
        return QDir().rename(staging, current);
    }
    const QString backup = current + QStringLiteral(".activation-backup");
    if (QFileInfo::exists(backup) && !QDir(backup).removeRecursively()) {
        return false;
    }
    if (!QDir().rename(current, backup)) {
        return false;
    }
    if (!QDir().rename(staging, current)) {
        QDir().rename(backup, current);
        return false;
    }
    QDir(backup).removeRecursively();
    return true;
#endif
}


bool setGenerationReadOnly(const QString &directory)
{
    QStringList directories;
    QDirIterator iterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo info(iterator.next());
        if (Thoughts::Platform::isDirectoryLink(info)) {
            return false;
        }
        if (info.isDir()) {
            directories.append(info.absoluteFilePath());
            continue;
        }
        if (!QFile::setPermissions(info.absoluteFilePath(), QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
            return false;
        }
    }
    std::sort(directories.begin(), directories.end(), [](const QString &left, const QString &right) { return left.size() > right.size(); });
    for (const QString &path : std::as_const(directories)) {
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
            return false;
        }
    }
    return QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
}

bool removeReadOnlyGeneration(const QString &directory)
{
    QStringList directories {directory};
    QDirIterator iterator(directory, QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        directories.append(iterator.next());
    }
    for (const QString &path : std::as_const(directories)) {
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
            return false;
        }
    }
    return QDir(directory).removeRecursively();
}

bool externallyOpenGenerationDirectories(const QString &docsRoot, QSet<QString> *result)
{
    if (!result) {
        return false;
    }
    result->clear();
#ifdef Q_OS_MACOS
    QProcess process;
    process.start(QStringLiteral("/usr/sbin/lsof"), {QStringLiteral("-Fn")});
    // A cold system-wide lsof can take several seconds; cleanup runs off the UI thread.
    if (!process.waitForStarted(1000) || !process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    const QString canonicalDocsRoot = QFileInfo(docsRoot).canonicalFilePath();
    if (canonicalDocsRoot.isEmpty()) {
        return false;
    }
    const QString prefix = QDir(canonicalDocsRoot).filePath(QStringLiteral(".thoughts-library-stage-"));
    static const QRegularExpression suffix(QStringLiteral("^[0-9a-f]{64}(?:/|$)"));
    for (const QByteArray &rawLine : process.readAllStandardOutput().split('\n')) {
        if (!rawLine.startsWith('n')) {
            continue;
        }
        const QString path = QString::fromUtf8(rawLine.mid(1));
        if (!path.startsWith(prefix)) {
            continue;
        }
        const QString remainder = path.mid(prefix.size());
        const QRegularExpressionMatch match = suffix.match(remainder);
        if (match.hasMatch()) {
            result->insert(QDir::cleanPath(QDir(docsRoot).filePath(QStringLiteral(".thoughts-library-stage-") + remainder.left(64))));
        }
    }
#elif defined(Q_OS_WIN)
    // Windows cannot delete a file another process has open, such as a PDF
    // still shown from an older generation, so keep those generations.
    for (const QFileInfo &info : QDir(docsRoot).entryInfoList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (Thoughts::Platform::directoryHasFilesInUse(info.absoluteFilePath())) {
            result->insert(QDir::cleanPath(info.absoluteFilePath()));
        }
    }
#else
    Q_UNUSED(docsRoot)
#endif
    return true;
}

// What kind of failure a failed request is, for whoever decides about trying again:
// "transient" (no answer, a timeout, a gateway or availability error: asking again may
// work), "needs_user" (the server refuses this computer or this app until the user acts)
// or "bug" (the server refused the request or failed on it; asking again changes nothing).
QString replyFailureKind(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0 || status == 408 || status == 429 || status == 502 || status == 503 || status == 504) {
        return QStringLiteral("transient");
    }
    if (status == 401 || status == 403 || status == 426) {
        return QStringLiteral("needs_user");
    }
    return QStringLiteral("bug");
}

bool transientReplyFailure(QNetworkReply *reply)
{
    return reply->error() != QNetworkReply::NoError && replyFailureKind(reply) == QLatin1String("transient");
}

// The same from the text a failed publisher or installer process left on stderr.
QString processFailureKind(const QString &detail)
{
    static const QRegularExpression transient(QStringLiteral("urlopen error|timed out|HTTP Error (?:408|429|502|503|504)\\b|Remote end closed|Connection refused|Temporary failure"), QRegularExpression::CaseInsensitiveOption);
    if (detail.contains(QLatin1String("source conflict was preserved")) || detail.contains(QLatin1String("HTTP Error 401")) || detail.contains(QLatin1String("HTTP Error 403"))) {
        return QStringLiteral("needs_user");
    }
    return transient.match(detail).hasMatch() ? QStringLiteral("transient") : QStringLiteral("bug");
}

// The kind of the request failure that was read last, for the failManualSync that follows
// it in the same handler.
struct ReplyFailureNote {
    QString kind;
    QElapsedTimer age;
};
ReplyFailureNote &lastReplyFailure()
{
    static ReplyFailureNote note;
    return note;
}

void noteReplyFailure(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        lastReplyFailure().kind = replyFailureKind(reply);
        lastReplyFailure().age.start();
    }
}

Thoughts::SyncErrorInfo replyErrorInfo(QNetworkReply *reply, const QList<QJsonObject> &operations = {})
{
    noteReplyFailure(reply);
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return Thoughts::SyncClient::parseErrorResponse(body, reply->errorString(), status, operations);
}

QString replyError(QNetworkReply *reply)
{
    return replyErrorInfo(reply).summary;
}

}

namespace Okular
{

QByteArray Internal::decodeZstdPublicationDelta(const QByteArray &base, const QByteArray &patch, qint64 expectedSize)
{
#ifdef THOUGHTS_HAVE_ZSTD
    if (base.isEmpty() || patch.isEmpty() || expectedSize <= 0 || expectedSize > std::numeric_limits<int>::max()) {
        return {};
    }
    QByteArray result(static_cast<qsizetype>(expectedSize), Qt::Uninitialized);
    ZSTD_DCtx *const context = ZSTD_createDCtx();
    if (!context) {
        return {};
    }
    const size_t decoded = ZSTD_decompress_usingDict(context,
                                                     result.data(),
                                                     static_cast<size_t>(result.size()),
                                                     patch.constData(),
                                                     static_cast<size_t>(patch.size()),
                                                     base.constData(),
                                                     static_cast<size_t>(base.size()));
    ZSTD_freeDCtx(context);
    return !ZSTD_isError(decoded) && decoded == static_cast<size_t>(expectedSize) ? result : QByteArray();
#else
    Q_UNUSED(base)
    Q_UNUSED(patch)
    Q_UNUSED(expectedSize)
    return {};
#endif
}

QByteArray Internal::reconstructVerifiedPdfDelta(const QString &docsRoot,
                                                 const QString &filename,
                                                 const QString &baseSha256,
                                                 const QByteArray &patch,
                                                 const QString &resultSha256,
                                                 qint64 resultSize)
{
    const QByteArray base = verifiedActivePdfBase(docsRoot, filename, baseSha256);
    const QByteArray result = decodeZstdPublicationDelta(base, patch, resultSize);
    return result.size() == resultSize && validSha256(resultSha256) && Thoughts::SyncClient::verifyObject(result, resultSha256.toLatin1()) ? result : QByteArray();
}

bool Internal::readerPdfDeltaPlanIsUsable(const QJsonObject &plan, const QJsonObject &publication)
{
    return pdfDeltaPlanForPublication(plan, publication).has_value();
}

Thoughts::FrozenPublicationBundle Internal::discoverProjectTargetFrozenPublication(const QString &executable,
                                                                                   const QString &docsRoot,
                                                                                   const QString &stateRoot,
                                                                                   const QString &manifestPath,
                                                                                   const QString &project,
                                                                                   const QString &target,
                                                                                   const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                                   const QList<Thoughts::DesktopTrackedPath> &baselines)
{
    return discoverProjectTargetFrozenPublicationImpl(executable, docsRoot, stateRoot, manifestPath, project, target, snapshot, baselines);
}

QJsonObject Internal::publicationIdentityForBuild(const QString &target, const QJsonObject &build, const QJsonObject &currentPublication)
{
    return ::publicationIdentityForBuild(target, build, currentPublication);
}

QString Internal::operationTraceIdForBuildRun(const QJsonObject &run, const QString &fallback)
{
    const QString traceId = run.value(QStringLiteral("trace_id")).toString();
    return traceId.isEmpty() ? fallback : traceId;
}

QString Internal::authoritativeBuildStatus(const QJsonObject &build)
{
    return build.value(QStringLiteral("status")).toString();
}

bool Internal::sourceManifestIsProjectScoped(const QByteArray &contents)
{
    return sourceManifestIsProjectScopedImpl(contents);
}

bool Internal::sourceManifestMissesDefaultFolderSources(const QString &docsRoot, const QString &manifestPath, const QString &onlyOwner)
{
    return manifestMissesDefaultFolderSources(docsRoot, manifestPath, onlyOwner);
}

bool Internal::pendingProjectFileDeclaresUnbuiltDefaultFolderDocument(const QString &docsRoot,
                                                                     const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                     const QSet<QString> &publishedFilenames)
{
    static const QRegularExpression projectFile(QStringLiteral("^\\.thoughts/projects/[0-9a-f]{64}/(default|documents)/thoughts-project\\.json$"));
    for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
        if (!projectFile.match(file.path).hasMatch()) continue;
        const QJsonObject configuration = Thoughts::ProjectConfiguration::read(QDir(docsRoot).filePath(file.path));
        const QString owner = file.path.section(QLatin1Char('/'), 0, 3);
        const QString defaultFolder = configuration.value(QStringLiteral("default_folder_id")).toString();
        const QJsonObject rendering = configuration.value(QStringLiteral("rendering")).toObject();
        for (const QJsonValue &value : configuration.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = value.toObject();
            const QString format = document.value(QStringLiteral("format")).toString();
            const QJsonObject source = document.value(QStringLiteral("source")).toObject();
            if (!document.value(QStringLiteral("included")).toBool() || (format != QLatin1String("latex") && format != QLatin1String("markdown"))
                || defaultFolder.isEmpty() || source.value(QStringLiteral("folder_id")).toString() != defaultFolder) {
                continue;
            }
            QString output = rendering.value(document.value(QStringLiteral("document_id")).toString()).toObject().value(QStringLiteral("output")).toString();
            if (output.isEmpty()) output = Thoughts::SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + source.value(QStringLiteral("path")).toString());
            if (!publishedFilenames.contains(output)) return true;
        }
    }
    return false;
}

Internal::SourcePublicationDecision Internal::sourcePublicationDecision(bool trackerAvailable,
                                                                        bool trackerAuthoritative,
                                                                        bool manifestMatches,
                                                                        Thoughts::DesktopSyncStatus status,
                                                                        const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                        const QString &requestedTarget,
                                                                        const QHash<QString, QString> &knownProjectsByTarget)
{
    SourcePublicationDecision decision;
    // Fedora accepts a project-scoped publication only for that project's own
    // files. A pending file used by several projects needs a global one.
    const auto ownedBy = [&knownProjectsByTarget](const Thoughts::DesktopPendingFile &file, const QString &owner) {
        return !file.affectedTargets.isEmpty() && std::all_of(file.affectedTargets.cbegin(), file.affectedTargets.cend(), [&](const QString &target) {
            return knownProjectsByTarget.value(target) == owner;
        });
    };
    if (!requestedTarget.isEmpty() && safePdfFilename(requestedTarget)
        && safeSourceProject(knownProjectsByTarget.value(requestedTarget))) {
        const QString owner = knownProjectsByTarget.value(requestedTarget);
        const bool affected = std::any_of(snapshot.files.cbegin(), snapshot.files.cend(), [&](const Thoughts::DesktopPendingFile &file) {
            if (file.path.startsWith(QLatin1String(".thoughts/projects/"))) return ownedBy(file, owner);
            return file.path.startsWith(owner + QLatin1Char('/')) || file.affectedTargets.contains(requestedTarget);
        });
        if (!affected) return decision;
        decision.mode = SourcePublicationMode::ProjectScoped;
        decision.targets = {requestedTarget};
        decision.projectsByTarget.insert(requestedTarget, owner);
        // A pending project build input (e.g. tex/assets/*.pdf) is shared by
        // the project's other PDFs. Publishing it for the requested PDF alone
        // leaves the siblings' accepted inventories declaring the old bytes,
        // and Fedora's next project build then fails closure verification.
        for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
            if (!file.affectedTargets.contains(requestedTarget)) continue;
            const QJsonObject metadata = file.hasTrackedState ? file.trackedState.metadata : file.metadata;
            const QString role = metadata.value(QStringLiteral("role")).toString();
            if (metadata.value(QStringLiteral("source_kind")).toString() != QLatin1String("build_input")
                || (role != QLatin1String("asset") && role != QLatin1String("generated") && role != QLatin1String("external_pdf"))) {
                continue;
            }
            for (const QString &target : file.affectedTargets) {
                if (safePdfFilename(target) && knownProjectsByTarget.value(target) == owner && !decision.targets.contains(target)) {
                    decision.targets.append(target);
                    decision.projectsByTarget.insert(target, owner);
                }
            }
        }
        if (decision.targets.size() > 1) {
            std::sort(decision.targets.begin() + 1, decision.targets.end(), [](const QString &left, const QString &right) {
                const int folded = left.compare(right, Qt::CaseInsensitive);
                return folded == 0 ? left < right : folded < 0;
            });
        }
        return decision;
    }
    if (!trackerAvailable || !manifestMatches || status == Thoughts::DesktopSyncStatus::Unknown) {
        decision.mode = SourcePublicationMode::Global;
        return decision;
    }
    if (status == Thoughts::DesktopSyncStatus::Clean) {
        // A clean queue is only conclusive while every tracked path is still
        // covered.  If watcher/reconciliation authority was lost without a
        // path-specific pending row, there is no safe project assignment and
        // the complete inventory must be rediscovered.
        if (!trackerAuthoritative) {
            decision.mode = SourcePublicationMode::Global;
        }
        return decision;
    }
    if (snapshot.files.isEmpty()) {
        decision.mode = SourcePublicationMode::Global;
        return decision;
    }

    QSet<QString> affected;
    bool requiresProjectInspection = !trackerAuthoritative;
    for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
        if (file.affectedTargets.isEmpty() || std::any_of(file.affectedTargets.cbegin(), file.affectedTargets.cend(), [](const QString &target) { return !safePdfFilename(target); })) {
            decision.mode = SourcePublicationMode::Global;
            return decision;
        }
        const QJsonObject metadata = file.hasTrackedState ? file.trackedState.metadata : file.metadata;
        const QString sourceKind = metadata.value(QStringLiteral("source_kind")).toString();
        const bool unresolved = metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved");
        requiresProjectInspection = requiresProjectInspection || sourceKind == QLatin1String("source") || unresolved;
        affected.unite(QSet<QString>(file.affectedTargets.cbegin(), file.affectedTargets.cend()));
    }

    if (!requestedTarget.isEmpty()) {
        if (!safePdfFilename(requestedTarget)) {
            decision.mode = SourcePublicationMode::Global;
        } else if (affected.contains(requestedTarget)) {
            decision.targets = {requestedTarget};
        }
    } else {
        decision.targets = affected.values();
        std::sort(decision.targets.begin(), decision.targets.end(), [](const QString &left, const QString &right) {
            const int folded = left.compare(right, Qt::CaseInsensitive);
            return folded == 0 ? left < right : folded < 0;
        });
    }
    if (decision.targets.isEmpty()) {
        return decision;
    }
    if (!requiresProjectInspection) {
        decision.mode = SourcePublicationMode::Scoped;
        return decision;
    }
    for (const QString &target : std::as_const(decision.targets)) {
        const QString project = knownProjectsByTarget.value(target);
        // Project-scoped frozen publications serve docs-* folders only; a
        // default folder's documents are published globally (spec Rule 5).
        if (!safeSourceProject(project)) {
            decision.targets.clear();
            decision.projectsByTarget.clear();
            decision.mode = SourcePublicationMode::Global;
            return decision;
        }
        decision.projectsByTarget.insert(target, project);
    }
    for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
        const bool selected = std::any_of(file.affectedTargets.cbegin(), file.affectedTargets.cend(), [&](const QString &target) { return decision.targets.contains(target); });
        if (selected && !ownedBy(file, knownProjectsByTarget.value(file.affectedTargets.constFirst()))) {
            decision.targets.clear();
            decision.projectsByTarget.clear();
            decision.mode = SourcePublicationMode::Global;
            return decision;
        }
    }
    decision.mode = SourcePublicationMode::ProjectScoped;
    return decision;
}

Internal::SourcePublicationDecision Internal::sourcePublicationWithoutBlockedTargets(SourcePublicationDecision decision,
                                                                                     const Thoughts::DesktopSyncSnapshot &snapshot,
                                                                                     const QSet<QString> &blockedTargets,
                                                                                     const QHash<QString, QString> &knownProjectsByTarget,
                                                                                     const QString &requestedTarget)
{
    if (blockedTargets.isEmpty() || decision.mode == SourcePublicationMode::None) {
        return decision;
    }
    if (decision.mode == SourcePublicationMode::Global) {
        // Global discovery cannot publish conflicted sources. Continue only
        // known frozen changes; unchanged projects have nothing to stage.
        // Keep the reconciliation obligation and all deferred pending paths.
        QSet<QString> pendingTargets;
        for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
            for (const QString &target : file.affectedTargets) {
                if (safePdfFilename(target) && safeSourceProject(knownProjectsByTarget.value(target)) && (requestedTarget.isEmpty() || requestedTarget == target)) {
                    pendingTargets.insert(target);
                }
            }
        }
        decision.targets = pendingTargets.values();
        std::sort(decision.targets.begin(), decision.targets.end());
        decision.projectsByTarget.clear();
        for (const QString &target : std::as_const(decision.targets)) {
            decision.projectsByTarget.insert(target, knownProjectsByTarget.value(target));
        }
        decision.selectedPaths.clear();
        decision.mode = SourcePublicationMode::ProjectScoped;
    }
    decision.targets.removeIf([&blockedTargets](const QString &target) { return blockedTargets.contains(target); });
    for (const QString &target : blockedTargets) {
        decision.projectsByTarget.remove(target);
    }
    if (decision.targets.isEmpty()) {
        decision = {};
    }
    return decision;
}

Thoughts::DesktopSyncSnapshot Internal::selectedSourcePublicationSnapshot(const Thoughts::DesktopSyncSnapshot &snapshot, const SourcePublicationDecision &decision)
{
    if (decision.mode != SourcePublicationMode::Global || decision.selectedPaths.isEmpty()) {
        return snapshot;
    }
    const QSet<QString> selected(decision.selectedPaths.cbegin(), decision.selectedPaths.cend());
    Thoughts::DesktopSyncSnapshot filtered;
    filtered.throughGeneration = snapshot.throughGeneration;
    for (const Thoughts::DesktopPendingFile &file : snapshot.files) {
        const QString path = QDir::cleanPath(file.path.isEmpty() && file.hasTrackedState ? file.trackedState.path : file.path);
        if (selected.contains(path)) {
            filtered.files.append(file);
        }
    }
    return filtered;
}

QList<Thoughts::DesktopTrackedPath> Internal::acceptedDesktopSyncBaselinesForScope(const QString &docsRoot,
                                                                                   const QString &manifestPath,
                                                                                   const QString &sourceStatePath,
                                                                                   const QJsonObject &publication,
                                                                                   const QString &scopeTarget,
                                                                                   const QList<Thoughts::DesktopTrackedPath> &previousBaselines,
                                                                                   QString *error)
{
    return ::acceptedDesktopSyncBaselines(docsRoot, manifestPath, sourceStatePath, publication, scopeTarget.isEmpty() ? QStringList() : QStringList {scopeTarget}, previousBaselines, error);
}

bool Internal::verifyReaderObjectWithCache(Thoughts::DesktopSyncStore *store,
                                           const QString &path,
                                           const QString &sha256,
                                           qint64 expectedSize,
                                           const QString &kind,
                                           const QString &semanticRole,
                                           const QString &owningPublication,
                                           bool *contentRead,
                                           QString *error)
{
    if (contentRead) {
        *contentRead = false;
    }
    if (error) {
        error->clear();
    }
    const QJsonObject metadata = readerFileMetadata(path);
    if (!validSha256(sha256) || expectedSize < 0 || kind.trimmed().isEmpty() || metadata.isEmpty() || metadata.value(QStringLiteral("size_bytes")).toInteger(-1) != expectedSize) {
        return false;
    }
    if (store) {
        QString storeError;
        const std::optional<Thoughts::DesktopVerifiedObject> cached = store->verifiedObject(sha256, &storeError);
        if (!storeError.isEmpty()) {
            if (error) {
                *error = storeError;
            }
            return false;
        }
        if (cached && cached->sizeBytes == expectedSize && sameReaderFileMetadata(cached->metadata, metadata)) {
            return true;
        }
    }
    if (contentRead) {
        *contentRead = true;
    }
    if (fileSha256(path) != sha256.toLatin1()) {
        return false;
    }
    if (!store) {
        return true;
    }
    Thoughts::DesktopVerifiedObject verified;
    verified.sha256 = sha256;
    verified.sizeBytes = expectedSize;
    verified.kind = kind;
    verified.semanticRole = semanticRole;
    verified.owningPublication = owningPublication;
    verified.verifiedAtMs = QDateTime::currentMSecsSinceEpoch();
    verified.metadata = metadata;
    return store->upsertVerifiedObject(verified, error);
}

QList<QJsonObject> Internal::takeSourcePublisherEvents(QByteArray &buffer, const QByteArray &incoming, bool flush)
{
    buffer.append(incoming);
    QList<QJsonObject> events;
    while (true) {
        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0 && !flush) {
            break;
        }
        const QByteArray line = newline < 0 ? buffer : buffer.left(newline);
        if (newline < 0) {
            buffer.clear();
        } else {
            buffer.remove(0, newline + 1);
        }
        const QJsonObject event = parseSourcePublisherEvent(line);
        if (!event.isEmpty()) {
            events.append(event);
        }
        if (newline < 0) {
            break;
        }
    }
    return events;
}

void FedoraRepository::SyncCoordinator::begin(const QString &filename, bool checkRenderedInputs, const QString &traceId)
{
    operationId = traceId;
    message.clear();
    details.clear();
    errorCode.clear();
    failureKind.clear();
    buildFailure.clear();
    warningCode.clear();
    warning.clear();
    buildFailureDocuments = {};
    pendingBuildDocuments.clear();
    // The operation trace identifies the Desktop journal.  The render request
    // has a separate idempotency identity and must never double as a trace
    // owned by another Fedora device.
    renderCheckRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sourcePublicationTraceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    serverStatePurpose = ServerStatePurpose::Initial;
    libraryRefreshState = LibraryRefreshState::NotRequested;
    postLocalWorkAction = PostLocalWorkAction::ConditionalRenderCheck;
    libraryLatestServerSequence = -1;
    remoteLatestServerSequence = -1;
    authoringCheckedSourceFrontier = -1;
    blockedAuthoringPaths.clear();
    pendingAuthoringPaths.clear();
    providerErrors.clear();
    publishedSourceFrontier = -1;
    renderCheckedSourceFrontier = -1;
    discoveryReentries = 0;
    serverState = {};
    sourceSnapshot.reset();
    frozenPublication.reset();
    sourcePublicationMode = Internal::SourcePublicationMode::None;
    sourcePublicationTargets.clear();
    sourcePublicationProjectsByTarget.clear();
    sourcePublicationSelectedPaths.clear();
    completedSourcePublicationTargets.clear();
    sourcePublicationBatch.clear();
    sourcePublicationClock.invalidate();
    sourcePublicationRuns = 0;
    sourcePublicationDocuments = 0;
    requiresGlobalRenderCheck = false;
    localReconciliationStartedAtMs = -1;
    localFullReconciliationStarted = false;
    proposalClaimsChecked = false;
    setTarget(filename);
    conditionalRenderCheck = checkRenderedInputs;
    phase = ManualSyncPhase::ServerState;
    state = ManualSyncState::Running;
    Q_ASSERT(lifecycleIsValid());
}

void FedoraRepository::SyncCoordinator::fail(const QString &failureMessage, const QString &failureDetails)
{
    phase = ManualSyncPhase::None;
    state = ManualSyncState::Failed;
    message = failureMessage.isEmpty() ? QStringLiteral("Synchronization failed.") : failureMessage;
    details = failureDetails;
    errorCode = QStringLiteral("sync_failed");
    if (!buildFailure.isEmpty() && !details.contains(buildFailure)) {
        details += QStringLiteral("\nPDF build also failed: ") + buildFailure;
    }
    Q_ASSERT(lifecycleIsValid());
}

void FedoraRepository::SyncCoordinator::succeed()
{
    phase = ManualSyncPhase::None;
    state = ManualSyncState::Succeeded;
    message.clear();
    details.clear();
    Q_ASSERT(lifecycleIsValid());
}

FedoraRepository::SyncCoordinator::SyncCoordinator() = default;
FedoraRepository::SyncCoordinator::~SyncCoordinator() = default;
FedoraRepository::SyncCoordinator::SyncCoordinator(SyncCoordinator &&) noexcept = default;
FedoraRepository::SyncCoordinator &FedoraRepository::SyncCoordinator::operator=(SyncCoordinator &&) noexcept = default;

void FedoraRepository::SyncCoordinator::reset()
{
    begin(QString(), true, QString());
    phase = ManualSyncPhase::None;
    state = ManualSyncState::Idle;
    renderCheckRequestId.clear();
    sourcePublicationTraceId.clear();
    Q_ASSERT(lifecycleIsValid());
}

void FedoraRepository::SyncCoordinator::setSourcePublication(const Internal::SourcePublicationDecision &decision)
{
    sourcePublicationMode = decision.mode;
    sourcePublicationTargets = decision.targets;
    sourcePublicationProjectsByTarget = decision.projectsByTarget;
    sourcePublicationSelectedPaths = decision.selectedPaths;
    // Sibling PDFs published with a shared build input must be rebuilt too.
    requiresGlobalRenderCheck = requiresGlobalRenderCheck
        || (isDocument() && (decision.mode == Internal::SourcePublicationMode::Global || decision.targets.size() > 1));
    completedSourcePublicationTargets.clear();
    sourcePublicationBatch.clear();
}

void FedoraRepository::SyncCoordinator::promoteSourcePublicationToGlobal()
{
    requiresGlobalRenderCheck = requiresGlobalRenderCheck || isDocument();
    sourcePublicationMode = Internal::SourcePublicationMode::Global;
    sourcePublicationTargets.clear();
    sourcePublicationProjectsByTarget.clear();
    sourcePublicationSelectedPaths.clear();
    completedSourcePublicationTargets.clear();
    sourcePublicationBatch.clear();
    frozenPublication.reset();
}

void FedoraRepository::SyncCoordinator::completeSourcePublication(const QString &publishedTarget)
{
    if (publishedTarget.isEmpty()) {
        sourcePublicationTargets.clear();
        sourcePublicationProjectsByTarget.clear();
    } else {
        sourcePublicationProjectsByTarget.remove(publishedTarget);
        if (!sourcePublicationTargets.isEmpty() && sourcePublicationTargets.constFirst() == publishedTarget) {
            sourcePublicationTargets.removeFirst();
        } else {
            sourcePublicationTargets.removeAll(publishedTarget);
        }
    }
    sourcePublicationBatch.removeAll(publishedTarget);
    if (sourcePublicationTargets.isEmpty()) {
        sourcePublicationMode = Internal::SourcePublicationMode::None;
        sourcePublicationSelectedPaths.clear();
        completedSourcePublicationTargets.clear();
        sourcePublicationBatch.clear();
    }
}

namespace
{
// Rule R of the default-folder rename on an in-memory JSON copy.
QJsonObject rewriteLegacyDefaultFolders(const QJsonObject &value)
{
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    const QByteArray rewritten = Thoughts::SourceOwner::rewriteLegacyDefaultFolder(bytes);
    return rewritten == bytes ? value : QJsonDocument::fromJson(rewritten).object();
}

QJsonArray rewriteLegacyDefaultFolders(const QJsonArray &value)
{
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    const QByteArray rewritten = Thoughts::SourceOwner::rewriteLegacyDefaultFolder(bytes);
    return rewritten == bytes ? value : QJsonDocument::fromJson(rewritten).array();
}

Thoughts::DefaultFolderMigration::Paths defaultFolderMigrationPaths(const QString &docsRoot, const QString &stateRoot, const QString &sourceManifestPath)
{
    return {docsRoot, stateRoot, Thoughts::Platform::applicationDataRoot(), QFileInfo(sourceManifestPath).absolutePath()};
}

Thoughts::SourceFolderMigration::Paths sourceFolderMigrationPaths(const QString &docsRoot, const QString &stateRoot, const QString &sourceManifestPath)
{
    return {docsRoot, stateRoot, Thoughts::Platform::applicationDataRoot(), QFileInfo(sourceManifestPath).absolutePath()};
}

}

FedoraRepository *FedoraRepository::instance(const QString &docsRoot)
{
    static QPointer<FedoraRepository> repository;
    if (!repository) {
        repository = new FedoraRepository(docsRoot, QCoreApplication::instance());
    }
    return repository;
}

FedoraRepository::FedoraRepository(QString docsRoot, QObject *parent)
    : QObject(parent)
    , m_docsRoot(std::move(docsRoot))
    , m_stateRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server")))
    , m_sourceManifestPath(QDir(Thoughts::Platform::thoughtsDataRoot()).filePath(QStringLiteral("source-manifest.json")))
    , m_outboxPath(QDir(m_stateRoot).filePath(QStringLiteral("outbox.json")))
    , m_outbox(std::make_unique<Thoughts::DesktopSyncOutbox>(QDir(m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite")), m_outboxPath))
    , m_syncTransferLedger(std::make_unique<Thoughts::SyncTransferLedger>())
{
    QString outboxDetails;
    ensureOutboxReady(&outboxDetails);
    m_pendingChangesTimer = new QTimer(this);
    m_pendingChangesTimer->setSingleShot(true);
    m_pendingChangesTimer->setInterval(100);
    connect(m_pendingChangesTimer, &QTimer::timeout, this, &FedoraRepository::refreshPendingChanges);
    // Only Thoughts writes the sync databases, so their commits arrive as
    // in-process announcements. Files other processes can replace (the Library
    // project list, the published source index, the accepted source manifest
    // written by the publisher child) come from the shared recursive streams.
    Thoughts::connectDesktopSyncStoreChanges(this, SLOT(handleDesktopSyncStoreChanged(QString)));
    Thoughts::DocsChangeMonitor *docsMonitor = Thoughts::DocsChangeMonitor::forRoot(m_docsRoot);
    connect(docsMonitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, [this](const QStringList &paths) {
        if (std::any_of(paths.cbegin(), paths.cend(), [this](const QString &path) { return affectsPendingChanges(path); })) {
            schedulePendingChangesRefresh();
        }
    });
    connect(docsMonitor, &Thoughts::DocsChangeMonitor::rescanRequired, this, &FedoraRepository::schedulePendingChangesRefresh);
    Thoughts::DocsChangeMonitor *manifestMonitor = Thoughts::DocsChangeMonitor::forRoot(QFileInfo(m_sourceManifestPath).absolutePath());
    connect(manifestMonitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, [this](const QStringList &paths) {
        if (paths.contains(QDir::cleanPath(m_sourceManifestPath))) {
            schedulePendingChangesRefresh();
        }
    });
    m_sourceCommentTimer = new QTimer(this);
    m_sourceCommentTimer->setSingleShot(true);
    m_sourceCommentTimer->setInterval(150);
    connect(m_sourceCommentTimer, &QTimer::timeout, this, &FedoraRepository::refreshSourceComments);
    QFile savedComments(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-snapshot.json")));
    if (savedComments.open(QIODevice::ReadOnly)) {
        const auto saved = QJsonDocument::fromJson(savedComments.readAll()).object();
        if (saved.value(QStringLiteral("docs_root")).toString() == m_docsRoot) {
            m_sourceCommentSnapshot = saved.value(QStringLiteral("entries")).toObject();
            m_sourceCommentFileIndex = saved.value(QStringLiteral("files")).toObject();
        }
    }
    m_pendingChangesSnapshot = calculatePendingChanges();
    // Parts and the Shell read the current projections when they start, so
    // the first pending refresh only broadcasts reviews that change after this.
    m_reviewProjections = reviewProjections(effectiveProjections());
    m_reviewProjectionsKnown = true;
    scheduleSourceCommentRefresh();
    connect(this, &FedoraRepository::libraryChanged, this, [this] {
        completeLibraryBuildActivationIfReady();
        if (m_syncCoordinator.running() && m_syncCoordinator.phase == ManualSyncPhase::Library) {
            m_syncCoordinator.libraryRefreshState = SyncCoordinator::LibraryRefreshState::Ready;
            scheduleManualSyncAdvance();
        }
    });
    connect(this, &FedoraRepository::libraryGenerationActivated, this, [this](const QStringList &, bool success) {
        if (success && !m_pendingLibraryBuildActivationTarget.isEmpty()) {
            completeLibraryBuildActivationIfReady();
        } else if (!success && !m_pendingLibraryBuildActivationTarget.isEmpty()) {
            failTargetedBuildActivation(QStringLiteral("The verified Library generation from the server could not be activated locally."));
        }
    });
    connect(this, &FedoraRepository::connectionStateChanged, this, [this](bool online, const QString &message) {
        if (!online && m_syncCoordinator.running() && m_syncCoordinator.phase == ManualSyncPhase::Library) {
            failManualSync(message.isEmpty() ? QStringLiteral("The Library could not be synchronized.") : message);
        }
        if (!online && !m_pendingLibraryBuildActivationTarget.isEmpty()) {
            failTargetedBuildActivation(message.isEmpty() ? QStringLiteral("The Library could not be synchronized.") : message);
        }
    });

    const QJsonObject state = readRepositoryState();
    const QJsonObject persistedRenderStatuses = state.value(QStringLiteral("render_statuses")).toObject();
    for (auto iterator = persistedRenderStatuses.begin(); iterator != persistedRenderStatuses.end(); ++iterator) {
        if (safePdfFilename(iterator.key()) && iterator.value().isObject()) {
            m_renderStatuses.insert(iterator.key(), iterator.value().toObject());
        }
    }
    m_renderState = state.value(QStringLiteral("render_state")).toObject();
    // Pruning runs only after a verified Library activation. Startup must not
    // open every active Library object: a temporarily unavailable File
    // Provider can block open(2), which would also prevent clean application
    // teardown while QCoreApplication waits for the global thread pool.
    QTimer::singleShot(0, this, &FedoraRepository::reconcileInterruptedWorkOnStartup);
    QTimer::singleShot(0, this, &FedoraRepository::schedulePendingChangesRefresh);
    QTimer::singleShot(0, this, &FedoraRepository::retireUnusedRepositoryState);
}

FedoraRepository::~FedoraRepository() = default;

void FedoraRepository::reconcileInterruptedWorkOnStartup()
{
    // Transient Fedora states are observations, not durable proof that work is
    // still active after this process restarts. Keep the saved details for
    // diagnostics, but require a fresh status response before presenting them
    // as live work. This set is deliberately not part of repository.json.
    m_restoredTransientRenderStatuses.clear();
    for (auto iterator = m_renderStatuses.begin(); iterator != m_renderStatuses.end(); ++iterator) {
        const QString state = iterator.value().value(QStringLiteral("status")).toString();
        if (state == QLatin1String("building") || state == QLatin1String("publishing") || state == QLatin1String("preparing")) {
            m_restoredTransientRenderStatuses.insert(iterator.key());
        }
    }

    QJsonObject state = readRepositoryState();
    const QJsonObject pendingGeneration = state.value(QStringLiteral("pending_library_generation")).toObject();
    if (!pendingGeneration.isEmpty()) {
        // Verification and any repair are performed by the existing
        // background activation path. Never open or hash Documents from the
        // UI thread during startup.
        QTimer::singleShot(0, this, &FedoraRepository::startPendingLibraryActivation);
    }

    // A default-folder rename interrupted by a crash or a quit finishes
    // before anything reads the half-renamed state.
    if (Thoughts::DefaultFolderMigration(defaultFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath)).interrupted()) {
        QString error, details;
        if (!runDefaultFolderMigration(&error, &details)) qWarning().noquote() << error << details;
    }
    // Likewise an interrupted source folder rename, with the map it recorded.
    if (const Thoughts::SourceFolderMigration folders(sourceFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath)); folders.interrupted()) {
        QString error, details;
        if (!runSourceFolderMigration(folders.journalRenames(), &error, &details)) qWarning().noquote() << error << details;
    }

    // This check is entirely local. It finishes an activation only when the
    // active immutable Library and its cached source artifact already match.
    completeLibraryBuildActivationIfReady();

    // Retry cleanup that an earlier session could not finish.
    scheduleLibraryGenerationCleanup(120000);

    state = readRepositoryState();
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    QJsonObject statuses = state.value(QStringLiteral("render_statuses")).toObject();
    const QString message = QStringLiteral("Thoughts restarted before this operation completed. Start it again explicitly.");
    const QString finishedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSet<QString> interruptedTargets;
    bool changed = false;

    for (auto iterator = runs.begin(); iterator != runs.end(); ++iterator) {
        QJsonObject run = iterator.value().toObject();
        const QString previousStatus = run.value(QStringLiteral("status")).toString();
        if (!activeBuildStatus(previousStatus)) {
            continue;
        }
        run.insert(QStringLiteral("status"), QStringLiteral("interrupted"));
        run.insert(QStringLiteral("message"), message);
        run.insert(QStringLiteral("finished_at"), finishedAt);
        iterator.value() = run;
        interruptedTargets.insert(iterator.key());
        const QString runId = run.value(QStringLiteral("run_id")).toString();
        if (!runId.isEmpty() && QFileInfo(buildRunDirectory(runId)).isDir()) {
            writeSubmissionStatus(buildRunDirectory(runId), QStringLiteral("interrupted"), message);
        }

        QJsonObject interrupted = statuses.value(iterator.key()).toObject();
        interrupted.insert(QStringLiteral("status"), QStringLiteral("error"));
        interrupted.insert(QStringLiteral("reason"), message);
        if (previousStatus == QLatin1String("activating")) {
            interrupted.insert(QStringLiteral("local_state"), QStringLiteral("error"));
            interrupted.insert(QStringLiteral("local_error"), message);
        }
        statuses.insert(iterator.key(), interrupted);
        setLiveRenderStatus(iterator.key(), interrupted);
        changed = true;
    }

    if (state.value(QStringLiteral("source_publication_requested")).toBool()) {
        QJsonObject sourceRun = state.value(QStringLiteral("source_publication_run")).toObject();
        if (!sourceRun.isEmpty()) {
            sourceRun.insert(QStringLiteral("status"), QStringLiteral("interrupted"));
            sourceRun.insert(QStringLiteral("message"), message);
            sourceRun.insert(QStringLiteral("finished_at"), finishedAt);
            state.insert(QStringLiteral("source_publication_run"), sourceRun);
            const QString runId = sourceRun.value(QStringLiteral("run_id")).toString();
            if (!runId.isEmpty() && QFileInfo(buildRunDirectory(runId)).isDir()) {
                writeSubmissionStatus(buildRunDirectory(runId), QStringLiteral("interrupted"), message);
            }
        }
        state.remove(QStringLiteral("source_publication_requested"));
        state.remove(QStringLiteral("source_publication_build_target"));
        state.remove(QStringLiteral("source_publication_project"));
        changed = true;
    }

    const QJsonArray deferred = deferredTargetedBuilds(state);
    for (const QJsonValue &value : deferred) {
        const QString target = value.toString();
        if (!safePdfFilename(target)) {
            continue;
        }
        interruptedTargets.insert(target);
        QJsonObject interrupted = statuses.value(target).toObject();
        interrupted.insert(QStringLiteral("status"), QStringLiteral("error"));
        interrupted.insert(QStringLiteral("reason"), message);
        statuses.insert(target, interrupted);
        setLiveRenderStatus(target, interrupted);
    }
    if (!deferred.isEmpty()) {
        storeDeferredTargetedBuilds(state, {});
        changed = true;
    }
    if (!state.value(QStringLiteral("active_builds")).toArray().isEmpty()) {
        state.remove(QStringLiteral("active_builds"));
        changed = true;
    }

    if (!changed) {
        return;
    }
    state.insert(QStringLiteral("build_runs"), runs);
    state.insert(QStringLiteral("render_statuses"), statuses);
    if (!writeRepositoryState(state)) {
        qWarning() << "Could not persist interrupted server work during startup reconciliation.";
        return;
    }
    for (const QString &target : std::as_const(interruptedTargets)) {
        Q_EMIT renderStatusChanged(target, m_renderStatuses.value(target));
    }
}

FedoraRepository::ManualSyncState FedoraRepository::manualSyncState() const
{
    return m_syncCoordinator.state;
}

QString FedoraRepository::manualSyncTarget() const
{
    return m_syncCoordinator.target;
}

QString FedoraRepository::manualSyncOperationId() const
{
    return m_syncCoordinator.operationId;
}

QString FedoraRepository::manualSyncMessage() const
{
    return m_syncCoordinator.message;
}

QString FedoraRepository::manualSyncErrorCode() const
{
    return m_syncCoordinator.errorCode;
}

QString FedoraRepository::manualSyncDetails() const
{
    return m_syncCoordinator.details;
}

bool FedoraRepository::ensureOutboxReady(QString *details)
{
    if (m_outboxReady) {
        return true;
    }
    QString prepareError;
    if (!m_outbox->ready(&prepareError)) {
        if (details) {
            *details = prepareError;
        }
        return false;
    }
    const Thoughts::AnnotationSubtypeRepairResult repair = m_outbox->repairBlankAnnotationSubtypes();
    if (!repair.success) {
        if (details) {
            *details = repair.error;
        }
        return false;
    }
    m_outboxReady = true;
    if (repair.repaired > 0) {
        qInfo().noquote() << QStringLiteral("Repaired %1 queued annotation subtype(s); backup=%2 sha256=%3").arg(repair.repaired).arg(repair.backupPath, QString::fromLatin1(repair.backupSha256));
    }
    return true;
}

// Comment bundles can remain cached after a deletion. Keep an explicit
// tombstone so consumers can suppress those older copies until a restore.
static bool isCommentProjection(const QString &type)
{
    return type == QLatin1String("logical_comment") || type == QLatin1String("annotation");
}

// Answer and document feedback tombstones are kept too: a deletion is sticky (an edit made
// without seeing it must not bring the highlight back), and Undo writes the tombstone's revision + 1.
static bool keepsTombstones(const QString &type)
{
    return isCommentProjection(type) || type == Thoughts::ReviewCardFeedback::entityType() || type == Thoughts::DocumentFeedback::entityType();
}

static bool isKeptTombstone(const QString &type, const QJsonObject &payload)
{
    return keepsTombstones(type) && payload.value(QStringLiteral("deleted")).toBool();
}

// Mirror Fedora's root-deletion cascade in the optimistic and durable snapshots.
// Apply it in operation order so a later explicit Undo can restore the replies.
static void cascadeRootDeletion(QJsonObject &projections, const QJsonObject &root)
{
    if (!root.value(QStringLiteral("deleted")).toBool() || !root.value(QStringLiteral("parent_id")).toString().isEmpty()) return;
    const QString rootId = root.value(QStringLiteral("comment_id")).toString();
    if (rootId.isEmpty()) return;
    for (const QString &key : projections.keys()) {
        if (!key.startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        auto reply = projections.value(key).toObject();
        if (reply.value(QStringLiteral("comment_id")).toString() == rootId || reply.value(QStringLiteral("deleted")).toBool()
            || reply.value(QStringLiteral("thread_root_id")).toString() != rootId) continue;
        reply.insert(QStringLiteral("deleted"), true);
        reply.insert(QStringLiteral("revision"), reply.value(QStringLiteral("revision")).toInteger() + 1);
        reply.insert(QStringLiteral("updated_at"), root.value(QStringLiteral("updated_at")));
        projections.insert(key, reply);
    }
}

// What a comment says, as opposed to where it is placed or whether it is deleted.
static bool logicalCommentContentChanged(const QJsonObject &previous, const QJsonObject &payload)
{
    static const QStringList contentFields {QStringLiteral("contents"), QStringLiteral("author"), QStringLiteral("subtype"), QStringLiteral("selected_text"), QStringLiteral("color"),
        QStringLiteral("priority"), QStringLiteral("location"), QStringLiteral("chatgpt_url"), QStringLiteral("needs_action")};
    return previous.isEmpty() || std::any_of(contentFields.cbegin(), contentFields.cend(), [&](const QString &field) { return previous.value(field) != payload.value(field); });
}

// The PDF version a comment or reply was written on (thoughts-feedback-version-stamps-SPEC.md):
// a new comment, or a change of what it says, gets the stamp of its document's PDF on screen
// (@p knownPdfSha256 when the reader knows it, else the cached hash of the current PDF); the edit's
// stamp replaces the earlier one. Deletions, restores, placement-only changes and Undo/Redo (whose
// payload already differs from the stored stamp) keep the stamp they carry. A to-do (a Note
// without a PDF) gets none.
static QJsonObject stampedLogicalComment(const QString &docsRoot, QJsonObject payload, const QJsonObject &previous, const QJsonObject &threadRoot,
                                         const QString &knownPdfSha256 = {})
{
    const QString field = Thoughts::FeedbackVersion::field();
    if (payload.value(QStringLiteral("deleted")).toBool() || payload.value(field) != previous.value(field)
        || !logicalCommentContentChanged(previous, payload)) return payload;
    QJsonObject origin = payload.value(QStringLiteral("origin")).toObject();
    if (origin.isEmpty()) origin = threadRoot.value(QStringLiteral("origin")).toObject();
    if (origin.isEmpty()) return payload;
    const QString path = Thoughts::FeedbackVersion::documentPdfPath(docsRoot, origin.value(QStringLiteral("document_id")).toString(),
                                                                    origin.value(QStringLiteral("collection")).toString());
    const bool sourceLinked = payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("source_linked");
    return Thoughts::FeedbackVersion::stamped(payload, Thoughts::FeedbackVersion::forPdf(docsRoot, path, sourceLinked, knownPdfSha256));
}

namespace {
Okular::DocsCommentSynchronizer::Options localSourceOptions(const QString &root, const QSet<QString> &trackedSources)
{
    return {.docsRoot = root,
        .sourceBundleRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")),
        .synctexExecutable = QStringLiteral("synctex"), .processEnvironment = QProcessEnvironment::systemEnvironment(),
        .allowLegacySourceResolution = false, .additionalCommentSourceFiles = trackedSources};
}

bool localSourceComment(const QString &root, QJsonObject *payload, const QJsonObject &previous, bool mutate, Okular::DocsCommentSynchronizer *shared = nullptr,
                        const QSet<QString> &trackedSources = {})
{
    // A comment made in the HTML view has no PDF geometry: the server places its marker from its
    // quote (and source range) at the next Sync. Until then it stays pdf_only and waits.
    if (mutate && payload->value(QStringLiteral("origin")).toObject().value(QStringLiteral("created_in")).toString() == QLatin1String("html")
        && payload->value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString() != QLatin1String("source_linked")) {
        payload->insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
        if (!payload->value(QStringLiteral("deleted")).toBool())
            payload->insert(QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pdf_only")}, {QStringLiteral("reason"), QStringLiteral("html_pending_server_link")}});
        return true;
    }
    if (mutate && payload->value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString() != QLatin1String("source_linked")) {
        const auto origin = payload->value(QStringLiteral("origin")).toObject();
        QFile file(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
        if (file.open(QIODevice::ReadOnly)) {
            const auto index = QJsonDocument::fromJson(file.readAll()).object();
            for (const auto &value : index.value(QStringLiteral("pdfs")).toArray()) {
                const auto entry = value.toObject();
                if (entry.value(QStringLiteral("filename")).toString() == origin.value(QStringLiteral("collection")).toString() + QLatin1String(".pdf")
                    && entry.value(QStringLiteral("pdf_sha256")) == origin.value(QStringLiteral("pdf_sha256"))
                    && entry.value(QStringLiteral("kind")).toString() == QLatin1String("unlinked")) {
                    payload->insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
                    payload->insert(QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pdf_only")}, {QStringLiteral("reason"), QStringLiteral("no_source")}});
                    return true;
                }
            }
        }
    }
    Okular::DocsCommentSynchronizer synchronizer(localSourceOptions(root, trackedSources));
    return (shared ? *shared : synchronizer).sourceAuthoritativeComment(payload, previous,
        [](const QString &path, int number, QString *) -> std::optional<Okular::DocsCommentSynchronizer::PageGeometry> {
            auto pdf = Poppler::Document::load(path);
            if (!pdf || number < 1 || number > pdf->numPages()) return std::nullopt;
            auto page = pdf->page(number - 1);
            if (!page || page->orientation() != Poppler::Page::Portrait) return std::nullopt;
            return Okular::DocsCommentSynchronizer::PageGeometry{page->pageSizeF().width(), page->pageSizeF().height(), 0};
        }, mutate);
}
}

QJsonObject FedoraRepository::effectiveProjections(bool includeSourceJournals, bool includeSourceSnapshot) const
{
    QJsonObject index;
    QJsonObject projections;
    if (Thoughts::CurrentAnnotationIndex::read(m_stateRoot, &index)) {
        projections = index.value(QStringLiteral("projections")).toObject();
    } else {
        const QJsonObject stored = readRepositoryState().value(QStringLiteral("projections")).toObject();
        for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
            if (!it.key().startsWith(QLatin1String("proposal_event\nproposals\n"))) projections.insert(it.key(), it.value());
        }
        if (QFileInfo::exists(QDir(m_stateRoot).filePath(QStringLiteral("repository.json")))
            && !Thoughts::CurrentAnnotationIndex::write(m_stateRoot, readRepositoryState()))
            qWarning() << "Could not create the current annotation database.";
    }
    QString error;
    const QList<QJsonObject> operations = m_outbox->operations(&error);
    if (!error.isEmpty()) {
        return projections;
    }
    for (const QJsonObject &operation : operations) {
        QString type = operation.value(QStringLiteral("entity_type")).toString();
        if (type == QLatin1String("comment")) {
            type = QStringLiteral("annotation");
        }
        const QString collection = operation.value(QStringLiteral("collection")).toString();
        const QString entityId = operation.value(QStringLiteral("entity_id")).toString();
        const QJsonValue payload = operation.value(QStringLiteral("payload"));
        if (!safeComponent(type) || !safeComponent(collection) || !safeComponent(entityId) || !payload.isObject()) {
            continue;
        }
        const QString key = type + QLatin1Char('\n') + collection + QLatin1Char('\n') + entityId;
        if (type == Thoughts::ReviewCardFeedback::entityType()) {
            // A queued write shows only where the server will apply it too (§5 of the spec).
            projections.insert(key, Thoughts::ReviewCardFeedback::merge(projections.value(key).toObject(), payload.toObject()));
        } else if (type == Thoughts::DocumentFeedback::entityType()) {
            projections.insert(key, Thoughts::DocumentFeedback::merge(projections.value(key).toObject(), payload.toObject()));
        } else if (payload.toObject().value(QStringLiteral("deleted")).toBool() && !isCommentProjection(type)) {
            projections.remove(key);
        } else {
            projections.insert(key, payload);
            if (type == QLatin1String("logical_comment")) cascadeRootDeletion(projections, payload.toObject());
        }
    }
    if (includeSourceJournals) {
        const QDir journal(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-journal")));
        for (const auto &path : journal.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile file(journal.filePath(path));
            if (!file.open(QIODevice::ReadOnly)) continue;
            for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
                const auto payload = value.toObject();
                const auto key = QStringLiteral("logical_comment\ncomments\n") + payload.value(QStringLiteral("comment_id")).toString();
                if (payload.value(QStringLiteral("revision")).toInteger() > projections.value(key).toObject().value(QStringLiteral("revision")).toInteger()) projections.insert(key, payload);
            }
        }
    }
    if (includeSourceSnapshot) {
        for (auto it = projections.begin(); it != projections.end(); ++it) {
            const auto cached = m_sourceCommentSnapshot.value(it.key()).toObject();
            // A projection may only replace the exact record from which it was derived.
            if (cached.value(QStringLiteral("input")) == it.value()) {
                it.value() = cached.value(QStringLiteral("output"));
            }
        }
        // Local-only Issues overlay; never queued or synced.
        if (!m_missingCommentMarkers.isEmpty())
            projections.insert(Thoughts::commentMissingProjectionKey(), QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("markers"), m_missingCommentMarkers}});
    }
    QHash<QString, QJsonObject> deletedRoots;
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        const auto root = it.value().toObject();
        if (root.value(QStringLiteral("deleted")).toBool() && root.value(QStringLiteral("parent_id")).toString().isEmpty())
            deletedRoots.insert(root.value(QStringLiteral("comment_id")).toString(), root);
    }
    for (auto it = projections.begin(); it != projections.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        auto reply = it.value().toObject();
        const auto rootId = reply.value(QStringLiteral("thread_root_id")).toString();
        if (reply.value(QStringLiteral("deleted")).toBool() || reply.value(QStringLiteral("comment_id")).toString() == rootId || !deletedRoots.contains(rootId)) continue;
        reply.insert(QStringLiteral("deleted"), true);
        reply.insert(QStringLiteral("revision"), reply.value(QStringLiteral("revision")).toInteger() + 1);
        reply.insert(QStringLiteral("updated_at"), deletedRoots.value(rootId).value(QStringLiteral("updated_at")));
        it.value() = reply;
    }
    return projections;
}

bool FedoraRepository::hasProjectionSnapshot() const
{
    QJsonObject index;
    if (Thoughts::CurrentAnnotationIndex::read(m_stateRoot, &index))
        return index.value(QStringLiteral("bootstrapped")).toBool() || m_outbox->cursor() > 0
            || !index.value(QStringLiteral("projections")).toObject().isEmpty();
    const QJsonObject state = readRepositoryState();
    return state.value(QStringLiteral("bootstrapped")).toBool() || m_outbox->cursor() > 0 || !state.value(QStringLiteral("projections")).toObject().isEmpty();
}

bool FedoraRepository::enqueueLogicalComment(const QJsonObject &payload)
{
    return enqueueLogicalComments({payload});
}

bool FedoraRepository::enqueueLogicalComments(const QList<QJsonObject> &incoming)
{
    QList<QJsonObject> operations;
    const auto projections = effectiveProjections();
    for (const auto &payload : incoming) {
        const auto scope = payload.value(QStringLiteral("scope")).toObject();
        const auto origin = payload.value(QStringLiteral("origin"));
        const QString state = scope.value(QStringLiteral("state")).toString();
        const bool inheritedOrigin = !payload.value(QStringLiteral("parent_id")).toString().isEmpty()
            || state == QLatin1String("source_linked") || state == QLatin1String("linking") || state == QLatin1String("needs_attention")
            || (state == QLatin1String("standalone") && payload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note"));
        if (payload.value(QStringLiteral("schema_version")).toInt() != 1
            || !safeComponent(payload.value(QStringLiteral("comment_id")).toString())
            || payload.value(QStringLiteral("revision")).toInteger() < 1
            || payload.value(QStringLiteral("updated_at")).toString().isEmpty() || scope.isEmpty()
            || (!origin.isObject() && !(origin.isNull() && inheritedOrigin))) return false;
    }
    // Stamped before the journal, so a replay after a crash queues the same bytes.
    QList<QJsonObject> payloads;
    QHash<QString, QJsonObject> batch;
    for (const auto &payload : incoming) batch.insert(payload.value(QStringLiteral("comment_id")).toString(), payload);
    for (const auto &payload : incoming) {
        const QString rootId = payload.value(QStringLiteral("thread_root_id")).toString();
        const QJsonObject root = batch.contains(rootId) ? batch.value(rootId) : projections.value(QStringLiteral("logical_comment\ncomments\n") + rootId).toObject();
        payloads.append(stampedLogicalComment(m_docsRoot, payload, projections.value(QStringLiteral("logical_comment\ncomments\n") + payload.value(QStringLiteral("comment_id")).toString()).toObject(), root));
    }
    const QDir journal(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-journal")));
    if (!QDir().mkpath(journal.path())) return false;
    const QString journalPath = journal.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    QJsonArray pending;
    for (auto payload : payloads) {
        if (payload.value(QStringLiteral("subtype")).toString() != QLatin1String("Note")) payload.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
        pending.append(payload);
    }
    if (!writeJsonFile(journalPath, QJsonDocument(pending).toJson(QJsonDocument::Compact))) return false;
    const QSet<QString> trackedSources = trackedCommentSourceFiles();
    QSet<QString> deletedRoots;
    for (const auto &payload : payloads) {
        if (payload.value(QStringLiteral("deleted")).toBool() && payload.value(QStringLiteral("parent_id")).toString().isEmpty())
            deletedRoots.insert(payload.value(QStringLiteral("comment_id")).toString());
    }
    for (QJsonObject payload : payloads) {
        const QString id = payload.value(QStringLiteral("comment_id")).toString();
        const qint64 revision = payload.value(QStringLiteral("revision")).toInteger();
        const QString timestamp = payload.value(QStringLiteral("updated_at")).toString();
        const auto scope = payload.value(QStringLiteral("scope")).toObject();
        const QString state = scope.value(QStringLiteral("state")).toString();
        const bool inheritedOrigin = !payload.value(QStringLiteral("parent_id")).toString().isEmpty()
            || state == QLatin1String("source_linked") || state == QLatin1String("linking") || state == QLatin1String("needs_attention")
            || (state == QLatin1String("standalone") && payload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note"));
        if (payload.value(QStringLiteral("schema_version")).toInt() != 1 || !safeComponent(id) || revision < 1 || timestamp.isEmpty() || scope.isEmpty()
            || (!payload.value(QStringLiteral("origin")).isObject() && !(payload.value(QStringLiteral("origin")).isNull() && inheritedOrigin))) return false;
        const auto previous = projections.value(QStringLiteral("logical_comment\ncomments\n") + id).toObject();
        if (!localSourceComment(m_docsRoot, &payload, previous, true, nullptr, trackedSources)) return false;
        const bool changed = logicalCommentContentChanged(previous, payload);
        payload.insert(QStringLiteral("content_updated_at"), changed ? timestamp : previous.value(QStringLiteral("content_updated_at")).toString(previous.value(QStringLiteral("updated_at")).toString()));
        // The server owns the cascade; retain replies in service history for Undo,
        // but do not enqueue their redundant deletion requests.
        if (payload.value(QStringLiteral("deleted")).toBool() && !payload.value(QStringLiteral("parent_id")).toString().isEmpty()
            && deletedRoots.contains(payload.value(QStringLiteral("thread_root_id")).toString())) continue;
        operations.append({{QStringLiteral("entity_type"), QStringLiteral("logical_comment")}, {QStringLiteral("entity_id"), id}, {QStringLiteral("collection"), QStringLiteral("comments")},
            {QStringLiteral("base_revision"), revision - 1}, {QStringLiteral("client_timestamp"), timestamp}, {QStringLiteral("payload"), payload}});
    }
    if (!operations.isEmpty() && (!ensureOutboxReady() || m_outbox->enqueueBatch(operations).size() != operations.size())) return false;
    if (!QFile::remove(journalPath)) return false;
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueReviewCorrespondenceReview(const QJsonObject &payload)
{
    const QString reviewKey = payload.value(QStringLiteral("review_key")).toString();
    const qint64 revision = payload.value(QStringLiteral("revision")).toInteger();
    const QString timestamp = payload.value(QStringLiteral("updated_at")).toString();
    const QString action = payload.value(QStringLiteral("action")).toString();
    const QString sourceBundleSha256 = payload.value(QStringLiteral("source_bundle_sha256")).toString();
    const QString pdfSha256 = payload.value(QStringLiteral("pdf_sha256")).toString();
    static const QSet<QString> actions {
        QStringLiteral("confirm"),
        QStringLiteral("flag"),
        QStringLiteral("recompute"),
    };
    if (payload.value(QStringLiteral("schema_version")).toInt() != 1 || !safeComponent(reviewKey) || revision < 1 || timestamp.isEmpty() || !actions.contains(action) || !validSha256(sourceBundleSha256) ||
        (!pdfSha256.isEmpty() && !validSha256(pdfSha256))) {
        return false;
    }
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The placement decision could not be added to the local sync queue."));
        return false;
    }
    const QJsonObject operation = m_outbox->enqueue(QStringLiteral("review_correspondence_review"), reviewKey, QStringLiteral("review-correspondence"), revision - 1, payload, timestamp);
    if (operation.isEmpty()) {
        return false;
    }
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueuePdfLinkOverride(const QJsonObject &record, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::PdfLinkOverrides::enqueue(*m_outbox, record, error)) {
        return false;
    }
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueAgentConversation(const QJsonObject &record, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::AgentConversations::enqueue(*m_outbox, record, error)) {
        return false;
    }
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueDocumentFeedback(const QJsonObject &record, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::DocumentFeedback::enqueue(*m_outbox, record, error)) {
        return false;
    }
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueReviewCardFeedback(const QJsonObject &record, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::ReviewCardFeedback::enqueue(*m_outbox, record, error)) {
        return false;
    }
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueReviewCardFeedbackBatch(const QList<QJsonObject> &records, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::ReviewCardFeedback::enqueueAll(*m_outbox, records, error)) return false;
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

bool FedoraRepository::enqueueDocumentFeedbackBatch(const QList<QJsonObject> &records, QString *error)
{
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        if (error) *error = outboxDetails.isEmpty() ? QStringLiteral("The local sync queue is not available.") : outboxDetails;
        return false;
    }
    if (!Thoughts::DocumentFeedback::enqueueAll(*m_outbox, records, error)) return false;
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    return true;
}

QJsonObject FedoraRepository::pendingChanges() const
{
    return m_pendingChangesSnapshot;
}

QJsonObject FedoraRepository::calculatePendingChanges() const
{
    QJsonObject result {
        {QStringLiteral("available"), false},
        {QStringLiteral("comments"), 0},
        {QStringLiteral("review_cards"), 0},
        {QStringLiteral("review_events"), 0},
        {QStringLiteral("other"), 0},
        {QStringLiteral("total"), 0},
    };
    QString error;
    const QList<QJsonObject> operations = m_outbox->operations(&error);
    if (!error.isEmpty()) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    QString trackerError;
    Thoughts::DesktopSyncStore *const store = const_cast<FedoraRepository *>(this)->desktopSyncStore(&trackerError);
    QList<Thoughts::DesktopLibraryOperation> libraryOperations;
    if (store && trackerError.isEmpty()) {
        libraryOperations = store->libraryOperations(false, &trackerError);
    }
    if (!store || !trackerError.isEmpty()) {
        result.insert(QStringLiteral("error"), trackerError.isEmpty() ? QStringLiteral("The durable local Library queue is unavailable.") : trackerError);
        return result;
    }

    const auto cardStem = [](const QJsonObject &payload) { return payload.value(QStringLiteral("primary")).toObject().value(QStringLiteral("pdf_stem")).toString(); };
    QHash<QString, QString> reviewCardStems;
    // Only card-to-document identities are needed here. Comment projection and
    // thread deletion propagation belong to the comment snapshot, not status reads.
    // Only review events need card stems; most status reads have none queued.
    const bool reviewEventsQueued = std::any_of(operations.cbegin(), operations.cend(), [](const QJsonObject &operation) {
        return operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_event");
    });
    const QJsonObject projections = reviewEventsQueued ? readRepositoryState().value(QStringLiteral("projections")).toObject() : QJsonObject();
    for (auto iterator = projections.constBegin(); iterator != projections.constEnd(); ++iterator) {
        if (!iterator.key().startsWith(QLatin1String("review_card\n"))) continue;
        const QStringList components = iterator.key().split(QLatin1Char('\n'));
        if (components.size() == 3 && components.at(0) == QLatin1String("review_card")) {
            const QString stem = cardStem(iterator.value().toObject());
            if (!stem.isEmpty()) {
                reviewCardStems.insert(components.at(2), stem);
            }
        }
    }
    for (const QJsonObject &operation : operations) {
        if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_card")) {
            const QString stem = cardStem(operation.value(QStringLiteral("payload")).toObject());
            if (!stem.isEmpty()) {
                reviewCardStems.insert(operation.value(QStringLiteral("entity_id")).toString(), stem);
            }
        }
    }

    int comments = 0;
    int reviewCards = 0;
    int reviewEvents = 0;
    Thoughts::LibraryProjectStore projectStore(m_docsRoot);
    const auto projectOperations = projectStore.pending();
    if (!projectStore.error().isEmpty()) { result.insert(QStringLiteral("error"), projectStore.error()); return result; }
    int other = projectOperations.size();
    QString latestTimestamp;
    QJsonObject documents;
    for (const QJsonObject &operation : operations) {
        QString entityType = operation.value(QStringLiteral("entity_type")).toString();
        if (entityType == QLatin1String("comment")) {
            entityType = QStringLiteral("annotation");
        }
        const QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
        QString stem;
        if (entityType == QLatin1String("annotation")) {
            stem = operation.value(QStringLiteral("collection")).toString();
        } else if (entityType == QLatin1String("review_card")) {
            stem = cardStem(payload);
        } else if (entityType == QLatin1String("review_event")) {
            stem = reviewCardStems.value(payload.value(QStringLiteral("card_id")).toString());
        }
        QString category;
        if (entityType == QLatin1String("annotation")) {
            ++comments;
            category = QStringLiteral("comments");
        } else if (entityType == QLatin1String("review_card")) {
            ++reviewCards;
            category = QStringLiteral("review_cards");
        } else if (entityType == QLatin1String("review_event")) {
            ++reviewEvents;
            category = QStringLiteral("review_events");
        } else {
            ++other;
            category = QStringLiteral("other");
        }
        const QString timestamp = operation.value(QStringLiteral("client_timestamp")).toString();
        if (timestamp > latestTimestamp) {
            latestTimestamp = timestamp;
        }
        QStringList filenames;
        if (!stem.isEmpty()) {
            filenames.append(stem.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) ? stem : stem + QStringLiteral(".pdf"));
        }
        const QJsonArray proposalFilenames = payload.value(QStringLiteral("pdf_filenames")).toArray();
        for (const QJsonValue &value : proposalFilenames) {
            if (safePdfFilename(value.toString())) {
                filenames.append(value.toString());
            }
        }
        filenames.removeDuplicates();
        for (const QString &filename : std::as_const(filenames)) {
            QJsonObject document = documents.value(filename).toObject();
            document.insert(QStringLiteral("available"), true);
            document.insert(category, document.value(category).toInt() + 1);
            document.insert(QStringLiteral("total"), document.value(QStringLiteral("total")).toInt() + 1);
            if (timestamp > document.value(QStringLiteral("latest_timestamp")).toString()) {
                document.insert(QStringLiteral("latest_timestamp"), timestamp);
            }
            documents.insert(filename, document);
        }
    }
    int libraryChanges = 0;
    QJsonArray libraryOperationValues;
    QSet<QString> queuedLibraryRemovals;
    for (const Thoughts::DesktopLibraryOperation &operation : std::as_const(libraryOperations)) {
        ++libraryChanges;
        ++other;
        const QString timestamp = QDateTime::fromMSecsSinceEpoch(operation.createdAtMs, Qt::UTC).toString(Qt::ISODateWithMs);
        if (timestamp > latestTimestamp) {
            latestTimestamp = timestamp;
        }
        const QJsonObject value {{QStringLiteral("operation_id"), operation.operationId},
                                 {QStringLiteral("generation"), static_cast<qint64>(operation.generation)},
                                 {QStringLiteral("type"), operation.type},
                                 {QStringLiteral("filename"), operation.filename},
                                 {QStringLiteral("state"), operation.state},
                                 {QStringLiteral("mutation"), operation.mutation}};
        libraryOperationValues.append(value);
        if (operation.type == QLatin1String("remove")) {
            queuedLibraryRemovals.insert(operation.filename + QLatin1Char('\n') + operation.mutation.value(QStringLiteral("sha256")).toString());
        }
        QJsonObject document = documents.value(operation.filename).toObject();
        document.insert(QStringLiteral("available"), true);
        document.insert(QStringLiteral("other"), document.value(QStringLiteral("other")).toInt() + 1);
        document.insert(QStringLiteral("library_changes"), document.value(QStringLiteral("library_changes")).toInt() + 1);
        document.insert(QStringLiteral("total"), document.value(QStringLiteral("total")).toInt() + 1);
        QJsonArray documentLibraryOperations = document.value(QStringLiteral("library_operations")).toArray();
        documentLibraryOperations.append(value);
        document.insert(QStringLiteral("library_operations"), documentLibraryOperations);
        if (timestamp > document.value(QStringLiteral("latest_timestamp")).toString()) {
            document.insert(QStringLiteral("latest_timestamp"), timestamp);
        }
        documents.insert(operation.filename, document);
    }
    const QJsonObject localRemovalTombstones = readRepositoryState().value(QStringLiteral("local_library_removals")).toObject();
    for (auto iterator = localRemovalTombstones.constBegin(); iterator != localRemovalTombstones.constEnd(); ++iterator) {
        const QString filename = iterator.key();
        const QJsonObject tombstone = iterator.value().toObject();
        const QString digest = tombstone.value(QStringLiteral("sha256")).toString();
        if (!safePdfFilename(filename) || !validSha256(digest)
            || queuedLibraryRemovals.contains(filename + QLatin1Char('\n') + digest)) {
            continue;
        }
        ++libraryChanges;
        ++other;
        const QString timestamp = tombstone.value(QStringLiteral("created_at")).toString();
        if (timestamp > latestTimestamp) {
            latestTimestamp = timestamp;
        }
        const QJsonObject mutation {{QStringLiteral("type"), QStringLiteral("remove")},
                                    {QStringLiteral("request_id"), tombstone.value(QStringLiteral("request_id"))},
                                    {QStringLiteral("filename"), filename},
                                    {QStringLiteral("sha256"), digest}};
        const QJsonObject value {{QStringLiteral("operation_id"), tombstone.value(QStringLiteral("request_id"))},
                                 {QStringLiteral("generation"), 0},
                                 {QStringLiteral("type"), QStringLiteral("remove")},
                                 {QStringLiteral("filename"), filename},
                                 {QStringLiteral("state"), QStringLiteral("pending_activation")},
                                 {QStringLiteral("mutation"), mutation}};
        libraryOperationValues.append(value);
        QJsonObject document = documents.value(filename).toObject();
        document.insert(QStringLiteral("available"), true);
        document.insert(QStringLiteral("other"), document.value(QStringLiteral("other")).toInt() + 1);
        document.insert(QStringLiteral("library_changes"), document.value(QStringLiteral("library_changes")).toInt() + 1);
        document.insert(QStringLiteral("total"), document.value(QStringLiteral("total")).toInt() + 1);
        QJsonArray documentLibraryOperations = document.value(QStringLiteral("library_operations")).toArray();
        documentLibraryOperations.append(value);
        document.insert(QStringLiteral("library_operations"), documentLibraryOperations);
        if (timestamp > document.value(QStringLiteral("latest_timestamp")).toString()) {
            document.insert(QStringLiteral("latest_timestamp"), timestamp);
        }
        documents.insert(filename, document);
    }
    result.insert(QStringLiteral("available"), true);
    result.insert(QStringLiteral("comments"), comments);
    result.insert(QStringLiteral("review_cards"), reviewCards);
    result.insert(QStringLiteral("review_events"), reviewEvents);
    result.insert(QStringLiteral("other"), other);
    result.insert(QStringLiteral("library_changes"), libraryChanges);
    result.insert(QStringLiteral("library_operations"), libraryOperationValues);
    int sourceFiles = 0;
    const Thoughts::DesktopSyncSummary sourceSummary = store->summary(&trackerError);
    QString sourceState = QStringLiteral("unknown");
    bool inventoryAccepted = false;
    const_cast<FedoraRepository *>(this)->sourceManifestMatchesAccepted(store, &inventoryAccepted, &trackerError);
    // A background re-check after a save briefly clears the tracker's
    // authority. Keep reporting its last settled result meanwhile, so the
    // Library does not flash "checking" and rebuild twice per save; startup,
    // failures, unresolved files and a manual Sync still report "unknown".
    const bool sourceAuthoritative = inventoryAccepted && m_sourceChangeTracker
        && (m_sourceChangeTracker->authoritative()
            || (m_sourceTrackingSettled && m_sourceChangeTracker->reconciliationInProgress() && !m_syncCoordinator.running()));
    if (trackerError.isEmpty() && sourceAuthoritative && sourceSummary.status == Thoughts::DesktopSyncStatus::Clean) {
        sourceState = QStringLiteral("clean");
    } else if (trackerError.isEmpty() && sourceAuthoritative && sourceSummary.status == Thoughts::DesktopSyncStatus::Pending) {
        sourceState = QStringLiteral("pending");
    }
    sourceFiles = sourceSummary.pendingFileCount;
    QJsonArray filePaths;
    for (const QString &path : sourceSummary.dirtyPaths) {
        filePaths.append(path);
    }
    QJsonObject fileGroups;
    for (auto iterator = sourceSummary.filesByTarget.cbegin(); iterator != sourceSummary.filesByTarget.cend(); ++iterator) {
        QJsonArray files;
        for (const Thoughts::DesktopPendingFile &file : iterator.value()) {
            files.append(QJsonObject {
                {QStringLiteral("path"), file.path}, {QStringLiteral("role"), file.metadata.value(QStringLiteral("role"))}, {QStringLiteral("action"), file.action}, {QStringLiteral("generation"), static_cast<qint64>(file.generation)}});
        }
        fileGroups.insert(iterator.key(), files);
        if (safePdfFilename(iterator.key())) {
            QJsonObject document = documents.value(iterator.key()).toObject();
            document.insert(QStringLiteral("available"), true);
            document.insert(QStringLiteral("files"), files.size());
            document.insert(QStringLiteral("file_paths"), files);
            document.insert(QStringLiteral("total"), document.value(QStringLiteral("total")).toInt() + files.size());
            documents.insert(iterator.key(), document);
        }
    }
    result.insert(QStringLiteral("files"), sourceFiles);
    result.insert(QStringLiteral("file_paths"), filePaths);
    result.insert(QStringLiteral("file_groups"), fileGroups);
    result.insert(QStringLiteral("source_state"), sourceState);
    result.insert(QStringLiteral("total"), comments + reviewCards + reviewEvents + other + sourceFiles);
    result.insert(QStringLiteral("latest_timestamp"), latestTimestamp);
    result.insert(QStringLiteral("documents"), documents);
    if (!trackerError.isEmpty()) {
        result.insert(QStringLiteral("source_error"), trackerError);
    }
    return result;
}

QJsonObject FedoraRepository::pendingDocumentChanges(const QString &target) const
{
    QJsonObject unavailable {
        {QStringLiteral("available"), false},
        {QStringLiteral("source_coverage_available"), false},
        {QStringLiteral("comments"), 0},
        {QStringLiteral("review_cards"), 0},
        {QStringLiteral("review_events"), 0},
        {QStringLiteral("other"), 0},
        {QStringLiteral("total"), 0},
    };
    if (!safePdfFilename(target)) {
        return unavailable;
    }
    const QJsonObject snapshot = pendingChanges();
    if (!snapshot.value(QStringLiteral("available")).toBool()) {
        if (snapshot.contains(QStringLiteral("error"))) {
            unavailable.insert(QStringLiteral("error"), snapshot.value(QStringLiteral("error")));
        }
        return unavailable;
    }
    QJsonObject document = snapshot.value(QStringLiteral("documents")).toObject().value(target).toObject();
    document.insert(QStringLiteral("available"), true);
    document.insert(QStringLiteral("source_coverage_available"), snapshot.value(QStringLiteral("source_state")).toString() != QLatin1String("unknown"));
    for (const QString &field : {QStringLiteral("comments"), QStringLiteral("review_cards"), QStringLiteral("review_events"), QStringLiteral("other"), QStringLiteral("files"), QStringLiteral("total")}) {
        if (!document.contains(field)) {
            document.insert(field, 0);
        }
    }
    return document;
}

void FedoraRepository::scheduleSourceCommentRefresh()
{
    Thoughts::invalidateSourceLocationIndex(m_docsRoot);
    ++m_sourceCommentGeneration;
    if (m_sourceCommentTimer) m_sourceCommentTimer->start();
}

void FedoraRepository::refreshSourceComments()
{
    if (m_sourceCommentRunning) return;
    if (!m_sourceChangeTracker || m_sourceChangeTracker->reconciliationInProgress()) return;
    const auto input = effectiveProjections(true, false);
    const auto paths = m_sourceChangeTracker->trackedPaths();
    const bool complete = m_sourceChangeTracker->authoritative();
    // A file still settling or unresolved only makes its own project's
    // locations incomplete; the other projects keep their cards available.
    QSet<QString> settledProjects;
    if (!complete) {
        for (const auto &path : m_sourceChangeTracker->trackedPaths()) {
            const QString project = path.metadata.value(QStringLiteral("project")).toString();
            if (!project.isEmpty() && !settledProjects.contains(project) && m_sourceChangeTracker->sourceSettled(project)) settledProjects.insert(project);
        }
    }
    const auto generation = m_sourceCommentGeneration;
    const auto root = m_docsRoot;
    const auto previous = m_sourceCommentSnapshot;
    const auto previousFiles = m_sourceCommentFileIndex;
    // Before the first Sync the store cannot tell a missing comment from one not yet downloaded.
    const bool storeLoaded = hasProjectionSnapshot();
    m_sourceCommentRunning = true;
    QPointer<FedoraRepository> guard(this);
    QThreadPool::globalInstance()->start([guard, input, paths, complete, settledProjects, generation, root, previous, previousFiles, storeLoaded] {
        QHash<QString, QSet<QString>> projects;
        QHash<QString, QString> owners;
        QHash<QString, QSet<QString>> targetProjects;
        QHash<QString, QString> signatures;
        QHash<QString, QStringList> fileTargets;
        QStringList identifiers;
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) {
                const auto record = it.value().toObject();
                // Deleted roots are scanned too: their leftover markers are cleaned up below.
                if (!record.value(QStringLiteral("parent_id")).toString().isEmpty()
                    || record.value(QStringLiteral("subtype")).toString() == QLatin1String("Note")) continue;
                const auto id = record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("anchor_id")).toString(record.value(QStringLiteral("comment_id")).toString());
                if (!id.isEmpty()) identifiers.append(id);
            }
        }
        identifiers.removeDuplicates();
        std::sort(identifiers.begin(), identifiers.end());
        QJsonObject files;
        QSet<QString> unreadableProjects;
        for (const auto &path : paths) {
            if (!path.path.endsWith(QLatin1String(".tex")) && !path.path.endsWith(QLatin1String(".md")) && !path.path.endsWith(QLatin1String(".markdown"))) continue;
            if (path.affectedTargets.isEmpty()) continue;
            // Ignored descriptors are excluded from builds and never re-observed
            // by the tracker, so they cannot anchor source comments.
            if (path.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("ignored")) continue;
            const auto project = path.metadata.value(QStringLiteral("project")).toString();
            if (project.isEmpty()) continue;
            owners.insert(path.path, project);
            for (const auto &target : path.affectedTargets) targetProjects[target].insert(project);
            fileTargets.insert(path.path, QStringList(path.affectedTargets.cbegin(), path.affectedTargets.cend()));
            signatures[project] += path.path + QLatin1Char(':') + path.sha256 + (path.exists ? QLatin1String(":present\n") : QLatin1String(":deleted\n"));
            if (path.exists) {
                projects[project].insert(path.path);
                auto indexed = previousFiles.value(path.path).toObject();
                if (indexed.value(QStringLiteral("sha256")).toString() != path.sha256 || indexed.value(QStringLiteral("annotation_parser_version")).toInt() != Thoughts::annotationParserVersion) {
                    indexed = DocsCommentSynchronizer::sourceCommentFileIndex(QDir(root).filePath(path.path), identifiers);
                }
                if (indexed.value(QStringLiteral("sha256")).toString() != path.sha256 || !indexed.contains(QStringLiteral("markers")) || !indexed.value(QStringLiteral("valid")).toBool()) unreadableProjects.insert(project);
                files.insert(path.path, indexed);
            }
        }
        QJsonObject indexedFiles, indexedProjects;
        for (auto it = signatures.begin(); it != signatures.end(); ++it)
            indexedProjects.insert(it.key(), (complete || settledProjects.contains(it.key())) && !unreadableProjects.contains(it.key()));
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto f = it.value().toObject();
            indexedFiles.insert(it.key(), QJsonObject{{QStringLiteral("project"), owners.value(it.key())},
                {QStringLiteral("sha256"), f.value(QStringLiteral("sha256"))}, {QStringLiteral("valid"), f.value(QStringLiteral("valid"))},
                {QStringLiteral("locations"), f.value(QStringLiteral("locations"))}, {QStringLiteral("mentioned"), f.value(QStringLiteral("mentioned"))}});
        }
        const QJsonObject locationIndex{{QStringLiteral("schema_version"), 1}, {QStringLiteral("files"), indexedFiles}, {QStringLiteral("projects"), indexedProjects}};
        QJsonObject snapshot;
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
            const auto record = it.value().toObject();
            if (record.value(QStringLiteral("subtype")).toString() == QLatin1String("Note") || !record.value(QStringLiteral("parent_id")).toString().isEmpty()) continue;
            const auto source = record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_file")).toString();
            auto project = owners.value(source);
            // Failed first-time linking can leave no cached source path. Recover only
            // within the one project recorded for this PDF, never across projects.
            if (project.isEmpty() && source.isEmpty()) {
                const auto target = record.value(QStringLiteral("origin")).toObject().value(QStringLiteral("collection")).toString() + QLatin1String(".pdf");
                const auto candidates = targetProjects.value(target);
                if (candidates.size() == 1) project = *candidates.cbegin();
            }
            if (project.isEmpty()) continue; // Unknown coverage cannot establish deletion or a move.
            // The parser version is part of the key, so a parser fix re-derives cached outputs too.
            const QString signature = QStringLiteral("annotation-parser:%1\n").arg(Thoughts::annotationParserVersion)
                + signatures.value(project) + (complete ? QLatin1String("complete") : QLatin1String("incomplete"));
            const auto cached = previous.value(it.key()).toObject();
            if (cached.value(QStringLiteral("input")) == it.value() && cached.value(QStringLiteral("sources")).toString() == signature) {
                snapshot.insert(it.key(), cached);
                continue;
            }
            // Preserve annotation content on incomplete scans, but do not claim a current location.
            if (!complete || unreadableProjects.contains(project)) {
                auto output = cached.value(QStringLiteral("input")) == it.value() ? cached.value(QStringLiteral("output")).toObject() : record;
                if (Thoughts::isAnchorRemovedScope(record.value(QStringLiteral("scope")).toObject())) output = record;
                auto scope = output.value(QStringLiteral("scope")).toObject();
                if (Thoughts::isAnchorRemovedScope(scope)) {
                    // Only a complete scan can relink it; keep its last known location for Restore.
                    snapshot.insert(it.key(), QJsonObject{{QStringLiteral("input"), record}, {QStringLiteral("output"), output}, {QStringLiteral("sources"), signature}});
                    continue;
                }
                scope.insert(QStringLiteral("state"), QStringLiteral("source_unavailable"));
                scope.insert(QStringLiteral("reason"), QStringLiteral("source_project_incomplete"));
                scope.remove(QStringLiteral("source_line")); scope.remove(QStringLiteral("source_end_line"));
                output.remove(QStringLiteral("source_line")); output.insert(QStringLiteral("scope"), scope);
                snapshot.insert(it.key(), QJsonObject{{QStringLiteral("input"), record}, {QStringLiteral("output"), output}, {QStringLiteral("sources"), signature}});
                continue;
            }
            auto output = record;
            auto scope = record.value(QStringLiteral("scope")).toObject();
            const auto id = scope.value(QStringLiteral("anchor_id")).toString(record.value(QStringLiteral("comment_id")).toString());
            QJsonArray matches;
            bool mentioned = false;
            for (const auto &path : projects.value(project)) {
                const auto indexed = files.value(path).toObject();
                mentioned = mentioned || indexed.value(QStringLiteral("mentioned")).toArray().contains(id);
                for (const auto &value : indexed.value(QStringLiteral("markers")).toObject().value(id).toArray()) {
                    auto marker = value.toObject(); marker.insert(QStringLiteral("source_file"), path); matches.append(marker);
                }
            }
            if (!record.value(QStringLiteral("deleted")).toBool() && record.value(QStringLiteral("parent_id")).toString().isEmpty()
                && record.value(QStringLiteral("subtype")).toString() != QLatin1String("Note")) {
                if (matches.size() == 1 && !unreadableProjects.contains(project)) {
                    const auto marker = matches.first().toObject();
                    output.insert(QStringLiteral("contents"), marker.value(QStringLiteral("contents")));
                    if (marker.contains(QStringLiteral("selected_text"))) output.insert(QStringLiteral("selected_text"), marker.value(QStringLiteral("selected_text")));
                    for (const auto &key : {QStringLiteral("anchor_kind"), QStringLiteral("anchor_mirror"), QStringLiteral("anchor_removed_at")}) scope.remove(key);
                    scope.insert(QStringLiteral("state"), QStringLiteral("source_linked"));
                    scope.insert(QStringLiteral("anchor_id"), id);
                    scope.insert(QStringLiteral("source_file"), marker.value(QStringLiteral("source_file")));
                    const auto location = Thoughts::commentSourceLocation(locationIndex, record.value(QStringLiteral("subtype")).toString(), id, project);
                    if (location.value(QStringLiteral("state")).toString() != QLatin1String("linked")) {
                        scope.insert(QStringLiteral("state"), QStringLiteral("needs_attention"));
                        scope.insert(QStringLiteral("reason"), QStringLiteral("source_marker_unavailable"));
                    } else {
                        scope.insert(QStringLiteral("source_file"), location.value(QStringLiteral("source_file")));
                        scope.insert(QStringLiteral("source_line"), location.value(QStringLiteral("start_line")));
                        scope.insert(QStringLiteral("source_end_line"), location.value(QStringLiteral("end_line")));
                        scope.insert(QStringLiteral("reason"), QStringLiteral("unique_source_marker"));
                    }
                } else if (matches.isEmpty() && !mentioned && complete && !unreadableProjects.contains(project)
                           && scope.value(QStringLiteral("state")).toString() == QLatin1String("source_linked")) {
                    // The marker vanished without an explicit Delete: keep the comment and its replies.
                    scope = Thoughts::anchorRemovedScope(scope, record);
                } else if (matches.isEmpty() && Thoughts::isAnchorRemovedScope(scope)) {
                    // Still no marker: the anchor-removed state passes through unchanged.
                } else {
                    scope.insert(QStringLiteral("state"), QStringLiteral("needs_attention"));
                    scope.insert(QStringLiteral("reason"), matches.size() > 1 ? QStringLiteral("source_marker_ambiguous") : QStringLiteral("source_marker_unavailable"));
                }
                output.remove(QStringLiteral("source_line"));
                output.insert(QStringLiteral("scope"), scope);
            }
            snapshot.insert(it.key(), QJsonObject{{QStringLiteral("input"), record}, {QStringLiteral("output"), output},
                {QStringLiteral("sources"), signature}});
        }
        // A comment explicitly deleted elsewhere can leave its marker in this
        // Mac's source. Past automatic deletions stay restorable and are skipped.
        QSet<QString> legacyAutoDeleted;
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (it.key().startsWith(QLatin1String("annotation\n")) && it.value().toObject().value(QStringLiteral("source_anchor_deletion")).toBool())
                legacyAutoDeleted.insert(it.key().section(QLatin1Char('\n'), 2));
        }
        QJsonArray cleanup;
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
            const auto record = it.value().toObject();
            if (!record.value(QStringLiteral("deleted")).toBool() || !record.value(QStringLiteral("parent_id")).toString().isEmpty()
                || record.value(QStringLiteral("subtype")).toString() == QLatin1String("Note")
                || record.value(QStringLiteral("source_authority")).toString() != QLatin1String("client-v1")
                || Thoughts::isAutomaticDeletionReason(record.value(QStringLiteral("deletion_reason")).toString())) continue;
            const auto commentId = record.value(QStringLiteral("comment_id")).toString();
            const auto scope = record.value(QStringLiteral("scope")).toObject();
            const auto id = scope.value(QStringLiteral("anchor_id")).toString(commentId);
            if (id.isEmpty() || legacyAutoDeleted.contains(commentId) || legacyAutoDeleted.contains(id)) continue;
            auto project = owners.value(scope.value(QStringLiteral("source_file")).toString());
            if (project.isEmpty()) {
                const auto candidates = targetProjects.value(record.value(QStringLiteral("origin")).toObject().value(QStringLiteral("collection")).toString() + QLatin1String(".pdf"));
                if (candidates.size() == 1) project = *candidates.cbegin();
            }
            if (project.isEmpty() || unreadableProjects.contains(project) || !(complete || settledProjects.contains(project))) continue;
            QJsonArray found;
            for (const auto &path : projects.value(project)) {
                for (const auto &value : files.value(path).toObject().value(QStringLiteral("markers")).toObject().value(id).toArray()) {
                    found.append(QJsonObject{{QStringLiteral("source_file"), path}, {QStringLiteral("line"), value.toObject().value(QStringLiteral("line"))},
                        {QStringLiteral("sha256"), files.value(path).toObject().value(QStringLiteral("sha256"))}});
                }
            }
            if (found.size() != 1) continue;
            auto candidate = found.first().toObject();
            candidate.insert(QStringLiteral("anchor_id"), id);
            candidate.insert(QStringLiteral("comment_id"), commentId);
            cleanup.append(candidate);
        }
        // Markers whose comment no longer exists: listed under Issues as "Comment missing".
        QJsonArray missing;
        if (storeLoaded) {
            const QSet<QString> live = Thoughts::liveCommentMarkerIdentities(input);
            for (auto it = files.begin(); it != files.end(); ++it) {
                const QString path = it.key();
                const QString project = owners.value(path);
                if (project.isEmpty() || unreadableProjects.contains(project) || !(complete || settledProjects.contains(project))) continue;
                if (path.startsWith(QLatin1String("tex/generated/")) || path.contains(QLatin1String("/tex/generated/"))) continue;
                const auto indexed = it.value().toObject();
                const auto markers = indexed.value(QStringLiteral("markers")).toObject();
                for (auto marker = markers.begin(); marker != markers.end(); ++marker) {
                    if (live.contains(Thoughts::commentMarkerIdentity(marker.key()))) continue;
                    const QJsonObject deleted = Thoughts::deletedCommentForMarker(input, marker.key());
                    for (const auto &value : marker.value().toArray()) {
                        const auto found = value.toObject();
                        const QString kind = found.value(QStringLiteral("kind")).toString();
                        if (!Thoughts::isCommentMarkerKind(kind)) continue;
                        QJsonObject entry{{QStringLiteral("anchor_id"), marker.key()}, {QStringLiteral("source_file"), path},
                            {QStringLiteral("line"), found.value(QStringLiteral("line"))}, {QStringLiteral("contents"), found.value(QStringLiteral("contents"))},
                            {QStringLiteral("kind"), kind}, {QStringLiteral("sha256"), indexed.value(QStringLiteral("sha256"))}, {QStringLiteral("project"), project},
                            {QStringLiteral("targets"), QJsonArray::fromStringList(fileTargets.value(path))},
                            {QStringLiteral("state"), deleted.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("deleted")}};
                        if ((kind == QLatin1String("highlight") || kind == QLatin1String("strikethrough")) && found.contains(QStringLiteral("selected_text")))
                            entry.insert(QStringLiteral("selected_text"), found.value(QStringLiteral("selected_text")));
                        if (!deleted.isEmpty()) {
                            entry.insert(QStringLiteral("comment_id"), deleted.value(QStringLiteral("comment_id")));
                            entry.insert(QStringLiteral("deletion_reason"), deleted.value(QStringLiteral("deletion_reason")).toString());
                        }
                        missing.append(entry);
                    }
                }
            }
        }
        // The tracker is the only judge of whether sources changed: a change it
        // has not processed yet schedules a reconciliation, which bumps the
        // generation checked below, and its completion refreshes this snapshot.
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, snapshot, files, input, locationIndex, cleanup, missing] {
            if (!guard) return;
            guard->m_sourceCommentRunning = false;
            if (generation != guard->m_sourceCommentGeneration || input != guard->effectiveProjections(true, false)) {
                guard->scheduleSourceCommentRefresh();
                return;
            }
            const bool locationsChanged = Thoughts::sourceLocationIndex(guard->m_docsRoot) != locationIndex;
            Thoughts::installSourceLocationIndex(guard->m_docsRoot, locationIndex, files);
            if (locationsChanged || snapshot != guard->m_sourceCommentSnapshot || files != guard->m_sourceCommentFileIndex || missing != guard->m_missingCommentMarkers) {
                guard->m_missingCommentMarkers = missing;
                guard->m_sourceCommentFileIndex = files;
                guard->m_sourceCommentSnapshot = snapshot;
                writeJsonFile(QDir(guard->m_stateRoot).filePath(QStringLiteral("source-comment-snapshot.json")),
                    QJsonDocument(QJsonObject{{QStringLiteral("docs_root"), guard->m_docsRoot}, {QStringLiteral("entries"), snapshot}, {QStringLiteral("files"), files}}).toJson(QJsonDocument::Compact));
                Q_EMIT guard->projectionsChanged(guard->effectiveProjections(), guard->m_outbox->cursor());
                guard->schedulePendingChangesRefresh();
            }
            guard->cleanupDeletedCommentAnchors(cleanup);
        }, Qt::QueuedConnection);
    });
}

QSet<QString> FedoraRepository::trackedCommentSourceFiles() const
{
    QSet<QString> files;
    if (!m_sourceChangeTracker) return files;
    for (const auto &path : m_sourceChangeTracker->trackedPaths()) {
        if (!path.exists || path.affectedTargets.isEmpty() || path.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("ignored")) continue;
        if (path.path.endsWith(QLatin1String(".tex")) || path.path.endsWith(QLatin1String(".md")) || path.path.endsWith(QLatin1String(".markdown")))
            files.insert(QDir(m_docsRoot).filePath(path.path));
    }
    return files;
}

void FedoraRepository::cleanupDeletedCommentAnchors(const QJsonArray &candidates)
{
    if (candidates.isEmpty()) return;
    const QString statePath = QDir(m_stateRoot).filePath(QStringLiteral("source-comment-cleanup.json"));
    QFile stateFile(statePath);
    QJsonObject state = stateFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(stateFile.readAll()).object() : QJsonObject();
    stateFile.close();
    QJsonObject failures = state.value(QStringLiteral("failures")).toObject();
    bool stateChanged = false;
    bool removed = false;
    // Our own removals change a file's hash; later candidates in it expect the new one.
    QHash<QString, QString> rewritten;
    for (const auto &value : candidates) {
        const auto candidate = value.toObject();
        const QString id = candidate.value(QStringLiteral("anchor_id")).toString();
        const QString relative = candidate.value(QStringLiteral("source_file")).toString();
        const QString scannedSha256 = candidate.value(QStringLiteral("sha256")).toString();
        const QString sha256 = rewritten.value(relative, scannedSha256);
        const QString path = QDir(m_docsRoot).filePath(relative);
        // A failure is reported once per source version instead of retried in a loop.
        if (failures.value(id).toObject().value(QStringLiteral("sha256")).toString() == sha256) continue;
        // The scan is stale if the file changed meanwhile; the next refresh reconsiders it.
        if (QString::fromLatin1(Thoughts::artifactFileSha256(path)) != sha256) continue;
        QString error;
        if (Okular::DocsCommentSynchronizer::removeCommentAnchor(path, id, &error)) {
            qInfo().noquote() << QStringLiteral("Removed the leftover anchor of deleted comment %1 from %2:%3.").arg(id, relative).arg(candidate.value(QStringLiteral("line")).toInt());
            stateChanged = failures.contains(id) || stateChanged;
            failures.remove(id);
            removed = true;
            rewritten.insert(relative, QString::fromLatin1(Thoughts::artifactFileSha256(path)));
        } else {
            qWarning().noquote() << QStringLiteral("Could not remove the leftover anchor of deleted comment %1 from %2: %3").arg(id, relative, error);
            failures.insert(id, QJsonObject{{QStringLiteral("sha256"), sha256}, {QStringLiteral("source_file"), relative}, {QStringLiteral("error"), error},
                {QStringLiteral("attempted_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
            stateChanged = true;
        }
    }
    if (stateChanged) {
        state.insert(QStringLiteral("schema_version"), 1);
        state.insert(QStringLiteral("failures"), failures);
        writeJsonFile(statePath, QJsonDocument(state).toJson(QJsonDocument::Compact));
    }
    // The source tracker publishes the edit with the next Sync, like a local delete's anchor removal.
    if (removed) {
        scheduleSourceCommentRefresh();
        schedulePendingChangesRefresh();
    }
}

QString FedoraRepository::proposalDecisionDatabase() const
{
    return m_outbox ? m_outbox->databasePath() : QDir(m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite"));
}

QVariantList FedoraRepository::restoreRemovedProposals(const QStringList &ids)
{
    QVariantList results;
    const QJsonObject projections = effectiveProjections();
    bool changed = false;
    for (const QString &id : ids) {
        const QJsonObject row = projections.value(QStringLiteral("proposal\nproposals\n") + id).toObject();
        QJsonObject result = Thoughts::ProposalDecisions::restoreRemovedProposal(m_docsRoot, row);
        result.insert(QStringLiteral("id"), id);
        changed = changed || (result.value(QStringLiteral("success")).toBool() && result.value(QStringLiteral("code")).toString().isEmpty());
        results.append(result.toVariantMap());
    }
    // A pure source edit: the tracker publishes it with the next Sync, and the
    // server sets the proposal pending when it sees the wrapper again.
    if (changed) schedulePendingChangesRefresh();
    return results;
}

QVariantList FedoraRepository::dismissRemovedProposals(const QStringList &ids)
{
    QVariantList results;
    const QJsonObject projections = effectiveProjections();
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    bool queued = false;
    for (const QString &id : ids) {
        const QJsonObject row = projections.value(QStringLiteral("proposal\nproposals\n") + id).toObject();
        QVariantMap result{{QStringLiteral("id"), id}, {QStringLiteral("success"), false}};
        const qint64 revision = row.value(QStringLiteral("revision")).toInteger(-1);
        if (!Thoughts::ProposalDecisions::isRemovedOutside(row) || !safeComponent(id) || revision < 1) {
            result.insert(QStringLiteral("message"), QStringLiteral("Proposal %1 is no longer removed outside Thoughts.").arg(id));
            results.append(result);
            continue;
        }
        // §10.6: the only proposal operation a client sends; offline it waits in the outbox.
        const QJsonObject operation = m_outbox->enqueue(QStringLiteral("proposal"), id, QStringLiteral("proposals"), revision,
                                                        Thoughts::ProposalDecisions::dismissedPayload(row), now);
        const bool success = !operation.isEmpty();
        queued = queued || success;
        result.insert(QStringLiteral("success"), success);
        result.insert(QStringLiteral("message"), success ? QString() : QStringLiteral("The Dismiss of proposal %1 could not be queued.").arg(id));
        results.append(result);
    }
    if (queued) {
        emitEffectiveProjections();
        schedulePendingChangesRefresh();
    }
    return results;
}

QVariantList FedoraRepository::restoreRemovedAnchors(const QStringList &ids)
{
    QVariantList results;
    const auto projections = effectiveProjections();
    const auto durable = effectiveProjections(true, false);
    Okular::DocsCommentSynchronizer synchronizer(localSourceOptions(m_docsRoot, trackedCommentSourceFiles()));
    bool changed = false;
    for (const QString &id : ids) {
        const QString key = QStringLiteral("logical_comment\ncomments\n") + id;
        QJsonObject payload = projections.value(key).toObject();
        if (!Thoughts::isAnchorRemovedScope(payload.value(QStringLiteral("scope")).toObject())
            && Thoughts::isAnchorRemovedScope(durable.value(key).toObject().value(QStringLiteral("scope")).toObject()))
            payload = durable.value(key).toObject();
        QVariantMap entry{{QStringLiteral("id"), id}, {QStringLiteral("success"), false}};
        if (payload.isEmpty() || payload.value(QStringLiteral("deleted")).toBool()) {
            entry.insert(QStringLiteral("message"), QStringLiteral("The comment is no longer available."));
        } else if (!Thoughts::isAnchorRemovedScope(payload.value(QStringLiteral("scope")).toObject())) {
            entry.insert(QStringLiteral("message"), QStringLiteral("The comment's anchor was not removed."));
        } else {
            const auto restored = synchronizer.restoreRemovedAnchor(payload);
            using Placement = Okular::DocsCommentSynchronizer::AnchorRestoreResult::Placement;
            static const QHash<Placement, QString> placements{{Placement::Exact, QStringLiteral("exact")}, {Placement::NearestLine, QStringLiteral("nearest_line")},
                {Placement::EndOfFile, QStringLiteral("end_of_file")}, {Placement::FallbackFile, QStringLiteral("fallback_file")}, {Placement::AlreadyPresent, QStringLiteral("already_present")}};
            entry.insert(QStringLiteral("success"), restored.success);
            entry.insert(QStringLiteral("message"), restored.message);
            entry.insert(QStringLiteral("source_file"), restored.sourceFile);
            entry.insert(QStringLiteral("line"), restored.line);
            entry.insert(QStringLiteral("requested_line"), restored.requestedLine);
            entry.insert(QStringLiteral("placement"), placements.value(restored.placement));
            entry.insert(QStringLiteral("point_anchor_fallback"), restored.pointAnchorFallback);
            changed = changed || (restored.success && restored.placement != Placement::AlreadyPresent);
        }
        results.append(entry);
    }
    // The overlay shows the comment linked again once the tracker re-reads the source.
    if (changed) {
        scheduleSourceCommentRefresh();
        schedulePendingChangesRefresh();
    }
    return results;
}

namespace
{
// The managed source a "Comment missing" action may edit, or an empty path and a reason.
QString missingMarkerSourcePath(const QString &docsRoot, const QSet<QString> &tracked, const QString &relative, QString *reason)
{
    const QString cleaned = QDir::cleanPath(relative);
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || cleaned.startsWith(QLatin1String("../")) || cleaned == QLatin1String("..")
        || cleaned.startsWith(QLatin1String("tex/generated/")) || cleaned.contains(QLatin1String("/tex/generated/"))) {
        *reason = QStringLiteral("%1 is not a managed source.").arg(relative);
        return {};
    }
    const QString path = QDir(docsRoot).filePath(cleaned);
    if (!tracked.contains(path)) {
        *reason = QStringLiteral("%1 is not a managed source.").arg(relative);
        return {};
    }
    return path;
}
}

QVariantList FedoraRepository::removeMissingCommentMarkers(const QVariantList &markers)
{
    QVariantList results;
    const QSet<QString> tracked = trackedCommentSourceFiles();
    const QSet<QString> live = Thoughts::liveCommentMarkerIdentities(effectiveProjections(true, false));
    bool changed = false;
    for (const QVariant &value : markers) {
        const QVariantMap marker = value.toMap();
        const QString id = marker.value(QStringLiteral("anchor_id")).toString();
        const QString relative = marker.value(QStringLiteral("source_file")).toString();
        QVariantMap entry{{QStringLiteral("anchor_id"), id}, {QStringLiteral("source_file"), relative}, {QStringLiteral("success"), false}};
        QString reason;
        const QString path = missingMarkerSourcePath(m_docsRoot, tracked, relative, &reason);
        if (id.isEmpty()) {
            entry.insert(QStringLiteral("message"), QStringLiteral("The marker has no id."));
        } else if (path.isEmpty()) {
            entry.insert(QStringLiteral("message"), reason);
        } else if (live.contains(Thoughts::commentMarkerIdentity(id))) {
            entry.insert(QStringLiteral("message"), QStringLiteral("The comment of the marker in %1 exists again; the marker was kept.").arg(relative));
        } else if (QString error; Okular::DocsCommentSynchronizer::removeMissingCommentMarker(path, id, &error)) {
            entry.insert(QStringLiteral("success"), true);
            entry.insert(QStringLiteral("message"), QStringLiteral("Marker removed from %1.").arg(relative));
            changed = true;
        } else {
            entry.insert(QStringLiteral("message"), QStringLiteral("%1: %2").arg(relative, error));
        }
        results.append(entry);
    }
    // The source tracker publishes the edit with the next Sync.
    if (changed) {
        scheduleSourceCommentRefresh();
        schedulePendingChangesRefresh();
    }
    return results;
}

QVariantList FedoraRepository::restoreMissingComments(const QVariantList &markers, const QString &author)
{
    QVariantList results;
    const QSet<QString> tracked = trackedCommentSourceFiles();
    QHash<QString, QStringList> targetsByPath;
    if (m_sourceChangeTracker) {
        for (const auto &path : m_sourceChangeTracker->trackedPaths()) {
            QStringList targets(path.affectedTargets.cbegin(), path.affectedTargets.cend());
            targets.sort();
            targetsByPath.insert(QDir(m_docsRoot).filePath(path.path), targets);
        }
    }
    bool changed = false;
    for (const QVariant &value : markers) {
        const QVariantMap marker = value.toMap();
        const QString id = marker.value(QStringLiteral("anchor_id")).toString();
        const QString relative = marker.value(QStringLiteral("source_file")).toString();
        QVariantMap entry{{QStringLiteral("anchor_id"), id}, {QStringLiteral("source_file"), relative}, {QStringLiteral("success"), false}};
        const auto fail = [&](const QString &message) {
            entry.insert(QStringLiteral("message"), message);
            results.append(entry);
        };
        // Earlier entries of this batch are already in the outbox.
        const QJsonObject durable = effectiveProjections(true, false);
        QString reason;
        const QString path = missingMarkerSourcePath(m_docsRoot, tracked, relative, &reason);
        if (id.isEmpty() || !safeComponent(id)) { fail(QStringLiteral("The marker id cannot name a comment.")); continue; }
        if (path.isEmpty()) { fail(reason); continue; }
        if (Thoughts::liveCommentMarkerIdentities(durable).contains(Thoughts::commentMarkerIdentity(id))) {
            fail(QStringLiteral("The comment of the marker in %1 already exists.").arg(relative));
            continue;
        }
        // The comment is recreated from the marker as it is in the source now.
        const QJsonArray found = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {}).value(QStringLiteral("markers")).toObject().value(id).toArray();
        if (found.size() != 1 || !Thoughts::isCommentMarkerKind(found.first().toObject().value(QStringLiteral("kind")).toString())) {
            fail(found.isEmpty() ? QStringLiteral("The marker is no longer in %1.").arg(relative)
                                 : QStringLiteral("The marker appears %1 times in %2; remove the extra copies first.").arg(found.size()).arg(relative));
            continue;
        }
        const QJsonObject current = found.first().toObject();
        const QString kind = current.value(QStringLiteral("kind")).toString();
        const bool markup = kind == QLatin1String("highlight") || kind == QLatin1String("strikethrough");
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QJsonObject scope{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), id},
            {QStringLiteral("source_file"), QDir::cleanPath(relative)}, {QStringLiteral("source_line"), current.value(QStringLiteral("line")).toInt()},
            {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}};
        const QJsonObject deleted = Thoughts::deletedCommentForMarker(durable, id);
        QJsonObject payload;
        if (!deleted.isEmpty()) {
            // Undelete: the same comment, one revision later (base_revision = its revision).
            if (Thoughts::deletedCommentBlocksRestore(deleted)) {
                fail(QStringLiteral("The comment's document was deleted, so the server does not restore it. Remove the marker instead."));
                continue;
            }
            const qint64 revision = deleted.value(QStringLiteral("revision")).toInteger();
            if (revision < 1) { fail(QStringLiteral("The deleted comment has no valid revision; Sync and try again.")); continue; }
            payload = deleted;
            for (const QString &key : {QStringLiteral("deletion_reason"), QStringLiteral("document_deletion"), QStringLiteral("source_deletion")}) payload.remove(key);
            payload.insert(QStringLiteral("deleted"), false);
            payload.insert(QStringLiteral("revision"), revision + 1);
            payload.insert(QStringLiteral("updated_at"), now);
            payload.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
            payload.insert(QStringLiteral("contents"), current.value(QStringLiteral("contents")));
            if (markup) payload.insert(QStringLiteral("selected_text"), current.value(QStringLiteral("selected_text")));
            payload.insert(QStringLiteral("scope"), scope);
            entry.insert(QStringLiteral("operation"), QStringLiteral("undelete"));
        } else {
            // Unknown id: a new root comment with the marker's id, on its document's current PDF.
            const QStringList targets = targetsByPath.value(path);
            QJsonObject identity;
            QString target;
            for (const QString &candidate : targets) {
                identity = Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(candidate);
                if (Thoughts::DocumentIdentityStore::validId(identity.value(QStringLiteral("document_id")).toString())
                    && validSha256(identity.value(QStringLiteral("pdf_sha256")).toString())) { target = candidate; break; }
            }
            if (target.isEmpty()) {
                fail(QStringLiteral("The document of %1 is not in the Library yet; Sync before restoring this comment.").arg(relative));
                continue;
            }
            payload = QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id}, {QStringLiteral("thread_root_id"), id},
                {QStringLiteral("parent_id"), QJsonValue::Null}, {QStringLiteral("revision"), 1}, {QStringLiteral("created_at"), now}, {QStringLiteral("updated_at"), now},
                {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()}, {QStringLiteral("deleted"), false},
                {QStringLiteral("contents"), current.value(QStringLiteral("contents"))}, {QStringLiteral("author"), author},
                {QStringLiteral("subtype"), kind == QLatin1String("highlight") ? QStringLiteral("Highlight") : kind == QLatin1String("strikethrough") ? QStringLiteral("StrikeOut") : QStringLiteral("Text")},
                {QStringLiteral("priority"), QStringLiteral("normal")}, {QStringLiteral("scope"), scope},
                {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QFileInfo(target).completeBaseName()},
                    {QStringLiteral("document_id"), identity.value(QStringLiteral("document_id"))}, {QStringLiteral("pdf_sha256"), identity.value(QStringLiteral("pdf_sha256"))},
                    {QStringLiteral("page_index"), 0}, {QStringLiteral("rect"), QJsonArray()}, {QStringLiteral("fragments"), QJsonArray()},
                    {QStringLiteral("geometry_format"), QStringLiteral("pdf_points_bottom_left")}, {QStringLiteral("raw_pdf_geometry"), QJsonObject()}}}};
            if (markup) payload.insert(QStringLiteral("selected_text"), current.value(QStringLiteral("selected_text")));
            entry.insert(QStringLiteral("operation"), QStringLiteral("create"));
        }
        if (!enqueueLogicalComment(payload)) {
            fail(QStringLiteral("The comment could not be saved to the local sync queue."));
            continue;
        }
        entry.insert(QStringLiteral("success"), true);
        entry.insert(QStringLiteral("comment_id"), payload.value(QStringLiteral("comment_id")));
        entry.insert(QStringLiteral("message"), deleted.isEmpty() ? QStringLiteral("Comment recreated from the marker in %1.").arg(relative)
                                                                  : QStringLiteral("Comment restored from the marker in %1.").arg(relative));
        results.append(entry);
        changed = true;
    }
    if (changed) scheduleSourceCommentRefresh();
    return results;
}

QJsonObject FedoraRepository::reviewProjections(const QJsonObject &projections)
{
    QJsonObject reviews;
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("review_card\ncards\n"))
            || it.key().startsWith(QLatin1String("review_event\nevents\n"))) reviews.insert(it.key(), it.value());
    }
    return reviews;
}

bool FedoraRepository::affectsPendingChanges(const QString &path) const
{
    const QString root = QDir::cleanPath(m_docsRoot);
    if (path == QDir::cleanPath(Thoughts::LibraryProjectStore(m_docsRoot).path())) {
        return true;
    }
    // current_pdfs is atomically replaced when imports/removals activate; the
    // source index inside a published generation describes source coverage.
    if (path == root + QStringLiteral("/current_pdfs")) {
        return true;
    }
    const QFileInfo info(path);
    return info.fileName() == QLatin1String(".thoughts-source-index.json") && QFileInfo(info.absolutePath()).absolutePath() == root;
}

void FedoraRepository::handleDesktopSyncStoreChanged(const QString &databasePath)
{
    const QString outboxDatabase = m_outbox ? m_outbox->databasePath() : QDir(m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite"));
    if (QDir::cleanPath(databasePath) == QDir::cleanPath(outboxDatabase)) {
        schedulePendingChangesRefresh();
    }
}

void FedoraRepository::schedulePendingChangesRefresh()
{
    if (m_pendingChangesTimer) {
        m_pendingChangesTimer->start();
    }
}

void FedoraRepository::refreshPendingChanges()
{
    // Review records can change while the number of pending operations stays the same.
    // Emit their effective snapshot so the desktop SQL index includes local reviews.
    const QJsonObject projections = effectiveProjections();
    const QJsonObject reviews = reviewProjections(projections);
    // Compare directly: serializing and hashing every review record on each
    // refresh cost seconds of UI time during Sync.
    if (!m_reviewProjectionsKnown || reviews != m_reviewProjections) {
        m_reviewProjections = reviews;
        m_reviewProjectionsKnown = true;
        Q_EMIT projectionsChanged(projections, m_outbox->cursor());
    }
    const QJsonObject snapshot = calculatePendingChanges();
    if (snapshot == m_pendingChangesSnapshot) {
        return;
    }
    m_pendingChangesSnapshot = snapshot;
    Q_EMIT pendingChangesChanged(snapshot);
}

QJsonObject FedoraRepository::agentProposalBundle() const
{
    QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
    if (!QFileInfo::exists(bundleRoot.filePath(QStringLiteral("documents")))) {
        bundleRoot = QDir(QDir(m_docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
    }
    QJsonArray proposals;
    const QJsonObject resolvedProposals = readRepositoryState().value(QStringLiteral("resolved_agent_proposals")).toObject();
    const QDir documents(bundleRoot.filePath(QStringLiteral("documents")));
    for (const QFileInfo &pointerInfo : documents.entryInfoList({QStringLiteral("*.pdf.json")}, QDir::Files, QDir::Name)) {
        QFile pointerFile(pointerInfo.absoluteFilePath());
        if (!pointerFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject pointer = QJsonDocument::fromJson(pointerFile.readAll()).object();
        const QString artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
        const QString filename = pointer.value(QStringLiteral("filename")).toString();
        const QString pdfSha256 = pointer.value(QStringLiteral("pdf_sha256")).toString();
        if (pointer.value(QStringLiteral("schema_version")).toInt() != 1 || !safePdfFilename(filename) || !validSha256(artifactSha256) || !validSha256(pdfSha256) || QFileInfo(pointerInfo).fileName() != filename + QStringLiteral(".json") ||
            fileSha256(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename))) != pdfSha256.toLatin1()) {
            continue;
        }
        QFile manifestFile(bundleRoot.filePath(QStringLiteral("objects/%1").arg(artifactSha256)));
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray contents = manifestFile.readAll();
        const QJsonObject manifest = QJsonDocument::fromJson(contents).object();
        const QJsonObject publication {{QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), pdfSha256}, {QStringLiteral("source_artifact_sha256"), artifactSha256}};
        if (!documentSourceArtifactMatchesPublication(contents, publication)) {
            continue;
        }
        QHash<QString, QString> proposalObjects;
        for (const QJsonValue &value : manifest.value(QStringLiteral("proposal_objects")).toArray()) {
            const QJsonObject reference = value.toObject();
            proposalObjects.insert(reference.value(QStringLiteral("id")).toString(), reference.value(QStringLiteral("object")).toString());
        }
        for (const QJsonValue &value : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
            QJsonObject proposal = value.toObject();
            if (resolvedProposals.contains(proposal.value(QStringLiteral("id")).toString())) {
                continue;
            }
            const QString objectSha256 = proposalObjects.value(proposal.value(QStringLiteral("id")).toString());
            if (!validSha256(objectSha256)) {
                continue;
            }
            proposal.insert(QStringLiteral("artifact_sha256"), artifactSha256);
            proposal.insert(QStringLiteral("proposal_object_sha256"), objectSha256);
            proposals.append(proposal);
        }
    }
    return QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact-set")}, {QStringLiteral("agent_proposals"), proposals}};
}

QString FedoraRepository::resolveAgentProposals(const QString &bundleId, const QString &action, const QStringList &proposalIds)
{
    static const QRegularExpression proposalId(QStringLiteral("^[a-z0-9][a-z0-9._:-]{0,127}$"));
    QSet<QString> uniqueIds;
    for (const QString &id : proposalIds) {
        if (!proposalId.match(id).hasMatch() || uniqueIds.contains(id)) {
            return {};
        }
        uniqueIds.insert(id);
    }
    if (!validSha256(bundleId) || (action != QLatin1String("accept") && action != QLatin1String("disregard")) || uniqueIds.isEmpty() || uniqueIds.size() > 500) {
        return {};
    }
    QJsonArray proposalObjects;
    const QJsonArray available = agentProposalBundle().value(QStringLiteral("agent_proposals")).toArray();
    for (const QString &id : proposalIds) {
        const auto found = std::find_if(available.begin(), available.end(), [&id, &bundleId](const QJsonValue &value) {
            const QJsonObject proposal = value.toObject();
            return proposal.value(QStringLiteral("id")).toString() == id && proposal.value(QStringLiteral("artifact_sha256")).toString() == bundleId;
        });
        if (found == available.end()) {
            return {};
        }
        const QString objectSha256 = found->toObject().value(QStringLiteral("proposal_object_sha256")).toString();
        if (!validSha256(objectSha256)) {
            return {};
        }
        proposalObjects.append(QJsonObject {{QStringLiteral("id"), id}, {QStringLiteral("object_sha256"), objectSha256}});
    }
    const QJsonObject operation = m_outbox->enqueueAgentProposalResolution(bundleId, action, proposalObjects);
    if (operation.isEmpty()) {
        return {};
    }
    schedulePendingChangesRefresh();
    return operation.value(QStringLiteral("operation_id")).toString();
}

bool FedoraRepository::persistAppliedAgentProposalOperation(const QJsonObject &operation)
{
    const QString kind = operation.value(QStringLiteral("outbox_kind")).toString();
    QJsonObject state = readRepositoryState();
    QJsonObject resolved = state.value(QStringLiteral("resolved_agent_proposals")).toObject();
    if (kind == QLatin1String("agent_proposal_resolution")) {
        const QString operationId = operation.value(QStringLiteral("operation_id")).toString();
        const QString artifactSha256 = operation.value(QStringLiteral("artifact_sha256")).toString();
        const QString action = operation.value(QStringLiteral("action")).toString();
        for (const QJsonValue &value : operation.value(QStringLiteral("proposals")).toArray()) {
            const QString proposalId = value.toObject().value(QStringLiteral("id")).toString();
            if (!proposalId.isEmpty()) {
                resolved.insert(proposalId, QJsonObject {{QStringLiteral("operation_id"), operationId}, {QStringLiteral("artifact_sha256"), artifactSha256}, {QStringLiteral("action"), action}});
            }
        }
    } else if (kind == QLatin1String("agent_proposal_undo")) {
        const QString undoOf = operation.value(QStringLiteral("undo_of")).toString();
        for (auto iterator = resolved.begin(); iterator != resolved.end();) {
            if (iterator.value().toObject().value(QStringLiteral("operation_id")).toString() == undoOf) {
                iterator = resolved.erase(iterator);
            } else {
                ++iterator;
            }
        }
    } else {
        return false;
    }
    if (resolved.isEmpty()) {
        state.remove(QStringLiteral("resolved_agent_proposals"));
    } else {
        state.insert(QStringLiteral("resolved_agent_proposals"), resolved);
    }
    return writeRepositoryState(state);
}

QString FedoraRepository::undoAgentProposalResolution(const QString &operationId)
{
    if (!safeComponent(operationId)) {
        return {};
    }
    const QJsonObject operation = m_outbox->enqueueAgentProposalUndo(operationId);
    if (operation.isEmpty()) {
        return {};
    }
    schedulePendingChangesRefresh();
    return operation.value(QStringLiteral("operation_id")).toString();
}

void FedoraRepository::requestSourceConflictResolutionBundle(const QString &path)
{
    if (m_sourceConflictBundleRequestRunning) {
        Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("A source conflict is already being prepared."));
        return;
    }
    if (!ensureClient()) {
        Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("The server is not configured or its credentials are unavailable."));
        return;
    }

    QFile stateFile(QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")));
    if (!stateFile.open(QIODevice::ReadOnly)) {
        Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("The local source-publication state is unavailable."));
        return;
    }
    const QJsonDocument stateDocument = QJsonDocument::fromJson(stateFile.readAll());
    const QJsonObject localConflicts = stateDocument.object().value(QStringLiteral("conflicts")).toObject();
    if (!stateDocument.isObject() || localConflicts.isEmpty()) {
        Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("No unresolved local source conflict is recorded."));
        return;
    }

    m_sourceConflictBundleRequestRunning = true;
    QNetworkReply *reply = m_client->conflicts();
    connect(reply, &QNetworkReply::finished, this, [this, reply, localConflicts, requestedPath = path] {
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo networkError = replyErrorInfo(reply);
            reply->deleteLater();
            m_sourceConflictBundleRequestRunning = false;
            Q_EMIT sourceConflictResolutionBundleReady({}, networkError.summary);
            return;
        }
        const QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!response.isArray()) {
            m_sourceConflictBundleRequestRunning = false;
            Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("The server returned an invalid source-conflict list."));
            return;
        }

        QJsonObject selected;
        QJsonObject localState;
        for (auto iterator = localConflicts.constBegin(); iterator != localConflicts.constEnd() && selected.isEmpty(); ++iterator) {
            if (!requestedPath.isEmpty() && iterator.key() != requestedPath) {
                continue;
            }
            const QString conflictId = iterator.value().toObject().value(QStringLiteral("conflict_id")).toString();
            for (const QJsonValue &value : response.array()) {
                const QJsonObject candidate = value.toObject();
                if (candidate.value(QStringLiteral("id")).toString() == conflictId && candidate.value(QStringLiteral("path")).toString() == iterator.key() && candidate.value(QStringLiteral("status")).toString() == QLatin1String("unresolved")) {
                    selected = candidate;
                    localState = iterator.value().toObject();
                    break;
                }
            }
        }
        const QString path = selected.value(QStringLiteral("path")).toString();
        const QString conflictId = selected.value(QStringLiteral("id")).toString();
        const qint64 expectedRevision = selected.value(QStringLiteral("expected_revision")).toInteger(-1);
        const QString baseSha256 = selected.value(QStringLiteral("base_sha256")).toString();
        const QString currentSha256 = selected.value(QStringLiteral("current_sha256")).toString();
        const QString incomingSha256 = selected.value(QStringLiteral("incoming_sha256")).toString();
        const QString mergedSha256 = selected.value(QStringLiteral("merged_object_sha256")).toString();
        // Any editable source can conflict (.css, .bib, .sty ...), not only
        // documents; a conflict on one of them must stay resolvable.
        if (selected.isEmpty() || !safeTrackedPath(path) || path.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\\\]"))) || !safeComponent(conflictId) || expectedRevision < 0 || !validSha256(baseSha256) || !validSha256(currentSha256) || !validSha256(incomingSha256)
            || (!mergedSha256.isEmpty() && !validSha256(mergedSha256))) {
            m_sourceConflictBundleRequestRunning = false;
            Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("The recorded source conflict does not match a valid unresolved conflict on the server."));
            return;
        }
        if (localState.value(QStringLiteral("server_revision")).toInteger(-1) != expectedRevision || localState.value(QStringLiteral("local_sha256")).toString() != incomingSha256) {
            m_sourceConflictBundleRequestRunning = false;
            Q_EMIT sourceConflictResolutionBundleReady({}, QStringLiteral("The local and server source-conflict identities no longer match."));
            return;
        }

        struct DownloadState {
            QJsonObject bundle;
            int pending = 0;
            QString error;
        };
        const auto downloads = QSharedPointer<DownloadState>::create();
        downloads->bundle = selected;
        downloads->bundle.insert(QStringLiteral("local_working_path"), QDir(m_docsRoot).filePath(path));
        const QList<QPair<QString, QString>> objects {
            {QStringLiteral("base_content_base64"), baseSha256},
            {QStringLiteral("fedora_content_base64"), currentSha256},
            {QStringLiteral("mac_content_base64"), incomingSha256},
            {QStringLiteral("merged_content_base64"), mergedSha256},
        };
        for (const auto &[key, digest] : objects) {
            if (!digest.isEmpty()) {
                ++downloads->pending;
                QNetworkReply *objectReply = m_client->downloadObject(digest.toLatin1());
                connect(objectReply, &QNetworkReply::finished, this, [this, objectReply, downloads, key, digest] {
                    if (objectReply->error() != QNetworkReply::NoError) {
                        if (downloads->error.isEmpty()) {
                            downloads->error = replyErrorInfo(objectReply).summary;
                        }
                    } else {
                        const QByteArray contents = objectReply->readAll();
                        if (!Thoughts::SyncClient::verifyObject(contents, digest.toLatin1())) {
                            downloads->error = QStringLiteral("A downloaded source-conflict object failed its identity check.");
                        } else {
                            downloads->bundle.insert(key, QString::fromLatin1(contents.toBase64()));
                        }
                    }
                    objectReply->deleteLater();
                    if (--downloads->pending == 0) {
                        m_sourceConflictBundleRequestRunning = false;
                        Q_EMIT sourceConflictResolutionBundleReady(downloads->error.isEmpty() ? downloads->bundle : QJsonObject(), downloads->error);
                    }
                });
            }
        }
    });
}

QString FedoraRepository::queueSourceConflictResolution(const QString &conflictId,
                                                        qint64 expectedRevision,
                                                        const QString &expectedSha256,
                                                        const QByteArray &contents)
{
    if (!safeComponent(conflictId) || expectedRevision < 0 || !validSha256(expectedSha256) || contents.size() > 4 * 1024 * 1024 || contents.contains('\0')) {
        return {};
    }
    const QString decoded = QString::fromUtf8(contents);
    // Only a marker line is a merge marker: a LaTeX "% ====...====" rule
    // in both versions made every resolution of that source unsavable.
    static const QRegularExpression marker(QStringLiteral("(^|\\n)(<<<<<<<|=======|>>>>>>>)(?: |$|\\n)"));
    if (decoded.toUtf8() != contents || marker.match(decoded).hasMatch()) {
        return {};
    }
    // Decision claims held back by the conflict travel with its resolution (§10.8).
    QJsonArray proposalDecisions;
    QFile stateFile(QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")));
    if (stateFile.open(QIODevice::ReadOnly)) {
        const QJsonObject conflicts = QJsonDocument::fromJson(stateFile.readAll()).object().value(QStringLiteral("conflicts")).toObject();
        for (auto it = conflicts.begin(); it != conflicts.end(); ++it) {
            if (it.value().toObject().value(QStringLiteral("conflict_id")).toString() != conflictId) continue;
            QString claimError;
            for (const QJsonObject &claim : Thoughts::ProposalDecisions::claimsForConflictResolution(proposalDecisionDatabase(), it.key(), decoded, &claimError))
                proposalDecisions.append(claim);
            if (!claimError.isEmpty()) qWarning().noquote() << "Proposal decisions for the conflict resolution:" << claimError;
        }
    }
    const QJsonObject operation = m_outbox->enqueueSourceConflictResolution(conflictId,
                                                                            expectedRevision,
                                                                            expectedSha256,
                                                                            contents,
                                                                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                                                            proposalDecisions);
    if (operation.isEmpty()) {
        return {};
    }
    schedulePendingChangesRefresh();
    return operation.value(QStringLiteral("operation_id")).toString();
}

void FedoraRepository::emitEffectiveProjections()
{
    scheduleSourceCommentRefresh();
    Q_EMIT projectionsChanged(effectiveProjections(), m_outbox->cursor());
}

QJsonObject FedoraRepository::withRenderStatusSnapshot(QJsonObject state) const
{
    const QString path = QDir(m_stateRoot).filePath(QStringLiteral("render-status.json"));
    const QString identity = Thoughts::artifactFileIdentity(path);
    if (identity != m_cachedRenderStatusIdentity || path != m_cachedRenderStatusPath) {
        QFile file(path);
        m_cachedRenderStatusSnapshot = file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        m_cachedRenderStatusIdentity = identity;
        m_cachedRenderStatusPath = path;
    }
    const QJsonObject &snapshot = m_cachedRenderStatusSnapshot;
    if (snapshot.value(QStringLiteral("schema_version")).toInt() == 1) {
        state.insert(QStringLiteral("render_state"), snapshot.value(QStringLiteral("render_state")));
        state.insert(QStringLiteral("render_statuses"), snapshot.value(QStringLiteral("render_statuses")));
    }
    return state;
}

bool FedoraRepository::writeRenderStatusSnapshot(const QJsonObject &state) const
{
    const QJsonObject snapshot{{QStringLiteral("schema_version"), 1},
                               {QStringLiteral("render_state"), state.value(QStringLiteral("render_state"))},
                               {QStringLiteral("render_statuses"), state.value(QStringLiteral("render_statuses"))}};
    return writeJsonFile(QDir(m_stateRoot).filePath(QStringLiteral("render-status.json")),
                         QJsonDocument(snapshot).toJson(QJsonDocument::Compact) + '\n');
}

QJsonObject FedoraRepository::readRepositoryState() const
{
    const QString statePath = QDir(m_stateRoot).filePath(QStringLiteral("repository.json"));
    const QString legacyPath = QDir(m_stateRoot).filePath(QStringLiteral("mirror.json"));
    const QString selectedPath = QFileInfo::exists(statePath) ? statePath : legacyPath;
    const QFileInfo info(selectedPath);
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 birthMs = info.birthTime().toMSecsSinceEpoch();
    if (m_cachedRepositoryStateValid && selectedPath == m_cachedRepositoryStatePath && info.size() == m_cachedRepositoryStateSize && modifiedMs == m_cachedRepositoryStateModifiedMs && birthMs == m_cachedRepositoryStateBirthMs) {
        return withRenderStatusSnapshot(m_cachedRepositoryState);
    }
    QFile file(selectedPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_cachedRepositoryStateValid = false;
        return {{QStringLiteral("fingerprints"), QJsonObject()}, {QStringLiteral("pending_builds"), QJsonArray()}, {QStringLiteral("projections"), QJsonObject()}};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        m_cachedRepositoryStateValid = false;
        return {};
    }
    QJsonObject loaded = document.object();
    const qint64 projectionRevision = loaded.value(QStringLiteral("projection_store_revision")).toInteger();
    if (projectionRevision > 0) {
        Thoughts::RepositoryProjectionStore records(m_stateRoot);
        QJsonObject projections;
        if (!records.read(projectionRevision, &projections)) {
            m_cachedRepositoryStateValid = false;
            qWarning() << "Could not read the committed repository projection revision.";
            return {};
        }
        loaded.insert(QStringLiteral("projections"), projections);
    }
    m_cachedRepositoryState = loaded;
    m_cachedRepositoryStatePath = selectedPath;
    m_cachedRepositoryStateSize = info.size();
    m_cachedRepositoryStateModifiedMs = modifiedMs;
    m_cachedRepositoryStateBirthMs = birthMs;
    m_cachedRepositoryStateValid = true;
    return withRenderStatusSnapshot(m_cachedRepositoryState);
}

bool FedoraRepository::writeRepositoryState(const QJsonObject &state) const
{
    const QString statePath = QDir(m_stateRoot).filePath(QStringLiteral("repository.json"));
    QLockFile persistenceLock(QDir(m_stateRoot).filePath(QStringLiteral("repository-state.lock")));
    if (!QDir().mkpath(m_stateRoot) || !persistenceLock.tryLock(5000)) return false;
    const QJsonObject previousState = readRepositoryState();
    if (QFileInfo::exists(statePath) && previousState.isEmpty()) return false;
    if (state.contains(QStringLiteral("projection_store_revision"))
        && state.value(QStringLiteral("projection_store_revision")) != previousState.value(QStringLiteral("projection_store_revision"))) return false;
    QJsonObject oldData = previousState;
    QJsonObject newData = state;
    for (const QString &key : {QStringLiteral("render_state"), QStringLiteral("render_statuses"), QStringLiteral("projection_store_revision")}) {
        oldData.remove(key);
        newData.remove(key);
    }
    if (m_cachedRepositoryStateValid && oldData == newData)
        return writeRenderStatusSnapshot(state);
    QJsonObject previousIndex;
    const bool indexed = Thoughts::CurrentAnnotationIndex::read(m_stateRoot, &previousIndex);
    const QJsonObject previousProjections = indexed ? readRepositoryState().value(QStringLiteral("projections")).toObject() : QJsonObject();
    const QJsonObject nextProjections = state.value(QStringLiteral("projections")).toObject();
    Thoughts::RepositoryProjectionStore records(m_stateRoot);
    qint64 projectionRevision = 0;
    if (!records.write(previousState.value(QStringLiteral("projection_store_revision")).toInteger(),
                       previousState.value(QStringLiteral("projections")).toObject(), nextProjections, &projectionRevision)) return false;
    QJsonObject persisted = state;
    persisted.remove(QStringLiteral("projections"));
    persisted.insert(QStringLiteral("projection_store_revision"), projectionRevision);
    if (!writeJsonFile(statePath, QJsonDocument(persisted).toJson(QJsonDocument::Compact) + '\n')) return false;
    const QFileInfo info(statePath);
    m_cachedRepositoryState = persisted;
    m_cachedRepositoryState.insert(QStringLiteral("projections"), nextProjections);
    m_cachedRepositoryStatePath = statePath;
    m_cachedRepositoryStateSize = info.size();
    m_cachedRepositoryStateModifiedMs = info.lastModified().toMSecsSinceEpoch();
    m_cachedRepositoryStateBirthMs = info.birthTime().toMSecsSinceEpoch();
    m_cachedRepositoryStateValid = true;
    if (!Thoughts::CurrentAnnotationIndex::write(m_stateRoot, state, !indexed || previousProjections != nextProjections))
        qWarning() << "Could not refresh the rebuildable current annotation index.";
    // The old filename described the former filesystem-mirroring design. Keep
    // it only as a one-time read fallback, then retire it after the first
    // successful repository-state commit.
    QFile::remove(QDir(m_stateRoot).filePath(QStringLiteral("mirror.json")));
    return writeRenderStatusSnapshot(state);
}

bool FedoraRepository::writeLibraryIndex(const QJsonObject &library, const QString &directory) const
{
    QJsonArray pdfs;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const QString generationId = publication.value(QStringLiteral("generation_id")).toString();
        const QString kind = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("kind")).toString(QStringLiteral("source_linked"));
        if (!safePdfFilename(filename) || !validSha256(digest) || !validSha256(generationId) || (kind != QLatin1String("source_linked") && kind != QLatin1String("unlinked"))) {
            continue;
        }
        QJsonObject entry {{QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), digest}, {QStringLiteral("kind"), kind}};
        entry.insert(QStringLiteral("document_id"), publication.value(QStringLiteral("document_id")));
        entry.insert(QStringLiteral("generation_id"), publication.value(QStringLiteral("generation_id")));
        entry.insert(QStringLiteral("project"), publication.value(QStringLiteral("project")));
        entry.insert(QStringLiteral("build_id"), publication.value(QStringLiteral("build_id")));
        entry.insert(QStringLiteral("source_frontier"), publication.value(QStringLiteral("source_frontier")));
        entry.insert(QStringLiteral("comment_frontier"), publication.value(QStringLiteral("comment_frontier")));
        entry.insert(QStringLiteral("annotation_frontier"), publication.value(QStringLiteral("annotation_frontier")));
        entry.insert(QStringLiteral("placement_manifest_sha256"), publication.value(QStringLiteral("placement_manifest_sha256")));
        entry.insert(QStringLiteral("input_snapshot_sha256"), publication.value(QStringLiteral("input_snapshot_sha256")));
        if (publication.value(QStringLiteral("source_resources")).isArray()) {
            entry.insert(QStringLiteral("source_resources"), publication.value(QStringLiteral("source_resources")));
        }
        const QString publishedAt = publication.value(QStringLiteral("published_at")).toString();
        if (!publishedAt.isEmpty()) {
            entry.insert(QStringLiteral("published_at"), publishedAt);
        }
        entry.insert(QStringLiteral("pdf_stale"), publication.value(QStringLiteral("pdf_stale")).toBool());
        if (publication.value(QStringLiteral("latest_build")).isObject()) {
            entry.insert(QStringLiteral("latest_build"), publication.value(QStringLiteral("latest_build")));
        }
        const QString displayName = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("display_name")).toString().trimmed();
        if (!displayName.isEmpty()) {
            entry.insert(QStringLiteral("display_name"), displayName);
        }
        entry.insert(QStringLiteral("archived"), publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("archived")).toBool());
        pdfs.append(entry);
    }
    const QJsonObject index {{QStringLiteral("schema_version"), 1},
                             {QStringLiteral("generated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("library_revision"), library.value(QStringLiteral("library_revision"))},
                             {QStringLiteral("source_index_sha256"), library.value(QStringLiteral("source_index_sha256"))},
                             {QStringLiteral("pdfs"), pdfs}};
    const QString root = directory.isEmpty() ? QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs")) : directory;
    return writeJsonFile(QDir(root).filePath(QStringLiteral(".thoughts-library-index.json")), QJsonDocument(index).toJson(QJsonDocument::Indented));
}

bool FedoraRepository::writeLibraryGenerationMetadata(const QJsonObject &library, const QString &directory) const
{
    const QString revision = library.value(QStringLiteral("library_revision")).toString();
    const QString sourceIndexSha256 = library.value(QStringLiteral("source_index_sha256")).toString();
    const QString expectedDirectory = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts-library-stage-%1").arg(revision));
    const QString currentDirectory = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs"));
    const QString actualDirectory = QFileInfo(directory).absoluteFilePath();
    if (!validSha256(revision) || !validSha256(sourceIndexSha256) || (actualDirectory != QFileInfo(expectedDirectory).absoluteFilePath() && actualDirectory != QFileInfo(currentDirectory).absoluteFilePath()) ||
        !library.value(QStringLiteral("publications")).isArray() || !writeLibraryIndex(library, directory)) {
        return false;
    }
    const QJsonObject manifest {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("library_revision"), revision},
        {QStringLiteral("artifact_revision"), effectiveArtifactRevision(library)},
        {QStringLiteral("source_index_sha256"), sourceIndexSha256},
        {QStringLiteral("publications"), library.value(QStringLiteral("publications"))},
        {QStringLiteral("project_readings"), library.value(QStringLiteral("project_readings")).toArray()},
    };
    return writeJsonFile(QDir(directory).filePath(QStringLiteral(".thoughts-generation.json")), QJsonDocument(manifest).toJson(QJsonDocument::Indented));
}

bool FedoraRepository::patchLibraryIndexEntry(const QString &filename, const QString &sha256, const QString &kind, bool remove) const
{
    const QString path = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    // Windows cannot replace a file that is still open.
    file.close();
    if (!document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 1 || !document.object().value(QStringLiteral("pdfs")).isArray()) {
        return false;
    }
    QJsonArray updated;
    bool found = false;
    for (const QJsonValue &value : document.object().value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() == filename) {
            found = true;
            if (!remove) {
                QJsonObject replacement {{QStringLiteral("document_id"), Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(filename).value(QStringLiteral("document_id"))}, {QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), sha256}, {QStringLiteral("kind"), kind}};
                const QString displayName = entry.value(QStringLiteral("display_name")).toString().trimmed();
                if (!displayName.isEmpty()) {
                    replacement.insert(QStringLiteral("display_name"), displayName);
                }
                replacement.insert(QStringLiteral("archived"), entry.value(QStringLiteral("archived")).toBool());
                updated.append(replacement);
            }
        } else {
            updated.append(entry);
        }
    }
    if (!remove && !found) {
        updated.append(QJsonObject {{QStringLiteral("document_id"), Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(filename).value(QStringLiteral("document_id"))}, {QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), sha256}, {QStringLiteral("kind"), kind}, {QStringLiteral("archived"), false}});
    }
    QJsonObject root = document.object();
    root.insert(QStringLiteral("generated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("pdfs"), updated);
    return writeJsonFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool FedoraRepository::patchLibraryDisplayName(const QString &filename, const QString &sha256, const QString &displayName) const
{
    const QString path = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    // Windows cannot replace a file that is still open.
    file.close();
    if (!document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 1 || !document.object().value(QStringLiteral("pdfs")).isArray()) {
        return false;
    }
    QJsonArray updated;
    bool found = false;
    for (const QJsonValue &value : document.object().value(QStringLiteral("pdfs")).toArray()) {
        QJsonObject entry = value.toObject();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        if (entry.value(QStringLiteral("filename")).toString() == filename && (kind == QLatin1String("source_linked") || kind == QLatin1String("unlinked")) && entry.value(QStringLiteral("pdf_sha256")).toString() == sha256) {
            entry.insert(QStringLiteral("display_name"), displayName);
            found = true;
        }
        updated.append(entry);
    }
    if (!found) {
        return false;
    }
    QJsonObject root = document.object();
    root.insert(QStringLiteral("generated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("pdfs"), updated);
    return writeJsonFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool FedoraRepository::enqueueLibraryMutation(const QJsonObject &mutation)
{
    const QString type = mutation.value(QStringLiteral("type")).toString();
    const QString filename = mutation.value(QStringLiteral("filename")).toString();
    if (!safePdfFilename(filename) || (type != QLatin1String("add") && type != QLatin1String("rename") && type != QLatin1String("archive") && type != QLatin1String("remove"))) {
        return false;
    }

    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store) {
        return false;
    }
    const Thoughts::DesktopLibraryEnqueueResult result = store->enqueueLibraryOperation(mutation);
    if (!result.success) {
        return false;
    }
    schedulePendingChangesRefresh();
    return true;
}

void FedoraRepository::captureDocument(const QString &pdfStem)
{
    if (QFileInfo(m_docsRoot).canonicalFilePath() != QFileInfo(m_fedoraDocsRoot).canonicalFilePath()) {
        return;
    }
    if (!safeComponent(pdfStem)) {
        return;
    }
    QFile sidecar(QDir(m_docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem)));
    if (!sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QList<QJsonObject> records;
    while (!sidecar.atEnd()) {
        const QByteArray line = sidecar.readLine().trimmed();
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!line.isEmpty() && document.isObject()) {
            records.append(document.object());
        }
    }
    captureCommentRecords(m_docsRoot, pdfStem, records);
}

bool FedoraRepository::captureCommentRecords(const QString &docsRoot, const QString &pdfStem, const QList<QJsonObject> &records, const QString &verifiedFallbackPdfSha256, const QSet<QString> &createdCommentIds)
{
    // Test/dev docs roots have no Fedora authority.  Their local sidecar is the
    // terminal durable store, so acknowledging the private journal is safe.
    if (QFileInfo(docsRoot).canonicalFilePath() != QFileInfo(m_docsRoot).canonicalFilePath() || QFileInfo(m_docsRoot).canonicalFilePath() != QFileInfo(m_fedoraDocsRoot).canonicalFilePath()) {
        return true;
    }
    if (!safeComponent(pdfStem) || records.isEmpty()) {
        return records.isEmpty();
    }
    const QString fallbackPdfSha256 = verifiedFallbackPdfSha256.toLower();
    if (!fallbackPdfSha256.isEmpty() && !validSha256(fallbackPdfSha256)) {
        return false;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject fingerprints = state.value(QStringLiteral("fingerprints")).toObject();
    QHash<QString, QJsonObject> logicalComments;
    const QString logicalPrefix = QStringLiteral("logical_comment\ncomments\n");
    const QJsonObject storedProjections = state.value(QStringLiteral("projections")).toObject();
    for (auto iterator = storedProjections.constBegin(); iterator != storedProjections.constEnd(); ++iterator) {
        if (iterator.key().startsWith(logicalPrefix) && iterator.value().isObject()) {
            logicalComments.insert(iterator.key().mid(logicalPrefix.size()), iterator.value().toObject());
        }
    }
    QString queuedOperationError;
    const QList<QJsonObject> queuedOperations = m_outbox->operations(&queuedOperationError);
    if (!queuedOperationError.isEmpty()) {
        return false;
    }
    for (const QJsonObject &operation : queuedOperations) {
        if (operation.value(QStringLiteral("entity_type")).toString() != QLatin1String("logical_comment") || operation.value(QStringLiteral("collection")).toString() != QLatin1String("comments") || !operation.value(QStringLiteral("payload")).isObject()) {
            continue;
        }
        logicalComments.insert(operation.value(QStringLiteral("entity_id")).toString(), operation.value(QStringLiteral("payload")).toObject());
    }
    const QString documentId = Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(pdfStem + QStringLiteral(".pdf")).value(QStringLiteral("document_id")).toString();
    if (!Thoughts::DocumentIdentityStore::validId(documentId)) return false;
    bool changed = false;
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The local sync queue could not be checked safely. Your changes remain safely queued."));
        return false;
    }
    // Sidecars are ordered by identity, so replies can precede their root.
    QList<QJsonObject> orderedRecords;
    QList<QJsonObject> pendingRecords = records;
    QSet<QString> availableIds;
    for (auto it = logicalComments.cbegin(); it != logicalComments.cend(); ++it) availableIds.insert(it.key());
    while (!pendingRecords.isEmpty()) {
        bool progressed = false;
        for (qsizetype index = 0; index < pendingRecords.size();) {
            const auto payload = pendingRecords.at(index);
            const auto parentId = payload.value(QStringLiteral("in_reply_to")).toString();
            const auto id = payload.value(QStringLiteral("id")).toString();
            if (availableIds.contains(id) || parentId.isEmpty() || availableIds.contains(parentId)) {
                orderedRecords.append(payload);
                availableIds.insert(id);
                pendingRecords.removeAt(index);
                progressed = true;
            } else {
                ++index;
            }
        }
        if (!progressed) return false;
    }
    for (QJsonObject payload : orderedRecords) {
        const QString entityId = payload.value(QStringLiteral("id")).toString();
        const qint64 revision = payload.value(QStringLiteral("revision")).toInteger();
        if (!safeComponent(entityId) || revision < 1) {
            return false;
        }
        QString pdfSha256 = payload.value(QStringLiteral("annotation_pdf_sha256")).toString().toLower();
        if (!validSha256(pdfSha256)) {
            // An old journal without a captured hash must still save, but
            // cannot acquire the identity of whatever PDF happens to be open.
            pdfSha256 = QString(64, QLatin1Char('0'));
        }
        const QString key = QStringLiteral("annotation\n%1\n%2").arg(pdfStem, entityId);
        payload.insert(QStringLiteral("schema_version"), 1);
        payload.insert(QStringLiteral("annotation_id"), entityId);
        payload.insert(QStringLiteral("document_id"), documentId);
        payload.insert(QStringLiteral("pdf_sha256"), pdfSha256);
        payload.insert(QStringLiteral("updated_at"), payloadTimestamp(payload));
        payload.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
        payload.insert(QStringLiteral("discussion"), true);
        payload = Thoughts::SyncClient::normalizedAnnotationPayload(payload);
        const QString currentFingerprint = QString::fromLatin1(fingerprint(payload));
        if (fingerprints.value(key).toString() == currentFingerprint) {
            continue;
        }
        const auto logical = logicalComments.constFind(entityId);
        if (logical != logicalComments.cend()) {
            auto decision = Internal::logicalCommentMutationForPlacement(logical.value(), payload);
            if (!decision.error.isEmpty()) {
                return false;
            }
            if (decision.changed) {
                decision.payload = stampedLogicalComment(m_docsRoot, decision.payload, logical.value(),
                    logicalComments.value(decision.payload.value(QStringLiteral("thread_root_id")).toString()), pdfSha256 != QString(64, QLatin1Char('0')) ? pdfSha256 : QString());
                if (!localSourceComment(m_docsRoot, &decision.payload, logical.value(), true, nullptr, trackedCommentSourceFiles())) return false;
                const QJsonObject operation = m_outbox->enqueue(QStringLiteral("logical_comment"), entityId, QStringLiteral("comments"), decision.baseRevision, decision.payload, payloadTimestamp(decision.payload));
                if (operation.isEmpty()) {
                    return false;
                }
                logicalComments.insert(entityId, decision.payload);
                schedulePendingChangesRefresh();
                fingerprints.insert(key, currentFingerprint);
                changed = true;
                continue;
            }
            if (decision.suppressPlacement) {
                fingerprints.insert(key, currentFingerprint);
                changed = true;
                continue;
            }
        }
        if (logical != logicalComments.cend() || !createdCommentIds.contains(entityId)) {
            fingerprints.insert(key, currentFingerprint);
            changed = true;
            continue;
        }
        QJsonObject canonical = Internal::logicalCommentForNewPlacement(payload, pdfStem, logicalComments.value(payload.value(QStringLiteral("in_reply_to")).toString()));
        if (canonical.isEmpty()) return false;
        canonical = stampedLogicalComment(m_docsRoot, canonical, {}, logicalComments.value(canonical.value(QStringLiteral("thread_root_id")).toString()),
            pdfSha256 != QString(64, QLatin1Char('0')) ? pdfSha256 : QString());
        if (!localSourceComment(m_docsRoot, &canonical, {}, true, nullptr, trackedCommentSourceFiles())) return false;
        const QJsonObject operation = m_outbox->enqueue(QStringLiteral("logical_comment"), entityId, QStringLiteral("comments"), 0, canonical, payloadTimestamp(payload));
        if (operation.isEmpty()) {
            return false;
        }
        logicalComments.insert(entityId, canonical);
        schedulePendingChangesRefresh();
        fingerprints.insert(key, currentFingerprint);
        changed = true;
    }
    if (!changed) {
        return true;
    }
    state.insert(QStringLiteral("fingerprints"), fingerprints);
    // The outbox entries are already durable.  The fingerprint cache is only
    // a duplicate-suppression optimization and must not hold the journal open.
    writeRepositoryState(state);
    emitEffectiveProjections();
    return true;
}

QByteArray FedoraRepository::bearerToken()
{
    if (!m_cachedToken.isEmpty()) {
        return m_cachedToken;
    }
    const QByteArray environmentToken = qgetenv("THOUGHTS_TOKEN").trimmed();
    if (!environmentToken.isEmpty()) {
        m_cachedToken = environmentToken;
        return m_cachedToken;
    }
    // The stored token goes only to the server that issued it (thoughtsserveraddress.h).
    const QString address = Thoughts::ServerAddress::configured();
    QString issuedFor;
    m_cachedToken = Thoughts::ServerAddress::storedToken(Thoughts::Platform::desktopDeviceAccount(), address, &issuedFor);
    m_tokenWithheld = m_cachedToken.isEmpty() && !issuedFor.isEmpty()
        ? Thoughts::ServerAddress::tokenWithheldMessage(Thoughts::Platform::desktopDeviceAccount(), issuedFor, address) : QString();
    return m_cachedToken;
}

QByteArray FedoraRepository::sourcePublisherToken()
{
    if (!m_cachedSourcePublisherToken.isEmpty()) {
        return m_cachedSourcePublisherToken;
    }
    const QByteArray environmentToken = qgetenv("THOUGHTS_SOURCE_TOKEN").trimmed();
    if (!environmentToken.isEmpty()) {
        m_cachedSourcePublisherToken = environmentToken;
        return m_cachedSourcePublisherToken;
    }
    const QString address = Thoughts::ServerAddress::configured();
    QString issuedFor;
    m_cachedSourcePublisherToken = Thoughts::ServerAddress::storedToken(QStringLiteral("thoughts-source-sync"), address, &issuedFor);
    m_sourcePublisherTokenWithheld = m_cachedSourcePublisherToken.isEmpty() && !issuedFor.isEmpty()
        ? Thoughts::ServerAddress::tokenWithheldMessage(QStringLiteral("thoughts-source-sync"), issuedFor, address) : QString();
    return m_cachedSourcePublisherToken;
}

void FedoraRepository::openNoteAttachment(const QString &docsRoot, const QJsonObject &attachment, QObject *owner, std::function<void(const QString &)> failed)
{
    const QString digest = attachment.value(QStringLiteral("object_sha256")).toString();
    const QString name = attachment.value(QStringLiteral("name")).toString();
    const qint64 size = attachment.value(QStringLiteral("size_bytes")).toInteger(-1);
    // The server chose the name: it must be a plain file name (no invisible
    // or direction-changing characters), see noteattachment.h.
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch()
        || !Thoughts::NoteAttachment::nameIsSafe(name)
        || size < 0 || size > 100 * 1024 * 1024) {
        failed(QStringLiteral("Invalid attachment reference."));
        return;
    }
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/note_attachments/") + digest;
    if (!QDir().mkpath(directory)) { failed(QStringLiteral("Cannot create attachment storage.")); return; }
    const QString path = QDir(directory).filePath(name);
    // Documents, pictures and text open in their application. Anything else
    // (a program, a script, a web page, an archive) is only shown in the
    // file manager. Either way the file is marked as downloaded first.
    const Thoughts::NoteAttachment::Action action = Thoughts::NoteAttachment::actionFor(name);
    const auto open = [path, failed, action] {
        const bool marked = Thoughts::Platform::markDownloaded(path);
        if (action == Thoughts::NoteAttachment::Action::Open) {
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) failed(QStringLiteral("No application could open this attachment."));
        } else if (!marked) {
            // Without the mark the system would run it unchecked.
            failed(QStringLiteral("The attachment was saved but could not be marked as downloaded, so it is not shown: %1").arg(QDir::toNativeSeparators(path)));
        } else if (!Thoughts::Platform::revealInFileManager(path)) {
            failed(QStringLiteral("The attachment was saved as %1, but the file manager could not show it.").arg(QDir::toNativeSeparators(path)));
        }
    };
    QFile cached(path);
    if (cached.open(QIODevice::ReadOnly) && cached.size() == size
        && QCryptographicHash::hash(cached.readAll(), QCryptographicHash::Sha256).toHex() == digest.toLatin1()) {
        open(); return;
    }
    cached.close();
    auto *repository = instance(docsRoot);
    if (!repository->ensureClient()) { failed(QStringLiteral("Configure Sync to download this attachment.")); return; }
    auto *reply = repository->m_client->downloadObject(digest.toLatin1());
    auto bytes = std::make_shared<QByteArray>();
    connect(owner, &QObject::destroyed, reply, &QNetworkReply::abort);
    connect(reply, &QIODevice::readyRead, reply, [reply, bytes, size] {
        bytes->append(reply->readAll());
        if (bytes->size() > size) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, reply, [reply] { reply->deleteLater(); });
    connect(reply, &QNetworkReply::finished, owner, [reply, bytes, digest, size, path, open, failed] {
        bytes->append(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || bytes->size() != size
            || QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex() != digest.toLatin1()) {
            failed(QStringLiteral("The attachment could not be downloaded or verified.")); return;
        }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(*bytes) != bytes->size() || !output.commit()) {
            failed(QStringLiteral("Cannot save the attachment.")); return;
        }
        open();
    });
}

void FedoraRepository::exportProjectPdf(const QString &docsRoot, const QJsonObject &project, const QString &destination,
    QObject *owner, std::function<void(const QString &)> progress, std::function<void(bool, const QString &)> completed)
{
    const auto id = project.value(QStringLiteral("id")).toString();
    const auto cached = project.value(QStringLiteral("reading_revision")).toString();
    if (id.isEmpty() || !validSha256(cached)) { completed(false, QStringLiteral("Sync this project before exporting it.")); return; }
    auto *repository = instance(docsRoot);
    if (!repository->ensureClient()) { completed(false, QStringLiteral("Configure Sync to export this project.")); return; }
    // The Library list can lag Fedora's latest build, so export the project's current revision.
    progress(QStringLiteral("Checking the project's latest version…"));
    auto *latest = repository->m_client->libraryProjects();
    connect(latest, &QNetworkReply::finished, latest, &QNetworkReply::deleteLater);
    connect(latest, &QNetworkReply::finished, owner, [=] {
        QString revision = cached;
        if (latest->error() == QNetworkReply::NoError) {
            for (const auto &value : QJsonDocument::fromJson(latest->readAll()).object().value(QStringLiteral("projects")).toArray()) {
                const auto current = value.toObject();
                if (current.value(QStringLiteral("id")).toString() == id && validSha256(current.value(QStringLiteral("reading_revision")).toString()))
                    revision = current.value(QStringLiteral("reading_revision")).toString();
            }
        }
        exportProjectRevision(repository, id, revision, project.value(QStringLiteral("export_document_ids")).toArray(), destination, owner, progress, completed);
    });
}

void FedoraRepository::exportProjectRevision(FedoraRepository *repository, const QString &id, const QString &revision, const QJsonArray &documentIds,
    const QString &destination, QObject *owner, std::function<void(const QString &)> progress, std::function<void(bool, const QString &)> completed)
{
    // Each selection of documents is its own export of the revision; none means every document.
    QStringList chosen;
    for (const auto &value : documentIds) chosen.append(value.toString());
    chosen.sort();
    const auto key = QStringLiteral("project-exports/") + revision + (chosen.isEmpty() ? QString()
        : QLatin1Char('-') + QString::fromLatin1(QCryptographicHash::hash(chosen.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex().left(16)));
    QSettings settings;
    auto requestId = settings.value(key).toString();
    if (requestId.isEmpty()) { requestId = QUuid::createUuid().toString(QUuid::WithoutBraces); settings.setValue(key, requestId); settings.sync(); }
    auto *poll = new QTimer(owner);
    poll->setInterval(2000);
    auto busy = std::make_shared<bool>(false);
    const auto accept = [poll, busy, repository, requestId, revision, destination, key, progress, completed](QNetworkReply *reply) {
        *busy = true;
        connect(poll, &QObject::destroyed, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
        connect(reply, &QNetworkReply::finished, poll, [=] {
            *busy = false;
            if (reply->error() != QNetworkReply::NoError) { poll->deleteLater(); completed(false, replyErrorInfo(reply).summary); return; }
            const auto status = QJsonDocument::fromJson(reply->readAll()).object();
            if (status.value(QStringLiteral("id")).toString() != requestId || status.value(QStringLiteral("reading_revision")).toString() != revision) {
                poll->deleteLater(); completed(false, QStringLiteral("The export response does not match the requested project revision.")); return;
            }
            const auto state = status.value(QStringLiteral("status")).toString();
            if (state == QLatin1String("failed")) {
                QSettings().remove(key); poll->deleteLater(); completed(false, status.value(QStringLiteral("error")).toString()); return;
            }
            if (state == QLatin1String("queued") || state == QLatin1String("running")) {
                progress(state == QLatin1String("queued") ? QStringLiteral("Export queued…") : QStringLiteral("Creating the combined PDF…"));
                if (!poll->isActive()) poll->start(); return;
            }
            const auto digest = status.value(QStringLiteral("sha256")).toString();
            const auto size = status.value(QStringLiteral("size_bytes")).toInteger(-1);
            if (state != QLatin1String("complete") || !validSha256(digest) || size <= 0) {
                poll->deleteLater(); completed(false, QStringLiteral("The export is incomplete.")); return;
            }
            poll->stop();
            progress(QStringLiteral("Downloading the combined PDF…"));
            auto output = std::make_shared<QSaveFile>(destination);
            if (!output->open(QIODevice::WriteOnly)) { poll->deleteLater(); completed(false, output->errorString()); return; }
            auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
            auto written = std::make_shared<qint64>(0);
            auto *download = repository->m_client->downloadObject(digest.toLatin1());
            connect(poll, &QObject::destroyed, download, &QNetworkReply::abort);
            const auto drain = [download, output, hash, written, size] {
                const auto bytes = download->readAll(); *written += bytes.size();
                if (*written > size || output->write(bytes) != bytes.size()) { download->abort(); return; }
                hash->addData(bytes);
            };
            connect(download, &QIODevice::readyRead, poll, drain);
            connect(download, &QNetworkReply::finished, download, &QNetworkReply::deleteLater);
            connect(download, &QNetworkReply::finished, poll, [=] {
                drain();
                const bool okay = download->error() == QNetworkReply::NoError && *written == size && hash->result().toHex() == digest.toLatin1() && output->commit();
                poll->deleteLater();
                completed(status.value(QStringLiteral("older_revision")).toBool(), okay ? QString() : QStringLiteral("The exported PDF could not be downloaded and verified."));
            });
        });
    };
    connect(poll, &QTimer::timeout, poll, [=] { if (!*busy) accept(repository->m_client->projectExport(requestId)); });
    progress(QStringLiteral("Requesting project export…"));
    QJsonObject request {{QStringLiteral("request_id"), requestId}, {QStringLiteral("project_id"), id}, {QStringLiteral("reading_revision"), revision}};
    if (!documentIds.isEmpty()) request.insert(QStringLiteral("document_ids"), documentIds);
    accept(repository->m_client->requestProjectExport(request));
}

bool FedoraRepository::ensureClient()
{
    if (m_client) {
        return true;
    }
    const QByteArray token = bearerToken();
    if (token.isEmpty()) {
        return false;
    }
    const QUrl url(Thoughts::ServerAddress::normalized(Thoughts::ServerAddress::configured()));
    if (url.isEmpty()) {
        return false;
    }
    m_client = std::make_unique<Thoughts::SyncClient>(url, token, this);
    m_client->setTraceId(m_activeTraceId.toUtf8());
    return true;
}

QJsonObject FedoraRepository::readerSyncPlanRequest(const QString &target)
{
    QSet<QString> known;
    QSet<QString> pdfDeltaBases;
    QString storeError;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&storeError);
    if (!storeError.isEmpty()) {
        storeError.clear();
    }
    const QDir current(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs")));
    QFile generationFile(current.filePath(QStringLiteral(".thoughts-generation.json")));
    const QJsonObject generation = generationFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(generationFile.readAll()).object() : QJsonObject();
    for (const QJsonValue &value : generation.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        const QFileInfo pdf(current.filePath(filename));
        if (safePdfFilename(filename) && Internal::verifyReaderObjectWithCache(store, pdf.absoluteFilePath(), digest, size, QStringLiteral("pdf"), QStringLiteral("pdf"), filename)) {
            known.insert(digest);
            pdfDeltaBases.insert(digest);
        }
    }
    const QString sourceIndexDigest = generation.value(QStringLiteral("source_index_sha256")).toString();
    const QString sourceIndexPath = current.filePath(QStringLiteral(".thoughts-source-index.json"));
    const QFileInfo sourceIndexInfo(sourceIndexPath);
    if (Internal::verifyReaderObjectWithCache(store, sourceIndexPath, sourceIndexDigest, sourceIndexInfo.isFile() ? sourceIndexInfo.size() : -1, QStringLiteral("source_index"), QStringLiteral("source_index"))) {
        known.insert(sourceIndexDigest);
    }

    const QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
    const QDir objects(bundleRoot.filePath(QStringLiteral("objects")));
    for (const QFileInfo &object : objects.entryInfoList(QDir::Files, QDir::Name)) {
        if (Internal::verifyReaderObjectWithCache(store, object.absoluteFilePath(), object.fileName(), object.size(), QStringLiteral("source"))) {
            known.insert(object.fileName());
        }
    }

    bool allowPdfDelta = false;
#ifdef THOUGHTS_HAVE_ZSTD
    allowPdfDelta = target.isEmpty() ? !m_libraryPdfDeltaFallbackRequested : !m_targetPdfDeltaFallbackRequested;
#endif
    QStringList identities;
    if (allowPdfDelta) {
        QStringList bases = pdfDeltaBases.values();
        QStringList remaining = (known - pdfDeltaBases).values();
        std::sort(bases.begin(), bases.end());
        std::sort(remaining.begin(), remaining.end());
        identities = bases + remaining;
        identities = identities.mid(0, 20000);
    } else {
        identities = known.values();
        std::sort(identities.begin(), identities.end());
        if (identities.size() > 20000) {
            identities = identities.mid(identities.size() - 20000);
        }
    }
    QJsonArray knownObjects;
    for (const QString &identity : std::as_const(identities)) {
        knownObjects.append(identity);
    }
    QJsonArray advertisedPdfDeltaBases;
    if (allowPdfDelta) {
        const QSet<QString> advertisedIdentities(identities.cbegin(), identities.cend());
        QStringList bases = (pdfDeltaBases & advertisedIdentities).values();
        std::sort(bases.begin(), bases.end());
        for (const QString &base : std::as_const(bases)) {
            advertisedPdfDeltaBases.append(base);
        }
    }
    QJsonArray capabilities {QStringLiteral("project_reading_v1"), QStringLiteral("source_resource_v1"), QStringLiteral("compiler_review_placement_v3"), QStringLiteral("markdown_source_v1")};
    if (allowPdfDelta) {
        capabilities.append(QStringLiteral("pdf_delta_v1"));
    }
    const QJsonObject state = readRepositoryState();
    QJsonObject request {
        {QStringLiteral("contract_version"), 2},
        {QStringLiteral("source_profile"), QStringLiteral("reader-v1")},
        {QStringLiteral("capabilities"), capabilities},
        {QStringLiteral("include_voice_media"), false},
        {QStringLiteral("after_sequence"), m_outbox ? m_outbox->cursor() : 0},
        {QStringLiteral("projection_bootstrapped"), hasProjectionSnapshot()},
        {QStringLiteral("known_object_sha256s"), knownObjects},
    };
    if (allowPdfDelta) {
        request.insert(QStringLiteral("pdf_delta_base_sha256s"), advertisedPdfDeltaBases);
    }
    const QString libraryRevision = state.value(QStringLiteral("active_library_revision")).toString();
    if (validSha256(libraryRevision)) {
        request.insert(QStringLiteral("library_revision"), libraryRevision);
    }
    if (!target.isEmpty()) {
        request.insert(QStringLiteral("target_pdf_filename"), target);
    }
    return request;
}

void FedoraRepository::requestReaderSyncPlan(const QString &target, std::function<void(const QJsonObject &, const QString &)> completion)
{
    if (!m_client || m_readerPlanPulling || (!target.isEmpty() && !safePdfFilename(target))) {
        completion({}, QStringLiteral("A reader synchronization plan is already being prepared or is invalid."));
        return;
    }
    m_readerPlanPulling = true;
    const QJsonObject request = readerSyncPlanRequest(target);
    retryingRequest([this, request] { return m_client->syncPlan(request); }, [this, target, request, completion = std::move(completion)](QNetworkReply *planReply) mutable {
        m_readerPlanPulling = false;
        if (planReply->error() != QNetworkReply::NoError) {
            const int status = planReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const Thoughts::SyncErrorInfo parsed = replyErrorInfo(planReply);
            planReply->deleteLater();
            completion({}, (status == 404 || status == 426) ? QStringLiteral("The server does not support the required sync transfer plan v2 contract.") : parsed.summary);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(planReply->readAll());
        planReply->deleteLater();
        QJsonObject plan = document.object();
        bool valid = document.isObject() && plan.value(QStringLiteral("contract_version")).toInt() == 2 && plan.value(QStringLiteral("source_profile")).toString() == QLatin1String("reader-v1") &&
            !plan.value(QStringLiteral("include_voice_media")).toBool() && validSha256(plan.value(QStringLiteral("plan_id")).toString()) && plan.value(QStringLiteral("latest_server_sequence")).toInteger(-1) >= 0 &&
            validSha256(plan.value(QStringLiteral("library_revision")).toString()) && plan.value(QStringLiteral("metadata_download_bytes")).toInteger(-1) >= 0 && plan.value(QStringLiteral("download_bytes")).toInteger(-1) >= 0 &&
            plan.value(QStringLiteral("objects")).isArray();
        if (!valid) {
            completion({}, QStringLiteral("The server returned an invalid reader synchronization plan."));
            return;
        }
        QSet<QString> objectIds;
        qint64 exactDownloadBytes = plan.value(QStringLiteral("metadata_download_bytes")).toInteger(-1);
        for (const QJsonValue &value : plan.value(QStringLiteral("objects")).toArray()) {
            const QJsonObject object = value.toObject();
            const QString sha256 = object.value(QStringLiteral("sha256")).toString();
            const qint64 size = object.value(QStringLiteral("size_bytes")).toInteger(-1);
            const QString kind = object.value(QStringLiteral("kind")).toString();
            const QJsonArray roles = object.value(QStringLiteral("roles")).toArray();
            const QJsonArray publications = object.value(QStringLiteral("publications")).toArray();
            static const QSet<QString> allowedRoles {
                QStringLiteral("project_reading"), QStringLiteral("document_pdf"), QStringLiteral("document_reading_package"),
                QStringLiteral("pdf"),
                QStringLiteral("pdf_delta"),
                QStringLiteral("source_index"),
                QStringLiteral("source_manifest"),
                QStringLiteral("source_snapshot"),
                QStringLiteral("source_resource"),
                QStringLiteral("renderer_evidence"),
                QStringLiteral("reverse_lookup"),
                QStringLiteral("raw_synctex_fallback"),
                QStringLiteral("voice"),
            };
            const bool pdfDelta = plannedObjectHasRole(object, QStringLiteral("pdf_delta"));
            const bool pdfDeltaRequested = request.value(QStringLiteral("capabilities")).toArray().contains(QJsonValue(QStringLiteral("pdf_delta_v1")));
            const bool rendererEvidence = plannedObjectHasRole(object, QStringLiteral("renderer_evidence"));
            const bool markdownRequested = request.value(QStringLiteral("capabilities")).toArray().contains(QJsonValue(QStringLiteral("markdown_source_v1")));
            if (!validSha256(sha256) || objectIds.contains(sha256) || size < 0 || size > std::numeric_limits<qint64>::max() - exactDownloadBytes ||
                (kind != QLatin1String("pdf") && kind != QLatin1String("source") && kind != QLatin1String("source_index") && kind != QLatin1String("voice")) || !object.value(QStringLiteral("roles")).isArray() || roles.isEmpty() ||
                std::any_of(roles.cbegin(), roles.cend(), [](const QJsonValue &role) { return !role.isString() || !allowedRoles.contains(role.toString()); }) || !object.value(QStringLiteral("publications")).isArray() ||
                std::any_of(publications.cbegin(), publications.cend(), [](const QJsonValue &publication) { return !publication.isString() || (!safePdfFilename(publication.toString()) && !QRegularExpression(QStringLiteral("^project_[0-9a-f]{24}\\.thoughts-project$")).match(publication.toString()).hasMatch()); }) ||
                (pdfDelta && (!pdfDeltaRequested || kind != QLatin1String("pdf") || publications.size() != 1)) ||
                (rendererEvidence && (!markdownRequested || kind != QLatin1String("source")))) {
                completion({}, QStringLiteral("The server returned an invalid planned object."));
                return;
            }
            objectIds.insert(sha256);
            exactDownloadBytes += size;
        }
        if (exactDownloadBytes != plan.value(QStringLiteral("download_bytes")).toInteger(-1)) {
            completion({}, QStringLiteral("The server returned a reader plan with an inexact byte total."));
            return;
        }
        plan.insert(QStringLiteral("_desktop_after_sequence"), request.value(QStringLiteral("after_sequence")));
        plan.insert(QStringLiteral("_desktop_projection_bootstrapped"), request.value(QStringLiteral("projection_bootstrapped")));
        plan.insert(QStringLiteral("_desktop_requested_library_revision"), request.value(QStringLiteral("library_revision")));
        plan.insert(QStringLiteral("_desktop_known_objects"), request.value(QStringLiteral("known_object_sha256s")));
        plan.insert(QStringLiteral("_desktop_pdf_delta_requested"), request.value(QStringLiteral("capabilities")).toArray().contains(QJsonValue(QStringLiteral("pdf_delta_v1"))));
        plan.insert(QStringLiteral("_desktop_pdf_delta_bases"), request.value(QStringLiteral("pdf_delta_base_sha256s")));
        plan.insert(QStringLiteral("_desktop_library_required"), request.value(QStringLiteral("library_revision")).toString() != plan.value(QStringLiteral("library_revision")).toString());
        if (!target.isEmpty()) {
            plan.insert(QStringLiteral("_desktop_target"), target);
        }
        emitSyncProgress(QStringLiteral("checking_server"),
                         0,
                         plan.value(QStringLiteral("download_bytes")).toInteger(),
                         target.isEmpty() ? QStringLiteral("Reader synchronization plan prepared") : QStringLiteral("Target reader synchronization plan prepared"));
        completion(plan, QString());
    });
}

bool FedoraRepository::readerPlanAllowsObject(const QJsonObject &plan, const QString &sha256, const QString &kind, const QString &role, const QString &publication, qint64 expectedSize) const
{
    if (plan.value(QStringLiteral("contract_version")).toInt() != 2 || plan.value(QStringLiteral("source_profile")).toString() != QLatin1String("reader-v1")) {
        return false;
    }
    for (const QJsonValue &value : plan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("sha256")).toString() != sha256) {
            continue;
        }
        const QJsonArray roles = object.value(QStringLiteral("roles")).toArray();
        const QJsonArray publications = object.value(QStringLiteral("publications")).toArray();
        const bool hasRole = std::any_of(roles.cbegin(), roles.cend(), [&role](const QJsonValue &value) { return value.toString() == role; });
        const bool hasPublication = publication.isEmpty() || std::any_of(publications.cbegin(), publications.cend(), [&publication](const QJsonValue &value) { return value.toString() == publication; });
        return object.value(QStringLiteral("kind")).toString() == kind && hasRole && hasPublication && (expectedSize < 0 || object.value(QStringLiteral("size_bytes")).toInteger(-1) == expectedSize);
    }
    return false;
}

qint64 FedoraRepository::readerPlanObjectSize(const QJsonObject &plan, const QString &sha256) const
{
    for (const QJsonValue &value : plan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("sha256")).toString() == sha256) {
            return object.value(QStringLiteral("size_bytes")).toInteger(-1);
        }
    }
    return -1;
}

void FedoraRepository::initializeReaderPlanTransfers(const QJsonObject &plan)
{
    if (!m_syncTransferLedger) {
        m_syncTransferLedger = std::make_unique<Thoughts::SyncTransferLedger>();
    }
    QHash<QString, qint64> items;
    const QString planId = plan.value(QStringLiteral("plan_id")).toString();
    items.insert(QStringLiteral("metadata:%1").arg(planId), plan.value(QStringLiteral("metadata_download_bytes")).toInteger());
    for (const QJsonValue &value : plan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        items.insert(object.value(QStringLiteral("sha256")).toString(), object.value(QStringLiteral("size_bytes")).toInteger());
    }
    if (!m_syncTransferLedger->replaceDownloadPlan(planId, items)) {
        return;
    }
    m_activeReaderTransferPlanId = planId;
    m_libraryReaderMetadataBytes = 0;
    emitReaderTransferSnapshot();
}

bool FedoraRepository::accountReaderMetadataBytes(qint64 bytes, bool complete)
{
    const QJsonObject plan = m_targetReaderPlan.value(QStringLiteral("plan_id")).toString() == m_activeReaderTransferPlanId ? m_targetReaderPlan : m_libraryReaderPlan;
    const QString item = QStringLiteral("metadata:%1").arg(plan.value(QStringLiteral("plan_id")).toString());
    const qint64 total = plan.value(QStringLiteral("metadata_download_bytes")).toInteger(-1);
    if (!m_syncTransferLedger || total < 0 || bytes < 0 || bytes > total || m_libraryReaderMetadataBytes > total - bytes) {
        return false;
    }
    m_libraryReaderMetadataBytes += bytes;
    const bool accepted = complete ? m_libraryReaderMetadataBytes == total && m_syncTransferLedger->commitItem(Thoughts::SyncTransferLedger::Direction::Download, item, total)
                                   : m_syncTransferLedger->updateItem(Thoughts::SyncTransferLedger::Direction::Download, item, m_libraryReaderMetadataBytes);
    if (accepted) {
        emitReaderTransferSnapshot(complete);
    }
    return accepted;
}

void FedoraRepository::accountReaderObjectProgress(const QString &sha256, qint64 bytes, bool complete, bool rollback)
{
    if (!m_syncTransferLedger || !validSha256(sha256)) {
        return;
    }
    bool changed = false;
    if (rollback) {
        changed = m_syncTransferLedger->rollbackItem(Thoughts::SyncTransferLedger::Direction::Download, sha256);
    } else if (complete) {
        changed = m_syncTransferLedger->commitItem(Thoughts::SyncTransferLedger::Direction::Download, sha256, bytes);
    } else {
        changed = m_syncTransferLedger->updateItem(Thoughts::SyncTransferLedger::Direction::Download, sha256, bytes);
    }
    if (changed) {
        emitReaderTransferSnapshot(complete || rollback);
    }
}

bool FedoraRepository::planManualUpload(const QString &itemId, qint64 bytes)
{
    if (!m_syncTransferLedger || !m_syncTransferLedger->planItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId, bytes)) {
        return false;
    }
    emitReaderTransferSnapshot();
    return true;
}

void FedoraRepository::accountManualUploadProgress(const QString &itemId, qint64 bytes, bool complete, bool rollback)
{
    if (!m_syncTransferLedger || itemId.isEmpty()) {
        return;
    }
    bool changed = false;
    if (rollback) {
        changed = m_syncTransferLedger->rollbackItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId);
    } else if (complete) {
        changed = m_syncTransferLedger->commitItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId, bytes);
    } else {
        changed = m_syncTransferLedger->updateItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId, bytes);
    }
    if (changed) {
        emitReaderTransferSnapshot(complete || rollback);
    }
}

bool FedoraRepository::updateManualTransferTotals(const QString &itemId, qint64 uploadedBytes, qint64 downloadedBytes)
{
    if (!m_syncTransferLedger || itemId.isEmpty() || uploadedBytes < 0 || downloadedBytes < 0 || !m_syncTransferLedger->updateUnplannedItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId, uploadedBytes) ||
        !m_syncTransferLedger->updateUnplannedItem(Thoughts::SyncTransferLedger::Direction::Download, itemId, downloadedBytes)) {
        return false;
    }
    emitReaderTransferSnapshot();
    return true;
}

bool FedoraRepository::commitManualTransferTotals(const QString &itemId, qint64 uploadedBytes, qint64 downloadedBytes)
{
    if (!m_syncTransferLedger || itemId.isEmpty() || uploadedBytes < 0 || downloadedBytes < 0 || !m_syncTransferLedger->commitUnplannedItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId, uploadedBytes) ||
        !m_syncTransferLedger->commitUnplannedItem(Thoughts::SyncTransferLedger::Direction::Download, itemId, downloadedBytes)) {
        return false;
    }
    emitReaderTransferSnapshot(true);
    return true;
}

void FedoraRepository::rollbackManualTransferTotals(const QString &itemId)
{
    if (!m_syncTransferLedger || itemId.isEmpty()) {
        return;
    }
    const bool upload = m_syncTransferLedger->rollbackItem(Thoughts::SyncTransferLedger::Direction::Upload, itemId);
    const bool download = m_syncTransferLedger->rollbackItem(Thoughts::SyncTransferLedger::Direction::Download, itemId);
    if (upload || download) {
        emitReaderTransferSnapshot(true);
    }
}

void FedoraRepository::emitReaderTransferSnapshot(bool flush)
{
    if (!m_activeTraceId.isEmpty() && m_syncTransferLedger) {
        const QJsonObject snapshot = m_syncTransferLedger->snapshot(flush);
        if (!snapshot.isEmpty()) {
            Q_EMIT syncTransferProgressChanged(m_activeTraceId, snapshot);
        }
    }
}

bool FedoraRepository::recordVerifiedReaderObject(const QString &path, const QString &sha256, qint64 size, const QString &kind, const QString &role, const QString &publication)
{
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store || !error.isEmpty()) {
        return false;
    }
    const QJsonObject metadata = readerFileMetadata(path);
    if (metadata.isEmpty() || metadata.value(QStringLiteral("size_bytes")).toInteger(-1) != size) {
        return false;
    }
    Thoughts::DesktopVerifiedObject object;
    object.sha256 = sha256;
    object.sizeBytes = size;
    object.kind = kind;
    object.semanticRole = role;
    object.owningPublication = publication;
    object.verifiedAtMs = QDateTime::currentMSecsSinceEpoch();
    object.metadata = metadata;
    return store->upsertVerifiedObject(object, &error);
}

bool FedoraRepository::recordActivatedReaderGenerationObjects(const QJsonObject &library)
{
    const QDir current(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs")));
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString sha256 = publication.value(QStringLiteral("sha256")).toString();
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (!safePdfFilename(filename) || !validSha256(sha256) || size < 0 || !recordVerifiedReaderObject(current.filePath(filename), sha256, size, QStringLiteral("pdf"), QStringLiteral("pdf"), filename)) {
            return false;
        }
    }
    const QString sourceIndexSha256 = library.value(QStringLiteral("source_index_sha256")).toString();
    const QString sourceIndexPath = current.filePath(QStringLiteral(".thoughts-source-index.json"));
    const QFileInfo sourceIndex(sourceIndexPath);
    return validSha256(sourceIndexSha256) && sourceIndex.isFile() && recordVerifiedReaderObject(sourceIndexPath, sourceIndexSha256, sourceIndex.size(), QStringLiteral("source_index"), QStringLiteral("source_index"));
}

void FedoraRepository::applyTraceId(const QString &traceId)
{
    static const QRegularExpression uuidPattern(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"));
    m_activeTraceId = uuidPattern.match(traceId).hasMatch() ? traceId.toLower() : QString();
    if (m_client) {
        m_client->setTraceId(m_activeTraceId.toUtf8());
    }
}

void FedoraRepository::emitSyncProgress(const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &message)
{
    if (!m_activeTraceId.isEmpty()) {
        Q_EMIT syncProgressChanged(m_activeTraceId, phase, transferredBytes, totalBytes, message);
    }
}

void FedoraRepository::emitReaderStepTiming(const QString &step)
{
    if (m_readerPhaseClock.isValid())
        emitSyncTiming(QStringLiteral("reader_") + step, QStringLiteral("reader step %1 at %2 ms").arg(step).arg(m_readerPhaseClock.elapsed()));
}

void FedoraRepository::emitSyncTiming(const QString &key, const QString &message)
{
    if (m_syncCoordinator.running()) {
        emitSyncProgress(QStringLiteral("timing_desktop_") + key, 0, 0, QStringLiteral("Desktop timing: ") + message);
    }
}

void FedoraRepository::syncNow(const QString &traceId, bool conditionalRenderCheck)
{
    beginManualSync(QString(), conditionalRenderCheck, traceId);
}

void FedoraRepository::syncDocument(const QString &target, bool publishSources, const QString &traceId)
{
    // Keep source publication scoped; artifact activation still installs a coherent collection.
    beginManualSync(target, publishSources, traceId);
}

void FedoraRepository::forgetServerSupport()
{
    m_reviewTagGroupsSupport = -1;
    m_pdfLinkOverridesSupport = -1;
    m_reviewCardFeedbackSupport = -1;
    m_documentFeedbackSupport = -1;
    m_documentFeedbackAttachmentSupport = -1;
    m_documentFeedbackKindsSupport = -1;
    m_documentFeedbackDrawingSupport = -1;
    m_documentFeedbackTextSupport = -1;
    m_agentConversationsSupport = -1;
    m_reviewTagScaleSupport = -1;
    m_reviewTagSectionSupport = -1;
    m_reviewTagPhaseSupport = -1;
}

void FedoraRepository::beginManualSync(const QString &target, bool conditionalRenderCheck, const QString &traceId)
{
    m_clientReviewSourcesReady = false;
    m_clientReviewSourceReplacementReady = false;
    m_clientReviewCardMoveReady = false;
    forgetServerSupport();
    m_uploadedReviewSourceObjects.clear();
    if (m_syncCoordinator.running() || (!target.isEmpty() && !safePdfFilename(target))) {
        return;
    }

    const QDir sourceJournal(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-journal")));
    const auto durable = effectiveProjections(false);
    for (const auto &path : sourceJournal.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile file(sourceJournal.filePath(path));
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto document = QJsonDocument::fromJson(file.readAll());
        // Windows cannot delete a file that is still open.
        file.close();
        if (!document.isArray()) return;
        QList<QJsonObject> recovered;
        for (const auto &value : document.array()) {
            auto payload = value.toObject();
            const auto id = payload.value(QStringLiteral("comment_id")).toString();
            const auto revision = payload.value(QStringLiteral("revision")).toInteger();
            if (revision <= durable.value(QStringLiteral("logical_comment\ncomments\n") + id).toObject().value(QStringLiteral("revision")).toInteger()) continue;
            localSourceComment(m_docsRoot, &payload, {}, false, nullptr, trackedCommentSourceFiles());
            recovered.append({{QStringLiteral("entity_type"), QStringLiteral("logical_comment")}, {QStringLiteral("entity_id"), id},
                {QStringLiteral("collection"), QStringLiteral("comments")}, {QStringLiteral("base_revision"), revision - 1},
                {QStringLiteral("client_timestamp"), payload.value(QStringLiteral("updated_at"))}, {QStringLiteral("payload"), payload}});
        }
        if (!recovered.isEmpty() && (!ensureOutboxReady() || m_outbox->enqueueBatch(recovered).size() != recovered.size())) return;
        if (!QFile::remove(file.fileName())) return;
    }
    applyTraceId(traceId);
    if (m_activeTraceId.isEmpty()) {
        applyTraceId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    if (m_syncTransferLedger) {
        m_syncTransferLedger->reset();
    }
    m_activeReaderTransferPlanId.clear();
    m_syncCoordinator.begin(target, conditionalRenderCheck, m_activeTraceId);
    if (!QDir(QDir(m_docsRoot).filePath(QStringLiteral("reviews/source-transactions"))).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty()) {
        failManualSync(QStringLiteral("An interrupted review source edit needs recovery. Open Reviews before retrying Sync."), QString(), QStringLiteral("needs_user"));
        return;
    }
    m_projectSyncReady = false;
    m_projectSyncRunning = false;
    m_defaultFolderMigrationChecked = false;
    m_deviceSequenceRecoveryAttempts = 0;
    m_projectSyncDrainingOutbox = false;
    m_targetReaderPlan = {};
    m_targetReaderPlanTarget.clear();
    m_targetReaderPlanPublicationIdentity.clear();
    m_libraryReaderMetadataBytes = 0;
    m_libraryReaderPlanRetries = 0;
    m_libraryReaderObjectRetries = 0;
    m_targetReaderPlanRetries = 0;
    m_libraryPdfDeltaFallbackRequested = false;
    m_targetPdfDeltaFallbackRequested = false;
    m_libraryReaderPlan = {};

    if (!m_syncCoordinator.isDocument()) {
        QJsonObject state = readRepositoryState();
        QJsonObject projections = state.value(QStringLiteral("projections")).toObject();
        bool compacted = false;
        for (auto iterator = projections.begin(); iterator != projections.end();) {
            if (!iterator.key().startsWith(QLatin1String("logical_comment\ncomments\n"))
                && iterator.value().toObject().value(QStringLiteral("deleted")).toBool()) {
                iterator = projections.erase(iterator);
                compacted = true;
            } else {
                ++iterator;
            }
        }
        if (compacted) {
            state.insert(QStringLiteral("projections"), projections);
            if (!writeRepositoryState(state)) {
                failManualSync(QStringLiteral("The historical deleted records could not be compacted safely."));
                return;
            }
        }
    }

    Q_EMIT manualSyncStateChanged(m_syncCoordinator.state, QString(), QString());
    emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("Checking server"));
    QString outboxDetails;
    if (!ensureOutboxReady(&outboxDetails)) {
        failManualSync(QStringLiteral("The local sync queue could not be checked safely. Your changes remain safely queued."), outboxDetails);
        return;
    }
    // Finish any interrupted proposal source edit before publishing sources.
    if (!Thoughts::ProposalHistory::recover(m_docsRoot, &outboxDetails)) {
        failManualSync(QStringLiteral("An interrupted proposal edit could not be completed safely."), outboxDetails); return;
    }
    if (!ensureClient()) {
        QString addressError;
        if (Thoughts::ServerAddress::normalized(Thoughts::ServerAddress::configured(), &addressError).isEmpty()) {
            failManualSync(QStringLiteral("The server address is invalid."),
                           Thoughts::ServerAddress::overriddenByEnvironment()
                               ? QStringLiteral("%1 Correct THOUGHTS_SERVER_URL.").arg(addressError)
                               : QStringLiteral("%1 Correct it in Server… (Thoughts menu on macOS, File menu elsewhere).").arg(addressError));
            return;
        }
        if (!m_tokenWithheld.isEmpty()) {
            // The address changed to another server: its token is needed, the old one stays here.
            failManualSync(QStringLiteral("This computer's device token belongs to another server."), m_tokenWithheld);
            return;
        }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        failManualSync(QStringLiteral("The server is not configured or its credentials are unavailable."),
                       QStringLiteral("Enter the tokens the server's owner gave you for this computer in Server… (Thoughts menu on macOS, File menu elsewhere)."));
#else
        failManualSync(QStringLiteral("The server is not configured or its credentials are unavailable."));
#endif
        return;
    }
    // Each Sync refreshes the cached server capabilities, which local commands
    // (such as moving a card to another Project) read without asking the server.
    m_serverCapabilitiesFresh = false;
    queryServerCapabilities(this, [](const QStringList &) {});

    // Thoughts never fetches from, merges with, or pushes to Overleaf or a Git
    // remote, here or on Fedora: Sync continues with the canonical sources
    // Fedora already knows about.
    continueManualSyncAfterOverleaf();
}

void FedoraRepository::synchronizeLibraryProjects()
{
    if (m_projectSyncRunning || !m_client || !m_syncCoordinator.running()) return;
    const QString syncTrace = m_activeTraceId;
    const auto active = [this, syncTrace] { return m_syncCoordinator.running() && m_activeTraceId == syncTrace; };
    QJsonObject operation;
    Thoughts::LibraryProjectStore store(m_docsRoot);
    const auto pending = store.pending();
    if (!store.error().isEmpty()) { failManualSync(store.error()); return; }
    // A move changes annotation placement revisions. Publish this device's
    // queued edits first so the transfer includes their latest content.
    const bool hasMove = std::any_of(pending.cbegin(), pending.cend(), [](const QJsonValue &value) {
        const QString action = value.toObject().value(QStringLiteral("action")).toString();
        return action == QLatin1String("move_document") || action == Thoughts::SourceDocumentMove::action;
    });
    if (hasMove) {
        QString error;
        auto edits = m_outbox->operations(&error);
        if (!error.isEmpty()) { failManualSync(error); return; }
        if (!m_uploading && !m_pulling && holdUnsupportedOperations(edits)) { m_projectSyncDrainingOutbox = true; return; }
        if (!edits.isEmpty()) {
            m_projectSyncDrainingOutbox = true;
            if (m_uploading || m_pulling) { scheduleManualSyncAdvance(100); return; }
            uploadNextManualBatch(edits.mid(0, 200));
            return;
        }
    }
    m_projectSyncDrainingOutbox = false;
    if (!pending.isEmpty()) operation = pending.first().toObject();
    const bool sourceDocumentMove = operation.value(QStringLiteral("action")).toString() == Thoughts::SourceDocumentMove::action;
    if (sourceDocumentMove) {
        // Fedora deletes the old source path. Edits this computer has not
        // published yet would then conflict, so this Sync publishes them first
        // and the move (with every change queued after it) waits for the next Sync.
        const QStringList unpublished = Thoughts::SourceDocumentMove::unpublishedEdits(
            m_docsRoot, QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")), operation);
        if (!unpublished.isEmpty()) {
            Q_EMIT syncProgressChanged(m_activeTraceId, QStringLiteral("preparing"), 0, 0,
                                       QStringLiteral("Moving %1 waits until its edits reach the server; Sync again to move it.")
                                           .arg(operation.value(QStringLiteral("document_name")).toString()));
            operation = {};
        } else {
            QJsonObject staged = operation;
            QString error = Thoughts::SourceDocumentMove::stageTransfers(m_docsRoot, staged);
            if (error.isEmpty() && staged != operation) error = store.updatePending(staged);
            if (!error.isEmpty()) {
                Thoughts::SourceDocumentMove::rollbackTransfers(m_docsRoot, staged);
                store.dropPending(operation.value(QStringLiteral("operation_id")).toString());
                schedulePendingChangesRefresh();
                failManualSync(QStringLiteral("Moving %1 was cancelled; nothing changed. Choose Move to project again.")
                                   .arg(operation.value(QStringLiteral("document_name")).toString()), error);
                return;
            }
            operation = staged;
        }
    }
    if (operation.value(QStringLiteral("action")).toString() == QLatin1String("import_folder")) {
        const auto error = store.prepareFolderUpload(operation.value(QStringLiteral("operation_id")).toString());
        if (!error.isEmpty()) { failManualSync(QStringLiteral("Source folder import could not be prepared."), error); return; }
        operation = store.pending().first().toObject();
    }
    m_projectSyncRunning = true;
    const auto fail = [this, active](const QString &error) { if (!active()) return; m_projectSyncRunning = false; failManualSync(QStringLiteral("Library project synchronization failed."), error); };
    const auto send = [this, operation, fail, active] {
        if (!active()) return;
        QJsonObject payload = Thoughts::SourceDocumentMove::serverPayload(operation); payload.remove(QStringLiteral("local_path")); payload.remove(QStringLiteral("size_bytes"));
        const auto finished = [this, operation, fail, active](QNetworkReply *reply) {
            if (!active()) { reply->deleteLater(); return; }
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) {
                reply->deleteLater();
                if (operation.isEmpty()) { m_projectSyncRunning = false; m_projectSyncReady = true; continueManualSyncAfterOverleaf(); }
                else fail(QStringLiteral("Project changes are saved on this device. Install the Project update on the server, then click Sync."));
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                const QByteArray body = reply->peek(reply->bytesAvailable());
                const auto error = replyErrorInfo(reply);
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (operation.value(QStringLiteral("action")).toString() == QLatin1String("import_folder") && status == 409 && body.contains("source_folder_exists")) {
                    // Fedora already has a folder of that name: take the next
                    // longer name (rule N) here; the next Sync sends it again.
                    const QString refused = Thoughts::LibraryProjectStore::sourceProject(operation);
                    QString renamed;
                    const QString renameError = Thoughts::LibraryProjectStore(m_docsRoot).renameRefusedImportFolder(operation.value(QStringLiteral("operation_id")).toString(), refused, &renamed);
                    reply->deleteLater();
                    m_projectSyncRunning = false;
                    schedulePendingChangesRefresh();
                    failManualSync(QStringLiteral("The server already has a source folder named %1, so the import of %2 was not added.")
                                       .arg(refused, operation.value(QStringLiteral("name")).toString()),
                                   renameError.isEmpty() ? QStringLiteral("The import now uses the folder name %1. Click Sync again to import it.").arg(renamed)
                                                         : renameError);
                    return;
                }
                if (operation.value(QStringLiteral("action")).toString() == QLatin1String("import_folder") && (status == 400 || status == 409 || status == 413 || status == 422))
                    Thoughts::LibraryProjectStore(m_docsRoot).releaseRejectedFolderUpload(operation.value(QStringLiteral("operation_id")).toString());
                if (operation.value(QStringLiteral("action")).toString() == Thoughts::SourceDocumentMove::action && (status == 400 || status == 409 || status == 422)) {
                    // A refused move changed nothing on Fedora. Retrying it unchanged
                    // cannot succeed, and would block every later Sync.
                    Thoughts::SourceDocumentMove::rollbackTransfers(m_docsRoot, operation);
                    Thoughts::LibraryProjectStore(m_docsRoot).dropPending(operation.value(QStringLiteral("operation_id")).toString());
                    schedulePendingChangesRefresh();
                    reply->deleteLater();
                    if (!active()) return;
                    m_projectSyncRunning = false;
                    failManualSync(QStringLiteral("Moving %1 was cancelled; nothing changed. Choose Move to project again.")
                                       .arg(operation.value(QStringLiteral("document_name")).toString()), error.details.isEmpty() ? error.summary : error.details);
                    return;
                }
                reply->deleteLater(); fail(error.summary + QLatin1Char('\n') + error.details); return;
            }
            const auto snapshot = QJsonDocument::fromJson(reply->readAll()).object(); reply->deleteLater();
            // Fedora renamed its default folders: rename this computer's before
            // installing Fedora's manifests or publishing anything.
            QString migrationError, migrationDetails;
            if (!migrateDefaultFoldersIfNeeded(snapshot, &migrationError, &migrationDetails)) {
                m_projectSyncRunning = false;
                failManualSync(migrationError, migrationDetails);
                return;
            }
            // Fedora renamed its connected folders (source folder names spec):
            // rename this computer's before installing Fedora's manifests.
            if (!migrateSourceFoldersIfNeeded(Thoughts::SourceFolderMigration::renamesFrom(snapshot), &migrationError, &migrationDetails)) {
                m_projectSyncRunning = false;
                failManualSync(migrationError, migrationDetails);
                return;
            }
            // A server from before rule N names the import docs-import-<32 hex>: follow it.
            if (const QString adoptError = Thoughts::LibraryProjectStore(m_docsRoot).adoptImportFolderName(snapshot, operation); !adoptError.isEmpty()) {
                fail(adoptError);
                return;
            }
            const QString error = Thoughts::LibraryProjectStore(m_docsRoot).receive(snapshot, operation.value(QStringLiteral("operation_id")).toString());
            if (!error.isEmpty()) { fail(error); return; }
            if (operation.value(QStringLiteral("action")).toString() == Thoughts::SourceDocumentMove::action) {
                const QStringList kept = Thoughts::SourceDocumentMove::finishTransfers(m_docsRoot, operation);
                if (!kept.isEmpty())
                    qWarning() << "Moved document inputs changed after review and were kept at their old place:" << kept;
            }
            m_projectSyncRunning = false;
            schedulePendingChangesRefresh();
            if (operation.isEmpty()) { m_projectSyncReady = true; continueManualSyncAfterOverleaf(); }
            else synchronizeLibraryProjects();
        };
        if (operation.isEmpty()) {
            // Reading the catalogue is the first request of every Sync of the whole Library
            // and changes nothing: a transient failure is asked again. A change is sent once.
            retryingRequest([this] { return m_client->libraryProjects(); }, finished);
        } else {
            QNetworkReply *reply = m_client->editLibraryProject(payload);
            connect(reply, &QNetworkReply::finished, this, [reply, finished] { finished(reply); });
        }
    };
    const QString path = operation.value(QStringLiteral("local_path")).toString();
    if (path.isEmpty()) { send(); return; }
    auto *file = new QFile(path, this);
    const bool folderImport = operation.value(QStringLiteral("action")).toString() == QLatin1String("import_folder");
    const bool htmlImport = operation.value(QStringLiteral("action")).toString() == QLatin1String("import_html");
    if (!file->open(QIODevice::ReadOnly)) { file->deleteLater(); fail(QStringLiteral("The imported copy could not be read.")); return; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(file) || !file->seek(0) || (htmlImport && (file->size() < 1 || file->size() > 100000000))) {
        file->deleteLater(); fail(QStringLiteral("The imported copy could not be verified.")); return;
    }
    const QString digest = QString::fromLatin1(hash.result().toHex());
    if (digest != operation.value(QStringLiteral("sha256")).toString()) { file->deleteLater(); fail(QStringLiteral("The imported copy has changed since it was selected.")); return; }
    auto *upload = m_client->uploadObject(file, digest.toLatin1(), file->size(), htmlImport ? QByteArrayLiteral("text/html") : folderImport ? QByteArrayLiteral("application/vnd.thoughts.folder+json") : QByteArrayLiteral("application/pdf"));
    file->setParent(upload);
    connect(upload, &QNetworkReply::finished, this, [this, upload, operation, digest, send, fail, active, folderImport, htmlImport] {
        if (!active()) { upload->deleteLater(); return; }
        if (upload->error() != QNetworkReply::NoError) { const auto error = replyErrorInfo(upload); upload->deleteLater(); fail(error.summary); return; }
        upload->deleteLater();
        if (htmlImport) { send(); return; }
        if (folderImport) { emitSyncProgress(QStringLiteral("importing_folder"), 0, 0, QStringLiteral("Rendering imported folder")); send(); return; }
        auto *import = m_client->importLibraryPdf({{QStringLiteral("document_id"), operation.value(QStringLiteral("document_id"))}, {QStringLiteral("filename"), operation.value(QStringLiteral("filename"))}, {QStringLiteral("object_sha256"), digest}});
        connect(import, &QNetworkReply::finished, this, [this, import, operation, send, fail, active] {
            if (!active()) { import->deleteLater(); return; }
            if (import->error() != QNetworkReply::NoError) { const auto error = replyErrorInfo(import); import->deleteLater(); fail(error.summary); return; }
            const auto result = QJsonDocument::fromJson(import->readAll()).object();
            import->deleteLater();
            if (result.value(QStringLiteral("document_id")) != operation.value(QStringLiteral("document_id"))) { fail(QStringLiteral("The server returned a different document identifier.")); return; }
            send();
        });
    });
}

bool FedoraRepository::migrateDefaultFoldersIfNeeded(const QJsonObject &librarySnapshot, QString *error, QString *details)
{
    if (!Thoughts::DefaultFolderMigration(defaultFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath)).needed(librarySnapshot)) {
        return true;
    }
    return runDefaultFolderMigration(error, details);
}

bool FedoraRepository::runDefaultFolderMigration(QString *error, QString *details)
{
    if (m_syncCoordinator.running()) {
        emitSyncProgress(QStringLiteral("preparing"), 0, 0, QStringLiteral("Renaming Project folders"));
    }
    // Writers of the rewritten state stop first: the source change tracker
    // and its store reopen from the migrated tables afterwards.
    m_sourceChangeTracker.reset();
    m_sourceChangeTrackerRoot.clear();
    m_desktopSyncStore.reset();
    m_desktopSyncStorePath.clear();
    m_sourceTrackingSettled = false;
    // An obsolete scan may still be committing what it saw before the rename.
    if (!Thoughts::waitForSourceTrackerWork(30000)) {
        qWarning() << "Source tracking work is still running; renaming the default folders anyway.";
    }
    Thoughts::DefaultFolderMigration migration(defaultFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath));
    const auto result = migration.run();

    // In-memory copies of the rewritten records.
    m_cachedRepositoryStateValid = false;
    QFile savedComments(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-snapshot.json")));
    if (savedComments.open(QIODevice::ReadOnly)) {
        const auto saved = QJsonDocument::fromJson(savedComments.readAll()).object();
        if (saved.value(QStringLiteral("docs_root")).toString() == m_docsRoot) {
            m_sourceCommentSnapshot = saved.value(QStringLiteral("entries")).toObject();
            m_sourceCommentFileIndex = saved.value(QStringLiteral("files")).toObject();
        }
    } else {
        m_sourceCommentSnapshot = rewriteLegacyDefaultFolders(m_sourceCommentSnapshot);
        m_sourceCommentFileIndex = rewriteLegacyDefaultFolders(m_sourceCommentFileIndex);
    }
    m_missingCommentMarkers = rewriteLegacyDefaultFolders(m_missingCommentMarkers);
    for (auto iterator = m_renderStatuses.begin(); iterator != m_renderStatuses.end(); ++iterator) {
        iterator.value() = rewriteLegacyDefaultFolders(iterator.value());
    }
    m_renderState = rewriteLegacyDefaultFolders(m_renderState);
    m_reviewProjections = rewriteLegacyDefaultFolders(m_reviewProjections);
    m_pendingChangesSnapshot = rewriteLegacyDefaultFolders(m_pendingChangesSnapshot);
    Thoughts::discardSourceLocationIndex(m_docsRoot);

    QString storeError;
    desktopSyncStore(&storeError);
    scheduleSourceCommentRefresh();
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    if (!result.success) {
        qWarning().noquote() << QStringLiteral("Default-folder rename failed: %1 %2").arg(result.error, result.details);
        if (error) *error = result.error;
        if (details) *details = result.details;
        return false;
    }
    if (result.migrated) {
        qInfo().noquote() << QStringLiteral("Renamed the Project default folders to default/: %1")
                                 .arg(QString::fromUtf8(QJsonDocument(result.report).toJson(QJsonDocument::Compact)));
    }
    if (!storeError.isEmpty()) {
        if (error) *error = QStringLiteral("The local source synchronization state could not be checked safely.");
        if (details) *details = storeError;
        return false;
    }
    return true;
}

bool FedoraRepository::migrateSourceFoldersIfNeeded(const Thoughts::SourceFolderRenames &renames, QString *error, QString *details)
{
    const Thoughts::SourceFolderMigration migration(sourceFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath));
    if (!migration.interrupted() && migration.pending(renames).isEmpty()) {
        return true;
    }
    return runSourceFolderMigration(renames, error, details);
}

bool FedoraRepository::runSourceFolderMigration(const Thoughts::SourceFolderRenames &fedoraRenames, QString *error, QString *details)
{
    if (m_syncCoordinator.running()) {
        emitSyncProgress(QStringLiteral("preparing"), 0, 0, QStringLiteral("Renaming source folders"));
    }
    // Writers of the rewritten state stop first: the source change tracker
    // (its watchers hold the old folders) and its store reopen afterwards.
    m_sourceChangeTracker.reset();
    m_sourceChangeTrackerRoot.clear();
    m_desktopSyncStore.reset();
    m_desktopSyncStorePath.clear();
    m_sourceTrackingSettled = false;
    if (!Thoughts::waitForSourceTrackerWork(30000)) {
        qWarning() << "Source tracking work is still running; renaming the source folders anyway.";
    }
    Thoughts::SourceFolderMigration migration(sourceFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath));
    const auto result = migration.run(fedoraRenames);
    const Thoughts::SourceFolderRenames renames = migration.journalRenames().isEmpty() ? fedoraRenames : migration.journalRenames();
    const auto rewrite = [&renames](const QJsonObject &value) {
        return Thoughts::SourceFolderNames::rewriteJson(value, renames);
    };

    // In-memory copies of the rewritten records.
    m_cachedRepositoryStateValid = false;
    QFile savedComments(QDir(m_stateRoot).filePath(QStringLiteral("source-comment-snapshot.json")));
    bool reloaded = false;
    if (savedComments.open(QIODevice::ReadOnly)) {
        const auto saved = QJsonDocument::fromJson(savedComments.readAll()).object();
        if (saved.value(QStringLiteral("docs_root")).toString() == m_docsRoot) {
            m_sourceCommentSnapshot = saved.value(QStringLiteral("entries")).toObject();
            m_sourceCommentFileIndex = saved.value(QStringLiteral("files")).toObject();
            reloaded = true;
        }
    }
    if (!reloaded) {
        m_sourceCommentSnapshot = rewrite(m_sourceCommentSnapshot);
        m_sourceCommentFileIndex = rewrite(m_sourceCommentFileIndex);
    }
    m_missingCommentMarkers = Thoughts::SourceFolderNames::rewriteJson(m_missingCommentMarkers, renames);
    for (auto iterator = m_renderStatuses.begin(); iterator != m_renderStatuses.end(); ++iterator) {
        iterator.value() = rewrite(iterator.value());
    }
    m_renderState = rewrite(m_renderState);
    m_reviewProjections = rewrite(m_reviewProjections);
    m_pendingChangesSnapshot = rewrite(m_pendingChangesSnapshot);
    Thoughts::discardSourceLocationIndex(m_docsRoot);

    QString storeError;
    desktopSyncStore(&storeError);
    scheduleSourceCommentRefresh();
    schedulePendingChangesRefresh();
    emitEffectiveProjections();
    if (!result.success) {
        qWarning().noquote() << QStringLiteral("Source folder rename failed: %1 %2").arg(result.error, result.details);
        if (error) *error = result.error;
        if (details) *details = result.details;
        return false;
    }
    if (result.migrated) {
        qInfo().noquote() << QStringLiteral("Renamed the source folders: %1").arg(QString::fromUtf8(QJsonDocument(result.report).toJson(QJsonDocument::Compact)));
    }
    if (!storeError.isEmpty()) {
        if (error) *error = QStringLiteral("The local source synchronization state could not be checked safely.");
        if (details) *details = storeError;
        return false;
    }
    return true;
}

void FedoraRepository::checkDefaultFolderMigrationForDocumentSync()
{
    m_defaultFolderMigrationChecked = true;
    Thoughts::DefaultFolderMigration migration(defaultFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath));
    if (migration.interrupted()) {
        QString error, details;
        if (!runDefaultFolderMigration(&error, &details)) { failManualSync(error, details); return; }
        continueManualSyncAfterOverleaf();
        return;
    }
    // The source folder rename: resume it, or apply the map the last full
    // Sync stored in the catalogue, before anything is published.
    // A declared folder of the old form the stored map does not cover may
    // have been renamed on Fedora since: ask its catalogue, at most once per
    // stored catalogue in this app run (full Syncs always fetch it).
    const Thoughts::SourceFolderMigration folders(sourceFolderMigrationPaths(m_docsRoot, m_stateRoot, m_sourceManifestPath));
    const Thoughts::SourceFolderRenames known = Thoughts::LibraryProjectStore::sourceFolderRenames(m_docsRoot);
    const auto folderCheck = folders.documentSyncCheck(known, Thoughts::SourceFolderMigration::catalogueKey(m_docsRoot), &m_folderNamesAskedCatalogue);
    if (folderCheck == Thoughts::SourceFolderMigration::DocumentSyncCheck::ApplyStored) {
        QString error, details;
        if (!runSourceFolderMigration(known, &error, &details)) {
            failManualSync(error, details);
            return;
        }
    }
    const bool askForFolderNames = folderCheck == Thoughts::SourceFolderMigration::DocumentSyncCheck::AskFedora;
    // Nothing on this computer to rename: Fedora's spelling no longer matters here.
    if ((migration.legacyFolders().isEmpty() && !askForFolderNames) || !m_client) {
        continueManualSyncAfterOverleaf();
        return;
    }
    const QString syncTrace = m_activeTraceId;
    auto *reply = m_client->libraryProjects();
    connect(reply, &QNetworkReply::finished, this, [this, reply, syncTrace] {
        reply->deleteLater();
        if (!m_syncCoordinator.running() || m_activeTraceId != syncTrace) return;
        // Without an answer the document Sync goes on as before: a later Sync
        // (or Fedora's path alias) catches up.
        if (reply->error() == QNetworkReply::NoError) {
            const auto snapshot = QJsonDocument::fromJson(reply->readAll()).object();
            QString error, details;
            if (!migrateDefaultFoldersIfNeeded(snapshot, &error, &details)) { failManualSync(error, details); return; }
            if (!migrateSourceFoldersIfNeeded(Thoughts::SourceFolderMigration::renamesFrom(snapshot), &error, &details)) { failManualSync(error, details); return; }
        }
        continueManualSyncAfterOverleaf();
    });
}

void FedoraRepository::continueManualSyncAfterOverleaf()
{
    if (!m_syncCoordinator.isDocument() && !m_projectSyncReady) { synchronizeLibraryProjects(); return; }
    if (m_syncCoordinator.isDocument() && !m_defaultFolderMigrationChecked) { checkDefaultFolderMigrationForDocumentSync(); return; }
    // Flush the durable watcher state before asking Fedora which generation
    // is authoritative. Otherwise a slow local hash can finish after the
    // state response and make that response stale before we freeze the local
    // work it was meant to describe.
    QString trackerError;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&trackerError);
    if (!store || !trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The local source synchronization state could not be checked safely."), trackerError);
        return;
    }
    if (m_sourceChangeTracker && !m_sourceChangeTracker->trackedPaths().isEmpty()
        && !m_syncCoordinator.localFullReconciliationStarted) {
        m_syncCoordinator.localFullReconciliationStarted = true;
        m_syncCoordinator.phase = ManualSyncPhase::LocalReconciliation;
        m_syncCoordinator.localReconciliationStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        emitSyncProgress(QStringLiteral("checking_local_changes"), 0, 0, QStringLiteral("Checking local changes"));
        m_sourceChangeTracker->reconcileAsync();
        scheduleManualSyncAdvance(100);
        return;
    }
    if (m_sourceChangeTracker && !m_sourceChangeTracker->trackedPaths().isEmpty() && m_sourceChangeTracker->reconciliationInProgress()) {
        m_syncCoordinator.phase = ManualSyncPhase::LocalReconciliation;
        m_syncCoordinator.localReconciliationStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        emitSyncProgress(QStringLiteral("checking_local_changes"), 0, 0, QStringLiteral("Checking local changes"));
        m_sourceChangeTracker->reconcileAsync();
        scheduleManualSyncAdvance(100);
        return;
    }
    scheduleManualSyncAdvance();
}

void FedoraRepository::scheduleManualSyncAdvance(int delayMs)
{
    if (!m_syncCoordinator.running()) {
        return;
    }
    QTimer::singleShot(qMax(0, delayMs), this, &FedoraRepository::advanceManualSync);
}

void FedoraRepository::wakeManualSyncAfterBackgroundActivity()
{
    if (m_syncCoordinator.running()) {
        scheduleManualSyncAdvance();
    }
}

void FedoraRepository::advanceManualSync()
{
    if (m_overleafSyncRunning || !m_syncCoordinator.running()) {
        return;
    }

    const bool documentScope = m_syncCoordinator.isDocument();
    const bool libraryBusy = m_libraryPulling || m_agentProposalBundlePulling || m_libraryPreparing || m_libraryActivating || m_projectingPdf;
    if (m_uploading || m_pulling || m_projectingPdf || (!documentScope && m_syncCoordinator.phase != ManualSyncPhase::Library && libraryBusy)) {
        scheduleManualSyncAdvance(100);
        return;
    }
    if (m_projectSyncDrainingOutbox) {
        synchronizeLibraryProjects();
        return;
    }
    // Library project changes go first and resume the Sync when Fedora has
    // applied them. A stray wake-up must not check the server state earlier.
    if (!documentScope && !m_projectSyncReady) {
        return;
    }

    switch (m_syncCoordinator.phase) {
    case ManualSyncPhase::ServerState:
        checkManualServerState();
        break;
    case ManualSyncPhase::LocalReconciliation:
        resumeManualSyncAfterSourceReconciliation();
        break;
    case ManualSyncPhase::AuthoringReconciliation:
        break; // The asynchronous source installer resumes the captured state.
    case ManualSyncPhase::SourcePublication:
        if (m_publishingSources) {
            return;
        } else if (readRepositoryState().value(QStringLiteral("source_publication_requested")).toBool()) {
            startPendingSourcePublication();
        } else if (m_syncCoordinator.hasSourcePublication()) {
            startManualSourcePublication();
        } else {
            m_syncCoordinator.phase = ManualSyncPhase::DrainOutbox;
            scheduleManualSyncAdvance();
        }
        break;
    case ManualSyncPhase::DrainOutbox: {
        QString error;
        const QList<QJsonObject> operations = m_outbox->operations(&error);
        if (!error.isEmpty()) {
            failManualSync(error);
        } else {
            QList<QJsonObject> selected = documentScope ? documentOperations(m_syncCoordinator.target, operations) : operations;
            if (holdUnsupportedOperations(selected)) break;
            if (!selected.isEmpty()) {
                uploadNextManualBatch(selected.mid(0, 200));
                break;
            }
            const bool hasSourceWork = readRepositoryState().value(QStringLiteral("source_publication_requested")).toBool() || m_syncCoordinator.hasSourcePublication();
            if (hasSourceWork) {
                m_syncCoordinator.postLocalWorkAction = SyncCoordinator::PostLocalWorkAction::ConditionalRenderCheck;
                m_syncCoordinator.phase = ManualSyncPhase::SourcePublication;
            } else if (!documentScope) {
                m_syncCoordinator.phase = ManualSyncPhase::LibraryMutations;
            } else if (m_syncCoordinator.conditionalRenderCheck) {
                m_syncCoordinator.phase = ManualSyncPhase::RenderCheck;
            } else {
                beginManualReaderStateRecheck();
                break;
            }
            scheduleManualSyncAdvance();
        }
        break;
    }
    case ManualSyncPhase::LibraryMutations:
        if (!documentScope) {
            processNextManualLibraryMutation();
        }
        break;
    case ManualSyncPhase::RemoteChanges:
        if (!documentScope) {
            pullPlanlessManualChangePage();
        }
        break;
    case ManualSyncPhase::ProjectionSnapshot:
        if (!documentScope) {
            pullManualProjectionSnapshot();
        }
        break;
    case ManualSyncPhase::ChangePages:
    case ManualSyncPhase::FinalChangePages:
        if (!documentScope) {
            pullManualChangePage();
        }
        break;
    case ManualSyncPhase::RenderCheck:
        startManualRenderCheck();
        break;
    case ManualSyncPhase::WaitBuild:
        pollBuilds();
        break;
    case ManualSyncPhase::Library:
        if (documentScope) {
            break;
        } else if (m_syncCoordinator.libraryRefreshState == SyncCoordinator::LibraryRefreshState::NotRequested) {
            m_syncCoordinator.libraryRefreshState = SyncCoordinator::LibraryRefreshState::Waiting;
            refreshLibrary();
        } else if (m_syncCoordinator.libraryRefreshState == SyncCoordinator::LibraryRefreshState::Ready && !libraryBusy) {
            if (m_syncCoordinator.libraryLatestServerSequence < 0 || m_syncCoordinator.libraryLatestServerSequence > m_outbox->cursor()) {
                m_syncCoordinator.phase = ManualSyncPhase::FinalChangePages;
                scheduleManualSyncAdvance();
            } else {
                completeManualSync();
            }
        }
        break;
    case ManualSyncPhase::None:
        break;
    case ManualSyncPhase::DocumentSnapshot:
        if (documentScope) {
            pullDocumentSnapshot();
        }
        break;
    }
}

void FedoraRepository::checkManualServerState()
{
    if (!m_client || m_pulling || !m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::ServerState) {
        return;
    }
    m_pulling = true;
    QElapsedTimer requestTimer;
    requestTimer.start();
    // The state endpoint is itself the capability check. Avoid a separate
    // /ready round trip on every unchanged synchronization.
    retryingRequest([this] { return m_client->syncState(); }, [this, requestTimer](QNetworkReply *stateReply) {
        m_pulling = false;
        const qint64 requestMs = requestTimer.elapsed();
        if (stateReply->error() != QNetworkReply::NoError) {
            const int status = stateReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const Thoughts::SyncErrorInfo error = replyErrorInfo(stateReply);
            stateReply->deleteLater();
            if (status == 404 || status == 426) {
                failManualSync(QStringLiteral("The server does not support the required sync state v2 contract."), error.details, QStringLiteral("needs_user"));
            } else {
                failManualSync(error.summary, error.details);
            }
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(stateReply->readAll());
        stateReply->deleteLater();
        if (!document.isObject() || !applyManualServerState(document.object())) {
            failManualSync(QStringLiteral("The server returned an invalid sync state."));
            return;
        }
        emitSyncTiming(QStringLiteral("server_state"), QStringLiteral("server state request %1 ms, local freeze and decision %2 ms")
                                                           .arg(requestMs).arg(requestTimer.elapsed() - requestMs));
    });
}

bool FedoraRepository::applyManualServerState(const QJsonObject &serverState)
{
    const qint64 latestSequence = serverState.value(QStringLiteral("latest_server_sequence")).toInteger(-1);
    const qint64 sourceFrontier = serverState.value(QStringLiteral("source_frontier")).toInteger(-1);
    const QString libraryRevision = serverState.value(QStringLiteral("library_revision")).toString();
    if (!m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::ServerState || serverState.value(QStringLiteral("contract_version")).toInt() != 2 || latestSequence < 0 || sourceFrontier < 0 ||
        !validSha256(libraryRevision)) {
        return false;
    }

    m_syncCoordinator.serverState = serverState;
    QString trackerError;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&trackerError);
    if (!store || !trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The local source synchronization state could not be checked safely."), trackerError);
        return true;
    }
    if (m_sourceChangeTracker && !m_sourceChangeTracker->trackedPaths().isEmpty() && m_sourceChangeTracker->reconciliationInProgress()) {
        m_syncCoordinator.phase = ManualSyncPhase::LocalReconciliation;
        m_syncCoordinator.localReconciliationStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        emitSyncProgress(QStringLiteral("checking_local_changes"), 0, 0, QStringLiteral("Checking local changes"));
        scheduleManualSyncAdvance(100);
        return true;
    }
    const Thoughts::DesktopSyncFrontier authoringFrontier = store->frontier(&trackerError);
    if (!trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The editable-source checkpoint could not be read."), trackerError);
        return true;
    }
    const QJsonObject manifestIdentity = sourceManifestFileIdentity(m_sourceManifestPath, false, &trackerError);
    const bool checkedManifestMatches = sameSourceManifestFileMetadata(authoringFrontier.metadata.value(QStringLiteral("materialized_source_manifest")).toObject(), manifestIdentity);
    const qint64 materialized = checkedManifestMatches ? authoringFrontier.metadata.value(QStringLiteral("materialized_source_frontier")).toInteger(-1) : -1;
    const bool sourceInventoryReady = sourceInventoryCoversLibrary(m_docsRoot, m_sourceManifestPath, m_syncCoordinator.isDocument() ? m_syncCoordinator.target : QString())
        && (checkedManifestMatches || sourceManifestPathIsProjectScoped(m_sourceManifestPath, &trackerError));
    if (sourceInventoryReady && m_syncCoordinator.authoringCheckedSourceFrontier < sourceFrontier && materialized < sourceFrontier) {
        startManualAuthoringReconciliation(sourceFrontier);
        return true;
    }
    m_syncCoordinator.authoringCheckedSourceFrontier = std::max(m_syncCoordinator.authoringCheckedSourceFrontier, materialized);
    if (m_syncCoordinator.serverStatePurpose == SyncCoordinator::ServerStatePurpose::ReaderRecheck) {
        QString frontierError;
        const qint64 expectedSourceFrontier = manualExpectedSourceFrontier(&frontierError);
        if (!frontierError.isEmpty()) {
            failManualSync(QStringLiteral("The checked source generation could not be verified safely."), frontierError);
            return true;
        }
        m_syncCoordinator.serverStatePurpose = SyncCoordinator::ServerStatePurpose::Initial;
        if (!sourceInventoryCoversLibrary(m_docsRoot, m_sourceManifestPath, m_syncCoordinator.isDocument() ? m_syncCoordinator.target : QString())) {
            // The download may have activated a different set of projects.
            // Re-enter discovery before declaring this Sync complete.
            if (++m_syncCoordinator.discoveryReentries > maximumDiscoveryReentries) {
                failManualSync(QStringLiteral("Sync stopped because the local source inventory still does not match the Library after %1 attempts.").arg(maximumDiscoveryReentries),
                               QStringLiteral("Library projects and the accepted source manifest disagree about which documents this device publishes."));
                return true;
            }
            // The Project catalogue was read before this Sync published; a
            // Project or document it just published is missing from it until
            // the catalogue is read again.
            m_projectSyncReady = false;
            m_syncCoordinator.phase = ManualSyncPhase::ServerState;
            continueManualSyncAfterOverleaf();
            return true;
        }
        // A matching reader cache says nothing about whether its PDFs need
        // rebuilding. Validate this source generation once per manual Sync.
        if (expectedSourceFrontier == sourceFrontier && m_syncCoordinator.conditionalRenderCheck && m_syncCoordinator.renderCheckedSourceFrontier < sourceFrontier) {
            m_syncCoordinator.phase = ManualSyncPhase::RenderCheck;
            scheduleManualSyncAdvance();
        } else if (expectedSourceFrontier == sourceFrontier && manualReaderStateCanFinish(serverState)) {
            emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("No Library transfer is needed"));
            completeManualSync(true);
        } else if (expectedSourceFrontier == sourceFrontier && !m_syncCoordinator.isDocument() && canPullPlanlessRemoteChanges(serverState)) {
            m_syncCoordinator.remoteLatestServerSequence = latestSequence;
            m_syncCoordinator.phase = ManualSyncPhase::RemoteChanges;
            emitSyncProgress(QStringLiteral("downloading_updates"), 0, 0, QStringLiteral("Downloading remote changes"));
            scheduleManualSyncAdvance();
        } else if (expectedSourceFrontier != sourceFrontier && m_syncCoordinator.conditionalRenderCheck) {
            // Re-run the initial decision against this fresh state. It will
            // drain any newly queued local work and then conditionally check
            // the PDFs. The render response records the exact source frontier
            // it validated, so a later recheck cannot loop or accept a newer
            // unchecked generation.
            emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("Sources on the server changed; checking affected PDFs"));
            if (++m_syncCoordinator.discoveryReentries > maximumDiscoveryReentries) {
                failManualSync(QStringLiteral("Sync stopped because sources on the server kept changing during %1 checks. Sync again when other devices finish.").arg(maximumDiscoveryReentries), QString(), QStringLiteral("transient"));
                return true;
            }
            return applyManualServerState(serverState);
        } else {
            m_syncCoordinator.phase = !m_syncCoordinator.isDocument() ? ManualSyncPhase::Library : ManualSyncPhase::DocumentSnapshot;
            m_syncCoordinator.libraryRefreshState = SyncCoordinator::LibraryRefreshState::NotRequested;
            scheduleManualSyncAdvance();
        }
        return true;
    }
    // The local sources are checked: forget the decisions no upload can carry
    // now, whether or not this Sync publishes anything.
    dropInapplicableProposalClaims();
    const QJsonObject state = readRepositoryState();
    const Thoughts::DesktopSyncSummary sourceSummary = store->summary(&trackerError);
    bool manifestMatches = false;
    if (store && trackerError.isEmpty() && !sourceManifestMatchesAccepted(store, &manifestMatches, &trackerError)) {
        manifestMatches = false;
    }
    if (store && trackerError.isEmpty() && sourceSummary.status != Thoughts::DesktopSyncStatus::Clean) {
        m_syncCoordinator.sourceSnapshot = std::make_unique<Thoughts::DesktopSyncSnapshot>(store->freezeForPublication(m_syncCoordinator.sourcePublicationTraceId, &trackerError));
    } else {
        m_syncCoordinator.sourceSnapshot.reset();
    }
    if (!trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The local source synchronization state could not be frozen safely."), trackerError);
        return true;
    }

    const Thoughts::DesktopSyncSnapshot emptySnapshot;
    QHash<QString, QString> projectsByTarget = sourceManifestProjectsByTarget(m_sourceManifestPath);
    QFile projectCatalogue(QDir(m_docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json")));
    const QJsonObject catalogue = projectCatalogue.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(projectCatalogue.readAll()).object() : QJsonObject();
    for (const QJsonValue &project : catalogue.value(QStringLiteral("projects")).toArray()) {
        for (const QJsonValue &value : project.toObject().value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = value.toObject();
            const QString filename = document.value(QStringLiteral("filename")).toString();
            const QString owner = document.value(QStringLiteral("source_project")).toString();
            if (safePdfFilename(filename) && safeSourceOwner(owner)) projectsByTarget.insert(filename, owner);
        }
    }
    Internal::SourcePublicationDecision sourceDecision = Internal::sourcePublicationDecision(store != nullptr,
                                                                                             m_sourceChangeTracker && m_sourceChangeTracker->authoritative(),
                                                                                             manifestMatches,
                                                                                             sourceSummary.status,
                                                                                             m_syncCoordinator.sourceSnapshot ? *m_syncCoordinator.sourceSnapshot : emptySnapshot,
                                                                                             m_syncCoordinator.target,
                                                                                             projectsByTarget);
    if ((sourceDecision.mode == Internal::SourcePublicationMode::Scoped || sourceDecision.mode == Internal::SourcePublicationMode::ProjectScoped)
        && m_syncCoordinator.sourceSnapshot) {
        // A Project file that newly declares a default-folder document: its PDF
        // was never built, so no earlier observation ties it to this change, and
        // a project-scoped publication for another document sends no build
        // inputs for it (Fedora then fails "affected target has no exact build
        // input"). A global publication discovers every declared document.
        QSet<QString> published;
        QFile index(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
        if (index.open(QIODevice::ReadOnly)) {
            for (const QJsonValue &value : QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray())
                published.insert(value.toObject().value(QStringLiteral("filename")).toString());
        }
        if (Internal::pendingProjectFileDeclaresUnbuiltDefaultFolderDocument(m_docsRoot, *m_syncCoordinator.sourceSnapshot, published)) {
            sourceDecision = {};
            sourceDecision.mode = Internal::SourcePublicationMode::Global;
        }
    }
    if (!m_syncCoordinator.pendingAuthoringPaths.isEmpty() && sourceDecision.mode == Internal::SourcePublicationMode::None) {
        // A legacy watcher baseline may already match local bytes while the
        // true authoring base still differs. The durable installer obligation
        // must not disappear merely because that older queue is empty.
        if (!m_syncCoordinator.isDocument()) sourceDecision.mode = Internal::SourcePublicationMode::Global;
        else {
            const QString owner = projectsByTarget.value(m_syncCoordinator.target);
            const bool owned = safeSourceOwner(owner) && std::any_of(m_syncCoordinator.pendingAuthoringPaths.cbegin(), m_syncCoordinator.pendingAuthoringPaths.cend(),
                [&](const QString &path) { return path.startsWith(owner + QLatin1Char('/')); });
            const bool affected = owned && safeSourceProject(owner);
            // A default folder publishes globally; it has no project-scoped frozen publication.
            if (owned && !affected) sourceDecision.mode = Internal::SourcePublicationMode::Global;
            if (affected) {
                sourceDecision.mode = Internal::SourcePublicationMode::ProjectScoped;
                sourceDecision.targets = {m_syncCoordinator.target};
                sourceDecision.projectsByTarget.insert(m_syncCoordinator.target, owner);
            }
        }
    }
    if (!m_syncCoordinator.blockedAuthoringPaths.isEmpty() && sourceDecision.mode != Internal::SourcePublicationMode::None) {
        QSet<QString> blockedTargets;
        const auto baselines = store->baselines(&trackerError);
        for (const QString &path : std::as_const(m_syncCoordinator.blockedAuthoringPaths)) {
            for (const Thoughts::DesktopTrackedPath &baseline : baselines) {
                if (baseline.path == path) blockedTargets.unite(QSet<QString>(baseline.affectedTargets.cbegin(), baseline.affectedTargets.cend()));
            }
            for (auto entry = projectsByTarget.cbegin(); entry != projectsByTarget.cend(); ++entry) {
                if (path.startsWith(entry.value() + QLatin1Char('/'))) blockedTargets.insert(entry.key());
            }
        }
        if (!trackerError.isEmpty() || blockedTargets.isEmpty()) {
            failManualSync(QStringLiteral("The affected documents could not be identified safely."), trackerError);
            return true;
        }
        sourceDecision = Internal::sourcePublicationWithoutBlockedTargets(sourceDecision,
                                                                          m_syncCoordinator.sourceSnapshot ? *m_syncCoordinator.sourceSnapshot : emptySnapshot,
                                                                          blockedTargets,
                                                                          projectsByTarget,
                                                                          m_syncCoordinator.target);
        if (!m_syncCoordinator.sourceSnapshot) m_syncCoordinator.sourceSnapshot = std::make_unique<Thoughts::DesktopSyncSnapshot>();
    }
    if (sourceDecision.mode == Internal::SourcePublicationMode::ProjectScoped && !m_syncCoordinator.sourceSnapshot)
        m_syncCoordinator.sourceSnapshot = std::make_unique<Thoughts::DesktopSyncSnapshot>();
    m_syncCoordinator.setSourcePublication(sourceDecision);
    if (sourceDecision.mode != Internal::SourcePublicationMode::None) {
        if (!m_syncCoordinator.sourcePublicationClock.isValid()) {
            m_syncCoordinator.sourcePublicationClock.start();
        }
        emitSyncTiming(QStringLiteral("source_plan"), sourceDecision.mode == Internal::SourcePublicationMode::Global
                                                           ? QStringLiteral("source publication plan: all sources")
                                                           : QStringLiteral("source publication plan: %1 document(s) with pending changes").arg(sourceDecision.targets.size()));
    }
    if (sourceDecision.mode == Internal::SourcePublicationMode::None && !state.value(QStringLiteral("source_publication_requested")).toBool()) {
        m_syncCoordinator.sourceSnapshot.reset();
    }

    QString outboxError;
    const QList<QJsonObject> pendingOperations = m_outbox->operations(&outboxError);
    if (!outboxError.isEmpty()) {
        failManualSync(outboxError);
        return true;
    }
    const Thoughts::DesktopSyncFrontier frontier = store->frontier(&trackerError);
    if (!trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The local source synchronization state could not be read."), trackerError);
        return true;
    }
    const QList<Thoughts::DesktopLibraryOperation> pendingLibraryOperations = store->libraryOperations(false, &trackerError);
    if (!trackerError.isEmpty()) {
        failManualSync(QStringLiteral("The local Library queue could not be read."), trackerError);
        return true;
    }
    const bool noLocalWork = pendingOperations.isEmpty() && pendingLibraryOperations.isEmpty() && !state.value(QStringLiteral("source_publication_requested")).toBool() && sourceDecision.mode == Internal::SourcePublicationMode::None &&
        !m_syncCoordinator.sourceSnapshot;
    const auto completionDecision = Thoughts::SyncCore::nextAction({
        {QStringLiteral("authoring_checked"), m_syncCoordinator.authoringCheckedSourceFrontier >= sourceFrontier},
        {QStringLiteral("logical_pending"), !pendingOperations.isEmpty() || !pendingLibraryOperations.isEmpty()},
        {QStringLiteral("sources_pending"), !noLocalWork},
        {QStringLiteral("build_needed"), frontier.sourceFrontier != sourceFrontier},
        {QStringLiteral("artifacts_current"), manualReaderStateCanFinish(serverState)},
        {QStringLiteral("documents_blocked"), !m_syncCoordinator.blockedAuthoringPaths.isEmpty()},
    });
    const bool localStateMatches = completionDecision.action == QLatin1String("complete");
    const bool logicalOnlyFastPath = !m_syncCoordinator.isDocument() && !pendingOperations.isEmpty() && pendingLibraryOperations.isEmpty() && !state.value(QStringLiteral("source_publication_requested")).toBool() &&
        sourceDecision.mode == Internal::SourcePublicationMode::None && !m_syncCoordinator.sourceSnapshot && frontier.sourceFrontier == sourceFrontier && manualReaderArtifactsCurrent(serverState);
    m_syncCoordinator.postLocalWorkAction = logicalOnlyFastPath ? SyncCoordinator::PostLocalWorkAction::ReaderStateRecheck : SyncCoordinator::PostLocalWorkAction::ConditionalRenderCheck;
    if (!m_syncCoordinator.isDocument() && noLocalWork
        && (localStateMatches || (m_syncCoordinator.conditionalRenderCheck && m_outbox->cursor() == latestSequence && manualReaderArtifactsCurrent(serverState)))) {
        if (m_syncCoordinator.conditionalRenderCheck && m_syncCoordinator.renderCheckedSourceFrontier < sourceFrontier) {
            m_syncCoordinator.phase = ManualSyncPhase::RenderCheck;
            scheduleManualSyncAdvance();
            return true;
        }
        emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("Already up to date"));
        completeManualSync(true);
        return true;
    }
    if (!m_syncCoordinator.isDocument() && noLocalWork && frontier.sourceFrontier == sourceFrontier && canPullPlanlessRemoteChanges(serverState)) {
        m_syncCoordinator.remoteLatestServerSequence = latestSequence;
        m_syncCoordinator.phase = ManualSyncPhase::RemoteChanges;
        emitSyncProgress(QStringLiteral("downloading_updates"), 0, 0, QStringLiteral("Downloading remote changes"));
        scheduleManualSyncAdvance();
        return true;
    }

    m_syncCoordinator.phase = ManualSyncPhase::DrainOutbox;
    scheduleManualSyncAdvance();
    return true;
}

void FedoraRepository::startManualAuthoringReconciliation(qint64 through)
{
    const QByteArray token = sourcePublisherToken();
    if (token.isEmpty() && !m_sourcePublisherTokenWithheld.isEmpty()) {
        failManualSync(QStringLiteral("The source-publisher token belongs to another server."), m_sourcePublisherTokenWithheld, QStringLiteral("needs_user"));
        return;
    }
    const QString executable = sourcePublisherExecutable();
    if (token.isEmpty() || executable.isEmpty()) {
        failManualSync(QStringLiteral("Editable sources could not be synchronized: the source publisher installed with Thoughts is missing."));
        return;
    }
    auto *process = new QProcess(this);
    process->setProcessEnvironment(Internal::sourcePublisherProcessEnvironment(token, m_syncCoordinator.sourcePublicationTraceId));
    // The publisher runs the installer ("-m thoughts_server.source_reconciliation" is its command name).
    process->setProgram(m_sourceReconciliationProgram.isEmpty() ? executable : m_sourceReconciliationProgram);
    QStringList arguments {QStringLiteral("-m"), QStringLiteral("thoughts_server.source_reconciliation")};
    arguments.append(sourcePublicationArguments(QString()));
    arguments.append({QStringLiteral("--through"), QString::number(through)});
    process->setArguments(arguments);
    m_syncCoordinator.phase = ManualSyncPhase::AuthoringReconciliation;
    m_publishingSources = true;
    emitSyncProgress(QStringLiteral("installing_sources"), 0, 0, QStringLiteral("Checking and installing changes into editable sources"));
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    timeout->start(120000);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_publishingSources = false;
            failManualSync(QStringLiteral("The editable-source installer could not start."), process->errorString());
            process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, this, [this, process, through](int exitCode, QProcess::ExitStatus status) {
        m_publishingSources = false;
        const QByteArray output = process->readAllStandardOutput();
        const QString detail = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        if (!m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::AuthoringReconciliation) return;
        const QJsonObject report = QJsonDocument::fromJson(output).object();
        if (status != QProcess::NormalExit || exitCode != 0 || report.value(QStringLiteral("schema_version")).toInt() != 1 || report.value(QStringLiteral("checked_frontier")).toInteger(-1) < through) {
            failManualSync(QStringLiteral("Editable-source synchronization needs attention."), detail, processFailureKind(detail));
            return;
        }
        const qint64 downloaded = report.value(QStringLiteral("downloaded_bytes")).toInteger(-1);
        if (downloaded < 0 || !commitManualTransferTotals(QStringLiteral("authoring:%1").arg(through), 0, downloaded)) {
            failManualSync(QStringLiteral("The editable-source transfer receipt is invalid."));
            return;
        }
        QJsonObject summary = report;
        summary.remove(QStringLiteral("verified"));
        QJsonArray decisions;
        for (const QJsonValue &value : report.value(QStringLiteral("decisions")).toArray()) {
            if (value.toObject().value(QStringLiteral("action")).toString() != QLatin1String("acknowledge")) decisions.append(value);
        }
        summary.insert(QStringLiteral("decisions"), decisions);
        QJsonObject event {{QStringLiteral("metadata"), summary},
                           {QStringLiteral("component"), QStringLiteral("source_installer")},
                           {QStringLiteral("duration_ms"), report.value(QStringLiteral("duration_ms"))},
                           {QStringLiteral("message"), QStringLiteral("Editable sources: %1 installed, %2 checked, %3 need attention")
                                .arg(report.value(QStringLiteral("installed_files")).toInteger())
                                .arg(report.value(QStringLiteral("hashed_files")).toInteger())
                                .arg(report.value(QStringLiteral("blocked_paths")).toArray().size())}};
        event.insert(QStringLiteral("phase"), QStringLiteral("installing_sources"));
        event.insert(QStringLiteral("state"), report.value(QStringLiteral("blocked_paths")).toArray().isEmpty() ? QStringLiteral("succeeded") : QStringLiteral("needs_attention"));
        if (!m_activeTraceId.isEmpty()) Q_EMIT syncJournalEvent(m_activeTraceId, event);
        QString error;
        Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
        if (!store || !error.isEmpty()) {
            failManualSync(QStringLiteral("The installed source receipt could not be recorded."), error);
            return;
        }
        QList<Thoughts::DesktopTrackedPath> baselines = store->baselines(&error);
        QSet<QString> acknowledged;
        Thoughts::DesktopSyncSnapshot confirmed = store->freezeForPublication(m_syncCoordinator.sourcePublicationTraceId, &error);
        if (!error.isEmpty()) {
            failManualSync(QStringLiteral("The installed source baseline could not be recorded."), error);
            return;
        }
        for (const QJsonValue &value : report.value(QStringLiteral("verified")).toArray()) {
            const QJsonObject receipt = value.toObject();
            const QString path = receipt.value(QStringLiteral("path")).toString();
            const bool tracked = std::any_of(baselines.cbegin(), baselines.cend(), [&path](const Thoughts::DesktopTrackedPath &baseline) { return baseline.path == path; });
            if (!tracked && receipt.value(QStringLiteral("action")).toString() != QLatin1String("auto_merge")) {
                // A source Fedora created at a new path (another device's new
                // file, or a document moved into this folder) was just installed.
                // The watcher saw it appear; it is Fedora's bytes, not an edit.
                const auto pending = std::find_if(confirmed.files.cbegin(), confirmed.files.cend(), [&path](const Thoughts::DesktopPendingFile &file) { return file.path == path; });
                const QJsonObject identity = sourceManifestFileIdentity(QDir(m_docsRoot).filePath(path), true, &error);
                if (error.isEmpty() && receipt.value(QStringLiteral("exists")).toBool()
                    && identity.value(QStringLiteral("exists")).toBool() && identity.value(QStringLiteral("sha256")).toString() == receipt.value(QStringLiteral("sha256")).toString()) {
                    Thoughts::DesktopTrackedPath baseline;
                    baseline.path = path;
                    baseline.exists = true;
                    baseline.sha256 = receipt.value(QStringLiteral("sha256")).toString();
                    baseline.sizeBytes = identity.value(QStringLiteral("size_bytes")).toInteger(-1);
                    baseline.modifiedAtMs = identity.value(QStringLiteral("modified_at_ms")).toInteger(-1);
                    baseline.metadata = Internal::installedNewSourceMetadata(path, baselines);
                    baseline.metadata.insert(QStringLiteral("_local_file_identity"), identity.value(QStringLiteral("file_identity")));
                    baseline.metadata.insert(QStringLiteral("_local_ctime_ms"), identity.value(QStringLiteral("metadata_changed_at_ms")));
                    if (pending != confirmed.files.cend()) baseline.affectedTargets = pending->affectedTargets;
                    qInfo().noquote() << "Accepted a source the server installed at a new path:" << path << (pending != confirmed.files.cend() ? "(was pending)" : "");
                    baselines.append(baseline);
                    acknowledged.insert(path);
                }
                error.clear();
                continue;
            }
            for (Thoughts::DesktopTrackedPath &baseline : baselines) {
                if (baseline.path != path) continue;
                const QJsonObject identity = sourceManifestFileIdentity(QDir(m_docsRoot).filePath(path), true, &error);
                if (!error.isEmpty()) break;
                const bool exists = receipt.value(QStringLiteral("exists")).toBool();
                if (receipt.value(QStringLiteral("action")).toString() == QLatin1String("auto_merge")) {
                    // The file now holds this device's edit merged onto
                    // Fedora's version. That version is the new authoring
                    // base; the merged edit stays pending and is published
                    // against it. Without this the pending edit keeps the
                    // pre-merge base, which the publisher rightly rejects.
                    if (!identity.value(QStringLiteral("exists")).toBool()
                        || identity.value(QStringLiteral("sha256")).toString() != receipt.value(QStringLiteral("local_sha256")).toString()) break;
                    baseline.exists = true;
                    baseline.sha256 = receipt.value(QStringLiteral("sha256")).toString();
                    baseline.sizeBytes = -1;
                    baseline.modifiedAtMs = -1;
                    baseline.metadata.remove(QStringLiteral("_local_file_identity"));
                    baseline.metadata.remove(QStringLiteral("_local_ctime_ms"));
                    break;
                }
                if (identity.value(QStringLiteral("exists")).toBool() != exists ||
                    (exists && identity.value(QStringLiteral("sha256")).toString() != receipt.value(QStringLiteral("sha256")).toString())) continue;
                baseline.exists = exists;
                baseline.sha256 = receipt.value(QStringLiteral("sha256")).toString();
                baseline.sizeBytes = identity.value(QStringLiteral("size_bytes")).toInteger(-1);
                baseline.modifiedAtMs = identity.value(QStringLiteral("modified_at_ms")).toInteger(-1);
                baseline.metadata.insert(QStringLiteral("_local_file_identity"), identity.value(QStringLiteral("file_identity")));
                baseline.metadata.insert(QStringLiteral("_local_ctime_ms"), identity.value(QStringLiteral("metadata_changed_at_ms")));
                acknowledged.insert(path);
                break;
            }
        }
        confirmed.files.removeIf([&acknowledged](const Thoughts::DesktopPendingFile &file) { return !acknowledged.contains(file.path); });
        if (!error.isEmpty() || !store->acceptBaselinesAndAcknowledge(baselines, confirmed, &error)) {
            failManualSync(QStringLiteral("The installed source baseline could not be recorded."), error);
            return;
        }
        // Baselines describe accepted server bytes; observations retain current
        // local identities, pending edits and discovered paths. Preserve that
        // inventory instead of forcing unrelated sources to be hashed again.
        const QList<Thoughts::DesktopTrackedPath> trackingPaths = store->trackingPaths(&error);
        if (!error.isEmpty() || (m_sourceChangeTracker && !m_sourceChangeTracker->setTrackedPaths(trackingPaths, &error))) {
            failManualSync(QStringLiteral("The installed source baseline could not be recorded."), error);
            return;
        }
        const qint64 checked = report.value(QStringLiteral("checked_frontier")).toInteger();
        m_syncCoordinator.authoringCheckedSourceFrontier = checked;
        m_syncCoordinator.blockedAuthoringPaths.clear();
        m_syncCoordinator.pendingAuthoringPaths.clear();
        for (const QJsonValue &path : report.value(QStringLiteral("pending_paths")).toArray()) m_syncCoordinator.pendingAuthoringPaths.append(path.toString());
        const QString scopedOwner = sourceManifestProjectsByTarget(m_sourceManifestPath).value(m_syncCoordinator.target);
        for (const QJsonValue &path : report.value(QStringLiteral("blocked_paths")).toArray()) {
            if (!m_syncCoordinator.isDocument() || scopedOwner.isEmpty() || path.toString().startsWith(scopedOwner + QLatin1Char('/')))
                m_syncCoordinator.blockedAuthoringPaths.append(path.toString());
        }
        if (report.value(QStringLiteral("complete")).toBool()) {
            Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
            const QJsonObject manifest = sourceManifestFileIdentity(m_sourceManifestPath, true, &error);
            if (manifest.value(QStringLiteral("sha256")).toString() != report.value(QStringLiteral("manifest_sha256")).toString()) {
                failManualSync(QStringLiteral("The managed source inventory changed during synchronization; sync again to include it."));
                return;
            }
            frontier.metadata.insert(QStringLiteral("materialized_source_manifest"), manifest);
            frontier.metadata.insert(QStringLiteral("materialized_source_frontier"), checked);
            if (!error.isEmpty() || !store->setFrontier(frontier, &error)) {
                failManualSync(QStringLiteral("The editable-source checkpoint could not be saved."), error);
                return;
            }
        }
        m_syncCoordinator.phase = ManualSyncPhase::ServerState;
        applyManualServerState(m_syncCoordinator.serverState);
    });
    process->start();
}

void FedoraRepository::noteVerifiedSourcePublication(const QJsonObject &publication)
{
    // After its own publication the Sync used to run the editable-source installer a
    // second time (about 2 s and one more reading of the server's source tree), which
    // found only what had just been sent. The publisher now says when the server's tree
    // after the publication is the tree the installer checked plus exactly that
    // publication. Then the installer's check holds through the new frontier.
    const QJsonObject verified = publication.value(QStringLiteral("source_tree_verified")).toObject();
    const qint64 from = verified.value(QStringLiteral("from")).toInteger(-1);
    const qint64 through = verified.value(QStringLiteral("through")).toInteger(-1);
    if (from < 0 || through < from || m_syncCoordinator.authoringCheckedSourceFrontier != from) {
        return;
    }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store || !error.isEmpty()) {
        return;
    }
    Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    if (!error.isEmpty()) {
        return;
    }
    // Only a complete installation of this very manifest carries over: a manifest that
    // the publication changed may name folders the installer has not looked at.
    const QJsonObject materializedManifest = frontier.metadata.value(QStringLiteral("materialized_source_manifest")).toObject();
    const QJsonObject manifest = sourceManifestFileIdentity(m_sourceManifestPath, true, &error);
    if (!error.isEmpty() || frontier.metadata.value(QStringLiteral("materialized_source_frontier")).toInteger(-1) != from
        || !manifest.value(QStringLiteral("exists")).toBool() || manifest.value(QStringLiteral("sha256")).toString().isEmpty()
        || manifest.value(QStringLiteral("sha256")) != materializedManifest.value(QStringLiteral("sha256"))) {
        return;
    }
    frontier.metadata.insert(QStringLiteral("materialized_source_manifest"), manifest);
    frontier.metadata.insert(QStringLiteral("materialized_source_frontier"), through);
    if (!store->setFrontier(frontier, &error)) {
        return;
    }
    m_syncCoordinator.authoringCheckedSourceFrontier = through;
    emitSyncTiming(QStringLiteral("source_install_skipped"), QStringLiteral("editable sources verified through %1 by the publication; no second installer pass").arg(through));
}

void FedoraRepository::beginManualReaderStateRecheck()
{
    if (!m_syncCoordinator.running()) {
        return;
    }
    m_syncCoordinator.serverStatePurpose = SyncCoordinator::ServerStatePurpose::ReaderRecheck;
    m_syncCoordinator.phase = ManualSyncPhase::ServerState;
    emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("Confirming the current Library state"));
    scheduleManualSyncAdvance();
}

void FedoraRepository::resumeManualSyncAfterSourceReconciliation()
{
    if (!m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::LocalReconciliation || !m_sourceChangeTracker) {
        return;
    }
    if (m_syncCoordinator.localReconciliationStartedAtMs > 0
        && QDateTime::currentMSecsSinceEpoch()
                - m_syncCoordinator.localReconciliationStartedAtMs
            >= manualLocalReconciliationTimeoutMs) {
        failManualSync(
            QStringLiteral("Local source monitoring did not settle; nothing was uploaded."),
            QStringLiteral("The local source check remained active for 60 seconds. Your changes remain safely stored on this computer."),
            QStringLiteral("transient"));
        return;
    }
    // A file created while Thoughts was closed is first seen by this scan and
    // counted only once it is stable. Continuing before that reported
    // "Sync succeeded" while the new, renamed, or deleted file stayed
    // unpublished.
    if (m_sourceChangeTracker->reconciliationInProgress() || m_sourceChangeTracker->settlingCandidates()) {
        scheduleManualSyncAdvance(100);
        return;
    }
    const QVariantMap diagnostics = m_sourceChangeTracker->diagnostics();
    emitSyncTiming(QStringLiteral("local_check"), QStringLiteral("local source check %1 ms (last scan %2 ms, store %3 ms, %4 hashed)")
                                                      .arg(QDateTime::currentMSecsSinceEpoch() - m_syncCoordinator.localReconciliationStartedAtMs)
                                                      .arg(diagnostics.value(QStringLiteral("scan_ms")).toLongLong())
                                                      .arg(diagnostics.value(QStringLiteral("store_ms")).toLongLong())
                                                      .arg(diagnostics.value(QStringLiteral("hashed_paths")).toLongLong()));
    // Never reuse the response that arrived before reconciliation completed.
    // Fedora may have advanced while the local worker was hashing, and the
    // local snapshot must be frozen before the authoritative state request.
    m_syncCoordinator.serverState = {};
    m_syncCoordinator.localReconciliationStartedAtMs = -1;
    m_syncCoordinator.phase = ManualSyncPhase::ServerState;
    scheduleManualSyncAdvance();
}

bool FedoraRepository::manualReaderStateCanFinish(const QJsonObject &serverState)
{
    if (m_syncCoordinator.conditionalRenderCheck
        && m_syncCoordinator.renderCheckedSourceFrontier < serverState.value(QStringLiteral("source_frontier")).toInteger(-1)) {
        return false;
    }
    const qint64 latest = serverState.value(QStringLiteral("latest_server_sequence")).toInteger(-1);
    return latest >= 0 && m_outbox && m_outbox->cursor() == latest && manualReaderArtifactsCurrent(serverState);
}

qint64 FedoraRepository::manualExpectedSourceFrontier(QString *error)
{
    if (error) {
        error->clear();
    }
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(error);
    if (!store || (error && !error->isEmpty())) {
        return -1;
    }
    const Thoughts::DesktopSyncFrontier durable = store->frontier(error);
    if (error && !error->isEmpty()) {
        return -1;
    }
    return std::max({durable.sourceFrontier, m_syncCoordinator.publishedSourceFrontier, m_syncCoordinator.renderCheckedSourceFrontier});
}

bool FedoraRepository::manualReaderArtifactsRecorded(const QJsonObject &serverState, QList<QJsonObject> *receipts)
{
    const QString revision = serverState.value(QStringLiteral("library_revision")).toString();
    const QJsonObject state = readRepositoryState();
    if (!validSha256(revision) || !m_outbox || !hasProjectionSnapshot() || state.value(QStringLiteral("active_library_revision")).toString() != revision || !state.value(QStringLiteral("pending_library_generation")).toObject().isEmpty() ||
        !state.value(QStringLiteral("pending_pdf_projections")).toArray().isEmpty()) {
        return false;
    }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store || !error.isEmpty()) {
        return false;
    }
    const Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    const QJsonArray objects = frontier.metadata.value(QStringLiteral("reader_cache_objects")).toArray();
    if (!error.isEmpty() || frontier.metadata.value(QStringLiteral("reader_cache_revision")).toString() != revision || !frontier.metadata.value(QStringLiteral("reader_cache_objects")).isArray()) {
        return false;
    }
    if (objects.isEmpty()) {
        return true;
    }
    // Only the receipts of these objects, in a few queries: this runs in the window every 20 s, and
    // reading every receipt (tens of thousands) froze it for seconds.
    QStringList wanted;
    for (const QJsonValue &value : objects) wanted.append(value.toString());
    const QHash<QString, Thoughts::DesktopVerifiedObject> known = store->verifiedObjects(wanted, &error);
    if (!error.isEmpty()) {
        return false;
    }
    for (const QJsonValue &value : objects) {
        const auto object = known.constFind(value.toString());
        if (object == known.cend() || object->sizeBytes < 0) {
            return false;
        }
        receipts->append(object->metadata);
    }
    return true;
}

bool FedoraRepository::readerArtifactFilesUnchanged(const QList<QJsonObject> &receipts)
{
    // One stat per cached object. On a folder a File Provider serves this can take
    // seconds (10.7 s in one freeze report), so the periodic check runs it on a worker.
    return std::all_of(receipts.cbegin(), receipts.cend(), [](const QJsonObject &receipt) {
        return sameReaderFileMetadata(receipt, readerFileMetadata(receipt.value(QStringLiteral("local_path")).toString()));
    });
}

bool FedoraRepository::manualReaderArtifactsCurrent(const QJsonObject &serverState)
{
    QList<QJsonObject> receipts;
    return manualReaderArtifactsRecorded(serverState, &receipts) && readerArtifactFilesUnchanged(receipts);
}

void FedoraRepository::checkRemoteWork(std::function<void(bool pending, const QString &stateKey)> done)
{
    // The watch of the builds a Sync left running stops with its client or at a restart; the
    // periodic check starts it again.
    watchPendingServerBuilds();
    evaluateRemoteWork(true, std::move(done));
}

void FedoraRepository::evaluateRemoteWork(bool whileBuildsPending, std::function<void(bool pending, const QString &stateKey)> done)
{
    if (!m_client || !m_outbox || m_syncCoordinator.running()) {
        done(false, QString());
        return;
    }
    QNetworkReply *reply = m_client->syncState();
    connect(reply, &QNetworkReply::finished, this, [this, reply, whileBuildsPending, done = std::move(done)]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(false, QString());
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject state = document.object();
        const qint64 latest = state.value(QStringLiteral("latest_server_sequence")).toInteger(-1);
        const QString revision = state.value(QStringLiteral("library_revision")).toString();
        if (!document.isObject() || state.value(QStringLiteral("contract_version")).toInt() != 2 || latest < 0 || !validSha256(revision) || !m_outbox
            || m_syncCoordinator.running()) {
            done(false, QString());
            return;
        }
        const QString key = QStringLiteral("%1/%2/%3").arg(latest).arg(revision).arg(state.value(QStringLiteral("source_frontier")).toInteger(-1));
        if (m_outbox->cursor() != latest) {
            done(true, key);
            return;
        }
        // A running build changes the Library revision as it progresses. A Sync started for
        // that would only find the build still running; its end is announced by the watch.
        if (whileBuildsPending && pendingServerBuildsSuppressRevisionChanges()) {
            done(false, key);
            return;
        }
        // What this computer recorded is read here; whether the files still are the
        // recorded ones is looked at on a worker thread, so that this check, which runs
        // every 20 s, never holds the window.
        QList<QJsonObject> receipts;
        if (!manualReaderArtifactsRecorded(state, &receipts)) {
            done(true, key);
            return;
        }
        if (receipts.isEmpty()) {
            done(false, key);
            return;
        }
        if (m_readerArtifactCheckRunning) {
            // The previous look at the files has not returned (a stalled volume): nothing
            // is known, and no second worker waits beside it.
            done(false, QString());
            return;
        }
        m_readerArtifactCheckRunning = true;
        const QPointer<FedoraRepository> repository(this);
        QThreadPool::globalInstance()->start([repository, receipts, key, done] {
            const bool unchanged = readerArtifactFilesUnchanged(receipts);
            const Qt::HANDLE worker = QThread::currentThreadId();
            QMetaObject::invokeMethod(
                repository,
                [repository, unchanged, worker, key, done] {
                    if (!repository) {
                        return;
                    }
                    repository->m_readerArtifactCheckRunning = false;
                    repository->m_readerArtifactCheckThread = worker;
                    if (repository->m_syncCoordinator.running()) {
                        done(false, QString());
                        return;
                    }
                    done(!unchanged, key);
                },
                Qt::QueuedConnection);
        });
    });
}

QJsonArray FedoraRepository::pendingServerBuilds() const
{
    return readRepositoryState().value(QStringLiteral("pending_server_builds")).toArray();
}

QString FedoraRepository::manualSyncFailureKind() const
{
    return m_syncCoordinator.state == ManualSyncState::Failed ? m_syncCoordinator.failureKind : QString();
}

void FedoraRepository::retryingRequest(std::function<QNetworkReply *()> send, std::function<void(QNetworkReply *)> finished, int attempt)
{
    QNetworkReply *reply = send();
    const QString trace = m_activeTraceId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, send, finished, attempt, trace] {
        const bool sameSync = m_syncCoordinator.running() && m_activeTraceId == trace;
        if (!sameSync || attempt >= m_retryDelaysMs.size() || !transientReplyFailure(reply)) {
            noteReplyFailure(reply);
            finished(reply);
            return;
        }
        // No answer, a timeout or 502/503/504: the request changes nothing on the server
        // when it is repeated, so it is asked again after 1, 2 and 4 s before the Sync
        // gives up. What the server refused (4xx) is never asked again.
        const int delayMs = m_retryDelaysMs.at(attempt);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!m_activeTraceId.isEmpty()) {
            Q_EMIT syncJournalEvent(m_activeTraceId,
                                    QJsonObject {{QStringLiteral("at_ms"), QDateTime::currentMSecsSinceEpoch()},
                                                 {QStringLiteral("state"), QStringLiteral("running")},
                                                 {QStringLiteral("message"),
                                                  status > 0 ? QStringLiteral("The server answered %1; asking again in %2 s.").arg(status).arg(delayMs / 1000.0)
                                                             : QStringLiteral("The server did not answer (%1); asking again in %2 s.").arg(reply->errorString()).arg(delayMs / 1000.0)},
                                                 {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("retry_attempt"), attempt + 1}, {QStringLiteral("retry_in_ms"), delayMs}}}});
        }
        const QPointer<QNetworkReply> failed(reply);
        QTimer::singleShot(delayMs, this, [this, failed, send, finished, attempt, trace] {
            if (!failed) {
                return; // The client went away with its replies; nobody waits for this one.
            }
            if (!m_client || !m_syncCoordinator.running() || m_activeTraceId != trace) {
                // The Sync ended meanwhile: its handler still gets the answer it waits for.
                noteReplyFailure(failed);
                finished(failed);
                return;
            }
            failed->deleteLater();
            retryingRequest(send, finished, attempt + 1);
        });
    });
}

QString FedoraRepository::manualSyncWarningCode() const
{
    return m_syncCoordinator.state == ManualSyncState::Succeeded ? m_syncCoordinator.warningCode : QString();
}

QString FedoraRepository::manualSyncWarning() const
{
    return m_syncCoordinator.state == ManualSyncState::Succeeded ? m_syncCoordinator.warning : QString();
}

bool FedoraRepository::pendingServerBuildsSuppressRevisionChanges() const
{
    // Only while the watch keeps hearing from the server that a build still runs: a watch
    // that stopped must not hold back the Sync that fetches the finished PDF.
    constexpr qint64 confirmationLifetimeMs = 90000;
    return m_pendingBuildConfirmedAtMs >= 0 && QDateTime::currentMSecsSinceEpoch() - m_pendingBuildConfirmedAtMs < confirmationLifetimeMs && !pendingServerBuilds().isEmpty();
}

void FedoraRepository::recordPendingServerBuild(const QJsonObject &build, const QStringList &documents, bool wholeLibrary)
{
    QJsonObject state = readRepositoryState();
    const QJsonArray previous = state.value(QStringLiteral("pending_server_builds")).toArray();
    const QString buildId = build.value(QStringLiteral("id")).toString();
    QJsonArray next;
    QJsonObject recorded;
    for (const QJsonValue &value : previous) {
        if (value.toObject().value(QStringLiteral("id")).toString() == buildId) {
            recorded = value.toObject();
        } else if (!wholeLibrary) {
            next.append(value);
        }
    }
    if (safeComponent(buildId)) {
        const QJsonArray names = QJsonArray::fromStringList(documents);
        if (recorded.value(QStringLiteral("documents")).toArray() != names || recorded.value(QStringLiteral("target")) != build.value(QStringLiteral("target"))) {
            recorded = QJsonObject {{QStringLiteral("id"), buildId},
                                    {QStringLiteral("target"), build.value(QStringLiteral("target"))},
                                    {QStringLiteral("documents"), names},
                                    {QStringLiteral("recorded_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        }
        next.append(recorded);
        m_pendingBuildConfirmedAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (next == previous) {
        return;
    }
    if (next.isEmpty()) {
        state.remove(QStringLiteral("pending_server_builds"));
    } else {
        state.insert(QStringLiteral("pending_server_builds"), next);
    }
    writeRepositoryState(state);
}

void FedoraRepository::watchPendingServerBuilds()
{
    if (!m_client || m_pendingBuildReply) {
        return;
    }
    const QJsonArray pending = pendingServerBuilds();
    if (pending.isEmpty()) {
        return;
    }
    const QString buildId = pending.first().toObject().value(QStringLiteral("id")).toString();
    if (!safeComponent(buildId)) {
        finishPendingServerBuild(buildId, {});
        return;
    }
    // One request the server answers when the build ends (or after the wait): the end is
    // noticed at once instead of at the next periodic check, and nothing waits on it.
    QNetworkReply *reply = m_client->build(buildId, m_pendingBuildWaitMs);
    m_pendingBuildReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, buildId] {
        reply->deleteLater();
        if (m_pendingBuildReply == reply) {
            m_pendingBuildReply.clear();
        }
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) {
                // The server no longer knows this build; the periodic check decides the rest.
                finishPendingServerBuild(buildId, {});
                return;
            }
            // Losing the connection changes nothing about the server's build. Ask again,
            // slowly after three attempts.
            static constexpr int backoffMs[] {1000, 2000, 4000};
            const int attempt = ++m_pendingBuildWatchFailures;
            QTimer::singleShot(attempt <= 3 ? backoffMs[attempt - 1] : 30000, this, &FedoraRepository::watchPendingServerBuilds);
            return;
        }
        m_pendingBuildWatchFailures = 0;
        const QJsonObject build = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = Internal::authoritativeBuildStatus(build);
        if (status == QLatin1String("succeeded") || status == QLatin1String("attention") || status == QLatin1String("failed") || status == QLatin1String("cancelled")) {
            finishPendingServerBuild(buildId, build);
            return;
        }
        if (status == QLatin1String("queued") || status == QLatin1String("running")) {
            m_pendingBuildConfirmedAtMs = QDateTime::currentMSecsSinceEpoch();
            // After a restart the saved "building" status is only a memory; the server has
            // just confirmed it.
            for (const QJsonValue &value : pendingServerBuilds()) {
                if (value.toObject().value(QStringLiteral("id")).toString() != buildId) {
                    continue;
                }
                for (const QJsonValue &document : value.toObject().value(QStringLiteral("documents")).toArray()) {
                    const QString filename = document.toString();
                    if (m_restoredTransientRenderStatuses.contains(filename) && m_renderStatuses.value(filename).value(QStringLiteral("status")).toString() == QLatin1String("building")) {
                        m_restoredTransientRenderStatuses.remove(filename);
                        Q_EMIT renderStatusChanged(filename, m_renderStatuses.value(filename));
                    }
                }
            }
        }
        // An answer that names no known status is not asked for again at once.
        QTimer::singleShot(status == QLatin1String("queued") || status == QLatin1String("running") ? 0 : 5000, this, &FedoraRepository::watchPendingServerBuilds);
    });
}

void FedoraRepository::finishPendingServerBuild(const QString &buildId, const QJsonObject &build)
{
    QJsonObject state = readRepositoryState();
    QJsonArray retained;
    QJsonObject finished;
    for (const QJsonValue &value : state.value(QStringLiteral("pending_server_builds")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")).toString() == buildId) {
            finished = value.toObject();
        } else {
            retained.append(value);
        }
    }
    if (retained.isEmpty()) {
        state.remove(QStringLiteral("pending_server_builds"));
        m_pendingBuildConfirmedAtMs = -1;
    } else {
        state.insert(QStringLiteral("pending_server_builds"), retained);
    }
    // Each ended build is forgotten before its Sync is announced, so a Sync that fails does
    // not start another one for it.
    writeRepositoryState(state);

    const QString status = Internal::authoritativeBuildStatus(build);
    if (status == QLatin1String("attention") || status == QLatin1String("failed") || status == QLatin1String("cancelled")) {
        // The Sync that left this build running succeeded; the failure belongs to the PDF.
        const QString buildError = build.value(QStringLiteral("error")).toString().trimmed();
        QHash<QString, QString> failures;
        for (const QJsonValue &value : build.value(QStringLiteral("result")).toObject().value(QStringLiteral("failed_documents")).toArray()) {
            const QJsonObject failure = value.toObject();
            const QString filename = failure.value(QStringLiteral("pdf_filename")).toString(failure.value(QStringLiteral("filename")).toString());
            if (safePdfFilename(filename)) {
                failures.insert(filename, failure.value(QStringLiteral("message")).toString(failure.value(QStringLiteral("error")).toString()).trimmed());
            }
        }
        if (failures.isEmpty()) {
            for (const QJsonValue &value : finished.value(QStringLiteral("documents")).toArray()) {
                if (safePdfFilename(value.toString())) {
                    failures.insert(value.toString(), QString());
                }
            }
            const QString target = build.value(QStringLiteral("target")).toString();
            if (failures.isEmpty() && safePdfFilename(target)) {
                failures.insert(target, QString());
            }
        }
        QHash<QString, QJsonObject> failedStatuses;
        for (auto failure = failures.cbegin(); failure != failures.cend(); ++failure) {
            QJsonObject failed = m_renderStatuses.value(failure.key());
            QString reason = failure.value().isEmpty() ? buildError : failure.value();
            if (reason.isEmpty()) {
                reason = QStringLiteral("The server could not build this PDF; the last PDF that built stays in the Library.");
            }
            failed.insert(QStringLiteral("status"), status == QLatin1String("attention") ? QStringLiteral("attention") : QStringLiteral("error"));
            failed.insert(QStringLiteral("reason"), reason);
            failed.insert(QStringLiteral("build"), build);
            failed.insert(QStringLiteral("checking"), false);
            failedStatuses.insert(failure.key(), failed);
        }
        storeRenderStatuses(failedStatuses);
    }
    // The other PDFs of this build are built: until the announced Sync fetches them they
    // wait for Sync instead of staying "updating" should that Sync not run.
    QHash<QString, QJsonObject> builtStatuses;
    for (const QJsonValue &value : finished.value(QStringLiteral("documents")).toArray()) {
        const QString filename = value.toString();
        QJsonObject built = m_renderStatuses.value(filename);
        if (status.isEmpty() || built.value(QStringLiteral("status")).toString() != QLatin1String("building")
            || built.value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString() != buildId) {
            continue;
        }
        built.insert(QStringLiteral("status"), QStringLiteral("synced"));
        built.insert(QStringLiteral("local_state"), QStringLiteral("sync_pending"));
        built.insert(QStringLiteral("reason"), QStringLiteral("The server finished updating this PDF. Sync brings it to this computer."));
        built.insert(QStringLiteral("build"), build);
        builtStatuses.insert(filename, built);
    }
    storeRenderStatuses(builtStatuses);
    announceRemoteWorkAfterPendingBuild();
    if (!retained.isEmpty()) {
        QTimer::singleShot(0, this, &FedoraRepository::watchPendingServerBuilds);
    }
}

void FedoraRepository::announceRemoteWorkAfterPendingBuild()
{
    if (m_syncCoordinator.running()) {
        // The running Sync may already fetch the new PDF; ask once it is over.
        m_pendingBuildAnnouncementDue = true;
        return;
    }
    m_pendingBuildAnnouncementDue = false;
    evaluateRemoteWork(false, [this](bool pending, const QString &stateKey) {
        if (pending && !stateKey.isEmpty()) {
            Q_EMIT remoteWorkFound(true, stateKey);
        }
    });
}

bool FedoraRepository::canPullPlanlessRemoteChanges(const QJsonObject &serverState)
{
    const qint64 latest = serverState.value(QStringLiteral("latest_server_sequence")).toInteger(-1);
    const qint64 sourceFrontier = serverState.value(QStringLiteral("source_frontier")).toInteger(-1);
    if (!m_outbox || latest < 0 || sourceFrontier < 0 || m_outbox->cursor() < 0 || m_outbox->cursor() >= latest || !manualReaderArtifactsCurrent(serverState)) {
        return false;
    }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    return store && error.isEmpty() && store->frontier(&error).sourceFrontier == sourceFrontier && error.isEmpty();
}

Thoughts::DesktopSyncStore *FedoraRepository::desktopSyncStore(QString *error)
{
    const QString expectedPath = QDir(m_stateRoot).filePath(QStringLiteral("desktop-sync.sqlite"));
    bool recoverInterruptedPublication = false;
    if (!m_desktopSyncStore || m_desktopSyncStorePath != expectedPath) {
        m_sourceChangeTracker.reset();
        m_sourceChangeTrackerRoot.clear();
        m_desktopSyncStore = std::make_unique<Thoughts::DesktopSyncStore>(expectedPath);
        m_desktopSyncStorePath = expectedPath;
        recoverInterruptedPublication = true;
    }
    if (!m_desktopSyncStore->open(error)) {
        return nullptr;
    }
    QFile sourcePublicationState(QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")));
    if (recoverInterruptedPublication && sourcePublicationState.open(QIODevice::ReadOnly)) {
        const QJsonObject lastPublication = QJsonDocument::fromJson(sourcePublicationState.readAll()).object().value(QStringLiteral("last_publication")).toObject();
        if (!lastPublication.isEmpty()) {
            QString recoveryError;
            const int recovered = m_desktopSyncStore->reconcileAppliedSourcePublication(lastPublication, &recoveryError);
            if (recovered < 0) {
                if (error) {
                    *error = recoveryError;
                }
                return nullptr;
            }
            if (recovered > 0) {
                qInfo().noquote() << QStringLiteral("Recovered %1 exact source publication result(s) after response loss.").arg(recovered);
                if (m_sourceChangeTracker) {
                    const QList<Thoughts::DesktopTrackedPath> recoveredPaths = m_desktopSyncStore->trackingPaths(&recoveryError);
                    if (!recoveryError.isEmpty() || !m_sourceChangeTracker->setTrackedPaths(recoveredPaths, &recoveryError)) {
                        if (error) {
                            *error = recoveryError;
                        }
                        return nullptr;
                    }
                }
            }
        }
    }
    if (recoverInterruptedPublication) {
        QString monitoringError;
        const QList<Thoughts::DesktopTrackedPath> storedBaselines =
            m_desktopSyncStore->baselines(&monitoringError);
        if (!monitoringError.isEmpty()) {
            if (error) {
                *error = monitoringError;
            }
            return nullptr;
        }
        if (!storedBaselines.isEmpty()) {
            const QList<Thoughts::DesktopTrackedPath> scopedBaselines =
                desktopMonitoringBaselines(m_docsRoot, storedBaselines);
            const auto semanticIdentity = [](const Thoughts::DesktopTrackedPath &path) {
                return std::make_tuple(path.path,
                                       desktopSyncSemanticMetadata(path.metadata),
                                       normalizedDesktopSyncTargets(path.affectedTargets));
            };
            bool monitoringChanged = storedBaselines.size() != scopedBaselines.size();
            for (qsizetype index = 0;
                 !monitoringChanged && index < storedBaselines.size();
                 ++index) {
                monitoringChanged = semanticIdentity(storedBaselines.at(index))
                    != semanticIdentity(scopedBaselines.at(index));
            }
            if (monitoringChanged
                && !m_desktopSyncStore->replaceBaselines(scopedBaselines,
                                                         &monitoringError)) {
                if (error) {
                    *error = monitoringError;
                }
                return nullptr;
            }
        }
    }
    const QString legacyStatePath = QDir(m_stateRoot).filePath(QStringLiteral("repository.json"));
    const QJsonArray legacyLibraryOperations = readRepositoryState().value(QStringLiteral("pending_library_mutations")).toArray();
    const Thoughts::DesktopLegacyLibraryImportResult legacyLibraryImport = m_desktopSyncStore->importLegacyLibraryOperations(legacyStatePath, legacyLibraryOperations);
    if (!legacyLibraryImport.success) {
        if (error) {
            *error = legacyLibraryImport.error;
        }
        return nullptr;
    }
    if (!m_sourceChangeTracker || m_sourceChangeTrackerRoot != m_docsRoot) {
        m_sourceChangeTracker = std::make_unique<Thoughts::SourceChangeTracker>(m_docsRoot, m_desktopSyncStore.get(), this);
        m_sourceChangeTrackerRoot = m_docsRoot;
        connect(m_sourceChangeTracker.get(), &Thoughts::SourceChangeTracker::reconciliationScheduled, this, &FedoraRepository::scheduleSourceCommentRefresh);
        connect(m_sourceChangeTracker.get(), &Thoughts::SourceChangeTracker::reconciliationFinished, this, [this] {
            m_sourceTrackingSettled = m_sourceChangeTracker->authoritative();
            // Settlement is per project: another project may have settled.
            scheduleSourceCommentRefresh();
            emitEffectiveProjections();
            schedulePendingChangesRefresh();
            resumeManualSyncAfterSourceReconciliation();
        });
        connect(m_sourceChangeTracker.get(), &Thoughts::SourceChangeTracker::reconciliationFailed, this, [this](const QString &reconciliationError) {
            m_sourceTrackingSettled = false;
            schedulePendingChangesRefresh();
            if (m_syncCoordinator.running() && m_syncCoordinator.phase == ManualSyncPhase::LocalReconciliation) {
                failManualSync(QStringLiteral("The local source synchronization state could not be checked safely."), reconciliationError);
            }
        });
        m_sourceTrackingSettled = false;
        connect(m_sourceChangeTracker.get(), &Thoughts::SourceChangeTracker::authoritativeChanged, this, [this](bool authoritative) {
            if (authoritative) {
                m_sourceTrackingSettled = true;
                // Source locations are complete only from an authoritative scan:
                // rebuild them now rather than at the next source edit.
                scheduleSourceCommentRefresh();
            }
            schedulePendingChangesRefresh();
        });
        QString baselineError;
        QElapsedTimer trackingLoad;
        trackingLoad.start();
        const QList<Thoughts::DesktopTrackedPath> trackingPaths = m_desktopSyncStore->trackingPaths(&baselineError);
        m_sourceTrackingLoadMs = trackingLoad.elapsed();
        if (baselineError.isEmpty() && !trackingPaths.isEmpty()
            && m_sourceChangeTracker->setTrackedPaths(trackingPaths, &baselineError)) {
            QString pruneError;
            if (m_sourceChangeTracker->pruneIgnoredTombstones(&pruneError) < 0) {
                qWarning() << "Ignored source markers could not be pruned:" << pruneError;
            }
        }
        if (!baselineError.isEmpty() && error) {
            *error = baselineError;
            return nullptr;
        }
    }
    return m_desktopSyncStore.get();
}

bool FedoraRepository::sourceManifestMatchesAccepted(Thoughts::DesktopSyncStore *store, bool *matches, QString *error)
{
    if (matches) {
        *matches = false;
    }
    if (!store) {
        if (error) {
            *error = QStringLiteral("The desktop synchronization store is unavailable.");
        }
        return false;
    }
    if (!sourceInventoryCoversLibrary(m_docsRoot, m_sourceManifestPath, m_syncCoordinator.isDocument() ? m_syncCoordinator.target : QString())) {
        return true;
    }
    Thoughts::DesktopSyncFrontier frontier = store->frontier(error);
    if (error && !error->isEmpty()) {
        return false;
    }
    const QJsonObject accepted = frontier.metadata.value(QStringLiteral("source_manifest")).toObject();
    const QString acceptedSha256 = accepted.value(QStringLiteral("sha256")).toString();
    if (!accepted.value(QStringLiteral("exists")).toBool() || !validSha256(acceptedSha256)) {
        return true;
    }
    const QString manifestPath = m_sourceManifestPath;
    const QJsonObject metadata = sourceManifestFileIdentity(manifestPath, false, error);
    if (error && !error->isEmpty()) {
        return false;
    }
    if (!metadata.value(QStringLiteral("exists")).toBool()) {
        return true;
    }
    const bool acceptedProjectManifest = accepted.value(QStringLiteral("project_sources_v1")).toBool();
    if (sameSourceManifestFileMetadata(accepted, metadata) && acceptedProjectManifest) {
        if (matches) {
            *matches = true;
        }
        return true;
    }

    QJsonObject verified = sourceManifestFileIdentity(manifestPath, true, error);
    if (error && !error->isEmpty()) {
        return false;
    }
    if (verified.value(QStringLiteral("sha256")).toString() != acceptedSha256) {
        return true;
    }

    const bool projectManifest = sourceManifestPathIsProjectScoped(manifestPath, error);
    if (error && !error->isEmpty()) {
        return false;
    }
    if (!projectManifest) {
        // Regenerate inventories from an earlier layout before accepting
        // coverage, even when the previous file identity still matches.
        return true;
    }

    // A metadata-only touch or atomic replacement with identical bytes is
    // still the accepted manifest. Persist the new cheap identity so the
    // next unchanged Sync performs no content read.
    verified.insert(QStringLiteral("project_sources_v1"), true);
    frontier.metadata.insert(QStringLiteral("source_manifest"), verified);
    if (!store->setFrontier(frontier, error)) {
        return false;
    }
    if (matches) {
        *matches = true;
    }
    return true;
}

bool FedoraRepository::recordAcceptedSourceManifest(Thoughts::DesktopSyncStore *store, bool requirePreviousIdentity, QString *error)
{
    if (!store) {
        if (error) {
            *error = QStringLiteral("The desktop synchronization store is unavailable.");
        }
        return false;
    }
    Thoughts::DesktopSyncFrontier frontier = store->frontier(error);
    if (error && !error->isEmpty()) {
        return false;
    }
    const QJsonObject previous = frontier.metadata.value(QStringLiteral("source_manifest")).toObject();
    const QString manifestPath = m_sourceManifestPath;
    QJsonObject accepted = sourceManifestFileIdentity(manifestPath, true, error);
    if ((error && !error->isEmpty()) || !accepted.value(QStringLiteral("exists")).toBool() || !validSha256(accepted.value(QStringLiteral("sha256")).toString())) {
        return false;
    }
    if (sourceManifestPathIsProjectScoped(manifestPath, error)) {
        accepted.insert(QStringLiteral("project_sources_v1"), true);
    } else if (error && !error->isEmpty()) {
        return false;
    }
    if (requirePreviousIdentity && previous.value(QStringLiteral("sha256")).toString() != accepted.value(QStringLiteral("sha256")).toString()) {
        // A scoped run did not cover an independently changed global
        // manifest. Keep the previous identity so the next Sync must take the
        // safe global path.
        return true;
    }
    frontier.metadata.insert(QStringLiteral("source_manifest"), accepted);
    return store->setFrontier(frontier, error);
}

bool FedoraRepository::acceptDesktopSourcePublication(const QJsonObject &publication, const QString &scopeTarget, QString *error)
{
    return acceptDesktopSourcePublicationForTargets(publication, scopeTarget.isEmpty() ? QStringList() : QStringList {scopeTarget}, error);
}

bool FedoraRepository::acceptDesktopSourcePublicationForTargets(const QJsonObject &publication, const QStringList &scopeTargets, QString *error)
{
    const bool scoped = !scopeTargets.isEmpty();
    const QString manifestPath = m_sourceManifestPath;
    const QString sourceStatePath = QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json"));
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(error);
    if (!store) {
        return false;
    }
    const QList<Thoughts::DesktopTrackedPath> previousBaselines = store->baselines(error);
    if (error && !error->isEmpty()) {
        return false;
    }
    // A new Library: no Project has a source yet and this device never
    // recorded a baseline, so there is nothing to record and nothing to lose.
    // Only then is an empty source manifest a success.
    if (!scoped && previousBaselines.isEmpty() && (!m_syncCoordinator.sourceSnapshot || m_syncCoordinator.sourceSnapshot->files.isEmpty())
        && sourceManifestHasNoCoverage(manifestPath, publication)) {
        if (error) {
            error->clear();
        }
        return true;
    }
    QList<Thoughts::DesktopTrackedPath> baselines = acceptedDesktopSyncBaselines(m_docsRoot, manifestPath, sourceStatePath, publication, scopeTargets, previousBaselines, error);
    if (baselines.isEmpty()) {
        store->markReconciliationRequired(nullptr);
        return false;
    }

    // Retain local stat/file identity only when it belongs to bytes whose hash
    // the publisher explicitly accepted. This makes the next reconciliation
    // cheap without ever treating a concurrent edit as published.
    QHash<QString, Thoughts::DesktopTrackedPath> previousByPath;
    for (const Thoughts::DesktopTrackedPath &baseline : previousBaselines) {
        previousByPath.insert(baseline.path, baseline);
    }
    QHash<QString, int> acceptedIndex;
    for (int index = 0; index < baselines.size(); ++index) {
        acceptedIndex.insert(baselines.at(index).path, index);
        const Thoughts::DesktopTrackedPath previous = previousByPath.value(baselines.at(index).path);
        const bool directory = baselines.at(index).metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
        if (!previous.path.isEmpty() && previous.exists == baselines.at(index).exists && sameDesktopSyncDescriptor(previous, baselines.at(index)) && (directory || previous.sha256 == baselines.at(index).sha256)) {
            Thoughts::DesktopTrackedPath enriched = previous;
            enriched.metadata = previous.metadata;
            enriched.affectedTargets = baselines.at(index).affectedTargets;
            baselines[index] = enriched;
        }
    }

    const auto restorePreviousBaseline = [&baselines, &acceptedIndex, &previousByPath](const QString &path) {
        const auto accepted = acceptedIndex.constFind(path);
        const Thoughts::DesktopTrackedPath previous = previousByPath.value(path);
        if (accepted != acceptedIndex.cend()) {
            if (previous.path.isEmpty()) {
                baselines[accepted.value()].path.clear();
            } else {
                baselines[accepted.value()] = previous;
            }
        } else if (!previous.path.isEmpty()) {
            acceptedIndex.insert(path, baselines.size());
            baselines.append(previous);
        }
    };
    const auto scopeWasPublished = [this, scoped](const Thoughts::DesktopPendingFile &file) {
        if (!scoped) {
            return true;
        }
        if (file.affectedTargets.isEmpty()) {
            return false;
        }
        return std::all_of(file.affectedTargets.cbegin(), file.affectedTargets.cend(), [this](const QString &target) { return m_syncCoordinator.completedSourcePublicationTargets.contains(target); });
    };

    // Build inputs any closure accepted by this publication still names.
    QSet<QString> acceptedBuildInputPaths;
    const QJsonObject acceptedBuildInputs = publication.value(QStringLiteral("build_input_entries")).toObject();
    for (auto target = acceptedBuildInputs.begin(); target != acceptedBuildInputs.end(); ++target) {
        for (const QJsonValue &entry : target.value().toObject().value(QStringLiteral("entries")).toArray()) {
            acceptedBuildInputPaths.insert(QDir::cleanPath(entry.toObject().value(QStringLiteral("path")).toString()));
        }
    }
    const QHash<QString, QString> manifestProjectsByTarget = sourceManifestProjectsByTarget(manifestPath);
    // A deleted build input is published once every PDF it may belong to
    // (its pending targets and its last accepted ones that still exist) has
    // published a fresh closure in this Sync.
    const auto deletedBuildInputPublished = [&](const Thoughts::DesktopPendingFile &file, const Thoughts::DesktopTrackedPath &previous) {
        const QString kind = file.trackedState.metadata.value(QStringLiteral("source_kind")).toString();
        if (file.action != QLatin1String("delete") || file.trackedState.exists || kind != QLatin1String("build_input") || acceptedBuildInputPaths.contains(file.path) || !scopeWasPublished(file)) {
            return false;
        }
        if (!previous.path.isEmpty()
            && (previous.metadata.value(QStringLiteral("source_kind")).toString() != QLatin1String("build_input")
                || desktopSyncSemanticMetadata(previous.metadata) != desktopSyncSemanticMetadata(file.trackedState.metadata))) {
            return false;
        }
        return std::all_of(previous.affectedTargets.cbegin(), previous.affectedTargets.cend(), [this, &manifestProjectsByTarget](const QString &target) {
            return m_syncCoordinator.completedSourcePublicationTargets.contains(target) || !manifestProjectsByTarget.contains(target);
        });
    };

    Thoughts::DesktopSyncSnapshot confirmedSnapshot;
    if (m_syncCoordinator.sourceSnapshot) {
        Thoughts::DesktopSyncSnapshot publicationSnapshot = *m_syncCoordinator.sourceSnapshot;
        if (!scoped && !m_syncCoordinator.sourcePublicationSelectedPaths.isEmpty()) {
            Internal::SourcePublicationDecision selection;
            selection.mode = Internal::SourcePublicationMode::Global;
            selection.selectedPaths = m_syncCoordinator.sourcePublicationSelectedPaths;
            publicationSnapshot = Internal::selectedSourcePublicationSnapshot(publicationSnapshot, selection);
        }
        confirmedSnapshot.throughGeneration = publicationSnapshot.throughGeneration;
        for (const Thoughts::DesktopPendingFile &file : std::as_const(publicationSnapshot.files)) {
            // Fedora keeps one copy of a source for all its PDFs, so once it
            // accepted these exact bytes they are the base for every target,
            // even if publishing another target of this Sync fails. Build
            // inputs are accepted per target and wait for all of them.
            const auto acceptedSource = acceptedIndex.constFind(file.path);
            const bool sourceAcceptedByFedora = file.hasTrackedState && scoped && file.action == QLatin1String("upsert") && file.trackedState.exists
                && file.trackedState.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("source")
                && acceptedSource != acceptedIndex.cend() && validSha256(file.trackedState.sha256)
                && baselines.at(acceptedSource.value()).sha256 == file.trackedState.sha256;
            if (!file.hasTrackedState || (!scopeWasPublished(file) && !sourceAcceptedByFedora)) {
                restorePreviousBaseline(file.path);
                continue;
            }
            auto accepted = acceptedIndex.constFind(file.path);
            const QString pendingKind = file.trackedState.metadata.value(QStringLiteral("source_kind")).toString();
            const bool authoritativeOmission = !scoped || pendingKind == QLatin1String("build_input") || file.trackedState.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved");
            // Discovery can record an exclusion as a deletion before this path
            // ever had a published baseline. A successful global inventory plus
            // an explicit current ignore rule resolves that unknown path; it
            // must not leave its frozen claim pending forever.
            bool ignoredDiscoveryDeletion = false;
            if (!scoped && file.action == QLatin1String("delete") && !file.trackedState.exists
                && accepted == acceptedIndex.cend() && !previousByPath.contains(file.path)
                && file.trackedState.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved")) {
                const QString project = Thoughts::SourceOwner::of(file.path);
                const qsizetype separator = project.size();
                if (!project.isEmpty() && file.path.size() > separator) {
                    const Thoughts::ThoughtIgnore ignored(QDir(m_docsRoot).filePath(project));
                    if (!ignored.error().isEmpty()) {
                        if (error) *error = project + QStringLiteral(": ") + ignored.error();
                        return false;
                    }
                    ignoredDiscoveryDeletion = ignored.excludes(file.path.mid(separator + 1),
                        file.trackedState.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory"));
                }
            }
            if (authoritativeOmission && accepted == acceptedIndex.cend()
                && ((file.action == QLatin1String("upsert") && file.trackedState.exists) || ignoredDiscoveryDeletion)) {
                Thoughts::DesktopTrackedPath classified = file.trackedState;
                classified.metadata.remove(QStringLiteral("_tracking_resolution"));
                classified.metadata.remove(QStringLiteral("_affected_target_hints"));
                classified.metadata.remove(QStringLiteral("role"));
                classified.metadata.remove(QStringLiteral("media_type"));
                classified.metadata.insert(QStringLiteral("source_kind"), QStringLiteral("ignored"));
                acceptedIndex.insert(file.path, baselines.size());
                baselines.append(classified);
                confirmedSnapshot.files.append(file);
                continue;
            }
            if (file.action == QLatin1String("upsert") && file.trackedState.exists && accepted != acceptedIndex.cend()) {
                const int index = accepted.value();
                const Thoughts::DesktopTrackedPath &serverAccepted = baselines.at(index);
                const bool directory = serverAccepted.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
                const bool identityMatches = directory ||
                    (validSha256(file.trackedState.sha256) && file.trackedState.sha256 == serverAccepted.sha256 &&
                     (file.trackedState.sizeBytes < 0 || serverAccepted.sizeBytes < 0 || file.trackedState.sizeBytes == serverAccepted.sizeBytes));
                const bool projectResolvedPending = scoped && file.trackedState.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved") &&
                    std::all_of(file.affectedTargets.cbegin(), file.affectedTargets.cend(), [&serverAccepted](const QString &target) { return serverAccepted.affectedTargets.contains(target); }) &&
                    (file.trackedState.metadata.value(QStringLiteral("project")).toString().isEmpty() || file.trackedState.metadata.value(QStringLiteral("project")) == serverAccepted.metadata.value(QStringLiteral("project")));
                // A new or changed build input that one publisher run per PDF
                // published: each run accepts only its own PDF, so the last one
                // names fewer PDFs than the pending path. Once every PDF it
                // names has published these exact bytes in this Sync, the
                // pending path is published. A newly discovered path may still
                // be unresolved; Fedora's acceptance resolves it.
                const bool pendingUnresolved = file.trackedState.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved");
                const bool sameBuildInput = serverAccepted.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("build_input")
                    && (pendingKind == QLatin1String("build_input")
                            ? desktopSyncSemanticMetadata(file.trackedState.metadata) == desktopSyncSemanticMetadata(serverAccepted.metadata)
                            : (pendingUnresolved && pendingKind.isEmpty()
                               && (file.trackedState.metadata.value(QStringLiteral("project")).toString().isEmpty()
                                   || file.trackedState.metadata.value(QStringLiteral("project")) == serverAccepted.metadata.value(QStringLiteral("project")))));
                const bool buildInputPublishedForAllTargets = scoped && sameBuildInput
                    && std::all_of(serverAccepted.affectedTargets.cbegin(), serverAccepted.affectedTargets.cend(),
                                   [this](const QString &target) { return m_syncCoordinator.completedSourcePublicationTargets.contains(target); })
                    && scopeWasPublished(file);
                if (identityMatches && (sameDesktopSyncDescriptor(file.trackedState, serverAccepted) || !scoped || projectResolvedPending || buildInputPublishedForAllTargets)) {
                    if (sameDesktopSyncDescriptor(file.trackedState, serverAccepted)) {
                        baselines[index] = file.trackedState;
                    } else if (buildInputPublishedForAllTargets) {
                        Thoughts::DesktopTrackedPath published = serverAccepted;
                        published.exists = file.trackedState.exists;
                        published.sizeBytes = file.trackedState.sizeBytes;
                        published.modifiedAtMs = file.trackedState.modifiedAtMs;
                        for (const QString &key : {QStringLiteral("_local_ctime_ms"), QStringLiteral("_local_file_identity")}) {
                            if (file.trackedState.metadata.contains(key)) {
                                published.metadata.insert(key, file.trackedState.metadata.value(key));
                            }
                        }
                        published.affectedTargets = normalizedDesktopSyncTargets(serverAccepted.affectedTargets + file.affectedTargets);
                        baselines[index] = published;
                    } else {
                        // Global or project discovery resolves an Unknown path
                        // to its authoritative role/targets. Keep those
                        // semantic fields while retaining the exact local stat
                        // identity of the frozen bytes so startup stays cheap.
                        Thoughts::DesktopTrackedPath resolved = serverAccepted;
                        resolved.exists = file.trackedState.exists;
                        resolved.sizeBytes = file.trackedState.sizeBytes;
                        resolved.modifiedAtMs = file.trackedState.modifiedAtMs;
                        for (const QString &key : {QStringLiteral("_local_ctime_ms"), QStringLiteral("_local_file_identity")}) {
                            if (file.trackedState.metadata.contains(key)) {
                                resolved.metadata.insert(key, file.trackedState.metadata.value(key));
                            }
                        }
                        baselines[index] = resolved;
                    }
                    confirmedSnapshot.files.append(file);
                } else {
                    restorePreviousBaseline(file.path);
                }
                continue;
            }

            // Complete coverage for all of this path's affected targets
            // confirms that a previously accepted source or build input is
            // now absent. Keep an exact tombstone so a concurrent re-creation
            // is detected as a new generation.
            const Thoughts::DesktopTrackedPath previous = previousByPath.value(file.path);
            const QString previousKind = previous.metadata.value(QStringLiteral("source_kind")).toString();
            if (file.action == QLatin1String("delete") && !file.trackedState.exists && accepted == acceptedIndex.cend() &&
                ((!previous.path.isEmpty() && (previousKind == QLatin1String("build_input") || previousKind == QLatin1String("source")) && sameDesktopSyncDescriptor(file.trackedState, previous)) ||
                 (!scoped && previous.path.isEmpty() && (pendingKind == QLatin1String("build_input") || pendingKind == QLatin1String("source"))))) {
                if (!previous.path.isEmpty()) {
                    acceptedIndex.insert(file.path, baselines.size());
                    baselines.append(file.trackedState);
                }
                confirmedSnapshot.files.append(file);
            } else if (scoped && deletedBuildInputPublished(file, previous)) {
                // Every PDF that could read this deleted build input has now
                // published a closure without it (a missing file cannot be in a
                // fresh inventory), and none of the closures accepted here names
                // it. What the scoped baselines still retain for it is the old
                // bookkeeping of those same PDFs. Before 2026-10-14 such a
                // deletion was never acknowledged: its pending row lists every
                // PDF of the Project, its baseline only the PDFs that had
                // published it, so every later Sync republished the Project.
                Thoughts::DesktopTrackedPath tombstone = file.trackedState;
                tombstone.affectedTargets = normalizedDesktopSyncTargets(tombstone.affectedTargets + previous.affectedTargets);
                if (accepted != acceptedIndex.cend()) {
                    baselines[accepted.value()] = tombstone;
                } else if (!previous.path.isEmpty()) {
                    acceptedIndex.insert(file.path, baselines.size());
                    baselines.append(tombstone);
                }
                confirmedSnapshot.files.append(file);
            } else {
                restorePreviousBaseline(file.path);
            }
        }
    }

    baselines.erase(std::remove_if(baselines.begin(), baselines.end(), [](const Thoughts::DesktopTrackedPath &baseline) { return baseline.path.isEmpty(); }), baselines.end());
    std::sort(baselines.begin(), baselines.end(), [](const Thoughts::DesktopTrackedPath &left, const Thoughts::DesktopTrackedPath &right) { return left.path < right.path; });
    if (!store->acceptBaselinesAndAcknowledge(baselines, confirmedSnapshot, error, publication.value(QStringLiteral("publication_id")).toString())) {
        store->markReconciliationRequired(nullptr);
        return false;
    }

    const QList<Thoughts::DesktopTrackedPath> acceptedBaselines = store->trackingPaths(error);
    if ((error && !error->isEmpty()) || !m_sourceChangeTracker || !m_sourceChangeTracker->setTrackedPaths(acceptedBaselines, error)) {
        store->markReconciliationRequired(nullptr);
        return false;
    }
    // Paths newly excluded by .ThoughtIgnore are now accepted as deleted;
    // forget them so later scans do not keep revisiting them.
    QString pruneError;
    if (m_sourceChangeTracker->pruneIgnoredTombstones(&pruneError) < 0) {
        qWarning() << "Ignored source markers could not be pruned:" << pruneError;
    }
    if (!recordAcceptedSourceManifest(store, scoped, error)) {
        store->markReconciliationRequired(nullptr);
        return false;
    }
    return true;
}

void FedoraRepository::persistManualSyncFrontier()
{
    if (m_syncCoordinator.serverState.isEmpty()) {
        return;
    }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store) {
        return;
    }
    Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    if (!error.isEmpty()) {
        return;
    }
    const QJsonObject state = readRepositoryState();
    const QString activeRevision = state.value(QStringLiteral("active_library_revision")).toString();
    frontier.libraryRevision = validSha256(activeRevision) ? activeRevision : m_syncCoordinator.serverState.value(QStringLiteral("library_revision")).toString();
    frontier.sourceFrontier =
        std::max({frontier.sourceFrontier, m_syncCoordinator.publishedSourceFrontier, m_syncCoordinator.renderCheckedSourceFrontier, m_syncCoordinator.serverState.value(QStringLiteral("source_frontier")).toInteger(-1)});
    frontier.cursor = m_outbox ? m_outbox->cursor() : -1;
    store->setFrontier(frontier, &error);
}

// Claims for files Sync does not manage (tex/generated/, outside every
// Project) or for files gone with no deletion to publish wait forever: no
// publication carries them. The publisher drops them, but only runs when a
// source changed, so each Sync checks them once here. Without stored claims
// this is one database read.
void FedoraRepository::dropInapplicableProposalClaims()
{
    // A running publisher checks them itself, against the state it is writing.
    if (m_syncCoordinator.proposalClaimsChecked || m_publishingSources) {
        return;
    }
    m_syncCoordinator.proposalClaimsChecked = true;
    Thoughts::Publisher::dropInapplicableProposalClaims(m_docsRoot, m_sourceManifestPath, QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")),
                                                        proposalDecisionDatabase(), [this](const QString &message) {
                                                            qInfo().noquote() << message;
                                                            emitSyncProgress(QStringLiteral("checking_local_changes"), 0, 0, message);
                                                        });
}

void FedoraRepository::reportSourcePublicationTotals()
{
    SyncCoordinator &coordinator = m_syncCoordinator;
    if (!coordinator.sourcePublicationClock.isValid()) {
        return;
    }
    emitSyncTiming(QStringLiteral("source_publication"), QStringLiteral("source publication: %1 document(s) in %2 publisher run(s), %3 ms")
                                                             .arg(coordinator.sourcePublicationDocuments)
                                                             .arg(coordinator.sourcePublicationRuns)
                                                             .arg(coordinator.sourcePublicationClock.elapsed()));
    coordinator.sourcePublicationClock.invalidate();
}

QStringList FedoraRepository::nextSourcePublicationBatch() const
{
    if (m_syncCoordinator.publishesAllSources() || m_syncCoordinator.sourcePublicationTargets.isEmpty()) {
        return {};
    }
    const QStringList &targets = m_syncCoordinator.sourcePublicationTargets;
    QStringList batch {targets.constFirst()};
    if (qEnvironmentVariable("THOUGHTS_SOURCE_PUBLICATION_BATCH") == QLatin1String("0")) {
        return batch;
    }
    // Fedora publishes a Project's sources once for all its PDFs, so the
    // PDFs of one Project share one publisher run and one frontier check.
    // A project-scoped publication names the Project of each PDF; a scoped
    // one (build inputs only) takes it from the accepted source manifest.
    const QHash<QString, QString> &decided = m_syncCoordinator.sourcePublicationProjectsByTarget;
    const QHash<QString, QString> manifest = decided.isEmpty() ? sourceManifestProjectsByTarget(m_sourceManifestPath) : QHash<QString, QString>();
    const auto projectOf = [&](const QString &target) {
        return decided.isEmpty() ? manifest.value(target) : decided.value(target);
    };
    const QString project = projectOf(targets.constFirst());
    if (!safeSourceProject(project)) {
        return batch;
    }
    for (qsizetype index = 1; index < targets.size(); ++index) {
        if (projectOf(targets.at(index)) == project && safePdfFilename(targets.at(index)) && !batch.contains(targets.at(index))) {
            batch.append(targets.at(index));
        }
    }
    return batch;
}

void FedoraRepository::startManualSourcePublication()
{
    if (m_publishingSources || !m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::SourcePublication || !m_syncCoordinator.hasSourcePublication()) {
        return;
    }
    // A frozen bundle publishes exactly the PDFs it was staged for.
    const QStringList batch = m_syncCoordinator.frozenPublication ? m_syncCoordinator.sourcePublicationBatch : nextSourcePublicationBatch();
    const QString scopeTarget = batch.isEmpty() ? QString() : batch.constFirst();
    const QString scopeProject = scopeTarget.isEmpty() ? QString() : m_syncCoordinator.sourcePublicationProjectsByTarget.value(scopeTarget);
    if (std::any_of(batch.cbegin(), batch.cend(), [](const QString &target) { return !safePdfFilename(target); })) {
        failManualSync(QStringLiteral("The scoped source publication target is invalid."));
        return;
    }
    const QString batchLabel = batch.size() > 1 ? QStringLiteral("%1 documents").arg(batch.size()) : scopeTarget;
    const QByteArray token = sourcePublisherToken();
    if (token.isEmpty() && !m_sourcePublisherTokenWithheld.isEmpty()) {
        failManualSync(QStringLiteral("The source-publisher token belongs to another server."), m_sourcePublisherTokenWithheld, QStringLiteral("needs_user"));
        return;
    }
    const QString executable = sourcePublisherExecutable();
    const QString manifest = m_sourceManifestPath;
    if (token.isEmpty() || !QFileInfo::exists(executable) || (!scopeTarget.isEmpty() && !QFileInfo::exists(manifest))) {
        failManualSync(QStringLiteral("The source publisher installed with Thoughts is missing; reinstall Thoughts."), QString(), QStringLiteral("needs_user"));
        return;
    }

    if (!scopeTarget.isEmpty() && !m_syncCoordinator.sourceSnapshot) {
        failManualSync(QStringLiteral("The source publication has no frozen pending generation."));
        return;
    }
    if (!m_syncCoordinator.frozenPublication) {
        Thoughts::DesktopSyncSnapshot snapshot = m_syncCoordinator.sourceSnapshot ? *m_syncCoordinator.sourceSnapshot : Thoughts::DesktopSyncSnapshot();
        if (scopeTarget.isEmpty() && !m_syncCoordinator.sourcePublicationSelectedPaths.isEmpty()) {
            Internal::SourcePublicationDecision selection;
            selection.mode = Internal::SourcePublicationMode::Global;
            selection.selectedPaths = m_syncCoordinator.sourcePublicationSelectedPaths;
            snapshot = Internal::selectedSourcePublicationSnapshot(snapshot, selection);
        }
        QString stagingError;
        Thoughts::DesktopSyncStore *const store = desktopSyncStore(&stagingError);
        const QList<Thoughts::DesktopTrackedPath> baselines = store && stagingError.isEmpty() ? store->baselines(&stagingError) : QList<Thoughts::DesktopTrackedPath>();
        if (!store || !stagingError.isEmpty()) {
            failManualSync(QStringLiteral("The immutable source publication could not be prepared safely."), stagingError);
            return;
        }
        const QPointer<FedoraRepository> repository(this);
        const QString docsRoot = m_docsRoot;
        const QString stateRoot = m_stateRoot;
        const QString manifestPath = m_sourceManifestPath;
        m_publishingSources = true;
        m_syncCoordinator.sourcePublicationBatch = batch;
        m_publicationTimer.start();
        emitSyncProgress(QStringLiteral("preparing"),
                         0,
                         0,
                         scopeTarget.isEmpty()
                             ? QStringLiteral("Discovering and freezing the complete source dependency closure")
                             : (scopeProject.isEmpty() ? QStringLiteral("Inspecting and freezing changes for %1").arg(batchLabel) : QStringLiteral("Inspecting %1 and freezing changes for %2").arg(scopeProject, batchLabel)));
        QThreadPool::globalInstance()->start([repository, executable, docsRoot, stateRoot, manifestPath, batch, scopeTarget, scopeProject, snapshot, baselines] {
            Thoughts::FrozenPublicationBundle bundle;
            if (scopeTarget.isEmpty()) {
                bundle = discoverGlobalFrozenPublication(executable, docsRoot, stateRoot, snapshot, baselines);
            } else if (!scopeProject.isEmpty()) {
                bundle = discoverProjectTargetsFrozenPublicationImpl(executable, docsRoot, stateRoot, manifestPath, scopeProject, batch, snapshot, baselines);
            } else {
                QString discoveryError;
                const QList<QPair<QString, QJsonObject>> inventories = discoverScopedBuildInputInventories(executable, docsRoot, manifestPath, batch, &discoveryError);
                if (inventories.isEmpty()) {
                    bundle.error = discoveryError;
                } else {
                    bundle = Thoughts::FrozenPublicationStager::stageScopedWithFreshInventories(docsRoot, stateRoot, inventories, snapshot, baselines);
                }
            }
            QMetaObject::invokeMethod(
                repository,
                [repository, batch, scopeTarget, scopeProject, bundle = std::move(bundle)]() mutable {
                    if (!repository) {
                        Thoughts::FrozenPublicationStager::discard(bundle, nullptr);
                        return;
                    }
                    repository->m_publishingSources = false;
                    SyncCoordinator &coordinator = repository->m_syncCoordinator;
                    const QString currentProject = scopeTarget.isEmpty() ? QString() : coordinator.sourcePublicationProjectsByTarget.value(scopeTarget);
                    const bool sameScope = batch.isEmpty() ? coordinator.publishesAllSources()
                                                           : (!coordinator.publishesAllSources() && coordinator.sourcePublicationBatch == batch
                                                              && std::all_of(batch.cbegin(), batch.cend(), [&coordinator](const QString &target) { return coordinator.sourcePublicationTargets.contains(target); }));
                    if (!coordinator.running() || coordinator.phase != ManualSyncPhase::SourcePublication || !sameScope || currentProject != scopeProject) {
                        Thoughts::FrozenPublicationStager::discard(bundle, nullptr);
                        return;
                    }
                    if (!bundle.ready()) {
                        if (bundle.disposition == Thoughts::FrozenPublicationDisposition::NoChanges) {
                            Thoughts::FrozenPublicationStager::discard(bundle, nullptr);
                            if (batch.isEmpty()) {
                                coordinator.completeSourcePublication(QString());
                            }
                            for (const QString &target : batch) {
                                coordinator.completeSourcePublication(target);
                            }
                            if (!coordinator.hasSourcePublication()) {
                                coordinator.sourceSnapshot.reset();
                                coordinator.phase = ManualSyncPhase::DrainOutbox;
                                repository->reportSourcePublicationTotals();
                            }
                            repository->scheduleManualSyncAdvance();
                            return;
                        }
                        if (!coordinator.isDocument() && !scopeProject.isEmpty() && bundle.disposition == Thoughts::FrozenPublicationDisposition::RequiresLiveScan) {
                            Thoughts::FrozenPublicationStager::discard(bundle, nullptr);
                            coordinator.promoteSourcePublicationToGlobal();
                            repository->emitSyncProgress(QStringLiteral("preparing"), 0, 0, QStringLiteral("Resolving an unassigned source dependency safely"));
                            repository->scheduleManualSyncAdvance();
                            return;
                        }
                        QString trackingError;
                        if (Thoughts::DesktopSyncStore *const store = repository->desktopSyncStore(&trackingError)) {
                            store->markReconciliationRequired(nullptr);
                        }
                        repository->failManualSync(scopeTarget.isEmpty() ? QStringLiteral("The complete source dependency closure could not be frozen safely.")
                                                                         : QStringLiteral("The affected source dependency closure could not be frozen safely."),
                                                   bundle.error.isEmpty() ? trackingError : bundle.error);
                        return;
                    }
                    // PDFs of the batch no pending path touches had nothing to publish.
                    const QStringList staged = bundle.targets.isEmpty() ? (scopeTarget.isEmpty() ? QStringList() : QStringList {bundle.target}) : bundle.targets;
                    for (const QString &target : batch) {
                        if (!staged.contains(target)) {
                            coordinator.completeSourcePublication(target);
                        }
                    }
                    coordinator.sourcePublicationBatch = staged;
                    coordinator.frozenPublication = std::make_unique<Thoughts::FrozenPublicationBundle>(std::move(bundle));
                    repository->startManualSourcePublication();
                },
                Qt::QueuedConnection);
        });
        return;
    }
    const quint64 sourceGeneration = m_syncCoordinator.sourceSnapshot ? m_syncCoordinator.sourceSnapshot->throughGeneration : 0;
    const QString sourceTransferId = Internal::sourcePublicationTransferId(scopeTarget, sourceGeneration);
    const auto frozenBundle = QSharedPointer<Thoughts::FrozenPublicationBundle>::create();
    *frozenBundle = std::move(*m_syncCoordinator.frozenPublication);
    m_syncCoordinator.frozenPublication.reset();
    const QStringList bundleTargets = frozenBundle->targets.isEmpty() ? (frozenBundle->target.isEmpty() ? QStringList() : QStringList {frozenBundle->target}) : frozenBundle->targets;
    if (!frozenBundle->ready() || frozenBundle->target != scopeTarget || bundleTargets != batch) {
        Thoughts::FrozenPublicationStager::discard(*frozenBundle, nullptr);
        failManualSync(QStringLiteral("The source publication is not backed by a complete immutable bundle."));
        return;
    }
    auto *process = new QProcess(this);
    const QProcessEnvironment environment = Internal::sourcePublisherProcessEnvironment(token, m_syncCoordinator.sourcePublicationTraceId);
    process->setProcessEnvironment(environment);
    process->setProgram(executable);
    QStringList arguments = sourcePublicationArguments(QString());
    if (!frozenBundle->sourceManifestPath.isEmpty()) {
        const qsizetype manifestArgument = arguments.indexOf(QStringLiteral("--manifest"));
        if (manifestArgument < 0 || manifestArgument + 1 >= arguments.size()) {
            Thoughts::FrozenPublicationStager::discard(*frozenBundle, nullptr);
            failManualSync(QStringLiteral("The frozen source manifest could not be selected safely."));
            process->deleteLater();
            return;
        }
        arguments[manifestArgument + 1] = frozenBundle->sourceManifestPath;
    }
    if (!scopeTarget.isEmpty()) {
        arguments.append({QStringLiteral("--scope-target"), scopeTarget});
        for (qsizetype index = 1; index < batch.size(); ++index) {
            arguments.append({QStringLiteral("--also-scope-target"), batch.at(index)});
        }
    }
    if (frozenBundle->ready()) {
        arguments.append({QStringLiteral("--frozen-inputs"), frozenBundle->manifestPath});
    }
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    const auto stderrBuffer = QSharedPointer<QByteArray>::create();
    const auto rawStderr = QSharedPointer<QByteArray>::create();
    const auto transferAccountingError = QSharedPointer<bool>::create(false);
    const auto consumeSourcePublisherStderr = [this, process, stderrBuffer, rawStderr, sourceTransferId, transferAccountingError](bool flush) {
        const QByteArray incoming = process->readAllStandardError();
        rawStderr->append(incoming);
        const QList<QJsonObject> events = Internal::takeSourcePublisherEvents(*stderrBuffer, incoming, flush);
        for (const QJsonObject &event : events) {
            const qint64 uploadedBytes = event.value(QStringLiteral("uploaded_bytes")).toInteger(-1);
            const qint64 downloadedBytes = event.value(QStringLiteral("downloaded_bytes")).toInteger(-1);
            if (uploadedBytes >= 0 && downloadedBytes >= 0 && !updateManualTransferTotals(sourceTransferId, uploadedBytes, downloadedBytes)) {
                *transferAccountingError = true;
            }
            if (!m_activeTraceId.isEmpty()) {
                QJsonObject recordedEvent = event;
                QJsonObject metadata = recordedEvent.value(QStringLiteral("metadata")).toObject();
                metadata.insert(QStringLiteral("transfer_id"), sourceTransferId);
                recordedEvent.insert(QStringLiteral("metadata"), metadata);
                Q_EMIT syncJournalEvent(m_activeTraceId, recordedEvent);
            }
        }
    };
    connect(process, &QProcess::readyReadStandardError, this, [consumeSourcePublisherStderr] { consumeSourcePublisherStderr(false); });
    m_publishingSources = true;
    const qint64 discoveryMs = m_publicationTimer.isValid() ? m_publicationTimer.elapsed() : -1;
    m_publicationTimer.invalidate();
    QElapsedTimer publisherTimer;
    publisherTimer.start();
    emitSyncProgress(QStringLiteral("uploading"), 0, 0,
                     scopeTarget.isEmpty() ? QStringLiteral("Publishing local source changes")
                                           : (batch.size() > 1 ? QStringLiteral("Publishing local changes for %1 documents: %2").arg(batch.size()).arg(batch.join(QStringLiteral(", ")))
                                                               : QStringLiteral("Publishing local changes for %1").arg(scopeTarget)));
    connect(process, &QProcess::finished, this, [this, process, batch, scopeTarget, scopeProject, sourceTransferId, frozenBundle, transferAccountingError, consumeSourcePublisherStderr, rawStderr, discoveryMs, publisherTimer](int exitCode, QProcess::ExitStatus status) {
        const qint64 publisherMs = publisherTimer.elapsed();
        const auto discardFrozen = qScopeGuard([frozenBundle] { Thoughts::FrozenPublicationStager::discard(*frozenBundle, nullptr); });
        m_publishingSources = false;
        consumeSourcePublisherStderr(true);
        const QString detail = QString::fromUtf8(*rawStderr).trimmed();
        const QByteArray output = process->readAllStandardOutput();
        process->deleteLater();
        if (status != QProcess::NormalExit || exitCode != 0) {
            rollbackManualTransferTotals(sourceTransferId);
            failManualSync(QStringLiteral("The local source changes could not be published to the server."), detail, processFailureKind(detail));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(output);
        if (!document.isObject()) {
            rollbackManualTransferTotals(sourceTransferId);
            failManualSync(QStringLiteral("The source publisher returned an invalid accepted state."));
            return;
        }
        const QJsonObject publication = document.object();
        const qint64 frontier = publication.value(QStringLiteral("source_frontier")).toInteger(-1);
        const qint64 uploadedBytes = publication.value(QStringLiteral("uploaded_bytes")).toInteger(-1);
        const qint64 downloadedBytes = publication.value(QStringLiteral("downloaded_bytes")).toInteger(-1);
        if (frontier < 0) {
            rollbackManualTransferTotals(sourceTransferId);
            failManualSync(QStringLiteral("The source publisher returned an invalid source frontier."));
            return;
        }
        if (uploadedBytes < 0 || downloadedBytes < 0 || *transferAccountingError) {
            rollbackManualTransferTotals(sourceTransferId);
            failManualSync(QStringLiteral("The source publisher returned inconsistent transfer progress."),
                           QStringLiteral("Transfer %1: uploaded=%2, downloaded=%3, progress_rejected=%4")
                               .arg(sourceTransferId).arg(uploadedBytes).arg(downloadedBytes).arg(*transferAccountingError));
            return;
        }
        if (!commitManualTransferTotals(sourceTransferId, uploadedBytes, downloadedBytes)) {
            rollbackManualTransferTotals(sourceTransferId);
            failManualSync(QStringLiteral("The source publisher returned inconsistent transfer totals."));
            return;
        }
        if (frozenBundle->ready() && publication.value(QStringLiteral("accepted_frozen_entries")).toArray() != frozenBundle->acceptedEntries) {
            failManualSync(QStringLiteral("The server did not accept the exact immutable source generation."));
            return;
        }
        if (!frozenBundle->sourceManifestPath.isEmpty() && !Internal::installAcceptedSourceManifest(frozenBundle->sourceManifestPath, m_sourceManifestPath)) {
            failManualSync(QStringLiteral("The accepted source manifest could not be activated locally."));
            return;
        }
        for (const QString &target : batch) {
            m_syncCoordinator.completedSourcePublicationTargets.insert(target);
        }
        QString trackingError;
        if (!acceptDesktopSourcePublicationForTargets(publication, batch, &trackingError)) {
            failManualSync(QStringLiteral("The published source baseline could not be recorded safely."), trackingError);
            return;
        }
        emitSyncTiming(QStringLiteral("publication"), QStringLiteral("publication %1: discovery %2 ms, publisher process %3 ms, accept %4 ms")
                                                          .arg(scopeTarget.isEmpty() ? QStringLiteral("all")
                                                                                     : (batch.size() > 1 ? QStringLiteral("%1 (%2 documents of %3)").arg(batch.join(QStringLiteral(", "))).arg(batch.size()).arg(scopeProject.isEmpty() ? sourceManifestProjectsByTarget(m_sourceManifestPath).value(scopeTarget) : scopeProject)
                                                                                                         : scopeTarget))
                                                          .arg(discoveryMs).arg(publisherMs).arg(publisherTimer.elapsed() - publisherMs));
        m_syncCoordinator.publishedSourceFrontier = frontier;
        m_syncCoordinator.pendingAuthoringPaths.clear();
        noteVerifiedSourcePublication(publication);
        if (batch.isEmpty()) {
            m_syncCoordinator.completeSourcePublication(QString());
        }
        for (const QString &target : batch) {
            m_syncCoordinator.completeSourcePublication(target);
        }
        ++m_syncCoordinator.sourcePublicationRuns;
        m_syncCoordinator.sourcePublicationDocuments += batch.isEmpty() ? 1 : batch.size();
        if (!m_syncCoordinator.hasSourcePublication()) {
            m_syncCoordinator.sourceSnapshot.reset();
            m_syncCoordinator.phase = ManualSyncPhase::DrainOutbox;
            reportSourcePublicationTotals();
        }
        scheduleManualSyncAdvance();
    });
    process->start();
}

QList<QJsonObject> FedoraRepository::documentOperations(const QString &target, const QList<QJsonObject> &operations) const
{
    if (!safePdfFilename(target)) {
        return {};
    }
    const QString stem = QFileInfo(target).completeBaseName();
    QHash<QString, QString> cardStems;
    QSet<QString> targetCaptures;
    const auto inspect = [&](const QString &type, const QString &entityId, const QJsonObject &payload) {
        if (type == QLatin1String("review_card")) {
            const QString cardStem = payload.value(QStringLiteral("primary")).toObject().value(QStringLiteral("pdf_stem")).toString();
            if (!cardStem.isEmpty()) {
                cardStems.insert(entityId, cardStem);
            }
        } else if (type == QLatin1String("voice_assignment") && payload.value(QStringLiteral("target")).toObject().value(QStringLiteral("collection")).toString() == stem) {
            targetCaptures.insert(entityId);
        }
    };
    const QJsonObject projections = effectiveProjections();
    for (auto iterator = projections.constBegin(); iterator != projections.constEnd(); ++iterator) {
        const QStringList key = iterator.key().split(QLatin1Char('\n'));
        if (key.size() == 3) {
            inspect(key.at(0), key.at(2), iterator.value().toObject());
        }
    }
    for (const QJsonObject &operation : operations) {
        inspect(operation.value(QStringLiteral("entity_type")).toString(), operation.value(QStringLiteral("entity_id")).toString(), operation.value(QStringLiteral("payload")).toObject());
    }

    // Link overrides belong to the PDF that contains the overridden link.
    const QString targetDocumentId = Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(target).value(QStringLiteral("document_id")).toString();
    QList<QJsonObject> scoped;
    for (const QJsonObject &operation : operations) {
        const QString outboxKind = operation.value(QStringLiteral("outbox_kind")).toString();
        if (outboxKind == QLatin1String("agent_proposal_resolution") || outboxKind == QLatin1String("agent_proposal_undo") || outboxKind == QLatin1String("source_conflict_resolution")) {
            // Proposal decisions and source-conflict resolutions mutate
            // guarded TeX independently of a PDF collection. An explicit
            // document/CLI Sync must still drain them; otherwise it could
            // report success while durable source work remained queued.
            scoped.append(operation);
            continue;
        }
        QString type = operation.value(QStringLiteral("entity_type")).toString();
        if (type == QLatin1String("comment")) {
            type = QStringLiteral("annotation");
        }
        const QString entityId = operation.value(QStringLiteral("entity_id")).toString();
        const QString collection = operation.value(QStringLiteral("collection")).toString();
        const QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
        const bool directFilename = payload.value(QStringLiteral("pdf_filename")).toString() == target || payload.value(QStringLiteral("library_filename")).toString() == target ||
            (payload.value(QStringLiteral("target")).isString() && payload.value(QStringLiteral("target")).toString() == target);
        const bool belongs = type == QLatin1String("proposal_event") || collection == stem || directFilename || (type == QLatin1String("review_card") && cardStems.value(entityId) == stem) ||
            ((type == QLatin1String("review_event") || type == Thoughts::ReviewCardFeedback::entityType())
             && cardStems.value(payload.value(QStringLiteral("card_id")).toString()) == stem) ||
            (type == QLatin1String("review_exclusion") && payload.value(QStringLiteral("pdf_stem")).toString() == stem) || (type == QLatin1String("voice_assignment") && targetCaptures.contains(entityId)) ||
            (type == QLatin1String("voice_capture") && targetCaptures.contains(entityId))
            || (type == Thoughts::PdfLinkOverrides::entityType() && !targetDocumentId.isEmpty()
                && payload.value(QStringLiteral("source_document_id")).toString() == targetDocumentId)
            // Document feedback belongs to its anchor's document.
            || (type == Thoughts::DocumentFeedback::entityType() && !targetDocumentId.isEmpty()
                && Thoughts::DocumentFeedback::documentIdOf(payload) == targetDocumentId);
        if (belongs && outboxKind.isEmpty()) {
            scoped.append(operation);
        }
    }
    return scoped;
}

bool FedoraRepository::applyDocumentSnapshot(const QJsonObject &snapshot)
{
    if (snapshot.value(QStringLiteral("target")).toString() != m_syncCoordinator.target || !snapshot.value(QStringLiteral("projections")).isArray()) {
        return false;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject projections = state.value(QStringLiteral("projections")).toObject();
    QJsonObject incoming;
    for (const QJsonValue &value : snapshot.value(QStringLiteral("projections")).toArray()) {
        const QJsonObject projection = value.toObject();
        QString type = projection.value(QStringLiteral("entity_type")).toString();
        if (type == QLatin1String("comment")) {
            type = QStringLiteral("annotation");
        }
        const QString collection = projection.value(QStringLiteral("collection")).toString();
        const QString entityId = projection.value(QStringLiteral("entity_id")).toString();
        QJsonObject payload = projection.value(QStringLiteral("payload")).toObject();
        const bool deleted = projection.value(QStringLiteral("deleted")).toBool() || payload.value(QStringLiteral("deleted")).toBool();
        if (!safeComponent(type) || !safeComponent(collection) || !safeComponent(entityId) || !projection.value(QStringLiteral("payload")).isObject() || (deleted && !isCommentProjection(type))
            || type == QLatin1String("proposal_event")) {
            continue;
        }
        if (deleted) {
            payload.insert(QStringLiteral("deleted"), true);
        }
        incoming.insert(type + QLatin1Char('\n') + collection + QLatin1Char('\n') + entityId, payload);
    }

    QHash<QString, QString> cardStems;
    QSet<QString> targetCaptures;
    const QString stem = QFileInfo(m_syncCoordinator.target).completeBaseName();
    const auto inspectRelations = [&](const QJsonObject &values) {
        for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator) {
            const QStringList key = iterator.key().split(QLatin1Char('\n'));
            if (key.size() != 3) {
                continue;
            }
            const QJsonObject payload = iterator.value().toObject();
            if (key.at(0) == QLatin1String("review_card")) {
                cardStems.insert(key.at(2), payload.value(QStringLiteral("primary")).toObject().value(QStringLiteral("pdf_stem")).toString());
            } else if (key.at(0) == QLatin1String("voice_assignment") && payload.value(QStringLiteral("target")).toObject().value(QStringLiteral("collection")).toString() == stem) {
                targetCaptures.insert(key.at(2));
            }
        }
    };
    inspectRelations(projections);
    inspectRelations(incoming);
    const QString targetDocumentId = Thoughts::DocumentIdentityStore(m_docsRoot).byFilename(m_syncCoordinator.target).value(QStringLiteral("document_id")).toString();
    for (auto iterator = projections.begin(); iterator != projections.end();) {
        const QStringList key = iterator.key().split(QLatin1Char('\n'));
        const QJsonObject payload = iterator.value().toObject();
        const QString type = key.value(0);
        const QString collection = key.value(1);
        const QString entityId = key.value(2);
        const bool directFilename = payload.value(QStringLiteral("pdf_filename")).toString() == m_syncCoordinator.target || payload.value(QStringLiteral("library_filename")).toString() == m_syncCoordinator.target ||
            (payload.value(QStringLiteral("target")).isString() && payload.value(QStringLiteral("target")).toString() == m_syncCoordinator.target);
        const bool belongs = key.size() == 3 &&
            (collection == stem || directFilename || (type == QLatin1String("review_card") && cardStems.value(entityId) == stem) ||
             ((type == QLatin1String("review_event") || type == Thoughts::ReviewCardFeedback::entityType())
              && cardStems.value(payload.value(QStringLiteral("card_id")).toString()) == stem) ||
             (type == QLatin1String("review_exclusion") && payload.value(QStringLiteral("pdf_stem")).toString() == stem) ||
             ((type == QLatin1String("voice_assignment") || type == QLatin1String("voice_capture")) && targetCaptures.contains(entityId)) ||
             // The snapshot holds every current link override of the document.
             (type == Thoughts::PdfLinkOverrides::entityType() && !targetDocumentId.isEmpty()
              && payload.value(QStringLiteral("source_document_id")).toString() == targetDocumentId) ||
             // ... and every current document feedback of the document (anchor.pdf.document_id).
             (type == Thoughts::DocumentFeedback::entityType() && !targetDocumentId.isEmpty()
              && Thoughts::DocumentFeedback::documentIdOf(payload) == targetDocumentId));
        if (belongs && !isKeptTombstone(type, payload)) {
            iterator = projections.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = incoming.constBegin(); iterator != incoming.constEnd(); ++iterator) {
        projections.insert(iterator.key(), iterator.value());
    }
    state.insert(QStringLiteral("projections"), projections);
    if (!writeRepositoryState(state)) {
        return false;
    }
    emitEffectiveProjections();
    return true;
}

void FedoraRepository::pullDocumentSnapshot()
{
    if (!m_client || m_pulling || m_readerPlanPulling || !safePdfFilename(m_syncCoordinator.target)) {
        return;
    }
    const QString target = m_syncCoordinator.target;
    if (m_targetReaderPlan.isEmpty() || m_targetReaderPlanTarget != target) {
        requestReaderSyncPlan(target, [this, target](const QJsonObject &plan, const QString &error) {
            if (plan.isEmpty()) {
                failManualSync(QStringLiteral("The target reader synchronization plan could not be prepared."), error);
                return;
            }
            m_targetReaderPlan = plan;
            m_targetReaderPlanTarget = target;
            m_targetReaderPlanPublicationIdentity.clear();
            initializeReaderPlanTransfers(plan);
            pullDocumentSnapshot();
        });
        return;
    }
    m_pulling = true;
    // A target reader plan models the full compact document snapshot.  An ETag
    // shortcut would transfer a different body and is therefore forbidden.
    retryingRequest([this, target] { return m_client->documentSync(target); }, [this, target](QNetworkReply *reply) {
        m_pulling = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            failManualSync(error.summary, error.details);
            return;
        }
        const QByteArray body = reply->readAll();
        QByteArray responseEtag = reply->rawHeader(QByteArrayLiteral("ETag")).trimmed();
        if (responseEtag.startsWith('"') && responseEtag.endsWith('"')) {
            responseEtag = responseEtag.mid(1, responseEtag.size() - 2);
        }
        const QJsonDocument response = QJsonDocument::fromJson(body);
        reply->deleteLater();
        const QJsonObject snapshot = response.object();
        if (!response.isObject() || snapshot.value(QStringLiteral("target")).toString() != target ||
            snapshot.value(QStringLiteral("snapshot_sequence")).toInteger(-1) != m_targetReaderPlan.value(QStringLiteral("latest_server_sequence")).toInteger(-2) || !accountReaderMetadataBytes(body.size()) ||
            !accountReaderMetadataBytes(0, true)) {
            supersedeTargetReaderPlan(target, {}, QStringLiteral("The document snapshot diverged from its approved reader plan."));
            return;
        }
        const QJsonObject plan = m_targetReaderPlan;
        verifyReaderPlanState(plan, [this, target, snapshot, responseEtag, plan](bool current, const QString &error) {
            if (m_targetReaderPlan.value(QStringLiteral("plan_id")) != plan.value(QStringLiteral("plan_id"))) {
                return;
            }
            const QJsonObject publication = snapshot.value(QStringLiteral("publication")).toObject();
            if (!current) {
                supersedeTargetReaderPlan(target, publication, error);
                return;
            }
            if (!applyDocumentSnapshot(snapshot)) {
                failManualSync(QStringLiteral("The server returned an invalid document snapshot."));
                return;
            }
            QJsonObject repositoryState = readRepositoryState();
            if (validSha256(QString::fromLatin1(responseEtag))) {
                QJsonObject etags = repositoryState.value(QStringLiteral("document_sync_etags")).toObject();
                etags.insert(target, QString::fromLatin1(responseEtag));
                repositoryState.insert(QStringLiteral("document_sync_etags"), etags);
                writeRepositoryState(repositoryState);
            }
            if (publication.value(QStringLiteral("filename")).toString() != target || !validSha256(publication.value(QStringLiteral("sha256")).toString()) || publication.value(QStringLiteral("size_bytes")).toInteger(-1) < 0) {
                failManualSync(QStringLiteral("The server returned an invalid document publication."));
                return;
            }
            QJsonObject renderStatus = snapshot.value(QStringLiteral("render_status")).toObject();
            if (renderStatus.isEmpty()) {
                renderStatus.insert(QStringLiteral("status"), QStringLiteral("synced"));
                renderStatus.insert(QStringLiteral("reason"), QStringLiteral("This unlinked Library PDF is synchronized with the server."));
            }
            renderStatus.insert(QStringLiteral("publication"), publication);
            renderStatus.insert(QStringLiteral("refreshed"), true);
            const QString localState = localPublicationState(target, publication);
            renderStatus.insert(QStringLiteral("local_state"), localState);
            storeRenderStatus(target, renderStatus);
            const QString identity = readerPublicationIdentity(publication);
            m_targetReaderPlanPublicationIdentity = identity;
            if (localState == QLatin1String("current") && m_targetReaderPlan.value(QStringLiteral("objects")).toArray().isEmpty()) {
                m_targetReaderPlanRetries = 0;
                completeManualSync();
                return;
            }
            QJsonObject stateWithRun = readRepositoryState();
            QJsonObject runs = stateWithRun.value(QStringLiteral("build_runs")).toObject();
            const QString runId = m_activeTraceId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_activeTraceId;
            runs.insert(target,
                        QJsonObject {{QStringLiteral("run_id"), runId},
                                     {QStringLiteral("trace_id"), runId},
                                     {QStringLiteral("status"), QStringLiteral("activating")},
                                     {QStringLiteral("publication"), publication},
                                     {QStringLiteral("first_publication"), false}});
            stateWithRun.insert(QStringLiteral("build_runs"), runs);
            if (!writeRepositoryState(stateWithRun)) {
                failManualSync(QStringLiteral("The targeted PDF activation could not be journaled."));
                return;
            }
            m_targetedBuildTarget = target;
            startTargetedPublicationActivation(target, publication);
        });
    });
}

void FedoraRepository::processNextManualLibraryMutation()
{
    if (m_libraryMutationRunning || !m_client) {
        if (!m_client) {
            failManualSync(QStringLiteral("The server connection is unavailable."));
        }
        return;
    }
    QString queueError;
    Thoughts::DesktopSyncStore *const queueStore = desktopSyncStore(&queueError);
    const std::optional<Thoughts::DesktopLibraryOperation> claimed = queueStore ? queueStore->claimNextLibraryOperation(&queueError) : std::nullopt;
    if (!queueError.isEmpty() || !queueStore) {
        failManualSync(QStringLiteral("The durable local Library queue could not be read."), queueError);
        return;
    }
    if (!claimed) {
        if (m_syncCoordinator.postLocalWorkAction == SyncCoordinator::PostLocalWorkAction::ReaderStateRecheck) {
            beginManualReaderStateRecheck();
        } else if (m_syncCoordinator.conditionalRenderCheck) {
            m_syncCoordinator.phase = ManualSyncPhase::RenderCheck;
            scheduleManualSyncAdvance();
        } else {
            beginManualReaderStateRecheck();
        }
        return;
    }

    const Thoughts::DesktopLibraryOperation libraryOperation = *claimed;
    m_syncCoordinator.postLocalWorkAction = SyncCoordinator::PostLocalWorkAction::ConditionalRenderCheck;
    const QJsonObject mutation = libraryOperation.mutation;
    const QString type = mutation.value(QStringLiteral("type")).toString();
    const QString filename = mutation.value(QStringLiteral("filename")).toString();
    const QString digest = mutation.value(QStringLiteral("sha256")).toString();
    const QString requestIdentity =
        mutation.value(QStringLiteral("request_id")).toString().isEmpty() ? QString::fromLatin1(QCryptographicHash::hash(compactJson(mutation), QCryptographicHash::Sha256).toHex()) : mutation.value(QStringLiteral("request_id")).toString();
    m_libraryMutationRunning = true;
    schedulePendingChangesRefresh();
    const auto acknowledge = [this, libraryOperation] {
        QString error;
        Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
        m_libraryMutationRunning = false;
        if (!store || !store->acknowledgeLibraryOperation(libraryOperation.operationId, libraryOperation.generation, &error)) {
            failManualSync(QStringLiteral("The synchronized Library change could not be acknowledged safely."), error);
            return;
        }
        schedulePendingChangesRefresh();
        scheduleManualSyncAdvance();
    };
    const auto releaseAndFail = [this, libraryOperation](const QString &message, const QString &details = QString()) {
        QString releaseError;
        Thoughts::DesktopSyncStore *const store = desktopSyncStore(&releaseError);
        if (store) {
            store->releaseLibraryOperation(libraryOperation.operationId, libraryOperation.generation, &releaseError);
        }
        m_libraryMutationRunning = false;
        schedulePendingChangesRefresh();
        failManualSync(message, releaseError.isEmpty() ? details : (details.isEmpty() ? releaseError : details + QLatin1Char('\n') + releaseError));
    };
    const auto fail = [this, releaseAndFail](QNetworkReply *reply) {
        const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
        Q_EMIT connectionStateChanged(false, error.summary);
        releaseAndFail(error.summary, error.details);
    };

    if (!safePdfFilename(filename) || !validSha256(digest)) {
        releaseAndFail(QStringLiteral("A durable local Library change is invalid."));
        return;
    }
    if (type == QLatin1String("remove")) {
        QJsonObject state = readRepositoryState();
        QJsonObject localRemovalTombstones = state.value(QStringLiteral("local_library_removals")).toObject();
        const QJsonObject existing = localRemovalTombstones.value(filename).toObject();
        if (existing.value(QStringLiteral("sha256")).toString() != digest) {
            localRemovalTombstones.insert(filename,
                                          QJsonObject {{QStringLiteral("request_id"), requestIdentity},
                                                       {QStringLiteral("sha256"), digest},
                                                       {QStringLiteral("created_at"), QDateTime::fromMSecsSinceEpoch(libraryOperation.createdAtMs, Qt::UTC).toString(Qt::ISODateWithMs)}});
            state.insert(QStringLiteral("local_library_removals"), localRemovalTombstones);
            if (!writeRepositoryState(state)) {
                releaseAndFail(QStringLiteral("The local Library removal could not be preserved before synchronization."));
                return;
            }
            schedulePendingChangesRefresh();
        }
    }

    if (type == QLatin1String("add")) {
        QFile file(mutation.value(QStringLiteral("local_path")).toString());
        if (!file.open(QIODevice::ReadOnly)) {
            releaseAndFail(QStringLiteral("The locally added PDF is no longer available; it was not sent to the server."), file.errorString());
            return;
        }
        const auto contents = QSharedPointer<QByteArray>::create(file.readAll());
        if (QString::fromLatin1(Thoughts::SyncClient::sha256(*contents)) != digest) {
            releaseAndFail(QStringLiteral("The locally added PDF changed before Sync; it was not sent to the server."));
            return;
        }
        const auto documentId = mutation.value(QStringLiteral("document_id")).toString();
        if (!Thoughts::DocumentIdentityStore::validId(documentId)) { releaseAndFail(QStringLiteral("The imported document identifier is missing.")); return; }
        const auto importObject = [this, filename, digest, documentId, requestIdentity, acknowledge, fail, releaseAndFail] {
            const QJsonObject payload {{QStringLiteral("document_id"), documentId}, {QStringLiteral("filename"), filename}, {QStringLiteral("object_sha256"), digest}};
            const QByteArray encoded = compactJson(payload);
            const QString metadataUploadItem = QStringLiteral("library-import:%1").arg(requestIdentity);
            if (!planManualUpload(metadataUploadItem, encoded.size())) {
                releaseAndFail(QStringLiteral("The Library import has inconsistent transfer accounting."));
                return;
            }
            QNetworkReply *request = m_client->importLibraryPdf(payload);
            connect(request, &QNetworkReply::uploadProgress, this, [this, metadataUploadItem, size = encoded.size()](qint64 sent, qint64) { accountManualUploadProgress(metadataUploadItem, qMin(qMax<qint64>(0, sent), size)); });
            connect(request, &QNetworkReply::finished, this, [this, request, filename, digest, documentId, metadataUploadItem, size = encoded.size(), acknowledge, fail, releaseAndFail] {
                if (request->error() != QNetworkReply::NoError) {
                    accountManualUploadProgress(metadataUploadItem, 0, false, true);
                    fail(request);
                    request->deleteLater();
                    return;
                }
                accountManualUploadProgress(metadataUploadItem, size, true);
                const QJsonDocument response = QJsonDocument::fromJson(request->readAll());
                const QJsonObject result = response.object();
                request->deleteLater();
                const QString status = result.value(QStringLiteral("status")).toString();
                if (!response.isObject() || result.value(QStringLiteral("document_id")).toString() != documentId || result.value(QStringLiteral("filename")).toString() != filename || result.value(QStringLiteral("sha256")).toString() != digest ||
                    (status != QLatin1String("imported") && status != QLatin1String("existing"))) {
                    releaseAndFail(QStringLiteral("The server returned an invalid Library import result."));
                    return;
                }
                acknowledge();
            });
        };
        QNetworkReply *metadata = m_client->objectMetadata(digest.toLatin1());
        connect(metadata, &QNetworkReply::finished, this, [this, metadata, contents, digest, requestIdentity, importObject, fail, releaseAndFail] {
            const int status = metadata->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (metadata->error() == QNetworkReply::NoError) {
                bool sizeOk = false;
                const qint64 remoteSize = metadata->rawHeader(QByteArrayLiteral("Content-Length")).toLongLong(&sizeOk);
                const QByteArray expectedEtag = QByteArrayLiteral("\"") + digest.toLatin1() + QByteArrayLiteral("\"");
                const QByteArray remoteEtag = metadata->rawHeader(QByteArrayLiteral("ETag"));
                metadata->deleteLater();
                if (!sizeOk || remoteSize != contents->size() || remoteEtag != expectedEtag) {
                    releaseAndFail(QStringLiteral("The server reported inconsistent metadata for the existing PDF object."));
                    return;
                }
                importObject();
                return;
            }
            if (status != 404 && status != 405 && status != 501) {
                fail(metadata);
                metadata->deleteLater();
                return;
            }
            metadata->deleteLater();
            const QString objectUploadItem = QStringLiteral("library-object:%1").arg(requestIdentity);
            if (!planManualUpload(objectUploadItem, contents->size())) {
                releaseAndFail(QStringLiteral("The local PDF has inconsistent transfer accounting."));
                return;
            }
            QNetworkReply *upload = m_client->uploadObject(*contents, QByteArrayLiteral("application/pdf"));
            connect(upload, &QNetworkReply::uploadProgress, this, [this, objectUploadItem, size = contents->size()](qint64 sent, qint64) { accountManualUploadProgress(objectUploadItem, qMin(qMax<qint64>(0, sent), size)); });
            connect(upload, &QNetworkReply::finished, this, [this, upload, contents, digest, objectUploadItem, importObject, fail, releaseAndFail] {
                if (upload->error() != QNetworkReply::NoError) {
                    accountManualUploadProgress(objectUploadItem, 0, false, true);
                    fail(upload);
                    upload->deleteLater();
                    return;
                }
                accountManualUploadProgress(objectUploadItem, contents->size(), true);
                const QJsonDocument response = QJsonDocument::fromJson(upload->readAll());
                const QJsonObject uploaded = response.object();
                upload->deleteLater();
                if (!response.isObject() || uploaded.value(QStringLiteral("sha256")).toString() != digest || uploaded.value(QStringLiteral("size_bytes")).toInteger(-1) != contents->size()) {
                    releaseAndFail(QStringLiteral("The server stored a different PDF object."));
                    return;
                }
                importObject();
            });
        });
        return;
    }

    QNetworkReply *request = nullptr;
    QJsonObject payload;
    if (type == QLatin1String("rename")) {
        payload = QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("expected_sha256"), digest}, {QStringLiteral("display_name"), mutation.value(QStringLiteral("display_name"))}};
    } else if (type == QLatin1String("archive")) {
        payload = QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("expected_sha256"), digest}, {QStringLiteral("archived"), mutation.value(QStringLiteral("archived"))}};
    } else if (type == QLatin1String("remove")) {
        payload = QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("expected_sha256"), digest}};
    }
    if (payload.isEmpty()) {
        releaseAndFail(QStringLiteral("A durable local Library change is invalid."));
        return;
    }
    const QByteArray encoded = compactJson(payload);
    const QString metadataUploadItem = QStringLiteral("library-%1:%2").arg(type, requestIdentity);
    if (!planManualUpload(metadataUploadItem, encoded.size())) {
        releaseAndFail(QStringLiteral("The local Library change has inconsistent transfer accounting."));
        return;
    }
    if (type == QLatin1String("rename")) {
        request = m_client->setLibraryDisplayName(payload);
    } else if (type == QLatin1String("archive")) {
        request = m_client->setLibraryArchiveState(payload);
    } else {
        request = m_client->removeLibraryPdf(payload);
    }
    connect(request, &QNetworkReply::uploadProgress, this, [this, metadataUploadItem, size = encoded.size()](qint64 sent, qint64) { accountManualUploadProgress(metadataUploadItem, qMin(qMax<qint64>(0, sent), size)); });
    connect(request, &QNetworkReply::finished, this, [this, request, type, digest, mutation, metadataUploadItem, size = encoded.size(), acknowledge, fail, releaseAndFail] {
        if (request->error() != QNetworkReply::NoError) {
            accountManualUploadProgress(metadataUploadItem, 0, false, true);
            fail(request);
            request->deleteLater();
            return;
        }
        accountManualUploadProgress(metadataUploadItem, size, true);
        const QJsonObject result = QJsonDocument::fromJson(request->readAll()).object();
        request->deleteLater();
        const QString status = result.value(QStringLiteral("status")).toString();
        const bool success = type == QLatin1String("rename") ? (status == QLatin1String("updated") || status == QLatin1String("unchanged")) && result.value(QStringLiteral("sha256")).toString() == digest &&
                result.value(QStringLiteral("display_name")).toString() == mutation.value(QStringLiteral("display_name")).toString()
            : type == QLatin1String("archive") ? (status == QLatin1String("updated") || status == QLatin1String("unchanged")) && result.value(QStringLiteral("sha256")).toString() == digest &&
                result.value(QStringLiteral("archived")).toBool() == mutation.value(QStringLiteral("archived")).toBool()
                                               : (status == QLatin1String("removed") || status == QLatin1String("already_removed")) && result.value(QStringLiteral("sha256")).toString() == digest;
        if (!success) {
            releaseAndFail(QStringLiteral("The server returned an invalid Library change result."));
            return;
        }
        acknowledge();
    });
}

void FedoraRepository::recoverManualDeviceSequence()
{
    ++m_deviceSequenceRecoveryAttempts;
    m_uploading = true;
    emitSyncProgress(QStringLiteral("reconciling_device_sequence"), 0, 0, QStringLiteral("Reconciling the local sync sequence"));
    QNetworkReply *reply = m_client->deviceSequence();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_uploading = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            failManualSync(error.summary, error.details);
            return;
        }
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const qint64 next = response.value(QStringLiteral("next_device_sequence")).toInteger(-1);
        QString error;
        if (!m_outbox->reconcileDeviceSequence(next, &error)) {
            failManualSync(QStringLiteral("The local sync sequence could not be reconciled. Your changes remain queued."), error);
            return;
        }
        // Re-read the durable queue; the failed request contains stale numbers.
        scheduleManualSyncAdvance();
    });
}

bool FedoraRepository::retainAcknowledgedProjections(const QJsonArray &operations)
{
    QJsonObject state = readRepositoryState();
    QJsonObject projections = state.value(QStringLiteral("projections")).toObject();
    bool changed = false;
    for (const QJsonValue &value : operations) {
        const QJsonObject operation = value.toObject();
        const QString incomingType = operation.value(QStringLiteral("entity_type")).toString();
        const QString type = incomingType == QLatin1String("comment") ? QStringLiteral("annotation") : incomingType;
        const QString collection = operation.value(QStringLiteral("collection")).toString();
        const QString entityId = operation.value(QStringLiteral("entity_id")).toString();
        if ((!isCommentProjection(type) && type != QLatin1String("review_event") && type != QLatin1String("review_card")
             && type != QLatin1String("review_tag_groups") && type != Thoughts::PdfLinkOverrides::entityType()
             && type != Thoughts::ReviewCardFeedback::entityType() && type != Thoughts::DocumentFeedback::entityType()
             && type != Thoughts::AgentConversations::entityType())
            || !safeComponent(collection) || !safeComponent(entityId) || !operation.value(QStringLiteral("payload")).isObject()) {
            continue;
        }
        const QString key = type + QLatin1Char('\n') + collection + QLatin1Char('\n') + entityId;
        const QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
        // A removed link override is gone, as when its tombstone is downloaded.
        if (type == Thoughts::PdfLinkOverrides::entityType() && payload.value(QStringLiteral("deleted")).toBool()) {
            if (projections.contains(key)) {
                projections.remove(key);
                changed = true;
            }
            continue;
        }
        // Accepted cards must survive removal of the outbox overlay until the
        // server projection arrives, without replacing a newer server revision.
        if (type == QLatin1String("review_card") && projections.contains(key)
            && projections.value(key).toObject().value(QStringLiteral("revision")).toInteger()
                > payload.value(QStringLiteral("revision")).toInteger()) continue;
        // Accepted feedback (tombstones included) goes through the server's merge rules.
        if (type == Thoughts::ReviewCardFeedback::entityType() || type == Thoughts::DocumentFeedback::entityType()) {
            const QJsonValue merged = type == Thoughts::DocumentFeedback::entityType()
                ? Thoughts::DocumentFeedback::merge(projections.value(key).toObject(), payload)
                : Thoughts::ReviewCardFeedback::merge(projections.value(key).toObject(), payload);
            if (projections.value(key) != merged) {
                projections.insert(key, merged);
                changed = true;
            }
            continue;
        }
        if (projections.value(key) != payload) {
            projections.insert(key, payload);
            changed = true;
        }
        if (type == QLatin1String("logical_comment")) {
            const auto beforeCascade = projections;
            cascadeRootDeletion(projections, payload);
            changed = changed || projections != beforeCascade;
        }
    }
    if (!changed) {
        return true;
    }
    // Journal the accepted state before removing the optimistic queue overlay:
    // a later download can fail or omit deleted entities from its snapshot.
    state.insert(QStringLiteral("projections"), projections);
    return writeRepositoryState(state);
}

QStringList FedoraRepository::knownServerCapabilities() const
{
    return m_serverCapabilitiesFresh ? m_serverCapabilities : QSettings().value(QStringLiteral("thoughts-server/capabilities")).toStringList();
}

void FedoraRepository::queryServerCapabilities(QObject *context, std::function<void(const QStringList &)> done)
{
    if (m_serverCapabilitiesFresh || !m_client) {
        done(knownServerCapabilities());
        return;
    }
    QNetworkReply *reply = m_client->ready();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            if (body.value(QStringLiteral("capabilities")).isArray()) {
                QStringList capabilities;
                for (const QJsonValue &value : body.value(QStringLiteral("capabilities")).toArray()) {
                    if (value.isString()) capabilities.append(value.toString());
                }
                m_serverCapabilities = capabilities;
                m_serverCapabilitiesFresh = true;
                QSettings().setValue(QStringLiteral("thoughts-server/capabilities"), capabilities);
            }
        }
        reply->deleteLater();
    });
    connect(reply, &QNetworkReply::finished, context, [this, done] { done(knownServerCapabilities()); });
}

bool FedoraRepository::holdUnsupportedOperations(QList<QJsonObject> &operations)
{
    // Support of the server for the entity type of @p operation: -1 not asked yet, 0 no, 1 yes.
    const auto support = [this](const QJsonObject &operation) {
        const QString type = operation.value(QStringLiteral("entity_type")).toString();
        if (type == QLatin1String("review_tag_groups")) {
            // A group shown as a slider carries "scale", which servers before
            // review_tag_scales_v1 reject (and with it the whole batch); a group under a
            // section header carries "section", which needs review_tag_sections_v1, and a group
            // asked before the answer "phase", which needs review_tag_phases_v1.
            const QJsonArray groups = operation.value(QStringLiteral("payload")).toObject().value(QStringLiteral("groups")).toArray();
            const auto any = [&groups](const QString &field) {
                return std::any_of(groups.begin(), groups.end(), [&field](const QJsonValue &group) { return group.toObject().contains(field); });
            };
            int support = m_reviewTagGroupsSupport;
            if (any(QStringLiteral("scale"))) support = std::min(support, m_reviewTagScaleSupport);
            if (any(QStringLiteral("section"))) support = std::min(support, m_reviewTagSectionSupport);
            if (any(QStringLiteral("phase"))) support = std::min(support, m_reviewTagPhaseSupport);
            return support;
        }
        if (type == Thoughts::PdfLinkOverrides::entityType()) return m_pdfLinkOverridesSupport;
        if (type == Thoughts::ReviewCardFeedback::entityType()) return m_reviewCardFeedbackSupport;
        if (type == Thoughts::DocumentFeedback::entityType()) {
            // Made in an HTML attachment: an older server would refuse the whole batch.
            const QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
            int support = m_documentFeedbackSupport;
            if (Thoughts::DocumentFeedback::isAttachmentFeedback(payload)) support = std::min(support, m_documentFeedbackAttachmentSupport);
            if (!Thoughts::DocumentFeedback::kindOf(payload).isEmpty()) support = std::min(support, m_documentFeedbackKindsSupport);
            if (Thoughts::DocumentFeedback::isDrawing(payload)) support = std::min(support, m_documentFeedbackDrawingSupport);
            if (Thoughts::DocumentFeedback::isTextBox(payload)) support = std::min(support, m_documentFeedbackTextSupport);
            return support;
        }
        if (type == Thoughts::AgentConversations::entityType()) return m_agentConversationsSupport;
        return 1;
    };
    if (std::all_of(operations.cbegin(), operations.cend(), [&](const QJsonObject &operation) { return support(operation) == 1; })) return false;
    if (std::none_of(operations.cbegin(), operations.cend(), [&](const QJsonObject &operation) { return support(operation) < 0; })) {
        // Older servers reject the whole batch; these records stay queued until one accepts them.
        operations.removeIf([&](const QJsonObject &operation) { return support(operation) == 0; });
        return false;
    }
    if (!m_client || m_uploading) return false;
    m_uploading = true;
    auto *reply = m_client->ready();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_uploading = false;
        const QJsonArray capabilities = reply->error() == QNetworkReply::NoError
            ? QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("capabilities")).toArray() : QJsonArray();
        m_reviewTagGroupsSupport = capabilities.contains(QJsonValue(QStringLiteral("review_tag_groups_v1"))) ? 1 : 0;
        m_pdfLinkOverridesSupport = capabilities.contains(QJsonValue(Thoughts::PdfLinkOverrides::capability())) ? 1 : 0;
        m_reviewCardFeedbackSupport = capabilities.contains(QJsonValue(Thoughts::ReviewCardFeedback::capability())) ? 1 : 0;
        m_documentFeedbackSupport = capabilities.contains(QJsonValue(Thoughts::DocumentFeedback::capability())) ? 1 : 0;
        m_documentFeedbackAttachmentSupport = capabilities.contains(QJsonValue(Thoughts::DocumentFeedback::attachmentCapability())) ? 1 : 0;
        m_documentFeedbackKindsSupport = capabilities.contains(QJsonValue(Thoughts::DocumentFeedback::kindsCapability())) ? 1 : 0;
        m_documentFeedbackDrawingSupport = capabilities.contains(QJsonValue(Thoughts::DocumentFeedback::drawingCapability())) ? 1 : 0;
        m_documentFeedbackTextSupport = capabilities.contains(QJsonValue(Thoughts::DocumentFeedback::textCapability())) ? 1 : 0;
        m_agentConversationsSupport = capabilities.contains(QJsonValue(Thoughts::AgentConversations::capability())) ? 1 : 0;
        m_reviewTagScaleSupport = capabilities.contains(QJsonValue(QStringLiteral("review_tag_scales_v1"))) ? 1 : 0;
        m_reviewTagSectionSupport = capabilities.contains(QJsonValue(QStringLiteral("review_tag_sections_v1"))) ? 1 : 0;
        m_reviewTagPhaseSupport = capabilities.contains(QJsonValue(QStringLiteral("review_tag_phases_v1"))) ? 1 : 0;
        reply->deleteLater();
        scheduleManualSyncAdvance();
    });
    return true;
}

void FedoraRepository::uploadNextManualBatch(const QList<QJsonObject> &operations)
{
    if (operations.isEmpty() || !m_client) {
        failManualSync(QStringLiteral("The pending local changes could not be prepared for synchronization."));
        return;
    }
    if (!operations.constFirst().value(QStringLiteral("outbox_kind")).toString().isEmpty()) {
        uploadSpecialOutboxOperation(operations.constFirst(), true);
        return;
    }
    QList<QJsonObject> ordinaryOperations;
    const auto isDismissal = [](const QJsonObject &operation) {
        return operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("proposal");
    };
    for (const QJsonObject &operation : operations) {
        if (!operation.value(QStringLiteral("outbox_kind")).toString().isEmpty()) {
            break;
        }
        // The server refuses a stale Dismiss with a 409 for the whole batch
        // (protocol/agent-proposals.md §10.6): each Dismiss travels alone.
        if (!ordinaryOperations.isEmpty() && (isDismissal(operation) || isDismissal(ordinaryOperations.constFirst()))) {
            break;
        }
        ordinaryOperations.append(operation);
    }
    const bool clientReviews = std::any_of(ordinaryOperations.cbegin(), ordinaryOperations.cend(), [](const auto &op) {
        return op.value(QStringLiteral("payload")).toObject().value(QStringLiteral("source_authority")).toString() == QLatin1String("client-v1")
            && (op.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_card") || op.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_event"));
    });
    // A source change of an existing card (its areas placed again) has its own transaction
    // ID; creations and deletions use the card's or event's ID.
    const bool replacements = std::any_of(ordinaryOperations.cbegin(), ordinaryOperations.cend(), [](const auto &op) {
        const auto payload = op.value(QStringLiteral("payload")).toObject();
        const auto change = payload.value(QStringLiteral("source_change")).toObject();
        return op.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_card") && !change.isEmpty()
            && change.value(QStringLiteral("transaction_id")).toString() != payload.value(QStringLiteral("card_id")).toString();
    });
    // A card moved to another document (move-review-card) needs the server's move checks.
    const bool moves = std::any_of(ordinaryOperations.cbegin(), ordinaryOperations.cend(), [](const auto &op) {
        return op.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_card")
            && op.value(QStringLiteral("payload")).toObject().contains(QStringLiteral("move"));
    });
    if ((clientReviews && !m_clientReviewSourcesReady) || (replacements && !m_clientReviewSourceReplacementReady)
        || (moves && !m_clientReviewCardMoveReady)) {
        m_uploading = true;
        auto *reply = m_client->ready();
        connect(reply, &QNetworkReply::finished, this, [this, reply, operations, replacements, moves] {
            m_uploading = false;
            const auto capabilities = reply->error() == QNetworkReply::NoError
                ? QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("capabilities")).toArray() : QJsonArray();
            reply->deleteLater();
            if (!capabilities.contains(QJsonValue(QStringLiteral("client_review_sources_v1")))) {
                failManualSync(QStringLiteral("Update the server to synchronize local review source changes. They remain saved on this device."));
                return;
            }
            m_clientReviewSourcesReady = true;
            if (replacements && !capabilities.contains(QJsonValue(QStringLiteral("client_review_source_replacement_v1")))) {
                failManualSync(QStringLiteral("Update the server to synchronize review cards whose answer areas were placed again. "
                                              "The new areas and source markers remain saved on this device."));
                return;
            }
            m_clientReviewSourceReplacementReady = m_clientReviewSourceReplacementReady || replacements;
            if (moves && !capabilities.contains(QJsonValue(QStringLiteral("client_review_card_move_v1")))) {
                failManualSync(QStringLiteral("Update the server to synchronize review cards moved to another document. "
                                              "The moved cards remain saved on this device."));
                return;
            }
            m_clientReviewCardMoveReady = m_clientReviewCardMoveReady || moves;
            uploadNextManualBatch(operations);
        });
        return;
    }
    for (const auto &op : std::as_const(ordinaryOperations)) {
        const auto change = op.value(QStringLiteral("payload")).toObject().value(QStringLiteral("source_change")).toObject();
        if (change.isEmpty()) continue;
        for (const QString &field : {QStringLiteral("before_sha256"), QStringLiteral("after_sha256")}) {
            const QString hash = change.value(field).toString();
            if (m_uploadedReviewSourceObjects.contains(hash)) continue;
            QFile file(QDir(m_docsRoot).filePath(QStringLiteral("reviews/source-objects/") + hash));
            if (!validSha256(hash) || !file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
                failManualSync(QStringLiteral("A pending review source object is unavailable. The card operation remains queued.")); return;
            }
            const auto bytes = file.readAll();
            if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != hash) {
                failManualSync(QStringLiteral("A pending review source object failed verification.")); return;
            }
            m_uploading = true;
            auto *head = m_client->objectMetadata(hash.toLatin1());
            connect(head, &QNetworkReply::finished, this, [this, head, operations, hash, bytes] {
                const bool present = head->error() == QNetworkReply::NoError;
                const int status = head->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                head->deleteLater();
                if (present) { m_uploading = false; m_uploadedReviewSourceObjects.insert(hash); uploadNextManualBatch(operations); return; }
                if (status != 404) { m_uploading = false; failManualSync(QStringLiteral("Could not verify the pending review source upload.")); return; }
                const QString item = QStringLiteral("review-source:") + hash;
                if (!planManualUpload(item, bytes.size())) { m_uploading = false; failManualSync(QStringLiteral("Invalid review source transfer accounting.")); return; }
                auto *upload = m_client->uploadObject(bytes, QByteArrayLiteral("text/plain; charset=utf-8"));
                connect(upload, &QNetworkReply::uploadProgress, this, [this, item, size=bytes.size()](qint64 sent, qint64) { accountManualUploadProgress(item, qMin(sent, qint64(size))); });
                connect(upload, &QNetworkReply::finished, this, [this, upload, operations, hash, item, size=bytes.size()] {
                    m_uploading = false;
                    const bool ok = upload->error() == QNetworkReply::NoError;
                    upload->deleteLater();
                    accountManualUploadProgress(item, ok ? size : 0, ok, !ok);
                    if (!ok) { failManualSync(QStringLiteral("The review source upload failed; its card operation remains queued.")); return; }
                    m_uploadedReviewSourceObjects.insert(hash);
                    uploadNextManualBatch(operations);
                });
            });
            return;
        }
    }
    ordinaryOperations = Thoughts::SyncClient::normalizedOperationsForUpload(ordinaryOperations);
    QJsonArray operationValues;
    for (const QJsonObject &operation : std::as_const(ordinaryOperations)) {
        operationValues.append(operation);
    }
    const QByteArray requestBody = compactJson(QJsonObject {{QStringLiteral("operations"), operationValues}});
    const QString uploadItem = transferItemId(QStringLiteral("operations"), requestBody);
    if (!planManualUpload(uploadItem, requestBody.size())) {
        failManualSync(QStringLiteral("The pending local change batch has inconsistent transfer accounting."));
        return;
    }
    m_uploading = true;
    QNetworkReply *reply = m_client->uploadOperations(ordinaryOperations);
    connect(reply, &QNetworkReply::uploadProgress, this, [this, uploadItem, size = requestBody.size()](qint64 sent, qint64) { accountManualUploadProgress(uploadItem, qMin(qMax<qint64>(0, sent), size)); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, ordinaryOperations, uploadItem, size = requestBody.size()] {
        m_uploading = false;
        if (reply->error() != QNetworkReply::NoError) {
            accountManualUploadProgress(uploadItem, 0, false, true);
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply, ordinaryOperations);
            reply->deleteLater();
            static const QRegularExpression sequenceConflict(QStringLiteral("^device sequence [0-9]+ is already assigned to another operation$"));
            if (error.httpStatus == 409 && sequenceConflict.match(error.summary).hasMatch() && m_deviceSequenceRecoveryAttempts < 2) {
                recoverManualDeviceSequence();
                return;
            }
            // A Dismiss refused because the proposal is no longer removed outside
            // Thoughts (restored or dismissed elsewhere) is dropped. It travels alone.
            QStringList dismissals;
            for (const QJsonObject &operation : ordinaryOperations) {
                if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("proposal")) dismissals.append(operation.value(QStringLiteral("operation_id")).toString());
            }
            if (error.httpStatus == 409 && dismissals.size() == ordinaryOperations.size()
                && QString(error.summary + error.details).contains(QLatin1String("no longer removed outside Thoughts"), Qt::CaseInsensitive)) {
                QString acknowledgeError;
                if (m_outbox->acknowledge(dismissals, &acknowledgeError)) {
                    emitEffectiveProjections();
                    scheduleManualSyncAdvance();
                    return;
                }
            }
            Q_EMIT connectionStateChanged(false, error.summary);
            failManualSync(error.summary, error.details);
            return;
        }

        accountManualUploadProgress(uploadItem, size, true);

        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        QHash<QString, QString> expectedEntityTypes;
        QSet<QString> replyDeletions;
        for (const QJsonObject &operation : ordinaryOperations) {
            expectedEntityTypes.insert(operation.value(QStringLiteral("operation_id")).toString(), operation.value(QStringLiteral("entity_type")).toString());
        }
        for (const auto &operation : ordinaryOperations) {
            const auto payload = operation.value(QStringLiteral("payload")).toObject();
            if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("logical_comment")
                && payload.value(QStringLiteral("deleted")).toBool() && !payload.value(QStringLiteral("parent_id")).toString().isEmpty())
                replyDeletions.insert(operation.value(QStringLiteral("operation_id")).toString());
        }
        const Internal::OperationBatchDisposition disposition = Internal::classifyOperationResults(expectedEntityTypes, response.value(QStringLiteral("results")).toArray(), replyDeletions);
        if (!disposition.error.isEmpty()) {
            failManualSync(disposition.error);
            return;
        }
        if (!disposition.logicalConflicts.isEmpty()) {
            failManualSync(QStringLiteral("The server returned an invalid superseded result for a logical comment. The human change remains queued."));
            return;
        }
        QJsonArray acceptedOperations;
        for (const QJsonObject &operation : ordinaryOperations) {
            const QString operationId = operation.value(QStringLiteral("operation_id")).toString();
            // A superseded card, tag layout, link override, answer or document feedback or agent conversation lost to a newer one; keep the server's.
            const QString entityType = operation.value(QStringLiteral("entity_type")).toString();
            if (disposition.acknowledged.contains(operationId) && !disposition.refused.contains(operationId)
                && !((entityType == QLatin1String("review_card") || entityType == QLatin1String("review_tag_groups")
                      || entityType == Thoughts::PdfLinkOverrides::entityType() || entityType == Thoughts::ReviewCardFeedback::entityType()
                      || entityType == Thoughts::DocumentFeedback::entityType() || entityType == Thoughts::AgentConversations::entityType())
                     && disposition.superseded.contains(operationId))) {
                acceptedOperations.append(operation);
            }
        }
        if (!retainAcknowledgedProjections(acceptedOperations)) {
            failManualSync(QStringLiteral("The accepted changes could not be saved locally. They remain safely queued."));
            return;
        }
        QString error;
        if (!disposition.acknowledged.isEmpty() && !m_outbox->acknowledge(QList<QString>(disposition.acknowledged.begin(), disposition.acknowledged.end()), &error)) {
            failManualSync(error.isEmpty() ? QStringLiteral("The acknowledged local changes could not be removed from the durable queue.") : error);
            return;
        }
        Q_EMIT connectionStateChanged(true, QString());
        scheduleManualSyncAdvance();
    });
}

void FedoraRepository::uploadSpecialOutboxOperation(const QJsonObject &operation, bool manual)
{
    if (!m_client || m_uploading) {
        if (manual) {
            failManualSync(QStringLiteral("The pending proposal decision could not be prepared for synchronization."));
        }
        return;
    }
    const QString kind = operation.value(QStringLiteral("outbox_kind")).toString();
    const QString operationId = operation.value(QStringLiteral("operation_id")).toString();
    if (kind == QLatin1String("source_conflict_resolution")) {
        const QString conflictId = operation.value(QStringLiteral("conflict_id")).toString();
        const QString expectedSha256 = operation.value(QStringLiteral("expected_sha256")).toString();
        const QString contentSha256 = operation.value(QStringLiteral("content_sha256")).toString();
        const qint64 expectedRevision = operation.value(QStringLiteral("expected_revision")).toInteger(-1);
        const QByteArray contents = QByteArray::fromBase64(operation.value(QStringLiteral("content_base64")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        const bool valid = safeComponent(conflictId) && safeComponent(operationId) && expectedRevision >= 0 && validSha256(expectedSha256) && validSha256(contentSha256) && !contents.isNull()
            && contents.size() <= 4 * 1024 * 1024 && !contents.contains('\0') && QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) == contentSha256;
        if (!valid) {
            Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("failed"), QStringLiteral("The durable source-conflict resolution is invalid."));
            if (manual) {
                failManualSync(QStringLiteral("The durable source-conflict resolution is invalid."));
            }
            return;
        }
        const QJsonArray proposalDecisions = operation.value(QStringLiteral("proposal_decisions")).toArray();
        if (!proposalDecisions.isEmpty() && m_proposalDecisionsSupport < 0) {
            // Claims go only to a server that reads them (§10.7.1).
            m_uploading = true;
            QNetworkReply *ready = m_client->ready();
            connect(ready, &QNetworkReply::finished, this, [this, ready, operation, operationId, manual] {
                m_uploading = false;
                if (ready->error() != QNetworkReply::NoError) {
                    const Thoughts::SyncErrorInfo error = replyErrorInfo(ready);
                    ready->deleteLater();
                    Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("waiting"), error.summary);
                    if (manual) failManualSync(error.summary, error.details);
                    return;
                }
                m_proposalDecisionsSupport = QJsonDocument::fromJson(ready->readAll()).object().value(QStringLiteral("capabilities")).toArray()
                    .contains(QJsonValue(QLatin1String(Thoughts::ProposalDecisions::Capability))) ? 1 : 0;
                ready->deleteLater();
                uploadSpecialOutboxOperation(operation, manual);
            });
            return;
        }
        QJsonObject requestBody {
            {QStringLiteral("operation_id"), operationId},
            {QStringLiteral("device_sequence"), operation.value(QStringLiteral("device_sequence"))},
            {QStringLiteral("expected_revision"), expectedRevision},
            {QStringLiteral("expected_sha256"), expectedSha256},
            {QStringLiteral("content_sha256"), contentSha256},
            {QStringLiteral("client_timestamp"), operation.value(QStringLiteral("client_timestamp"))},
        };
        if (!proposalDecisions.isEmpty() && m_proposalDecisionsSupport == 1) {
            requestBody.insert(QStringLiteral("proposal_decisions"), proposalDecisions);
        }
        QStringList decisionIds;
        for (const QJsonValue &claim : proposalDecisions) decisionIds.append(claim.toObject().value(QStringLiteral("decision_id")).toString());
        const QByteArray encodedRequest = compactJson(requestBody);
        const QString objectUploadItem = QStringLiteral("source-conflict-object:%1").arg(operationId);
        const QString resolutionUploadItem = QStringLiteral("source-conflict-resolution:%1").arg(operationId);
        if (manual && (!planManualUpload(objectUploadItem, contents.size()) || !planManualUpload(resolutionUploadItem, encodedRequest.size()))) {
            failManualSync(QStringLiteral("The source-conflict resolution has inconsistent transfer accounting."));
            return;
        }

        m_uploading = true;
        QNetworkReply *upload = m_client->uploadObject(contents, QByteArrayLiteral("text/x-tex; charset=utf-8"));
        if (manual) {
            connect(upload, &QNetworkReply::uploadProgress, this, [this, objectUploadItem, size = contents.size()](qint64 sent, qint64) { accountManualUploadProgress(objectUploadItem, qMin(qMax<qint64>(0, sent), size)); });
        }
        connect(upload, &QNetworkReply::finished, this, [this, upload, operation, operationId, conflictId, requestBody, decisionIds, manual, objectUploadItem, resolutionUploadItem, objectSize = contents.size(), requestSize = encodedRequest.size()] {
            m_uploading = false;
            if (upload->error() != QNetworkReply::NoError) {
                if (manual) {
                    accountManualUploadProgress(objectUploadItem, 0, false, true);
                }
                const Thoughts::SyncErrorInfo error = replyErrorInfo(upload);
                upload->deleteLater();
                Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("waiting"), error.summary);
                if (manual) {
                    failManualSync(error.summary, error.details);
                }
                return;
            }
            if (manual) {
                accountManualUploadProgress(objectUploadItem, objectSize, true);
            }
            upload->deleteLater();

            m_uploading = true;
            QNetworkReply *resolve = m_client->resolveConflict(conflictId, requestBody);
            if (manual) {
                connect(resolve, &QNetworkReply::uploadProgress, this, [this, resolutionUploadItem, requestSize](qint64 sent, qint64) { accountManualUploadProgress(resolutionUploadItem, qMin(qMax<qint64>(0, sent), requestSize)); });
            }
            connect(resolve, &QNetworkReply::finished, this, [this, resolve, operationId, conflictId, contentSha256 = requestBody.value(QStringLiteral("content_sha256")).toString(),
                                                              decisionIds, manual, resolutionUploadItem, requestSize] {
                m_uploading = false;
                if (resolve->error() != QNetworkReply::NoError) {
                    if (manual) {
                        accountManualUploadProgress(resolutionUploadItem, 0, false, true);
                    }
                    const Thoughts::SyncErrorInfo error = replyErrorInfo(resolve);
                    const int statusCode = resolve->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    resolve->deleteLater();
                    if (statusCode == 409 || statusCode == 422) {
                        QString acknowledgeError;
                        m_outbox->acknowledge({operationId}, &acknowledgeError);
                        Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("failed"), acknowledgeError.isEmpty() ? error.summary : acknowledgeError);
                    } else {
                        Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("waiting"), error.summary);
                    }
                    if (manual) {
                        failManualSync(error.summary, error.details);
                    }
                    return;
                }
                if (manual) {
                    accountManualUploadProgress(resolutionUploadItem, requestSize, true);
                }
                const QJsonDocument response = QJsonDocument::fromJson(resolve->readAll());
                resolve->deleteLater();
                const QString outcome = response.object().value(QStringLiteral("outcome")).toString();
                if (!response.isObject() || !QSet<QString> {QStringLiteral("applied"), QStringLiteral("merged"), QStringLiteral("duplicate"), QStringLiteral("conflict")}.contains(outcome)) {
                    const QString error = QStringLiteral("The server returned an invalid source-conflict resolution result.");
                    Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("failed"), error);
                    if (manual) {
                        failManualSync(error);
                    }
                    return;
                }
                QString acknowledgeError;
                if (!m_outbox->acknowledge({operationId}, &acknowledgeError)) {
                    Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("failed"), acknowledgeError);
                    if (manual) {
                        failManualSync(acknowledgeError);
                    }
                    return;
                }
                schedulePendingChangesRefresh();
                // Answered: the claims expire (§10.4.2); a new conflict keeps them for its resolution.
                if (outcome != QLatin1String("conflict") && !decisionIds.isEmpty()) {
                    QString claimError;
                    if (!Thoughts::ProposalDecisions::forgetClaims(proposalDecisionDatabase(), decisionIds, &claimError))
                        qWarning().noquote() << "Answered proposal decisions could not be removed:" << claimError;
                }
                if (outcome == QLatin1String("conflict")) {
                    // Fedora superseded the resolved conflict with a new one
                    // (base: its Fedora side, incoming: this resolution); the
                    // old one never resolves, so the file follows the new one
                    // or it could never be resolved or published again.
                    const QJsonObject result = response.object();
                    const QString successorId = result.value(QStringLiteral("conflict_id")).toString();
                    const QString path = result.value(QStringLiteral("path")).toString();
                    const qint64 revision = result.value(QStringLiteral("revision")).toInteger(-1);
                    QString recordError;
                    if (!safeComponent(successorId) || successorId == conflictId || !safeTrackedPath(path) || revision < 0) {
                        recordError = QStringLiteral("The server's answer names no valid successor conflict");
                    } else {
                        Thoughts::Publisher::recordSuccessorSourceConflict(QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")), path, conflictId,
                                                                           successorId, revision, contentSha256, &recordError);
                    }
                    if (!recordError.isEmpty()) {
                        // The publisher follows the successor at its next run.
                        qWarning().noquote() << "The successor of source conflict" << conflictId << "was not recorded:" << recordError;
                    }
                    const QString message = QStringLiteral("The source changed again on the server while the resolution was open. A new guarded conflict was preserved; resolve it again.");
                    Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("conflict"), message);
                    if (manual) {
                        failManualSync(message);
                    }
                    return;
                }
                Q_EMIT connectionStateChanged(true, QString());
                Q_EMIT sourceConflictResolutionStatusChanged(operationId, QStringLiteral("applied"), QString());
                if (manual) {
                    scheduleManualSyncAdvance();
                }
            });
        });
        return;
    }
    QJsonObject requestBody;
    QString undoOf;
    if (kind == QLatin1String("agent_proposal_resolution")) {
        requestBody = operation;
        requestBody.remove(QStringLiteral("outbox_kind"));
    } else if (kind == QLatin1String("agent_proposal_undo")) {
        undoOf = operation.value(QStringLiteral("undo_of")).toString();
        requestBody = QJsonObject {{QStringLiteral("operation_id"), operationId}, {QStringLiteral("device_sequence"), operation.value(QStringLiteral("device_sequence"))}};
    }
    if (requestBody.isEmpty()) {
        Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), QStringLiteral("The durable proposal decision is invalid."));
        if (manual) {
            failManualSync(QStringLiteral("The durable proposal decision is invalid."));
        }
        return;
    }
    const QByteArray encodedRequest = compactJson(requestBody);
    const QString uploadItem = QStringLiteral("proposal-operation:%1").arg(operationId);
    if (manual && !planManualUpload(uploadItem, encodedRequest.size())) {
        failManualSync(QStringLiteral("The proposal decision has inconsistent transfer accounting."));
        return;
    }
    QNetworkReply *reply = kind == QLatin1String("agent_proposal_resolution") ? m_client->resolveAgentProposals(requestBody) : m_client->undoAgentProposalResolution(undoOf, requestBody);
    m_uploading = true;
    if (manual) {
        connect(reply, &QNetworkReply::uploadProgress, this, [this, uploadItem, size = encodedRequest.size()](qint64 sent, qint64) { accountManualUploadProgress(uploadItem, qMin(qMax<qint64>(0, sent), size)); });
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, operationId, manual, uploadItem, size = encodedRequest.size()] {
        m_uploading = false;
        if (reply->error() != QNetworkReply::NoError) {
            if (manual) {
                accountManualUploadProgress(uploadItem, 0, false, true);
            }
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            Q_EMIT connectionStateChanged(false, error.summary);
            if (statusCode == 409 || statusCode == 422) {
                QString acknowledgeError;
                m_outbox->acknowledge({operationId}, &acknowledgeError);
                Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), acknowledgeError.isEmpty() ? error.summary : acknowledgeError);
            } else {
                Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("waiting"), error.summary);
            }
            if (manual) {
                failManualSync(error.summary, error.details);
            }
            return;
        }
        if (manual) {
            accountManualUploadProgress(uploadItem, size, true);
        }
        const QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QString status = response.object().value(QStringLiteral("status")).toString();
        if (!response.isObject() || !QSet<QString> {QStringLiteral("queued"), QStringLiteral("validating"), QStringLiteral("applied"), QStringLiteral("undone")}.contains(status)) {
            const QString responseError = QStringLiteral("The server returned an invalid proposal-resolution result.");
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), responseError);
            if (manual) {
                failManualSync(responseError);
            }
            return;
        }
        if (status == QLatin1String("queued") || status == QLatin1String("validating")) {
            pollAgentProposalResolution(operationId, manual);
            return;
        }
        QString error;
        if (!persistAppliedAgentProposalOperation(operation)) {
            error = QStringLiteral("The confirmed proposal decision could not be saved locally.");
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), error);
            if (manual) {
                failManualSync(error);
            }
            return;
        }
        if (!m_outbox->acknowledge({operationId}, &error)) {
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), error);
            if (manual) {
                failManualSync(error);
            }
            return;
        }
        Q_EMIT connectionStateChanged(true, QString());
        Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("applied"), QString());
        if (manual) {
            scheduleManualSyncAdvance();
        }
    });
}

void FedoraRepository::pollAgentProposalResolution(const QString &operationId, bool manual, int attempt)
{
    if (!m_client || m_uploading) {
        QTimer::singleShot(250, this, [this, operationId, manual, attempt] { pollAgentProposalResolution(operationId, manual, attempt); });
        return;
    }
    m_uploading = true;
    QNetworkReply *reply = m_client->agentProposalResolution(operationId);
    connect(reply, &QNetworkReply::finished, this, [this, reply, operationId, manual, attempt] {
        m_uploading = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("waiting"), error.summary);
            if (manual) {
                failManualSync(error.summary, error.details);
            }
            return;
        }
        const QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QJsonObject record = response.object();
        const QString status = record.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("queued") || status == QLatin1String("validating")) {
            QTimer::singleShot(qMin(2000, 250 * (attempt + 1)), this, [this, operationId, manual, attempt] { pollAgentProposalResolution(operationId, manual, attempt + 1); });
            return;
        }
        if (!response.isObject() || (status != QLatin1String("applied") && status != QLatin1String("undone") && status != QLatin1String("failed"))) {
            const QString responseError = QStringLiteral("The server returned an invalid proposal-resolution status.");
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), responseError);
            if (manual) {
                failManualSync(responseError);
            }
            return;
        }
        if (status == QLatin1String("failed")) {
            QString responseError =
                record.value(QStringLiteral("error")).toString().trimmed().isEmpty() ? QStringLiteral("The server rejected the proposal decision without an error detail.") : record.value(QStringLiteral("error")).toString().trimmed();
            QString acknowledgeError;
            if (!m_outbox->acknowledge({operationId}, &acknowledgeError)) {
                responseError = acknowledgeError.isEmpty() ? QStringLiteral("%1 The terminal operation could not be removed from the durable queue.").arg(responseError) : QStringLiteral("%1 %2").arg(responseError, acknowledgeError);
            }
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), responseError);
            if (manual) {
                failManualSync(responseError);
            }
            return;
        }
        QString error;
        const QList<QJsonObject> pendingOperations = m_outbox->operations(&error);
        const auto pending = std::find_if(pendingOperations.cbegin(), pendingOperations.cend(), [&operationId](const QJsonObject &operation) { return operation.value(QStringLiteral("operation_id")).toString() == operationId; });
        if (pending == pendingOperations.cend() || !persistAppliedAgentProposalOperation(*pending)) {
            error = error.isEmpty() ? QStringLiteral("The confirmed proposal decision could not be saved locally.") : error;
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), error);
            if (manual) {
                failManualSync(error);
            }
            return;
        }
        if (!m_outbox->acknowledge({operationId}, &error)) {
            Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("failed"), error);
            return;
        }
        Q_EMIT connectionStateChanged(true, QString());
        Q_EMIT agentProposalResolutionStatusChanged(operationId, QStringLiteral("applied"), QString());
        if (manual) {
            scheduleManualSyncAdvance();
        }
    });
}

void FedoraRepository::pullManualProjectionSnapshot()
{
    if (!m_client || m_libraryReaderPlan.isEmpty() || !m_libraryPulling) {
        failManualSync(QStringLiteral("The server connection is unavailable."));
        return;
    }
    m_pulling = true;
    retryingRequest([this] { return m_client->projections(); }, [this](QNetworkReply *reply) {
        m_pulling = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            Q_EMIT connectionStateChanged(false, error.summary);
            failManualSync(error.summary, error.details);
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument response = QJsonDocument::fromJson(body);
        reply->deleteLater();
        if (!response.isObject() || !accountReaderMetadataBytes(body.size()) ||
            response.object().value(QStringLiteral("snapshot_sequence")).toInteger(-1) != m_libraryReaderPlan.value(QStringLiteral("latest_server_sequence")).toInteger(-2) || !applyProjectionSnapshot(response.object())) {
            supersedeGlobalReaderPlan(QStringLiteral("The projection snapshot diverged from its approved reader plan."));
            return;
        }
        if (m_libraryReaderPlan.value(QStringLiteral("_desktop_library_required")).toBool()) {
            pullLibraryWithReaderPlan();
        } else {
            finishGlobalReaderMetadata(readRepositoryState().value(QStringLiteral("library")).toObject());
        }
    });
}

void FedoraRepository::pullManualChangePage()
{
    if (!m_client || m_libraryReaderPlan.isEmpty() || !m_libraryPulling) {
        failManualSync(QStringLiteral("The server connection is unavailable."));
        return;
    }
    const qint64 cursor = m_outbox->cursor();
    m_pulling = true;
    retryingRequest([this, cursor] { return m_client->changes(cursor); }, [this, cursor](QNetworkReply *reply) {
        m_pulling = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            Q_EMIT connectionStateChanged(false, error.summary);
            failManualSync(error.summary, error.details);
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument response = QJsonDocument::fromJson(body);
        reply->deleteLater();
        const QJsonObject page = response.object();
        const qint64 nextCursor = page.value(QStringLiteral("next_cursor")).toInteger(-1);
        const qint64 plannedLatest = m_libraryReaderPlan.value(QStringLiteral("latest_server_sequence")).toInteger(-2);
        if (!response.isObject() || !accountReaderMetadataBytes(body.size()) || nextCursor < cursor || nextCursor > plannedLatest || (!page.value(QStringLiteral("has_more")).toBool() && nextCursor != plannedLatest) ||
            !applyChangePage(page, cursor)) {
            supersedeGlobalReaderPlan(QStringLiteral("The change page diverged from its approved reader plan."));
            return;
        }
        if (page.value(QStringLiteral("has_more")).toBool()) {
            QTimer::singleShot(0, this, &FedoraRepository::pullManualChangePage);
            return;
        }
        if (m_libraryReaderPlan.value(QStringLiteral("_desktop_library_required")).toBool()) {
            pullLibraryWithReaderPlan();
        } else {
            finishGlobalReaderMetadata(readRepositoryState().value(QStringLiteral("library")).toObject());
        }
    });
}

void FedoraRepository::pullPlanlessManualChangePage()
{
    if (!m_client || m_pulling || !m_outbox || !m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::RemoteChanges) {
        return;
    }
    const qint64 cursor = m_outbox->cursor();
    const qint64 expectedLatest = m_syncCoordinator.remoteLatestServerSequence;
    if (cursor < 0 || expectedLatest <= cursor) {
        failManualSync(QStringLiteral("The remote change frontier is invalid."));
        return;
    }
    m_pulling = true;
    retryingRequest([this, cursor] { return m_client->changes(cursor); }, [this, cursor, expectedLatest](QNetworkReply *reply) {
        m_pulling = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            Q_EMIT connectionStateChanged(false, error.summary);
            failManualSync(error.summary, error.details);
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument response = QJsonDocument::fromJson(body);
        reply->deleteLater();
        const QJsonObject page = response.object();
        const qint64 nextCursor = page.value(QStringLiteral("next_cursor")).toInteger(-1);
        const bool hasMore = page.value(QStringLiteral("has_more")).toBool();
        const bool frontierValid = nextCursor >= cursor && nextCursor <= expectedLatest && (!hasMore || nextCursor > cursor) && (hasMore || nextCursor == expectedLatest);
        if (!response.isObject() || !page.value(QStringLiteral("changes")).isArray() || !frontierValid || !applyChangePage(page, cursor)) {
            failManualSync(QStringLiteral("The server returned remote changes outside the checked frontier."));
            return;
        }
        const QString transferItem = QStringLiteral("remote-changes:%1:%2").arg(cursor).arg(nextCursor);
        if (!commitManualTransferTotals(transferItem, 0, body.size())) {
            failManualSync(QStringLiteral("The remote change transfer could not be recorded accurately."));
            return;
        }
        if (hasMore) {
            scheduleManualSyncAdvance();
            return;
        }
        m_syncCoordinator.remoteLatestServerSequence = -1;
        m_syncCoordinator.postLocalWorkAction = SyncCoordinator::PostLocalWorkAction::ConditionalRenderCheck;
        Q_EMIT connectionStateChanged(true, QString());
        beginManualReaderStateRecheck();
    });
}

void FedoraRepository::startManualRenderCheck()
{
    if (!m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::RenderCheck || !m_syncCoordinator.conditionalRenderCheck || m_pulling) {
        return;
    }
    if (!m_client) {
        failManualSync(QStringLiteral("The server connection is unavailable."));
        return;
    }
    const QString target = m_syncCoordinator.conditionalRenderTarget();
    if (target != QLatin1String("all") && !safePdfFilename(target)) {
        failManualSync(QStringLiteral("The conditional PDF update target is invalid."));
        return;
    }
    if (m_syncCoordinator.renderCheckRequestId.isEmpty()) {
        m_syncCoordinator.renderCheckRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    emitSyncProgress(QStringLiteral("checking"), 0, 0, QStringLiteral("Checking whether the server needs to update any PDFs"));
    m_pulling = true;
    retryingRequest([this, target] { return m_client->renderCheck(target, m_syncCoordinator.renderCheckRequestId); }, [this](QNetworkReply *reply) {
        m_pulling = false;
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            Q_EMIT connectionStateChanged(false, error.summary);
            failManualSync(error.summary, error.details);
            return;
        }
        const QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!response.isObject()) {
            failManualSync(QStringLiteral("The server returned an invalid conditional render result."));
            return;
        }
        applyManualRenderCheckResponse(response.object());
    });
}

bool FedoraRepository::applyManualRenderCheckResponse(const QJsonObject &response)
{
    if (!m_syncCoordinator.running() || m_syncCoordinator.phase != ManualSyncPhase::RenderCheck) {
        return false;
    }
    const QString status = response.value(QStringLiteral("status")).toString();
    const qint64 libraryRevision = response.value(QStringLiteral("library_revision")).toInteger(-1);
    if (libraryRevision < 0 || !response.value(QStringLiteral("documents")).isArray() || (status != QLatin1String("current") && status != QLatin1String("building") && status != QLatin1String("attention"))) {
        failManualSync(QStringLiteral("The server returned an invalid conditional render result."));
        return false;
    }

    const QString checkTarget = m_syncCoordinator.conditionalRenderTarget();
    const QJsonValue buildValue = response.value(QStringLiteral("build"));
    if (!buildValue.isUndefined() && !buildValue.isNull() && !buildValue.isObject()) {
        failManualSync(QStringLiteral("The server returned an invalid conditional build identity."));
        return false;
    }
    const QJsonObject build = buildValue.toObject();
    const QString buildId = build.value(QStringLiteral("id")).toString();
    const QString buildTarget = build.value(QStringLiteral("target")).toString();
    const QString buildStatus = build.value(QStringLiteral("status")).toString();
    const bool validBuildStatus = buildStatus == QLatin1String("queued") || buildStatus == QLatin1String("running") || buildStatus == QLatin1String("succeeded") || buildStatus == QLatin1String("attention") ||
        buildStatus == QLatin1String("failed") || buildStatus == QLatin1String("cancelled");
    const bool validBuildTarget = buildTarget == QLatin1String("all") || (checkTarget == QLatin1String("all") ? safePdfFilename(buildTarget) : buildTarget == checkTarget);
    if (!build.isEmpty() && (!safeComponent(buildId) || !validBuildTarget || !validBuildStatus)) {
        failManualSync(QStringLiteral("The server returned an invalid conditional build identity."));
        return false;
    }
    m_syncCoordinator.renderCheckedSourceFrontier = libraryRevision;
    // A Sync of the whole Library asked about every PDF: what it saw replaces the builds an
    // earlier Sync left running. A Sync of one PDF keeps waiting for that PDF's build.
    const bool wholeLibrary = !m_syncCoordinator.isDocument();
    QStringList buildingDocuments;
    for (const QJsonValue &value : response.value(QStringLiteral("documents")).toArray()) {
        const QJsonObject document = value.toObject();
        const QString state = document.value(QStringLiteral("state")).toString();
        const QString filename = document.value(QStringLiteral("filename")).toString();
        if ((state == QLatin1String("queued") || state == QLatin1String("building")) && safePdfFilename(filename) && !buildingDocuments.contains(filename)) {
            buildingDocuments.append(filename);
        }
    }

    const auto failureMessage = [&response](const QJsonObject &build) {
        const QString buildError = build.value(QStringLiteral("error")).toString().trimmed();
        const QJsonArray failedDocuments = build.value(QStringLiteral("result")).toObject().value(QStringLiteral("failed_documents")).toArray();
        const auto documentFailure = [&failedDocuments](const QString &filename) {
            for (const QJsonValue &value : failedDocuments) {
                const QJsonObject failure = value.toObject();
                if (failure.value(QStringLiteral("pdf_filename")).toString(failure.value(QStringLiteral("filename")).toString()) == filename) {
                    return failure.value(QStringLiteral("message")).toString(failure.value(QStringLiteral("error")).toString()).trimmed();
                }
            }
            return QString();
        };
        // Report the PDFs that need attention now, each with its own failure.
        // The build in the response is the newest one of those PDFs; it can
        // also list failures of other PDFs that a later build has fixed, so its
        // first failure may name a PDF that is current (2026-10-14).
        QStringList messages;
        for (const QJsonValue &value : response.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = value.toObject();
            if (document.value(QStringLiteral("state")).toString() != QLatin1String("attention")) {
                continue;
            }
            const QString filename = document.value(QStringLiteral("filename")).toString().trimmed();
            QString message = documentFailure(filename);
            if (message.isEmpty()) {
                message = buildError;
            }
            if (message.isEmpty()) {
                const QString reason = document.value(QStringLiteral("reason")).toString().trimmed();
                message = filename.isEmpty() || reason.isEmpty() ? reason : QStringLiteral("%1: %2").arg(filename, reason);
            }
            if (!message.isEmpty() && !messages.contains(message)) {
                messages.append(message);
            }
        }
        QString message = messages.join(QLatin1Char('\n'));
        if (message.isEmpty()) {
            message = buildError;
        }
        if (message.isEmpty() && !failedDocuments.isEmpty()) {
            const QJsonObject failure = failedDocuments.first().toObject();
            message = failure.value(QStringLiteral("message")).toString(failure.value(QStringLiteral("error")).toString()).trimmed();
        }
        return message.isEmpty() ? QStringLiteral("The server could not update the required PDFs; the last-good Library PDFs remain active.") : message;
    };
    // The PDFs that need attention, for the Sync summary: a failed build is a warning of a
    // Sync whose changes arrived, and the Library status may not list it yet.
    const auto failedDocuments = [&response](const QJsonObject &build) {
        QJsonArray failed;
        for (const QJsonValue &value : response.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = value.toObject();
            const QString filename = document.value(QStringLiteral("filename")).toString();
            if (document.value(QStringLiteral("state")).toString() != QLatin1String("attention") || !safePdfFilename(filename)) {
                continue;
            }
            failed.append(QJsonObject {{QStringLiteral("filename"), filename},
                                       {QStringLiteral("reason"), document.value(QStringLiteral("reason")).toString().trimmed()},
                                       {QStringLiteral("build_id"), build.value(QStringLiteral("id")).toString()}});
        }
        return failed;
    };

    if (status == QLatin1String("current")) {
        if (wholeLibrary) {
            recordPendingServerBuild({}, {}, true);
        }
        m_syncCoordinator.buildFailure.clear();
        if (safePdfFilename(m_syncCoordinator.target) && m_restoredTransientRenderStatuses.contains(m_syncCoordinator.target)) {
            QJsonObject refreshed = m_renderStatuses.value(m_syncCoordinator.target);
            refreshed.insert(QStringLiteral("status"), QStringLiteral("synced"));
            refreshed.insert(QStringLiteral("reason"), QStringLiteral("The server reports that this PDF needs no rendered update."));
            refreshed.insert(QStringLiteral("checked_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            refreshed.insert(QStringLiteral("checking"), false);
            refreshed.insert(QStringLiteral("refreshed"), true);
            refreshed.remove(QStringLiteral("refresh_error"));
            const QJsonObject publication = refreshed.value(QStringLiteral("publication")).toObject();
            if (!publication.isEmpty()) {
                refreshed.insert(QStringLiteral("local_state"), localPublicationState(m_syncCoordinator.target, publication));
            }
            storeRenderStatus(m_syncCoordinator.target, refreshed);
        }
        emitSyncProgress(QStringLiteral("checking"), 0, 0, QStringLiteral("No PDF update is needed"));
        resumeManualSyncAfterRenderCheck();
        return true;
    }

    if (status == QLatin1String("attention")) {
        if (wholeLibrary) {
            recordPendingServerBuild({}, {}, true);
        }
        deferManualBuildFailure(build, failureMessage(build), failedDocuments(build));
        return true;
    }

    if (build.isEmpty()) {
        failManualSync(QStringLiteral("The server returned an invalid conditional build identity."));
        return false;
    }
    if (buildStatus == QLatin1String("succeeded")) {
        if (wholeLibrary) {
            recordPendingServerBuild({}, {}, true);
        }
        m_syncCoordinator.buildFailure.clear();
        emitSyncProgress(QStringLiteral("building"), 0, 0, QStringLiteral("The server already completed the required PDF update"));
        resumeManualSyncAfterRenderCheck();
        return true;
    }
    if (buildStatus == QLatin1String("attention") || buildStatus == QLatin1String("failed") || buildStatus == QLatin1String("cancelled")) {
        if (wholeLibrary) {
            recordPendingServerBuild({}, {}, true);
        }
        deferManualBuildFailure(build, failureMessage(build), failedDocuments(build));
        return true;
    }

    if (wholeLibrary) {
        // The server is still building. Everything else this Sync exchanges does not depend
        // on that build, so it goes on and ends without the new PDF: the PDF this computer
        // has stays, and the follow-up Sync that watchPendingServerBuilds announces fetches
        // the new one. (13 November 2026: the median Sync waited 30 s here.)
        if (buildingDocuments.isEmpty() && safePdfFilename(buildTarget)) {
            buildingDocuments.append(buildTarget);
        }
        recordPendingServerBuild(build, buildingDocuments, true);
        m_syncCoordinator.pendingBuildDocuments = buildingDocuments;
        m_syncCoordinator.buildFailure.clear();
        QHash<QString, QJsonObject> buildingStatuses;
        for (const QString &filename : std::as_const(buildingDocuments)) {
            QJsonObject building = m_renderStatuses.value(filename);
            building.insert(QStringLiteral("status"), QStringLiteral("building"));
            building.insert(QStringLiteral("reason"), QStringLiteral("The server is updating this PDF. The PDF on this computer stays until the new one arrives."));
            building.insert(QStringLiteral("build"), build);
            building.insert(QStringLiteral("checking"), false);
            building.remove(QStringLiteral("refresh_error"));
            buildingStatuses.insert(filename, building);
        }
        storeRenderStatuses(buildingStatuses);
        const QString pendingMessage = buildingDocuments.size() == 1
            ? QStringLiteral("The server is still updating 1 PDF; Sync continues without waiting for it.")
            : QStringLiteral("The server is still updating %1 PDFs; Sync continues without waiting for them.").arg(qMax<qsizetype>(buildingDocuments.size(), 1));
        emitSyncProgress(QStringLiteral("checking"), 0, 0, pendingMessage);
        if (!m_activeTraceId.isEmpty()) {
            Q_EMIT syncJournalEvent(m_activeTraceId,
                                    QJsonObject {{QStringLiteral("at_ms"), QDateTime::currentMSecsSinceEpoch()},
                                                 {QStringLiteral("phase"), QStringLiteral("checking")},
                                                 {QStringLiteral("state"), QStringLiteral("running")},
                                                 {QStringLiteral("message"), pendingMessage},
                                                 {QStringLiteral("build_id"), buildId},
                                                 {QStringLiteral("target"), buildTarget},
                                                 {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("pending_build_documents"), QJsonArray::fromStringList(buildingDocuments)}}}});
        }
        resumeManualSyncAfterRenderCheck();
        return true;
    }

    const QString trackingTarget = buildTarget;
    QJsonObject state = readRepositoryState();
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    const QJsonObject existingRun = runs.value(trackingTarget).toObject();
    if (!existingRun.isEmpty() && activeBuildStatus(existingRun.value(QStringLiteral("status")).toString()) && existingRun.value(QStringLiteral("sync_origin")).toString() != QLatin1String("manual_conditional") &&
        existingRun.value(QStringLiteral("build_id")).toString() != buildId) {
        failManualSync(QStringLiteral("Another server build already owns this PDF target."));
        return false;
    }
    QJsonArray activeBuilds;
    for (const QJsonValue &value : state.value(QStringLiteral("active_builds")).toArray()) {
        const QJsonObject active = value.toObject();
        if (active.value(QStringLiteral("id")).toString() != buildId && active.value(QStringLiteral("target")).toString() != trackingTarget) {
            activeBuilds.append(active);
        }
    }
    activeBuilds.append(QJsonObject {{QStringLiteral("id"), buildId}, {QStringLiteral("target"), trackingTarget}});
    state.insert(QStringLiteral("active_builds"), activeBuilds);
    const QString runId = m_syncCoordinator.renderCheckRequestId;
    QJsonObject run {{QStringLiteral("run_id"), runId},
                     {QStringLiteral("trace_id"), m_activeTraceId},
                     {QStringLiteral("build_trace_id"), build.value(QStringLiteral("trace_id")).toString()},
                     {QStringLiteral("request_id"), m_syncCoordinator.renderCheckRequestId},
                     {QStringLiteral("target"), trackingTarget},
                     {QStringLiteral("sync_scope_target"), checkTarget},
                     {QStringLiteral("status"), buildStatus},
                     {QStringLiteral("build_id"), buildId},
                     {QStringLiteral("sync_origin"), QStringLiteral("manual_conditional")},
                     {QStringLiteral("started_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    runs.insert(trackingTarget, run);
    state.insert(QStringLiteral("build_runs"), runs);
    if (!writeRepositoryState(state)) {
        failManualSync(QStringLiteral("The conditional server build could not be journaled safely."));
        return false;
    }

    m_syncCoordinator.phase = ManualSyncPhase::WaitBuild;
    if (safePdfFilename(trackingTarget)) {
        m_targetedBuildTarget = trackingTarget;
        storeRenderStatus(
            trackingTarget,
            QJsonObject {{QStringLiteral("status"), QStringLiteral("building")}, {QStringLiteral("reason"), QStringLiteral("The server is updating this PDF because its rendered inputs changed.")}, {QStringLiteral("build"), build}});
    }
    emitSyncProgress(QStringLiteral("building"), 0, 0, QStringLiteral("Waiting for the PDF update on the server"));
    if (!m_activeTraceId.isEmpty()) {
        Q_EMIT syncJournalEvent(m_activeTraceId,
                                QJsonObject {{QStringLiteral("at_ms"), QDateTime::currentMSecsSinceEpoch()},
                                             {QStringLiteral("phase"), QStringLiteral("building")},
                                             {QStringLiteral("state"), QStringLiteral("running")},
                                             {QStringLiteral("message"), QStringLiteral("The server accepted the conditional PDF update.")},
                                             {QStringLiteral("build_id"), buildId},
                                             {QStringLiteral("target"), trackingTarget}});
    }
    pollBuilds();
    return true;
}

void FedoraRepository::resumeManualSyncAfterRenderCheck()
{
    if (!m_syncCoordinator.running()) {
        return;
    }
    m_syncCoordinator.libraryLatestServerSequence = -1;
    beginManualReaderStateRecheck();
}

bool FedoraRepository::manualConditionalBuildOwnsCurrentSync(const QJsonObject &run) const
{
    const QString expectedScope = m_syncCoordinator.conditionalRenderTarget();
    const QString recordedScope = run.value(QStringLiteral("sync_scope_target")).toString(run.value(QStringLiteral("target")).toString());
    return m_syncCoordinator.running() && m_syncCoordinator.phase == ManualSyncPhase::WaitBuild && run.value(QStringLiteral("sync_origin")).toString() == QLatin1String("manual_conditional") && recordedScope == expectedScope;
}

bool FedoraRepository::resumeManualSyncAfterConditionalBuild(const QJsonObject &run)
{
    if (!manualConditionalBuildOwnsCurrentSync(run)) {
        return false;
    }
    resumeManualSyncAfterRenderCheck();
    return true;
}

bool FedoraRepository::applyManualConditionalBuildTerminalResult(const QJsonObject &run, const QString &target, const QJsonObject &build, bool succeeded, const QString &message, bool journaled)
{
    if (run.value(QStringLiteral("sync_origin")).toString() != QLatin1String("manual_conditional")) {
        return false;
    }
    m_targetedBuildTarget.clear();
    const bool ownsManualSync = manualConditionalBuildOwnsCurrentSync(run);
    if (!journaled) {
        if (ownsManualSync) {
            failManualSync(QStringLiteral("The completed conditional server build could not be journaled safely."));
        }
        return true;
    }
    if (succeeded) {
        Q_EMIT connectionStateChanged(true, QString());
        if (ownsManualSync) {
            m_syncCoordinator.buildFailure.clear();
            resumeManualSyncAfterConditionalBuild(run);
        }
        return true;
    }

    const QString status = build.value(QStringLiteral("status")).toString();
    if (safePdfFilename(target)) {
        storeRenderStatus(target,
                          QJsonObject {{QStringLiteral("status"), status == QLatin1String("attention") ? QStringLiteral("attention") : QStringLiteral("error")}, {QStringLiteral("reason"), message}, {QStringLiteral("build"), build}});
    }
    if (ownsManualSync) {
        deferManualBuildFailure(build, message);
    }
    return true;
}

void FedoraRepository::deferManualBuildFailure(const QJsonObject &build, const QString &message, const QJsonArray &failedDocuments)
{
    m_syncCoordinator.buildFailure = message;
    m_syncCoordinator.buildFailureDocuments = failedDocuments;
    Q_EMIT syncJournalEvent(m_activeTraceId,
                            QJsonObject {{QStringLiteral("phase"), QStringLiteral("downloading")},
                                         {QStringLiteral("state"), QStringLiteral("running")},
                                         {QStringLiteral("build_id"), build.value(QStringLiteral("id"))},
                                         {QStringLiteral("message"), message},
                                         {QStringLiteral("error_code"), QStringLiteral("pdf_build_pending")},
                                         {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("build"), build}}}});
    resumeManualSyncAfterRenderCheck();
}

void FedoraRepository::failManualSync(const QString &message, const QString &details, const QString &kind)
{
    // The failure of the request that was just read, unless the caller knows better.
    ReplyFailureNote &note = lastReplyFailure();
    const QString replyKind = note.age.isValid() && note.age.elapsed() < 100 ? note.kind : QString();
    note.kind.clear();
    note.age.invalidate();
    if (!m_syncCoordinator.running()) {
        return;
    }
    m_syncCoordinator.fail(message, details);
    m_syncCoordinator.failureKind = !kind.isEmpty() ? kind : !replyKind.isEmpty() ? replyKind : QStringLiteral("bug");
    emitReaderTransferSnapshot(true);
    Q_EMIT manualSyncStateChanged(m_syncCoordinator.state, m_syncCoordinator.message, m_syncCoordinator.details);
    QTimer::singleShot(0, this, &FedoraRepository::watchPendingServerBuilds);
    if (m_pendingBuildAnnouncementDue) {
        QTimer::singleShot(0, this, [this] { announceRemoteWorkAfterPendingBuild(); });
    }
}

void FedoraRepository::completeManualSync(bool skipRenderStateRefresh)
{
    if (!m_syncCoordinator.running()) {
        return;
    }
    const qint64 expectedAuthoring = std::max({m_syncCoordinator.publishedSourceFrontier,
                                              m_syncCoordinator.renderCheckedSourceFrontier,
                                              m_syncCoordinator.serverState.value(QStringLiteral("source_frontier")).toInteger(-1)});
    if (expectedAuthoring >= 0 && m_syncCoordinator.authoringCheckedSourceFrontier < expectedAuthoring) {
        beginManualReaderStateRecheck();
        return;
    }
    if (!m_syncCoordinator.isDocument() && !m_libraryReaderPlan.isEmpty()) {
        markGlobalReaderCacheCurrent();
    }
    emitReaderTransferSnapshot(true);
    emitEffectiveProjections();
    persistManualSyncFrontier();
    if (!m_activeTraceId.isEmpty()) {
        // Preserve known PDF failures in this run, including the no-transfer
        // completion path, so the saved outcome agrees with the Library status.
        QJsonArray failedDocuments;
        for (auto it = m_renderStatuses.cbegin(); it != m_renderStatuses.cend(); ++it) {
            if (m_syncCoordinator.isDocument() && it.key() != m_syncCoordinator.target) continue;
            const QString status = it.value().value(QStringLiteral("status")).toString();
            if (status != QLatin1String("error") && status != QLatin1String("attention")) continue;
            failedDocuments.append(QJsonObject {{QStringLiteral("filename"), it.key()},
                {QStringLiteral("reason"), it.value().value(QStringLiteral("reason"))},
                {QStringLiteral("build_id"), it.value().value(QStringLiteral("build_id")).toString(it.value().value(QStringLiteral("build")).toObject().value(QStringLiteral("id")).toString())}});
        }
        // The PDFs this Sync's own check found failed: the Library status above is refreshed
        // only after the Sync and may not name them yet.
        for (const QJsonValue &value : std::as_const(m_syncCoordinator.buildFailureDocuments)) {
            const QString filename = value.toObject().value(QStringLiteral("filename")).toString();
            const bool listed = std::any_of(failedDocuments.cbegin(), failedDocuments.cend(), [&filename](const QJsonValue &known) {
                return known.toObject().value(QStringLiteral("filename")).toString() == filename;
            });
            if (!listed && (!m_syncCoordinator.isDocument() || filename == m_syncCoordinator.target)) {
                failedDocuments.append(value);
            }
        }
        const bool allChangesSynced = m_syncCoordinator.blockedAuthoringPaths.isEmpty() && m_syncCoordinator.providerErrors.isEmpty();
        emitReaderStepTiming(QStringLiteral("done"));
        m_readerPhaseClock.invalidate();
        QJsonObject completion {{QStringLiteral("failed_documents"), failedDocuments},
                                {QStringLiteral("sync_outcome"), allChangesSynced ? QStringLiteral("changes_synced") : QStringLiteral("available_updates_synced")}};
        if (!m_syncCoordinator.pendingBuildDocuments.isEmpty()) {
            completion.insert(QStringLiteral("pending_build_documents"), QJsonArray::fromStringList(m_syncCoordinator.pendingBuildDocuments));
        }
        Q_EMIT syncJournalEvent(m_activeTraceId,
            QJsonObject {{QStringLiteral("phase"), QStringLiteral("downloading_updates")},
                         {QStringLiteral("state"), QStringLiteral("running")},
                         {QStringLiteral("message"), QStringLiteral("Available updates synchronized with the server.")},
                         {QStringLiteral("metadata"), completion}});
    }
    const bool refreshRenderState = !m_syncCoordinator.isDocument() && !skipRenderStateRefresh;
    if (!m_syncCoordinator.blockedAuthoringPaths.isEmpty() || !m_syncCoordinator.providerErrors.isEmpty()) {
        const QStringList problems = m_syncCoordinator.blockedAuthoringPaths + m_syncCoordinator.providerErrors;
        failManualSync(QStringLiteral("Available updates were synchronized; some documents need attention."), problems.join(QLatin1Char('\n')), QStringLiteral("needs_user"));
        return;
    }
    // A PDF the server could not build does not undo a Sync whose changes arrived: the Sync
    // succeeded, with a warning; the document itself reports the failed build.
    const QString buildFailure = m_syncCoordinator.buildFailure;
    m_syncCoordinator.succeed();
    if (!buildFailure.isEmpty()) {
        m_syncCoordinator.warningCode = QStringLiteral("pdf_build_failed");
        m_syncCoordinator.warning = Thoughts::SyncStatusLabels::pdfBuildFailed() + QStringLiteral("\nYour changes are synced with the server. The last successful PDF remains available.\n") + buildFailure;
    }
    Q_EMIT manualSyncStateChanged(m_syncCoordinator.state, QString(), QString());
    // An earlier Sync's failure may have marked the PDF; a skipped refresh would keep that.
    if (refreshRenderState || (!buildFailure.isEmpty() && !m_syncCoordinator.isDocument())) {
        refreshRenderStateAfterExplicitOperation();
    }
    QTimer::singleShot(0, this, &FedoraRepository::watchPendingServerBuilds);
    if (m_pendingBuildAnnouncementDue) {
        QTimer::singleShot(0, this, [this] { announceRemoteWorkAfterPendingBuild(); });
    }
}

void FedoraRepository::markGlobalReaderCacheCurrent()
{
    const QString revision = m_libraryReaderPlan.value(QStringLiteral("library_revision")).toString();
    if (!validSha256(revision) || readRepositoryState().value(QStringLiteral("active_library_revision")).toString() != revision) {
        return;
    }
    QSet<QString> identities;
    for (const QJsonValue &value : m_libraryReaderPlan.value(QStringLiteral("_desktop_known_objects")).toArray()) {
        if (validSha256(value.toString())) {
            identities.insert(value.toString());
        }
    }
    for (const QJsonValue &value : m_libraryReaderPlan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        if (plannedObjectHasRole(object, QStringLiteral("pdf_delta"))) {
            continue;
        }
        const QString identity = object.value(QStringLiteral("sha256")).toString();
        if (validSha256(identity)) {
            identities.insert(identity);
        }
    }
    for (const QJsonValue &value : readRepositoryState().value(QStringLiteral("library")).toObject().value(QStringLiteral("publications")).toArray()) {
        const QString identity = value.toObject().value(QStringLiteral("sha256")).toString();
        if (validSha256(identity)) {
            identities.insert(identity);
        }
    }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    if (!store || !error.isEmpty()) {
        return;
    }
    QSet<QString> planned;
    for (const QJsonValue &value : m_libraryReaderPlan.value(QStringLiteral("objects")).toArray()) {
        const QJsonObject object = value.toObject();
        if (!plannedObjectHasRole(object, QStringLiteral("pdf_delta"))) {
            planned.insert(object.value(QStringLiteral("sha256")).toString());
        }
    }
    QStringList ordered = identities.values();
    std::sort(ordered.begin(), ordered.end());
    QStringList retained;
    const QHash<QString, Thoughts::DesktopVerifiedObject> receipts = store->verifiedObjects(ordered, &error);
    if (!error.isEmpty()) {
        return;
    }
    for (const QString &identity : std::as_const(ordered)) {
        const auto found = receipts.constFind(identity);
        const std::optional<Thoughts::DesktopVerifiedObject> object = found == receipts.constEnd() ? std::nullopt : std::optional(*found);
        if (!object || !error.isEmpty() || !sameReaderFileMetadata(object->metadata, readerFileMetadata(object->metadata.value(QStringLiteral("local_path")).toString()))) {
            if (planned.contains(identity)) {
                if (!m_activeTraceId.isEmpty()) {
                    Q_EMIT syncJournalEvent(m_activeTraceId, QJsonObject {
                        {QStringLiteral("phase"), QStringLiteral("checking_cache")},
                        {QStringLiteral("state"), QStringLiteral("succeeded")},
                        {QStringLiteral("message"), QStringLiteral("Reader cache checkpoint deferred: a planned object has no current file receipt.")},
                        {QStringLiteral("component"), QStringLiteral("reader_cache")},
                        {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("reason"), QStringLiteral("missing_current_object_receipt")},
                            {QStringLiteral("sha256"), identity}, {QStringLiteral("local_path"), object ? object->metadata.value(QStringLiteral("local_path")) : QJsonValue()}}}});
                }
                return;
            }
            error.clear();
            continue;
        }
        retained.append(identity);
    }
    ordered = retained;
    Thoughts::DesktopSyncFrontier frontier = store->frontier(&error);
    if (!error.isEmpty()) {
        return;
    }
    QJsonArray objects;
    for (const QString &identity : std::as_const(ordered)) {
        objects.append(identity);
    }
    frontier.metadata.insert(QStringLiteral("reader_cache_revision"), revision);
    frontier.metadata.insert(QStringLiteral("reader_cache_objects"), objects);
    store->setFrontier(frontier, &error);
}

void FedoraRepository::refreshLibrary()
{
    if (!m_client || m_libraryPulling || m_libraryPreparing || m_libraryActivating || m_projectingPdf) {
        return;
    }
    m_libraryPulling = true;
    requestReaderSyncPlan(QString(), [this](const QJsonObject &plan, const QString &error) {
        if (plan.isEmpty()) {
            m_libraryPulling = false;
            Q_EMIT connectionStateChanged(false, error.isEmpty() ? QStringLiteral("The reader synchronization plan could not be prepared.") : error);
            if (m_syncCoordinator.running()) {
                failManualSync(QStringLiteral("The reader synchronization plan could not be prepared."), error);
            }
            return;
        }
        m_libraryReaderPlan = plan;
        initializeReaderPlanTransfers(plan);
        emitSyncProgress(QStringLiteral("downloading_updates"), 0, 0, QStringLiteral("Downloading updated PDFs and source data"));
        m_readerPhaseClock.start();
        startGlobalReaderMetadata();
    });
}

void FedoraRepository::startGlobalReaderMetadata()
{
    if (m_libraryReaderPlan.isEmpty() || !m_libraryPulling) {
        return;
    }
    const qint64 plannedCursor = m_libraryReaderPlan.value(QStringLiteral("_desktop_after_sequence")).toInteger(-1);
    if (!m_outbox || plannedCursor < 0 || m_outbox->cursor() != plannedCursor) {
        supersedeGlobalReaderPlan(QStringLiteral("The local change cursor moved before the approved reader metadata could be applied."));
        return;
    }
    if (m_libraryReaderPlan.value(QStringLiteral("_desktop_projection_bootstrapped")).toBool()) {
        m_syncCoordinator.phase = ManualSyncPhase::ChangePages;
        pullManualChangePage();
    } else {
        m_syncCoordinator.phase = ManualSyncPhase::ProjectionSnapshot;
        pullManualProjectionSnapshot();
    }
}

void FedoraRepository::pullLibraryWithReaderPlan()
{
    if (!m_client || !m_libraryPulling || m_libraryReaderPlan.isEmpty()) {
        return;
    }
    emitReaderStepTiming(QStringLiteral("change_pages"));
    retryingRequest([this] { return m_client->library(); }, [this](QNetworkReply *reply) {
        emitReaderStepTiming(QStringLiteral("library_received"));
        if (reply->error() != QNetworkReply::NoError) {
            m_libraryPulling = false;
            Q_EMIT connectionStateChanged(false, reply->errorString());
            reply->deleteLater();
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject library = document.object();
        reply->deleteLater();
        if (!document.isObject() || !accountReaderMetadataBytes(body.size()) || library.value(QStringLiteral("library_revision")).toString() != m_libraryReaderPlan.value(QStringLiteral("library_revision")).toString()) {
            supersedeGlobalReaderPlan(QStringLiteral("The Library response diverged from its approved reader plan."));
            return;
        }
        finishGlobalReaderMetadata(library);
    });
}

void FedoraRepository::finishGlobalReaderMetadata(const QJsonObject &library)
{
    if (!accountReaderMetadataBytes(0, true)) {
        supersedeGlobalReaderPlan(QStringLiteral("The reader metadata byte count diverged from its approved plan."));
        return;
    }
    const QJsonObject plan = m_libraryReaderPlan;
    verifyReaderPlanState(plan, [this, plan, library](bool current, const QString &error) {
        if (m_libraryReaderPlan.value(QStringLiteral("plan_id")) != plan.value(QStringLiteral("plan_id"))) {
            return;
        }
        if (!current) {
            supersedeGlobalReaderPlan(error.isEmpty() ? QStringLiteral("The server state changed after the reader plan was approved.") : error);
            return;
        }
        m_libraryReaderPlanRetries = 0;
        m_syncCoordinator.phase = ManualSyncPhase::Library;
        emitReaderStepTiming(QStringLiteral("plan_verified"));
        processGlobalReaderLibrary(library);
    });
}

void FedoraRepository::verifyReaderPlanState(const QJsonObject &plan, std::function<void(bool, const QString &)> completion)
{
    if (!m_client || plan.isEmpty()) {
        completion(false, QStringLiteral("The reader plan is unavailable."));
        return;
    }
    QNetworkReply *reply = m_client->syncState();
    connect(reply, &QNetworkReply::finished, this, [reply, plan, completion = std::move(completion)]() mutable {
        if (reply->error() != QNetworkReply::NoError) {
            const Thoughts::SyncErrorInfo error = replyErrorInfo(reply);
            reply->deleteLater();
            completion(false, error.summary);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QJsonObject state = document.object();
        const bool current = document.isObject() && state.value(QStringLiteral("contract_version")).toInt() == 2 &&
            state.value(QStringLiteral("latest_server_sequence")).toInteger(-1) == plan.value(QStringLiteral("latest_server_sequence")).toInteger(-2) &&
            state.value(QStringLiteral("library_revision")).toString() == plan.value(QStringLiteral("library_revision")).toString();
        completion(current, current ? QString() : QStringLiteral("The server state changed after the reader plan was approved."));
    });
}

void FedoraRepository::supersedeGlobalReaderPlan(const QString &message)
{
    m_libraryPulling = false;
    m_libraryReaderPlan = {};
    m_libraryReaderMetadataBytes = 0;
    // The server's Library changed between the plan and what was read under it. Since Sync
    // no longer waits for the server's builds, it often reads while a build that just ended
    // still publishes its results one after the other. Planning again changes nothing on
    // the server: plan again at once, then after a short pause, before giving up.
    static constexpr int replanDelaysMs[] {0, 500, 1500};
    if (m_libraryReaderPlanRetries < int(std::size(replanDelaysMs))) {
        const int delayMs = m_readerReplanDelayScale * replanDelaysMs[m_libraryReaderPlanRetries++];
        m_syncCoordinator.phase = ManualSyncPhase::Library;
        m_syncCoordinator.libraryRefreshState = SyncCoordinator::LibraryRefreshState::Waiting;
        QTimer::singleShot(delayMs, this, &FedoraRepository::refreshLibrary);
        return;
    }
    m_libraryReaderPlanRetries = 0;
    lastReplyFailure().kind = QStringLiteral("transient");
    lastReplyFailure().age.start();
    Q_EMIT connectionStateChanged(false, message);
    if (m_syncCoordinator.running()) {
        failManualSync(message, QString(), QStringLiteral("transient"));
    }
}

void FedoraRepository::supersedeTargetReaderPlan(const QString &target, const QJsonObject &publication, const QString &message)
{
    if (!publication.isEmpty()) {
        m_targetActivationPublication = publication;
    }
    m_targetReaderPlan = {};
    m_targetReaderPlanTarget.clear();
    m_targetReaderPlanPublicationIdentity.clear();
    m_libraryReaderMetadataBytes = 0;
    m_targetSourceDownloadQueue.clear();
    m_targetSourceDownloadsInFlight.clear();
    m_targetDownloadAttempts.clear();
    m_targetPdfDownloadAttempts = 0;
    m_targetPdfDownloading = false;
    if (m_targetReaderPlanRetries++ < 1) {
        if (m_syncCoordinator.running() && m_syncCoordinator.target == target) {
            m_syncCoordinator.phase = ManualSyncPhase::DocumentSnapshot;
            QTimer::singleShot(0, this, &FedoraRepository::pullDocumentSnapshot);
        } else if (m_targetActivationPublication.value(QStringLiteral("filename")).toString() == target) {
            const QJsonObject retryPublication = m_targetActivationPublication;
            QTimer::singleShot(0, this, [this, target, retryPublication] { startTargetedPublicationActivation(target, retryPublication); });
        }
        return;
    }
    m_targetReaderPlanRetries = 0;
    failTargetedBuildActivation(message.isEmpty() ? QStringLiteral("The target reader plan was superseded twice.") : message, target);
}

void FedoraRepository::retryGlobalReaderPlanWithoutPdfDelta(const QString &message)
{
    if (m_libraryPdfDeltaFallbackRequested) {
        Q_EMIT connectionStateChanged(false, message);
        if (m_syncCoordinator.running()) {
            failManualSync(message);
        }
        return;
    }
    m_libraryPdfDeltaFallbackRequested = true;
    m_libraryPulling = false;
    m_libraryReaderPlan = {};
    m_libraryReaderMetadataBytes = 0;
    m_libraryReaderObjectRetries = 0;
    m_libraryReaderPlanRetries = 0;
    m_syncCoordinator.libraryRefreshState = SyncCoordinator::LibraryRefreshState::Waiting;
    emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("The PDF delta could not be verified; requesting the complete PDF."));
    QTimer::singleShot(0, this, &FedoraRepository::refreshLibrary);
}

void FedoraRepository::retryTargetReaderPlanWithoutPdfDelta(const QString &target, const QJsonObject &publication, const QString &message)
{
    if (m_targetPdfDeltaFallbackRequested) {
        failTargetedBuildActivation(message, target);
        return;
    }
    m_targetPdfDeltaFallbackRequested = true;
    m_targetReaderPlan = {};
    m_targetReaderPlanTarget.clear();
    m_targetReaderPlanPublicationIdentity.clear();
    m_libraryReaderMetadataBytes = 0;
    m_targetReaderPlanRetries = 0;
    m_targetSourceDownloadQueue.clear();
    m_targetSourceDownloadsInFlight.clear();
    m_targetDownloadAttempts.clear();
    m_targetPdfDownloadAttempts = 0;
    m_targetPdfDownloading = false;
    emitSyncProgress(QStringLiteral("checking_server"), 0, 0, QStringLiteral("The PDF delta could not be verified; requesting the complete PDF."));
    QTimer::singleShot(0, this, [this, target, publication] { startTargetedPublicationActivation(target, publication); });
}

void FedoraRepository::processGlobalReaderLibrary(const QJsonObject &library)
{
    if (library.value(QStringLiteral("library_revision")).toString() != m_libraryReaderPlan.value(QStringLiteral("library_revision")).toString()) {
        supersedeGlobalReaderPlan(QStringLiteral("The cached Library does not match the approved reader plan."));
        return;
    }
    m_syncCoordinator.libraryLatestServerSequence = m_libraryReaderPlan.value(QStringLiteral("latest_server_sequence")).toInteger(-1);
    refreshAgentProposalBundle(library);
    m_libraryPreparing = true;
    const QPointer<FedoraRepository> repository(this);
    QThreadPool::globalInstance()->start([repository, library] {
        if (!repository) {
            return;
        }
        const PreparedLibraryGeneration prepared = repository->prepareLibraryGeneration(library);
        QMetaObject::invokeMethod(
            repository,
            [repository, library, prepared] {
                if (!repository) {
                    return;
                }
                repository->m_libraryPreparing = false;
                repository->m_libraryPulling = false;
                if (!prepared.success) {
                    Q_EMIT repository->connectionStateChanged(false, prepared.error);
                    if (repository->m_syncCoordinator.running()) {
                        repository->failManualSync(prepared.error);
                    }
                    return;
                }

                QJsonObject state = repository->readRepositoryState();
                bool journalRequired = false;
                if (prepared.alreadyCurrent) {
                    const bool stateAlreadyCurrent = state.value(QStringLiteral("library")) == library && state.value(QStringLiteral("active_library_revision")) == library.value(QStringLiteral("library_revision")) &&
                        state.value(QStringLiteral("active_artifact_revision")).toString() == effectiveArtifactRevision(library) && state.value(QStringLiteral("pending_library_generation")).toObject().isEmpty() &&
                        state.value(QStringLiteral("pending_pdf_projections")).toArray().isEmpty();
                    if (!stateAlreadyCurrent) {
                        state.insert(QStringLiteral("library"), library);
                        state.insert(QStringLiteral("active_library_revision"), library.value(QStringLiteral("library_revision")));
                        state.insert(QStringLiteral("active_artifact_revision"), effectiveArtifactRevision(library));
                        state.remove(QStringLiteral("pending_library_generation"));
                        state.remove(QStringLiteral("pending_pdf_projections"));
                        journalRequired = true;
                    }
                } else if (!prepared.reuseExisting) {
                    state.insert(QStringLiteral("pending_library_generation"), prepared.generation);
                    state.insert(QStringLiteral("pending_pdf_projections"), prepared.pendingObjects);
                    journalRequired = true;
                }
                if (journalRequired && !repository->writeRepositoryState(state)) {
                    Q_EMIT repository->connectionStateChanged(false, QStringLiteral("The prepared Library generation could not be journaled."));
                    if (repository->m_syncCoordinator.running()) {
                        repository->failManualSync(QStringLiteral("The prepared Library generation could not be journaled."));
                    }
                    return;
                }

                if (state.value(QStringLiteral("pending_library_generation")).toObject().isEmpty()) {
                    repository->m_libraryPdfDeltaFallbackRequested = false;
                    if (!repository->recordActivatedReaderGenerationObjects(library)) {
                        qWarning() << "The active Library object identities could not be refreshed in the durable cache.";
                    }
                    repository->activateReaderSourceArtifactsForActiveLibrary(library);
                    Q_EMIT repository->libraryChanged(library);
                    Q_EMIT repository->connectionStateChanged(true, QStringLiteral("Library generation is current."));
                    return;
                }
                Q_EMIT repository->connectionStateChanged(true, QStringLiteral("Preparing a verified Library generation."));
                repository->projectNextPdf();
            },
            Qt::QueuedConnection);
    });
}

void FedoraRepository::retireAgentProposalBundleDownloads()
{
    if (!m_agentProposalBundleDraining) {
        m_agentProposalBundleDraining = true;
        ++m_agentProposalBundleDownloadGeneration;
    }
    // Keep the overall activity busy until every reply from the retired batch
    // has emitted finished and the latest queued request can take ownership.
    m_agentProposalBundlePulling = true;
    QList<QPointer<QNetworkReply>> replies;
    replies.reserve(m_agentProposalBundleReplies.size());
    for (QNetworkReply *reply : std::as_const(m_agentProposalBundleReplies)) {
        replies.append(reply);
    }
    QTimer::singleShot(0, this, [replies] {
        for (const QPointer<QNetworkReply> &reply : replies) {
            if (reply && reply->isRunning()) {
                reply->abort();
            }
        }
    });
    finishAgentProposalBundleDownloadDrain();
}

void FedoraRepository::finishAgentProposalBundleDownloadDrain()
{
    if (!m_agentProposalBundleDraining || !m_agentProposalBundleReplies.isEmpty() || m_agentProposalBundleRestartScheduled) {
        return;
    }
    if (m_pendingAgentProposalBundleLibrary.isEmpty()) {
        m_agentProposalBundleDraining = false;
        m_agentProposalBundlePulling = false;
        m_activeAgentProposalBundleLibrary = {};
        m_activeAgentProposalBundlePlanId.clear();
        wakeManualSyncAfterBackgroundActivity();
        return;
    }

    m_agentProposalBundleRestartScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_agentProposalBundleRestartScheduled = false;
        if (!m_agentProposalBundleDraining || !m_agentProposalBundleReplies.isEmpty()) {
            return;
        }
        const QJsonObject pendingLibrary = std::exchange(m_pendingAgentProposalBundleLibrary, {});
        const QString pendingPlanId = std::exchange(m_pendingAgentProposalBundlePlanId, {});
        m_agentProposalBundleDraining = false;
        m_agentProposalBundlePulling = false;
        m_activeAgentProposalBundleLibrary = {};
        m_activeAgentProposalBundlePlanId.clear();
        if (pendingLibrary.isEmpty() || m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != pendingPlanId) {
            wakeManualSyncAfterBackgroundActivity();
            return;
        }
        refreshAgentProposalBundle(pendingLibrary);
        if (!m_agentProposalBundlePulling) {
            wakeManualSyncAfterBackgroundActivity();
        }
    });
}

void FedoraRepository::refreshAgentProposalBundle(const QJsonObject &library)
{
    if (!m_client) {
        return;
    }
    const QString requestedPlanId = m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString();
    if (m_agentProposalBundlePulling || m_agentProposalBundleDraining || !m_agentProposalBundleReplies.isEmpty()) {
        if (!m_agentProposalBundleDraining && requestedPlanId == m_activeAgentProposalBundlePlanId && library == m_activeAgentProposalBundleLibrary) {
            return;
        }
        m_pendingAgentProposalBundleLibrary = library;
        m_pendingAgentProposalBundlePlanId = requestedPlanId;
        retireAgentProposalBundleDownloads();
        return;
    }
    const QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
    QDir().mkpath(bundleRoot.filePath(QStringLiteral("objects")));
    QDir().mkpath(bundleRoot.filePath(QStringLiteral("documents")));
    const QHash<QString, QJsonObject> desired = readerSourcePublications(library);
    QSet<QString> downloads;
    QString validationError;
    for (auto iterator = desired.cbegin(); iterator != desired.cend(); ++iterator) {
        QSet<QString> missing;
        QString error;
        if (!completeDocumentSourceArtifact(bundleRoot, iterator.value(), &missing, &error)) {
            if (!error.isEmpty()) {
                validationError = error;
                break;
            }
            downloads.unite(missing);
        }
    }
    for (const auto &value : m_libraryReaderPlan.value(QStringLiteral("objects")).toArray()) {
        const auto object = value.toObject();
        if (!plannedObjectHasRole(object, QStringLiteral("project_reading"))
            && !plannedObjectHasRole(object, QStringLiteral("document_pdf"))
            && !plannedObjectHasRole(object, QStringLiteral("document_reading_package"))) continue;
        const QString digest = object.value(QStringLiteral("sha256")).toString();
        const QString path = bundleRoot.filePath(QStringLiteral("objects/") + digest);
        if (QFileInfo(path).size() != object.value(QStringLiteral("size_bytes")).toInteger(-1)
            || fileSha256(path) != digest.toLatin1()) downloads.insert(digest);
    }
    if (!validationError.isEmpty()) {
        Q_EMIT connectionStateChanged(false, validationError);
        return;
    }
    if (downloads.isEmpty()) {
        // Pending source bytes are cache entries, not an active generation.
        // The helper is deliberately a no-op until the PDF generation has
        // completed its atomic activation and has been recorded as current.
        activateReaderSourceArtifactsForActiveLibrary(library);
        return;
    }

    struct DownloadBatch {
        QStringList queue;
        QSet<QString> inFlight;
        QHash<QString, int> attempts;
        QJsonObject library;
        QString planId;
        quint64 generation = 0;
        bool active = true;
    };

    m_agentProposalBundlePulling = true;
    const auto batch = QSharedPointer<DownloadBatch>::create();
    batch->queue = downloads.values();
    std::sort(batch->queue.begin(), batch->queue.end());
    batch->library = library;
    batch->planId = requestedPlanId;
    batch->generation = ++m_agentProposalBundleDownloadGeneration;
    m_activeAgentProposalBundleLibrary = library;
    m_activeAgentProposalBundlePlanId = requestedPlanId;
    const auto advance = QSharedPointer<std::function<void()>>::create();
    const QWeakPointer<std::function<void()>> weakAdvance(advance);
    *advance = [this, bundleRoot, batch, weakAdvance] {
        if (!batch->active) {
            return;
        }
        if (m_agentProposalBundleDownloadGeneration != batch->generation) {
            batch->active = false;
            batch->queue.clear();
            batch->inFlight.clear();
            finishAgentProposalBundleDownloadDrain();
            return;
        }
        if (m_agentProposalBundleDraining || !m_agentProposalBundlePulling || m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != batch->planId) {
            batch->active = false;
            batch->queue.clear();
            batch->inFlight.clear();
            retireAgentProposalBundleDownloads();
            return;
        }

        if (batch->queue.isEmpty() && batch->inFlight.isEmpty()) {
            const QJsonObject completedLibrary = batch->library;
            batch->active = false;
            if (const auto keepAlive = weakAdvance.toStrongRef()) {
                QTimer::singleShot(0, this, [this, completedLibrary, batch, keepAlive] {
                    *keepAlive = {};
                    if (m_agentProposalBundleDownloadGeneration != batch->generation || m_agentProposalBundleDraining) {
                        finishAgentProposalBundleDownloadDrain();
                        return;
                    }
                    m_agentProposalBundlePulling = false;
                    m_activeAgentProposalBundleLibrary = {};
                    m_activeAgentProposalBundlePlanId.clear();
                    refreshAgentProposalBundle(completedLibrary);
                    if (!m_agentProposalBundlePulling) {
                        wakeManualSyncAfterBackgroundActivity();
                    }
                });
            } else {
                m_agentProposalBundlePulling = false;
                m_activeAgentProposalBundleLibrary = {};
                m_activeAgentProposalBundlePlanId.clear();
                wakeManualSyncAfterBackgroundActivity();
            }
            return;
        }

        while (batch->inFlight.size() < 4 && !batch->queue.isEmpty()) {
            const QString objectSha256 = batch->queue.takeFirst();
            const qint64 plannedSize = readerPlanObjectSize(m_libraryReaderPlan, objectSha256);
            const bool planned = readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("project_reading")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("document_pdf")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("document_reading_package")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_manifest")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_snapshot")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_resource")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("renderer_evidence")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("reverse_lookup")) ||
                readerPlanAllowsObject(m_libraryReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("raw_synctex_fallback"));
            if (!planned) {
                batch->active = false;
                batch->queue.clear();
                batch->inFlight.clear();
                retireAgentProposalBundleDownloads();
                if (m_libraryReaderObjectRetries++ < 1) {
                    m_libraryReaderPlanRetries = 0;
                    supersedeGlobalReaderPlan(QStringLiteral("The source-artifact download no longer matches its reader synchronization plan."));
                } else {
                    m_libraryReaderObjectRetries = 0;
                    Q_EMIT connectionStateChanged(false, QStringLiteral("A source-artifact download no longer matches its reader synchronization plan."));
                }
                return;
            }

            batch->inFlight.insert(objectSha256);
            QNetworkReply *reply = m_client->sourceBundleObject(objectSha256.toLatin1(), batch->planId.toLatin1());
            const auto stream = streamBundleObject(reply, bundleRoot, objectSha256, plannedSize);
            m_agentProposalBundleReplies.insert(reply);
            connect(reply, &QNetworkReply::downloadProgress, this, [this, batch, objectSha256, plannedSize](qint64 received, qint64) {
                if (batch->active && !m_agentProposalBundleDraining && m_agentProposalBundlePulling && m_agentProposalBundleDownloadGeneration == batch->generation &&
                    m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() == batch->planId) {
                    accountReaderObjectProgress(objectSha256, qMin(received, plannedSize));
                }
            });
            const auto replyAdvance = weakAdvance.toStrongRef();
            connect(reply, &QNetworkReply::finished, this, [this, reply, bundleRoot, batch, advance = replyAdvance, objectSha256, plannedSize, stream] {
                m_agentProposalBundleReplies.remove(reply);
                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool networkSuccess = reply->error() == QNetworkReply::NoError;
                const QString networkError = reply->errorString();
                const bool stored = stream->finish(reply, objectSha256);
                reply->deleteLater();
                const auto clearAdvance = [advance] {
                    if (advance) {
                        *advance = {};
                    }
                };
                if (!batch->active) {
                    finishAgentProposalBundleDownloadDrain();
                    return;
                }
                if (m_agentProposalBundleDownloadGeneration != batch->generation) {
                    batch->active = false;
                    batch->queue.clear();
                    batch->inFlight.clear();
                    clearAdvance();
                    finishAgentProposalBundleDownloadDrain();
                    return;
                }
                if (m_agentProposalBundleDraining || !m_agentProposalBundlePulling || m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != batch->planId) {
                    batch->active = false;
                    batch->queue.clear();
                    batch->inFlight.clear();
                    clearAdvance();
                    retireAgentProposalBundleDownloads();
                    return;
                }
                batch->inFlight.remove(objectSha256);

                if (httpStatus == 409) {
                    accountReaderObjectProgress(objectSha256, 0, false, true);
                    batch->active = false;
                    batch->queue.clear();
                    batch->inFlight.clear();
                    clearAdvance();
                    retireAgentProposalBundleDownloads();
                    if (m_libraryReaderObjectRetries++ < 1) {
                        m_libraryReaderPlanRetries = 0;
                        supersedeGlobalReaderPlan(QStringLiteral("The source-artifact reader plan was superseded."));
                    } else {
                        m_libraryReaderObjectRetries = 0;
                        Q_EMIT connectionStateChanged(false, QStringLiteral("The source-artifact reader plan was superseded twice."));
                    }
                    return;
                }

                if (!networkSuccess) {
                    accountReaderObjectProgress(objectSha256, 0, false, true);
                    const int attempt = batch->attempts.value(objectSha256) + 1;
                    batch->attempts.insert(objectSha256, attempt);
                    if (attempt <= 3) {
                        // Keep this retry in flight while it is backing off so
                        // the batch remains bounded even during an outage.
                        batch->inFlight.insert(objectSha256);
                        static constexpr int backoffMs[] {1000, 2000, 4000};
                        const int delay = backoffMs[attempt - 1] + QRandomGenerator::global()->bounded(251);
                        emitSyncProgress(QStringLiteral("waiting_network"), 0, 0, QStringLiteral("Source-artifact download interrupted; retrying the same object."));
                        QTimer::singleShot(delay, this, [this, batch, advance, objectSha256] {
                            const auto clearAdvance = [advance] {
                                if (advance) {
                                    *advance = {};
                                }
                            };
                            if (!batch->active) {
                                return;
                            }
                            if (m_agentProposalBundleDownloadGeneration != batch->generation) {
                                batch->active = false;
                                batch->queue.clear();
                                batch->inFlight.clear();
                                clearAdvance();
                                finishAgentProposalBundleDownloadDrain();
                                return;
                            }
                            if (m_agentProposalBundleDraining || !m_agentProposalBundlePulling || m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != batch->planId) {
                                batch->active = false;
                                batch->queue.clear();
                                batch->inFlight.clear();
                                clearAdvance();
                                retireAgentProposalBundleDownloads();
                                return;
                            }
                            batch->inFlight.remove(objectSha256);
                            batch->queue.prepend(objectSha256);
                            if (advance && *advance) {
                                (*advance)();
                            }
                        });
                        if (advance && *advance) {
                            (*advance)();
                        }
                        return;
                    }
                }

                const bool success = networkSuccess && stored;
                if (!success) {
                    accountReaderObjectProgress(objectSha256, 0, false, true);
                    batch->active = false;
                    batch->queue.clear();
                    batch->inFlight.clear();
                    clearAdvance();
                    retireAgentProposalBundleDownloads();
                    Q_EMIT connectionStateChanged(false,
                                                  networkError.isEmpty() ? QStringLiteral("A source-artifact object could not be downloaded or verified.") : networkError);
                    return;
                }

                const QString path = bundleRoot.filePath(QStringLiteral("objects/%1").arg(objectSha256));
                recordVerifiedReaderObject(path, objectSha256, plannedSize, QStringLiteral("source"));
                accountReaderObjectProgress(objectSha256, plannedSize, true);
                batch->attempts.remove(objectSha256);
                m_libraryReaderObjectRetries = 0;
                if (advance && *advance) {
                    (*advance)();
                }
            });
        }
    };
    Q_EMIT connectionStateChanged(true, QStringLiteral("Downloading %1 verified source-artifact objects (up to four concurrently).").arg(batch->queue.size()));
    (*advance)();
}

bool FedoraRepository::activateReaderSourceArtifactsForActiveLibrary(const QJsonObject &library)
{
    const QJsonObject state = readRepositoryState();
    const QString revision = library.value(QStringLiteral("library_revision")).toString();
    if (!validSha256(revision) || state.value(QStringLiteral("active_library_revision")).toString() != revision || state.value(QStringLiteral("library")).toObject().value(QStringLiteral("library_revision")).toString() != revision) {
        return false;
    }

    const QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
    const QHash<QString, QJsonObject> desired = readerSourcePublications(library);
    for (auto iterator = desired.cbegin(); iterator != desired.cend(); ++iterator) {
        if (!completeDocumentSourceArtifact(bundleRoot, iterator.value())) {
            return false;
        }
    }
    for (auto iterator = desired.cbegin(); iterator != desired.cend(); ++iterator) {
        if (!activateDocumentSourceArtifact(bundleRoot, iterator.value())) {
            return false;
        }
    }
    const QDir documents(bundleRoot.filePath(QStringLiteral("documents")));
    for (const QFileInfo &pointer : documents.entryInfoList({QStringLiteral("*.pdf.json")}, QDir::Files)) {
        const QString filename = pointer.fileName().chopped(5);
        if (!desired.contains(filename)) {
            QFile::remove(pointer.absoluteFilePath());
        }
    }
    if (!writeActiveSourceArtifactIndex(bundleRoot, library, desired)) {
        return false;
    }

    // Be conservative if another verified generation is already pending.  In
    // normal global activation this set is empty, while a concurrent targeted
    // activation can still retain its complete source-object closure.
    QList<QJsonObject> retained = desired.values();
    const QJsonObject pendingLibrary = state.value(QStringLiteral("pending_library_generation")).toObject().value(QStringLiteral("library")).toObject();
    const QHash<QString, QJsonObject> pending = readerSourcePublications(pendingLibrary);
    for (auto iterator = pending.cbegin(); iterator != pending.cend(); ++iterator) {
        retained.append(iterator.value());
    }
    if (!m_targetActivationPublication.isEmpty()) {
        const QJsonObject target = m_targetActivationPublication;
        const QString filename = target.value(QStringLiteral("filename")).toString();
        if (safePdfFilename(filename) && validSha256(target.value(QStringLiteral("source_artifact_sha256")).toString())) {
            retained.append(target);
        }
    }
    QSet<QString> readingObjects;
    for (const auto &snapshot : {library, pendingLibrary}) {
        for (const auto &value : snapshot.value(QStringLiteral("project_readings")).toArray()) {
            const QString digest = value.toObject().value(QStringLiteral("sha256")).toString();
            QByteArray bytes;
            if (!Thoughts::readVerifiedArtifact(bundleRoot.filePath(QStringLiteral("objects/") + digest), digest, &bytes)) return true;
            const auto reading = Thoughts::ProjectReading::parse(bytes);
            if (!reading) return true;
            readingObjects.insert(digest);
            for (const auto &document : reading->documents) {
                readingObjects.insert(document.pdfSha256);
                readingObjects.insert(document.packageSha256);
            }
        }
    }
    // Open readers pin their immutable generation. Retain its source closures too,
    // including document versions no longer present in the newest publication list.
    const QDir generationRoot(m_docsRoot);
    const auto generations = generationRoot.entryInfoList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    // A retained generation never changes after it is written, yet every Sync reloaded each of its
    // readings and every document package on the GUI thread (23 readings took about 0.4 s). A new
    // generation mostly reuses the previous one's packages, which are content-addressed, so only
    // the small manifest is read and each package is parsed once, when its digest first appears.
    struct PackageIdentity { bool sourceArtifact = false; QString filename; };
    static QHash<QString, PackageIdentity> packageIdentities;
    for (const auto &generation : generations) {
        const QDir directory(generation.absoluteFilePath());
        for (const auto &name : directory.entryList({QStringLiteral("*.thoughts-project")}, QDir::Files)) {
            QFile file(directory.filePath(name));
            if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return true;
            const QByteArray bytes = file.readAll();
            const auto manifest = Thoughts::ProjectReading::parse(bytes);
            if (!manifest) return true; // An unreadable retained generation cannot authorize pruning.
            readingObjects.insert(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            for (const auto &document : manifest->documents) {
                const QString packagePath = directory.filePath(QStringLiteral("objects/") + document.packageSha256);
                auto identity = packageIdentities.constFind(document.packageSha256);
                if (identity == packageIdentities.cend()) {
                    QByteArray raw;
                    if (!Thoughts::readVerifiedArtifact(packagePath, document.packageSha256, &raw)) return true;
                    const auto package = QJsonDocument::fromJson(raw).object();
                    const auto kind = package.value(QStringLiteral("kind")).toString();
                    if (kind != QLatin1String("thoughts-document-source-artifact") && kind != QLatin1String("thoughts-document-reading-package")) return true;
                    static const QRegularExpression pdfName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"));
                    if (kind == QLatin1String("thoughts-document-source-artifact")
                        && !pdfName.match(package.value(QStringLiteral("filename")).toString()).hasMatch()) return true;
                    identity = packageIdentities.insert(document.packageSha256,
                        {kind == QLatin1String("thoughts-document-source-artifact"), package.value(QStringLiteral("filename")).toString()});
                } else if (!QFileInfo::exists(packagePath)) {
                    return true;
                }
                readingObjects.insert(document.pdfSha256);
                readingObjects.insert(document.packageSha256);
                if (!identity->sourceArtifact) continue;
                retained.append(QJsonObject{{QStringLiteral("filename"), identity->filename},
                    {QStringLiteral("sha256"), document.pdfSha256},
                    {QStringLiteral("source_artifact_sha256"), document.packageSha256}});
            }
        }
    }
    pruneInactiveReaderSourceObjects(bundleRoot, retained, readingObjects);
    return true;
}

bool FedoraRepository::validateSourceIndex(const QByteArray &contents, const QJsonObject &library, QString *error)
{
    const QJsonDocument document = QJsonDocument::fromJson(contents);
    const QJsonObject root = document.object();
    if (!document.isObject() || root.value(QStringLiteral("schema_version")).toInt() != 1 || !root.value(QStringLiteral("pdfs")).isArray()) {
        if (error) {
            *error = QStringLiteral("The server's source-layout index is not a supported JSON document.");
        }
        return false;
    }

    QHash<QString, QString> publicationDigests;
    QSet<QString> expectedLinked;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const QString generationId = publication.value(QStringLiteral("generation_id")).toString();
        const QString kind = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("kind")).toString(QStringLiteral("source_linked"));
        if (safePdfFilename(filename) && validSha256(digest) && validSha256(generationId)) {
            publicationDigests.insert(filename, digest);
            if (kind == QLatin1String("source_linked")) {
                expectedLinked.insert(filename);
            }
        }
    }

    QSet<QString> indexed;
    for (const QJsonValue &value : root.value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString filename = entry.value(QStringLiteral("filename")).toString();
        const QString digest = entry.value(QStringLiteral("pdf_sha256")).toString();
        if (!safePdfFilename(filename) || !expectedLinked.contains(filename) || indexed.contains(filename) || (!digest.isEmpty() && digest != publicationDigests.value(filename))) {
            if (error) {
                *error = QStringLiteral("The server's source-layout index does not match its Library publication set.");
            }
            return false;
        }
        indexed.insert(filename);
    }
    if (indexed != expectedLinked) {
        if (error) {
            *error = QStringLiteral("The server's source-layout index is missing a source-linked Library PDF.");
        }
        return false;
    }
    return true;
}

bool FedoraRepository::directoryMatchesLibraryGeneration(const QString &directory, const QJsonObject &generation, QString *error)
{
    const QString revision = generation.value(QStringLiteral("library_revision")).toString();
    const QString artifactRevision = generation.value(QStringLiteral("artifact_revision")).toString(revision);
    const QString sourceIndexSha256 = generation.value(QStringLiteral("source_index_sha256")).toString();
    const QJsonObject library = generation.value(QStringLiteral("library")).toObject();
    QFile manifestFile(QDir(directory).filePath(QStringLiteral(".thoughts-generation.json")));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("The Library generation manifest is missing.");
        }
        return false;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    if (manifest.value(QStringLiteral("schema_version")).toInt() != 1 || manifest.value(QStringLiteral("library_revision")).toString() != revision ||
        manifest.value(QStringLiteral("artifact_revision")).toString(revision) != artifactRevision || manifest.value(QStringLiteral("source_index_sha256")).toString() != sourceIndexSha256 ||
        manifest.value(QStringLiteral("publications")).toArray() != library.value(QStringLiteral("publications")).toArray() ||
        manifest.value(QStringLiteral("project_readings")).toArray() != library.value(QStringLiteral("project_readings")).toArray()) {
        if (error) {
            *error = QStringLiteral("The Library generation manifest does not match the server.");
        }
        return false;
    }

    if (!Thoughts::prepareProjectReadings(library, directory, QString(), false, error)) return false;

    const QString target = generation.value(QStringLiteral("targeted_activation")).toBool()
        ? generation.value(QStringLiteral("target_publication")).toObject().value(QStringLiteral("filename")).toString() : QString();
    if (!target.isEmpty() && !safePdfFilename(target)) return false;
    QSet<QString> verifiedFilenames;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const QString generationId = publication.value(QStringLiteral("generation_id")).toString();
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        const QString path = QDir(directory).filePath(filename);
        const QFileInfo info(path);
        if (!safePdfFilename(filename) || verifiedFilenames.contains(filename) || !validSha256(digest) || !validSha256(generationId) || size < 0 ||
            ((target.isEmpty() || filename == target) && (!info.isFile() || info.size() != size || fileSha256(path) != digest.toLatin1()))) {
            if (error) {
                *error = QStringLiteral("The staged Library PDF %1 failed identity verification.").arg(filename);
            }
            return false;
        }
        verifiedFilenames.insert(filename);
    }
    QSet<QString> directoryPdfFilenames;
    for (const QFileInfo &info : QDir(directory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (info.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            directoryPdfFilenames.insert(info.fileName());
        }
    }
    if (target.isEmpty() ? directoryPdfFilenames != verifiedFilenames
                         : !directoryPdfFilenames.contains(target) || !(directoryPdfFilenames - verifiedFilenames).isEmpty()) {
        if (error) {
            *error = QStringLiteral("The Library directory contains PDFs outside the published generation.");
        }
        return false;
    }

    const QString sourceIndexPath = QDir(directory).filePath(QStringLiteral(".thoughts-source-index.json"));
    QFile sourceIndex(sourceIndexPath);
    if (!sourceIndex.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("The staged source-layout index is missing.");
        }
        return false;
    }
    const QByteArray sourceContents = sourceIndex.readAll();
    if (Thoughts::SyncClient::sha256(sourceContents) != sourceIndexSha256.toLatin1() || !validateSourceIndex(sourceContents, library, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("The staged source-layout index failed SHA-256 verification.");
        }
        return false;
    }

    QFile libraryIndex(QDir(directory).filePath(QStringLiteral(".thoughts-library-index.json")));
    const QJsonDocument libraryIndexDocument = libraryIndex.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(libraryIndex.readAll()) : QJsonDocument();
    const QJsonObject libraryIndexRoot = libraryIndexDocument.object();
    if (!libraryIndexDocument.isObject() || libraryIndexRoot.value(QStringLiteral("schema_version")).toInt() != 1 || libraryIndexRoot.value(QStringLiteral("library_revision")).toString() != revision ||
        libraryIndexRoot.value(QStringLiteral("source_index_sha256")).toString() != sourceIndexSha256 || !libraryIndexRoot.value(QStringLiteral("pdfs")).isArray()) {
        if (error) {
            *error = QStringLiteral("The staged Library membership index is incomplete.");
        }
        return false;
    }
    QHash<QString, QJsonObject> expectedMembership;
    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const QString generationId = publication.value(QStringLiteral("generation_id")).toString();
        const QString kind = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("kind")).toString(QStringLiteral("source_linked"));
        QJsonObject entry {{QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), digest}, {QStringLiteral("kind"), kind}};
        entry.insert(QStringLiteral("document_id"), publication.value(QStringLiteral("document_id")));
        entry.insert(QStringLiteral("generation_id"), publication.value(QStringLiteral("generation_id")));
        entry.insert(QStringLiteral("project"), publication.value(QStringLiteral("project")));
        entry.insert(QStringLiteral("build_id"), publication.value(QStringLiteral("build_id")));
        entry.insert(QStringLiteral("source_frontier"), publication.value(QStringLiteral("source_frontier")));
        entry.insert(QStringLiteral("comment_frontier"), publication.value(QStringLiteral("comment_frontier")));
        entry.insert(QStringLiteral("annotation_frontier"), publication.value(QStringLiteral("annotation_frontier")));
        entry.insert(QStringLiteral("placement_manifest_sha256"), publication.value(QStringLiteral("placement_manifest_sha256")));
        entry.insert(QStringLiteral("input_snapshot_sha256"), publication.value(QStringLiteral("input_snapshot_sha256")));
        if (publication.value(QStringLiteral("source_resources")).isArray()) {
            entry.insert(QStringLiteral("source_resources"), publication.value(QStringLiteral("source_resources")));
        }
        const QString publishedAt = publication.value(QStringLiteral("published_at")).toString();
        if (!publishedAt.isEmpty()) {
            entry.insert(QStringLiteral("published_at"), publishedAt);
        }
        entry.insert(QStringLiteral("pdf_stale"), publication.value(QStringLiteral("pdf_stale")).toBool());
        if (publication.value(QStringLiteral("latest_build")).isObject()) {
            entry.insert(QStringLiteral("latest_build"), publication.value(QStringLiteral("latest_build")));
        }
        const QString displayName = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("display_name")).toString().trimmed();
        if (!displayName.isEmpty()) {
            entry.insert(QStringLiteral("display_name"), displayName);
        }
        entry.insert(QStringLiteral("archived"), publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("archived")).toBool());
        expectedMembership.insert(filename, entry);
    }
    QSet<QString> indexedMembership;
    for (const QJsonValue &value : libraryIndexRoot.value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString filename = entry.value(QStringLiteral("filename")).toString();
        const auto expected = expectedMembership.constFind(filename);
        if (expected == expectedMembership.cend() || indexedMembership.contains(filename) || entry != expected.value()) {
            if (error) {
                *error = QStringLiteral("The staged Library membership index does not match the server.");
            }
            return false;
        }
        indexedMembership.insert(filename);
    }
    if (indexedMembership.size() != expectedMembership.size()) {
        if (error) {
            *error = QStringLiteral("The staged Library membership index is incomplete.");
        }
        return false;
    }
    return true;
}

bool FedoraRepository::libraryGenerationMatches(const QString &directory, const QJsonObject &generation) const
{
    return directoryMatchesLibraryGeneration(directory, generation, nullptr);
}

int FedoraRepository::garbageCollectLibraryGenerations()
{
    const auto running = m_libraryGarbageCollecting;
    if (running->exchange(true)) {
        return 0;
    }
    const auto releaseGarbageCollection = qScopeGuard([running] { running->store(false); });
    return collectObsoleteLibraryGenerations(m_docsRoot, readRepositoryState(), 0);
}

void FedoraRepository::scheduleLibraryGenerationCleanup(int delayMs)
{
    QTimer::singleShot(delayMs, this, [this] {
        if (m_libraryPreparing || m_libraryActivating || m_projectingPdf) {
            scheduleLibraryGenerationCleanup(60000);
            return;
        }
        const auto running = m_libraryGarbageCollecting;
        if (running->exchange(true)) {
            return;
        }
        // Read the state here: readRepositoryState() updates caches and is not
        // thread-safe. The worker only receives copies, and is detached so an
        // unavailable File Provider can never block the UI or shutdown.
        std::thread([running, docsRoot = m_docsRoot, state = readRepositoryState()] {
            const int removed = collectObsoleteLibraryGenerations(docsRoot, state, 10 * 60);
            if (removed < 0) {
                qWarning() << "Obsolete Library generations were kept: the active generation or open-file check could not be verified.";
            } else if (removed > 0) {
                qInfo() << "Removed" << removed << "obsolete Library generation(s).";
            }
            running->store(false);
        }).detach();
    });
}

int FedoraRepository::collectObsoleteLibraryGenerations(const QString &docsRoot, const QJsonObject &state, qint64 minimumAgeSeconds)
{
    const QJsonObject activeLibrary = state.value(QStringLiteral("library")).toObject();
    const QString activeRevision = state.value(QStringLiteral("active_library_revision")).toString();
    const QString activeArtifactRevision = state.value(QStringLiteral("active_artifact_revision")).toString(activeRevision);
    const QString activeSourceIndex = activeLibrary.value(QStringLiteral("source_index_sha256")).toString();
    const QString current = QDir(docsRoot).filePath(QStringLiteral("current_pdfs"));
    const QJsonObject activeGeneration {
        {QStringLiteral("library_revision"), activeRevision},
        {QStringLiteral("artifact_revision"), activeArtifactRevision},
        {QStringLiteral("source_index_sha256"), activeSourceIndex},
        {QStringLiteral("library"), activeLibrary},
    };
    if (!validSha256(activeRevision) || !validSha256(activeArtifactRevision) || !validSha256(activeSourceIndex) || activeLibrary.isEmpty() || !directoryMatchesLibraryGeneration(current, activeGeneration, nullptr)) {
        return -1;
    }

    QSet<QString> protectedDirectories;
    if (!externallyOpenGenerationDirectories(docsRoot, &protectedDirectories)) {
        return -1;
    }
    const QFileInfo currentInfo(current);
    const QString linkedTarget = Thoughts::Platform::directoryLinkTarget(currentInfo);
    const QString activeTarget = linkedTarget.isEmpty() ? currentInfo.absoluteFilePath() : linkedTarget;
    protectedDirectories.insert(QDir::cleanPath(activeTarget));
    const QString pending = state.value(QStringLiteral("pending_library_generation")).toObject().value(QStringLiteral("staging_path")).toString();
    if (!pending.isEmpty()) {
        protectedDirectories.insert(QDir::cleanPath(QFileInfo(pending).absoluteFilePath()));
    }

    static const QRegularExpression stageName(QStringLiteral("^\\.thoughts-library-stage-([0-9a-f]{64})$"));
    int removed = 0;
    const QDir root(docsRoot);
    const QDateTime newestRemovable = QDateTime::currentDateTimeUtc().addSecs(-minimumAgeSeconds);
    for (const QFileInfo &info : root.entryInfoList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const QRegularExpressionMatch nameMatch = stageName.match(info.fileName());
        const QString directory = QDir::cleanPath(info.absoluteFilePath());
        // A recent directory may belong to a Sync that has not recorded its
        // pending generation yet.
        if (!nameMatch.hasMatch() || protectedDirectories.contains(directory) || (minimumAgeSeconds > 0 && info.lastModified().toUTC() > newestRemovable)) {
            continue;
        }
        QFile manifestFile(QDir(directory).filePath(QStringLiteral(".thoughts-generation.json")));
        const QJsonObject manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
        // Windows cannot delete a folder while one of its files is open.
        manifestFile.close();
        const QString revision = manifest.value(QStringLiteral("library_revision")).toString();
        const QString artifactRevision = manifest.value(QStringLiteral("artifact_revision")).toString(revision);
        const QString sourceIndex = manifest.value(QStringLiteral("source_index_sha256")).toString();
        const QJsonArray publications = manifest.value(QStringLiteral("publications")).toArray();
        const QJsonObject library {
            {QStringLiteral("library_revision"), revision},
            {QStringLiteral("artifact_revision"), artifactRevision},
            {QStringLiteral("source_index_sha256"), sourceIndex},
            {QStringLiteral("publications"), publications},
            {QStringLiteral("project_readings"), manifest.value(QStringLiteral("project_readings")).toArray()},
        };
        const QJsonObject generation {
            {QStringLiteral("library_revision"), revision},
            {QStringLiteral("artifact_revision"), artifactRevision},
            {QStringLiteral("source_index_sha256"), sourceIndex},
            {QStringLiteral("library"), library},
        };
        if (manifest.value(QStringLiteral("schema_version")).toInt() != 1 || revision != nameMatch.captured(1) || !validSha256(artifactRevision) || !validSha256(sourceIndex) || publications.isEmpty() ||
            !directoryMatchesLibraryGeneration(directory, generation, nullptr)) {
            continue;
        }
        if (removeReadOnlyGeneration(directory)) {
            ++removed;
        } else {
            qWarning() << "The obsolete Library generation could not be removed:" << directory;
        }
    }
    return removed;
}

FedoraRepository::PreparedLibraryGeneration FedoraRepository::prepareLibraryGeneration(const QJsonObject &library)
{
    PreparedLibraryGeneration prepared;
    const QString revision = library.value(QStringLiteral("library_revision")).toString();
    const QString artifactRevision = effectiveArtifactRevision(library);
    const QString sourceIndexSha256 = library.value(QStringLiteral("source_index_sha256")).toString();
    if (!validSha256(revision) || !validSha256(artifactRevision) || !validSha256(sourceIndexSha256) || !library.value(QStringLiteral("publications")).isArray()) {
        prepared.error = QStringLiteral("The server returned an incomplete Library generation identity.");
        return prepared;
    }

    QJsonObject state = readRepositoryState();
    const QString current = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs"));
    const QJsonObject activeGeneration {
        {QStringLiteral("library_revision"), revision}, {QStringLiteral("artifact_revision"), artifactRevision}, {QStringLiteral("source_index_sha256"), sourceIndexSha256}, {QStringLiteral("library"), library}};
    if (state.value(QStringLiteral("active_library_revision")).toString() == revision && state.value(QStringLiteral("library")) == library && state.value(QStringLiteral("pending_library_generation")).toObject().isEmpty() &&
        state.value(QStringLiteral("pending_pdf_projections")).toArray().isEmpty() && directoryMatchesLibraryGeneration(current, activeGeneration, nullptr)) {
        prepared.success = true;
        prepared.alreadyCurrent = true;
        return prepared;
    }

    const QJsonObject existing = state.value(QStringLiteral("pending_library_generation")).toObject();
    const QString existingStaging = existing.value(QStringLiteral("staging_path")).toString();
    if (existing.value(QStringLiteral("library_revision")).toString() == revision && existing.value(QStringLiteral("source_index_sha256")).toString() == sourceIndexSha256 && existing.value(QStringLiteral("library")).toObject() == library &&
        QFileInfo::exists(existingStaging)) {
        if (!writeLibraryGenerationMetadata(library, existingStaging)) {
            prepared.error = QStringLiteral("The pending Library generation metadata could not be repaired.");
            return prepared;
        }
        QJsonObject repaired = existing;
        repaired.insert(QStringLiteral("library_revision"), revision);
        repaired.insert(QStringLiteral("artifact_revision"), artifactRevision);
        repaired.insert(QStringLiteral("source_index_sha256"), sourceIndexSha256);
        repaired.insert(QStringLiteral("library"), library);
        if (!state.value(QStringLiteral("pending_pdf_projections")).toArray().isEmpty() || directoryMatchesLibraryGeneration(existingStaging, repaired, nullptr)) {
            prepared.success = true;
            prepared.reuseExisting = true;
            prepared.generation = repaired;
            return prepared;
        }
    }

    if (directoryMatchesLibraryGeneration(current, activeGeneration, nullptr)) {
        prepared.success = true;
        prepared.alreadyCurrent = true;
        return prepared;
    }

    QSet<QString> filenames;
    QJsonArray pendingObjects;
    QJsonArray changedPdfs;
    const QString staging = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts-library-stage-%1").arg(revision));
    if (QFileInfo::exists(staging) && pathsReferToSameLocation(current, staging)) {
        prepared.error = QStringLiteral("The calculated Library staging generation is active but failed verification; refusing to replace it.");
        return prepared;
    }
    // A generation that failed activation after it was finalized is read-only.
    if (QFileInfo::exists(staging) && !removeReadOnlyGeneration(staging)) {
        prepared.error = QStringLiteral("The stale Library staging directory could not be cleared.");
        return prepared;
    }
    if (!QDir().mkpath(staging)) {
        prepared.error = QStringLiteral("The Library staging directory could not be created.");
        return prepared;
    }

    QFile currentManifestFile(QDir(current).filePath(QStringLiteral(".thoughts-generation.json")));
    const QJsonObject currentManifest = currentManifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(currentManifestFile.readAll()).object() : QJsonObject();
    bool currentIdentitiesValid = false;
    const auto currentIdentities = publicationArtifactIdentities(currentManifest.value(QStringLiteral("publications")).toArray(), &currentIdentitiesValid);

    for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
        const QJsonObject publication = value.toObject();
        const QString filename = publication.value(QStringLiteral("filename")).toString();
        const QString digest = publication.value(QStringLiteral("sha256")).toString();
        const QString generationId = publication.value(QStringLiteral("generation_id")).toString();
        const qint64 size = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
        const QString kind = publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("kind")).toString(QStringLiteral("source_linked"));
        if (!safePdfFilename(filename) || !validSha256(digest) || !validSha256(generationId) || size < 0 || filenames.contains(filename) || (kind != QLatin1String("source_linked") && kind != QLatin1String("unlinked"))) {
            prepared.error = QStringLiteral("The server returned an invalid or duplicate Library publication.");
            QDir(staging).removeRecursively();
            return prepared;
        }
        filenames.insert(filename);
        const QString source = QDir(current).filePath(filename);
        const QString destination = QDir(staging).filePath(filename);
        const bool alreadyCurrent = currentIdentitiesValid && currentIdentities.value(filename) == qMakePair(digest, size) && QFileInfo(source).isFile() && QFileInfo(source).size() == size;
        if (alreadyCurrent) {
            if (!linkOrCopyFile(source, destination)) {
                prepared.error = QStringLiteral("The current Library PDF %1 could not be staged.").arg(filename);
                QDir(staging).removeRecursively();
                return prepared;
            }
        } else {
            pendingObjects.append(QJsonObject {{QStringLiteral("type"), QStringLiteral("pdf")}, {QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), digest}, {QStringLiteral("size_bytes"), size}});
            changedPdfs.append(filename);
        }
    }

    const QString currentSourceIndex = QDir(current).filePath(QStringLiteral(".thoughts-source-index.json"));
    const QString stagedSourceIndex = QDir(staging).filePath(QStringLiteral(".thoughts-source-index.json"));
    if (currentManifest.value(QStringLiteral("source_index_sha256")).toString() == sourceIndexSha256 && QString::fromLatin1(fileSha256(currentSourceIndex)) == sourceIndexSha256) {
        if (!linkOrCopyFile(currentSourceIndex, stagedSourceIndex)) {
            prepared.error = QStringLiteral("The verified source-layout index could not be staged.");
            QDir(staging).removeRecursively();
            return prepared;
        }
    } else {
        pendingObjects.append(QJsonObject {{QStringLiteral("type"), QStringLiteral("source_index")}, {QStringLiteral("filename"), QStringLiteral(".thoughts-source-index.json")}, {QStringLiteral("sha256"), sourceIndexSha256}});
    }

    if (!writeLibraryGenerationMetadata(library, staging)) {
        prepared.error = QStringLiteral("The staged Library generation metadata could not be written.");
        QDir(staging).removeRecursively();
        return prepared;
    }

    const QJsonObject generation {{QStringLiteral("library_revision"), revision},
                                  {QStringLiteral("artifact_revision"), artifactRevision},
                                  {QStringLiteral("source_index_sha256"), sourceIndexSha256},
                                  {QStringLiteral("staging_path"), staging},
                                  {QStringLiteral("changed_pdfs"), changedPdfs},
                                  {QStringLiteral("library"), library}};
    prepared.success = true;
    prepared.generation = generation;
    prepared.pendingObjects = pendingObjects;
    return prepared;
}

QJsonObject FedoraRepository::prepareLibraryGenerationResult(const QJsonObject &library)
{
    const PreparedLibraryGeneration prepared = prepareLibraryGeneration(library);
    return QJsonObject {
        {QStringLiteral("success"), prepared.success},
        {QStringLiteral("already_current"), prepared.alreadyCurrent},
        {QStringLiteral("reuse_existing"), prepared.reuseExisting},
        {QStringLiteral("generation"), prepared.generation},
        {QStringLiteral("pending_objects"), prepared.pendingObjects},
        {QStringLiteral("error"), prepared.error},
    };
}

void FedoraRepository::finalizeLibraryGeneration(const QJsonObject &generation)
{
    QJsonObject state = readRepositoryState();
    const QJsonObject library = generation.value(QStringLiteral("library")).toObject();
    if (generation.value(QStringLiteral("targeted_activation")).toBool()) {
        m_targetPdfDeltaFallbackRequested = false;
        const QJsonObject publication = generation.value(QStringLiteral("target_publication")).toObject();
        const QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
        if (!activateDocumentSourceArtifact(bundleRoot, publication)) {
            qWarning() << "The target generation is active, but its compatibility source pointer could not be refreshed.";
        }
    } else {
        m_libraryPdfDeltaFallbackRequested = false;
    }
    state.insert(QStringLiteral("library"), library);
    state.insert(QStringLiteral("active_library_revision"), generation.value(QStringLiteral("library_revision")));
    state.insert(QStringLiteral("active_artifact_revision"), generation.value(QStringLiteral("artifact_revision")).toString(generation.value(QStringLiteral("library_revision")).toString()));
    state.remove(QStringLiteral("pending_library_generation"));
    state.remove(QStringLiteral("pending_pdf_projections"));
    QJsonObject localRemovalTombstones = state.value(QStringLiteral("local_library_removals")).toObject();
    if (!localRemovalTombstones.isEmpty()) {
        QSet<QString> activePublicationIdentities;
        for (const QJsonValue &value : library.value(QStringLiteral("publications")).toArray()) {
            const QJsonObject publication = value.toObject();
            activePublicationIdentities.insert(publication.value(QStringLiteral("filename")).toString()
                                               + QLatin1Char('\n')
                                               + publication.value(QStringLiteral("sha256")).toString());
        }
        for (auto iterator = localRemovalTombstones.begin(); iterator != localRemovalTombstones.end();) {
            const QString identity = iterator.key() + QLatin1Char('\n') + iterator.value().toObject().value(QStringLiteral("sha256")).toString();
            if (!activePublicationIdentities.contains(identity)) {
                iterator = localRemovalTombstones.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (localRemovalTombstones.isEmpty()) {
            state.remove(QStringLiteral("local_library_removals"));
        } else {
            state.insert(QStringLiteral("local_library_removals"), localRemovalTombstones);
        }
    }
    if (!writeRepositoryState(state)) {
        Q_EMIT libraryGenerationActivated({}, false);
        Q_EMIT connectionStateChanged(false, QStringLiteral("The activated Library generation could not be recorded."));
        return;
    }
    schedulePendingChangesRefresh();
    if (!activateReaderSourceArtifactsForActiveLibrary(library)) {
        qWarning() << "The Library generation is active, but its source-artifact set is not complete yet.";
    }
    if (!recordActivatedReaderGenerationObjects(library)) {
        qWarning() << "The active Library object identities could not be refreshed in the durable cache.";
    }
    Q_EMIT libraryChanged(library);
    QStringList changedPdfs;
    for (const QJsonValue &value : generation.value(QStringLiteral("changed_pdfs")).toArray()) {
        const QString filename = value.toString();
        changedPdfs.append(filename);
        const QString path = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename));
        QString digest;
        for (const QJsonValue &publicationValue : library.value(QStringLiteral("publications")).toArray()) {
            const QJsonObject publication = publicationValue.toObject();
            if (publication.value(QStringLiteral("filename")).toString() == filename) {
                digest = publication.value(QStringLiteral("sha256")).toString();
                break;
            }
        }
        Q_EMIT pdfProjected(filename, path, digest);
    }
    Q_EMIT libraryGenerationActivated(changedPdfs, true);
    Q_EMIT connectionStateChanged(true, QString());
    // Readers have been handed the new generation; remove the superseded
    // directories afterwards. The walk runs on a detached thread rather than
    // the global pool, because QFile/QDir access to a temporarily unavailable
    // File Provider cannot be cancelled and must not block shutdown.
    scheduleLibraryGenerationCleanup(30000);
}

void FedoraRepository::startPendingLibraryActivation()
{
    if (m_libraryPreparing || m_libraryActivating || m_projectingPdf) {
        return;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject generation = state.value(QStringLiteral("pending_library_generation")).toObject();
    if (generation.isEmpty()) {
        return;
    }
    generation.insert(QStringLiteral("activation_started"), true);
    state.insert(QStringLiteral("pending_library_generation"), generation);
    if (!writeRepositoryState(state)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The verified Library activation could not be journaled."));
        return;
    }

    emitReaderStepTiming(QStringLiteral("activation_started"));
    Q_EMIT libraryGenerationWillActivate();
    m_libraryActivating = true;
    const QPointer<FedoraRepository> repository(this);
    QThreadPool::globalInstance()->start([repository, generation] {
        if (!repository) {
            return;
        }
        const ActivatedLibraryGeneration activated = repository->activateLibraryGeneration(generation);
        QMetaObject::invokeMethod(
            repository,
            [repository, activated] {
                if (!repository) {
                    return;
                }
                repository->m_libraryActivating = false;
                if (!activated.success) {
                    QJsonObject state = repository->readRepositoryState();
                    const QJsonObject pending = state.value(QStringLiteral("pending_library_generation")).toObject();
                    if (pending.value(QStringLiteral("library_revision")) == activated.generation.value(QStringLiteral("library_revision")) &&
                        pending.value(QStringLiteral("staging_path")) == activated.generation.value(QStringLiteral("staging_path"))) {
                        state.remove(QStringLiteral("pending_library_generation"));
                        state.remove(QStringLiteral("pending_pdf_projections"));
                        repository->writeRepositoryState(state);
                    }
                    Q_EMIT repository->connectionStateChanged(false, activated.error);
                    Q_EMIT repository->libraryGenerationActivated({}, false);
                    return;
                }
                repository->emitReaderStepTiming(QStringLiteral("activated"));
                repository->finalizeLibraryGeneration(activated.generation);
                repository->emitReaderStepTiming(QStringLiteral("finalized"));
            },
            Qt::QueuedConnection);
    });
}

FedoraRepository::ActivatedLibraryGeneration FedoraRepository::activateLibraryGeneration(const QJsonObject &generation) const
{
    ActivatedLibraryGeneration activated;
    activated.generation = generation;
    const QString current = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs"));
    QString verificationError;
    if (directoryMatchesLibraryGeneration(current, generation, &verificationError)) {
        if (!setGenerationReadOnly(QFileInfo(current).canonicalFilePath())) {
            activated.error = QStringLiteral("The verified Library generation could not be finalized as read-only.");
            return activated;
        }
        activated.generation.insert(QStringLiteral("changed_pdfs"), QJsonArray());
        activated.success = true;
        return activated;
    }

    const QString staging = generation.value(QStringLiteral("staging_path")).toString();
    // Readings can keep document versions that are no longer published; the
    // retained generations still hold those objects.
    QStringList retainedObjects;
    const QString stagingCanonical = QFileInfo(staging).canonicalFilePath();
    for (const QFileInfo &retained : QDir(m_docsRoot).entryInfoList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Time)) {
        if (retained.canonicalFilePath() != stagingCanonical) {
            retainedObjects.append(QDir(retained.absoluteFilePath()).filePath(QStringLiteral("objects")));
        }
    }
    if (!Thoughts::prepareProjectReadings(generation.value(QStringLiteral("library")).toObject(), staging,
            QDir(m_stateRoot).filePath(QStringLiteral("source-bundle/objects")), true, &activated.error, retainedObjects)) return activated;

    if (!writeLibraryGenerationMetadata(generation.value(QStringLiteral("library")).toObject(), staging)) {
        activated.error = QStringLiteral("The pending Library generation metadata could not be repaired.");
        return activated;
    }
    if (!directoryMatchesLibraryGeneration(staging, generation, &verificationError)) {
        activated.error = verificationError;
        return activated;
    }
    if (!setGenerationReadOnly(staging)) {
        activated.error = QStringLiteral("The verified Library generation could not be finalized as read-only.");
        return activated;
    }

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // A stable link (a symlink, or a junction on Windows) keeps open PDFs
    // valid while a complete, verified generation becomes visible through one
    // atomic pointer replacement. A new Library has no generation yet; its
    // first one starts the pointer.
    if (Thoughts::Platform::isDirectoryLink(QFileInfo(current)) || !QFileInfo::exists(current)) {
        QString linkError;
        if (!Thoughts::Platform::replaceDirectoryLink(current, staging, &linkError)) {
            activated.error = QStringLiteral("The verified Library generation pointer could not be activated atomically.");
            if (!linkError.isEmpty()) {
                activated.error += QLatin1Char(' ') + linkError;
            }
            return activated;
        }
        activated.success = true;
        return activated;
    }
#endif
#ifdef Q_OS_WIN
    // current_pdfs became a real folder before the first generation (an
    // import, or the Library index). Windows cannot swap folders that hold
    // open PDFs, so set it aside once, without deleting anything, and start
    // the junction instead.
    const QString legacy = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts-library-legacy-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().rename(current, legacy)) {
        activated.error = QStringLiteral("current_pdfs is in use. Close the PDFs opened from it, then Sync again.");
        return activated;
    }
    QString linkError;
    if (!Thoughts::Platform::replaceDirectoryLink(current, staging, &linkError)) {
        QDir().rename(legacy, current);
        activated.error = QStringLiteral("The verified Library generation pointer could not be created: %1").arg(linkError);
        return activated;
    }
    activated.success = true;
    return activated;
#endif

    if (!swapDirectories(current, staging)) {
        activated.error = QStringLiteral("The verified Library generation could not be activated atomically.");
        return activated;
    }
    if (!directoryMatchesLibraryGeneration(current, generation, &verificationError)) {
        swapDirectories(current, staging);
        activated.error = QStringLiteral("The activated Library generation failed verification: %1").arg(verificationError);
        return activated;
    }

    const QString previous = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts-library-previous"));
    if (QFileInfo::exists(previous)) {
        removeReadOnlyGeneration(previous);
    }
    QDir().rename(staging, previous);
    activated.success = true;
    return activated;
}

void FedoraRepository::importLibraryPdf(const QString &requestId, const QString &sourcePath, const QString &filename)
{
    if (requestId.isEmpty() || !safePdfFilename(filename) || m_importingLibrary) {
        Q_EMIT libraryImportFinished(requestId, filename, QString(), QString(), false, 0, m_importingLibrary ? QStringLiteral("Another Library import is already running.") : QStringLiteral("The Library request is invalid."));
        return;
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        Q_EMIT libraryImportFinished(requestId, filename, QString(), QString(), false, 0, source.errorString());
        return;
    }
    const QByteArray content = source.readAll();
    if (!content.startsWith("%PDF-") || content.isEmpty()) {
        Q_EMIT libraryImportFinished(requestId, filename, QString(), QString(), false, 0, QStringLiteral("The selected file is not a PDF."));
        return;
    }
    const QString digest = QString::fromLatin1(Thoughts::SyncClient::sha256(content));
    const QString destination = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename));
    if (QFileInfo::exists(destination) && QString::fromLatin1(fileSha256(destination)) != digest) {
        Q_EMIT libraryImportFinished(requestId, filename, QString(), digest, false, 409, QStringLiteral("A different PDF with this filename is already in the local Library."));
        return;
    }

    m_importingLibrary = true;
    bool activated = QDir().mkpath(QFileInfo(destination).absolutePath());
    if (activated && !QFileInfo::exists(destination)) {
        QSaveFile file(destination);
        file.setDirectWriteFallback(false);
        activated = file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit();
    }
    if (activated) {
        activated = QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    QString registrationError;
    const auto documentId = activated ? Thoughts::DocumentIdentityStore(m_docsRoot).registerImport(filename, digest, destination, &registrationError) : QString();
    if (!activated || documentId.isEmpty() || !patchLibraryIndexEntry(filename, digest, QStringLiteral("unlinked"), false) ||
        !enqueueLibraryMutation(QJsonObject {
            {QStringLiteral("type"), QStringLiteral("add")}, {QStringLiteral("document_id"), documentId}, {QStringLiteral("request_id"), requestId}, {QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), digest}, {QStringLiteral("local_path"), destination}})) {
        m_importingLibrary = false;
        Q_EMIT libraryImportFinished(requestId, filename, QString(), digest, false, 0, QStringLiteral("The PDF could not be added to the durable local Library queue."));
        return;
    }
    m_importingLibrary = false;
    Q_EMIT libraryImportFinished(requestId, filename, destination, digest, true, 200, QString());
}

void FedoraRepository::setLibraryDisplayName(const QString &requestId, const QString &filename, const QString &expectedSha256, const QString &displayName)
{
    const QString normalized = displayName.trimmed();
    const bool validName = !normalized.isEmpty() && normalized.size() <= 200 && std::none_of(normalized.cbegin(), normalized.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; });
    if (requestId.isEmpty() || !safePdfFilename(filename) || !validSha256(expectedSha256) || !validName || m_renamingLibrary) {
        Q_EMIT libraryDisplayNameFinished(requestId, filename, expectedSha256, normalized, false, m_renamingLibrary ? QStringLiteral("Another Library rename is already running.") : QStringLiteral("The Library rename is invalid."));
        return;
    }
    m_renamingLibrary = true;
    const bool success = patchLibraryDisplayName(filename, expectedSha256, normalized) &&
        enqueueLibraryMutation(QJsonObject {
            {QStringLiteral("type"), QStringLiteral("rename")}, {QStringLiteral("request_id"), requestId}, {QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), expectedSha256}, {QStringLiteral("display_name"), normalized}});
    m_renamingLibrary = false;
    Q_EMIT libraryDisplayNameFinished(requestId, filename, expectedSha256, normalized, success, success ? QString() : QStringLiteral("The display name could not be added to the durable local Library queue."));
}

void FedoraRepository::removeLibraryPdf(const QString &requestId, const QString &filename, const QString &expectedSha256)
{
    if (requestId.isEmpty() || !safePdfFilename(filename) || !validSha256(expectedSha256) || m_removingLibrary) {
        Q_EMIT libraryRemovalFinished(requestId, filename, expectedSha256, false, m_removingLibrary ? QStringLiteral("Another Library removal is already running.") : QStringLiteral("The Library removal is invalid."));
        return;
    }
    m_removingLibrary = true;
    QJsonObject state = readRepositoryState();
    QJsonObject localRemovalTombstones = state.value(QStringLiteral("local_library_removals")).toObject();
    const QJsonValue previousTombstone = localRemovalTombstones.value(filename);
    localRemovalTombstones.insert(filename,
                                  QJsonObject {{QStringLiteral("request_id"), requestId},
                                               {QStringLiteral("sha256"), expectedSha256},
                                               {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    state.insert(QStringLiteral("local_library_removals"), localRemovalTombstones);
    const bool tombstoneStored = writeRepositoryState(state);
    // current_pdfs points at a verified immutable generation.  The local
    // membership index stays unchanged for open readers and recovery.  The
    // durable tombstone is the immediate local Library projection; manual
    // Sync publishes it and activates a complete replacement generation.
    const bool success = tombstoneStored && enqueueLibraryMutation(
        QJsonObject {{QStringLiteral("type"), QStringLiteral("remove")}, {QStringLiteral("request_id"), requestId}, {QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), expectedSha256}});
    if (!success && tombstoneStored) {
        state = readRepositoryState();
        localRemovalTombstones = state.value(QStringLiteral("local_library_removals")).toObject();
        const QJsonObject currentTombstone = localRemovalTombstones.value(filename).toObject();
        if (currentTombstone.value(QStringLiteral("request_id")).toString() == requestId
            && currentTombstone.value(QStringLiteral("sha256")).toString() == expectedSha256) {
            if (previousTombstone.isUndefined()) {
                localRemovalTombstones.remove(filename);
            } else {
                localRemovalTombstones.insert(filename, previousTombstone);
            }
            if (localRemovalTombstones.isEmpty()) {
                state.remove(QStringLiteral("local_library_removals"));
            } else {
                state.insert(QStringLiteral("local_library_removals"), localRemovalTombstones);
            }
            writeRepositoryState(state);
        }
    }
    m_removingLibrary = false;
    Q_EMIT libraryRemovalFinished(requestId, filename, expectedSha256, success, success ? QString() : QStringLiteral("The removal could not be added to the durable local Library queue."));
}

void FedoraRepository::projectNextPdf()
{
    if (!m_client || m_libraryPreparing || m_libraryActivating || m_projectingPdf) {
        return;
    }
    QJsonObject state = readRepositoryState();
    const QJsonObject generation = state.value(QStringLiteral("pending_library_generation")).toObject();
    if (generation.isEmpty()) {
        return;
    }
    if (generation.value(QStringLiteral("activation_started")).toBool()) {
        startPendingLibraryActivation();
        return;
    }
    const QJsonArray pending = state.value(QStringLiteral("pending_pdf_projections")).toArray();
    if (pending.isEmpty()) {
        startPendingLibraryActivation();
        return;
    }
    const QJsonObject projection = pending.first().toObject();
    const QString type = projection.value(QStringLiteral("type")).toString();
    const QString filename = projection.value(QStringLiteral("filename")).toString();
    const QByteArray digest = projection.value(QStringLiteral("sha256")).toString().toLatin1();
    const bool validItem = (type == QLatin1String("pdf") && safePdfFilename(filename)) || (type == QLatin1String("source_index") && filename == QLatin1String(".thoughts-source-index.json"));
    if (!validItem || !validSha256(QString::fromLatin1(digest))) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The pending Library projection journal is invalid."));
        return;
    }
    const QString plannedKind = type == QLatin1String("pdf") ? QStringLiteral("pdf") : QStringLiteral("source_index");
    const QString publicationFilename = type == QLatin1String("pdf") ? filename : QString();
    const qint64 expectedSize = type == QLatin1String("pdf") ? projection.value(QStringLiteral("size_bytes")).toInteger(-1) : readerPlanObjectSize(m_libraryReaderPlan, QString::fromLatin1(digest));
    QByteArray transferDigest = digest;
    qint64 transferSize = expectedSize;
    QString plannedRole = plannedKind;
    std::optional<PdfDeltaPlan> delta;
    if (type == QLatin1String("pdf") && readerPlanHasPdfDelta(m_libraryReaderPlan, filename)) {
        const QJsonObject publication = publicationForFilename(generation.value(QStringLiteral("library")).toObject(), filename);
        delta = pdfDeltaPlanForPublication(m_libraryReaderPlan, publication);
        if (!delta || delta->resultSha256 != QString::fromLatin1(digest) || delta->resultSize != expectedSize) {
            retryGlobalReaderPlanWithoutPdfDelta(QStringLiteral("The planned PDF delta did not match the authoritative Library publication."));
            return;
        }
        transferDigest = delta->patchSha256.toLatin1();
        transferSize = delta->patchSize;
        plannedRole = QStringLiteral("pdf_delta");
    }
    if (!readerPlanAllowsObject(m_libraryReaderPlan, QString::fromLatin1(transferDigest), plannedKind, plannedRole, publicationFilename, transferSize)) {
        if (delta) {
            retryGlobalReaderPlanWithoutPdfDelta(QStringLiteral("The planned PDF delta escaped its reader synchronization allowlist."));
        } else if (m_libraryReaderObjectRetries++ < 1) {
            m_libraryReaderPlan = {};
            QTimer::singleShot(0, this, &FedoraRepository::refreshLibrary);
        } else {
            m_libraryReaderObjectRetries = 0;
            Q_EMIT connectionStateChanged(false, QStringLiteral("The Library download no longer matches its reader synchronization plan."));
        }
        return;
    }
    m_projectingPdf = true;
    const QString requestedPlanId = m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString();
    QNetworkReply *reply = m_client->downloadObject(transferDigest, requestedPlanId.toLatin1());
    connect(reply, &QNetworkReply::downloadProgress, this, [this, transferDigest, transferSize](qint64 received, qint64) {
        accountReaderObjectProgress(QString::fromLatin1(transferDigest), qMin(received, transferSize));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, projection, type, filename, digest, expectedSize, transferDigest, transferSize, delta, requestedPlanId] {
        const QString message = reply->errorString();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool networkSuccess = reply->error() == QNetworkReply::NoError;
        noteReplyFailure(reply);
        QByteArray content = networkSuccess ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != requestedPlanId) {
            m_projectingPdf = false;
            return;
        }
        if (!networkSuccess) {
            m_projectingPdf = false;
            accountReaderObjectProgress(QString::fromLatin1(transferDigest), 0, false, true);
            if (httpStatus == 409 && m_libraryReaderObjectRetries++ < 1) {
                m_libraryReaderPlan = {};
                QTimer::singleShot(0, this, &FedoraRepository::refreshLibrary);
                return;
            }
            m_libraryReaderObjectRetries = 0;
            Q_EMIT connectionStateChanged(false, message);
            return;
        }
        m_libraryReaderObjectRetries = 0;
        const QString destination = QDir(generation.value(QStringLiteral("staging_path")).toString()).filePath(filename);
        const QPointer<FedoraRepository> repository(this);
        QThreadPool::globalInstance()->start([repository, generation, projection, type, filename, destination, digest, expectedSize, transferDigest, transferSize, delta, requestedPlanId, content = std::move(content)]() mutable {
            bool success = false;
            bool deltaVerified = !delta.has_value();
            QString error;
            QByteArray result;
            if (repository && content.size() == transferSize && Thoughts::SyncClient::verifyObject(content, transferDigest)) {
                result = std::move(content);
                if (delta) {
                    result = Internal::reconstructVerifiedPdfDelta(repository->m_docsRoot, filename, delta->baseSha256, result, delta->resultSha256, delta->resultSize);
                    deltaVerified = !result.isEmpty();
                }
                const bool contentValid = deltaVerified && expectedSize == result.size() && Thoughts::SyncClient::verifyObject(result, digest);
                const bool indexValid = type != QLatin1String("source_index") || repository->validateSourceIndex(result, generation.value(QStringLiteral("library")).toObject(), &error);
                if (contentValid && indexValid && QDir().mkpath(QFileInfo(destination).absolutePath())) {
                    QSaveFile file(destination);
                    file.setDirectWriteFallback(false);
                    success = file.open(QIODevice::WriteOnly) && file.write(result) == result.size() && file.commit();
                    if (success && type == QLatin1String("pdf")) {
                        const QString current = QDir(repository->m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(projection.value(QStringLiteral("filename")).toString()));
                        success = preserveMacDocumentAccess(current, destination);
                    }
                    if (success) {
                        success = QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
                    }
                }
            }
            if (!success && error.isEmpty()) {
                error = delta && !deltaVerified ? QStringLiteral("The PDF delta or its active base failed exact identity verification.") : QStringLiteral("The server returned a Library object with the wrong identity.");
            }
            QMetaObject::invokeMethod(
                repository,
                [repository, generation, projection, type, destination, digest, expectedSize, transferDigest, transferSize, delta, requestedPlanId, success, deltaVerified, error] {
                    if (!repository) {
                        return;
                    }
                    repository->m_projectingPdf = false;
                    if (repository->m_libraryReaderPlan.value(QStringLiteral("plan_id")).toString() != requestedPlanId) {
                        QTimer::singleShot(0, repository, &FedoraRepository::projectNextPdf);
                        return;
                    }
                    if (!success) {
                        repository->accountReaderObjectProgress(QString::fromLatin1(transferDigest), 0, false, true);
                        if (delta && !deltaVerified) {
                            repository->retryGlobalReaderPlanWithoutPdfDelta(error);
                        } else {
                            Q_EMIT repository->connectionStateChanged(false, error);
                        }
                        return;
                    }
                    repository->recordVerifiedReaderObject(destination,
                                                           QString::fromLatin1(digest),
                                                           expectedSize,
                                                           type == QLatin1String("pdf") ? QStringLiteral("pdf") : QStringLiteral("source_index"),
                                                           type == QLatin1String("pdf") ? QStringLiteral("pdf") : QStringLiteral("source_index"),
                                                           type == QLatin1String("pdf") ? projection.value(QStringLiteral("filename")).toString() : QString());
                    repository->accountReaderObjectProgress(QString::fromLatin1(transferDigest), transferSize, true);

                    QJsonObject state = repository->readRepositoryState();
                    const QJsonObject currentGeneration = state.value(QStringLiteral("pending_library_generation")).toObject();
                    const QJsonArray pending = state.value(QStringLiteral("pending_pdf_projections")).toArray();
                    if (currentGeneration.value(QStringLiteral("library_revision")) != generation.value(QStringLiteral("library_revision")) || pending.isEmpty() || pending.first().toObject() != projection) {
                        QTimer::singleShot(0, repository, &FedoraRepository::projectNextPdf);
                        return;
                    }
                    QJsonArray retained;
                    for (qsizetype index = 1; index < pending.size(); ++index) {
                        retained.append(pending.at(index));
                    }
                    state.insert(QStringLiteral("pending_pdf_projections"), retained);
                    if (!repository->writeRepositoryState(state)) {
                        Q_EMIT repository->connectionStateChanged(false, QStringLiteral("The verified Library download could not be journaled."));
                        return;
                    }
                    Q_EMIT repository->connectionStateChanged(true, QStringLiteral("Preparing a verified Library generation."));
                    QTimer::singleShot(0, repository, &FedoraRepository::projectNextPdf);
                },
                Qt::QueuedConnection);
        });
    });
}

void FedoraRepository::watchTrace(const QString &traceId, qint64 afterSequence)
{
    if (!ensureClient() || traceId.isEmpty() || m_explicitTraceReplies.contains(traceId)) {
        return;
    }
    m_stoppedTraceWatches.remove(traceId);
    auto parser = std::make_shared<Thoughts::TraceSseParser>();
    auto cursor = std::make_shared<qint64>(qMax<qint64>(0, afterSequence));
    auto terminal = std::make_shared<bool>(false);
    QNetworkReply *reply = m_client->traceStream(traceId, *cursor);
    m_explicitTraceReplies.insert(traceId, reply);
    Q_EMIT syncJournalEvent(traceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("live")}});
    const auto consume = [this, reply, parser, cursor, terminal, traceId](bool flush) {
        for (const QJsonObject &record : takeTraceEvents(*parser, reply->readAll(), flush)) {
            if (record.value(QStringLiteral("trace_id")).toString() != traceId) {
                continue;
            }
            const qint64 sequence = record.value(QStringLiteral("sequence")).toVariant().toLongLong();
            if (sequence <= *cursor) {
                continue;
            }
            Q_EMIT syncJournalEvent(traceId, record);
            *cursor = sequence;
            *terminal = record.value(QStringLiteral("terminal")).toBool();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [consume] { consume(false); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, consume, cursor, terminal, traceId] {
        consume(true);
        const bool stopped = m_stoppedTraceWatches.remove(traceId);
        m_explicitTraceReplies.remove(traceId);
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : replyError(reply);
        reply->deleteLater();
        if (*terminal || stopped) {
            m_explicitTraceAttempts.remove(traceId);
            Q_EMIT syncJournalEvent(traceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("offline")}});
            return;
        }
        const int attempt = m_explicitTraceAttempts.value(traceId) + 1;
        m_explicitTraceAttempts.insert(traceId, attempt);
        Q_EMIT syncJournalEvent(traceId, QJsonObject {{QStringLiteral("stream_state"), attempt <= 3 ? QStringLiteral("reconnecting") : QStringLiteral("offline")}, {QStringLiteral("message"), error}});
        if (attempt <= 3) {
            static constexpr int backoffMs[] {1000, 2000, 4000};
            const int jitter = QRandomGenerator::global()->bounded(251);
            QTimer::singleShot(backoffMs[attempt - 1] + jitter, this, [this, traceId, cursor] {
                if (!m_stoppedTraceWatches.contains(traceId)) {
                    watchTrace(traceId, *cursor);
                }
            });
        }
    });
}

void FedoraRepository::stopWatchingTrace(const QString &traceId)
{
    m_stoppedTraceWatches.insert(traceId);
    if (QNetworkReply *reply = m_explicitTraceReplies.value(traceId)) {
        reply->abort();
    } else {
        Q_EMIT syncJournalEvent(traceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("offline")}});
    }
}

void FedoraRepository::startBuildTraceStream(const QString &buildId, const QString &target, const QString &streamTraceId, const QString &operationTraceId, qint64 afterSequence)
{
    if (!m_client || m_pollingBuild || streamTraceId.isEmpty() || operationTraceId.isEmpty()) {
        return;
    }
    m_pollingBuild = true;
    m_buildTraceParser = std::make_unique<Thoughts::TraceSseParser>();
    auto cursor = std::make_shared<QJsonObject>(readBuildTraceCursor(m_stateRoot, streamTraceId, buildId, target));
    afterSequence = qMax(afterSequence, cursor->value(QStringLiteral("last_server_sequence")).toVariant().toLongLong());
    QNetworkReply *reply = m_client->traceStream(streamTraceId, afterSequence);
    m_buildTraceReply = reply;
    Q_EMIT syncJournalEvent(operationTraceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("live")}});
    struct CursorSave { QElapsedTimer clock; bool unsaved = false; };
    auto cursorSaved = std::make_shared<CursorSave>();
    const auto consume = [this, reply, cursor, cursorSaved, buildId, target, streamTraceId, operationTraceId](bool flush) {
        if (!m_buildTraceParser) {
            return;
        }
        // Journal rows are committed per event; a crash before the cursor is saved
        // only replays events that the journal and run sequence already skip.
        std::optional<QJsonObject> pendingState;
        const auto saveCursor = [&] {
            // Events usually arrive one per read; save at most once a second and
            // always when the stream ends.
            if (!cursorSaved->unsaved || (!flush && cursorSaved->clock.isValid() && cursorSaved->clock.elapsed() < 1000)) return;
            cursorSaved->clock.start();
            cursorSaved->unsaved = false;
            if (!writeJsonFile(buildTraceCursorPath(m_stateRoot, streamTraceId), QJsonDocument(*cursor).toJson(QJsonDocument::Compact) + '\n')) {
                // Preserve the durable replay cursor if the small record cannot be saved.
                // The cache already holds this state, so compare against the file.
                const QJsonObject state = pendingState ? *pendingState : readRepositoryState();
                m_cachedRepositoryStateValid = false;
                writeRepositoryState(state);
            }
        };
        const auto finishBatch = qScopeGuard(saveCursor);
        for (const QJsonObject &record : takeTraceEvents(*m_buildTraceParser, reply->readAll(), flush)) {
            if (record.value(QStringLiteral("trace_id")).toString() != streamTraceId) {
                continue;
            }
            QJsonObject state = readRepositoryState();
            QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
            QJsonObject run = runs.value(target).toObject();
            const qint64 sequence = record.value(QStringLiteral("sequence")).toVariant().toLongLong();
            if (sequence <= qMax(run.value(QStringLiteral("last_server_sequence")).toVariant().toLongLong(),
                                 cursor->value(QStringLiteral("last_server_sequence")).toVariant().toLongLong())) {
                continue;
            }
            // The Shell connection is direct, so the SQLite journal commit completes
            // before this durable replay cursor advances.
            Q_EMIT syncJournalEvent(operationTraceId, record);
            run.insert(QStringLiteral("last_server_sequence"), sequence);
            run.insert(QStringLiteral("server_state"), record.value(QStringLiteral("state")));
            run.insert(QStringLiteral("server_terminal"), record.value(QStringLiteral("terminal")));
            run.insert(QStringLiteral("stream_state"), QStringLiteral("live"));
            run.insert(QStringLiteral("transient_attempts"), 0);
            runs.insert(target, run);
            state.insert(QStringLiteral("build_runs"), runs);
            *cursor = {{QStringLiteral("trace_id"), streamTraceId},
                       {QStringLiteral("build_id"), buildId},
                       {QStringLiteral("target"), target},
                       {QStringLiteral("last_server_sequence"), sequence},
                       {QStringLiteral("server_state"), record.value(QStringLiteral("state"))},
                       {QStringLiteral("server_terminal"), record.value(QStringLiteral("terminal"))}};
            // Other repository operations in this process see the current run; the
            // durable cursor is saved once per received batch below.
            m_cachedRepositoryState = state;
            pendingState = state;
            cursorSaved->unsaved = true;
            emitSyncProgress(record.value(QStringLiteral("phase")).toString(),
                             record.value(QStringLiteral("bytes_completed")).toVariant().toLongLong(),
                             record.value(QStringLiteral("bytes_total")).toVariant().toLongLong(),
                             record.value(QStringLiteral("message")).toString());
            if (record.value(QStringLiteral("terminal")).toBool() && record.value(QStringLiteral("build_id")).toString() == buildId) {
                m_terminalTraceBuilds.insert(buildId);
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [consume] { consume(false); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, consume, cursor, buildId, target, streamTraceId, operationTraceId] {
        consume(true);
        const bool terminal = m_terminalTraceBuilds.contains(buildId);
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : replyError(reply);
        reply->deleteLater();
        m_buildTraceReply.clear();
        m_buildTraceParser.reset();
        m_pollingBuild = false;
        QJsonObject state = readRepositoryState();
        QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
        QJsonObject run = runs.value(target).toObject();
        if (cursor->value(QStringLiteral("last_server_sequence")).toVariant().toLongLong()
            > run.value(QStringLiteral("last_server_sequence")).toVariant().toLongLong()) {
            run.insert(QStringLiteral("last_server_sequence"), cursor->value(QStringLiteral("last_server_sequence")));
            run.insert(QStringLiteral("server_state"), cursor->value(QStringLiteral("server_state")));
            run.insert(QStringLiteral("server_terminal"), cursor->value(QStringLiteral("server_terminal")));
            run.insert(QStringLiteral("transient_attempts"), 0);
        }
        if (!terminal && httpStatus == 404) {
            // The server has no such trace: waiting does not create one (the three retries
            // cost 7 s before the build was first asked about). Ask about the build itself
            // at once; pollBuilds returns to the stream only if the build names this trace.
            run.insert(QStringLiteral("trace_stream_missing"), true);
            run.insert(QStringLiteral("stream_state"), QStringLiteral("offline"));
            runs.insert(target, run);
            state.insert(QStringLiteral("build_runs"), runs);
            m_cachedRepositoryStateValid = false; // The cache already holds the streamed run; compare against the file.
            if (writeRepositoryState(state)) QFile::remove(buildTraceCursorPath(m_stateRoot, streamTraceId));
            Q_EMIT syncJournalEvent(operationTraceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("offline")}});
            QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
            return;
        }
        if (!terminal) {
            const int attempt = run.value(QStringLiteral("transient_attempts")).toInt() + 1;
            run.insert(QStringLiteral("transient_attempts"), attempt);
            run.insert(QStringLiteral("stream_state"), attempt <= 3 ? QStringLiteral("reconnecting") : QStringLiteral("offline"));
            if (!error.isEmpty()) {
                run.insert(QStringLiteral("last_network_error"), error);
            }
            if (attempt > 3) {
                run.insert(QStringLiteral("trace_stream_disabled"), true);
            }
            runs.insert(target, run);
            state.insert(QStringLiteral("build_runs"), runs);
            m_cachedRepositoryStateValid = false; // The cache already holds the streamed run; compare against the file.
            if (writeRepositoryState(state)) QFile::remove(buildTraceCursorPath(m_stateRoot, streamTraceId));
            Q_EMIT syncJournalEvent(operationTraceId, QJsonObject {{QStringLiteral("stream_state"), attempt <= 3 ? QStringLiteral("reconnecting") : QStringLiteral("offline")}});
            if (attempt <= 3) {
                static constexpr int backoffMs[] {1000, 2000, 4000};
                QTimer::singleShot(backoffMs[attempt - 1] + QRandomGenerator::global()->bounded(251), this, &FedoraRepository::pollBuilds);
            } else {
                QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
            }
            return;
        }
        runs.insert(target, run);
        state.insert(QStringLiteral("build_runs"), runs);
        m_cachedRepositoryStateValid = false; // As above: the cached run is not on disk yet.
        if (writeRepositoryState(state)) QFile::remove(buildTraceCursorPath(m_stateRoot, streamTraceId));
        Q_EMIT syncJournalEvent(operationTraceId, QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("offline")}});
        QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
    });
}

QJsonObject FedoraRepository::preferredActiveBuildForPolling(const QJsonArray &builds, const QJsonObject &runs) const
{
    if (m_syncCoordinator.running() && m_syncCoordinator.phase == ManualSyncPhase::WaitBuild) {
        for (const QJsonValue &value : builds) {
            const QJsonObject active = value.toObject();
            const QJsonObject run = runs.value(active.value(QStringLiteral("target")).toString()).toObject();
            if (manualConditionalBuildOwnsCurrentSync(run)) {
                return active;
            }
        }
    }
    return builds.isEmpty() ? QJsonObject() : builds.first().toObject();
}

void FedoraRepository::pollBuilds()
{
    if (!m_client || m_pollingBuild) {
        return;
    }
    QJsonObject state = readRepositoryState();
    const QJsonArray builds = state.value(QStringLiteral("active_builds")).toArray();
    if (builds.isEmpty()) {
        return;
    }
    const QJsonObject active = preferredActiveBuildForPolling(builds, state.value(QStringLiteral("build_runs")).toObject());
    const QString buildId = active.value(QStringLiteral("id")).toString();
    const QString target = active.value(QStringLiteral("target")).toString();
    if (!safeComponent(buildId)) {
        return;
    }
    const QJsonObject activeRun = state.value(QStringLiteral("build_runs")).toObject().value(target).toObject();
    const QString operationTraceId = Internal::operationTraceIdForBuildRun(activeRun, m_activeTraceId);
    applyTraceId(operationTraceId);
    if (!m_traceCapabilityKnown) {
        m_pollingBuild = true;
        QNetworkReply *capabilitiesReply = m_client->ready();
        connect(capabilitiesReply, &QNetworkReply::finished, this, [this, capabilitiesReply] {
            m_pollingBuild = false;
            if (capabilitiesReply->error() == QNetworkReply::NoError) {
                const QJsonArray capabilities = QJsonDocument::fromJson(capabilitiesReply->readAll()).object().value(QStringLiteral("capabilities")).toArray();
                m_traceEventsSupported = std::any_of(capabilities.cbegin(), capabilities.cend(), [](const QJsonValue &value) { return value.toString() == QLatin1String("trace_events_v1"); });
            }
            m_traceCapabilityKnown = true;
            capabilitiesReply->deleteLater();
            QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
        });
        return;
    }
    const bool fetchTerminalBuild = m_terminalTraceBuilds.remove(buildId);
    // The stream answered 404: this one request is not a long poll, so the build's own
    // answer decides at once between the stream and waiting on the build.
    const bool streamMissing = activeRun.value(QStringLiteral("trace_stream_missing")).toBool();
    if (m_traceEventsSupported && !fetchTerminalBuild && !streamMissing && !activeRun.value(QStringLiteral("trace_stream_disabled")).toBool()) {
        startBuildTraceStream(buildId, target, operationTraceId, operationTraceId, activeRun.value(QStringLiteral("last_server_sequence")).toVariant().toLongLong());
        return;
    }
    emitSyncProgress(QStringLiteral("building"), 0, 0, QStringLiteral("Waiting for the server build"));
    m_pollingBuild = true;
    QNetworkReply *reply = m_client->build(buildId, fetchTerminalBuild || streamMissing ? 0 : 30000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, buildId, target, operationTraceId] {
        m_pollingBuild = false;
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = replyError(reply);
            reply->deleteLater();
            QJsonObject state = readRepositoryState();
            QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
            QJsonObject run = runs.value(target).toObject();
            const int attempt = run.value(QStringLiteral("transient_attempts")).toInt() + 1;
            run.insert(QStringLiteral("transient_attempts"), attempt);
            run.insert(QStringLiteral("last_network_error"), error);
            runs.insert(target, run);
            state.insert(QStringLiteral("build_runs"), runs);
            writeRepositoryState(state);
            Q_EMIT connectionStateChanged(false, error);
            if (attempt <= 3) {
                static constexpr int backoffMs[] {1000, 2000, 4000};
                const int jitter = QRandomGenerator::global()->bounded(251);
                QTimer::singleShot(backoffMs[attempt - 1] + jitter, this, &FedoraRepository::pollBuilds);
            } else {
                // Losing the live transport does not change Fedora's build state.
                // Keep the local execution in network-waiting mode and retry the
                // authoritative build record without submitting replacement work.
                Q_EMIT syncJournalEvent(m_activeTraceId,
                                        QJsonObject {{QStringLiteral("stream_state"), QStringLiteral("offline")}, {QStringLiteral("message"), QStringLiteral("The server is temporarily unreachable; the build may still be running.")}});
                QTimer::singleShot(30000, this, &FedoraRepository::pollBuilds);
            }
            return;
        }
        bool terminal = false;
        bool succeeded = false;
        QString terminalMessage;
        QJsonObject build;
        build = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = Internal::authoritativeBuildStatus(build);
        terminal = status == QLatin1String("succeeded") || status == QLatin1String("attention") || status == QLatin1String("failed") || status == QLatin1String("cancelled");
        const QJsonObject result = build.value(QStringLiteral("result")).toObject();
        const QJsonArray failedDocuments = result.value(QStringLiteral("failed_documents")).toArray();
        const QJsonObject observedRun = readRepositoryState().value(QStringLiteral("build_runs")).toObject().value(target).toObject();
        const bool manualConditional = observedRun.value(QStringLiteral("sync_origin")).toString() == QLatin1String("manual_conditional");
        succeeded = manualConditional ? status == QLatin1String("succeeded") : status == QLatin1String("succeeded") || (status == QLatin1String("attention") && failedDocuments.isEmpty());
        terminalMessage = build.value(QStringLiteral("error")).toString().trimmed();
        if (terminalMessage.isEmpty() && !failedDocuments.isEmpty()) {
            terminalMessage = failedDocuments.first().toObject().value(QStringLiteral("message")).toString().trimmed();
        }
        if (terminal && !succeeded && terminalMessage.isEmpty()) {
            terminalMessage = QStringLiteral("The server could not publish the requested PDF; the last-good Library PDF remains active.");
        }
        if (terminal) {
            QJsonObject state = readRepositoryState();
            QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
            QJsonObject completedRun = runs.value(target).toObject();
            const bool completedManualConditional = completedRun.value(QStringLiteral("sync_origin")).toString() == QLatin1String("manual_conditional");
            const bool fullLibraryActivation = completedRun.value(QStringLiteral("first_publication")).toBool();
            const QJsonObject publication = completedManualConditional ? QJsonObject() : publicationIdentityForBuild(target, build, currentPublicationForTarget(state, target));
            QJsonArray retained;
            for (const QJsonValue &value : state.value(QStringLiteral("active_builds")).toArray()) {
                if (value.toObject().value(QStringLiteral("id")).toString() != buildId) {
                    retained.append(value);
                }
            }
            state.insert(QStringLiteral("active_builds"), retained);
            if (succeeded && !completedManualConditional) {
                completedRun.insert(QStringLiteral("status"), QStringLiteral("activating"));
                completedRun.insert(QStringLiteral("build"), build);
                completedRun.insert(QStringLiteral("publication"), publication);
                runs.insert(target, completedRun);
            } else {
                runs.remove(target);
            }
            state.insert(QStringLiteral("build_runs"), runs);
            const bool journaled = writeRepositoryState(state);
            if (completedManualConditional) {
                applyManualConditionalBuildTerminalResult(completedRun, target, build, succeeded, terminalMessage, journaled);
            } else if (succeeded && safePdfFilename(target)) {
                m_targetedBuildTarget = target;
                activateTargetedBuildArtifact(target, build, fullLibraryActivation);
            } else if (!succeeded) {
                m_targetedBuildTarget.clear();
                setLiveRenderStatus(
                    target,
                    QJsonObject {{QStringLiteral("status"), status == QLatin1String("attention") ? QStringLiteral("attention") : QStringLiteral("error")}, {QStringLiteral("reason"), terminalMessage}, {QStringLiteral("build"), build}});
                Q_EMIT renderStatusChanged(target, m_renderStatuses.value(target));
                Q_EMIT connectionStateChanged(false, terminalMessage);
            }
            QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
        } else {
            if (build.isEmpty()) {
                // An answer that is no build record cannot send the stream question again
                // at once: from here the build is waited on with long polls.
                QJsonObject state = readRepositoryState();
                QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
                QJsonObject activeRun = runs.value(target).toObject();
                if (activeRun.value(QStringLiteral("trace_stream_missing")).toBool()) {
                    activeRun.remove(QStringLiteral("trace_stream_missing"));
                    activeRun.insert(QStringLiteral("trace_stream_disabled"), true);
                    runs.insert(target, activeRun);
                    state.insert(QStringLiteral("build_runs"), runs);
                    writeRepositoryState(state);
                }
            }
            if (!build.isEmpty()) {
                QJsonObject state = readRepositoryState();
                QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
                QJsonObject activeRun = runs.value(target).toObject();
                if (!activeRun.isEmpty()) {
                    const QString nextStatus = build.value(QStringLiteral("status")).toString();
                    const QJsonObject nextProgress = build.value(QStringLiteral("progress")).toObject();
                    const bool streamMissing = activeRun.value(QStringLiteral("trace_stream_missing")).toBool();
                    if (streamMissing) {
                        // Back to the stream once, and only if the build names the trace
                        // that was asked for; otherwise the build is waited on directly.
                        activeRun.remove(QStringLiteral("trace_stream_missing"));
                        const bool buildNamesTrace = !operationTraceId.isEmpty() && build.value(QStringLiteral("trace_id")).toString() == operationTraceId;
                        if (buildNamesTrace && !activeRun.value(QStringLiteral("trace_stream_retried")).toBool()) {
                            activeRun.insert(QStringLiteral("trace_stream_retried"), true);
                        } else {
                            activeRun.insert(QStringLiteral("trace_stream_disabled"), true);
                        }
                    }
                    if (streamMissing || activeRun.value(QStringLiteral("status")).toString() != nextStatus || activeRun.value(QStringLiteral("progress")).toObject() != nextProgress || activeRun.value(QStringLiteral("transient_attempts")).toInt() != 0) {
                        activeRun.insert(QStringLiteral("status"), nextStatus);
                        activeRun.insert(QStringLiteral("progress"), nextProgress);
                        activeRun.insert(QStringLiteral("transient_attempts"), 0);
                        activeRun.insert(QStringLiteral("last_heartbeat_at"), build.value(QStringLiteral("updated_at")));
                        activeRun.remove(QStringLiteral("last_network_error"));
                        runs.insert(target, activeRun);
                        state.insert(QStringLiteral("build_runs"), runs);
                        writeRepositoryState(state);
                    }
                }
                if (safePdfFilename(target) && target == m_targetedBuildTarget) {
                    const QJsonObject nextRenderStatus {{QStringLiteral("status"), QStringLiteral("building")}, {QStringLiteral("build"), build}};
                    if (m_renderStatuses.value(target) != nextRenderStatus) {
                        setLiveRenderStatus(target, nextRenderStatus);
                        Q_EMIT renderStatusChanged(target, nextRenderStatus);
                    }
                }
            }
            QTimer::singleShot(0, this, &FedoraRepository::pollBuilds);
        }
        reply->deleteLater();
    });
}

void FedoraRepository::activateTargetedBuildArtifact(const QString &target, const QJsonObject &build, bool fullLibraryActivation)
{
    Q_UNUSED(fullLibraryActivation)
    if (!m_client || !safePdfFilename(target)) {
        m_targetedBuildTarget.clear();
        Q_EMIT connectionStateChanged(false, QStringLiteral("The requested PDF build result is invalid."));
        return;
    }
    QJsonObject state = readRepositoryState();
    const QJsonObject publication = publicationIdentityForBuild(target, build, currentPublicationForTarget(state, target));
    if (publication.isEmpty()) {
        failTargetedBuildActivation(QStringLiteral("The server completed the build without a valid publication identity for %1.").arg(target), target);
        return;
    }
    m_targetPdfDeltaFallbackRequested = false;

    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    QJsonObject run = runs.value(target).toObject();
    run.insert(QStringLiteral("status"), QStringLiteral("activating"));
    run.insert(QStringLiteral("build"), build);
    run.insert(QStringLiteral("publication"), publication);
    run.insert(QStringLiteral("first_publication"), fullLibraryActivation);
    runs.insert(target, run);
    state.insert(QStringLiteral("build_runs"), runs);
    if (!writeRepositoryState(state)) {
        m_targetedBuildTarget.clear();
        Q_EMIT connectionStateChanged(false, QStringLiteral("The pending Library activation could not be journaled."));
        return;
    }

    storeRenderStatus(target,
                      QJsonObject {{QStringLiteral("status"), QStringLiteral("activating")},
                                   {QStringLiteral("local_state"), QStringLiteral("activating")},
                                   {QStringLiteral("reason"), QStringLiteral("Activating the requested verified PDF without changing unrelated Library publications.")},
                                   {QStringLiteral("build"), build},
                                   {QStringLiteral("publication"), publication}});
    startTargetedPublicationActivation(target, publication);
}

void FedoraRepository::startTargetedPublicationActivation(const QString &target, const QJsonObject &publication)
{
    if (!m_client || !safePdfFilename(target) || publication.value(QStringLiteral("filename")).toString() != target) {
        failTargetedBuildActivation(QStringLiteral("The requested document publication is invalid."), target);
        return;
    }
    m_pendingLibraryBuildActivationTarget = target;
    m_targetActivationPublication = publication;
    // A document build can advance several project compositions. Activate the
    // complete immutable collection through the normal changed-object plan.
    refreshLibrary();
}

void FedoraRepository::continueTargetedSourceArtifactDownload()
{
    const QString target = m_pendingLibraryBuildActivationTarget;
    const QJsonObject publication = m_targetActivationPublication;
    if (!m_client || !safePdfFilename(target) || publication.value(QStringLiteral("filename")).toString() != target) {
        return;
    }
    const QDir bundleRoot(QDir(m_stateRoot).filePath(QStringLiteral("source-bundle")));
    if (!QDir().mkpath(bundleRoot.filePath(QStringLiteral("objects"))) || !QDir().mkpath(bundleRoot.filePath(QStringLiteral("documents")))) {
        failTargetedBuildActivation(QStringLiteral("The target source cache could not be prepared."));
        return;
    }

    if (m_targetSourceDownloadQueue.isEmpty() && m_targetSourceDownloadsInFlight.isEmpty()) {
        QSet<QString> missing;
        QString error;
        if (completeDocumentSourceArtifact(bundleRoot, publication, &missing, &error)) {
            emitSyncProgress(QStringLiteral("verifying"), 0, 0, QStringLiteral("The target source artifact is complete and verified."));
            downloadTargetedPublicationPdf();
            return;
        }
        if (!error.isEmpty()) {
            failTargetedBuildActivation(error);
            return;
        }
        m_targetSourceDownloadQueue = missing.values();
        std::sort(m_targetSourceDownloadQueue.begin(), m_targetSourceDownloadQueue.end());
        const bool planned = std::all_of(m_targetSourceDownloadQueue.cbegin(), m_targetSourceDownloadQueue.cend(), [this, &target](const QString &objectSha256) {
            return readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_manifest"), target) ||
                readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_snapshot"), target) ||
                readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("source_resource"), target) ||
                readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("renderer_evidence"), target) ||
                readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("reverse_lookup"), target) ||
                readerPlanAllowsObject(m_targetReaderPlan, objectSha256, QStringLiteral("source"), QStringLiteral("raw_synctex_fallback"), target);
        });
        if (!planned) {
            m_targetSourceDownloadQueue.clear();
            if (m_targetReaderPlanRetries++ < 1) {
                m_targetReaderPlan = {};
                m_targetReaderPlanTarget.clear();
                m_targetReaderPlanPublicationIdentity.clear();
                QTimer::singleShot(0, this, [this, target, publication] { startTargetedPublicationActivation(target, publication); });
            } else {
                m_targetReaderPlanRetries = 0;
                failTargetedBuildActivation(QStringLiteral("The target source download no longer matches its reader synchronization plan."));
            }
            return;
        }
    }

    while (m_targetSourceDownloadsInFlight.size() < 4 && !m_targetSourceDownloadQueue.isEmpty()) {
        const QString objectSha256 = m_targetSourceDownloadQueue.takeFirst();
        const qint64 plannedSize = readerPlanObjectSize(m_targetReaderPlan, objectSha256);
        if (m_targetSourceDownloadsInFlight.contains(objectSha256)) {
            continue;
        }
        m_targetSourceDownloadsInFlight.insert(objectSha256);
        const QString requestedPlanId = m_targetReaderPlan.value(QStringLiteral("plan_id")).toString();
        QNetworkReply *reply = m_client->sourceBundleObject(objectSha256.toLatin1(), requestedPlanId.toLatin1());
        const auto stream = streamBundleObject(reply, bundleRoot, objectSha256, plannedSize);
        connect(reply, &QNetworkReply::downloadProgress, this, [this, target, objectSha256, plannedSize](qint64 received, qint64 total) {
            if (m_pendingLibraryBuildActivationTarget == target) {
                accountReaderObjectProgress(objectSha256, qMin(received, plannedSize));
                emitSyncProgress(QStringLiteral("downloading"), received, total, QStringLiteral("Downloading target source-artifact objects (up to four concurrently)."));
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, bundleRoot, objectSha256, target, plannedSize, requestedPlanId, stream] {
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool stored = stream->finish(reply, objectSha256);
            const QString networkError = reply->errorString();
            const bool networkSuccess = reply->error() == QNetworkReply::NoError;
            const bool success = networkSuccess && stored;
            reply->deleteLater();
            if (m_pendingLibraryBuildActivationTarget != target || m_targetActivationPublication.value(QStringLiteral("filename")).toString() != target) {
                return;
            }
            if (m_targetReaderPlan.value(QStringLiteral("plan_id")).toString() != requestedPlanId) {
                return;
            }
            m_targetSourceDownloadsInFlight.remove(objectSha256);
            if (httpStatus == 409) {
                accountReaderObjectProgress(objectSha256, 0, false, true);
                m_targetSourceDownloadQueue.clear();
                m_targetSourceDownloadsInFlight.clear();
                m_targetDownloadAttempts.clear();
                if (m_targetReaderPlanRetries++ < 1) {
                    m_targetReaderPlan = {};
                    m_targetReaderPlanTarget.clear();
                    m_targetReaderPlanPublicationIdentity.clear();
                    QTimer::singleShot(0, this, [this, target, publication = m_targetActivationPublication] { startTargetedPublicationActivation(target, publication); });
                } else {
                    m_targetReaderPlanRetries = 0;
                    failTargetedBuildActivation(QStringLiteral("The target source reader plan was superseded twice."));
                }
                return;
            }
            if (!networkSuccess) {
                accountReaderObjectProgress(objectSha256, 0, false, true);
                const int attempt = m_targetDownloadAttempts.value(objectSha256) + 1;
                m_targetDownloadAttempts.insert(objectSha256, attempt);
                if (attempt <= 3) {
                    m_targetSourceDownloadsInFlight.insert(objectSha256);
                    static constexpr int backoffMs[] {1000, 2000, 4000};
                    const int delay = backoffMs[attempt - 1] + QRandomGenerator::global()->bounded(251);
                    emitSyncProgress(QStringLiteral("waiting_network"), 0, 0, QStringLiteral("Target source download interrupted; retrying the same object."));
                    QTimer::singleShot(delay, this, [this, target, objectSha256] {
                        if (m_pendingLibraryBuildActivationTarget == target) {
                            m_targetSourceDownloadsInFlight.remove(objectSha256);
                            m_targetSourceDownloadQueue.prepend(objectSha256);
                            continueTargetedSourceArtifactDownload();
                        }
                    });
                    return;
                }
            }
            if (!success) {
                accountReaderObjectProgress(objectSha256, 0, false, true);
                failTargetedBuildActivation(networkError.isEmpty() ? QStringLiteral("A target source-artifact object failed identity verification.") : networkError);
                return;
            }
            recordVerifiedReaderObject(bundleRoot.filePath(QStringLiteral("objects/%1").arg(objectSha256)), objectSha256, plannedSize, QStringLiteral("source"), QString(), target);
            accountReaderObjectProgress(objectSha256, plannedSize, true);
            m_targetDownloadAttempts.remove(objectSha256);
            continueTargetedSourceArtifactDownload();
        });
    }
}

void FedoraRepository::downloadTargetedPublicationPdf()
{
    if (m_targetPdfDownloading) {
        return;
    }
    const QString target = m_pendingLibraryBuildActivationTarget;
    const QJsonObject publication = m_targetActivationPublication;
    const QByteArray digest = publication.value(QStringLiteral("sha256")).toString().toLatin1();
    const qint64 expectedSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
    if (!safePdfFilename(target) || !validSha256(QString::fromLatin1(digest)) || expectedSize < 0) {
        failTargetedBuildActivation(QStringLiteral("The target PDF identity is invalid."));
        return;
    }

    const QString currentPdf = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(target));
    QString cacheError;
    Thoughts::DesktopSyncStore *const cacheStore = desktopSyncStore(&cacheError);
    if (Internal::verifyReaderObjectWithCache(cacheError.isEmpty() ? cacheStore : nullptr, currentPdf, QString::fromLatin1(digest), expectedSize, QStringLiteral("pdf"), QStringLiteral("pdf"), target)) {
        emitSyncProgress(QStringLiteral("verifying"), expectedSize, expectedSize, QStringLiteral("The target PDF is already present; verifying the next immutable generation."));
        const PreparedLibraryGeneration prepared = prepareTargetedLibraryGeneration(target, publication, {});
        if (!prepared.success) {
            failTargetedBuildActivation(prepared.error);
            return;
        }
        QJsonObject state = readRepositoryState();
        state.insert(QStringLiteral("pending_library_generation"), prepared.generation);
        state.insert(QStringLiteral("pending_pdf_projections"), QJsonArray());
        if (!writeRepositoryState(state)) {
            failTargetedBuildActivation(QStringLiteral("The target activation could not be journaled."));
            return;
        }
        emitSyncProgress(QStringLiteral("activating"), expectedSize, expectedSize, QStringLiteral("Atomically switching to the verified target generation."));
        startPendingLibraryActivation();
        return;
    }

    QByteArray transferDigest = digest;
    qint64 transferSize = expectedSize;
    QString transferRole = QStringLiteral("pdf");
    std::optional<PdfDeltaPlan> delta;
    if (readerPlanHasPdfDelta(m_targetReaderPlan, target)) {
        delta = pdfDeltaPlanForPublication(m_targetReaderPlan, publication);
        if (!delta || delta->resultSha256 != QString::fromLatin1(digest) || delta->resultSize != expectedSize) {
            retryTargetReaderPlanWithoutPdfDelta(target, publication, QStringLiteral("The planned target PDF delta did not match the authoritative publication."));
            return;
        }
        transferDigest = delta->patchSha256.toLatin1();
        transferSize = delta->patchSize;
        transferRole = QStringLiteral("pdf_delta");
    }
    if (!readerPlanAllowsObject(m_targetReaderPlan, QString::fromLatin1(transferDigest), QStringLiteral("pdf"), transferRole, target, transferSize)) {
        if (delta) {
            retryTargetReaderPlanWithoutPdfDelta(target, publication, QStringLiteral("The target PDF delta escaped its reader synchronization allowlist."));
        } else if (m_targetReaderPlanRetries++ < 1) {
            m_targetReaderPlan = {};
            m_targetReaderPlanTarget.clear();
            m_targetReaderPlanPublicationIdentity.clear();
            QTimer::singleShot(0, this, [this, target, publication] { startTargetedPublicationActivation(target, publication); });
        } else {
            m_targetReaderPlanRetries = 0;
            failTargetedBuildActivation(QStringLiteral("The target PDF download no longer matches its reader synchronization plan."));
        }
        return;
    }

    m_targetPdfDownloading = true;
    const QString requestedPlanId = m_targetReaderPlan.value(QStringLiteral("plan_id")).toString();
    QNetworkReply *reply = m_client->downloadObject(transferDigest, requestedPlanId.toLatin1());
    connect(reply, &QNetworkReply::downloadProgress, this, [this, target, transferDigest, transferSize, delta](qint64 received, qint64 total) {
        if (m_pendingLibraryBuildActivationTarget == target) {
            accountReaderObjectProgress(QString::fromLatin1(transferDigest), qMin(received, transferSize));
            emitSyncProgress(QStringLiteral("downloading"), received, total, delta ? QStringLiteral("Downloading the compact target PDF update.") : QStringLiteral("Downloading only the requested PDF."));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, target, publication, digest, transferDigest, transferSize, delta, requestedPlanId] {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray contents = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        const QString networkError = reply->errorString();
        const bool networkSuccess = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (m_pendingLibraryBuildActivationTarget != target || m_targetActivationPublication != publication || m_targetReaderPlan.value(QStringLiteral("plan_id")).toString() != requestedPlanId) {
            m_targetPdfDownloading = false;
            return;
        }
        if (httpStatus == 409) {
            m_targetPdfDownloading = false;
            accountReaderObjectProgress(QString::fromLatin1(transferDigest), 0, false, true);
            if (m_targetReaderPlanRetries++ < 1) {
                m_targetReaderPlan = {};
                m_targetReaderPlanTarget.clear();
                m_targetReaderPlanPublicationIdentity.clear();
                QTimer::singleShot(0, this, [this, target, publication] { startTargetedPublicationActivation(target, publication); });
            } else {
                m_targetReaderPlanRetries = 0;
                failTargetedBuildActivation(QStringLiteral("The target PDF reader plan was superseded twice."));
            }
            return;
        }
        if (!networkSuccess) {
            m_targetPdfDownloading = false;
            accountReaderObjectProgress(QString::fromLatin1(transferDigest), 0, false, true);
            ++m_targetPdfDownloadAttempts;
            if (m_targetPdfDownloadAttempts <= 3) {
                static constexpr int backoffMs[] {1000, 2000, 4000};
                const int delay = backoffMs[m_targetPdfDownloadAttempts - 1] + QRandomGenerator::global()->bounded(251);
                emitSyncProgress(QStringLiteral("waiting_network"), 0, transferSize, QStringLiteral("Target PDF transfer interrupted; retrying the same publication."));
                QTimer::singleShot(delay, this, [this, target] {
                    if (m_pendingLibraryBuildActivationTarget == target) {
                        downloadTargetedPublicationPdf();
                    }
                });
                return;
            }
            failTargetedBuildActivation(networkError.isEmpty() ? QStringLiteral("The target PDF transfer failed.") : networkError);
            return;
        }
        if (contents.size() != transferSize || !Thoughts::SyncClient::verifyObject(contents, transferDigest)) {
            m_targetPdfDownloading = false;
            accountReaderObjectProgress(QString::fromLatin1(transferDigest), 0, false, true);
            if (delta) {
                retryTargetReaderPlanWithoutPdfDelta(target, publication, QStringLiteral("The downloaded target PDF delta failed exact identity verification."));
            } else {
                failTargetedBuildActivation(QStringLiteral("The target PDF failed size or SHA-256 verification."));
            }
            return;
        }
        m_targetPdfDownloadAttempts = 0;
        emitSyncProgress(QStringLiteral("verifying"), contents.size(), transferSize, QStringLiteral("Preparing a target-only immutable generation."));
        const QPointer<FedoraRepository> repository(this);
        QThreadPool::globalInstance()->start([repository, target, publication, delta, requestedPlanId, transferDigest, transferSize, contents = std::move(contents)]() mutable {
            if (!repository) {
                return;
            }
            bool deltaVerified = !delta.has_value();
            if (delta) {
                contents = Internal::reconstructVerifiedPdfDelta(repository->m_docsRoot, target, delta->baseSha256, contents, delta->resultSha256, delta->resultSize);
                deltaVerified = !contents.isEmpty();
            }
            PreparedLibraryGeneration prepared;
            if (deltaVerified) {
                prepared = repository->prepareTargetedLibraryGeneration(target, publication, contents);
            } else {
                prepared.error = QStringLiteral("The target PDF delta or its active base failed exact identity verification.");
            }
            QMetaObject::invokeMethod(
                repository,
                [repository, target, publication, prepared, delta, deltaVerified, requestedPlanId, transferDigest = QString::fromLatin1(transferDigest), transferSize] {
                    if (!repository) {
                        return;
                    }
                    repository->m_targetPdfDownloading = false;
                    if (repository->m_pendingLibraryBuildActivationTarget != target || repository->m_targetActivationPublication != publication ||
                        repository->m_targetReaderPlan.value(QStringLiteral("plan_id")).toString() != requestedPlanId) {
                        return;
                    }
                    if (!deltaVerified) {
                        repository->accountReaderObjectProgress(transferDigest, 0, false, true);
                        repository->retryTargetReaderPlanWithoutPdfDelta(target, publication, prepared.error);
                        return;
                    }
                    if (!prepared.success) {
                        repository->accountReaderObjectProgress(transferDigest, 0, false, true);
                        repository->failTargetedBuildActivation(prepared.error);
                        return;
                    }
                    repository->recordVerifiedReaderObject(QDir(prepared.generation.value(QStringLiteral("staging_path")).toString()).filePath(target),
                                                           publication.value(QStringLiteral("sha256")).toString(),
                                                           publication.value(QStringLiteral("size_bytes")).toInteger(-1),
                                                           QStringLiteral("pdf"),
                                                           QStringLiteral("pdf"),
                                                           target);
                    repository->accountReaderObjectProgress(transferDigest, transferSize, true);
                    QJsonObject state = repository->readRepositoryState();
                    state.insert(QStringLiteral("pending_library_generation"), prepared.generation);
                    state.insert(QStringLiteral("pending_pdf_projections"), QJsonArray());
                    if (!repository->writeRepositoryState(state)) {
                        repository->failTargetedBuildActivation(QStringLiteral("The target activation could not be journaled."));
                        return;
                    }
                    const qint64 finalSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
                    repository->emitSyncProgress(QStringLiteral("activating"), finalSize, finalSize, QStringLiteral("Atomically switching to the verified target generation."));
                    repository->startPendingLibraryActivation();
                },
                Qt::QueuedConnection);
        });
    });
}

FedoraRepository::PreparedLibraryGeneration FedoraRepository::prepareTargetedLibraryGeneration(const QString &target, const QJsonObject &publication, const QByteArray &pdfContents)
{
    PreparedLibraryGeneration prepared;
    const QString digest = publication.value(QStringLiteral("sha256")).toString();
    const qint64 expectedSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
    if (!safePdfFilename(target) || publication.value(QStringLiteral("filename")).toString() != target || !validSha256(digest) || !validSha256(publication.value(QStringLiteral("source_artifact_sha256")).toString()) || expectedSize < 0 ||
        (!pdfContents.isEmpty() && (pdfContents.size() != expectedSize || !Thoughts::SyncClient::verifyObject(pdfContents, digest.toLatin1())))) {
        prepared.error = QStringLiteral("The target publication is incomplete or failed identity verification.");
        return prepared;
    }

    const QString current = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs"));
    QFile manifestFile(QDir(current).filePath(QStringLiteral(".thoughts-generation.json")));
    const QJsonObject currentManifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
    QJsonArray publications = currentManifest.value(QStringLiteral("publications")).toArray();
    if (publications.isEmpty()) {
        const QStringList existingPdfs = QDir(current).entryList({QStringLiteral("*.pdf")}, QDir::Files);
        if (std::any_of(existingPdfs.cbegin(), existingPdfs.cend(), [&target](const QString &filename) { return filename != target; })) {
            prepared.error = QStringLiteral("The active Library has no verifiable generation manifest; use Sync to repair it without guessing membership.");
            return prepared;
        }
    }

    QJsonObject mergedPublication;
    QJsonArray nextPublications;
    bool replaced = false;
    for (const QJsonValue &value : std::as_const(publications)) {
        QJsonObject existing = value.toObject();
        const QString filename = existing.value(QStringLiteral("filename")).toString();
        const QString existingDigest = existing.value(QStringLiteral("sha256")).toString();
        const qint64 existingSize = existing.value(QStringLiteral("size_bytes")).toInteger(-1);
        if (!safePdfFilename(filename) || !validSha256(existingDigest) || !validSha256(existing.value(QStringLiteral("generation_id")).toString()) || existingSize < 0) {
            prepared.error = QStringLiteral("The active Library manifest contains an unverifiable publication; use Sync to repair it.");
            return prepared;
        }
        if (filename == target) {
            mergedPublication = existing;
            for (auto iterator = publication.constBegin(); iterator != publication.constEnd(); ++iterator) {
                mergedPublication.insert(iterator.key(), iterator.value());
            }
            replaced = true;
            nextPublications.append(mergedPublication);
            continue;
        }
        nextPublications.append(existing);
    }
    if (!replaced) {
        mergedPublication = publication;
        nextPublications.append(mergedPublication);
    }
    if (!validSha256(publication.value(QStringLiteral("generation_id")).toString())) {
        mergedPublication.insert(QStringLiteral("generation_id"), digest);
    }
    QJsonObject metadata = mergedPublication.value(QStringLiteral("metadata")).toObject();
    if (metadata.value(QStringLiteral("kind")).toString().isEmpty()) {
        metadata.insert(QStringLiteral("kind"), QStringLiteral("source_linked"));
    }
    mergedPublication.insert(QStringLiteral("metadata"), metadata);
    for (qsizetype index = 0; index < nextPublications.size(); ++index) {
        if (nextPublications.at(index).toObject().value(QStringLiteral("filename")).toString() == target) {
            nextPublications.replace(index, mergedPublication);
            break;
        }
    }

    QFile sourceIndexFile(QDir(current).filePath(QStringLiteral(".thoughts-source-index.json")));
    QJsonObject sourceIndex = sourceIndexFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(sourceIndexFile.readAll()).object() : QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray()}};
    if (sourceIndex.value(QStringLiteral("schema_version")).toInt() != 1 || !sourceIndex.value(QStringLiteral("pdfs")).isArray()) {
        prepared.error = QStringLiteral("The active source-layout index is invalid; use Sync to repair it.");
        return prepared;
    }
    QJsonArray sourcePdfs;
    bool sourceEntryReplaced = false;
    for (const QJsonValue &value : sourceIndex.value(QStringLiteral("pdfs")).toArray()) {
        QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() == target) {
            sourceEntryReplaced = true;
            entry.insert(QStringLiteral("pdf_sha256"), digest);
            if (entry.value(QStringLiteral("project")).toString().isEmpty() && !mergedPublication.value(QStringLiteral("project")).toString().isEmpty()) {
                entry.insert(QStringLiteral("project"), mergedPublication.value(QStringLiteral("project")));
            }
        }
        sourcePdfs.append(entry);
    }
    if (metadata.value(QStringLiteral("kind")).toString() == QLatin1String("source_linked") && !sourceEntryReplaced) {
        QJsonObject entry {{QStringLiteral("filename"), target}, {QStringLiteral("pdf_sha256"), digest}};
        if (!mergedPublication.value(QStringLiteral("project")).toString().isEmpty()) {
            entry.insert(QStringLiteral("project"), mergedPublication.value(QStringLiteral("project")));
        }
        sourcePdfs.append(entry);
    }
    sourceIndex.insert(QStringLiteral("pdfs"), sourcePdfs);
    const QByteArray sourceIndexContents = QJsonDocument(sourceIndex).toJson(QJsonDocument::Compact);
    const QString sourceIndexSha256 = QString::fromLatin1(Thoughts::SyncClient::sha256(sourceIndexContents));
    const QByteArray revisionInput = QJsonDocument(QJsonObject {{QStringLiteral("publications"), nextPublications}, {QStringLiteral("source_index_sha256"), sourceIndexSha256}}).toJson(QJsonDocument::Compact);
    const QString revision = QString::fromLatin1(Thoughts::SyncClient::sha256(revisionInput));
    const QString staging = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts-library-stage-%1").arg(revision));
    QJsonObject library = readRepositoryState().value(QStringLiteral("library")).toObject();
    library.insert(QStringLiteral("library_revision"), revision);
    library.insert(QStringLiteral("artifact_revision"), revision);
    library.insert(QStringLiteral("source_index_sha256"), sourceIndexSha256);
    library.insert(QStringLiteral("publications"), nextPublications);
    QJsonObject generation {{QStringLiteral("library_revision"), revision},
                            {QStringLiteral("artifact_revision"), revision},
                            {QStringLiteral("source_index_sha256"), sourceIndexSha256},
                            {QStringLiteral("staging_path"), staging},
                            {QStringLiteral("changed_pdfs"), QJsonArray {target}},
                            {QStringLiteral("targeted_activation"), true},
                            {QStringLiteral("target_publication"), mergedPublication},
                            {QStringLiteral("library"), library}};
    if (directoryMatchesLibraryGeneration(current, generation, nullptr)) {
        prepared.success = true;
        prepared.alreadyCurrent = true;
        prepared.generation = generation;
        return prepared;
    }
    if (QFileInfo::exists(staging) && pathsReferToSameLocation(current, staging)) {
        prepared.error = QStringLiteral("The calculated target staging generation is active but failed verification; refusing to replace it.");
        return prepared;
    }
    if (QFileInfo::exists(staging) && !removeReadOnlyGeneration(staging)) {
        prepared.error = QStringLiteral("The stale target staging generation could not be cleared.");
        return prepared;
    }
    if (!QDir().mkpath(staging)) {
        prepared.error = QStringLiteral("The target staging generation could not be created.");
        return prepared;
    }
    const auto discard = [&staging] { QDir(staging).removeRecursively(); };
    for (const QJsonValue &value : std::as_const(nextPublications)) {
        const QJsonObject item = value.toObject();
        const QString filename = item.value(QStringLiteral("filename")).toString();
        const QString destination = QDir(staging).filePath(filename);
        if (filename == target) {
            if (pdfContents.isEmpty()) {
                if (!linkOrCopyFile(QDir(current).filePath(filename), destination)) {
                    prepared.error = QStringLiteral("The verified target PDF could not be staged.");
                    discard();
                    return prepared;
                }
            } else {
                QSaveFile file(destination);
                file.setDirectWriteFallback(false);
                if (!file.open(QIODevice::WriteOnly) || file.write(pdfContents) != pdfContents.size() || !file.commit()) {
                    prepared.error = QStringLiteral("The verified target PDF could not be written to staging.");
                    discard();
                    return prepared;
                }
                if (!preserveMacDocumentAccess(QDir(current).filePath(filename), destination)) {
                    prepared.error = QStringLiteral("The target PDF's local document access metadata could not be preserved.");
                    discard();
                    return prepared;
                }
            }
        } else if (QFileInfo::exists(QDir(current).filePath(filename)) && !linkOrCopyFile(QDir(current).filePath(filename), destination)) {
            prepared.error = QStringLiteral("The unrelated PDF %1 could not be retained in the next generation.").arg(filename);
            discard();
            return prepared;
        }
        if (filename == target) QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    if (!writeJsonFile(QDir(staging).filePath(QStringLiteral(".thoughts-source-index.json")), sourceIndexContents) ||
        !writeJsonFile(QDir(staging).filePath(QStringLiteral(".thoughts-source-artifacts/%1.json").arg(target)), QJsonDocument(documentSourceArtifactPointer(mergedPublication)).toJson(QJsonDocument::Compact))) {
        prepared.error = QStringLiteral("The target generation manifests could not be staged.");
        discard();
        return prepared;
    }

    if (!writeLibraryGenerationMetadata(library, staging)) {
        prepared.error = QStringLiteral("The target generation metadata could not be staged.");
        discard();
        return prepared;
    }
    QString verificationError;
    if (!directoryMatchesLibraryGeneration(staging, generation, &verificationError)) {
        prepared.error = verificationError;
        discard();
        return prepared;
    }
    prepared.success = true;
    prepared.generation = generation;
    return prepared;
}

QJsonObject FedoraRepository::prepareTargetedLibraryGenerationResult(const QString &target, const QJsonObject &publication, const QByteArray &pdfContents)
{
    const PreparedLibraryGeneration prepared = prepareTargetedLibraryGeneration(target, publication, pdfContents);
    return QJsonObject {{QStringLiteral("success"), prepared.success},
                        {QStringLiteral("already_current"), prepared.alreadyCurrent},
                        {QStringLiteral("generation"), prepared.generation},
                        {QStringLiteral("pending_objects"), prepared.pendingObjects},
                        {QStringLiteral("error"), prepared.error}};
}

QString FedoraRepository::localPublicationState(const QString &target, const QJsonObject &publication) const
{
    const QString digest = publication.value(QStringLiteral("sha256")).toString();
    const qint64 expectedSize = publication.value(QStringLiteral("size_bytes")).toInteger(-1);
    const QString buildId = publication.value(QStringLiteral("build_id")).toString();
    const QJsonValue sourceFrontier = publication.value(QStringLiteral("source_frontier"));
    if (!safePdfFilename(target) || publication.value(QStringLiteral("filename")).toString() != target || !validSha256(digest) || !validSha256(publication.value(QStringLiteral("source_artifact_sha256")).toString()) || expectedSize < 0) {
        return QStringLiteral("unknown");
    }
    const QDir current(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QFileInfo pdf(current.filePath(target));
    if (!pdf.isFile() || pdf.size() != expectedSize || fileSha256(pdf.filePath()) != digest.toLatin1()) {
        return QStringLiteral("sync_pending");
    }

    const QJsonDocument index = cachedJsonFile(current.filePath(QStringLiteral(".thoughts-library-index.json")));
    const QJsonDocument manifest = cachedJsonFile(current.filePath(QStringLiteral(".thoughts-generation.json")));
    if (!index.isObject() || !manifest.isObject()) {
        return QStringLiteral("sync_pending");
    }
    bool indexMatches = false;
    for (const QJsonValue &value : index.object().value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() == target && entry.value(QStringLiteral("pdf_sha256")).toString() == digest && (buildId.isEmpty() || entry.value(QStringLiteral("build_id")).toString() == buildId) &&
            (sourceFrontier.isUndefined() || sourceFrontier.isNull() || entry.value(QStringLiteral("source_frontier")) == sourceFrontier)) {
            indexMatches = true;
            break;
        }
    }
    bool manifestMatches = false;
    for (const QJsonValue &value : manifest.object().value(QStringLiteral("publications")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() == target && entry.value(QStringLiteral("sha256")).toString() == digest && entry.value(QStringLiteral("size_bytes")).toInteger(-1) == expectedSize &&
            (buildId.isEmpty() || entry.value(QStringLiteral("build_id")).toString() == buildId) && (sourceFrontier.isUndefined() || sourceFrontier.isNull() || entry.value(QStringLiteral("source_frontier")) == sourceFrontier)) {
            manifestMatches = true;
            break;
        }
    }
    return indexMatches && manifestMatches && activeDocumentSourceArtifactMatches(m_docsRoot, m_stateRoot, publication) ? QStringLiteral("current") : QStringLiteral("sync_pending");
}

void FedoraRepository::setLiveRenderStatus(const QString &target, const QJsonObject &status)
{
    m_restoredTransientRenderStatuses.remove(target);
    m_renderStatuses.insert(target, status);
}

void FedoraRepository::storeRenderStatus(const QString &target, const QJsonObject &status)
{
    if (!safePdfFilename(target)) {
        return;
    }
    QJsonObject storedStatus = status;
    storedStatus.remove(QStringLiteral("restored_from_cache"));
    setLiveRenderStatus(target, storedStatus);
    QJsonObject state = readRepositoryState();
    QJsonObject statuses = state.value(QStringLiteral("render_statuses")).toObject();
    statuses.insert(target, storedStatus);
    state.insert(QStringLiteral("render_statuses"), statuses);
    writeRepositoryState(state);
    Q_EMIT renderStatusChanged(target, storedStatus);
}

void FedoraRepository::storeRenderStatuses(const QHash<QString, QJsonObject> &statusesByTarget)
{
    // One write for a build that covers many PDFs (a shared file changed).
    QJsonObject state = readRepositoryState();
    QJsonObject statuses = state.value(QStringLiteral("render_statuses")).toObject();
    QStringList changed;
    for (auto status = statusesByTarget.cbegin(); status != statusesByTarget.cend(); ++status) {
        if (!safePdfFilename(status.key())) {
            continue;
        }
        QJsonObject storedStatus = status.value();
        storedStatus.remove(QStringLiteral("restored_from_cache"));
        setLiveRenderStatus(status.key(), storedStatus);
        statuses.insert(status.key(), storedStatus);
        changed.append(status.key());
    }
    if (changed.isEmpty()) {
        return;
    }
    state.insert(QStringLiteral("render_statuses"), statuses);
    writeRepositoryState(state);
    for (const QString &target : std::as_const(changed)) {
        Q_EMIT renderStatusChanged(target, m_renderStatuses.value(target));
    }
}

void FedoraRepository::failTargetedBuildActivation(const QString &message, const QString &explicitTarget)
{
    const QString target = safePdfFilename(explicitTarget) ? explicitTarget : !m_pendingLibraryBuildActivationTarget.isEmpty() ? m_pendingLibraryBuildActivationTarget : m_targetedBuildTarget;
    if (!safePdfFilename(target)) {
        return;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    QJsonObject run = runs.value(target).toObject();
    run.insert(QStringLiteral("status"), QStringLiteral("activation_failed"));
    run.insert(QStringLiteral("message"), message);
    runs.insert(target, run);
    state.insert(QStringLiteral("build_runs"), runs);
    QJsonObject etags = state.value(QStringLiteral("document_sync_etags")).toObject();
    etags.remove(target);
    state.insert(QStringLiteral("document_sync_etags"), etags);
    writeRepositoryState(state);
    QJsonObject failed = m_renderStatuses.value(target);
    if (failed.value(QStringLiteral("status")).toString().isEmpty() || failed.value(QStringLiteral("status")).toString() == QLatin1String("activating")) {
        failed.insert(QStringLiteral("status"), QStringLiteral("synced"));
    }
    failed.insert(QStringLiteral("local_state"), QStringLiteral("error"));
    failed.insert(QStringLiteral("local_error"), message);
    if (failed.value(QStringLiteral("publication")).toObject().isEmpty()) {
        failed.insert(QStringLiteral("publication"), run.value(QStringLiteral("publication")));
    }
    storeRenderStatus(target, failed);
    m_pendingLibraryBuildActivationTarget.clear();
    m_targetedBuildTarget.clear();
    m_targetActivationPublication = {};
    m_targetSourceDownloadQueue.clear();
    m_targetSourceDownloadsInFlight.clear();
    m_targetDownloadAttempts.clear();
    m_targetPdfDownloadAttempts = 0;
    m_targetPdfDownloading = false;
    m_targetReaderPlan = {};
    m_targetReaderPlanTarget.clear();
    m_targetReaderPlanPublicationIdentity.clear();
    m_targetReaderPlanRetries = 0;
    m_targetPdfDeltaFallbackRequested = false;
    if (m_syncCoordinator.running() && m_syncCoordinator.target == target) {
        failManualSync(message);
    }
}

void FedoraRepository::activatePublishedPdf(const QString &target)
{
    if (!safePdfFilename(target)) {
        return;
    }
    const QJsonObject remote = m_renderStatuses.value(target);
    const QJsonObject publication = remote.value(QStringLiteral("publication")).toObject();
    if (remote.value(QStringLiteral("status")).toString() != QLatin1String("synced") || localPublicationState(target, publication) == QLatin1String("unknown")) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The server has not provided a valid publication identity for this PDF."));
        return;
    }
    if (localPublicationState(target, publication) == QLatin1String("current")) {
        QJsonObject current = remote;
        current.insert(QStringLiteral("local_state"), QStringLiteral("current"));
        current.insert(QStringLiteral("reason"), QStringLiteral("This computer has the verified server publication."));
        storeRenderStatus(target, current);
        return;
    }
    m_targetPdfDeltaFallbackRequested = false;

    QJsonObject state = readRepositoryState();
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    const QString runId = m_activeTraceId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_activeTraceId;
    QJsonObject run {{QStringLiteral("run_id"), runId},
                     {QStringLiteral("trace_id"), runId},
                     {QStringLiteral("status"), QStringLiteral("activating")},
                     {QStringLiteral("first_publication"), false},
                     {QStringLiteral("publication"), publication},
                     {QStringLiteral("build"), remote.value(QStringLiteral("build"))}};
    runs.insert(target, run);
    state.insert(QStringLiteral("build_runs"), runs);
    if (!writeRepositoryState(state)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The pending Library activation could not be journaled."));
        return;
    }
    m_targetedBuildTarget = target;
    QJsonObject activating = remote;
    activating.insert(QStringLiteral("local_state"), QStringLiteral("activating"));
    activating.insert(QStringLiteral("reason"), QStringLiteral("Activating the requested verified PDF on this computer."));
    storeRenderStatus(target, activating);
    if (!ensureClient()) {
        failTargetedBuildActivation(QStringLiteral("The server credential or configuration is unavailable."));
        return;
    }
    startTargetedPublicationActivation(target, publication);
}

void FedoraRepository::completeLibraryBuildActivationIfReady()
{
    if (m_libraryActivationCompletionChecking) {
        m_libraryActivationCompletionRecheck = true;
        return;
    }
    const QJsonObject runs = readRepositoryState().value(QStringLiteral("build_runs")).toObject();
    const bool hasCandidate = std::any_of(runs.begin(), runs.end(), [](const QJsonValue &value) {
        const QString status = value.toObject().value(QStringLiteral("status")).toString();
        return status == QLatin1String("activating") || status == QLatin1String("activation_failed") || status == QLatin1String("interrupted");
    });
    if (!hasCandidate) {
        return;
    }
    m_libraryActivationCompletionChecking = true;
    const QString docsRoot = m_docsRoot;
    const QString stateRoot = m_stateRoot;
    const QPointer<FedoraRepository> repository(this);
    QThreadPool::globalInstance()->start([repository, docsRoot, stateRoot, runs] {
        const LibraryActivationInspection inspection = inspectCompletedLibraryBuildActivations(docsRoot, stateRoot, runs);
        QMetaObject::invokeMethod(
            repository,
            [repository, inspection] {
                if (!repository) {
                    return;
                }
                repository->m_libraryActivationCompletionChecking = false;
                repository->applyCompletedLibraryBuildActivations(inspection.completedRuns, inspection.error);
                if (repository->m_libraryActivationCompletionRecheck) {
                    repository->m_libraryActivationCompletionRecheck = false;
                    QTimer::singleShot(0, repository, &FedoraRepository::completeLibraryBuildActivationIfReady);
                }
            },
            Qt::QueuedConnection);
    });
}

void FedoraRepository::applyCompletedLibraryBuildActivations(const QHash<QString, QJsonObject> &inspectedRuns, const QString &inspectionError)
{
    if (!inspectionError.isEmpty()) {
        qWarning().noquote() << inspectionError;
    }
    if (inspectedRuns.isEmpty()) {
        return;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    QHash<QString, QJsonObject> completedRuns;
    for (auto iterator = inspectedRuns.cbegin(); iterator != inspectedRuns.cend(); ++iterator) {
        const QJsonObject current = runs.value(iterator.key()).toObject();
        const QJsonObject inspected = iterator.value();
        const QString inspectedRunId = inspected.value(QStringLiteral("run_id")).toString();
        if ((!inspectedRunId.isEmpty() && current.value(QStringLiteral("run_id")).toString() == inspectedRunId) || current == inspected) {
            completedRuns.insert(iterator.key(), inspected);
        }
    }
    if (completedRuns.isEmpty()) {
        return;
    }
    for (auto iterator = completedRuns.cbegin(); iterator != completedRuns.cend(); ++iterator) {
        runs.remove(iterator.key());
    }
    state.insert(QStringLiteral("build_runs"), runs);
    if (!writeRepositoryState(state)) {
        return;
    }
    bool completeManualOperation = false;
    for (auto iterator = completedRuns.cbegin(); iterator != completedRuns.cend(); ++iterator) {
        const QString target = iterator.key();
        const QJsonObject run = iterator.value();
        const QJsonObject publication = run.value(QStringLiteral("publication")).toObject();
        const QString runDirectory = buildRunDirectory(run.value(QStringLiteral("run_id")).toString());
        if (QFileInfo(runDirectory).isDir()) {
            writeSubmissionStatus(runDirectory, QStringLiteral("succeeded"), QStringLiteral("The verified Library generation from the server is active on this computer."));
        }
        QJsonObject completed = m_renderStatuses.value(target);
        completed.insert(QStringLiteral("status"), QStringLiteral("synced"));
        completed.insert(QStringLiteral("local_state"), QStringLiteral("current"));
        completed.insert(QStringLiteral("reason"), QStringLiteral("The authoritative Library generation from the server is active on this computer."));
        completed.insert(QStringLiteral("publication"), publication);
        completed.insert(QStringLiteral("refreshed"), true);
        storeRenderStatus(target, completed);
        if (m_pendingLibraryBuildActivationTarget == target) {
            m_pendingLibraryBuildActivationTarget.clear();
        }
        if (m_targetedBuildTarget == target) {
            m_targetedBuildTarget.clear();
        }
        if (m_targetActivationPublication.value(QStringLiteral("filename")).toString() == target) {
            m_targetActivationPublication = {};
            m_targetSourceDownloadQueue.clear();
            m_targetSourceDownloadsInFlight.clear();
            m_targetDownloadAttempts.clear();
            m_targetPdfDownloadAttempts = 0;
            m_targetPdfDownloading = false;
            m_targetReaderPlan = {};
            m_targetReaderPlanTarget.clear();
            m_targetReaderPlanPublicationIdentity.clear();
            m_targetReaderPlanRetries = 0;
            m_targetPdfDeltaFallbackRequested = false;
        }
        completeManualOperation = completeManualOperation || (m_syncCoordinator.running() && m_syncCoordinator.target == target);
    }
    Q_EMIT connectionStateChanged(true, QStringLiteral("The verified Library generation from the server is active on this computer."));
    if (completeManualOperation) {
        completeManualSync();
    }
}

void FedoraRepository::publishSources(const QString &buildTarget, const QString &traceId)
{
    if (!buildTarget.isEmpty() && !safePdfFilename(buildTarget)) {
        return;
    }
    if (!traceId.isEmpty()) {
        applyTraceId(traceId);
    }
    QJsonObject state = readRepositoryState();
    if (!buildTarget.isEmpty() && buildActivity(buildTarget).value(QStringLiteral("active")).toBool()) {
        m_targetedBuildTarget = buildTarget;
        if (ensureClient()) {
            pollBuilds();
        }
        return;
    }
    if (state.value(QStringLiteral("source_publication_requested")).toBool()) {
        const QString pendingTarget = state.value(QStringLiteral("source_publication_build_target")).toString();
        if (pendingTarget == buildTarget) {
            if (!buildTarget.isEmpty()) {
                m_targetedBuildTarget = buildTarget;
            }
            startPendingSourcePublication();
        } else if (!buildTarget.isEmpty()) {
            QJsonArray deferred = deferredTargetedBuilds(state);
            if (!deferred.contains(buildTarget)) {
                deferred.append(buildTarget);
                storeDeferredTargetedBuilds(state, deferred);
                if (!writeRepositoryState(state)) {
                    Q_EMIT connectionStateChanged(false, QStringLiteral("The requested server build could not be queued locally."));
                    return;
                }
            }
            storeRenderStatus(buildTarget,
                              QJsonObject {{QStringLiteral("status"), QStringLiteral("publishing")},
                                           {QStringLiteral("queued"), true},
                                           {QStringLiteral("reason"), QStringLiteral("Waiting for the active source publication to finish before requesting this build.")}});
            startPendingSourcePublication();
        } else {
            startPendingSourcePublication();
        }
        return;
    }
    if (!buildTarget.isEmpty()) {
        const QString runId = m_activeTraceId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_activeTraceId;
        applyTraceId(runId);
        const QString runDirectory = buildRunDirectory(runId);
        const QString executable = sourcePublisherExecutable();
        const QStringList arguments = sourcePublicationArguments(buildTarget);
        QJsonArray command {executable};
        for (const QString &argument : arguments) {
            command.append(argument);
        }
        const QJsonObject request {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("target"), buildTarget},
            {QStringLiteral("submission_command"), command},
        };
        const QString requestPath = QDir(runDirectory).filePath(QStringLiteral("request.json"));
        if (!writeJsonFile(requestPath, QJsonDocument(request).toJson(QJsonDocument::Indented)) || !writeSubmissionStatus(runDirectory, QStringLiteral("publishing"))) {
            Q_EMIT connectionStateChanged(false, QStringLiteral("The server build run could not be recorded locally."));
            return;
        }
        QFile::setPermissions(requestPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
        runs.insert(buildTarget,
                    QJsonObject {
                        {QStringLiteral("run_id"), runId},
                        {QStringLiteral("trace_id"), runId},
                        {QStringLiteral("target"), buildTarget},
                        {QStringLiteral("status"), QStringLiteral("publishing")},
                        {QStringLiteral("started_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    });
        state.insert(QStringLiteral("build_runs"), runs);
        const QJsonArray deferred = deferredTargetedBuilds(state);
        if (!deferred.isEmpty()) {
            QJsonArray retained;
            bool removed = false;
            for (const QJsonValue &value : deferred) {
                if (!removed && value.toString() == buildTarget) {
                    removed = true;
                    continue;
                }
                retained.append(value);
            }
            storeDeferredTargetedBuilds(state, retained);
        }
    } else {
        const QString runId = m_activeTraceId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_activeTraceId;
        applyTraceId(runId);
        const QString runDirectory = buildRunDirectory(runId);
        const QString executable = sourcePublisherExecutable();
        QJsonArray command {executable};
        for (const QString &argument : sourcePublicationArguments(QString())) {
            command.append(argument);
        }
        const QJsonObject request {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("submission_command"), command},
        };
        const QString requestPath = QDir(runDirectory).filePath(QStringLiteral("request.json"));
        if (!writeJsonFile(requestPath, QJsonDocument(request).toJson(QJsonDocument::Indented))) {
            Q_EMIT connectionStateChanged(false, QStringLiteral("The Sync run could not be recorded locally."));
            return;
        }
        QFile::setPermissions(requestPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        state.insert(QStringLiteral("source_publication_run"),
                     QJsonObject {
                         {QStringLiteral("run_id"), runId},
                         {QStringLiteral("trace_id"), runId},
                         {QStringLiteral("status"), QStringLiteral("publishing")},
                         {QStringLiteral("started_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     });
    }
    state.insert(QStringLiteral("source_publication_requested"), true);
    if (!buildTarget.isEmpty()) {
        state.insert(QStringLiteral("source_publication_build_target"), buildTarget);
    }
    if (writeRepositoryState(state)) {
        if (!buildTarget.isEmpty()) {
            m_targetedBuildTarget = buildTarget;
            storeRenderStatus(buildTarget, QJsonObject {{QStringLiteral("status"), QStringLiteral("publishing")}, {QStringLiteral("reason"), QStringLiteral("Publishing sources and exact build inputs to the server.")}});
            startPendingSourcePublication();
        } else if (m_syncCoordinator.running()) {
            scheduleManualSyncAdvance();
        }
    }
}

void FedoraRepository::publishNewSourceDocument(const QString &project, const QString &buildTarget, const QString &traceId)
{
    if (!traceId.isEmpty()) {
        applyTraceId(traceId);
    }
    const QString expectedProject = QStringLiteral("docs-%1").arg(QFileInfo(buildTarget).completeBaseName());
    const QString projectRoot = QDir(m_docsRoot).filePath(project);
    const bool explicitDocuments = !Thoughts::ProjectConfiguration::find(m_docsRoot, project).isEmpty()
        || QFileInfo(QDir(projectRoot).filePath(QStringLiteral("thoughts-project.json"))).isFile();
    const bool legacyDocuments = QFileInfo::exists(QDir(projectRoot).filePath(QStringLiteral("tex/docs_order_manifest.txt")))
        && QFileInfo(QDir(projectRoot).filePath(QStringLiteral("tex/source"))).isDir();
    // A renamed folder still publishes the PDF of its old name (rule F).
    const bool expectedFolder = project == expectedProject
        || Thoughts::SourceFolderNames::oldNameOf(project, Thoughts::SourceFolderNames::knownRenames(m_docsRoot)) == expectedProject;
    if (!safePdfFilename(buildTarget) || !safeComponent(project) || !expectedFolder || (!explicitDocuments && !legacyDocuments)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The selected source project does not use the required docs-X to X.pdf layout."));
        return;
    }

    QJsonObject state = readRepositoryState();
    const QJsonObject existingRun = state.value(QStringLiteral("build_runs")).toObject().value(buildTarget).toObject();
    if (activeBuildStatus(existingRun.value(QStringLiteral("status")).toString())) {
        if (!existingRun.value(QStringLiteral("first_publication")).toBool() || existingRun.value(QStringLiteral("project")).toString() != project) {
            Q_EMIT connectionStateChanged(false, QStringLiteral("Another server build already owns this PDF target."));
            return;
        }
        m_targetedBuildTarget = buildTarget;
        const QString status = existingRun.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("activating")) {
            if (ensureClient()) {
                const QJsonObject publication = existingRun.value(QStringLiteral("publication")).toObject();
                if (publication.isEmpty()) {
                    Q_EMIT connectionStateChanged(false, QStringLiteral("The previous target activation has no scoped publication identity; start a fresh Build."));
                } else {
                    startTargetedPublicationActivation(buildTarget, publication);
                }
            }
        } else if (status == QLatin1String("preparing") || status == QLatin1String("publishing")) {
            startPendingSourcePublication();
        } else if (ensureClient()) {
            pollBuilds();
        }
        return;
    }
    if (state.value(QStringLiteral("source_publication_requested")).toBool()) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("Another source publication is still being submitted to the server."));
        return;
    }

    const QString runId = m_activeTraceId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_activeTraceId;
    applyTraceId(runId);
    const QString runDirectory = buildRunDirectory(runId);
    const QString publisher = sourcePublisherExecutable();
    QJsonArray submissionCommand {publisher};
    for (const QString &argument : sourcePublicationArguments(buildTarget, project)) {
        submissionCommand.append(argument);
    }
    const QJsonObject request {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("project"), project},
        {QStringLiteral("target"), buildTarget},
        {QStringLiteral("submission_command"), submissionCommand},
        {QStringLiteral("activation"), QStringLiteral("targeted_immutable_generation")},
    };
    const QString requestPath = QDir(runDirectory).filePath(QStringLiteral("request.json"));
    if (!writeJsonFile(requestPath, QJsonDocument(request).toJson(QJsonDocument::Indented)) || !writeSubmissionStatus(runDirectory, QStringLiteral("publishing"))) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The first-publication run could not be recorded locally."));
        return;
    }
    QFile::setPermissions(requestPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
    runs.insert(buildTarget,
                QJsonObject {
                    {QStringLiteral("run_id"), runId},
                    {QStringLiteral("trace_id"), runId},
                    {QStringLiteral("project"), project},
                    {QStringLiteral("target"), buildTarget},
                    {QStringLiteral("status"), QStringLiteral("publishing")},
                    {QStringLiteral("first_publication"), true},
                    {QStringLiteral("started_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                });
    state.insert(QStringLiteral("build_runs"), runs);
    state.insert(QStringLiteral("source_publication_requested"), true);
    state.insert(QStringLiteral("source_publication_build_target"), buildTarget);
    state.insert(QStringLiteral("source_publication_project"), project);
    if (!writeRepositoryState(state)) {
        Q_EMIT connectionStateChanged(false, QStringLiteral("The first-publication run could not be journaled locally."));
        return;
    }
    m_targetedBuildTarget = buildTarget;
    setLiveRenderStatus(buildTarget, QJsonObject {{QStringLiteral("status"), QStringLiteral("preparing")}, {QStringLiteral("reason"), QStringLiteral("Preparing local source and exact build inputs.")}});
    Q_EMIT renderStatusChanged(buildTarget, m_renderStatuses.value(buildTarget));
    startPendingSourcePublication();
}

QJsonObject FedoraRepository::buildActivity(const QString &target) const
{
    if (!safePdfFilename(target)) {
        return {};
    }
    const QJsonObject run = readRepositoryState().value(QStringLiteral("build_runs")).toObject().value(target).toObject();
    const QString status = run.value(QStringLiteral("status")).toString();
    if (!activeBuildStatus(status) || run.value(QStringLiteral("sync_origin")).toString() == QLatin1String("manual_conditional")) {
        return {{QStringLiteral("active"), false}};
    }
    QJsonObject activity = run;
    activity.insert(QStringLiteral("active"), true);
    activity.insert(QStringLiteral("run_directory"), buildRunDirectory(run.value(QStringLiteral("run_id")).toString()));
    activity.insert(QStringLiteral("server"), Thoughts::ServerAddress::configured());
    activity.insert(QStringLiteral("watcher"), Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-watch-build")));
    return activity;
}

QJsonObject FedoraRepository::sourcePublicationActivity() const
{
    const QJsonObject state = readRepositoryState();
    const QString target = state.value(QStringLiteral("source_publication_build_target")).toString();
    const QJsonObject run = target.isEmpty() ? state.value(QStringLiteral("source_publication_run")).toObject() : state.value(QStringLiteral("build_runs")).toObject().value(target).toObject();
    const QString runId = run.value(QStringLiteral("run_id")).toString();
    if (runId.isEmpty()) {
        return {{QStringLiteral("active"), false}};
    }
    const QString runDirectory = buildRunDirectory(runId);
    QJsonObject activity = run;
    activity.insert(QStringLiteral("active"), m_publishingSources || state.value(QStringLiteral("source_publication_requested")).toBool());
    activity.insert(QStringLiteral("run_directory"), runDirectory);
    activity.insert(QStringLiteral("log_path"), QDir(runDirectory).filePath(QStringLiteral("source-publication.log")));
    return activity;
}

QVariantMap FedoraRepository::sourceTrackingDiagnostics() const
{
    if (!m_sourceChangeTracker) return {};
    QVariantMap diagnostics = m_sourceChangeTracker->diagnostics();
    diagnostics.insert(QStringLiteral("inventory_read_ms"), m_sourceTrackingLoadMs);
    return diagnostics;
}

QJsonObject FedoraRepository::renderStatusOutsideRenderState(const QJsonObject &renderState, const QJsonObject &publication)
{
    const auto parsed = [](const QJsonValue &value) {
        const QDateTime withMs = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
        return withMs.isValid() ? withMs : QDateTime::fromString(value.toString(), Qt::ISODate);
    };
    // Only a verified, complete render-state lists every build target.
    const QDateTime checkedAt = parsed(renderState.value(QStringLiteral("checked_at")));
    if (!checkedAt.isValid() || !renderState.value(QStringLiteral("documents")).isArray()) {
        return {};
    }
    const QString filename = publication.value(QStringLiteral("filename")).toString();
    if (!safePdfFilename(filename)
        || publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("kind")).toString(QStringLiteral("source_linked")) != QLatin1String("source_linked")) {
        return {};
    }
    for (const QJsonValue &value : renderState.value(QStringLiteral("documents")).toArray()) {
        if (value.toObject().value(QStringLiteral("filename")).toString() == filename) {
            return {};
        }
    }
    // A publication newer than the observation may simply not be in it yet.
    const QDateTime publishedAt = parsed(publication.value(QStringLiteral("published_at")));
    if (!publishedAt.isValid() || publishedAt > checkedAt) {
        return {};
    }
    return {{QStringLiteral("status"), QStringLiteral("synced")},
            {QStringLiteral("not_build_target"), true},
            {QStringLiteral("reason"), QStringLiteral("The server does not build this PDF: its document is not selected in its Project. The last PDF is kept.")},
            {QStringLiteral("checked_at"), renderState.value(QStringLiteral("checked_at"))}};
}

QJsonObject FedoraRepository::renderStatus(const QString &target)
{
    if (!safePdfFilename(target)) {
        return {};
    }
    QJsonObject status = m_renderStatuses.value(target);
    if (status.isEmpty() && !m_renderState.isEmpty()) {
        // Without this a PDF the complete render-state leaves out had no status
        // at all and read as waiting for Sync after every Sync.
        status = renderStatusOutsideRenderState(m_renderState, currentPublicationForTarget(readRepositoryState(), target));
    }
    if (m_restoredTransientRenderStatuses.contains(target)) {
        status.insert(QStringLiteral("restored_from_cache"), true);
    }
    return status;
}

void FedoraRepository::refreshRenderStatus(const QString &target)
{
    if (!safePdfFilename(target) || m_renderStatusRequests.contains(target)) {
        return;
    }
    QJsonObject checking = m_renderStatuses.value(target);
    if (checking.isEmpty()) {
        checking.insert(QStringLiteral("status"), QStringLiteral("unknown"));
    }
    checking.insert(QStringLiteral("checking"), true);
    checking.insert(QStringLiteral("refreshed"), false);
    checking.remove(QStringLiteral("refresh_error"));
    m_renderStatuses.insert(target, checking);
    Q_EMIT renderStatusChanged(target, checking);
    if (!ensureClient()) {
        checking.insert(QStringLiteral("checking"), false);
        checking.insert(QStringLiteral("refresh_error"), QStringLiteral("The server credential or configuration is unavailable."));
        m_renderStatuses.insert(target, checking);
        Q_EMIT renderStatusChanged(target, checking);
        return;
    }

    m_renderStatusRequests.insert(target);
    QNetworkReply *reply = m_client->renderStatus(target);
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        m_renderStatusRequests.remove(target);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = replyError(reply);
            reply->deleteLater();
            QJsonObject retained = m_renderStatuses.value(target);
            if (retained.isEmpty()) {
                retained.insert(QStringLiteral("status"), QStringLiteral("unknown"));
            }
            retained.insert(QStringLiteral("checking"), false);
            retained.insert(QStringLiteral("refreshed"), false);
            retained.insert(QStringLiteral("refresh_error"), error);
            m_renderStatuses.insert(target, retained);
            Q_EMIT renderStatusChanged(target, retained);
            Q_EMIT connectionStateChanged(false, error);
            return;
        }

        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        QJsonObject verified = document.object();
        static const QSet<QString> statuses {QStringLiteral("synced"), QStringLiteral("out_of_sync"), QStringLiteral("building"), QStringLiteral("attention"), QStringLiteral("error")};
        if (!document.isObject() || !statuses.contains(verified.value(QStringLiteral("status")).toString())) {
            QJsonObject retained = m_renderStatuses.value(target);
            if (retained.isEmpty()) {
                retained.insert(QStringLiteral("status"), QStringLiteral("unknown"));
            }
            retained.insert(QStringLiteral("checking"), false);
            retained.insert(QStringLiteral("refreshed"), false);
            retained.insert(QStringLiteral("refresh_error"), QStringLiteral("The server returned an invalid PDF status."));
            m_renderStatuses.insert(target, retained);
            Q_EMIT renderStatusChanged(target, retained);
            return;
        }
        verified.insert(QStringLiteral("checked_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        verified.insert(QStringLiteral("checking"), false);
        verified.insert(QStringLiteral("refreshed"), true);
        verified.remove(QStringLiteral("refresh_error"));
        const QJsonObject publication = publicationWithBuildSourceArtifact(verified.value(QStringLiteral("publication")).toObject(), target, verified.value(QStringLiteral("build")).toObject());
        verified.insert(QStringLiteral("publication"), publication);
        const QJsonObject run = readRepositoryState().value(QStringLiteral("build_runs")).toObject().value(target).toObject();
        const QString runStatus = run.value(QStringLiteral("status")).toString();
        if (runStatus == QLatin1String("activating")) {
            verified.insert(QStringLiteral("local_state"), QStringLiteral("activating"));
        } else if (runStatus == QLatin1String("activation_failed")) {
            verified.insert(QStringLiteral("local_state"), QStringLiteral("error"));
            verified.insert(QStringLiteral("local_error"), run.value(QStringLiteral("message")));
        } else {
            verified.insert(QStringLiteral("local_state"), localPublicationState(target, verified.value(QStringLiteral("publication")).toObject()));
            verified.remove(QStringLiteral("local_error"));
        }
        storeRenderStatus(target, verified);
        if (verified.value(QStringLiteral("status")).toString() == QLatin1String("synced") && verified.value(QStringLiteral("local_state")).toString() == QLatin1String("sync_pending")) {
            QTimer::singleShot(0, this, [this, target] { activatePublishedPdf(target); });
        }
        Q_EMIT connectionStateChanged(true, QString());
    });
}

void FedoraRepository::refreshLatestBuildInputs(const QString &target)
{
    if (!safePdfFilename(target) || m_buildInputSnapshotRequests.contains(target)) {
        return;
    }
    if (!ensureClient()) {
        Q_EMIT buildInputSnapshotChanged(target, {}, QStringLiteral("The server credential or configuration is unavailable."));
        return;
    }
    m_buildInputSnapshotRequests.insert(target);
    QNetworkReply *reply = m_client->latestBuildInputs(target);
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        m_buildInputSnapshotRequests.remove(target);
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = replyError(reply);
            reply->deleteLater();
            Q_EMIT buildInputSnapshotChanged(target, {}, error);
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        reply->deleteLater();
        const QJsonObject snapshot = document.object();
        if (parseError.error != QJsonParseError::NoError || !document.isObject() || snapshot.value(QStringLiteral("target")).toString() != target || !validSha256(snapshot.value(QStringLiteral("sha256")).toString()) ||
            !snapshot.value(QStringLiteral("entries")).isArray()) {
            Q_EMIT buildInputSnapshotChanged(target, {}, QStringLiteral("The server returned invalid build-input provenance."));
            return;
        }
        Q_EMIT buildInputSnapshotChanged(target, snapshot, QString());
    });
}

static QJsonObject localMonitoringInformation(const QString &docsRoot, const QString &target,
                                             const QList<Thoughts::DesktopTrackedPath> &baselines,
                                             const Thoughts::DesktopSyncSnapshot &pending,
                                             bool authoritative, bool reconciling, const QString &error)
{
    QJsonObject result {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("target"), target},
        {QStringLiteral("status"), QStringLiteral("unavailable")},
        {QStringLiteral("authoritative"), false},
        {QStringLiteral("folders"), QJsonArray {}},
        {QStringLiteral("input_count"), 0},
        {QStringLiteral("tracked_file_count"), 0},
        {QStringLiteral("pending_count"), 0},
    };
    if (!safePdfFilename(target)) {
        result.insert(QStringLiteral("error"), QStringLiteral("The document target is invalid."));
        return result;
    }
    if (!error.isEmpty()) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    const auto localFileState = [&docsRoot](const QString &path) {
        const QFileInfo information(QDir(docsRoot).filePath(path));
        const bool exists = information.exists() && information.isFile();
        const qint64 sizeBytes = exists ? information.size() : 0;
        return QJsonObject {
            {QStringLiteral("exists"), exists},
            {QStringLiteral("size_bytes"), sizeBytes},
            {QStringLiteral("size_available"), !exists || sizeBytes >= 0},
        };
    };

    struct FolderInfo {
        QJsonObject descriptor;
        QJsonArray inputs;
    };
    QHash<QString, FolderInfo> byFolder;
    for (const Thoughts::DesktopTrackedPath &path : baselines) {
        if (!path.affectedTargets.contains(target)
            || path.metadata.value(QStringLiteral("kind")).toString()
                != QLatin1String("directory")) {
            continue;
        }
        const QString mode = path.metadata.value(QStringLiteral("_monitoring_mode")).toString();
        if (mode != QLatin1String("discovery") && mode != QLatin1String("parent")) {
            continue;
        }
        FolderInfo folder;
        const bool exists = QFileInfo(QDir(docsRoot).filePath(path.path)).isDir();
        folder.descriptor = QJsonObject {
            {QStringLiteral("path"), path.path},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("project"), path.metadata.value(QStringLiteral("project"))},
            {QStringLiteral("exists"), exists},
        };
        byFolder.insert(path.path, folder);
    }
    const auto folderForPath = [&byFolder](const QString &path) {
        QString selected;
        for (auto iterator = byFolder.cbegin(); iterator != byFolder.cend(); ++iterator) {
            if ((path == iterator.key() || path.startsWith(iterator.key() + QLatin1Char('/')))
                && iterator.key().size() > selected.size()) {
                selected = iterator.key();
            }
        }
        return selected;
    };
    QSet<QString> publishedPaths;
    int inputCount = 0;
    for (const Thoughts::DesktopTrackedPath &path : baselines) {
        if (!path.affectedTargets.contains(target)
            || path.metadata.value(QStringLiteral("kind")).toString()
                != QLatin1String("file")) {
            continue;
        }
        const QString sourceKind = path.metadata.value(QStringLiteral("source_kind")).toString();
        if (sourceKind != QLatin1String("source")
            && sourceKind != QLatin1String("build_input")) {
            continue;
        }
        QString folderPath = folderForPath(path.path);
        if (folderPath.isEmpty()) {
            folderPath = QFileInfo(path.path).path();
            FolderInfo folder;
            folder.descriptor = QJsonObject {
                {QStringLiteral("path"), folderPath},
                {QStringLiteral("mode"), QStringLiteral("parent")},
                {QStringLiteral("project"), path.metadata.value(QStringLiteral("project"))},
                {QStringLiteral("shared"), false},
                {QStringLiteral("exists"), QFileInfo(QDir(docsRoot).filePath(folderPath)).isDir()},
            };
            byFolder.insert(folderPath, folder);
        }
        QJsonObject input = localFileState(path.path);
        input.insert(QStringLiteral("path"), path.path);
        input.insert(QStringLiteral("role"), path.metadata.value(QStringLiteral("role")));
        input.insert(QStringLiteral("source_kind"), sourceKind);
        input.insert(QStringLiteral("pending"), false);
        FolderInfo folder = byFolder.value(folderPath);
        folder.inputs.append(input);
        byFolder.insert(folderPath, folder);
        publishedPaths.insert(path.path);
        ++inputCount;
    }

    int pendingCount = 0;
    for (const Thoughts::DesktopPendingFile &file : pending.files) {
        if (!file.affectedTargets.contains(target)) {
            continue;
        }
        ++pendingCount;
        if (publishedPaths.contains(file.path)) {
            for (auto iterator = byFolder.begin(); iterator != byFolder.end(); ++iterator) {
                for (qsizetype index = 0; index < iterator->inputs.size(); ++index) {
                    QJsonObject input = iterator->inputs.at(index).toObject();
                    if (input.value(QStringLiteral("path")).toString() == file.path) {
                        input.insert(QStringLiteral("pending"), true);
                        input.insert(QStringLiteral("action"), file.action);
                        const QJsonObject localState = localFileState(file.path);
                        input.insert(QStringLiteral("exists"), localState.value(QStringLiteral("exists")));
                        input.insert(QStringLiteral("size_bytes"), localState.value(QStringLiteral("size_bytes")));
                        input.insert(QStringLiteral("size_available"), localState.value(QStringLiteral("size_available")));
                        iterator->inputs[index] = input;
                    }
                }
            }
            continue;
        }
        QString folderPath = folderForPath(file.path);
        if (folderPath.isEmpty()) {
            folderPath = QFileInfo(file.path).path();
            FolderInfo folder;
            folder.descriptor = QJsonObject {
                {QStringLiteral("path"), folderPath},
                {QStringLiteral("mode"), QStringLiteral("parent")},
                {QStringLiteral("exists"), QFileInfo(QDir(docsRoot).filePath(folderPath)).isDir()},
            };
            byFolder.insert(folderPath, folder);
        }
        FolderInfo folder = byFolder.value(folderPath);
        QJsonObject input = localFileState(file.path);
        input.insert(QStringLiteral("path"), file.path);
        input.insert(QStringLiteral("role"), file.metadata.value(QStringLiteral("role")));
        input.insert(QStringLiteral("source_kind"), file.metadata.value(QStringLiteral("source_kind")));
        input.insert(QStringLiteral("pending"), true);
        input.insert(QStringLiteral("action"), file.action);
        folder.inputs.append(input);
        byFolder.insert(folderPath, folder);
    }

    QHash<QString, QJsonObject> effectiveInputs;
    for (auto iterator = byFolder.cbegin(); iterator != byFolder.cend(); ++iterator) {
        for (const QJsonValue &value : iterator->inputs) {
            const QJsonObject input = value.toObject();
            effectiveInputs.insert(input.value(QStringLiteral("path")).toString(), input);
        }
    }

    QStringList folderPaths = byFolder.keys();
    std::sort(folderPaths.begin(), folderPaths.end());
    QJsonArray folders;
    for (const QString &folderPath : std::as_const(folderPaths)) {
        FolderInfo folder = byFolder.value(folderPath);
        QList<QJsonObject> orderedInputs;
        for (const QJsonValue &value : std::as_const(folder.inputs)) {
            orderedInputs.append(value.toObject());
        }
        std::sort(orderedInputs.begin(), orderedInputs.end(), [](const QJsonObject &left, const QJsonObject &right) {
            return left.value(QStringLiteral("path")).toString()
                < right.value(QStringLiteral("path")).toString();
        });
        QJsonArray inputs;
        for (const QJsonObject &input : std::as_const(orderedInputs)) {
            inputs.append(input);
        }
        folder.descriptor.insert(QStringLiteral("inputs"), inputs);
        qint64 trackedBytes = 0;
        bool trackedBytesAvailable = true;
        for (auto iterator = effectiveInputs.cbegin(); iterator != effectiveInputs.cend(); ++iterator) {
            if ((iterator.key() != folderPath
                 && !iterator.key().startsWith(folderPath + QLatin1Char('/')))
                || !iterator->value(QStringLiteral("exists")).toBool()) {
                continue;
            }
            const qint64 inputBytes = iterator->value(QStringLiteral("size_bytes")).toInteger(-1);
            if (!iterator->value(QStringLiteral("size_available")).toBool()
                || inputBytes < 0
                || inputBytes > std::numeric_limits<qint64>::max() - trackedBytes) {
                trackedBytesAvailable = false;
                break;
            }
            trackedBytes += inputBytes;
        }
        folder.descriptor.insert(QStringLiteral("tracked_bytes"), trackedBytesAvailable ? trackedBytes : -1);
        folder.descriptor.insert(QStringLiteral("tracked_bytes_available"), trackedBytesAvailable);
        folders.append(folder.descriptor);
    }
    // Global tracker authority says nothing about a newly imported target that
    // has no baseline yet. Empty coverage is unknown, never current.
    authoritative = authoritative && inputCount > 0;
    result.insert(QStringLiteral("status"), authoritative ? QStringLiteral("current")
                                                           : (reconciling ? QStringLiteral("reconciling")
                                                                          : QStringLiteral("unavailable")));
    result.insert(QStringLiteral("authoritative"), authoritative);
    result.insert(QStringLiteral("folders"), folders);
    result.insert(QStringLiteral("input_count"), inputCount);
    result.insert(QStringLiteral("tracked_file_count"), inputCount);
    result.insert(QStringLiteral("pending_count"), pendingCount);
    if (!authoritative && !reconciling) {
        result.insert(QStringLiteral("error"), QStringLiteral("Local source monitoring has not completed an authoritative check."));
    }
    return result;
}

QJsonObject FedoraRepository::sourceMonitoringInformation(const QString &target)
{
    if (!safePdfFilename(target)) { return localMonitoringInformation(m_docsRoot, target, {}, {}, false, false, {}); }
    QString error;
    Thoughts::DesktopSyncStore *const store = desktopSyncStore(&error);
    const auto baselines = store && error.isEmpty() ? store->baselines(&error) : QList<Thoughts::DesktopTrackedPath>();
    const auto pending = store && error.isEmpty() ? store->freeze(&error) : Thoughts::DesktopSyncSnapshot();
    return localMonitoringInformation(m_docsRoot, target, baselines, pending,
                                      m_sourceChangeTracker && m_sourceChangeTracker->authoritative(),
                                      m_sourceChangeTracker && m_sourceChangeTracker->reconciliationInProgress(), error);
}

void FedoraRepository::requestSourceMonitoringInformation(const QStringList &targets, std::function<void(const QVariantMap &)> completion)
{
    // Summary is a local read. Do not initialize/reconcile the tracker, reuse its
    // SQLite connection on a worker, or enter any network/sync operation here.
    const QString docsRoot = m_docsRoot;
    const QString stateRoot = m_stateRoot;
    const bool authoritative = m_sourceChangeTracker && m_sourceChangeTracker->authoritative();
    const bool reconciling = m_sourceChangeTracker && m_sourceChangeTracker->reconciliationInProgress();
    QPointer<FedoraRepository> guard(this);
    QThreadPool::globalInstance()->start([guard, docsRoot, stateRoot, targets, authoritative, reconciling, completion = std::move(completion)] {
        QString error;
        QList<Thoughts::DesktopTrackedPath> baselines;
        Thoughts::DesktopSyncSnapshot pending;
        Thoughts::DesktopSyncStore::readMonitoringSnapshot(QDir(stateRoot).filePath(QStringLiteral("desktop-sync.sqlite")), &baselines, &pending, &error);
        QVariantMap results;
        for (const QString &target : targets) {
            results.insert(target, localMonitoringInformation(docsRoot, target, baselines, pending, authoritative, reconciling, error).toVariantMap());
        }
        if (!guard) { return; }
        QMetaObject::invokeMethod(guard.data(), [guard, docsRoot, stateRoot, results, completion] {
            if (!guard) { return; }
            if (guard->m_docsRoot != docsRoot || guard->m_stateRoot != stateRoot) { completion({}); return; }
            completion(results);
        }, Qt::QueuedConnection);
    });
}

QJsonObject FedoraRepository::renderState() const
{
    return m_renderState;
}

void FedoraRepository::refreshRenderStateAfterExplicitOperation()
{
    if (m_renderStateRequestRunning) {
        return;
    }
    if (!ensureClient()) {
        QJsonObject retained = m_renderState;
        retained.insert(QStringLiteral("refreshed"), false);
        retained.insert(QStringLiteral("refresh_error"), QStringLiteral("The server credential or configuration is unavailable."));
        Q_EMIT renderStateChanged(retained);
        return;
    }
    m_renderStateRequestRunning = true;
    QNetworkReply *reply = m_client->renderState();
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        m_renderStateRequestRunning = false;
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = replyError(reply);
            reply->deleteLater();
            QJsonObject retained = m_renderState;
            retained.insert(QStringLiteral("refreshed"), false);
            retained.insert(QStringLiteral("refresh_error"), error);
            Q_EMIT renderStateChanged(retained);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        QJsonObject verified = document.object();
        static const QSet<QString> statuses {QStringLiteral("current"), QStringLiteral("changed"), QStringLiteral("building"), QStringLiteral("attention")};
        static const QSet<QString> documentStatuses {QStringLiteral("synced"), QStringLiteral("out_of_sync"), QStringLiteral("building"), QStringLiteral("attention"), QStringLiteral("error")};
        const QJsonArray documents = verified.value(QStringLiteral("documents")).toArray();
        const QJsonObject storage = verified.value(QStringLiteral("storage")).toObject();
        const auto validStorageBytes = [&storage](const QString &key) {
            const QJsonValue value = storage.value(key);
            return value.isDouble() && value.toVariant().toLongLong() >= 0;
        };
        const bool validStorage = !verified.contains(QStringLiteral("storage"))
            || (verified.value(QStringLiteral("storage")).isObject()
                && validStorageBytes(QStringLiteral("total_bytes"))
                && storage.value(QStringLiteral("total_bytes")).toVariant().toLongLong() > 0
                && validStorageBytes(QStringLiteral("used_bytes"))
                && validStorageBytes(QStringLiteral("available_bytes")));
        const QDateTime observedAt = QDateTime::fromString(verified.value(QStringLiteral("observed_at")).toString(), Qt::ISODateWithMs);
        const QString lastSuccessfulBuildAtText = verified.value(QStringLiteral("last_successful_build_at")).toString();
        const QDateTime lastSuccessfulBuildAt = QDateTime::fromString(lastSuccessfulBuildAtText, Qt::ISODateWithMs);
        QHash<QString, QJsonObject> nextStatuses;
        bool validDocuments = verified.value(QStringLiteral("documents")).isArray() && validStorage && observedAt.isValid() && (lastSuccessfulBuildAtText.isEmpty() || lastSuccessfulBuildAt.isValid());
        for (const QJsonValue &value : documents) {
            const QJsonObject remote = value.toObject();
            const QString target = remote.value(QStringLiteral("filename")).toString();
            const QJsonObject publication = remote.value(QStringLiteral("publication")).toObject();
            const QString documentBuiltAtText = remote.value(QStringLiteral("last_successful_build_at")).toString();
            const QDateTime documentBuiltAt = QDateTime::fromString(documentBuiltAtText, Qt::ISODateWithMs);
            if (!value.isObject() || !safePdfFilename(target) || nextStatuses.contains(target) || !documentStatuses.contains(remote.value(QStringLiteral("status")).toString()) ||
                (!documentBuiltAtText.isEmpty() && !documentBuiltAt.isValid()) || (!publication.isEmpty() && publication.value(QStringLiteral("filename")).toString() != target)) {
                validDocuments = false;
                break;
            }
            nextStatuses.insert(target, remote);
        }
        if (!document.isObject() || !statuses.contains(verified.value(QStringLiteral("status")).toString()) || !validDocuments) {
            QJsonObject retained = m_renderState;
            retained.insert(QStringLiteral("refreshed"), false);
            retained.insert(QStringLiteral("refresh_error"), QStringLiteral("The server returned an invalid global PDF state."));
            Q_EMIT renderStateChanged(retained);
            return;
        }
        const QString checkedAt = observedAt.toUTC().toString(Qt::ISODateWithMs);
        verified.insert(QStringLiteral("checked_at"), checkedAt);
        verified.insert(QStringLiteral("refreshed"), true);
        verified.remove(QStringLiteral("refresh_error"));
        QJsonObject persistedStatuses;
        for (auto iterator = nextStatuses.begin(); iterator != nextStatuses.end(); ++iterator) {
            QJsonObject status = iterator.value();
            status.insert(QStringLiteral("checked_at"), checkedAt);
            status.insert(QStringLiteral("checking"), false);
            status.insert(QStringLiteral("refreshed"), true);
            status.remove(QStringLiteral("refresh_error"));
            status.insert(QStringLiteral("local_state"), localPublicationState(iterator.key(), status.value(QStringLiteral("publication")).toObject()));
            iterator.value() = status;
            persistedStatuses.insert(iterator.key(), status);
        }
        const QJsonObject state{{QStringLiteral("render_state"), verified},
                                {QStringLiteral("render_statuses"), persistedStatuses}};
        if (!writeRenderStatusSnapshot(state)) {
            QJsonObject retained = m_renderState;
            retained.insert(QStringLiteral("refreshed"), false);
            retained.insert(QStringLiteral("refresh_error"), QStringLiteral("The server PDF status snapshot could not be persisted."));
            Q_EMIT renderStateChanged(retained);
            return;
        }
        const QHash<QString, QJsonObject> previousStatuses = m_renderStatuses;
        m_renderState = verified;
        m_renderStatuses = nextStatuses;
        m_restoredTransientRenderStatuses.clear();
        QSet<QString> changedTargets(previousStatuses.keyBegin(), previousStatuses.keyEnd());
        changedTargets.unite(QSet<QString>(m_renderStatuses.keyBegin(), m_renderStatuses.keyEnd()));
        for (const QString &target : std::as_const(changedTargets)) {
            if (previousStatuses.value(target) != m_renderStatuses.value(target)) {
                Q_EMIT renderStatusChanged(target, m_renderStatuses.value(target));
            }
        }
        Q_EMIT renderStateChanged(verified);
    });
}

QStringList FedoraRepository::sourcePublicationArguments(const QString &target, const QString &project) const
{
    const QString manifest = m_sourceManifestPath;
    QStringList arguments {QStringLiteral("--root"),
                           m_docsRoot,
                           QStringLiteral("--manifest"),
                           manifest,
                           QStringLiteral("--state"),
                           QDir(m_stateRoot).filePath(QStringLiteral("source-publication.json")),
                           QStringLiteral("--bootstrap-state"),
                           QDir(Thoughts::Platform::thoughtsDataRoot()).filePath(QStringLiteral("source-sync-state.json")),
                           QStringLiteral("--server"),
                           Thoughts::ServerAddress::configured()};
    if (!target.isEmpty()) {
        arguments.append({QStringLiteral("--target"), target, QStringLiteral("--force-build")});
    }
    if (!project.isEmpty()) {
        arguments.append({QStringLiteral("--project"), project});
    }
    return arguments;
}

QString FedoraRepository::buildRunDirectory(const QString &runId) const
{
    return QDir(m_stateRoot).filePath(QStringLiteral("build-runs/%1").arg(runId));
}

void FedoraRepository::startNextDeferredTargetedBuild()
{
    if (m_publishingSources) {
        return;
    }
    const QJsonObject state = readRepositoryState();
    if (state.value(QStringLiteral("source_publication_requested")).toBool()) {
        return;
    }
    const QJsonArray deferred = deferredTargetedBuilds(state);
    if (deferred.isEmpty()) {
        return;
    }
    const QString target = deferred.first().toString();
    if (safePdfFilename(target)) {
        // publishSources() atomically removes this deferred entry while it
        // records the target-specific source-publication run.
        publishSources(target);
    }
}

void FedoraRepository::startPendingSourcePublication()
{
    if (m_publishingSources) {
        return;
    }
    QJsonObject state = readRepositoryState();
    if (!state.value(QStringLiteral("source_publication_requested")).toBool()) {
        return;
    }
    const QString requestedTarget = state.value(QStringLiteral("source_publication_build_target")).toString();
    const QString requestedProject = state.value(QStringLiteral("source_publication_project")).toString();
    QJsonObject run = requestedTarget.isEmpty() ? state.value(QStringLiteral("source_publication_run")).toObject() : state.value(QStringLiteral("build_runs")).toObject().value(requestedTarget).toObject();
    if (run.isEmpty()) {
        const QString message =
            requestedTarget.isEmpty() ? QStringLiteral("The source publication has no durable run identity. Start Sync again explicitly.") : QStringLiteral("The targeted build has no durable run identity. Start Build again explicitly.");
        state.remove(QStringLiteral("source_publication_requested"));
        state.remove(QStringLiteral("source_publication_build_target"));
        state.remove(QStringLiteral("source_publication_project"));
        writeRepositoryState(state);
        Q_EMIT connectionStateChanged(false, message);
        if (m_syncCoordinator.running()) {
            failManualSync(message);
        }
        return;
    }
    const QString runDirectory = buildRunDirectory(run.value(QStringLiteral("run_id")).toString());
    const auto failPermanently = [this, requestedTarget, runDirectory](const QString &message, const QString &errorCode = QString()) {
        QJsonObject failedState = readRepositoryState();
        if (!requestedTarget.isEmpty()) {
            writeSubmissionStatus(runDirectory, QStringLiteral("failed"), message);
            QJsonObject runs = failedState.value(QStringLiteral("build_runs")).toObject();
            QJsonObject failedRun = runs.value(requestedTarget).toObject();
            failedRun.insert(QStringLiteral("status"), QStringLiteral("failed"));
            failedRun.insert(QStringLiteral("message"), message);
            if (!errorCode.isEmpty()) {
                failedRun.insert(QStringLiteral("error_code"), errorCode);
            }
            runs.insert(requestedTarget, failedRun);
            failedState.insert(QStringLiteral("build_runs"), runs);
        } else {
            QJsonObject failedRun = failedState.value(QStringLiteral("source_publication_run")).toObject();
            failedRun.insert(QStringLiteral("status"), QStringLiteral("failed"));
            failedRun.insert(QStringLiteral("message"), message);
            if (!errorCode.isEmpty()) {
                failedRun.insert(QStringLiteral("error_code"), errorCode);
            }
            failedState.insert(QStringLiteral("source_publication_run"), failedRun);
        }
        failedState.remove(QStringLiteral("source_publication_requested"));
        failedState.remove(QStringLiteral("source_publication_build_target"));
        failedState.remove(QStringLiteral("source_publication_project"));
        const QJsonArray deferred = deferredTargetedBuilds(failedState);
        storeDeferredTargetedBuilds(failedState, {});
        QJsonObject statuses = failedState.value(QStringLiteral("render_statuses")).toObject();
        for (const QJsonValue &value : deferred) {
            const QString target = value.toString();
            if (!safePdfFilename(target)) {
                continue;
            }
            QJsonObject failed {{QStringLiteral("status"), QStringLiteral("error")}, {QStringLiteral("reason"), QStringLiteral("The source publication required before this build failed: %1").arg(message)}};
            if (!errorCode.isEmpty()) {
                failed.insert(QStringLiteral("error_code"), errorCode);
            }
            statuses.insert(target, failed);
            setLiveRenderStatus(target, failed);
        }
        failedState.insert(QStringLiteral("render_statuses"), statuses);
        writeRepositoryState(failedState);
        if (!requestedTarget.isEmpty()) {
            QJsonObject failed {{QStringLiteral("status"), QStringLiteral("error")}, {QStringLiteral("reason"), message}};
            if (!errorCode.isEmpty()) {
                failed.insert(QStringLiteral("error_code"), errorCode);
            }
            setLiveRenderStatus(requestedTarget, failed);
            Q_EMIT renderStatusChanged(requestedTarget, m_renderStatuses.value(requestedTarget));
        }
        for (const QJsonValue &value : deferred) {
            const QString target = value.toString();
            if (safePdfFilename(target)) {
                Q_EMIT renderStatusChanged(target, m_renderStatuses.value(target));
            }
        }
        Q_EMIT connectionStateChanged(false, message);
    };
    if (!requestedProject.isEmpty() && run.value(QStringLiteral("status")).toString() == QLatin1String("preparing")) {
        run.insert(QStringLiteral("status"), QStringLiteral("publishing"));
        QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
        runs.insert(requestedTarget, run);
        state.insert(QStringLiteral("build_runs"), runs);
        if (!writeRepositoryState(state) || !writeSubmissionStatus(runDirectory, QStringLiteral("publishing"))) {
            failPermanently(QStringLiteral("The source publication could not be journaled."));
            return;
        }
    }
    const QByteArray token = sourcePublisherToken();
    if (token.isEmpty()) {
        failPermanently(m_sourcePublisherTokenWithheld.isEmpty() ? QStringLiteral("The source-publisher credential for the server is unavailable.") : m_sourcePublisherTokenWithheld);
        return;
    }
    const QString executable = sourcePublisherExecutable();
    const QString manifest = m_sourceManifestPath;
    if (!QFileInfo::exists(executable) || (requestedProject.isEmpty() && !QFileInfo::exists(manifest))) {
        failPermanently(QStringLiteral("The source publisher installed with Thoughts is missing; reinstall Thoughts."));
        return;
    }
    auto *process = new QProcess(this);
    QProcessEnvironment environment = Internal::sourcePublisherProcessEnvironment(token);
    const QString traceId = run.value(QStringLiteral("trace_id")).toString(m_activeTraceId);
    if (!traceId.isEmpty()) {
        applyTraceId(traceId);
    }
    process->setProcessEnvironment(environment);
    process->setProgram(executable);
    const QStringList arguments = sourcePublicationArguments(requestedTarget, requestedProject);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    const QString publicationLog = QDir(runDirectory).filePath(QStringLiteral("source-publication.log"));
    const QString startedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    writeJsonFile(publicationLog, QStringLiteral("[%1] Starting source publication to the server.\n").arg(startedAt).toUtf8());
    const auto stderrBuffer = QSharedPointer<QByteArray>::create();
    const auto consumeSourcePublisherStderr = [this, process, publicationLog, traceId, stderrBuffer](bool flush) {
        const QByteArray incoming = process->readAllStandardError();
        appendLogFile(publicationLog, incoming);
        const QList<QJsonObject> events = Internal::takeSourcePublisherEvents(*stderrBuffer, incoming, flush);
        for (const QJsonObject &event : events) {
            if (!traceId.isEmpty()) {
                if (event.value(QStringLiteral("state")).toString() == QLatin1String("failed")) {
                    process->setProperty("thoughtsFailureMessage", event.value(QStringLiteral("message")).toString());
                    process->setProperty("thoughtsFailureCode", event.value(QStringLiteral("error_code")).toString());
                }
                Q_EMIT syncJournalEvent(traceId, event);
            }
        }
    };
    connect(process, &QProcess::readyReadStandardError, this, [consumeSourcePublisherStderr] { consumeSourcePublisherStderr(false); });
    m_publishingSources = true;
    emitSyncProgress(QStringLiteral("uploading"), 0, 0, QStringLiteral("Publishing sources"));
    connect(process, &QProcess::finished, this, [this, process, requestedTarget, runDirectory, publicationLog, failPermanently, consumeSourcePublisherStderr](int exitCode, QProcess::ExitStatus status) {
        m_publishingSources = false;
        consumeSourcePublisherStderr(true);
        appendLogFile(publicationLog, QStringLiteral("[%1] Source publication command finished with exit code %2.\n").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)).arg(exitCode).toUtf8());
        if (status == QProcess::NormalExit && exitCode == 0) {
            const QJsonObject publication = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
            const qint64 publishedSourceFrontier = publication.value(QStringLiteral("source_frontier")).toInteger(-1);
            const QString buildId = publication.value(QStringLiteral("build_id")).toString();
            if (!requestedTarget.isEmpty() && buildId.isEmpty()) {
                failPermanently(QStringLiteral("The server accepted the sources but did not return the required build identity."));
                process->deleteLater();
                return;
            }
            QJsonObject state = readRepositoryState();
            if (state.value(QStringLiteral("source_publication_build_target")).toString() == requestedTarget) {
                state.remove(QStringLiteral("source_publication_build_target"));
                state.remove(QStringLiteral("source_publication_requested"));
                state.remove(QStringLiteral("source_publication_project"));
            }
            if (requestedTarget.isEmpty()) {
                QJsonObject completedRun = state.value(QStringLiteral("source_publication_run")).toObject();
                completedRun.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
                completedRun.insert(QStringLiteral("finished_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                state.insert(QStringLiteral("source_publication_run"), completedRun);
            }
            if (!requestedTarget.isEmpty() && !buildId.isEmpty()) {
                QJsonArray activeBuilds = state.value(QStringLiteral("active_builds")).toArray();
                activeBuilds.append(QJsonObject {{QStringLiteral("id"), buildId}, {QStringLiteral("target"), requestedTarget}});
                state.insert(QStringLiteral("active_builds"), activeBuilds);
                QJsonObject runs = state.value(QStringLiteral("build_runs")).toObject();
                QJsonObject activeRun = runs.value(requestedTarget).toObject();
                activeRun.insert(QStringLiteral("status"), QStringLiteral("queued"));
                activeRun.insert(QStringLiteral("build_id"), buildId);
                runs.insert(requestedTarget, activeRun);
                state.insert(QStringLiteral("build_runs"), runs);
                writeJsonFile(QDir(runDirectory).filePath(QStringLiteral("build-id")), buildId.toUtf8() + '\n');
                writeSubmissionStatus(runDirectory, QStringLiteral("queued"));
            }
            if (!writeRepositoryState(state)) {
                const QString message = QStringLiteral("The completed source publication could not be journaled safely.");
                failPermanently(message, QStringLiteral("source_publication_journal_failed"));
                if (m_syncCoordinator.running()) {
                    failManualSync(message);
                }
                process->deleteLater();
                return;
            }
            if (requestedTarget.isEmpty()) {
                QString trackingError;
                if (!acceptDesktopSourcePublication(publication, QString(), &trackingError)) {
                    const QString message = QStringLiteral("The published source baseline could not be recorded safely.");
                    if (m_syncCoordinator.running()) {
                        failManualSync(message, trackingError);
                    }
                    process->deleteLater();
                    return;
                }
                if (m_syncCoordinator.running()) {
                    m_syncCoordinator.completeSourcePublication(QString());
                    m_syncCoordinator.sourceSnapshot.reset();
                }
            }
            if (requestedTarget.isEmpty() && publishedSourceFrontier >= 0) {
                m_syncCoordinator.publishedSourceFrontier = publishedSourceFrontier;
            }
            Q_EMIT connectionStateChanged(true, QString());
            if (!requestedTarget.isEmpty()) {
                if (!ensureClient()) {
                    failPermanently(QStringLiteral("The server is not configured or its credentials are unavailable."));
                } else {
                    pollBuilds();
                }
            } else {
                if (m_syncCoordinator.running()) {
                    scheduleManualSyncAdvance();
                }
            }
            QTimer::singleShot(0, this, &FedoraRepository::startNextDeferredTargetedBuild);
        } else {
            QFile failureLog(publicationLog);
            const QString error = failureLog.open(QIODevice::ReadOnly) ? QString::fromUtf8(failureLog.readAll()).trimmed() : QString();
            const QString recordedMessage = process->property("thoughtsFailureMessage").toString().trimmed();
            const QString recordedCode = process->property("thoughtsFailureCode").toString().trimmed();
            const QString message = recordedMessage.isEmpty() ? QStringLiteral("Source publication failed before the server could start the build.") : recordedMessage;
            failPermanently(message, recordedCode.isEmpty() ? QStringLiteral("source_publication_failed") : recordedCode);
            if (m_syncCoordinator.running()) {
                failManualSync(message, error.isEmpty() ? publicationLog : error);
            }
        }
        process->deleteLater();
    });
    process->start();
}

bool FedoraRepository::applyChangePage(const QJsonObject &page, qint64 expectedCursor)
{
    const qint64 nextCursor = page.value(QStringLiteral("next_cursor")).toInteger(-1);
    if (nextCursor < expectedCursor) {
        return false;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject projections = state.value(QStringLiteral("projections")).toObject();
    bool projectionsChanged = false;
    for (const QJsonValue &value : page.value(QStringLiteral("changes")).toArray()) {
        const QJsonObject change = value.toObject();
        if (change.value(QStringLiteral("outcome")).toString() != QLatin1String("applied")) {
            continue;
        }
        const QString incomingType = change.value(QStringLiteral("entity_type")).toString();
        const QString type = incomingType == QLatin1String("comment") ? QStringLiteral("annotation") : incomingType;
        const QString collection = change.value(QStringLiteral("collection")).toString();
        const QString entityId = change.value(QStringLiteral("entity_id")).toString();
        if (!safeComponent(type) || !safeComponent(collection) || !safeComponent(entityId)) {
            continue;
        }
        // Proposal history is retired; only current proposals are kept.
        if (type == QLatin1String("proposal_event")) {
            continue;
        }
        const QString key = type + QLatin1Char('\n') + collection + QLatin1Char('\n') + entityId;
        const QJsonObject payload = change.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("deleted")).toBool() && !keepsTombstones(type)) {
            if (projections.contains(key)) {
                projections.remove(key);
                projectionsChanged = true;
            }
        } else if (change.value(QStringLiteral("payload")).isObject() && projections.value(key) != payload) {
            projections.insert(key, payload);
            projectionsChanged = true;
        }
    }
    if (projectionsChanged) {
        state.insert(QStringLiteral("projections"), projections);
        if (!writeRepositoryState(state)) {
            return false;
        }
    }
    if (!m_outbox->advanceCursor(expectedCursor, nextCursor)) {
        return false;
    }
    if (projectionsChanged) {
        emitEffectiveProjections();
    }
    return true;
}

void FedoraRepository::retireUnusedRepositoryState()
{
    // Every state write serializes and hashes the whole state. Older versions kept
    // ~44k received proposal history events (now retired) and every failed build
    // record (never read) in it.
    static const QLatin1String prefix("proposal_event\nproposals\n");
    QJsonObject state = readRepositoryState();
    const bool hadFailedBuilds = state.contains(QStringLiteral("failed_builds"));
    state.remove(QStringLiteral("failed_builds"));
    QJsonObject projections = state.value(QStringLiteral("projections")).toObject();
    int removed = 0;
    for (auto it = projections.begin(); it != projections.end();) {
        if (it.key().startsWith(prefix)) {
            it = projections.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed == 0 && !hadFailedBuilds) {
        return;
    }
    state.insert(QStringLiteral("projections"), projections);
    if (!writeRepositoryState(state)) {
        qWarning() << "Could not remove retired records from the repository state.";
        return;
    }
    if (removed > 0) {
        Thoughts::RepositoryProjectionStore(m_stateRoot).purgeKeys(prefix);
        qInfo() << "Removed" << removed << "retired proposal history records from the repository state.";
    }
}

bool FedoraRepository::applyProjectionSnapshot(const QJsonObject &snapshot)
{
    const qint64 frontier = snapshot.value(QStringLiteral("snapshot_sequence")).toInteger(-1);
    const qint64 cursor = m_outbox->cursor();
    if (frontier < cursor) {
        return false;
    }
    QJsonObject state = readRepositoryState();
    QJsonObject projections;
    const QJsonObject previous = state.value(QStringLiteral("projections")).toObject();
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        if (isKeptTombstone(it.key().section(QLatin1Char('\n'), 0, 0), it.value().toObject())) {
            projections.insert(it.key(), it.value());
        }
    }
    for (const QJsonValue &value : snapshot.value(QStringLiteral("projections")).toArray()) {
        const QJsonObject projection = value.toObject();
        const QString incomingType = projection.value(QStringLiteral("entity_type")).toString();
        const QString type = incomingType == QLatin1String("comment") ? QStringLiteral("annotation") : incomingType;
        const QString collection = projection.value(QStringLiteral("collection")).toString();
        const QString entityId = projection.value(QStringLiteral("entity_id")).toString();
        QJsonObject payload = projection.value(QStringLiteral("payload")).toObject();
        const bool deleted = projection.value(QStringLiteral("deleted")).toBool() || payload.value(QStringLiteral("deleted")).toBool();
        if (!safeComponent(type) || !safeComponent(collection) || !safeComponent(entityId) || !projection.value(QStringLiteral("payload")).isObject() || (deleted && !isCommentProjection(type))
            || type == QLatin1String("proposal_event")) {
            continue;
        }
        if (deleted) {
            payload.insert(QStringLiteral("deleted"), true);
        }
        projections.insert(type + QLatin1Char('\n') + collection + QLatin1Char('\n') + entityId, payload);
    }
    state.insert(QStringLiteral("projections"), projections);
    state.insert(QStringLiteral("bootstrapped"), true);
    if (!writeRepositoryState(state) || !m_outbox->advanceCursor(cursor, frontier)) {
        return false;
    }
    emitEffectiveProjections();
    return true;
}

}


void Okular::FedoraRepository::requestBuildSourceDiff(const QString &buildId)
{
    if (QUuid(buildId).isNull() || !ensureClient()) {
        Q_EMIT buildSourceDiffReady(buildId, {}, QStringLiteral("The build or server connection is unavailable."));
        return;
    }
    QNetworkReply *reply = m_client->buildSourceDiff(buildId);
    connect(reply, &QNetworkReply::finished, this, [this, reply, buildId] {
        QString error;
        QJsonObject diff;
        if (reply->error() != QNetworkReply::NoError) {
            error = replyError(reply);
        } else {
            diff = QJsonDocument::fromJson(reply->readAll()).object();
            if (diff.value(QStringLiteral("build_id")).toString() != buildId || !diff.value(QStringLiteral("documents")).isArray()) {
                error = QStringLiteral("The server returned an invalid source comparison.");
            }
        }
        reply->deleteLater();
        Q_EMIT buildSourceDiffReady(buildId, diff, error);
    });
}
