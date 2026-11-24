/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/automaticsyncpolicy.h"
#include "../shell/localchangesync.h"

#include <QTest>

using Thoughts::AutomaticSyncPolicy;
using Thoughts::LocalChangeSync;
using Decision = LocalChangeSync::Decision;
using Conditions = LocalChangeSync::Conditions;

namespace
{
constexpr qint64 second = 1000;
constexpr qint64 minute = 60 * second;

QString at(int seconds)
{
    return QStringLiteral("2026-01-01T10:%1:%2.000Z").arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// A schedule that knows an outbox of `operations`, the newest written at 10:00:00.
LocalChangeSync knowing(int operations)
{
    LocalChangeSync schedule;
    schedule.outboxChanged(operations, operations > 0 ? at(0) : QString(), 0);
    return schedule;
}

Decision decideAt(LocalChangeSync &schedule, qint64 nowMs, qint64 *next = nullptr, const Conditions &conditions = {})
{
    return schedule.decide(nowMs, conditions, next);
}
}

class LocalChangeSyncTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testDelays();
    void testStartsAQuietTimeAfterTheLastChange();
    void testEditingWithoutPauseIsSentWithinTheMaximumWait();
    void testOnlyAnAddedOperationIsAChange();
    void testASyncThatStartsSendsTheBurst();
    void testAChangeWhileASyncRunsStartsOneAfterIt();
    void testNothingStartsWhenTheSyncSentTheChange();
    void testWhatRunsDelaysTheStart();
    void testTheFailureWaitHolds();
    void testAFailureThatNeedsTheUserHoldsTheChangesBack();
    void testNoServerHoldsTheChangesBack();
    void testTogetherWithTheAutomaticSyncPolicy();
    void testChangesSeenOnlyDuringSyncsStartFewSyncs();
};

void LocalChangeSyncTest::testDelays()
{
    const LocalChangeSync schedule;
    QCOMPARE(schedule.delays().quietMs, 45 * second);
    QCOMPARE(schedule.delays().maximumWaitMs, 3 * minute);
    QCOMPARE(schedule.delays().afterSyncMs, 5 * second);
    QCOMPARE(schedule.delays().busyRetryMs, 15 * second);
    QCOMPARE(schedule.operations(), -1);
    QVERIFY(!schedule.wanted());
    QCOMPARE(schedule.dueAtMs(), 0);
}

void LocalChangeSyncTest::testStartsAQuietTimeAfterTheLastChange()
{
    LocalChangeSync schedule = knowing(2);
    QVERIFY(schedule.outboxChanged(3, at(10), 10 * second));
    QVERIFY(schedule.wanted());
    QCOMPARE(schedule.dueAtMs(), 55 * second);
    qint64 next = 0;
    QCOMPARE(decideAt(schedule, 30 * second, &next), Decision::Later);
    QCOMPARE(next, 55 * second);
    // Two more changes: the quiet time counts from the last.
    QVERIFY(schedule.outboxChanged(4, at(30), 30 * second));
    QVERIFY(schedule.outboxChanged(5, at(50), 50 * second));
    QCOMPARE(schedule.changes(), 3);
    QCOMPARE(schedule.firstChangeMs(), 10 * second);
    QCOMPARE(schedule.lastChangeMs(), 50 * second);
    QCOMPARE(decideAt(schedule, 55 * second, &next), Decision::Later);
    QCOMPARE(next, 95 * second);
    QCOMPARE(decideAt(schedule, 95 * second - 1, &next), Decision::Later);
    QCOMPARE(decideAt(schedule, 95 * second), Decision::Start);
    // Start leaves the burst to the Sync that starts.
    QVERIFY(schedule.wanted());
    schedule.syncStarted();
    QVERIFY(!schedule.wanted());
    QCOMPARE(decideAt(schedule, 200 * second), Decision::NothingToSend);
}

void LocalChangeSyncTest::testEditingWithoutPauseIsSentWithinTheMaximumWait()
{
    LocalChangeSync schedule = knowing(0);
    // A change every 30 seconds, never 45 seconds of quiet.
    int operations = 0;
    for (qint64 now = 0; now < 3 * minute; now += 30 * second) {
        QVERIFY(schedule.outboxChanged(++operations, at(int(now / second)), now));
        QCOMPARE(decideAt(schedule, now), Decision::Later);
    }
    QCOMPARE(schedule.dueAtMs(), 3 * minute);
    QCOMPARE(decideAt(schedule, 3 * minute - 1), Decision::Later);
    QCOMPARE(decideAt(schedule, 3 * minute), Decision::Start);
    // The next burst has its own three minutes.
    schedule.syncStarted();
    schedule.syncEnded(3 * minute + 20 * second);
    QVERIFY(schedule.outboxChanged(1, at(210), 3 * minute + 30 * second));
    QCOMPARE(schedule.dueAtMs(), 3 * minute + 75 * second);
}

void LocalChangeSyncTest::testOnlyAnAddedOperationIsAChange()
{
    LocalChangeSync schedule;
    // The first state known is no change, nor is an unknown one.
    QVERIFY(!schedule.outboxChanged(-1, QString(), 0));
    QVERIFY(!schedule.outboxChanged(4, at(3), 0));
    QVERIFY(!schedule.outboxChanged(-1, QString(), second));
    QCOMPARE(schedule.operations(), 4);
    // The same outbox again, as another tab or window relays it.
    QVERIFY(!schedule.outboxChanged(4, at(3), 2 * second));
    // A Sync sent two: fewer wait, and the newest of those left is older.
    QVERIFY(!schedule.outboxChanged(2, at(1), 3 * second));
    QVERIFY(!schedule.outboxChanged(0, QString(), 4 * second));
    QVERIFY(!schedule.wanted());
    // One was added.
    QVERIFY(schedule.outboxChanged(1, at(5), 5 * second));
    QVERIFY(schedule.wanted());
    // A Sync sent that one while two were added: fewer than before, but newer.
    schedule.syncStarted();
    QVERIFY(schedule.outboxChanged(3, at(7), 7 * second));
    QVERIFY(schedule.outboxChanged(2, at(8), 8 * second));
    QCOMPARE(schedule.changes(), 2);
    // Sent, the newest of those left is older than the newest before.
    QVERIFY(!schedule.outboxChanged(1, at(7), 9 * second));
    QCOMPARE(schedule.changes(), 2);
}

void LocalChangeSyncTest::testASyncThatStartsSendsTheBurst()
{
    // The user's own Sync, a proposal decision's or one from the server.
    LocalChangeSync schedule = knowing(1);
    QVERIFY(schedule.outboxChanged(2, at(1), second));
    schedule.syncStarted();
    QVERIFY(!schedule.wanted());
    schedule.syncEnded(10 * second);
    QCOMPARE(decideAt(schedule, 5 * minute), Decision::NothingToSend);
    // That Sync sent what waited.
    QVERIFY(!schedule.outboxChanged(0, QString(), 10 * second));
    QCOMPARE(decideAt(schedule, 5 * minute), Decision::NothingToSend);
}

void LocalChangeSyncTest::testAChangeWhileASyncRunsStartsOneAfterIt()
{
    LocalChangeSync schedule = knowing(3);
    schedule.syncStarted();
    QVERIFY(schedule.outboxChanged(4, at(10), 10 * second));
    qint64 next = 0;
    // Due while the Sync still runs: later.
    QCOMPARE(decideAt(schedule, 55 * second, &next), Decision::Later);
    QCOMPARE(next, 70 * second);
    QCOMPARE(decideAt(schedule, 70 * second, &next), Decision::Later);
    QCOMPARE(next, 85 * second);
    // The Sync ends: it sent the three before it, the one added meanwhile waits.
    schedule.syncEnded(100 * second);
    QVERIFY(!schedule.outboxChanged(1, at(10), 100 * second + 200));
    QCOMPARE(schedule.dueAtMs(), 105 * second);
    QCOMPARE(decideAt(schedule, 104 * second, &next), Decision::Later);
    QCOMPARE(next, 105 * second);
    QCOMPARE(decideAt(schedule, 105 * second), Decision::Start);
    schedule.syncStarted();
    schedule.syncEnded(120 * second);
    QVERIFY(!schedule.outboxChanged(0, QString(), 120 * second));
    // One Sync after the other, and no further one.
    QCOMPARE(decideAt(schedule, 10 * minute), Decision::NothingToSend);
}

void LocalChangeSyncTest::testNothingStartsWhenTheSyncSentTheChange()
{
    // The change was added just before a Sync took the outbox, but its
    // snapshot came after the start: the Sync sent it.
    LocalChangeSync schedule = knowing(0);
    schedule.syncStarted();
    QVERIFY(schedule.outboxChanged(1, at(1), second));
    schedule.syncEnded(20 * second);
    QVERIFY(!schedule.outboxChanged(0, QString(), 20 * second));
    QVERIFY(schedule.wanted());
    QCOMPARE(decideAt(schedule, 46 * second), Decision::NothingToSend);
    QVERIFY(!schedule.wanted());
}

void LocalChangeSyncTest::testWhatRunsDelaysTheStart()
{
    LocalChangeSync schedule = knowing(0);
    QVERIFY(schedule.outboxChanged(1, at(0), 0));
    qint64 next = 0;
    // A PDF build, a Sync the shell knows of, the rewrite of the journal.
    QCOMPARE(decideAt(schedule, 45 * second, &next, Conditions {true, 0, false, true}), Decision::Later);
    QCOMPARE(next, 60 * second);
    QCOMPARE(decideAt(schedule, 60 * second, &next, Conditions {true, 0, false, true}), Decision::Later);
    QCOMPARE(next, 75 * second);
    QCOMPARE(decideAt(schedule, 75 * second, &next), Decision::Start);
}

void LocalChangeSyncTest::testTheFailureWaitHolds()
{
    LocalChangeSync schedule = knowing(0);
    QVERIFY(schedule.outboxChanged(1, at(0), 0));
    qint64 next = 0;
    // An automatic Sync failed after the change: this one waits as long.
    const Conditions waiting {false, 2 * minute, false, true};
    QCOMPARE(decideAt(schedule, 45 * second, &next, waiting), Decision::Later);
    QCOMPARE(next, 2 * minute);
    QCOMPARE(decideAt(schedule, 2 * minute, &next, waiting), Decision::Start);
    // A wait that ends before the quiet time changes nothing.
    schedule.syncStarted();
    schedule.syncEnded(3 * minute);
    QVERIFY(schedule.outboxChanged(2, at(240), 4 * minute));
    QCOMPARE(decideAt(schedule, 4 * minute + 44 * second, &next, Conditions {false, 4 * minute + 10 * second, false, true}), Decision::Later);
    QCOMPARE(next, 4 * minute + 45 * second);
    QCOMPARE(decideAt(schedule, 4 * minute + 45 * second, &next, Conditions {false, 4 * minute + 10 * second, false, true}), Decision::Start);
}

void LocalChangeSyncTest::testAFailureThatNeedsTheUserHoldsTheChangesBack()
{
    LocalChangeSync schedule = knowing(0);
    QVERIFY(schedule.outboxChanged(1, at(0), 0));
    const Conditions needsUser {false, 0, true, true};
    // Not before it is due ...
    qint64 next = 0;
    QCOMPARE(decideAt(schedule, 10 * second, &next, needsUser), Decision::Later);
    QCOMPARE(next, 45 * second);
    // ... and then the burst is over: the user's Sync sends it.
    QCOMPARE(decideAt(schedule, 45 * second, &next, needsUser), Decision::HeldBack);
    QVERIFY(!schedule.wanted());
    QCOMPARE(decideAt(schedule, 10 * minute), Decision::NothingToSend);
    // A new change once the failure is over starts a Sync again.
    QVERIFY(schedule.outboxChanged(2, at(700), 11 * minute));
    QCOMPARE(decideAt(schedule, 11 * minute + 45 * second), Decision::Start);
}

void LocalChangeSyncTest::testNoServerHoldsTheChangesBack()
{
    LocalChangeSync schedule = knowing(0);
    QVERIFY(schedule.outboxChanged(1, at(0), 0));
    QCOMPARE(decideAt(schedule, 45 * second, nullptr, Conditions {false, 0, false, false}), Decision::HeldBack);
    QVERIFY(!schedule.wanted());
}

// What the shell does with both: a change made while no Sync runs ends the
// wait after failures; a change made during a Sync that fails does not.
void LocalChangeSyncTest::testTogetherWithTheAutomaticSyncPolicy()
{
    AutomaticSyncPolicy policy;
    LocalChangeSync schedule = knowing(0);
    const auto conditions = [&policy] {
        return Conditions {false, policy.notBeforeMs(), policy.lastFailureNeedsUser(), true};
    };
    const auto startLocalChangeSync = [&] {
        policy.syncStarted(AutomaticSyncPolicy::Initiation::Automatic);
        schedule.syncStarted();
    };

    // An automatic Sync fails: automatic Syncs wait a minute.
    policy.syncStarted(AutomaticSyncPolicy::Initiation::Automatic);
    schedule.syncStarted();
    policy.syncFinished(false, AutomaticSyncPolicy::FailureKind::Unknown, 0);
    schedule.syncEnded(0);
    QCOMPARE(policy.notBeforeMs(), minute);
    // The user likes a passage: the wait is over, the Sync starts after the quiet time.
    QVERIFY(schedule.outboxChanged(1, at(10), 10 * second));
    policy.localChange();
    QCOMPARE(schedule.decide(55 * second, conditions()), Decision::Start);
    startLocalChangeSync();

    // The user adds a comment while it runs, and it fails again.
    QVERIFY(schedule.outboxChanged(2, at(60), minute));
    policy.syncFinished(false, AutomaticSyncPolicy::FailureKind::Unknown, 70 * second);
    schedule.syncEnded(70 * second);
    QCOMPARE(policy.notBeforeMs(), 70 * second + minute);
    qint64 next = 0;
    QCOMPARE(schedule.decide(105 * second, conditions(), &next), Decision::Later);
    QCOMPARE(next, 130 * second);
    QCOMPARE(schedule.decide(130 * second, conditions()), Decision::Start);
    startLocalChangeSync();
    // Once more failed: nothing new, nothing starts. The next change does.
    policy.syncFinished(false, AutomaticSyncPolicy::FailureKind::Unknown, 140 * second);
    schedule.syncEnded(140 * second);
    QVERIFY(!schedule.outboxChanged(2, at(60), 140 * second));
    QCOMPARE(schedule.decide(10 * minute, conditions()), Decision::NothingToSend);
    QCOMPARE(policy.consecutiveFailures(), 2);
}

// Every Sync queues an operation again that it does not send (a defect
// elsewhere): a few Syncs follow, then none until a change made between Syncs.
void LocalChangeSyncTest::testChangesSeenOnlyDuringSyncsStartFewSyncs()
{
    LocalChangeSync schedule = knowing(1);
    qint64 now = 0;
    int operations = 1;
    int started = 0;
    const auto syncThatQueuesOneAgain = [&] {
        schedule.syncStarted();
        now += 2 * second;
        QVERIFY(schedule.outboxChanged(++operations, at(0), now));
        QVERIFY(schedule.onlyDuringSyncs());
        now += 2 * second;
        schedule.syncEnded(now);
    };
    syncThatQueuesOneAgain();
    for (;;) {
        qint64 next = 0;
        now = schedule.dueAtMs();
        const Decision decision = decideAt(schedule, now, &next);
        if (decision != Decision::Start) {
            QCOMPARE(decision, Decision::OnlyDuringSyncs);
            break;
        }
        ++started;
        QCOMPARE(schedule.chain(), started);
        syncThatQueuesOneAgain();
        QVERIFY(started <= LocalChangeSync::MaximumChain);
    }
    QCOMPARE(started, LocalChangeSync::MaximumChain);
    QVERIFY(!schedule.wanted());
    // A change made between Syncs is the user's: its Sync starts.
    QVERIFY(schedule.outboxChanged(++operations, at(1), now + second));
    QVERIFY(!schedule.onlyDuringSyncs());
    QCOMPARE(schedule.chain(), 0);
    QCOMPARE(decideAt(schedule, schedule.dueAtMs()), Decision::Start);
    QCOMPARE(schedule.chain(), 0);
    // And a change while that one runs is sent after it again.
    schedule.syncStarted();
    QVERIFY(schedule.outboxChanged(++operations, at(2), now + 2 * second));
    schedule.syncEnded(now + 3 * second);
    QCOMPARE(decideAt(schedule, schedule.dueAtMs()), Decision::Start);
    QCOMPARE(schedule.chain(), 1);
}

QTEST_GUILESS_MAIN(LocalChangeSyncTest)

#include "localchangesynctest.moc"
