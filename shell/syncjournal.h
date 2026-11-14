/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_SYNCJOURNAL_H
#define THOUGHTS_SYNCJOURNAL_H

#include "syncoperationstate.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QSqlDatabase;

namespace Thoughts
{

// A run is kept while it is younger than SyncJournalRetentionDays or among the
// newest SyncJournalRetentionMinimumOperations, whichever keeps more.
inline constexpr int SyncJournalRetentionDays = 30;
inline constexpr int SyncJournalRetentionMinimumOperations = 2000;

// How the journal's worker batches writes and paces its background work.
struct SyncJournalPolicy {
    // A burst is committed once no write arrived for this long ...
    int burstCoalesceMs = 150;
    // ... and at the latest this long after its first write,
    int burstMaximumMs = 1000;
    // or after this many writes.
    int burstMaximumWrites = 512;
    // Background retention starts this long after the journal opened,
    int maintenanceStartDelayMs = 30 * 1000;
    // runs again this long after it found nothing left to do,
    int maintenanceIntervalMs = 6 * 60 * 60 * 1000;
    // and pauses this long between two steps.
    int maintenanceStepPauseMs = 25;
    int retentionDays = SyncJournalRetentionDays;
    int retentionMinimumOperations = SyncJournalRetentionMinimumOperations;
    // One retention step removes at most this many events of one run.
    int pruneEventsPerStep = 2000;
    // One step of the one-time search for events without a run reads this many rows.
    int orphanRowsPerStep = 20000;
    // One step gives this many free pages back to the file system.
    int reclaimPagesPerStep = 256;
    // An existing journal with more events than this gets its new event
    // index during compact() instead of while it opens.
    qint64 deferredIndexEvents = 50000;
};


// The Sync journal (thoughts-server/sync-operations.sqlite): the runs the Logs
// window lists and the events of each.
//
// Every read and write runs on one worker thread with one connection, in the
// order it was asked for. Writes are queued and return at once; the worker
// stores a burst of them in one transaction. A read sees every write queued
// before it, committed or not, because it uses the same connection. Old runs
// are removed in the background, a few rows at a time, while nothing else is
// queued.
class SyncJournal : public QObject
{
    Q_OBJECT
public:
    static constexpr int RetentionDays = SyncJournalRetentionDays;
    static constexpr int RetentionMinimumOperations = SyncJournalRetentionMinimumOperations;
    // The Logs window loads this many runs at a time.
    static constexpr int LogsPageSize = 100;
    // How long the Logs window waits for its first page before it opens empty
    // and fills in when the page arrives.
    static constexpr int LogsOpenWaitMs = 250;

    using Policy = SyncJournalPolicy;

    // The journal of this process, in the application data folder.
    static SyncJournal *instance();
    static QString defaultPath();

    explicit SyncJournal(const QString &path, QObject *parent = nullptr, const Policy &policy = Policy());
    ~SyncJournal() override;

    QString path() const;

    // ---- Writes. Queued; none waits for the disk except beginOperation, which
    // waits for its row (not for the commit) because callers need to know that
    // the run exists.
    QString beginOperation(const QString &kind, const QString &target, const QString &phase, const QString &initiationControl, const QString &retryOf = {});
    // `warningCode` and `warning`: what a Sync that succeeded still has to say (its changes
    // arrived, a PDF could not be built). Kept with the run as warning_code and warning_message.
    void updateOperation(const QString &id, SyncOperationState state, const QString &phase, const QString &event, const QString &buildId = {}, const QString &errorCode = {},
                         const QString &warningCode = {}, const QString &warning = {});
    void appendEvent(const QString &id, const QJsonObject &event);
    void appendProgress(const QString &id, const QString &phase, qint64 transferredBytes, qint64 totalBytes, const QString &event);
    void updateTransferProgress(const QString &traceId, const QJsonObject &absoluteSnapshot);
    void reconcileInterrupted(qint64 startedBeforeMs);
    // Returns once everything queued so far is committed.
    void flush();
    // Stores what is queued, closes the file and ends the worker. Later writes
    // are dropped and later reads answer as an empty journal does.
    void shutDown();

    // ---- Reads that wait for their answer (single indexed rows).
    // The two the status area asks for on every refresh answer with their last
    // value instead of waiting while compact() runs.
    QJsonObject latestGlobalSummary();
    QDateTime lastSuccessfulGlobalSyncAt();
    QJsonObject latestDocumentSummary(const QString &target);
    QJsonObject operationSummary(const QString &operationId);
    QList<QJsonObject> runningSummaries();
    QString runningBuildOperationIdForTarget(const QString &target);
    // The newest 500 runs of the last 90 days, as older callers expect them.
    QJsonArray recentOperations(bool includeHistory);
    QJsonObject operationWithHistory(const QString &operationId);

    // ---- Reads for the Logs window. `done` runs on this object's thread, and
    // only while `context` exists.
    struct Page {
        // Summaries without events, newest created first.
        QList<QJsonObject> operations;
        // The runs asked for by name that the page does not hold.
        QList<QJsonObject> named;
        bool hasOlder = false;
        // Where the next page starts.
        qint64 oldestCreatedAtMs = 0;
        QString oldestId;
    };
    struct PageRequest {
        // Empty for the newest page; otherwise the previous page's end.
        qint64 beforeCreatedAtMs = 0;
        QString beforeId;
        int limit = LogsPageSize;
        // Runs to return as well when the page does not hold them.
        QStringList named;
    };
    // Waits up to `waitMs` for the page and then calls `done` before returning
    // (true); otherwise `done` is called later (false).
    bool fetchPage(QObject *context, const PageRequest &request, std::function<void(const Page &)> done, int waitMs = 0);

    struct Event {
        qint64 row = 0;
        // The order events were read in before this change: (time, row).
        qint64 orderMs = 0;
        QJsonObject json;
    };
    struct History {
        bool found = false;
        // The run without its events, not yet normalized.
        QJsonObject operation;
        // Its events after the requested row, in row order.
        QList<Event> events;
    };
    // `waitMs` as for fetchPage().
    bool fetchHistory(QObject *context, const QString &operationId, qint64 afterEventRow, std::function<void(const History &)> done, int waitMs = 0);
    // The run as operationWithHistory returns it, from a run row and all of its events.
    static QJsonObject operationFromHistory(const QJsonObject &operation, QList<Event> events);

    // ---- Maintenance.
    // Whether the journal still needs compact(): its file was created before
    // free pages could be returned to the file system, or an index is missing.
    // False until background retention has finished once.
    bool compactionPending() const;
    bool compactionRunning() const;
    // Rewrites the file once so that later retention shrinks it (auto_vacuum),
    // and adds missing indexes. This takes seconds on a large journal and
    // nothing else is answered meanwhile: ask for it only while no Sync runs
    // and nobody uses the app.
    void compact();

    struct Statistics {
        qint64 fileBytes = 0;
        qint64 runs = 0;
        qint64 events = 0;
        qint64 freePages = 0;
        qint64 pageSize = 0;
        int autoVacuum = 0;
        // Transactions committed and writes stored since the journal opened.
        qint64 commits = 0;
        qint64 writes = 0;
        bool errorIndex = false;
    };
    Statistics statistics();
    // Runs background retention to its end now (tests).
    void runMaintenanceNow();
    // The plan SQLite chose for a statement (tests).
    QStringList queryPlan(const QString &statement);

Q_SIGNALS:
    // compact() ended; the status area asks again.
    void compactionFinished();

private:
    enum class TaskKind { Read, Write, DurableWrite, Flush, Compact, Maintenance };
    using Task = std::function<void(QSqlDatabase &)>;
    bool enqueue(TaskKind kind, Task task);
    void runAndWait(TaskKind kind, Task task);
    template<typename T>
    T call(TaskKind kind, std::function<T(QSqlDatabase &)> query)
    {
        T result {};
        runAndWait(kind, [&result, &query](QSqlDatabase &database) { result = query(database); });
        return result;
    }
    template<typename T>
    bool fetch(QObject *context, std::function<T(QSqlDatabase &)> query, std::function<void(const T &)> done, int waitMs = 0)
    {
        struct State {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<T> result;
            bool deliverLater = false;
        };
        auto state = std::make_shared<State>();
        state->deliverLater = waitMs <= 0;
        const QPointer<QObject> guard(context);
        const bool queued = enqueue(TaskKind::Read, [this, state, guard, done, query = std::move(query)](QSqlDatabase &database) {
            T result = query(database);
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->deliverLater) {
                state->result = std::move(result);
                state->ready.notify_all();
                return;
            }
            lock.unlock();
            // Through this object, which outlives the worker: `context` may be
            // gone, and only its own thread may look.
            QMetaObject::invokeMethod(this, [guard, done, result = std::move(result)] {
                if (guard) {
                    done(result);
                }
            }, Qt::QueuedConnection);
        });
        if (waitMs <= 0 || !queued) {
            return false;
        }
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->ready.wait_for(lock, std::chrono::milliseconds(waitMs), [&state] { return state->result.has_value(); })) {
            state->deliverLater = true;
            return false;
        }
        const T result = std::move(*state->result);
        lock.unlock();
        done(result);
        return true;
    }

    class Worker;
    friend class Worker;
    std::unique_ptr<Worker> d;
    QJsonObject m_lastLatestGlobalSummary;
    QDateTime m_lastSuccessfulGlobalSyncAt;
};

// A transfer snapshot with every counter present and consistent, or false.
bool normalizedSyncTransferSnapshot(const QJsonObject &incoming, QJsonObject *normalized);

}

#endif
