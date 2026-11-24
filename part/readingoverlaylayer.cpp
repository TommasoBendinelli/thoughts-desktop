// SPDX-License-Identifier: GPL-2.0-or-later
#include "readingoverlaylayer.h"

#include "documentidentity.h"
#include "projectfeedback.h"
#include "readingoverlays.h"
#include "thoughtsplatform.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QPainter>

#include <algorithm>

namespace Thoughts
{
namespace
{
// Overlays hidden with their eye, on this computer (§4). Only ids are kept, the most recent last.
constexpr int maximumHidden = 1000;

KConfigGroup hiddenGroup()
{
    return KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("Reading Overlays"));
}

bool before(const ReadingOverlayLayer::Mark &a, const ReadingOverlayLayer::Mark &b)
{
    const auto &[pageA, rectA] = a.rects.constFirst();
    const auto &[pageB, rectB] = b.rects.constFirst();
    if (pageA != pageB) return pageA < pageB;
    if (!qFuzzyCompare(rectA.top() + 1.0, rectB.top() + 1.0)) return rectA.top() < rectB.top();
    return rectA.left() < rectB.left();
}
}

ReadingOverlayLayer::ReadingOverlayLayer(QObject *parent)
    : QObject(parent)
{
    const QStringList hidden = hiddenGroup().readEntry("Hidden", QStringList());
    m_hidden = QSet<QString>(hidden.cbegin(), hidden.cend());
}

void ReadingOverlayLayer::setDocument(const QString &documentId, const QString &pdfSha256, int pageCount)
{
    const bool valid = DocumentIdentityStore::validId(documentId.toLower()) && pdfSha256.size() == 64;
    const QString id = valid ? documentId.toLower() : QString();
    const QString sha = valid ? pdfSha256.toLower() : QString();
    if (id == m_documentId && sha == m_pdfSha256 && pageCount == m_pageCount) return;
    if (id != m_documentId) m_sources.clear();
    m_documentId = id;
    m_pdfSha256 = sha;
    m_pageCount = pageCount;
    m_placements.clear();
    m_marks.clear();
    Q_EMIT changed();
}

bool ReadingOverlayLayer::hasDocument() const
{
    return !m_documentId.isEmpty() && !m_pdfSha256.isEmpty();
}

QString ReadingOverlayLayer::documentId() const
{
    return m_documentId;
}

QString ReadingOverlayLayer::pdfSha256() const
{
    return m_pdfSha256;
}

void ReadingOverlayLayer::setPageText(PageText pageText)
{
    m_pageText = std::move(pageText);
}

void ReadingOverlayLayer::setRecords(const QList<QJsonObject> &records)
{
    QList<Source> sources;
    for (const QJsonObject &record : ReadingOverlays::recordsOfDocument(records, m_documentId, true)) sources.append({record, 0, QString()});
    setSources(sources);
}

void ReadingOverlayLayer::setSources(const QList<Source> &sources)
{
    const bool same = sources.size() == m_sources.size() && std::equal(sources.cbegin(), sources.cend(), m_sources.cbegin(), [](const Source &a, const Source &b) {
        return a.record == b.record && a.pageOffset == b.pageOffset && a.documentPdfSha256 == b.documentPdfSha256;
    });
    if (same) return;
    m_sources = sources;
    m_marks.clear();
    // Each mark keeps its placement while its anchor stays (placementOf compares it); the marks of
    // overlays that are gone are forgotten.
    QSet<QString> ids;
    for (const Source &source : std::as_const(m_sources)) ids.insert(ReadingOverlays::overlayIdOf(source.record));
    for (auto it = m_placements.begin(); it != m_placements.end();) {
        if (ids.contains(it.key())) ++it;
        else it = m_placements.erase(it);
    }
    Q_EMIT changed();
}

void ReadingOverlayLayer::resetPages()
{
    if (m_placements.isEmpty()) return;
    m_placements.clear();
    m_marks.clear();
    Q_EMIT changed();
}

void ReadingOverlayLayer::clear()
{
    if (m_documentId.isEmpty() && m_sources.isEmpty() && m_placements.isEmpty()) return;
    m_documentId.clear();
    m_pdfSha256.clear();
    m_pageCount = 0;
    m_sources.clear();
    m_placements.clear();
    m_marks.clear();
    Q_EMIT changed();
}

void ReadingOverlayLayer::setWriter(Writer writer)
{
    m_writer = std::move(writer);
}

QList<QJsonObject> ReadingOverlayLayer::overlays() const
{
    QList<QJsonObject> result;
    for (const Source &source : m_sources) {
        if (!ReadingOverlays::isDeleted(source.record)) result.append(source.record);
    }
    return result;
}

const ReadingOverlayLayer::Source *ReadingOverlayLayer::source(const QString &overlayId) const
{
    for (const Source &source : m_sources) {
        if (ReadingOverlays::overlayIdOf(source.record) == overlayId) return &source;
    }
    return nullptr;
}

QJsonObject ReadingOverlayLayer::record(const QString &overlayId) const
{
    const Source *found = source(overlayId);
    return found ? found->record : QJsonObject();
}

bool ReadingOverlayLayer::isShown(const QString &overlayId) const
{
    return !m_hidden.contains(overlayId);
}

void ReadingOverlayLayer::setShown(const QString &overlayId, bool shown)
{
    if (overlayId.isEmpty() || isShown(overlayId) == shown) return;
    if (shown) m_hidden.remove(overlayId);
    else m_hidden.insert(overlayId);
    saveHidden();
    Q_EMIT changed();
}

void ReadingOverlayLayer::saveHidden() const
{
    KConfigGroup group = hiddenGroup();
    QStringList kept = group.readEntry("Hidden", QStringList());
    kept.removeIf([this](const QString &id) { return !m_hidden.contains(id); });
    for (const QString &id : m_hidden) {
        if (!kept.contains(id)) kept.append(id);
    }
    while (kept.size() > maximumHidden) kept.removeFirst();
    group.writeEntry("Hidden", kept);
    group.sync();
}

int ReadingOverlayLayer::shownCount() const
{
    const QList<QJsonObject> shown = overlays();
    return int(std::count_if(shown.cbegin(), shown.cend(), [this](const QJsonObject &record) { return isShown(ReadingOverlays::overlayIdOf(record)); }));
}

DocumentFeedback::Placement ReadingOverlayLayer::placementOf(const Source &source, const QJsonObject &mark)
{
    if (!hasDocument()) return {QStringLiteral("orphaned"), {}};
    QJsonObject passage = ReadingOverlays::passageOf(mark);
    // A document of a Project read as one PDF: its pages where it starts there, its PDF the reading's.
    if (source.pageOffset != 0 || !source.documentPdfSha256.isEmpty())
        passage = ProjectFeedback::moved(passage, source.pageOffset, m_documentId, source.documentPdfSha256, m_pdfSha256);
    const QJsonObject anchor = passage.value(QStringLiteral("anchor")).toObject();
    Placed &placed = m_placements[ReadingOverlays::overlayIdOf(source.record)][ReadingOverlays::markIdOf(mark)];
    if (placed.placement.state.isEmpty() || placed.anchor != anchor) {
        // As document feedback (document-feedback.md §4); never written back.
        placed.anchor = anchor;
        placed.placement = DocumentFeedback::place(passage, m_pdfSha256, m_pageCount, m_pageText);
    }
    return placed.placement;
}

QList<ReadingOverlayLayer::Mark> ReadingOverlayLayer::marks(const QString &overlayId)
{
    const auto cached = m_marks.constFind(overlayId);
    if (cached != m_marks.cend()) return *cached;
    QList<Mark> placed;
    QList<Mark> missing;
    const Source *found = source(overlayId);
    if (!found || ReadingOverlays::isDeleted(found->record)) return {};
    const Source current = *found;
    for (const QJsonObject &mark : ReadingOverlays::marksOf(current.record)) {
        const DocumentFeedback::Placement placement = placementOf(current, mark);
        Mark item{overlayId, mark, placement.state, placement.rects};
        (item.rects.isEmpty() ? missing : placed).append(item);
    }
    std::stable_sort(placed.begin(), placed.end(), before);
    const QList<Mark> result = placed + missing;
    m_marks.insert(overlayId, result);
    return result;
}

ReadingOverlayLayer::Mark ReadingOverlayLayer::mark(const QString &overlayId, const QString &markId)
{
    for (const Mark &item : marks(overlayId)) {
        if (ReadingOverlays::markIdOf(item.mark) == markId) return item;
    }
    return {};
}

QList<ReadingOverlayLayer::Mark> ReadingOverlayLayer::shownMarks(const QSet<int> &pages)
{
    QList<Mark> result;
    if (!hasDocument()) return result;
    // Oldest overlay first, so the newest is painted on top.
    for (auto it = m_sources.crbegin(); it != m_sources.crend(); ++it) {
        const QString id = ReadingOverlays::overlayIdOf(it->record);
        if (ReadingOverlays::isDeleted(it->record) || !isShown(id)) continue;
        for (const Mark &item : marks(id)) {
            if (std::any_of(item.rects.cbegin(), item.rects.cend(), [&pages](const QPair<int, QRectF> &rect) { return pages.contains(rect.first); }))
                result.append(item);
        }
    }
    return result;
}

QList<QPair<ReadingOverlayLayer::Mark, QRectF>> ReadingOverlayLayer::markers(const QSet<int> &pages, const Map &map)
{
    // One marker per mark with a comment, in the right margin of the page of its first region,
    // level with it; markers that would overlap go down the margin one under the other.
    QHash<int, QList<QPair<Mark, QRectF>>> byPage;
    for (const Mark &item : shownMarks(pages)) {
        if (ReadingOverlays::commentOf(item.mark).isEmpty()) continue;
        const auto &[page, rect] = item.rects.constFirst();
        if (!pages.contains(page)) continue;
        const QRectF shownPage = map(page, QRectF(0.0, 0.0, 1.0, 1.0));
        const QRectF line = map(page, rect);
        if (shownPage.isEmpty() || line.isEmpty()) continue;
        const double inset = std::max(markerSize * 0.75, shownPage.width() * 0.045);
        const QPointF centre(shownPage.right() - inset, line.center().y());
        byPage[page].append({item, QRectF(centre.x() - markerSize / 2.0, centre.y() - markerSize / 2.0, markerSize, markerSize)});
    }
    QList<QPair<Mark, QRectF>> result;
    for (auto it = byPage.begin(); it != byPage.end(); ++it) {
        auto &placed = it.value();
        std::stable_sort(placed.begin(), placed.end(), [](const auto &a, const auto &b) { return a.second.top() < b.second.top(); });
        for (int index = 1; index < placed.size(); ++index) {
            const QRectF previous = placed.at(index - 1).second;
            QRectF &current = placed[index].second;
            if (current.top() < previous.bottom() + 2.0) current.moveTop(previous.bottom() + 2.0);
        }
        result += placed;
    }
    return result;
}

QColor ReadingOverlayLayer::washColour(const QString &kind)
{
    QColor colour;
    if (kind == ReadingOverlays::read()) {
        colour = QColor(52, 168, 83);
        colour.setAlphaF(0.20);
    } else if (kind == ReadingOverlays::skim()) {
        colour = QColor(251, 188, 5);
        colour.setAlphaF(0.22);
    } else if (kind == ReadingOverlays::skip()) {
        colour = QColor(110, 110, 110);
        colour.setAlphaF(0.22);
    }
    return colour;
}

QColor ReadingOverlayLayer::noteColour()
{
    return QColor(66, 133, 244);
}

QColor ReadingOverlayLayer::kindColour(const QString &kind)
{
    if (kind == ReadingOverlays::read()) return QColor(52, 168, 83);
    if (kind == ReadingOverlays::skim()) return QColor(251, 188, 5);
    if (kind == ReadingOverlays::skip()) return QColor(110, 110, 110);
    return noteColour();
}

void ReadingOverlayLayer::paint(QPainter *painter, const QSet<int> &pages, const Map &map)
{
    for (const int page : pages) paintWashes(painter, page, [&map, page](const QRectF &rect) { return map(page, rect); });
    paintMarkers(painter, pages, map);
}

void ReadingOverlayLayer::paintWashes(QPainter *painter, int page, const std::function<QRectF(const QRectF &)> &map)
{
    if (page < 1 || !hasDocument()) return;
    const QList<Mark> shown = shownMarks({page});
    if (shown.isEmpty()) return;
    painter->save();
    for (const Mark &item : shown) {
        const QColor wash = washColour(ReadingOverlays::kindOf(item.mark));
        QPen underline(noteColour(), 2.0);
        underline.setCapStyle(Qt::FlatCap);
        for (const auto &[markPage, rect] : item.rects) {
            if (markPage != page) continue;
            const QRectF shownRect = map(rect);
            if (shownRect.isEmpty()) continue;
            if (wash.isValid()) {
                painter->fillRect(shownRect, wash);
            } else {
                // A note gives no advice: no wash, only a line under its text.
                painter->setPen(underline);
                painter->drawLine(QPointF(shownRect.left(), shownRect.bottom()), QPointF(shownRect.right(), shownRect.bottom()));
            }
        }
    }
    painter->restore();
}

void ReadingOverlayLayer::paintMarkers(QPainter *painter, const QSet<int> &pages, const Map &map)
{
    if (pages.isEmpty() || !hasDocument()) return;
    const QList<QPair<Mark, QRectF>> placed = markers(pages, map);
    if (placed.isEmpty()) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (const auto &[item, rect] : placed) {
        const QColor accent = kindColour(ReadingOverlays::kindOf(item.mark));
        // A small speech bubble: a rounded body with a tail at its lower left.
        QPainterPath bubble;
        const QRectF body = rect.adjusted(0.5, 0.5, -0.5, -3.0);
        bubble.addRoundedRect(body, 3.0, 3.0);
        QPolygonF tail;
        tail << QPointF(body.left() + 3.0, body.bottom() - 0.5) << QPointF(body.left() + 3.0, rect.bottom() - 0.5) << QPointF(body.left() + 7.0, body.bottom() - 0.5);
        bubble.addPolygon(tail);
        bubble.setFillRule(Qt::WindingFill);
        painter->setPen(QPen(accent.darker(135), 1.0));
        painter->setBrush(accent);
        painter->drawPath(bubble.simplified());
        // Two lines of "text" in it.
        painter->setPen(QPen(QColor(255, 255, 255, 230), 1.2, Qt::SolidLine, Qt::RoundCap));
        const double y = body.center().y();
        painter->drawLine(QPointF(body.left() + 3.0, y - 1.8), QPointF(body.right() - 3.0, y - 1.8));
        painter->drawLine(QPointF(body.left() + 3.0, y + 1.8), QPointF(body.right() - 5.0, y + 1.8));
    }
    painter->restore();
}

ReadingOverlayLayer::Hit ReadingOverlayLayer::hitTest(int page, const QPointF &point, const Map &map)
{
    if (page < 1 || !hasDocument()) return {};
    // The comment markers first: they are drawn on top.
    const QList<QPair<Mark, QRectF>> placed = markers({page}, map);
    for (auto it = placed.crbegin(); it != placed.crend(); ++it) {
        if (it->second.adjusted(-2.0, -2.0, 2.0, 2.0).contains(point)) return {it->first.overlayId, ReadingOverlays::markIdOf(it->first.mark), true};
    }
    // Then the washes, the newest overlay on top.
    const QList<Mark> shown = shownMarks({page});
    for (auto it = shown.crbegin(); it != shown.crend(); ++it) {
        for (const auto &[markPage, rect] : it->rects) {
            if (markPage != page) continue;
            const QRectF shownRect = map(markPage, rect);
            if (!shownRect.isEmpty() && shownRect.adjusted(0.0, 0.0, 0.0, 2.0).contains(point)) return {it->overlayId, ReadingOverlays::markIdOf(it->mark), false};
        }
    }
    return {};
}

QRectF ReadingOverlayLayer::markerRect(const QString &overlayId, const QString &markId, const Map &map)
{
    const Mark found = mark(overlayId, markId);
    if (found.rects.isEmpty()) return {};
    for (const auto &[item, rect] : markers({found.rects.constFirst().first}, map)) {
        if (item.overlayId == overlayId && ReadingOverlays::markIdOf(item.mark) == markId) return rect;
    }
    return {};
}

bool ReadingOverlayLayer::write(const QJsonObject &record, QString *error)
{
    if (!m_writer) {
        if (error) *error = i18n("Reading overlays cannot be changed here.");
        return false;
    }
    if (!m_writer(record, error)) return false;
    // Shown at once; the projections bring the same record back.
    QList<Source> sources = m_sources;
    for (Source &existing : sources) {
        if (ReadingOverlays::overlayIdOf(existing.record) == ReadingOverlays::overlayIdOf(record)) existing.record = ReadingOverlays::merge(existing.record, record);
    }
    setSources(sources);
    return true;
}

bool ReadingOverlayLayer::removeMark(const QString &overlayId, const QString &markId, QString *error)
{
    const QJsonObject next = ReadingOverlays::withoutMark(record(overlayId), markId, Platform::reviewOriginId());
    if (next.isEmpty()) {
        if (error) *error = i18n("This mark is no longer in the overlay.");
        return false;
    }
    return write(next, error);
}

bool ReadingOverlayLayer::removeOverlay(const QString &overlayId, QString *error)
{
    const QJsonObject current = record(overlayId);
    if (current.isEmpty() || ReadingOverlays::isDeleted(current)) {
        if (error) *error = i18n("This overlay was already deleted.");
        return false;
    }
    return write(ReadingOverlays::remove(current, Platform::reviewOriginId()), error);
}
}
