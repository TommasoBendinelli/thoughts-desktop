// SPDX-License-Identifier: GPL-2.0-or-later
#include "visualstudiocode.h"

#import <AppKit/AppKit.h>

namespace Thoughts::VisualStudioCode
{
QString applicationForBundleId(const QString &bundleId)
{
    if (bundleId.isEmpty()) {
        return {};
    }
    @autoreleasepool {
        NSURL *url = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:bundleId.toNSString()];
        return url.isFileURL ? QString::fromNSString(url.path) : QString();
    }
}
}
