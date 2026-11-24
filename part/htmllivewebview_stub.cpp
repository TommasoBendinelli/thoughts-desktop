// SPDX-License-Identifier: GPL-2.0-or-later
// Live HTML pages need WebKit (htmllivewebview_mac.mm): elsewhere the reader says so and opens
// the address in the browser instead.
#include "htmllivewebview.h"

#include "core/htmllivepage.h"

namespace Thoughts
{
bool HtmlLiveWebView::available()
{
    return false;
}

HtmlLiveWebView::HtmlLiveWebView(QObject *parent)
    : QObject(parent)
{
}

HtmlLiveWebView::~HtmlLiveWebView() = default;

void HtmlLiveWebView::load(const QUrl &url)
{
    m_url = url;
    Q_EMIT loadFailed(QStringLiteral("there is no web view for live pages here"));
}

void HtmlLiveWebView::reload()
{
    load(m_url);
}

void HtmlLiveWebView::callAsync(const QString &body, const QJsonObject &args, std::function<void(bool, const QString &, const QString &)> done)
{
    Q_UNUSED(body)
    Q_UNUSED(args)
    done(false, QString(), QStringLiteral("there is no web view for live pages here"));
}

void HtmlLiveWebView::snapshot(std::function<void(const QImage &)> done)
{
    done(QImage());
}

void HtmlLiveWebView::copySelection()
{
}

bool HtmlLiveWebView::hasNativeFocus() const
{
    return false;
}

qint64 HtmlLiveWebView::millisecondsSinceUserInput() const
{
    return -1;
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
    if (LivePage::isBrowserLink(url)) Q_EMIT linkRequested(url, false);
}

void HtmlLiveWebView::nativeSelectionMenu()
{
    // No web view here: the platform's menu stays (html-live-pages.md §2.1).
}

void HtmlLiveWebView::nativeSelectionChoice(const QString &)
{
}

bool HtmlLiveWebView::nativeAllows(const QUrl &target, bool mainFrame) const
{
    return LivePage::navigationStays(m_url, target, mainFrame);
}
}
