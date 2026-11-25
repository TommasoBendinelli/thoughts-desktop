// SPDX-License-Identifier: GPL-2.0-or-later
// The web view of live HTML pages on Windows (htmllivewebview.h): a Microsoft Edge WebView2 with a
// configuration of its own, as htmllivewebview_mac.mm makes a WKWebView:
//
// - InPrivate under a profile name of its own (cookies, storage and caches end with the view and
//   are shared with no other page), the profile deleted when the view closes; a WebView2 runtime
//   without InPrivate gets a user-data folder of its own in the temporary folder instead, removed
//   once its browser has quit;
// - no host objects and no web messages, so the page gets none of Thoughts' page bridge;
// - the page's scripts and the network allowed; no downloads, new windows, dropped files,
//   external applications, permissions (camera, microphone, location…), script dialogs, DevTools
//   or media playing before the reader asks;
// - navigation only within the page's origin in the main frame (frames inside it may load any web
//   address); a link elsewhere or to a new window is stopped and reported as linkRequested.
//
// Thoughts' own scripts (callAsync: Take Snapshot, reading the selection) run through the DevTools
// protocol in an isolated world of their own ("thoughts"), which the page's scripts cannot reach,
// as WKContentWorld.defaultClientWorld on the Mac. The web view's own right-click menu of selected
// text shows Thoughts' Like, Dislike, Question… and Comment… with the web view's Copy (one menu);
// every other menu stays the web view's. The web view is a child of a QWindow of its own, which
// HtmlDocumentView embeds with createWindowContainer.
#include "htmllivewebview.h"

#include "core/htmllivepage.h"
#include "htmldocumentview.h"

#include <QApplication>
#include <QDir>
#include <QExposeEvent>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <wrl/client.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <atomic>
#include <memory>

namespace
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

const wchar_t *wide(const QString &text)
{
    return reinterpret_cast<const wchar_t *>(text.utf16());
}

// A string WebView2 gave (CoTaskMemAlloc'ed), freed.
QString taken(LPWSTR text)
{
    if (!text) {
        return {};
    }
    const QString result = QString::fromWCharArray(text);
    CoTaskMemFree(text);
    return result;
}

QString hresultText(HRESULT result)
{
    return QStringLiteral("0x%1").arg(quint32(result), 8, 16, QLatin1Char('0'));
}

QString jsString(const QString &text)
{
    // A JSON string is a JS string literal (U+2028/9 are valid in JS strings since ES2019).
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QByteArray streamBytes(IStream *stream)
{
    STATSTG stat{};
    if (!stream || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > quint64(512) * 1024 * 1024) {
        return {};
    }
    QByteArray bytes(qsizetype(stat.cbSize.QuadPart), Qt::Uninitialized);
    LARGE_INTEGER start{};
    ULONG read = 0;
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) || FAILED(stream->Read(bytes.data(), ULONG(bytes.size()), &read))) {
        return {};
    }
    bytes.resize(read);
    return bytes;
}

// What a failed navigation says, as the Mac shows NSError's description.
QString navigationError(COREWEBVIEW2_WEB_ERROR_STATUS status)
{
    switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
        return QStringLiteral("The certificate for this server is invalid.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
        return QStringLiteral("Could not connect to the server.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
        return QStringLiteral("The request timed out.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
        return QStringLiteral("The server sent an invalid response.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
        return QStringLiteral("The network connection was lost.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
        return QStringLiteral("A server with the specified hostname could not be found.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED:
        return QStringLiteral("The page could not be redirected.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_PROXY_AUTHENTICATION_REQUIRED:
        return QStringLiteral("The page needs a sign-in.");
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN:
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNEXPECTED_ERROR:
        break;
    }
    return QStringLiteral("The page could not be loaded.");
}

// Callbacks reach Qt after WebView2's call returns: a slot may delete the view.
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

// The browser's own files, shared by the views of this computer's account: no browsing data, every
// view is InPrivate.
QString sharedDataFolder()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/live-pages-webview2");
    QDir().mkpath(folder);
    return QDir::toNativeSeparators(folder);
}

QString temporaryPrefix()
{
    return QStringLiteral("thoughts-live-webview2-");
}

QString temporaryLockName()
{
    return QStringLiteral("/thoughts.lock");
}

// Without InPrivate: a user-data folder of the view's own. Those of views gone (their lock free)
// are removed first.
QString temporaryDataFolder(std::unique_ptr<QLockFile> *lock)
{
    const QDir temp(QDir::tempPath());
    const QStringList stale = temp.entryList({temporaryPrefix() + QLatin1Char('*')}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : stale) {
        QLockFile other(temp.filePath(name) + temporaryLockName());
        if (other.tryLock(0)) {
            other.unlock();
            QDir(temp.filePath(name)).removeRecursively();
        }
    }
    QTemporaryDir folder(temp.filePath(temporaryPrefix() + QStringLiteral("XXXXXX")));
    if (!folder.isValid()) {
        return {};
    }
    folder.setAutoRemove(false);
    auto file = std::make_unique<QLockFile>(folder.path() + temporaryLockName());
    if (!file->tryLock(0)) {
        QDir(folder.path()).removeRecursively();
        return {};
    }
    *lock = std::move(file);
    return QDir::toNativeSeparators(folder.path());
}

class LiveWindow;

constexpr UINT64 NoNavigation = ~UINT64(0);

// The WebView2 of one HtmlLiveWebView, its window and what the view knows of its state.
class Native : public QObject
{
public:
    explicit Native(Thoughts::HtmlLiveWebView *view);
    ~Native() override;

    void start();
    void close();
    void navigate(const QUrl &url);
    void syncBounds();
    void syncVisibility();
    void takeFocus();
    void parentMoved();
    void windowGone();
    void windowBack();
    void devTools(const QString &method, const QJsonObject &params, std::function<void(bool ok, const QJsonObject &reply, const QString &error)> reply);
    void callIsolated(const QString &expression, std::function<void(bool, const QString &, const QString &)> done);

    QPointer<Thoughts::HtmlLiveWebView> owner;
    QPointer<LiveWindow> window;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    ComPtr<ICoreWebView2Profile> profile;
    // A new start makes the callbacks of an earlier environment and controller stale.
    int generation = 0;
    bool starting = false;
    bool windowLost = false;
    // The controller's GotFocus/LostFocus.
    bool focused = false;
    // A page of the address shows (reload() reloads it; else the address is loaded again).
    bool shown = false;
    // The navigation whose page began to show (none: NoNavigation).
    UINT64 committedNavigation = NoNavigation;
    bool pendingLoad = false;
    QUrl pending;
    // Whether the reader's own click asked for the link being reported (WebView2's
    // IsUserInitiated); -1 when unknown.
    int linkGesture = -1;
    QString profileName;
    QString temporaryFolder;
    std::unique_ptr<QLockFile> temporaryLock;

private:
    void createEnvironment(const QString &folder, bool inPrivate);
    void environmentReady(HRESULT result, ICoreWebView2Environment *created, bool inPrivate);
    void controllerReady(HRESULT result, ICoreWebView2Controller *created);
    void startFailed(HRESULT result);
    void configure();
    void reportLink(const QUrl &target, bool userGesture);
    void selectionMenu(ICoreWebView2ContextMenuRequestedEventArgs *args);
    void handOffFocus(QWidget *now);
    void removeTemporaryFolder();

    bool m_comInitialized = false;
};

// The window the web view is a child of: its HWND is the WebView2's parent window, and the bounds,
// visibility and focus of the web view follow it.
class LiveWindow : public QWindow
{
public:
    explicit LiveWindow(Native *state)
        : native(state)
    {
    }
    ~LiveWindow() override
    {
        if (m_top) {
            m_top->removeEventFilter(this);
        }
        // Deleted without destroy() first: the web view closes while the HWND is still there.
        if (native) native->windowGone();
    }

    Native *native = nullptr;

protected:
    void exposeEvent(QExposeEvent *event) override
    {
        QWindow::exposeEvent(event);
        if (native) native->syncBounds();
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QWindow::resizeEvent(event);
        if (native) native->syncBounds();
    }
    void moveEvent(QMoveEvent *event) override
    {
        QWindow::moveEvent(event);
        if (native) native->parentMoved();
    }
    void showEvent(QShowEvent *event) override
    {
        QWindow::showEvent(event);
        if (native) {
            native->syncBounds();
            native->syncVisibility();
        }
    }
    void hideEvent(QHideEvent *event) override
    {
        QWindow::hideEvent(event);
        if (native) native->syncVisibility();
    }
    // The container gave this window the focus (HtmlDocumentView::focusPage): the page takes it.
    void focusInEvent(QFocusEvent *event) override
    {
        QWindow::focusInEvent(event);
        if (native) native->takeFocus();
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::PlatformSurface && native) {
            // Before the HWND goes (the container's destroy()), the web view in it closes.
            const auto type = static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType();
            if (type == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                native->windowGone();
            } else if (type == QPlatformSurfaceEvent::SurfaceCreated) {
                native->windowBack();
            }
        } else if (event->type() == QEvent::ParentChange) {
            watchTopLevel();
        }
        return QWindow::event(event);
    }
    // The window that shows this one moved: WebView2 places its popups (a <select>'s list, the IME)
    // by it.
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == m_top && event->type() == QEvent::Move && native) {
            native->parentMoved();
        }
        return QWindow::eventFilter(object, event);
    }

private:
    void watchTopLevel()
    {
        QWindow *top = this;
        while (top->parent()) {
            top = top->parent();
        }
        if (top == this) {
            top = nullptr;
        }
        if (top != m_top) {
            if (m_top) m_top->removeEventFilter(this);
            m_top = top;
            if (m_top) m_top->installEventFilter(this);
        }
        if (native) native->parentMoved();
    }

    QPointer<QWindow> m_top;
};

Native::Native(Thoughts::HtmlLiveWebView *view)
    : owner(view)
{
    // WebView2 needs a single-threaded apartment (Qt's windows platform has made one already).
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(apartment);
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        connect(app, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            handOffFocus(now);
        });
    }
}

Native::~Native()
{
    close();
    if (m_comInitialized) {
        CoUninitialize();
    }
}

void Native::start()
{
    if (starting || controller || !hwnd) {
        return;
    }
    starting = true;
    ++generation;
    createEnvironment(sharedDataFolder(), true);
}

void Native::createEnvironment(const QString &folder, bool inPrivate)
{
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    // Media plays when the reader asks, as WKWebView's mediaTypesRequiringUserActionForPlayback.
    if (options) options->put_AdditionalBrowserArguments(L"--autoplay-policy=user-gesture-required");
    QPointer<Native> guard(this);
    const int started = generation;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        wide(folder),
        options.Get(),
        handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([guard, started, inPrivate](HRESULT status, ICoreWebView2Environment *created) -> HRESULT {
            if (guard && guard->generation == started) {
                guard->environmentReady(status, created, inPrivate);
            }
            return S_OK;
        }).Get());
    if (FAILED(result)) {
        startFailed(result);
    }
}

void Native::environmentReady(HRESULT result, ICoreWebView2Environment *created, bool inPrivate)
{
    if (FAILED(result) || !created) {
        startFailed(FAILED(result) ? result : E_FAIL);
        return;
    }
    environment = created;
    QPointer<Native> guard(this);
    const int started = generation;
    auto done = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([guard, started](HRESULT status, ICoreWebView2Controller *made) -> HRESULT {
        if (guard && guard->generation == started) {
            guard->controllerReady(status, made);
        } else if (made) {
            // A view closed meanwhile: its controller too.
            made->Close();
        }
        return S_OK;
    });
    HRESULT asked = E_FAIL;
    ComPtr<ICoreWebView2Environment10> withOptions;
    ComPtr<ICoreWebView2ControllerOptions> controllerOptions;
    if (inPrivate && SUCCEEDED(environment.As(&withOptions)) && SUCCEEDED(withOptions->CreateCoreWebView2ControllerOptions(&controllerOptions)) && controllerOptions
        && SUCCEEDED(controllerOptions->put_IsInPrivateModeEnabled(TRUE))) {
        // A profile of the view's own: no other page shares its InPrivate session.
        static int views = 0;
        profileName = QStringLiteral("live-%1-%2").arg(QCoreApplication::applicationPid()).arg(++views);
        controllerOptions->put_ProfileName(wide(profileName));
        asked = withOptions->CreateCoreWebView2ControllerWithOptions(hwnd, controllerOptions.Get(), done.Get());
    } else if (inPrivate) {
        // A WebView2 runtime without InPrivate: a user-data folder of the view's own.
        environment.Reset();
        const QString folder = temporaryDataFolder(&temporaryLock);
        if (folder.isEmpty()) {
            startFailed(E_ACCESSDENIED);
            return;
        }
        temporaryFolder = folder;
        createEnvironment(folder, false);
        return;
    } else {
        asked = environment->CreateCoreWebView2Controller(hwnd, done.Get());
    }
    if (FAILED(asked)) {
        startFailed(asked);
    }
}

void Native::controllerReady(HRESULT result, ICoreWebView2Controller *created)
{
    starting = false;
    if (FAILED(result) || !created) {
        startFailed(FAILED(result) ? result : E_FAIL);
        return;
    }
    controller = created;
    if (FAILED(controller->get_CoreWebView2(&web)) || !web) {
        controller->Close();
        controller.Reset();
        startFailed(E_NOINTERFACE);
        return;
    }
    configure();
    syncBounds();
    syncVisibility();
    if (pendingLoad) {
        navigate(pending);
    }
}

void Native::startFailed(HRESULT result)
{
    starting = false;
    environment.Reset();
    qWarning().noquote() << "Live page web view: WebView2 could not start:" << hresultText(result);
    if (pendingLoad) {
        pendingLoad = false;
        const QString text = QStringLiteral("The web view could not be started (%1).").arg(hresultText(result));
        later(owner, [text](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(text); });
    }
}

void Native::configure()
{
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(web->get_Settings(&settings)) && settings) {
        settings->put_IsScriptEnabled(TRUE);
        // No page bridge: nothing the page posts reaches Thoughts, no host objects.
        settings->put_IsWebMessageEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        // As WKWebView without its UI delegate's panels: no alert, confirm or prompt.
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        // A failure shows Thoughts' message, not the web view's error page.
        settings->put_IsBuiltInErrorPageEnabled(FALSE);
        settings->put_IsZoomControlEnabled(TRUE);
        ComPtr<ICoreWebView2Settings4> forms;
        if (SUCCEEDED(settings.As(&forms))) {
            forms->put_IsPasswordAutosaveEnabled(FALSE);
            forms->put_IsGeneralAutofillEnabled(FALSE);
        }
    }
    ComPtr<ICoreWebView2Controller4> drops;
    if (SUCCEEDED(controller.As(&drops))) {
        // A file dropped on the page is not opened in it.
        drops->put_AllowExternalDrop(FALSE);
    }
    ComPtr<ICoreWebView2_13> withProfile;
    if (SUCCEEDED(web.As(&withProfile))) {
        withProfile->get_Profile(&profile);
    }

    QPointer<Native> guard(this);
    EventRegistrationToken token{};
    web->add_NavigationStarting(handler<ICoreWebView2NavigationStartingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                    if (!args) return S_OK;
                                    if (!guard || !guard->owner) {
                                        args->put_Cancel(TRUE);
                                        return S_OK;
                                    }
                                    LPWSTR uri = nullptr;
                                    args->get_Uri(&uri);
                                    const QUrl target(taken(uri));
                                    BOOL user = FALSE;
                                    BOOL redirected = FALSE;
                                    args->get_IsUserInitiated(&user);
                                    args->get_IsRedirected(&redirected);
                                    if (guard->owner->nativeAllows(target, true)) {
                                        if (!redirected) later(guard->owner, [](Thoughts::HtmlLiveWebView *self) { self->nativeStarted(); });
                                        return S_OK;
                                    }
                                    args->put_Cancel(TRUE);
                                    // A server's redirect elsewhere is not the reader's click.
                                    guard->reportLink(target, user && !redirected);
                                    return S_OK;
                                }).Get(),
                                &token);
    web->add_FrameNavigationStarting(handler<ICoreWebView2NavigationStartingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                         if (!args) return S_OK;
                                         LPWSTR uri = nullptr;
                                         args->get_Uri(&uri);
                                         const QUrl target(taken(uri));
                                         if (!guard || !guard->owner || !guard->owner->nativeAllows(target, false)) {
                                             args->put_Cancel(TRUE);
                                         }
                                         return S_OK;
                                     }).Get(),
                                     &token);
    web->add_ContentLoading(handler<ICoreWebView2ContentLoadingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ContentLoadingEventArgs *args) -> HRESULT {
                                BOOL errorPage = FALSE;
                                UINT64 id = 0;
                                if (args) {
                                    args->get_IsErrorPage(&errorPage);
                                    args->get_NavigationId(&id);
                                }
                                if (!guard || errorPage) return S_OK;
                                guard->committedNavigation = id;
                                guard->shown = true;
                                later(guard->owner, [](Thoughts::HtmlLiveWebView *self) { self->nativeCommitted(); });
                                return S_OK;
                            }).Get(),
                            &token);
    web->add_NavigationCompleted(handler<ICoreWebView2NavigationCompletedEventHandler>([guard](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                     if (!guard || !args) return S_OK;
                                     BOOL success = FALSE;
                                     COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                     UINT64 id = 0;
                                     args->get_IsSuccess(&success);
                                     args->get_WebErrorStatus(&status);
                                     args->get_NavigationId(&id);
                                     const bool committed = id == guard->committedNavigation;
                                     // A navigation this view stopped or replaced (a link elsewhere, a
                                     // new load), or stopped after the page began to show: what it
                                     // shows stays.
                                     if (!success && status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
                                     // A new page showed (the server's own page of an error, 404, too, as
                                     // WebKit says).
                                     if (committed) {
                                         LPWSTR source = nullptr;
                                         if (sender) sender->get_Source(&source);
                                         const QUrl url(taken(source));
                                         later(guard->owner, [url](Thoughts::HtmlLiveWebView *self) { self->nativeFinished(url); });
                                         return S_OK;
                                     }
                                     // Within the page (#hash, pushState): no new page, as on the Mac.
                                     if (success) return S_OK;
                                     guard->shown = false;
                                     const QString text = navigationError(status);
                                     later(guard->owner, [text](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(text); });
                                     return S_OK;
                                 }).Get(),
                                 &token);
    web->add_NewWindowRequested(handler<ICoreWebView2NewWindowRequestedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                    if (!args) return S_OK;
                                    // Never a second window: without NewWindow, window.open returns null.
                                    args->put_Handled(TRUE);
                                    LPWSTR uri = nullptr;
                                    args->get_Uri(&uri);
                                    const QUrl target(taken(uri));
                                    BOOL user = FALSE;
                                    args->get_IsUserInitiated(&user);
                                    // As javaScriptCanOpenWindowsAutomatically = NO: only from the reader's click.
                                    if (!guard || !guard->owner || !user) return S_OK;
                                    if (guard->owner->nativeAllows(target, true)) {
                                        // The page itself when it stays in its origin.
                                        QMetaObject::invokeMethod(
                                            guard.data(),
                                            [guard, target] {
                                                if (guard) guard->navigate(target);
                                            },
                                            Qt::QueuedConnection);
                                    } else {
                                        guard->reportLink(target, true);
                                    }
                                    return S_OK;
                                }).Get(),
                                &token);
    web->add_PermissionRequested(handler<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                                     // No camera, microphone, location, notifications or clipboard reading.
                                     if (args) args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                     return S_OK;
                                 }).Get(),
                                 &token);
    web->add_ScriptDialogOpening(handler<ICoreWebView2ScriptDialogOpeningEventHandler>([](ICoreWebView2 *, ICoreWebView2ScriptDialogOpeningEventArgs *args) -> HRESULT {
                                     COREWEBVIEW2_SCRIPT_DIALOG_KIND kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
                                     // Leaving the page is never held up by it; nothing else is shown.
                                     if (args && SUCCEEDED(args->get_Kind(&kind)) && kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD) args->Accept();
                                     return S_OK;
                                 }).Get(),
                                 &token);
    web->add_ProcessFailed(handler<ICoreWebView2ProcessFailedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                               COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                               if (!guard || !args || FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
                               if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                   // The web view is gone: Reload starts a new one.
                                   QMetaObject::invokeMethod(
                                       guard.data(),
                                       [guard] {
                                           if (guard) guard->close();
                                       },
                                       Qt::QueuedConnection);
                               } else if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
                                   // A frame's, the GPU's or a helper's process: the browser recovers.
                                   return S_OK;
                               }
                               guard->shown = false;
                               later(guard->owner, [](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(QStringLiteral("The page stopped working.")); });
                               return S_OK;
                           }).Get(),
                           &token);
    ComPtr<ICoreWebView2_4> withDownloads;
    if (SUCCEEDED(web.As(&withDownloads))) {
        withDownloads->add_DownloadStarting(handler<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
                                                // Downloads are not this view's (<a download>, a file it cannot show).
                                                if (args) {
                                                    args->put_Cancel(TRUE);
                                                    args->put_Handled(TRUE);
                                                }
                                                return S_OK;
                                            }).Get(),
                                            &token);
    }
    ComPtr<ICoreWebView2_11> withMenus;
    if (SUCCEEDED(web.As(&withMenus))) {
        withMenus->add_ContextMenuRequested(handler<ICoreWebView2ContextMenuRequestedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ContextMenuRequestedEventArgs *args) -> HRESULT {
                                                if (guard && args) guard->selectionMenu(args);
                                                return S_OK;
                                            }).Get(),
                                            &token);
    }
    ComPtr<ICoreWebView2_18> withSchemes;
    if (SUCCEEDED(web.As(&withSchemes))) {
        withSchemes->add_LaunchingExternalUriScheme(handler<ICoreWebView2LaunchingExternalUriSchemeEventHandler>([guard](ICoreWebView2 *, ICoreWebView2LaunchingExternalUriSchemeEventArgs *args) -> HRESULT {
                                                        // No other application is started by the web view: a mailto
                                                        // link is reported, as one elsewhere.
                                                        if (!args) return S_OK;
                                                        args->put_Cancel(TRUE);
                                                        LPWSTR uri = nullptr;
                                                        args->get_Uri(&uri);
                                                        const QUrl target(taken(uri));
                                                        BOOL user = FALSE;
                                                        args->get_IsUserInitiated(&user);
                                                        if (guard) guard->reportLink(target, user);
                                                        return S_OK;
                                                    }).Get(),
                                                    &token);
    }
    controller->add_GotFocus(handler<ICoreWebView2FocusChangedEventHandler>([guard](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
                                 if (guard) guard->focused = true;
                                 return S_OK;
                             }).Get(),
                             &token);
    controller->add_LostFocus(handler<ICoreWebView2FocusChangedEventHandler>([guard](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
                                  if (guard) guard->focused = false;
                                  return S_OK;
                              }).Get(),
                              &token);
}

void Native::reportLink(const QUrl &target, bool userGesture)
{
    QPointer<Native> guard(this);
    later(owner, [guard, target, userGesture](Thoughts::HtmlLiveWebView *self) {
        if (guard) guard->linkGesture = userGesture ? 1 : 0;
        self->nativeLinkRequested(target);
        if (guard) guard->linkGesture = -1;
    });
}

void Native::selectionMenu(ICoreWebView2ContextMenuRequestedEventArgs *args)
{
    // The menu of selected text (it has the web view's Copy of it; a link's or an image's Copy is
    // named otherwise), from the reader's right click only.
    ComPtr<ICoreWebView2ContextMenuTarget> target;
    BOOL selected = FALSE;
    if (FAILED(args->get_ContextMenuTarget(&target)) || !target || FAILED(target->get_HasSelection(&selected)) || !selected) {
        return;
    }
    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
    UINT32 count = 0;
    if (FAILED(args->get_MenuItems(&items)) || !items || FAILED(items->get_Count(&count))) {
        return;
    }
    int copy = -1;
    for (UINT32 index = 0; index < count && copy < 0; ++index) {
        ComPtr<ICoreWebView2ContextMenuItem> item;
        LPWSTR name = nullptr;
        if (SUCCEEDED(items->GetValueAtIndex(index, &item)) && item && SUCCEEDED(item->get_Name(&name)) && taken(name) == QLatin1String("copy")) {
            copy = int(index);
        }
    }
    ComPtr<ICoreWebView2Environment9> factory;
    if (copy < 0 || !environment || FAILED(environment.As(&factory))) {
        return;
    }
    // One menu: Thoughts' items, then the web view's own Copy; the selection is read when one is
    // chosen.
    for (UINT32 index = count; index-- > 0;) {
        if (int(index) != copy) items->RemoveValueAtIndex(index);
    }
    if (!owner || !owner->selectionFeedback()) {
        return;
    }
    const struct {
        const wchar_t *title;
        const char *choice;
    } choices[] = {{L"Like", "like"}, {L"Dislike", "dislike"}, {L"Question\u2026", "question"}, {L"Comment\u2026", "comment"}};
    UINT32 at = 0;
    QPointer<Native> guard(this);
    for (const auto &entry : choices) {
        ComPtr<ICoreWebView2ContextMenuItem> item;
        if (FAILED(factory->CreateContextMenuItem(entry.title, nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &item)) || !item) {
            continue;
        }
        const QString picked = QString::fromLatin1(entry.choice);
        EventRegistrationToken token{};
        item->add_CustomItemSelected(handler<ICoreWebView2CustomItemSelectedEventHandler>([guard, picked](ICoreWebView2ContextMenuItem *, IUnknown *) -> HRESULT {
                                         if (guard) later(guard->owner, [picked](Thoughts::HtmlLiveWebView *self) { self->nativeSelectionChoice(picked); });
                                         return S_OK;
                                     }).Get(),
                                     &token);
        items->InsertValueAtIndex(at++, item.Get());
    }
    ComPtr<ICoreWebView2ContextMenuItem> separator;
    if (at > 0 && SUCCEEDED(factory->CreateContextMenuItem(L"", nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR, &separator)) && separator) {
        items->InsertValueAtIndex(at, separator.Get());
    }
}

void Native::handOffFocus(QWidget *now)
{
    // Focus given to a Qt widget of this window, or to a popup over it (the feedback editor's
    // comment), while the web view has the keyboard: keys must leave the web view. Qt sends a
    // popup its keys through the window under it, which the web view's HWND otherwise keeps.
    if (!now || !focused || !hwnd || now->inherits("QWindowContainer")) {
        return;
    }
    QWidget *top = now->window();
    while (top && top->windowType() == Qt::Popup && top->parentWidget()) {
        top = top->parentWidget()->window();
    }
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!top || !root || !top->windowHandle() || HWND(top->windowHandle()->winId()) != root) {
        return;
    }
    SetFocus(root);
}

void Native::navigate(const QUrl &url)
{
    if (!web) {
        pending = url;
        pendingLoad = true;
        start();
        return;
    }
    pendingLoad = false;
    // The full address: WebView2 scrolls to and marks a text fragment (#:~:text=…) by itself.
    const HRESULT result = web->Navigate(wide(url.toString(QUrl::FullyEncoded)));
    if (FAILED(result)) {
        shown = false;
        const QString text = result == E_INVALIDARG ? QStringLiteral("The address is not valid.") : QStringLiteral("The page could not be loaded (%1).").arg(hresultText(result));
        later(owner, [text](Thoughts::HtmlLiveWebView *self) { self->nativeFailed(text); });
    }
}

void Native::syncBounds()
{
    if (!controller || !hwnd) {
        return;
    }
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    controller->put_Bounds(bounds);
}

void Native::syncVisibility()
{
    if (controller && window) {
        controller->put_IsVisible(window->isVisible() ? TRUE : FALSE);
    }
}

void Native::takeFocus()
{
    // Only when this window itself has the keyboard (the page may have it already).
    if (controller && hwnd && GetFocus() == hwnd) {
        controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void Native::parentMoved()
{
    if (controller) {
        controller->NotifyParentWindowPositionChanged();
    }
}

void Native::windowGone()
{
    close();
    hwnd = nullptr;
    windowLost = true;
}

void Native::windowBack()
{
    // The same window again with a new HWND: a new web view in it, at the page's address.
    if (!windowLost || !window) {
        return;
    }
    windowLost = false;
    hwnd = HWND(window->winId());
    if (owner && owner->url().isValid()) {
        pending = owner->url();
        pendingLoad = true;
    }
    start();
}

void Native::close()
{
    ++generation;
    starting = false;
    focused = false;
    shown = false;
    committedNavigation = NoNavigation;
    if (controller) {
        controller->Close();
    }
    // The view's own profile goes with it (its folder when the browser quits).
    ComPtr<ICoreWebView2Profile8> deletable;
    if (profile && SUCCEEDED(profile.As(&deletable))) {
        deletable->Delete();
    }
    profile.Reset();
    web.Reset();
    controller.Reset();
    environment.Reset();
    removeTemporaryFolder();
}

void Native::removeTemporaryFolder()
{
    if (temporaryFolder.isEmpty()) {
        return;
    }
    // After its browser has quit (a few seconds); else at the next start of a view without InPrivate.
    const QString folder = temporaryFolder;
    std::shared_ptr<QLockFile> lock(temporaryLock.release());
    temporaryFolder.clear();
    QTimer::singleShot(5000, QCoreApplication::instance(), [folder, lock] {
        lock->unlock();
        QDir(folder).removeRecursively();
    });
}

void Native::devTools(const QString &method, const QJsonObject &params, std::function<void(bool, const QJsonObject &, const QString &)> reply)
{
    if (!web) {
        reply(false, {}, QStringLiteral("there is no web view here"));
        return;
    }
    auto answered = std::make_shared<bool>(false);
    const QString json = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    const HRESULT result = web->CallDevToolsProtocolMethod(
        wide(method),
        wide(json),
        handler<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([reply, answered](HRESULT status, LPCWSTR text) -> HRESULT {
            if (*answered) return S_OK;
            *answered = true;
            const QJsonObject object = QJsonDocument::fromJson(QString::fromWCharArray(text ? text : L"").toUtf8()).object();
            const bool ok = SUCCEEDED(status);
            const QString error = ok ? QString() : object.value(QStringLiteral("message")).toString(hresultText(status));
            // After WebView2's call returns: the receiver may ask the user (a dialog).
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [reply, ok, object, error] { reply(ok, object, error); },
                Qt::QueuedConnection);
            return S_OK;
        }).Get());
    if (FAILED(result) && !*answered) {
        *answered = true;
        reply(false, {}, hresultText(result));
    }
}

void Native::callIsolated(const QString &expression, std::function<void(bool, const QString &, const QString &)> done)
{
    // The main frame's isolated world "thoughts" (one per document, made again after a navigation).
    QPointer<Native> guard(this);
    const int started = generation;
    const auto closed = [guard, started] { return !guard || guard->generation != started || !guard->web; };
    devTools(QStringLiteral("Page.getFrameTree"), {}, [guard, closed, expression, done](bool ok, const QJsonObject &reply, const QString &error) {
        if (closed()) {
            done(false, QString(), QStringLiteral("the page was closed"));
            return;
        }
        const QString frame = reply.value(QStringLiteral("frameTree")).toObject().value(QStringLiteral("frame")).toObject().value(QStringLiteral("id")).toString();
        if (!ok || frame.isEmpty()) {
            done(false, QString(), error.isEmpty() ? QStringLiteral("the page has no document") : error);
            return;
        }
        const QJsonObject world{{QStringLiteral("frameId"), frame}, {QStringLiteral("worldName"), QStringLiteral("thoughts")}};
        guard->devTools(QStringLiteral("Page.createIsolatedWorld"), world, [guard, closed, expression, done](bool created, const QJsonObject &made, const QString &why) {
            if (closed()) {
                done(false, QString(), QStringLiteral("the page was closed"));
                return;
            }
            const int context = made.value(QStringLiteral("executionContextId")).toInt();
            if (!created || context == 0) {
                done(false, QString(), why.isEmpty() ? QStringLiteral("no script world for Thoughts in this page") : why);
                return;
            }
            const QJsonObject evaluate{{QStringLiteral("expression"), expression},
                                       {QStringLiteral("contextId"), context},
                                       {QStringLiteral("awaitPromise"), true},
                                       {QStringLiteral("returnByValue"), true}};
            guard->devTools(QStringLiteral("Runtime.evaluate"), evaluate, [guard, done](bool evaluated, const QJsonObject &answer, const QString &failure) {
                if (!guard) {
                    done(false, QString(), QStringLiteral("the page was closed"));
                    return;
                }
                if (!evaluated) {
                    done(false, QString(), failure);
                    return;
                }
                const QJsonObject exception = answer.value(QStringLiteral("exceptionDetails")).toObject();
                if (!exception.isEmpty()) {
                    QString thrown = exception.value(QStringLiteral("exception")).toObject().value(QStringLiteral("description")).toString();
                    if (thrown.isEmpty()) thrown = exception.value(QStringLiteral("text")).toString();
                    done(false, QString(), thrown.isEmpty() ? QStringLiteral("A JavaScript exception occurred") : thrown);
                    return;
                }
                const QJsonObject value = answer.value(QStringLiteral("result")).toObject();
                done(true, value.value(QStringLiteral("type")).toString() == QLatin1String("string") ? value.value(QStringLiteral("value")).toString() : QString(), QString());
            });
        });
    });
}

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
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
        return false;
    }
    // The WebView2 runtime (Evergreen, part of Windows 11 and of updated Windows 10) is installed.
    static bool installed = false;
    if (!installed) {
        LPWSTR version = nullptr;
        installed = SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version && *version;
        if (version) CoTaskMemFree(version);
    }
    return installed;
}

HtmlLiveWebView::HtmlLiveWebView(QObject *parent)
    : QObject(parent)
{
    if (!available()) {
        return;
    }
    auto *state = new Native(this);
    m_native = state;
    auto *window = new LiveWindow(state);
    state->window = window;
    m_window = window;
    // The HWND now (the page loads before the container shows it); the container makes it a child
    // of the reader's window.
    state->hwnd = HWND(window->winId());
    state->start();
}

HtmlLiveWebView::~HtmlLiveWebView()
{
    Native *state = native(m_native);
    if (!state) {
        return;
    }
    state->owner = nullptr;
    // The web view closes while its window is still there.
    state->close();
    if (m_window) {
        static_cast<LiveWindow *>(m_window.data())->native = nullptr;
        // The container owns the window; without one (never shown) it is ours.
        if (!m_window->parent()) {
            delete m_window.data();
        }
    }
    delete state;
    m_native = nullptr;
}

void HtmlLiveWebView::load(const QUrl &url)
{
    m_url = url;
    Native *state = native(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    state->navigate(url);
}

void HtmlLiveWebView::reload()
{
    Native *state = native(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    // After a failure there is no page to reload: the address again.
    if (!state->web || !state->shown) {
        load(m_url);
        return;
    }
    state->web->Reload();
}

void HtmlLiveWebView::callAsync(const QString &body, const QJsonObject &args, std::function<void(bool, const QString &, const QString &)> done)
{
    Native *state = native(m_native);
    if (!state || !state->web) {
        done(false, QString(), QStringLiteral("there is no web view here"));
        return;
    }
    // As callAsyncJavaScript: the body of an async function of argsJson, its promise awaited.
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    const QString expression = QStringLiteral("(async function (argsJson) {\n") + body + QStringLiteral("\n})(") + jsString(json) + QStringLiteral(")");
    state->callIsolated(expression, std::move(done));
}

void HtmlLiveWebView::snapshot(std::function<void(const QImage &)> done)
{
    Native *state = native(m_native);
    ComPtr<IStream> stream;
    if (!state || !state->web || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        done(QImage());
        return;
    }
    auto answered = std::make_shared<bool>(false);
    auto callback = done;
    const HRESULT result = state->web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                                                      stream.Get(),
                                                      handler<ICoreWebView2CapturePreviewCompletedHandler>([stream, callback, answered](HRESULT status) -> HRESULT {
                                                          if (*answered) return S_OK;
                                                          *answered = true;
                                                          QImage image;
                                                          if (SUCCEEDED(status)) image = QImage::fromData(streamBytes(stream.Get()), "PNG");
                                                          QMetaObject::invokeMethod(
                                                              QCoreApplication::instance(),
                                                              [callback, image] { callback(image); },
                                                              Qt::QueuedConnection);
                                                          return S_OK;
                                                      }).Get());
    if (FAILED(result) && !*answered) {
        *answered = true;
        done(QImage());
    }
}

void HtmlLiveWebView::copySelection()
{
    Native *state = native(m_native);
    if (!state || !state->web) {
        return;
    }
    // The web view's own Copy, as its menu would have done: Ctrl+C with its copy command, in the
    // frame (or form field) that has the selection.
    const auto ignore = [](bool, const QJsonObject &, const QString &) { };
    QJsonObject key{{QStringLiteral("modifiers"), 2},
                    {QStringLiteral("windowsVirtualKeyCode"), 0x43},
                    {QStringLiteral("nativeVirtualKeyCode"), 0x43},
                    {QStringLiteral("code"), QStringLiteral("KeyC")},
                    {QStringLiteral("key"), QStringLiteral("c")}};
    QJsonObject down = key;
    down.insert(QStringLiteral("type"), QStringLiteral("rawKeyDown"));
    down.insert(QStringLiteral("commands"), QJsonArray{QStringLiteral("copy")});
    key.insert(QStringLiteral("type"), QStringLiteral("keyUp"));
    state->devTools(QStringLiteral("Input.dispatchKeyEvent"), down, ignore);
    state->devTools(QStringLiteral("Input.dispatchKeyEvent"), key, ignore);
}

bool HtmlLiveWebView::hasNativeFocus() const
{
    Native *state = native(m_native);
    return state && state->focused;
}

qint64 HtmlLiveWebView::millisecondsSinceUserInput() const
{
    // The web view's input goes to its browser's process: the session's last input, while the
    // window that shows the view is the one in front (Windows counts pointer moves too).
    Native *state = native(m_native);
    if (!state || !state->hwnd) {
        return -1;
    }
    const HWND root = GetAncestor(state->hwnd, GA_ROOT);
    LASTINPUTINFO input{};
    input.cbSize = sizeof(input);
    if (!root || GetForegroundWindow() != root || !GetLastInputInfo(&input)) {
        return -1;
    }
    return qint64(DWORD(GetTickCount() - input.dwTime));
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
    // The page's own script can navigate too: only the reader's click counts as theirs (WebView2
    // says which it was; else the time since the reader's last input, as on the Mac).
    Native *state = native(m_native);
    const bool userGesture = state && state->linkGesture >= 0 ? state->linkGesture == 1 : HtmlDocumentView::isUserGesture(millisecondsSinceUserInput());
    Q_EMIT linkRequested(url, userGesture);
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
