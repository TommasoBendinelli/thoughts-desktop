// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnotesnotifier.h"

#include "agentnotes.h"
#include "developerlog.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

static NSString *const ThoughtsAgentNoteIdKey = @"thoughts_agent_note_id";

@interface ThoughtsAgentNoteNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation ThoughtsAgentNoteNotificationDelegate

// Banners also while Thoughts is the active app: the note is for whoever sits at the Mac.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    Q_UNUSED(center);
    Q_UNUSED(notification);
    if (@available(macOS 11.0, *)) {
        completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
    } else {
        completionHandler(UNNotificationPresentationOptionAlert);
    }
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    Q_UNUSED(center);
    id value = response.notification.request.content.userInfo[ThoughtsAgentNoteIdKey];
    const QString noteId = [value isKindOfClass:[NSString class]] ? QString::fromNSString(static_cast<NSString *>(value)) : QString();
    if (!noteId.isEmpty()) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [noteId] { Thoughts::AgentNotes::Center::instance()->requestOpen(noteId); }, Qt::QueuedConnection);
    }
    completionHandler();
}

@end

namespace Thoughts::AgentNotes
{
namespace
{
bool usable()
{
    // UNUserNotificationCenter throws outside an app bundle (a test binary has no bundle ID).
    return QGuiApplication::platformName() == QLatin1String("cocoa") && NSBundle.mainBundle.bundleIdentifier.length > 0;
}

QString body(const Note &note)
{
    QString text = (!note.message.isEmpty() ? note.message : note.reviewQuestion).simplified();
    if (text.size() > 240) text = text.left(239) + QChar(0x2026);
    return text;
}

void post(const Note &note)
{
    UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
    NSString *identifier = note.id.toNSString();
    NSString *title = (note.title.isEmpty() ? QStringLiteral("Agent note") : note.title).toNSString();
    NSString *subtitle = note.senderLabel().toNSString();
    NSString *text = body(note).toNSString();
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL granted, NSError *error) {
                              if (!granted) {
                                  const QString reason = error ? QString::fromNSString(error.localizedDescription) : QString();
                                  QMetaObject::invokeMethod(QCoreApplication::instance(), [reason] {
                                      DeveloperLog::record(QStringLiteral("agent_notes.notification.not_allowed"), {{QStringLiteral("reason"), reason}});
                                  }, Qt::QueuedConnection);
                                  return;
                              }
                              UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
                              content.title = title;
                              content.subtitle = subtitle;
                              content.body = text;
                              content.threadIdentifier = @"agent-notes";
                              content.userInfo = @{ThoughtsAgentNoteIdKey: identifier};
                              UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:nil];
                              [center addNotificationRequest:request withCompletionHandler:^(NSError *addError) {
                                  if (!addError) return;
                                  const QString reason = QString::fromNSString(addError.localizedDescription);
                                  QMetaObject::invokeMethod(QCoreApplication::instance(), [reason] {
                                      DeveloperLog::record(QStringLiteral("agent_notes.notification.failed"), {{QStringLiteral("reason"), reason}});
                                  }, Qt::QueuedConnection);
                              }];
                          }];
}
}

void installSystemNotifications()
{
    static bool installed = false;
    if (installed || !usable()) return;
    installed = true;
    static ThoughtsAgentNoteNotificationDelegate *delegate = [[ThoughtsAgentNoteNotificationDelegate alloc] init];
    UNUserNotificationCenter.currentNotificationCenter.delegate = delegate;
    Center *center = Center::instance();
    QObject::connect(center, &Center::noteNeedsAttention, center, [](const Note &note) { post(note); });
    // A note read here or opened on the phone leaves Notification Center too.
    const auto withdrawRead = [center] {
        NSMutableArray<NSString *> *read = [NSMutableArray array];
        for (const Note &note : center->notes()) {
            if (!center->isUnread(note)) [read addObject:note.id.toNSString()];
        }
        if (read.count) [UNUserNotificationCenter.currentNotificationCenter removeDeliveredNotificationsWithIdentifiers:read];
    };
    QObject::connect(center, &Center::seenChanged, center, withdrawRead);
    QObject::connect(center, &Center::notesChanged, center, withdrawRead);
}
}
