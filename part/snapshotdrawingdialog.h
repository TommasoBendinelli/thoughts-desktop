// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Draw on This Page (thoughts-server protocol/document-feedback.md 10.1): an HTML attachment page has
// no fixed layout to draw on (zoom, hidden series, page state), so the pencil draws on a snapshot, a
// picture of the page as the user saw it. This dialog shows the picture and takes strokes on it with
// the pencil's colours, an eraser that removes whole strokes, Undo and Clear, as on a page of the
// PDF. Save hands over the strokes, normalized to the picture (origin at its top left); the dialog
// writes nothing. A drawing made before opens with its strokes, which can grow, and Delete deletes it.
// It never draws on the live page.
#include "documentfeedback.h"
#include "okularpart_export.h"

#include <QDialog>
#include <QImage>
#include <QList>

class QAbstractButton;
class QLabel;
class QPushButton;
class QToolButton;

namespace Thoughts
{
class SnapshotCanvas;

class OKULARPART_EXPORT SnapshotDrawingDialog : public QDialog
{
    Q_OBJECT
public:
    // @p snapshot at its device pixel ratio (the page's size on screen); @p strokes of a drawing made
    // before (@p existing), else none; the pencil's @p colour ("#rrggbb", or empty for its own blue)
    // and @p eraser as chosen last.
    SnapshotDrawingDialog(const QImage &snapshot, const QList<DocumentFeedback::Stroke> &strokes, bool existing, const QString &colour, bool eraser,
                          QWidget *parent = nullptr);

    QList<DocumentFeedback::Stroke> strokes() const;
    // Whether the strokes differ from those it opened with.
    bool changed() const;
    QString colour() const;
    bool erasing() const;
    // The pencil with @p colour, or its eraser.
    void choose(const QString &colour, bool eraser);
    // Where the picture is on the canvas (canvas coordinates), and the canvas (tests drive it).
    QRectF pictureRect() const;
    QWidget *canvas() const;
    void undo();
    void clear();

Q_SIGNALS:
    void saveRequested(const QList<DocumentFeedback::Stroke> &strokes);
    void deleteRequested();
    // The pencil's colour or its eraser chosen here: every pencil follows the choice.
    void pencilChosen(const QString &colour, bool eraser);

private:
    void updateButtons();

    SnapshotCanvas *m_canvas = nullptr;
    QList<QToolButton *> m_colours;
    QToolButton *m_eraser = nullptr;
    QToolButton *m_undo = nullptr;
    QToolButton *m_clear = nullptr;
    QLabel *m_full = nullptr;
    QPushButton *m_save = nullptr;
    QList<DocumentFeedback::Stroke> m_initial;
};
}
