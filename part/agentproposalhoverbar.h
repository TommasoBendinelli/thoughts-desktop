/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALHOVERBAR_H
#define THOUGHTS_AGENTPROPOSALHOVERBAR_H

#include <QFrame>
#include <QString>

#include "okularpart_export.h"

class QToolButton;

// Whether a proposal can be accepted or rejected right now, and if not, why.
struct AgentProposalDecisionState {
    bool decidable = false;
    // Shown in the disabled buttons' tooltips.
    QString reason;
    // The proposal removes text: Accept is an "Accept removal".
    bool removal = false;
};

// The small toolbar that appears next to a proposal's pencil on the page:
// Open (the full proposal window), Accept, Reject.
class OKULARPART_EXPORT AgentProposalHoverBar : public QFrame
{
    Q_OBJECT

public:
    explicit AgentProposalHoverBar(QWidget *parent);

    void setProposal(const QString &proposalId, const AgentProposalDecisionState &state);
    QString proposalId() const;
    bool pointerInside() const;

Q_SIGNALS:
    void openRequested(const QString &proposalId);
    void resolutionRequested(const QString &proposalId, const QString &action);
    void pointerEntered();
    void pointerLeft();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // Clicks on the frame or a disabled button stop here instead of reaching the page.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QString m_proposalId;
    bool m_pointerInside = false;
    QToolButton *m_open = nullptr;
    QToolButton *m_accept = nullptr;
    QToolButton *m_reject = nullptr;
};

#endif
