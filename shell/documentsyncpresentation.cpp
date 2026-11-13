/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "documentsyncpresentation.h"
#include "../part/sourcefoldernames.h"
#include "../part/thoughtshome.h"
#include "../part/thoughtsplatform.h"
#include "syncoperationstate.h"
#include "syncstatuslabels.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QHash>
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <limits>

namespace Thoughts
{

static QString documentSyncStatusId(DocumentSyncStatus status)
{
    switch (status) {
    case DocumentSyncStatus::Current:
        return QStringLiteral("current");
    case DocumentSyncStatus::SyncNeeded:
        return QStringLiteral("sync_needed");
    case DocumentSyncStatus::BuildNeeded:
        return QStringLiteral("build_needed");
    case DocumentSyncStatus::Syncing:
        return QStringLiteral("syncing");
    case DocumentSyncStatus::WaitingForSourcePublication:
        return QStringLiteral("waiting_source_publication");
    case DocumentSyncStatus::Building:
        return QStringLiteral("building");
    case DocumentSyncStatus::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("sync_needed");
}

static QString documentSyncStatusLabel(DocumentSyncStatus status)
{
    switch (status) {
    case DocumentSyncStatus::Current:
        return i18n("Synced");
    case DocumentSyncStatus::SyncNeeded:
        return i18n("Sync needed");
    case DocumentSyncStatus::BuildNeeded:
        return i18n("Build needed");
    case DocumentSyncStatus::Syncing:
        return SyncStatusLabels::syncing();
    case DocumentSyncStatus::WaitingForSourcePublication:
        return i18n("Waiting to publish sources…");
    case DocumentSyncStatus::Building:
        return SyncStatusLabels::syncing();
    case DocumentSyncStatus::Error:
        return i18n("Error");
    }
    return i18n("Sync needed");
}

static void setDocumentSyncStatus(DocumentSyncPresentation &presentation, DocumentSyncStatus status)
{
    presentation.state = status;
    presentation.status = documentSyncStatusLabel(status);
}

bool DocumentSyncPresentation::requiresBuild() const
{
    return action == DocumentSyncAction::BuildAndSync;
}

bool DocumentSyncPresentation::requiresSync() const
{
    return action == DocumentSyncAction::Sync;
}

QJsonObject DocumentSyncPresentation::toJson() const
{
    return {
        {QStringLiteral("state"), documentSyncStatusId(state)},
        {QStringLiteral("status"), status},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("action_label"), actionLabel},
        {QStringLiteral("action_id"), actionId},
        {QStringLiteral("icon"), iconName},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("last_build_failed"), lastBuildFailed},
        {QStringLiteral("requires_build"), requiresBuild()},
        {QStringLiteral("requires_sync"), requiresSync()},
    };
}

QJsonObject AggregateSyncPresentation::toJson() const
{
    return {
        {QStringLiteral("state"), documentSyncStatusId(state)},
        {QStringLiteral("status"), status},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("action_label"), actionLabel},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("requires_build"), requiresBuild},
        {QStringLiteral("requires_sync"), requiresSync},
        {QStringLiteral("pending_documents"), pendingDocuments},
    };
}

DocumentSyncPresentation documentSyncPresentation(const DocumentSyncState &state)
{
    DocumentSyncPresentation result;
    const QString remoteState = state.renderStatus.value(QStringLiteral("status")).toString();
    const QString localState = state.renderStatus.value(QStringLiteral("local_state")).toString();
    const QString localError = state.renderStatus.value(QStringLiteral("local_error")).toString().trimmed();
    const QString remoteReason = state.renderStatus.value(QStringLiteral("reason")).toString().trimmed();
    const bool remoteChecking = state.renderStatus.value(QStringLiteral("checking")).toBool();
    const QString refreshError = state.renderStatus.value(QStringLiteral("refresh_error")).toString().trimmed();
    const QString activityStatus = state.buildActivity.value(QStringLiteral("status")).toString();
    const bool restoredTransientStatus = state.renderStatus.value(QStringLiteral("restored_from_cache")).toBool();

    if (state.manualSyncState == 1) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Syncing);
        result.reason = state.manualSyncMessage.isEmpty() ? i18n("Sending local changes and receiving server updates.") : state.manualSyncMessage;
        result.actionLabel = SyncStatusLabels::syncing();
        result.actionId = QStringLiteral("syncing");
        result.iconName = QStringLiteral("view-refresh");
        result.action = DocumentSyncAction::None;
        result.enabled = false;
        return result;
    }

    if (state.sourceLinked && (activityStatus == QLatin1String("activating") || localState == QLatin1String("activating"))) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Syncing);
        result.reason = remoteReason.isEmpty() ? i18n("Downloading the complete verified Library generation from the server and activating it on this computer.") : remoteReason;
        result.actionLabel = SyncStatusLabels::syncing();
        result.actionId = QStringLiteral("syncing");
        result.iconName = QStringLiteral("document-save");
        result.action = DocumentSyncAction::None;
        result.enabled = false;
        return result;
    }

    if (state.sourceLinked && !restoredTransientStatus && remoteState == QLatin1String("publishing")
        && state.renderStatus.value(QStringLiteral("queued")).toBool()) {
        setDocumentSyncStatus(result, DocumentSyncStatus::WaitingForSourcePublication);
        result.reason = remoteReason.isEmpty() ? i18n("Waiting for the active source publication before the server can accept this build.") : remoteReason;
        result.actionLabel = i18n("Waiting to publish sources…");
        result.actionId = QStringLiteral("waiting-for-source-publication");
        result.iconName = QStringLiteral("document-send");
        result.action = DocumentSyncAction::None;
        result.enabled = false;
        return result;
    }

    if (state.sourceLinked
        && (state.buildActivity.value(QStringLiteral("active")).toBool()
            || (!restoredTransientStatus && remoteState == QLatin1String("building")))) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Building);
        result.reason = remoteReason.isEmpty() ? i18n("The server is building this PDF. Click to open its live output in Terminal.") : remoteReason + QLatin1Char('\n') + i18n("Click to open its live output in Terminal.");
        result.actionLabel = i18n("Sync in progress — click for output");
        result.actionId = QStringLiteral("build-active");
        result.iconName = QStringLiteral("system-run");
        result.action = DocumentSyncAction::OpenBuild;
        return result;
    }

    // Source freshness determines the next action, not the outcome of the last build.
    // Active builds return above and supersede the previous failure.
    result.lastBuildFailed = state.sourceLinked
        && (remoteState == QLatin1String("error") || remoteState == QLatin1String("attention")
            || (state.libraryPdfStale && !state.latestBuildMessage.isEmpty()));

    const int pendingComments = state.pendingDocumentSync.value(QStringLiteral("comments")).toInt();
    const int pendingReviewCards = state.pendingDocumentSync.value(QStringLiteral("review_cards")).toInt();
    const int pendingReviewEvents = state.pendingDocumentSync.value(QStringLiteral("review_events")).toInt();
    const int pendingTotal = state.pendingDocumentSync.value(QStringLiteral("total")).toInt();
    QStringList pendingDetails;
    if (pendingComments > 0) {
        pendingDetails.append(i18np("one unsynced comment", "%1 unsynced comments", pendingComments));
    }
    if (pendingReviewCards > 0) {
        pendingDetails.append(i18np("one unsynced review card", "%1 unsynced review cards", pendingReviewCards));
    }
    if (pendingReviewEvents > 0) {
        pendingDetails.append(i18np("one unsynced review event", "%1 unsynced review events", pendingReviewEvents));
    }

    if (remoteChecking && state.sourceLinked) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Syncing);
        result.reason = i18n("Reading the authoritative server status without starting a build.");
        result.action = DocumentSyncAction::None;
        result.enabled = false;
    } else if (state.manualSyncState == 3) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Error);
        result.reason = state.manualSyncMessage.isEmpty() ? i18n("The local changes remain safely stored on this computer.") : state.manualSyncMessage;
        if (!state.manualSyncDetails.isEmpty()) {
            result.reason += QLatin1Char('\n') + state.manualSyncDetails;
        }
        result.action = DocumentSyncAction::Sync;
    } else if (state.sourceLinked && !state.buildTarget) {
        // Fedora keeps this PDF but builds nothing for it, so only local
        // comment or review changes can be waiting for a Sync.
        if (pendingTotal > 0) {
            setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
            result.reason = pendingDetails.isEmpty() ? i18n("This PDF has local changes waiting to sync.") : i18n("Waiting to sync: %1.", pendingDetails.join(i18n(", ")));
            result.action = DocumentSyncAction::Sync;
        } else {
            setDocumentSyncStatus(result, DocumentSyncStatus::Current);
            result.reason = remoteReason.isEmpty()
                ? i18n("The server does not build this PDF: its document is not selected in its Project. The last PDF is kept.")
                : remoteReason;
            result.action = DocumentSyncAction::None;
        }
    } else if (!state.sourceLinked) {
        if (pendingTotal > 0) {
            setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
            result.reason = pendingDetails.isEmpty() ? i18n("This PDF has local changes waiting to sync.") : i18n("Waiting to sync: %1.", pendingDetails.join(i18n(", ")));
            result.action = DocumentSyncAction::Sync;
        } else {
            setDocumentSyncStatus(result, DocumentSyncStatus::Current);
            result.reason = i18n("This unlinked PDF has no local changes waiting to sync.");
            result.action = DocumentSyncAction::None;
        }
    } else if (state.pendingDocumentSync.contains(QStringLiteral("source_coverage_available"))
               && !state.pendingDocumentSync.value(QStringLiteral("source_coverage_available")).toBool()) {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = i18n("Sync must check source tracking for the current Library before this PDF can be marked synced.");
        result.action = DocumentSyncAction::Sync;
    } else if (!state.deferLocalFreshness && !state.localFreshnessAvailable) {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = state.localFreshnessMessage.isEmpty() ? i18n("Sync must publish local sources and read the authoritative server status.") : state.localFreshnessMessage;
        result.action = DocumentSyncAction::Sync;
    } else if (!state.deferLocalFreshness && !state.localSourcesCurrent) {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = state.localFreshnessMessage.isEmpty() ? i18n("Local source changes must be published before the server can decide whether a build is required.") : state.localFreshnessMessage;
        result.action = DocumentSyncAction::Sync;
    } else if (pendingTotal > 0) {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = pendingDetails.isEmpty() ? i18n("This PDF has local changes waiting to sync.") : i18n("Waiting to sync: %1.", pendingDetails.join(i18n(", ")));
        result.action = DocumentSyncAction::Sync;
    } else if (remoteState == QLatin1String("synced") && (localState == QLatin1String("sync_pending") || localState == QLatin1String("error"))) {
        setDocumentSyncStatus(result, localState == QLatin1String("error") ? DocumentSyncStatus::Error : DocumentSyncStatus::SyncNeeded);
        result.reason = localState == QLatin1String("error")
            ? (localError.isEmpty() ? i18n("The server has the current PDF, but this computer could not activate the verified Library generation.") : localError)
            : i18n("The server has a newer verified PDF that Sync will activate on this computer.");
        result.action = DocumentSyncAction::Sync;
    } else if (remoteState == QLatin1String("attention")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Error);
        result.reason = remoteReason.isEmpty() ? i18n("The latest server build retained the last-good PDF and needs attention.") : remoteReason;
        result.action = DocumentSyncAction::BuildAndSync;
    } else if (remoteState == QLatin1String("error")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Error);
        result.reason = remoteReason.isEmpty() ? (state.latestBuildMessage.isEmpty() ? i18n("The latest server build failed.") : state.latestBuildMessage) : remoteReason;
        result.action = DocumentSyncAction::BuildAndSync;
    } else if (remoteState == QLatin1String("out_of_sync")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::BuildNeeded);
        result.reason = remoteReason.isEmpty() ? i18n("The server must build this PDF from the synchronized sources.") : remoteReason;
        result.action = DocumentSyncAction::BuildAndSync;
    } else if (state.libraryPdfStale) {
        setDocumentSyncStatus(result, state.latestBuildMessage.isEmpty() ? DocumentSyncStatus::BuildNeeded : DocumentSyncStatus::Error);
        result.reason = state.latestBuildMessage.isEmpty()
            ? i18n("The server must build this PDF from the synchronized sources.")
            : state.latestBuildMessage;
        result.action = DocumentSyncAction::BuildAndSync;
    } else if (restoredTransientStatus && localState == QLatin1String("current")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Current);
        result.reason = i18n("The local PDF matches the last synchronized publication. Sync to refresh the server status.");
        result.action = DocumentSyncAction::None;
    } else if (remoteState == QLatin1String("synced") && localState == QLatin1String("current")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::Current);
        result.reason = remoteReason;
        result.action = DocumentSyncAction::None;
    } else if (remoteState == QLatin1String("synced") && localState == QLatin1String("unknown")) {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = i18n("Sync must obtain enough publication identity to verify this computer's PDF.");
        result.action = DocumentSyncAction::Sync;
    } else {
        setDocumentSyncStatus(result, DocumentSyncStatus::SyncNeeded);
        result.reason = i18n("Sync must read the authoritative server status.");
        result.action = DocumentSyncAction::Sync;
    }

    if (!refreshError.isEmpty()) {
        if (!result.reason.isEmpty()) {
            result.reason += QLatin1Char('\n');
        }
        result.reason += i18n("Not refreshed: %1", refreshError);
    }

    switch (result.action) {
    case DocumentSyncAction::Sync:
        result.actionLabel = i18n("Sync");
        result.actionId = QStringLiteral("sync-now");
        break;
    case DocumentSyncAction::BuildAndSync:
        result.actionLabel = i18n("Build");
        result.actionId = QStringLiteral("build-and-sync-now");
        break;
    case DocumentSyncAction::OpenBuild:
    case DocumentSyncAction::None:
        break;
    }
    result.iconName = result.state == DocumentSyncStatus::Error
        ? QStringLiteral("dialog-warning")
        : QStringLiteral("view-refresh");
    return result;
}

AggregateSyncPresentation aggregateSyncPresentation(const QList<DocumentSyncPresentation> &documents)
{
    AggregateSyncPresentation result;
    int buildNeeded = 0;
    int syncNeeded = 0;
    int syncing = 0;
    int waiting = 0;
    int building = 0;
    int synced = 0;
    int errors = 0;
    int syncErrors = 0;
    int buildErrors = 0;
    for (const DocumentSyncPresentation &document : documents) {
        switch (document.state) {
        case DocumentSyncStatus::BuildNeeded:
            ++buildNeeded;
            break;
        case DocumentSyncStatus::SyncNeeded:
            ++syncNeeded;
            break;
        case DocumentSyncStatus::Syncing:
            ++syncing;
            break;
        case DocumentSyncStatus::WaitingForSourcePublication:
            ++waiting;
            break;
        case DocumentSyncStatus::Building:
            ++building;
            break;
        case DocumentSyncStatus::Current:
            ++synced;
            break;
        case DocumentSyncStatus::Error:
            ++errors;
            syncErrors += document.requiresSync() ? 1 : 0;
            buildErrors += document.requiresBuild() ? 1 : 0;
            break;
        }
    }
    result.requiresSync = syncNeeded > 0 || syncErrors > 0;
    result.requiresBuild = !result.requiresSync && (buildNeeded > 0 || buildErrors > 0);
    result.pendingDocuments = buildNeeded + syncNeeded + errors;
    const int buildCount = buildNeeded + buildErrors;
    result.actionLabel = result.requiresBuild
        ? i18np("Build 1 PDF", "Build %1 PDFs", buildCount)
        : i18n("Sync");

    QStringList counts;
    const auto appendCount = [&counts](int count, const QString &label) {
        if (count > 0) {
            counts.append(QStringLiteral("%1 %2").arg(count).arg(label));
        }
    };
    appendCount(synced, i18n("Synced"));
    appendCount(syncNeeded, i18n("Sync needed"));
    appendCount(buildNeeded, i18n("Build needed"));
    appendCount(syncing + building, SyncStatusLabels::syncing());
    appendCount(waiting, i18n("Waiting to publish sources…"));
    appendCount(errors, i18n("Error"));
    result.reason = counts.join(QStringLiteral(" · "));

    if (documents.isEmpty()) {
        result.state = DocumentSyncStatus::Current;
        result.reason = i18n("0 Synced");
        result.enabled = true;
    } else if (errors > 0) {
        result.state = DocumentSyncStatus::Error;
    } else if (building > 0) {
        result.state = DocumentSyncStatus::Building;
    } else if (waiting > 0) {
        result.state = DocumentSyncStatus::WaitingForSourcePublication;
    } else if (syncing > 0) {
        result.state = DocumentSyncStatus::Syncing;
    } else if (syncNeeded > 0) {
        result.state = DocumentSyncStatus::SyncNeeded;
    } else if (buildNeeded > 0) {
        result.state = DocumentSyncStatus::BuildNeeded;
    } else {
        result.state = DocumentSyncStatus::Current;
    }
    result.status = documentSyncStatusLabel(result.state);
    return result;
}

QJsonObject latestDocumentSyncOperation(const QJsonArray &operations, const QString &target)
{
    QJsonObject latest;
    qint64 latestUpdatedAt = -1;
    for (const QJsonValue &value : operations) {
        const QJsonObject operation = value.toObject();
        if (operation.value(QStringLiteral("kind")).toString() != QLatin1String("sync")
            || operation.value(QStringLiteral("target")).toString() != target) {
            continue;
        }
        const qint64 updatedAt = operation.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong();
        if (updatedAt > latestUpdatedAt) {
            latest = operation;
            latestUpdatedAt = updatedAt;
        }
    }
    return latest;
}

QJsonArray reconcileSyncOperationsAfterRestart(const QJsonArray &operations, qint64 nowMs)
{
    QJsonArray reconciled = operations;
    for (qsizetype index = 0; index < reconciled.size(); ++index) {
        QJsonObject operation = reconciled.at(index).toObject();
        if (!syncOperationIsRunning(operation)) {
            continue;
        }
        const qint64 startedAt = operation.value(operation.contains(QStringLiteral("started_at_ms")) ? QStringLiteral("started_at_ms") : QStringLiteral("created_at_ms"))
                                     .toVariant()
                                     .toLongLong();
        const qint64 duration = qMax<qint64>(0, nowMs - startedAt);
        QJsonArray events = operation.value(QStringLiteral("events")).toArray();
        events.append(QJsonObject {{QStringLiteral("at_ms"), nowMs},
                                   {QStringLiteral("phase"), QStringLiteral("interrupted")},
                                   {QStringLiteral("state"), QStringLiteral("interrupted")},
                                   {QStringLiteral("duration_ms"), duration},
                                   {QStringLiteral("message"), i18n("Interrupted when Thoughts restarted")}});
        operation.insert(QStringLiteral("status"), QStringLiteral("interrupted"));
        operation.insert(QStringLiteral("phase"), QStringLiteral("interrupted"));
        operation.insert(QStringLiteral("updated_at_ms"), nowMs);
        operation.insert(QStringLiteral("completed_at_ms"), nowMs);
        operation.insert(QStringLiteral("duration_ms"), duration);
        operation.insert(QStringLiteral("events"), events);
        reconciled.replace(index, operation);
    }
    return reconciled;
}

QString syncElapsedSecondsLabel(qint64 elapsedSeconds)
{
    return i18np("%1 second elapsed", "%1 seconds elapsed", qMax<qint64>(0, elapsedSeconds));
}

QString compactSyncActivityAge(qint64 elapsedSeconds)
{
    if (elapsedSeconds < 60) {
        return i18n("just now");
    }
    if (elapsedSeconds < 60 * 60) {
        return i18n("%1m ago", elapsedSeconds / 60);
    }
    if (elapsedSeconds < 24 * 60 * 60) {
        return i18n("%1h ago", elapsedSeconds / (60 * 60));
    }
    return i18n("%1d ago", elapsedSeconds / (24 * 60 * 60));
}

QString pendingSyncActivityLabel(const QJsonObject &pendingChanges, int pendingDocuments)
{
    if (!pendingChanges.value(QStringLiteral("available")).toBool()) {
        return i18n("Checking local changes…");
    }

    if (pendingDocuments < 0) {
        pendingDocuments = 0;
        const QJsonObject documents = pendingChanges.value(QStringLiteral("documents")).toObject();
        for (auto iterator = documents.constBegin(); iterator != documents.constEnd(); ++iterator) {
            if (iterator.value().toObject().value(QStringLiteral("total")).toInt() > 0) {
                ++pendingDocuments;
            }
        }
    }
    if (pendingDocuments > 0) {
        return SyncStatusLabels::changesPending();
    }

    const int total = pendingChanges.value(QStringLiteral("total")).toInt();
    if (total <= 0) {
        if (pendingChanges.value(QStringLiteral("source_state")).toString() == QLatin1String("unknown")) {
            return i18n("Checking local changes…");
        }
        return i18n("No pending changes");
    }
    return SyncStatusLabels::changesPending();
}

static bool syncOperationHasBuildFailure(const QJsonObject &operation)
{
    const QString code = operation.value(QStringLiteral("last_error_code")).toString();
    return code == QLatin1String("pdf_build_failed") || code == QLatin1String("build_failed");
}

// A source conflict outranks a build failure: the changes cannot sync until merged.
static QString syncOperationFailureLabel(const QJsonObject &operation)
{
    return syncOperationReportsPreservedConflict(operation) ? SyncStatusLabels::mergeNeeded()
        : syncOperationHasBuildFailure(operation)       ? SyncStatusLabels::pdfBuildFailed()
                                                        : SyncStatusLabels::syncFailed();
}

QString syncOperationHistoryLabel(const QJsonObject &operation, qint64 nowMs)
{
    if (operation.isEmpty()) {
        return i18n("Last sync: Never");
    }
    const SyncOperationState lifecycle = syncOperationState(operation);
    if (lifecycle == SyncOperationState::Running) {
        return documentSyncActivityDetailLabel(operation, nowMs);
    }
    const qint64 referenceMs = operation.value(QStringLiteral("updated_at_ms")).toVariant().toLongLong();
    const QString relative = compactSyncActivityAge(qMax<qint64>(0, nowMs - referenceMs) / 1000);
    if (lifecycle == SyncOperationState::Succeeded) {
        return i18n("Last sync: %1", relative);
    }
    if (lifecycle == SyncOperationState::Failed || lifecycle == SyncOperationState::Interrupted || lifecycle == SyncOperationState::Cancelled) {
        return i18n("Last attempt: %1", relative);
    }
    return i18n("Last sync: Never");
}

SyncActivityPresentation syncActivityPresentation(const QJsonObject &operation,
                                                  const QJsonObject &pendingChanges,
                                                  qint64 nowMs,
                                                  int pendingDocuments)
{
    if (syncOperationState(operation) == SyncOperationState::Running) {
        return {documentSyncActivityLabel(operation, nowMs), documentSyncActivityDetailLabel(operation, nowMs)};
    }
    if (syncOperationState(operation) == SyncOperationState::Failed) {
        return {syncOperationDisplayStatus(operation), syncOperationHistoryLabel(operation, nowMs)};
    }
    QString status = pendingSyncActivityLabel(pendingChanges, pendingDocuments);
    if (syncOperationState(operation) == SyncOperationState::Succeeded
        && operation.value(QStringLiteral("kind")).toString() == QLatin1String("sync")
        && status == i18n("No pending changes")) {
        status = SyncStatusLabels::upToDate();
    }
    return {status, syncOperationHistoryLabel(operation, nowMs)};
}

QString documentSyncActivityLabel(const QJsonObject &operation, qint64 nowMs)
{
    if (operation.isEmpty()) {
        return i18n("Never synced");
    }

    const SyncOperationState lifecycle = syncOperationState(operation);
    const bool running = lifecycle == SyncOperationState::Running;
    const qint64 referenceMs = operation.value(running ? QStringLiteral("created_at_ms") : QStringLiteral("updated_at_ms"))
                                   .toVariant()
                                   .toLongLong();
    const qint64 elapsedSeconds = qMax<qint64>(0, nowMs - referenceMs) / 1000;
    if (running) {
        return syncOperationDisplayStatus(operation);
    }
    const QString relative = compactSyncActivityAge(elapsedSeconds);
    if (lifecycle == SyncOperationState::Succeeded) {
        return i18n("Synced %1", relative);
    }
    if (lifecycle == SyncOperationState::Failed || lifecycle == SyncOperationState::Interrupted || lifecycle == SyncOperationState::Cancelled) {
        const QString label = syncOperationFailureLabel(operation);
        return QStringLiteral("%1 · %2").arg(label, relative);
    }
    return i18n("Never synced");
}

QString documentSyncActivityDetailLabel(const QJsonObject &operation, qint64 nowMs)
{
    if (syncOperationState(operation) != SyncOperationState::Running) {
        return {};
    }
    const qint64 referenceMs = operation.value(QStringLiteral("created_at_ms")).toVariant().toLongLong();
    const qint64 elapsedSeconds = qMax<qint64>(0, nowMs - referenceMs) / 1000;
    return syncElapsedSecondsLabel(elapsedSeconds);
}

QString syncOperationDisplayStatus(const QJsonObject &operation)
{
    const SyncOperationState lifecycle = syncOperationState(operation);
    switch (lifecycle) {
    case SyncOperationState::Succeeded:
        return operation.value(QStringLiteral("kind")).toString() == QLatin1String("build") ? i18n("Built") : i18n("Synced");
    case SyncOperationState::Failed:
        return syncOperationFailureLabel(operation);
    case SyncOperationState::Interrupted:
        return i18n("Interrupted");
    case SyncOperationState::Cancelled:
        return i18n("Cancelled");
    case SyncOperationState::Running:
    {
        const QString phase = operation.value(QStringLiteral("phase")).toString().trimmed().toLower();
        if (phase == QLatin1String("building")) {
            return SyncStatusLabels::syncing();
        }
        if (phase == QLatin1String("waiting_source_publication")) {
            return i18n("Waiting to publish sources…");
        }
        return SyncStatusLabels::syncing();
    }
    case SyncOperationState::Unknown:
        break;
    }
    const QString legacyStatus = operation.value(QStringLiteral("status")).toString();
    return legacyStatus.isEmpty() ? i18n("Unknown") : legacyStatus;
}

QString syncOperationLifecycleTitle(const QJsonObject &operation)
{
    const bool build = operation.value(QStringLiteral("kind")).toString() == QLatin1String("build");
    const SyncOperationState lifecycle = syncOperationState(operation);
    switch (lifecycle) {
    case SyncOperationState::Succeeded:
        return build ? i18n("Build complete") : i18n("Sync complete");
    case SyncOperationState::Failed:
        return syncOperationFailureLabel(operation);
    case SyncOperationState::Interrupted:
    case SyncOperationState::Cancelled:
        return build ? i18n("Build interrupted") : SyncStatusLabels::syncFailed();
    case SyncOperationState::Running:
    {
        const QString phase = operation.value(QStringLiteral("phase")).toString();
        const QString legacyStatus = operation.value(QStringLiteral("status")).toString().trimmed();
        const bool waiting = phase == QLatin1String("queued") || phase.startsWith(QLatin1String("waiting_"))
            || legacyStatus.compare(QStringLiteral("queued"), Qt::CaseInsensitive) == 0;
        if (waiting) {
            return build ? i18n("Waiting to build") : i18n("Waiting to sync");
        }
        return build ? i18n("Building") : i18n("Syncing");
    }
    case SyncOperationState::Unknown:
        return build ? i18n("Build") : i18n("Sync");
    }
    return build ? i18n("Build") : i18n("Sync");
}

static QString readableProjectFailure(const QString &reason)
{
    if (reason.contains(QLatin1String("strict review correspondence gate failed"), Qt::CaseInsensitive))
        return i18n("A proposed edit could not be matched to its location in the updated PDF.");
    if (reason.contains(QLatin1String("attachment is unavailable"), Qt::CaseInsensitive)) {
        static const QRegularExpression attachment(QStringLiteral("attachment is unavailable:\\s*([^;\\r\\n]+)"));
        const QString filename = attachment.match(reason).captured(1).trimmed();
        return filename.isEmpty() ? i18n("A required attachment is missing.") : i18n("Missing attachment: %1", filename);
    }
    if (reason.contains(QLatin1String("cancel"), Qt::CaseInsensitive)) return i18n("The build was cancelled.");
    if (reason.contains(QLatin1String("latex"), Qt::CaseInsensitive) || reason.contains(QLatin1String("Missing $ inserted")))
        return i18n("A formatting error prevented the PDF from building.");
    if (reason.contains(QLatin1String("timeout"), Qt::CaseInsensitive) || reason.contains(QLatin1String("timed out"), Qt::CaseInsensitive))
        return i18n("The build took too long to finish.");
    if (reason.contains(QLatin1String("conflict"), Qt::CaseInsensitive)) return i18n("Conflicting source changes need to be resolved.");
    return i18n("This project could not be updated. See diagnostics for details.");
}

SyncOutcomePresentation syncOperationOutcome(const QJsonObject &operation)
{
    const bool buildOnly = operation.value(QStringLiteral("kind")).toString() == QLatin1String("build");
    const SyncOperationState lifecycle = syncOperationState(operation);
    const QString errorCode = operation.value(QStringLiteral("last_error_code")).toString();
    const QString lastError = operation.value(QStringLiteral("last_error_message")).toString();
    QString outcome = operation.value(QStringLiteral("sync_outcome")).toString();
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    QJsonObject finalBuildSnapshot;
    if (!buildOnly && (lifecycle == SyncOperationState::Succeeded || lifecycle == SyncOperationState::Failed)) {
        for (const QJsonValue &value : events) {
            const QJsonObject event = value.toObject();
            const QJsonObject metadata = event.value(QStringLiteral("metadata")).toObject();
            const QString completedOutcome = metadata.value(QStringLiteral("sync_outcome")).toString();
            if (event.value(QStringLiteral("origin")).toString() != QLatin1String("server")
                && event.value(QStringLiteral("phase")).toString() == QLatin1String("downloading_updates")
                && (completedOutcome == QLatin1String("changes_synced") || completedOutcome == QLatin1String("available_updates_synced"))
                && metadata.value(QStringLiteral("failed_documents")).isArray()) {
                finalBuildSnapshot = metadata;
            }
        }
    }
    // The completion checkpoint reflects the installed Library. An earlier
    // aggregate build can still contain failures superseded by targeted builds.
    // Keep those events as diagnostics, but do not union them into the outcome.
    const bool hasFinalBuildSnapshot = !finalBuildSnapshot.isEmpty();
    QMap<QString, QString> finalFailedBuilds;
    for (const QJsonValue &value : finalBuildSnapshot.value(QStringLiteral("failed_documents")).toArray()) {
        const QJsonObject failure = value.toObject();
        finalFailedBuilds.insert(QFileInfo(failure.value(QStringLiteral("filename")).toString()).fileName(),
                                failure.value(QStringLiteral("build_id")).toString());
    }
    QMap<QString, QStringList> projects;
    QSet<QString> buildProjects;
    const auto addProject = [&projects](QString name, const QString &reason) {
        if (name.isEmpty() || name == QLatin1String("all")) return;
        name = QFileInfo(name).fileName();
        if (name.endsWith(QLatin1String(".pdf"))) name.chop(4);
        // A renamed folder is shown under the short name of its PDFs (rule F).
        if (name.startsWith(QLatin1String("docs-"))) name = Thoughts::SourceFolderNames::pdfShortName(name, Thoughts::SourceFolderNames::knownRenames(Thoughts::thoughtsHome()));
        const QString readable = readableProjectFailure(reason);
        if (!projects[name].contains(readable)) projects[name].append(readable);
    };
    const auto addBuildFailure = [&](const QString &name, const QString &reason, const QString &buildId) {
        const QString filename = QFileInfo(name).fileName();
        if (hasFinalBuildSnapshot
            && (!finalFailedBuilds.contains(filename) || buildId.isEmpty() || finalFailedBuilds.value(filename) != buildId)) return;
        if (!filename.isEmpty() && filename != QLatin1String("all")) buildProjects.insert(filename);
        addProject(name, reason);
    };
    const auto addFailures = [&addBuildFailure](const QJsonObject &container, const QString &buildId = QString()) {
        for (const QJsonValue &value : container.value(QStringLiteral("failed_documents")).toArray()) {
            const QJsonObject failure = value.toObject();
            const QString filename = failure.value(QStringLiteral("pdf_filename")).toString(
                failure.value(QStringLiteral("filename")).toString(failure.value(QStringLiteral("target")).toString()));
            addBuildFailure(filename, failure.value(QStringLiteral("error")).toString(
                failure.value(QStringLiteral("message")).toString(failure.value(QStringLiteral("reason")).toString())),
                failure.value(QStringLiteral("build_id")).toString(buildId));
        }
    };
    addFailures(operation, operation.value(QStringLiteral("build_id")).toString());
    QStringList messages {lastError};
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        const QJsonObject metadata = event.value(QStringLiteral("metadata")).toObject();
        if (event.value(QStringLiteral("origin")).toString() != QLatin1String("server") && metadata.contains(QStringLiteral("sync_outcome")))
            outcome = metadata.value(QStringLiteral("sync_outcome")).toString();
        // Add the final snapshot below only as a fallback, retaining precise
        // diagnostics from the same build instead of duplicating generic text.
        if (metadata != finalBuildSnapshot) addFailures(metadata);
        addFailures(metadata.value(QStringLiteral("result")).toObject());
        const QJsonObject build = metadata.value(QStringLiteral("build")).toObject();
        const QString buildId = build.value(QStringLiteral("id")).toString();
        addFailures(build, buildId);
        addFailures(build.value(QStringLiteral("result")).toObject(), buildId);
        const QString buildStatus = build.value(QStringLiteral("status")).toString();
        if (build.value(QStringLiteral("result")).toObject().value(QStringLiteral("failed_documents")).toArray().isEmpty()
            && (buildStatus == QLatin1String("failed") || buildStatus == QLatin1String("attention") || buildStatus == QLatin1String("cancelled")))
            addBuildFailure(build.value(QStringLiteral("target")).toString(), build.value(QStringLiteral("error")).toString(event.value(QStringLiteral("message")).toString()), buildId);
        for (const QJsonValue &errorValue : metadata.value(QStringLiteral("errors")).toArray()) {
            const QJsonObject error = errorValue.toObject();
            QString project = error.value(QStringLiteral("project")).toString();
            if (project.isEmpty()) project = error.value(QStringLiteral("filename")).toString();
            if (project.isEmpty() && !error.value(QStringLiteral("config")).toString().isEmpty())
                project = QFileInfo(error.value(QStringLiteral("config")).toString()).dir().dirName();
            addProject(project, error.value(QStringLiteral("error")).toString(error.value(QStringLiteral("message")).toString()));
        }
        if (event.value(QStringLiteral("state")).toString() == QLatin1String("failed") || !event.value(QStringLiteral("error_code")).toString().isEmpty())
            messages.append(event.value(QStringLiteral("message")).toString());
    }
    for (const QJsonValue &value : finalBuildSnapshot.value(QStringLiteral("failed_documents")).toArray()) {
        const QJsonObject failure = value.toObject();
        const QString filename = QFileInfo(failure.value(QStringLiteral("filename")).toString()).fileName();
        if (buildProjects.contains(filename)) continue;
        buildProjects.insert(filename);
        addProject(filename, failure.value(QStringLiteral("reason")).toString());
    }
    // Old journals predate the explicit completion marker. Use only known
    // terminal messages; byte counts and a successful build do not prove sync.
    bool available = outcome == QLatin1String("available_updates_synced");
    bool synced = outcome == QLatin1String("changes_synced") || (!buildOnly && lifecycle == SyncOperationState::Succeeded)
        || (!buildOnly && lifecycle == SyncOperationState::Failed && errorCode == QLatin1String("pdf_build_failed"));
    bool pdfFailure = errorCode == QLatin1String("pdf_build_failed");
    bool preparationFailed = false;
    static const QRegularExpression pdfReason(QStringLiteral("(?:^|:\\s*)([A-Za-z0-9_.-]+\\.pdf)\\s*(?:—|:)\\s*(.+)"));
    for (const QString &message : std::as_const(messages)) {
        if (lifecycle == SyncOperationState::Failed && !buildOnly) {
            available |= message.startsWith(QLatin1String("Available updates were synchronized;"));
            synced |= message.startsWith(QLatin1String("Files were synchronized; some proposal previews"));
        }
        preparationFailed |= message.startsWith(QLatin1String("The complete source dependency closure could not be frozen safely."));
        const auto match = pdfReason.match(message);
        if (!hasFinalBuildSnapshot) {
            if (match.hasMatch()) addProject(match.captured(1), match.captured(2));
            pdfFailure |= message.contains(QLatin1String("latex_error:"));
        }
    }
    if (!hasFinalBuildSnapshot && projects.isEmpty() && (pdfFailure || (buildOnly && lifecycle == SyncOperationState::Failed)))
        addProject(operation.value(QStringLiteral("target")).toString(), lastError);

    SyncOutcomePresentation result;
    if (buildOnly) {
        result.title = syncOperationLifecycleTitle(operation);
        result.message = lifecycle == SyncOperationState::Succeeded ? i18n("The PDF build completed. This operation does not confirm that local changes are synced.")
            : i18n("This is a PDF build operation. Sync status is tracked separately.");
    } else if (synced || available) {
        // Projects that need attention for reasons other than a PDF build kept
        // their changes from taking effect, as did local changes that were not
        // sent: updates were received, but the Sync did not send everything.
        const bool needsAttention = !projects.isEmpty() && buildProjects.size() != projects.size();
        const bool buildFailed = !projects.isEmpty() || pdfFailure;
        if (needsAttention || !synced) {
            result.title = SyncStatusLabels::syncFailed();
        } else if (buildFailed) {
            result.title = SyncStatusLabels::pdfBuildFailed();
            result.message = projects.isEmpty() ? SyncStatusLabels::changesSynced() : SyncStatusLabels::changesSyncedWithFailedProjects(projects.size());
        } else {
            result.title = SyncStatusLabels::upToDate();
        }
        if (!synced) result.message = i18n("Available updates were synced with the server. Some project changes still need attention.");
        if (pdfFailure && projects.isEmpty()) result.message += (result.message.isEmpty() ? QString() : QStringLiteral("\n\n")) + i18n("A PDF could not be built. The affected project was not recorded.");
    } else if (lifecycle == SyncOperationState::Running) {
        result.title = SyncStatusLabels::syncing();
        result.message = i18n("Your changes are saved on this device. This status updates as Sync progresses.");
    } else if (lifecycle == SyncOperationState::Unknown) {
        result.title = i18n("Sync status unavailable");
        result.message = i18n("This operation has no recorded Sync outcome.");
    } else {
        result.title = SyncStatusLabels::syncFailed();
        result.message = lifecycle == SyncOperationState::Cancelled ? i18n("This Sync was cancelled before it completed. Your changes remain saved on this device. Run Sync to try again.")
            : lifecycle == SyncOperationState::Interrupted ? i18n("This Sync was interrupted before it completed. Your changes remain saved on this device. Run Sync to try again.")
            : preparationFailed ? i18n("This attempt stopped while preparing source files, before upload and download could complete. Your changes remain saved on this device.")
            : i18n("This attempt did not complete Sync. Your changes remain saved on this device. Run Sync to try again.");
    }
    if (!buildOnly && lifecycle != SyncOperationState::Running && syncOperationReportsPreservedConflict(operation)) {
        result.title = SyncStatusLabels::mergeNeeded();
        result.message = i18n("A source file changed on this computer and on the server in overlapping places. Merge it, then Sync again. Your changes remain saved on this device.");
    }
    if (!projects.isEmpty()) {
        if (buildProjects.size() != projects.size()) result.message += (result.message.isEmpty() ? QString() : QStringLiteral("\n\n")) + i18n("Projects needing attention");
        for (auto it = projects.cbegin(); it != projects.cend(); ++it)
            result.message += (result.message.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("%1\n%2").arg(it.key(), it.value().join(QLatin1Char('\n')));
    }
    return result;
}

qint64 syncOperationDurationMs(const QJsonObject &operation)
{
    const SyncOperationState lifecycle = syncOperationState(operation);
    if (lifecycle == SyncOperationState::Running) {
        return -1;
    }
    if (operation.contains(QStringLiteral("duration_ms"))) {
        return qMax<qint64>(0, operation.value(QStringLiteral("duration_ms")).toVariant().toLongLong());
    }
    if (lifecycle != SyncOperationState::Succeeded && lifecycle != SyncOperationState::Failed
        && lifecycle != SyncOperationState::Interrupted && lifecycle != SyncOperationState::Cancelled) {
        return -1;
    }

    const QString startKey = operation.contains(QStringLiteral("started_at_ms"))
        ? QStringLiteral("started_at_ms")
        : QStringLiteral("created_at_ms");
    const QString completedKey = operation.contains(QStringLiteral("completed_at_ms"))
        ? QStringLiteral("completed_at_ms")
        : QStringLiteral("updated_at_ms");
    if (!operation.contains(startKey) || !operation.contains(completedKey)) {
        return -1;
    }
    const qint64 startedAt = operation.value(startKey).toVariant().toLongLong();
    const qint64 completedAt = operation.value(completedKey).toVariant().toLongLong();
    return completedAt >= startedAt ? completedAt - startedAt : -1;
}

QString syncOperationDurationLabel(const QJsonObject &operation)
{
    const qint64 durationMs = syncOperationDurationMs(operation);
    if (durationMs < 0) {
        return {};
    }
    if (durationMs < 1000) {
        return i18n("%1 ms", durationMs);
    }
    const qint64 totalSeconds = durationMs / 1000;
    if (totalSeconds < 60) {
        return i18n("%1 s", QLocale().toString(durationMs / 1000.0, 'f', 1));
    }
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    if (totalMinutes < 60) {
        return seconds == 0 ? i18n("%1 min", totalMinutes) : i18n("%1 min %2 s", totalMinutes, seconds);
    }
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    return minutes == 0 ? i18n("%1 h", hours) : i18n("%1 h %2 min", hours, minutes);
}

QString syncOperationElapsedLabel(const QJsonObject &operation, qint64 nowMs)
{
    qint64 durationMs = syncOperationDurationMs(operation);
    if (syncOperationState(operation) == SyncOperationState::Running) {
        const QString startKey = operation.contains(QStringLiteral("started_at_ms"))
            ? QStringLiteral("started_at_ms")
            : QStringLiteral("created_at_ms");
        if (!operation.contains(startKey)) {
            return {};
        }
        const qint64 startedAt = operation.value(startKey).toVariant().toLongLong();
        if (startedAt <= 0 || nowMs < startedAt) {
            return {};
        }
        durationMs = nowMs - startedAt;
    }
    if (durationMs < 0) {
        return {};
    }
    const qint64 totalSeconds = durationMs / 1000;
    if (totalSeconds < 60) {
        return i18n("%1 s", totalSeconds);
    }
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    if (totalMinutes < 60) {
        return seconds == 0 ? i18n("%1 min", totalMinutes) : i18n("%1 min %2 s", totalMinutes, seconds);
    }
    const qint64 hours = totalMinutes / 60;
    const qint64 minutes = totalMinutes % 60;
    return minutes == 0 ? i18n("%1 h", hours) : i18n("%1 h %2 min", hours, minutes);
}

static QString syncOperationDirectionalTransferLabel(const QJsonObject &operation,
                                                     const QString &completedField,
                                                     const QString &totalField)
{
    if (!operation.value(QStringLiteral("transfer_metrics_recorded")).toBool()) {
        return i18n("Not recorded");
    }
    const qint64 completed = qMax<qint64>(0, operation.value(completedField).toVariant().toLongLong());
    const qint64 total = qMax(completed, operation.value(totalField).toVariant().toLongLong());
    const auto byteLabel = [](qint64 bytes) {
        return bytes == 0 ? i18n("0 B") : QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
    };
    const QString completedLabel = byteLabel(completed);
    const SyncOperationState lifecycle = syncOperationState(operation);
    if ((lifecycle == SyncOperationState::Running || lifecycle == SyncOperationState::Failed
         || lifecycle == SyncOperationState::Interrupted || lifecycle == SyncOperationState::Cancelled)
        && total > completed) {
        return i18n("%1 of %2", completedLabel, byteLabel(total));
    }
    return completedLabel;
}

QString syncOperationUploadedLabel(const QJsonObject &operation)
{
    return syncOperationDirectionalTransferLabel(operation,
                                                 QStringLiteral("uploaded_completed_bytes"),
                                                 QStringLiteral("uploaded_total_bytes"));
}

QString syncOperationDownloadedLabel(const QJsonObject &operation)
{
    return syncOperationDirectionalTransferLabel(operation,
                                                 QStringLiteral("downloaded_completed_bytes"),
                                                 QStringLiteral("downloaded_total_bytes"));
}

QString syncOperationServerStreamLabel(const QJsonObject &operation)
{
    const QString streamState = operation.value(QStringLiteral("stream_state")).toString();
    if (streamState == QLatin1String("live")) {
        return i18n("Live");
    }
    if (streamState == QLatin1String("reconnecting")) {
        return i18n("Reconnecting");
    }
    return i18n("Saved log");
}

static QString inferredEventPhase(const QJsonObject &operation, const QJsonObject &event)
{
    const QString stored = event.value(QStringLiteral("phase")).toString();
    if (!stored.isEmpty()) {
        return stored;
    }
    const QString detail = event.value(QStringLiteral("message")).toString().toLower();
    if (detail.contains(QStringLiteral("start"))) {
        return QStringLiteral("queued");
    }
    if (detail.contains(QStringLiteral("manifest")) || detail.contains(QStringLiteral("select"))
        || detail.contains(QStringLiteral("frontier")) || detail.contains(QStringLiteral("read"))
        || detail.contains(QStringLiteral("prepar"))) {
        return QStringLiteral("preflight");
    }
    if (detail.contains(QStringLiteral("upload")) || detail.contains(QStringLiteral("publish"))
        || detail.contains(QStringLiteral("staging"))) {
        return QStringLiteral("uploading");
    }
    if (detail.contains(QStringLiteral("build")) || detail.contains(QStringLiteral("compil"))) {
        return QStringLiteral("building");
    }
    if (detail.contains(QStringLiteral("download")) || detail.contains(QStringLiteral("transfer"))) {
        return QStringLiteral("downloading");
    }
    if (detail.contains(QStringLiteral("verif"))) {
        return QStringLiteral("verifying");
    }
    if (detail.contains(QStringLiteral("activat")) || detail.contains(QStringLiteral("install"))) {
        return QStringLiteral("activating");
    }
    if (detail.contains(QStringLiteral("network"))) {
        return QStringLiteral("waiting_network");
    }
    return operation.value(QStringLiteral("phase")).toString(QStringLiteral("queued"));
}

static QString eventPhaseLabel(const QString &phase)
{
    const QString normalized = phase.trimmed().toLower();
    if (normalized == QLatin1String("queued")) {
        return i18n("Starting");
    }
    if (normalized == QLatin1String("checking_server")) {
        return i18n("Checking server");
    }
    if (normalized == QLatin1String("preflight")) {
        return i18n("Checking server");
    }
    if (normalized == QLatin1String("preparing")) {
        return i18n("Preparing local changes");
    }
    if (normalized == QLatin1String("uploading")) {
        return i18n("Uploading local changes");
    }
    if (normalized == QLatin1String("waiting_network")) {
        return i18n("Waiting for network");
    }
    if (normalized == QLatin1String("waiting_user")) {
        return i18n("Waiting for approval");
    }
    if (normalized == QLatin1String("waiting_source_publication")) {
        return i18n("Waiting for source publication");
    }
    if (normalized == QLatin1String("building")) {
        return i18n("Building affected PDFs");
    }
    if (normalized == QLatin1String("downloading")) {
        return i18n("Downloading updates");
    }
    if (normalized == QLatin1String("verifying")) {
        return i18n("Verifying");
    }
    if (normalized == QLatin1String("activating")) {
        return i18n("Activating");
    }
    if (normalized == QLatin1String("failed")) {
        return i18n("Failed");
    }
    if (normalized == QLatin1String("succeeded")) {
        return i18n("Complete");
    }
    QString readable = normalized;
    readable.replace(QLatin1Char('_'), QLatin1Char(' '));
    return readable.isEmpty() ? i18n("Starting") : readable.front().toUpper() + readable.mid(1);
}

static QRegularExpressionMatch unusedTexInputMatch(const QString &message)
{
    static const QRegularExpression pattern(QStringLiteral("(?:^|:\\s*)unused_tex_input:\\s*([^;\\r\\n]+)"));
    return pattern.match(message);
}

static QRegularExpressionMatch emptyDocumentMatch(const QString &message)
{
    static const QRegularExpression pattern(QStringLiteral(
        "empty_document:\\s*Source\\s+([^;\\r\\n]+)\\s+produced no pages;\\s*"
        "it remains listed in\\s+([^;\\r\\n]+);\\s*"
        "add renderable content or remove it from the manifest;\\s*latexmk exit\\s+(\\d+)"));
    return pattern.match(message);
}

static QString embeddedFailureCode(const QString &message)
{
    if (emptyDocumentMatch(message).hasMatch()) {
        return QStringLiteral("empty_document");
    }
    if (unusedTexInputMatch(message).hasMatch()) {
        return QStringLiteral("unused_tex_input");
    }
    return {};
}

static QString userFacingFailureMessage(QString message)
{
    const QRegularExpressionMatch emptyDocument = emptyDocumentMatch(message);
    if (emptyDocument.hasMatch()) {
        QString target = i18n("PDF update");
        static const QRegularExpression targetPattern(QStringLiteral(
            "(?:^|:\\s*)([A-Za-z0-9_.-]+\\.pdf)\\s+—\\s+empty_document:"));
        const QRegularExpressionMatch targetMatch = targetPattern.match(message);
        if (targetMatch.hasMatch()) {
            target = targetMatch.captured(1);
        }
        QString result = i18n(
            "%1 failed. Source: “%2”. Cause: Empty document — LaTeX produced no pages. "
            "Context: The source remains listed in “%3”. "
            "Possible fixes: Add renderable content or remove it from the manifest. "
            "Technical details: latexmk exit %4.",
            target,
            emptyDocument.captured(1).trimmed(),
            emptyDocument.captured(2).trimmed(),
            emptyDocument.captured(3));
        static const QRegularExpression logPattern(QStringLiteral("(?:^|;\\s*)log object ([0-9a-f]{64})(?:$|[;\\s])"));
        const QRegularExpressionMatch log = logPattern.match(message);
        if (log.hasMatch()) {
            result += QLatin1Char(' ') + i18n("Diagnostic log: %1.", log.captured(1));
        }
        return result;
    }

    const QRegularExpressionMatch unusedInput = unusedTexInputMatch(message);
    if (!unusedInput.hasMatch()) {
        return message;
    }

    const QString path = unusedInput.captured(1).trimmed();
    QString result = i18n(
        "PDF update stopped because no TeX source uses the generated file “%1”. "
        "This can happen after rejecting an Agent Edit that added a figure. "
        "The TeX change was saved, but the server did not publish a new PDF.",
        path);
    static const QRegularExpression logPattern(QStringLiteral("(?:^|;\\s*)log object ([0-9a-f]{64})(?:$|[;\\s])"));
    const QRegularExpressionMatch log = logPattern.match(message);
    if (log.hasMatch()) {
        result += QLatin1Char(' ') + i18n("Diagnostic log: %1.", log.captured(1));
    }
    return result;
}

static QString userFacingJournalMessage(const QJsonObject &operation, const QJsonObject &event)
{
    QString message = event.value(QStringLiteral("message")).toString().trimmed();
    if (message.compare(QStringLiteral("queued"), Qt::CaseInsensitive) == 0) {
        return operation.value(QStringLiteral("kind")).toString() == QLatin1String("build")
            ? i18n("Waiting to build")
            : i18n("Waiting to sync");
    }
    const QString failureMessage = userFacingFailureMessage(message);
    if (failureMessage != message) {
        return failureMessage;
    }
    // Older servers send "Reusing Fedora build <id>.", newer ones "Reusing server build <id>.".
    static const QRegularExpression reusedBuildPattern(QStringLiteral("^Reusing (?:Fedora|server) build ([A-Za-z0-9-]+)\\.?$"));
    const QRegularExpressionMatch reusedBuild = reusedBuildPattern.match(message);
    if (reusedBuild.hasMatch()
        && stableSyncOperationState(event.value(QStringLiteral("state")).toString()) == SyncOperationState::Failed) {
        return i18n("The previous failed server build %1 was reused; no new build was run.", reusedBuild.captured(1));
    }
    message.replace(QRegularExpression(QStringLiteral("\\bpreflight\\b"), QRegularExpression::CaseInsensitiveOption),
                    i18n("server check"));
    message.replace(QRegularExpression(QStringLiteral("\\boffline copy\\b"), QRegularExpression::CaseInsensitiveOption),
                    i18n("Saved log"));
    return message;
}

static qint64 measuredEventDurationMs(const QJsonObject &event)
{
    const QJsonValue value = event.value(QStringLiteral("duration_ms"));
    if (!value.isDouble()) {
        return -1;
    }
    const qint64 durationMs = value.toVariant().toLongLong();
    return durationMs > 0 ? durationMs : -1;
}

static QString eventTimingLane(const QJsonObject &event)
{
    const QString origin = event.value(QStringLiteral("origin")).toString(QStringLiteral("desktop"));
    const QString component = event.value(QStringLiteral("component")).toString(origin);
    return origin + QLatin1Char('\n') + component;
}

static QHash<int, qint64> completedEventPhaseDurations(const QJsonObject &operation, const QJsonArray &events)
{
    struct ActivePhase {
        QString phase;
        int firstEvent = -1;
        qint64 startedAt = 0;
    };

    QHash<QString, ActivePhase> activePhases;
    QHash<int, qint64> durations;
    for (qsizetype index = 0; index < events.size(); ++index) {
        const QJsonObject event = events.at(index).toObject();
        const qint64 atMs = event.value(QStringLiteral("at_ms")).toVariant().toLongLong();
        const QString phase = inferredEventPhase(operation, event);
        if (atMs <= 0 || phase.isEmpty()) {
            continue;
        }

        const QString lane = eventTimingLane(event);
        const auto active = activePhases.constFind(lane);
        if (active != activePhases.cend() && active->phase == phase) {
            continue;
        }
        if (active != activePhases.cend() && active->firstEvent >= 0 && atMs > active->startedAt) {
            durations.insert(active->firstEvent, atMs - active->startedAt);
        }
        activePhases.insert(lane, ActivePhase{phase, static_cast<int>(index), atMs});
    }
    return durations;
}

static QJsonValue syncEventValue(const QJsonObject &event, const QString &key)
{
    const QJsonValue direct = event.value(key);
    return !direct.isUndefined() && !direct.isNull() ? direct : event.value(QStringLiteral("metadata")).toObject().value(key);
}

QList<SyncLocalChange> syncOperationLocalChanges(const QJsonObject &operation)
{
    QList<SyncLocalChange> changes;
    QHash<QString, qsizetype> byPath;
    static const QRegularExpression deletionPattern(QStringLiteral("^Scheduling (?:verified )?(?:frozen )?source deletion: (.+)$"));
    static const QRegularExpression updatePattern(QStringLiteral("^Scheduling (?:frozen )?source update: (.+)$"));
    static const QRegularExpression uploadPattern(QStringLiteral("^(?:Uploading|Uploaded) source change (.+?)(?: \\([^)]*\\))?\\.$"));
    const auto record = [&changes, &byPath](QString path, SyncLocalChangeKind kind, qint64 bytes) {
        path = path.trimmed();
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
            return;
        }
        const auto found = byPath.constFind(path);
        if (found == byPath.cend()) {
            byPath.insert(path, changes.size());
            changes.append(SyncLocalChange{kind, path, bytes});
            return;
        }
        SyncLocalChange &change = changes[*found];
        change.kind = kind;
        if (bytes >= 0) {
            change.sizeBytes = bytes;
        }
    };

    for (const QJsonValue &value : operation.value(QStringLiteral("events")).toArray()) {
        const QJsonObject event = value.toObject();
        const QString message = event.value(QStringLiteral("message")).toString().trimmed();
        const QJsonValue bytesValue = syncEventValue(event, QStringLiteral("total_bytes"));
        const qint64 bytes = bytesValue.isDouble() ? bytesValue.toVariant().toLongLong() : -1;
        QString path = syncEventValue(event, QStringLiteral("path")).toString();
        const QString action = syncEventValue(event, QStringLiteral("change_action")).toString();
        if (!action.isEmpty() && !path.isEmpty()) {
            SyncLocalChangeKind kind = SyncLocalChangeKind::Modified;
            if (action == QLatin1String("added")) {
                kind = SyncLocalChangeKind::Added;
            } else if (action == QLatin1String("deleted")) {
                kind = SyncLocalChangeKind::Deleted;
            }
            record(path, kind, kind == SyncLocalChangeKind::Deleted ? -1 : bytes);
            continue;
        }
        QRegularExpressionMatch match = deletionPattern.match(message);
        if (match.hasMatch()) {
            record(match.captured(1), SyncLocalChangeKind::Deleted, -1);
            continue;
        }
        match = updatePattern.match(message);
        if (match.hasMatch()) {
            record(match.captured(1), SyncLocalChangeKind::Modified, bytes);
            continue;
        }
        match = uploadPattern.match(message);
        if (match.hasMatch()) {
            record(path.isEmpty() ? match.captured(1) : path, SyncLocalChangeKind::Modified, bytes);
        }
    }
    return changes;
}

QString syncLocalChangeKindLabel(SyncLocalChangeKind kind)
{
    switch (kind) {
    case SyncLocalChangeKind::Added:
        return i18n("Added");
    case SyncLocalChangeKind::Modified:
        return i18n("Modified");
    case SyncLocalChangeKind::Deleted:
        return i18n("Deleted");
    }
    return i18n("Modified");
}

QString syncLocalChangeSizeLabel(const SyncLocalChange &change)
{
    if (change.kind == SyncLocalChangeKind::Deleted || change.sizeBytes < 0) {
        return {};
    }
    return change.sizeBytes == 0 ? i18n("0 B") : QLocale().formattedDataSize(change.sizeBytes, 1, QLocale::DataSizeIecFormat);
}

QString syncLocalChangeDisplayPath(const QList<SyncLocalChange> &changes, const SyncLocalChange &change)
{
    QString project;
    for (const SyncLocalChange &candidate : changes) {
        const QString candidateProject = candidate.path.section(QLatin1Char('/'), 0, 0);
        if (!candidateProject.startsWith(QLatin1String("docs-")) || (!project.isEmpty() && candidateProject != project)) {
            return change.path;
        }
        project = candidateProject;
    }
    const QString prefix = project + QLatin1Char('/');
    return !project.isEmpty() && change.path.startsWith(prefix) ? change.path.mid(prefix.size()) : change.path;
}

static bool isInternalSourceActivity(const QString &message)
{
    static const QStringList prefixes {
        QStringLiteral("Preparing the managed source inventory"),
        QStringLiteral("The managed source manifest contains"),
        QStringLiteral("Selecting managed sources used by"),
        QStringLiteral("Inspecting the selected managed source identities"),
        QStringLiteral("Checking the current server source frontier"),
        QStringLiteral("Checking the current Fedora source frontier"), // recorded by earlier versions
        QStringLiteral("Scheduling verified frozen source deletion:"),
        QStringLiteral("Scheduling verified source deletion:"),
        QStringLiteral("Scheduling frozen source update:"),
        QStringLiteral("Scheduling source update:"),
        QStringLiteral("Uploading source change "),
        QStringLiteral("Uploaded source change "),
        QStringLiteral("Transfer progress:"),
        QStringLiteral("Uploading metadata for "),
        QStringLiteral("Transfer totals:"),
        QStringLiteral("Source publication command finished successfully"),
        QStringLiteral("The reproducible build inputs for "),
        QStringLiteral("Publishing local changes for "),
        QStringLiteral("Publishing local source changes"),
        QStringLiteral("Inspecting docs-"),
        QStringLiteral("No source changes need publication"),
    };
    static const QRegularExpression selectedPattern(QStringLiteral("^Selected \\d+ (?:authored source path|exact build object)\\(s\\)"));
    static const QRegularExpression readPattern(QStringLiteral("^Read \\d+ managed path\\(s\\)"));
    return selectedPattern.match(message).hasMatch() || readPattern.match(message).hasMatch()
        || std::any_of(prefixes.cbegin(), prefixes.cend(), [&message](const QString &prefix) { return message.startsWith(prefix); });
}

static QString relativeActivityTime(qint64 elapsedMs)
{
    const qint64 seconds = qMax<qint64>(0, elapsedMs) / 1000;
    if (seconds < 60) {
        return i18n("+%1 sec", seconds);
    }
    const qint64 minutes = seconds / 60;
    return minutes < 60 ? i18n("+%1 min %2 sec", minutes, seconds % 60) : i18n("+%1 h %2 min", minutes / 60, minutes % 60);
}

QStringList syncOperationActivityLines(const QJsonObject &operation)
{
    QStringList lines;
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    if (events.isEmpty()) {
        return lines;
    }
    qint64 startedAtMs = operation.value(QStringLiteral("started_at_ms")).toVariant().toLongLong();
    if (startedAtMs <= 0) {
        startedAtMs = operation.value(QStringLiteral("created_at_ms")).toVariant().toLongLong();
    }
    if (startedAtMs <= 0) {
        startedAtMs = events.first().toObject().value(QStringLiteral("at_ms")).toVariant().toLongLong();
    }

    const QHash<int, qint64> phaseDurations = completedEventPhaseDurations(operation, events);
    QHash<QString, qint64> recentMessages;
    QString previousPhase;
    qint64 sourceStartedAt = -1;
    qint64 sourceUploadedBytes = -1;
    static const QRegularExpression publicationPattern(
        QStringLiteral("^Publication complete at source frontier \\d+: (\\d+) updated, (\\d+) deleted, (\\d+) unchanged\\.$"));
    for (qsizetype index = 0; index < events.size(); ++index) {
        const QJsonObject event = events.at(index).toObject();
        const qint64 atMs = event.value(QStringLiteral("at_ms")).toVariant().toLongLong();
        QString message = userFacingJournalMessage(operation, event);
        if (message.isEmpty()) {
            continue;
        }
        const bool failed = event.value(QStringLiteral("state")).toString() == QLatin1String("failed") || !event.value(QStringLiteral("error_code")).toString().isEmpty();
        const bool sourceDetail = isInternalSourceActivity(message);
        if (sourceDetail && sourceStartedAt < 0 && (message.startsWith(QLatin1String("Scheduling ")) || message.startsWith(QLatin1String("Uploading source change ")))) {
            sourceStartedAt = atMs;
        }
        const QJsonValue uploadedValue = syncEventValue(event, QStringLiteral("uploaded_bytes"));
        if (uploadedValue.isDouble()) {
            sourceUploadedBytes = qMax(sourceUploadedBytes, uploadedValue.toVariant().toLongLong());
        }

        qint64 durationMs = measuredEventDurationMs(event);
        const QRegularExpressionMatch publication = publicationPattern.match(message);
        if (publication.hasMatch()) {
            const int changed = publication.captured(1).toInt() + publication.captured(2).toInt();
            message = i18np("Uploaded %1 local change", "Uploaded %1 local changes", changed);
            if (sourceUploadedBytes > 0) {
                message += QStringLiteral(" · %1").arg(QLocale().formattedDataSize(sourceUploadedBytes, 1, QLocale::DataSizeIecFormat));
            }
            if (sourceStartedAt > 0 && atMs > sourceStartedAt) {
                durationMs = atMs - sourceStartedAt;
            }
            sourceStartedAt = -1;
            sourceUploadedBytes = -1;
        } else if (sourceDetail && !failed) {
            continue;
        }
        // Journals recorded by earlier versions say "Fedora accepted …".
        if (message == QLatin1String("The server accepted the conditional PDF update.") || message == QLatin1String("Fedora accepted the conditional PDF update.")) {
            continue;
        }

        const QString phase = inferredEventPhase(operation, event);
        if (phase != previousPhase) {
            // Returning to an earlier phase starts a distinct interval, even
            // when its label repeats within the duplicate-message window.
            recentMessages.clear();
            previousPhase = phase;
        }
        const QString duplicateKey = phase + QLatin1Char('\n') + message;
        const qint64 previousAt = recentMessages.value(duplicateKey, std::numeric_limits<qint64>::min());
        if (previousAt != std::numeric_limits<qint64>::min() && atMs >= previousAt && atMs - previousAt <= 3000) {
            continue;
        }
        recentMessages.insert(duplicateKey, atMs);
        if (durationMs <= 0) {
            durationMs = phaseDurations.value(static_cast<int>(index), -1);
        }
        if (durationMs > 0) {
            message += QStringLiteral(" · %1").arg(syncOperationDurationLabel(QJsonObject{{QStringLiteral("duration_ms"), durationMs}}));
        }
        if (event.value(QStringLiteral("total")).toVariant().toLongLong() > 0) {
            message += QStringLiteral(" · %1 of %2")
                           .arg(event.value(QStringLiteral("completed")).toVariant().toLongLong())
                           .arg(event.value(QStringLiteral("total")).toVariant().toLongLong());
        }
        QString errorCode = event.value(QStringLiteral("error_code")).toString();
        const QString embeddedCode = embeddedFailureCode(event.value(QStringLiteral("message")).toString());
        if (!embeddedCode.isEmpty()) {
            errorCode = embeddedCode;
        }
        if (!errorCode.isEmpty()) {
            message += QStringLiteral(" [%1]").arg(errorCode);
        }

        QString component = event.value(QStringLiteral("component")).toString();
        const bool localDesktop = event.value(QStringLiteral("origin")).toString() != QLatin1String("server") && component.compare(QStringLiteral("desktop"), Qt::CaseInsensitive) == 0;
        if (localDesktop) {
            component.clear();
        } else {
            component.replace(QLatin1Char('_'), QLatin1Char(' '));
            if (component.compare(QStringLiteral("api"), Qt::CaseInsensitive) == 0) {
                component = QStringLiteral("API");
            } else if (!component.isEmpty()) {
                component = component.front().toUpper() + component.mid(1);
            }
        }
        const QString phaseLabel = eventPhaseLabel(phase);
        QStringList parts {relativeActivityTime(atMs - startedAtMs)};
        if (!component.isEmpty()) {
            parts.append(component);
        }
        if (message.compare(phaseLabel, Qt::CaseInsensitive) != 0 && !message.startsWith(phaseLabel + QLatin1Char(' '), Qt::CaseInsensitive)) {
            parts.append(phaseLabel);
        }
        parts.append(message);
        lines.append(parts.join(QStringLiteral(" · ")));
    }
    return lines;
}

QString syncOperationReadableLog(const QJsonObject &operation)
{
    const QList<SyncLocalChange> changes = syncOperationLocalChanges(operation);
    QStringList lines {i18np("LOCAL CHANGES · %1 FILE", "LOCAL CHANGES · %1 FILES", changes.size())};
    if (changes.isEmpty()) {
        lines.append(i18n("No local source files changed."));
    } else {
        for (const SyncLocalChange &change : changes) {
            QString code = QStringLiteral("M");
            if (change.kind == SyncLocalChangeKind::Added) {
                code = QStringLiteral("A");
            } else if (change.kind == SyncLocalChangeKind::Deleted) {
                code = QStringLiteral("D");
            }
            QString line = QStringLiteral("%1  %2").arg(code, syncLocalChangeDisplayPath(changes, change));
            const QString size = syncLocalChangeSizeLabel(change);
            if (!size.isEmpty()) {
                line += QStringLiteral("  %1").arg(size);
            }
            lines.append(line);
        }
    }
    lines.append(QString());
    lines.append(i18n("SYNC ACTIVITY"));
    const QStringList activity = syncOperationActivityLines(operation);
    lines.append(activity.isEmpty() ? QStringList {i18n("No activity was recorded.")} : activity);
    return lines.join(QLatin1Char('\n'));
}

QStringList syncOperationLogLines(const QJsonObject &operation)
{
    QStringList lines;
    QString previousKey;
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    const QHash<int, qint64> phaseDurations = completedEventPhaseDurations(operation, events);
    for (qsizetype index = 0; index < events.size(); ++index) {
        const QJsonValue value = events.at(index);
        const QJsonObject event = value.toObject();
        QString message = userFacingJournalMessage(operation, event);
        const QString phase = inferredEventPhase(operation, event);
        const QString duplicateKey = phase + QLatin1Char('\n') + message;
        const qint64 explicitDurationMs = measuredEventDurationMs(event);
        const qint64 derivedDurationMs = phaseDurations.value(static_cast<int>(index), -1);
        const qint64 durationMs = explicitDurationMs > 0 ? explicitDurationMs : derivedDurationMs;
        if (message.isEmpty() || (duplicateKey == previousKey && durationMs <= 0)) {
            continue;
        }
        previousKey = duplicateKey;
        if (durationMs > 0) {
            const QJsonObject durationOperation{{QStringLiteral("duration_ms"), durationMs}};
            message += QStringLiteral(" · %1").arg(syncOperationDurationLabel(durationOperation));
        }
        if (event.value(QStringLiteral("total")).toVariant().toLongLong() > 0) {
            message += QStringLiteral(" · %1 of %2")
                           .arg(event.value(QStringLiteral("completed")).toVariant().toLongLong())
                           .arg(event.value(QStringLiteral("total")).toVariant().toLongLong());
        }
        QString errorCode = event.value(QStringLiteral("error_code")).toString();
        const QString embeddedCode = embeddedFailureCode(event.value(QStringLiteral("message")).toString());
        if (!embeddedCode.isEmpty()) {
            errorCode = embeddedCode;
        }
        if (!errorCode.isEmpty()) {
            message += QStringLiteral(" [%1]").arg(errorCode);
        }
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(event.value(QStringLiteral("at_ms")).toVariant().toLongLong()).toLocalTime();
        QString component = event.value(QStringLiteral("component")).toString();
        component.replace(QLatin1Char('_'), QLatin1Char(' '));
        if (!component.isEmpty()) {
            component = component.front().toUpper() + component.mid(1);
        }
        const bool isLocalDesktopEvent = event.value(QStringLiteral("origin")).toString() != QLatin1String("server")
            && component.compare(QStringLiteral("Desktop"), Qt::CaseInsensitive) == 0;
        if (isLocalDesktopEvent) {
            component.clear();
        }
        const QString category = component.isEmpty()
            ? eventPhaseLabel(phase)
            : QStringLiteral("%1 · %2").arg(component, eventPhaseLabel(phase));
        lines.append(QStringLiteral("%1 · %2 · %3").arg(at.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss t")), category, message));
    }
    return lines;
}

QString syncOperationFailureJournalMessage(const QString &summary, const QString &details)
{
    const QString conciseSummary = summary.simplified();
    const QString conciseDetails = details.simplified();
    if (conciseSummary.isEmpty()) {
        return conciseDetails;
    }
    if (conciseDetails.isEmpty() || conciseDetails == conciseSummary) {
        return conciseSummary;
    }
    if (conciseDetails.startsWith(conciseSummary)) {
        return conciseDetails;
    }
    return conciseSummary + QLatin1Char(' ') + conciseDetails;
}

QString syncOperationFailureMessage(const QJsonObject &operation)
{
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    for (qsizetype index = events.size(); index > 0; --index) {
        const QString message = events.at(index - 1).toObject().value(QStringLiteral("message")).toString().trimmed();
        if (!embeddedFailureCode(message).isEmpty()) {
            return userFacingFailureMessage(message);
        }
    }
    const QString stored = operation.value(QStringLiteral("last_error_message")).toString().trimmed();
    if (!embeddedFailureCode(stored).isEmpty()) {
        return userFacingFailureMessage(stored);
    }
    for (qsizetype index = events.size(); index > 0; --index) {
        const QJsonObject event = events.at(index - 1).toObject();
        const SyncOperationState eventState = stableSyncOperationState(event.value(QStringLiteral("state")).toString());
        if (event.value(QStringLiteral("origin")).toString() == QLatin1String("server")
            && ((eventState == SyncOperationState::Failed || eventState == SyncOperationState::Interrupted || eventState == SyncOperationState::Cancelled)
                || !event.value(QStringLiteral("error_code")).toString().isEmpty())) {
            return userFacingFailureMessage(event.value(QStringLiteral("message")).toString().trimmed());
        }
    }
    if (!stored.isEmpty()) {
        return userFacingFailureMessage(stored);
    }
    for (qsizetype index = events.size(); index > 0; --index) {
        const QJsonObject event = events.at(index - 1).toObject();
        const SyncOperationState eventState = stableSyncOperationState(event.value(QStringLiteral("state")).toString());
        if (eventState == SyncOperationState::Failed || eventState == SyncOperationState::Interrupted || eventState == SyncOperationState::Cancelled
            || !event.value(QStringLiteral("error_code")).toString().isEmpty()) {
            return userFacingFailureMessage(event.value(QStringLiteral("message")).toString().trimmed());
        }
    }
    return {};
}

QString syncOperationErrorCode(const QJsonObject &operation)
{
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    for (qsizetype index = events.size(); index > 0; --index) {
        const QString code = embeddedFailureCode(events.at(index - 1).toObject().value(QStringLiteral("message")).toString());
        if (!code.isEmpty()) {
            return code;
        }
    }
    const QString storedCode = embeddedFailureCode(operation.value(QStringLiteral("last_error_message")).toString());
    if (!storedCode.isEmpty()) {
        return storedCode;
    }
    for (qsizetype index = events.size(); index > 0; --index) {
        const QJsonObject event = events.at(index - 1).toObject();
        const QString errorCode = event.value(QStringLiteral("error_code")).toString().trimmed();
        if (event.value(QStringLiteral("origin")).toString() == QLatin1String("server") && !errorCode.isEmpty()) {
            return errorCode;
        }
    }
    const QString stored = operation.value(QStringLiteral("last_error_code")).toString().trimmed();
    if (!stored.isEmpty()) {
        return stored;
    }
    for (qsizetype index = events.size(); index > 0; --index) {
        const QString errorCode = events.at(index - 1).toObject().value(QStringLiteral("error_code")).toString().trimmed();
        if (!errorCode.isEmpty()) {
            return errorCode;
        }
    }
    return {};
}

QString fedoraBuildProgressEvent(const QJsonObject &renderStatus)
{
    const QJsonObject progress = renderStatus.value(QStringLiteral("build")).toObject().value(QStringLiteral("progress")).toObject();
    QString detail = progress.value(QStringLiteral("detail")).toString().trimmed();
    detail.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (detail.isEmpty()) {
        return {};
    }

    const int completed = progress.value(QStringLiteral("completed")).toInt(-1);
    const int total = progress.value(QStringLiteral("total")).toInt(-1);
    if (completed >= 0 && total > 1) {
        return i18n("%1 · %2/%3", detail, qMin(completed, total), total);
    }
    return detail;
}

bool syncOperationReportsSourceConflict(const QJsonObject &operation)
{
    if (operation.value(QStringLiteral("needs_source_conflict_resolution")).toBool()
        || operation.value(QStringLiteral("last_error_code")).toString() == QLatin1String("invalid_source_publication")
        || operation.value(QStringLiteral("error_code")).toString() == QLatin1String("invalid_source_publication")) {
        return true;
    }
    for (const QJsonValue &value : operation.value(QStringLiteral("events")).toArray()) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("error_code")).toString() == QLatin1String("invalid_source_publication")
            || event.value(QStringLiteral("code")).toString() == QLatin1String("invalid_source_publication")) {
            return true;
        }
    }
    return false;
}

bool syncOperationReportsPreservedConflict(const QJsonObject &operation)
{
    const auto preserved = [](const QString &code, const QString &message) {
        return code == QLatin1String("invalid_source_publication") && message.contains(QLatin1String("source conflict was preserved"));
    };
    if (preserved(operation.value(QStringLiteral("last_error_code")).toString(), operation.value(QStringLiteral("last_error_message")).toString())) {
        return true;
    }
    for (const QJsonValue &value : operation.value(QStringLiteral("events")).toArray()) {
        const QJsonObject event = value.toObject();
        if (preserved(event.value(QStringLiteral("error_code")).toString(event.value(QStringLiteral("code")).toString()), event.value(QStringLiteral("message")).toString())) {
            return true;
        }
    }
    return false;
}

QStringList recordedSourceConflictPaths(const QString &statePath)
{
    // Parsed again only when the record changes; status refreshes call this often.
    static QString cachedPath;
    static QString cachedIdentity;
    static QStringList cachedPaths;
    const QString path = statePath.isEmpty()
        ? QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-publication.json"))
        : statePath;
    const QFileInfo info(path);
    const QString identity = info.exists() ? QString::number(info.size()) + QLatin1Char(':') + QString::number(info.lastModified().toMSecsSinceEpoch()) : QString();
    if (path == cachedPath && identity == cachedIdentity) {
        return cachedPaths;
    }
    cachedPath = path;
    cachedIdentity = identity;
    cachedPaths.clear();
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        cachedPaths = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("conflicts")).toObject().keys();
    }
    return cachedPaths;
}

int sourceFilesNeedingMerge(const QJsonObject &latestOperation, const QString &statePath)
{
    const qsizetype recorded = recordedSourceConflictPaths(statePath).size();
    return recorded > 0 ? int(recorded) : syncOperationReportsPreservedConflict(latestOperation) ? 1 : 0;
}

}
