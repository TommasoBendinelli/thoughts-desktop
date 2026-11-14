/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_COMMAND_PROTOCOL_H
#define THOUGHTS_COMMAND_PROTOCOL_H

#include <QJsonObject>
#include <QString>

namespace Thoughts
{
QString syncCommandSocketPath();
bool validSyncCommandTarget(const QString &target);
void mergeSyncEventIdentity(QJsonObject &operation,
                            QJsonObject &event,
                            const QJsonObject &incomingEvent);
QString syncOperationBuildOutputTarget(const QJsonObject &operation);
QString syncCommandState(const QJsonObject &operation);
QJsonObject syncCommandOperationResult(const QJsonObject &operation, bool reused = false);
QJsonObject syncCommandStatusRequest(const QString &operationId);
// Whether a terminal Sync result left the sources synchronized with Fedora:
// the Sync succeeded, or every change was synced and only a PDF build failed
// (failure_code pdf_build_failed). Conflicts and unsent changes are not synced.
bool syncCommandSourcesSynced(const QJsonObject &result);
// The warning for a Sync whose sources synced but whose PDF build failed;
// empty otherwise. @p sync names the Sync ("before" or "after"). Such a Sync
// succeeds with "warning_code" and "warning" in its result; an app from before
// Sync stopped waiting for the server's builds reports it as a failed Sync
// with failure_code pdf_build_failed and sources_synced. Both give the same
// warning {code, sync, message, operation_id}.
QJsonObject syncCommandBuildWarning(const QJsonObject &result, const QString &sync);
QJsonObject syncCommandError(const QString &code,
                             const QString &message,
                             const QString &target = QString(),
                             const QString &blockingOperationId = QString());
int syncCommandExitCode(const QJsonObject &result);
}

#endif
