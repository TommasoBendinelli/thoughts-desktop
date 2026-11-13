// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnotes.h"

#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"
#include "developerlog.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>

namespace Thoughts::AgentNotes
{

namespace
{
QDateTime parseTime(const QJsonValue &value)
{
    const QString text = value.toString();
    if (text.isEmpty()) return {};
    QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::fromString(text, Qt::ISODate);
    return time.isValid() ? time.toUTC() : QDateTime();
}

int intValue(const QJsonObject &json, const char *key)
{
    return int(json.value(QLatin1String(key)).toDouble());
}

bool validSha256(const QString &sha)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(sha).hasMatch();
}

QString extensionFor(const QString &mediaType)
{
    return mediaType.compare(QLatin1String("image/jpeg"), Qt::CaseInsensitive) == 0 ? QStringLiteral("jpg") : QStringLiteral("png");
}

// image_url of a record: a path on this server below /v1/agent-notes/, nothing else.
bool validImagePath(const QString &url)
{
    const QUrl parsed(url);
    return parsed.isRelative() && parsed.host().isEmpty() && parsed.path().startsWith(QLatin1String("/v1/agent-notes/")) && !parsed.path().contains(QLatin1String(".."));
}

// What the window shows of the notes: a poll that changes none of it repaints nothing.
QByteArray signatureOf(const QList<Note> &notes)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const QString &text) {
        hash.addData(text.toUtf8());
        hash.addData(QByteArrayView("\x1f", 1));
    };
    for (const Note &note : notes) {
        for (const QString &field : {note.id, QString::number(note.sequence), note.title, note.message, note.imageSha256, note.documentId,
                                     note.documentFilename, note.reviewCardId, note.reviewQuestion, note.senderDeviceId,
                                     note.openedAt.toString(Qt::ISODateWithMs), QString::fromUtf8(QJsonDocument(note.origin).toJson(QJsonDocument::Compact))}) {
            add(field);
        }
        for (const Reply &reply : note.replies) {
            for (const QString &field : {reply.id, reply.message, reply.imageSha256, QString::number(reply.strokeCount), reply.photoSha256}) add(field);
        }
        hash.addData(QByteArrayView("\x1e", 1));
    }
    return hash.result();
}

constexpr int SeenLimit = 5000;
constexpr int DecidedLimit = 2000;
constexpr int CachedImageLimit = 400;
constexpr int MaxAttentionPerPass = 3;
}

// ---------------------------------------------------------------- records

std::optional<Reply> Reply::fromJson(const QJsonObject &json)
{
    Reply reply;
    reply.id = json.value(QLatin1String("reply_id")).toString();
    if (reply.id.isEmpty()) return std::nullopt;
    reply.sequence = qint64(json.value(QLatin1String("sequence")).toDouble());
    reply.deviceId = json.value(QLatin1String("device_id")).toString();
    reply.createdAt = parseTime(json.value(QLatin1String("created_at")));
    reply.mediaType = json.value(QLatin1String("media_type")).toString(QStringLiteral("image/png"));
    reply.imageWidth = intValue(json, "image_width");
    reply.imageHeight = intValue(json, "image_height");
    reply.imageSha256 = json.value(QLatin1String("image_sha256")).toString();
    reply.imageUrl = json.value(QLatin1String("image_url")).toString();
    reply.strokeCount = intValue(json, "stroke_count");
    reply.message = json.value(QLatin1String("message")).toString();
    const QJsonObject photo = json.value(QLatin1String("photo")).toObject();
    if (!photo.isEmpty()) {
        reply.photoUrl = photo.value(QLatin1String("url")).toString();
        reply.photoSha256 = photo.value(QLatin1String("sha256")).toString();
        reply.photoMediaType = photo.value(QLatin1String("media_type")).toString(QStringLiteral("image/jpeg"));
        reply.photoCapture = photo.value(QLatin1String("capture")).toString();
        reply.photoWidth = intValue(photo, "width");
        reply.photoHeight = intValue(photo, "height");
    }
    return reply;
}

Listening listeningFromJson(const QJsonObject &json)
{
    const QString state = json.value(QLatin1String("state")).toString();
    if (state == QLatin1String("listening")) return Listening::Listening;
    if (state == QLatin1String("not_listening")) return Listening::NotListening;
    return Listening::Unknown;
}

std::optional<Note> Note::fromJson(const QJsonObject &json)
{
    Note note;
    note.id = json.value(QLatin1String("note_id")).toString();
    if (note.id.isEmpty()) return std::nullopt;
    note.sequence = qint64(json.value(QLatin1String("sequence")).toDouble());
    note.createdAt = parseTime(json.value(QLatin1String("created_at")));
    note.senderDeviceId = json.value(QLatin1String("sender_device_id")).toString();
    note.title = json.value(QLatin1String("title")).toString();
    note.message = json.value(QLatin1String("message")).toString();
    note.mediaType = json.value(QLatin1String("media_type")).toString(QStringLiteral("image/png"));
    note.imageWidth = intValue(json, "image_width");
    note.imageHeight = intValue(json, "image_height");
    note.imageSha256 = json.value(QLatin1String("image_sha256")).toString();
    note.imageUrl = json.value(QLatin1String("image_url")).toString();
    note.documentId = json.value(QLatin1String("document_id")).toString();
    note.documentFilename = json.value(QLatin1String("document_filename")).toString();
    note.reviewCardId = json.value(QLatin1String("review_card_id")).toString();
    note.reviewQuestion = json.value(QLatin1String("review_question")).toString();
    note.origin = json.value(QLatin1String("origin")).toObject();
    note.openedAt = parseTime(json.value(QLatin1String("opened_at")));
    note.deleted = json.value(QLatin1String("deleted")).toBool();
    for (const QJsonValue &value : json.value(QLatin1String("replies")).toArray()) {
        if (auto reply = Reply::fromJson(value.toObject())) note.replies.append(*reply);
    }
    std::sort(note.replies.begin(), note.replies.end(), [](const Reply &a, const Reply &b) { return a.sequence < b.sequence; });
    return note;
}

bool Note::isPhonePage() const
{
    return origin.value(QLatin1String("kind")).toString() == QLatin1String("phone_page");
}

QString Note::senderLabel() const
{
    if (isPhonePage()) return senderDeviceId == Platform::desktopDeviceAccount() ? i18n("You, from this Mac") : i18n("You, from the phone");
    const QString conversationAgent = origin.value(QLatin1String("conversation_agent")).toString().trimmed();
    QString agent;
    if (conversationAgent.compare(QLatin1String("claude"), Qt::CaseInsensitive) == 0) agent = QStringLiteral("Claude");
    else if (conversationAgent.compare(QLatin1String("codex"), Qt::CaseInsensitive) == 0) agent = QStringLiteral("Codex");
    else if (!conversationAgent.isEmpty()) agent = conversationAgent.left(1).toUpper() + conversationAgent.mid(1);
    else agent = i18n("Agent");
    if (senderDeviceId == QLatin1String("connector")) return i18nc("@info agent name", "%1 via the connector", agent);
    if (!senderDeviceId.isEmpty()) return i18nc("@info agent name, device", "%1 on %2", agent, senderDeviceId);
    return agent;
}

QUrl Note::conversationUrl() const
{
    const QUrl url(origin.value(QLatin1String("conversation_url")).toString(), QUrl::StrictMode);
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty() ? url : QUrl();
}

std::optional<QList<Note>> parseNotes(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject() || !document.object().value(QLatin1String("notes")).isArray()) {
        if (error) *error = parseError.error != QJsonParseError::NoError ? parseError.errorString() : QStringLiteral("the answer has no notes list");
        return std::nullopt;
    }
    QList<Note> notes;
    for (const QJsonValue &value : document.object().value(QLatin1String("notes")).toArray()) {
        if (auto note = Note::fromJson(value.toObject()); note && !note->deleted) notes.append(*note);
    }
    return notes;
}

// ---------------------------------------------------------------- read state

QList<LibraryDocument> libraryDocuments(const QJsonObject &catalogue)
{
    QList<LibraryDocument> documents;
    for (const QJsonValue &projectValue : catalogue.value(QLatin1String("projects")).toArray()) {
        const QJsonObject project = projectValue.toObject();
        const QString projectId = project.value(QLatin1String("id")).toString();
        const QString projectName = project.value(QLatin1String("name")).toString().trimmed();
        for (const QJsonValue &documentValue : project.value(QLatin1String("documents")).toArray()) {
            const QJsonObject document = documentValue.toObject();
            LibraryDocument entry{document.value(QLatin1String("document_id")).toString(), document.value(QLatin1String("filename")).toString(),
                                  document.value(QLatin1String("name")).toString().trimmed(), projectId, projectName};
            if (!entry.id.isEmpty() || !entry.filename.isEmpty()) documents.append(entry);
        }
    }
    return documents;
}

bool Filter::matches(const Note &note) const
{
    if (isEmpty()) return true;
    return (!note.documentId.isEmpty() && documentIds.contains(note.documentId))
        || (!note.documentFilename.isEmpty() && filenames.contains(note.documentFilename));
}

std::optional<Filter> Filter::project(const QList<LibraryDocument> &documents, const QString &idOrName)
{
    const QString wanted = idOrName.trimmed();
    if (wanted.isEmpty()) return std::nullopt;
    // The id first; a name only when no project has that id.
    for (const bool byName : {false, true}) {
        Filter filter;
        for (const LibraryDocument &document : documents) {
            const bool match = byName ? document.projectName.compare(wanted, Qt::CaseInsensitive) == 0 : document.projectId == wanted;
            if (!match || (!filter.key.isEmpty() && filter.key != document.projectId)) continue;
            filter.kind = QStringLiteral("project");
            filter.key = document.projectId;
            filter.label = document.projectName.isEmpty() ? document.projectId : document.projectName;
            if (!document.id.isEmpty()) filter.documentIds.append(document.id);
            if (!document.filename.isEmpty()) filter.filenames.append(document.filename);
        }
        if (!filter.isEmpty()) return filter;
    }
    return std::nullopt;
}

std::optional<Filter> Filter::document(const QList<LibraryDocument> &documents, const QString &idFilenameOrName)
{
    const QString wanted = idFilenameOrName.trimmed();
    if (wanted.isEmpty()) return std::nullopt;
    const auto make = [&documents](const LibraryDocument &document) {
        const int siblings = int(std::count_if(documents.cbegin(), documents.cend(), [&](const LibraryDocument &other) { return other.projectId == document.projectId; }));
        // The only document of its project: the same notes as the project's.
        if (siblings == 1 && !document.projectId.isEmpty()) {
            if (const auto project = Filter::project(documents, document.projectId)) return *project;
        }
        Filter filter;
        filter.kind = QStringLiteral("document");
        filter.key = !document.id.isEmpty() ? document.id : document.filename;
        const QString name = !document.name.isEmpty() ? document.name : document.filename;
        filter.label = document.projectName.isEmpty() ? name : siblings > 1 ? QStringLiteral("%1 › %2").arg(document.projectName, name) : document.projectName;
        if (!document.id.isEmpty()) filter.documentIds.append(document.id);
        if (!document.filename.isEmpty()) filter.filenames.append(document.filename);
        return filter;
    };
    for (const LibraryDocument &document : documents) {
        if ((!document.id.isEmpty() && document.id == wanted) || (!document.filename.isEmpty() && document.filename == wanted)) return make(document);
    }
    // A name: only when exactly one document has it.
    std::optional<LibraryDocument> named;
    for (const LibraryDocument &document : documents) {
        if (document.name.compare(wanted, Qt::CaseInsensitive) != 0) continue;
        if (named) return std::nullopt;
        named = document;
    }
    if (named) return make(*named);
    return std::nullopt;
}

ReadState::ReadState(QString path)
    : m_path(std::move(path))
{
    load();
}

void ReadState::load()
{
    QFile file(m_path);
    if (m_path.isEmpty() || !file.open(QIODevice::ReadOnly)) return;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &value : json.value(QLatin1String("seen")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !m_seen.contains(id)) {
            m_seen.insert(id);
            m_seenOrder.append(id);
        }
    }
    for (const QJsonValue &value : json.value(QLatin1String("decided")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !m_decided.contains(id)) {
            m_decided.insert(id);
            m_decidedOrder.append(id);
        }
    }
    const QJsonValue baseline = json.value(QLatin1String("notification_baseline"));
    m_baseline = baseline.isDouble() ? qint64(baseline.toDouble()) : -1;
}

void ReadState::save() const
{
    if (m_path.isEmpty()) return;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    const QJsonObject json{{QStringLiteral("schema_version"), 1},
                           {QStringLiteral("seen"), QJsonArray::fromStringList(m_seenOrder)},
                           {QStringLiteral("decided"), QJsonArray::fromStringList(m_decidedOrder)},
                           {QStringLiteral("notification_baseline"), m_baseline < 0 ? QJsonValue() : QJsonValue(double(m_baseline))}};
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    file.commit();
}

void ReadState::remember(QStringList &order, QSet<QString> &set, const QString &id, int limit)
{
    set.insert(id);
    order.append(id);
    while (order.size() > limit) set.remove(order.takeFirst());
}

bool ReadState::markSeen(const QString &noteId)
{
    if (noteId.isEmpty() || m_seen.contains(noteId)) return false;
    remember(m_seenOrder, m_seen, noteId, SeenLimit);
    save();
    return true;
}

void ReadState::setNotificationBaseline(qint64 sequence)
{
    if (m_baseline == sequence) return;
    m_baseline = sequence;
    save();
}

void ReadState::markDecided(const QString &noteId)
{
    if (noteId.isEmpty() || m_decided.contains(noteId)) return;
    remember(m_decidedOrder, m_decided, noteId, DecidedLimit);
    save();
}

bool isUnread(const Note &note, const ReadState &state)
{
    return !note.isPhonePage() && !note.openedAt.isValid() && !state.isSeen(note.id);
}

// ---------------------------------------------------------------- client

Client::Client(QString cacheRoot, QObject *parent)
    : QObject(parent)
    , m_cacheRoot(std::move(cacheRoot))
{
}

void Client::setServer(const QUrl &baseUrl, const QByteArray &token)
{
    m_baseUrl = baseUrl;
    m_token = token;
    m_explicitServer = true;
}

bool Client::ensureServer(QString *error)
{
    // Read again while unset: the Mac may be paired (or re-paired) while Thoughts runs.
    if (!m_explicitServer && (m_token.isEmpty() || !m_baseUrl.isValid() || m_baseUrl.isEmpty())) {
        m_token = qgetenv("THOUGHTS_TOKEN").trimmed();
        if (QStandardPaths::isTestModeEnabled() && (m_token.isEmpty() || !ServerAddress::overriddenByEnvironment())) {
            m_token.clear();
            m_baseUrl.clear();
        } else {
            const QString address = ServerAddress::configured();
            if (m_token.isEmpty()) m_token = ServerAddress::storedToken(Platform::desktopDeviceAccount(), address);
            m_baseUrl = QUrl(ServerAddress::normalized(address));
        }
    }
    if (m_token.isEmpty() || !m_baseUrl.isValid() || m_baseUrl.isEmpty()) {
        if (error) *error = i18n("This Mac is not connected to a Thoughts server.");
        return false;
    }
    if (!m_network) m_network = new QNetworkAccessManager(this);
    return true;
}

QNetworkRequest Client::request(const QString &pathAndQuery, int timeoutMs) const
{
    QUrl url = m_baseUrl;
    QString base = url.path();
    if (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const QUrl relative(pathAndQuery);
    url.setPath(base + relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    // The token is this server's: a redirect to another origin is not followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("Accept", "application/json, image/*");
    request.setRawHeader("User-Agent", "thoughts-desktop-agent-notes/1");
    request.setTransferTimeout(timeoutMs);
    return request;
}

QNetworkReply *Client::get(const QString &pathAndQuery, int timeoutMs)
{
    return m_network->get(request(pathAndQuery, timeoutMs));
}

namespace
{
Client::Failure failureOf(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403) return Client::Failure::Unauthorized;
    if (status == 404) return Client::Failure::Unsupported;
    if (status == 0 && reply->error() != QNetworkReply::NoError) return Client::Failure::Unreachable;
    if (status >= 400 || reply->error() != QNetworkReply::NoError) return Client::Failure::Failed;
    return Client::Failure::None;
}

QString describe(Client::Failure failure, QNetworkReply *reply)
{
    switch (failure) {
    case Client::Failure::Unauthorized:
        return i18n("The Thoughts server refused this Mac's device token.");
    case Client::Failure::Unsupported:
        return i18n("This Thoughts server has no agent notes.");
    case Client::Failure::Unreachable:
        return i18n("The Thoughts server could not be reached: %1", reply->errorString());
    default:
        return i18n("The Thoughts server answered with an error: %1", reply->errorString());
    }
}
}

void Client::list(int limit, QObject *context, std::function<void(const Result &)> done, const QString &documentId)
{
    QString error;
    if (!ensureServer(&error)) {
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, this, [guard, done = std::move(done), error] {
            if (guard) done({{}, Failure::NotConnected, error});
        });
        return;
    }
    QUrlQuery query;
    if (!documentId.isEmpty()) query.addQueryItem(QStringLiteral("document_id"), documentId);
    query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 200)));
    QNetworkReply *reply = get(QStringLiteral("/v1/agent-notes?") + query.toString(QUrl::FullyEncoded), 30000);
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [this, reply, guard, done = std::move(done)] {
        reply->deleteLater();
        const Failure failure = failureOf(reply);
        // A refused token is read again next time (the Mac may have been paired again).
        if (failure == Failure::Unauthorized && !m_explicitServer) m_token.clear();
        if (!guard) return;
        if (failure != Failure::None) {
            done({{}, failure, describe(failure, reply)});
            return;
        }
        QString parseError;
        const auto notes = parseNotes(reply->readAll(), &parseError);
        if (!notes) {
            done({{}, Failure::Failed, i18n("The Thoughts server sent an unreadable list of agent notes: %1", parseError)});
            return;
        }
        done({*notes, Failure::None, {}});
    });
}

void Client::post(const QString &path, const QJsonObject &body, QObject *context, std::function<void(const WriteResult &)> done)
{
    QString error;
    if (!ensureServer(&error)) {
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, this, [guard, done = std::move(done), error] {
            if (guard) done({0, {}, Failure::NotConnected, error, {}});
        });
        return;
    }
    QNetworkRequest post = request(path, 180000);
    post.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    finishWrite(m_network->post(post, QJsonDocument(body).toJson(QJsonDocument::Compact)), context, std::move(done));
}

void Client::remove(const QString &path, QObject *context, std::function<void(const WriteResult &)> done)
{
    QString error;
    if (!ensureServer(&error)) {
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, this, [guard, done = std::move(done), error] {
            if (guard) done({0, {}, Failure::NotConnected, error, {}});
        });
        return;
    }
    finishWrite(m_network->deleteResource(request(path, 30000)), context, std::move(done));
}

void Client::finishWrite(QNetworkReply *reply, QObject *context, std::function<void(const WriteResult &)> done)
{
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [this, reply, guard, done = std::move(done)] {
        reply->deleteLater();
        WriteResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.json = QJsonDocument::fromJson(reply->readAll()).object();
        result.errorCode = result.json.value(QLatin1String("error_code")).toString();
        result.failure = failureOf(reply);
        // A 404 that names the note is the note gone, not a server without agent notes.
        if (result.failure == Failure::Unsupported && !result.errorCode.isEmpty()) result.failure = Failure::Failed;
        if (result.failure == Failure::Unauthorized && !m_explicitServer) m_token.clear();
        if (result.failure != Failure::None) {
            const QString detail = result.json.value(QLatin1String("detail")).toString();
            if (result.errorCode == QLatin1String("agent_note_not_found")) result.error = i18n("This note is no longer on the Thoughts server.");
            else if (!detail.isEmpty() && result.failure == Failure::Failed) result.error = i18n("The Thoughts server refused it: %1", detail);
            else result.error = describe(result.failure, reply);
        }
        if (guard) done(result);
    });
}

QString Client::cachedImagePath(const QString &sha256, const QString &mediaType) const
{
    if (m_cacheRoot.isEmpty() || !validSha256(sha256)) return {};
    return QDir(m_cacheRoot).filePath(sha256 + QLatin1Char('.') + extensionFor(mediaType));
}

void Client::listening(const QString &noteId, QObject *context, std::function<void(bool, Listening)> done)
{
    QPointer<QObject> guard(context);
    QString error;
    if (!ensureServer(&error)) {
        QTimer::singleShot(0, this, [guard, done = std::move(done)] {
            if (guard) done(false, Listening::Unknown);
        });
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/v1/agent-notes/%1/listening").arg(QString::fromLatin1(QUrl::toPercentEncoding(noteId))), 15000);
    connect(reply, &QNetworkReply::finished, this, [reply, guard, done = std::move(done)] {
        reply->deleteLater();
        if (!guard) return;
        const Failure failure = failureOf(reply);
        // A server before agent_note_listening_v1 has no such route: it cannot tell.
        if (failure == Failure::Unsupported) {
            done(true, Listening::Unknown);
            return;
        }
        if (failure != Failure::None) {
            done(false, Listening::Unknown);
            return;
        }
        done(true, listeningFromJson(QJsonDocument::fromJson(reply->readAll()).object()));
    });
}

void Client::image(const QString &url, const QString &sha256, const QString &mediaType, QObject *context,
                   std::function<void(const QImage &, const QString &)> done)
{
    QPointer<QObject> guard(context);
    const auto answer = [this, guard](std::function<void(const QImage &, const QString &)> callback, QImage image, QString error) {
        QTimer::singleShot(0, this, [guard, callback = std::move(callback), image = std::move(image), error = std::move(error)] {
            if (guard) callback(image, error);
        });
    };
    const QString cached = cachedImagePath(sha256, mediaType);
    if (!cached.isEmpty() && QFileInfo::exists(cached)) {
        QImageReader reader(cached);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (!image.isNull()) {
            answer(std::move(done), std::move(image), {});
            return;
        }
        QFile::remove(cached);
    }
    QString error;
    if (!ensureServer(&error)) {
        answer(std::move(done), {}, error);
        return;
    }
    if (!validImagePath(url)) {
        answer(std::move(done), {}, i18n("The server named an unexpected picture address."));
        return;
    }
    QNetworkReply *reply = get(url, 120000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, guard, cached, sha256, done = std::move(done)] {
        reply->deleteLater();
        const Failure failure = failureOf(reply);
        if (failure != Failure::None) {
            if (guard) done({}, failure == Failure::Unsupported ? i18n("The picture is no longer on the server.") : describe(failure, reply));
            return;
        }
        const QByteArray bytes = reply->readAll();
        if (validSha256(sha256) && QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != sha256) {
            if (guard) done({}, i18n("The picture the server sent does not match its checksum."));
            return;
        }
        QImage image;
        image.loadFromData(bytes);
        if (image.isNull()) {
            if (guard) done({}, i18n("The picture could not be read."));
            return;
        }
        if (!cached.isEmpty()) {
            QDir().mkpath(QFileInfo(cached).absolutePath());
            QSaveFile file(cached);
            if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
            if (++m_downloadsSincePrune >= 20) pruneCache();
        }
        if (guard) done(image, {});
    });
}

void Client::pruneCache()
{
    m_downloadsSincePrune = 0;
    QDir dir(m_cacheRoot);
    QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Time);
    for (int index = CachedImageLimit; index < files.size(); ++index) QFile::remove(files.at(index).absoluteFilePath());
}

// ---------------------------------------------------------------- center

Center *Center::instance()
{
    static QPointer<Center> center;
    if (!center) {
        const QString root = QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("agent-notes"));
        auto *client = new Client(QDir(root).filePath(QStringLiteral("images")));
        center = new Center(client, QDir(root).filePath(QStringLiteral("state.json")), {}, QCoreApplication::instance());
        client->setParent(center);
    }
    return center;
}

Center::Center(Client *client, QString statePath, Clock clock, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_state(std::move(statePath))
    , m_clock(clock ? std::move(clock) : Clock([] { return QDateTime::currentDateTimeUtc(); }))
    , m_timer(new QTimer(this))
{
    qRegisterMetaType<Thoughts::AgentNotes::Note>();
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &Center::poll);
}

Center::~Center() = default;

std::optional<Note> Center::note(const QString &noteId) const
{
    for (const Note &note : m_notes) {
        if (note.id == noteId) return note;
    }
    return std::nullopt;
}

int Center::unreadCount() const
{
    return int(std::count_if(m_notes.cbegin(), m_notes.cend(), [this](const Note &note) { return isUnread(note); }));
}

void Center::emitUnreadIfChanged()
{
    const int count = unreadCount();
    if (count == m_lastUnread) return;
    m_lastUnread = count;
    Q_EMIT unreadCountChanged(count);
}

void Center::markSeen(const QString &noteId)
{
    if (!m_state.markSeen(noteId)) return;
    Q_EMIT seenChanged();
    emitUnreadIfChanged();
}

void Center::markAllSeen()
{
    bool changed = false;
    for (const Note &note : std::as_const(m_notes)) {
        if (isUnread(note)) changed = m_state.markSeen(note.id) || changed;
    }
    if (!changed) return;
    Q_EMIT seenChanged();
    emitUnreadIfChanged();
}

void Center::start(int firstDelayMs)
{
    if (m_started) return;
    m_started = true;
    schedule(firstDelayMs);
}

void Center::setWatching(const QObject *viewer, bool watching)
{
    if (watching) {
        m_watchers.insert(viewer);
        if (std::exchange(m_watching, true)) return;
        refresh();
        return;
    }
    if (!m_watchers.remove(viewer) || !m_watchers.isEmpty()) return;
    // Settled once control returns: one view hidden for another (a tab's sidebar for the
    // next tab's) keeps watching without asking the server again.
    QTimer::singleShot(0, this, [this] {
        if (!m_watchers.isEmpty() || !std::exchange(m_watching, false)) return;
        if (m_started && !m_inFlight) schedule(IdleIntervalMs);
    });
}

void Center::refresh()
{
    m_timer->stop();
    poll();
}

void Center::requestOpen(const QString &noteId)
{
    Q_EMIT openRequested(noteId);
}

void Center::noteShown(const QString &noteId)
{
    markSeen(noteId);
    const auto shown = note(noteId);
    if (!shown || shown->openedAt.isValid() || shown->isPhonePage() || m_openedPosted.contains(noteId)) return;
    m_openedPosted.insert(noteId);
    m_client->post(QStringLiteral("/v1/agent-notes/%1/receipt").arg(noteId), {{QStringLiteral("state"), QStringLiteral("opened")}}, this,
                   [this, noteId](const Client::WriteResult &result) {
                       if (result.failure != Client::Failure::None) {
                           // Said again when the note is shown again.
                           m_openedPosted.remove(noteId);
                           DeveloperLog::record(QStringLiteral("agent_notes.receipt.failed"), {{QStringLiteral("status"), result.status}});
                           return;
                       }
                       if (const auto updated = Note::fromJson(result.json)) replaceNote(*updated);
                   });
}

void Center::replaceNote(const Note &updated)
{
    for (Note &existing : m_notes) {
        if (existing.id != updated.id) continue;
        existing = updated;
        const QByteArray signature = signatureOf(m_notes);
        if (signature != m_signature) {
            m_signature = signature;
            Q_EMIT notesChanged();
        }
        emitUnreadIfChanged();
        return;
    }
}

std::shared_ptr<Center::Draft> Center::draft(const QString &noteId)
{
    auto &draft = m_drafts[noteId];
    if (!draft) draft = std::make_shared<Draft>();
    return draft;
}

void Center::setDraftMessage(const QString &noteId, const QString &message)
{
    const auto current = draft(noteId);
    if (current->message == message) return;
    current->message = message;
    Q_EMIT draftChanged(noteId);
}

void Center::draftDrawingChanged(const QString &noteId)
{
    if (m_drafts.contains(noteId)) Q_EMIT draftChanged(noteId);
}

void Center::sendReply(const QString &noteId, std::function<void(bool, const QString &)> done)
{
    const auto target = note(noteId);
    const auto current = draft(noteId);
    if (!target || current->sending) {
        if (done) done(false, target ? i18n("The reply is being sent.") : i18n("This note is no longer listed."));
        return;
    }
    current->sending = true;
    current->error.clear();
    m_replyCallbacks.insert(noteId, std::move(done));
    Q_EMIT draftChanged(noteId);
    const bool drawn = !current->drawing.isNull() && !current->drawing.isEmpty();
    // The picture from the cache (downloaded if it went): drawn on, or sent as it is.
    m_client->image(target->imageUrl, target->imageSha256, target->mediaType, this,
                    [this, noteId, drawn, sha = target->imageSha256, mediaType = target->mediaType](const QImage &image, const QString &error) {
                        const auto current = existingDraft(noteId);
                        if (!current) return;
                        if (image.isNull()) {
                            finishReply(noteId, false, error.isEmpty() ? i18n("The note's picture could not be loaded.") : error);
                            return;
                        }
                        if (drawn) {
                            QByteArray png;
                            QBuffer buffer(&png);
                            buffer.open(QIODevice::WriteOnly);
                            current->drawing.composite(image).save(&buffer, "PNG");
                            postReply(noteId, png, QStringLiteral("image/png"), image.size());
                            return;
                        }
                        QFile file(m_client->cachedImagePath(sha, mediaType));
                        if (file.open(QIODevice::ReadOnly)) {
                            postReply(noteId, file.readAll(), mediaType, image.size());
                            return;
                        }
                        // No copy on disk (no cache folder): the decoded picture as PNG.
                        QByteArray png;
                        QBuffer buffer(&png);
                        buffer.open(QIODevice::WriteOnly);
                        image.save(&buffer, "PNG");
                        postReply(noteId, png, QStringLiteral("image/png"), image.size());
                    });
}

void Center::postReply(const QString &noteId, const QByteArray &image, const QString &mediaType, const QSize &size)
{
    const auto current = existingDraft(noteId);
    const auto target = note(noteId);
    if (!current || !target) {
        finishReply(noteId, false, i18n("This note is no longer listed."));
        return;
    }
    // Without a drawing: an empty stroke document at the picture's size, as the phone sends.
    const AnnotationDocument strokes = !current->drawing.isNull() ? current->drawing : AnnotationDocument(size.width(), size.height());
    const QString message = current->message.trimmed();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(image);
    hash.addData(strokes.toJson());
    hash.addData(message.toUtf8());
    const QByteArray content = hash.result();
    // A retry of the same reply keeps its id: the server answers it once.
    if (current->replyId.isEmpty() || current->sentContent != content) current->replyId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    current->sentContent = content;
    QJsonObject body{{QStringLiteral("reply_id"), current->replyId},
                     {QStringLiteral("image"),
                      QJsonObject{{QStringLiteral("media_type"), mediaType}, {QStringLiteral("data_base64"), QString::fromLatin1(image.toBase64())}}},
                     {QStringLiteral("strokes"), QJsonDocument::fromJson(strokes.toJson()).object()}};
    if (!message.isEmpty()) body.insert(QStringLiteral("message"), message);
    m_client->post(QStringLiteral("/v1/agent-notes/%1/replies").arg(noteId), body, this, [this, noteId](const Client::WriteResult &result) {
        if (result.failure != Client::Failure::None) {
            finishReply(noteId, false, result.error);
            return;
        }
        // Whether an agent hears it (the server's answer; older servers send none).
        if (result.json.contains(QLatin1String("listening"))) {
            setListening(noteId, listeningFromJson(result.json.value(QLatin1String("listening")).toObject()));
        }
        // The reply belongs to the note now; the next list shows it.
        if (const auto reply = Reply::fromJson(result.json)) {
            for (Note &existing : m_notes) {
                if (existing.id != noteId) continue;
                if (std::none_of(existing.replies.cbegin(), existing.replies.cend(), [&](const Reply &known) { return known.id == reply->id; })) {
                    existing.replies.append(*reply);
                    m_signature = signatureOf(m_notes);
                    Q_EMIT notesChanged();
                }
                break;
            }
        }
        finishReply(noteId, true, {});
        refresh();
    });
}

void Center::checkListening(const QString &noteId)
{
    if (noteId.isEmpty() || m_listeningAsked.contains(noteId)) return;
    m_listeningAsked.insert(noteId);
    m_client->listening(noteId, this, [this, noteId](bool ok, Listening state) {
        m_listeningAsked.remove(noteId);
        if (ok) setListening(noteId, state);
    });
}

void Center::setListening(const QString &noteId, Listening state)
{
    const bool changed = listening(noteId) != state || !m_listening.contains(noteId);
    m_listening.insert(noteId, state);
    if (changed) Q_EMIT listeningChanged(noteId);
}

void Center::finishReply(const QString &noteId, bool ok, const QString &error)
{
    if (ok) {
        m_drafts.remove(noteId);
    } else if (const auto current = existingDraft(noteId)) {
        current->sending = false;
        current->error = error;
    }
    DeveloperLog::record(ok ? QStringLiteral("agent_notes.reply.sent") : QStringLiteral("agent_notes.reply.failed"), {});
    Q_EMIT draftChanged(noteId);
    if (auto done = m_replyCallbacks.take(noteId)) done(ok, error);
}

void Center::sendPage(const Page &page, std::function<void(bool, const QString &)> done)
{
    const bool drawn = !page.drawing.isNull() && !page.drawing.isEmpty();
    const QImage image = drawn ? page.drawing.composite(page.image) : page.image.convertToFormat(QImage::Format_RGB32);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    // As the phone's study mode: a note from this device, origin.kind "phone_page".
    QJsonObject origin{{QStringLiteral("kind"), QStringLiteral("phone_page")},
                       {QStringLiteral("device"), QStringLiteral("mac")},
                       {QStringLiteral("stroke_count"), drawn ? int(page.drawing.strokes().size()) : 0}};
    if (page.page > 0) origin.insert(QStringLiteral("page"), page.page);
    if (page.pages > 0) origin.insert(QStringLiteral("pages"), page.pages);
    if (!page.documentName.isEmpty()) origin.insert(QStringLiteral("document_name"), page.documentName);
    QJsonObject body{{QStringLiteral("note_id"), page.noteId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : page.noteId},
                     {QStringLiteral("title"), page.title.trimmed().left(200)},
                     {QStringLiteral("origin"), origin},
                     {QStringLiteral("image"),
                      QJsonObject{{QStringLiteral("media_type"), QStringLiteral("image/png")}, {QStringLiteral("data_base64"), QString::fromLatin1(png.toBase64())}}}};
    const QString message = page.message.trimmed();
    if (!message.isEmpty()) body.insert(QStringLiteral("message"), message);
    if (!page.documentId.isEmpty()) body.insert(QStringLiteral("document_id"), page.documentId);
    if (!page.documentFilename.isEmpty()) body.insert(QStringLiteral("document_filename"), page.documentFilename);
    if (!page.reviewCardId.isEmpty()) body.insert(QStringLiteral("review_card_id"), page.reviewCardId);
    m_client->post(QStringLiteral("/v1/agent-notes"), body, this, [this, done = std::move(done)](const Client::WriteResult &result) {
        DeveloperLog::record(result.failure == Client::Failure::None ? QStringLiteral("agent_notes.page.sent") : QStringLiteral("agent_notes.page.failed"),
                             {{QStringLiteral("status"), result.status}});
        if (result.failure != Client::Failure::None) {
            if (done) done(false, result.error);
            return;
        }
        refresh();
        if (done) done(true, {});
    });
}

void Center::deleteNote(const QString &noteId, std::function<void(bool, const QString &)> done)
{
    m_client->remove(QStringLiteral("/v1/agent-notes/%1").arg(noteId), this, [this, noteId, done = std::move(done)](const Client::WriteResult &result) {
        // Gone already counts as deleted.
        const bool gone = result.failure == Client::Failure::None || result.errorCode == QLatin1String("agent_note_not_found");
        if (gone) {
            const auto before = m_notes.size();
            m_notes.erase(std::remove_if(m_notes.begin(), m_notes.end(), [&](const Note &note) { return note.id == noteId; }), m_notes.end());
            m_drafts.remove(noteId);
            if (m_notes.size() != before) {
                m_signature = signatureOf(m_notes);
                Q_EMIT notesChanged();
                emitUnreadIfChanged();
            }
        }
        if (done) done(gone, gone ? QString() : result.error);
    });
}

void Center::schedule(int ms)
{
    m_timer->start(qMax(0, ms));
}

void Center::poll()
{
    if (m_inFlight) {
        m_refreshAgain = true;
        return;
    }
    m_inFlight = true;
    Q_EMIT stateChanged();
    m_client->list(m_watching ? WatchingLimit : IdleLimit, this, [this](const Client::Result &result) { finished(result); });
}

void Center::finished(const Client::Result &result)
{
    m_inFlight = false;
    const bool again = std::exchange(m_refreshAgain, false);
    if (result.failure != Client::Failure::None) {
        if (m_lastFailure != result.failure) {
            DeveloperLog::record(QStringLiteral("agent_notes.list.failed"), {{QStringLiteral("failure"), int(result.failure)}});
        }
        m_lastFailure = result.failure;
        m_lastError = result.error;
        ++m_failures;
        int delay = m_watching ? WatchingIntervalMs : IdleIntervalMs;
        if (result.failure == Client::Failure::NotConnected || result.failure == Client::Failure::Unsupported
            || result.failure == Client::Failure::Unauthorized) {
            delay = 30 * 60 * 1000;
        } else {
            for (int step = 1; step < m_failures && delay < 10 * 60 * 1000; ++step) delay *= 2;
            delay = qMin(delay, 10 * 60 * 1000);
        }
        Q_EMIT stateChanged();
        if (again) poll();
        else if (m_started || m_watching) schedule(delay);
        return;
    }
    m_failures = 0;
    m_lastFailure = Client::Failure::None;
    m_lastError.clear();
    m_lastUpdated = m_clock();
    apply(result.notes);
    Q_EMIT stateChanged();
    if (again) {
        poll();
        return;
    }
    if (!m_started && !m_watching) return;
    int delay = m_watching ? WatchingIntervalMs : IdleIntervalMs;
    // A new note still inside its grace period: look again once it ends.
    const QDateTime now = m_clock();
    for (const Note &note : std::as_const(m_notes)) {
        if (note.sequence <= m_state.notificationBaseline() || m_state.isDecided(note.id) || !note.createdAt.isValid()) continue;
        const qint64 remaining = qint64(GracePeriodSeconds) * 1000 - note.createdAt.msecsTo(now);
        if (remaining > 0) delay = int(qBound(qint64(2000), remaining + 1000, qint64(delay)));
    }
    schedule(delay);
}

void Center::apply(const QList<Note> &fetched)
{
    QList<Note> merged;
    if (!fetched.isEmpty()) {
        qint64 lowest = fetched.first().sequence;
        for (const Note &note : fetched) lowest = qMin(lowest, note.sequence);
        // The answer is the newest notes: everything from its lowest sequence up is what the
        // server has now (a note missing there was deleted); older notes stay as they were.
        merged = fetched;
        for (const Note &note : std::as_const(m_notes)) {
            if (note.sequence < lowest) merged.append(note);
        }
        std::sort(merged.begin(), merged.end(), [](const Note &a, const Note &b) { return a.sequence > b.sequence; });
    }
    m_notes = merged;
    if (m_state.notificationBaseline() < 0) {
        m_state.setNotificationBaseline(m_notes.isEmpty() ? 0 : m_notes.first().sequence);
    }
    const bool firstList = !std::exchange(m_listed, true);
    decideNotifications();
    const QByteArray signature = signatureOf(m_notes);
    if (firstList || signature != m_signature) {
        m_signature = signature;
        Q_EMIT notesChanged();
    }
    emitUnreadIfChanged();
}

void Center::merge(const QList<Note> &fetched)
{
    bool changed = false;
    for (const Note &note : fetched) {
        auto existing = std::find_if(m_notes.begin(), m_notes.end(), [&](const Note &known) { return known.id == note.id; });
        if (existing == m_notes.end()) {
            m_notes.append(note);
            changed = true;
        } else {
            *existing = note;
        }
    }
    if (changed) std::sort(m_notes.begin(), m_notes.end(), [](const Note &a, const Note &b) { return a.sequence > b.sequence; });
    const QByteArray signature = signatureOf(m_notes);
    if (signature != m_signature) {
        m_signature = signature;
        Q_EMIT notesChanged();
    }
    emitUnreadIfChanged();
}

void Center::fetchDocuments(const QStringList &documentIds)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int asked = 0;
    for (const QString &id : documentIds) {
        if (id.isEmpty() || asked >= 20) continue;
        if (m_documentFetched.contains(id) && now - m_documentFetched.value(id) < 60 * 1000) continue;
        m_documentFetched.insert(id, now);
        ++asked;
        m_client->list(200, this, [this, id](const Client::Result &result) {
            if (result.failure != Client::Failure::None) {
                m_documentFetched.remove(id);  // asked again next time
                return;
            }
            merge(result.notes);
        }, id);
    }
}

void Center::decideNotifications()
{
    const QDateTime now = m_clock();
    QList<Note> attention;
    for (auto it = m_notes.crbegin(); it != m_notes.crend(); ++it) {
        const Note &note = *it;
        if (note.sequence <= m_state.notificationBaseline() || m_state.isDecided(note.id)) continue;
        if (!isUnread(note) || !note.createdAt.isValid()) {
            m_state.markDecided(note.id);
            continue;
        }
        const qint64 age = note.createdAt.secsTo(now);
        if (age > NotifyWithinSeconds) {
            m_state.markDecided(note.id);
            continue;
        }
        if (age < GracePeriodSeconds) continue;
        m_state.markDecided(note.id);
        attention.append(note);
    }
    // Several at once (the Mac was asleep): the newest few, not a stack of banners.
    const qsizetype first = qMax<qsizetype>(0, attention.size() - MaxAttentionPerPass);
    for (qsizetype index = first; index < attention.size(); ++index) Q_EMIT noteNeedsAttention(attention.at(index));
}

QString relativeTime(const QDateTime &time, const QDateTime &now)
{
    if (!time.isValid()) return {};
    const QDateTime local = time.toLocalTime();
    const QDate today = now.toLocalTime().date();
    const QLocale locale;
    const QString clock = locale.toString(local.time(), QLocale::ShortFormat);
    const qint64 days = local.date().daysTo(today);
    if (days == 0) return i18nc("@info time of today", "Today, %1", clock);
    if (days == 1) return i18nc("@info time of yesterday", "Yesterday, %1", clock);
    if (days > 1 && days < 7) return i18nc("@info weekday, time", "%1, %2", locale.dayName(local.date().dayOfWeek(), QLocale::LongFormat), clock);
    return locale.toString(local, QLocale::ShortFormat);
}

}
