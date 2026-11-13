// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnotedrawing.h"

#include <QFont>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <limits>

namespace Thoughts::AgentNotes
{

namespace
{
QString oneDecimal(qreal value)
{
    // As the phone writes them: "12.0", "-3.5".
    return QString::number(std::round(value * 10) / 10, 'f', 1);
}

QString colorHex(const QColor &color)
{
    return QStringLiteral("#%1").arg(color.rgb() & 0xFFFFFF, 6, 16, QLatin1Char('0')).toUpper();
}

QString opacityText(qreal opacity)
{
    QString text = QString::number(opacity, 'f', 2);
    while (text.endsWith(QLatin1Char('0')) && !text.endsWith(QLatin1String(".0"))) text.chop(1);
    return text;
}

QString strokeJson(const Stroke &stroke)
{
    QString json = QStringLiteral("{\"tool\":\"%1\",\"color\":\"%2\",\"opacity\":%3,\"width\":%4,\"points\":[")
                       .arg(stroke.tool == Stroke::Tool::Highlighter ? QStringLiteral("highlighter") : QStringLiteral("pen"), colorHex(stroke.color),
                            opacityText(stroke.opacity), oneDecimal(stroke.width));
    for (int index = 0; index < stroke.points.size(); ++index) {
        if (index > 0) json += QLatin1Char(',');
        json += QLatin1Char('[') + oneDecimal(stroke.points.at(index).x()) + QLatin1Char(',') + oneDecimal(stroke.points.at(index).y()) + QLatin1Char(']');
    }
    return json + QStringLiteral("]}");
}

QString markerJson(const Marker &marker)
{
    return QStringLiteral("{\"x\":%1,\"y\":%2,\"number\":%3,\"color\":\"%4\"}")
        .arg(oneDecimal(marker.centre.x()), oneDecimal(marker.centre.y()))
        .arg(marker.number)
        .arg(colorHex(marker.color));
}

qreal segmentDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal length = QPointF::dotProduct(ab, ab);
    qreal t = length > 0 ? QPointF::dotProduct(point - a, ab) / length : 0;
    t = qBound(0.0, t, 1.0);
    const QPointF nearest = a + t * ab;
    return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

bool segmentsCross(const QPointF &p, const QPointF &q, const QPointF &a, const QPointF &b)
{
    const qreal d1 = cross(q - p, a - p);
    const qreal d2 = cross(q - p, b - p);
    const qreal d3 = cross(b - a, p - a);
    const qreal d4 = cross(b - a, q - a);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// Closest approach of the stroke's centre line to the segment a-b (0 where they cross).
qreal strokeDistance(const Stroke &stroke, const QPointF &a, const QPointF &b)
{
    qreal best = std::numeric_limits<qreal>::max();
    const auto &points = stroke.points;
    if (points.size() == 1) return qMin(segmentDistance(points.first(), a, b), best);
    for (int index = 1; index < points.size(); ++index) {
        const QPointF p = points.at(index - 1);
        const QPointF q = points.at(index);
        if (segmentsCross(p, q, a, b)) return 0;
        best = qMin(best, qMin(qMin(segmentDistance(p, a, b), segmentDistance(q, a, b)), qMin(segmentDistance(a, p, q), segmentDistance(b, p, q))));
    }
    return best;
}
}

QRectF Stroke::bounds() const
{
    if (points.isEmpty()) return {};
    qreal left = points.first().x(), right = left, top = points.first().y(), bottom = top;
    for (const QPointF &point : points) {
        left = qMin(left, point.x());
        right = qMax(right, point.x());
        top = qMin(top, point.y());
        bottom = qMax(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

AnnotationDocument::AnnotationDocument(int imageWidth, int imageHeight)
    : m_width(imageWidth)
    , m_height(imageHeight)
{
}

qreal AnnotationDocument::penWidthFor(int imageWidth)
{
    return qMax(2.0, imageWidth / 250.0);
}

qreal AnnotationDocument::markerRadiusFor(int imageWidth)
{
    return qMax(14.0, imageWidth * 0.03);
}

int AnnotationDocument::jsonBytes(const Stroke &stroke)
{
    return int(strokeJson(stroke).size()) + 1;
}

int AnnotationDocument::jsonBytes(const Marker &marker)
{
    return int(markerJson(marker).size()) + 1;
}

Stroke AnnotationDocument::makeStroke(Stroke::Tool tool, const QVector<QPointF> &points, const QColor &color) const
{
    Stroke stroke;
    stroke.tool = tool;
    stroke.color = color.toRgb();
    stroke.color.setAlpha(255);
    stroke.width = tool == Stroke::Tool::Highlighter ? penWidth() * HighlighterWidthFactor : penWidth();
    stroke.opacity = tool == Stroke::Tool::Highlighter ? HighlighterOpacity : 1.0;
    stroke.points = points;
    return stroke;
}

bool AnnotationDocument::addStroke(Stroke::Tool tool, const QVector<QPointF> &points, const QColor &color)
{
    if (points.isEmpty() || isNull()) return false;
    const Stroke stroke = makeStroke(tool, points, color);
    if (m_strokes.size() + 1 > MaxStrokes || m_points + stroke.points.size() > MaxPoints || m_bytes + jsonBytes(stroke) > MaxJsonBytes) return false;
    const Action action = AddStroke{stroke};
    apply(action);
    m_undo.append(action);
    m_redo.clear();
    return true;
}

int AnnotationDocument::nextMarkerNumber() const
{
    int highest = 0;
    for (const Marker &marker : m_markers) highest = qMax(highest, marker.number);
    return highest + 1;
}

std::optional<Marker> AnnotationDocument::addMarker(const QPointF &centre, const QColor &color)
{
    if (isNull()) return std::nullopt;
    Marker marker{centre, nextMarkerNumber(), color.toRgb()};
    marker.color.setAlpha(255);
    if (m_markers.size() + 1 > MaxMarkers || m_bytes + jsonBytes(marker) > MaxJsonBytes) return std::nullopt;
    const Action action = AddMarker{marker};
    apply(action);
    m_undo.append(action);
    m_redo.clear();
    return marker;
}

int AnnotationDocument::erase(const QPointF &from, const QPointF &to, qreal radius)
{
    Removal removal;
    for (int index = 0; index < m_strokes.size(); ++index) {
        const Stroke &stroke = m_strokes.at(index);
        const qreal reach = radius + stroke.width / 2;
        if (!stroke.bounds().adjusted(-reach, -reach, reach, reach).intersects(QRectF(from, to).normalized().adjusted(-1, -1, 1, 1))) continue;
        if (strokeDistance(stroke, from, to) <= reach) removal.strokes.append({index, stroke});
    }
    const qreal markerReach = radius + markerRadius();
    for (int index = 0; index < m_markers.size(); ++index) {
        if (segmentDistance(m_markers.at(index).centre, from, to) <= markerReach) removal.markers.append({index, m_markers.at(index)});
    }
    const int count = int(removal.strokes.size() + removal.markers.size());
    if (count == 0) return 0;
    const Action action = removal;
    apply(action);
    m_undo.append(action);
    m_redo.clear();
    return count;
}

void AnnotationDocument::clear()
{
    if (isEmpty()) return;
    Removal removal;
    for (int index = 0; index < m_strokes.size(); ++index) removal.strokes.append({index, m_strokes.at(index)});
    for (int index = 0; index < m_markers.size(); ++index) removal.markers.append({index, m_markers.at(index)});
    const Action action = removal;
    apply(action);
    m_undo.append(action);
    m_redo.clear();
}

void AnnotationDocument::endGesture(int start)
{
    if (start < 0 || m_undo.size() - start < 2) return;
    Erasure erasure;
    for (int index = start; index < m_undo.size(); ++index) {
        const auto *removal = std::get_if<Removal>(&m_undo.at(index));
        if (!removal) return;  // only erasures are joined
        erasure.steps.append(*removal);
    }
    m_undo.erase(m_undo.begin() + start, m_undo.end());
    m_undo.append(erasure);
}

void AnnotationDocument::undo()
{
    if (m_undo.isEmpty()) return;
    const Action action = m_undo.takeLast();
    revert(action);
    m_redo.append(action);
}

void AnnotationDocument::redo()
{
    if (m_redo.isEmpty()) return;
    const Action action = m_redo.takeLast();
    apply(action);
    m_undo.append(action);
}

void AnnotationDocument::apply(const Action &action)
{
    if (const auto *add = std::get_if<AddStroke>(&action)) {
        insertStroke(int(m_strokes.size()), add->stroke);
    } else if (const auto *marker = std::get_if<AddMarker>(&action)) {
        insertMarker(int(m_markers.size()), marker->marker);
    } else if (const auto *removal = std::get_if<Removal>(&action)) {
        applyRemoval(*removal);
    } else if (const auto *erasure = std::get_if<Erasure>(&action)) {
        for (const Removal &step : erasure->steps) applyRemoval(step);
    }
    ++m_revision;
}

void AnnotationDocument::applyRemoval(const Removal &removal)
{
    // Highest index first, so the others keep theirs.
    for (auto it = removal.strokes.crbegin(); it != removal.strokes.crend(); ++it) removeStrokeAt(it->first);
    for (auto it = removal.markers.crbegin(); it != removal.markers.crend(); ++it) removeMarkerAt(it->first);
}

void AnnotationDocument::revertRemoval(const Removal &removal)
{
    for (const auto &[index, stroke] : removal.strokes) insertStroke(qMin(index, int(m_strokes.size())), stroke);
    for (const auto &[index, marker] : removal.markers) insertMarker(qMin(index, int(m_markers.size())), marker);
}

void AnnotationDocument::revert(const Action &action)
{
    if (std::holds_alternative<AddStroke>(action)) {
        removeStrokeAt(int(m_strokes.size()) - 1);
    } else if (std::holds_alternative<AddMarker>(action)) {
        removeMarkerAt(int(m_markers.size()) - 1);
    } else if (const auto *removal = std::get_if<Removal>(&action)) {
        revertRemoval(*removal);
    } else if (const auto *erasure = std::get_if<Erasure>(&action)) {
        for (auto it = erasure->steps.crbegin(); it != erasure->steps.crend(); ++it) revertRemoval(*it);
    }
    ++m_revision;
}

void AnnotationDocument::insertStroke(int index, const Stroke &stroke)
{
    m_strokes.insert(index, stroke);
    m_points += int(stroke.points.size());
    m_bytes += jsonBytes(stroke);
}

void AnnotationDocument::removeStrokeAt(int index)
{
    if (index < 0 || index >= m_strokes.size()) return;
    const Stroke stroke = m_strokes.takeAt(index);
    m_points -= int(stroke.points.size());
    m_bytes -= jsonBytes(stroke);
}

void AnnotationDocument::insertMarker(int index, const Marker &marker)
{
    m_markers.insert(index, marker);
    m_bytes += jsonBytes(marker);
}

void AnnotationDocument::removeMarkerAt(int index)
{
    if (index < 0 || index >= m_markers.size()) return;
    m_bytes -= jsonBytes(m_markers.takeAt(index));
}

QByteArray AnnotationDocument::toJson() const
{
    QString json = QStringLiteral("{\"schema_version\":1,\"image_width\":%1,\"image_height\":%2,\"source_width\":%1,\"source_height\":%2,\"strokes\":[")
                       .arg(m_width)
                       .arg(m_height);
    for (int index = 0; index < m_strokes.size(); ++index) {
        if (index > 0) json += QLatin1Char(',');
        json += strokeJson(m_strokes.at(index));
    }
    json += QLatin1Char(']');
    if (!m_markers.isEmpty()) {
        json += QStringLiteral(",\"markers\":[");
        for (int index = 0; index < m_markers.size(); ++index) {
            if (index > 0) json += QLatin1Char(',');
            json += markerJson(m_markers.at(index));
        }
        json += QLatin1Char(']');
    }
    json += QLatin1Char('}');
    return json.toUtf8();
}

std::optional<AnnotationDocument> AnnotationDocument::fromJson(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.value(QLatin1String("schema_version")).toInt() != 1) return std::nullopt;
    AnnotationDocument document(root.value(QLatin1String("image_width")).toInt(), root.value(QLatin1String("image_height")).toInt());
    if (document.isNull()) return std::nullopt;
    for (const QJsonValue &value : root.value(QLatin1String("strokes")).toArray()) {
        const QJsonObject object = value.toObject();
        Stroke stroke;
        stroke.tool = object.value(QLatin1String("tool")).toString() == QLatin1String("highlighter") ? Stroke::Tool::Highlighter : Stroke::Tool::Pen;
        stroke.color = QColor(object.value(QLatin1String("color")).toString());
        if (!stroke.color.isValid()) stroke.color = Qt::black;
        stroke.opacity = object.value(QLatin1String("opacity")).toDouble(1);
        stroke.width = object.value(QLatin1String("width")).toDouble(document.penWidth());
        for (const QJsonValue &point : object.value(QLatin1String("points")).toArray()) {
            const QJsonArray pair = point.toArray();
            stroke.points.append(QPointF(pair.at(0).toDouble(), pair.at(1).toDouble()));
        }
        if (!stroke.points.isEmpty()) document.insertStroke(int(document.m_strokes.size()), stroke);
    }
    for (const QJsonValue &value : root.value(QLatin1String("markers")).toArray()) {
        const QJsonObject object = value.toObject();
        Marker marker{QPointF(object.value(QLatin1String("x")).toDouble(), object.value(QLatin1String("y")).toDouble()),
                      object.value(QLatin1String("number")).toInt(1), QColor(object.value(QLatin1String("color")).toString())};
        if (!marker.color.isValid()) marker.color = Qt::black;
        document.insertMarker(int(document.m_markers.size()), marker);
    }
    return document;
}

void AnnotationDocument::paintStroke(QPainter *painter, const Stroke &stroke)
{
    QColor color = stroke.color;
    color.setAlphaF(qBound(0.0, stroke.opacity, 1.0));
    if (stroke.points.size() == 1) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(stroke.points.first(), stroke.width / 2, stroke.width / 2);
        return;
    }
    QPainterPath path(stroke.points.first());
    for (int index = 1; index < stroke.points.size(); ++index) path.lineTo(stroke.points.at(index));
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(path);
}

void AnnotationDocument::paint(QPainter *painter, const Stroke *live) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    for (const Stroke::Tool tool : {Stroke::Tool::Highlighter, Stroke::Tool::Pen}) {
        for (const Stroke &stroke : m_strokes) {
            if (stroke.tool == tool) paintStroke(painter, stroke);
        }
        if (live && live->tool == tool && !live->points.isEmpty()) paintStroke(painter, *live);
    }
    // Markers on top: a filled disc in the marker's colour, a thin white ring (visible on dark
    // ink) and the number in white.
    const qreal radius = markerRadius();
    for (const Marker &marker : m_markers) {
        painter->setBrush(marker.color);
        painter->setPen(QPen(Qt::white, radius * 0.12));
        painter->drawEllipse(marker.centre, radius, radius);
        const QString label = QString::number(marker.number);
        QFont font = painter->font();
        font.setBold(true);
        font.setPixelSize(qMax(1, qRound(radius * (label.size() <= 2 ? 1.15 : 0.85))));
        painter->setFont(font);
        painter->setPen(Qt::white);
        painter->drawText(QRectF(marker.centre.x() - radius, marker.centre.y() - radius, 2 * radius, 2 * radius), Qt::AlignCenter, label);
    }
    painter->restore();
}

QImage AnnotationDocument::composite(const QImage &image) const
{
    QImage output(image.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::white);
    QPainter painter(&output);
    painter.drawImage(0, 0, image);
    if (!isNull() && (m_width != image.width() || m_height != image.height())) {
        painter.scale(double(image.width()) / m_width, double(image.height()) / m_height);
    }
    paint(&painter);
    painter.end();
    return output.convertToFormat(QImage::Format_RGB32);
}

}
