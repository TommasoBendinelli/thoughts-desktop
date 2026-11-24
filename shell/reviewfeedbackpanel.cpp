/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "reviewfeedbackpanel.h"

#include "../part/reviewcardfeedback.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace Thoughts
{
namespace
{
// Feedback whose passage is marked on the PDF with @p sha256.
bool givenOn(const QVariantMap &item, const QString &sha256)
{
    return !sha256.isEmpty() && !item.value(QStringLiteral("deleted")).toBool() && item.value(QStringLiteral("anchor_pdf_sha256")).toString() == sha256
        && !item.value(QStringLiteral("anchor_regions")).toList().isEmpty();
}

// Counts and notes: the text colour, faded.
void makeSecondary(QWidget *widget)
{
    QPalette palette = widget->palette();
    QColor colour = palette.color(QPalette::WindowText);
    colour.setAlphaF(0.6);
    palette.setColor(QPalette::WindowText, colour);
    widget->setPalette(palette);
}

QString rgba(const QColor &colour)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(colour.red()).arg(colour.green()).arg(colour.blue()).arg(colour.alphaF(), 0, 'f', 2);
}

// A version is a rounded card on the panel; each card reviewed on it a row that lights up under
// the mouse when it opens.
QString historyStyleSheet(const QPalette &palette)
{
    QColor line = palette.color(QPalette::WindowText);
    line.setAlphaF(0.16);
    QColor hover = palette.color(QPalette::Highlight);
    hover.setAlphaF(0.12);
    return QStringLiteral(
               "QFrame[historyRole=\"version\"] { background: palette(base); border: 1px solid %1; border-radius: 8px; }"
               "QFrame[historyRole=\"card\"] { background: transparent; border: none; border-radius: 6px; }"
               "QFrame[historyRole=\"card\"][opens=\"true\"]:hover { background: %2; }"
               "QFrame[historyRole=\"focus\"] { background: rgba(255, 200, 0, 0.30); border: none; border-radius: 6px; }"
               "QPushButton[historyRole=\"open\"] { border: none; background: transparent; color: palette(link); padding: 0 2px; }"
               "QPushButton[historyRole=\"open\"]:hover { text-decoration: underline; }")
        .arg(rgba(line), rgba(hover));
}

QLabel *boldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setWordWrap(true);
    return label;
}

// The comments of a card's feedback on a version, one per line, for its row's tooltip.
QString feedbackTooltip(const QList<QVariantMap> &feedback)
{
    QStringList lines;
    for (const QVariantMap &item : feedback) {
        const QString comment = item.value(QStringLiteral("comment")).toString().simplified();
        const QString quote = item.value(QStringLiteral("quote")).toString().simplified();
        const QString polarity = item.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative() ? i18n("Disliked") : i18n("Liked");
        QString line = polarity;
        if (!quote.isEmpty()) line += QStringLiteral(" “") + quote + QStringLiteral("”");
        if (!comment.isEmpty()) line += QStringLiteral(": ") + comment;
        lines.append(line.toHtmlEscaped());
    }
    return lines.join(QStringLiteral("<br>"));
}
}

ReviewFeedbackPanel::ReviewFeedbackPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("reviewFeedbackPanel"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setMinimumWidth(220);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 4, 8);
    layout->setSpacing(6);
    m_title = new QLabel(i18n("History"), this);
    m_title->setObjectName(QStringLiteral("reviewFeedbackPanelTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("reviewHistorySummary"));
    m_summaryLabel->setWordWrap(true);
    makeSecondary(m_summaryLabel);
    layout->addWidget(m_summaryLabel);
    layout->addSpacing(4);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list = new QWidget(m_scroll);
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, 0, 4, 0);
    m_listLayout->setSpacing(10);
    m_scroll->setWidget(m_list);
    layout->addWidget(m_scroll, 1);
}

QSize ReviewFeedbackPanel::sizeHint() const
{
    return {300, QFrame::sizeHint().height()};
}

void ReviewFeedbackPanel::setRefresh(std::function<void()> refresh)
{
    m_refresh = std::move(refresh);
}

void ReviewFeedbackPanel::setHistory(const QList<Version> &versions, const QString &summary)
{
    if (m_built && versions == m_versions && summary == m_summary) return;
    m_versions = versions;
    m_summary = summary;
    rebuild();
}

void ReviewFeedbackPanel::scrollToCard(const QString &cardId)
{
    m_scrollCardId = cardId;
    // After the list is laid out at the panel's width.
    QTimer::singleShot(0, this, &ReviewFeedbackPanel::applyCardScroll);
}

void ReviewFeedbackPanel::applyCardScroll()
{
    if (m_scrollCardId.isEmpty() || !isVisible()) return;
    QFrame *card = nullptr;
    for (QFrame *row : m_list->findChildren<QFrame *>(QStringLiteral("reviewHistoryCard"))) {
        if (row->property("cardId").toString() != m_scrollCardId) continue;
        // The newest version: the topmost.
        if (!card || row->mapTo(m_list, QPoint()).y() < card->mapTo(m_list, QPoint()).y()) card = row;
    }
    if (!card) return;
    m_scrollCardId.clear();
    m_listLayout->activate();
    m_scroll->verticalScrollBar()->setValue(std::max(0, card->mapTo(m_list, QPoint()).y() - 12));
    card->setProperty("historyRole", QStringLiteral("focus"));
    card->style()->unpolish(card);
    card->style()->polish(card);
    QPointer<QFrame> guarded(card);
    QTimer::singleShot(1600, this, [guarded] {
        if (!guarded) return;
        guarded->setProperty("historyRole", QStringLiteral("card"));
        guarded->style()->unpolish(guarded);
        guarded->style()->polish(guarded);
    });
}

void ReviewFeedbackPanel::setMaskedCard(const QString &cardId)
{
    if (cardId == m_maskedCardId) return;
    m_maskedCardId = cardId;
    if (m_built) rebuild();
}

void ReviewFeedbackPanel::rebuild()
{
    m_built = true;
    while (QLayoutItem *child = m_listLayout->takeAt(0)) {
        if (QWidget *row = child->widget()) {
            // Gone from the panel now, deleted at the next turn of the event
            // loop: a row can be the button whose click led here, or the focus
            // widget a caller still holds (Shell::setActiveTab).
            row->hide();
            row->setParent(nullptr);
            row->deleteLater();
        }
        delete child;
    }
    m_list->setStyleSheet(historyStyleSheet(palette()));
    m_summaryLabel->setText(m_summary);
    m_summaryLabel->setVisible(!m_summary.isEmpty());
    const auto note = [](const QString &text, QWidget *parent) {
        auto *label = new QLabel(text, parent);
        label->setWordWrap(true);
        makeSecondary(label);
        return label;
    };
    for (const Version &version : std::as_const(m_versions)) {
        auto *block = new QFrame(m_list);
        block->setObjectName(QStringLiteral("reviewHistoryVersion-") + (version.sha256.isEmpty() ? QStringLiteral("unrecorded") : version.sha256));
        block->setProperty("historyRole", QStringLiteral("version"));
        auto *blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(8, 8, 8, 8);
        blockLayout->setSpacing(2);
        const QString when = version.firstAt.isValid() ? version.firstAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
        QString title = version.sha256.isEmpty() ? i18n("Earlier (version not recorded)")
            : version.current                    ? i18n("Current version")
            : when.isEmpty()                     ? i18n("Earlier version")
                                                 : i18n("Version of %1", when);
        if (version.shown && !version.current) title = i18n("%1 · open here", title);
        // Its title, and for another version than this tab's "Open ›" on the same line.
        auto *header = new QHBoxLayout;
        header->setContentsMargins(4, 0, 0, 4);
        header->setSpacing(6);
        auto *heading = boldLabel(title, block);
        heading->setObjectName(QStringLiteral("reviewHistoryVersionTitle"));
        header->addWidget(heading, 1);
        const bool opens = !version.sha256.isEmpty();
        if (opens && !version.shown) {
            // The passages given on it marked, else its pages.
            QVariantList regions;
            for (const Entry &entry : version.entries) {
                if (givenOn(entry.feedback, version.sha256)) regions += entry.feedback.value(QStringLiteral("anchor_regions")).toList();
            }
            for (const QVariantMap &comment : version.comments) regions += comment.value(QStringLiteral("regions")).toList();
            auto *open = new QPushButton(i18n("Open ›"), block);
            open->setObjectName(QStringLiteral("reviewHistoryOpenVersion"));
            open->setProperty("historyRole", QStringLiteral("open"));
            open->setFlat(true);
            open->setCursor(Qt::PointingHandCursor);
            open->setToolTip(version.current ? i18n("Open the current version of the PDF") : i18n("Open this version of the PDF, with the passages that got feedback on it marked"));
            header->addWidget(open, 0, Qt::AlignRight | Qt::AlignTop);
            connect(open, &QPushButton::clicked, this, [this, sha256 = version.sha256, at = version.firstAt, regions] { Q_EMIT versionRequested(sha256, at, regions); });
        }
        blockLayout->addLayout(header);
        // The cards reviewed on it, in the order of the entries (reading order).
        struct Card {
            QString id;
            QString question;
            QList<QPair<int, QRectF>> answer;
            int reviews = 0;
            QList<QVariantMap> feedback;
            int carried = 0; // Of feedback: given on an earlier version, found here.
        };
        QList<Card> cards;
        int unreviewed = 0;
        for (const Entry &entry : version.entries) {
            if (entry.cardId.isEmpty()) continue;
            auto card = std::find_if(cards.begin(), cards.end(), [&entry](const Card &candidate) { return candidate.id == entry.cardId; });
            if (card == cards.end()) {
                cards.append(Card{entry.cardId, entry.cardQuestion.simplified(), entry.cardAnswer, 0, {}, 0});
                card = cards.end() - 1;
            }
            if (!entry.feedback.isEmpty()) {
                if (!entry.feedback.value(QStringLiteral("deleted")).toBool()) card->feedback.append(entry.feedback);
                if (entry.feedback.value(QStringLiteral("carried")).toBool()) ++card->carried;
            } else if (!entry.html.isEmpty()) {
                ++card->reviews;
            }
        }
        cards.removeIf([&unreviewed](const Card &card) {
            const bool empty = card.reviews == 0 && card.feedback.isEmpty();
            if (empty) ++unreviewed;
            return empty;
        });
        // The comments made on it, oldest first.
        for (const QVariantMap &comment : version.comments) {
            const QString kind = comment.value(QStringLiteral("kind")).toString();
            const QString kindText = kind == QLatin1String("like") ? i18n("Like") : kind == QLatin1String("dislike") ? i18n("Dislike")
                : kind == QLatin1String("question") ? i18n("Question") : kind == QLatin1String("drawing") ? i18n("Drawing")
                : kind == QLatin1String("text") ? i18n("Text box") : i18n("Comment");
            const QString written = comment.value(QStringLiteral("comment")).toString().simplified();
            // A drawing or a text box has no passage: its quote is a placeholder.
            const QString quote = kind == QLatin1String("drawing") || kind == QLatin1String("text") ? QString() : comment.value(QStringLiteral("quote")).toString().simplified();
            auto *row = new QFrame(block);
            row->setObjectName(QStringLiteral("reviewHistoryComment"));
            row->setProperty("historyRole", QStringLiteral("card"));
            row->setProperty("feedbackId", comment.value(QStringLiteral("feedback_id")));
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(4, 4, 4, 4);
            rowLayout->setSpacing(6);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(0);
            // What was written, else the passage it is about, else where it is.
            const QVariantList regions = comment.value(QStringLiteral("regions")).toList();
            const int page = regions.isEmpty() ? 0 : regions.constFirst().toMap().value(QStringLiteral("page")).toInt();
            const QString where = page > 0 ? i18n("%1 on page %2", kindText, page) : kindText;
            auto *text = new QLabel(!written.isEmpty() ? written : !quote.isEmpty() ? QStringLiteral("“%1”").arg(quote) : where, row);
            text->setObjectName(QStringLiteral("reviewHistoryCommentText"));
            text->setTextFormat(Qt::PlainText);
            text->setWordWrap(true);
            texts->addWidget(text);
            QStringList details{kindText};
            if (!written.isEmpty() && !quote.isEmpty()) details.append(QStringLiteral("“%1”").arg(quote.size() > 60 ? quote.left(59) + QStringLiteral("…") : quote));
            if (comment.value(QStringLiteral("resolved")).toBool()) details.append(i18n("Resolved"));
            if (const QString page = comment.value(QStringLiteral("html_page")).toString(); !page.isEmpty()) details.append(i18n("HTML page %1", page));
            auto *detail = note(details.join(QStringLiteral(" · ")), row);
            detail->setObjectName(QStringLiteral("reviewHistoryCommentKind"));
            texts->addWidget(detail);
            rowLayout->addLayout(texts, 1);
            const bool rowOpens = opens && (!version.shown || !regions.isEmpty());
            row->setProperty("opens", rowOpens);
            if (rowOpens) {
                rowLayout->addWidget(note(QStringLiteral("›"), row), 0, Qt::AlignVCenter);
                row->setProperty("sha256", version.sha256);
                row->setProperty("shown", version.shown);
                row->setProperty("firstAt", version.firstAt);
                row->setProperty("regions", regions);
                row->setCursor(Qt::PointingHandCursor);
                row->installEventFilter(this);
                for (QWidget *child : row->findChildren<QWidget *>()) child->installEventFilter(this);
            }
            QString tooltip = kindText;
            if (!written.isEmpty()) tooltip += QStringLiteral(": ") + written;
            if (!quote.isEmpty()) tooltip += QStringLiteral("\n“%1”").arg(quote);
            if (rowOpens) tooltip += QStringLiteral("\n\n") + (version.shown ? i18n("Show it in the PDF") : i18n("Open this version of the PDF at it"));
            row->setToolTip(tooltip);
            blockLayout->addWidget(row);
        }
        if (cards.isEmpty() && version.comments.isEmpty()) {
            auto *empty = note(unreviewed ? i18n("No card reviewed on this version yet.") : i18n("Nothing recorded on this version yet."), block);
            empty->setObjectName(QStringLiteral("reviewHistoryEmpty"));
            empty->setContentsMargins(4, 0, 0, 0);
            blockLayout->addWidget(empty);
            unreviewed = 0;
        }
        for (const Card &card : std::as_const(cards)) {
            const bool masked = card.id == m_maskedCardId;
            auto *row = new QFrame(block);
            row->setObjectName(QStringLiteral("reviewHistoryCard"));
            row->setProperty("historyRole", QStringLiteral("card"));
            row->setProperty("cardId", card.id);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(4, 4, 4, 4);
            rowLayout->setSpacing(6);
            auto *texts = new QVBoxLayout;
            texts->setSpacing(0);
            auto *question = new QLabel(card.question.isEmpty() ? i18n("Card") : card.question, row);
            question->setObjectName(QStringLiteral("reviewHistoryCardQuestion"));
            question->setTextFormat(Qt::PlainText);
            question->setWordWrap(true);
            texts->addWidget(question);
            QStringList counts;
            if (card.reviews) counts.append(i18np("1 review", "%1 reviews", card.reviews));
            if (const int given = card.feedback.size() - card.carried) counts.append(i18np("1 feedback", "%1 feedback", given));
            if (card.carried) counts.append(i18np("1 feedback from an earlier version", "%1 feedback from earlier versions", card.carried));
            auto *count = note(counts.join(QStringLiteral(" · ")), row);
            count->setObjectName(QStringLiteral("reviewHistoryCardCounts"));
            texts->addWidget(count);
            rowLayout->addLayout(texts, 1);
            // Its answer in this version, else the passages given on it, else the version alone.
            QVariantList regions;
            if (!card.answer.isEmpty()) {
                regions = ReviewCardFeedback::regionsJson(card.answer).toVariantList();
            } else {
                for (const QVariantMap &item : card.feedback)
                    if (givenOn(item, version.sha256)) regions += item.value(QStringLiteral("anchor_regions")).toList();
            }
            // This tab's version needs a passage to show; another one opens anyway.
            const bool rowOpens = opens && !masked && (!version.shown || !regions.isEmpty());
            row->setProperty("opens", rowOpens);
            if (rowOpens) {
                auto *chevron = note(QStringLiteral("›"), row);
                rowLayout->addWidget(chevron, 0, Qt::AlignVCenter);
                row->setProperty("sha256", version.sha256);
                row->setProperty("shown", version.shown);
                row->setProperty("firstAt", version.firstAt);
                row->setProperty("regions", regions);
                row->setCursor(Qt::PointingHandCursor);
                row->installEventFilter(this);
                for (QWidget *child : row->findChildren<QWidget *>()) child->installEventFilter(this);
            }
            const QString comments = masked ? QString() : feedbackTooltip(card.feedback);
            const QString action = masked    ? i18n("Opens once you reveal its answer in the review.")
                : !rowOpens                  ? QString()
                : version.shown              ? i18n("Show its answer in the PDF")
                                             : i18n("Open this version of the PDF at its answer");
            QString tooltip = card.question.toHtmlEscaped();
            if (!comments.isEmpty()) tooltip += QStringLiteral("<br><br>") + comments;
            if (!action.isEmpty()) tooltip += QStringLiteral("<br><br><i>") + action.toHtmlEscaped() + QStringLiteral("</i>");
            row->setToolTip(tooltip);
            blockLayout->addWidget(row);
        }
        if (unreviewed) {
            auto *others = note(i18np("1 more card not reviewed on this version", "%1 more cards not reviewed on this version", unreviewed), block);
            others->setObjectName(QStringLiteral("reviewHistoryUnreviewed"));
            others->setContentsMargins(4, 2, 0, 0);
            blockLayout->addWidget(others);
        }
        m_listLayout->addWidget(block);
    }
    m_listLayout->addStretch(1);
    if (!m_scrollCardId.isEmpty()) QTimer::singleShot(0, this, &ReviewFeedbackPanel::applyCardScroll);
}

bool ReviewFeedbackPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        for (QObject *object = watched; object && object != m_list; object = object->parent()) {
            if (!object->property("opens").toBool()) continue;
            const QVariantList regions = object->property("regions").toList();
            if (object->property("shown").toBool()) Q_EMIT shownPassageRequested(regions);
            else Q_EMIT versionRequested(object->property("sha256").toString(), object->property("firstAt").toDateTime(), regions);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ReviewFeedbackPanel::showEvent(QShowEvent *event)
{
    if (m_refresh) m_refresh();
    QFrame::showEvent(event);
    if (!m_scrollCardId.isEmpty()) QTimer::singleShot(0, this, &ReviewFeedbackPanel::applyCardScroll);
}
}
