#include "../part/documentidentity.h"
#include "../part/thoughtsplatform.h"
#include "../part/thoughtshome.h"
#include "../part/sourceannotationindex.h"
#include "../part/sourceowner.h"
#include "../part/projectconfiguration.h"
/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "reviewstore.h"
#include "reviewdocumenttools.h"
#include <QLockFile>
#include "../part/reviewcardregions.h"
#include "../part/reviewcardfeedback.h"
#include "../part/documentfeedback.h"
#include "../part/feedbackversion.h"
#include "../part/verifiedartifactcache.h"
#include "../core/htmlattachment.h"

#include "part/desktopsyncstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QThread>
#include <atomic>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QUuid>
#include <QStandardPaths>

#include <algorithm>
#include <optional>
#include <cmath>
#include <memory>
#include <limits>
#include <tuple>

namespace Thoughts
{
namespace
{

constexpr int schemaVersion = 3;
constexpr int maxIntervalDays = 365;
constexpr int maximumQuestionLength = 500;
constexpr int currentSchedulerVersion = 2;
constexpr int learnAheadLimitMinutes = 20;
constexpr int firstLearningStepMinutes = 1;
constexpr int secondLearningStepMinutes = 10;
constexpr int firstLearningHardMinutes = 6;
constexpr int relearningStepMinutes = 10;
constexpr int relearningHardMinutes = 15;
constexpr int graduatingIntervalDays = 3;
constexpr int easyIntervalDays = 7;
constexpr int relearningIntervalDays = 1;

QString objectString(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

bool isInferredSourceDeletion(const QJsonObject &event)
{
    if (objectString(event, QStringLiteral("rating")) != QLatin1String("delete")) return false;
    if (objectString(event, QStringLiteral("origin_id")) == QLatin1String("thoughts-source-review")
        && objectString(event, QStringLiteral("reason")) == QLatin1String("source_markers_removed")) return true;
    const auto evidence = event.value(QStringLiteral("document_deletion")).toObject();
    return objectString(event, QStringLiteral("origin_id")) == QLatin1String("thoughts-document-deletion")
        && objectString(event, QStringLiteral("reason")) == QLatin1String("document_deleted")
        && !evidence.value(QStringLiteral("source_files")).toArray().isEmpty()
        && evidence.value(QStringLiteral("filenames")).toArray().isEmpty()
        && !evidence.value(QStringLiteral("project_document_removed")).toBool();
}

static QRegularExpression cardMarkerPattern(const QString &relative, const QString &cardId, const QString &segment)
{
    const QString id = QRegularExpression::escape(cardId);
    const QString part = segment.isEmpty() ? QStringLiteral("[^}\\s:]+") : QRegularExpression::escape(segment);
    return QRegularExpression(relative.endsWith(QLatin1String(".tex"))
        ? QStringLiteral(R"(\\ReviewCardMask(?:Start|End)\s*\{%1\}\s*\{%2\})").arg(id, part)
        : QStringLiteral(R"(<!--\s*thoughts:v1:(?:start|end)\s+review:%1:answer:%2\s*-->)").arg(id, part));
}

// Removes a deleted card's markers from its source passages, all in one file. Returns
// why that is not safe now, or an empty string with the edit in before/after.
static QString cardMarkerRemoval(const QString &docsRoot, const QString &cardId, const QJsonArray &segments,
                                 QString *path, QByteArray *before, QByteArray *after)
{
    if (segments.isEmpty()) return QStringLiteral("The card has no source passage.");
    const QString relative = segments.first().toObject().value(QStringLiteral("source_file")).toString();
    QSet<QString> segmentIds;
    for (const auto &value : segments) {
        if (value.toObject().value(QStringLiteral("source_file")).toString() != relative)
            return QStringLiteral("The card's answer spans several source files.");
        segmentIds.insert(value.toObject().value(QStringLiteral("segment_id")).toString());
    }
    if (segmentIds.size() != segments.size()) return QStringLiteral("The card's source passages are not identified uniquely.");
    const auto location = annotationSourceLocation(docsRoot, QStringLiteral("review_card"), cardId, SourceOwner::of(relative));
    if (location.value(QStringLiteral("state")).toString() != QLatin1String("linked") || location.value(QStringLiteral("source_file")).toString() != relative)
        return QStringLiteral("The card's markers are not at one indexed source location yet.");
    *path = QDir(docsRoot).filePath(relative);
    QFile file(*path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("The source file could not be read.");
    *before = file.readAll();
    if (location.value(QStringLiteral("source_sha256")).toString() != QString::fromLatin1(QCryptographicHash::hash(*before, QCryptographicHash::Sha256).toHex()))
        return QStringLiteral("The source changed after Thoughts last indexed it.");
    QString reason;
    *after = removeReviewCardMarkers(relative, *before, cardId, segmentIds, &reason);
    return reason;
}

// Whether any marker of the card remains in its source file.
static bool cardMarkersPresent(const QString &docsRoot, const QJsonArray &segments, const QString &cardId)
{
    for (const auto &value : segments) {
        const QString relative = value.toObject().value(QStringLiteral("source_file")).toString();
        QFile file(QDir(docsRoot).filePath(relative));
        if (!file.exists()) continue;
        // An unreadable source may still hold the markers.
        if (!file.open(QIODevice::ReadOnly)) return true;
        if (cardMarkerPattern(relative, cardId, {}).match(QString::fromUtf8(file.readAll())).hasMatch()) return true;
    }
    return false;
}

QJsonObject primaryObject(const QJsonObject &card)
{
    return card.value(QStringLiteral("primary")).toObject();
}

// Where reviewCardAgentFeedback reads @p card's answer: its PDF, that PDF's hash and its regions.
ReviewCardAgentInput agentFeedbackAnswer(const QString &docsRoot, const ReviewCard &card)
{
    ReviewCardAgentInput answer;
    answer.cardId = card.id();
    answer.pdfPath = reviewCardPdfPath(docsRoot, card.record);
    if (answer.pdfPath.isEmpty() && !card.pdfStem().isEmpty()) answer.pdfPath = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(card.pdfStem()));
    answer.pdfSha256 = card.pdfSha256();
    answer.regions = QJsonArray::fromVariantList(card.regions());
    answer.page = card.page();
    return answer;
}

// Its answer when its PDF cannot be read: the source text between its markers, else the text
// its answer anchor recorded (as thoughtsctl search-review-cards reads it).
QString agentFeedbackAnswerText(const QString &docsRoot, const ReviewCard &card)
{
    QStringList files;
    const QJsonObject primary = primaryObject(card.record);
    for (const QJsonValue &value : primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray()) {
        const QString file = value.toObject().value(QStringLiteral("source_file")).toString().trimmed();
        if (!file.isEmpty() && !files.contains(file)) files.append(file);
    }
    if (!card.linkingSource().trimmed().isEmpty() && !files.contains(card.linkingSource().trimmed())) files.append(card.linkingSource().trimmed());
    QStringList texts;
    for (const auto &passage : reviewCardSourcePassages(docsRoot, card.id(), files))
        for (const auto &segment : passage.segments) texts.append(segment.text);
    return texts.isEmpty() ? primary.value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString() : texts.join(QStringLiteral("\n\n"));
}

QString sha256(const QByteArray &contents)
{
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

/**
 * Whether two lists of a card's source passages describe the same answer. A card with several
 * identified passages compares them by segment id, since file order need not follow segment
 * order. Any other card compares contents by position, and ids only where the saved passage has
 * one: some older cards were saved without segment ids, while the source scan always reports them.
 */
bool sameReviewPassages(const QJsonArray &saved, const QJsonArray &observed)
{
    if (saved.isEmpty() || saved.size() != observed.size()) return false;
    const auto id = [](const QJsonValue &value) { return value.toObject().value(QStringLiteral("segment_id")).toString(); };
    const auto content = [](const QJsonValue &value) { return value.toObject().value(QStringLiteral("content_sha256")).toString(); };
    const bool identified = std::all_of(saved.cbegin(), saved.cend(), [&id](const QJsonValue &value) { return !id(value).isEmpty(); });
    if (saved.size() > 1 && identified) {
        QHash<QString, QString> expected, found;
        for (const auto &value : saved) expected.insert(id(value), content(value));
        for (const auto &value : observed) found.insert(id(value), content(value));
        return expected == found;
    }
    for (int i = 0; i < saved.size(); ++i) {
        if (content(saved[i]) != content(observed[i])) return false;
        if (!id(saved[i]).isEmpty() && id(saved[i]) != id(observed[i])) return false;
    }
    return true;
}

// An editable source of a docs-* folder or a Project's default folder:
// anywhere in it, as LaTeX entry points and their inputs may be, but not in a
// hidden or generated part.
bool editableReviewSource(const QString &relative)
{
    static const QStringList generated{QStringLiteral("__pycache__"), QStringLiteral("build"), QStringLiteral("current_pdfs"),
        QStringLiteral("node_modules"), QStringLiteral("pdf"), QStringLiteral("tmp"), QStringLiteral("venv")};
    const QString owner = SourceOwner::of(relative);
    if (owner.isEmpty() || relative.size() <= owner.size() + 1) return false;
    const auto parts = relative.mid(owner.size() + 1).split(QLatin1Char('/'));
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const auto &part = parts.at(index);
        if (part.isEmpty() || part.startsWith(QLatin1Char('.')) || (index + 1 < parts.size() && generated.contains(part))) return false;
    }
    return true;
}

QJsonObject currentReviewSourceAnchor(const QString &root, const QString &id, const QJsonObject &anchor,
                                      const QString &failedSource = {})
{
    const bool initialLink = anchor.isEmpty() && !failedSource.isEmpty();
    if (!initialLink && anchor.value(QStringLiteral("version")).toInt() != 2) return {};
    const auto previous = anchor.value(QStringLiteral("segments")).toArray();
    if (!initialLink && previous.isEmpty()) return {};
    const QString knownSource = initialLink ? failedSource : previous.first().toObject().value(QStringLiteral("source_file")).toString();
    if (initialLink && (!editableReviewSource(knownSource) || !knownSource.endsWith(QLatin1String(".tex")))) return {};
    const QString project = SourceOwner::of(knownSource);
    const auto location = annotationSourceLocation(root, QStringLiteral("review_card"), id, project, knownSource);
    if (location.value(QStringLiteral("state")).toString() != QLatin1String("linked")) return {};
    const auto evidence = reviewSourceEvidence(root, id, project, knownSource);
    const auto ranges = location.contains(QStringLiteral("ranges")) ? location.value(QStringLiteral("ranges")).toArray() : QJsonArray{location};
    if (evidence.isEmpty() || evidence.size() != ranges.size() || evidence.size() > 20) return {};
    const QString path = location.value(QStringLiteral("source_file")).toString();
    const QString sourceSha = location.value(QStringLiteral("source_sha256")).toString();
    if (path.isEmpty() || sourceSha.size() != 64) return {};
    // Evidence and ranges are both recorded at each end marker, in file order.
    QList<QJsonObject> found;
    for (int i = 0; i < evidence.size(); ++i) {
        QJsonObject segment = evidence[i].toObject();
        if (segment.value(QStringLiteral("segment_id")).toString().isEmpty()
            || segment.value(QStringLiteral("content_sha256")).toString().size() != 64) return {};
        segment.insert(QStringLiteral("source_file"), path);
        segment.insert(QStringLiteral("source_sha256"), sourceSha);
        segment.insert(QStringLiteral("line_start"), ranges[i].toObject().value(QStringLiteral("start_line")));
        segment.insert(QStringLiteral("line_end"), ranges[i].toObject().value(QStringLiteral("end_line")));
        found.append(segment);
    }
    std::sort(found.begin(), found.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("segment_id")).toString().toInt() < b.value(QStringLiteral("segment_id")).toString().toInt();
    });
    QJsonArray segments;
    for (const auto &segment : std::as_const(found)) segments.append(segment);
    if (previous == segments) return anchor;
    QFile source(QDir(root).filePath(path));
    if (!source.open(QIODevice::ReadOnly) || sha256(source.readAll()) != sourceSha) return {};
    if (initialLink) {
        if (!path.startsWith(project + QLatin1Char('/')) || !editableReviewSource(path)
            || !path.endsWith(QLatin1String(".tex"))) return {};
        source.seek(0);
        const auto plan = readReviewSourceAnchor(path, source.readAll(), id);
        if (!plan.error.isEmpty() || plan.anchor.value(QStringLiteral("segments")).toArray() != segments) return {};
        return plan.anchor;
    }
    auto current = anchor;
    current.insert(QStringLiteral("segments"), segments);
    return current;
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QByteArray contents = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

// The parsed JSON files kept in memory, by the memory they take. One entry per
// file: a file that is written again replaces its earlier version here instead
// of leaving it behind (the source index is rewritten with every Sync).
constexpr qint64 KeptObjectBytesLimit = 16 * 1024 * 1024;

QJsonObject readObject(const QString &path, QString *error)
{
    struct KeptObject {
        QString identity;
        QJsonObject object;
    };
    static QMutex mutex;
    static QCache<QString, KeptObject> cache(KeptObjectBytesLimit);
    const QString identity = artifactFileIdentity(path);
    {
        QMutexLocker lock(&mutex);
        if (const auto *cached = cache.object(path); cached && !identity.isEmpty() && cached->identity == identity) return cached->object;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.errorString();
        }
        return {};
    }
    const QJsonObject result = document.object();
    if (!identity.isEmpty() && artifactFileIdentity(path) == identity) {
        QMutexLocker lock(&mutex);
        cache.insert(path, new KeptObject {identity, result}, qMax<qsizetype>(1, file.size() * ParsedJsonHeapFactor));
    }
    return result;
}

QList<QJsonObject> readObjectsBelow(const QString &path, QStringList *errors)
{
    QList<QJsonObject> output;
    QDirIterator iterator(path, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString filePath = iterator.next();
        QString error;
        const QJsonObject object = readObject(filePath, &error);
        if (object.isEmpty()) {
            if (errors) {
                errors->append(QStringLiteral("%1: %2").arg(filePath, error));
            }
            continue;
        }
        output.append(object);
    }
    return output;
}

bool validSha256(const QString &value)
{
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        return character.isDigit() || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
    });
}

bool validRect(const QJsonArray &rect)
{
    if (rect.size() != 4) {
        return false;
    }
    const double left = rect.at(0).toDouble(-1.0);
    const double top = rect.at(1).toDouble(-1.0);
    const double right = rect.at(2).toDouble(-1.0);
    const double bottom = rect.at(3).toDouble(-1.0);
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) && std::isfinite(bottom) && left >= 0.0 && top >= 0.0 && right <= 1.0 && bottom <= 1.0 && left < right && top < bottom;
}

bool validQuestion(const QString &question)
{
    const QString normalized = question.trimmed();
    return !normalized.isEmpty() && normalized.size() <= maximumQuestionLength;
}

bool validReviewCardRecord(const QJsonObject &record)
{
    const int version = record.value(QStringLiteral("schema_version")).toInt();
    return (version == schemaVersion || (version == 4 && primaryObject(record).contains(QStringLiteral("regions"))))
        && !QUuid(objectString(record, QStringLiteral("card_id"))).isNull()
        && validQuestion(objectString(primaryObject(record), QStringLiteral("question")));
}

double intersectionOverUnion(const QJsonArray &first, const QJsonArray &second)
{
    if (!validRect(first) || !validRect(second)) {
        return 0.0;
    }
    const double left = std::max(first.at(0).toDouble(), second.at(0).toDouble());
    const double top = std::max(first.at(1).toDouble(), second.at(1).toDouble());
    const double right = std::min(first.at(2).toDouble(), second.at(2).toDouble());
    const double bottom = std::min(first.at(3).toDouble(), second.at(3).toDouble());
    const double intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
    const double firstArea = (first.at(2).toDouble() - first.at(0).toDouble()) * (first.at(3).toDouble() - first.at(1).toDouble());
    const double secondArea = (second.at(2).toDouble() - second.at(0).toDouble()) * (second.at(3).toDouble() - second.at(1).toDouble());
    return intersection / (firstArea + secondArea - intersection);
}

QDateTime parseUtc(const QString &value)
{
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    if (parsed.isValid()) {
        parsed = parsed.toUTC();
    }
    return parsed;
}

QString utcString(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODate);
}

bool reviewRating(const QString &rating)
{
    return rating == QLatin1String("hard") || rating == QLatin1String("good") || rating == QLatin1String("easy")
        || rating == QLatin1String("again") || rating == QLatin1String("delete") || rating == QLatin1String("retained")
        || rating == QLatin1String("ten_minutes") || rating == QLatin1String("next_day") || rating == QLatin1String("next_week")
        || rating == QLatin1String("custom_interval");
}

QHash<QString, QDateTime> skippedUntilByCard(const QList<QJsonObject> &events)
{
    struct Skip { QString cardId; QString key; QDateTime until; };
    QList<Skip> skips;
    QSet<QString> undone;
    for (const QJsonObject &event : events) {
        const QString action = objectString(event, QStringLiteral("queue_action"));
        const QString cardId = objectString(event, QStringLiteral("card_id"));
        const QString skipId = objectString(event, QStringLiteral("skip_id"));
        const QDateTime at = parseUtc(objectString(event, QStringLiteral("reviewed_at")));
        if (event.value(QStringLiteral("schema_version")).toInt() != 3
            || QUuid(objectString(event, QStringLiteral("event_id"))).isNull()
            || QUuid(cardId).isNull() || QUuid(skipId).isNull() || !at.isValid()) {
            continue;
        }
        const QString key = QUuid(cardId).toString(QUuid::WithoutBraces) + QLatin1Char('\n') + QUuid(skipId).toString(QUuid::WithoutBraces);
        if (action == QLatin1String("undo_skip")) {
            undone.insert(key);
        } else if (action == QLatin1String("skip")) {
            const QDateTime until = parseUtc(objectString(event, QStringLiteral("skip_until")));
            if (until.isValid() && until > at) {
                skips.append({cardId, key, until});
            }
        }
    }
    QHash<QString, QDateTime> result;
    for (const Skip &skip : skips) {
        if (!undone.contains(skip.key) && (!result.value(skip.cardId).isValid() || result.value(skip.cardId) < skip.until)) {
            result.insert(skip.cardId, skip.until);
        }
    }
    return result;
}

}

namespace ReviewRetire
{
QString retireAction() { return QStringLiteral("retire"); }
QString restoreAction() { return QStringLiteral("restore"); }
QString undoAction() { return QStringLiteral("undo_retire"); }
QString undoneField() { return QStringLiteral("undone_event_id"); }

bool isEvent(const QJsonObject &event)
{
    const QString action = objectString(event, QStringLiteral("queue_action"));
    return action == retireAction() || action == restoreAction() || action == undoAction();
}

QHash<QString, State> fold(const QList<QJsonObject> &events)
{
    // The first copy of an event ID counts, as on the server; events never change.
    QHash<QString, QJsonObject> unique;
    for (const QJsonObject &event : events) {
        const QString eventId = objectString(event, QStringLiteral("event_id"));
        if (!eventId.isEmpty() && !unique.contains(eventId)) unique.insert(eventId, event);
    }
    struct Action {
        QString eventId, cardId, action, at;
        QDateTime reviewedAt;
    };
    QHash<QString, Action> actions; // event_id -> retire or restore
    QHash<QString, State> states;
    QList<QPair<QString, QString>> undos; // (card_id, undone event_id)
    for (const QJsonObject &event : std::as_const(unique)) {
        const QString cardId = objectString(event, QStringLiteral("card_id"));
        if (cardId.isEmpty()) continue;
        const QString action = objectString(event, QStringLiteral("queue_action"));
        // Deletion wins; an inferred source deletion is not one (review_reanchor._deleted_review_card_ids).
        if (action.isEmpty() && objectString(event, QStringLiteral("rating")) == QLatin1String("delete") && !isInferredSourceDeletion(event)) {
            states[cardId].deleted = true;
        } else if (action == retireAction() || action == restoreAction()) {
            const QString at = objectString(event, QStringLiteral("reviewed_at"));
            const QDateTime reviewedAt = parseUtc(at);
            if (reviewedAt.isValid()) actions.insert(objectString(event, QStringLiteral("event_id")), {objectString(event, QStringLiteral("event_id")), cardId, action, at, reviewedAt});
        } else if (action == undoAction()) {
            undos.append({cardId, objectString(event, undoneField())});
        }
    }
    // An undo cancels a retire or restore of its own card, whenever either arrived.
    for (const auto &[cardId, undone] : std::as_const(undos)) {
        const auto target = actions.constFind(undone);
        if (target != actions.cend() && target->cardId == cardId) actions.erase(target);
    }
    QHash<QString, Action> latest;
    for (const Action &action : std::as_const(actions)) {
        const auto current = latest.constFind(action.cardId);
        if (current == latest.cend() || current->reviewedAt < action.reviewedAt
            || (current->reviewedAt == action.reviewedAt && current->eventId < action.eventId))
            latest.insert(action.cardId, action);
    }
    for (const Action &action : std::as_const(latest)) {
        State &state = states[action.cardId];
        if (state.deleted || action.action != retireAction()) continue;
        state.retired = true;
        state.retiredAt = action.reviewedAt;
        state.retiredAtText = action.at;
        state.retireEventId = action.eventId;
    }
    return states;
}
}

namespace ReviewTags
{
QString correct() { return QStringLiteral("correct"); }
QString wrong() { return QStringLiteral("wrong"); }
QString skip() { return QStringLiteral("skip"); }
// Skip postpones a card without answering it, so it is not a tag.
QStringList builtIn() { return {correct(), wrong()}; }

QString normalize(const QString &text, const QHash<QString, QString> &builtInLabels)
{
    const QString tag = text.simplified().left(1000).trimmed();
    if (tag.isEmpty()) return {};
    for (const QString &id : builtIn()) {
        if (id.compare(tag, Qt::CaseInsensitive) == 0 || builtInLabels.value(id).compare(tag, Qt::CaseInsensitive) == 0) return id;
    }
    return tag;
}

QString shortLabel(const QString &label)
{
    constexpr int characters = 22;
    constexpr int words = 3;
    if (label.size() <= characters) return label;
    const QStringList parts = label.split(QLatin1Char(' '));
    QString result = parts.constFirst();
    for (int index = 1; index < std::min<int>(parts.size(), words); ++index) {
        if (result.size() + 1 + parts.at(index).size() > characters) break;
        result += QLatin1Char(' ') + parts.at(index);
    }
    return result.left(characters).trimmed() + QStringLiteral("…");
}

QStringList byPopularity(const QHash<QString, int> &usage, const QStringList &extra, const QHash<QString, QString> &merges)
{
    QStringList tags;
    for (const QString &tag : builtIn()) {
        if (!merges.contains(tag)) tags.append(tag);
    }
    for (const QString &tag : usage.keys() + extra) {
        if (!tags.contains(tag) && tag != skip()) tags.append(tag);
    }
    const QStringList order = builtIn();
    std::stable_sort(tags.begin(), tags.end(), [&](const QString &a, const QString &b) {
        if (usage.value(a) != usage.value(b)) return usage.value(a) > usage.value(b);
        const int ai = order.contains(a) ? order.indexOf(a) : std::numeric_limits<int>::max();
        const int bi = order.contains(b) ? order.indexOf(b) : std::numeric_limits<int>::max();
        if (ai != bi) return ai < bi;
        return a.toLower() < b.toLower();
    });
    return tags;
}

QStringList byUsage(QStringList tags, const QHash<QString, int> &usage, const std::function<QString(const QString &)> &label)
{
    QHash<QString, QString> labels;
    for (const QString &tag : std::as_const(tags)) labels.insert(tag, label ? label(tag) : tag);
    std::stable_sort(tags.begin(), tags.end(), [&](const QString &a, const QString &b) {
        if (usage.value(a) != usage.value(b)) return usage.value(a) > usage.value(b);
        if (const int order = labels.value(a).compare(labels.value(b), Qt::CaseInsensitive)) return order < 0;
        return a < b;
    });
    return tags;
}

QString withCount(const QString &label, int count)
{
    return label + QStringLiteral(" (") + QString::number(count) + QLatin1Char(')');
}

QString chipText(const QString &label, int count)
{
    return withCount(shortLabel(label), count);
}

QString legacyReviewAction(const QStringList &tags)
{
    if (tags.contains(wrong())) return QStringLiteral("wrong");
    if (tags.contains(correct())) return QStringLiteral("correct");
    if (tags.contains(skip())) return QStringLiteral("skip");
    return {};
}

QStringList ofEvent(const QJsonObject &event)
{
    QStringList tags;
    for (const auto &value : event.value(QStringLiteral("review_tags")).toArray()) {
        const QString tag = normalize(value.toString());
        // Earlier skips were recorded as a "skip" tag; they keep the card's tag.
        if (!tag.isEmpty() && tag.compare(skip(), Qt::CaseInsensitive) != 0 && !tags.contains(tag)) tags.append(tag);
    }
    if (!tags.isEmpty()) return tags;
    const auto queue = event.value(QStringLiteral("queue_action")).toString();
    // An answer given with no tag records an empty list: it has none, not an older client's Correct.
    if (event.value(QStringLiteral("review_tags")).isArray() && queue.isEmpty()) return {};
    const auto action = event.value(QStringLiteral("review_action")).toString();
    const auto rating = event.value(QStringLiteral("rating")).toString();
    if (queue == QLatin1String("skip") || action == QLatin1String("skip")
        || event.value(QStringLiteral("review_tags")).toArray().size() > 0) return {};
    if (!queue.isEmpty() || !event.value(QStringLiteral("interval_action")).toString().isEmpty()
        || !event.value(QStringLiteral("retracted_event_id")).toString().isEmpty()
        || rating.isEmpty() || rating == QLatin1String("delete")) return {};
    if (action == QLatin1String("wrong") || action == QLatin1String("error") || rating == QLatin1String("again") || rating == QLatin1String("ten_minutes"))
        return {wrong()};
    return {correct()};
}

QString mergeEntityType() { return QStringLiteral("review_tag_merge"); }
QString mergeCollection() { return QStringLiteral("review-tags"); }

QHash<QString, QString> mergeMapping(const QList<QJsonObject> &merges)
{
    struct Merge {
        QString id, createdAt, to;
        QStringList from;
    };
    QList<Merge> parsed;
    QSet<QString> seen;
    for (const QJsonObject &payload : merges) {
        // Malformed merges are ignored, as on Android.
        Merge merge{payload.value(QStringLiteral("merge_id")).toString(), payload.value(QStringLiteral("created_at")).toString(),
                    payload.value(QStringLiteral("to")).toString().trimmed(), {}};
        for (const auto &value : payload.value(QStringLiteral("from")).toArray()) {
            const QString tag = value.toString().trimmed();
            if (!tag.isEmpty()) merge.from.append(tag);
        }
        if (merge.id.trimmed().isEmpty() || merge.to.isEmpty() || merge.from.isEmpty() || seen.contains(merge.id)) continue;
        seen.insert(merge.id);
        parsed.append(merge);
    }
    std::sort(parsed.begin(), parsed.end(), [](const Merge &a, const Merge &b) {
        return a.createdAt != b.createdAt ? a.createdAt < b.createdAt : a.id < b.id;
    });
    QHash<QString, QString> mapping;
    for (const Merge &merge : std::as_const(parsed)) {
        const QString target = mapping.value(merge.to, merge.to);
        for (const QString &source : merge.from) {
            if (source == target) continue;
            mapping.insert(source, target);
            // Keep every value final, so a later merge of a merge target carries its sources along.
            for (auto it = mapping.begin(); it != mapping.end(); ++it) {
                if (it.value() == source) it.value() = target;
            }
        }
    }
    return mapping;
}

QString resolve(const QString &tag, const QHash<QString, QString> &mapping)
{
    return mapping.value(tag, tag);
}

QStringList applyMerges(const QStringList &tags, const QHash<QString, QString> &mapping)
{
    if (mapping.isEmpty()) return tags;
    QStringList result;
    for (const QString &tag : tags) {
        const QString name = resolve(tag, mapping);
        if (!result.contains(name)) result.append(name);
    }
    return result;
}

QString groupsEntityType() { return QStringLiteral("review_tag_groups"); }
QString groupsId() { return QStringLiteral("groups"); }
QString groupsCapability() { return QStringLiteral("review_tag_groups_v1"); }
QString otherGroupName() { return QStringLiteral("Other"); }

namespace
{
// The server counts characters as Python does, in code points.
qsizetype codePoints(const QString &text)
{
    return text.toUcs4().size();
}

constexpr int maximumGroups = 50;
constexpr int maximumGroupIdLength = 64;
constexpr int maximumGroupNameLength = 80;
constexpr int maximumGroupedTagLength = 200;
constexpr int maximumGroupedTags = 1000;
constexpr int maximumSectionLength = 60;
}

QString groupsError(const QList<TagGroup> &groups)
{
    if (groups.size() > maximumGroups) return QStringLiteral("There can be at most %1 groups.").arg(maximumGroups);
    QSet<QString> ids, names;
    int total = 0;
    for (const TagGroup &group : groups) {
        if (group.id.trimmed().isEmpty() || codePoints(group.id) > maximumGroupIdLength) return QStringLiteral("A group has an invalid ID.");
        if (ids.contains(group.id)) return QStringLiteral("Two groups have the same ID.");
        ids.insert(group.id);
        const QString name = group.name.trimmed();
        if (name.isEmpty()) return QStringLiteral("A group needs a name.");
        if (codePoints(name) > maximumGroupNameLength)
            return QStringLiteral("Group names can be at most %1 characters long.").arg(maximumGroupNameLength);
        if (name.toCaseFolded() == otherGroupName().toCaseFolded())
            return QStringLiteral("“%1” is the group of tags in no group; choose another name.").arg(otherGroupName());
        if (names.contains(name.toCaseFolded())) return QStringLiteral("There is already a group called “%1”.").arg(name);
        names.insert(name.toCaseFolded());
        QSet<QString> tags;
        for (const QString &tag : group.tags) {
            if (tag.trimmed().isEmpty()) return QStringLiteral("A group lists an empty tag.");
            if (codePoints(tag) > maximumGroupedTagLength)
                return QStringLiteral("Tags longer than %1 characters cannot be put in a group.").arg(maximumGroupedTagLength);
            if (tags.contains(tag)) return QStringLiteral("A group lists the same tag twice.");
            tags.insert(tag);
        }
        QSet<QString> steps;
        for (const QString &step : group.scale) {
            if (!tags.contains(step)) return QStringLiteral("A slider step of “%1” is not one of its tags.").arg(name);
            if (steps.contains(step)) return QStringLiteral("A slider lists the same step twice.");
            steps.insert(step);
        }
        if (!group.section.isEmpty() && group.section.trimmed().isEmpty()) return QStringLiteral("A section needs a name.");
        if (codePoints(group.section.trimmed()) > maximumSectionLength)
            return QStringLiteral("Section names can be at most %1 characters long.").arg(maximumSectionLength);
        if (!group.phase.isEmpty() && group.phase != beforePhase() && group.phase != beforeAndAfterPhase())
            return QStringLiteral("“%1” is asked at an unknown time.").arg(name);
        total += group.tags.size();
    }
    if (total > maximumGroupedTags) return QStringLiteral("Groups can hold at most %1 tags in total.").arg(maximumGroupedTags);
    return {};
}

bool parseGroups(const QJsonObject &record, QList<TagGroup> *groups, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = QStringLiteral("invalid review tag groups: %1").arg(message);
        return false;
    };
    if (record.value(QStringLiteral("layout_id")).toString() != groupsId()) return fail(QStringLiteral("layout_id is not \"groups\""));
    if (!record.value(QStringLiteral("schema_version")).isDouble() || record.value(QStringLiteral("schema_version")).toDouble() != 1)
        return fail(QStringLiteral("unsupported schema_version"));
    if (!record.value(QStringLiteral("groups")).isArray()) return fail(QStringLiteral("groups is not a list"));
    QList<TagGroup> parsed;
    QSet<QString> seen;
    for (const auto &value : record.value(QStringLiteral("groups")).toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("a group is not an object"));
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("group_id")).isString() || !object.value(QStringLiteral("name")).isString()
            || !(object.value(QStringLiteral("tags")).isArray() || object.value(QStringLiteral("tags")).isUndefined()))
            return fail(QStringLiteral("a group has the wrong fields"));
        TagGroup group{object.value(QStringLiteral("group_id")).toString(), object.value(QStringLiteral("name")).toString().trimmed(), {}};
        for (const auto &tag : object.value(QStringLiteral("tags")).toArray()) {
            if (!tag.isString()) return fail(QStringLiteral("a tag is not a string"));
            // A tag listed again, in this group or another, stays in its first group.
            if (!seen.contains(tag.toString())) group.tags.append(tag.toString());
            seen.insert(tag.toString());
        }
        // A slider's steps, each once, as on the phone: a step missing from the group's tags is
        // added to them, and one already in an earlier group's tags is no step here. A malformed
        // scale only loses the slider.
        const QJsonValue scale = object.value(QStringLiteral("scale"));
        if (scale.isArray()) {
            for (const auto &value : scale.toArray()) {
                const QString step = value.toString();
                if (!value.isString() || step.trimmed().isEmpty() || group.scale.contains(step)) continue;
                if (!group.tags.contains(step)) {
                    if (seen.contains(step)) continue;
                    group.tags.append(step);
                    seen.insert(step);
                }
                group.scale.append(step);
            }
        }
        // A scale that cannot be read is dropped on the next save: the server would refuse it.
        // So is a section that is not a name the server accepts; the group only leaves its section.
        const QString section = object.value(QStringLiteral("section")).toString().trimmed();
        if (codePoints(section) <= maximumSectionLength) group.section = section;
        group.extra = unknownRecordFields(object);
        // A phase this client knows is its own; another value (a newer client's) stays in extra,
        // written back unchanged, and the group is asked after the answer.
        if (const QString phase = object.value(QStringLiteral("phase")).toString(); phase == beforePhase() || phase == beforeAndAfterPhase()) {
            group.phase = phase;
            group.extra.remove(QStringLiteral("phase"));
        }
        parsed.append(group);
    }
    const QString problem = groupsError(parsed);
    if (!problem.isEmpty()) return fail(problem);
    if (groups) *groups = parsed;
    return true;
}

QStringList onlySteps(const QStringList &tags, const QList<TagGroup> &groups, const QHash<QString, QString> &mapping)
{
    QSet<QString> steps;
    for (const TagGroup &group : effectiveGroups(groups, mapping)) {
        for (const QString &step : group.scale) steps.insert(step);
    }
    QStringList kept;
    for (const QString &tag : tags) {
        if (steps.contains(resolve(tag, mapping))) kept.append(tag);
    }
    return kept;
}

QList<TagGroup> effectiveGroups(const QList<TagGroup> &groups, const QHash<QString, QString> &mapping)
{
    // Skip postpones a card; it is not a tag and is never grouped.
    const auto shownName = [&mapping](const QString &stored, bool *renamed) {
        const QString normalized = normalize(stored);
        if (normalized.isEmpty() || normalized.compare(skip(), Qt::CaseInsensitive) == 0) return QString();
        const QString tag = resolve(normalized, mapping);
        *renamed = tag != normalized;
        return tag;
    };
    // A merged tag shows in the group that lists its final name; failing that, in the first
    // group that lists one of its old names. Either way it shows once.
    QHash<QString, qsizetype> owner;
    for (const bool takeRenamed : {false, true}) {
        for (qsizetype index = 0; index < groups.size(); ++index) {
            for (const QString &stored : groups.at(index).tags) {
                bool renamed = false;
                const QString tag = shownName(stored, &renamed);
                if (!tag.isEmpty() && renamed == takeRenamed && !owner.contains(tag)) owner.insert(tag, index);
            }
        }
    }
    QList<TagGroup> result;
    for (qsizetype index = 0; index < groups.size(); ++index) {
        TagGroup shown{groups.at(index).id, groups.at(index).name.trimmed(), {}, {}, groups.at(index).section, groups.at(index).extra, groups.at(index).phase};
        for (const QString &stored : groups.at(index).tags) {
            bool renamed = false;
            const QString tag = shownName(stored, &renamed);
            if (!tag.isEmpty() && owner.value(tag, -1) == index && !shown.tags.contains(tag)) shown.tags.append(tag);
        }
        // Steps are shown under their merged names too; two steps merged into one count once.
        for (const QString &stored : groups.at(index).scale) {
            bool renamed = false;
            const QString tag = shownName(stored, &renamed);
            if (!tag.isEmpty() && shown.tags.contains(tag) && !shown.scale.contains(tag)) shown.scale.append(tag);
        }
        result.append(shown);
    }
    return result;
}

QJsonObject groupsRecord(const QList<TagGroup> &groups, const QString &updatedAt, const QString &originId, const QJsonObject &extra)
{
    QJsonArray values;
    for (const TagGroup &group : groups) {
        // Unknown fields first, so this client's own fields always win.
        QJsonObject value = group.extra;
        value.insert(QStringLiteral("group_id"), group.id);
        value.insert(QStringLiteral("name"), group.name.trimmed());
        value.insert(QStringLiteral("tags"), QJsonArray::fromStringList(group.tags));
        if (!group.scale.isEmpty()) value.insert(QStringLiteral("scale"), QJsonArray::fromStringList(group.scale));
        if (!group.section.trimmed().isEmpty()) value.insert(QStringLiteral("section"), group.section.trimmed());
        if (!group.phase.isEmpty()) value.insert(QStringLiteral("phase"), group.phase);
        values.append(value);
    }
    QJsonObject record = extra;
    record.insert(QStringLiteral("layout_id"), groupsId());
    record.insert(QStringLiteral("schema_version"), 1);
    record.insert(QStringLiteral("groups"), values);
    record.insert(QStringLiteral("updated_at"), updatedAt);
    record.insert(QStringLiteral("origin_id"), originId);
    return record;
}

QJsonObject unknownRecordFields(const QJsonObject &record)
{
    // Record fields and group fields together: neither level uses the other's names. A group's
    // "phase" is left here; parseGroups() takes the values it knows.
    static const QStringList known{QStringLiteral("layout_id"), QStringLiteral("schema_version"), QStringLiteral("groups"),
                                   QStringLiteral("updated_at"), QStringLiteral("origin_id"), QStringLiteral("group_id"),
                                   QStringLiteral("name"), QStringLiteral("tags"), QStringLiteral("scale"), QStringLiteral("section"),
                                   // Local bookkeeping of a stored copy, never part of the shared record.
                                   QStringLiteral("revision")};
    QJsonObject result;
    for (auto it = record.constBegin(); it != record.constEnd(); ++it) {
        if (!known.contains(it.key())) result.insert(it.key(), it.value());
    }
    return result;
}

QString scaleCapability() { return QStringLiteral("review_tag_scales_v1"); }

bool recordHasScale(const QJsonObject &record)
{
    const QJsonArray groups = record.value(QStringLiteral("groups")).toArray();
    return std::any_of(groups.begin(), groups.end(), [](const QJsonValue &group) { return group.toObject().contains(QStringLiteral("scale")); });
}

QString sectionCapability() { return QStringLiteral("review_tag_sections_v1"); }

bool recordHasSection(const QJsonObject &record)
{
    const QJsonArray groups = record.value(QStringLiteral("groups")).toArray();
    return std::any_of(groups.begin(), groups.end(), [](const QJsonValue &group) { return group.toObject().contains(QStringLiteral("section")); });
}

int sectionMaxLength() { return maximumSectionLength; }

QString beforePhase() { return QStringLiteral("before"); }
QString beforeAndAfterPhase() { return QStringLiteral("before_and_after"); }
QString phaseCapability() { return QStringLiteral("review_tag_phases_v1"); }

bool recordHasPhase(const QJsonObject &record)
{
    const QJsonArray groups = record.value(QStringLiteral("groups")).toArray();
    return std::any_of(groups.begin(), groups.end(), [](const QJsonValue &group) { return group.toObject().contains(QStringLiteral("phase")); });
}

bool askedBefore(const TagGroup &group)
{
    return group.phase == beforePhase() || group.phase == beforeAndAfterPhase();
}

bool askedAfter(const TagGroup &group)
{
    return group.phase != beforePhase();
}

const TagGroup *recallGroup(const QList<TagGroup> &groups)
{
    for (const TagGroup &group : groups) {
        if (group.phase == beforeAndAfterPhase() && !group.scale.isEmpty()) return &group;
    }
    return nullptr;
}

QStringList sectionNames(const QList<TagGroup> &groups)
{
    QStringList names;
    for (const TagGroup &group : groups) {
        if (!group.section.isEmpty() && !names.contains(group.section)) names.append(group.section);
    }
    return names;
}


QString groupOf(const QList<TagGroup> &groups, const QString &tag)
{
    for (const TagGroup &group : groups) {
        if (group.tags.contains(tag)) return group.id;
    }
    return {};
}

QList<TagGroup> sections(const QList<TagGroup> &groups, const QStringList &knownTags)
{
    QList<TagGroup> result;
    QSet<QString> grouped;
    for (const TagGroup &group : groups) {
        TagGroup section = group;
        // Order inside a group is not meaningful: show its tags the way the plain list is ordered.
        std::stable_sort(section.tags.begin(), section.tags.end(), [&knownTags](const QString &a, const QString &b) {
            const qsizetype ai = knownTags.indexOf(a), bi = knownTags.indexOf(b);
            return (ai < 0 ? std::numeric_limits<qsizetype>::max() : ai) < (bi < 0 ? std::numeric_limits<qsizetype>::max() : bi);
        });
        for (const QString &tag : std::as_const(section.tags)) grouped.insert(tag);
        result.append(section);
    }
    TagGroup other{QString(), otherGroupName(), {}};
    for (const QString &tag : knownTags) {
        if (!grouped.contains(tag) && tag != skip() && !other.tags.contains(tag)) other.tags.append(tag);
    }
    if (!other.tags.isEmpty()) result.append(other);
    return result;
}

namespace
{
bool acceptGroups(QList<TagGroup> &groups, const QList<TagGroup> &edited, QString *error)
{
    const QString problem = groupsError(edited);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return false;
    }
    groups = edited;
    return true;
}

qsizetype groupIndex(const QList<TagGroup> &groups, const QString &groupId)
{
    for (qsizetype index = 0; index < groups.size(); ++index) {
        if (groups.at(index).id == groupId) return index;
    }
    return -1;
}
}

bool addGroup(QList<TagGroup> &groups, const QString &name, QString *error, QString *createdId)
{
    QList<TagGroup> edited = groups;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    edited.append({id, name.trimmed(), {}});
    if (!acceptGroups(groups, edited, error)) return false;
    if (createdId) *createdId = id;
    return true;
}

bool renameGroup(QList<TagGroup> &groups, const QString &groupId, const QString &name, QString *error)
{
    const qsizetype index = groupIndex(groups, groupId);
    if (index < 0) {
        if (error) *error = QStringLiteral("The group no longer exists.");
        return false;
    }
    QList<TagGroup> edited = groups;
    edited[index].name = name.trimmed();
    return acceptGroups(groups, edited, error);
}

bool moveGroup(QList<TagGroup> &groups, const QString &groupId, int offset)
{
    const qsizetype index = groupIndex(groups, groupId);
    const qsizetype target = index + offset;
    if (index < 0 || offset == 0 || target < 0 || target >= groups.size()) return false;
    groups.move(index, target);
    return true;
}

bool deleteGroup(QList<TagGroup> &groups, const QString &groupId)
{
    const qsizetype index = groupIndex(groups, groupId);
    if (index < 0) return false;
    // Its tags simply leave the record, so they show under Other.
    groups.removeAt(index);
    return true;
}

bool setScale(QList<TagGroup> &groups, const QString &groupId, const QStringList &steps, QString *error)
{
    const qsizetype index = groupIndex(groups, groupId);
    if (index < 0) {
        if (error) *error = QStringLiteral("The group no longer exists.");
        return false;
    }
    if (steps.size() == 1) {
        if (error) *error = QStringLiteral("A slider needs at least two steps.");
        return false;
    }
    QList<TagGroup> edited = groups;
    edited[index].scale = steps;
    return acceptGroups(groups, edited, error);
}

bool setSection(QList<TagGroup> &groups, const QString &groupId, const QString &section, QString *error)
{
    const qsizetype index = groupIndex(groups, groupId);
    if (index < 0) {
        if (error) *error = QStringLiteral("The group no longer exists.");
        return false;
    }
    QList<TagGroup> edited = groups;
    edited[index].section = section.trimmed();
    return acceptGroups(groups, edited, error);
}

bool setPhase(QList<TagGroup> &groups, const QString &groupId, const QString &phase, QString *error)
{
    const qsizetype index = groupIndex(groups, groupId);
    if (index < 0) {
        if (error) *error = QStringLiteral("The group no longer exists.");
        return false;
    }
    QList<TagGroup> edited = groups;
    edited[index].phase = phase;
    // A newer client's value gives way to the one chosen here.
    edited[index].extra.remove(QStringLiteral("phase"));
    return acceptGroups(groups, edited, error);
}

bool moveTag(QList<TagGroup> &groups, const QString &tag, const QString &groupId, QString *error)
{
    if (tag.trimmed().isEmpty() || tag == skip()) {
        if (error) *error = QStringLiteral("Only tags can be put in a group.");
        return false;
    }
    const qsizetype target = groupId.isEmpty() ? -1 : groupIndex(groups, groupId);
    if (!groupId.isEmpty() && target < 0) {
        if (error) *error = QStringLiteral("The group no longer exists.");
        return false;
    }
    QList<TagGroup> edited = groups;
    for (qsizetype index = 0; index < edited.size(); ++index) {
        TagGroup &group = edited[index];
        if (index == target && group.tags.contains(tag)) continue;
        group.tags.removeAll(tag);
        // A step that leaves its group stops being a step.
        group.scale.removeAll(tag);
    }
    if (target >= 0 && !edited[target].tags.contains(tag)) edited[target].tags.append(tag);
    return acceptGroups(groups, edited, error);
}

QString groupNotesField() { return QStringLiteral("review_group_notes"); }
QString otherGroupNoteKey() { return QStringLiteral("other"); }
QString answerNoteKey() { return QStringLiteral("note"); }
QString beforeNoteKey() { return QStringLiteral("before_note"); }
QString answerNoteName() { return QStringLiteral("Note"); }
QString beforeNoteName() { return QStringLiteral("Before note"); }
int groupNoteMaxLength() { return 500; }

QString cutGroupNote(const QString &text)
{
    qsizetype end = 0;
    for (int characters = 0; end < text.size() && characters < groupNoteMaxLength(); ++characters)
        end += text.at(end).isHighSurrogate() && end + 1 < text.size() && text.at(end + 1).isLowSurrogate() ? 2 : 1;
    return text.left(end);
}

QString cleanGroupNote(const QString &text)
{
    return cutGroupNote(text.trimmed()).trimmed();
}

QHash<QString, QString> cleanGroupNotes(const QHash<QString, QString> &notes)
{
    QHash<QString, QString> result;
    for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
        const QString note = cleanGroupNote(it.value());
        if (!it.key().isEmpty() && !note.isEmpty()) result.insert(it.key(), note);
    }
    return result;
}

QHash<QString, QString> groupNotesOfEvent(const QJsonObject &event)
{
    QHash<QString, QString> notes;
    const QJsonObject recorded = event.value(groupNotesField()).toObject();
    for (auto it = recorded.constBegin(); it != recorded.constEnd(); ++it) {
        const QString note = it.value().toString().trimmed();
        if (!it.key().isEmpty() && !note.isEmpty()) notes.insert(it.key(), note);
    }
    return notes;
}

QStringList groupNoteLines(const QHash<QString, QString> &notes, const QList<TagGroup> &groups, const QString &otherLabel, const QString &noteLabel,
                           const QString &beforeNoteLabel)
{
    QStringList lines;
    // The answer's own notes are never listed among the groups, whatever their IDs.
    QSet<QString> shown{answerNoteKey(), beforeNoteKey()};
    const auto add = [&](const QString &key, const QString &label) {
        if (shown.contains(key) || !notes.contains(key)) return;
        shown.insert(key);
        lines.append(label + QStringLiteral(": ") + notes.value(key));
    };
    for (const TagGroup &group : groups) add(group.id, group.name);
    add(otherGroupNoteKey(), otherLabel);
    QStringList unknown = notes.keys();
    unknown.sort();
    for (const QString &key : std::as_const(unknown)) add(key, key);
    if (const QString note = notes.value(beforeNoteKey()); !note.isEmpty()) lines.append(beforeNoteLabel + QStringLiteral(": ") + note);
    if (const QString note = notes.value(answerNoteKey()); !note.isEmpty()) lines.append(noteLabel + QStringLiteral(": ") + note);
    return lines;
}
}

namespace ReviewEntry
{
QString field() { return QStringLiteral("review_entry"); }
QString dueQueue() { return QStringLiteral("due_queue"); }
QString reviewNow() { return QStringLiteral("review_now"); }
}

namespace ReviewPhases
{
QString field() { return QStringLiteral("review_phases"); }

Recorded ofEvent(const QJsonObject &event)
{
    Recorded recorded;
    const QJsonValue value = event.value(field());
    if (!value.isObject()) return recorded;
    const QJsonObject phases = value.toObject();
    recorded.present = true;
    const auto tagsOf = [](const QJsonValue &step) {
        QStringList tags;
        for (const auto &tag : step.toObject().value(QStringLiteral("tags")).toArray()) {
            if (tag.isString() && !tag.toString().trimmed().isEmpty()) tags.append(tag.toString());
        }
        return tags;
    };
    recorded.beforeTags = tagsOf(phases.value(QStringLiteral("before")));
    recorded.afterTags = tagsOf(phases.value(QStringLiteral("after")));
    recorded.answerShownAt = QDateTime::fromString(phases.value(QStringLiteral("before")).toObject().value(QStringLiteral("answer_shown_at")).toString(),
                                                   Qt::ISODateWithMs);
    // Only a prediction that names its tag is one.
    const QJsonObject prediction = phases.value(QStringLiteral("agent_prediction")).toObject();
    if (!prediction.value(QStringLiteral("tag")).toString().trimmed().isEmpty()) recorded.agentPrediction = prediction;
    return recorded;
}

QJsonObject toField(const QStringList &beforeTags, const QStringList &afterTags, const QDateTime &answerShownAt, const QJsonObject &agentPrediction)
{
    QJsonObject before{{QStringLiteral("tags"), QJsonArray::fromStringList(beforeTags)}};
    if (answerShownAt.isValid()) before.insert(QStringLiteral("answer_shown_at"), answerShownAt.toUTC().toString(Qt::ISODateWithMs));
    QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("before"), before},
                       {QStringLiteral("after"), QJsonObject{{QStringLiteral("tags"), QJsonArray::fromStringList(afterTags)}}}};
    if (!agentPrediction.isEmpty()) result.insert(QStringLiteral("agent_prediction"), agentPrediction);
    return result;
}
}

namespace ReviewContextNotes
{
QString field() { return QStringLiteral("review_context_notes"); }
QString sectionScope() { return QStringLiteral("section"); }
QString pageScope() { return QStringLiteral("page"); }
QString documentScope() { return QStringLiteral("document"); }
QStringList scopes() { return {sectionScope(), pageScope(), documentScope()}; }
QString sectionSeparator() { return QStringLiteral(" › "); }

QList<OutlineItem> outlineFromVariant(const QVariantList &items)
{
    QList<OutlineItem> outline;
    outline.reserve(items.size());
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        outline.append({item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("page")).toInt(),
                        item.value(QStringLiteral("top")).toDouble(), outlineFromVariant(item.value(QStringLiteral("children")).toList())});
    }
    return outline;
}

namespace
{
struct Position {
    int page = 0;
    double top = 0;
};
bool earlier(const Position &a, const Position &b) { return a.page < b.page || (a.page == b.page && a.top < b.top); }

// Where an entry starts: its own destination, else the earliest of its entries' (an entry without
// a destination, such as a part heading, starts where its first section does).
std::optional<Position> startOf(const OutlineItem &item, int depth = 0)
{
    if (item.page > 0) return Position{item.page, std::isfinite(item.top) ? item.top : 0.0};
    if (depth > 64) return std::nullopt;
    std::optional<Position> first;
    for (const OutlineItem &child : item.children) {
        const auto start = startOf(child, depth + 1);
        if (start && (!first || earlier(*start, *first))) first = start;
    }
    return first;
}
}

bool sectionAt(const QList<OutlineItem> &outline, int page, double top, Note *section)
{
    const Position position{page, top};
    QStringList path;
    const QList<OutlineItem> *level = &outline;
    // At each level the entry that starts last at or before the position; then its own entries.
    for (int depth = 0; depth <= 64 && page > 0; ++depth) {
        const OutlineItem *chosen = nullptr;
        Position chosenAt;
        for (const OutlineItem &item : *level) {
            const auto start = startOf(item);
            if (!start || earlier(position, *start)) continue;
            // Equal starts: the later entry, as it is listed after the other.
            if (!chosen || !earlier(*start, chosenAt)) {
                chosen = &item;
                chosenAt = *start;
            }
        }
        if (!chosen) break;
        path.append(chosen->title.simplified());
        level = &chosen->children;
    }
    if (path.isEmpty()) return false;
    if (section) *section = {path.join(sectionSeparator()), path.constLast(), {}};
    return true;
}

QString documentKey(const QJsonObject &cardRecord)
{
    const QJsonObject primary = primaryObject(cardRecord);
    const QString documentId = primary.value(QStringLiteral("document_id")).toString();
    return documentId.isEmpty() ? primary.value(QStringLiteral("pdf_stem")).toString() : documentId;
}

bool primaryPosition(const ReviewCard &card, int *page, double *top)
{
    int firstPage = 0;
    double highest = 0;
    const auto topOf = [](const QVariantList &rect) {
        return rect.size() == 4 ? std::min(rect.at(1).toDouble(), rect.at(3).toDouble()) : 0.0;
    };
    for (const QVariant &value : card.regions()) {
        const QVariantMap region = value.toMap();
        const int regionPage = region.value(QStringLiteral("page")).toInt();
        const double regionTop = topOf(region.value(QStringLiteral("rect")).toList());
        if (regionPage < 1) continue;
        if (firstPage == 0 || regionPage < firstPage || (regionPage == firstPage && regionTop < highest)) {
            firstPage = regionPage;
            highest = regionTop;
        }
    }
    if (firstPage == 0) {
        firstPage = card.page();
        highest = topOf(card.rect());
    }
    if (firstPage < 1) return false;
    if (page) *page = firstPage;
    if (top) *top = highest;
    return true;
}

Note pageNote(int page) { return {QString::number(page), QStringLiteral("Page %1").arg(page), {}}; }

QHash<QString, Note> ofEvent(const QJsonObject &event)
{
    QHash<QString, Note> notes;
    const QJsonObject recorded = event.value(field()).toObject();
    for (const QString &scope : scopes()) {
        const QJsonObject note = recorded.value(scope).toObject();
        if (!note.value(QStringLiteral("key")).isString() || !note.value(QStringLiteral("text")).isString()) continue;
        notes.insert(scope, {note.value(QStringLiteral("key")).toString(), note.value(QStringLiteral("label")).toString(),
                             note.value(QStringLiteral("text")).toString().trimmed()});
    }
    return notes;
}

QJsonObject toField(const QHash<QString, Note> &changes)
{
    QJsonObject recorded;
    for (const QString &scope : scopes()) {
        const auto note = changes.constFind(scope);
        if (note == changes.cend()) continue;
        recorded.insert(scope, QJsonObject{{QStringLiteral("key"), note->key}, {QStringLiteral("label"), note->label},
                                           {QStringLiteral("text"), ReviewTags::cleanGroupNote(note->text)}});
    }
    return recorded;
}

QString noteId(const QString &scope, const QString &key) { return scope + QLatin1Char('\n') + key; }

QHash<QString, QString> current(const QList<QJsonObject> &cardRecords, const QList<QJsonObject> &events, const QString &documentKey)
{
    QSet<QString> cards;
    for (const QJsonObject &record : cardRecords) {
        if (ReviewContextNotes::documentKey(record) == documentKey) cards.insert(objectString(record, QStringLiteral("card_id")));
    }
    // Undone answers no longer count; an undo names the event it retracts on the same card.
    QSet<QString> retracted;
    for (const QJsonObject &event : events) {
        const QString target = objectString(event, QStringLiteral("retracted_event_id"));
        if (!target.isEmpty()) retracted.insert(objectString(event, QStringLiteral("card_id")) + QLatin1Char('\n') + target);
    }
    struct Newest {
        QDateTime at;
        QString eventId;
        QString text;
    };
    QHash<QString, Newest> newest;
    for (const QJsonObject &event : events) {
        const QString cardId = objectString(event, QStringLiteral("card_id"));
        if (!event.contains(field()) || !cards.contains(cardId) || event.value(QStringLiteral("deleted")).toBool()) continue;
        const QString eventId = objectString(event, QStringLiteral("event_id"));
        const QDateTime at = parseUtc(objectString(event, QStringLiteral("reviewed_at")));
        if (eventId.isEmpty() || !at.isValid() || retracted.contains(cardId + QLatin1Char('\n') + eventId)) continue;
        const auto notes = ofEvent(event);
        for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
            const QString id = noteId(it.key(), it->key);
            const auto found = newest.constFind(id);
            if (found == newest.cend() || at > found->at || (at == found->at && eventId > found->eventId)) newest.insert(id, {at, eventId, it->text});
        }
    }
    QHash<QString, QString> result;
    for (auto it = newest.cbegin(); it != newest.cend(); ++it) {
        if (!it->text.isEmpty()) result.insert(it.key(), it->text);
    }
    return result;
}

QHash<QString, Note> Draft::changes() const
{
    QHash<QString, Note> result;
    for (const Entry &entry : entries) {
        if (!entry.available) continue;
        const QString text = ReviewTags::cleanGroupNote(entry.text);
        if (text != entry.note.text) result.insert(entry.scope, {entry.note.key, entry.note.label, text});
    }
    return result;
}
}

namespace
{
// Local copy of this computer's latest tag groups edit (below reviews/).
QString groupsFolder() { return QStringLiteral("review-tag-groups"); }
}

namespace
{
// Answers still in effect, oldest first: ratings not undone and skips not cancelled.
QList<QJsonObject> effectiveAnswers(const ReviewCard &card)
{
    QHash<QString, QJsonObject> unique;
    for (const auto &event : card.history) {
        if (event.value(QStringLiteral("card_id")).toString() == card.id())
            unique.insert(event.value(QStringLiteral("event_id")).toString(), event);
    }
    QSet<QString> retracted, undone;
    for (const auto &event : unique) {
        const auto target = event.value(QStringLiteral("retracted_event_id")).toString();
        if (unique.contains(target) && reviewRating(unique.value(target).value(QStringLiteral("rating")).toString())
            && unique.value(target).value(QStringLiteral("rating")).toString() != QLatin1String("delete")) retracted.insert(target);
        if (event.value(QStringLiteral("queue_action")).toString() == QLatin1String("undo_skip"))
            undone.insert(event.value(QStringLiteral("skip_id")).toString());
    }
    auto ordered = unique.values();
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        const auto at = parseUtc(a.value(QStringLiteral("reviewed_at")).toString());
        const auto bt = parseUtc(b.value(QStringLiteral("reviewed_at")).toString());
        return at == bt ? a.value(QStringLiteral("event_id")).toString() < b.value(QStringLiteral("event_id")).toString() : at < bt;
    });
    QList<QJsonObject> answers;
    for (const auto &event : ordered) {
        if (retracted.contains(event.value(QStringLiteral("event_id")).toString())) continue;
        const auto at = parseUtc(event.value(QStringLiteral("reviewed_at")).toString());
        if (!at.isValid()) continue;
        const auto queue = event.value(QStringLiteral("queue_action")).toString();
        if (!queue.isEmpty()) {
            if (queue == QLatin1String("skip") && !undone.contains(event.value(QStringLiteral("skip_id")).toString())
                && parseUtc(event.value(QStringLiteral("skip_until")).toString()) > at) answers.append(event);
            continue;
        }
        const auto rating = event.value(QStringLiteral("rating")).toString();
        if (!reviewRating(rating)) continue;
        if (rating == QLatin1String("delete")) break;
        if (rating == QLatin1String("custom_interval")) {
            const auto due = parseUtc(event.value(QStringLiteral("scheduled_due_at")).toString());
            if (!due.isValid() || due <= at) continue;
        }
        answers.append(event);
    }
    return answers;
}
}

namespace
{
// Whether an event still in effect is an answer: not a skip (nor one an older client recorded as
// a "skip" answer or tag) and not an interval change.
bool isAnswer(const QJsonObject &event)
{
    if (!event.value(QStringLiteral("queue_action")).toString().isEmpty()
        || !event.value(QStringLiteral("interval_action")).toString().isEmpty()
        || event.value(QStringLiteral("review_action")).toString().compare(ReviewTags::skip(), Qt::CaseInsensitive) == 0)
        return false;
    const QJsonArray recorded = event.value(QStringLiteral("review_tags")).toArray();
    const bool skipOnly = !recorded.isEmpty() && std::all_of(recorded.begin(), recorded.end(), [](const QJsonValue &value) {
        return ReviewTags::normalize(value.toString()).compare(ReviewTags::skip(), Qt::CaseInsensitive) == 0;
    });
    return !skipOnly;
}

// The latest answer still in effect, as the phone reads it: not undone, not an undo, not a skip
// (nor one an older client recorded as a "skip" answer or tag), not a deletion, not an interval
// change. Empty when the card was never answered.
std::optional<QJsonObject> lastAnswer(const ReviewCard &card)
{
    const auto answers = effectiveAnswers(card);
    for (auto answer = answers.crbegin(); answer != answers.crend(); ++answer) {
        if (isAnswer(*answer)) return *answer;
    }
    return std::nullopt;
}
}

QList<QJsonObject> ReviewCard::answers() const
{
    QList<QJsonObject> result;
    for (const auto &event : effectiveAnswers(*this)) {
        if (isAnswer(event)) result.append(event);
    }
    return result;
}

QStringList ReviewCard::currentTags() const
{
    const auto answer = lastAnswer(*this);
    return answer ? tagsOfEvent(*answer) : QStringList{};
}

bool ReviewCard::wasAnswered() const
{
    return lastAnswer(*this).has_value();
}

QStringList ReviewCard::tagsOfEvent(const QJsonObject &event) const
{
    return ReviewTags::applyMerges(ReviewTags::ofEvent(event), tagMerges);
}

QString ReviewCard::currentMark() const
{
    const QStringList tags = currentTags();
    if (tags.isEmpty()) return {};
    const QString legacy = ReviewTags::legacyReviewAction(tags);
    return legacy.isEmpty() ? tags.constFirst() : legacy;
}

bool ReviewBrowserCard::matchesFilter(const QString &filter) const
{
    const auto tags = card.currentTags();
    // Retired cards, and cards of an unpublished document, have a filter of their own; the others
    // list cards still reviewed.
    const bool outOfReview = card.schedule.retired || state == ReviewBrowserState::Unpublished;
    if (filter == QLatin1String("retired")) return outOfReview;
    if (outOfReview && (filter == QLatin1String("new") || filter == QLatin1String("seen") || filter == QLatin1String("never")
                                  || filter == QLatin1String("unavailable") || filter == QLatin1String("issues"))) return false;
    if (filter == QLatin1String("new")) return !card.wasAnswered();
    if (filter == QLatin1String("seen")) return card.wasAnswered();
    if (filter == QLatin1String("never")) return card.schedule.retained;
    if (filter.startsWith(QLatin1String("tag:"))) return tags.contains(ReviewTags::resolve(filter.mid(4), card.tagMerges));
    if (filter == QLatin1String("unavailable") || filter == QLatin1String("issues")) return hasIssues;
    return true;
}

bool ReviewBrowserCard::canReview() const
{
    return !hasIssues && card.isApproved() && !card.schedule.deleted
        && (state == ReviewBrowserState::New || state == ReviewBrowserState::Learning || state == ReviewBrowserState::Review
            || state == ReviewBrowserState::Future || state == ReviewBrowserState::Skipped || state == ReviewBrowserState::Retained);
}

QString ReviewCard::id() const
{
    return objectString(record, QStringLiteral("card_id"));
}

QString ReviewCard::status() const
{
    return objectString(record, QStringLiteral("status"));
}

QString ReviewCard::pdfStem() const
{
    return objectString(primaryObject(record), QStringLiteral("pdf_stem"));
}

QString ReviewCard::pdfSha256() const
{
    return objectString(primaryObject(record), QStringLiteral("pdf_sha256"));
}

QString ReviewCard::pageVisualSha256() const
{
    return objectString(primaryObject(record), QStringLiteral("page_visual_sha256"));
}

QString ReviewCard::question() const
{
    return objectString(primaryObject(record), QStringLiteral("question")).trimmed();
}

int ReviewCard::page() const
{
    const auto primary = primaryObject(record);
    const auto custom = primary.value(QStringLiteral("answer_region_override")).toObject().value(QStringLiteral("regions")).toArray();
    const auto regions = effectiveReviewCardRegions(primary, record.value(QStringLiteral("schema_version")).toInt());
    if (!custom.isEmpty() && custom == regions) return regions.first().toObject().value(QStringLiteral("page")).toInt();
    return primary.value(QStringLiteral("page")).toInt();
}

QVariantList ReviewCard::rect() const
{
    const auto primary = primaryObject(record);
    const auto custom = primary.value(QStringLiteral("answer_region_override")).toObject().value(QStringLiteral("regions")).toArray();
    const auto regions = effectiveReviewCardRegions(primary, record.value(QStringLiteral("schema_version")).toInt());
    if (!custom.isEmpty() && custom == regions) return regions.first().toObject().value(QStringLiteral("rect")).toArray().toVariantList();
    const auto rect = primary.value(QStringLiteral("rect")).toArray();
    return validRect(rect) ? rect.toVariantList() : QVariantList();
}

QVariantList ReviewCard::regions() const
{
    const auto primary = primaryObject(record);
    // Preserve the legacy accessor fallback; the sidebar separately validates schema-4 placement.
    const auto regions = effectiveReviewCardRegions(primary, primary.contains(QStringLiteral("regions")) ? record.value(QStringLiteral("schema_version")).toInt() : 3);
    for (const auto &value : regions) {
        const auto region = value.toObject();
        if (region.value(QStringLiteral("page")).toInt() < 1 || !validNormalizedReviewCoordinates(region.value(QStringLiteral("rect")).toArray(), 4)) return {};
    }
    return regions.toVariantList();
}

QString ReviewCard::pagesLabel() const
{
    QList<int> pages;
    for (const QVariant &value : regions()) {
        const int page = value.toMap().value(QStringLiteral("page")).toInt();
        if (!pages.contains(page)) pages.append(page);
    }
    std::sort(pages.begin(), pages.end());
    QStringList labels;
    for (int page : pages) labels.append(QString::number(page));
    return labels.isEmpty() ? QString::number(this->page()) : labels.join(QStringLiteral(", "));
}

bool ReviewCard::isApproved() const
{
    return status() == QLatin1String("approved");
}

ReviewStore::ReviewStore(QString docsRoot, ProjectionProvider projectionProvider, QString syncStateRoot, bool readOnly)
    : m_readOnly(readOnly)
    , m_docsRoot(std::move(docsRoot))
    , m_projectionProvider(std::move(projectionProvider))
{
    if (m_readOnly) return;
    if (syncStateRoot.isEmpty()
        && QFileInfo(m_docsRoot).canonicalFilePath() == QFileInfo(Thoughts::thoughtsHome()).canonicalFilePath()) {
        syncStateRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server"));
    }
    if (!syncStateRoot.isEmpty()) {
        const QDir stateDirectory(syncStateRoot);
        m_outbox = std::make_unique<DesktopSyncOutbox>(stateDirectory.filePath(QStringLiteral("desktop-sync.sqlite")),
                                                       stateDirectory.filePath(QStringLiteral("outbox.json")));
    }
    migrateLegacyStore();
    recoverSourceTransactions();
}

ReviewStore::~ReviewStore() = default;

void ReviewStore::recoverSourceTransactions()
{
    const QDir directory(QDir(rootPath()).filePath(QStringLiteral("source-transactions")));
    if (!directory.exists()) return;
    QLockFile lock(directory.filePath(QStringLiteral("writer.lock")));
    if (!lock.tryLock(0)) return;
    for (const QString &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile journalFile(directory.filePath(name));
        if (!journalFile.open(QIODevice::ReadOnly)) continue;
        const auto journal = QJsonDocument::fromJson(journalFile.readAll()).object();
        journalFile.close();
        if (name.startsWith(QLatin1String("merge-"))) {
            // A card merge (mergeCards): its source, then its deletions and the kept card in one batch.
            if (journal.value(QStringLiteral("kind")).toString() != QLatin1String("review_card_merge") || QUuid(name.mid(6).chopped(5)).isNull()) continue;
            const QString path = journal.value(QStringLiteral("path")).toString();
            const QString relative = QDir(QFileInfo(m_docsRoot).canonicalFilePath()).relativeFilePath(QFileInfo(path).canonicalFilePath());
            if (SourceOwner::of(relative).isEmpty() || relative.contains(QLatin1String("/../"))
                || !QStringList{QStringLiteral("tex"), QStringLiteral("md"), QStringLiteral("markdown")}.contains(QFileInfo(relative).suffix())) continue;
            const auto before = QByteArray::fromBase64(journal.value(QStringLiteral("before")).toString().toLatin1());
            const auto after = QByteArray::fromBase64(journal.value(QStringLiteral("after")).toString().toLatin1());
            QFile file(path);
            if (before.isEmpty() || !file.open(QIODevice::ReadOnly)) continue;
            const auto current = file.readAll(); file.close();
            if (current == before) {
                if (!Thoughts::Platform::replaceFileContents(path, after)) continue;
            } else if (current != after) {
                qWarning() << "Review card merge recovery requires attention:" << directory.filePath(name);
                continue;
            }
            QString error;
            if (writeServerRecords(journal.value(QStringLiteral("records")).toArray(), &error)) QFile::remove(directory.filePath(name));
            else qWarning() << "Review card merge recovery failed:" << error;
            continue;
        }
        const auto card = journal.value(journal.contains(QStringLiteral("record")) ? QStringLiteral("record") : QStringLiteral("card")).toObject();
        const QString id = card.value(card.contains(QStringLiteral("event_id")) ? QStringLiteral("event_id") : QStringLiteral("card_id")).toString();
        if (QUuid(id).isNull() || name != id + QStringLiteral(".json")) continue;
        const QString recordPath = journal.value(QStringLiteral("relative_path")).toString(QStringLiteral("cards/%1.json").arg(id));
        if (recordPath != QStringLiteral("cards/%1.json").arg(id) && recordPath != QStringLiteral("events/mac-okular/%1.json").arg(id)) continue;
        const QString path = journal.value(QStringLiteral("path")).toString();
        const QString relative = QDir(QFileInfo(m_docsRoot).canonicalFilePath()).relativeFilePath(QFileInfo(path).canonicalFilePath());
        if (SourceOwner::of(relative).isEmpty() || relative.contains(QLatin1String("/../")) || !QStringList{QStringLiteral("tex"), QStringLiteral("md"), QStringLiteral("markdown")}.contains(QFileInfo(relative).suffix())) continue;
        const auto before = QByteArray::fromBase64(journal.value(QStringLiteral("before")).toString().toLatin1());
        const auto after = QByteArray::fromBase64(journal.value(QStringLiteral("after")).toString().toLatin1());
        const auto change = card.value(QStringLiteral("source_change")).toObject();
        if (!change.isEmpty() && (change.value(QStringLiteral("path")).toString() != relative
            || change.value(QStringLiteral("before_sha256")).toString() != QString::fromLatin1(QCryptographicHash::hash(before, QCryptographicHash::Sha256).toHex())
            || change.value(QStringLiteral("after_sha256")).toString() != QString::fromLatin1(QCryptographicHash::hash(after, QCryptographicHash::Sha256).toHex()))) continue;
        QFile file(path);
        if (before.isEmpty() || !file.open(QIODevice::ReadOnly)) continue;
        const auto current = file.readAll(); file.close();
        if (current == before) {
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(after) != after.size() || !output.commit()) continue;
        } else if (current != after) {
            qWarning() << "Review source recovery requires attention:" << directory.filePath(name);
            continue;
        }
        QString error;
        if (writeServerJson(recordPath, card, &error)) QFile::remove(directory.filePath(name));
    }
}

void ReviewStore::migrateLegacyStore()
{
    const QString root = rootPath();
    const QDir cardsDirectory(QDir(root).filePath(QStringLiteral("cards")));
    const QStringList cardFiles = cardsDirectory.entryList({QStringLiteral("*.json")}, QDir::Files);
    bool hasLegacyCards = false;
    for (const QString &fileName : cardFiles) {
        const QJsonObject card = readObject(cardsDirectory.filePath(fileName), nullptr);
        if (!card.isEmpty() && card.value(QStringLiteral("schema_version")).toInt() != schemaVersion
            && card.value(QStringLiteral("schema_version")).toInt() != 4) {
            hasLegacyCards = true;
            break;
        }
    }
    if (!hasLegacyCards) {
        return;
    }

    const QString backupRoot = QDir(m_docsRoot).filePath(QStringLiteral(".runtime/review-backups"));
    QDir().mkpath(backupRoot);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString backup = QDir(backupRoot).filePath(QStringLiteral("schema-legacy-%1").arg(stamp));
    QDir docsDirectory(m_docsRoot);
    if (!docsDirectory.rename(QStringLiteral("reviews"), QDir(m_docsRoot).relativeFilePath(backup))) {
        qWarning() << "Could not archive the legacy PDF review store at" << root;
        return;
    }
    QDir().mkpath(QDir(root).filePath(QStringLiteral("cards")));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("events")));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("exclusions")));
}

QString ReviewStore::rootPath() const
{
    return QDir(m_docsRoot).filePath(QStringLiteral("reviews"));
}

QList<QJsonObject> ReviewStore::records(const QString &entityType, const QString &collection, const QString &idField, QStringList *errors, const QString &pdfStem,
                                       const QJsonObject *projectionSnapshot) const
{
    if (!m_readOnly) flushQueueActions();
    QList<QJsonObject> source;
    if (m_projectionProvider) {
        const QJsonObject projections = projectionSnapshot ? *projectionSnapshot : m_projectionProvider();
        const QString prefix = entityType + QLatin1Char('\n') + collection + QLatin1Char('\n');
        for (auto iterator = projections.constBegin(); iterator != projections.constEnd(); ++iterator) {
            if (!iterator.key().startsWith(prefix) || !iterator.value().isObject()) {
                continue;
            }
            const QJsonObject record = iterator.value().toObject();
            if (!pdfStem.isEmpty() && entityType == QLatin1String("review_card")
                && objectString(primaryObject(record), QStringLiteral("pdf_stem")) != pdfStem) continue;
            const QString entityId = objectString(record, idField);
            if (entityId.isEmpty() || iterator.key().mid(prefix.size()) != entityId) {
                if (errors) {
                    errors->append(QStringLiteral("invalid %1 projection %2").arg(entityType, iterator.key()));
                }
                continue;
            }
            source.append(record);
        }
        if (m_outbox && (entityType == QLatin1String("review_card") || entityType == QLatin1String("review_event")
                         || entityType == ReviewTags::mergeEntityType() || entityType == ReviewTags::groupsEntityType()
                         || entityType == ReviewCardFeedback::entityType() || entityType == DocumentFeedback::entityType())) {
            QString outboxError;
            const auto operations = m_outbox->operations(&outboxError);
            if (errors && !outboxError.isEmpty()) errors->append(outboxError);
            for (const auto &operation : operations) {
                if (operation.value(QStringLiteral("entity_type")).toString() != entityType
                    || operation.value(QStringLiteral("collection")).toString() != collection) continue;
                const auto record = operation.value(QStringLiteral("payload")).toObject();
                if (entityType != QLatin1String("review_card") || pdfStem.isEmpty()
                    || objectString(primaryObject(record), QStringLiteral("pdf_stem")) == pdfStem)
                    source.append(record);
            }
        } else if (!m_outbox && entityType == QLatin1String("review_event")) {
            source.append(readObjectsBelow(QDir(rootPath()).filePath(collection), errors));
        }
    } else {
        // Tag groups share the review-tags collection with merges but not their local folder.
        source = readObjectsBelow(QDir(rootPath()).filePath(entityType == ReviewTags::groupsEntityType() ? groupsFolder() : collection), errors);
        if (!pdfStem.isEmpty() && entityType == QLatin1String("review_card")) {
            source.removeIf([&](const QJsonObject &record) {
                return objectString(primaryObject(record), QStringLiteral("pdf_stem")) != pdfStem;
            });
        }
    }

    if (entityType == QLatin1String("review_event")) {
        for (const auto &manifest : readObjectsBelow(QDir(rootPath()).filePath(QStringLiteral("queue-actions")), errors)) {
            for (const auto &action : manifest.value(QStringLiteral("actions")).toArray()) source.append(action.toObject());
        }
    }
    QHash<QString, QJsonObject> unique;
    for (const QJsonObject &record : std::as_const(source)) {
        const QString id = objectString(record, idField);
        if (id.isEmpty()) {
            if (errors) {
                errors->append(QStringLiteral("%1 record has no %2").arg(entityType, idField));
            }
            continue;
        }
        const QJsonObject current = unique.value(id);
        if (entityType == ReviewCardFeedback::entityType()) {
            // Projections first, then queued writes in order: the server's own merge.
            unique.insert(id, ReviewCardFeedback::merge(current, record));
            continue;
        }
        if (entityType == DocumentFeedback::entityType()) {
            unique.insert(id, DocumentFeedback::merge(current, record));
            continue;
        }
        const qint64 revision = record.value(QStringLiteral("revision")).toInteger();
        const qint64 currentRevision = current.value(QStringLiteral("revision")).toInteger();
        const QString updatedAt = objectString(record, QStringLiteral("updated_at"));
        const QString currentUpdatedAt = objectString(current, QStringLiteral("updated_at"));
        if (current.isEmpty() || revision > currentRevision || (revision == currentRevision && updatedAt > currentUpdatedAt)) {
            unique.insert(id, record);
        }
    }
    return unique.values();
}

QList<QJsonObject> ReviewStore::events(QStringList *errors) const
{
    return records(QStringLiteral("review_event"), QStringLiteral("events"), QStringLiteral("event_id"), errors);
}

bool ReviewStore::refreshQueryDatabase(const QString &databasePath, QString *error) const
{
    QStringList readErrors;
    auto cardRecords = records(QStringLiteral("review_card"), QStringLiteral("cards"), QStringLiteral("card_id"), &readErrors);
    auto eventRecords = events(&readErrors);
    if (!readErrors.isEmpty()) {
        if (error) *error = readErrors.join(QLatin1Char('\n'));
        return false;
    }
    // Invalid legacy cards are excluded from the UI; they must not freeze the editor index.
    cardRecords.removeIf([](const QJsonObject &record) { return !validReviewCardRecord(record); });
    const auto byId = [](const QJsonObject &a, const QJsonObject &b, const QString &field) {
        return a.value(field).toString() < b.value(field).toString();
    };
    std::sort(cardRecords.begin(), cardRecords.end(), [&](const auto &a, const auto &b) { return byId(a, b, QStringLiteral("card_id")); });
    std::sort(eventRecords.begin(), eventRecords.end(), [&](const auto &a, const auto &b) { return byId(a, b, QStringLiteral("event_id")); });
    QJsonArray source;
    for (const auto &card : std::as_const(cardRecords)) source.append(card);
    for (const auto &event : std::as_const(eventRecords)) source.append(event);
    // Tag merges change the marks shown for unchanged answers.
    auto mergeRecords = records(ReviewTags::mergeEntityType(), ReviewTags::mergeCollection(), QStringLiteral("merge_id"));
    std::sort(mergeRecords.begin(), mergeRecords.end(), [&](const auto &a, const auto &b) { return byId(a, b, QStringLiteral("merge_id")); });
    for (const auto &merge : std::as_const(mergeRecords)) source.append(merge);
    // The tag groups let readers such as `thoughtsctl knowledge-profile` name each tag's group.
    const auto groupRecords = records(ReviewTags::groupsEntityType(), ReviewTags::mergeCollection(), QStringLiteral("layout_id"));
    for (const auto &groups : groupRecords) source.append(groups);
    // Answer feedback, tombstones included (agents may ask for deleted feedback).
    auto feedbackRecords = records(ReviewCardFeedback::entityType(), ReviewCardFeedback::collection(), QStringLiteral("feedback_id"));
    std::sort(feedbackRecords.begin(), feedbackRecords.end(), [&](const auto &a, const auto &b) { return byId(a, b, QStringLiteral("feedback_id")); });
    for (const auto &feedback : std::as_const(feedbackRecords)) source.append(feedback);
    // Document feedback (document-feedback.md), tombstones included.
    auto documentFeedbackRecords = records(DocumentFeedback::entityType(), DocumentFeedback::collection(), QStringLiteral("feedback_id"));
    std::sort(documentFeedbackRecords.begin(), documentFeedbackRecords.end(), [&](const auto &a, const auto &b) { return byId(a, b, QStringLiteral("feedback_id")); });
    for (const auto &feedback : std::as_const(documentFeedbackRecords)) source.append(feedback);
    // Include the schedule folding version so a corrected rule rebuilds old query indexes
    // (3: adds review_tag_records; 4: adds review_feedback; 5: adds review_card_retirements;
    // 6: adds document_feedback).
    const QString sourceHash = QStringLiteral("6:") + sha256(QJsonDocument(source).toJson(QJsonDocument::Compact));
    if (!QDir().mkpath(QFileInfo(databasePath).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the review query database directory.");
        return false;
    }

    const QString connectionName = QStringLiteral("thoughts-review-query-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (!database.open()) {
            if (error) *error = database.lastError().text();
        } else {
            const auto publish = [&]() -> bool {
                const auto execute = [&](const QString &statement) {
                    QSqlQuery query(database);
                    if (query.exec(statement)) return true;
                    if (error) *error = query.lastError().text();
                    return false;
                };
                if (!execute(QStringLiteral("PRAGMA busy_timeout=5000"))
                    || !execute(QStringLiteral("PRAGMA journal_mode=WAL"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_cards (card_id TEXT PRIMARY KEY, document_id TEXT, pdf_stem TEXT, question TEXT, status TEXT, state TEXT, current_mark TEXT, due_at TEXT, last_reviewed_at TEXT, review_count INTEGER NOT NULL, lapse_count INTEGER NOT NULL, interval_days INTEGER NOT NULL, is_deleted INTEGER NOT NULL, is_retained INTEGER NOT NULL, card_json TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_events (event_id TEXT PRIMARY KEY, card_id TEXT NOT NULL, reviewed_at TEXT, origin_id TEXT, rating TEXT, review_action TEXT, queue_action TEXT, scheduled_due_at TEXT, retracted_event_id TEXT, is_retracted INTEGER NOT NULL, event_json TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_tag_records (entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, record_json TEXT NOT NULL, PRIMARY KEY (entity_type, entity_id))"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_feedback (feedback_id TEXT PRIMARY KEY, card_id TEXT NOT NULL, polarity TEXT NOT NULL, quote TEXT NOT NULL, comment TEXT NOT NULL, created_at TEXT, updated_at TEXT, deleted INTEGER NOT NULL, record_json TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS review_feedback_card ON review_feedback(card_id, created_at)"))
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS document_feedback (feedback_id TEXT PRIMARY KEY, document_id TEXT NOT NULL, pdf_sha256 TEXT NOT NULL, page INTEGER, polarity TEXT NOT NULL, quote TEXT NOT NULL, comment TEXT NOT NULL, created_at TEXT, updated_at TEXT, deleted INTEGER NOT NULL, record_json TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS document_feedback_document ON document_feedback(document_id, created_at)"))
                    // Retired cards (ReviewRetire::fold), one row each; a table of its own, so older Thoughts versions still write this index.
                    || !execute(QStringLiteral("CREATE TABLE IF NOT EXISTS review_card_retirements (card_id TEXT PRIMARY KEY, retired_at TEXT NOT NULL, retire_event_id TEXT NOT NULL)"))
                    || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS review_cards_due ON review_cards(due_at)"))
                    || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS review_cards_state ON review_cards(state, review_count)"))
                    || !execute(QStringLiteral("CREATE INDEX IF NOT EXISTS review_events_card_time ON review_events(card_id, reviewed_at)"))) return false;
                QSqlQuery previous(database);
                previous.prepare(QStringLiteral("SELECT value FROM review_meta WHERE key='source_hash'"));
                if (!previous.exec()) {
                    if (error) *error = previous.lastError().text();
                    return false;
                }
                if (previous.next() && previous.value(0).toString() == sourceHash) return true;

                QStringList scheduleErrors;
                const QList<ReviewCard> scheduled = cards(&scheduleErrors);
                scheduleErrors.removeIf([](const QString &message) { return message.startsWith(QLatin1String("invalid review card ")); });
                if (!scheduleErrors.isEmpty()) {
                    if (error) *error = scheduleErrors.join(QLatin1Char('\n'));
                    return false;
                }
                QHash<QString, ReviewCard> scheduledById;
                for (const auto &card : scheduled) scheduledById.insert(card.id(), card);
                QHash<QString, QJsonObject> reviewEventsById;
                for (const auto &event : std::as_const(eventRecords)) {
                    if (!event.contains(QStringLiteral("queue_action")))
                        reviewEventsById.insert(event.value(QStringLiteral("event_id")).toString(), event);
                }
                QSet<QString> retracted;
                for (const auto &event : std::as_const(eventRecords)) {
                    const QString id = event.value(QStringLiteral("retracted_event_id")).toString();
                    const auto target = reviewEventsById.value(id);
                    if (!id.isEmpty() && !target.isEmpty()
                        && event.value(QStringLiteral("card_id")) == target.value(QStringLiteral("card_id"))
                        && reviewRating(target.value(QStringLiteral("rating")).toString())
                        && target.value(QStringLiteral("rating")).toString() != QLatin1String("delete")) retracted.insert(id);
                }
                if (!database.transaction()) {
                    if (error) *error = database.lastError().text();
                    return false;
                }
                const auto abort = [&] {
                    database.rollback();
                    return false;
                };
                if (!execute(QStringLiteral("DELETE FROM review_cards")) || !execute(QStringLiteral("DELETE FROM review_events"))
                    || !execute(QStringLiteral("DELETE FROM review_tag_records")) || !execute(QStringLiteral("DELETE FROM review_feedback"))
                    || !execute(QStringLiteral("DELETE FROM document_feedback"))
                    || !execute(QStringLiteral("DELETE FROM review_card_retirements"))) return abort();
                QSqlQuery insertCard(database);
                if (!insertCard.prepare(QStringLiteral("INSERT INTO review_cards VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))) {
                    if (error) *error = insertCard.lastError().text();
                    return abort();
                }
                for (const auto &raw : std::as_const(cardRecords)) {
                    const QString id = raw.value(QStringLiteral("card_id")).toString();
                    const auto primary = raw.value(QStringLiteral("primary")).toObject();
                    const auto card = scheduledById.value(id);
                    const bool deleted = raw.value(QStringLiteral("deleted")).toBool() || card.schedule.deleted;
                    const QString status = raw.value(QStringLiteral("status")).toString();
                    const QString mark = card.currentMark();
                    const QString state = deleted ? QStringLiteral("deleted")
                        : (status == QLatin1String("needs_reanchor") || raw.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("needs_attention")) ? QStringLiteral("unavailable")
                        : !card.wasAnswered() ? QStringLiteral("new") : mark.isEmpty() ? QStringLiteral("no_tag") : mark;
                    const auto dateValue = [](const QDateTime &date) -> QVariant {
                        return date.isValid() ? QVariant(date.toUTC().toString(Qt::ISODateWithMs)) : QVariant();
                    };
                    insertCard.bindValue(0, id);
                    insertCard.bindValue(1, primary.value(QStringLiteral("document_id")).toString());
                    insertCard.bindValue(2, primary.value(QStringLiteral("pdf_stem")).toString());
                    insertCard.bindValue(3, primary.value(QStringLiteral("question")).toString());
                    insertCard.bindValue(4, status);
                    insertCard.bindValue(5, state);
                    insertCard.bindValue(6, mark);
                    insertCard.bindValue(7, dateValue(card.schedule.dueAt.isValid() ? card.schedule.dueAt : QDateTime::fromString(raw.value(QStringLiteral("initial_due_at")).toString(), Qt::ISODate)));
                    insertCard.bindValue(8, dateValue(card.schedule.lastReviewedAt));
                    insertCard.bindValue(9, card.schedule.reviewCount);
                    insertCard.bindValue(10, card.schedule.lapseCount);
                    insertCard.bindValue(11, card.schedule.intervalDays);
                    insertCard.bindValue(12, int(deleted));
                    insertCard.bindValue(13, int(card.schedule.retained));
                    insertCard.bindValue(14, QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Compact)));
                    if (!insertCard.exec()) {
                        if (error) *error = insertCard.lastError().text();
                        return abort();
                    }
                }
                QSqlQuery insertRetirement(database);
                if (!insertRetirement.prepare(QStringLiteral("INSERT INTO review_card_retirements VALUES (?,?,?)"))) {
                    if (error) *error = insertRetirement.lastError().text();
                    return abort();
                }
                for (const auto &raw : std::as_const(cardRecords)) {
                    const auto card = scheduledById.value(raw.value(QStringLiteral("card_id")).toString());
                    if (!card.schedule.retired) continue;
                    insertRetirement.bindValue(0, card.id());
                    insertRetirement.bindValue(1, card.schedule.retiredAt.toUTC().toString(Qt::ISODateWithMs));
                    insertRetirement.bindValue(2, card.schedule.retireEventId);
                    if (!insertRetirement.exec()) {
                        if (error) *error = insertRetirement.lastError().text();
                        return abort();
                    }
                }
                QSqlQuery insertEvent(database);
                if (!insertEvent.prepare(QStringLiteral("INSERT INTO review_events VALUES (?,?,?,?,?,?,?,?,?,?,?)"))) {
                    if (error) *error = insertEvent.lastError().text();
                    return abort();
                }
                for (const auto &event : std::as_const(eventRecords)) {
                    const QString id = event.value(QStringLiteral("event_id")).toString();
                    insertEvent.bindValue(0, id);
                    insertEvent.bindValue(1, event.value(QStringLiteral("card_id")).toString());
                    insertEvent.bindValue(2, event.value(QStringLiteral("reviewed_at")).toString());
                    insertEvent.bindValue(3, event.value(QStringLiteral("origin_id")).toString());
                    insertEvent.bindValue(4, event.value(QStringLiteral("rating")).toString());
                    insertEvent.bindValue(5, event.value(QStringLiteral("review_action")).toString());
                    insertEvent.bindValue(6, event.value(QStringLiteral("queue_action")).toString());
                    insertEvent.bindValue(7, event.value(QStringLiteral("scheduled_due_at")).toString());
                    insertEvent.bindValue(8, event.value(QStringLiteral("retracted_event_id")).toString());
                    insertEvent.bindValue(9, int(retracted.contains(id)));
                    insertEvent.bindValue(10, QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
                    if (!insertEvent.exec()) {
                        if (error) *error = insertEvent.lastError().text();
                        return abort();
                    }
                }
                // Tag merges and the tag groups record, exactly as stored.
                QSqlQuery insertTagRecord(database);
                if (!insertTagRecord.prepare(QStringLiteral("INSERT OR REPLACE INTO review_tag_records VALUES (?,?,?)"))) {
                    if (error) *error = insertTagRecord.lastError().text();
                    return abort();
                }
                const auto addTagRecords = [&](const QList<QJsonObject> &tagRecords, const QString &entityType, const QString &idField) {
                    for (const auto &record : tagRecords) {
                        insertTagRecord.bindValue(0, entityType);
                        insertTagRecord.bindValue(1, record.value(idField).toString());
                        insertTagRecord.bindValue(2, QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact)));
                        if (!insertTagRecord.exec()) {
                            if (error) *error = insertTagRecord.lastError().text();
                            return false;
                        }
                    }
                    return true;
                };
                if (!addTagRecords(mergeRecords, ReviewTags::mergeEntityType(), QStringLiteral("merge_id"))
                    || !addTagRecords(groupRecords, ReviewTags::groupsEntityType(), QStringLiteral("layout_id"))) return abort();
                QSqlQuery insertFeedback(database);
                if (!insertFeedback.prepare(QStringLiteral("INSERT OR REPLACE INTO review_feedback VALUES (?,?,?,?,?,?,?,?,?)"))) {
                    if (error) *error = insertFeedback.lastError().text();
                    return abort();
                }
                for (const auto &feedback : std::as_const(feedbackRecords)) {
                    insertFeedback.bindValue(0, feedback.value(QStringLiteral("feedback_id")).toString());
                    insertFeedback.bindValue(1, feedback.value(QStringLiteral("card_id")).toString());
                    insertFeedback.bindValue(2, feedback.value(QStringLiteral("polarity")).toString());
                    insertFeedback.bindValue(3, feedback.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString());
                    insertFeedback.bindValue(4, feedback.value(QStringLiteral("comment")).toString());
                    insertFeedback.bindValue(5, feedback.value(QStringLiteral("created_at")).toString());
                    insertFeedback.bindValue(6, feedback.value(QStringLiteral("updated_at")).toString());
                    insertFeedback.bindValue(7, int(feedback.value(QStringLiteral("deleted")).toBool()));
                    insertFeedback.bindValue(8, QString::fromUtf8(QJsonDocument(feedback).toJson(QJsonDocument::Compact)));
                    if (!insertFeedback.exec()) {
                        if (error) *error = insertFeedback.lastError().text();
                        return abort();
                    }
                }
                QSqlQuery insertDocumentFeedback(database);
                if (!insertDocumentFeedback.prepare(QStringLiteral("INSERT OR REPLACE INTO document_feedback VALUES (?,?,?,?,?,?,?,?,?,?,?)"))) {
                    if (error) *error = insertDocumentFeedback.lastError().text();
                    return abort();
                }
                for (const auto &feedback : std::as_const(documentFeedbackRecords)) {
                    const QJsonObject anchor = feedback.value(QStringLiteral("anchor")).toObject();
                    const QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
                    const QJsonArray regions = pdf.value(QStringLiteral("regions")).toArray();
                    insertDocumentFeedback.bindValue(0, feedback.value(QStringLiteral("feedback_id")).toString());
                    insertDocumentFeedback.bindValue(1, pdf.value(QStringLiteral("document_id")).toString());
                    insertDocumentFeedback.bindValue(2, pdf.value(QStringLiteral("pdf_sha256")).toString());
                    // The page of the first anchor region, in the PDF the passage was marked on.
                    insertDocumentFeedback.bindValue(3, regions.isEmpty() ? QVariant() : QVariant(regions.first().toObject().value(QStringLiteral("page")).toInt()));
                    insertDocumentFeedback.bindValue(4, feedback.value(QStringLiteral("polarity")).toString());
                    insertDocumentFeedback.bindValue(5, anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString());
                    insertDocumentFeedback.bindValue(6, feedback.value(QStringLiteral("comment")).toString());
                    insertDocumentFeedback.bindValue(7, feedback.value(QStringLiteral("created_at")).toString());
                    insertDocumentFeedback.bindValue(8, feedback.value(QStringLiteral("updated_at")).toString());
                    insertDocumentFeedback.bindValue(9, int(feedback.value(QStringLiteral("deleted")).toBool()));
                    insertDocumentFeedback.bindValue(10, QString::fromUtf8(QJsonDocument(feedback).toJson(QJsonDocument::Compact)));
                    if (!insertDocumentFeedback.exec()) {
                        if (error) *error = insertDocumentFeedback.lastError().text();
                        return abort();
                    }
                }
                QSqlQuery metadata(database);
                metadata.prepare(QStringLiteral("INSERT OR REPLACE INTO review_meta(key,value) VALUES ('source_hash',?)"));
                metadata.addBindValue(sourceHash);
                if (!metadata.exec()) {
                    if (error) *error = metadata.lastError().text();
                    return abort();
                }
                metadata.prepare(QStringLiteral("INSERT OR REPLACE INTO review_meta(key,value) VALUES ('indexed_at_utc',?)"));
                metadata.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                if (!metadata.exec()) {
                    if (error) *error = metadata.lastError().text();
                    return abort();
                }
                if (!database.commit()) {
                    if (error) *error = database.lastError().text();
                    return abort();
                }
                return true;
            };
            success = publish();
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

QList<QJsonObject> ReviewStore::cardFeedback(const QString &cardId, bool includeDeleted, QStringList *errors) const
{
    auto result = records(ReviewCardFeedback::entityType(), ReviewCardFeedback::collection(), QStringLiteral("feedback_id"), errors);
    result.removeIf([&](const QJsonObject &record) {
        return (!cardId.isEmpty() && record.value(QStringLiteral("card_id")).toString() != cardId)
            || (!includeDeleted && record.value(QStringLiteral("deleted")).toBool());
    });
    ReviewCardFeedback::sortByCreation(result);
    return result;
}

QList<QJsonObject> ReviewStore::documentFeedback(const QString &documentId, bool includeDeleted, QStringList *errors) const
{
    auto result = records(DocumentFeedback::entityType(), DocumentFeedback::collection(), QStringLiteral("feedback_id"), errors);
    if (!documentId.isEmpty()) return DocumentFeedback::recordsOfDocument(result, documentId, includeDeleted);
    if (!includeDeleted) result.removeIf([](const QJsonObject &record) { return record.value(QStringLiteral("deleted")).toBool(); });
    ReviewCardFeedback::sortByCreation(result);
    return result;
}

bool ReviewStore::feedbackCardExists(const ReviewCard &card)
{
    const QString status = card.status();
    return !card.schedule.deleted && !card.record.value(QStringLiteral("deleted")).toBool() && status != QLatin1String("candidate")
        && status != QLatin1String("rejected");
}

QHash<QString, QJsonObject> ReviewStore::feedbackHomeCards(const QString &cardId, const QSet<QString> &documentIds) const
{
    QSet<QString> homes;
    for (const QJsonObject &record : cardFeedback()) {
        const QString home = record.value(QStringLiteral("card_id")).toString();
        const QString document = ReviewCardFeedback::documentIdOf(record);
        if (home == cardId || (!documentIds.isEmpty() && DocumentIdentityStore::validId(document) && !documentIds.contains(document))) continue;
        homes.insert(home);
    }
    QHash<QString, QJsonObject> result;
    if (homes.isEmpty()) return result;
    // Only those cards are built: building every card took a second each time a review card was
    // shown (13 Nov).
    for (const ReviewCard &card : cardsWithIds(homes)) {
        if (feedbackCardExists(card)) result.insert(card.id(), card.record);
    }
    return result;
}

QList<QJsonObject> ReviewStore::exclusions(QStringList *errors) const
{
    return records(QStringLiteral("review_exclusion"), QStringLiteral("exclusions"), QStringLiteral("exclusion_id"), errors);
}

QHash<QString, QString> ReviewStore::tagMerges(QStringList *errors) const
{
    return ReviewTags::mergeMapping(records(ReviewTags::mergeEntityType(), ReviewTags::mergeCollection(), QStringLiteral("merge_id"), errors));
}

bool ReviewStore::recordTagMerge(const QStringList &from, const QString &to, QString *error) const
{
    // Same record as Android's ReviewTagMerges.record: `to` itself is dropped from `from`.
    const QString target = to.trimmed();
    QStringList sources;
    for (const QString &tag : from) {
        const QString source = tag.trimmed();
        if (!source.isEmpty() && source != target && !sources.contains(source)) sources.append(source);
    }
    if (target.isEmpty() || sources.isEmpty()) {
        if (error) *error = QStringLiteral("Choose at least one other tag to merge.");
        return false;
    }
    const QString mergeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject merge {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("merge_id"), mergeId},
        {QStringLiteral("from"), QJsonArray::fromStringList(sources)},
        {QStringLiteral("to"), target},
        {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
    };
    return writeServerJson(ReviewTags::mergeCollection() + QLatin1Char('/') + mergeId + QStringLiteral(".json"), merge, error);
}

namespace
{
QJsonObject newestGroupsRecord(const QList<QJsonObject> &records)
{
    QJsonObject newest;
    for (const QJsonObject &record : records) {
        if (record.value(QStringLiteral("layout_id")).toString() != ReviewTags::groupsId()) continue;
        if (newest.isEmpty() || record.value(QStringLiteral("updated_at")).toString() > newest.value(QStringLiteral("updated_at")).toString())
            newest = record;
    }
    return newest;
}
}

QList<ReviewTags::TagGroup> ReviewStore::tagGroups(QStringList *errors) const
{
    const QJsonObject record = newestGroupsRecord(records(ReviewTags::groupsEntityType(), ReviewTags::mergeCollection(), QStringLiteral("layout_id"), errors));
    if (record.isEmpty()) return {};
    QList<ReviewTags::TagGroup> groups;
    QString error;
    if (!ReviewTags::parseGroups(record, &groups, &error)) {
        // An unreadable layout only loses the grouping: every tag is shown under Other.
        if (errors) errors->append(error);
        return {};
    }
    return ReviewTags::effectiveGroups(groups, tagMerges());
}

bool ReviewStore::saveTagGroups(const QList<ReviewTags::TagGroup> &groups, QString *error) const
{
    const QString problem = ReviewTags::groupsError(groups);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return false;
    }
    // Last write wins by updated_at here; never fall behind the layout being replaced, even
    // when it came from a device whose clock runs ahead.
    QDateTime updatedAt = QDateTime::currentDateTimeUtc();
    const QJsonObject current = newestGroupsRecord(records(ReviewTags::groupsEntityType(), ReviewTags::mergeCollection(), QStringLiteral("layout_id")));
    const QDateTime currentAt = QDateTime::fromString(current.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
    if (currentAt.isValid() && currentAt >= updatedAt) updatedAt = currentAt.toUTC().addMSecs(1);
    // Fields of a newer client (on the record or a group) are written back unchanged.
    const QJsonObject record = ReviewTags::groupsRecord(groups, updatedAt.toString(Qt::ISODateWithMs), Thoughts::Platform::reviewOriginId(),
                                                        ReviewTags::unknownRecordFields(current));
    return writeServerJson(groupsFolder() + QLatin1Char('/') + ReviewTags::groupsId() + QStringLiteral(".json"), record, error);
}

QHash<QString, ReviewSchedule> ReviewStore::foldEvents(const QList<QJsonObject> &events, QStringList *errors)
{
    QHash<QString, QJsonObject> unique;
    for (const QJsonObject &event : events) {
        if (event.contains(QStringLiteral("queue_action")) || isInferredSourceDeletion(event)) {
            continue;
        }
        const QString eventId = objectString(event, QStringLiteral("event_id"));
        if (eventId.isEmpty()) {
            if (errors) {
                errors->append(QStringLiteral("review event has no event_id"));
            }
            continue;
        }
        if (!unique.contains(eventId)) {
            unique.insert(eventId, event);
        }
    }
    QList<QJsonObject> ordered = unique.values();
    QSet<QString> retractedEventIds;
    for (const QJsonObject &retraction : std::as_const(ordered)) {
        const QString targetId = objectString(retraction, QStringLiteral("retracted_event_id"));
        const QJsonObject target = unique.value(targetId);
        if (!targetId.isEmpty()
            && !target.isEmpty()
            && !objectString(retraction, QStringLiteral("card_id")).isEmpty()
            && objectString(retraction, QStringLiteral("card_id")) == objectString(target, QStringLiteral("card_id"))
            && reviewRating(objectString(target, QStringLiteral("rating")))
            && objectString(target, QStringLiteral("rating")) != QLatin1String("delete")) {
            retractedEventIds.insert(targetId);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const QJsonObject &left, const QJsonObject &right) {
        const QDateTime leftAt = parseUtc(objectString(left, QStringLiteral("reviewed_at")));
        const QDateTime rightAt = parseUtc(objectString(right, QStringLiteral("reviewed_at")));
        return leftAt == rightAt
            ? objectString(left, QStringLiteral("event_id")) < objectString(right, QStringLiteral("event_id"))
            : leftAt < rightAt;
    });

    QHash<QString, ReviewSchedule> schedules;
    for (const QJsonObject &event : std::as_const(ordered)) {
        if (retractedEventIds.contains(objectString(event, QStringLiteral("event_id")))) {
            continue;
        }
        if (!objectString(event, QStringLiteral("retracted_event_id")).isEmpty()) {
            continue;
        }
        const QString cardId = objectString(event, QStringLiteral("card_id"));
        const QString rating = objectString(event, QStringLiteral("rating"));
        const QDateTime reviewedAt = parseUtc(objectString(event, QStringLiteral("reviewed_at")));
        if (objectString(event, QStringLiteral("interval_action")) == QLatin1String("set")) {
            const QString unit = objectString(event, QStringLiteral("interval_unit"));
            const QDateTime due = parseUtc(objectString(event, QStringLiteral("scheduled_due_at")));
            const double amount = event.value(QStringLiteral("interval_amount")).toDouble();
            const bool never = unit == QLatin1String("never");
            const bool finiteUnit = unit == QLatin1String("minutes") || unit == QLatin1String("hours")
                || unit == QLatin1String("days") || unit == QLatin1String("weeks")
                || unit == QLatin1String("months") || unit == QLatin1String("date");
            if (cardId.isEmpty() || !reviewedAt.isValid() || (!never && (!finiteUnit || !due.isValid()
                || due <= reviewedAt || (unit != QLatin1String("date") && (!std::isfinite(amount) || amount <= 0))))) {
                if (errors) errors->append(QStringLiteral("invalid interval event %1").arg(objectString(event, QStringLiteral("event_id"))));
                continue;
            }
            ReviewSchedule schedule = schedules.value(cardId);
            if (schedule.deleted) continue;
            schedule.retained = never;
            schedule.fixedInterval = true;
            schedule.dueAt = never ? reviewedAt : due;
            schedule.intervalDays = never ? 0 : int(std::min<qint64>(reviewedAt.secsTo(due) / 86400, std::numeric_limits<int>::max()));
            schedule.learningStep = -1;
            schedule.relearning = false;
            schedules.insert(cardId, schedule);
            continue;
        }
        if (cardId.isEmpty() || !reviewRating(rating) || !reviewedAt.isValid()) {
            if (errors) {
                errors->append(QStringLiteral("invalid review event %1").arg(objectString(event, QStringLiteral("event_id"))));
            }
            continue;
        }
        if (schedules.value(cardId).deleted) {
            continue;
        }
        const QDateTime scheduledDueAt = parseUtc(objectString(event, QStringLiteral("scheduled_due_at")));
        if (rating == QLatin1String("custom_interval") && (!scheduledDueAt.isValid() || scheduledDueAt <= reviewedAt)) {
            if (errors) {
                errors->append(QStringLiteral("invalid custom review date %1").arg(objectString(event, QStringLiteral("event_id"))));
            }
            continue;
        }
        const bool hasPrevious = schedules.contains(cardId);
        ReviewSchedule schedule = nextSchedule(schedules.value(cardId),
                                               rating,
                                               reviewedAt,
                                               hasPrevious,
                                               event.value(QStringLiteral("scheduler_version")).toInt(),
                                               scheduledDueAt);
        if (rating == QLatin1String("delete")) {
            schedule.deleteEventId = objectString(event, QStringLiteral("event_id"));
        }
        const auto previous = schedules.value(cardId);
        schedule.suggestionReviewedAt = previous.suggestionReviewedAt;
        schedule.suggestionDueAt = previous.suggestionDueAt;
        if (!schedule.deleted && !schedule.retained) {
            schedule.suggestionReviewedAt = reviewedAt;
            schedule.suggestionDueAt = schedule.dueAt;
        }
        schedules.insert(cardId, schedule);
    }
    return schedules;
}

ReviewSchedule ReviewStore::nextSchedule(const ReviewSchedule &previous,
                                         const QString &rating,
                                         const QDateTime &reviewedAt,
                                         bool hasPrevious,
                                         int schedulerVersion,
                                         const QDateTime &scheduledDueAt)
{
    ReviewSchedule schedule;
    schedule.reviewed = true;
    schedule.reviewCount = hasPrevious ? previous.reviewCount : 0;
    schedule.lapseCount = hasPrevious ? previous.lapseCount : 0;
    schedule.lastRating = rating;
    schedule.lastReviewedAt = reviewedAt.toUTC();
    if (rating == QLatin1String("delete")) {
        schedule.deleted = true;
        schedule.dueAt = reviewedAt.toUTC();
        return schedule;
    }
    ++schedule.reviewCount;
    if (rating == QLatin1String("again")
        && (schedulerVersion < currentSchedulerVersion || (hasPrevious && previous.intervalDays > 0))) {
        ++schedule.lapseCount;
    }

    // Explicit Android choices preserve their exact delay and bypass learn-ahead.
    // Historical Again/Hard/Good/Easy events keep their original scheduler.
    if (rating == QLatin1String("retained")) {
        schedule.retained = true;
        schedule.dueAt = reviewedAt.toUTC();
        return schedule;
    }
    if (rating == QLatin1String("custom_interval")) {
        if (!scheduledDueAt.isValid() || scheduledDueAt <= reviewedAt) {
            return previous;
        }
        schedule.fixedInterval = true;
        schedule.dueAt = scheduledDueAt.toUTC();
        schedule.intervalDays = int(std::min<qint64>(reviewedAt.secsTo(scheduledDueAt) / 86400, std::numeric_limits<int>::max()));
        return schedule;
    }
    if (rating == QLatin1String("ten_minutes") || rating == QLatin1String("next_day") || rating == QLatin1String("next_week")) {
        schedule.fixedInterval = true;
        schedule.intervalDays = rating == QLatin1String("next_week") ? 7 : (rating == QLatin1String("next_day") ? 1 : 0);
        schedule.dueAt = schedule.intervalDays > 0 ? reviewedAt.toUTC().addDays(schedule.intervalDays) : reviewedAt.toUTC().addSecs(10 * 60);
        return schedule;
    }

    if (schedulerVersion < currentSchedulerVersion) {
        if (rating == QLatin1String("again")) {
            const int previousReviewDays = !hasPrevious || previous.deleted
                ? 0
                : (previous.intervalDays > 0 ? previous.intervalDays : previous.previousIntervalDays);
            schedule.relearning = previousReviewDays > 0;
            schedule.previousIntervalDays = previousReviewDays;
            schedule.dueAt = reviewedAt.toUTC().addSecs(10 * 60);
            return schedule;
        }
        const int previousDays = !hasPrevious || previous.deleted ? 0 : previous.intervalDays;
        if (rating == QLatin1String("hard")) {
            schedule.intervalDays = previousDays <= 0 ? 1 : std::max(1, qRound(previousDays * 1.2));
        } else if (rating == QLatin1String("good")) {
            schedule.intervalDays = previousDays <= 0 ? 3 : std::max(previousDays + 1, qRound(previousDays * 2.5));
        } else if (rating == QLatin1String("easy")) {
            schedule.intervalDays = previousDays <= 0 ? 7 : std::max(previousDays + 1, qRound(previousDays * 4.0));
        }
        schedule.intervalDays = std::min(maxIntervalDays, schedule.intervalDays);
        schedule.dueAt = reviewedAt.toUTC().addDays(schedule.intervalDays);
        return schedule;
    }

    const int learningStep = previous.learningStep >= 0
        ? previous.learningStep
        : (hasPrevious && previous.intervalDays == 0 ? 0 : -1);
    const bool isLearning = learningStep >= 0;
    const bool isRelearning = previous.relearning;
    const int previousReviewDays = !hasPrevious || previous.deleted
        ? 0
        : (isLearning ? previous.previousIntervalDays : previous.intervalDays);

    const auto learningSchedule = [&](int delayMinutes, int step, bool relearning) {
        schedule.intervalDays = 0;
        schedule.learningStep = step;
        schedule.relearning = relearning;
        schedule.previousIntervalDays = previousReviewDays;
        schedule.dueAt = reviewedAt.toUTC().addSecs(delayMinutes * 60);
        return schedule;
    };
    const auto reviewInterval = [&](const QString &passingRating) {
        if (passingRating == QLatin1String("hard")) {
            return std::max(1, qRound(previousReviewDays * 1.2));
        }
        if (passingRating == QLatin1String("good")) {
            return std::max(previousReviewDays + 1, qRound(previousReviewDays * 2.5));
        }
        return std::max(previousReviewDays + 1, qRound(previousReviewDays * 4.0));
    };
    const auto reviewSchedule = [&](int intervalDays) {
        schedule.intervalDays = std::min(maxIntervalDays, intervalDays);
        schedule.dueAt = reviewedAt.toUTC().addDays(schedule.intervalDays);
        return schedule;
    };

    if (!hasPrevious || isLearning) {
        const bool relearning = hasPrevious && isRelearning;
        if (rating == QLatin1String("again")) {
            return learningSchedule(relearning ? relearningStepMinutes : firstLearningStepMinutes, 0, relearning);
        }
        if (rating == QLatin1String("hard")) {
            const int delay = relearning
                ? relearningHardMinutes
                : (learningStep == 1 ? secondLearningStepMinutes : firstLearningHardMinutes);
            return learningSchedule(delay, learningStep >= 0 ? learningStep : 0, relearning);
        }
        if (rating == QLatin1String("good")) {
            if (relearning) {
                return reviewSchedule(relearningIntervalDays);
            }
            if (learningStep == 1) {
                return reviewSchedule(graduatingIntervalDays);
            }
            return learningSchedule(secondLearningStepMinutes, 1, false);
        }
        if (rating == QLatin1String("easy")) {
            return reviewSchedule(relearning && previousReviewDays > 0
                                      ? reviewInterval(rating)
                                      : easyIntervalDays);
        }
    }

    if (rating == QLatin1String("again")) {
        return learningSchedule(relearningStepMinutes, 0, true);
    }
    return reviewSchedule(reviewInterval(rating));
}

namespace
{
std::atomic<quint64> guiThreadFullCardLoads{0};
}

quint64 ReviewStore::guiThreadFullLoads()
{
    return guiThreadFullCardLoads.load();
}

QList<ReviewCard> ReviewStore::cards(QStringList *errors, const QString &pdfStem) const
{
    if (pdfStem.isEmpty() && QThread::isMainThread()) ++guiThreadFullCardLoads;
    return loadCards(errors, pdfStem, nullptr);
}

QList<ReviewCard> ReviewStore::cardsWithIds(const QSet<QString> &cardIds, QStringList *errors) const
{
    if (cardIds.isEmpty()) return {};
    return loadCards(errors, {}, &cardIds);
}

QList<ReviewCard> ReviewStore::loadCards(QStringList *errors, const QString &pdfStem, const QSet<QString> *onlyIds) const
{
    QList<QJsonObject> cardRecords = records(QStringLiteral("review_card"), QStringLiteral("cards"), QStringLiteral("card_id"), errors, pdfStem);
    if (onlyIds) cardRecords.removeIf([onlyIds](const QJsonObject &record) { return !onlyIds->contains(objectString(record, QStringLiteral("card_id"))); });
    QSet<QString> cardIds;
    for (const auto &record : cardRecords) cardIds.insert(objectString(record, QStringLiteral("card_id")));
    // Events and tag merges come from one snapshot; each provider call rebuilds every projection.
    const QJsonObject snapshot = m_projectionProvider ? m_projectionProvider() : QJsonObject();
    const QJsonObject *shared = m_projectionProvider ? &snapshot : nullptr;
    QList<QJsonObject> allEvents = records(QStringLiteral("review_event"), QStringLiteral("events"), QStringLiteral("event_id"), errors, {}, shared);
    allEvents.removeIf(isInferredSourceDeletion);
    if (!pdfStem.isEmpty() || onlyIds) allEvents.removeIf([&](const QJsonObject &event) {
        return !cardIds.contains(objectString(event, QStringLiteral("card_id")));
    });
    // History includes queue actions; scheduling deliberately folds only review marks.
    std::sort(allEvents.begin(), allEvents.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const auto at = parseUtc(objectString(a, QStringLiteral("reviewed_at")));
        const auto bt = parseUtc(objectString(b, QStringLiteral("reviewed_at")));
        return at == bt ? objectString(a, QStringLiteral("event_id")) < objectString(b, QStringLiteral("event_id")) : at < bt;
    });
    QHash<QString, QList<QJsonObject>> histories;
    for (const auto &event : std::as_const(allEvents)) histories[objectString(event, QStringLiteral("card_id"))].append(event);
    const QHash<QString, ReviewSchedule> schedules = foldEvents(allEvents, errors);
    auto queueEvents = allEvents;
    queueEvents.removeIf([&](const auto &event) {
        return event.value(QStringLiteral("queue_action")).toString() == QLatin1String("skip")
            && parseUtc(event.value(QStringLiteral("reviewed_at")).toString()) < schedules.value(event.value(QStringLiteral("card_id")).toString()).lastReviewedAt;
    });
    const QHash<QString, QDateTime> skippedUntil = skippedUntilByCard(queueEvents);
    // Retire and restore never touch the schedule; they only say whether the card is reviewed.
    const QHash<QString, ReviewRetire::State> retirements = ReviewRetire::fold(allEvents);
    // A malformed merge is skipped; it must not make the cards themselves unavailable.
    const QHash<QString, QString> merges = ReviewTags::mergeMapping(
        records(ReviewTags::mergeEntityType(), ReviewTags::mergeCollection(), QStringLiteral("merge_id"), nullptr, {}, shared));
    // One view of where the sources hold each card's passage, for every card.
    const QJsonObject locationIndex = sourceLocationIndex(m_docsRoot);
    QList<ReviewCard> output;
    for (const QJsonObject &savedRecord : cardRecords) {
        QJsonObject record = savedRecord;
        const QString id = objectString(record, QStringLiteral("card_id"));
        if (!validReviewCardRecord(record)) {
            if (errors) {
                errors->append(QStringLiteral("invalid review card %1").arg(id));
            }
            continue;
        }
        if (record.value(QStringLiteral("deleted")).toBool() || objectString(record, QStringLiteral("status")) == QLatin1String("candidate")) continue;
        // A retired card stays on the PDF version it was retired on, with its passage
        // as it was then: no source scan moves it (thoughts-server protocol/review-card-retire.md 5).
        const bool retired = retirements.value(id).retired;
        const auto savedAnchor = primaryObject(record).value(QStringLiteral("source_anchor")).toObject();
        const auto link = record.value(QStringLiteral("source_link")).toObject();
        const QString failedSource = savedAnchor.isEmpty() && link.value(QStringLiteral("state")).toString() == QLatin1String("linking_failed")
            ? link.value(QStringLiteral("source_file")).toString() : QString();
        const auto currentAnchor = retired ? QJsonObject() : currentReviewSourceAnchor(m_docsRoot, id, savedAnchor, failedSource);
        if (!currentAnchor.isEmpty() && currentAnchor != savedAnchor) {
            auto primary = primaryObject(record);
            primary.insert(QStringLiteral("source_anchor"), currentAnchor);
            record.insert(QStringLiteral("primary"), primary);
            if (!failedSource.isEmpty()) {
                record.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
                record.insert(QStringLiteral("source_link"), QJsonObject{
                    {QStringLiteral("state"), QStringLiteral("linked")},
                    {QStringLiteral("source_file"), currentAnchor.value(QStringLiteral("segments")).toArray().first().toObject().value(QStringLiteral("source_file"))}});
            }
            const auto reconciliation = savedRecord.value(QStringLiteral("source_anchor_reconciliation")).toObject();
            const auto placed = reconciliation.isEmpty() ? savedAnchor : reconciliation.value(QStringLiteral("placement_anchor")).toObject();
            const auto placedSegments = placed.value(QStringLiteral("segments")).toArray();
            const bool answerChanged = !sameReviewPassages(placedSegments, currentAnchor.value(QStringLiteral("segments")).toArray());
            if (answerChanged) {
                if (reconciliation.isEmpty()) record.insert(QStringLiteral("source_anchor_reconciliation"), QJsonObject{
                    {QStringLiteral("placement_anchor"), savedAnchor},
                    {QStringLiteral("placement_reanchor"), savedRecord.value(QStringLiteral("reanchor"))}});
                record.insert(QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("needs_attention")},
                    {QStringLiteral("reason"), QStringLiteral("pdf_placement_pending")}});
            } else if (!reconciliation.isEmpty()) {
                record.remove(QStringLiteral("source_anchor_reconciliation"));
                if (reconciliation.value(QStringLiteral("placement_reanchor")).isObject())
                    record.insert(QStringLiteral("reanchor"), reconciliation.value(QStringLiteral("placement_reanchor")));
                else record.remove(QStringLiteral("reanchor"));
            }
            // Source scanning is a local presentation overlay, not a user edit.
            // Publishing source revisions lets the server reconcile durable placement;
            // merely opening the card browser must never create an outgoing operation.
        }
        ReviewCard card{record, schedules.value(id), skippedUntil.value(id), histories.value(id)};
        card.tagMerges = merges;
        if (!card.schedule.reviewed && !card.schedule.dueAt.isValid())
            card.schedule.dueAt = parseUtc(objectString(record, QStringLiteral("initial_due_at")));
        if (const auto retirement = retirements.constFind(id); retirement != retirements.cend() && retirement->retired && !card.schedule.deleted) {
            card.schedule.retired = true;
            card.schedule.retiredAt = retirement->retiredAt;
            card.schedule.retireEventId = retirement->retireEventId;
        }
        for (const auto &event : histories.value(id)) {
            if (event.value(QStringLiteral("rating")).toString() == QLatin1String("delete"))
                card.record.insert(QStringLiteral("marker_removal"), event.value(QStringLiteral("marker_removal")));
        }
        // A removal left pending is done once the markers are gone, whoever removed them.
        if (card.record.value(QStringLiteral("marker_removal")).toString() == QLatin1String("pending")
            && !cardMarkersPresent(m_docsRoot, primaryObject(record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray(), id))
            card.record.insert(QStringLiteral("marker_removal"), QStringLiteral("complete"));
        // Once linked, local source markup is authoritative even before Sync.
        // Never continue showing an old answer when its marked content changed.
        if (primaryObject(record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("version")).toInt() == 2) {
            const auto anchor = primaryObject(record).value(QStringLiteral("source_anchor")).toObject();
            const auto segments = anchor.value(QStringLiteral("segments")).toArray();
            if (!segments.isEmpty()) {
                QString relative = segments.first().toObject().value(QStringLiteral("source_file")).toString();
                if (locationIndex.contains(QStringLiteral("files"))) {
                    const QString project = SourceOwner::of(relative);
                    card.sourceLocation = annotationSourceLocation(locationIndex, QStringLiteral("review_card"), id, project, relative);
                    // While its project's sources are being checked (the scan after each Sync or edit,
                    // or a file still settling) the index has no answer yet. Changes to the card wait
                    // for the check (unavailable), but review does not: every card of the project would
                    // otherwise drop out of review until the scan ends. A broken file that holds or
                    // mentions the card is no check.
                    if (!project.isEmpty() && !locationIndex.value(QStringLiteral("projects")).toObject().value(project).toBool()) {
                        const QJsonObject files = locationIndex.value(QStringLiteral("files")).toObject();
                        bool broken = false;
                        for (auto it = files.begin(); it != files.end() && !broken; ++it) {
                            const QJsonObject file = it.value().toObject();
                            broken = !file.value(QStringLiteral("valid")).toBool()
                                && (it.key() == relative || file.value(QStringLiteral("mentioned")).toArray().contains(id));
                        }
                        if (!broken) card.sourceLocation.insert(QStringLiteral("checking"), true);
                    }
                }
                bool verifiedPendingSource = false;
                if (record.value(QStringLiteral("creation_method")).toString() == QLatin1String("source_selection_v1")
                    && primaryObject(record).value(QStringLiteral("pdf_sha256")).toString().isEmpty()) {
                    QFile file(QDir(m_docsRoot).filePath(relative));
                    if (file.open(QIODevice::ReadOnly)) {
                        const auto bytes = file.readAll();
                        const auto current = readReviewSourceAnchor(relative, bytes, id);
                        const auto currentSegments = current.anchor.value(QStringLiteral("segments")).toArray();
                        bool sameAnswer = current.error.isEmpty() && currentSegments.size() == segments.size();
                        for (int i = 0; sameAnswer && i < segments.size(); ++i) {
                            const auto saved = segments[i].toObject();
                            const auto observed = currentSegments[i].toObject();
                            sameAnswer = saved.value(QStringLiteral("segment_id")) == observed.value(QStringLiteral("segment_id"))
                                && saved.value(QStringLiteral("source_file")) == observed.value(QStringLiteral("source_file"))
                                && saved.value(QStringLiteral("content_sha256")) == observed.value(QStringLiteral("content_sha256"));
                        }
                        if (sameAnswer) {
                            card.sourceLocation = currentSegments.first().toObject();
                            card.sourceLocation.insert(QStringLiteral("state"), QStringLiteral("linked"));
                            verifiedPendingSource = true;
                        } else {
                            card.sourceLocation = QJsonObject{{QStringLiteral("state"), QStringLiteral("unavailable")}};
                        }
                    }
                }
                const auto locationState = card.sourceLocation.value(QStringLiteral("state")).toString();
                if (locationState == QLatin1String("linked")) relative = card.sourceLocation.value(QStringLiteral("source_file")).toString();
                bool changed = false;
                if (locationState == QLatin1String("linked") && !verifiedPendingSource) {
                    changed = !sameReviewPassages(segments, reviewSourceEvidence(m_docsRoot, id, SourceOwner::of(relative), relative));
                } else if (locationState == QLatin1String("ambiguous") && segments.size() == 1) changed = true;
                // A retired card keeps the passage it was retired with: nothing to place again.
                if (changed && !card.schedule.retired) card.record.insert(QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("needs_attention")},
                    {QStringLiteral("reason"), QStringLiteral("source_content_changed_or_unavailable")}});
            }
        }
        output.append(card);
    }
    return output;
}

bool ReviewCard::isWholeDocument() const
{
    return primaryObject(record).value(QStringLiteral("scope")).toString() == QLatin1String("document");
}

QString ReviewCard::linkingState() const
{
    if (record.value(QStringLiteral("marker_removal")).toString() == QLatin1String("pending")) return QStringLiteral("marker_removal_pending");
    if (primaryObject(record).value(QStringLiteral("source_kind")).toString() == QLatin1String("pdf")
        && primaryObject(record).value(QStringLiteral("source_anchor")).toObject().isEmpty()) return QStringLiteral("pdf_without_source");
    if (!sourceLocation.isEmpty() && sourceLocation.value(QStringLiteral("state")).toString() != QLatin1String("linked")) return QStringLiteral("unavailable");
    if (!primaryObject(record).value(QStringLiteral("source_selection")).toObject().isEmpty()
        && primaryObject(record).value(QStringLiteral("source_anchor")).toObject().isEmpty()) return record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString(QStringLiteral("pending"));
    if (!primaryObject(record).value(QStringLiteral("source_anchor")).toObject().isEmpty()) return QStringLiteral("linked");
    return record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString();
}

static bool hasReviewIssue(const ReviewCard &card, bool stale)
{
    const QString reanchorState = card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
    // A passage only waiting for its project's source check keeps the card in review.
    const QString link = card.linkingState() == QLatin1String("unavailable") && card.sourceLocation.value(QStringLiteral("checking")).toBool()
        ? QStringLiteral("linked") : card.linkingState();
    return reanchorState == QLatin1String("needs_attention") || card.status() == QLatin1String("needs_reanchor")
        || QStringList{QStringLiteral("queued"), QStringLiteral("retry"), QStringLiteral("running"), QStringLiteral("updating")}.contains(reanchorState)
        || stale
        || QStringList{QStringLiteral("marker_removal_pending"), QStringLiteral("failed"), QStringLiteral("linking_failed"), QStringLiteral("unavailable"), QStringLiteral("ambiguous"), QStringLiteral("pending"),
                       QStringLiteral("pending_server_link")}.contains(link);
}

QString ReviewCard::linkingReason() const
{
    return record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("reason")).toString();
}

QString ReviewCard::linkingSource() const
{
    if (sourceLocation.value(QStringLiteral("state")).toString() == QLatin1String("linked")) return sourceLocation.value(QStringLiteral("source_file")).toString();
    const auto segments = primaryObject(record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    if (!segments.isEmpty()) return segments.first().toObject().value(QStringLiteral("source_file")).toString();
    return record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("source_file")).toString();
}

bool ReviewStore::checkManualSourceLink(const ReviewCard &expected, const QString &sourcePath, QString *error)
{
    auto card = cardRecord(expected.id(), error);
    if (card.isEmpty()) return false;
    if (card != expected.record || expected.linkingState() != QLatin1String("linking_failed")) {
        if (error) *error = QStringLiteral("The card changed. Reload it before checking the connection.");
        return false;
    }
    const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
    const QString absolute = QFileInfo(sourcePath).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(absolute);
    const QString suffix = QFileInfo(relative).suffix().toLower();
    const bool tex = suffix == QLatin1String("tex");
    const bool markdown = suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
    if (root.isEmpty() || absolute.isEmpty() || !absolute.startsWith(root + QLatin1Char('/'))
        || (!tex && !markdown) || !editableReviewSource(relative)) {
        if (error) *error = QStringLiteral("The linked source must be an editable TeX or Markdown file in this documentation library.");
        return false;
    }
    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    const auto bytes = file.readAll();
    file.close();
    const auto plan = readReviewSourceAnchor(relative, bytes, expected.id());
    if (!plan.error.isEmpty()) { if (error) *error = plan.error; return false; }
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != bytes) {
        if (error) *error = QStringLiteral("The source changed while checking the connection. Check it again.");
        return false;
    }
    // Windows cannot replace a file that is still open.
    file.close();
    auto primary = primaryObject(card);
    primary.insert(QStringLiteral("source_anchor"), plan.anchor);
    card.insert(QStringLiteral("primary"), primary);
    card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")},
        {QStringLiteral("source_file"), relative}});
    card = prepareCardUpdate(card);
    return commitSourceChange(absolute, plan.original, plan.replacement,
        QStringLiteral("cards/%1.json").arg(expected.id()), card, error);
}

QHash<QString, QString> ReviewStore::canonicalPdfHashes(QStringList *errors) const
{
    const QString indexPath = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"));
    QString error;
    const QJsonObject index = readObject(indexPath, &error);
    if (index.value(QStringLiteral("schema_version")).toInt() != 1 || !index.value(QStringLiteral("pdfs")).isArray()) {
        if (errors) {
            errors->append(QStringLiteral("%1: %2").arg(indexPath, error.isEmpty() ? QStringLiteral("invalid Library index") : error));
        }
        return {};
    }
    QHash<QString, QString> hashes;
    for (const QJsonValue &value : index.value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString filename = objectString(entry, QStringLiteral("filename"));
        const QString digest = objectString(entry, QStringLiteral("pdf_sha256"));
        if (!filename.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) || !validSha256(digest)) {
            continue;
        }
        hashes.insert(QFileInfo(filename).completeBaseName(), digest);
    }
    return hashes;
}

namespace
{
bool givenOnVersion(const QJsonObject &record, const QString &pdfSha256, bool orMarkedOn)
{
    if (record.value(FeedbackVersion::field()).toObject().value(QStringLiteral("pdf_sha256")).toString() == pdfSha256) return true;
    return orMarkedOn
        && record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString() == pdfSha256;
}

QString agentQuote(const QJsonObject &record)
{
    QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString().simplified();
    const QList<uint> points = quote.toUcs4();
    if (points.size() > 300) quote = QString::fromUcs4(reinterpret_cast<const char32_t *>(points.constData()), 300) + QStringLiteral("…");
    return quote;
}

// What a document feedback record is, as the phone's Copy for agent names it.
QString agentFeedbackLabel(const QJsonObject &record)
{
    const QString kind = DocumentFeedback::kindOf(record);
    if (kind == DocumentFeedback::question()) return QStringLiteral("Question");
    if (kind == DocumentFeedback::other()) return QStringLiteral("Comment");
    if (kind == DocumentFeedback::drawing()) return QStringLiteral("Drawing");
    if (kind == DocumentFeedback::textBox()) return QStringLiteral("Text box");
    return record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative() ? QStringLiteral("Disliked") : QStringLiteral("Liked");
}

QString agentTagLabel(const QString &tag)
{
    if (tag == ReviewTags::correct()) return QStringLiteral("Correct");
    if (tag == ReviewTags::wrong()) return QStringLiteral("Wrong");
    return tag;
}
}

QString agentVersionFeedback(const QList<ReviewCard> &cards, const QList<QJsonObject> &answerFeedback, const QList<QJsonObject> &textFeedback,
                             const QList<ReviewTags::TagGroup> &groups, const QString &pdfSha256)
{
    if (pdfSha256.isEmpty()) return {};
    // The user's rule (10 November): likes, dislikes and questions belong to the version they were
    // given on, as do drawings, text boxes and a card's answer feedback, so only what was given on
    // this version counts, even where the reader finds an older passage again. Comments never go to
    // agents.
    const auto feedbackHere = [&](const QJsonObject &record) {
        return DocumentFeedback::kindOf(record) != DocumentFeedback::other() && givenOnVersion(record, pdfSha256, true);
    };
    QStringList lines;
    QList<const ReviewCard *> reviewed;
    for (const ReviewCard &card : cards) {
        if (card.schedule.deleted) continue;
        const QList<QJsonObject> answers = card.answers();
        const bool answered = std::any_of(answers.cbegin(), answers.cend(),
                                          [&](const QJsonObject &event) { return givenOnVersion(event, pdfSha256, false); });
        const bool feedback = std::any_of(answerFeedback.cbegin(), answerFeedback.cend(), [&](const QJsonObject &record) {
            return record.value(QStringLiteral("card_id")).toString() == card.id() && !record.value(QStringLiteral("deleted")).toBool() && feedbackHere(record);
        });
        if (answered || feedback) reviewed.append(&card);
    }
    std::stable_sort(reviewed.begin(), reviewed.end(), [](const ReviewCard *a, const ReviewCard *b) { return a->page() < b->page(); });
    if (!reviewed.isEmpty()) lines.append(QStringLiteral("Cards reviewed on this PDF version:"));
    for (const ReviewCard *card : std::as_const(reviewed)) {
        const QString pages = card->pagesLabel();
        const QString page = pages.isEmpty() || pages == QLatin1String("0") ? QString() : QStringLiteral(" (page %1)").arg(pages);
        lines.append(QStringLiteral("- Card %1%2: \"%3\"").arg(card->id(), page, card->question().simplified()));
        for (const QJsonObject &event : card->answers()) {
            if (!givenOnVersion(event, pdfSha256, false)) continue;
            const QDateTime at = QDateTime::fromString(event.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODate);
            const QStringList tags = card->tagsOfEvent(event);
            QStringList listed, parts;
            for (const auto &group : groups) {
                QStringList chosen;
                for (const QString &tag : group.scale + group.tags) {
                    if (tags.contains(tag) && !listed.contains(tag)) {
                        chosen.append(agentTagLabel(tag));
                        listed.append(tag);
                    }
                }
                if (!chosen.isEmpty()) parts.append(QStringLiteral("%1: %2").arg(group.name, chosen.join(QStringLiteral(", "))));
            }
            QStringList other;
            for (const QString &tag : tags) {
                if (!listed.contains(tag)) other.append(agentTagLabel(tag));
            }
            if (!other.isEmpty()) parts.append(QStringLiteral("%1: %2").arg(ReviewTags::otherGroupName(), other.join(QStringLiteral(", "))));
            lines.append(QStringLiteral("  - Reviewed %1: %2").arg(at.isValid() ? at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("(no date)"),
                                                                 parts.isEmpty() ? QStringLiteral("no tags") : parts.join(QStringLiteral("; "))));
            for (const QString &note : ReviewTags::groupNoteLines(ReviewTags::groupNotesOfEvent(event), groups))
                lines.append(QStringLiteral("    - ") + note.simplified());
        }
        QList<QJsonObject> feedback;
        for (const QJsonObject &record : answerFeedback) {
            if (record.value(QStringLiteral("card_id")).toString() == card->id() && feedbackHere(record)) feedback.append(record);
        }
        const QString section = ReviewCardFeedback::agentSection(feedback, {});
        for (const QString &line : section.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) lines.append(QStringLiteral("  ") + line);
    }
    QList<QJsonObject> text;
    for (const QJsonObject &record : textFeedback) {
        if (!record.value(QStringLiteral("deleted")).toBool() && feedbackHere(record)) text.append(record);
    }
    ReviewCardFeedback::sortByCreation(text);
    if (!text.isEmpty()) lines.append(QStringLiteral("Text feedback on this PDF version:"));
    for (const QJsonObject &record : std::as_const(text)) {
        const QJsonArray regions = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray();
        const int page = regions.isEmpty() ? 0 : regions.first().toObject().value(QStringLiteral("page")).toInt();
        // A drawing or a text box is on its page, not on a passage: it has no quote of its own.
        QString line = QStringLiteral("- %1%2").arg(agentFeedbackLabel(record), page > 0 ? QStringLiteral(" (page %1)").arg(page) : QString());
        if (!DocumentFeedback::isPageMark(record)) line += QStringLiteral(": \"%1\"").arg(agentQuote(record));
        const QString comment = record.value(QStringLiteral("comment")).toString().simplified();
        if (!comment.isEmpty()) line += QStringLiteral(" — ") + comment;
        if (DocumentFeedback::canResolve(record) && DocumentFeedback::isResolved(record)) line += QStringLiteral(" (resolved)");
        lines.append(line);
    }
    return lines.isEmpty() ? QString() : lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString cardDocumentId(const ReviewCard &card, const QString &docsRoot)
{
    // protocol/project-document-publication.md §4: the PDF's document for a card made
    // in an attachment, else the card's own document, else its Library PDF's.
    const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
    for (const char *key : {"pdf_document_id", "document_id"}) {
        const QString id = primary.value(QLatin1String(key)).toString();
        if (!id.isEmpty()) return id;
    }
    return DocumentIdentityStore(docsRoot).byFilename(card.pdfStem() + QStringLiteral(".pdf")).value(QStringLiteral("document_id")).toString();
}

QList<ReviewCard> ReviewStore::activeCards(QStringList *errors, const QString &pdfStem) const
{
    return activeOf(cards(errors, pdfStem), errors);
}

QList<ReviewCard> ReviewStore::activeOf(const QList<ReviewCard> &cards, QStringList *errors, bool compareLibraryPdfs) const
{
    const QList<QJsonObject> currentExclusions = exclusions(errors);
    const bool comparePdfs = compareLibraryPdfs && m_projectionProvider;
    const QHash<QString, QString> canonicalHashes = comparePdfs ? canonicalPdfHashes(errors) : QHash<QString, QString>();
    const QSet<QString> unpublished = ProjectConfiguration::unpublishedDocumentIds(m_docsRoot);
    QList<ReviewCard> output;
    for (const ReviewCard &card : cards) {
        if (!card.isApproved() || card.schedule.deleted || card.schedule.retained || card.schedule.retired) {
            continue;
        }
        // Kept with its schedule, but its document is unpublished.
        if (!unpublished.isEmpty() && unpublished.contains(cardDocumentId(card, m_docsRoot))) {
            continue;
        }
        const QString projectionState = card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
        if (projectionState == QLatin1String("queued") || projectionState == QLatin1String("retry") || projectionState == QLatin1String("running")
            || projectionState == QLatin1String("updating") || projectionState == QLatin1String("needs_attention")) {
            continue;
        }
        // A card in an HTML attachment is found by its text in the page, not on its owner's PDF.
        const bool stale = !Thoughts::isHtmlAttachmentCard(card.record) && comparePdfs && canonicalHashes.value(card.pdfStem()) != card.pdfSha256();
        if (hasReviewIssue(card, stale)) {
            continue;
        }
        bool excluded = false;
        for (const QJsonObject &exclusion : currentExclusions) {
            const QString pageVisualSha256 = objectString(exclusion, QStringLiteral("page_visual_sha256"));
            const QString pdfStem = objectString(exclusion, QStringLiteral("pdf_stem"));
            const int page = exclusion.value(QStringLiteral("page")).toInt();
            if ((!pageVisualSha256.isEmpty() && pageVisualSha256 == card.pageVisualSha256()) || (!pdfStem.isEmpty() && pdfStem == card.pdfStem() && page == card.page())) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            output.append(card);
        }
    }
    return output;
}

QList<ReviewCard> ReviewStore::candidateCards(QStringList *errors) const
{
    QList<ReviewCard> output;
    for (const ReviewCard &card : cards(errors)) {
        if (card.status() == QLatin1String("candidate")) {
            output.append(card);
        }
    }
    return output;
}

QList<ReviewBrowserCard> ReviewStore::browserCards(const QDateTime &now, QStringList *errors) const
{
    return browserCardsOf(cards(errors), now, errors);
}

QList<ReviewBrowserCard> ReviewStore::browserCardsOf(const QList<ReviewCard> &cards, const QDateTime &now, QStringList *errors) const
{
    const QList<QJsonObject> currentExclusions = exclusions(errors);
    const QHash<QString, QString> canonicalHashes = m_projectionProvider ? canonicalPdfHashes(errors) : QHash<QString, QString>();
    const QSet<QString> unpublished = ProjectConfiguration::unpublishedDocumentIds(m_docsRoot);
    QList<ReviewBrowserCard> output;
    for (const ReviewCard &card : cards) {
        if (card.schedule.deleted) continue;
        bool excluded = false;
        for (const QJsonObject &exclusion : currentExclusions) {
            const QString pageVisualSha256 = objectString(exclusion, QStringLiteral("page_visual_sha256"));
            const QString pdfStem = objectString(exclusion, QStringLiteral("pdf_stem"));
            const int page = exclusion.value(QStringLiteral("page")).toInt();
            if ((!pageVisualSha256.isEmpty() && pageVisualSha256 == card.pageVisualSha256())
                || (!pdfStem.isEmpty() && pdfStem == card.pdfStem() && page == card.page())) {
                excluded = true;
                break;
            }
        }

        const QString reanchorState = card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
        ReviewBrowserState state = ReviewBrowserState::New;
        if (card.schedule.deleted) {
            state = ReviewBrowserState::Deleted;
        } else if (card.status() == QLatin1String("candidate")) {
            state = ReviewBrowserState::Candidate;
        } else if (card.status() == QLatin1String("rejected")) {
            state = ReviewBrowserState::Rejected;
        } else if (card.schedule.retired) {
            state = ReviewBrowserState::Retired;
        } else if (!unpublished.isEmpty() && unpublished.contains(cardDocumentId(card, m_docsRoot))) {
            state = ReviewBrowserState::Unpublished;
        } else if (reanchorState == QLatin1String("needs_attention")) {
            state = ReviewBrowserState::NeedsAttention;
        } else if (card.status() == QLatin1String("needs_reanchor")
                   || reanchorState == QLatin1String("queued")
                   || reanchorState == QLatin1String("retry")
                   || reanchorState == QLatin1String("running")
                   || reanchorState == QLatin1String("updating")) {
            state = ReviewBrowserState::Repairing;
        } else if (!Thoughts::isHtmlAttachmentCard(card.record) && m_projectionProvider && canonicalHashes.value(card.pdfStem()) != card.pdfSha256()) {
            state = ReviewBrowserState::Stale;
        } else if (card.schedule.retained) {
            state = ReviewBrowserState::Retained;
        } else if (excluded) {
            state = ReviewBrowserState::Excluded;
        } else if (card.skippedUntil > now.toUTC()) {
            state = ReviewBrowserState::Skipped;
        } else if (!card.schedule.reviewed) {
            state = ReviewBrowserState::New;
        } else if (card.schedule.fixedInterval && card.schedule.dueAt > now.toUTC()) {
            state = ReviewBrowserState::Future;
        } else if (card.schedule.intervalDays == 0) {
            state = ReviewBrowserState::Learning;
        } else if (card.schedule.dueAt > now.toUTC()) {
            state = ReviewBrowserState::Future;
        } else {
            state = ReviewBrowserState::Review;
        }
        // A retired card, or one of an unpublished document, is not reviewed, so its placement
        // keeps nothing from review.
        const bool issues = state != ReviewBrowserState::Retired && state != ReviewBrowserState::Unpublished
            && hasReviewIssue(card, !Thoughts::isHtmlAttachmentCard(card.record) && m_projectionProvider
                && canonicalHashes.value(card.pdfStem()) != card.pdfSha256());
        output.append({card, state, issues});
    }
    return output;
}

bool ReviewStore::isExcluded(const ReviewCard &card) const
{
    for (const QJsonObject &exclusion : exclusions()) {
        const QString pageVisualSha256 = objectString(exclusion, QStringLiteral("page_visual_sha256"));
        const QString pdfStem = objectString(exclusion, QStringLiteral("pdf_stem"));
        const int page = exclusion.value(QStringLiteral("page")).toInt();
        if ((!pageVisualSha256.isEmpty() && pageVisualSha256 == card.pageVisualSha256()) || (!pdfStem.isEmpty() && pdfStem == card.pdfStem() && page == card.page())) {
            return true;
        }
    }
    return false;
}

bool ReviewStore::isDueNow(const ReviewCard &card, const QDateTime &now)
{
    const QDateTime utcNow = now.toUTC();
    const QDateTime learnAheadLimit = utcNow.addSecs(learnAheadLimitMinutes * 60);
    if (card.schedule.retired || card.skippedUntil > utcNow) {
        return false;
    }
    return (!card.schedule.reviewed && (!card.schedule.dueAt.isValid() || card.schedule.dueAt <= utcNow))
        || (card.schedule.reviewed && card.schedule.dueAt <= utcNow)
        || (card.schedule.reviewed && card.schedule.intervalDays == 0 && !card.schedule.fixedInterval && card.schedule.dueAt <= learnAheadLimit);
}

QDateTime ReviewStore::dueFrom(const ReviewCard &card)
{
    // isDueNow() as an instant: its conditions only wait for now to pass a time.
    if (card.schedule.retired) return {};
    const QDateTime always = QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC);
    const QDateTime due = card.schedule.dueAt.toUTC();
    QDateTime from = !due.isValid() ? always
        // A learning step is taken this much early.
        : card.schedule.reviewed && card.schedule.intervalDays == 0 && !card.schedule.fixedInterval ? due.addSecs(-learnAheadLimitMinutes * 60)
        : due;
    // A skip hides the card until then.
    if (card.skippedUntil.isValid() && card.skippedUntil.toUTC() > from) from = card.skippedUntil.toUTC();
    return from;
}

QList<ReviewCard> ReviewStore::dueCards(const QDateTime &now, const QString &pdfStemFilter, QStringList *errors) const
{
    return dueCardsOf(cards(errors, pdfStemFilter), now, pdfStemFilter, errors);
}

QList<ReviewCard> ReviewStore::dueCardsOf(const QList<ReviewCard> &cards, const QDateTime &now, const QString &pdfStemFilter, QStringList *errors) const
{
    QList<ReviewCard> output;
    const QDateTime utcNow = now.toUTC();
    for (const ReviewCard &card : activeOf(cards, errors)) {
        if (!pdfStemFilter.isEmpty() && card.pdfStem() != pdfStemFilter) {
            continue;
        }
        if (isDueNow(card, utcNow)) {
            output.append(card);
        }
    }
    // Oldest due first; a skip hides a card but keeps its due date, so it returns to its place.
    // Never-answered cards follow the due ones, and learning steps pulled in early come last.
    // Same order as the phone (ReviewStore.dueCards).
    const QString shuffleSeed = utcNow.date().toString(Qt::ISODate);
    std::sort(output.begin(), output.end(), [&shuffleSeed, &utcNow](const ReviewCard &left, const ReviewCard &right) {
        const auto queueGroup = [&utcNow](const ReviewCard &card) {
            if (!card.schedule.reviewed) {
                return 1;
            }
            return card.schedule.dueAt <= utcNow ? 0 : 2;
        };
        const auto queueDate = [](const ReviewCard &card) {
            if (card.schedule.dueAt.isValid()) {
                return card.schedule.dueAt;
            }
            const QDateTime created = parseUtc(objectString(card.record, QStringLiteral("created_at")));
            return created.isValid() ? created : QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC);
        };
        const int leftGroup = queueGroup(left);
        const int rightGroup = queueGroup(right);
        if (leftGroup != rightGroup) {
            return leftGroup < rightGroup;
        }
        const QDateTime leftDate = queueDate(left);
        const QDateTime rightDate = queueDate(right);
        if (leftDate != rightDate) {
            return leftDate < rightDate;
        }
        return sha256((shuffleSeed + left.id()).toUtf8()) < sha256((shuffleSeed + right.id()).toUtf8());
    });
    return output;
}

QJsonObject ReviewStore::cardRecord(const QString &cardId, QString *error) const
{
    for (const QJsonObject &record : records(QStringLiteral("review_card"), QStringLiteral("cards"), QStringLiteral("card_id"))) {
        if (objectString(record, QStringLiteral("card_id")) == cardId) {
            return record;
        }
    }
    if (error) {
        *error = QStringLiteral("The review card is unavailable in the current server projection.");
    }
    return {};
}

QJsonObject ReviewStore::prepareCardUpdate(QJsonObject card) const
{
    card.remove(QStringLiteral("source_change"));
    card.insert(QStringLiteral("revision"), qMax<qint64>(1, card.value(QStringLiteral("revision")).toInteger() + 1));
    card.insert(QStringLiteral("updated_at"), utcString(QDateTime::currentDateTimeUtc()));
    card.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
    return card;
}

bool ReviewStore::commitSourceChange(const QString &path, const QByteArray &before, const QByteArray &after,
                                    const QString &relativePath, QJsonObject &object, QString *error, const QString &baseSha256,
                                    const QString &transactionId) const
{
    const auto fail = [&](const QString &reason) { if (error) *error = reason; return false; };
    const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (root.isEmpty() || !canonical.startsWith(root + QLatin1Char('/')) || before.size() > 4 * 1024 * 1024 || after.size() > 4 * 1024 * 1024)
        return fail(QStringLiteral("The review source is unavailable or too large."));
    const QString journalRoot = QDir(rootPath()).filePath(QStringLiteral("source-transactions"));
    const QString objectRoot = QDir(rootPath()).filePath(QStringLiteral("source-objects"));
    if (!QDir().mkpath(journalRoot) || !QDir().mkpath(objectRoot)) return fail(QStringLiteral("Cannot prepare review source recovery."));
    QLockFile lock(QDir(journalRoot).filePath(QStringLiteral("writer.lock")));
    if (!lock.tryLock(2000)) return fail(QStringLiteral("Another review source edit is in progress."));
    QFile current(canonical);
    if (!current.open(QIODevice::ReadOnly)) return fail(QStringLiteral("The source is unavailable."));
    const auto currentBytes = current.readAll(); current.close();
    if (currentBytes != before && currentBytes != after) return fail(QStringLiteral("The source changed before the review edit."));
    const auto digest = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    for (const auto &bytes : {before, after}) {
        QSaveFile file(QDir(objectRoot).filePath(digest(bytes)));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return fail(QStringLiteral("Cannot retain the verified source objects."));
    }
    object.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
    const QString id = object.value(object.contains(QStringLiteral("event_id")) ? QStringLiteral("event_id") : QStringLiteral("card_id")).toString();
    const QString journalPath = QDir(journalRoot).filePath(id + QStringLiteral(".json"));
    // An earlier change of this record still awaiting recovery must not be overwritten.
    if (!transactionId.isEmpty() && QFileInfo::exists(journalPath))
        return fail(QStringLiteral("An earlier source edit of this card is still awaiting recovery. Restart Thoughts and try again."));
    object.insert(QStringLiteral("source_change"), QJsonObject{{QStringLiteral("transaction_id"), transactionId.isEmpty() ? QJsonValue(id) : QJsonValue(transactionId)},
        {QStringLiteral("path"), QDir(root).relativeFilePath(canonical)},
        {QStringLiteral("before_sha256"), digest(before)}, {QStringLiteral("after_sha256"), digest(after)},
        {QStringLiteral("base_sha256"), baseSha256.isEmpty() ? digest(before) : baseSha256}});
    const QJsonObject journal{{QStringLiteral("path"), canonical}, {QStringLiteral("before"), QString::fromLatin1(before.toBase64())},
        {QStringLiteral("after"), QString::fromLatin1(after.toBase64())}, {QStringLiteral("record"), object}, {QStringLiteral("relative_path"), relativePath}};
    if (!writeJson(journalPath, journal, error)) return false;
    if (!Thoughts::Platform::replaceFileContents(canonical, after)) return fail(QStringLiteral("The review source edit is saved for recovery."));
    if (!writeServerJson(relativePath, object, error)) return false;
    QFile::remove(journalPath);
    return true;
}

bool ReviewStore::writeCard(const QJsonObject &card, QString *error) const
{
    const QString id = objectString(card, QStringLiteral("card_id"));
    if (QUuid(id).isNull()) {
        if (error) {
            *error = QStringLiteral("card_id must be a UUID");
        }
        return false;
    }
    return writeServerJson(QStringLiteral("cards/%1.json").arg(id), card, error);
}

int ReviewStore::retryPendingMarkerRemovals(QStringList *errors) const
{
    if (m_readOnly) return 0;
    int removed = 0;
    for (const ReviewCard &card : cards(errors)) {
        if (card.record.value(QStringLiteral("marker_removal")).toString() != QLatin1String("pending")) continue;
        const auto segments = primaryObject(card.record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
        QString path;
        QByteArray before, after;
        if (!cardMarkerRemoval(m_docsRoot, card.id(), segments, &path, &before, &after).isEmpty()) continue;
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly) || current.readAll() != before) continue;
        current.close();
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(after) != after.size() || !output.commit()) {
            if (errors) errors->append(QStringLiteral("Could not remove the markers of deleted card %1: %2").arg(card.id(), output.errorString()));
            continue;
        }
        ++removed;
    }
    return removed;
}

bool ReviewStore::writeServerJson(const QString &relativePath, const QJsonObject &object, QString *error) const
{
    if (m_readOnly) {
        if (error) *error = QStringLiteral("Cannot write through a review snapshot.");
        return false;
    }
    if (relativePath.startsWith(QLatin1String("events/")) && object.value(QStringLiteral("rating")).toString() == QLatin1String("delete")
        && object.value(QStringLiteral("source_authority")).toString() != QLatin1String("client-v1")) {
        QFile priorFile(QDir(rootPath()).filePath(relativePath));
        if (priorFile.open(QIODevice::ReadOnly)) {
            const auto prior = QJsonDocument::fromJson(priorFile.readAll()).object();
            // Windows cannot replace a file that is still open.
            priorFile.close();
            if (prior.value(QStringLiteral("event_id")) == object.value(QStringLiteral("event_id"))
                && prior.value(QStringLiteral("card_id")) == object.value(QStringLiteral("card_id"))
                && prior.value(QStringLiteral("rating")) == object.value(QStringLiteral("rating"))
                && prior.value(QStringLiteral("source_authority")).toString() == QLatin1String("client-v1"))
                return writeServerJson(relativePath, prior, error);
        }
        auto event = object;
        event.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
        const QString cardId = object.value(QStringLiteral("card_id")).toString();
        const auto anchor = object.contains(QStringLiteral("source_anchor")) ? object.value(QStringLiteral("source_anchor")).toObject()
            : primaryObject(ReviewStore(m_docsRoot, m_projectionProvider, {}, true).cardRecord(cardId)).value(QStringLiteral("source_anchor")).toObject();
        const auto segments = anchor.value(QStringLiteral("segments")).toArray();
        event.insert(QStringLiteral("marker_removal"), segments.isEmpty() ? QStringLiteral("not_required") : QStringLiteral("pending"));
        if (!segments.isEmpty()) {
            QString path;
            QByteArray before, after;
            const QString reason = cardMarkerRemoval(m_docsRoot, cardId, segments, &path, &before, &after);
            if (reason.isEmpty()) {
                event.insert(QStringLiteral("marker_removal"), QStringLiteral("complete"));
                const QString base = segments.first().toObject().value(QStringLiteral("source_sha256")).toString();
                if (commitSourceChange(path, before, after, relativePath, event, error, base)) return true;
                // Once a journal exists, recovery owns both halves. Before that point,
                // an unsafe source edit still records a visible pending deletion.
                if (QFileInfo::exists(QDir(rootPath()).filePath(QStringLiteral("source-transactions/%1.json").arg(event.value(QStringLiteral("event_id")).toString())))) return false;
                event.remove(QStringLiteral("source_change"));
                event.insert(QStringLiteral("marker_removal"), QStringLiteral("pending"));
                if (error) { event.insert(QStringLiteral("marker_removal_reason"), *error); error->clear(); }
            } else {
                event.insert(QStringLiteral("marker_removal_reason"), reason);
            }
        }
        return writeServerJson(relativePath, event, error);
    }
    if (!writeJson(QDir(rootPath()).filePath(relativePath), object, error)) {
        return false;
    }
    // Alternate/test roots have no Fedora authority unless their caller
    // explicitly supplied an isolated synchronization state directory.
    if (!m_outbox) {
        return true;
    }
    QString entityType;
    QString entityId;
    QString collection;
    if (relativePath.startsWith(QLatin1String("cards/"))) {
        entityType = QStringLiteral("review_card");
        entityId = objectString(object, QStringLiteral("card_id"));
        collection = QStringLiteral("cards");
    } else if (relativePath.startsWith(QLatin1String("events/"))) {
        entityType = QStringLiteral("review_event");
        entityId = objectString(object, QStringLiteral("event_id"));
        collection = QStringLiteral("events");
    } else if (relativePath.startsWith(ReviewTags::mergeCollection() + QLatin1Char('/'))) {
        entityType = ReviewTags::mergeEntityType();
        entityId = objectString(object, QStringLiteral("merge_id"));
        collection = ReviewTags::mergeCollection();
    } else if (relativePath.startsWith(groupsFolder() + QLatin1Char('/'))) {
        entityType = ReviewTags::groupsEntityType();
        entityId = objectString(object, QStringLiteral("layout_id"));
        collection = ReviewTags::mergeCollection();
    } else if (relativePath.startsWith(ReviewCardFeedback::collection() + QLatin1Char('/'))) {
        // Never let the final branch turn answer feedback into a page exclusion.
        QString problem = ReviewCardFeedback::validationError(object);
        if (!problem.isEmpty()) {
            if (error) *error = problem;
            return false;
        }
        entityType = ReviewCardFeedback::entityType();
        entityId = objectString(object, QStringLiteral("feedback_id"));
        collection = ReviewCardFeedback::collection();
    } else if (relativePath.startsWith(DocumentFeedback::collection() + QLatin1Char('/'))) {
        // Document feedback is never a page exclusion either.
        QString problem = DocumentFeedback::validationError(object);
        if (!problem.isEmpty()) {
            if (error) *error = problem;
            return false;
        }
        entityType = DocumentFeedback::entityType();
        entityId = objectString(object, QStringLiteral("feedback_id"));
        collection = DocumentFeedback::collection();
    } else {
        entityType = QStringLiteral("review_exclusion");
        entityId = objectString(object, QStringLiteral("exclusion_id"));
        collection = QStringLiteral("exclusions");
    }
    const qint64 revision = object.value(QStringLiteral("revision")).toInteger(1);
    const QString timestamp = objectString(object, QStringLiteral("updated_at")).isEmpty()
        ? (objectString(object, QStringLiteral("reviewed_at")).isEmpty()
               ? objectString(object, QStringLiteral("created_at"))
               : objectString(object, QStringLiteral("reviewed_at")))
        : objectString(object, QStringLiteral("updated_at"));
    QString queueError;
    const bool queueReady = m_outbox->ready(&queueError);
    if (entityId.isEmpty() || !queueReady || m_outbox->enqueue(entityType, entityId, collection, revision - 1, object, timestamp).isEmpty()) {
        if (error) {
            *error = queueError.isEmpty()
                ? QStringLiteral("Review was saved locally but could not be queued for the server.")
                : QStringLiteral("Review was saved locally but could not be queued for the server: %1").arg(queueError);
        }
        return false;
    }
    return true;
}

ReviewImportResult ReviewStore::importAgentOutput(const QByteArray &output, const QVariantMap &document, int maxCandidates)
{
    ReviewImportResult result;
    if (maxCandidates < 1) {
        result.errors.append(QStringLiteral("The maximum candidate count must be positive."));
        return result;
    }
    const QString expectedPdfStem = document.value(QStringLiteral("pdf_stem")).toString();
    const QString expectedPdfSha256 = document.value(QStringLiteral("pdf_sha256")).toString();
    const int expectedPageCount = document.value(QStringLiteral("page_count")).toInt();
    if (expectedPdfStem.isEmpty() || !validSha256(expectedPdfSha256) || expectedPageCount < 1) {
        result.errors.append(QStringLiteral("The current PDF metadata is incomplete."));
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument agentDocument = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !agentDocument.isObject()) {
        result.errors.append(QStringLiteral("Agent output is not valid JSON: %1").arg(parseError.errorString()));
        return result;
    }
    const QJsonObject root = agentDocument.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != schemaVersion || !root.value(QStringLiteral("candidates")).isArray()) {
        result.errors.append(QStringLiteral("Agent output has an unsupported schema."));
        return result;
    }
    const QJsonArray candidates = root.value(QStringLiteral("candidates")).toArray();
    if (candidates.size() > maxCandidates) {
        result.errors.append(QStringLiteral("Agent output contains more than the requested %1 candidates.").arg(maxCandidates));
        return result;
    }

    QList<ReviewCard> existingCards = cards();
    for (const QJsonValue &candidateValue : candidates) {
        const QJsonObject candidate = candidateValue.toObject();
        const auto candidateAnchor = candidate.value(QStringLiteral("source_anchor")).toObject();
        if (!candidateAnchor.isEmpty() && candidateAnchor.value(QStringLiteral("segments")).toArray().size() != 1) {
            result.errors.append(QStringLiteral("A new card must select one continuous source range.")); continue;
        }
        if (m_projectionProvider && candidate.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("version")).toInt() != 2) {
            result.errors.append(QStringLiteral("The server returned a card without a source anchor. Update the server before generating cards."));
            continue;
        }
        const int page = candidate.value(QStringLiteral("page")).toInt();
        const QJsonArray rect = candidate.value(QStringLiteral("rect")).toArray();
        const QString question = objectString(candidate, QStringLiteral("question")).trimmed();
        const QString pageVisualSha256 = objectString(candidate, QStringLiteral("page_visual_sha256"));
        if (page < 1 || page > expectedPageCount || !validRect(rect) || !validSha256(pageVisualSha256) || !validQuestion(question)) {
            result.errors.append(QStringLiteral("Rejected an invalid visual region on PDF page %1.").arg(page));
            continue;
        }
        const QString generatedId = objectString(candidate, QStringLiteral("card_id"));
        if (!generatedId.isEmpty() && QUuid(generatedId).isNull()) {
            result.errors.append(QStringLiteral("Rejected a candidate with an invalid card ID."));
            continue;
        }
        bool duplicate = false;
        for (const ReviewCard &existing : std::as_const(existingCards)) {
            if ((!generatedId.isEmpty() && existing.id() == generatedId)
                || (!candidate.value(QStringLiteral("duplicate_check")).isObject()
                    && existing.status() != QLatin1String("rejected") && !existing.schedule.deleted
                    && existing.pdfStem() == expectedPdfStem && existing.pageVisualSha256() == pageVisualSha256
                    && intersectionOverUnion(primaryObject(existing.record).value(QStringLiteral("rect")).toArray(), rect) >= 0.5)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            result.errors.append(QStringLiteral("Skipped an overlapping visual region on PDF page %1.").arg(page));
            continue;
        }

        QJsonObject normalizedPrimary;
        normalizedPrimary.insert(QStringLiteral("document_id"), DocumentIdentityStore(m_docsRoot).byFilename(expectedPdfStem + QStringLiteral(".pdf")).value(QStringLiteral("document_id")));
        normalizedPrimary.insert(QStringLiteral("pdf_stem"), expectedPdfStem);
        normalizedPrimary.insert(QStringLiteral("pdf_sha256"), expectedPdfSha256);
        normalizedPrimary.insert(QStringLiteral("page"), page);
        normalizedPrimary.insert(QStringLiteral("rect"), rect);
        normalizedPrimary.insert(QStringLiteral("question"), question);
        normalizedPrimary.insert(QStringLiteral("page_visual_sha256"), pageVisualSha256);
        if (candidate.value(QStringLiteral("source_anchor")).isObject()) normalizedPrimary.insert(QStringLiteral("source_anchor"), candidate.value(QStringLiteral("source_anchor")));
        QJsonObject card;
        card.insert(QStringLiteral("schema_version"), schemaVersion);
        card.insert(QStringLiteral("card_type"), QStringLiteral("visual_region"));
        card.insert(QStringLiteral("card_id"), generatedId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : generatedId);
        card.insert(QStringLiteral("status"), QStringLiteral("approved"));
        card.insert(QStringLiteral("approved_at"), utcString(QDateTime::currentDateTimeUtc()));
        card.insert(QStringLiteral("created_at"), utcString(QDateTime::currentDateTimeUtc()));
        card.insert(QStringLiteral("updated_at"), objectString(card, QStringLiteral("created_at")));
        card.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
        card.insert(QStringLiteral("revision"), 1);
        card.insert(QStringLiteral("primary"), normalizedPrimary);
        if (candidate.value(QStringLiteral("reanchor")).isObject()) card.insert(QStringLiteral("reanchor"), candidate.value(QStringLiteral("reanchor")));
        if (candidate.value(QStringLiteral("duplicate_check")).isObject()) {
            card.insert(QStringLiteral("duplicate_check"), candidate.value(QStringLiteral("duplicate_check")));
        }
        if (!objectString(candidate, QStringLiteral("note")).isEmpty()) {
            card.insert(QStringLiteral("note"), objectString(candidate, QStringLiteral("note")));
        }
        QString error;
        if (!writeCard(card, &error)) {
            result.errors.append(error);
            continue;
        }
        const ReviewCard importedCard{card, {}};
        result.cards.append(importedCard);
        existingCards.append(importedCard);
    }
    return result;
}

ReviewImportResult ReviewStore::createBasicCards(const QVariantMap &document, int *skipped)
{
    ReviewImportResult result;
    if (skipped) *skipped = 0;
    const QString path = document.value(QStringLiteral("pdf_path")).toString();
    const QString stem = document.value(QStringLiteral("pdf_stem")).toString();
    const QString sha = document.value(QStringLiteral("pdf_sha256")).toString();
    const auto fingerprints = document.value(QStringLiteral("basic_page_fingerprints")).toList();
    QFile file(path);
    if (stem.isEmpty() || QFileInfo(path).completeBaseName() != stem || !validSha256(sha)
        || fingerprints.isEmpty() || fingerprints.size() != document.value(QStringLiteral("page_count")).toInt()
        || !file.open(QIODevice::ReadOnly)
        || QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()) != sha) {
        result.errors.append(QStringLiteral("The PDF changed or its page information is incomplete. Generate again."));
        return result;
    }
    for (const auto &fingerprint : fingerprints) {
        if (!validSha256(fingerprint.toString())) {
            result.errors.append(QStringLiteral("A page fingerprint is invalid."));
            return result;
        }
    }
    const auto existing = cards();
    const QString timestamp = utcString(QDateTime::currentDateTimeUtc());
    const auto candidates = QJsonArray::fromVariantList(document.value(QStringLiteral("basic_candidates")).toList());
    if (candidates.isEmpty()) { result.errors.append(QStringLiteral("No complete passages are available.")); return result; }
    QSet<QString> coveredPassages;
    const auto passageId = [](const QJsonObject &span) { return span.value(QStringLiteral("source_file")).toString()
        + QLatin1Char('\n') + span.value(QStringLiteral("content_sha256")).toString(); };
    for (const auto &card : existing) {
        if (card.pdfStem() != stem || card.schedule.deleted || !isBasicReviewCard(card.record)) continue;
        for (const auto &span : primaryObject(card.record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray())
            coveredPassages.insert(passageId(span.toObject()));
    }
    for (const auto &value : candidates) {
        auto candidate = value.toObject();
        auto sourceAnchor = candidate.value(QStringLiteral("source_anchor")).toObject();
        const auto spans = sourceAnchor.value(QStringLiteral("segments")).toArray();
        if (!sourceAnchor.isEmpty() && spans.size() != 1) {
            result.errors.append(QStringLiteral("Generate again after Sync: each card must select one continuous source passage.")); continue;
        }
        if (!spans.isEmpty()) {
            QJsonArray remaining, remainingRegions;
            for (const auto &span : spans) {
                if (coveredPassages.contains(passageId(span.toObject()))) continue;
                remaining.append(span);
                for (const auto &r : span.toObject().value(QStringLiteral("regions")).toArray()) remainingRegions.append(r);
            }
            if (remaining.isEmpty()) { if (skipped) ++*skipped; continue; }
            if (remaining.size() != spans.size() && !remainingRegions.isEmpty()) {
                sourceAnchor.insert(QStringLiteral("segments"),remaining);
                candidate.insert(QStringLiteral("source_anchor"),sourceAnchor);
                candidate.insert(QStringLiteral("regions"),remainingRegions);
                QByteArray identity("basic-source-subset-v1\n");
                for (const auto &span : remaining) identity += passageId(span.toObject()).toUtf8() + '\n';
                candidate.insert(QStringLiteral("key"),QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex()));
            }
        }
        const auto regions = candidate.value(QStringLiteral("regions")).toArray();
        const auto key = candidate.value(QStringLiteral("key")).toString();
        bool valid = validSha256(key) && !regions.isEmpty();
        for (const auto &r : regions) {
            const auto region = r.toObject(); const int page = region.value(QStringLiteral("page")).toInt();
            valid = valid && page > 0 && page <= fingerprints.size() && validNormalizedReviewCoordinates(region.value(QStringLiteral("rect")).toArray(), 4);
        }
        if (!valid) { result.errors.append(QStringLiteral("The passage plan has invalid locations.")); continue; }
        if (std::any_of(existing.cbegin(), existing.cend(), [&](const ReviewCard &card) {
            return card.pdfStem() == stem && isBasicReviewCard(card.record)
                && card.record.value(QStringLiteral("basic_passage_key")).toString() == key;
        })) { if (skipped) ++*skipped; continue; }
        const auto first = regions.first().toObject();
        const int page = first.value(QStringLiteral("page")).toInt();
        QJsonObject primary {
            {QStringLiteral("document_id"), DocumentIdentityStore(m_docsRoot).byFilename(stem + QStringLiteral(".pdf")).value(QStringLiteral("document_id"))},
            {QStringLiteral("pdf_stem"), stem}, {QStringLiteral("pdf_sha256"), sha},
            {QStringLiteral("page"), page}, {QStringLiteral("rect"), first.value(QStringLiteral("rect"))},
            {QStringLiteral("regions"), regions}, {QStringLiteral("question"), QStringLiteral("Recall this section.")},
            {QStringLiteral("page_visual_sha256"), fingerprints[page-1].toString()},
            {QStringLiteral("page_visual_fingerprint_method"), QString::fromLatin1(reviewFingerprintMethod)},
        };
        const auto anchor = candidate.value(QStringLiteral("source_anchor")).toObject();
        if (!anchor.isEmpty()) primary.insert(QStringLiteral("source_selection"), anchor);
        else {
            primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
            const QString pdfPath = document.value(QStringLiteral("pdf_path")).toString();
            const QString bundleRoot = document.value(QStringLiteral("source_bundle_root"),
                QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"))).toString();
            const auto origin = reviewPdfComponentOrigin(bundleRoot, pdfPath, sha, page);
            if (!origin.isEmpty()) primary.insert(QStringLiteral("component_origin"), origin);
        }
        const QJsonObject card {
            {QStringLiteral("schema_version"), 4}, {QStringLiteral("card_type"), QStringLiteral("visual_region")},
            {QStringLiteral("card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("generation_method"), anchor.isEmpty() ? QStringLiteral("basic_passages_v1") : QStringLiteral("basic_passages_v2")}, {QStringLiteral("basic_passage_key"), key},
            {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1},
            {QStringLiteral("created_at"), timestamp}, {QStringLiteral("updated_at"), timestamp},
            {QStringLiteral("approved_at"), timestamp}, {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
            {QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), anchor.isEmpty() ? QStringLiteral("succeeded") : QStringLiteral("pending")},
                {QStringLiteral("strategy"), anchor.isEmpty() ? QStringLiteral("basic_pdf_snapshot_v1") : QStringLiteral("source_markers_v2")},
                {QStringLiteral("target_pdf_sha256"), sha}}},
            {QStringLiteral("primary"), primary},
        };
        QString error;
        if (!writeCard(card, &error)) { result.errors.append(error); return result; }
        result.cards.append({card, {}});
    }
    return result;
}

// The one Library document whose published sources include this source, and
// the source's last published version (the merge base of a card's source edit).
static QString sourceDocumentOwner(const QString &root, const QString &relative, const QString &bundleRoot, QJsonObject *owner, QString *base)
{
    *owner = QJsonObject(); base->clear();
    const auto index = readObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")), nullptr);
    for (const auto &value : index.value(QStringLiteral("pdfs")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("project")).toString() != SourceOwner::of(relative)) continue;
        const QString artifactHash = entry.value(QStringLiteral("source_artifact_sha256")).toString();
        if (!validSha256(artifactHash)) continue;
        QFile file(QDir(bundleRoot).filePath(QStringLiteral("objects/") + artifactHash));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto bytes = file.readAll(); if (sha256(bytes) != artifactHash) continue;
        const auto artifact = QJsonDocument::fromJson(bytes).object();
        if (artifact.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")) != entry.value(QStringLiteral("pdf_sha256"))) continue;
        for (const auto &input : artifact.value(QStringLiteral("sources")).toArray()) {
            if (input.toObject().value(QStringLiteral("path")).toString() != relative) continue;
            const auto identity = DocumentIdentityStore(root).byFilename(entry.value(QStringLiteral("filename")).toString());
            if (!DocumentIdentityStore::validId(identity.value(QStringLiteral("document_id")).toString())) continue;
            if (!owner->isEmpty()) return QStringLiteral("ambiguous_document");
            *owner = identity; *base = input.toObject().value(QStringLiteral("sha256")).toString();
        }
    }
    return owner->isEmpty() || !validSha256(*base) ? QStringLiteral("unregistered_source") : QString();
}

QJsonObject ReviewStore::sourceCardCommand(const QJsonObject &request, const QString &bundleRoot)
{
    const auto failure = [](const QString &code, const QString &message) {
        return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), false},
            {QStringLiteral("terminal"), true}, {QStringLiteral("error_code"), code}, {QStringLiteral("message"), message}};
    };
    const QString command = request.value(QStringLiteral("command")).toString();
    if (command == QLatin1String("delete-review-card") || command == QLatin1String("set-review-card-interval")) {
        const bool deleting = command == QLatin1String("delete-review-card");
        const QSet<QString> allowed{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("card_id"),
            QStringLiteral("request_id"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"),
            QStringLiteral("interval_unit"), QStringLiteral("interval_amount")};
        for (auto it = request.begin(); it != request.end(); ++it)
            if (!allowed.contains(it.key()) || (deleting && it.key().startsWith(QLatin1String("interval_"))))
                return failure(QStringLiteral("invalid_request"), QStringLiteral("Unexpected field: %1").arg(it.key()));
        const QString id = request.value(QStringLiteral("card_id")).toString();
        const QString requestId = request.value(QStringLiteral("request_id")).toString();
        if (m_readOnly || QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id || !DocumentIdentityStore::validId(requestId))
            return failure(QStringLiteral("invalid_request"), QStringLiteral("Provide valid card and request UUIDs."));
        if (!QDir().mkpath(rootPath())) return failure(QStringLiteral("write_failed"), QStringLiteral("Cannot prepare local reviews."));
        QLockFile lock(QDir(rootPath()).filePath(QStringLiteral("card-commands.lock")));
        if (!lock.tryLock(2000)) return failure(QStringLiteral("busy"), QStringLiteral("Another card action is in progress."));
        const QString hash = sha256(QJsonDocument(request).toJson(QJsonDocument::Compact));
        const QString journalPath = QDir(rootPath()).filePath(QStringLiteral("card-commands/%1.json").arg(requestId));
        auto journal = readObject(journalPath, nullptr);
        const bool replay = !journal.isEmpty();
        if (replay && journal.value(QStringLiteral("sha256")).toString() != hash)
            return failure(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
        QString error;
        if (!replay) {
            const auto snapshot = cards();
            const auto card = std::find_if(snapshot.cbegin(), snapshot.cend(), [&](const ReviewCard &c) { return c.id() == id; });
            if (card == snapshot.cend() || card->schedule.deleted || (!deleting && !card->isApproved()))
                return failure(QStringLiteral("card_unavailable"), QStringLiteral("This card is unavailable or already deleted."));
            if (request.contains(QStringLiteral("source")) || request.contains(QStringLiteral("expected_source_sha256"))) {
                const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
                const QString path = QFileInfo(QDir(root).absoluteFilePath(request.value(QStringLiteral("source")).toString())).canonicalFilePath();
                const QString relative = QDir(root).relativeFilePath(path);
                QFile source(path);
                const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
                if (path.isEmpty() || !path.startsWith(root + QLatin1Char('/')) || SourceOwner::of(relative).isEmpty()
                    || !validSha256(expected) || !source.open(QIODevice::ReadOnly))
                    return failure(QStringLiteral("invalid_source"), QStringLiteral("Choose a saved Thoughts source with its current SHA-256."));
                const auto bytes = source.readAll();
                if (sha256(bytes) != expected)
                    return failure(QStringLiteral("source_conflict"), QStringLiteral("The source changed. Reopen the card controls and try again."));
                if (const auto anchor = readReviewSourceAnchor(relative, bytes, id); !anchor.error.isEmpty())
                    return failure(QStringLiteral("invalid_source"), QStringLiteral("The card's markers in this source cannot be read: %1").arg(anchor.error));
            }
            auto now = QDateTime::currentDateTimeUtc();
            for (const auto &prior : card->history) {
                const auto at = parseUtc(objectString(prior, QStringLiteral("reviewed_at")));
                if (at.isValid() && at >= now) now = at.addMSecs(1);
            }
            QJsonObject event{{QStringLiteral("schema_version"), schemaVersion}, {QStringLiteral("event_id"), requestId},
                {QStringLiteral("card_id"), id}, {QStringLiteral("reviewed_at"), now.toString(Qt::ISODateWithMs)}, {QStringLiteral("updated_at"), now.toString(Qt::ISODateWithMs)},
                {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1}};
            QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                {QStringLiteral("card_id"), id}, {QStringLiteral("event_id"), requestId},
                {QStringLiteral("state"), deleting ? QStringLiteral("deleted") : QStringLiteral("interval_set")}};
            if (deleting) {
                event.insert(QStringLiteral("rating"), QStringLiteral("delete"));
                event.insert(QStringLiteral("scheduler_version"), currentSchedulerVersion);
                event.insert(QStringLiteral("source_anchor"), primaryObject(card->record).value(QStringLiteral("source_anchor")));
            } else {
                const QString unit = request.value(QStringLiteral("interval_unit")).toString();
                const QStringList units{QStringLiteral("minutes"), QStringLiteral("hours"), QStringLiteral("days"), QStringLiteral("weeks"), QStringLiteral("months")};
                const bool never = unit == QLatin1String("never");
                ReviewInterval interval;
                if ((!never && (!units.contains(unit) || !request.value(QStringLiteral("interval_amount")).isDouble()))
                    || (never && request.contains(QStringLiteral("interval_amount"))))
                    return failure(QStringLiteral("invalid_request"), QStringLiteral("Choose a positive interval or Never."));
                interval.unit = never ? ReviewInterval::Days : static_cast<ReviewInterval::Unit>(units.indexOf(unit));
                interval.amount = request.value(QStringLiteral("interval_amount")).toDouble(1);
                const auto due = never ? QDateTime{} : interval.dueAt(now);
                if (!never && !due.isValid()) return failure(QStringLiteral("invalid_request"), QStringLiteral("Choose a valid future interval."));
                event.insert(QStringLiteral("interval_action"), QStringLiteral("set"));
                event.insert(QStringLiteral("interval_unit"), unit);
                if (!never) {
                    event.insert(QStringLiteral("interval_amount"), interval.amount);
                    event.insert(QStringLiteral("scheduled_due_at"), utcString(due));
                }
                result.insert(QStringLiteral("due_at"), utcString(due));
                result.insert(QStringLiteral("interval_unit"), unit);
                if (const QJsonObject version = reviewVersion(card->record); !version.isEmpty()) event.insert(reviewVersionField(), version);
            }
            // Persist exact event bytes before changing either the source or history.
            // An interrupted command replays this event rather than inventing another.
            journal = {{QStringLiteral("sha256"), hash}, {QStringLiteral("event"), event}, {QStringLiteral("result"), result}};
            if (!writeJson(journalPath, journal, &error)) return failure(QStringLiteral("write_failed"), error);
        }
        if (!journal.value(QStringLiteral("complete")).toBool()) {
            const QString sourceJournal = QDir(rootPath()).filePath(QStringLiteral("source-transactions/%1.json").arg(requestId));
            if (QFileInfo::exists(sourceJournal)) {
                recoverSourceTransactions();
                if (QFileInfo::exists(sourceJournal))
                    return failure(QStringLiteral("recovery_pending"), QStringLiteral("The source transaction is still awaiting recovery; retry this same request."));
            }
            const QString eventPath = QStringLiteral("events/mac-okular/%1.json").arg(requestId);
            if (!writeServerJson(eventPath, journal.value(QStringLiteral("event")).toObject(), &error))
                return failure(QStringLiteral("recovery_pending"), error);
            auto result = journal.value(QStringLiteral("result")).toObject();
            if (deleting) result.insert(QStringLiteral("marker_removal"), readObject(QDir(rootPath()).filePath(eventPath), nullptr).value(QStringLiteral("marker_removal")));
            journal.insert(QStringLiteral("result"), result);
            journal.insert(QStringLiteral("complete"), true);
            if (!writeJson(journalPath, journal, &error)) return failure(QStringLiteral("recovery_pending"), error);
        }
        auto result = journal.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("replayed"), replay);
        return result;
    }
    if (command == QLatin1String("merge-review-cards")) {
        // The same merge as Merge cards…: one atomic change, no proposals to decide one by one.
        const QSet<QString> allowed{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("card_ids"),
            QStringLiteral("keep_card_id"), QStringLiteral("question"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"),
            QStringLiteral("request_id")};
        for (auto it = request.begin(); it != request.end(); ++it)
            if (!allowed.contains(it.key())) return failure(QStringLiteral("invalid_request"), QStringLiteral("Unexpected field: %1").arg(it.key()));
        const QString requestId = request.value(QStringLiteral("request_id")).toString();
        const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
        const QString keep = request.value(QStringLiteral("keep_card_id")).toString();
        QStringList ids;
        bool validIds = request.value(QStringLiteral("card_ids")).isArray();
        for (const auto &value : request.value(QStringLiteral("card_ids")).toArray()) {
            const QString cardId = value.toString();
            validIds = validIds && value.isString() && !QUuid(cardId).isNull() && QUuid(cardId).toString(QUuid::WithoutBraces) == cardId && !ids.contains(cardId);
            ids.append(cardId);
        }
        // Cards of an imported PDF have no source: their request leaves out source and its SHA-256.
        const bool pdfRequest = !request.contains(QStringLiteral("source")) && !request.contains(QStringLiteral("expected_source_sha256"));
        if (m_readOnly || !validIds || ids.size() < 2 || ids.size() > 20 || !DocumentIdentityStore::validId(requestId)
            || (!pdfRequest && (!validSha256(expected) || !request.value(QStringLiteral("source")).isString()))
            || (request.contains(QStringLiteral("keep_card_id")) && !ids.contains(keep))
            || (request.contains(QStringLiteral("question")) && !request.value(QStringLiteral("question")).isString()))
            return failure(QStringLiteral("invalid_request"), QStringLiteral("Provide 2 to 20 different card UUIDs, a keep_card_id among them if any, "
                                                                            "a request UUID and, for cards marked in a note, its source and current "
                                                                            "SHA-256 (leave both out for cards of a PDF)."));
        if (!QDir().mkpath(QDir(rootPath()).filePath(QStringLiteral("card-commands")))) return failure(QStringLiteral("write_failed"), QStringLiteral("Cannot prepare local reviews."));
        QLockFile lock(QDir(rootPath()).filePath(QStringLiteral("card-commands.lock")));
        if (!lock.tryLock(2000)) return failure(QStringLiteral("busy"), QStringLiteral("Another card action is in progress."));
        const QString hash = sha256(QJsonDocument(request).toJson(QJsonDocument::Compact));
        const QString journalPath = QDir(rootPath()).filePath(QStringLiteral("card-commands/%1.json").arg(requestId));
        if (const auto journal = readObject(journalPath, nullptr); !journal.isEmpty()) {
            if (journal.value(QStringLiteral("sha256")).toString() != hash)
                return failure(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
            auto result = journal.value(QStringLiteral("result")).toObject();
            result.insert(QStringLiteral("replayed"), true);
            return result;
        }
        QList<ReviewCard> chosen;
        for (const ReviewCard &card : cards())
            if (ids.contains(card.id()) && !card.schedule.deleted) chosen.append(card);
        if (chosen.size() != ids.size())
            return failure(QStringLiteral("card_unavailable"), QStringLiteral("A card is unavailable or already deleted."));
        QString path;
        if (!pdfRequest) {
            const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
            path = QFileInfo(QDir(root).absoluteFilePath(request.value(QStringLiteral("source")).toString())).canonicalFilePath();
            const QString relative = QDir(root).relativeFilePath(path);
            QFile source(path);
            if (path.isEmpty() || !path.startsWith(root + QLatin1Char('/')) || SourceOwner::of(relative).isEmpty() || !source.open(QIODevice::ReadOnly))
                return failure(QStringLiteral("invalid_source"), QStringLiteral("Choose the saved Thoughts source that holds the cards' markers."));
            if (sha256(source.readAll()) != expected)
                return failure(QStringLiteral("source_conflict"), QStringLiteral("The source changed; read it again and retry with its current SHA-256."));
            source.close();
            for (const ReviewCard &card : std::as_const(chosen)) {
                for (const auto &value : primaryObject(card.record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray()) {
                    if (value.toObject().value(QStringLiteral("source_file")).toString() != relative)
                        return failure(QStringLiteral("invalid_source"), QStringLiteral("Card %1 has a passage in %2, not in this source.")
                                                                             .arg(card.id(), value.toObject().value(QStringLiteral("source_file")).toString()));
                }
            }
        }
        CardMerge preview;
        if (const QString problem = cardMergeProblem(chosen, bundleRoot, &preview); !problem.isEmpty())
            return failure(QStringLiteral("merge_refused"), problem);
        if (pdfRequest && !preview.pdf)
            return failure(QStringLiteral("invalid_request"), QStringLiteral("These cards are marked in a note: give its source and expected_source_sha256."));
        if (!pdfRequest && preview.pdf)
            return failure(QStringLiteral("invalid_request"), QStringLiteral("These cards are only on the PDF: leave out source and expected_source_sha256."));
        QList<ReviewCard> ordered;
        for (const QString &cardId : std::as_const(preview.order))
            for (const ReviewCard &card : std::as_const(chosen))
                if (card.id() == cardId) ordered.append(card);
        const QString kept = keep.isEmpty() ? preview.keptId : keep;
        const QString question = request.contains(QStringLiteral("question")) ? request.value(QStringLiteral("question")).toString().trimmed()
                                                                               : joinedMergeQuestion(ordered);
        QString error;
        CardMerge merged;
        if (!mergeCards(ordered, kept, question, bundleRoot, &error, &merged)) return failure(QStringLiteral("merge_refused"), error);
        QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
            {QStringLiteral("state"), QStringLiteral("merged")}, {QStringLiteral("request_id"), requestId},
            {QStringLiteral("kept_card_id"), merged.keptId}, {QStringLiteral("deleted_card_ids"), QJsonArray::fromStringList(merged.deletedIds)},
            {QStringLiteral("delete_event_ids"), QJsonArray::fromStringList(merged.deleteEventIds)},
            {QStringLiteral("card_order"), QJsonArray::fromStringList(merged.order)}, {QStringLiteral("question"), question},
            {QStringLiteral("kind"), merged.pdf ? QStringLiteral("pdf") : QStringLiteral("source")},
            {QStringLiteral("source"), merged.pdf ? QJsonValue(QJsonValue::Null) : QJsonValue(path)},
            {QStringLiteral("source_sha256"), merged.pdf ? QJsonValue(QJsonValue::Null) : QJsonValue(merged.sourceSha256)},
            {QStringLiteral("passages"), merged.passages}, {QStringLiteral("replayed"), false},
            {QStringLiteral("message"), merged.pdf ? QStringLiteral("Merged on this computer; the next Sync carries the whole merge.")
                                                   : QStringLiteral("Merged on this computer; the next Sync carries the whole merge. PDF placement follows the next build.")}};
        if (!writeJson(journalPath, {{QStringLiteral("sha256"), hash}, {QStringLiteral("result"), result}, {QStringLiteral("complete"), true}}, &error))
            qWarning() << "The merge is done, but its request receipt could not be saved:" << error;
        return result;
    }
    if (command == QLatin1String("move-review-card")) {
        const QSet<QString> allowed{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("card_id"),
            QStringLiteral("request_id"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"),
            QStringLiteral("start_line"), QStringLiteral("end_line")};
        for (auto it = request.begin(); it != request.end(); ++it)
            if (!allowed.contains(it.key())) return failure(QStringLiteral("invalid_request"), QStringLiteral("Unexpected field: %1").arg(it.key()));
        const QString id = request.value(QStringLiteral("card_id")).toString();
        const QString requestId = request.value(QStringLiteral("request_id")).toString();
        const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
        const auto line = [&request](const QString &key) {
            const auto v = request.value(key);
            return v.isDouble() && v.toDouble() >= 1 && v.toDouble() <= 2147483647 && std::floor(v.toDouble()) == v.toDouble() ? v.toInt() : 0;
        };
        const int startLine = line(QStringLiteral("start_line")), endLine = line(QStringLiteral("end_line"));
        if (m_readOnly || QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id || !DocumentIdentityStore::validId(requestId)
            || !validSha256(expected) || startLine < 1 || endLine <= startLine)
            return failure(QStringLiteral("invalid_request"), QStringLiteral("Provide card and request UUIDs, the current source SHA-256, and one nonempty line range."));
        if (!QDir().mkpath(rootPath())) return failure(QStringLiteral("write_failed"), QStringLiteral("Cannot prepare local reviews."));
        QLockFile commandLock(QDir(rootPath()).filePath(QStringLiteral("source-creation.lock")));
        if (!commandLock.tryLock(2000)) return failure(QStringLiteral("busy"), QStringLiteral("Another card is being created or moved."));
        const QString requestHash = sha256(QJsonDocument(request).toJson(QJsonDocument::Compact));
        QString error;
        auto card = cardRecord(id, &error);
        if (card.isEmpty()) return failure(QStringLiteral("card_unavailable"), QStringLiteral("This card is unavailable or already deleted."));
        // The request UUID identifies this move, so an uncertain retry cannot move the card twice.
        const auto receipt = card.value(QStringLiteral("move_request")).toObject();
        if (receipt.value(QStringLiteral("request_id")).toString() == requestId) {
            if (receipt.value(QStringLiteral("sha256")).toString() != requestHash)
                return failure(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
            if (QFileInfo::exists(QDir(rootPath()).filePath(QStringLiteral("source-transactions/%1.json").arg(id))))
                return failure(QStringLiteral("recovery_pending"), QStringLiteral("The move is saved for recovery; retry with the identical request."));
            auto result = receipt.value(QStringLiteral("result")).toObject(); result.insert(QStringLiteral("replayed"), true); return result;
        }
        const auto snapshot = cards();
        const auto current = std::find_if(snapshot.cbegin(), snapshot.cend(), [&](const ReviewCard &c) { return c.id() == id; });
        if (current == snapshot.cend() || current->schedule.deleted || !current->isApproved())
            return failure(QStringLiteral("card_unavailable"), QStringLiteral("This card is unavailable or already deleted."));
        if (current->isWholeDocument() || isBasicReviewCard(card))
            return failure(QStringLiteral("unsupported_card"), QStringLiteral("This card's answer is not placed by areas."));
        if (!primaryObject(card).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().isEmpty())
            return failure(QStringLiteral("linked_card"), QStringLiteral("This card is linked to source markers. Move its marked passage with "
                                                                         "thoughtsctl move-card-passage (MCP move_review_card with start_line equal to end_line), "
                                                                         "which proposes the move for the user to accept, or cut and paste it by hand."));
        const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
        const QString requested = request.value(QStringLiteral("source")).toString();
        const QString path = QFileInfo(QDir(root).absoluteFilePath(requested)).canonicalFilePath();
        const QString relative = QDir(root).relativeFilePath(path);
        if (requested.isEmpty() || requested.contains(QChar::Null) || path.isEmpty() || !path.startsWith(root + QLatin1Char('/'))
            || SourceOwner::of(relative).isEmpty() || !QStringList{QStringLiteral("tex"), QStringLiteral("md"), QStringLiteral("markdown")}.contains(QFileInfo(path).suffix().toLower()))
            return failure(QStringLiteral("invalid_source"), QStringLiteral("Choose a LaTeX or Markdown source inside a Thoughts project."));
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly) || source.size() > 4 * 1024 * 1024)
            return failure(QStringLiteral("read_failed"), QStringLiteral("The source is unavailable or too large."));
        const auto before = source.readAll(); source.close();
        if (sha256(before) != expected) return failure(QStringLiteral("source_conflict"), QStringLiteral("The source changed; select the passage again."));
        QJsonObject owner; QString base;
        const QString ownerError = sourceDocumentOwner(root, relative, bundleRoot, &owner, &base);
        if (ownerError == QLatin1String("ambiguous_document")) return failure(ownerError, QStringLiteral("This source belongs to multiple Library documents."));
        if (!ownerError.isEmpty()) return failure(ownerError, QStringLiteral("Sync this source to register its Library document before moving a card into it."));
        const QString fromStem = current->pdfStem();
        const QString toStem = QFileInfo(owner.value(QStringLiteral("filename")).toString()).completeBaseName();
        if (toStem == fromStem) return failure(QStringLiteral("same_document"), QStringLiteral("The card already belongs to this document."));
        // A Project spans its default folder (imported PDFs) and its connected docs-* folders.
        // A card may leave its Project once the server accepts that (an older one would refuse the queued move).
        const bool anyProject = QSettings().value(QStringLiteral("thoughts-server/capabilities")).toStringList()
            .contains(QStringLiteral("client_review_card_move_any_project_v1"));
        auto primary = primaryObject(card);
        QString fromDocument = primary.value(QStringLiteral("document_id")).toString();
        if (!DocumentIdentityStore::validId(fromDocument)) fromDocument = primary.value(QStringLiteral("pdf_document_id")).toString();
        if (!DocumentIdentityStore::validId(fromDocument))
            fromDocument = DocumentIdentityStore(root).byFilename(fromStem + QStringLiteral(".pdf")).value(QStringLiteral("document_id")).toString();
        const auto project = ProjectConfiguration::find(root, SourceOwner::of(relative));
        const auto declared = project.value(QStringLiteral("documents")).toArray();
        if (!anyProject && !std::any_of(declared.cbegin(), declared.cend(), [&fromDocument](const QJsonValue &d) {
                return d.toObject().value(QStringLiteral("document_id")).toString() == fromDocument; }))
            return failure(QStringLiteral("other_project"), QStringLiteral("Moving a card to another Project needs an updated Thoughts server; "
                                                                           "until then it can only move within its Project."));
        const auto plan = markReviewSourceSelection(relative, before, startLine, endLine, id);
        if (!plan.error.isEmpty()) return failure(QStringLiteral("invalid_selection"), plan.error);
        QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
            {QStringLiteral("state"), QStringLiteral("card_moved")}, {QStringLiteral("card_id"), id}, {QStringLiteral("request_id"), requestId},
            {QStringLiteral("from_document"), QString(fromStem + QStringLiteral(".pdf"))}, {QStringLiteral("document"), owner.value(QStringLiteral("filename"))},
            {QStringLiteral("source"), path}, {QStringLiteral("source_sha256"), sha256(plan.replacement)},
            {QStringLiteral("source_link_state"), QStringLiteral("linked")}, {QStringLiteral("pdf_placement_state"), QStringLiteral("pending")},
            {QStringLiteral("replayed"), false}, {QStringLiteral("message"), QStringLiteral("Card moved locally with its history. Linked: PDF placement pending.")}};
        // The old PDF's placement does not apply to the new document; the next build places the markers.
        for (const auto &key : {QStringLiteral("answer_region_override"), QStringLiteral("source_kind"), QStringLiteral("component_origin"),
                                QStringLiteral("pdf_sha256"), QStringLiteral("page"), QStringLiteral("rect"), QStringLiteral("page_visual_sha256"),
                                QStringLiteral("page_visual_fingerprint_method"), QStringLiteral("answer_anchor")})
            primary.remove(key);
        primary.insert(QStringLiteral("document_id"), owner.value(QStringLiteral("document_id")));
        primary.insert(QStringLiteral("pdf_document_id"), owner.value(QStringLiteral("document_id")));
        primary.insert(QStringLiteral("pdf_stem"), toStem);
        primary.insert(QStringLiteral("regions"), QJsonArray{});
        primary.insert(QStringLiteral("source_anchor"), plan.anchor);
        card.insert(QStringLiteral("primary"), primary);
        card.insert(QStringLiteral("schema_version"), 4);
        card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")}});
        card.insert(QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("updating")}, {QStringLiteral("reason"), QStringLiteral("pdf_placement_pending")}});
        if (objectString(card, QStringLiteral("status")) == QLatin1String("needs_reanchor")) card.insert(QStringLiteral("status"), QStringLiteral("approved"));
        card.insert(QStringLiteral("move"), QJsonObject{{QStringLiteral("from_pdf_stem"), fromStem}, {QStringLiteral("to_pdf_stem"), toStem}});
        card.insert(QStringLiteral("move_request"), QJsonObject{{QStringLiteral("request_id"), requestId}, {QStringLiteral("sha256"), requestHash}, {QStringLiteral("result"), result}});
        card = prepareCardUpdate(card);
        if (!commitSourceChange(path, before, plan.replacement, QStringLiteral("cards/%1.json").arg(id), card, &error, base, requestId))
            return failure(QStringLiteral("move_incomplete"), error);
        return result;
    }
    const bool create = request.value(QStringLiteral("command")).toString() == QLatin1String("create-review-card");
    if (!create && request.value(QStringLiteral("command")).toString() != QLatin1String("list-review-cards"))
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Unknown review command."));
    const QSet<QString> allowed = create
        ? QSet<QString>{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"),
            QStringLiteral("start_line"), QStringLiteral("end_line"), QStringLiteral("question"), QStringLiteral("due_at"), QStringLiteral("request_id")}
        : QSet<QString>{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("source"), QStringLiteral("start_line"), QStringLiteral("end_line"), QStringLiteral("document")};
    for (auto it = request.begin(); it != request.end(); ++it)
        if (!allowed.contains(it.key())) return failure(QStringLiteral("invalid_request"), QStringLiteral("Unexpected field: %1").arg(it.key()));
    const auto integer = [](const QJsonValue &v) { return v.isDouble() && v.toDouble() >= 1 && v.toDouble() <= 2147483647 && std::floor(v.toDouble()) == v.toDouble(); };
    const bool hasRange = request.contains(QStringLiteral("start_line")) || request.contains(QStringLiteral("end_line"));
    if ((create || hasRange) && (!integer(request.value(QStringLiteral("start_line"))) || !integer(request.value(QStringLiteral("end_line")))
        || request.value(QStringLiteral("end_line")).toInt() <= request.value(QStringLiteral("start_line")).toInt()))
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Select one nonempty range with one-based start and exclusive end lines."));
    const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
    const QString requested = request.value(QStringLiteral("source")).toString();
    const QString path = QFileInfo(QDir(root).absoluteFilePath(requested)).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(path);
    const bool validSource = !requested.isEmpty() && !requested.contains(QChar::Null) && !path.isEmpty() && path.startsWith(root + QLatin1Char('/'))
        && !SourceOwner::of(relative).isEmpty() && QStringList{QStringLiteral("tex"), QStringLiteral("md"), QStringLiteral("markdown")}.contains(QFileInfo(path).suffix().toLower());
    if (!create) {
        if (!requested.isEmpty() && !validSource)
            return failure(QStringLiteral("invalid_source"), QStringLiteral("Choose a LaTeX or Markdown source inside a Thoughts project."));
        const QString document = request.value(QStringLiteral("document")).toString();
        if ((requested.isEmpty() && document.isEmpty()) || (hasRange && requested.isEmpty()))
            return failure(QStringLiteral("invalid_request"), QStringLiteral("Provide a source file or exact Library PDF filename."));
        QStringList errors; QJsonArray result;
        // Every card's feedback: a card also lists the other cards' feedback found in its answer (shared).
        const QList<QJsonObject> allFeedback = cardFeedback();
        const QList<ReviewCard> allCards = cards(&errors);
        QHash<QString, QJsonObject> homeCards;
        for (const auto &card : allCards) {
            if (feedbackCardExists(card)) homeCards.insert(card.id(), card.record);
        }
        for (const auto &card : allCards) {
            if (card.schedule.deleted || (!document.isEmpty() && card.pdfStem() + QStringLiteral(".pdf") != document)) continue;
            // Every passage of the card in the requested file; source_range is the first.
            QJsonArray segments;
            for (const auto &v : primaryObject(card.record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray()) {
                const auto s = v.toObject();
                if (s.value(QStringLiteral("source_file")).toString() == relative) segments.append(s);
            }
            if (!requested.isEmpty()) {
                if (segments.isEmpty()) continue;
                QFile file(path); if (!file.open(QIODevice::ReadOnly)) return failure(QStringLiteral("read_failed"), file.errorString());
                const auto anchor = readReviewSourceAnchor(relative, file.readAll(), card.id());
                if (anchor.error.isEmpty()) segments = anchor.anchor.value(QStringLiteral("segments")).toArray();
                if (hasRange) {
                    if (!anchor.error.isEmpty()) continue;
                    const bool overlaps = std::any_of(segments.cbegin(), segments.cend(), [&request](const QJsonValue &value) {
                        const auto s = value.toObject();
                        return s.value(QStringLiteral("line_start")).toInt() < request.value(QStringLiteral("end_line")).toInt()
                            && s.value(QStringLiteral("line_end")).toInt() >= request.value(QStringLiteral("start_line")).toInt();
                    });
                    if (!overlaps) continue;
                }
            }
            const QJsonObject segment = segments.isEmpty() ? QJsonObject() : segments.first().toObject();
            result.append(QJsonObject{{QStringLiteral("card_id"), card.id()}, {QStringLiteral("document"), QString(card.pdfStem() + QStringLiteral(".pdf"))},
                {QStringLiteral("question"), card.question()}, {QStringLiteral("due_at"), utcString(card.schedule.dueAt)},
                {QStringLiteral("interval_days"), card.schedule.intervalDays}, {QStringLiteral("current_mark"), card.currentMark()},
                {QStringLiteral("source_link_state"), card.linkingState()}, {QStringLiteral("pdf_placement"), card.record.value(QStringLiteral("reanchor"))},
                {QStringLiteral("source_range"), segment}, {QStringLiteral("source_ranges"), segments},
                {QStringLiteral("feedback"), reviewCardAgentFeedback(m_docsRoot, card.record, agentFeedbackAnswer(m_docsRoot, card), allFeedback, homeCards, false,
                                                                     [this, &card] { return agentFeedbackAnswerText(m_docsRoot, card); })}});
            // Retired: never reviewed, its schedule kept for a Restore to review.
            if (card.schedule.retired) {
                QJsonObject listed = result.last().toObject();
                listed.insert(QStringLiteral("retired"), true);
                listed.insert(QStringLiteral("retired_at"), card.schedule.retiredAt.toUTC().toString(Qt::ISODateWithMs));
                result.replace(result.size() - 1, listed);
            }
        }
        return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
            {QStringLiteral("state"), QStringLiteral("listed")}, {QStringLiteral("cards"), result},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(errors)}};
    }
    const QString requestId = request.value(QStringLiteral("request_id")).toString();
    const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
    const QString question = request.value(QStringLiteral("question")).toString().trimmed();
    const QString dueString = request.value(QStringLiteral("due_at")).toString();
    const auto due = QDateTime::fromString(dueString, Qt::ISODateWithMs);
    if (!DocumentIdentityStore::validId(requestId) || !validSha256(expected) || !validQuestion(question)
        || !due.isValid() || !QRegularExpression(QStringLiteral(R"((Z|[+-]\d\d:\d\d)$)")).match(dueString).hasMatch())
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Provide a UUID request ID, current source SHA-256, question (1–500 characters), and an ISO review time with timezone."));
    if (!QDir().mkpath(rootPath())) return failure(QStringLiteral("write_failed"), QStringLiteral("Cannot prepare local reviews."));
    QLockFile commandLock(QDir(rootPath()).filePath(QStringLiteral("source-creation.lock")));
    if (!commandLock.tryLock(2000)) return failure(QStringLiteral("busy"), QStringLiteral("Another card is being created."));
    // The request UUID also identifies this creation's card, so an uncertain
    // retry cannot create another card even after the source has changed.
    const QString id = requestId;
    const QString requestHash = sha256(QJsonDocument(request).toJson(QJsonDocument::Compact));
    const auto prior = readObject(QDir(rootPath()).filePath(QStringLiteral("cards/%1.json").arg(id)), nullptr);
    if (!prior.isEmpty()) {
        const auto receipt = prior.value(QStringLiteral("creation_request")).toObject();
        if (receipt.value(QStringLiteral("sha256")).toString() != requestHash)
            return failure(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
        if (QFileInfo::exists(QDir(rootPath()).filePath(QStringLiteral("source-transactions/%1.json").arg(id))))
            return failure(QStringLiteral("recovery_pending"), QStringLiteral("Card creation is saved for recovery; retry with the identical request."));
        auto result = receipt.value(QStringLiteral("result")).toObject(); result.insert(QStringLiteral("replayed"), true); return result;
    }
    if (!validSource)
        return failure(QStringLiteral("invalid_source"), QStringLiteral("Choose a LaTeX or Markdown source inside a Thoughts project."));
    if (due.toUTC() < QDateTime::currentDateTimeUtc().addSecs(-300))
        return failure(QStringLiteral("invalid_request"), QStringLiteral("Choose now or a future review time."));
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() > 4 * 1024 * 1024)
        return failure(QStringLiteral("read_failed"), QStringLiteral("The source is unavailable or too large."));
    const auto before = source.readAll(); source.close();
    if (sha256(before) != expected) return failure(QStringLiteral("source_conflict"), QStringLiteral("The source changed; select the passage again."));
    QJsonObject owner; QString base;
    const QString ownerError = sourceDocumentOwner(root, relative, bundleRoot, &owner, &base);
    if (ownerError == QLatin1String("ambiguous_document")) return failure(ownerError, QStringLiteral("This source belongs to multiple Library documents."));
    if (!ownerError.isEmpty()) return failure(ownerError, QStringLiteral("Sync this source to register its Library document before creating a card."));
    auto plan = markReviewSourceSelection(relative, before, request.value(QStringLiteral("start_line")).toInt(), request.value(QStringLiteral("end_line")).toInt(), id);
    if (!plan.error.isEmpty()) return failure(QStringLiteral("invalid_selection"), plan.error);
    const QString timestamp = utcString(QDateTime::currentDateTimeUtc());
    QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
        {QStringLiteral("state"), QStringLiteral("card_created")}, {QStringLiteral("card_id"), id}, {QStringLiteral("request_id"), requestId},
        {QStringLiteral("source"), path}, {QStringLiteral("source_sha256"), sha256(plan.replacement)},
        {QStringLiteral("due_at"), utcString(due.toUTC())}, {QStringLiteral("document"), owner.value(QStringLiteral("filename"))},
        {QStringLiteral("source_link_state"), QStringLiteral("linked")}, {QStringLiteral("pdf_placement_state"), QStringLiteral("pending")},
        {QStringLiteral("replayed"), false}, {QStringLiteral("message"), QStringLiteral("Card saved locally. Linked: PDF placement pending.")}};
    QJsonObject primary{{QStringLiteral("document_id"), owner.value(QStringLiteral("document_id"))},
        {QStringLiteral("pdf_document_id"), owner.value(QStringLiteral("document_id"))},
        {QStringLiteral("pdf_stem"), QFileInfo(owner.value(QStringLiteral("filename")).toString()).completeBaseName()},
        {QStringLiteral("question"), question}, {QStringLiteral("regions"), QJsonArray{}}, {QStringLiteral("source_anchor"), plan.anchor}};
    QJsonObject card{{QStringLiteral("schema_version"), 4}, {QStringLiteral("card_type"), QStringLiteral("visual_region")},
        {QStringLiteral("card_id"), id}, {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1},
        {QStringLiteral("created_at"), timestamp}, {QStringLiteral("updated_at"), timestamp}, {QStringLiteral("approved_at"), timestamp},
        {QStringLiteral("initial_due_at"), utcString(due.toUTC())}, {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
        {QStringLiteral("creation_method"), QStringLiteral("source_selection_v1")}, {QStringLiteral("primary"), primary},
        {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")}}},
        {QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("updating")}, {QStringLiteral("reason"), QStringLiteral("pdf_placement_pending")}}},
        {QStringLiteral("creation_request"), QJsonObject{{QStringLiteral("sha256"), requestHash}, {QStringLiteral("result"), result}}}};
    QString error;
    if (!commitSourceChange(path, before, plan.replacement, QStringLiteral("cards/%1.json").arg(id), card, &error, base))
        return failure(QStringLiteral("creation_incomplete"), error);
    return result;
}

bool ReviewStore::createManualCard(const QVariantMap &document, int page, const QVariantList &rect, const QString &question, const QString &pageVisualSha256, ReviewCard *createdCard, QString *error, int firstDueDays, const QDateTime &customFirstDueAt, const ReviewAnchorPlan *preparedPlan, const QString &preparedCardId)
{
    if (preparedPlan && QUuid(preparedCardId).isNull()) {
        if (error) *error = QStringLiteral("The prepared card identity is invalid.");
        return false;
    }
    const QString pdfStem = document.value(QStringLiteral("pdf_stem")).toString().trimmed();
    const QString pdfSha256 = document.value(QStringLiteral("pdf_sha256")).toString();
    const int pageCount = document.value(QStringLiteral("page_count")).toInt();
    // A plan for several areas lists them in segment order; the first is the card's page.
    const bool linkOnServer = preparedPlan && preparedPlan->linkOnServer && preparedPlan->sourceMapUnavailable && !preparedPlan->error.isEmpty();
    const QJsonArray planRegions = preparedPlan && (preparedPlan->error.isEmpty() || linkOnServer) ? preparedPlan->regions : QJsonArray();
    if (planRegions.size() > 1) {
        page = planRegions.first().toObject().value(QStringLiteral("page")).toInt();
        for (const auto &value : planRegions) {
            const auto region = value.toObject();
            if (region.value(QStringLiteral("page")).toInt() < 1 || region.value(QStringLiteral("page")).toInt() > pageCount
                || !validRect(region.value(QStringLiteral("rect")).toArray())) {
                if (error) *error = QStringLiteral("Every answer area must be inside a page of this PDF.");
                return false;
            }
        }
    }
    const QJsonArray jsonRect = planRegions.size() > 1 ? planRegions.first().toObject().value(QStringLiteral("rect")).toArray() : QJsonArray::fromVariantList(rect);
    const QString visualSha256 = planRegions.size() > 1 && validSha256(preparedPlan->pageVisualSha256) ? preparedPlan->pageVisualSha256 : pageVisualSha256;
    const QString normalizedQuestion = question.trimmed();
    if (pdfStem.isEmpty() || !validSha256(pdfSha256) || pageCount < 1) {
        if (error) {
            *error = QStringLiteral("The current PDF metadata is incomplete.");
        }
        return false;
    }
    if (page < 1 || page > pageCount || !validRect(jsonRect)) {
        if (error) {
            *error = QStringLiteral("The visual region must be inside one PDF page.");
        }
        return false;
    }
    if (!validQuestion(normalizedQuestion)) {
        if (error) {
            *error = QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength);
        }
        return false;
    }
    if (!validSha256(visualSha256)) {
        if (error) {
            *error = QStringLiteral("The visual page fingerprint is invalid.");
        }
        return false;
    }
    if (firstDueDays < -1 || firstDueDays > 3650 || (firstDueDays == -1 && (!customFirstDueAt.isValid() || customFirstDueAt <= QDateTime::currentDateTimeUtc()))) {
        if (error) *error = QStringLiteral("The first review time is invalid.");
        return false;
    }
    for (const ReviewCard &existing : cards()) {
        if (!existing.isWholeDocument() && existing.pageVisualSha256() == visualSha256 && intersectionOverUnion(primaryObject(existing.record).value(QStringLiteral("rect")).toArray(), jsonRect) >= 0.5) {
            if (error) {
                *error = QStringLiteral("A review card already covers this visual region.");
            }
            return false;
        }
    }

    QJsonObject primary;
    primary.insert(QStringLiteral("document_id"), document.value(QStringLiteral("document_id")).toString());
    primary.insert(QStringLiteral("pdf_stem"), pdfStem);
    primary.insert(QStringLiteral("pdf_sha256"), pdfSha256);
    primary.insert(QStringLiteral("page"), page);
    primary.insert(QStringLiteral("rect"), jsonRect);
    primary.insert(QStringLiteral("question"), normalizedQuestion);
    primary.insert(QStringLiteral("page_visual_sha256"), visualSha256);
    primary.insert(QStringLiteral("page_visual_fingerprint_method"), QString::fromLatin1(reviewFingerprintMethod));
    if (planRegions.size() > 1) primary.insert(QStringLiteral("regions"), planRegions);
    const QString timestamp = utcString(QDateTime::currentDateTimeUtc());
    QJsonObject card;
    card.insert(QStringLiteral("schema_version"), planRegions.size() > 1 ? 4 : schemaVersion);
    card.insert(QStringLiteral("card_type"), QStringLiteral("visual_region"));
    card.insert(QStringLiteral("card_id"), preparedPlan ? preparedCardId : QUuid::createUuid().toString(QUuid::WithoutBraces));
    card.insert(QStringLiteral("status"), QStringLiteral("approved"));
    card.insert(QStringLiteral("created_at"), timestamp);
    if (firstDueDays == -1) card.insert(QStringLiteral("initial_due_at"), utcString(customFirstDueAt.toUTC()));
    else if (firstDueDays > 0) card.insert(QStringLiteral("initial_due_at"), utcString(QDateTime::currentDateTimeUtc().addDays(firstDueDays)));
    card.insert(QStringLiteral("updated_at"), timestamp);
    card.insert(QStringLiteral("approved_at"), timestamp);
    card.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
    card.insert(QStringLiteral("revision"), 1);
    card.insert(QStringLiteral("primary"), primary);
    ReviewAnchorPlan plan;
    const QString pdfPath = document.value(QStringLiteral("pdf_path")).toString();
    if (!pdfPath.isEmpty()) {
        const QString bundleRoot = document.value(QStringLiteral("source_bundle_root"),
            QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"))).toString();
        plan = preparedPlan ? *preparedPlan : planReviewSourceAnchor(m_docsRoot, bundleRoot, pdfPath, pdfSha256, page,
            QRectF(jsonRect[0].toDouble(), jsonRect[1].toDouble(), jsonRect[2].toDouble()-jsonRect[0].toDouble(), jsonRect[3].toDouble()-jsonRect[1].toDouble()), card.value(QStringLiteral("card_id")).toString());
        if (plan.invalidPdf) { if (error) *error = plan.error; return false; }
        // Without this device's source map, the server links the card at the next Sync.
        QJsonObject link {{QStringLiteral("state"), plan.withoutSource ? QStringLiteral("pdf_without_source")
            : linkOnServer ? QStringLiteral("pending_server_link")
            : plan.error.isEmpty() && !plan.anchor.isEmpty() ? QStringLiteral("linked") : QStringLiteral("linking_failed")}};
        if (!plan.error.isEmpty()) link.insert(QStringLiteral("reason"), plan.error);
        if (!plan.fallbackSource.isEmpty()) link.insert(QStringLiteral("source_file"), plan.fallbackSource);
        card.insert(QStringLiteral("source_link"), link);
        if (!plan.error.isEmpty()) {
            // Keep the verified PDF geometry and question, but never commit a
            // partial or guessed source edit after localization failed.
            plan.path.clear();
            plan.anchor = {};
        }
        if (!plan.answerAnchor.isEmpty()) primary.insert(QStringLiteral("answer_anchor"), plan.answerAnchor);
        if (plan.withoutSource) {
            primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
            const auto origin = plan.componentOrigin;
            if (!origin.isEmpty()) primary.insert(QStringLiteral("component_origin"), origin);
        }
        card.insert(QStringLiteral("primary"), primary);
        if (!plan.anchor.isEmpty()) {
            primary.insert(QStringLiteral("source_anchor"), plan.anchor);
            card.insert(QStringLiteral("primary"), primary);
        }
    } else if (m_projectionProvider) {
        if (error) *error = QStringLiteral("The exact review PDF is required.");
        return false;
    }
    card.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
    // The PDF the card is made on (thoughts-feedback-version-stamps-SPEC.md).
    card = FeedbackVersion::stamped(card, reviewVersion(card));
    if (!plan.path.isEmpty()) {
        if (!commitSourceChange(plan.path, plan.original, plan.replacement,
                QStringLiteral("cards/%1.json").arg(card.value(QStringLiteral("card_id")).toString()), card, error)) {
            if (QFileInfo::exists(QDir(rootPath()).filePath(QStringLiteral("source-transactions/%1.json").arg(card.value(QStringLiteral("card_id")).toString())))) return false;
            primary.remove(QStringLiteral("source_anchor")); card.insert(QStringLiteral("primary"), primary);
            card.remove(QStringLiteral("source_change"));
            card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linking_failed")},
                {QStringLiteral("reason"), error ? *error : QStringLiteral("Place the markers manually.")}, {QStringLiteral("source_file"), plan.fallbackSource}});
            if (error) error->clear();
            card = FeedbackVersion::stamped(card, reviewVersion(card));
            if (!writeCard(card, error)) return false;
        }
    } else if (!writeCard(card, error)) {
        return false;
    }
    if (createdCard) {
        *createdCard = {card, {}};
    }
    return true;
}

bool ReviewStore::updateCardLocation(const QString &cardId, const QString &pdfSha256, int page, QString *error)
{
    QJsonObject card = cardRecord(cardId, error);
    if (card.isEmpty() || !validSha256(pdfSha256) || page < 1) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("The card or PDF fingerprint is invalid.");
        }
        return false;
    }
    QJsonObject primary = primaryObject(card);
    primary.insert(QStringLiteral("pdf_sha256"), pdfSha256);
    primary.insert(QStringLiteral("page"), page);
    card.insert(QStringLiteral("primary"), primary);
    card.insert(QStringLiteral("status"), QStringLiteral("approved"));
    card.insert(QStringLiteral("reanchored_at"), utcString(QDateTime::currentDateTimeUtc()));
    return writeCard(prepareCardUpdate(card), error);
}

bool ReviewStore::markNeedsReanchor(const QString &cardId, QString *error)
{
    QJsonObject card = cardRecord(cardId, error);
    if (card.isEmpty()) {
        return false;
    }
    card.insert(QStringLiteral("status"), QStringLiteral("needs_reanchor"));
    card.insert(QStringLiteral("reanchor_failed_at"), utcString(QDateTime::currentDateTimeUtc()));
    return writeCard(prepareCardUpdate(card), error);
}

QString ReviewStore::reviewVersionField() { return FeedbackVersion::field(); }

bool ReviewStore::createHtmlCard(const QVariantMap &document, const QJsonObject &html, const QString &question, ReviewCard *createdCard, QString *error,
                                 int firstDueDays, const QDateTime &customFirstDueAt)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString pdfStem = document.value(QStringLiteral("pdf_stem")).toString().trimmed();
    const QString pdfSha256 = document.value(QStringLiteral("pdf_sha256")).toString().toLower();
    const QString documentId = document.value(QStringLiteral("document_id")).toString();
    const QString edition = html.value(QStringLiteral("html_edition_sha256")).toString();
    const QJsonObject quote = html.value(QStringLiteral("text_quote")).toObject();
    const QString exact = quote.value(QStringLiteral("exact")).toString();
    static const QRegularExpression stemPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!stemPattern.match(pdfStem).hasMatch() || !validSha256(pdfSha256) || documentId.isEmpty())
        return fail(QStringLiteral("The current PDF metadata is incomplete."));
    if (html.value(QStringLiteral("created_in")).toString() != QLatin1String("html") || !validSha256(edition))
        return fail(QStringLiteral("The HTML edition is unknown."));
    if (exact.trimmed().isEmpty() || exact.size() > 500000) return fail(QStringLiteral("Select the answer text."));
    const QString normalizedQuestion = question.trimmed();
    if (!validQuestion(normalizedQuestion))
        return fail(QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength));
    if (firstDueDays < -1 || firstDueDays > 3650 || (firstDueDays == -1 && (!customFirstDueAt.isValid() || customFirstDueAt <= QDateTime::currentDateTimeUtc())))
        return fail(QStringLiteral("The first review time is invalid."));
    QJsonObject primary{{QStringLiteral("document_id"), documentId},
                        {QStringLiteral("pdf_stem"), pdfStem},
                        {QStringLiteral("pdf_sha256"), pdfSha256},
                        {QStringLiteral("selected_text"), exact},
                        {QStringLiteral("question"), normalizedQuestion},
                        {QStringLiteral("created_in"), QStringLiteral("html")},
                        {QStringLiteral("html_edition_sha256"), edition.toLower()},
                        {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), exact},
                                                                   {QStringLiteral("prefix"), quote.value(QStringLiteral("prefix")).toString()},
                                                                   {QStringLiteral("suffix"), quote.value(QStringLiteral("suffix")).toString()}}}};
    const QJsonObject range = html.value(QStringLiteral("source_range")).toObject();
    if (!range.isEmpty()) {
        if (range.value(QStringLiteral("source_file")).toString().isEmpty() || !validSha256(range.value(QStringLiteral("source_sha256")).toString())
            || range.value(QStringLiteral("start_byte")).toInteger(-1) < 0 || range.value(QStringLiteral("end_byte")).toInteger(-1) < range.value(QStringLiteral("start_byte")).toInteger())
            return fail(QStringLiteral("The selection's source range is invalid."));
        primary.insert(QStringLiteral("source_range"), range);
    }
    const QString timestamp = utcString(QDateTime::currentDateTimeUtc());
    QJsonObject card{{QStringLiteral("schema_version"), schemaVersion},
                     {QStringLiteral("card_type"), QStringLiteral("visual_region")},
                     {QStringLiteral("card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("status"), QStringLiteral("approved")},
                     {QStringLiteral("created_at"), timestamp},
                     {QStringLiteral("updated_at"), timestamp},
                     {QStringLiteral("approved_at"), timestamp},
                     {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
                     {QStringLiteral("revision"), 1},
                     {QStringLiteral("primary"), primary},
                     {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
                     {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pending_server_link")},
                                                                 {QStringLiteral("reason"), QStringLiteral("Made in the HTML view; the server places its markers at the next Sync.")}}}};
    if (firstDueDays == -1) card.insert(QStringLiteral("initial_due_at"), utcString(customFirstDueAt.toUTC()));
    else if (firstDueDays > 0) card.insert(QStringLiteral("initial_due_at"), utcString(QDateTime::currentDateTimeUtc().addDays(firstDueDays)));
    // The PDF the card is made on (thoughts-feedback-version-stamps-SPEC.md).
    card = FeedbackVersion::stamped(card, reviewVersion(card));
    if (!writeCard(card, error)) return false;
    if (createdCard) {
        *createdCard = {card, {}};
    }
    return true;
}

bool ReviewStore::createHtmlAttachmentCard(const QVariantMap &document, const QJsonObject &attachment, const QString &question, ReviewCard *createdCard,
                                           QString *error, int firstDueDays, const QDateTime &customFirstDueAt)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString pdfStem = document.value(QStringLiteral("pdf_stem")).toString().trimmed();
    const QString pdfSha256 = document.value(QStringLiteral("pdf_sha256")).toString().toLower();
    const QString documentId = document.value(QStringLiteral("document_id")).toString();
    static const QRegularExpression stemPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    if (!stemPattern.match(pdfStem).hasMatch() || !validSha256(pdfSha256) || documentId.isEmpty())
        return fail(QStringLiteral("The current PDF metadata is incomplete."));
    const QJsonObject ref = attachment.value(QStringLiteral("html_attachment")).toObject();
    const QString path = ref.value(QStringLiteral("path")).toString();
    const QString attachmentSha = ref.value(QStringLiteral("sha256")).toString();
    if (attachment.value(QStringLiteral("created_in")).toString() != QLatin1String("html_attachment") || !Thoughts::isHtmlSourcePath(path) || path.size() > 1024
        || !validSha256(attachmentSha) || attachmentSha != attachmentSha.toLower())
        return fail(QStringLiteral("The HTML attachment is unknown."));
    const QJsonObject quote = attachment.value(QStringLiteral("text_quote")).toObject();
    const QString exact = quote.value(QStringLiteral("exact")).toString();
    if (exact.trimmed().isEmpty() || exact.size() > 500000) return fail(QStringLiteral("Select the answer text."));
    // The page state the text was selected in: location.hash, "" when none.
    const QString hash = attachment.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString();
    static const QRegularExpression control(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    if (hash.size() > 512 || (!hash.isEmpty() && !hash.startsWith(QLatin1Char('#'))) || hash.contains(control))
        return fail(QStringLiteral("The page state of the selection is invalid."));
    const QString normalizedQuestion = question.trimmed();
    if (!validQuestion(normalizedQuestion))
        return fail(QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength));
    if (firstDueDays < -1 || firstDueDays > 3650 || (firstDueDays == -1 && (!customFirstDueAt.isValid() || customFirstDueAt <= QDateTime::currentDateTimeUtc())))
        return fail(QStringLiteral("The first review time is invalid."));
    // Context around the quote: the viewer sends up to 64 code points, the server accepts 200.
    const QString prefix = quote.value(QStringLiteral("prefix")).toString().right(200);
    const QString suffix = quote.value(QStringLiteral("suffix")).toString().left(200);
    // No page, rect, regions or other PDF geometry: the card is found by its text in the page.
    const QJsonObject primary{{QStringLiteral("document_id"), documentId},
                              {QStringLiteral("pdf_stem"), pdfStem},
                              {QStringLiteral("pdf_sha256"), pdfSha256},
                              {QStringLiteral("created_in"), QStringLiteral("html_attachment")},
                              {QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("sha256"), attachmentSha}}},
                              {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), exact}, {QStringLiteral("prefix"), prefix}, {QStringLiteral("suffix"), suffix}}},
                              {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), hash}}},
                              {QStringLiteral("selected_text"), exact},
                              {QStringLiteral("question"), normalizedQuestion}};
    const QString timestamp = utcString(QDateTime::currentDateTimeUtc());
    QJsonObject card{{QStringLiteral("schema_version"), schemaVersion},
                     {QStringLiteral("card_type"), QStringLiteral("visual_region")},
                     {QStringLiteral("card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("status"), QStringLiteral("approved")},
                     {QStringLiteral("created_at"), timestamp},
                     {QStringLiteral("updated_at"), timestamp},
                     {QStringLiteral("approved_at"), timestamp},
                     {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
                     {QStringLiteral("revision"), 1},
                     {QStringLiteral("primary"), primary},
                     {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
                     // Terminal: the server never links these cards to source markers.
                     {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("html_attachment")}}}};
    if (firstDueDays == -1) card.insert(QStringLiteral("initial_due_at"), utcString(customFirstDueAt.toUTC()));
    else if (firstDueDays > 0) card.insert(QStringLiteral("initial_due_at"), utcString(QDateTime::currentDateTimeUtc().addDays(firstDueDays)));
    // The PDF whose link opened the page (thoughts-feedback-version-stamps-SPEC.md).
    card = FeedbackVersion::stamped(card, reviewVersion(card));
    if (!writeCard(card, error)) return false;
    if (createdCard) {
        *createdCard = {card, {}};
    }
    return true;
}

QJsonObject ReviewStore::reviewVersion(const QJsonObject &cardRecord) const
{
    if (cardRecord.isEmpty()) return {};
    const QJsonObject primary = primaryObject(cardRecord);
    // The PDF the reader opens for the card: its document's current PDF, else the one named by its stem.
    QString path = reviewCardPdfPath(m_docsRoot, cardRecord);
    const QString stem = primary.value(QStringLiteral("pdf_stem")).toString();
    if ((path.isEmpty() || !QFileInfo(path).isFile()) && !stem.isEmpty())
        path = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(stem));
    if (path.isEmpty() || !QFileInfo(path).isFile()) return {};
    // Built from a source when the Library index says so or the card has source markers.
    return FeedbackVersion::forPdf(m_docsRoot, path,
        !primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().isEmpty());
}

QHash<QString, QString> ReviewStore::contextNotes(const QString &documentKey) const
{
    if (documentKey.isEmpty()) return {};
    // Card records alone: no source scan, and deleted cards keep their notes.
    return ReviewContextNotes::current(records(QStringLiteral("review_card"), QStringLiteral("cards"), QStringLiteral("card_id")), events(nullptr), documentKey);
}

ReviewContextNotes::Draft ReviewStore::contextDraft(const ReviewCard &card, const QList<ReviewContextNotes::OutlineItem> &outline, const QString &documentLabel) const
{
    using namespace ReviewContextNotes;
    Draft draft;
    draft.cardId = card.id();
    const QString document = documentKey(card.record);
    const QHash<QString, QString> notes = contextNotes(document);
    int page = 0;
    double top = 0;
    const bool placed = primaryPosition(card, &page, &top);
    Note section;
    const bool hasSection = placed && sectionAt(outline, page, top, &section);
    const auto add = [&](const QString &scope, const Note &note, bool available) {
        Draft::Entry entry{scope, note, {}, available};
        if (available) entry.note.text = notes.value(noteId(scope, note.key));
        entry.text = entry.note.text;
        draft.entries.append(entry);
    };
    add(sectionScope(), hasSection ? section : Note{}, hasSection);
    add(pageScope(), placed ? pageNote(page) : Note{}, placed);
    add(documentScope(), Note{QString(), documentLabel.isEmpty() ? card.pdfStem() : documentLabel, {}}, !document.isEmpty());
    draft.selected = hasSection ? 0 : placed ? 1 : 2;
    return draft;
}

QList<ReviewStore::ContextNoteConversion> ReviewStore::convertWholeDocumentCards(const std::function<QString(const ReviewCard &)> &documentLabel, QStringList *errors,
                                                                                 const QDateTime &now)
{
    using namespace ReviewContextNotes;
    QList<ContextNoteConversion> converted;
    if (m_readOnly || !now.isValid()) return converted;
    const QList<ReviewCard> all = cards();
    if (std::none_of(all.cbegin(), all.cend(), [](const ReviewCard &card) { return card.isWholeDocument() && !card.schedule.deleted; })) return converted;
    const QUuid urlNamespace(QStringLiteral("6ba7b811-9dad-11d1-80b4-00c04fd430c8"));
    QDateTime at = now.toUTC();
    const auto iso = [](const QDateTime &value) { return value.toUTC().toString(Qt::ISODateWithMs); };
    for (const ReviewCard &card : all) {
        if (!card.isWholeDocument() || card.schedule.deleted || QUuid(card.id()).isNull()) continue;
        const QString cardId = card.id();
        const QString noteEventId = QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("context-note-conversion:") + cardId)).toString(QUuid::WithoutBraces);
        const QString deleteEventId = QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("context-note-conversion-delete:") + cardId)).toString(QUuid::WithoutBraces);
        const QString document = documentKey(card.record);
        // Later than every event of the card and every note of its document, so this note is
        // the newest and the deletion comes after it.
        const QList<QJsonObject> cardRecords = records(QStringLiteral("review_card"), QStringLiteral("cards"), QStringLiteral("card_id"));
        const QList<QJsonObject> allEvents = events(nullptr);
        QSet<QString> documentCards;
        for (const QJsonObject &record : std::as_const(cardRecords))
            if (documentKey(record) == document) documentCards.insert(objectString(record, QStringLiteral("card_id")));
        bool noteRecorded = false;
        for (const QJsonObject &event : allEvents) {
            const QString eventCard = objectString(event, QStringLiteral("card_id"));
            if (eventCard == cardId && objectString(event, QStringLiteral("event_id")) == noteEventId) noteRecorded = true;
            if (eventCard != cardId && !(documentCards.contains(eventCard) && event.contains(field()))) continue;
            const QDateTime eventAt = parseUtc(objectString(event, QStringLiteral("reviewed_at")));
            if (eventAt.isValid() && eventAt >= at) at = eventAt.addMSecs(1);
        }
        QString error;
        if (!noteRecorded) {
            // The question, then the latest answer's own Note.
            QString text = card.question();
            const QList<QJsonObject> answers = card.answers();
            for (auto answer = answers.crbegin(); answer != answers.crend(); ++answer) {
                const QString note = ReviewTags::groupNotesOfEvent(*answer).value(ReviewTags::answerNoteKey());
                if (note.isEmpty()) continue;
                text += QLatin1Char('\n') + note;
                break;
            }
            const QString existing = current(cardRecords, allEvents, document).value(noteId(documentScope(), QString()));
            if (!existing.isEmpty()) text = existing + QLatin1Char('\n') + text;
            const QString label = documentLabel ? documentLabel(card) : QString();
            const QJsonObject context{{documentScope(), QJsonObject{{QStringLiteral("key"), QString()},
                                                                    {QStringLiteral("label"), label.isEmpty() ? card.pdfStem() : label},
                                                                    {QStringLiteral("text"), ReviewTags::cleanGroupNote(text)}}}};
            // A skip of one minute carries the note: every client reads it, and it is no answer.
            const QJsonObject event{{QStringLiteral("schema_version"), schemaVersion}, {QStringLiteral("event_id"), noteEventId},
                                    {QStringLiteral("card_id"), cardId}, {QStringLiteral("skip_id"), noteEventId},
                                    {QStringLiteral("queue_action"), QStringLiteral("skip")}, {QStringLiteral("skip_until"), iso(at.addSecs(60))},
                                    {QStringLiteral("reviewed_at"), iso(at)}, {QStringLiteral("updated_at"), iso(at)},
                                    {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1},
                                    {field(), context}};
            QJsonObject stamped = event;
            if (const QJsonObject version = reviewVersion(card.record); !version.isEmpty()) stamped.insert(reviewVersionField(), version);
            if (!writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(noteEventId), stamped, &error)) {
                if (errors) errors->append(QStringLiteral("card %1: %2").arg(cardId, error));
                continue;
            }
            at = at.addMSecs(1);
        }
        // As thoughtsctl delete-review-card: a delete event; a whole-document card has no markers.
        const QJsonObject deletion{{QStringLiteral("schema_version"), schemaVersion}, {QStringLiteral("event_id"), deleteEventId},
                                   {QStringLiteral("card_id"), cardId}, {QStringLiteral("rating"), QStringLiteral("delete")},
                                   {QStringLiteral("scheduler_version"), currentSchedulerVersion},
                                   {QStringLiteral("source_anchor"), primaryObject(card.record).value(QStringLiteral("source_anchor")).toObject()},
                                   {QStringLiteral("reviewed_at"), iso(at)}, {QStringLiteral("updated_at"), iso(at)},
                                   {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1}};
        if (!writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(deleteEventId), deletion, &error)) {
            if (errors) errors->append(QStringLiteral("card %1: %2").arg(cardId, error));
            continue;
        }
        at = at.addMSecs(1);
        converted.append({cardId, document, noteRecorded ? QString() : noteEventId, deleteEventId});
    }
    return converted;
}

QHash<QString, int> ReviewStore::reviewTagUsage() const
{
    // As on the phone: the cards, deleted ones aside, whose current tags (their latest answer in
    // effect, merged names) include each tag. A card answered again moves to its new tags, an
    // undo moves it back, a skip keeps them, and a card never answered counts for nothing. A tag
    // of an earlier answer still in effect that no card has now is listed with 0.
    QHash<QString, int> usage;
    for (const auto &card : cards()) {
        for (const auto &event : effectiveAnswers(card)) {
            for (const QString &tag : card.tagsOfEvent(event))
                if (!usage.contains(tag)) usage.insert(tag, 0);
        }
        if (card.schedule.deleted) continue;
        QStringList tags = card.currentTags();
        tags.removeDuplicates();
        for (const QString &tag : std::as_const(tags)) ++usage[tag];
    }
    return usage;
}

bool ReviewStore::recordRating(const QString &cardId, const QString &rating, const QDateTime &reviewedAt, QString *error, const QDateTime &scheduledDueAt, QString *recordedEventId, const QString &requestedAction, const QStringList &reviewTags,
                               const QHash<QString, QString> &groupNotes, const QHash<QString, ReviewContextNotes::Note> &contextNotes,
                               const QJsonObject *cardRecord, const QJsonObject &reviewPhases, const QString &reviewEntry)
{
    QStringList tags;
    for (const QString &value : reviewTags) {
        const QString tag = ReviewTags::normalize(value);
        // Skip is a postponement (ReviewStore::skip), never an answer's tag.
        if (!tag.isEmpty() && tag.compare(ReviewTags::skip(), Qt::CaseInsensitive) != 0 && !tags.contains(tag)) tags.append(tag);
    }
    const QString reviewAction = requestedAction.isEmpty() ? ReviewTags::legacyReviewAction(tags) : requestedAction;
    if ((!reviewAction.isEmpty() && reviewAction != QLatin1String("correct") && reviewAction != QLatin1String("error") && reviewAction != QLatin1String("wrong") && reviewAction != QLatin1String("skip"))
        || QUuid(cardId).isNull() || !reviewRating(rating) || !reviewedAt.isValid()
        || (rating == QLatin1String("custom_interval") && (!scheduledDueAt.isValid() || scheduledDueAt <= reviewedAt))) {
        if (error) {
            *error = QStringLiteral("Review rating or card identifier is invalid.");
        }
        return false;
    }
    const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject event;
    event.insert(QStringLiteral("schema_version"), schemaVersion);
    event.insert(QStringLiteral("event_id"), eventId);
    event.insert(QStringLiteral("card_id"), cardId);
    event.insert(QStringLiteral("rating"), rating);
    if (!reviewAction.isEmpty()) event.insert(QStringLiteral("review_action"), reviewAction);
    // An answer given with no tag says so with an empty list, so readers do not take it for an
    // older client's untagged Correct. Deletions and older ratings or actions carry no list.
    const bool taggedAnswer = requestedAction.isEmpty() && (rating == QLatin1String("custom_interval") || rating == QLatin1String("retained"));
    if (!tags.isEmpty() || taggedAnswer) event.insert(QStringLiteral("review_tags"), QJsonArray::fromStringList(tags));
    // Notes go with an answer's tags, never with a deletion.
    if (const auto notes = ReviewTags::cleanGroupNotes(groupNotes); !notes.isEmpty() && rating != QLatin1String("delete")) {
        QJsonObject recorded;
        for (auto it = notes.cbegin(); it != notes.cend(); ++it) recorded.insert(it.key(), it.value());
        event.insert(ReviewTags::groupNotesField(), recorded);
    }
    // Section, page and document notes changed with this answer; an empty text clears one.
    if (const QJsonObject context = ReviewContextNotes::toField(contextNotes); !context.isEmpty() && rating != QLatin1String("delete"))
        event.insert(ReviewContextNotes::field(), context);
    // What was picked before the answer was shown and after it, with the agent's prediction.
    if (!reviewPhases.isEmpty() && rating != QLatin1String("delete")) event.insert(ReviewPhases::field(), reviewPhases);
    // Whether the answer came from the due cards or from Review now (ReviewEntry).
    if (!reviewEntry.isEmpty() && rating != QLatin1String("delete")) event.insert(ReviewEntry::field(), reviewEntry);
    if (rating != QLatin1String("delete")) {
        if (const QJsonObject version = reviewVersion(cardRecord ? *cardRecord : this->cardRecord(cardId)); !version.isEmpty())
            event.insert(reviewVersionField(), version);
    }
    if (rating == QLatin1String("custom_interval")) {
        event.insert(QStringLiteral("scheduled_due_at"), scheduledDueAt.toUTC().toString(Qt::ISODateWithMs));
    }
    event.insert(QStringLiteral("scheduler_version"), currentSchedulerVersion);
    event.insert(QStringLiteral("reviewed_at"), reviewedAt.toUTC().toString(Qt::ISODateWithMs));
    event.insert(QStringLiteral("updated_at"), reviewedAt.toUTC().toString(Qt::ISODateWithMs));
    event.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
    event.insert(QStringLiteral("revision"), 1);
    const bool saved = writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(eventId), event, error);
    if (saved && recordedEventId) {
        *recordedEventId = eventId;
    }
    return saved;
}

bool ReviewStore::setInterval(const QString &cardId, const ReviewInterval &interval, bool never,
                              const QDateTime &selectedAt, QString *error)
{
    const auto currentCards = cards();
    const auto card = std::find_if(currentCards.cbegin(), currentCards.cend(), [&cardId](const ReviewCard &item) {
        return item.id() == cardId;
    });
    if (QUuid(cardId).isNull() || !selectedAt.isValid() || card == currentCards.cend()
        || !card->isApproved() || card->schedule.deleted) {
        if (error) *error = QStringLiteral("Review card is unavailable for interval changes.");
        return false;
    }
    QDateTime effectiveAt = selectedAt.toUTC();
    for (const auto &prior : card->history) {
        const QDateTime priorAt = parseUtc(objectString(prior, QStringLiteral("reviewed_at")));
        if (priorAt.isValid() && priorAt >= effectiveAt) effectiveAt = priorAt.addMSecs(1);
    }
    const QDateTime due = never ? QDateTime{} : interval.dueAt(effectiveAt);
    if (!never && !due.isValid()) {
        if (error) *error = QStringLiteral("Choose a future review interval.");
        return false;
    }
    QString unit;
    if (never) unit = QStringLiteral("never");
    else if (interval.date.isValid()) unit = QStringLiteral("date");
    else switch (interval.unit) {
    case ReviewInterval::Minutes: unit = QStringLiteral("minutes"); break;
    case ReviewInterval::Hours: unit = QStringLiteral("hours"); break;
    case ReviewInterval::Days: unit = QStringLiteral("days"); break;
    case ReviewInterval::Weeks: unit = QStringLiteral("weeks"); break;
    case ReviewInterval::Months: unit = QStringLiteral("months"); break;
    }
    const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject event{{QStringLiteral("schema_version"), schemaVersion},
                      {QStringLiteral("event_id"), eventId},
                      {QStringLiteral("card_id"), cardId},
                      {QStringLiteral("interval_action"), QStringLiteral("set")},
                      {QStringLiteral("interval_unit"), unit},
                      {QStringLiteral("reviewed_at"), effectiveAt.toUTC().toString(Qt::ISODateWithMs)},
                      {QStringLiteral("updated_at"), effectiveAt.toUTC().toString(Qt::ISODateWithMs)},
                      {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
                      {QStringLiteral("revision"), 1}};
    if (!never) {
        event.insert(QStringLiteral("scheduled_due_at"), due.toUTC().toString(Qt::ISODateWithMs));
        if (unit == QLatin1String("date")) event.insert(QStringLiteral("interval_date"), interval.date.toString(Qt::ISODate));
        else event.insert(QStringLiteral("interval_amount"), interval.amount);
    }
    if (const QJsonObject version = reviewVersion(card->record); !version.isEmpty()) event.insert(reviewVersionField(), version);
    return writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(eventId), event, error);
}

bool ReviewStore::retractRating(const QString &cardId, const QString &eventId, const QDateTime &retractedAt, QString *error) const
{
    if (QUuid(cardId).isNull() || QUuid(eventId).isNull()) {
        if (error) {
            *error = QStringLiteral("Review card or event identifier is invalid.");
        }
        return false;
    }
    for (const auto &event : events(nullptr)) {
        if (objectString(event, QStringLiteral("event_id")) == eventId
            && objectString(event, QStringLiteral("rating")) == QLatin1String("delete")) {
            if (error) *error = QStringLiteral("Deleted review cards cannot be restored.");
            return false;
        }
    }
    const QString retractionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject event;
    event.insert(QStringLiteral("schema_version"), schemaVersion);
    event.insert(QStringLiteral("event_id"), retractionId);
    event.insert(QStringLiteral("card_id"), cardId);
    event.insert(QStringLiteral("retracted_event_id"), eventId);
    event.insert(QStringLiteral("reviewed_at"), utcString(retractedAt));
    event.insert(QStringLiteral("updated_at"), utcString(retractedAt));
    event.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
    event.insert(QStringLiteral("revision"), 1);
    return writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(retractionId), event, error);
}

bool ReviewStore::answerInEffect(const ReviewCard &card, const QString &eventId)
{
    if (eventId.isEmpty() || card.schedule.deleted) return false;
    const auto answer = std::find_if(card.history.cbegin(), card.history.cend(), [&eventId](const QJsonObject &event) {
        return objectString(event, QStringLiteral("event_id")) == eventId;
    });
    if (answer == card.history.cend() || objectString(*answer, QStringLiteral("card_id")) != card.id()) return false;
    const QString rating = objectString(*answer, QStringLiteral("rating"));
    if (!reviewRating(rating) || rating == QLatin1String("delete") || !objectString(*answer, QStringLiteral("queue_action")).isEmpty()
        || !objectString(*answer, QStringLiteral("interval_action")).isEmpty() || !objectString(*answer, QStringLiteral("retracted_event_id")).isEmpty())
        return false;
    // Taken back already (Undo, or here), on any device.
    return std::none_of(card.history.cbegin(), card.history.cend(), [&eventId, &card](const QJsonObject &event) {
        return objectString(event, QStringLiteral("retracted_event_id")) == eventId && objectString(event, QStringLiteral("card_id")) == card.id();
    });
}

bool ReviewStore::updateCardContent(const ReviewCard &expected, const QVariantList &rect, const QString &question, QString *error)
{
    const QJsonArray region = QJsonArray::fromVariantList(rect);
    if (!validRect(region) || !validQuestion(question)) {
        if (error) {
            *error = QStringLiteral("Enter a question of 1–500 characters and a valid region inside one PDF page.");
        }
        return false;
    }
    QJsonObject card = cardRecord(expected.id(), error);
    if (card.isEmpty()) {
        return false;
    }
    QJsonObject primary = primaryObject(card);
    if (primary != primaryObject(expected.record) || objectString(card, QStringLiteral("status")) != expected.status()) {
        if (error) {
            *error = QStringLiteral("This card changed during editing. Reopen the editor and try again.");
        }
        return false;
    }
    // rect() reports the edited areas' first rectangle when Edit Areas… overrode them, so
    // that value also means "unchanged": a question-only edit must not touch the areas.
    const bool rectUnchanged = primary.value(QStringLiteral("rect")).toArray() == region
        || QJsonArray::fromVariantList(expected.rect()) == region;
    if (rectUnchanged && objectString(primary, QStringLiteral("question")) == question.trimmed()) {
        return true;
    }
    if (primary.contains(QStringLiteral("regions")) && !rectUnchanged) {
        if (error) *error = QStringLiteral("Use Edit Areas… to change the answer areas of this card.");
        return false;
    }
    if (!rectUnchanged) primary.insert(QStringLiteral("rect"), region);
    primary.insert(QStringLiteral("question"), question.trimmed());
    card.insert(QStringLiteral("primary"), primary);
    return writeCard(prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card))), error);
}

bool ReviewStore::updateCardAnswer(const ReviewCard &expected, const QVariantList &regions, const QString &question, int pageCount, QString *error)
{
    if (!validQuestion(question) || regions.isEmpty() || regions.size() > 1000 || !validSha256(expected.pdfSha256())) {
        if (error) *error = QStringLiteral("Enter a question of 1–500 characters and select at least one answer area.");
        return false;
    }
    for (const auto &value : regions) {
        const auto region = value.toMap();
        const int page = region.value(QStringLiteral("page")).toInt();
        if (page < 1 || page > pageCount || !validNormalizedReviewCoordinates(QJsonArray::fromVariantList(region.value(QStringLiteral("rect")).toList()), 4)) {
            if (error) *error = QStringLiteral("Every answer area must be inside a page of this PDF.");
            return false;
        }
    }
    auto card = cardRecord(expected.id(), error);
    if (card.isEmpty()) return false;
    auto primary = primaryObject(card);
    if (primary != primaryObject(expected.record) || objectString(card, QStringLiteral("status")) != expected.status()) {
        if (error) *error = QStringLiteral("This card changed during editing. Reopen the editor and try again.");
        return false;
    }
    if (regions == expected.regions() && question.trimmed() == expected.question()) return true;
    if (regions != expected.regions()) {
        if (isBasicReviewCard(card)) {
            primary.remove(QStringLiteral("source_anchor"));
            primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
            card.insert(QStringLiteral("reanchor"), QJsonObject{{QStringLiteral("state"), QStringLiteral("succeeded")},
                {QStringLiteral("strategy"), QStringLiteral("basic_pdf_snapshot_v1")}, {QStringLiteral("target_pdf_sha256"), expected.pdfSha256()}});
        }
        primary.insert(QStringLiteral("answer_region_override"), QJsonObject{
            {QStringLiteral("pdf_sha256"), expected.pdfSha256()}, {QStringLiteral("regions"), QJsonArray::fromVariantList(regions)}});
    }
    primary.insert(QStringLiteral("question"), question.trimmed());
    card.insert(QStringLiteral("primary"), primary);
    return writeCard(prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card))), error);
}

bool ReviewStore::replaceCardAreas(const ReviewCard &expected, const QString &pdfSha256, const QString &question,
                                   const ReviewAnchorPlan &plan, QString *error)
{
    const auto fail = [error](const QString &reason) { if (error) *error = reason; return false; };
    if (m_readOnly) return fail(QStringLiteral("Cannot write through a review snapshot."));
    if (!validQuestion(question)) return fail(QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength));
    if (!plan.error.isEmpty()) return fail(plan.error);
    if (plan.withoutSource || plan.anchor.value(QStringLiteral("segments")).toArray().isEmpty() || plan.path.isEmpty()
        || plan.regions.isEmpty() || !validSha256(pdfSha256) || !validSha256(plan.pageVisualSha256))
        return fail(QStringLiteral("The new answer areas are not linked to the source."));
    for (const auto &value : plan.regions) {
        const auto region = value.toObject();
        if (region.value(QStringLiteral("page")).toInt() < 1 || !validRect(region.value(QStringLiteral("rect")).toArray()))
            return fail(QStringLiteral("Every answer area must be inside a page of this PDF."));
    }
    auto card = cardRecord(expected.id(), error);
    if (card.isEmpty()) return false;
    if (card != expected.record) return fail(QStringLiteral("This card changed during editing. Reopen the editor and try again."));
    if (expected.isWholeDocument() || isBasicReviewCard(card)) return fail(QStringLiteral("This card's answer is not placed by areas."));
    const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(QFileInfo(plan.path).canonicalFilePath());
    // One source change edits one file: markers the card still has elsewhere would remain.
    for (const auto &value : primaryObject(card).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray()) {
        const QString other = value.toObject().value(QStringLiteral("source_file")).toString();
        if (other.isEmpty() || other == relative) continue;
        QFile file(QDir(m_docsRoot).filePath(other));
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(expected.id().toUtf8()))
            return fail(QStringLiteral("This card's passage is marked in %1. Place its areas in that file, or remove its markers there first.").arg(other));
    }
    const auto first = plan.regions.first().toObject();
    auto primary = primaryObject(card);
    primary.remove(QStringLiteral("answer_region_override"));
    primary.remove(QStringLiteral("source_kind"));
    primary.remove(QStringLiteral("component_origin"));
    primary.insert(QStringLiteral("question"), question.trimmed());
    primary.insert(QStringLiteral("pdf_sha256"), pdfSha256);
    primary.insert(QStringLiteral("page"), first.value(QStringLiteral("page")).toInt());
    primary.insert(QStringLiteral("rect"), first.value(QStringLiteral("rect")).toArray());
    primary.insert(QStringLiteral("regions"), plan.regions);
    primary.insert(QStringLiteral("page_visual_sha256"), plan.pageVisualSha256);
    primary.insert(QStringLiteral("page_visual_fingerprint_method"), QString::fromLatin1(reviewFingerprintMethod));
    primary.insert(QStringLiteral("source_anchor"), plan.anchor);
    if (!plan.answerAnchor.isEmpty()) primary.insert(QStringLiteral("answer_anchor"), plan.answerAnchor);
    card.insert(QStringLiteral("primary"), primary);
    card.insert(QStringLiteral("schema_version"), 4);
    card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")}});
    // The areas are placed on this PDF; the next build places the markers again.
    card.remove(QStringLiteral("reanchor"));
    if (objectString(card, QStringLiteral("status")) == QLatin1String("needs_reanchor")) card.insert(QStringLiteral("status"), QStringLiteral("approved"));
    card.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
    card = prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card)));
    return commitSourceChange(plan.path, plan.original, plan.replacement, QStringLiteral("cards/%1.json").arg(expected.id()), card, error,
                              {}, QUuid::createUuid().toString(QUuid::WithoutBraces));
}

struct ReviewStore::CardMergePlan {
    QList<ReviewCard> cards; // Current copies, in reading order.
    QHash<QString, QJsonObject> records;
    QString keptId;
    QStringList others;      // The deletions' order: reading order.
    QString relative, path;
    QByteArray source;
    ReviewCardMergeSource merge;
    bool pdf = false;        // Cards of an imported PDF: nothing in a source changes.
};

// A card as merge messages name it: its question, shortened.
static QString mergeCardLabel(const ReviewCard &card)
{
    QString question = card.question().simplified();
    if (question.size() > 60) question = question.left(57) + QStringLiteral("…");
    return question.isEmpty() ? card.id() : question;
}

QString ReviewStore::defaultMergeKeptCard(const QList<ReviewCard> &cards)
{
    const ReviewCard *best = nullptr;
    for (const ReviewCard &card : cards)
        if (!best || card.schedule.reviewCount > best->schedule.reviewCount) best = &card;
    return best ? best->id() : QString();
}

QString ReviewStore::joinedMergeQuestion(const QList<ReviewCard> &cards)
{
    QStringList questions;
    for (const ReviewCard &card : cards) {
        const QString question = card.question().trimmed();
        if (!question.isEmpty() && !questions.contains(question)) questions.append(question);
    }
    return questions.join(QLatin1Char(' '));
}

QString ReviewStore::prepareCardMerge(const QList<ReviewCard> &requested, QString keptId, const QString &bundleRoot, CardMergePlan *plan, bool *syncFirst) const
{
    if (syncFirst) *syncFirst = false;
    QStringList ids, documents;
    for (const ReviewCard &card : requested) {
        if (ids.contains(card.id())) continue;
        ids.append(card.id());
        const QString document = card.pdfStem().isEmpty() ? card.id() : card.pdfStem() + QStringLiteral(".pdf");
        if (!documents.contains(document)) documents.append(document);
    }
    if (ids.size() < 2) return QStringLiteral("Select two or more cards to merge.");
    if (ids.size() > 20) return QStringLiteral("Merge at most 20 cards at once.");
    if (documents.size() > 1)
        return QStringLiteral("Only cards of one document can be merged. The selected cards belong to %1 documents: %2.")
            .arg(documents.size()).arg(documents.join(QStringLiteral(", ")));
    if (!keptId.isEmpty() && !ids.contains(keptId)) return QStringLiteral("The card to keep must be one of the cards to merge.");
    QHash<QString, ReviewCard> current;
    for (const ReviewCard &card : cards())
        if (ids.contains(card.id())) current.insert(card.id(), card);
    // Cards marked in a note merge in its source; cards with no source (an imported PDF) merge on the
    // PDF alone. The two kinds are not merged together.
    QString markedExample, pdfExample;
    for (const ReviewCard &card : requested) {
        const auto found = current.constFind(card.id());
        if (found == current.cend()) continue;
        const bool marked = !primaryObject(found->record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().isEmpty();
        if (marked && markedExample.isEmpty()) markedExample = mergeCardLabel(*found);
        if (!marked && !found->isWholeDocument() && !isBasicReviewCard(found->record) && pdfExample.isEmpty()) pdfExample = mergeCardLabel(*found);
    }
    if (!markedExample.isEmpty() && !pdfExample.isEmpty())
        return QStringLiteral("Card “%1” is marked in its note and card “%2” is only on the PDF. Merge cards of a note with each other, "
                              "and cards of a PDF with each other.").arg(markedExample, pdfExample);
    const bool pdfMerge = markedExample.isEmpty();
    QStringList files, pdfs;
    QHash<QString, QSet<QString>> segmentIds;
    QSet<QString> placedOn; // The source versions the cards' passages were marked or found in.
    QSet<QString> seen;
    for (const ReviewCard &card : requested) {
        if (seen.contains(card.id())) continue;
        seen.insert(card.id());
        const auto found = current.constFind(card.id());
        if (found == current.cend() || found->schedule.deleted) return QStringLiteral("A selected card is no longer available. Refresh and try again.");
        // The caller's copy is compared as cards() gives it; the stored record is what is written.
        const QJsonObject record = cardRecord(card.id());
        if (record.isEmpty() || found->record != card.record) return QStringLiteral("Card “%1” changed. Refresh and try again.").arg(mergeCardLabel(card));
        if (!found->isApproved()) return QStringLiteral("Card “%1” is not an approved card.").arg(mergeCardLabel(card));
        if (pdfMerge) {
            if (found->isWholeDocument() || isBasicReviewCard(record))
                return QStringLiteral("Card “%1” has no answer areas of its own on the PDF, so it cannot be merged.").arg(mergeCardLabel(card));
            if (found->regions().isEmpty()) return QStringLiteral("Card “%1” has no answer areas on the PDF.").arg(mergeCardLabel(card));
            if (!pdfs.contains(found->pdfSha256())) pdfs.append(found->pdfSha256());
            plan->cards.append(*found);
            plan->records.insert(card.id(), record);
            continue;
        }
        const auto segments = primaryObject(record).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
        if (found->isWholeDocument() || isBasicReviewCard(record) || segments.isEmpty())
            return QStringLiteral("Card “%1” is not linked to a source passage. Only cards whose answers are marked in the source can be merged.").arg(mergeCardLabel(card));
        // Linked, but its markers are not where Thoughts last found them. While the sources are only
        // being checked again (after each Sync) the merge goes on: it reads the markers in the file itself.
        if (const QString state = found->linkingState();
            state != QLatin1String("linked") && !(state == QLatin1String("unavailable") && found->sourceLocation.value(QStringLiteral("checking")).toBool())) {
            if (syncFirst) *syncFirst = state == QLatin1String("unavailable");
            return QStringLiteral("The markers of card “%1” cannot be confirmed in its source right now (%2). Try again in a moment, or Sync first.")
                .arg(mergeCardLabel(card), state);
        }
        for (const auto &value : segments) {
            const auto segment = value.toObject();
            const QString file = segment.value(QStringLiteral("source_file")).toString();
            if (!files.contains(file)) files.append(file);
            segmentIds[card.id()].insert(segment.value(QStringLiteral("segment_id")).toString());
            placedOn.insert(segment.value(QStringLiteral("source_sha256")).toString());
        }
        if (segmentIds.value(card.id()).size() != segments.size())
            return QStringLiteral("The passages of card “%1” are not identified uniquely.").arg(mergeCardLabel(card));
        if (!pdfs.contains(found->pdfSha256())) pdfs.append(found->pdfSha256());
        if (found->regions().isEmpty())
            return QStringLiteral("Card “%1” has no answer areas on the PDF yet. Sync and wait for the PDF to be rebuilt, then merge the cards.").arg(mergeCardLabel(card));
        plan->cards.append(*found);
        plan->records.insert(card.id(), record);
    }
    if (pdfMerge) {
        if (pdfs.size() > 1 || !validSha256(pdfs.value(0)))
            return QStringLiteral("The cards are placed on different versions of the PDF, so their areas cannot be combined.");
        // Reading order: each card by its first area (page, then top, then left).
        const auto firstArea = [](const ReviewCard &card) {
            std::tuple<int, double, double> first{std::numeric_limits<int>::max(), 0, 0};
            for (const QVariant &value : card.regions()) {
                const QVariantMap region = value.toMap();
                const QVariantList rect = region.value(QStringLiteral("rect")).toList();
                first = std::min(first, std::tuple<int, double, double>{region.value(QStringLiteral("page")).toInt(), rect.value(1).toDouble(), rect.value(0).toDouble()});
            }
            return first;
        };
        std::stable_sort(plan->cards.begin(), plan->cards.end(), [&firstArea](const ReviewCard &a, const ReviewCard &b) { return firstArea(a) < firstArea(b); });
        if (keptId.isEmpty()) keptId = defaultMergeKeptCard(plan->cards);
        plan->keptId = keptId;
        for (const ReviewCard &card : std::as_const(plan->cards))
            if (card.id() != keptId) plan->others.append(card.id());
        plan->pdf = true;
        return {};
    }
    if (files.size() > 1)
        return QStringLiteral("The cards' passages are in different source files (%1). A card's passages must be in one file.").arg(files.join(QStringLiteral(", ")));
    if (pdfs.size() > 1 || !validSha256(pdfs.value(0)))
        return QStringLiteral("The cards are placed on different versions of the PDF. Sync and wait for the PDF to be rebuilt, then merge them.");
    const QString relative = files.first();
    const QString root = QFileInfo(m_docsRoot).canonicalFilePath();
    const QString path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    if (root.isEmpty() || path.isEmpty() || !path.startsWith(root + QLatin1Char('/')) || QDir(root).relativeFilePath(path) != relative)
        return QStringLiteral("The cards' source %1 is unavailable.").arg(relative);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return QStringLiteral("The source %1 is unavailable or too large.").arg(relative);
    const QByteArray source = file.readAll();
    file.close();
    // The server applies the merge to the version it has; local edits it lacks would conflict.
    const QString sourceSha = sha256(source);
    QJsonObject owner;
    QString built;
    sourceDocumentOwner(root, relative, bundleRoot, &owner, &built);
    if (built != sourceSha && !placedOn.contains(sourceSha)) {
        if (syncFirst) *syncFirst = true;
        return QStringLiteral("%1 has changes the server does not have yet. Sync, then merge the cards.").arg(relative);
    }
    const QDir transactions(QDir(rootPath()).filePath(QStringLiteral("source-transactions")));
    if (!transactions.entryList({QStringLiteral("merge-*.json")}, QDir::Files).isEmpty())
        return QStringLiteral("An earlier card merge is still awaiting recovery. Restart Thoughts and try again.");
    for (const ReviewCard &card : std::as_const(plan->cards))
        if (QFileInfo::exists(transactions.filePath(card.id() + QStringLiteral(".json"))))
            return QStringLiteral("An earlier source edit of card “%1” is still awaiting recovery. Restart Thoughts and try again.").arg(mergeCardLabel(card));
    // Plan once for the reading order, then again so the deletions follow it.
    const QString provisional = keptId.isEmpty() ? ids.first() : keptId;
    QStringList others = ids;
    others.removeAll(provisional);
    QHash<QString, QString> names;
    for (const ReviewCard &card : std::as_const(plan->cards)) names.insert(card.id(), mergeCardLabel(card));
    auto merge = planReviewCardMergeSource(relative, source, provisional, others, segmentIds, names);
    if (!merge.error.isEmpty()) return merge.error;
    QStringList order;
    for (const QStringList &origin : std::as_const(merge.origins))
        for (const QString &id : origin)
            if (!order.contains(id)) order.append(id);
    if (order.size() != ids.size()) return QStringLiteral("The cards' passages cannot all be found in %1.").arg(relative);
    std::stable_sort(plan->cards.begin(), plan->cards.end(), [&order](const ReviewCard &a, const ReviewCard &b) { return order.indexOf(a.id()) < order.indexOf(b.id()); });
    if (keptId.isEmpty()) keptId = defaultMergeKeptCard(plan->cards);
    QStringList deletions = order;
    deletions.removeAll(keptId);
    if (keptId != provisional || deletions != others) {
        merge = planReviewCardMergeSource(relative, source, keptId, deletions, segmentIds, names);
        if (!merge.error.isEmpty()) return merge.error;
    }
    plan->keptId = keptId;
    plan->others = deletions;
    plan->relative = relative;
    plan->path = path;
    plan->source = source;
    plan->merge = merge;
    return {};
}

QString ReviewStore::cardMergeProblem(const QList<ReviewCard> &cards, const QString &bundleRoot, CardMerge *preview, bool *syncFirst) const
{
    CardMergePlan plan;
    const QString problem = prepareCardMerge(cards, {}, bundleRoot, &plan, syncFirst);
    if (problem.isEmpty() && preview) {
        *preview = {};
        preview->keptId = plan.keptId;
        preview->deletedIds = plan.others;
        for (const ReviewCard &card : std::as_const(plan.cards)) preview->order.append(card.id());
        preview->pdf = plan.pdf;
        if (plan.pdf) {
            for (const ReviewCard &card : std::as_const(plan.cards)) preview->passages += card.regions().size();
        } else {
            preview->sourceFile = plan.relative;
            preview->sourceSha256 = sha256(plan.merge.replacement);
            preview->passages = plan.merge.anchor.value(QStringLiteral("segments")).toArray().size();
        }
    }
    return problem;
}

bool ReviewStore::mergeCards(const QList<ReviewCard> &cards, const QString &keptId, const QString &question, const QString &bundleRoot,
                             QString *error, CardMerge *result)
{
    const auto fail = [error](const QString &reason) { if (error) *error = reason; return false; };
    if (m_readOnly) return fail(QStringLiteral("Cannot write through a review snapshot."));
    if (!validQuestion(question)) return fail(QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength));
    if (keptId.isEmpty()) return fail(QStringLiteral("Choose the card to keep."));
    CardMergePlan plan;
    if (const QString problem = prepareCardMerge(cards, keptId, bundleRoot, &plan); !problem.isEmpty()) return fail(problem);
    const auto cardById = [&plan](const QString &id) {
        return *std::find_if(plan.cards.cbegin(), plan.cards.cend(), [&id](const ReviewCard &card) { return card.id() == id; });
    };
    // The kept card's areas: each card's own, in the order of its first passage.
    QJsonArray regions;
    QStringList answers;
    const ReviewCard *first = nullptr;
    for (const ReviewCard &card : std::as_const(plan.cards)) {
        if (!first) first = &card;
        QVariantList own = card.regions();
        // The kept card's page and page fingerprint are the first card's own page: its areas there
        // lead, before the lines its passage's headings put in the table of contents earlier in the PDF.
        if (&card == first)
            std::stable_partition(own.begin(), own.end(), [&card](const QVariant &region) { return region.toMap().value(QStringLiteral("page")).toInt() == card.page(); });
        for (const QVariant &region : std::as_const(own)) regions.append(QJsonObject::fromVariantMap(region.toMap()));
        const QString answer = primaryObject(card.record).value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString().trimmed();
        if (!answer.isEmpty()) answers.append(answer);
    }
    const auto firstRegion = regions.first().toObject();
    if (!first || firstRegion.value(QStringLiteral("page")).toInt() != first->page() || !validSha256(first->pageVisualSha256()))
        return fail(QStringLiteral("Card “%1” has no verified page on the PDF. Sync and wait for the PDF to be rebuilt, then merge the cards.").arg(first ? mergeCardLabel(*first) : QString()));
    if (plan.pdf) return mergePdfCards(plan, regions, answers, question, error, result);
    const QString base = sha256(plan.source);
    const auto &versions = plan.merge.versions;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonArray records;
    QStringList eventIds;
    for (int index = 0; index < plan.others.size(); ++index) {
        const ReviewCard card = cardById(plan.others.at(index));
        // After the card's own events, whatever this clock says.
        QDateTime at = now;
        for (const auto &prior : card.history) {
            const QDateTime earlier = parseUtc(objectString(prior, QStringLiteral("reviewed_at")));
            if (earlier.isValid() && earlier >= at) at = earlier.addMSecs(1);
        }
        const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        eventIds.append(eventId);
        const QJsonObject event{{QStringLiteral("schema_version"), schemaVersion}, {QStringLiteral("event_id"), eventId},
            {QStringLiteral("card_id"), card.id()}, {QStringLiteral("reviewed_at"), utcString(at)}, {QStringLiteral("updated_at"), utcString(at)},
            {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1},
            {QStringLiteral("rating"), QStringLiteral("delete")}, {QStringLiteral("scheduler_version"), currentSchedulerVersion},
            {QStringLiteral("source_anchor"), primaryObject(plan.records.value(card.id())).value(QStringLiteral("source_anchor")).toObject()},
            {QStringLiteral("merged_into"), plan.keptId},
            {QStringLiteral("source_authority"), QStringLiteral("client-v1")}, {QStringLiteral("marker_removal"), QStringLiteral("complete")},
            {QStringLiteral("source_change"), QJsonObject{{QStringLiteral("transaction_id"), eventId}, {QStringLiteral("path"), plan.relative},
                {QStringLiteral("before_sha256"), sha256(versions.at(index))}, {QStringLiteral("after_sha256"), sha256(versions.at(index + 1))},
                {QStringLiteral("base_sha256"), base}}}};
        records.append(QJsonObject{{QStringLiteral("relative_path"), QStringLiteral("events/mac-okular/%1.json").arg(eventId)}, {QStringLiteral("record"), event}});
    }
    // The kept card, placed as replaceCardAreas places it: its markers move within the file.
    auto card = plan.records.value(plan.keptId);
    auto primary = primaryObject(card);
    primary.remove(QStringLiteral("answer_region_override"));
    primary.remove(QStringLiteral("source_kind"));
    primary.remove(QStringLiteral("component_origin"));
    primary.insert(QStringLiteral("question"), question.trimmed());
    primary.insert(QStringLiteral("pdf_sha256"), first->pdfSha256());
    primary.insert(QStringLiteral("page"), firstRegion.value(QStringLiteral("page")).toInt());
    primary.insert(QStringLiteral("rect"), firstRegion.value(QStringLiteral("rect")).toArray());
    primary.insert(QStringLiteral("regions"), regions);
    primary.insert(QStringLiteral("page_visual_sha256"), first->pageVisualSha256());
    primary.insert(QStringLiteral("page_visual_fingerprint_method"), QString::fromLatin1(reviewFingerprintMethod));
    primary.insert(QStringLiteral("source_anchor"), plan.merge.anchor);
    if (answers.isEmpty()) primary.remove(QStringLiteral("answer_anchor"));
    else {
        const QString text = answers.join(QLatin1Char(' '));
        primary.insert(QStringLiteral("answer_anchor"), QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("normalized_text"), text},
            {QStringLiteral("text_sha256"), sha256(text.toUtf8())}});
    }
    card.insert(QStringLiteral("primary"), primary);
    card.insert(QStringLiteral("schema_version"), 4);
    card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linked")}});
    card.remove(QStringLiteral("reanchor"));
    if (objectString(card, QStringLiteral("status")) == QLatin1String("needs_reanchor")) card.insert(QStringLiteral("status"), QStringLiteral("approved"));
    card = prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card)));
    card.insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
    card.insert(QStringLiteral("source_change"), QJsonObject{{QStringLiteral("transaction_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("path"), plan.relative}, {QStringLiteral("before_sha256"), sha256(versions.constLast())},
        {QStringLiteral("after_sha256"), sha256(plan.merge.replacement)}, {QStringLiteral("base_sha256"), base}});
    records.append(QJsonObject{{QStringLiteral("relative_path"), QStringLiteral("cards/%1.json").arg(plan.keptId)}, {QStringLiteral("record"), card}});

    // Written as one: the source objects, a journal, the source, then every record in one batch.
    const QString journalRoot = QDir(rootPath()).filePath(QStringLiteral("source-transactions"));
    const QString objectRoot = QDir(rootPath()).filePath(QStringLiteral("source-objects"));
    if (!QDir().mkpath(journalRoot) || !QDir().mkpath(objectRoot)) return fail(QStringLiteral("Cannot prepare review source recovery."));
    QLockFile lock(QDir(journalRoot).filePath(QStringLiteral("writer.lock")));
    if (!lock.tryLock(2000)) return fail(QStringLiteral("Another review source edit is in progress."));
    QFile current(plan.path);
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != plan.source) return fail(QStringLiteral("The source changed while the cards were merged. Try again."));
    current.close();
    for (const QByteArray &bytes : versions + QList<QByteArray>{plan.merge.replacement}) {
        QSaveFile object(QDir(objectRoot).filePath(sha256(bytes)));
        if (!object.open(QIODevice::WriteOnly) || object.write(bytes) != bytes.size() || !object.commit())
            return fail(QStringLiteral("Cannot retain the verified source objects."));
    }
    const QString journalPath = QDir(journalRoot).filePath(QStringLiteral("merge-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QJsonObject journal{{QStringLiteral("kind"), QStringLiteral("review_card_merge")}, {QStringLiteral("path"), plan.path},
        {QStringLiteral("before"), QString::fromLatin1(plan.source.toBase64())}, {QStringLiteral("after"), QString::fromLatin1(plan.merge.replacement.toBase64())},
        {QStringLiteral("records"), records}};
    if (!writeJson(journalPath, journal, error)) return false;
    if (!Thoughts::Platform::replaceFileContents(plan.path, plan.merge.replacement)) return fail(QStringLiteral("The merge is saved for recovery."));
    if (!writeServerRecords(records, error)) return false;
    QFile::remove(journalPath);
    if (result) {
        *result = {};
        result->keptId = plan.keptId;
        result->deletedIds = plan.others;
        result->deleteEventIds = eventIds;
        for (const ReviewCard &merged : std::as_const(plan.cards)) result->order.append(merged.id());
        result->sourceFile = plan.relative;
        result->sourceSha256 = sha256(plan.merge.replacement);
        result->passages = plan.merge.anchor.value(QStringLiteral("segments")).toArray().size();
    }
    return true;
}

// Cards of an imported PDF: nothing in a source changes. The kept card takes every card's areas
// (@p regions, in reading order) and the merged question; the others are deleted as Delete card
// deletes them, naming the kept card. One batch, so the server applies all of it or none.
bool ReviewStore::mergePdfCards(const CardMergePlan &plan, const QJsonArray &regions, const QStringList &answers, const QString &question,
                                QString *error, CardMerge *result)
{
    const ReviewCard &first = plan.cards.constFirst();
    const auto firstRegion = regions.first().toObject();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonArray records;
    QStringList eventIds;
    for (const QString &id : plan.others) {
        const ReviewCard &card = *std::find_if(plan.cards.cbegin(), plan.cards.cend(), [&id](const ReviewCard &c) { return c.id() == id; });
        // After the card's own events, whatever this clock says.
        QDateTime at = now;
        for (const auto &prior : card.history) {
            const QDateTime earlier = parseUtc(objectString(prior, QStringLiteral("reviewed_at")));
            if (earlier.isValid() && earlier >= at) at = earlier.addMSecs(1);
        }
        const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        eventIds.append(eventId);
        const QJsonObject event{{QStringLiteral("schema_version"), schemaVersion}, {QStringLiteral("event_id"), eventId},
            {QStringLiteral("card_id"), card.id()}, {QStringLiteral("reviewed_at"), utcString(at)}, {QStringLiteral("updated_at"), utcString(at)},
            {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1},
            {QStringLiteral("rating"), QStringLiteral("delete")}, {QStringLiteral("scheduler_version"), currentSchedulerVersion},
            {QStringLiteral("source_anchor"), primaryObject(plan.records.value(card.id())).value(QStringLiteral("source_anchor")).toObject()},
            {QStringLiteral("merged_into"), plan.keptId}};
        records.append(QJsonObject{{QStringLiteral("relative_path"), QStringLiteral("events/mac-okular/%1.json").arg(eventId)}, {QStringLiteral("record"), event}});
    }
    auto card = plan.records.value(plan.keptId);
    auto primary = primaryObject(card);
    primary.remove(QStringLiteral("answer_region_override"));
    primary.remove(QStringLiteral("source_kind"));
    primary.remove(QStringLiteral("component_origin"));
    primary.insert(QStringLiteral("question"), question.trimmed());
    primary.insert(QStringLiteral("pdf_sha256"), first.pdfSha256());
    primary.insert(QStringLiteral("page"), firstRegion.value(QStringLiteral("page")).toInt());
    primary.insert(QStringLiteral("rect"), firstRegion.value(QStringLiteral("rect")).toArray());
    primary.insert(QStringLiteral("regions"), regions);
    primary.insert(QStringLiteral("page_visual_sha256"), first.pageVisualSha256());
    primary.insert(QStringLiteral("page_visual_fingerprint_method"), QString::fromLatin1(reviewFingerprintMethod));
    if (answers.isEmpty()) primary.remove(QStringLiteral("answer_anchor"));
    else {
        const QString text = answers.join(QLatin1Char(' '));
        primary.insert(QStringLiteral("answer_anchor"), QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("normalized_text"), text},
            {QStringLiteral("text_sha256"), sha256(text.toUtf8())}});
    }
    card.insert(QStringLiteral("primary"), primary);
    card.insert(QStringLiteral("schema_version"), 4);
    card.remove(QStringLiteral("reanchor"));
    if (objectString(card, QStringLiteral("status")) == QLatin1String("needs_reanchor")) card.insert(QStringLiteral("status"), QStringLiteral("approved"));
    card = prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card)));
    records.append(QJsonObject{{QStringLiteral("relative_path"), QStringLiteral("cards/%1.json").arg(plan.keptId)}, {QStringLiteral("record"), card}});
    if (!writeServerRecords(records, error)) return false;
    if (result) {
        *result = {};
        result->keptId = plan.keptId;
        result->deletedIds = plan.others;
        result->deleteEventIds = eventIds;
        for (const ReviewCard &merged : std::as_const(plan.cards)) result->order.append(merged.id());
        result->passages = regions.size();
        result->pdf = true;
    }
    return true;
}

bool ReviewStore::writeServerRecords(const QJsonArray &records, QString *error) const
{
    const auto fail = [error](const QString &reason) { if (error) *error = reason; return false; };
    if (m_readOnly) return fail(QStringLiteral("Cannot write through a review snapshot."));
    QList<QJsonObject> operations;
    for (const auto &value : records) {
        const auto item = value.toObject();
        const QString relativePath = item.value(QStringLiteral("relative_path")).toString();
        const auto object = item.value(QStringLiteral("record")).toObject();
        const bool card = relativePath.startsWith(QLatin1String("cards/"));
        const QString id = objectString(object, card ? QStringLiteral("card_id") : QStringLiteral("event_id"));
        if (QUuid(id).isNull() || relativePath != (card ? QStringLiteral("cards/%1.json") : QStringLiteral("events/mac-okular/%1.json")).arg(id))
            return fail(QStringLiteral("Invalid review record."));
        if (!writeJson(QDir(rootPath()).filePath(relativePath), object, error)) return false;
        const QString timestamp = !objectString(object, QStringLiteral("updated_at")).isEmpty() ? objectString(object, QStringLiteral("updated_at"))
            : !objectString(object, QStringLiteral("reviewed_at")).isEmpty() ? objectString(object, QStringLiteral("reviewed_at")) : objectString(object, QStringLiteral("created_at"));
        QJsonObject operation{{QStringLiteral("entity_type"), card ? QStringLiteral("review_card") : QStringLiteral("review_event")},
            {QStringLiteral("entity_id"), id}, {QStringLiteral("collection"), card ? QStringLiteral("cards") : QStringLiteral("events")},
            {QStringLiteral("client_timestamp"), timestamp}, {QStringLiteral("payload"), object}};
        if (const qint64 base = object.value(QStringLiteral("revision")).toInteger(1) - 1; base >= 0) operation.insert(QStringLiteral("base_revision"), base);
        operations.append(operation);
    }
    // Alternate/test roots have no Fedora authority unless their caller supplied a state directory.
    if (!m_outbox) return true;
    QString queueError;
    if (!m_outbox->ready(&queueError) || m_outbox->enqueueBatch(operations, &queueError).size() != operations.size())
        return fail(QStringLiteral("The review cards were saved locally but could not be queued for the server: %1").arg(queueError));
    return true;
}

bool ReviewStore::updateQuestion(const QString &cardId, const QString &question, QString *error)
{
    const QString normalizedQuestion = question.trimmed();
    if (QUuid(cardId).isNull() || !validQuestion(normalizedQuestion)) {
        if (error) {
            *error = QStringLiteral("The question must contain between 1 and %1 characters.").arg(maximumQuestionLength);
        }
        return false;
    }
    QJsonObject card = cardRecord(cardId, error);
    if (card.isEmpty()) {
        return false;
    }
    QJsonObject primary = primaryObject(card);
    if (objectString(primary, QStringLiteral("question")).trimmed() == normalizedQuestion) {
        return true;
    }
    primary.insert(QStringLiteral("question"), normalizedQuestion);
    card.insert(QStringLiteral("primary"), primary);
    return writeCard(prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card))), error);
}

bool ReviewStore::updateNote(const ReviewCard &expected, const QString &note, QString *error)
{
    QJsonObject card = cardRecord(expected.id(), error);
    if (card.isEmpty()) return false;
    if (note.trimmed().size() > 5000 || card.value(QStringLiteral("note")) != expected.record.value(QStringLiteral("note"))) {
        if (error) *error = QStringLiteral("The note changed during editing, or exceeds 5000 characters. Reopen the editor and try again.");
        return false;
    }
    if (card.value(QStringLiteral("note")).toString() == note.trimmed()) return true;
    card.insert(QStringLiteral("note"), note.trimmed());
    // Notes do not change the source-derived question or PDF geometry.
    return writeServerJson(QStringLiteral("cards/%1.json").arg(expected.id()), prepareCardUpdate(FeedbackVersion::stamped(card, reviewVersion(card))), error);
}

QHash<QString, QString> ReviewStore::projectsByPdfStem() const
{
    const auto index = readObject(QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")), nullptr);
    if (index.value(QStringLiteral("schema_version")).toInt() != 1 || !index.value(QStringLiteral("pdfs")).isArray()) return {};
    QHash<QString, QString> result;
    QSet<QString> filenames;
    for (const auto &value : index.value(QStringLiteral("pdfs")).toArray()) {
        const auto entry = value.toObject();
        const auto filename = entry.value(QStringLiteral("filename")).toString();
        if (!filename.endsWith(QLatin1String(".pdf")) || filename.size() <= 4 || filename.contains(QLatin1Char('/'))
            || filename.contains(QLatin1Char('\\')) || filenames.contains(filename)) return {};
        filenames.insert(filename);
        const QString project = entry.value(QStringLiteral("project")).toString().trimmed();
        if (!project.isEmpty()) result.insert(filename.chopped(4), project);
    }
    return result;
}

bool ReviewStore::saveQueueActions(const QList<QJsonObject> &actions, QString *error)
{
    QJsonArray encoded;
    for (const auto &action : actions) encoded.append(action);
    const QString path = QDir(rootPath()).filePath(QStringLiteral("queue-actions/%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    // Commit the complete batch first; replaying after a failure uses the same event IDs.
    if (!writeJson(path, {{QStringLiteral("actions"), encoded}}, error)) return false;
    flushQueueActions();
    return true;
}

void ReviewStore::flushQueueActions() const
{
    const QDir directory(QDir(rootPath()).filePath(QStringLiteral("queue-actions")));
    for (const QString &filename : directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        const auto manifest = readObject(directory.filePath(filename), nullptr);
        const auto actions = manifest.value(QStringLiteral("actions")).toArray();
        if (actions.isEmpty()) continue;
        bool complete = true;
        for (const auto &value : actions) {
            const auto action = value.toObject();
            const auto id = action.value(QStringLiteral("event_id")).toString();
            QString error;
            if (QUuid(id).isNull() || !writeServerJson(QStringLiteral("events/mac-okular/%1.json").arg(id), action, &error)) {
                complete = false;
                qWarning() << "Review queue action remains staged:" << error;
                break;
            }
        }
        if (complete) QFile::remove(directory.filePath(filename));
    }
}

bool ReviewStore::skip(const QString &cardId, ReviewSkipScope scope, const QDateTime &until, ReviewSkipBatch *batch,
                       QString *error, const QDateTime &now, bool dueOnly)
{
    if (!now.isValid() || !until.isValid() || until <= now) {
        if (error) *error = QStringLiteral("Choose a future time.");
        return false;
    }
    const auto snapshot = cards();
    const auto current = std::find_if(snapshot.cbegin(), snapshot.cend(), [&](const auto &card) { return card.id() == cardId; });
    if (current == snapshot.cend() || !current->isApproved() || current->schedule.deleted || current->schedule.retained || current->schedule.retired) {
        if (error) *error = QStringLiteral("The review card is unavailable.");
        return false;
    }
    const auto projects = projectsByPdfStem();
    const auto project = projects.value(current->pdfStem());
    if (scope == ReviewSkipScope::Project && project.isEmpty()) {
        if (error) *error = QStringLiteral("Project information is unavailable for this document.");
        return false;
    }
    ReviewSkipBatch saved{QUuid::createUuid().toString(QUuid::WithoutBraces), {}, until};
    QList<QJsonObject> actions;
    for (const auto &card : snapshot) {
        if (!card.isApproved() || card.schedule.deleted || card.schedule.retained || card.schedule.retired) continue;
        // Single-card skips remain available for cards opened ahead of schedule.
        if (dueOnly && scope != ReviewSkipScope::Card && card.schedule.reviewed && card.schedule.dueAt > now.toUTC()) continue;
        if (scope == ReviewSkipScope::Card && card.id() != cardId) continue;
        if (scope == ReviewSkipScope::Document && card.pdfStem() != current->pdfStem()) continue;
        if (scope == ReviewSkipScope::Project && projects.value(card.pdfStem()) != project) continue;
        saved.cardIds.append(card.id());
        QJsonObject action{{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                           {QStringLiteral("card_id"), card.id()}, {QStringLiteral("skip_id"), saved.id}, {QStringLiteral("queue_action"), QStringLiteral("skip")},
                           {QStringLiteral("skip_until"), utcString(until)}, {QStringLiteral("reviewed_at"), utcString(now)},
                           {QStringLiteral("updated_at"), utcString(now)}, {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1}};
        if (const QJsonObject version = reviewVersion(card.record); !version.isEmpty()) action.insert(reviewVersionField(), version);
        actions.append(action);
    }
    if (!saveQueueActions(actions, error)) return false;
    if (batch) *batch = saved;
    return true;
}

bool ReviewStore::undoSkip(const QString &skipId, QString *error, const QDateTime &now)
{
    QSet<QString> cardIds;
    for (const auto &event : events(nullptr)) {
        if (event.value(QStringLiteral("skip_id")).toString() == skipId
            && event.value(QStringLiteral("queue_action")).toString() == QLatin1String("skip")) cardIds.insert(event.value(QStringLiteral("card_id")).toString());
    }
    if (QUuid(skipId).isNull() || cardIds.isEmpty() || !now.isValid()) {
        if (error) *error = QStringLiteral("The skip is unavailable.");
        return false;
    }
    QList<QJsonObject> actions;
    for (const auto &cardId : cardIds) {
        actions.append({{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("card_id"), cardId}, {QStringLiteral("skip_id"), skipId}, {QStringLiteral("queue_action"), QStringLiteral("undo_skip")},
                        {QStringLiteral("reviewed_at"), now.toString(Qt::ISODateWithMs)}, {QStringLiteral("updated_at"), now.toString(Qt::ISODateWithMs)},
                        {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()}, {QStringLiteral("revision"), 1}});
    }
    return saveQueueActions(actions, error);
}

bool ReviewStore::recordRetireAction(const QString &cardId, const QString &action, const QString &undoneEventId, const QDateTime &now, QString *error,
                                     QString *recordedEventId)
{
    if (m_readOnly || QUuid(cardId).isNull() || !now.isValid()) {
        if (error) *error = QStringLiteral("Invalid review card operation.");
        return false;
    }
    const auto snapshot = cards();
    const auto card = std::find_if(snapshot.cbegin(), snapshot.cend(), [&cardId](const ReviewCard &item) { return item.id() == cardId; });
    if (card == snapshot.cend() || card->schedule.deleted) {
        if (error) *error = QStringLiteral("The review card is unavailable.");
        return false;
    }
    // Last writer wins by reviewed_at (ReviewRetire::fold): come after the card's earlier actions,
    // even when this clock is behind the device that recorded them.
    QDateTime at = now.toUTC();
    for (const auto &event : card->history) {
        if (!ReviewRetire::isEvent(event)) continue;
        const QDateTime earlier = parseUtc(objectString(event, QStringLiteral("reviewed_at")));
        if (earlier.isValid() && earlier >= at) at = earlier.addMSecs(1);
    }
    const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject event{{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), eventId}, {QStringLiteral("card_id"), cardId},
                      {QStringLiteral("queue_action"), action}, {QStringLiteral("reviewed_at"), at.toString(Qt::ISODateWithMs)},
                      {QStringLiteral("updated_at"), at.toString(Qt::ISODateWithMs)}, {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
                      {QStringLiteral("revision"), 1}};
    if (!undoneEventId.isEmpty()) event.insert(ReviewRetire::undoneField(), undoneEventId);
    if (const QJsonObject version = reviewVersion(card->record); !version.isEmpty()) event.insert(reviewVersionField(), version);
    if (!saveQueueActions({event}, error)) return false;
    if (recordedEventId) *recordedEventId = eventId;
    return true;
}

bool ReviewStore::retireCard(const QString &cardId, QString *error, const QDateTime &now, QString *recordedEventId)
{
    return recordRetireAction(cardId, ReviewRetire::retireAction(), {}, now, error, recordedEventId);
}

bool ReviewStore::restoreRetiredCard(const QString &cardId, QString *error, const QDateTime &now, QString *recordedEventId)
{
    return recordRetireAction(cardId, ReviewRetire::restoreAction(), {}, now, error, recordedEventId);
}

bool ReviewStore::undoRetire(const QString &eventId, QString *error, const QDateTime &now)
{
    QString cardId;
    for (const auto &event : events(nullptr)) {
        const QString action = objectString(event, QStringLiteral("queue_action"));
        if (objectString(event, QStringLiteral("event_id")) == eventId && (action == ReviewRetire::retireAction() || action == ReviewRetire::restoreAction())) {
            cardId = objectString(event, QStringLiteral("card_id"));
            break;
        }
    }
    if (QUuid(eventId).isNull() || cardId.isEmpty()) {
        if (error) *error = QStringLiteral("There is nothing to undo.");
        return false;
    }
    return recordRetireAction(cardId, ReviewRetire::undoAction(), eventId, now, error, nullptr);
}

bool ReviewStore::setCardsDeleted(const QStringList &cardIds, bool deleted, const QDateTime &now, QString *error)
{
    if (!deleted) {
        if (error) *error = QStringLiteral("Deleted review cards cannot be restored.");
        return false;
    }
    if (m_readOnly || cardIds.isEmpty() || !now.isValid()) {
        if (error) *error = QStringLiteral("Invalid review card operation.");
        return false;
    }
    // Validate the entire selection against one snapshot before writing anything.
    QHash<QString, ReviewCard> snapshot;
    for (const auto &card : cards()) snapshot.insert(card.id(), card);
    QList<QJsonObject> actions;
    QSet<QString> seen;
    for (const auto &id : cardIds) {
        if (seen.contains(id)) continue;
        seen.insert(id);
        const auto found = snapshot.constFind(id);
        if (found == snapshot.cend() || QUuid(id).isNull() || found->schedule.deleted == deleted
            || (!deleted && found->schedule.deleteEventId.isEmpty())) {
            if (error) *error = QStringLiteral("A selected review card is unavailable or its deletion state changed. Refresh and try again.");
            return false;
        }
        QJsonObject event {
            {QStringLiteral("schema_version"), schemaVersion},
            {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("card_id"), id},
            {QStringLiteral("reviewed_at"), utcString(now)},
            {QStringLiteral("updated_at"), utcString(now)},
            {QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId()},
            {QStringLiteral("revision"), 1},
        };
        if (deleted) {
            event.insert(QStringLiteral("rating"), QStringLiteral("delete"));
            event.insert(QStringLiteral("source_anchor"), primaryObject(found->record).value(QStringLiteral("source_anchor")).toObject());
            event.insert(QStringLiteral("scheduler_version"), currentSchedulerVersion);
        } else {
            event.insert(QStringLiteral("retracted_event_id"), found->schedule.deleteEventId);
        }
        actions.append(event);
    }
    return saveQueueActions(actions, error);
}

bool ReviewStore::deleteCard(const QString &cardId, const QDateTime &deletedAt, QString *error)
{
    return setCardsDeleted({cardId}, true, deletedAt, error);
}

bool ReviewStore::restoreCard(const QString &cardId, const QDateTime &restoredAt, QString *error)
{
    return setCardsDeleted({cardId}, false, restoredAt, error);
}

bool ReviewStore::excludePage(const QString &pageVisualSha256, int page, const QString &pdfStem, QString *error)
{
    if (!validSha256(pageVisualSha256) || page < 1 || pdfStem.isEmpty()) {
        if (error) {
            *error = QStringLiteral("A valid visual page anchor is required.");
        }
        return false;
    }
    const QString id = sha256((pageVisualSha256 + QLatin1Char('\n') + pdfStem).toUtf8()).left(32);
    QJsonObject exclusion;
    exclusion.insert(QStringLiteral("schema_version"), schemaVersion);
    exclusion.insert(QStringLiteral("exclusion_id"), id);
    exclusion.insert(QStringLiteral("page_visual_sha256"), pageVisualSha256);
    exclusion.insert(QStringLiteral("pdf_stem"), pdfStem);
    exclusion.insert(QStringLiteral("page"), page);
    exclusion.insert(QStringLiteral("created_at"), utcString(QDateTime::currentDateTimeUtc()));
    exclusion.insert(QStringLiteral("updated_at"), objectString(exclusion, QStringLiteral("created_at")));
    exclusion.insert(QStringLiteral("origin_id"), Thoughts::Platform::reviewOriginId());
    exclusion.insert(QStringLiteral("revision"), 1);
    return writeServerJson(QStringLiteral("exclusions/%1.json").arg(id), exclusion, error);
}

QJsonObject ReviewStore::agentRequest(const QVariantMap &document,
                                      const QString &agentModel,
                                      const QString &agentReasoningEffort,
                                      int maxCandidates,
                                      const QString &agentInstruction) const
{
    QJsonArray avoidRegions;
    for (const ReviewCard &card : cards()) {
        if (card.pdfStem() == document.value(QStringLiteral("pdf_stem")).toString()) {
            QJsonObject summary;
            summary.insert(QStringLiteral("page_visual_sha256"), card.pageVisualSha256());
            summary.insert(QStringLiteral("page"), card.page());
            summary.insert(QStringLiteral("rect"), primaryObject(card.record).value(QStringLiteral("rect")));
            avoidRegions.append(summary);
        }
    }
    QJsonArray exclusionRecords;
    for (const QJsonObject &exclusion : exclusions()) {
        exclusionRecords.append(exclusion);
    }
    QJsonObject request;
    request.insert(QStringLiteral("schema_version"), schemaVersion);
    request.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    request.insert(QStringLiteral("docs_root"), m_docsRoot);
    request.insert(QStringLiteral("document"), QJsonObject::fromVariantMap(document));
    request.insert(QStringLiteral("avoid_regions"), avoidRegions);
    request.insert(QStringLiteral("exclusions"), exclusionRecords);
    request.insert(QStringLiteral("max_candidates"), maxCandidates);
    const QString normalizedAgentModel = agentModel.trimmed();
    if (!normalizedAgentModel.isEmpty()) {
        request.insert(QStringLiteral("agent_model"), normalizedAgentModel);
    }
    const QString normalizedAgentReasoningEffort = agentReasoningEffort.trimmed();
    if (!normalizedAgentReasoningEffort.isEmpty()) {
        request.insert(QStringLiteral("agent_reasoning_effort"), normalizedAgentReasoningEffort);
    }
    const QString normalizedAgentInstruction = agentInstruction.trimmed();
    if (!normalizedAgentInstruction.isEmpty()) {
        request.insert(QStringLiteral("agent_instruction"), normalizedAgentInstruction);
    }
    return request;
}

}
