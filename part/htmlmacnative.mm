// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmlmacnative.h"

#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

@interface ThoughtsScriptMessageHandler : NSObject <WKScriptMessageHandler>
{
@public
    std::function<void(const QByteArray &)> callback;
}
@end

@implementation ThoughtsScriptMessageHandler
- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message
{
    Q_UNUSED(userContentController)
    if (!callback || ![message.body isKindOfClass:[NSString class]]) {
        return;
    }
    NSString *body = (NSString *)message.body;
    callback(QByteArray([body UTF8String]));
}
@end

namespace
{
WKWebView *findWebView(QWindow *webView)
{
    if (!webView) {
        return nil;
    }
    for (QObject *child : webView->children()) {
        if (auto *window = qobject_cast<QWindow *>(child)) {
            NSView *view = reinterpret_cast<NSView *>(window->winId());
            if ([view isKindOfClass:[WKWebView class]]) {
                return (WKWebView *)view;
            }
        }
    }
    return nil;
}
}

namespace Thoughts::HtmlMacNative
{
void focusQt(QWidget *widget)
{
    if (!widget || !widget->window() || !widget->window()->windowHandle()) {
        return;
    }
    QWidget *native = widget->nativeParentWidget() ? widget->nativeParentWidget() : widget->window();
    NSView *view = reinterpret_cast<NSView *>(native->winId());
    if (view.window && view.window.firstResponder != view) {
        [view.window makeFirstResponder:view];
    }
}

bool webViewHasNativeFocus(QWindow *webView)
{
    WKWebView *wk = findWebView(webView);
    if (!wk || !wk.window) {
        return false;
    }
    NSResponder *responder = wk.window.firstResponder;
    return [responder isKindOfClass:[NSView class]] && [(NSView *)responder isDescendantOf:wk];
}

bool setAppearance(QWindow *webView, int mode)
{
    WKWebView *wk = findWebView(webView);
    if (!wk) {
        return false;
    }
    wk.appearance = mode == 0 ? nil : [NSAppearance appearanceNamed:mode == 2 ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    return true;
}

void snapshot(QWindow *webView, std::function<void(const QImage &)> done)
{
    WKWebView *wk = findWebView(webView);
    if (!wk) {
        done(QImage());
        return;
    }
    auto callback = done;
    [wk takeSnapshotWithConfiguration:nil
                    completionHandler:^(NSImage *image, NSError *error) {
                        QImage result;
                        if (image && !error) {
                            CGImageRef cg = [image CGImageForProposedRect:nullptr context:nil hints:nil];
                            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
                            NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                            result = QImage::fromData(QByteArray::fromRawNSData(png), "PNG");
                            [rep release];
                        }
                        callback(result);
                    }];
}

void *addMessageHandler(QWindow *webView, const QString &name, std::function<void(const QByteArray &)> callback)
{
    WKWebView *wk = findWebView(webView);
    if (!wk) {
        return nullptr;
    }
    WKUserContentController *controller = wk.configuration.userContentController;
    if (!controller) {
        return nullptr;
    }
    NSString *handlerName = name.toNSString();
    [controller removeScriptMessageHandlerForName:handlerName];
    ThoughtsScriptMessageHandler *handler = [[ThoughtsScriptMessageHandler alloc] init];
    handler->callback = std::move(callback);
    [controller addScriptMessageHandler:handler name:handlerName];
    return handler; // our reference, released by removeMessageHandler
}

void removeMessageHandler(void *handle, QWindow *webView, const QString &name)
{
    if (!handle) {
        return;
    }
    ThoughtsScriptMessageHandler *handler = (ThoughtsScriptMessageHandler *)handle;
    handler->callback = nullptr;
    if (WKWebView *wk = findWebView(webView)) {
        [wk.configuration.userContentController removeScriptMessageHandlerForName:name.toNSString()];
    }
    [handler release];
}

namespace
{
NSTimeInterval lastUserInputAt = 0;
NSInteger lastUserInputWindow = 0;
}

void watchUserInput()
{
    static id monitor = nil;
    if (monitor) {
        return;
    }
    // A local monitor sees the application's own events before any view does, the WKWebView's
    // included, and only events the system delivered: a script in a page cannot produce one.
    monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskOtherMouseUp | NSEventMaskKeyDown)
                                                     handler:^NSEvent *(NSEvent *event) {
                                                         lastUserInputAt = [NSProcessInfo processInfo].systemUptime;
                                                         lastUserInputWindow = event.windowNumber;
                                                         return event;
                                                     }] retain];
}

qint64 millisecondsSinceUserInput(QWindow *webView)
{
    WKWebView *wk = findWebView(webView);
    if (!wk || !wk.window || lastUserInputAt <= 0 || wk.window.windowNumber != lastUserInputWindow) {
        return -1;
    }
    return qint64(([NSProcessInfo processInfo].systemUptime - lastUserInputAt) * 1000.0);
}
}
