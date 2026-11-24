/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALWINDOW_H
#define THOUGHTS_AGENTPROPOSALWINDOW_H

#include <QFrame>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <optional>

#include "annotationmodel.h"
#include "okularpart_export.h"

class QLabel;
class QCloseEvent;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

class OKULARPART_EXPORT AgentProposalWindow : public QFrame
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

    // The window goes beside @p passage (global coordinates) without covering it: right of
    // @p page if there is room, else left of it, else beside the passage itself, else below or
    // above it; always on @p screen (QScreen::availableGeometry). Not once the user moved it
    // since it was last placed: it stays where they put it.
    void placeBeside(const QRect &passage, const QRect &page, const QRect &screen);
    // With no passage: the top right corner of @p view (the reader's page view), on @p screen.
    void placeInCorner(const QRect &view, const QRect &screen);
    // The user moved the window since Thoughts last placed it.
    bool movedByUser() const;
    // Where a window of frame size @p frame goes for placeBeside.
    static QPoint besidePosition(const QRect &passage, const QRect &page, const QSize &frame, const QRect &screen);
    // Its size when nothing was remembered, and the smallest it gets.
    static QSize defaultSize();

Q_SIGNALS:
    void closed();
    void resolutionRequested(const QString &proposalId, const QString &action);
    // ⌘Z and ⇧⌘Z while this window is in front: the reader's proposal decisions.
    void undoRequested();
    void redoRequested();
    void openSourceRequested(const QString &sourcePath, int sourceLine);

protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateDiffFormatting();
    void updateActions();
    // Moves the window's frame to @p position, remembering it as placed.
    void placeAt(const QPoint &position);
    QSize placedFrameSize() const;

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
    // Where Thoughts last put the window's frame; a window moved away from it was moved by the user.
    std::optional<QPoint> m_placedAt;
};

#endif
