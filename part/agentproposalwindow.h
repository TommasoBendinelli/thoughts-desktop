/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALWINDOW_H
#define THOUGHTS_AGENTPROPOSALWINDOW_H

#include <QFrame>

#include "annotationmodel.h"

class QLabel;
class QCloseEvent;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

class AgentProposalWindow : public QFrame
{
    Q_OBJECT

public:
    explicit AgentProposalWindow(QWidget *parent);

    void setProposal(const AgentProposalSidebarEntry &proposal);
    void setOperationPending(bool pending, const QString &message = QString());
    void setLocationUnavailable(bool unavailable);
    void setPdfBuildRequired(bool required);
    void showResolutionError(const QString &message);
    void showResolutionSuccess(const QString &message);
    QString proposalId() const;

Q_SIGNALS:
    void closed();
    void resolutionRequested(const QString &proposalId, const QString &action);
    void openSourceRequested(const QString &sourcePath, int sourceLine);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void updateDiffFormatting();
    void updateActions();

    AgentProposalSidebarEntry m_proposal;
    QWidget *m_titleBar = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_closeButton = nullptr;
    QPlainTextEdit *m_motivation = nullptr;
    QPlainTextEdit *m_diff = nullptr;
    QWidget *m_locationIndicator = nullptr;
    QLabel *m_locationStatus = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_openSource = nullptr;
    QPushButton *m_openConversation = nullptr;
    QString m_conversationUrl;
    QPushButton *m_reject = nullptr;
    QPushButton *m_accept = nullptr;
    bool m_operationPending = false;
};

#endif
