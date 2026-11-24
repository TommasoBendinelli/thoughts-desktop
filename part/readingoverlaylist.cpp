// SPDX-License-Identifier: GPL-2.0-or-later
#include "readingoverlaylist.h"

#include "agentconversationopener.h"
#include "proposalprovenance.h"
#include "readingoverlaylayer.h"
#include "readingoverlayrequests.h"
#include "readingoverlays.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QIcon>
#include <QPushButton>
#include <QResizeEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace Thoughts
{
namespace
{
// A mark of the list: a click on it asks the page for the mark, a right-click opens its menu.
class MarkRow : public QFrame
{
public:
    MarkRow(std::function<void()> clicked, std::function<void(const QPoint &globalPosition)> menu, QWidget *parent)
        : QFrame(parent)
        , m_clicked(std::move(clicked))
        , m_menu(std::move(menu))
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && m_clicked) m_clicked();
        QFrame::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!m_menu) {
            QFrame::contextMenuEvent(event);
            return;
        }
        event->accept();
        // A copy: the menu's action may rebuild the list, this row with it.
        const auto menu = m_menu;
        menu(event->globalPos());
    }

private:
    std::function<void()> m_clicked;
    std::function<void(const QPoint &)> m_menu;
};

// The eye of an overlay, open or struck through, drawn in @p colour (no icon theme here).
QIcon eyeIcon(bool open, const QColor &colour)
{
    QPixmap pixmap(QSize(32, 32));
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(colour, 1.3));
    painter.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(1.5, 8.0);
    eye.quadTo(8.0, 1.5, 14.5, 8.0);
    eye.quadTo(8.0, 14.5, 1.5, 8.0);
    painter.drawPath(eye);
    painter.setBrush(colour);
    painter.drawEllipse(QPointF(8.0, 8.0), 2.2, 2.2);
    if (!open) {
        painter.setPen(QPen(colour, 1.5));
        painter.drawLine(QPointF(2.5, 13.5), QPointF(13.5, 2.5));
    }
    return QIcon(pixmap);
}

// Three dots: the overlay's menu.
QIcon menuIcon(const QColor &colour)
{
    QPixmap pixmap(QSize(32, 32));
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    for (const double x : {3.5, 8.0, 12.5}) painter.drawEllipse(QPointF(x, 8.0), 1.4, 1.4);
    return QIcon(pixmap);
}

QString kindName(const QString &kind)
{
    if (kind == ReadingOverlays::read()) return i18nc("@label reading overlay mark", "Read carefully");
    if (kind == ReadingOverlays::skim()) return i18nc("@label reading overlay mark", "Skim");
    if (kind == ReadingOverlays::skip()) return i18nc("@label reading overlay mark", "You can skip this");
    return i18nc("@label reading overlay mark", "Note");
}

// The kind's colour as the page shows it: a wash, or the line under a note.
QPixmap kindSwatch(const QString &kind)
{
    QPixmap pixmap(QSize(20, 20));
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ReadingOverlayLayer::kindColour(kind));
    if (kind == ReadingOverlays::note()) painter.drawRoundedRect(QRectF(0.5, 7.0, 9.0, 2.5), 1.0, 1.0);
    else painter.drawRoundedRect(QRectF(0.5, 0.5, 9.0, 9.0), 2.0, 2.0);
    return pixmap;
}

QLabel *secondaryLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    return label;
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
}

QString breakableText(const QString &text)
{
    QString result;
    int run = 0;
    for (const QChar character : text) {
        run = character.isSpace() ? 0 : run + 1;
        result.append(character);
        if (run >= 24 && !character.isLowSurrogate() && !character.isHighSurrogate()) {
            result.append(QChar(0x200B));
            run = 0;
        }
    }
    return result;
}

ReadingOverlayList::ReadingOverlayList(QWidget *parent)
    : QScrollArea(parent)
{
    setObjectName(QStringLiteral("readingOverlayList"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(0);
    connect(m_rebuildTimer, &QTimer::timeout, this, &ReadingOverlayList::rebuild);
    rebuild();
}

void ReadingOverlayList::setLayer(ReadingOverlayLayer *layer)
{
    if (m_layer == layer) return;
    if (m_layer) disconnect(m_layer, nullptr, this, nullptr);
    m_layer = layer;
    if (m_layer) connect(m_layer, &ReadingOverlayLayer::changed, this, &ReadingOverlayList::scheduleRebuild);
    scheduleRebuild();
}

int ReadingOverlayList::shownCount() const
{
    return m_shownCount;
}

QString ReadingOverlayList::revealedOverlay() const
{
    return m_revealed;
}

void ReadingOverlayList::scheduleRebuild()
{
    // Coalesced, and never inside the click of a button that the rebuild replaces.
    m_rebuildTimer->start();
}

QWidget *ReadingOverlayList::overlayEntry(const QString &overlayId)
{
    if (!widget()) return nullptr;
    for (QFrame *entry : widget()->findChildren<QFrame *>(QStringLiteral("readingOverlay"))) {
        if (entry->property("overlayId").toString() == overlayId) return entry;
    }
    return nullptr;
}

QWidget *ReadingOverlayList::markRow(const QString &overlayId, const QString &markId)
{
    QWidget *entry = overlayEntry(overlayId);
    if (!entry) return nullptr;
    for (QFrame *row : entry->findChildren<QFrame *>(QStringLiteral("readingOverlayMark"))) {
        if (row->property("markId").toString() == markId) return row;
    }
    return nullptr;
}

QString ReadingOverlayList::revealedMark() const
{
    return m_revealedMark;
}

bool ReadingOverlayList::revealMark(const QString &overlayId, const QString &markId)
{
    if (!revealOverlay(overlayId)) return false;
    m_revealedMark = markId;
    QWidget *row = markRow(overlayId, markId);
    for (QFrame *other : widget()->findChildren<QFrame *>(QStringLiteral("readingOverlayMark"))) {
        if (other->property("revealed").toBool() == (other == row)) continue;
        other->setProperty("revealed", other == row);
        repolish(other);
    }
    if (!row) return false;
    // After revealOverlay() put the overlay's top at the top: the row in view too.
    QTimer::singleShot(0, this, [this, overlayId, markId] {
        if (QWidget *shown = markRow(overlayId, markId)) ensureWidgetVisible(shown, 0, 24);
    });
    return true;
}

QMenu *ReadingOverlayList::markMenu(const QString &overlayId, const QString &markId, QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("readingOverlayMarkMenu"));
    QAction *remove = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Mark"), this, [this, overlayId, markId] {
        QString error;
        if (m_layer && !m_layer->removeMark(overlayId, markId, &error)) Q_EMIT failed(i18n("The mark could not be deleted: %1", error));
    });
    remove->setObjectName(QStringLiteral("readingOverlayDeleteMark"));
    remove->setToolTip(i18n("Take this mark off the overlay, on every device"));
    return menu;
}

bool ReadingOverlayList::revealOverlay(const QString &overlayId)
{
    m_revealed = overlayId;
    m_revealedMark.clear();
    if (m_rebuildTimer->isActive()) rebuild();
    QWidget *entry = overlayEntry(overlayId);
    if (!entry) return false;
    for (QFrame *other : widget()->findChildren<QFrame *>(QStringLiteral("readingOverlay"))) other->setProperty("revealed", other == entry);
    widget()->style()->unpolish(widget());
    widget()->style()->polish(widget());
    for (QFrame *row : widget()->findChildren<QFrame *>(QStringLiteral("readingOverlayMark"))) {
        if (!row->property("revealed").toBool()) continue;
        row->setProperty("revealed", false);
        repolish(row);
    }
    // Once laid out: its top at the top of the list.
    QTimer::singleShot(0, this, [this, overlayId] {
        if (QWidget *shown = overlayEntry(overlayId)) verticalScrollBar()->setValue(shown->mapTo(widget(), QPoint(0, 0)).y());
    });
    return true;
}

void ReadingOverlayList::rebuild()
{
    m_rebuildTimer->stop();
    const int scroll = verticalScrollBar()->value();
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("readingOverlayContent"));
    content->setStyleSheet(QStringLiteral(
        "QFrame#readingOverlay { border-radius: 8px; background: rgba(128, 128, 128, 22); }"
        "QFrame#readingOverlay[revealed=\"true\"] { background: rgba(66, 133, 244, 40); }"
        "QFrame#readingOverlayMark { border-radius: 5px; }"
        "QFrame#readingOverlayMark:hover { background: rgba(128, 128, 128, 40); }"
        "QFrame#readingOverlayMark[revealed=\"true\"] { background: rgba(66, 133, 244, 70); }"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(8);
    const QColor text = palette().color(QPalette::WindowText);
    // The eye and the menu are plain icons, a checked eye no pressed button.
    const QString buttonStyle = QStringLiteral("QToolButton { border: 0; background: transparent; padding: 2px; }"
                                               "QToolButton:hover { background: rgba(128, 128, 128, 50); border-radius: 4px; }");
    const QList<QJsonObject> overlays = m_layer ? m_layer->overlays() : QList<QJsonObject>();
    int shown = 0;
    for (const QJsonObject &record : overlays) {
        const QString overlayId = ReadingOverlays::overlayIdOf(record);
        const bool visible = m_layer->isShown(overlayId);
        if (visible) ++shown;
        auto *entry = new QFrame(content);
        entry->setObjectName(QStringLiteral("readingOverlay"));
        entry->setProperty("overlayId", overlayId);
        entry->setProperty("revealed", overlayId == m_revealed);
        auto *entryLayout = new QVBoxLayout(entry);
        entryLayout->setContentsMargins(8, 6, 6, 8);
        entryLayout->setSpacing(4);

        // Title row: the eye, the title, the menu.
        auto *head = new QHBoxLayout;
        head->setSpacing(4);
        auto *eye = new QToolButton(entry);
        eye->setObjectName(QStringLiteral("readingOverlayEye"));
        eye->setStyleSheet(buttonStyle);
        eye->setCheckable(true);
        eye->setChecked(visible);
        eye->setAutoRaise(true);
        eye->setIcon(eyeIcon(visible, text));
        eye->setToolTip(visible ? i18n("Shown on the page. Click to hide this overlay on this computer.") : i18n("Hidden. Click to show this overlay on the page."));
        connect(eye, &QToolButton::toggled, this, [this, overlayId](bool on) {
            if (m_layer) m_layer->setShown(overlayId, on);
        });
        head->addWidget(eye, 0, Qt::AlignTop);
        auto *title = new QLabel(breakableText(record.value(QStringLiteral("title")).toString()), entry);
        title->setObjectName(QStringLiteral("readingOverlayTitle"));
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(true);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        head->addWidget(title, 1);
        auto *menuButton = new QToolButton(entry);
        menuButton->setObjectName(QStringLiteral("readingOverlayMenu"));
        menuButton->setStyleSheet(buttonStyle + QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
        menuButton->setAutoRaise(true);
        menuButton->setIcon(menuIcon(text));
        menuButton->setToolTip(i18n("More"));
        menuButton->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(menuButton);
        const QString conversation = proposalConversationLink(ReadingOverlays::conversationUrl(record), ReadingOverlays::conversationId(record));
        if (!conversation.isEmpty()) {
            QAction *open = menu->addAction(i18n("Open Conversation"), this, [this, conversation] {
                if (!openAgentConversationUrl(conversation)) Q_EMIT failed(i18n("The conversation could not be opened."));
            });
            open->setObjectName(QStringLiteral("readingOverlayOpenConversation"));
            open->setToolTip(i18n("Open the conversation of the agent that made this overlay."));
        }
        QAction *remove = menu->addAction(i18n("Delete Overlay"), this, [this, overlayId] {
            QString error;
            if (m_layer && !m_layer->removeOverlay(overlayId, &error)) Q_EMIT failed(i18n("The overlay could not be deleted: %1", error));
        });
        remove->setObjectName(QStringLiteral("readingOverlayDelete"));
        menuButton->setMenu(menu);
        head->addWidget(menuButton, 0, Qt::AlignTop);
        entryLayout->addLayout(head);

        // The agent and the date.
        const QString label = ReadingOverlays::authorLabel(record);
        const QDateTime created = QDateTime::fromString(record.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        const QString date = created.isValid() ? QLocale().toString(created.toLocalTime(), QLocale::ShortFormat) : QString();
        QLabel *byline = secondaryLabel(label.isEmpty() ? date : date.isEmpty() ? label : i18nc("@info agent label · date", "%1 · %2", label, date), entry);
        byline->setObjectName(QStringLiteral("readingOverlayByline"));
        entryLayout->addWidget(byline);
        const QString summary = record.value(QStringLiteral("summary")).toString().trimmed();
        if (!summary.isEmpty()) {
            auto *summaryLabel = new QLabel(breakableText(summary), entry);
            summaryLabel->setObjectName(QStringLiteral("readingOverlaySummary"));
            summaryLabel->setTextFormat(Qt::PlainText);
            summaryLabel->setWordWrap(true);
            summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            entryLayout->addWidget(summaryLabel);
        }

        // The marks in page order; those not in this version last, under their own heading.
        bool missingHeading = false;
        for (const ReadingOverlayLayer::Mark &mark : m_layer->marks(overlayId)) {
            const QString markId = ReadingOverlays::markIdOf(mark.mark);
            if (mark.rects.isEmpty() && !missingHeading) {
                missingHeading = true;
                QLabel *heading = secondaryLabel(i18n("Not in this version"), entry);
                heading->setObjectName(QStringLiteral("readingOverlayNotInVersion"));
                heading->setToolTip(i18n("These passages were not found in the PDF on screen: it changed since the agent marked them."));
                entryLayout->addSpacing(2);
                entryLayout->addWidget(heading);
            }
            auto *row = new MarkRow([this, overlayId, markId] { Q_EMIT markRequested(overlayId, markId); },
                                    [this, overlayId, markId](const QPoint &globalPosition) {
                                        // Right-click: Delete Mark (§4; the page's popup asks about the mark instead).
                                        QMenu *menu = markMenu(overlayId, markId, this);
                                        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
                                        menu->popup(globalPosition);
                                    },
                                    entry);
            row->setObjectName(QStringLiteral("readingOverlayMark"));
            row->setProperty("overlayId", overlayId);
            row->setProperty("markId", markId);
            row->setProperty("notInVersion", mark.rects.isEmpty());
            row->setProperty("revealed", overlayId == m_revealed && markId == m_revealedMark);
            const QString kind = ReadingOverlays::kindOf(mark.mark);
            row->setToolTip(kindName(kind));
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(4, 3, 4, 3);
            rowLayout->setSpacing(6);
            // The kind's colour, as on the page: a wash, or the line under a note.
            auto *swatch = new QLabel(row);
            swatch->setObjectName(QStringLiteral("readingOverlayMarkColour"));
            swatch->setProperty("kind", kind);
            swatch->setFixedSize(10, 10);
            swatch->setPixmap(kindSwatch(kind));
            rowLayout->addWidget(swatch, 0, Qt::AlignTop);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(1);
            auto *quote = new QLabel(QStringLiteral("“%1”").arg(breakableText(ReadingOverlays::quoteStart(mark.mark))), row);
            quote->setObjectName(QStringLiteral("readingOverlayMarkQuote"));
            quote->setTextFormat(Qt::PlainText);
            quote->setWordWrap(true);
            QFont italic = quote->font();
            italic.setItalic(true);
            quote->setFont(italic);
            if (mark.rects.isEmpty()) {
                QPalette faded = quote->palette();
                faded.setColor(QPalette::WindowText, faded.color(QPalette::PlaceholderText));
                quote->setPalette(faded);
            }
            texts->addWidget(quote);
            const QString commentText = ReadingOverlays::commentOf(mark.mark);
            if (!commentText.isEmpty()) {
                auto *comment = new QLabel(breakableText(commentText), row);
                comment->setObjectName(QStringLiteral("readingOverlayMarkComment"));
                comment->setTextFormat(Qt::PlainText);
                comment->setWordWrap(true);
                texts->addWidget(comment);
            }
            rowLayout->addLayout(texts, 1);
            entryLayout->addWidget(row);
        }
        if (!visible) {
            // Hidden on the page: listed, faded.
            QPalette faded = title->palette();
            faded.setColor(QPalette::WindowText, faded.color(QPalette::PlaceholderText));
            title->setPalette(faded);
        }
        layout->addWidget(entry);
    }
    if (overlays.isEmpty()) {
        QLabel *empty = secondaryLabel(i18n("No reading overlays on this PDF yet. Copy a request above and paste it to an agent with the Thoughts tools "
                                            "(an Agent Study session, or the Claude app): it marks here what to read, skim or skip, with its comments, "
                                            "and the overlay shows after the next Sync."),
                                       content);
        empty->setObjectName(QStringLiteral("readingOverlayEmpty"));
        empty->setAlignment(Qt::AlignCenter);
        layout->addSpacing(12);
        layout->addWidget(empty);
    }
    layout->addStretch(1);
    // The old list goes after this event (a button of it may be what is being clicked).
    QWidget *old = takeWidget();
    if (old) old->deleteLater();
    setWidget(content);
    verticalScrollBar()->setValue(scroll);
    if (shown != m_shownCount) {
        m_shownCount = shown;
        Q_EMIT shownCountChanged(shown);
    }
}

ReadingOverlayRequestBar::ReadingOverlayRequestBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("readingOverlayRequestBar"));
    m_layout = new QBoxLayout(QBoxLayout::LeftToRight, this);
    auto *layout = m_layout;
    layout->setContentsMargins(4, 4, 4, 0);
    layout->setSpacing(6);
    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("readingOverlayRequestKind"));
    m_kind->setToolTip(i18n("What the agent bases the reading overlay on"));
    m_kind->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_kind->setMinimumContentsLength(8);
    m_kind->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("Reading Overlays"));
    const auto remembered = ReadingOverlayRequests::fromKey(config.readEntry("RequestKind", QString()));
    for (const auto kind : ReadingOverlayRequests::kinds()) {
        m_kind->addItem(ReadingOverlayRequests::label(kind), ReadingOverlayRequests::key(kind));
        m_kind->setItemData(m_kind->count() - 1, ReadingOverlayRequests::description(kind), Qt::ToolTipRole);
        if (kind == remembered) m_kind->setCurrentIndex(m_kind->count() - 1);
    }
    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] {
        KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Reading Overlays"));
        group.writeEntry("RequestKind", m_kind->currentData().toString());
        group.sync();
    });
    layout->addWidget(m_kind, 1);
    m_copy = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Request"), this);
    m_copy->setObjectName(QStringLiteral("readingOverlayRequestCopy"));
    m_copy->setAutoDefault(false);
    m_copy->setToolTip(i18n("Copies a request to paste to an agent with the Thoughts tools (an Agent Study session, or the Claude app): "
                            "it makes a reading overlay of this PDF"));
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (!copyRequest()) return;
        m_copy->setText(i18n("Copied"));
        QTimer::singleShot(2000, m_copy, [copy = m_copy] { copy->setText(i18n("Copy Request")); });
    });
    m_copy->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(m_copy);
    arrange();
}

void ReadingOverlayRequestBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    arrange();
}

void ReadingOverlayRequestBar::arrange()
{
    int longest = 0;
    for (int index = 0; index < m_kind->count(); ++index) longest = std::max(longest, m_kind->fontMetrics().horizontalAdvance(m_kind->itemText(index)));
    // The combo's arrow and frame take about three character heights.
    const int kindWidth = longest + 3 * m_kind->fontMetrics().height();
    const QMargins margins = m_layout->contentsMargins();
    const int needed = margins.left() + kindWidth + m_layout->spacing() + m_copy->sizeHint().width() + margins.right();
    const auto direction = width() >= needed ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
    if (m_layout->direction() != direction) m_layout->setDirection(direction);
}

void ReadingOverlayRequestBar::setDocumentProvider(std::function<Document()> provider)
{
    m_provider = std::move(provider);
}

QString ReadingOverlayRequestBar::request() const
{
    if (!m_provider) return QString();
    const Document document = m_provider();
    if (document.filename.isEmpty() && document.documentId.isEmpty()) return QString();
    return ReadingOverlayRequests::text(ReadingOverlayRequests::fromKey(m_kind->currentData().toString()), document.name, document.filename,
                                        document.documentId);
}

bool ReadingOverlayRequestBar::copyRequest()
{
    const QString text = request();
    if (text.isEmpty()) return false;
    QApplication::clipboard()->setText(text);
    Q_EMIT copied(text);
    return true;
}
}
