/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "agentproposalhoverbar.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QToolButton>

#include <KLocalizedString>

#include <functional>

namespace
{
constexpr int IconSize = 16;

// Icons drawn here, in the colours of the proposal highlight, so they look the
// same with every icon theme.
QIcon paintedIcon(const std::function<void(QPainter *)> &draw)
{
    QIcon icon;
    for (const qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(IconSize, IconSize) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        draw(&painter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon pencilIcon(const QColor &color)
{
    return paintedIcon([color](QPainter *painter) {
        // The pencil drawn on the page marker (20 px box), scaled to 16 px.
        painter->scale(IconSize / 20.0, IconSize / 20.0);
        painter->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath pencil;
        pencil.moveTo(4, 16);
        pencil.lineTo(5, 11.5);
        pencil.lineTo(12.5, 4);
        pencil.lineTo(16, 7.5);
        pencil.lineTo(8.5, 15);
        pencil.closeSubpath();
        painter->drawPath(pencil);
        painter->drawLine(QPointF(10.5, 6), QPointF(14, 9.5));
    });
}

QIcon checkIcon()
{
    return paintedIcon([](QPainter *painter) {
        painter->setPen(QPen(QColor(35, 155, 75), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath check;
        check.moveTo(3, 8.5);
        check.lineTo(6.5, 12);
        check.lineTo(13, 4.5);
        painter->drawPath(check);
    });
}

QIcon crossIcon()
{
    return paintedIcon([](QPainter *painter) {
        painter->setPen(QPen(QColor(205, 35, 35), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawLine(QPointF(4, 4), QPointF(12, 12));
        painter->drawLine(QPointF(12, 4), QPointF(4, 12));
    });
}

QToolButton *makeButton(QWidget *parent, const QString &objectName, const QIcon &icon, const QString &text)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(icon);
    button->setIconSize(QSize(IconSize, IconSize));
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    // Comfortable to hit: at least 28 px high and 30 px wide.
    button->setMinimumSize(30, 28);
    return button;
}
}

AgentProposalHoverBar::AgentProposalHoverBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("AgentProposalHoverBar"));
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(3, 2, 3, 2);
    layout->setSpacing(2);
    m_open = makeButton(this, QStringLiteral("agentProposalHoverOpen"), pencilIcon(palette().color(QPalette::Text)), i18n("Open"));
    m_open->setToolTip(i18n("Open agent edit"));
    m_accept = makeButton(this, QStringLiteral("agentProposalHoverAccept"), checkIcon(), i18n("Accept"));
    m_reject = makeButton(this, QStringLiteral("agentProposalHoverReject"), crossIcon(), i18n("Reject"));
    layout->addWidget(m_open);
    layout->addWidget(m_accept);
    layout->addWidget(m_reject);
    connect(m_open, &QToolButton::clicked, this, [this] { Q_EMIT openRequested(m_proposalId); });
    connect(m_accept, &QToolButton::clicked, this, [this] { Q_EMIT resolutionRequested(m_proposalId, QStringLiteral("accept")); });
    connect(m_reject, &QToolButton::clicked, this, [this] { Q_EMIT resolutionRequested(m_proposalId, QStringLiteral("disregard")); });
    hide();
}

void AgentProposalHoverBar::setProposal(const QString &proposalId, const AgentProposalDecisionState &state)
{
    m_proposalId = proposalId;
    const QString acceptTip = state.removal ? i18n("Accept removal") : i18n("Accept edit");
    const QString rejectTip = i18n("Reject proposal");
    m_accept->setEnabled(state.decidable);
    m_reject->setEnabled(state.decidable);
    if (state.decidable) {
        m_accept->setToolTip(acceptTip);
        m_reject->setToolTip(rejectTip);
        setToolTip(QString());
    } else {
        const QString reason = state.reason.isEmpty() ? i18n("This proposal cannot be decided here. Open it to see why.") : state.reason;
        m_accept->setToolTip(i18nc("@info:tooltip disabled action, then why", "%1 (unavailable)\n%2", acceptTip, reason));
        m_reject->setToolTip(i18nc("@info:tooltip disabled action, then why", "%1 (unavailable)\n%2", rejectTip, reason));
        setToolTip(reason);
    }
    adjustSize();
}

QString AgentProposalHoverBar::proposalId() const
{
    return m_proposalId;
}

bool AgentProposalHoverBar::pointerInside() const
{
    return m_pointerInside && isVisible();
}

void AgentProposalHoverBar::paintEvent(QPaintEvent *)
{
    // The pencil marker's look: base colour, highlight outline, rounded corners.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor shadow(0, 0, 0, 40);
    painter.setPen(Qt::NoPen);
    painter.setBrush(shadow);
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.5, 2.0, -0.5, 0), 5, 5);
    painter.setPen(QPen(palette().color(QPalette::Highlight), 1.0));
    painter.setBrush(palette().color(QPalette::Base));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -1.5, -1.5), 5, 5);
}

void AgentProposalHoverBar::enterEvent(QEnterEvent *event)
{
    QFrame::enterEvent(event);
    m_pointerInside = true;
    Q_EMIT pointerEntered();
}

void AgentProposalHoverBar::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    m_pointerInside = false;
    Q_EMIT pointerLeft();
}

void AgentProposalHoverBar::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void AgentProposalHoverBar::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void AgentProposalHoverBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
}
