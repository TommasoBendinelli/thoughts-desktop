// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "serverlink.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Thoughts
{

// The pause between two of the app's checks whether the server holds work for this computer
// (Shell::startRemoteWorkWatch). checkDue() says when the next check is to run; the next pause
// starts right after it.
//
// With a server that has sync_state_wait_v1 the pause is one held request: GET
// /v2/sync/state?wait_ms=25000 with If-None-Match, which the server answers as soon as its state
// differs from the one this watch saw last (200), or when the wait is over (304). A change thus
// runs the check at once. The request goes with Qt networking to the server and with the token
// the agent notes use (ServerLink); the check itself stays the repository's.
//
// Without the capability, without a server, and for the pause after a held request that failed
// (or that the server answered at once without holding it), the pause is a plain 20 s.
//
// The first pause after start() is always plain: the app is starting. Nothing here keeps the
// Mac awake: a held request is an open connection that waits, like any other; asleep, it ends
// by its transfer timeout and the next pause starts over. stop() ends it (the app quits).
class SyncStateWatch : public QObject
{
    Q_OBJECT
public:
    explicit SyncStateWatch(QObject *parent = nullptr);
    ~SyncStateWatch() override;

    static constexpr int PlainPauseMs = 20 * 1000;
    static constexpr int HeldWaitMs = 25 * 1000;
    // A held request ends by itself this long after the wait it asked for (the server's answer
    // is late, or the connection is gone: the Mac slept).
    static constexpr int HeldTransferMarginMs = 10 * 1000;
    // A 304 sooner than this did not wait: the server does not hold the request.
    static constexpr int ShortestHeldMs = 5 * 1000;
    // Changes in a row (a Sync writing) run the check at most this often.
    static constexpr int ShortestCheckSpacingMs = 2 * 1000;
    // A server without the capability is asked again after this long (it may be updated), and
    // a token that is missing or was refused is read again after this long.
    static constexpr int CapabilitiesKeptMs = 10 * 60 * 1000;
    static QString capability();

    // The server (tests); otherwise THOUGHTS_SERVER_URL / THOUGHTS_TOKEN, else the saved server
    // address and the Keychain token, as the agent notes (ServerLink).
    void setServer(const QUrl &baseUrl, const QByteArray &token);
    // Shorter pauses (tests); 0 keeps the default.
    void setTimingForTesting(int plainPauseMs, int heldWaitMs, int shortestHeldMs, int checkSpacingMs, int heldMarginMs = 0);

    // Starts the pauses: the first check is due at the end of the first one, which is always a
    // plain one (the app starts meanwhile).
    void start();
    // Ends the pause: a held request is aborted, a timer stopped; no checkDue follows.
    void stop();
    bool isRunning() const { return m_running; }

    // Spacing: a change came sooner than ShortestCheckSpacingMs after the last check.
    enum class Pause { None, Plain, AskingCapabilities, Held, Spacing };
    Pause pause() const { return m_pause; }
    // The version of the server's state the last held request saw (its ETag).
    QByteArray stateVersion() const { return m_version; }
    // Whether the server said it has sync_state_wait_v1 (asked before the first held request).
    bool serverHolds() const { return m_capable == Capable::Yes; }

Q_SIGNALS:
    void checkDue();

private:
    enum class Capable { Unknown, Yes, No };

    // A server and a token are known (read now and then while not).
    bool linked();
    // The server refused the token.
    void refused();
    void beginPause();
    // A plain pause for what is left of PlainPauseMs since the pause began.
    void plainPause();
    void askCapabilities();
    void sendHeld();
    void heldFinished(QNetworkReply *reply);
    // The check is due (at most once per ShortestCheckSpacingMs).
    void due();
    void emitDue();
    QNetworkAccessManager *network();

    ServerLink m_link;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_timer = nullptr;
    QPointer<QNetworkReply> m_reply;
    bool m_running = false;
    Pause m_pause = Pause::None;
    Capable m_capable = Capable::Unknown;
    QElapsedTimer m_capabilitiesAnswered;
    QElapsedTimer m_unlinked;  // since no token was found, or the server refused it
    QElapsedTimer m_pauseBegan;
    QElapsedTimer m_lastDue;
    QElapsedTimer m_heldSent;
    QByteArray m_version;
    bool m_heldConditional = false;  // the held request carried If-None-Match
    int m_plainPauseMs = PlainPauseMs;
    int m_heldWaitMs = HeldWaitMs;
    int m_shortestHeldMs = ShortestHeldMs;
    int m_checkSpacingMs = ShortestCheckSpacingMs;
    int m_heldMarginMs = HeldTransferMarginMs;
};

}
