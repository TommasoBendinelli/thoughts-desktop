// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "documentfeedback.h"
#include "okularpart_export.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPainterPath>
#include <QRectF>
#include <QSet>

#include <functional>

class QPainter;

namespace Thoughts
{
// The reading overlays of the PDF being read (reading-overlays.md §4): finds each mark on the
// displayed PDF as document feedback is found (DocumentFeedback::place: the regions as they are on
// the same PDF, else the quote searched near its pages), paints the washes and the comment markers
// in the margin, says what is under the pointer, and writes the user's Delete Mark and Delete
// Overlay through a writer. Whether an overlay shows on the page (its eye) is kept on this
// computer; a new overlay shows. The page view decides whether overlays show at all (Visible
// markup › Reading overlays).
class OKULARPART_EXPORT ReadingOverlayLayer : public QObject
{
    Q_OBJECT
public:
    using Writer = std::function<bool(const QJsonObject &record, QString *error)>;
    using PageText = std::function<ReviewCardFeedback::AnswerText(const QList<int> &pages)>;
    using Map = std::function<QRectF(int page, const QRectF &rect)>;

    // One overlay as the displayed PDF shows it. A Project read as one PDF numbers its pages across
    // its documents: an overlay of one of them has its pages moved by @p pageOffset and, while it is
    // on @p documentPdfSha256 (that document's PDF in the reading), counts as made on @p shownPdfSha256.
    struct Source {
        QJsonObject record;
        int pageOffset = 0;
        QString documentPdfSha256;
    };

    // A mark as the displayed PDF shows it.
    struct Mark {
        QString overlayId;
        QJsonObject mark;
        QString state;                       // exact, relocated or orphaned (not in this version)
        QList<QPair<int, QRectF>> rects;     // 1-based page, normalized; empty when orphaned
    };

    // What is at a point of the page: a mark's comment marker, or its wash.
    struct Hit {
        QString overlayId;
        QString markId;
        bool marker = false;
        bool isEmpty() const
        {
            return overlayId.isEmpty();
        }
    };

    explicit ReadingOverlayLayer(QObject *parent = nullptr);

    // The displayed PDF: its Library document ID and the SHA-256 of its bytes, and its page count.
    // Placements are forgotten when they change; without both no overlay shows.
    void setDocument(const QString &documentId, const QString &pdfSha256, int pageCount);
    bool hasDocument() const;
    QString documentId() const;
    QString pdfSha256() const;
    // The text of whole pages of the displayed PDF, for marks made on another version
    // (DocumentFeedbackOverlay::pagesText, which keeps the pages' glyphs).
    void setPageText(PageText pageText);
    // The document's overlays, deleted ones included (their tombstones), as stored.
    void setRecords(const QList<QJsonObject> &records);
    void setSources(const QList<Source> &sources);
    // The PDF's pages were replaced (a reload): the marks are found again.
    void resetPages();
    void clear();
    void setWriter(Writer writer);

    // The overlays not deleted, newest first; record() also a deleted one.
    QList<QJsonObject> overlays() const;
    QJsonObject record(const QString &overlayId) const;

    // The eye (§4): shown on the page or not, kept on this computer by overlay_id.
    bool isShown(const QString &overlayId) const;
    void setShown(const QString &overlayId, bool shown);
    // The overlays not deleted that are shown.
    int shownCount() const;

    // The marks of @p overlayId as the displayed PDF shows them, in page order (page, then top,
    // then left of the first rectangle), those not in this version last in the record's order.
    QList<Mark> marks(const QString &overlayId);
    Mark mark(const QString &overlayId, const QString &markId);

    // Paints the washes of the shown overlays' marks that lie on @p pages (1-based, the pages
    // painted now), the oldest overlay first, then their comment markers. @p map gives a
    // normalized rectangle of a page in painter coordinates, or an empty one when it is not shown.
    void paint(QPainter *painter, const QSet<int> &pages, const Map &map);
    // paint() in two layers: the washes of @p page with @p map giving a normalized rectangle of that
    // page in painter coordinates (on the page image, under the user's markup), and the markers.
    void paintWashes(QPainter *painter, int page, const std::function<QRectF(const QRectF &normalized)> &map);
    void paintMarkers(QPainter *painter, const QSet<int> &pages, const Map &map);
    // The mark under @p point (painter coordinates) on @p page: a comment marker first, then a wash,
    // the newest overlay on top. Empty when none.
    Hit hitTest(int page, const QPointF &point, const Map &map);
    // Where the comment marker of a mark is (painter coordinates); empty when it has none or its
    // overlay is hidden.
    QRectF markerRect(const QString &overlayId, const QString &markId, const Map &map);

    // Delete Mark and Delete Overlay (§2): the next revision through the writer, shown at once.
    bool removeMark(const QString &overlayId, const QString &markId, QString *error = nullptr);
    bool removeOverlay(const QString &overlayId, QString *error = nullptr);

    // The wash of a kind (§4): read green, skim yellow, skip grey, with their alpha; none for note.
    static QColor washColour(const QString &kind);
    // The underline of a note, and the colour of a kind's comment marker (opaque).
    static QColor noteColour();
    static QColor kindColour(const QString &kind);
    // The marker's size in pixels.
    static constexpr double markerSize = 14.0;

Q_SIGNALS:
    void changed();

private:
    struct Placed {
        QJsonObject anchor;
        DocumentFeedback::Placement placement;
    };
    DocumentFeedback::Placement placementOf(const Source &source, const QJsonObject &mark);
    const Source *source(const QString &overlayId) const;
    // The shown marks with a comment on @p pages, with their marker rectangles (one per mark,
    // stacked down the margin where they would overlap).
    QList<QPair<Mark, QRectF>> markers(const QSet<int> &pages, const Map &map);
    QList<Mark> shownMarks(const QSet<int> &pages);
    bool write(const QJsonObject &record, QString *error);
    void saveHidden() const;

    QString m_documentId;
    QString m_pdfSha256;
    int m_pageCount = 0;
    PageText m_pageText;
    QList<Source> m_sources;
    Writer m_writer;
    QSet<QString> m_hidden;
    // By overlay_id, then mark_id: the anchor it was placed from and where it is.
    QHash<QString, QHash<QString, Placed>> m_placements;
    // marks() of each overlay as they are now (painting and the pointer ask for them all the time);
    // forgotten whenever the overlays, the PDF or the placements change.
    QHash<QString, QList<Mark>> m_marks;
};
}
