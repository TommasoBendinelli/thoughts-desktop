/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALDECISIONQUEUE_H
#define THOUGHTS_AGENTPROPOSALDECISIONQUEUE_H

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QThreadPool>

#include <memory>

#include "shell/agentproposalparser.h"

namespace Thoughts
{

// One Accept or Reject of proposals in a local source, as the user saw it.
struct LiveProposalDecisionRequest {
    QString docsRoot;
    QString sourcePath;
    // The source revision the proposal was shown from.
    QString expectedSourceSha256;
    QStringList proposalIds;
    // "accept" or "disregard".
    QString action;
    AgentProposalDecisionOptions options;
};

struct LiveProposalDecisionResult {
    quint64 ticket = 0;
    LiveProposalDecisionRequest request;
    bool ok = false;
    QString error;
    QString errorCode;
    // The source before and after the decision, for Undo.
    QByteArray previousContents;
    QString updatedSourceSha256;
    // The other files the decision wrote: the other half of a move pair.
    QList<AgentProposalSourceChange> otherSources;
    // Time spent waiting behind earlier decisions, and doing this one
    // (read, validate, claim, journal, write).
    qint64 waitMs = 0;
    qint64 workMs = 0;
};

// Writes local proposal decisions off the UI thread.
//
// Decisions run one at a time, in the order they were requested, through
// resolveLiveAgentProposedEdits(): the same validation, decision claims
// (protocol/agent-proposals.md §10.5) and compare-and-swap source write as a
// decision made on the UI thread. finished() is emitted on the queue's thread.
//
// A proposal shown before an earlier decision in the same file reached the
// view still names the older revision of the file. Such a request is applied
// to the current file only when every change since that revision was written
// by this queue; any other change still refuses it as "source changed".
//
// The destructor waits for decisions in flight, so a decision the user made
// is written before its window goes away.
class LiveProposalDecisionQueue : public QObject
{
    Q_OBJECT

public:
    explicit LiveProposalDecisionQueue(QObject *parent = nullptr);
    ~LiveProposalDecisionQueue() override;

    quint64 enqueue(const LiveProposalDecisionRequest &request);
    // Requested and not yet reported through finished().
    int pending() const;
    // Waits until every requested decision has been written or refused; the
    // finished() signals are still delivered through the event loop.
    bool waitForWritten(int timeoutMs = -1);

Q_SIGNALS:
    void finished(const Thoughts::LiveProposalDecisionResult &result);

private:
    struct Shared;
    std::shared_ptr<Shared> m_shared;
    QThreadPool m_pool;
    quint64 m_nextTicket = 0;
    int m_pending = 0;
};

}

Q_DECLARE_METATYPE(Thoughts::LiveProposalDecisionResult)

#endif
