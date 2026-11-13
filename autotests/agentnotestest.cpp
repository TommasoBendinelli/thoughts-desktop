// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/agentnotes.h"
#include "../shell/agentnotecomposer.h"
#include "../shell/agentnoteswindow.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUrlQuery>

using namespace Thoughts;
using namespace Thoughts::AgentNotes;

namespace
{
QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray png(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QString iso(const QDateTime &time)
{
    return time.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz000Z"));
}

// A note record as the server writes it (protocol/agent-notes.md, "Note record").
QJsonObject noteRecord(const QString &id, qint64 sequence, const QDateTime &created, const QString &title, const QByteArray &image = {})
{
    return {{QStringLiteral("note_id"), id},
            {QStringLiteral("sequence"), double(sequence)},
            {QStringLiteral("created_at"), iso(created)},
            {QStringLiteral("sender_device_id"), QStringLiteral("connector")},
            {QStringLiteral("title"), title},
            {QStringLiteral("message"), QStringLiteral("Message of %1").arg(title)},
            {QStringLiteral("media_type"), QStringLiteral("image/png")},
            {QStringLiteral("image_width"), 200},
            {QStringLiteral("image_height"), 500},
            {QStringLiteral("image_bytes"), image.size()},
            {QStringLiteral("image_sha256"), image.isEmpty() ? QString(64, QLatin1Char('0')) : sha(image)},
            {QStringLiteral("image_url"), QStringLiteral("/v1/agent-notes/%1/image").arg(id)},
            {QStringLiteral("document_id"), QJsonValue()},
            {QStringLiteral("document_filename"), QJsonValue()},
            {QStringLiteral("review_card_id"), QJsonValue()},
            {QStringLiteral("review_question"), QJsonValue()},
            {QStringLiteral("origin"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("connector")}}},
            {QStringLiteral("delivered_at"), QJsonValue()},
            {QStringLiteral("delivered_device_id"), QJsonValue()},
            {QStringLiteral("opened_at"), QJsonValue()},
            {QStringLiteral("deleted"), false},
            {QStringLiteral("replies"), QJsonArray()}};
}

QJsonObject replyRecord(const QString &id, const QString &noteId, qint64 sequence, const QDateTime &created, const QString &message, int strokes,
                        const QByteArray &image = {})
{
    return {{QStringLiteral("reply_id"), id},
            {QStringLiteral("sequence"), double(sequence)},
            {QStringLiteral("note_id"), noteId},
            {QStringLiteral("device_id"), QStringLiteral("example-phone")},
            {QStringLiteral("created_at"), iso(created)},
            {QStringLiteral("media_type"), QStringLiteral("image/png")},
            {QStringLiteral("image_width"), 200},
            {QStringLiteral("image_height"), 500},
            {QStringLiteral("image_bytes"), image.size()},
            {QStringLiteral("image_sha256"), image.isEmpty() ? QString(64, QLatin1Char('1')) : sha(image)},
            {QStringLiteral("image_url"), QStringLiteral("/v1/agent-notes/replies/%1/image").arg(id)},
            {QStringLiteral("strokes_url"), strokes ? QJsonValue(QStringLiteral("/v1/agent-notes/replies/%1/strokes").arg(id)) : QJsonValue()},
            {QStringLiteral("stroke_count"), strokes},
            {QStringLiteral("message"), message.isEmpty() ? QJsonValue() : QJsonValue(message)}};
}

Note note(const QJsonObject &record)
{
    return *Note::fromJson(record);
}

// The agent-notes endpoints the Mac reads; everything else is 404.
class FakeServer : public QObject
{
public:
    FakeServer()
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    // A body (POST): wait for all of it.
                    qsizetype length = 0;
                    for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    }
                    if (buffer.size() < headerEnd + 4 + length) return;
                    answer(socket, buffer.left(headerEnd + 4), buffer.mid(headerEnd + 4, length));
                    m_buffers.remove(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    QJsonArray notes;  // newest first
    QJsonArray olderNotes;  // listed only for their document (?document_id=)
    QHash<QString, QByteArray> files;  // path -> body
    QStringList requests;  // "GET /v1/agent-notes?limit=20"
    QList<QJsonObject> bodies;  // of the POSTs, in order
    QByteArray token = "secret";
    int listStatus = 200;
    int writeStatus = 0;  // set: every POST and DELETE answers it
    // note id -> "listening" / "not_listening" (GET …/listening, and the answer to a reply); a note
    // without an entry answers 404, as a server before agent_note_listening_v1.
    QHash<QString, QString> listening;
    QString device = QStringLiteral("thoughts-mac");

    QJsonObject *noteRecordOf(const QString &id)
    {
        for (int index = 0; index < notes.size(); ++index) {
            if (notes.at(index).toObject().value(QStringLiteral("note_id")).toString() == id) {
                m_scratch = notes.at(index).toObject();
                m_scratchIndex = index;
                return &m_scratch;
            }
        }
        return nullptr;
    }
    void storeScratch()
    {
        if (m_scratchIndex >= 0) notes.replace(m_scratchIndex, m_scratch);
    }

private:
    QJsonObject m_scratch;
    int m_scratchIndex = -1;
    qint64 m_replySequence = 100;

    void answer(QTcpSocket *socket, const QByteArray &request, const QByteArray &requestBody)
    {
        const QList<QByteArray> words = request.left(request.indexOf("\r\n")).split(' ');
        const QString method = QString::fromLatin1(words.value(0));
        const QUrl target(QString::fromLatin1(words.value(1)));
        requests.append(method + QLatin1Char(' ') + target.toString());
        int code = 404;
        QByteArray body;
        QByteArray type = "application/json";
        if (!request.contains(QByteArray(QByteArray("Authorization: Bearer ") + token))) {
            code = 401;
            body = R"({"detail":"invalid token"})";
        } else if (method == QLatin1String("GET") && target.path() == QLatin1String("/v1/agent-notes")) {
            code = listStatus;
            const int limit = QUrlQuery(target).queryItemValue(QStringLiteral("limit")).toInt();
            const QString document = QUrlQuery(target).queryItemValue(QStringLiteral("document_id"));
            QJsonArray page;
            // One document's notes: older ones too (olderNotes stand for notes below the newest page).
            QJsonArray source = notes;
            if (!document.isEmpty()) {
                for (const QJsonValue &value : std::as_const(olderNotes)) source.append(value);
            }
            for (const QJsonValue &value : std::as_const(source)) {
                if (page.size() >= limit) break;
                if (!document.isEmpty() && value.toObject().value(QStringLiteral("document_id")).toString() != document) continue;
                page.append(value);
            }
            body = QJsonDocument(QJsonObject{{QStringLiteral("notes"), page}}).toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("GET") && target.path().endsWith(QLatin1String("/listening"))) {
            const QString id = target.path().section(QLatin1Char('/'), -2, -2);
            if (listening.contains(id)) {
                code = 200;
                body = QJsonDocument(QJsonObject{{QStringLiteral("note_id"), id}, {QStringLiteral("state"), listening.value(id)},
                                                 {QStringLiteral("listeners"), QJsonArray()}})
                           .toJson(QJsonDocument::Compact);
            }
        } else if (method == QLatin1String("GET") && files.contains(target.path())) {
            code = 200;
            body = files.value(target.path());
            type = "image/png";
        } else if ((method == QLatin1String("POST") || method == QLatin1String("DELETE")) && writeStatus) {
            if (method == QLatin1String("POST")) bodies.append(QJsonDocument::fromJson(requestBody).object());
            code = writeStatus;
            body = R"({"detail":"try later","error_code":"busy"})";
        } else if (method == QLatin1String("POST")) {
            // As the production server (f6baac4) answers.
            const QJsonObject payload = QJsonDocument::fromJson(requestBody).object();
            bodies.append(payload);
            const QStringList parts = target.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);  // v1 agent-notes <id> receipt|replies
            if (parts.size() == 2) {
                QJsonObject note = noteRecord(payload.value(QStringLiteral("note_id")).toString(), notes.isEmpty() ? 1 : notes.first().toObject().value(QStringLiteral("sequence")).toInt() + 1,
                                              QDateTime::currentDateTimeUtc(), payload.value(QStringLiteral("title")).toString());
                note.insert(QStringLiteral("sender_device_id"), device);
                note.insert(QStringLiteral("message"), payload.value(QStringLiteral("message")));
                note.insert(QStringLiteral("origin"), payload.value(QStringLiteral("origin")));
                note.insert(QStringLiteral("document_id"), payload.value(QStringLiteral("document_id")));
                const QByteArray image = QByteArray::fromBase64(payload.value(QStringLiteral("image")).toObject().value(QStringLiteral("data_base64")).toString().toLatin1());
                note.insert(QStringLiteral("image_sha256"), sha(image));
                files.insert(note.value(QStringLiteral("image_url")).toString(), image);
                notes.prepend(note);
                code = 201;
                body = QJsonDocument(QJsonObject{{QStringLiteral("note"), note}, {QStringLiteral("delivery"), QJsonObject{{QStringLiteral("live"), false}}}}).toJson(QJsonDocument::Compact);
            } else if (parts.size() == 4 && noteRecordOf(parts.at(2))) {
                if (parts.at(3) == QLatin1String("receipt")) {
                    if (m_scratch.value(QStringLiteral("opened_at")).isNull()) m_scratch.insert(QStringLiteral("opened_at"), iso(QDateTime::currentDateTimeUtc()));
                    if (m_scratch.value(QStringLiteral("delivered_at")).isNull()) {
                        m_scratch.insert(QStringLiteral("delivered_at"), iso(QDateTime::currentDateTimeUtc()));
                        m_scratch.insert(QStringLiteral("delivered_device_id"), device);
                    }
                    storeScratch();
                    code = 200;
                    body = QJsonDocument(m_scratch).toJson(QJsonDocument::Compact);
                } else if (parts.at(3) == QLatin1String("replies")) {
                    const QByteArray image = QByteArray::fromBase64(payload.value(QStringLiteral("image")).toObject().value(QStringLiteral("data_base64")).toString().toLatin1());
                    const QString replyId = payload.value(QStringLiteral("reply_id")).toString();
                    QJsonArray replies = m_scratch.value(QStringLiteral("replies")).toArray();
                    QJsonObject reply;
                    for (const QJsonValue &existing : std::as_const(replies)) {
                        if (existing.toObject().value(QStringLiteral("reply_id")).toString() == replyId) reply = existing.toObject();
                    }
                    code = reply.isEmpty() ? 201 : 200;
                    if (reply.isEmpty()) {
                        reply = replyRecord(replyId, parts.at(2), ++m_replySequence, QDateTime::currentDateTimeUtc(), payload.value(QStringLiteral("message")).toString(),
                                            payload.value(QStringLiteral("strokes")).toObject().value(QStringLiteral("strokes")).toArray().size(), image);
                        reply.insert(QStringLiteral("device_id"), device);
                        reply.insert(QStringLiteral("media_type"), payload.value(QStringLiteral("image")).toObject().value(QStringLiteral("media_type")));
                        files.insert(reply.value(QStringLiteral("image_url")).toString(), image);
                        replies.append(reply);
                        m_scratch.insert(QStringLiteral("replies"), replies);
                        storeScratch();
                    }
                    QJsonObject answer = reply;
                    if (listening.contains(parts.at(2))) answer.insert(QStringLiteral("listening"), QJsonObject{{QStringLiteral("state"), listening.value(parts.at(2))}});
                    body = QJsonDocument(answer).toJson(QJsonDocument::Compact);
                }
            } else {
                body = R"({"detail":"note not found","error_code":"agent_note_not_found"})";
            }
        } else if (method == QLatin1String("DELETE")) {
            const QString id = target.path().section(QLatin1Char('/'), -1);
            if (noteRecordOf(id)) {
                notes.removeAt(m_scratchIndex);
                m_scratchIndex = -1;
                code = 204;
            } else {
                body = R"({"detail":"note not found","error_code":"agent_note_not_found"})";
            }
        }
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QListWidget *list(QWidget &view)
{
    return view.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
}

template<typename Predicate>
bool waitFor(Predicate predicate, int timeout = 5000)
{
    return QTest::qWaitFor(predicate, timeout);
}
}

class AgentNotesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void parsesNoteRecords();
    void senderLabels();
    void readStatePersists();
    void unreadRule();
    void clientListsWithTheDeviceToken();
    void clientReportsFailures();
    void clientCachesPicturesBySha();
    void clientStaysOfflineInTestMode();
    void centerMergesNewestPages();
    void centerAnnouncesUnopenedNotesAfterTheGracePeriod();
    void centerPollsThroughTheClient();
    void windowListsNewestFirstWithUnreadMarks();
    void windowShowsTheNoteWithLinksPictureAndReplies();
    void windowWithoutLinksOrDocumentOnThisMac();
    void windowShowNoteAndMarkAllRead();
    void windowSaysOpenedButNeverDelivered();
    void sidebarOpensNotesInPlace();
    void sidebarShowsARequestedNoteOnceListed();
    void centerWatchesWhileAnyViewDoes();
    void drawingMatchesThePhone();
    void textReplySendsThePictureAsItIs();
    void drawnReplySendsTheComposite();
    void pageGoesToTheAgent();
    void deleteAsksFirst();
    void filtersFollowTheLibrary();
    void viewShowsOneProjectWithItsOlderNotes();
    void viewWarnsWhileNoAgentListens();
    void replyPhotoIsShownWithTheReply();

private:
    QTemporaryDir m_dir;
    int m_counter = 0;
    QString statePath() { return m_dir.filePath(QStringLiteral("state-%1.json").arg(++m_counter)); }
    QString cachePath() { return m_dir.filePath(QStringLiteral("cache-%1").arg(++m_counter)); }
};

void AgentNotesTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    qunsetenv("THOUGHTS_TOKEN");
    qunsetenv("THOUGHTS_SERVER_URL");
    QVERIFY(m_dir.isValid());
}

void AgentNotesTest::parsesNoteRecords()
{
    const QDateTime created(QDate(2026, 11, 10), QTime(12, 21, 56, 678), QTimeZone::UTC);
    QJsonObject record = noteRecord(QStringLiteral("n1"), 629, created, QStringLiteral("Teacher check"));
    record.insert(QStringLiteral("created_at"), QStringLiteral("2026-11-10T12:21:56.678975Z"));
    record.insert(QStringLiteral("opened_at"), QStringLiteral("2026-11-10T12:24:39.118592Z"));
    record.insert(QStringLiteral("document_id"), QStringLiteral("78f95f08-fb36-461a-91ea-cbe67d46872f"));
    record.insert(QStringLiteral("review_card_id"), QStringLiteral("cc7b3a63-0760-5237-83fe-275aed2ad4ae"));
    record.insert(QStringLiteral("review_question"), QStringLiteral("How does 1F1B reduce memory?"));
    record.insert(QStringLiteral("replies"), QJsonArray{replyRecord(QStringLiteral("r2"), QStringLiteral("n1"), 9, created.addSecs(120), QStringLiteral("second"), 0),
                                                        replyRecord(QStringLiteral("r1"), QStringLiteral("n1"), 4, created.addSecs(60), QStringLiteral("first"), 3)});
    QJsonObject deleted = noteRecord(QStringLiteral("gone"), 628, created, QStringLiteral("Deleted"));
    deleted.insert(QStringLiteral("deleted"), true);
    QJsonObject nameless = noteRecord(QString(), 627, created, QStringLiteral("No id"));
    nameless.remove(QStringLiteral("note_id"));
    const QByteArray answer = QJsonDocument(QJsonObject{{QStringLiteral("notes"), QJsonArray{record, deleted, nameless}}}).toJson();

    QString error;
    const auto notes = parseNotes(answer, &error);
    QVERIFY2(notes, qPrintable(error));
    QCOMPARE(notes->size(), 1);
    const Note &parsed = notes->first();
    QCOMPARE(parsed.id, QStringLiteral("n1"));
    QCOMPARE(parsed.sequence, 629);
    QCOMPARE(parsed.title, QStringLiteral("Teacher check"));
    QCOMPARE(parsed.message, QStringLiteral("Message of Teacher check"));
    // Microseconds, as the server writes them.
    QVERIFY(qAbs(parsed.createdAt.msecsTo(QDateTime(QDate(2026, 11, 10), QTime(12, 21, 56, 678), QTimeZone::UTC))) <= 1);
    QVERIFY(qAbs(parsed.openedAt.msecsTo(QDateTime(QDate(2026, 11, 10), QTime(12, 24, 39, 118), QTimeZone::UTC))) <= 1);
    QCOMPARE(parsed.createdAt.timeSpec(), Qt::UTC);
    QCOMPARE(parsed.documentId, QStringLiteral("78f95f08-fb36-461a-91ea-cbe67d46872f"));
    QCOMPARE(parsed.reviewCardId, QStringLiteral("cc7b3a63-0760-5237-83fe-275aed2ad4ae"));
    QCOMPARE(parsed.reviewQuestion, QStringLiteral("How does 1F1B reduce memory?"));
    QCOMPARE(parsed.imageWidth, 200);
    QCOMPARE(parsed.imageHeight, 500);
    QCOMPARE(parsed.imageUrl, QStringLiteral("/v1/agent-notes/n1/image"));
    QVERIFY(parsed.documentFilename.isEmpty());
    // Replies oldest first, whatever order they came in.
    QCOMPARE(parsed.replies.size(), 2);
    QCOMPARE(parsed.replies.at(0).id, QStringLiteral("r1"));
    QCOMPARE(parsed.replies.at(0).strokeCount, 3);
    QCOMPARE(parsed.replies.at(0).message, QStringLiteral("first"));
    QCOMPARE(parsed.replies.at(1).deviceId, QStringLiteral("example-phone"));
    QCOMPARE(parsed.replies.at(1).imageUrl, QStringLiteral("/v1/agent-notes/replies/r2/image"));

    QVERIFY(!parseNotes("not json", &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!parseNotes(R"({"items": []})", &error));
    QCOMPARE(parseNotes(R"({"notes": []})")->size(), 0);
}

void AgentNotesTest::senderLabels()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("a"), 1, now, QStringLiteral("A"));
    QCOMPARE(note(record).senderLabel(), QStringLiteral("Agent via the connector"));
    record.insert(QStringLiteral("sender_device_id"), QStringLiteral("thoughts-mac"));
    record.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("mcp")}, {QStringLiteral("conversation_agent"), QStringLiteral("claude")},
                                                        {QStringLiteral("conversation_url"), QStringLiteral("https://claude.ai/chat/abc")}});
    QCOMPARE(note(record).senderLabel(), QStringLiteral("Claude on thoughts-mac"));
    QCOMPARE(note(record).conversationUrl(), QUrl(QStringLiteral("https://claude.ai/chat/abc")));
    record.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("conversation_agent"), QStringLiteral("codex")}, {QStringLiteral("conversation_url"), QStringLiteral("javascript:alert(1)")}});
    QCOMPARE(note(record).senderLabel(), QStringLiteral("Codex on thoughts-mac"));
    QVERIFY(note(record).conversationUrl().isEmpty());
    record.insert(QStringLiteral("sender_device_id"), QStringLiteral("example-phone"));
    record.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}, {QStringLiteral("page"), 3}});
    QVERIFY(note(record).isPhonePage());
    QCOMPARE(note(record).senderLabel(), QStringLiteral("You, from the phone"));
}

void AgentNotesTest::readStatePersists()
{
    const QString path = statePath();
    {
        ReadState state(path);
        QCOMPARE(state.notificationBaseline(), -1);
        QVERIFY(state.markSeen(QStringLiteral("n1")));
        QVERIFY(!state.markSeen(QStringLiteral("n1")));
        state.setNotificationBaseline(41);
        state.markDecided(QStringLiteral("n2"));
    }
    ReadState again(path);
    QVERIFY(again.isSeen(QStringLiteral("n1")));
    QVERIFY(!again.isSeen(QStringLiteral("n2")));
    QVERIFY(again.isDecided(QStringLiteral("n2")));
    QCOMPARE(again.notificationBaseline(), 41);
}

void AgentNotesTest::unreadRule()
{
    ReadState state(statePath());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("u"), 5, now, QStringLiteral("U"));
    QVERIFY(isUnread(note(record), state));
    // Opened on the phone: read everywhere.
    QJsonObject opened = record;
    opened.insert(QStringLiteral("opened_at"), iso(now));
    QVERIFY(!isUnread(note(opened), state));
    // The user's own page from the phone is never unread.
    QJsonObject page = record;
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}});
    QVERIFY(!isUnread(note(page), state));
    // Shown on this Mac.
    state.markSeen(QStringLiteral("u"));
    QVERIFY(!isUnread(note(record), state));
}

void AgentNotesTest::clientListsWithTheDeviceToken()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int sequence = 30; sequence > 0; --sequence) server.notes.append(noteRecord(QStringLiteral("n%1").arg(sequence), sequence, now, QStringLiteral("Note %1").arg(sequence)));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    std::optional<Client::Result> result;
    client.list(5, this, [&](const Client::Result &answer) { result = answer; });
    QVERIFY(waitFor([&] { return result.has_value(); }));
    QCOMPARE(result->failure, Client::Failure::None);
    QCOMPARE(result->notes.size(), 5);
    QCOMPARE(result->notes.first().sequence, 30);
    QCOMPARE(server.requests, QStringList{QStringLiteral("GET /v1/agent-notes?limit=5")});
}

void AgentNotesTest::clientReportsFailures()
{
    FakeServer server;
    Client refused(cachePath());
    refused.setServer(server.url(), "wrong");
    std::optional<Client::Result> result;
    refused.list(5, this, [&](const Client::Result &answer) { result = answer; });
    QVERIFY(waitFor([&] { return result.has_value(); }));
    QCOMPARE(result->failure, Client::Failure::Unauthorized);
    QVERIFY(!result->error.isEmpty());

    server.listStatus = 404;  // a server without agent notes
    Client old(cachePath());
    old.setServer(server.url(), "secret");
    result.reset();
    old.list(5, this, [&](const Client::Result &answer) { result = answer; });
    QVERIFY(waitFor([&] { return result.has_value(); }));
    QCOMPARE(result->failure, Client::Failure::Unsupported);

    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();
    Client offline(cachePath());
    offline.setServer(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)), "secret");
    result.reset();
    offline.list(5, this, [&](const Client::Result &answer) { result = answer; });
    QVERIFY(waitFor([&] { return result.has_value(); }, 15000));
    QCOMPARE(result->failure, Client::Failure::Unreachable);
}

void AgentNotesTest::clientCachesPicturesBySha()
{
    FakeServer server;
    const QByteArray picture = png(QSize(60, 150), Qt::red);
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    const QString cache = cachePath();
    Client client(cache);
    client.setServer(server.url(), "secret");
    QImage image;
    QString error;
    bool done = false;
    client.image(QStringLiteral("/v1/agent-notes/n1/image"), sha(picture), QStringLiteral("image/png"), this, [&](const QImage &answer, const QString &failure) {
        image = answer;
        error = failure;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QVERIFY2(!image.isNull(), qPrintable(error));
    QCOMPARE(image.size(), QSize(60, 150));
    QVERIFY(QFile::exists(client.cachedImagePath(sha(picture), QStringLiteral("image/png"))));
    QCOMPARE(server.requests.size(), 1);

    // Pictures never change: the second time comes from the cache.
    done = false;
    image = QImage();
    client.image(QStringLiteral("/v1/agent-notes/n1/image"), sha(picture), QStringLiteral("image/png"), this, [&](const QImage &answer, const QString &) {
        image = answer;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QCOMPARE(image.size(), QSize(60, 150));
    QCOMPARE(server.requests.size(), 1);

    // Bytes that do not match the record's checksum are refused and not kept.
    const QString wrongSha = QString(64, QLatin1Char('a'));
    done = false;
    client.image(QStringLiteral("/v1/agent-notes/n1/image"), wrongSha, QStringLiteral("image/png"), this, [&](const QImage &answer, const QString &failure) {
        image = answer;
        error = failure;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QVERIFY(image.isNull());
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(client.cachedImagePath(wrongSha, QStringLiteral("image/png"))));

    // Only paths of this server below /v1/agent-notes/ are fetched.
    const int before = server.requests.size();
    for (const QString &url : {QStringLiteral("http://example.com/v1/agent-notes/n1/image"), QStringLiteral("/v1/sources/secret"), QStringLiteral("/v1/agent-notes/../objects/x")}) {
        done = false;
        client.image(url, QString(64, QLatin1Char('b')), QStringLiteral("image/png"), this, [&](const QImage &answer, const QString &failure) {
            image = answer;
            error = failure;
            done = true;
        });
        QVERIFY(waitFor([&] { return done; }));
        QVERIFY2(image.isNull(), qPrintable(url));
    }
    QCOMPARE(server.requests.size(), before);
}

void AgentNotesTest::clientStaysOfflineInTestMode()
{
    // No setServer and no THOUGHTS_TOKEN / THOUGHTS_SERVER_URL: under test mode the Keychain
    // and the saved address are never used.
    Client client(cachePath());
    QString error;
    QVERIFY(!client.ensureServer(&error));
    QVERIFY(!error.isEmpty());
    std::optional<Client::Result> result;
    client.list(5, this, [&](const Client::Result &answer) { result = answer; });
    QVERIFY(waitFor([&] { return result.has_value(); }));
    QCOMPARE(result->failure, Client::Failure::NotConnected);
}

void AgentNotesTest::centerMergesNewestPages()
{
    Client client(cachePath());
    const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-2);
    Center center(&client, statePath(), [] { return QDateTime::currentDateTimeUtc(); });
    QSignalSpy changed(&center, &Center::notesChanged);
    QSignalSpy unread(&center, &Center::unreadCountChanged);
    QSignalSpy seen(&center, &Center::seenChanged);
    QList<Note> first;
    for (int sequence = 10; sequence >= 6; --sequence) first.append(note(noteRecord(QStringLiteral("n%1").arg(sequence), sequence, old, QStringLiteral("Note %1").arg(sequence))));
    center.apply(first);
    QCOMPARE(center.notes().size(), 5);
    QCOMPARE(center.unreadCount(), 5);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(unread.count(), 1);

    // The same answer again changes nothing on screen.
    center.apply(first);
    QCOMPARE(changed.count(), 1);

    // A shorter, newer page: from its lowest sequence up it is the truth (n9 was deleted),
    // older notes stay; n10 was opened on the phone.
    QJsonObject opened = noteRecord(QStringLiteral("n10"), 10, old, QStringLiteral("Note 10"));
    opened.insert(QStringLiteral("opened_at"), iso(old.addSecs(30)));
    const QList<Note> second{note(noteRecord(QStringLiteral("n12"), 12, old, QStringLiteral("Note 12"))), note(noteRecord(QStringLiteral("n11"), 11, old, QStringLiteral("Note 11"))),
                             note(opened)};
    center.apply(second);
    QStringList ids;
    for (const Note &entry : center.notes()) ids.append(entry.id);
    QCOMPARE(ids, (QStringList{QStringLiteral("n12"), QStringLiteral("n11"), QStringLiteral("n10"), QStringLiteral("n9"), QStringLiteral("n8"), QStringLiteral("n7"), QStringLiteral("n6")}));
    center.apply({note(noteRecord(QStringLiteral("n12"), 12, old, QStringLiteral("Note 12"))), note(noteRecord(QStringLiteral("n11"), 11, old, QStringLiteral("Note 11"))), note(opened),
                  note(noteRecord(QStringLiteral("n8"), 8, old, QStringLiteral("Note 8")))});
    ids.clear();
    for (const Note &entry : center.notes()) ids.append(entry.id);
    QCOMPARE(ids, (QStringList{QStringLiteral("n12"), QStringLiteral("n11"), QStringLiteral("n10"), QStringLiteral("n8"), QStringLiteral("n7"), QStringLiteral("n6")}));
    QCOMPARE(center.unreadCount(), 5);

    // Shown here: one unread less, and only the marks change.
    const int changes = changed.count();
    center.markSeen(QStringLiteral("n12"));
    QCOMPARE(center.unreadCount(), 4);
    QCOMPARE(seen.count(), 1);
    QCOMPARE(changed.count(), changes);
    QCOMPARE(unread.last().first().toInt(), 4);
    center.markSeen(QStringLiteral("n12"));
    QCOMPARE(seen.count(), 1);
    center.markAllSeen();
    QCOMPARE(center.unreadCount(), 0);
    QCOMPARE(seen.count(), 2);
}

void AgentNotesTest::centerAnnouncesUnopenedNotesAfterTheGracePeriod()
{
    Client client(cachePath());
    QDateTime now(QDate(2026, 11, 10), QTime(14, 0), QTimeZone::UTC);
    const QString path = statePath();
    auto center = std::make_unique<Center>(&client, path, [&now] { return now; });
    QSignalSpy attention(center.get(), &Center::noteNeedsAttention);
    QList<QJsonObject> records{noteRecord(QStringLiteral("old"), 100, now.addSecs(-5), QStringLiteral("Before this Mac listed"))};
    const auto answer = [&records] {
        QList<Note> notes;
        for (auto it = records.crbegin(); it != records.crend(); ++it) notes.append(note(*it));
        return notes;
    };
    // The first list sets the baseline: nothing that already existed is announced.
    center->apply(answer());
    QCOMPARE(center->readState().notificationBaseline(), 100);
    now = now.addSecs(60);
    center->apply(answer());
    QCOMPARE(attention.count(), 0);

    // A new note waits for the phone first.
    records.append(noteRecord(QStringLiteral("new"), 101, now, QStringLiteral("New note")));
    center->apply(answer());
    QCOMPARE(attention.count(), 0);
    now = now.addSecs(Center::GracePeriodSeconds - 5);
    center->apply(answer());
    QCOMPARE(attention.count(), 0);
    now = now.addSecs(10);
    center->apply(answer());
    QCOMPARE(attention.count(), 1);
    QCOMPARE(attention.first().first().value<Note>().id, QStringLiteral("new"));
    // Once only, also after a restart.
    center->apply(answer());
    center = std::make_unique<Center>(&client, path, [&now] { return now; });
    QSignalSpy afterRestart(center.get(), &Center::noteNeedsAttention);
    center->apply(answer());
    QCOMPARE(afterRestart.count(), 0);

    // Opened on the phone within the grace period: never announced.
    QJsonObject opened = noteRecord(QStringLiteral("opened"), 102, now, QStringLiteral("Read on the phone"));
    records.append(opened);
    center->apply(answer());
    opened.insert(QStringLiteral("opened_at"), iso(now.addSecs(3)));
    records.last() = opened;
    now = now.addSecs(Center::GracePeriodSeconds + 1);
    center->apply(answer());
    // The user's own page, and a note that is already old when this Mac first sees it.
    QJsonObject page = noteRecord(QStringLiteral("page"), 103, now.addSecs(-60), QStringLiteral("Page 3"));
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}});
    records.append(page);
    records.append(noteRecord(QStringLiteral("stale"), 104, now.addSecs(-(Center::NotifyWithinSeconds + 60)), QStringLiteral("While the Mac slept")));
    center->apply(answer());
    QCOMPARE(afterRestart.count(), 0);

    // Many at once: the newest three.
    for (int index = 0; index < 5; ++index) records.append(noteRecord(QStringLiteral("burst%1").arg(index), 110 + index, now.addSecs(-40), QStringLiteral("Burst %1").arg(index)));
    center->apply(answer());
    QCOMPARE(afterRestart.count(), 3);
    QCOMPARE(afterRestart.at(0).first().value<Note>().id, QStringLiteral("burst2"));
    QCOMPARE(afterRestart.at(2).first().value<Note>().id, QStringLiteral("burst4"));
    center->apply(answer());
    QCOMPARE(afterRestart.count(), 3);
}

void AgentNotesTest::centerPollsThroughTheClient()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n2"), 2, now.addSecs(-120), QStringLiteral("Second")));
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-240), QStringLiteral("First")));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    QSignalSpy changed(&center, &Center::notesChanged);
    center.refresh();
    QVERIFY(center.isRefreshing());
    QVERIFY(waitFor([&] { return center.hasListed(); }));
    QCOMPARE(center.notes().size(), 2);
    QCOMPARE(center.lastFailure(), Client::Failure::None);
    QVERIFY(center.lastUpdated().isValid());
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::IdleLimit));

    // The window asks for more.
    center.setWatching(&center, true);
    QVERIFY(waitFor([&] { return server.requests.size() == 2 && !center.isRefreshing(); }));
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::WatchingLimit));
    center.setWatching(&center, false);

    server.token = "rotated";
    center.refresh();
    QVERIFY(waitFor([&] { return !center.isRefreshing() && center.lastFailure() != Client::Failure::None; }));
    QCOMPARE(center.lastFailure(), Client::Failure::Unauthorized);
    QVERIFY(!center.lastError().isEmpty());
    // The notes already listed stay.
    QCOMPARE(center.notes().size(), 2);
}

void AgentNotesTest::windowListsNewestFirstWithUnreadMarks()
{
    FakeServer server;
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject withReplies = noteRecord(QStringLiteral("n3"), 3, now.addSecs(-60), QStringLiteral("Newest"));
    withReplies.insert(QStringLiteral("replies"), QJsonArray{replyRecord(QStringLiteral("r1"), QStringLiteral("n3"), 1, now, QStringLiteral("a"), 0),
                                                             replyRecord(QStringLiteral("r2"), QStringLiteral("n3"), 2, now, QStringLiteral("b"), 0)});
    QJsonObject opened = noteRecord(QStringLiteral("n2"), 2, now.addSecs(-600), QStringLiteral("Read on the phone"));
    opened.insert(QStringLiteral("opened_at"), iso(now.addSecs(-500)));
    // The server lists the same notes (opening the window asks it again).
    server.notes = QJsonArray{withReplies, opened, noteRecord(QStringLiteral("n1"), 1, now.addSecs(-900), QStringLiteral("Oldest"))};
    QList<Note> listed;
    for (const QJsonValue &value : std::as_const(server.notes)) listed.append(note(value.toObject()));
    center.apply(listed);
    QCOMPARE(center.unreadCount(), 2);

    AgentNotesWindow window(&center, {}, {}, {});
    auto *list = window.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 3);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Newest"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("Read on the phone"));
    QCOMPARE(list->item(2)->text(), QStringLiteral("Oldest"));
    QVERIFY(list->item(0)->data(AgentNotesWindow::UnreadRole).toBool());
    QVERIFY(!list->item(1)->data(AgentNotesWindow::UnreadRole).toBool());
    QVERIFY(list->item(2)->data(AgentNotesWindow::UnreadRole).toBool());
    const QString meta = list->item(0)->data(AgentNotesWindow::MetaRole).toString();
    QVERIFY2(meta.contains(QStringLiteral("Agent via the connector")), qPrintable(meta));
    QVERIFY2(meta.contains(QStringLiteral("2 replies")), qPrintable(meta));
    QVERIFY2(meta.startsWith(QStringLiteral("Today")) || meta.startsWith(QStringLiteral("Yesterday")), qPrintable(meta));
    QCOMPARE(list->item(0)->data(AgentNotesWindow::PreviewRole).toString(), QStringLiteral("Message of Newest"));
    // Nothing is read before it is on screen.
    QCOMPARE(window.currentNoteId(), QStringLiteral("n3"));
    QCOMPARE(center.unreadCount(), 2);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size() == 1 && !center.isRefreshing(); }));
    QCOMPARE(list->count(), 3);
    QVERIFY(waitFor([&] { return center.unreadCount() == 1; }));
    QVERIFY(!list->item(0)->data(AgentNotesWindow::UnreadRole).toBool());
    QVERIFY(list->item(2)->data(AgentNotesWindow::UnreadRole).toBool());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("agentNotesStatus")));
    // Selecting the next unread note reads it too.
    list->setCurrentRow(2);
    QVERIFY(waitFor([&] { return center.unreadCount() == 0; }));
    QVERIFY(!list->item(2)->data(AgentNotesWindow::UnreadRole).toBool());
    QCOMPARE(window.currentNoteId(), QStringLiteral("n1"));
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("agentNotesMarkAllRead"))->isEnabled());
}

void AgentNotesTest::windowShowsTheNoteWithLinksPictureAndReplies()
{
    FakeServer server;
    const QByteArray picture = png(QSize(200, 500), QColor(30, 90, 200));
    const QByteArray annotated = png(QSize(200, 500), QColor(200, 30, 30));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("n1"), 7, now.addSecs(-300), QStringLiteral("G = sequences per optimizer step"), picture);
    record.insert(QStringLiteral("message"), QStringLiteral("Fixed: G = sequences per optimizer step."));
    record.insert(QStringLiteral("document_id"), QStringLiteral("doc-1"));
    record.insert(QStringLiteral("review_card_id"), QStringLiteral("card-1"));
    record.insert(QStringLiteral("review_question"), QStringLiteral("How does a 1F1B schedule reduce memory?"));
    record.insert(QStringLiteral("opened_at"), iso(now.addSecs(-200)));
    record.insert(QStringLiteral("replies"), QJsonArray{replyRecord(QStringLiteral("r1"), QStringLiteral("n1"), 1, now.addSecs(-100), QStringLiteral("What is b exactly?"), 0),
                                                        replyRecord(QStringLiteral("r2"), QStringLiteral("n1"), 2, now.addSecs(-50), QString(), 4, annotated)});
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/replies/r2/image"), annotated);
    server.notes.append(record);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.apply({note(record)});

    QStringList resolved;
    QStringList opened;
    QStringList cards;
    AgentNotesWindow window(
        &center,
        [&](const QString &documentId, const QString &filename) {
            resolved.append(documentId + QLatin1Char('|') + filename);
            return AgentNotesWindow::DocumentLink{QStringLiteral("/library/current_pdfs/concepts_lsaie_ta.pdf"), QStringLiteral("Concepts: LSAIE TA")};
        },
        [&](const QString &documentId, const QString &path) { opened.append(documentId + QLatin1Char('|') + path); },
        [&](const QString &cardId) { cards.append(cardId); });
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return !server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).isEmpty() && !center.isRefreshing(); }));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n1"));

    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("agentNoteTitle"))->text(), QStringLiteral("G = sequences per optimizer step"));
    const QString meta = window.findChild<QLabel *>(QStringLiteral("agentNoteMeta"))->text();
    QVERIFY2(meta.contains(QStringLiteral("Agent via the connector")), qPrintable(meta));
    QVERIFY2(meta.contains(QStringLiteral("Opened ")), qPrintable(meta));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("agentNoteMessage"))->text(), QStringLiteral("Fixed: G = sequences per optimizer step."));
    auto *question = window.findChild<QLabel *>(QStringLiteral("agentNoteQuestion"));
    QVERIFY(question->isVisible());
    QVERIFY(question->text().contains(QStringLiteral("How does a 1F1B schedule reduce memory?")));

    // The linked document and card.
    auto *documentButton = window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenDocument"));
    QVERIFY(documentButton->isVisible());
    QVERIFY(documentButton->isEnabled());
    QVERIFY2(documentButton->text().contains(QStringLiteral("Concepts: LSAIE TA")), qPrintable(documentButton->text()));
    QCOMPARE(resolved.last(), QStringLiteral("doc-1|"));
    documentButton->click();
    QCOMPARE(opened, QStringList{QStringLiteral("doc-1|/library/current_pdfs/concepts_lsaie_ta.pdf")});
    auto *cardButton = window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenCard"));
    QVERIFY(cardButton->isVisible());
    QCOMPARE(cardButton->toolTip(), QStringLiteral("How does a 1F1B schedule reduce memory?"));
    cardButton->click();
    QCOMPARE(cards, QStringList{QStringLiteral("card-1")});
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenConversation"))->isVisible());

    // The picture, large: as wide as the window allows, at most MaxWidth, aspect kept.
    auto *image = window.findChild<AgentNoteImageView *>(QStringLiteral("agentNoteImage"));
    QVERIFY(image);
    QVERIFY(waitFor([&] { return !image->image().isNull(); }));
    QCOMPARE(image->image().size(), QSize(200, 500));
    QCOMPARE(image->width(), AgentNoteImageView::MaxWidth);
    QCOMPARE(image->height(), qRound(AgentNoteImageView::MaxWidth * 2.5));
    window.resize(700, 800);
    QVERIFY(waitFor([&] { return image->width() < AgentNoteImageView::MaxWidth; }));
    QCOMPARE(image->height(), qRound(image->width() * 2.5));

    // The replies from the phone, oldest first; a drawing only when strokes were drawn.
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("agentNoteRepliesHeader"))->isVisible());
    QList<QWidget *> replies;
    for (QWidget *child : window.findChildren<QWidget *>(QStringLiteral("agentNoteReply"))) {
        if (child->isVisible()) replies.append(child);
    }
    QCOMPARE(replies.size(), 2);
    std::sort(replies.begin(), replies.end(), [](QWidget *a, QWidget *b) { return a->y() < b->y(); });
    QCOMPARE(replies.at(0)->property("replyId").toString(), QStringLiteral("r1"));
    QCOMPARE(replies.at(0)->findChild<QLabel *>(QStringLiteral("agentNoteReplyMessage"))->text(), QStringLiteral("What is b exactly?"));
    QVERIFY(!replies.at(0)->findChild<AgentNoteImageView *>());
    QVERIFY(replies.at(0)->findChild<QLabel *>(QStringLiteral("agentNoteReplyMeta"))->text().contains(QStringLiteral("example-phone")));
    auto *drawing = replies.at(1)->findChild<AgentNoteImageView *>(QStringLiteral("agentNoteReplyImage"));
    QVERIFY(drawing);
    QVERIFY(waitFor([&] { return !drawing->image().isNull(); }));
    QCOMPARE(drawing->image().pixelColor(10, 10), QColor(200, 30, 30));
    QVERIFY(replies.at(1)->findChild<QLabel *>(QStringLiteral("agentNoteReplyMeta"))->text().contains(QStringLiteral("4 strokes")));

    // A refresh with an unchanged note keeps the pictures (no second download).
    const int requests = server.requests.size();
    center.apply({note(record)});
    QJsonObject withThird = record;
    QJsonArray replyRecords = record.value(QStringLiteral("replies")).toArray();
    replyRecords.append(replyRecord(QStringLiteral("r3"), QStringLiteral("n1"), 3, now, QStringLiteral("Third"), 0));
    withThird.insert(QStringLiteral("replies"), replyRecords);
    center.apply({note(withThird)});
    QVERIFY(waitFor([&] {
        int visible = 0;
        for (QWidget *child : window.findChildren<QWidget *>(QStringLiteral("agentNoteReply"))) visible += child->isVisible() ? 1 : 0;
        return visible == 3;
    }));
    QTest::qWait(100);
    QCOMPARE(server.requests.size(), requests);
    QVERIFY(!image->image().isNull());
}

void AgentNotesTest::windowWithoutLinksOrDocumentOnThisMac()
{
    Client client(cachePath());
    Center center(&client, statePath());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject elsewhere = noteRecord(QStringLiteral("n2"), 2, now, QStringLiteral("Document elsewhere"));
    elsewhere.insert(QStringLiteral("document_id"), QStringLiteral("doc-missing"));
    elsewhere.insert(QStringLiteral("document_filename"), QStringLiteral("missing.pdf"));
    QJsonObject plain = noteRecord(QStringLiteral("n1"), 1, now.addSecs(-10), QStringLiteral("No links"));
    plain.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("conversation_agent"), QStringLiteral("claude")}, {QStringLiteral("conversation_url"), QStringLiteral("https://claude.ai/chat/x")}});
    center.apply({note(elsewhere), note(plain)});
    AgentNotesWindow window(&center, [](const QString &, const QString &) { return AgentNotesWindow::DocumentLink{}; }, {}, {});
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *documentButton = window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenDocument"));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n2"));
    QVERIFY(documentButton->isVisible());
    QVERIFY(!documentButton->isEnabled());
    QVERIFY(documentButton->text().contains(QStringLiteral("missing.pdf")));
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenCard"))->isVisible());
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("agentNoteQuestion"))->isVisible());
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("agentNoteRepliesHeader"))->isVisible());

    window.findChild<QListWidget *>(QStringLiteral("agentNotesList"))->setCurrentRow(1);
    QCOMPARE(window.currentNoteId(), QStringLiteral("n1"));
    QVERIFY(!documentButton->isVisible());
    auto *conversation = window.findChild<QPushButton *>(QStringLiteral("agentNoteOpenConversation"));
    QVERIFY(conversation->isVisible());
    QCOMPARE(conversation->toolTip(), QStringLiteral("https://claude.ai/chat/x"));
    // Without a picture server the picture says why it is missing.
    auto *image = window.findChild<AgentNoteImageView *>(QStringLiteral("agentNoteImage"));
    QVERIFY(waitFor([&] { return image->failed(); }));
    QVERIFY(!image->statusText().isEmpty());
}

void AgentNotesTest::windowShowNoteAndMarkAllRead()
{
    Client client(cachePath());
    Center center(&client, statePath());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<Note> notes;
    for (int sequence = 5; sequence >= 1; --sequence) notes.append(note(noteRecord(QStringLiteral("n%1").arg(sequence), sequence, now.addSecs(-sequence * 60), QStringLiteral("Note %1").arg(sequence))));
    center.apply(notes);
    AgentNotesWindow window(&center, {}, {}, {});
    // A notification's note, selected when the window opens.
    window.showNote(QStringLiteral("n2"));
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n2"));
    QVERIFY(waitFor([&] { return center.unreadCount() == 4; }));
    QVERIFY(!center.isUnread(*center.note(QStringLiteral("n2"))));

    // A new note arrives: the selection stays where the reader is.
    notes.prepend(note(noteRecord(QStringLiteral("n6"), 6, now, QStringLiteral("Note 6"))));
    center.apply(notes);
    auto *list = window.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
    QCOMPARE(list->count(), 6);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Note 6"));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n2"));
    QVERIFY(list->item(0)->data(AgentNotesWindow::UnreadRole).toBool());
    QCOMPARE(center.unreadCount(), 5);

    auto *markAll = window.findChild<QPushButton *>(QStringLiteral("agentNotesMarkAllRead"));
    QVERIFY(markAll->isEnabled());
    markAll->click();
    QCOMPARE(center.unreadCount(), 0);
    for (int row = 0; row < list->count(); ++row) QVERIFY(!list->item(row)->data(AgentNotesWindow::UnreadRole).toBool());
    QVERIFY(!markAll->isEnabled());

    // An unknown note (deleted meanwhile) falls back to the newest.
    window.showNote(QStringLiteral("gone"));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n6"));
}

// Receipts as the phone posts them: "opened" for a note shown here that nobody opened yet,
// once; never for one already opened, nor for the user's own page; never "delivered", and
// never the inbox (which would tell agents a phone is listening).
void AgentNotesTest::windowSaysOpenedButNeverDelivered()
{
    FakeServer server;
    const QByteArray picture = png(QSize(40, 40), Qt::green);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject page = noteRecord(QStringLiteral("n3"), 3, now, QStringLiteral("Page"), picture);
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}});
    QJsonObject opened = noteRecord(QStringLiteral("n2"), 2, now.addSecs(-30), QStringLiteral("Opened on the phone"), picture);
    opened.insert(QStringLiteral("opened_at"), iso(now.addSecs(-20)));
    server.notes = QJsonArray{page, opened, noteRecord(QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("Not opened"), picture)};
    for (const QString &id : {QStringLiteral("n1"), QStringLiteral("n2"), QStringLiteral("n3")}) server.files.insert(QStringLiteral("/v1/agent-notes/%1/image").arg(id), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    const auto receipts = [&] { return server.requests.filter(QStringLiteral("/receipt")); };
    {
        AgentNotesWindow window(&center, {}, {}, {});
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Opening the window asks the server at once.
        QVERIFY(waitFor([&] { return center.hasListed(); }));
        auto *list = window.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
        for (int row : {0, 1, 2, 1, 2}) {
            list->setCurrentRow(row);
            QTest::qWait(50);
        }
        QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->openedAt.isValid(); }));
        QCOMPARE(receipts(), QStringList{QStringLiteral("POST /v1/agent-notes/n1/receipt")});
        QCOMPARE(server.bodies.last(), (QJsonObject{{QStringLiteral("state"), QStringLiteral("opened")}}));
        // Opened now, for the agent and for this Mac's unread count.
        QCOMPARE(center.unreadCount(), 0);
        window.findChild<QPushButton *>(QStringLiteral("agentNotesRefresh"))->click();
        QVERIFY(waitFor([&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size() >= 2 && !center.isRefreshing(); }));
        window.hide();
    }
    QCOMPARE(receipts().size(), 1);
    for (const QString &request : std::as_const(server.requests)) {
        QVERIFY2(!request.contains(QLatin1String("/inbox")), qPrintable(request));
    }
    for (const QJsonObject &body : std::as_const(server.bodies)) QVERIFY(body.value(QStringLiteral("state")).toString() != QLatin1String("delivered"));
}

// Sidebar › Agent Notes: the list, newest first with unread notes marked; a click shows the
// note in its place, at the sidebar's width; Open Large (or a click on the picture) asks for
// the Agent Notes window.
void AgentNotesTest::sidebarOpensNotesInPlace()
{
    Client client(cachePath());
    Center center(&client, statePath());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject opened = noteRecord(QStringLiteral("n2"), 2, now.addSecs(-600), QStringLiteral("Note 2"));
    opened.insert(QStringLiteral("opened_at"), iso(now.addSecs(-500)));
    QJsonObject linked = noteRecord(QStringLiteral("n1"), 1, now.addSecs(-900), QStringLiteral("Note 1"));
    linked.insert(QStringLiteral("document_id"), QStringLiteral("doc-1"));
    linked.insert(QStringLiteral("review_card_id"), QStringLiteral("card-1"));
    linked.insert(QStringLiteral("review_question"), QStringLiteral("How does a 1F1B schedule reduce memory, compared with GPipe?"));
    linked.insert(QStringLiteral("replies"), QJsonArray{replyRecord(QStringLiteral("r1"), QStringLiteral("n1"), 1, now, QStringLiteral("What is b exactly?"), 0)});
    QList<Note> notes{note(noteRecord(QStringLiteral("n3"), 3, now.addSecs(-60), QStringLiteral("Note 3"))), note(opened), note(linked)};
    center.apply(notes);

    QStringList large;
    QStringList opens;
    AgentNotesView sidebar(
        AgentNotesView::Layout::Sidebar, &center,
        [](const QString &, const QString &) {
            return AgentNotesView::DocumentLink{QStringLiteral("/library/current_pdfs/long.pdf"),
                                                QStringLiteral("Distributed training of very large language models: a field guide")};
        },
        [&](const QString &documentId, const QString &) { opens.append(documentId); }, {});
    sidebar.setLargeOpener([&](const QString &noteId) { large.append(noteId); });
    QCOMPARE(sidebar.objectName(), QStringLiteral("agentNotesSidebar"));
    // Its title names the reader's Sidebar choice.
    QCOMPARE(sidebar.windowTitle(), QStringLiteral("Agent Notes (2 unread)"));
    sidebar.resize(280, 700);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));

    auto *pages = sidebar.findChild<QStackedWidget *>(QStringLiteral("agentNotesPages"));
    auto *list = sidebar.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
    QVERIFY(pages && list);
    QVERIFY(!sidebar.findChild<QWidget *>(QStringLiteral("agentNotesSplitter")));
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("agentNotesListPage"));
    QVERIFY(sidebar.findChild<QLabel *>(QStringLiteral("agentNotesHeading"))->isVisible());
    QCOMPARE(list->count(), 3);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Note 3"));
    QVERIFY(list->item(0)->data(AgentNotesView::UnreadRole).toBool());
    QVERIFY(!list->item(1)->data(AgentNotesView::UnreadRole).toBool());
    // Nothing opens, and nothing counts as read, until a note is chosen.
    QTest::qWait(50);
    QVERIFY(sidebar.shownNoteId().isEmpty());
    QVERIFY(!list->currentItem());
    QCOMPARE(center.unreadCount(), 2);
    // Narrow enough for a sidebar, its rows as wide as it.
    QVERIFY2(sidebar.minimumSizeHint().width() <= 200, qPrintable(QString::number(sidebar.minimumSizeHint().width())));
    sidebar.resize(200, 700);
    QVERIFY(waitFor([&] { return list->viewport()->width() <= 200; }));
    QVERIFY(list->visualItemRect(list->item(0)).right() < list->viewport()->width());
    sidebar.resize(280, 700);
    QVERIFY(waitFor([&] { return list->viewport()->width() > 200; }));

    // A click shows the note in place of the list, and it counts as read.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("agentNotePage"));
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n1"));
    QCOMPARE(sidebar.findChild<QLabel *>(QStringLiteral("agentNoteTitle"))->text(), QStringLiteral("Note 1"));
    QVERIFY(waitFor([&] { return !center.isUnread(*center.note(QStringLiteral("n1"))); }));
    QCOMPARE(sidebar.windowTitle(), QStringLiteral("Agent Notes (1 unread)"));
    QVERIFY(sidebar.findChild<QLabel *>(QStringLiteral("agentNoteRepliesHeader"))->isVisible());

    // Sized for the sidebar: the picture as wide as the sidebar, and nothing wider than it.
    auto *scroll = sidebar.findChild<QScrollArea *>(QStringLiteral("agentNoteScroll"));
    auto *detail = sidebar.findChild<QWidget *>(QStringLiteral("agentNoteDetail"));
    auto *image = sidebar.findChild<AgentNoteImageView *>(QStringLiteral("agentNoteImage"));
    QVERIFY(waitFor([&] { return image->width() < 280 && image->width() > 180; }));
    QVERIFY2(detail->width() <= scroll->viewport()->width(), qPrintable(QStringLiteral("%1 > %2").arg(detail->width()).arg(scroll->viewport()->width())));
    auto *documentButton = sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteOpenDocument"));
    QVERIFY(documentButton->isVisible());
    QVERIFY(documentButton->text().contains(QChar(0x2026)));
    QVERIFY(documentButton->geometry().right() <= detail->width());
    QVERIFY(sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteOpenCard"))->isVisible());
    documentButton->click();
    QCOMPARE(opens, QStringList{QStringLiteral("doc-1")});

    // Too big for the sidebar: Open Large, or a click on the picture.
    auto *openLarge = sidebar.findChild<QToolButton *>(QStringLiteral("agentNoteOpenLarge"));
    QVERIFY(openLarge && openLarge->isVisible());
    openLarge->click();
    QCOMPARE(large, QStringList{QStringLiteral("n1")});
    QTest::mouseClick(image, Qt::LeftButton);
    QCOMPARE(large, (QStringList{QStringLiteral("n1"), QStringLiteral("n1")}));

    // Back to the list, which kept its place.
    sidebar.findChild<QToolButton *>(QStringLiteral("agentNoteBack"))->click();
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("agentNotesListPage"));
    QVERIFY(sidebar.shownNoteId().isEmpty());
    QCOMPARE(list->currentItem(), list->item(2));
    // A new note arrives: the list shows it, nothing opens.
    notes.prepend(note(noteRecord(QStringLiteral("n4"), 4, now, QStringLiteral("Note 4"))));
    center.apply(notes);
    QCOMPARE(list->count(), 4);
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("agentNotesListPage"));
    QCOMPARE(sidebar.windowTitle(), QStringLiteral("Agent Notes (2 unread)"));

    // Return opens the current note; the note shown keeps up with the server, and a note
    // deleted there takes the sidebar back to the list.
    list->setCurrentRow(0);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Return);
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n4"));
    QJsonObject replied = noteRecord(QStringLiteral("n4"), 4, now, QStringLiteral("Note 4"));
    replied.insert(QStringLiteral("replies"), QJsonArray{replyRecord(QStringLiteral("r9"), QStringLiteral("n4"), 9, now, QStringLiteral("Yes"), 0)});
    notes.replace(0, note(replied));
    center.apply(notes);
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n4"));
    QVERIFY(waitFor([&] {
        const auto replies = sidebar.findChildren<QWidget *>(QStringLiteral("agentNoteReply"));
        return std::count_if(replies.cbegin(), replies.cend(), [](QWidget *reply) { return reply->isVisible(); }) == 1;
    }));
    notes.removeFirst();
    center.apply(notes);
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("agentNotesListPage"));
    QVERIFY(sidebar.shownNoteId().isEmpty());
    QCOMPARE(list->count(), 3);
    // Mark All as Read, above the list.
    auto *markAll = sidebar.findChild<QAbstractButton *>(QStringLiteral("agentNotesMarkAllRead"));
    QVERIFY(markAll && markAll->isVisible() && markAll->isEnabled());
    QVERIFY(center.unreadCount() > 0);
    markAll->click();
    QCOMPARE(center.unreadCount(), 0);
    QCOMPARE(sidebar.windowTitle(), QStringLiteral("Agent Notes"));
    QVERIFY(!markAll->isEnabled());
    QVERIFY(sidebar.findChild<QAbstractButton *>(QStringLiteral("agentNotesRefresh"))->isVisible());
}

// A note asked for before the list came (a notification's): shown in place once listed.
void AgentNotesTest::sidebarShowsARequestedNoteOnceListed()
{
    Client client(cachePath());
    Center center(&client, statePath());
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.resize(300, 600);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    auto *placeholder = sidebar.findChild<QLabel *>(QStringLiteral("agentNotesPlaceholder"));
    QVERIFY(placeholder->isVisible());
    // Not connected under test mode: it says so where the list goes.
    QVERIFY(waitFor([&] { return placeholder->text().contains(QStringLiteral("not connected")); }));
    sidebar.selectNote(QStringLiteral("n2"));
    QVERIFY(sidebar.shownNoteId().isEmpty());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    center.apply({note(noteRecord(QStringLiteral("n3"), 3, now, QStringLiteral("Note 3"))), note(noteRecord(QStringLiteral("n2"), 2, now.addSecs(-60), QStringLiteral("Note 2")))});
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n2"));
    QVERIFY(!placeholder->isVisible());
    QVERIFY(waitFor([&] { return center.unreadCount() == 1; }));
    QVERIFY(center.isUnread(*center.note(QStringLiteral("n3"))));
    // Hidden, nothing more counts as read.
    sidebar.hide();
    sidebar.selectNote(QStringLiteral("n3"));
    QTest::qWait(50);
    QCOMPARE(center.unreadCount(), 1);
    sidebar.show();
    QVERIFY(waitFor([&] { return center.unreadCount() == 0; }));
}

// The window and the sidebars of every tab: polled often while any is on screen.
void AgentNotesTest::centerWatchesWhileAnyViewDoes()
{
    FakeServer server;
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    QObject window;
    QObject sidebar;
    const auto lists = [&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size(); };
    center.setWatching(&window, true);
    QVERIFY(center.isWatching());
    QVERIFY(waitFor([&] { return lists() == 1 && !center.isRefreshing(); }));
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::WatchingLimit));
    // A second view, then the first one hidden: still watching, nothing asked again.
    center.setWatching(&sidebar, true);
    center.setWatching(&window, false);
    QTest::qWait(50);
    QVERIFY(center.isWatching());
    // One view hidden for another in the same turn (another tab's sidebar): the same.
    center.setWatching(&sidebar, false);
    center.setWatching(&window, true);
    QTest::qWait(50);
    QVERIFY(center.isWatching());
    QCOMPARE(lists(), 1);
    // The last one gone: back to the slow pace.
    center.setWatching(&window, false);
    QVERIFY(center.isWatching());
    QVERIFY(waitFor([&] { return !center.isWatching(); }));
    QCOMPARE(lists(), 1);
    // Shown again: asks at once.
    center.setWatching(&sidebar, true);
    QVERIFY(waitFor([&] { return lists() == 2; }));
}

namespace
{
// A drag on the canvas, in its own coordinates, as the mouse makes it.
void drag(QWidget *canvas, const QList<QPointF> &points)
{
    const auto send = [canvas](QEvent::Type type, const QPointF &point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    send(QEvent::MouseButtonPress, points.first(), Qt::LeftButton, Qt::LeftButton);
    for (int index = 1; index < points.size(); ++index) send(QEvent::MouseMove, points.at(index), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, points.last(), Qt::LeftButton, Qt::NoButton);
}

AgentNoteComposer *openComposer()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *composer = qobject_cast<AgentNoteComposer *>(widget); composer && composer->isVisible()) return composer;
    }
    return nullptr;
}
}

// The stroke document and the drawing as the phone makes them (AnnotationDocument.kt).
void AgentNotesTest::drawingMatchesThePhone()
{
    AnnotationDocument document(1000, 2000);
    QCOMPARE(document.penWidth(), 4.0);  // width / 250
    QCOMPARE(AnnotationDocument::penWidthFor(300), 2.0);
    QCOMPARE(document.markerRadius(), 30.0);  // 3 %
    QCOMPARE(AnnotationDocument::markerRadiusFor(200), 14.0);
    QVERIFY(document.addStroke(Stroke::Tool::Pen, {QPointF(10, 10), QPointF(110.04, 10)}));
    QVERIFY(document.addStroke(Stroke::Tool::Highlighter, {QPointF(10, 300), QPointF(500, 300)}));
    QCOMPARE(document.strokes().at(1).width, 16.0);
    QCOMPARE(document.strokes().at(1).opacity, 0.35);
    QCOMPARE(document.addMarker(QPointF(500, 1000))->number, 1);
    QCOMPARE(document.addMarker(QPointF(600, 1000))->number, 2);
    QCOMPARE(QString::fromUtf8(document.toJson()),
             QStringLiteral("{\"schema_version\":1,\"image_width\":1000,\"image_height\":2000,\"source_width\":1000,\"source_height\":2000,\"strokes\":["
                            "{\"tool\":\"pen\",\"color\":\"#000000\",\"opacity\":1.0,\"width\":4.0,\"points\":[[10.0,10.0],[110.0,10.0]]},"
                            "{\"tool\":\"highlighter\",\"color\":\"#000000\",\"opacity\":0.35,\"width\":16.0,\"points\":[[10.0,300.0],[500.0,300.0]]}],"
                            "\"markers\":[{\"x\":500.0,\"y\":1000.0,\"number\":1,\"color\":\"#000000\"},{\"x\":600.0,\"y\":1000.0,\"number\":2,\"color\":\"#000000\"}]}"));
    const auto copy = AnnotationDocument::fromJson(document.toJson());
    QVERIFY(copy);
    QCOMPARE(copy->toJson(), document.toJson());

    // The eraser takes whole strokes and markers; one drag is one undo step.
    const int gesture = document.beginGesture();
    QCOMPARE(document.erase(QPointF(50, 0), QPointF(50, 20), 5), 1);
    QCOMPARE(document.erase(QPointF(500, 980), QPointF(500, 990), 5), 1);
    document.endGesture(gesture);
    QCOMPARE(document.strokes().size(), 1);
    QCOMPARE(document.markers().size(), 1);
    // The next marker counts on from the highest still there.
    QCOMPARE(document.nextMarkerNumber(), 3);
    document.undo();
    QCOMPARE(document.strokes().size(), 2);
    QCOMPARE(document.markers().size(), 2);
    QCOMPARE(document.strokes().first().tool, Stroke::Tool::Pen);
    document.clear();
    QVERIFY(document.isEmpty());
    document.undo();
    QCOMPARE(document.strokes().size(), 2);
    document.redo();
    QVERIFY(document.isEmpty() && !document.canRedo());

    // Drawn onto the picture at its size: pen black, highlighter see-through, markers on top.
    QImage white(1000, 2000, QImage::Format_RGB32);
    white.fill(Qt::white);
    document.undo();
    const QImage drawn = document.composite(white);
    QCOMPARE(drawn.size(), white.size());
    QCOMPARE(drawn.pixelColor(60, 10), QColor(Qt::black));
    const QColor highlighted = drawn.pixelColor(250, 300);
    QVERIFY2(highlighted.lightness() > 120 && highlighted.lightness() < 200, qPrintable(highlighted.name()));
    QCOMPARE(drawn.pixelColor(500 - 20, 1000), QColor(Qt::black));
    QCOMPARE(drawn.pixelColor(900, 1900), QColor(Qt::white));

    // The server's limits hold.
    AnnotationDocument full(100, 100);
    for (int index = 0; index < AnnotationDocument::MaxMarkers; ++index) QVERIFY(full.addMarker(QPointF(50, 50)));
    QVERIFY(!full.addMarker(QPointF(50, 50)));
}

// A typed reply sends the note's picture unchanged, an empty stroke document and the comment,
// as the phone does with no strokes; a failed send keeps everything and a retry is the same reply.
void AgentNotesTest::textReplySendsThePictureAsItIs()
{
    FakeServer server;
    const QByteArray picture = png(QSize(120, 300), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("Question"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return center.hasListed() && window.currentNoteId() == QLatin1String("n1"); }));
    QVERIFY(list(window)->item(0)->data(AgentNotesView::MetaRole).toString().contains(QStringLiteral("not answered yet")));
    auto *field = window.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"));
    auto *send = window.findChild<QPushButton *>(QStringLiteral("agentNoteSend"));
    QVERIFY(field && field->isVisible() && send);
    QVERIFY(!send->isEnabled());
    field->setFocus();
    QTest::keyClicks(field, QStringLiteral("Why the minus sign?"));
    QVERIFY(send->isEnabled());

    server.writeStatus = 503;
    send->click();
    auto *status = window.findChild<QLabel *>(QStringLiteral("agentNoteReplyStatus"));
    QVERIFY(waitFor([&] { return status->text().contains(QStringLiteral("Not sent")); }));
    QCOMPARE(field->toPlainText(), QStringLiteral("Why the minus sign?"));
    QVERIFY(send->isEnabled());
    const QString firstId = server.bodies.last().value(QStringLiteral("reply_id")).toString();
    QVERIFY(!firstId.isEmpty());

    server.writeStatus = 0;
    send->click();
    QVERIFY(waitFor([&] { return field->toPlainText().isEmpty(); }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("reply_id")).toString(), firstId);
    QCOMPARE(body.value(QStringLiteral("message")).toString(), QStringLiteral("Why the minus sign?"));
    const QJsonObject image = body.value(QStringLiteral("image")).toObject();
    QCOMPARE(image.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    QCOMPARE(QByteArray::fromBase64(image.value(QStringLiteral("data_base64")).toString().toLatin1()), picture);
    const QJsonObject strokes = body.value(QStringLiteral("strokes")).toObject();
    QCOMPARE(strokes.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(strokes.value(QStringLiteral("image_width")).toInt(), 120);
    QVERIFY(strokes.value(QStringLiteral("strokes")).toArray().isEmpty());
    QVERIFY(!center.existingDraft(QStringLiteral("n1")));
    // The reply shows under the note at once, and the list says so.
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->replies.size() == 1; }));
    QVERIFY(list(window)->item(0)->data(AgentNotesView::MetaRole).toString().contains(QStringLiteral("1 reply")));
    QVERIFY(waitFor([&] {
        const auto replies = window.findChildren<QLabel *>(QStringLiteral("agentNoteReplyMessage"));
        return std::any_of(replies.cbegin(), replies.cend(), [](QLabel *label) { return label->isVisible() && label->text() == QLatin1String("Why the minus sign?"); });
    }));
    QVERIFY(!send->isEnabled());
}

// Draw on the Picture: the phone's tools on the picture large; Send posts the drawing on the
// picture (PNG), its stroke document and the comment typed under the note.
void AgentNotesTest::drawnReplySendsTheComposite()
{
    FakeServer server;
    const QByteArray picture = png(QSize(400, 600), Qt::white);
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, QDateTime::currentDateTimeUtc(), QStringLiteral("Draw the graph"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    // In the sidebar too: the drawing opens in its own window.
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.resize(300, 800);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    QVERIFY(waitFor([&] { return center.hasListed(); }));
    sidebar.selectNote(QStringLiteral("n1"));
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n1"));
    auto *field = sidebar.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"));
    field->setPlainText(QStringLiteral("1: here"));
    sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteDraw"))->click();
    AgentNoteComposer *composer = nullptr;
    QVERIFY(waitFor([&] { return (composer = openComposer()) != nullptr; }));
    QVERIFY(QTest::qWaitForWindowExposed(composer));
    AgentNoteCanvas *canvas = composer->canvas();
    QVERIFY(waitFor([&] { return !canvas->image().isNull() && canvas->width() > 100; }));
    // The comment typed under the note came along.
    QCOMPARE(composer->findChild<QPlainTextEdit *>(QStringLiteral("agentNoteComposerComment"))->toPlainText(), QStringLiteral("1: here"));
    auto *sendButton = composer->findChild<QPushButton *>(QStringLiteral("agentNoteComposerSend"));
    QVERIFY(sendButton->isEnabled());

    // A pen stroke across the middle, a marker, an extra stroke erased again.
    const qreal toCanvas = canvas->width() / 400.0;
    drag(canvas, {QPointF(50, 300) * toCanvas, QPointF(200, 300) * toCanvas, QPointF(350, 300) * toCanvas});
    QCOMPARE(composer->document()->strokes().size(), 1);
    composer->findChild<QToolButton *>(QStringLiteral("agentNoteNumber"))->click();
    drag(canvas, {QPointF(200, 100) * toCanvas});
    QCOMPARE(composer->document()->markers().size(), 1);
    composer->findChild<QToolButton *>(QStringLiteral("agentNotePen"))->click();
    drag(canvas, {QPointF(50, 500) * toCanvas, QPointF(350, 500) * toCanvas});
    QCOMPARE(composer->document()->strokes().size(), 2);
    composer->findChild<QToolButton *>(QStringLiteral("agentNoteEraser"))->click();
    drag(canvas, {QPointF(200, 480) * toCanvas, QPointF(200, 520) * toCanvas});
    QCOMPARE(composer->document()->strokes().size(), 1);
    composer->findChild<QToolButton *>(QStringLiteral("agentNoteUndo"))->click();
    QCOMPARE(composer->document()->strokes().size(), 2);
    composer->findChild<QToolButton *>(QStringLiteral("agentNoteRedo"))->click();
    QCOMPARE(composer->document()->strokes().size(), 1);
    // The note's reply box knows about the drawing.
    QVERIFY(waitFor([&] { return sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteDraw"))->text().contains(QStringLiteral("Edit")); }));

    QPointer<AgentNoteComposer> guard(composer);
    sendButton->click();
    QVERIFY(waitFor([&] { return !guard || !guard->isVisible(); }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("message")).toString(), QStringLiteral("1: here"));
    const QJsonObject strokes = body.value(QStringLiteral("strokes")).toObject();
    QCOMPARE(strokes.value(QStringLiteral("strokes")).toArray().size(), 1);
    QCOMPARE(strokes.value(QStringLiteral("markers")).toArray().size(), 1);
    QCOMPARE(strokes.value(QStringLiteral("image_width")).toInt(), 400);
    const QJsonObject image = body.value(QStringLiteral("image")).toObject();
    QCOMPARE(image.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    const QImage sent = QImage::fromData(QByteArray::fromBase64(image.value(QStringLiteral("data_base64")).toString().toLatin1()));
    QCOMPARE(sent.size(), QSize(400, 600));
    QCOMPARE(sent.pixelColor(200, 300), QColor(Qt::black));
    QCOMPARE(sent.pixelColor(200, 500), QColor(Qt::white));
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->replies.size() == 1; }));
    QCOMPARE(center.note(QStringLiteral("n1"))->replies.first().strokeCount, 1);
    QVERIFY(field->toPlainText().isEmpty());

    // Drawing open, the reply sent from the box under the note: the drawing went with it, its window closes.
    sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteDraw"))->click();
    QVERIFY(waitFor([&] { return (composer = openComposer()) != nullptr && !composer->canvas()->image().isNull(); }));
    guard = composer;
    QVERIFY(waitFor([&] { return composer->canvas()->width() > 100; }));
    drag(composer->canvas(), {QPointF(50, 100) * toCanvas, QPointF(350, 100) * toCanvas});
    field->setPlainText(QStringLiteral("2: and here"));
    sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteSend"))->click();
    QVERIFY(waitFor([&] { return !guard || !guard->isVisible(); }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("strokes")).toObject().value(QStringLiteral("strokes")).toArray().size(), 1);
    QCOMPARE(server.bodies.last().value(QStringLiteral("message")).toString(), QStringLiteral("2: and here"));
}

// Send Page to Agent: the page on screen, drawn on or as it is, goes as the phone's study mode
// sends it (origin.kind "phone_page"); the agent's page watcher finds it.
void AgentNotesTest::pageGoesToTheAgent()
{
    FakeServer server;
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    auto *sendPage = sidebar.findChild<QAbstractButton *>(QStringLiteral("agentNotesSendPage"));
    QVERIFY(sendPage && sendPage->isHidden());
    int rendered = 0;
    sidebar.setPageProvider([&]() -> std::optional<AgentNotesView::PageCapture> {
        AgentNotesView::PageCapture capture;
        capture.documentName = QStringLiteral("AgentsVsNetwork");
        capture.documentId = QStringLiteral("doc-7");
        capture.documentFilename = QStringLiteral("agents_vs_network.pdf");
        capture.page = 3;
        capture.pages = 12;
        capture.render = [&rendered](int width) {
            ++rendered;
            QImage page(width, width * 3 / 2, QImage::Format_RGB32);
            page.fill(Qt::white);
            return page;
        };
        return capture;
    });
    sidebar.resize(300, 800);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    QVERIFY(sendPage->isVisible());
    sendPage->click();
    QCOMPARE(rendered, 1);
    AgentNoteComposer *composer = nullptr;
    QVERIFY(waitFor([&] { return (composer = openComposer()) != nullptr; }));
    QVERIFY(composer->isPage());
    QCOMPARE(composer->findChild<QLineEdit *>(QStringLiteral("agentNoteComposerTitle"))->text(), QStringLiteral("AgentsVsNetwork · page 3"));
    QCOMPARE(composer->canvas()->image().width(), 1600);
    QVERIFY(QTest::qWaitForWindowExposed(composer));
    QVERIFY(waitFor([&] { return composer->canvas()->width() > 100; }));
    const qreal toCanvas = composer->canvas()->width() / 1600.0;
    drag(composer->canvas(), {QPointF(100, 100) * toCanvas, QPointF(900, 100) * toCanvas});
    composer->findChild<QPlainTextEdit *>(QStringLiteral("agentNoteComposerComment"))->setPlainText(QStringLiteral("Is eq. 3 right?"));
    QPointer<AgentNoteComposer> guard(composer);
    composer->findChild<QPushButton *>(QStringLiteral("agentNoteComposerSend"))->click();
    QVERIFY(waitFor([&] { return !guard || !guard->isVisible(); }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("title")).toString(), QStringLiteral("AgentsVsNetwork · page 3"));
    QCOMPARE(body.value(QStringLiteral("message")).toString(), QStringLiteral("Is eq. 3 right?"));
    QCOMPARE(body.value(QStringLiteral("document_id")).toString(), QStringLiteral("doc-7"));
    QCOMPARE(body.value(QStringLiteral("document_filename")).toString(), QStringLiteral("agents_vs_network.pdf"));
    QCOMPARE(body.value(QStringLiteral("origin")).toObject(),
             (QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}, {QStringLiteral("device"), QStringLiteral("mac")},
                          {QStringLiteral("page"), 3}, {QStringLiteral("pages"), 12}, {QStringLiteral("document_name"), QStringLiteral("AgentsVsNetwork")},
                          {QStringLiteral("stroke_count"), 1}}));
    const QImage sent = QImage::fromData(QByteArray::fromBase64(body.value(QStringLiteral("image")).toObject().value(QStringLiteral("data_base64")).toString().toLatin1()));
    QCOMPARE(sent.width(), 1600);
    QCOMPARE(sent.pixelColor(500, 100), QColor(Qt::black));
    QVERIFY(!body.value(QStringLiteral("note_id")).toString().isEmpty());
    // Listed as the user's own page from this Mac: no reply box, never unread.
    QVERIFY(waitFor([&] { return center.notes().size() == 1; }));
    const Note page = center.notes().first();
    QVERIFY(page.isPhonePage());
    QCOMPARE(page.senderLabel(), QStringLiteral("You, from this Mac"));
    QCOMPARE(center.unreadCount(), 0);
    sidebar.selectNote(page.id);
    QVERIFY(!sidebar.findChild<QWidget *>(QStringLiteral("agentNoteReplyBox"))->isVisible());
}

// Delete Note…: asks first, then deletes it for every device.
void AgentNotesTest::deleteAsksFirst()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes = QJsonArray{noteRecord(QStringLiteral("n2"), 2, now, QStringLiteral("Keep")), noteRecord(QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("Drop"))};
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.show();
    QVERIFY(waitFor([&] { return center.notes().size() == 2; }));
    QStringList asked;
    bool answer = false;
    sidebar.m_confirmDelete = [&](const QString &title) {
        asked.append(title);
        return answer;
    };
    sidebar.deleteNote(QStringLiteral("n1"));
    QCOMPARE(asked, QStringList{QStringLiteral("Drop")});
    QTest::qWait(50);
    QVERIFY(server.requests.filter(QStringLiteral("DELETE")).isEmpty());
    answer = true;
    sidebar.deleteNote(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return center.notes().size() == 1; }));
    QCOMPARE(server.requests.filter(QStringLiteral("DELETE")), QStringList{QStringLiteral("DELETE /v1/agent-notes/n1")});
    QCOMPARE(list(sidebar)->count(), 1);
    QCOMPARE(list(sidebar)->item(0)->text(), QStringLiteral("Keep"));
}

namespace
{
const QString D1 = QStringLiteral("01e71f15-6c88-442b-aa0f-e9d8734b2f12");
const QString D2 = QStringLiteral("2b9f2c11-0a43-4a8e-9a47-4f3e1d7c2a10");
const QString D3 = QStringLiteral("8489a61a-4f2b-4d1f-8a24-d2f01dbb9ba9");

// A project catalogue as .thoughts/library-projects.json has it.
QJsonObject catalogue()
{
    const auto document = [](const QString &id, const QString &filename, const QString &name) {
        return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("filename"), filename}, {QStringLiteral("name"), name}};
    };
    return {{QStringLiteral("projects"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("new:a63fa59e")},
                                    {QStringLiteral("name"), QStringLiteral("MethodsComparison")},
                                    {QStringLiteral("documents"), QJsonArray{document(D1, QStringLiteral("import-5ebe_main.pdf"), QStringLiteral("main")),
                                                                             document(D2, QStringLiteral("import-5ebe_notes.pdf"), QStringLiteral("notes"))}}},
                        QJsonObject{{QStringLiteral("id"), QStringLiteral("new:77b1")},
                                    {QStringLiteral("name"), QStringLiteral("Reading group")},
                                    {QStringLiteral("documents"), QJsonArray{document(D3, QStringLiteral("reading_group.pdf"), QStringLiteral("main"))}}}}}};
}

QJsonObject noteOf(const QString &id, qint64 sequence, const QString &documentId)
{
    QJsonObject record = noteRecord(id, sequence, QDateTime::currentDateTimeUtc().addSecs(-60 * sequence), QStringLiteral("Note %1").arg(id));
    record.insert(QStringLiteral("document_id"), documentId.isEmpty() ? QJsonValue() : QJsonValue(documentId));
    return record;
}

QStringList filterTexts(QComboBox *box)
{
    QStringList texts;
    for (int index = 0; index < box->count(); ++index) texts.append(box->itemText(index));
    return texts;
}
}

// A project by its id or name, a document by its id, filename or (unique) name: the notes of
// those Library documents.
void AgentNotesTest::filtersFollowTheLibrary()
{
    const QList<LibraryDocument> library = libraryDocuments(catalogue());
    QCOMPARE(library.size(), 3);
    QCOMPARE(library.first().projectName, QStringLiteral("MethodsComparison"));

    for (const QString &asked : {QStringLiteral("new:a63fa59e"), QStringLiteral("methodscomparison"), QStringLiteral(" MethodsComparison ")}) {
        const auto project = Filter::project(library, asked);
        QVERIFY2(project, qPrintable(asked));
        QCOMPARE(project->kind, QStringLiteral("project"));
        QCOMPARE(project->key, QStringLiteral("new:a63fa59e"));
        QCOMPARE(project->label, QStringLiteral("MethodsComparison"));
        QCOMPARE(project->documentIds, (QStringList{D1, D2}));
    }
    QVERIFY(!Filter::project(library, QStringLiteral("Off")));
    QVERIFY(!Filter::project(library, QString()));

    QCOMPARE(Filter::document(library, D2)->label, QStringLiteral("MethodsComparison › notes"));
    QCOMPARE(Filter::document(library, QStringLiteral("import-5ebe_main.pdf"))->documentIds, QStringList{D1});
    // The only document of its project: the project's filter.
    QCOMPARE(Filter::document(library, D3)->label, QStringLiteral("Reading group"));
    QCOMPARE(Filter::document(library, D3)->kind, QStringLiteral("project"));
    QCOMPARE(Filter::document(library, QStringLiteral("NOTES"))->key, D2);
    // Two documents are called "main": a name alone does not say which.
    QVERIFY(!Filter::document(library, QStringLiteral("main")));

    const Filter project = *Filter::project(library, QStringLiteral("MethodsComparison"));
    QVERIFY(project.matches(note(noteOf(QStringLiteral("a"), 1, D2))));
    QVERIFY(!project.matches(note(noteOf(QStringLiteral("b"), 2, D3))));
    QVERIFY(!project.matches(note(noteOf(QStringLiteral("c"), 3, QString()))));
    QJsonObject byFilename = noteOf(QStringLiteral("d"), 4, QString());
    byFilename.insert(QStringLiteral("document_filename"), QStringLiteral("import-5ebe_main.pdf"));
    QVERIFY(project.matches(note(byFilename)));
    QVERIFY(Filter().matches(note(noteOf(QStringLiteral("e"), 5, QString()))));
}

// Show: a project's notes only, its documents one by one, and the notes older than the newest
// page (the server's document_id list) as well; All Notes again shows every note.
void AgentNotesTest::viewShowsOneProjectWithItsOlderNotes()
{
    FakeServer server;
    server.notes = QJsonArray{noteOf(QStringLiteral("n4"), 4, D3), noteOf(QStringLiteral("n3"), 3, D1), noteOf(QStringLiteral("n2"), 2, D2),
                              noteOf(QStringLiteral("n1"), 1, QString())};
    server.olderNotes = QJsonArray{noteOf(QStringLiteral("old"), 0, D1)};
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesWindow window(&center, {}, {}, {});
    window.setLibraryProvider([] { return libraryDocuments(catalogue()); });
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return center.hasListed() && !center.isRefreshing(); }));
    auto *box = window.findChild<QComboBox *>(QStringLiteral("agentNotesFilter"));
    QVERIFY(box && box->isVisible());
    QCOMPARE(filterTexts(box), (QStringList{QStringLiteral("All Notes (4)"), QStringLiteral("MethodsComparison (2)"), QStringLiteral("    main (1)"),
                                            QStringLiteral("    notes (1)"), QStringLiteral("Reading group (1)")}));
    QCOMPARE(list(window)->count(), 4);

    // The user picks the project.
    box->setCurrentIndex(1);
    Q_EMIT box->activated(1);
    QCOMPARE(window.filter().key, QStringLiteral("new:a63fa59e"));
    QCOMPARE(list(window)->count(), 2);
    QCOMPARE(window.currentNoteId(), QStringLiteral("n3"));
    // Its documents' older notes are asked for and come in.
    QVERIFY(waitFor([&] { return list(window)->count() == 3; }));
    const QStringList asked = server.requests.filter(QStringLiteral("document_id="));
    QCOMPARE(asked.size(), 2);
    QVERIFY(asked.join(QLatin1Char(' ')).contains(D1) && asked.join(QLatin1Char(' ')).contains(D2));
    QCOMPARE(list(window)->item(2)->data(AgentNotesView::NoteIdRole).toString(), QStringLiteral("old"));
    QCOMPARE(box->currentText(), QStringLiteral("MethodsComparison (3)"));
    // The next newest page keeps them (older notes stay); one document; then all again.
    window.findChild<QPushButton *>(QStringLiteral("agentNotesRefresh"))->click();
    QVERIFY(waitFor([&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size() >= 2 && !center.isRefreshing(); }));
    QCOMPARE(list(window)->count(), 3);
    const int notes = filterTexts(box).indexOf(QStringLiteral("    notes (1)"));
    box->setCurrentIndex(notes);
    Q_EMIT box->activated(notes);
    QCOMPARE(list(window)->count(), 1);
    QCOMPARE(window.currentNoteId(), QStringLiteral("n2"));
    box->setCurrentIndex(0);
    Q_EMIT box->activated(0);
    QVERIFY(window.filter().isEmpty());
    QCOMPARE(list(window)->count(), 5);

    // Set from outside (an agent's open-agent-notes): the menu follows, an empty project too.
    window.setFilter(*Filter::document(libraryDocuments(catalogue()), D3));
    QCOMPARE(box->currentText(), QStringLiteral("Reading group (1)"));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n4"));
    QJsonObject emptyCatalogue = catalogue();
    QJsonArray projects = emptyCatalogue.value(QStringLiteral("projects")).toArray();
    projects.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("new:empty")}, {QStringLiteral("name"), QStringLiteral("Quiet")},
                                {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), QStringLiteral("5e0b3a77-1a2b-4c3d-8e9f-0a1b2c3d4e5f")}}}}});
    emptyCatalogue.insert(QStringLiteral("projects"), projects);
    window.setFilter(*Filter::project(libraryDocuments(emptyCatalogue), QStringLiteral("Quiet")));
    QCOMPARE(list(window)->count(), 0);
    QCOMPARE(box->currentText(), QStringLiteral("Quiet (0)"));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("agentNotesPlaceholder"))->text().contains(QStringLiteral("Quiet")));

    // The sidebar has the same menu.
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.setLibraryProvider([] { return libraryDocuments(catalogue()); });
    auto *sidebarBox = sidebar.findChild<QComboBox *>(QStringLiteral("agentNotesFilter"));
    QVERIFY(sidebarBox);
    QCOMPARE(sidebarBox->itemText(1), QStringLiteral("MethodsComparison (3)"));
}

void AgentNotesTest::viewWarnsWhileNoAgentListens()
{
    FakeServer server;
    const QByteArray picture = png(QSize(200, 500), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonObject agentNote = noteRecord(QStringLiteral("n1"), 7, now.addSecs(-300), QStringLiteral("New drafter"), picture);
    QJsonObject page = noteRecord(QStringLiteral("p1"), 6, now.addSecs(-400), QStringLiteral("Notes · page 2"), picture);
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}});
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/p1/image"), picture);
    server.notes = {agentNote, page};
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("not_listening"));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.apply({note(agentNote), note(page)});

    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.showNote(QStringLiteral("n1"));
    auto *warning = window.findChild<QLabel *>(QStringLiteral("agentNoteNotListening"));
    QVERIFY(warning);
    QVERIFY(waitFor([&] { return warning->isVisible(); }));
    QVERIFY(warning->text().contains(QStringLiteral("No agent is listening")));
    QCOMPARE(center.listening(QStringLiteral("n1")), Listening::NotListening);

    // The agent starts listening: the next check takes the warning away.
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("listening"));
    center.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return !warning->isVisible(); }));

    // The answer to a reply says whether it was heard.
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("not_listening"));
    center.setDraftMessage(QStringLiteral("n1"), QStringLiteral("Give me a new version"));
    bool sent = false;
    center.sendReply(QStringLiteral("n1"), [&](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QCOMPARE(center.listening(QStringLiteral("n1")), Listening::NotListening);
    QVERIFY(waitFor([&] { return warning->isVisible(); }));

    // A page the user sent is not asked about; a server that cannot tell (404) shows nothing.
    const int asked = server.requests.filter(QStringLiteral("/p1/listening")).size();
    window.showNote(QStringLiteral("p1"));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("p1"); }));
    QTest::qWait(200);
    QCOMPARE(server.requests.filter(QStringLiteral("/p1/listening")).size(), asked);
    QVERIFY(!warning->isVisible());
    server.listening.remove(QStringLiteral("n1"));
    center.setListening(QStringLiteral("n1"), Listening::NotListening);
    window.showNote(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("n1"); }));
    QVERIFY(waitFor([&] { return center.listening(QStringLiteral("n1")) == Listening::Unknown; }));
    QVERIFY(waitFor([&] { return !warning->isVisible(); }));
}

void AgentNotesTest::replyPhotoIsShownWithTheReply()
{
    FakeServer server;
    const QByteArray picture = png(QSize(200, 500), QColor(30, 90, 200));
    const QByteArray photo = png(QSize(320, 240), QColor(10, 160, 60));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("n1"), 7, now.addSecs(-300), QStringLiteral("New drafter"), picture);
    QJsonObject reply = replyRecord(QStringLiteral("r1"), QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("Give me a new version based on this"), 0);
    reply.insert(QStringLiteral("photo"), QJsonObject{{QStringLiteral("url"), QStringLiteral("/v1/agent-notes/replies/r1/photo")},
                                                      {QStringLiteral("media_type"), QStringLiteral("image/png")},
                                                      {QStringLiteral("width"), 320},
                                                      {QStringLiteral("height"), 240},
                                                      {QStringLiteral("bytes"), photo.size()},
                                                      {QStringLiteral("sha256"), sha(photo)},
                                                      {QStringLiteral("capture"), QStringLiteral("IMG_20261111_170138_123_ab12")}});
    record.insert(QStringLiteral("replies"), QJsonArray{reply});
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/replies/r1/photo"), photo);
    server.notes.append(record);

    const Note parsed = note(record);
    QCOMPARE(parsed.replies.size(), 1);
    QVERIFY(parsed.replies.first().hasPhoto());
    QCOMPARE(parsed.replies.first().photoCapture, QStringLiteral("IMG_20261111_170138_123_ab12"));
    QCOMPARE(parsed.replies.first().photoWidth, 320);

    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.apply({parsed});
    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.showNote(QStringLiteral("n1"));
    // A refresh rebuilds the replies: look the photo up each time.
    const auto shownPhoto = [&]() -> AgentNoteImageView * {
        for (AgentNoteImageView *view : window.findChildren<AgentNoteImageView *>(QStringLiteral("agentNoteReplyPhoto"))) {
            if (view->isVisibleTo(&window) && !view->image().isNull()) return view;
        }
        return nullptr;
    };
    QVERIFY2(waitFor([&] { return shownPhoto() != nullptr; }), qPrintable(server.requests.join(QStringLiteral(", "))));
    AgentNoteImageView *shown = shownPhoto();
    QCOMPARE(shown->image().size(), QSize(320, 240));
    QVERIFY(shown->toolTip().contains(QStringLiteral("IMG_20261111_170138_123_ab12")));
    const QString meta = window.findChild<QLabel *>(QStringLiteral("agentNoteReplyMeta"))->text();
    QVERIFY2(meta.contains(QStringLiteral("with a photo from the glasses")), qPrintable(meta));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/replies/r1/photo")));
}

QTEST_MAIN(AgentNotesTest)
#include "agentnotestest.moc"
