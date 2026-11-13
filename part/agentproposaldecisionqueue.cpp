/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "agentproposaldecisionqueue.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>

namespace Thoughts
{

struct LiveProposalDecisionQueue::Shared {
    QMutex mutex;
    // "<canonical source>\n<sha before>" -> sha after, for decisions this
    // queue wrote.
    QHash<QString, QString> successors;
};

namespace
{
QString canonicalSource(const LiveProposalDecisionRequest &request)
{
    const QFileInfo requested(request.sourcePath);
    const QFileInfo source(requested.isAbsolute() ? requested.absoluteFilePath() : QDir(request.docsRoot).absoluteFilePath(request.sourcePath));
    const QString canonical = source.canonicalFilePath();
    return canonical.isEmpty() ? source.absoluteFilePath() : canonical;
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

// Bounded: one entry per decision of this session.
constexpr int MaximumSuccessors = 4096;
}

LiveProposalDecisionQueue::LiveProposalDecisionQueue(QObject *parent)
    : QObject(parent)
    , m_shared(std::make_shared<Shared>())
{
    qRegisterMetaType<Thoughts::LiveProposalDecisionResult>();
    // Serial: decisions of one file must apply in the order they were made.
    m_pool.setMaxThreadCount(1);
    m_pool.setObjectName(QStringLiteral("LiveProposalDecisionQueue"));
}

LiveProposalDecisionQueue::~LiveProposalDecisionQueue()
{
    m_pool.waitForDone();
}

quint64 LiveProposalDecisionQueue::enqueue(const LiveProposalDecisionRequest &request)
{
    const quint64 ticket = ++m_nextTicket;
    ++m_pending;
    QElapsedTimer queued;
    queued.start();
    const std::shared_ptr<Shared> shared = m_shared;
    const QPointer<LiveProposalDecisionQueue> queue(this);
    m_pool.start([ticket, request, queued, shared, queue] {
        LiveProposalDecisionResult result;
        result.ticket = ticket;
        result.request = request;
        result.waitMs = queued.elapsed();
        QElapsedTimer work;
        work.start();

        const QString source = canonicalSource(request);
        QString expected = request.expectedSourceSha256;
        const QString current = fileSha256(source);
        if (!current.isEmpty() && expected != current) {
            QMutexLocker locker(&shared->mutex);
            QSet<QString> visited;
            while (expected != current && !visited.contains(expected)) {
                visited.insert(expected);
                const auto next = shared->successors.constFind(source + QLatin1Char('\n') + expected);
                if (next == shared->successors.constEnd()) {
                    break;
                }
                expected = next.value();
            }
            // Only a complete chain of this queue's own writes stands in for
            // the revision the user saw.
            if (expected != current) {
                expected = request.expectedSourceSha256;
            }
        }

        result.ok = resolveLiveAgentProposedEdits(request.docsRoot,
                                                  request.sourcePath,
                                                  expected,
                                                  request.proposalIds,
                                                  request.action,
                                                  &result.error,
                                                  &result.previousContents,
                                                  &result.updatedSourceSha256,
                                                  &result.errorCode,
                                                  request.options,
                                                  &result.otherSources);
        if (result.ok) {
            QMutexLocker locker(&shared->mutex);
            if (shared->successors.size() >= MaximumSuccessors) {
                shared->successors.clear();
            }
            shared->successors.insert(source + QLatin1Char('\n') + expected, result.updatedSourceSha256);
        }
        result.workMs = work.elapsed();

        // The queue waits for this task in its destructor, so it is alive here.
        QMetaObject::invokeMethod(
            queue.data(),
            [queue, result] {
                if (!queue) {
                    return;
                }
                --queue->m_pending;
                Q_EMIT queue->finished(result);
            },
            Qt::QueuedConnection);
    });
    return ticket;
}

int LiveProposalDecisionQueue::pending() const
{
    return m_pending;
}

bool LiveProposalDecisionQueue::waitForWritten(int timeoutMs)
{
    return m_pool.waitForDone(timeoutMs);
}

}
