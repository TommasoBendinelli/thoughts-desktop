/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_LOCALCHANGESYNC_H
#define THOUGHTS_LOCALCHANGESYNC_H

#include <QString>
#include <QtGlobal>

namespace Thoughts
{

// When a change the user made on this computer is sent by a Sync that Thoughts
// starts by itself (initiation control "local_change").
//
// Likes, dislikes, comments, review answers, cards, to-dos and the other
// changes made on this computer wait in the outbox for the next Sync. Only news
// from the server, a proposal decision, a command or the user's own Sync used
// to start one, so 22 pieces of feedback once waited 80 minutes to be sent.
//
// The rules:
//  - A local change is an operation added to the outbox: the number waiting to
//    be sent went up, or the newest one waiting is newer than the newest before
//    (a Sync sent some while one was added). A Sync's own work takes
//    operations out (acknowledgements) and adds none for what it downloads.
//    Source files waiting to be published are not counted: the source tracker
//    counts files that a Sync wrote as well.
//  - The Sync starts quietMs (45 s) after the last change, but at most
//    maximumWaitMs (3 min) after the first change of a burst, so that editing
//    without a pause is sent too.
//  - Any Sync that starts sends what waits and ends the burst. A change made
//    while a Sync runs begins the next burst. Its Sync starts once the running
//    one has ended, afterSyncMs (5 s) later at the earliest, so that the outbox
//    the Sync left is known, and only while operations still wait to be sent.
//  - Changes seen only while Syncs ran, Sync after Sync, may be those Syncs'
//    own (a journal that every Sync queues again, say). After MaximumChain
//    such Syncs in a row, the next one waits for a change made between Syncs,
//    or for a Sync of another kind: no loop of Syncs that feed themselves.
//  - The rules of automatic Syncs hold (automaticsyncpolicy.h). No start while
//    automatic Syncs wait after failures: a local change ends that wait
//    (AutomaticSyncPolicy::localChange), so only a failure after the change
//    makes this one wait: that of the Sync it was made during, say. None while
//    the last failure needs the user, as for the Sync that sends proposal
//    decisions, and none without a server: the changes stay saved here and go
//    with the next Sync. While a Sync, a PDF build or the rewrite of the Sync
//    journal runs, the start is tried again busyRetryMs (15 s) later.
class LocalChangeSync
{
public:
    static constexpr int MaximumChain = 3;

    struct Delays {
        qint64 quietMs = 45 * 1000;
        qint64 maximumWaitMs = 3 * 60 * 1000;
        qint64 afterSyncMs = 5 * 1000;
        qint64 busyRetryMs = 15 * 1000;
    };

    // What else holds a start back when it is due.
    struct Conditions {
        // A Sync, a PDF build or the rewrite of the Sync journal runs.
        bool busy = false;
        // Automatic Syncs wait until then after failures (AutomaticSyncPolicy::notBeforeMs).
        qint64 notBeforeMs = 0;
        // AutomaticSyncPolicy::lastFailureNeedsUser.
        bool lastFailureNeedsUser = false;
        bool serverConfigured = true;
    };

    enum class Decision {
        Start,
        // Not yet: ask again at the time decide() gives.
        Later,
        // No change waits, or what changed has been sent meanwhile.
        NothingToSend,
        // The last failure needs the user, or there is no server: the changes
        // stay saved here and go with the next Sync. The burst is over.
        HeldBack,
        // Changes seen only while Syncs ran, after MaximumChain Syncs for such
        // changes in a row. The burst is over; they go with the next Sync.
        OnlyDuringSyncs,
    };

    LocalChangeSync() = default;
    explicit LocalChangeSync(const Delays &delays)
        : m_delays(delays)
    {
    }

    // The outbox, as the latest pending-changes snapshot shows it: the
    // operations waiting to be sent (-1: unknown) and the client time of the
    // newest. True when an operation was added; the first known state is none.
    bool outboxChanged(int operations, const QString &newestTimestamp, qint64 nowMs)
    {
        if (operations < 0) {
            return false;
        }
        const bool added = m_operations >= 0 && (operations > m_operations || (!newestTimestamp.isEmpty() && newestTimestamp > m_newestTimestamp));
        m_operations = operations;
        m_newestTimestamp = newestTimestamp;
        if (!added) {
            return false;
        }
        if (!m_running) {
            // Made between Syncs: by the user, not by a Sync.
            m_chain = 0;
        }
        if (!m_wanted) {
            m_wanted = true;
            m_firstChangeMs = nowMs;
            m_changes = 0;
            m_onlyDuringSyncs = true;
        }
        m_onlyDuringSyncs = m_onlyDuringSyncs && m_running;
        m_lastChangeMs = nowMs;
        ++m_changes;
        return true;
    }

    // Any Sync started: it sends what waits now.
    void syncStarted()
    {
        forget();
        m_running = true;
    }

    void syncEnded(qint64 nowMs)
    {
        m_running = false;
        m_syncEnded = true;
        m_endedAtMs = nowMs;
    }

    // When the wanted Sync is due by the changes alone; 0 when none is wanted.
    qint64 dueAtMs() const
    {
        if (!m_wanted) {
            return 0;
        }
        const qint64 due = qMin(m_lastChangeMs + m_delays.quietMs, m_firstChangeMs + m_delays.maximumWaitMs);
        return m_syncEnded ? qMax(due, m_endedAtMs + m_delays.afterSyncMs) : due;
    }

    // Whether the wanted Sync starts now. Later sets `nextCheckMs`: when to ask
    // again. Start must be followed by syncStarted() when the Sync really starts.
    Decision decide(qint64 nowMs, const Conditions &conditions, qint64 *nextCheckMs = nullptr)
    {
        if (!m_wanted) {
            return Decision::NothingToSend;
        }
        qint64 next = dueAtMs();
        if (nowMs >= next) {
            if (m_operations == 0) {
                forget();
                return Decision::NothingToSend;
            }
            if (conditions.lastFailureNeedsUser || !conditions.serverConfigured) {
                forget();
                return Decision::HeldBack;
            }
            if (m_onlyDuringSyncs && m_chain >= MaximumChain) {
                forget();
                return Decision::OnlyDuringSyncs;
            }
            if (m_running || conditions.busy) {
                next = nowMs + m_delays.busyRetryMs;
            } else if (conditions.notBeforeMs > nowMs) {
                next = conditions.notBeforeMs;
            }
        }
        if (nowMs < next) {
            if (nextCheckMs) {
                *nextCheckMs = next;
            }
            return Decision::Later;
        }
        m_chain = m_onlyDuringSyncs ? m_chain + 1 : 0;
        return Decision::Start;
    }

    // No change waits any more. What is known about the outbox stays.
    void forget()
    {
        m_wanted = false;
        m_changes = 0;
    }

    bool wanted() const
    {
        return m_wanted;
    }
    // Between syncStarted() and syncEnded().
    bool syncRunning() const
    {
        return m_running;
    }
    // The changes of the current burst.
    int changes() const
    {
        return m_changes;
    }
    // The operations waiting to be sent, as last known; -1: unknown.
    int operations() const
    {
        return m_operations;
    }
    qint64 firstChangeMs() const
    {
        return m_firstChangeMs;
    }
    qint64 lastChangeMs() const
    {
        return m_lastChangeMs;
    }
    // Whether every change of the burst was seen while a Sync ran.
    bool onlyDuringSyncs() const
    {
        return m_wanted && m_onlyDuringSyncs;
    }
    // Syncs started in a row for changes seen only while Syncs ran.
    int chain() const
    {
        return m_chain;
    }
    const Delays &delays() const
    {
        return m_delays;
    }

private:
    Delays m_delays;
    int m_operations = -1;
    QString m_newestTimestamp;
    bool m_wanted = false;
    int m_changes = 0;
    qint64 m_firstChangeMs = 0;
    qint64 m_lastChangeMs = 0;
    bool m_onlyDuringSyncs = false;
    int m_chain = 0;
    bool m_running = false;
    bool m_syncEnded = false;
    qint64 m_endedAtMs = 0;
};

}

#endif
