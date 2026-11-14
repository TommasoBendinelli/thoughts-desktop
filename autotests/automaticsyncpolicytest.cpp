/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/automaticsyncpolicy.h"

#include <QTest>

using Thoughts::AutomaticSyncPolicy;
using Decision = AutomaticSyncPolicy::Decision;
using Initiation = AutomaticSyncPolicy::Initiation;
using FailureKind = AutomaticSyncPolicy::FailureKind;

namespace
{
constexpr qint64 minute = 60 * 1000;
// The app asks the server every 20 seconds.
constexpr qint64 poll = 20 * 1000;

// One question to the server and its answer.
Decision ask(AutomaticSyncPolicy &policy, bool pending, const QString &key, qint64 nowMs)
{
    policy.stateRequested(nowMs);
    return policy.stateAnswered(pending, key, nowMs);
}

// An automatic Sync for `key` that fails, and the question asked right after it.
void failAutomatic(AutomaticSyncPolicy &policy, const QString &key, const QString &keyAfter, qint64 nowMs, FailureKind kind = FailureKind::Unknown)
{
    policy.automaticSyncStartedFor(key);
    policy.syncStarted(Initiation::Automatic);
    policy.syncFinished(false, kind, nowMs);
    ask(policy, true, keyAfter, nowMs + 1000);
}
}

class AutomaticSyncPolicyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testWaitsDoubleUpToTenMinutes();
    void testRemoteWorkStartsOneSyncPerState();
    void testAFailedSyncsOwnStateStartsNothing();
    void testFailuresInARowWaitLonger();
    void testSuccessUserSyncAndLocalChangeEndTheWait();
    void testUnreachableServerIsTriedAgainAfterTheWait();
    void testTransientFailureIsTriedAgainAfterTheWait();
    void testAFailureThatNeedsTheUserHoldsBackOnlyTheDecisionSync();
    void testFailureKindsComeFromTheRepository();
    void testAnEndedBuildDoesNotWait();
    void testAFailedUserSyncStartsNoAutomaticOne();
    void testAnAnswerAskedBeforeTheEndIsNotTheStateAfter();
    void testFailingSyncsThatChangeTheStateStartFewSyncs();
};

void AutomaticSyncPolicyTest::testWaitsDoubleUpToTenMinutes()
{
    QCOMPARE(AutomaticSyncPolicy::waitAfter(0), 0);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(1), 1 * minute);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(2), 2 * minute);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(3), 4 * minute);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(4), 8 * minute);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(5), 10 * minute);
    QCOMPARE(AutomaticSyncPolicy::waitAfter(50), 10 * minute);
    QCOMPARE(AutomaticSyncPolicy::FirstRetryWaitMs, minute);
    QCOMPARE(AutomaticSyncPolicy::MaximumRetryWaitMs, 10 * minute);
}

void AutomaticSyncPolicyTest::testRemoteWorkStartsOneSyncPerState()
{
    AutomaticSyncPolicy policy;
    QCOMPARE(ask(policy, false, QStringLiteral("10/a/1"), 0), Decision::NothingToDo);
    // The server was not reached.
    QCOMPARE(ask(policy, false, QString(), poll), Decision::NothingToDo);
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), 2 * poll), Decision::Start);
    policy.automaticSyncStartedFor(QStringLiteral("11/a/1"));
    policy.syncStarted(Initiation::Automatic);
    policy.syncFinished(true, FailureKind::Unknown, 3 * poll);
    QCOMPARE(policy.notBeforeMs(), 0);
    QCOMPARE(ask(policy, false, QStringLiteral("11/a/1"), 4 * poll), Decision::NothingToDo);
    // The server still reports work for the state a Sync just ran for: not again.
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), 5 * poll), Decision::Covered);
    // Another device wrote.
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/1"), 6 * poll), Decision::Start);
}

void AutomaticSyncPolicyTest::testAFailedSyncsOwnStateStartsNothing()
{
    AutomaticSyncPolicy policy;
    qint64 now = 0;
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), now), Decision::Start);
    // The failing Sync wrote to the server: its state has a new key.
    failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/2"), now);
    QCOMPARE(policy.consecutiveFailures(), 1);
    QCOMPARE(policy.notBeforeMs(), now + minute);
    // However long it is asked: the state the failed Sync left starts nothing.
    for (now = poll; now < 30 * minute; now += poll) {
        QCOMPARE(ask(policy, true, QStringLiteral("12/a/2"), now), Decision::Covered);
    }
    // A change from elsewhere does.
    QCOMPARE(ask(policy, true, QStringLiteral("13/a/2"), now), Decision::Start);
}

void AutomaticSyncPolicyTest::testFailuresInARowWaitLonger()
{
    AutomaticSyncPolicy policy;
    qint64 now = 0;
    int sequence = 100;
    const QList<qint64> waits {1 * minute, 2 * minute, 4 * minute, 8 * minute, 10 * minute, 10 * minute};
    for (const qint64 wait : waits) {
        const QString key = QStringLiteral("%1/a/1").arg(sequence);
        QCOMPARE(ask(policy, true, key, now), Decision::Start);
        // Another device keeps writing, so every answer names a new state.
        failAutomatic(policy, key, QStringLiteral("%1/a/1").arg(++sequence), now);
        QCOMPARE(policy.notBeforeMs(), now + wait);
        const QString changed = QStringLiteral("%1/a/1").arg(++sequence);
        QCOMPARE(ask(policy, true, changed, now + poll), Decision::Waiting);
        QCOMPARE(ask(policy, true, changed, now + wait - 1), Decision::Waiting);
        now += wait;
    }
    QCOMPARE(policy.consecutiveFailures(), int(waits.size()));
}

void AutomaticSyncPolicyTest::testSuccessUserSyncAndLocalChangeEndTheWait()
{
    // A change the user makes on this computer.
    {
        AutomaticSyncPolicy policy;
        failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/1"), 0);
        QCOMPARE(ask(policy, true, QStringLiteral("13/a/1"), poll), Decision::Waiting);
        policy.localChange();
        QCOMPARE(policy.notBeforeMs(), 0);
        QCOMPARE(policy.consecutiveFailures(), 0);
        QCOMPARE(ask(policy, true, QStringLiteral("13/a/1"), 2 * poll), Decision::Start);
        // The state the failed Sync left is still its own.
        QCOMPARE(ask(policy, true, QStringLiteral("12/a/1"), 2 * poll), Decision::Covered);
    }
    // A Sync the user starts, whatever comes of it.
    {
        AutomaticSyncPolicy policy;
        failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/1"), 0);
        failAutomatic(policy, QStringLiteral("13/a/1"), QStringLiteral("14/a/1"), 2 * minute);
        QCOMPARE(policy.consecutiveFailures(), 2);
        policy.syncStarted(Initiation::User);
        QCOMPARE(policy.notBeforeMs(), 0);
        QCOMPARE(policy.consecutiveFailures(), 0);
    }
    // A Sync that succeeds, also one a command asked for.
    {
        AutomaticSyncPolicy policy;
        failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/1"), 0);
        policy.syncStarted(Initiation::Command);
        // A command neither waits nor ends the wait by starting.
        QCOMPARE(policy.consecutiveFailures(), 1);
        policy.syncFinished(true, FailureKind::Unknown, poll);
        QCOMPARE(policy.notBeforeMs(), 0);
        QCOMPARE(policy.consecutiveFailures(), 0);
        QCOMPARE(ask(policy, true, QStringLiteral("15/a/1"), 2 * poll), Decision::Start);
    }
    // After the wait ended the next failure starts at one minute again.
    {
        AutomaticSyncPolicy policy;
        failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/1"), 0);
        failAutomatic(policy, QStringLiteral("13/a/1"), QStringLiteral("14/a/1"), 2 * minute);
        policy.localChange();
        failAutomatic(policy, QStringLiteral("15/a/1"), QStringLiteral("16/a/1"), 5 * minute);
        QCOMPARE(policy.notBeforeMs(), 6 * minute);
    }
}

void AutomaticSyncPolicyTest::testUnreachableServerIsTriedAgainAfterTheWait()
{
    AutomaticSyncPolicy policy;
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), 0), Decision::Start);
    // The network went away: the Sync fails and so does the question after it.
    failAutomatic(policy, QStringLiteral("11/a/1"), QString(), 0);
    QCOMPARE(ask(policy, false, QString(), poll), Decision::NothingToDo);
    // Back within the wait: not yet.
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), 2 * poll), Decision::Waiting);
    // After it the same state is tried again, since nothing says the failed Sync reached the server.
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), minute), Decision::Start);
}

void AutomaticSyncPolicyTest::testTransientFailureIsTriedAgainAfterTheWait()
{
    AutomaticSyncPolicy policy;
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), 0), Decision::Start);
    failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("11/a/1"), 0, FailureKind::Transient);
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), poll), Decision::Waiting);
    QCOMPARE(ask(policy, true, QStringLiteral("11/a/1"), minute), Decision::Start);
    // A failure that is not known to be transient leaves the same state alone.
    AutomaticSyncPolicy other;
    failAutomatic(other, QStringLiteral("11/a/1"), QStringLiteral("11/a/1"), 0, FailureKind::Bug);
    QCOMPARE(ask(other, true, QStringLiteral("11/a/1"), 30 * minute), Decision::Covered);
}

void AutomaticSyncPolicyTest::testAFailureThatNeedsTheUserHoldsBackOnlyTheDecisionSync()
{
    AutomaticSyncPolicy policy;
    failAutomatic(policy, QStringLiteral("11/a/1"), QStringLiteral("12/a/1"), 0, FailureKind::NeedsUser);
    QVERIFY(policy.lastFailureNeedsUser());
    // The state that Sync left starts nothing, as after any failure ...
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/1"), 30 * minute), Decision::Covered);
    // ... but what another device sends is still fetched, after the wait.
    QCOMPARE(ask(policy, true, QStringLiteral("13/a/1"), poll), Decision::Waiting);
    QCOMPARE(ask(policy, true, QStringLiteral("13/a/1"), minute), Decision::Start);
    // A change on this computer ends the wait, not the need for the user.
    policy.localChange();
    QVERIFY(policy.lastFailureNeedsUser());
    // The user's own Sync is the answer, and so is any Sync that gets through.
    policy.syncStarted(Initiation::User);
    QVERIFY(!policy.lastFailureNeedsUser());
    policy.syncFinished(false, FailureKind::NeedsUser, 31 * minute);
    QVERIFY(policy.lastFailureNeedsUser());
    policy.syncStarted(Initiation::Command);
    QVERIFY(policy.lastFailureNeedsUser());
    policy.syncFinished(true, FailureKind::Unknown, 32 * minute);
    QVERIFY(!policy.lastFailureNeedsUser());
    // A failure of another kind does not need the user.
    failAutomatic(policy, QStringLiteral("14/a/1"), QStringLiteral("15/a/1"), 33 * minute, FailureKind::Bug);
    QVERIFY(!policy.lastFailureNeedsUser());
}

void AutomaticSyncPolicyTest::testFailureKindsComeFromTheRepository()
{
    QCOMPARE(AutomaticSyncPolicy::failureKindFromName(QStringLiteral("transient")), FailureKind::Transient);
    QCOMPARE(AutomaticSyncPolicy::failureKindFromName(QStringLiteral("needs_user")), FailureKind::NeedsUser);
    QCOMPARE(AutomaticSyncPolicy::failureKindFromName(QStringLiteral("bug")), FailureKind::Bug);
    // A Sync that could not be started, or a part that reports no kind.
    QCOMPARE(AutomaticSyncPolicy::failureKindFromName(QString()), FailureKind::Unknown);
    QCOMPARE(AutomaticSyncPolicy::failureKindFromName(QStringLiteral("something_new")), FailureKind::Unknown);
}

// A Sync ends while the server still builds the PDF; when the build has ended
// the repository says so. The Sync that fetches the PDF does not wait.
void AutomaticSyncPolicyTest::testAnEndedBuildDoesNotWait()
{
    // The usual case: the Sync before it succeeded.
    {
        AutomaticSyncPolicy policy;
        policy.syncStarted(Initiation::User);
        policy.syncFinished(true, FailureKind::Unknown, 0);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("12/b/1")), Decision::Start);
        policy.automaticSyncStartedFor(QStringLiteral("12/b/1"));
        policy.syncStarted(Initiation::Automatic);
        policy.syncFinished(true, FailureKind::Unknown, poll);
        // Every open tab relays the announcement: the same state starts one Sync.
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("12/b/1")), Decision::Covered);
        // The build changed nothing this computer needs.
        QCOMPARE(policy.stateAnnounced(false, QStringLiteral("13/b/1")), Decision::NothingToDo);
        QCOMPARE(policy.stateAnnounced(true, QString()), Decision::NothingToDo);
    }
    // Automatic Syncs wait after failures; the ended build is fetched all the same.
    {
        AutomaticSyncPolicy policy;
        qint64 now = 0;
        for (int failure = 0; failure < 5; ++failure) {
            failAutomatic(policy, QStringLiteral("%1/a/1").arg(20 + 2 * failure), QStringLiteral("%1/a/1").arg(21 + 2 * failure), now);
            now = policy.notBeforeMs();
        }
        QCOMPARE(policy.notBeforeMs() - (now - 10 * minute), 10 * minute);
        const qint64 waiting = policy.notBeforeMs() - 9 * minute;
        QCOMPARE(ask(policy, true, QStringLiteral("40/a/1"), waiting), Decision::Waiting);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("40/b/1")), Decision::Start);
        // It is one Sync per ended build, not a way around the wait: when it fails
        // too, the state it left starts nothing and the wait goes on.
        failAutomatic(policy, QStringLiteral("40/b/1"), QStringLiteral("41/b/1"), waiting);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("41/b/1")), Decision::Covered);
        QCOMPARE(ask(policy, true, QStringLiteral("42/b/1"), waiting + minute), Decision::Waiting);
    }
    // An announcement is not the answer to the question asked after a failed Sync:
    // it is not taken for the state that Sync left.
    {
        AutomaticSyncPolicy policy;
        policy.automaticSyncStartedFor(QStringLiteral("50/a/1"));
        policy.syncStarted(Initiation::Automatic);
        policy.syncFinished(false, FailureKind::Bug, 1000);
        policy.stateRequested(2000);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("52/b/1")), Decision::Start);
        // The answer to the question is.
        QCOMPARE(policy.stateAnswered(true, QStringLiteral("51/a/1"), 2100), Decision::Covered);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("52/b/1")), Decision::Start);
        QCOMPARE(policy.stateAnnounced(true, QStringLiteral("51/a/1")), Decision::Covered);
    }
}

void AutomaticSyncPolicyTest::testAFailedUserSyncStartsNoAutomaticOne()
{
    AutomaticSyncPolicy policy;
    policy.syncStarted(Initiation::User);
    policy.syncFinished(false, FailureKind::Unknown, 0);
    // Not an automatic failure: nothing waits.
    QCOMPARE(policy.consecutiveFailures(), 0);
    QCOMPARE(policy.notBeforeMs(), 0);
    // But the state it left is its own.
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/1"), 1000), Decision::Covered);
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/1"), poll), Decision::Covered);
    QCOMPARE(ask(policy, true, QStringLiteral("13/a/1"), 2 * poll), Decision::Start);
}

void AutomaticSyncPolicyTest::testAnAnswerAskedBeforeTheEndIsNotTheStateAfter()
{
    AutomaticSyncPolicy policy;
    policy.automaticSyncStartedFor(QStringLiteral("11/a/1"));
    policy.syncStarted(Initiation::Automatic);
    // Asked while the Sync ran, answered after it ended.
    policy.stateRequested(900);
    policy.syncFinished(false, FailureKind::Unknown, 1000);
    QCOMPARE(policy.stateAnswered(true, QStringLiteral("11/a/1"), 1100), Decision::Waiting);
    // The question asked after the end names the state the Sync left.
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/2"), 2000), Decision::Covered);
    QCOMPARE(ask(policy, true, QStringLiteral("12/a/2"), 30 * minute), Decision::Covered);
}

// Every Sync failed after writing to the server, so every answer named a new
// state and every answer started a Sync: one every 20 seconds or so.
void AutomaticSyncPolicyTest::testFailingSyncsThatChangeTheStateStartFewSyncs()
{
    AutomaticSyncPolicy policy;
    int started = 0;
    int sequence = 100;
    // An hour of asking. The phone writes every five minutes; nothing else changes.
    for (qint64 now = 0; now < 60 * minute; now += poll) {
        if (now % (5 * minute) == 0) {
            ++sequence;
        }
        const QString key = QStringLiteral("%1/a/1").arg(sequence);
        if (ask(policy, true, key, now) != Decision::Start) {
            continue;
        }
        ++started;
        // The failing Sync writes too.
        failAutomatic(policy, key, QStringLiteral("%1/a/1").arg(++sequence), now);
    }
    // The first at once, then after 1, 2, 4, 8 and 10 minutes at the earliest and
    // only for a change from elsewhere: a handful, not 180.
    QVERIFY2(started >= 5 && started <= 8, qPrintable(QString::number(started)));
}

QTEST_GUILESS_MAIN(AutomaticSyncPolicyTest)
#include "automaticsyncpolicytest.moc"
