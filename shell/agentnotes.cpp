// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnotes.h"

#include "../part/thoughtsplatform.h"
#include "developerlog.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QCoreApplication>
#include <QGuiApplication>
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

namespace
{
constexpr QLatin1StringView SenderRole{"sender"};
constexpr QLatin1StringView OwnerRole{"owner"};
constexpr QLatin1StringView ResponderRole{"responder"};

QString agentLabel(const QJsonObject &json)
{
    return json.value(QLatin1String("label")).toString().trimmed().left(120);
}

// The live listener that answers first, from a listening answer's "listeners": the sender's
// agent, then a topic owner, then the reply agent, which waits for both. A listener from a
// server before topic owners has no "role": "responder": true is the reply agent, anything else
// the sender's agent.
std::optional<Agent> covererOf(const QJsonObject &json)
{
    if (listeningFromJson(json) != Listening::Listening) return std::nullopt;
    QList<Agent> coverers;
    for (const QJsonValue &value : json.value(QLatin1String("listeners")).toArray()) {
        const QJsonObject listener = value.toObject();
        if (listener.isEmpty()) continue;
        Agent agent;
        agent.role = listener.value(QLatin1String("role")).toString();
        if (agent.role.isEmpty()) agent.role = listener.value(QLatin1String("responder")).toBool() ? ResponderRole : SenderRole;
        if (agent.role != SenderRole && agent.role != OwnerRole && agent.role != ResponderRole) continue;
        agent.deviceId = listener.value(QLatin1String("device_id")).toString();
        agent.conversationId = listener.value(QLatin1String("conversation_id")).toString();
        agent.ownerId = listener.value(QLatin1String("owner_id")).toString();
        if (agent.role == OwnerRole) agent.label = agentLabel(listener);
        agent.live = true;
        coverers.append(agent);
    }
    for (const QLatin1StringView role : {SenderRole, OwnerRole, ResponderRole}) {
        const auto found = std::find_if(coverers.cbegin(), coverers.cend(), [role](const Agent &agent) { return agent.role == role; });
        if (found != coverers.cend()) return *found;
    }
    return std::nullopt;
}

bool sameRecord(const Agent &a, const Agent &b)
{
    return a.isSameAgent(b) && a.label == b.label && a.live == b.live;
}

bool sameRecord(const std::optional<Agent> &a, const std::optional<Agent> &b)
{
    return a.has_value() == b.has_value() && (!a || sameRecord(*a, *b));
}

bool sameWake(const std::optional<Wake> &a, const std::optional<Wake> &b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a
        || (a->state == b->state && a->role == b->role && a->machine == b->machine && a->title == b->title && a->sessionState == b->sessionState
            && a->label == b->label && a->fallbackAt == b->fallbackAt);
}

bool sameAnswer(const ListeningAnswer &a, const ListeningAnswer &b)
{
    return a.state == b.state && sameRecord(a.coverer, b.coverer) && sameRecord(a.answerBy, b.answerBy)
        && std::equal(a.candidates.cbegin(), a.candidates.cend(), b.candidates.cbegin(), b.candidates.cend(),
                      [](const Agent &x, const Agent &y) { return sameRecord(x, y); })
        && sameWake(a.wake, b.wake);
}

// Whether the view names the listener rather than the session a waker wakes: an agent listens and
// it is not only the responder. The server's own waker listens as the responder so that older
// apps still see an agent listening; while it does, the session the waker wakes is who answers.
bool namesTheListener(Listening state, const std::optional<Agent> &named, const std::optional<Wake> &wake)
{
    return state == Listening::Listening && (!wake || !named || named->role != ResponderRole);
}
}

QString wakerMachineName(const QString &machine)
{
    return machine.compare(QLatin1String("mac"), Qt::CaseInsensitive) == 0 ? i18nc("@info the machine a session runs on", "the Mac")
                                                                           : i18nc("@info the machine a session runs on", "the server");
}

bool Wake::willBeWoken() const
{
    return state == QLatin1String("will_be_woken");
}

QString Wake::name() const
{
    if (role == ResponderRole) return i18nc("@info the session that answers what has no other home", "The study session");
    if (!title.isEmpty()) return title;
    Agent agent;
    agent.role = role;
    agent.label = label;
    return agent.name();
}

QString Wake::text() const
{
    if (!willBeWoken()) {
        QString machineName = wakerMachineName(machine);
        if (!machineName.isEmpty()) machineName[0] = machineName.at(0).toUpper();
        if (!fallbackAt.isValid()) {
            return i18nc("@info a machine", "%1 is offline; the study session answers later if it is still offline", machineName);
        }
        return i18nc("@info a machine, a time of day", "%1 is offline; the study session answers at %2 if it is still offline", machineName,
                     QLocale::system().toString(fallbackAt.toLocalTime().time(), QLocale::ShortFormat));
    }
    if (sessionState == QLatin1String("busy")) return i18nc("@info a session's title", "%1 answers when it finishes its current task", name());
    if (role == ResponderRole) return i18nc("@info a session's title", "%1 will be woken", name());
    return i18nc("@info a session's title, a machine", "%1 will be woken on %2", name(), wakerMachineName(machine));
}

QString Wake::sentText() const
{
    return i18nc("@info a session's title", "Sent ✓ · %1 will be woken", name());
}

std::optional<Wake> Wake::fromJson(const QJsonObject &json)
{
    Wake wake;
    wake.state = json.value(QLatin1String("state")).toString();
    if (wake.state != QLatin1String("will_be_woken") && wake.state != QLatin1String("waker_offline")) return std::nullopt;
    wake.role = json.value(QLatin1String("role")).toString();
    if (wake.role != SenderRole && wake.role != OwnerRole && wake.role != ResponderRole) return std::nullopt;
    wake.machine = json.value(QLatin1String("machine")).toString();
    if (wake.machine.trimmed().isEmpty()) return std::nullopt;
    wake.title = json.value(QLatin1String("title")).toString().trimmed().left(120);
    wake.sessionState = json.value(QLatin1String("session_state")).toString();
    wake.label = agentLabel(json);
    wake.fallbackAt = parseTime(json.value(QLatin1String("fallback_at")));
    return wake;
}

bool Agent::isSameAgent(const Agent &other) const
{
    return role == other.role && deviceId == other.deviceId && conversationId == other.conversationId && ownerId == other.ownerId;
}

QString Agent::name() const
{
    if (role == ResponderRole) return i18n("Thoughts reply agent");
    if (role == OwnerRole) return label.isEmpty() ? i18n("The topic's agent") : i18nc("@info a topic's agent, by the topic", "%1 agent", label);
    return i18n("The agent that sent it");
}

QJsonObject Agent::toChoiceJson() const
{
    return {{QStringLiteral("role"), role},
            {QStringLiteral("device_id"), deviceId},
            {QStringLiteral("conversation_id"), conversationId.isEmpty() ? QJsonValue() : QJsonValue(conversationId)},
            {QStringLiteral("owner_id"), ownerId.isEmpty() ? QJsonValue() : QJsonValue(ownerId)}};
}

std::optional<Agent> Agent::fromJson(const QJsonObject &json)
{
    Agent agent;
    agent.role = json.value(QLatin1String("role")).toString();
    if (agent.role != SenderRole && agent.role != OwnerRole && agent.role != ResponderRole) return std::nullopt;
    agent.deviceId = json.value(QLatin1String("device_id")).toString();
    if (agent.deviceId.trimmed().isEmpty()) return std::nullopt;
    agent.conversationId = json.value(QLatin1String("conversation_id")).toString();
    agent.ownerId = json.value(QLatin1String("owner_id")).toString();
    agent.label = agentLabel(json);
    agent.live = json.value(QLatin1String("live")).toBool();
    return agent;
}

ListeningAnswer ListeningAnswer::fromJson(const QJsonObject &json)
{
    ListeningAnswer answer;
    answer.state = listeningFromJson(json);
    answer.coverer = covererOf(json);
    answer.answerBy = Agent::fromJson(json.value(QLatin1String("answer_by")).toObject());
    for (const QJsonValue &value : json.value(QLatin1String("candidates")).toArray()) {
        if (const auto agent = Agent::fromJson(value.toObject())) answer.candidates.append(*agent);
    }
    answer.wake = Wake::fromJson(json.value(QLatin1String("wake")).toObject());
    return answer;
}

QString answerByCapability()
{
    return QStringLiteral("agent_note_answer_by_v1");
}

std::optional<AnswerLine> answerLine(const std::optional<ListeningAnswer> &answer, bool canChange, bool reachable)
{
    if (!answer || (answer->state == Listening::Unknown && !answer->wake)) return std::nullopt;
    AnswerLine line;
    line.changeOffered = canChange;
    line.changeEnabled = canChange && reachable;
    // With a choice the server says "listening" exactly while the chosen agent is live.
    const std::optional<Agent> &named = answer->answerBy ? answer->answerBy : answer->coverer;
    if (namesTheListener(answer->state, named, answer->wake)) {
        line.mark = AnswerLine::Mark::Listening;
        line.text = named ? i18nc("@info an agent's name", "%1 is listening", named->name()) : i18n("An agent is listening");
    } else if (answer->wake) {
        line.mark = answer->wake->willBeWoken() ? AnswerLine::Mark::Woken : AnswerLine::Mark::Offline;
        line.text = answer->wake->text();
        line.muted = !answer->wake->willBeWoken();
    } else if (answer->answerBy) {
        line.mark = AnswerLine::Mark::Waiting;
        line.text = i18nc("@info an agent's name", "%1 is not listening right now. The Thoughts reply agent answers after 10 minutes.", answer->answerBy->name());
    } else {
        line.mark = AnswerLine::Mark::Nobody;
        line.text = i18n("No agent is listening right now");
        line.warning = true;
    }
    return line;
}

QList<AnswerChoice> answerChoices(const ListeningAnswer &answer)
{
    QList<Agent> agents;
    for (const Agent &candidate : answer.candidates) {
        if (std::none_of(agents.cbegin(), agents.cend(), [&candidate](const Agent &known) { return known.isSameAgent(candidate); })) agents.append(candidate);
    }
    QHash<QString, int> names;
    const auto numbered = [&names](const QString &name) {
        const int count = ++names[name];
        return count == 1 ? name : QStringLiteral("%1 (%2)").arg(name).arg(count);
    };
    const std::optional<Agent> &chosen = answer.answerBy;
    QList<AnswerChoice> choices;
    choices.append({std::nullopt, i18nc("@item who answers a reply", "Automatic"),
                    i18n("The agent that sent the note answers first, then the Thoughts reply agent"), !chosen});
    bool listed = false;
    for (const Agent &agent : std::as_const(agents)) {
        const bool checked = chosen && agent.isSameAgent(*chosen);
        listed = listed || checked;
        choices.append({agent, numbered(agent.name()), QString(), checked});
    }
    // What is chosen is always in the list, also when it could not answer now.
    if (chosen && !listed) choices.append({*chosen, i18nc("@item an agent's name", "%1 (not listening)", chosen->name()), QString(), true});
    return choices;
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
    m_link.setServer(baseUrl, token);
}

bool Client::ensureServer(QString *error)
{
    if (!m_link.ensure()) {
        if (error) *error = i18n("This Mac is not connected to a Thoughts server.");
        return false;
    }
    if (!m_network) m_network = new QNetworkAccessManager(this);
    return true;
}

QNetworkRequest Client::request(const QString &pathAndQuery, int timeoutMs) const
{
    QNetworkRequest request = m_link.request(pathAndQuery, timeoutMs, "thoughts-desktop-agent-notes/1");
    request.setRawHeader("Accept", "application/json, image/*");
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
            if (guard) done({{}, Failure::NotConnected, error, false});
        });
        return;
    }
    QUrlQuery query;
    if (!documentId.isEmpty()) query.addQueryItem(QStringLiteral("document_id"), documentId);
    query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 200)));
    const QString pathAndQuery = QStringLiteral("/v1/agent-notes?") + query.toString(QUrl::FullyEncoded);
    QNetworkRequest listRequest = request(pathAndQuery, 30000);
    // The version the server gave this list last time: unchanged, it answers 304 without the notes.
    const QByteArray version = m_listVersions.value(pathAndQuery);
    if (!version.isEmpty()) listRequest.setRawHeader("If-None-Match", version);
    QNetworkReply *reply = m_network->get(listRequest);
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [this, reply, guard, pathAndQuery, done = std::move(done)] {
        reply->deleteLater();
        const Failure failure = failureOf(reply);
        // A refused token is read again next time (the Mac may have been paired again).
        if (failure == Failure::Unauthorized) m_link.tokenRefused();
        if (failure != Failure::None) m_listVersions.remove(pathAndQuery);
        if (!guard) return;
        if (failure != Failure::None) {
            done({{}, failure, describe(failure, reply), false});
            return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 304) {
            done({{}, Failure::None, {}, true});
            return;
        }
        QString parseError;
        const auto notes = parseNotes(reply->readAll(), &parseError);
        if (!notes) {
            m_listVersions.remove(pathAndQuery);
            done({{}, Failure::Failed, i18n("The Thoughts server sent an unreadable list of agent notes: %1", parseError), false});
            return;
        }
        // A few lists are asked for (the newest 20, the newest 100, a document's): keep each one's version.
        const QByteArray answered = reply->rawHeader("ETag");
        if (answered.isEmpty()) {
            m_listVersions.remove(pathAndQuery);
        } else {
            if (m_listVersions.size() >= 64 && !m_listVersions.contains(pathAndQuery)) m_listVersions.clear();
            m_listVersions.insert(pathAndQuery, answered);
        }
        done({*notes, Failure::None, {}, false});
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

void Client::put(const QString &path, const QJsonObject &body, QObject *context, std::function<void(const WriteResult &)> done)
{
    QString error;
    if (!ensureServer(&error)) {
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, this, [guard, done = std::move(done), error] {
            if (guard) done({0, {}, Failure::NotConnected, error, {}});
        });
        return;
    }
    QNetworkRequest put = request(path, 30000);
    put.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    finishWrite(m_network->put(put, QJsonDocument(body).toJson(QJsonDocument::Compact)), context, std::move(done));
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
        if (result.failure == Failure::Unauthorized) m_link.tokenRefused();
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

void Client::listening(const QString &noteId, QObject *context, std::function<void(const ListeningResult &)> done)
{
    QPointer<QObject> guard(context);
    QString error;
    if (!ensureServer(&error)) {
        QTimer::singleShot(0, this, [guard, done = std::move(done)] {
            if (guard) done({Failure::NotConnected, {}});
        });
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/v1/agent-notes/%1/listening").arg(QString::fromLatin1(QUrl::toPercentEncoding(noteId))), 15000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, guard, done = std::move(done)] {
        reply->deleteLater();
        const Failure failure = failureOf(reply);
        if (failure == Failure::Unauthorized) m_link.tokenRefused();
        if (!guard) return;
        // A server before agent_note_listening_v1 has no such route: it cannot tell.
        if (failure == Failure::Unsupported) {
            done({Failure::None, {}});
            return;
        }
        if (failure != Failure::None) {
            done({failure, {}});
            return;
        }
        done({Failure::None, ListeningAnswer::fromJson(QJsonDocument::fromJson(reply->readAll()).object())});
    });
}

void Client::setAnswerBy(const QString &noteId, const std::optional<Agent> &agent, QObject *context, std::function<void(const WriteResult &)> done)
{
    put(QStringLiteral("/v1/agent-notes/%1/answer-by").arg(QString::fromLatin1(QUrl::toPercentEncoding(noteId))),
        {{QStringLiteral("agent"), agent ? QJsonValue(agent->toChoiceJson()) : QJsonValue()}}, context, std::move(done));
}

void Client::capabilities(QObject *context, std::function<void(bool, const QStringList &)> done)
{
    QPointer<QObject> guard(context);
    QString error;
    if (!ensureServer(&error)) {
        QTimer::singleShot(0, this, [guard, done = std::move(done)] {
            if (guard) done(false, {});
        });
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/v1/health"), 15000);
    connect(reply, &QNetworkReply::finished, this, [reply, guard, done = std::move(done)] {
        reply->deleteLater();
        if (!guard) return;
        const QByteArray body = reply->readAll();
        const bool ok = failureOf(reply) == Failure::None && QJsonDocument::fromJson(body).object().value(QLatin1String("capabilities")).isArray();
        done(ok, ok ? serverCapabilities(body) : QStringList());
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
        // Behind another application nobody looks at the notes: the slow pace is enough.
        if (auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
            center->setForeground(application->applicationState() == Qt::ApplicationActive);
            connect(application, &QGuiApplication::applicationStateChanged, center, [](Qt::ApplicationState state) {
                if (center) center->setForeground(state == Qt::ApplicationActive);
            });
        }
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

void Center::setForeground(bool foreground)
{
    if (m_foreground == foreground) return;
    m_foreground = foreground;
    Q_EMIT foregroundChanged(foreground);
    if (!foreground) return;
    // The user is back: the pace starts over, and a list older than the fast pace is asked for now.
    m_unchangedPolls = 0;
    if (!m_watching || m_inFlight) return;
    if (!m_lastUpdated.isValid() || m_lastUpdated.msecsTo(m_clock()) >= WatchingIntervalMs) {
        refresh();
    } else {
        schedule(WatchingIntervalMs);
    }
}

// The pace of the kind of poll that is due. It doubles with each poll in a row that found
// nothing new, after the first: 15, 15, 30 seconds while a view is watched, and 1, 1, 2
// minutes otherwise.
int Center::pollInterval() const
{
    const int maximum = watchingNow() ? WatchingMaximumIntervalMs : IdleMaximumIntervalMs;
    qint64 interval = watchingNow() ? WatchingIntervalMs : IdleIntervalMs;
    for (int poll = 1; poll < m_unchangedPolls && interval < maximum; ++poll) interval *= 2;
    return int(qMin<qint64>(interval, maximum));
}

int Center::nextPollDelayMs() const
{
    return m_timer->isActive() ? m_timer->interval() : -1;
}

void Center::pollNow()
{
    m_timer->stop();
    poll();
}

// Asked for (the window's Refresh, a view coming on screen, a reply sent): the pace starts over.
void Center::refresh()
{
    m_unchangedPolls = 0;
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
        // Whether an agent hears it, and which, or who a reply waker wakes for it (the server's
        // answer; older servers send none).
        m_sentStatus.remove(noteId);
        if (result.json.contains(QLatin1String("listening"))) {
            const ListeningAnswer heard = ListeningAnswer::fromJson(result.json.value(QLatin1String("listening")).toObject());
            setListening(noteId, heard.state, heard.coverer, heard.wake);
            const std::optional<Agent> &named = heard.coverer;
            if (heard.wake && !namesTheListener(heard.state, named, heard.wake)) m_sentStatus.insert(noteId, heard.wake->sentText());
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

Listening Center::listening(const QString &noteId) const
{
    const auto known = m_answers.constFind(noteId);
    return known == m_answers.cend() ? Listening::Unknown : known->state;
}

std::optional<ListeningAnswer> Center::answer(const QString &noteId) const
{
    const auto known = m_answers.constFind(noteId);
    return known == m_answers.cend() ? std::nullopt : std::optional<ListeningAnswer>(*known);
}

void Center::checkListening(const QString &noteId)
{
    if (noteId.isEmpty() || m_listeningAsked.contains(noteId)) return;
    m_listeningAsked.insert(noteId);
    learnWhetherTheChoiceIsOffered();
    m_client->listening(noteId, this, [this, noteId, choices = m_choices.value(noteId)](const Client::ListeningResult &result) {
        m_listeningAsked.remove(noteId);
        // A transport failure means the server cannot be reached now; a refusal does not.
        if (result.failure == Client::Failure::Unreachable) setServerReachable(false);
        else if (result.failure != Client::Failure::NotConnected) setServerReachable(true);
        // An answer from before a choice the user made meanwhile is older than the answer to that choice.
        if (result.failure == Client::Failure::None && choices == m_choices.value(noteId)) setAnswer(noteId, result.answer);
    });
}

void Center::checkListeningIfOlder(const QString &noteId, int maximumAgeMs)
{
    const QDateTime answered = m_answeredAt.value(noteId);
    if (!answered.isValid() || answered.msecsTo(m_clock()) >= maximumAgeMs) checkListening(noteId);
}

void Center::setAnswer(const QString &noteId, const ListeningAnswer &answer)
{
    const auto known = m_answers.constFind(noteId);
    const bool changed = known == m_answers.cend() || !sameAnswer(*known, answer);
    m_answers.insert(noteId, answer);
    m_answeredAt.insert(noteId, m_clock());
    if (changed) Q_EMIT listeningChanged(noteId);
}

void Center::setListening(const QString &noteId, Listening state, const std::optional<Agent> &coverer, const std::optional<Wake> &wake)
{
    ListeningAnswer merged = m_answers.value(noteId);
    merged.state = state;
    merged.coverer = coverer;
    merged.wake = wake;
    setAnswer(noteId, merged);
}

QString Center::sentStatus(const QString &noteId) const
{
    // A new reply started meanwhile: what it says is about that one.
    return existingDraft(noteId) ? QString() : m_sentStatus.value(noteId);
}

void Center::learnWhetherTheChoiceIsOffered()
{
    if (m_canChangeAnswerer || m_capabilitiesAsked) return;
    if (m_capabilitiesAnsweredAt.isValid() && m_capabilitiesAnsweredAt.secsTo(m_clock()) < CapabilitiesKeptSeconds) return;
    m_capabilitiesAsked = true;
    m_client->capabilities(this, [this](bool ok, const QStringList &capabilities) {
        m_capabilitiesAsked = false;
        if (!ok) return;  // asked again with the next check
        m_capabilitiesAnsweredAt = m_clock();
        if (!capabilities.contains(answerByCapability())) return;
        m_canChangeAnswerer = true;
        Q_EMIT answerOptionsChanged();
    });
}

void Center::setServerReachable(bool reachable)
{
    if (m_serverReachable == reachable) return;
    m_serverReachable = reachable;
    Q_EMIT answerOptionsChanged();
}

void Center::chooseAnswerer(const QString &noteId, const std::optional<Agent> &agent, std::function<void(bool, const QString &)> done)
{
    if (noteId.isEmpty() || m_choosing.contains(noteId)) {
        if (done) done(false, i18n("The choice before this one is still being sent."));
        return;
    }
    m_choosing.insert(noteId);
    ++m_choices[noteId];
    Q_EMIT answerOptionsChanged();
    m_client->setAnswerBy(noteId, agent, this, [this, noteId, done = std::move(done)](const Client::WriteResult &result) {
        m_choosing.remove(noteId);
        ++m_choices[noteId];
        if (result.failure == Client::Failure::Unreachable) setServerReachable(false);
        else if (result.failure != Client::Failure::NotConnected) setServerReachable(true);
        if (result.failure != Client::Failure::None) {
            DeveloperLog::record(QStringLiteral("agent_notes.answer_by.failed"), {{QStringLiteral("status"), result.status}});
            Q_EMIT answerOptionsChanged();
            const bool unknownAgent = result.errorCode == QLatin1String("agent_note_answerer_unknown");
            // The agents to choose from are no longer the ones listed: list them again.
            if (unknownAgent) checkListening(noteId);
            if (done) {
                done(false, unknownAgent ? i18n("The server no longer knows that agent. The choice stays as it was.")
                                         : i18n("The server did not save the choice. It stays as it was."));
            }
            return;
        }
        // The server's answer is the note's listening answer from now on.
        setAnswer(noteId, ListeningAnswer::fromJson(result.json));
        Q_EMIT answerOptionsChanged();
        if (done) done(true, {});
    });
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
            m_answers.remove(noteId);
            m_answeredAt.remove(noteId);
            m_choices.remove(noteId);
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
    m_client->list(watchingNow() ? WatchingLimit : IdleLimit, this, [this](const Client::Result &result) { finished(result); });
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
        int delay = watchingNow() ? WatchingIntervalMs : IdleIntervalMs;
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
    // The server's list is the one already shown (304), or the same notes came again.
    const bool changed = !result.notModified && apply(result.notes);
    m_unchangedPolls = changed ? 0 : qMin(m_unchangedPolls + 1, 16);
    Q_EMIT stateChanged();
    if (again) {
        poll();
        return;
    }
    if (!m_started && !m_watching) return;
    int delay = pollInterval();
    // A new note still inside its grace period: look again once it ends.
    const QDateTime now = m_clock();
    for (const Note &note : std::as_const(m_notes)) {
        if (note.sequence <= m_state.notificationBaseline() || m_state.isDecided(note.id) || !note.createdAt.isValid()) continue;
        const qint64 remaining = qint64(GracePeriodSeconds) * 1000 - note.createdAt.msecsTo(now);
        if (remaining > 0) delay = int(qBound(qint64(2000), remaining + 1000, qint64(delay)));
    }
    schedule(delay);
}

bool Center::apply(const QList<Note> &fetched)
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
    const bool changed = firstList || signature != m_signature;
    if (changed) {
        m_signature = signature;
        Q_EMIT notesChanged();
    }
    emitUnreadIfChanged();
    return changed;
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
