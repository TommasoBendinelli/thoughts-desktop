// SPDX-License-Identifier: GPL-2.0-or-later
// The WebView2 of embedded HTML pages and HTML editions on Windows (htmlpagewebview_win.h).
#include "htmlpagewebview_win.h"

#include "webview2host_win.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

namespace Thoughts
{
namespace
{
bool openableOutside(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"));
}
}

bool HtmlPageWebView::available()
{
    return WebView2::available();
}

HtmlPageWebView::HtmlPageWebView()
{
    WebView2::Host::Options options;
    // The viewer's native transport of attachments (kind "webview2"), as WKScriptMessageHandler
    // on the Mac.
    options.webMessages = true;
    // As WKWebView: no zoom of its own (the viewer sets the text size), and none of a browser's
    // keys (Find, Print, Reload): the viewer forwards Thoughts' shortcuts.
    options.zoom = false;
    options.browserKeys = false;
    m_host = new WebView2::Host(this, options);
    m_host->allowNavigation = [this](const QUrl &url, bool userGesture, bool redirected) {
        // No file access (AllowFileAccess off).
        if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0) return false;
        return !m_filter || m_filter(url, userGesture && !redirected, false);
    };
    m_host->allowFrameNavigation = [](const QUrl &url) {
        return url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0;
    };
    m_host->newWindow = [this](const QUrl &url, bool userGesture) {
        openOutside(url, userGesture);
    };
    m_host->externalScheme = [this](const QUrl &url, bool userGesture) {
        openOutside(url, userGesture);
    };
    m_host->started = [this](const QUrl &url) {
        Q_EMIT loadingChanged(HtmlPageLoading(url, HtmlPageLoading::LoadStatus::Started));
    };
    m_host->finished = [this](const QUrl &url) {
        Q_EMIT loadingChanged(HtmlPageLoading(url, HtmlPageLoading::LoadStatus::Succeeded));
    };
    m_host->stopped = [this](const QUrl &url) {
        Q_EMIT loadingChanged(HtmlPageLoading(url, HtmlPageLoading::LoadStatus::Stopped));
    };
    m_host->failed = [this](const QString &error) {
        Q_EMIT loadingChanged(HtmlPageLoading(m_url, HtmlPageLoading::LoadStatus::Failed, error));
    };
    m_host->message = [this](const QString &message, const QUrl &source) {
        if (m_messages) m_messages(message.toUtf8(), source);
    };
}

HtmlPageWebView::~HtmlPageWebView()
{
    WebView2::Host *host = m_host;
    m_host = nullptr;
    // The web view closes while the HWND is still there.
    host->windowDestroying();
    delete host;
}

bool HtmlPageWebView::event(QEvent *event)
{
    if (m_host) m_host->windowEvent(event);
    return QWindow::event(event);
}

void HtmlPageWebView::openOutside(const QUrl &url, bool userGesture)
{
    // A new window, or another application's address (mailto): outside, from the reader's click
    // only (as WebKit, which opens no window by itself); an attachment's filter has its say.
    if (!userGesture || !openableOutside(url)) return;
    if (m_filter && !m_filter(url, userGesture, true)) return;
    QDesktopServices::openUrl(url);
}

void HtmlPageWebView::setUrl(const QUrl &url)
{
    m_url = url;
    m_host->navigate(url);
}

void HtmlPageWebView::stop()
{
    m_host->stop();
}

void HtmlPageWebView::runJavaScript(const QString &script, const std::function<void(const QVariant &)> &resultCallback)
{
    m_host->executeScript(script, [resultCallback](bool ok, const QString &json) {
        if (!resultCallback) return;
        // The result as JSON: a string, a number, an object… (undefined is null).
        const QJsonArray wrapped = ok ? QJsonDocument::fromJson((QLatin1Char('[') + json + QLatin1Char(']')).toUtf8()).array() : QJsonArray();
        resultCallback(wrapped.isEmpty() ? QVariant() : wrapped.at(0).toVariant());
    });
}

void HtmlPageWebView::setNavigationFilter(std::function<bool(const QUrl &, bool, bool)> filter)
{
    m_filter = std::move(filter);
}

void HtmlPageWebView::setMessageHandler(std::function<void(const QByteArray &, const QUrl &)> handler)
{
    m_messages = std::move(handler);
}

void HtmlPageWebView::snapshot(std::function<void(const QImage &)> done)
{
    m_host->capture(std::move(done));
}

void HtmlPageWebView::setAppearance(int mode)
{
    m_host->setColorScheme(mode);
}

bool HtmlPageWebView::hasNativeFocus() const
{
    return m_host->hasFocus();
}
}
