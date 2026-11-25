// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmldocumentview.h"

#include "htmleditionserver.h"
#include "htmllivewebview.h"
#include "htmlmacnative.h"

#include <QApplication>
#include <QGuiApplication>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPalette>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#if THOUGHTS_HAVE_QTWEBVIEW
#include <QWebView>
#include <QWebViewLoadingInfo>
#include <QWebViewSettings>
#define THOUGHTS_PAGE_VIEW 1
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
// Windows, where Qt WebView has no backend: a WebView2 of Thoughts' own with QWebView's interface.
#include "htmlpagewebview_win.h"
#define THOUGHTS_PAGE_VIEW 1
#else
#define THOUGHTS_PAGE_VIEW 0
#endif

namespace Thoughts
{
namespace
{
QString resourceText(const QString &name)
{
    QFile file(QStringLiteral(":/thoughts/html-viewer/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString jsString(const QString &text)
{
    // A JSON string is a JS string literal (U+2028/9 are valid in JS strings since ES2019).
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

#if THOUGHTS_HAVE_QTWEBVIEW
using PageWebView = QWebView;
using PageLoadingInfo = QWebViewLoadingInfo;
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
using PageWebView = HtmlPageWebView;
using PageLoadingInfo = HtmlPageLoading;
#endif

#if THOUGHTS_PAGE_VIEW
PageWebView *asWebView(QWindow *window)
{
    return static_cast<PageWebView *>(window);
}
#endif
}

HtmlDocumentView::HtmlDocumentView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("thoughtsHtmlDocumentView"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_bar = new QWidget(this);
    m_bar->setObjectName(QStringLiteral("thoughtsHtmlViewBar"));
    auto *bar = new QHBoxLayout(m_bar);
    bar->setContentsMargins(8, 2, 8, 2);
    // A live page: "Live · <address>" (its Reload and Take Snapshot are with the buttons).
    m_liveLabel = new QLabel(m_bar);
    m_liveLabel->setObjectName(QStringLiteral("thoughtsHtmlLiveLabel"));
    m_liveLabel->setTextFormat(Qt::PlainText);
    m_liveLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_liveLabel->setMinimumWidth(80);
    m_liveLabel->hide();
    bar->addWidget(m_liveLabel, 2);
    m_notice = new QLabel(m_bar);
    m_notice->setObjectName(QStringLiteral("thoughtsHtmlViewNotice"));
    m_notice->setWordWrap(true);
    m_notice->hide();
    bar->addWidget(m_notice, 1);
    m_noticeButton = new QToolButton(m_bar);
    m_noticeButton->setObjectName(QStringLiteral("thoughtsHtmlViewNoticeButton"));
    m_noticeButton->hide();
    connect(m_noticeButton, &QToolButton::clicked, this, &HtmlDocumentView::noticeClicked);
    bar->addWidget(m_noticeButton);
    bar->addStretch(0);
    // Attachment pages: the action for the page's selection (New Review Card…).
    m_selectionAction = new QToolButton(m_bar);
    m_selectionAction->setObjectName(QStringLiteral("thoughtsHtmlSelectionAction"));
    m_selectionAction->hide();
    connect(m_selectionAction, &QToolButton::clicked, this, &HtmlDocumentView::selectionActionClicked);
    bar->addWidget(m_selectionAction);
    auto addButton = [this, bar](const QString &text, const QString &tip, const QString &name) {
        auto *button = new QToolButton(m_bar);
        button->setText(text);
        button->setToolTip(tip);
        button->setObjectName(name);
        button->setAutoRaise(true);
        bar->addWidget(button);
        return button;
    };
    m_reload = addButton(tr("Reload"), tr("Load the live page again"), QStringLiteral("thoughtsHtmlLiveReload"));
    m_reload->hide();
    m_takeSnapshot = addButton(tr("Take Snapshot"), QString(), QStringLiteral("thoughtsHtmlLiveTakeSnapshot"));
    m_takeSnapshot->hide();
    connect(m_reload, &QToolButton::clicked, this, &HtmlDocumentView::reloadRequested);
    connect(m_takeSnapshot, &QToolButton::clicked, this, &HtmlDocumentView::takeSnapshotRequested);
    QToolButton *smaller = addButton(QStringLiteral("A\u2212"), tr("Smaller text"), QStringLiteral("thoughtsHtmlTextSmaller"));
    m_textSize = new QLabel(m_bar);
    m_textSize->setObjectName(QStringLiteral("thoughtsHtmlTextSize"));
    bar->addWidget(m_textSize);
    QToolButton *larger = addButton(QStringLiteral("A+"), tr("Larger text"), QStringLiteral("thoughtsHtmlTextLarger"));
    m_openInBrowser = addButton(tr("Open in Browser"), tr("Open this page in the default web browser"), QStringLiteral("thoughtsHtmlOpenInBrowser"));
    m_openInBrowser->hide();
    QToolButton *pdf = addButton(tr("PDF"), tr("Show the PDF"), QStringLiteral("thoughtsHtmlShowPdf"));
    m_smaller = smaller;
    m_larger = larger;
    m_back = pdf;
    connect(smaller, &QToolButton::clicked, this, [this] {
        if (m_textSizeHandler) m_textSizeHandler(-10);
    });
    connect(larger, &QToolButton::clicked, this, [this] {
        if (m_textSizeHandler) m_textSizeHandler(10);
    });
    connect(m_openInBrowser, &QToolButton::clicked, this, &HtmlDocumentView::openInBrowserRequested);
    connect(pdf, &QToolButton::clicked, this, &HtmlDocumentView::showPdfRequested);
    outer->addWidget(m_bar);
    auto *stackHost = new QWidget(this);
    outer->addWidget(stackHost, 1);
    m_readAloudHost = new QWidget(this);
    m_readAloudHost->setObjectName(QStringLiteral("thoughtsHtmlReadAloudHost"));
    m_readAloudHost->hide();
    outer->addWidget(m_readAloudHost);
    m_stackHost = stackHost;
    m_stack = new QStackedLayout(stackHost);
    m_stack->setContentsMargins(0, 0, 0, 0);
    // The message, with its buttons below it (Retry, Open / Cancel).
    m_messagePage = new QWidget(stackHost);
    m_messagePage->setObjectName(QStringLiteral("thoughtsHtmlViewMessagePage"));
    auto *messageLayout = new QVBoxLayout(m_messagePage);
    messageLayout->addStretch(1);
    m_messageLabel = new QLabel(m_messagePage);
    m_messageLabel->setObjectName(QStringLiteral("thoughtsHtmlViewMessage"));
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_messageLabel->setMargin(24);
    messageLayout->addWidget(m_messageLabel);
    auto *messageButtons = new QHBoxLayout;
    messageButtons->addStretch(1);
    m_messageAction = new QToolButton(m_messagePage);
    m_messageAction->setObjectName(QStringLiteral("thoughtsHtmlViewMessageAction"));
    m_messageAction->hide();
    m_messageCancel = new QToolButton(m_messagePage);
    m_messageCancel->setObjectName(QStringLiteral("thoughtsHtmlViewMessageCancel"));
    m_messageCancel->hide();
    messageButtons->addWidget(m_messageCancel);
    messageButtons->addWidget(m_messageAction);
    messageButtons->addStretch(1);
    messageLayout->addLayout(messageButtons);
    messageLayout->addStretch(2);
    connect(m_messageAction, &QToolButton::clicked, this, &HtmlDocumentView::messageActionClicked);
    connect(m_messageCancel, &QToolButton::clicked, this, &HtmlDocumentView::messageCancelClicked);
    m_stack->addWidget(m_messagePage);
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(120);
    // A live page that never shows (an address that takes the connection and never answers):
    // stopped and said, as a failure would be.
    m_liveTimeout.setSingleShot(true);
    m_liveTimeout.setInterval(liveLoadTimeoutMilliseconds);
    connect(&m_liveTimeout, &QTimer::timeout, this, [this] {
        if (!m_live || !m_liveWeb) return;
        m_liveWeb->stop();
        m_liveLoaded = false;
        Q_EMIT liveLoadFailed(tr("The page did not answer within %1 seconds.").arg(liveLoadTimeoutMilliseconds / 1000));
    });
    connect(&m_holdTimer, &QTimer::timeout, this, [this] { flushHeld(true); });
    // The polling transport of attachment pages (no native message handler).
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &HtmlDocumentView::pollQueue);
    if (!webViewAvailable()) {
        showMessage(tr("HTML not available: there is no web view here."));
    }
#ifdef Q_OS_MACOS
    // A click on the WKWebView goes to the web view, not to Qt: a popup open over it (the feedback
    // editor) would never learn of a click outside it.
    HtmlMacNative::closePopupsOnWebViewClicks();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        // Programmatic focus elsewhere in our window, or in a popup opened over it (the feedback
        // editor's comment): keys must leave the WKWebView (Spike 2). The popup gets its keys
        // through our window, whose first responder the WKWebView otherwise stays: it then kept
        // Space (scrolling the page) and the arrows, and only the other keys reached the comment.
        if ((!m_web && !m_liveWeb) || !now || now == m_container || (m_liveContainer && now == m_liveContainer)) {
            return;
        }
        QWidget *top = now->window();
        while (top && top->windowType() == Qt::Popup && top->parentWidget()) {
            top = top->parentWidget()->window();
        }
        if (top != window()) {
            return;
        }
        if ((m_web && HtmlMacNative::webViewHasNativeFocus(m_web)) || (m_liveWeb && m_liveWeb->hasNativeFocus())) {
            HtmlMacNative::focusQt(now->window() == window() ? now : this);
        }
        m_pageFocused = false;
    });
#endif
}

HtmlDocumentView::~HtmlDocumentView()
{
    m_pendingCalls.clear();
    m_pollTimer.stop();
    removeNativeTransport();
    destroyLive();
}

bool HtmlDocumentView::webViewAvailable()
{
#if THOUGHTS_HAVE_QTWEBVIEW
    // Native web views need a real window system (not the offscreen platform of tests).
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
    // The same, and the WebView2 runtime installed.
    return HtmlPageWebView::available();
#else
    return false;
#endif
}

void HtmlDocumentView::showMessage(const QString &text, const QString &actionText, const QString &cancelText)
{
    m_messageLabel->setText(text);
    m_messageAction->setText(actionText);
    m_messageAction->setVisible(!actionText.isEmpty());
    m_messageCancel->setText(cancelText);
    m_messageCancel->setVisible(!cancelText.isEmpty());
    m_stack->setCurrentWidget(m_messagePage);
}

QString HtmlDocumentView::message() const
{
    return m_stack->currentWidget() == m_messagePage ? m_messageLabel->text() : QString();
}

QString HtmlDocumentView::messageActionText() const
{
    return m_stack->currentWidget() == m_messagePage && !m_messageAction->isHidden() ? m_messageAction->text() : QString();
}

QString HtmlDocumentView::messageCancelText() const
{
    return m_stack->currentWidget() == m_messagePage && !m_messageCancel->isHidden() ? m_messageCancel->text() : QString();
}

QWindow *HtmlDocumentView::webWindow() const
{
    return m_web;
}

QWidget *HtmlDocumentView::webContainer() const
{
    return m_live ? m_liveContainer : m_container;
}

void HtmlDocumentView::snapshotPage(std::function<void(const QImage &)> done)
{
    // A live page as its own web view draws it.
    if (m_live) {
        if (m_liveWeb) {
            m_liveWeb->snapshot(std::move(done));
        } else {
            done(QImage());
        }
        return;
    }
#ifdef Q_OS_MACOS
    if (m_web) {
        HtmlMacNative::snapshot(m_web, std::move(done));
        return;
    }
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
    if (m_web) {
        asWebView(m_web)->snapshot(std::move(done));
        return;
    }
#endif
    done(QImage());
}

QString HtmlDocumentView::loadedBundleDir() const
{
    return m_bundleDir;
}

void HtmlDocumentView::ensureWebView()
{
#if THOUGHTS_PAGE_VIEW
    if (m_web) {
        return;
    }
    auto *web = new PageWebView();
#ifdef Q_OS_MACOS
    HtmlMacNative::watchUserInput();
#endif
#if THOUGHTS_HAVE_QTWEBVIEW
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::LocalStorageEnabled, false);
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::AllowFileAccess, false);
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::LocalContentCanAccessFileUrls, false);
#else
    // The WebView2 has these fixed (InPrivate, no file access). Decided before the page goes
    // anywhere, WebView2 saying whether the reader clicked: an attachment page keeps the view and
    // its web links (target=_blank too) open outside, as the Started case below does for Qt
    // WebView; an edition's go where they lead, as with Qt WebView.
    web->setNavigationFilter([this](const QUrl &url, bool userGesture, bool newWindow) {
        if (!m_attachment || !m_pageUrl.isValid() || (!newWindow && isLoadedPage(url))) {
            return true;
        }
        const bool loopback = url.host() == QLatin1String("127.0.0.1") || url.host() == QLatin1String("localhost");
        if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("mailto")) && !loopback) {
            // After WebView2's call returns: the receiver may ask the reader.
            QMetaObject::invokeMethod(
                this,
                [this, url, userGesture] {
                    Q_EMIT externalLinkRequested(url, userGesture);
                },
                Qt::QueuedConnection);
        }
        return false;
    });
#endif
    m_web = web;
    m_container = QWidget::createWindowContainer(web, m_stackHost);
    m_container->setObjectName(QStringLiteral("thoughtsHtmlViewContainer"));
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_stack->addWidget(m_container);
    connect(web, &PageWebView::loadingChanged, this, [this](const PageLoadingInfo &info) {
        // Nothing shown (unload()): about:blank gets no viewer.
        if (!m_pageUrl.isValid()) {
            return;
        }
        // An attachment page keeps the view: a navigation away is stopped (web links open
        // outside), and a foreign page that loaded anyway is replaced by the attachment again.
        if (m_attachment && !isLoadedPage(info.url())) {
            if (info.status() == PageLoadingInfo::LoadStatus::Started) {
                asWebView(m_web)->stop();
                const QUrl url = info.url();
                const bool loopback = url.host() == QLatin1String("127.0.0.1") || url.host() == QLatin1String("localhost");
                if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("mailto")) && !loopback) {
                    // The page's own script can navigate too (location = …), with anything it
                    // knows in the address: only the reader's click opens a link outside.
                    bool userGesture = false;
#ifdef Q_OS_MACOS
                    userGesture = isUserGesture(HtmlMacNative::millisecondsSinceUserInput(m_web));
#endif
                    Q_EMIT externalLinkRequested(url, userGesture);
                }
            } else if (info.status() == PageLoadingInfo::LoadStatus::Succeeded) {
                asWebView(m_web)->setUrl(m_pageUrl);
            }
            return;
        }
        if (info.status() == PageLoadingInfo::LoadStatus::Started && m_attachment) {
            installNativeTransport();
        }
        if (info.status() == PageLoadingInfo::LoadStatus::Succeeded) {
            injectViewer();
        } else if (info.status() == PageLoadingInfo::LoadStatus::Failed) {
            m_ready = false;
            Q_EMIT loadFailed(info.errorString());
        }
    });
    m_server = new HtmlEditionServer(this);
    connect(m_server, &HtmlEditionServer::message, this, &HtmlDocumentView::onRawMessage);
#endif
}

void HtmlDocumentView::loadEdition(const QString &bundleDir, const QString &entry, const QJsonObject &options)
{
#if THOUGHTS_PAGE_VIEW
    if (!webViewAvailable()) {
        Q_EMIT loadFailed(tr("there is no web view here"));
        return;
    }
    ensureWebView();
    ++m_loadGeneration;
    m_ready = false;
    m_nextSeq = 1;
    m_held.clear();
    m_loadOptions = options;
    m_bundleDir = bundleDir;
    m_attachment = false;
    destroyLive();
    m_pollTimer.stop();
    m_transportKind = QStringLiteral("http");
    if (!m_server->serve(bundleDir)) {
        Q_EMIT loadFailed(tr("The HTML edition could not be opened."));
        return;
    }
    m_stack->setCurrentWidget(m_container);
    m_pageUrl = m_server->entryUrl(entry);
    asWebView(m_web)->setUrl(m_pageUrl);
#else
    Q_UNUSED(bundleDir)
    Q_UNUSED(entry)
    Q_UNUSED(options)
    Q_EMIT loadFailed(tr("this build of Thoughts has no web view"));
#endif
}

void HtmlDocumentView::loadAttachment(const QUrl &url, const QJsonObject &options)
{
    ++m_loadGeneration;
    m_ready = false;
    m_nextSeq = 1;
    m_held.clear();
    m_pendingCalls.clear();
    m_loadOptions = options;
    m_loadOptions.insert(QStringLiteral("mode"), QStringLiteral("attachment"));
    m_bundleDir.clear();
    m_attachment = true;
    m_pageUrl = url;
    destroyLive();
    m_pollTimer.stop();
#if THOUGHTS_PAGE_VIEW
    if (!webViewAvailable()) {
        Q_EMIT loadFailed(tr("there is no web view here"));
        return;
    }
    ensureWebView();
    m_stack->setCurrentWidget(m_container);
    installNativeTransport();
    asWebView(m_web)->setUrl(url);
#else
    Q_EMIT loadFailed(tr("this build of Thoughts has no web view"));
#endif
}

void HtmlDocumentView::unload()
{
    ++m_loadGeneration;
    m_ready = false;
    m_held.clear();
    m_pendingCalls.clear();
    m_pollTimer.stop();
    m_attachment = false;
    m_pageUrl = QUrl();
    m_transportKind.clear();
    destroyLive();
#if THOUGHTS_PAGE_VIEW
    if (m_web) {
        asWebView(m_web)->setUrl(QUrl(QStringLiteral("about:blank")));
    }
#endif
    showMessage(QString());
}

void HtmlDocumentView::loadLive(const QUrl &url)
{
    // Nothing of an embedded page or edition keeps running behind it.
    unload();
    m_live = true;
    m_liveLoaded = false;
    m_liveUrl = url;
    m_transportKind = QStringLiteral("live");
    if (!HtmlLiveWebView::available()) {
        Q_EMIT liveLoadFailed(tr("there is no web view for live pages here"));
        return;
    }
    m_liveWeb = new HtmlLiveWebView(this);
    connect(m_liveWeb, &HtmlLiveWebView::loadCommitted, this, [this] {
        m_liveTimeout.stop();
        if (m_liveContainer) m_stack->setCurrentWidget(m_liveContainer);
    });
    connect(m_liveWeb, &HtmlLiveWebView::loadFinished, this, [this](const QUrl &loaded) {
        m_liveTimeout.stop();
        m_liveLoaded = true;
        if (m_liveContainer) m_stack->setCurrentWidget(m_liveContainer);
        Q_EMIT liveLoaded(loaded);
    });
    connect(m_liveWeb, &HtmlLiveWebView::loadFailed, this, [this](const QString &error) {
        m_liveTimeout.stop();
        m_liveLoaded = false;
        Q_EMIT liveLoadFailed(error);
    });
    connect(m_liveWeb, &HtmlLiveWebView::linkRequested, this, &HtmlDocumentView::externalLinkRequested);
    connect(m_liveWeb, &HtmlLiveWebView::selectionMenuRequested, this, &HtmlDocumentView::liveSelectionMenuRequested);
    connect(m_liveWeb, &HtmlLiveWebView::selectionChoice, this, &HtmlDocumentView::liveSelectionChoice);
    if (m_liveSelectionFeedback) m_liveWeb->setSelectionFeedbackProvider(m_liveSelectionFeedback);
    if (m_liveWeb->window()) {
        m_liveContainer = QWidget::createWindowContainer(m_liveWeb->window(), m_stackHost);
        m_liveContainer->setObjectName(QStringLiteral("thoughtsHtmlLiveContainer"));
        m_liveContainer->setFocusPolicy(Qt::StrongFocus);
        m_stack->addWidget(m_liveContainer);
    }
    showMessage(tr("Loading %1\u2026").arg(url.toDisplayString()));
    startLiveLoad();
    m_liveWeb->load(url);
}

void HtmlDocumentView::startLiveLoad()
{
    m_liveTimeout.start();
}

void HtmlDocumentView::reloadLive()
{
    if (!m_live) {
        return;
    }
    if (!m_liveWeb) {
        loadLive(m_liveUrl);
        return;
    }
    // From a message (a failure): "Loading…" until the page shows again.
    if (m_stack->currentWidget() != m_liveContainer) {
        showMessage(tr("Loading %1\u2026").arg(m_liveUrl.toDisplayString()));
    }
    m_liveLoaded = false;
    startLiveLoad();
    m_liveWeb->reload();
}

void HtmlDocumentView::callLive(const QString &body, const QJsonObject &args, std::function<void(bool, const QString &, const QString &)> done)
{
    if (!m_live || !m_liveWeb) {
        done(false, QString(), tr("no live page is shown"));
        return;
    }
    m_liveWeb->callAsync(body, args, std::move(done));
}

void HtmlDocumentView::setLiveSelectionFeedbackProvider(std::function<bool()> provider)
{
    m_liveSelectionFeedback = std::move(provider);
    if (m_liveWeb) m_liveWeb->setSelectionFeedbackProvider(m_liveSelectionFeedback);
}

void HtmlDocumentView::copyLiveSelection()
{
    if (m_live && m_liveWeb) {
        m_liveWeb->copySelection();
    }
}

void HtmlDocumentView::destroyLive()
{
    m_liveTimeout.stop();
    m_live = false;
    m_liveLoaded = false;
    m_liveUrl = QUrl();
    // The container (and with it the foreign window) before the web view it shows.
    if (m_liveContainer) {
        if (m_stack) m_stack->removeWidget(m_liveContainer);
        delete m_liveContainer;
        m_liveContainer = nullptr;
    }
    delete m_liveWeb;
    m_liveWeb = nullptr;
}

bool HtmlDocumentView::isLoadedPage(const QUrl &url) const
{
    // The same document: only the fragment (the page's #hash routes) may differ.
    return url.adjusted(QUrl::RemoveFragment) == m_pageUrl.adjusted(QUrl::RemoveFragment);
}

void HtmlDocumentView::installNativeTransport()
{
#if THOUGHTS_HAVE_QTWEBVIEW && defined(Q_OS_MACOS)
    if (m_nativeHandler || !m_web) {
        return;
    }
    QPointer<HtmlDocumentView> self(this);
    m_nativeHandler = HtmlMacNative::addMessageHandler(m_web, QStringLiteral("thoughts"), [self](const QByteArray &body) {
        if (self && self->m_attachment) {
            self->onRawMessage(body);
        }
    });
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
    if (m_nativeHandler || !m_web) {
        return;
    }
    // window.chrome.webview.postMessage: no network, so it works under the attachment CSP.
    QPointer<HtmlDocumentView> self(this);
    asWebView(m_web)->setMessageHandler([self](const QByteArray &body, const QUrl &source) {
        // From the attachment's own document only.
        if (self && self->m_attachment && self->isLoadedPage(source)) {
            self->onRawMessage(body);
        }
    });
    m_nativeHandler = m_web;
#endif
}

void HtmlDocumentView::removeNativeTransport()
{
#if THOUGHTS_HAVE_QTWEBVIEW && defined(Q_OS_MACOS)
    if (m_nativeHandler) {
        HtmlMacNative::removeMessageHandler(m_nativeHandler, m_web, QStringLiteral("thoughts"));
        m_nativeHandler = nullptr;
    }
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
    if (m_nativeHandler) {
        if (m_web) asWebView(m_web)->setMessageHandler({});
        m_nativeHandler = nullptr;
    }
#endif
}

void HtmlDocumentView::pollQueue()
{
#if THOUGHTS_PAGE_VIEW
    if (!m_web || !m_attachment || m_polling) {
        return;
    }
    m_polling = true;
    const int generation = m_loadGeneration;
    QPointer<HtmlDocumentView> self(this);
    asWebView(m_web)->runJavaScript(QStringLiteral("typeof ThoughtsViewer === 'undefined' ? '[]' : ThoughtsViewer.drain()"), [self, generation](const QVariant &result) {
        if (!self) {
            return;
        }
        self->m_polling = false;
        if (generation != self->m_loadGeneration) {
            return;
        }
        const QJsonArray messages = QJsonDocument::fromJson(result.toString().toUtf8()).array();
        for (const QJsonValue &value : messages) {
            if (value.isObject()) {
                self->onMessage(value.toObject());
            }
        }
    });
#endif
}

void HtmlDocumentView::injectViewer()
{
#if THOUGHTS_PAGE_VIEW
    if (!m_attachment) {
        startViewer();
        return;
    }
    // A page's own scripts may report a load again for the same document (#hash routes): the
    // viewer is injected once per document (a reload is a new document without ThoughtsViewer).
    const int generation = m_loadGeneration;
    QPointer<HtmlDocumentView> self(this);
    asWebView(m_web)->runJavaScript(QStringLiteral("typeof ThoughtsViewer"), [self, generation](const QVariant &result) {
        if (self && generation == self->m_loadGeneration && result.toString() != QLatin1String("object")) {
            self->startViewer();
        }
    });
#endif
}

void HtmlDocumentView::startViewer()
{
#if THOUGHTS_PAGE_VIEW
    // A new document context: its messages count from 1 again.
    m_ready = false;
    m_nextSeq = 1;
    m_held.clear();
    QString script;
    for (const QString &name : {QStringLiteral("geometry.js"), QStringLiteral("text.js"), QStringLiteral("viewer.js")}) {
        script += resourceText(name) + QStringLiteral("\n;\n");
    }
    QJsonObject options = m_loadOptions;
    if (!m_attachment) {
        options.insert(QStringLiteral("transport"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("http")}, {QStringLiteral("url"), m_server->messageUrl().path()}});
    } else {
        // No network for attachments: the native message handler, else the queue polled by Qt.
        installNativeTransport();
#if defined(THOUGHTS_HAVE_WEBVIEW2)
        m_transportKind = m_nativeHandler ? QStringLiteral("webview2") : QStringLiteral("queue");
        options.insert(QStringLiteral("transport"), QJsonObject{{QStringLiteral("kind"), m_transportKind}});
#else
        m_transportKind = m_nativeHandler ? QStringLiteral("webkit") : QStringLiteral("queue");
        options.insert(QStringLiteral("transport"), m_nativeHandler ? QJsonObject{{QStringLiteral("kind"), QStringLiteral("webkit")}, {QStringLiteral("handler"), QStringLiteral("thoughts")}}
                                                                    : QJsonObject{{QStringLiteral("kind"), QStringLiteral("queue")}});
#endif
        if (m_nativeHandler) {
            m_pollTimer.stop();
        } else {
            m_pollTimer.start();
        }
    }
    options.insert(QStringLiteral("css"), resourceText(QStringLiteral("viewer.css")));
    if (!options.contains(QStringLiteral("theme"))) {
        options.insert(QStringLiteral("theme"), currentTheme());
    }
    const QString argsJson = QString::fromUtf8(QJsonDocument(QJsonArray{options}).toJson(QJsonDocument::Compact));
    script += QStringLiteral("ThoughtsViewer.call(\"load\", %1);").arg(jsString(argsJson));
    const int generation = m_loadGeneration;
    asWebView(m_web)->runJavaScript(script, [this, generation](const QVariant &result) {
        if (generation != m_loadGeneration) {
            return;
        }
        const QJsonObject reply = QJsonDocument::fromJson(result.toString().toUtf8()).object();
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            Q_EMIT loadFailed(reply.value(QStringLiteral("error")).toString(QStringLiteral("the viewer did not start")));
        }
    });
#endif
}

void HtmlDocumentView::call(const QString &name, const QJsonArray &args, CallCallback callback)
{
#if THOUGHTS_PAGE_VIEW
    if (!webViewAvailable()) {
        if (callback) {
            callback(false, QJsonValue(), QStringLiteral("no web view"));
        }
        return;
    }
    if (!m_web || !m_ready) {
        m_pendingCalls.append({name, args, std::move(callback)});
        return;
    }
    const QString argsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("ThoughtsViewer.call(%1, %2)").arg(jsString(name), jsString(argsJson));
    QPointer<HtmlDocumentView> self(this);
    const int generation = m_loadGeneration;
    asWebView(m_web)->runJavaScript(script, [self, generation, callback](const QVariant &result) {
        if (!callback) {
            return;
        }
        if (!self || generation != self->m_loadGeneration) {
            callback(false, QJsonValue(), QStringLiteral("page changed"));
            return;
        }
        const QJsonObject reply = QJsonDocument::fromJson(result.toString().toUtf8()).object();
        callback(reply.value(QStringLiteral("ok")).toBool(), reply.value(QStringLiteral("result")), reply.value(QStringLiteral("error")).toString());
    });
#else
    Q_UNUSED(name)
    Q_UNUSED(args)
    if (callback) {
        callback(false, QJsonValue(), QStringLiteral("no web view"));
    }
#endif
}

void HtmlDocumentView::runScript(const QString &script, std::function<void(const QVariant &)> callback)
{
#if THOUGHTS_PAGE_VIEW
    if (m_web) {
        asWebView(m_web)->runJavaScript(script, callback ? callback : std::function<void(const QVariant &)>());
        return;
    }
#else
    Q_UNUSED(script)
#endif
    if (callback) {
        callback(QVariant());
    }
}

void HtmlDocumentView::flushPendingCalls()
{
    const auto pending = std::exchange(m_pendingCalls, {});
    for (const PendingCall &pendingCall : pending) {
        call(pendingCall.name, pendingCall.args, pendingCall.callback);
    }
}

void HtmlDocumentView::onRawMessage(const QByteArray &body)
{
    onMessage(QJsonDocument::fromJson(body).object());
}

void HtmlDocumentView::onMessage(const QJsonObject &message)
{
    if (message.isEmpty()) {
        return;
    }
    const qint64 seq = message.value(QStringLiteral("seq")).toInteger(-1);
    if (seq < 0) {
        deliver(message);
        return;
    }
    if (seq < m_nextSeq) {
        // A new page context restarts at 1; anything older than what was delivered is a duplicate.
        if (seq == 1) {
            m_nextSeq = 1;
            m_held.clear();
        } else {
            return;
        }
    }
    m_held.insert(seq, message);
    flushHeld(false);
}

void HtmlDocumentView::flushHeld(bool force)
{
    while (!m_held.isEmpty()) {
        auto first = m_held.begin();
        if (first.key() != m_nextSeq && !force) {
            m_holdTimer.start();
            return;
        }
        const QJsonObject message = first.value();
        m_nextSeq = first.key() + 1;
        m_held.erase(first);
        deliver(message);
    }
    m_holdTimer.stop();
}

void HtmlDocumentView::deliver(const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ready")) {
        m_ready = true;
        Q_EMIT ready(message);
        flushPendingCalls();
    } else if (type == QLatin1String("focus")) {
        m_pageFocused = true;
        if (m_container && !m_container->hasFocus()) {
            m_container->setFocus(Qt::MouseFocusReason);
        }
    } else if (type == QLatin1String("blur")) {
        m_pageFocused = false;
    }
    Q_EMIT viewerMessage(message);
}

QString HtmlDocumentView::themeForPalette() const
{
    return palette().color(QPalette::Window).lightness() < 128 ? QStringLiteral("dark") : QStringLiteral("light");
}

QString HtmlDocumentView::currentTheme() const
{
    return m_theme == QLatin1String("system") ? themeForPalette() : m_theme;
}

void HtmlDocumentView::setTheme(const QString &theme)
{
    m_theme = theme.isEmpty() ? QStringLiteral("system") : theme;
#ifdef Q_OS_MACOS
    // An attachment keeps its own look (it follows the app's appearance by itself).
    if (m_web && !m_attachment) {
        HtmlMacNative::setAppearance(m_web, currentTheme() == QLatin1String("dark") ? 2 : 1);
    }
#elif defined(THOUGHTS_HAVE_WEBVIEW2)
    if (m_web && !m_attachment) {
        asWebView(m_web)->setAppearance(currentTheme() == QLatin1String("dark") ? 2 : 1);
    }
#endif
    if (m_ready) {
        call(QStringLiteral("setTheme"), QJsonArray{currentTheme()});
    }
}

void HtmlDocumentView::focusPage()
{
    if (QWidget *container = webContainer()) {
        container->setFocus(Qt::OtherFocusReason);
    }
}

void HtmlDocumentView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) && m_theme == QLatin1String("system")) {
        setTheme(m_theme);
    }
}

void HtmlDocumentView::setNotice(const QString &text, const QString &buttonText)
{
    m_notice->setText(text);
    m_notice->setVisible(!text.isEmpty());
    m_noticeButton->setText(buttonText);
    m_noticeButton->setVisible(!text.isEmpty() && !buttonText.isEmpty());
}

QString HtmlDocumentView::notice() const
{
    return m_notice->isVisible() || !m_notice->isHidden() ? m_notice->text() : QString();
}

void HtmlDocumentView::setTextSizeHandler(std::function<void(int)> handler)
{
    m_textSizeHandler = std::move(handler);
}

void HtmlDocumentView::setTextSizeLabel(int percent)
{
    m_textSize->setText(QStringLiteral("%1 %").arg(percent));
}

void HtmlDocumentView::setReadAloudHostVisible(bool visible, int barHeight)
{
    m_readAloudHost->setFixedHeight(visible ? barHeight + 24 : 0);
    m_readAloudHost->setVisible(visible);
}

void HtmlDocumentView::setAttachmentBar(const QString &backText, const QString &backTip)
{
    m_smaller->hide();
    m_larger->hide();
    m_textSize->hide();
    m_openInBrowser->show();
    m_back->setText(backText);
    m_back->setToolTip(backTip);
}

void HtmlDocumentView::setSelectionAction(const QString &text, bool visible, bool enabled, const QString &toolTip)
{
    m_selectionAction->setText(text);
    m_selectionAction->setToolTip(toolTip);
    m_selectionAction->setVisible(visible);
    m_selectionAction->setEnabled(enabled);
}

QString HtmlDocumentView::selectionActionText() const
{
    return m_selectionAction->isHidden() ? QString() : m_selectionAction->text();
}

bool HtmlDocumentView::selectionActionEnabled() const
{
    return !m_selectionAction->isHidden() && m_selectionAction->isEnabled();
}

QString HtmlDocumentView::selectionActionToolTip() const
{
    return m_selectionAction->toolTip();
}

void HtmlDocumentView::setLiveBar(const QString &text, bool takeSnapshot)
{
    m_liveLabel->setText(text);
    m_liveLabel->setToolTip(text);
    m_liveLabel->setVisible(!text.isEmpty());
    m_reload->setVisible(!text.isEmpty());
    m_takeSnapshot->setVisible(!text.isEmpty() && takeSnapshot);
}

QString HtmlDocumentView::liveBarText() const
{
    return m_liveLabel->isHidden() ? QString() : m_liveLabel->text();
}

void HtmlDocumentView::setTakeSnapshotAction(bool enabled, const QString &toolTip)
{
    m_takeSnapshot->setEnabled(enabled);
    m_takeSnapshot->setToolTip(toolTip);
}

bool HtmlDocumentView::takeSnapshotEnabled() const
{
    return !m_takeSnapshot->isHidden() && m_takeSnapshot->isEnabled();
}

QString HtmlDocumentView::takeSnapshotToolTip() const
{
    return m_takeSnapshot->toolTip();
}
}
