/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// The queue that writes Accept/Reject from the proposal window off the UI
// thread: durability, refusals, ordering and two quick decisions in one file.

#include "../part/agentproposaldecisionqueue.h"
#include "../part/projectconfiguration.h"
#include "../shell/proposaldecisions.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QUuid>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using Thoughts::LiveProposalDecisionQueue;
using Thoughts::LiveProposalDecisionRequest;
using Thoughts::LiveProposalDecisionResult;

namespace
{
QByteArray wrapper(const QString &id, const QString &before, const QString &after)
{
    return QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{%1}{Why.}{%2}{%3}")
        .arg(id, before, after)
        .toUtf8();
}

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}

class AgentProposalDecisionQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void acceptIsWrittenWithItsClaim();
    void rejectRestoresTheOriginal();
    void twoQuickDecisionsInOneFile();
    void severalProposalsInOneDecision();
    void externalChangeIsRefused();
    void externalChangeAfterOwnWriteIsRefused();
    void destructorWaitsForTheWrite();
    void moveHalfDecidesItsPartnerInAnotherFile();

private:
    QString writeSource(const QByteArray &contents);
    QByteArray readSource() const;
    LiveProposalDecisionRequest request(const QString &id, const QString &action, const QString &expectedSha256) const;
    QList<LiveProposalDecisionResult> results(QSignalSpy &spy, int count);

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_source;
    QString m_claims;
};

void AgentProposalDecisionQueueTest::init()
{
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    const QString directory = QDir(m_root->path()).filePath(QStringLiteral("docs-test/tex/source"));
    QVERIFY(QDir().mkpath(directory));
    m_source = QDir(directory).filePath(QStringLiteral("main.tex"));
    // A Project connects the folder, so Sync manages the source and decisions get claims.
    QJsonObject project = Thoughts::ProjectConfiguration::empty(m_root->path(), QStringLiteral("project"), QStringLiteral("Project"));
    QJsonArray folders = project.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject {{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("path"), QStringLiteral("docs-test")},
                                {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    project.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(m_root->path(), QStringLiteral("project")), project).isEmpty());
    m_claims = QDir(m_root->path()).filePath(QStringLiteral("state/desktop-sync.sqlite"));
}

QString AgentProposalDecisionQueueTest::writeSource(const QByteArray &contents)
{
    QFile file(m_source);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(contents) != contents.size()) {
        return {};
    }
    return sha(contents);
}

QByteArray AgentProposalDecisionQueueTest::readSource() const
{
    QFile file(m_source);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

LiveProposalDecisionRequest AgentProposalDecisionQueueTest::request(const QString &id, const QString &action, const QString &expectedSha256) const
{
    LiveProposalDecisionRequest request;
    request.docsRoot = m_root->path();
    request.sourcePath = m_source;
    request.expectedSourceSha256 = expectedSha256;
    request.proposalIds = {id};
    request.action = action;
    request.options.claimDatabase = m_claims;
    return request;
}

QList<LiveProposalDecisionResult> AgentProposalDecisionQueueTest::results(QSignalSpy &spy, int count)
{
    QList<LiveProposalDecisionResult> collected;
    if (!QTest::qWaitFor([&spy, count] { return spy.size() >= count; }, 10000)) {
        return collected;
    }
    for (const QList<QVariant> &arguments : std::as_const(spy)) {
        collected.append(arguments.constFirst().value<LiveProposalDecisionResult>());
    }
    return collected;
}

void AgentProposalDecisionQueueTest::severalProposalsInOneDecision()
{
    const QByteArray contents = "A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " and "
        + wrapper(QStringLiteral("agent-b"), QStringLiteral("Old B"), QStringLiteral("New B")) + " end.\n";
    const QString original = writeSource(contents);
    QVERIFY(!original.isEmpty());
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    LiveProposalDecisionRequest accept = request(QStringLiteral("agent-a"), QStringLiteral("accept"), original);
    accept.proposalIds = {QStringLiteral("agent-a"), QStringLiteral("agent-b")};
    queue.enqueue(accept);
    auto collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY2(collected.first().ok, qPrintable(collected.first().error));
    QCOMPARE(readSource(), QByteArray("A New A and New B end.\n"));
    // One write, so one Undo brings both back.
    QCOMPARE(collected.first().previousContents, contents);

    const QString again = writeSource(contents);
    finished.clear();
    LiveProposalDecisionRequest reject = accept;
    reject.action = QStringLiteral("disregard");
    reject.expectedSourceSha256 = again;
    queue.enqueue(reject);
    collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY2(collected.first().ok, qPrintable(collected.first().error));
    QCOMPARE(readSource(), QByteArray("A Old A and Old B end.\n"));
}

void AgentProposalDecisionQueueTest::acceptIsWrittenWithItsClaim()
{
    const QString original = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " end.\n");
    QVERIFY(!original.isEmpty());
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    const quint64 ticket = queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("accept"), original));
    QCOMPARE(queue.pending(), 1);
    const auto collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY2(collected.first().ok, qPrintable(collected.first().error));
    QCOMPARE(collected.first().ticket, ticket);
    QCOMPARE(queue.pending(), 0);
    QCOMPARE(readSource(), QByteArray("A New A end.\n"));
    QCOMPARE(collected.first().updatedSourceSha256, sha(readSource()));
    QCOMPARE(sha(collected.first().previousContents), original);
    // The decision claim (protocol §10.5) is recorded as for a decision made on the UI thread.
    const QList<QJsonObject> claims = Thoughts::ProposalDecisions::storedClaims(m_claims);
    QCOMPARE(claims.size(), 1);
    QCOMPARE(claims.first().value(QStringLiteral("proposal_id")).toString(), QStringLiteral("agent-a"));
    QCOMPARE(claims.first().value(QStringLiteral("action")).toString(), QStringLiteral("accept"));
    QCOMPARE(claims.first().value(QStringLiteral("origin")).toString(), QStringLiteral("desktop"));
}

void AgentProposalDecisionQueueTest::rejectRestoresTheOriginal()
{
    const QString original = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " end.\n");
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("disregard"), original));
    const auto collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY2(collected.first().ok, qPrintable(collected.first().error));
    QCOMPARE(readSource(), QByteArray("A Old A end.\n"));
    const QList<QJsonObject> claims = Thoughts::ProposalDecisions::storedClaims(m_claims);
    QCOMPARE(claims.size(), 1);
    QCOMPARE(claims.first().value(QStringLiteral("action")).toString(), QStringLiteral("reject"));
}

void AgentProposalDecisionQueueTest::twoQuickDecisionsInOneFile()
{
    // Both proposals were shown from the same revision: the second click
    // happens before the first decision's write reaches the view.
    const QString original = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " B "
                                         + wrapper(QStringLiteral("agent-b"), QStringLiteral("Old B"), QStringLiteral("New B")) + " C "
                                         + wrapper(QStringLiteral("agent-c"), QStringLiteral("Old C"), QStringLiteral("New C")) + " end.\n");
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    const quint64 first = queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("accept"), original));
    const quint64 second = queue.enqueue(request(QStringLiteral("agent-b"), QStringLiteral("disregard"), original));
    const quint64 third = queue.enqueue(request(QStringLiteral("agent-c"), QStringLiteral("accept"), original));
    QCOMPARE(queue.pending(), 3);
    const auto collected = results(finished, 3);
    QCOMPARE(collected.size(), 3);
    // Reported in click order, each applied over the previous one.
    QCOMPARE(collected.at(0).ticket, first);
    QCOMPARE(collected.at(1).ticket, second);
    QCOMPARE(collected.at(2).ticket, third);
    for (const auto &result : collected) {
        QVERIFY2(result.ok, qPrintable(result.error));
    }
    QCOMPARE(sha(collected.at(1).previousContents), collected.at(0).updatedSourceSha256);
    QCOMPARE(sha(collected.at(2).previousContents), collected.at(1).updatedSourceSha256);
    QCOMPARE(readSource(), QByteArray("A New A B Old B C New C end.\n"));
    QCOMPARE(Thoughts::ProposalDecisions::storedClaims(m_claims).size(), 3);
}

void AgentProposalDecisionQueueTest::externalChangeIsRefused()
{
    const QString shown = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " end.\n");
    const QByteArray edited = "Edited in VS Code. A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " end.\n";
    QVERIFY(!writeSource(edited).isEmpty());
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("accept"), shown));
    const auto collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY(!collected.first().ok);
    QCOMPARE(collected.first().error, QStringLiteral("The TeX source changed before this proposal could be applied."));
    QCOMPARE(readSource(), edited);
    QVERIFY(Thoughts::ProposalDecisions::storedClaims(m_claims).isEmpty());
}

void AgentProposalDecisionQueueTest::externalChangeAfterOwnWriteIsRefused()
{
    const QString original = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " B "
                                         + wrapper(QStringLiteral("agent-b"), QStringLiteral("Old B"), QStringLiteral("New B")) + " end.\n");
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("accept"), original));
    QCOMPARE(results(finished, 1).size(), 1);
    // Someone else edits the file after the queue's own write.
    QByteArray edited = readSource();
    edited.prepend("Edited elsewhere. ");
    QVERIFY(!writeSource(edited).isEmpty());
    queue.enqueue(request(QStringLiteral("agent-b"), QStringLiteral("accept"), original));
    const auto collected = results(finished, 2);
    QCOMPARE(collected.size(), 2);
    QVERIFY(!collected.at(1).ok);
    QCOMPARE(readSource(), edited);
}

void AgentProposalDecisionQueueTest::destructorWaitsForTheWrite()
{
    const QString original = writeSource("A " + wrapper(QStringLiteral("agent-a"), QStringLiteral("Old A"), QStringLiteral("New A")) + " end.\n");
    {
        LiveProposalDecisionQueue queue;
        queue.enqueue(request(QStringLiteral("agent-a"), QStringLiteral("accept"), original));
        // Closing the reader right after the click: no event loop runs.
    }
    QCOMPARE(readSource(), QByteArray("A New A end.\n"));
    QCOMPARE(Thoughts::ProposalDecisions::storedClaims(m_claims).size(), 1);
}


// protocol/agent-proposals.md §12: accepting the removal half of a card move accepts its
// insertion in another file, in the same write, with a claim for each half.
void AgentProposalDecisionQueueTest::moveHalfDecidesItsPartnerInAnotherFile()
{
    const QString base = QStringLiteral("agent-0d1e2f30-4152-4637-8899-aabbccddeeff");
    const QString passage = QStringLiteral("\\ReviewCardMaskStart{6f1c2a0e-3b4d-4c5e-8f60-718293a4b5c6}{0}%\nAnswer.\n"
                                           "\\ReviewCardMaskEnd{6f1c2a0e-3b4d-4c5e-8f60-718293a4b5c6}{0}%\n");
    const QString original = writeSource("Intro.\n" + wrapper(base + QStringLiteral(".cut"), passage, QString()) + "Outro.\n");
    QVERIFY(!original.isEmpty());
    const QString other = QDir(QFileInfo(m_source).absolutePath()).filePath(QStringLiteral("other.tex"));
    {
        QFile file(other);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("First.\n" + wrapper(base + QStringLiteral(".paste"), QString(), passage) + "Last.\n");
    }
    LiveProposalDecisionQueue queue;
    QSignalSpy finished(&queue, &LiveProposalDecisionQueue::finished);
    queue.enqueue(request(base + QStringLiteral(".cut"), QStringLiteral("accept"), original));
    const auto collected = results(finished, 1);
    QCOMPARE(collected.size(), 1);
    QVERIFY2(collected.first().ok, qPrintable(collected.first().error));
    QCOMPARE(readSource(), QByteArray("Intro.\nOutro.\n"));
    QFile file(other);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), "First.\n" + passage.toUtf8() + "Last.\n");
    QCOMPARE(collected.first().otherSources.size(), 1);
    QCOMPARE(collected.first().otherSources.first().path, QFileInfo(other).canonicalFilePath());
    QStringList claimed;
    for (const auto &claim : Thoughts::ProposalDecisions::storedClaims(m_claims))
        claimed.append(claim.value(QStringLiteral("proposal_id")).toString() + QLatin1Char(':') + claim.value(QStringLiteral("action")).toString());
    claimed.sort();
    QCOMPARE(claimed, QStringList({base + QStringLiteral(".cut:accept"), base + QStringLiteral(".paste:accept")}));
}
QTEST_GUILESS_MAIN(AgentProposalDecisionQueueTest)
#include "agentproposaldecisionqueuetest.moc"
