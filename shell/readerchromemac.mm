// SPDX-License-Identifier: GPL-2.0-or-later
#include "readerchrome.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <cmath>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface ThoughtsWindowButtonAligner : NSObject
@end

@implementation ThoughtsWindowButtonAligner
- (void)align:(NSNotification *)notification
{
    NSWindow *window = notification.object;
    NSButton *close = [window standardWindowButton:NSWindowCloseButton];
    NSView *titlebar = close.superview;
    NSView *container = titlebar.superview;
    if (!container || (window.styleMask & NSWindowStyleMaskFullScreen)) {
        return;
    }
    const CGFloat height = Thoughts::ReaderChrome::toolBarHeight();
    NSRect frame = container.frame;
    frame.size.height = height;
    frame.origin.y = NSHeight(window.frame) - height;
    container.frame = frame;
    titlebar.frame = container.bounds;
    for (const NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton *button = [window standardWindowButton:kind];
        [button setFrameOrigin:NSMakePoint(button.frame.origin.x, std::round((height - NSHeight(button.frame)) / 2))];
    }
}
@end

namespace
{
NSWindow *nativeWindow(QWidget *window)
{
    if (!Thoughts::ReaderChrome::mergesTitleBar()) {
        return nil;
    }
    return reinterpret_cast<NSView *>(window->winId()).window;
}

// One square of the toolbar's icon size, so symbols of different shapes line up.
QPixmap symbolPixmap(NSImage *symbol, const QColor &tint, int size, int scale)
{
    const NSSize drawn = symbol.size;
    const int pixels = size * scale;
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                       pixelsWide:pixels
                                                                       pixelsHigh:pixels
                                                                    bitsPerSample:8
                                                                  samplesPerPixel:4
                                                                         hasAlpha:YES
                                                                         isPlanar:NO
                                                                   colorSpaceName:NSDeviceRGBColorSpace
                                                                      bytesPerRow:pixels * 4
                                                                     bitsPerPixel:32];
    bitmap.size = NSMakeSize(size, size);
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    const NSRect target = NSMakeRect((size - drawn.width) / 2, (size - drawn.height) / 2, drawn.width, drawn.height);
    [symbol drawInRect:target];
    [[NSColor colorWithDeviceRed:tint.redF() green:tint.greenF() blue:tint.blueF() alpha:tint.alphaF()] set];
    NSRectFillUsingOperation(NSMakeRect(0, 0, size, size), NSCompositingOperationSourceIn);
    [NSGraphicsContext restoreGraphicsState];

    const QImage image(bitmap.bitmapData, pixels, pixels, static_cast<int>(bitmap.bytesPerRow), QImage::Format_RGBA8888_Premultiplied);
    QPixmap pixmap = QPixmap::fromImage(image.copy());
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}
}

namespace Thoughts::ReaderChrome
{
bool mergesTitleBar()
{
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

int toolBarHeight()
{
    return 52;
}

int toolBarIconSize()
{
    return mergesTitleBar() ? 22 : 18;
}

void alignWindowButtons(QWidget *window)
{
    NSWindow *native = nativeWindow(window);
    if (!native) {
        return;
    }
    static char key;
    ThoughtsWindowButtonAligner *aligner = objc_getAssociatedObject(native, &key);
    if (!aligner) {
        aligner = [[ThoughtsWindowButtonAligner alloc] init];
        objc_setAssociatedObject(native, &key, aligner, OBJC_ASSOCIATION_RETAIN);
        for (NSNotificationName name : {NSWindowDidResizeNotification, NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                                        NSWindowDidExitFullScreenNotification, NSWindowDidDeminiaturizeNotification}) {
            [NSNotificationCenter.defaultCenter addObserver:aligner selector:@selector(align:) name:name object:native];
        }
    }
    [aligner align:[NSNotification notificationWithName:NSWindowDidResizeNotification object:native]];
}

void hideWindowTitle(QWidget *window)
{
    nativeWindow(window).titleVisibility = NSWindowTitleHidden;
}

void zoomWindow(QWidget *window)
{
    NSWindow *native = nativeWindow(window);
    NSString *action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"]) {
        [native performMiniaturize:nil];
    } else if (![action isEqualToString:@"None"]) {
        [native performZoom:nil];
    }
}

QIcon symbolIcon(const QString &symbol, const QColor &tint, const QIcon &fallback)
{
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol.toNSString() accessibilityDescription:nil];
    if (!image) {
        return fallback;
    }
    const int size = toolBarIconSize();
    image = [image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:17 weight:NSFontWeightRegular scale:NSImageSymbolScaleMedium]];
    QIcon icon;
    for (const int scale : {1, 2, 3}) {
        icon.addPixmap(symbolPixmap(image, tint, size, scale));
    }
    return icon;
}
}
