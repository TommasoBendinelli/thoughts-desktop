// SPDX-License-Identifier: GPL-2.0-or-later
#include <QStringList>
#import <AppKit/AppKit.h>

QStringList visibleNativeApplicationMenuItems()
{
    QStringList result;
    NSMenu *menu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
    for (NSMenuItem *item in [menu itemArray]) {
        if (![item isHidden] && [item isEnabled] && ![item isSeparatorItem]) {
            result.append(QString::fromUtf8([[item title] UTF8String]));
        }
    }
    return result;
}
