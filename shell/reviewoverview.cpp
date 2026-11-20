/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "reviewoverview.h"
#include "recallpredictorconfig.h"
#include "../core/htmlattachment.h"
#include "rowbrowsertree.h"
#include "reviewdocumenttools.h"
#include "../part/agentpromptsettings.h"
#include "reviewcardeditor.h"
#include "reviewcardmerge.h"
#include "readerdisplaytitle.h"
#include "developerlog.h"
#include "../part/libraryprojectstore.h"
#include "../part/sourceannotationindex.h"
#include "../part/reviewcardfeedback.h"
#include "../part/documentfeedback.h"
#include "../part/documentidentity.h"
#include "../part/feedbackversion.h"
#include "../part/pdfversions.h"
#include "../part/agentconversationopener.h"
#include "../part/agentconversations.h"
#include "../part/thoughtsplatform.h"
#include "reviewfeedbackpanel.h"
#include <poppler-qt6.h>

#include <KConfigGroup>
#include <KGuiItem>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <KStandardGuiItem>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QCloseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QRadioButton>
#include <QDateEdit>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCursor>
#include <QDesktopServices>
#include <QEventLoop>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QFileDialog>
#include <QFrame>
#include <QThread>
#include <QThreadPool>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <functional>
#include <memory>
#include <optional>
#include <QFormLayout>
#include <QGridLayout>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QShortcut>
#include <QStandardPaths>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QButtonGroup>
#include <QStyleOptionButton>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextList>
#include <QAbstractTextDocumentLayout>
#include <QToolButton>
#include <QToolTip>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QRegularExpression>

#include <algorithm>
#include <numeric>
#ifndef Q_OS_WIN
#include <cerrno>
#include <signal.h>
#endif

namespace Thoughts
{
namespace
{
// A child of the reader widget hosting the review panel, created before the
// panel joins it: Qt deletes a widget's children in order, so a closing tab or
// window deletes this guard first, and the panel is handed back before Qt
// would delete it with the host.
class SessionHostGuard : public QObject
{
public:
    SessionHostGuard(QObject *host, std::function<void()> release)
        : QObject(host)
        , m_release(std::move(release))
    {
    }
    ~SessionHostGuard() override
    {
        if (m_release) {
            m_release();
        }
    }

private:
    std::function<void()> m_release;
};


// Keep row objects stable across filtering and order them by the computed rank.
class ReviewBrowserItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        return data(0, Qt::UserRole + 1).toInt() < other.data(0, Qt::UserRole + 1).toInt();
    }
};

// Review Card browser column widths are user-controlled. They live in their own
// config group so they survive restarts independently of the parent
// CommentsOverview window state.
QString reviewCardOverviewConfigGroup()
{
    return QStringLiteral("ReviewCardOverview");
}

QString reviewBrowseHeaderStateKey()
{
    return QStringLiteral("BrowseHeaderStateV2");
}

QJsonObject primaryObject(const ReviewCard &card)
{
    return card.record.value(QStringLiteral("primary")).toObject();
}

QString intervalLabel(const ReviewInterval &interval)
{
    if (interval.date.isValid()) return QLocale().toString(interval.date, QLocale::ShortFormat);
    QString amount = QLocale().toString(interval.amount, 'f', 6);
    while (amount.endsWith(QLatin1Char('0'))) amount.chop(1);
    if (amount.endsWith(QLocale().decimalPoint())) amount.chop(QLocale().decimalPoint().size());
    switch (interval.unit) {
    case ReviewInterval::Minutes: return interval.amount == 1 ? i18n("%1 minute", amount) : i18n("%1 minutes", amount);
    case ReviewInterval::Hours: return interval.amount == 1 ? i18n("%1 hour", amount) : i18n("%1 hours", amount);
    case ReviewInterval::Days: return interval.amount == 1 ? i18n("%1 day", amount) : i18n("%1 days", amount);
    case ReviewInterval::Weeks: return interval.amount == 1 ? i18n("%1 week", amount) : i18n("%1 weeks", amount);
    case ReviewInterval::Months: return interval.amount == 1 ? i18n("%1 month", amount) : i18n("%1 months", amount);
    }
    return {};
}

QString reviewTagLabel(const QString &tag)
{
    if (tag == ReviewTags::correct()) return i18n("Correct");
    if (tag == ReviewTags::wrong()) return i18n("Wrong");
    if (tag == ReviewTags::skip()) return i18n("Skip");
    return tag;
}

// The tags a card answered with @p tags is counted under, as ReviewStore::reviewTagUsage()
// reads them back: normalized, Skip left out, merged names, each once.
QStringList countedReviewTags(const QStringList &tags, const QHash<QString, QString> &merges)
{
    return ReviewTags::applyMerges(ReviewTags::ofEvent({{QStringLiteral("review_tags"), QJsonArray::fromStringList(tags)}}), merges);
}

QString reviewTagLabels(const QStringList &tags)
{
    QStringList labels;
    for (const QString &tag : tags) labels.append(reviewTagLabel(tag));
    return labels.join(QStringLiteral(", "));
}

// Round, toggleable tag chips, shared by the answer dialog and the card browser.
QString reviewTagChipStyle()
{
    return QStringLiteral(
        "QPushButton { border: 1px solid palette(mid); border-radius: 11px; padding: 3px 12px; min-height: 16px; background: transparent; }"
        "QPushButton:hover { background: palette(midlight); }"
        "QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }");
}

// Search tokens such as tag:Correct or tag:"paper submitted" pick cards by their current tags.
const QRegularExpression &browseTagToken()
{
    static const QRegularExpression token(QStringLiteral("(?:^|\\s)tag:(?:\"([^\"]*)\"|(\\S+))"), QRegularExpression::CaseInsensitiveOption);
    return token;
}

QString browseTagTokenValue(const QRegularExpressionMatch &match)
{
    return match.captured(1).isNull() ? match.captured(2) : match.captured(1);
}

QString browseTagTokenText(const QString &label)
{
    QString value = label;
    value.remove(QLatin1Char('"'));
    return QStringLiteral("tag:") + (value.contains(QRegularExpression(QStringLiteral("\\s"))) ? QLatin1Char('"') + value + QLatin1Char('"') : value);
}

// A typed tag; an old name that was merged becomes the tag it was merged into.
QString normalizeReviewTag(const QString &text, const QHash<QString, QString> &tagMerges = {})
{
    QHash<QString, QString> labels;
    for (const QString &tag : ReviewTags::builtIn()) labels.insert(tag, reviewTagLabel(tag));
    const QString tag = ReviewTags::normalize(text, labels);
    return tag.isEmpty() ? tag : ReviewTags::resolve(tag, tagMerges);
}

// A token names a tag by its stored name or by the label shown for it, or by an old name merged into it.
bool browseTagTokenMatches(const QString &token, const QString &tag, const QHash<QString, QString> &tagMerges = {})
{
    return token.compare(tag, Qt::CaseInsensitive) == 0 || token.compare(reviewTagLabel(tag), Qt::CaseInsensitive) == 0
        || (!tagMerges.isEmpty() && normalizeReviewTag(token, tagMerges) == tag);
}

// Tags created on this computer before their first use; used tags come from review history.
QStringList createdReviewTags(const QHash<QString, QString> &tagMerges = {})
{
    return ReviewTags::applyMerges(KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("ReviewTags")).readEntry("Created", QStringList()), tagMerges);
}

void rememberReviewTag(const QString &tag)
{
    if (ReviewTags::builtIn().contains(tag)) return;
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("ReviewTags"));
    QStringList created = group.readEntry("Created", QStringList());
    if (created.contains(tag)) return;
    created.append(tag);
    group.writeEntry("Created", created);
    group.sync();
}

// Outlined cells; Correct and Wrong keep a faint wash so they read at a glance.
QString reviewCellStyle(const QString &tag)
{
    const QColor tint = tag == ReviewTags::correct() ? QColor(45, 133, 79) : tag == ReviewTags::wrong() ? QColor(188, 77, 69) : QColor();
    if (!tint.isValid()) {
        return QStringLiteral("QPushButton { background: transparent; border: 1px solid palette(mid); border-radius: 10px; padding: 6px 10px; }"
                              "QPushButton:hover { background: palette(midlight); }"
                              "QPushButton:disabled { color: palette(placeholder-text); }");
    }
    return QStringLiteral("QPushButton { background: rgba(%1, %2, %3, 40); border: 1px solid rgba(%1, %2, %3, 150); border-radius: 10px; padding: 6px 10px; }"
                          "QPushButton:hover { background: rgba(%1, %2, %3, 70); }"
                          "QPushButton:disabled { color: palette(placeholder-text); }")
        .arg(tint.red()).arg(tint.green()).arg(tint.blue());
}

QString recordedIntervalLabel(const QJsonObject &event)
{
    const QString unit = event.value(QStringLiteral("interval_unit")).toString();
    if (unit == QLatin1String("never")) return i18n("Never");
    if (unit == QLatin1String("date")) {
        const QDate date = QDate::fromString(event.value(QStringLiteral("interval_date")).toString(), Qt::ISODate);
        return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) : QString();
    }
    const double amount = event.value(QStringLiteral("interval_amount")).toDouble();
    for (const auto &candidate : {qMakePair(QStringLiteral("minutes"), ReviewInterval::Minutes),
                                  qMakePair(QStringLiteral("hours"), ReviewInterval::Hours),
                                  qMakePair(QStringLiteral("days"), ReviewInterval::Days),
                                  qMakePair(QStringLiteral("weeks"), ReviewInterval::Weeks),
                                  qMakePair(QStringLiteral("months"), ReviewInterval::Months)}) {
        if (unit == candidate.first && amount > 0) {
            // Older reviews recorded long intervals in the hours they were suggested in.
            if (candidate.second == ReviewInterval::Hours) return intervalLabel(ReviewInterval::readable(amount * 3600000.0));
            return intervalLabel({amount, candidate.second, {}});
        }
    }
    return {};
}

QString currentIntervalLabel(const ReviewCard &card)
{
    if (card.schedule.retained) return i18n("Never");
    if (!card.history.isEmpty() && card.history.constLast().value(QStringLiteral("interval_action")).toString() == QLatin1String("set")) {
        const QString label = recordedIntervalLabel(card.history.constLast());
        if (!label.isEmpty()) return label;
    }
    if (card.schedule.lastReviewedAt.isValid() && card.schedule.dueAt > card.schedule.lastReviewedAt) {
        return intervalLabel(ReviewInterval::between(card.schedule.lastReviewedAt, card.schedule.dueAt));
    }
    if (card.schedule.intervalDays > 0) {
        const bool weeks = card.schedule.intervalDays % 7 == 0;
        return intervalLabel({double(weeks ? card.schedule.intervalDays / 7 : card.schedule.intervalDays),
                              weeks ? ReviewInterval::Weeks : ReviewInterval::Days, {}});
    }
    return i18n("No interval selected");
}

ReviewInterval currentIntervalChoice(const ReviewCard &card)
{
    if (!card.history.isEmpty()) {
        const auto last = card.history.constLast();
        if (last.value(QStringLiteral("interval_action")).toString() == QLatin1String("set")) {
            const QString unit = last.value(QStringLiteral("interval_unit")).toString();
            const double amount = last.value(QStringLiteral("interval_amount")).toDouble();
            for (const auto &candidate : {qMakePair(QStringLiteral("minutes"), ReviewInterval::Minutes),
                                          qMakePair(QStringLiteral("hours"), ReviewInterval::Hours),
                                          qMakePair(QStringLiteral("days"), ReviewInterval::Days),
                                          qMakePair(QStringLiteral("weeks"), ReviewInterval::Weeks),
                                          qMakePair(QStringLiteral("months"), ReviewInterval::Months)}) {
                if (unit == candidate.first && amount > 0) return {amount, candidate.second, {}};
            }
        }
    }
    if (card.schedule.lastReviewedAt.isValid() && card.schedule.dueAt > card.schedule.lastReviewedAt && !card.schedule.retained) {
        return ReviewInterval::between(card.schedule.lastReviewedAt, card.schedule.dueAt);
    }
    if (card.schedule.intervalDays > 0) {
        const bool weeks = card.schedule.intervalDays % 7 == 0;
        return {double(weeks ? card.schedule.intervalDays / 7 : card.schedule.intervalDays),
                weeks ? ReviewInterval::Weeks : ReviewInterval::Days, {}};
    }
    return {2, ReviewInterval::Weeks, {}};
}

class ReviewAmountSpinBox : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    QString textFromValue(double value) const override
    {
        QString text = QDoubleSpinBox::textFromValue(value);
        if (text.contains(locale().decimalPoint())) {
            while (text.endsWith(QLatin1Char('0'))) text.chop(1);
            if (text.endsWith(locale().decimalPoint())) text.chop(locale().decimalPoint().size());
        }
        return text;
    }
};

// One line of text cut with "…" to the width it is given; the full text is kept.
class ReviewElidedLabel : public QLabel
{
public:
    explicit ReviewElidedLabel(QWidget *parent)
        : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setFullText(const QString &text)
    {
        m_full = text;
        setProperty("fullText", text);
        // The width it asks for follows the text.
        updateGeometry();
        elide();
    }
    QString fullText() const { return m_full; }
    QSize sizeHint() const override
    {
        const auto margins = contentsMargins();
        // Rounded up: a width rounded down would cut the last letter of a text that fits.
        const int width = int(std::ceil(QFontMetricsF(font()).horizontalAdvance(m_full))) + 1;
        return {width + margins.left() + margins.right(), fontMetrics().height() + margins.top() + margins.bottom()};
    }
    QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        elide();
    }

private:
    void elide()
    {
        QString line = m_full;
        line.replace(QLatin1Char('\n'), QLatin1Char(' '));
        QLabel::setText(fontMetrics().elidedText(line, Qt::ElideRight, std::max(0, contentsRect().width())));
    }
    QString m_full;
};

// A group's row in the tag picker: a button whose face is laid out with labels.
class ReviewTagGroupRow : public QPushButton
{
public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override { return layout() ? layout()->sizeHint() : QPushButton::sizeHint(); }
    QSize minimumSizeHint() const override { return layout() ? layout()->minimumSize() : QPushButton::minimumSizeHint(); }

protected:
    // Enter toggles the group like Space, rather than reaching the dialog's default button.
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (!event->isAutoRepeat()) click();
            event->accept();
            return;
        }
        QPushButton::keyPressEvent(event);
    }
};

// Lays its widgets out left to right at their natural width, wrapping onto the next line (Qt's
// flow layout); a widget wider than the line is given the line's width.
class ReviewFlowLayout : public QLayout
{
public:
    explicit ReviewFlowLayout(QWidget *parent, int spacing = 6)
        : QLayout(parent)
        , m_spacing(spacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~ReviewFlowLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) delete item;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        const int height = arrange(rect, false);
        // A window sizes itself from minimum sizes, which know nothing of wrapping (one line here),
        // so a dialog could squeeze the wrapped lines under the widgets below them. Ask for them.
        QWidget *owner = parentWidget();
        if (owner && height > 0 && owner->minimumHeight() != height) owner->setMinimumHeight(height);
    }
    QSize sizeHint() const override
    {
        // As wide as the widest widget: a line fits whatever width the parent gives.
        return minimumSize();
    }
    QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem *item : m_items) size = size.expandedTo(item->minimumSize());
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }

private:
    int arrange(const QRect &rect, bool measureOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = area.x(), y = area.y(), lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            if (item->isEmpty()) continue;
            const QSize hint = item->sizeHint();
            const int width = std::max(1, std::min(hint.width(), area.width()));
            if (x > area.x() && x + width > area.right() + 1) {
                x = area.x();
                y += lineHeight + m_spacing;
                lineHeight = 0;
            }
            if (!measureOnly) item->setGeometry(QRect(QPoint(x, y), QSize(width, hint.height())));
            x += width + m_spacing;
            lineHeight = std::max(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_spacing = 6;
};

// A tag chip as wide as its text, "Label (count)". The label is cut with "…" only when the
// whole chip is wider than its line; the count is never cut.
class ReviewTagChip : public QPushButton
{
public:
    using QPushButton::QPushButton;
    void setLabel(const QString &label, int count)
    {
        m_label = label;
        m_count = count;
        m_hasCount = true;
        updateGeometry();
        // A new count can make the chip wider: lay its line out again now, so its text is not
        // cut while the line has room.
        if (isVisible() && parentWidget() && parentWidget()->layout()) parentWidget()->layout()->activate();
        fit();
    }
    QSize sizeHint() const override
    {
        if (!m_hasCount) return QPushButton::sizeHint();
        ensurePolished();
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text = shown(fullText());
        const QSize text = fontMetrics().size(Qt::TextShowMnemonic, option.text);
        return style()->sizeFromContents(QStyle::CT_PushButton, &option, text, this);
    }
    QSize minimumSizeHint() const override
    {
        if (!m_hasCount) return QPushButton::minimumSizeHint();
        const QSize hint = sizeHint();
        return {std::min(hint.width(), hint.width() - fontMetrics().horizontalAdvance(m_label) + fontMetrics().horizontalAdvance(QStringLiteral("…"))), hint.height()};
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        fit();
    }
    void changeEvent(QEvent *event) override
    {
        QPushButton::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) fit();
    }
    void showEvent(QShowEvent *event) override
    {
        QPushButton::showEvent(event);
        fit();
    }

private:
    QString countText() const { return QStringLiteral(" (") + QString::number(m_count) + QLatin1Char(')'); }
    QString fullText() const { return m_label + countText(); }
    // Button text treats "&" as a mnemonic; a tag shows it as typed.
    static QString shown(QString text) { return text.replace(QLatin1Char('&'), QStringLiteral("&&")); }
    void fit()
    {
        if (!m_hasCount) return;
        const QString full = fullText();
        const int spare = sizeHint().width() - fontMetrics().horizontalAdvance(full);
        const int available = width() - spare;
        // Hidden (a closed group), a chip may hold a stale width: it shows its whole text until shown.
        if (!isVisible()) {
            setText(shown(full));
            return;
        }
        if (available >= fontMetrics().horizontalAdvance(full)) {
            setText(shown(full));
            return;
        }
        const QString count = countText();
        setText(shown(fontMetrics().elidedText(m_label, Qt::ElideRight, std::max(0, available - fontMetrics().horizontalAdvance(count))) + count));
    }
    QString m_label;
    int m_count = 0;
    bool m_hasCount = false;
};

// A tag group shown as a slider: one tick per step (weakest first) with its label and card count
// below, "Label (count)" as on a chip. No thumb until a step is chosen; choosing the chosen step
// again clears it. The steps are plain tags of the answer, so nothing else changes.
class ReviewTagScale : public QWidget
{
public:
    ReviewTagScale(const QString &groupName, const QStringList &labels, QWidget *parent)
        : QWidget(parent)
        , m_name(groupName)
        , m_labels(labels)
    {
        setObjectName(QStringLiteral("reviewAnswerTagScale"));
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(false);
        updateDescription();
    }
    int value() const { return m_value; }
    // Each step's count, as a chip shows it (ReviewTagPicker::liveCountOf()); the label never hides it.
    void setCounts(const QList<int> &counts)
    {
        if (counts == m_counts) return;
        m_counts = counts;
        QStringList texts;
        for (int index = 0; index < m_labels.size(); ++index) texts.append(stepText(index));
        setProperty("reviewTagStepTexts", texts);
        updateDescription();
        updateGeometry();
        update();
    }
    // The steps an answer had, as on the phone: an old answer may have several steps of one slider.
    // All are bold, the thumb on the first; touching the slider leaves one.
    void setSelection(QList<int> steps)
    {
        std::sort(steps.begin(), steps.end());
        steps.removeIf([this](int step) { return step < 0 || step >= m_labels.size(); });
        m_value = steps.isEmpty() ? -1 : steps.takeFirst();
        m_marked = steps;
        updateDescription();
        update();
    }
    // A step chosen by the user: the slider's only step (-1: none).
    void setValue(int value)
    {
        value = value < 0 || value >= m_labels.size() ? -1 : value;
        if (value == m_value && m_marked.isEmpty()) return;
        m_value = value;
        m_marked.clear();
        updateDescription();
        update();
        if (onChanged) onChanged(m_value);
    }
    std::function<void(int)> onChanged;

    QSize sizeHint() const override
    {
        int widest = 0;
        QFont bold = font();
        bold.setBold(true);
        for (int index = 0; index < m_labels.size(); ++index) widest = std::max(widest, QFontMetrics(bold).horizontalAdvance(stepText(index)));
        return {std::max(240, int(m_labels.size()) * std::min(widest + 8, 120)), labelTop() + 2 * fontMetrics().lineSpacing() + 4};
    }
    QSize minimumSizeHint() const override { return {int(m_labels.size()) * 40, sizeHint().height()}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int count = int(m_labels.size());
        if (count == 0) return;
        const QColor line = palette().color(QPalette::Mid);
        const QColor accent = palette().color(QPalette::Highlight);
        const int y = trackY();
        painter.setPen(QPen(line, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(center(0), y), QPointF(center(count - 1), y));
        if (m_value >= 0) {
            painter.setPen(QPen(accent, 3, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(center(0), y), QPointF(center(m_value), y));
        }
        for (int index = 0; index < count; ++index) {
            const bool reached = m_value >= 0 && index <= m_value;
            painter.setPen(Qt::NoPen);
            painter.setBrush(reached ? accent : line);
            painter.drawEllipse(QPointF(center(index), y), 3.5, 3.5);
        }
        if (m_value >= 0) {
            painter.setPen(QPen(palette().color(QPalette::Base), 2));
            painter.setBrush(accent);
            painter.drawEllipse(QPointF(center(m_value), y), 8, 8);
        }
        if (hasFocus()) {
            painter.setPen(QPen(accent, 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            const int focused = m_value >= 0 ? m_value : 0;
            painter.drawRoundedRect(QRectF(center(focused) - 11, y - 11, 22, 22), 11, 11);
        }
        painter.setPen(palette().color(QPalette::WindowText));
        for (int index = 0; index < count; ++index) {
            QFont labelFont = font();
            labelFont.setBold(index == m_value || m_marked.contains(index));
            painter.setFont(labelFont);
            const QRect cell(int(center(index) - cellWidth() / 2) + 2, labelTop(), int(cellWidth()) - 4, height() - labelTop());
            // Two lines at most: the label is cut with "…" to leave room for its count, never the count.
            const QFontMetrics metrics(labelFont);
            const QString count = countText(index);
            const QString text = metrics.elidedText(m_labels.at(index), Qt::ElideRight, std::max(0, cell.width() * 2 - 8 - metrics.horizontalAdvance(count))) + count;
            painter.drawText(cell, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, text);
        }
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        // Where each step is, for tests and tools driving the slider.
        QVariantList centers;
        for (int index = 0; index < m_labels.size(); ++index) centers.append(int(center(index)));
        setProperty("tickCenters", centers);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
        const int index = indexAt(event->position().x());
        m_dragged = false;
        m_pressChanged = index != m_value;
        if (m_pressChanged) setValue(index);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) return;
        const int index = indexAt(event->position().x());
        if (index != m_value) {
            m_dragged = true;
            setValue(index);
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(event);
        // A click on the chosen step clears the slider; with other steps of an old answer still
        // marked, it keeps only that step.
        if (!m_pressChanged && !m_dragged && indexAt(event->position().x()) == m_value) setValue(m_marked.isEmpty() ? -1 : m_value);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        const int last = int(m_labels.size()) - 1;
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Down:
            setValue(m_value < 0 ? 0 : std::max(0, m_value - 1));
            return;
        case Qt::Key_Right:
        case Qt::Key_Up:
            setValue(m_value < 0 ? 0 : std::min(last, m_value + 1));
            return;
        case Qt::Key_Home:
            setValue(0);
            return;
        case Qt::Key_End:
            setValue(last);
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            setValue(-1);
            return;
        default:
            QWidget::keyPressEvent(event);
        }
    }
    void focusInEvent(QFocusEvent *event) override
    {
        QWidget::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

private:
    qreal cellWidth() const { return m_labels.isEmpty() ? width() : qreal(width()) / m_labels.size(); }
    qreal center(int index) const { return cellWidth() * (index + 0.5); }
    int trackY() const { return 12; }
    int labelTop() const { return 26; }
    int indexAt(qreal x) const { return std::clamp(int(x / cellWidth()), 0, int(m_labels.size()) - 1); }
    // " (count)" once counts are set, as on a chip; nothing before.
    QString countText(int index) const
    {
        return index < m_counts.size() ? QStringLiteral(" (") + QString::number(m_counts.at(index)) + QLatin1Char(')') : QString();
    }
    QString stepText(int index) const { return m_labels.at(index) + countText(index); }
    void updateDescription()
    {
        QStringList shown;
        if (m_value >= 0) shown.append(m_labels.at(m_value));
        for (int index : m_marked) shown.append(m_labels.at(index));
        const QString chosen = shown.isEmpty() ? i18n("not set") : shown.join(QStringLiteral(", "));
        setAccessibleName(i18n("%1: %2", m_name, chosen));
        QStringList steps;
        for (int index = 0; index < m_labels.size(); ++index) steps.append(stepText(index));
        setAccessibleDescription(i18n("Steps: %1. Arrow keys choose a step; Delete clears it.", steps.join(QStringLiteral(", "))));
        setToolTip(m_value >= 0 ? i18n("%1: %2. Click it again to clear.", m_name, chosen) : i18n("%1: not set", m_name));
        setProperty("reviewTagScaleValue", m_value);
    }
    QString m_name;
    QStringList m_labels;
    QList<int> m_counts; // Per step; empty until set.
    int m_value = -1;
    QList<int> m_marked; // Further steps of an old answer, until the slider is touched.
    bool m_pressChanged = false;
    bool m_dragged = false;
};

// Edit slider (and Show as Slider…), as on the phone: one slider is one thing, the group's tags
// another. The dialog lists only the steps, numbered 1..n from left to right, each with move up,
// move down and remove; Add step appends a step, typed or picked from the group's free tags.
// Nothing changes until Save, which needs at least two steps. A removed step stays in the group
// as a plain tag, so no answer changes. Steps are not renamed here.
class ReviewSliderEditor : public QDialog
{
public:
    // @p groupTags are the group's tags in the order quick picks list them; @p resolve maps a
    // typed name to the known tag it stands for (ignoring case, merged names resolved), or
    // returns the cleaned name of a new tag.
    ReviewSliderEditor(const QString &title, const QStringList &steps, const QStringList &groupTags,
                       std::function<QString(const QString &)> resolve, QWidget *parent)
        : QDialog(parent)
        , m_steps(steps)
        , m_groupTags(groupTags)
        , m_resolve(std::move(resolve))
    {
        setObjectName(QStringLiteral("reviewSliderDialog"));
        setWindowTitle(title);
        resize(400, 420);
        auto *layout = new QVBoxLayout(this);
        auto *prompt = new QLabel(i18n("Steps go from left to right. Removing a step does not change past answers."), this);
        prompt->setObjectName(QStringLiteral("reviewSliderPrompt"));
        prompt->setTextFormat(Qt::PlainText);
        prompt->setWordWrap(true);
        layout->addWidget(prompt);
        auto *scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("reviewSliderSteps"));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *rows = new QWidget(scroll);
        m_rows = new QVBoxLayout(rows);
        m_rows->setContentsMargins(4, 4, 4, 4);
        m_rows->setSpacing(2);
        m_rows->addStretch(1);
        scroll->setWidget(rows);
        layout->addWidget(scroll, 1);
        auto *add = new QPushButton(i18n("Add step"), this);
        add->setObjectName(QStringLiteral("reviewSliderAddStep"));
        add->setAutoDefault(false);
        QObject::connect(add, &QPushButton::clicked, this, [this] { addStep(); });
        layout->addWidget(add, 0, Qt::AlignLeft);
        auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        box->setObjectName(QStringLiteral("reviewSliderButtons"));
        QObject::connect(box, &QDialogButtonBox::accepted, this, [this] {
            if (m_steps.size() < 2) {
                KMessageBox::information(this, i18n("A slider needs at least two steps."), windowTitle());
                return;
            }
            accept();
        });
        QObject::connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(box);
        showSteps();
    }

    QStringList steps() const { return m_steps; }

    // The group's tags that are not steps (removed steps included), offered by Add step.
    QStringList freeTags() const
    {
        QStringList tags;
        for (const QString &tag : m_groupTags) {
            if (!m_steps.contains(tag)) tags.append(tag);
        }
        return tags;
    }

private:
    // Appends @p tag as the last step; a name that is already a step (ignoring case, or an old
    // name merged into it) says so instead of doing nothing.
    void appendStep(const QString &tag)
    {
        if (tag.isEmpty()) return;
        if (m_steps.contains(tag)) {
            KMessageBox::information(this, i18n("“%1” is already a step of this slider.", reviewTagLabel(tag)), windowTitle());
            return;
        }
        m_steps.append(tag);
        showSteps(m_steps.size() - 1, "reviewSliderStepUp");
    }

    // One row per step; the row's buttons act on its index. @p focusIndex / @p focusButton keep
    // the keyboard on the step that just moved.
    void showSteps(int focusIndex = -1, const char *focusButton = nullptr)
    {
        // A row's button may still be handling the click that led here.
        for (QWidget *row : std::as_const(m_rowWidgets)) {
            row->hide();
            row->setObjectName(QString());
            m_rows->removeWidget(row);
            row->deleteLater();
        }
        m_rowWidgets.clear();
        setProperty("reviewSliderSteps", m_steps);
        QWidget *host = m_rows->parentWidget();
        for (int index = 0; index < m_steps.size(); ++index) {
            const QString tag = m_steps.at(index);
            const QString label = reviewTagLabel(tag);
            auto *row = new QWidget(host);
            row->setObjectName(QStringLiteral("reviewSliderStep"));
            row->setProperty("reviewTag", tag);
            auto *line = new QHBoxLayout(row);
            line->setContentsMargins(0, 0, 0, 0);
            line->setSpacing(6);
            auto *number = new QLabel(QString::number(index + 1), row);
            number->setObjectName(QStringLiteral("reviewSliderStepNumber"));
            number->setMinimumWidth(number->fontMetrics().horizontalAdvance(QStringLiteral("00")));
            number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            line->addWidget(number);
            auto *name = new QLabel(label, row);
            name->setObjectName(QStringLiteral("reviewSliderStepName"));
            name->setTextFormat(Qt::PlainText);
            name->setWordWrap(true);
            line->addWidget(name, 1);
            const auto button = [this, row, line, &label](const QString &text, const char *objectName, const QString &tip, bool enabled,
                                                               std::function<void()> action) {
                auto *tool = new QToolButton(row);
                tool->setObjectName(QString::fromLatin1(objectName));
                tool->setText(text);
                tool->setAutoRaise(true);
                tool->setToolTip(tip);
                tool->setAccessibleName(QStringLiteral("%1: %2").arg(tip, label));
                tool->setEnabled(enabled);
                QObject::connect(tool, &QToolButton::clicked, this, std::move(action));
                line->addWidget(tool);
                return tool;
            };
            QToolButton *up = button(QStringLiteral("↑"), "reviewSliderStepUp", i18n("Move up"), index > 0, [this, index] { move(index, -1); });
            QToolButton *down = button(QStringLiteral("↓"), "reviewSliderStepDown", i18n("Move down"), index + 1 < m_steps.size(), [this, index] { move(index, 1); });
            QToolButton *remove = button(QStringLiteral("✕"), "reviewSliderStepRemove", i18n("Remove step"), true, [this, index] { removeStep(index); });
            m_rows->insertWidget(index, row);
            m_rowWidgets.append(row);
            if (index == focusIndex && focusButton) {
                for (QToolButton *tool : {up, down, remove}) {
                    if (tool->objectName() == QLatin1String(focusButton) && tool->isEnabled()) tool->setFocus();
                }
            }
        }
    }

    void move(int index, int offset)
    {
        const int target = index + offset;
        if (index < 0 || index >= m_steps.size() || target < 0 || target >= m_steps.size()) return;
        m_steps.move(index, target);
        showSteps(target, offset < 0 ? "reviewSliderStepUp" : "reviewSliderStepDown");
    }

    void removeStep(int index)
    {
        if (index < 0 || index >= m_steps.size()) return;
        m_steps.removeAt(index);
        showSteps(std::min<int>(index, m_steps.size() - 1), "reviewSliderStepRemove");
    }

    // Asks for the new step's name, with the group's free tags as quick picks. The step goes at
    // the end.
    void addStep()
    {
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("reviewSliderAddStepDialog"));
        dialog.setWindowTitle(i18n("Add step"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *prompt = new QLabel(i18n("Step name:"), &dialog);
        layout->addWidget(prompt);
        auto *name = new QLineEdit(&dialog);
        name->setObjectName(QStringLiteral("reviewSliderAddStepName"));
        prompt->setBuddy(name);
        layout->addWidget(name);
        QString picked;
        const QStringList picks = freeTags();
        if (!picks.isEmpty()) {
            auto *caption = new QLabel(i18n("Or use a tag of this group:"), &dialog);
            caption->setObjectName(QStringLiteral("reviewSliderAddStepPicksCaption"));
            layout->addSpacing(6);
            layout->addWidget(caption);
            auto *chips = new QWidget(&dialog);
            chips->setObjectName(QStringLiteral("reviewSliderAddStepPicks"));
            chips->setStyleSheet(reviewTagChipStyle());
            auto *flow = new ReviewFlowLayout(chips);
            for (const QString &tag : picks) {
                auto *chip = new QPushButton(reviewTagLabel(tag), chips);
                chip->setObjectName(QStringLiteral("reviewSliderAddStepPick"));
                chip->setProperty("reviewTag", tag);
                chip->setAutoDefault(false);
                chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
                QObject::connect(chip, &QPushButton::clicked, &dialog, [&dialog, &picked, tag] {
                    picked = tag;
                    dialog.accept();
                });
                flow->addWidget(chip);
            }
            layout->addWidget(chips);
        }
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        box->setObjectName(QStringLiteral("reviewSliderAddStepButtons"));
        auto *ok = box->button(QDialogButtonBox::Ok);
        ok->setText(i18n("Add"));
        ok->setEnabled(false);
        QObject::connect(name, &QLineEdit::textChanged, ok, [ok](const QString &text) { ok->setEnabled(!text.trimmed().isEmpty()); });
        QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(box);
        name->setFocus();
        if (dialog.exec() != QDialog::Accepted) return;
        // A typed name is the known tag it names (in any group), else a new tag.
        appendStep(picked.isEmpty() ? m_resolve(name->text()) : picked);
    }

    QStringList m_steps;
    QStringList m_groupTags;
    std::function<QString(const QString &)> m_resolve;
    QVBoxLayout *m_rows = nullptr;
    QList<QWidget *> m_rowWidgets;
};

// Which tag groups of the picker are open is remembered on this device, not synced: KConfig group
// ReviewTagGroups lists in Open and Closed the keys (group IDs, "other" for Other) of the groups
// the user opened or closed. A group never opened or closed is open when it is Understanding or
// Recall (or Other when there are no groups); any other group starts closed, one made later too.
static KConfigGroup tagGroupFoldConfig()
{
    return KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("ReviewTagGroups"));
}

static bool tagGroupOpenByDefault(const QString &id, const QString &name)
{
    for (const QString &open : {QStringLiteral("Understanding"), QStringLiteral("Recall")}) {
        if (id.compare(open, Qt::CaseInsensitive) == 0 || name.compare(open, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

static bool tagGroupOpen(const QString &key, bool byDefault)
{
    const KConfigGroup config = tagGroupFoldConfig();
    if (config.readEntry("Open", QStringList()).contains(key)) return true;
    if (config.readEntry("Closed", QStringList()).contains(key)) return false;
    return byDefault;
}

static void rememberTagGroupOpen(const QString &key, bool open)
{
    KConfigGroup config = tagGroupFoldConfig();
    QStringList opened = config.readEntry("Open", QStringList());
    QStringList closed = config.readEntry("Closed", QStringList());
    opened.removeAll(key);
    closed.removeAll(key);
    (open ? opened : closed).append(key);
    config.writeEntry("Open", opened);
    config.writeEntry("Closed", closed);
    config.sync();
}

// Sections (ReviewTags::TagGroup::section) fold too, remembered on this device by their name in
// ReviewTagGroups' ClosedSections; a section is open until the user closes it.
static bool tagSectionOpen(const QString &name)
{
    return !tagGroupFoldConfig().readEntry("ClosedSections", QStringList()).contains(name);
}

static void rememberTagSectionOpen(const QString &name, bool open)
{
    KConfigGroup config = tagGroupFoldConfig();
    QStringList closed = config.readEntry("ClosedSections", QStringList());
    closed.removeAll(name);
    if (!open) closed.append(name);
    config.writeEntry("ClosedSections", closed);
    config.sync();
}

// A group menu's Ask (review-phases spec): when the group is asked, as radio items. A phase this
// client does not know reads as After the answer. @p choose gets the phase picked.
static QMenu *addPhaseMenu(QMenu *menu, const QString &current, std::function<void(const QString &)> choose)
{
    QMenu *ask = menu->addMenu(i18n("Ask"));
    ask->setObjectName(QStringLiteral("reviewTagGroupAskMenu"));
    auto *choices = new QActionGroup(ask);
    choices->setExclusive(true);
    const QList<std::pair<QString, QString>> phases{{QString(), i18n("After the answer")},
                                                     {ReviewTags::beforePhase(), i18n("Before the answer")},
                                                     {ReviewTags::beforeAndAfterPhase(), i18n("Before and after")}};
    for (const auto &[phase, text] : phases) {
        QAction *action = ask->addAction(text);
        action->setObjectName(QStringLiteral("reviewTagGroupPhase"));
        action->setProperty("reviewTagPhase", phase);
        action->setCheckable(true);
        action->setChecked(phase == current);
        choices->addAction(action);
        QObject::connect(action, &QAction::triggered, ask, [choose, phase] { choose(phase); });
    }
    return ask;
}

// The tag picker, as on the phone: only the list of groups (each group, then Other, then New
// group). Every group, slider or not, folds under its header: a closed header shows the group's
// name, the tags chosen in it ("Recall · Recalled") and whether it has a note; clicking it (or
// Space/Enter on it) opens the group with its slider, chips, New tag and the group's note for this
// answer. Groups open and close on their own, remembered per group (tagGroupOpen()). Groups only
// organize the list; tags from any groups can be selected together, in closed groups too.
// A tag no card has (0) gets no chip unless it is selected; one that drops to 0 while the picker
// is open keeps its chip until it closes, and New tag picks a hidden tag by its name.
// Adjacent groups with the same section show indented under its header, which folds them all;
// closed, it names what is chosen in them: "Work left · Structure: Missing steps · Style: Clean".
// Sliders still come first, so a section holding sliders and chip groups has a header in each part.
// The picker is the after step of an answer (review-phases spec): groups asked before the answer
// only are not listed; the before panel under the question asks them.
class ReviewTagPicker : public QWidget
{
public:
    ReviewTagPicker(const QStringList &available, const QStringList &selected, const QHash<QString, QString> &tagMerges,
                    const ReviewStore *store, QWidget *parent, const QHash<QString, QString> &groupNotes = {},
                    const QHash<QString, int> &usage = {}, const QStringList &current = {}, ReviewContextNotes::Draft *context = nullptr)
        : QWidget(parent)
        , m_context(context && !context->isEmpty() ? context : nullptr)
        , m_known(available)
        , m_selected(selected)
        , m_notes(groupNotes)
        , m_tagMerges(tagMerges)
        , m_usage(usage)
        , m_current(current)
        , m_store(store)
    {
        setObjectName(QStringLiteral("reviewAnswerTags"));
        setStyleSheet(reviewTagChipStyle()
                      + QStringLiteral("QPushButton#reviewAnswerNewTag, QPushButton#reviewAnswerNewGroup { border: none; color: palette(link); }"
                                       "QPushButton#reviewAnswerTagGroupRow { border: none; border-radius: 6px; padding: 0px; min-height: 0px; background: transparent; }"
                                       "QPushButton#reviewAnswerTagGroupRow:hover { background: palette(midlight); }"
                                       "QPushButton#reviewAnswerTagGroupRow:checked { background: palette(alternate-base); }"
                                       "QPushButton#reviewAnswerTagSectionRow { border: none; border-radius: 6px; padding: 0px; min-height: 0px; background: transparent; }"
                                       "QPushButton#reviewAnswerTagSectionRow:hover { background: palette(midlight); }"
                                       "QLabel#reviewAnswerTagGroup { font-weight: bold; color: palette(window-text); }"
                                       "QLabel#reviewAnswerTagSectionTitle { color: palette(window-text); }"
                                       "QLabel#reviewAnswerTagGroupArrow, QLabel#reviewAnswerTagSectionArrow { color: palette(window-text); }"
                                       "QLabel#reviewAnswerTagGroupSummary, QLabel#reviewAnswerTagScaleTagsCaption, QLabel#reviewAnswerTagSectionSummary { color: palette(placeholder-text); }"
                                       "QLabel#reviewAnswerTagGroupNoteMarker { color: palette(link); }"
                                       "QToolButton#reviewAnswerTagGroupMenu { border: none; padding: 0px 4px; }"
                                       "QToolButton#reviewAnswerTagGroupMenu::menu-indicator { image: none; width: 0px; }"));
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        // Height left by a group that closed goes below the list, not between its rows.
        m_layout->addStretch(1);
        // The answer's own note, after every group; rebuild() puts the list above it.
        m_layout->insertWidget(0, answerNoteSection());
        // The section, page and document notes, below the answer's note.
        if (m_context) m_layout->insertWidget(1, contextSection());
        if (m_store) m_groups = m_store->tagGroups();
        // A starting tag answered elsewhere since the counts were taken is still offered.
        for (const QString &tag : selected) {
            if (!m_known.contains(tag)) m_known.append(tag);
        }
        // Tags put in a group on another device are known even before their first use here.
        for (const auto &group : std::as_const(m_groups)) {
            for (const QString &tag : group.tags) {
                if (!m_known.contains(tag)) m_known.append(tag);
            }
        }
        rebuild();
    }

    // In the order of the plain list, whatever groups the tags are in.
    QStringList selectedTags() const
    {
        QStringList result;
        for (const QString &tag : m_known) {
            if (m_selected.contains(tag)) result.append(tag);
        }
        return result;
    }

    // This answer's notes by group ID ("other" for Other, "note" for the answer's own note), as
    // typed; notes of groups deleted meanwhile are left out.
    QHash<QString, QString> groupNotes() const
    {
        QHash<QString, QString> notes;
        for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
            const bool exists = it.key() == ReviewTags::otherGroupNoteKey() || it.key() == ReviewTags::answerNoteKey()
                || std::any_of(m_groups.cbegin(), m_groups.cend(), [&it](const auto &group) { return group.id == it.key(); });
            if (exists && !it.value().trimmed().isEmpty()) notes.insert(it.key(), it.value());
        }
        return notes;
    }

    std::function<void()> onChanged;

private:
    struct Section {
        QString key; // The note key: the group ID, or "other".
        QString id;  // Empty for Other.
        QString name;
        QStringList tags;
        QStringList scale; // A slider's steps; empty for a group of chips.
        QPushButton *row = nullptr;
        QLabel *arrow = nullptr;
        QLabel *dot = nullptr;
        ReviewElidedLabel *summary = nullptr;
        QWidget *body = nullptr;
        int run = -1; // Its index in m_runs when the group is under a section header.
    };

    // Adjacent groups of one section: a header over them, indented, folding them together.
    struct SectionRun {
        QString name;
        QStringList keys; // Its groups' keys, as shown.
        QPushButton *row = nullptr;
        QLabel *arrow = nullptr;
        QLabel *dot = nullptr;
        ReviewElidedLabel *summary = nullptr;
        QWidget *body = nullptr;
    };

    static QString keyOf(const QString &groupId) { return groupId.isEmpty() ? ReviewTags::otherGroupNoteKey() : groupId; }

    // The cards that currently have @p tag, counted once when the picker opened
    // (ReviewStore::reviewTagUsage()). The order of the chips and "Move tags here…" use it, so
    // chips do not move while the picker is open.
    int countOf(const QString &tag) const { return m_usage.value(tag); }

    // What a chip shows, as on the phone: the cards that would have the tag once this answer is
    // saved. The card counts in countOf() when its current tags have it; it counts here when the
    // tag is selected. A review starts with the card's tags selected, so they show their count;
    // selecting a tag the card lacks shows one more, deselecting one it has one fewer.
    int liveCountOf(const QString &tag) const
    {
        return std::max(0, countOf(tag) - (m_current.contains(tag) ? 1 : 0)) + (m_selected.contains(tag) ? 1 : 0);
    }

    // Whether @p tag gets a chip: when a card would have it once this answer is saved (a selected
    // tag always does), or when it had one since the picker opened, so a chip never goes away
    // under the pointer. "Move tags here…" and New tag still reach every tag.
    bool showsChip(const QString &tag)
    {
        if (!m_shown.contains(tag) && liveCountOf(tag) <= 0) return false;
        m_shown.insert(tag);
        return true;
    }

    // The known tag a New tag name stands for, ignoring case: an old name becomes the tag it was
    // merged into, and a known tag (hidden ones included) is picked by its name or label.
    QString typedTag(const QString &text) const
    {
        QString tag = normalizeReviewTag(text, m_tagMerges);
        if (tag.isEmpty()) return tag;
        for (auto it = m_tagMerges.cbegin(); it != m_tagMerges.cend(); ++it) {
            if (it.key().compare(tag, Qt::CaseInsensitive) == 0) {
                tag = it.value();
                break;
            }
        }
        for (const QString &known : m_known) {
            if (known.compare(tag, Qt::CaseInsensitive) == 0) return known;
        }
        for (const QString &known : m_known) {
            if (reviewTagLabel(known).compare(tag, Qt::CaseInsensitive) == 0) return known;
        }
        return tag;
    }

    // The chip's text, tooltip and accessible name with its live count.
    void showCount(ReviewTagChip *button, const QString &tag) const
    {
        const QString label = reviewTagLabel(tag);
        button->setLabel(label, liveCountOf(tag));
        button->setToolTip(ReviewTags::withCount(label, liveCountOf(tag)));
        button->setAccessibleName(button->toolTip());
    }

    // The groups, then Other, each with its most-used tags first (ties by label), as on the phone.
    QList<ReviewTags::TagGroup> sections() const
    {
        QList<ReviewTags::TagGroup> result = ReviewTags::sections(m_groups, m_known);
        for (auto &section : result) section.tags = ReviewTags::byUsage(section.tags, m_usage, reviewTagLabel);
        return result;
    }

    QString nameOf(const QString &groupId) const
    {
        if (groupId.isEmpty()) return i18n("Other");
        for (const auto &group : m_groups) {
            if (group.id == groupId) return group.name;
        }
        return groupId;
    }

    void rebuild()
    {
        // A chip or menu of the old content may still be handling the event that led here.
        if (m_content) {
            m_content->hide();
            m_layout->removeWidget(m_content);
            m_content->setParent(nullptr);
            m_retired.emplace_back(m_content);
        }
        m_sections.clear();
        m_runs.clear();
        m_content = new QWidget(this);
        auto *column = new QVBoxLayout(m_content);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        QList<ReviewTags::TagGroup> sections = this->sections();
        // Tags are gone: only slider groups are listed (no chip groups, no Other), as on the phone;
        // groups asked before the answer only were asked in the before panel.
        sections.removeIf([](const ReviewTags::TagGroup &section) { return section.scale.isEmpty() || !ReviewTags::askedAfter(section); });
        // Adjacent groups with the same section form a run, in layout order (the same name further
        // down is another run).
        QList<int> runOf;
        for (qsizetype index = 0; index < sections.size(); ++index) {
            const QString &section = sections.at(index).section;
            if (section.isEmpty()) runOf.append(-1);
            else runOf.append(index > 0 && sections.at(index - 1).section == section ? runOf.constLast() : int(index));
        }
        QList<qsizetype> order(sections.size());
        std::iota(order.begin(), order.end(), 0);
        // Sliders come first; the other groups, then Other, below them. As on the phone, a section
        // holding both gets a header in each part, and the two fold together (by name).
        std::stable_partition(order.begin(), order.end(), [&sections](qsizetype index) { return !sections.at(index).scale.isEmpty(); });
        m_sections.reserve(sections.size());
        QVBoxLayout *runColumn = nullptr;
        int lastRun = -1;
        bool lastSlider = true;
        for (const qsizetype index : std::as_const(order)) {
            const int run = runOf.at(index);
            const bool slider = !sections.at(index).scale.isEmpty();
            if (run < 0) {
                addListedGroup(column, sections.at(index));
                lastRun = -1;
                continue;
            }
            // A run's groups placed one after another in the same part share its header.
            if (run != lastRun || slider != lastSlider) runColumn = addRun(column, sections.at(index).section);
            lastRun = run;
            lastSlider = slider;
            addListedGroup(runColumn, sections.at(index), int(m_runs.size()) - 1);
        }
        m_layout->insertWidget(0, m_content);
        applyOpen();
        // A layout shows a widget added to a visible window only later; show the edit at once.
        m_content->show();
    }

    // Whether the group of @p key is open: as the user left it on this device, else by default.
    bool isOpen(const Section &section) const
    {
        // Without groups the list is Other alone: open it.
        const bool byDefault = section.id.isEmpty() ? m_groups.isEmpty() : tagGroupOpenByDefault(section.id, section.name);
        return tagGroupOpen(section.key, byDefault);
    }

    // Opens or closes a group, remembered on this device; the dialog follows the new height.
    void setOpen(const QString &key, bool open)
    {
        rememberTagGroupOpen(key, open);
        applyOpen();
        fitWindow();
    }

    // The window (the Tags or Skip dialog) takes the height its content needs, within the screen.
    void fitWindow()
    {
        auto *dialog = qobject_cast<QDialog *>(window());
        if (!dialog || !dialog->layout() || !dialog->isVisible()) return;
        // A group just shown or hidden has only posted its layout request, and the layouts above it
        // still hold the sizes they cached: drop those caches up to the dialog, then lay it out now.
        for (const Section &section : std::as_const(m_sections)) {
            for (QWidget *widget = section.body->parentWidget(); widget && widget != dialog; widget = widget->parentWidget()) widget->updateGeometry();
        }
        dialog->layout()->activate();
        int height = dialog->layout()->hasHeightForWidth() ? dialog->layout()->totalHeightForWidth(dialog->width()) : dialog->sizeHint().height();
        height = std::max(height, dialog->minimumSizeHint().height());
        if (const QScreen *screen = dialog->screen()) height = std::min(height, screen->availableGeometry().height());
        dialog->resize(dialog->width(), height);
    }

    void addListedGroup(QVBoxLayout *column, const ReviewTags::TagGroup &group, int run = -1)
    {
        if (!group.scale.isEmpty()) addScale(column, group);
        else addSection(column, group);
        m_sections.last().run = run;
        if (run >= 0) m_runs[run].keys.append(m_sections.constLast().key);
    }

    // A section's header (arrow, name and, closed, what its groups have chosen), then its groups
    // indented below it, each still folding on its own.
    // Returns the column its groups go in.
    QVBoxLayout *addRun(QVBoxLayout *column, const QString &name)
    {
        SectionRun run;
        run.name = name;
        auto *block = new QWidget(m_content);
        block->setObjectName(QStringLiteral("reviewAnswerTagSectionBlock"));
        block->setProperty("reviewTagSection", run.name);
        auto *layout = new QVBoxLayout(block);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);

        auto *row = new ReviewTagGroupRow(block);
        row->setObjectName(QStringLiteral("reviewAnswerTagSectionRow"));
        row->setProperty("reviewTagSection", run.name);
        row->setCheckable(true);
        row->setAutoDefault(false);
        row->setFocusPolicy(Qt::StrongFocus);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *top = new QHBoxLayout(row);
        top->setContentsMargins(6, 5, 6, 5);
        top->setSpacing(6);
        const auto faceLabel = [row](QLabel *label, const char *objectName) {
            label->setObjectName(QString::fromLatin1(objectName));
            label->setTextFormat(Qt::PlainText);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            label->setProperty("reviewTagSection", row->property("reviewTagSection"));
            return label;
        };
        run.arrow = faceLabel(new QLabel(row), "reviewAnswerTagSectionArrow");
        top->addWidget(run.arrow);
        auto *title = faceLabel(new QLabel(run.name, row), "reviewAnswerTagSectionTitle");
        QFont larger = title->font();
        larger.setBold(true);
        if (larger.pointSizeF() > 0) larger.setPointSizeF(larger.pointSizeF() * 1.1);
        else larger.setPixelSize(std::max(1, int(larger.pixelSize() * 1.1)));
        title->setFont(larger);
        top->addWidget(title);
        run.dot = faceLabel(new QLabel(QStringLiteral("·"), row), "reviewAnswerTagSectionSummaryDot");
        top->addWidget(run.dot);
        run.summary = new ReviewElidedLabel(row);
        faceLabel(run.summary, "reviewAnswerTagSectionSummary");
        run.summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        top->addWidget(run.summary);
        top->addStretch(1);
        QObject::connect(row, &QPushButton::clicked, this, [this, name] {
            rememberTagSectionOpen(name, !tagSectionOpen(name));
            applyOpen();
            fitWindow();
        });
        run.row = row;
        layout->addWidget(row);

        run.body = new QWidget(block);
        run.body->setObjectName(QStringLiteral("reviewAnswerTagSectionBody"));
        run.body->setProperty("reviewTagSection", run.name);
        auto *children = new QVBoxLayout(run.body);
        children->setContentsMargins(run.arrow->sizeHint().width() + 12, 0, 0, 0);
        children->setSpacing(2);
        layout->addWidget(run.body);
        column->addWidget(block);
        m_runs.append(run);
        return children;
    }

    // The tags chosen in a group, a slider's step first, as their labels.
    QStringList chosenIn(const Section &section) const
    {
        QStringList chosen;
        for (const QString &step : section.scale) {
            if (m_selected.contains(step)) chosen.append(reviewTagLabel(step));
        }
        for (const QString &tag : section.tags) {
            if (!section.scale.contains(tag) && m_selected.contains(tag)) chosen.append(reviewTagLabel(tag));
        }
        return chosen;
    }

    // A section's header: closed, what its groups have chosen ("Structure: Missing steps · Style: Clean").
    void updateRun(int index)
    {
        if (index < 0 || index >= int(m_runs.size())) return;
        const SectionRun &run = m_runs.at(index);
        QStringList parts;
        for (const QString &key : run.keys) {
            for (const Section &section : std::as_const(m_sections)) {
                if (section.key != key) continue;
                if (const QStringList chosen = chosenIn(section); !chosen.isEmpty())
                    parts.append(QStringLiteral("%1: %2").arg(section.name, chosen.join(QStringLiteral(", "))));
            }
        }
        const QString summary = parts.join(QStringLiteral(" · "));
        const bool open = tagSectionOpen(run.name);
        run.summary->setFullText(summary);
        const bool summaryShown = !summary.isEmpty() && !open;
        run.summary->setVisible(summaryShown);
        run.dot->setVisible(summaryShown);
        run.row->layout()->invalidate();
        run.row->layout()->activate();
        QStringList spoken{run.name};
        if (!summary.isEmpty()) spoken.append(i18n("Selected: %1", summary));
        spoken.append(open ? i18n("Expanded") : i18n("Collapsed"));
        run.row->setAccessibleName(spoken.join(QStringLiteral(". ")));
        run.row->setToolTip(summary.isEmpty() ? (open ? i18n("Close %1", run.name) : i18n("Open %1", run.name)) : summary);
    }

    // A group's header: a row with the disclosure arrow, the name, the tags chosen in the group
    // (shown while it is closed) and its note marker; clicking it opens or closes the group. The
    // caller adds its buttons (Edit slider, menu) after the row in @p header.
    void addHeader(QHBoxLayout *header, Section &section, QWidget *parent)
    {
        header->setSpacing(2);
        auto *row = new ReviewTagGroupRow(parent);
        row->setObjectName(QStringLiteral("reviewAnswerTagGroupRow"));
        row->setProperty("reviewTagGroup", section.id);
        row->setCheckable(true);
        row->setAutoDefault(false);
        row->setFocusPolicy(Qt::StrongFocus);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *top = new QHBoxLayout(row);
        top->setContentsMargins(6, 4, 6, 4);
        top->setSpacing(6);
        const auto faceLabel = [](QLabel *label, const char *name) {
            label->setObjectName(QString::fromLatin1(name));
            label->setTextFormat(Qt::PlainText);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            return label;
        };
        section.arrow = faceLabel(new QLabel(row), "reviewAnswerTagGroupArrow");
        top->addWidget(section.arrow);
        auto *title = faceLabel(new QLabel(section.name, row), "reviewAnswerTagGroup");
        title->setProperty("reviewTagGroup", section.id);
        top->addWidget(title);
        section.dot = faceLabel(new QLabel(QStringLiteral("·"), row), "reviewAnswerTagGroupSummaryDot");
        top->addWidget(section.dot);
        // As wide as its text, cut when the row is too narrow.
        section.summary = new ReviewElidedLabel(row);
        faceLabel(section.summary, "reviewAnswerTagGroupSummary");
        section.summary->setProperty("reviewTagGroup", section.id);
        section.summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        top->addWidget(section.summary);
        top->addStretch(1);
        const QString key = section.key;
        QObject::connect(row, &QPushButton::clicked, this, [this, key] {
            const auto found = std::find_if(m_sections.cbegin(), m_sections.cend(), [&key](const Section &candidate) { return candidate.key == key; });
            if (found != m_sections.cend()) setOpen(key, !isOpen(*found));
        });
        section.row = row;
        header->addWidget(row, 1);
    }

    void addSection(QVBoxLayout *column, const ReviewTags::TagGroup &group)
    {
        Section section;
        section.id = group.id;
        section.key = keyOf(group.id);
        section.name = group.id.isEmpty() ? i18n("Other") : group.name;
        section.tags = group.tags;

        auto *header = new QHBoxLayout;
        addHeader(header, section, m_content);
        // Other has no name to change; it only takes tags back, when there are groups to take them from.
        if (m_store && (!group.id.isEmpty() || !m_groups.isEmpty())) header->addWidget(groupMenu(group.id), 0, Qt::AlignVCenter);
        column->addLayout(header);

        section.body = new QWidget(m_content);
        section.body->setObjectName(QStringLiteral("reviewAnswerTagGroupBody"));
        section.body->setProperty("reviewTagGroup", group.id);
        auto *body = new QVBoxLayout(section.body);
        body->setContentsMargins(section.arrow->sizeHint().width() + 12, 2, 0, 8);
        body->setSpacing(6);
        addChips(body, group);
        column->addWidget(section.body);
        m_sections.append(section);
    }

    void applyOpen()
    {
        for (int index = 0; index < int(m_runs.size()); ++index) {
            const SectionRun &run = m_runs.at(index);
            const bool open = tagSectionOpen(run.name);
            run.row->setChecked(open);
            run.arrow->setText(open ? QStringLiteral("▾") : QStringLiteral("▸"));
            run.body->setVisible(open);
            updateRun(index);
        }
        for (const Section &section : std::as_const(m_sections)) {
            const bool open = isOpen(section);
            section.row->setChecked(open);
            section.arrow->setText(open ? QStringLiteral("▾") : QStringLiteral("▸"));
            section.body->setVisible(open);
            updateRow(section.key);
        }
    }

    // The header's summary of the tags chosen in its group (a slider's step first) and its note marker.
    void updateRow(const QString &key)
    {
        for (const Section &section : std::as_const(m_sections)) {
            if (section.key != key) continue;
            const QString summary = chosenIn(section).join(QStringLiteral(", "));
            const bool open = isOpen(section);
            section.summary->setFullText(summary);
            // An open group shows its choice on its slider and chips, with their counts; the
            // summary is for closed ones: "Recall · Recalled".
            const bool summaryShown = !summary.isEmpty() && !open;
            section.summary->setVisible(summaryShown);
            section.dot->setVisible(summaryShown);
            // Lay the header out at once, so the summary is cut to the room it really has.
            section.row->layout()->invalidate();
            section.row->layout()->activate();
            QStringList spoken{section.name};
            if (!summary.isEmpty()) spoken.append(i18n("Selected: %1", summary));
            spoken.append(open ? i18n("Expanded") : i18n("Collapsed"));
            section.row->setAccessibleName(spoken.join(QStringLiteral(". ")));
            section.row->setToolTip(summary.isEmpty() ? (open ? i18n("Close %1", section.name) : i18n("Open %1", section.name)) : summary);
            updateRun(section.run);
        }
    }

    // The chips of @p section's free tags (a slider's steps are not chips), each as wide as its
    // text and wrapping onto more lines, then New tag. Returns how many chips it added.
    int addChips(QVBoxLayout *column, const ReviewTags::TagGroup &section)
    {
        int added = 0;
        auto *chips = new QWidget(m_content);
        chips->setObjectName(QStringLiteral("reviewAnswerTagSection"));
        chips->setProperty("reviewTagGroup", section.id);
        auto *flow = new ReviewFlowLayout(chips);
        const QString key = keyOf(section.id);
        for (const QString &tag : section.tags) {
            if (section.scale.contains(tag) || !showsChip(tag)) continue;
            // "Label (count)": a label wider than the line is cut on the chip, never its count;
            // hovering or pressing it shows the whole tag with its count.
            auto *button = new ReviewTagChip(chips);
            button->setObjectName(QStringLiteral("reviewAnswerTag"));
            showCount(button, tag);
            button->setProperty("reviewTag", tag);
            button->setProperty("reviewTagGroup", section.id);
            button->setCheckable(true);
            button->setChecked(m_selected.contains(tag));
            button->setAutoDefault(false);
            button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            QObject::connect(button, &QPushButton::pressed, button, [button] {
                QToolTip::showText(button->mapToGlobal(QPoint(0, button->height())), button->toolTip(), button);
            });
            QObject::connect(button, &QPushButton::toggled, this, [this, button, tag, key](bool checked) {
                if (checked && !m_selected.contains(tag)) m_selected.append(tag);
                if (!checked) m_selected.removeAll(tag);
                // The count follows the selection; the chip keeps its place.
                showCount(button, tag);
                updateRow(key);
                if (onChanged) onChanged();
            });
            flow->addWidget(button);
            ++added;
        }
        auto *newTag = new QPushButton(i18n("+ New tag"), chips);
        newTag->setObjectName(QStringLiteral("reviewAnswerNewTag"));
        newTag->setProperty("reviewTagGroup", section.id);
        newTag->setAutoDefault(false);
        if (!section.id.isEmpty()) newTag->setToolTip(i18n("Add a tag to %1", section.name));
        const QString groupId = section.id;
        QObject::connect(newTag, &QPushButton::clicked, this, [this, groupId] { createTag(groupId); });
        newTag->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        flow->addWidget(newTag);
        column->addWidget(chips);
        return added;
    }

    // A slider group. Its header: the row with its name (and, closed, its step and tags), then Edit
    // slider and its menu. Open, below it: the slider; a small "Tags" caption over the group's
    // free-tag chips (hidden when there are none) and New tag. Groups have no note of their own:
    // the answer has one Note, below every group. One slider is one thing, the tags another: steps are edited only in Edit slider,
    // never as chips. Picking a step replaces the group's other step.
    void addScale(QVBoxLayout *column, const ReviewTags::TagGroup &group)
    {
        Section section;
        section.key = keyOf(group.id);
        section.id = group.id;
        section.name = group.name;
        section.tags = group.tags;
        section.scale = group.scale;
        auto *block = new QWidget(m_content);
        block->setObjectName(QStringLiteral("reviewAnswerTagScaleGroup"));
        block->setProperty("reviewTagGroup", group.id);
        auto *layout = new QVBoxLayout(block);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *header = new QHBoxLayout;
        addHeader(header, section, block);
        if (m_store) {
            auto *edit = new QToolButton(block);
            edit->setObjectName(QStringLiteral("reviewAnswerTagGroupEditSlider"));
            edit->setProperty("reviewTagGroup", group.id);
            const QIcon pencil = QIcon::fromTheme(QStringLiteral("document-edit"));
            if (pencil.isNull()) edit->setText(QStringLiteral("✏\uFE0E"));
            else edit->setIcon(pencil);
            edit->setAutoRaise(true);
            edit->setToolTip(i18n("Edit slider"));
            edit->setAccessibleName(edit->toolTip());
            const QString id = group.id;
            QObject::connect(edit, &QToolButton::clicked, this, [this, id] { editSlider(id); });
            header->addWidget(edit, 0, Qt::AlignVCenter);
        }
        const QString key = section.key;
        if (m_store) header->addWidget(groupMenu(group.id), 0, Qt::AlignVCenter);
        layout->addLayout(header);

        section.body = new QWidget(block);
        section.body->setObjectName(QStringLiteral("reviewAnswerTagGroupBody"));
        section.body->setProperty("reviewTagGroup", group.id);
        auto *body = new QVBoxLayout(section.body);
        body->setContentsMargins(section.arrow->sizeHint().width() + 12, 2, 0, 8);
        body->setSpacing(4);

        QStringList labels;
        for (const QString &step : group.scale) labels.append(reviewTagLabel(step));
        auto *scale = new ReviewTagScale(group.name, labels, section.body);
        scale->setProperty("reviewTagGroup", group.id);
        scale->setProperty("reviewTagSteps", group.scale);
        QList<int> chosen;
        for (int index = 0; index < group.scale.size(); ++index) {
            if (m_selected.contains(group.scale.at(index))) chosen.append(index);
        }
        scale->setSelection(chosen);
        const QStringList steps = group.scale;
        // Every step shows its count, a step at 0 too: the slider keeps all its steps.
        const auto showCounts = [this, scale, steps] {
            QList<int> counts;
            for (const QString &step : steps) counts.append(liveCountOf(step));
            scale->setCounts(counts);
        };
        showCounts();
        scale->onChanged = [this, steps, showCounts, key](int value) {
            for (const QString &step : steps) m_selected.removeAll(step);
            if (value >= 0) m_selected.append(steps.at(value));
            // The count follows the selection, as on a chip.
            showCounts();
            updateRow(key);
            if (onChanged) onChanged();
        };
        body->addWidget(scale);
        layout->addWidget(section.body);
        column->addWidget(block);
        m_sections.append(section);
    }

    // The answer's free-form note: a caption and a box about 3 lines high, always shown below the
    // groups, at most groupNoteMaxLength() characters. Made once, so typing survives rebuild().
    QWidget *answerNoteSection()
    {
        auto *section = new QWidget(this);
        section->setObjectName(QStringLiteral("reviewAnswerNoteSection"));
        auto *column = new QVBoxLayout(section);
        column->setContentsMargins(0, 8, 0, 0);
        column->setSpacing(2);
        auto *caption = new QLabel(i18n("Note"), section);
        caption->setObjectName(QStringLiteral("reviewAnswerNoteCaption"));
        QFont captionFont = caption->font();
        captionFont.setPointSizeF(captionFont.pointSizeF() * 0.9);
        caption->setFont(captionFont);
        column->addWidget(caption);
        auto *note = new QPlainTextEdit(section);
        note->setObjectName(QStringLiteral("reviewAnswerNote"));
        note->setPlaceholderText(i18n("Anything else about this answer…"));
        note->setAccessibleName(i18n("Note"));
        note->setTabChangesFocus(true);
        note->setPlainText(m_notes.value(ReviewTags::answerNoteKey()));
        note->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        note->setFixedHeight(note->fontMetrics().lineSpacing() * 3 + int(std::ceil(2 * note->document()->documentMargin())) + 2 * note->frameWidth() + 2);
        caption->setBuddy(note);
        QObject::connect(note, &QPlainTextEdit::textChanged, this, [this, note] {
            QString text = note->toPlainText();
            if (const QString cut = ReviewTags::cutGroupNote(text); cut.size() < text.size()) {
                text = cut;
                const QSignalBlocker blocker(note);
                note->setPlainText(text);
                note->moveCursor(QTextCursor::End);
            }
            if (text.isEmpty()) m_notes.remove(ReviewTags::answerNoteKey());
            else m_notes.insert(ReviewTags::answerNoteKey(), text);
            if (onChanged) onChanged();
        });
        column->addWidget(note);
        return section;
    }

    // "About this…": one box for the note shared by every card of this card's section, page or
    // document, chosen with Section | Page | Document. Each scope keeps what was typed in it while
    // another is shown; the text goes into m_context, recorded with the answer.
    QWidget *contextSection()
    {
        auto *section = new QWidget(this);
        section->setObjectName(QStringLiteral("reviewContextNoteSection"));
        auto *column = new QVBoxLayout(section);
        column->setContentsMargins(0, 8, 0, 0);
        column->setSpacing(2);
        auto *caption = new QLabel(i18n("About this…"), section);
        caption->setObjectName(QStringLiteral("reviewContextNoteCaption"));
        QFont captionFont = caption->font();
        captionFont.setPointSizeF(captionFont.pointSizeF() * 0.9);
        caption->setFont(captionFont);
        column->addWidget(caption);
        auto *selector = new QWidget(section);
        selector->setObjectName(QStringLiteral("reviewContextScopes"));
        auto *row = new QHBoxLayout(selector);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto *buttons = new QButtonGroup(section);
        buttons->setExclusive(true);
        for (int index = 0; index < m_context->entries.size(); ++index) {
            const auto &entry = m_context->entries.at(index);
            const QString scope = entry.scope;
            const QString name = scope == ReviewContextNotes::sectionScope() ? i18n("Section")
                : scope == ReviewContextNotes::pageScope() ? i18n("Page") : i18n("Document");
            auto *button = new QPushButton(name, selector);
            button->setObjectName(QStringLiteral("reviewContextScope"));
            button->setProperty("reviewContextScope", scope);
            button->setCheckable(true);
            button->setAutoDefault(false);
            button->setEnabled(entry.available);
            if (!entry.available)
                button->setToolTip(scope == ReviewContextNotes::sectionScope() ? i18n("No section headings in this PDF")
                                   : scope == ReviewContextNotes::pageScope() ? i18n("This card has no page") : i18n("This card has no document"));
            buttons->addButton(button, index);
            row->addWidget(button);
        }
        row->addStretch(1);
        column->addWidget(selector);
        auto *about = new QLabel(section);
        about->setObjectName(QStringLiteral("reviewContextNoteLabel"));
        about->setWordWrap(true);
        about->setTextFormat(Qt::PlainText);
        column->addWidget(about);
        auto *note = new QPlainTextEdit(section);
        note->setObjectName(QStringLiteral("reviewContextNote"));
        note->setTabChangesFocus(true);
        note->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        note->setFixedHeight(note->fontMetrics().lineSpacing() * 3 + int(std::ceil(2 * note->document()->documentMargin())) + 2 * note->frameWidth() + 2);
        about->setBuddy(note);
        column->addWidget(note);
        const auto show = [this, buttons, about, note](int index) {
            if (index < 0 || index >= m_context->entries.size() || !m_context->entries.at(index).available) return;
            m_context->selected = index;
            const auto &entry = m_context->entries.at(index);
            const bool page = entry.scope == ReviewContextNotes::pageScope();
            about->setText(page ? i18n("Page %1", entry.note.key) : entry.note.label);
            note->setPlaceholderText(entry.scope == ReviewContextNotes::sectionScope() ? i18n("Anything about this section…")
                                     : page ? i18n("Anything about this page…") : i18n("Anything about this document…"));
            note->setAccessibleName(entry.scope == ReviewContextNotes::sectionScope() ? i18n("Note about this section")
                                    : page ? i18n("Note about this page") : i18n("Note about this document"));
            if (QAbstractButton *button = buttons->button(index)) button->setChecked(true);
            const QSignalBlocker blocker(note);
            note->setPlainText(entry.text);
            note->moveCursor(QTextCursor::End);
        };
        QObject::connect(buttons, &QButtonGroup::idClicked, this, show);
        QObject::connect(note, &QPlainTextEdit::textChanged, this, [this, note] {
            QString text = note->toPlainText();
            if (const QString cut = ReviewTags::cutGroupNote(text); cut.size() < text.size()) {
                text = cut;
                const QSignalBlocker blocker(note);
                note->setPlainText(text);
                note->moveCursor(QTextCursor::End);
            }
            // The draft may have been replaced meanwhile (another card shown): keep nothing then.
            if (m_context->selected >= 0 && m_context->selected < m_context->entries.size()) m_context->entries[m_context->selected].text = text;
            if (onChanged) onChanged();
        });
        int selected = m_context->selected;
        if (selected < 0 || selected >= m_context->entries.size() || !m_context->entries.at(selected).available) {
            const auto first = std::find_if(m_context->entries.cbegin(), m_context->entries.cend(), [](const auto &entry) { return entry.available; });
            selected = first == m_context->entries.cend() ? -1 : int(first - m_context->entries.cbegin());
        }
        if (selected < 0) {
            section->setEnabled(false);
            return section;
        }
        show(selected);
        return section;
    }

    // Each group's menu; Other's only takes tags back.
    QToolButton *groupMenu(const QString &id)
    {
        const QString name = nameOf(id);
        auto *button = new QToolButton(m_content);
        button->setObjectName(QStringLiteral("reviewAnswerTagGroupMenu"));
        button->setProperty("reviewTagGroup", id);
        button->setText(QStringLiteral("⋯"));
        button->setAutoRaise(true);
        button->setToolTip(id.isEmpty() ? i18n("Move tags to %1", name) : i18n("Rename, move, fill or delete %1", name));
        button->setAccessibleName(button->toolTip());
        button->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(button);
        const auto add = [this, menu](const QString &text, const char *objectName, std::function<void()> action) {
            QAction *item = menu->addAction(text);
            item->setObjectName(QString::fromLatin1(objectName));
            QObject::connect(item, &QAction::triggered, this, std::move(action));
            return item;
        };
        if (!id.isEmpty()) {
            const qsizetype index = std::distance(m_groups.cbegin(), std::find_if(m_groups.cbegin(), m_groups.cend(), [&id](const auto &group) { return group.id == id; }));
            add(i18n("Rename…"), "reviewTagGroupRename", [this, id] { renameGroup(id); });
            add(i18n("Section…"), "reviewTagGroupSection", [this, id] { editSection(id); });
            addPhaseMenu(menu, index < m_groups.size() ? m_groups.at(index).phase : QString(), [this, id](const QString &phase) { setPhase(id, phase); });
            add(i18n("Move up"), "reviewTagGroupMoveUp", [this, id] { moveGroup(id, -1); })->setEnabled(index > 0);
            add(i18n("Move down"), "reviewTagGroupMoveDown", [this, id] { moveGroup(id, 1); })->setEnabled(index + 1 < m_groups.size());
            menu->addSeparator();
            // Only sliders are listed: no turning one into tags, and no tags to move.
            add(i18n("Edit Slider…"), "reviewTagGroupEditSlider", [this, id] { editSlider(id); });
            menu->addSeparator();
            add(i18n("Delete…"), "reviewTagGroupDelete", [this, id] { deleteGroup(id); });
        }
        button->setMenu(menu);
        return button;
    }

    // Every tag of the other groups, under its group's heading, in full; the checked ones move here.
    void moveTagsHere(const QString &groupId)
    {
        const QString name = nameOf(groupId);
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("reviewMoveTagsDialog"));
        dialog.setWindowTitle(i18n("Move Tags to %1", name));
        dialog.resize(420, 420);
        auto *layout = new QVBoxLayout(&dialog);
        auto *prompt = new QLabel(i18n("Choose the tags to move to %1:", name), &dialog);
        prompt->setTextFormat(Qt::PlainText);
        prompt->setWordWrap(true);
        layout->addWidget(prompt);
        auto *list = new QTreeWidget(&dialog);
        list->setObjectName(QStringLiteral("reviewMoveTagsList"));
        list->setHeaderHidden(true);
        list->setColumnCount(1);
        list->setRootIsDecorated(false);
        list->setIndentation(16);
        list->setWordWrap(true);
        list->setUniformRowHeights(false);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setItemsExpandable(false);
        int candidates = 0;
        for (const auto &section : sections()) {
            if (keyOf(section.id) == keyOf(groupId) || section.tags.isEmpty()) continue;
            auto *heading = new QTreeWidgetItem(list, {section.id.isEmpty() ? i18n("Other") : section.name});
            heading->setFlags(Qt::ItemIsEnabled);
            QFont bold = heading->font(0);
            bold.setBold(true);
            heading->setFont(0, bold);
            heading->setData(0, Qt::UserRole + 1, section.id);
            for (const QString &tag : section.tags) {
                // Recorded answers only: this list is about where tags live, not this answer.
                const QString label = ReviewTags::withCount(reviewTagLabel(tag), countOf(tag));
                auto *item = new QTreeWidgetItem(heading, {label});
                item->setToolTip(0, label);
                item->setData(0, Qt::UserRole, tag);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setCheckState(0, Qt::Unchecked);
                ++candidates;
            }
        }
        list->expandAll();
        layout->addWidget(list, 1);
        if (!candidates) {
            auto *none = new QLabel(i18n("Every tag is already in %1.", name), &dialog);
            none->setObjectName(QStringLiteral("reviewMoveTagsNone"));
            layout->addWidget(none);
            list->hide();
        }
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        auto *move = buttons->button(QDialogButtonBox::Ok);
        move->setText(i18n("Move"));
        move->setEnabled(false);
        const auto checked = [list] {
            QStringList tags;
            for (int heading = 0; heading < list->topLevelItemCount(); ++heading) {
                for (int index = 0; index < list->topLevelItem(heading)->childCount(); ++index) {
                    const auto *item = list->topLevelItem(heading)->child(index);
                    if (item->checkState(0) == Qt::Checked) tags.append(item->data(0, Qt::UserRole).toString());
                }
            }
            return tags;
        };
        QObject::connect(list, &QTreeWidget::itemChanged, move, [move, checked] { move->setEnabled(!checked().isEmpty()); });
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        auto edited = m_groups;
        QString error;
        for (const QString &tag : checked()) {
            if (!ReviewTags::moveTag(edited, tag, groupId, &error)) {
                refuse(error);
                return;
            }
        }
        // The group opens to show the tags it took.
        rememberTagGroupOpen(keyOf(groupId), true);
        if (edited != m_groups) commit(edited);
        else applyOpen();
        fitWindow();
    }

    // Edit slider (or Show as Slider… for a chip group, starting with no steps): only the steps,
    // left to right, in ReviewSliderEditor. On Save a step that is not yet a tag of the group
    // joins it (a tag of another group moves here; a new name becomes a new tag); a removed step
    // stays in the group as a plain tag, a chip while cards have it. No answer changes.
    void editSlider(const QString &groupId)
    {
        const auto group = std::find_if(m_groups.cbegin(), m_groups.cend(), [&groupId](const auto &candidate) { return candidate.id == groupId; });
        if (group == m_groups.cend()) return;
        const QString title = group->scale.isEmpty() ? i18n("Make “%1” a slider", group->name) : i18n("Edit slider “%1”", group->name);
        ReviewSliderEditor dialog(title, group->scale, ReviewTags::byUsage(group->tags, m_usage, reviewTagLabel),
                                  [this](const QString &text) { return typedTag(text); }, this);
        if (dialog.exec() != QDialog::Accepted) return;
        const QStringList steps = dialog.steps();
        auto edited = m_groups;
        QString error;
        for (const QString &step : steps) {
            if (ReviewTags::groupOf(edited, step) == groupId) continue;
            if (!ReviewTags::moveTag(edited, step, groupId, &error)) {
                refuse(error);
                return;
            }
        }
        if (!ReviewTags::setScale(edited, groupId, steps, &error)) {
            refuse(error);
            return;
        }
        for (const QString &step : steps) {
            if (m_known.contains(step)) continue;
            rememberReviewTag(step);
            m_known.append(step);
        }
        if (edited != m_groups) commit(edited);
    }

    // The steps become chips again; no tag changes.
    void showAsTags(const QString &groupId)
    {
        auto edited = m_groups;
        QString error;
        if (!ReviewTags::setScale(edited, groupId, {}, &error)) refuse(error);
        else if (edited != m_groups) {
            rememberTagGroupOpen(keyOf(groupId), true);
            commit(edited);
            fitWindow();
        }
    }

    QString askName(const QString &title, const QString &label, const QString &initial = {})
    {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &accepted).trimmed();
        return accepted ? name : QString();
    }

    // Every edit writes the whole record; on failure nothing changes.
    bool commit(const QList<ReviewTags::TagGroup> &edited)
    {
        QString error;
        if (!m_store || !m_store->saveTagGroups(edited, &error)) {
            KMessageBox::error(this, error.isEmpty() ? i18n("The tag groups could not be saved.") : error, i18n("Tag Groups"));
            return false;
        }
        m_groups = edited;
        rebuild();
        return true;
    }

    bool refuse(const QString &error)
    {
        KMessageBox::error(this, error, i18n("Tag Groups"));
        return false;
    }

    void createTag(const QString &groupId)
    {
        bool accepted = false;
        // A tag hidden at 0 (or an old name merged into one) is picked, not made again.
        const QString tag = typedTag(QInputDialog::getText(this, i18n("New tag"), i18n("Tag name:"), QLineEdit::Normal, {}, &accepted));
        if (!accepted || tag.isEmpty()) return;
        if (!m_known.contains(tag)) {
            rememberReviewTag(tag);
            m_known.append(tag);
        }
        if (!m_selected.contains(tag)) m_selected.append(tag);
        // A new tag goes in the group it was made in; an existing one stays in its own group.
        bool rebuilt = false;
        if (!groupId.isEmpty() && ReviewTags::groupOf(m_groups, tag).isEmpty()) {
            auto edited = m_groups;
            QString error;
            if (ReviewTags::moveTag(edited, tag, groupId, &error)) rebuilt = commit(edited);
            else refuse(error);
        }
        if (!rebuilt) rebuild();
        if (onChanged) onChanged();
    }

    void createGroup()
    {
        const QString name = askName(i18n("New group"), i18n("Group name:"));
        if (name.isEmpty()) return;
        auto edited = m_groups;
        QString error;
        if (!ReviewTags::addGroup(edited, name, &error)) {
            refuse(error);
            return;
        }
        // Like any group made later, it starts closed.
        commit(edited);
    }

    void renameGroup(const QString &groupId)
    {
        const auto group = std::find_if(m_groups.cbegin(), m_groups.cend(), [&groupId](const auto &candidate) { return candidate.id == groupId; });
        if (group == m_groups.cend()) return;
        const QString name = askName(i18n("Rename group"), i18n("Group name:"), group->name);
        if (name.isEmpty() || name == group->name) return;
        auto edited = m_groups;
        QString error;
        if (!ReviewTags::renameGroup(edited, groupId, name, &error)) refuse(error);
        else commit(edited);
    }

    // Section…: the name of the section the group shows under, typed or picked from the sections
    // there are; empty (or No Section) for none. Only adjacent groups show together.
    void editSection(const QString &groupId)
    {
        const auto group = std::find_if(m_groups.cbegin(), m_groups.cend(), [&groupId](const auto &candidate) { return candidate.id == groupId; });
        if (group == m_groups.cend()) return;
        const QString current = group->section;
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("reviewTagSectionDialog"));
        dialog.setWindowTitle(i18n("Section of %1", group->name));
        auto *layout = new QVBoxLayout(&dialog);
        auto *prompt = new QLabel(i18n("Show “%1” under the section:", group->name), &dialog);
        prompt->setTextFormat(Qt::PlainText);
        prompt->setWordWrap(true);
        layout->addWidget(prompt);
        auto *name = new QLineEdit(current, &dialog);
        name->setObjectName(QStringLiteral("reviewTagSectionName"));
        name->setPlaceholderText(i18n("No section"));
        name->setMaxLength(ReviewTags::sectionMaxLength());
        prompt->setBuddy(name);
        layout->addWidget(name);
        QStringList picks = ReviewTags::sectionNames(m_groups);
        picks.removeAll(current);
        if (!picks.isEmpty()) {
            auto *caption = new QLabel(i18n("Or pick a section:"), &dialog);
            caption->setObjectName(QStringLiteral("reviewTagSectionPicksCaption"));
            layout->addSpacing(6);
            layout->addWidget(caption);
            auto *chips = new QWidget(&dialog);
            chips->setObjectName(QStringLiteral("reviewTagSectionPicks"));
            chips->setStyleSheet(reviewTagChipStyle());
            auto *flow = new ReviewFlowLayout(chips);
            for (const QString &pick : std::as_const(picks)) {
                auto *chip = new QPushButton(pick, chips);
                chip->setObjectName(QStringLiteral("reviewTagSectionPick"));
                chip->setProperty("reviewTagSection", pick);
                chip->setAutoDefault(false);
                chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
                QObject::connect(chip, &QPushButton::clicked, &dialog, [&dialog, name, pick] {
                    name->setText(pick);
                    dialog.accept();
                });
                flow->addWidget(chip);
            }
            layout->addWidget(chips);
        }
        auto *hint = new QLabel(i18n("Groups next to each other with the same section show together under it."), &dialog);
        hint->setObjectName(QStringLiteral("reviewTagSectionHint"));
        hint->setWordWrap(true);
        layout->addSpacing(6);
        layout->addWidget(hint);
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        box->setObjectName(QStringLiteral("reviewTagSectionButtons"));
        if (!current.isEmpty()) {
            auto *remove = box->addButton(i18n("No Section"), QDialogButtonBox::ResetRole);
            remove->setObjectName(QStringLiteral("reviewTagSectionRemove"));
            QObject::connect(remove, &QPushButton::clicked, &dialog, [&dialog, name] {
                name->clear();
                dialog.accept();
            });
        }
        QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(box);
        name->setFocus();
        name->selectAll();
        if (dialog.exec() != QDialog::Accepted) return;
        const QString section = name->text().trimmed();
        if (section == current) return;
        auto edited = m_groups;
        QString error;
        if (!ReviewTags::setSection(edited, groupId, section, &error)) refuse(error);
        else {
            commit(edited);
            fitWindow();
        }
    }

    void moveGroup(const QString &groupId, int offset)
    {
        auto edited = m_groups;
        if (ReviewTags::moveGroup(edited, groupId, offset)) commit(edited);
    }

    // Ask: when the group is asked. One asked before the answer only leaves this list; the before
    // panel of the next card asks it.
    void setPhase(const QString &groupId, const QString &phase)
    {
        auto edited = m_groups;
        QString error;
        if (!ReviewTags::setPhase(edited, groupId, phase, &error)) refuse(error);
        else if (edited != m_groups) {
            commit(edited);
            fitWindow();
        }
    }

    void deleteGroup(const QString &groupId)
    {
        const auto group = std::find_if(m_groups.cbegin(), m_groups.cend(), [&groupId](const auto &candidate) { return candidate.id == groupId; });
        if (group == m_groups.cend()) return;
        if (KMessageBox::warningContinueCancel(this, i18n("Delete the group “%1”? Its tags move to Other; no tag is deleted.", group->name),
                                               i18n("Delete Group"), KGuiItem(i18n("Delete"))) != KMessageBox::Continue)
            return;
        auto edited = m_groups;
        if (ReviewTags::deleteGroup(edited, groupId)) commit(edited);
    }

    ReviewContextNotes::Draft *m_context = nullptr; // The section, page and document notes; none outside a review.
    QStringList m_known;
    QStringList m_selected;
    QHash<QString, QString> m_notes;
    QHash<QString, QString> m_tagMerges;
    QHash<QString, int> m_usage;
    QStringList m_current; // The card's current tags, which countOf() already counts it under.
    QSet<QString> m_shown; // Tags given a chip since the picker opened.
    const ReviewStore *m_store = nullptr;
    QList<ReviewTags::TagGroup> m_groups;
    QList<Section> m_sections;
    QList<SectionRun> m_runs;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;
    std::vector<std::unique_ptr<QWidget>> m_retired;
};

// Skip's default interval, which the Skip dialog opens with and S uses: ten minutes until the
// user saves another from the dialog.
KConfigGroup skipDefaultConfig()
{
    return KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("ReviewSkip"));
}

ReviewInterval savedSkipInterval()
{
    const KConfigGroup group = skipDefaultConfig();
    const ReviewInterval saved{group.readEntry("DefaultAmount", 10.0), ReviewInterval::Unit(group.readEntry("DefaultUnit", int(ReviewInterval::Minutes))), {}};
    if (saved.unit < ReviewInterval::Minutes || saved.unit > ReviewInterval::Months || !saved.dueAt(QDateTime::currentDateTimeUtc()).isValid())
        return ReviewInterval{10, ReviewInterval::Minutes, {}};
    return saved;
}

void saveSkipInterval(const ReviewInterval &interval)
{
    KConfigGroup group = skipDefaultConfig();
    group.writeEntry("DefaultAmount", interval.amount);
    group.writeEntry("DefaultUnit", int(interval.unit));
    group.sync();
}

bool chooseReviewInterval(QWidget *parent, ReviewInterval *interval, ReviewSkipScope *scope = nullptr,
                          const QString &document = {}, const QString &project = {}, bool *dueOnly = nullptr, bool *never = nullptr,
                          const QString &currentInterval = {}, QStringList *tags = nullptr, const QStringList &availableTags = {},
                          const QHash<QString, QString> &tagMerges = {}, const ReviewStore *tagStore = nullptr,
                          QHash<QString, QString> *groupNotes = nullptr, const QHash<QString, int> &tagUsage = {},
                          const QStringList &currentTags = {}, ReviewContextNotes::Draft *context = nullptr)
{
    QDialog dialog(parent);
    dialog.setObjectName(tags ? QStringLiteral("reviewAnswerDialog") : scope ? QStringLiteral("reviewSkipDialog") : QStringLiteral("reviewIntervalDialog"));
    const bool historyUpdate = !scope && never;
    dialog.setWindowTitle(tags ? i18n("Review") : scope ? i18n("Skip") : historyUpdate ? i18n("Update history") : i18n("Review again in"));
    dialog.resize(430, 230);
    auto *layout = new QVBoxLayout(&dialog);
    if (historyUpdate) layout->addWidget(new QLabel(i18n("Current interval: %1", currentInterval), &dialog));
    auto *form = new QFormLayout;
    auto *amount = new ReviewAmountSpinBox(&dialog);
    amount->setObjectName(QStringLiteral("reviewIntervalAmount"));
    amount->setDecimals(6);
    amount->setRange(0.000001, 1000000);
    amount->setSingleStep(0.5);
    auto *unit = new QComboBox(&dialog);
    unit->setObjectName(QStringLiteral("reviewIntervalUnit"));
    unit->addItem(i18n("minutes"), ReviewInterval::Minutes);
    unit->addItem(i18n("hours"), ReviewInterval::Hours);
    unit->addItem(i18n("days"), ReviewInterval::Days);
    unit->addItem(i18n("weeks"), ReviewInterval::Weeks);
    unit->addItem(i18n("months"), ReviewInterval::Months);
    const int initialUnit = unit->findData(interval->unit);
    unit->setCurrentIndex(initialUnit >= 0 ? initialUnit : 0);
    amount->setValue(initialUnit >= 0 ? interval->amount : 1);
    auto *row = new QHBoxLayout;
    row->addWidget(amount, 1);
    row->addWidget(unit);
    form->addRow(tags ? i18n("Review again in") : scope ? i18n("Hide for") : historyUpdate ? i18n("New interval") : i18n("Review again in"), row);
    auto *useDate = new QCheckBox(i18n("Choose a date instead"), &dialog);
    useDate->setObjectName(QStringLiteral("reviewIntervalUseDate"));
    useDate->setChecked(interval->date.isValid());
    if (!scope && !historyUpdate) form->addRow(useDate);
    auto *date = new QDateEdit(&dialog);
    date->setObjectName(QStringLiteral("reviewIntervalDate"));
    date->setCalendarPopup(true);
    date->setMinimumDate(QDate::currentDate().addDays(1));
    date->setDate(interval->date.isValid() ? interval->date : QDate::currentDate().addDays(14));
    if (!scope && !historyUpdate) form->addRow(i18n("Date"), date);
    layout->addLayout(form);
    auto *preview = new QLabel(&dialog);
    preview->setObjectName(QStringLiteral("reviewIntervalDue"));
    layout->addWidget(preview);
    QCheckBox *applyDocument = nullptr;
    QCheckBox *applyProject = nullptr;
    QRadioButton *onlyDue = nullptr;
    QGroupBox *whichCards = nullptr;
    QCheckBox *neverBox = nullptr;
    QPushButton *customDate = nullptr;
    QPushButton *randomDate = nullptr;
    QCheckBox *saveDefault = nullptr;
    if (scope || historyUpdate) {
        useDate->hide();
        if (scope) {
            customDate = new QPushButton(i18n("Custom…"), &dialog);
            customDate->setObjectName(QStringLiteral("reviewIntervalCustomDate"));
            auto *dateButtons = new QHBoxLayout;
            dateButtons->addWidget(customDate);
            if (!tags) {
                // Skip only: a random number of days, mostly within two weeks; the user still confirms it.
                randomDate = new QPushButton(i18n("Random"), &dialog);
                randomDate->setObjectName(QStringLiteral("reviewSkipRandom"));
                randomDate->setToolTip(i18n("Pick a random number of days, at least 2, within 14 days 95% of the time"));
                dateButtons->addWidget(randomDate);
                // Sets the interval to a whole number of days (the date option off); each click samples again.
                QObject::connect(randomDate, &QPushButton::clicked, &dialog, [useDate, amount, unit] {
                    useDate->setChecked(false);
                    unit->setCurrentIndex(unit->findData(ReviewInterval::Days));
                    amount->setValue(randomSkipDays(randomSkipUniformSource()()));
                });
            }
            dateButtons->addStretch();
            layout->addLayout(dateButtons);
            layout->addWidget(date);
            if (!tags) {
                saveDefault = new QCheckBox(i18n("Make this the default for Skip and S"), &dialog);
                saveDefault->setObjectName(QStringLiteral("reviewSkipSaveDefault"));
                saveDefault->setToolTip(i18n("The Skip dialog opens with this interval, and S skips by it"));
                layout->addWidget(saveDefault);
            }
            QObject::connect(customDate, &QPushButton::clicked, useDate, [useDate] { useDate->setChecked(!useDate->isChecked()); });
        }
        neverBox = new QCheckBox(i18n("Never"), &dialog);
        neverBox->setObjectName(scope ? QStringLiteral("reviewSkipNever") : QStringLiteral("reviewIntervalNever"));
        neverBox->setChecked(never && *never);
        layout->addWidget(neverBox);
        // Skip only postpones; offer Never only to a caller that records it.
        if (!never) neverBox->hide();
    }
    ReviewTagPicker *tagPicker = nullptr;
    std::function<void()> refreshApply; // Set once the preview updater exists.
    if (tags) {
        auto *tagsLabel = new QLabel(i18n("Rating"), &dialog);
        QFont tagsFont = tagsLabel->font();
        tagsFont.setBold(true);
        tagsLabel->setFont(tagsFont);
        layout->addSpacing(6);
        layout->addWidget(tagsLabel);
        tagPicker = new ReviewTagPicker(availableTags, *tags, tagMerges, tagStore, &dialog, groupNotes ? *groupNotes : QHash<QString, QString>(),
                                        tagUsage, currentTags, context);
        tagPicker->onChanged = [&refreshApply] { if (refreshApply) refreshApply(); };
        layout->addWidget(tagPicker);
    }
    const auto selectedTags = [tagPicker] { return tagPicker ? tagPicker->selectedTags() : QStringList(); };
    if (scope) {
        applyDocument = new QCheckBox(i18n("Apply to all cards in this document"), &dialog);
        applyDocument->setObjectName(QStringLiteral("reviewSkipDocument"));
        applyDocument->setToolTip(document);
        applyProject = new QCheckBox(i18n("Apply to all cards in this project"), &dialog);
        applyProject->setObjectName(QStringLiteral("reviewSkipProject"));
        applyProject->setEnabled(!project.isEmpty());
        applyProject->setToolTip(project.isEmpty() ? i18n("Project information is unavailable") : project);
        layout->addWidget(applyDocument);
        layout->addWidget(applyProject);
        whichCards = new QGroupBox(i18n("Which cards?"), &dialog);
        whichCards->setObjectName(QStringLiteral("reviewSkipWhichCards"));
        auto *whichLayout = new QVBoxLayout(whichCards);
        onlyDue = new QRadioButton(i18n("Only cards due now"), whichCards);
        onlyDue->setObjectName(QStringLiteral("reviewSkipDueOnly"));
        onlyDue->setChecked(true);
        auto *allCards = new QRadioButton(i18n("All cards, including those due later"), whichCards);
        allCards->setObjectName(QStringLiteral("reviewSkipAllCards"));
        whichLayout->addWidget(onlyDue);
        whichLayout->addWidget(allCards);
        layout->addWidget(whichCards);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    if (scope) buttons->setLayoutDirection(Qt::RightToLeft);
    auto *apply = buttons->button(QDialogButtonBox::Ok);
    apply->setText(scope ? i18n("Next") : historyUpdate ? i18n("Save interval") : i18n("Apply"));
    if (scope) {
        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch();
        buttonRow->addWidget(buttons);
        layout->addLayout(buttonRow);
    } else {
        layout->addWidget(buttons);
    }
    const auto selection = [&] {
        return ReviewInterval{amount->value(), ReviewInterval::Unit(unit->currentData().toInt()), useDate->isChecked() ? date->date() : QDate{}};
    };
    const auto update = [&] {
        const bool neverSelected = neverBox && neverBox->isChecked();
        amount->setEnabled(!useDate->isChecked() && !neverSelected);
        unit->setEnabled(!useDate->isChecked() && !neverSelected);
        date->setEnabled(useDate->isChecked() && !neverSelected);
        if (scope) {
            date->setVisible(useDate->isChecked());
            customDate->setEnabled(!neverSelected);
            if (randomDate) randomDate->setEnabled(!neverSelected);
            // A date is not an interval to reuse.
            if (saveDefault) saveDefault->setEnabled(!useDate->isChecked() && !neverSelected);
        }
        const auto due = selection().dueAt(QDateTime::currentDateTimeUtc());
        // An answer may have no tag, as Next records it.
        apply->setEnabled(neverSelected || due.isValid());
        preview->setText(neverSelected ? i18n("Due Never") : due.isValid() ? i18n("Due %1", QLocale().dayName(due.toLocalTime().date().dayOfWeek()) + QStringLiteral(", ") + QLocale().toString(due.toLocalTime(), QLocale::ShortFormat)) : i18n("Choose a future review time."));
        if (scope) {
            const bool multiple = applyDocument->isChecked() || applyProject->isChecked();
            whichCards->setEnabled(multiple);
        }
    };
    QObject::connect(amount, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, update);
    QObject::connect(unit, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, update);
    QObject::connect(useDate, &QCheckBox::toggled, &dialog, update);
    QObject::connect(date, &QDateEdit::dateChanged, &dialog, update);
    if (neverBox) QObject::connect(neverBox, &QCheckBox::toggled, &dialog, update);
    refreshApply = update;
    if (scope) {
        QObject::connect(applyDocument, &QCheckBox::toggled, &dialog, update);
        QObject::connect(applyProject, &QCheckBox::toggled, &dialog, update);
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        amount->interpretText();
        update();
        if (apply->isEnabled()) dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    if (dialog.exec() != QDialog::Accepted) return false;
    *interval = selection();
    if (saveDefault && saveDefault->isEnabled() && saveDefault->isChecked()) saveSkipInterval(*interval);
    if (dueOnly) *dueOnly = onlyDue && onlyDue->isChecked();
    if (never) *never = neverBox && neverBox->isChecked();
    if (tags) *tags = selectedTags();
    if (tagPicker && groupNotes) *groupNotes = tagPicker->groupNotes();
    if (scope) *scope = applyProject->isChecked() ? ReviewSkipScope::Project : applyDocument->isChecked() ? ReviewSkipScope::Document : ReviewSkipScope::Card;
    return true;
}

enum class BrowserSort {
    Due,
    Question,
    Deck,
    Page,
    Updated,
    Interval,
    Reviews,
    Lapses,
    Created,
    Project,
    Source,
    State,
    CardStatus,
};

QString browserCardStateLabel(const ReviewBrowserCard &item)
{
    if (!item.card.wasAnswered()) return i18n("New");
    const auto tags = item.card.currentTags();
    return tags.isEmpty() ? i18n("Not rated") : reviewTagLabels(tags);
}

QString browserIntervalLabel(const ReviewCard &card)
{
    const QString label = currentIntervalLabel(card);
    return label == i18n("No interval selected") ? QStringLiteral("—") : label;
}

// Chosen interval length in milliseconds, matching browserIntervalLabel; Never sorts last, no interval first.
qint64 browserIntervalSortKey(const ReviewCard &card)
{
    if (card.schedule.retained) return std::numeric_limits<qint64>::max();
    if (card.schedule.lastReviewedAt.isValid() && card.schedule.dueAt > card.schedule.lastReviewedAt)
        return card.schedule.lastReviewedAt.msecsTo(card.schedule.dueAt);
    return card.schedule.intervalDays > 0 ? qint64(card.schedule.intervalDays) * 86400000 : -1;
}

enum class BrowserCardStatus { PdfOnly, WholeDocument, LinkRemoved, SourceUnavailable, LinkFailed, LinkPending, PlacementPending, Linked, PlacementFailed };

BrowserCardStatus browserCardStatus(const ReviewBrowserCard &item)
{
    const auto &card = item.card;
    const QString location = card.sourceLocation.value(QStringLiteral("state")).toString();
    const QString link = card.record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString();
    const auto primary = primaryObject(card);
    const bool anchored = !primary.value(QStringLiteral("source_anchor")).toObject().isEmpty();
    const bool pdfOnly = primary.value(QStringLiteral("source_kind")).toString() == QLatin1String("pdf") && !anchored;
    const QString reanchor = card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
    const bool placed = (QStringList{QStringLiteral("succeeded"), QStringLiteral("current"), QStringLiteral("ok"),
        QStringLiteral("exact_text"), QStringLiteral("exact_point")}.contains(reanchor) || (pdfOnly && reanchor.isEmpty()))
        && item.state != ReviewBrowserState::Stale && item.state != ReviewBrowserState::NeedsAttention
        && item.state != ReviewBrowserState::Repairing;
    if (pdfOnly && placed) return BrowserCardStatus::PdfOnly;
    if (card.isWholeDocument()) {
        if (QStringList{QStringLiteral("queued"), QStringLiteral("retry"), QStringLiteral("running"),
            QStringLiteral("updating")}.contains(reanchor) || item.state == ReviewBrowserState::Stale)
            return BrowserCardStatus::PlacementPending;
        return item.state == ReviewBrowserState::NeedsAttention || item.state == ReviewBrowserState::Repairing
            ? BrowserCardStatus::PlacementFailed : BrowserCardStatus::WholeDocument;
    }
    if (location == QLatin1String("missing")) return BrowserCardStatus::LinkRemoved;
    if (location == QLatin1String("unavailable") || location == QLatin1String("ambiguous")) return BrowserCardStatus::SourceUnavailable;
    if (!anchored && (link == QLatin1String("linking_failed") || link == QLatin1String("failed"))) return BrowserCardStatus::LinkFailed;
    if (!anchored && !pdfOnly) return BrowserCardStatus::LinkPending;
    if (QStringList{QStringLiteral("queued"), QStringLiteral("retry"), QStringLiteral("running"),
        QStringLiteral("updating")}.contains(reanchor) || item.state == ReviewBrowserState::Stale)
        return BrowserCardStatus::PlacementPending;
    return placed ? BrowserCardStatus::Linked : BrowserCardStatus::PlacementFailed;
}

QString browserCardStatusLabel(const ReviewBrowserCard &item)
{
    switch (browserCardStatus(item)) {
    case BrowserCardStatus::PdfOnly: return i18n("PDF only");
    case BrowserCardStatus::WholeDocument: return i18n("Whole document");
    case BrowserCardStatus::LinkRemoved: return i18n("Link removed");
    case BrowserCardStatus::SourceUnavailable: return i18n("Source unavailable");
    case BrowserCardStatus::LinkFailed: return i18n("Link failed");
    case BrowserCardStatus::LinkPending: return i18n("Link pending");
    case BrowserCardStatus::PlacementPending: return i18n("Placement pending");
    case BrowserCardStatus::Linked: return i18n("Linked");
    case BrowserCardStatus::PlacementFailed: return i18n("Placement failed");
    }
    return {};
}

// The Type filter groups the Card Status column: linked, PDF only, still linking, or a link problem.
QString browserCardType(const ReviewBrowserCard &item)
{
    switch (browserCardStatus(item)) {
    case BrowserCardStatus::Linked: return QStringLiteral("linked");
    case BrowserCardStatus::PdfOnly:
    case BrowserCardStatus::WholeDocument: return QStringLiteral("pdf");
    case BrowserCardStatus::LinkPending:
    case BrowserCardStatus::PlacementPending: return QStringLiteral("pending");
    default: return QStringLiteral("problem");
    }
}

bool browserStateIsActive(ReviewBrowserState state)
{
    return state == ReviewBrowserState::New || state == ReviewBrowserState::Learning
        || state == ReviewBrowserState::Review || state == ReviewBrowserState::Future || state == ReviewBrowserState::Skipped;
}

QString browserUnavailableReason(ReviewBrowserState state)
{
    switch (state) {
    case ReviewBrowserState::Repairing:
        return i18n("Location updating: the answer’s position is being checked. This card will return when its location is verified.");
    case ReviewBrowserState::NeedsAttention:
        return i18n("Location repair failed: Thoughts could not locate the answer reliably. Its location needs repair before you can review this card.");
    case ReviewBrowserState::Stale:
        return i18n("Location needs updating: the saved answer position was verified for a different PDF version. The PDF on this device may already be current.");
    default:
        return {};
    }
}

bool browserStateIsDue(ReviewBrowserState state)
{
    return state == ReviewBrowserState::New || state == ReviewBrowserState::Learning || state == ReviewBrowserState::Review;
}

const QList<BrowserSort> browserColumnSorts = {BrowserSort::Question, BrowserSort::Due, BrowserSort::Project,
    BrowserSort::Deck, BrowserSort::Page, BrowserSort::Source, BrowserSort::State, BrowserSort::CardStatus, BrowserSort::Created, BrowserSort::Interval};

Qt::SortOrder defaultBrowserSortOrder(BrowserSort sort)
{
    return sort == BrowserSort::Updated || sort == BrowserSort::Interval || sort == BrowserSort::Reviews || sort == BrowserSort::Lapses
        ? Qt::DescendingOrder : Qt::AscendingOrder;
}

QStringList sourceFiles(const ReviewCard &card)
{
    if (card.sourceLocation.value(QStringLiteral("state")).toString() == QLatin1String("linked")) return {card.linkingSource()};
    QStringList files;
    const QJsonObject primary = primaryObject(card);
    for (const QJsonValue &value : primary.value(QStringLiteral("source_files")).toArray()) {
        const QString file = value.toString().trimmed();
        if (!file.isEmpty() && !files.contains(file)) {
            files.append(file);
        }
    }
    const QJsonArray segments = primary.value(primary.contains(QStringLiteral("source_anchor")) ? QStringLiteral("source_anchor") : QStringLiteral("source_selection")).toObject().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue &value : segments) {
        const QString file = value.toObject().value(QStringLiteral("source_file")).toString().trimmed();
        if (!file.isEmpty() && !files.contains(file)) {
            files.append(file);
        }
    }
    return files;
}

// "Retired 2026-10-27 14:02": the state a retired card is shown with.
QString retiredLabel(const ReviewCard &card)
{
    return card.schedule.retiredAt.isValid() ? i18n("Retired %1", card.schedule.retiredAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                             : i18n("Retired");
}

// Delete is permanent; its confirmation points to Retire, which keeps the history.
QString deleteCardQuestion()
{
    return i18n("Permanently delete this review card? It and its history cannot be restored.\n\n"
                "Retire card instead stops reviewing it and keeps its history; it can be restored to review later.");
}

// "Changed since you last saw it": a project card whose passage changed since its latest answer.
QString passageChangedText()
{
    return i18n("Changed since you last saw it");
}

// What a card's kind is and where it comes from (review-card-kinds.md §3, §5).
QString cardKindText(const ReviewCard &card)
{
    if (card.reviewKindSource == ReviewKinds::sourceCard())
        return card.isProjectCard() ? i18n("Project card, chosen for this card") : i18n("Knowledge card, chosen for this card");
    if (card.reviewKindSource == ReviewKinds::sourceProject())
        return card.isProjectCard() ? i18n("Project card, as its Project") : i18n("Knowledge card, as its Project");
    return card.isProjectCard() ? i18n("Project card") : i18n("Knowledge card");
}

// The card menu's kind entries (review-card-kinds.md §5), as {label, kind}: Make project card or
// Make knowledge card, then Follow the Project when the card has a kind of its own and a Project holds it.
QList<QPair<QString, QString>> cardKindChoices(const ReviewCard &card)
{
    QList<QPair<QString, QString>> choices{card.isProjectCard() ? qMakePair(i18n("Make knowledge card"), ReviewKinds::knowledge())
                                                                : qMakePair(i18n("Make project card"), ReviewKinds::project())};
    if (card.reviewKindSource == ReviewKinds::sourceCard() && !card.kindProjectIds.isEmpty())
        choices.append({card.projectsKind == ReviewKinds::project() ? i18n("Follow the Project (Project)") : i18n("Follow the Project (Knowledge)"),
                        ReviewKinds::followProject()});
    return choices;
}

QString browserDueText(const ReviewBrowserCard &item)
{
    if (item.card.schedule.retired) return i18n("Retired");
    if (item.card.schedule.retained) return i18n("Never");
    // A skip hides the card without moving its due date; the state column says Skipped.
    const auto due = item.card.schedule.dueAt;
    if (due.isValid()) return due.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return item.card.wasAnswered() ? QStringLiteral("—") : i18n("New");
}

QDateTime recordDate(const ReviewCard &card, const QString &key)
{
    return QDateTime::fromString(card.record.value(key).toString(), Qt::ISODate).toUTC();
}

// One entry of a card's History.
struct HistoryEntry {
    QDateTime at; // Invalid: kept right after the entry before it.
    QString html; // The entry's text, without the version link.
    QString versionSha256; // The PDF version it was made on, when recorded.
    int page = 0;
    QVariantList regions; // Answer feedback: its passage in that version.
    QJsonObject feedback; // Answer feedback: its record.
    bool cardEvent = false; // Created or approved: not made on a version.
    QString answerEventId = {}; // An answer still in effect, which "Take back this answer…" retracts.
};

QString historyTime(const QDateTime &at)
{
    return at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QDateTime historyTimestamp(const QString &value)
{
    const QDateTime at = QDateTime::fromString(value, Qt::ISODateWithMs);
    return at.isValid() ? at : QDateTime::fromString(value, Qt::ISODate);
}

// @p groups (the current record) name the groups of the notes an answer recorded.
QList<HistoryEntry> reviewHistoryEntries(const ReviewCard &card, const QList<ReviewTags::TagGroup> &groups)
{
    QList<HistoryEntry> entries;
    const auto timestamp = [&card](const QString &key) {
        const auto at = QDateTime::fromString(card.record.value(key).toString(), Qt::ISODate);
        return at.isValid() ? at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    };
    const auto recordTime = [&card](const QString &key) { return QDateTime::fromString(card.record.value(key).toString(), Qt::ISODate); };
    if (const QString created = timestamp(QStringLiteral("created_at")); !created.isEmpty())
        entries.append({recordTime(QStringLiteral("created_at")), i18n("Created · %1", created).toHtmlEscaped(), {}, 0, {}, {}, true});
    // Cards are approved as they are created; only an older card approved later shows its approval.
    if (const QString approved = timestamp(QStringLiteral("approved_at")); !approved.isEmpty() && approved != timestamp(QStringLiteral("created_at")))
        entries.append({recordTime(QStringLiteral("approved_at")), i18n("Approved · %1", approved).toHtmlEscaped(), {}, 0, {}, {}, true});
    for (const auto &event : card.history) {
        // Events made since 2026-10-21 name the PDF they were made on.
        const QString version = PdfVersions::recordedSha256(event);
        if (event.value(QStringLiteral("interval_action")).toString() == QLatin1String("set")) {
            const QDateTime at = QDateTime::fromString(event.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODate);
            const QDateTime due = QDateTime::fromString(event.value(QStringLiteral("scheduled_due_at")).toString(), Qt::ISODate);
            const QString label = recordedIntervalLabel(event);
            if (!at.isValid() || label.isEmpty()) continue;
            QString text = i18n("Interval set") + QStringLiteral(" · ")
                + at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) + QStringLiteral(" · ") + label;
            if (due.isValid()) text += QStringLiteral(" · ") + i18n("Next review %1", due.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            entries.append({at, text.toHtmlEscaped(), version, card.page(), {}, {}, false});
            continue;
        }
        const auto action = event.value(QStringLiteral("queue_action")).toString();
        const auto rating = event.value(QStringLiteral("rating")).toString();
        const auto reviewAction = event.value(QStringLiteral("review_action")).toString();
        QString mark;
        // Like Android, merged names apply to tags an answer recorded; older answers keep their fixed labels.
        const QStringList explicitTags = event.value(QStringLiteral("review_tags")).toArray().isEmpty() ? QStringList{} : card.tagsOfEvent(event);
        if (!explicitTags.isEmpty() && event.value(QStringLiteral("retracted_event_id")).toString().isEmpty()) mark = reviewTagLabels(explicitTags);
        else if (action == QLatin1String("skip")) mark = i18n("Skip");
        // An answer given with no tag records an empty list.
        else if (event.value(QStringLiteral("review_tags")).isArray() && event.value(QStringLiteral("review_tags")).toArray().isEmpty() && action.isEmpty()
                 && event.value(QStringLiteral("retracted_event_id")).toString().isEmpty() && !rating.isEmpty() && rating != QLatin1String("delete"))
            mark = i18n("Not rated");
        else if (action == QLatin1String("undo_skip")) mark = i18n("Undo Skip");
        else if (action == ReviewKinds::action()) {
            const QString kind = event.value(ReviewKinds::field()).toString();
            mark = kind == ReviewKinds::project() ? i18n("Made a project card")
                : kind == ReviewKinds::knowledge() ? i18n("Made a knowledge card")
                : kind == ReviewKinds::followProject() ? i18n("Follows the Project's kind") : QString();
        }
        else if (action == ReviewRetire::retireAction()) mark = i18n("Retired");
        else if (action == ReviewRetire::restoreAction()) mark = i18n("Restored to review");
        else if (action == ReviewRetire::undoAction()) mark = i18n("Undo retire");
        else if (!event.value(QStringLiteral("retracted_event_id")).toString().isEmpty()) mark = i18n("Undo review");
        else if (rating == QLatin1String("delete")) mark = i18n("Delete");
        else if (reviewAction == QLatin1String("skip")) mark = i18n("Skip");
        else if (reviewAction == QLatin1String("wrong") || reviewAction == QLatin1String("error") || rating == QLatin1String("again") || rating == QLatin1String("ten_minutes")) mark = i18n("Wrong");
        else if (reviewAction == QLatin1String("correct") || rating == QLatin1String("custom_interval") || rating == QLatin1String("good") || rating == QLatin1String("retained")) mark = i18n("Correct");
        else mark = rating;
        if (mark.isEmpty()) continue;
        const auto at = QDateTime::fromString(event.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODate);
        const auto until = QDateTime::fromString(event.value(action == QLatin1String("skip") ? QStringLiteral("skip_until") : QStringLiteral("scheduled_due_at")).toString(), Qt::ISODate);
        QString text = mark + QStringLiteral(" · ") + (at.isValid() ? at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : i18n("Unavailable"));
        if (until.isValid() && at.isValid() && until > at) {
            text += QStringLiteral(" · ") + intervalLabel(ReviewInterval::between(at, until)) + QStringLiteral(" · ") + i18n("Until %1", until.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        } else if (rating == QLatin1String("retained")) text += QStringLiteral(" · ") + i18n("Never");
        QString html = text.toHtmlEscaped();
        // The answer's group notes, one "<Group>: <note>" line each, then its own "Note: <note>".
        if (event.value(QStringLiteral("retracted_event_id")).toString().isEmpty()) {
            QHash<QString, QString> notes = ReviewTags::groupNotesOfEvent(event);
            // An answer given in two steps (review-phases spec) says what was picked before the
            // answer was shown and after it, each with its note, and what the agent predicted.
            if (const auto phases = ReviewPhases::ofEvent(event); phases.present) {
                const auto step = [&](const QString &label, const QStringList &tags, const QString &note) {
                    const QStringList shown = ReviewTags::applyMerges(tags, card.tagMerges);
                    QString line = label + QStringLiteral(": ") + (shown.isEmpty() ? i18n("Not rated") : reviewTagLabels(shown));
                    if (!note.isEmpty()) line += QStringLiteral(" — ") + note;
                    return line;
                };
                QStringList lines{step(i18n("Before"), phases.beforeTags, notes.take(ReviewTags::beforeNoteKey())),
                                  step(i18n("After"), phases.afterTags, notes.take(ReviewTags::answerNoteKey()))};
                if (!phases.agentPrediction.isEmpty())
                    lines.append(i18n("Agent predicted: %1",
                                      reviewTagLabel(ReviewTags::resolve(phases.agentPrediction.value(QStringLiteral("tag")).toString(), card.tagMerges))));
                for (const QString &line : std::as_const(lines))
                    html += QStringLiteral("<br>") + line.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            }
            for (const QString &line : ReviewTags::groupNoteLines(notes, groups, i18n("Other"), i18n("Note"), i18n("Before note")))
                html += QStringLiteral("<br>") + line.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        }
        const QString eventId = event.value(QStringLiteral("event_id")).toString();
        entries.append({at, html, version, card.page(), {}, {}, false, ReviewStore::answerInEffect(card, eventId) ? eventId : QString()});
    }
    return entries;
}

// Answer feedback (likes and dislikes of answer passages), deleted ones included: they are kept
// for the study. @p notFound: feedback_ids whose passage the current answer no longer has.
// Records made on another card than @p cardId (shared feedback found in its answer) say so.
QList<HistoryEntry> feedbackHistoryEntries(const QList<QJsonObject> &records, const QSet<QString> &notFound, const QString &cardId = {},
                                           const QHash<QString, QString> &homeQuestions = {})
{
    QList<HistoryEntry> entries;
    for (const QJsonObject &record : records) {
        const QDateTime created = historyTimestamp(record.value(QStringLiteral("created_at")).toString());
        const QDateTime updated = historyTimestamp(record.value(QStringLiteral("updated_at")).toString());
        const bool deleted = record.value(QStringLiteral("deleted")).toBool();
        QString heading = record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative() ? i18n("Disliked") : i18n("Liked");
        heading += QStringLiteral(" · ") + (created.isValid() ? historyTime(created) : i18n("Unavailable"));
        if (deleted) {
            const QDateTime at = historyTimestamp(record.value(QStringLiteral("deleted_at")).toString());
            heading += QStringLiteral(" · ") + i18n("deleted %1", at.isValid() ? historyTime(at) : i18n("Unavailable"));
        } else if (created.isValid() && updated.isValid() && created.secsTo(updated) >= 60) {
            heading += QStringLiteral(" · ") + i18n("edited %1", historyTime(updated));
        }
        QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString().simplified();
        if (quote.size() > 160) quote = quote.left(159).trimmed() + QStringLiteral("…");
        QString html = heading.toHtmlEscaped() + QStringLiteral("<br>");
        if (!cardId.isEmpty() && ReviewCardFeedback::isShared(record, cardId)) {
            const QString question = homeQuestions.value(record.value(QStringLiteral("card_id")).toString()).simplified();
            html += QStringLiteral("<i>%1</i><br>").arg((question.isEmpty() ? i18n("From another card") : i18n("From another card · “%1”", question)).toHtmlEscaped());
        }
        html += QString(QStringLiteral("“") + quote + QStringLiteral("”")).toHtmlEscaped();
        if (notFound.contains(record.value(QStringLiteral("feedback_id")).toString()))
            html += QStringLiteral(" <i>%1</i>").arg(i18n("(not in the current version)").toHtmlEscaped());
        if (const QString comment = record.value(QStringLiteral("comment")).toString().trimmed(); !comment.isEmpty())
            html += QStringLiteral("<br>") + comment.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QJsonArray regions = ReviewCardFeedback::regionsJson(ReviewCardFeedback::regionsOf(record));
        // Its passage is marked on the PDF it was given on (an edit may stamp a later one).
        QString version = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
        if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(version).hasMatch()) version = PdfVersions::recordedSha256(record);
        entries.append({created, html, version, regions.isEmpty() ? 0 : regions.first().toObject().value(QStringLiteral("page")).toInt(),
                        regions.toVariantList(), record});
    }
    return entries;
}

// @p feedback (in time order) placed among @p review (in the card's order) by time: each before
// the first review entry made after it. Review entries without a time stay after the one before.
QList<HistoryEntry> mergedHistory(const QList<HistoryEntry> &review, QList<HistoryEntry> feedback)
{
    std::stable_sort(feedback.begin(), feedback.end(), [](const HistoryEntry &a, const HistoryEntry &b) {
        if (a.at.isValid() != b.at.isValid()) return a.at.isValid();
        return a.at.isValid() && a.at < b.at;
    });
    QList<HistoryEntry> result;
    qsizetype next = 0;
    QDateTime previous;
    for (const HistoryEntry &entry : review) {
        const QDateTime at = entry.at.isValid() ? entry.at : previous;
        while (at.isValid() && next < feedback.size() && feedback.at(next).at.isValid() && feedback.at(next).at < at) result.append(feedback.at(next++));
        result.append(entry);
        previous = at;
    }
    while (next < feedback.size()) result.append(feedback.at(next++));
    return result;
}

// The History of @p card. With @p versions, entries that name the PDF version they were made on
// link to it (thoughts-version:<index into versions>); @p currentSha256 is the card's PDF now.
// @p answers: for each item of the list, in order, the answer it can take back (or nothing).
QString cardHistoryHtml(const ReviewCard &card, const QList<ReviewTags::TagGroup> &groups = {}, const QList<QJsonObject> &feedback = {},
                        const QSet<QString> &notFound = {}, const QString &currentSha256 = {}, QList<HistoryEntry> *versions = nullptr,
                        const QHash<QString, QString> &homeQuestions = {}, QStringList *answers = nullptr)
{
    const QList<HistoryEntry> entries = mergedHistory(reviewHistoryEntries(card, groups), feedbackHistoryEntries(feedback, notFound, card.id(), homeQuestions));
    QStringList items;
    if (answers) answers->clear();
    for (const HistoryEntry &entry : entries) {
        if (answers) answers->append(entry.answerEventId);
        QString html = entry.html;
        if (versions && !entry.versionSha256.isEmpty()) {
            const bool current = entry.versionSha256 == currentSha256;
            // Placed after the entry's first line.
            const qsizetype lineEnd = html.indexOf(QStringLiteral("<br>"));
            const QString link = QStringLiteral(" · <a href=\"thoughts-version:%1\">%2</a>")
                                     .arg(versions->size())
                                     .arg((current ? i18n("Open current version") : i18n("Open this version")).toHtmlEscaped());
            html.insert(lineEnd < 0 ? html.size() : lineEnd, link);
            versions->append(entry);
        }
        items.append(QStringLiteral("<li>%1</li>").arg(html));
    }
    // A retired card says so first: it is not reviewed until it is restored.
    const QString retired = card.schedule.retired ? QStringLiteral("<p><b>%1</b></p>").arg(retiredLabel(card).toHtmlEscaped()) : QString();
    return retired + QStringLiteral("<p>%1</p>")
        .arg(i18n("%1 reviews · %2 lapses", card.schedule.reviewCount, card.schedule.lapseCount).toHtmlEscaped())
        + (items.isEmpty() ? QStringLiteral("<p>%1</p>").arg(i18n("No history").toHtmlEscaped()) : QStringLiteral("<ol>%1</ol>").arg(items.join(QString())));
}

// The card's History for the review's panel, one block per PDF version (ReviewFeedbackPanel):
// the version shown now (@p currentPdfPath, SHA-256 @p currentSha256) first, with the card's
// answer regions; earlier ones newest first by their first entry; last the entries whose version
// was not recorded (made before 2026-10-21). Review answers are placed by the version their event
// records, feedback by the PDF its passage is marked on. Creation and approval are left out.
// @p states: feedback_id -> state in the current answer, as the reader finds it. @p feedback may
// hold other cards' records found in this answer (shared): each says so, with the question of the
// card it was made on from @p homeQuestions (card_id -> question of the cards that still exist).
QList<ReviewFeedbackPanel::Version> cardHistoryVersions(const ReviewCard &card, const QList<ReviewTags::TagGroup> &groups, const QList<QJsonObject> &feedback,
                                                        const QVariantMap &states, const QString &currentSha256, const QString &currentPdfPath,
                                                        const QHash<QString, QString> &homeQuestions = {})
{
    QList<HistoryEntry> review = reviewHistoryEntries(card, groups);
    review.removeIf([](const HistoryEntry &entry) { return entry.cardEvent; });
    QList<ReviewFeedbackPanel::Version> versions;
    if (!currentSha256.isEmpty()) {
        ReviewFeedbackPanel::Version current;
        current.sha256 = currentSha256;
        current.current = true;
        current.pdfPath = currentPdfPath;
        // A whole-document card's answer is every page: its passages are drawn alone.
        if (!card.isWholeDocument()) current.answerRegions = ReviewCardFeedback::regionsFromVariant(card.regions());
        versions.append(current);
    }
    ReviewFeedbackPanel::Version unrecorded;
    for (const HistoryEntry &entry : mergedHistory(review, feedbackHistoryEntries(feedback, {}))) {
        ReviewFeedbackPanel::Entry item{entry.at, {}, {}};
        if (entry.feedback.isEmpty()) {
            item.html = entry.html;
        } else {
            const QJsonObject &record = entry.feedback;
            const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
            const QString id = record.value(QStringLiteral("feedback_id")).toString();
            const bool deleted = record.value(QStringLiteral("deleted")).toBool();
            item.feedback = QVariantMap{
                {QStringLiteral("feedback_id"), id},
                {QStringLiteral("polarity"), record.value(QStringLiteral("polarity")).toString()},
                {QStringLiteral("comment"), record.value(QStringLiteral("comment")).toString()},
                {QStringLiteral("quote"), anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString()},
                {QStringLiteral("state"), deleted ? QString() : states.value(id).toString()},
                {QStringLiteral("created_at"), record.value(QStringLiteral("created_at")).toString()},
                {QStringLiteral("updated_at"), record.value(QStringLiteral("updated_at")).toString()},
                {QStringLiteral("deleted"), deleted},
                {QStringLiteral("deleted_at"), record.value(QStringLiteral("deleted_at")).toString()},
                {QStringLiteral("revision"), record.value(QStringLiteral("revision")).toInteger()},
                {QStringLiteral("anchor_pdf_sha256"), anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString()},
                {QStringLiteral("anchor_regions"), entry.regions},
                {QStringLiteral("card_id"), record.value(QStringLiteral("card_id")).toString()},
                {QStringLiteral("shared"), ReviewCardFeedback::isShared(record, card.id())},
                {QStringLiteral("home_question"), homeQuestions.value(record.value(QStringLiteral("card_id")).toString())},
            };
        }
        if (entry.versionSha256.isEmpty()) {
            unrecorded.entries.append(item);
            continue;
        }
        auto version = std::find_if(versions.begin(), versions.end(), [&entry](const ReviewFeedbackPanel::Version &candidate) {
            return candidate.sha256 == entry.versionSha256;
        });
        if (version == versions.end()) {
            ReviewFeedbackPanel::Version earlier;
            earlier.sha256 = entry.versionSha256;
            versions.append(earlier);
            version = versions.end() - 1;
        }
        version->entries.append(item);
        if (entry.at.isValid() && (!version->firstAt.isValid() || entry.at < version->firstAt)) version->firstAt = entry.at;
    }
    // Earlier versions newest first, by their first entry.
    std::stable_sort(versions.begin(), versions.end(), [](const ReviewFeedbackPanel::Version &a, const ReviewFeedbackPanel::Version &b) {
        if (a.current != b.current) return a.current;
        if (a.firstAt.isValid() != b.firstAt.isValid()) return a.firstAt.isValid();
        return a.firstAt > b.firstAt;
    });
    if (!unrecorded.entries.isEmpty()) {
        for (const auto &entry : std::as_const(unrecorded.entries)) {
            if (entry.at.isValid() && (!unrecorded.firstAt.isValid() || entry.at < unrecorded.firstAt)) unrecorded.firstAt = entry.at;
        }
        versions.append(unrecorded);
    }
    return versions;
}

// The card's PDF as review events record it (ReviewStore::reviewVersion): its document's current
// PDF, else current_pdfs/<pdf_stem>.pdf. Empty when not here.
QString historyPdfPath(const QString &docsRoot, const ReviewCard &card)
{
    const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
    return FeedbackVersion::documentPdfPath(docsRoot, primary.value(QStringLiteral("pdf_document_id")).toString(primary.value(QStringLiteral("document_id")).toString()),
                                            card.pdfStem());
}

// The text of a card's answer (@p regions, 1-based page, normalized) in the PDF at @p pdfPath, as
// the reader reads it (§4.1: the characters whose box centre is inside, regions joined with a
// newline); none when the PDF cannot be read.
std::optional<QString> pdfAnswerText(const QString &pdfPath, const QList<QPair<int, QRectF>> &regions)
{
    if (regions.isEmpty() || pdfPath.isEmpty()) return {};
    const std::unique_ptr<Poppler::Document> document(Poppler::Document::load(pdfPath));
    if (!document || document->isLocked()) return {};
    QHash<int, QList<ReviewCardFeedback::Glyph>> pages;
    QList<QList<ReviewCardFeedback::Glyph>> regionGlyphs;
    for (const auto &[page, rect] : regions) {
        if (!pages.contains(page)) {
            QList<ReviewCardFeedback::Glyph> glyphs;
            const std::unique_ptr<Poppler::Page> pdfPage(page >= 1 && page <= document->numPages() ? document->page(page - 1) : nullptr);
            const QSizeF size = pdfPage ? pdfPage->pageSizeF() : QSizeF();
            if (pdfPage && size.width() > 0 && size.height() > 0) {
                // Characters in reading order, as the reader's text layer has them.
                for (const auto &box : pdfPage->textList()) {
                    const QString text = box->text();
                    for (int i = 0; i < text.size(); ++i) {
                        const QRectF r = box->charBoundingBox(i);
                        glyphs.append({page, QString(text.at(i)), QRectF(r.x() / size.width(), r.y() / size.height(), r.width() / size.width(), r.height() / size.height())});
                    }
                    if (box->hasSpaceAfter()) glyphs.append({page, QStringLiteral(" "), {}});
                }
            }
            pages.insert(page, glyphs);
        }
        QList<ReviewCardFeedback::Glyph> inside;
        for (const auto &glyph : pages.value(page)) {
            if (!glyph.box.isEmpty() ? rect.contains(glyph.box.center()) : !inside.isEmpty()) inside.append(glyph);
        }
        regionGlyphs.append(inside);
    }
    return ReviewCardFeedback::answerText(regionGlyphs).text;
}

// The feedback_ids of @p records whose passage is no longer in the card's answer in the PDF at
// @p pdfPath (SHA-256 @p pdfSha256), found as the reader finds it (ReviewFeedbackOverlay): feedback
// given on this PDF is there; other feedback is looked for in the text of the answer's regions.
// Empty when the PDF cannot be read.
QSet<QString> feedbackNotInAnswer(const QString &pdfPath, const QString &pdfSha256, const QVariantList &answerRegions, const QList<QJsonObject> &records)
{
    QList<QJsonObject> elsewhere;
    for (const QJsonObject &record : records) {
        const QString given = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
        if (pdfSha256.isEmpty() || given != pdfSha256) elsewhere.append(record);
    }
    if (elsewhere.isEmpty()) return {};
    const std::optional<QString> answer = pdfAnswerText(pdfPath, ReviewCardFeedback::regionsFromVariant(answerRegions));
    if (!answer) return {};
    QSet<QString> result;
    for (const QJsonObject &record : std::as_const(elsewhere)) {
        if (ReviewCardFeedback::resolve(record, *answer, pdfSha256).state == QLatin1String("orphaned"))
            result.insert(record.value(QStringLiteral("feedback_id")).toString());
    }
    return result;
}

// The answer feedback of other cards of @p card's document that lies in its answer in the PDF at
// @p pdfPath (SHA-256 @p pdfSha256), found as the reader finds it (shared feedback,
// ReviewCardFeedback::sharedStates). @p records: every card's records; @p homeCards: the cards
// that exist (ReviewStore::feedbackHomeCards).
QList<QJsonObject> sharedFeedbackInAnswer(const QString &docsRoot, const ReviewCard &card, const QString &pdfPath, const QString &pdfSha256,
                                          const QList<QJsonObject> &records, const QHash<QString, QJsonObject> &homeCards)
{
    if (homeCards.isEmpty()) return {};
    const QString documentId = DocumentIdentityStore(docsRoot).byFilename(card.pdfStem() + QStringLiteral(".pdf")).value(QStringLiteral("document_id")).toString();
    const auto regions = ReviewCardFeedback::regionsFromVariant(card.regions());
    const QHash<QString, QString> states = ReviewCardFeedback::sharedStates(card.id(), ReviewCardFeedback::documentOf(card.record, {documentId}), homeCards, pdfSha256,
                                                                            regions, records, [&] { return pdfAnswerText(pdfPath, regions).value_or(QString()); });
    QList<QJsonObject> result;
    for (const QJsonObject &record : records) {
        if (states.contains(record.value(QStringLiteral("feedback_id")).toString())) result.append(record);
    }
    return result;
}

// card_id -> question of @p cards (card_id -> card record).
QHash<QString, QString> questionsOf(const QHash<QString, QJsonObject> &cards)
{
    QHash<QString, QString> result;
    for (auto it = cards.cbegin(); it != cards.cend(); ++it)
        result.insert(it.key(), it.value().value(QStringLiteral("primary")).toObject().value(QStringLiteral("question")).toString().trimmed());
    return result;
}

// A card's document as the reader names it, e.g. its Library display name; empty when unknown.
QString reviewCardDocumentName(const QString &docsRoot, const ReviewCard &card)
{
    if (card.pdfStem().isEmpty()) return {};
    const QString pdfPath = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(card.pdfStem()));
    const QJsonArray projects = LibraryProjectStore(docsRoot).projects(false);
    QFile index(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    const QJsonArray publications = index.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray() : QJsonArray{};
    return readerDisplayTitle(projects, publications, docsRoot, QUrl::fromLocalFile(pdfPath), {}, {}, card.pdfStem());
}

// The agent recall prediction service (review-phases spec, section 4): recall-predictor.json in
// the application data folder, {"url": …, "token": …}. Without it, or with either missing, no
// prediction is asked for. The token is only sent, never logged, and only over https or to
// this computer itself (recallpredictorconfig.h).
using RecallPredictor = Thoughts::RecallPredictorConfig::Service;

RecallPredictor recallPredictor()
{
    QFile file(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("recall-predictor.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) return {};
    QString refusal;
    const RecallPredictor service = Thoughts::RecallPredictorConfig::parse(file.readAll(), &refusal);
    if (!refusal.isEmpty()) qWarning().noquote() << refusal;
    return service;
}

// The card's answer for the recall prediction, as "Copy for agent" reads it: the source text
// between its markers, else the text in its answer regions of its PDF (@p pdfPath), else the text
// recorded with the card. Reads files only, so it runs off the GUI thread.
QString recallAnswerText(const QString &docsRoot, const ReviewCard &card, const QString &pdfPath)
{
    QStringList files = sourceFiles(card);
    if (!card.linkingSource().isEmpty() && !files.contains(card.linkingSource())) files.append(card.linkingSource());
    QStringList texts;
    for (const auto &passage : reviewCardSourcePassages(docsRoot, card.id(), files)) {
        for (const auto &segment : passage.segments) texts.append(segment.text);
    }
    QString answer = texts.join(QStringLiteral("\n\n")).trimmed();
    if (answer.isEmpty() && !pdfPath.isEmpty()) {
        ReviewCardAgentInput input;
        input.cardId = card.id();
        input.pdfPath = pdfPath;
        input.pdfSha256 = card.pdfSha256();
        input.regions = QJsonArray::fromVariantList(card.regions());
        input.page = card.page();
        answer = reviewCardPdfAnswerText(input).trimmed();
    }
    const QJsonObject primary = primaryObject(card);
    if (answer.isEmpty()) answer = primary.value(QStringLiteral("selected_text")).toString().trimmed();
    if (answer.isEmpty()) answer = primary.value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString().trimmed();
    if (answer.isEmpty()) answer = primary.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString().trimmed();
    return answer;
}

// "Copy for agent" on a review card: the saved template
// (Settings > Agent prompts) filled in for the card and its document.
// @p feedback may hold other cards' records found in this card's answer (shared): their lines name
// their card's question from @p homeQuestions.
QString reviewCardPromptText(const QString &docsRoot, const ReviewCard &card, QString pdfPath,
                             const QList<QJsonObject> &feedback = {}, const QSet<QString> &orphaned = {}, const QHash<QString, QString> &homeQuestions = {},
                             const QString &currentReview = {})
{
    if (pdfPath.isEmpty() && !card.pdfStem().isEmpty())
        pdfPath = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(card.pdfStem()));
    const QJsonArray projects = LibraryProjectStore(docsRoot).projects(false);
    QFile index(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    const QJsonArray publications = index.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray() : QJsonArray{};
    ReviewCardAgentInput input;
    input.cardId = card.id();
    input.question = card.question();
    input.pdfPath = pdfPath;
    input.documentName = pdfPath.isEmpty() ? QString() : readerDisplayTitle(projects, publications, docsRoot, QUrl::fromLocalFile(pdfPath));
    input.regions = QJsonArray::fromVariantList(card.regions());
    input.page = card.page();
    input.wholeDocument = card.isWholeDocument();
    input.pdfSha256 = card.pdfSha256();
    // Where the markers are now (the location index), else where the card was anchored.
    input.sourceFiles = sourceFiles(card);
    if (!input.sourceFiles.contains(card.linkingSource())) input.sourceFiles.append(card.linkingSource());
    // The liked and disliked passages; "(not found …)" where the reader could not find one.
    input.answerFeedback = ReviewCardFeedback::agentSection(feedback, orphaned, card.id(), homeQuestions);
    const QString agentTemplate = AgentPrompts::savedTemplate(AgentPrompts::Kind::Agent);
    // The answer text is read only for a template that shows it.
    const bool answerText = AgentPrompts::usesPlaceholder(agentTemplate, QStringLiteral("answer_text"));
    auto fields = reviewCardPromptFields(docsRoot, projects, input, answerText);
    if (!currentReview.isEmpty()) fields.insert(QStringLiteral("current_review"), currentReview);
    return AgentPrompts::renderForCard(fields, agentTemplate);
}

QString reviewCardAgentText(const QString &docsRoot, const ReviewCard &card, const QString &pdfPath, const QList<QJsonObject> &feedback,
                            const QHash<QString, QString> &homeQuestions = {})
{
    return reviewCardPromptText(docsRoot, card, pdfPath, feedback, {}, homeQuestions);
}

QJsonArray reviewSourceLocations(const ReviewCard &card, const QString &docsRoot)
{
    QJsonArray locations;
    const QString state = card.sourceLocation.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("linked")) {
        const auto location = card.sourceLocation;
        const QString path = location.value(QStringLiteral("source_file")).toString();
        const int first = location.value(QStringLiteral("start_line")).toInt();
        const int last = location.value(QStringLiteral("end_line")).toInt();
        if (!path.isEmpty() && first > 0 && last >= first)
            locations.append(location);
    } else if (state == QLatin1String("ambiguous")) {
        const QString project = SourceOwner::of(card.linkingSource());
        const auto files = sourceLocationIndex(docsRoot).value(QStringLiteral("files")).toObject();
        const QString key = QStringLiteral("review_card\n") + card.id();
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto file = it.value().toObject();
            if (file.value(QStringLiteral("project")).toString() != project || !file.value(QStringLiteral("valid")).toBool()) continue;
            for (const auto &value : file.value(QStringLiteral("locations")).toObject().value(key).toArray()) {
                const auto location = value.toObject();
                const int first = location.value(QStringLiteral("start_line")).toInt();
                const int last = location.value(QStringLiteral("end_line")).toInt();
                if (first > 0 && last >= first) {
                    auto candidate = location;
                    candidate.insert(QStringLiteral("source_file"), it.key());
                    locations.append(candidate);
                }
            }
        }
    }
    return locations;
}

QString reviewSourceLocationsText(const QJsonArray &candidates)
{
    QMap<QString, QStringList> locations;
    for (const auto &value : candidates) {
        const auto location = value.toObject();
        const QString path = location.value(QStringLiteral("source_file")).toString();
        const int first = location.value(QStringLiteral("start_line")).toInt();
        const int last = location.value(QStringLiteral("end_line")).toInt();
        if (!path.isEmpty() && first > 0 && last >= first)
            locations[path].append(QStringLiteral("%1–%2").arg(first).arg(last));
    }
    QStringList parts;
    for (auto it = locations.cbegin(); it != locations.cend(); ++it)
        parts.append(i18n("%1 (lines %2)", it.key(), it.value().join(i18n(" and "))));
    return parts.join(QStringLiteral("; "));
}

QString cardDetailsHtml(const ReviewBrowserCard &item, const QString &docsRoot, const QStringList &names)
{
    const ReviewCard &card = item.card;
    const QString state = card.linkingState();
    const QString locationState = card.sourceLocation.value(QStringLiteral("state")).toString();
    const QString locations = reviewSourceLocationsText(reviewSourceLocations(card, docsRoot));
    const int anchorRanges = primaryObject(card).value(QStringLiteral("source_anchor")).toObject()
        .value(QStringLiteral("segments")).toArray().size();
    // A card may mark several passages (one per answer area); all are listed in locations.
    QString linking;
    if (state == QLatin1String("marker_removal_pending")) linking = i18n("Marker removal pending");
    else if (state == QLatin1String("pdf_without_source")) linking = i18n("PDF without source location");
    else if (locationState == QLatin1String("ambiguous"))
        linking = locations.isEmpty() ? i18n("Error: Card found at multiple source locations.")
            : i18n("Error: Card found at multiple source locations: %1", locations);
    else if (state == QLatin1String("linked") && locationState == QLatin1String("linked") && !locations.isEmpty())
        linking = i18n("Linked") + QLatin1Char('\n') + locations;
    else if (state == QLatin1String("linking_failed"))
        linking = i18n("Error: %1", card.linkingReason().isEmpty() ? i18n("Linking failed") : card.linkingReason());
    else if (locationState == QLatin1String("unavailable") || (anchorRanges > 0 && locationState != QLatin1String("linked")))
        linking = i18n("Error: Source location unavailable.");
    else if (state == QLatin1String("pending")) linking = i18n("Source linking pending Sync");
    else if (state == QLatin1String("pending_server_link"))
        linking = i18n("Link pending: the server links this card to its source at the next Sync. It is not reviewed until then.");
    else if (state == QLatin1String("html_attachment"))
        linking = i18n("Made in the HTML page %1: found by its text in the page, no source markers.",
                       primaryObject(card).value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString());
    else linking = i18n("Unavailable");

    const QString reanchor = card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
    const bool currentPlacement = QStringList{QStringLiteral("succeeded"), QStringLiteral("current"), QStringLiteral("ok"),
        QStringLiteral("exact_text"), QStringLiteral("exact_point")}.contains(reanchor)
        && item.state != ReviewBrowserState::Repairing && item.state != ReviewBrowserState::NeedsAttention
        && item.state != ReviewBrowserState::Stale;
    const QString placement = item.state == ReviewBrowserState::Unpublished ? i18n("No PDF: its document is unpublished")
        : reanchor == QLatin1String("updating")
        && card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("reason")).toString() == QLatin1String("pdf_placement_pending")
        ? i18n("Linked: PDF placement pending")
        : currentPlacement ? i18n("Answer on pages %1 in the current PDF", card.pagesLabel())
        : locationState == QLatin1String("linked") ? i18n("Linked but PDF-placement failed")
        : i18n("PDF placement unavailable");
    const QString interval = card.schedule.intervalDays == 0 && !card.wasAnswered() ? i18n("New")
        : currentIntervalLabel(card);
    const QJsonObject reanchorRecord = card.record.value(QStringLiteral("reanchor")).toObject();
    const QString errorCode = reanchorRecord.value(QStringLiteral("error_code")).toString();
    QStringList unsupportedCommands;
    for (const auto &value : reanchorRecord.value(QStringLiteral("unsupported_commands")).toArray())
        if (!value.toString().isEmpty()) unsupportedCommands.append(QLatin1Char('\\') + value.toString());
    const QString storedMessage = reanchorRecord.value(QStringLiteral("message")).toString().trimmed();
    const QString placeAgain = i18n("Choose Place This Card Again to draw its answer areas on the current PDF; Thoughts marks their source text anew.");
    const QString reason = errorCode == QLatin1String("basic_content_changed")
        ? i18n("The passage changed, was removed, or is no longer unique. The original answer and review history are preserved.")
        : errorCode == QLatin1String("review_card_place_again")
        ? (storedMessage.isEmpty() ? i18n("Thoughts could not link this card's answer to its source text.") : storedMessage) + QLatin1Char(' ') + placeAgain
        : errorCode == QLatin1String("review_source_link_unresolved")
        ? (storedMessage.isEmpty() ? i18n("The card's answer could not be found in its source.") : storedMessage) + QLatin1Char(' ') + placeAgain
        : errorCode == QLatin1String("review_source_marker_missing_or_ambiguous")
        ? (storedMessage.isEmpty() ? i18n("The card's source markers are missing, incomplete, or appear more than once.") : storedMessage) + QLatin1Char(' ') + placeAgain
        : state == QLatin1String("linking_failed") && reviewCardNeedsPlacingAgain(card)
        ? placeAgain
        : errorCode == QLatin1String("review_card_latex_unsupported")
        ? (unsupportedCommands.isEmpty()
              ? i18n("This card's answer contains LaTeX that card placement does not support, so its position in the PDF cannot be found. Rewrite that part of the answer or move it outside the card; the card returns after the next build.")
              : i18n("This card's answer contains LaTeX that card placement does not support (%1), so its position in the PDF cannot be found. Rewrite those commands or move them outside the card; the card returns after the next build.",
                     unsupportedCommands.join(QStringLiteral(", "))))
        : browserUnavailableReason(item.state);
    const auto field = [](const QString &label, const QString &value) {
        return QStringLiteral("<p><b>%1</b><br>%2</p>").arg(label.toHtmlEscaped(), value.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    };
    const QString linkingHtml = QStringLiteral("<p><b>%1:</b> %2</p>")
        .arg(i18n("Linking status").toHtmlEscaped(), linking.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    return field(i18n("Question"), card.question())
        + (card.schedule.retired ? field(i18n("State"), retiredLabel(card) + QLatin1Char('\n') + i18n("Not reviewed; its history and schedule are kept. Restore to review brings it back."))
           : item.state == ReviewBrowserState::Unpublished
           ? field(i18n("State"), i18n("Unpublished") + QLatin1Char('\n')
                       + i18n("Its document is not published in its Project, so the card is not reviewed. Its history and schedule are kept; publish the document again to bring it back."))
           : QString())
        + field(i18n("Kind"), cardKindText(card)
                                  + (card.isProjectCard() ? QString(QLatin1Char('\n') + i18n("Comes back at least once a week, and at once when its passage changes.")) : QString())
                                  + (card.isProjectCard() && card.passageChanged ? QString(QLatin1Char('\n') + passageChangedText()) : QString()))
        + field(i18n("Card ID"), card.id())
        + field(i18n("Project"), names.value(0))
        + field(i18n("Document"), names.value(1))
        + linkingHtml
        + field(i18n("PDF placement"), placement)
        + (reason.isEmpty() ? QString() : QStringLiteral("<p>%1</p>").arg(reason.toHtmlEscaped()))
        + field(i18n("Schedule"), i18n("Due: %1\nInterval: %2", browserDueText(item), interval));
}

struct CardExportField {
    QString key;
    QString label;
    bool byDefault;
};

QList<CardExportField> cardExportFields()
{
    return {
        {QStringLiteral("question"), i18n("Question"), true},
        {QStringLiteral("card_id"), i18n("Card ID"), true},
        {QStringLiteral("project"), i18n("Project"), true},
        {QStringLiteral("document"), i18n("Document"), true},
        {QStringLiteral("pages"), i18n("Page"), true},
        {QStringLiteral("source"), i18n("Source"), true},
        {QStringLiteral("current_tag"), i18n("Current Tag"), true},
        {QStringLiteral("card_status"), i18n("Card Status"), false},
        {QStringLiteral("due"), i18n("Due"), true},
        {QStringLiteral("interval"), i18n("Interval"), true},
        {QStringLiteral("created"), i18n("Created"), false},
        {QStringLiteral("reviews"), i18n("Number of Repetitions"), false},
        {QStringLiteral("lapses"), i18n("Lapses"), false},
        {QStringLiteral("history"), i18n("History"), false},
    };
}

// Values match the browser's columns; @p structured keeps numbers, lists and raw history for JSON.
QJsonValue cardExportValue(const ReviewBrowserCard &item, const QString &key, const QStringList &names, bool structured,
                           const QList<ReviewTags::TagGroup> &groups = {})
{
    const ReviewCard &card = item.card;
    if (key == QLatin1String("question")) return card.question();
    if (key == QLatin1String("card_id")) return card.id();
    if (key == QLatin1String("project")) return names.value(0);
    if (key == QLatin1String("document")) return names.value(1);
    if (key == QLatin1String("pages")) return card.pagesLabel();
    if (key == QLatin1String("source")) {
        const QStringList files = sourceFiles(card);
        return structured ? QJsonValue(QJsonArray::fromStringList(files)) : QJsonValue(files.join(QStringLiteral(", ")));
    }
    if (key == QLatin1String("current_tag")) return browserCardStateLabel(item);
    if (key == QLatin1String("card_status")) return browserCardStatusLabel(item);
    if (key == QLatin1String("due")) return browserDueText(item);
    if (key == QLatin1String("interval")) return browserIntervalLabel(card);
    if (key == QLatin1String("created")) {
        const QDateTime at = recordDate(card, QStringLiteral("created_at"));
        return at.isValid() ? at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    }
    if (key == QLatin1String("reviews")) return structured ? QJsonValue(card.schedule.reviewCount) : QJsonValue(QString::number(card.schedule.reviewCount));
    if (key == QLatin1String("lapses")) return structured ? QJsonValue(card.schedule.lapseCount) : QJsonValue(QString::number(card.schedule.lapseCount));
    if (key == QLatin1String("history")) {
        if (structured) {
            QJsonArray events;
            for (const QJsonObject &event : card.history) events.append(event);
            return events;
        }
        QTextDocument history;
        history.setHtml(cardHistoryHtml(card, groups));
        return history.toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts).join(QStringLiteral("; "));
    }
    return {};
}

QString csvCell(QString text)
{
    if (!text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r')))
        return text;
    return QLatin1Char('"') + text.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
}

class BrowseCardListAccessible : public QAccessibleWidget
{
public:
    explicit BrowseCardListAccessible(QTreeWidget *tree)
        : QAccessibleWidget(tree, QAccessible::List)
        , m_tree(tree)
    {
    }

    int childCount() const override
    {
        return 0;
    }

    QAccessibleInterface *child(int) const override
    {
        return nullptr;
    }

    int indexOfChild(const QAccessibleInterface *) const override
    {
        return -1;
    }

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name) {
            return i18n("Review cards");
        }
        if (type == QAccessible::Description) {
            return i18n("Use the arrow keys to select a card; hold Shift while clicking to select a range. The current card's details and actions are shown to the right.");
        }
        if (type == QAccessible::Value && m_tree && m_tree->currentItem()) {
            return m_tree->currentItem()->text(0);
        }
        return QAccessibleWidget::text(type);
    }

private:
    QTreeWidget *m_tree = nullptr;
};

QAccessibleInterface *reviewOverviewAccessibleFactory(const QString &, QObject *object)
{
    auto *tree = qobject_cast<QTreeWidget *>(object);
    if (tree && tree->objectName() == QLatin1String("reviewBrowseTable")) {
        return new BrowseCardListAccessible(tree);
    }
    return nullptr;
}

QColor blendedColor(const QColor &surface, const QColor &accent, qreal accentWeight)
{
    const qreal surfaceWeight = 1.0 - accentWeight;
    return QColor::fromRgbF(surface.redF() * surfaceWeight + accent.redF() * accentWeight,
                            surface.greenF() * surfaceWeight + accent.greenF() * accentWeight,
                            surface.blueF() * surfaceWeight + accent.blueF() * accentWeight,
                            1.0);
}

QPair<QFrame *, QLabel *> createReviewCountTile(QWidget *parent, const QString &label, const QString &objectName, const QColor &accent)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName + QStringLiteral("Tile"));
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setMinimumHeight(102);
    frame->setAutoFillBackground(true);
    QPalette tilePalette = frame->palette();
    const QColor surface = tilePalette.color(QPalette::Window);
    const qreal accentWeight = surface.lightnessF() < 0.5 ? 0.24 : 0.20;
    tilePalette.setColor(QPalette::Window, blendedColor(surface, accent, accentWeight));
    frame->setPalette(tilePalette);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 14, 18, 14);
    auto *value = new QLabel(QStringLiteral("0"), frame);
    value->setObjectName(objectName);
    value->setAlignment(Qt::AlignCenter);
    QFont valueFont = value->font();
    valueFont.setPointSizeF(valueFont.pointSizeF() + 8.0);
    valueFont.setWeight(QFont::DemiBold);
    value->setFont(valueFont);
    auto *caption = new QLabel(label, frame);
    caption->setAlignment(Qt::AlignCenter);
    layout->addWidget(value, 1);
    layout->addWidget(caption);
    return {frame, value};
}

void configureNormalizedCoordinate(QDoubleSpinBox *coordinate)
{
    coordinate->setRange(0.0, 1.0);
    coordinate->setDecimals(4);
    coordinate->setSingleStep(0.01);
}

// Resolves where a sourced card's markers live, asking when the card is linked in several places.
// Returns false only when that choice is cancelled; *source stays empty when no source file exists.
bool chooseReviewSourceLocation(QWidget *parent, const ReviewCard &card, const QString &docsRoot, QString *source, int *line)
{
    const auto locations = reviewSourceLocations(card, docsRoot);
    int sourceLine = 1;
    QString relative = card.linkingSource();
    if (!locations.isEmpty()) {
        int selected = 0;
        if (locations.size() > 1) {
            QStringList choices;
            for (const auto &value : locations) {
                const auto location = value.toObject();
                choices.append(i18n("%1 · lines %2–%3", location.value(QStringLiteral("source_file")).toString(),
                    location.value(QStringLiteral("start_line")).toInt(), location.value(QStringLiteral("end_line")).toInt()));
            }
            bool accepted = false;
            const QString choice = QInputDialog::getItem(parent, i18n("Edit placement"), i18n("Source range"), choices, 0, false, &accepted);
            if (!accepted) return false;
            selected = choices.indexOf(choice);
        }
        const auto location = locations.at(qMax(0, selected)).toObject();
        relative = location.value(QStringLiteral("source_file")).toString();
        sourceLine = location.value(QStringLiteral("start_line")).toInt(1);
    }
    const QString path = relative.isEmpty() ? QString() : QDir(docsRoot).filePath(relative);
    *source = !path.isEmpty() && QFileInfo(path).isFile() ? path : QString();
    *line = sourceLine;
    return true;
}

// With @p editAreas the dialog offers Edit Areas…, which closes it and sets *areasRequested.
bool editReviewCardRegion(QWidget *parent,
                              const ReviewCard &card,
                              const ReviewCardsWidget::CardPresenter &presenter,
                              QVariantList *rect,
                              QString *question,
                              const std::function<void()> &editMarkers = {},
                              bool *areasRequested = nullptr)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("reviewCardRegionDialog"));
    dialog.setWindowTitle(i18n("Edit Review Card"));
    dialog.resize(560, 390);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Edit the question and answer region. Coordinates are fractions of the PDF page."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    if (card.record.contains(QStringLiteral("duplicate_check"))) {
        auto *duplicateWarning = new QLabel(card.record.value(QStringLiteral("note")).toString(), &dialog);
        duplicateWarning->setTextFormat(Qt::PlainText);
        duplicateWarning->setWordWrap(true);
        duplicateWarning->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(duplicateWarning);
    }

    auto *form = new QFormLayout;
    auto *questionEditor = new QPlainTextEdit(card.question(), &dialog);
    questionEditor->setObjectName(QStringLiteral("reviewCardRegionQuestion"));
    questionEditor->setMaximumHeight(110);
    form->addRow(i18n("Question:"), questionEditor);

    // Only a card with source markers is placed in the source. Any other card with
    // several areas (an imported PDF, a whole document) keeps its areas here and
    // changes them with Edit Areas…, not with coordinates of its first area.
    const bool sourceRegions = !primaryObject(card).value(QStringLiteral("source_anchor")).toObject().isEmpty();
    const bool areasOnly = !sourceRegions && (areasRequested || primaryObject(card).contains(QStringLiteral("regions")));
    if (sourceRegions) description->setText(i18n("Edit the question here. The answer regions follow the marked text in the source document."));
    else if (areasOnly) description->setText(areasRequested
        ? i18n("Edit the question here. Use Edit Areas… to change the answer areas on the page.")
        : i18n("Edit the question here."));
    const QVariantList originalRect = card.rect();
    auto *left = new QDoubleSpinBox(&dialog);
    auto *top = new QDoubleSpinBox(&dialog);
    auto *right = new QDoubleSpinBox(&dialog);
    auto *bottom = new QDoubleSpinBox(&dialog);
    const QList<QDoubleSpinBox *> coordinates = {left, top, right, bottom};
    const QStringList coordinateNames = {
        QStringLiteral("reviewCardRegionLeft"),
        QStringLiteral("reviewCardRegionTop"),
        QStringLiteral("reviewCardRegionRight"),
        QStringLiteral("reviewCardRegionBottom"),
    };
    for (int index = 0; index < coordinates.size(); ++index) {
        configureNormalizedCoordinate(coordinates.at(index));
        coordinates.at(index)->setEnabled(!sourceRegions && !areasOnly);
        coordinates.at(index)->setObjectName(coordinateNames.at(index));
        coordinates.at(index)->setValue(originalRect.value(index).toDouble());
    }
    auto *horizontalRegion = new QWidget(&dialog);
    auto *horizontalLayout = new QHBoxLayout(horizontalRegion);
    horizontalLayout->setContentsMargins(0, 0, 0, 0);
    horizontalLayout->addWidget(new QLabel(i18n("Left"), horizontalRegion));
    horizontalLayout->addWidget(left);
    horizontalLayout->addWidget(new QLabel(i18n("Right"), horizontalRegion));
    horizontalLayout->addWidget(right);
    auto *verticalRegion = new QWidget(&dialog);
    auto *verticalLayout = new QHBoxLayout(verticalRegion);
    verticalLayout->setContentsMargins(0, 0, 0, 0);
    verticalLayout->addWidget(new QLabel(i18n("Top"), verticalRegion));
    verticalLayout->addWidget(top);
    verticalLayout->addWidget(new QLabel(i18n("Bottom"), verticalRegion));
    verticalLayout->addWidget(bottom);
    if (sourceRegions) {
        // The markers in the source define the regions; move them there instead of editing coordinates.
        horizontalRegion->hide();
        verticalRegion->hide();
        auto *markersButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Edit Marker Position in VS Code"), &dialog);
        markersButton->setObjectName(QStringLiteral("reviewCardRegionEditMarkers"));
        markersButton->setEnabled(bool(editMarkers));
        QObject::connect(markersButton, &QPushButton::clicked, &dialog, [&] { editMarkers(); });
        form->addRow(i18n("Answer:"), markersButton);
    } else if (areasOnly) {
        horizontalRegion->hide();
        verticalRegion->hide();
    } else {
        form->addRow(i18n("Horizontal region:"), horizontalRegion);
        form->addRow(i18n("Vertical region:"), verticalRegion);
    }
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton *approveButton = buttons->button(QDialogButtonBox::Ok);
    approveButton->setText(i18n("Save"));
    auto *previewButton = buttons->addButton(i18n("Preview Region"), QDialogButtonBox::ActionRole);
    previewButton->setEnabled(bool(presenter));
    previewButton->setVisible(!sourceRegions && !areasOnly);
    if (areasRequested) {
        *areasRequested = false;
        // The area editor links each drawn area to its own source passage.
        auto *areasButton = buttons->addButton(i18n("Edit Areas…"), QDialogButtonBox::ActionRole);
        areasButton->setObjectName(QStringLiteral("reviewCardRegionEditAreas"));
        areasButton->setToolTip(i18n("Draw the card's answer areas on the PDF; on a document with source, its markers move with them."));
        QObject::connect(areasButton, &QPushButton::clicked, &dialog, [&dialog, areasRequested] {
            *areasRequested = true;
            dialog.reject();
        });
    }
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto validate = [=] {
        const QString text = questionEditor->toPlainText().trimmed();
        const double width = right->value() - left->value();
        const double height = bottom->value() - top->value();
        approveButton->setEnabled(!text.isEmpty() && text.size() <= 500 && width > 0 && height > 0 && width * height <= 0.5);
    };
    QObject::connect(questionEditor, &QPlainTextEdit::textChanged, &dialog, validate);
    for (QDoubleSpinBox *coordinate : coordinates) {
        QObject::connect(coordinate, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, validate);
    }
    validate();
    bool previewed = false;
    QObject::connect(previewButton, &QPushButton::clicked, &dialog, [&] {
        ReviewCard preview = card;
        QJsonObject primary = primaryObject(preview);
        primary.insert(QStringLiteral("rect"), QJsonArray::fromVariantList({left->value(), top->value(), right->value(), bottom->value()}));
        preview.record.insert(QStringLiteral("primary"), primary);
        QString error;
        if (!presenter(preview, true, &error)) {
            KMessageBox::error(&dialog, error.isEmpty() ? i18n("The answer region could not be previewed.") : error, i18n("Cannot Preview Region"));
            return;
        }
        previewed = true;
    });

    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (previewed && presenter) {
        QString ignoredError;
        presenter(card, false, &ignoredError);
    }
    if (!accepted) {
        return false;
    }
    *rect = sourceRegions || areasOnly ? originalRect : QVariantList{left->value(), top->value(), right->value(), bottom->value()};
    *question = questionEditor->toPlainText().trimmed();
    return true;
}

}

QString pdfVersionError(const PdfVersions::Result &result)
{
    QString error;
    switch (result.failure) {
    case PdfVersions::Failure::None:
        break;
    case PdfVersions::Failure::NotFound:
        error = i18n("This version of the PDF is no longer kept on the server.");
        break;
    case PdfVersions::Failure::NotConnected:
        error = i18n("This version is not on this computer. Connect this computer to the server to get it.");
        break;
    case PdfVersions::Failure::Unreachable:
        error = i18n("This version is not on this computer and the server cannot be reached (%1).", result.detail);
        break;
    case PdfVersions::Failure::Mismatch:
        error = i18n("The server sent a PDF that is not this version.");
        break;
    case PdfVersions::Failure::Failed:
        error = i18n("This version could not be opened: %1", result.detail);
        break;
    }
    return error;
}

// The current version first, then earlier ones newest first, last the entries whose version was not recorded.
static bool historyOrder(const ReviewFeedbackPanel::Version &a, const ReviewFeedbackPanel::Version &b)
{
    if (a.current != b.current) return a.current;
    if (a.sha256.isEmpty() != b.sha256.isEmpty()) return b.sha256.isEmpty();
    if (a.firstAt.isValid() != b.firstAt.isValid()) return a.firstAt.isValid();
    return a.firstAt > b.firstAt;
}

QList<ReviewFeedbackPanel::Version> documentHistoryVersions(const ReviewStore &store, const QString &pdfStem, const QString &currentPdfPath)
{
    const QString currentSha256 = QFileInfo(currentPdfPath).isFile() ? FeedbackVersion::cachedFileSha256(currentPdfPath) : QString();
    QList<ReviewCard> cards = store.cards(nullptr, pdfStem);
    cards.removeIf([&pdfStem](const ReviewCard &card) { return card.pdfStem() != pdfStem || !card.isApproved() || !ReviewStore::feedbackCardExists(card); });
    // In reading order.
    std::stable_sort(cards.begin(), cards.end(), [](const ReviewCard &a, const ReviewCard &b) { return a.page() < b.page(); });
    const QList<ReviewTags::TagGroup> groups = store.tagGroups();
    const QList<QJsonObject> feedback = store.cardFeedback({}, true);
    QList<ReviewFeedbackPanel::Version> versions;
    for (const ReviewCard &card : std::as_const(cards)) {
        QList<QJsonObject> own = feedback;
        own.removeIf([&card](const QJsonObject &record) { return record.value(QStringLiteral("card_id")).toString() != card.id(); });
        for (const ReviewFeedbackPanel::Version &cardVersion : cardHistoryVersions(card, groups, own, {}, currentSha256, currentPdfPath)) {
            auto version = std::find_if(versions.begin(), versions.end(), [&cardVersion](const ReviewFeedbackPanel::Version &candidate) {
                return candidate.sha256 == cardVersion.sha256;
            });
            if (version == versions.end()) {
                ReviewFeedbackPanel::Version added = cardVersion;
                added.answerRegions.clear();
                added.entries.clear();
                added.firstAt = {};
                versions.append(added);
                version = versions.end() - 1;
            }
            if (cardVersion.firstAt.isValid() && (!version->firstAt.isValid() || cardVersion.firstAt < version->firstAt)) version->firstAt = cardVersion.firstAt;
            QList<ReviewFeedbackPanel::Entry> entries = cardVersion.entries;
            // Every card shows in the version read now, with or without entries.
            if (entries.isEmpty()) entries.append(ReviewFeedbackPanel::Entry{});
            entries.first().cardAnswer = cardVersion.answerRegions;
            for (ReviewFeedbackPanel::Entry &entry : entries) {
                entry.cardId = card.id();
                entry.cardQuestion = card.question();
                version->entries.append(entry);
            }
            // Feedback given on an earlier version stays with that version:
            // it is listed under the version it was given on, never carried to the current one.
        }
    }
    std::stable_sort(versions.begin(), versions.end(), historyOrder);
    return versions;
}

void addDocumentVersions(QList<ReviewFeedbackPanel::Version> &versions, const QList<QJsonObject> &records, const QList<QPair<QString, QDateTime>> &kept,
                         const QString &currentSha256, const QString &currentPdfPath)
{
    const auto versionOf = [&versions](const QString &sha256) {
        auto found = std::find_if(versions.begin(), versions.end(), [&sha256](const ReviewFeedbackPanel::Version &version) { return version.sha256 == sha256; });
        if (found != versions.end()) return found;
        ReviewFeedbackPanel::Version added;
        added.sha256 = sha256;
        versions.append(added);
        return versions.end() - 1;
    };
    const auto seen = [](ReviewFeedbackPanel::Version &version, const QDateTime &at) {
        if (at.isValid() && (!version.firstAt.isValid() || at < version.firstAt)) version.firstAt = at;
    };
    if (!currentSha256.isEmpty()) {
        auto current = versionOf(currentSha256);
        current->current = true;
        current->pdfPath = currentPdfPath;
    }
    for (const auto &[sha256, at] : kept) {
        if (!sha256.isEmpty()) seen(*versionOf(sha256), at);
    }
    for (const QJsonObject &record : records) {
        if (record.value(QStringLiteral("deleted")).toBool()) continue;
        const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
        // The version it was first made on, as the version rule of Annotations reads it.
        QString sha256 = anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
        if (sha256.isEmpty()) sha256 = PdfVersions::recordedSha256(record);
        auto version = versionOf(sha256);
        const QDateTime at = QDateTime::fromString(record.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        seen(*version, at);
        QString kind = DocumentFeedback::kindOf(record);
        if (kind.isEmpty()) kind = record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative() ? QStringLiteral("dislike") : QStringLiteral("like");
        // A drawing opens at its strokes and a text box at its box; their region is their whole page.
        const auto regions = DocumentFeedback::isPageMark(record) && !ReviewCardFeedback::regionsOf(record).isEmpty()
            ? QList<QPair<int, QRectF>>{{ReviewCardFeedback::regionsOf(record).constFirst().first, DocumentFeedback::pageMarkBounds(record)}}
            : ReviewCardFeedback::regionsOf(record);
        version->comments.append(QVariantMap{
            {QStringLiteral("feedback_id"), record.value(QStringLiteral("feedback_id")).toString()},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("comment"), record.value(QStringLiteral("comment")).toString()},
            {QStringLiteral("quote"), anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString()},
            {QStringLiteral("regions"), ReviewCardFeedback::regionsJson(regions).toVariantList()},
            {QStringLiteral("resolved"), DocumentFeedback::isResolved(record)},
            {QStringLiteral("html_page"), anchor.value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString()},
            {QStringLiteral("at"), at},
        });
    }
    std::stable_sort(versions.begin(), versions.end(), historyOrder);
}

QList<QPair<QString, QDateTime>> keptDocumentVersions(const QString &docsRoot, const QString &filename)
{
    QList<QPair<QString, QDateTime>> result;
    if (docsRoot.isEmpty() || filename.isEmpty()) return result;
    const QDir root(docsRoot);
    const QStringList generations = root.entryList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &generation : generations) {
        QFile index(root.filePath(generation + QStringLiteral("/.thoughts-library-index.json")));
        if (!index.open(QIODevice::ReadOnly)) continue;
        const QJsonObject library = QJsonDocument::fromJson(index.readAll()).object();
        const QDateTime madeAt = QDateTime::fromString(library.value(QStringLiteral("generated_at")).toString(), Qt::ISODateWithMs);
        for (const QJsonValue &value : library.value(QStringLiteral("pdfs")).toArray()) {
            const QJsonObject entry = value.toObject();
            const QString sha256 = entry.value(QStringLiteral("pdf_sha256")).toString();
            // Only what is still here to open.
            if (entry.value(QStringLiteral("filename")).toString() != filename || sha256.isEmpty() || !QFileInfo(root.filePath(generation + QLatin1Char('/') + filename)).isFile()) continue;
            if (std::none_of(result.cbegin(), result.cend(), [&sha256](const auto &kept) { return kept.first == sha256; })) result.append({sha256, madeAt});
        }
    }
    return result;
}

QString versionPdfStem(const ReviewStore &store, const QString &sha256)
{
    if (sha256.isEmpty()) return {};
    const QList<ReviewCard> cards = store.cards();
    QHash<QString, QString> stems;
    for (const ReviewCard &card : cards) stems.insert(card.id(), card.pdfStem());
    for (const QJsonObject &record : store.cardFeedback({}, true)) {
        if (PdfVersions::recordedSha256(record) != sha256) continue;
        if (const QString stem = stems.value(record.value(QStringLiteral("card_id")).toString()); !stem.isEmpty()) return stem;
    }
    const QList<ReviewTags::TagGroup> groups = store.tagGroups();
    for (const ReviewCard &card : cards) {
        for (const HistoryEntry &entry : reviewHistoryEntries(card, groups))
            if (entry.versionSha256 == sha256 && !card.pdfStem().isEmpty()) return card.pdfStem();
    }
    return {};
}

QJsonObject sidebarReviewSchedule(const ReviewCard &card, bool active, bool unpublished)
{
    const auto utc = [](const QDateTime &at) { return at.toUTC().toString(Qt::ISODateWithMs); };
    const ReviewSchedule &schedule = card.schedule;
    const QString state = schedule.retired ? QStringLiteral("retired")
        : schedule.retained ? QStringLiteral("never")
        : !active ? (unpublished ? QStringLiteral("unpublished") : QStringLiteral("unavailable"))
        : !schedule.reviewed ? QStringLiteral("new")
        : schedule.intervalDays == 0 && !schedule.fixedInterval ? QStringLiteral("learning")
        : QStringLiteral("review");
    QJsonObject result{{QStringLiteral("state"), state},
                       {QStringLiteral("reviews"), schedule.reviewCount},
                       {QStringLiteral("lapses"), schedule.lapseCount},
                       {QStringLiteral("answered"), card.wasAnswered()}};
    if (schedule.dueAt.isValid()) result.insert(QStringLiteral("due_at"), utc(schedule.dueAt));
    // Only a card the queue takes becomes due; the list compares this with its own clock.
    if (const QDateTime from = ReviewStore::dueFrom(card); active && from.isValid()) result.insert(QStringLiteral("due_from"), utc(from));
    if (card.skippedUntil.isValid()) result.insert(QStringLiteral("skipped_until"), utc(card.skippedUntil));
    if (schedule.lastReviewedAt.isValid()) result.insert(QStringLiteral("last_reviewed_at"), utc(schedule.lastReviewedAt));
    if (schedule.retired && schedule.retiredAt.isValid()) result.insert(QStringLiteral("retired_at"), utc(schedule.retiredAt));
    // Its kind (review-card-kinds.md §5): the Cards list marks project cards and offers the choice.
    result.insert(QStringLiteral("review_kind"), card.reviewKind);
    result.insert(QStringLiteral("review_kind_source"), card.reviewKindSource);
    if (!card.kindProjectIds.isEmpty()) result.insert(QStringLiteral("projects_kind"), card.projectsKind);
    if (card.isProjectCard() && card.passageChanged) result.insert(QStringLiteral("passage_changed"), true);
    // The interval as the Review tab's Interval column writes it; "every …" only for a length.
    const QString interval = browserIntervalLabel(card);
    if (interval != QStringLiteral("—")) result.insert(QStringLiteral("interval"), interval);
    const QString unit = !card.history.isEmpty() && card.history.constLast().value(QStringLiteral("interval_action")).toString() == QLatin1String("set")
        ? card.history.constLast().value(QStringLiteral("interval_unit")).toString() : QString();
    result.insert(QStringLiteral("repeats"), interval != QStringLiteral("—") && !schedule.retained && unit != QLatin1String("date") && unit != QLatin1String("never"));
    QJsonArray tags;
    for (const QString &tag : card.currentTags()) tags.append(reviewTagLabel(tag));
    if (!tags.isEmpty()) result.insert(QStringLiteral("tags"), tags);
    return result;
}

QJsonObject recallPredictionRequest(const ReviewCard &card, const QList<ReviewCard> &documentCards, const QString &answer, const QString &document,
                                    const ReviewTags::TagGroup &recall, const QDateTime &now)
{
    // The card as read with its document's cards is the newer copy.
    ReviewCard current = card;
    for (const ReviewCard &candidate : documentCards) {
        if (candidate.id() == card.id()) {
            current = candidate;
            break;
        }
    }
    QJsonArray history;
    const QList<QJsonObject> answers = current.answers();
    for (qsizetype index = std::max<qsizetype>(0, answers.size() - 30); index < answers.size(); ++index) {
        const QJsonObject &event = answers.at(index);
        QJsonObject item{{QStringLiteral("reviewed_at"), event.value(QStringLiteral("reviewed_at")).toString()},
                         {QStringLiteral("tags"), QJsonArray::fromStringList(current.tagsOfEvent(event))}};
        QJsonObject notes;
        const auto recorded = ReviewTags::groupNotesOfEvent(event);
        for (auto it = recorded.cbegin(); it != recorded.cend(); ++it) notes.insert(it.key(), it.value());
        if (!notes.isEmpty()) item.insert(QStringLiteral("notes"), notes);
        if (const auto phases = ReviewPhases::ofEvent(event); phases.present)
            item.insert(QStringLiteral("before_tags"), QJsonArray::fromStringList(ReviewTags::applyMerges(phases.beforeTags, current.tagMerges)));
        history.append(item);
    }
    // The newest answers of the document's other cards in the last 14 days, at most 20.
    QList<std::pair<QDateTime, QJsonObject>> related;
    for (const ReviewCard &other : documentCards) {
        if (other.id() == card.id() || other.schedule.deleted) continue;
        for (const QJsonObject &event : other.answers()) {
            const QDateTime at = historyTimestamp(event.value(QStringLiteral("reviewed_at")).toString());
            if (!at.isValid() || at < now.addDays(-14)) continue;
            related.append({at, QJsonObject{{QStringLiteral("question"), other.question()},
                                            {QStringLiteral("reviewed_at"), event.value(QStringLiteral("reviewed_at")).toString()},
                                            {QStringLiteral("tags"), QJsonArray::fromStringList(other.tagsOfEvent(event))}}});
        }
    }
    std::stable_sort(related.begin(), related.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QJsonArray newest;
    for (qsizetype index = 0; index < related.size() && index < 20; ++index) newest.append(related.at(index).second);
    return {{QStringLiteral("card_id"), card.id()},
            {QStringLiteral("question"), current.question()},
            {QStringLiteral("answer"), answer.left(3000)},
            {QStringLiteral("document"), document},
            {QStringLiteral("scale"), QJsonArray::fromStringList(recall.scale)},
            {QStringLiteral("recall_group_id"), recall.id},
            {QStringLiteral("history"), history},
            {QStringLiteral("related"), newest},
            {QStringLiteral("now"), now.toUTC().toString(Qt::ISODateWithMs)}};
}

ReviewCardsWidget::ReviewCardsWidget(const QString &docsRoot,
                                     CardPresenter cardPresenter,
                                     CardReviewer cardReviewer,
                                     QWidget *parent,
                                     ReviewStore::ProjectionProvider projectionProvider)
    : QWidget(parent)
    , m_docsRoot(docsRoot)
    , m_projectionProvider(projectionProvider)
    , m_store(docsRoot, std::move(projectionProvider))
    , m_cardPresenter(std::move(cardPresenter))
    , m_cardReviewer(std::move(cardReviewer))
{
    buildUi();
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(75);
    connect(m_reloadTimer, &QTimer::timeout, this, &ReviewCardsWidget::startReload);
    reload();
}

ReviewCardsWidget::~ReviewCardsWidget()
{
    saveBrowseQuestion(false);
    saveBrowseHeaderState();
}

void ReviewCardsWidget::buildUi()
{
    static const bool accessibleFactoryInstalled = [] {
        QAccessible::installFactory(reviewOverviewAccessibleFactory);
        return true;
    }();
    Q_UNUSED(accessibleFactoryInstalled)

    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    m_browseSearch = new QLineEdit(this);
    m_browseSearch->setObjectName(QStringLiteral("reviewBrowseSearch"));
    m_browseSearch->setClearButtonEnabled(true);
    m_browseSearch->setPlaceholderText(i18n("Search questions, documents, pages, sources, card IDs, or tag:name"));
    auto *refreshButton = new QPushButton(i18n("Refresh"), this);
    searchRow->addWidget(m_browseSearch, 1);
    searchRow->addWidget(refreshButton);
    layout->addLayout(searchRow);

    auto *filters = new QHBoxLayout;
    m_browseDocumentFilter = new QComboBox(this);
    m_browseDocumentFilter->setObjectName(QStringLiteral("reviewBrowseDocumentFilter"));
    m_browseStateFilter = new QComboBox(this);
    m_browseStateFilter->setObjectName(QStringLiteral("reviewBrowseStateFilter"));
    const QList<QPair<QString, QString>> filtersByMark = {
        {i18n("New"), QStringLiteral("new")}, {i18n("Seen"), QStringLiteral("seen")},
        {i18n("Never"), QStringLiteral("never")}, {i18n("Retired / Unpublished"), QStringLiteral("retired")},
        {i18n("Unavailable"), QStringLiteral("unavailable")}, {i18n("All cards"), QStringLiteral("all")},
    };
    for (const auto &filter : filtersByMark) m_browseStateFilter->addItem(filter.first, filter.second);
    m_browseTypeFilter = new QComboBox(this);
    m_browseTypeFilter->setObjectName(QStringLiteral("reviewBrowseTypeFilter"));
    m_browseTypeFilter->setToolTip(i18n("Whether a card is linked to its source, attached to the PDF only, or still being linked"));
    for (const QString &type : {QStringLiteral("all"), QStringLiteral("linked"), QStringLiteral("pdf"), QStringLiteral("pending"), QStringLiteral("problem")})
        m_browseTypeFilter->addItem(QString(), type);
    m_browseStateFilter->setCurrentIndex(m_browseStateFilter->findData(QStringLiteral("all")));
    // Clicking a column header sorts; sorts without a column are in the header's right-click menu.
    m_browseSortKey = int(BrowserSort::Due);
    filters->addWidget(new QLabel(i18n("Document:"), this));
    filters->addWidget(m_browseDocumentFilter, 1);
    filters->addWidget(new QLabel(i18n("State:"), this));
    filters->addWidget(m_browseStateFilter);
    filters->addWidget(new QLabel(i18n("Type:"), this));
    filters->addWidget(m_browseTypeFilter);
    layout->addLayout(filters);

    m_browseCount = new QLabel(this);
    m_browseCount->setObjectName(QStringLiteral("reviewBrowseCount"));
    // List and card actions share one menu on the count row.
    m_browseActionsButton = new QToolButton(this);
    m_browseActionsButton->setObjectName(QStringLiteral("reviewBrowseActionsButton"));
    m_browseActionsButton->setText(i18n("Actions"));
    m_browseActionsButton->setPopupMode(QToolButton::InstantPopup);
    auto *actionsMenu = new QMenu(m_browseActionsButton);
    m_browseExportAction = actionsMenu->addAction(i18n("Export cards…"));
    m_browseExportAction->setObjectName(QStringLiteral("reviewBrowseExportAction"));
    m_browseExportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    m_browseExportAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_browseExportAction);
    connect(m_browseExportAction, &QAction::triggered, this, [this] { exportBrowseCards(); });
    auto *compactTagsAction = actionsMenu->addAction(i18n("Compact tags…"));
    compactTagsAction->setObjectName(QStringLiteral("reviewBrowseCompactTagsAction"));
    connect(compactTagsAction, &QAction::triggered, this, &ReviewCardsWidget::compactTags);
    actionsMenu->addSeparator();
    // Knowledge card or project card (review-card-kinds.md §5); the text and kind follow the card.
    m_browseKindAction = actionsMenu->addAction(i18n("Make project card"));
    m_browseKindAction->setObjectName(QStringLiteral("reviewBrowseKindAction"));
    connect(m_browseKindAction, &QAction::triggered, this, [this] { setBrowseCardKind(m_browseKindAction->data().toString()); });
    m_browseFollowAction = actionsMenu->addAction(i18n("Follow the Project (Knowledge)"));
    m_browseFollowAction->setObjectName(QStringLiteral("reviewBrowseFollowProjectAction"));
    m_browseFollowAction->setVisible(false);
    connect(m_browseFollowAction, &QAction::triggered, this, [this] { setBrowseCardKind(ReviewKinds::followProject()); });
    // Retire keeps the card and its history out of review; on a retired card it restores it.
    m_browseRetireAction = actionsMenu->addAction(i18n("Retire card"));
    m_browseRetireAction->setObjectName(QStringLiteral("reviewBrowseRetireAction"));
    connect(m_browseRetireAction, &QAction::triggered, this, &ReviewCardsWidget::retireOrRestoreBrowseCard);
    m_browseDeleteAction = actionsMenu->addAction(i18n("Delete card"));
    m_browseDeleteAction->setObjectName(QStringLiteral("reviewBrowseDeleteAction"));
    connect(m_browseDeleteAction, &QAction::triggered, this, &ReviewCardsWidget::deleteOrRestoreBrowseCard);
    // Two or more selected cards of one document become one; refused with a reason otherwise.
    m_browseMergeAction = actionsMenu->addAction(mergeReviewCardsActionText());
    m_browseMergeAction->setObjectName(QStringLiteral("reviewBrowseMergeAction"));
    m_browseMergeAction->setEnabled(false);
    connect(m_browseMergeAction, &QAction::triggered, this, &ReviewCardsWidget::mergeBrowseCards);
    m_browseActionsButton->setMenu(actionsMenu);
    auto *countRow = new QHBoxLayout;
    countRow->addWidget(m_browseCount, 1);
    countRow->addWidget(m_browseActionsButton);
    layout->addLayout(countRow);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_browseTable = new RowBrowserTree<QTreeWidget>(splitter);
    m_browseTable->setObjectName(QStringLiteral("reviewBrowseTable"));
    m_browseTable->setColumnCount(10);
    m_browseTable->setHeaderLabels({i18n("Question"), i18n("Due"), i18n("Project"), i18n("Document"), i18n("Page"), i18n("Source"), i18n("Current Tag"), i18n("Card Status"), i18n("Created"), i18n("Interval")});
    m_browseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_browseTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_browseTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_browseTable->setAlternatingRowColors(true);
    m_browseTable->setWordWrap(false);
    m_browseTable->setRootIsDecorated(false);
    m_browseTable->setUniformRowHeights(true);
    m_browseTable->setColumnWidth(8, 160);
    configureBrowseHeader();
    // Right-clicking a column header chooses the columns shown.
    QHeaderView *columnsHeader = m_browseTable->header();
    auto *columnsMenu = new QMenu(columnsHeader);
    columnsMenu->setObjectName(QStringLiteral("reviewBrowseColumnsMenu"));
    QList<QAction *> columnActions;
    for (int visual = 0; visual < m_browseTable->columnCount(); ++visual) {
        const int column = m_browseTable->header()->logicalIndex(visual);
        auto *action = columnsMenu->addAction(m_browseTable->headerItem()->text(column));
        columnActions.append(action);
        action->setCheckable(true);
        action->setChecked(!m_browseTable->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            m_browseTable->setColumnHidden(column, !visible);
            saveBrowseHeaderState();
        });
    }
    // The last visible column stays, or no header would be left to bring the others back.
    // Sorts that have no column of their own.
    columnsMenu->addSection(i18n("Also sort by"));
    QList<QAction *> sortActions;
    for (const auto &[label, sort] : {std::pair{i18n("Updated"), BrowserSort::Updated}, std::pair{i18n("Number of Repetitions"), BrowserSort::Reviews},
                                      std::pair{i18n("Lapses"), BrowserSort::Lapses}}) {
        auto *action = columnsMenu->addAction(label);
        action->setObjectName(QStringLiteral("reviewBrowseExtraSort"));
        action->setCheckable(true);
        action->setData(int(sort));
        sortActions.append(action);
        connect(action, &QAction::triggered, this, [this, sort] {
            // Most first; choosing it again flips the order, as a column header does.
            m_browseSortOrder = m_browseSortKey == int(sort) ? (m_browseSortOrder == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder)
                                                              : defaultBrowserSortOrder(sort);
            m_browseSortKey = int(sort);
            sortBrowseTable();
        });
    }
    connect(columnsMenu, &QMenu::aboutToShow, this, [this, columnActions, sortActions] {
        const auto visible = std::count_if(columnActions.cbegin(), columnActions.cend(), [](const QAction *action) { return action->isChecked(); });
        for (QAction *action : columnActions) action->setEnabled(!action->isChecked() || visible > 1);
        for (QAction *action : sortActions) action->setChecked(action->data().toInt() == m_browseSortKey);
    });
    columnsHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columnsHeader, &QWidget::customContextMenuRequested, this, [columnsHeader, columnsMenu](const QPoint &position) {
        columnsMenu->exec(columnsHeader->viewport()->mapToGlobal(position));
    });

    auto *detailsWidget = new QWidget(splitter);
    detailsWidget->setMinimumWidth(360);
    auto *detailsLayout = new QVBoxLayout(detailsWidget);
    auto *detailsTop = new QHBoxLayout;
    m_browseState = new QLabel(detailsWidget);
    QFont stateFont = m_browseState->font();
    stateFont.setBold(true);
    m_browseState->setFont(stateFont);
    detailsTop->addWidget(m_browseState);
    detailsTop->addStretch(1);
    detailsLayout->addLayout(detailsTop);
    m_browseDetails = new QTextBrowser(detailsWidget);
    m_browseDetails->setObjectName(QStringLiteral("reviewBrowseDetails"));
    m_browseDetails->setOpenExternalLinks(false);
    detailsLayout->addWidget(m_browseDetails, 1);
    auto *actions = new QHBoxLayout;
    m_browseReviewButton = new QPushButton(i18n("Review Card"), detailsWidget);
    m_browseReviewButton->setObjectName(QStringLiteral("reviewBrowseReviewButton"));
    m_browseEditButton = new QPushButton(i18n("Edit question"), detailsWidget);
    m_browseEditButton->setObjectName(QStringLiteral("reviewBrowseEditButton"));
    m_browseHistoryButton = new QPushButton(i18n("Show history"), detailsWidget);
    m_browseHistoryButton->setObjectName(QStringLiteral("reviewBrowseHistoryButton"));
    actions->addWidget(m_browseReviewButton);
    actions->addWidget(m_browseEditButton);
    actions->addWidget(m_browseHistoryButton);
    detailsLayout->addLayout(actions);
    m_browsePlaceAgainButton = new QPushButton(i18n("Place This Card Again…"), detailsWidget);
    m_browsePlaceAgainButton->setObjectName(QStringLiteral("reviewBrowsePlaceAgainButton"));
    m_browsePlaceAgainButton->setToolTip(i18n("Draw the card's answer areas on the current PDF; Thoughts marks their source text anew."));
    m_browsePlaceAgainButton->hide();
    detailsLayout->addWidget(m_browsePlaceAgainButton);

    m_browseEditorPanel = new QWidget(detailsWidget);
    m_browseEditorPanel->setObjectName(QStringLiteral("reviewBrowseEditorPanel"));
    auto *editorLayout = new QVBoxLayout(m_browseEditorPanel);
    editorLayout->addWidget(new QLabel(i18n("Edit question"), m_browseEditorPanel));
    m_browseQuestion = new QPlainTextEdit(m_browseEditorPanel);
    m_browseQuestion->setObjectName(QStringLiteral("reviewBrowseQuestion"));
    m_browseQuestion->setAccessibleName(i18n("Question"));
    m_browseQuestion->setPlaceholderText(i18n("Select a card to edit its question"));
    m_browseQuestion->setTabChangesFocus(true);
    m_browseQuestion->setMinimumHeight(72);
    m_browseQuestion->setMaximumHeight(110);
    QFont questionFont = m_browseQuestion->font();
    questionFont.setPointSizeF(questionFont.pointSizeF() + 2.0);
    m_browseQuestion->setFont(questionFont);
    editorLayout->addWidget(m_browseQuestion);
    m_browseQuestionStatus = new QLabel(m_browseEditorPanel);
    m_browseQuestionStatus->setObjectName(QStringLiteral("reviewBrowseQuestionStatus"));
    editorLayout->addWidget(m_browseQuestionStatus);
    auto *placementRow = new QHBoxLayout;
    m_browseSourceButton = new QPushButton(i18n("Edit placement"), m_browseEditorPanel);
    m_browseSourceButton->setObjectName(QStringLiteral("reviewBrowseSourceButton"));
    placementRow->addWidget(m_browseSourceButton);
    m_browseAreasButton = new QPushButton(i18n("Edit Areas…"), m_browseEditorPanel);
    m_browseAreasButton->setObjectName(QStringLiteral("reviewBrowseAreasButton"));
    m_browseAreasButton->setToolTip(i18n("Draw the card's answer areas on the current PDF; on a document with source, its markers move with them."));
    placementRow->addWidget(m_browseAreasButton);
    editorLayout->addLayout(placementRow);
    detailsLayout->addWidget(m_browseEditorPanel);
    m_browseEditorPanel->hide();

    m_browseHistoryPanel = new QWidget(detailsWidget);
    m_browseHistoryPanel->setObjectName(QStringLiteral("reviewBrowseHistoryPanel"));
    auto *historyLayout = new QVBoxLayout(m_browseHistoryPanel);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    auto *historyHeader = new QHBoxLayout;
    auto *historyTitle = new QLabel(i18n("History"), m_browseHistoryPanel);
    historyTitle->setObjectName(QStringLiteral("reviewBrowseHistoryTitle"));
    QFont historyFont = historyTitle->font();
    historyFont.setBold(true);
    historyTitle->setFont(historyFont);
    historyHeader->addWidget(historyTitle);
    historyHeader->addStretch(1);
    m_browseUpdateHistoryButton = new QPushButton(i18n("Update history"), m_browseHistoryPanel);
    m_browseUpdateHistoryButton->setObjectName(QStringLiteral("reviewBrowseUpdateHistoryButton"));
    historyHeader->addWidget(m_browseUpdateHistoryButton);
    historyLayout->addLayout(historyHeader);
    m_browseHistory = new QTextBrowser(m_browseHistoryPanel);
    m_browseHistory->setObjectName(QStringLiteral("reviewBrowseHistory"));
    m_browseHistory->setMaximumHeight(240);
    // Its links open PDF versions (openHistoryVersion), not URLs.
    m_browseHistory->setOpenLinks(false);
    m_browseHistory->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_browseHistory, &QWidget::customContextMenuRequested, this, &ReviewCardsWidget::showHistoryMenu);
    historyLayout->addWidget(m_browseHistory);
    m_browseHistoryStatus = new QLabel(m_browseHistoryPanel);
    m_browseHistoryStatus->setObjectName(QStringLiteral("reviewBrowseHistoryStatus"));
    m_browseHistoryStatus->setWordWrap(true);
    m_browseHistoryStatus->hide();
    historyLayout->addWidget(m_browseHistoryStatus);
    detailsLayout->addWidget(m_browseHistoryPanel);
    m_browseHistoryPanel->hide();
    m_browseDeleteRestoreButton = new QPushButton(i18n("Delete"), detailsWidget);
    m_browseDeleteRestoreButton->setObjectName(QStringLiteral("reviewBrowseDeleteRestoreButton"));
    detailsLayout->addWidget(m_browseDeleteRestoreButton);
    m_browseDeleteRestoreButton->hide();
    m_browseMergeButton = new QPushButton(mergeReviewCardsActionText(), detailsWidget);
    m_browseMergeButton->setObjectName(QStringLiteral("reviewBrowseMergeButton"));
    detailsLayout->addWidget(m_browseMergeButton);
    m_browseMergeButton->hide();
    connect(m_browseSourceButton, &QPushButton::clicked, this, &ReviewCardsWidget::repairBrowseSourceLink);
    connect(m_browseAreasButton, &QPushButton::clicked, this, [this] { placeBrowseCard(false); });
    connect(m_browsePlaceAgainButton, &QPushButton::clicked, this, [this] { placeBrowseCard(true); });
    m_browseQuestionSaveTimer = new QTimer(this);
    m_browseQuestionSaveTimer->setSingleShot(true);
    m_browseQuestionSaveTimer->setInterval(600);
    splitter->addWidget(m_browseTable);
    splitter->addWidget(detailsWidget);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    connect(m_browseSearch, &QLineEdit::textChanged, this, &ReviewCardsWidget::refreshBrowseTable);
    connect(m_browseDocumentFilter, &QComboBox::currentIndexChanged, this, &ReviewCardsWidget::refreshBrowseTable);
    connect(m_browseStateFilter, &QComboBox::currentIndexChanged, this, &ReviewCardsWidget::refreshBrowseTable);
    connect(m_browseTypeFilter, &QComboBox::currentIndexChanged, this, &ReviewCardsWidget::refreshBrowseTable);
    m_browseTable->header()->setSectionsClickable(true);
    connect(m_browseTable->header(), &QHeaderView::sectionClicked, this, [this](int column) {
        const BrowserSort sort = browserColumnSorts.at(column);
        m_browseSortOrder = m_browseSortKey == int(sort) && m_browseSortOrder == Qt::AscendingOrder
            ? Qt::DescendingOrder : Qt::AscendingOrder;
        m_browseSortKey = int(sort);
        sortBrowseTable();
    });
    connect(refreshButton, &QPushButton::clicked, this, &ReviewCardsWidget::reload);
    connect(m_browseTable, &QTreeWidget::itemSelectionChanged, this, &ReviewCardsWidget::browseSelectionChanged);
    connect(m_browseTable, &QTreeWidget::itemDoubleClicked, this, &ReviewCardsWidget::openBrowseCard);
    m_browseTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_browseTable, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const int selected = m_browseTable->selectedItems().size();
        if (selected == 0) return;
        QMenu menu(m_browseTable);
        populateBrowseContextMenu(&menu);
        menu.exec(m_browseTable->viewport()->mapToGlobal(position));
    });
    connect(m_browseQuestion, &QPlainTextEdit::textChanged, this, &ReviewCardsWidget::browseQuestionChanged);
    connect(m_browseQuestionSaveTimer, &QTimer::timeout, this, [this] { saveBrowseQuestion(); });
    connect(m_browseReviewButton, &QPushButton::clicked, this, &ReviewCardsWidget::reviewBrowseCard);
    connect(m_browseEditButton, &QPushButton::clicked, this, [this] {
        m_browseEditorPanel->setVisible(!m_browseEditorPanel->isVisible());
        if (m_browseEditorPanel->isVisible()) m_browseQuestion->setFocus();
    });
    connect(m_browseHistoryButton, &QPushButton::clicked, this, [this] {
        const bool show = !m_browseHistoryPanel->isVisible();
        m_browseHistoryPanel->setVisible(show);
        m_browseHistoryButton->setText(show ? i18n("Hide history") : i18n("Show history"));
        // Now also says which feedback the current answer no longer has.
        if (show) refreshBrowseHistory(selectedBrowseCard());
    });
    connect(m_browseHistory, &QTextBrowser::anchorClicked, this, [this](const QUrl &link) {
        bool valid = false;
        const int index = link.scheme() == QLatin1String("thoughts-version") ? link.path().toInt(&valid) : -1;
        if (valid) openHistoryVersion(index);
    });
    connect(m_browseUpdateHistoryButton, &QPushButton::clicked, this, &ReviewCardsWidget::updateBrowseHistory);
    connect(m_browseDeleteRestoreButton, &QPushButton::clicked, this, &ReviewCardsWidget::deleteOrRestoreBrowseCard);
    connect(m_browseMergeButton, &QPushButton::clicked, this, &ReviewCardsWidget::mergeBrowseCards);
}

void ReviewCardsWidget::configureBrowseHeader()
{
    QHeaderView *header = m_browseTable->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    const QByteArray savedState = KConfigGroup(KSharedConfig::openConfig(), reviewCardOverviewConfigGroup())
                                      .readEntry(reviewBrowseHeaderStateKey(), QByteArray());
    if (!savedState.isEmpty()) {
        header->restoreState(savedState);
    }
    // Keep stable logical column IDs so existing widths and visibility still apply.
    header->moveSection(header->visualIndex(8), 0);
    // A saved state can carry a resize mode from an earlier layout (for example
    // the Stretch mode used before the Question column became user-resizable).
    // Re-apply Interactive after restoring so every boundary stays draggable.
    header->setSectionResizeMode(QHeaderView::Interactive);
    if (savedState.isEmpty()) {
        const QList<int> defaultWidths = {320, 85, 160, 190, 55, 190, 100, 170, 160, 90};
        for (int column = 0; column < defaultWidths.size() && column < m_browseTable->columnCount(); ++column) {
            m_browseTable->setColumnWidth(column, defaultWidths.at(column));
        }
    }
}

void ReviewCardsWidget::saveBrowseHeaderState()
{
    if (!m_browseTable) {
        return;
    }
    KConfigGroup group(KSharedConfig::openConfig(), reviewCardOverviewConfigGroup());
    group.writeEntry(reviewBrowseHeaderStateKey(), m_browseTable->header()->saveState());
    group.sync();
}

int ReviewCardsWidget::issueCount() const
{
    return std::count_if(m_browseCards.cbegin(), m_browseCards.cend(), [](const auto &card) { return card.hasIssues; });
}

int ReviewCardsWidget::activeCardCount() const
{
    return m_activeCardCount;
}

int ReviewCardsWidget::dueCardCount() const
{
    return m_dueCardCount;
}

void ReviewCardsWidget::setStateFilter(const QString &stateFilter)
{
    m_browseSearch->clear();
    m_browseDocumentFilter->setCurrentIndex(0);
    m_browseTypeFilter->setCurrentIndex(0);
    // Free-form tags replaced the fixed good/Skip/wrong marks and their filters, so those open All cards.
    const QString normalized = stateFilter == QLatin1String("issues") ? QStringLiteral("unavailable")
        : stateFilter == QLatin1String("retained") ? QStringLiteral("never") : stateFilter;
    if (normalized.startsWith(QLatin1String("tag:"))) {
        m_browseStateFilter->setCurrentIndex(m_browseStateFilter->findData(QStringLiteral("all")));
        m_browseSearch->setText(browseTagTokenText(reviewTagLabel(ReviewTags::resolve(normalized.mid(4), m_tagMerges))));
        return;
    }
    const int index = m_browseStateFilter->findData(normalized);
    if (index < 0) m_browseStateFilter->setCurrentIndex(m_browseStateFilter->findData(QStringLiteral("all")));
    if (index >= 0) {
        m_browseStateFilter->setCurrentIndex(index);
    }
}

QStringList ReviewCardsWidget::browseSearchTags() const
{
    QStringList tags;
    for (auto it = browseTagToken().globalMatch(m_browseSearch->text()); it.hasNext();) {
        const QString value = browseTagTokenValue(it.next());
        if (!value.isEmpty()) tags.append(value);
    }
    return tags;
}

void ReviewCardsWidget::selectCard(const QString &cardId)
{
    saveBrowseQuestion();
    {
        const QSignalBlocker searchBlocker(m_browseSearch);
        const QSignalBlocker documentBlocker(m_browseDocumentFilter);
        const QSignalBlocker stateBlocker(m_browseStateFilter);
        m_browseSearch->clear();
        m_browseDocumentFilter->setCurrentIndex(0);
        m_browseStateFilter->setCurrentIndex(m_browseStateFilter->findData(QStringLiteral("all")));
    }
    m_browseTable->clearSelection();
    m_browseTable->setProperty("preferredCardId", cardId);
    refreshBrowseTable();
    if (auto *item = m_browseTable->currentItem()) {
        m_browseTable->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

ReviewStore::ProjectionProvider ReviewCardsWidget::snapshotProvider() const
{
    // Providers may access GUI-owned state; invoke them only on the GUI thread.
    if (!m_projectionProvider) return {};
    const auto snapshot = m_projectionProvider();
    return [snapshot] { return snapshot; };
}

void ReviewCardsWidget::retryPendingMarkerRemovals()
{
    // Each check rebuilds every card; once a minute is enough for a rare repair.
    if (m_syncInProgress || m_markerRetryRunning || (m_markerRetryElapsed.isValid() && m_markerRetryElapsed.elapsed() < 60000)) return;
    m_markerRetryRunning = true;
    m_markerRetryElapsed.start();
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider] {
        const int removed = ReviewStore(root, provider).retryPendingMarkerRemovals();
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, removed] {
            if (!guard) return;
            guard->m_markerRetryRunning = false;
            if (removed > 0) guard->reload();
        }, Qt::QueuedConnection);
    });
}

void ReviewCardsWidget::ensureLoaded()
{
    if (!m_loadedAt.isValid() || m_loadedAt.secsTo(QDateTime::currentDateTimeUtc()) >= 60) {
        if (!m_loading && !m_reloadTimer->isActive()) reload();
    }
}

void ReviewCardsWidget::setSyncInProgress(bool running)
{
    if (m_syncInProgress == running) return;
    m_syncInProgress = running;
    if (m_browseUpdateHistoryButton) {
        const auto *item = selectedBrowseCard();
        m_browseUpdateHistoryButton->setEnabled(!running && item && item->card.isApproved() && !item->card.schedule.deleted);
    }
    // A merge writes the source and queues its cards: never while a Sync runs.
    if (m_browseMergeAction && running) m_browseMergeAction->setEnabled(false);
    if (m_browseMergeButton && running) m_browseMergeButton->setEnabled(false);
    ++m_revision; // Discard reads that began before the synchronization boundary.
    m_reloadPending = true;
    if (running) m_reloadTimer->stop();
    else m_reloadTimer->start();
}

void ReviewCardsWidget::reload()
{
    ++m_revision;
    m_reloadPending = true;
    m_reloadTimer->start();
}

void ReviewCardsWidget::startReload()
{
    if (m_syncInProgress || m_loading || m_mutating || !m_reloadPending) return;
    if (!saveBrowseQuestion()) return;
    m_reloadPending = false;
    m_loading = true;
    const auto revision = m_revision;
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    if (!m_loadedAt.isValid()) m_browseCount->setText(i18n("Loading cards…"));
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, revision] {
        ReviewStore store(root, provider, {}, true);
        QStringList errors;
        auto cards = store.browserCards(QDateTime::currentDateTimeUtc(), &errors);
        const auto tagMerges = store.tagMerges();
        const auto tagGroups = store.tagGroups();
        const auto projects = LibraryProjectStore(root).projects(false);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, revision, cards = std::move(cards), errors, projects, tagMerges, tagGroups]() mutable {
            if (!guard) return;
            guard->m_loading = false;
            if (revision == guard->m_revision && !guard->m_mutating && !guard->m_syncInProgress) {
                // Saving here invalidates a snapshot taken before an edit made during loading.
                if (guard->saveBrowseQuestion() && revision == guard->m_revision) {
                    guard->m_browseProjects = projects;
                    guard->m_tagMerges = tagMerges;
                    guard->m_tagGroups = tagGroups;
                    guard->applyCards(std::move(cards), errors);
                }
            }
            if (guard->m_reloadPending) guard->m_reloadTimer->start();
        }, Qt::QueuedConnection);
    });
}

void ReviewCardsWidget::applyCards(QList<ReviewBrowserCard> cards, const QStringList &errors)
{
    const QString selectedDocument = m_browseDocumentFilter->currentData().toString();
    m_browseCards = std::move(cards);
    m_loadedAt = QDateTime::currentDateTimeUtc();
    m_libraryCountsAvailable = errors.isEmpty();
    m_libraryCardIds.clear();
    for (const auto &item : std::as_const(m_browseCards)) {
        const auto &card = item.card;
        // The Library's review-card columns count active cards: not deleted, rejected or retired.
        if (card.schedule.deleted || card.schedule.retired || card.record.value(QStringLiteral("deleted")).toBool()
            || card.status() == QLatin1String("rejected")) continue;
        const QString documentId = card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("document_id")).toString();
        if (!documentId.isEmpty() && !card.id().isEmpty()) m_libraryCardIds[documentId].insert(card.id());
    }
    m_activeCardCount = int(std::count_if(m_browseCards.cbegin(), m_browseCards.cend(), [](const ReviewBrowserCard &item) {
        return browserStateIsActive(item.state);
    }));
    // Match the review queue: the Today view and this count must agree.
    const QDateTime loadedAt = m_loadedAt;
    const auto due = [&loadedAt](const ReviewBrowserCard &item) {
        return browserStateIsDue(item.state) && !item.hasIssues && item.card.isApproved() && ReviewStore::isDueNow(item.card, loadedAt);
    };
    m_dueCardCount = int(std::count_if(m_browseCards.cbegin(), m_browseCards.cend(), due));
    // The Library's Due cards column counts the same cards.
    m_libraryDueCardIds.clear();
    for (const auto &item : std::as_const(m_browseCards))
        if (!item.card.id().isEmpty() && due(item)) m_libraryDueCardIds.insert(item.card.id());

    QSet<QString> documents;
    for (const ReviewBrowserCard &item : std::as_const(m_browseCards)) {
        if (!item.card.pdfStem().isEmpty()) {
            documents.insert(item.card.pdfStem());
        }
    }
    QStringList orderedDocuments = documents.values();
    orderedDocuments.sort(Qt::CaseInsensitive);
    m_browseDocumentFilter->blockSignals(true);
    m_browseDocumentFilter->clear();
    m_browseDocumentFilter->addItem(i18n("All documents"), QString());
    for (const QString &document : std::as_const(orderedDocuments)) {
        m_browseDocumentFilter->addItem(document, document);
    }
    const int selectedDocumentIndex = m_browseDocumentFilter->findData(selectedDocument);
    if (selectedDocumentIndex >= 0) {
        m_browseDocumentFilter->setCurrentIndex(selectedDocumentIndex);
    }
    m_browseDocumentFilter->blockSignals(false);
    m_browseCount->setToolTip(errors.join(QLatin1Char('\n')));
    refreshBrowseTable();
    Q_EMIT countsChanged(m_activeCardCount, m_dueCardCount);
}

void ReviewCardsWidget::populateBrowseContextMenu(QMenu *menu)
{
    const int selected = m_browseTable->selectedItems().size();
    QAction *copyForAgent = menu->addAction(i18n("Copy for agent"));
    copyForAgent->setObjectName(QStringLiteral("reviewBrowseCopyForAgentAction"));
    copyForAgent->setEnabled(selected == 1 && selectedBrowseCard());
    connect(copyForAgent, &QAction::triggered, this, [this] {
        if (const auto *item = selectedBrowseCard()) {
            const QString pdfPath = reviewCardPdfPath(m_docsRoot, item->card.record);
            // With the other cards' feedback of its document found in its answer (shared).
            QList<QJsonObject> feedback = m_store.cardFeedback(item->card.id());
            const QString historyPath = historyPdfPath(m_docsRoot, item->card);
            const QHash<QString, QJsonObject> homeCards = m_store.feedbackHomeCards(item->card.id());
            feedback += sharedFeedbackInAnswer(m_docsRoot, item->card, historyPath, historyPath.isEmpty() ? QString() : FeedbackVersion::cachedFileSha256(historyPath),
                                               m_store.cardFeedback(), homeCards);
            QGuiApplication::clipboard()->setText(reviewCardAgentText(m_docsRoot, item->card, pdfPath, feedback, questionsOf(homeCards)));
        }
    });
    connect(menu->addAction(i18np("Export selected card…", "Export %1 selected cards…", selected)), &QAction::triggered,
            this, [this] { exportBrowseCards(true); });
    QAction *merge = menu->addAction(mergeReviewCardsActionText());
    merge->setObjectName(QStringLiteral("reviewBrowseContextMergeAction"));
    merge->setEnabled(m_browseMergeAction && m_browseMergeAction->isEnabled());
    connect(merge, &QAction::triggered, this, &ReviewCardsWidget::mergeBrowseCards);
    // The card's kind (review-card-kinds.md §5).
    if (const auto *item = selectedBrowseCard(); item && selected == 1 && !item->card.schedule.deleted) {
        menu->addSeparator();
        for (const auto &[label, kind] : cardKindChoices(item->card)) {
            QAction *choice = menu->addAction(label);
            choice->setObjectName(kind == ReviewKinds::followProject() ? QStringLiteral("reviewBrowseContextFollowProjectAction")
                                                                       : QStringLiteral("reviewBrowseContextKindAction"));
            choice->setEnabled(!m_mutating);
            connect(choice, &QAction::triggered, this, [this, kind] { setBrowseCardKind(kind); });
        }
    }
}

QStringList ReviewCardsWidget::browseNames(const ReviewCard &card) const
{
    const QString filename = card.pdfStem() + QStringLiteral(".pdf");
    QStringList projects;
    for (const auto &value : m_browseProjects) {
        const auto project = value.toObject();
        bool member = project.value(QStringLiteral("target")).toString() == filename;
        for (const auto &docValue : project.value(QStringLiteral("documents")).toArray()) {
            const auto doc = docValue.toObject();
            if (doc.value(QStringLiteral("filename")).toString() != filename) continue;
            member = true;
        }
        if (member) {
            const auto name = project.value(QStringLiteral("name")).toString(project.value(QStringLiteral("id")).toString());
            if (!name.isEmpty()) projects.append(name);
        }
    }
    projects.removeDuplicates();
    return {projects.isEmpty() ? i18n("Project unavailable") : projects.join(QStringLiteral(", ")), filename};
}

void ReviewCardsWidget::sortVisibleBrowseCards()
{
    const BrowserSort sort = BrowserSort(m_browseSortKey);
    QHash<QString, QStringList> namesById;
    QHash<QString, QDateTime> datesById;
    if (sort == BrowserSort::Project || sort == BrowserSort::Deck) {
        for (const auto &item : std::as_const(m_visibleBrowseCards)) namesById.insert(item.card.id(), browseNames(item.card));
    } else if (sort == BrowserSort::Created || sort == BrowserSort::Updated) {
        const auto key = sort == BrowserSort::Created ? QStringLiteral("created_at") : QStringLiteral("updated_at");
        for (const auto &item : std::as_const(m_visibleBrowseCards)) datesById.insert(item.card.id(), recordDate(item.card, key));
    }
    std::sort(m_visibleBrowseCards.begin(), m_visibleBrowseCards.end(), [this, sort, &namesById, &datesById](const ReviewBrowserCard &left, const ReviewBrowserCard &right) {
        int comparison = 0;
        const auto compare = [](const auto &a, const auto &b) { return a < b ? -1 : (b < a ? 1 : 0); };
        switch (sort) {
        case BrowserSort::Due: {
            const auto due = [](const ReviewBrowserCard &item) {
                if (!item.card.schedule.reviewed && !item.card.schedule.dueAt.isValid()) return QDateTime(QDate(1, 1, 1), QTime(0, 0), QTimeZone::UTC);
                if (item.card.schedule.retained || item.card.schedule.retired) return QDateTime();
                return item.card.schedule.dueAt;
            };
            const auto a = due(left), b = due(right);
            if (a.isValid() != b.isValid()) return a.isValid();
            comparison = compare(a, b);
            break;
        }
        case BrowserSort::Created:
        case BrowserSort::Updated: {
            const auto a = datesById.value(left.card.id()), b = datesById.value(right.card.id());
            if (a.isValid() != b.isValid()) return a.isValid();
            comparison = compare(a, b);
            break;
        }
        case BrowserSort::Question:
            comparison = QString::compare(left.card.question(), right.card.question(), Qt::CaseInsensitive);
            break;
        case BrowserSort::Project:
        case BrowserSort::Deck:
            comparison = QString::compare(namesById.value(left.card.id()).at(sort == BrowserSort::Project ? 0 : 1),
                                          namesById.value(right.card.id()).at(sort == BrowserSort::Project ? 0 : 1), Qt::CaseInsensitive);
            break;
        case BrowserSort::Source:
            comparison = QString::compare(sourceFiles(left.card).join(QStringLiteral(", ")), sourceFiles(right.card).join(QStringLiteral(", ")), Qt::CaseInsensitive);
            break;
        case BrowserSort::State:
            comparison = QString::compare(browserCardStateLabel(left), browserCardStateLabel(right), Qt::CaseInsensitive);
            break;
        case BrowserSort::CardStatus:
            comparison = QString::compare(browserCardStatusLabel(left), browserCardStatusLabel(right), Qt::CaseInsensitive);
            break;
        case BrowserSort::Page: comparison = compare(left.card.page(), right.card.page()); break;
        case BrowserSort::Interval: comparison = compare(browserIntervalSortKey(left.card), browserIntervalSortKey(right.card)); break;
        case BrowserSort::Reviews: comparison = compare(left.card.schedule.reviewCount, right.card.schedule.reviewCount); break;
        case BrowserSort::Lapses: comparison = compare(left.card.schedule.lapseCount, right.card.schedule.lapseCount); break;
        }
        return comparison == 0 ? left.card.id() < right.card.id()
            : m_browseSortOrder == Qt::AscendingOrder ? comparison < 0 : comparison > 0;
    });
}

void ReviewCardsWidget::sortBrowseTable()
{
    // Sorting changes only row order, so retain the selection and editor state.
    const auto *topItem = m_browseTable->itemAt(QPoint(1, 1));
    const int scrollValue = m_browseTable->verticalScrollBar()->value();
    sortVisibleBrowseCards();
    QHash<QString, int> ranks;
    for (int row = 0; row < m_visibleBrowseCards.size(); ++row) {
        ranks.insert(m_visibleBrowseCards.at(row).card.id(), row);
    }
    const QSignalBlocker blocker(m_browseTable);
    m_browseTable->setUpdatesEnabled(false);
    for (int row = 0; row < m_browseTable->topLevelItemCount(); ++row) {
        auto *item = m_browseTable->topLevelItem(row);
        item->setData(0, Qt::UserRole + 1, ranks.value(item->data(0, Qt::UserRole).toString(), int(m_browseCards.size())));
    }
    m_browseTable->sortItems(0, Qt::AscendingOrder);
    const int column = browserColumnSorts.indexOf(BrowserSort(m_browseSortKey));
    m_browseTable->header()->setSortIndicatorShown(column >= 0);
    if (column >= 0) m_browseTable->header()->setSortIndicator(column, m_browseSortOrder);
    if (topItem) {
        m_browseTable->scrollToItem(topItem, QAbstractItemView::PositionAtTop);
    } else {
        m_browseTable->verticalScrollBar()->setValue(scrollValue);
    }
    m_browseTable->setUpdatesEnabled(true);
}

void ReviewCardsWidget::refreshBrowseTypeCounts(const QString &selectedDocument)
{
    QHash<QString, int> types;
    for (const ReviewBrowserCard &item : std::as_const(m_browseCards)) {
        if (!selectedDocument.isEmpty() && item.card.pdfStem() != selectedDocument) continue;
        ++types[browserCardType(item)];
    }
    const QHash<QString, QString> typeLabels{{QStringLiteral("all"), i18n("All types")}, {QStringLiteral("linked"), i18n("Linked to source")},
        {QStringLiteral("pdf"), i18n("PDF only")}, {QStringLiteral("pending"), i18n("Linking pending")}, {QStringLiteral("problem"), i18n("Link problem")}};
    for (int index = 0; index < m_browseTypeFilter->count(); ++index) {
        const QString type = m_browseTypeFilter->itemData(index).toString();
        const QString label = type == QLatin1String("all") ? typeLabels.value(type)
            : i18n("%1 (%2)", typeLabels.value(type), types.value(type));
        if (m_browseTypeFilter->itemText(index) != label) m_browseTypeFilter->setItemText(index, label);
    }
}

void ReviewCardsWidget::refreshBrowseTable()
{
    saveBrowseQuestion();
    QString selectedCardId = m_browseTable->property("preferredCardId").toString();
    if (std::any_of(m_browseCards.cbegin(), m_browseCards.cend(), [&selectedCardId](const auto &item) { return item.card.id() == selectedCardId; })) {
        m_browseTable->setProperty("preferredCardId", QVariant());
    }
    if (selectedCardId.isEmpty()) {
        const ReviewBrowserCard *selected = selectedBrowseCard();
        selectedCardId = selected ? selected->card.id() : QString();
    }
    const QString selectedDocument = m_browseDocumentFilter->currentData().toString();
    refreshBrowseTypeCounts(selectedDocument);
    const QString selectedState = m_browseStateFilter->currentData().toString();
    const QString selectedType = m_browseTypeFilter->currentData().toString();
    const QStringList searchedTags = browseSearchTags();
    QString query = m_browseSearch->text();
    query.remove(browseTagToken());
    query = query.simplified();
    QSet<QString> selectedIds;
    for (auto *item : m_browseTable->selectedItems()) selectedIds.insert(item->data(0, Qt::UserRole).toString());
    const auto *topItem = m_browseTable->itemAt(QPoint(1, 1));
    const QString topId = topItem ? topItem->data(0, Qt::UserRole).toString() : QString();
    const int scrollValue = m_browseTable->verticalScrollBar()->value();
    m_visibleBrowseCards.clear();
    int cardsInDocumentScope = 0;
    for (const ReviewBrowserCard &item : std::as_const(m_browseCards)) {
        if (!selectedDocument.isEmpty() && item.card.pdfStem() != selectedDocument) {
            continue;
        }
        ++cardsInDocumentScope;
        const bool stateMatches = item.matchesFilter(selectedState);
        if (!stateMatches) {
            continue;
        }
        if (selectedType != QLatin1String("all") && browserCardType(item) != selectedType) continue;
        // Several tags show the cards carrying any of them.
        if (!searchedTags.isEmpty()) {
            const QStringList tags = item.card.currentTags();
            const bool tagged = std::any_of(searchedTags.cbegin(), searchedTags.cend(), [this, &tags](const QString &token) {
                return std::any_of(tags.cbegin(), tags.cend(), [this, &token](const QString &tag) { return browseTagTokenMatches(token, tag, m_tagMerges); });
            });
            if (!tagged) continue;
        }
        const QString searchable = QStringList {
            item.card.question(),
            item.card.pdfStem(),
            browseNames(item.card).join(QLatin1Char(' ')),
            item.card.pagesLabel(),
            item.card.id(),
            browserCardStateLabel(item),
            browserCardStatusLabel(item),
            sourceFiles(item.card).join(QLatin1Char(' ')),
            item.card.isProjectCard() ? i18n("Project card") : QString(),
            item.card.isProjectCard() && item.card.passageChanged ? passageChangedText() : QString(),
        }.join(QLatin1Char('\n'));
        if (!query.isEmpty() && !searchable.contains(query, Qt::CaseInsensitive)) {
            continue;
        }
        m_visibleBrowseCards.append(item);
    }

    sortVisibleBrowseCards();

    const QSignalBlocker blocker(m_browseTable);
    m_browseTable->setUpdatesEnabled(false);
    QHash<QString, QTreeWidgetItem *> rows;
    QSet<QString> currentIds;
    for (const auto &item : std::as_const(m_browseCards)) currentIds.insert(item.card.id());
    for (int row = m_browseTable->topLevelItemCount() - 1; row >= 0; --row) {
        auto *item = m_browseTable->topLevelItem(row);
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!currentIds.contains(id)) { delete item; continue; }
        rows.insert(id, item);
        item->setData(0, Qt::UserRole + 1, int(m_browseCards.size()));
        item->setHidden(true);
        item->setSelected(false);
    }
    QTreeWidgetItem *currentItem = nullptr;
    bool hasSelection = false;
    for (int row = 0; row < m_visibleBrowseCards.size(); ++row) {
        const ReviewBrowserCard &item = m_visibleBrowseCards.at(row);
        const auto names = browseNames(item.card);
        const QDateTime createdAt = recordDate(item.card, QStringLiteral("created_at"));
        // A project card says so after its due time, with the mark of a changed passage.
        QString due = browserDueText(item);
        if (item.card.isProjectCard()) due += QStringLiteral(" · ") + i18n("Project");
        if (item.card.isProjectCard() && item.card.passageChanged) due += QStringLiteral(" · ") + passageChangedText();
        const QStringList values = {
            item.card.question(), due, names.at(0), names.at(1),
            item.card.pagesLabel(), sourceFiles(item.card).join(QStringLiteral(", ")),
            browserCardStateLabel(item), browserCardStatusLabel(item),
            createdAt.isValid() ? createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : i18n("Unavailable"),
            browserIntervalLabel(item.card),
        };
        auto *tableItem = rows.value(item.card.id());
        if (!tableItem) {
            tableItem = new ReviewBrowserItem(m_browseTable);
            rows.insert(item.card.id(), tableItem);
        }
        for (int column = 0; column < values.size(); ++column) {
            if (tableItem->text(column) != values.at(column)) tableItem->setText(column, values.at(column));
            tableItem->setData(column, Qt::UserRole, item.card.id());
            tableItem->setToolTip(column, values.at(column));
        }
        tableItem->setData(0, Qt::UserRole + 1, row);
        tableItem->setHidden(false);
        const bool selected = selectedIds.contains(item.card.id()) || item.card.id() == selectedCardId;
        tableItem->setSelected(selected);
        hasSelection |= selected;
        if (item.card.id() == selectedCardId) currentItem = tableItem;
    }
    m_browseTable->sortItems(0, Qt::AscendingOrder);
    const int sortedColumn = browserColumnSorts.indexOf(BrowserSort(m_browseSortKey));
    m_browseTable->header()->setSortIndicatorShown(sortedColumn >= 0);
    if (sortedColumn >= 0) m_browseTable->header()->setSortIndicator(sortedColumn, m_browseSortOrder);
    if (!currentItem && !m_visibleBrowseCards.isEmpty()) currentItem = m_browseTable->topLevelItem(0);
    if (currentItem) {
        m_browseTable->setCurrentItem(currentItem, 0, QItemSelectionModel::NoUpdate);
        if (!hasSelection) currentItem->setSelected(true);
    }
    m_browseCount->setText(i18n("Showing %1 of %2 cards", m_visibleBrowseCards.size(), cardsInDocumentScope));
    updateBrowseExportAction();
    showBrowseCard(selectedBrowseCard());
    if (auto *top = rows.value(topId); top && !top->isHidden()) {
        m_browseTable->scrollToItem(top, QAbstractItemView::PositionAtTop);
    } else {
        m_browseTable->verticalScrollBar()->setValue(scrollValue);
    }
    m_browseTable->setUpdatesEnabled(true);
}

const ReviewBrowserCard *ReviewCardsWidget::selectedBrowseCard() const
{
    const auto selected = m_browseTable->selectedItems();
    if (selected.size() != 1) {
        return nullptr;
    }
    const int row = m_browseTable->indexOfTopLevelItem(selected.constFirst());
    return row >= 0 && row < m_visibleBrowseCards.size() ? &m_visibleBrowseCards.at(row) : nullptr;
}

void ReviewCardsWidget::browseSelectionChanged()
{
    saveBrowseQuestion();
    updateBrowseExportAction();
    showBrowseCard(selectedBrowseCard());
}

void ReviewCardsWidget::updateBrowseExportAction()
{
    // One row is always selected for the details panel, so only a real multi-selection names the selection.
    const int selected = m_browseTable->selectedItems().size();
    m_browseExportAction->setText(selected > 1 ? i18n("Export %1 selected…", selected) : i18n("Export cards…"));
    m_browseExportAction->setEnabled(!m_browseCards.isEmpty());
}

QByteArray ReviewCardsWidget::cardExportData(const QList<ReviewBrowserCard> &cards, const QStringList &fields, const QString &format) const
{
    QHash<QString, QString> labels;
    for (const auto &field : cardExportFields()) labels.insert(field.key, field.label);
    if (format == QLatin1String("json")) {
        QJsonArray rows;
        for (const auto &item : cards) {
            const QStringList names = browseNames(item.card);
            QJsonObject row;
            for (const QString &key : fields) row.insert(key, cardExportValue(item, key, names, true, m_tagGroups));
            rows.append(row);
        }
        return QJsonDocument(rows).toJson(QJsonDocument::Indented);
    }
    if (format == QLatin1String("md")) {
        QStringList blocks{QStringLiteral("# %1\n\n%2").arg(i18n("Thoughts cards"),
            i18np("Exported %2 · 1 card", "Exported %2 · %1 cards", cards.size(),
                  QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))))};
        for (int index = 0; index < cards.size(); ++index) {
            const auto &item = cards.at(index);
            const QStringList names = browseNames(item.card);
            const QString question = item.card.question().simplified();
            QStringList lines{QStringLiteral("## ") + (fields.contains(QStringLiteral("question")) && !question.isEmpty() ? question : i18n("Card %1", index + 1))};
            for (const QString &key : fields) {
                if (key == QLatin1String("question")) continue;
                const QString value = cardExportValue(item, key, names, false, m_tagGroups).toString().trimmed();
                if (value.isEmpty()) continue;
                lines.append(QStringLiteral("- **%1:** %2").arg(labels.value(key), QString(value).replace(QLatin1Char('\n'), QStringLiteral("  \n  "))));
            }
            blocks.append(lines.join(QLatin1Char('\n')));
        }
        return (blocks.join(QStringLiteral("\n\n")) + QLatin1Char('\n')).toUtf8();
    }
    QStringList header;
    for (const QString &key : fields) header.append(csvCell(labels.value(key)));
    QStringList rows{header.join(QLatin1Char(','))};
    for (const auto &item : cards) {
        const QStringList names = browseNames(item.card);
        QStringList row;
        for (const QString &key : fields) row.append(csvCell(cardExportValue(item, key, names, false, m_tagGroups).toString()));
        rows.append(row.join(QLatin1Char(',')));
    }
    // The byte-order mark lets spreadsheet apps read the file as UTF-8.
    return QByteArrayLiteral("\xEF\xBB\xBF") + (rows.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8();
}

void ReviewCardsWidget::exportBrowseCards(bool preferSelection)
{
    saveBrowseQuestion();
    QSet<QString> selectedIds;
    for (const auto *row : m_browseTable->selectedItems()) selectedIds.insert(row->data(0, Qt::UserRole).toString());
    QList<ReviewBrowserCard> selected;
    for (const auto &item : std::as_const(m_visibleBrowseCards))
        if (selectedIds.contains(item.card.id())) selected.append(item);
    const QList<QList<ReviewBrowserCard>> scopes{selected, m_visibleBrowseCards, m_browseCards};

    KConfigGroup settings(KSharedConfig::openConfig(), QStringLiteral("ReviewCardExport"));
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("reviewExportDialog"));
    dialog.setWindowTitle(i18n("Export Cards"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *scopeBox = new QGroupBox(i18n("Cards"), &dialog);
    auto *scopeLayout = new QVBoxLayout(scopeBox);
    const QStringList scopeLabels{i18n("Selected cards (%1)", selected.size()),
        i18n("Cards shown in the list (%1)", m_visibleBrowseCards.size()), i18n("All cards (%1)", m_browseCards.size())};
    QList<QRadioButton *> scopeButtons;
    for (int index = 0; index < scopeLabels.size(); ++index) {
        auto *button = new QRadioButton(scopeLabels.at(index), scopeBox);
        button->setObjectName(QStringLiteral("reviewExportScope%1").arg(index));
        button->setEnabled(!scopes.at(index).isEmpty());
        scopeLayout->addWidget(button);
        scopeButtons.append(button);
    }
    int defaultScope = !selected.isEmpty() && (preferSelection || selected.size() > 1) ? 0 : 1;
    if (scopes.at(defaultScope).isEmpty()) defaultScope = 2;
    scopeButtons.at(defaultScope)->setChecked(true);
    layout->addWidget(scopeBox);

    auto *format = new QComboBox(&dialog);
    format->setObjectName(QStringLiteral("reviewExportFormat"));
    format->addItem(i18n("CSV (spreadsheet)"), QStringLiteral("csv"));
    format->addItem(i18n("JSON"), QStringLiteral("json"));
    format->addItem(i18n("Markdown"), QStringLiteral("md"));
    format->setCurrentIndex(qMax(0, format->findData(settings.readEntry("Format", QStringLiteral("csv")))));
    auto *formatRow = new QFormLayout;
    formatRow->addRow(i18n("Format:"), format);
    layout->addLayout(formatRow);

    auto *fieldBox = new QGroupBox(i18n("Fields"), &dialog);
    auto *fieldGrid = new QGridLayout(fieldBox);
    const auto fields = cardExportFields();
    QStringList chosen = settings.readEntry("Fields", QStringList());
    if (chosen.isEmpty())
        for (const auto &field : fields)
            if (field.byDefault) chosen.append(field.key);
    QList<QCheckBox *> fieldChecks;
    for (int index = 0; index < fields.size(); ++index) {
        auto *check = new QCheckBox(fields.at(index).label, fieldBox);
        check->setProperty("exportField", fields.at(index).key);
        check->setChecked(chosen.contains(fields.at(index).key));
        fieldGrid->addWidget(check, index / 3, index % 3);
        fieldChecks.append(check);
    }
    layout->addWidget(fieldBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton *exportButton = buttons->button(QDialogButtonBox::Ok);
    layout->addWidget(buttons);
    const auto scopeIndex = [&scopeButtons] {
        for (int index = 0; index < scopeButtons.size(); ++index)
            if (scopeButtons.at(index)->isChecked()) return index;
        return 1;
    };
    const auto chosenFields = [&fieldChecks] {
        QStringList keys;
        for (const auto *check : fieldChecks)
            if (check->isChecked()) keys.append(check->property("exportField").toString());
        return keys;
    };
    const auto update = [&] {
        const int count = scopes.at(scopeIndex()).size();
        exportButton->setText(i18np("Export 1 card…", "Export %1 cards…", count));
        exportButton->setEnabled(count > 0 && !chosenFields().isEmpty());
    };
    for (auto *button : std::as_const(scopeButtons)) connect(button, &QRadioButton::toggled, &dialog, update);
    for (auto *check : std::as_const(fieldChecks)) connect(check, &QCheckBox::toggled, &dialog, update);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    if (dialog.exec() != QDialog::Accepted) return;

    const QString formatKey = format->currentData().toString();
    const QStringList keys = chosenFields();
    const QList<ReviewBrowserCard> cards = scopes.at(scopeIndex());
    settings.writeEntry("Format", formatKey);
    settings.writeEntry("Fields", keys);
    const QString directory = settings.readEntry("Directory", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    const QString suggested = QDir(directory).filePath(QStringLiteral("thoughts-cards-%1.%2").arg(QDate::currentDate().toString(Qt::ISODate), formatKey));
    const QString filter = formatKey == QLatin1String("json") ? i18n("JSON files (*.json)")
        : formatKey == QLatin1String("md") ? i18n("Markdown files (*.md)") : i18n("CSV files (*.csv)");
    const QString path = QFileDialog::getSaveFileName(this, i18n("Export Cards"), suggested, filter);
    if (path.isEmpty()) {
        settings.sync();
        return;
    }
    settings.writeEntry("Directory", QFileInfo(path).absolutePath());
    settings.sync();
    const QByteArray data = cardExportData(cards, keys, formatKey);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        KMessageBox::error(this, i18n("Could not write %1: %2", QDir::toNativeSeparators(path), file.errorString()), i18n("Could Not Export Cards"));
    }
}

void ReviewCardsWidget::browseQuestionChanged()
{
    if (m_browseQuestionCardId.isEmpty()) {
        return;
    }
    const QString question = m_browseQuestion->toPlainText().trimmed();
    if (question == m_browseQuestionSavedText) {
        m_browseQuestionSaveTimer->stop();
        m_browseQuestionStatus->setText(i18n("Saved"));
        return;
    }
    if (question.isEmpty()) {
        m_browseQuestionSaveTimer->stop();
        m_browseQuestionStatus->setText(i18n("Question cannot be empty"));
        return;
    }
    if (question.size() > 500) {
        m_browseQuestionSaveTimer->stop();
        m_browseQuestionStatus->setText(i18n("Question is too long — maximum 500 characters"));
        return;
    }
    m_browseQuestionStatus->setText(i18n("Saving…"));
    m_browseQuestionSaveTimer->start();
}

void ReviewCardsWidget::showBrowseCard(const ReviewBrowserCard *item)
{
    const bool available = item != nullptr;
    const auto selected = m_browseTable->selectedItems();
    const bool multiple = selected.size() > 1;
    m_browseState->setVisible(multiple);
    m_browseDetails->setVisible(!multiple);
    m_browseReviewButton->setVisible(!multiple);
    m_browseEditButton->setVisible(!multiple);
    m_browseHistoryButton->setVisible(!multiple);
    m_browseDeleteAction->setEnabled(available && !multiple);
    m_browseRetireAction->setEnabled(available && !multiple && !item->card.schedule.deleted);
    m_browseRetireAction->setText(available && item->card.schedule.retired ? i18n("Restore to review") : i18n("Retire card"));
    {
        const auto choices = available ? cardKindChoices(item->card) : QList<QPair<QString, QString>>{qMakePair(i18n("Make project card"), ReviewKinds::project())};
        m_browseKindAction->setText(choices.first().first);
        m_browseKindAction->setData(choices.first().second);
        m_browseKindAction->setEnabled(available && !multiple && !item->card.schedule.deleted);
        m_browseFollowAction->setVisible(choices.size() > 1);
        if (choices.size() > 1) m_browseFollowAction->setText(choices.at(1).first);
        m_browseFollowAction->setEnabled(available && !multiple && !item->card.schedule.deleted);
    }
    m_browseDeleteRestoreButton->setVisible(multiple);
    bool mergeable = multiple && !m_syncInProgress;
    for (auto *selectedItem : selected) {
        const int row = m_browseTable->indexOfTopLevelItem(selectedItem);
        mergeable = mergeable && row >= 0 && row < m_visibleBrowseCards.size() && !m_visibleBrowseCards.at(row).card.schedule.deleted;
    }
    m_browseMergeAction->setEnabled(mergeable);
    m_browseMergeButton->setVisible(multiple);
    m_browseMergeButton->setEnabled(mergeable);
    m_browseEditorPanel->hide();
    const bool reopenHistory = item && item->card.id() == m_historyCardToReopen;
    m_browseHistoryPanel->setVisible(reopenHistory);
    if (reopenHistory) m_historyCardToReopen.clear();
    m_browseHistoryButton->setText(i18n("Show history"));
    if (reopenHistory) m_browseHistoryButton->setText(i18n("Hide history"));
    m_browseSourceButton->setVisible(available && !multiple);
    m_browseSourceButton->setEnabled(available && !m_syncInProgress);
    const bool placeable = available && !multiple && canPlaceReviewCardByAreas(item->card) && !item->card.schedule.deleted;
    m_browseAreasButton->setVisible(placeable);
    m_browseAreasButton->setEnabled(placeable && !m_syncInProgress && !reviewCardPdfPath(m_docsRoot, item->card.record).isEmpty());
    m_browsePlaceAgainButton->setVisible(placeable && reviewCardNeedsPlacingAgain(item->card));
    m_browsePlaceAgainButton->setEnabled(m_browseAreasButton->isEnabled());
    m_browseReviewButton->setEnabled(available && bool(m_cardReviewer) && item->canReview());
    m_browseEditButton->setEnabled(available);
    m_browseHistoryButton->setEnabled(available);
    m_browseUpdateHistoryButton->setEnabled(available && item->card.isApproved() && !item->card.schedule.deleted && !m_syncInProgress);
    m_browseQuestion->setEnabled(available);
    m_browseDeleteRestoreButton->setEnabled(available);
    if (!item) {
        m_browseState->setText(multiple ? i18n("%1 cards selected", selected.size()) : QString());
        m_browseQuestionSaveTimer->stop();
        m_browseQuestionCardId.clear();
        m_browseQuestionSavedText.clear();
        m_browseQuestion->blockSignals(true);
        m_browseQuestion->clear();
        m_browseQuestion->blockSignals(false);
        m_browseQuestionStatus->clear();
        m_browseDetails->clear();
        refreshBrowseHistory(nullptr);
        bool allActive = true;
        for (auto *selectedItem : selected) {
            const int row = m_browseTable->indexOfTopLevelItem(selectedItem);
            if (row < 0 || row >= m_visibleBrowseCards.size()) {
                allActive = false;
                break;
            }
            const bool deleted = m_visibleBrowseCards.at(row).card.schedule.deleted;
            allActive &= !deleted;
        }
        m_browseDeleteRestoreButton->setText(i18n("Delete"));
        m_browseDeleteRestoreButton->setEnabled(multiple && allActive);
        return;
    }

    const ReviewCard &card = item->card;
    m_browseState->clear();
    m_browseQuestionSaveTimer->stop();
    m_browseQuestionCardId = card.id();
    m_browseQuestionSavedText = card.question();
    m_browseQuestion->blockSignals(true);
    m_browseQuestion->setPlainText(card.question());
    m_browseQuestion->blockSignals(false);
    m_browseQuestionStatus->setText(i18n("Edit directly — changes save automatically"));
    m_browseDeleteRestoreButton->setText(i18n("Delete"));

    m_browseDetails->setHtml(cardDetailsHtml(*item, m_docsRoot, browseNames(card)));
    refreshBrowseHistory(item);
}

void ReviewCardsWidget::refreshBrowseHistory(const ReviewBrowserCard *item)
{
    m_browseHistoryVersions.clear();
    const QString cardId = item ? item->card.id() : QString();
    if (cardId != std::exchange(m_browseHistoryCardId, cardId)) {
        // A version still being fetched for another card is not opened.
        ++m_versionRequest;
        m_browseHistoryStatus->clear();
        m_browseHistoryStatus->hide();
    }
    if (!item) {
        m_browseHistoryAnswers.clear();
        m_browseHistory->clear();
        return;
    }
    const ReviewCard &card = item->card;
    if (m_browseHistoryPanel->isHidden()) {
        // Feedback, versions and the PDF's hash are read when the History is shown.
        m_browseHistory->setHtml(cardHistoryHtml(card, m_tagGroups, {}, {}, {}, nullptr, {}, &m_browseHistoryAnswers));
        return;
    }
    QList<QJsonObject> feedback = m_store.cardFeedback(card.id(), true);
    const QString pdfPath = historyPdfPath(m_docsRoot, card);
    const QString currentSha256 = pdfPath.isEmpty() ? QString() : FeedbackVersion::cachedFileSha256(pdfPath);
    // The other cards' feedback of its document found in its answer (shared); never "not found".
    const QHash<QString, QJsonObject> homeCards = m_store.feedbackHomeCards(card.id());
    const QList<QJsonObject> shared = sharedFeedbackInAnswer(m_docsRoot, card, pdfPath, currentSha256, m_store.cardFeedback(), homeCards);
    QSet<QString> notFound;
    if (!feedback.isEmpty()) {
        // Read once per card, PDF and feedback revision.
        QString key = card.id() + QLatin1Char('\n') + currentSha256;
        for (const QJsonObject &record : feedback)
            key += QLatin1Char('\n') + record.value(QStringLiteral("feedback_id")).toString() + QLatin1Char('@') + record.value(QStringLiteral("updated_at")).toString();
        if (const auto cached = m_feedbackNotFound.constFind(key); cached != m_feedbackNotFound.cend()) {
            notFound = *cached;
        } else {
            notFound = feedbackNotInAnswer(pdfPath, currentSha256, card.regions(), feedback);
            if (m_feedbackNotFound.size() > 256) m_feedbackNotFound.clear();
            m_feedbackNotFound.insert(key, notFound);
        }
    }
    feedback += shared;
    QList<HistoryEntry> versions;
    m_browseHistory->setHtml(cardHistoryHtml(card, m_tagGroups, feedback, notFound, currentSha256, &versions, questionsOf(homeCards), &m_browseHistoryAnswers));
    const QString document = reviewCardDocumentName(m_docsRoot, card);
    const QString documentTitle = document.isEmpty() ? card.pdfStem() : document;
    for (const HistoryEntry &entry : std::as_const(versions)) {
        const QString when = entry.at.isValid() ? historyTime(entry.at) : QString();
        m_browseHistoryVersions.append({card.id(), entry.versionSha256,
                                        when.isEmpty() ? i18n("%1 — earlier version", documentTitle) : i18n("%1 — version of %2", documentTitle, when),
                                        entry.page > 0 ? entry.page : card.page(), entry.regions, entry.versionSha256 == currentSha256});
    }
}

QString ReviewCardsWidget::historyAnswerAt(const QPoint &viewportPos) const
{
    // One list item per History entry (cardHistoryHtml), its lines in one block.
    const QTextBlock block = m_browseHistory->cursorForPosition(viewportPos).block();
    const QTextList *list = block.textList();
    if (!list) return {};
    // Only on the entry itself, not on the empty space under the list.
    const QRectF bounds = m_browseHistory->document()->documentLayout()->blockBoundingRect(block);
    const qreal y = viewportPos.y() + m_browseHistory->verticalScrollBar()->value();
    if (y < bounds.top() || y > bounds.bottom()) return {};
    const int item = list->itemNumber(block);
    return item >= 0 && item < m_browseHistoryAnswers.size() ? m_browseHistoryAnswers.at(item) : QString();
}

void ReviewCardsWidget::showHistoryMenu(const QPoint &viewportPos)
{
    QMenu *menu = m_browseHistory->createStandardContextMenu();
    menu->setObjectName(QStringLiteral("reviewHistoryMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (const QString eventId = historyAnswerAt(viewportPos); !eventId.isEmpty() && !m_mutating) {
        QAction *first = menu->actions().value(0);
        auto *takeBack = new QAction(i18n("Take back this answer…"), menu);
        takeBack->setObjectName(QStringLiteral("reviewHistoryTakeBackAction"));
        takeBack->setToolTip(i18n("Remove this answer, as Undo does: the card's schedule and tags come from its other answers again"));
        menu->insertAction(first, takeBack);
        menu->insertSeparator(first);
        connect(takeBack, &QAction::triggered, this, [this, eventId] { takeBackHistoryAnswer(eventId); });
    }
    menu->popup(m_browseHistory->viewport()->mapToGlobal(viewportPos));
}

void ReviewCardsWidget::takeBackHistoryAnswer(const QString &eventId)
{
    const ReviewBrowserCard *item = selectedBrowseCard();
    if (!item || item->card.id() != m_browseHistoryCardId || m_mutating) return;
    // The card as stored now: another device or window may have taken the answer back already.
    ReviewStore store(m_docsRoot, m_projectionProvider);
    const QString cardId = item->card.id();
    ReviewCard card;
    for (const ReviewCard &candidate : store.cards(nullptr, item->card.pdfStem()))
        if (candidate.id() == cardId) card = candidate;
    const QString title = i18n("Take Back Answer");
    if (!ReviewStore::answerInEffect(card, eventId)) {
        KMessageBox::information(this, i18n("This answer was already taken back, or its card was deleted."), title);
        reload();
        return;
    }
    // What will be removed, as the History words it: when, its tags and interval, its notes.
    QString removed;
    for (const HistoryEntry &entry : reviewHistoryEntries(card, m_tagGroups)) {
        if (entry.answerEventId != eventId) continue;
        QTextDocument text;
        text.setHtml(entry.html);
        removed = text.toPlainText().replace(QChar::LineSeparator, QLatin1Char('\n'));
    }
    const QString question = card.question().simplified();
    const QString text = i18n("Take back this answer to “%1”?\n\n%2\n\nThe card's schedule and tags then come from its other answers, as after Undo. "
                              "The History keeps the answer and lists “Undo review”; your other devices follow at the next Sync.",
                              question.size() > 120 ? question.left(119) + QStringLiteral("…") : question, removed);
    if (KMessageBox::warningContinueCancel(this, text, title, KGuiItem(i18n("Take Back"), QStringLiteral("edit-undo")), KStandardGuiItem::cancel(), {},
                                           KMessageBox::Notify | KMessageBox::Dangerous)
        != KMessageBox::Continue)
        return;
    QString error;
    if (!store.retractRating(cardId, eventId, QDateTime::currentDateTimeUtc(), &error)) {
        KMessageBox::error(this, error, i18n("Cannot Take Back the Answer"));
        return;
    }
    DeveloperLog::record(QStringLiteral("review.answer.taken_back"), {{QStringLiteral("card_id"), cardId}, {QStringLiteral("event_id"), eventId}});
    ++m_revision;
    // Shown at once; the reload below brings the card's new schedule to the list.
    for (const ReviewCard &candidate : store.cards(nullptr, card.pdfStem())) {
        if (candidate.id() != cardId) continue;
        for (auto &listed : m_browseCards)
            if (listed.card.id() == cardId) listed.card = candidate;
        for (auto &listed : m_visibleBrowseCards)
            if (listed.card.id() == cardId) listed.card = candidate;
    }
    refreshBrowseHistory(selectedBrowseCard());
    reload();
}

void ReviewCardsWidget::openHistoryVersion(int index)
{
    if (index < 0 || index >= m_browseHistoryVersions.size()) return;
    const HistoryVersion version = m_browseHistoryVersions.at(index);
    const ReviewBrowserCard *item = selectedBrowseCard();
    if (!item || item->card.id() != version.cardId) return;
    const quint64 request = ++m_versionRequest;
    const auto report = [this](const QString &text) {
        m_browseHistoryStatus->setText(text);
        m_browseHistoryStatus->setVisible(!text.isEmpty());
    };
    if (version.current) {
        report({});
        const QString pdfPath = historyPdfPath(m_docsRoot, item->card);
        if (pdfPath.isEmpty() || !QFileInfo(pdfPath).isFile()) {
            report(i18n("The PDF of this card is not on this computer."));
            return;
        }
        Q_EMIT pdfVersionOpenRequested(pdfPath, {}, version.page, version.regions, true);
        return;
    }
    if (!m_versionFetcher) m_versionFetcher = new PdfVersions::Fetcher(m_docsRoot, {}, {}, this);
    report(i18n("Getting this version of the PDF…"));
    m_versionFetcher->fetch(version.sha256, this, [this, request, version, report](const PdfVersions::Result &result) {
        if (request != m_versionRequest) return;
        switch (result.failure) {
        case PdfVersions::Failure::None:
            report({});
            Q_EMIT pdfVersionOpenRequested(result.path, version.title, version.page, version.regions, false);
            return;
        case PdfVersions::Failure::NotFound:
            report(i18n("This version of the PDF is no longer kept on the server."));
            return;
        case PdfVersions::Failure::NotConnected:
            report(i18n("This version is not on this computer. Connect this computer to the server to get it."));
            return;
        case PdfVersions::Failure::Unreachable:
            report(i18n("This version is not on this computer and the server cannot be reached (%1).", result.detail));
            return;
        case PdfVersions::Failure::Mismatch:
            report(i18n("The server sent a PDF that is not this version."));
            return;
        case PdfVersions::Failure::Failed:
            report(i18n("This version could not be opened: %1", result.detail));
            return;
        }
    });
}

void ReviewCardsWidget::openBrowseCard()
{
    const ReviewBrowserCard *item = selectedBrowseCard();
    if (!item || !m_cardPresenter) {
        return;
    }
    QString error;
    if (!m_cardPresenter(item->card, false, &error)) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Review Card"));
    }
}

void ReviewCardsWidget::repairBrowseSourceLink()
{
    const auto *item = selectedBrowseCard();
    if (!item || m_syncInProgress) return;
    const QString id = item->card.id();
    if (!saveBrowseQuestion()) return;
    // Saving the question can replace the cached row; retrieve the durable card.
    ReviewStore store(m_docsRoot, m_projectionProvider);
    const auto cards = store.cards();
    const auto found = std::find_if(cards.cbegin(), cards.cend(), [&](const auto &card) { return card.id() == id; });
    if (found == cards.cend()) return;
    const ReviewCard card = *found;
    const auto locations = reviewSourceLocations(card, m_docsRoot);
    int sourceLine = 1;
    QString relative = card.linkingSource();
    if (!locations.isEmpty()) {
        int selected = 0;
        if (locations.size() > 1) {
            QStringList choices;
            for (const auto &value : locations) {
                const auto location = value.toObject();
                choices.append(i18n("%1 · lines %2–%3", location.value(QStringLiteral("source_file")).toString(),
                    location.value(QStringLiteral("start_line")).toInt(), location.value(QStringLiteral("end_line")).toInt()));
            }
            bool accepted = false;
            const QString choice = QInputDialog::getItem(this, i18n("Edit placement"), i18n("Source range"), choices, 0, false, &accepted);
            if (!accepted) return;
            selected = choices.indexOf(choice);
        }
        const auto location = locations.at(qMax(0, selected)).toObject();
        relative = location.value(QStringLiteral("source_file")).toString();
        sourceLine = location.value(QStringLiteral("start_line")).toInt(1);
    }
    const QString source = relative.isEmpty() ? QString() : QDir(m_docsRoot).filePath(relative);
    if (!source.isEmpty() && QFileInfo(source).isFile()) Q_EMIT openSourceRequested(source, sourceLine);
    if (card.linkingState() != QLatin1String("linking_failed")) return;

    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Link review card to source"));
    dialog.setObjectName(QStringLiteral("reviewSourceLinkDialog"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *instructions = new QLabel(i18n("Place the markers on separate lines before and after the answer passage. Save the source file; Thoughts will link the existing card when it finds one valid answer passage."), &dialog);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto *formatRow = new QHBoxLayout;
    auto *formatColumn = new QVBoxLayout;
    auto *formatLabel = new QLabel(i18n("Source format"), &dialog);
    auto *format = new QComboBox(&dialog);
    format->setObjectName(QStringLiteral("reviewSourceFormat"));
    format->addItem(i18n("Markdown (.md, .markdown)"), QStringLiteral("markdown"));
    format->addItem(i18n("LaTeX (.tex)"), QStringLiteral("tex"));
    const bool sourceIsMarkdown = relative.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
        || relative.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
    format->setCurrentIndex(sourceIsMarkdown ? 0 : 1);
    formatColumn->addWidget(formatLabel);
    formatColumn->addWidget(format);
    formatRow->addLayout(formatColumn);
    formatRow->addStretch();
    auto *openDocument = new QPushButton(i18n("Open currently linked document"), &dialog);
    openDocument->setObjectName(QStringLiteral("reviewOpenLinkedDocument"));
    openDocument->setEnabled(m_cardPresenter && !reviewCardPdfPath(m_docsRoot, card.record).isEmpty());
    formatRow->addWidget(openDocument, 0, Qt::AlignBottom);
    layout->addLayout(formatRow);
    connect(openDocument, &QPushButton::clicked, &dialog, [this, &dialog, card] {
        dialog.reject();
        QString error;
        if (!m_cardPresenter(card, false, &error))
            KMessageBox::error(this, error.isEmpty() ? i18n("The PDF could not be opened.") : error, i18n("Cannot Open Review Card"));
    });

    auto *markerHeading = new QHBoxLayout;
    markerHeading->addWidget(new QLabel(i18n("Markers for the answer passage"), &dialog));
    markerHeading->addStretch();
    auto *copy = new QPushButton(i18n("Copy markers"), &dialog);
    copy->setObjectName(QStringLiteral("reviewCopySourceMarkers"));
    markerHeading->addWidget(copy);
    layout->addLayout(markerHeading);
    auto *markers = new QPlainTextEdit(&dialog);
    markers->setReadOnly(true);
    markers->setObjectName(QStringLiteral("reviewSourceMarkers"));
    markers->setMaximumHeight(90);
    layout->addWidget(markers);
    const auto markerText = [&] {
        return format->currentData().toString() == QLatin1String("markdown")
            ? QStringLiteral("<!-- thoughts:v1:start review:%1:answer:0 -->\n\n<!-- thoughts:v1:end review:%1:answer:0 -->").arg(card.id())
            : QStringLiteral("\\ReviewCardMaskStart{%1}{0}%\n\n\\ReviewCardMaskEnd{%1}{0}%").arg(card.id());
    };
    markers->setPlainText(markerText());
    connect(format, &QComboBox::currentIndexChanged, &dialog, [=] { markers->setPlainText(markerText()); });
    connect(copy, &QPushButton::clicked, &dialog, [=] { QGuiApplication::clipboard()->setText(markerText()); });

    auto *footer = new QHBoxLayout;
    auto *status = new QLabel(i18n("Waiting for saved source markers"), &dialog);
    status->setObjectName(QStringLiteral("reviewSourceLinkStatus"));
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer->addWidget(status, 1);
    auto *openMarkers = new QPushButton(&dialog);
    openMarkers->setObjectName(QStringLiteral("reviewOpenSourceMarkers"));
    openMarkers->hide();
    footer->addWidget(openMarkers);
    auto *close = new QPushButton(i18n("Close"), &dialog);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    footer->addWidget(close);
    layout->addLayout(footer);
    int markerLine = 0;
    connect(openMarkers, &QPushButton::clicked, &dialog, [&, source] {
        if (!source.isEmpty() && markerLine > 0) Q_EMIT openSourceRequested(source, markerLine);
    });

    QTimer poll(&dialog);
    poll.setInterval(700);
    QByteArray lastSource;
    bool checked = false;
    const auto inspect = [&] {
        QFile file(source);
        if (source.isEmpty() || !file.open(QIODevice::ReadOnly)) {
            status->setText(i18n("The linked source file is unavailable."));
            return;
        }
        const QByteArray bytes = file.readAll();
        // Windows cannot replace a file that is still open, and linking writes it.
        file.close();
        if (checked && bytes == lastSource) return;
        checked = true;
        lastSource = bytes;
        const bool markdown = format->currentData().toString() == QLatin1String("markdown");
        const QString start = markdown
            ? QStringLiteral("<!-- thoughts:v1:start review:%1:answer:0 -->").arg(card.id())
            : QStringLiteral("\\ReviewCardMaskStart{%1}{0}%").arg(card.id());
        markerLine = 0;
        const auto lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) if (lines.at(i).trimmed() == start) { markerLine = i + 1; break; }
        openMarkers->setVisible(markerLine > 0);
        if (markerLine > 0) openMarkers->setText(i18n("Open markers · line %1", markerLine));
        if (markdown != sourceIsMarkdown) {
            status->setText(i18n("Select the format of the linked source file to connect this card."));
            return;
        }
        if (!markerLine) {
            status->setText(i18n("Waiting for saved source markers"));
            return;
        }
        QString error;
        if (!store.checkManualSourceLink(card, source, &error)) { status->setText(error); return; }
        poll.stop();
        status->setText(i18n("Markers found · linked at line %1", markerLine));
        reload();
    };
    connect(format, &QComboBox::currentIndexChanged, &dialog, [&] { checked = false; inspect(); });
    connect(&poll, &QTimer::timeout, &dialog, inspect);
    poll.start();
    QTimer::singleShot(0, &dialog, inspect);
    dialog.resize(660, 290);
    dialog.exec();
}

void ReviewCardsWidget::mergeBrowseCards()
{
    QStringList ids;
    for (auto *selectedItem : m_browseTable->selectedItems()) {
        const int row = m_browseTable->indexOfTopLevelItem(selectedItem);
        if (row < 0 || row >= m_visibleBrowseCards.size()) return;
        ids.append(m_visibleBrowseCards.at(row).card.id());
    }
    if (ids.size() < 2 || m_mutating || m_syncInProgress || !saveBrowseQuestion()) return;
    // The durable cards, as the merge compares them: saving the question can replace the cached rows.
    ReviewStore store(m_docsRoot, m_projectionProvider);
    QList<ReviewCard> chosen;
    for (const ReviewCard &card : store.cards())
        if (ids.contains(card.id())) chosen.append(card);
    std::stable_sort(chosen.begin(), chosen.end(), [&ids](const ReviewCard &a, const ReviewCard &b) { return ids.indexOf(a.id()) < ids.indexOf(b.id()); });
    if (chosen.size() != ids.size()) {
        KMessageBox::error(this, i18n("A selected card is no longer available. Refresh and try again."), i18n("Cannot Merge Review Cards"));
        reload();
        return;
    }
    ReviewStore::CardMerge merged;
    if (!mergeReviewCards(this, store, chosen, defaultReviewSourceBundleRoot(), &merged)) return;
    ++m_revision;
    dropDeletedBrowseCards(merged.deletedIds);
    m_browseTable->setProperty("preferredCardId", merged.keptId);
    reload();
}

void ReviewCardsWidget::placeBrowseCard(bool again)
{
    const auto *item = selectedBrowseCard();
    if (!item || m_syncInProgress) return;
    const QString id = item->card.id();
    if (!saveBrowseQuestion()) return;
    // Saving the question can replace the cached row; retrieve the durable card.
    ReviewStore store(m_docsRoot, m_projectionProvider);
    const auto cards = store.cards();
    const auto found = std::find_if(cards.cbegin(), cards.cend(), [&](const auto &card) { return card.id() == id; });
    if (found == cards.cend()) return;
    const QString pdfPath = reviewCardPdfPath(m_docsRoot, found->record);
    if (pdfPath.isEmpty() || !QFileInfo::exists(pdfPath)) {
        KMessageBox::error(this, i18n("The card's document has no PDF on this device. Sync, then try again."), i18n("Cannot Edit Review Card"));
        return;
    }
    if (editReviewCardAreas(this, store, m_docsRoot, *found, pdfPath, again || reviewCardNeedsPlacingAgain(*found))) reload();
}

void ReviewCardsWidget::reviewBrowseCard()
{
    const ReviewBrowserCard *item = selectedBrowseCard();
    if (!item || !m_cardReviewer || !item->canReview()) {
        return;
    }
    m_cardReviewer(item->card.id());
}

void ReviewCardsWidget::updateBrowseHistory()
{
    if (m_syncInProgress || m_mutating || !saveBrowseQuestion()) return;
    const ReviewBrowserCard *item = selectedBrowseCard();
    if (!item || !item->card.isApproved() || item->card.schedule.deleted) return;
    const QString cardId = item->card.id();
    const QString current = currentIntervalLabel(item->card);
    ReviewInterval selected = currentIntervalChoice(item->card);
    bool never = item->card.schedule.retained;
    if (!chooseReviewInterval(this, &selected, nullptr, {}, {}, nullptr, &never, current)) return;
    const QDateTime selectedAt = QDateTime::currentDateTimeUtc();
    ++m_revision;
    m_mutating = true;
    setEnabled(false);
    m_browseCount->setText(i18n("Saving interval…"));
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, cardId, selected, never, selectedAt] {
        ReviewStore store(root, provider);
        QString error;
        const bool saved = store.setInterval(cardId, selected, never, selectedAt, &error);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cardId, saved, error] {
            if (!guard) return;
            guard->m_mutating = false;
            guard->setEnabled(true);
            if (!saved) KMessageBox::error(guard, error, i18n("Cannot Update History"));
            else {
                guard->m_historyCardToReopen = cardId;
                guard->m_browseTable->setProperty("preferredCardId", cardId);
            }
            guard->reload();
        }, Qt::QueuedConnection);
    });
}

bool ReviewCardsWidget::saveBrowseQuestion(bool reportFailure)
{
    if (!m_browseQuestionSaveTimer || !m_browseQuestion || m_browseQuestionCardId.isEmpty()) {
        return true;
    }
    m_browseQuestionSaveTimer->stop();
    const QString question = m_browseQuestion->toPlainText().trimmed();
    if (question == m_browseQuestionSavedText) {
        return true;
    }
    if (question.isEmpty() || question.size() > 500) {
        if (reportFailure) {
            m_browseQuestionStatus->setText(i18n("Question must contain between 1 and 500 characters"));
        }
        return false;
    }
    QString error;
    if (!m_store.updateQuestion(m_browseQuestionCardId, question, &error)) {
        if (reportFailure) {
            m_browseQuestionStatus->setText(i18n("Could not save: %1", error));
        }
        return false;
    }
    updateCachedBrowseQuestion(m_browseQuestionCardId, question);
    if (const auto *selected = selectedBrowseCard(); selected && selected->card.id() == m_browseQuestionCardId)
        m_browseDetails->setHtml(cardDetailsHtml(*selected, m_docsRoot, browseNames(selected->card)));
    ++m_revision;
    if (m_loading) { m_reloadPending = true; m_reloadTimer->start(); }
    m_browseQuestionSavedText = question;
    m_browseQuestionStatus->setText(i18n("Saved"));
    return true;
}

void ReviewCardsWidget::updateCachedBrowseQuestion(const QString &cardId, const QString &question)
{
    const auto update = [&cardId, &question](QList<ReviewBrowserCard> *items) {
        for (ReviewBrowserCard &item : *items) {
            if (item.card.id() != cardId) {
                continue;
            }
            QJsonObject primary = primaryObject(item.card);
            primary.insert(QStringLiteral("question"), question);
            item.card.record.insert(QStringLiteral("primary"), primary);
            break;
        }
    };
    update(&m_browseCards);
    update(&m_visibleBrowseCards);
    for (int row = 0; row < m_browseTable->topLevelItemCount(); ++row) {
        QTreeWidgetItem *tableItem = m_browseTable->topLevelItem(row);
        if (tableItem->data(0, Qt::UserRole).toString() == cardId) {
            tableItem->setText(0, question);
            tableItem->setToolTip(0, question);
            break;
        }
    }
}

void ReviewCardsWidget::deleteOrRestoreBrowseCard()
{
    QStringList cardIds;
    for (auto *selectedItem : m_browseTable->selectedItems()) {
        const int row = m_browseTable->indexOfTopLevelItem(selectedItem);
        if (row < 0 || row >= m_visibleBrowseCards.size()) {
            return;
        }
        const auto &card = m_visibleBrowseCards.at(row).card;
        if (card.schedule.deleted) {
            return;
        }
        cardIds.append(card.id());
    }
    if (cardIds.isEmpty()) {
        return;
    }
    if (KMessageBox::questionTwoActions(this,
                                           cardIds.size() == 1
                                               ? deleteCardQuestion()
                                               : i18n("Permanently delete %1 selected review cards? They and their history cannot be restored.\n\n"
                                                      "Retire card instead stops reviewing a card and keeps its history; it can be restored to review later.", cardIds.size()),
                                           i18n("Delete Review Card"),
                                           KGuiItem(i18n("Delete"), QStringLiteral("edit-delete")),
                                           KStandardGuiItem::cancel())
            != KMessageBox::PrimaryAction) {
        return;
    }
    if (m_mutating || !saveBrowseQuestion()) return;
    ++m_revision;
    m_mutating = true;
    setEnabled(false);
    m_browseCount->setText(i18n("Deleting %1 cards…", cardIds.size()));
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, cardIds] {
        ReviewStore store(root, provider);
        QString error;
        const bool saved = store.setCardsDeleted(cardIds, true, QDateTime::currentDateTimeUtc(), &error);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, saved, error, cardIds] {
            if (!guard) return;
            guard->m_mutating = false;
            guard->setEnabled(true);
            if (!saved) KMessageBox::error(guard, error, i18n("Cannot Delete Review Cards"));
            else guard->dropDeletedBrowseCards(cardIds);
            guard->reload();
        }, Qt::QueuedConnection);
    });
}

void ReviewCardsWidget::retireOrRestoreBrowseCard()
{
    const auto *item = selectedBrowseCard();
    if (!item || item->card.schedule.deleted || m_mutating || !saveBrowseQuestion()) return;
    const QString cardId = item->card.id();
    const bool restore = item->card.schedule.retired;
    ++m_revision;
    m_mutating = true;
    setEnabled(false);
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, cardId, restore] {
        ReviewStore store(root, provider);
        QString error;
        const bool saved = restore ? store.restoreRetiredCard(cardId, &error) : store.retireCard(cardId, &error);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, saved, error, restore, cardId] {
            if (!guard) return;
            guard->m_mutating = false;
            guard->setEnabled(true);
            if (!saved) {
                KMessageBox::error(guard, error, restore ? i18n("Cannot Restore Review Card") : i18n("Cannot Retire Review Card"));
            } else {
                // Saved locally: show it now; the reload below follows once any Sync is done.
                for (auto &card : guard->m_browseCards) {
                    if (card.card.id() != cardId) continue;
                    card.card.schedule.retired = !restore;
                    card.card.schedule.retiredAt = restore ? QDateTime() : QDateTime::currentDateTimeUtc();
                    if (!restore) {
                        card.state = ReviewBrowserState::Retired;
                        card.hasIssues = false;
                    }
                }
                if (!restore) guard->applyCards(guard->m_browseCards, guard->m_browseCount->toolTip().split(QLatin1Char('\n'), Qt::SkipEmptyParts));
            }
            guard->reload();
        }, Qt::QueuedConnection);
    });
}

int ReviewCardsWidget::activeProjectCardCount(const QString &projectId) const
{
    if (!m_libraryCountsAvailable) return -1;
    QList<ReviewCard> cards;
    for (const ReviewBrowserCard &item : m_browseCards) cards.append(item.card);
    return int(ReviewStore::activeProjectCards(cards, projectId).size());
}

QList<ReviewCard> ReviewCardsWidget::activeProjectCards(const QString &projectId)
{
    return ReviewStore::activeProjectCards(m_store.cards(), projectId);
}

bool ReviewCardsWidget::retireProjectCards(const QString &projectId, QString *error)
{
    if (m_mutating) {
        if (error) *error = i18n("Another change to the cards is being saved. Try again.");
        return false;
    }
    if (!saveBrowseQuestion()) return false;
    ++m_revision;
    const bool saved = m_store.retireProjectCards(projectId, error);
    reload();
    return saved;
}

void ReviewCardsWidget::setBrowseCardKind(const QString &kind)
{
    const auto *item = selectedBrowseCard();
    if (!item || item->card.schedule.deleted || m_mutating || !saveBrowseQuestion()) return;
    const QString cardId = item->card.id();
    ++m_revision;
    m_mutating = true;
    setEnabled(false);
    const auto provider = snapshotProvider();
    const auto root = m_docsRoot;
    QPointer<ReviewCardsWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, cardId, kind] {
        ReviewStore store(root, provider);
        QString error;
        const bool saved = store.setCardKind(cardId, kind, &error);
        // The card as its kind now makes it: its mark, its due time.
        const QList<ReviewCard> changed = saved ? store.cardsWithIds({cardId}) : QList<ReviewCard>{};
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, saved, error, cardId, changed] {
            if (!guard) return;
            guard->m_mutating = false;
            guard->setEnabled(true);
            if (!saved) {
                KMessageBox::error(guard, error, i18n("Cannot Change the Card's Kind"));
            } else {
                for (auto &card : guard->m_browseCards) {
                    if (card.card.id() != cardId) continue;
                    for (const ReviewCard &read : changed) {
                        if (read.id() == cardId) card.card = read;
                    }
                }
                guard->applyCards(guard->m_browseCards, guard->m_browseCount->toolTip().split(QLatin1Char('\n'), Qt::SkipEmptyParts));
            }
            guard->reload();
        }, Qt::QueuedConnection);
    });
}

void ReviewCardsWidget::compactTags()
{
    const QHash<QString, int> usage = m_store.reviewTagUsage();
    const QHash<QString, QString> merges = m_store.tagMerges();
    const QStringList tags = ReviewTags::byPopularity(usage, createdReviewTags(merges), merges);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("reviewCompactTagsDialog"));
    dialog.setWindowTitle(i18n("Compact tags"));
    dialog.resize(420, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(i18n("Choose tags to merge into one. Answers keep their tags, but every device shows them under the new name. This cannot be undone."), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QMap<QString, QStringList> existing;
    for (auto it = merges.constBegin(); it != merges.constEnd(); ++it) existing[it.value()].append(reviewTagLabel(it.key()));
    if (!existing.isEmpty()) {
        QStringList lines;
        for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
            QStringList from = it.value();
            from.sort(Qt::CaseInsensitive);
            lines.append(from.join(QStringLiteral(", ")) + QStringLiteral(" → ") + reviewTagLabel(it.key()));
        }
        auto *already = new QLabel(i18n("Already merged:\n%1", lines.join(QLatin1Char('\n'))), &dialog);
        already->setWordWrap(true);
        already->setEnabled(false);
        layout->addWidget(already);
    }
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("reviewCompactTagsList"));
    for (const QString &tag : tags) {
        auto *item = new QListWidgetItem(i18n("%1 (%2)", reviewTagLabel(tag), usage.value(tag)), list);
        item->setData(Qt::UserRole, tag);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    layout->addWidget(list, 1);
    auto *target = new QLineEdit(&dialog);
    target->setObjectName(QStringLiteral("reviewCompactTagsTarget"));
    target->setPlaceholderText(i18n("Merge into"));
    layout->addWidget(target);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Merge"));
    layout->addWidget(buttons);
    const auto chosen = [list] {
        QStringList result;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->checkState() == Qt::Checked) result.append(list->item(row)->data(Qt::UserRole).toString());
        return result;
    };
    // Suggest the most used chosen tag until a name is typed; the list is most-used first.
    bool edited = false;
    connect(target, &QLineEdit::textEdited, &dialog, [&edited] { edited = true; });
    connect(list, &QListWidget::itemChanged, &dialog, [&edited, target, chosen] {
        if (edited) return;
        const QStringList picked = chosen();
        target->setText(picked.isEmpty() ? QString() : reviewTagLabel(picked.constFirst()));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, target, chosen] {
        const QString to = normalizeReviewTag(target->text());
        QStringList from = chosen();
        from.removeAll(to);
        if (to.isEmpty() || from.isEmpty()) {
            KMessageBox::information(&dialog, i18n("Choose the tags to merge and the name to merge them into."), i18n("Compact tags"));
            return;
        }
        if (KMessageBox::warningContinueCancel(&dialog,
                i18n("%1 will be shown as %2 on every device. This cannot be undone.", reviewTagLabels(from), reviewTagLabel(to)),
                i18n("Merge tags permanently?"), KGuiItem(i18n("Merge"))) != KMessageBox::Continue) {
            return;
        }
        QString error;
        if (!m_store.recordTagMerge(from, to, &error)) {
            KMessageBox::error(&dialog, error.isEmpty() ? i18n("The tags could not be merged.") : error, i18n("Compact tags"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    reload();
    KMessageBox::information(this, i18n("Tags merged. Sync to share the merge with your other devices."), i18n("Compact tags"));
}

void ReviewCardsWidget::dropDeletedBrowseCards(const QStringList &cardIds)
{
    // The deletion is saved locally; show it now. A reload waits for any running Sync.
    const QSet<QString> deleted(cardIds.cbegin(), cardIds.cend());
    auto cards = m_browseCards;
    cards.removeIf([&deleted](const ReviewBrowserCard &item) { return deleted.contains(item.card.id()); });
    applyCards(std::move(cards), m_browseCount->toolTip().split(QLatin1Char('\n'), Qt::SkipEmptyParts));
}

ReviewOverview::ReviewOverview(const QString &docsRoot,
                               DocumentProvider documentProvider,
                               CardPresenter cardPresenter,
                               MaskClearer maskClearer,
                               QWidget *parent,
                               ReviewStore::ProjectionProvider projectionProvider,
                               CardBrowserOpener cardBrowserOpener,
                               SessionHostProvider sessionHostProvider,
                               bool syncInProgress)
    : QDialog(parent)
    , m_syncInProgress(syncInProgress)
    , m_docsRoot(docsRoot)
    , m_store(docsRoot, projectionProvider)
    , m_documentProvider(std::move(documentProvider))
    , m_cardPresenter(std::move(cardPresenter))
    , m_maskClearer(std::move(maskClearer))
    , m_cardBrowserOpener(std::move(cardBrowserOpener))
    , m_sessionHostProvider(std::move(sessionHostProvider))
    , m_conversationProjections(std::move(projectionProvider))
{
    setWindowTitle(i18n("PDF Review"));
    setModal(false);
    resize(760, 430);
    m_beforeNoteSaveTimer = new QTimer(this);
    m_beforeNoteSaveTimer->setSingleShot(true);
    m_beforeNoteSaveTimer->setInterval(300);
    connect(m_beforeNoteSaveTimer, &QTimer::timeout, this, &ReviewOverview::keepBeforeStep);
    buildUi();
    m_queueRefreshTimer = new QTimer(this);
    m_queueRefreshTimer->setSingleShot(true);
    m_queueRefreshTimer->setInterval(150);
    connect(m_queueRefreshTimer, &QTimer::timeout, this, &ReviewOverview::startQueueRefresh);
    restoreVerifiedCounts();
    loadBarDrafts();
    reload();
}

ReviewOverview::~ReviewOverview()
{
    // A note typed a moment ago is kept too.
    if (m_beforeNoteSaveTimer->isActive()) keepBeforeStep();
    delete m_tagsDialog.data();
    delete m_sessionHostGuard.data();
    if (m_sessionHost) {
        m_sessionHost->removeEventFilter(this);
    }
    delete m_sessionPage.data();
    delete m_reviewVisibilityButton.data();
    m_maskClearer();
}

void ReviewOverview::closeEvent(QCloseEvent *event)
{
    m_pendingSkip = {};
    m_maskClearer();
    m_currentReview = -1;
    m_sessionComplete = false;
    // Preserve the verified landing snapshot if Sync or its source scan is pending.
    m_reviewHistory.clear();
    m_scopedRatingEvents.clear();
    m_retireEvents.clear();
    m_reviewHidden = false;
    m_lastActionWasSkip = false;
    m_reviewQuestion->clear();
    m_reviewStatus->clear();
    m_ratingPanel->hide();
    updateAnswerControls();
    setSessionControlsVisible(false);
    m_reviewStack->setCurrentWidget(m_overviewPage);
    QDialog::closeEvent(event);
}

bool ReviewOverview::eventFilter(QObject *watched, QEvent *event)
{
    // The Tags window belongs to the answer bar: it goes when the bar does.
    if (watched == m_ratingPanel && event->type() == QEvent::Hide) closeTagsDialog();
    if (watched == m_conversationLine && event->type() == QEvent::Resize) elideConversationTitle();
    if (watched == m_beforeSummaryRow && event->type() == QEvent::Resize) elideBeforeSummary();
    if (watched == m_sessionHost
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest)) {
        positionSessionPanel();
    }
    if (watched == m_sessionHost && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "thoughtsNativeDocumentView") {
        applySessionPanelWindowing();
        positionSessionPanel();
    }
    // A floating panel follows its reader window.
    if (m_sessionHost && watched == m_sessionHost->window() && sessionPanelFloats()
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        positionSessionPanel();
    }
    return QDialog::eventFilter(watched, event);
}

void ReviewOverview::buildUi()
{
    static const bool accessibleFactoryInstalled = [] {
        QAccessible::installFactory(reviewOverviewAccessibleFactory);
        return true;
    }();
    Q_UNUSED(accessibleFactoryInstalled)
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(i18n("Document:"), this));
    m_documentFilter = new QComboBox(this);
    m_documentFilter->setObjectName(QStringLiteral("reviewDocumentFilter"));
    m_documentFilter->addItem(i18n("All documents"), QString());
    m_documentFilter->setEnabled(false);
    toolbar->addWidget(m_documentFilter, 1);
    m_browseCardsButton = new QPushButton(i18n("Browse cards"), this);
    m_browseCardsButton->setObjectName(QStringLiteral("reviewBrowseCardsButton"));
    m_browseCardsButton->setAutoDefault(false);
    toolbar->addWidget(m_browseCardsButton);
    // Library-wide review has two entries (review-card-kinds.md §7): Study (knowledge cards) and
    // Projects (project cards, of all Projects or one); a document's review keeps every card.
    m_startButton = new QPushButton(i18n("Start Review"), this);
    m_startButton->setObjectName(QStringLiteral("reviewStartButton"));
    m_startButton->setDefault(true);
    m_startButton->setEnabled(false);
    toolbar->addWidget(m_startButton);
    m_startProjectsButton = new QPushButton(i18n("Projects · %1 due", 0), this);
    m_startProjectsButton->setObjectName(QStringLiteral("reviewStartProjectsButton"));
    m_startProjectsButton->setAutoDefault(false);
    m_startProjectsButton->setToolTip(i18n("Review the project cards: the cards about work you are writing or building"));
    m_projectsMenu = new QMenu(m_startProjectsButton);
    m_projectsMenu->setObjectName(QStringLiteral("reviewProjectsMenu"));
    connect(m_projectsMenu, &QMenu::aboutToShow, this, &ReviewOverview::fillProjectsMenu);
    m_startProjectsButton->setMenu(m_projectsMenu);
    m_startProjectsButton->hide();
    toolbar->addWidget(m_startProjectsButton);
    layout->addLayout(toolbar);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    m_reviewStack = new QStackedWidget(this);
    m_reviewStack->setObjectName(QStringLiteral("reviewPageStack"));
    m_overviewPage = new QWidget(m_reviewStack);
    m_overviewPage->setObjectName(QStringLiteral("reviewOverviewPage"));
    auto *overviewLayout = new QVBoxLayout(m_overviewPage);
    overviewLayout->setContentsMargins(12, 14, 12, 12);
    auto *today = new QLabel(i18n("<h2>Today</h2>"), m_overviewPage);
    today->setObjectName(QStringLiteral("reviewTodayHeading"));
    overviewLayout->addWidget(today);
    m_dueSummary = new QLabel(i18n("Checking cards…"), m_overviewPage);
    m_dueSummary->setObjectName(QStringLiteral("reviewDueSummary"));
    overviewLayout->addWidget(m_dueSummary);

    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(12);
    const auto newTile = createReviewCountTile(m_overviewPage, i18n("New"), QStringLiteral("reviewNewCount"), QColor(95, 165, 214));
    const auto learningTile = createReviewCountTile(m_overviewPage, i18n("Learning"), QStringLiteral("reviewLearningCount"), QColor(155, 111, 209));
    const auto reviewTile = createReviewCountTile(m_overviewPage, i18n("Review"), QStringLiteral("reviewReviewCount"), QColor(73, 166, 194));
    m_newCount = newTile.second;
    m_learningCount = learningTile.second;
    m_reviewCount = reviewTile.second;
    for (auto *count : {m_newCount, m_learningCount, m_reviewCount}) count->setText(QStringLiteral("—"));
    metrics->addWidget(newTile.first, 1);
    metrics->addWidget(learningTile.first, 1);
    // Learning cards are due cards like any other; the dashboard shows them under Review.
    learningTile.first->hide();
    metrics->addWidget(reviewTile.first, 1);
    overviewLayout->addSpacing(10);
    overviewLayout->addLayout(metrics);

    m_attentionPanel = new QFrame(m_overviewPage);
    m_attentionPanel->setObjectName(QStringLiteral("reviewNeedsAttentionPanel"));
    m_attentionPanel->setFrameShape(QFrame::StyledPanel);
    m_attentionPanel->setAutoFillBackground(true);
    QPalette attentionPalette = m_attentionPanel->palette();
    const QColor attentionSurface = attentionPalette.color(QPalette::Window);
    attentionPalette.setColor(QPalette::Window, blendedColor(attentionSurface, QColor(222, 164, 37), attentionSurface.lightnessF() < 0.5 ? 0.20 : 0.16));
    m_attentionPanel->setPalette(attentionPalette);
    auto *attentionLayout = new QHBoxLayout(m_attentionPanel);
    m_attentionSummary = new QLabel(m_attentionPanel);
    m_attentionSummary->setObjectName(QStringLiteral("reviewNeedsAttentionSummary"));
    m_attentionSummary->setWordWrap(true);
    attentionLayout->addWidget(m_attentionSummary, 1);
    m_attentionButton = new QPushButton(i18n("Review problems"), m_attentionPanel);
    m_attentionButton->setObjectName(QStringLiteral("reviewNeedsAttentionButton"));
    m_attentionButton->setAutoDefault(false);
    attentionLayout->addWidget(m_attentionButton);
    overviewLayout->addSpacing(8);
    overviewLayout->addWidget(m_attentionPanel);
    m_attentionPanel->hide();
    overviewLayout->addStretch(1);
    m_reviewStack->addWidget(m_overviewPage);

    m_reviewStack->setCurrentWidget(m_overviewPage);
    layout->addWidget(m_reviewStack, 1);

    auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(closeButtons);

    m_sessionPage = new QFrame(this);
    m_sessionPage->setObjectName(QStringLiteral("reviewSessionPage"));
    m_sessionPage->setFrameShape(QFrame::NoFrame);
    m_sessionPage->setAttribute(Qt::WA_StyledBackground, true);
    m_sessionPage->setStyleSheet(QStringLiteral(
        "QFrame#reviewSessionPage { background: palette(base); border: 1px solid palette(midlight); border-radius: 14px; }"));
    auto *shadow = new QGraphicsDropShadowEffect(m_sessionPage);
    shadow->setBlurRadius(26);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 76));
    m_sessionPage->setGraphicsEffect(shadow);

    auto *sessionLayout = new QVBoxLayout(m_sessionPage);
    sessionLayout->setContentsMargins(18, 14, 18, 14);
    sessionLayout->setSpacing(7);
    auto *questionRow = new QHBoxLayout;
    questionRow->setSpacing(10);
    m_reviewQuestion = new QLabel(m_sessionPage);
    m_reviewQuestion->setObjectName(QStringLiteral("reviewQuestionLabel"));
    m_reviewQuestion->setWordWrap(true);
    m_reviewQuestion->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont questionFont = m_reviewQuestion->font();
    questionFont.setPointSizeF(questionFont.pointSizeF() + 1.0);
    questionFont.setWeight(QFont::DemiBold);
    m_reviewQuestion->setFont(questionFont);
    questionRow->addWidget(m_reviewQuestion, 1);
    // As on the phone: with several answer areas, ‹ › step through them, wrapping around.
    for (const bool next : {false, true}) {
        auto *arrow = new QToolButton(m_sessionPage);
        arrow->setObjectName(next ? QStringLiteral("reviewNextAreaButton") : QStringLiteral("reviewPreviousAreaButton"));
        arrow->setText(next ? QString::fromUtf8("›") : QString::fromUtf8("‹"));
        arrow->setAccessibleName(next ? i18n("Next answer area") : i18n("Previous answer area"));
        arrow->setToolTip(arrow->accessibleName());
        arrow->setAutoRaise(true);
        arrow->setMinimumSize(30, 30);
        arrow->hide();
        connect(arrow, &QToolButton::clicked, this, [this, next] { stepAnswerArea(next ? 1 : -1); });
        questionRow->addWidget(arrow, 0, Qt::AlignTop);
        (next ? m_nextAreaButton : m_previousAreaButton) = arrow;
    }
    m_cardMenuButton = new QToolButton(m_sessionPage);
    m_cardMenuButton->setObjectName(QStringLiteral("reviewCardMenuButton"));
    m_cardMenuButton->setText(QString::fromUtf8("⋮"));
    m_cardMenuButton->setAccessibleName(i18n("Card options"));
    m_cardMenuButton->setToolTip(i18n("Card options"));
    m_cardMenuButton->setAutoRaise(true);
    m_cardMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_cardMenuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; }"));
    m_cardMenuButton->setMinimumSize(30, 30);
    auto *cardMenu = new QMenu(m_cardMenuButton);
    m_cardMenuButton->setMenu(cardMenu);
    // The phone's order. Flip is offered until the answer is shown, Hide again once it is.
    cardMenu->addAction(i18n("Focus"), this, &ReviewOverview::focusReview)->setObjectName(QStringLiteral("reviewFocusAction"));
    cardMenu->addAction(i18n("More info"), this, &ReviewOverview::showCardInfo)->setObjectName(QStringLiteral("reviewMoreInfoAction"));
    cardMenu->addAction(i18n("Copy for agent"), this, &ReviewOverview::copyCardForAgent)->setObjectName(QStringLiteral("reviewCopyForAgentAction"));
    cardMenu->addAction(i18n("Edit"), this, &ReviewOverview::editCurrentCard)->setObjectName(QStringLiteral("reviewEditAction"));
    cardMenu->addAction(i18n("Skip"), this, &ReviewOverview::chooseSkip)->setObjectName(QStringLiteral("reviewSkipAction"));
    cardMenu->addAction(i18n("Undo"), this, &ReviewOverview::undoPrevious)->setObjectName(QStringLiteral("reviewUndoAction"));
    // Flip hides the answer's context instead, so the passage itself prompts recall; it reads Unflip then.
    cardMenu->addAction(i18n("Flip"), this, &ReviewOverview::toggleMaskFlipped)->setObjectName(QStringLiteral("reviewFlipAction"));
    cardMenu->addAction(i18n("Hide again"), this, &ReviewOverview::hideAnswer)->setObjectName(QStringLiteral("reviewHideAgainAction"));
    // The card's History (review answers and answer feedback by PDF version): its document's
    // History in the sidebar of the tab under review, at the card.
    m_feedbackHistoryAction = cardMenu->addAction(i18n("Show history"), this, [this] {
        if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
        Q_EMIT cardHistoryRequested(m_dueCards.at(m_currentReview).id());
    });
    m_feedbackHistoryAction->setObjectName(QStringLiteral("reviewShowHistoryAction"));
    m_feedbackHistoryAction->setVisible(false);
    // Every agent conversation linked to the card, newest first (filled when the menu opens).
    m_agentConversationsMenu = cardMenu->addMenu(i18n("Agent conversations"));
    m_agentConversationsMenu->setObjectName(QStringLiteral("reviewAgentConversationsMenu"));
    m_agentConversationsMenu->menuAction()->setObjectName(QStringLiteral("reviewAgentConversationsAction"));
    // Knowledge card or project card (review-card-kinds.md §5); the texts follow the card shown.
    auto *kindAction = cardMenu->addAction(i18n("Make project card"));
    kindAction->setObjectName(QStringLiteral("reviewKindAction"));
    connect(kindAction, &QAction::triggered, this, [this, kindAction] { setCurrentCardKind(kindAction->data().toString()); });
    auto *followAction = cardMenu->addAction(i18n("Follow the Project (Knowledge)"));
    followAction->setObjectName(QStringLiteral("reviewFollowProjectAction"));
    connect(followAction, &QAction::triggered, this, [this] { setCurrentCardKind(ReviewKinds::followProject()); });
    // Retire, right above Delete: out of review for good, history kept; Undo brings it back.
    cardMenu->addAction(i18n("Retire card"), this, &ReviewOverview::retireCard)->setObjectName(QStringLiteral("reviewRetireAction"));
    cardMenu->addAction(i18n("Delete card"), this, &ReviewOverview::deleteCard)->setObjectName(QStringLiteral("reviewDeleteAction"));
    cardMenu->addAction(i18n("End review"), this, &ReviewOverview::showReviewLanding)->setObjectName(QStringLiteral("reviewEndSessionAction"));
    connect(cardMenu, &QMenu::aboutToShow, this, &ReviewOverview::updateCardMenu);
    connect(cardMenu, &QMenu::aboutToShow, this, &ReviewOverview::fillAgentConversationsMenu);
    questionRow->addWidget(m_cardMenuButton, 0, Qt::AlignTop);
    // S skips the card as the Skip dialog would by default, without asking. The panel lives in the
    // reader window while reviewing, so the shortcut works there; text fields keep their S.
    auto *skipShortcut = new QShortcut(QKeySequence(Qt::Key_S), m_sessionPage);
    skipShortcut->setObjectName(QStringLiteral("reviewSkipShortcut"));
    skipShortcut->setContext(Qt::WindowShortcut);
    connect(skipShortcut, &QShortcut::activated, this, &ReviewOverview::skipWithDefaults);
    sessionLayout->addLayout(questionRow);

    m_reviewStatus = new QLabel(m_sessionPage);
    m_reviewStatus->setObjectName(QStringLiteral("reviewStatusLabel"));
    m_reviewStatus->setWordWrap(true);
    QPalette statusPalette = m_reviewStatus->palette();
    statusPalette.setColor(QPalette::WindowText, statusPalette.color(QPalette::PlaceholderText));
    m_reviewStatus->setPalette(statusPalette);
    sessionLayout->addWidget(m_reviewStatus);
    // A project card says so, and when its passage changed since it was last answered.
    m_cardKindLine = new QLabel(m_sessionPage);
    m_cardKindLine->setObjectName(QStringLiteral("reviewCardKindLine"));
    m_cardKindLine->setWordWrap(true);
    m_cardKindLine->setTextFormat(Qt::PlainText);
    m_cardKindLine->hide();
    sessionLayout->addWidget(m_cardKindLine);

    // The review in two steps (review-phases spec): under the question, before the answer is
    // shown, the groups asked before it (a slider each) and the before note; once the answer is
    // shown, one locked line with what was picked, and the agent's prediction under it.
    m_beforePanel = new QWidget(m_sessionPage);
    m_beforePanel->setObjectName(QStringLiteral("reviewBeforePanel"));
    {
        auto *before = new QVBoxLayout(m_beforePanel);
        before->setContentsMargins(0, 0, 0, 0);
        before->setSpacing(4);
        m_beforeGroups = new QWidget(m_beforePanel);
        m_beforeGroups->setObjectName(QStringLiteral("reviewBeforeGroups"));
        auto *groups = new QVBoxLayout(m_beforeGroups);
        groups->setContentsMargins(0, 0, 0, 0);
        before->addWidget(m_beforeGroups);
        m_beforeNote = new QPlainTextEdit(m_beforePanel);
        m_beforeNote->setObjectName(QStringLiteral("reviewBeforeNote"));
        m_beforeNote->setPlaceholderText(i18n("Before note: what you expect, before seeing the answer…"));
        m_beforeNote->setAccessibleName(i18n("Before note"));
        m_beforeNote->setTabChangesFocus(true);
        m_beforeNote->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_beforeNote->setFixedHeight(m_beforeNote->fontMetrics().lineSpacing() * 2 + int(std::ceil(2 * m_beforeNote->document()->documentMargin()))
                                     + 2 * m_beforeNote->frameWidth() + 2);
        connect(m_beforeNote, &QPlainTextEdit::textChanged, this, [this] {
            QString text = m_beforeNote->toPlainText();
            if (const QString cut = ReviewTags::cutGroupNote(text); cut.size() < text.size()) {
                text = cut;
                const QSignalBlocker blocker(m_beforeNote);
                m_beforeNote->setPlainText(text);
                m_beforeNote->moveCursor(QTextCursor::End);
            }
            // Typing after the answer was shown is not "before": the box is hidden by then.
            if (!m_before.locked()) {
                m_before.note = text;
                m_beforeNoteSaveTimer->start();
            }
        });
        before->addWidget(m_beforeNote);
        // Locked: one line with the picks, as high as its text, and the pencil that opens them again.
        m_beforeSummaryRow = new QWidget(m_beforePanel);
        m_beforeSummaryRow->setObjectName(QStringLiteral("reviewBeforeSummaryRow"));
        auto *summaryRow = new QHBoxLayout(m_beforeSummaryRow);
        summaryRow->setContentsMargins(0, 0, 0, 0);
        summaryRow->setSpacing(4);
        m_beforeSummary = new QLabel(m_beforeSummaryRow);
        m_beforeSummary->setObjectName(QStringLiteral("reviewBeforeSummary"));
        m_beforeSummary->setTextFormat(Qt::PlainText);
        // Elided to the bar's width (elideBeforeSummary), never wrapped: the bar keeps its height.
        m_beforeSummary->setMinimumWidth(1);
        summaryRow->addWidget(m_beforeSummary, 0, Qt::AlignVCenter);
        m_beforeEditButton = new QToolButton(m_beforeSummaryRow);
        m_beforeEditButton->setObjectName(QStringLiteral("reviewBeforeEditButton"));
        const QIcon pencil = QIcon::fromTheme(QStringLiteral("document-edit"));
        if (pencil.isNull()) m_beforeEditButton->setText(QStringLiteral("✏\uFE0E"));
        else m_beforeEditButton->setIcon(pencil);
        m_beforeEditButton->setAutoRaise(true);
        m_beforeEditButton->setFocusPolicy(Qt::TabFocus);
        m_beforeEditButton->setToolTip(i18n("Change what you picked before seeing the answer"));
        m_beforeEditButton->setAccessibleName(m_beforeEditButton->toolTip());
        const int line = m_beforeSummary->fontMetrics().height();
        m_beforeEditButton->setIconSize(QSize(line - 2, line - 2));
        m_beforeEditButton->setFixedSize(line + 4, line);
        connect(m_beforeEditButton, &QToolButton::clicked, this, &ReviewOverview::reopenBeforeStep);
        summaryRow->addWidget(m_beforeEditButton, 0, Qt::AlignVCenter);
        summaryRow->addStretch(1);
        m_beforeSummaryRow->installEventFilter(this);
        m_beforeSummaryRow->hide();
        before->addWidget(m_beforeSummaryRow);
        m_predictionLine = new QLabel(m_beforePanel);
        m_predictionLine->setObjectName(QStringLiteral("reviewAgentPrediction"));
        m_predictionLine->setWordWrap(true);
        for (QLabel *label : {m_beforeSummary, m_predictionLine}) {
            label->setTextFormat(Qt::PlainText);
            QPalette secondary = label->palette();
            secondary.setColor(QPalette::WindowText, secondary.color(QPalette::PlaceholderText));
            label->setPalette(secondary);
        }
        m_predictionLine->hide();
        before->addWidget(m_predictionLine);
    }
    m_beforePanel->hide();
    sessionLayout->addWidget(m_beforePanel);

    // The card's newest agent conversation, one flat line above Reveal; no space is kept for it
    // when the card has none.
    m_conversationLine = new QPushButton(m_sessionPage);
    m_conversationLine->setObjectName(QStringLiteral("reviewAgentConversationLine"));
    m_conversationLine->setFlat(true);
    m_conversationLine->setAutoDefault(false);
    m_conversationLine->setFocusPolicy(Qt::NoFocus);
    m_conversationLine->setCursor(Qt::PointingHandCursor);
    m_conversationLine->setMinimumWidth(0);
    m_conversationLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_conversationLine->setStyleSheet(QStringLiteral("QPushButton#reviewAgentConversationLine { border: none; border-radius: 6px; padding: 0px; text-align: left; }"
                                                     "QPushButton#reviewAgentConversationLine:hover { background: palette(alternate-base); }"));
    {
        auto *line = new QHBoxLayout(m_conversationLine);
        line->setContentsMargins(4, 3, 4, 3);
        line->setSpacing(6);
        m_conversationIcon = new QLabel(m_conversationLine);
        m_conversationIcon->setObjectName(QStringLiteral("reviewAgentConversationIcon"));
        m_conversationTitle = new QLabel(m_conversationLine);
        m_conversationTitle->setObjectName(QStringLiteral("reviewAgentConversationTitle"));
        m_conversationTitle->setMinimumWidth(0);
        m_conversationDetails = new QLabel(m_conversationLine);
        m_conversationDetails->setObjectName(QStringLiteral("reviewAgentConversationDetails"));
        QPalette secondary = m_conversationDetails->palette();
        secondary.setColor(QPalette::WindowText, secondary.color(QPalette::PlaceholderText));
        m_conversationDetails->setPalette(secondary);
        m_conversationStatus = new QLabel(m_conversationLine);
        m_conversationStatus->setObjectName(QStringLiteral("reviewAgentConversationStatus"));
        m_conversationStatus->setFixedSize(8, 8);
        m_conversationStatus->hide();
        for (QLabel *label : {m_conversationIcon, m_conversationTitle, m_conversationDetails, m_conversationStatus}) {
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            line->addWidget(label, 0, Qt::AlignVCenter);
        }
        line->addStretch(1);
    }
    m_conversationLine->installEventFilter(this);
    m_conversationLine->hide();
    connect(m_conversationLine, &QPushButton::clicked, this, [this] { openCardConversation(m_conversationShown); });
    sessionLayout->addWidget(m_conversationLine);
    // Whether the Claude session runs, and works: checked only while the line shows.
    m_conversationStatusTimer = new QTimer(this);
    m_conversationStatusTimer->setInterval(5000);
    connect(m_conversationStatusTimer, &QTimer::timeout, this, &ReviewOverview::updateConversationStatus);

    // As on the phone: one Reveal, which shows the answer with the current state and the bar.
    // Flip is in the card menu until then; Hide again there covers the answer again.
    m_answerRow = new QWidget(m_sessionPage);
    m_answerRow->setObjectName(QStringLiteral("reviewAnswerRow"));
    auto *answerRow = new QHBoxLayout(m_answerRow);
    answerRow->setContentsMargins(0, 0, 0, 0);
    answerRow->setSpacing(6);
    m_revealButton = new QPushButton(i18n("Reveal"), m_answerRow);
    m_revealButton->setObjectName(QStringLiteral("reviewRevealButton"));
    m_revealButton->setToolTip(i18n("Show the answer and choose its tags and when to review it again"));
    m_revealButton->setAutoDefault(false);
    m_revealButton->setMinimumWidth(0);
    m_revealButton->setMinimumHeight(36);
    m_revealButton->setStyleSheet(QStringLiteral("QPushButton { background: palette(highlight); color: palette(highlighted-text); border: none; border-radius: 10px; padding: 6px 10px; }"
                                                 "QPushButton:disabled { background: palette(mid); }"));
    answerRow->addWidget(m_revealButton, 1);
    sessionLayout->addWidget(m_answerRow);

    m_currentState = new QLabel(m_sessionPage);
    m_currentState->setObjectName(QStringLiteral("reviewCurrentState"));
    m_currentState->setWordWrap(true);
    QPalette currentPalette = m_currentState->palette();
    currentPalette.setColor(QPalette::WindowText, currentPalette.color(QPalette::PlaceholderText));
    m_currentState->setPalette(currentPalette);
    m_currentState->hide();
    sessionLayout->addWidget(m_currentState);

    m_ratingPanel = new QWidget(m_sessionPage);
    m_ratingPanel->installEventFilter(this);
    m_ratingPanel->setObjectName(QStringLiteral("reviewRatingPanel"));
    auto *ratings = new QHBoxLayout(m_ratingPanel);
    ratings->setContentsMargins(0, 0, 0, 0);
    ratings->setSpacing(6);
    // Two-line cells: a small caption over the value, as the phone's labelled fields.
    const auto field = [this, ratings](const QString &name, const QString &caption) {
        auto *button = new QPushButton(m_ratingPanel);
        button->setObjectName(name);
        button->setAutoDefault(false);
        button->setMinimumWidth(0);
        button->setMinimumHeight(52);
        button->setStyleSheet(reviewCellStyle({}));
        auto *column = new QVBoxLayout(button);
        column->setContentsMargins(8, 4, 8, 4);
        column->setSpacing(0);
        auto *captionLabel = new QLabel(caption, button);
        captionLabel->setObjectName(QStringLiteral("reviewFieldCaption"));
        QFont small = captionLabel->font();
        small.setPointSizeF(small.pointSizeF() * 0.82);
        captionLabel->setFont(small);
        QPalette captionPalette = captionLabel->palette();
        captionPalette.setColor(QPalette::WindowText, captionPalette.color(QPalette::PlaceholderText));
        captionLabel->setPalette(captionPalette);
        auto *value = new QLabel(button);
        value->setObjectName(QStringLiteral("reviewFieldValue"));
        for (QLabel *label : {captionLabel, value}) {
            label->setAlignment(Qt::AlignCenter);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            column->addWidget(label);
        }
        ratings->addWidget(button, 1);
        return button;
    };
    m_tagField = field(QStringLiteral("reviewTagField"), i18n("Rating"));
    m_intervalField = field(QStringLiteral("reviewIntervalField"), i18n("Interval"));
    connect(m_tagField, &QPushButton::clicked, this, &ReviewOverview::chooseTags);
    connect(m_intervalField, &QPushButton::clicked, this, &ReviewOverview::chooseInterval);
    const QColor accent = palette().color(QPalette::Highlight);
    const QString accentRgb = QStringLiteral("%1, %2, %3").arg(accent.red()).arg(accent.green()).arg(accent.blue());
    m_nextButton = new QPushButton(i18n("Next"), m_ratingPanel);
    m_nextButton->setObjectName(QStringLiteral("reviewNextButton"));
    m_nextButton->setAutoDefault(false);
    m_nextButton->setMinimumWidth(0);
    m_nextButton->setMinimumHeight(52);
    m_nextButton->setStyleSheet(QStringLiteral("QPushButton { background: rgba(%1, 42); border: 1px solid rgba(%1, 150); border-radius: 10px; padding: 6px 10px; }"
                                               "QPushButton:hover { background: rgba(%1, 70); }"
                                               "QPushButton:disabled { background: transparent; border-color: palette(mid); color: palette(placeholder-text); }").arg(accentRgb));
    // Right-click keeps the desktop's other answers: Never, or a whole document or project.
    m_nextButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_nextButton, &QPushButton::clicked, this, &ReviewOverview::recordNext);
    connect(m_nextButton, &QWidget::customContextMenuRequested, this, [this] { chooseAnswer(m_selectedTags); });
    ratings->addWidget(m_nextButton, 1);
    m_ratingPanel->hide();
    sessionLayout->addWidget(m_ratingPanel);

    m_doneButton = new QPushButton(i18n("Back to Today"), m_sessionPage);
    m_doneButton->setObjectName(QStringLiteral("reviewDoneButton"));
    sessionLayout->addWidget(m_doneButton);
    m_sessionPage->hide();

    connect(m_browseCardsButton, &QPushButton::clicked, this, &ReviewOverview::showBrowseCards);
    connect(m_attentionButton, &QPushButton::clicked, this, &ReviewOverview::showAttentionCards);
    connect(m_doneButton, &QPushButton::clicked, this, &ReviewOverview::showReviewLanding);
    connect(m_documentFilter, &QComboBox::currentIndexChanged, this, &ReviewOverview::refreshDueQueue);
    connect(m_startButton, &QPushButton::clicked, this, &ReviewOverview::startReview);
    connect(m_revealButton, &QPushButton::clicked, this, &ReviewOverview::revealAnswer);
    setSessionControlsVisible(false);
}

QString ReviewOverview::dashboardSnapshotPath() const
{
    const auto root = QDir(m_docsRoot).absolutePath().toUtf8();
    const auto key = QString::fromLatin1(QCryptographicHash::hash(root, QCryptographicHash::Sha256).toHex());
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("review-dashboard/%1.json").arg(key));
}

void ReviewOverview::restoreVerifiedCounts()
{
    QFile file(dashboardSnapshotPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto snapshot = QJsonDocument::fromJson(file.readAll()).object();
    const auto counts = snapshot.value(QStringLiteral("counts")).toArray();
    if (snapshot.value(QStringLiteral("schema_version")).toInt() != 1 || counts.size() != 4) return;
    for (const auto &value : counts) {
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 10000000
            || value.toDouble() != value.toInt()) return;
    }
    // Only presentation is restored: cached counts never authorize a review.
    const QSignalBlocker blocker(m_documentFilter);
    for (const auto &value : snapshot.value(QStringLiteral("documents")).toArray()) {
        const auto stem = value.toString();
        if (!stem.isEmpty() && m_documentFilter->findData(stem) < 0) m_documentFilter->addItem(stem, stem);
    }
    // The saved counts do not say which cards are project cards: Study shows them all until the
    // background refresh counts them again.
    m_studyDue = counts[0].toInt() + counts[1].toInt() + counts[2].toInt();
    m_projectDue = 0;
    displayCounts(counts[0].toInt(), counts[1].toInt(), counts[2].toInt(), counts[3].toInt());
    // Shown with the queue loaded here until the background refresh counts them again.
    m_unavailableCount = counts[3].toInt();
    if (refreshDeferred()) applyDeferredState();
}

QString ReviewOverview::barDraftsPath() const
{
    const auto root = QDir(m_docsRoot).absolutePath().toUtf8();
    const auto key = QString::fromLatin1(QCryptographicHash::hash(root, QCryptographicHash::Sha256).toHex());
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("review-skip-drafts/%1.json").arg(key));
}

void ReviewOverview::loadBarDrafts()
{
    QFile file(barDraftsPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    if (saved.value(QStringLiteral("schema_version")).toInt() != 1) return;
    const auto stringHash = [](const QJsonObject &object) {
        QHash<QString, QString> hash;
        for (auto it = object.begin(); it != object.end(); ++it) hash.insert(it.key(), it.value().toString());
        return hash;
    };
    const auto noteOf = [](const QJsonObject &object) {
        return ReviewContextNotes::Note{object.value(QStringLiteral("key")).toString(), object.value(QStringLiteral("label")).toString(),
                                        object.value(QStringLiteral("text")).toString()};
    };
    // A card skipped and never shown again (deleted, or reviewed elsewhere) is not kept forever.
    const auto oldest = QDateTime::currentDateTimeUtc().addDays(-30);
    const auto drafts = saved.value(QStringLiteral("drafts")).toObject();
    for (auto it = drafts.begin(); it != drafts.end(); ++it) {
        const auto value = it.value().toObject();
        BarDraft draft;
        draft.savedAt = QDateTime::fromString(value.value(QStringLiteral("saved_at")).toString(), Qt::ISODateWithMs);
        if (it.key().isEmpty() || !draft.savedAt.isValid() || draft.savedAt < oldest) continue;
        for (const auto &tag : value.value(QStringLiteral("tags")).toArray()) draft.tags.append(tag.toString());
        const auto interval = value.value(QStringLiteral("interval")).toObject();
        const int unit = interval.value(QStringLiteral("unit")).toInt(-1);
        if (unit < ReviewInterval::Minutes || unit > ReviewInterval::Months) continue;
        draft.interval = {interval.value(QStringLiteral("amount")).toDouble(), ReviewInterval::Unit(unit),
                          QDate::fromString(interval.value(QStringLiteral("date")).toString(), Qt::ISODate)};
        draft.groupNotes = stringHash(value.value(QStringLiteral("group_notes")).toObject());
        // Invalid (empty) for a card never answered, as its schedule has it.
        draft.answeredAt = QDateTime::fromString(value.value(QStringLiteral("answered_at")).toString(), Qt::ISODateWithMs);
        const auto context = value.value(QStringLiteral("context")).toObject();
        draft.context.cardId = context.value(QStringLiteral("card_id")).toString();
        draft.context.selected = context.value(QStringLiteral("selected")).toInt();
        for (const auto &entryValue : context.value(QStringLiteral("entries")).toArray()) {
            const auto entry = entryValue.toObject();
            draft.context.entries.append({entry.value(QStringLiteral("scope")).toString(), noteOf(entry.value(QStringLiteral("note")).toObject()),
                                          entry.value(QStringLiteral("text")).toString(), entry.value(QStringLiteral("available")).toBool()});
        }
        m_barDrafts.insert(it.key(), draft);
    }
}

void ReviewOverview::saveBarDrafts() const
{
    const auto stringObject = [](const QHash<QString, QString> &hash) {
        QJsonObject object;
        for (auto it = hash.begin(); it != hash.end(); ++it) object.insert(it.key(), it.value());
        return object;
    };
    QJsonObject drafts;
    for (auto it = m_barDrafts.begin(); it != m_barDrafts.end(); ++it) {
        const BarDraft &draft = it.value();
        QJsonArray entries;
        for (const auto &entry : draft.context.entries)
            entries.append(QJsonObject{{QStringLiteral("scope"), entry.scope},
                                       {QStringLiteral("note"), QJsonObject{{QStringLiteral("key"), entry.note.key}, {QStringLiteral("label"), entry.note.label},
                                                                            {QStringLiteral("text"), entry.note.text}}},
                                       {QStringLiteral("text"), entry.text}, {QStringLiteral("available"), entry.available}});
        drafts.insert(it.key(), QJsonObject{
            {QStringLiteral("tags"), QJsonArray::fromStringList(draft.tags)},
            {QStringLiteral("interval"), QJsonObject{{QStringLiteral("amount"), draft.interval.amount}, {QStringLiteral("unit"), int(draft.interval.unit)},
                                                     {QStringLiteral("date"), draft.interval.date.toString(Qt::ISODate)}}},
            {QStringLiteral("group_notes"), stringObject(draft.groupNotes)},
            {QStringLiteral("answered_at"), draft.answeredAt.isValid() ? draft.answeredAt.toUTC().toString(Qt::ISODateWithMs) : QString()},
            {QStringLiteral("context"), QJsonObject{{QStringLiteral("card_id"), draft.context.cardId}, {QStringLiteral("selected"), draft.context.selected},
                                                    {QStringLiteral("entries"), entries}}},
            {QStringLiteral("saved_at"), (draft.savedAt.isValid() ? draft.savedAt : QDateTime::currentDateTimeUtc()).toString(Qt::ISODateWithMs)}});
    }
    const QString path = barDraftsPath();
    if (drafts.isEmpty()) {
        QFile::remove(path);
        return;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    QSaveFile file(path);
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("drafts"), drafts}}).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}

namespace
{
// The kept before steps by card ID, as beforeStepsPath() holds them; read and written whole each
// time, so that two review windows never write back each other's older copy.
QJsonObject readBeforeSteps(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    if (saved.value(QStringLiteral("schema_version")).toInt() != 1) return {};
    return saved.value(QStringLiteral("steps")).toObject();
}

void writeBeforeSteps(const QString &path, const QJsonObject &steps)
{
    if (steps.isEmpty()) {
        QFile::remove(path);
        return;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    QSaveFile file(path);
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("steps"), steps}}).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}

QString beforeStepTime(const QDateTime &time)
{
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}
}

QString ReviewOverview::beforeStepsPath() const
{
    const auto root = QDir(m_docsRoot).absolutePath().toUtf8();
    const auto key = QString::fromLatin1(QCryptographicHash::hash(root, QCryptographicHash::Sha256).toHex());
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("review-before-steps/%1.json").arg(key));
}

ReviewOverview::BeforeStep ReviewOverview::keptBeforeStep(const ReviewCard &card) const
{
    BeforeStep step{card.id(), {}, {}, {}, false};
    step.answeredAt = card.schedule.lastReviewedAt;
    const QString path = beforeStepsPath();
    QJsonObject steps = readBeforeSteps(path);
    const auto found = steps.constFind(card.id());
    if (found == steps.constEnd()) return step;
    const QJsonObject kept = found->toObject();
    QStringList tags;
    for (const auto &tag : kept.value(QStringLiteral("tags")).toArray()) {
        if (!tag.toString().isEmpty()) tags.append(tag.toString());
    }
    const QString note = kept.value(QStringLiteral("note")).toString();
    // Started after another answer than the card's last: it was answered since, here or elsewhere.
    if (kept.value(QStringLiteral("answered_at")).toString() != beforeStepTime(card.schedule.lastReviewedAt) || (tags.isEmpty() && note.trimmed().isEmpty())) {
        steps.remove(card.id());
        writeBeforeSteps(path, steps);
        DeveloperLog::record(QStringLiteral("review.before_step.obsolete"), {{QStringLiteral("card_id"), card.id()}});
        return step;
    }
    step.tags = tags;
    step.note = note;
    step.answerShownAt = QDateTime::fromString(kept.value(QStringLiteral("answer_shown_at")).toString(), Qt::ISODateWithMs);
    step.reopened = step.answerShownAt.isValid() && kept.value(QStringLiteral("reopened")).toBool();
    step.shown = true;
    DeveloperLog::record(QStringLiteral("review.before_step.restored"),
                         {{QStringLiteral("card_id"), card.id()}, {QStringLiteral("locked"), step.locked()}, {QStringLiteral("tags"), int(tags.size())},
                          {QStringLiteral("note"), !note.trimmed().isEmpty()}});
    return step;
}

void ReviewOverview::keepBeforeStep()
{
    m_beforeNoteSaveTimer->stop();
    if (m_before.cardId.isEmpty() || !m_before.shown) return;
    const QString path = beforeStepsPath();
    QJsonObject steps = readBeforeSteps(path);
    if (m_before.tags.isEmpty() && m_before.note.trimmed().isEmpty()) {
        if (!steps.contains(m_before.cardId)) return;
        steps.remove(m_before.cardId);
    } else {
        QJsonObject step{{QStringLiteral("tags"), QJsonArray::fromStringList(m_before.tags)},
                         {QStringLiteral("note"), m_before.note},
                         {QStringLiteral("answered_at"), beforeStepTime(m_before.answeredAt)},
                         {QStringLiteral("saved_at"), beforeStepTime(QDateTime::currentDateTimeUtc())}};
        if (m_before.answerShownAt.isValid()) step.insert(QStringLiteral("answer_shown_at"), beforeStepTime(m_before.answerShownAt));
        if (m_before.reopened) step.insert(QStringLiteral("reopened"), true);
        steps.insert(m_before.cardId, step);
    }
    writeBeforeSteps(path, steps);
}

void ReviewOverview::forgetBeforeSteps(const QStringList &cardIds)
{
    // A note typed a moment ago must not bring the step of an answered card back.
    if (cardIds.contains(m_before.cardId)) m_beforeNoteSaveTimer->stop();
    const QString path = beforeStepsPath();
    QJsonObject steps = readBeforeSteps(path);
    bool changed = false;
    for (const QString &cardId : cardIds) {
        if (!steps.contains(cardId)) continue;
        steps.remove(cardId);
        changed = true;
    }
    if (changed) writeBeforeSteps(path, steps);
}

void ReviewOverview::saveVerifiedCounts(int newCards, int learningCards, int reviewCards, int unavailable) const
{
    // A single-card session or document filter is not the All documents dashboard.
    if (m_currentReview >= 0 || !m_documentFilter->currentData().toString().isEmpty()) return;
    QJsonArray documents;
    for (int i = 1; i < m_documentFilter->count(); ++i) documents.append(m_documentFilter->itemData(i).toString());
    const QJsonObject snapshot{{QStringLiteral("schema_version"), 1},
        {QStringLiteral("verified_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("counts"), QJsonArray{newCards, learningCards, reviewCards, unavailable}},
        {QStringLiteral("documents"), documents}};
    const QString path = dashboardSnapshotPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    QSaveFile file(path);
    const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}

bool ReviewOverview::refreshDeferred() const
{
    return m_syncInProgress || sourceLocationIndex(m_docsRoot).value(QStringLiteral("refreshing")).toBool();
}

void ReviewOverview::setSyncInProgress(bool running)
{
    if (m_syncInProgress == running) return;
    m_syncInProgress = running;
    // Sync only freezes the displayed counts. Starting a review keeps working on
    // the cards already loaded; ratings are saved locally and sent by the next Sync.
    if (running) applyDeferredState();
    else reloadFromProjections();
}

void ReviewOverview::applyDeferredState()
{
    if (!refreshDeferred()) return;
    // The counts stay frozen, but Start Review reads the actual queue when pressed,
    // so the frozen dashboard must stay usable (also when it was opened, or a session
    // ended, after the deferral began).
    if (m_countsShown) {
        m_documentFilter->setEnabled(true);
        updateEntryButtons();
    }
    // Sync's end reloads by itself. A source scan that outlasts Sync only says so
    // through projection updates, which may already have come: retry until it is done.
    if (m_syncInProgress || m_deferredRetryScheduled) return;
    m_deferredRetryScheduled = true;
    QTimer::singleShot(1500, this, [this] {
        m_deferredRetryScheduled = false;
        if (m_syncInProgress) return;
        if (!refreshDeferred()) {
            reloadFromProjections();
        } else if (isVisible() || m_currentReview >= 0) {
            applyDeferredState();
        }
        // A hidden dashboard stops polling; reopening it reloads (or retries) anyway.
    });
}

void ReviewOverview::reload()
{
    if (refreshDeferred()) {
        applyDeferredState();
        return;
    }
    convertWholeDocumentCards();
    // During a review the card shown stays the one reviewed (or the card a merge made of it): a
    // new queue taken by position would put another card under the bar (Merge cards, 13 Nov).
    if (!m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size()) {
        reloadFromProjections();
        return;
    }
    // The queue, the document filter and the counts: built off the GUI thread.
    requestQueueRefresh();
}

void ReviewOverview::showBrowseCards()
{
    openCardBrowser(QStringLiteral("active"));
}

void ReviewOverview::showAttentionCards()
{
    openCardBrowser(m_attentionBrowserState);
}

void ReviewOverview::openCardBrowser(const QString &stateFilter)
{
    if (!m_cardBrowserOpener) {
        return;
    }
    setSessionControlsVisible(false);
    hide();
    m_cardBrowserOpener(stateFilter);
}

void ReviewOverview::showLandingPage()
{
    showReviewLanding();
}

void ReviewOverview::showReviewLanding()
{
    closeTagsDialog();
    m_pendingSkip = {};
    m_lostPresentationCardId.clear();
    m_maskClearer();
    m_currentReview = -1;
    m_sessionComplete = false;
    // Reopening during Sync must retain the last verified queue and counts.
    m_reviewHistory.clear();
    m_scopedRatingEvents.clear();
    m_retireEvents.clear();
    m_reviewHidden = false;
    m_lastActionWasSkip = false;
    m_reviewStatus->clear();
    m_reviewQuestion->clear();
    setSessionControlsVisible(false);
    refreshDueQueue();
    m_reviewStack->setCurrentWidget(m_overviewPage);
    show();
    raise();
    activateWindow();
}

void ReviewOverview::setSessionControlsVisible(bool visible)
{
    if (!m_sessionPage) {
        return;
    }
    if (!visible) {
        m_sessionPage->hide();
        if (m_reviewVisibilityButton) m_reviewVisibilityButton->hide();
        if (m_conversationStatusTimer) m_conversationStatusTimer->stop();
        return;
    }
    updateCardMenu();
    updateSessionExtras();
    attachSessionPanel();
    // After the bar shows: the line's status is checked only while it is visible.
    refreshCardConversations(false);
}

void ReviewOverview::attachSessionPanel()
{
    QWidget *host = m_sessionHostProvider ? m_sessionHostProvider() : parentWidget();
    if (!host) {
        host = this;
    }
    if (m_sessionHost != host) {
        if (m_sessionHost) {
            m_sessionHost->removeEventFilter(this);
        }
        delete m_reviewVisibilityButton.data();
        delete m_sessionHostGuard.data();
        if (host != this) {
            m_sessionHostGuard = new SessionHostGuard(host, [overview = QPointer<ReviewOverview>(this)] {
                if (overview) {
                    overview->releaseSessionHost();
                }
            });
        }
        m_sessionPage->setParent(host);
        m_sessionHost = host;
        m_sessionHost->installEventFilter(this);
        m_reviewVisibilityButton = new QPushButton(host);
        m_reviewVisibilityButton->setObjectName(QStringLiteral("reviewVisibilityButton"));
        m_reviewVisibilityButton->setAutoDefault(false);
        m_reviewVisibilityButton->setMinimumHeight(32);
        connect(m_reviewVisibilityButton, &QPushButton::clicked, this, &ReviewOverview::toggleReviewVisibility);
    }

    applySessionPanelWindowing();
    hide();
    QWidget *const viewerWindow = host->window();
    if (viewerWindow && viewerWindow != this) {
        viewerWindow->show();
        viewerWindow->raise();
        viewerWindow->activateWindow();
    }
    m_sessionPage->setVisible(!m_reviewHidden);
    updateReviewVisibilityButton();
    m_reviewVisibilityButton->setVisible(!m_sessionComplete);
    positionSessionPanel();
    m_sessionPage->raise();
    m_reviewVisibilityButton->raise();
}

// The review panel lives in a reader tab while reviewing. When that tab or its
// window closes, take the panel back: deleted with the host, every pointer to
// its rating panel, labels and buttons would dangle, and the next Sync update
// (reloadFromProjections, refreshDueQueue) would use freed widgets.
void ReviewOverview::releaseSessionHost()
{
    if (m_sessionHost) {
        m_sessionHost->removeEventFilter(this);
    }
    m_sessionHost = nullptr;
    if (m_sessionPage) {
        m_sessionPage->hide();
        m_sessionPage->setParent(this, Qt::Widget);
    }
    if (!std::exchange(m_maskedHistoryCard, QString()).isEmpty()) Q_EMIT maskedHistoryCardChanged({});
}

bool ReviewOverview::sessionPanelFloats() const
{
    return m_sessionPage && m_sessionPage->isWindow() && m_sessionHost && m_sessionPage->parentWidget() == m_sessionHost;
}

void ReviewOverview::applySessionPanelWindowing()
{
    if (!m_sessionHost || !m_sessionPage || m_sessionHost == this) return;
    const bool floating = m_sessionHost->property("thoughtsNativeDocumentView").toBool();
    if (floating == sessionPanelFloats()) return;
    const Qt::WindowFlags flags = floating ? (Qt::Tool | Qt::FramelessWindowHint) : Qt::Widget;
    const bool pageVisible = !m_sessionPage->isHidden();
    m_sessionPage->setParent(m_sessionHost, flags);
    m_sessionPage->setVisible(pageVisible);
    if (m_reviewVisibilityButton) {
        const bool buttonVisible = !m_reviewVisibilityButton->isHidden();
        m_reviewVisibilityButton->setParent(m_sessionHost, flags);
        m_reviewVisibilityButton->setVisible(buttonVisible);
    }
    if (QWidget *window = m_sessionHost->window(); window && window != m_sessionHost) {
        if (floating) window->installEventFilter(this);
        else window->removeEventFilter(this);
    }
}

void ReviewOverview::positionSessionPanel()
{
    if (!m_sessionHost || !m_sessionPage || m_sessionHost->width() <= 0 || m_sessionHost->height() <= 0) {
        return;
    }
    constexpr int outerMargin = 12;
    constexpr int maximumPanelWidth = 1040;
    const int availableWidth = m_sessionHost->width() - (outerMargin * 2);
    const int availableHeight = m_sessionHost->height() - (outerMargin * 2);
    // Floating (tool windows over a native document view): host coordinates on screen.
    const QPoint origin = sessionPanelFloats() ? m_sessionHost->mapToGlobal(QPoint(0, 0)) : QPoint(0, 0);
    if (m_reviewVisibilityButton) {
        m_reviewVisibilityButton->adjustSize();
        m_reviewVisibilityButton->move(origin + QPoint(outerMargin, outerMargin));
    }
    const int right = m_sessionHost->width();
    const int panelWidth = qMin(maximumPanelWidth, qMin(availableWidth, right - outerMargin * 2));
    if (panelWidth <= 0 || availableHeight <= 0) {
        m_sessionPage->hide();
        return;
    }
    m_sessionPage->setMinimumWidth(panelWidth);
    m_sessionPage->setMaximumWidth(panelWidth);
    m_sessionPage->layout()->activate();
    const int preferredHeight = m_sessionPage->layout()->hasHeightForWidth()
        ? m_sessionPage->layout()->totalHeightForWidth(panelWidth)
        : m_sessionPage->sizeHint().height();
    const int panelHeight = qMin(preferredHeight, availableHeight);
    const int x = (right - panelWidth) / 2;
    const int y = qMax(outerMargin, m_sessionHost->height() - panelHeight - outerMargin);
    m_sessionPage->setGeometry(origin.x() + x, origin.y() + y, panelWidth, panelHeight);
}

void ReviewOverview::updateReviewVisibilityButton()
{
    if (!m_reviewVisibilityButton) return;
    m_reviewVisibilityButton->setText(m_reviewHidden ? i18n("Focus") : i18n("Hide review"));
    m_reviewVisibilityButton->setToolTip(m_reviewHidden ? i18n("Focus the current review card") : i18n("Hide the review card"));
    m_reviewVisibilityButton->adjustSize();
}

void ReviewOverview::toggleReviewVisibility()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    if (m_reviewHidden) {
        focusReview();
        return;
    }
    m_reviewHidden = true;
    m_lostPresentationCardId.clear();
    m_maskClearer();
    m_sessionPage->hide();
    refreshAnswerFeedback();
    updateReviewVisibilityButton();
}

void ReviewOverview::focusReview()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    QString error;
    // A revealed answer comes back outlined, so present it masked and reveal it again.
    if (!m_cardPresenter(m_dueCards.at(m_currentReview), true, &error)) {
        KMessageBox::error(m_sessionPage, error, i18n("Could Not Focus Review Card"));
        return;
    }
    m_lostPresentationCardId.clear();
    const bool answerVisible = !m_ratingPanel->isHidden();
    if (answerVisible) Q_EMIT answerRevealed();
    m_reviewHidden = false;
    // Hiding the review cleared the mask; restore Flip with it.
    if (m_maskFlipped && !answerVisible) Q_EMIT reviewMaskInvertedChanged(true);
    m_sessionPage->show();
    refreshAnswerFeedback();
    positionSessionPanel();
    m_sessionPage->raise();
    if (m_reviewVisibilityButton) m_reviewVisibilityButton->raise();
    updateReviewVisibilityButton();
}

void ReviewOverview::updateRatingLabels(const ReviewCard &card)
{
    // Merges that arrived through Sync rename tags, so count them again under their new names.
    if (card.tagMerges != m_tagMerges) {
        m_tagMerges = card.tagMerges;
        m_tagUsage.reset();
    }
    // As on the phone, each card starts with the tags of its last answer (none for a card never
    // answered) and twice its last interval (two weeks for a new card); a refresh while it is
    // shown keeps what was chosen.
    m_barTagGroups = m_store.tagGroups();
    if (card.id() != m_barCardId) {
        m_barCardId = card.id();
        m_interval = ReviewInterval::suggested(card.schedule.suggestionReviewedAt, card.schedule.suggestionDueAt);
        // Only slider steps: the card's free tags from before are not carried into the new answer.
        m_selectedTags = ReviewTags::onlySteps(card.currentTags(), m_barTagGroups, card.tagMerges);
        m_groupNotes.clear();
        m_contextDraft = {};
        // A skipped card comes back with what the bar held when it was skipped, until it is
        // answered; a newer answer (another device) makes the draft obsolete.
        const auto draft = m_barDrafts.constFind(card.id());
        if (draft != m_barDrafts.cend() && draft->answeredAt == card.schedule.suggestionReviewedAt) {
            m_interval = draft->interval;
            m_selectedTags = ReviewTags::onlySteps(draft->tags, m_barTagGroups, card.tagMerges);
            m_groupNotes = draft->groupNotes;
            if (draft->context.cardId == card.id()) m_contextDraft = draft->context;
        } else if (draft != m_barDrafts.cend()) {
            m_barDrafts.erase(draft);
            saveBarDrafts();
        }
        // Groups asked before the answer only are not part of the after step: the before panel
        // asks them afresh.
        m_selectedTags.removeIf([this](const QString &tag) { return beforeOnlyTag(tag); });
    }
    const QStringList tags = card.currentTags();
    const ReviewInterval current = ReviewInterval::between(card.schedule.suggestionReviewedAt, card.schedule.suggestionDueAt);
    QStringList parts;
    if (!tags.isEmpty()) parts.append(reviewTagLabels(tags));
    else if (card.wasAnswered()) parts.append(i18n("Not rated"));
    if (current.amount > 0) parts.append(intervalLabel(current));
    m_currentState->setText(parts.isEmpty() ? i18n("Current: new card") : i18n("Current: %1", parts.join(QStringLiteral(" · "))));
    refreshBarLabels();
}

const QHash<QString, int> &ReviewOverview::tagUsage()
{
    if (!m_tagUsage) m_tagUsage = m_store.reviewTagUsage();
    return *m_tagUsage;
}

void ReviewOverview::moveTagUsage(const QStringList &from, const QStringList &to)
{
    if (!m_tagUsage) return; // Counted from the store when next needed.
    for (const QString &tag : from) {
        auto count = m_tagUsage->find(tag);
        if (count != m_tagUsage->end() && *count > 0) --*count;
    }
    for (const QString &tag : to) ++(*m_tagUsage)[tag];
}

void ReviewOverview::updateSessionMessage(const QString &message)
{
    m_sessionComplete = true;
    m_reviewHidden = false;
    m_reviewQuestion->setText(message);
    m_reviewQuestion->setToolTip({});
    m_reviewStatus->hide();
    m_ratingPanel->hide();
    m_doneButton->show();
    refreshCardConversations(true);
    updateAnswerControls();
    updateCardMenu();
    updateSessionExtras();
}

void ReviewOverview::reviewCard(const QString &cardId)
{
    // Only that card is read (its state as Cards shows it).
    const QList<ReviewBrowserCard> cards = m_store.browserCardsOf(m_store.cardsWithIds({cardId}));
    const auto selected = std::find_if(cards.cbegin(), cards.cend(), [&cardId](const ReviewBrowserCard &item) {
        return item.card.id() == cardId && item.canReview();
    });
    if (selected == cards.cend()) {
        KMessageBox::error(this, i18n("This review card is no longer available for review."), i18n("Cannot Review Card"));
        return;
    }
    m_pendingSkip = {};
    m_reviewHidden = false;
    m_lastActionWasSkip = false;
    m_scopedRatingEvents.clear();
    m_retireEvents.clear();
    m_reviewHistory.clear();
    m_tagUsage.reset(); // Counted from the store once per session (see tagUsage()).
    clearRecallPredictions();
    m_singleCardSession = true;
    // Review now keeps the card whatever its kind.
    m_sessionKind.clear();
    m_sessionProjectId.clear();
    m_dueCards = {selected->card};
    m_currentReview = 0;
    m_sessionComplete = false;
    updateEntryButtons();
    showCurrentCard();
    if (m_currentReview >= 0) Q_EMIT reviewStarted();
}

void ReviewOverview::applyDocumentStems(QStringList stems)
{
    const QString previous = m_documentFilter->currentData().toString();
    stems.sort(Qt::CaseInsensitive);
    QStringList listed;
    for (int index = 1; index < m_documentFilter->count(); ++index) listed.append(m_documentFilter->itemData(index).toString());
    if (listed == stems) return;
    m_documentFilter->blockSignals(true);
    m_documentFilter->clear();
    m_documentFilter->addItem(i18n("All documents"), QString());
    for (const QString &stem : std::as_const(stems)) {
        m_documentFilter->addItem(stem, stem);
    }
    const int restored = m_documentFilter->findData(previous);
    if (restored >= 0) {
        m_documentFilter->setCurrentIndex(restored);
    }
    m_documentFilter->blockSignals(false);
}

namespace
{
// The card @p cardId became when it was merged into another (Merge cards, thoughtsctl
// merge-review-cards, or the server's merge): the card its newest deletion names in merged_into,
// followed through later merges to a card that still exists. Empty when it was not merged away.
// The cards of the way are read from @p store as needed, into @p cards.
QString mergedIntoCard(const ReviewStore &store, QList<ReviewCard> *cards, QString cardId)
{
    for (int hop = 0; hop < 8; ++hop) {
        const auto find = [cards, &cardId] {
            return std::find_if(cards->cbegin(), cards->cend(), [&cardId](const ReviewCard &item) { return item.id() == cardId; });
        };
        auto card = find();
        if (card == cards->cend()) {
            *cards += store.cardsWithIds({cardId});
            card = find();
        }
        if (card == cards->cend()) return {};
        if (!card->schedule.deleted) return hop > 0 ? cardId : QString();
        QString next;
        for (auto event = card->history.crbegin(); event != card->history.crend(); ++event) {
            if (event->value(QStringLiteral("rating")).toString() != QLatin1String("delete")) continue;
            next = event->value(QStringLiteral("merged_into")).toString();
            break;
        }
        if (next.isEmpty() || next == cardId) return {};
        cardId = next;
    }
    return {};
}
}

void ReviewOverview::reloadFromProjections()
{
    if (refreshDeferred()) {
        applyDeferredState();
        return;
    }
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        reload();
        return;
    }

    const QString currentCardId = m_dueCards.at(m_currentReview).id();
    const QVariantList placesShown = m_dueCards.at(m_currentReview).regions();
    const QString pdfShown = m_dueCards.at(m_currentReview).pdfSha256();
    const bool answerRevealed = !m_ratingPanel->isHidden();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    // Only the card under review is read here; the rest of the queue, the document filter and the
    // counts come from a background refresh (updateDueSummary). Editing a card's source rebuilds
    // its PDF and re-places the card, which hides it from the due queue until that finishes: it
    // is still the same card and still due, so the review goes on. Review now also shows a card
    // that is not due (as reviewCard() lets it). A card deleted or reviewed elsewhere ends it.
    QList<ReviewCard> read = m_store.cardsWithIds({currentCardId});
    const auto same = std::find_if(read.cbegin(), read.cend(), [&currentCardId](const ReviewCard &card) { return card.id() == currentCardId; });
    const bool reviewable = same != read.cend() && same->isApproved() && !same->schedule.deleted
        && (m_singleCardSession ? !same->schedule.retired : !same->schedule.retained && ReviewStore::isDueNow(*same, now));
    if (!reviewable) {
        if (const QString keptId = mergedIntoCard(m_store, &read, currentCardId); !keptId.isEmpty()) {
            // Merged into another card (Merge cards here, thoughtsctl or another device): the review
            // goes on with the card it became, masked, from its own before step. The picks and note
            // made for this one carry over while its answer was not shown yet.
            const auto kept = std::find_if(read.cbegin(), read.cend(), [&keptId](const ReviewCard &card) { return card.id() == keptId; });
            if (kept != read.cend() && kept->isApproved() && !kept->schedule.retired) {
                QList<ReviewCard> queue = m_singleCardSession ? QList<ReviewCard>{} : m_dueCards.mid(m_currentReview + 1);
                queue.removeIf([&](const ReviewCard &card) { return card.id() == keptId || card.id() == currentCardId; });
                queue.prepend(*kept);
                BeforeStep carried;
                if (m_before.cardId == currentCardId && !m_before.answerShownAt.isValid()) {
                    carried = {keptId, m_before.tags, m_before.note, {}, false};
                    // Kept from now on for the answer the card it became has.
                    carried.answeredAt = kept->schedule.lastReviewedAt;
                }
                m_dueCards = std::move(queue);
                m_currentReview = 0;
                // The merged card is gone: what it kept is carried, or dropped.
                forgetBeforeSteps({currentCardId});
                DeveloperLog::record(QStringLiteral("review.current_card.merged"),
                                     {{QStringLiteral("card_id"), currentCardId}, {QStringLiteral("kept_card_id"), keptId},
                                      {QStringLiteral("before_carried"), !carried.cardId.isEmpty()}});
                showCurrentCard(carried);
                updateDueSummary();
                return;
            }
        }
        // Unavailable or no longer due, unanswered: its before step stays kept for when it comes back.
        if (m_beforeNoteSaveTimer->isActive()) keepBeforeStep();
        DeveloperLog::record(QStringLiteral("review.current_card.left"),
                             {{QStringLiteral("card_id"), currentCardId}, {QStringLiteral("before_kept"), m_before.cardId == currentCardId && m_before.shown
                                                                                                    && (!m_before.tags.isEmpty() || !m_before.note.trimmed().isEmpty())}});
        m_maskClearer();
        m_dueCards.removeIf([&currentCardId](const ReviewCard &card) { return card.id() == currentCardId; });
        m_currentReview = -1;
        updateDueSummary();
        updateSessionMessage(i18n("The current review card changed in another window or during synchronization. Return to Today to start again with the updated queue."));
        return;
    }
    m_dueCards[m_currentReview] = *same;
    const ReviewCard &card = m_dueCards.at(m_currentReview);
    m_reviewQuestion->setText(card.question());
    showAnswerPages(card);
    m_doneButton->hide();
    m_ratingPanel->setVisible(answerRevealed);
    const auto groupsShown = m_barTagGroups;
    updateRatingLabels(card);
    // The before panel follows groups changed elsewhere until it is locked.
    if (m_barTagGroups != groupsShown && !m_before.locked()) rebuildBeforePanel();
    updateAnswerControls();
    // Projection refreshes update content without presenting or moving windows, except to give
    // back a presentation a reload of the PDF took away (the card's places on it came with them),
    // or to cover places the card gained on the PDF shown (another card merged into it).
    updateCardMenu();
    updateSessionExtras();
    updateDueSummary();
    if (!m_lostPresentationCardId.isEmpty()) restoreReviewPresentation(m_lostPresentationCardId);
    else if (card.pdfSha256() == pdfShown && card.regions() != placesShown) restoreReviewPresentation(currentCardId);
}

QString ReviewOverview::currentPdfPath(const ReviewCard &card) const
{
    const QVariantMap document = m_documentProvider();
    const QJsonObject primary = primaryObject(card);
    const QString openPdfPath = document.value(QStringLiteral("pdf_path")).toString();
    if (document.value(QStringLiteral("pdf_stem")).toString() == card.pdfStem()
        && document.value(QStringLiteral("pdf_sha256")).toString() == primary.value(QStringLiteral("pdf_sha256")).toString()
        && QFileInfo::exists(openPdfPath)) {
        return openPdfPath;
    }
    return reviewCardPdfPath(m_docsRoot, card.record);
}

QString ReviewOverview::fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QVariantMap ReviewOverview::locateVisualPage(const QString &pdfPath, const QString &pageVisualSha256) const
{
    return locateReviewPage(pdfPath, pageVisualSha256);
}

void ReviewOverview::refreshDueQueue()
{
    if (refreshDeferred()) {
        applyDeferredState();
        return;
    }
    const QList<ReviewCard> due = m_store.dueCards(QDateTime::currentDateTimeUtc(), m_documentFilter->currentData().toString());
    // During a session the queue keeps to its entry; otherwise it is every due card (the counts).
    m_dueCards = m_currentReview >= 0 && !m_sessionComplete ? entryCards(due) : due;
    updateDueSummary();
    if (m_currentReview < 0 && !m_sessionComplete) {
        m_reviewQuestion->clear();
        m_ratingPanel->hide();
        updateAnswerControls();
        setSessionControlsVisible(false);
    }
}

void ReviewOverview::updateDueSummary()
{
    if (refreshDeferred()) return;
    // During a review the queue still holds the cards answered: the background refresh counts.
    const bool reviewing = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    if (!reviewing) displayQueueCounts(m_dueCards, m_unavailableCount);
    m_documentFilter->setEnabled(true);
    updateEntryButtons();
    requestQueueRefresh();
}

void ReviewOverview::displayQueueCounts(const QList<ReviewCard> &due, int unavailable)
{
    int newCards = 0;
    int learningCards = 0;
    int reviewCards = 0;
    m_studyDue = 0;
    m_projectDue = 0;
    for (const ReviewCard &card : due) {
        ++(card.isProjectCard() ? m_projectDue : m_studyDue);
        if (!card.schedule.reviewed) {
            ++newCards;
        } else if (card.schedule.intervalDays == 0) {
            ++learningCards;
        } else {
            ++reviewCards;
        }
    }
    displayCounts(newCards, learningCards, reviewCards, unavailable);
    saveVerifiedCounts(newCards, learningCards, reviewCards, unavailable);
}

void ReviewOverview::requestQueueRefresh()
{
    // Requests within a moment (Sync's end and its projections, an answer and the reload after
    // it) start one refresh.
    if (m_queueRefreshTimer && !m_queueRefreshTimer->isActive()) m_queueRefreshTimer->start();
}

void ReviewOverview::startQueueRefresh()
{
    // Sync freezes the dashboard; its end refreshes again (setSyncInProgress).
    if (refreshDeferred()) return;
    if (m_queueRefreshRunning) {
        m_queueRefreshPending = true;
        return;
    }
    m_queueRefreshRunning = true;
    QueueRefresh request;
    request.filter = m_documentFilter->currentData().toString();
    if (!m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size()) request.sessionCardId = m_dueCards.at(m_currentReview).id();
    // Providers run on the GUI thread only: the worker reads a snapshot of the projections.
    ReviewStore::ProjectionProvider snapshot;
    if (m_conversationProjections) {
        const QJsonObject projections = m_conversationProjections();
        snapshot = [projections] { return projections; };
    }
    const QString root = m_docsRoot;
    QPointer<ReviewOverview> guard(this);
    QThreadPool::globalInstance()->start([guard, root, snapshot, request]() mutable {
        QElapsedTimer elapsed;
        elapsed.start();
        const ReviewStore store(root, snapshot, {}, true);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QList<ReviewCard> cards = store.cards();
        request.cards = int(cards.size());
        QSet<QString> stems;
        for (const ReviewCard &card : cards) {
            if ((card.status() == QLatin1String("approved") || card.status() == QLatin1String("needs_reanchor")) && !card.schedule.deleted) stems.insert(card.pdfStem());
        }
        request.stems = stems.values();
        request.due = store.dueCardsOf(cards, now, request.filter);
        // The Projects entry lists the Projects that hold an active project card (review-card-kinds.md §7).
        for (const ReviewCard &card : cards) {
            if (!card.isProjectCard() || card.schedule.deleted || card.schedule.retired || card.status() == QLatin1String("candidate")
                || card.status() == QLatin1String("rejected")) continue;
            for (const QString &id : card.kindProjectIds)
                if (!request.projectIds.contains(id)) request.projectIds.append(id);
        }
        for (const ReviewBrowserCard &item : store.browserCardsOf(cards, now)) {
            if ((request.filter.isEmpty() || item.card.pdfStem() == request.filter) && item.hasIssues) ++request.unavailable;
        }
        request.durationMs = elapsed.elapsed();
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, request] {
            if (guard) guard->applyQueueRefresh(request);
        }, Qt::QueuedConnection);
    });
}

void ReviewOverview::applyQueueRefresh(const QueueRefresh &result)
{
    m_queueRefreshRunning = false;
    bool again = std::exchange(m_queueRefreshPending, false);
    DeveloperLog::record(QStringLiteral("review.queue_refresh"), {{QStringLiteral("duration_ms"), result.durationMs}, {QStringLiteral("cards"), result.cards},
                                                                  {QStringLiteral("due"), result.due.size()}, {QStringLiteral("reviewing"), !result.sessionCardId.isEmpty()}});
    // Sync began meanwhile: the counts stay frozen and its end refreshes again.
    if (refreshDeferred()) return;
    applyDocumentStems(result.stems);
    const bool reviewing = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    const QString shown = reviewing ? m_dueCards.at(m_currentReview).id() : QString();
    if (result.filter != m_documentFilter->currentData().toString() || result.sessionCardId != shown) {
        // Made for another document or another card under review (answered, skipped or merged
        // meanwhile): not applied; a new one is.
        again = true;
    } else {
        m_unavailableCount = result.unavailable;
        if (reviewing) {
            // The card under review stays where the bar has it, as it was last read; the queue
            // around it is the new one.
            if (!m_singleCardSession) {
                QList<ReviewCard> queue = entryCards(result.due);
                const auto at = std::find_if(queue.cbegin(), queue.cend(), [&shown](const ReviewCard &card) { return card.id() == shown; });
                const int index = at == queue.cend() ? 0 : int(std::distance(queue.cbegin(), at));
                queue.removeIf([&shown](const ReviewCard &card) { return card.id() == shown; });
                queue.insert(index, m_dueCards.at(m_currentReview));
                m_dueCards = std::move(queue);
                m_currentReview = index;
            }
        } else {
            m_dueCards = result.due;
        }
        m_projectEntryIds = result.projectIds;
        displayQueueCounts(result.due, result.unavailable);
        m_documentFilter->setEnabled(true);
        updateEntryButtons();
    }
    if (again) requestQueueRefresh();
}

void ReviewOverview::displayCounts(int newCards, int learningCards, int reviewCards, int unavailable)
{
    m_countsShown = true;
    m_displayedDue = newCards + learningCards + reviewCards;
    m_newCount->setNum(newCards);
    m_learningCount->setNum(learningCards);
    m_reviewCount->setNum(learningCards + reviewCards);
    m_dueSummary->setText(i18np("One card due", "%1 cards due", newCards + learningCards + reviewCards));
    if (unavailable > 0) {
        const QString heading = i18np("%1 card unavailable", "%1 cards unavailable", unavailable);
        m_attentionSummary->setText(QStringLiteral("<b>%1</b><br>%2").arg(
            heading.toHtmlEscaped(), i18n("Open a card’s details to see why it is unavailable.").toHtmlEscaped()));
        m_attentionButton->setText(i18n("View cards"));
        m_attentionBrowserState = QStringLiteral("unavailable");
        m_attentionPanel->show();
    } else {
        m_attentionBrowserState.clear();
        m_attentionPanel->hide();
    }
}

void ReviewOverview::startReview()
{
    // Library-wide, Start Review is the Study entry; a document's review keeps every card.
    startEntryReview(m_documentFilter->currentData().toString().isEmpty() ? ReviewKinds::knowledge() : QString(), {});
}

QList<ReviewCard> ReviewOverview::entryCards(const QList<ReviewCard> &due) const
{
    if (m_sessionKind.isEmpty() || m_singleCardSession) return due;
    QList<ReviewCard> offered;
    for (const ReviewCard &card : due) {
        if (card.reviewKind != m_sessionKind) continue;
        if (!m_sessionProjectId.isEmpty() && !card.kindProjectIds.contains(m_sessionProjectId)) continue;
        offered.append(card);
    }
    return offered;
}

void ReviewOverview::updateEntryButtons()
{
    const bool libraryWide = m_documentFilter->currentData().toString().isEmpty();
    const bool idle = m_currentReview < 0;
    m_startButton->setText(libraryWide ? i18n("Study · %1 due", m_studyDue) : i18n("Start Review"));
    m_startButton->setToolTip(libraryWide ? i18n("Review the knowledge cards: what you are studying") : QString());
    m_startButton->setEnabled(idle && (libraryWide ? m_studyDue : m_displayedDue) > 0);
    m_startProjectsButton->setText(i18n("Projects · %1 due", m_projectDue));
    // Shown once there is a project card; a document's review keeps every card.
    m_startProjectsButton->setVisible(libraryWide && (m_projectDue > 0 || !m_projectEntryIds.isEmpty()));
    m_startProjectsButton->setEnabled(idle && m_projectDue > 0);
}

void ReviewOverview::fillProjectsMenu()
{
    m_projectsMenu->clear();
    // Due per Project, from the due cards of the whole Library.
    QHash<QString, int> due;
    if (m_currentReview < 0) {
        for (const ReviewCard &card : std::as_const(m_dueCards)) {
            if (!card.isProjectCard()) continue;
            for (const QString &id : card.kindProjectIds) ++due[id];
        }
    }
    QStringList ids = m_projectEntryIds;
    for (auto it = due.cbegin(); it != due.cend(); ++it)
        if (!ids.contains(it.key())) ids.append(it.key());
    auto *all = m_projectsMenu->addAction(i18n("All Projects · %1 due", m_projectDue));
    all->setObjectName(QStringLiteral("reviewProjectsAllAction"));
    all->setEnabled(m_projectDue > 0);
    connect(all, &QAction::triggered, this, [this] { startEntryReview(ReviewKinds::project(), {}); });
    if (!ids.isEmpty()) m_projectsMenu->addSeparator();
    const QJsonArray catalogue = ReviewKinds::catalogue(m_docsRoot);
    QList<QPair<QString, QString>> named;
    for (const QString &id : std::as_const(ids)) {
        const QString name = ReviewKinds::catalogueProject(catalogue, id).value(QStringLiteral("name")).toString();
        named.append({name.isEmpty() ? id : name, id});
    }
    std::sort(named.begin(), named.end(), [](const auto &a, const auto &b) { return QString::localeAwareCompare(a.first, b.first) < 0; });
    for (const auto &[name, id] : std::as_const(named)) {
        auto *entry = m_projectsMenu->addAction(i18n("%1 · %2 due", name, due.value(id)));
        entry->setObjectName(QStringLiteral("reviewProjectsProjectAction"));
        entry->setData(id);
        entry->setEnabled(due.value(id) > 0);
        connect(entry, &QAction::triggered, this, [this, id] { startEntryReview(ReviewKinds::project(), id); });
    }
}

void ReviewOverview::startEntryReview(const QString &kind, const QString &projectId)
{
    m_pendingSkip = {};
    m_reviewHidden = false;
    m_lastActionWasSkip = false;
    m_scopedRatingEvents.clear();
    m_retireEvents.clear();
    m_singleCardSession = false;
    m_reviewHistory.clear();
    m_tagUsage.reset(); // Counted from the store once per session (see tagUsage()).
    clearRecallPredictions();
    m_sessionComplete = false;
    m_sessionKind = kind;
    m_sessionProjectId = projectId;
    if (refreshDeferred()) {
        // During Sync the counts stay frozen, but the queue follows the chosen document.
        m_dueCards = m_store.dueCards(QDateTime::currentDateTimeUtc(), m_documentFilter->currentData().toString());
    } else {
        refreshDueQueue();
    }
    // A session started from one entry offers only that entry's cards.
    m_dueCards = entryCards(m_dueCards);
    if (m_dueCards.isEmpty()) {
        updateEntryButtons();
        return;
    }
    m_currentReview = 0;
    updateEntryButtons();
    showCurrentCard();
    if (m_currentReview >= 0) Q_EMIT reviewStarted();
}

bool ReviewOverview::ensureCurrentGeometry(ReviewCard *card, QString *error)
{
    const QString currentHash = fileSha256(currentPdfPath(*card));
    if (currentHash.isEmpty()) {
        *error = i18n("The source PDF is unavailable: %1", currentPdfPath(*card));
        return false;
    }
    if (currentHash == card->pdfSha256() && !card->regions().isEmpty()) {
        return true;
    }
    // A card made in an HTML attachment has no place on the PDF: its page finds it by its text.
    if (Thoughts::isHtmlAttachmentCard(card->record)) {
        return true;
    }
    if (primaryObject(*card).contains(QStringLiteral("regions"))) {
        *error = i18n("The answer locations need to be updated for this PDF. Sync to retrieve the current locations.");
        return false;
    }
    const QVariantMap location = locateVisualPage(currentPdfPath(*card), card->pageVisualSha256());
    const QString locationError = location.value(QStringLiteral("error")).toString();
    if (!locationError.isEmpty()) {
        m_store.markNeedsReanchor(card->id());
        *error = locationError;
        return false;
    }
    if (!m_store.updateCardLocation(card->id(), currentHash, location.value(QStringLiteral("page")).toInt(), error)) {
        m_store.markNeedsReanchor(card->id());
        return false;
    }
    const QList<ReviewCard> reloaded = m_store.cardsWithIds({card->id()});
    for (const ReviewCard &candidate : reloaded) {
        if (candidate.id() == card->id()) {
            *card = candidate;
            break;
        }
    }
    return true;
}

void ReviewOverview::showCurrentCard()
{
    showCurrentCard(BeforeStep{});
}

void ReviewOverview::showCurrentCard(const BeforeStep &kept)
{
    // The before step on screen is kept before another one (or the same card's, read back) replaces it.
    if (m_beforeNoteSaveTimer->isActive()) keepBeforeStep();
    closeTagsDialog();
    m_lostPresentationCardId.clear();
    m_answerNotFoundCardId.clear();
    m_maskClearer();
    QStringList skippedCards;
    while (m_currentReview >= 0 && m_currentReview < m_dueCards.size()) {
        ReviewCard &card = m_dueCards[m_currentReview];
        QString error;
        if (!ensureCurrentGeometry(&card, &error) || !m_cardPresenter(card, !m_reviewHidden, &error)) {
            skippedCards.append(i18n("%1 page %2: %3").arg(card.pdfStem()).arg(card.page()).arg(error));
            ++m_currentReview;
            continue;
        }
        m_reviewQuestion->setText(card.question());
        m_sessionComplete = false;
        showAnswerPages(card);
        m_doneButton->hide();
        // Every card starts with only the answer covered, and its own tag and interval.
        setMaskFlipped(false);
        m_ratingPanel->hide();
        if (m_reviewHidden) m_maskClearer();
        m_barCardId.clear();
        m_answerArea = 0;
        updateRatingLabels(card);
        // The before step starts empty for every card shown: nothing carries over into a prediction,
        // except the step of this very card: @p kept (shown again after Edit, or made of the card
        // under review by a merge), else the one kept on disk since its last answer (the card left
        // the review before it was answered: unavailable for a while, skipped, the review ended).
        m_before = kept.cardId == card.id() ? kept : keptBeforeStep(card);
        {
            const QSignalBlocker blocker(m_beforeNote);
            m_beforeNote->setPlainText(m_before.note);
        }
        rebuildBeforePanel();
        // Kept locked (its answer had been shown): the after step starts where Reveal started it.
        if (m_before.locked()) startAfterStepAtPrediction();
        requestRecallPrediction(card);
        // A new card: its conversation line may appear or go once the bar shows (below).
        m_conversationCardId.clear();
        updateAnswerControls();
        setSessionControlsVisible(true);
        return;
    }
    m_currentReview = -1;
    refreshDueQueue();
    if (skippedCards.isEmpty()) {
        updateSessionMessage(i18n("Review complete."));
    } else {
        updateSessionMessage(i18np("The due card could not be displayed: %2", "%1 due cards could not be displayed: %2", skippedCards.size(), skippedCards.join(QStringLiteral("\n"))));
    }
    setSessionControlsVisible(true);
}

// As on the phone, only an answer spread over several pages says where it is: in the question's
// tooltip, so the bar keeps its line under the question for the before step's picks.
void ReviewOverview::showAnswerPages(const ReviewCard &card)
{
    const QString pages = card.pagesLabel();
    m_reviewQuestion->setToolTip(pages.contains(QLatin1Char(',')) ? i18n("Answer on pages %1", pages) : QString());
    m_reviewStatus->clear();
    m_reviewStatus->hide();
}

void ReviewOverview::setMaskFlipped(bool flipped)
{
    m_maskFlipped = flipped;
    Q_EMIT reviewMaskInvertedChanged(flipped);
}

// The card menu's Flip (Unflip once flipped): offered only while the answer is covered.
void ReviewOverview::toggleMaskFlipped()
{
    if (m_sessionComplete || m_currentReview < 0 || m_currentReview >= m_dueCards.size() || !m_ratingPanel->isHidden()) return;
    setMaskFlipped(!m_maskFlipped);
    updateAnswerControls();
    updateCardMenu();
}

// The phone's states: the question (Reveal, with Flip in the card menu); and the answer shown,
// where the row gives way to the current tags and interval and the Tag | Interval | Next bar.
void ReviewOverview::updateAnswerControls()
{
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    const bool knowledge = current && !m_ratingPanel->isHidden();
    m_answerRow->setVisible(current && !knowledge);
    m_revealButton->setEnabled(current);
    m_currentState->setVisible(knowledge);
    m_tagField->setEnabled(knowledge);
    m_intervalField->setEnabled(knowledge);
    // Next records an answer with no tag too.
    m_nextButton->setEnabled(knowledge);
    const int areas = current && m_answerAreaCount ? m_answerAreaCount() : 0;
    m_previousAreaButton->setVisible(areas >= 2);
    m_nextAreaButton->setVisible(areas >= 2);
    // An HTML attachment card whose text is no longer in its page: the stored answer, once revealed.
    if (current && !m_answerNotFoundCardId.isEmpty() && m_dueCards.at(m_currentReview).id() == m_answerNotFoundCardId) {
        const QJsonObject primary = primaryObject(m_dueCards.at(m_currentReview));
        QString answer = primary.value(QStringLiteral("selected_text")).toString();
        if (answer.isEmpty()) answer = primary.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString();
        m_reviewStatus->setText(knowledge ? i18n("Answer not found in the current page. Stored answer:\n%1", answer) : i18n("Answer not found in the current page."));
        m_reviewStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_reviewStatus->show();
    }
    updateCardKindLine();
    updateBeforePanel();
    refreshAnswerFeedback();
    positionSessionPanel();
}

void ReviewOverview::showAnswerNotFound(const QString &cardId)
{
    if (m_sessionComplete || m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_dueCards.at(m_currentReview).id() != cardId) return;
    m_answerNotFoundCardId = cardId;
    updateAnswerControls();
}

QString ReviewOverview::answerNotFoundCardId() const
{
    return m_answerNotFoundCardId;
}

void ReviewOverview::setAnswerFeedbackStates(std::function<QVariantMap()> states)
{
    m_answerFeedbackStates = std::move(states);
    refreshAnswerFeedback();
}

QString ReviewOverview::maskedHistoryCard() const
{
    return m_maskedHistoryCard;
}

void ReviewOverview::setPresentationRestorer(std::function<bool(const ReviewCard &, QString *)> restorer)
{
    m_presentationRestorer = std::move(restorer);
}

void ReviewOverview::restoreReviewPresentation(const QString &lostCardId)
{
    const QString cardId = lostCardId; // may be m_lostPresentationCardId itself
    m_lostPresentationCardId.clear();
    // Hiding the review, or another card, presents (or clears) the mask by itself.
    if (m_sessionComplete || m_reviewHidden || m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    ReviewCard &card = m_dueCards[m_currentReview];
    if (cardId.isEmpty() || card.id() != cardId) return;
    QString error;
    const bool placed = ensureCurrentGeometry(&card, &error);
    if (!placed || !(m_presentationRestorer ? m_presentationRestorer(card, &error) : m_cardPresenter(card, true, &error))) {
        // Not placed on the rebuilt PDF yet: tried again when Sync's projections bring its places.
        m_lostPresentationCardId = cardId;
        return;
    }
    // As the bar shows it: revealed, else masked, flipped or not.
    if (!m_ratingPanel->isHidden()) Q_EMIT answerRevealed();
    else if (m_maskFlipped) Q_EMIT reviewMaskInvertedChanged(true);
    updateAnswerControls();
}

void ReviewOverview::setContextOutline(std::function<QVariantList(const ReviewCard &)> outline)
{
    m_contextOutline = std::move(outline);
    m_contextDraft = {};
}

ReviewContextNotes::Draft &ReviewOverview::contextDraft(const ReviewCard &card)
{
    if (m_contextDraft.cardId != card.id()) {
        const auto outline = m_contextOutline ? ReviewContextNotes::outlineFromVariant(m_contextOutline(card)) : QList<ReviewContextNotes::OutlineItem>();
        m_contextDraft = m_store.contextDraft(card, outline, reviewCardDocumentName(m_docsRoot, card));
    }
    return m_contextDraft;
}

void ReviewOverview::convertWholeDocumentCards()
{
    if (std::exchange(m_wholeDocumentCardsConverted, true)) return;
    QStringList errors;
    const auto converted = m_store.convertWholeDocumentCards([this](const ReviewCard &card) { return reviewCardDocumentName(m_docsRoot, card); }, &errors);
    for (const auto &card : converted) {
        qInfo() << "Converted the whole-document review card" << card.cardId << "of document" << card.documentKey << "into a document note";
        DeveloperLog::record(QStringLiteral("review.whole_document_card.converted"),
                             {{QStringLiteral("card_id"), card.cardId}, {QStringLiteral("note_event_id"), card.noteEventId},
                              {QStringLiteral("delete_event_id"), card.deleteEventId}});
    }
    for (const QString &error : std::as_const(errors)) qWarning() << "Whole-document review card not converted:" << error;
    // A failed card is tried again the next time.
    if (!errors.isEmpty()) m_wholeDocumentCardsConverted = false;
}

QVariantMap ReviewOverview::answerFeedbackStates() const
{
    return m_answerFeedbackStates ? m_answerFeedbackStates() : QVariantMap();
}

QList<QJsonObject> ReviewOverview::sessionFeedback(const ReviewCard &card, bool includeDeleted, const QVariantMap &states) const
{
    // The card's own records, and the other cards' ones the reader found in its answer (shared:
    // the reader lists another card's record only where its passage lies in this answer).
    QList<QJsonObject> result;
    for (const QJsonObject &record : m_store.cardFeedback({}, true)) {
        const bool deleted = record.value(QStringLiteral("deleted")).toBool();
        if (!ReviewCardFeedback::isShared(record, card.id())) {
            if (includeDeleted || !deleted) result.append(record);
        } else if (!deleted && states.contains(record.value(QStringLiteral("feedback_id")).toString())) {
            result.append(record);
        }
    }
    return result;
}

QJsonObject ReviewOverview::feedbackHomeCards(const ReviewCard &card)
{
    // Only for feedback of its document: the cards are read when other cards' feedback is there.
    const QString documentId = DocumentIdentityStore(m_docsRoot).byFilename(card.pdfStem() + QStringLiteral(".pdf")).value(QStringLiteral("document_id")).toString();
    m_homeCardsCardId = card.id();
    m_homeCards = m_store.feedbackHomeCards(card.id(), ReviewCardFeedback::documentOf(card.record, {documentId}).ids);
    QJsonObject result;
    for (auto it = m_homeCards.cbegin(); it != m_homeCards.cend(); ++it) result.insert(it.key(), it.value());
    return result;
}

QHash<QString, QJsonObject> ReviewOverview::sessionHomeCards(const ReviewCard &card, const QList<QJsonObject> &feedback)
{
    // Read only when shared feedback is shown, and then once per card presented.
    if (std::none_of(feedback.cbegin(), feedback.cend(), [&card](const QJsonObject &record) { return ReviewCardFeedback::isShared(record, card.id()); })) return {};
    if (m_homeCardsCardId != card.id()) {
        m_homeCardsCardId = card.id();
        m_homeCards = m_store.feedbackHomeCards(card.id());
    }
    return m_homeCards;
}

QSet<QString> ReviewOverview::orphanedFeedback() const
{
    QSet<QString> result;
    const QVariantMap states = m_answerFeedbackStates ? m_answerFeedbackStates() : QVariantMap();
    for (auto it = states.cbegin(); it != states.cend(); ++it) {
        if (it.value().toString() == QLatin1String("orphaned")) result.insert(it.key());
    }
    return result;
}

void ReviewOverview::refreshAnswerFeedback()
{
    if (!m_feedbackHistoryAction) return;
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    m_feedbackHistoryAction->setVisible(current);
    const bool answerVisible = current && !m_ratingPanel->isHidden();
    const QString masked = current && !answerVisible ? m_dueCards.at(m_currentReview).id() : QString();
    if (std::exchange(m_maskedHistoryCard, masked) != masked) Q_EMIT maskedHistoryCardChanged(masked);
}

void ReviewOverview::setAnswerAreaNavigation(std::function<int()> count, std::function<bool(int)> focus)
{
    m_answerAreaCount = std::move(count);
    m_focusAnswerArea = std::move(focus);
}

// The card opens on its first answer area; the arrows cycle from there.
void ReviewOverview::stepAnswerArea(int step)
{
    const int areas = m_answerAreaCount ? m_answerAreaCount() : 0;
    if (areas < 2 || !m_focusAnswerArea) return;
    m_answerArea = ((m_answerArea + step) % areas + areas) % areas;
    m_focusAnswerArea(m_answerArea);
}

// Hide again covers the answer again, keeping Flip, the tag and the interval.
void ReviewOverview::hideAnswer()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    QString error;
    if (!m_cardPresenter(m_dueCards.at(m_currentReview), !m_reviewHidden, &error)) {
        KMessageBox::error(m_sessionPage, error, i18n("Could Not Hide the Answer"));
        return;
    }
    m_lostPresentationCardId.clear();
    m_ratingPanel->hide();
    if (m_maskFlipped) Q_EMIT reviewMaskInvertedChanged(true);
    updateAnswerControls();
    updateCardMenu();
    updateSessionExtras();
}

// Reveal: frame the answer and choose its tags and when to review it again.
void ReviewOverview::revealAnswer()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        return;
    }
    lockBeforeStep();
    Q_EMIT answerRevealed();
    m_ratingPanel->show();
    updateAnswerControls();
    updateCardMenu();
    updateSessionExtras();
}

void ReviewOverview::rebuildBeforePanel()
{
    if (!m_beforeGroups) return;
    // A menu of the old list may still be handling the choice that led here.
    if (auto *old = m_beforeGroups->findChild<QWidget *>(QStringLiteral("reviewBeforeGroupList"), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->setObjectName(QString());
        old->deleteLater();
    }
    // Picks of groups no longer asked before (changed here or by Sync) are not kept.
    m_before.tags.removeIf([this](const QString &tag) {
        return std::none_of(m_barTagGroups.cbegin(), m_barTagGroups.cend(), [&tag](const ReviewTags::TagGroup &group) {
            return ReviewTags::askedBefore(group) && group.scale.contains(tag);
        });
    });
    auto *list = new QWidget(m_beforeGroups);
    list->setObjectName(QStringLiteral("reviewBeforeGroupList"));
    auto *column = new QVBoxLayout(list);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    // As in the tag sheet, groups are sliders; a group's menu chooses when it is asked.
    int asked = 0;
    for (const ReviewTags::TagGroup &group : std::as_const(m_barTagGroups)) {
        if (!ReviewTags::askedBefore(group) || group.scale.isEmpty()) continue;
        ++asked;
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        auto *name = new QLabel(group.name, list);
        name->setObjectName(QStringLiteral("reviewBeforeGroupName"));
        name->setTextFormat(Qt::PlainText);
        name->setProperty("reviewTagGroup", group.id);
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        row->addWidget(name, 1);
        auto *menuButton = new QToolButton(list);
        menuButton->setObjectName(QStringLiteral("reviewBeforeGroupMenu"));
        menuButton->setProperty("reviewTagGroup", group.id);
        menuButton->setText(QStringLiteral("⋯"));
        menuButton->setAutoRaise(true);
        menuButton->setToolTip(i18n("When %1 is asked", group.name));
        menuButton->setAccessibleName(menuButton->toolTip());
        menuButton->setPopupMode(QToolButton::InstantPopup);
        menuButton->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 0px 4px; } QToolButton::menu-indicator { image: none; width: 0px; }"));
        auto *menu = new QMenu(menuButton);
        const QString id = group.id;
        addPhaseMenu(menu, group.phase, [this, id](const QString &phase) {
            QList<ReviewTags::TagGroup> groups = m_store.tagGroups();
            QString error;
            if (!ReviewTags::setPhase(groups, id, phase, &error) || !m_store.saveTagGroups(groups, &error)) {
                KMessageBox::error(m_sessionPage, error.isEmpty() ? i18n("The tag groups could not be saved.") : error, i18n("Tag Groups"));
                return;
            }
            m_barTagGroups = m_store.tagGroups();
            rebuildBeforePanel();
            positionSessionPanel();
        });
        menuButton->setMenu(menu);
        row->addWidget(menuButton, 0, Qt::AlignVCenter);
        column->addLayout(row);
        QStringList labels;
        for (const QString &step : group.scale) labels.append(reviewTagLabel(step));
        auto *scale = new ReviewTagScale(group.name, labels, list);
        scale->setObjectName(QStringLiteral("reviewBeforeTagScale"));
        scale->setProperty("reviewTagGroup", group.id);
        scale->setProperty("reviewTagSteps", group.scale);
        QList<int> chosen;
        for (int index = 0; index < group.scale.size(); ++index) {
            if (m_before.tags.contains(group.scale.at(index))) chosen.append(index);
        }
        scale->setSelection(chosen);
        const QStringList steps = group.scale;
        scale->onChanged = [this, steps](int value) {
            if (m_before.locked()) return;
            for (const QString &step : steps) m_before.tags.removeAll(step);
            if (value >= 0) m_before.tags.append(steps.at(value));
            // In group order (Feeling, then Recall), as the phone records them.
            QStringList ordered;
            for (const ReviewTags::TagGroup &shown : std::as_const(m_barTagGroups)) {
                for (const QString &step : shown.scale) {
                    if (m_before.tags.contains(step) && !ordered.contains(step)) ordered.append(step);
                }
            }
            m_before.tags = ordered;
            keepBeforeStep();
        };
        column->addWidget(scale);
    }
    m_beforeGroups->layout()->addWidget(list);
    // A layout shows a widget added to a visible window only later; show the new list at once.
    list->show();
    // Without a group asked before, there is no before step: no panel, an answer as before.
    if (asked > 0 && !m_before.locked()) m_before.shown = true;
    updateBeforePanel();
    keepBeforeStep();
}

void ReviewOverview::updateBeforePanel()
{
    if (!m_beforePanel) return;
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size()
        && m_dueCards.at(m_currentReview).id() == m_before.cardId && m_before.shown;
    m_beforePanel->setVisible(current);
    if (!current) return;
    const bool locked = m_before.locked();
    m_beforeGroups->setVisible(!locked);
    m_beforeNote->setVisible(!locked);
    const QString note = m_before.note.simplified();
    // Locked: "Recall: Partly recalled · Feeling: OK · Setup: Mac — note…", each group with the
    // step picked, in the groups' order ("Before: not rated" without a pick).
    QString summary;
    QString whole;
    if (locked) {
        QStringList picked;
        QStringList named;
        for (const ReviewTags::TagGroup &group : std::as_const(m_barTagGroups)) {
            for (const QString &step : group.scale) {
                if (!m_before.tags.contains(step) || named.contains(step)) continue;
                named.append(step);
                picked.append(i18nc("@info a group asked before the answer and the step picked in it", "%1: %2", group.name,
                                    ReviewTags::shortLabel(reviewTagLabel(step))));
                break;
            }
        }
        // A pick whose group is gone (the layout changed since) is still named, alone.
        for (const QString &tag : std::as_const(m_before.tags)) {
            if (!named.contains(tag)) picked.append(ReviewTags::shortLabel(reviewTagLabel(tag)));
        }
        summary = picked.isEmpty() ? i18n("Before: not rated") : picked.join(QStringLiteral(" · "));
        whole = note.isEmpty() ? summary : summary + QStringLiteral(" — ") + note;
        if (!note.isEmpty()) summary += QStringLiteral(" — ") + (note.size() > 60 ? note.left(59).trimmed() + QStringLiteral("…") : note);
    }
    m_beforeSummaryText = summary;
    m_beforeSummary->setToolTip(whole);
    m_beforeSummaryRow->setVisible(!summary.isEmpty());
    elideBeforeSummary();
    // The agent's prediction shows only once the answer is shown, after the person's own pick.
    QString prediction;
    const auto found = m_predictions.constFind(m_before.cardId);
    if (locked && found != m_predictions.cend()) {
        if (!found->result.isEmpty()) {
            const QString tag = reviewTagLabel(found->result.value(QStringLiteral("tag")).toString());
            const QString reason = found->result.value(QStringLiteral("reason")).toString().simplified();
            prediction = reason.isEmpty() ? i18n("Agent predicted: %1", tag) : i18n("Agent predicted: %1 — %2", tag, reason);
        } else if (found->pending) {
            prediction = i18n("Agent: predicting…");
        }
    }
    m_predictionLine->setText(prediction);
    m_predictionLine->setVisible(!prediction.isEmpty());
}

void ReviewOverview::elideBeforeSummary()
{
    if (!m_beforeSummary) return;
    // Before the first layout the row has no width yet: the whole line, elided once it is placed.
    const int available = m_beforeSummaryRow->width() - m_beforeEditButton->width() - m_beforeSummaryRow->layout()->spacing();
    const QString text = m_beforeSummaryRow->width() > 0 && available > 0
        ? m_beforeSummary->fontMetrics().elidedText(m_beforeSummaryText, Qt::ElideRight, available)
        : m_beforeSummaryText;
    if (m_beforeSummary->text() != text) m_beforeSummary->setText(text);
}

void ReviewOverview::lockBeforeStep()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    if (m_before.cardId != m_dueCards.at(m_currentReview).id() || !m_before.shown || m_before.locked()) return;
    // Shown again after the pencil, the answer keeps the time it was first shown (as on the phone):
    // the picks may have changed since, the moment the answer was seen did not.
    if (!m_before.answerShownAt.isValid()) m_before.answerShownAt = QDateTime::currentDateTimeUtc();
    m_before.reopened = false;
    keepBeforeStep();
    startAfterStepAtPrediction();
}

void ReviewOverview::startAfterStepAtPrediction()
{
    // The after step asks each group asked before and after again, from the step predicted;
    // without a prediction it starts as the bar has it.
    for (const ReviewTags::TagGroup &group : std::as_const(m_barTagGroups)) {
        if (group.phase != ReviewTags::beforeAndAfterPhase()) continue;
        const auto predicted = std::find_if(group.scale.cbegin(), group.scale.cend(), [this](const QString &step) { return m_before.tags.contains(step); });
        if (predicted == group.scale.cend()) continue;
        m_selectedTags.removeIf([this, &group](const QString &tag) { return group.scale.contains(ReviewTags::resolve(tag, m_tagMerges)); });
        m_selectedTags.append(*predicted);
    }
    refreshBarLabels();
}

// The phone's "Before: … · Tap to change" (its card menu's "Back to before step"), with the same
// data: the answer is covered again, the rows open on the picks and the note, and Reveal locks
// the changed picks; the answer records them with the time it was first shown, and nothing marks
// the change.
void ReviewOverview::reopenBeforeStep()
{
    if (m_sessionComplete || m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    if (m_before.cardId != m_dueCards.at(m_currentReview).id() || !m_before.shown || !m_before.locked()) return;
    if (!m_ratingPanel->isHidden()) {
        hideAnswer();
        // The reader could not cover it again: the picks stay as they were.
        if (!m_ratingPanel->isHidden()) return;
    }
    m_before.reopened = true;
    {
        const QSignalBlocker blocker(m_beforeNote);
        m_beforeNote->setPlainText(m_before.note);
    }
    rebuildBeforePanel();
    updateAnswerControls();
    // The pencil is gone with the line: the keyboard goes on in the first row.
    if (auto *list = m_beforeGroups->findChild<QWidget *>(QStringLiteral("reviewBeforeGroupList"), Qt::FindDirectChildrenOnly)) {
        if (auto *first = list->findChild<QWidget *>(QStringLiteral("reviewBeforeTagScale"))) first->setFocus(Qt::OtherFocusReason);
    }
}

bool ReviewOverview::beforeOnlyTag(const QString &tag) const
{
    const QString owner = ReviewTags::groupOf(m_barTagGroups, ReviewTags::resolve(tag, m_tagMerges));
    return !owner.isEmpty() && std::any_of(m_barTagGroups.cbegin(), m_barTagGroups.cend(), [&owner](const ReviewTags::TagGroup &group) {
        return group.id == owner && !ReviewTags::askedAfter(group);
    });
}

QStringList ReviewOverview::answerTags(const QStringList &afterTags) const
{
    // The tags in effect: the before-only groups' (Feeling) and the after step's, the actual
    // recall among them; the predicted recall is not one.
    QStringList tags;
    for (const QString &tag : m_before.tags) {
        if (beforeOnlyTag(tag) && !tags.contains(tag)) tags.append(tag);
    }
    for (const QString &tag : afterTags) {
        if (!beforeOnlyTag(tag) && !tags.contains(tag)) tags.append(tag);
    }
    return tags;
}

QString ReviewOverview::reviewEntry() const
{
    return m_singleCardSession ? ReviewEntry::reviewNow() : ReviewEntry::dueQueue();
}

QJsonObject ReviewOverview::answerPhases(const QStringList &afterTags) const
{
    QStringList after;
    for (const QString &tag : afterTags) {
        if (!beforeOnlyTag(tag) && !after.contains(tag)) after.append(tag);
    }
    // The agent's prediction, when it arrived before the answer was saved.
    QJsonObject prediction;
    const auto found = m_predictions.constFind(m_before.cardId);
    if (found != m_predictions.cend() && !found->result.isEmpty()) {
        const QJsonObject &result = found->result;
        const QDateTime madeAt = historyTimestamp(result.value(QStringLiteral("made_at")).toString());
        const QDateTime arrived = madeAt.isValid() ? madeAt : found->arrivedAt;
        prediction = {{QStringLiteral("tag"), result.value(QStringLiteral("tag")).toString()},
                      {QStringLiteral("group_id"), found->groupId},
                      {QStringLiteral("reason"), result.value(QStringLiteral("reason")).toString()},
                      {QStringLiteral("model"), result.value(QStringLiteral("model")).toString()},
                      {QStringLiteral("prediction_id"), result.value(QStringLiteral("prediction_id")).toString()},
                      {QStringLiteral("requested_at"), found->requestedAt.toUTC().toString(Qt::ISODateWithMs)},
                      {QStringLiteral("made_at"), madeAt.isValid() ? result.value(QStringLiteral("made_at")).toString()
                                                                    : found->arrivedAt.toUTC().toString(Qt::ISODateWithMs)},
                      {QStringLiteral("arrived_before_answer"), !m_before.answerShownAt.isValid() || arrived <= m_before.answerShownAt}};
    }
    return ReviewPhases::toField(m_before.tags, after, m_before.answerShownAt, prediction);
}

void ReviewOverview::clearRecallPredictions()
{
    // An aborted reply finishes at once and finds no prediction to fill.
    const auto predictions = std::exchange(m_predictions, {});
    for (const RecallPrediction &prediction : predictions) {
        if (prediction.reply) prediction.reply->abort();
    }
}

void ReviewOverview::requestRecallPrediction(const ReviewCard &card)
{
    if (m_predictions.contains(card.id())) return;
    const ReviewTags::TagGroup *recall = ReviewTags::recallGroup(m_barTagGroups);
    if (!recall) return;
    const RecallPredictor service = recallPredictor();
    if (service.url.isEmpty()) return;
    RecallPrediction &prediction = m_predictions[card.id()];
    prediction.groupId = recall->id;
    prediction.scale = recall->scale;
    prediction.requestedAt = QDateTime::currentDateTimeUtc();
    prediction.pending = true;
    // The request reads the card's answer and its document's cards from disk: it is put together
    // off the GUI thread, from a snapshot of the projections (providers run on the GUI thread only).
    ReviewStore::ProjectionProvider provider;
    if (m_conversationProjections) {
        const QJsonObject snapshot = m_conversationProjections();
        provider = [snapshot] { return snapshot; };
    }
    const QString root = m_docsRoot;
    const QString pdfPath = currentPdfPath(card);
    const ReviewTags::TagGroup group = *recall;
    const QDateTime requestedAt = prediction.requestedAt;
    QPointer<ReviewOverview> guard(this);
    QThreadPool::globalInstance()->start([guard, root, provider, card, pdfPath, group, requestedAt, service] {
        ReviewStore store(root, provider, {}, true);
        const QJsonObject body = recallPredictionRequest(card, store.cards(nullptr, card.pdfStem()), recallAnswerText(root, card, pdfPath),
                                                         reviewCardDocumentName(root, card), group, requestedAt);
        QMetaObject::invokeMethod(guard.data(), [guard, cardId = card.id(), body, requestedAt, service] {
            if (!guard) return;
            const auto found = guard->m_predictions.find(cardId);
            // Another due session began meanwhile.
            if (found == guard->m_predictions.end() || found->requestedAt != requestedAt) return;
            if (!guard->m_predictionNetwork) guard->m_predictionNetwork = new QNetworkAccessManager(guard.data());
            QNetworkRequest request(service.url);
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            request.setRawHeader("X-Recall-Token", service.token);
            // The token is this service's: a redirect to another origin is not followed.
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
            request.setTransferTimeout(60000);
            QNetworkReply *reply = guard->m_predictionNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
            found->reply = reply;
            QObject::connect(reply, &QNetworkReply::finished, guard.data(), [self = guard.data(), reply, cardId, requestedAt] {
                reply->deleteLater();
                const auto prediction = self->m_predictions.find(cardId);
                if (prediction == self->m_predictions.end() || prediction->requestedAt != requestedAt) return;
                prediction->reply = nullptr;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (reply->error() == QNetworkReply::NoError && status == 200) {
                    self->applyRecallPrediction(cardId, QJsonDocument::fromJson(reply->readAll()).object());
                    return;
                }
                // Shows and records nothing.
                qWarning() << "No recall prediction for review card" << cardId << "- HTTP status" << status << reply->error();
                prediction->pending = false;
                if (self->m_before.cardId == cardId) {
                    self->updateBeforePanel();
                    self->positionSessionPanel();
                }
            });
        }, Qt::QueuedConnection);
    });
}

void ReviewOverview::applyRecallPrediction(const QString &cardId, const QJsonObject &response)
{
    const auto found = m_predictions.find(cardId);
    if (found == m_predictions.end()) return;
    found->pending = false;
    // Only a step of the recall group is a prediction; anything else shows and records nothing.
    if (!found->scale.contains(response.value(QStringLiteral("tag")).toString())) {
        qWarning() << "The recall prediction for review card" << cardId << "is not a step of the recall group";
    } else {
        found->result = response;
        found->arrivedAt = QDateTime::currentDateTimeUtc();
    }
    if (m_before.cardId == cardId) {
        updateBeforePanel();
        positionSessionPanel();
    }
}

void ReviewOverview::rateCurrent(const QString &rating, const QDateTime &scheduledDueAt, const QDateTime &reviewedAt, const QStringList &tags,
                                 const QHash<QString, QString> &groupNotes, const QHash<QString, ReviewContextNotes::Note> &contextNotes)
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        return;
    }
    QString error;
    const QString cardId = m_dueCards.at(m_currentReview).id();
    const QStringList previousTags = m_dueCards.at(m_currentReview).currentTags();
    const QJsonObject record = m_dueCards.at(m_currentReview).record;
    QString eventId;
    // An answer of the card the before panel was shown for records both steps; @p tags are the after step's.
    const bool twoSteps = rating != QLatin1String("delete") && m_before.cardId == cardId && m_before.shown;
    const QStringList recordedTags = twoSteps ? answerTags(tags) : tags;
    QHash<QString, QString> notes = groupNotes;
    if (twoSteps && !m_before.note.trimmed().isEmpty()) notes.insert(ReviewTags::beforeNoteKey(), m_before.note);
    if (!m_store.recordRating(cardId, rating, reviewedAt, &error, scheduledDueAt, &eventId, {}, recordedTags, notes, contextNotes, &record,
                              twoSteps ? answerPhases(tags) : QJsonObject(), reviewEntry())) {
        KMessageBox::error(this, error, i18n("Could Not Record Review"));
        return;
    }
    // Answered (or deleted): the before step kept for the card is done with.
    forgetBeforeSteps({cardId});
    if (rating == QLatin1String("delete")) {
        m_reviewHistory.removeIf([&cardId](const auto &entry) { return entry.first == cardId; });
        moveTagUsage(previousTags, {});
    } else {
        m_reviewHistory.append({cardId, eventId});
        m_lastActionWasSkip = false;
        moveTagUsage(previousTags, countedReviewTags(recordedTags, m_tagMerges));
    }
    ++m_currentReview;
    showCurrentCard();
    Q_EMIT reviewRecorded();
}

void ReviewOverview::rateRetained()
{
    rateCurrent(QStringLiteral("retained"));
}

void ReviewOverview::refreshBarLabels()
{
    if (!m_tagField) return;
    const auto setValue = [](QPushButton *field, const QString &value) {
        field->findChild<QLabel *>(QStringLiteral("reviewFieldValue"))->setText(value);
    };
    // As on the phone: up to two names, slider steps first (in group order), then "+N".
    QStringList ordered;
    for (const auto &group : std::as_const(m_barTagGroups)) {
        for (const QString &step : group.scale) {
            if (m_selectedTags.contains(step) && !ordered.contains(step)) ordered.append(step);
        }
    }
    for (const QString &selected : std::as_const(m_selectedTags)) {
        if (!ordered.contains(selected)) ordered.append(selected);
    }
    QString tag = i18n("Not rated");
    if (!ordered.isEmpty()) {
        QStringList names;
        for (const QString &name : ordered.mid(0, 2)) names.append(ReviewTags::shortLabel(reviewTagLabel(name)));
        tag = names.join(QStringLiteral(" · "));
        if (ordered.size() > 2) tag += QStringLiteral(" +%1").arg(ordered.size() - 2);
    }
    setValue(m_tagField, tag);
    m_tagField->setAccessibleName(i18n("Tag: %1. Choose tags", m_selectedTags.isEmpty() ? tag : reviewTagLabels(m_selectedTags)));
    m_tagField->setToolTip(m_tagField->accessibleName());
    QString amount = QLocale().toString(m_interval.amount, 'f', 6);
    while (amount.endsWith(QLatin1Char('0'))) amount.chop(1);
    if (amount.endsWith(QLocale().decimalPoint())) amount.chop(QLocale().decimalPoint().size());
    const QString interval = m_interval.date.isValid() ? QLocale().toString(m_interval.date, QStringLiteral("d MMM"))
        : m_interval.unit == ReviewInterval::Minutes ? i18n("%1 min", amount)
        : m_interval.unit == ReviewInterval::Hours ? i18n("%1 h", amount)
        : intervalLabel(m_interval);
    setValue(m_intervalField, interval);
    // A project card comes back within a week whatever the interval (review-card-kinds.md §6.2).
    const bool project = m_currentReview >= 0 && m_currentReview < m_dueCards.size() && m_dueCards.at(m_currentReview).isProjectCard();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime chosenDue = m_interval.dueAt(now);
    const bool capped = project && (!chosenDue.isValid() || chosenDue > now.addDays(ReviewKinds::projectIntervalDays()));
    const QString again = capped ? i18n("7 days (a project card; %1 as a knowledge card)", intervalLabel(m_interval)) : intervalLabel(m_interval);
    m_intervalField->setAccessibleName(i18n("Again in %1. Change the interval", again));
    m_intervalField->setToolTip(m_intervalField->accessibleName());
    const QString answerTags = m_selectedTags.isEmpty() ? i18n("Not rated") : reviewTagLabels(m_selectedTags);
    m_nextButton->setAccessibleName(i18n("%1, again in %2", answerTags, again));
    m_nextButton->setToolTip(i18n("%1, again in %2\nRight-click for Never, or to answer for a whole document or project.",
                                  answerTags, again));
    positionSessionPanel();
}

// The phone's tag sheet: any tags, grouped as everywhere else; closing keeps the choice.
// Not modal, so the document behind it can still be scrolled; it closes when the card changes.
void ReviewOverview::chooseTags()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_ratingPanel->isHidden()) return;
    if (m_tagsDialog) {
        m_tagsDialog->raise();
        m_tagsDialog->activateWindow();
        return;
    }
    const auto card = m_dueCards.at(m_currentReview);
    const QString cardId = card.id();
    auto *dialog = new QDialog(m_sessionPage);
    m_tagsDialog = dialog;
    dialog->setWindowModality(Qt::NonModal);
    dialog->setObjectName(QStringLiteral("reviewTagsDialog"));
    dialog->setWindowTitle(i18n("Rating"));
    dialog->resize(420, 260);
    auto *layout = new QVBoxLayout(dialog);
    auto *picker = new ReviewTagPicker(ReviewTags::byPopularity(tagUsage(), createdReviewTags(card.tagMerges), card.tagMerges), m_selectedTags, card.tagMerges, &m_store, dialog,
                                       m_groupNotes, tagUsage(), card.currentTags(), &contextDraft(card));
    picker->onChanged = [this, picker, cardId] {
        if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_dueCards.at(m_currentReview).id() != cardId) return;
        m_selectedTags = picker->selectedTags();
        m_groupNotes = picker->groupNotes();
        refreshBarLabels();
    };
    layout->addWidget(picker);
    layout->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::accept);
    layout->addWidget(buttons);
    // Waits like exec() for the window to close, but leaves the reader usable meanwhile.
    QPointer<ReviewOverview> self(this);
    QPointer<QDialog> guard(dialog);
    QEventLoop loop;
    connect(dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    connect(dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
    dialog->show();
    loop.exec(QEventLoop::DialogExec);
    delete guard.data();
    if (!self) return;
    // The groups may have changed in the picker (a slider made or taken away).
    m_barTagGroups = m_store.tagGroups();
    refreshBarLabels();
}

void ReviewOverview::closeTagsDialog()
{
    if (m_tagsDialog) m_tagsDialog->close();
}

// Changes only this card's interval; the next card starts from its own suggestion.
void ReviewOverview::chooseInterval()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_ratingPanel->isHidden()) return;
    ReviewInterval interval = m_interval;
    if (!chooseReviewInterval(m_sessionPage, &interval)) return;
    m_interval = interval;
    refreshBarLabels();
}

void ReviewOverview::recordNext()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_ratingPanel->isHidden()) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto due = m_interval.dueAt(now);
    if (!due.isValid()) {
        chooseInterval();
        return;
    }
    const QString cardId = m_dueCards.at(m_currentReview).id();
    rateCurrent(QStringLiteral("custom_interval"), due, now, m_selectedTags, m_groupNotes,
                m_contextDraft.cardId == cardId ? m_contextDraft.changes() : QHash<QString, ReviewContextNotes::Note>());
}

void ReviewOverview::chooseAnswer(const QStringList &selectedTags, bool requireRevealed)
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || (requireRevealed && m_ratingPanel->isHidden())) return;
    const auto card = m_dueCards.at(m_currentReview);
    const QString id = card.id();
    ReviewInterval interval = m_interval;
    ReviewSkipScope scope = ReviewSkipScope::Card;
    bool dueOnly = true;
    bool never = false;
    if (selectedTags == QStringList{ReviewTags::skip()}) {
        // Skip postpones briefly by default, as on the phone, not by the bar's interval.
        interval = defaultSkipInterval();
        if (!chooseReviewInterval(m_sessionPage, &interval, &scope, card.pdfStem(), m_store.projectsByPdfStem().value(card.pdfStem()), &dueOnly)
            || m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_dueCards.at(m_currentReview).id() != id)
            return;
        skipCurrent(interval, scope, dueOnly);
        return;
    }
    QStringList tags = selectedTags;
    QHash<QString, QString> groupNotes = m_groupNotes;
    // Edited on a copy: cancelling the dialog leaves the card's notes as they were.
    ReviewContextNotes::Draft context = contextDraft(card);
    const QStringList available = ReviewTags::byPopularity(tagUsage(), createdReviewTags(card.tagMerges), card.tagMerges);
    if (!chooseReviewInterval(m_sessionPage, &interval, &scope, card.pdfStem(), m_store.projectsByPdfStem().value(card.pdfStem()),
                              &dueOnly, &never, {}, &tags, available, card.tagMerges, &m_store, &groupNotes, tagUsage(), card.currentTags(), &context)
        || m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_dueCards.at(m_currentReview).id() != id
        || (requireRevealed && m_ratingPanel->isHidden()))
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    const QString rating = never ? QStringLiteral("retained") : QStringLiteral("custom_interval");
    const QDateTime due = never ? QDateTime{} : interval.dueAt(now);
    if (scope == ReviewSkipScope::Card) rateCurrent(rating, due, now, tags, groupNotes, context.changes());
    else rateScoped(rating, due, tags, scope, dueOnly, groupNotes, context.changes());
}

void ReviewOverview::chooseSkip()
{
    // The card menu can skip before the answer is revealed.
    chooseAnswer({ReviewTags::skip()}, false);
}

ReviewInterval ReviewOverview::defaultSkipInterval()
{
    return savedSkipInterval();
}

void ReviewOverview::skipWithDefaults()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    skipCurrent(defaultSkipInterval(), ReviewSkipScope::Card, true);
}

void ReviewOverview::skipCurrent(const ReviewInterval &interval, ReviewSkipScope scope, bool dueOnly)
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto &skipped = m_dueCards.at(m_currentReview);
    const QString id = skipped.id();
    if (m_barCardId == id)
        m_barDrafts.insert(id, {m_selectedTags, m_interval, m_groupNotes, skipped.schedule.suggestionReviewedAt,
                                m_contextDraft.cardId == id && !m_contextDraft.changes().isEmpty() ? m_contextDraft : ReviewContextNotes::Draft(), now});
    QString error;
    ReviewSkipBatch batch;
    if (!m_store.skip(id, scope, interval.dueAt(now), &batch, &error, now, dueOnly)) {
        m_barDrafts.remove(id);
        KMessageBox::error(m_sessionPage, error, i18n("Could Not Skip Cards"));
        return;
    }
    saveBarDrafts();
    m_pendingSkip = batch;
    m_lastActionWasSkip = true;
    m_skippedCardId = id;
    m_skippedSingleCardSession = m_singleCardSession;
    auto remaining = m_dueCards.mid(m_currentReview + 1);
    remaining.removeIf([&](const auto &card) { return batch.cardIds.contains(card.id()); });
    m_dueCards = std::move(remaining);
    m_currentReview = 0;
    showCurrentCard();
    Q_EMIT reviewRecorded();
}

void ReviewOverview::rateScoped(const QString &rating, const QDateTime &scheduledDueAt, const QStringList &tags, ReviewSkipScope scope, bool dueOnly,
                                const QHash<QString, QString> &groupNotes, const QHash<QString, ReviewContextNotes::Note> &contextNotes)
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    const auto now = QDateTime::currentDateTimeUtc();
    const QString currentId = m_dueCards.at(m_currentReview).id();
    const auto snapshot = m_store.cards();
    const auto current = std::find_if(snapshot.cbegin(), snapshot.cend(), [&currentId](const ReviewCard &card) { return card.id() == currentId; });
    if (current == snapshot.cend() || !current->isApproved() || current->schedule.deleted) return;
    const auto projects = m_store.projectsByPdfStem();
    const QString project = projects.value(current->pdfStem());
    if (scope == ReviewSkipScope::Project && project.isEmpty()) {
        KMessageBox::error(m_sessionPage, i18n("Project information is unavailable for this document."), i18n("Could Not Record Reviews"));
        return;
    }
    QList<QString> targets{currentId};
    QList<QStringList> previousTags{current->currentTags()};
    QHash<QString, QJsonObject> records{{currentId, current->record}};
    for (const auto &card : snapshot) {
        if (card.id() == currentId || !card.isApproved() || card.schedule.deleted || card.schedule.retained || card.schedule.retired) continue;
        if (dueOnly && card.schedule.reviewed && card.schedule.dueAt > now) continue;
        if (scope == ReviewSkipScope::Document && card.pdfStem() != current->pdfStem()) continue;
        if (scope == ReviewSkipScope::Project && projects.value(card.pdfStem()) != project) continue;
        targets.append(card.id());
        previousTags.append(card.currentTags());
        records.insert(card.id(), card.record);
    }
    QList<QPair<QString, QString>> recorded;
    QString error;
    // The answer's tags in effect (the before-only groups' too); its two steps, before note and
    // agent prediction are only the current card's, the one the before panel was shown for.
    const bool twoSteps = m_before.cardId == currentId && m_before.shown;
    const QStringList recordedTags = twoSteps ? answerTags(tags) : tags;
    for (const QString &id : std::as_const(targets)) {
        QString eventId;
        // The section, page and document notes are this card's; the others only share the answer.
        const QJsonObject record = records.value(id);
        QHash<QString, QString> notes = groupNotes;
        if (twoSteps && id == currentId && !m_before.note.trimmed().isEmpty()) notes.insert(ReviewTags::beforeNoteKey(), m_before.note);
        if (!m_store.recordRating(id, rating, now, &error, scheduledDueAt, &eventId, {}, recordedTags, notes,
                                  id == currentId ? contextNotes : QHash<QString, ReviewContextNotes::Note>(), &record,
                                  twoSteps && id == currentId ? answerPhases(tags) : QJsonObject(), reviewEntry())) {
            int rollbackFailures = 0;
            for (const auto &entry : std::as_const(recorded)) {
                QString retractError;
                if (!m_store.retractRating(entry.first, entry.second, now, &retractError)) ++rollbackFailures;
            }
            if (rollbackFailures) error += QLatin1Char('\n') + i18np("One card could not be rolled back. Check its history.",
                                                                       "%1 cards could not be rolled back. Check their history.", rollbackFailures);
            KMessageBox::error(m_sessionPage, error, i18n("Could Not Record Reviews"));
            return;
        }
        recorded.append({id, eventId});
        m_barDrafts.remove(id);
    }
    saveBarDrafts();
    forgetBeforeSteps(targets);
    // Every card it was given to now has these tags, as ReviewStore::reviewTagUsage() counts them.
    const QStringList answered = countedReviewTags(recordedTags, m_tagMerges);
    for (const QStringList &previous : std::as_const(previousTags)) moveTagUsage(previous, answered);
    Q_EMIT reviewRecorded();
    m_reviewHistory.append(recorded.constFirst());
    m_scopedRatingEvents.insert(recorded.constFirst().second, recorded);
    m_lastActionWasSkip = false;
    // Scoped cards leave the queue with the current one.
    QSet<QString> done;
    for (const auto &entry : std::as_const(recorded)) done.insert(entry.first);
    auto remaining = m_dueCards.mid(m_currentReview + 1);
    remaining.removeIf([&done](const auto &card) { return done.contains(card.id()); });
    m_dueCards = std::move(remaining);
    m_currentReview = 0;
    showCurrentCard();
}

void ReviewOverview::undoSkip()
{
    if (m_pendingSkip.id.isEmpty()) return;
    QString error;
    if (!m_store.undoSkip(m_pendingSkip.id, &error)) {
        KMessageBox::error(m_sessionPage, error, i18n("Could Not Undo Skip"));
        return;
    }
    const QString id = m_skippedCardId;
    m_pendingSkip = {};
    m_lastActionWasSkip = false;
    m_singleCardSession = m_skippedSingleCardSession;
    m_dueCards = entryCards(m_store.dueCards(QDateTime::currentDateTimeUtc(), m_documentFilter->currentData().toString()));
    const auto cards = m_store.cardsWithIds({id});
    const auto selected = std::find_if(cards.cbegin(), cards.cend(), [&](const auto &card) { return card.id() == id; });
    if (selected != cards.cend() && selected->isApproved() && !selected->schedule.deleted && !selected->schedule.retained && !selected->schedule.retired
        && (!selected->skippedUntil.isValid() || selected->skippedUntil <= QDateTime::currentDateTimeUtc())) {
        m_dueCards.removeIf([&](const auto &card) { return card.id() == id; });
        if (m_singleCardSession) m_dueCards.clear();
        m_dueCards.prepend(*selected);
    }
    m_currentReview = 0;
    m_sessionComplete = false;
    showCurrentCard();
}

void ReviewOverview::undoPrevious()
{
    if (m_lastActionWasSkip && !m_pendingSkip.id.isEmpty()) {
        undoSkip();
    } else if (!m_reviewHistory.isEmpty()) {
        previousQuestion();
    } else if (!m_pendingSkip.id.isEmpty()) {
        undoSkip();
    }
}

void ReviewOverview::updateSessionExtras()
{
    positionSessionPanel();
}

void ReviewOverview::deleteCard()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        return;
    }
    const QString cardId = m_dueCards.at(m_currentReview).id();
    if (KMessageBox::questionTwoActions(m_sessionPage, deleteCardQuestion(),
                                       i18n("Delete Review Card"), KGuiItem(i18n("Delete"), QStringLiteral("edit-delete")), KStandardGuiItem::cancel())
        == KMessageBox::PrimaryAction && m_currentReview >= 0 && m_currentReview < m_dueCards.size() && m_dueCards.at(m_currentReview).id() == cardId) {
        rateCurrent(QStringLiteral("delete"));
    }
}

void ReviewOverview::retireCard()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    const QString cardId = m_dueCards.at(m_currentReview).id();
    QString error;
    QString eventId;
    if (!m_store.retireCard(cardId, &error, QDateTime::currentDateTimeUtc(), &eventId)) {
        KMessageBox::error(m_sessionPage, error, i18n("Could Not Retire Card"));
        return;
    }
    // Like an answer for Undo: the card leaves the session and Undo brings it back (undo_retire).
    m_reviewHistory.append({cardId, eventId});
    m_retireEvents.insert(eventId);
    m_lastActionWasSkip = false;
    ++m_currentReview;
    showCurrentCard();
    Q_EMIT reviewRecorded();
}

void ReviewOverview::setCurrentCardKind(const QString &kind)
{
    if (m_sessionComplete || m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    const QString cardId = m_dueCards.at(m_currentReview).id();
    QString error;
    if (!m_store.setCardKind(cardId, kind, &error)) {
        KMessageBox::error(m_sessionPage, error, i18n("Cannot Change the Card's Kind"));
        return;
    }
    // The card stays under review with its new kind (its mark and due time); the queue around it
    // follows at the next refresh.
    for (const ReviewCard &read : m_store.cardsWithIds({cardId})) {
        if (read.id() != cardId) continue;
        ReviewCard &shown = m_dueCards[m_currentReview];
        shown.reviewKind = read.reviewKind;
        shown.reviewKindSource = read.reviewKindSource;
        shown.kindProjectIds = read.kindProjectIds;
        shown.projectsKind = read.projectsKind;
        shown.passageChanged = read.passageChanged;
        shown.schedule.dueAt = read.schedule.dueAt;
        shown.schedule.retained = read.schedule.retained;
        shown.history = read.history;
    }
    updateCardKindLine();
    refreshBarLabels();
    Q_EMIT reviewRecorded();
}

void ReviewOverview::updateCardKindLine()
{
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    const bool project = current && m_dueCards.at(m_currentReview).isProjectCard();
    QStringList parts;
    if (project) parts.append(i18n("Project card"));
    if (project && m_dueCards.at(m_currentReview).passageChanged) parts.append(passageChangedText());
    m_cardKindLine->setText(parts.join(QStringLiteral(" · ")));
    m_cardKindLine->setToolTip(project ? i18n("A card about work in progress: it comes back at least once a week, and at once when its passage changes.") : QString());
    m_cardKindLine->setVisible(project);
}

void ReviewOverview::updateCardMenu()
{
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    auto *menu = m_cardMenuButton->menu();
    for (const auto &name : {"reviewFocusAction", "reviewMoreInfoAction", "reviewCopyForAgentAction", "reviewEditAction", "reviewRetireAction",
                              "reviewDeleteAction", "reviewSkipAction"}) {
        menu->findChild<QAction *>(QString::fromLatin1(name))->setEnabled(current);
    }
    menu->findChild<QAction *>(QStringLiteral("reviewUndoAction"))->setEnabled(!m_reviewHistory.isEmpty() || !m_pendingSkip.id.isEmpty());
    const bool answerVisible = current && !m_ratingPanel->isHidden();
    // Like the phone, Skip is offered before the answer is revealed too.
    menu->findChild<QAction *>(QStringLiteral("reviewSkipAction"))->setVisible(current);
    auto *flip = menu->findChild<QAction *>(QStringLiteral("reviewFlipAction"));
    flip->setText(m_maskFlipped ? i18n("Unflip") : i18n("Flip"));
    flip->setVisible(current && !answerVisible);
    menu->findChild<QAction *>(QStringLiteral("reviewHideAgainAction"))->setVisible(answerVisible);
    {
        auto *kind = menu->findChild<QAction *>(QStringLiteral("reviewKindAction"));
        auto *follow = menu->findChild<QAction *>(QStringLiteral("reviewFollowProjectAction"));
        const auto choices = current ? cardKindChoices(m_dueCards.at(m_currentReview)) : QList<QPair<QString, QString>>{};
        kind->setVisible(current);
        if (current) {
            kind->setText(choices.first().first);
            kind->setData(choices.first().second);
        }
        follow->setVisible(choices.size() > 1);
        if (choices.size() > 1) follow->setText(choices.at(1).first);
    }
    m_cardMenuButton->setVisible(current || !m_reviewHistory.isEmpty() || !m_pendingSkip.id.isEmpty());
}

namespace
{
// "just now", "5 min ago", "3 h ago", else the date: how long ago a conversation was active.
QString conversationAge(const QDateTime &active, const QDateTime &now)
{
    if (!active.isValid()) return QString();
    const qint64 minutes = std::max<qint64>(0, active.secsTo(now) / 60);
    if (minutes < 1) return i18n("just now");
    if (minutes < 60) return i18np("1 min ago", "%1 min ago", minutes);
    if (minutes < 24 * 60) return i18np("1 h ago", "%1 h ago", minutes / 60);
    return QLocale().toString(active.toLocalTime().date(), QStringLiteral("d MMM"));
}

QDateTime conversationTime(const QJsonObject &object)
{
    return QDateTime::fromString(object.value(QStringLiteral("last_active_at")).toString(), Qt::ISODateWithMs);
}

QIcon conversationIcon(const QJsonObject &record)
{
    // The Library rows' symbols.
    return QIcon::fromTheme(record.value(QStringLiteral("agent")).toString() == QLatin1String("codex") ? QStringLiteral("utilities-terminal") : QStringLiteral("im-user"));
}

QString conversationAgentName(const QJsonObject &record)
{
    return record.value(QStringLiteral("agent")).toString() == QLatin1String("codex") ? i18n("Codex") : i18n("Claude");
}

QString noLinkMessage(const QJsonObject &record)
{
    return record.value(QStringLiteral("agent")).toString() == QLatin1String("codex") ? i18n("No link: this Codex thread is not on this computer.")
                                                                                       : i18n("No link: this Claude session was not started with Remote Control.");
}

// The running Claude Code session of @p conversationId: "busy" or "idle", or "" when none runs here.
QString claudeSessionStatus(const QString &conversationId)
{
#ifdef Q_OS_WIN
    Q_UNUSED(conversationId)
    return QString();
#else
    const QString home = qEnvironmentVariableIsEmpty("CLAUDE_CONFIG_DIR") ? QDir::home().filePath(QStringLiteral(".claude")) : qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    const QDir sessions(QDir(home).filePath(QStringLiteral("sessions")));
    for (const QFileInfo &info : sessions.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(info.filePath());
        if (info.size() > 256 * 1024 || !file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
        if (session.value(QStringLiteral("sessionId")).toString() != conversationId) continue;
        const qint64 pid = session.value(QStringLiteral("pid")).toInteger();
        if (pid <= 1 || (::kill(pid_t(pid), 0) != 0 && errno != EPERM)) continue;
        const QString status = session.value(QStringLiteral("status")).toString();
        return status == QLatin1String("busy") ? status : QStringLiteral("idle");
    }
    return QString();
#endif
}
}

void ReviewOverview::refreshAgentConversations(const QJsonObject &projections)
{
    refreshCardConversations(false, projections);
}

void ReviewOverview::refreshCardConversations(bool cardChanged, const QJsonObject &projections)
{
    if (!m_conversationLine) return;
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    const QString cardId = current ? m_dueCards.at(m_currentReview).id().toLower() : QString();
    const QList<AgentConversations::CardConversation> conversations = cardId.isEmpty() ? QList<AgentConversations::CardConversation>()
        : !projections.isEmpty()                                                    ? AgentConversations::recordsForCard(projections, cardId)
        : m_conversationProjections                                                 ? AgentConversations::recordsForCard(m_conversationProjections(), cardId)
                                                                                    : QList<AgentConversations::CardConversation>();
    const bool sameCard = cardId == m_conversationCardId;
    const bool settingKnowledge = current && !m_ratingPanel->isHidden();
    // While the knowledge is being set, the line neither appears nor goes (no buttons move
    // under the pointer); it only follows the newest conversation if it shows.
    if (!cardChanged && sameCard && settingKnowledge && (conversations.isEmpty() || m_conversationLine->isHidden())) return;
    m_conversationCardId = cardId;
    if (conversations.isEmpty()) {
        m_conversationShown = {};
        m_conversationLine->hide();
        m_conversationStatusTimer->stop();
        return;
    }
    const QJsonObject record = conversations.constFirst().record;
    const QJsonObject entry = conversations.constFirst().entry;
    m_conversationShown = record;
    const QIcon icon = conversationIcon(record);
    m_conversationIcon->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(16, 16));
    m_conversationIcon->setVisible(!icon.isNull());
    QStringList facts;
    if (icon.isNull()) facts.append(conversationAgentName(record));
    if (const QString session = record.value(QStringLiteral("session_name")).toString(); !session.isEmpty()) facts.append(session);
    const QDateTime active = conversationTime(entry);
    facts.append(conversationAge(active, QDateTime::currentDateTimeUtc()));
    m_conversationDetails->setText(QStringLiteral("· ") + facts.join(QStringLiteral(" · ")));
    const QString url = record.value(QStringLiteral("conversation_url")).toString();
    const QString title = record.value(QStringLiteral("title")).toString();
    m_conversationLine->setToolTip(title + QLatin1Char('\n') + conversationAgentName(record)
                                   + (record.value(QStringLiteral("session_name")).toString().isEmpty() ? QString() : QStringLiteral(" · ") + record.value(QStringLiteral("session_name")).toString())
                                   + QStringLiteral(" · ") + i18n("last active %1", QLocale().toString(active.toLocalTime(), QLocale::ShortFormat)) + QLatin1Char('\n')
                                   + (url.isEmpty() ? noLinkMessage(record) : url));
    m_conversationLine->setAccessibleName(i18n("Agent conversation: %1", title));
    m_conversationLine->show();
    elideConversationTitle();
    m_conversationStatusTimer->start();
    updateConversationStatus();
}

void ReviewOverview::elideConversationTitle()
{
    if (!m_conversationLine || m_conversationShown.isEmpty()) return;
    const QString title = m_conversationShown.value(QStringLiteral("title")).toString();
    const QMargins margins = m_conversationLine->layout()->contentsMargins();
    const int spacing = m_conversationLine->layout()->spacing();
    int used = margins.left() + margins.right() + m_conversationDetails->sizeHint().width() + spacing;
    if (m_conversationIcon->isVisibleTo(m_conversationLine)) used += 16 + spacing;
    if (m_conversationStatus->isVisibleTo(m_conversationLine)) used += m_conversationStatus->width() + spacing;
    const int available = std::max(24, m_conversationLine->width() - used);
    m_conversationTitle->setText(m_conversationTitle->fontMetrics().elidedText(title, Qt::ElideMiddle, available));
}

void ReviewOverview::updateConversationStatus()
{
    // Only while the line shows: the timer stops when it goes (or the card bar does).
    if (!m_conversationLine || m_conversationShown.isEmpty() || m_conversationLine->isHidden() || !m_sessionPage || !m_sessionPage->isVisible()) {
        if (m_conversationStatusTimer) m_conversationStatusTimer->stop();
        return;
    }
    const bool claude = m_conversationShown.value(QStringLiteral("agent")).toString() == QLatin1String("claude");
    const QString status = claude ? claudeSessionStatus(m_conversationShown.value(QStringLiteral("conversation_id")).toString()) : QString();
    const bool wasVisible = m_conversationStatus->isVisibleTo(m_conversationLine);
    m_conversationStatus->setVisible(!status.isEmpty());
    if (!status.isEmpty()) {
        const QColor color = status == QLatin1String("busy") ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid);
        m_conversationStatus->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
        m_conversationStatus->setToolTip(status == QLatin1String("busy") ? i18n("working") : i18n("open"));
        m_conversationStatus->setProperty("conversationStatus", status == QLatin1String("busy") ? QStringLiteral("working") : QStringLiteral("open"));
    } else {
        m_conversationStatus->setProperty("conversationStatus", QString());
    }
    if (wasVisible != !status.isEmpty()) elideConversationTitle();
}

void ReviewOverview::openCardConversation(const QJsonObject &record)
{
    if (record.isEmpty()) return;
    const QString url = record.value(QStringLiteral("conversation_url")).toString();
    if (url.isEmpty()) {
        // Nothing to open: the session's name finds it in Claude (or the thread id in Codex).
        const QString session = record.value(QStringLiteral("session_name")).toString();
        QGuiApplication::clipboard()->setText(session.isEmpty() ? record.value(QStringLiteral("conversation_id")).toString() : session);
        QToolTip::showText(QCursor::pos(), noLinkMessage(record) + QLatin1Char('\n') + i18n("Copied its name."), m_conversationLine);
        return;
    }
    if (!Thoughts::openAgentConversationUrl(url)) {
        // The Library rows' messages.
        QToolTip::showText(QCursor::pos(), url.startsWith(QLatin1String("codex:")) ? i18n("Could not open the conversation in Codex. Check that Codex is installed.")
                                                                                   : i18n("Could not open the conversation."),
                           m_conversationLine);
    }
}

void ReviewOverview::fillAgentConversationsMenu()
{
    if (!m_agentConversationsMenu) return;
    m_agentConversationsMenu->clear();
    const bool current = !m_sessionComplete && m_currentReview >= 0 && m_currentReview < m_dueCards.size();
    const QString cardId = current ? m_dueCards.at(m_currentReview).id().toLower() : QString();
    const QList<AgentConversations::CardConversation> conversations = cardId.isEmpty() || !m_conversationProjections
        ? QList<AgentConversations::CardConversation>()
        : AgentConversations::recordsForCard(m_conversationProjections(), cardId);
    m_agentConversationsMenu->setTitle(i18n("Agent conversations (%1)", conversations.size()));
    m_agentConversationsMenu->menuAction()->setEnabled(!conversations.isEmpty());
    for (const auto &conversation : conversations) {
        const QJsonObject record = conversation.record;
        const bool codex = record.value(QStringLiteral("agent")).toString() == QLatin1String("codex");
        const QString url = record.value(QStringLiteral("conversation_url")).toString();
        QStringList facts{conversationAgentName(record)};
        if (const QString session = record.value(QStringLiteral("session_name")).toString(); !session.isEmpty()) facts.append(session);
        facts.append(QLocale().toString(conversationTime(conversation.entry).toLocalTime(), QStringLiteral("d MMM HH:mm")));
        QString title = record.value(QStringLiteral("title")).toString();
        if (title.size() > 80) title = title.left(79) + QChar(0x2026);
        title.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *entry = m_agentConversationsMenu->addMenu(conversationIcon(record), title + QStringLiteral(" \u2014 ") + facts.join(QStringLiteral(" · ")));
        entry->menuAction()->setObjectName(QStringLiteral("reviewAgentConversationEntry"));
        entry->menuAction()->setData(record.value(QStringLiteral("link_id")).toString());
        entry->setToolTipsVisible(true);
        auto *open = entry->addAction(codex ? i18n("Open in Codex") : i18n("Open in Claude"), this, [this, record] { openCardConversation(record); });
        open->setObjectName(QStringLiteral("reviewOpenAgentConversationAction"));
        open->setEnabled(!url.isEmpty());
        if (url.isEmpty()) open->setToolTip(noLinkMessage(record));
        auto *copyLink = entry->addAction(i18n("Copy Link"), this, [url] { QGuiApplication::clipboard()->setText(url); });
        copyLink->setObjectName(QStringLiteral("reviewCopyAgentConversationLinkAction"));
        copyLink->setEnabled(!url.isEmpty());
        entry->addAction(i18n("Copy Conversation ID"), this, [record] { QGuiApplication::clipboard()->setText(record.value(QStringLiteral("conversation_id")).toString()); })
            ->setObjectName(QStringLiteral("reviewCopyAgentConversationIdAction"));
        entry->addSeparator();
        auto *remove = entry->addAction(i18n("Remove from this Card"), this, [this, record, cardId] {
            const QJsonObject next = AgentConversations::removeFromCard(record, cardId, Thoughts::Platform::desktopOriginId());
            if (next == record) return;
            Q_EMIT agentConversationChangeRequested(next.toVariantMap());
            // The user's own removal: the line follows at once, also while the knowledge is set.
            m_conversationCardId.clear();
        });
        remove->setObjectName(QStringLiteral("reviewRemoveAgentConversationAction"));
    }
}

// What the Tag | Interval | Next bar holds for @p card, as the agent texts show it: the tags by
// group, the interval and the notes typed for this answer. Empty unless the bar is showing for it.
QString ReviewOverview::currentReviewText(const ReviewCard &card) const
{
    if (!m_ratingPanel || m_ratingPanel->isHidden() || m_barCardId != card.id()) return {};
    QStringList lines{QStringLiteral("Current review (being given now, not saved yet):")};
    if (m_before.cardId == card.id() && m_before.shown) {
        QStringList before;
        for (const QString &tag : m_before.tags) before.append(reviewTagLabel(tag));
        QString line = QStringLiteral("- Before seeing the answer: %1").arg(before.isEmpty() ? QStringLiteral("no tags") : before.join(QStringLiteral(", ")));
        if (const QString note = m_before.note.simplified(); !note.isEmpty()) line += QStringLiteral(" (note: %1)").arg(note);
        lines.append(line);
    }
    QStringList listed;
    for (const auto &group : std::as_const(m_barTagGroups)) {
        QStringList chosen;
        for (const QString &tag : group.scale + group.tags) {
            if (m_selectedTags.contains(tag) && !listed.contains(tag)) {
                chosen.append(reviewTagLabel(tag));
                listed.append(tag);
            }
        }
        if (!chosen.isEmpty()) lines.append(QStringLiteral("- %1: %2").arg(group.name, chosen.join(QStringLiteral(", "))));
    }
    QStringList other;
    for (const QString &tag : std::as_const(m_selectedTags)) {
        if (!listed.contains(tag)) other.append(reviewTagLabel(tag));
    }
    if (!other.isEmpty()) lines.append(QStringLiteral("- %1: %2").arg(ReviewTags::otherGroupName(), other.join(QStringLiteral(", "))));
    if (m_selectedTags.isEmpty()) lines.append(QStringLiteral("- Tags: none"));
    lines.append(m_interval.date.isValid() ? QStringLiteral("- Again on: %1").arg(intervalLabel(m_interval))
                                           : QStringLiteral("- Again in: %1").arg(intervalLabel(m_interval)));
    const QStringList notes = ReviewTags::groupNoteLines(ReviewTags::cleanGroupNotes(m_groupNotes), m_barTagGroups);
    if (!notes.isEmpty()) {
        lines.append(QStringLiteral("- Notes:"));
        for (const QString &note : notes) lines.append(QStringLiteral("  - ") + note.simplified());
    }
    if (m_contextDraft.cardId == card.id()) {
        const auto changes = m_contextDraft.changes();
        for (const QString &scope : ReviewContextNotes::scopes()) {
            if (!changes.contains(scope)) continue;
            const auto &note = changes.value(scope);
            const QString name = scope == ReviewContextNotes::sectionScope() ? QStringLiteral("Section note")
                : scope == ReviewContextNotes::pageScope() ? QStringLiteral("Page note") : QStringLiteral("Document note");
            const QString where = note.label.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(note.label.simplified());
            lines.append(QStringLiteral("- %1%2: %3").arg(name, where, note.text.isEmpty() ? QStringLiteral("(cleared)") : note.text.simplified()));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

void ReviewOverview::copyCardForAgent()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) return;
    const auto &card = m_dueCards.at(m_currentReview);
    const QList<QJsonObject> feedback = sessionFeedback(card, false, answerFeedbackStates());
    QGuiApplication::clipboard()->setText(reviewCardPromptText(m_docsRoot, card, currentPdfPath(card), feedback, orphanedFeedback(),
                                                               questionsOf(sessionHomeCards(card, feedback)), currentReviewText(card)));
}

void ReviewOverview::showCardInfo()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        return;
    }
    Q_EMIT cardInfoRequested(m_dueCards.at(m_currentReview).id());
}

void ReviewOverview::editCurrentCard()
{
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size()) {
        return;
    }
    const ReviewCard card = m_dueCards.at(m_currentReview);
    const bool revealed = !m_ratingPanel->isHidden();
    QVariantList rect;
    QString question;
    const auto editMarkers = [this, &card] {
        QString source;
        int line = 1;
        if (!chooseReviewSourceLocation(m_sessionPage, card, m_docsRoot, &source, &line)) return;
        if (source.isEmpty()) {
            KMessageBox::error(m_sessionPage, i18n("The source file with this card's markers could not be found."), i18n("Edit Marker Position"));
            return;
        }
        Q_EMIT openSourceRequested(source, line);
    };
    bool editAreas = false;
    const QString pdfPath = currentPdfPath(card);
    if (editReviewCardRegion(m_sessionPage, card, m_cardPresenter, &rect, &question, editMarkers,
                             canPlaceReviewCardByAreas(card) && QFileInfo::exists(pdfPath) ? &editAreas : nullptr)) {
        QString error;
        if (!m_store.updateCardContent(card, rect, question, &error)) {
            KMessageBox::error(m_sessionPage, error, i18n("Cannot Edit Review Card"));
        }
    } else if (editAreas) {
        editReviewCardAreas(m_sessionPage, m_store, m_docsRoot, card, pdfPath, reviewCardNeedsPlacingAgain(card));
    }
    if (m_currentReview < 0 || m_currentReview >= m_dueCards.size() || m_dueCards.at(m_currentReview).id() != card.id()) {
        return;
    }
    const auto cards = m_store.cardsWithIds({card.id()});
    for (const auto &updated : cards) {
        if (updated.id() == card.id()) {
            m_dueCards[m_currentReview] = updated;
            break;
        }
    }
    // Still the same card: what was picked before its answer (and when that was shown) stays.
    showCurrentCard(BeforeStep(m_before));
    if (revealed && m_currentReview >= 0 && m_dueCards.at(m_currentReview).id() == card.id()) {
        revealAnswer();
    }
}

void ReviewOverview::previousQuestion()
{
    if (m_reviewHistory.isEmpty()) {
        return;
    }
    m_tagUsage.reset(); // Recount after the undone answer is retracted.
    const auto entry = m_reviewHistory.constLast();
    const auto cards = m_store.cardsWithIds({entry.first});
    auto previous = std::find_if(cards.cbegin(), cards.cend(), [&entry](const ReviewCard &card) { return card.id() == entry.first && card.isApproved() && !card.schedule.deleted; });
    if (previous == cards.cend()) {
        KMessageBox::error(m_sessionPage, i18n("The previous question is no longer available."), i18n("Cannot Open Previous Question"));
        return;
    }
    ReviewCard card = *previous;
    QString error;
    if (!ensureCurrentGeometry(&card, &error)) {
        KMessageBox::error(m_sessionPage, error, i18n("Cannot Open Previous Question"));
        return;
    }
    // Undo of Retire card: an undo_retire for that retire brings the card back; an answer is retracted.
    const bool retired = m_retireEvents.contains(entry.second);
    if (retired) {
        if (!m_store.undoRetire(entry.second, &error)) {
            KMessageBox::error(m_sessionPage, error, i18n("Cannot Open Previous Question"));
            return;
        }
        m_retireEvents.remove(entry.second);
        Q_EMIT reviewRecorded();
    }
    auto remainingEvents = retired ? QList<QPair<QString, QString>>{} : m_scopedRatingEvents.value(entry.second, {{entry.first, entry.second}});
    while (!remainingEvents.isEmpty()) {
        const auto event = remainingEvents.constFirst();
        if (!m_store.retractRating(event.first, event.second, QDateTime::currentDateTimeUtc(), &error)) {
            m_scopedRatingEvents.insert(entry.second, remainingEvents);
            KMessageBox::error(m_sessionPage, error, i18n("Cannot Open Previous Question"));
            return;
        }
        remainingEvents.removeFirst();
        if (m_scopedRatingEvents.contains(entry.second)) m_scopedRatingEvents.insert(entry.second, remainingEvents);
    }
    m_scopedRatingEvents.remove(entry.second);
    m_reviewHistory.removeLast();
    m_lastActionWasSkip = !m_pendingSkip.id.isEmpty();
    QList<ReviewCard> remaining = m_currentReview >= 0 ? m_dueCards.mid(m_currentReview) : m_dueCards;
    if (m_singleCardSession) remaining.clear();
    remaining.removeIf([&entry](const ReviewCard &item) { return item.id() == entry.first; });
    const auto refreshed = m_store.cardsWithIds({entry.first});
    for (const auto &item : refreshed) {
        if (item.id() == entry.first) {
            card = item;
            break;
        }
    }
    remaining.prepend(card);
    m_dueCards = std::move(remaining);
    m_currentReview = 0;
    m_sessionComplete = false;
    showCurrentCard();
}

MentalModelView::MentalModelView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("thoughtsOverviewMentalModelPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    auto *bar = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("mentalModelStatus"));
    m_status->setWordWrap(true);
    bar->addWidget(m_status, 1);
    auto *reveal = new QPushButton(i18n("Show file"), this);
    connect(reveal, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path()).absolutePath()));
    });
    bar->addWidget(reveal);
    m_refresh = new QPushButton(i18n("Refresh"), this);
    m_refresh->setObjectName(QStringLiteral("mentalModelRefresh"));
    m_refresh->setToolTip(i18n("Write the Mental Model again from your review cards"));
    connect(m_refresh, &QPushButton::clicked, this, &MentalModelView::rewrite);
    bar->addWidget(m_refresh);
    layout->addLayout(bar);
    m_text = new QTextBrowser(this);
    m_text->setObjectName(QStringLiteral("mentalModelText"));
    m_text->setOpenLinks(false);
    connect(m_text, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == QLatin1String("thoughts-card")) Q_EMIT cardRequested(url.path());
        else QDesktopServices::openUrl(url);
    });
    layout->addWidget(m_text, 1);
    reload();
}

QString MentalModelView::path()
{
    return QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("mental-model/mental-model.md"));
}

// The Python that runs the writer, with its arguments before the script. Python from python.org has no python3 on
// Windows, only python and the launcher py. -X utf8: Windows' Python would otherwise read the cards and write the page
// in the ANSI code page, and stop at the first character it lacks.
static QStringList mentalModelPython()
{
#ifdef Q_OS_WIN
    if (!QStandardPaths::findExecutable(QStringLiteral("py")).isEmpty())
        return {QStringLiteral("py"), QStringLiteral("-3"), QStringLiteral("-X"), QStringLiteral("utf8")};
    return {QStringLiteral("python"), QStringLiteral("-X"), QStringLiteral("utf8")};
#else
    return {QStringLiteral("python3"), QStringLiteral("-X"), QStringLiteral("utf8")};
#endif
}

static QString mentalModelNeedsPython()
{
    return i18n("The Mental Model was not rewritten: Refresh needs Python 3. Install it from python.org, then click Refresh again.");
}

void MentalModelView::rewrite()
{
    if (m_writer) return;
    // The generator and its prompt ship in the app; they run from a copy next to the document.
    const QDir folder(QFileInfo(path()).absolutePath() + QStringLiteral("/generator"));
    folder.mkpath(QStringLiteral("."));
    for (const QString &name : {QStringLiteral("build.py"), QStringLiteral("prompt.md")}) {
        QFile::remove(folder.filePath(name));
        if (!QFile::copy(QStringLiteral(":/thoughts/mental-model/") + name, folder.filePath(name))) {
            m_status->setText(i18n("Could not prepare the Mental Model writer in %1.", folder.path()));
            return;
        }
        QFile::setPermissions(folder.filePath(name), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    m_writer = new QProcess(this);
    m_writer->setProcessChannelMode(QProcess::SeparateChannels);
    m_refresh->setEnabled(false);
    m_status->setText(i18n("Writing from your review cards… (a minute or two)"));
    connect(m_writer, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QString error = QString::fromUtf8(m_writer->readAllStandardError());
        const QString output = QString::fromUtf8(m_writer->readAllStandardOutput());
        m_writer->deleteLater();
        m_writer = nullptr;
        m_refresh->setEnabled(true);
        if (status == QProcess::NormalExit && exitCode == 0) {
            reload();
            // build.py rewrote the Library note (it says so on a line of its own).
            if (output.contains(QLatin1String("\nlibrary note written: ")) || output.startsWith(QLatin1String("library note written: ")))
                Q_EMIT libraryNoteWritten();
            return;
        }
#ifdef Q_OS_WIN
        // Without Python, Windows' own python is a shortcut to the Microsoft Store that ends with 9009.
        if (exitCode == 9009 || error.contains(QLatin1String("Python was not found"))) {
            m_status->setText(mentalModelNeedsPython());
            return;
        }
#endif
        // build.py ends with the reason; poppler warnings from thoughtsctl come before it.
        const QStringList lines = error.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        m_status->setText(i18n("The Mental Model was not rewritten: %1", lines.isEmpty() ? i18n("the writer stopped") : lines.last()));
    });
    connect(m_writer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_writer->deleteLater();
        m_writer = nullptr;
        m_refresh->setEnabled(true);
        m_status->setText(mentalModelNeedsPython());
    });
    QStringList arguments = mentalModelPython();
    const QString python = arguments.takeFirst();
    arguments << folder.filePath(QStringLiteral("build.py")) << QStringLiteral("--thoughtsctl")
              << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("thoughtsctl")) << QStringLiteral("--out") << path();
    m_writer->start(python, arguments);
}

void MentalModelView::reload()
{
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly)) {
        m_status->setText(i18n("No Mental Model yet."));
        m_text->setMarkdown(i18n("A Mental Model describes you from your review cards: what you know well and what not yet, "
                                 "what interests you, how you study, and what you like in answers. Each point links to the cards "
                                 "it comes from. It is written by an agent from your cards and appears here."));
        return;
    }
    m_loaded = QFileInfo(file).lastModified();
    m_status->setText(i18n("Written from your review cards · updated %1", QLocale().toString(m_loaded, QLocale::ShortFormat)));
    // No HTML: quoted card text such as "docs-import-<id>" must show as written, not as a tag.
    m_text->document()->setMarkdown(QString::fromUtf8(file.readAll()),
                                    QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));
}

}

