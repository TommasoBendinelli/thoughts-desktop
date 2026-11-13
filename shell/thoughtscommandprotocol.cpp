/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "thoughtscommandprotocol.h"
#include "syncoperationstate.h"
#include "../part/thoughtsplatform.h"
#include <QCryptographicHash>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>

namespace
{
QString operationMessage(const QJsonObject &operation)
{
    const QString error = operation.value(QStringLiteral("last_error_message")).toString();
    if (!error.isEmpty()) {
        return error;
    }
    const QJsonArray events = operation.value(QStringLiteral("events")).toArray();
    for (qsizetype index = events.size(); index > 0; --index) {
        const QString message = events.at(index - 1).toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) {
            return message;
        }
    }
    return operation.value(QStringLiteral("status")).toString();
}
}

QString Thoughts::syncCommandSocketPath()
{
#ifdef Q_OS_WIN
    // A named pipe: one per user data folder, so isolated test profiles get
    // their own. THOUGHTS_COMMAND_SOCKET selects another one explicitly.
    const QString configured = qEnvironmentVariable("THOUGHTS_COMMAND_SOCKET");
    if (!configured.isEmpty()) {
        return configured;
    }
    const QByteArray root = Thoughts::Platform::thoughtsDataRoot().toLower().toUtf8();
    return QStringLiteral("thoughts-command-%1").arg(QString::fromLatin1(QCryptographicHash::hash(root, QCryptographicHash::Sha256).toHex().left(16)));
#else
    return QDir(Thoughts::Platform::thoughtsDataRoot()).filePath(QStringLiteral("thoughts-command.sock"));
#endif
}

bool Thoughts::validSyncCommandTarget(const QString &target)
{
    return target.isEmpty()
        || (!target.contains(QLatin1Char('/')) && !target.contains(QLatin1Char('\\'))
            && QFileInfo(target).fileName() == target
            && target.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive));
}

void Thoughts::mergeSyncEventIdentity(QJsonObject &operation,
                                      QJsonObject &event,
                                      const QJsonObject &incomingEvent)
{
    const QString buildId = incomingEvent.value(QStringLiteral("build_id")).toString().trimmed().left(128);
    if (!buildId.isEmpty()) {
        operation.insert(QStringLiteral("build_id"), buildId);
        event.insert(QStringLiteral("build_id"), buildId);
        const QString buildTarget = incomingEvent.value(QStringLiteral("target")).toString().trimmed();
        if (buildTarget == QLatin1String("all")
            || (!buildTarget.isEmpty() && validSyncCommandTarget(buildTarget))) {
            operation.insert(QStringLiteral("build_target"), buildTarget);
            event.insert(QStringLiteral("build_target"), buildTarget);
        }
    }
}

QString Thoughts::syncOperationBuildOutputTarget(const QJsonObject &operation)
{
    const QString buildTarget = operation.value(QStringLiteral("build_target")).toString().trimmed();
    if (buildTarget == QLatin1String("all")) {
        return {};
    }
    if (!buildTarget.isEmpty()) {
        return validSyncCommandTarget(buildTarget) ? buildTarget : QString();
    }
    const QString legacyTarget = operation.value(QStringLiteral("target")).toString().trimmed();
    return !legacyTarget.isEmpty() && validSyncCommandTarget(legacyTarget) ? legacyTarget : QString();
}

QString Thoughts::syncCommandState(const QJsonObject &operation)
{
    const SyncOperationState lifecycle = syncOperationState(operation);
    if (lifecycle == SyncOperationState::Succeeded) {
        return QStringLiteral("succeeded");
    }
    if (lifecycle == SyncOperationState::Failed || lifecycle == SyncOperationState::Interrupted || lifecycle == SyncOperationState::Cancelled) {
        return QStringLiteral("failed");
    }

    const QString phase = operation.value(QStringLiteral("phase")).toString();
    if (phase == QLatin1String("building")) {
        return QStringLiteral("building");
    }
    if (phase == QLatin1String("waiting_network") || phase == QLatin1String("waiting_user") || phase == QLatin1String("waiting_source_publication")) {
        return QStringLiteral("waiting");
    }
    if (phase == QLatin1String("queued")) {
        const QString status = operation.value(QStringLiteral("status")).toString();
        return stableSyncOperationState(status) == SyncOperationState::Running
                || status.contains(QStringLiteral("sync"), Qt::CaseInsensitive)
            ? QStringLiteral("syncing")
            : QStringLiteral("queued");
    }
    if (phase.isEmpty()) {
        const QString status = operation.value(QStringLiteral("status")).toString();
        if (status.startsWith(QStringLiteral("Building"), Qt::CaseInsensitive)) {
            return QStringLiteral("building");
        }
        if (status.startsWith(QStringLiteral("Waiting"), Qt::CaseInsensitive)) {
            return QStringLiteral("waiting");
        }
    }
    return QStringLiteral("syncing");
}

QJsonObject Thoughts::syncCommandOperationResult(const QJsonObject &operation, bool reused)
{
    const QString target = operation.value(QStringLiteral("target")).toString();
    const QString state = syncCommandState(operation);
    QJsonObject result {{QStringLiteral("schema_version"), 1},
                        {QStringLiteral("accepted"), true},
                        {QStringLiteral("reused"), reused},
                        {QStringLiteral("operation_id"), operation.value(QStringLiteral("id")).toString()},
                        {QStringLiteral("scope"), target.isEmpty() ? QStringLiteral("all") : QStringLiteral("target")},
                        {QStringLiteral("state"), state},
                        {QStringLiteral("terminal"), state == QLatin1String("succeeded") || state == QLatin1String("failed")},
                        {QStringLiteral("message"), operationMessage(operation)}};
    if (!target.isEmpty()) {
        result.insert(QStringLiteral("target"), target);
    }
    const QString buildId = operation.value(QStringLiteral("build_id")).toString();
    if (!buildId.isEmpty()) {
        result.insert(QStringLiteral("build_id"), buildId);
    }
    if (state == QLatin1String("succeeded") || state == QLatin1String("failed")) {
        // Not "error_code": a failed Sync's status is still a valid answer.
        const QString failureCode = operation.value(QStringLiteral("last_error_code")).toString();
        if (state == QLatin1String("failed") && !failureCode.isEmpty()) {
            result.insert(QStringLiteral("failure_code"), failureCode);
        }
        // The repository reports pdf_build_failed only once no local change
        // is blocked or unsent; any other failure leaves sources unsynced.
        result.insert(QStringLiteral("sources_synced"),
                      state == QLatin1String("succeeded") || failureCode == QLatin1String("pdf_build_failed"));
    }
    return result;
}

bool Thoughts::syncCommandSourcesSynced(const QJsonObject &result)
{
    const QString state = result.value(QStringLiteral("state")).toString();
    return state == QLatin1String("succeeded")
        || (state == QLatin1String("failed") && result.value(QStringLiteral("terminal")).toBool()
            && result.value(QStringLiteral("sources_synced")).toBool());
}

QJsonObject Thoughts::syncCommandBuildWarning(const QJsonObject &result, const QString &sync)
{
    if (result.value(QStringLiteral("state")).toString() != QLatin1String("failed") || !syncCommandSourcesSynced(result)) {
        return {};
    }
    QJsonObject warning {{QStringLiteral("code"), result.value(QStringLiteral("failure_code")).toString(QStringLiteral("pdf_build_failed"))},
                         {QStringLiteral("sync"), sync},
                         {QStringLiteral("message"), result.value(QStringLiteral("message")).toString()}};
    const QString operationId = result.value(QStringLiteral("operation_id")).toString();
    if (!operationId.isEmpty()) {
        warning.insert(QStringLiteral("operation_id"), operationId);
    }
    return warning;
}

QJsonObject Thoughts::syncCommandStatusRequest(const QString &operationId)
{
    return {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("command"), QStringLiteral("status")},
            {QStringLiteral("operation_id"), operationId}};
}

QJsonObject Thoughts::syncCommandError(const QString &code,
                                       const QString &message,
                                       const QString &target,
                                       const QString &blockingOperationId)
{
    QJsonObject result {{QStringLiteral("schema_version"), 1},
                        {QStringLiteral("accepted"), false},
                        {QStringLiteral("reused"), false},
                        {QStringLiteral("scope"), target.isEmpty() ? QStringLiteral("all") : QStringLiteral("target")},
                        {QStringLiteral("state"), code == QLatin1String("busy") ? QStringLiteral("busy") : QStringLiteral("failed")},
                        {QStringLiteral("terminal"), code != QLatin1String("busy")},
                        {QStringLiteral("message"), message},
                        {QStringLiteral("error_code"), code}};
    if (!target.isEmpty()) {
        result.insert(QStringLiteral("target"), target);
    }
    if (!blockingOperationId.isEmpty()) {
        result.insert(QStringLiteral("blocking_operation_id"), blockingOperationId);
    }
    return result;
}

int Thoughts::syncCommandExitCode(const QJsonObject &result)
{
    if (result.value(QStringLiteral("client_timeout")).toBool()) {
        return 5;
    }
    const QString errorCode = result.value(QStringLiteral("error_code")).toString();
    if (errorCode == QLatin1String("busy")) {
        return 6;
    }
    if (errorCode == QLatin1String("invalid_request") || errorCode == QLatin1String("unknown_operation")) {
        return 2;
    }
    if (errorCode == QLatin1String("app_unavailable") || errorCode == QLatin1String("not_configured") || errorCode == QLatin1String("transport_unavailable")) {
        return 3;
    }
    if (result.value(QStringLiteral("state")).toString() == QLatin1String("failed")) {
        return 4;
    }
    return 0;
}
