/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/thoughtscommandprotocol.h"
#include "../shell/syncoperationstate.h"

#include <QJsonArray>
#include <QTest>

class ThoughtsCommandProtocolTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void validatesTargets();
    void preservesBuildIdentityFromLocalEvent();
    void normalizesOperationLifecycle();
    void mapsOperationStates();
    void serializesOperationResult();
    void separatesBuildFailureFromSourceSync();
    void buildsStatusRequest();
    void mapsExitCodes();
};

void ThoughtsCommandProtocolTest::validatesTargets()
{
    QVERIFY(Thoughts::validSyncCommandTarget(QString()));
    QVERIFY(Thoughts::validSyncCommandTarget(QStringLiteral("paper.pdf")));
    QVERIFY(Thoughts::validSyncCommandTarget(QStringLiteral("PAPER.PDF")));
    QVERIFY(!Thoughts::validSyncCommandTarget(QStringLiteral("paper.tex")));
    QVERIFY(!Thoughts::validSyncCommandTarget(QStringLiteral("../paper.pdf")));
    QVERIFY(!Thoughts::validSyncCommandTarget(QStringLiteral("folder/paper.pdf")));
}

void ThoughtsCommandProtocolTest::normalizesOperationLifecycle()
{
    using Thoughts::SyncOperationState;

    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("status"), QStringLiteral("succeeded")},
                                           {QStringLiteral("phase"), QStringLiteral("building")}}),
             SyncOperationState::Succeeded);
    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("status"), QStringLiteral("failed")},
                                           {QStringLiteral("phase"), QStringLiteral("uploading")}}),
             SyncOperationState::Failed);
    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("status"), QStringLiteral("running")},
                                           {QStringLiteral("phase"), QStringLiteral("failed")}}),
             SyncOperationState::Running);
    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("status"), QStringLiteral("Error")},
                                           {QStringLiteral("phase"), QStringLiteral("uploading")}}),
             SyncOperationState::Failed);
    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("status"), QStringLiteral("Synced")},
                                           {QStringLiteral("phase"), QStringLiteral("building")}}),
             SyncOperationState::Succeeded);
    QCOMPARE(Thoughts::syncOperationState({{QStringLiteral("phase"), QStringLiteral("interrupted")}}),
             SyncOperationState::Interrupted);
    const QJsonObject completedLegacy {{QStringLiteral("status"), QStringLiteral("Fehler")},
                                       {QStringLiteral("phase"), QStringLiteral("uploading")},
                                       {QStringLiteral("completed_at_ms"), 42}};
    QCOMPARE(Thoughts::syncOperationState(completedLegacy), SyncOperationState::Interrupted);
    const QJsonObject failedEvent {{QStringLiteral("phase"), QStringLiteral("uploading")},
                                   {QStringLiteral("events"), QJsonArray {QJsonObject {{QStringLiteral("state"), QStringLiteral("failed")}}}}};
    QCOMPARE(Thoughts::syncOperationState(failedEvent), SyncOperationState::Failed);
    QCOMPARE(Thoughts::syncOperationStateId(SyncOperationState::Cancelled), QStringLiteral("cancelled"));
    QVERIFY(Thoughts::syncOperationIsRunning({{QStringLiteral("phase"), QStringLiteral("downloading")}}));
    QVERIFY(Thoughts::syncOperationCanRetry({{QStringLiteral("status"), QStringLiteral("Error")}}));
}

void ThoughtsCommandProtocolTest::preservesBuildIdentityFromLocalEvent()
{
    QJsonObject operation {{QStringLiteral("id"), QStringLiteral("operation")},
                           {QStringLiteral("phase"), QStringLiteral("building")}};
    QJsonObject event {{QStringLiteral("phase"), QStringLiteral("building")}};
    Thoughts::mergeSyncEventIdentity(
        operation,
        event,
        QJsonObject {{QStringLiteral("build_id"), QStringLiteral(" conditional-build ")},
                     {QStringLiteral("target"), QStringLiteral("paper.pdf")}});

    QCOMPARE(operation.value(QStringLiteral("build_id")).toString(), QStringLiteral("conditional-build"));
    QCOMPARE(event.value(QStringLiteral("build_id")).toString(), QStringLiteral("conditional-build"));
    QCOMPARE(operation.value(QStringLiteral("build_target")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(event.value(QStringLiteral("build_target")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(Thoughts::syncOperationBuildOutputTarget(operation), QStringLiteral("paper.pdf"));
    QCOMPARE(Thoughts::syncCommandOperationResult(operation).value(QStringLiteral("build_id")).toString(),
             QStringLiteral("conditional-build"));

    operation.insert(QStringLiteral("build_target"), QStringLiteral("all"));
    QVERIFY(Thoughts::syncOperationBuildOutputTarget(operation).isEmpty());
    operation.remove(QStringLiteral("build_target"));
    operation.insert(QStringLiteral("target"), QStringLiteral("legacy.pdf"));
    QCOMPARE(Thoughts::syncOperationBuildOutputTarget(operation), QStringLiteral("legacy.pdf"));
}

void ThoughtsCommandProtocolTest::mapsOperationStates()
{
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("queued")}}), QStringLiteral("queued"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("queued")}, {QStringLiteral("status"), QStringLiteral("Syncing…")}}), QStringLiteral("syncing"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("queued")}, {QStringLiteral("status"), QStringLiteral("running")}}), QStringLiteral("syncing"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("waiting_network")}}), QStringLiteral("waiting"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("waiting_source_publication")}, {QStringLiteral("status"), QStringLiteral("running")}}),
             QStringLiteral("waiting"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("building")}}), QStringLiteral("building"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("downloading")}}), QStringLiteral("syncing"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("succeeded")}}), QStringLiteral("succeeded"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("phase"), QStringLiteral("interrupted")}}), QStringLiteral("failed"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("succeeded")}, {QStringLiteral("phase"), QStringLiteral("building")}}),
             QStringLiteral("succeeded"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("phase"), QStringLiteral("uploading")}}),
             QStringLiteral("failed"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("Error")}, {QStringLiteral("phase"), QStringLiteral("uploading")}}),
             QStringLiteral("failed"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("running")}, {QStringLiteral("phase"), QStringLiteral("failed")}}),
             QStringLiteral("syncing"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("Building…")}}), QStringLiteral("building"));
    QCOMPARE(Thoughts::syncCommandState({{QStringLiteral("status"), QStringLiteral("Waiting to publish sources…")}}), QStringLiteral("waiting"));
}

void ThoughtsCommandProtocolTest::serializesOperationResult()
{
    const QJsonObject result = Thoughts::syncCommandOperationResult(
        {{QStringLiteral("id"), QStringLiteral("operation")},
         {QStringLiteral("target"), QStringLiteral("paper.pdf")},
         {QStringLiteral("phase"), QStringLiteral("building")},
         {QStringLiteral("build_id"), QStringLiteral("build")},
         {QStringLiteral("events"), QJsonArray {QJsonObject {{QStringLiteral("message"), QStringLiteral("Building paper.pdf")}}}}},
        true);

    QCOMPARE(result.value(QStringLiteral("schema_version")).toInt(), 1);
    QVERIFY(result.value(QStringLiteral("accepted")).toBool());
    QVERIFY(result.value(QStringLiteral("reused")).toBool());
    QCOMPARE(result.value(QStringLiteral("scope")).toString(), QStringLiteral("target"));
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("building"));
    QVERIFY(!result.value(QStringLiteral("terminal")).toBool());
    QCOMPARE(result.value(QStringLiteral("build_id")).toString(), QStringLiteral("build"));
}

void ThoughtsCommandProtocolTest::separatesBuildFailureFromSourceSync()
{
    // 2026-10-14: a failed PDF build failed the proposal tools' Sync, so the
    // proposals that would fix the build were refused. Only sources gate them.
    const auto operation = [](const QString &status, const QString &code) {
        QJsonObject value {{QStringLiteral("id"), QStringLiteral("operation")},
                           {QStringLiteral("status"), status},
                           {QStringLiteral("phase"), status}};
        if (!code.isEmpty()) {
            value.insert(QStringLiteral("last_error_code"), code);
            value.insert(QStringLiteral("last_error_message"), QStringLiteral("PDF build failed"));
        }
        return value;
    };

    const QJsonObject succeeded = Thoughts::syncCommandOperationResult(operation(QStringLiteral("succeeded"), QString()));
    QVERIFY(succeeded.value(QStringLiteral("sources_synced")).toBool());
    QVERIFY(Thoughts::syncCommandSourcesSynced(succeeded));
    QVERIFY(Thoughts::syncCommandBuildWarning(succeeded, QStringLiteral("before")).isEmpty());

    const QJsonObject buildFailed = Thoughts::syncCommandOperationResult(operation(QStringLiteral("failed"), QStringLiteral("pdf_build_failed")));
    QCOMPARE(buildFailed.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    QCOMPARE(buildFailed.value(QStringLiteral("failure_code")).toString(), QStringLiteral("pdf_build_failed"));
    QVERIFY(!buildFailed.contains(QStringLiteral("error_code"))); // a status answer, not a command error
    QVERIFY(buildFailed.value(QStringLiteral("sources_synced")).toBool());
    QVERIFY(Thoughts::syncCommandSourcesSynced(buildFailed));
    const QJsonObject warning = Thoughts::syncCommandBuildWarning(buildFailed, QStringLiteral("before"));
    QCOMPARE(warning.value(QStringLiteral("code")).toString(), QStringLiteral("pdf_build_failed"));
    QCOMPARE(warning.value(QStringLiteral("sync")).toString(), QStringLiteral("before"));
    QCOMPARE(warning.value(QStringLiteral("operation_id")).toString(), QStringLiteral("operation"));
    QVERIFY(!warning.value(QStringLiteral("message")).toString().isEmpty());

    for (const QString &code : {QStringLiteral("sync_failed"), QString()}) {
        const QJsonObject failed = Thoughts::syncCommandOperationResult(operation(QStringLiteral("failed"), code));
        QVERIFY(!failed.value(QStringLiteral("sources_synced")).toBool());
        QVERIFY(!Thoughts::syncCommandSourcesSynced(failed));
        QVERIFY(Thoughts::syncCommandBuildWarning(failed, QStringLiteral("before")).isEmpty());
    }
    const QJsonObject running = Thoughts::syncCommandOperationResult(operation(QStringLiteral("running"), QString()));
    QVERIFY(!running.contains(QStringLiteral("sources_synced")));
    QVERIFY(!Thoughts::syncCommandSourcesSynced(running));
    QVERIFY(!Thoughts::syncCommandSourcesSynced(Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("down"))));
    QVERIFY(!Thoughts::syncCommandSourcesSynced({}));
}

void ThoughtsCommandProtocolTest::buildsStatusRequest()
{
    const QJsonObject request = Thoughts::syncCommandStatusRequest(QStringLiteral("operation"));

    QCOMPARE(request.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(request.value(QStringLiteral("command")).toString(), QStringLiteral("status"));
    QCOMPARE(request.value(QStringLiteral("operation_id")).toString(), QStringLiteral("operation"));
}

void ThoughtsCommandProtocolTest::mapsExitCodes()
{
    QCOMPARE(Thoughts::syncCommandExitCode({{QStringLiteral("state"), QStringLiteral("queued")}}), 0);
    QCOMPARE(Thoughts::syncCommandExitCode(Thoughts::syncCommandError(QStringLiteral("invalid_request"), QStringLiteral("bad"))), 2);
    QCOMPARE(Thoughts::syncCommandExitCode(Thoughts::syncCommandError(QStringLiteral("app_unavailable"), QStringLiteral("bad"))), 3);
    QCOMPARE(Thoughts::syncCommandExitCode({{QStringLiteral("state"), QStringLiteral("failed")}}), 4);
    QCOMPARE(Thoughts::syncCommandExitCode({{QStringLiteral("client_timeout"), true}}), 5);
    QCOMPARE(Thoughts::syncCommandExitCode(Thoughts::syncCommandError(QStringLiteral("busy"), QStringLiteral("busy"))), 6);
}

QTEST_GUILESS_MAIN(ThoughtsCommandProtocolTest)

#include "thoughtscommandprotocoltest.moc"
