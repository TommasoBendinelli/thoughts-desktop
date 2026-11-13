// SPDX-License-Identifier: GPL-2.0-or-later
#include "readaloud.h"

#include "thoughtsplatform.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

#if THOUGHTS_HAVE_QTMULTIMEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#endif

#if THOUGHTS_HAVE_TEXTTOSPEECH
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace Thoughts::ReadAloud
{

// ------------------------------------------------------------ OpenRouter request

bool modelAcceptsInstructions(const QString &model)
{
    return model.startsWith(QLatin1String("fish-audio/")) || model.startsWith(QLatin1String("openai/gpt-4o-mini-tts"))
        || model.startsWith(QLatin1String("google/gemini"));
}

QByteArray requestBody(const QString &model, const QString &text)
{
    QJsonObject body{{QStringLiteral("model"), model},
                     {QStringLiteral("input"), text.left(SpeechClient::maximumTextLength)},
                     {QStringLiteral("response_format"), QStringLiteral("mp3")},
                     {QStringLiteral("voice"), QString::fromLatin1(defaultVoice)}};
    if (modelAcceptsInstructions(model)) {
        body.insert(QStringLiteral("instructions"), QString::fromLatin1(defaultInstructions));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool shouldTryFallbackModel(int httpStatus)
{
    return httpStatus == 0 || httpStatus == 402 || httpStatus == 429 || httpStatus >= 500;
}

// -------------------------------------------------------------------- KeyStore

namespace
{

class KeychainKeyStore final : public KeyStore
{
public:
    // A found key is kept in memory; without one, every reading asks the
    // Keychain again (a key stored by other means then works at once).
    QByteArray read() override
    {
        if (m_key.isEmpty()) {
            m_key = Thoughts::Platform::readSecret(QString::fromLatin1(keychainService), QString::fromLatin1(keychainAccount));
        }
        return m_key;
    }

    bool write(const QByteArray &key, QString *error) override
    {
        m_key.clear();
        if (!Thoughts::Platform::writeSecret(QString::fromLatin1(keychainService), QString::fromLatin1(keychainAccount), key, error)) {
            return false;
        }
        m_key = key;
        return true;
    }

    bool remove(QString *error) override
    {
        m_key.clear();
        return Thoughts::Platform::deleteSecret(QString::fromLatin1(keychainService), QString::fromLatin1(keychainAccount), error);
    }

private:
    QByteArray m_key;
};

class EmptyKeyStore final : public KeyStore
{
public:
    QByteArray read() override
    {
        return {};
    }
    bool write(const QByteArray &, QString *error) override
    {
        if (error) {
            *error = QStringLiteral("THOUGHTS_READ_ALOUD_KEY_STORE=none keeps no key.");
        }
        return false;
    }
    bool remove(QString *) override
    {
        return true;
    }
};

std::shared_ptr<KeyStore> &currentKeyStore()
{
    static std::shared_ptr<KeyStore> store;
    return store;
}

}

std::shared_ptr<KeyStore> keyStore()
{
    std::shared_ptr<KeyStore> &store = currentKeyStore();
    if (store) {
        return store;
    }
    // Checked on every call, so a test can switch it on after the Keychain store exists.
    if (qEnvironmentVariable("THOUGHTS_READ_ALOUD_KEY_STORE") == QLatin1String("none")) {
        static const std::shared_ptr<KeyStore> empty = std::make_shared<EmptyKeyStore>();
        return empty;
    }
    static const std::shared_ptr<KeyStore> keychain = std::make_shared<KeychainKeyStore>();
    return keychain;
}

void setKeyStore(std::shared_ptr<KeyStore> store)
{
    currentKeyStore() = std::move(store);
}

QByteArray normalizedKey(const QString &text, QString *error)
{
    QString key = text.trimmed();
    if (key.startsWith(QLatin1String("Bearer "), Qt::CaseInsensitive)) {
        key = key.mid(7).trimmed();
    }
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QByteArray();
    };
    if (key.isEmpty()) {
        return fail(QStringLiteral("Paste an OpenRouter key."));
    }
    // OpenRouter keys look like sk-or-v1-<64 hex digits>; accept any plain token.
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9._~+/=-]{20,300}$"));
    if (!token.match(key).hasMatch()) {
        return fail(QStringLiteral("That does not look like an OpenRouter key (it should start with sk-or-)."));
    }
    return key.toLatin1();
}

QString maskedKey(const QByteArray &key)
{
    if (key.isEmpty()) {
        return {};
    }
    const QString text = QString::fromLatin1(key);
    const QString prefix = text.startsWith(QLatin1String("sk-or-v1-")) ? QStringLiteral("sk-or-v1-") : text.left(std::min<qsizetype>(3, text.size() / 4));
    return prefix + QChar(0x2026) + text.right(std::min<qsizetype>(4, text.size() / 4));
}

// ---------------------------------------------------------------- SpeechClient

namespace
{

// OpenRouter's own error message, shortened; never the request, never the key.
QString upstreamMessage(const QByteArray &body, const QByteArray &key)
{
    const QJsonObject error = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject();
    QString message = error.value(QStringLiteral("message")).toString().simplified().left(200);
    if (!key.isEmpty()) {
        message.replace(QString::fromLatin1(key), QStringLiteral("…"));
    }
    return message;
}

}

SpeechClient::SpeechClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    const QString override = qEnvironmentVariable("THOUGHTS_OPENROUTER_SPEECH_URL").trimmed();
    m_url = QUrl(override.isEmpty() ? QString::fromLatin1(openRouterSpeechUrl) : override);
}

SpeechClient::~SpeechClient()
{
    cancel();
}

void SpeechClient::setEndpoint(const QUrl &url)
{
    m_url = url;
}

void SpeechClient::setKey(const QByteArray &key)
{
    m_key = key;
    m_fixedKey = true;
}

bool SpeechClient::resolveKey(QString *reason)
{
    if (!m_fixedKey) {
        m_key = keyStore()->read();
    }
    if (m_key.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("no_key");
        }
        return false;
    }
    return true;
}

void SpeechClient::reset()
{
    m_primaryRefused = false;
}

void SpeechClient::synthesize(const QString &text, std::function<void(const Result &)> finished)
{
    cancel();
    if (m_key.isEmpty() && !resolveKey()) {
        Result result;
        result.error = QStringLiteral("no_key");
        QTimer::singleShot(0, this, [finished = std::move(finished), result] { finished(result); });
        return;
    }
    QStringList models;
    if (!m_primaryRefused) {
        models.append(QString::fromLatin1(primaryModel));
    }
    models.append(QString::fromLatin1(fallbackModel));
    attempt(text, models, 0, QString(), std::move(finished));
}

void SpeechClient::attempt(const QString &text, const QStringList &models, int index, const QString &earlierFailures, std::function<void(const Result &)> finished)
{
    const QString model = models.at(index);
    QNetworkRequest request(m_url);
    request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + m_key);
    // The key is this service's: a redirect to another origin is not followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("X-Title"), QByteArrayLiteral("Thoughts"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // A long chunk can take a while to synthesize; this bounds a stalled answer.
    request.setTransferTimeout(60000);
    QNetworkReply *reply = m_network->post(request, requestBody(model, text));
    m_reply = reply;

    // Give up when OpenRouter cannot be reached at all.
    auto *connectTimer = new QTimer(reply);
    connectTimer->setSingleShot(true);
    connectTimer->setInterval(10000);
    connect(reply, &QNetworkReply::requestSent, connectTimer, &QTimer::stop);
    connect(connectTimer, &QTimer::timeout, reply, &QNetworkReply::abort);
    connectTimer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, text, models, index, model, earlierFailures, finished = std::move(finished)]() mutable {
        if (m_reply == reply) {
            m_reply = nullptr;
        }
        reply->deleteLater();
        Result result;
        result.model = model;
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (result.httpStatus == 0) {
            const QNetworkReply::NetworkError error = reply->error();
            result.error = error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError ? QStringLiteral("timeout")
                                                                                                                 : QStringLiteral("network_%1").arg(int(error));
        } else if (result.httpStatus != 200) {
            result.error = QStringLiteral("http_%1").arg(result.httpStatus);
            result.message = upstreamMessage(reply->read(64 * 1024), m_key);
        } else {
            const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
            result.audio = reply->readAll();
            if (type == QLatin1String("application/json") || type == QLatin1String("text/plain") || type == QLatin1String("text/html")) {
                result.error = QStringLiteral("not_audio");
                result.message = upstreamMessage(result.audio, m_key);
                result.audio.clear();
            } else if (result.audio.isEmpty()) {
                result.error = QStringLiteral("empty_audio");
            } else if (result.audio.size() > maximumAudioBytes) {
                result.error = QStringLiteral("audio_too_large");
                result.audio.clear();
            }
        }
        if (!result.error.isEmpty()) {
            const QString where = result.httpStatus ? QStringLiteral("HTTP %1").arg(result.httpStatus) : result.error;
            QString failure = QStringLiteral("%1: %2").arg(model, where);
            if (!result.message.isEmpty()) {
                failure += QStringLiteral(" (%1)").arg(result.message);
            }
            const QString failures = earlierFailures.isEmpty() ? failure : earlierFailures + QStringLiteral("; ") + failure;
            if (index + 1 < models.size() && shouldTryFallbackModel(result.httpStatus)) {
                if (model == QLatin1String(primaryModel)) {
                    m_primaryRefused = true;
                }
                attempt(text, models, index + 1, failures, std::move(finished));
                return;
            }
            result.message = failures;
        }
        finished(result);
    });
}

void SpeechClient::cancel()
{
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

// ------------------------------------------------------------- QtMultimedia output

#if THOUGHTS_HAVE_QTMULTIMEDIA
namespace
{

// Two players take turns: while one plays a chunk, the other has the next one
// loaded, so the next chunk starts without a decoding pause.
class MediaOutput final : public AudioOutput
{
public:
    explicit MediaOutput(QObject *parent)
        : AudioOutput(parent)
    {
        for (Slot &slot : m_slots) {
            slot.player = new QMediaPlayer(this);
            slot.audio = new QAudioOutput(this);
            slot.player->setAudioOutput(slot.audio);
            QMediaPlayer *player = slot.player;
            connect(player, &QMediaPlayer::mediaStatusChanged, this, [this, player](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia && player == current().player && !current().file.isEmpty()) {
                    advance();
                } else if (status == QMediaPlayer::InvalidMedia && (player == current().player || player == other().player)) {
                    fail(QStringLiteral("invalid_media"));
                }
            });
            connect(player, &QMediaPlayer::playbackStateChanged, this, [this, player](QMediaPlayer::PlaybackState state) {
                if (state == QMediaPlayer::PlayingState && player == current().player && !current().started) {
                    current().started = true;
                    Q_EMIT chunkStarted();
                }
            });
            connect(player, &QMediaPlayer::errorOccurred, this, [this, player](QMediaPlayer::Error error, const QString &) {
                if (error != QMediaPlayer::NoError && (player == current().player || player == other().player)) {
                    fail(QStringLiteral("player_%1").arg(int(error)));
                }
            });
        }
    }

    ~MediaOutput() override
    {
        stop();
    }

    void setVolume(float volume)
    {
        for (Slot &slot : m_slots) {
            slot.audio->setVolume(volume);
        }
    }

    void enqueue(const QByteArray &mp3) override
    {
        if (!m_directory) {
            m_directory = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("thoughts-read-aloud-XXXXXX")));
        }
        const QString path = m_directory->filePath(QStringLiteral("chunk-%1.mp3").arg(++m_counter));
        QFile file(path);
        if (!m_directory->isValid() || !file.open(QIODevice::WriteOnly) || file.write(mp3) != mp3.size()) {
            QTimer::singleShot(0, this, [this] { fail(QStringLiteral("temp_file")); });
            return;
        }
        file.close();
        m_queue.append(path);
        pump();
    }

    void pause() override
    {
        m_paused = true;
        if (!current().file.isEmpty()) {
            current().player->pause();
        }
    }

    void resume() override
    {
        m_paused = false;
        if (!current().file.isEmpty()) {
            current().player->play();
        }
    }

    void stop() override
    {
        for (Slot &slot : m_slots) {
            clear(slot);
        }
        for (const QString &path : std::as_const(m_queue)) {
            QFile::remove(path);
        }
        m_queue.clear();
    }

    int pending() const override
    {
        int count = int(m_queue.size());
        for (const Slot &slot : m_slots) {
            count += slot.file.isEmpty() ? 0 : 1;
        }
        return count;
    }

private:
    struct Slot {
        QMediaPlayer *player = nullptr;
        QAudioOutput *audio = nullptr;
        QString file;
        bool started = false;
    };

    Slot &current()
    {
        return m_slots[m_current];
    }
    Slot &other()
    {
        return m_slots[1 - m_current];
    }

    static void clear(Slot &slot)
    {
        slot.player->stop();
        slot.player->setSource(QUrl());
        if (!slot.file.isEmpty()) {
            QFile::remove(slot.file);
        }
        slot.file.clear();
        slot.started = false;
    }

    void load(Slot &slot)
    {
        slot.file = m_queue.takeFirst();
        slot.started = false;
        slot.player->setSource(QUrl::fromLocalFile(slot.file));
    }

    void pump()
    {
        if (current().file.isEmpty() && !m_queue.isEmpty()) {
            load(current());
            if (!m_paused) {
                current().player->play();
            }
        }
        if (!current().file.isEmpty() && other().file.isEmpty() && !m_queue.isEmpty()) {
            load(other());
        }
    }

    void advance()
    {
        const int finishedIndex = m_current;
        m_current = 1 - m_current;
        if (!current().file.isEmpty() && !m_paused) {
            current().player->play();
        }
        // The finished chunk no longer counts as pending; its player is reset
        // after this signal returns, unless pump() reuses it for the next chunk.
        Slot &done = m_slots[finishedIndex];
        const QString path = std::exchange(done.file, QString());
        done.started = false;
        QMediaPlayer *player = done.player;
        QTimer::singleShot(0, this, [this, player, path, finishedIndex] {
            if (m_slots[finishedIndex].file.isEmpty()) {
                player->stop();
                player->setSource(QUrl());
            }
            QFile::remove(path);
        });
        pump();
        Q_EMIT chunkFinished();
    }

    void fail(const QString &reason)
    {
        if (pending() == 0) {
            return;
        }
        stop();
        Q_EMIT failed(reason);
    }

    Slot m_slots[2];
    int m_current = 0;
    QStringList m_queue;
    bool m_paused = false;
    int m_counter = 0;
    std::unique_ptr<QTemporaryDir> m_directory;
};

}
#endif

std::unique_ptr<AudioOutput> createAudioOutput(QObject *parent)
{
#if THOUGHTS_HAVE_QTMULTIMEDIA
    auto output = std::make_unique<MediaOutput>(parent);
    bool ok = false;
    // For tests that must not make a sound.
    const float volume = qEnvironmentVariable("THOUGHTS_READ_ALOUD_VOLUME").toFloat(&ok);
    if (ok) {
        output->setVolume(volume);
    }
    return output;
#else
    Q_UNUSED(parent)
    return {};
#endif
}

// ------------------------------------------------------------- QTextToSpeech voice

#if THOUGHTS_HAVE_TEXTTOSPEECH
namespace
{

class SpeechVoice final : public SystemVoice
{
public:
    explicit SpeechVoice(QObject *parent)
        : SystemVoice(parent)
    {
    }

    bool available() const override
    {
        ensure();
        return m_speech && m_speech->state() != QTextToSpeech::Error;
    }

    void say(const QString &text) override
    {
        ensure();
        m_speaking = true;
        m_started = false;
        if (m_speech->state() == QTextToSpeech::Ready || m_speech->state() == QTextToSpeech::Speaking || m_speech->state() == QTextToSpeech::Paused) {
            chooseVoice();
            m_speech->say(text);
        } else {
            m_waiting = text;
        }
    }

    void pause() override
    {
        if (m_speech) {
            m_speech->pause();
        }
    }

    void resume() override
    {
        if (m_speech) {
            m_speech->resume();
        }
    }

    void stop() override
    {
        m_speaking = false;
        m_waiting.clear();
        if (m_speech) {
            m_speech->stop();
        }
    }

private:
    void ensure() const
    {
        if (m_speech) {
            return;
        }
        QString engine = qEnvironmentVariable("THOUGHTS_READ_ALOUD_ENGINE");
#ifdef Q_OS_MACOS
        if (engine.isEmpty() && QTextToSpeech::availableEngines().contains(QLatin1String("darwin"))) {
            engine = QStringLiteral("darwin");
        }
#endif
        auto *self = const_cast<SpeechVoice *>(this);
        self->m_speech = engine.isEmpty() ? new QTextToSpeech(self) : new QTextToSpeech(engine, self);
        connect(m_speech, &QTextToSpeech::stateChanged, self, [self](QTextToSpeech::State state) { self->stateChanged(state); });
    }

    void stateChanged(QTextToSpeech::State state)
    {
        if (state == QTextToSpeech::Ready && !m_waiting.isEmpty()) {
            chooseVoice();
            m_speech->say(std::exchange(m_waiting, QString()));
            return;
        }
        if (state == QTextToSpeech::Speaking && m_speaking && !m_started) {
            m_started = true;
            Q_EMIT started();
        } else if ((state == QTextToSpeech::Ready || state == QTextToSpeech::Error) && m_speaking && m_waiting.isEmpty()) {
            m_speaking = false;
            Q_EMIT finished();
        }
    }

    // A good English voice: Premium, then Enhanced, then a few known natural
    // ones, then whatever the system offers for English.
    void chooseVoice()
    {
        if (m_voiceChosen) {
            return;
        }
        m_voiceChosen = true;
        const QList<QVoice> voices = m_speech->findVoices(QLocale::English);
        if (voices.isEmpty()) {
            return;
        }
        const auto score = [](const QVoice &voice) {
            const QString name = voice.name();
            int value = 0;
            if (name.contains(QLatin1String("Premium"), Qt::CaseInsensitive)) {
                value += 100;
            } else if (name.contains(QLatin1String("Enhanced"), Qt::CaseInsensitive)) {
                value += 50;
            }
            static const QStringList preferred = {QStringLiteral("Ava"), QStringLiteral("Zoe"), QStringLiteral("Evan"), QStringLiteral("Samantha"), QStringLiteral("Daniel"), QStringLiteral("Alex")};
            for (qsizetype i = 0; i < preferred.size(); ++i) {
                if (name.startsWith(preferred.at(i))) {
                    value += 20 - int(i);
                    break;
                }
            }
            if (voice.locale().territory() == QLocale::UnitedStates || voice.locale().territory() == QLocale::UnitedKingdom) {
                value += 5;
            }
            return value;
        };
        const QVoice *best = &voices.constFirst();
        for (const QVoice &voice : voices) {
            if (score(voice) > score(*best)) {
                best = &voice;
            }
        }
        m_speech->setLocale(best->locale());
        m_speech->setVoice(*best);
    }

    QTextToSpeech *m_speech = nullptr;
    QString m_waiting;
    bool m_speaking = false;
    bool m_started = false;
    bool m_voiceChosen = false;
};

}
#endif

std::unique_ptr<SystemVoice> createSystemVoice(QObject *parent)
{
#if THOUGHTS_HAVE_TEXTTOSPEECH
    return std::make_unique<SpeechVoice>(parent);
#else
    Q_UNUSED(parent)
    return {};
#endif
}

// ------------------------------------------------------------------- Controller

Controller::Controller(QObject *parent, std::unique_ptr<SpeechClient> client, std::unique_ptr<AudioOutput> output, std::unique_ptr<SystemVoice> voice)
    : QObject(parent)
    , m_client(client ? std::move(client) : std::make_unique<SpeechClient>())
    , m_output(output ? std::move(output) : createAudioOutput())
    , m_voice(voice ? std::move(voice) : createSystemVoice())
{
    if (m_output) {
        connect(m_output.get(), &AudioOutput::chunkStarted, this, &Controller::outputChunkStarted);
        connect(m_output.get(), &AudioOutput::chunkFinished, this, &Controller::outputChunkFinished);
        connect(m_output.get(), &AudioOutput::failed, this, &Controller::outputFailed);
    }
    if (m_voice) {
        connect(m_voice.get(), &SystemVoice::started, this, [this] {
            if (m_systemSpeaking) {
                firstAudio(QStringLiteral("system"));
                if (m_state == State::Loading) {
                    setState(State::Playing);
                }
            }
        });
        connect(m_voice.get(), &SystemVoice::finished, this, [this] {
            if (m_systemSpeaking) {
                m_systemSpeaking = false;
                finish();
            }
        });
    }
}

Controller::~Controller()
{
    m_generation++;
    m_client->cancel();
    if (m_output) {
        m_output->stop();
    }
    if (m_voice) {
        m_systemSpeaking = false;
        m_voice->stop();
    }
}

void Controller::setChunkLimits(const ChunkLimits &limits)
{
    m_limits = limits;
}

Controller::State Controller::state() const
{
    return m_state;
}

bool Controller::active() const
{
    return m_state != State::Idle;
}

bool Controller::usingSystemVoice() const
{
    return m_systemVoice;
}

QStringList Controller::chunks() const
{
    return m_chunks;
}

void Controller::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged(state);
    }
}

void Controller::start(const QString &text)
{
    stop();
    const QString prepared = prepareText(text);
    m_chunks = chunkText(prepared, m_limits);
    if (m_chunks.isEmpty()) {
        return;
    }
    m_generation++;
    m_client->reset();
    m_nextFetch = 0;
    m_played = 0;
    m_fetching = false;
    m_systemVoice = false;
    m_systemFrom = -1;
    m_systemSpeaking = false;
    m_heardFirstAudio = false;
    m_elapsed.start();
    setState(State::Loading);
    Q_EMIT developerEvent(QStringLiteral("read_aloud.started"), QJsonObject{{QStringLiteral("chars"), int(prepared.size())}, {QStringLiteral("chunks"), int(m_chunks.size())}});

    QString reason;
    if (!m_output) {
        fallBack(QStringLiteral("no_audio_output"), 0);
    } else if (!m_client->resolveKey(&reason)) {
        fallBack(reason, 0);
    } else {
        fetchMore();
    }
}

void Controller::stop()
{
    const bool wasActive = m_state != State::Idle;
    m_generation++;
    m_client->cancel();
    m_fetching = false;
    if (m_output) {
        m_output->stop();
    }
    if (m_voice && (m_systemSpeaking || m_systemVoice)) {
        m_systemSpeaking = false;
        m_voice->stop();
    }
    m_systemSpeaking = false;
    m_systemFrom = -1;
    if (wasActive) {
        Q_EMIT developerEvent(QStringLiteral("read_aloud.stopped"),
                              QJsonObject{{QStringLiteral("elapsed_ms"), qint64(m_elapsed.elapsed())}, {QStringLiteral("chunks_played"), m_played}, {QStringLiteral("chunks"), int(m_chunks.size())}});
    }
    setState(State::Idle);
}

void Controller::pause()
{
    if (m_state != State::Loading && m_state != State::Playing) {
        return;
    }
    if (m_output) {
        m_output->pause();
    }
    if (m_voice && m_systemSpeaking) {
        m_voice->pause();
    }
    setState(State::Paused);
}

void Controller::resume()
{
    if (m_state != State::Paused) {
        return;
    }
    setState(m_heardFirstAudio ? State::Playing : State::Loading);
    if (m_output) {
        m_output->resume();
    }
    if (m_voice && m_systemSpeaking) {
        m_voice->resume();
    }
    startSystemVoiceIfReady();
}

void Controller::togglePause()
{
    if (m_state == State::Paused) {
        resume();
    } else {
        pause();
    }
}

void Controller::fetchMore()
{
    if (m_systemVoice || m_fetching || m_nextFetch >= m_chunks.size() || !m_output) {
        return;
    }
    // The chunk playing plus one ready behind it.
    if (m_output->pending() >= 2) {
        return;
    }
    m_fetching = true;
    const int index = m_nextFetch++;
    const int generation = m_generation;
    m_client->synthesize(m_chunks.at(index), [this, generation, index](const SpeechClient::Result &result) { chunkFetched(generation, index, result); });
}

void Controller::chunkFetched(int generation, int index, const SpeechClient::Result &result)
{
    if (generation != m_generation) {
        return;
    }
    m_fetching = false;
    if (!result.error.isEmpty()) {
        fallBack(result.error, index, result.httpStatus, result.model);
        return;
    }
    if (index == 0) {
        Q_EMIT developerEvent(QStringLiteral("read_aloud.first_chunk"),
                              QJsonObject{{QStringLiteral("fetch_ms"), qint64(m_elapsed.elapsed())},
                                          {QStringLiteral("bytes"), int(result.audio.size())},
                                          {QStringLiteral("model"), result.model}});
    }
    m_output->enqueue(result.audio);
    fetchMore();
}

void Controller::outputChunkStarted()
{
    if (m_systemVoice && m_systemSpeaking) {
        return;
    }
    firstAudio(QStringLiteral("cloud"));
    if (m_state == State::Loading) {
        setState(State::Playing);
    }
}

void Controller::outputChunkFinished()
{
    ++m_played;
    if (m_output->pending() > 0) {
        fetchMore();
        return;
    }
    if (m_systemFrom >= 0) {
        startSystemVoiceIfReady();
    } else if (m_systemVoice || m_played >= m_chunks.size()) {
        // Everything was read, or the rest cannot be.
        if (!m_systemSpeaking) {
            finish();
        }
    } else {
        // The next chunk is late: show loading until it arrives.
        if (m_state == State::Playing) {
            setState(State::Loading);
        }
        fetchMore();
    }
}

void Controller::outputFailed(const QString &reason)
{
    // The chunk that was playing is lost with everything queued behind it.
    m_client->cancel();
    m_fetching = false;
    fallBack(QStringLiteral("playback_") + reason, m_played);
}

void Controller::fallBack(const QString &reason, int fromChunk, int httpStatus, const QString &model)
{
    if (m_systemVoice) {
        return;
    }
    m_systemVoice = true;
    m_client->cancel();
    m_fetching = false;
    const bool voiceAvailable = m_voice && m_voice->available();
    QJsonObject metadata{{QStringLiteral("reason"), reason},
                         {QStringLiteral("chunk"), fromChunk},
                         {QStringLiteral("system_voice"), voiceAvailable},
                         {QStringLiteral("elapsed_ms"), qint64(m_elapsed.elapsed())}};
    if (httpStatus) {
        metadata.insert(QStringLiteral("http_status"), httpStatus);
    }
    if (!model.isEmpty()) {
        metadata.insert(QStringLiteral("model"), model);
    }
    Q_EMIT developerEvent(QStringLiteral("read_aloud.fallback"), metadata);
    if (!voiceAvailable) {
        Q_EMIT unavailable(reason);
        if (!m_output || m_output->pending() == 0) {
            finish();
        }
        return;
    }
    Q_EMIT fallingBack(reason);
    m_systemFrom = fromChunk;
    if (!m_output || m_output->pending() == 0) {
        startSystemVoiceIfReady();
    }
}

void Controller::startSystemVoiceIfReady()
{
    if (m_systemFrom < 0 || m_state == State::Paused || (m_output && m_output->pending() > 0)) {
        return;
    }
    const QString rest = m_chunks.mid(m_systemFrom).join(QLatin1Char(' '));
    m_systemFrom = -1;
    if (rest.isEmpty()) {
        finish();
        return;
    }
    m_systemSpeaking = true;
    m_voice->say(rest);
}

void Controller::firstAudio(const QString &voice)
{
    if (m_heardFirstAudio) {
        return;
    }
    m_heardFirstAudio = true;
    Q_EMIT developerEvent(QStringLiteral("read_aloud.first_audio"), QJsonObject{{QStringLiteral("latency_ms"), qint64(m_elapsed.elapsed())}, {QStringLiteral("voice"), voice}});
}

void Controller::finish()
{
    if (m_state == State::Idle) {
        return;
    }
    m_generation++;
    Q_EMIT developerEvent(QStringLiteral("read_aloud.finished"),
                          QJsonObject{{QStringLiteral("elapsed_ms"), qint64(m_elapsed.elapsed())},
                                      {QStringLiteral("voice"), m_systemVoice ? QStringLiteral("system") : QStringLiteral("cloud")},
                                      {QStringLiteral("chunks"), int(m_chunks.size())}});
    m_systemSpeaking = false;
    m_systemFrom = -1;
    setState(State::Idle);
}

}
