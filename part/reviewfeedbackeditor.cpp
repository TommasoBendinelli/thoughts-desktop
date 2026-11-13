// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewfeedbackeditor.h"

#include "documentfeedback.h"
#include "reviewcardfeedback.h"
#include "reviewfeedbackoverlay.h"

#include <KLocalizedString>

#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QDateTime>
#include <QFontMetrics>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace Thoughts
{
namespace
{
QString dateText(const QString &value)
{
    QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::fromString(value, Qt::ISODate);
    return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : value;
}
}

QString ReviewFeedbackEditor::elidedLines(const QString &text, const QFont &font, int width, int lines)
{
    const QString simplified = text.simplified();
    if (width <= 0 || lines <= 0) return simplified;
    QTextLayout layout(simplified, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QList<int> starts;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        starts.append(line.textStart());
    }
    layout.endLayout();
    if (starts.size() <= lines) return simplified;
    // The first lines as they are; the last one holds the rest, cut with "…".
    const QString head = simplified.left(starts.at(lines - 1));
    return head + QFontMetrics(font).elidedText(simplified.mid(starts.at(lines - 1)), Qt::ElideRight, width);
}

ReviewFeedbackEditor::ReviewFeedbackEditor(const QJsonObject &record, bool isNew, QWidget *parent, bool kinds)
    : QFrame(parent, Qt::Popup)
    , m_record(record)
    , m_new(isNew)
{
    setObjectName(QStringLiteral("reviewFeedbackEditor"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(340);
    setMaximumWidth(460);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    const QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString();
    m_quote = new QLabel(this);
    m_quote->setObjectName(QStringLiteral("reviewFeedbackQuote"));
    m_quote->setWordWrap(true);
    m_quote->setToolTip(quote.simplified());
    QFont quoteFont = m_quote->font();
    quoteFont.setItalic(true);
    m_quote->setFont(quoteFont);
    m_quote->setText(QStringLiteral("“%1”").arg(elidedLines(quote, quoteFont, 420, 3)));
    layout->addWidget(m_quote);

    auto *polarityRow = new QHBoxLayout;
    polarityRow->setSpacing(6);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    const auto addChoice = [&](QToolButton *&slot, const QString &name, const QString &text, const QString &choice) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setText(text);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setMinimumWidth(kinds ? 70 : 84);
        // The chosen one in its highlight colour (review-card-feedback.md §6.3).
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor accent = ReviewFeedbackOverlay::colour(choice, dark);
        button->setStyleSheet(QStringLiteral("QToolButton { padding: 3px 10px; border: 1px solid palette(mid); border-radius: 4px; }"
                                             "QToolButton:checked { border: 2px solid %1; background: rgba(%2, %3, %4, %5); font-weight: 600; }")
                                  .arg(accent.name()).arg(accent.red()).arg(accent.green()).arg(accent.blue()).arg(dark ? 0.28 : 0.22));
        group->addButton(button);
        polarityRow->addWidget(button);
        slot = button;
    };
    addChoice(m_like, QStringLiteral("reviewFeedbackLike"), i18n("Like"), ReviewCardFeedback::positive());
    addChoice(m_dislike, QStringLiteral("reviewFeedbackDislike"), i18n("Dislike"), ReviewCardFeedback::negative());
    if (kinds) {
        addChoice(m_question, QStringLiteral("reviewFeedbackQuestion"), i18n("Question"), ReviewCardFeedback::neutral());
        addChoice(m_other, QStringLiteral("reviewFeedbackOther"), i18n("Comment"), ReviewCardFeedback::neutral());
    }
    polarityRow->addStretch(1);
    layout->addLayout(polarityRow);
    const bool negative = record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative();
    const QString kind = DocumentFeedback::kindOf(record);
    (kind == DocumentFeedback::question() ? m_question : kind == DocumentFeedback::other() ? m_other : negative ? m_dislike : m_like)->setChecked(true);
    // A drawing stays a drawing and a text box a text box: there is no passage to quote and no
    // other kind to choose.
    if (DocumentFeedback::isPageMark(record)) {
        m_quote->hide();
        for (QToolButton *choice : {m_like, m_dislike, m_question, m_other}) {
            if (choice) choice->hide();
        }
    }

    m_comment = new QPlainTextEdit(this);
    m_comment->setObjectName(QStringLiteral("reviewFeedbackComment"));
    m_comment->setPlaceholderText(DocumentFeedback::isTextBox(record) ? i18n("The text on the page")
                                  : kinds ? i18n("Your comment") : i18n("What do you like or dislike here?"));
    m_comment->setPlainText(record.value(QStringLiteral("comment")).toString());
    m_comment->setTabChangesFocus(true);
    m_comment->setFixedHeight(m_comment->fontMetrics().lineSpacing() * 4 + 14);
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this] {
        // At most 4000 characters, as the server accepts.
        const QString text = m_comment->toPlainText();
        if (text.toUcs4().size() > ReviewCardFeedback::maximumComment) {
            const QSignalBlocker blocker(m_comment);
            const auto ucs4 = text.toUcs4().mid(0, ReviewCardFeedback::maximumComment);
            m_comment->setPlainText(QString::fromUcs4(reinterpret_cast<const char32_t *>(ucs4.constData()), ucs4.size()));
            m_comment->moveCursor(QTextCursor::End);
        }
    });
    layout->addWidget(m_comment);

    if (!isNew) {
        const QString created = record.value(QStringLiteral("created_at")).toString();
        const QString updated = record.value(QStringLiteral("updated_at")).toString();
        QString dates = i18n("Created %1", dateText(created));
        if (!updated.isEmpty() && dateText(updated) != dateText(created)) dates += QStringLiteral(", ") + i18n("edited %1", dateText(updated));
        auto *datesLabel = new QLabel(dates, this);
        datesLabel->setObjectName(QStringLiteral("reviewFeedbackDates"));
        QPalette palette = datesLabel->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
        datesLabel->setPalette(palette);
        layout->addWidget(datesLabel);
    }

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    if (!isNew) {
        auto *remove = new QPushButton(i18n("Delete"), this);
        remove->setObjectName(QStringLiteral("reviewFeedbackDelete"));
        remove->setAutoDefault(false);
        connect(remove, &QPushButton::clicked, this, [this] {
            m_finished = true;
            Q_EMIT deleteRequested();
            close();
        });
        buttons->addWidget(remove);
        if (DocumentFeedback::canResolve(record)) {
            auto *resolve = new QPushButton(DocumentFeedback::isResolved(record) ? i18n("Reopen") : i18n("Resolve"), this);
            resolve->setObjectName(QStringLiteral("reviewFeedbackResolve"));
            resolve->setAutoDefault(false);
            connect(resolve, &QPushButton::clicked, this, [this] {
                keep();
                Q_EMIT resolveRequested();
                close();
            });
            buttons->addWidget(resolve);
        }
    }
    if (!isNew) {
        m_changePassage = new QPushButton(i18n("Change passage"), this);
        m_changePassage->setObjectName(QStringLiteral("reviewFeedbackChangePassage"));
        m_changePassage->setAutoDefault(false);
        m_changePassage->hide();
        connect(m_changePassage, &QPushButton::clicked, this, [this] {
            m_finished = true;
            const QString chosenPolarity = polarity(), chosenKind = this->kind(), chosenComment = comment();
            close();
            Q_EMIT changePassageRequested(chosenPolarity, chosenKind, chosenComment);
        });
        buttons->addWidget(m_changePassage);
    }
    // No Cancel: nothing written is ever thrown away. Clicking elsewhere and Esc keep the text,
    // and only Delete removes a comment. A new one has no buttons at all.
    buttons->addStretch(1);
    if (buttons->count() > 1) {
        layout->addLayout(buttons);
    } else {
        delete buttons;
    }
    m_comment->setFocus();
}

void ReviewFeedbackEditor::setChangePassageAvailable(bool available)
{
    // A drawing or a text box has no passage.
    if (m_changePassage) m_changePassage->setVisible(available && !DocumentFeedback::isPageMark(m_record));
}

QString ReviewFeedbackEditor::kind() const
{
    if (DocumentFeedback::isPageMark(m_record)) return DocumentFeedback::kindOf(m_record);
    if (m_question && m_question->isChecked()) return DocumentFeedback::question();
    return m_other && m_other->isChecked() ? DocumentFeedback::other() : QString();
}

QString ReviewFeedbackEditor::polarity() const
{
    if (!kind().isEmpty()) return ReviewCardFeedback::neutral();
    return m_dislike->isChecked() ? ReviewCardFeedback::negative() : ReviewCardFeedback::positive();
}

QString ReviewFeedbackEditor::comment() const
{
    return ReviewCardFeedback::cleanComment(m_comment->toPlainText());
}

QString ReviewFeedbackEditor::feedbackId() const
{
    return m_record.value(QStringLiteral("feedback_id")).toString();
}

bool ReviewFeedbackEditor::isNew() const
{
    return m_new;
}

bool ReviewFeedbackEditor::changed() const
{
    return m_new || kind() != DocumentFeedback::kindOf(m_record) || polarity() != m_record.value(QStringLiteral("polarity")).toString() || comment() != m_record.value(QStringLiteral("comment")).toString();
}

void ReviewFeedbackEditor::keep()
{
    if (std::exchange(m_finished, true)) return;
    // A new one with nothing written is nothing to keep, and a question, a comment or a text box is its text.
    if (comment().isEmpty() && (m_new || (!kind().isEmpty() && !DocumentFeedback::isDrawing(m_record)))) return;
    if (changed()) Q_EMIT saveRequested(polarity(), comment());
}

void ReviewFeedbackEditor::hideEvent(QHideEvent *event)
{
    // However it goes away: a click elsewhere, Esc, another application.
    keep();
    QFrame::hideEvent(event);
}

void ReviewFeedbackEditor::mousePressEvent(QMouseEvent *event)
{
    // One click opens it, so the second click of a double click must not close it again.
    if (!rect().contains(event->position().toPoint()) && m_shown.isValid() && m_shown.elapsed() < QApplication::doubleClickInterval()) {
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ReviewFeedbackEditor::popupAt(const QRect &anchor)
{
    adjustSize();
    QPoint position(anchor.left(), anchor.bottom() + 6);
    const QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (position.y() + height() > available.bottom()) position.setY(anchor.top() - height() - 6);
        position.setX(std::clamp(position.x(), available.left(), std::max(available.left(), available.right() - width())));
        position.setY(std::clamp(position.y(), available.top(), std::max(available.top(), available.bottom() - height())));
    }
    move(position);
    show();
    m_shown.start();
    m_comment->setFocus();
}
}
