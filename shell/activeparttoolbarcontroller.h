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
    // Whether the action is one of the window's stand-ins for the active part's actions. They are
    // for its toolbars only: the part's own action is in the menus.
    bool isStandIn(const QAction *action) const;

private:
    class Private;
    std::unique_ptr<Private> d;
};
