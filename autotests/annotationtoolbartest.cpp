/*
    SPDX-FileCopyrightText: 2020 Simone Gaiarin <simgunz@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// clazy:excludeall=qstring-allocations

#include <QTest>

#include <QThreadPool>
#include <QToolBar>
#include <QWidget>

#include <KActionCollection>

#include "../part/part.h"
#include "../settings.h"
#include "../shell/commentsoverview.h"
#include "../shell/okular_main.h"
#include "../shell/shell.h"
#include "../shell/shellutils.h"

namespace Okular
{
class PartTest
{
public:
    Okular::Document *partDocument(Okular::Part *part) const
    {
        return part->m_document;
    }
};
}

class AnnotationToolBarTest : public QObject, public Okular::PartTest
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testAnnotationToolsStayRemoved();
    void testAnnotationToolsStayRemoved_data();
};

Shell *findShell(Shell *ignore = nullptr)
{
    const QWidgetList wList = QApplication::topLevelWidgets();
    for (QWidget *widget : wList) {
        Shell *s = qobject_cast<Shell *>(widget);
        if (s && s != ignore) {
            return s;
        }
    }
    return nullptr;
}

void AnnotationToolBarTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // Don't pollute people's okular settings
    Okular::Settings::instance(QStringLiteral("annotationtoolbartest"));
}

void AnnotationToolBarTest::init()
{
    // Default settings for every test
    Okular::Settings::self()->setDefaults();
}

void AnnotationToolBarTest::cleanup()
{
    // Shell starts review index workers on the global pool; they must not
    // outlive QApplication.
    QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
    if (Shell::s_commentsOverview) {
        for (QThreadPool *pool : Shell::s_commentsOverview->findChildren<QThreadPool *>()) {
            QVERIFY(pool->waitForDone(10000));
        }
    }
    delete Shell::s_commentsOverview.data();
    Shell *s;
    while ((s = findShell())) {
        delete s;
    }
    QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
}

// Thoughts replaced Okular's annotation toolbars with its own comment and
// highlight workflow: every annotation tool stays disabled and hidden, and
// re-enabling the document's actions must not bring them back (bug 424296).
void AnnotationToolBarTest::testAnnotationToolsStayRemoved()
{
    QFETCH(QString, document);

    QString serializedOptions = ShellUtils::serializeOptions(false, false, false, false, false, QString(), QString(), QString());
    QCOMPARE(Okular::main({document}, serializedOptions), Okular::Success);
    Shell *s = findShell();
    QVERIFY(s);
    QVERIFY(QTest::qWaitForWindowExposed(s));

    Okular::Part *part = s->findChild<Okular::Part *>();
    QVERIFY(part);
    QVERIFY(partDocument(part)->pages() > 0);
    QVERIFY(!s->findChild<QToolBar *>(QStringLiteral("annotationToolBar")));
    QVERIFY(!s->findChild<QToolBar *>(QStringLiteral("quickAnnotationToolBar")));

    KActionCollection *ac = part->actionCollection();
    const auto verifyRemoved = [ac] {
        int annotationActions = 0;
        for (QAction *action : ac->actions()) {
            const QString name = action->objectName();
            if (!name.startsWith(QLatin1String("annotation_")) && name != QLatin1String("mouse_toggle_annotate")) {
                continue;
            }
            ++annotationActions;
            if (action->isEnabled() || action->isVisible()) {
                qWarning() << "annotation action is available:" << name;
                return false;
            }
        }
        return annotationActions > 0;
    };
    QVERIFY(verifyRemoved());

    // Switching the mouse mode reparses the tools, and Alt+1 / 6 were the
    // annotation shortcuts.
    ac->action(QStringLiteral("mouse_select"))->trigger();
    ac->action(QStringLiteral("mouse_drag"))->trigger();
    QTest::keyClick(part->widget(), Qt::Key_1, Qt::AltModifier);
    QTest::keyClick(part->widget(), Qt::Key_6);
    QVERIFY(verifyRemoved());
    QCOMPARE(Okular::Settings::mouseMode(), static_cast<int>(Okular::Settings::EnumMouseMode::Browse));
}

void AnnotationToolBarTest::testAnnotationToolsStayRemoved_data()
{
    QTest::addColumn<QString>("document");

    QTest::addRow("pdf") << QStringLiteral(KDESRCDIR "data/file1.pdf");
    QTest::addRow("protected-pdf") << QStringLiteral(KDESRCDIR "data/protected.pdf");
    QTest::addRow("image") << QStringLiteral(KDESRCDIR "data/potato.jpg");
}

QTEST_MAIN(AnnotationToolBarTest)
#include "annotationtoolbartest.moc"
