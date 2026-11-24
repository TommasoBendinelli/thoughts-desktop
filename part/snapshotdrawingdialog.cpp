// SPDX-License-Identifier: GPL-2.0-or-later
#include "snapshotdrawingdialog.h"

#include "reviewfeedbackoverlay.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Thoughts
{
namespace
{
// How far from the pointer the eraser reaches, on screen, as on the PDF.
constexpr double eraserRadius = 8.0;

bool sameStrokes(const QList<DocumentFeedback::Stroke> &a, const QList<DocumentFeedback::Stroke> &b)
{
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(), [](const DocumentFeedback::Stroke &x, const DocumentFeedback::Stroke &y) {
        return x.colour == y.colour && x.points == y.points;
    });
}

bool darkPalette(const QWidget *widget)
{
    return widget->palette().color(QPalette::Window).lightness() < 128;
}

// A ring as wide as the eraser reaches, dark with a light edge to show on any picture.
QCursor eraserCursor()
{
    const int side = int(std::ceil(eraserRadius * 2.0)) + 4;
    QPixmap pixmap(QSize(side, side) * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPointF centre(side / 2.0, side / 2.0);
    painter.setPen(QPen(QColor(255, 255, 255, 220), 3.0));
    painter.drawEllipse(centre, eraserRadius, eraserRadius);
    painter.setPen(QPen(QColor(0, 0, 0, 200), 1.2));
    painter.drawEllipse(centre, eraserRadius, eraserRadius);
    painter.end();
    return QCursor(pixmap, side / 2, side / 2);
}
}

// The picture and the strokes on it. Each change (a stroke, an eraser drag, Clear) can be taken back
// with Undo, as on the PDF.
class SnapshotCanvas : public QWidget
{
public:
    SnapshotCanvas(const QImage &picture, const QList<DocumentFeedback::Stroke> &strokes, QWidget *parent)
        : QWidget(parent)
        , strokes(strokes)
        , m_picture(picture)
    {
        setObjectName(QStringLiteral("snapshotDrawingCanvas"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(240, 160);
        setCursor(Qt::CrossCursor);
    }

    QList<DocumentFeedback::Stroke> strokes;
    QList<QList<DocumentFeedback::Stroke>> history;
    QString colour;
    bool eraser = false;
    std::function<void()> changed;
    std::function<void()> full;

    QSize sizeHint() const override
    {
        return m_picture.deviceIndependentSize().toSize().expandedTo(minimumSize());
    }

    // As large as it was on screen, smaller when the dialog is, in the middle.
    QRectF pictureRect() const
    {
        const QSizeF natural = m_picture.deviceIndependentSize();
        if (natural.isEmpty() || width() <= 0 || height() <= 0) return {};
        const double scale = std::min({1.0, width() / natural.width(), height() / natural.height()});
        const QSizeF shown = natural * scale;
        return QRectF(QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0), shown);
    }

    void setEraser(bool on)
    {
        eraser = on;
        setCursor(on ? eraserCursor() : QCursor(Qt::CrossCursor));
    }

    void undo()
    {
        if (history.isEmpty()) return;
        strokes = history.takeLast();
        update();
        if (changed) changed();
    }

    void clear()
    {
        if (strokes.isEmpty()) return;
        history.append(strokes);
        strokes.clear();
        update();
        if (changed) changed();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window).darker(darkPalette(this) ? 130 : 108));
        const QRectF picture = pictureRect();
        if (picture.isEmpty()) return;
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(picture, m_picture);
        QList<DocumentFeedback::Stroke> shown = strokes;
        if (!m_stroke.points.isEmpty()) shown.append(m_stroke);
        ReviewFeedbackOverlay::paintStrokes(&painter, 1, shown, [picture](int, const QRectF &) { return picture; }, darkPalette(this));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QRectF picture = pictureRect();
        // On the picture, not on the space around it.
        if (picture.isEmpty() || !picture.adjusted(-eraserRadius, -eraserRadius, eraserRadius, eraserRadius).contains(event->position())) return;
        m_pressed = true;
        if (eraser) {
            m_beforeErasing = strokes;
            m_eraserAt = event->position();
            eraseAlong(event->position());
            return;
        }
        if (strokes.size() >= DocumentFeedback::maximumStrokes) {
            m_pressed = false;
            if (full) full();
            return;
        }
        m_stroke = {{}, colour};
        extend(event->position());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        if (eraser) eraseAlong(event->position());
        else extend(event->position());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !std::exchange(m_pressed, false)) return;
        if (eraser) {
            // One Undo puts back what one drag removed.
            if (!sameStrokes(strokes, m_beforeErasing)) {
                history.append(std::exchange(m_beforeErasing, {}));
                if (changed) changed();
            }
            return;
        }
        const DocumentFeedback::Stroke stroke = std::exchange(m_stroke, {});
        if (stroke.points.isEmpty()) return;
        history.append(strokes);
        strokes.append(stroke);
        update();
        if (changed) changed();
    }

private:
    void extend(const QPointF &at)
    {
        const QRectF picture = pictureRect();
        if (picture.isEmpty()) return;
        // It stays on the picture, however far the pointer goes.
        const QPointF point(std::clamp((at.x() - picture.left()) / picture.width(), 0.0, 1.0), std::clamp((at.y() - picture.top()) / picture.height(), 0.0, 1.0));
        QList<QPointF> &points = m_stroke.points;
        if (!points.isEmpty()) {
            // Movement under a pixel and a half adds size to the record, not shape to the line.
            const QPointF last = points.constLast();
            if (std::hypot((point.x() - last.x()) * picture.width(), (point.y() - last.y()) * picture.height()) < 1.5) return;
            if (points.size() >= DocumentFeedback::maximumStrokePoints) return;
        }
        points.append(point);
        update();
    }

    void eraseAlong(const QPointF &to)
    {
        const QPointF from = std::exchange(m_eraserAt, to);
        const QRectF picture = pictureRect();
        const qsizetype before = strokes.size();
        strokes.removeIf([&](const DocumentFeedback::Stroke &stroke) { return DocumentFeedback::strokeNear(stroke, picture, from, to, eraserRadius); });
        if (strokes.size() != before) update();
    }

    QImage m_picture;
    DocumentFeedback::Stroke m_stroke;
    bool m_pressed = false;
    QPointF m_eraserAt;
    QList<DocumentFeedback::Stroke> m_beforeErasing;
};

SnapshotDrawingDialog::SnapshotDrawingDialog(const QImage &snapshot, const QList<DocumentFeedback::Stroke> &strokes, bool existing, const QString &colour, bool eraser,
                                             QWidget *parent)
    : QDialog(parent)
    , m_initial(strokes)
{
    setObjectName(QStringLiteral("snapshotDrawingDialog"));
    setWindowTitle(i18n("Draw on This Page"));
    auto *layout = new QVBoxLayout(this);

    auto *tools = new QHBoxLayout;
    // The pencil's colours, as in the menu of its button (PageView::setupActions).
    const struct {
        const char *key;
        QString name;
        QString colour;
    } colours[] = {
        {"blue", i18nc("@action pencil colour", "Blue"), QString()},
        {"black", i18nc("@action pencil colour", "Black"), QStringLiteral("#212121")},
        {"red", i18nc("@action pencil colour", "Red"), QStringLiteral("#d32f2f")},
        {"green", i18nc("@action pencil colour", "Green"), QStringLiteral("#2e7d32")},
        {"orange", i18nc("@action pencil colour", "Orange"), QStringLiteral("#ef6c00")},
    };
    for (const auto &choice : colours) {
        auto *button = new QToolButton(this);
        button->setObjectName(QStringLiteral("snapshotColour_") + QLatin1StringView(choice.key));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolTip(choice.name);
        button->setAccessibleName(choice.name);
        button->setProperty("colour", choice.colour);
        QPixmap swatch(32, 32);
        swatch.setDevicePixelRatio(2.0);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(choice.colour.isEmpty() ? ReviewFeedbackOverlay::colour(ReviewCardFeedback::neutral(), darkPalette(this)) : QColor::fromString(choice.colour));
        painter.drawEllipse(QRectF(2.0, 2.0, 12.0, 12.0));
        painter.end();
        button->setIcon(QIcon(swatch));
        connect(button, &QToolButton::clicked, this, [this, colour = choice.colour] {
            choose(colour, false);
            Q_EMIT pencilChosen(colour, false);
        });
        tools->addWidget(button);
        m_colours.append(button);
    }
    m_eraser = new QToolButton(this);
    m_eraser->setObjectName(QStringLiteral("snapshotEraser"));
    m_eraser->setCheckable(true);
    m_eraser->setAutoRaise(true);
    m_eraser->setIcon(QIcon::fromTheme(QStringLiteral("draw-eraser")));
    m_eraser->setText(i18nc("@action the pencil's eraser", "Eraser"));
    m_eraser->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_eraser->setToolTip(i18n("Remove whole strokes"));
    connect(m_eraser, &QToolButton::clicked, this, [this] {
        choose(this->colour(), true);
        Q_EMIT pencilChosen(this->colour(), true);
    });
    tools->addWidget(m_eraser);
    tools->addStretch(1);
    m_full = new QLabel(i18np("A drawing holds %1 stroke.", "A drawing holds %1 strokes.", DocumentFeedback::maximumStrokes), this);
    m_full->setObjectName(QStringLiteral("snapshotDrawingFull"));
    m_full->setVisible(false);
    tools->addWidget(m_full);
    m_undo = new QToolButton(this);
    m_undo->setObjectName(QStringLiteral("snapshotUndo"));
    m_undo->setText(i18n("Undo"));
    m_undo->setAutoRaise(true);
    m_undo->setShortcut(QKeySequence::Undo);
    tools->addWidget(m_undo);
    m_clear = new QToolButton(this);
    m_clear->setObjectName(QStringLiteral("snapshotClear"));
    m_clear->setText(i18n("Clear"));
    m_clear->setAutoRaise(true);
    tools->addWidget(m_clear);
    layout->addLayout(tools);

    m_canvas = new SnapshotCanvas(snapshot, strokes, this);
    m_canvas->changed = [this] { updateButtons(); };
    m_canvas->full = [this] { m_full->setVisible(true); };
    layout->addWidget(m_canvas, 1);
    connect(m_undo, &QToolButton::clicked, this, &SnapshotDrawingDialog::undo);
    connect(m_clear, &QToolButton::clicked, this, &SnapshotDrawingDialog::clear);

    auto *buttons = new QDialogButtonBox(this);
    if (existing) {
        QPushButton *remove = buttons->addButton(i18n("Delete"), QDialogButtonBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("snapshotDrawingDelete"));
        remove->setAutoDefault(false);
        connect(remove, &QPushButton::clicked, this, &SnapshotDrawingDialog::deleteRequested);
    }
    QPushButton *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("snapshotDrawingCancel"));
    cancel->setAutoDefault(false);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_save = buttons->addButton(i18n("Save"), QDialogButtonBox::ApplyRole);
    m_save->setObjectName(QStringLiteral("snapshotDrawingSave"));
    m_save->setDefault(true);
    // The owner writes the record and closes the dialog once it is saved.
    connect(m_save, &QPushButton::clicked, this, [this] { Q_EMIT saveRequested(this->strokes()); });
    layout->addWidget(buttons);

    choose(colour, eraser);
    updateButtons();
    // As large as the page was on screen, within the screen.
    QSize wanted = m_canvas->sizeHint() + QSize(32, 120);
    const QScreen *screen = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
    if (screen) wanted = wanted.boundedTo(screen->availableGeometry().size() * 0.9);
    resize(wanted);
}

QList<DocumentFeedback::Stroke> SnapshotDrawingDialog::strokes() const
{
    return m_canvas->strokes;
}

bool SnapshotDrawingDialog::changed() const
{
    return !sameStrokes(m_canvas->strokes, m_initial);
}

QString SnapshotDrawingDialog::colour() const
{
    return m_canvas->colour;
}

bool SnapshotDrawingDialog::erasing() const
{
    return m_canvas->eraser;
}

void SnapshotDrawingDialog::choose(const QString &colour, bool eraser)
{
    m_canvas->colour = DocumentFeedback::strokeColour(colour);
    m_canvas->setEraser(eraser);
    for (QToolButton *button : std::as_const(m_colours)) button->setChecked(!eraser && button->property("colour").toString() == m_canvas->colour);
    m_eraser->setChecked(eraser);
}

QRectF SnapshotDrawingDialog::pictureRect() const
{
    return m_canvas->pictureRect();
}

QWidget *SnapshotDrawingDialog::canvas() const
{
    return m_canvas;
}

void SnapshotDrawingDialog::undo()
{
    m_canvas->undo();
}

void SnapshotDrawingDialog::clear()
{
    m_canvas->clear();
}

void SnapshotDrawingDialog::updateButtons()
{
    const QList<DocumentFeedback::Stroke> &now = m_canvas->strokes;
    m_undo->setEnabled(!m_canvas->history.isEmpty());
    m_clear->setEnabled(!now.isEmpty());
    // Nothing to keep without a stroke: Delete removes a drawing made before.
    m_save->setEnabled(!now.isEmpty() && changed());
    if (now.size() < DocumentFeedback::maximumStrokes) m_full->setVisible(false);
}
}
