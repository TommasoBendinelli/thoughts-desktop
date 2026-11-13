// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewfeedbackoverlay.h"

#include "thoughtsplatform.h"
#include "feedbackicon.h"
#include "feedbackversion.h"

#include "core/area.h"
#include "core/document.h"
#include "core/page.h"
#include "core/textpage.h"

#include <KLocalizedString>

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace Thoughts
{
ReviewFeedbackOverlay::ReviewFeedbackOverlay(Okular::Document *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
{
}

void ReviewFeedbackOverlay::setCard(const QJsonObject &card, const QVariantList &regions, const QString &documentId, const QString &pdfSha256)
{
    const QString cardId = card.value(QStringLiteral("card_id")).toString();
    const auto answerRegions = ReviewCardFeedback::regionsFromVariant(regions);
    if (cardId == m_cardId && answerRegions == m_regions && pdfSha256 == m_pdfSha256 && documentId == m_documentId && card == m_card) return;
    if (cardId != m_cardId) m_records.clear();
    m_card = card;
    m_cardId = cardId;
    m_regions = answerRegions;
    m_documentId = documentId;
    m_answerDocument = ReviewCardFeedback::documentOf(card, {documentId});
    m_pdfSha256 = pdfSha256;
    m_answer.reset();
    m_items.reset();
    Q_EMIT changed();
}

void ReviewFeedbackOverlay::setRecords(const QList<QJsonObject> &records)
{
    // Its own records and those of other cards of its document (shared feedback).
    const QList<QJsonObject> own = ReviewCardFeedback::recordsForCard(records, m_cardId, m_answerDocument, m_homeCards, true);
    if (own == m_records) return;
    m_records = own;
    m_items.reset();
    Q_EMIT changed();
}

void ReviewFeedbackOverlay::clear()
{
    if (m_cardId.isEmpty() && m_records.isEmpty()) return;
    m_card = {};
    m_cardId.clear();
    m_hidden.clear();
    m_regions.clear();
    m_documentId.clear();
    m_answerDocument = {};
    m_homeCards.clear();
    m_pdfSha256.clear();
    m_records.clear();
    m_answer.reset();
    m_items.reset();
    Q_EMIT changed();
}

bool ReviewFeedbackOverlay::hasCard() const
{
    return !m_cardId.isEmpty();
}

QString ReviewFeedbackOverlay::cardId() const
{
    return m_cardId;
}

void ReviewFeedbackOverlay::setHomeCards(const ReviewCardFeedback::HomeCards &cards)
{
    m_homeCards = cards;
}

ReviewCardFeedback::HomeCards ReviewFeedbackOverlay::homeCards() const
{
    return m_homeCards;
}

ReviewCardFeedback::CardDocument ReviewFeedbackOverlay::document() const
{
    return m_answerDocument;
}

void ReviewFeedbackOverlay::setReviewVersion(const QJsonObject &version)
{
    m_reviewVersion = version;
}

QJsonObject ReviewFeedbackOverlay::reviewVersion() const
{
    return m_reviewVersion;
}

QJsonObject ReviewFeedbackOverlay::card() const
{
    return m_card;
}

void ReviewFeedbackOverlay::setWriter(Writer writer)
{
    m_writer = std::move(writer);
}

QList<QPair<int, QRectF>> ReviewFeedbackOverlay::answerRegions() const
{
    return m_regions;
}

QList<ReviewCardFeedback::Glyph> ReviewFeedbackOverlay::glyphs(int page, const QRectF &rect) const
{
    QList<ReviewCardFeedback::Glyph> result;
    if (!m_document || page < 1 || page > int(m_document->pages())) return result;
    const Okular::Page *pdfPage = m_document->page(page - 1);
    if (!pdfPage) return result;
    if (!pdfPage->hasTextPage()) m_document->requestTextPage(page - 1);
    Okular::RegularAreaRect area;
    area.append(Okular::NormalizedRect(rect.left(), rect.top(), rect.right(), rect.bottom()));
    // The characters whose box centre lies in the region, in the text layer's reading order.
    for (const Okular::TextEntity &entity : pdfPage->words(&area, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour)) {
        const Okular::NormalizedRect box = entity.area();
        result.append({page, entity.text(), QRectF(QPointF(box.left, box.top), QPointF(box.right, box.bottom))});
    }
    return result;
}

void ReviewFeedbackOverlay::ensureAnswer()
{
    if (m_answer) return;
    QList<QList<ReviewCardFeedback::Glyph>> regions;
    for (const auto &[page, rect] : std::as_const(m_regions)) regions.append(glyphs(page, rect));
    m_answer = ReviewCardFeedback::answerText(regions);
}

QList<ReviewFeedbackOverlay::Item> ReviewFeedbackOverlay::items()
{
    if (m_items) return *m_items;
    QList<Item> result;
    // The answer's text is read only when there is feedback to find in it (a whole-document
    // card's answer is every page).
    if (std::none_of(m_records.cbegin(), m_records.cend(), [](const QJsonObject &record) { return !record.value(QStringLiteral("deleted")).toBool(); })) {
        m_items = result;
        return result;
    }
    ensureAnswer();
    for (const QJsonObject &record : std::as_const(m_records)) {
        if (record.value(QStringLiteral("deleted")).toBool()) continue;
        Item item;
        item.record = record;
        // Another card's record: shown only where it lies in this answer.
        const bool shared = ReviewCardFeedback::isShared(record, m_cardId);
        const auto resolution = shared ? ReviewCardFeedback::resolveShared(record, [this] { return m_answer->text; }, m_pdfSha256, m_regions)
                                       : ReviewCardFeedback::resolve(record, m_answer->text, m_pdfSha256);
        if (shared && resolution.state == QLatin1String("orphaned")) continue;
        item.state = resolution.state;
        if (resolution.state == QLatin1String("exact")) {
            item.rects = ReviewCardFeedback::regionsOf(record);
            // Its place in the answer, for the list: where its text is, else after the found ones.
            const auto located = ReviewCardFeedback::resolve(record, m_answer->text, QString());
            item.position = located.start >= 0 ? located.start : std::numeric_limits<int>::max();
        } else if (resolution.state == QLatin1String("relocated")) {
            item.rects = ReviewCardFeedback::rangeRects(*m_answer, resolution.start, resolution.end);
            item.position = resolution.start;
        } else {
            item.position = std::numeric_limits<qint64>::max();
        }
        result.append(item);
    }
    std::stable_sort(result.begin(), result.end(), [](const Item &a, const Item &b) {
        const bool aOrphaned = a.state == QLatin1String("orphaned"), bOrphaned = b.state == QLatin1String("orphaned");
        if (aOrphaned != bOrphaned) return bOrphaned;
        return a.position < b.position;
    });
    m_items = result;
    return result;
}

QVariantList ReviewFeedbackOverlay::itemMaps()
{
    QVariantList result;
    for (const Item &item : items()) {
        result.append(QVariantMap{
            {QStringLiteral("feedback_id"), item.record.value(QStringLiteral("feedback_id")).toString()},
            {QStringLiteral("polarity"), item.record.value(QStringLiteral("polarity")).toString()},
            {QStringLiteral("comment"), item.record.value(QStringLiteral("comment")).toString()},
            {QStringLiteral("quote"), item.record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString()},
            {QStringLiteral("state"), item.state},
            {QStringLiteral("created_at"), item.record.value(QStringLiteral("created_at")).toString()},
            {QStringLiteral("updated_at"), item.record.value(QStringLiteral("updated_at")).toString()},
            {QStringLiteral("revision"), item.record.value(QStringLiteral("revision")).toInteger()},
            // Where the passage was marked: its regions on the PDF it was given on.
            {QStringLiteral("anchor_pdf_sha256"), item.record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString()},
            {QStringLiteral("anchor_regions"), ReviewCardFeedback::regionsJson(ReviewCardFeedback::regionsOf(item.record)).toVariantList()},
            {QStringLiteral("displayed_pdf_sha256"), m_pdfSha256},
            // The card it was made on: another one for shared feedback.
            {QStringLiteral("card_id"), item.record.value(QStringLiteral("card_id")).toString()},
            {QStringLiteral("shared"), ReviewCardFeedback::isShared(item.record, m_cardId)},
        });
    }
    return result;
}

QVariantMap ReviewFeedbackOverlay::states()
{
    QVariantMap result;
    for (const Item &item : items()) result.insert(item.record.value(QStringLiteral("feedback_id")).toString(), item.state);
    return result;
}

QJsonObject ReviewFeedbackOverlay::record(const QString &feedbackId) const
{
    for (const QJsonObject &record : m_records) {
        if (record.value(QStringLiteral("feedback_id")).toString() == feedbackId) return record;
    }
    return {};
}

bool ReviewFeedbackOverlay::qualifies(const QList<QPair<int, QRectF>> &lines) const
{
    return hasCard() && ReviewCardFeedback::qualifies(lines, m_regions);
}

QJsonObject ReviewFeedbackOverlay::newRecord(const QString &polarity, const QString &comment, const QList<QPair<int, QRectF>> &lines, const QString &text,
                                             const std::function<bool(int, const QPointF &)> &selected, QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QJsonObject();
    };
    if (!qualifies(lines)) return fail(i18n("Select a passage inside the answer."));
    if (lines.size() > ReviewCardFeedback::maximumRegions) return fail(i18n("Select a shorter passage."));
    const QString exact = text.trimmed();
    if (exact.isEmpty()) return fail(i18n("The selection has no text."));
    // Context: the answer text right before and after the selected characters.
    QString prefix, suffix;
    const_cast<ReviewFeedbackOverlay *>(this)->ensureAnswer();
    const ReviewCardFeedback::AnswerText &answer = *m_answer;
    int first = -1, last = -1;
    for (int i = 0; i < answer.source.size(); ++i) {
        const int index = answer.source.at(i);
        if (index < 0) continue;
        const ReviewCardFeedback::Glyph &glyph = answer.glyphs.at(index);
        if (!selected(glyph.page, glyph.box.center())) continue;
        if (first < 0) first = i;
        last = i + 1;
    }
    if (first >= 0) {
        while (first < last && answer.text.at(first).isSpace()) ++first;
        while (last > first && answer.text.at(last - 1).isSpace()) --last;
        int from = std::max(0, first - ReviewCardFeedback::contextLength);
        if (from > 0 && answer.text.at(from).isLowSurrogate()) ++from;
        prefix = answer.text.mid(from, first - from);
        int to = std::min(int(answer.text.size()), last + ReviewCardFeedback::contextLength);
        if (to < answer.text.size() && answer.text.at(to).isLowSurrogate()) --to;
        suffix = answer.text.mid(last, to - last);
    }
    // The source segment, when the card has exactly one.
    QString segmentId, segmentSha256;
    const QJsonArray segments = m_card.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    if (segments.size() == 1) {
        segmentId = segments.first().toObject().value(QStringLiteral("segment_id")).toString();
        segmentSha256 = segments.first().toObject().value(QStringLiteral("content_sha256")).toString();
        if (segmentId.isEmpty() || segmentId.size() > 64 || segmentSha256.size() != 64 || segmentSha256 != segmentSha256.toLower()) segmentId.clear(), segmentSha256.clear();
    }
    const QJsonObject anchor = ReviewCardFeedback::anchor(exact, prefix, suffix, m_documentId, m_pdfSha256, ReviewCardFeedback::regionsJson(lines), segmentId, segmentSha256);
    const QJsonObject record = FeedbackVersion::stamped(ReviewCardFeedback::create(m_cardId, polarity, comment, anchor, Platform::reviewOriginId()), m_reviewVersion);
    const QString problem = ReviewCardFeedback::validationError(record);
    if (!problem.isEmpty()) return fail(problem);
    return record;
}

bool ReviewFeedbackOverlay::writeAll(const QList<QJsonObject> &written, QString *error)
{
    if (!m_batchWriter && !m_writer) {
        if (error) *error = i18n("Feedback cannot be saved here.");
        return false;
    }
    if (m_batchWriter) {
        if (!m_batchWriter(written, error)) return false;
    } else {
        for (const QJsonObject &record : written) {
            if (!m_writer(record, error)) return false;
        }
    }
    QList<QJsonObject> records = m_records;
    for (const QJsonObject &record : written) {
        const QString id = record.value(QStringLiteral("feedback_id")).toString();
        bool found = false;
        for (QJsonObject &existing : records) {
            if (existing.value(QStringLiteral("feedback_id")).toString() == id) {
                existing = ReviewCardFeedback::merge(existing, record);
                found = true;
            }
        }
        if (!found) records.append(record);
    }
    setRecords(records);
    return true;
}

void ReviewFeedbackOverlay::setBatchWriter(BatchWriter writer)
{
    m_batchWriter = std::move(writer);
}

void ReviewFeedbackOverlay::setHiddenFeedback(const QString &feedbackId)
{
    if (m_hidden == feedbackId) return;
    m_hidden = feedbackId;
    Q_EMIT changed();
}

QString ReviewFeedbackOverlay::hiddenFeedback() const
{
    return m_hidden;
}

bool ReviewFeedbackOverlay::write(const QJsonObject &record, QString *error)
{
    if (!m_writer) {
        if (error) *error = i18n("Feedback cannot be saved here.");
        return false;
    }
    if (!m_writer(record, error)) return false;
    // Shown at once; the projections bring the same record back.
    const QString id = record.value(QStringLiteral("feedback_id")).toString();
    QList<QJsonObject> records = m_records;
    bool found = false;
    for (QJsonObject &existing : records) {
        if (existing.value(QStringLiteral("feedback_id")).toString() == id) {
            existing = ReviewCardFeedback::merge(existing, record);
            found = true;
        }
    }
    if (!found) records.append(record);
    setRecords(records);
    return true;
}

// A smooth line through the points of each stroke, on @p page in painter coordinates.
static QPainterPath strokesPath(const QList<DocumentFeedback::Stroke> &strokes, const QRectF &page)
{
    QPainterPath path;
    const auto at = [&page](const QPointF &point) { return QPointF(page.left() + point.x() * page.width(), page.top() + point.y() * page.height()); };
    for (const DocumentFeedback::Stroke &drawn : strokes) {
        const QList<QPointF> &stroke = drawn.points;
        if (stroke.isEmpty()) continue;
        path.moveTo(at(stroke.constFirst()));
        // A click is a dot: a line too short to see, with round ends.
        if (stroke.size() == 1) path.lineTo(at(stroke.constFirst()) + QPointF(0.01, 0.0));
        for (int i = 1; i + 1 < stroke.size(); ++i) path.quadTo(at(stroke.at(i)), (at(stroke.at(i)) + at(stroke.at(i + 1))) / 2.0);
        if (stroke.size() > 1) path.lineTo(at(stroke.constLast()));
    }
    return path;
}

// Text boxes are laid out on a page this many units high, whatever the zoom, so that a line breaks at
// the same word at every zoom; the painter scales that page to the one shown.
static constexpr double textLayoutHeight = 10000.0;

QFont ReviewFeedbackOverlay::textBoxFont(double pixelSize)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPixelSize(std::max(1, qRound(pixelSize)));
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

// Lays @p text out @p width wide in @p font (line breaks kept, long words broken), paints it at the
// painter's origin when there is a painter, and gives the width of its longest line and its height.
static QSizeF layOutText(QPainter *painter, const QString &text, const QFont &font, double width)
{
    QString lines = text;
    lines.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(lines, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.setCacheEnabled(true);
    double height = 0.0;
    double widest = 0.0;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        line.setPosition(QPointF(0.0, height));
        height += line.height();
        widest = std::max(widest, line.naturalTextWidth());
    }
    layout.endLayout();
    if (painter) layout.draw(painter, QPointF(0.0, 0.0));
    return {widest, height};
}

QSizeF ReviewFeedbackOverlay::textBoxSize(const QString &text, double fontSize, double maximumWidth, double aspect)
{
    if (aspect <= 0.0) return {};
    const double pageWidth = textLayoutHeight * aspect;
    const QFont font = textBoxFont(fontSize * textLayoutHeight);
    // A tenth of a letter more than the longest line, so that rounding never wraps it.
    const double spare = fontSize * textLayoutHeight * 0.1;
    const double natural = layOutText(nullptr, text, font, std::numeric_limits<float>::max()).width() + spare;
    const double width = std::clamp(natural, std::min(spare * 10.0, maximumWidth * pageWidth), maximumWidth * pageWidth);
    const double height = layOutText(nullptr, text, font, width).height();
    return {width / pageWidth, std::min(1.0, height / textLayoutHeight)};
}

void ReviewFeedbackOverlay::paintTextBox(QPainter *painter, int page, const QRectF &box, double fontSize, const QString &text,
                                         const std::function<QRectF(int, const QRectF &)> &map, bool dark, int frame)
{
    const QRectF shown = map(page, QRectF(0.0, 0.0, 1.0, 1.0));
    if (shown.isEmpty() || box.isEmpty()) return;
    const QColor accent = colour(ReviewCardFeedback::neutral(), dark);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (frame > 0) {
        const QRectF around = QRectF(shown.left() + box.left() * shown.width(), shown.top() + box.top() * shown.height(), box.width() * shown.width(),
                                     box.height() * shown.height()).adjusted(-3.0, -2.0, 3.0, 2.0);
        QColor line = accent;
        if (frame == 1) line.setAlphaF(0.45);
        painter->setPen(QPen(line, frame == 2 ? 1.5 : 1.0, frame == 2 ? Qt::SolidLine : Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(around, 3.0, 3.0);
    }
    const double scale = shown.height() / textLayoutHeight;
    painter->translate(shown.left() + box.left() * shown.width(), shown.top() + box.top() * shown.height());
    painter->scale(scale, scale);
    painter->setPen(accent);
    layOutText(painter, text, textBoxFont(fontSize * textLayoutHeight), box.width() * shown.width() / scale);
    painter->restore();
}

void ReviewFeedbackOverlay::paintStrokes(QPainter *painter, int page, const QList<DocumentFeedback::Stroke> &strokes, const std::function<QRectF(int, const QRectF &)> &map,
                                         bool dark)
{
    const QRectF shown = map(page, QRectF(0.0, 0.0, 1.0, 1.0));
    if (shown.isEmpty()) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    // Oldest first, each in its colour; a run of one colour is one path, so its crossings look the same.
    for (qsizetype start = 0; start < strokes.size();) {
        const QString colour = strokes.at(start).colour;
        qsizetype end = start + 1;
        while (end < strokes.size() && strokes.at(end).colour == colour) ++end;
        // As thick on the page at every zoom.
        painter->setPen(QPen(colour.isEmpty() ? ReviewFeedbackOverlay::colour(ReviewCardFeedback::neutral(), dark) : QColor::fromString(colour),
                             std::max(1.5, shown.width() * 0.0025), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(strokesPath(strokes.mid(start, end - start), shown));
        start = end;
    }
    painter->restore();
}

QColor ReviewFeedbackOverlay::colour(const QString &polarity, bool dark)
{
    return FeedbackIcon::colour(polarity, dark);
}

double ReviewFeedbackOverlay::fillOpacity(bool dark)
{
    return dark ? 0.28 : 0.22;
}

void ReviewFeedbackOverlay::paint(QPainter *painter, const std::function<QRectF(int, const QRectF &)> &map, bool dark)
{
    QList<Item> shown = items();
    if (!m_hidden.isEmpty()) shown.removeIf([this](const Item &item) { return item.record.value(QStringLiteral("feedback_id")).toString() == m_hidden; });
    paintItems(painter, shown, map, dark);
}

void ReviewFeedbackOverlay::paintItems(QPainter *painter, const QList<Item> &all, const std::function<QRectF(int, const QRectF &)> &map, bool dark,
                                       const QString &selectedTextBox, bool textBoxFrames)
{
    QList<Item> ordered;
    for (const Item &item : all) {
        if (!item.rects.isEmpty()) ordered.append(item);
    }
    // Oldest first: the most recently created highlight is drawn on top.
    std::stable_sort(ordered.begin(), ordered.end(), [](const Item &a, const Item &b) {
        return a.record.value(QStringLiteral("created_at")).toString() < b.record.value(QStringLiteral("created_at")).toString();
    });
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (const Item &item : std::as_const(ordered)) {
        if (DocumentFeedback::isDrawing(item.record)) {
            paintStrokes(painter, item.rects.constFirst().first, DocumentFeedback::strokesOf(item.record), map, dark);
            continue;
        }
        if (DocumentFeedback::isTextBox(item.record)) {
            const bool selected = !selectedTextBox.isEmpty() && item.record.value(QStringLiteral("feedback_id")).toString() == selectedTextBox;
            paintTextBox(painter, item.rects.constFirst().first, DocumentFeedback::textBoxRect(item.record), DocumentFeedback::textBoxFontSize(item.record),
                         item.record.value(QStringLiteral("comment")).toString(), map, dark, selected ? 2 : textBoxFrames ? 1 : 0);
            continue;
        }
        const QString polarity = item.record.value(QStringLiteral("polarity")).toString();
        const QColor accent = colour(polarity, dark);
        QColor fill = accent;
        fill.setAlphaF(fillOpacity(dark));
        QPen underline(accent, 2);
        // Solid, dashed, dotted: the three differ without colour.
        underline.setStyle(polarity == ReviewCardFeedback::negative() ? Qt::DashLine : polarity == ReviewCardFeedback::neutral() ? Qt::DotLine : Qt::SolidLine);
        underline.setCapStyle(Qt::FlatCap);
        QRectF lastLine;
        for (const auto &[page, rect] : item.rects) {
            const QRectF shown = map(page, rect);
            if (shown.isEmpty()) continue;
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRect(shown);
            painter->setPen(underline);
            painter->setBrush(Qt::NoBrush);
            painter->drawLine(QPointF(shown.left(), shown.bottom() - 1.0), QPointF(shown.right(), shown.bottom() - 1.0));
            lastLine = shown;
        }
        // A comment: a small dot at the end of the last line.
        if (!lastLine.isEmpty() && !item.record.value(QStringLiteral("comment")).toString().trimmed().isEmpty()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            painter->drawEllipse(QPointF(lastLine.right() + 5.0, lastLine.center().y()), 3.0, 3.0);
        }
    }
    painter->restore();
}

QString ReviewFeedbackOverlay::hitTest(const QPointF &point, const std::function<QRectF(int, const QRectF &)> &map)
{
    QList<Item> shown = items();
    if (!m_hidden.isEmpty()) shown.removeIf([this](const Item &item) { return item.record.value(QStringLiteral("feedback_id")).toString() == m_hidden; });
    return hitTestItems(shown, point, map);
}

QString ReviewFeedbackOverlay::hitTestItems(const QList<Item> &all, const QPointF &point, const std::function<QRectF(int, const QRectF &)> &map)
{
    QString best;
    QString bestCreated;
    for (const Item &item : all) {
        const QString created = item.record.value(QStringLiteral("created_at")).toString();
        if (DocumentFeedback::isDrawing(item.record)) {
            // On the lines, not in the empty space between them.
            const QRectF page = item.rects.isEmpty() ? QRectF() : map(item.rects.constFirst().first, QRectF(0.0, 0.0, 1.0, 1.0));
            if (page.isEmpty()) continue;
            QPainterPathStroker near;
            near.setWidth(10.0);
            near.setCapStyle(Qt::RoundCap);
            if (near.createStroke(strokesPath(DocumentFeedback::strokesOf(item.record), page)).contains(point) && (best.isEmpty() || created >= bestCreated)) {
                best = item.record.value(QStringLiteral("feedback_id")).toString();
                bestCreated = created;
            }
            continue;
        }
        if (DocumentFeedback::isTextBox(item.record)) {
            // Anywhere in its box, and a little around it.
            const QRectF shown = item.rects.isEmpty() ? QRectF() : map(item.rects.constFirst().first, DocumentFeedback::textBoxRect(item.record));
            if (!shown.isEmpty() && shown.adjusted(-4.0, -4.0, 4.0, 4.0).contains(point) && (best.isEmpty() || created >= bestCreated)) {
                best = item.record.value(QStringLiteral("feedback_id")).toString();
                bestCreated = created;
            }
            continue;
        }
        for (const auto &[page, rect] : item.rects) {
            const QRectF shown = map(page, rect).adjusted(-1.5, -1.5, 8.0, 1.5);
            if (!shown.contains(point)) continue;
            // Overlaps open the newest one, which is drawn on top.
            if (best.isEmpty() || created >= bestCreated) {
                best = item.record.value(QStringLiteral("feedback_id")).toString();
                bestCreated = created;
            }
            break;
        }
    }
    return best;
}

QList<QPair<int, QRectF>> ReviewFeedbackOverlay::rectsOf(const QString &feedbackId)
{
    for (const Item &item : items()) {
        if (item.record.value(QStringLiteral("feedback_id")).toString() == feedbackId) return item.rects;
    }
    return {};
}

QString ReviewFeedbackOverlay::summary(const QList<Item> &items)
{
    int liked = 0, disliked = 0, orphaned = 0;
    for (const Item &item : items) {
        (item.record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative() ? disliked : liked) += 1;
        if (item.state == QLatin1String("orphaned")) ++orphaned;
    }
    if (liked + disliked == 0) return QString();
    QStringList parts;
    if (liked) parts.append(i18n("%1 liked", liked));
    if (disliked) parts.append(i18n("%1 disliked", disliked));
    QString text = parts.join(QStringLiteral(" · "));
    if (orphaned) text += QStringLiteral(" ") + i18n("(%1 not found)", orphaned);
    return text;
}
}
