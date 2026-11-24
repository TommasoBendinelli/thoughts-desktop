// SPDX-License-Identifier: GPL-2.0-or-later
#include "readingoverlays.h"

#include "desktopsyncstore.h"
#include "reviewcardfeedback.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace Thoughts::ReadingOverlays
{
namespace
{
QString string(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

bool validUuid(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.version() == QUuid::Random && uuid.toString(QUuid::WithoutBraces) == id;
}

// The next write of @p record by this computer: revision + 1, a later updated_at, our origin.
QJsonObject nextWrite(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = record;
    QDateTime at = now.toUTC();
    const QDateTime previous = QDateTime::fromString(string(record, QStringLiteral("updated_at")), Qt::ISODateWithMs);
    if (previous.isValid() && previous.toUTC() >= at) at = previous.toUTC().addMSecs(1);
    next.insert(QStringLiteral("revision"), record.value(QStringLiteral("revision")).toInteger() + 1);
    next.insert(QStringLiteral("updated_at"), ReviewCardFeedback::timestamp(at));
    next.insert(QStringLiteral("origin_id"), originId);
    return next;
}
}

QString entityType()
{
    return QStringLiteral("reading_overlay");
}

QString collection()
{
    return QStringLiteral("reading-overlays");
}

QString capability()
{
    return QStringLiteral("reading_overlays_v1");
}

QString projectionKey(const QString &overlayId)
{
    return entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n') + overlayId;
}

QString read()
{
    return QStringLiteral("read");
}

QString skim()
{
    return QStringLiteral("skim");
}

QString skip()
{
    return QStringLiteral("skip");
}

QString note()
{
    return QStringLiteral("note");
}

bool isKind(const QString &kind)
{
    return kind == read() || kind == skim() || kind == skip() || kind == note();
}

QString overlayIdOf(const QJsonObject &record)
{
    return string(record, QStringLiteral("overlay_id"));
}

QString documentIdOf(const QJsonObject &record)
{
    return string(record.value(QStringLiteral("document")).toObject(), QStringLiteral("document_id"));
}

QString pdfSha256Of(const QJsonObject &record)
{
    return string(record.value(QStringLiteral("document")).toObject(), QStringLiteral("pdf_sha256"));
}

bool isDeleted(const QJsonObject &record)
{
    return record.value(QStringLiteral("deleted")).toBool();
}

QList<QJsonObject> marksOf(const QJsonObject &record)
{
    QList<QJsonObject> result;
    for (const QJsonValue &value : record.value(QStringLiteral("marks")).toArray()) {
        if (value.isObject()) result.append(value.toObject());
    }
    return result;
}

QJsonObject markOf(const QJsonObject &record, const QString &markId)
{
    for (const QJsonObject &mark : marksOf(record)) {
        if (markIdOf(mark) == markId) return mark;
    }
    return {};
}

QString markIdOf(const QJsonObject &mark)
{
    return string(mark, QStringLiteral("mark_id"));
}

QString kindOf(const QJsonObject &mark)
{
    const QString kind = string(mark, QStringLiteral("kind"));
    return isKind(kind) ? kind : note();
}

QString commentOf(const QJsonObject &mark)
{
    return string(mark, QStringLiteral("comment")).trimmed();
}

QString noteIdOf(const QJsonObject &mark)
{
    const QString id = string(mark, QStringLiteral("note_id")).trimmed().toLower();
    return QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id ? QString() : id;
}

QString quoteOf(const QJsonObject &mark)
{
    return string(mark.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject(), QStringLiteral("exact"));
}

QString quoteStart(const QJsonObject &mark, int length)
{
    // Lines of the PDF are joined with a space; a quote may still carry line breaks.
    const QString quote = quoteOf(mark).simplified();
    const QList<uint> codePoints = quote.toUcs4();
    if (codePoints.size() <= length) return quote;
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(codePoints.constData()), length).trimmed() + QStringLiteral("…");
}

// The quote in a question's message: one line, at most questionQuoteLength characters, no ellipsis
// (reading-overlays.md §4, as the phone's ReadingOverlayQuestions.messageQuote).
static QString messageQuote(const QJsonObject &mark)
{
    const QString quote = quoteOf(mark).simplified();
    const QList<uint> codePoints = quote.toUcs4();
    if (codePoints.size() <= questionQuoteLength) return quote;
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(codePoints.constData()), questionQuoteLength).trimmed();
}

QJsonObject passageOf(const QJsonObject &mark)
{
    return QJsonObject{{QStringLiteral("anchor"), mark.value(QStringLiteral("anchor"))}};
}

QString authorLabel(const QJsonObject &record)
{
    const QJsonObject author = record.value(QStringLiteral("author")).toObject();
    const QString label = string(author, QStringLiteral("label")).trimmed();
    return label.isEmpty() ? string(author, QStringLiteral("agent")).trimmed() : label;
}

QString conversationUrl(const QJsonObject &record)
{
    return string(record.value(QStringLiteral("author")).toObject(), QStringLiteral("conversation_url")).trimmed();
}

QString conversationId(const QJsonObject &record)
{
    return string(record.value(QStringLiteral("author")).toObject(), QStringLiteral("conversation_id")).trimmed();
}

QString defaultQuestion()
{
    return QStringLiteral("Tell me more about this passage.");
}

Question question(const QJsonObject &record, const QJsonObject &mark, const QString &documentName, int page, const QString &asked)
{
    const QString kind = kindOf(mark);
    const QString comment = commentOf(mark);
    const QString text = asked.simplified();
    Question result;
    // reading-overlays.md §4, "Asking about a mark": the templates word for word. One arg() for all
    // of them, so a % in a quote or a comment stays as it is.
    result.title = QStringLiteral("About “%1” (p. %2)").arg(quoteStart(mark, questionTitleQuoteLength), QString::number(page));
    result.message = QStringLiteral("Question about a mark of your reading overlay \"%1\" on \"%2\", page %3.\n"
                                    "Mark (%4): \"%5\"\n"
                                    "Your comment: %6\n"
                                    "My question: %7\n"
                                    "Answer with a note (send_note) attached to this document.")
                         .arg(string(record, QStringLiteral("title")).simplified(), documentName.simplified(), QString::number(page), kind,
                              messageQuote(mark), comment.isEmpty() ? QStringLiteral("(none)") : comment,
                              text.isEmpty() ? defaultQuestion() : text);
    const QString conversation = conversationId(record);
    if (!conversation.isEmpty()) result.origin.insert(QStringLiteral("conversation_id"), conversation);
    result.origin.insert(QStringLiteral("reading_overlay"),
                         QJsonObject{{QStringLiteral("overlay_id"), overlayIdOf(record)}, {QStringLiteral("mark_id"), markIdOf(mark)}, {QStringLiteral("kind"), kind}});
    return result;
}

QJsonObject withoutMark(const QJsonObject &record, const QString &markId, const QString &originId, const QDateTime &now)
{
    if (record.isEmpty() || isDeleted(record) || markId.isEmpty()) return {};
    QJsonArray kept;
    bool found = false;
    for (const QJsonValue &value : record.value(QStringLiteral("marks")).toArray()) {
        if (markIdOf(value.toObject()) == markId) found = true;
        else kept.append(value);
    }
    if (!found) return {};
    // A record with no marks left is the overlay's tombstone instead (§2): the server refuses an
    // overlay without marks, so the last mark stays in it.
    if (kept.isEmpty()) return remove(record, originId, now);
    QJsonObject next = nextWrite(record, originId, now);
    next.insert(QStringLiteral("marks"), kept);
    return next;
}

QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = nextWrite(record, originId, now);
    next.insert(QStringLiteral("deleted"), true);
    return next;
}

bool applies(const QJsonObject &incoming, const QJsonObject &stored)
{
    return ReviewCardFeedback::applies(incoming, stored);
}

QJsonObject merge(const QJsonObject &stored, const QJsonObject &incoming)
{
    if (stored.isEmpty()) return incoming;
    if (!applies(incoming, stored)) return stored;
    QJsonObject result = incoming;
    // Fixed by the first write (§1). The document's PDF and filename change when the agent replaces
    // the overlay on a newer PDF version; its document_id never does.
    for (const QString &key : {QStringLiteral("overlay_id"), QStringLiteral("author"), QStringLiteral("created_at"), QStringLiteral("created_origin_id")}) {
        if (stored.contains(key)) result.insert(key, stored.value(key));
    }
    const QJsonObject storedDocument = stored.value(QStringLiteral("document")).toObject();
    if (storedDocument.contains(QStringLiteral("document_id"))) {
        QJsonObject document = result.value(QStringLiteral("document")).toObject();
        document.insert(QStringLiteral("document_id"), storedDocument.value(QStringLiteral("document_id")));
        result.insert(QStringLiteral("document"), document);
    }
    return result;
}

QString validationError(const QJsonObject &record)
{
    const auto invalid = [](const QString &what) { return QStringLiteral("The reading overlay has no valid %1.").arg(what); };
    if (!validUuid(overlayIdOf(record))) return invalid(QStringLiteral("overlay_id"));
    if (!validUuid(documentIdOf(record))) return invalid(QStringLiteral("document"));
    // The user's edits are always later revisions of an agent's overlay.
    if (record.value(QStringLiteral("revision")).toInteger() < 2) return invalid(QStringLiteral("revision"));
    if (!QDateTime::fromString(string(record, QStringLiteral("updated_at")), Qt::ISODateWithMs).isValid()) return invalid(QStringLiteral("updated_at"));
    if (string(record, QStringLiteral("origin_id")).isEmpty()) return invalid(QStringLiteral("origin_id"));
    if (!record.value(QStringLiteral("deleted")).isBool()) return invalid(QStringLiteral("deleted"));
    const QJsonArray marks = record.value(QStringLiteral("marks")).toArray();
    if (!isDeleted(record) && (marks.isEmpty() || marks.size() > maximumMarks)) return invalid(QStringLiteral("marks"));
    // The server refuses the whole batch otherwise (reading-overlays.md §1).
    static const QRegularExpression markId(QStringLiteral("^[A-Za-z0-9_-]{1,16}$"));
    QSet<QString> ids;
    for (const QJsonValue &value : marks) {
        const QString id = markIdOf(value.toObject());
        if (!markId.match(id).hasMatch() || ids.contains(id)) return invalid(QStringLiteral("mark_id"));
        ids.insert(id);
    }
    if (record.value(QStringLiteral("title")).toString().trimmed().isEmpty()) return invalid(QStringLiteral("title"));
    return {};
}

bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error)
{
    const QString problem = validationError(record);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return false;
    }
    QString queueError;
    if (!outbox.ready(&queueError)
        || outbox.enqueue(entityType(), overlayIdOf(record), collection(), record.value(QStringLiteral("revision")).toInteger() - 1, record,
                          string(record, QStringLiteral("updated_at"))).isEmpty()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The reading overlay could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

QList<QJsonObject> recordsOfDocument(const QList<QJsonObject> &records, const QString &documentId, bool includeDeleted)
{
    QList<QJsonObject> result;
    if (documentId.isEmpty()) return result;
    for (const QJsonObject &record : records) {
        if (overlayIdOf(record).isEmpty() || documentIdOf(record).compare(documentId, Qt::CaseInsensitive) != 0) continue;
        if (!includeDeleted && isDeleted(record)) continue;
        result.append(record);
    }
    // Newest first (§4).
    std::stable_sort(result.begin(), result.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const QDateTime at = QDateTime::fromString(string(a, QStringLiteral("created_at")), Qt::ISODateWithMs);
        const QDateTime bt = QDateTime::fromString(string(b, QStringLiteral("created_at")), Qt::ISODateWithMs);
        if (at.isValid() && bt.isValid() && at != bt) return at > bt;
        if (at.isValid() != bt.isValid()) return at.isValid();
        return overlayIdOf(a) < overlayIdOf(b);
    });
    return result;
}

QList<QJsonObject> recordsOfDocument(const QJsonObject &projections, const QString &documentId, bool includeDeleted)
{
    QList<QJsonObject> records;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (it.key().startsWith(prefix) && it.value().isObject()) records.append(it.value().toObject());
    }
    return recordsOfDocument(records, documentId, includeDeleted);
}
}
