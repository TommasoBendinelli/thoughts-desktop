// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmldocumentview.h"

#include "htmleditionserver.h"
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
QWebView *asWebView(QWindow *window)
{
    return static_cast<QWebView *>(window);
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
    m_stack = new QStackedLayout(stackHost);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_messageLabel = new QLabel(stackHost);
    m_messageLabel->setObjectName(QStringLiteral("thoughtsHtmlViewMessage"));
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_messageLabel->setMargin(24);
    m_stack->addWidget(m_messageLabel);
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(120);
    connect(&m_holdTimer, &QTimer::timeout, this, [this] { flushHeld(true); });
    // The polling transport of attachment pages (no native message handler).
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &HtmlDocumentView::pollQueue);
    if (!webViewAvailable()) {
        showMessage(tr("HTML not available: there is no web view here."));
    }
#ifdef Q_OS_MACOS
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        // Programmatic focus elsewhere in our window: keys must leave the WKWebView (Spike 2).
        if (!m_web || !now || now == m_container || now->window() != window()) {
            return;
        }
        if (HtmlMacNative::webViewHasNativeFocus(m_web)) {
            HtmlMacNative::focusQt(now);
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
}

bool HtmlDocumentView::webViewAvailable()
{
#if THOUGHTS_HAVE_QTWEBVIEW
    // Native web views need a real window system (not the offscreen platform of tests).
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
#else
    return false;
#endif
}

void HtmlDocumentView::showMessage(const QString &text)
{
    m_messageLabel->setText(text);
    m_stack->setCurrentWidget(m_messageLabel);
}

QString HtmlDocumentView::message() const
{
    return m_stack->currentWidget() == m_messageLabel ? m_messageLabel->text() : QString();
}

QWindow *HtmlDocumentView::webWindow() const
{
    return m_web;
}

QWidget *HtmlDocumentView::webContainer() const
{
    return m_container;
}

void HtmlDocumentView::snapshotPage(std::function<void(const QImage &)> done)
{
#ifdef Q_OS_MACOS
    if (m_web) {
        HtmlMacNative::snapshot(m_web, std::move(done));
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
#if THOUGHTS_HAVE_QTWEBVIEW
    if (m_web) {
        return;
    }
    auto *web = new QWebView();
#ifdef Q_OS_MACOS
    HtmlMacNative::watchUserInput();
#endif
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::LocalStorageEnabled, false);
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::AllowFileAccess, false);
    web->settings()->setAttribute(QWebViewSettings::WebAttribute::LocalContentCanAccessFileUrls, false);
    m_web = web;
    m_container = QWidget::createWindowContainer(web, m_messageLabel->parentWidget());
    m_container->setObjectName(QStringLiteral("thoughtsHtmlViewContainer"));
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_stack->addWidget(m_container);
    connect(web, &QWebView::loadingChanged, this, [this](const QWebViewLoadingInfo &info) {
        // Nothing shown (unload()): about:blank gets no viewer.
        if (!m_pageUrl.isValid()) {
            return;
        }
        // An attachment page keeps the view: a navigation away is stopped (web links open
        // outside), and a foreign page that loaded anyway is replaced by the attachment again.
        if (m_attachment && !isLoadedPage(info.url())) {
            if (info.status() == QWebViewLoadingInfo::LoadStatus::Started) {
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
            } else if (info.status() == QWebViewLoadingInfo::LoadStatus::Succeeded) {
                asWebView(m_web)->setUrl(m_pageUrl);
            }
            return;
        }
        if (info.status() == QWebViewLoadingInfo::LoadStatus::Started && m_attachment) {
            installNativeTransport();
        }
        if (info.status() == QWebViewLoadingInfo::LoadStatus::Succeeded) {
            injectViewer();
        } else if (info.status() == QWebViewLoadingInfo::LoadStatus::Failed) {
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
#if THOUGHTS_HAVE_QTWEBVIEW
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
    m_pollTimer.stop();
#if THOUGHTS_HAVE_QTWEBVIEW
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
#if THOUGHTS_HAVE_QTWEBVIEW
    if (m_web) {
        asWebView(m_web)->setUrl(QUrl(QStringLiteral("about:blank")));
    }
#endif
    showMessage(QString());
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
#endif
}

void HtmlDocumentView::removeNativeTransport()
{
#if THOUGHTS_HAVE_QTWEBVIEW && defined(Q_OS_MACOS)
    if (m_nativeHandler) {
        HtmlMacNative::removeMessageHandler(m_nativeHandler, m_web, QStringLiteral("thoughts"));
        m_nativeHandler = nullptr;
    }
#endif
}

void HtmlDocumentView::pollQueue()
{
#if THOUGHTS_HAVE_QTWEBVIEW
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
#if THOUGHTS_HAVE_QTWEBVIEW
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
#if THOUGHTS_HAVE_QTWEBVIEW
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
        m_transportKind = m_nativeHandler ? QStringLiteral("webkit") : QStringLiteral("queue");
        options.insert(QStringLiteral("transport"), m_nativeHandler ? QJsonObject{{QStringLiteral("kind"), QStringLiteral("webkit")}, {QStringLiteral("handler"), QStringLiteral("thoughts")}}
                                                                    : QJsonObject{{QStringLiteral("kind"), QStringLiteral("queue")}});
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
#if THOUGHTS_HAVE_QTWEBVIEW
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
#if THOUGHTS_HAVE_QTWEBVIEW
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
#endif
    if (m_ready) {
        call(QStringLiteral("setTheme"), QJsonArray{currentTheme()});
    }
}

void HtmlDocumentView::focusPage()
{
    if (m_container) {
        m_container->setFocus(Qt::OtherFocusReason);
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
}
