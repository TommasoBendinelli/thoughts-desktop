/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>
    SPDX-FileCopyrightText: 2009 Eike Hein <hein@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sidebar.h"
#include "readertheme.h"

#include <qaction.h>
#include <qapplication.h>
#include <qevent.h>
#include <qfont.h>
#include <qfontmetrics.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlist.h>
#include <qmimedata.h>
#include <qpainter.h>
#include <qscrollbar.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qtabbar.h>
#include <qtabwidget.h>

#include <KLocalizedString>
#include <kcolorscheme.h>
#include <kiconloader.h>
#include <kurlmimedata.h>
#include <qmenu.h>

#include "settings.h"

/* Private storage. */
class Sidebar::Private
{
public:
    Private() = default;

    QSplitter *splitter = nullptr;
    QStackedWidget *contentStack = nullptr;
    QTabWidget *viewChooserTabs = nullptr;
    QWidget *transientWidget = nullptr;
    QWidget *sideContainer = nullptr;
    QVBoxLayout *vlay = nullptr;
    QWidget *sideWidget = nullptr;
    QWidget *bottomWidget = nullptr;
    bool splitterSizesSet = false;
};

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    QHBoxLayout *mainlay = new QHBoxLayout(this);
    mainlay->setContentsMargins(0, 0, 0, 0);
    mainlay->setSpacing(0);

    setAutoFillBackground(true);
    setAcceptDrops(true);

    d->splitter = new QSplitter(this);
    mainlay->addWidget(d->splitter);
    d->splitter->setOpaqueResize(true);
    d->splitter->setChildrenCollapsible(false);

    // d->sideContainer holds all the actual content
    d->sideContainer = new QWidget(d->splitter);
    d->sideContainer->setMinimumWidth(90);
    d->sideContainer->setMaximumWidth(600);
    d->vlay = new QVBoxLayout(d->sideContainer);
    d->vlay->setContentsMargins(0, 0, 0, 0);

    d->contentStack = new QStackedWidget(d->sideContainer);
    d->viewChooserTabs = new QTabWidget(d->contentStack);
    d->viewChooserTabs->setDocumentMode(true);
    // The toolbar's "Sidebar" chooser picks the panel (Annotations or
    // Contents), so the tab strip is never shown.
    d->viewChooserTabs->setTabBarAutoHide(false);
    d->viewChooserTabs->tabBar()->hide();
    d->contentStack->addWidget(d->viewChooserTabs);
    connect(d->viewChooserTabs, &QTabWidget::currentChanged, this, [this] { Q_EMIT currentItemChanged(currentItem()); });
    d->vlay->addWidget(d->contentStack);

    d->sideContainer->setObjectName(QStringLiteral("readerSidePanel"));
    d->sideContainer->setAttribute(Qt::WA_StyledBackground);
    styleSidePanel();

    connect(d->splitter, &QSplitter::splitterMoved, this, &Sidebar::splitterMoved);
}

void Sidebar::styleSidePanel()
{
    using namespace Thoughts::ReaderTheme;
    const QPalette colors = palette();
    d->sideContainer->setStyleSheet(QStringLiteral(
        "#readerSidePanel { background: %1; }"
        "#readerSidePanel QTreeView, #readerSidePanel QListView { background: transparent; border: 0; }"
        "#readerSidePanel QLineEdit { border: 0; border-radius: 6px; padding: 4px 8px; background: %2; }"
        "#readerSidePanel QAbstractScrollArea::corner { background: %1; }").arg(chrome(colors).name(), wash(colors, 13))
        + scrollBars(colors, chrome(colors), QStringLiteral("#readerSidePanel ")));
}

void Sidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && d) {
        styleSidePanel();
    }
}

Sidebar::~Sidebar()
{
    // The tab widget outlives d (QWidget deletes children later) and emits
    // currentChanged while its tabs go away; currentItem() must not run then.
    d->viewChooserTabs->disconnect(this);
    delete d;
    d = nullptr;
}

int Sidebar::addItem(QWidget *widget, const QIcon &icon, const QString &text)
{
    if (!widget) {
        return -1;
    }

    const int existingIndex = d->viewChooserTabs->indexOf(widget);
    if (existingIndex >= 0) {
        return existingIndex;
    }

    widget->setParent(d->viewChooserTabs);
    d->viewChooserTabs->addTab(widget, icon, text);
    const int thisTabIndex = d->viewChooserTabs->count() - 1;
    // The tab strip stays hidden; the name remains as the tab's tooltip.
    d->viewChooserTabs->setTabText(thisTabIndex, QString());
    d->viewChooserTabs->setTabToolTip(thisTabIndex, text);
    return thisTabIndex;
}

void Sidebar::removeItem(QWidget *widget)
{
    const int index = d->viewChooserTabs->indexOf(widget);
    if (index >= 0) {
        d->viewChooserTabs->removeTab(index);
    }
}

void Sidebar::setMainWidget(QWidget *widget)
{
    delete d->sideWidget;
    d->sideWidget = widget;
    if (d->sideWidget) {
        // setting the splitter as parent for the widget automatically plugs it
        // into the splitter, neat!
        d->sideWidget->setParent(d->splitter);
        setFocusProxy(widget);

        if (!d->splitterSizesSet) {
            QList<int> splitterSizes = Okular::Settings::splitterSizes();
            if (splitterSizes.isEmpty()) {
                // the first time use 1/10 for the panel and 9/10 for the pageView
                splitterSizes.push_back(50);
                splitterSizes.push_back(500);
            }
            d->splitter->setSizes(splitterSizes);
            d->splitterSizesSet = true;
        }
    }
}

void Sidebar::setBottomWidget(QWidget *widget)
{
    delete d->bottomWidget;
    d->bottomWidget = widget;
    if (d->bottomWidget) {
        d->bottomWidget->setParent(this);
        d->vlay->addWidget(d->bottomWidget);
    }
}

void Sidebar::setTransientWidget(QWidget *widget)
{
    if (d->transientWidget == widget) {
        return;
    }

    if (d->transientWidget) {
        d->contentStack->removeWidget(d->transientWidget);
    }

    d->transientWidget = widget;
    if (d->transientWidget) {
        d->contentStack->addWidget(d->transientWidget);
    }
    d->contentStack->setCurrentWidget(d->viewChooserTabs);
}

void Sidebar::showTransientWidget()
{
    if (d->transientWidget) {
        d->contentStack->setCurrentWidget(d->transientWidget);
    }
}

void Sidebar::hideTransientWidget()
{
    d->contentStack->setCurrentWidget(d->viewChooserTabs);
}

bool Sidebar::isTransientWidgetVisible() const
{
    return d->transientWidget && d->contentStack->currentWidget() == d->transientWidget;
}

void Sidebar::setCurrentItem(QWidget *widget)
{
    d->viewChooserTabs->setCurrentWidget(widget);
}

QWidget *Sidebar::currentItem() const
{
    if (d->viewChooserTabs->currentIndex() == -1) {
        return nullptr;
    }

    return d->viewChooserTabs->currentWidget();
}

void Sidebar::setSidebarVisibility(bool visible)
{
    d->sideContainer->setHidden(!visible);
}

bool Sidebar::isSidebarVisible() const
{
    return !d->sideContainer->isHidden();
}

void Sidebar::moveSplitter(int sideWidgetSize)
{
    QList<int> splitterSizeList = d->splitter->sizes();
    const int total = splitterSizeList.at(0) + splitterSizeList.at(1);
    splitterSizeList.replace(0, total - sideWidgetSize);
    splitterSizeList.replace(1, sideWidgetSize);
    d->splitter->setSizes(splitterSizeList);
}

QWidget *Sidebar::getSideContainer() const
{
    return d->sideContainer;
}

void Sidebar::splitterMoved(int /*pos*/, int index)
{
    // if the side panel has been resized, save splitter sizes
    if (index == 1) {
        saveSplitterSize();
    }
}

void Sidebar::saveSplitterSize() const
{
    Okular::Settings::setSplitterSizes(d->splitter->sizes());
    Okular::Settings::self()->save();
}

void Sidebar::dragEnterEvent(QDragEnterEvent *event)
{
    event->setAccepted(event->mimeData()->hasUrls());
}

void Sidebar::dropEvent(QDropEvent *event)
{
    const QList<QUrl> list = KUrlMimeData::urlsFromMimeData(event->mimeData());
    Q_EMIT urlsDropped(list);
}
