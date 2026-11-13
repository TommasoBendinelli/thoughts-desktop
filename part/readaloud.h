// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

#include "readaloudtext.h"

class QNetworkAccessManager;
class QNetworkReply;

// Read Aloud: the reader's selection spoken by a cloud voice that this Mac
// asks OpenRouter for directly (text-to-speech, MP3 per chunk) with the key
// the user saved in Settings > Read Aloud, falling back to the system voice
// without a key or when the cloud voice fails. The Thoughts server is not
// involved.
namespace Thoughts::ReadAloud
{

// What OpenRouter is asked for (the same choices the server's /v1/speech
// draft made): the free Fish Audio tier first, then its paid model when the
// free one refuses or is down.
inline constexpr char openRouterSpeechUrl[] = "https://openrouter.ai/api/v1/audio/speech";
inline constexpr char primaryModel[] = "fish-audio/s2.1-pro-free:free";
inline constexpr char fallbackModel[] = "fish-audio/s2.1-pro";
// Fish Audio's "Ethan".
inline constexpr char defaultVoice[] = "536d3a5e000945adb7038665781a4aca";
inline constexpr char defaultInstructions[] = "Read calmly and naturally, like an audiobook narrator reading an essay.";

// Only these model families honour "instructions" (OpenRouter's TTS guide).
bool modelAcceptsInstructions(const QString &model);
// The JSON body of one OpenRouter speech request.
QByteArray requestBody(const QString &model, const QString &text);
// Whether a failed attempt moves on to the fallback model: no answer at all
// (httpStatus 0), 402 (no credit), 429 (the free tier's limit) or a 5xx.
bool shouldTryFallbackModel(int httpStatus);

// The OpenRouter key: a generic password in the macOS Keychain (service
// "thoughts-openrouter", account "read-aloud"), never in a settings file, a
// log or a diagnostic. Once found it is kept in memory for the process.
class KeyStore
{
public:
    virtual ~KeyStore() = default;
    // Empty when no key is saved.
    virtual QByteArray read() = 0;
    virtual bool write(const QByteArray &key, QString *error = nullptr) = 0;
    // True also when no key was saved.
    virtual bool remove(QString *error = nullptr) = 0;
};

inline constexpr char keychainService[] = "thoughts-openrouter";
inline constexpr char keychainAccount[] = "read-aloud";

// The store Read Aloud and Settings use: the Keychain, unless replaced with
// setKeyStore() (tests) or THOUGHTS_READ_ALOUD_KEY_STORE=none (a store that is
// always empty, for tests that must not find the user's key).
std::shared_ptr<KeyStore> keyStore();
// Null restores the default.
void setKeyStore(std::shared_ptr<KeyStore> store);

// The pasted key without surrounding space or a "Bearer " prefix; empty, with
// *error, when it cannot be an OpenRouter key.
QByteArray normalizedKey(const QString &text, QString *error = nullptr);
// "sk-or-v1-…1a2b": enough to recognise which key is saved.
QString maskedKey(const QByteArray &key);

// One synthesis: POST the text to OpenRouter -> MP3, trying the fallback model
// when the primary one refuses (see shouldTryFallbackModel).
class SpeechClient : public QObject
{
    Q_OBJECT

public:
    struct Result {
        QByteArray audio;
        int httpStatus = 0;
        // Empty on success; otherwise a short reason code for the developer
        // log ("no_key", "http_401", "network_3", "timeout", "not_audio", ...).
        QString error;
        // The model that answered, or the last one tried.
        QString model;
        // OpenRouter's own error message, shortened and without the key; for
        // Settings' Test voice, not for the developer log.
        QString message;
    };

    explicit SpeechClient(QObject *parent = nullptr);
    ~SpeechClient() override;

    // For tests and THOUGHTS_OPENROUTER_SPEECH_URL (which, unlike the default,
    // may be plain http to a local test server).
    void setEndpoint(const QUrl &url);
    // A fixed key instead of keyStore()'s; an empty one means "no key".
    void setKey(const QByteArray &key);
    // Whether a key is known; *reason is "no_key" when not.
    bool resolveKey(QString *reason = nullptr);
    // A new reading: try the primary model first again.
    void reset();

    // Starts one synthesis; finished is called exactly once unless it is
    // aborted with cancel().
    void synthesize(const QString &text, std::function<void(const Result &)> finished);
    void cancel();

    static constexpr int maximumTextLength = 4000;
    static constexpr qint64 maximumAudioBytes = 32 * 1024 * 1024;

private:
    void attempt(const QString &text, const QStringList &models, int index, const QString &earlierFailures, std::function<void(const Result &)> finished);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QUrl m_url;
    QByteArray m_key;
    bool m_fixedKey = false;
    // The primary model refused during this reading: later chunks go straight
    // to the fallback model.
    bool m_primaryRefused = false;
};

// Plays MP3 chunks back to back.
class AudioOutput : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    // Queues one chunk; playback starts as soon as one is queued.
    virtual void enqueue(const QByteArray &mp3) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    // Stops and drops everything queued.
    virtual void stop() = 0;
    // Chunks queued or playing.
    virtual int pending() const = 0;

Q_SIGNALS:
    // The first sound of a chunk.
    void chunkStarted();
    // A chunk played to its end.
    void chunkFinished();
    // A chunk could not be played; the output is empty afterwards.
    void failed(const QString &reason);
};

// The system's own voice.
class SystemVoice : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    virtual bool available() const = 0;
    virtual void say(const QString &text) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;

Q_SIGNALS:
    void started();
    void finished();
};

// QMediaPlayer (QtMultimedia) when Thoughts is built with it; otherwise null.
std::unique_ptr<AudioOutput> createAudioOutput(QObject *parent = nullptr);
// QTextToSpeech ("darwin" on macOS, a good English voice) when Thoughts is
// built with it; otherwise null.
std::unique_ptr<SystemVoice> createSystemVoice(QObject *parent = nullptr);

class Controller : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Loading, Playing, Paused };
    Q_ENUM(State)

    // Null output or voice: created with createAudioOutput()/createSystemVoice().
    explicit Controller(QObject *parent = nullptr, std::unique_ptr<SpeechClient> client = {}, std::unique_ptr<AudioOutput> output = {},
                        std::unique_ptr<SystemVoice> voice = {});
    ~Controller() override;

    // Reads text aloud, replacing whatever was being read.
    void start(const QString &text);
    void stop();
    void pause();
    void resume();
    void togglePause();

    State state() const;
    bool active() const;
    // Whether the rest is read by the system voice.
    bool usingSystemVoice() const;
    QStringList chunks() const;

    void setChunkLimits(const ChunkLimits &limits);

Q_SIGNALS:
    void stateChanged(Thoughts::ReadAloud::Controller::State state);
    // The cloud voice failed and the system voice takes over; reason is a
    // short code (see SpeechClient::Result::error).
    void fallingBack(const QString &reason);
    // Neither voice can read.
    void unavailable(const QString &reason);
    // Developer-log events: metadata only, never text.
    void developerEvent(const QString &event, const QJsonObject &metadata);

private:
    void setState(State state);
    void fetchMore();
    void chunkFetched(int generation, int index, const SpeechClient::Result &result);
    void outputChunkStarted();
    void outputChunkFinished();
    void outputFailed(const QString &reason);
    void fallBack(const QString &reason, int fromChunk, int httpStatus = 0, const QString &model = {});
    void startSystemVoiceIfReady();
    void finish();
    void firstAudio(const QString &voice);

    std::unique_ptr<SpeechClient> m_client;
    std::unique_ptr<AudioOutput> m_output;
    std::unique_ptr<SystemVoice> m_voice;
    ChunkLimits m_limits;
    QStringList m_chunks;
    State m_state = State::Idle;
    int m_generation = 0;
    int m_nextFetch = 0;
    bool m_fetching = false;
    int m_played = 0;
    bool m_systemVoice = false;
    // Chunks the system voice reads once the cloud chunks already queued end.
    int m_systemFrom = -1;
    bool m_systemSpeaking = false;
    bool m_heardFirstAudio = false;
    QElapsedTimer m_elapsed;
};

}
