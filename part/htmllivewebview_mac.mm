// SPDX-License-Identifier: GPL-2.0-or-later
// The WKWebView of live HTML pages (htmllivewebview.h). Manual reference counting, as
// htmlmacnative.mm.
#include "htmllivewebview.h"

#include "core/htmllivepage.h"
#include "htmldocumentview.h"
#include "htmlmacnative.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

@interface ThoughtsLivePageDelegate : NSObject <WKNavigationDelegate, WKUIDelegate>
{
@public
    QPointer<Thoughts::HtmlLiveWebView> owner;
}
@end

// The web view itself: its right-click menu of selected text is Thoughts' (html-live-pages.md §2.1).
@interface ThoughtsLiveWKWebView : WKWebView
{
@public
    QPointer<Thoughts::HtmlLiveWebView> owner;
}
- (void)thoughtsChoose:(NSMenuItem *)item;
@end

namespace
{
// Delegate callbacks reach Qt after WebKit's call returns: a slot may delete the view.
template<typename Run>
void later(Thoughts::HtmlLiveWebView *owner, Run run)
{
    if (!owner) {
        return;
    }
    QPointer<Thoughts::HtmlLiveWebView> guard(owner);
    QMetaObject::invokeMethod(
        owner,
        [guard, run] {
            if (guard) {
                run(guard.data());
            }
        },
        Qt::QueuedConnection);
}

bool ignoredError(NSError *error)
{
    // A navigation this view stopped or replaced (a link elsewhere, a new load) is no failure.
    return ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)
        || ([error.domain isEqualToString:@"WebKitErrorDomain"] && (error.code == 102 || error.code == 204));
}
}

@implementation ThoughtsLivePageDelegate
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
    Thoughts::HtmlLiveWebView *view = owner.data();
    const QUrl target = QUrl::fromNSURL(action.request.URL);
    if (!view) {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    if (@available(macOS 11.3, *)) {
        // Downloads are not this view's (<a download>, a file the page cannot show).
        if (action.shouldPerformDownload) {
            decisionHandler(WKNavigationActionPolicyCancel);
            return;
        }
    }
    // A new window (target=_blank, window.open): the page itself when it stays in its origin.
    if (!action.targetFrame) {
        decisionHandler(WKNavigationActionPolicyCancel);
        if (view->nativeAllows(target, true)) {
            [webView loadRequest:action.request];
        } else {
            later(view, [target](Thoughts::HtmlLiveWebView *self) { self->nativeLinkRequested(target); });
        }
        return;
    }
    const bool mainFrame = action.targetFrame.isMainFrame;
    if (view->nativeAllows(target, mainFrame)) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    decisionHandler(WKNavigationActionPolicyCancel);
    if (mainFrame) {
        later(view, [target](Thoughts::HtmlLiveWebView *self) { self->nativeLinkRequested(target); });
    }
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationResponse:(WKNavigationResponse *)response decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler
{
    Q_UNUSED(webView)
    // What the view cannot show would be a download.
    decisionHandler(response.canShowMIMEType ? WKNavigationResponsePolicyAllow : WKNavigationResponsePolicyCancel);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation
{
    Q_UNUSED(webView)
    Q_UNUSED(navigation)
    later(owner.data(), [](Thoughts::HtmlLiveWebView *self) { self->nativeStarted(); });
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation
{
    Q_UNUSED(webView)
    Q_UNUSED(navigation)
    later(owner.data(), [](Thoughts::HtmlLiveWebView *self) { self->nativeCommitted(); });
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    Q_UNUSED(navigation)
    const QUrl url = QUrl::fromNSURL(webView.URL);
    later(owner.data(), [url](Thoughts::HtmlLiveWebView *self) { self->nativeFinished(url); });
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    Q_UNUSED(webView)
    Q_UNUSED(navigation)
    if (ignoredError(error)) {
        return;
    }
    const QString text = QString::fromNSString(error.localizedDescription);
    later(owner.data(), [text](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(text); });
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    Q_UNUSED(webView)
    Q_UNUSED(navigation)
    // After the page began to show: what it shows stays (a later resource failed or it was stopped).
    Q_UNUSED(error)
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    Q_UNUSED(webView)
    later(owner.data(), [](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(QStringLiteral("The page stopped working.")); });
}

- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)action
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    Q_UNUSED(webView)
    Q_UNUSED(configuration)
    Q_UNUSED(action)
    Q_UNUSED(windowFeatures)
    // Never a second web view (decidePolicyForNavigationAction handled the address).
    return nil;
}

- (void)webView:(WKWebView *)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
                          initiatedByFrame:(WKFrameInfo *)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:(void (^)(WKPermissionDecision decision))decisionHandler API_AVAILABLE(macos(12.0))
{
    Q_UNUSED(webView)
    Q_UNUSED(origin)
    Q_UNUSED(frame)
    Q_UNUSED(type)
    decisionHandler(WKPermissionDecisionDeny);
}
@end

@implementation ThoughtsLiveWKWebView
- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event
{
    [super willOpenMenu:menu withEvent:event];
    Thoughts::HtmlLiveWebView *view = owner.data();
    if (!view) {
        return;
    }
    // Reading the selection needs callAsyncJavaScript in a content world: without it the web
    // view's menu stays and Thoughts offers nothing.
    if (@available(macOS 11.0, *)) {
        // WebKit's menu of selected text has its Copy (WKMenuItemIdentifierCopy, the value of
        // WebKit's _WKMenuItemIdentifierCopy; a link's or an image's Copy has an identifier of its
        // own). It comes from the reader's right click (or Control-click) only.
        BOOL selected = NO;
        for (NSMenuItem *item in menu.itemArray) {
            if ([item.identifier isEqualToString:@"WKMenuItemIdentifierCopy"]) {
                selected = YES;
                break;
            }
        }
        if (!selected) {
            return;
        }
        // One menu: Thoughts' items, then the web view's own Copy. An emptied menu still opened
        // (with the system's Services in it) beside a second one of Thoughts' (23 Nov 2026), so the
        // items go into this menu itself; the selection is read when one is chosen.
        NSMenuItem *copy = nil;
        for (NSMenuItem *item in menu.itemArray) {
            if ([item.identifier isEqualToString:@"WKMenuItemIdentifierCopy"]) {
                copy = item;
                break;
            }
        }
        [menu removeAllItems];
        menu.allowsContextMenuPlugIns = NO; // no Services submenu
        if (view->selectionFeedback()) {
            const struct {
                NSString *title;
                NSString *choice;
            } choices[] = {{@"Like", @"like"}, {@"Dislike", @"dislike"}, {@"Question\u2026", @"question"}, {@"Comment\u2026", @"comment"}};
            for (const auto &entry : choices) {
                NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:entry.title action:@selector(thoughtsChoose:) keyEquivalent:@""];
                item.target = self;
                item.representedObject = entry.choice;
                [menu addItem:item];
            }
            if (copy) [menu addItem:[NSMenuItem separatorItem]];
        }
        if (copy) [menu addItem:copy];
    }
}

- (void)thoughtsChoose:(NSMenuItem *)item
{
    NSString *choice = [item.representedObject isKindOfClass:[NSString class]] ? (NSString *)item.representedObject : nil;
    if (!choice) return;
    const QString picked = QString::fromNSString(choice);
    later(owner.data(), [picked](Thoughts::HtmlLiveWebView *self) { self->nativeSelectionChoice(picked); });
}
@end

namespace
{
struct Native {
    ThoughtsLiveWKWebView *web = nil;
    ThoughtsLivePageDelegate *delegate = nil;
};

Native *native(void *handle)
{
    return static_cast<Native *>(handle);
}
}

namespace Thoughts
{
bool HtmlLiveWebView::available()
{
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
}

HtmlLiveWebView::HtmlLiveWebView(QObject *parent)
    : QObject(parent)
{
    if (!available()) {
        return;
    }
    HtmlMacNative::watchUserInput();
    auto *state = new Native;
    WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
    // Its own storage, gone with the view: nothing is kept on this computer or shared with
    // another page.
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    // No user scripts, no message handlers: the page bridge is never in this view.
    WKUserContentController *controller = [[WKUserContentController alloc] init];
    configuration.userContentController = controller;
    [controller release];
    configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
    if (@available(macOS 11.0, *)) {
        configuration.defaultWebpagePreferences.allowsContentJavaScript = YES;
    }
    configuration.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeAll;
    state->web = [[ThoughtsLiveWKWebView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480) configuration:configuration];
    state->web->owner = this;
    [configuration release];
    state->delegate = [[ThoughtsLivePageDelegate alloc] init];
    state->delegate->owner = this;
    state->web.navigationDelegate = state->delegate;
    state->web.UIDelegate = state->delegate;
    state->web.allowsMagnification = YES;
    m_native = state;
    m_window = QWindow::fromWinId(WId(state->web));
}

HtmlLiveWebView::~HtmlLiveWebView()
{
    Native *state = native(m_native);
    if (!state) {
        return;
    }
    state->delegate->owner = nullptr;
    state->web->owner = nullptr;
    [state->web stopLoading];
    state->web.navigationDelegate = nil;
    state->web.UIDelegate = nil;
    // The container owns the window; without one (never shown) it is ours.
    if (m_window && !m_window->parent()) {
        delete m_window.data();
    }
    [state->web removeFromSuperview];
    [state->web release];
    [state->delegate release];
    delete state;
}

void HtmlLiveWebView::load(const QUrl &url)
{
    m_url = url;
    Native *state = native(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    [state->web loadRequest:[NSURLRequest requestWithURL:url.toNSURL()]];
}

void HtmlLiveWebView::reload()
{
    Native *state = native(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    // After a failure there is no page to reload: the address again.
    if (!state->web.URL) {
        load(m_url);
        return;
    }
    [state->web reload];
}

void HtmlLiveWebView::callAsync(const QString &body, const QJsonObject &args, std::function<void(bool, const QString &, const QString &)> done)
{
    Native *state = native(m_native);
    if (!state) {
        done(false, QString(), QStringLiteral("there is no web view here"));
        return;
    }
    if (@available(macOS 11.0, *)) {
        NSString *json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).toNSString();
        QPointer<HtmlLiveWebView> guard(this);
        auto callback = done;
        [state->web callAsyncJavaScript:body.toNSString()
                              arguments:@{@"argsJson": json}
                                inFrame:nil
                         inContentWorld:WKContentWorld.defaultClientWorld
                      completionHandler:^(id result, NSError *error) {
                          const bool ok = !error;
                          const QString text = [result isKindOfClass:[NSString class]] ? QString::fromNSString((NSString *)result) : QString();
                          const QString why = error ? QString::fromNSString(error.localizedDescription) : QString();
                          // After WebKit's call returns: the receiver may ask the user (a dialog).
                          QMetaObject::invokeMethod(
                              qApp,
                              [guard, callback, ok, text, why] {
                                  if (!guard) {
                                      callback(false, QString(), QStringLiteral("the page was closed"));
                                  } else {
                                      callback(ok, text, why);
                                  }
                              },
                              Qt::QueuedConnection);
                      }];
        return;
    }
    done(false, QString(), QStringLiteral("this needs macOS 11 or later"));
}

void HtmlLiveWebView::snapshot(std::function<void(const QImage &)> done)
{
    Native *state = native(m_native);
    if (!state) {
        done(QImage());
        return;
    }
    HtmlMacNative::snapshotView(state->web, std::move(done));
}

void HtmlLiveWebView::copySelection()
{
    Native *state = native(m_native);
    if (state) {
        // The web view's own Copy, as its menu would have done.
        [NSApp sendAction:@selector(copy:) to:state->web from:nil];
    }
}

bool HtmlLiveWebView::hasNativeFocus() const
{
    Native *state = native(m_native);
    return state && HtmlMacNative::viewHasNativeFocus(state->web);
}

qint64 HtmlLiveWebView::millisecondsSinceUserInput() const
{
    Native *state = native(m_native);
    return state ? HtmlMacNative::millisecondsSinceUserInputInView(state->web) : -1;
}

void HtmlLiveWebView::nativeStarted()
{
    Q_EMIT loadStarted();
}

void HtmlLiveWebView::nativeCommitted()
{
    Q_EMIT loadCommitted();
}

void HtmlLiveWebView::nativeFinished(const QUrl &url)
{
    Q_EMIT loadFinished(url);
}

void HtmlLiveWebView::nativeFailed(const QString &error)
{
    Q_EMIT loadFailed(error);
}

void HtmlLiveWebView::nativeLinkRequested(const QUrl &url)
{
    if (!LivePage::isBrowserLink(url)) {
        return;
    }
    // The page's own script can navigate too: only the reader's click counts as theirs.
    Q_EMIT linkRequested(url, HtmlDocumentView::isUserGesture(millisecondsSinceUserInput()));
}

void HtmlLiveWebView::nativeSelectionMenu()
{
    Q_EMIT selectionMenuRequested();
}

void HtmlLiveWebView::nativeSelectionChoice(const QString &choice)
{
    Q_EMIT selectionChoice(choice);
}

bool HtmlLiveWebView::nativeAllows(const QUrl &target, bool mainFrame) const
{
    return LivePage::navigationStays(m_url, target, mainFrame);
}
}
