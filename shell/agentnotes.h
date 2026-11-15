// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "agentnotedrawing.h"
#include "serverlink.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QTimer;

// Agent notes on the Mac (thoughts-server protocol/agent-notes.md): the notes agents send
// to the phone, read from GET /v1/agent-notes with this computer's device token, and the
// phone's tools for them: a reply (a comment, a drawing on the picture), a page sent to the
// agent (origin.kind "phone_page", as the phone's study mode), deleting a note.
// Receipts: like the phone, the Mac says "opened" when it shows a note nobody opened yet (the
// agent then knows it was read; the first stamp wins). It never says "delivered" (the Mac stores
// nothing for the phone's sake) and never asks the inbox: the list is all it needs, and an inbox
// request would make the Mac a listener, so agents on other devices would be told a phone is
// listening live.
namespace Thoughts::AgentNotes
{

struct Reply {
    QString id;
    qint64 sequence = 0;
    QString deviceId;
    QDateTime createdAt;
    QString mediaType;
    int imageWidth = 0;
    int imageHeight = 0;
    QString imageSha256;
    QString imageUrl;
    int strokeCount = 0;
    QString message;
    // A photo sent with the reply (the phone's Send with picture: the glasses camera), else empty.
    QString photoUrl;
    QString photoSha256;
    QString photoMediaType;
    QString photoCapture;  // the glasses' capture name (IMG_…)
    int photoWidth = 0;
    int photoHeight = 0;
    // What the answering agent may use (ReplySources); empty when the sender did not say.
    QString sources;

    bool hasPhoto() const { return !photoUrl.isEmpty(); }
    static std::optional<Reply> fromJson(const QJsonObject &json);
};

// What the agent answering a reply may use to ground its answer, sent with every reply
// (server agent_note_reply_sources_v1): no Thoughts documents at all, only the PDF the note
// is linked to, every Library document, or every document and the Mental Model too.
namespace ReplySources
{
inline const QString None = QStringLiteral("none");
inline const QString LinkedPdf = QStringLiteral("linked_pdf");
inline const QString Library = QStringLiteral("library");
inline const QString LibraryAndMentalModel = QStringLiteral("library_and_mental_model");
// In the order the choice lists them.
QStringList all();
// What agents used before the choice existed, and the default.
inline QString defaultValue() { return LibraryAndMentalModel; }
// "Linked PDF only", "All documents + Mental Model"; empty for an unknown value.
QString label(const QString &value);
// One line on what the agent may use.
QString description(const QString &value);
}

// Whether an agent waits to act on replies to a note (protocol/agent-notes.md "Who hears a
// reply"): Unknown when the sender's agents never say, or before the server answered.
enum class Listening { Unknown, Listening, NotListening };
Listening listeningFromJson(const QJsonObject &json);

// An agent that can answer the user's reply to a note (protocol/agent-notes.md "The user's
// choice"): an agent of the device that sent it ("sender"), a topic owner ("owner", with the
// topic's label) or the Thoughts reply agent ("responder").
struct Agent {
    QString role;
    QString deviceId;
    QString conversationId;  // empty: none (a device-wide sender, a responder)
    QString ownerId;  // an owner's
    QString label;  // an owner's topic
    bool live = false;  // it is listening right now

    // The same agent, whatever its label or whether it is live says now.
    bool isSameAgent(const Agent &other) const;
    // How the view names it, at the start of a sentence or as a list entry: "Thoughts reply
    // agent", "Course notes agent", "The agent that sent it".
    QString name() const;
    // The agent as PUT /v1/agent-notes/{id}/answer-by names it: its identity only.
    QJsonObject toChoiceJson() const;
    // Nothing for what is not an agent the view can name (an unknown role, no device).
    static std::optional<Agent> fromJson(const QJsonObject &json);
};

// The session a reply waker would wake for a reply arriving now (protocol/agent-notes.md
// "Wakers", "wake" in every listening answer, server agent_note_wakers_v1): the note's own session
// ("sender"), a topic owner's ("owner") or the fallback session that answers what has no other
// home ("responder", the study session), on the waker's machine. While that machine is offline the
// fallback answers at fallbackAt instead.
struct Wake {
    QString state;  // "will_be_woken" or "waker_offline"
    QString role;
    QString machine;
    QString title;  // the session's; empty when the server has none
    QString sessionState;  // idle, busy (running a task) or closed
    QString label;  // an owner's topic
    QDateTime fallbackAt;  // UTC; invalid when the server gave none

    bool willBeWoken() const;
    // How the view names the session: the study session for the fallback, else its title, or its
    // agent's name when it has none.
    QString name() const;
    // What the line under the note says: "<title> will be woken on the Mac", "The study session will be
    // woken", "<title> answers when it finishes its current task" while it runs a task, or "The Mac
    // is offline; the study session answers at <local time> if it is still offline".
    QString text() const;
    // The reply box after Send: "Sent ✓ · <title> will be woken".
    QString sentText() const;
    // Nothing for what the view cannot say (an unknown state or role, no machine).
    static std::optional<Wake> fromJson(const QJsonObject &json);
};
// The machine a reply waker runs on, as the view names it: "the Mac", else "the server" (there
// are two wakers, and a text never names a host).
QString wakerMachineName(const QString &machine);

// The server's answer about who listens for replies to a note (GET …/listening, the answer to
// PUT …/answer-by). With agent_note_answer_by_v1 it also says which agent the user chose to answer
// (answerBy; none is automatic: the agent that sent the note first, then the Thoughts reply
// agent) and which agents could answer now (candidates); with a choice, the state is Listening
// exactly while the chosen agent is live. With agent_note_wakers_v1, wake is the session a reply
// waker would wake for a reply now.
struct ListeningAnswer {
    Listening state = Listening::Unknown;
    // The live listener that answers first while the state is Listening: the sender's agent,
    // then a topic owner, then the reply agent. None when nobody listens or none is named.
    std::optional<Agent> coverer;
    std::optional<Agent> answerBy;
    QList<Agent> candidates;
    std::optional<Wake> wake;

    static ListeningAnswer fromJson(const QJsonObject &json);
};

// The server capability for choosing who answers a note (answer_by, candidates, the PUT).
QString answerByCapability();

// The line a view shows with an agent's note: whether an agent listens for a reply, which one,
// and "Change".
struct AnswerLine {
    enum class Mark {
        Listening,  // an agent is listening and answers a reply
        Woken,  // a reply waker wakes the session that answers a reply (or messages it while it runs)
        Offline,  // the machine of the session that answers is offline; the study session answers later instead
        Waiting,  // the chosen agent is not listening; the Thoughts reply agent answers after a while
        Nobody,  // nobody listens: a reply is kept, but no agent acts on it
    };
    Mark mark = Mark::Nobody;
    QString text;
    bool warning = false;  // nobody will act on a reply
    bool muted = false;  // the machine that answers is offline, so the answer comes later
    bool changeOffered = false;  // the server lets the user choose who answers
    bool changeEnabled = false;  // and it answered the last request
};
// What the line says for @p answer, or nothing for no line (no answer yet, or a server that
// cannot tell). @p canChange: the server has agent_note_answer_by_v1; an older one gets the line
// without "Change". @p reachable: the last request about who listens reached the server.
// An agent that listens is named as before. Otherwise, when a reply waker would wake the session
// that answers, the line names it (Wake::text); without a wake the line is as before.
std::optional<AnswerLine> answerLine(const std::optional<ListeningAnswer> &answer, bool canChange, bool reachable);

// One entry of the list "Change" opens.
struct AnswerChoice {
    std::optional<Agent> agent;  // none: Automatic
    QString title;
    QString detail;  // a line under the title; only Automatic has one
    bool checked = false;
};
// Automatic first, then every agent that could answer now, by the names the line uses; the
// current choice is checked. A chosen agent that is not listening (so not among the candidates)
// is listed too, marked as such. Two agents that would read the same get a number.
QList<AnswerChoice> answerChoices(const ListeningAnswer &answer);

// A source the note's picture links to (server agent_note_links_v1): a rectangle of the picture
// (normalized x, y, width, height, top-left origin) and where a click goes: a Library PDF page
// (thoughts-pdf-link:<filename>.pdf?page=<0-based>) or a web page (https).
struct NoteLink {
    QRectF rect;
    QString href;
    QString label;
};

struct Note {
    QString id;
    qint64 sequence = 0;
    QDateTime createdAt;
    QString senderDeviceId;
    QString title;
    QString message;
    QString mediaType;
    int imageWidth = 0;
    int imageHeight = 0;
    QString imageSha256;
    QString imageUrl;
    QString documentId;
    QString documentFilename;
    QString reviewCardId;
    QString reviewQuestion;
    QJsonObject origin;
    QDateTime openedAt;
    bool deleted = false;
    QList<Reply> replies;
    QList<NoteLink> links;

    // A page the user sent to an agent from the phone's reader (or this Mac's): origin.kind
    // "phone_page".
    bool isPhonePage() const;
    // Who sent it: "Claude on thoughts-mac", "Agent via the connector", "You, from the phone",
    // "You, from this Mac".
    QString senderLabel() const;
    // origin.conversation_url when it is an https link, else empty.
    QUrl conversationUrl() const;

    static std::optional<Note> fromJson(const QJsonObject &json);
};

// The notes of a GET /v1/agent-notes answer ({"notes": [...]}), in the server's order
// (newest first). Records without note_id are skipped; anything else unreadable is an error.
std::optional<QList<Note>> parseNotes(const QByteArray &json, QString *error = nullptr);

// What this Mac has shown, and which new notes it has already decided to notify about.
// A small JSON file; every change is saved at once.
class ReadState
{
public:
    explicit ReadState(QString path);
    QString path() const { return m_path; }

    bool isSeen(const QString &noteId) const { return m_seen.contains(noteId); }
    // True when the note was not seen before.
    bool markSeen(const QString &noteId);

    // The highest note sequence when this Mac first listed notes: nothing up to it is
    // announced with a notification. Unset (-1) until the first successful list.
    qint64 notificationBaseline() const { return m_baseline; }
    void setNotificationBaseline(qint64 sequence);
    bool isDecided(const QString &noteId) const { return m_decided.contains(noteId); }
    void markDecided(const QString &noteId);

private:
    void load();
    void save() const;
    static void remember(QStringList &order, QSet<QString> &set, const QString &id, int limit);

    QString m_path;
    QStringList m_seenOrder;
    QSet<QString> m_seen;
    QStringList m_decidedOrder;
    QSet<QString> m_decided;
    qint64 m_baseline = -1;
};

// Unread on this Mac: not opened on any device (the phone stamps opened_at when it shows a
// note) and not shown here. The user's own phone pages are never unread.
bool isUnread(const Note &note, const ReadState &state);

// A Library document as the project catalogue lists it (.thoughts/library-projects.json).
struct LibraryDocument {
    QString id;  // document_id
    QString filename;
    QString name;
    QString projectId;
    QString projectName;
};
// The documents of the catalogue, project by project (the catalogue's order).
QList<LibraryDocument> libraryDocuments(const QJsonObject &catalogue);

// Which notes a view shows: those of some Library documents (a project's, or one), or all.
struct Filter {
    QString kind;  // "project", "document" or empty (all notes)
    QString key;  // the project's or document's id
    QString label;  // "MethodsComparison", "MethodsComparison › main"
    QStringList documentIds;
    QStringList filenames;

    bool isEmpty() const { return kind.isEmpty(); }
    bool matches(const Note &note) const;
    bool operator==(const Filter &other) const { return kind == other.kind && key == other.key; }
    // The filter for a project (by id, or by name ignoring case) or a document (by document_id,
    // Library filename, or document name when only one has it); nullopt when none matches.
    static std::optional<Filter> project(const QList<LibraryDocument> &documents, const QString &idOrName);
    static std::optional<Filter> document(const QList<LibraryDocument> &documents, const QString &idFilenameOrName);
};

// The agent-notes endpoints with the device token (Bearer, as FedoraRepository).
class Client : public QObject
{
    Q_OBJECT
public:
    enum class Failure { None, NotConnected, Unreachable, Unauthorized, Unsupported, Failed };
    struct Result {
        QList<Note> notes;
        Failure failure = Failure::None;
        QString error;
        // The server answered that its list is the one it sent last time (304 to
        // If-None-Match): `notes` is empty and the caller keeps what it has.
        bool notModified = false;
    };

    // @p cacheRoot keeps downloaded pictures by their sha256 (they never change).
    explicit Client(QString cacheRoot, QObject *parent = nullptr);

    // Defaults (on first use): THOUGHTS_SERVER_URL / THOUGHTS_TOKEN, else the saved server
    // address and the Keychain token. Under QStandardPaths' test mode only the environment
    // counts, so a test never reaches the real server with the real token.
    void setServer(const QUrl &baseUrl, const QByteArray &token);
    bool ensureServer(QString *error = nullptr);

    // The newest notes; with @p documentId only that document's (the server's own filter).
    // When the server named a version of the list (an ETag) the last time this same list was
    // asked for, the request says so and an unchanged list comes back without its notes.
    void list(int limit, QObject *context, std::function<void(const Result &)> done, const QString &documentId = {});

    // A write: POST or PUT (with a JSON body) or DELETE. @p status is the HTTP status (0 when
    // the server was not reached); @p json its answer; @p errorCode the server's error_code.
    struct WriteResult {
        int status = 0;
        QJsonObject json;
        Failure failure = Failure::None;
        QString error;
        QString errorCode;
    };
    void post(const QString &path, const QJsonObject &body, QObject *context, std::function<void(const WriteResult &)> done);
    void put(const QString &path, const QJsonObject &body, QObject *context, std::function<void(const WriteResult &)> done);
    void remove(const QString &path, QObject *context, std::function<void(const WriteResult &)> done);
    // GET /v1/agent-notes/{id}/listening. A failure (the server was not reached, or failed)
    // leaves the answer empty and the caller keeps what it knew; a server without the endpoint
    // answers Unknown.
    struct ListeningResult {
        Failure failure = Failure::None;
        ListeningAnswer answer;
    };
    void listening(const QString &noteId, QObject *context, std::function<void(const ListeningResult &)> done);
    // PUT /v1/agent-notes/{id}/answer-by: the agent that answers replies to the note, or none
    // for automatic. The answer's json is the note's listening answer as it is then.
    void setAnswerBy(const QString &noteId, const std::optional<Agent> &agent, QObject *context, std::function<void(const WriteResult &)> done);
    // What the server can do (GET /v1/health). @p ok is false when it did not say.
    void capabilities(QObject *context, std::function<void(bool ok, const QStringList &capabilities)> done);
    // A note's or reply's picture (image_url of a record), from the cache when present.
    void image(const QString &url, const QString &sha256, const QString &mediaType, QObject *context,
               std::function<void(const QImage &, const QString &error)> done);
    QString cachedImagePath(const QString &sha256, const QString &mediaType) const;

private:
    QNetworkReply *get(const QString &pathAndQuery, int timeoutMs);
    QNetworkRequest request(const QString &pathAndQuery, int timeoutMs) const;
    void finishWrite(QNetworkReply *reply, QObject *context, std::function<void(const WriteResult &)> done);
    void pruneCache();

    QString m_cacheRoot;
    ServerLink m_link;
    QNetworkAccessManager *m_network = nullptr;
    int m_downloadsSincePrune = 0;
    // The ETag of the last answer to each list request (by its query).
    QHash<QString, QByteArray> m_listVersions;
};

// The notes of this process (one for all windows): polls the list, counts unread notes and
// announces new ones that nobody opened within the grace period (the phone usually shows a
// note within seconds; the Mac speaks up only when it did not).
class Center : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<QDateTime()>;

    static Center *instance();

    Center(Client *client, QString statePath, Clock clock = {}, QObject *parent = nullptr);
    ~Center() override;

    Client *client() const { return m_client; }
    ReadState &readState() { return m_state; }

    QList<Note> notes() const { return m_notes; }
    std::optional<Note> note(const QString &noteId) const;
    int unreadCount() const;
    bool isUnread(const Note &note) const { return AgentNotes::isUnread(note, m_state); }
    void markSeen(const QString &noteId);
    void markAllSeen();

    bool hasListed() const { return m_listed; }
    bool isRefreshing() const { return m_inFlight; }
    Client::Failure lastFailure() const { return m_lastFailure; }
    QString lastError() const { return m_lastError; }
    QDateTime lastUpdated() const { return m_lastUpdated; }

    // Starts polling (first poll after @p firstDelayMs); a no-op when already started.
    void start(int firstDelayMs = 3000);
    bool isStarted() const { return m_started; }
    // A view of the notes is on screen (the window, a sidebar): poll more often and list more
    // notes while any is. Each view says so for itself.
    void setWatching(const QObject *viewer, bool watching);
    bool isWatching() const { return m_watching; }
    // Whether Thoughts is the application in front. Behind another application nobody looks at
    // its views: the list is then asked for at the slow pace, whatever is open.
    void setForeground(bool foreground);
    bool isForeground() const { return m_foreground; }
    // The pace of the next poll, in milliseconds, or -1 when none is scheduled. Each poll that
    // finds the list unchanged doubles it, up to the maximum of its kind.
    int nextPollDelayMs() const;
    // What the timer does when it fires (tests call it instead of waiting).
    void pollNow();
    // Asks the server now (the window's Refresh, or the window opening).
    void refresh();

    // A notification was clicked: windows show this note.
    void requestOpen(const QString &noteId);

    // Whether an agent listens for replies to a note, as last asked (Unknown before).
    Listening listening(const QString &noteId) const;
    // The server's last answer about who listens, with the user's choice and the agents to
    // choose from; nothing before any.
    std::optional<ListeningAnswer> answer(const QString &noteId) const;
    // Asks the server (a view showing the note asks now and then); listeningChanged follows.
    void checkListening(const QString &noteId);
    // The same, unless the server answered about this note less than @p maximumAgeMs ago.
    void checkListeningIfOlder(const QString &noteId, int maximumAgeMs);
    // Records an answer (tests). Emits listeningChanged when it changed.
    void setAnswer(const QString &noteId, const ListeningAnswer &answer);
    // Records what the server said when it took a reply: whether an agent listens and which, and
    // who a reply waker wakes for it.
    // The choice and the candidates of the answer kept so far stay with it.
    void setListening(const QString &noteId, Listening state, const std::optional<Agent> &coverer = std::nullopt,
                      const std::optional<Wake> &wake = std::nullopt);
    // What the reply box says after the last reply to a note was sent, while no new one is
    // started: "Sent ✓ · <title> will be woken" when the server named a session a waker wakes for
    // it; empty otherwise (as before).
    QString sentStatus(const QString &noteId) const;
    // Whether the server lets the user choose who answers (agent_note_answer_by_v1); asked with
    // the first check and kept.
    bool canChangeAnswerer() const { return m_canChangeAnswerer; }
    // False once a request about who listens did not reach the server, until one does.
    bool serverReachable() const { return m_serverReachable; }
    // Sends the user's choice of who answers replies to a note (none: automatic). The answer
    // kept for the note follows the server's; after a failure it is as it was, and @p error says
    // what to tell the user.
    void chooseAnswerer(const QString &noteId, const std::optional<Agent> &agent, std::function<void(bool ok, const QString &error)> done = {});
    bool isChoosingAnswerer(const QString &noteId) const { return m_choosing.contains(noteId); }

    // A note on screen on this Mac: seen here, and "opened" on the server when nobody opened
    // it yet (once per note and run; a page the user sent is never opened).
    void noteShown(const QString &noteId);

    // What the user is writing and drawing in reply to a note, kept until it is sent (in memory).
    struct Draft {
        QString message;
        AnnotationDocument drawing;  // null until the drawing view opens
        bool sending = false;
        QString error;  // the last failure, until the next try
        QString replyId;  // kept for a retry of the same reply (the server answers it once)
        QByteArray sentContent;
    };
    // The note's draft, made on first use. The pointer stays valid until the reply is sent.
    std::shared_ptr<Draft> draft(const QString &noteId);
    std::shared_ptr<Draft> existingDraft(const QString &noteId) const { return m_drafts.value(noteId); }
    void setDraftMessage(const QString &noteId, const QString &message);
    // The drawing changed (the drawing view calls it): views update their labels.
    void draftDrawingChanged(const QString &noteId);
    // Sends the draft as a reply: the drawing on the note's picture (PNG), else the picture as it
    // is, with the stroke document and the message. The draft goes once the server has it.
    void sendReply(const QString &noteId, std::function<void(bool ok, const QString &error)> done = {});
    // What the agent may use when it answers, sent with every reply; the user's last choice
    // (kept in the app's settings), ReplySources::defaultValue() until they choose.
    QString replySources() const;
    void setReplySources(const QString &value);

    // A page of a document sent to the agent, as the phone's study mode sends one.
    struct Page {
        QString noteId;  // the same for a retry
        QString title;
        QString message;
        QString documentId;
        QString documentFilename;
        QString documentName;
        QString reviewCardId;
        int page = 0;
        int pages = 0;
        QImage image;
        AnnotationDocument drawing;
    };
    void sendPage(const Page &page, std::function<void(bool ok, const QString &error)> done = {});

    // Deletes the note on the server (for every device) and here.
    void deleteNote(const QString &noteId, std::function<void(bool ok, const QString &error)> done = {});

    // Applies one list answer; @p complete when it holds every note the server has above its
    // lowest sequence (always, for the newest-first list). Public for tests. True when the
    // list changed.
    bool apply(const QList<Note> &fetched);
    // Adds or updates notes from a partial answer (one document's), removing none.
    void merge(const QList<Note> &fetched);
    // Lists these documents' notes too, older ones included (a filtered view asks; at most 20
    // documents, each once a minute).
    void fetchDocuments(const QStringList &documentIds);

    static constexpr int GracePeriodSeconds = 30;
    static constexpr int NotifyWithinSeconds = 10 * 60;
    static constexpr int IdleIntervalMs = 60 * 1000;
    static constexpr int WatchingIntervalMs = 15 * 1000;
    // While polls find the list unchanged the pace doubles, up to these.
    // An unchanged list costs the server nothing (304), so the pace stays short enough for a
    // new note to show within two minutes even when Thoughts is behind another application.
    static constexpr int IdleMaximumIntervalMs = 2 * 60 * 1000;
    static constexpr int WatchingMaximumIntervalMs = 30 * 1000;
    static constexpr int IdleLimit = 20;
    static constexpr int WatchingLimit = 100;
    // A server without the capability to choose who answers is asked again after this long
    // (it may be updated meanwhile); one that has it is not asked again.
    static constexpr int CapabilitiesKeptSeconds = 10 * 60;

Q_SIGNALS:
    // The list changed (new, changed or deleted notes); polls that change nothing are silent.
    void notesChanged();
    // Notes were shown on this Mac (or all marked as read): unread marks change, nothing else.
    void seenChanged();
    void unreadCountChanged(int count);
    void stateChanged();
    // A new note nobody opened within the grace period: worth a system notification.
    void noteNeedsAttention(const Thoughts::AgentNotes::Note &note);
    void openRequested(const QString &noteId);
    // A draft changed (its message, drawing, or sending state).
    void draftChanged(const QString &noteId);
    // What the server said about who listens for replies to a note changed.
    void listeningChanged(const QString &noteId);
    // Whether "Change" is offered, can be used now, or a choice is on its way changed.
    void answerOptionsChanged();
    void foregroundChanged(bool foreground);
    // The choice of what the agent may use changed (in any view).
    void replySourcesChanged();

private:
    void poll();
    void schedule(int ms);
    // A view is on screen and Thoughts is in front.
    bool watchingNow() const { return m_watching && m_foreground; }
    int pollInterval() const;
    void finished(const Client::Result &result);
    void decideNotifications();
    void emitUnreadIfChanged();
    void replaceNote(const Note &note);
    void postReply(const QString &noteId, const QByteArray &image, const QString &mediaType, const QSize &size);
    void finishReply(const QString &noteId, bool ok, const QString &error);

    Client *m_client = nullptr;
    ReadState m_state;
    Clock m_clock;
    QTimer *m_timer = nullptr;
    QList<Note> m_notes;
    bool m_started = false;
    bool m_watching = false;
    bool m_foreground = true;
    // Polls in a row that found the list as it was.
    int m_unchangedPolls = 0;
    QSet<const QObject *> m_watchers;
    bool m_inFlight = false;
    bool m_refreshAgain = false;
    bool m_listed = false;
    int m_failures = 0;
    int m_lastUnread = -1;
    Client::Failure m_lastFailure = Client::Failure::None;
    QString m_lastError;
    QDateTime m_lastUpdated;
    QByteArray m_signature;
    QSet<QString> m_openedPosted;
    QHash<QString, qint64> m_documentFetched;  // document id -> msecs since epoch
    QHash<QString, std::shared_ptr<Draft>> m_drafts;
    QHash<QString, std::function<void(bool, const QString &)>> m_replyCallbacks;
    QHash<QString, QString> m_sentStatus;  // note id -> sentStatus()
    void learnWhetherTheChoiceIsOffered();
    void setServerReachable(bool reachable);

    QHash<QString, ListeningAnswer> m_answers;
    QHash<QString, QDateTime> m_answeredAt;
    QSet<QString> m_listeningAsked;  // requests in flight
    QSet<QString> m_choosing;  // choices on their way
    QHash<QString, int> m_choices;  // counts the choices sent and answered, per note
    bool m_canChangeAnswerer = false;
    bool m_capabilitiesAsked = false;  // a request in flight
    QDateTime m_capabilitiesAnsweredAt;
    bool m_serverReachable = true;
};

// Human labels shared by the window and notifications.
QString relativeTime(const QDateTime &time, const QDateTime &now);

}

Q_DECLARE_METATYPE(Thoughts::AgentNotes::Note)
