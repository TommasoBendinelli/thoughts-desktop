// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// macOS helpers for the HTML view (Spike 2): the WKWebView behind Qt WebView's QWebView is a
// foreign native view, so keyboard focus and appearance need a nudge from AppKit.
#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>

class QWidget;
class QWindow;

namespace Thoughts::HtmlMacNative
{
// Makes the Qt view of `widget`'s window the native first responder again, so keys reach Qt
// after a programmatic setFocus() elsewhere (otherwise they keep going to the page).
void focusQt(QWidget *widget);
// True when the WKWebView behind `webView` is the window's first responder.
bool webViewHasNativeFocus(QWindow *webView);
// 0 follow the app, 1 light, 2 dark.
bool setAppearance(QWindow *webView, int mode);
// The page as WKWebView draws it (tests, screenshots); a null image when it cannot.
void snapshot(QWindow *webView, std::function<void(const QImage &)> done);
// A WKScriptMessageHandler `name` on the web view's user content controller: the page posts with
// window.webkit.messageHandlers[name].postMessage(string) and `callback` receives the UTF-8 bytes
// (on the main thread). This transport needs no network, so it works under a sandbox CSP with
// connect-src 'none' (HTML attachments). Returns a handle, or nullptr when there is no WKWebView yet.
void *addMessageHandler(QWindow *webView, const QString &name, std::function<void(const QByteArray &)> callback);
// Detaches and releases a handle of addMessageHandler (the callback is never called again).
void removeMessageHandler(void *handle, QWindow *webView, const QString &name);
// Starts noting when the reader last pressed a mouse button or a key in this application:
// real AppKit events, which a page's script cannot make. Safe to call again.
void watchUserInput();
// Milliseconds since the reader last did so in the window that shows `webView`; -1 if never.
qint64 millisecondsSinceUserInput(QWindow *webView);
}
