// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The web view of a live HTML page (thoughts-server protocol/html-live-pages.md §2), apart from
// the one of embedded pages and HTML editions (Qt WebView's): on macOS a WKWebView with a
// configuration of its own, made here:
//
// - a non-persistent WKWebsiteDataStore of its own (cookies, storage and caches end with it);
// - an empty WKUserContentController: no user scripts and no message handlers, so the page gets
//   none of Thoughts' page bridge (no viewer, cards or selection actions of its own);
// - the page's scripts and the network allowed; no file access, downloads, new windows, camera or
//   microphone;
// - navigation only within the page's origin in the main frame (frames inside it may load any
//   web address); a link elsewhere is stopped and reported as linkRequested.
//
// Thoughts runs its own scripts (Take Snapshot, reading the selection) in
// WKContentWorld.defaultClientWorld, a script world the page's scripts cannot reach. Text feedback
// (html-live-pages.md §2.1) is the reader's: the web view's own right-click menu of selected text
// shows Thoughts' Like, Dislike, Question… and Comment… with the web view's own Copy (one menu,
// no Services), and the item chosen comes as selectionChoice; every other menu (a link, an image,
// no selection) stays the web view's. Elsewhere (no WebKit) available() is false and the platform
// menu stays.
#include "okularpart_export.h"

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QWindow>

#include <functional>

namespace Thoughts
{
class OKULARPART_EXPORT HtmlLiveWebView : public QObject
{
    Q_OBJECT
public:
    explicit HtmlLiveWebView(QObject *parent = nullptr);
    ~HtmlLiveWebView() override;

    // Whether this build can show live pages (WebKit on macOS, a real window system).
    static bool available();

    // The web view as a foreign QWindow, for QWidget::createWindowContainer (which then owns it).
    QWindow *window() const
    {
        return m_window;
    }
    // Loads @p url; its origin is the page's own from now on.
    void load(const QUrl &url);
    void reload();
    QUrl url() const
    {
        return m_url;
    }
    // Runs @p body as the body of an async function with the one argument `argsJson` (@p args as
    // JSON) in the isolated script world; @p done gets its result as a string.
    void callAsync(const QString &body, const QJsonObject &args, std::function<void(bool ok, const QString &result, const QString &error)> done);
    // The page as the web view draws it; a null image when it cannot.
    void snapshot(std::function<void(const QImage &)> done);
    // The web view's own Copy of what is selected (any frame, a form field too).
    void copySelection();
    // Whether the WKWebView has the keyboard (its window's first responder).
    bool hasNativeFocus() const;
    // Milliseconds since the reader last clicked or typed in the window that shows it; -1 if never.
    qint64 millisecondsSinceUserInput() const;

    // Called by the native delegate (main thread).
    void nativeStarted();
    void nativeCommitted();
    void nativeFinished(const QUrl &url);
    void nativeFailed(const QString &error);
    void nativeLinkRequested(const QUrl &url);
    void nativeSelectionMenu();
    void nativeSelectionChoice(const QString &choice);
    bool nativeAllows(const QUrl &target, bool mainFrame) const;

    // Asked as the menu of selected text opens: whether it offers feedback now (a Library document,
    // no review of one of the page's cards); without it the menu keeps only the web view's Copy.
    void setSelectionFeedbackProvider(std::function<bool()> provider)
    {
        m_selectionFeedback = std::move(provider);
    }
    bool selectionFeedback() const
    {
        return m_selectionFeedback && m_selectionFeedback();
    }

Q_SIGNALS:
    void loadStarted();
    // The page began to show (its first content arrived).
    void loadCommitted();
    void loadFinished(const QUrl &url);
    // The address could not be loaded (unreachable, refused, a certificate, the page's process ended).
    void loadFailed(const QString &error);
    // A link of the page to another origin or a new window (a web or mailto address): the reader's
    // click when userGesture.
    void linkRequested(const QUrl &url, bool userGesture);
    // The reader right-clicked selected text: the web view's menu was not shown, Thoughts' shows
    // instead (only ever from the reader's own click; the page cannot open a menu).
    void selectionMenuRequested();
    // The reader picked an item of that menu: "like", "dislike", "question" or "comment" (the
    // selection is still shown; read it now).
    void selectionChoice(const QString &choice);

private:
    std::function<bool()> m_selectionFeedback;
    void *m_native = nullptr; // the WKWebView (ThoughtsLiveWKWebView) and its delegate (htmllivewebview_mac.mm)
    QPointer<QWindow> m_window;
    QUrl m_url;
};
}
