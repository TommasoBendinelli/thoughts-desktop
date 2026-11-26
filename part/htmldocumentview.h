// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The HTML view of a document (spec "Desktop"): a lazily created Qt WebView QWebView (WKWebView;
// on Windows HtmlPageWebView, a WebView2 of Thoughts' own with the same interface) in a window
// container, showing an HTML edition served by HtmlEditionServer, with the
// shared viewer (part/html-viewer) injected by runJavaScript. App -> viewer: call(); viewer -> app:
// viewerMessage() (HTTP POST /<token>/msg, delivered in the viewer's seq order).
//
// No native child widget may overlap the web view (it would blank it on macOS): everything the
// view shows on top of the text (highlights, review mask) lives in the page.
#include "okularpart_export.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>

class QLabel;
class QStackedLayout;
class QToolButton;
class QWindow;

namespace Thoughts
{
class HtmlEditionServer;
class HtmlLiveWebView;

class OKULARPART_EXPORT HtmlDocumentView : public QWidget
{
    Q_OBJECT
public:
    using CallCallback = std::function<void(bool ok, const QJsonValue &result, const QString &error)>;

    explicit HtmlDocumentView(QWidget *parent = nullptr);
    ~HtmlDocumentView() override;

    // False when this build has no web view for pages (Qt WebView, THOUGHTS_HTML_VIEW, or on
    // Windows WebView2 with its runtime), and on the offscreen and minimal platforms.
    static bool webViewAvailable();

    // Replaces the page by a centred message ("HTML is being built", "HTML not available: …"),
    // with up to two buttons under it (messageActionClicked, messageCancelClicked).
    void showMessage(const QString &text, const QString &actionText = QString(), const QString &cancelText = QString());
    QString message() const;
    QString messageActionText() const;
    QString messageCancelText() const;

    // Serves `bundleDir` and loads `entry`; injects the viewer and calls load(options) (the
    // transport, css and theme are filled in). `ready` follows the viewer's ready message.
    void loadEdition(const QString &bundleDir, const QString &entry, const QJsonObject &options);
    QString loadedBundleDir() const;
    // An HTML attachment (a standalone page that runs its own scripts), already served at `url`
    // with the attachment sandbox CSP (connect-src 'none'): the viewer runs in attachment mode
    // and talks over WKScriptMessageHandler on macOS (WebView2's postMessage on Windows), else by
    // polling ThoughtsViewer.drain()
    // (the http transport would need connect-src). Navigation away from the page is stopped:
    // web links go to externalLinkRequested().
    void loadAttachment(const QUrl &url, const QJsonObject &options);
    // A live page (thoughts-server protocol/html-live-pages.md §2): `url` in a web view of its
    // own (HtmlLiveWebView: the network and the page's scripts, a non-persistent data store, no
    // viewer and no message handler), never in the one of embedded pages, which shows nothing
    // meanwhile. "Loading…" until the page shows; then liveLoaded, or liveLoadFailed (also when
    // nothing showed within liveLoadTimeoutMilliseconds: a tunnel that takes the connection and
    // never answers). Links to other origins arrive as externalLinkRequested.
    Q_INVOKABLE void loadLive(const QUrl &url);
    bool isLive() const
    {
        return m_live;
    }
    bool isLiveLoaded() const
    {
        return m_live && m_liveLoaded;
    }
    QUrl liveUrl() const
    {
        return m_liveUrl;
    }
    // Reload (and Retry): the page again, or its address after a failure.
    Q_INVOKABLE void reloadLive();
    static constexpr int liveLoadTimeoutMilliseconds = 30000;
    // Runs `body`, an async function body with the argument argsJson (@p args), in the live page's
    // isolated script world (Take Snapshot, reading the selection).
    void callLive(const QString &body, const QJsonObject &args, std::function<void(bool ok, const QString &result, const QString &error)> done);
    // The live web view's own Copy of its selection.
    void copyLiveSelection();
    // Asked as the live page's menu of selected text opens (HtmlLiveWebView::setSelectionFeedbackProvider).
    void setLiveSelectionFeedbackProvider(std::function<bool()> provider);
    // Leaves the page (about:blank): its scripts stop; nothing is injected. A live page's web view
    // (and its storage) is gone.
    void unload();
    bool isAttachment() const
    {
        return m_attachment;
    }
    QUrl loadedUrl() const
    {
        return m_pageUrl;
    }
    // "webkit" (macOS), "webview2" (Windows), "queue" (polling) or "http" (an edition); empty
    // before a page was loaded.
    QString transportKind() const
    {
        return m_transportKind;
    }
    bool isReady() const
    {
        return m_ready;
    }

    // ThoughtsViewer.call(name, args). Calls made before `ready` wait for it.
    void call(const QString &name, const QJsonArray &args = {}, CallCallback callback = {});
    // Runs arbitrary script in the page (tests, diagnostics).
    void runScript(const QString &script, std::function<void(const QVariant &)> callback = {});

    void setTheme(const QString &theme);  // "light" | "dark" | "system"
    QString currentTheme() const;
    // Gives the page keyboard focus / takes it back to Qt (macOS first-responder fix).
    void focusPage();
    bool pageHasFocus() const
    {
        return m_pageFocused;
    }
    QWindow *webWindow() const;
    QWidget *webContainer() const;
    // The page as the web view draws it (screenshots in tests); a null image when unavailable.
    void snapshotPage(std::function<void(const QImage &)> done);

    // The bar above the page (alien widgets beside, never over, the web view): a notice with a
    // button, the text size (A−, size, A+; `handler(delta)`, 0 = reset) and "PDF".
    void setNotice(const QString &text, const QString &buttonText);
    QString notice() const;
    void setTextSizeHandler(std::function<void(int delta)> handler);
    // The attachment bar: no text size; a selection action ("New Review Card…"), "Open in
    // Browser" and the back button (`backText`) instead of "PDF".
    void setAttachmentBar(const QString &backText, const QString &backTip);
    void setSelectionAction(const QString &text, bool visible, bool enabled, const QString &toolTip = QString());
    QString selectionActionText() const;
    bool selectionActionEnabled() const;
    QString selectionActionToolTip() const;
    // A live page's bar: `text` ("Live · <address>") with Reload and Take Snapshot (enabled as
    // set by setTakeSnapshotAction; not at all without @p takeSnapshot); an empty text hides them.
    void setLiveBar(const QString &text, bool takeSnapshot = true);
    QString liveBarText() const;
    void setTakeSnapshotAction(bool enabled, const QString &toolTip);
    bool takeSnapshotEnabled() const;
    QString takeSnapshotToolTip() const;
    void setTextSizeLabel(int percent);
    // A strip below the page for the Read Aloud bar.
    QWidget *readAloudHost() const
    {
        return m_readAloudHost;
    }
    void setReadAloudHostVisible(bool visible, int barHeight);

Q_SIGNALS:
    void ready(const QJsonObject &message);
    void viewerMessage(const QJsonObject &message);
    void loadFailed(const QString &error);
    void showPdfRequested();
    void noticeClicked();
    void selectionActionClicked();
    void openInBrowserRequested();
    // A live page: it loaded (the first time or again), or could not be loaded.
    void liveLoaded(const QUrl &url);
    void liveLoadFailed(const QString &error);
    // The reader right-clicked selected text of the live page: its web view showed no menu, the
    // receiver shows its own (html-live-pages.md §2.1).
    void liveSelectionMenuRequested();
    // An item of the live page's menu of selected text: "like", "dislike", "question" or "comment".
    void liveSelectionChoice(const QString &choice);
    // The live bar's Reload and Take Snapshot.
    void reloadRequested();
    void takeSnapshotRequested();
    // The message's buttons (showMessage).
    void messageActionClicked();
    void messageCancelClicked();
    // A link of an attachment page that would leave it (http, https, mailto). userGesture: the
    // reader clicked or typed in this window just before, so the page did not navigate by itself
    // (always false where Thoughts cannot see the page's clicks; the receiver then asks).
    void externalLinkRequested(const QUrl &url, bool userGesture);
    // A thoughts-html: link of an attachment page (another page of the Library; the receiver
    // decides which it opens). userGesture as for externalLinkRequested. On Windows WebView2's
    // navigation says it; elsewhere the viewer's link message does (pageLinkOfMessage), as the
    // web view hands such a link to the system and never navigates.
    void pageLinkRequested(const QUrl &url, bool userGesture);

public:
    // The thoughts-html: link a viewer "link" message (the page's click on a link) names; empty
    // for any other message or link.
    static QUrl pageLinkOfMessage(const QJsonObject &message);

protected:
    void changeEvent(QEvent *event) override;

private:
    void ensureWebView();
    void injectViewer();
    void onRawMessage(const QByteArray &body);
    void onMessage(const QJsonObject &message);
    void startViewer();
    void pollQueue();
    void installNativeTransport();
    void removeNativeTransport();
    bool isLoadedPage(const QUrl &url) const;
    void destroyLive();

public:
    // Whether a navigation counts as the reader's own: an input at most this long ago.
    static constexpr qint64 userGestureMilliseconds = 3000;
    static bool isUserGesture(qint64 millisecondsSinceInput)
    {
        return millisecondsSinceInput >= 0 && millisecondsSinceInput <= userGestureMilliseconds;
    }

private:
    void deliver(const QJsonObject &message);
    void flushHeld(bool force);
    void flushPendingCalls();
    QString themeForPalette() const;

    QStackedLayout *m_stack = nullptr;
    QWidget *m_bar = nullptr;
    QLabel *m_notice = nullptr;
    QToolButton *m_noticeButton = nullptr;
    QToolButton *m_selectionAction = nullptr;
    QToolButton *m_openInBrowser = nullptr;
    QToolButton *m_smaller = nullptr;
    QToolButton *m_larger = nullptr;
    QToolButton *m_back = nullptr;
    QLabel *m_textSize = nullptr;
    QWidget *m_readAloudHost = nullptr;
    std::function<void(int)> m_textSizeHandler;
    QLabel *m_messageLabel = nullptr;
    QWidget *m_messagePage = nullptr;
    QWidget *m_stackHost = nullptr;
    QToolButton *m_messageAction = nullptr;
    QToolButton *m_messageCancel = nullptr;
    QLabel *m_liveLabel = nullptr;
    QToolButton *m_reload = nullptr;
    QToolButton *m_takeSnapshot = nullptr;
    HtmlLiveWebView *m_liveWeb = nullptr;
    std::function<bool()> m_liveSelectionFeedback;
    QWidget *m_liveContainer = nullptr;
    bool m_live = false;
    bool m_liveLoaded = false;
    QUrl m_liveUrl;
    // Started with each load of the live page, stopped when it shows or fails.
    QTimer m_liveTimeout;
    void startLiveLoad();
    QWidget *m_container = nullptr;
    QWindow *m_web = nullptr; // QWebView, or HtmlPageWebView on Windows (a QWindow), when there is a web view
    HtmlEditionServer *m_server = nullptr;
    QJsonObject m_loadOptions;
    QString m_bundleDir;
    QString m_theme = QStringLiteral("system");
    bool m_ready = false;
    bool m_pageFocused = false;
    bool m_attachment = false;
    QUrl m_pageUrl;
    QString m_transportKind;
    void *m_nativeHandler = nullptr;
    QTimer m_pollTimer;
    bool m_polling = false;
    int m_loadGeneration = 0;
    qint64 m_nextSeq = 1;
    QMap<qint64, QJsonObject> m_held;
    QTimer m_holdTimer;
    struct PendingCall {
        QString name;
        QJsonArray args;
        CallCallback callback;
    };
    QList<PendingCall> m_pendingCalls;
};
}
