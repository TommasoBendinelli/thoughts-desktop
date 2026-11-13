// SPDX-License-Identifier: GPL-2.0-or-later
#include "macquitevent.h"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#import <AppKit/AppKit.h>

namespace
{
// Watches the Quit event that Qt sends the application while AppKit asks it to terminate.
// The application ignores the event when a window refuses to close (unsaved changes, say).
class QuitDecision : public QObject
{
public:
    bool accepted = false;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != QCoreApplication::instance() || event->type() != QEvent::Quit) {
            return false;
        }
        // Deliver it here, as the application would, to read its answer afterwards.
        watched->event(event);
        accepted = event->isAccepted();
        return true;
    }
};
}

@interface ThoughtsQuitEventHandler : NSObject
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation ThoughtsQuitEventHandler
- (void)handleQuitEvent:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply
{
    Q_UNUSED(event)
    // Qt's application delegate answers NSTerminateCancel even to a termination it accepts,
    // so that Thoughts leaves through main() rather than AppKit's exit(). AppKit's own handler
    // reads that as a refusal and replies userCanceledErr. Run the same termination (session
    // data, window closing) and reply with the application's actual decision.
    QuitDecision decision;
    QCoreApplication::instance()->installEventFilter(&decision);
    [NSApp terminate:nil];
    QCoreApplication::instance()->removeEventFilter(&decision);
    if (!decision.accepted) {
        [reply setParamDescriptor:[NSAppleEventDescriptor descriptorWithInt32:userCanceledErr] forKeyword:keyErrorNumber];
    }
}
@end

void Thoughts::installMacQuitEventHandler()
{
    static ThoughtsQuitEventHandler *const handler = [[ThoughtsQuitEventHandler alloc] init];
    static const auto install = [] {
        [[NSAppleEventManager sharedAppleEventManager] setEventHandler:handler
                                                          andSelector:@selector(handleQuitEvent:withReplyEvent:)
                                                        forEventClass:kCoreEventClass
                                                           andEventID:kAEQuitApplication];
    };
    install();
    // AppKit installs its default handlers while the application finishes launching, replacing
    // ours if that has not happened yet; install it again once launching is done.
    static id observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification
                                                                           object:nil
                                                                            queue:nil
                                                                       usingBlock:^(NSNotification *) {
                                                                           install();
                                                                       }];
    Q_UNUSED(observer)
}
