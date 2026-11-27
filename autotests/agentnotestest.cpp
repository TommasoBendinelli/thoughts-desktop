// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/agentnotes.h"
#include "../shell/agentnotecomposer.h"
#include "../shell/agentnoteswindow.h"
#include "../part/thoughtsplatform.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QComboBox>
#include <tuple>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QPointer>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>
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

// An agent as the server names it (protocol/agent-notes.md, "The user's choice").
QJsonObject agentRecord(const QString &role, const QString &device, const QString &conversation = {}, const QString &owner = {}, const QString &label = {}, bool live = true)
{
    return {{QStringLiteral("role"), role},
            {QStringLiteral("device_id"), device},
            {QStringLiteral("conversation_id"), conversation.isEmpty() ? QJsonValue() : QJsonValue(conversation)},
            {QStringLiteral("owner_id"), owner.isEmpty() ? QJsonValue() : QJsonValue(owner)},
            {QStringLiteral("label"), label.isEmpty() ? QJsonValue() : QJsonValue(label)},
            {QStringLiteral("live"), live}};
}

const QString ConversationA = QStringLiteral("0190a000-0000-7000-8000-00000000000a");
const QString ConversationB = QStringLiteral("0190a000-0000-7000-8000-00000000000b");
const QString TopicOwner = QStringLiteral("0190a000-0000-7000-8000-00000000000c");

QJsonObject replyAgentRecord(bool live = true)
{
    return agentRecord(QStringLiteral("responder"), QStringLiteral("example-reply-host"), {}, {}, {}, live);
}

QJsonObject topicAgentRecord(bool live = true)
{
    return agentRecord(QStringLiteral("owner"), QStringLiteral("example-laptop"), {}, TopicOwner, QStringLiteral("Example topic"), live);
}

// The session a reply waker would wake (a listening answer's "wake"), as the server sends it.
QJsonObject wakeRecord(const QString &state, const QString &role, const QString &machine, const QString &title, const QString &sessionState = QStringLiteral("closed"),
                       const QString &fallbackAt = {})
{
    return {{QStringLiteral("state"), state},
            {QStringLiteral("role"), role},
            {QStringLiteral("device_id"), QStringLiteral("example-laptop")},
            {QStringLiteral("machine"), machine},
            {QStringLiteral("conversation_id"), ConversationA},
            {QStringLiteral("title"), title.isEmpty() ? QJsonValue() : QJsonValue(title)},
            {QStringLiteral("session_state"), sessionState},
            {QStringLiteral("owner_id"), QJsonValue()},
            {QStringLiteral("label"), QJsonValue()},
            {QStringLiteral("waker_live"), state == QLatin1String("will_be_woken")},
            {QStringLiteral("fallback_at"), fallbackAt.isEmpty() ? QJsonValue() : QJsonValue(fallbackAt)}};
}

// The identity of an agent, as PUT …/answer-by names it.
QJsonObject choiceOf(const QJsonObject &agent)
{
    return {{QStringLiteral("role"), agent.value(QStringLiteral("role"))},
            {QStringLiteral("device_id"), agent.value(QStringLiteral("device_id"))},
            {QStringLiteral("conversation_id"), agent.value(QStringLiteral("conversation_id"))},
            {QStringLiteral("owner_id"), agent.value(QStringLiteral("owner_id"))}};
}

QStringList actionTitles(const QMenu *menu)
{
    QStringList titles;
    for (const QAction *action : menu->actions()) {
        if (!action->isSeparator()) titles.append(action->text());
    }
    return titles;
}

QAction *actionTitled(const QMenu *menu, const QString &title)
{
    for (QAction *action : menu->actions()) {
        if (action->text() == title) return action;
    }
    return nullptr;
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
    QJsonArray markNotes;  // in no list: GET /v1/agent-notes/{id} only (reading overlay mark notes)
    // GET /v1/agent-notes/wakers; off: 404, as a server before agent_note_wakers_v1.
    QJsonObject wakers{{QStringLiteral("wakers"), QJsonArray()}};
    bool wakersSupported = true;
    QHash<QString, QByteArray> files;  // path -> body
    QStringList requests;  // "GET /v1/agent-notes?limit=20"
    QList<QJsonObject> bodies;  // of the POSTs, in order
    QByteArray token = "secret";
    int listStatus = 200;
    // Set: the list carries an ETag and an unchanged list is answered 304, as a server with
    // conditional lists does. notModified counts those answers; listBytes what the lists weighed.
    bool listVersions = false;
    int notModified = 0;
    qint64 listBytes = 0;
    int writeStatus = 0;  // set: every POST and DELETE answers it
    // note id -> "listening" / "not_listening" (GET …/listening, and the answer to a reply); a note
    // without an entry answers 404, as a server before agent_note_listening_v1.
    QHash<QString, QString> listening;
    // note id -> the whole answer of GET …/listening (state, listeners, answer_by, candidates), as
    // a server with agent_note_answer_by_v1 gives it; it goes before `listening`. A PUT
    // …/answer-by changes it: a candidate (by its identity) or null, else 409.
    QHash<QString, QJsonObject> answers;
    // note id -> the state while nobody was chosen (the answer to {"agent": null}).
    QHash<QString, QString> automaticState;
    int putStatus = 0;  // set: every PUT answers it
    // GET /v1/health: {"status": "ok", "capabilities": [...]}.
    QStringList capabilities;
    // item id -> GET /v1/agent-notes/handling/{item_id}'s answer (who answers it); else 404.
    QHash<QString, QJsonObject> handling;
    // This computer's device account, so pages it sends are its own on every system.
    QString device = Thoughts::Platform::desktopDeviceAccount();
    // GET /v1/agent-notes/changes: its cursor; a request with that cursor as `after` is held until
    // change() (as the server waits). Off: 404, as a server before agent_note_changes_v1.
    bool changesSupported = true;
    QString changesCursor = QStringLiteral("1.0");
    int waitingChanges() const { return int(m_waiting.size()); }
    void change(const QString &cursor)
    {
        changesCursor = cursor;
        const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("cursor"), cursor}, {QStringLiteral("changed"), true}}).toJson(QJsonDocument::Compact);
        for (QTcpSocket *socket : std::exchange(m_waiting, {})) {
            if (!socket) continue;
            socket->write("HTTP/1.1 200 X\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        }
    }

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
    QList<QPointer<QTcpSocket>> m_waiting;
    qint64 m_replySequence = 100;

    void answer(QTcpSocket *socket, const QByteArray &request, const QByteArray &requestBody)
    {
        const QList<QByteArray> words = request.left(request.indexOf("\r\n")).split(' ');
        const QString method = QString::fromLatin1(words.value(0));
        const QUrl target(QString::fromLatin1(words.value(1)));
        requests.append(method + QLatin1Char(' ') + target.toString());
        int code = 404;
        QByteArray body;
        QByteArray extraHeaders;
        QByteArray type = "application/json";
        if (!request.contains(QByteArray(QByteArray("Authorization: Bearer ") + token))) {
            code = 401;
            body = R"({"detail":"invalid token"})";
        } else if (method == QLatin1String("GET") && target.path() == QLatin1String("/v1/agent-notes/changes") && changesSupported) {
            const QString after = QUrlQuery(target).queryItemValue(QStringLiteral("after"));
            if (!after.isEmpty() && after == changesCursor) {
                m_waiting.append(socket);  // answered by change()
                return;
            }
            code = 200;
            body = QJsonDocument(QJsonObject{{QStringLiteral("cursor"), changesCursor}, {QStringLiteral("changed"), !after.isEmpty()}}).toJson(QJsonDocument::Compact);
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
            if (listVersions && code == 200) {
                const QByteArray version = QByteArray("\"") + QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() + QByteArray("\"");
                const QByteArray condition = QByteArray("If-None-Match: ") + version + QByteArray("\r\n");
                if (request.contains(condition)) {
                    code = 304;
                    body.clear();
                    ++notModified;
                }
                extraHeaders = QByteArray("ETag: ") + version + QByteArray("\r\n");
            }
            listBytes += body.size();
        } else if (method == QLatin1String("GET") && target.path() == QLatin1String("/v1/agent-notes/wakers")) {
            if (wakersSupported) {
                code = 200;
                body = QJsonDocument(wakers).toJson(QJsonDocument::Compact);
            }
        } else if (method == QLatin1String("GET") && target.path().startsWith(QLatin1String("/v1/agent-notes/")) && target.path().count(QLatin1Char('/')) == 3
                   && !files.contains(target.path())) {
            // One note by its id, listed or not.
            const QString id = target.path().section(QLatin1Char('/'), -1);
            body = R"({"detail":"note not found","error_code":"agent_note_not_found"})";
            for (const QJsonArray *source : {&notes, &markNotes}) {
                for (const QJsonValue &value : *source) {
                    if (value.toObject().value(QStringLiteral("note_id")).toString() != id) continue;
                    code = 200;
                    body = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
                }
            }
        } else if (method == QLatin1String("GET") && target.path().startsWith(QLatin1String("/v1/agent-notes/handling/"))) {
            const QString id = target.path().section(QLatin1Char('/'), -1);
            code = handling.contains(id) ? 200 : 404;
            body = QJsonDocument(handling.value(id)).toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("GET") && target.path() == QLatin1String("/v1/health")) {
            code = 200;
            body = QJsonDocument(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities)}})
                       .toJson(QJsonDocument::Compact);
        } else if (method == QLatin1String("GET") && target.path().endsWith(QLatin1String("/listening"))) {
            const QString id = target.path().section(QLatin1Char('/'), -2, -2);
            if (answers.contains(id)) {
                code = 200;
                body = QJsonDocument(answers.value(id)).toJson(QJsonDocument::Compact);
            } else if (listening.contains(id)) {
                code = 200;
                body = QJsonDocument(QJsonObject{{QStringLiteral("note_id"), id}, {QStringLiteral("state"), listening.value(id)},
                                                 {QStringLiteral("listeners"), QJsonArray()}})
                           .toJson(QJsonDocument::Compact);
            }
        } else if (method == QLatin1String("GET") && files.contains(target.path())) {
            code = 200;
            body = files.value(target.path());
            type = "image/png";
        } else if (method == QLatin1String("PUT")) {
            const QJsonObject payload = QJsonDocument::fromJson(requestBody).object();
            bodies.append(payload);
            const QStringList parts = target.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);  // v1 agent-notes <id> answer-by
            const QString id = parts.value(2);
            if (putStatus) {
                code = putStatus;
                body = putStatus == 409 ? QByteArray(R"({"detail":"unknown agent","error_code":"agent_note_answerer_unknown"})")
                                        : QByteArray(R"({"detail":"try later","error_code":"busy"})");
            } else if (parts.size() == 4 && parts.at(3) == QLatin1String("answer-by") && answers.contains(id)) {
                QJsonObject answer = answers.value(id);
                const QJsonValue chosen = payload.value(QStringLiteral("agent"));
                const auto same = [](const QJsonObject &a, const QJsonObject &b) {
                    for (const QString &key : {QStringLiteral("role"), QStringLiteral("device_id"), QStringLiteral("conversation_id"), QStringLiteral("owner_id")}) {
                        if (a.value(key).toString() != b.value(key).toString()) return false;
                    }
                    return true;
                };
                QJsonObject match;
                if (chosen.isObject()) {
                    for (const QJsonValue &candidate : answer.value(QStringLiteral("candidates")).toArray()) {
                        if (same(candidate.toObject(), chosen.toObject())) match = candidate.toObject();
                    }
                    if (match.isEmpty() && same(answer.value(QStringLiteral("answer_by")).toObject(), chosen.toObject())) match = answer.value(QStringLiteral("answer_by")).toObject();
                }
                if (chosen.isObject() && match.isEmpty()) {
                    code = 409;
                    body = R"({"detail":"unknown agent","error_code":"agent_note_answerer_unknown"})";
                } else {
                    answer.insert(QStringLiteral("answer_by"), match.isEmpty() ? QJsonValue() : QJsonValue(match));
                    answer.insert(QStringLiteral("state"), match.isEmpty() ? automaticState.value(id, QStringLiteral("not_listening"))
                                                                           : match.value(QStringLiteral("live")).toBool() ? QStringLiteral("listening") : QStringLiteral("not_listening"));
                    answers.insert(id, answer);
                    code = 200;
                    body = QJsonDocument(answer).toJson(QJsonDocument::Compact);
                }
            } else {
                body = R"({"detail":"note not found","error_code":"agent_note_not_found"})";
            }
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
                        if (payload.contains(QStringLiteral("sources"))) reply.insert(QStringLiteral("sources"), payload.value(QStringLiteral("sources")));
                        files.insert(reply.value(QStringLiteral("image_url")).toString(), image);
                        replies.append(reply);
                        m_scratch.insert(QStringLiteral("replies"), replies);
                        storeScratch();
                    }
                    QJsonObject answer = reply;
                    if (answers.contains(parts.at(2))) {
                        const QJsonObject known = answers.value(parts.at(2));
                        QJsonObject heard{{QStringLiteral("state"), known.value(QStringLiteral("state"))},
                                          {QStringLiteral("listeners"), known.value(QStringLiteral("listeners"))}};
                        if (known.contains(QStringLiteral("wake"))) heard.insert(QStringLiteral("wake"), known.value(QStringLiteral("wake")));
                        answer.insert(QStringLiteral("listening"), heard);
                    } else if (listening.contains(parts.at(2))) {
                        answer.insert(QStringLiteral("listening"), QJsonObject{{QStringLiteral("state"), listening.value(parts.at(2))}});
                    }
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
                      + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + body);
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
    void aNewNoteShowsAtOnceWhileTheServerOffersChanges();
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
    void listComesBackEmptyWhenTheServerSaysItIsUnchanged();
    void centerSlowsDownWhileNothingChanges();
    void centerPollsSlowlyBehindAnotherApplication();
    void drawingMatchesThePhone();
    void textReplySendsThePictureAsItIs();
    void replyCarriesWhatTheAgentMayUse();
    void continuedConversationIsTheSendersOwn();
    void notesListHidesLikeTheReadersSidebar();
    void replyToTheSendingConversationSaysSoInsteadOfSources();
    void noteLinksOpenWhenClicked();
    void drawnReplySendsTheComposite();
    void highlightOnTheNoteItself();
    void pageGoesToTheAgent();
    void markNotesAreNeverListed();
    void markNoteOpensByItsId();
    void askedPageNamesItsSession();
    void sendToAgentRecipients();
    void sendToAgentSessionMenu();
    void sendToAgentRanking();
    void sendToAgentRankedMenu();
    void copyForAgentMessageGoesAsItIs();
    void deleteAsksFirst();
    void filtersFollowTheLibrary();
    void viewShowsOneProjectWithItsOlderNotes();
    void viewWarnsWhileNoAgentListens();
    void answerLineSaysWhoListens();
    void clientChoosesWhoAnswers();
    void centerAsksWhoListensOncePerMinute();
    void viewLetsTheUserChooseWhoAnswers();
    void answerLineSaysWhoWillBeWoken();
    void viewSaysWhoWillBeWoken();
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

void AgentNotesTest::aNewNoteShowsAtOnceWhileTheServerOffersChanges()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-120), QStringLiteral("First")));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    QSignalSpy attention(&center, &Center::noteNeedsAttention);
    center.start(0);
    // The first list, then a request that waits for the next change.
    QVERIFY(waitFor([&] { return center.hasListed() && center.isLive() && server.waitingChanges() == 1; }));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/changes")));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/changes?after=1.0&wait=%1").arg(Center::ChangesWaitSeconds)));
    // A note arrives: the list is asked for at once, not at the next timed poll (15 s to 2 min away).
    server.notes.prepend(noteRecord(QStringLiteral("n2"), 2, QDateTime::currentDateTimeUtc(), QStringLiteral("Second")));
    server.change(QStringLiteral("2.0"));
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n2")).has_value(); }, 3000));
    QVERIFY(waitFor([&] { return server.waitingChanges() == 1; }));
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/changes?after=2.0&wait=%1").arg(Center::ChangesWaitSeconds)));
    // Nobody opened it: announced once its grace period is over, with no list asked for meanwhile.
    QCOMPARE(attention.count(), 0);
    QVERIFY(waitFor([&] { return attention.count() == 1; }, (Center::GracePeriodSeconds + 3) * 1000));
    QCOMPARE(attention.first().first().value<Note>().id, QStringLiteral("n2"));
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
    // Said the same way on every system.
    QCOMPARE(result->error, QStringLiteral("The Thoughts server refused this computer's device token."));

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

// A server that names a version of its list (an ETag) is told the version this Mac has, and
// answers an unchanged list without its notes. A server that names none is asked as before.
void AgentNotesTest::listComesBackEmptyWhenTheServerSaysItIsUnchanged()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int index = 0; index < 20; ++index) {
        QJsonObject record = noteRecord(QStringLiteral("n%1").arg(index), 100 - index, now.addSecs(-3600 - index), QStringLiteral("Note %1").arg(index));
        // As long as a real note's message.
        record.insert(QStringLiteral("message"), QString(2000, QLatin1Char('x')));
        server.notes.append(record);
    }
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    QList<Client::Result> results;
    const auto list = [&](int limit) {
        const qsizetype before = results.size();
        client.list(limit, &client, [&results](const Client::Result &result) { results.append(result); });
        return waitFor([&] { return results.size() == before + 1; });
    };

    // No versions: every answer carries the notes.
    QVERIFY(list(20));
    QVERIFY(list(20));
    QCOMPARE(results.at(1).notes.size(), 20);
    QVERIFY(!results.at(1).notModified);
    QCOMPARE(server.notModified, 0);
    const qint64 fullListBytes = server.listBytes / 2;
    QVERIFY(fullListBytes > 20 * 2000);

    server.listVersions = true;
    server.listBytes = 0;
    QVERIFY(list(20));
    QCOMPARE(results.last().notes.size(), 20);
    QVERIFY(!results.last().notModified);
    QCOMPARE(server.listBytes, fullListBytes);
    // The same list again: no notes, no failure, nothing to read.
    QVERIFY(list(20));
    QVERIFY(results.last().notModified);
    QVERIFY(results.last().notes.isEmpty());
    QCOMPARE(results.last().failure, Client::Failure::None);
    QCOMPARE(server.notModified, 1);
    QCOMPARE(server.listBytes, fullListBytes);
    // Another list (more notes) has a version of its own.
    QVERIFY(list(100));
    QVERIFY(!results.last().notModified);
    QVERIFY(list(100));
    QVERIFY(results.last().notModified);
    QCOMPARE(server.notModified, 2);
    // A note changed: the list comes in full again.
    QJsonObject *changed = server.noteRecordOf(QStringLiteral("n3"));
    QVERIFY(changed);
    changed->insert(QStringLiteral("title"), QStringLiteral("Changed"));
    server.storeScratch();
    QVERIFY(list(20));
    QVERIFY(!results.last().notModified);
    QCOMPARE(results.last().notes.size(), 20);
    QCOMPARE(server.notModified, 2);
    // After a failure the version is forgotten: the next answer must carry the notes.
    server.listStatus = 500;
    QVERIFY(list(20));
    QCOMPARE(results.last().failure, Client::Failure::Failed);
    server.listStatus = 200;
    QVERIFY(list(20));
    QVERIFY(!results.last().notModified);
    QCOMPARE(results.last().notes.size(), 20);
}

// While polls find the list as it was, the next one comes later and later; a change, or the
// user asking, brings the pace back.
void AgentNotesTest::centerSlowsDownWhileNothingChanges()
{
    FakeServer server;
    server.listVersions = true;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-3600), QStringLiteral("First")));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    QSignalSpy changed(&center, &Center::notesChanged);
    const auto lists = [&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size(); };
    const auto poll = [&] {
        const int before = lists();
        center.pollNow();
        return waitFor([&] { return lists() == before + 1 && !center.isRefreshing(); });
    };
    QCOMPARE(center.nextPollDelayMs(), -1);
    center.start(0);
    QVERIFY(waitFor([&] { return center.hasListed() && !center.isRefreshing(); }));
    QCOMPARE(changed.count(), 1);
    // Nobody watches: once a minute, then less and less often, down to every five minutes.
    QCOMPARE(center.nextPollDelayMs(), Center::IdleIntervalMs);
    QList<int> paces;
    for (int index = 0; index < 6; ++index) {
        QVERIFY(poll());
        paces.append(center.nextPollDelayMs());
    }
    QCOMPARE(paces, (QList<int> {60000, 120000, 120000, 120000, 120000, 120000}));
    QCOMPARE(Center::IdleMaximumIntervalMs, 120000);
    // The unchanged lists came back without their notes, and the notes stayed.
    QCOMPARE(server.notModified, 6);
    QCOMPARE(center.notes().size(), 1);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(center.lastFailure(), Client::Failure::None);

    // A new note: back to once a minute.
    server.notes.prepend(noteRecord(QStringLiteral("n2"), 2, now.addSecs(-1800), QStringLiteral("Second")));
    QVERIFY(poll());
    QCOMPARE(center.notes().size(), 2);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(center.nextPollDelayMs(), Center::IdleIntervalMs);

    // A view on screen: every 15 seconds, then down to once a minute. (The list asked for as
    // the view opened held nothing new, which is the first poll of the row.)
    QObject view;
    center.setWatching(&view, true);
    QVERIFY(waitFor([&] { return !center.isRefreshing() && server.requests.last().endsWith(QStringLiteral("limit=%1").arg(Center::WatchingLimit)); }));
    paces = {center.nextPollDelayMs()};
    for (int index = 0; index < 5; ++index) {
        QVERIFY(poll());
        paces.append(center.nextPollDelayMs());
    }
    QCOMPARE(paces, (QList<int> {15000, 30000, 30000, 30000, 30000, 30000}));
    QCOMPARE(Center::WatchingMaximumIntervalMs, 30000);
    // The user asks (Refresh, a reply sent): the pace starts over.
    center.refresh();
    QVERIFY(waitFor([&] { return !center.isRefreshing(); }));
    QCOMPARE(center.nextPollDelayMs(), Center::WatchingIntervalMs);
    center.setWatching(&view, false);
}

// Behind another application nobody looks at the notes: the list is asked for at the slow pace
// and without the extra notes, whatever view is open. Back in front, it is asked for at once.
void AgentNotesTest::centerPollsSlowlyBehindAnotherApplication()
{
    FakeServer server;
    QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-3600), QStringLiteral("First")));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath(), [&now] { return now; });
    QVERIFY(center.isForeground());
    const auto lists = [&] { return server.requests.filter(QStringLiteral("/v1/agent-notes?limit=")).size(); };
    QObject sidebar;
    center.setWatching(&sidebar, true);
    QVERIFY(waitFor([&] { return lists() == 1 && !center.isRefreshing(); }));
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::WatchingLimit));
    QCOMPARE(center.nextPollDelayMs(), Center::WatchingIntervalMs);

    // Another application comes to the front. The view stays open; the next poll is an idle one.
    center.setForeground(false);
    QVERIFY(center.isWatching());
    center.pollNow();
    QVERIFY(waitFor([&] { return lists() == 2 && !center.isRefreshing(); }));
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::IdleLimit));
    QCOMPARE(center.nextPollDelayMs(), Center::IdleIntervalMs);

    // Back within seconds: nothing is asked, the fast pace resumes.
    now = now.addSecs(5);
    center.setForeground(true);
    QTest::qWait(50);
    QCOMPARE(lists(), 2);
    QCOMPARE(center.nextPollDelayMs(), Center::WatchingIntervalMs);
    // Back after a while: the list is asked for at once, with the notes a view shows.
    center.setForeground(false);
    now = now.addSecs(600);
    center.setForeground(true);
    QVERIFY(waitFor([&] { return lists() == 3 && !center.isRefreshing(); }));
    QCOMPARE(server.requests.last(), QStringLiteral("GET /v1/agent-notes?limit=%1").arg(Center::WatchingLimit));
    QCOMPARE(center.nextPollDelayMs(), Center::WatchingIntervalMs);
    // With no view open, coming to the front asks nothing.
    center.setWatching(&sidebar, false);
    QVERIFY(waitFor([&] { return !center.isWatching(); }));
    center.setForeground(false);
    now = now.addSecs(600);
    center.setForeground(true);
    QTest::qWait(50);
    QCOMPARE(lists(), 3);
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
void AgentNotesTest::noteLinksOpenWhenClicked()
{
    // A note's sources are rectangles of its picture: a click on one opens it, elsewhere nothing.
    FakeServer server;
    const QByteArray picture = png(QSize(400, 800), QColor(240, 240, 235));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("With sources"), picture);
    record.insert(QStringLiteral("links"), QJsonArray{
        QJsonObject{{QStringLiteral("rect"), QJsonArray{0.1, 0.25, 0.3, 0.05}}, {QStringLiteral("href"), QStringLiteral("https://example.org/paper.pdf")},
                    {QStringLiteral("label"), QStringLiteral("pp. 9, 26")}},
        QJsonObject{{QStringLiteral("rect"), QJsonArray{0.5, 0.75, 0.2, 0.05}}, {QStringLiteral("href"), QStringLiteral("thoughts-pdf-link:notes.pdf?page=3")},
                    {QStringLiteral("label"), QStringLiteral("p. 4")}},
        QJsonObject{{QStringLiteral("rect"), QJsonArray{0.5, 0.5}}, {QStringLiteral("href"), QStringLiteral("https://example.org/broken")}}});
    server.notes.append(record);
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesWindow window(&center, {}, {}, {});
    QStringList opened;
    window.setLinkOpener([&opened](const QString &href) { opened.append(href); });
    window.resize(1200, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return center.hasListed() && window.currentNoteId() == QLatin1String("n1"); }));
    // A link without a full rectangle is dropped.
    QCOMPARE(center.note(QStringLiteral("n1"))->links.size(), 2);
    auto *image = window.findChild<AgentNoteImageView *>(QStringLiteral("agentNoteImage"));
    QVERIFY(image);
    QVERIFY(waitFor([&] { return !image->image().isNull() && image->width() > 0; }));
    QCOMPARE(image->links().size(), 2);
    const auto at = [image](double x, double y) { return QPoint(qRound(x * image->width()), qRound(y * image->height())); };
    QCOMPARE(image->linkAt(at(0.2, 0.27)), 0);
    QCOMPARE(image->linkAt(at(0.6, 0.77)), 1);
    QCOMPARE(image->linkAt(at(0.9, 0.1)), -1);
    QTest::mouseClick(image, Qt::LeftButton, {}, at(0.2, 0.27));
    QTest::mouseClick(image, Qt::LeftButton, {}, at(0.9, 0.1));
    QTest::mouseClick(image, Qt::LeftButton, {}, at(0.6, 0.77));
    QCOMPARE(opened, (QStringList{QStringLiteral("https://example.org/paper.pdf"), QStringLiteral("thoughts-pdf-link:notes.pdf?page=3")}));
    // Over a link the pointer is a hand.
    QTest::mouseMove(image, at(0.2, 0.27));
    QVERIFY(waitFor([&] { return image->cursor().shape() == Qt::PointingHandCursor; }));
    QTest::mouseMove(image, at(0.9, 0.1));
    QVERIFY(waitFor([&] { return image->cursor().shape() == Qt::ArrowCursor; }));
}

void AgentNotesTest::replyCarriesWhatTheAgentMayUse()
{
    // Sources: what the answering agent may use, sent with every reply and kept until changed.
    QSettings().remove(QStringLiteral("AgentNotes/ReplySources"));
    const auto restore = qScopeGuard([] { QSettings().remove(QStringLiteral("AgentNotes/ReplySources")); });
    FakeServer server;
    const QByteArray picture = png(QSize(120, 300), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, now.addSecs(-60), QStringLiteral("Question"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    QCOMPARE(center.replySources(), AgentNotes::ReplySources::LibraryAndMentalModel);
    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(waitFor([&] { return center.hasListed() && window.currentNoteId() == QLatin1String("n1"); }));
    auto *field = window.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"));
    auto *send = window.findChild<QPushButton *>(QStringLiteral("agentNoteSend"));
    auto *sources = window.findChild<QComboBox *>(QStringLiteral("agentNoteReplySources"));
    QVERIFY(field && send && sources && sources->isVisible());
    QCOMPARE(sources->count(), 4);
    QCOMPARE(sources->currentText(), QStringLiteral("All documents + Mental Model"));
    // The default goes with a reply too.
    field->setFocus();
    QTest::keyClicks(field, QStringLiteral("First"));
    send->click();
    QVERIFY(waitFor([&] { return field->toPlainText().isEmpty(); }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("sources")).toString(), QStringLiteral("library_and_mental_model"));
    // Chosen once, kept for the next replies and in the settings.
    sources->setCurrentIndex(sources->findData(AgentNotes::ReplySources::LinkedPdf));
    Q_EMIT sources->activated(sources->currentIndex());
    QCOMPARE(center.replySources(), QStringLiteral("linked_pdf"));
    QCOMPARE(QSettings().value(QStringLiteral("AgentNotes/ReplySources")).toString(), QStringLiteral("linked_pdf"));
    QTest::keyClicks(field, QStringLiteral("Second"));
    send->click();
    QVERIFY(waitFor([&] { return field->toPlainText().isEmpty(); }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("sources")).toString(), QStringLiteral("linked_pdf"));
    // The narrowed reply says so under the note; the default one does not.
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->replies.size() == 2; }));
    QStringList metas;
    QVERIFY(waitFor([&] {
        metas.clear();
        for (auto *meta : window.findChildren<QLabel *>(QStringLiteral("agentNoteReplyMeta"))) metas.append(meta->text());
        return metas.size() == 2;
    }));
    QCOMPARE(metas.filter(QStringLiteral("Linked PDF only")).size(), 1);
    QCOMPARE(metas.filter(QStringLiteral("Mental Model")).size(), 0);
    // Another Center (the next start) reads the choice back; an unknown value falls back to the default.
    Center later(&client, statePath());
    QCOMPARE(later.replySources(), QStringLiteral("linked_pdf"));
    QSettings().setValue(QStringLiteral("AgentNotes/ReplySources"), QStringLiteral("everything"));
    QCOMPARE(later.replySources(), AgentNotes::ReplySources::defaultValue());
}

// 19 Nov 2026: asking what the agent may use ("All documents + Mental Model") is wrong when
// the reply goes back to the chat that sent the note, which has it all open. Who answers
// decides: the note's own conversation (listening, woken, chosen, or not known yet) continues;
// a topic owner, the reply agent, another conversation or nobody start a new one.
void AgentNotesTest::continuedConversationIsTheSendersOwn()
{
    QJsonObject record = noteRecord(QStringLiteral("n1"), 1, QDateTime::currentDateTimeUtc(), QStringLiteral("Question"));
    record.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("mcp")}, {QStringLiteral("conversation_agent"), QStringLiteral("claude")},
                                                        {QStringLiteral("conversation_id"), ConversationA}});
    const Note fromConversation = note(record);
    const Note connector = note(noteRecord(QStringLiteral("n2"), 2, QDateTime::currentDateTimeUtc(), QStringLiteral("Plain")));
    QJsonObject page = noteRecord(QStringLiteral("p1"), 3, QDateTime::currentDateTimeUtc(), QStringLiteral("Page"));
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}, {QStringLiteral("conversation_id"), ConversationA}});
    QJsonObject linked = noteRecord(QStringLiteral("n3"), 4, QDateTime::currentDateTimeUtc(), QStringLiteral("Chat"));
    linked.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("conversation_agent"), QStringLiteral("claude")}, {QStringLiteral("conversation_url"), QStringLiteral("https://claude.ai/chat/abc")}});

    // Before the server says who listens: the note's own conversation, if it has one.
    QVERIFY(continuedConversation(fromConversation, std::nullopt));
    QVERIFY(continuedConversation(note(linked), std::nullopt));
    QVERIFY(continuedConversation(fromConversation, ListeningAnswer{}));
    QVERIFY(!continuedConversation(connector, std::nullopt));
    QVERIFY(!continuedConversation(note(page), std::nullopt));
    QCOMPARE(continuedConversation(fromConversation, std::nullopt)->text(), QStringLiteral("Your reply goes back to the agent's conversation."));

    const auto answerOf = [](const QJsonObject &json) { return std::optional<ListeningAnswer>(ListeningAnswer::fromJson(json)); };
    const QJsonObject ownSender = agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationA);
    const QJsonObject otherSender = agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationB);
    // Its agent listens.
    QVERIFY(continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("listeners"), QJsonArray{ownSender}}})));
    // A reply waker wakes its session: named by its title.
    const auto woken = continuedConversation(
        fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")},
                                    {QStringLiteral("wake"), wakeRecord(QStringLiteral("will_be_woken"), QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Example session"))}}));
    QVERIFY(woken);
    QCOMPARE(woken->title, QStringLiteral("Example session"));
    QCOMPARE(woken->text(), QStringLiteral("Your reply goes back to the agent's conversation “Example session”."));
    // ... also while its machine is offline (it answers when back).
    QVERIFY(continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")},
                                                              {QStringLiteral("wake"), wakeRecord(QStringLiteral("waker_offline"), QStringLiteral("sender"), QStringLiteral("mac"), {})}})));
    // Chosen by the user, and live.
    QVERIFY(continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("answer_by"), ownSender}})));

    // Someone else answers: a new conversation, the choice stays.
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("listeners"), QJsonArray{topicAgentRecord()}}})));
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("listeners"), QJsonArray{replyAgentRecord()}}})));
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("answer_by"), otherSender}})));
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")},
                                                               {QStringLiteral("wake"), wakeRecord(QStringLiteral("will_be_woken"), QStringLiteral("responder"), QStringLiteral("example-server"), QStringLiteral("Agent Study"))}})));
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")},
                                                               {QStringLiteral("wake"), wakeRecord(QStringLiteral("will_be_woken"), QStringLiteral("owner"), QStringLiteral("mac"), QStringLiteral("Topic session"))}})));
    // Nobody listens, or the agent chosen does not: the reply agent may take it.
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")}})));
    QVERIFY(!continuedConversation(fromConversation, answerOf({{QStringLiteral("state"), QStringLiteral("not_listening")},
                                                               {QStringLiteral("answer_by"), agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationA, {}, {}, false)}})));
}

// 19 Nov: ⌘B in the reader and in Agent Notes, and a button in the Agent Notes window for it.
// The window's notes list (beside the open note) hides and shows with ⌘B (on macOS Control-B
// too) and with the reader's sidebar icon over it, which says which it does; the choice is kept
// for the next window. The reader's sidebar of notes has no such toggle (the reader's own ⌘B
// hides that sidebar).
void AgentNotesTest::notesListHidesLikeTheReadersSidebar()
{
    QSettings().remove(QStringLiteral("AgentNotes/ListShown"));
    const auto restore = qScopeGuard([] { QSettings().remove(QStringLiteral("AgentNotes/ListShown")); });
    FakeServer server;
    const QByteArray picture = png(QSize(120, 300), QColor(30, 90, 200));
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, QDateTime::currentDateTimeUtc().addSecs(-60), QStringLiteral("Question"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 900);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QVERIFY(waitFor([&] { return center.hasListed() && window.currentNoteId() == QLatin1String("n1"); }));
    auto *list = window.findChild<QListWidget *>(QStringLiteral("agentNotesList"));
    auto *button = window.findChild<QToolButton *>(QStringLiteral("agentNotesToggleListButton"));
    QVERIFY(list && button);
    QVERIFY(list->isVisible() && button->isVisible() && button->isChecked());
    QVERIFY(window.listShown());
    const QString key = QKeySequence(Qt::CTRL | Qt::Key_B).toString(QKeySequence::NativeText);
    QCOMPARE(button->toolTip(), QStringLiteral("Hide Sidebar (%1)").arg(key));

    // ⌘B with the focus in the list: hidden, the note stays, and so does the choice.
    list->setFocus();
    QTest::keyClick(list, Qt::Key_B, Qt::ControlModifier);
    QVERIFY(waitFor([&] { return !list->isVisible(); }));
    QVERIFY(!button->isChecked() && !window.listShown());
    QCOMPARE(button->toolTip(), QStringLiteral("Show Sidebar (%1)").arg(key));
    QCOMPARE(window.currentNoteId(), QStringLiteral("n1"));
    QCOMPARE(QSettings().value(QStringLiteral("AgentNotes/ListShown")).toBool(), false);
    // The button: shown again.
    button->click();
    QVERIFY(list->isVisible() && button->isChecked());
    QCOMPARE(QSettings().value(QStringLiteral("AgentNotes/ListShown")).toBool(), true);
#ifdef Q_OS_MACOS
    QTest::keyClick(&window, Qt::Key_B, Qt::MetaModifier);
#else
    QTest::keyClick(&window, Qt::Key_B, Qt::ControlModifier);
#endif
    QVERIFY(waitFor([&] { return !list->isVisible(); }));
    // The next window starts as this one was left.
    AgentNotesWindow later(&center, {}, {}, {});
    QVERIFY(later.findChild<QListWidget *>(QStringLiteral("agentNotesList"))->isHidden());
    QVERIFY(!later.findChild<QToolButton *>(QStringLiteral("agentNotesToggleListButton"))->isChecked());
    // The reader's sidebar of notes has none.
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    QVERIFY(!sidebar.findChild<QToolButton *>(QStringLiteral("agentNotesToggleListButton")));
    QVERIFY(sidebar.listShown());
}

// The reply box of a note from an agent's conversation: in the window and in the reader's
// sidebar, a line saying where the reply goes instead of the Sources choice, and the reply carries
// the default sources whatever was chosen for new conversations. When nobody listens any more,
// the choice is back.
void AgentNotesTest::replyToTheSendingConversationSaysSoInsteadOfSources()
{
    QSettings().setValue(QStringLiteral("AgentNotes/ReplySources"), QStringLiteral("linked_pdf"));
    const auto restore = qScopeGuard([] { QSettings().remove(QStringLiteral("AgentNotes/ReplySources")); });
    FakeServer server;
    const QByteArray picture = png(QSize(120, 300), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject record = noteRecord(QStringLiteral("n1"), 1, now.addSecs(-120), QStringLiteral("From a chat"), picture);
    record.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("mcp")}, {QStringLiteral("conversation_agent"), QStringLiteral("claude")},
                                                        {QStringLiteral("conversation_id"), ConversationA}});
    server.notes.append(record);
    server.notes.append(noteRecord(QStringLiteral("n2"), 2, now.addSecs(-60), QStringLiteral("From the connector"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/n2/image"), picture);
    server.answers.insert(QStringLiteral("n1"), QJsonObject{{QStringLiteral("note_id"), QStringLiteral("n1")}, {QStringLiteral("state"), QStringLiteral("not_listening")},
                                                            {QStringLiteral("listeners"), QJsonArray()},
                                                            {QStringLiteral("wake"), wakeRecord(QStringLiteral("will_be_woken"), QStringLiteral("sender"), QStringLiteral("mac"),
                                                                                                QStringLiteral("Example session"))}});
    server.listening.insert(QStringLiteral("n2"), QStringLiteral("not_listening"));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.resize(300, 800);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    QVERIFY(waitFor([&] { return center.hasListed(); }));
    window.selectNote(QStringLiteral("n1"));
    sidebar.selectNote(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return center.answer(QStringLiteral("n1")).has_value(); }));
    const QString statement = QStringLiteral("Your reply goes back to the agent's conversation “Example session”.");
    for (QWidget *view : {static_cast<QWidget *>(&window), static_cast<QWidget *>(&sidebar)}) {
        auto *sources = view->findChild<QComboBox *>(QStringLiteral("agentNoteReplySources"));
        auto *continues = view->findChild<QLabel *>(QStringLiteral("agentNoteReplyContinues"));
        QVERIFY(sources && continues);
        QVERIFY2(waitFor([&] { return continues->isVisible() && continues->text() == statement; }), qPrintable(continues->text()));
        QVERIFY(!sources->isVisible());
    }
    // The label "Sources:" goes with the choice.
    for (QLabel *label : window.findChildren<QLabel *>()) {
        if (label->text() == QStringLiteral("Sources:")) QVERIFY(!label->isVisible());
    }

    // The reply goes with the default sources, not the choice kept for new conversations.
    auto *field = window.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"));
    auto *send = window.findChild<QPushButton *>(QStringLiteral("agentNoteSend"));
    field->setFocus();
    QTest::keyClicks(field, QStringLiteral("Go on"));
    send->click();
    QVERIFY(waitFor([&] { return field->toPlainText().isEmpty(); }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("sources")).toString(), QStringLiteral("library_and_mental_model"));
    QCOMPARE(center.replySources(), QStringLiteral("linked_pdf"));

    // A note of no conversation: the choice, as before, and the reply carries it.
    window.selectNote(QStringLiteral("n2"));
    auto *sources = window.findChild<QComboBox *>(QStringLiteral("agentNoteReplySources"));
    auto *continues = window.findChild<QLabel *>(QStringLiteral("agentNoteReplyContinues"));
    QVERIFY(waitFor([&] { return sources->isVisible() && !continues->isVisible(); }));
    QCOMPARE(sources->currentData().toString(), QStringLiteral("linked_pdf"));
    QTest::keyClicks(field, QStringLiteral("New question"));
    send->click();
    QVERIFY(waitFor([&] { return field->toPlainText().isEmpty(); }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("sources")).toString(), QStringLiteral("linked_pdf"));

    // Nobody would answer in the conversation any more: the choice is back for the first note too.
    server.answers.insert(QStringLiteral("n1"), QJsonObject{{QStringLiteral("note_id"), QStringLiteral("n1")}, {QStringLiteral("state"), QStringLiteral("not_listening")},
                                                            {QStringLiteral("listeners"), QJsonArray()}});
    window.selectNote(QStringLiteral("n1"));
    center.checkListeningIfOlder(QStringLiteral("n1"), 0);
    QVERIFY(waitFor([&] { return sources->isVisible() && !continues->isVisible(); }));
    auto *sidebarSources = sidebar.findChild<QComboBox *>(QStringLiteral("agentNoteReplySources"));
    QVERIFY(waitFor([&] { return sidebarSources->isVisible(); }));
}

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
// 24 Nov 2026: highlighting meant scrolling to the end of the note for Draw on the Picture…, which opened a
// window. Highlight sits at the top of the note and draws on the note itself; the marks stay shown over the
// note and go with the reply, whose box stays in view under the note.
void AgentNotesTest::highlightOnTheNoteItself()
{
    FakeServer server;
    const QByteArray picture = png(QSize(400, 600), Qt::white);
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, QDateTime::currentDateTimeUtc(), QStringLiteral("Read this"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesView view(AgentNotesView::Layout::Split, &center, {}, {}, {});
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(waitFor([&] { return center.hasListed(); }));
    view.selectNote(QStringLiteral("n1"));
    auto *image = view.findChild<AgentNoteImageView *>();
    QVERIFY(waitFor([&] { return !image->image().isNull(); }));
    // The reply box is under the note, out of its scroll area: always in view.
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("agentNoteScroll"));
    auto *replyBox = view.findChild<QWidget *>(QStringLiteral("agentNoteReplyBox"));
    QVERIFY(replyBox && !scroll->widget()->isAncestorOf(replyBox) && replyBox->isVisible());

    auto *highlight = view.findChild<QToolButton *>(QStringLiteral("agentNoteInlineHighlight"));
    QVERIFY(highlight && highlight->isVisible());
    // The tools are with the reply, in view wherever the note is scrolled; no button opens a window.
    QVERIFY(replyBox->isAncestorOf(highlight));
    QVERIFY(!view.findChild<QPushButton *>(QStringLiteral("agentNoteDraw")));
    highlight->click();
    auto *canvas = view.findChild<AgentNoteCanvas *>(QStringLiteral("agentNoteCanvas"));
    QVERIFY(highlight->isChecked() && canvas->isVisible() && !image->isVisible());
    QVERIFY(waitFor([&] { return canvas->width() > 100; }));
    const qreal toCanvas = canvas->width() / 400.0;
    drag(canvas, {QPointF(50, 300) * toCanvas, QPointF(200, 300) * toCanvas, QPointF(350, 300) * toCanvas});
    auto draft = center.existingDraft(QStringLiteral("n1"));
    QVERIFY(draft);
    QCOMPARE(draft->drawing.strokes().size(), 1);
    // Back to reading: the mark stays over the note, Undo can take it back, the reply knows of it.
    highlight->click();
    QVERIFY(!highlight->isChecked() && !canvas->isVisible() && image->isVisible());
    auto *undo = view.findChild<QToolButton *>(QStringLiteral("agentNoteInlineUndo"));
    QVERIFY(undo->isEnabled());
    QVERIFY(waitFor([&] { return view.findChild<QLabel *>(QStringLiteral("agentNoteReplyStatus"))->text().contains(QStringLiteral("With your drawing")); }));
    QImage shown(image->size(), QImage::Format_ARGB32);
    image->render(&shown);
    QVERIFY2(shown.pixelColor(image->width() / 2, int(image->height() * 0.5)) != QColor(Qt::white), "the highlight is drawn over the note");

    view.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"))->setPlainText(QStringLiteral("this part"));
    view.findChild<QPushButton *>(QStringLiteral("agentNoteSend"))->click();
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->replies.size() == 1; }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("message")).toString(), QStringLiteral("this part"));
    const QJsonArray strokes = body.value(QStringLiteral("strokes")).toObject().value(QStringLiteral("strokes")).toArray();
    QCOMPARE(strokes.size(), 1);
    QCOMPARE(strokes.first().toObject().value(QStringLiteral("tool")).toString(), QStringLiteral("highlighter"));
    // Sent: its marks leave the note.
    QVERIFY(waitFor([&] { return !undo->isEnabled(); }));
}

void AgentNotesTest::drawnReplySendsTheComposite()
{
    FakeServer server;
    const QByteArray picture = png(QSize(400, 600), Qt::white);
    server.notes.append(noteRecord(QStringLiteral("n1"), 1, QDateTime::currentDateTimeUtc(), QStringLiteral("Draw the graph"), picture));
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    // In the sidebar too: the drawing happens on the note itself, with the tools by the reply.
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.resize(300, 800);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    QVERIFY(waitFor([&] { return center.hasListed(); }));
    sidebar.selectNote(QStringLiteral("n1"));
    QCOMPARE(sidebar.shownNoteId(), QStringLiteral("n1"));
    auto *image = sidebar.findChild<AgentNoteImageView *>();
    QVERIFY(waitFor([&] { return !image->image().isNull(); }));
    auto *field = sidebar.findChild<QPlainTextEdit *>(QStringLiteral("agentNoteReplyField"));
    field->setPlainText(QStringLiteral("1: here"));
    const auto tool = [&sidebar](const char *name) { return sidebar.findChild<QToolButton *>(QString::fromLatin1(name)); };
    tool("agentNoteInlinePen")->click();
    auto *canvas = sidebar.findChild<AgentNoteCanvas *>(QStringLiteral("agentNoteCanvas"));
    QVERIFY(canvas->isVisible());
    QVERIFY(waitFor([&] { return !canvas->image().isNull() && canvas->width() > 100; }));
    auto draft = center.existingDraft(QStringLiteral("n1"));
    QVERIFY(draft);
    const auto strokes = [&center] { return center.existingDraft(QStringLiteral("n1"))->drawing.strokes().size(); };

    // A pen stroke across the middle, a marker, an extra stroke erased again.
    const qreal toCanvas = canvas->width() / 400.0;
    drag(canvas, {QPointF(50, 300) * toCanvas, QPointF(200, 300) * toCanvas, QPointF(350, 300) * toCanvas});
    QCOMPARE(strokes(), 1);
    tool("agentNoteInlineNumber")->click();
    QVERIFY(tool("agentNoteInlineNumber")->isChecked() && !tool("agentNoteInlinePen")->isChecked());
    drag(canvas, {QPointF(200, 100) * toCanvas});
    QCOMPARE(center.existingDraft(QStringLiteral("n1"))->drawing.markers().size(), 1);
    tool("agentNoteInlinePen")->click();
    drag(canvas, {QPointF(50, 500) * toCanvas, QPointF(350, 500) * toCanvas});
    QCOMPARE(strokes(), 2);
    tool("agentNoteInlineErase")->click();
    drag(canvas, {QPointF(200, 480) * toCanvas, QPointF(200, 520) * toCanvas});
    QCOMPARE(strokes(), 1);
    QVERIFY(waitFor([&] { return tool("agentNoteInlineUndo")->isEnabled(); }));
    tool("agentNoteInlineUndo")->click();
    QCOMPARE(strokes(), 2);
    QVERIFY(waitFor([&] { return tool("agentNoteInlineRedo")->isEnabled(); }));
    tool("agentNoteInlineRedo")->click();
    QCOMPARE(strokes(), 1);
    // Clear takes every mark away, and Undo brings them back.
    QVERIFY(waitFor([&] { return tool("agentNoteInlineClear")->isEnabled(); }));
    tool("agentNoteInlineClear")->click();
    QVERIFY(center.existingDraft(QStringLiteral("n1"))->drawing.isEmpty());
    tool("agentNoteInlineUndo")->click();
    QCOMPARE(strokes(), 1);
    QCOMPARE(center.existingDraft(QStringLiteral("n1"))->drawing.markers().size(), 1);
    // The note's reply box knows about the drawing.
    QVERIFY(waitFor([&] { return sidebar.findChild<QLabel *>(QStringLiteral("agentNoteReplyStatus"))->text().contains(QStringLiteral("With your drawing")); }));

    sidebar.findChild<QPushButton *>(QStringLiteral("agentNoteSend"))->click();
    QVERIFY(waitFor([&] { return center.note(QStringLiteral("n1"))->replies.size() == 1; }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("message")).toString(), QStringLiteral("1: here"));
    const QJsonObject sentStrokes = body.value(QStringLiteral("strokes")).toObject();
    QCOMPARE(sentStrokes.value(QStringLiteral("strokes")).toArray().size(), 1);
    QCOMPARE(sentStrokes.value(QStringLiteral("markers")).toArray().size(), 1);
    QCOMPARE(sentStrokes.value(QStringLiteral("image_width")).toInt(), 400);
    const QJsonObject sentImage = body.value(QStringLiteral("image")).toObject();
    QCOMPARE(sentImage.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    const QImage sent = QImage::fromData(QByteArray::fromBase64(sentImage.value(QStringLiteral("data_base64")).toString().toLatin1()));
    QCOMPARE(sent.size(), QSize(400, 600));
    QCOMPARE(sent.pixelColor(200, 300), QColor(Qt::black));
    QCOMPARE(sent.pixelColor(200, 500), QColor(Qt::white));
    QCOMPARE(center.note(QStringLiteral("n1"))->replies.first().strokeCount, 1);
    QVERIFY(field->toPlainText().isEmpty());
    // Sent: the marks leave the note, and the note shows its picture again.
    QVERIFY(waitFor([&] { return !tool("agentNoteInlineUndo")->isEnabled() && image->isVisible() && !canvas->isVisible(); }));
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
    QCOMPARE(page.senderLabel(), QStringLiteral("You, from this computer"));
    QCOMPARE(center.unreadCount(), 0);
    sidebar.selectNote(page.id);
    QVERIFY(!sidebar.findChild<QWidget *>(QStringLiteral("agentNoteReplyBox"))->isVisible());
}

// Ask About This on a reading overlay's mark (reading-overlays.md §4): a page like Send Page's
// whose origin also names the overlay's conversation and the mark (agent-notes.md, "Pages sent to
// one session"); what every page has is never replaced.
void AgentNotesTest::askedPageNamesItsSession()
{
    FakeServer server;
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    Center::Page page;
    page.title = QStringLiteral("About “Reverse KL” (p. 3)");
    page.message = QStringLiteral("Question about a mark of your reading overlay …");
    page.documentId = QStringLiteral("doc-7");
    page.documentFilename = QStringLiteral("agents_vs_network.pdf");
    page.documentName = QStringLiteral("AgentsVsNetwork");
    page.page = 3;
    page.pages = 12;
    page.image = QImage(400, 600, QImage::Format_RGB32);
    page.image.fill(Qt::white);
    const QJsonObject mark{{QStringLiteral("overlay_id"), QStringLiteral("5b0c1f6e-2a47-4d1e-9c3b-7e8f2a6d4c10")},
                           {QStringLiteral("mark_id"), QStringLiteral("m1")},
                           {QStringLiteral("kind"), QStringLiteral("skip")}};
    page.origin = QJsonObject{{QStringLiteral("conversation_id"), ConversationA}, {QStringLiteral("reading_overlay"), mark},
                              {QStringLiteral("kind"), QStringLiteral("other")}, {QStringLiteral("page"), 99}};
    bool sent = false;
    center.sendPage(page, [&sent](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    const QJsonObject body = server.bodies.last();
    QCOMPARE(body.value(QStringLiteral("title")).toString(), page.title);
    QCOMPARE(body.value(QStringLiteral("message")).toString(), page.message);
    QCOMPARE(body.value(QStringLiteral("document_id")).toString(), QStringLiteral("doc-7"));
    QCOMPARE(body.value(QStringLiteral("document_filename")).toString(), QStringLiteral("agents_vs_network.pdf"));
    QCOMPARE(body.value(QStringLiteral("origin")).toObject(),
             (QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}, {QStringLiteral("device"), QStringLiteral("mac")},
                          {QStringLiteral("page"), 3}, {QStringLiteral("pages"), 12}, {QStringLiteral("document_name"), QStringLiteral("AgentsVsNetwork")},
                          {QStringLiteral("stroke_count"), 0}, {QStringLiteral("conversation_id"), ConversationA}, {QStringLiteral("reading_overlay"), mark}}));
}

// A reading overlay mark's note (origin.kind "overlay_mark", reading-overlays.md "Mark notes") is
// opened from its mark, never from the list: a server before reading_overlay_mark_notes_v1 lists
// it, and the list here leaves it out all the same (no unread count, no notification).
void AgentNotesTest::markNotesAreNeverListed()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject mark = noteRecord(QStringLiteral("4b1f2c3d-5e6f-4a7b-8c9d-0e1f2a3b4c5d"), 3, now, QStringLiteral("What to read · Read carefully: “The loss”…"));
    mark.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("overlay_mark")}, {QStringLiteral("agent"), QStringLiteral("claude")}});
    server.notes = QJsonArray{mark, noteRecord(QStringLiteral("n2"), 2, now.addSecs(-60), QStringLiteral("Listed"))};
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    center.refresh();
    QVERIFY(waitFor([&] { return center.hasListed() && !center.isRefreshing(); }));
    QCOMPARE(center.notes().size(), 1);
    QCOMPARE(center.notes().first().id, QStringLiteral("n2"));
    QVERIFY(!center.note(QStringLiteral("4b1f2c3d-5e6f-4a7b-8c9d-0e1f2a3b4c5d")));
    QCOMPARE(center.unreadCount(), 1);
    QVERIFY(waitFor([&] { return list(sidebar)->count() == 1; }));
    QCOMPARE(list(sidebar)->item(0)->text(), QStringLiteral("Listed"));
    // A document's notes (merged into the list) leave it out too.
    center.merge({note(mark)});
    QCOMPARE(center.notes().size(), 1);
    QVERIFY(note(mark).isMarkNote());
    QVERIFY(!note(server.notes.at(1).toObject()).isMarkNote());
}

// A mark note is fetched by its id (GET /v1/agent-notes/{id}) and kept apart from the list: the
// Agent Notes window shows it with nothing selected in its list, also while the list is empty and
// when it is asked for again; picking a note in the list shows that one. A deleted or unknown note
// is not kept.
void AgentNotesTest::markNoteOpensByItsId()
{
    FakeServer server;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString markId = QStringLiteral("4b1f2c3d-5e6f-4a7b-8c9d-0e1f2a3b4c5d");
    QImage picture(400, 900, QImage::Format_RGB32);
    picture.fill(Qt::white);
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
    }
    QJsonObject mark = noteRecord(markId, 7, now, QStringLiteral("What to read · Read carefully: “The loss”…"), png);
    mark.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("overlay_mark")}, {QStringLiteral("agent"), QStringLiteral("claude")},
                                                      {QStringLiteral("conversation_id"), ConversationA}});
    mark.insert(QStringLiteral("document_id"), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"));
    server.markNotes = QJsonArray{mark};
    server.files.insert(QStringLiteral("/v1/agent-notes/%1/image").arg(markId), png);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.refresh();
    QVERIFY(waitFor([&] { return center.hasListed() && !center.isRefreshing(); }));
    QVERIFY(center.notes().isEmpty());

    std::optional<Note> fetched;
    bool answered = false;
    center.fetchNote(markId, [&](const std::optional<Note> &note, const QString &) {
        fetched = note;
        answered = true;
    });
    QVERIFY(waitFor([&] { return answered; }));
    QVERIFY(fetched && fetched->isMarkNote());
    QVERIFY(server.requests.contains(QStringLiteral("GET /v1/agent-notes/%1").arg(markId)));
    // Kept apart: found by its id, never listed, never unread.
    QVERIFY(center.note(markId));
    QVERIFY(center.notes().isEmpty());
    QCOMPARE(center.unreadCount(), 0);

    AgentNotesWindow window(&center, {}, {}, {});
    window.showNote(markId);
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.shownNoteId(), markId);
    QVERIFY(window.currentNoteId().isEmpty());
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("agentNoteTitle"))->text(), mark.value(QStringLiteral("title")).toString());
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("agentNotesPlaceholder"))->isVisible());
    auto *image = window.findChild<AgentNoteImageView *>(QStringLiteral("agentNoteImage"));
    QVERIFY(waitFor([&] { return !image->image().isNull(); }));
    QCOMPARE(image->image().size(), picture.size());
    QCOMPARE(list(window)->count(), 0);

    // The list fills; the mark note stays shown, nothing selected, and is not among them.
    server.notes = QJsonArray{noteRecord(QStringLiteral("n1"), 5, now.addSecs(-60), QStringLiteral("Listed"))};
    center.refresh();
    QVERIFY(waitFor([&] { return list(window)->count() == 1 && !center.isRefreshing(); }));
    QCOMPARE(window.shownNoteId(), markId);
    QVERIFY(window.currentNoteId().isEmpty());
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("agentNoteTitle"))->text(), mark.value(QStringLiteral("title")).toString());
    // Picked in the list: that note.
    list(window)->setCurrentRow(0);
    QCOMPARE(window.shownNoteId(), QStringLiteral("n1"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("agentNoteTitle"))->text(), QStringLiteral("Listed"));
    // Asked for again (its mark clicked again): shown again.
    window.showNote(markId);
    QCOMPARE(window.shownNoteId(), markId);
    QVERIFY(window.currentNoteId().isEmpty());

    // Unknown, or deleted since: not kept, and the window goes back to the list.
    answered = false;
    QString error;
    center.fetchNote(QStringLiteral("0c1d2e3f-4a5b-4c6d-8e7f-8091a2b3c4d5"), [&](const std::optional<Note> &note, const QString &why) {
        fetched = note;
        error = why;
        answered = true;
    });
    QVERIFY(waitFor([&] { return answered; }));
    QVERIFY(!fetched);
    QVERIFY(!error.isEmpty());
    mark.insert(QStringLiteral("deleted"), true);
    server.markNotes = QJsonArray{mark};
    answered = false;
    center.fetchNote(markId, [&](const std::optional<Note> &note, const QString &) {
        fetched = note;
        answered = true;
    });
    QVERIFY(waitFor([&] { return answered; }));
    QVERIFY(!fetched);
    QVERIFY(!center.note(markId));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("n1"); }));
}

// A menu's entries as text: "---" for a separator, "<title> > [<entries>]" for a submenu, " [off]"
// for a disabled entry and " [x]" for a checked one.
QStringList menuEntries(QMenu &menu)
{
    QStringList texts;
    for (QAction *action : menu.actions()) {
        if (action->isSeparator()) {
            texts.append(QStringLiteral("---"));
        } else if (action->menu()) {
            texts.append(action->text() + QStringLiteral(" > [") + menuEntries(*action->menu()).join(QStringLiteral(", ")) + QLatin1Char(']'));
        } else {
            texts.append(action->text() + (action->isEnabled() ? QString() : QStringLiteral(" [off]")) + (action->isChecked() ? QStringLiteral(" [x]") : QString()));
        }
    }
    return texts;
}

// A wakers answer (GET /v1/agent-notes/wakers) of two machines, as the "To:" of Send to Agent lists it.
QJsonObject testWakers()
{
    const auto session = [](const QString &id, const QString &title, const QString &state, const QString &lastActive, std::optional<bool> wakeable = std::nullopt) {
        QJsonObject record{{QStringLiteral("session_id"), id}, {QStringLiteral("title"), title}, {QStringLiteral("state"), state}, {QStringLiteral("kind"), QStringLiteral("claude")}};
        if (!lastActive.isEmpty()) record.insert(QStringLiteral("last_active_at"), lastActive);
        if (wakeable) record.insert(QStringLiteral("wakeable"), *wakeable);
        return record;
    };
    // Listed offline machine first on purpose: the live one's sessions come first all the same.
    return QJsonObject{{QStringLiteral("wakers"),
                        QJsonArray{QJsonObject{{QStringLiteral("machine"), QStringLiteral("macbook")}, {QStringLiteral("live"), false},
                                               {QStringLiteral("sessions"), QJsonArray{session(QStringLiteral("s6"), QStringLiteral("Laptop session"), QStringLiteral("idle"), QStringLiteral("2026-11-20T12:00:00Z"))}}},
                                   QJsonObject{{QStringLiteral("machine"), QStringLiteral("example-server")}, {QStringLiteral("live"), true},
                                               {QStringLiteral("sessions"),
                                                QJsonArray{session(QStringLiteral("s2"), QStringLiteral("Old work"), QStringLiteral("closed"), QStringLiteral("2026-11-20T11:00:00Z")),
                                                           session(QStringLiteral("s3"), QStringLiteral("Busy one"), QStringLiteral("busy"), QStringLiteral("2026-11-20T09:00:00Z")),
                                                           session(QStringLiteral("s4"), QStringLiteral("Cannot wake"), QStringLiteral("idle"), QStringLiteral("2026-11-20T13:00:00Z"), false),
                                                           session(QStringLiteral("0f9e8d7c-6b5a-4938-a271-605f4e3d2c1b"), QString(), QStringLiteral("idle"), QString()),
                                                           session(QStringLiteral("s1"), QStringLiteral("Thought Editor"), QStringLiteral("idle"), QStringLiteral("2026-11-20T10:00:00Z"))}}}}},
                       {QStringLiteral("checked_at"), QStringLiteral("2026-11-20T13:30:00Z")}};
}

// Send to Agent's "To:": the sessions reply wakers can wake, those of live machines first, open
// ones before closed ones, the most recently active first; one that cannot be woken left out. Each
// reads "<title> · <machine>", with what it is doing when it is not simply waiting. The message is
// the request, a blank line, then Copy for Agent's text.
void AgentNotesTest::sendToAgentRecipients()
{
    const QList<Recipient> sessions = recipients(testWakers());
    QStringList ids;
    QStringList choices;
    for (const Recipient &session : sessions) {
        ids.append(session.sessionId);
        choices.append(session.choiceText());
    }
    QCOMPARE(ids, (QStringList{QStringLiteral("s1"), QStringLiteral("s3"), QStringLiteral("0f9e8d7c-6b5a-4938-a271-605f4e3d2c1b"), QStringLiteral("s2"), QStringLiteral("s6")}));
    QCOMPARE(choices, (QStringList{QStringLiteral("Thought Editor · example-server"), QStringLiteral("Busy one · example-server · busy"), QStringLiteral("Session 0f9e8d7c · example-server"),
                                   QStringLiteral("Old work · example-server · closed"), QStringLiteral("Laptop session · macbook · offline")}));
    QCOMPARE(sessions.first().label(), QStringLiteral("Thought Editor · example-server"));
    // A session two machines name is listed once, where it ranks best.
    QJsonObject twice = testWakers();
    QJsonArray machines = twice.value(QStringLiteral("wakers")).toArray();
    QJsonObject laptop = machines.at(0).toObject();
    QJsonArray laptopSessions = laptop.value(QStringLiteral("sessions")).toArray();
    laptopSessions.append(QJsonObject{{QStringLiteral("session_id"), QStringLiteral("s1")}, {QStringLiteral("title"), QStringLiteral("Thought Editor")}, {QStringLiteral("state"), QStringLiteral("idle")}});
    laptop.insert(QStringLiteral("sessions"), laptopSessions);
    machines.replace(0, laptop);
    twice.insert(QStringLiteral("wakers"), machines);
    QCOMPARE(recipients(twice).size(), 5);
    QCOMPARE(recipients(twice).first().machine, QStringLiteral("example-server"));
    QVERIFY(recipients(QJsonObject()).isEmpty());

    // Kept between runs as they were.
    const QList<Recipient> kept = recipientsFromJson(recipientsJson(sessions));
    QCOMPARE(kept.size(), sessions.size());
    for (int index = 0; index < kept.size(); ++index) {
        QCOMPARE(kept.at(index).sessionId, sessions.at(index).sessionId);
        QCOMPARE(kept.at(index).choiceText(), sessions.at(index).choiceText());
        QCOMPARE(kept.at(index).lastActive, sessions.at(index).lastActive);
    }
}

// Send to Agent's menus: the sessions as last fetched (kept in the app's settings, so a menu lists
// them at once), fetched again in the background; what comes while the menu is open shows in it.
// The session sent to last says so in its tip; a click hands its session over. With nothing to list, one
// disabled entry says why.
void AgentNotesTest::sendToAgentSessionMenu()
{
    FakeServer server;
    server.wakers = testWakers();
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    const auto forget = [] {
        QSettings settings;
        for (const QString &key : {QStringLiteral("AgentNotes/SendToSessions"), QStringLiteral("AgentNotes/SendToSession"), QStringLiteral("AgentNotes/SendToSessionLabel"),
                                   QStringLiteral("AgentNotes/ServerAgents"), QStringLiteral("AgentNotes/ServerSession")})
            settings.remove(key);
    };
    forget();  // nothing kept from an earlier run
    const auto restore = qScopeGuard(forget);
    const auto entries = [](QMenu &menu) { return menuEntries(menu); };
    {
        // Nothing fetched yet: listing, then the sessions, also into the open menu.
        AgentSessionList sessions(&center);
        QMenu menu;
        std::optional<Recipient> clicked;
        sessions.populate(&menu, [&clicked](const Recipient &session) { clicked = session; });
        QCOMPARE(entries(menu), QStringList{QStringLiteral("Listing the sessions… [off]")});
        QVERIFY(sessions.isRefreshing());
        menu.popup(QPoint(10, 10));
        QVERIFY(waitFor([&] { return menu.actions().size() == 6; }));
        // No proposals anywhere: the wakers' order; the untitled session (nobody named it) under More.
        QCOMPARE(entries(menu), (QStringList{QStringLiteral("Thought Editor · example-server"), QStringLiteral("Busy one · example-server · busy"), QStringLiteral("Old work · example-server · closed"),
                                             QStringLiteral("Laptop session · macbook · offline"), QStringLiteral("---"),
                                             QStringLiteral("More Sessions > [Session 0f9e8d7c · example-server]")}));
        menu.close();
        // A click hands its session over; the one sent to last says so next time.
        menu.actions().at(1)->trigger();
        QVERIFY(clicked);
        QCOMPARE(clicked->sessionId, QStringLiteral("s3"));
        sessions.setLastSession(*clicked);
        QCOMPARE(sessions.lastSession(), QStringLiteral("s3"));
        QCOMPARE(sessions.lastSessionLabel(), QStringLiteral("Busy one · example-server"));
        sessions.populate(&menu, [](const Recipient &) {});
        // No tick (one session gets each send): its tip says it was sent to last.
        QCOMPARE(entries(menu).at(1), QStringLiteral("Busy one · example-server · busy"));
        QVERIFY(menu.actions().at(1)->toolTip().contains(QStringLiteral("Sent to last")));
        QVERIFY(!menu.actions().at(0)->toolTip().contains(QStringLiteral("Sent to last")));
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
    }
    {
        // The next run: the list as last fetched at once, also when the server cannot list it now.
        server.wakersSupported = false;
        AgentSessionList sessions(&center);
        QMenu menu;
        sessions.populate(&menu, [](const Recipient &) {});
        QCOMPARE(menu.actions().size(), 6);
        QCOMPARE(menu.actions().at(0)->text(), QStringLiteral("Thought Editor · example-server"));
        QVERIFY(menu.actions().at(1)->toolTip().contains(QStringLiteral("Sent to last")));
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        QVERIFY(!sessions.error().isEmpty());
        sessions.populate(&menu, [](const Recipient &) {});
        QCOMPARE(menu.actions().size(), 6);
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
    }
    {
        // Nothing kept and nothing to be had: why.
        QSettings().remove(QStringLiteral("AgentNotes/SendToSessions"));
        AgentSessionList sessions(&center);
        QMenu menu;
        sessions.populate(&menu, [](const Recipient &) {});
        menu.popup(QPoint(10, 10));
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        QVERIFY(waitFor([&] { return menu.actions().size() == 1 && menu.actions().first()->text().startsWith(QStringLiteral("No sessions: ")); }));
        QVERIFY(!menu.actions().first()->isEnabled());
        menu.close();
        // A server that lists none: none can be woken.
        server.wakersSupported = true;
        server.wakers = QJsonObject{{QStringLiteral("wakers"), QJsonArray()}};
        sessions.populate(&menu, [](const Recipient &) {});
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        sessions.populate(&menu, [](const Recipient &) {});
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        QCOMPARE(entries(menu), QStringList{QStringLiteral("No session can be woken now [off]")});
    }
    {
        // A server with its own agent (server_agents_v1; 27 Nov 2026, for someone without a
        // computer that runs a reply waker): after the computers' sessions, a new server session.
        server.capabilities = {QStringLiteral("server_agents_v1")};
        AgentSessionList sessions(&center);
        QMenu menu;
        std::optional<Recipient> clicked;
        const auto send = [&clicked](const Recipient &session) { clicked = session; };
        sessions.populate(&menu, send);
        QVERIFY(waitFor([&] { return !sessions.isRefreshing() && sessions.serverAgentsAvailable(); }));
        sessions.populate(&menu, send);
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        QCOMPARE(entries(menu), (QStringList{QStringLiteral("No session on a computer can be woken now [off]"), QStringLiteral("---"), QStringLiteral("Server agent: new session")}));
        menu.actions().last()->trigger();
        QVERIFY(clicked && AgentSessionList::isServerAgent(clicked->sessionId));
        // Its page starts a server session (server-agents.md §1), with no conversation of its own.
        Center::Page page;
        page.origin = QJsonObject{{QStringLiteral("copy_for_agent"), true}};
        AgentSessionList::preparePage(page, *clicked);
        QVERIFY(!page.noteId.isEmpty());
        QVERIFY(page.origin.value(QStringLiteral("start_session")).toBool());
        QCOMPARE(page.origin.value(QStringLiteral("where")).toString(), QStringLiteral("server"));
        QVERIFY(!page.origin.contains(QStringLiteral("conversation_id")));
        QVERIFY(page.origin.value(QStringLiteral("copy_for_agent")).toBool());
        // Sent: the session the server claimed it for is offered next and is the last session.
        server.handling.insert(page.noteId, QJsonObject{{QStringLiteral("claim"), QJsonObject{{QStringLiteral("conversation_id"), QStringLiteral("server-agent:5e55")}}}});
        sessions.pageSent(page, *clicked);
        QVERIFY(waitFor([&] { return sessions.lastSession() == QStringLiteral("server-agent:5e55"); }));
        QCOMPARE(sessions.lastSessionLabel(), QStringLiteral("Server agent · Thoughts server"));
        sessions.populate(&menu, send);
        QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
        QCOMPARE(entries(menu), (QStringList{QStringLiteral("No session on a computer can be woken now [off]"), QStringLiteral("---"), QStringLiteral("Server agent"),
                                             QStringLiteral("Server agent: new session")}));
        // A page to that session goes to its conversation (the server resumes it, §2).
        Center::Page next;
        AgentSessionList::preparePage(next, AgentSessionList::serverSession(QStringLiteral("server-agent:5e55")));
        QCOMPARE(next.origin.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("server-agent:5e55"));
        QVERIFY(!next.origin.contains(QStringLiteral("start_session")));
        // With the computers' sessions, after them.
        server.wakers = testWakers();
        sessions.populate(&menu, send);
        QVERIFY(waitFor([&] { return !sessions.isRefreshing() && !sessions.sessions().isEmpty(); }));
        sessions.populate(&menu, send);
        QCOMPARE(entries(menu).constLast(), QStringLiteral("Server agent: new session"));
        QCOMPARE(entries(menu).constFirst(), QStringLiteral("Thought Editor · example-server"));
        server.capabilities.clear();
    }
}

// Send to Agent's order: the sessions that wrote the most proposals on the document first, then
// the most in the whole Library, then the wakers' order; six shown, the session sent to last always
// among them; sessions nobody named (agent-3f, …) only with proposals on the document; the rest under
// More Sessions in the wakers' order. A proposal is on a document in its source, or for LaTeX in a
// file below its source's folder that is no other document's source.
void AgentNotesTest::sendToAgentRanking()
{
    const DocumentSources paper{QStringLiteral("docs-paper/overleaf/tex/source/main.tex"), {QStringLiteral("docs-paper/overleaf/tex/source/appendix-paper.tex")}};
    QVERIFY(belongsTo(QStringLiteral("docs-paper/overleaf/tex/source/main.tex"), paper));
    QVERIFY(belongsTo(QStringLiteral("docs-paper/overleaf/tex/source/sections/intro.tex"), paper));
    QVERIFY(!belongsTo(QStringLiteral("docs-paper/overleaf/tex/source/appendix-paper.tex"), paper));
    QVERIFY(!belongsTo(QStringLiteral("docs-paper/overleaf/tex/main.tex"), paper));
    QVERIFY(!belongsTo(QStringLiteral("docs-other/overleaf/tex/source/main.tex"), paper));
    const DocumentSources note{QStringLiteral("docs-notes/kl.md"), {}};
    QVERIFY(belongsTo(QStringLiteral("docs-notes/kl.md"), note));
    QVERIFY(!belongsTo(QStringLiteral("docs-notes/other.md"), note));
    QVERIFY(!belongsTo(QStringLiteral("docs-notes/kl.md"), DocumentSources{}));

    const auto session = [](const QString &id, const QString &title) { return Recipient{id, title, QStringLiteral("example-server"), QStringLiteral("idle"), true, {}}; };
    // The wakers' order.
    const QList<Recipient> sessions{session(QStringLiteral("a"), QStringLiteral("Alpha")), session(QStringLiteral("b"), QStringLiteral("Beta")),
                                    session(QStringLiteral("c"), QStringLiteral("agent-3f")), session(QStringLiteral("d"), QStringLiteral("Delta")),
                                    session(QStringLiteral("e"), QStringLiteral("Echo")), session(QStringLiteral("f"), QStringLiteral("Foxtrot")),
                                    session(QStringLiteral("g"), QStringLiteral("Golf")), session(QStringLiteral("h"), QStringLiteral("agent-9a")),
                                    session(QStringLiteral("i"), QStringLiteral("India"))};
    QList<ProposalAuthorship> proposals;
    const auto wrote = [&proposals](const QString &conversation, const QString &source, int count) {
        for (int index = 0; index < count; ++index) proposals.append({conversation, source});
    };
    wrote(QStringLiteral("d"), QStringLiteral("docs-paper/overleaf/tex/source/main.tex"), 2);
    wrote(QStringLiteral("d"), QStringLiteral("docs-other/main.tex"), 1);
    wrote(QStringLiteral("g"), QStringLiteral("docs-paper/overleaf/tex/source/sections/intro.tex"), 12);
    wrote(QStringLiteral("i"), QStringLiteral("docs-other/main.tex"), 40);
    wrote(QStringLiteral("b"), QStringLiteral("docs-other/main.tex"), 3);
    wrote(QStringLiteral("h"), QStringLiteral("docs-paper/overleaf/tex/source/main.tex"), 1);
    wrote(QStringLiteral("c"), QStringLiteral("docs-other/main.tex"), 50);
    wrote(QStringLiteral("unknown"), QStringLiteral("docs-paper/overleaf/tex/source/main.tex"), 5);
    const QHash<QString, ProposalCounts> counts = proposalCounts(proposals, paper);
    QCOMPARE(counts.value(QStringLiteral("g")).here, 12);
    QCOMPARE(counts.value(QStringLiteral("d")).here, 2);
    QCOMPARE(counts.value(QStringLiteral("d")).all, 3);
    QCOMPARE(counts.value(QStringLiteral("i")).here, 0);
    QCOMPARE(counts.value(QStringLiteral("i")).all, 40);
    QCOMPARE(countLabel(counts.value(QStringLiteral("g"))), QStringLiteral("— 12 here"));
    QCOMPARE(countLabel(counts.value(QStringLiteral("i"))), QStringLiteral("— 40 in all"));
    QCOMPARE(countLabel(counts.value(QStringLiteral("a"))), QString());
    QVERIFY(isAutoNamed(session(QStringLiteral("c"), QStringLiteral("agent-3f"))));
    QVERIFY(isAutoNamed(session(QStringLiteral("x"), QString())));
    QVERIFY(!isAutoNamed(session(QStringLiteral("a"), QStringLiteral("Alpha"))));

    const auto ids = [](const QList<Recipient> &list) {
        QStringList result;
        for (const Recipient &item : list) result.append(item.sessionId);
        return result;
    };
    // Proposals here first (g 12, d 2, h 1), then in all (i 40, b 3), then the wakers' order (a, e,
    // f); the session nobody named with none here (c) under More whatever its count.
    SessionMenu menu = sessionMenu(sessions, counts, QString());
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("g"), QStringLiteral("d"), QStringLiteral("h"), QStringLiteral("i"), QStringLiteral("b"), QStringLiteral("a")}));
    QCOMPARE(ids(menu.more), (QStringList{QStringLiteral("c"), QStringLiteral("e"), QStringLiteral("f")}));
    // The session sent to last, ranked lower: the sixth.
    menu = sessionMenu(sessions, counts, QStringLiteral("f"));
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("g"), QStringLiteral("d"), QStringLiteral("h"), QStringLiteral("i"), QStringLiteral("b"), QStringLiteral("f")}));
    QCOMPARE(ids(menu.more), (QStringList{QStringLiteral("a"), QStringLiteral("c"), QStringLiteral("e")}));
    // Already among them: as ranked.
    menu = sessionMenu(sessions, counts, QStringLiteral("d"));
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("g"), QStringLiteral("d"), QStringLiteral("h"), QStringLiteral("i"), QStringLiteral("b"), QStringLiteral("a")}));
    // No proposals at all: the wakers' order, six, sessions nobody named under More.
    menu = sessionMenu(sessions, {}, QString());
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f"), QStringLiteral("g")}));
    QCOMPARE(ids(menu.more), (QStringList{QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("i")}));
    // A document without proposals: by those in all.
    menu = sessionMenu(sessions, proposalCounts(proposals, note), QString());
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("i"), QStringLiteral("g"), QStringLiteral("b"), QStringLiteral("d"), QStringLiteral("a"), QStringLiteral("e")}));
    QVERIFY(ids(menu.more).contains(QStringLiteral("c")) && ids(menu.more).contains(QStringLiteral("h")));
    // Few sessions: all shown, nothing under More.
    menu = sessionMenu(sessions.mid(0, 2), counts, QString());
    QCOMPARE(ids(menu.shown), (QStringList{QStringLiteral("b"), QStringLiteral("a")}));
    QVERIFY(menu.more.isEmpty());
}

// The menu itself: the sessions in that order with their counts, More Sessions below; the counts
// that arrive while it is open order it again.
void AgentNotesTest::sendToAgentRankedMenu()
{
    FakeServer server;
    QJsonArray sessionsJson;
    for (const auto &[id, title, state] : std::initializer_list<std::tuple<QString, QString, QString>>{
             {QStringLiteral("a"), QStringLiteral("Thought Editor"), QStringLiteral("idle")}, {QStringLiteral("b"), QStringLiteral("Agent Study"), QStringLiteral("busy")},
             {QStringLiteral("c"), QStringLiteral("agent-3f"), QStringLiteral("idle")}, {QStringLiteral("d"), QStringLiteral("Paper revision"), QStringLiteral("idle")},
             {QStringLiteral("e"), QStringLiteral("Mental Model"), QStringLiteral("closed")}, {QStringLiteral("f"), QStringLiteral("Glasses watcher"), QStringLiteral("idle")},
             {QStringLiteral("g"), QStringLiteral("Data sweep"), QStringLiteral("idle")}, {QStringLiteral("h"), QStringLiteral("Overlays"), QStringLiteral("idle")}})
        sessionsJson.append(QJsonObject{{QStringLiteral("session_id"), id}, {QStringLiteral("title"), title}, {QStringLiteral("state"), state}});
    server.wakers = QJsonObject{{QStringLiteral("wakers"), QJsonArray{QJsonObject{{QStringLiteral("machine"), QStringLiteral("example-server")}, {QStringLiteral("live"), true},
                                                                                  {QStringLiteral("sessions"), sessionsJson}}}}};
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    const auto forget = [] {
        QSettings settings;
        for (const QString &key : {QStringLiteral("AgentNotes/SendToSessions"), QStringLiteral("AgentNotes/SendToSession"), QStringLiteral("AgentNotes/SendToSessionLabel")})
            settings.remove(key);
    };
    forget();
    const auto restore = qScopeGuard(forget);
    AgentSessionList sessions(&center);
    int loads = 0;
    sessions.setProposalLoader([&loads] { ++loads; });
    const DocumentSources paper{QStringLiteral("docs-paper/main.tex"), {}};
    QMenu menu;
    sessions.populate(&menu, [](const Recipient &) {}, true, [paper] { return paper; });
    QCOMPARE(loads, 1);
    menu.popup(QPoint(10, 10));
    QVERIFY(waitFor([&] { return !sessions.isRefreshing() && menu.actions().size() == 8; }));
    // No counts yet: the wakers' order (open sessions before closed ones), six; the session nobody named
    // under More.
    QCOMPARE(menuEntries(menu), (QStringList{QStringLiteral("Thought Editor · example-server"), QStringLiteral("Agent Study · example-server · busy"), QStringLiteral("Paper revision · example-server"),
                                             QStringLiteral("Glasses watcher · example-server"), QStringLiteral("Data sweep · example-server"), QStringLiteral("Overlays · example-server"),
                                             QStringLiteral("---"), QStringLiteral("More Sessions > [agent-3f · example-server, Mental Model · example-server · closed]")}));
    // The counts arrive while it is open: in their order now.
    QList<ProposalAuthorship> proposals;
    for (int index = 0; index < 12; ++index) proposals.append({QStringLiteral("d"), QStringLiteral("docs-paper/main.tex")});
    for (int index = 0; index < 40; ++index) proposals.append({QStringLiteral("h"), QStringLiteral("docs-other/main.tex")});
    proposals.append({QStringLiteral("c"), QStringLiteral("docs-paper/main.tex")});
    sessions.setProposals(proposals);
    QCOMPARE(menuEntries(menu), (QStringList{QStringLiteral("Paper revision · example-server — 12 here"), QStringLiteral("agent-3f · example-server — 1 here"), QStringLiteral("Overlays · example-server — 40 in all"),
                                             QStringLiteral("Thought Editor · example-server"), QStringLiteral("Agent Study · example-server · busy"), QStringLiteral("Glasses watcher · example-server"),
                                             QStringLiteral("---"), QStringLiteral("More Sessions > [Data sweep · example-server, Mental Model · example-server · closed]")}));
    const QString screenshots = qEnvironmentVariable("THOUGHTS_TEST_SCREENSHOTS");
    if (!screenshots.isEmpty()) QVERIFY(menu.grab().save(screenshots + QStringLiteral("/send-to-agent-ranked.png")));
    menu.close();
    // Each opening reads the proposals again.
    const int before = loads;
    QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
    QCOMPARE(loads, before + 1);
    QVERIFY(waitFor([&] { return !sessions.isRefreshing(); }));
}

// Send to Agent's page carries exactly what Copy for agent copies: a verbatim message goes as it is,
// to the last character; other pages' messages are trimmed as before.
void AgentNotesTest::copyForAgentMessageGoesAsItIs()
{
    FakeServer server;
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    Center::Page page;
    page.title = QStringLiteral("Copy for agent: AgentsVsNetwork · page 3");
    page.message = QStringLiteral("Thoughts document: \"AgentsVsNetwork\"\n  Selection: \"x\"\n");
    page.verbatim = true;
    page.page = 3;
    page.pages = 12;
    page.documentName = QStringLiteral("AgentsVsNetwork");
    page.image = QImage(200, 300, QImage::Format_RGB32);
    page.image.fill(Qt::white);
    page.origin = QJsonObject{{QStringLiteral("conversation_id"), QStringLiteral("s1")}, {QStringLiteral("copy_for_agent"), true}};
    bool sent = false;
    center.sendPage(page, [&sent](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("message")).toString(), page.message);
    QCOMPARE(server.bodies.last().value(QStringLiteral("origin")).toObject().value(QStringLiteral("copy_for_agent")), QJsonValue(true));
    QCOMPARE(server.bodies.last().value(QStringLiteral("origin")).toObject().value(QStringLiteral("conversation_id")).toString(), QStringLiteral("s1"));
    page.verbatim = false;
    page.noteId.clear();
    sent = false;
    center.sendPage(page, [&sent](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QCOMPARE(server.bodies.last().value(QStringLiteral("message")).toString(), page.message.trimmed());
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
    // A server before agent_note_answer_by_v1: the line, from the state it sends, without Change.
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
    auto *line = window.findChild<QWidget *>(QStringLiteral("agentNoteAnswerLine"));
    auto *text = window.findChild<QLabel *>(QStringLiteral("agentNoteAnswerText"));
    auto *change = window.findChild<QToolButton *>(QStringLiteral("agentNoteAnswerChange"));
    QVERIFY(line && text && change);
    QVERIFY(waitFor([&] { return line->isVisible() && text->text() == QStringLiteral("No agent is listening right now"); }));
    QVERIFY(line->property("warning").toBool());
    QCOMPARE(center.listening(QStringLiteral("n1")), Listening::NotListening);
    // The server was asked what it can do: without the capability there is no Change.
    QVERIFY(waitFor([&] { return server.requests.contains(QStringLiteral("GET /v1/health")); }));
    QTest::qWait(100);
    QVERIFY(!change->isVisible());
    // The bar over the reply field is gone: the same thing is never said twice.
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("agentNoteNotListening")));
    const auto saidListening = [&] {
        int count = 0;
        for (const QLabel *label : window.findChildren<QLabel *>()) {
            if (label->isVisible() && label->text().contains(QStringLiteral("listening"), Qt::CaseInsensitive)) ++count;
        }
        return count;
    };
    QCOMPARE(saidListening(), 1);

    // The agent starts listening: the next check says so.
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("listening"));
    center.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("An agent is listening"); }));
    QVERIFY(!line->property("warning").toBool());
    QCOMPARE(line->findChild<QLabel *>(QStringLiteral("agentNoteAnswerMark"))->property("mark").toString(), QStringLiteral("listening"));

    // The answer to a reply says whether it was heard.
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("not_listening"));
    center.setDraftMessage(QStringLiteral("n1"), QStringLiteral("Give me a new version"));
    bool sent = false;
    center.sendReply(QStringLiteral("n1"), [&](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QCOMPARE(center.listening(QStringLiteral("n1")), Listening::NotListening);
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("No agent is listening right now"); }));

    // A page the user sent is not asked about; a server that cannot tell (404) shows nothing.
    const int asked = server.requests.filter(QStringLiteral("/p1/listening")).size();
    window.showNote(QStringLiteral("p1"));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("p1"); }));
    QTest::qWait(200);
    QCOMPARE(server.requests.filter(QStringLiteral("/p1/listening")).size(), asked);
    QVERIFY(!line->isVisible());
    server.listening.remove(QStringLiteral("n1"));
    center.setListening(QStringLiteral("n1"), Listening::NotListening);
    window.showNote(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("n1"); }));
    QVERIFY(waitFor([&] { return center.listening(QStringLiteral("n1")) == Listening::Unknown; }));
    QVERIFY(waitFor([&] { return !line->isVisible(); }));
}

// Who answers, as the phone says it (AgentNoteCoverage.kt): the line's texts and the list behind Change.
void AgentNotesTest::answerLineSaysWhoListens()
{
    const QJsonObject sender = agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationA, {}, {}, false);
    // Listeners name who answers first: the sender's agent, a topic owner, the reply agent.
    // Candidates the view cannot name (an unknown role, no device) are left out.
    const ListeningAnswer parsed = ListeningAnswer::fromJson(
        {{QStringLiteral("state"), QStringLiteral("listening")},
         {QStringLiteral("listeners"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("responder")}, {QStringLiteral("device_id"), QStringLiteral("example-reply-host")}},
                                                   QJsonObject{{QStringLiteral("role"), QStringLiteral("owner")}, {QStringLiteral("device_id"), QStringLiteral("example-laptop")},
                                                               {QStringLiteral("owner_id"), TopicOwner}, {QStringLiteral("label"), QStringLiteral("Example topic")}}}},
         {QStringLiteral("answer_by"), QJsonValue()},
         {QStringLiteral("candidates"), QJsonArray{replyAgentRecord(), topicAgentRecord(),
                                                   QJsonObject{{QStringLiteral("role"), QStringLiteral("observer")}, {QStringLiteral("device_id"), QStringLiteral("example-laptop")}},
                                                   QJsonObject{{QStringLiteral("role"), QStringLiteral("sender")}}}}});
    QCOMPARE(parsed.state, Listening::Listening);
    QVERIFY(parsed.coverer);
    QCOMPARE(parsed.coverer->role, QStringLiteral("owner"));
    QVERIFY(!parsed.answerBy);
    QCOMPARE(parsed.candidates.size(), 2);
    QCOMPARE(parsed.candidates.at(1).label, QStringLiteral("Example topic"));
    QVERIFY(parsed.candidates.at(1).live);
    // A listener of a server before topic owners: "responder": true is the reply agent.
    const ListeningAnswer older = ListeningAnswer::fromJson(
        {{QStringLiteral("state"), QStringLiteral("listening")}, {QStringLiteral("listeners"), QJsonArray{QJsonObject{{QStringLiteral("responder"), true}}}}});
    QVERIFY(older.coverer);
    QCOMPARE(older.coverer->role, QStringLiteral("responder"));

    // No line before an answer, nor while the server cannot tell.
    QVERIFY(!answerLine(std::nullopt, true, true));
    QVERIFY(!answerLine(ListeningAnswer{}, true, true));

    auto line = answerLine(parsed, false, true);
    QVERIFY(line);
    QCOMPARE(line->text, QStringLiteral("Example topic agent is listening"));
    QCOMPARE(line->mark, AnswerLine::Mark::Listening);
    QVERIFY(!line->warning);
    QVERIFY(!line->changeOffered);
    QCOMPARE(answerLine(older, true, true)->text, QStringLiteral("Thoughts reply agent is listening"));
    ListeningAnswer anyone;
    anyone.state = Listening::Listening;
    QCOMPARE(answerLine(anyone, true, true)->text, QStringLiteral("An agent is listening"));

    ListeningAnswer nobody;
    nobody.state = Listening::NotListening;
    line = answerLine(nobody, true, true);
    QCOMPARE(line->text, QStringLiteral("No agent is listening right now"));
    QCOMPARE(line->mark, AnswerLine::Mark::Nobody);
    QVERIFY(line->warning);
    QVERIFY(line->changeOffered);
    QVERIFY(line->changeEnabled);
    // Change needs the server.
    line = answerLine(nobody, true, false);
    QVERIFY(line->changeOffered);
    QVERIFY(!line->changeEnabled);

    // A chosen agent that is not listening: the reply agent answers after a while.
    ListeningAnswer waiting = nobody;
    waiting.answerBy = Agent::fromJson(sender);
    waiting.candidates = {*Agent::fromJson(replyAgentRecord())};
    line = answerLine(waiting, true, true);
    QCOMPARE(line->text, QStringLiteral("The agent that sent it is not listening right now. The Thoughts reply agent answers after 10 minutes."));
    QCOMPARE(line->mark, AnswerLine::Mark::Waiting);
    QVERIFY(!line->warning);
    // A chosen agent that is live: the server says listening, and the line names the choice.
    ListeningAnswer chosen;
    chosen.state = Listening::Listening;
    chosen.coverer = Agent::fromJson(topicAgentRecord());
    chosen.answerBy = Agent::fromJson(replyAgentRecord());
    QCOMPARE(answerLine(chosen, true, true)->text, QStringLiteral("Thoughts reply agent is listening"));

    // Change: Automatic with who answers then, and each agent by the line's name; the choice checked.
    QList<AnswerChoice> choices = answerChoices(parsed);
    QCOMPARE(choices.size(), 3);
    QCOMPARE(choices.at(0).title, QStringLiteral("Automatic"));
    QCOMPARE(choices.at(0).detail, QStringLiteral("The agent that sent the note answers first, then the Thoughts reply agent"));
    QVERIFY(!choices.at(0).agent);
    QVERIFY(choices.at(0).checked);
    QCOMPARE(choices.at(1).title, QStringLiteral("Thoughts reply agent"));
    QCOMPARE(choices.at(2).title, QStringLiteral("Example topic agent"));
    QVERIFY(!choices.at(1).checked && !choices.at(2).checked);
    QVERIFY(choices.at(1).detail.isEmpty());
    // Two agents that would read the same are numbered; the same agent twice is listed once.
    ListeningAnswer two;
    two.state = Listening::Listening;
    const QJsonObject other = agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationB);
    two.candidates = {*Agent::fromJson(agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationA)), *Agent::fromJson(other), *Agent::fromJson(other)};
    two.answerBy = Agent::fromJson(other);
    choices = answerChoices(two);
    QCOMPARE(choices.size(), 3);
    QCOMPARE(choices.at(1).title, QStringLiteral("The agent that sent it"));
    QCOMPARE(choices.at(2).title, QStringLiteral("The agent that sent it (2)"));
    QVERIFY(!choices.at(0).checked && !choices.at(1).checked && choices.at(2).checked);
    // What is chosen is listed also when it could not answer now.
    choices = answerChoices(waiting);
    QCOMPARE(choices.size(), 3);
    QCOMPARE(choices.last().title, QStringLiteral("The agent that sent it (not listening)"));
    QVERIFY(choices.last().checked);
    QVERIFY(!choices.first().checked);

    // The PUT names the agent by its identity only.
    QCOMPARE(Agent::fromJson(replyAgentRecord())->toChoiceJson(), choiceOf(replyAgentRecord()));
    QCOMPARE(Agent::fromJson(topicAgentRecord())->toChoiceJson().value(QStringLiteral("owner_id")).toString(), TopicOwner);
    QVERIFY(!Agent::fromJson(topicAgentRecord())->toChoiceJson().contains(QStringLiteral("label")));
}

void AgentNotesTest::clientChoosesWhoAnswers()
{
    FakeServer server;
    server.capabilities = {QStringLiteral("agent_notes_v1"), answerByCapability()};
    server.answers.insert(QStringLiteral("n1"), QJsonObject{{QStringLiteral("note_id"), QStringLiteral("n1")},
                                                            {QStringLiteral("state"), QStringLiteral("not_listening")},
                                                            {QStringLiteral("listeners"), QJsonArray()},
                                                            {QStringLiteral("answer_by"), QJsonValue()},
                                                            {QStringLiteral("candidates"), QJsonArray{replyAgentRecord()}}});
    Client client(cachePath());
    client.setServer(server.url(), "secret");

    Client::WriteResult result;
    bool done = false;
    client.setAnswerBy(QStringLiteral("n1"), Agent::fromJson(replyAgentRecord()), &client, [&](const Client::WriteResult &answer) {
        result = answer;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QCOMPARE(result.status, 200);
    QCOMPARE(result.failure, Client::Failure::None);
    QVERIFY(server.requests.contains(QStringLiteral("PUT /v1/agent-notes/n1/answer-by")));
    QCOMPARE(server.bodies.last(), (QJsonObject{{QStringLiteral("agent"), choiceOf(replyAgentRecord())}}));
    const ListeningAnswer answered = ListeningAnswer::fromJson(result.json);
    QVERIFY(answered.answerBy);
    QCOMPARE(answered.answerBy->role, QStringLiteral("responder"));
    QCOMPARE(answered.state, Listening::Listening);

    // Automatic: {"agent": null}.
    done = false;
    client.setAnswerBy(QStringLiteral("n1"), std::nullopt, &client, [&](const Client::WriteResult &answer) {
        result = answer;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QCOMPARE(result.status, 200);
    QCOMPARE(server.bodies.last(), (QJsonObject{{QStringLiteral("agent"), QJsonValue()}}));
    QVERIFY(server.bodies.last().value(QStringLiteral("agent")).isNull());
    QVERIFY(!ListeningAnswer::fromJson(result.json).answerBy);

    // An agent the server does not know: 409 with its error_code.
    done = false;
    client.setAnswerBy(QStringLiteral("n1"), Agent::fromJson(topicAgentRecord()), &client, [&](const Client::WriteResult &answer) {
        result = answer;
        done = true;
    });
    QVERIFY(waitFor([&] { return done; }));
    QCOMPARE(result.status, 409);
    QCOMPARE(result.failure, Client::Failure::Failed);
    QCOMPARE(result.errorCode, QStringLiteral("agent_note_answerer_unknown"));

    // What the server can do.
    bool known = false;
    QStringList capabilities;
    client.capabilities(&client, [&](bool ok, const QStringList &names) {
        known = ok;
        capabilities = names;
        done = true;
    });
    QVERIFY(waitFor([&] { return known; }));
    QVERIFY(capabilities.contains(answerByCapability()));
}

void AgentNotesTest::centerAsksWhoListensOncePerMinute()
{
    FakeServer server;
    server.listening.insert(QStringLiteral("n1"), QStringLiteral("listening"));
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    QDateTime now(QDate(2026, 11, 14), QTime(9, 0), QTimeZone::UTC);
    Center center(&client, statePath(), [&] { return now; });
    const auto asked = [&] { return server.requests.filter(QStringLiteral("/n1/listening")).size(); };
    center.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return center.listening(QStringLiteral("n1")) == Listening::Listening; }));
    QCOMPARE(asked(), 1);
    // Another view asked half a minute ago: not again.
    now = now.addSecs(30);
    center.checkListeningIfOlder(QStringLiteral("n1"), AgentNotesView::ListeningRefreshMs - 5000);
    QTest::qWait(150);
    QCOMPARE(asked(), 1);
    now = now.addSecs(31);
    center.checkListeningIfOlder(QStringLiteral("n1"), AgentNotesView::ListeningRefreshMs - 5000);
    QVERIFY(waitFor([&] { return asked() == 2; }));
    QVERIFY(center.serverReachable());

    // A server that cannot be reached: Change waits for it.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();
    Client away(cachePath());
    away.setServer(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)), "secret");
    Center unreachable(&away, statePath());
    unreachable.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return !unreachable.serverReachable(); }));
    QVERIFY(!unreachable.answer(QStringLiteral("n1")));
}

// Who will be woken, as the phone says it (AgentNoteCoverage.kt): reply wakers ("Wakers" in protocol/agent-notes.md).
void AgentNotesTest::answerLineSaysWhoWillBeWoken()
{
    const QString woken = QStringLiteral("will_be_woken");
    const QString offline = QStringLiteral("waker_offline");
    const auto withWake = [](const QString &state, const QJsonObject &wake, const QJsonArray &listeners = {}) {
        return ListeningAnswer::fromJson({{QStringLiteral("state"), state}, {QStringLiteral("listeners"), listeners}, {QStringLiteral("wake"), wake}});
    };
    // The server's own waker listens as the reply agent, so older apps still see an agent listening.
    const QJsonArray responder{QJsonObject{{QStringLiteral("role"), QStringLiteral("responder")}, {QStringLiteral("device_id"), QStringLiteral("example-reply-host")}}};
    const ListeningAnswer mac = withWake(QStringLiteral("listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("  Chapter draft ")), responder);
    QVERIFY(mac.wake);
    QCOMPARE(mac.wake->title, QStringLiteral("Chapter draft"));
    auto line = answerLine(mac, true, true);
    QVERIFY(line);
    QCOMPARE(line->text, QStringLiteral("Chapter draft will be woken on the Mac"));
    QCOMPARE(line->mark, AnswerLine::Mark::Woken);
    QVERIFY(!line->warning);
    QVERIFY(!line->muted);
    QVERIFY(line->changeOffered && line->changeEnabled);
    // Nobody listens at all: not "No agent is listening", since the session will be woken.
    line = answerLine(withWake(QStringLiteral("not_listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("example-server"), QStringLiteral("Figure review"), QStringLiteral("idle"))), true, true);
    QCOMPARE(line->text, QStringLiteral("Figure review will be woken on the server"));
    QVERIFY(!line->warning);
    // The fallback session is named without its machine; a session without a title as its agent.
    QCOMPARE(answerLine(withWake(QStringLiteral("listening"), wakeRecord(woken, QStringLiteral("responder"), QStringLiteral("example-server"), QStringLiteral("The study session"), QStringLiteral("idle")), responder), true, true)->text,
             QStringLiteral("The study session will be woken"));
    QCOMPARE(answerLine(withWake(QStringLiteral("not_listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), {})), true, true)->text,
             QStringLiteral("The agent that sent it will be woken on the Mac"));
    // A session busy with a task answers when it is done.
    line = answerLine(withWake(QStringLiteral("listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"), QStringLiteral("busy")), responder), true, true);
    QCOMPARE(line->text, QStringLiteral("Chapter draft answers when it finishes its current task"));
    QCOMPARE(line->mark, AnswerLine::Mark::Woken);

    // An offline machine: when the study session answers instead, at the local time, in the muted look.
    const QDateTime fallbackAt = QDateTime::fromString(QStringLiteral("2026-11-15T02:14:00Z"), Qt::ISODate);
    QVERIFY(fallbackAt.isValid());
    const QString at = QLocale::system().toString(fallbackAt.toLocalTime().time(), QLocale::ShortFormat);
    line = answerLine(withWake(QStringLiteral("listening"), wakeRecord(offline, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"), QStringLiteral("closed"),
                                                                       QStringLiteral("2026-11-15T02:14:00.123456Z")), responder), true, true);
    QCOMPARE(line->text, QStringLiteral("The Mac is offline; the study session answers at %1 if it is still offline").arg(at));
    QCOMPARE(line->mark, AnswerLine::Mark::Offline);
    QVERIFY(line->muted);
    QVERIFY(!line->warning);
    QVERIFY(line->changeOffered);
    QCOMPARE(answerLine(withWake(QStringLiteral("not_listening"), wakeRecord(offline, QStringLiteral("sender"), QStringLiteral("example-server"), QStringLiteral("Figure review"), QStringLiteral("closed"),
                                                                     QStringLiteral("2026-11-15T02:14:00Z"))), true, true)->text,
             QStringLiteral("The server is offline; the study session answers at %1 if it is still offline").arg(at));
    QCOMPARE(answerLine(withWake(QStringLiteral("not_listening"), wakeRecord(offline, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"))), true, true)->text,
             QStringLiteral("The Mac is offline; the study session answers later if it is still offline"));

    // An agent that listens by itself is still named first.
    const QJsonArray sender{QJsonObject{{QStringLiteral("role"), QStringLiteral("sender")}, {QStringLiteral("device_id"), QStringLiteral("example-laptop")}}};
    QCOMPARE(answerLine(withWake(QStringLiteral("listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft")), sender), true, true)->text,
             QStringLiteral("The agent that sent it is listening"));
    // A chosen agent a waker wakes: named instead of the ten minutes.
    ListeningAnswer chosen = withWake(QStringLiteral("not_listening"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft")));
    chosen.answerBy = Agent::fromJson(agentRecord(QStringLiteral("sender"), QStringLiteral("example-laptop"), ConversationA, {}, {}, false));
    QCOMPARE(answerLine(chosen, true, true)->text, QStringLiteral("Chapter draft will be woken on the Mac"));
    // A server that cannot tell who listens, but names a session to wake.
    QCOMPARE(answerLine(withWake(QStringLiteral("unknown"), wakeRecord(woken, QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"))), true, true)->text,
             QStringLiteral("Chapter draft will be woken on the Mac"));

    // An older server has no wake, and one the view cannot say is none: the lines as before.
    const ListeningAnswer older = ListeningAnswer::fromJson({{QStringLiteral("state"), QStringLiteral("not_listening")}, {QStringLiteral("listeners"), QJsonArray()}});
    QVERIFY(!older.wake);
    QCOMPARE(answerLine(older, true, true)->text, QStringLiteral("No agent is listening right now"));
    QVERIFY(!withWake(QStringLiteral("not_listening"), wakeRecord(QStringLiteral("something new"), QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("x"))).wake);
    QVERIFY(!withWake(QStringLiteral("not_listening"), wakeRecord(woken, QStringLiteral("observer"), QStringLiteral("mac"), QStringLiteral("x"))).wake);
    QVERIFY(!withWake(QStringLiteral("not_listening"), wakeRecord(woken, QStringLiteral("sender"), {}, QStringLiteral("x"))).wake);
    QVERIFY(!answerLine(ListeningAnswer{}, true, true));

    // After Send.
    QCOMPARE(mac.wake->sentText(), QStringLiteral("Sent ✓ · Chapter draft will be woken"));
}

void AgentNotesTest::viewSaysWhoWillBeWoken()
{
    FakeServer server;
    server.capabilities = {QStringLiteral("agent_notes_v1"), answerByCapability()};
    const QByteArray picture = png(QSize(200, 500), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonObject agentNote = noteRecord(QStringLiteral("n1"), 7, now.addSecs(-300), QStringLiteral("New drafter"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.notes = {agentNote};
    const QJsonArray responder{QJsonObject{{QStringLiteral("role"), QStringLiteral("responder")}, {QStringLiteral("device_id"), QStringLiteral("example-reply-host")}}};
    QJsonObject answer{{QStringLiteral("note_id"), QStringLiteral("n1")},
                       {QStringLiteral("state"), QStringLiteral("listening")},
                       {QStringLiteral("listeners"), responder},
                       {QStringLiteral("answer_by"), QJsonValue()},
                       {QStringLiteral("candidates"), QJsonArray{replyAgentRecord()}},
                       {QStringLiteral("wake"), wakeRecord(QStringLiteral("will_be_woken"), QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"))}};
    server.answers.insert(QStringLiteral("n1"), answer);
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.apply({note(agentNote)});

    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.showNote(QStringLiteral("n1"));
    auto *line = window.findChild<QWidget *>(QStringLiteral("agentNoteAnswerLine"));
    auto *text = window.findChild<QLabel *>(QStringLiteral("agentNoteAnswerText"));
    auto *mark = window.findChild<QLabel *>(QStringLiteral("agentNoteAnswerMark"));
    auto *change = window.findChild<QToolButton *>(QStringLiteral("agentNoteAnswerChange"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("agentNoteReplyStatus"));
    QVERIFY(line && text && mark && change && status);
    QVERIFY(waitFor([&] { return line->isVisible() && text->text() == QStringLiteral("Chapter draft will be woken on the Mac"); }));
    QVERIFY(!line->property("warning").toBool());
    QCOMPARE(mark->property("mark").toString(), QStringLiteral("woken"));
    QCOMPARE(mark->accessibleName(), QStringLiteral("Will be woken"));
    QVERIFY(waitFor([&] { return change->isVisible() && change->isEnabled(); }));

    // The reply's answer names the session too: the reply box says so after Send.
    center.setDraftMessage(QStringLiteral("n1"), QStringLiteral("Give me a new version"));
    bool sent = false;
    center.sendReply(QStringLiteral("n1"), [&](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QVERIFY(waitFor([&] { return status->isVisible() && status->text() == QStringLiteral("Sent ✓ · Chapter draft will be woken"); }));
    QCOMPARE(center.sentStatus(QStringLiteral("n1")), QStringLiteral("Sent ✓ · Chapter draft will be woken"));
    // The next reply started: the status is about that one.
    center.setDraftMessage(QStringLiteral("n1"), QStringLiteral("And another"));
    QVERIFY(waitFor([&] { return !status->isVisible(); }));
    QVERIFY(center.sentStatus(QStringLiteral("n1")).isEmpty());

    // The Mac goes to sleep: the line says when the study session answers, muted.
    answer.insert(QStringLiteral("wake"), wakeRecord(QStringLiteral("waker_offline"), QStringLiteral("sender"), QStringLiteral("mac"), QStringLiteral("Chapter draft"),
                                                    QStringLiteral("closed"), QStringLiteral("2026-11-15T02:14:00Z")));
    server.answers.insert(QStringLiteral("n1"), answer);
    center.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return text->text().startsWith(QStringLiteral("The Mac is offline; the study session answers at ")); }));
    QCOMPARE(mark->property("mark").toString(), QStringLiteral("offline"));
    QVERIFY(!line->property("warning").toBool());
    QVERIFY(change->isVisible());

    // A reply without a wake (no waker would wake anyone): as before, no status after Send.
    answer.remove(QStringLiteral("wake"));
    server.answers.insert(QStringLiteral("n1"), answer);
    sent = false;
    center.sendReply(QStringLiteral("n1"), [&](bool ok, const QString &) { sent = ok; });
    QVERIFY(waitFor([&] { return sent; }));
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("Thoughts reply agent is listening"); }));
    QVERIFY(center.sentStatus(QStringLiteral("n1")).isEmpty());
    QVERIFY(!status->isVisible());
}

void AgentNotesTest::viewLetsTheUserChooseWhoAnswers()
{
    FakeServer server;
    server.capabilities = {QStringLiteral("agent_notes_v1"), answerByCapability()};
    const QByteArray picture = png(QSize(200, 500), QColor(30, 90, 200));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QJsonObject agentNote = noteRecord(QStringLiteral("n1"), 7, now.addSecs(-300), QStringLiteral("New drafter"), picture);
    QJsonObject page = noteRecord(QStringLiteral("p1"), 6, now.addSecs(-400), QStringLiteral("Notes · page 2"), picture);
    page.insert(QStringLiteral("origin"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("phone_page")}});
    server.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    server.files.insert(QStringLiteral("/v1/agent-notes/p1/image"), picture);
    server.notes = {agentNote, page};
    server.answers.insert(QStringLiteral("n1"), QJsonObject{{QStringLiteral("note_id"), QStringLiteral("n1")},
                                                            {QStringLiteral("state"), QStringLiteral("not_listening")},
                                                            {QStringLiteral("listeners"), QJsonArray()},
                                                            {QStringLiteral("answer_by"), QJsonValue()},
                                                            {QStringLiteral("candidates"), QJsonArray{replyAgentRecord(), topicAgentRecord()}}});
    Client client(cachePath());
    client.setServer(server.url(), "secret");
    Center center(&client, statePath());
    center.apply({note(agentNote), note(page)});

    AgentNotesWindow window(&center, {}, {}, {});
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.showNote(QStringLiteral("n1"));
    auto *line = window.findChild<QWidget *>(QStringLiteral("agentNoteAnswerLine"));
    auto *text = window.findChild<QLabel *>(QStringLiteral("agentNoteAnswerText"));
    auto *change = window.findChild<QToolButton *>(QStringLiteral("agentNoteAnswerChange"));
    auto *failure = window.findChild<QLabel *>(QStringLiteral("agentNoteAnswerFailure"));
    QVERIFY(line && text && change && failure);
    QVERIFY(waitFor([&] { return line->isVisible() && text->text() == QStringLiteral("No agent is listening right now"); }));
    QVERIFY(line->property("warning").toBool());
    // Change once the server said it has the capability (asked with the first check).
    QVERIFY(waitFor([&] { return change->isVisible() && change->isEnabled(); }));
    QCOMPARE(change->text(), QStringLiteral("Change"));
    // Always in view: over the note, not part of what scrolls.
    auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("agentNoteScroll"));
    QVERIFY(scroll);
    QVERIFY(!scroll->isAncestorOf(line));
    QVERIFY(line->geometry().bottom() <= window.findChild<QWidget *>(QStringLiteral("agentNotePane"))->mapFromGlobal(scroll->mapToGlobal(QPoint(0, 0))).y());
    // It is refreshed every minute while the note is on screen.
    auto *refresh = window.findChild<QTimer *>(QStringLiteral("agentNoteListeningTimer"));
    QVERIFY(refresh && refresh->isActive());
    QCOMPARE(refresh->interval(), 60 * 1000);

    // Change: Automatic with who answers then, and each agent that could answer now.
    {
        std::unique_ptr<QMenu> menu(window.createAnswerMenu());
        QVERIFY(menu);
        QCOMPARE(actionTitles(menu.get()), (QStringList{QStringLiteral("Who answers your reply"), QStringLiteral("Automatic"),
                                                       QStringLiteral("The agent that sent the note answers first, then the Thoughts reply agent"),
                                                       QStringLiteral("Thoughts reply agent"), QStringLiteral("Example topic agent")}));
        QVERIFY(actionTitled(menu.get(), QStringLiteral("Automatic"))->isChecked());
        QVERIFY(!actionTitled(menu.get(), QStringLiteral("Example topic agent"))->isChecked());
        QVERIFY(!actionTitled(menu.get(), QStringLiteral("The agent that sent the note answers first, then the Thoughts reply agent"))->isEnabled());
        actionTitled(menu.get(), QStringLiteral("Example topic agent"))->trigger();
    }
    // The choice goes to the server; the line follows its answer.
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("Example topic agent is listening"); }));
    QVERIFY(server.requests.contains(QStringLiteral("PUT /v1/agent-notes/n1/answer-by")));
    QCOMPARE(server.bodies.last(), (QJsonObject{{QStringLiteral("agent"), choiceOf(topicAgentRecord())}}));
    QVERIFY(!line->property("warning").toBool());
    QVERIFY(center.answer(QStringLiteral("n1"))->answerBy);
    QCOMPARE(center.answer(QStringLiteral("n1"))->answerBy->role, QStringLiteral("owner"));
    QVERIFY(change->isEnabled());
    QVERIFY(!failure->isVisible());

    // The chosen agent stops listening: the reply agent answers later, and the list still shows the choice.
    QJsonObject gone = server.answers.value(QStringLiteral("n1"));
    gone.insert(QStringLiteral("state"), QStringLiteral("not_listening"));
    gone.insert(QStringLiteral("answer_by"), topicAgentRecord(false));
    gone.insert(QStringLiteral("candidates"), QJsonArray{replyAgentRecord()});
    server.answers.insert(QStringLiteral("n1"), gone);
    center.checkListening(QStringLiteral("n1"));
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("Example topic agent is not listening right now. The Thoughts reply agent answers after 10 minutes."); }));
    QVERIFY(!line->property("warning").toBool());
    QCOMPARE(line->findChild<QLabel *>(QStringLiteral("agentNoteAnswerMark"))->property("mark").toString(), QStringLiteral("waiting"));
    {
        std::unique_ptr<QMenu> menu(window.createAnswerMenu());
        QCOMPARE(actionTitles(menu.get()).last(), QStringLiteral("Example topic agent (not listening)"));
        QVERIFY(actionTitled(menu.get(), QStringLiteral("Example topic agent (not listening)"))->isChecked());
        QVERIFY(!actionTitled(menu.get(), QStringLiteral("Automatic"))->isChecked());
        // The server refuses: the user is told, and the choice stays.
        server.putStatus = 409;
        actionTitled(menu.get(), QStringLiteral("Automatic"))->trigger();
    }
    QVERIFY(waitFor([&] { return failure->isVisible(); }));
    QCOMPARE(failure->text(), QStringLiteral("The server no longer knows that agent. The choice stays as it was."));
    QCOMPARE(text->text(), QStringLiteral("Example topic agent is not listening right now. The Thoughts reply agent answers after 10 minutes."));
    QCOMPARE(center.answer(QStringLiteral("n1"))->answerBy->role, QStringLiteral("owner"));
    server.putStatus = 500;
    {
        std::unique_ptr<QMenu> menu(window.createAnswerMenu());
        actionTitled(menu.get(), QStringLiteral("Thoughts reply agent"))->trigger();
    }
    QVERIFY(waitFor([&] { return failure->isVisible() && failure->text() == QStringLiteral("The server did not save the choice. It stays as it was."); }));
    QCOMPARE(center.answer(QStringLiteral("n1"))->answerBy->role, QStringLiteral("owner"));
    QVERIFY(change->isEnabled());

    // Back to automatic: nobody listens now, and the failure is no longer said.
    server.putStatus = 0;
    {
        std::unique_ptr<QMenu> menu(window.createAnswerMenu());
        actionTitled(menu.get(), QStringLiteral("Automatic"))->trigger();
    }
    QVERIFY(waitFor([&] { return text->text() == QStringLiteral("No agent is listening right now"); }));
    QVERIFY(server.bodies.last().value(QStringLiteral("agent")).isNull());
    QVERIFY(!center.answer(QStringLiteral("n1"))->answerBy);
    QVERIFY(!failure->isVisible());

    // The reader's sidebar has the same line, over its note too.
    AgentNotesView sidebar(AgentNotesView::Layout::Sidebar, &center, {}, {}, {});
    sidebar.resize(320, 800);
    sidebar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sidebar));
    sidebar.selectNote(QStringLiteral("n1"));
    auto *sidebarLine = sidebar.findChild<QWidget *>(QStringLiteral("agentNoteAnswerLine"));
    QVERIFY(waitFor([&] { return sidebarLine->isVisible(); }));
    QCOMPARE(sidebar.findChild<QLabel *>(QStringLiteral("agentNoteAnswerText"))->text(), QStringLiteral("No agent is listening right now"));
    QVERIFY(!sidebar.findChild<QScrollArea *>(QStringLiteral("agentNoteScroll"))->isAncestorOf(sidebarLine));
    QVERIFY(sidebar.findChild<QToolButton *>(QStringLiteral("agentNoteAnswerChange"))->isVisible());
    sidebar.showList();
    QVERIFY(!sidebarLine->isVisible());

    // A page the user sent has no line, and is not asked about every minute.
    window.showNote(QStringLiteral("p1"));
    QVERIFY(waitFor([&] { return window.shownNoteId() == QStringLiteral("p1"); }));
    QVERIFY(!line->isVisible());
    QVERIFY(!refresh->isActive());

    // A server without the capability: the line without Change.
    FakeServer older;
    older.notes = {agentNote};
    older.files.insert(QStringLiteral("/v1/agent-notes/n1/image"), picture);
    older.answers.insert(QStringLiteral("n1"), QJsonObject{{QStringLiteral("note_id"), QStringLiteral("n1")},
                                                           {QStringLiteral("state"), QStringLiteral("listening")},
                                                           {QStringLiteral("listeners"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("responder")},
                                                                                                                {QStringLiteral("device_id"), QStringLiteral("example-reply-host")}}}}});
    Client olderClient(cachePath());
    olderClient.setServer(older.url(), "secret");
    Center olderCenter(&olderClient, statePath());
    olderCenter.apply({note(agentNote)});
    AgentNotesWindow olderWindow(&olderCenter, {}, {}, {});
    olderWindow.resize(1200, 800);
    olderWindow.show();
    QVERIFY(QTest::qWaitForWindowExposed(&olderWindow));
    olderWindow.showNote(QStringLiteral("n1"));
    auto *olderText = olderWindow.findChild<QLabel *>(QStringLiteral("agentNoteAnswerText"));
    QVERIFY(waitFor([&] { return olderText->isVisible() && olderText->text() == QStringLiteral("Thoughts reply agent is listening"); }));
    QVERIFY(waitFor([&] { return older.requests.contains(QStringLiteral("GET /v1/health")); }));
    QTest::qWait(100);
    QVERIFY(!olderWindow.findChild<QToolButton *>(QStringLiteral("agentNoteAnswerChange"))->isVisible());
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
