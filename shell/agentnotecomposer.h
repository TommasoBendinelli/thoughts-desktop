// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "agentnotes.h"

#include <QDialog>
#include <QPixmap>
#include <QPointer>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QToolButton;

class AgentNotesTest;
class MainShellTest;

namespace Thoughts
{

// A picture to draw on, as on the phone: the pen and the highlighter draw, the eraser removes
// whole strokes and markers, a click with the number tool drops the next numbered marker. The
// picture fits the width it is given; the drawing is kept in image pixels.
class AgentNoteCanvas : public QWidget
{
    Q_OBJECT
public:
    enum class Tool { None, Pen, Highlighter, Eraser, Number };

    explicit AgentNoteCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    QImage image() const { return m_image; }
    // Not owned; null draws nothing.
    void setDocument(AgentNotes::AnnotationDocument *document);
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    void fitWidth(int available);
    // Widget pixels to image pixels, and back.
    QPointF toImage(const QPointF &point) const;
    QPointF fromImage(const QPointF &point) const;
    qreal eraserRadius() const;

Q_SIGNALS:
    void documentChanged();
    void limitReached();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void updateSize();
    qreal scale() const;
    void addPoint(const QPointF &imagePoint);

    QImage m_image;
    QPixmap m_scaled;
    AgentNotes::AnnotationDocument *m_document = nullptr;
    Tool m_tool = Tool::Pen;
    int m_available = 800;
    bool m_pressed = false;
    QVector<QPointF> m_points;
    QPointF m_lastErase;
    int m_gesture = -1;
};

// The phone's note screen on the Mac: the note's picture large with the drawing tools, the
// comment, and Send. Two uses: a reply to an agent note (the drawing and comment are the
// note's draft, kept in the Center until sent) and a page of a document sent to the agent.
class AgentNoteComposer : public QDialog
{
    Q_OBJECT
    friend class ::AgentNotesTest;
    friend class ::MainShellTest;

public:
    // A reply to @p noteId.
    AgentNoteComposer(AgentNotes::Center *center, const QString &noteId, QWidget *parent = nullptr);
    // @p page, to send to the agent.
    AgentNoteComposer(AgentNotes::Center *center, const AgentNotes::Center::Page &page, QWidget *parent = nullptr);

    QString noteId() const { return m_noteId; }
    bool isPage() const { return m_isPage; }
    AgentNoteCanvas *canvas() const { return m_canvas; }
    AgentNotes::AnnotationDocument *document();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setUp(const QString &heading);
    void imageReady(const QImage &image);
    void setTool(AgentNoteCanvas::Tool tool);
    void updateControls();
    void fitCanvas();
    void send();

    QPointer<AgentNotes::Center> m_center;
    QString m_noteId;
    bool m_isPage = false;
    std::shared_ptr<AgentNotes::Center::Draft> m_draft;
    AgentNotes::Center::Page m_page;
    bool m_sending = false;
    bool m_syncingComment = false;

    QLineEdit *m_title = nullptr;
    QScrollArea *m_scroll = nullptr;
    AgentNoteCanvas *m_canvas = nullptr;
    QToolButton *m_pen = nullptr;
    QToolButton *m_highlighter = nullptr;
    QToolButton *m_eraser = nullptr;
    QToolButton *m_number = nullptr;
    QToolButton *m_undo = nullptr;
    QToolButton *m_redo = nullptr;
    QToolButton *m_clear = nullptr;
    QPlainTextEdit *m_comment = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
};

}
