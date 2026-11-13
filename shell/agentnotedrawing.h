// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include <optional>
#include <variant>

class QPainter;

// What the user draws on an agent note's picture before replying, as the phone draws it
// (thoughts-android data/agentnotes/AnnotationDocument.kt): pen and highlighter strokes, numbered
// markers, an eraser that removes whole strokes and markers, undo and redo. Written as the
// protocol's stroke document (schema 1, protocol/agent-notes.md) and drawn onto the picture for
// the reply image. Everything is in image pixels.
namespace Thoughts::AgentNotes
{

struct Stroke {
    enum class Tool { Pen, Highlighter };
    Tool tool = Tool::Pen;
    QColor color = Qt::black;  // opaque; opacity apart
    qreal width = 2;
    qreal opacity = 1;
    QVector<QPointF> points;

    QRectF bounds() const;
};

// "1", "2", ...: a place the user names in the comment ("1: why this sign?").
struct Marker {
    QPointF centre;
    int number = 1;
    QColor color = Qt::black;
};

class AnnotationDocument
{
public:
    AnnotationDocument() = default;
    AnnotationDocument(int imageWidth, int imageHeight);

    // The server's limits (the phone's, with the same margin).
    static constexpr int MaxStrokes = 5000;
    static constexpr int MaxPoints = 200000;
    static constexpr int MaxMarkers = 99;
    static constexpr int MaxJsonBytes = 2 * 1024 * 1024 - 4096;
    static constexpr qreal HighlighterOpacity = 0.35;
    static constexpr qreal HighlighterWidthFactor = 4;

    int imageWidth() const { return m_width; }
    int imageHeight() const { return m_height; }
    bool isNull() const { return m_width <= 0 || m_height <= 0; }
    bool isEmpty() const { return m_strokes.isEmpty() && m_markers.isEmpty(); }
    const QList<Stroke> &strokes() const { return m_strokes; }
    const QList<Marker> &markers() const { return m_markers; }
    int pointCount() const { return m_points; }

    // Thin enough to write with, visible when the picture is fit to a window.
    static qreal penWidthFor(int imageWidth);
    // 3% of the width, at least 14 px.
    static qreal markerRadiusFor(int imageWidth);
    qreal penWidth() const { return penWidthFor(m_width); }
    qreal markerRadius() const { return markerRadiusFor(m_width); }

    // A finished stroke of @p tool through @p points (image pixels; one point is a dot).
    // False when a limit would be passed.
    bool addStroke(Stroke::Tool tool, const QVector<QPointF> &points, const QColor &color = Qt::black);
    Stroke makeStroke(Stroke::Tool tool, const QVector<QPointF> &points, const QColor &color = Qt::black) const;
    // A marker numbered one more than the highest present, centred at @p centre.
    std::optional<Marker> addMarker(const QPointF &centre, const QColor &color = Qt::black);
    int nextMarkerNumber() const;
    // Removes every stroke and marker within @p radius of the segment @p from - @p to, as one
    // undo step. Returns how many went.
    int erase(const QPointF &from, const QPointF &to, qreal radius);
    // Removes everything, as one undo step.
    void clear();
    // One eraser drag is one undo step: the erasures between beginGesture() and
    // endGesture(its value) become one.
    int beginGesture() const { return int(m_undo.size()); }
    void endGesture(int start);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();
    // Grows with every change (views repaint and relabel when it moves).
    int revision() const { return m_revision; }

    // The stroke document: schema 1, points with one decimal, markers only when there are any.
    QByteArray toJson() const;
    static std::optional<AnnotationDocument> fromJson(const QByteArray &json);

    // Highlighters first, then pens, then the markers, onto @p painter in image pixels.
    void paint(QPainter *painter, const Stroke *live = nullptr) const;
    static void paintStroke(QPainter *painter, const Stroke &stroke);
    // @p image (white under transparency) with the drawing on it, at the image's size.
    QImage composite(const QImage &image) const;

private:
    struct AddStroke {
        Stroke stroke;
    };
    struct AddMarker {
        Marker marker;
    };
    struct Removal {
        QList<std::pair<int, Stroke>> strokes;  // index each had, ascending
        QList<std::pair<int, Marker>> markers;
    };
    struct Erasure {
        QList<Removal> steps;  // in the order they happened
    };
    using Action = std::variant<AddStroke, AddMarker, Removal, Erasure>;
    void applyRemoval(const Removal &removal);
    void revertRemoval(const Removal &removal);

    void apply(const Action &action);
    void revert(const Action &action);
    void insertStroke(int index, const Stroke &stroke);
    void removeStrokeAt(int index);
    void insertMarker(int index, const Marker &marker);
    void removeMarkerAt(int index);
    static int jsonBytes(const Stroke &stroke);
    static int jsonBytes(const Marker &marker);

    int m_width = 0;
    int m_height = 0;
    QList<Stroke> m_strokes;
    QList<Marker> m_markers;
    int m_points = 0;
    int m_bytes = 160;
    QList<Action> m_undo;
    QList<Action> m_redo;
    int m_revision = 0;
};

}
