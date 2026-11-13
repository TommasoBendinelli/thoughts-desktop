// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/macquitevent.h"

#include <QCloseEvent>
#include <QEventLoop>
#include <QTest>
#include <QTimer>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace
{
class StubbornWindow : public QWidget
{
protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
    }
};

// Sends this process the quit Apple event, as the Dock or a script does, and returns the
// reply's error number (0 for none).
int sendQuit()
{
    NSAppleEventDescriptor *quit = [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                                                                            eventID:kAEQuitApplication
                                                                   targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
                                                                           returnID:kAutoGenerateReturnID
                                                                      transactionID:kAnyTransactionID];
    NSError *error = nil;
    NSAppleEventDescriptor *reply = [quit sendEventWithOptions:NSAppleEventSendWaitForReply timeout:10 error:&error];
    if (error) {
        return int(error.code);
    }
    NSAppleEventDescriptor *number = [reply paramDescriptorForKeyword:keyErrorNumber];
    return number ? number.int32Value : 0;
}

// Qt only handles a termination request while an event loop runs, so send it from one.
int quitFromEventLoop(int *loopResult)
{
    QEventLoop loop;
    int reply = -1;
    QTimer::singleShot(0, &loop, [&reply] { reply = sendQuit(); });
    // Ends the loop when the application did not quit.
    QTimer::singleShot(500, &loop, [&loop] { loop.exit(7); });
    *loopResult = loop.exec();
    return reply;
}
}

class MacQuitEventTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testRefusedQuitRepliesCanceled();
    // Last: the accepted quit ends the application's event loops.
    void testAcceptedQuitRepliesSuccess();
};

void MacQuitEventTest::initTestCase()
{
    Thoughts::installMacQuitEventHandler();
}

void MacQuitEventTest::testRefusedQuitRepliesCanceled()
{
    StubbornWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int loopResult = 0;
    QCOMPARE(quitFromEventLoop(&loopResult), int(userCanceledErr));
    QCOMPARE(loopResult, 7);
    QVERIFY(window.isVisible());
}

void MacQuitEventTest::testAcceptedQuitRepliesSuccess()
{
    QWidget window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int loopResult = 7;
    QCOMPARE(quitFromEventLoop(&loopResult), 0);
    QCOMPARE(loopResult, 0);
    QVERIFY(!window.isVisible());
}

QTEST_MAIN(MacQuitEventTest)
#include "macquiteventtest.moc"
