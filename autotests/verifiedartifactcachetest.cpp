/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../part/verifiedartifactcache.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

namespace
{
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Writes `bytes` as the object named by its digest and returns the digest.
QString writeObject(const QDir &directory, const QByteArray &bytes)
{
    const QString digest = sha256(bytes);
    QFile file(directory.filePath(digest));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return digest;
}

// A source artifact of `filename` as the server builds one: mostly the plan of
// the basic review cards and the review card anchors.
QByteArray sourceArtifact(const QString &filename, int revision, int bulk)
{
    QJsonArray cards;
    QJsonArray anchors;
    for (int index = 0; index < bulk; ++index) {
        cards.append(QJsonObject {{QStringLiteral("key"), QStringLiteral("card-%1-%2").arg(revision).arg(index)}, {QStringLiteral("regions"), QJsonArray {0.1, 0.2, 0.8, 0.9}}});
        anchors.append(QJsonObject {{QStringLiteral("id"), QStringLiteral("anchor-%1-%2").arg(revision).arg(index)}, {QStringLiteral("page"), index % 40 + 1}, {QStringLiteral("rect"), QJsonArray {0.1, 0.2, 0.8, 0.9}}});
    }
    const QJsonObject artifact {{QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), filename},
                                {QStringLiteral("revision"), revision},
                                {QStringLiteral("sources"), QJsonArray {QStringLiteral("docs-sam/tex/source/intro.tex")}},
                                {QStringLiteral("review_card_anchors"), anchors},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), filename},
                                                                    {QStringLiteral("page_count"), 40},
                                                                    {QStringLiteral("segments"), QJsonArray {QJsonObject {{QStringLiteral("source_id"), QStringLiteral("docs-sam/tex/source/intro.tex")}, {QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), 40}}}},
                                                                    {QStringLiteral("basic_review"), QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("cards"), cards}}}}}};
    return QJsonDocument(artifact).toJson(QJsonDocument::Compact);
}
}

class VerifiedArtifactCacheTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void testLimitsAreThirtyTwoMebibytes();
    void testVerifyingKeepsNoBytes();
    void testBytesAreBoundedAndLeastRecentlyUsedGoFirst();
    void testARewrittenFileIsVerifiedAgain();
    void testParsedArtifactsLeaveOutTheUnreadSections();
    void testParsedArtifactsAreBounded();
    void testARebuiltPdfReplacesItsArtifact();
    void testVerifiedSourceArtifactReadsOnceAndKeepsNoBytes();
};

void VerifiedArtifactCacheTest::init()
{
    Thoughts::clearVerifiedArtifactCaches();
}

void VerifiedArtifactCacheTest::testLimitsAreThirtyTwoMebibytes()
{
    QCOMPARE(Thoughts::VerifiedArtifactBytesLimit, qint64(32) * 1024 * 1024);
    QCOMPARE(Thoughts::SourceArtifactJsonBytesLimit, qint64(32) * 1024 * 1024);
    const Thoughts::VerifiedArtifactCacheStatistics empty = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(empty.bytesKept, 0);
    QCOMPARE(empty.jsonBytesKept, 0);
    QCOMPARE(empty.verifiedIdentities, 0);
}

void VerifiedArtifactCacheTest::testVerifyingKeepsNoBytes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir directory(root.path());
    QStringList digests;
    for (int index = 0; index < 40; ++index) {
        digests.append(writeObject(directory, QByteArray(256 * 1024, char('a' + index % 26)) + QByteArray::number(index)));
    }
    // A Sync checks every object of the Library and wants none of their bytes.
    for (const QString &digest : std::as_const(digests)) {
        QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digest), digest));
    }
    Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.bytesEntries, 0);
    QCOMPARE(statistics.bytesKept, 0);
    QCOMPARE(statistics.verifiedIdentities, 40);
    QCOMPARE(statistics.bytesMisses, 40);
    // Verified once: asked again, the files are not read.
    for (const QString &digest : std::as_const(digests)) {
        QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digest), digest));
    }
    statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.bytesMisses, 40);
    QCOMPARE(statistics.bytesKept, 0);

    // A caller that wants the bytes gets them, and they stay.
    QByteArray bytes;
    QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digests.constFirst()), digests.constFirst(), &bytes));
    QCOMPARE(sha256(bytes), digests.constFirst());
    statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.bytesEntries, 1);
    QCOMPARE(statistics.bytesKept, bytes.size());
    QByteArray again;
    QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digests.constFirst()), digests.constFirst(), &again));
    QCOMPARE(again, bytes);
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().bytesHits, 1);
    // Wrong digests and missing files do not verify.
    QVERIFY(!Thoughts::readVerifiedArtifact(directory.filePath(digests.at(1)), digests.at(2)));
    QVERIFY(!Thoughts::readVerifiedArtifact(directory.filePath(QStringLiteral("missing")), digests.at(2)));
    QVERIFY(!Thoughts::readVerifiedArtifact(directory.filePath(digests.at(1)), QStringLiteral("short")));
}

void VerifiedArtifactCacheTest::testBytesAreBoundedAndLeastRecentlyUsedGoFirst()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir directory(root.path());
    // 48 objects of 1 MiB: half as much again as may be kept.
    constexpr int megabyte = 1024 * 1024;
    QStringList digests;
    for (int index = 0; index < 48; ++index) {
        digests.append(writeObject(directory, QByteArray(megabyte - 16, char('a' + index % 26)) + QByteArray::number(index).rightJustified(16, '0')));
    }
    QByteArray bytes;
    for (const QString &digest : std::as_const(digests)) {
        QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digest), digest, &bytes));
        QCOMPARE(bytes.size(), megabyte);
    }
    Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    QVERIFY2(statistics.bytesKept <= Thoughts::VerifiedArtifactBytesLimit, qPrintable(QString::number(statistics.bytesKept)));
    QCOMPARE(statistics.bytesEntries, 32);
    // The newest are kept, the oldest read from the file again.
    const qint64 misses = statistics.bytesMisses;
    QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digests.constLast()), digests.constLast(), &bytes));
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().bytesMisses, misses);
    QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digests.constFirst()), digests.constFirst(), &bytes));
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().bytesMisses, misses + 1);
    QCOMPARE(sha256(bytes), digests.constFirst());

    // An artifact larger than the largest that is kept is returned and not kept.
    Thoughts::clearVerifiedArtifactCaches();
    const QString large = writeObject(directory, QByteArray(int(Thoughts::VerifiedArtifactLargestKept) + 1, 'z'));
    QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(large), large, &bytes));
    QCOMPARE(bytes.size(), Thoughts::VerifiedArtifactLargestKept + 1);
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().bytesKept, 0);
}

void VerifiedArtifactCacheTest::testARewrittenFileIsVerifiedAgain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir directory(root.path());
    const QByteArray contents("{\"value\":1}");
    const QString digest = writeObject(directory, contents);
    const QString path = directory.filePath(digest);
    QByteArray bytes;
    QVERIFY(Thoughts::readVerifiedArtifact(path, digest, &bytes));
    QVERIFY(Thoughts::readVerifiedArtifact(path, digest));
    // Same length, other bytes: neither the kept bytes nor the earlier verdict apply.
    QTest::qWait(20);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write("{\"value\":2}"), qint64(contents.size()));
    file.close();
    QVERIFY(!Thoughts::readVerifiedArtifact(path, digest));
    QVERIFY(!Thoughts::readVerifiedArtifact(path, digest, &bytes));
    QVERIFY(Thoughts::verifiedSourceArtifactJson(path, digest).isEmpty());
}

void VerifiedArtifactCacheTest::testParsedArtifactsLeaveOutTheUnreadSections()
{
    const QByteArray contents = sourceArtifact(QStringLiteral("paper.pdf"), 1, 400);
    const QString digest = sha256(contents);
    const QJsonObject parsed = Thoughts::sourceArtifactJson(contents, digest);
    // Everything the app reads from a parsed artifact is there ...
    QCOMPARE(parsed.value(QStringLiteral("kind")).toString(), QStringLiteral("thoughts-document-source-artifact"));
    QCOMPARE(parsed.value(QStringLiteral("filename")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(parsed.value(QStringLiteral("sources")).toArray().size(), 1);
    const QJsonObject pdf = parsed.value(QStringLiteral("pdf")).toObject();
    QCOMPARE(pdf.value(QStringLiteral("filename")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(pdf.value(QStringLiteral("page_count")).toInt(), 40);
    QCOMPARE(pdf.value(QStringLiteral("segments")).toArray().size(), 1);
    // ... and the two sections nothing reads from it are not.
    QVERIFY(!parsed.contains(QStringLiteral("review_card_anchors")));
    QVERIFY(!pdf.contains(QStringLiteral("basic_review")));
    // The same object the second time, without parsing.
    QCOMPARE(Thoughts::sourceArtifactJson(QByteArray(), digest), parsed);
    const Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.jsonEntries, 1);
    QCOMPARE(statistics.jsonHits, 1);
    QCOMPARE(statistics.jsonMisses, 1);
    // What is kept is a small part of the artifact.
    QVERIFY2(statistics.jsonBytesKept * 5 < contents.size(), qPrintable(QStringLiteral("%1 of %2").arg(statistics.jsonBytesKept).arg(contents.size())));
    // Unreadable contents give an empty object and keep nothing.
    QVERIFY(Thoughts::sourceArtifactJson(QByteArrayLiteral("not json"), sha256(QByteArrayLiteral("not json"))).isEmpty());
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().jsonEntries, 1);
}

void VerifiedArtifactCacheTest::testParsedArtifactsAreBounded()
{
    // Artifacts whose kept part is large: 300 of them would take about 60 MiB.
    for (int index = 0; index < 300; ++index) {
        QJsonArray sources;
        for (int source = 0; source < 900; ++source) {
            sources.append(QStringLiteral("docs-sam/tex/source/chapter-%1/section-%2.tex").arg(index).arg(source));
        }
        const QByteArray contents = QJsonDocument(QJsonObject {{QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")}, {QStringLiteral("sources"), sources}}).toJson(QJsonDocument::Compact);
        QCOMPARE(Thoughts::sourceArtifactJson(contents, sha256(contents)).value(QStringLiteral("sources")).toArray().size(), 900);
    }
    const Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    QVERIFY2(statistics.jsonBytesKept <= Thoughts::SourceArtifactJsonBytesLimit, qPrintable(QString::number(statistics.jsonBytesKept)));
    QVERIFY2(statistics.jsonBytesKept > Thoughts::SourceArtifactJsonBytesLimit / 2, qPrintable(QString::number(statistics.jsonBytesKept)));
    QVERIFY(statistics.jsonEntries < 300);
    QCOMPARE(statistics.jsonMisses, 300);
}

void VerifiedArtifactCacheTest::testARebuiltPdfReplacesItsArtifact()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir directory(root.path());
    // Twenty PDFs, each rebuilt ten times: only the latest artifact of each stays.
    QHash<QString, QString> latest;
    for (int revision = 1; revision <= 10; ++revision) {
        for (int pdf = 0; pdf < 20; ++pdf) {
            const QString filename = QStringLiteral("paper-%1.pdf").arg(pdf);
            const QString digest = writeObject(directory, sourceArtifact(filename, revision, 20));
            QCOMPARE(Thoughts::verifiedSourceArtifactJson(directory.filePath(digest), digest, filename).value(QStringLiteral("revision")).toInt(), revision);
            QByteArray bytes;
            QVERIFY(Thoughts::readVerifiedArtifact(directory.filePath(digest), digest, &bytes, filename));
            latest.insert(filename, digest);
        }
    }
    Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.jsonEntries, 20);
    QCOMPARE(statistics.bytesEntries, 20);
    QCOMPARE(statistics.jsonMisses, 200);
    for (auto it = latest.cbegin(); it != latest.cend(); ++it) {
        QCOMPARE(Thoughts::verifiedSourceArtifactJson(directory.filePath(it.value()), it.value(), it.key()).value(QStringLiteral("revision")).toInt(), 10);
    }
    statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.jsonMisses, 200);
    QCOMPARE(statistics.jsonHits, 20);
    // Without an owner nothing is replaced: the bound alone applies.
    for (int revision = 1; revision <= 3; ++revision) {
        const QByteArray contents = sourceArtifact(QStringLiteral("other.pdf"), revision, 20);
        Thoughts::sourceArtifactJson(contents, sha256(contents));
    }
    QCOMPARE(Thoughts::verifiedArtifactCacheStatistics().jsonEntries, 23);
}

void VerifiedArtifactCacheTest::testVerifiedSourceArtifactReadsOnceAndKeepsNoBytes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir directory(root.path());
    const QByteArray contents = sourceArtifact(QStringLiteral("paper.pdf"), 7, 200);
    const QString digest = writeObject(directory, contents);
    const QString path = directory.filePath(digest);
    const QJsonObject parsed = Thoughts::verifiedSourceArtifactJson(path, digest, QStringLiteral("paper.pdf"));
    QCOMPARE(parsed.value(QStringLiteral("revision")).toInt(), 7);
    Thoughts::VerifiedArtifactCacheStatistics statistics = Thoughts::verifiedArtifactCacheStatistics();
    // Read and hashed once; the bytes are gone, the parsed artifact stays.
    QCOMPARE(statistics.bytesMisses, 1);
    QCOMPARE(statistics.bytesKept, 0);
    QCOMPARE(statistics.jsonEntries, 1);
    QCOMPARE(statistics.verifiedIdentities, 1);
    QCOMPARE(Thoughts::verifiedSourceArtifactJson(path, digest, QStringLiteral("paper.pdf")), parsed);
    statistics = Thoughts::verifiedArtifactCacheStatistics();
    QCOMPARE(statistics.bytesMisses, 1);
    QCOMPARE(statistics.jsonHits, 1);
    // The same as the two calls it replaces.
    QByteArray bytes;
    QVERIFY(Thoughts::readVerifiedArtifact(path, digest, &bytes));
    QCOMPARE(Thoughts::sourceArtifactJson(bytes, digest), parsed);
    // A file that is gone does not verify, whatever is kept.
    QVERIFY(QFile::remove(path));
    QVERIFY(Thoughts::verifiedSourceArtifactJson(path, digest, QStringLiteral("paper.pdf")).isEmpty());
    // Not the artifact it claims to be.
    const QString other = writeObject(directory, sourceArtifact(QStringLiteral("other.pdf"), 1, 5));
    QVERIFY(Thoughts::verifiedSourceArtifactJson(directory.filePath(other), digest).isEmpty());
}

QTEST_GUILESS_MAIN(VerifiedArtifactCacheTest)
#include "verifiedartifactcachetest.moc"
