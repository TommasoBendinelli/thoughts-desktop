// SPDX-License-Identifier: GPL-2.0-or-later
// Microsoft Edge WebView2 in a QWindow (webview2host_win.h).
#include "webview2host_win.h"

#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLockFile>
#include <QPlatformSurfaceEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <objbase.h>
#include <wrl/implements.h>

// Qt defines __has_attribute as 0 for a compiler without it (MSVC), and the options header takes
// any definition as clang's: its __attribute__((annotate(…))) does not compile with MSVC. Without
// Qt's definition while it is read, MSVC skips that branch; clang keeps its own.
#pragma push_macro("__has_attribute")
#ifdef _MSC_VER
#ifndef __clang__
#undef __has_attribute
#endif
#endif
#include <WebView2EnvironmentOptions.h>
#pragma pop_macro("__has_attribute")

namespace Thoughts::WebView2
{
namespace
{
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

// The browser's own files, shared by the web views of this computer's account: no browsing data,
// every web view is InPrivate. All of them use the same environment options (WebView2 requires it
// of one folder).
QString sharedDataFolder()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/webview2");
    QDir().mkpath(folder);
    return QDir::toNativeSeparators(folder);
}

QString temporaryPrefix()
{
    return QStringLiteral("thoughts-webview2-");
}

QString temporaryLockName()
{
    return QStringLiteral("/thoughts.lock");
}

// Without InPrivate: a user-data folder of the web view's own. Those of web views gone (their
// lock free) are removed first.
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
}

const wchar_t *wide(const QString &text)
{
    return reinterpret_cast<const wchar_t *>(text.utf16());
}

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

bool available()
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

template<typename Run>
void Host::post(Run run)
{
    QPointer<Host> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, run] {
            if (guard) run();
        },
        Qt::QueuedConnection);
}

Host::Host(QWindow *window, const Options &options)
    : m_window(window)
    , m_options(options)
{
    // WebView2 needs a single-threaded apartment (Qt's windows platform has made one already).
    m_comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        connect(app, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            handOffFocus(now);
        });
    }
    // The HWND now (the page loads before the container shows it); the container makes it a child
    // of the reader's window.
    m_hwnd = HWND(window->winId());
    start();
}

Host::~Host()
{
    close();
    if (m_top) m_top->removeEventFilter(this);
    if (m_comInitialized) CoUninitialize();
}

void Host::windowEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::Expose:
    case QEvent::Resize:
        syncBounds();
        break;
    case QEvent::Show:
    case QEvent::Hide:
        syncBounds();
        syncVisibility();
        break;
    case QEvent::Move:
        parentMoved();
        break;
    // The container gave the window the focus (focusPage): the page takes it.
    case QEvent::FocusIn:
        takeFocus();
        break;
    case QEvent::ParentChange:
        watchTopLevel();
        break;
    case QEvent::PlatformSurface:
        // Before the HWND goes (the container's destroy()), the web view in it closes.
        if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            windowGone();
        } else {
            windowBack();
        }
        break;
    default:
        break;
    }
}

void Host::windowDestroying()
{
    // Deleted without destroy() first: the web view closes while the HWND is still there.
    windowGone();
    if (m_top) m_top->removeEventFilter(this);
    m_top = nullptr;
    m_window = nullptr;
}

bool Host::eventFilter(QObject *object, QEvent *event)
{
    // The window that shows the web view moved: WebView2 places its popups (a <select>'s list,
    // the IME) by it.
    if (object == m_top && event->type() == QEvent::Move) parentMoved();
    return QObject::eventFilter(object, event);
}

void Host::watchTopLevel()
{
    if (!m_window) return;
    QWindow *top = m_window;
    while (top->parent()) {
        top = top->parent();
    }
    if (top == m_window) top = nullptr;
    if (top != m_top) {
        if (m_top) m_top->removeEventFilter(this);
        m_top = top;
        if (m_top) m_top->installEventFilter(this);
    }
    parentMoved();
}

void Host::start()
{
    if (m_starting || m_controller || !m_hwnd) {
        return;
    }
    m_starting = true;
    ++m_generation;
    createEnvironment(sharedDataFolder(), true);
}

void Host::createEnvironment(const QString &folder, bool inPrivate)
{
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    // Media plays when the reader asks, as WKWebView's mediaTypesRequiringUserActionForPlayback.
    if (options) options->put_AdditionalBrowserArguments(L"--autoplay-policy=user-gesture-required");
    QPointer<Host> guard(this);
    const int generation = m_generation;
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        wide(folder),
        options.Get(),
        handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([guard, generation, inPrivate](HRESULT status, ICoreWebView2Environment *created) -> HRESULT {
            if (guard && guard->m_generation == generation) guard->environmentReady(status, created, inPrivate);
            return S_OK;
        }).Get());
    if (FAILED(result)) startFailed(result);
}

void Host::environmentReady(HRESULT result, ICoreWebView2Environment *created, bool inPrivate)
{
    if (FAILED(result) || !created) {
        startFailed(FAILED(result) ? result : E_FAIL);
        return;
    }
    m_environment = created;
    QPointer<Host> guard(this);
    const int generation = m_generation;
    auto done = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([guard, generation](HRESULT status, ICoreWebView2Controller *made) -> HRESULT {
        if (guard && guard->m_generation == generation) {
            guard->controllerReady(status, made);
        } else if (made) {
            // A web view closed meanwhile: its controller too.
            made->Close();
        }
        return S_OK;
    });
    HRESULT asked = E_FAIL;
    ComPtr<ICoreWebView2Environment10> withOptions;
    ComPtr<ICoreWebView2ControllerOptions> controllerOptions;
    if (inPrivate && SUCCEEDED(m_environment.As(&withOptions)) && SUCCEEDED(withOptions->CreateCoreWebView2ControllerOptions(&controllerOptions)) && controllerOptions
        && SUCCEEDED(controllerOptions->put_IsInPrivateModeEnabled(TRUE))) {
        // A profile of the web view's own: no other page shares its InPrivate session.
        static int views = 0;
        m_profileName = QStringLiteral("view-%1-%2").arg(QCoreApplication::applicationPid()).arg(++views);
        controllerOptions->put_ProfileName(wide(m_profileName));
        asked = withOptions->CreateCoreWebView2ControllerWithOptions(m_hwnd, controllerOptions.Get(), done.Get());
    } else if (inPrivate) {
        // A WebView2 runtime without InPrivate: a user-data folder of the web view's own.
        m_environment.Reset();
        const QString folder = temporaryDataFolder(&m_temporaryLock);
        if (folder.isEmpty()) {
            startFailed(E_ACCESSDENIED);
            return;
        }
        m_temporaryFolder = folder;
        createEnvironment(folder, false);
        return;
    } else {
        asked = m_environment->CreateCoreWebView2Controller(m_hwnd, done.Get());
    }
    if (FAILED(asked)) startFailed(asked);
}

void Host::controllerReady(HRESULT result, ICoreWebView2Controller *created)
{
    m_starting = false;
    if (FAILED(result) || !created) {
        startFailed(FAILED(result) ? result : E_FAIL);
        return;
    }
    m_controller = created;
    if (FAILED(m_controller->get_CoreWebView2(&m_web)) || !m_web) {
        m_controller->Close();
        m_controller.Reset();
        startFailed(E_NOINTERFACE);
        return;
    }
    configure();
    applyColorScheme();
    syncBounds();
    syncVisibility();
    if (m_pendingLoad) navigate(m_pending);
}

void Host::startFailed(HRESULT result)
{
    m_starting = false;
    m_environment.Reset();
    qWarning().noquote() << "WebView2 could not start:" << hresultText(result);
    if (m_pendingLoad) {
        m_pendingLoad = false;
        const QString text = QStringLiteral("The web view could not be started (%1).").arg(hresultText(result));
        post([this, text] {
            if (failed) failed(text);
        });
    }
}

void Host::configure()
{
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_web->get_Settings(&settings)) && settings) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_IsWebMessageEnabled(m_options.webMessages ? TRUE : FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        // As WKWebView without its UI delegate's panels: no alert, confirm or prompt.
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        // A failure shows Thoughts' message, not the web view's error page.
        settings->put_IsBuiltInErrorPageEnabled(FALSE);
        settings->put_IsZoomControlEnabled(m_options.zoom ? TRUE : FALSE);
        ComPtr<ICoreWebView2Settings3> keys;
        if (SUCCEEDED(settings.As(&keys))) keys->put_AreBrowserAcceleratorKeysEnabled(m_options.browserKeys ? TRUE : FALSE);
        ComPtr<ICoreWebView2Settings4> forms;
        if (SUCCEEDED(settings.As(&forms))) {
            forms->put_IsPasswordAutosaveEnabled(FALSE);
            forms->put_IsGeneralAutofillEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Settings5> pinch;
        if (SUCCEEDED(settings.As(&pinch))) pinch->put_IsPinchZoomEnabled(m_options.zoom ? TRUE : FALSE);
    }
    ComPtr<ICoreWebView2Controller4> drops;
    // A file dropped on the page is not opened in it.
    if (SUCCEEDED(m_controller.As(&drops))) drops->put_AllowExternalDrop(FALSE);
    ComPtr<ICoreWebView2_13> withProfile;
    if (SUCCEEDED(m_web.As(&withProfile))) withProfile->get_Profile(&m_profile);

    QPointer<Host> guard(this);
    EventRegistrationToken token{};
    m_web->add_NavigationStarting(handler<ICoreWebView2NavigationStartingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                      if (!args) return S_OK;
                                      if (!guard) {
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
                                      if (guard->allowNavigation && !guard->allowNavigation(target, user, redirected)) {
                                          args->put_Cancel(TRUE);
                                          return S_OK;
                                      }
                                      if (!redirected) guard->post([guard, target] {
                                          if (guard->started) guard->started(target);
                                      });
                                      return S_OK;
                                  }).Get(),
                                  &token);
    m_web->add_FrameNavigationStarting(handler<ICoreWebView2NavigationStartingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                           if (!args) return S_OK;
                                           LPWSTR uri = nullptr;
                                           args->get_Uri(&uri);
                                           const QUrl target(taken(uri));
                                           if (!guard || (guard->allowFrameNavigation && !guard->allowFrameNavigation(target))) args->put_Cancel(TRUE);
                                           return S_OK;
                                       }).Get(),
                                       &token);
    m_web->add_ContentLoading(handler<ICoreWebView2ContentLoadingEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ContentLoadingEventArgs *args) -> HRESULT {
                                  BOOL errorPage = FALSE;
                                  UINT64 id = 0;
                                  if (args) {
                                      args->get_IsErrorPage(&errorPage);
                                      args->get_NavigationId(&id);
                                  }
                                  if (!guard || errorPage) return S_OK;
                                  guard->m_committed = id;
                                  guard->m_shown = true;
                                  guard->post([guard] {
                                      if (guard->committed) guard->committed();
                                  });
                                  return S_OK;
                              }).Get(),
                              &token);
    m_web->add_NavigationCompleted(handler<ICoreWebView2NavigationCompletedEventHandler>([guard](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                       if (!guard || !args) return S_OK;
                                       BOOL success = FALSE;
                                       COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                       UINT64 id = 0;
                                       args->get_IsSuccess(&success);
                                       args->get_WebErrorStatus(&status);
                                       args->get_NavigationId(&id);
                                       LPWSTR source = nullptr;
                                       if (sender) sender->get_Source(&source);
                                       const QUrl url(taken(source));
                                       // A navigation this web view stopped or replaced (a link elsewhere, a
                                       // new load), or stopped after the page began to show: what it shows
                                       // stays.
                                       if (!success && status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                           guard->post([guard, url] {
                                               if (guard->stopped) guard->stopped(url);
                                           });
                                           return S_OK;
                                       }
                                       // A new page showed (the server's own page of an error, 404, too, as
                                       // WebKit says).
                                       if (id == guard->m_committed) {
                                           guard->post([guard, url] {
                                               if (guard->finished) guard->finished(url);
                                           });
                                           return S_OK;
                                       }
                                       // Within the page (#hash, pushState): no new page, as on the Mac.
                                       if (success) return S_OK;
                                       guard->m_shown = false;
                                       const QString text = navigationError(status);
                                       guard->post([guard, text] {
                                           if (guard->failed) guard->failed(text);
                                       });
                                       return S_OK;
                                   }).Get(),
                                   &token);
    m_web->add_NewWindowRequested(handler<ICoreWebView2NewWindowRequestedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                      if (!args) return S_OK;
                                      // Never a second web view: without NewWindow, window.open returns null.
                                      args->put_Handled(TRUE);
                                      LPWSTR uri = nullptr;
                                      args->get_Uri(&uri);
                                      const QUrl target(taken(uri));
                                      BOOL user = FALSE;
                                      args->get_IsUserInitiated(&user);
                                      if (guard) guard->post([guard, target, user] {
                                          if (guard->newWindow) guard->newWindow(target, user);
                                      });
                                      return S_OK;
                                  }).Get(),
                                  &token);
    m_web->add_PermissionRequested(handler<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                                       // No camera, microphone, location, notifications or clipboard reading.
                                       if (args) args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                       return S_OK;
                                   }).Get(),
                                   &token);
    m_web->add_ScriptDialogOpening(handler<ICoreWebView2ScriptDialogOpeningEventHandler>([](ICoreWebView2 *, ICoreWebView2ScriptDialogOpeningEventArgs *args) -> HRESULT {
                                       COREWEBVIEW2_SCRIPT_DIALOG_KIND kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
                                       // Leaving the page is never held up by it; nothing else is shown.
                                       if (args && SUCCEEDED(args->get_Kind(&kind)) && kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD) args->Accept();
                                       return S_OK;
                                   }).Get(),
                                   &token);
    m_web->add_ProcessFailed(handler<ICoreWebView2ProcessFailedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                                 COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                                 if (!guard || !args || FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
                                 const bool browser = kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                 // A frame's, the GPU's or a helper's process: the browser recovers.
                                 if (!browser && kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) return S_OK;
                                 guard->m_shown = false;
                                 guard->post([guard, browser] {
                                     // The web view is gone: the next navigate() starts a new one.
                                     if (browser) guard->close();
                                     if (guard->failed) guard->failed(QStringLiteral("The page stopped working."));
                                 });
                                 return S_OK;
                             }).Get(),
                             &token);
    ComPtr<ICoreWebView2_4> withDownloads;
    if (SUCCEEDED(m_web.As(&withDownloads))) {
        withDownloads->add_DownloadStarting(handler<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
                                                // Downloads are not the web view's (<a download>, a file it cannot show).
                                                if (args) {
                                                    args->put_Cancel(TRUE);
                                                    args->put_Handled(TRUE);
                                                }
                                                return S_OK;
                                            }).Get(),
                                            &token);
    }
    ComPtr<ICoreWebView2_11> withMenus;
    if (SUCCEEDED(m_web.As(&withMenus))) {
        withMenus->add_ContextMenuRequested(handler<ICoreWebView2ContextMenuRequestedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2ContextMenuRequestedEventArgs *args) -> HRESULT {
                                                if (guard && args && guard->contextMenu) guard->contextMenu(args);
                                                return S_OK;
                                            }).Get(),
                                            &token);
    }
    ComPtr<ICoreWebView2_18> withSchemes;
    if (SUCCEEDED(m_web.As(&withSchemes))) {
        withSchemes->add_LaunchingExternalUriScheme(handler<ICoreWebView2LaunchingExternalUriSchemeEventHandler>([guard](ICoreWebView2 *, ICoreWebView2LaunchingExternalUriSchemeEventArgs *args) -> HRESULT {
                                                        // No other application is started by the web view itself.
                                                        if (!args) return S_OK;
                                                        args->put_Cancel(TRUE);
                                                        LPWSTR uri = nullptr;
                                                        args->get_Uri(&uri);
                                                        const QUrl target(taken(uri));
                                                        BOOL user = FALSE;
                                                        args->get_IsUserInitiated(&user);
                                                        if (guard) guard->post([guard, target, user] {
                                                            if (guard->externalScheme) guard->externalScheme(target, user);
                                                        });
                                                        return S_OK;
                                                    }).Get(),
                                                    &token);
    }
    if (m_options.webMessages) {
        m_web->add_WebMessageReceived(handler<ICoreWebView2WebMessageReceivedEventHandler>([guard](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                          if (!guard || !args) return S_OK;
                                          LPWSTR text = nullptr;
                                          LPWSTR source = nullptr;
                                          // Only strings (postMessage of a string), from the main frame.
                                          if (FAILED(args->TryGetWebMessageAsString(&text)) || !text) return S_OK;
                                          const QString body = taken(text);
                                          args->get_Source(&source);
                                          const QUrl from(taken(source));
                                          guard->post([guard, body, from] {
                                              if (guard->message) guard->message(body, from);
                                          });
                                          return S_OK;
                                      }).Get(),
                                      &token);
    }
    m_controller->add_GotFocus(handler<ICoreWebView2FocusChangedEventHandler>([guard](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
                                   if (!guard) return S_OK;
                                   guard->m_focused = true;
                                   // A click into the web view closes the open Qt popup, as a click outside it
                                   // does elsewhere (Qt never sees that click).
                                   guard->post([] {
                                       if (QWidget *popup = QApplication::activePopupWidget()) popup->close();
                                   });
                                   return S_OK;
                               }).Get(),
                               &token);
    m_controller->add_LostFocus(handler<ICoreWebView2FocusChangedEventHandler>([guard](ICoreWebView2Controller *, IUnknown *) -> HRESULT {
                                    if (guard) guard->m_focused = false;
                                    return S_OK;
                                }).Get(),
                                &token);
}

void Host::handOffFocus(QWidget *now)
{
    // Focus given to a Qt widget of this window, or to a popup over it (the feedback editor's
    // comment), while the web view has the keyboard: keys must leave the web view. Qt sends a
    // popup its keys through the window under it, which the web view's HWND otherwise keeps.
    if (!now || !m_focused || !m_hwnd || now->inherits("QWindowContainer")) {
        return;
    }
    QWidget *top = now->window();
    while (top && top->windowType() == Qt::Popup && top->parentWidget()) {
        top = top->parentWidget()->window();
    }
    const HWND root = GetAncestor(m_hwnd, GA_ROOT);
    if (!top || !root || !top->windowHandle() || HWND(top->windowHandle()->winId()) != root) {
        return;
    }
    SetFocus(root);
}

void Host::navigate(const QUrl &url)
{
    if (!m_web) {
        m_pending = url;
        m_pendingLoad = true;
        start();
        return;
    }
    m_pendingLoad = false;
    // The full address: WebView2 scrolls to and marks a text fragment (#:~:text=…) by itself.
    const HRESULT result = m_web->Navigate(wide(url.toString(QUrl::FullyEncoded)));
    if (FAILED(result)) {
        m_shown = false;
        const QString text = result == E_INVALIDARG ? QStringLiteral("The address is not valid.") : QStringLiteral("The page could not be loaded (%1).").arg(hresultText(result));
        post([this, text] {
            if (failed) failed(text);
        });
    }
}

bool Host::reload()
{
    if (!m_web || !m_shown) {
        return false;
    }
    m_web->Reload();
    return true;
}

void Host::stop()
{
    m_pendingLoad = false;
    if (m_web) m_web->Stop();
}

void Host::syncBounds()
{
    if (!m_controller || !m_hwnd) {
        return;
    }
    RECT bounds{};
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
}

void Host::syncVisibility()
{
    if (m_controller && m_window) {
        m_controller->put_IsVisible(m_window->isVisible() ? TRUE : FALSE);
    }
}

void Host::takeFocus()
{
    // Only when the window itself has the keyboard (the page may have it already).
    if (m_controller && m_hwnd && GetFocus() == m_hwnd) {
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void Host::parentMoved()
{
    if (m_controller) {
        m_controller->NotifyParentWindowPositionChanged();
    }
}

void Host::windowGone()
{
    // The address shown is loaded again if the window comes back (windowBack).
    QUrl shown;
    LPWSTR source = nullptr;
    if (m_web && SUCCEEDED(m_web->get_Source(&source))) shown = QUrl(taken(source));
    close();
    m_hwnd = nullptr;
    m_windowLost = true;
    if (!m_pendingLoad && shown.isValid() && shown.scheme() != QLatin1String("about")) {
        m_pending = shown;
        m_pendingLoad = true;
    }
}

void Host::windowBack()
{
    // The same window again with a new HWND: a new web view in it, at the last address.
    if (!m_windowLost || !m_window) {
        return;
    }
    m_windowLost = false;
    m_hwnd = HWND(m_window->winId());
    start();
}

void Host::close()
{
    ++m_generation;
    m_starting = false;
    m_focused = false;
    m_shown = false;
    m_committed = ~UINT64(0);
    if (m_controller) m_controller->Close();
    // The web view's own profile goes with it (its folder when the browser quits).
    ComPtr<ICoreWebView2Profile8> deletable;
    if (m_profile && SUCCEEDED(m_profile.As(&deletable))) deletable->Delete();
    m_profile.Reset();
    m_web.Reset();
    m_controller.Reset();
    m_environment.Reset();
    removeTemporaryFolder();
}

void Host::removeTemporaryFolder()
{
    if (m_temporaryFolder.isEmpty()) {
        return;
    }
    // After its browser has quit (a few seconds); else at the next start without InPrivate.
    const QString folder = m_temporaryFolder;
    std::shared_ptr<QLockFile> lock(m_temporaryLock.release());
    m_temporaryFolder.clear();
    QTimer::singleShot(5000, QCoreApplication::instance(), [folder, lock] {
        lock->unlock();
        QDir(folder).removeRecursively();
    });
}

void Host::executeScript(const QString &script, std::function<void(bool, const QString &)> done)
{
    if (!m_web) {
        if (done) done(false, QString());
        return;
    }
    auto answered = std::make_shared<bool>(false);
    QPointer<Host> guard(this);
    const HRESULT result = m_web->ExecuteScript(wide(script),
                                                handler<ICoreWebView2ExecuteScriptCompletedHandler>([guard, done, answered](HRESULT status, LPCWSTR json) -> HRESULT {
                                                    if (*answered) return S_OK;
                                                    *answered = true;
                                                    const QString text = QString::fromWCharArray(json ? json : L"null");
                                                    const bool ok = SUCCEEDED(status);
                                                    if (guard && done) guard->post([done, ok, text] { done(ok, text); });
                                                    return S_OK;
                                                }).Get());
    if (FAILED(result) && !*answered) {
        *answered = true;
        if (done) done(false, QString());
    }
}

void Host::devTools(const QString &method, const QJsonObject &params, std::function<void(bool, const QJsonObject &, const QString &)> reply)
{
    if (!m_web) {
        reply(false, {}, QStringLiteral("there is no web view here"));
        return;
    }
    auto answered = std::make_shared<bool>(false);
    const QString json = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
    const HRESULT result = m_web->CallDevToolsProtocolMethod(
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

void Host::capture(std::function<void(const QImage &)> done)
{
    ComPtr<IStream> stream;
    if (!m_web || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        done(QImage());
        return;
    }
    auto answered = std::make_shared<bool>(false);
    const HRESULT result = m_web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                                                 stream.Get(),
                                                 handler<ICoreWebView2CapturePreviewCompletedHandler>([stream, done, answered](HRESULT status) -> HRESULT {
                                                     if (*answered) return S_OK;
                                                     *answered = true;
                                                     QImage image;
                                                     if (SUCCEEDED(status)) image = QImage::fromData(streamBytes(stream.Get()), "PNG");
                                                     QMetaObject::invokeMethod(
                                                         QCoreApplication::instance(),
                                                         [done, image] { done(image); },
                                                         Qt::QueuedConnection);
                                                     return S_OK;
                                                 }).Get());
    if (FAILED(result) && !*answered) {
        *answered = true;
        done(QImage());
    }
}

void Host::setColorScheme(int mode)
{
    m_colorScheme = mode;
    applyColorScheme();
}

void Host::applyColorScheme()
{
    if (!m_profile) return;
    const COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme = m_colorScheme == 2 ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
        : m_colorScheme == 1                                              ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT
                                                                          : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
    m_profile->put_PreferredColorScheme(scheme);
}

qint64 Host::millisecondsSinceUserInput() const
{
    if (!m_hwnd) {
        return -1;
    }
    const HWND root = GetAncestor(m_hwnd, GA_ROOT);
    LASTINPUTINFO input{};
    input.cbSize = sizeof(input);
    if (!root || GetForegroundWindow() != root || !GetLastInputInfo(&input)) {
        return -1;
    }
    return qint64(DWORD(GetTickCount() - input.dwTime));
}
}
