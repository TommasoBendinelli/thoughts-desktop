// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QFrame>
#include <QPointer>

#include "readaloud.h"

class KBusyIndicatorWidget;
class QLabel;
class QToolButton;

// The small floating control at the bottom of the reader while Read Aloud
// runs: a spinner until the first audio, then "Reading aloud", Pause/Resume
// and Stop. It hides itself when reading ends.
class ReadAloudBar : public QFrame
{
    Q_OBJECT

public:
    ReadAloudBar(Thoughts::ReadAloud::Controller *controller, QWidget *host);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateState();
    void place();

    QPointer<Thoughts::ReadAloud::Controller> m_controller;
    QWidget *m_host;
    KBusyIndicatorWidget *m_spinner;
    QLabel *m_label;
    QToolButton *m_pause;
    QToolButton *m_stop;
};
