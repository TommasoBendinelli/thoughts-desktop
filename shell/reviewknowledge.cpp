// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewknowledge.h"

#include "reviewdocumenttools.h"
#include "../part/documentfeedback.h"
#include "../part/libraryprojectstore.h"
#include "../part/pdfversions.h"
#include "../part/reviewcardfeedback.h"
#include "../part/thoughtsplatform.h"

#include <poppler-qt6.h>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Thoughts::ReviewKnowledge
{
namespace
{
constexpr int defaultLimit = 20;
constexpr int defaultProfileLimit = 40;
constexpr int maximumLimit = 200;
constexpr int excerptLength = 600;
constexpr int maximumTextLength = 2000;

// ---------------------------------------------------------------- reading the index

struct Stamp {
    qint64 size = -1;
    qint64 modified = 0;
    bool operator==(const Stamp &) const = default;
};

Stamp stampOf(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? Stamp{info.size(), info.lastModified().toMSecsSinceEpoch()} : Stamp{};
}

bool readDatabase(const QString &uri, IndexSnapshot *snapshot, QString *error)
{
    const QString connection = QStringLiteral("thoughts-review-knowledge-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=2000"));
        database.setDatabaseName(uri);
        if (!database.open()) {
            *error = database.lastError().text();
        } else {
            ok = [&] {
                QSqlQuery query(database);
                const auto run = [&](const QString &statement) {
                    if (query.exec(statement)) return true;
                    *error = query.lastError().text();
                    return false;
                };
                const auto object = [&](int column) { return QJsonDocument::fromJson(query.value(column).toString().toUtf8()).object(); };
                // One read transaction, so every table comes from the same refresh.
                if (!run(QStringLiteral("BEGIN"))) return false;
                IndexSnapshot result;
                if (!run(QStringLiteral("SELECT card_id, card_json FROM review_cards"))) return false;
                while (query.next()) result.projections.insert(QStringLiteral("review_card\ncards\n") + query.value(0).toString(), object(1));
                if (!run(QStringLiteral("SELECT event_id, event_json FROM review_events"))) return false;
                while (query.next()) result.projections.insert(QStringLiteral("review_event\nevents\n") + query.value(0).toString(), object(1));
                if (!run(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='review_tag_records'"))) return false;
                result.hasTagRecords = query.next() && query.value(0).toInt() > 0;
                if (result.hasTagRecords) {
                    if (!run(QStringLiteral("SELECT entity_type, entity_id, record_json FROM review_tag_records"))) return false;
                    while (query.next()) {
                        result.projections.insert(query.value(0).toString() + QLatin1Char('\n') + ReviewTags::mergeCollection() + QLatin1Char('\n')
                                                      + query.value(1).toString(),
                                                  object(2));
                    }
                }
                // Answer feedback; an index written before review_feedback has none.
                if (!run(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='review_feedback'"))) return false;
                if (query.next() && query.value(0).toInt() > 0) {
                    if (!run(QStringLiteral("SELECT feedback_id, record_json FROM review_feedback"))) return false;
                    while (query.next()) result.projections.insert(ReviewCardFeedback::projectionKey(query.value(0).toString()), object(1));
                }
                // Document feedback; an index written before document_feedback has none.
                if (!run(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='document_feedback'"))) return false;
                if (query.next() && query.value(0).toInt() > 0) {
                    if (!run(QStringLiteral("SELECT feedback_id, record_json FROM document_feedback"))) return false;
                    while (query.next()) result.projections.insert(DocumentFeedback::projectionKey(query.value(0).toString()), object(1));
                }
                if (run(QStringLiteral("SELECT value FROM review_meta WHERE key='indexed_at_utc'")) && query.next())
                    result.indexedAt = query.value(0).toString();
                query.finish();
                query.exec(QStringLiteral("COMMIT"));
                *snapshot = result;
                return true;
            }();
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

// ---------------------------------------------------------------- words and matching

QStringList wordsOf(const QString &value)
{
    static const QRegularExpression separator(QStringLiteral("[^\\p{L}\\p{N}]+"));
    return value.toCaseFolded().split(separator, Qt::SkipEmptyParts);
}

const QSet<QString> &stopWords()
{
    static const QSet<QString> words{QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"), QStringLiteral("are"), QStringLiteral("as"),
        QStringLiteral("at"), QStringLiteral("be"), QStringLiteral("by"), QStringLiteral("do"), QStringLiteral("does"), QStringLiteral("for"),
        QStringLiteral("from"), QStringLiteral("how"), QStringLiteral("in"), QStringLiteral("is"), QStringLiteral("it"), QStringLiteral("its"),
        QStringLiteral("of"), QStringLiteral("on"), QStringLiteral("or"), QStringLiteral("that"), QStringLiteral("the"), QStringLiteral("this"),
        QStringLiteral("to"), QStringLiteral("what"), QStringLiteral("when"), QStringLiteral("which"), QStringLiteral("why"), QStringLiteral("with")};
    return words;
}

// Query words: case-insensitive, without stop words unless nothing else is left.
QStringList queryTokens(const QString &query)
{
    QStringList all, meaningful;
    for (const QString &word : wordsOf(query)) {
        if (all.contains(word)) continue;
        all.append(word);
        if (!stopWords().contains(word) && (word.size() > 1 || word.at(0).isDigit())) meaningful.append(word);
    }
    return meaningful.isEmpty() ? all : meaningful;
}

// A word matches a query word it equals or starts with ("advantage" finds "advantages"), and
// a plural or inflected query word finds its stem ("advantages" finds "advantage").
bool wordMatches(const QString &word, const QString &token)
{
    if (word == token) return true;
    if (token.size() >= 3 && word.startsWith(token)) return true;
    return word.size() >= 4 && token.startsWith(word) && token.size() - word.size() <= 2;
}

bool fieldMatches(const QStringList &words, const QString &token)
{
    return std::any_of(words.begin(), words.end(), [&](const QString &word) { return wordMatches(word, token); });
}

qsizetype occurrences(const QStringList &words, const QString &token)
{
    return std::count_if(words.begin(), words.end(), [&](const QString &word) { return wordMatches(word, token); });
}

// Whether the query words appear one after another.
bool phraseMatches(const QStringList &words, const QStringList &tokens)
{
    if (tokens.size() < 2) return false;
    for (int start = 0; start + tokens.size() <= words.size(); ++start) {
        bool all = true;
        for (int i = 0; i < tokens.size() && all; ++i) all = wordMatches(words.at(start + i), tokens.at(i));
        if (all) return true;
    }
    return false;
}

// At most excerptLength characters, starting near the first query word when it is further in.
QString excerptOf(const QString &value, const QStringList &tokens, bool *truncated)
{
    QString answer = value.trimmed();
    *truncated = answer.size() > excerptLength;
    if (!*truncated) return answer;
    qsizetype start = 0;
    static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{N}]+"));
    auto matches = word.globalMatch(answer);
    while (matches.hasNext() && !tokens.isEmpty()) {
        const auto match = matches.next();
        const QString folded = match.captured().toCaseFolded();
        if (std::any_of(tokens.begin(), tokens.end(), [&](const QString &token) { return wordMatches(folded, token); })) {
            if (match.capturedStart() > excerptLength * 3 / 4) start = match.capturedStart() - excerptLength / 4;
            break;
        }
    }
    if (start > 0) {
        while (start < answer.size() && !answer.at(start).isSpace()) ++start;
    }
    qsizetype end = std::min<qsizetype>(answer.size(), start + excerptLength - 2);
    if (end < answer.size()) {
        qsizetype space = end;
        while (space > start + excerptLength / 2 && !answer.at(space).isSpace()) --space;
        if (space > start + excerptLength / 2) end = space;
    }
    QString result = answer.mid(start, end - start).trimmed();
    if (start > 0) result.prepend(QStringLiteral("…"));
    if (end < answer.size()) result.append(QStringLiteral("…"));
    return result;
}

// ---------------------------------------------------------------- cards

QString tagLabel(const QString &tag)
{
    if (tag == ReviewTags::correct()) return QStringLiteral("Correct");
    if (tag == ReviewTags::wrong()) return QStringLiteral("Wrong");
    return tag;
}

QStringList tagLabels(const QStringList &tags)
{
    QStringList labels;
    for (const QString &tag : tags) labels.append(tagLabel(tag));
    return labels;
}

QJsonObject primaryOf(const ReviewCard &card)
{
    return card.record.value(QStringLiteral("primary")).toObject();
}

// Where the card's markers may be: as "Copy for agent" looks for them.
QStringList sourceFilesOf(const ReviewCard &card)
{
    QStringList files;
    const auto add = [&files](const QString &file) {
        const QString trimmed = file.trimmed();
        if (!trimmed.isEmpty() && !files.contains(trimmed)) files.append(trimmed);
    };
    const QJsonObject primary = primaryOf(card);
    for (const auto &value : primary.value(QStringLiteral("source_files")).toArray()) add(value.toString());
    const QString anchorKey = primary.contains(QStringLiteral("source_anchor")) ? QStringLiteral("source_anchor") : QStringLiteral("source_selection");
    for (const auto &value : primary.value(anchorKey).toObject().value(QStringLiteral("segments")).toArray())
        add(value.toObject().value(QStringLiteral("source_file")).toString());
    add(card.linkingSource());
    return files;
}

struct Entry {
    const ReviewCard *card = nullptr;
    QString pdfFilename;
    QString documentName;
    QString documentId;
    QString project;
    QStringList currentTags;
    QList<ReviewCardSourcePassage> passages;
    QString answer;
    QString answerFrom;
    // Words of each searchable field.
    QStringList question, note, names, tags, answerWords;
    double score = 0;
    QStringList matched;
};

QJsonObject projectOfPdf(const QJsonArray &projects, const QString &filename, QJsonObject *document)
{
    for (const auto &projectValue : projects) {
        const auto project = projectValue.toObject();
        for (const auto &documentValue : project.value(QStringLiteral("documents")).toArray()) {
            const auto entry = documentValue.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == filename || entry.value(QStringLiteral("attachment_filename")).toString() == filename) {
                if (document) *document = entry;
                return project;
            }
        }
    }
    return {};
}

QHash<QString, QString> groupNamesById(const QList<ReviewTags::TagGroup> &groups)
{
    QHash<QString, QString> names;
    for (const auto &group : groups) names.insert(group.id, group.name);
    names.insert(ReviewTags::otherGroupNoteKey(), ReviewTags::otherGroupName());
    return names;
}

QJsonObject groupNotesJson(const QJsonObject &event, const QHash<QString, QString> &groupNames)
{
    QJsonObject notes;
    const auto recorded = ReviewTags::groupNotesOfEvent(event);
    for (auto it = recorded.cbegin(); it != recorded.cend(); ++it) {
        // The answer's own notes are not a group's: they go in the answer's "note" and "before_note".
        if (it.key() != ReviewTags::answerNoteKey() && it.key() != ReviewTags::beforeNoteKey())
            notes.insert(groupNames.value(it.key(), it.key()), it.value());
    }
    return notes;
}

std::optional<double> intervalDaysOf(const QJsonObject &event)
{
    const auto at = QDateTime::fromString(event.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODateWithMs);
    const auto due = QDateTime::fromString(event.value(QStringLiteral("scheduled_due_at")).toString(), Qt::ISODateWithMs);
    if (!at.isValid() || !due.isValid() || due <= at) return std::nullopt;
    return std::round(at.msecsTo(due) / 8640000.0) / 10.0;
}

QJsonValue isoOrNull(const QDateTime &value)
{
    return value.isValid() ? QJsonValue(value.toUTC().toString(Qt::ISODateWithMs)) : QJsonValue(QJsonValue::Null);
}

struct Schedule {
    bool due = false;
    bool retained = false;
    bool retired = false; // Never offered for review (ReviewRetire); its history still counts.
    bool unpublished = false; // Its document is unpublished: offered for nothing, schedule kept.
    QDateTime dueAt;
    int overdueDays = 0; // Whole days since the due day, when it was before today.
};

Schedule scheduleOf(const ReviewCard &card, const QDateTime &now)
{
    Schedule schedule;
    schedule.retained = card.schedule.retained;
    schedule.retired = card.schedule.retired;
    schedule.unpublished = card.schedule.unpublished;
    schedule.dueAt = card.skippedUntil.isValid() && card.skippedUntil > card.schedule.dueAt ? card.skippedUntil : card.schedule.dueAt;
    schedule.due = card.isApproved() && !schedule.retained && !schedule.retired && !schedule.unpublished && ReviewStore::isDueNow(card, now);
    if (schedule.due && schedule.dueAt.isValid()) {
        const QDate dueDay = schedule.dueAt.toLocalTime().date();
        const QDate today = now.toLocalTime().date();
        schedule.overdueDays = dueDay < today ? int(dueDay.daysTo(today)) : 0;
    }
    return schedule;
}

int wrongCountOf(const ReviewCard &card)
{
    int count = 0;
    // Wrong may have been merged into another tag (e.g. Forgot); count answers under that name.
    const QString wrong = ReviewTags::resolve(ReviewTags::wrong(), card.tagMerges);
    for (const auto &answer : card.answers()) count += card.tagsOfEvent(answer).contains(wrong) ? 1 : 0;
    return count;
}

// ---------------------------------------------------------------- requests

struct Query {
    QString text;
    QStringList tokens;
    QStringList tags;      // Effective tag names, case-folded.
    QStringList tagGroups; // As given.
    QString project, document, state, sort;
    int limit = defaultLimit;
    bool includeAnswer = true;
    // The whole answer instead of an excerpt of excerptLength characters.
    bool fullAnswer = false;
    bool includeHistory = true;
    // Answer feedback: deleted records too, with deleted_at (for the user study).
    bool includeDeleted = false;
};

bool optionalString(const QJsonObject &request, const QString &key, QString *value, QString *problem)
{
    const QJsonValue field = request.value(key);
    if (field.isUndefined() || field.isNull()) return true;
    if (!field.isString() || field.toString().size() > maximumTextLength) {
        *problem = QStringLiteral("%1 must be a string of at most %2 characters.").arg(key).arg(maximumTextLength);
        return false;
    }
    *value = field.toString().trimmed();
    return true;
}

bool optionalStrings(const QJsonObject &request, const QString &key, QStringList *values, QString *problem)
{
    const QJsonValue field = request.value(key);
    if (field.isUndefined() || field.isNull()) return true;
    const QJsonArray array = field.isString() ? QJsonArray{field} : field.toArray();
    if ((!field.isString() && !field.isArray()) || array.size() > 100) {
        *problem = QStringLiteral("%1 must be a list of at most 100 strings.").arg(key);
        return false;
    }
    for (const auto &value : array) {
        if (!value.isString() || value.toString().trimmed().isEmpty() || value.toString().size() > 1000) {
            *problem = QStringLiteral("%1 must be a list of non-empty strings.").arg(key);
            return false;
        }
        values->append(value.toString().trimmed());
    }
    return true;
}

bool optionalBool(const QJsonObject &request, const QString &key, bool *value, QString *problem)
{
    const QJsonValue field = request.value(key);
    if (field.isUndefined() || field.isNull()) return true;
    if (!field.isBool()) {
        *problem = QStringLiteral("%1 must be true or false.").arg(key);
        return false;
    }
    *value = field.toBool();
    return true;
}

bool optionalLimit(const QJsonObject &request, int *value, QString *problem)
{
    const QJsonValue field = request.value(QStringLiteral("limit"));
    if (field.isUndefined() || field.isNull()) return true;
    const double number = field.toDouble(-1);
    if (!field.isDouble() || number != std::floor(number) || number < 1 || number > maximumLimit) {
        *problem = QStringLiteral("limit must be a whole number from 1 to %1.").arg(maximumLimit);
        return false;
    }
    *value = int(number);
    return true;
}

bool onlyKeys(const QJsonObject &request, const QStringList &allowed, QString *problem)
{
    for (auto it = request.constBegin(); it != request.constEnd(); ++it) {
        if (!allowed.contains(it.key()) && it.key() != QLatin1String("command") && it.key() != QLatin1String("schema_version")) {
            *problem = QStringLiteral("Unknown field %1; expected %2.").arg(it.key(), allowed.join(QStringLiteral(", ")));
            return false;
        }
    }
    return true;
}

// The cards that pass the filters and match the query, best first; empty error when fine.
QList<Entry> findCards(const Library &library, const Query &query, const QDateTime &now, QJsonObject *failure)
{
    // Library documents and projects, for names and the project/document filters.
    QJsonObject projectFilter;
    if (!query.project.isEmpty()) {
        QStringList names;
        for (const auto &value : library.projects) {
            const auto project = value.toObject();
            const QString name = project.value(QStringLiteral("name")).toString().trimmed();
            names.append(name);
            if (name.compare(query.project, Qt::CaseInsensitive) == 0 || project.value(QStringLiteral("id")).toString() == query.project)
                projectFilter = project;
        }
        if (projectFilter.isEmpty()) {
            *failure = error(QStringLiteral("unknown_project"), QStringLiteral("No Library project is called \"%1\".").arg(query.project));
            failure->insert(QStringLiteral("projects"), QJsonArray::fromStringList(names));
            return {};
        }
    }
    QSet<QString> projectFiles;
    for (const auto &value : projectFilter.value(QStringLiteral("documents")).toArray()) {
        projectFiles.insert(value.toObject().value(QStringLiteral("filename")).toString());
        projectFiles.insert(value.toObject().value(QStringLiteral("attachment_filename")).toString());
    }
    projectFiles.remove(QString());

    QSet<QString> groupTags;
    if (!query.tagGroups.isEmpty()) {
        QSet<QString> grouped;
        for (const auto &group : library.groups) for (const auto &tag : group.tags) grouped.insert(tag);
        for (const QString &wanted : query.tagGroups) {
            bool known = false;
            for (const auto &group : library.groups) {
                if (group.name.compare(wanted, Qt::CaseInsensitive) != 0 && group.id != wanted) continue;
                known = true;
                for (const auto &tag : group.tags) groupTags.insert(tag.toCaseFolded());
            }
            if (wanted.compare(ReviewTags::otherGroupName(), Qt::CaseInsensitive) == 0) {
                known = true;
                for (const auto &card : library.cards) {
                    for (const auto &answer : card.answers())
                        for (const auto &tag : card.tagsOfEvent(answer)) if (!grouped.contains(tag)) groupTags.insert(tag.toCaseFolded());
                }
            }
            if (!known) {
                QStringList names;
                for (const auto &group : library.groups) names.append(group.name);
                *failure = error(QStringLiteral("unknown_tag_group"), library.groups.isEmpty()
                    ? QStringLiteral("No tag groups are known yet; Thoughts stores them in the review index after its next update.")
                    : QStringLiteral("No tag group is called \"%1\".").arg(wanted));
                failure->insert(QStringLiteral("tag_groups"), QJsonArray::fromStringList(names));
                return {};
            }
        }
    }

    const QString document = query.document.toCaseFolded();
    bool documentKnown = document.isEmpty();
    QList<Entry> entries;
    QHash<QString, QString> sourceCache;
    for (const auto &card : library.cards) {
        Entry entry;
        entry.card = &card;
        entry.pdfFilename = card.pdfStem() + QStringLiteral(".pdf");
        QJsonObject libraryDocument;
        const QJsonObject project = projectOfPdf(library.projects, entry.pdfFilename, &libraryDocument);
        entry.project = project.value(QStringLiteral("name")).toString().trimmed();
        entry.documentName = libraryDocument.value(QStringLiteral("name")).toString().trimmed();
        if (entry.documentName.isEmpty()) entry.documentName = card.pdfStem();
        entry.documentId = primaryOf(card).value(QStringLiteral("document_id")).toString();
        if (!document.isEmpty()) {
            const QStringList names{entry.pdfFilename, card.pdfStem(), entry.documentName, entry.documentId};
            if (std::none_of(names.begin(), names.end(), [&](const QString &name) { return name.toCaseFolded() == document; })) continue;
            documentKnown = true;
        }
        if (!projectFilter.isEmpty() && !projectFiles.contains(entry.pdfFilename)) continue;
        entry.currentTags = card.currentTags();
        if (!query.tags.isEmpty() && std::none_of(entry.currentTags.begin(), entry.currentTags.end(), [&](const QString &tag) {
                return query.tags.contains(tag.toCaseFolded());
            })) continue;
        if (!query.tagGroups.isEmpty() && std::none_of(entry.currentTags.begin(), entry.currentTags.end(), [&](const QString &tag) {
                return groupTags.contains(tag.toCaseFolded());
            })) continue;
        if (!query.state.isEmpty()) {
            const Schedule schedule = scheduleOf(card, now);
            const bool answered = card.wasAnswered();
            const bool keep = query.state == QLatin1String("retired") ? schedule.retired
                : query.state == QLatin1String("active") ? !schedule.retired && !schedule.unpublished
                : query.state == QLatin1String("new") ? !answered && !schedule.retired && !schedule.unpublished
                : query.state == QLatin1String("reviewed") ? answered
                : query.state == QLatin1String("untagged_last") ? answered && entry.currentTags.isEmpty()
                : query.state == QLatin1String("due") ? schedule.due
                : schedule.due && schedule.overdueDays > 0;
            if (!keep) continue;
        }
        entry.passages = reviewCardSourcePassages(library.docsRoot, card.id(), sourceFilesOf(card), &sourceCache);
        QStringList texts;
        for (const auto &passage : std::as_const(entry.passages)) for (const auto &segment : passage.segments) texts.append(segment.text);
        if (!texts.isEmpty()) {
            entry.answer = texts.join(QStringLiteral("\n\n"));
            entry.answerFrom = QStringLiteral("source");
        } else {
            entry.answer = primaryOf(card).value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString();
            if (!entry.answer.isEmpty()) entry.answerFrom = QStringLiteral("answer_anchor");
        }
        entry.question = wordsOf(card.question());
        QStringList notes{card.record.value(QStringLiteral("note")).toString()};
        for (const auto &answer : card.answers()) notes += ReviewTags::groupNotesOfEvent(answer).values();
        entry.note = wordsOf(notes.join(QLatin1Char(' ')));
        entry.names = wordsOf(QStringList{entry.documentName, card.pdfStem(), entry.project}.join(QLatin1Char(' ')));
        entry.tags = wordsOf(entry.currentTags.join(QLatin1Char(' ')));
        entry.answerWords = wordsOf(entry.answer);
        entries.append(entry);
    }
    if (!documentKnown) {
        bool inLibrary = false;
        for (const auto &projectValue : library.projects) {
            for (const auto &value : projectValue.toObject().value(QStringLiteral("documents")).toArray()) {
                const auto entry = value.toObject();
                for (const QString &key : {QStringLiteral("filename"), QStringLiteral("name"), QStringLiteral("document_id")}) {
                    QString name = entry.value(key).toString().toCaseFolded();
                    inLibrary = inLibrary || name == document || (key == QLatin1String("filename") && name.chopped(std::min<qsizetype>(4, name.size())) == document);
                }
            }
        }
        if (!inLibrary) {
            *failure = error(QStringLiteral("unknown_document"),
                             QStringLiteral("No Library document is called \"%1\"; give its PDF filename, name or document ID.").arg(query.document));
            return {};
        }
    }

    if (!query.tokens.isEmpty()) {
        // Rarer words count more (BM25's inverse document frequency); a word in the question
        // counts more than one in the answer.
        const double count = entries.size();
        QHash<QString, double> rarity;
        for (const QString &token : query.tokens) {
            int found = 0;
            for (const auto &entry : std::as_const(entries)) {
                found += fieldMatches(entry.question, token) || fieldMatches(entry.note, token) || fieldMatches(entry.names, token)
                    || fieldMatches(entry.tags, token) || fieldMatches(entry.answerWords, token);
            }
            rarity.insert(token, std::log(1.0 + (count - found + 0.5) / (found + 0.5)));
        }
        const QList<QPair<QString, double>> weights{{QStringLiteral("question"), 3.0}, {QStringLiteral("note"), 2.0},
            {QStringLiteral("document"), 1.5}, {QStringLiteral("tags"), 1.5}, {QStringLiteral("answer"), 1.0}};
        for (auto &entry : entries) {
            const QList<const QStringList *> fields{&entry.question, &entry.note, &entry.names, &entry.tags, &entry.answerWords};
            int found = 0;
            double score = 0;
            QSet<QString> matched;
            for (const QString &token : query.tokens) {
                double best = 0;
                qsizetype seen = 0;
                for (int i = 0; i < fields.size(); ++i) {
                    const qsizetype count = occurrences(*fields.at(i), token);
                    if (!count) continue;
                    seen += count;
                    matched.insert(weights.at(i).first);
                    best = std::max(best, weights.at(i).second);
                }
                if (best > 0) {
                    ++found;
                    // Mentioned again and again: a little more about it.
                    score += rarity.value(token) * best * (1 + 0.15 * std::log(double(seen)));
                }
            }
            if (!found) continue;
            score *= double(found) / query.tokens.size();
            if (phraseMatches(entry.question, query.tokens) || phraseMatches(entry.answerWords, query.tokens)) score *= 1.25;
            entry.score = std::round(score * 1000) / 1000;
            for (const auto &weight : weights) if (matched.contains(weight.first)) entry.matched.append(weight.first);
        }
        entries.removeIf([](const Entry &entry) { return entry.score <= 0; });
    }

    const QString sort = query.sort.isEmpty() ? (query.tokens.isEmpty() ? QStringLiteral("due") : QStringLiteral("relevance")) : query.sort;
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        if (sort == QLatin1String("relevance") && a.score != b.score) return a.score > b.score;
        if (sort == QLatin1String("last_reviewed")) {
            const auto at = a.card->schedule.lastReviewedAt, bt = b.card->schedule.lastReviewedAt;
            if (at.isValid() != bt.isValid()) return at.isValid();
            if (at != bt) return at > bt;
        } else {
            // Cards never asked again come last.
            const auto dueOf = [&](const ReviewCard &card) { return card.schedule.retained || card.schedule.retired ? QDateTime() : scheduleOf(card, now).dueAt; };
            const auto ad = dueOf(*a.card), bd = dueOf(*b.card);
            if (ad.isValid() != bd.isValid()) return ad.isValid();
            if (ad != bd) return ad < bd;
        }
        return a.card->id() < b.card->id();
    });
    return entries;
}

bool parseQuery(const QJsonObject &request, Query *query, const Library &library, QString *problem)
{
    if (!optionalString(request, QStringLiteral("query"), &query->text, problem)
        || !optionalString(request, QStringLiteral("project"), &query->project, problem)
        || !optionalString(request, QStringLiteral("document"), &query->document, problem)
        || !optionalString(request, QStringLiteral("state"), &query->state, problem)
        || !optionalString(request, QStringLiteral("sort"), &query->sort, problem)
        || !optionalStrings(request, QStringLiteral("tag_groups"), &query->tagGroups, problem)
        || !optionalLimit(request, &query->limit, problem)
        || !optionalBool(request, QStringLiteral("include_answer"), &query->includeAnswer, problem)
        || !optionalBool(request, QStringLiteral("full_answer"), &query->fullAnswer, problem)
        || !optionalBool(request, QStringLiteral("include_history"), &query->includeHistory, problem)
        || !optionalBool(request, QStringLiteral("include_deleted"), &query->includeDeleted, problem))
        return false;
    QStringList tags;
    if (!optionalStrings(request, QStringLiteral("tags"), &tags, problem)) return false;
    for (const QString &tag : std::as_const(tags)) {
        // "Correct" and "Wrong" are the built-in tags; merged names find the tag they became.
        const QString normalized = ReviewTags::normalize(tag);
        query->tags.append(normalized.toCaseFolded());
        for (auto it = library.merges.cbegin(); it != library.merges.cend(); ++it)
            if (it.key().compare(normalized, Qt::CaseInsensitive) == 0) query->tags.append(it.value().toCaseFolded());
    }
    const QStringList states{QStringLiteral("new"), QStringLiteral("due"), QStringLiteral("overdue"), QStringLiteral("untagged_last"), QStringLiteral("reviewed"),
                             QStringLiteral("retired"), QStringLiteral("active")};
    if (!query->state.isEmpty() && !states.contains(query->state)) {
        *problem = QStringLiteral("state must be one of %1.").arg(states.join(QStringLiteral(", ")));
        return false;
    }
    const QStringList sorts{QStringLiteral("relevance"), QStringLiteral("due"), QStringLiteral("last_reviewed")};
    if (!query->sort.isEmpty() && !sorts.contains(query->sort)) {
        *problem = QStringLiteral("sort must be one of %1.").arg(sorts.join(QStringLiteral(", ")));
        return false;
    }
    query->tokens = queryTokens(query->text);
    if (query->sort == QLatin1String("relevance") && query->tokens.isEmpty()) query->sort = QStringLiteral("due");
    return true;
}

QJsonObject indexJson(const Library &library)
{
    return QJsonObject{{QStringLiteral("indexed_at"), library.indexedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(library.indexedAt)},
                       {QStringLiteral("tag_groups_available"), library.tagRecords},
                       {QStringLiteral("cards"), int(library.cards.size())}};
}

QJsonArray groupsJson(const Library &library)
{
    QJsonArray groups;
    for (const auto &group : library.groups)
        groups.append(QJsonObject{{QStringLiteral("name"), group.name}, {QStringLiteral("tags"), QJsonArray::fromStringList(tagLabels(group.tags))}});
    return groups;
}

QJsonObject cardJson(const Library &library, const Entry &entry, const Query &query, const QDateTime &now)
{
    const ReviewCard &card = *entry.card;
    const Schedule schedule = scheduleOf(card, now);
    QJsonObject result{{QStringLiteral("card_id"), card.id()}, {QStringLiteral("question"), card.question()}};
    const QString note = card.record.value(QStringLiteral("note")).toString().trimmed();
    if (!note.isEmpty()) result.insert(QStringLiteral("note"), note);
    result.insert(QStringLiteral("document"), QJsonObject{{QStringLiteral("name"), entry.documentName},
                                                          {QStringLiteral("filename"), entry.pdfFilename},
                                                          {QStringLiteral("document_id"), entry.documentId}});
    result.insert(QStringLiteral("project"), entry.project.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(entry.project));
    QJsonValue source = QJsonValue::Null;
    if (!entry.passages.isEmpty()) {
        QJsonArray lines;
        for (const auto &segment : entry.passages.first().segments) lines.append(QJsonArray{segment.first, segment.last});
        source = QJsonObject{{QStringLiteral("path"), entry.passages.first().path}, {QStringLiteral("lines"), lines}, {QStringLiteral("markers_found"), true}};
    } else {
        const auto segments = primaryOf(card).value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
        if (!segments.isEmpty()) {
            QJsonArray lines;
            for (const auto &value : segments)
                lines.append(QJsonArray{value.toObject().value(QStringLiteral("line_start")).toInt(), value.toObject().value(QStringLiteral("line_end")).toInt()});
            source = QJsonObject{{QStringLiteral("path"), QDir(library.docsRoot).filePath(segments.first().toObject().value(QStringLiteral("source_file")).toString())},
                                 {QStringLiteral("lines"), lines}, {QStringLiteral("markers_found"), false}};
        }
    }
    result.insert(QStringLiteral("source"), source);
    result.insert(QStringLiteral("current_tags"), QJsonArray::fromStringList(tagLabels(entry.currentTags)));
    result.insert(QStringLiteral("answered"), card.wasAnswered());
    result.insert(QStringLiteral("status"), schedule.retired ? QStringLiteral("retired") : schedule.unpublished ? QStringLiteral("unpublished")
                                            : schedule.retained ? QStringLiteral("never")
                                            : schedule.overdueDays > 0 ? QStringLiteral("overdue")
                                            : schedule.due ? QStringLiteral("due") : QStringLiteral("scheduled"));
    // A retired card keeps its schedule for a Restore to review, but is due for nothing.
    result.insert(QStringLiteral("due_at"), schedule.retained || schedule.retired ? QJsonValue(QJsonValue::Null) : isoOrNull(schedule.dueAt));
    if (schedule.retired) result.insert(QStringLiteral("retired_at"), isoOrNull(card.schedule.retiredAt));
    if (schedule.overdueDays > 0) result.insert(QStringLiteral("overdue_days"), schedule.overdueDays);
    result.insert(QStringLiteral("interval_days"), card.schedule.intervalDays);
    result.insert(QStringLiteral("review_count"), card.schedule.reviewCount);
    result.insert(QStringLiteral("lapse_count"), card.schedule.lapseCount);
    result.insert(QStringLiteral("wrong_count"), wrongCountOf(card));
    result.insert(QStringLiteral("last_reviewed_at"), isoOrNull(card.schedule.lastReviewedAt));
    {
        // Its own feedback and the other cards' feedback of its document found in its answer
        // (shared): in the text of its answer regions of its PDF, else in its source passage.
        ReviewCardAgentInput answer;
        answer.cardId = card.id();
        answer.pdfPath = QDir(library.docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(entry.pdfFilename));
        answer.pdfSha256 = card.pdfSha256();
        answer.regions = QJsonArray::fromVariantList(card.regions());
        answer.page = card.page();
        QList<QJsonObject> records = library.feedback.value(card.id());
        for (auto it = library.feedback.cbegin(); it != library.feedback.cend(); ++it) {
            if (it.key() != card.id()) records += it.value();
        }
        result.insert(QStringLiteral("feedback"), reviewCardAgentFeedback(library.docsRoot, card.record, answer, records, library.cardRecords, query.includeDeleted,
                                                                          [&entry] { return entry.answer; }));
    }
    if (!query.tokens.isEmpty()) {
        result.insert(QStringLiteral("relevance"), entry.score);
        result.insert(QStringLiteral("matched"), QJsonArray::fromStringList(entry.matched));
    }
    if (query.includeAnswer) {
        QString answer = entry.answerFrom == QLatin1String("source") ? entry.answer : QString();
        QString from = entry.answerFrom;
        if (answer.isEmpty()) {
            // Like "Copy for agent": the text in the answer regions of the card's own PDF.
            ReviewCardAgentInput input;
            input.cardId = card.id();
            input.pdfPath = QDir(library.docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(entry.pdfFilename));
            input.pdfSha256 = card.pdfSha256();
            input.regions = QJsonArray::fromVariantList(card.regions());
            input.page = card.page();
            answer = reviewCardPdfAnswerText(input);
            from = QStringLiteral("pdf");
        }
        if (answer.isEmpty()) {
            answer = entry.answer;
            from = entry.answerFrom;
        }
        if (!answer.isEmpty()) {
            bool truncated = false;
            const QString text = query.fullAnswer ? answer.trimmed() : excerptOf(answer, query.tokens, &truncated);
            result.insert(QStringLiteral("answer"), QJsonObject{{QStringLiteral("text"), text},
                                                                {QStringLiteral("from"), from}, {QStringLiteral("truncated"), truncated}});
        } else {
            result.insert(QStringLiteral("answer"), QJsonValue::Null);
        }
    }
    if (query.includeHistory) {
        const auto groupNames = groupNamesById(library.groups);
        QJsonArray history;
        const auto answers = card.answers();
        for (auto answer = answers.crbegin(); answer != answers.crend(); ++answer) {
            QJsonObject item{{QStringLiteral("reviewed_at"), answer->value(QStringLiteral("reviewed_at")).toString()},
                             {QStringLiteral("tags"), QJsonArray::fromStringList(tagLabels(card.tagsOfEvent(*answer)))}};
            const QJsonObject notes = groupNotesJson(*answer, groupNames);
            if (!notes.isEmpty()) item.insert(QStringLiteral("group_notes"), notes);
            if (const QString note = ReviewTags::groupNotesOfEvent(*answer).value(ReviewTags::answerNoteKey()); !note.isEmpty())
                item.insert(QStringLiteral("note"), note);
            // An answer given in two steps: what was picked and noted before the answer was shown
            // (a predicted recall among them), and the agent's prediction (review-phases spec).
            if (const auto phases = ReviewPhases::ofEvent(*answer); phases.present) {
                item.insert(QStringLiteral("before_tags"), QJsonArray::fromStringList(tagLabels(ReviewTags::applyMerges(phases.beforeTags, card.tagMerges))));
                if (const QString note = ReviewTags::groupNotesOfEvent(*answer).value(ReviewTags::beforeNoteKey()); !note.isEmpty())
                    item.insert(QStringLiteral("before_note"), note);
                if (!phases.agentPrediction.isEmpty())
                    item.insert(QStringLiteral("agent_prediction"),
                                QJsonObject{{QStringLiteral("tag"), tagLabel(ReviewTags::resolve(phases.agentPrediction.value(QStringLiteral("tag")).toString(), card.tagMerges))},
                                            {QStringLiteral("reason"), phases.agentPrediction.value(QStringLiteral("reason")).toString()}});
            }
            const auto interval = intervalDaysOf(*answer);
            item.insert(QStringLiteral("interval_days"), interval ? QJsonValue(*interval) : QJsonValue(QJsonValue::Null));
            if (answer->value(QStringLiteral("rating")).toString() == QLatin1String("retained")) item.insert(QStringLiteral("never"), true);
            history.append(item);
        }
        result.insert(QStringLiteral("history"), history);
    }
    return result;
}

// ---------------------------------------------------------------- knowledge levels

double levelValue(const QString &level)
{
    return level == QLatin1String("solid") ? 1.0 : level == QLatin1String("shaky") ? 0.5 : 0.0;
}

QString shortQuestion(const QString &question)
{
    QString line = question.simplified();
    return line.size() > 90 ? line.left(88).trimmed() + QStringLiteral("…") : line;
}

QString daysText(double days)
{
    if (days >= 1 || days == 0) return QStringLiteral("%1 d").arg(std::round(days * 10) / 10);
    return QStringLiteral("%1 h").arg(std::max(1.0, std::round(days * 24)));
}
}

QJsonObject error(const QString &code, const QString &message)
{
    return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), false},
                       {QStringLiteral("error_code"), code}, {QStringLiteral("message"), message}};
}

QString indexPath()
{
    return QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("thoughts-server/review-query.sqlite"));
}

bool readIndex(const QString &databasePath, IndexSnapshot *snapshot, QString *message, QString *errorCode)
{
    const QString path = QFileInfo(databasePath).absoluteFilePath();
    if (!QFileInfo(path).isFile()) {
        if (errorCode) *errorCode = QStringLiteral("index_unavailable");
        *message = QStringLiteral("There is no review index at %1 yet; Thoughts creates it when it starts.").arg(path);
        return false;
    }
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    QString lastError;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (attempt) QThread::msleep(100);
        // Thoughts rewrites the index in one short transaction and then removes its log: wait
        // for that rather than share the log, and use it only if it stays (Thoughts quit
        // without removing it).
        const bool logged = QFileInfo::exists(path + QStringLiteral("-wal"));
        if (logged && attempt < 15) continue;
        const Stamp before = stampOf(path);
        IndexSnapshot read;
        // immutable: no locks and no -wal or -shm file. readonly_shm: never create the
        // shared-memory file, only read an existing one.
        if (!readDatabase(uri + (logged ? QStringLiteral("?mode=ro&readonly_shm=1") : QStringLiteral("?mode=ro&immutable=1")), &read, &lastError)) continue;
        // Without locks the read only counts if the file did not change meanwhile.
        if (!logged && stampOf(path) != before) continue;
        *snapshot = read;
        return true;
    }
    if (errorCode) *errorCode = QStringLiteral("index_unreadable");
    *message = QStringLiteral("The review index %1 could not be read: %2").arg(path, lastError);
    return false;
}

Library libraryFromSnapshot(const IndexSnapshot &snapshot, const QString &docsRoot)
{
    Library library;
    library.docsRoot = docsRoot;
    library.indexedAt = snapshot.indexedAt;
    library.tagRecords = snapshot.hasTagRecords;
    const QJsonObject projections = snapshot.projections;
    // Read-only: no outbox, no migration, no source change.
    const ReviewStore store(docsRoot, [projections] { return projections; }, {}, true);
    const QSet<QString> unpublished = ProjectConfiguration::unpublishedDocumentIds(docsRoot);
    for (auto card : store.cards()) {
        const QString status = card.status();
        if (card.schedule.deleted || card.record.value(QStringLiteral("deleted")).toBool() || status == QLatin1String("candidate")
            || status == QLatin1String("rejected")) continue;
        card.schedule.unpublished = !unpublished.isEmpty() && unpublished.contains(cardDocumentId(card, docsRoot));
        library.cards.append(card);
        library.cardRecords.insert(card.id(), card.record);
    }
    library.merges = store.tagMerges();
    for (const auto &feedback : store.cardFeedback({}, true)) library.feedback[feedback.value(QStringLiteral("card_id")).toString()].append(feedback);
    library.documentFeedback = store.documentFeedback();
    library.groups = store.tagGroups();
    library.projects = LibraryProjectStore(docsRoot).projects(false);
    return library;
}

QStringList levelNames()
{
    return {QStringLiteral("solid"), QStringLiteral("shaky"), QStringLiteral("unknown"), QStringLiteral("ignore")};
}

Levels defaultLevels()
{
    // The user's slider groups (2026-10-15): Understanding and Recall say how well the answer is
    // known; Interest, Study, Material and Source say nothing about it. Tags merged into these
    // (e.g. the built-in Correct into Recalled) count under them.
    Levels levels;
    levels.source = QStringLiteral("default mapping");
    levels.tags = {
        {QStringLiteral("I get it"), QStringLiteral("solid")},
        {QStringLiteral("Deeply"), QStringLiteral("solid")},
        {QStringLiteral("Recalled"), QStringLiteral("solid")},
        {QStringLiteral("Know by heart"), QStringLiteral("solid")},
        {QStringLiteral("Partly"), QStringLiteral("shaky")},
        {QStringLiteral("Partly recalled"), QStringLiteral("shaky")},
        {QStringLiteral("Don't get it"), QStringLiteral("unknown")},
        {QStringLiteral("Forgot"), QStringLiteral("unknown")},
    };
    return levels;
}

QString levelsPath()
{
    return QDir(Platform::thoughtsDataRoot()).filePath(QStringLiteral("knowledge-tag-levels.json"));
}

QJsonObject levelsDocument(const Levels &levels)
{
    QJsonObject tags;
    for (const auto &[tag, level] : levels.tags) tags.insert(tagLabel(tag), level);
    return QJsonObject{{QStringLiteral("schema_version"), 1},
                       {QStringLiteral("about"), QStringLiteral("How well each review tag says the user knows a card's answer, for thoughtsctl knowledge-profile: "
                                                                "solid, shaky, unknown or ignore. Tags not listed are ignored. An answer with several "
                                                                "listed tags counts at its lowest level.")},
                       {QStringLiteral("levels"), tags}};
}

bool loadLevels(const QString &path, Levels *levels, QString *message)
{
    QFile file(path);
    if (!file.exists()) {
        *levels = defaultLevels();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *message = QStringLiteral("%1 could not be read.").arg(path);
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.read(1024 * 1024), &parse);
    const QJsonObject root = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() || root.value(QStringLiteral("schema_version")).toInt() != 1
        || !root.value(QStringLiteral("levels")).isObject()) {
        *message = QStringLiteral("%1 is not a knowledge level mapping: expected {\"schema_version\": 1, \"levels\": {\"<tag>\": \"solid\"|\"shaky\"|\"unknown\"|\"ignore\"}}.").arg(path);
        return false;
    }
    Levels result;
    result.source = path;
    const QJsonObject tags = root.value(QStringLiteral("levels")).toObject();
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        const QString level = it.value().toString();
        if (!levelNames().contains(level) || it.key().trimmed().isEmpty()) {
            *message = QStringLiteral("%1: the level of \"%2\" must be one of %3.").arg(path, it.key(), levelNames().join(QStringLiteral(", ")));
            return false;
        }
        result.tags.append({it.key(), level});
    }
    *levels = result;
    return true;
}

bool initLevels(const QString &path, QString *message)
{
    if (QFileInfo::exists(path)) {
        *message = QStringLiteral("%1 already exists; edit it, or remove it to start again from the default mapping.").arg(path);
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *message = QStringLiteral("Could not create %1.").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    const QByteArray data = QJsonDocument(levelsDocument(defaultLevels())).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *message = QStringLiteral("Could not write %1.").arg(path);
        return false;
    }
    return true;
}

bool updateLevels(const QString &path, const QList<QPair<QString, QString>> &levels, const QStringList &mergedAway, QString *backupPath,
                  QString *message)
{
    if (!QFileInfo::exists(path)) return true;
    Levels current;
    if (!loadLevels(path, &current, message)) return false;
    const auto key = [](const QString &tag) { return ReviewTags::normalize(tag).toCaseFolded(); };
    QSet<QString> replaced;
    for (const auto &[tag, level] : levels) replaced.insert(key(tag));
    for (const QString &tag : mergedAway) replaced.insert(key(tag));
    Levels result;
    result.tags = levels;
    for (const auto &[tag, level] : current.tags) {
        if (!replaced.contains(key(tag))) result.tags.append({tag, level});
    }
    const QString backup = path + QStringLiteral(".backup-") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    if (!QFile::copy(path, backup)) {
        *message = QStringLiteral("Could not back up %1 to %2.").arg(path, backup);
        return false;
    }
    QSaveFile file(path);
    const QByteArray data = QJsonDocument(levelsDocument(result)).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *message = QStringLiteral("Could not write %1 (the previous file is kept as %2).").arg(path, backup);
        return false;
    }
    if (backupPath) *backupPath = backup;
    return true;
}

QJsonObject search(const Library &library, const QJsonObject &request, const QDateTime &now)
{
    QString problem;
    Query query;
    if (!onlyKeys(request, {QStringLiteral("query"), QStringLiteral("tags"), QStringLiteral("tag_groups"), QStringLiteral("project"),
                            QStringLiteral("document"), QStringLiteral("state"), QStringLiteral("sort"), QStringLiteral("limit"),
                            QStringLiteral("include_answer"), QStringLiteral("full_answer"), QStringLiteral("include_history"),
                            QStringLiteral("include_deleted")}, &problem)
        || !parseQuery(request, &query, library, &problem))
        return error(QStringLiteral("invalid_request"), problem);
    QJsonObject failure;
    const QList<Entry> entries = findCards(library, query, now, &failure);
    if (!failure.isEmpty()) return failure;
    QJsonArray cards;
    for (int i = 0; i < entries.size() && i < query.limit; ++i) cards.append(cardJson(library, entries.at(i), query, now));
    QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true},
                       {QStringLiteral("matched"), int(entries.size())}, {QStringLiteral("returned"), int(cards.size())},
                       {QStringLiteral("cards"), cards}, {QStringLiteral("index"), indexJson(library)},
                       {QStringLiteral("tag_groups"), groupsJson(library)}};
    if (!query.tokens.isEmpty()) result.insert(QStringLiteral("query_words"), QJsonArray::fromStringList(query.tokens));
    return result;
}

QJsonObject profile(const Library &library, const QJsonObject &request, const Levels &levels, const QDateTime &now)
{
    QString problem;
    QString topic;
    QJsonObject searchRequest;
    int limit = defaultProfileLimit;
    if (!onlyKeys(request, {QStringLiteral("topic"), QStringLiteral("project"), QStringLiteral("document"), QStringLiteral("limit")}, &problem)
        || !optionalString(request, QStringLiteral("topic"), &topic, &problem) || !optionalLimit(request, &limit, &problem))
        return error(QStringLiteral("invalid_request"), problem);
    if (topic.isEmpty()) return error(QStringLiteral("invalid_request"), QStringLiteral("topic must name what to profile, for example \"GRPO advantage\"."));
    searchRequest.insert(QStringLiteral("query"), topic);
    for (const QString &key : {QStringLiteral("project"), QStringLiteral("document")})
        if (request.contains(key)) searchRequest.insert(key, request.value(key));
    Query query;
    if (!parseQuery(searchRequest, &query, library, &problem)) return error(QStringLiteral("invalid_request"), problem);
    QJsonObject failure;
    QList<Entry> entries = findCards(library, query, now, &failure);
    if (!failure.isEmpty()) return failure;
    // Weak matches (one common word somewhere in a long answer) say little about the topic.
    if (!entries.isEmpty()) {
        const double best = entries.first().score;
        entries.removeIf([best](const Entry &entry) { return entry.score < best * 0.2; });
    }
    const int matched = int(entries.size());
    if (entries.size() > limit) entries.resize(limit);

    // Tag -> level, by the name each tag is shown under now.
    QHash<QString, QString> levelOf;
    QJsonObject mapping;
    for (const auto &[tag, level] : levels.tags) {
        const QString normalized = ReviewTags::normalize(tag);
        const QString key = ReviewTags::resolve(normalized, library.merges).toCaseFolded();
        if (!levelOf.contains(key)) levelOf.insert(key, level);
        mapping.insert(tagLabel(normalized), level);
    }
    QStringList unmapped;
    for (const auto &group : library.groups) {
        // The Understanding group, and the steps of every slider: each step should have a level.
        const bool understanding = group.name.compare(QLatin1String("Understanding"), Qt::CaseInsensitive) == 0;
        for (const QString &tag : understanding ? group.tags : group.scale)
            if (!levelOf.contains(tag.toCaseFolded()) && !unmapped.contains(tagLabel(tag))) unmapped.append(tagLabel(tag));
    }

    const QStringList classes{QStringLiteral("solid"), QStringLiteral("shaky"), QStringLiteral("unknown"), QStringLiteral("unreviewed")};
    QHash<QString, QJsonArray> byClass;
    QHash<QString, QStringList> questions;
    for (const auto &entry : std::as_const(entries)) {
        const ReviewCard &card = *entry.card;
        const Schedule schedule = scheduleOf(card, now);
        QString level;
        QStringList reason;
        if (!card.wasAnswered()) {
            level = QStringLiteral("unreviewed");
            reason.append(QStringLiteral("never answered"));
        } else {
            // Newest first: each answer at the lowest level of its mapped tags.
            struct Rated { double value; QStringList tags; };
            QList<Rated> rated;
            const auto answers = card.answers();
            for (auto answer = answers.crbegin(); answer != answers.crend(); ++answer) {
                std::optional<double> value;
                QStringList shown;
                for (const QString &tag : card.tagsOfEvent(*answer)) {
                    const QString tagLevel = levelOf.value(tag.toCaseFolded());
                    if (tagLevel.isEmpty() || tagLevel == QLatin1String("ignore")) continue;
                    value = std::min(value.value_or(1.0), levelValue(tagLevel));
                    shown.append(tagLabel(tag));
                }
                if (value) rated.append({*value, shown});
            }
            double score = 0;
            if (rated.isEmpty()) {
                // Answered, but never with a tag that says how well: only the interval chosen tells.
                score = card.schedule.intervalDays >= 21 && card.schedule.lapseCount == 0 ? 0.8 : 0.5;
                reason.append(QStringLiteral("%1 answer%2, none with an understanding tag").arg(answers.size()).arg(answers.size() == 1 ? QString() : QStringLiteral("s")));
            } else {
                // The latest answer weighs most: each older one half as much as the next.
                double weight = 1, total = 0;
                for (const auto &answer : std::as_const(rated)) {
                    score += weight * answer.value;
                    total += weight;
                    weight /= 2;
                }
                score /= total;
                QStringList shown;
                for (int i = 0; i < rated.size() && i < 3; ++i) shown.append(rated.at(i).tags.join(QLatin1Char('+')));
                reason.append(rated.size() == 1 ? QStringLiteral("last answer: %1").arg(shown.first())
                                                : QStringLiteral("last %1 answers: %2").arg(shown.size()).arg(shown.join(QStringLiteral(", "))));
            }
            if (card.schedule.lapseCount > 0) score -= 0.1 * std::min(card.schedule.lapseCount, 3);
            if (card.schedule.intervalDays >= 30) score += 0.1;
            // Long past due: it may have faded since.
            if (schedule.overdueDays > std::max(7, card.schedule.intervalDays)) score -= 0.15;
            level = score >= 0.75 ? QStringLiteral("solid") : score >= 0.4 ? QStringLiteral("shaky") : QStringLiteral("unknown");
            if (schedule.retained) reason.append(QStringLiteral("never asked again"));
            else if (card.schedule.intervalDays > 0) reason.append(QStringLiteral("interval %1").arg(daysText(card.schedule.intervalDays)));
            if (card.schedule.lapseCount > 0) reason.append(QStringLiteral("%1 lapse%2").arg(card.schedule.lapseCount).arg(card.schedule.lapseCount == 1 ? QString() : QStringLiteral("s")));
        }
        if (schedule.overdueDays > 0) reason.append(QStringLiteral("overdue %1 d").arg(schedule.overdueDays));
        if (schedule.retired) reason.append(QStringLiteral("retired"));
        QJsonObject item{{QStringLiteral("card_id"), card.id()}, {QStringLiteral("question"), card.question()},
                         {QStringLiteral("document"), entry.documentName}, {QStringLiteral("reason"), reason.join(QStringLiteral("; "))}};
        if (!entry.project.isEmpty()) item.insert(QStringLiteral("project"), entry.project);
        byClass[level].append(item);
        questions[level].append(shortQuestion(card.question()));
    }

    QJsonObject counts, cards;
    for (const QString &name : classes) {
        counts.insert(name, int(byClass.value(name).size()));
        cards.insert(name, byClass.value(name));
    }
    QStringList summary;
    if (entries.isEmpty()) {
        summary.append(QStringLiteral("No review cards match \"%1\": there is no record of what the user knows about it.").arg(topic));
    } else {
        summary.append(QStringLiteral("\"%1\": %2 matching review card%3: %4 solid, %5 shaky, %6 not understood, %7 not yet reviewed.")
                           .arg(topic).arg(entries.size()).arg(entries.size() == 1 ? QString() : QStringLiteral("s"))
                           .arg(byClass.value(QStringLiteral("solid")).size()).arg(byClass.value(QStringLiteral("shaky")).size())
                           .arg(byClass.value(QStringLiteral("unknown")).size()).arg(byClass.value(QStringLiteral("unreviewed")).size()));
        const QList<QPair<QString, QString>> labels{{QStringLiteral("solid"), QStringLiteral("Solid")}, {QStringLiteral("shaky"), QStringLiteral("Shaky")},
            {QStringLiteral("unknown"), QStringLiteral("Not understood")}, {QStringLiteral("unreviewed"), QStringLiteral("Not yet reviewed")}};
        for (const auto &[name, label] : labels) {
            const QStringList shown = questions.value(name);
            if (shown.isEmpty()) continue;
            QString line = label + QStringLiteral(": ") + shown.mid(0, 3).join(QStringLiteral("; "));
            if (shown.size() > 3) line += QStringLiteral(" (+%1 more)").arg(shown.size() - 3);
            summary.append(line);
        }
    }
    QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("topic"), topic},
                       {QStringLiteral("matched"), matched}, {QStringLiteral("counts"), counts}, {QStringLiteral("cards"), cards},
                       {QStringLiteral("summary"), summary.join(QLatin1Char('\n'))}, {QStringLiteral("index"), indexJson(library)}};
    QJsonObject mappingInfo{{QStringLiteral("source"), levels.source}, {QStringLiteral("levels"), mapping}};
    if (!unmapped.isEmpty()) mappingInfo.insert(QStringLiteral("unmapped_understanding_tags"), QJsonArray::fromStringList(unmapped));
    result.insert(QStringLiteral("mapping"), mappingInfo);
    if (!query.project.isEmpty()) result.insert(QStringLiteral("project"), query.project);
    if (!query.document.isEmpty()) result.insert(QStringLiteral("document"), query.document);
    return result;
}

QJsonObject run(const QString &command, const QJsonObject &request, const QString &databasePath, const QString &docsRoot,
                const QString &levelsFile, const QDateTime &now)
{
    Levels levels;
    QString message;
    if (command == QLatin1String("knowledge-profile") && !loadLevels(levelsFile, &levels, &message))
        return error(QStringLiteral("invalid_levels"), message);
    IndexSnapshot snapshot;
    QString code;
    if (!readIndex(databasePath, &snapshot, &message, &code)) return error(code, message);
    const Library library = libraryFromSnapshot(snapshot, docsRoot);
    return command == QLatin1String("knowledge-profile") ? profile(library, request, levels, now) : search(library, request, now);
}

bool readLibrary(const QString &databasePath, const QString &docsRoot, Library *library, QJsonObject *failure)
{
    IndexSnapshot snapshot;
    QString message, code;
    if (!readIndex(databasePath, &snapshot, &message, &code)) {
        *failure = error(code, message);
        return false;
    }
    *library = libraryFromSnapshot(snapshot, docsRoot);
    return true;
}

// ---------------------------------------------------------------- document feedback

namespace
{
struct FeedbackRequest {
    QDateTime since;
    bool includePages = true;
    QString imagesDir;
};

bool parseFeedbackRequest(const QJsonObject &request, FeedbackRequest *parsed, QString *problem)
{
    QString since;
    if (!onlyKeys(request, {QStringLiteral("since"), QStringLiteral("include_pages"), QStringLiteral("images_dir")}, problem)
        || !optionalString(request, QStringLiteral("since"), &since, problem)
        || !optionalBool(request, QStringLiteral("include_pages"), &parsed->includePages, problem)
        || !optionalString(request, QStringLiteral("images_dir"), &parsed->imagesDir, problem))
        return false;
    if (!since.isEmpty()) {
        parsed->since = QDateTime::fromString(since, Qt::ISODateWithMs);
        if (!parsed->since.isValid()) {
            *problem = QStringLiteral("since must be an ISO 8601 date, e.g. 2026-05-12 or 2026-05-12T00:00:00Z.");
            return false;
        }
    }
    return true;
}

// Likes, dislikes, questions, drawings and text boxes: every kind but comments, newest first.
QList<QJsonObject> listedFeedback(const Library &library, const FeedbackRequest &request)
{
    QList<QJsonObject> records;
    for (const QJsonObject &record : library.documentFeedback) {
        if (record.value(QStringLiteral("deleted")).toBool() || DocumentFeedback::kindOf(record) == DocumentFeedback::other()) continue;
        if (request.since.isValid()) {
            const QDateTime created = QDateTime::fromString(record.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
            if (!created.isValid() || created < request.since) continue;
        }
        records.append(record);
    }
    std::stable_sort(records.begin(), records.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return QDateTime::fromString(a.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs)
            > QDateTime::fromString(b.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
    });
    return records;
}

// The pages (1-based) the record is on; none for a passage of an HTML attachment.
QList<int> feedbackPages(const QJsonObject &record)
{
    QList<int> pages;
    if (DocumentFeedback::isAttachmentFeedback(record)) return pages;
    for (const QJsonValue &value : record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray()) {
        const int page = value.toObject().value(QStringLiteral("page")).toInt();
        if (page > 0 && !pages.contains(page)) pages.append(page);
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

QString feedbackKind(const QJsonObject &record)
{
    if (DocumentFeedback::isDrawing(record)) return QStringLiteral("drawing");
    if (DocumentFeedback::isTextBox(record)) return QStringLiteral("text_box");
    if (const QString kind = DocumentFeedback::kindOf(record); !kind.isEmpty()) return kind;
    const QString polarity = record.value(QStringLiteral("polarity")).toString();
    return polarity == QLatin1String("positive") ? QStringLiteral("like") : polarity == QLatin1String("negative") ? QStringLiteral("dislike") : polarity;
}

struct LibraryDocument {
    QString name, filename, project, pdfSha256;
};

QHash<QString, LibraryDocument> documentsById(const QJsonArray &projects)
{
    QHash<QString, LibraryDocument> documents;
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject project = projectValue.toObject();
        for (const QJsonValue &value : project.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject entry = value.toObject();
            const QString id = entry.value(QStringLiteral("document_id")).toString().toLower();
            if (id.isEmpty() || documents.contains(id)) continue;
            documents.insert(id, {entry.value(QStringLiteral("name")).toString().trimmed(), entry.value(QStringLiteral("filename")).toString(),
                                  project.value(QStringLiteral("name")).toString(), entry.value(QStringLiteral("sha256")).toString()});
        }
    }
    return documents;
}

// The versions with pages to read, in the order the records first name them.
QStringList versionsOf(const QList<QJsonObject> &records)
{
    QStringList versions;
    for (const QJsonObject &record : records) {
        const QString version = PdfVersions::recordedSha256(record);
        if (!version.isEmpty() && !feedbackPages(record).isEmpty() && !versions.contains(version)) versions.append(version);
    }
    return versions;
}

QJsonArray rounded(const QRectF &rect)
{
    const auto round3 = [](double value) { return std::round(value * 1000.0) / 1000.0; };
    return QJsonArray{round3(rect.x()), round3(rect.y()), round3(rect.width()), round3(rect.height())};
}

// A PNG of the page a drawing or a text box is on, with the strokes or the box drawn on it.
QString writeMarkImage(Poppler::Document *pdf, const QJsonObject &record, const QString &folder)
{
    const QList<int> pages = feedbackPages(record);
    if (!pdf || pages.isEmpty() || pages.first() > pdf->numPages()) return {};
    const std::unique_ptr<Poppler::Page> page = pdf->page(pages.first() - 1);
    QImage image = page ? page->renderToImage(110, 110) : QImage();
    if (image.isNull()) return {};
    image = image.convertToFormat(QImage::Format_RGB32);
    const double width = image.width(), height = image.height(), line = std::max(2.0, width / 320.0);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    if (DocumentFeedback::isDrawing(record)) {
        for (const DocumentFeedback::Stroke &stroke : DocumentFeedback::strokesOf(record)) {
            if (stroke.points.isEmpty()) continue;
            QPen pen(QColor(stroke.colour.isEmpty() ? QStringLiteral("#1e6fd9") : stroke.colour), line);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            QPainterPath path(QPointF(stroke.points.first().x() * width, stroke.points.first().y() * height));
            for (const QPointF &point : stroke.points) path.lineTo(point.x() * width, point.y() * height);
            painter.drawPath(path);
        }
    } else {
        const QRectF box = DocumentFeedback::textBoxRect(record);
        if (box.isEmpty()) return {};
        painter.setPen(QPen(QColor(QStringLiteral("#e0702a")), line));
        painter.drawRect(QRectF(box.x() * width, box.y() * height, box.width() * width, box.height() * height));
    }
    painter.end();
    if (!QDir().mkpath(folder)) return {};
    const QString path = QDir(folder).filePath(record.value(QStringLiteral("feedback_id")).toString() + QStringLiteral(".png"));
    return image.save(path, "PNG") ? path : QString();
}
}

QStringList documentFeedbackVersions(const Library &library, const QJsonObject &request)
{
    FeedbackRequest parsed;
    QString problem;
    if (!parseFeedbackRequest(request, &parsed, &problem)) return {};
    return versionsOf(listedFeedback(library, parsed));
}

QJsonObject documentFeedback(const Library &library, const QJsonObject &request, const VersionPath &versionPath)
{
    FeedbackRequest parsed;
    QString problem;
    if (!parseFeedbackRequest(request, &parsed, &problem)) return error(QStringLiteral("invalid_request"), problem);
    const QList<QJsonObject> records = listedFeedback(library, parsed);
    const QHash<QString, LibraryDocument> documents = documentsById(library.projects);
    QHash<QString, QList<int>> pagesOfVersion;
    QHash<QString, QString> documentOfVersion;
    QHash<QString, QList<QJsonObject>> marksOfVersion;
    QList<QJsonObject> items;
    for (const QJsonObject &record : records) {
        const QString documentId = DocumentFeedback::documentIdOf(record).toLower();
        const LibraryDocument document = documents.value(documentId);
        const QString version = PdfVersions::recordedSha256(record);
        const QList<int> pages = feedbackPages(record);
        const auto orNull = [](const QString &value) { return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value); };
        QJsonObject item{{QStringLiteral("feedback_id"), record.value(QStringLiteral("feedback_id")).toString()},
                         {QStringLiteral("kind"), feedbackKind(record)},
                         {QStringLiteral("created_at"), record.value(QStringLiteral("created_at")).toString()},
                         {QStringLiteral("comment"), record.value(QStringLiteral("comment")).toString().trimmed()},
                         {QStringLiteral("document"), QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("name"), orNull(document.name)},
                                                                  {QStringLiteral("filename"), orNull(document.filename)},
                                                                  {QStringLiteral("project"), orNull(document.project)}}},
                         {QStringLiteral("pdf_sha256"), orNull(version)},
                         {QStringLiteral("current_version"), !version.isEmpty() && version == document.pdfSha256},
                         {QStringLiteral("pages"), [&pages] {
                              QJsonArray array;
                              for (int page : pages) array.append(page);
                              return array;
                          }()}};
        if (const QString updated = record.value(QStringLiteral("updated_at")).toString(); !updated.isEmpty()) item.insert(QStringLiteral("updated_at"), updated);
        if (DocumentFeedback::isAttachmentFeedback(record)) item.insert(QStringLiteral("view"), DocumentFeedback::attachmentView());
        if (DocumentFeedback::canResolve(record)) item.insert(QStringLiteral("resolved"), DocumentFeedback::isResolved(record));
        if (DocumentFeedback::isDrawing(record)) {
            item.insert(QStringLiteral("strokes"), int(DocumentFeedback::strokesOf(record).size()));
            item.insert(QStringLiteral("area"), rounded(DocumentFeedback::strokeBounds(record)));
        } else if (DocumentFeedback::isTextBox(record)) {
            item.insert(QStringLiteral("area"), rounded(DocumentFeedback::textBoxRect(record)));
        } else {
            const QJsonObject quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject();
            item.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), quote.value(QStringLiteral("exact")).toString()},
                                                             {QStringLiteral("prefix"), quote.value(QStringLiteral("prefix")).toString()},
                                                             {QStringLiteral("suffix"), quote.value(QStringLiteral("suffix")).toString()}});
        }
        if (!version.isEmpty() && !pages.isEmpty()) {
            documentOfVersion.insert(version, documentId);
            for (int page : pages)
                if (!pagesOfVersion[version].contains(page)) pagesOfVersion[version].append(page);
            if (DocumentFeedback::isPageMark(record)) marksOfVersion[version].append(record);
        }
        items.append(item);
    }
    // Each version is read once: the text of its pages with feedback, and the pictures of its marks.
    QJsonArray versions;
    QHash<QString, QString> images;
    const bool read = parsed.includePages || !parsed.imagesDir.isEmpty();
    for (const QString &version : versionsOf(records)) {
        QJsonObject entry{{QStringLiteral("pdf_sha256"), version}, {QStringLiteral("document_id"), documentOfVersion.value(version)}};
        if (read) {
            QString unavailable;
            const QString path = versionPath ? versionPath(version, &unavailable) : QString();
            const std::unique_ptr<Poppler::Document> pdf = path.isEmpty() ? nullptr : Poppler::Document::load(path);
            if (!pdf || pdf->isLocked()) {
                entry.insert(QStringLiteral("available"), false);
                entry.insert(QStringLiteral("unavailable"), !unavailable.isEmpty() ? unavailable : path.isEmpty() ? QStringLiteral("The version is not available.")
                                                                                                                  : QStringLiteral("The PDF could not be read."));
            } else {
                entry.insert(QStringLiteral("available"), true);
                entry.insert(QStringLiteral("page_count"), pdf->numPages());
                if (parsed.includePages) {
                    QList<int> pages = pagesOfVersion.value(version);
                    std::sort(pages.begin(), pages.end());
                    QJsonArray texts;
                    for (int page : std::as_const(pages)) {
                        const std::unique_ptr<Poppler::Page> pdfPage = page <= pdf->numPages() ? pdf->page(page - 1) : nullptr;
                        texts.append(QJsonObject{{QStringLiteral("page"), page},
                                                 {QStringLiteral("text"), pdfPage ? pdfPage->text(QRectF()).trimmed() : QString()}});
                    }
                    entry.insert(QStringLiteral("pages"), texts);
                }
                if (!parsed.imagesDir.isEmpty()) {
                    for (const QJsonObject &mark : marksOfVersion.value(version)) {
                        const QString image = writeMarkImage(pdf.get(), mark, parsed.imagesDir);
                        if (!image.isEmpty()) images.insert(mark.value(QStringLiteral("feedback_id")).toString(), image);
                    }
                }
            }
        }
        versions.append(entry);
    }
    QJsonArray feedback;
    for (QJsonObject item : std::as_const(items)) {
        if (const QString image = images.value(item.value(QStringLiteral("feedback_id")).toString()); !image.isEmpty()) item.insert(QStringLiteral("image"), image);
        feedback.append(item);
    }
    return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("returned"), int(feedback.size())},
                       {QStringLiteral("feedback"), feedback}, {QStringLiteral("versions"), versions}, {QStringLiteral("index"), indexJson(library)}};
}
}
