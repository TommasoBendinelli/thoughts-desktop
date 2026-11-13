// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/syncstatuslabels.h"

#include <QTest>

// Thoughts on the phone shows the same words (ui/sync/SyncStatusLabels.kt).
// Change both together, never one alone.
class SyncStatusLabelsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void stateLabelsMatchThePhone()
    {
        using namespace Thoughts::SyncStatusLabels;
        QCOMPARE(syncing(), QStringLiteral("Syncing…"));
        QCOMPARE(syncing().right(1), QString(QChar(0x2026)));
        QCOMPARE(upToDate(), QStringLiteral("Up to date"));
        QCOMPARE(changesPending(), QStringLiteral("Changes pending"));
        QCOMPARE(pdfBuildFailed(), QStringLiteral("PDF build failed"));
        QCOMPARE(syncFailed(), QStringLiteral("Sync failed"));
        QCOMPARE(showLogs(), QStringLiteral("Show logs"));
        QCOMPARE(mergeNeeded(), QStringLiteral("Merge needed"));
        QCOMPARE(showMerge(), QStringLiteral("Show merge"));
        QCOMPARE(sourceFilesNeedMerge(1), QStringLiteral("1 source file needs a merge"));
        QCOMPARE(sourceFilesNeedMerge(2), QStringLiteral("2 source files need a merge"));
    }

    void buildFailureDetailsMatchThePhone()
    {
        using namespace Thoughts::SyncStatusLabels;
        QCOMPARE(changesSynced(), QStringLiteral("Your changes are synced."));
        QCOMPARE(changesSyncedWithFailedPdfs(1), QStringLiteral("Your changes are synced · 1 PDF failed to build."));
        QCOMPARE(changesSyncedWithFailedPdfs(3), QStringLiteral("Your changes are synced · 3 PDFs failed to build."));
        QCOMPARE(changesSyncedWithFailedProjects(1), QStringLiteral("Your changes are synced · 1 project failed to build."));
        QCOMPARE(changesSyncedWithFailedProjects(2), QStringLiteral("Your changes are synced · 2 projects failed to build."));
    }
};

QTEST_GUILESS_MAIN(SyncStatusLabelsTest)
#include "syncstatuslabelstest.moc"
