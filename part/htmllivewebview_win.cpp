// SPDX-License-Identifier: GPL-2.0-or-later
// The web view of live HTML pages on Windows (htmllivewebview.h): a Microsoft Edge WebView2 of
// its own (webview2host_win.h: InPrivate under a profile of its own, no downloads, new windows,
// permissions or script dialogs), as htmllivewebview_mac.mm makes a WKWebView:
//
// - no host objects and no web messages, so the page gets none of Thoughts' page bridge;
// - navigation only within the page's origin in the main frame (frames inside it may load any web
//   address); a link elsewhere or to a new window is stopped and reported as linkRequested, a
//   user gesture as WebView2 says (a server's redirect is none).
//
// Thoughts' own scripts (callAsync: Take Snapshot, reading the selection) run through the DevTools
// protocol in an isolated world of their own ("thoughts"), which the page's scripts cannot reach,
// as WKContentWorld.defaultClientWorld on the Mac. The web view's own right-click menu of selected
// text shows Thoughts' Like, Dislike, Question… and Comment… with the web view's Copy (one menu);
// every other menu stays the web view's.
#include "htmllivewebview.h"

#include "core/htmllivepage.h"
#include "htmldocumentview.h"
#include "webview2host_win.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QWindow>

#include <memory>
#include <utility>

namespace
{
using namespace Thoughts::WebView2;

QString jsString(const QString &text)
{
    // A JSON string is a JS string literal (U+2028/9 are valid in JS strings since ES2019).
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

// The window the web view is a child of (its events go to the Host).
class LiveWindow : public QWindow
{
public:
    ~LiveWindow() override
    {
        if (host) host->windowDestroying();
    }
    Host *host = nullptr;

protected:
    bool event(QEvent *event) override
    {
        if (host) host->windowEvent(event);
        return QWindow::event(event);
    }
};

// What the view keeps besides its Host.
class LiveState : public QObject
{
public:
    std::unique_ptr<Host> host;
    // Whether the reader's own click asked for the link being reported (WebView2's
    // IsUserInitiated); -1 when unknown.
    int linkGesture = -1;
};

LiveState *liveState(void *handle)
{
    return static_cast<LiveState *>(handle);
}

// The menu of selected text: Thoughts' items, then the web view's own Copy.
void selectionMenu(Thoughts::HtmlLiveWebView *view, Host *host, ICoreWebView2ContextMenuRequestedEventArgs *args)
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
    ComPtr<ICoreWebView2Environment> environment(host->environment());
    ComPtr<ICoreWebView2Environment9> factory;
    if (copy < 0 || !environment || FAILED(environment.As(&factory))) {
        return;
    }
    // One menu: Thoughts' items, then the web view's own Copy; the selection is read when one is
    // chosen.
    for (UINT32 index = count; index-- > 0;) {
        if (int(index) != copy) items->RemoveValueAtIndex(index);
    }
    if (!view->selectionFeedback()) {
        return;
    }
    const struct {
        const wchar_t *title;
        const char *choice;
    } choices[] = {{L"Like", "like"}, {L"Dislike", "dislike"}, {L"Question…", "question"}, {L"Comment…", "comment"}};
    UINT32 at = 0;
    QPointer<Thoughts::HtmlLiveWebView> guard(view);
    for (const auto &entry : choices) {
        ComPtr<ICoreWebView2ContextMenuItem> item;
        if (FAILED(factory->CreateContextMenuItem(entry.title, nullptr, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &item)) || !item) {
            continue;
        }
        const QString picked = QString::fromLatin1(entry.choice);
        EventRegistrationToken token{};
        item->add_CustomItemSelected(handler<ICoreWebView2CustomItemSelectedEventHandler>([guard, picked](ICoreWebView2ContextMenuItem *, IUnknown *) -> HRESULT {
                                         // After WebView2's call returns: a slot may delete the view.
                                         if (guard) {
                                             QMetaObject::invokeMethod(
                                                 guard.data(),
                                                 [guard, picked] {
                                                     if (guard) guard->nativeSelectionChoice(picked);
                                                 },
                                                 Qt::QueuedConnection);
                                         }
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

// The main frame's isolated world "thoughts" (one per document, made again after a navigation).
void callIsolated(Host *host, const QString &expression, std::function<void(bool, const QString &, const QString &)> done)
{
    QPointer<Host> guard(host);
    const auto closed = [guard] {
        return !guard || !guard->isReady();
    };
    host->devTools(QStringLiteral("Page.getFrameTree"), {}, [guard, closed, expression, done](bool ok, const QJsonObject &reply, const QString &error) {
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
}

namespace Thoughts
{
bool HtmlLiveWebView::available()
{
    return WebView2::available();
}

HtmlLiveWebView::HtmlLiveWebView(QObject *parent)
    : QObject(parent)
{
    if (!available()) {
        return;
    }
    auto *state = new LiveState;
    m_native = state;
    auto *window = new LiveWindow;
    m_window = window;
    state->host = std::make_unique<Host>(window, Host::Options{});
    window->host = state->host.get();
    Host *host = state->host.get();

    QPointer<HtmlLiveWebView> self(this);
    QPointer<LiveState> kept(state);
    // Reported after WebView2's call returns: a slot may delete the view.
    const auto link = [self, kept](const QUrl &url, bool userGesture) {
        if (!self) return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, kept, url, userGesture] {
                if (!self || !kept) return;
                kept->linkGesture = userGesture ? 1 : 0;
                self->nativeLinkRequested(url);
                if (kept) kept->linkGesture = -1;
            },
            Qt::QueuedConnection);
    };
    // A load this view allowed is under way and has not shown yet: the site's own redirects on
    // the way (a sign-in or cookie hop, a DOI to its publisher) are followed, as in a browser.
    auto hopping = std::make_shared<bool>(false);
    auto lastHop = std::make_shared<QUrl>();
    host->allowNavigation = [this, link, hopping, lastHop](const QUrl &url, bool userGesture, bool redirected) {
        if (redirected && *hopping) {
            *lastHop = url;
            return true;
        }
        if (nativeAllows(url, true)) {
            *hopping = true;
            *lastHop = QUrl();
            return true;
        }
        // A server's redirect elsewhere is not the reader's click.
        link(url, userGesture && !redirected);
        return false;
    };
    host->allowFrameNavigation = [this](const QUrl &url) {
        return nativeAllows(url, false);
    };
    host->newWindow = [this, host, link](const QUrl &url, bool userGesture) {
        // As javaScriptCanOpenWindowsAutomatically = NO: only from the reader's click; the page
        // itself when it stays in its origin.
        if (!userGesture) return;
        if (nativeAllows(url, true)) host->navigate(url);
        else link(url, true);
    };
    // A mailto link is reported, as one elsewhere.
    host->externalScheme = link;
    host->started = [this](const QUrl &) {
        nativeStarted();
    };
    host->committed = [this, hopping, lastHop] {
        *hopping = false;
        nativeCommitted(std::exchange(*lastHop, QUrl()));
    };
    host->finished = [this](const QUrl &url) {
        nativeFinished(url);
    };
    host->failed = [this, hopping](const QString &error) {
        *hopping = false;
        nativeFailed(error);
    };
    host->contextMenu = [this, host](ICoreWebView2ContextMenuRequestedEventArgs *args) {
        selectionMenu(this, host, args);
    };
}

HtmlLiveWebView::~HtmlLiveWebView()
{
    LiveState *state = liveState(m_native);
    if (!state) {
        return;
    }
    // The web view closes while its window is still there.
    state->host->close();
    if (m_window) {
        static_cast<LiveWindow *>(m_window.data())->host = nullptr;
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
    LiveState *state = liveState(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    state->host->navigate(url);
}

void HtmlLiveWebView::reload()
{
    LiveState *state = liveState(m_native);
    if (!state) {
        Q_EMIT loadFailed(QStringLiteral("there is no web view here"));
        return;
    }
    // After a failure there is no page to reload: the address again.
    if (!state->host->reload()) load(m_url);
}

void HtmlLiveWebView::stop()
{
    LiveState *state = liveState(m_native);
    if (state) state->host->stop();
}

void HtmlLiveWebView::callAsync(const QString &body, const QJsonObject &args, std::function<void(bool, const QString &, const QString &)> done)
{
    LiveState *state = liveState(m_native);
    if (!state || !state->host->isReady()) {
        done(false, QString(), QStringLiteral("there is no web view here"));
        return;
    }
    // As callAsyncJavaScript: the body of an async function of argsJson, its promise awaited.
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    const QString expression = QStringLiteral("(async function (argsJson) {\n") + body + QStringLiteral("\n})(") + jsString(json) + QStringLiteral(")");
    callIsolated(state->host.get(), expression, std::move(done));
}

void HtmlLiveWebView::snapshot(std::function<void(const QImage &)> done)
{
    LiveState *state = liveState(m_native);
    if (!state) {
        done(QImage());
        return;
    }
    state->host->capture(std::move(done));
}

void HtmlLiveWebView::copySelection()
{
    LiveState *state = liveState(m_native);
    if (!state || !state->host->isReady()) {
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
    state->host->devTools(QStringLiteral("Input.dispatchKeyEvent"), down, ignore);
    state->host->devTools(QStringLiteral("Input.dispatchKeyEvent"), key, ignore);
}

bool HtmlLiveWebView::hasNativeFocus() const
{
    LiveState *state = liveState(m_native);
    return state && state->host->hasFocus();
}

qint64 HtmlLiveWebView::millisecondsSinceUserInput() const
{
    LiveState *state = liveState(m_native);
    return state ? state->host->millisecondsSinceUserInput() : -1;
}

void HtmlLiveWebView::nativeStarted()
{
    Q_EMIT loadStarted();
}

void HtmlLiveWebView::nativeCommitted(const QUrl &redirectedTo)
{
    // A load that a server sent elsewhere (a DOI to its publisher): the page's own navigations
    // stay within where it arrived.
    if (redirectedTo.isValid()) m_url = redirectedTo;
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
    LiveState *state = liveState(m_native);
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
