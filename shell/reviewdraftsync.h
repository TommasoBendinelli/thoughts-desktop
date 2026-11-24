// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "serverlink.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Thoughts
{

// The before step of a review card being answered, live between this computer and the user's
// other devices through the Thoughts server (protocol/review-drafts.md, capability
// review_drafts_v1): what the review writes (PUT after each pick, DELETE once answered), reads
// (GET when a card is shown) and hears (one held GET /v1/review-drafts/changes while a review is
// open). The review decides what a draft means for the card shown (ReviewOverview); this class
// only talks to the server, with the device token the agent notes use (ServerLink).
//
// Nothing here waits on the GUI thread: the token is read off it, every request runs in the
// background, and a failure is logged and tried again with the next change of the card or the
// next answer of the changes request. Without a server, without a token or without the
// capability nothing is sent, and the review works with the steps kept on this computer.
//
// One write per card at a time (§4.1): a change made while one is in flight is sent after it,
// with the newest content. Changes of a card from other devices wait while this computer writes
// that card or the user types an edit not yet written (setEditing): once written, the newest one
// that waited comes only if the server recorded it after this computer's write (the last write
// wins). A change older than what was heard or written of the card never comes.
class ReviewDraftSync : public QObject
{
    Q_OBJECT
public:
    // A draft of §1, or its tombstone (deleted).
    struct Draft {
        QString cardId; // Lowercase, as the server stores it.
        QStringList tags;
        QString note;
        QDateTime basedOnAnswerAt; // Invalid: the card had never been answered.
        bool deleted = false;
        QString reason; // A tombstone's: "answered" or "cleared".
        qint64 revision = 0;
        qint64 sequence = 0;
        QDateTime updatedAt;
        bool fromThisDevice = false;
        // A pick or a note.
        bool hasContent() const { return !tags.isEmpty() || !note.trimmed().isEmpty(); }
        // A record or a tombstone of §1; nothing without a card_id.
        static std::optional<Draft> fromJson(const QJsonObject &json);
    };

    explicit ReviewDraftSync(QObject *parent = nullptr);
    ~ReviewDraftSync() override;

    static QString capability();
    static QString answeredReason();
    static QString clearedReason();
    static constexpr int ChangesWaitSeconds = 25;
    // A held request ends by itself this long after the wait it asked for (the Mac slept).
    static constexpr int ChangesTransferMarginMs = 15 * 1000;
    // A server without the capability is asked again after this long (it may be updated), and a
    // token that is missing or was refused is read again after this long.
    static constexpr int CapabilitiesKeptMs = 10 * 60 * 1000;
    // The changes request after a failed one: 2 s, then twice as long each time, up to a minute.
    static constexpr int FirstRetryMs = 2 * 1000;
    static constexpr int LongestRetryMs = 60 * 1000;
    // The changes request ends this long after watching does, unless watching begins again (the
    // next card of the review shows) and it goes on.
    static constexpr int UnwatchDelayMs = 2 * 1000;

    // The server (tests); otherwise THOUGHTS_SERVER_URL / THOUGHTS_TOKEN, else the saved server
    // address and the Keychain token (ServerLink).
    void setServer(const QUrl &baseUrl, const QByteArray &token);
    // Shorter pauses after failures (tests); 0 keeps the default.
    void setRetryDelaysForTesting(int firstMs, int longestMs);

    // PUT the whole before step of @p cardId: its picks in pick order, its note, and the card's last
    // answer when the step began (invalid: never answered). No pick and a blank note delete it on
    // the server (reason "cleared").
    void write(const QString &cardId, const QStringList &tags, const QString &note, const QDateTime &basedOnAnswerAt);
    // DELETE the draft of @p cardId with @p reason (answeredReason() or clearedReason()).
    void remove(const QString &cardId, const QString &reason);
    // GET the draft of @p cardId: fetched() follows when the server tells (a draft, or none). A
    // failed request is asked again with the next answer of the changes request.
    void fetch(const QString &cardId);
    // The user is typing an edit of @p cardId's step that is not written yet: changes of the card
    // from other devices wait until the next write() of it is stored.
    void setEditing(const QString &cardId);
    // Whether a write of @p cardId is in flight or waits, or an edit of it is being typed.
    bool busy(const QString &cardId) const;
    // Whether the server holds a draft of @p cardId, as last heard here.
    bool hasDraft(const QString &cardId) const;
    // The changes request is open while watching (a card with a before step is shown). Watching
    // anew lists the current drafts first.
    void setWatching(bool watching);
    bool isWatching() const { return m_watching; }
    // Whether the server is known to have review drafts (tests).
    bool serverHasDrafts() const { return m_capable == Capable::Yes; }
    // Ends every request (the app quits); nothing follows.
    void stop();

Q_SIGNALS:
    // GET answered for @p cardId: its @p draft when @p found, else the server has none.
    void fetched(const QString &cardId, bool found, const Thoughts::ReviewDraftSync::Draft &draft);
    // A change of a draft made on another device (the changes request), to apply now.
    void changed(const Thoughts::ReviewDraftSync::Draft &change);
    // A write of this computer was stored: @p record is what the server holds now (a tombstone
    // after a DELETE or an empty PUT).
    void written(const Thoughts::ReviewDraftSync::Draft &record);

private:
    enum class Capable { Unknown, Asking, Yes, No };
    struct Write {
        bool remove = false;
        QString reason;
        QStringList tags;
        QString note;
        QDateTime basedOnAnswerAt;
        bool operator==(const Write &other) const
        {
            return remove == other.remove && reason == other.reason && tags == other.tags && note == other.note && basedOnAnswerAt == other.basedOnAnswerAt;
        }
    };
    struct Card {
        std::optional<Write> pending; // Waits: the newest content.
        std::optional<Write> sending; // In flight.
        QPointer<QNetworkReply> reply;
        bool failed = false; // pending is a write that failed: sent again with the next change or poll.
        bool editing = false;
        std::optional<Draft> held; // The newest change of another device that waited.
        qint64 sequence = 0; // The highest sequence of the card's draft heard or written here.
        quint64 generation = 0; // Changes applied and writes stored: a GET answered after one says nothing new.
        bool busy() const { return editing || reply || pending; }
    };

    static QString key(const QString &cardId) { return cardId.trimmed().toLower(); }
    QNetworkAccessManager *network();
    QNetworkRequest request(const QString &pathAndQuery, int timeoutMs) const;
    QString draftPath(const QString &cardId) const;
    // Sends what waits, once a server, a token and the capability are known (asked for as needed).
    void proceed();
    // Whether requests can go now; otherwise starts what makes them possible, which proceeds.
    bool ready();
    void link();
    void linked(const ServerLink &link, bool ok);
    void refused();
    void askCapabilities();
    // Nothing can be sent (no server, no token, no capability): what waits is dropped.
    void dropWaiting(const QString &why);
    void send(const QString &cardKey);
    void writeFinished(const QString &cardKey, QNetworkReply *reply);
    void sendFetch(const QString &cardKey);
    void sendChanges();
    void changesFinished(QNetworkReply *reply);
    void scheduleChanges(int delayMs);
    // After a failed changes request: 2 s, then twice as long each time, up to a minute.
    void retryChangesLater();
    void retryChanges();
    // A change heard from the changes request.
    void hear(const Draft &change);
    // What waited of @p card comes once its writes are stored.
    void release(const QString &cardKey);

    ServerLink m_link;
    bool m_explicitServer = false;
    bool m_linked = false;
    bool m_linking = false;
    quint64 m_linkGeneration = 0;
    QElapsedTimer m_unlinked; // Since no token was found, or the server refused it.
    Capable m_capable = Capable::Unknown;
    QElapsedTimer m_capabilitiesAnswered;
    QPointer<QNetworkReply> m_capabilitiesReply;
    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, Card> m_cards;
    QSet<QString> m_drafts; // The cards with a draft on the server, as last heard.
    QSet<QString> m_fetchWaiting; // GETs asked for before requests could go, or that failed.
    QHash<QString, QPointer<QNetworkReply>> m_fetching;
    bool m_watching = false;
    bool m_stopped = false;
    QPointer<QNetworkReply> m_changesReply;
    QTimer *m_changesTimer = nullptr;
    QTimer *m_unwatchTimer = nullptr;
    QElapsedTimer m_changesSent;
    qint64 m_cursor = -1; // None yet: the first request lists the current drafts.
    int m_changesFailures = 0;
    int m_firstRetryMs = FirstRetryMs;
    int m_longestRetryMs = LongestRetryMs;
};

}

Q_DECLARE_METATYPE(Thoughts::ReviewDraftSync::Draft)
