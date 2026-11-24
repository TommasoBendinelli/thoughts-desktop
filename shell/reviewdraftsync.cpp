// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewdraftsync.h"

#include "../part/thoughtsserveraddress.h"
#include "developerlog.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

namespace Thoughts
{

namespace
{
QDateTime draftTime(const QJsonValue &value)
{
    const QString text = value.toString();
    if (text.isEmpty()) return {};
    const QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    return time.isValid() ? time.toUTC() : QDateTime();
}

QString draftTimeText(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

int statusOf(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// A failure worth trying again: no answer, the server busy or down, or a token to read again.
bool transient(int status)
{
    return status == 0 || status == 401 || status == 403 || status == 408 || status == 429 || status >= 500;
}
}

std::optional<ReviewDraftSync::Draft> ReviewDraftSync::Draft::fromJson(const QJsonObject &json)
{
    const QString cardId = json.value(QLatin1String("card_id")).toString().trimmed();
    if (cardId.isEmpty()) return std::nullopt;
    Draft draft;
    draft.cardId = cardId.toLower();
    draft.deleted = json.value(QLatin1String("deleted")).toBool();
    draft.reason = json.value(QLatin1String("reason")).toString();
    for (const QJsonValue &tag : json.value(QLatin1String("before_tags")).toArray()) {
        if (tag.isString() && !tag.toString().isEmpty() && !draft.tags.contains(tag.toString())) draft.tags.append(tag.toString());
    }
    draft.note = json.value(QLatin1String("before_note")).toString();
    draft.basedOnAnswerAt = draftTime(json.value(QLatin1String("based_on_answer_at")));
    draft.revision = json.value(QLatin1String("revision")).toInteger();
    draft.sequence = json.value(QLatin1String("sequence")).toInteger();
    draft.updatedAt = draftTime(json.value(QLatin1String("updated_at")));
    draft.fromThisDevice = json.value(QLatin1String("from_this_device")).toBool();
    return draft;
}

QString ReviewDraftSync::capability()
{
    return QStringLiteral("review_drafts_v1");
}

QString ReviewDraftSync::answeredReason()
{
    return QStringLiteral("answered");
}

QString ReviewDraftSync::clearedReason()
{
    return QStringLiteral("cleared");
}

ReviewDraftSync::ReviewDraftSync(QObject *parent)
    : QObject(parent)
    , m_changesTimer(new QTimer(this))
    , m_unwatchTimer(new QTimer(this))
{
    qRegisterMetaType<Thoughts::ReviewDraftSync::Draft>();
    m_changesTimer->setSingleShot(true);
    connect(m_changesTimer, &QTimer::timeout, this, &ReviewDraftSync::retryChanges);
    m_unwatchTimer->setSingleShot(true);
    m_unwatchTimer->setInterval(UnwatchDelayMs);
    connect(m_unwatchTimer, &QTimer::timeout, this, [this] {
        if (m_watching) return;
        m_changesTimer->stop();
        if (QNetworkReply *reply = m_changesReply.data()) {
            m_changesReply.clear();
            reply->abort();
        }
    });
    // Nothing is left waiting for the server when the app quits.
    if (QCoreApplication *app = QCoreApplication::instance()) connect(app, &QCoreApplication::aboutToQuit, this, &ReviewDraftSync::stop);
}

ReviewDraftSync::~ReviewDraftSync()
{
    stop();
}

void ReviewDraftSync::setServer(const QUrl &baseUrl, const QByteArray &token)
{
    m_link.setServer(baseUrl, token);
    m_explicitServer = true;
    // Another server: what the last one could do and its cursor say nothing about it.
    m_linked = false;
    m_unlinked.invalidate();
    m_capable = Capable::Unknown;
    m_cursor = -1;
}

void ReviewDraftSync::setRetryDelaysForTesting(int firstMs, int longestMs)
{
    m_firstRetryMs = firstMs > 0 ? firstMs : FirstRetryMs;
    m_longestRetryMs = longestMs > 0 ? longestMs : LongestRetryMs;
}

QNetworkAccessManager *ReviewDraftSync::network()
{
    if (!m_network) m_network = new QNetworkAccessManager(this);
    return m_network;
}

QNetworkRequest ReviewDraftSync::request(const QString &pathAndQuery, int timeoutMs) const
{
    QNetworkRequest request = m_link.request(pathAndQuery, timeoutMs, "thoughts-desktop-review-drafts/1");
    request.setRawHeader("Accept", "application/json");
    return request;
}

QString ReviewDraftSync::draftPath(const QString &cardKey) const
{
    return QStringLiteral("/v1/review-drafts/") + QString::fromLatin1(QUrl::toPercentEncoding(cardKey));
}

void ReviewDraftSync::write(const QString &cardId, const QStringList &tags, const QString &note, const QDateTime &basedOnAnswerAt)
{
    const QString cardKey = key(cardId);
    if (cardKey.isEmpty() || m_stopped) return;
    Card &card = m_cards[cardKey];
    card.editing = false;
    const Write next{false, {}, tags, note, basedOnAnswerAt.isValid() ? basedOnAnswerAt.toUTC() : QDateTime()};
    // The same content is on its way already.
    if ((card.reply && card.sending == next && !card.pending) || (card.pending == next && !card.failed)) return;
    card.pending = next;
    card.failed = false;
    proceed();
}

void ReviewDraftSync::remove(const QString &cardId, const QString &reason)
{
    const QString cardKey = key(cardId);
    if (cardKey.isEmpty() || m_stopped) return;
    Card &card = m_cards[cardKey];
    card.editing = false;
    const Write next{true, reason.isEmpty() ? clearedReason() : reason, {}, {}, {}};
    if ((card.reply && card.sending == next && !card.pending) || (card.pending == next && !card.failed)) return;
    card.pending = next;
    card.failed = false;
    proceed();
}

void ReviewDraftSync::fetch(const QString &cardId)
{
    const QString cardKey = key(cardId);
    if (cardKey.isEmpty() || m_stopped) return;
    if (m_fetching.value(cardKey)) return;
    m_fetchWaiting.insert(cardKey);
    proceed();
}

void ReviewDraftSync::setEditing(const QString &cardId)
{
    const QString cardKey = key(cardId);
    if (cardKey.isEmpty()) return;
    m_cards[cardKey].editing = true;
}

bool ReviewDraftSync::busy(const QString &cardId) const
{
    const auto found = m_cards.constFind(key(cardId));
    return found != m_cards.cend() && found->busy();
}

bool ReviewDraftSync::hasDraft(const QString &cardId) const
{
    return m_drafts.contains(key(cardId));
}

void ReviewDraftSync::setWatching(bool watching)
{
    if (m_stopped || m_watching == watching) return;
    m_watching = watching;
    if (!watching) {
        // A moment later: the next card of the review shows meanwhile, and the request stays open.
        m_unwatchTimer->start();
        return;
    }
    if (m_unwatchTimer->isActive()) {
        m_unwatchTimer->stop();
        if (!m_changesReply && !m_changesTimer->isActive()) scheduleChanges(0);
        return;
    }
    m_changesFailures = 0;
    // A review begins: the current drafts first (no tombstones), so that nothing heard is older
    // than this review; then the changes after them, as they come.
    m_cursor = -1;
    scheduleChanges(0);
}

void ReviewDraftSync::stop()
{
    m_stopped = true;
    m_watching = false;
    if (m_changesTimer) m_changesTimer->stop();
    if (m_unwatchTimer) m_unwatchTimer->stop();
    QList<QPointer<QNetworkReply>> replies{m_changesReply, m_capabilitiesReply};
    for (const Card &card : std::as_const(m_cards)) replies.append(card.reply);
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_fetching)) replies.append(reply);
    m_changesReply.clear();
    m_capabilitiesReply.clear();
    m_fetching.clear();
    for (const QPointer<QNetworkReply> &reply : std::as_const(replies)) {
        if (reply) reply->abort();
    }
}

bool ReviewDraftSync::ready()
{
    if (m_linked && !m_explicitServer && m_linkGeneration != ServerAddress::connectionGeneration()) {
        // Signed in or out since: another server, or another token.
        m_linked = false;
        m_unlinked.invalidate();
        m_capable = Capable::Unknown;
        m_cursor = -1;
    }
    if (!m_linked) {
        if (m_unlinked.isValid() && m_unlinked.elapsed() < CapabilitiesKeptMs) {
            dropWaiting(QStringLiteral("no_server"));
            return false;
        }
        link();
        return false;
    }
    switch (m_capable) {
    case Capable::Yes:
        return true;
    case Capable::Asking:
        return false;
    case Capable::No:
        if (m_capabilitiesAnswered.isValid() && m_capabilitiesAnswered.elapsed() < CapabilitiesKeptMs) {
            dropWaiting(QStringLiteral("no_capability"));
            return false;
        }
        [[fallthrough]];
    case Capable::Unknown:
        askCapabilities();
        return false;
    }
    return false;
}

void ReviewDraftSync::proceed()
{
    if (m_stopped || !ready()) return;
    QStringList writes;
    for (auto card = m_cards.cbegin(); card != m_cards.cend(); ++card) {
        if (card->pending && !card->reply) writes.append(card.key());
    }
    for (const QString &cardKey : std::as_const(writes)) send(cardKey);
    const QSet<QString> fetches = std::exchange(m_fetchWaiting, {});
    for (const QString &cardKey : fetches) sendFetch(cardKey);
    if (m_watching && !m_changesReply && !m_changesTimer->isActive()) sendChanges();
}

void ReviewDraftSync::link()
{
    if (m_linking) return;
    m_linking = true;
    if (m_explicitServer) {
        QTimer::singleShot(0, this, [this] {
            ServerLink link = m_link;
            const bool ok = link.ensure();
            linked(link, ok);
        });
        return;
    }
    // Reading the token runs the Keychain's tool: never on the GUI thread, which shows the review.
    QPointer<ReviewDraftSync> guard(this);
    ServerLink copy = m_link;
    QThreadPool::globalInstance()->start([guard, copy]() mutable {
        const bool ok = copy.ensure();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, copy, ok] {
                if (guard) guard->linked(copy, ok);
            },
            Qt::QueuedConnection);
    });
}

void ReviewDraftSync::linked(const ServerLink &link, bool ok)
{
    m_linking = false;
    if (m_stopped) return;
    if (!ok) {
        m_unlinked.start();
        DeveloperLog::record(QStringLiteral("review_drafts.no_server"), {}, true);
        dropWaiting(QStringLiteral("no_server"));
        if (m_watching) scheduleChanges(CapabilitiesKeptMs);
        return;
    }
    m_link = link;
    m_linked = true;
    m_unlinked.invalidate();
    m_linkGeneration = ServerAddress::connectionGeneration();
    proceed();
}

void ReviewDraftSync::refused()
{
    m_link.tokenRefused();
    m_linked = false;
    m_unlinked.start();
    m_capable = Capable::Unknown;
    DeveloperLog::record(QStringLiteral("review_drafts.token_refused"));
}

void ReviewDraftSync::askCapabilities()
{
    if (m_capable == Capable::Asking) return;
    m_capable = Capable::Asking;
    QNetworkReply *reply = network()->get(request(QStringLiteral("/v1/health"), 15000));
    m_capabilitiesReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_stopped || reply != m_capabilitiesReply.data()) return;
        m_capabilitiesReply.clear();
        const int status = statusOf(reply);
        if (status == 401 || status == 403) {
            refused();
        } else if (reply->error() == QNetworkReply::NoError && status == 200) {
            m_capable = serverCapabilities(reply->readAll()).contains(capability()) ? Capable::Yes : Capable::No;
            m_capabilitiesAnswered.start();
            DeveloperLog::record(QStringLiteral("review_drafts.capability"), {{QStringLiteral("available"), m_capable == Capable::Yes}}, true);
        } else {
            // Unreachable: what waits stays, for the next change or poll.
            m_capable = Capable::Unknown;
            DeveloperLog::record(QStringLiteral("review_drafts.health_failed"), {{QStringLiteral("status"), status}, {QStringLiteral("error"), reply->errorString()}}, true);
        }
        switch (m_capable) {
        case Capable::Yes:
            proceed();
            return;
        case Capable::No:
            dropWaiting(QStringLiteral("no_capability"));
            if (m_watching) scheduleChanges(CapabilitiesKeptMs);
            return;
        default:
            if (m_watching) retryChangesLater();
            return;
        }
    });
}

void ReviewDraftSync::dropWaiting(const QString &why)
{
    int dropped = 0;
    for (Card &card : m_cards) {
        if (card.pending) ++dropped;
        card.pending.reset();
        card.failed = false;
        if (!card.reply && !card.editing) card.held.reset();
    }
    dropped += int(m_fetchWaiting.size());
    m_fetchWaiting.clear();
    if (dropped > 0) DeveloperLog::record(QStringLiteral("review_drafts.not_sent"), {{QStringLiteral("reason"), why}, {QStringLiteral("requests"), dropped}}, true);
}

void ReviewDraftSync::send(const QString &cardKey)
{
    auto found = m_cards.find(cardKey);
    if (found == m_cards.end() || !found->pending || found->reply) return;
    const Write write = *found->pending;
    found->sending = write;
    found->pending.reset();
    found->failed = false;
    QNetworkRequest put = request(draftPath(cardKey), 30000);
    put.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = nullptr;
    if (write.remove) {
        const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("reason"), write.reason}}).toJson(QJsonDocument::Compact);
        reply = network()->sendCustomRequest(put, "DELETE", body);
    } else {
        const QJsonObject body{{QStringLiteral("before_tags"), QJsonArray::fromStringList(write.tags)},
                               {QStringLiteral("before_note"), write.note},
                               {QStringLiteral("based_on_answer_at"), write.basedOnAnswerAt.isValid() ? QJsonValue(draftTimeText(write.basedOnAnswerAt)) : QJsonValue()}};
        reply = network()->put(put, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    found->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, cardKey, reply] { writeFinished(cardKey, reply); });
}

void ReviewDraftSync::writeFinished(const QString &cardKey, QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_stopped) return;
    auto found = m_cards.find(cardKey);
    if (found == m_cards.end() || found->reply.data() != reply) return;
    found->reply.clear();
    const Write sent = found->sending.value_or(Write{});
    found->sending.reset();
    const int status = statusOf(reply);
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    const QString errorCode = json.value(QLatin1String("error_code")).toString();
    if (status == 200 && reply->error() == QNetworkReply::NoError) {
        const auto record = Draft::fromJson(json);
        if (record) {
            found->sequence = qMax(found->sequence, record->sequence);
            ++found->generation;
            if (record->deleted) m_drafts.remove(cardKey);
            else m_drafts.insert(cardKey);
            // The handler may write again (another card, or this one): nothing of found is used after it.
            Q_EMIT written(*record);
        }
        found = m_cards.find(cardKey);
        if (found == m_cards.end()) return;
        if (found->pending) proceed();
        else release(cardKey);
        return;
    }
    if (status == 404 && errorCode.isEmpty()) {
        // A server without review drafts (it was replaced by an older one).
        m_capable = Capable::No;
        m_capabilitiesAnswered.start();
        dropWaiting(QStringLiteral("no_capability"));
        release(cardKey);
        return;
    }
    DeveloperLog::record(QStringLiteral("review_drafts.write_failed"), {{QStringLiteral("card_id"), cardKey}, {QStringLiteral("remove"), sent.remove},
                                                                        {QStringLiteral("status"), status}, {QStringLiteral("error_code"), errorCode},
                                                                        {QStringLiteral("error"), reply->errorString()}});
    if (status == 401 || status == 403) refused();
    if (transient(status)) {
        // Sent again with the next change of the card, or the next answer of the changes request;
        // a newer change waiting already goes instead.
        if (!found->pending) {
            found->pending = sent;
            found->failed = true;
        }
        return;
    }
    // Refused for good (not a draft the server takes): the newest change, if any, still goes.
    if (found->pending) proceed();
    else release(cardKey);
}

void ReviewDraftSync::sendFetch(const QString &cardKey)
{
    if (m_fetching.value(cardKey)) return;
    const quint64 generation = m_cards[cardKey].generation;
    QNetworkReply *reply = network()->get(request(draftPath(cardKey), 15000));
    m_fetching.insert(cardKey, reply);
    connect(reply, &QNetworkReply::finished, this, [this, cardKey, reply, generation] {
        reply->deleteLater();
        if (m_stopped || m_fetching.value(cardKey).data() != reply) return;
        m_fetching.remove(cardKey);
        const int status = statusOf(reply);
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        const QString errorCode = json.value(QLatin1String("error_code")).toString();
        if (status == 200 && reply->error() == QNetworkReply::NoError) {
            const auto draft = Draft::fromJson(json);
            if (draft && !draft->deleted && key(draft->cardId) == cardKey) {
                Card &card = m_cards[cardKey];
                // Something newer of the card, or this very draft, was heard or written meanwhile.
                if (draft->sequence < card.sequence || (card.generation != generation && draft->sequence <= card.sequence)) return;
                card.sequence = draft->sequence;
                m_drafts.insert(cardKey);
                Q_EMIT fetched(cardKey, true, *draft);
                return;
            }
        } else if (status == 404 && errorCode == QLatin1String("review_draft_not_found")) {
            // A draft heard or written since the request went says more than this answer.
            if (m_cards.value(cardKey).generation != generation) return;
            m_drafts.remove(cardKey);
            Q_EMIT fetched(cardKey, false, Draft{});
            return;
        } else if (status == 404) {
            m_capable = Capable::No;
            m_capabilitiesAnswered.start();
            dropWaiting(QStringLiteral("no_capability"));
            return;
        }
        DeveloperLog::record(QStringLiteral("review_drafts.fetch_failed"),
                             {{QStringLiteral("card_id"), cardKey}, {QStringLiteral("status"), status}, {QStringLiteral("error"), reply->errorString()}});
        if (status == 401 || status == 403) refused();
        // Asked again with the next change or the next answer of the changes request.
        m_fetchWaiting.insert(cardKey);
    });
}

void ReviewDraftSync::scheduleChanges(int delayMs)
{
    if (m_stopped || !m_watching) return;
    m_changesTimer->start(qMax(0, delayMs));
}

void ReviewDraftSync::retryChangesLater()
{
    int delay = m_firstRetryMs;
    for (int step = 0; step < m_changesFailures && delay < m_longestRetryMs; ++step) delay *= 2;
    ++m_changesFailures;
    scheduleChanges(qMin(delay, m_longestRetryMs));
}

void ReviewDraftSync::retryChanges()
{
    if (m_stopped || !m_watching || m_changesReply) return;
    if (ready()) {
        sendChanges();
        return;
    }
    // Waiting for the token or the capability, which proceed() once they come; without a
    // server or the capability, asked again in a while.
    if (!m_linking && m_capable != Capable::Asking) scheduleChanges(CapabilitiesKeptMs);
}

void ReviewDraftSync::sendChanges()
{
    if (m_changesReply) return;
    QString path = QStringLiteral("/v1/review-drafts/changes");
    int timeoutMs = ChangesTransferMarginMs;
    // Without a cursor yet, the current drafts at once; then the changes after it, held.
    if (m_cursor >= 0) {
        path += QStringLiteral("?after=%1&wait=%2").arg(m_cursor).arg(ChangesWaitSeconds);
        timeoutMs += ChangesWaitSeconds * 1000;
    }
    QNetworkReply *reply = network()->get(request(path, timeoutMs));
    m_changesReply = reply;
    m_changesSent.start();
    connect(reply, &QNetworkReply::finished, this, [this, reply] { changesFinished(reply); });
}

void ReviewDraftSync::changesFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_stopped || reply != m_changesReply.data()) return;
    m_changesReply.clear();
    const int status = statusOf(reply);
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    if (status == 200 && reply->error() == QNetworkReply::NoError && json.value(QLatin1String("changes")).isArray()
        && json.value(QLatin1String("cursor")).isDouble()) {
        const qint64 asked = m_cursor;
        const qint64 cursor = json.value(QLatin1String("cursor")).toInteger();
        const QJsonArray changes = json.value(QLatin1String("changes")).toArray();
        const bool more = json.value(QLatin1String("more")).toBool();
        // A server restored from an older copy answers its current drafts and its own cursor.
        if (asked >= 0 && cursor < asked) DeveloperLog::record(QStringLiteral("review_drafts.cursor_back"), {{QStringLiteral("after"), asked}, {QStringLiteral("cursor"), cursor}});
        m_cursor = cursor;
        m_changesFailures = 0;
        // The next request first (one is open while watching), then what this one brought. A
        // server that answers at once with nothing, though asked to wait, is asked again a little
        // later, not at once over and over.
        const bool answeredWithoutWaiting = asked >= 0 && changes.isEmpty() && !more && m_changesSent.elapsed() < 1000;
        scheduleChanges(answeredWithoutWaiting ? 5000 : 0);
        for (const QJsonValue &value : changes) {
            if (m_stopped) return;
            if (const auto change = Draft::fromJson(value.toObject())) hear(*change);
        }
        // What failed is tried again now that the server answers.
        proceed();
        return;
    }
    DeveloperLog::record(QStringLiteral("review_drafts.changes_failed"),
                         {{QStringLiteral("status"), status}, {QStringLiteral("failures"), m_changesFailures}, {QStringLiteral("error"), reply->errorString()}}, true);
    if (status == 401 || status == 403) {
        refused();
        scheduleChanges(CapabilitiesKeptMs);
        return;
    }
    if (status == 404 && json.value(QLatin1String("error_code")).toString().isEmpty()) {
        m_capable = Capable::No;
        m_capabilitiesAnswered.start();
        dropWaiting(QStringLiteral("no_capability"));
        scheduleChanges(CapabilitiesKeptMs);
        return;
    }
    // A cursor the server does not take: its current drafts again.
    if (status == 400) m_cursor = -1;
    retryChangesLater();
}

void ReviewDraftSync::hear(const Draft &change)
{
    const QString cardKey = key(change.cardId);
    if (change.deleted) m_drafts.remove(cardKey);
    else m_drafts.insert(cardKey);
    Card &card = m_cards[cardKey];
    if (change.fromThisDevice) {
        // The echo of this computer's own write.
        card.sequence = qMax(card.sequence, change.sequence);
        return;
    }
    if (change.deleted && change.reason == answeredReason()) {
        // Answered on another device: final, whatever this computer writes of the card meanwhile.
        // What waits to be written is dropped (it would bring the draft of an answered card back).
        card.pending.reset();
        card.failed = false;
        card.editing = false;
        card.held.reset();
        card.sequence = qMax(card.sequence, change.sequence);
        ++card.generation;
        Q_EMIT changed(change);
        return;
    }
    // Older than what was heard or written here.
    if (change.sequence <= card.sequence) return;
    if (card.busy()) {
        // Comes once this computer's write is stored, if it was recorded after it.
        if (!card.held || card.held->sequence < change.sequence) card.held = change;
        return;
    }
    card.sequence = change.sequence;
    ++card.generation;
    Q_EMIT changed(change);
}

void ReviewDraftSync::release(const QString &cardKey)
{
    auto found = m_cards.find(cardKey);
    if (found == m_cards.end() || found->busy() || !found->held) return;
    const Draft held = *found->held;
    found->held.reset();
    // Written here after it: the last write wins.
    if (held.sequence <= found->sequence) return;
    found->sequence = held.sequence;
    ++found->generation;
    Q_EMIT changed(held);
}

}
