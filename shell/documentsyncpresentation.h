/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_DOCUMENTSYNCPRESENTATION_H
#define THOUGHTS_DOCUMENTSYNCPRESENTATION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace Thoughts
{

enum class DocumentSyncAction {
    Sync,
    BuildAndSync,
    OpenBuild,
    None,
};

enum class DocumentSyncStatus {
    Current,
    SyncNeeded,
    BuildNeeded,
    Syncing,
    WaitingForSourcePublication,
    Building,
    Error,
};

struct DocumentSyncState {
    bool sourceLinked = true;
    // False for a source-linked PDF that Fedora neither builds nor lists: its
    // document is unchecked in its Project, or a complete Fedora PDF status
    // newer than the publication leaves it out. It never needs a build.
    bool buildTarget = true;
    bool libraryPdfStale = false;
    bool localFreshnessAvailable = false;
    bool localSourcesCurrent = false;
    bool deferLocalFreshness = false;
    int manualSyncState = 0;
    QString manualSyncMessage;
    QString manualSyncDetails;
    QString latestBuildMessage;
    QString localFreshnessMessage;
    QVariantMap pendingDocumentSync;
    QVariantMap buildActivity;
    QVariantMap renderStatus;
};

struct DocumentSyncPresentation {
    DocumentSyncStatus state = DocumentSyncStatus::SyncNeeded;
    QString status;
    QString reason;
    QString actionLabel;
    QString actionId;
    QString iconName;
    DocumentSyncAction action = DocumentSyncAction::Sync;
    bool enabled = true;
    bool lastBuildFailed = false;

    bool requiresBuild() const;
    bool requiresSync() const;
    QJsonObject toJson() const;
};

struct AggregateSyncPresentation {
    DocumentSyncStatus state = DocumentSyncStatus::Current;
    QString status;
    QString reason;
    QString actionLabel;
    bool enabled = true;
    bool requiresBuild = false;
    bool requiresSync = false;
    int pendingDocuments = 0;

    QJsonObject toJson() const;
};

struct SyncActivityPresentation {
    QString status;
    QString detail;
};

struct SyncOutcomePresentation {
    QString title;
    QString message;
};

SyncOutcomePresentation syncOperationOutcome(const QJsonObject &operation);
// A document whose new PDF the server is still building after Sync ended.
QString pdfUpdatingOnServerLabel();
// The Sync summary's line for the PDFs a finished Sync left building on the server.
QString pdfsStillUpdatingOnServerMessage(int pdfs);

enum class SyncLocalChangeKind {
    Added,
    Modified,
    Deleted,
};

struct SyncLocalChange {
    SyncLocalChangeKind kind = SyncLocalChangeKind::Modified;
    QString path;
    qint64 sizeBytes = -1;
};

DocumentSyncPresentation documentSyncPresentation(const DocumentSyncState &state);
AggregateSyncPresentation aggregateSyncPresentation(const QList<DocumentSyncPresentation> &documents);
QJsonObject latestDocumentSyncOperation(const QJsonArray &operations, const QString &target);
QJsonArray reconcileSyncOperationsAfterRestart(const QJsonArray &operations, qint64 nowMs);
QString documentSyncActivityLabel(const QJsonObject &operation, qint64 nowMs);
QString documentSyncActivityDetailLabel(const QJsonObject &operation, qint64 nowMs);
QString syncElapsedSecondsLabel(qint64 elapsedSeconds);
QString pendingSyncActivityLabel(const QJsonObject &pendingChanges, int pendingDocuments = -1);
QString compactSyncActivityAge(qint64 elapsedSeconds);
QString syncOperationHistoryLabel(const QJsonObject &operation, qint64 nowMs);
SyncActivityPresentation syncActivityPresentation(const QJsonObject &operation,
                                                  const QJsonObject &pendingChanges,
                                                  qint64 nowMs,
                                                  int pendingDocuments = -1);
QString syncOperationDisplayStatus(const QJsonObject &operation);
QString syncOperationLifecycleTitle(const QJsonObject &operation);
qint64 syncOperationDurationMs(const QJsonObject &operation);
QString syncOperationDurationLabel(const QJsonObject &operation);
QString syncOperationElapsedLabel(const QJsonObject &operation, qint64 nowMs);
QString syncOperationUploadedLabel(const QJsonObject &operation);
QString syncOperationDownloadedLabel(const QJsonObject &operation);
QString syncOperationServerStreamLabel(const QJsonObject &operation);
QList<SyncLocalChange> syncOperationLocalChanges(const QJsonObject &operation);
QString syncLocalChangeKindLabel(SyncLocalChangeKind kind);
QString syncLocalChangeSizeLabel(const SyncLocalChange &change);
QString syncLocalChangeDisplayPath(const QList<SyncLocalChange> &changes, const SyncLocalChange &change);
QStringList syncOperationActivityLines(const QJsonObject &operation);
QString syncOperationReadableLog(const QJsonObject &operation);
QStringList syncOperationLogLines(const QJsonObject &operation);
QString syncOperationFailureJournalMessage(const QString &summary, const QString &details);
QString syncOperationFailureMessage(const QJsonObject &operation);
QString syncOperationErrorCode(const QJsonObject &operation);
QString fedoraBuildProgressEvent(const QJsonObject &renderStatus);
// Whether this operation's source publication was refused. Covers preserved
// conflicts and every other refusal (for example a stale base).
bool syncOperationReportsSourceConflict(const QJsonObject &operation);
// Whether Fedora preserved a source conflict that needs a merge. Only this,
// or a recorded conflict, is Merge needed; other refusals are Sync failed.
bool syncOperationReportsPreservedConflict(const QJsonObject &operation);
// Docs-relative paths whose conflict the source publisher recorded and still
// awaits a decision on. An empty statePath reads this Mac's publisher state.
QStringList recordedSourceConflictPaths(const QString &statePath = {});
// Source files that need a merge now: every recorded conflict, or one when
// only the latest Sync reported it.
int sourceFilesNeedingMerge(const QJsonObject &latestOperation, const QString &statePath = {});

}

#endif
