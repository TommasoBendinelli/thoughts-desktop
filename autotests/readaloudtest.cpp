// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/readaloud.h"
#include "../part/readaloudtext.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using namespace Thoughts::ReadAloud;

namespace
{

// A tiny HTTP/1.1 server: one request per connection.
class FakeServer : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
    };
    struct Response {
        int status = 200;
        QByteArray contentType = "audio/mpeg";
        QByteArray body;
        int delayMs = 0;
    };

    explicit FakeServer(std::function<Response(const Request &)> handler)
        : m_handler(std::move(handler))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }

    QList<Request> requests;
    int open = 0;
    int maxOpen = 0;

private:
    void read(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        Request request;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        request.method = first.value(0);
        request.path = first.value(1);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines.at(i).indexOf(':');
            if (colon > 0) {
                request.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
            }
        }
        const int length = request.headers.value("content-length").toInt();
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        request.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        requests.append(request);
        maxOpen = std::max(maxOpen, ++open);
        const Response response = m_handler(request);
        const auto send = [this, socket, response] {
            --open;
            QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\nContent-Type: " + response.contentType + "\r\nContent-Length: " + QByteArray::number(response.body.size())
                + "\r\nConnection: close\r\n\r\n" + response.body;
            socket->write(reply);
            socket->disconnectFromHost();
        };
        if (response.delayMs > 0) {
            QTimer::singleShot(response.delayMs, socket, send);
        } else {
            send();
        }
    }

    QTcpServer m_server;
    std::function<Response(const Request &)> m_handler;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

// Plays nothing: each chunk "starts" right away and ends when the test says so.
class FakeOutput : public AudioOutput
{
public:
    void enqueue(const QByteArray &mp3) override
    {
        enqueued.append(mp3);
        queue.append(mp3);
        if (queue.size() == 1 && !paused) {
            QTimer::singleShot(0, this, [this] {
                if (!queue.isEmpty()) {
                    Q_EMIT chunkStarted();
                }
            });
        }
    }
    void pause() override
    {
        paused = true;
    }
    void resume() override
    {
        paused = false;
    }
    void stop() override
    {
        queue.clear();
        ++stops;
    }
    int pending() const override
    {
        return int(queue.size());
    }
    // The chunk playing ends.
    void finishCurrent()
    {
        QVERIFY(!queue.isEmpty());
        queue.removeFirst();
        if (!queue.isEmpty()) {
            Q_EMIT chunkStarted();
        }
        Q_EMIT chunkFinished();
    }

    QList<QByteArray> enqueued;
    QList<QByteArray> queue;
    bool paused = false;
    int stops = 0;
};

class FakeVoice : public SystemVoice
{
public:
    bool available() const override
    {
        return isAvailable;
    }
    void say(const QString &text) override
    {
        said.append(text);
        QTimer::singleShot(0, this, [this] { Q_EMIT started(); });
    }
    void pause() override
    {
    }
    void resume() override
    {
    }
    void stop() override
    {
        ++stops;
    }
    void finishSpeaking()
    {
        Q_EMIT finished();
    }

    bool isAvailable = true;
    QStringList said;
    int stops = 0;
};

// The Keychain stand-in: never touches the user's Keychain.
class MemoryKeyStore : public KeyStore
{
public:
    QByteArray read() override
    {
        ++reads;
        return key;
    }
    bool write(const QByteArray &value, QString *error = nullptr) override
    {
        if (refuse) {
            if (error) {
                *error = QStringLiteral("refused");
            }
            return false;
        }
        key = value;
        return true;
    }
    bool remove(QString * = nullptr) override
    {
        key.clear();
        return true;
    }
    QByteArray key;
    bool refuse = false;
    int reads = 0;
};

QString inputOf(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object().value(QStringLiteral("input")).toString();
}

QString modelOf(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object().value(QStringLiteral("model")).toString();
}

QString longText()
{
    QStringList sentences;
    for (int i = 0; i < 30; ++i) {
        sentences.append(QStringLiteral("Sentence number %1 talks about gradient descent, attention and the softmax function in some detail.").arg(i));
    }
    return sentences.join(QLatin1Char(' '));
}

}

class ReadAloudTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void prepareJoinsLinesAndHyphens();
    void prepareCollapsesWhitespaceAndLigatures();
    void chunksStartShortAndGrow();
    void chunksNeverCutWords();
    void chunksKeepAbbreviationsAndNumbers();
    void chunksSplitOverlongSentences();
    void requestBodyIsOpenRoutersSpeechRequest();
    void fallbackModelOnlyForRefusalsAndOutages();
    void keyIsNormalizedAndMasked();
    void keyComesFromTheKeyStore();
    void emptyKeyStoreFromEnvironment();
    void clientPostsToOpenRouterWithKey();
    void clientFallsBackToPaidModel();
    void clientDoesNotFallBackOnRefusedKey();
    void clientReportsServerErrors();
    void clientReportsUnreachableServer();
    void controllerPlaysChunksInOrderWithOnePrefetched();
    void controllerWithoutKeyUsesSystemVoice();
    void controllerFallsBackOn503();
    void controllerFallsBackMidway();
    void controllerWithoutAnyVoice();
    void controllerStopCancels();
    void controllerEventsCarryNoText();
    void mediaOutputPlaysMp3();
    void controllerPlaysCloudAudioEndToEnd();
    void systemVoiceSpeaksWithMockEngine();

    void cleanup();

private:
    std::unique_ptr<SpeechClient> clientFor(const FakeServer &server, const QByteArray &key = "sk-or-v1-test-key")
    {
        auto client = std::make_unique<SpeechClient>();
        client->setEndpoint(QUrl(server.url().toString() + QStringLiteral("/api/v1/audio/speech")));
        client->setKey(key);
        return client;
    }
    SpeechClient::Result synthesizeNow(SpeechClient &client, const QString &text = QStringLiteral("Hi."))
    {
        SpeechClient::Result result;
        bool done = false;
        client.synthesize(text, [&](const SpeechClient::Result &value) {
            result = value;
            done = true;
        });
        if (!QTest::qWaitFor([&done] { return done; }, 15000)) {
            result.error = QStringLiteral("test_timeout");
        }
        return result;
    }
    QByteArray m_mp3;
};

void ReadAloudTest::initTestCase()
{
    QFile file(QStringLiteral(KDESRCDIR "data/readaloud-hello.mp3"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    m_mp3 = file.readAll();
    QVERIFY(m_mp3.size() > 1000);
}

void ReadAloudTest::cleanup()
{
    setKeyStore(nullptr);
    qunsetenv("THOUGHTS_READ_ALOUD_KEY_STORE");
}

void ReadAloudTest::prepareJoinsLinesAndHyphens()
{
    QCOMPARE(prepareText(QStringLiteral("The exam-\nple shows\nthat lines join.")), QStringLiteral("The example shows that lines join."));
    QCOMPARE(prepareText(QStringLiteral("a well-\n  known result")), QStringLiteral("a wellknown result"));
    // An upper-case or numeric continuation keeps the hyphen: a compound.
    QCOMPARE(prepareText(QStringLiteral("COVID-\n19 and Anti-\nInflammatory")), QStringLiteral("COVID-19 and Anti-Inflammatory"));
    // A dash that is not at a line end stays.
    QCOMPARE(prepareText(QStringLiteral("state-of-the-art - really")), QStringLiteral("state-of-the-art - really"));
    QCOMPARE(prepareText(QStringLiteral("soft­\nhyphen and in­line")), QStringLiteral("softhyphen and inline"));
    QCOMPARE(prepareText(QStringLiteral("Windows\r\nline\rbreaks")), QStringLiteral("Windows line breaks"));
}

void ReadAloudTest::prepareCollapsesWhitespaceAndLigatures()
{
    QCOMPARE(prepareText(QStringLiteral("  Many \t spaces\n\n\nand  paragraphs  ")), QStringLiteral("Many spaces and paragraphs"));
    QCOMPARE(prepareText(QStringLiteral("eﬃcient ﬁnal")), QStringLiteral("efficient final"));
    QCOMPARE(prepareText(QString()), QString());
    QCOMPARE(prepareText(QStringLiteral(" \n\t ")), QString());
}

void ReadAloudTest::chunksStartShortAndGrow()
{
    const QString text = prepareText(longText());
    const QStringList chunks = chunkText(text);
    QVERIFY(chunks.size() >= 3);
    QCOMPARE(chunks.join(QLatin1Char(' ')), text);
    // The first chunk is one sentence.
    QVERIFY(chunks.at(0).endsWith(QLatin1Char('.')));
    QCOMPARE(chunks.at(0).count(QStringLiteral("Sentence number")), 1);
    QVERIFY(chunks.at(0).size() <= 400);
    QVERIFY(chunks.at(1).size() <= 800);
    for (qsizetype i = 2; i < chunks.size(); ++i) {
        QVERIFY(chunks.at(i).size() <= 1200);
    }
    // Later chunks are filled up, not one sentence each.
    QVERIFY(chunks.at(2).size() > 1000);
    for (const QString &chunk : chunks) {
        QVERIFY2(chunk.endsWith(QLatin1Char('.')), qPrintable(chunk));
    }

    // A short first sentence takes the next one along.
    const QStringList shortStart =
        chunkText(QStringLiteral("Introduction. Neural networks learn useful representations of their input from many examples. They are trained by gradient descent."));
    QCOMPARE(shortStart.value(0), QStringLiteral("Introduction. Neural networks learn useful representations of their input from many examples."));
    QCOMPARE(chunkText(QStringLiteral("One sentence only")), QStringList{QStringLiteral("One sentence only")});
    QVERIFY(chunkText(QString()).isEmpty());
}

void ReadAloudTest::chunksNeverCutWords()
{
    // No punctuation at all: chunks break between words.
    QStringList words;
    for (int i = 0; i < 600; ++i) {
        words.append(QStringLiteral("word%1").arg(i));
    }
    const QString text = words.join(QLatin1Char(' '));
    const QStringList chunks = chunkText(text);
    QVERIFY(chunks.size() > 3);
    QCOMPARE(chunks.join(QLatin1Char(' ')), text);
    for (const QString &chunk : chunks) {
        for (const QString &word : chunk.split(QLatin1Char(' '))) {
            QVERIFY2(word.startsWith(QStringLiteral("word")) && word.size() > 4, qPrintable(word));
        }
    }
    QVERIFY(chunks.at(0).size() <= 400);
}

void ReadAloudTest::chunksKeepAbbreviationsAndNumbers()
{
    const ChunkLimits tiny{.first = 400, .second = 800, .later = 1200, .firstMinimum = 1};
    QCOMPARE(chunkText(QStringLiteral("See e.g. Fig. 3 and Smith et al. for details. Pi is 3.14 here. Next."), tiny),
             (QStringList{QStringLiteral("See e.g. Fig. 3 and Smith et al. for details."), QStringLiteral("Pi is 3.14 here. Next.")}));
    QCOMPARE(chunkText(QStringLiteral("Really? Yes! “Quoted.” Then more."), tiny).value(0), QStringLiteral("Really?"));
}

void ReadAloudTest::chunksSplitOverlongSentences()
{
    QStringList clauses;
    for (int i = 0; i < 40; ++i) {
        clauses.append(QStringLiteral("clause %1 continues the thought").arg(i));
    }
    const QString sentence = clauses.join(QStringLiteral(", ")) + QLatin1Char('.');
    const QStringList chunks = chunkText(sentence);
    QVERIFY(chunks.size() >= 2);
    QCOMPARE(chunks.join(QLatin1Char(' ')), sentence);
    QVERIFY(chunks.at(0).size() <= 400);
    // Split after a comma, not inside a clause.
    QVERIFY2(chunks.at(0).endsWith(QLatin1Char(',')), qPrintable(chunks.at(0)));

    // A single overlong token is the one thing that is cut.
    const QString url = QStringLiteral("https://example.com/") + QString(900, QLatin1Char('a'));
    const QStringList cut = chunkText(url);
    QVERIFY(cut.size() >= 2);
    QCOMPARE(cut.join(QString()), url);
}

void ReadAloudTest::requestBodyIsOpenRoutersSpeechRequest()
{
    QCOMPARE(QString::fromLatin1(openRouterSpeechUrl), QStringLiteral("https://openrouter.ai/api/v1/audio/speech"));
    QCOMPARE(QString::fromLatin1(primaryModel), QStringLiteral("fish-audio/s2.1-pro-free:free"));
    QCOMPARE(QString::fromLatin1(fallbackModel), QStringLiteral("fish-audio/s2.1-pro"));
    QCOMPARE(QString::fromLatin1(defaultVoice), QStringLiteral("536d3a5e000945adb7038665781a4aca"));

    const QJsonObject fish = QJsonDocument::fromJson(requestBody(QString::fromLatin1(primaryModel), QStringLiteral("Hello there."))).object();
    QCOMPARE(fish.value(QStringLiteral("model")).toString(), QString::fromLatin1(primaryModel));
    QCOMPARE(fish.value(QStringLiteral("input")).toString(), QStringLiteral("Hello there."));
    QCOMPARE(fish.value(QStringLiteral("response_format")).toString(), QStringLiteral("mp3"));
    QCOMPARE(fish.value(QStringLiteral("voice")).toString(), QString::fromLatin1(defaultVoice));
    QCOMPARE(fish.value(QStringLiteral("instructions")).toString(), QString::fromLatin1(defaultInstructions));
    QCOMPARE(fish.keys().size(), 5);

    // Models that ignore instructions do not get them.
    QVERIFY(modelAcceptsInstructions(QStringLiteral("fish-audio/s2.1-pro")));
    QVERIFY(modelAcceptsInstructions(QStringLiteral("openai/gpt-4o-mini-tts")));
    QVERIFY(modelAcceptsInstructions(QStringLiteral("google/gemini-2.5-flash-preview-tts")));
    QVERIFY(!modelAcceptsInstructions(QStringLiteral("openai/tts-1")));
    const QJsonObject other = QJsonDocument::fromJson(requestBody(QStringLiteral("openai/tts-1"), QStringLiteral("Hi."))).object();
    QVERIFY(!other.contains(QStringLiteral("instructions")));

    // Never more than OpenRouter's request limit.
    const QJsonObject longest = QJsonDocument::fromJson(requestBody(QString::fromLatin1(primaryModel), QString(5000, QLatin1Char('a')))).object();
    QCOMPARE(longest.value(QStringLiteral("input")).toString().size(), SpeechClient::maximumTextLength);
}

void ReadAloudTest::fallbackModelOnlyForRefusalsAndOutages()
{
    QVERIFY(shouldTryFallbackModel(0));
    QVERIFY(shouldTryFallbackModel(402));
    QVERIFY(shouldTryFallbackModel(429));
    QVERIFY(shouldTryFallbackModel(500));
    QVERIFY(shouldTryFallbackModel(503));
    QVERIFY(!shouldTryFallbackModel(400));
    QVERIFY(!shouldTryFallbackModel(401));
    QVERIFY(!shouldTryFallbackModel(403));
    QVERIFY(!shouldTryFallbackModel(404));
}

void ReadAloudTest::keyIsNormalizedAndMasked()
{
    // Built here, so that no key-shaped text sits in the source: 64 hex digits after the prefix.
    const QByteArray key = QByteArray("sk-or-v1-") + QByteArray("0123456789abcdef").repeated(4);
    QCOMPARE(normalizedKey(QStringLiteral("  ") + QString::fromLatin1(key) + QStringLiteral("\n")), key);
    QCOMPARE(normalizedKey(QStringLiteral("Bearer ") + QString::fromLatin1(key)), key);
    QString error;
    QVERIFY(normalizedKey(QString(), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(normalizedKey(QStringLiteral("sk-or-v1 with spaces in the middle"), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(normalizedKey(QStringLiteral("short")).isEmpty());
    QVERIFY(normalizedKey(QStringLiteral("sk-or-v1-\"quoted\"-0123456789")).isEmpty());

    const QString masked = maskedKey(key);
    QCOMPARE(masked, QStringLiteral("sk-or-v1-…cdef"));
    QVERIFY(!masked.contains(QStringLiteral("0123456789")));
    QCOMPARE(maskedKey(QByteArray()), QString());
}

void ReadAloudTest::keyComesFromTheKeyStore()
{
    auto store = std::make_shared<MemoryKeyStore>();
    setKeyStore(store);
    QCOMPARE(keyStore(), std::static_pointer_cast<KeyStore>(store));

    FakeServer server([this](const FakeServer::Request &) { return FakeServer::Response{200, "audio/mpeg", m_mp3}; });
    SpeechClient client;
    client.setEndpoint(QUrl(server.url().toString() + QStringLiteral("/api/v1/audio/speech")));
    QString reason;
    QVERIFY(!client.resolveKey(&reason));
    QCOMPARE(reason, QStringLiteral("no_key"));
    // Without a key nothing is sent.
    QCOMPARE(synthesizeNow(client).error, QStringLiteral("no_key"));
    QCOMPARE(server.requests.size(), 0);

    QVERIFY(store->write("sk-or-v1-saved-key-0123456789"));
    QVERIFY(client.resolveKey());
    QCOMPARE(synthesizeNow(client).error, QString());
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.constLast().headers.value("authorization"), QByteArray("Bearer sk-or-v1-saved-key-0123456789"));

    // A removed key is gone at the next reading.
    QVERIFY(store->remove());
    QVERIFY(!client.resolveKey(&reason));
    QCOMPARE(reason, QStringLiteral("no_key"));
}

void ReadAloudTest::emptyKeyStoreFromEnvironment()
{
    qputenv("THOUGHTS_READ_ALOUD_KEY_STORE", "none");
    QVERIFY(keyStore()->read().isEmpty());
    QString error;
    QVERIFY(!keyStore()->write("sk-or-v1-never-stored-0123", &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(keyStore()->read().isEmpty());
    QVERIFY(keyStore()->remove());
}

void ReadAloudTest::clientPostsToOpenRouterWithKey()
{
    FakeServer server([this](const FakeServer::Request &) { return FakeServer::Response{200, "audio/mpeg", m_mp3}; });
    auto client = clientFor(server);
    QVERIFY(client->resolveKey());
    const SpeechClient::Result result = synthesizeNow(*client, QStringLiteral("Hello there."));
    QCOMPARE(result.error, QString());
    QCOMPARE(result.httpStatus, 200);
    QCOMPARE(result.audio, m_mp3);
    QCOMPARE(result.model, QString::fromLatin1(primaryModel));
    QCOMPARE(server.requests.size(), 1);
    const FakeServer::Request request = server.requests.constFirst();
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.path, QByteArray("/api/v1/audio/speech"));
    QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer sk-or-v1-test-key"));
    QCOMPARE(request.headers.value("x-title"), QByteArray("Thoughts"));
    QVERIFY(request.headers.value("content-type").startsWith("application/json"));
    QCOMPARE(request.body, requestBody(QString::fromLatin1(primaryModel), QStringLiteral("Hello there.")));

    // OpenRouter may label MP3 as a generic binary: still audio.
    FakeServer generic([this](const FakeServer::Request &) { return FakeServer::Response{200, "application/octet-stream", m_mp3}; });
    QCOMPARE(synthesizeNow(*clientFor(generic)).audio, m_mp3);
}

void ReadAloudTest::clientFallsBackToPaidModel()
{
    FakeServer server([this](const FakeServer::Request &request) {
        if (modelOf(request.body) == QLatin1String(primaryModel)) {
            return FakeServer::Response{429, "application/json", "{\"error\":{\"code\":429,\"message\":\"free tier rate limited\"}}"};
        }
        return FakeServer::Response{200, "audio/mpeg", m_mp3};
    });
    auto client = clientFor(server);
    SpeechClient::Result result = synthesizeNow(*client);
    QCOMPARE(result.error, QString());
    QCOMPARE(result.model, QString::fromLatin1(fallbackModel));
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(modelOf(server.requests.at(0).body), QString::fromLatin1(primaryModel));
    QCOMPARE(modelOf(server.requests.at(1).body), QString::fromLatin1(fallbackModel));
    QCOMPARE(inputOf(server.requests.at(1).body), QStringLiteral("Hi."));

    // The rest of the reading goes straight to the paid model.
    QCOMPARE(synthesizeNow(*client).model, QString::fromLatin1(fallbackModel));
    QCOMPARE(server.requests.size(), 3);
    // A new reading tries the free model again.
    client->reset();
    synthesizeNow(*client);
    QCOMPARE(server.requests.size(), 5);
    QCOMPARE(modelOf(server.requests.at(3).body), QString::fromLatin1(primaryModel));
}

void ReadAloudTest::clientDoesNotFallBackOnRefusedKey()
{
    FakeServer server([](const FakeServer::Request &) {
        // An error message that (unusually) repeats the key.
        return FakeServer::Response{401, "application/json", "{\"error\":{\"code\":401,\"message\":\"User not found for sk-or-v1-test-key.\"}}"};
    });
    auto client = clientFor(server);
    const SpeechClient::Result result = synthesizeNow(*client);
    QCOMPARE(result.error, QStringLiteral("http_401"));
    QCOMPARE(result.httpStatus, 401);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY2(result.message.contains(QStringLiteral("User not found")), qPrintable(result.message));
    QVERIFY2(!result.message.contains(QStringLiteral("test-key")), qPrintable(result.message));
}

void ReadAloudTest::clientReportsServerErrors()
{
    int status = 503;
    QByteArray type = "application/json";
    FakeServer server([&](const FakeServer::Request &) { return FakeServer::Response{status, type, "{\"error\":{\"message\":\"provider down\"}}"}; });
    auto client = clientFor(server);
    SpeechClient::Result result = synthesizeNow(*client);
    // Both models were tried.
    QCOMPARE(result.error, QStringLiteral("http_503"));
    QCOMPARE(result.httpStatus, 503);
    QCOMPARE(result.model, QString::fromLatin1(fallbackModel));
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(result.audio.isEmpty());
    QVERIFY2(result.message.contains(QString::fromLatin1(primaryModel)) && result.message.contains(QStringLiteral("provider down")), qPrintable(result.message));
    client->reset();
    status = 200;
    result = synthesizeNow(*client);
    QCOMPARE(result.error, QStringLiteral("not_audio"));
    QCOMPARE(server.requests.size(), 3);
    status = 400;
    type = "audio/mpeg";
    QCOMPARE(synthesizeNow(*client).error, QStringLiteral("http_400"));
}

void ReadAloudTest::clientReportsUnreachableServer()
{
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();
    SpeechClient client;
    client.setEndpoint(QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/audio/speech").arg(port)));
    client.setKey("sk-or-v1-test-key");
    const SpeechClient::Result result = synthesizeNow(client);
    QVERIFY2(result.error.startsWith(QLatin1String("network_")), qPrintable(result.error));
    QCOMPARE(result.httpStatus, 0);
    QCOMPARE(result.model, QString::fromLatin1(fallbackModel));
}

void ReadAloudTest::controllerPlaysChunksInOrderWithOnePrefetched()
{
    FakeServer server([](const FakeServer::Request &request) {
        // The chunk's own text as "audio", to check the order.
        return FakeServer::Response{200, "audio/mpeg", inputOf(request.body).toUtf8(), 20};
    });
    auto *output = new FakeOutput;
    auto *voice = new FakeVoice;
    Controller controller(nullptr, clientFor(server), std::unique_ptr<AudioOutput>(output), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy events(&controller, &Controller::developerEvent);
    QSignalSpy fallback(&controller, &Controller::fallingBack);

    controller.start(longText());
    const QStringList chunks = controller.chunks();
    QVERIFY(chunks.size() >= 3);
    QCOMPARE(controller.state(), Controller::State::Loading);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Playing, 5000);
    // The playing chunk and one behind it; no more requests until one ends.
    QTRY_COMPARE_WITH_TIMEOUT(output->enqueued.size(), 2, 5000);
    QTest::qWait(150);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(server.maxOpen, 1);

    for (int played = 0; played < chunks.size(); ++played) {
        QTRY_VERIFY_WITH_TIMEOUT(output->pending() >= 1, 5000);
        output->finishCurrent();
    }
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Idle, 5000);
    QCOMPARE(server.requests.size(), chunks.size());
    QCOMPARE(server.maxOpen, 1);
    QCOMPARE(output->enqueued.size(), chunks.size());
    for (qsizetype i = 0; i < chunks.size(); ++i) {
        QCOMPARE(QString::fromUtf8(output->enqueued.at(i)), chunks.at(i));
    }
    QVERIFY(fallback.isEmpty());
    QVERIFY(voice->said.isEmpty());

    QStringList names;
    for (const QList<QVariant> &event : std::as_const(events)) {
        names.append(event.at(0).toString());
    }
    QCOMPARE(names.value(0), QStringLiteral("read_aloud.started"));
    QVERIFY(names.contains(QStringLiteral("read_aloud.first_audio")));
    QCOMPARE(names.constLast(), QStringLiteral("read_aloud.finished"));
    const QJsonObject started = events.at(0).at(1).toJsonObject();
    QCOMPARE(started.value(QStringLiteral("chunks")).toInt(), int(chunks.size()));
    QCOMPARE(started.value(QStringLiteral("chars")).toInt(), int(prepareText(longText()).size()));
    const QJsonObject first = events.at(names.indexOf(QStringLiteral("read_aloud.first_audio"))).at(1).toJsonObject();
    QCOMPARE(first.value(QStringLiteral("voice")).toString(), QStringLiteral("cloud"));
    QVERIFY(first.contains(QStringLiteral("latency_ms")));
}

void ReadAloudTest::controllerWithoutKeyUsesSystemVoice()
{
    // The default client reads the key store: an empty one means the system
    // voice at once, without a request, and a "no_key" reason for the hint.
    auto store = std::make_shared<MemoryKeyStore>();
    setKeyStore(store);
    FakeServer server([this](const FakeServer::Request &) { return FakeServer::Response{200, "audio/mpeg", m_mp3}; });
    auto client = std::make_unique<SpeechClient>();
    client->setEndpoint(QUrl(server.url().toString() + QStringLiteral("/api/v1/audio/speech")));
    auto *output = new FakeOutput;
    auto *voice = new FakeVoice;
    Controller controller(nullptr, std::move(client), std::unique_ptr<AudioOutput>(output), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy fallback(&controller, &Controller::fallingBack);
    controller.start(QStringLiteral("First sentence. Second sentence."));
    QCOMPARE(fallback.size(), 1);
    QCOMPARE(fallback.at(0).at(0).toString(), QStringLiteral("no_key"));
    QCOMPARE(voice->said, QStringList{QStringLiteral("First sentence. Second sentence.")});
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Playing, 5000);
    voice->finishSpeaking();
    QCOMPARE(controller.state(), Controller::State::Idle);
    QCOMPARE(server.requests.size(), 0);
    QVERIFY(output->enqueued.isEmpty());

    // Once a key is saved, the next reading uses the cloud voice.
    store->key = "sk-or-v1-saved-key-0123456789";
    controller.start(QStringLiteral("Now in the cloud voice."));
    QTRY_COMPARE_WITH_TIMEOUT(output->enqueued.size(), 1, 5000);
    QCOMPARE(fallback.size(), 1);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.constFirst().headers.value("authorization"), QByteArray("Bearer sk-or-v1-saved-key-0123456789"));
    output->finishCurrent();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Idle, 5000);
}

void ReadAloudTest::controllerFallsBackOn503()
{
    FakeServer server([](const FakeServer::Request &) { return FakeServer::Response{503, "application/json", "{\"detail\":\"not configured\"}"}; });
    auto *output = new FakeOutput;
    auto *voice = new FakeVoice;
    Controller controller(nullptr, clientFor(server), std::unique_ptr<AudioOutput>(output), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy events(&controller, &Controller::developerEvent);
    QSignalSpy fallback(&controller, &Controller::fallingBack);

    const QString text = QStringLiteral("First sen-\ntence here. Second sentence follows.");
    controller.start(text);
    QTRY_COMPARE_WITH_TIMEOUT(fallback.size(), 1, 5000);
    QCOMPARE(fallback.at(0).at(0).toString(), QStringLiteral("http_503"));
    QCOMPARE(voice->said, QStringList{prepareText(text)});
    QVERIFY(output->enqueued.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Playing, 5000);
    QVERIFY(controller.usingSystemVoice());
    voice->finishSpeaking();
    QCOMPARE(controller.state(), Controller::State::Idle);

    bool sawFallback = false;
    for (const QList<QVariant> &event : std::as_const(events)) {
        if (event.at(0).toString() == QLatin1String("read_aloud.fallback")) {
            sawFallback = true;
            const QJsonObject data = event.at(1).toJsonObject();
            QCOMPARE(data.value(QStringLiteral("reason")).toString(), QStringLiteral("http_503"));
            QCOMPARE(data.value(QStringLiteral("http_status")).toInt(), 503);
            QCOMPARE(data.value(QStringLiteral("chunk")).toInt(), 0);
        }
        if (event.at(0).toString() == QLatin1String("read_aloud.first_audio")) {
            QCOMPARE(event.at(1).toJsonObject().value(QStringLiteral("voice")).toString(), QStringLiteral("system"));
        }
    }
    QVERIFY(sawFallback);
}

void ReadAloudTest::controllerFallsBackMidway()
{
    int served = 0;
    FakeServer server([&](const FakeServer::Request &request) {
        if (served++ == 0) {
            return FakeServer::Response{200, "audio/mpeg", inputOf(request.body).toUtf8()};
        }
        return FakeServer::Response{503, "application/json", "{}"};
    });
    auto *output = new FakeOutput;
    auto *voice = new FakeVoice;
    Controller controller(nullptr, clientFor(server), std::unique_ptr<AudioOutput>(output), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy fallback(&controller, &Controller::fallingBack);
    controller.start(longText());
    const QStringList chunks = controller.chunks();
    QTRY_COMPARE_WITH_TIMEOUT(fallback.size(), 1, 5000);
    // The cloud chunk already playing finishes first; then the system voice reads the rest.
    QCOMPARE(output->enqueued.size(), 1);
    QVERIFY(voice->said.isEmpty());
    output->finishCurrent();
    QCOMPARE(voice->said, QStringList{chunks.mid(1).join(QLatin1Char(' '))});
    voice->finishSpeaking();
    QCOMPARE(controller.state(), Controller::State::Idle);
}

void ReadAloudTest::controllerWithoutAnyVoice()
{
    auto client = std::make_unique<SpeechClient>();
    client->setEndpoint(QUrl(QStringLiteral("http://127.0.0.1:9")));
    client->setKey(QByteArray());
    auto *voice = new FakeVoice;
    voice->isAvailable = false;
    Controller controller(nullptr, std::move(client), std::make_unique<FakeOutput>(), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy unavailable(&controller, &Controller::unavailable);
    controller.start(QStringLiteral("Nothing can read this."));
    QCOMPARE(unavailable.size(), 1);
    QCOMPARE(unavailable.at(0).at(0).toString(), QStringLiteral("no_key"));
    QCOMPARE(controller.state(), Controller::State::Idle);
}

void ReadAloudTest::controllerStopCancels()
{
    FakeServer server([this](const FakeServer::Request &) { return FakeServer::Response{200, "audio/mpeg", m_mp3, 300}; });
    auto *output = new FakeOutput;
    auto *voice = new FakeVoice;
    Controller controller(nullptr, clientFor(server), std::unique_ptr<AudioOutput>(output), std::unique_ptr<SystemVoice>(voice));
    controller.start(longText());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 5000);
    controller.stop();
    QCOMPARE(controller.state(), Controller::State::Idle);
    QTest::qWait(500);
    // The answer that arrives after Stop plays nothing.
    QVERIFY(output->enqueued.isEmpty());
    QCOMPARE(server.requests.size(), 1);

    // Pause before the first audio holds it; Resume continues.
    controller.start(QStringLiteral("Short one. Two."));
    controller.pause();
    QCOMPARE(controller.state(), Controller::State::Paused);
    QVERIFY(output->paused);
    controller.resume();
    QVERIFY(!output->paused);
    QCOMPARE(controller.state(), Controller::State::Loading);
    // A new start replaces the old reading.
    controller.start(QStringLiteral("Another text."));
    QCOMPARE(controller.chunks(), QStringList{QStringLiteral("Another text.")});
}

void ReadAloudTest::controllerEventsCarryNoText()
{
    FakeServer server([](const FakeServer::Request &) { return FakeServer::Response{503, "application/json", "{}"}; });
    auto *voice = new FakeVoice;
    Controller controller(nullptr, clientFor(server), std::make_unique<FakeOutput>(), std::unique_ptr<SystemVoice>(voice));
    QSignalSpy events(&controller, &Controller::developerEvent);
    controller.start(QStringLiteral("Confidential paragraph about zebras."));
    QTRY_VERIFY_WITH_TIMEOUT(!voice->said.isEmpty(), 5000);
    voice->finishSpeaking();
    QVERIFY(events.size() >= 3);
    for (const QList<QVariant> &event : std::as_const(events)) {
        const QByteArray json = QJsonDocument(event.at(1).toJsonObject()).toJson();
        QVERIFY2(!json.contains("zebras") && !json.contains("Confidential"), json.constData());
        // Nor the key, nor a part of it.
        QVERIFY2(!json.contains("test-key") && !json.contains("sk-or"), json.constData());
    }
}

void ReadAloudTest::mediaOutputPlaysMp3()
{
    // AVFoundation (Qt's darwin media backend) finishes loading on the main
    // run loop, which the offscreen platform does not run. Run this function
    // without windows or a Dock icon with:
    //   QT_QPA_PLATFORM=cocoa QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 readaloudtest mediaOutputPlaysMp3
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        QSKIP("Media playback needs the native platform (see the comment)");
    }
    qputenv("THOUGHTS_READ_ALOUD_VOLUME", "0");
    std::unique_ptr<AudioOutput> output = createAudioOutput();
    if (!output) {
        QSKIP("Built without QtMultimedia");
    }
    QSignalSpy started(output.get(), &AudioOutput::chunkStarted);
    QSignalSpy finished(output.get(), &AudioOutput::chunkFinished);
    QSignalSpy failed(output.get(), &AudioOutput::failed);
    output->enqueue(m_mp3);
    output->enqueue(m_mp3);
    QCOMPARE(output->pending(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size() + failed.size(), 2, 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.at(0).at(0).toString()));
    QCOMPARE(started.size(), 2);
    QCOMPARE(output->pending(), 0);

    // Stop drops what is queued.
    output->enqueue(m_mp3);
    output->stop();
    QCOMPARE(output->pending(), 0);
}

void ReadAloudTest::controllerPlaysCloudAudioEndToEnd()
{
    // The real pipeline: HTTP client, QtMultimedia output, no fakes but the server.
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        QSKIP("Media playback needs the native platform (see mediaOutputPlaysMp3)");
    }
    qputenv("THOUGHTS_READ_ALOUD_VOLUME", "0");
    FakeServer server([this](const FakeServer::Request &) { return FakeServer::Response{200, "audio/mpeg", m_mp3, 50}; });
    Controller controller(nullptr, clientFor(server));
    controller.setChunkLimits({.first = 40, .second = 40, .later = 40, .firstMinimum = 1});
    QSignalSpy events(&controller, &Controller::developerEvent);
    QSignalSpy fallback(&controller, &Controller::fallingBack);
    controller.start(QStringLiteral("First chunk is here. Second chunk follows now. Third chunk ends the test."));
    QCOMPARE(controller.chunks().size(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Playing, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::State::Idle, 20000);
    QVERIFY(fallback.isEmpty());
    QCOMPARE(server.requests.size(), 3);
    QStringList names;
    for (const QList<QVariant> &event : std::as_const(events)) {
        names.append(event.at(0).toString());
        if (names.constLast() == QLatin1String("read_aloud.first_audio")) {
            qInfo() << "first audio after" << event.at(1).toJsonObject().value(QStringLiteral("latency_ms")).toInteger() << "ms";
        }
    }
    QVERIFY(names.contains(QStringLiteral("read_aloud.first_audio")));
    QCOMPARE(names.constLast(), QStringLiteral("read_aloud.finished"));
}

void ReadAloudTest::systemVoiceSpeaksWithMockEngine()
{
    qputenv("THOUGHTS_READ_ALOUD_ENGINE", "mock");
    std::unique_ptr<SystemVoice> voice = createSystemVoice();
    if (!voice) {
        QSKIP("Built without QtTextToSpeech");
    }
    if (!voice->available()) {
        QSKIP("The mock speech engine is not installed");
    }
    QSignalSpy started(voice.get(), &SystemVoice::started);
    QSignalSpy finished(voice.get(), &SystemVoice::finished);
    voice->say(QStringLiteral("Hello from the system voice."));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QCOMPARE(started.size(), 1);
    qunsetenv("THOUGHTS_READ_ALOUD_ENGINE");
}

QTEST_MAIN(ReadAloudTest)
#include "readaloudtest.moc"
