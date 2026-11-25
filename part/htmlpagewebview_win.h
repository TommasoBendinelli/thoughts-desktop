// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The web view of embedded HTML pages and HTML editions on Windows (HtmlDocumentView), where Qt
// WebView has no backend: a QWindow with a Microsoft Edge WebView2 in it (webview2host_win.h), with
// the part of QWebView's interface HtmlDocumentView uses (setUrl, stop, runJavaScript in the page's
// own world, loadingChanged) and what htmlmacnative.mm adds on the Mac (a snapshot, the page's
// appearance, a native message transport, the reader's clicks).
//
// Like WKWebView with LocalStorageEnabled off: InPrivate, nothing kept. A target=_blank link and an
// address of another application open outside (in the browser), as Qt WebView's WKWebView does,
// only from the reader's click.
#include <QImage>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <functional>

namespace Thoughts
{
namespace WebView2
{
class Host;
}

// QWebViewLoadingInfo's part HtmlDocumentView reads.
class HtmlPageLoading
{
public:
    enum class LoadStatus { Started, Stopped, Succeeded, Failed };
    HtmlPageLoading() = default;
    HtmlPageLoading(const QUrl &url, LoadStatus status, const QString &error = QString())
        : m_url(url)
        , m_status(status)
        , m_error(error)
    {
    }
    QUrl url() const
    {
        return m_url;
    }
    LoadStatus status() const
    {
        return m_status;
    }
    QString errorString() const
    {
        return m_error;
    }

private:
    QUrl m_url;
    LoadStatus m_status = LoadStatus::Started;
    QString m_error;
};

class HtmlPageWebView : public QWindow
{
    Q_OBJECT
public:
    HtmlPageWebView();
    ~HtmlPageWebView() override;

    // A real window system and the WebView2 runtime.
    static bool available();

    void setUrl(const QUrl &url);
    QUrl url() const
    {
        return m_url;
    }
    void stop();
    // @p script in the page's own world; @p resultCallback gets its result (a string, a number…).
    void runJavaScript(const QString &script, const std::function<void(const QVariant &)> &resultCallback = {});

    // Asked before the page goes to @p url (the main frame, or a new window when @p newWindow):
    // false keeps it where it is. @p userGesture: the reader's own click (WebView2 says).
    void setNavigationFilter(std::function<bool(const QUrl &url, bool userGesture, bool newWindow)> filter);
    // What the page posts with window.chrome.webview.postMessage(string), and the page that did.
    void setMessageHandler(std::function<void(const QByteArray &message, const QUrl &source)> handler);
    // The page as the web view draws it; a null image when it cannot.
    void snapshot(std::function<void(const QImage &)> done);
    // prefers-color-scheme: 0 follows Windows, 1 light, 2 dark.
    void setAppearance(int mode);
    bool hasNativeFocus() const;

Q_SIGNALS:
    void loadingChanged(const Thoughts::HtmlPageLoading &loadingInfo);

protected:
    bool event(QEvent *event) override;

private:
    void openOutside(const QUrl &url, bool userGesture);

    WebView2::Host *m_host = nullptr;
    QUrl m_url;
    std::function<bool(const QUrl &, bool, bool)> m_filter;
    std::function<void(const QByteArray &, const QUrl &)> m_messages;
};
}
