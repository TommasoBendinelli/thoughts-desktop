/*
    SPDX-FileCopyrightText: 2006 Chu Xiaodong <xiaodongchu@gmail.com>
    SPDX-FileCopyrightText: 2006 Pino Toscano <pino@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annotwindow.h"
#include "commentauthor.h"

#include <QSignalBlocker>

// qt/kde includes
#include <KLocalizedString>
#include <KStandardAction>
#include <KTextEdit>
#include <QAction>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSizeGrip>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

// local includes
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"
#include "gui/guiutils.h"
#include "latexrenderer.h"
#include "docscommentsynchronizer.h"
#include <KMessageBox>
#include <core/utils.h>

#include <algorithm>

static QDateTime annotationDisplayDate(const Okular::Annotation *annotation)
{
    const QDateTime creationDate = annotation->creationDate();
    return creationDate.isValid() ? creationDate : annotation->modificationDate();
}

static QList<Okular::Annotation *> annotationThread(Okular::Document *document, Okular::Annotation *selected)
{
    if (!document || !selected) {
        return {};
    }

    Okular::Annotation *root = selected;
    QSet<Okular::Annotation *> ancestors;
    while (root->previousAnnotation() && !ancestors.contains(root)) {
        ancestors.insert(root);
        root = root->previousAnnotation();
    }

    QSet<Okular::Annotation *> connected {root};
    bool added = true;
    while (added) {
        added = false;
        for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
            for (Okular::Annotation *annotation : document->page(pageNumber)->annotations()) {
                if (!annotation || connected.contains(annotation) || !connected.contains(annotation->previousAnnotation())) {
                    continue;
                }
                connected.insert(annotation);
                added = true;
            }
        }
    }

    QList<Okular::Annotation *> replies = connected.values();
    replies.removeAll(root);
    std::sort(replies.begin(), replies.end(), [](const Okular::Annotation *left, const Okular::Annotation *right) {
        const QDateTime leftDate = annotationDisplayDate(left);
        const QDateTime rightDate = annotationDisplayDate(right);
        if (leftDate != rightDate) {
            if (!leftDate.isValid()) {
                return false;
            }
            if (!rightDate.isValid()) {
                return true;
            }
            return leftDate < rightDate;
        }
        return left->uniqueName() < right->uniqueName();
    });
    replies.prepend(root);
    return replies;
}

class CloseButton : public QPushButton
{
    Q_OBJECT

public:
    explicit CloseButton(QWidget *parent = Q_NULLPTR, bool modern = false)
        : QPushButton(parent)
    {
        setObjectName(QStringLiteral("annotationClose"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        const QSize size = modern ? QSize(26, 26) : QSize(14, 14);
        setFixedSize(size);
        setIcon(style()->standardIcon(QStyle::SP_DockWidgetCloseButton));
        setIconSize(modern ? QSize(14, 14) : size);
        setFlat(modern);
        setToolTip(i18n("Close this note"));
        setCursor(Qt::ArrowCursor);
    }
};

class MovableTitle : public QWidget
{
    Q_OBJECT

public:
    explicit MovableTitle(AnnotWindow *parent, bool threadedCommentUi, KTextEdit *threadEditor)
        : QWidget(parent)
        , m_threadedCommentUi(threadedCommentUi)
        , m_threadEditor(threadEditor)
    {
        QVBoxLayout *mainlay = new QVBoxLayout(this);
        mainlay->setContentsMargins(0, 0, 0, 0);
        mainlay->setSpacing(0);

        QHBoxLayout *buttonlay = new QHBoxLayout();
        buttonlay->setContentsMargins(m_threadedCommentUi ? 18 : 0, m_threadedCommentUi ? 10 : 0, m_threadedCommentUi ? 10 : 0, m_threadedCommentUi ? 10 : 0);
        buttonlay->setSpacing(8);
        titleLabel = new QLabel(this);
        QFont f = titleLabel->font();
        f.setBold(true);
        if (m_threadedCommentUi) {
            f.setWeight(QFont::Medium);
        }
        titleLabel->setFont(f);
        titleLabel->setObjectName(QStringLiteral("annotationTitleLabel"));
        titleLabel->setCursor(GuiUtils::moveCursorShape());
        if (m_threadedCommentUi) {
            titleLabel->setText(i18n("Comment thread"));
        }
        buttonlay->addWidget(titleLabel, 1);
        dateLabel = new QLabel(this);
        dateLabel->setObjectName(QStringLiteral("annotationThreadCount"));
        dateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (m_threadedCommentUi) {
            dateLabel->hide();
            actionButton = new QToolButton(this);
            actionButton->setObjectName(QStringLiteral("annotationThreadAction"));
            actionButton->setCheckable(true);
            actionButton->setAutoRaise(true);
            actionButton->setFixedSize(28, 28);
            actionButton->setIconSize(QSize(20, 20));
            actionButton->setAccessibleName(i18n("Action needed"));
            actionButton->setEnabled(false);
            actionButton->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 5px; } QToolButton:checked { background: #fff0d5; } QToolButton:hover { background: rgba(128,128,128,0.12); }"));
            QIcon flag;
            for (bool checked : {false, true}) {
                QPixmap pixmap(40, 40);
                pixmap.setDevicePixelRatio(2);
                pixmap.fill(Qt::transparent);
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(checked ? QColor(QStringLiteral("#bc7800")) : palette().color(QPalette::WindowText), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                QPainterPath path;
                path.moveTo(4, 3);
                path.cubicTo(8, 0, 11, 7, 16, 3);
                path.lineTo(16, 12);
                path.cubicTo(11, 16, 8, 9, 4, 12);
                path.closeSubpath();
                painter.setBrush(checked ? QBrush(QColor(QStringLiteral("#d58b00"))) : Qt::NoBrush);
                painter.drawPath(path);
                painter.drawLine(QPointF(4, 3), QPointF(4, 18));
                painter.end();
                flag.addPixmap(pixmap, QIcon::Normal, checked ? QIcon::On : QIcon::Off);
            }
            actionButton->setIcon(flag);
            buttonlay->addWidget(actionButton);
        } else {
            buttonlay->addWidget(dateLabel);
        }
        CloseButton *close = new CloseButton(this, m_threadedCommentUi);
        connect(close, &QAbstractButton::clicked, parent, &QWidget::close);
        buttonlay->addWidget(close);
        if (m_threadedCommentUi) {
            auto *header = new QWidget(this);
            header->setObjectName(QStringLiteral("annotationThreadHeader"));
            header->setLayout(buttonlay);
            mainlay->addWidget(header);
        } else {
            mainlay->addLayout(buttonlay);
        }

        QScrollArea *scrollArea = new QScrollArea(this);
        scrollArea->setObjectName(QStringLiteral("annotationThreadScrollArea"));
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameStyle(QFrame::NoFrame);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        if (m_threadedCommentUi) {
            scrollArea->setMinimumHeight(40);
            scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        } else {
            scrollArea->setMinimumHeight(0);
            scrollArea->setMaximumHeight(112);
            scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        }
        QWidget *scrollContainer = new QWidget(scrollArea);
        scrollContainer->setObjectName(QStringLiteral("annotationThreadContainer"));
        scrollContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        threadLayout = new QVBoxLayout(scrollContainer);
        threadLayout->setContentsMargins(m_threadedCommentUi ? 14 : 2,
                                         m_threadedCommentUi ? 4 : 4,
                                         m_threadedCommentUi ? 14 : 10,
                                         m_threadedCommentUi ? 12 : 4);
        threadLayout->setSpacing(m_threadedCommentUi ? 10 : 0);
        if (m_threadedCommentUi) {
            threadLayout->setContentsMargins(0, 0, 0, 0);
            threadLayout->setSpacing(0);
            // Extra viewport space belongs below the conversation, never inside a message.
            threadLayout->setAlignment(Qt::AlignTop);
        }

        scrollArea->setWidget(scrollContainer);
        mainlay->addWidget(scrollArea, m_threadedCommentUi ? 1 : 0);

        optionButton = new QToolButton(this);
        QString opttext = i18n("Options");
        optionButton->setText(opttext);
        optionButton->setAutoRaise(true);
        QSize s = QFontMetrics(optionButton->font()).boundingRect(opttext).size() + QSize(8, 8);
        optionButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        optionButton->setFixedSize(s);
        optionButton->hide();

        if (!m_threadedCommentUi) {
            authorAndDateLabel = new QLabel(this);
            authorAndDateLabel->setContentsMargins(0, 10, 0, 0);
            authorAndDateLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: black"));
            authorAndDateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            authorAndDateLabel->setObjectName(QStringLiteral("annotationAuthorAndDateLabel"));
            authorAndDateLabel->setCursor(GuiUtils::moveCursorShape());

            QHBoxLayout *optionlay = new QHBoxLayout();
            optionlay->addWidget(authorAndDateLabel);
            optionlay->addWidget(optionButton);
            mainlay->addLayout(optionlay);
        }

        latexButton = new QToolButton(this);
        QHBoxLayout *latexlay = new QHBoxLayout();
        QString latextext = i18n("This annotation may contain LaTeX code.\nClick here to render.");
        latexButton->setText(latextext);
        latexButton->setAutoRaise(true);
        s = QFontMetrics(latexButton->font()).boundingRect(0, 0, this->width(), this->height(), 0, latextext).size() + QSize(8, 8);
        latexButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        latexButton->setFixedSize(s);
        latexButton->setCheckable(true);
        latexButton->setVisible(false);
        latexlay->addSpacing(1);
        latexlay->addWidget(latexButton);
        latexlay->addSpacing(1);
        connect(latexButton, &QToolButton::clicked, parent, &AnnotWindow::renderLatex);
        connect(parent, &AnnotWindow::containsLatex, latexButton, &QWidget::setVisible);
        mainlay->addLayout(latexlay);

        setScrollArea(scrollArea);
        scrollArea->setVisible(m_threadedCommentUi);

        titleLabel->installEventFilter(this);
        if (authorAndDateLabel) {
            authorAndDateLabel->installEventFilter(this);
        }
        if (m_threadedCommentUi && m_threadEditor && m_threadEditor->document() && m_threadEditor->document()->documentLayout()) {
            connect(m_threadEditor->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { updateThreadEditorHeight(); });
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (scrollAr && watched == scrollAr->viewport()) {
            if (event->type() == QEvent::Resize) {
                QTimer::singleShot(0, this, [this] { updateThreadGeometry(); });
            }
            return false;
        }
        if (watched != titleLabel && watched != authorAndDateLabel) {
            return false;
        }

        QMouseEvent *me = nullptr;
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            me = static_cast<QMouseEvent *>(event);
            mousePressPos = me->pos();
            static_cast<QWidget *>(watched)->setCursor(GuiUtils::moveCursorShape(true));
            parentWidget()->raise();
            break;
        case QEvent::MouseButtonRelease:
            mousePressPos = QPoint();
            static_cast<QWidget *>(watched)->setCursor(GuiUtils::moveCursorShape());
            break;
        case QEvent::MouseMove: {
            me = static_cast<QMouseEvent *>(event);
            if (!(me->buttons() & Qt::LeftButton)) {
                return false;
            }

            // viewport info
            const QPoint topLeftPoint = parentWidget()->parentWidget()->pos();
            const int viewportHeight = parentWidget()->parentWidget()->height();
            const int viewportWidth = parentWidget()->parentWidget()->width();

            // annotation's popup window info
            QPoint newPositionPoint = me->pos() - mousePressPos + parentWidget()->pos();
            const int annotHeight = parentWidget()->height();
            const int annotWidth = parentWidget()->width();

            // make sure x is in range
            if (newPositionPoint.x() < topLeftPoint.x()) {
                newPositionPoint.setX(topLeftPoint.x());
            } else if (newPositionPoint.x() + annotWidth > topLeftPoint.x() + viewportWidth) {
                newPositionPoint.setX(topLeftPoint.x() + viewportWidth - annotWidth);
            }

            // make sure y is in range
            if (newPositionPoint.y() < topLeftPoint.y()) {
                newPositionPoint.setY(topLeftPoint.y());
            } else if (newPositionPoint.y() + annotHeight > topLeftPoint.y() + viewportHeight) {
                newPositionPoint.setY(topLeftPoint.y() + viewportHeight - annotHeight);
            }

            parentWidget()->move(newPositionPoint);
            break;
        }
        default:
            return false;
        }
        return true;
    }

    int threadMessageCount() const
    {
        return countThreadMessages;
    }

    QToolButton *threadActionButton() const { return actionButton; }
    QLabel *threadCountLabel() const { return dateLabel; }

    void setThreadMessageCount(int threadMessageCount)
    {
        if (m_threadedCommentUi) {
            return;
        }
        if (threadMessageCount == countThreadMessages && listThreadAuthorAndDateLabels.size() == threadMessageCount) {
            return;
        }
        while (QLayoutItem *item = threadLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        listThreadAuthorAndDateLabels.clear();
        listThreadContentLabels.clear();
        countThreadMessages = threadMessageCount;
        for (int index = 0; index < threadMessageCount; ++index) {
            auto *message = new QWidget(threadLayout->parentWidget());
            message->setObjectName(QStringLiteral("annotationThreadMessage"));
            message->setMinimumWidth(0);
            message->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            auto *messageLayout = new QVBoxLayout(message);
            messageLayout->setContentsMargins(index == 0 ? 0 : 16, index == 0 ? 0 : 8, 0, 8);
            messageLayout->setSpacing(3);

            auto *authorLabel = new QLabel(message);
            authorLabel->setMinimumWidth(0);
            authorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            authorLabel->setWordWrap(true);
            QFont authorFont = authorLabel->font();
            authorFont.setBold(true);
            authorLabel->setFont(authorFont);
            authorLabel->setObjectName(QStringLiteral("annotationThreadAuthor"));
            listThreadAuthorAndDateLabels.append(authorLabel);
            messageLayout->addWidget(authorLabel);

            auto *contentsLabel = new QLabel(message);
            contentsLabel->setMinimumWidth(0);
            contentsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            contentsLabel->setWordWrap(true);
            contentsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentsLabel->setObjectName(QStringLiteral("annotationThreadContent"));
            listThreadContentLabels.append(contentsLabel);
            messageLayout->addWidget(contentsLabel);

            threadLayout->addWidget(message);

            if (index + 1 < threadMessageCount) {
                auto *separator = new QFrame(threadLayout->parentWidget());
                separator->setFrameShape(QFrame::HLine);
                separator->setFrameShadow(QFrame::Sunken);
                separator->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.1);"));
                threadLayout->addWidget(separator);
            }
        }
        if (scrollAr) {
            scrollAr->setVisible(threadMessageCount > 0);
            updateThreadGeometry();
        }
    }

    void setThreadAnnotations(const QList<Okular::Annotation *> &thread, Okular::Annotation *selected)
    {
        if (!m_threadedCommentUi || !m_threadEditor) {
            return;
        }

        if (m_threadEditor->parentWidget() != this) {
            m_threadEditor->setParent(this);
        }
        m_threadEditor->hide();
        authorAndDateLabel = nullptr;
        selectedDateLabel = nullptr;
        m_selectedMessage = nullptr;
        while (QLayoutItem *item = threadLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        listThreadAuthorAndDateLabels.clear();
        listThreadContentLabels.clear();
        countThreadMessages = qMax(0, thread.size() - 1);
        dateLabel->setText(i18np("1 message", "%1 messages", thread.size()));

        for (Okular::Annotation *message : thread) {
            if (!message) {
                continue;
            }
            const bool isSelected = message == selected;
            auto *card = new QFrame(threadLayout->parentWidget());
            card->setObjectName(isSelected ? QStringLiteral("annotationSelectedMessage") : QStringLiteral("annotationThreadMessage"));
            card->setMinimumWidth(0);
            card->setProperty("threadFirst", threadLayout->count() == 0);
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            auto *messageLayout = new QVBoxLayout(card);
            messageLayout->setContentsMargins(18, 16, 18, 16);
            messageLayout->setSpacing(3);
            messageLayout->setAlignment(Qt::AlignTop);

            auto *authorLabel = new QLabel(card);
            authorLabel->setMinimumWidth(0);
            authorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            authorLabel->setWordWrap(true);
            authorLabel->setTextFormat(Qt::PlainText);
            authorLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            QFont authorFont = authorLabel->font();
            authorFont.setWeight(QFont::Medium);
            authorLabel->setFont(authorFont);
            authorLabel->setObjectName(isSelected ? QStringLiteral("annotationAuthorAndDateLabel") : QStringLiteral("annotationThreadAuthor"));

            const QString author = message->author().trimmed().isEmpty() ? i18n("Unknown author") : message->author();
            const QDateTime date = annotationDisplayDate(message);
            const QString dateText = date.isValid() ? QLocale().toString(date.toTimeSpec(Qt::LocalTime), QLocale::ShortFormat) : i18n("No date recognized");
            authorLabel->setText(author);
            messageLayout->addWidget(authorLabel);
            auto *messageDate = new QLabel(dateText, card);
            messageDate->setObjectName(QStringLiteral("annotationMessageDate"));
            messageDate->setTextFormat(Qt::PlainText);
            messageDate->setWordWrap(true);
            messageLayout->addWidget(messageDate);
            messageLayout->addSpacing(8);

            if (isSelected) {
                authorAndDateLabel = authorLabel;
                selectedDateLabel = messageDate;
                m_selectedMessage = card;
                m_threadEditor->setParent(card);
                m_threadEditor->setObjectName(QStringLiteral("annotationSelectedEditor"));
                m_threadEditor->setFrameStyle(QFrame::NoFrame);
                m_threadEditor->setFont(authorLabel->font());
                QFont bodyFont = m_threadEditor->font();
                bodyFont.setWeight(QFont::Normal);
                m_threadEditor->setFont(bodyFont);
                m_threadEditor->document()->setDocumentMargin(0);
                m_threadEditor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                m_threadEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                m_threadEditor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                m_threadEditor->show();
                messageLayout->addWidget(m_threadEditor);
            } else {
                listThreadAuthorAndDateLabels.append(authorLabel);
                auto *contentsLabel = new QLabel(card);
                contentsLabel->setMinimumWidth(0);
                contentsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
                contentsLabel->setWordWrap(true);
                contentsLabel->setTextFormat(Qt::PlainText);
                contentsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                contentsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                contentsLabel->setObjectName(QStringLiteral("annotationThreadContent"));
                contentsLabel->setText(message->contents());
                listThreadContentLabels.append(contentsLabel);
                messageLayout->addWidget(contentsLabel);
            }
            threadLayout->addWidget(card);
        }

        scrollAr->show();
        QTimer::singleShot(0, this, [this] {
            updateThreadGeometry();
            if (scrollAr && m_selectedMessage) {
                scrollAr->ensureWidgetVisible(m_selectedMessage, 18, 18);
            }
        });
    }

    void updateThreadGeometry()
    {
        if (!scrollAr || !scrollAr->widget() || !threadLayout) {
            return;
        }
        updateThreadEditorHeight();
        threadLayout->invalidate();
        const int availableWidth = qMax(1, scrollAr->viewport()->width());
        const int contentHeight = threadLayout->hasHeightForWidth() ? threadLayout->heightForWidth(availableWidth) : threadLayout->sizeHint().height();
        scrollAr->widget()->setMinimumHeight(qMax(0, contentHeight));
        scrollAr->widget()->updateGeometry();
        scrollAr->updateGeometry();
        if (!m_threadedCommentUi) {
            scrollToLatestThreadMessage();
        }
    }

    void updateThreadEditorHeight()
    {
        if (!m_threadedCommentUi || !m_threadEditor || !m_threadEditor->document() || !m_threadEditor->document()->documentLayout()) {
            return;
        }
        const qreal documentHeight = m_threadEditor->document()->documentLayout()->documentSize().height();
        const int desiredHeight = qMax(m_threadEditor->fontMetrics().height(), static_cast<int>(documentHeight + 0.5)) + 2;
        if (m_threadEditor->height() != desiredHeight) {
            m_threadEditor->setFixedHeight(desiredHeight);
        }
    }

    void scrollToLatestThreadMessage()
    {
        if (!scrollAr || !scrollAr->verticalScrollBar()) {
            return;
        }
        QTimer::singleShot(0, scrollAr, [this] {
            if (!scrollAr || !scrollAr->verticalScrollBar() || threadLayout->count() == 0) {
                return;
            }
            QWidget *latestMessage = threadLayout->itemAt(threadLayout->count() - 1)->widget();
            if (!latestMessage) {
                return;
            }
            QScrollBar *scrollBar = scrollAr->verticalScrollBar();
            scrollBar->setValue(qMin(scrollBar->maximum(), latestMessage->y()));
        });
    }

    void setTitle(const QString &title)
    {
        if (!m_threadedCommentUi) {
            titleLabel->setText(QStringLiteral(" ") + title);
        }
    }

    void setAuthorAndDate(const QString &author, const QDateTime &dt)
    {
        QString auth;
        (author.isNull() || author.isEmpty()) ? auth = i18n("Unknown author") : auth = author;

        QString dateTime;
        dt.isValid() ? dateTime = QLocale().toString(dt.toTimeSpec(Qt::LocalTime), QLocale::ShortFormat) : dateTime = i18n("No date recognized");

        if (authorAndDateLabel) {
            authorAndDateLabel->setText(m_threadedCommentUi ? auth : QStringLiteral(" %1 (%2)").arg(auth, dateTime));
        }
        if (selectedDateLabel) {
            selectedDateLabel->setText(dateTime);
        }
    }

    void connectOptionButton(QObject *recv, const char *method)
    {
        connect(optionButton, SIGNAL(clicked()), recv, method);
    }

    void uncheckLatexButton()
    {
        latexButton->setChecked(false);
    }

    void setThreadAuthorAndDate(int index, const QString &author, const QDateTime &dt)
    {
        if (index >= 0 && index < listThreadAuthorAndDateLabels.size()) {
            QString auth;
            (author.isNull() || author.isEmpty()) ? auth = i18n("Unknown author") : auth = author;

            QString dateTime;
            dt.isValid() ? dateTime = QLocale().toString(dt.toTimeSpec(Qt::LocalTime), QLocale::ShortFormat) : dateTime = i18n("No date recognized");

            listThreadAuthorAndDateLabels[index]->setText(index == 0 ? QStringLiteral("%1 (%2)").arg(auth, dateTime) : QStringLiteral("↳ %1 (%2)").arg(auth, dateTime));
        }
    }

    void setThreadContent(int index, const QString &content)
    {
        if (index >= 0 && index < listThreadContentLabels.size()) {
            listThreadContentLabels[index]->setText(content);
        }
    }

    void setScrollArea(QScrollArea *scrollArea)
    {
        scrollAr = scrollArea;
        scrollAr->viewport()->installEventFilter(this);
        if (!m_threadedCommentUi) {
            connect(scrollAr->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { scrollToLatestThreadMessage(); });
        }
    }

    QScrollArea *scrollArea()
    {
        return scrollAr;
    }

private:
    QLabel *titleLabel;
    int countThreadMessages = 0;
    QList<QLabel *> listThreadAuthorAndDateLabels;
    QList<QLabel *> listThreadContentLabels;
    QLabel *dateLabel;
    QToolButton *actionButton = nullptr;
    QLabel *authorAndDateLabel = nullptr;
    QLabel *selectedDateLabel = nullptr;
    QPoint mousePressPos;
    QToolButton *optionButton;
    QToolButton *latexButton;
    QScrollArea *scrollAr = nullptr;
    QVBoxLayout *threadLayout = nullptr;
    bool m_threadedCommentUi = false;
    KTextEdit *m_threadEditor = nullptr;
    QWidget *m_selectedMessage = nullptr;
};

// Qt::SubWindow is needed to make QSizeGrip work
AnnotWindow::AnnotWindow(QWidget *parent, Okular::Annotation *annot, Okular::Document *document, int page, bool useCommentThreadUi, bool detachedComment)
    : QFrame(parent, Qt::SubWindow)
    , m_annot(annot)
    , m_document(document)
    , m_page(page)
    , m_threadedCommentUi(useCommentThreadUi)
    , m_detachedComment(detachedComment)
{
    setAutoFillBackground(true);
    setFrameStyle(m_threadedCommentUi ? (StyledPanel | Plain) : (Panel | Raised));
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("AnnotWindow"));

    if (m_threadedCommentUi) {
        const QPalette appPalette = QApplication::palette();
        const QColor windowColor = appPalette.color(QPalette::Window);
        const QColor cardColor = appPalette.color(QPalette::Base);
        const QColor textColor = appPalette.color(QPalette::Text);
        const auto blend = [](const QColor &foreground, const QColor &background, qreal amount) {
            return QColor::fromRgbF(foreground.redF() * amount + background.redF() * (1 - amount),
                                    foreground.greenF() * amount + background.greenF() * (1 - amount),
                                    foreground.blueF() * amount + background.blueF() * (1 - amount));
        };
        const QColor borderColor = blend(textColor, cardColor, 0.14);
        const QColor mutedColor = blend(textColor, cardColor, 0.65);
        const QColor buttonColor = appPalette.color(QPalette::Highlight);
        const QColor buttonTextColor = appPalette.color(QPalette::HighlightedText);
        const auto cssColor = [](const QColor &color) { return color.name(QColor::HexArgb); };
        setStyleSheet(QStringLiteral(
                          "QFrame#AnnotWindow { background-color: %5; border: 1px solid %2; border-radius: 10px; }"
                          "QFrame#AnnotWindow QLabel { color: %3; background: transparent; border: none; }"
                          "QFrame#AnnotWindow QLabel#annotationThreadCount, QFrame#AnnotWindow QLabel#annotationMessageDate { color: %4; }"
                          "QFrame#AnnotWindow QWidget#annotationThreadHeader { background-color: %1; border-bottom: 1px solid %2; }"
                          "QFrame#AnnotWindow QScrollArea#annotationThreadScrollArea { background: transparent; border: none; }"
                          "QFrame#AnnotWindow QWidget#annotationThreadContainer { background-color: %5; }"
                          "QFrame#AnnotWindow QFrame#annotationThreadMessage, QFrame#AnnotWindow QFrame#annotationSelectedMessage { background-color: %5; border: none; }"
                          "QFrame#AnnotWindow QFrame#annotationThreadMessage[threadFirst=\"false\"], QFrame#AnnotWindow QFrame#annotationSelectedMessage[threadFirst=\"false\"] { border-top: 1px solid %2; }"
                          "QFrame#AnnotWindow QTextEdit#annotationSelectedEditor { background-color: %5; color: %3; border: none; padding: 0; selection-background-color: %6; selection-color: %7; }"
                          "QFrame#AnnotWindow QWidget#annotationReplyFooter { background-color: %5; border-top: 1px solid %2; }"
                          "QFrame#AnnotWindow QWidget#annotationReplyComposerPanel { background-color: %5; }"
                          "QFrame#AnnotWindow QTextEdit#annotationReplyComposer { background-color: %5; color: %3; border: 1px solid %2; border-radius: 7px; padding: 7px; selection-background-color: %6; selection-color: %7; }"
                          "QFrame#AnnotWindow QTextEdit#annotationReplyComposer:focus { border: 1px solid %6; }"
                          "QFrame#AnnotWindow QPushButton#annotationReply, QFrame#AnnotWindow QPushButton#annotationReplySend, QFrame#AnnotWindow QPushButton#annotationReplyCancel { background-color: %5; color: %3; border: 1px solid %2; border-radius: 6px; padding: 6px 12px; }"
                          "QFrame#AnnotWindow QPushButton#annotationReply:focus, QFrame#AnnotWindow QPushButton#annotationReplySend:focus, QFrame#AnnotWindow QPushButton#annotationReplyCancel:focus { border: 1px solid %6; }"
                          "QFrame#AnnotWindow QPushButton#annotationReply:hover, QFrame#AnnotWindow QPushButton#annotationReplySend:hover, QFrame#AnnotWindow QPushButton#annotationReplyCancel:hover { background-color: %1; }"
                          "QFrame#AnnotWindow QPushButton#annotationReply:disabled, QFrame#AnnotWindow QPushButton#annotationReplySend:disabled { color: %4; }"
                          "QFrame#AnnotWindow QPushButton#annotationClose { border: none; border-radius: 6px; padding: 0; }"
                          "QFrame#AnnotWindow QPushButton#annotationClose:hover { background-color: %2; }")
                          .arg(cssColor(windowColor),
                               cssColor(borderColor),
                               cssColor(textColor),
                               cssColor(mutedColor),
                               cssColor(cardColor),
                               cssColor(buttonColor),
                               cssColor(buttonTextColor)));
    }

    const bool canEditAnnotation = m_detachedComment || m_document->canModifyPageAnnotation(annot);

    m_threadAnnotations = m_detachedComment ? QList<Okular::Annotation *>{m_annot} : annotationThread(m_document, m_annot);
    if (!m_threadedCommentUi) {
        m_threadAnnotations.removeAll(m_annot);
    }

    textEdit = new KTextEdit(this);
    textEdit->setAcceptRichText(false);
    textEdit->setPlainText(m_annot->contents());
    textEdit->installEventFilter(this);
    textEdit->setUndoRedoEnabled(m_detachedComment);

    m_prevCursorPos = textEdit->textCursor().position();
    m_prevAnchorPos = textEdit->textCursor().anchor();

    connect(textEdit, &KTextEdit::textChanged, this, &AnnotWindow::slotsaveWindowText);
    connect(textEdit, &KTextEdit::cursorPositionChanged, this, &AnnotWindow::slotsaveWindowText);
    connect(textEdit, &KTextEdit::aboutToShowContextMenu, this, &AnnotWindow::slotUpdateUndoAndRedoInContextMenu);
    connect(m_document, &Okular::Document::annotationContentsChangedByUndoRedo, this, &AnnotWindow::slotHandleContentsChangedByUndoRedo);

    if (!canEditAnnotation) {
        textEdit->setReadOnly(true);
    }

    QVBoxLayout *mainlay = new QVBoxLayout(this);
    mainlay->setContentsMargins(m_threadedCommentUi ? 0 : 2, m_threadedCommentUi ? 0 : 2, m_threadedCommentUi ? 0 : 2, m_threadedCommentUi ? 0 : 2);
    mainlay->setSpacing(0);
    m_title = new MovableTitle(this, m_threadedCommentUi, textEdit);
    if (m_title->threadActionButton()) {
        connect(m_title->threadActionButton(), &QToolButton::clicked, this, [this](bool checked) {
            m_threadActionPending = true;
            m_title->threadActionButton()->setEnabled(false);
            Q_EMIT threadActionSaveRequested(m_threadRootId, checked);
        });
    }
    mainlay->addWidget(m_title, m_threadedCommentUi ? 1 : 0);
    if (!m_threadedCommentUi) {
        mainlay->addWidget(textEdit);
    }

    m_relatedToggle = new QToolButton(this);
    m_relatedToggle->setObjectName(QStringLiteral("annotationRelatedToggle"));
    m_relatedToggle->setText(i18n("Details"));
    m_relatedToggle->setCheckable(true);
    m_relatedToggle->setArrowType(Qt::RightArrow);
    m_relatedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_relatedToggle->setAutoRaise(true);
    m_relatedToggle->hide();
    if (!m_threadedCommentUi) {
        mainlay->addWidget(m_relatedToggle, 0, Qt::AlignLeft);
    }

    m_relatedPanel = new QWidget(this);
    m_relatedPanel->setObjectName(QStringLiteral("annotationRelatedPanel"));
    auto *relatedLayout = new QVBoxLayout(m_relatedPanel);
    relatedLayout->setContentsMargins(m_threadedCommentUi ? 18 : 6, 4, m_threadedCommentUi ? 18 : 6, 8);
    relatedLayout->setSpacing(4);

    auto *metadataLayout = new QFormLayout();
    metadataLayout->setContentsMargins(0, 0, 0, 0);
    metadataLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_commentTypeLabel = new QLabel(m_relatedPanel);
    m_commentTypeLabel->setObjectName(QStringLiteral("annotationCommentType"));
    m_commentTypeLabel->setWordWrap(true);
    m_commentTypeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metadataLayout->addRow(i18n("Type:"), m_commentTypeLabel);
    if (m_threadedCommentUi) {
        metadataLayout->addRow(i18n("Thread:"), m_title->threadCountLabel());
        m_title->threadCountLabel()->show();
    }
    m_linkingAttemptLabel = new QLabel(m_relatedPanel);
    m_linkingAttemptLabel->setObjectName(QStringLiteral("annotationLinkingAttempt"));
    m_linkingAttemptLabel->setWordWrap(true);
    m_linkingAttemptLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metadataLayout->addRow(i18n("Linking attempt:"), m_linkingAttemptLabel);
    m_linkingMethodLabel = new QLabel(m_relatedPanel);
    m_linkingMethodLabel->setObjectName(QStringLiteral("annotationLinkingMethod"));
    m_linkingMethodLabel->setWordWrap(true);
    m_linkingMethodLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metadataLayout->addRow(i18n("Linking method:"), m_linkingMethodLabel);
    m_priorityEditor = new QComboBox(m_relatedPanel);
    m_priorityEditor->setObjectName(QStringLiteral("annotationRelatedPriority"));
    m_priorityEditor->addItem(i18n("Normal"), QStringLiteral("normal"));
    m_priorityEditor->addItem(i18n("High"), QStringLiteral("high"));
    metadataLayout->addRow(i18n("Priority:"), m_priorityEditor);

    m_locationEnabled = new QCheckBox(i18n("Attach location"), m_relatedPanel);
    m_locationEnabled->setObjectName(QStringLiteral("annotationRelatedLocationEnabled"));
    metadataLayout->addRow(QString(), m_locationEnabled);
    m_locationLabel = new QLineEdit(m_relatedPanel);
    m_locationLabel->setObjectName(QStringLiteral("annotationRelatedLocationLabel"));
    m_locationLabel->setPlaceholderText(i18n("Place name"));
    metadataLayout->addRow(i18n("Place:"), m_locationLabel);

    auto *coordinates = new QWidget(m_relatedPanel);
    auto *coordinatesLayout = new QHBoxLayout(coordinates);
    coordinatesLayout->setContentsMargins(0, 0, 0, 0);
    m_locationLatitude = new QDoubleSpinBox(coordinates);
    m_locationLatitude->setObjectName(QStringLiteral("annotationRelatedLatitude"));
    m_locationLatitude->setRange(-90.0, 90.0);
    m_locationLatitude->setDecimals(6);
    m_locationLatitude->setSingleStep(0.001);
    m_locationLatitude->setPrefix(i18n("Lat "));
    m_locationLongitude = new QDoubleSpinBox(coordinates);
    m_locationLongitude->setObjectName(QStringLiteral("annotationRelatedLongitude"));
    m_locationLongitude->setRange(-180.0, 180.0);
    m_locationLongitude->setDecimals(6);
    m_locationLongitude->setSingleStep(0.001);
    m_locationLongitude->setPrefix(i18n("Lon "));
    coordinatesLayout->addWidget(m_locationLatitude);
    coordinatesLayout->addWidget(m_locationLongitude);
    metadataLayout->addRow(i18n("Coordinates:"), coordinates);
    relatedLayout->addLayout(metadataLayout);

    auto *metadataActions = new QHBoxLayout();
    m_applyRelatedButton = new QPushButton(i18n("Apply"), m_relatedPanel);
    m_applyRelatedButton->setObjectName(QStringLiteral("annotationRelatedApply"));
    m_applyRelatedButton->setAutoDefault(false);
    metadataActions->addStretch();
    metadataActions->addWidget(m_applyRelatedButton);
    relatedLayout->addLayout(metadataActions);

    auto *chatGptLayout = new QHBoxLayout();
    m_chatGptUrlEditor = new QLineEdit(m_relatedPanel);
    m_chatGptUrlEditor->setObjectName(QStringLiteral("annotationRelatedChatGptUrl"));
    m_chatGptUrlEditor->setPlaceholderText(i18n("ChatGPT link"));
    m_saveChatGptButton = new QPushButton(i18n("Save"), m_relatedPanel);
    m_saveChatGptButton->setObjectName(QStringLiteral("annotationRelatedSaveChatGpt"));
    m_saveChatGptButton->setAutoDefault(false);
    m_openChatGptButton = new QPushButton(i18n("Open"), m_relatedPanel);
    m_openChatGptButton->setObjectName(QStringLiteral("annotationRelatedOpenChatGpt"));
    m_openChatGptButton->setAutoDefault(false);
    chatGptLayout->addWidget(m_chatGptUrlEditor, 1);
    chatGptLayout->addWidget(m_saveChatGptButton);
    chatGptLayout->addWidget(m_openChatGptButton);
    relatedLayout->addLayout(chatGptLayout);

    auto *relatedActions = new QHBoxLayout();
    m_openCodexButton = new QPushButton(i18n("Open Codex"), m_relatedPanel);
    m_openCodexButton->setObjectName(QStringLiteral("annotationRelatedOpenCodex"));
    m_openCodexButton->setAutoDefault(false);
    m_openSourceButton = new QPushButton(i18n("Open Source"), m_relatedPanel);
    m_openSourceButton->setObjectName(QStringLiteral("annotationRelatedOpenSource"));
    m_openSourceButton->setAutoDefault(false);
    m_openOverviewButton = new QPushButton(i18n("Open in Annotations"), m_relatedPanel);
    m_openOverviewButton->setObjectName(QStringLiteral("annotationRelatedOpenOverview"));
    m_openOverviewButton->setAutoDefault(false);
    relatedActions->addWidget(m_openCodexButton);
    relatedActions->addWidget(m_openSourceButton);
    relatedActions->addWidget(m_openOverviewButton);
    relatedLayout->addLayout(relatedActions);

    m_relatedStatus = new QLabel(m_relatedPanel);
    m_relatedStatus->setObjectName(QStringLiteral("annotationRelatedStatus"));
    m_relatedStatus->setWordWrap(true);
    m_relatedStatus->hide();
    relatedLayout->addWidget(m_relatedStatus);
    m_relatedPanel->hide();
    mainlay->addWidget(m_relatedPanel);

    connect(m_relatedToggle, &QToolButton::toggled, this, [this](bool expanded) {
        if (expanded) {
            m_relatedCollapsedSize = size();
        }
        m_relatedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_relatedPanel->setVisible(expanded && m_relatedInfoAvailable);
        if (!expanded && m_relatedCollapsedSize.isValid()) {
            resize(m_relatedCollapsedSize);
            m_relatedCollapsedSize = QSize();
        } else if (m_threadedCommentUi) {
            QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents);
        } else if (expanded) {
            adjustSize();
        }
    });
    const auto relatedChanged = [this] {
        if (!m_loadingRelatedInfo) {
            updateRelatedActions();
        }
    };
    connect(m_priorityEditor, &QComboBox::currentIndexChanged, this, relatedChanged);
    connect(m_locationEnabled, &QCheckBox::toggled, this, [this, relatedChanged](bool enabled) {
        m_locationLabel->setEnabled(enabled && !m_relatedOperationPending);
        m_locationLatitude->setEnabled(enabled && !m_relatedOperationPending);
        m_locationLongitude->setEnabled(enabled && !m_relatedOperationPending);
        relatedChanged();
    });
    connect(m_locationLabel, &QLineEdit::textChanged, this, relatedChanged);
    connect(m_locationLatitude, &QDoubleSpinBox::valueChanged, this, relatedChanged);
    connect(m_locationLongitude, &QDoubleSpinBox::valueChanged, this, relatedChanged);
    connect(m_chatGptUrlEditor, &QLineEdit::textChanged, this, relatedChanged);
    connect(m_applyRelatedButton, &QPushButton::clicked, this, [this] {
        QString error;
        const QVariantMap patch = relatedMetadataPatch(&error);
        if (!error.isEmpty()) {
            m_relatedStatus->setText(error);
            m_relatedStatus->show();
            return;
        }
        if (!patch.isEmpty()) {
            setRelatedOperationPending(true);
            Q_EMIT relatedMetadataSaveRequested(patch);
        }
    });
    connect(m_saveChatGptButton, &QPushButton::clicked, this, [this] {
        setRelatedOperationPending(true);
        Q_EMIT relatedChatGptUrlSaveRequested(m_chatGptUrlEditor->text(), false);
    });
    connect(m_openChatGptButton, &QPushButton::clicked, this, [this] {
        if (m_chatGptUrlEditor->text() != m_originalChatGptUrl) {
            setRelatedOperationPending(true);
            Q_EMIT relatedChatGptUrlSaveRequested(m_chatGptUrlEditor->text(), true);
        } else {
            Q_EMIT relatedOpenChatGptRequested(m_originalChatGptUrl);
        }
    });
    connect(m_openCodexButton, &QPushButton::clicked, this, &AnnotWindow::relatedOpenCodexRequested);
    connect(m_openSourceButton, &QPushButton::clicked, this, [this] { Q_EMIT relatedOpenSourceRequested(m_sourcePath, m_sourceLine); });
    connect(m_openOverviewButton, &QPushButton::clicked, this, &AnnotWindow::relatedOpenOverviewRequested);

    if (m_threadedCommentUi) {
        m_replyComposerPanel = new QWidget(this);
        m_replyComposerPanel->setObjectName(QStringLiteral("annotationReplyComposerPanel"));
        auto *composerLayout = new QVBoxLayout(m_replyComposerPanel);
        composerLayout->setContentsMargins(18, 4, 18, 16);
        composerLayout->setSpacing(8);
        m_replyComposer = new KTextEdit(m_replyComposerPanel);
        m_replyComposer->setObjectName(QStringLiteral("annotationReplyComposer"));
        m_replyComposer->setAcceptRichText(false);
        m_replyComposer->setPlaceholderText(i18n("Write a reply…"));
        m_replyComposer->setAccessibleName(i18n("Reply to this thread"));
        m_replyComposer->setFixedHeight(88);
        composerLayout->addWidget(m_replyComposer);

        auto *composerActions = new QHBoxLayout();
        composerActions->addStretch();
        auto *cancelReplyButton = new QPushButton(i18n("Cancel"), m_replyComposerPanel);
        cancelReplyButton->setObjectName(QStringLiteral("annotationReplyCancel"));
        cancelReplyButton->setAutoDefault(false);
        m_replySendButton = new QPushButton(i18n("Send reply"), m_replyComposerPanel);
        m_replySendButton->setObjectName(QStringLiteral("annotationReplySend"));
        m_replySendButton->setAutoDefault(false);
        m_replySendButton->setEnabled(false);
        composerActions->addWidget(cancelReplyButton);
        composerActions->addWidget(m_replySendButton);
        composerLayout->addLayout(composerActions);
        m_replyComposerPanel->hide();
        mainlay->addWidget(m_replyComposerPanel);

        connect(m_replyComposer, &QTextEdit::textChanged, this, [this] {
            m_replySendButton->setEnabled(!m_replyOperationPending && !m_replyComposer->toPlainText().trimmed().isEmpty());
        });
        connect(cancelReplyButton, &QPushButton::clicked, this, [this] {
            if (!m_replyOperationPending) {
                m_replyComposer->clear();
                m_replyComposerPanel->hide();
                m_replyButton->show();
                QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents);
            }
        });
        connect(m_replySendButton, &QPushButton::clicked, this, [this] {
            const QString contents = m_replyComposer->toPlainText().trimmed();
            if (contents.isEmpty() || m_replyOperationPending) {
                return;
            }
            m_replyOperationPending = true;
            m_replyComposer->setEnabled(false);
            m_replySendButton->setEnabled(false);
            Q_EMIT replyRequested(contents);
        });
    }

    QHBoxLayout *lowerlay = nullptr;
    if (m_threadedCommentUi) {
        m_replyFooter = new QWidget(this);
        m_replyFooter->setObjectName(QStringLiteral("annotationReplyFooter"));
        lowerlay = new QHBoxLayout(m_replyFooter);
        lowerlay->setContentsMargins(18, 10, 10, 10);
        mainlay->insertWidget(mainlay->indexOf(m_relatedPanel), m_replyFooter);
        lowerlay->addWidget(m_relatedToggle);
        lowerlay->addStretch();
    } else {
        lowerlay = new QHBoxLayout();
        lowerlay->setContentsMargins(0, 0, 0, 0);
        mainlay->addLayout(lowerlay);
    }
    m_replyButton = new QPushButton(QIcon::fromTheme(QStringLiteral("mail-reply-sender")), i18n("Reply"), this);
    m_replyButton->setObjectName(QStringLiteral("annotationReply"));
    m_replyButton->setAutoDefault(false);
    if (m_threadedCommentUi) {
        m_replyButton->setIcon(QIcon());
    }
    m_replyButton->setEnabled((m_detachedComment || m_document->isAllowed(Okular::AllowNotes)) && !m_annot->uniqueName().isEmpty() && !m_annot->contents().trimmed().isEmpty());
    connect(m_replyButton, &QPushButton::clicked, this, [this] {
        if (!m_threadedCommentUi || !m_replyComposerPanel) {
            Q_EMIT replyRequested(QString());
            return;
        }
        m_replyButton->hide();
        m_replyComposerPanel->show();
        QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents);
        m_replyComposer->setFocus(Qt::OtherFocusReason);
    });
    lowerlay->addWidget(m_replyButton);
    if (!m_threadedCommentUi) {
        lowerlay->addStretch();
    }
    QSizeGrip *sb = new QSizeGrip(this);
    lowerlay->addWidget(sb);

    m_latexRenderer = new GuiUtils::LatexRenderer();
    // The Q_EMIT below is not wrong even if emitting signals from the constructor it's usually wrong
    // in this case the signal it's connected to inside MovableTitle constructor a few lines above
    Q_EMIT containsLatex(GuiUtils::LatexRenderer::mightContainLatex(m_annot->contents())); // clazy:exclude=incorrect-emit

    m_title->setTitle(m_annot->window().summary());
    m_title->connectOptionButton(this, SLOT(slotOptionBtn()));

    if (m_threadedCommentUi) {
        const int availableWidth = qMax(320, parent ? parent->width() - 20 : 460);
        const int availableHeight = qMax(320, parent ? parent->height() - 20 : 520);
        const int width = qMin(440, availableWidth);
        const int height = qMin(240, availableHeight);
        setMinimumSize(qMin(340, width), 160);
        setGeometry(10, 10, width, height);
    } else {
        setGeometry(10, 10, 300, 300);
    }

    reloadInfo();
    if (m_threadedCommentUi) {
        connect(textEdit, &QTextEdit::textChanged, this, [this] { QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents); });
    }
}

static Okular::Annotation *detachedDisplayAnnotation(const QJsonObject &record)
{
    Okular::Annotation *annotation = record.value(QStringLiteral("subtype")).toString() == QLatin1String("Highlight")
        ? static_cast<Okular::Annotation *>(new Okular::HighlightAnnotation()) : new Okular::TextAnnotation();
    annotation->setUniqueName(record.value(QStringLiteral("id")).toString());
    annotation->setContents(record.value(QStringLiteral("contents")).toString());
    annotation->setAuthor(Thoughts::commentAuthor(record));
    annotation->setCreationDate(Okular::DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("created"))));
    annotation->setModificationDate(Okular::DocsCommentSynchronizer::parseCommentDate(record.value(QStringLiteral("modified"))));
    return annotation;
}

AnnotWindow::AnnotWindow(QWidget *parent, const QList<QJsonObject> &thread, const QString &selectedId, Okular::Document *document, int page)
    : AnnotWindow(parent, detachedDisplayAnnotation([&] {
        for (const auto &record : thread) {
            if (record.value(QStringLiteral("id")).toString() == selectedId) return record;
        }
        return QJsonObject();
    }()), document, page, true, true)
{
    m_ownedThread.append(m_annot);
    setDetachedThread(thread);
}

void AnnotWindow::setDetachedThread(const QList<QJsonObject> &thread)
{
    Q_ASSERT(m_detachedComment);
    const QSignalBlocker refreshSignals(textEdit);
    QJsonObject selectedRecord;
    for (const auto &record : thread) {
        if (record.value(QStringLiteral("id")).toString() == m_annot->uniqueName()
            && !record.value(QStringLiteral("deleted")).toBool()) {
            selectedRecord = record;
            break;
        }
    }
    const QString incoming = selectedRecord.value(QStringLiteral("contents")).toString();
    const QString draft = textEdit->toPlainText();
    const bool clean = draft == m_annot->contents();
    const bool conflict = draft != m_annot->contents() && incoming != m_annot->contents() && incoming != draft;
    if (selectedRecord.isEmpty() || conflict) {
        m_detachedRefreshBlocked = true;
        textEdit->setReadOnly(true);
        if (!m_detachedRefreshNotice) {
            m_detachedRefreshNotice = new QLabel(this);
            m_detachedRefreshNotice->setObjectName(QStringLiteral("annotationRefreshNotice"));
            m_detachedRefreshNotice->setWordWrap(true);
            layout()->addWidget(m_detachedRefreshNotice);
        }
        m_detachedRefreshNotice->setText(selectedRecord.isEmpty()
            ? i18n("This message was deleted. Text saving is paused; you can still copy your draft.")
            : i18n("This message changed elsewhere. Text saving is paused to preserve your draft. Copy it, then close and reopen the comment to load the latest version."));
        m_detachedRefreshNotice->show();
        fitThreadToContents();
        // Preserve the draft and its selected card, including after repeated refreshes.
        return;
    }
    if (m_detachedRefreshBlocked) return;
    m_annot->setContents(incoming);
    m_annot->setAuthor(Thoughts::commentAuthor(selectedRecord));
    m_annot->setCreationDate(Okular::DocsCommentSynchronizer::parseCommentDate(selectedRecord.value(QStringLiteral("created"))));
    m_annot->setModificationDate(Okular::DocsCommentSynchronizer::parseCommentDate(selectedRecord.value(QStringLiteral("modified"))));
    if (clean && draft != incoming) {
        textEdit->setPlainText(incoming);
        m_prevCursorPos = textEdit->textCursor().position();
        m_prevAnchorPos = textEdit->textCursor().anchor();
    }
    // Keep the selected model/editor alive so a reply refresh preserves its cursor and undo history.
    for (auto *annotation : std::as_const(m_ownedThread)) {
        if (annotation != m_annot) delete annotation;
    }
    m_ownedThread = {m_annot};
    m_threadAnnotations.clear();
    for (const auto &record : thread) {
        auto *annotation = record.value(QStringLiteral("id")).toString() == m_annot->uniqueName() ? m_annot : detachedDisplayAnnotation(record);
        if (annotation != m_annot) m_ownedThread.append(annotation);
        m_threadAnnotations.append(annotation);
    }
    reloadInfo();
}

void AnnotWindow::fitThreadToContents()
{
    if (!m_threadedCommentUi) {
        return;
    }
    layout()->activate();
    m_title->layout()->activate();
    m_title->updateThreadGeometry();
    QScrollArea *scrollArea = m_title->scrollArea();
    // Expanded panels can change the minimum size before Qt has updated the
    // child geometry. Measure the controls from their size hints, not that
    // transient geometry, so Details cannot consume the conversation's space.
    const QWidget *header = m_title->findChild<QWidget *>(QStringLiteral("annotationThreadHeader"));
    const int chromeHeight = layout()->sizeHint().height() - m_title->sizeHint().height() + header->sizeHint().height() + height() - contentsRect().height();
    const int availableHeight = parentWidget() ? parentWidget()->height() - 20 : 560;
    const int maximumHeight = qMax(minimumSizeHint().height(), qMin(560, availableHeight));
    const int desiredHeight = qBound(minimumSizeHint().height(), scrollArea->widget()->minimumHeight() + chromeHeight, maximumHeight);
    resize(width(), desiredHeight);
    if (parentWidget()) {
        move(qMax(0, qMin(x(), parentWidget()->width() - width())), qMax(0, qMin(y(), parentWidget()->height() - height())));
    }
}

AnnotWindow::~AnnotWindow()
{
    delete m_latexRenderer;
    delete textEdit;
    qDeleteAll(m_ownedThread);
}

void AnnotWindow::closeEvent(QCloseEvent *event)
{
    Q_EMIT aboutToClose(m_annot, m_page);
    QFrame::closeEvent(event);
}

Okular::Annotation *AnnotWindow::annotation() const
{
    return m_annot;
}

void AnnotWindow::updateAnnotation(Okular::Annotation *a)
{
    m_annot = a;
    if (m_annot) {
        m_threadAnnotations = annotationThread(m_document, m_annot);
        reloadInfo();
    }
}

void AnnotWindow::reloadInfo()
{
    if (!m_annot) {
        return;
    }
    if (!m_detachedComment) {
        m_threadAnnotations = annotationThread(m_document, m_annot);
    }
    // A reply inherits the context of the original comment, even though replies are text notes.
    const Okular::Annotation *threadRoot = m_threadAnnotations.isEmpty() ? m_annot : m_threadAnnotations.constFirst();
    switch (threadRoot->subType()) {
    case Okular::Annotation::AText:
        m_commentTypeLabel->setText(i18n("Standalone comment"));
        break;
    case Okular::Annotation::AHighlight:
        m_commentTypeLabel->setText(i18n("Comment on highlighted text"));
        break;
    default:
        m_commentTypeLabel->setText(i18n("Comment"));
        break;
    }
    if (m_threadedCommentUi) {
        m_title->setThreadAnnotations(m_threadAnnotations, m_annot);
    } else {
        m_threadAnnotations.removeAll(m_annot);
        m_title->setThreadMessageCount(m_threadAnnotations.size());
    }
    if (m_replyButton) {
        m_replyButton->setEnabled((m_detachedComment || m_document->isAllowed(Okular::AllowNotes)) && !m_annot->uniqueName().isEmpty() && !m_annot->contents().trimmed().isEmpty());
    }
    if (m_threadedCommentUi) {
        setPalette(QApplication::palette());
        textEdit->setPalette(QApplication::palette());
    } else {
        QColor newcolor;
        if (m_annot->subType() == Okular::Annotation::AText) {
            Okular::TextAnnotation *textAnn = static_cast<Okular::TextAnnotation *>(m_annot);
            if (textAnn->textType() == Okular::TextAnnotation::InPlace && textAnn->inplaceIntent() == Okular::TextAnnotation::TypeWriter) {
                newcolor = QColor(0xfd, 0xfd, 0x96);
            }
        }
        if (!newcolor.isValid()) {
            newcolor = m_annot->style().color().isValid() ? QColor(m_annot->style().color().red(), m_annot->style().color().green(), m_annot->style().color().blue(), 255) : Qt::yellow;
        }
        if (newcolor != m_color) {
            m_color = newcolor;
            setPalette(QPalette(m_color));
            QPalette pl = textEdit->palette();
            pl.setColor(QPalette::Base, m_color);
            textEdit->setPalette(pl);
        }
    }

    if (!m_threadedCommentUi) {
        for (int index = 0; index < m_threadAnnotations.size() && index < m_title->threadMessageCount(); ++index) {
            Okular::Annotation *message = m_threadAnnotations.at(index);
            m_title->setThreadAuthorAndDate(index, message->author(), annotationDisplayDate(message));
            m_title->setThreadContent(index, message->contents());
        }
    }

    if (QScrollArea *scrollArea = m_title->scrollArea()) {
        scrollArea->widget()->updateGeometry();
        scrollArea->updateGeometry();
        m_title->updateThreadGeometry();
    }

    m_title->setAuthorAndDate(m_annot->author(), annotationDisplayDate(m_annot));
    if (m_threadedCommentUi) {
        QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents);
    }
}

QVariantMap AnnotWindow::relatedMetadataPatch(QString *error) const
{
    QVariantMap patch;
    const QString priority = m_priorityEditor->currentData().toString();
    if (priority != m_originalPriority) {
        if (priority != QLatin1String("normal") && priority != QLatin1String("high")) {
            if (error) {
                *error = i18n("Choose Normal or High before saving the priority.");
            }
            return {};
        }
        patch.insert(QStringLiteral("priority"), priority);
    }

    if (!m_locationEnabled->isChecked()) {
        if (m_originalLocationPresent) {
            patch.insert(QStringLiteral("location"), QVariant());
        }
        return patch;
    }

    const QString label = m_locationLabel->text().trimmed();
    if (label.isEmpty()) {
        if (error) {
            *error = i18n("Enter a place name for the comment location.");
        }
        return {};
    }
    QJsonObject location = m_originalLocationPresent ? m_originalLocation : QJsonObject();
    location.insert(QStringLiteral("label"), label);
    location.insert(QStringLiteral("latitude"), m_locationLatitude->value());
    location.insert(QStringLiteral("longitude"), m_locationLongitude->value());
    if (!location.contains(QStringLiteral("source"))) {
        location.insert(QStringLiteral("source"), QStringLiteral("manual"));
    }
    if (!m_originalLocationPresent || location != m_originalLocation) {
        patch.insert(QStringLiteral("location"), location.toVariantMap());
    }
    return patch;
}

void AnnotWindow::setRelatedInfo(const QVariantMap &info, const QString &status)
{
    setThreadActionInfo(info);
    m_loadingRelatedInfo = true;
    m_relatedInfoAvailable = info.value(QStringLiteral("available")).toBool();
    m_relatedToggle->setVisible(m_relatedInfoAvailable);
    if (!m_relatedInfoAvailable) {
        m_relatedToggle->setChecked(false);
        m_relatedPanel->hide();
        m_loadingRelatedInfo = false;
        return;
    }

    while (m_priorityEditor->count() > 2) {
        m_priorityEditor->removeItem(m_priorityEditor->count() - 1);
    }
    m_originalPriority = info.value(QStringLiteral("priority")).toString();
    if (m_originalPriority.isEmpty()) {
        m_originalPriority = QStringLiteral("normal");
    }
    int priorityIndex = m_priorityEditor->findData(m_originalPriority);
    if (priorityIndex < 0) {
        m_priorityEditor->addItem(i18n("Unknown (%1)", m_originalPriority), m_originalPriority);
        priorityIndex = m_priorityEditor->count() - 1;
    }
    m_priorityEditor->setCurrentIndex(priorityIndex);

    m_originalLocationPresent = info.value(QStringLiteral("locationPresent")).toBool();
    m_originalLocation = QJsonObject::fromVariantMap(info.value(QStringLiteral("location")).toMap());
    m_locationEnabled->setChecked(m_originalLocationPresent);
    m_locationLabel->setText(m_originalLocation.value(QStringLiteral("label")).toString());
    m_locationLatitude->setValue(m_originalLocation.value(QStringLiteral("latitude")).toDouble());
    m_locationLongitude->setValue(m_originalLocation.value(QStringLiteral("longitude")).toDouble());

    const QVariantMap linking = info.value(QStringLiteral("linking")).toMap();
    const QString method = linking.value(QStringLiteral("method")).toString();
    m_linkingMethodLabel->setText(method == QLatin1String("saved_pdf_source_map") ? i18n("Saved PDF source map")
        : method == QLatin1String("existing_source_anchor") ? i18n("Existing source anchor")
        : method == QLatin1String("legacy_build_text_fallback") ? i18n("Build-cache lookup → text-search fallback")
        : method == QLatin1String("legacy_build_lookup") ? i18n("Build-cache lookup")
        : method.isEmpty() ? i18n("Not recorded") : method);
    const QString linkingStatus = linking.value(QStringLiteral("status")).toString();
    QString attempt = linkingStatus == QLatin1String("succeeded") ? i18n("Linked")
        : linkingStatus == QLatin1String("needs_attention") || linkingStatus == QLatin1String("failed") ? i18n("Needs attention")
        : linkingStatus == QLatin1String("running") || linkingStatus == QLatin1String("queued") || linkingStatus == QLatin1String("retry") ? i18n("Linking")
        : i18n("No recorded attempt");
    const QDateTime linkedAt = QDateTime::fromString(linking.value(QStringLiteral("attempted_at")).toString(), Qt::ISODateWithMs);
    if (linkedAt.isValid()) attempt = QLocale().toString(linkedAt.toLocalTime(), QLocale::ShortFormat) + QStringLiteral(" — ") + attempt;
    const QString reason = linking.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty()) attempt += QStringLiteral(": ") + reason;
    m_linkingAttemptLabel->setText(attempt);
    m_originalChatGptUrl = info.value(QStringLiteral("chatGptUrl")).toString();
    m_chatGptUrlEditor->setText(m_originalChatGptUrl);
    m_sourcePath = info.value(QStringLiteral("sourcePath")).toString();
    m_sourceLine = info.value(QStringLiteral("sourceLine")).toInt();
    m_relatedStatus->setText(status);
    m_relatedStatus->setVisible(!status.isEmpty());
    m_loadingRelatedInfo = false;
    setRelatedOperationPending(false);
    updateRelatedActions();
}

void AnnotWindow::setThreadActionInfo(const QVariantMap &info)
{
    if (!m_title->threadActionButton()) return;
    m_threadRootId = info.value(QStringLiteral("threadRootId")).toString();
    m_threadNeedsAction = info.value(QStringLiteral("needsAction")).toBool();
    m_title->threadActionButton()->setChecked(m_threadNeedsAction);
    m_title->threadActionButton()->setToolTip(m_threadNeedsAction ? i18n("Remove action requirement") : i18n("Mark as needing action"));
    m_title->threadActionButton()->setEnabled(info.value(QStringLiteral("threadActionAvailable")).toBool() && !m_threadActionPending);
}

void AnnotWindow::completeThreadActionOperation(bool success)
{
    m_threadActionPending = false;
    if (!m_title->threadActionButton()) return;
    m_title->threadActionButton()->setChecked(m_threadNeedsAction);
    if (!success) {
        m_relatedStatus->setText(i18n("The action flag could not be saved locally. Please try again."));
        m_relatedStatus->show();
        m_relatedToggle->setChecked(true);
    }
}

void AnnotWindow::setRelatedOperationPending(bool pending)
{
    m_relatedOperationPending = pending;
    if (pending) {
        m_relatedStatus->setText(i18n("Saving details…"));
        m_relatedStatus->show();
    }
    updateRelatedActions();
}

void AnnotWindow::completeRelatedOperation(bool success, const QString &message)
{
    m_relatedOperationPending = false;
    m_relatedStatus->setText(message);
    m_relatedStatus->setVisible(!message.isEmpty());
    if (!success && m_detachedComment) m_relatedToggle->setChecked(true);
    if (success) {
        m_originalChatGptUrl = m_chatGptUrlEditor->text();
    }
    updateRelatedActions();
}

void AnnotWindow::completeReplyOperation(bool success)
{
    if (!m_threadedCommentUi || !m_replyComposer) {
        return;
    }
    m_replyOperationPending = false;
    m_replyComposer->setEnabled(true);
    if (success) {
        m_replyComposer->clear();
        m_replyComposerPanel->hide();
        m_replyButton->show();
        QTimer::singleShot(0, this, &AnnotWindow::fitThreadToContents);
        QTimer::singleShot(0, m_title, [title = m_title] { title->scrollToLatestThreadMessage(); });
    }
    m_replySendButton->setEnabled(!m_replyComposer->toPlainText().trimmed().isEmpty());
}

bool AnnotWindow::hasUnsavedRelatedChanges() const
{
    QString error;
    return (m_detachedComment && textEdit->toPlainText() != m_annot->contents()) || m_relatedOperationPending || !relatedMetadataPatch(&error).isEmpty() || !error.isEmpty() || m_chatGptUrlEditor->text() != m_originalChatGptUrl;
}

void AnnotWindow::updateRelatedActions()
{
    const bool enabled = m_relatedInfoAvailable && !m_relatedOperationPending;
    QString metadataError;
    const QVariantMap patch = relatedMetadataPatch(&metadataError);
    m_priorityEditor->setEnabled(enabled);
    m_locationEnabled->setEnabled(enabled);
    const bool locationFieldsEnabled = enabled && m_locationEnabled->isChecked();
    m_locationLabel->setEnabled(locationFieldsEnabled);
    m_locationLatitude->setEnabled(locationFieldsEnabled);
    m_locationLongitude->setEnabled(locationFieldsEnabled);
    m_applyRelatedButton->setEnabled(enabled && (!patch.isEmpty() || !metadataError.isEmpty()));
    m_chatGptUrlEditor->setEnabled(enabled);
    m_saveChatGptButton->setEnabled(enabled && m_chatGptUrlEditor->text() != m_originalChatGptUrl);
    m_openChatGptButton->setEnabled(enabled && !m_chatGptUrlEditor->text().trimmed().isEmpty());
    m_openCodexButton->setEnabled(enabled);
    m_openSourceButton->setEnabled(enabled && !m_sourcePath.isEmpty() && m_sourceLine > 0);
    m_openOverviewButton->setEnabled(enabled);
}

int AnnotWindow::pageNumber() const
{
    return m_page;
}

void AnnotWindow::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);

    // focus the content area by default
    textEdit->setFocus();
}

bool AnnotWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == textEdit) {
        if (event->type() == QEvent::ShortcutOverride) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (!m_detachedComment && keyEvent == QKeySequence::Undo) {
                m_document->undo();
                return true;
            } else if (!m_detachedComment && keyEvent == QKeySequence::Redo) {
                m_document->redo();
                return true;
            } else if (keyEvent->key() == Qt::Key_Escape) {
                close();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn) {
            raise();
        }
    }
    return QFrame::eventFilter(watched, event);
}

void AnnotWindow::slotUpdateUndoAndRedoInContextMenu(QMenu *menu)
{
    if (!menu || m_detachedComment) {
        return;
    }

    QList<QAction *> actionList = menu->actions();
    enum { UndoAct, RedoAct, CutAct, CopyAct, PasteAct, ClearAct, SelectAllAct, NCountActs };

    QAction *kundo = KStandardAction::create(
        KStandardAction::Undo,
        m_document,
        [doc = m_document] {
            // We need a QueuedConnection because undoing may end up destroying the menu this action is on
            // because it will undo the addition of the annotation. If it's not queued things gets unhappy
            // because the menu is destroyed in the middle of processing the click on the menu itself
            QMetaObject::invokeMethod(doc, &Okular::Document::undo, Qt::QueuedConnection);
        },
        menu);
    QAction *kredo = KStandardAction::create(KStandardAction::Redo, m_document, SLOT(redo()), menu);
    connect(m_document, &Okular::Document::canUndoChanged, kundo, &QAction::setEnabled);
    connect(m_document, &Okular::Document::canRedoChanged, kredo, &QAction::setEnabled);
    kundo->setEnabled(m_document->canUndo());
    kredo->setEnabled(m_document->canRedo());

    QAction *oldUndo, *oldRedo;
    oldUndo = actionList[UndoAct];
    oldRedo = actionList[RedoAct];

    menu->insertAction(oldUndo, kundo);
    menu->insertAction(oldRedo, kredo);

    menu->removeAction(oldUndo);
    menu->removeAction(oldRedo);
}

void AnnotWindow::slotOptionBtn()
{
    // TODO: call context menu in pageview
    // Q_EMIT sig...
}

void AnnotWindow::slotsaveWindowText()
{
    if (m_detachedRefreshBlocked) return;
    const QString contents = textEdit->toPlainText();
    const int cursorPos = textEdit->textCursor().position();
    if (contents != m_annot->contents()) {
        if (m_detachedComment) {
            Q_EMIT detachedContentsSaveRequested(contents);
        } else {
            m_document->editPageAnnotationContents(m_page, m_annot, contents, cursorPos, m_prevCursorPos, m_prevAnchorPos);
        }
        Q_EMIT containsLatex(GuiUtils::LatexRenderer::mightContainLatex(textEdit->toPlainText()));
    }
    m_prevCursorPos = cursorPos;
    m_prevAnchorPos = textEdit->textCursor().anchor();
    if (m_replyButton) {
        m_replyButton->setEnabled((m_detachedComment || m_document->canModifyPageAnnotation(m_annot)) && !m_annot->uniqueName().isEmpty() && !contents.trimmed().isEmpty());
    }
}

void AnnotWindow::renderLatex(bool render)
{
    if (render) {
        textEdit->setReadOnly(true);
        disconnect(textEdit, &KTextEdit::textChanged, this, &AnnotWindow::slotsaveWindowText);
        disconnect(textEdit, &KTextEdit::cursorPositionChanged, this, &AnnotWindow::slotsaveWindowText);
        textEdit->setAcceptRichText(true);
        QString contents = m_annot->contents();
        contents = Qt::convertFromPlainText(contents);
        QColor fontColor = textEdit->textColor();
        int fontSize = textEdit->fontPointSize();
        QString latexOutput;
        GuiUtils::LatexRenderer::Error errorCode = m_latexRenderer->renderLatexInHtml(contents, fontColor, fontSize, Okular::Utils::realDpi(nullptr).width(), latexOutput);
        switch (errorCode) {
        case GuiUtils::LatexRenderer::LatexNotFound:
            KMessageBox::error(this, i18n("Cannot find latex executable."), i18n("LaTeX rendering failed"));
            m_title->uncheckLatexButton();
            renderLatex(false);
            break;
        case GuiUtils::LatexRenderer::DvipngNotFound:
            KMessageBox::error(this, i18n("Cannot find dvipng executable."), i18n("LaTeX rendering failed"));
            m_title->uncheckLatexButton();
            renderLatex(false);
            break;
        case GuiUtils::LatexRenderer::LatexFailed:
            KMessageBox::detailedError(this, i18n("A problem occurred during the execution of the 'latex' command."), latexOutput, i18n("LaTeX rendering failed"));
            m_title->uncheckLatexButton();
            renderLatex(false);
            break;
        case GuiUtils::LatexRenderer::DvipngFailed:
            KMessageBox::error(this, i18n("A problem occurred during the execution of the 'dvipng' command."), i18n("LaTeX rendering failed"));
            m_title->uncheckLatexButton();
            renderLatex(false);
            break;
        case GuiUtils::LatexRenderer::NoError:
        default:
            textEdit->setHtml(contents);
            break;
        }
    } else {
        textEdit->setAcceptRichText(false);
        textEdit->setPlainText(m_annot->contents());
        connect(textEdit, &KTextEdit::textChanged, this, &AnnotWindow::slotsaveWindowText);
        connect(textEdit, &KTextEdit::cursorPositionChanged, this, &AnnotWindow::slotsaveWindowText);
        textEdit->setReadOnly(false);
    }
}

void AnnotWindow::slotHandleContentsChangedByUndoRedo(Okular::Annotation *annot, const QString &contents, int cursorPos, int anchorPos)
{
    if (annot != m_annot) {
        return;
    }

    textEdit->setPlainText(contents);
    QTextCursor c = textEdit->textCursor();
    c.setPosition(anchorPos);
    c.setPosition(cursorPos, QTextCursor::KeepAnchor);
    m_prevCursorPos = cursorPos;
    m_prevAnchorPos = anchorPos;
    textEdit->setTextCursor(c);
    textEdit->setFocus();
    Q_EMIT containsLatex(GuiUtils::LatexRenderer::mightContainLatex(m_annot->contents()));
}

#include "annotwindow.moc"
