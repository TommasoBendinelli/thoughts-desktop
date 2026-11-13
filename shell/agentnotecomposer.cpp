// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnotecomposer.h"

#include <KLocalizedString>

#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <cmath>

namespace Thoughts
{

using AgentNotes::AnnotationDocument;
using AgentNotes::Stroke;

// ---------------------------------------------------------------- canvas

AgentNoteCanvas::AgentNoteCanvas(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("agentNoteCanvas"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setTool(Tool::Pen);
    updateSize();
}

void AgentNoteCanvas::setImage(const QImage &image)
{
    m_image = image;
    m_scaled = QPixmap();
    updateSize();
    update();
}

void AgentNoteCanvas::setDocument(AnnotationDocument *document)
{
    m_document = document;
    update();
}

void AgentNoteCanvas::setTool(Tool tool)
{
    m_tool = tool;
    switch (tool) {
    case Tool::None:
        unsetCursor();
        break;
    case Tool::Eraser:
        setCursor(Qt::PointingHandCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
        break;
    }
}

void AgentNoteCanvas::fitWidth(int available)
{
    available = qMax(160, available);
    if (available == m_available) return;
    m_available = available;
    m_scaled = QPixmap();
    updateSize();
}

void AgentNoteCanvas::updateSize()
{
    if (m_image.isNull()) {
        setFixedSize(m_available, qMax(120, m_available / 2));
        return;
    }
    const int width = m_available;
    setFixedSize(width, qMax(1, qRound(width * double(m_image.height()) / m_image.width())));
}

qreal AgentNoteCanvas::scale() const
{
    return m_image.isNull() || width() <= 0 ? 1.0 : double(m_image.width()) / width();
}

QPointF AgentNoteCanvas::toImage(const QPointF &point) const
{
    const QPointF image = point * scale();
    return QPointF(qBound(0.0, image.x(), double(m_image.width())), qBound(0.0, image.y(), double(m_image.height())));
}

QPointF AgentNoteCanvas::fromImage(const QPointF &point) const
{
    return point / scale();
}

qreal AgentNoteCanvas::eraserRadius() const
{
    // About 10 points on screen, in image pixels; never thinner than the pen.
    return qMax(m_document ? m_document->penWidth() * 2 : 4.0, 10 * scale());
}

void AgentNoteCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (m_image.isNull()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, i18n("Loading the picture…"));
        return;
    }
    const qreal ratio = devicePixelRatioF();
    const QSize target = (QSizeF(size()) * ratio).toSize();
    if (m_scaled.isNull() || m_scaled.size() != target) {
        m_scaled = QPixmap::fromImage(m_image.size() == target ? m_image : m_image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        m_scaled.setDevicePixelRatio(ratio);
    }
    painter.drawPixmap(0, 0, m_scaled);
    if (!m_document) return;
    painter.scale(1 / scale(), 1 / scale());
    if (m_pressed && (m_tool == Tool::Pen || m_tool == Tool::Highlighter) && !m_points.isEmpty()) {
        const Stroke live = m_document->makeStroke(m_tool == Tool::Highlighter ? Stroke::Tool::Highlighter : Stroke::Tool::Pen, m_points);
        m_document->paint(&painter, &live);
    } else {
        m_document->paint(&painter);
    }
}

void AgentNoteCanvas::addPoint(const QPointF &imagePoint)
{
    // Points closer than 1.5 image pixels to the last kept one add nothing (the phone's rule).
    if (!m_points.isEmpty()) {
        const QPointF last = m_points.last();
        if (std::hypot(imagePoint.x() - last.x(), imagePoint.y() - last.y()) < 1.5) return;
    }
    m_points.append(QPointF(std::round(imagePoint.x() * 10) / 10, std::round(imagePoint.y() * 10) / 10));
}

void AgentNoteCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_document || m_image.isNull() || m_tool == Tool::None) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF point = toImage(event->position());
    switch (m_tool) {
    case Tool::Number:
        if (!m_document->addMarker(point)) Q_EMIT limitReached();
        else Q_EMIT documentChanged();
        update();
        return;
    case Tool::Eraser:
        m_pressed = true;
        m_gesture = m_document->beginGesture();
        m_lastErase = point;
        if (m_document->erase(point, point, eraserRadius()) > 0) Q_EMIT documentChanged();
        update();
        return;
    default:
        m_pressed = true;
        m_points.clear();
        addPoint(point);
        update();
        return;
    }
}

void AgentNoteCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressed || !m_document) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPointF point = toImage(event->position());
    if (m_tool == Tool::Eraser) {
        if (m_document->erase(m_lastErase, point, eraserRadius()) > 0) Q_EMIT documentChanged();
        m_lastErase = point;
    } else {
        addPoint(point);
    }
    update();
}

void AgentNoteCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_pressed || event->button() != Qt::LeftButton || !m_document) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_pressed = false;
    if (m_tool == Tool::Eraser) {
        m_document->endGesture(m_gesture);
        m_gesture = -1;
        Q_EMIT documentChanged();
    } else {
        addPoint(toImage(event->position()));
        const Stroke::Tool tool = m_tool == Tool::Highlighter ? Stroke::Tool::Highlighter : Stroke::Tool::Pen;
        if (!m_points.isEmpty()) {
            if (m_document->addStroke(tool, m_points)) Q_EMIT documentChanged();
            else Q_EMIT limitReached();
        }
        m_points.clear();
    }
    update();
}

// ---------------------------------------------------------------- composer

namespace
{
QToolButton *toolButton(const QString &objectName, const QString &icon, const QString &text, const QString &tip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(QIcon::fromTheme(icon));
    button->setText(text);
    button->setToolTip(tip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    return button;
}
}

AgentNoteComposer::AgentNoteComposer(AgentNotes::Center *center, const QString &noteId, QWidget *parent)
    : QDialog(parent)
    , m_center(center)
    , m_noteId(noteId)
{
    setObjectName(QStringLiteral("agentNoteComposer"));
    const auto note = center ? center->note(noteId) : std::nullopt;
    setWindowTitle(i18n("Reply: %1", note && !note->title.isEmpty() ? note->title : i18n("Agent note")));
    m_draft = center ? center->draft(noteId) : std::make_shared<AgentNotes::Center::Draft>();
    setUp(note ? note->title : QString());
    if (note && center) {
        QPointer<AgentNoteComposer> guard(this);
        center->client()->image(note->imageUrl, note->imageSha256, note->mediaType, this, [guard](const QImage &image, const QString &error) {
            if (!guard) return;
            if (image.isNull()) {
                guard->m_status->setText(error.isEmpty() ? i18n("The picture could not be loaded.") : error);
                return;
            }
            guard->imageReady(image);
        });
        connect(center, &AgentNotes::Center::draftChanged, this, [this](const QString &id) {
            if (id != m_noteId || m_sending) return;
            // Sent from the reply box under the note: this drawing went with it.
            if (m_center && m_center->existingDraft(m_noteId) != m_draft) {
                accept();
                return;
            }
            if (m_comment->hasFocus()) return;
            if (m_comment->toPlainText() != m_draft->message) {
                m_syncingComment = true;
                m_comment->setPlainText(m_draft->message);
                m_syncingComment = false;
            }
        });
    } else {
        m_status->setText(i18n("This note is no longer listed."));
    }
    m_comment->setPlainText(m_draft->message);
}

AgentNoteComposer::AgentNoteComposer(AgentNotes::Center *center, const AgentNotes::Center::Page &page, QWidget *parent)
    : QDialog(parent)
    , m_center(center)
    , m_isPage(true)
    , m_page(page)
{
    setObjectName(QStringLiteral("agentNotePageComposer"));
    setWindowTitle(i18n("Send Page to Agent"));
    // One note ID for this page: sending it again after a failure is the same note.
    if (m_page.noteId.isEmpty()) m_page.noteId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_noteId = m_page.noteId;
    setUp(QString());
    m_title->setText(m_page.title);
    m_comment->setPlainText(m_page.message);
    imageReady(m_page.image);
}

AnnotationDocument *AgentNoteComposer::document()
{
    return m_isPage ? &m_page.drawing : &m_draft->drawing;
}

void AgentNoteComposer::setUp(const QString &heading)
{
    resize(980, 1040);
    if (const QScreen *screen = parentWidget() ? parentWidget()->screen() : QGuiApplication::primaryScreen()) {
        const QSize available = screen->availableGeometry().size();
        resize(qMin(980, available.width() - 40), qMin(1040, available.height() - 40));
    }
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 12);
    outer->setSpacing(8);

    if (m_isPage) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(i18n("Title:"), this));
        m_title = new QLineEdit(this);
        m_title->setObjectName(QStringLiteral("agentNoteComposerTitle"));
        m_title->setMaxLength(200);
        row->addWidget(m_title, 1);
        outer->addLayout(row);
    } else if (!heading.isEmpty()) {
        auto *label = new QLabel(heading, this);
        label->setObjectName(QStringLiteral("agentNoteComposerHeading"));
        label->setWordWrap(true);
        QFont font = label->font();
        font.setWeight(QFont::Bold);
        font.setPointSizeF(font.pointSizeF() * 1.15);
        label->setFont(font);
        outer->addWidget(label);
    }

    // The phone's tools: pen, highlighter, eraser, numbered markers; undo, redo; and Clear.
    auto *tools = new QHBoxLayout;
    tools->setSpacing(2);
    m_pen = toolButton(QStringLiteral("agentNotePen"), QStringLiteral("draw-freehand"), i18n("Pen"), i18n("Draw in black"), this);
    m_highlighter = toolButton(QStringLiteral("agentNoteHighlighter"), QStringLiteral("draw-highlight"), i18n("Highlighter"), i18n("Mark broadly, see-through"), this);
    m_eraser = toolButton(QStringLiteral("agentNoteEraser"), QStringLiteral("draw-eraser"), i18n("Eraser"), i18n("Remove whole strokes and markers"), this);
    m_number = toolButton(QStringLiteral("agentNoteNumber"), QStringLiteral("format-list-ordered"), i18n("Number"),
                          i18n("Drop a numbered marker (1, 2, …) to refer to in the comment"), this);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    for (QToolButton *button : {m_pen, m_highlighter, m_eraser, m_number}) {
        button->setCheckable(true);
        group->addButton(button);
        tools->addWidget(button);
    }
    tools->addSpacing(12);
    m_undo = toolButton(QStringLiteral("agentNoteUndo"), QStringLiteral("edit-undo"), i18n("Undo"), QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText), this);
    m_redo = toolButton(QStringLiteral("agentNoteRedo"), QStringLiteral("edit-redo"), i18n("Redo"), QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText), this);
    m_clear = toolButton(QStringLiteral("agentNoteClear"), QStringLiteral("edit-clear"), i18n("Clear"), i18n("Remove the whole drawing (Undo brings it back)"), this);
    tools->addWidget(m_undo);
    tools->addWidget(m_redo);
    tools->addWidget(m_clear);
    tools->addStretch(1);
    outer->addLayout(tools);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("agentNoteComposerScroll"));
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_canvas = new AgentNoteCanvas(m_scroll);
    m_canvas->setDocument(nullptr);
    m_scroll->setWidget(m_canvas);
    m_scroll->viewport()->installEventFilter(this);
    outer->addWidget(m_scroll, 1);

    m_comment = new QPlainTextEdit(this);
    m_comment->setObjectName(QStringLiteral("agentNoteComposerComment"));
    m_comment->setPlaceholderText(m_isPage ? i18n("A comment for the agent (optional)") : i18n("Your reply to the agent (optional with a drawing)"));
    m_comment->setTabChangesFocus(true);
    m_comment->setFixedHeight(m_comment->fontMetrics().lineSpacing() * 4 + 14);
    outer->addWidget(m_comment);

    auto *buttons = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("agentNoteComposerStatus"));
    m_status->setWordWrap(true);
    auto *close = new QPushButton(m_isPage ? i18n("Cancel") : i18n("Close"), this);
    close->setObjectName(QStringLiteral("agentNoteComposerClose"));
    close->setToolTip(m_isPage ? QString() : i18n("The drawing and the comment stay with the note until you send them"));
    m_send = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), m_isPage ? i18n("Send to Agent") : i18n("Send Reply"), this);
    m_send->setObjectName(QStringLiteral("agentNoteComposerSend"));
    m_send->setDefault(true);
    m_send->setToolTip(QKeySequence(Qt::CTRL | Qt::Key_Return).toString(QKeySequence::NativeText));
    buttons->addWidget(m_status, 1);
    buttons->addWidget(close);
    buttons->addWidget(m_send);
    outer->addLayout(buttons);

    connect(m_pen, &QToolButton::clicked, this, [this] { setTool(AgentNoteCanvas::Tool::Pen); });
    connect(m_highlighter, &QToolButton::clicked, this, [this] { setTool(AgentNoteCanvas::Tool::Highlighter); });
    connect(m_eraser, &QToolButton::clicked, this, [this] { setTool(AgentNoteCanvas::Tool::Eraser); });
    connect(m_number, &QToolButton::clicked, this, [this] { setTool(AgentNoteCanvas::Tool::Number); });
    connect(m_undo, &QToolButton::clicked, this, [this] {
        document()->undo();
        m_canvas->update();
        Q_EMIT m_canvas->documentChanged();
    });
    connect(m_redo, &QToolButton::clicked, this, [this] {
        document()->redo();
        m_canvas->update();
        Q_EMIT m_canvas->documentChanged();
    });
    connect(m_clear, &QToolButton::clicked, this, [this] {
        document()->clear();
        m_canvas->update();
        Q_EMIT m_canvas->documentChanged();
    });
    // Undo and Redo while drawing; in the comment field its own text undo wins.
    auto *undo = new QShortcut(QKeySequence::Undo, this);
    connect(undo, &QShortcut::activated, m_undo, &QToolButton::click);
    auto *redo = new QShortcut(QKeySequence::Redo, this);
    connect(redo, &QShortcut::activated, m_redo, &QToolButton::click);
    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sendShortcut, &QShortcut::activated, m_send, &QPushButton::click);
    connect(m_canvas, &AgentNoteCanvas::documentChanged, this, [this] {
        if (!m_isPage && m_center) m_center->draftDrawingChanged(m_noteId);
        updateControls();
    });
    connect(m_canvas, &AgentNoteCanvas::limitReached, this, [this] { m_status->setText(i18n("The drawing is as large as a reply can be.")); });
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this] {
        if (m_syncingComment) return;
        if (m_isPage) m_page.message = m_comment->toPlainText();
        else if (m_center) m_center->setDraftMessage(m_noteId, m_comment->toPlainText());
        updateControls();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &AgentNoteComposer::send);
    m_pen->setChecked(true);
    setTool(AgentNoteCanvas::Tool::Pen);
    updateControls();
}

void AgentNoteComposer::imageReady(const QImage &image)
{
    AnnotationDocument *drawing = document();
    // The drawing is in this picture's pixels; one from another size starts again.
    if (drawing->isNull() || drawing->imageWidth() != image.width() || drawing->imageHeight() != image.height()) {
        *drawing = AnnotationDocument(image.width(), image.height());
    }
    m_canvas->setImage(image);
    m_canvas->setDocument(drawing);
    QTimer::singleShot(0, this, &AgentNoteComposer::fitCanvas);
    updateControls();
}

bool AgentNoteComposer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize) fitCanvas();
    return QDialog::eventFilter(watched, event);
}

void AgentNoteComposer::fitCanvas()
{
    // Measured without the scroll bar, so its coming and going does not resize the picture.
    QScrollBar *bar = m_scroll->verticalScrollBar();
    const int barWidth = bar->isVisible() ? 0 : qMax(style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, bar), bar->sizeHint().width());
    const int available = m_scroll->viewport()->width() - (bar->isVisible() ? 0 : barWidth);
    const int natural = m_canvas->image().isNull() ? available : m_canvas->image().width();
    m_canvas->fitWidth(qMin(available, qMax(natural, 400)));
}

void AgentNoteComposer::setTool(AgentNoteCanvas::Tool tool)
{
    m_canvas->setTool(tool);
    for (const auto &[button, value] : {std::pair{m_pen, AgentNoteCanvas::Tool::Pen}, std::pair{m_highlighter, AgentNoteCanvas::Tool::Highlighter},
                                        std::pair{m_eraser, AgentNoteCanvas::Tool::Eraser}, std::pair{m_number, AgentNoteCanvas::Tool::Number}}) {
        if (value == tool && !button->isChecked()) button->setChecked(true);
    }
}

void AgentNoteComposer::updateControls()
{
    AnnotationDocument *drawing = document();
    const bool ready = !m_canvas->image().isNull();
    m_undo->setEnabled(ready && drawing->canUndo());
    m_redo->setEnabled(ready && drawing->canRedo());
    m_clear->setEnabled(ready && !drawing->isEmpty());
    const bool hasContent = !drawing->isEmpty() || !m_comment->toPlainText().trimmed().isEmpty();
    // A page goes as it is; a reply needs a drawing or a comment.
    m_send->setEnabled(ready && !m_sending && (m_isPage || hasContent));
}

void AgentNoteComposer::send()
{
    if (!m_center || m_sending || !m_send->isEnabled()) return;
    m_sending = true;
    m_status->setText(i18n("Sending…"));
    updateControls();
    QPointer<AgentNoteComposer> guard(this);
    const auto finished = [guard](bool ok, const QString &error) {
        if (!guard) return;
        guard->m_sending = false;
        if (ok) {
            guard->accept();
            return;
        }
        guard->m_status->setText(i18n("Not sent: %1", error));
        guard->updateControls();
    };
    if (m_isPage) {
        m_page.title = m_title->text().trimmed().isEmpty() ? m_page.title : m_title->text().trimmed();
        m_page.message = m_comment->toPlainText();
        m_center->sendPage(m_page, finished);
    } else {
        m_center->sendReply(m_noteId, finished);
    }
}

}
