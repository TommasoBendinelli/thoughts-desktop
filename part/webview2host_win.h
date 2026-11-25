// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Microsoft Edge WebView2 in a QWindow (Windows only): what the web views of Windows share. Live
// pages (htmllivewebview_win.cpp) and embedded pages and HTML editions (htmlpagewebview_win.cpp)
// each make one Host for the QWindow they give createWindowContainer.
//
// - The QWindow's HWND is the WebView2's parent window; the window forwards its events (bounds,
//   visibility, focus, moves, its HWND going) to windowEvent().
// - Every web view is InPrivate under a profile name of its own, deleted when it closes; a runtime
//   without InPrivate gets a user-data folder of its own in the temporary folder instead.
// - No host objects, DevTools, status bar, built-in error page, autofill, dropped files,
//   downloads, permissions, script dialogs or media playing before the reader asks; new windows
//   and other applications' addresses are never opened by the web view itself (newWindow,
//   externalScheme).
// - Qt focus given elsewhere in the web view's window, or to a popup over it, takes the keyboard
//   from the web view; a click into the web view closes the open Qt popup (as on the Mac).
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wrl/client.h>

#include <WebView2.h>

class QEvent;
class QLockFile;
class QWidget;
class QWindow;

namespace Thoughts::WebView2
{
using Microsoft::WRL::ComPtr;

// A WebView2 completion or event handler (an interface with one Invoke) that runs a function.
template<typename Interface, typename Method = decltype(&Interface::Invoke)>
class Handler;

template<typename Interface, typename Owner, typename... Args>
class Handler<Interface, HRESULT (STDMETHODCALLTYPE Owner::*)(Args...)> final : public Interface
{
public:
    explicit Handler(std::function<HRESULT(Args...)> function)
        : m_function(std::move(function))
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *object = static_cast<Interface *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_references;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_references;
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override
    {
        return m_function ? m_function(args...) : S_OK;
    }

private:
    ~Handler() = default;
    std::function<HRESULT(Args...)> m_function;
    std::atomic<ULONG> m_references{1};
};

template<typename Interface, typename Function>
ComPtr<Interface> handler(Function &&function)
{
    ComPtr<Interface> result;
    result.Attach(new Handler<Interface>(std::forward<Function>(function)));
    return result;
}

const wchar_t *wide(const QString &text);
// A string WebView2 gave (CoTaskMemAlloc'ed), freed.
QString taken(LPWSTR text);
QString hresultText(HRESULT result);

// Whether WebView2 can show pages here: a real window system (not the offscreen or minimal
// platform of tests, as on the Mac) and the WebView2 runtime installed.
bool available();

class Host : public QObject
{
public:
    struct Options {
        // Pages may post messages to Thoughts (window.chrome.webview.postMessage, message).
        bool webMessages = false;
        // Ctrl+wheel and pinch zoom.
        bool zoom = true;
        // The browser's own keys (Find, Print, Reload, zoom); editing keys always work.
        bool browserKeys = true;
    };

    // @p window gives its HWND now; it forwards its events to windowEvent() and calls
    // windowDestroying() from its destructor.
    Host(QWindow *window, const Options &options);
    ~Host() override;

    void windowEvent(QEvent *event);
    void windowDestroying();

    // Asked in WebView2's own call, before the main frame (a frame) goes to @p url.
    std::function<bool(const QUrl &url, bool userGesture, bool redirected)> allowNavigation;
    std::function<bool(const QUrl &url)> allowFrameNavigation;
    // Delivered after WebView2's call returns (a receiver may delete the Host's owner).
    std::function<void(const QUrl &url, bool userGesture)> newWindow;
    std::function<void(const QUrl &url, bool userGesture)> externalScheme;
    std::function<void(const QUrl &url)> started;
    std::function<void()> committed;
    std::function<void(const QUrl &url)> finished;
    std::function<void(const QUrl &url)> stopped;
    std::function<void(const QString &error)> failed;
    std::function<void(const QString &message, const QUrl &source)> message;
    // In WebView2's own call: the web view's menu, to change before it shows.
    std::function<void(ICoreWebView2ContextMenuRequestedEventArgs *args)> contextMenu;

    // Loads @p url (once the web view exists).
    void navigate(const QUrl &url);
    // The shown page again; false when nothing shows (the caller loads the address).
    bool reload();
    void stop();
    // The web view closes (navigate() starts a new one).
    void close();
    bool isReady() const
    {
        return bool(m_web);
    }
    bool hasFocus() const
    {
        return m_focused;
    }
    HWND hwnd() const
    {
        return m_hwnd;
    }
    ICoreWebView2Environment *environment() const
    {
        return m_environment.Get();
    }
    // @p script in the page's own world; @p done gets the result as JSON (no call after the Host is
    // gone).
    void executeScript(const QString &script, std::function<void(bool ok, const QString &json)> done);
    // A DevTools protocol method; @p reply gets its result object or the error.
    void devTools(const QString &method, const QJsonObject &params, std::function<void(bool ok, const QJsonObject &reply, const QString &error)> reply);
    // The page as the web view draws it; a null image when it cannot.
    void capture(std::function<void(const QImage &)> done);
    // prefers-color-scheme: 0 follows Windows, 1 light, 2 dark.
    void setColorScheme(int mode);
    // Milliseconds since the session's last input while the window that shows the web view is in
    // front (its input goes to the browser's process; Windows counts pointer moves); -1 else.
    qint64 millisecondsSinceUserInput() const;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void start();
    void createEnvironment(const QString &folder, bool inPrivate);
    void environmentReady(HRESULT result, ICoreWebView2Environment *created, bool inPrivate);
    void controllerReady(HRESULT result, ICoreWebView2Controller *created);
    void startFailed(HRESULT result);
    void configure();
    void applyColorScheme();
    void syncBounds();
    void syncVisibility();
    void takeFocus();
    void parentMoved();
    void watchTopLevel();
    void windowGone();
    void windowBack();
    void handOffFocus(QWidget *now);
    void removeTemporaryFolder();
    template<typename Run>
    void post(Run run);

    QPointer<QWindow> m_window;
    QPointer<QWindow> m_top;
    Options m_options;
    HWND m_hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> m_environment;
    ComPtr<ICoreWebView2Controller> m_controller;
    ComPtr<ICoreWebView2> m_web;
    ComPtr<ICoreWebView2Profile> m_profile;
    // A new start makes the callbacks of an earlier environment and controller stale.
    int m_generation = 0;
    bool m_starting = false;
    bool m_windowLost = false;
    bool m_focused = false;
    // A page of the address shows (reload() reloads it).
    bool m_shown = false;
    // The navigation whose page began to show.
    UINT64 m_committed = ~UINT64(0);
    bool m_pendingLoad = false;
    QUrl m_pending;
    int m_colorScheme = 0;
    QString m_profileName;
    QString m_temporaryFolder;
    std::unique_ptr<QLockFile> m_temporaryLock;
    bool m_comInitialized = false;
};
}
