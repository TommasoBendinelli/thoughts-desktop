/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _SIDEBAR_H_
#define _SIDEBAR_H_

#include "okularpart_export.h"
#include <qwidget.h>

class QIcon;
class QListWidgetItem;

class OKULARPART_EXPORT Sidebar : public QWidget
{
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);
    ~Sidebar() override;

    int addItem(QWidget *widget, const QIcon &icon, const QString &text);
    void removeItem(QWidget *widget);

    void setMainWidget(QWidget *widget);
    void setBottomWidget(QWidget *widget);
    void setTransientWidget(QWidget *widget);
    void showTransientWidget();
    void hideTransientWidget();
    bool isTransientWidgetVisible() const;

    void setCurrentItem(QWidget *widget);
    QWidget *currentItem() const;

    void setSidebarVisibility(bool visible);
    bool isSidebarVisible() const;

    void moveSplitter(int sideWidgetSize);

    QWidget *getSideContainer() const;

Q_SIGNALS:
    void urlsDropped(const QList<QUrl> &urls);
    // The panel shown in the sidebar changed (also when one is added or removed).
    void currentItemChanged(QWidget *widget);

protected:
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private Q_SLOTS:
    void splitterMoved(int pos, int index);

private:
    void styleSidePanel();

    void saveSplitterSize() const;

    // private storage
    class Private;
    Private *d;
};

#endif
