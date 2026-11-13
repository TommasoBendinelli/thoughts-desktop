// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewcardfeedback.h"

#include "desktopsyncstore.h"

#include <QChar>
#include <QHash>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace Thoughts::ReviewCardFeedback
{
QString entityType()
{
    return QStringLiteral("review_card_feedback");
}

QString collection()
{
    return QStringLiteral("review-feedback");
}

QString capability()
{
    return QStringLiteral("review_card_feedback_v1");
}

QString projectionKey(const QString &feedbackId)
{
    return entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n') + feedbackId;
}

QString positive()
{
    return QStringLiteral("positive");
}

QString negative()
{
    return QStringLiteral("negative");
}

QString neutral()
{
    return QStringLiteral("neutral");
}

namespace
{
QString string(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

// Code points, as the server (Python) counts characters.
qsizetype codePoints(const QString &text)
{
    qsizetype count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) ++i;
        ++count;
    }
    return count;
}

// The first @p count code points of @p text.
QString leftCodePoints(const QString &text, qsizetype count)
{
    qsizetype i = 0;
    for (qsizetype seen = 0; i < text.size() && seen < count; ++seen) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) ++i;
        ++i;
    }
    return text.left(i);
}

bool validUuid4(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.version() == QUuid::Random && uuid.toString(QUuid::WithoutBraces) == id;
}

bool validTimestamp(const QString &value)
{
    // RFC 3339 with a timezone, as the server parses it (datetime.fromisoformat).
    static const QRegularExpression pattern(QStringLiteral(R"(^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(:\d{2}(\.\d{1,6})?)?(Z|[+-]\d{2}:\d{2})$)"));
    if (!pattern.match(value).hasMatch()) return false;
    return QDateTime::fromString(value, Qt::ISODateWithMs).isValid() || QDateTime::fromString(value, Qt::ISODate).isValid();
}

bool lowerHex64(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool dropped(char32_t c)
{
    switch (QChar::category(c)) {
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Mark_NonSpacing:
        return true;
    default:
        return false;
    }
}

bool hyphen(char32_t c)
{
    return c == U'-' || c == U'‐' || c == U'‑';
}

qsizetype commonSuffix(const std::u32string &a, const std::u32string &b)
{
    qsizetype count = 0;
    while (count < qsizetype(a.size()) && count < qsizetype(b.size()) && a[a.size() - 1 - count] == b[b.size() - 1 - count]) ++count;
    return count;
}

qsizetype commonPrefix(const std::u32string &a, const std::u32string &b)
{
    qsizetype count = 0;
    while (count < qsizetype(a.size()) && count < qsizetype(b.size()) && a[count] == b[count]) ++count;
    return count;
}

bool blank(const QString &text)
{
    return text.trimmed().isEmpty();
}
}

Skeleton skeleton(const QString &text, bool dropHyphens)
{
    Skeleton result;
    int offset = 0;
    while (offset < text.size()) {
        char32_t codePoint = text.at(offset).unicode();
        int width = 1;
        if (text.at(offset).isHighSurrogate() && offset + 1 < text.size() && text.at(offset + 1).isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(text.at(offset), text.at(offset + 1));
            width = 2;
        }
        if (QChar::category(codePoint) == QChar::Mark_NonSpacing && !result.spans.isEmpty()) {
            // A combining mark belongs to the character before it (its glyph is drawn with it).
            result.spans.last().second = offset + width;
            offset += width;
            continue;
        }
        const QString decomposed = QString::fromUcs4(&codePoint, 1).normalized(QString::NormalizationForm_KD);
        for (const char32_t character : decomposed.toUcs4()) {
            if (dropped(character) || (dropHyphens && hyphen(character))) continue;
            result.text.push_back(character);
            result.spans.append({offset, offset + width});
        }
        offset += width;
    }
    return result;
}

Resolution resolve(const QString &answerText, const QString &exact, const QString &prefix, const QString &suffix, bool samePdf)
{
    if (samePdf) return {QStringLiteral("exact")};
    for (const bool dropHyphens : {false, true}) {
        const std::u32string needle = skeleton(exact, dropHyphens).text;
        if (needle.empty()) return {QStringLiteral("orphaned")};
        const Skeleton haystack = skeleton(answerText, dropHyphens);
        QList<qsizetype> hits;
        for (std::size_t position = haystack.text.find(needle); position != std::u32string::npos; position = haystack.text.find(needle, position + 1))
            hits.append(qsizetype(position));
        if (hits.isEmpty()) continue;
        const std::u32string before = skeleton(prefix, dropHyphens).text;
        const std::u32string after = skeleton(suffix, dropHyphens).text;
        qsizetype best = -1, bestScore = -1;
        for (const qsizetype hit : std::as_const(hits)) {
            const qsizetype score = commonSuffix(before, haystack.text.substr(0, hit)) + commonPrefix(after, haystack.text.substr(hit + needle.size()));
            // The first hit wins a tie.
            if (score > bestScore) {
                best = hit;
                bestScore = score;
            }
        }
        return {QStringLiteral("relocated"), haystack.spans.at(best).first, haystack.spans.at(best + qsizetype(needle.size()) - 1).second};
    }
    return {QStringLiteral("orphaned")};
}

Resolution resolve(const QJsonObject &record, const QString &answerText, const QString &displayedPdfSha256)
{
    const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    const QJsonObject quote = anchor.value(QStringLiteral("quote")).toObject();
    const QString stored = anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
    // Without PDF regions (given in the HTML view) the quote is found in the answer text.
    const bool placedOnPdf = !anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray().isEmpty();
    return resolve(answerText, string(quote, QStringLiteral("exact")), string(quote, QStringLiteral("prefix")), string(quote, QStringLiteral("suffix")),
                   placedOnPdf && !stored.isEmpty() && stored == displayedPdfSha256);
}

AnswerText answerText(const QList<QList<Glyph>> &regionGlyphs)
{
    AnswerText result;
    for (qsizetype region = 0; region < regionGlyphs.size(); ++region) {
        if (region > 0) {
            result.text.append(QLatin1Char('\n'));
            result.source.append(-1);
        }
        for (const Glyph &glyph : regionGlyphs.at(region)) {
            const int index = int(result.glyphs.size());
            result.glyphs.append(glyph);
            result.text.append(glyph.text);
            for (qsizetype i = 0; i < glyph.text.size(); ++i) result.source.append(index);
        }
    }
    return result;
}

QList<QPair<int, QRectF>> lineRects(const QList<Glyph> &glyphs)
{
    QList<QPair<int, QRectF>> lines;
    for (const Glyph &glyph : glyphs) {
        if (blank(glyph.text) || !glyph.box.isValid()) continue;
        if (!lines.isEmpty()) {
            auto &line = lines.last();
            const double centre = glyph.box.center().y();
            // The same line: same page, the glyph's middle within the line's height, and not
            // far back to the left (the start of the next line).
            if (line.first == glyph.page && centre >= line.second.top() && centre <= line.second.bottom()
                && glyph.box.left() >= line.second.left() - line.second.height()) {
                line.second = line.second.united(glyph.box);
                continue;
            }
        }
        lines.append({glyph.page, glyph.box});
    }
    return lines;
}

QList<QPair<int, QRectF>> rangeRects(const AnswerText &answer, int start, int end)
{
    QList<Glyph> glyphs;
    int previous = -1;
    for (int i = std::max(0, start); i < end && i < answer.source.size(); ++i) {
        const int index = answer.source.at(i);
        if (index < 0 || index == previous) continue;
        previous = index;
        glyphs.append(answer.glyphs.at(index));
    }
    return lineRects(glyphs);
}

QList<QPair<int, QRectF>> regionsFromVariant(const QVariantList &regions)
{
    QList<QPair<int, QRectF>> result;
    for (const QVariant &value : regions) {
        const QVariantMap region = value.toMap();
        const QVariantList rect = region.value(QStringLiteral("rect")).toList();
        const int page = region.value(QStringLiteral("page")).toInt();
        if (page < 1 || rect.size() != 4) continue;
        result.append({page, QRectF(QPointF(rect.at(0).toDouble(), rect.at(1).toDouble()), QPointF(rect.at(2).toDouble(), rect.at(3).toDouble()))});
    }
    return result;
}

bool qualifies(const QList<QPair<int, QRectF>> &lines, const QList<QPair<int, QRectF>> &answerRegions)
{
    if (lines.isEmpty() || answerRegions.isEmpty()) return false;
    return std::all_of(lines.begin(), lines.end(), [&](const QPair<int, QRectF> &line) {
        const QPointF centre = line.second.center();
        return std::any_of(answerRegions.begin(), answerRegions.end(), [&](const QPair<int, QRectF> &region) {
            const QRectF grown = region.second.adjusted(-qualificationMargin, -qualificationMargin, qualificationMargin, qualificationMargin);
            return region.first == line.first && centre.x() >= grown.left() && centre.x() <= grown.right() && centre.y() >= grown.top()
                && centre.y() <= grown.bottom();
        });
    });
}

QJsonArray regionsJson(const QList<QPair<int, QRectF>> &lines)
{
    QJsonArray regions;
    const auto clamp = [](double value) { return std::round(std::clamp(value, 0.0, 1.0) * 10000.0) / 10000.0; };
    for (const auto &[page, rect] : lines) {
        regions.append(QJsonObject{{QStringLiteral("page"), page},
                                   {QStringLiteral("rect"), QJsonArray{clamp(rect.left()), clamp(rect.top()), clamp(rect.right()), clamp(rect.bottom())}}});
    }
    return regions;
}

QList<QPair<int, QRectF>> regionsOf(const QJsonObject &record)
{
    QList<QPair<int, QRectF>> result;
    for (const QJsonValue &value : record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray()) {
        const QJsonObject region = value.toObject();
        const QJsonArray rect = region.value(QStringLiteral("rect")).toArray();
        const int page = region.value(QStringLiteral("page")).toInt();
        if (page < 1 || rect.size() != 4) continue;
        result.append({page, QRectF(QPointF(rect.at(0).toDouble(), rect.at(1).toDouble()), QPointF(rect.at(2).toDouble(), rect.at(3).toDouble()))});
    }
    return result;
}

QString timestamp(const QDateTime &now)
{
    return now.toUTC().toString(Qt::ISODateWithMs);
}

QJsonObject anchor(const QString &exact, const QString &prefix, const QString &suffix, const QString &documentId, const QString &pdfSha256,
                   const QJsonArray &regions, const QString &segmentId, const QString &segmentContentSha256)
{
    QJsonObject result{{QStringLiteral("version"), 1},
                       {QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), exact.trimmed()},
                                                             {QStringLiteral("prefix"), prefix},
                                                             {QStringLiteral("suffix"), suffix}}},
                       {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("document_id"), documentId},
                                                           {QStringLiteral("pdf_sha256"), pdfSha256},
                                                           {QStringLiteral("regions"), regions}}}};
    if (!segmentId.isEmpty()) result.insert(QStringLiteral("segment_id"), segmentId);
    if (!segmentContentSha256.isEmpty()) result.insert(QStringLiteral("segment_content_sha256"), segmentContentSha256);
    return result;
}

QString cleanComment(const QString &comment)
{
    return leftCodePoints(comment.trimmed(), maximumComment);
}

QJsonObject create(const QString &cardId, const QString &polarity, const QString &comment, const QJsonObject &anchor, const QString &originId,
                   const QDateTime &now)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString at = timestamp(now);
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("feedback_id"), id},
        {QStringLiteral("card_id"), cardId},
        {QStringLiteral("polarity"), polarity},
        {QStringLiteral("comment"), cleanComment(comment)},
        {QStringLiteral("anchor"), anchor},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("created_at"), at},
        {QStringLiteral("updated_at"), at},
        {QStringLiteral("created_origin_id"), originId},
        {QStringLiteral("origin_id"), originId},
        {QStringLiteral("deleted"), false},
    };
}

namespace
{
// The next write of @p record: revision + 1, a later updated_at (never before the stored one, so
// a device whose clock runs behind still wins with its own next write), this origin.
QJsonObject nextWrite(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = record;
    QDateTime at = now.toUTC();
    const QDateTime previous = QDateTime::fromString(string(record, QStringLiteral("updated_at")), Qt::ISODateWithMs);
    if (previous.isValid() && previous.toUTC() >= at) at = previous.toUTC().addMSecs(1);
    next.insert(QStringLiteral("revision"), record.value(QStringLiteral("revision")).toInteger() + 1);
    next.insert(QStringLiteral("updated_at"), timestamp(at));
    next.insert(QStringLiteral("origin_id"), originId);
    return next;
}
}

QJsonObject edit(const QJsonObject &record, const QString &polarity, const QString &comment, const QString &originId, const QDateTime &now)
{
    QJsonObject next = nextWrite(record, originId, now);
    next.insert(QStringLiteral("polarity"), polarity);
    next.insert(QStringLiteral("comment"), cleanComment(comment));
    next.insert(QStringLiteral("deleted"), false);
    next.remove(QStringLiteral("deleted_at"));
    return next;
}

QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = nextWrite(record, originId, now);
    next.insert(QStringLiteral("deleted"), true);
    next.insert(QStringLiteral("deleted_at"), next.value(QStringLiteral("updated_at")));
    return next;
}

QJsonObject undo(const QJsonObject &tombstone, const QString &originId, const QDateTime &now)
{
    QJsonObject next = nextWrite(tombstone, originId, now);
    next.insert(QStringLiteral("deleted"), false);
    next.remove(QStringLiteral("deleted_at"));
    return next;
}

bool samePassage(const QJsonObject &record, const QJsonObject &anchor)
{
    const QJsonObject current = record.value(QStringLiteral("anchor")).toObject();
    const auto exact = [](const QJsonObject &a) { return a.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(); };
    const auto pdf = [](const QJsonObject &a) { return a.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString(); };
    return !exact(anchor).isEmpty() && exact(current) == exact(anchor) && pdf(current) == pdf(anchor);
}

namespace
{
std::optional<Replacement> replacement(const QJsonObject &current, QJsonObject created, const QString &originId, const QDateTime &now,
                                       const std::function<QString(const QJsonObject &)> &validation, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return std::optional<Replacement>();
    };
    if (current.isEmpty() || string(current, QStringLiteral("feedback_id")).isEmpty()) return fail(QStringLiteral("No such feedback."));
    if (current.value(QStringLiteral("deleted")).toBool()) return fail(QStringLiteral("This feedback was deleted."));
    const QString problem = validation(created);
    if (!problem.isEmpty()) return fail(problem);
    QJsonObject tombstone = remove(current, originId, now);
    return Replacement{std::move(created), std::move(tombstone)};
}
}

std::optional<Replacement> replacePassage(const QJsonObject &current, const QString &polarity, const QString &comment, const QJsonObject &anchor,
                                          const QString &originId, const QDateTime &now, QString *error)
{
    return replacement(current, create(string(current, QStringLiteral("card_id")), polarity, comment, anchor, originId, now), originId, now,
                       [](const QJsonObject &record) { return validationError(record); }, error);
}

QList<QJsonObject> undoReplacement(const Replacement &replacement, const QJsonObject &createdNow, const QJsonObject &oldNow, const QString &originId,
                                   const QDateTime &now)
{
    QList<QJsonObject> writes;
    // The new record goes, from what is stored now (Sync may have brought a later revision).
    const QJsonObject created = !createdNow.isEmpty() && !applies(replacement.created, createdNow) ? createdNow : replacement.created;
    if (!created.value(QStringLiteral("deleted")).toBool()) writes.append(remove(created, originId, now));
    // The old one comes back unless a later write already did that.
    const QJsonObject old = !oldNow.isEmpty() && !applies(replacement.tombstone, oldNow) ? oldNow : replacement.tombstone;
    if (old.value(QStringLiteral("deleted")).toBool()) writes.append(undo(old, originId, now));
    return writes;
}

bool applies(const QJsonObject &incoming, const QJsonObject &stored)
{
    if (stored.isEmpty()) return true;
    // The server's priority (spec §5): revision, then deleted (a delete beats an edit of the
    // same revision whatever the times or arrival order, so only Undo, a higher revision,
    // revives), then updated_at and origin_id as strings.
    const qint64 revision = incoming.value(QStringLiteral("revision")).toInteger();
    const qint64 storedRevision = stored.value(QStringLiteral("revision")).toInteger();
    if (revision != storedRevision) return revision > storedRevision;
    const bool deleted = incoming.value(QStringLiteral("deleted")).toBool();
    const bool storedDeleted = stored.value(QStringLiteral("deleted")).toBool();
    if (deleted != storedDeleted) return deleted;
    const QString updatedAt = string(incoming, QStringLiteral("updated_at"));
    const QString storedUpdatedAt = string(stored, QStringLiteral("updated_at"));
    if (updatedAt != storedUpdatedAt) return updatedAt > storedUpdatedAt;
    return string(incoming, QStringLiteral("origin_id")) > string(stored, QStringLiteral("origin_id"));
}

QJsonObject merge(const QJsonObject &stored, const QJsonObject &incoming)
{
    if (stored.isEmpty()) return incoming;
    if (!applies(incoming, stored)) return stored;
    QJsonObject result = incoming;
    for (const QString &key : {QStringLiteral("card_id"), QStringLiteral("anchor"), QStringLiteral("created_at"), QStringLiteral("created_origin_id")}) {
        if (stored.contains(key)) result.insert(key, stored.value(key));
    }
    return result;
}

void sortByCreation(QList<QJsonObject> &records)
{
    std::stable_sort(records.begin(), records.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const QDateTime at = QDateTime::fromString(string(a, QStringLiteral("created_at")), Qt::ISODateWithMs);
        const QDateTime bt = QDateTime::fromString(string(b, QStringLiteral("created_at")), Qt::ISODateWithMs);
        if (at.isValid() && bt.isValid() && at != bt) return at < bt;
        if (at.isValid() != bt.isValid()) return at.isValid();
        return string(a, QStringLiteral("feedback_id")) < string(b, QStringLiteral("feedback_id"));
    });
}

QList<QJsonObject> recordsOfCard(const QJsonObject &projections, const QString &cardId, bool includeDeleted)
{
    QList<QJsonObject> result;
    if (cardId.isEmpty()) return result;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (string(record, QStringLiteral("card_id")) != cardId || string(record, QStringLiteral("feedback_id")) != it.key().mid(prefix.size())) continue;
        if (!includeDeleted && record.value(QStringLiteral("deleted")).toBool()) continue;
        result.append(record);
    }
    sortByCreation(result);
    return result;
}

CardDocument documentOf(const QJsonObject &card, const QStringList &extraIds)
{
    CardDocument document;
    document.ids = QSet<QString>(extraIds.cbegin(), extraIds.cend());
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    document.ids.insert(string(primary, QStringLiteral("pdf_document_id")));
    document.ids.insert(string(primary, QStringLiteral("document_id")));
    document.ids.remove(QString());
    document.pdfStem = string(primary, QStringLiteral("pdf_stem"));
    return document;
}

QString documentIdOf(const QJsonObject &record)
{
    return record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("document_id")).toString();
}

bool inDocument(const QJsonObject &record, const QJsonObject &homeCard, const CardDocument &document)
{
    const QString anchored = documentIdOf(record);
    if (validUuid4(anchored) && !document.ids.isEmpty()) return document.ids.contains(anchored);
    // No document recorded with the passage, or none known for the card: the home card's.
    const CardDocument home = documentOf(homeCard);
    if (!home.ids.isEmpty() && !document.ids.isEmpty()) return home.ids.intersects(document.ids);
    return !home.pdfStem.isEmpty() && home.pdfStem == document.pdfStem;
}

bool isShared(const QJsonObject &record, const QString &cardId)
{
    return string(record, QStringLiteral("card_id")) != cardId;
}

QList<QJsonObject> recordsForCard(const QList<QJsonObject> &records, const QString &cardId, const CardDocument &document, const HomeCards &homeCards,
                                  bool includeDeleted)
{
    QList<QJsonObject> result;
    if (cardId.isEmpty()) return result;
    for (const QJsonObject &record : records) {
        if (!includeDeleted && record.value(QStringLiteral("deleted")).toBool()) continue;
        if (isShared(record, cardId)) {
            // Another card's: only while that card exists, and only in this card's document.
            const auto home = homeCards.constFind(string(record, QStringLiteral("card_id")));
            if (home == homeCards.cend() || !inDocument(record, *home, document)) continue;
        }
        result.append(record);
    }
    sortByCreation(result);
    return result;
}

QList<QJsonObject> recordsForCard(const QJsonObject &projections, const QString &cardId, const CardDocument &document, const HomeCards &homeCards,
                                  bool includeDeleted)
{
    QList<QJsonObject> records;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (string(record, QStringLiteral("feedback_id")) == it.key().mid(prefix.size())) records.append(record);
    }
    return recordsForCard(records, cardId, document, homeCards, includeDeleted);
}

Resolution resolveShared(const QJsonObject &record, const std::function<QString()> &answerText, const QString &displayedPdfSha256,
                         const QList<QPair<int, QRectF>> &answerRegions)
{
    const QString given = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
    if (!given.isEmpty() && given == displayedPdfSha256 && !regionsOf(record).isEmpty())
        return {qualifies(regionsOf(record), answerRegions) ? QStringLiteral("exact") : QStringLiteral("orphaned")};
    return resolve(record, answerText ? answerText() : QString(), QString());
}

QHash<QString, QString> sharedStates(const QString &cardId, const CardDocument &document, const HomeCards &homeCards, const QString &displayedPdfSha256,
                                     const QList<QPair<int, QRectF>> &answerRegions, const QList<QJsonObject> &records,
                                     const std::function<QString()> &answerText)
{
    QHash<QString, QString> result;
    std::optional<QString> text;
    const auto readText = [&]() -> QString {
        if (!text) text = answerText ? answerText() : QString();
        return *text;
    };
    for (const QJsonObject &record : recordsForCard(records, cardId, document, homeCards)) {
        if (!isShared(record, cardId)) continue;
        const Resolution resolution = resolveShared(record, readText, displayedPdfSha256, answerRegions);
        if (resolution.state != QLatin1String("orphaned")) result.insert(string(record, QStringLiteral("feedback_id")), resolution.state);
    }
    return result;
}

QString validationError(const QJsonObject &record)
{
    return fieldValidationError(record, true, QStringLiteral("answer feedback"));
}

QString fieldValidationError(const QJsonObject &record, bool requireCard, const QString &noun)
{
    const auto invalid = [&noun](const QString &what) { return QStringLiteral("The %1 has no valid %2.").arg(noun, what); };
    if (record.value(QStringLiteral("schema_version")).toInt() != 1 || !record.value(QStringLiteral("schema_version")).isDouble()) return invalid(QStringLiteral("schema_version"));
    if (!validUuid4(string(record, QStringLiteral("feedback_id")))) return invalid(QStringLiteral("feedback_id"));
    const qsizetype cardLength = codePoints(string(record, QStringLiteral("card_id")));
    if (requireCard && (!record.value(QStringLiteral("card_id")).isString() || cardLength < 1 || cardLength > 128)) return invalid(QStringLiteral("card_id"));
    const QString polarity = string(record, QStringLiteral("polarity"));
    if (polarity != positive() && polarity != negative() && (requireCard || polarity != neutral())) return invalid(QStringLiteral("polarity"));
    if (!record.value(QStringLiteral("comment")).isString() || codePoints(string(record, QStringLiteral("comment"))) > maximumComment) return invalid(QStringLiteral("comment"));
    const QJsonValue revision = record.value(QStringLiteral("revision"));
    if (!revision.isDouble() || revision.toDouble() < 1 || std::floor(revision.toDouble()) != revision.toDouble()) return invalid(QStringLiteral("revision"));
    for (const QString &key : {QStringLiteral("created_at"), QStringLiteral("updated_at")}) {
        if (!validTimestamp(string(record, key))) return invalid(key);
    }
    for (const QString &key : {QStringLiteral("created_origin_id"), QStringLiteral("origin_id")}) {
        const qsizetype length = codePoints(string(record, key));
        if (length < 1 || length > 128) return invalid(key);
    }
    if (record.contains(QStringLiteral("deleted")) && !record.value(QStringLiteral("deleted")).isBool()) return invalid(QStringLiteral("deleted"));
    const bool deleted = record.value(QStringLiteral("deleted")).toBool();
    if (deleted != record.contains(QStringLiteral("deleted_at")) || (deleted && !validTimestamp(string(record, QStringLiteral("deleted_at")))))
        return invalid(QStringLiteral("deleted_at"));
    const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    if (anchor.value(QStringLiteral("version")).toInt() != 1) return invalid(QStringLiteral("anchor version"));
    const QJsonObject quote = anchor.value(QStringLiteral("quote")).toObject();
    const QString exact = string(quote, QStringLiteral("exact"));
    if (exact.trimmed().isEmpty() || codePoints(exact) > maximumQuote) return invalid(QStringLiteral("quote"));
    for (const QString &key : {QStringLiteral("prefix"), QStringLiteral("suffix")}) {
        if (quote.contains(key) && (!quote.value(key).isString() || codePoints(string(quote, key)) > maximumContext)) return invalid(QStringLiteral("quote context"));
    }
    const QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    if (!validUuid4(string(pdf, QStringLiteral("document_id")))) return invalid(QStringLiteral("document"));
    if (!lowerHex64(string(pdf, QStringLiteral("pdf_sha256")))) return invalid(QStringLiteral("PDF fingerprint"));
    // Feedback given in the HTML view (view "html") is anchored by its quote and names the edition;
    // it has no PDF regions.
    const QString view = record.value(QStringLiteral("view")).toString(QStringLiteral("pdf"));
    // Document feedback made in an HTML attachment: DocumentFeedback::validationError checks the page.
    const bool attachment = !requireCard && view == QLatin1String("html_attachment");
    if (view != QLatin1String("pdf") && view != QLatin1String("html") && !attachment) return invalid(QStringLiteral("view"));
    const bool html = view == QLatin1String("html");
    if (html && !lowerHex64(string(record, QStringLiteral("html_edition_sha256")))) return invalid(QStringLiteral("HTML edition"));
    const QJsonArray regions = pdf.value(QStringLiteral("regions")).toArray();
    if ((regions.isEmpty() && !html && !attachment) || regions.size() > maximumRegions) return invalid(QStringLiteral("regions"));
    for (const QJsonValue &value : regions) {
        const QJsonObject region = value.toObject();
        const QJsonValue page = region.value(QStringLiteral("page"));
        const QJsonArray rect = region.value(QStringLiteral("rect")).toArray();
        if (!page.isDouble() || page.toDouble() < 1 || std::floor(page.toDouble()) != page.toDouble() || rect.size() != 4) return invalid(QStringLiteral("regions"));
        for (const QJsonValue &coordinate : rect) {
            if (!coordinate.isDouble() || coordinate.toDouble() < 0 || coordinate.toDouble() > 1) return invalid(QStringLiteral("regions"));
        }
        if (rect.at(0).toDouble() > rect.at(2).toDouble() || rect.at(1).toDouble() > rect.at(3).toDouble()) return invalid(QStringLiteral("regions"));
    }
    if (anchor.contains(QStringLiteral("segment_id"))) {
        const qsizetype length = codePoints(string(anchor, QStringLiteral("segment_id")));
        if (!anchor.value(QStringLiteral("segment_id")).isString() || length < 1 || length > 64) return invalid(QStringLiteral("segment_id"));
    }
    if (anchor.contains(QStringLiteral("segment_content_sha256")) && !lowerHex64(string(anchor, QStringLiteral("segment_content_sha256"))))
        return invalid(QStringLiteral("segment_content_sha256"));
    return QString();
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
        || outbox.enqueue(entityType(), string(record, QStringLiteral("feedback_id")), collection(), record.value(QStringLiteral("revision")).toInteger() - 1, record,
                          string(record, QStringLiteral("updated_at"))).isEmpty()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The answer feedback could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

bool enqueueAll(DesktopSyncOutbox &outbox, const QList<QJsonObject> &records, QString *error)
{
    QList<QJsonObject> operations;
    for (const QJsonObject &record : records) {
        const QString problem = validationError(record);
        if (!problem.isEmpty()) {
            if (error) *error = problem;
            return false;
        }
        operations.append({{QStringLiteral("entity_type"), entityType()}, {QStringLiteral("entity_id"), string(record, QStringLiteral("feedback_id"))},
                           {QStringLiteral("collection"), collection()}, {QStringLiteral("base_revision"), record.value(QStringLiteral("revision")).toInteger() - 1},
                           {QStringLiteral("client_timestamp"), string(record, QStringLiteral("updated_at"))}, {QStringLiteral("payload"), record}});
    }
    QString queueError;
    if (operations.isEmpty()) return true;
    if (!outbox.ready(&queueError) || outbox.enqueueBatch(operations, &queueError).size() != operations.size()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The answer feedback could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

QString agentSection(QList<QJsonObject> records, const QSet<QString> &orphaned, const QString &cardId, const QHash<QString, QString> &homeQuestions)
{
    records.removeIf([](const QJsonObject &record) { return record.value(QStringLiteral("deleted")).toBool(); });
    if (records.isEmpty()) return QString();
    sortByCreation(records);
    QStringList lines{QStringLiteral("Answer feedback:")};
    for (const QJsonObject &record : std::as_const(records)) {
        QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString().simplified();
        if (codePoints(quote) > 300) quote = leftCodePoints(quote, 300) + QStringLiteral("…");
        QString line = QStringLiteral("- %1: \"%2\"").arg(string(record, QStringLiteral("polarity")) == negative() ? QStringLiteral("Disliked") : QStringLiteral("Liked"), quote);
        QString comment = string(record, QStringLiteral("comment"));
        comment.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
        comment.replace(QLatin1Char('\n'), QLatin1Char(' '));
        comment.replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (!comment.trimmed().isEmpty()) line += QStringLiteral(" — ") + comment;
        // Shared: made on another card of the document, found in this card's answer.
        if (!cardId.isEmpty() && isShared(record, cardId)) {
            QString question = homeQuestions.value(string(record, QStringLiteral("card_id"))).simplified();
            if (codePoints(question) > 300) question = leftCodePoints(question, 300) + QStringLiteral("…");
            line += question.isEmpty() ? QStringLiteral(" (from another card)") : QStringLiteral(" (from another card: \"%1\")").arg(question);
        }
        if (orphaned.contains(string(record, QStringLiteral("feedback_id")))) line += QStringLiteral(" (not found in the current answer)");
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonObject agentJson(const QJsonObject &record, const QJsonObject &card, const QString &resolvedState)
{
    const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    const QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    QString state = resolvedState;
    if (state.isEmpty()) {
        const QString current = primary.value(QStringLiteral("pdf_sha256")).toString();
        state = !current.isEmpty() && string(pdf, QStringLiteral("pdf_sha256")) == current ? QStringLiteral("exact") : QStringLiteral("pdf_changed");
    }
    const QJsonArray regions = pdf.value(QStringLiteral("regions")).toArray();
    const QJsonValue page = regions.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(regions.first().toObject().value(QStringLiteral("page")).toInt());
    const QString segmentId = string(anchor, QStringLiteral("segment_id"));
    QJsonValue sourceChanged = QJsonValue::Null;
    const QString savedContent = string(anchor, QStringLiteral("segment_content_sha256"));
    if (!savedContent.isEmpty() && !segmentId.isEmpty()) {
        for (const QJsonValue &value : primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray()) {
            const QJsonObject segment = value.toObject();
            if (string(segment, QStringLiteral("segment_id")) != segmentId) continue;
            const QString current = string(segment, QStringLiteral("content_sha256"));
            if (!current.isEmpty()) sourceChanged = current != savedContent;
            break;
        }
    }
    const QString homeCard = string(record, QStringLiteral("card_id"));
    QJsonObject result{{QStringLiteral("feedback_id"), string(record, QStringLiteral("feedback_id"))},
                       {QStringLiteral("card_id"), homeCard},
                       {QStringLiteral("shared"), homeCard != string(card, QStringLiteral("card_id"))},
                       {QStringLiteral("polarity"), string(record, QStringLiteral("polarity"))},
                       {QStringLiteral("quote"), anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString()},
                       {QStringLiteral("comment"), string(record, QStringLiteral("comment"))},
                       {QStringLiteral("created_at"), string(record, QStringLiteral("created_at"))},
                       {QStringLiteral("updated_at"), string(record, QStringLiteral("updated_at"))},
                       {QStringLiteral("origin"), string(record, QStringLiteral("created_origin_id"))},
                       {QStringLiteral("anchor_state"), state},
                       {QStringLiteral("page"), page},
                       {QStringLiteral("segment_id"), segmentId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(segmentId)},
                       {QStringLiteral("source_changed"), sourceChanged}};
    if (record.value(QStringLiteral("deleted")).toBool()) {
        result.insert(QStringLiteral("deleted"), true);
        result.insert(QStringLiteral("deleted_at"), string(record, QStringLiteral("deleted_at")));
    }
    return result;
}

QJsonArray agentArray(QList<QJsonObject> records, const QJsonObject &card, bool includeDeleted, const QHash<QString, QString> &states)
{
    if (!includeDeleted) records.removeIf([](const QJsonObject &record) { return record.value(QStringLiteral("deleted")).toBool(); });
    sortByCreation(records);
    QJsonArray result;
    for (const QJsonObject &record : std::as_const(records)) result.append(agentJson(record, card, states.value(string(record, QStringLiteral("feedback_id")))));
    return result;
}
}
