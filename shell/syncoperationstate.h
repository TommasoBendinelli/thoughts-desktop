/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_SYNCOPERATIONSTATE_H
#define THOUGHTS_SYNCOPERATIONSTATE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace Thoughts
{

enum class SyncOperationState {
    Unknown,
    Running,
    Succeeded,
    Failed,
    Interrupted,
    Cancelled,
};

inline SyncOperationState stableSyncOperationState(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("running")) {
        return SyncOperationState::Running;
    }
    if (normalized == QLatin1String("succeeded")) {
        return SyncOperationState::Succeeded;
    }
    if (normalized == QLatin1String("failed")) {
        return SyncOperationState::Failed;
    }
    if (normalized == QLatin1String("interrupted")) {
        return SyncOperationState::Interrupted;
    }
    if (normalized == QLatin1String("cancelled") || normalized == QLatin1String("canceled")) {
        return SyncOperationState::Cancelled;
    }
    return SyncOperationState::Unknown;
}

inline SyncOperationState legacySyncOperationState(const QString &value)
{
    const QString normalized = value.trimmed().toCaseFolded();
    if (normalized == QStringLiteral("synced") || normalized == QStringLiteral("built")
        || normalized == QStringLiteral("sync complete") || normalized == QStringLiteral("build complete")) {
        return SyncOperationState::Succeeded;
    }
    if (normalized == QStringLiteral("error") || normalized == QStringLiteral("sync failed") || normalized == QStringLiteral("build failed")) {
        return SyncOperationState::Failed;
    }
    if (normalized == QStringLiteral("sync interrupted") || normalized == QStringLiteral("build interrupted")) {
        return SyncOperationState::Interrupted;
    }
    if (normalized == QStringLiteral("syncing…") || normalized == QStringLiteral("syncing...")
        || normalized == QStringLiteral("queued") || normalized == QStringLiteral("preflight")
        || normalized == QStringLiteral("waiting to publish sources…") || normalized == QStringLiteral("waiting to publish sources...")
        || normalized == QStringLiteral("waiting to sync") || normalized == QStringLiteral("waiting to build")
        || normalized == QStringLiteral("building…") || normalized == QStringLiteral("building...")) {
        return SyncOperationState::Running;
    }
    return SyncOperationState::Unknown;
}

inline SyncOperationState syncOperationState(const QJsonObject &operation)
{
    const QString status = operation.value(QStringLiteral("status")).toString();
    const SyncOperationState stableStatus = stableSyncOperationState(status);
    if (stableStatus != SyncOperationState::Unknown) {
        return stableStatus;
    }

    const SyncOperationState stableState = stableSyncOperationState(operation.value(QStringLiteral("state")).toString());
    if (stableState != SyncOperationState::Unknown) {
        return stableState;
    }

    const SyncOperationState legacyStatus = legacySyncOperationState(status);
    if (legacyStatus != SyncOperationState::Unknown) {
        return legacyStatus;
    }

    const QString phase = operation.value(QStringLiteral("phase")).toString().trimmed().toLower();
    if (phase == QLatin1String("succeeded")) {
        return SyncOperationState::Succeeded;
    }
    if (phase == QLatin1String("failed")) {
        return SyncOperationState::Failed;
    }
    if (phase == QLatin1String("interrupted")) {
        return SyncOperationState::Interrupted;
    }
    if (phase == QLatin1String("cancelled") || phase == QLatin1String("canceled")) {
        return SyncOperationState::Cancelled;
    }
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    SyncOperationState latestEventState = SyncOperationState::Unknown;
    for (qsizetype index = events.size(); index > 0; --index) {
        const SyncOperationState eventState = stableSyncOperationState(events.at(index - 1).toObject().value(QStringLiteral("state")).toString());
        if (eventState != SyncOperationState::Unknown) {
            latestEventState = eventState;
            break;
        }
    }

    if (latestEventState == SyncOperationState::Succeeded || latestEventState == SyncOperationState::Failed
        || latestEventState == SyncOperationState::Interrupted || latestEventState == SyncOperationState::Cancelled) {
        return latestEventState;
    }

    if (operation.value(QStringLiteral("completed_at_ms")).toVariant().toLongLong() > 0) {
        return SyncOperationState::Interrupted;
    }
    if (latestEventState == SyncOperationState::Running
        || phase == QLatin1String("queued") || phase == QLatin1String("checking_server") || phase == QLatin1String("preflight")
        || phase == QLatin1String("preparing") || phase == QLatin1String("uploading")
        || phase == QLatin1String("waiting_network") || phase == QLatin1String("waiting_user")
        || phase == QLatin1String("waiting_source_publication") || phase == QLatin1String("building")
        || phase == QLatin1String("downloading") || phase == QLatin1String("verifying") || phase == QLatin1String("activating")) {
        return SyncOperationState::Running;
    }
    return SyncOperationState::Unknown;
}

inline QString syncOperationStateId(SyncOperationState state)
{
    switch (state) {
    case SyncOperationState::Running:
        return QStringLiteral("running");
    case SyncOperationState::Succeeded:
        return QStringLiteral("succeeded");
    case SyncOperationState::Failed:
        return QStringLiteral("failed");
    case SyncOperationState::Interrupted:
        return QStringLiteral("interrupted");
    case SyncOperationState::Cancelled:
        return QStringLiteral("cancelled");
    case SyncOperationState::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

inline bool syncOperationIsRunning(const QJsonObject &operation)
{
    return syncOperationState(operation) == SyncOperationState::Running;
}

inline bool syncOperationCanRetry(const QJsonObject &operation)
{
    const SyncOperationState state = syncOperationState(operation);
    return state == SyncOperationState::Failed || state == SyncOperationState::Interrupted || state == SyncOperationState::Cancelled;
}

}

#endif
