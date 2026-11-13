// SPDX-License-Identifier: GPL-2.0-or-later
#include "readaloudbar.h"

#include <KBusyIndicatorWidget>
#include <KLocalizedString>

#include <QAbstractScrollArea>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QToolButton>

ReadAloudBar::ReadAloudBar(Thoughts::ReadAloud::Controller *controller, QWidget *host)
    : QFrame(host)
    , m_controller(controller)
    , m_host(host)
{
    setObjectName(QStringLiteral("readAloudBar"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(true);
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral("#readAloudBar { background: palette(window); border: 1px solid palette(mid); border-radius: 8px; }"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->setSpacing(6);

    m_spinner = new KBusyIndicatorWidget(this);
    m_spinner->setFixedSize(16, 16);
    layout->addWidget(m_spinner);

    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("readAloudStatus"));
    layout->addWidget(m_label);

    const auto makeButton = [this, layout](const QString &name, const QString &icon) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(QIcon::fromTheme(icon));
        layout->addWidget(button);
        return button;
    };
    m_pause = makeButton(QStringLiteral("readAloudPause"), QStringLiteral("media-playback-pause"));
    m_stop = makeButton(QStringLiteral("readAloudStop"), QStringLiteral("media-playback-stop"));
    m_stop->setText(i18nc("@action:button stop reading aloud", "Stop"));
    m_stop->setToolTip(i18n("Stop reading aloud (Esc)"));

    connect(m_pause, &QToolButton::clicked, this, [this] {
        if (m_controller) {
            m_controller->togglePause();
        }
    });
    connect(m_stop, &QToolButton::clicked, this, [this] {
        if (m_controller) {
            m_controller->stop();
        }
    });
    connect(controller, &Thoughts::ReadAloud::Controller::stateChanged, this, &ReadAloudBar::updateState);

    host->installEventFilter(this);
    hide();
    updateState();
}

bool ReadAloudBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        place();
    }
    return QFrame::eventFilter(watched, event);
}

void ReadAloudBar::updateState()
{
    using State = Thoughts::ReadAloud::Controller::State;
    const State state = m_controller ? m_controller->state() : State::Idle;
    if (state == State::Idle) {
        hide();
        return;
    }
    const bool loading = state == State::Loading;
    m_spinner->setVisible(loading);
    if (state == State::Paused) {
        m_label->setText(i18n("Read aloud paused"));
        m_pause->setText(i18nc("@action:button resume reading aloud", "Resume"));
        m_pause->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
    } else {
        m_label->setText(loading ? i18n("Preparing to read aloud…") : i18n("Reading aloud"));
        m_pause->setText(i18nc("@action:button pause reading aloud", "Pause"));
        m_pause->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-pause")));
    }
    adjustSize();
    place();
    show();
    raise();
}

void ReadAloudBar::place()
{
    QRect area = m_host->rect();
    if (auto *scrollArea = qobject_cast<QAbstractScrollArea *>(m_host)) {
        area = scrollArea->viewport()->geometry();
    }
    const QSize size = sizeHint();
    resize(size);
    move(area.left() + (area.width() - size.width()) / 2, area.bottom() - size.height() - 16);
}
