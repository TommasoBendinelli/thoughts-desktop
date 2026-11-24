/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AUTOMATICSYNCPOLICY_H
#define THOUGHTS_AUTOMATICSYNCPOLICY_H

#include <QString>
#include <QtGlobal>

namespace Thoughts
{

// When Thoughts may start a Sync by itself.
//
// Thoughts asks the server now and then whether it has work for this computer
// and starts a Sync when it does. The server's answer carries a key that
// changes whenever any device writes, a failing Sync's own writes included, so
// a Sync that could not finish was started again every time the app asked.
//
// The rules:
//  - After an automatic Sync failed, the next automatic one waits one minute,
//    and twice as long after each further failure in a row, at most ten
//    minutes. A Sync that succeeds, a Sync the user starts and a change the
//    user makes on this computer end the wait.
//  - The server's state right after a failed Sync is that Sync's own: it starts
//    no automatic Sync. Only a later change does. Two exceptions: a failure the
//    repository calls transient, and a server that could not be reached right
//    after the failure. Then the same state is tried again once the wait is
//    over.
//  - A failure that needs the user (a merge, a credential, an update, documents
//    that need attention) is no reason to stop fetching what other devices
//    send: news from the server still starts a Sync, after the wait. What does
//    not start is the Sync that would send the user's own proposal decisions
//    (lastFailureNeedsUser): that waits for the Sync the user starts.
//  - A PDF build that a Sync left running on the server and that has now ended
//    is announced by the repository. The Sync that fetches the new PDF starts
//    at once, also while automatic Syncs wait: the user's own change must not
//    wait ten minutes for its PDF. Each ended build is announced once, so this
//    cannot become a loop.
//  - A Sync the user starts, or one a command asks for, is never held back.
//  - A change made on this computer starts an automatic Sync of its own once the
//    user has paused (localchangesync.h). It is held back as the one for
//    proposal decisions is.
class AutomaticSyncPolicy
{
public:
    static constexpr qint64 FirstRetryWaitMs = 60 * 1000;
    static constexpr qint64 MaximumRetryWaitMs = 10 * 60 * 1000;

    // Who started a Sync: the app by itself, the user in a window, or a
    // command (thoughtsctl, an agent).
    enum class Initiation { Automatic, User, Command };
    // Why a Sync failed, as far as it is known.
    enum class FailureKind {
        Unknown,
        // The same Sync may succeed later (no network, a timeout).
        Transient,
        // Nothing changes until the user acts (a source file needs a merge).
        NeedsUser,
        // A defect: trying again does not help.
        Bug,
    };
    enum class Decision {
        Start,
        // The server has no work for this computer.
        NothingToDo,
        // A Sync already ran against this state of the server.
        Covered,
        // Automatic Syncs wait after failures (notBeforeMs).
        Waiting,
    };

    // The kind the repository gives a failed Sync ("failure_kind" of the part's
    // fedoraManualSyncState): "transient", "needs_user" or "bug". Anything
    // else, and a failure the repository did not report, is unknown.
    static FailureKind failureKindFromName(const QString &name)
    {
        if (name == QLatin1String("transient")) {
            return FailureKind::Transient;
        }
        if (name == QLatin1String("needs_user")) {
            return FailureKind::NeedsUser;
        }
        if (name == QLatin1String("bug")) {
            return FailureKind::Bug;
        }
        return FailureKind::Unknown;
    }

    // The wait after `consecutiveFailures` failed automatic Syncs in a row.
    static qint64 waitAfter(int consecutiveFailures)
    {
        if (consecutiveFailures <= 0) {
            return 0;
        }
        qint64 wait = FirstRetryWaitMs;
        for (int failure = 1; failure < consecutiveFailures && wait < MaximumRetryWaitMs; ++failure) {
            wait *= 2;
        }
        return qMin(wait, MaximumRetryWaitMs);
    }

    void syncStarted(Initiation initiation)
    {
        m_running = initiation;
        m_checkAfterSyncPending = false;
        if (initiation == Initiation::User) {
            // The user takes over: nothing waits any more, and the Sync they
            // start is the answer to a failure that needed them.
            endWait();
            m_needsUser = false;
        }
    }

    void syncFinished(bool success, FailureKind kind, qint64 nowMs)
    {
        const Initiation initiation = m_running;
        m_running = Initiation::Command;
        m_finishedAtMs = nowMs;
        if (success) {
            endWait();
            m_needsUser = false;
            m_checkAfterSyncPending = false;
            return;
        }
        if (initiation == Initiation::Automatic) {
            ++m_consecutiveFailures;
            m_notBeforeMs = nowMs + waitAfter(m_consecutiveFailures);
        }
        m_needsUser = kind == FailureKind::NeedsUser;
        // What the server holds now is what the failed Sync left there. A
        // transient failure is the exception: the same state deserves another
        // try once the wait is over.
        m_checkAfterSyncPending = kind != FailureKind::Transient;
        if (kind == FailureKind::Transient) {
            m_lastStartedKey.clear();
        }
    }

    // The user changed something on this computer.
    void localChange()
    {
        endWait();
    }

    // The app asks the server for its state.
    void stateRequested(qint64 nowMs)
    {
        m_requestedAtMs = nowMs;
    }

    // The server's answer (`stateKey` is empty when it was not reached), and
    // whether an automatic Sync may start for it. Start must be followed by
    // automaticSyncStartedFor() when the Sync really starts.
    Decision stateAnswered(bool pending, const QString &stateKey, qint64 nowMs)
    {
        // The first answer to a question asked after a failed Sync ended.
        if (m_checkAfterSyncPending && m_requestedAtMs >= m_finishedAtMs) {
            m_checkAfterSyncPending = false;
            if (!stateKey.isEmpty()) {
                m_coveredKey = stateKey;
            }
        }
        const Decision decision = decide(pending, stateKey);
        if (decision == Decision::Start && nowMs < m_notBeforeMs) {
            return Decision::Waiting;
        }
        return decision;
    }

    // The repository says, without having been asked, that a PDF build a Sync
    // left running on the server has ended, and what the server holds now. The
    // Sync that fetches the result does not wait after failures; it starts
    // unless there is nothing to fetch or a Sync already ran against this state.
    // Start must be followed by automaticSyncStartedFor() as for an answer.
    Decision stateAnnounced(bool pending, const QString &stateKey) const
    {
        return decide(pending, stateKey);
    }

    void automaticSyncStartedFor(const QString &stateKey)
    {
        m_lastStartedKey = stateKey;
    }

    // Whether the last failure needs the user: the Sync that would send a
    // decision the user made does not start by itself then.
    bool lastFailureNeedsUser() const
    {
        return m_needsUser;
    }
    int consecutiveFailures() const
    {
        return m_consecutiveFailures;
    }
    // When automatic Syncs may start again; 0 when nothing holds them back.
    qint64 notBeforeMs() const
    {
        return m_notBeforeMs;
    }

private:
    Decision decide(bool pending, const QString &stateKey) const
    {
        if (!pending || stateKey.isEmpty()) {
            return Decision::NothingToDo;
        }
        if (stateKey == m_coveredKey) {
            return Decision::Covered;
        }
        // Without a failure in between, the same state never starts a second Sync.
        if (m_consecutiveFailures == 0 && stateKey == m_lastStartedKey) {
            return Decision::Covered;
        }
        return Decision::Start;
    }

    void endWait()
    {
        m_consecutiveFailures = 0;
        m_notBeforeMs = 0;
    }

    Initiation m_running = Initiation::Command;
    int m_consecutiveFailures = 0;
    qint64 m_notBeforeMs = 0;
    bool m_needsUser = false;
    QString m_lastStartedKey;
    QString m_coveredKey;
    bool m_checkAfterSyncPending = false;
    qint64 m_finishedAtMs = 0;
    qint64 m_requestedAtMs = 0;
};

}

#endif
