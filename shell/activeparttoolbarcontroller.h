// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QPointer>
#include <QString>

#include <memory>

class QAction;
class KActionCollection;

class ActivePartToolbarController
{
public:
    ActivePartToolbarController(KActionCollection *shellActions, KActionCollection *initialPartActions);
    ~ActivePartToolbarController();

    void setActivePart(KActionCollection *partActions);
    QAction *action(const QString &name) const;

private:
    class Private;
    std::unique_ptr<Private> d;
};
