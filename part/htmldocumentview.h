// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The HTML view of a document (spec "Desktop"): a lazily created Qt WebView QWebView (WKWebView /
// WebView2) in a window container, showing an HTML edition served by HtmlEditionServer, with the
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

class OKULARPART_EXPORT HtmlDocumentView : public QWidget
{
    Q_OBJECT
public:
    using CallCallback = std::function<void(bool ok, const QJsonValue &result, const QString &error)>;

    explicit HtmlDocumentView(QWidget *parent = nullptr);
    ~HtmlDocumentView() override;

    // False when this build has no Qt WebView (THOUGHTS_HTML_VIEW off).
    static bool webViewAvailable();

    // Replaces the page by a centred message ("HTML is being built", "HTML not available: …").
    void showMessage(const QString &text);
    QString message() const;

    // Serves `bundleDir` and loads `entry`; injects the viewer and calls load(options) (the
    // transport, css and theme are filled in). `ready` follows the viewer's ready message.
    void loadEdition(const QString &bundleDir, const QString &entry, const QJsonObject &options);
    QString loadedBundleDir() const;
    // An HTML attachment (a standalone page that runs its own scripts), already served at `url`
    // with the attachment sandbox CSP (connect-src 'none'): the viewer runs in attachment mode
    // and talks over WKScriptMessageHandler on macOS, else by polling ThoughtsViewer.drain()
    // (the http transport would need connect-src). Navigation away from the page is stopped:
    // web links go to externalLinkRequested().
    void loadAttachment(const QUrl &url, const QJsonObject &options);
    // Leaves the page (about:blank): its scripts stop; nothing is injected.
    void unload();
    bool isAttachment() const
    {
        return m_attachment;
    }
    QUrl loadedUrl() const
    {
        return m_pageUrl;
    }
    // "webkit", "queue" (polling) or "http" (an edition); empty before a page was loaded.
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
    // A link of an attachment page that would leave it (http, https, mailto). userGesture: the
    // reader clicked or typed in this window just before, so the page did not navigate by itself
    // (always false where Thoughts cannot see the page's clicks; the receiver then asks).
    void externalLinkRequested(const QUrl &url, bool userGesture);

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
    QWidget *m_container = nullptr;
    QWindow *m_web = nullptr; // QWebView (a QWindow) when Qt WebView is available
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
