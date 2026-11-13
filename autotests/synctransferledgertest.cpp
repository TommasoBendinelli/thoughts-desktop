// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/synctransferledger.h"

#include <QTest>

#include <limits>

using Thoughts::SyncTransferLedger;

class SyncTransferLedgerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void chunkedProgressRollsBackAndCommitsIdempotently();
    void cumulativeUnplannedProgressAdvancesBeforeFinalTotals();
    void planReplacementDropsOnlyUnverifiedWork();
    void rejectsInvalidAndOverflowingState();
};

void SyncTransferLedgerTest::cumulativeUnplannedProgressAdvancesBeforeFinalTotals()
{
    SyncTransferLedger ledger;
    const auto upload = SyncTransferLedger::Direction::Upload;
    const auto download = SyncTransferLedger::Direction::Download;
    const QString item = QStringLiteral("source-publication:paper.pdf:7");
    QVERIFY(ledger.updateUnplannedItem(upload, item, 17));
    QVERIFY(ledger.updateUnplannedItem(download, item, 0));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_completed_bytes")).toInteger(), 17);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_total_bytes")).toInteger(), 17);
    QVERIFY(ledger.updateUnplannedItem(upload, item, 29));
    QVERIFY(ledger.updateUnplannedItem(download, item, 4));
    QVERIFY(ledger.commitUnplannedItem(upload, item, 29));
    QVERIFY(ledger.commitUnplannedItem(download, item, 4));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_completed_bytes")).toInteger(), 29);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_completed_bytes")).toInteger(), 4);
    QVERIFY(!ledger.updateUnplannedItem(upload, item, 30));
}

void SyncTransferLedgerTest::chunkedProgressRollsBackAndCommitsIdempotently()
{
    SyncTransferLedger ledger;
    const auto upload = SyncTransferLedger::Direction::Upload;
    QVERIFY(ledger.planItem(upload, QStringLiteral("operations:1"), 100));
    QVERIFY(ledger.updateItem(upload, QStringLiteral("operations:1"), 40));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_completed_bytes")).toInteger(), 40);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_total_bytes")).toInteger(), 100);
    QVERIFY(ledger.rollbackItem(upload, QStringLiteral("operations:1")));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_completed_bytes")).toInteger(), 0);
    QVERIFY(ledger.updateItem(upload, QStringLiteral("operations:1"), 100));
    QVERIFY(ledger.commitItem(upload, QStringLiteral("operations:1")));
    QVERIFY(ledger.commitItem(upload, QStringLiteral("operations:1"), 100));
    QVERIFY(!ledger.commitItem(upload, QStringLiteral("operations:1"), 99));
    QVERIFY(!ledger.updateItem(upload, QStringLiteral("operations:1"), 50));
    QCOMPARE(ledger.snapshot(true).value(QStringLiteral("flush")).toBool(), true);
}

void SyncTransferLedgerTest::planReplacementDropsOnlyUnverifiedWork()
{
    SyncTransferLedger ledger;
    const auto download = SyncTransferLedger::Direction::Download;
    // Work tentatively recorded before a plan identity is known is unfinished
    // old work too.  A replacement must drop it rather than failing or
    // contaminating the approved plan.
    QVERIFY(ledger.planItem(download, QStringLiteral("tentative"), 9));
    QVERIFY(ledger.updateItem(download, QStringLiteral("tentative"), 4));
    QVERIFY(ledger.replaceDownloadPlan(QStringLiteral("plan-one"),
                                       {{QStringLiteral("metadata:plan-one"), 20},
                                        {QStringLiteral("object:a"), 80},
                                        {QStringLiteral("object:b"), 40}}));
    QVERIFY(!ledger.updateItem(download, QStringLiteral("tentative"), 5));
    QVERIFY(ledger.updateItem(download, QStringLiteral("object:a"), 80));
    QVERIFY(ledger.commitItem(download, QStringLiteral("object:a")));
    QVERIFY(ledger.updateItem(download, QStringLiteral("object:b"), 10));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_completed_bytes")).toInteger(), 90);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_total_bytes")).toInteger(), 140);

    QVERIFY(ledger.replaceDownloadPlan(QStringLiteral("plan-two"),
                                       {{QStringLiteral("metadata:plan-two"), 12},
                                        {QStringLiteral("object:c"), 30}}));
    // The verified object remains honest transferred traffic. The unfinished
    // old metadata/object disappear, and the replacement plan starts at zero.
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_completed_bytes")).toInteger(), 80);
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_total_bytes")).toInteger(), 122);
    QVERIFY(!ledger.updateItem(download, QStringLiteral("object:b"), 20));
    QVERIFY(ledger.updateItem(download, QStringLiteral("metadata:plan-two"), 12));
    QVERIFY(ledger.commitItem(download, QStringLiteral("metadata:plan-two")));
    QVERIFY(ledger.updateItem(download, QStringLiteral("object:c"), 30));
    QVERIFY(ledger.commitItem(download, QStringLiteral("object:c")));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("downloaded_completed_bytes")).toInteger(), 122);
}

void SyncTransferLedgerTest::rejectsInvalidAndOverflowingState()
{
    SyncTransferLedger ledger;
    const auto upload = SyncTransferLedger::Direction::Upload;
    QVERIFY(!ledger.planItem(upload, QString(), 1));
    QVERIFY(!ledger.planItem(upload, QStringLiteral("negative"), -1));
    QVERIFY(ledger.planItem(upload, QStringLiteral("one"), 10));
    QVERIFY(!ledger.planItem(upload, QStringLiteral("one"), 11));
    QVERIFY(!ledger.updateItem(upload, QStringLiteral("one"), 11));
    QVERIFY(!ledger.commitItem(upload, QStringLiteral("one"), 9));
    QVERIFY(ledger.commitUnplannedItem(upload, QStringLiteral("measured"), 5));
    QCOMPARE(ledger.snapshot().value(QStringLiteral("uploaded_total_bytes")).toInteger(), 15);
    ledger.reset();
    QVERIFY(ledger.planItem(upload, QStringLiteral("maximum"), std::numeric_limits<qint64>::max()));
    QVERIFY(ledger.planItem(upload, QStringLiteral("overflow"), 1));
    QVERIFY(ledger.snapshot().isEmpty());
}

QTEST_GUILESS_MAIN(SyncTransferLedgerTest)
#include "synctransferledgertest.moc"
