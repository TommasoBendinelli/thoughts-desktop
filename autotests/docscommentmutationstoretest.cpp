// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/docscommentmutationstore.h"
#include "part/docscommentsynchronizer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Okular;

namespace
{

QJsonObject commentRecord(const QString &contents)
{
    return {
        {QStringLiteral("id"), QStringLiteral("comment-one")},
        {QStringLiteral("page"), 1},
        {QStringLiteral("page_width"), 612.0},
        {QStringLiteral("page_height"), 792.0},
        {QStringLiteral("page_rotation"), 0},
        {QStringLiteral("subtype"), QStringLiteral("Text")},
        {QStringLiteral("rect"), QJsonArray {0.1, 0.1, 0.2, 0.2}},
        {QStringLiteral("contents"), contents},
        {QStringLiteral("flags"), 4},
        {QStringLiteral("opacity"), 1.0},
        {QStringLiteral("name"), QStringLiteral("Comment")},
    };
}

QJsonObject mutation(const QString &operationId, const QString &kind, const QString &timestamp, const QJsonObject &record)
{
    QJsonObject value {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("operation_id"), operationId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("id"), QStringLiteral("comment-one")},
        {QStringLiteral("occurred_at"), timestamp},
        {QStringLiteral("annotation_pdf_sha256"), QString(64, QLatin1Char('a'))},
    };
    if (kind == QLatin1String("upsert")) {
        value.insert(QStringLiteral("record"), record);
    } else if (kind == QLatin1String("delete")) {
        value.insert(QStringLiteral("previous_record"), record);
    } else {
        value.insert(QStringLiteral("canonical_record"), record);
    }
    return value;
}

QJsonObject visibleRecord(const QString &docsRoot)
{
    const QList<QJsonObject> records = DocsCommentSynchronizer::visibleSidecarComments(docsRoot, QStringLiteral("paper"), nullptr, nullptr);
    return records.isEmpty() ? QJsonObject() : records.constFirst();
}

QJsonObject storedRecord(const QString &docsRoot)
{
    QFile file(QDir(docsRoot).filePath(QStringLiteral("comments/paper_comments.jsonl")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readLine().trimmed()).object();
}

}

class DocsCommentMutationStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void crashReplayAndPrefixAcknowledgement();
    void projectBatchSurvivesPartialAcknowledgement();
};

void DocsCommentMutationStoreTest::projectBatchSurvivesPartialAcknowledgement()
{
    QTemporaryDir temporary;
    const auto docs = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const auto state = QDir(temporary.path()).filePath(QStringLiteral("state"));
    QVERIFY(QDir().mkpath(QDir(docs).filePath(QStringLiteral("comments"))));
    const auto first = mutation(QStringLiteral("first"), QStringLiteral("upsert"), QStringLiteral("2026-09-28T10:00:00Z"), commentRecord(QStringLiteral("first")));
    auto secondRecord = commentRecord(QStringLiteral("second"));
    secondRecord.insert(QStringLiteral("id"), QStringLiteral("comment-two"));
    auto second = mutation(QStringLiteral("second"), QStringLiteral("upsert"), QStringLiteral("2026-09-28T10:00:00Z"), secondRecord);
    second.insert(QStringLiteral("id"), QStringLiteral("comment-two"));
    DocsCommentMutationStore store(state);
    QString error;
    QVERIFY2(store.queueBatches(docs, {{QStringLiteral("first"), {first}}, {QStringLiteral("second"), {second}}}, &error), qPrintable(error));
    QVERIFY(store.hasPending(docs, QStringLiteral("first")));
    QVERIFY(store.hasPending(docs, QStringLiteral("second")));
    const auto applied = store.flush(docs, QStringLiteral("first"));
    QVERIFY2(applied.success, qPrintable(applied.failureReason));
    QVERIFY2(store.acknowledge(docs, QStringLiteral("first"), applied.appliedLines, &error), qPrintable(error));
    QVERIFY(!store.hasPending(docs, QStringLiteral("first")));
    DocsCommentMutationStore recovered(state);
    QVERIFY(recovered.hasPending(docs, QStringLiteral("second")));
    const auto remaining = recovered.flush(docs, QStringLiteral("second"));
    QVERIFY2(remaining.success, qPrintable(remaining.failureReason));
    QCOMPARE(remaining.appliedLines.size(), 1);
    QVERIFY2(recovered.acknowledge(docs, QStringLiteral("second"), remaining.appliedLines, &error), qPrintable(error));
    QVERIFY(!recovered.hasPending(docs, QStringLiteral("second")));
}

void DocsCommentMutationStoreTest::crashReplayAndPrefixAcknowledgement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = QDir(temporary.path()).filePath(QStringLiteral("docs"));
    const QString stateRoot = QDir(temporary.path()).filePath(QStringLiteral("state"));
    QVERIFY(QDir().mkpath(QDir(docsRoot).filePath(QStringLiteral("comments"))));

    DocsCommentMutationStore store(stateRoot);
    QString error;
    QJsonObject first = mutation(QStringLiteral("operation-one"),
                                       QStringLiteral("upsert"),
                                       QStringLiteral("2026-08-17T10:00:00.000Z"),
                                       commentRecord(QStringLiteral("first")));
    first.insert(QStringLiteral("create_logical_comment"), true);
    QVERIFY2(store.queue(docsRoot, QStringLiteral("paper"), {first}, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(store.journalPath(docsRoot, QStringLiteral("paper"))));

    const DocsCommentMutationFlushResult firstFlush = store.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(firstFlush.success, qPrintable(firstFlush.failureReason));
    QCOMPARE(firstFlush.appliedLines.size(), 1);
    QCOMPARE(firstFlush.createdCommentIds, QSet<QString>{QStringLiteral("comment-one")});
    QCOMPARE(visibleRecord(docsRoot).value(QStringLiteral("contents")).toString(), QStringLiteral("first"));
    const int firstRevision = storedRecord(docsRoot).value(QStringLiteral("revision")).toInt();

    DocsCommentMutationStore recoveredStore(stateRoot);
    const DocsCommentMutationFlushResult replay = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(replay.success, qPrintable(replay.failureReason));
    QCOMPARE(replay.createdCommentIds, firstFlush.createdCommentIds);
    QCOMPARE(storedRecord(docsRoot).value(QStringLiteral("revision")).toInt(), firstRevision);

    const QJsonObject second = mutation(QStringLiteral("operation-two"),
                                        QStringLiteral("upsert"),
                                        QStringLiteral("2026-08-17T10:01:00.000Z"),
                                        commentRecord(QStringLiteral("second")));
    QVERIFY2(recoveredStore.queue(docsRoot, QStringLiteral("paper"), {second}, &error), qPrintable(error));
    QVERIFY2(recoveredStore.acknowledge(docsRoot, QStringLiteral("paper"), firstFlush.appliedLines, &error), qPrintable(error));
    QVERIFY(recoveredStore.hasPending(docsRoot, QStringLiteral("paper")));

    const DocsCommentMutationFlushResult secondFlush = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(secondFlush.success, qPrintable(secondFlush.failureReason));
    QVERIFY(secondFlush.createdCommentIds.isEmpty());
    QCOMPARE(visibleRecord(docsRoot).value(QStringLiteral("contents")).toString(), QStringLiteral("second"));
    QVERIFY2(recoveredStore.acknowledge(docsRoot, QStringLiteral("paper"), secondFlush.appliedLines, &error), qPrintable(error));
    QVERIFY(!recoveredStore.hasPending(docsRoot, QStringLiteral("paper")));

    const QJsonObject liveSecond = storedRecord(docsRoot);
    const QJsonObject deletion = mutation(QStringLiteral("operation-three"),
                                          QStringLiteral("delete"),
                                          QStringLiteral("2026-08-17T10:02:00.000Z"),
                                          liveSecond);
    QVERIFY2(recoveredStore.queue(docsRoot, QStringLiteral("paper"), {deletion}, &error), qPrintable(error));
    const DocsCommentMutationFlushResult deleteFlush = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(deleteFlush.success, qPrintable(deleteFlush.failureReason));
    QVERIFY(deleteFlush.createdCommentIds.isEmpty());
    QVERIFY(visibleRecord(docsRoot).isEmpty());
    const int deletedRevision = storedRecord(docsRoot).value(QStringLiteral("revision")).toInt();
    const DocsCommentMutationFlushResult deleteReplay = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(deleteReplay.success, qPrintable(deleteReplay.failureReason));
    QCOMPARE(storedRecord(docsRoot).value(QStringLiteral("revision")).toInt(), deletedRevision);
    QVERIFY2(recoveredStore.acknowledge(docsRoot, QStringLiteral("paper"), deleteFlush.appliedLines, &error), qPrintable(error));

    const QJsonObject restoration = mutation(QStringLiteral("operation-four"),
                                             QStringLiteral("restore"),
                                             QStringLiteral("2026-08-17T10:03:00.000Z"),
                                             liveSecond);
    QVERIFY2(recoveredStore.queue(docsRoot, QStringLiteral("paper"), {restoration}, &error), qPrintable(error));
    const DocsCommentMutationFlushResult restoreFlush = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(restoreFlush.success, qPrintable(restoreFlush.failureReason));
    QCOMPARE(visibleRecord(docsRoot).value(QStringLiteral("contents")).toString(), QStringLiteral("second"));
    const int restoredRevision = storedRecord(docsRoot).value(QStringLiteral("revision")).toInt();
    const DocsCommentMutationFlushResult restoreReplay = recoveredStore.flush(docsRoot, QStringLiteral("paper"));
    QVERIFY2(restoreReplay.success, qPrintable(restoreReplay.failureReason));
    QCOMPARE(storedRecord(docsRoot).value(QStringLiteral("revision")).toInt(), restoredRevision);
    QVERIFY2(recoveredStore.acknowledge(docsRoot, QStringLiteral("paper"), restoreFlush.appliedLines, &error), qPrintable(error));
}

QTEST_GUILESS_MAIN(DocsCommentMutationStoreTest)

#include "docscommentmutationstoretest.moc"
