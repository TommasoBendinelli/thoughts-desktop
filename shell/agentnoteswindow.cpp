// SPDX-License-Identifier: GPL-2.0-or-later
#include "agentnoteswindow.h"

#include "agentnotecomposer.h"

#include <KLocalizedString>

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QSignalBlocker>
#include <QToolTip>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace Thoughts
{

using AgentNotes::Note;
using AgentNotes::Reply;

namespace
{
QString firstLine(const QString &text, int limit = 160)
{
    QString line = text.simplified();
    if (line.size() > limit) line = line.left(limit - 1) + QChar(0x2026);
    return line;
}

QColor secondaryText(const QPalette &palette)
{
    QColor color = palette.color(QPalette::WindowText);
    color.setAlphaF(0.6);
    return color;
}

// Title, then "time · sender · replies", then the start of the message; a dot when unread.
class NoteItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        // Any width: a row is as wide as the list, a narrow sidebar's too (the text elides).
        const QFontMetrics metrics(option.font);
        return QSize(120, metrics.height() * 3 + metrics.leading() * 2 + 18);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem base(option);
        initStyleOption(&base, index);
        base.text.clear();
        base.icon = QIcon();
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &base, painter, widget);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool unread = index.data(AgentNotesView::UnreadRole).toBool();
        const QPalette &palette = option.palette;
        const QColor text = palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = text;
        dim.setAlphaF(selected ? 0.85 : 0.6);
        const QRect area = option.rect.adjusted(22, 8, -10, -8);
        if (unread) {
            const int side = 8;
            const QFontMetrics metrics(option.font);
            const QRectF dot(option.rect.left() + 8, area.top() + (metrics.height() - side) / 2.0, side, side);
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Highlight));
            painter->drawEllipse(dot);
        }
        QFont titleFont = option.font;
        titleFont.setWeight(unread ? QFont::Bold : QFont::Normal);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics metrics(option.font);
        int y = area.top();
        painter->setFont(titleFont);
        painter->setPen(text);
        painter->drawText(QRect(area.left(), y, area.width(), titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, area.width()));
        y += titleMetrics.height() + metrics.leading();
        painter->setFont(option.font);
        painter->setPen(dim);
        painter->drawText(QRect(area.left(), y, area.width(), metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data(AgentNotesView::MetaRole).toString(), Qt::ElideRight, area.width()));
        y += metrics.height() + metrics.leading();
        painter->drawText(QRect(area.left(), y, area.width(), metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data(AgentNotesView::PreviewRole).toString(), Qt::ElideRight, area.width()));
        painter->restore();
    }
};

QLabel *wrappingLabel(const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

void setSecondary(QLabel *label)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, secondaryText(label->palette()));
    label->setPalette(palette);
}
}

// ---------------------------------------------------------------- picture

AgentNoteImageView::AgentNoteImageView(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    updateSize();
}

void AgentNoteImageView::setSource(const QString &sha256, const QSize &expectedSize)
{
    m_sha256 = sha256;
    m_expectedSize = expectedSize;
    m_image = QImage();
    m_scaled = QPixmap();
    m_cachedPath.clear();
    m_status = i18n("Loading the picture…");
    m_loading = true;
    updateSize();
    update();
}

void AgentNoteImageView::setImage(const QImage &image)
{
    m_image = image;
    m_scaled = QPixmap();
    m_status.clear();
    m_loading = false;
    updateSize();
    update();
}

void AgentNoteImageView::setFailure(const QString &message)
{
    m_image = QImage();
    m_scaled = QPixmap();
    m_status = message;
    m_loading = false;
    updateSize();
    update();
}

void AgentNoteImageView::fitWidth(int available)
{
    available = qMax(120, available);
    if (available == m_available) return;
    m_available = available;
    m_scaled = QPixmap();
    updateSize();
}

void AgentNoteImageView::updateSize()
{
    const int width = qMin(m_available, MaxWidth);
    if (!m_image.isNull()) {
        setFixedSize(width, qMax(1, qRound(width * double(m_image.height()) / m_image.width())));
    } else if (m_loading && !m_expectedSize.isEmpty()) {
        // Room for the picture while it loads, so the replies below do not jump.
        setFixedSize(width, qMin(qRound(width * double(m_expectedSize.height()) / m_expectedSize.width()), 600));
    } else {
        setFixedSize(width, fontMetrics().height() * 3);
    }
}

void AgentNoteImageView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (m_image.isNull()) {
        painter.setPen(secondaryText(palette()));
        QColor frame = palette().color(QPalette::WindowText);
        frame.setAlphaF(0.15);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        painter.setPen(secondaryText(palette()));
        painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, m_status);
        return;
    }
    const qreal ratio = devicePixelRatioF();
    const QSize target = (QSizeF(size()) * ratio).toSize();
    if (m_scaled.isNull() || m_scaled.size() != target) {
        m_scaled = QPixmap::fromImage(m_image.size() == target ? m_image : m_image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        m_scaled.setDevicePixelRatio(ratio);
    }
    painter.drawPixmap(0, 0, m_scaled);
    QColor frame = palette().color(QPalette::WindowText);
    frame.setAlphaF(0.12);
    painter.setPen(frame);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void AgentNoteImageView::setClickable(bool clickable)
{
    m_clickable = clickable;
    updateCursor(m_hoveredLink);
}

void AgentNoteImageView::setLinks(const QList<AgentNotes::NoteLink> &links)
{
    m_links = links;
    m_hoveredLink = -1;
    setMouseTracking(!links.isEmpty());
    updateCursor(-1);
}

int AgentNoteImageView::linkAt(const QPointF &position) const
{
    if (m_links.isEmpty() || width() <= 0 || height() <= 0) return -1;
    // The picture fills the widget at its own aspect ratio (fitWidth), so widget fractions are picture fractions.
    const QPointF at(position.x() / width(), position.y() / height());
    for (int index = 0; index < m_links.size(); ++index) {
        if (m_links.at(index).rect.contains(at)) return index;
    }
    return -1;
}

void AgentNoteImageView::updateCursor(int link)
{
    setCursor(link >= 0 || m_clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void AgentNoteImageView::mouseMoveEvent(QMouseEvent *event)
{
    const int link = linkAt(event->position());
    if (link != m_hoveredLink) {
        m_hoveredLink = link;
        updateCursor(link);
        if (link >= 0) {
            const AgentNotes::NoteLink &target = m_links.at(link);
            const QString where = target.href.startsWith(QLatin1String("thoughts-pdf-link:"))
                ? i18n("Open in Thoughts") : target.href;
            QToolTip::showText(event->globalPosition().toPoint(), target.label.isEmpty() ? where : i18n("%1 — %2", target.label, where), this);
        } else {
            QToolTip::hideText();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void AgentNoteImageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        if (const int link = linkAt(event->position()); link >= 0) {
            Q_EMIT linkActivated(m_links.at(link).href);
            return;
        }
        if (m_clickable) {
            Q_EMIT clicked();
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void AgentNoteImageView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_image.isNull()) return;
    QMenu menu(this);
    QAction *copy = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Picture"));
    connect(copy, &QAction::triggered, this, [this] { QApplication::clipboard()->setImage(m_image); });
    if (!m_cachedPath.isEmpty() && QFileInfo::exists(m_cachedPath)) {
        QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open Picture"));
        connect(open, &QAction::triggered, this, [path = m_cachedPath] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    }
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------- view

AgentNotesView::AgentNotesView(Layout layout, AgentNotes::Center *center, DocumentResolver resolveDocument, DocumentOpener openDocument, CardOpener openCard,
                               QWidget *parent, Qt::WindowFlags flags)
    : QWidget(parent, flags)
    , m_layout(layout)
    , m_center(center)
    , m_resolveDocument(std::move(resolveDocument))
    , m_openDocument(std::move(openDocument))
    , m_openCard(std::move(openCard))
{
    const bool sidebar = layout == Layout::Sidebar;
    setObjectName(sidebar ? QStringLiteral("agentNotesSidebar") : QStringLiteral("agentNotesView"));
    setWindowTitle(i18n("Agent Notes"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Status, Mark All as Read and Refresh: above the list (Sidebar), across the window (Split).
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("agentNotesBar"));
    m_status = new QLabel(bar);
    m_status->setObjectName(QStringLiteral("agentNotesStatus"));
    m_status->setTextFormat(Qt::PlainText);
    setSecondary(m_status);
    const QString refreshTip = i18n("Ask the Thoughts server for new notes and replies (%1)", QKeySequence(QKeySequence::Refresh).toString(QKeySequence::NativeText));
    // Which notes: all, a project's or a document's.
    m_filterBox = new QComboBox(bar);
    m_filterBox->setObjectName(QStringLiteral("agentNotesFilter"));
    m_filterBox->setToolTip(i18n("Show the notes of one project or document"));
    m_filterBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_filterBox->setMinimumContentsLength(sidebar ? 10 : 24);
    if (sidebar) {
        // Two short rows, so the sidebar can stay narrow: "Agent Notes ⟳", then the status
        // with Mark All as Read.
        auto *markAllRead = new QToolButton(bar);
        markAllRead->setText(i18n("Mark All as Read"));
        markAllRead->setAutoRaise(true);
        m_markAllRead = markAllRead;
        auto *refresh = new QToolButton(bar);
        refresh->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
        refresh->setText(i18n("Refresh"));
        refresh->setToolButtonStyle(Qt::ToolButtonIconOnly);
        refresh->setAutoRaise(true);
        m_refresh = refresh;
        auto *barLayout = new QVBoxLayout(bar);
        barLayout->setContentsMargins(10, 6, 6, 4);
        barLayout->setSpacing(0);
        auto *top = new QHBoxLayout;
        top->setSpacing(4);
        auto *heading = new QLabel(i18n("Agent Notes"), bar);
        heading->setObjectName(QStringLiteral("agentNotesHeading"));
        QFont headingFont = heading->font();
        headingFont.setWeight(QFont::Bold);
        heading->setFont(headingFont);
        top->addWidget(heading, 1);
        top->addWidget(m_refresh);
        barLayout->addLayout(top);
        auto *show = new QHBoxLayout;
        show->setSpacing(4);
        show->addWidget(new QLabel(i18n("Show:"), bar));
        show->addWidget(m_filterBox, 1);
        barLayout->addLayout(show);
        auto *second = new QHBoxLayout;
        second->setSpacing(4);
        m_status->setWordWrap(true);
        QFont small = m_status->font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        m_status->setFont(small);
        m_markAllRead->setFont(small);
        second->addWidget(m_status, 1);
        second->addWidget(m_markAllRead, 0, Qt::AlignTop);
        barLayout->addLayout(second);
        // The phone's study mode: the page beside these notes, to the agent.
        auto *sendPage = new QToolButton(bar);
        sendPage->setText(i18n("Send Page to Agent…"));
        sendPage->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sendPage->setAutoRaise(true);
        m_sendPage = sendPage;
        barLayout->addWidget(m_sendPage, 0, Qt::AlignLeft);
    } else {
        m_markAllRead = new QPushButton(i18n("Mark All as Read"), bar);
        m_refresh = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Refresh"), bar);
        m_sendPage = new QPushButton(i18n("Send Page to Agent…"), bar);
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(12, 8, 12, 8);
        barLayout->addWidget(new QLabel(i18n("Show:"), bar));
        barLayout->addWidget(m_filterBox);
        barLayout->addSpacing(8);
        barLayout->addWidget(m_status, 1);
        barLayout->addWidget(m_sendPage);
        barLayout->addWidget(m_markAllRead);
        barLayout->addWidget(m_refresh);
    }
    m_sendPage->setObjectName(QStringLiteral("agentNotesSendPage"));
    m_sendPage->setIcon(QIcon::fromTheme(QStringLiteral("document-send")));
    m_sendPage->setToolTip(i18n("Send the page on screen in the reader to the agent, drawn on or as it is, with a comment"));
    m_sendPage->hide();
    m_markAllRead->setObjectName(QStringLiteral("agentNotesMarkAllRead"));
    m_refresh->setObjectName(QStringLiteral("agentNotesRefresh"));
    m_refresh->setToolTip(refreshTip);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("agentNotesList"));
    m_list->setItemDelegate(new NoteItemDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setUniformItemSizes(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_placeholder = new QLabel;
    m_placeholder->setObjectName(QStringLiteral("agentNotesPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setMargin(sidebar ? 16 : 32);
    setSecondary(m_placeholder);
    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("agentNoteScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(createDetail(m_scroll));
    m_scroll->installEventFilter(this);
    // Who listens for a reply: over the note, so it stays in view while the note scrolls.
    QWidget *answerLine = createAnswerLine(this);

    if (sidebar) {
        // The list, or one note in its place: a sidebar is too narrow for both.
        m_pages = new QStackedWidget(this);
        m_pages->setObjectName(QStringLiteral("agentNotesPages"));
        m_listPage = new QWidget(m_pages);
        m_listPage->setObjectName(QStringLiteral("agentNotesListPage"));
        auto *listLayout = new QVBoxLayout(m_listPage);
        listLayout->setContentsMargins(0, 0, 0, 0);
        listLayout->setSpacing(0);
        bar->setParent(m_listPage);
        listLayout->addWidget(bar);
        m_listStack = new QStackedWidget(m_listPage);
        m_listStack->addWidget(m_list);
        m_listStack->addWidget(m_placeholder);
        listLayout->addWidget(m_listStack, 1);
        m_pages->addWidget(m_listPage);

        m_notePage = new QWidget(m_pages);
        m_notePage->setObjectName(QStringLiteral("agentNotePage"));
        auto *noteLayout = new QVBoxLayout(m_notePage);
        noteLayout->setContentsMargins(0, 0, 0, 0);
        noteLayout->setSpacing(0);
        auto *nav = new QHBoxLayout;
        nav->setContentsMargins(4, 4, 6, 2);
        auto *back = new QToolButton(m_notePage);
        back->setObjectName(QStringLiteral("agentNoteBack"));
        back->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
        back->setText(i18n("All Notes"));
        back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        back->setAutoRaise(true);
        back->setToolTip(i18n("Back to the list of agent notes"));
        auto *large = new QToolButton(m_notePage);
        large->setObjectName(QStringLiteral("agentNoteOpenLarge"));
        large->setIcon(QIcon::fromTheme(QStringLiteral("view-fullscreen"), QIcon::fromTheme(QStringLiteral("window-new"))));
        large->setText(i18n("Open Large"));
        large->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        large->setAutoRaise(true);
        large->setToolTip(i18n("Show this note in the Agent Notes window, its picture large"));
        nav->addWidget(back);
        nav->addStretch(1);
        nav->addWidget(large);
        noteLayout->addLayout(nav);
        answerLine->setParent(m_notePage);
        noteLayout->addWidget(answerLine);
        m_scroll->setParent(m_notePage);
        noteLayout->addWidget(m_scroll, 1);
        m_pages->addWidget(m_notePage);
        outer->addWidget(m_pages, 1);

        connect(back, &QToolButton::clicked, this, &AgentNotesView::showList);
        connect(large, &QToolButton::clicked, this, &AgentNotesView::openLarge);
        m_image->setClickable(true);
        m_image->setToolTip(i18n("Click to see the picture large"));
        connect(m_image, &AgentNoteImageView::clicked, this, &AgentNotesView::openLarge);
        // A click opens the note; so do a double click and Return (which on macOS a list
        // keeps for editing, see eventFilter).
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { openInPlace(item->data(NoteIdRole).toString()); });
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { openInPlace(item->data(NoteIdRole).toString()); });
        m_list->installEventFilter(this);
    } else {
        outer->addWidget(bar);
        auto *splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setObjectName(QStringLiteral("agentNotesSplitter"));
        splitter->setChildrenCollapsible(false);
        m_list->setMinimumWidth(260);
        m_detailStack = new QStackedWidget(splitter);
        m_notePane = new QWidget(m_detailStack);
        m_notePane->setObjectName(QStringLiteral("agentNotePane"));
        auto *paneLayout = new QVBoxLayout(m_notePane);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        paneLayout->setSpacing(0);
        answerLine->setParent(m_notePane);
        paneLayout->addWidget(answerLine);
        m_scroll->setParent(m_notePane);
        paneLayout->addWidget(m_scroll, 1);
        m_detailStack->addWidget(m_placeholder);
        m_detailStack->addWidget(m_notePane);
        splitter->addWidget(m_list);
        splitter->addWidget(m_detailStack);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({360, 760});
        outer->addWidget(splitter, 1);
        connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) { currentChanged(current); });
    }

    connect(m_refresh, &QAbstractButton::clicked, this, [this] {
        if (m_center) m_center->refresh();
    });
    connect(m_markAllRead, &QAbstractButton::clicked, this, [this] {
        if (m_center) m_center->markAllSeen();
    });
    connect(m_openDocumentButton, &QPushButton::clicked, this, [this] {
        if (m_openDocument && !m_shownDocumentPath.isEmpty()) m_openDocument(m_shownDocumentId, m_shownDocumentPath);
    });
    connect(m_openCardButton, &QPushButton::clicked, this, [this] {
        if (m_openCard && !m_shownCardId.isEmpty()) m_openCard(m_shownCardId);
    });
    connect(m_openConversationButton, &QPushButton::clicked, this, [this] {
        if (m_shownConversation.isValid()) QDesktopServices::openUrl(m_shownConversation);
    });
    connect(m_sendPage, &QAbstractButton::clicked, this, &AgentNotesView::sendPage);
    connect(m_filterBox, &QComboBox::activated, this, &AgentNotesView::filterChosen);
    connect(m_replyField, &QPlainTextEdit::textChanged, this, [this] {
        if (m_settingReply || !m_center || m_shownNoteId.isEmpty()) return;
        m_center->setDraftMessage(m_shownNoteId, m_replyField->toPlainText());
    });
    connect(m_drawButton, &QPushButton::clicked, this, [this] { openComposer(m_shownNoteId); });
    const auto showSources = [this] {
        const QString value = m_center ? m_center->replySources() : AgentNotes::ReplySources::defaultValue();
        const QSignalBlocker blocker(m_sourcesBox);
        m_sourcesBox->setCurrentIndex(std::max(0, m_sourcesBox->findData(value)));
        m_sourcesBox->setToolTip(i18n("What the agent may use to answer: %1", AgentNotes::ReplySources::description(value)));
    };
    showSources();
    connect(m_sourcesBox, &QComboBox::activated, this, [this](int index) {
        if (m_center) m_center->setReplySources(m_sourcesBox->itemData(index).toString());
    });
    connect(m_sendReply, &QPushButton::clicked, this, [this] {
        if (m_center && !m_shownNoteId.isEmpty() && m_sendReply->isEnabled()) m_center->sendReply(m_shownNoteId);
    });
    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_replyField, nullptr, nullptr, Qt::WidgetShortcut);
    connect(sendShortcut, &QShortcut::activated, m_sendReply, &QPushButton::click);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &AgentNotesView::showListMenu);
    m_confirmDelete = [this](const QString &title) {
        return QMessageBox::question(this, i18n("Delete Agent Note"),
                                     i18n("Delete “%1”? It goes for every device: the phone and the agents no longer list it.", title),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
            == QMessageBox::Yes;
    };
    if (m_center) {
        connect(m_center, &AgentNotes::Center::draftChanged, this, [this](const QString &noteId) {
            if (noteId == m_shownNoteId) updateReplyBox();
        });
        connect(m_center, &AgentNotes::Center::notesChanged, this, &AgentNotesView::rebuildList);
        connect(m_center, &AgentNotes::Center::listeningChanged, this, [this](const QString &noteId) {
            if (noteId == m_shownNoteId) updateAnswerLine();
        });
        connect(m_center, &AgentNotes::Center::answerOptionsChanged, this, &AgentNotesView::updateAnswerLine);
        // The Agent Notes window and the reader sidebar show the same choice.
        connect(m_center, &AgentNotes::Center::replySourcesChanged, this, showSources);
        m_listeningTimer = new QTimer(this);
        m_listeningTimer->setObjectName(QStringLiteral("agentNoteListeningTimer"));
        m_listeningTimer->setInterval(ListeningRefreshMs);
        // Only while Thoughts is in front: behind another application nobody looks at the line.
        // Another view showing the same note may have asked a moment ago: one question a minute.
        const auto refreshAnswer = [this] {
            const QString id = answerNoteId();
            if (m_center && m_center->isForeground() && readsNow() && !id.isEmpty()) m_center->checkListeningIfOlder(id, ListeningRefreshMs - 5000);
        };
        connect(m_listeningTimer, &QTimer::timeout, this, refreshAnswer);
        connect(m_center, &AgentNotes::Center::foregroundChanged, this, [refreshAnswer](bool foreground) {
            if (foreground) refreshAnswer();
        });
        connect(m_center, &AgentNotes::Center::seenChanged, this, [this] {
            updateUnreadMarks();
            updateStatus();
        });
        connect(m_center, &AgentNotes::Center::stateChanged, this, &AgentNotesView::updateStatus);
    }
    rebuildList();
    updateStatus();
}

QWidget *AgentNotesView::createDetail(QWidget *parent)
{
    const bool sidebar = m_layout == Layout::Sidebar;
    m_detail = new QWidget(parent);
    m_detail->setObjectName(QStringLiteral("agentNoteDetail"));
    auto *detailLayout = new QVBoxLayout(m_detail);
    if (sidebar) detailLayout->setContentsMargins(12, 6, 12, 16);
    else detailLayout->setContentsMargins(24, 18, 24, 24);
    detailLayout->setSpacing(sidebar ? 8 : 10);

    m_title = wrappingLabel(QStringLiteral("agentNoteTitle"), m_detail);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * (sidebar ? 1.15 : 1.35));
    titleFont.setWeight(QFont::Bold);
    m_title->setFont(titleFont);
    m_meta = wrappingLabel(QStringLiteral("agentNoteMeta"), m_detail);
    setSecondary(m_meta);

    // Side by side in the window; one under the other in a sidebar.
    auto *links = new QBoxLayout(sidebar ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    links->setSpacing(sidebar ? 4 : 8);
    m_openDocumentButton = new QPushButton(QIcon::fromTheme(QStringLiteral("application-pdf")), QString(), m_detail);
    m_openDocumentButton->setObjectName(QStringLiteral("agentNoteOpenDocument"));
    m_openCardButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-task")), i18n("Show Card"), m_detail);
    m_openCardButton->setObjectName(QStringLiteral("agentNoteOpenCard"));
    m_openConversationButton = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-services")), i18n("Open Conversation"), m_detail);
    m_openConversationButton->setObjectName(QStringLiteral("agentNoteOpenConversation"));
    const Qt::Alignment linkAlignment = sidebar ? Qt::AlignLeft : Qt::Alignment();
    links->addWidget(m_openDocumentButton, 0, linkAlignment);
    links->addWidget(m_openCardButton, 0, linkAlignment);
    links->addWidget(m_openConversationButton, 0, linkAlignment);
    if (!sidebar) links->addStretch(1);

    m_question = wrappingLabel(QStringLiteral("agentNoteQuestion"), m_detail);
    QFont questionFont = m_question->font();
    questionFont.setItalic(true);
    m_question->setFont(questionFont);
    setSecondary(m_question);
    m_message = wrappingLabel(QStringLiteral("agentNoteMessage"), m_detail);
    if (!sidebar) {
        QFont messageFont = m_message->font();
        messageFont.setPointSizeF(messageFont.pointSizeF() * 1.1);
        m_message->setFont(messageFont);
    }
    m_image = new AgentNoteImageView(m_detail);
    m_image->setObjectName(QStringLiteral("agentNoteImage"));
    connect(m_image, &AgentNoteImageView::linkActivated, this, &AgentNotesView::openLink);

    // Reply, as on the phone: a comment, a drawing on the picture, or both.
    m_replyBox = new QWidget(m_detail);
    m_replyBox->setObjectName(QStringLiteral("agentNoteReplyBox"));
    auto *replyLayout = new QVBoxLayout(m_replyBox);
    replyLayout->setContentsMargins(0, 0, 0, 0);
    replyLayout->setSpacing(6);
    m_replyField = new QPlainTextEdit(m_replyBox);
    m_replyField->setObjectName(QStringLiteral("agentNoteReplyField"));
    m_replyField->setPlaceholderText(i18n("Reply to the agent…"));
    m_replyField->setTabChangesFocus(true);
    m_replyField->setFixedHeight(m_replyField->fontMetrics().lineSpacing() * (sidebar ? 4 : 3) + 14);
    replyLayout->addWidget(m_replyField);
    auto *replyButtons = new QBoxLayout(sidebar ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    replyButtons->setSpacing(6);
    m_drawButton = new QPushButton(QIcon::fromTheme(QStringLiteral("draw-freehand")), i18n("Draw on the Picture…"), m_replyBox);
    m_drawButton->setObjectName(QStringLiteral("agentNoteDraw"));
    m_drawButton->setToolTip(i18n("Draw on the note's picture with the pen, highlighter, eraser and numbered markers, then send it"));
    m_replyStatus = new QLabel(m_replyBox);
    m_replyStatus->setObjectName(QStringLiteral("agentNoteReplyStatus"));
    m_replyStatus->setWordWrap(true);
    setSecondary(m_replyStatus);
    m_sendReply = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), i18n("Send Reply"), m_replyBox);
    m_sendReply->setObjectName(QStringLiteral("agentNoteSend"));
    m_sendReply->setToolTip(i18n("Send the comment (and the drawing) to the agent (%1)", QKeySequence(Qt::CTRL | Qt::Key_Return).toString(QKeySequence::NativeText)));
    // What the answering agent may use: kept for every reply until changed.
    m_sourcesBox = new QComboBox(m_replyBox);
    m_sourcesBox->setObjectName(QStringLiteral("agentNoteReplySources"));
    // In the reader's narrow sidebar the box shrinks with it (its texts are cut, the list shows them whole).
    m_sourcesBox->setSizeAdjustPolicy(sidebar ? QComboBox::AdjustToMinimumContentsLengthWithIcon : QComboBox::AdjustToContents);
    if (sidebar) {
        m_sourcesBox->setMinimumContentsLength(4);
        m_sourcesBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    for (const QString &value : AgentNotes::ReplySources::all()) {
        m_sourcesBox->addItem(AgentNotes::ReplySources::label(value), value);
        m_sourcesBox->setItemData(m_sourcesBox->count() - 1, AgentNotes::ReplySources::description(value), Qt::ToolTipRole);
    }
    m_sourcesBox->setAccessibleName(i18n("What the agent may use to answer"));
    auto *sourcesLabel = new QLabel(i18nc("what the agent may use to answer a reply", "Sources:"), m_replyBox);
    sourcesLabel->setBuddy(m_sourcesBox);
    if (sidebar) {
        auto *sourcesRow = new QHBoxLayout;
        sourcesRow->addWidget(sourcesLabel);
        sourcesRow->addWidget(m_sourcesBox, 1);
        replyButtons->addLayout(sourcesRow);
        auto *row = new QHBoxLayout;
        row->addWidget(m_drawButton);
        row->addStretch(1);
        row->addWidget(m_sendReply);
        replyButtons->addLayout(row);
        replyButtons->addWidget(m_replyStatus);
    } else {
        replyButtons->addWidget(m_drawButton);
        replyButtons->addWidget(m_replyStatus, 1);
        replyButtons->addWidget(sourcesLabel);
        replyButtons->addWidget(m_sourcesBox);
        replyButtons->addWidget(m_sendReply);
    }
    replyLayout->addLayout(replyButtons);

    m_repliesHeader = new QLabel(m_detail);
    m_repliesHeader->setObjectName(QStringLiteral("agentNoteRepliesHeader"));
    QFont headerFont = m_repliesHeader->font();
    headerFont.setWeight(QFont::Bold);
    headerFont.setPointSizeF(headerFont.pointSizeF() * (sidebar ? 1.0 : 1.15));
    m_repliesHeader->setFont(headerFont);
    m_replies = new QWidget(m_detail);
    m_replies->setObjectName(QStringLiteral("agentNoteReplies"));
    m_repliesLayout = new QVBoxLayout(m_replies);
    m_repliesLayout->setContentsMargins(0, 0, 0, 0);
    m_repliesLayout->setSpacing(10);

    detailLayout->addWidget(m_title);
    detailLayout->addWidget(m_meta);
    detailLayout->addLayout(links);
    detailLayout->addWidget(m_question);
    detailLayout->addWidget(m_message);
    detailLayout->addSpacing(4);
    detailLayout->addWidget(m_image);
    detailLayout->addSpacing(4);
    detailLayout->addWidget(m_replyBox);
    detailLayout->addSpacing(sidebar ? 8 : 12);
    detailLayout->addWidget(m_repliesHeader);
    detailLayout->addWidget(m_replies);
    detailLayout->addStretch(1);
    return m_detail;
}

QWidget *AgentNotesView::createAnswerLine(QWidget *parent)
{
    const bool sidebar = m_layout == Layout::Sidebar;
    m_answerLine = new QWidget(parent);
    m_answerLine->setObjectName(QStringLiteral("agentNoteAnswerLine"));
    auto *outer = new QVBoxLayout(m_answerLine);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *row = new QHBoxLayout;
    if (sidebar) row->setContentsMargins(12, 5, 8, 5);
    else row->setContentsMargins(24, 7, 20, 7);
    row->setSpacing(8);
    m_answerMark = new QLabel(m_answerLine);
    m_answerMark->setObjectName(QStringLiteral("agentNoteAnswerMark"));
    m_answerMark->setFixedSize(16, 16);
    m_answerText = wrappingLabel(QStringLiteral("agentNoteAnswerText"), m_answerLine);
    m_answerText->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *change = new QToolButton(m_answerLine);
    change->setObjectName(QStringLiteral("agentNoteAnswerChange"));
    change->setText(i18nc("@action:button choose who answers a reply", "Change"));
    change->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_answerChange = change;
    row->addWidget(m_answerMark, 0, Qt::AlignVCenter);
    row->addWidget(m_answerText, 1);
    row->addWidget(m_answerChange, 0, Qt::AlignVCenter);
    outer->addLayout(row);
    // After a choice the server did not save: what happened, for a while.
    m_answerFailure = wrappingLabel(QStringLiteral("agentNoteAnswerFailure"), m_answerLine);
    m_answerFailure->setContentsMargins(sidebar ? 12 + 16 + 8 : 24 + 16 + 8, 0, 8, 6);
    m_answerFailure->hide();
    outer->addWidget(m_answerFailure);
    auto *rule = new QFrame(m_answerLine);
    rule->setObjectName(QStringLiteral("agentNoteAnswerRule"));
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    {
        QPalette palette = rule->palette();
        QColor line = palette.color(QPalette::WindowText);
        line.setAlphaF(0.12);
        palette.setColor(QPalette::WindowText, line);
        rule->setPalette(palette);
    }
    outer->addWidget(rule);
    m_answerFailureTimer = new QTimer(this);
    m_answerFailureTimer->setSingleShot(true);
    m_answerFailureTimer->setInterval(AnswerFailureShownMs);
    connect(m_answerFailureTimer, &QTimer::timeout, this, [this] {
        m_answerFailureNoteId.clear();
        updateAnswerLine();
    });
    connect(m_answerChange, &QToolButton::clicked, this, &AgentNotesView::showAnswerMenu);
    m_answerLine->hide();
    return m_answerLine;
}

AgentNotesView::~AgentNotesView()
{
    if (m_center) m_center->setWatching(this, false);
}

QString AgentNotesView::currentNoteId() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(NoteIdRole).toString() : QString();
}

QString AgentNotesView::shownNoteId() const
{
    if (m_layout == Layout::Split) return currentNoteId();
    return m_pages->currentWidget() == m_notePage ? m_shownNoteId : QString();
}

void AgentNotesView::selectNote(const QString &noteId)
{
    m_requestedNoteId = noteId;
    rebuildList();
}

void AgentNotesView::showList()
{
    if (m_layout != Layout::Sidebar) return;
    m_requestedNoteId.clear();
    m_pages->setCurrentWidget(m_listPage);
    m_list->setFocus(Qt::OtherFocusReason);
}

void AgentNotesView::openLarge()
{
    const QString id = shownNoteId();
    if (m_openLarge && !id.isEmpty()) m_openLarge(id);
}

bool AgentNotesView::readsNow() const
{
    return isVisible() && !window()->isMinimized();
}

void AgentNotesView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_center) m_center->setWatching(this, true);
    // What is on screen now counts as read.
    const QString shown = shownNoteId();
    if (!shown.isEmpty() && m_center) m_center->noteShown(shown);
    // Back on screen: who listens may have changed meanwhile.
    const QString answered = answerNoteId();
    if (!answered.isEmpty() && m_center) m_center->checkListeningIfOlder(answered, ListeningRefreshMs - 5000);
    QTimer::singleShot(0, this, &AgentNotesView::fitImages);
}

void AgentNotesView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_center && !isVisible()) m_center->setWatching(this, false);
}

bool AgentNotesView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll && event->type() == QEvent::Resize) fitImages();
    if (watched == m_list && event->type() == QEvent::KeyPress && m_layout == Layout::Sidebar) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !currentNoteId().isEmpty()) {
            openInPlace(currentNoteId());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentNotesView::fitImages()
{
    // Measured without the scroll bar, always: a picture that makes the bar appear must not
    // then shrink and make it go away again.
    const QMargins margins = m_detail->layout()->contentsMargins();
    QScrollBar *scrollBar = m_scroll->verticalScrollBar();
    const int bar = qMax(m_scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scrollBar), scrollBar->sizeHint().width());
    const int available = m_scroll->width() - 2 * m_scroll->frameWidth() - bar - margins.left() - margins.right();
    m_image->fitWidth(available);
    for (AgentNoteImageView *view : m_replies->findChildren<AgentNoteImageView *>()) view->fitWidth(available);
}

void AgentNotesView::updateStatus()
{
    if (!m_center) return;
    QString text;
    const int unread = m_center->unreadCount();
    if (m_center->isRefreshing() && !m_center->hasListed()) {
        text = i18n("Loading agent notes…");
    } else if (!m_center->lastError().isEmpty()) {
        text = m_center->lastError();
    } else if (m_center->lastUpdated().isValid()) {
        const QString updated = i18n("Updated %1", AgentNotes::relativeTime(m_center->lastUpdated(), QDateTime::currentDateTimeUtc()));
        text = unread > 0 ? i18np("%2 · 1 unread", "%2 · %1 unread", unread, updated) : updated;
    }
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty() || m_layout == Layout::Split);
    m_markAllRead->setEnabled(unread > 0);
    // A sidebar's title names its choice in the reader's Sidebar menu.
    if (m_layout == Layout::Sidebar) {
        setWindowTitle(unread > 0 ? i18np("Agent Notes (1 unread)", "Agent Notes (%1 unread)", unread) : i18n("Agent Notes"));
    }
    if (m_list->count() == 0) {
        if (!m_center->hasListed()) {
            showPlaceholder(m_center->lastError().isEmpty() ? i18n("Loading agent notes…") : m_center->lastError());
        } else if (!m_filter.isEmpty()) {
            showPlaceholder(i18n("No agent notes for %1 yet.", m_filter.label));
        } else {
            showPlaceholder(i18n("No agent notes yet.\nWhen an agent sends a note to your phone, it appears here too."));
        }
    }
}

void AgentNotesView::showPlaceholder(const QString &text)
{
    m_placeholder->setText(text);
    if (m_layout == Layout::Split) {
        m_detailStack->setCurrentWidget(m_placeholder);
    } else {
        m_listStack->setCurrentWidget(m_placeholder);
        m_pages->setCurrentWidget(m_listPage);
    }
    m_shownNoteId.clear();
}

void AgentNotesView::rebuildList()
{
    if (!m_center) return;
    const bool sidebar = m_layout == Layout::Sidebar;
    const QString keep = !m_requestedNoteId.isEmpty() ? m_requestedNoteId : currentNoteId();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QList<Note> notes = m_center->notes();
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const Note &note : notes) {
            if (!m_filter.matches(note)) continue;
            auto *item = new QListWidgetItem(note.title.isEmpty() ? i18n("(untitled note)") : note.title, m_list);
            item->setData(NoteIdRole, note.id);
            item->setData(UnreadRole, m_center->isUnread(note));
            QStringList meta{AgentNotes::relativeTime(note.createdAt, now), note.senderLabel()};
            if (!note.replies.isEmpty()) meta.append(i18np("1 reply", "%1 replies", note.replies.size()));
            else if (!note.isPhonePage()) meta.append(i18n("not answered yet"));
            item->setData(MetaRole, meta.join(QStringLiteral(" · ")));
            item->setData(PreviewRole, firstLine(!note.message.isEmpty() ? note.message : note.reviewQuestion));
            item->setToolTip(note.title);
        }
    }
    rebuildFilterMenu();
    if (sidebar && m_list->count() > 0) m_listStack->setCurrentWidget(m_list);
    QListWidgetItem *select = nullptr;
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(NoteIdRole).toString() == keep) {
            select = m_list->item(row);
            break;
        }
    }
    const bool requested = select && !m_requestedNoteId.isEmpty();
    if (select || keep.isEmpty() || m_center->hasListed()) m_requestedNoteId.clear();
    if (sidebar) {
        // The list keeps its place; a note opens only when asked for.
        if (select) {
            const QSignalBlocker blocker(m_list);
            m_list->setCurrentItem(select);
            m_list->scrollToItem(select);
        }
        if (requested) {
            openInPlace(select->data(NoteIdRole).toString());
        } else if (m_pages->currentWidget() == m_notePage) {
            // The note shown, brought up to date; gone from the server, back to the list.
            const auto shown = m_center->note(m_shownNoteId);
            if (shown) showDetail(*shown, true);
            else showList();
        }
        updateStatus();
        return;
    }
    if (!select && m_list->count() > 0) select = m_list->item(0);
    if (select) {
        const QSignalBlocker blocker(m_list);
        m_list->setCurrentItem(select);
        m_list->scrollToItem(select);
    }
    currentChanged(select);
    updateStatus();
}

void AgentNotesView::updateUnreadMarks()
{
    if (!m_center) return;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (const auto note = m_center->note(item->data(NoteIdRole).toString())) item->setData(UnreadRole, m_center->isUnread(*note));
    }
}

void AgentNotesView::currentChanged(QListWidgetItem *current)
{
    if (!current || !m_center) {
        if (m_list->count() == 0) updateStatus();
        return;
    }
    const auto note = m_center->note(current->data(NoteIdRole).toString());
    if (!note) return;
    showDetail(*note, note->id == m_shownNoteId);
    markSeenSoon(*note);
}

void AgentNotesView::openInPlace(const QString &noteId)
{
    if (!m_center || m_layout != Layout::Sidebar) return;
    const auto note = m_center->note(noteId);
    if (!note) return;
    const bool sameNote = note->id == m_shownNoteId && m_pages->currentWidget() == m_notePage;
    m_pages->setCurrentWidget(m_notePage);
    showDetail(*note, sameNote);
    markSeenSoon(*note);
}

void AgentNotesView::openLink(const QString &href)
{
    if (m_openLink) {
        m_openLink(href);
        return;
    }
    // Without the Shell (tests): a web page in the browser.
    const QUrl url(href);
    if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) QDesktopServices::openUrl(url);
}

void AgentNotesView::markSeenSoon(const Note &note)
{
    // Unread here, or not opened anywhere yet: the server hears "opened" (Center::noteShown).
    if (!readsNow() || (!m_center->isUnread(note) && (note.openedAt.isValid() || note.isPhonePage()))) return;
    // Marking it seen updates every view of the notes: let this change finish first.
    QTimer::singleShot(0, this, [this, id = note.id] {
        if (m_center && shownNoteId() == id && readsNow()) m_center->noteShown(id);
    });
}

void AgentNotesView::showDetail(const Note &note, bool sameNote)
{
    if (m_layout == Layout::Split) m_detailStack->setCurrentWidget(m_notePane);
    m_shownNoteId = note.id;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    m_title->setText(note.title);
    QStringList meta{AgentNotes::relativeTime(note.createdAt, now), note.senderLabel()};
    if (!note.isPhonePage()) {
        // Opened on the phone or on this Mac (the server keeps the first time, not where).
        meta.append(note.openedAt.isValid() ? i18n("Opened %1", AgentNotes::relativeTime(note.openedAt, now)) : i18n("Not opened yet"));
    }
    m_meta->setText(meta.join(QStringLiteral(" · ")));

    m_shownDocumentId = note.documentId;
    m_shownDocumentPath.clear();
    const bool hasDocument = !note.documentId.isEmpty() || !note.documentFilename.isEmpty();
    m_openDocumentButton->setVisible(hasDocument);
    if (hasDocument) {
        const DocumentLink link = m_resolveDocument ? m_resolveDocument(note.documentId, note.documentFilename) : DocumentLink{};
        m_shownDocumentPath = link.path;
        QString title = !link.title.isEmpty() ? link.title : note.documentFilename;
        // A button wider than the sidebar would widen the note past it.
        const int limit = m_layout == Layout::Sidebar ? 26 : 60;
        if (title.size() > limit) title = title.left(limit - 1) + QChar(0x2026);
        m_openDocumentButton->setText(title.isEmpty() ? i18n("Open Document") : i18n("Open “%1”", title));
        m_openDocumentButton->setEnabled(!link.path.isEmpty());
        m_openDocumentButton->setToolTip(link.path.isEmpty() ? i18n("The document of this note is not in this computer's Library.") : link.path);
    }
    m_shownCardId = note.reviewCardId;
    m_openCardButton->setVisible(!note.reviewCardId.isEmpty());
    m_openCardButton->setToolTip(note.reviewQuestion.isEmpty() ? i18n("Show the review card in the Library") : note.reviewQuestion);
    m_shownConversation = note.conversationUrl();
    m_openConversationButton->setVisible(m_shownConversation.isValid());
    m_openConversationButton->setToolTip(m_shownConversation.toString());
    m_question->setText(note.reviewQuestion.isEmpty() ? QString() : i18n("Card: %1", note.reviewQuestion));
    m_question->setVisible(!note.reviewQuestion.isEmpty());
    m_message->setText(note.message);
    m_message->setVisible(!note.message.isEmpty());

    // A picture that failed to load is asked for again when the note is shown again.
    m_image->setLinks(note.links);
    if (!sameNote || m_image->sha256() != note.imageSha256 || m_image->failed()) {
        m_image->setSource(note.imageSha256, QSize(note.imageWidth, note.imageHeight));
        loadImage(m_image, note.imageUrl, note.imageSha256, note.mediaType);
    }
    rebuildReplies(note);
    // The user's own pages are answered by the agent with a note of its own; agent notes get a reply box.
    m_replyBox->setVisible(!note.isPhonePage());
    if (m_center && !note.isPhonePage()) {
        // Who listens for a reply: asked when the note is shown, then every minute while it is
        // (a view out of sight asks when it comes back, see showEvent).
        if (!sameNote && readsNow()) m_center->checkListening(note.id);
        if (m_listeningTimer && !m_listeningTimer->isActive()) m_listeningTimer->start();
    } else if (m_listeningTimer) {
        m_listeningTimer->stop();
    }
    updateAnswerLine();
    if (!sameNote) {
        const auto draft = m_center ? m_center->existingDraft(note.id) : nullptr;
        m_settingReply = true;
        m_replyField->setPlainText(draft ? draft->message : QString());
        m_settingReply = false;
    }
    updateReplyBox();
    fitImages();
    if (!sameNote) m_scroll->verticalScrollBar()->setValue(0);
}

QString AgentNotesView::answerNoteId() const
{
    const QString id = shownNoteId();
    if (id.isEmpty() || !m_center) return {};
    const auto note = m_center->note(id);
    return note && !note->isPhonePage() ? id : QString();
}

void AgentNotesView::updateAnswerLine()
{
    if (!m_answerLine) return;
    const QString id = answerNoteId();
    const auto line = id.isEmpty() ? std::nullopt : AgentNotes::answerLine(m_center->answer(id), m_center->canChangeAnswerer(), m_center->serverReachable());
    m_answerLine->setVisible(line.has_value());
    if (!line) return;
    const QPalette base = palette();
    const bool dark = base.color(QPalette::Window).lightnessF() < 0.5;
    // The warning look the bar over the reply field had: nobody will act on a reply.
    const QColor warningText = dark ? QColor(0xf2, 0xb8, 0xb5) : QColor(0xb3, 0x26, 0x1e);
    QPalette linePalette = base;
    if (line->warning) {
        QColor tint = warningText;
        tint.setAlphaF(dark ? 0.16 : 0.10);
        const QColor window = base.color(QPalette::Window);
        const qreal alpha = tint.alphaF();
        linePalette.setColor(QPalette::Window, QColor::fromRgbF(window.redF() * (1 - alpha) + tint.redF() * alpha, window.greenF() * (1 - alpha) + tint.greenF() * alpha,
                                                                 window.blueF() * (1 - alpha) + tint.blueF() * alpha));
    }
    m_answerLine->setAutoFillBackground(line->warning);
    m_answerLine->setPalette(linePalette);
    m_answerLine->setProperty("warning", line->warning);

    QPalette textPalette = base;
    if (line->warning) textPalette.setColor(QPalette::WindowText, warningText);
    else if (line->muted || line->mark == AgentNotes::AnswerLine::Mark::Waiting) textPalette.setColor(QPalette::WindowText, secondaryText(base));
    m_answerText->setPalette(textPalette);
    m_answerText->setText(line->text);
    const auto note = m_center->note(id);
    if (line->mark == AgentNotes::AnswerLine::Mark::Nobody) {
        m_answerText->setToolTip(note && note->conversationUrl().isValid()
                                     ? i18n("A reply is kept with the note, but no agent acts on it until one listens. Open Conversation continues the conversation that sent it.")
                                     : i18n("A reply is kept with the note, but no agent acts on it until one listens."));
    } else if (line->mark == AgentNotes::AnswerLine::Mark::Waiting) {
        m_answerText->setToolTip(i18n("You chose this agent to answer your reply. While it does not listen, the Thoughts reply agent answers after 10 minutes."));
    } else if (line->mark == AgentNotes::AnswerLine::Mark::Woken) {
        m_answerText->setToolTip(i18n("A reply waker wakes this session with your reply, or gives it your reply when its current task is done."));
    } else if (line->mark == AgentNotes::AnswerLine::Mark::Offline) {
        m_answerText->setToolTip(i18n("The session that should answer is on a machine that is offline. It answers when the machine is back; if it stays offline, the study session answers instead."));
    } else {
        m_answerText->setToolTip(i18n("Your reply goes to this agent now."));
    }

    using Mark = AgentNotes::AnswerLine::Mark;
    QIcon icon;
    if (line->mark == Mark::Listening || line->mark == Mark::Woken) {
        icon = QIcon::fromTheme(QStringLiteral("checkmark"), style()->standardIcon(QStyle::SP_DialogApplyButton));
    } else if (line->mark == Mark::Offline) {
        icon = QIcon::fromTheme(QStringLiteral("chronometer"), style()->standardIcon(QStyle::SP_MessageBoxInformation));
    } else {
        icon = QIcon::fromTheme(QStringLiteral("dialog-warning"), style()->standardIcon(QStyle::SP_MessageBoxWarning));
    }
    m_answerMark->setPixmap(icon.pixmap(QSize(16, 16), devicePixelRatioF()));
    QString mark;
    QString markName;
    switch (line->mark) {
    case Mark::Listening:
        mark = QStringLiteral("listening");
        markName = i18nc("@info the state of the agent that answers", "Listening");
        break;
    case Mark::Woken:
        mark = QStringLiteral("woken");
        markName = i18nc("@info the state of the agent that answers", "Will be woken");
        break;
    case Mark::Offline:
        mark = QStringLiteral("offline");
        markName = i18nc("@info the state of the agent that answers", "Offline");
        break;
    case Mark::Waiting:
        mark = QStringLiteral("waiting");
        markName = i18nc("@info the state of the agent that answers", "Not listening");
        break;
    case Mark::Nobody:
        mark = QStringLiteral("nobody");
        markName = i18nc("@info the state of the agent that answers", "Not listening");
        break;
    }
    m_answerMark->setProperty("mark", mark);
    m_answerMark->setAccessibleName(markName);
    m_answerMark->setToolTip(markName);

    const bool choosing = m_center->isChoosingAnswerer(id);
    m_answerChange->setVisible(line->changeOffered);
    m_answerChange->setEnabled(line->changeEnabled && !choosing);
    m_answerChange->setToolTip(choosing                ? i18n("Sending your choice to the server…")
                                   : line->changeEnabled ? i18n("Choose which agent answers your reply")
                                                         : i18n("Change (needs the server)"));

    const bool failed = !m_answerFailureNoteId.isEmpty() && m_answerFailureNoteId == id;
    if (failed) {
        QPalette failurePalette = base;
        failurePalette.setColor(QPalette::WindowText, warningText);
        m_answerFailure->setPalette(failurePalette);
    }
    m_answerFailure->setVisible(failed);
}

QMenu *AgentNotesView::createAnswerMenu()
{
    const QString id = answerNoteId();
    const auto answer = id.isEmpty() ? std::nullopt : m_center->answer(id);
    if (!answer) return nullptr;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentNoteAnswerMenu"));
    QAction *heading = menu->addAction(i18n("Who answers your reply"));
    heading->setEnabled(false);
    menu->addSeparator();
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    QFont detailFont = menu->font();
    detailFont.setPointSizeF(detailFont.pointSizeF() * 0.85);
    for (const AgentNotes::AnswerChoice &choice : AgentNotes::answerChoices(*answer)) {
        QAction *action = menu->addAction(choice.title);
        action->setObjectName(choice.agent ? QStringLiteral("agentNoteAnswerChoice") : QStringLiteral("agentNoteAnswerAutomatic"));
        action->setCheckable(true);
        action->setChecked(choice.checked);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id, choice] {
            if (!choice.checked) chooseAnswerer(id, choice.agent);
        });
        if (!choice.detail.isEmpty()) {
            // The line under Automatic: who answers when nobody was chosen.
            action->setToolTip(choice.detail);
            QAction *detail = menu->addAction(choice.detail);
            detail->setObjectName(QStringLiteral("agentNoteAnswerDetail"));
            detail->setEnabled(false);
            detail->setFont(detailFont);
        }
    }
    return menu;
}

void AgentNotesView::showAnswerMenu()
{
    QMenu *menu = createAnswerMenu();
    if (!menu) return;
    const QPointer<QMenu> guard(menu);
    menu->exec(m_answerChange->mapToGlobal(QPoint(0, m_answerChange->height())));
    if (guard) guard->deleteLater();
}

void AgentNotesView::chooseAnswerer(const QString &noteId, const std::optional<AgentNotes::Agent> &agent)
{
    if (!m_center || noteId.isEmpty()) return;
    if (m_answerFailureNoteId == noteId) {
        m_answerFailureNoteId.clear();
        m_answerFailureTimer->stop();
    }
    const QPointer<AgentNotesView> guard(this);
    m_center->chooseAnswerer(noteId, agent, [guard, noteId](bool ok, const QString &error) {
        if (guard && !ok) guard->showAnswerFailure(noteId, error);
    });
    updateAnswerLine();
}

void AgentNotesView::showAnswerFailure(const QString &noteId, const QString &text)
{
    m_answerFailureNoteId = noteId;
    m_answerFailure->setText(text);
    m_answerFailureTimer->start();
    updateAnswerLine();
}

void AgentNotesView::updateReplyBox()
{
    if (!m_center || m_shownNoteId.isEmpty()) return;
    const auto draft = m_center->existingDraft(m_shownNoteId);
    const QString message = draft ? draft->message : QString();
    // The other view (the window or a sidebar) may have changed it; no draft: it was sent.
    if (m_replyField->toPlainText() != message && (!draft || !m_replyField->hasFocus())) {
        m_settingReply = true;
        m_replyField->setPlainText(message);
        m_settingReply = false;
    }
    const bool drawn = draft && !draft->drawing.isNull() && !draft->drawing.isEmpty();
    const bool sending = draft && draft->sending;
    m_drawButton->setText(drawn ? i18n("Edit the Drawing…") : i18n("Draw on the Picture…"));
    QString status;
    if (sending) status = i18n("Sending…");
    else if (draft && !draft->error.isEmpty()) status = i18n("Not sent: %1", draft->error);
    else if (drawn) status = i18np("With your drawing (1 mark).", "With your drawing (%1 marks).", int(draft->drawing.strokes().size() + draft->drawing.markers().size()));
    // The last reply went out: who a reply waker wakes for it ("Sent ✓ · … will be woken").
    else if (!draft) status = m_center->sentStatus(m_shownNoteId);
    m_replyStatus->setText(status);
    m_replyStatus->setVisible(!status.isEmpty());
    m_replyField->setReadOnly(sending);
    m_sendReply->setEnabled(!sending && (drawn || !message.trimmed().isEmpty()));
}

void AgentNotesView::setLibraryProvider(LibraryProvider provider)
{
    m_libraryProvider = std::move(provider);
    rebuildFilterMenu();
}

int AgentNotesView::listedCount() const
{
    return m_list->count();
}

void AgentNotesView::setFilter(const AgentNotes::Filter &filter)
{
    m_filter = filter;
    if (m_center && !filter.documentIds.isEmpty()) m_center->fetchDocuments(filter.documentIds);
    rebuildList();
}

void AgentNotesView::filterChosen(int index)
{
    if (m_settingFilterBox || index < 0) return;
    const QString data = m_filterBox->itemData(index).toString();
    if (data.isEmpty()) {
        setFilter({});
        return;
    }
    const QList<AgentNotes::LibraryDocument> documents = m_libraryProvider ? m_libraryProvider() : QList<AgentNotes::LibraryDocument>();
    const QString kind = data.section(QLatin1Char(':'), 0, 0);
    const QString key = data.section(QLatin1Char(':'), 1);
    std::optional<AgentNotes::Filter> chosen = kind == QLatin1String("project") ? AgentNotes::Filter::project(documents, key) : AgentNotes::Filter::document(documents, key);
    if (!chosen && kind == QLatin1String("document")) {
        // A document the catalogue does not list (yet): its notes by their document_id.
        AgentNotes::Filter raw;
        raw.kind = kind;
        raw.key = key;
        raw.label = m_filterBox->itemData(index, Qt::UserRole + 1).toString();
        raw.documentIds = {key};
        chosen = raw;
    }
    if (chosen && !(*chosen == m_filter)) setFilter(*chosen);
}

void AgentNotesView::rebuildFilterMenu()
{
    if (!m_center || !m_filterBox) return;
    const QList<AgentNotes::LibraryDocument> documents = m_libraryProvider ? m_libraryProvider() : QList<AgentNotes::LibraryDocument>();
    const QList<Note> notes = m_center->notes();
    // Notes per document, then per project, in the catalogue's order.
    QHash<QString, int> perDocument;
    QHash<QString, QString> documentLabel;
    for (const Note &note : notes) {
        const QString key = !note.documentId.isEmpty() ? note.documentId : note.documentFilename;
        if (key.isEmpty()) continue;
        ++perDocument[key];
        if (!documentLabel.contains(key)) documentLabel.insert(key, !note.documentFilename.isEmpty() ? note.documentFilename : key.left(8));
    }
    struct Entry {
        QString text;
        QString data;
        QString label;
    };
    QList<Entry> entries{{i18n("All Notes (%1)", notes.size()), QString(), QString()}};
    QSet<QString> listed;
    QStringList projectOrder;
    QHash<QString, QList<AgentNotes::LibraryDocument>> byProject;
    for (const AgentNotes::LibraryDocument &document : documents) {
        if (!byProject.contains(document.projectId)) projectOrder.append(document.projectId);
        byProject[document.projectId].append(document);
    }
    for (const QString &projectId : std::as_const(projectOrder)) {
        const auto &projectDocuments = byProject.value(projectId);
        int count = 0;
        QList<std::pair<AgentNotes::LibraryDocument, int>> withNotes;
        for (const AgentNotes::LibraryDocument &document : projectDocuments) {
            const int forDocument = perDocument.value(document.id) + (document.filename.isEmpty() ? 0 : perDocument.value(document.filename));
            for (const QString &key : {document.id, document.filename}) listed.insert(key);
            if (forDocument == 0) continue;
            count += forDocument;
            withNotes.append({document, forDocument});
        }
        const bool current = m_filter.kind == QLatin1String("project") && m_filter.key == projectId;
        if (count == 0 && !current) continue;
        const QString projectName = projectDocuments.first().projectName.isEmpty() ? projectId : projectDocuments.first().projectName;
        entries.append({QStringLiteral("%1 (%2)").arg(projectName).arg(count), QStringLiteral("project:") + projectId, projectName});
        // Its documents too, when the notes are spread over more than one.
        if (withNotes.size() > 1) {
            for (const auto &[document, forDocument] : std::as_const(withNotes)) {
                const QString name = !document.name.isEmpty() ? document.name : document.filename;
                entries.append({QStringLiteral("    %1 (%2)").arg(name).arg(forDocument), QStringLiteral("document:") + (!document.id.isEmpty() ? document.id : document.filename),
                                QStringLiteral("%1 › %2").arg(projectName, name)});
            }
        }
    }
    // Documents the catalogue does not list.
    for (auto it = perDocument.cbegin(); it != perDocument.cend(); ++it) {
        if (listed.contains(it.key())) continue;
        entries.append({QStringLiteral("%1 (%2)").arg(documentLabel.value(it.key())).arg(it.value()), QStringLiteral("document:") + it.key(), documentLabel.value(it.key())});
    }
    const QString currentData = m_filter.isEmpty() ? QString() : m_filter.kind + QLatin1Char(':') + m_filter.key;
    if (!currentData.isEmpty() && std::none_of(entries.cbegin(), entries.cend(), [&](const Entry &entry) { return entry.data == currentData; })) {
        const int count = int(std::count_if(notes.cbegin(), notes.cend(), [this](const Note &note) { return m_filter.matches(note); }));
        entries.append({QStringLiteral("%1 (%2)").arg(m_filter.label).arg(count), currentData, m_filter.label});
    }
    m_settingFilterBox = true;
    m_filterBox->clear();
    for (const Entry &entry : std::as_const(entries)) {
        m_filterBox->addItem(entry.text, entry.data);
        m_filterBox->setItemData(m_filterBox->count() - 1, entry.label, Qt::UserRole + 1);
        if (entry.data == currentData) m_filterBox->setCurrentIndex(m_filterBox->count() - 1);
    }
    m_settingFilterBox = false;
}

void AgentNotesView::setPageProvider(PageProvider provider)
{
    m_pageProvider = std::move(provider);
    m_sendPage->setVisible(bool(m_pageProvider));
}

void AgentNotesView::openComposer(const QString &noteId)
{
    if (!m_center || noteId.isEmpty()) return;
    if (QDialog *open = m_composers.value(noteId)) {
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }
    auto *composer = new AgentNoteComposer(m_center, noteId, window());
    composer->setAttribute(Qt::WA_DeleteOnClose);
    m_composers.insert(noteId, composer);
    composer->show();
}

void AgentNotesView::sendPage()
{
    if (!m_center || !m_pageProvider) return;
    const auto capture = m_pageProvider();
    if (!capture) {
        QMessageBox::information(this, i18n("Send Page to Agent"), i18n("Open a PDF in the reader first: its page on screen is what goes to the agent."));
        return;
    }
    // As wide as the phone sends a page: small print stays readable, the upload quick.
    const QImage image = capture->render ? capture->render(1600) : QImage();
    if (image.isNull()) {
        QMessageBox::warning(this, i18n("Send Page to Agent"), i18n("The page could not be drawn."));
        return;
    }
    AgentNotes::Center::Page page;
    page.title = capture->page > 0 ? i18nc("@title document, page number", "%1 · page %2", capture->documentName, capture->page) : capture->documentName;
    page.documentId = capture->documentId;
    page.documentFilename = capture->documentFilename;
    page.documentName = capture->documentName;
    page.reviewCardId = capture->reviewCardId;
    page.page = capture->page;
    page.pages = capture->pages;
    page.image = image;
    auto *composer = new AgentNoteComposer(m_center, page, window());
    composer->setAttribute(Qt::WA_DeleteOnClose);
    composer->show();
}

void AgentNotesView::deleteNote(const QString &noteId)
{
    const auto note = m_center ? m_center->note(noteId) : std::nullopt;
    if (!note) return;
    if (m_confirmDelete && !m_confirmDelete(note->title.isEmpty() ? i18n("(untitled note)") : note->title)) return;
    QPointer<AgentNotesView> guard(this);
    m_center->deleteNote(noteId, [guard](bool ok, const QString &error) {
        if (!guard || ok) return;
        QMessageBox::warning(guard, i18n("Delete Agent Note"), i18n("The note was not deleted: %1", error));
    });
}

void AgentNotesView::showListMenu(const QPoint &position)
{
    QListWidgetItem *item = m_list->itemAt(position);
    const auto note = item && m_center ? m_center->note(item->data(NoteIdRole).toString()) : std::nullopt;
    if (!note) return;
    QMenu menu(this);
    const QUrl conversation = note->conversationUrl();
    if (conversation.isValid()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("internet-services")), i18n("Open Conversation"), this, [conversation] { QDesktopServices::openUrl(conversation); });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Conversation Link"), this,
                       [conversation] { QApplication::clipboard()->setText(conversation.toString()); });
        menu.addSeparator();
    }
    if (!note->isPhonePage()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("draw-freehand")), i18n("Reply with a Drawing…"), this, [this, id = note->id] { openComposer(id); });
        menu.addSeparator();
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Note…"), this, [this, id = note->id] { deleteNote(id); });
    menu.exec(m_list->viewport()->mapToGlobal(position));
}

void AgentNotesView::rebuildReplies(const Note &note)
{
    // Keep the pictures already shown when the same replies come back from a refresh.
    QHash<QString, QImage> shown;
    for (AgentNoteImageView *view : m_replies->findChildren<AgentNoteImageView *>()) {
        if (!view->image().isNull()) shown.insert(view->sha256(), view->image());
    }
    while (QLayoutItem *item = m_repliesLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_repliesHeader->setVisible(!note.replies.isEmpty());
    m_replies->setVisible(!note.replies.isEmpty());
    if (note.replies.isEmpty()) return;
    m_repliesHeader->setText(note.isPhonePage() ? i18np("Reply", "Replies (%1)", note.replies.size())
                                                : i18np("Your reply", "Your replies (%1)", note.replies.size()));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const Reply &reply : note.replies) {
        auto *box = new QWidget(m_replies);
        box->setObjectName(QStringLiteral("agentNoteReply"));
        box->setProperty("replyId", reply.id);
        auto *layout = new QVBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *meta = wrappingLabel(QStringLiteral("agentNoteReplyMeta"), box);
        setSecondary(meta);
        QStringList parts{AgentNotes::relativeTime(reply.createdAt, now), reply.deviceId.isEmpty() ? i18n("from the phone") : i18n("from %1", reply.deviceId)};
        if (reply.strokeCount > 0) parts.append(i18np("1 stroke drawn", "%1 strokes drawn", reply.strokeCount));
        if (reply.hasPhoto()) parts.append(i18n("with a photo from the glasses"));
        // What the agent could use, when the reply narrowed it.
        if (!reply.sources.isEmpty() && reply.sources != AgentNotes::ReplySources::defaultValue()
            && !AgentNotes::ReplySources::label(reply.sources).isEmpty())
            parts.append(AgentNotes::ReplySources::label(reply.sources));
        meta->setText(parts.join(QStringLiteral(" · ")));
        layout->addWidget(meta);
        auto *message = wrappingLabel(QStringLiteral("agentNoteReplyMessage"), box);
        QFont messageFont = message->font();
        messageFont.setPointSizeF(messageFont.pointSizeF() * 1.1);
        message->setFont(messageFont);
        message->setText(!reply.message.isEmpty() ? reply.message : reply.strokeCount > 0 ? QString() : i18n("(sent without a drawing or a comment)"));
        message->setVisible(!message->text().isEmpty());
        layout->addWidget(message);
        // Without strokes the reply's picture is the note's picture again.
        if (reply.strokeCount > 0) {
            auto *image = new AgentNoteImageView(box);
            image->setObjectName(QStringLiteral("agentNoteReplyImage"));
            image->setSource(reply.imageSha256, QSize(reply.imageWidth, reply.imageHeight));
            // The note's picture drawn on: its links stay where they were.
            if (reply.imageWidth == note.imageWidth && reply.imageHeight == note.imageHeight) {
                image->setLinks(note.links);
                connect(image, &AgentNoteImageView::linkActivated, this, &AgentNotesView::openLink);
            }
            if (shown.contains(reply.imageSha256)) {
                image->setImage(shown.value(reply.imageSha256));
                if (m_center) image->setCachedPath(m_center->client()->cachedImagePath(reply.imageSha256, reply.mediaType));
            } else {
                loadImage(image, reply.imageUrl, reply.imageSha256, reply.mediaType);
            }
            layout->addWidget(image);
        }
        // Send with picture: the photo the user's words are about.
        if (reply.hasPhoto()) {
            auto *photo = new AgentNoteImageView(box);
            photo->setObjectName(QStringLiteral("agentNoteReplyPhoto"));
            photo->setToolTip(reply.photoCapture.isEmpty() ? i18n("Photo from the glasses") : i18n("Photo from the glasses (%1)", reply.photoCapture));
            photo->setSource(reply.photoSha256, QSize(reply.photoWidth, reply.photoHeight));
            if (!reply.photoSha256.isEmpty() && shown.contains(reply.photoSha256)) {
                photo->setImage(shown.value(reply.photoSha256));
                if (m_center) photo->setCachedPath(m_center->client()->cachedImagePath(reply.photoSha256, reply.photoMediaType));
            } else {
                loadImage(photo, reply.photoUrl, reply.photoSha256, reply.photoMediaType);
            }
            layout->addWidget(photo);
        }
        m_repliesLayout->addWidget(box);
    }
}

void AgentNotesView::loadImage(AgentNoteImageView *view, const QString &url, const QString &sha256, const QString &mediaType)
{
    if (!m_center) return;
    QPointer<AgentNoteImageView> guard(view);
    AgentNotes::Client *client = m_center->client();
    client->image(url, sha256, mediaType, view, [guard, client, sha256, mediaType](const QImage &image, const QString &error) {
        if (!guard || guard->sha256() != sha256) return;
        if (image.isNull()) {
            guard->setFailure(error.isEmpty() ? i18n("The picture could not be loaded.") : error);
            return;
        }
        guard->setImage(image);
        guard->setCachedPath(client->cachedImagePath(sha256, mediaType));
    });
}

// ---------------------------------------------------------------- window

AgentNotesWindow::AgentNotesWindow(AgentNotes::Center *center, DocumentResolver resolveDocument, DocumentOpener openDocument, CardOpener openCard,
                                   QWidget *parent)
    : AgentNotesView(Layout::Split, center, std::move(resolveDocument), std::move(openDocument), std::move(openCard), parent, Qt::Window)
{
    setObjectName(QStringLiteral("agentNotesWindow"));
    resize(1120, 820);
    auto *refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
    connect(refreshShortcut, &QShortcut::activated, this, [guard = QPointer<AgentNotes::Center>(center)] {
        if (guard) guard->refresh();
    });
    auto *closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QWidget::close);
}

void AgentNotesWindow::showNote(const QString &noteId)
{
    selectNote(noteId);
    show();
    if (windowState().testFlag(Qt::WindowMinimized)) setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

}
