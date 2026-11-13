// SPDX-License-Identifier: GPL-2.0-or-later
#include "documentfeedback.h"

#include "desktopsyncstore.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QUuid>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace Thoughts::DocumentFeedback
{
QString entityType()
{
    return QStringLiteral("document_feedback");
}

QString collection()
{
    return QStringLiteral("document-feedback");
}

QString capability()
{
    return QStringLiteral("document_feedback_v1");
}

QString attachmentCapability()
{
    return QStringLiteral("document_feedback_html_attachment_v1");
}

QString kindsCapability()
{
    return QStringLiteral("document_feedback_kinds_v1");
}

QString drawingCapability()
{
    return QStringLiteral("document_feedback_drawing_v1");
}

QString textCapability()
{
    return QStringLiteral("document_feedback_text_v1");
}

QString attachmentView()
{
    return QStringLiteral("html_attachment");
}

bool isAttachmentFeedback(const QJsonObject &record)
{
    return record.value(QStringLiteral("view")).toString() == attachmentView();
}

QString question()
{
    return QStringLiteral("question");
}

QString other()
{
    return QStringLiteral("other");
}

QString kindOf(const QJsonObject &record)
{
    return record.value(QStringLiteral("kind")).toString();
}

bool isResolved(const QJsonObject &record)
{
    return !record.value(QStringLiteral("resolved_at")).toString().isEmpty();
}

bool canResolve(const QJsonObject &record)
{
    return !kindOf(record).isEmpty() && !isPageMark(record);
}

bool isPageMark(const QJsonObject &record)
{
    return isDrawing(record) || isTextBox(record);
}

QRectF pageMarkBounds(const QJsonObject &record)
{
    return isTextBox(record) ? textBoxRect(record) : strokeBounds(record);
}

QString textBox()
{
    return QStringLiteral("text");
}

bool isTextBox(const QJsonObject &record)
{
    return kindOf(record) == textBox();
}

static QString textBoxField()
{
    return QStringLiteral("text_box");
}

QRectF textBoxRect(const QJsonObject &record)
{
    const QJsonArray rect = record.value(textBoxField()).toObject().value(QStringLiteral("rect")).toArray();
    if (rect.size() != 4) return {};
    return QRectF(QPointF(rect.at(0).toDouble(), rect.at(1).toDouble()), QPointF(rect.at(2).toDouble(), rect.at(3).toDouble()));
}

double textBoxFontSize(const QJsonObject &record)
{
    return record.value(textBoxField()).toObject().value(QStringLiteral("font_size")).toDouble();
}

// The rect on its page and the font size within their bounds; a field another client added stays.
static QJsonObject textBoxJson(QJsonObject box, const QRectF &rect, double fontSize)
{
    const auto rounded = [](double value) { return std::round(value * 10000.0) / 10000.0; };
    const double width = std::clamp(rounded(rect.width()), 0.0001, 1.0);
    const double height = std::clamp(rounded(rect.height()), 0.0001, 1.0);
    const double left = std::clamp(rounded(rect.left()), 0.0, 1.0 - width);
    const double top = std::clamp(rounded(rect.top()), 0.0, 1.0 - height);
    box.insert(QStringLiteral("rect"), QJsonArray{left, top, rounded(left + width), rounded(top + height)});
    box.insert(QStringLiteral("font_size"), std::clamp(rounded(fontSize), minimumFontSize, maximumFontSize));
    return box;
}

static bool validTextBox(const QJsonObject &record)
{
    const QJsonValue box = record.value(textBoxField());
    if (!box.isObject()) return false;
    const QJsonArray rect = box.toObject().value(QStringLiteral("rect")).toArray();
    if (rect.size() != 4 || !std::all_of(rect.cbegin(), rect.cend(), [](const QJsonValue &value) { return value.isDouble(); })) return false;
    const double left = rect.at(0).toDouble(), top = rect.at(1).toDouble(), right = rect.at(2).toDouble(), bottom = rect.at(3).toDouble();
    if (!(0.0 <= left && left < right && right <= 1.0 && 0.0 <= top && top < bottom && bottom <= 1.0)) return false;
    const QJsonValue fontSize = box.toObject().value(QStringLiteral("font_size"));
    if (!fontSize.isDouble() || fontSize.toDouble() < minimumFontSize || fontSize.toDouble() > maximumFontSize) return false;
    // A text box is its text.
    return !record.value(QStringLiteral("comment")).toString().trimmed().isEmpty();
}

QString drawing()
{
    return QStringLiteral("drawing");
}

bool isDrawing(const QJsonObject &record)
{
    return kindOf(record) == drawing();
}

QList<Stroke> strokesOf(const QJsonObject &record)
{
    QList<Stroke> result;
    for (const QJsonValue &value : record.value(drawing()).toObject().value(QStringLiteral("strokes")).toArray()) {
        const QJsonObject json = value.toObject();
        Stroke stroke;
        for (const QJsonValue &point : json.value(QStringLiteral("points")).toArray()) {
            stroke.points.append(QPointF(point.toArray().at(0).toDouble(), point.toArray().at(1).toDouble()));
        }
        // Anything but "#rrggbb" is the pencil's own colour.
        stroke.colour = strokeColour(json.value(QStringLiteral("color")).toString());
        result.append(stroke);
    }
    return result;
}

QString strokeColour(const QString &text)
{
    static const QRegularExpression rgb(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return rgb.match(text).hasMatch() ? text.toLower() : QString();
}

static double distanceToSegment(const QPointF &point, const QPointF &from, const QPointF &to)
{
    const QPointF along = to - from;
    const double length = QPointF::dotProduct(along, along);
    const double t = length > 0.0 ? std::clamp(QPointF::dotProduct(point - from, along) / length, 0.0, 1.0) : 0.0;
    const QPointF nearest = from + along * t;
    return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
}

static bool segmentsCross(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    const auto side = [](const QPointF &p, const QPointF &q, const QPointF &r) { return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x()); };
    const double d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

bool strokeNear(const Stroke &stroke, const QRectF &page, const QPointF &from, const QPointF &to, double radius)
{
    if (stroke.points.isEmpty() || page.isEmpty()) return false;
    const auto at = [&page](const QPointF &point) { return QPointF(page.left() + point.x() * page.width(), page.top() + point.y() * page.height()); };
    // The distance between two segments: none when they cross, else the shortest from an end of one
    // to the other.
    const auto near = [&](const QPointF &p, const QPointF &q) {
        return segmentsCross(p, q, from, to) || distanceToSegment(p, from, to) <= radius || distanceToSegment(q, from, to) <= radius
            || distanceToSegment(from, p, q) <= radius || distanceToSegment(to, p, q) <= radius;
    };
    QPointF previous = at(stroke.points.constFirst());
    if (stroke.points.size() == 1) return near(previous, previous);
    for (qsizetype i = 1; i < stroke.points.size(); ++i) {
        const QPointF next = at(stroke.points.at(i));
        if (near(previous, next)) return true;
        previous = next;
    }
    return false;
}

QRectF strokeBounds(const QJsonObject &record)
{
    double left = 1.0, top = 1.0, right = 0.0, bottom = 0.0;
    for (const Stroke &stroke : strokesOf(record)) {
        for (const QPointF &point : stroke.points) {
            left = std::min(left, point.x());
            top = std::min(top, point.y());
            right = std::max(right, point.x());
            bottom = std::max(bottom, point.y());
        }
    }
    if (left > right) return {};
    return QRectF(QPointF(left, top), QPointF(right, bottom)).adjusted(-0.005, -0.005, 0.005, 0.005).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

static QJsonObject drawingJson(const QList<Stroke> &strokes)
{
    // A tenth of a pixel on a page a thousand pixels wide, and a smaller record.
    const auto rounded = [](double value) { return std::round(value * 10000.0) / 10000.0; };
    QJsonArray result;
    for (const Stroke &stroke : strokes) {
        QJsonArray points;
        for (const QPointF &point : stroke.points) points.append(QJsonArray{rounded(point.x()), rounded(point.y())});
        QJsonObject json{{QStringLiteral("points"), points}};
        if (!stroke.colour.isEmpty()) json.insert(QStringLiteral("color"), stroke.colour);
        result.append(json);
    }
    return QJsonObject{{QStringLiteral("strokes"), result}};
}

static bool validDrawing(const QJsonObject &record)
{
    const QJsonArray strokes = record.value(drawing()).toObject().value(QStringLiteral("strokes")).toArray();
    if (strokes.isEmpty() || strokes.size() > maximumStrokes) return false;
    for (const QJsonValue &stroke : strokes) {
        const QJsonArray points = stroke.toObject().value(QStringLiteral("points")).toArray();
        if (points.isEmpty() || points.size() > maximumStrokePoints) return false;
        for (const QJsonValue &point : points) {
            const QJsonArray xy = point.toArray();
            if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble()) return false;
            const double x = xy.at(0).toDouble(), y = xy.at(1).toDouble();
            if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) return false;
        }
    }
    return true;
}

QJsonObject withKind(QJsonObject record, const QString &kind)
{
    if (kind.isEmpty()) {
        record.remove(QStringLiteral("kind"));
        record.remove(QStringLiteral("resolved_at"));
    } else {
        record.insert(QStringLiteral("kind"), kind);
        record.insert(QStringLiteral("polarity"), ReviewCardFeedback::neutral());
    }
    return record;
}

QJsonObject resolve(const QJsonObject &record, bool resolved, const QString &originId, const QDateTime &now)
{
    QJsonObject next = ReviewCardFeedback::edit(record, record.value(QStringLiteral("polarity")).toString(), record.value(QStringLiteral("comment")).toString(), originId, now);
    if (resolved) next.insert(QStringLiteral("resolved_at"), ReviewCardFeedback::timestamp(now));
    else next.remove(QStringLiteral("resolved_at"));
    return next;
}

bool shownOn(const QJsonObject &record, const QString &displayedPdfSha256, const QString &attachmentSha256)
{
    // A comment follows its text until resolved. A like, a dislike and a question belong to the
    // version they were given on (the user's rule, 10 November); a resolved question is done.
    if (kindOf(record) == other()) return !isResolved(record);
    if (canResolve(record) && isResolved(record)) return false;
    const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    if (isAttachmentFeedback(record)) return anchor.value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("sha256")).toString() == attachmentSha256;
    return anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString().compare(displayedPdfSha256, Qt::CaseInsensitive) == 0;
}

QJsonObject attachmentAnchor(const QJsonObject &placement, const QString &documentId, const QString &pdfSha256)
{
    const QJsonObject quote = placement.value(QStringLiteral("text_quote")).toObject();
    // Never half a character: the viewer's context is longer than feedback keeps.
    const auto start = [](const QString &text, int length) {
        return text.size() <= length ? text : text.left(text.at(length - 1).isHighSurrogate() ? length - 1 : length);
    };
    const auto end = [](const QString &text, int length) {
        return text.size() <= length ? text : text.mid(text.size() - length + (text.at(text.size() - length).isLowSurrogate() ? 1 : 0));
    };
    QJsonObject anchor = ReviewCardFeedback::anchor(start(quote.value(QStringLiteral("exact")).toString().trimmed(), ReviewCardFeedback::maximumQuote),
                                                    end(quote.value(QStringLiteral("prefix")).toString(), ReviewCardFeedback::contextLength),
                                                    start(quote.value(QStringLiteral("suffix")).toString(), ReviewCardFeedback::contextLength),
                                                    documentId, pdfSha256, QJsonArray());
    anchor.insert(QStringLiteral("html_attachment"), placement.value(QStringLiteral("html_attachment")));
    anchor.insert(QStringLiteral("page_state"), placement.value(QStringLiteral("page_state")));
    return anchor;
}

QString projectionKey(const QString &feedbackId)
{
    return entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n') + feedbackId;
}

QJsonObject create(const QString &polarity, const QString &comment, const QJsonObject &anchor, const QString &originId, const QDateTime &now)
{
    // Card feedback's first write without its card.
    QJsonObject record = ReviewCardFeedback::create(QString(), polarity, comment, anchor, originId, now);
    record.remove(QStringLiteral("card_id"));
    return record;
}

QJsonObject createDrawing(int page, const QList<Stroke> &strokes, const QString &documentId, const QString &pdfSha256, const QString &originId, const QDateTime &now)
{
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("Drawing"), QString(), QString(), documentId, pdfSha256,
                                                          ReviewCardFeedback::regionsJson({{page, QRectF(0.0, 0.0, 1.0, 1.0)}}));
    QJsonObject record = withKind(create(ReviewCardFeedback::neutral(), QString(), anchor, originId, now), drawing());
    record.insert(drawing(), drawingJson(strokes));
    return record;
}

QJsonObject createTextBox(int page, const QRectF &rect, double fontSize, const QString &text, const QString &documentId, const QString &pdfSha256,
                          const QString &originId, const QDateTime &now)
{
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("Text"), QString(), QString(), documentId, pdfSha256,
                                                          ReviewCardFeedback::regionsJson({{page, QRectF(0.0, 0.0, 1.0, 1.0)}}));
    QJsonObject record = withKind(create(ReviewCardFeedback::neutral(), text, anchor, originId, now), textBox());
    record.insert(textBoxField(), textBoxJson({}, rect, fontSize));
    return record;
}

QJsonObject withTextBox(const QJsonObject &record, const QRectF &rect, double fontSize, const QString &text, const QString &originId, const QDateTime &now)
{
    QJsonObject next = edit(record, record.value(QStringLiteral("polarity")).toString(), text, originId, now);
    next.insert(textBoxField(), textBoxJson(record.value(textBoxField()).toObject(), rect, fontSize));
    return next;
}

QJsonObject edit(const QJsonObject &record, const QString &polarity, const QString &comment, const QString &originId, const QDateTime &now)
{
    return ReviewCardFeedback::edit(record, polarity, comment, originId, now);
}

QJsonObject withStrokes(const QJsonObject &record, const QList<Stroke> &strokes, const QString &originId, const QDateTime &now)
{
    QJsonObject next = edit(record, record.value(QStringLiteral("polarity")).toString(), record.value(QStringLiteral("comment")).toString(), originId, now);
    next.insert(drawing(), drawingJson(strokes));
    return next;
}

QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    return ReviewCardFeedback::remove(record, originId, now);
}

QJsonObject undo(const QJsonObject &tombstone, const QString &originId, const QDateTime &now)
{
    return ReviewCardFeedback::undo(tombstone, originId, now);
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
    // Fixed by the first write (spec §3).
    for (const QString &key : {QStringLiteral("anchor"), QStringLiteral("created_at"), QStringLiteral("created_origin_id"), QStringLiteral("view")}) {
        if (stored.contains(key)) result.insert(key, stored.value(key));
        else result.remove(key);
    }
    return result;
}

QString validationError(const QJsonObject &record)
{
    const auto invalid = [](const QString &what) { return QStringLiteral("The document feedback has no valid %1.").arg(what); };
    // Card feedback is review_card_feedback; the server refuses a card here.
    if (record.contains(QStringLiteral("card_id"))) return QStringLiteral("Document feedback cannot name a card.");
    // Feedback made in the HTML edition view is not supported; an HTML attachment is (§8).
    const bool attachment = isAttachmentFeedback(record);
    if (record.contains(QStringLiteral("view")) && record.value(QStringLiteral("view")).toString() != QLatin1String("pdf") && !attachment) return invalid(QStringLiteral("view"));
    const QJsonValue pdf = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf"));
    if (!pdf.isObject()) return invalid(QStringLiteral("PDF anchor"));
    const qsizetype regions = pdf.toObject().value(QStringLiteral("regions")).toArray().size();
    if (attachment) {
        const QJsonObject page = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject();
        const QString path = page.value(QStringLiteral("path")).toString();
        static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
        if (path.isEmpty() || path.size() > 1024 || !sha256.match(page.value(QStringLiteral("sha256")).toString()).hasMatch()) return invalid(QStringLiteral("HTML attachment"));
        if (regions != 0) return invalid(QStringLiteral("regions"));
    } else if (regions < 1 || regions > ReviewCardFeedback::maximumRegions) {
        return invalid(QStringLiteral("regions"));
    }
    const QString kind = kindOf(record);
    if ((record.contains(QStringLiteral("kind")) && kind != question() && kind != other() && kind != drawing() && kind != textBox())
        || kind.isEmpty() == (record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::neutral())) return invalid(QStringLiteral("kind"));
    if (record.contains(drawing()) != isDrawing(record) || (isDrawing(record) && !validDrawing(record))) return invalid(QStringLiteral("drawing"));
    if (record.contains(textBoxField()) != isTextBox(record) || (isTextBox(record) && !validTextBox(record))) return invalid(QStringLiteral("text box"));
    if (record.contains(QStringLiteral("resolved_at"))
        && (!canResolve(record) || !QDateTime::fromString(record.value(QStringLiteral("resolved_at")).toString(), Qt::ISODateWithMs).isValid())) return invalid(QStringLiteral("resolution"));
    return ReviewCardFeedback::fieldValidationError(record, false, QStringLiteral("document feedback"));
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
        || outbox.enqueue(entityType(), record.value(QStringLiteral("feedback_id")).toString(), collection(), record.value(QStringLiteral("revision")).toInteger() - 1,
                          record, record.value(QStringLiteral("updated_at")).toString()).isEmpty()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The document feedback could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

QString documentIdOf(const QJsonObject &record)
{
    return ReviewCardFeedback::documentIdOf(record);
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
        operations.append({{QStringLiteral("entity_type"), entityType()}, {QStringLiteral("entity_id"), record.value(QStringLiteral("feedback_id")).toString()},
                           {QStringLiteral("collection"), collection()}, {QStringLiteral("base_revision"), record.value(QStringLiteral("revision")).toInteger() - 1},
                           {QStringLiteral("client_timestamp"), record.value(QStringLiteral("updated_at")).toString()}, {QStringLiteral("payload"), record}});
    }
    QString queueError;
    if (operations.isEmpty()) return true;
    if (!outbox.ready(&queueError) || outbox.enqueueBatch(operations, &queueError).size() != operations.size()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The document feedback could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}

std::optional<ReviewCardFeedback::Replacement> replacePassage(const QJsonObject &current, const QString &polarity, const QString &kind, const QString &comment,
                                                              const QJsonObject &anchor, const QString &originId, const QDateTime &now, QString *error)
{
    if (isPageMark(current)) {
        if (error) *error = isDrawing(current) ? QStringLiteral("A drawing has no passage to change.") : QStringLiteral("A text box has no passage to change.");
        return std::nullopt;
    }
    if (current.isEmpty() || current.value(QStringLiteral("feedback_id")).toString().isEmpty()) {
        if (error) *error = QStringLiteral("No such feedback.");
        return std::nullopt;
    }
    if (current.value(QStringLiteral("deleted")).toBool()) {
        if (error) *error = QStringLiteral("This feedback was deleted.");
        return std::nullopt;
    }
    QJsonObject created = withKind(create(polarity, comment, anchor, originId, now), kind);
    if (!kind.isEmpty() && current.contains(QStringLiteral("resolved_at"))) created.insert(QStringLiteral("resolved_at"), current.value(QStringLiteral("resolved_at")));
    const QString problem = validationError(created);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return std::nullopt;
    }
    return ReviewCardFeedback::Replacement{created, remove(current, originId, now)};
}

QList<QJsonObject> recordsOfDocument(const QList<QJsonObject> &records, const QString &documentId, bool includeDeleted)
{
    QList<QJsonObject> result;
    if (documentId.isEmpty()) return result;
    for (const QJsonObject &record : records) {
        if (record.contains(QStringLiteral("card_id")) || documentIdOf(record).compare(documentId, Qt::CaseInsensitive) != 0) continue;
        if (!includeDeleted && record.value(QStringLiteral("deleted")).toBool()) continue;
        result.append(record);
    }
    ReviewCardFeedback::sortByCreation(result);
    return result;
}

QList<QJsonObject> recordsOfDocument(const QJsonObject &projections, const QString &documentId, bool includeDeleted)
{
    QList<QJsonObject> records;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (record.value(QStringLiteral("feedback_id")).toString() == it.key().mid(prefix.size())) records.append(record);
    }
    return recordsOfDocument(records, documentId, includeDeleted);
}

QList<int> searchPages(const QList<int> &anchorPages, int pageCount, bool widened)
{
    const int window = widened ? pageWindow : 0;
    QList<int> pages;
    for (const int page : anchorPages) {
        for (int candidate = page - window; candidate <= page + window; ++candidate) {
            if (candidate >= 1 && candidate <= pageCount && !pages.contains(candidate)) pages.append(candidate);
        }
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

Placement place(const QJsonObject &record, const QString &displayedPdfSha256, int pageCount,
                const std::function<ReviewCardFeedback::AnswerText(const QList<int> &pages)> &pageText)
{
    // Its passage is in the page, never on the PDF.
    if (isAttachmentFeedback(record)) return {QStringLiteral("orphaned"), {}};
    const auto regions = ReviewCardFeedback::regionsOf(record);
    const QString given = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
    // Drawn or typed over what was on the page: there is no passage to look for on another version.
    if (isPageMark(record)) {
        if (displayedPdfSha256.isEmpty() || given.compare(displayedPdfSha256, Qt::CaseInsensitive) != 0 || regions.isEmpty()) return {QStringLiteral("orphaned"), {}};
        return {QStringLiteral("exact"), {{regions.constFirst().first, pageMarkBounds(record)}}};
    }
    // Same PDF: the regions as they are.
    if (!displayedPdfSha256.isEmpty() && given.compare(displayedPdfSha256, Qt::CaseInsensitive) == 0 && !regions.isEmpty())
        return {QStringLiteral("exact"), regions};
    QList<int> anchorPages;
    for (const auto &region : regions) {
        if (!anchorPages.contains(region.first)) anchorPages.append(region.first);
    }
    QList<int> searched;
    for (const bool widened : {false, true}) {
        const QList<int> pages = searchPages(anchorPages, pageCount, widened);
        if (pages.isEmpty() || pages == searched || !pageText) continue;
        searched = pages;
        const ReviewCardFeedback::AnswerText text = pageText(pages);
        const auto resolution = ReviewCardFeedback::resolve(record, text.text, QString());
        if (resolution.state != QLatin1String("relocated")) continue;
        const auto rects = ReviewCardFeedback::rangeRects(text, resolution.start, resolution.end);
        if (!rects.isEmpty()) return {QStringLiteral("relocated"), rects};
    }
    return {QStringLiteral("orphaned"), {}};
}

QPair<QString, QString> context(const ReviewCardFeedback::AnswerText &text, const std::function<bool(int, const QPointF &)> &selected)
{
    int first = -1, last = -1;
    for (int i = 0; i < text.source.size(); ++i) {
        const int index = text.source.at(i);
        if (index < 0) continue;
        const ReviewCardFeedback::Glyph &glyph = text.glyphs.at(index);
        if (!selected || !selected(glyph.page, glyph.box.center())) continue;
        if (first < 0) first = i;
        last = i + 1;
    }
    if (first < 0) return {};
    while (first < last && text.text.at(first).isSpace()) ++first;
    while (last > first && text.text.at(last - 1).isSpace()) --last;
    int from = std::max(0, first - ReviewCardFeedback::contextLength);
    if (from > 0 && text.text.at(from).isLowSurrogate()) ++from;
    int to = std::min(int(text.text.size()), last + ReviewCardFeedback::contextLength);
    if (to < text.text.size() && text.text.at(to).isLowSurrogate()) --to;
    return {text.text.mid(from, first - from), text.text.mid(last, to - last)};
}
}
