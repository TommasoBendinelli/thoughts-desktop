// SPDX-License-Identifier: GPL-2.0-or-later
#include "syncstatewatch.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace Thoughts
{

QString SyncStateWatch::capability()
{
    return QStringLiteral("sync_state_wait_v1");
}

SyncStateWatch::SyncStateWatch(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!m_running) return;
        if (m_pause == Pause::Spacing) emitDue();
        else due();
    });
}

SyncStateWatch::~SyncStateWatch()
{
    stop();
}

void SyncStateWatch::setServer(const QUrl &baseUrl, const QByteArray &token)
{
    m_link.setServer(baseUrl, token);
    m_unlinked.invalidate();
    // Another server: what the last one could do and its state's version say nothing about it.
    m_capable = Capable::Unknown;
    m_version.clear();
}

void SyncStateWatch::setTimingForTesting(int plainPauseMs, int heldWaitMs, int shortestHeldMs, int checkSpacingMs, int heldMarginMs)
{
    m_heldMarginMs = heldMarginMs > 0 ? heldMarginMs : HeldTransferMarginMs;
    m_plainPauseMs = plainPauseMs > 0 ? plainPauseMs : PlainPauseMs;
    m_heldWaitMs = heldWaitMs > 0 ? heldWaitMs : HeldWaitMs;
    m_shortestHeldMs = shortestHeldMs > 0 ? shortestHeldMs : ShortestHeldMs;
    m_checkSpacingMs = checkSpacingMs > 0 ? checkSpacingMs : ShortestCheckSpacingMs;
}

void SyncStateWatch::start()
{
    if (m_running) return;
    m_running = true;
    // The first pause is a plain one: nothing is asked of the server, nor read from the
    // Keychain, while the app starts.
    m_pauseBegan.start();
    plainPause();
}

void SyncStateWatch::stop()
{
    m_running = false;
    m_pause = Pause::None;
    if (m_timer) m_timer->stop();
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply.clear();
        reply->abort();
    }
}

QNetworkAccessManager *SyncStateWatch::network()
{
    if (!m_network) m_network = new QNetworkAccessManager(this);
    return m_network;
}

bool SyncStateWatch::linked()
{
    // Reading the token runs the Keychain's tool: while there is none, or the server refused
    // it, it is read again only now and then. The plain pauses go on meanwhile.
    if (m_unlinked.isValid() && m_unlinked.elapsed() < CapabilitiesKeptMs) return false;
    if (m_link.ensure()) {
        m_unlinked.invalidate();
        return true;
    }
    m_unlinked.start();
    return false;
}

void SyncStateWatch::refused()
{
    m_link.tokenRefused();
    m_unlinked.start();
    m_capable = Capable::Unknown;
}

void SyncStateWatch::beginPause()
{
    m_pauseBegan.start();
    if (!linked()) {
        plainPause();
        return;
    }
    if (m_capable == Capable::Unknown || (m_capable == Capable::No && m_capabilitiesAnswered.isValid() && m_capabilitiesAnswered.elapsed() >= CapabilitiesKeptMs)) {
        askCapabilities();
        return;
    }
    if (m_capable == Capable::Yes) sendHeld();
    else plainPause();
}

void SyncStateWatch::plainPause()
{
    m_pause = Pause::Plain;
    // What is left of the plain pause: a held request that failed late does not add another.
    m_timer->start(int(qMax<qint64>(0, m_plainPauseMs - m_pauseBegan.elapsed())));
}

void SyncStateWatch::askCapabilities()
{
    m_pause = Pause::AskingCapabilities;
    QNetworkRequest request = m_link.request(QStringLiteral("/v1/health"), 15000, "thoughts-desktop-sync-state/1");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_reply.data() || !m_running) return;
        m_reply.clear();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403) refused();
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            // A failure leaves it unknown: asked again with the next pause.
            m_capable = serverCapabilities(reply->readAll()).contains(capability()) ? Capable::Yes : Capable::No;
            m_capabilitiesAnswered.start();
        }
        if (m_capable == Capable::Yes) sendHeld();
        else plainPause();
    });
}

void SyncStateWatch::sendHeld()
{
    m_pause = Pause::Held;
    QNetworkRequest request = m_link.request(QStringLiteral("/v2/sync/state?wait_ms=%1").arg(m_heldWaitMs), m_heldWaitMs + m_heldMarginMs, "thoughts-desktop-sync-state/1");
    request.setRawHeader("Accept", "application/json");
    // Without a version (the first request) the server answers at once with the one it has.
    m_heldConditional = !m_version.isEmpty();
    if (m_heldConditional) request.setRawHeader("If-None-Match", m_version);
    m_heldSent.start();
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { heldFinished(reply); });
}

void SyncStateWatch::heldFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply.data() || !m_running) return;
    m_reply.clear();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray answered = reply->rawHeader("ETag");
    if (status == 401 || status == 403) refused();
    if (reply->error() != QNetworkReply::NoError || (status != 200 && status != 304)) {
        // A failed held request: this pause is a plain one.
        plainPause();
        return;
    }
    if (status == 304) {
        if (!answered.isEmpty()) m_version = answered;
        if (m_heldSent.elapsed() < m_shortestHeldMs) {
            // Answered at once, without holding it: plain pauses until the server is asked again.
            m_capable = Capable::No;
            m_capabilitiesAnswered.start();
            plainPause();
            return;
        }
        // The wait is over and nothing changed: the check that ends every pause.
        due();
        return;
    }
    // 200: the state the server holds now.
    if (answered.isEmpty()) {
        // Without a version the next request could not be held.
        m_version.clear();
        plainPause();
        return;
    }
    const bool conditional = m_heldConditional;
    const bool changed = answered != m_version;
    m_version = answered;
    if (!conditional) {
        // The first answer only names the version to wait on: the check before this pause read
        // the same state.
        sendHeld();
        return;
    }
    if (!changed) {
        // The server did not compare the versions: it cannot hold this request.
        plainPause();
        return;
    }
    due();
}

void SyncStateWatch::due()
{
    m_pause = Pause::None;
    if (m_lastDue.isValid() && m_lastDue.elapsed() < m_checkSpacingMs) {
        m_pause = Pause::Spacing;
        m_timer->start(int(m_checkSpacingMs - m_lastDue.elapsed()));
        return;
    }
    emitDue();
}

void SyncStateWatch::emitDue()
{
    m_pause = Pause::None;
    m_lastDue.start();
    Q_EMIT checkDue();
    // The next pause follows the check (unless a handler stopped the watch or began one).
    if (m_running && m_pause == Pause::None && !m_reply) beginPause();
}

}
