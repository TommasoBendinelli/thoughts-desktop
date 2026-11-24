// SPDX-License-Identifier: GPL-2.0-or-later
#include "documentfeedbackoverlay.h"

#include "documentidentity.h"
#include "feedbackversion.h"
#include "thoughtsplatform.h"

#include "core/area.h"
#include "core/document.h"
#include "core/page.h"
#include "core/textpage.h"

#include <KLocalizedString>

#include <QJsonArray>
#include <QPainter>

#include <algorithm>
#include <cstdlib>

namespace Thoughts
{
DocumentFeedbackOverlay::DocumentFeedbackOverlay(Okular::Document *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
{
}

void DocumentFeedbackOverlay::setDocument(const QString &documentId, const QString &pdfSha256)
{
    // Only a known Library document and PDF version get feedback (spec §5.1).
    const bool valid = DocumentIdentityStore::validId(documentId.toLower()) && pdfSha256.size() == 64;
    const QString id = valid ? documentId.toLower() : QString();
    const QString sha = valid ? pdfSha256.toLower() : QString();
    if (id == m_documentId && sha == m_pdfSha256) return;
    if (id != m_documentId) m_records.clear();
    m_showEarlier = false;
    m_documentId = id;
    m_pdfSha256 = sha;
    m_placements.clear();
    m_pageGlyphs.clear();
    Q_EMIT changed();
}

void DocumentFeedbackOverlay::setReviewVersion(const QJsonObject &version)
{
    m_reviewVersion = version;
}

QJsonObject DocumentFeedbackOverlay::reviewVersion() const
{
    return m_reviewVersion;
}

void DocumentFeedbackOverlay::setRecords(const QList<QJsonObject> &records)
{
    const QList<QJsonObject> own = DocumentFeedback::recordsOfDocument(records, m_documentId, true);
    if (own == m_records) return;
    // The anchor never changes, so a record keeps its placement; a new one is placed when needed,
    // and so is a drawing or a text box, whose place is its strokes or its rect.
    QSet<QString> ids;
    for (const QJsonObject &record : own) {
        if (!DocumentFeedback::isPageMark(record)) ids.insert(record.value(QStringLiteral("feedback_id")).toString());
    }
    for (auto it = m_placements.begin(); it != m_placements.end();) {
        if (ids.contains(it.key())) ++it;
        else it = m_placements.erase(it);
    }
    m_records = own;
    Q_EMIT changed();
}

void DocumentFeedbackOverlay::setAttachmentVersions(std::function<QString(const QString &path)> version)
{
    m_attachmentVersions = std::move(version);
}

void DocumentFeedbackOverlay::setShowEarlier(bool show)
{
    if (m_showEarlier == show) return;
    m_showEarlier = show;
    Q_EMIT changed();
}

bool DocumentFeedbackOverlay::showsEarlier() const
{
    return m_showEarlier;
}

bool DocumentFeedbackOverlay::isCurrent(const QJsonObject &record) const
{
    const QString page = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString();
    return DocumentFeedback::shownOn(record, m_pdfSha256, !page.isEmpty() && m_attachmentVersions ? m_attachmentVersions(page) : QString());
}

QList<QJsonObject> DocumentFeedbackOverlay::shownRecords() const
{
    QList<QJsonObject> result;
    for (const QJsonObject &record : m_records) {
        if (!record.value(QStringLiteral("deleted")).toBool() && (m_showEarlier || isCurrent(record))) result.append(record);
    }
    return result;
}

int DocumentFeedbackOverlay::earlierCount() const
{
    return std::count_if(m_records.cbegin(), m_records.cend(), [this](const QJsonObject &record) { return !record.value(QStringLiteral("deleted")).toBool() && !isCurrent(record); });
}

void DocumentFeedbackOverlay::resetPages()
{
    if (m_placements.isEmpty() && m_pageGlyphs.isEmpty()) return;
    m_placements.clear();
    m_pageGlyphs.clear();
    Q_EMIT changed();
}

void DocumentFeedbackOverlay::clear()
{
    if (m_documentId.isEmpty() && m_records.isEmpty() && m_placements.isEmpty()) return;
    m_documentId.clear();
    m_pdfSha256.clear();
    m_reviewVersion = {};
    m_showEarlier = false;
    m_records.clear();
    m_hidden.clear();
    m_selectedTextBox.clear();
    m_textBoxInHand.clear();
    m_placements.clear();
    m_pageGlyphs.clear();
    Q_EMIT changed();
}

bool DocumentFeedbackOverlay::hasDocument() const
{
    return !m_documentId.isEmpty() && !m_pdfSha256.isEmpty();
}

QString DocumentFeedbackOverlay::documentId() const
{
    return m_documentId;
}

QString DocumentFeedbackOverlay::pdfSha256() const
{
    return m_pdfSha256;
}

void DocumentFeedbackOverlay::setWriter(Writer writer)
{
    m_writer = std::move(writer);
}

QList<QJsonObject> DocumentFeedbackOverlay::records() const
{
    return m_records;
}

QJsonObject DocumentFeedbackOverlay::record(const QString &feedbackId) const
{
    for (const QJsonObject &record : m_records) {
        if (record.value(QStringLiteral("feedback_id")).toString() == feedbackId) return record;
    }
    return {};
}

int DocumentFeedbackOverlay::pageCount() const
{
    return m_document ? int(m_document->pages()) : 0;
}

QList<ReviewCardFeedback::Glyph> DocumentFeedbackOverlay::pageGlyphs(int page) const
{
    const auto cached = m_pageGlyphs.constFind(page);
    if (cached != m_pageGlyphs.cend()) return *cached;
    QList<ReviewCardFeedback::Glyph> result;
    const Okular::Page *pdfPage = m_document && page >= 1 && page <= pageCount() ? m_document->page(page - 1) : nullptr;
    if (pdfPage) {
        if (!pdfPage->hasTextPage()) m_document->requestTextPage(page - 1);
        Okular::RegularAreaRect area;
        area.append(Okular::NormalizedRect(0.0, 0.0, 1.0, 1.0));
        // Every character of the page, in the text layer's reading order (spec §4).
        for (const Okular::TextEntity &entity : pdfPage->words(&area, Okular::TextPage::CentralPixelTextAreaInclusionBehaviour)) {
            const Okular::NormalizedRect box = entity.area();
            result.append({page, entity.text(), QRectF(QPointF(box.left, box.top), QPointF(box.right, box.bottom))});
        }
    }
    m_pageGlyphs.insert(page, result);
    return result;
}

ReviewCardFeedback::AnswerText DocumentFeedbackOverlay::pagesText(const QList<int> &pages) const
{
    QList<QList<ReviewCardFeedback::Glyph>> regions;
    for (const int page : pages) regions.append(pageGlyphs(page));
    return ReviewCardFeedback::answerText(regions);
}

DocumentFeedback::Placement DocumentFeedbackOverlay::placementOf(const QJsonObject &record)
{
    const QString id = record.value(QStringLiteral("feedback_id")).toString();
    const auto cached = m_placements.constFind(id);
    if (cached != m_placements.cend()) return *cached;
    const DocumentFeedback::Placement placement = DocumentFeedback::place(record, m_pdfSha256, pageCount(), [this](const QList<int> &pages) { return pagesText(pages); });
    m_placements.insert(id, placement);
    return placement;
}

QList<DocumentFeedbackOverlay::Item> DocumentFeedbackOverlay::items(const QSet<int> &pages)
{
    QList<Item> result;
    if (!hasDocument()) return result;
    for (const QJsonObject &record : std::as_const(m_records)) {
        if (record.value(QStringLiteral("deleted")).toBool() || (!m_showEarlier && !isCurrent(record))) continue;
        // Made in an HTML attachment: in its page (a drawing on its snapshot), never on the PDF.
        if (DocumentFeedback::isAttachmentFeedback(record)) continue;
        if (!pages.isEmpty()) {
            // A passage is drawn on its own pages, or found within pageWindow of them.
            const QString id = record.value(QStringLiteral("feedback_id")).toString();
            const auto placed = m_placements.constFind(id);
            const auto rects = placed != m_placements.cend() ? placed->rects : ReviewCardFeedback::regionsOf(record);
            const int reach = placed != m_placements.cend() ? 0 : DocumentFeedback::pageWindow;
            const bool near = std::any_of(rects.cbegin(), rects.cend(), [&](const QPair<int, QRectF> &rect) {
                return std::any_of(pages.cbegin(), pages.cend(), [&](int page) { return std::abs(page - rect.first) <= reach; });
            });
            if (!near) continue;
        }
        const DocumentFeedback::Placement placement = placementOf(record);
        if (placement.rects.isEmpty()) continue; // Orphaned: kept, never drawn.
        Item item;
        item.record = record;
        item.state = placement.state;
        item.rects = placement.rects;
        result.append(item);
    }
    return result;
}

QVariantMap DocumentFeedbackOverlay::states()
{
    QVariantMap result;
    if (!hasDocument()) return result;
    for (const QJsonObject &record : std::as_const(m_records)) {
        if (record.value(QStringLiteral("deleted")).toBool()) continue;
        result.insert(record.value(QStringLiteral("feedback_id")).toString(), placementOf(record).state);
    }
    return result;
}

QList<QPair<int, QRectF>> DocumentFeedbackOverlay::rectsOf(const QString &feedbackId)
{
    const QJsonObject found = record(feedbackId);
    if (found.isEmpty() || found.value(QStringLiteral("deleted")).toBool() || !hasDocument()) return {};
    return placementOf(found).rects;
}

QJsonObject DocumentFeedbackOverlay::newRecord(const QString &polarity, const QString &comment, const QList<QPair<int, QRectF>> &lines, const QString &text,
                                               const std::function<bool(int, const QPointF &)> &selected, QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QJsonObject();
    };
    if (!hasDocument()) return fail(i18n("Feedback needs a Library PDF."));
    if (lines.isEmpty()) return fail(i18n("Select a passage."));
    if (lines.size() > ReviewCardFeedback::maximumRegions) return fail(i18n("Select a shorter passage."));
    const QString exact = text.trimmed();
    if (exact.isEmpty()) return fail(i18n("The selection has no text."));
    // Context: the text of the selected pages right before and after the selected characters.
    QList<int> pages;
    for (const auto &line : lines) {
        if (!pages.contains(line.first)) pages.append(line.first);
    }
    std::sort(pages.begin(), pages.end());
    const auto [prefix, suffix] = DocumentFeedback::context(pagesText(pages), selected);
    // At most maximumQuote characters (code points), as the server counts them.
    QString quote = exact;
    const auto codePoints = quote.toUcs4();
    if (codePoints.size() > ReviewCardFeedback::maximumQuote) quote = QString::fromUcs4(reinterpret_cast<const char32_t *>(codePoints.constData()), ReviewCardFeedback::maximumQuote).trimmed();
    const QJsonObject anchor = ReviewCardFeedback::anchor(quote, prefix, suffix, m_documentId, m_pdfSha256, ReviewCardFeedback::regionsJson(lines));
    const QJsonObject record = FeedbackVersion::stamped(DocumentFeedback::create(polarity, comment, anchor, Platform::reviewOriginId()), m_reviewVersion);
    const QString problem = DocumentFeedback::validationError(record);
    if (!problem.isEmpty()) return fail(problem);
    return record;
}

bool DocumentFeedbackOverlay::writeAll(const QList<QJsonObject> &written, QString *error)
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
                existing = DocumentFeedback::merge(existing, record);
                found = true;
            }
        }
        if (!found) records.append(record);
    }
    setRecords(records);
    return true;
}

void DocumentFeedbackOverlay::setBatchWriter(BatchWriter writer)
{
    m_batchWriter = std::move(writer);
}

void DocumentFeedbackOverlay::setHiddenFeedback(const QString &feedbackId)
{
    if (m_hidden == feedbackId) return;
    m_hidden = feedbackId;
    Q_EMIT changed();
}

QString DocumentFeedbackOverlay::hiddenFeedback() const
{
    return m_hidden;
}

bool DocumentFeedbackOverlay::write(const QJsonObject &record, QString *error)
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
            existing = DocumentFeedback::merge(existing, record);
            found = true;
        }
    }
    if (!found) records.append(record);
    setRecords(records);
    return true;
}

QList<DocumentFeedbackOverlay::Item> DocumentFeedbackOverlay::shownItems(const QSet<int> &pages, bool pageMarksOnly)
{
    QList<Item> result = items(pages);
    if (pageMarksOnly) result.removeIf([](const Item &item) { return !DocumentFeedback::isPageMark(item.record); });
    result.removeIf([this](const Item &item) {
        if (DocumentFeedback::isPageMark(item.record)) return false;
        return DocumentFeedback::kindOf(item.record) == DocumentFeedback::other() ? !m_showComments : !m_showFeedback;
    });
    if (!m_hidden.isEmpty()) result.removeIf([this](const Item &item) { return item.record.value(QStringLiteral("feedback_id")).toString() == m_hidden; });
    return result;
}

void DocumentFeedbackOverlay::paint(QPainter *painter, const QSet<int> &pages, const std::function<QRectF(int, const QRectF &)> &map, bool dark, bool pageMarksOnly,
                                    bool textBoxFrames)
{
    if (pages.isEmpty()) return;
    QList<Item> shown = shownItems(pages, pageMarksOnly);
    if (!m_textBoxInHand.isEmpty()) shown.removeIf([this](const Item &item) { return item.record.value(QStringLiteral("feedback_id")).toString() == m_textBoxInHand; });
    ReviewFeedbackOverlay::paintItems(painter, shown, map, dark, textBoxFrames ? m_selectedTextBox : QString(), textBoxFrames);
}

QString DocumentFeedbackOverlay::hitTest(int page, const QPointF &point, const std::function<QRectF(int, const QRectF &)> &map, bool pageMarksOnly)
{
    if (page < 1) return {};
    return ReviewFeedbackOverlay::hitTestItems(shownItems({page}, pageMarksOnly), point, map);
}

QString DocumentFeedbackOverlay::textBoxAt(int page, const QPointF &point, const std::function<QRectF(int, const QRectF &)> &map)
{
    if (page < 1) return {};
    QList<Item> boxes = shownItems({page}, true);
    boxes.removeIf([](const Item &item) { return !DocumentFeedback::isTextBox(item.record); });
    return ReviewFeedbackOverlay::hitTestItems(boxes, point, map);
}

QHash<QString, QList<int>> DocumentFeedbackOverlay::strokesNear(int page, const QPointF &from, const QPointF &to, double radius,
                                                                const std::function<QRectF(int, const QRectF &)> &map)
{
    QHash<QString, QList<int>> result;
    if (page < 1) return result;
    const QRectF shown = map(page, QRectF(0.0, 0.0, 1.0, 1.0));
    for (const Item &item : shownItems({page}, true)) {
        if (!DocumentFeedback::isDrawing(item.record)) continue;
        const QList<DocumentFeedback::Stroke> strokes = DocumentFeedback::strokesOf(item.record);
        for (int index = 0; index < strokes.size(); ++index) {
            if (DocumentFeedback::strokeNear(strokes.at(index), shown, from, to, radius)) result[item.record.value(QStringLiteral("feedback_id")).toString()].append(index);
        }
    }
    return result;
}

void DocumentFeedbackOverlay::setShownKinds(bool comments, bool feedback)
{
    if (m_showComments == comments && m_showFeedback == feedback) return;
    m_showComments = comments;
    m_showFeedback = feedback;
    Q_EMIT changed();
}

void DocumentFeedbackOverlay::setSelectedTextBox(const QString &feedbackId)
{
    m_selectedTextBox = feedbackId;
}

void DocumentFeedbackOverlay::setTextBoxInHand(const QString &feedbackId)
{
    m_textBoxInHand = feedbackId;
}

QString DocumentFeedbackOverlay::selectedTextBox() const
{
    return m_selectedTextBox;
}
}
