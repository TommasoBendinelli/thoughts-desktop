// SPDX-License-Identifier: GPL-2.0-or-later
#include "activeparttoolbarcontroller.h"

#include <KActionCollection>

#include <QAction>
#include <QBoxLayout>
#include <QMenu>
#include <QWidgetAction>

namespace
{
void copyActionState(QAction *proxy, QAction *target)
{
    proxy->setText(target ? target->text() : QString());
    proxy->setIcon(target ? target->icon() : QIcon());
    proxy->setIconText(target ? target->iconText() : QString());
    proxy->setToolTip(target ? target->toolTip() : QString());
    proxy->setStatusTip(target ? target->statusTip() : QString());
    proxy->setWhatsThis(target ? target->whatsThis() : QString());
    proxy->setData(target ? target->data() : QVariant());
    proxy->setMenu(target ? target->menu() : nullptr);
    proxy->setCheckable(target && target->isCheckable());
    proxy->setChecked(target && target->isChecked());
    proxy->setEnabled(target && target->isEnabled());
    proxy->setVisible(target && target->isVisible());
    proxy->setPriority(target ? target->priority() : QAction::NormalPriority);
}

class ActivePartAction final : public QAction
{
public:
    explicit ActivePartAction(QObject *parent)
        : QAction(parent)
    {
        connect(this, &QAction::triggered, this, [this] {
            if (m_target) {
                m_target->trigger();
                // A target in an exclusive group stays checked and reports no change, while a
                // click unchecked this proxy.
                copyActionState(this, m_target);
            }
        });
    }

    void setTarget(QAction *target)
    {
        if (m_target == target) {
            copyActionState(this, target);
            return;
        }
        if (m_changedConnection) {
            disconnect(m_changedConnection);
        }
        if (m_destroyedConnection) {
            disconnect(m_destroyedConnection);
        }
        m_target = target;
        if (target) {
            m_changedConnection = connect(target, &QAction::changed, this, [this] {
                copyActionState(this, m_target);
            });
            m_destroyedConnection = connect(target, &QObject::destroyed, this, [this] {
                setTarget(nullptr);
            });
        }
        copyActionState(this, target);
    }

private:
    QPointer<QAction> m_target;
    QMetaObject::Connection m_changedConnection;
    QMetaObject::Connection m_destroyedConnection;
};

class ActivePartWidgetAction final : public QWidgetAction
{
public:
    explicit ActivePartWidgetAction(QObject *parent)
        : QWidgetAction(parent)
    {
    }

    void setTarget(QAction *target)
    {
        if (m_target == target) {
            copyActionState(this, target);
            return;
        }
        if (m_changedConnection) {
            disconnect(m_changedConnection);
        }
        if (m_destroyedConnection) {
            disconnect(m_destroyedConnection);
        }
        releaseTargetWidgets();
        m_target = target;
        if (target) {
            m_changedConnection = connect(target, &QAction::changed, this, [this] {
                copyActionState(this, m_target);
            });
            m_destroyedConnection = connect(target, &QObject::destroyed, this, [this] {
                setTarget(nullptr);
            });
        }
        copyActionState(this, target);
        installTargetWidgets();
    }

protected:
    QWidget *createWidget(QWidget *parent) override
    {
        auto *host = new QWidget(parent);
        auto *layout = new QHBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_hosts.append(host);
        installTargetWidget(host);
        return host;
    }

    void deleteWidget(QWidget *widget) override
    {
        releaseTargetWidget(widget);
        m_hosts.removeAll(widget);
        QWidgetAction::deleteWidget(widget);
    }

private:
    void installTargetWidget(QWidget *host)
    {
        auto *target = qobject_cast<QWidgetAction *>(m_target.data());
        if (!host || !target || !host->layout()) {
            return;
        }
        QWidget *widget = target->requestWidget(host);
        if (widget) {
            host->layout()->addWidget(widget);
            m_targetWidgets.insert(host, widget);
        }
    }

    void installTargetWidgets()
    {
        for (QWidget *host : std::as_const(m_hosts)) {
            installTargetWidget(host);
        }
    }

    void releaseTargetWidget(QWidget *host)
    {
        QWidget *widget = m_targetWidgets.take(host);
        if (!widget) {
            return;
        }
        if (auto *target = qobject_cast<QWidgetAction *>(m_target.data())) {
            target->releaseWidget(widget);
        } else {
            widget->deleteLater();
        }
    }

    void releaseTargetWidgets()
    {
        for (QWidget *host : std::as_const(m_hosts)) {
            releaseTargetWidget(host);
        }
    }

    QPointer<QAction> m_target;
    QList<QWidget *> m_hosts;
    QHash<QWidget *, QPointer<QWidget>> m_targetWidgets;
    QMetaObject::Connection m_changedConnection;
    QMetaObject::Connection m_destroyedConnection;
};
}

class ActivePartToolbarController::Private
{
public:
    KActionCollection *shellActions = nullptr;
    QHash<QString, QAction *> proxies;
};

ActivePartToolbarController::ActivePartToolbarController(KActionCollection *shellActions,
                                                         KActionCollection *initialPartActions)
    : d(std::make_unique<Private>())
{
    d->shellActions = shellActions;
    for (QAction *target : initialPartActions->actions()) {
        const QString name = target->objectName();
        if (name.isEmpty() || shellActions->action(name)) {
            continue;
        }
        QAction *proxy = nullptr;
        if (qobject_cast<QWidgetAction *>(target)) {
            proxy = new ActivePartWidgetAction(shellActions);
        } else {
            proxy = new ActivePartAction(shellActions);
        }
        shellActions->addAction(name, proxy);
        d->proxies.insert(name, proxy);
    }
    setActivePart(initialPartActions);
}

ActivePartToolbarController::~ActivePartToolbarController() = default;

void ActivePartToolbarController::setActivePart(KActionCollection *partActions)
{
    for (auto iterator = d->proxies.constBegin(); iterator != d->proxies.constEnd(); ++iterator) {
        QAction *target = partActions ? partActions->action(iterator.key()) : nullptr;
        if (auto *proxy = dynamic_cast<ActivePartAction *>(iterator.value())) {
            proxy->setTarget(target);
        } else if (auto *proxy = dynamic_cast<ActivePartWidgetAction *>(iterator.value())) {
            proxy->setTarget(target);
        }
    }
}

QAction *ActivePartToolbarController::action(const QString &name) const
{
    return d->proxies.value(name);
}
