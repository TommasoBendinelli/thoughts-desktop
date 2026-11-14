// SPDX-License-Identifier: GPL-2.0-or-later
// What a document and the Sync summary say when Sync ended while the server
// was still building a PDF, and when a build failed after the changes arrived.
#include "../shell/documentsyncpresentation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

class DocumentSyncPresentationTest : public QObject
{
    Q_OBJECT

    static QJsonObject completion(const QJsonObject &metadata)
    {
        return {{QStringLiteral("phase"), QStringLiteral("downloading_updates")}, {QStringLiteral("state"), QStringLiteral("running")}, {QStringLiteral("metadata"), metadata}};
    }

    static QJsonObject succeededSync(const QJsonArray &events)
    {
        return {{QStringLiteral("kind"), QStringLiteral("sync")}, {QStringLiteral("status"), QStringLiteral("succeeded")}, {QStringLiteral("phase"), QStringLiteral("succeeded")}, {QStringLiteral("events"), events}};
    }

private Q_SLOTS:
    void documentWhosePdfTheServerStillBuilds()
    {
        Thoughts::DocumentSyncState state;
        state.sourceLinked = true;
        state.manualSyncState = 2; // The Sync that left the build running has succeeded.
        state.buildActivity = {{QStringLiteral("active"), false}};
        state.renderStatus = {{QStringLiteral("status"), QStringLiteral("building")},
                              {QStringLiteral("reason"), QStringLiteral("The server is updating this PDF. The PDF on this computer stays until the new one arrives.")}};
        const Thoughts::DocumentSyncPresentation updating = Thoughts::documentSyncPresentation(state);
        QVERIFY(updating.state == Thoughts::DocumentSyncStatus::Building);
        QCOMPARE(updating.status, QStringLiteral("PDF updating on the server"));
        QVERIFY(updating.reason.startsWith(QStringLiteral("The server is updating this PDF.")));
        QCOMPARE(updating.actionLabel, QStringLiteral("PDF updating on the server — click for output"));
        // The build's output opens the same way as for a build this computer started.
        QCOMPARE(updating.actionId, QStringLiteral("build-active"));
        QVERIFY(updating.action == Thoughts::DocumentSyncAction::OpenBuild);
        QVERIFY(updating.enabled);
        QVERIFY(!updating.lastBuildFailed);
        QVERIFY(!updating.requiresSync());
        QVERIFY(!updating.requiresBuild());

        // The server's own status after the Sync carries no reason of ours.
        state.renderStatus = {{QStringLiteral("status"), QStringLiteral("building")}};
        const Thoughts::DocumentSyncPresentation plain = Thoughts::documentSyncPresentation(state);
        QCOMPARE(plain.status, QStringLiteral("PDF updating on the server"));
        QVERIFY(plain.reason.contains(QStringLiteral("stays until the new one arrives")));

        // A build this computer started and follows keeps its wording.
        state.buildActivity = {{QStringLiteral("active"), true}};
        const Thoughts::DocumentSyncPresentation followed = Thoughts::documentSyncPresentation(state);
        QVERIFY(followed.state == Thoughts::DocumentSyncStatus::Building);
        QCOMPARE(followed.status, QStringLiteral("Syncing…"));
        QCOMPARE(followed.actionLabel, QStringLiteral("Sync in progress — click for output"));

        // After a restart the saved status is only a memory until the server confirms it.
        state.buildActivity = {{QStringLiteral("active"), false}};
        state.renderStatus = {{QStringLiteral("status"), QStringLiteral("building")}, {QStringLiteral("restored_from_cache"), true}, {QStringLiteral("local_state"), QStringLiteral("current")}};
        state.localFreshnessAvailable = true;
        state.localSourcesCurrent = true;
        QVERIFY(Thoughts::documentSyncPresentation(state).state != Thoughts::DocumentSyncStatus::Building);
    }

    void documentWhoseBuildFailedAfterSync()
    {
        Thoughts::DocumentSyncState state;
        state.sourceLinked = true;
        state.manualSyncState = 2;
        state.localFreshnessAvailable = true;
        state.localSourcesCurrent = true;
        state.buildActivity = {{QStringLiteral("active"), false}};
        state.renderStatus = {{QStringLiteral("status"), QStringLiteral("attention")}, {QStringLiteral("reason"), QStringLiteral("latex_error: Undefined control sequence")}};
        const Thoughts::DocumentSyncPresentation failed = Thoughts::documentSyncPresentation(state);
        QVERIFY(failed.state == Thoughts::DocumentSyncStatus::Error);
        QVERIFY(failed.lastBuildFailed);
        QCOMPARE(failed.reason, QStringLiteral("latex_error: Undefined control sequence"));
        QVERIFY(failed.requiresBuild());
    }

    void summaryOfASyncThatLeftPdfsBuilding()
    {
        const QJsonObject one = succeededSync({completion({{QStringLiteral("sync_outcome"), QStringLiteral("changes_synced")},
                                                           {QStringLiteral("failed_documents"), QJsonArray()},
                                                           {QStringLiteral("pending_build_documents"), QJsonArray {QStringLiteral("paper.pdf")}}})});
        Thoughts::SyncOutcomePresentation summary = Thoughts::syncOperationOutcome(one);
        QCOMPARE(summary.title, QStringLiteral("Up to date"));
        QCOMPARE(summary.message, QStringLiteral("1 PDF is still updating on the server. The PDF on this computer stays until the new one arrives."));
        QCOMPARE(Thoughts::syncOperationDisplayStatus(one), QStringLiteral("Synced"));
        QCOMPARE(Thoughts::syncOperationLifecycleTitle(one), QStringLiteral("Sync complete"));

        const QJsonObject three = succeededSync({completion({{QStringLiteral("sync_outcome"), QStringLiteral("changes_synced")},
                                                             {QStringLiteral("failed_documents"), QJsonArray()},
                                                             {QStringLiteral("pending_build_documents"), QJsonArray {QStringLiteral("a.pdf"), QStringLiteral("b.pdf"), QStringLiteral("c.pdf")}}})});
        summary = Thoughts::syncOperationOutcome(three);
        QCOMPARE(summary.message, QStringLiteral("3 PDFs are still updating on the server. The PDFs on this computer stay until the new ones arrive."));

        // Without pending builds the summary is unchanged.
        const QJsonObject none = succeededSync({completion({{QStringLiteral("sync_outcome"), QStringLiteral("changes_synced")}, {QStringLiteral("failed_documents"), QJsonArray()}})});
        summary = Thoughts::syncOperationOutcome(none);
        QCOMPARE(summary.title, QStringLiteral("Up to date"));
        QVERIFY(summary.message.isEmpty());
    }

    // A failed PDF build no longer fails the Sync: the operation succeeded and
    // its summary carries the failure as a warning.
    void summaryOfASucceededSyncWithAFailedBuild()
    {
        const QJsonObject build {{QStringLiteral("id"), QStringLiteral("build-four")}, {QStringLiteral("target"), QStringLiteral("paper.pdf")}, {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("error"), QStringLiteral("latexmk exit 12")}};
        const QJsonObject deferred {{QStringLiteral("phase"), QStringLiteral("downloading")},
                                    {QStringLiteral("state"), QStringLiteral("running")},
                                    {QStringLiteral("build_id"), QStringLiteral("build-four")},
                                    {QStringLiteral("message"), QStringLiteral("latexmk exit 12")},
                                    {QStringLiteral("error_code"), QStringLiteral("pdf_build_pending")},
                                    {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("build"), build}}}};
        const QJsonObject operation = succeededSync({deferred,
                                                     completion({{QStringLiteral("sync_outcome"), QStringLiteral("changes_synced")},
                                                                 {QStringLiteral("failed_documents"),
                                                                  QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("reason"), QStringLiteral("latexmk exit 12")}, {QStringLiteral("build_id"), QStringLiteral("build-four")}}}}})});
        const Thoughts::SyncOutcomePresentation summary = Thoughts::syncOperationOutcome(operation);
        QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
        QVERIFY2(summary.message.startsWith(QStringLiteral("Your changes are synced · 1 project failed to build.")), qPrintable(summary.message));
        QVERIFY(summary.message.contains(QStringLiteral("paper\n")));
        // The Sync itself is not a failed one.
        QCOMPARE(Thoughts::syncOperationDisplayStatus(operation), QStringLiteral("Synced"));
        QCOMPARE(Thoughts::syncOperationLifecycleTitle(operation), QStringLiteral("Sync complete"));
        QVERIFY(Thoughts::syncOperationErrorCode(operation) != QLatin1String("pdf_build_failed"));
    }
};

QTEST_GUILESS_MAIN(DocumentSyncPresentationTest)
#include "documentsyncpresentationtest.moc"
