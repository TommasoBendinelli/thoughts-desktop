#include "../core/busycursor.h"
#include "../part/thoughtsplatform.h"
#include "../core/htmlattachment.h"
#include "../part/documentfeedback.h"
#include "../part/feedbackicon.h"
#include "../part/noteattachment.h"
#include "../part/pdfversions.h"
#include "../part/reviewcardfeedback.h"
#include "../part/thoughtsserveraddress.h"
#include "../part/sourceannotationindex.h"
#include "../part/verifiedartifactcache.h"
#include "../part/reviewplacement.h"
#include "../part/agentconversationopener.h"
#include "../part/agentconversations.h"
#include "../part/libraryprojectstore.h"
#include "../part/sourcefoldernames.h"
#include "../part/thoughtshome.h"
#include "../part/sourcedocumentmove.h"
#include <thoughts/syncclient.h>
#include <QCheckBox>
#include <QTextBrowser>
#include "../part/desktopsyncstore.h"
#include "../part/docschangemonitor.h"
#include "../part/librarysourceinventory.h"
#include "../part/markdownfolderimportdialog.h"
#include <QFileDialog>
/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "commentsoverview.h"
#include "../part/commentanchorstate.h"
#include "../part/commentmissingstate.h"
#include "proposaldecisions.h"
#include "rowbrowsertree.h"
#include "core/htmlattachment.h"
#include <QStringDecoder>
#include <QClipboard>
#include "websiteimportdialog.h"
#include "agentproposalparser.h"
#include "developerlog.h"
#include "librarysearch.h"
#include "readerdisplaytitle.h"
#include "reviewdocumenttools.h"
#include "documentsyncpresentation.h"
#include "documentstyleinformation.h"
#include "documentmanagementdialog.h"
#include "librarymodificationdates.h"
#include "reviewoverview.h"
#include "syncoperationstate.h"
#include "syncstatuslabels.h"

#include <QProcess>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTreeWidgetItemIterator>
#include <QMenu>
#include <QToolButton>
#include <QWidgetAction>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimeZone>
#include <QMessageBox>

#include "../part/docscommentsynchronizer.h"
#include "../part/proposalhistory.h"

#include <KConfigGroup>
#include <KGuiItem>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <KStandardGuiItem>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QCalendarWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QModelIndex>
#include <QMutex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <QToolTip>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace
{
// Comments that need the user's attention: shown under Issues and counted there.
const QStringList &commentIssueStatuses()
{
    static const QStringList statuses{QStringLiteral("needs_anchor"), QStringLiteral("needs_attention"), QStringLiteral("orphaned"), QStringLiteral("anchor_removed"),
                                      QStringLiteral("comment_missing")};
    return statuses;
}

// Finish the native click before navigation can hide this view or change focus.
// A queued doubleClicked connection alone can run before the physical release.
class CommentNavigationTree : public RowBrowserTree<QTreeView>
{
public:
    using RowBrowserTree<QTreeView>::RowBrowserTree;
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const QSignalBlocker block(this);
        QTreeView::mouseDoubleClickEvent(event);
        m_pendingActivation = event->button() == Qt::LeftButton ? indexAt(event->position().toPoint()) : QModelIndex();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QTreeView::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton || !m_pendingActivation.isValid()) { return; }
        const QPersistentModelIndex target = std::exchange(m_pendingActivation, QPersistentModelIndex());
        QTimer::singleShot(0, this, [this, target] {
            if (target.isValid()) { Q_EMIT doubleClicked(target); }
        });
    }
    void hideEvent(QHideEvent *event) override
    {
        m_pendingActivation = QPersistentModelIndex();
        QTreeView::hideEvent(event);
    }
private:
    QPersistentModelIndex m_pendingActivation;
};

// Enter saves the diary entry; Shift+Enter starts a new line.
class DiaryInput final : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void()> submit;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (event->modifiers() & Qt::ShiftModifier) {
                insertPlainText(QStringLiteral("\n"));
            } else if (submit) {
                submit();
            }
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

enum DiaryItemRole {
    DiaryIdRole = Qt::UserRole,
    DiaryContentsRole,
    DiaryPdfPathRole,
};

// Like CommentNavigationTree, a double-click edits only once the click has finished.
class DiaryEntryTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(const QString &)> editRequested;

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        QTreeWidget::mouseDoubleClickEvent(event);
        const QTreeWidgetItem *item = event->button() == Qt::LeftButton ? itemAt(event->position().toPoint()) : nullptr;
        m_pendingEdit = item ? item->data(0, DiaryIdRole).toString() : QString();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QTreeWidget::mouseReleaseEvent(event);
        const QString id = std::exchange(m_pendingEdit, QString());
        if (event->button() != Qt::LeftButton || id.isEmpty()) { return; }
        QTimer::singleShot(0, this, [this, id] {
            if (editRequested) { editRequested(id); }
        });
    }

private:
    QString m_pendingEdit;
};


enum Column {
    CommentColumn,
    DocumentColumn,
    PageColumn,
    SnapshotColumn,
    SourceColumn,
    StatusColumn,
    AuthorColumn,
    ActivityColumn,
    ModifiedColumn,
    // ✓ when the comment's document is built from a LaTeX or Markdown source, ✗ for a PDF.
    // Last in the model so saved header layouts keep their columns; shown after Comment.
    SourceDocumentColumn,
    ColumnCount,
};

enum Role {
    DocumentRole = Qt::UserRole + 1,
    DocumentDisplayRole,
    PdfPathRole,
    AnnotationIdRole,
    PageRole,
    PdfAvailableRole,
    SearchRole,
    SortRole,
    ContentsRole,
    SelectedTextRole,
    SourceDisplayRole,
    SourceFileRole,
    SourceLineRole,
    AnchorStatusRole,
    StatusDisplayRole,
    AuthorRole,
    PriorityRole,
    LocationPresentRole,
    LocationLabelRole,
    LatitudeRole,
    LongitudeRole,
    LocationObjectRole,
    ChatGptUrlRole,
    ReplyOrderRole,
    ItemTypeRole,
    BundleIdRole,
    ProposalSuppressedRole,
    ProposalKindRole,
    ProposalBeforePreviewRole,
    ProposalAfterPreviewRole,
    ProposalSourceSha256Role,
    LiveSourceProposalRole,
    ProposalIssueRole,
    FeedbackKindRole,
    FeedbackRecordRole,
    RemovedProposalRole,
    LogicalCommentRole,
    LogicalPayloadRole,
    PdfLocationsRole,
    CorrespondenceRole,
    ThreadRootIdRole,
    MissingMarkerRole,
};

enum DocumentListRole {
    DocumentPathRole = Qt::UserRole + 1,
    DocumentItemKindRole,
    DocumentFilenameRole,
    DocumentKindRole,
    DocumentSha256Role,
    DocumentDisplayNameRole,
    DocumentNavigationKeyRole,
    DocumentThreadIdsRole,
    DocumentStartPageRole,
    DocumentCompositionIdRole,
    DocumentScopeRole,
    DocumentLibraryProjectRole,
    // The theme name of a document row's icon (documentRowPresentation).
    DocumentRowIconRole,
};

enum DocumentListItemKind {
    AllCommentsItem,
    SectionHeadingItem,
    DocumentListEntry,
    ComponentListEntry,
    ThreadGroupListEntry,
    ProjectListEntry,
};

constexpr int LibrarySortValueRole = Qt::UserRole + 100;
constexpr int LibraryFilenameRole = Qt::UserRole + 101;
constexpr int LibrarySha256Role = Qt::UserRole + 102;
constexpr int LibraryKindRole = Qt::UserRole + 103;
constexpr int LibraryDisplayNameRole = Qt::UserRole + 104;
constexpr int LibraryAttachmentProjectRole = Qt::UserRole + 105;
constexpr int LibraryAttachmentTargetRole = Qt::UserRole + 106;
constexpr int LibraryIdentityRole = Qt::UserRole + 107;
constexpr int LibraryOrderRole = Qt::UserRole + 108;
constexpr int LibraryPageRole = Qt::UserRole + 109;
constexpr int LibraryOverleafUrlRole = Qt::UserRole + 110;
constexpr int LibraryDocumentSourceRole = Qt::UserRole + 111;
constexpr int LibraryCompositionIdRole = Qt::UserRole + 112;

constexpr int LibraryDetailRole = Qt::UserRole + 113;
constexpr int LibraryTypeRole = Qt::UserRole + 114;
constexpr int LibraryModifiedPathRole = Qt::UserRole + 115;
constexpr int LibraryModifiedPdfRole = Qt::UserRole + 116;
constexpr int LibraryFolderPathRole = Qt::UserRole + 118;
constexpr int LibraryMissingSourcesRole = Qt::UserRole + 119;
constexpr int LibraryRecordedDateRole = Qt::UserRole + 120;
// Why this row's PDF build failed or needs attention; empty for healthy rows.
constexpr int LibraryBuildFailureRole = Qt::UserRole + 121;
// How a Library date is described in its tooltip; the tooltip text is formatted only when shown.
constexpr int LibraryDateTooltipRole = Qt::UserRole + 160;
// The agent_conversation record (QJsonObject) of an Agent conversations row.
constexpr int LibraryConversationRole = Qt::UserRole + 161;
// True on a Project document that is unpublished (protocol/project-document-publication.md).
constexpr int LibraryUnpublishedRole = Qt::UserRole + 162;
enum LibraryDateTooltip { PlainDateTooltip = 0, BuiltDateTooltip = 1, ModifiedDateTooltip = 2 };

QString libraryDateTooltip(qint64 stamp, int kind)
{
    if (stamp < 0) return i18n("Content modification date is unavailable.");
    const QString date = QLocale().toString(QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime(), QLocale::LongFormat);
    switch (kind) {
    case BuiltDateTooltip: return i18n("Last built: %1", date);
    case ModifiedDateTooltip: return i18n("Modified: %1", date);
    default: return date;
    }
}
constexpr int LibraryReviewCardsColumn = 4;
constexpr int LibraryPagesColumn = 5;
constexpr int LibraryFilesColumn = 6;
// Pages of documents built from linked sources (TeX, Markdown) and of plain PDFs, as the phone's
// Library rows split them: the kind is the one the document row's link symbol shows.
constexpr int LibrarySourcePagesColumn = 7;
constexpr int LibraryPdfPagesColumn = 8;
constexpr int LibraryDueCardsColumn = 9;
constexpr int LibraryDocumentsColumn = 10;
constexpr int LibraryColumnCount = 11;
// Column 2 (Tracked sources) is never shown and not offered in the header menu.
constexpr int LibraryTrackedSourcesColumn = 2;
// The columns in their default order and visibility, as the header menu lists them.
struct LibraryColumnDefault {
    int column;
    bool visible;
};
constexpr LibraryColumnDefault LibraryColumnDefaults[] = {{0, true}, {1, true}, {3, true}, {LibraryFilesColumn, true},
    {LibraryDocumentsColumn, false}, {LibraryReviewCardsColumn, true}, {LibraryDueCardsColumn, false}, {LibraryPagesColumn, true},
    {LibrarySourcePagesColumn, true}, {LibraryPdfPagesColumn, true}};
constexpr char LibraryHeaderStateKey[] = "LibraryHeaderStateV1";

// Draw disclosure arrows without the native tree's connector lines.
class LibraryTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override
    {
        if (!model()->hasChildren(index)) { return; }
        QStyleOption option;
        option.initFrom(this);
        option.rect = QRect(isRightToLeft() ? rect.left() : rect.right() - indentation() + 1, rect.top(), indentation(), rect.height());
        option.state |= QStyle::State_Children;
        option.state.setFlag(QStyle::State_Open, isExpanded(index));
        style()->drawPrimitive(QStyle::PE_IndicatorBranch, &option, painter, this);
    }
};

enum class LibrarySymbol { Folder, PendingImport, Documents, Source, Pdf, Linked, Unlinked, LinkUnknown, MissingSource };

// Small palette-aware outlines stay legible even when the platform PDF icon is tiny.
QIcon librarySymbol(LibrarySymbol symbol, const QPalette &palette)
{
    QIcon icon;
    QColor ink = palette.color(QPalette::Text);
    if (symbol == LibrarySymbol::MissingSource) { ink = QColor(180, 105, 0); }
    if (symbol == LibrarySymbol::Source) {
        const QColor accent = palette.color(QPalette::Highlight);
        ink = QColor((ink.red() + accent.red()) / 2, (ink.green() + accent.green()) / 2, (ink.blue() + accent.blue()) / 2);
    }
    for (int size : {16, 20, 24, 32, 40, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (symbol == LibrarySymbol::MissingSource) {
            QPainterPath triangle;
            triangle.moveTo(12, 3); triangle.lineTo(22, 20); triangle.lineTo(2, 20); triangle.closeSubpath();
            painter.drawPath(triangle);
            painter.drawLine(QPointF(12, 9), QPointF(12, 14));
            painter.drawPoint(QPointF(12, 17));
            painter.end();
            icon.addPixmap(pixmap);
            continue;
        }
        if (symbol == LibrarySymbol::Linked || symbol == LibrarySymbol::Unlinked || symbol == LibrarySymbol::LinkUnknown) {
            QPainterPath links;
            links.moveTo(10, 7); links.lineTo(7, 7);
            links.cubicTo(0.5, 7, 0.5, 17, 7, 17); links.lineTo(10, 17);
            links.moveTo(14, 7); links.lineTo(17, 7);
            links.cubicTo(23.5, 7, 23.5, 17, 17, 17); links.lineTo(14, 17);
            painter.drawPath(links);
            painter.drawLine(QPointF(8, 12), QPointF(16, 12));
            if (symbol == LibrarySymbol::Unlinked) {
                painter.setPen(QPen(palette.color(QPalette::Base), 4, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(3, 3), QPointF(21, 21));
                painter.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(3, 3), QPointF(21, 21));
            } else if (symbol == LibrarySymbol::LinkUnknown) {
                painter.setBrush(palette.color(QPalette::Base));
                painter.drawEllipse(QPointF(18, 18), 5, 5);
                painter.drawLine(QPointF(18, 15), QPointF(18, 18));
                painter.drawPoint(QPointF(18, 21));
            }
            painter.end();
            icon.addPixmap(pixmap);
            continue;
        }
        QPainterPath outline;
        if (symbol == LibrarySymbol::Folder || symbol == LibrarySymbol::PendingImport) {
            outline.moveTo(3, 6); outline.lineTo(9, 6); outline.lineTo(11, 8); outline.lineTo(21, 8);
            outline.lineTo(21, 20); outline.lineTo(3, 20); outline.closeSubpath();
        } else {
            if (symbol == LibrarySymbol::Documents) {
                painter.drawLine(QPointF(3, 5), QPointF(3, 21));
                painter.drawLine(QPointF(3, 21), QPointF(15, 21));
            }
            outline.moveTo(7, 3); outline.lineTo(15, 3); outline.lineTo(21, 9);
            outline.lineTo(21, 19); outline.lineTo(7, 19); outline.closeSubpath();
            outline.moveTo(15, 3); outline.lineTo(15, 9); outline.lineTo(21, 9);
        }
        painter.drawPath(outline);
        if (symbol == LibrarySymbol::PendingImport) {
            painter.setBrush(palette.color(QPalette::Base));
            painter.drawEllipse(QPointF(17, 17), 6, 6);
            painter.drawLine(QPointF(17, 13.5), QPointF(17, 17));
            painter.drawLine(QPointF(17, 17), QPointF(19.5, 18.5));
        }
        if (symbol == LibrarySymbol::Source) {
            const qreal center = 14;
            QPainterPath code;
            code.moveTo(center - 2, 11); code.lineTo(center - 4, 13.5); code.lineTo(center - 2, 16);
            code.moveTo(center + 2, 11); code.lineTo(center + 4, 13.5); code.lineTo(center + 2, 16);
            painter.drawPath(code);
        } else if (symbol == LibrarySymbol::Pdf) {
            painter.drawLine(QPointF(10, 12), QPointF(18, 12));
            painter.drawLine(QPointF(10, 15), QPointF(16, 15));
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QString librarySelectionDetail(const QTreeWidgetItem *item)
{
    if (!item) { return {}; }
    const QString path = item->data(0, LibraryDetailRole).toString();
    QString detail = path.isEmpty() ? QString() : item->data(0, LibraryTypeRole).toString() + QStringLiteral(" · ") + path;
    // The selected row's build failure in full, selectable for copying.
    if (const QString failure = item->data(0, LibraryBuildFailureRole).toString(); !failure.isEmpty()) {
        if (!detail.isEmpty()) detail += QLatin1Char('\n');
        detail += failure == Thoughts::SyncStatusLabels::pdfBuildFailed() ? failure : i18nc("@info PDF build failed: error", "%1: %2", Thoughts::SyncStatusLabels::pdfBuildFailed(), failure);
    }
    return detail;
}

class LibraryTreeItem final : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    QVariant data(int column, int role) const override
    {
        if (column != 1 || role != Qt::ToolTipRole) return QTreeWidgetItem::data(column, role);
        const QVariant kind = QTreeWidgetItem::data(1, LibraryDateTooltipRole);
        if (!kind.isValid()) return QTreeWidgetItem::data(column, role);
        // The long format names the time zone, which macOS resolves slowly; formatting every
        // row on each Library refresh stalled the window for seconds.
        return libraryDateTooltip(QTreeWidgetItem::data(1, LibrarySortValueRole).toLongLong(), kind.toInt());
    }

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const QVariant order = data(0, LibraryOrderRole);
        const QVariant otherOrder = other.data(0, LibraryOrderRole);
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const bool projectGroups = parent() && !parent()->parent();
        if (order.isValid() && otherOrder.isValid() && (column != 1 || projectGroups)) {
            const bool ascending = !treeWidget() || treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder;
            return ascending ? order.toInt() < otherOrder.toInt() : order.toInt() > otherOrder.toInt();
        }
        const QVariant leftSortValue = data(column, LibrarySortValueRole);
        const QVariant rightSortValue = other.data(column, LibrarySortValueRole);
        if (leftSortValue.isValid() && rightSortValue.isValid()) {
            return leftSortValue.toLongLong() < rightSortValue.toLongLong();
        }
        return QTreeWidgetItem::operator<(other);
    }
};

class LibraryTreeDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const QString type = index.data(LibraryTypeRole).toString();
        if (index.column() == 0 && !type.isEmpty()) {
            const QString detail = index.data(Qt::ToolTipRole).toString();
            QToolTip::showText(event->globalPos(), detail.isEmpty() ? type : type + QLatin1Char('\n') + detail, view);
            return true;
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem softened(option);
        // Keep the selected colour stable: Breeze otherwise lightens it on hover.
        if (softened.state.testFlag(QStyle::State_Selected)) {
            softened.state.setFlag(QStyle::State_MouseOver, false);
        }
        const QColor selectionBase = option.palette.color(QPalette::Base);
        const QColor accent = option.palette.color(QPalette::Highlight);
        softened.palette.setColor(QPalette::Highlight, QColor((selectionBase.red() * 7 + accent.red()) / 8,
                                                              (selectionBase.green() * 7 + accent.green()) / 8,
                                                              (selectionBase.blue() * 7 + accent.blue()) / 8));
        softened.palette.setColor(QPalette::HighlightedText, option.palette.color(QPalette::Text));
        const QStringList lines = index.data().toString().split(QLatin1Char('\n'));
        if (index.column() != 0 || lines.size() < 2) {
            QStyledItemDelegate::paint(painter, softened, index);
            return;
        }
        QStyleOptionViewItem styled(softened);
        initStyleOption(&styled, index);
        QStyle *style = styled.widget ? styled.widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, styled.widget).adjusted(2, 0, -3, 0);
        styled.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);
        painter->save();
        const bool selected = styled.state.testFlag(QStyle::State_Selected);
        painter->setPen(styled.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->setFont(styled.font);
        const int lineHeight = QFontMetrics(styled.font).height();
        int top = textRect.center().y() - (lineHeight * lines.size()) / 2;
        painter->drawText(QRect(textRect.left(), top, textRect.width(), lineHeight), Qt::AlignVCenter,
                          QFontMetrics(styled.font).elidedText(lines.first(), Qt::ElideRight, textRect.width()));
        QFont detailFont = styled.font;
        detailFont.setPointSizeF(qMax(9.0, detailFont.pointSizeF() - 1.0));
        painter->setFont(detailFont);
        QColor detail = styled.palette.color(QPalette::Text);
        const QColor base = styled.palette.color(QPalette::Base);
        detail.setRed((detail.red() * 3 + base.red()) / 4);
        detail.setGreen((detail.green() * 3 + base.green()) / 4);
        detail.setBlue((detail.blue() * 3 + base.blue()) / 4);
        painter->setPen(selected ? styled.palette.color(QPalette::HighlightedText) : detail);
        for (int i = 1; i < lines.size(); ++i) {
            top += lineHeight;
            painter->drawText(QRect(textRect.left(), top, textRect.width(), lineHeight), Qt::AlignVCenter,
                              QFontMetrics(detailFont).elidedText(lines.at(i), Qt::ElideMiddle, textRect.width()));
        }
        painter->restore();
    }
};

void configureOverviewHeader(QTreeView *table)
{
    QHeaderView *header = table->header();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setCascadingSectionResizes(false);
}

QString readStableId(const QJsonObject &record)
{
    QString id = record.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        id = record.value(QStringLiteral("comment_key")).toString(record.value(QStringLiteral("review_key")).toString());
    }
    if (id.startsWith(QLatin1String("id:"))) {
        id.remove(0, 3);
    }
    return id;
}

QList<QJsonObject> recordsWithFedoraProjections(const QList<QJsonObject> &sidecarRecords, const QString &stem, const QJsonObject &projections)
{
    QHash<QString, QJsonObject> recordsById;
    QStringList order;
    QList<QJsonObject> recordsWithoutIds;
    for (const QJsonObject &record : sidecarRecords) {
        const QString id = readStableId(record);
        if (id.isEmpty()) {
            recordsWithoutIds.append(record);
            continue;
        }
        if (!recordsById.contains(id)) {
            order.append(id);
        }
        recordsById.insert(id, record);
    }

    const QString prefix = QStringLiteral("annotation\n%1\n").arg(stem);
    QStringList projectionKeys;
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (it.key().startsWith(prefix) && it.value().isObject()) {
            projectionKeys.append(it.key());
        }
    }
    std::sort(projectionKeys.begin(), projectionKeys.end());
    for (const QString &key : std::as_const(projectionKeys)) {
        QJsonObject record = projections.value(key).toObject();
        QString id = record.value(QStringLiteral("annotation_id")).toString(record.value(QStringLiteral("id")).toString());
        if (id.isEmpty()) {
            id = key.mid(prefix.size());
        }
        if (id.isEmpty()) {
            continue;
        }
        if (record.value(QStringLiteral("deleted")).toBool()) {
            recordsById.remove(id);
            order.removeAll(id);
            continue;
        }
        record.insert(QStringLiteral("id"), id);
        if (!recordsById.contains(id)) {
            order.append(id);
        }
        recordsById.insert(id, record);
    }

    QList<QJsonObject> result = recordsWithoutIds;
    result.reserve(result.size() + order.size());
    for (const QString &id : std::as_const(order)) {
        // A logical deletion also suppresses legacy annotation projections of
        // the same comment, including when a newer bundle omits that comment.
        const QJsonObject logical = projections.value(QStringLiteral("logical_comment\ncomments\n") + id).toObject();
        if (logical.value(QStringLiteral("deleted")).toBool()) {
            continue;
        }
        result.append(recordsById.value(id));
    }
    return result;
}

QDateTime firstCommentDate(const QJsonObject &record, std::initializer_list<const char *> fields)
{
    for (const char *field : fields) {
        const QDateTime parsed = Okular::DocsCommentSynchronizer::parseCommentDate(record.value(QLatin1String(field)));
        if (parsed.isValid()) {
            return parsed;
        }
    }
    return {};
}

QDateTime commentDisplayDate(const QJsonObject &record)
{
    return firstCommentDate(record, {"created_at", "created", "first_seen_at", "updated_at", "modified"});
}

QDateTime rootCommentSortDate(const QJsonObject &record)
{
    if (record.contains(QStringLiteral("comment_id"))) {
        return firstCommentDate(record, {"content_updated_at", "updated_at", "modified", "created_at", "created", "first_seen_at"});
    }
    return firstCommentDate(record, {"content_updated_at", "modified", "updated_at", "created_at", "created", "first_seen_at"});
}

// Diary entries are notes too (subtype "Note"), told apart by entry_kind;
// every other note is a to-do.
bool diaryEntry(const QJsonObject &payload)
{
    return payload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note")
        && payload.value(QStringLiteral("entry_kind")).toString() == QLatin1String("diary");
}

// Navigation columns classify root threads, so replies never inflate totals.
// Diary entries are in no column (0): they are listed in the Diary tab only.
int threadCountColumn(const Thoughts::TrackedComment &comment)
{
    if (diaryEntry(comment.logicalPayload)) return 0;
    return comment.agentProposal ? 2 : comment.logicalPayload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note") ? 3 : 1;
}

bool visibleThread(const Thoughts::TrackedComment &comment, const QSet<QString> &resolvedProposals)
{
    return comment.threadRootId == comment.id && !(comment.agentProposal && resolvedProposals.contains(comment.id));
}

// "like", "dislike", "question" or "other": the four types of a comment on a passage.
QString feedbackKindOf(const QJsonObject &record)
{
    const QString kind = Thoughts::DocumentFeedback::kindOf(record);
    if (!kind.isEmpty()) return kind;
    return record.value(QStringLiteral("polarity")).toString() == Thoughts::ReviewCardFeedback::negative() ? QStringLiteral("dislike") : QStringLiteral("like");
}

QString feedbackKindLabel(const QString &kind)
{
    return kind == QLatin1String("like") ? i18n("Like") : kind == QLatin1String("dislike") ? i18n("Dislike")
        : kind == Thoughts::DocumentFeedback::question() ? i18n("Question") : kind == Thoughts::DocumentFeedback::drawing() ? i18n("Drawing")
        : kind == Thoughts::DocumentFeedback::textBox() ? i18n("Text box") : i18n("Comment");
}

// How many count tooltips are kept (three columns, one per distinct count).
constexpr int MaximumCountTooltips = 4096;

// Each row of the Library tree has three of these, and the tree has thousands of
// rows with the same few counts. Formatting a number asks the system for the
// user's number format every time (macOS creates a number formatter for it):
// more than half of the time a rebuild of the tree took. Keep the texts.
QString threadCountTooltip(int column, int count)
{
    static QHash<quint64, QString> texts;
    const quint64 key = (quint64(quint32(column)) << 32) | quint32(count);
    const auto known = texts.constFind(key);
    if (known != texts.cend()) {
        return *known;
    }
    if (texts.size() >= MaximumCountTooltips) {
        texts.clear();
    }
    const QString text = column == 1 ? i18np("%1 comment thread", "%1 comment threads", count)
        : column == 2 ? i18np("%1 pending agent proposal", "%1 pending agent proposals", count)
                      : i18np("%1 to-do", "%1 to-dos", count);
    texts.insert(key, text);
    return text;
}

// A to-do's "visible_from" (ISO date) keeps it out of the To-dos list until that day.
QDate todoVisibleFrom(const QJsonObject &payload)
{
    if (payload.value(QStringLiteral("subtype")).toString() != QLatin1String("Note")) return {};
    return QDate::fromString(payload.value(QStringLiteral("visible_from")).toString(), Qt::ISODate);
}

bool todoHiddenToday(const QJsonObject &payload)
{
    const QDate from = todoVisibleFrom(payload);
    return from.isValid() && from > QDate::currentDate();
}

// When a diary entry was written, in the UTC offset it was written at
// ("diary_at"); entries without one fall back to their creation time.
QDateTime diaryEntryTime(const QJsonObject &payload)
{
    const QDateTime at = QDateTime::fromString(payload.value(QStringLiteral("diary_at")).toString(), Qt::ISODate);
    if (at.isValid()) return at;
    return QDateTime::fromString(payload.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).toLocalTime();
}

// "diary_at": local date-time with its offset, to the second, e.g. 2026-10-24T14:03:27+02:00.
QString diaryTimestamp(const QDateTime &local)
{
    const int offset = local.offsetFromUtc();
    const int minutes = std::abs(offset) / 60;
    return local.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) + (offset < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        + QStringLiteral("%1:%2").arg(minutes / 60, 2, 10, QLatin1Char('0')).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString diaryDayTitle(const QDate &day)
{
    const QDate today = QDate::currentDate();
    if (day == today) return i18n("Today");
    if (day == today.addDays(-1)) return i18n("Yesterday");
    return QLocale().toString(day, day.year() == today.year() ? QStringLiteral("ddd d MMM") : QStringLiteral("ddd d MMM yyyy"));
}

QString formattedDocumentBytes(qint64 bytes)
{
    if (bytes < 0) {
        return i18n("Unavailable");
    }
    static const QLocale locale(QLocale::system().name());
    const double mebibytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const bool useKibibytes = mebibytes < 0.1;
    const double scaled = useKibibytes ? static_cast<double>(bytes) / 1024.0 : mebibytes;
    const int precision = scaled < 10.0 ? 2 : 1;
    return i18n("%1 %2",
                locale.toString(scaled, 'f', precision),
                useKibibytes ? QStringLiteral("KiB") : QStringLiteral("MiB"));
}

// Size-only changes must not replace tree items (selection and open menus retain them).
bool sameLibrarySourceFiles(const QHash<QString, QJsonObject> &before, const QHash<QString, QJsonObject> &after)
{
    if (before.size() != after.size()) return false;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        if (!before.contains(it.key())) return false;
        auto left = before.value(it.key()).value(QStringLiteral("files")).toArray();
        auto right = it.value().value(QStringLiteral("files")).toArray();
        if (left.size() != right.size()) return false;
        for (qsizetype i = 0; i < left.size(); ++i) {
            auto a = left[i].toObject(), b = right[i].toObject();
            a.remove(QStringLiteral("size_bytes")); b.remove(QStringLiteral("size_bytes"));
            if (a != b) return false;
        }
    }
    return true;
}

QString abbreviatedSha256(const QString &sha256)
{
    return sha256.size() == 64 ? sha256.left(12) + QChar(0x2026) : i18n("Unavailable");
}

QString documentStyleStatusText(const Thoughts::DocumentStyleResourceStatus &resource,
                                bool publishedResourcesAvailable,
                                bool projectOverride)
{
    const QString localSha = abbreviatedSha256(resource.localSha256);
    const QString publishedSha = abbreviatedSha256(resource.publishedSha256);
    const QString fedoraSha = abbreviatedSha256(resource.fedoraSha256);
    if (resource.liveAvailable) {
        if (resource.publishedIdentityAvailable) {
            return resource.localMatchesPublished()
                ? i18n("Matches published PDF · SHA %1", publishedSha)
                : i18n("Different from published PDF · current %1 · published %2", localSha, publishedSha);
        }
        if (resource.fedoraIdentityAvailable) {
            return resource.localMatchesFedora()
                ? i18n("Matches the server · published PDF SHA unavailable · SHA %1", fedoraSha)
                : i18n("Local edits waiting to sync · current %1 · server %2", localSha, fedoraSha);
        }
        if (projectOverride && publishedResourcesAvailable) {
            return i18n("Local override is not in the published PDF · SHA %1", localSha);
        }
        return i18n("Published PDF SHA unavailable · current SHA %1", localSha);
    }
    if (resource.publishedIdentityAvailable) {
        return i18n("Live file unavailable · published SHA %1", publishedSha);
    }
    if (projectOverride && publishedResourcesAvailable) {
        return i18n("No project override");
    }
    return projectOverride ? i18n("No project override · published PDF SHA unavailable")
                           : i18n("Shared style unavailable · published PDF SHA unavailable");
}

struct LibraryDocumentMetadata {
    QString filename;
    QString kind;
    QString sha256;
    QString displayName;
    QString project;
    QString documentId;
    QDateTime publishedAt;
    QDateTime builtAt;
    qint64 pageCount = -1;
    QString latestBuildMessage;
    QString latestBuildSource;
    qint64 verifiedBuildInputBytes = -1;
    qint64 verifiedBuildInputCount = -1;
    bool generationCurrent = false;
    bool pdfStale = false;
    bool verifiedBuildInputsAvailable = false;
};

std::optional<qint64> nonNegativeJsonInteger(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble(-1.0);
    constexpr double largestExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0.0 || std::trunc(number) != number || number > largestExactJsonInteger) {
        return std::nullopt;
    }
    return static_cast<qint64>(number);
}

QDateTime publishedDate(const QJsonValue &value)
{
    QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    }
    return parsed.toUTC();
}

QByteArray rawFileContentSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray();
}

QByteArray fileContentSha256(const QString &path)
{
    // A verified PDF's filesystem identity changes on replacement or edit.
    // Retain its checked digest across Library openings instead of re-reading
    // hundreds of megabytes of unchanged PDFs on every launch.
    if (!path.contains(QLatin1String("/current_pdfs/"))
        || QFileInfo(path).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0)
        return rawFileContentSha256(path);
    const QString identity = Thoughts::artifactFileIdentity(path);
    if (identity.isEmpty()) return {};
    const QString cachePath = QDir(Thoughts::Platform::applicationDataRoot())
                                  .filePath(QStringLiteral("thoughts-server/verified-pdf-hashes.json"));
    static QMutex mutex;
    static bool loaded = false;
    static QJsonObject entries;
    {
        QMutexLocker guard(&mutex);
        if (!loaded) {
            QFile input(cachePath);
            if (input.open(QIODevice::ReadOnly)) {
                const QJsonObject cache = QJsonDocument::fromJson(input.readAll()).object();
                const QJsonObject saved = cache.value(QStringLiteral("entries")).toObject();
                if (cache.value(QStringLiteral("schema_version")).toInt() == 1
                    && cache.value(QStringLiteral("entries_sha256")).toString() == QString::fromLatin1(QCryptographicHash::hash(
                           QJsonDocument(saved).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()))
                    entries = saved;
            }
            loaded = true;
        }
        const QJsonObject entry = entries.value(path).toObject();
        if (entry.value(QStringLiteral("identity")).toString() == identity) {
            const QByteArray hash = QByteArray::fromHex(entry.value(QStringLiteral("sha256")).toString().toLatin1());
            if (hash.size() == 32) return hash;
        }
    }
    const QByteArray hash = rawFileContentSha256(path);
    if (hash.size() != 32 || Thoughts::artifactFileIdentity(path) != identity) return {};
    QMutexLocker guard(&mutex);
    entries.insert(path, QJsonObject{{QStringLiteral("identity"), identity},
                                     {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())}});
    const QJsonObject cache {{QStringLiteral("schema_version"), 1}, {QStringLiteral("entries"), entries},
        {QStringLiteral("entries_sha256"), QString::fromLatin1(QCryptographicHash::hash(
             QJsonDocument(entries).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())}};
    if (QDir().mkpath(QFileInfo(cachePath).absolutePath())) {
        QSaveFile output(cachePath);
        const QByteArray bytes = QJsonDocument(cache).toJson(QJsonDocument::Compact);
        if (output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size()) output.commit();
    }
    return hash;
}

struct DocumentSegmentTiming {
    qint64 pointerMs = 0;
    qint64 artifactReadMs = 0;
    qint64 artifactHashMs = 0;
    qint64 pdfHashMs = 0;
    qint64 jsonParseMs = 0;
    qint64 validationMs = 0;
    qint64 artifactBytes = 0;
    qint64 pdfBytes = 0;
    int verifiedDocuments = 0;
};

// Read only the source artifact bound to this exact PDF publication. Legacy
// comments/.layout files may describe a different build and must not drive this UI.
QJsonArray libraryDocumentSegments(const QString &docsRoot, const Thoughts::TrackedCommentDocument &document,
                                   DocumentSegmentTiming *timing = nullptr)
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (document.libraryKind != QLatin1String("source_linked") || document.librarySha256.isEmpty()) {
        return {};
    }
    const auto readJson = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    };
    QDir bundle(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")));
    if (!QFileInfo::exists(bundle.filePath(QStringLiteral("documents")))) {
        bundle = QDir(QDir(docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
    }
    const QString active = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-artifacts/%1.json").arg(document.libraryFilename));
    const QJsonObject pointer = readJson(QFileInfo::exists(active) ? active : bundle.filePath(QStringLiteral("documents/%1.json").arg(document.libraryFilename)));
    const QString digest = pointer.value(QStringLiteral("artifact_sha256")).toString();
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    if (!sha.match(digest).hasMatch() || pointer.value(QStringLiteral("pdf_sha256")).toString() != document.librarySha256
        || pointer.value(QStringLiteral("filename")).toString() != document.libraryFilename) {
        return {};
    }
    if (timing) timing->pointerMs += elapsed.restart();
    // The artifact verified and parsed; its bytes are read only when the
    // parsed artifact is not kept already, and the artifact this PDF had
    // before is dropped.
    const QString artifactPath = bundle.filePath(QStringLiteral("objects/%1").arg(digest));
    const QJsonObject manifest = Thoughts::verifiedSourceArtifactJson(artifactPath, digest, document.libraryFilename);
    if (manifest.isEmpty()) return {};
    if (timing) {
        timing->artifactReadMs += elapsed.restart();
        timing->artifactBytes += QFileInfo(artifactPath).size();
    }
    if (fileContentSha256(document.pdfPath).toHex() != document.librarySha256.toLatin1()) return {};
    if (timing) {
        timing->pdfHashMs += elapsed.restart();
        timing->pdfBytes += QFileInfo(document.pdfPath).size();
        ++timing->verifiedDocuments;
    }
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    if (timing) timing->jsonParseMs += elapsed.restart();
    if (manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")
        || pdf.value(QStringLiteral("pdf_sha256")).toString() != document.librarySha256
        || pdf.value(QStringLiteral("filename")).toString() != document.libraryFilename) {
        return {};
    }
    QJsonArray segments = pdf.value(QStringLiteral("segments")).toArray();
    qint64 nextPage = 1;
    for (const QJsonValue &value : segments) {
        const QJsonObject segment = value.toObject();
        const int start = segment.value(QStringLiteral("start_page")).toInt();
        const int count = segment.value(QStringLiteral("page_count")).toInt();
        if (start != nextPage || count < 1) {
            return {};
        }
        nextPage += count;
    }
    if (nextPage - 1 != pdf.value(QStringLiteral("page_count")).toInt()) {
        return {};
    }
    if (timing) timing->validationMs += elapsed.elapsed();
    return segments;
}

// Resolve imported standalone documents without changing the shared source.
QString standaloneDocumentSource(const QString &projectRoot, const QString &entry)
{
    QFile file(entry);
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.readAll());
        if (text.startsWith(QLatin1String("% thoughts:standalone-import\n"))) {
            static const QRegularExpression imported(QStringLiteral(R"(\\subimport\{([^{}]+)\}\{([^{}]+)\})"));
            const auto match = imported.match(text);
            if (match.hasMatch()) {
                return QFileInfo(QDir(projectRoot).filePath(match.captured(1) + match.captured(2))).canonicalFilePath();
            }
        }
    }
    return QFileInfo(entry).canonicalFilePath();
}

QJsonObject documentOptions(const QString &projectRoot)
{
    return Thoughts::DocumentBuildSelection::read(QDir(projectRoot).filePath(QStringLiteral("tex/support/thoughts_document_options.txt")));
}

QString saveDocumentOptions(const QString &projectRoot, const QJsonObject &options)
{
    if (options.isEmpty()) return i18n("The document settings could not be read.");
    return Thoughts::DocumentBuildSelection::write(QDir(projectRoot).filePath(QStringLiteral("tex/support/thoughts_document_options.txt")), options);
}

bool documentExcluded(const QJsonObject &options, const QString &projectRoot, const QString &id)
{
    return options.value(QStringLiteral("excluded_documents")).toArray().contains(id)
        || (options.contains(QStringLiteral("selected_documents")) && id.startsWith(QFileInfo(projectRoot).fileName() + QLatin1Char('/'))
            && !options.value(QStringLiteral("selected_documents")).toArray().contains(id));
}

bool documentExcluded(const QString &projectRoot, const QString &id)
{
    return documentExcluded(documentOptions(projectRoot), projectRoot, id);
}

bool proposalSourceExcluded(const QString &docsRoot, const QString &source)
{
    if (source.isEmpty()) return false;
    const QString relative = QDir(docsRoot).relativeFilePath(QDir(docsRoot).absoluteFilePath(source));
    if (relative.startsWith(QStringLiteral("../"))) return true;
    // A docs-* folder or a Project's default folder applies its own policy.
    QString owner = Thoughts::SourceOwner::of(relative);
    if (owner.isEmpty()) owner = relative.section(QLatin1Char('/'), 0, 0);
    const Thoughts::ThoughtIgnore ignored(QDir(docsRoot).filePath(owner));
    return !ignored.error().isEmpty() || ignored.excludes(relative.mid(owner.size() + 1));
}

QString documentTitle(const QString &projectRoot, const QString &id, const QString &fallback)
{
    return documentOptions(projectRoot).value(QStringLiteral("titles")).toObject().value(id).toString(fallback);
}

QStringList standaloneDocumentEntries(const QString &projectRoot)
{
    const QDir sources(QDir(projectRoot).filePath(QStringLiteral("tex/source")));
    QStringList entries;
    QFile manifest(QDir(projectRoot).filePath(QStringLiteral("tex/docs_order_manifest.txt")));
    if (manifest.open(QIODevice::ReadOnly)) {
        for (const QString &line : QString::fromUtf8(manifest.readAll()).split(QLatin1Char('\n'))) {
            const QString name = line.trimmed();
            if (!name.startsWith(QLatin1String("appendix_")) && name != QLatin1String("documentation_index.tex") && name != QLatin1String("docs_combined_compact.tex") && name == QFileInfo(name).fileName() && name.endsWith(QLatin1String(".tex")) && sources.exists(name) && !entries.contains(name)) {
                entries.append(name);
            }
        }
    }
    for (const QString &name : sources.entryList({QStringLiteral("*.tex")}, QDir::Files, QDir::Name)) {
        if (name == QLatin1String("documentation_index.tex") || name == QLatin1String("docs_combined_compact.tex") || name.startsWith(QLatin1String("appendix_"))) { continue; }
        if (!entries.contains(name)) { entries.append(name); }
    }
    const QString prefix = QFileInfo(projectRoot).fileName() + QStringLiteral("/tex/source/");
    const auto options = documentOptions(projectRoot);
    entries.removeIf([&](const QString &name) { return documentExcluded(options, projectRoot, prefix + name); });
    return entries;
}

QList<QPair<QString, QString>> projectStandaloneSources(const QString &projectRoot)
{
    QList<QPair<QString, QString>> result;
    for (const QString &name : standaloneDocumentEntries(projectRoot)) {
        const QString source = standaloneDocumentSource(projectRoot, QDir(projectRoot).filePath(QStringLiteral("tex/source/") + name));
        if (!source.isEmpty()) result.append({name, source});
    }
    return result;
}

QString addStandaloneDocument(const QString &projectRoot, const QString &source)
{
    const QDir project(QFileInfo(projectRoot).canonicalFilePath());
    const QString canonical = QFileInfo(source).canonicalFilePath();
    QJsonObject options = documentOptions(projectRoot);
    const QString docsRoot = QFileInfo(projectRoot).absolutePath();
    const QString owner = QFileInfo(projectRoot).fileName();
    const auto selectEntry = [&](const QString &id) {
        QString error;
        const auto entries = Thoughts::DocumentBuildSelection::candidates(docsRoot, owner, &error);
        if (!error.isEmpty()) return error;
        QJsonArray selected;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("id")).toString() == id || Thoughts::DocumentBuildSelection::selected(options, entry)) selected.append(entry.value(QStringLiteral("id")));
        }
        return Thoughts::DocumentBuildSelection::save(docsRoot, owner, entries, selected);
    };
    for (const auto &value : Thoughts::DocumentBuildSelection::candidates(docsRoot, owner)) {
        const auto entry = value.toObject();
        if (standaloneDocumentSource(projectRoot, project.filePath(entry.value(QStringLiteral("source")).toString())) == canonical) {
            return selectEntry(entry.value(QStringLiteral("id")).toString());
        }
    }
    const QJsonArray excluded = options.value(QStringLiteral("excluded_documents")).toArray();
    for (const QJsonValue &id : excluded) {
        const QString entryPath = QDir(projectRoot).filePath(QStringLiteral("tex/source/") + QFileInfo(id.toString()).fileName());
        if (standaloneDocumentSource(projectRoot, entryPath) == canonical) {
            QJsonArray restored;
            for (const QJsonValue &other : excluded) { if (other != id) { restored.append(other); } }
            options.insert(QStringLiteral("excluded_documents"), restored);
            return saveDocumentOptions(projectRoot, options);
        }
    }
    for (const QString &entry : standaloneDocumentEntries(projectRoot)) {
        if (standaloneDocumentSource(projectRoot, project.filePath(QStringLiteral("tex/source/") + entry)) == canonical) { return {}; }
    }
    QFile input(canonical);
    if (!input.open(QIODevice::ReadOnly)) { return i18n("The source file could not be read."); }
    const QString text = QString::fromUtf8(input.readAll());
    static const QRegularExpression documentClass(QStringLiteral(R"((?m)^\s*\\documentclass\b)"));
    if (!documentClass.match(text).hasMatch()) { return i18n("Choose a complete LaTeX document containing a document class."); }
    const QString relative = project.relativeFilePath(canonical);
    // These characters cannot be used literally in an import command.
    static const QRegularExpression unsafe(QStringLiteral(R"([{}%#\\\r\n])"));
    if (unsafe.match(relative).hasMatch()) { return i18n("This source path contains characters that LaTeX cannot import."); }
    QString name = QFileInfo(canonical).completeBaseName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    if (name.isEmpty() || name == QLatin1String("main") || name == QLatin1String("all") || name == QLatin1String("documentation_index") || name == QLatin1String("docs_combined_compact") || name.startsWith(QLatin1String("appendix_"))) { name.prepend(QStringLiteral("document_")); }
    const QDir sources(project.filePath(QStringLiteral("tex/source")));
    if (!sources.exists()) { return i18n("The project's document source folder is missing."); }
    QString filename = name + QStringLiteral(".tex");
    for (int suffix = 2; sources.exists(filename); ++suffix) { filename = name + QStringLiteral("_%1.tex").arg(suffix); }
    const QString entryPath = sources.filePath(filename);
    const QByteArray wrapper = QStringLiteral("% thoughts:standalone-import\n\\RequirePackage{import}\n\\subimport{%1/}{%2}\n")
                                   .arg(QFileInfo(relative).path(), QFileInfo(relative).fileName()).toUtf8();
    QSaveFile output(entryPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(wrapper) != wrapper.size() || !output.commit()) { return i18n("The document could not be added."); }
    return selectEntry(owner + QStringLiteral("/tex/source/") + filename);
}

QString librarySegmentKey(const QString &pdfSha256, const QJsonObject &segment)
{
    return pdfSha256 + QLatin1Char(':') + QString::number(segment.value(QStringLiteral("start_page")).toInt())
        + QLatin1Char(':') + QString::number(segment.value(QStringLiteral("page_count")).toInt());
}

QString librarySegmentKey(const Thoughts::TrackedCommentDocument &document, const QJsonObject &segment)
{
    return librarySegmentKey(document.librarySha256, segment);
}

QString librarySegmentTitle(const QJsonObject &segment)
{
    QString title = segment.value(QStringLiteral("title")).toString().trimmed();
    if (!title.isEmpty()) {
        return title;
    }
    const QString path = segment.value(QStringLiteral("primary_source_file")).toString(segment.value(QStringLiteral("source_id")).toString());
    title = QFileInfo(path).completeBaseName();
    // PDF components carry the project prefix, whereas TeX components do not.
    // A default folder's documents have no such prefix.
    const bool defaultFolder = Thoughts::SourceOwner::isDefaultFolder(Thoughts::SourceOwner::of(path));
    // A renamed folder's PDFs keep its old short name (source folder names spec, rule F).
    const QString project = Thoughts::SourceFolderNames::pdfShortName(path.section(QLatin1Char('/'), 0, 0),
                                                                      Thoughts::SourceFolderNames::knownRenames(Thoughts::thoughtsHome()));
    if (path.endsWith(QLatin1String(".pdf")) && !defaultFolder && title.startsWith(project + QLatin1Char('_'))) {
        title.remove(0, project.size() + 1);
    }
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!title.isEmpty()) {
        title[0] = title.at(0).toUpper();
    }
    return title.isEmpty() ? i18n("Document") : title;
}

// A build of several PDFs lists the failure of each; an entry without a file
// name is the build's own failure and applies to every PDF of it.
QJsonObject libraryBuildFailureFor(const QJsonArray &failedDocuments, const QString &filename)
{
    QJsonObject fallback;
    for (const QJsonValue &value : failedDocuments) {
        const QJsonObject failure = value.toObject();
        const QString named = failure.value(QStringLiteral("pdf_filename")).toString(failure.value(QStringLiteral("filename")).toString());
        if (named.compare(filename, Qt::CaseInsensitive) == 0) return failure;
        if (named.isEmpty() && fallback.isEmpty()) fallback = failure;
    }
    return fallback;
}

QString failedBuildSourcePath(const QString &message)
{
    // "latex_error: Source docs-x/tex/fragments/a.tex:115 failed: ..." or
    // "compiler_failure: Source docs-x/tex/source/main.tex failed during ...".
    static const QRegularExpression source(QStringLiteral("(?:^|\\s)Source (\\S+?\\.[A-Za-z0-9]+)(?::\\d+)?(?:\\s|$)"));
    const QRegularExpressionMatch match = source.match(message);
    return match.hasMatch() ? match.captured(1) : QString();
}

LibraryDocumentMetadata libraryDocumentMetadata(const QString &docsRoot, const QString &filename, const QString &pdfPath)
{
    QFile index(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    if (!index.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(index.readAll());
    if (!document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 1 || !document.object().value(QStringLiteral("pdfs")).isArray()) {
        return {};
    }
    const QRegularExpression safeFilename(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    for (const QJsonValue &value : document.object().value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString entryFilename = entry.value(QStringLiteral("filename")).toString();
        if (entryFilename.compare(filename, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString digest = entry.value(QStringLiteral("pdf_sha256")).toString();
        if (!safeFilename.match(entryFilename).hasMatch() || (kind != QLatin1String("source_linked") && kind != QLatin1String("unlinked")) || !sha256.match(digest).hasMatch()) {
            return {};
        }
        QString displayName = entry.value(QStringLiteral("display_name")).toString().trimmed();
        if (displayName.size() > 200 || std::any_of(displayName.cbegin(), displayName.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; })) {
            displayName.clear();
        }
        const QString revision = document.object().value(QStringLiteral("library_revision")).toString();
        const QString sourceIndexSha256 = document.object().value(QStringLiteral("source_index_sha256")).toString();
        QFile manifestFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-generation.json")));
        const QJsonObject manifest = manifestFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifestFile.readAll()).object() : QJsonObject();
        bool generationCurrent =
            !revision.isEmpty() && !sourceIndexSha256.isEmpty() && manifest.value(QStringLiteral("library_revision")).toString() == revision && manifest.value(QStringLiteral("source_index_sha256")).toString() == sourceIndexSha256;
        QJsonObject matchedPublication;
        for (const QJsonValue &publicationValue : manifest.value(QStringLiteral("publications")).toArray()) {
            const QJsonObject publication = publicationValue.toObject();
            if (publication.value(QStringLiteral("filename")).toString() == entryFilename && publication.value(QStringLiteral("sha256")).toString() == digest) {
                matchedPublication = publication;
                break;
            }
        }
        generationCurrent = generationCurrent && !matchedPublication.isEmpty();
        const bool pdfStale = entry.value(QStringLiteral("pdf_stale")).toBool();
        const QJsonObject latestBuild = entry.value(QStringLiteral("latest_build")).toObject();
        QString latestBuildMessage = latestBuild.value(QStringLiteral("error")).toString().trimmed();
        // The failure of this PDF, not the first failure of a build shared with other PDFs.
        const QJsonObject failure = libraryBuildFailureFor(latestBuild.value(QStringLiteral("failed_documents")).toArray(), entryFilename);
        if (latestBuildMessage.isEmpty()) {
            latestBuildMessage = failure.value(QStringLiteral("message")).toString(failure.value(QStringLiteral("error")).toString()).trimmed();
        }
        LibraryDocumentMetadata result;
        result.latestBuildSource = failure.value(QStringLiteral("source")).toString().trimmed();
        if (result.latestBuildSource.isEmpty()) result.latestBuildSource = failedBuildSourcePath(latestBuildMessage);
        result.filename = entryFilename;
        result.kind = kind;
        result.sha256 = digest;
        result.displayName = displayName;
        result.project = entry.value(QStringLiteral("project")).toString().trimmed();
        result.documentId = entry.value(QStringLiteral("document_id")).toString();
        result.publishedAt = publishedDate(entry.value(QStringLiteral("published_at")));
        if (!entry.value(QStringLiteral("build_id")).toString().isEmpty()) result.builtAt = result.publishedAt;
        result.pageCount = entry.value(QStringLiteral("page_count")).toInteger(-1);
        result.latestBuildMessage = latestBuildMessage;
        result.generationCurrent = generationCurrent;
        result.pdfStale = pdfStale;
        const QJsonObject metadata = matchedPublication.value(QStringLiteral("metadata")).toObject();
        if (result.pageCount < 0) result.pageCount = metadata.value(QStringLiteral("page_count")).toInteger(-1);
        if (result.project.isEmpty()) {
            result.project = matchedPublication.value(QStringLiteral("project")).toString(metadata.value(QStringLiteral("project")).toString()).trimmed();
        }
        // The building folder: a docs-* folder or a Project's default folder.
        const QRegularExpression buildProject(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$"));
        if (kind != QLatin1String("source_linked") || !(buildProject.match(result.project).hasMatch() || Thoughts::SourceOwner::isDefaultFolder(result.project))) {
            result.project.clear();
        }
        const std::optional<qint64> inputCount = nonNegativeJsonInteger(metadata.value(QStringLiteral("verified_closure_input_count")));
        const std::optional<qint64> inputBytes = nonNegativeJsonInteger(metadata.value(QStringLiteral("verified_closure_input_bytes")));
        const bool verifiedIdentity = sha256.match(metadata.value(QStringLiteral("verified_closure_sha256")).toString()).hasMatch()
            && sha256.match(metadata.value(QStringLiteral("verified_closure_object_sha256")).toString()).hasMatch();
        if (!result.project.isEmpty() && generationCurrent && metadata.value(QStringLiteral("build_input_discovery_policy")).toString() == QLatin1String("tex-closure-v1") && verifiedIdentity && inputCount && inputBytes
            && fileContentSha256(pdfPath).toHex() == digest.toLatin1()) {
            result.verifiedBuildInputCount = *inputCount;
            result.verifiedBuildInputBytes = *inputBytes;
            result.verifiedBuildInputsAvailable = true;
        }
        return result;
    }
    return {};
}

QModelIndexList modelCommentIndexes(const QAbstractItemModel *model, const QModelIndex &parent = {})
{
    QModelIndexList indexes;
    if (!model) {
        return indexes;
    }
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, CommentColumn, parent);
        indexes.append(index);
        indexes.append(modelCommentIndexes(model, index));
    }
    return indexes;
}

bool readLayout(const QString &path, QJsonObject *layout)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    *layout = document.object();
    return layout->value(QStringLiteral("version")).toInt() == 1 && !layout->value(QStringLiteral("sources")).toArray().isEmpty();
}

struct AgentProposalArtifact {
    QString filename;
    QString artifactSha256;
    QString pdfSha256;
    QJsonArray sources;
    QJsonArray proposals;
    QJsonArray reviewComments;
    QJsonArray logicalComments;
    QJsonArray reviewCorrespondences;
    bool reviewCommentsAvailable = false;
    bool logicalCommentsAvailable = false;
    bool reviewCorrespondenceAvailable = false;
};

QDir sourceBundleRoot(const QString &docsRoot)
{
    QDir root(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")));
    if (!QFileInfo::exists(root.filePath(QStringLiteral("documents")))) {
        root = QDir(QDir(docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
    }
    return root;
}

struct BundleLoadTiming {
    qint64 resolvedIdsMs = 0;
    qint64 pointersMs = 0;
    qint64 cacheMs = 0;
    qint64 verificationMs = 0;
    qint64 parsingMs = 0;
    qint64 assemblyMs = 0;
    qint64 filteringMs = 0;
    qint64 objectBytes = 0;
    int cacheHits = 0;
    int cacheMisses = 0;
};

// How many PDFs' assembled proposal artifacts are kept in memory.
constexpr int MaximumKeptProposalArtifacts = 1024;

QList<AgentProposalArtifact> readAgentProposalArtifacts(const QString &docsRoot, const QString &onlyFilename = {},
                                                       BundleLoadTiming *timing = nullptr)
{
    // One assembled artifact per PDF: the artifact of a rebuilt PDF replaces
    // the one before it.
    struct KeptArtifact {
        QString key;
        AgentProposalArtifact artifact;
    };
    static QMutex cacheMutex;
    static QCache<QString, KeptArtifact> cache(MaximumKeptProposalArtifacts);
    QElapsedTimer elapsed;
    elapsed.start();
    const QDir root = sourceBundleRoot(docsRoot);
    const QSet<QString> locallyResolved = Thoughts::locallyResolvedAgentProposalIds(root.absolutePath());
    if (timing) timing->resolvedIdsMs = elapsed.restart();
    const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    QList<AgentProposalArtifact> artifacts;
    const QDir documents(root.filePath(QStringLiteral("documents")));
    for (const QFileInfo &pointerInfo : documents.entryInfoList({QStringLiteral("*.pdf.json")}, QDir::Files, QDir::Name)) {
        elapsed.restart();
        if (!onlyFilename.isEmpty() && pointerInfo.fileName().compare(onlyFilename + QStringLiteral(".json"), Qt::CaseInsensitive) != 0) { continue; }
        QFile pointerFile(pointerInfo.absoluteFilePath());
        if (!pointerFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject pointer = QJsonDocument::fromJson(pointerFile.readAll()).object();
        const QString artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
        const QString filename = pointer.value(QStringLiteral("filename")).toString();
        const QString pdfSha256 = pointer.value(QStringLiteral("pdf_sha256")).toString();
        if (pointer.value(QStringLiteral("schema_version")).toInt() != 1 || !sha256.match(artifactSha256).hasMatch() || !sha256.match(pdfSha256).hasMatch()
            || filename.isEmpty() || pointerInfo.fileName() != filename + QStringLiteral(".json")) {
            continue;
        }
        const QString cacheKey = Thoughts::artifactFileIdentity(root.filePath(QStringLiteral("objects/%1").arg(artifactSha256))) + artifactSha256 + pdfSha256 + filename;
        if (timing) timing->pointersMs += elapsed.restart();
        bool cacheHit = false;
        {
            QMutexLocker lock(&cacheMutex);
            if (const auto *cached = cache.object(filename); cached && cached->key == cacheKey) {
                artifacts.append(cached->artifact);
                cacheHit = true;
            }
        }
        if (timing) timing->cacheMs += elapsed.restart();
        if (cacheHit) {
            if (timing) ++timing->cacheHits;
            continue;
        }
        if (timing) ++timing->cacheMisses;
        // Verified and parsed; the artifact's bytes are not kept.
        const QString artifactPath = root.filePath(QStringLiteral("objects/%1").arg(artifactSha256));
        const QJsonObject manifest = Thoughts::verifiedSourceArtifactJson(artifactPath, artifactSha256, filename);
        if (manifest.isEmpty()) {
            continue;
        }
        if (timing) {
            timing->verificationMs += elapsed.restart();
            timing->objectBytes += QFileInfo(artifactPath).size();
        }
        const QJsonObject artifactPdf = manifest.value(QStringLiteral("pdf")).toObject();
        if (manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")
            || manifest.value(QStringLiteral("filename")).toString() != filename
            || artifactPdf.value(QStringLiteral("filename")).toString() != filename
            || artifactPdf.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256) {
            continue;
        }
        if (timing) timing->parsingMs += elapsed.restart();
        QHash<QString, QString> proposalObjects;
        for (const QJsonValue &value : manifest.value(QStringLiteral("proposal_objects")).toArray()) {
            const QJsonObject reference = value.toObject();
            proposalObjects.insert(reference.value(QStringLiteral("id")).toString(), reference.value(QStringLiteral("object")).toString());
        }
        AgentProposalArtifact artifact;
        artifact.filename = filename;
        artifact.artifactSha256 = artifactSha256;
        artifact.pdfSha256 = pdfSha256;
        artifact.sources = manifest.value(QStringLiteral("sources")).toArray();
        QHash<QString, QJsonObject> correspondenceByKey;
        if (manifest.value(QStringLiteral("review_correspondence_schema_version")).toInt() == 2) {
            artifact.reviewCorrespondenceAvailable = true;
            for (const QJsonValue &value : manifest.value(QStringLiteral("review_correspondences")).toArray()) {
                QJsonObject correspondence = value.toObject();
                if (manifest.value(QStringLiteral("review_placement_model_version")).toInt() >= 3
                    && (correspondence.value(QStringLiteral("state")).toString().startsWith(QLatin1String("exact_")) || !correspondence.value(QStringLiteral("locations")).toArray().isEmpty())
                    && !Thoughts::validCompilerReviewCorrespondence(correspondence, filename, pdfSha256, artifactPdf.value(QStringLiteral("page_count")).toInt())) {
                    correspondence.insert(QStringLiteral("locations"), QJsonArray());
                    correspondence.insert(QStringLiteral("state"), QStringLiteral("needs_attention"));
                    correspondence.insert(QStringLiteral("reason"), QStringLiteral("invalid_compiler_placement"));
                }
                const QString reviewKey = correspondence.value(QStringLiteral("review_key")).toString();
                if (correspondence.value(QStringLiteral("schema_version")).toInt() != 2 || reviewKey.isEmpty()) {
                    continue;
                }
                correspondenceByKey.insert(reviewKey, correspondence);
                artifact.reviewCorrespondences.append(correspondence);
            }
        }
        if (manifest.value(QStringLiteral("review_comments_schema_version")).toInt() == 1) {
            artifact.reviewCommentsAvailable = true;
            artifact.reviewComments = manifest.value(QStringLiteral("review_comments")).toArray();
        }
        if (manifest.value(QStringLiteral("logical_comments_schema_version")).toInt() == 1) {
            artifact.logicalCommentsAvailable = true;
            artifact.logicalComments = manifest.value(QStringLiteral("logical_comments")).toArray();
        }
        for (const QJsonValue &value : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
            QJsonObject proposal = value.toObject();
            const QString objectSha256 = proposalObjects.value(proposal.value(QStringLiteral("id")).toString());
            if (!sha256.match(objectSha256).hasMatch()) {
                continue;
            }
            proposal.insert(QStringLiteral("artifact_sha256"), artifactSha256);
            proposal.insert(QStringLiteral("proposal_object_sha256"), objectSha256);
            proposal.insert(QStringLiteral("artifact_pdf_sha256"), pdfSha256);
            const QJsonObject correspondence = correspondenceByKey.value(
                QStringLiteral("proposal:") + proposal.value(QStringLiteral("id")).toString());
            if (!correspondence.isEmpty()) {
                proposal.insert(QStringLiteral("review_correspondence"), correspondence);
                QHash<QString, QJsonArray> fragmentsByRole;
                QJsonArray points;
                for (const QJsonValue &locationValue : correspondence.value(QStringLiteral("locations")).toArray()) {
                    const QJsonObject location = locationValue.toObject();
                    if (location.value(QStringLiteral("pdf_filename")).toString().compare(filename, Qt::CaseInsensitive) != 0
                        || location.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256) {
                        continue;
                    }
                    const QString role = location.value(QStringLiteral("role")).toString(QStringLiteral("after"));
                    for (const QJsonValue &regionValue : location.value(QStringLiteral("regions")).toArray()) {
                        const QJsonArray region = regionValue.toArray();
                        if (region.size() != 4) {
                            continue;
                        }
                        QJsonObject fragment {
                            {QStringLiteral("pdf_filename"), filename},
                            {QStringLiteral("page"), location.value(QStringLiteral("page"))},
                            {QStringLiteral("left"), region.at(0)},
                            {QStringLiteral("top"), region.at(1)},
                            {QStringLiteral("right"), region.at(2)},
                            {QStringLiteral("bottom"), region.at(3)},
                        };
                        if (location.value(QStringLiteral("duplicate")).toBool()) {
                            fragment.insert(QStringLiteral("duplicate"), true);
                        }
                        fragmentsByRole[role].append(fragment);
                    }
                    if (location.value(QStringLiteral("point")).toArray().size() == 2) {
                        points.append(location);
                    }
                }
                QJsonArray regionGroups;
                QJsonArray primaryFragments;
                for (auto iterator = fragmentsByRole.cbegin(); iterator != fragmentsByRole.cend(); ++iterator) {
                    regionGroups.append(QJsonObject {
                        {QStringLiteral("role"), iterator.key()},
                        {QStringLiteral("quad_fragments"), iterator.value()},
                    });
                    if (iterator.key() == QLatin1String("after") || primaryFragments.isEmpty()) {
                        primaryFragments = iterator.value();
                    }
                }
                const QString state = correspondence.value(QStringLiteral("state")).toString();
                proposal.insert(QStringLiteral("placement"), QJsonObject {
                    {QStringLiteral("status"), state == QLatin1String("exact_text") || state == QLatin1String("exact_point") ? QStringLiteral("anchored") : QStringLiteral("source_only")},
                    {QStringLiteral("quad_fragments"), primaryFragments},
                    {QStringLiteral("regions"), regionGroups},
                    {QStringLiteral("points"), points},
                });
            }
            artifact.proposals.append(proposal);
        }
        if (timing) timing->assemblyMs += elapsed.restart();
        {
            QMutexLocker lock(&cacheMutex);
            cache.insert(filename, new KeptArtifact {cacheKey, artifact}, 1);
        }
        if (timing) timing->cacheMs += elapsed.restart();
        artifacts.append(artifact);
    }
    elapsed.restart();
    for (auto &artifact : artifacts) {
        QJsonArray visible;
        for (const auto &value : artifact.proposals) {
            if (!locallyResolved.contains(value.toObject().value(QStringLiteral("id")).toString())) visible.append(value);
        }
        artifact.proposals = visible;
    }
    if (timing) timing->filteringMs = elapsed.elapsed();
    return artifacts;
}

QJsonObject reviewCorrespondence(const AgentProposalArtifact &artifact, const QString &reviewKey)
{
    for (const QJsonValue &value : artifact.reviewCorrespondences) {
        const QJsonObject correspondence = value.toObject();
        if (correspondence.value(QStringLiteral("review_key")).toString() == reviewKey) {
            return correspondence;
        }
    }
    return {};
}

// The first matching PDF location that is the source's own occurrence. LaTeX-made
// copies (contents entries, running heads) are marked "duplicate" and are only
// used when every match is one.
template<typename Predicate>
QJsonObject firstPrimaryPdfLocation(const QJsonArray &locations, Predicate matches)
{
    QJsonObject first;
    for (const QJsonValue &value : locations) {
        const QJsonObject location = value.toObject();
        if (!matches(location)) {
            continue;
        }
        if (!location.value(QStringLiteral("duplicate")).toBool()) {
            return location;
        }
        if (first.isEmpty()) {
            first = location;
        }
    }
    return first;
}

QJsonArray compatiblePdfLocations(const QJsonObject &correspondence)
{
    QJsonArray result;
    for (const QJsonValue &value : correspondence.value(QStringLiteral("locations")).toArray()) {
        const QJsonObject v2 = value.toObject();
        QJsonObject location {
            {QStringLiteral("pdf_filename"), v2.value(QStringLiteral("pdf_filename"))},
            {QStringLiteral("pdf_sha256"), v2.value(QStringLiteral("pdf_sha256"))},
            {QStringLiteral("page"), v2.value(QStringLiteral("page"))},
            {QStringLiteral("status"), QStringLiteral("exact")},
            {QStringLiteral("point"), v2.value(QStringLiteral("point"))},
            {QStringLiteral("regions"), v2.value(QStringLiteral("regions"))},
            {QStringLiteral("proof"), v2.value(QStringLiteral("proof"))},
            {QStringLiteral("placement_kind"), v2.value(QStringLiteral("placement_kind"))},
        };
        if (v2.value(QStringLiteral("duplicate")).toBool()) {
            location.insert(QStringLiteral("duplicate"), true);
        }
        result.append(location);
    }
    return result;
}

void applyReviewCorrespondence(Thoughts::TrackedComment &comment,
                               const AgentProposalArtifact &artifact,
                               const QString &reviewKey)
{
    const QJsonObject correspondence = reviewCorrespondence(artifact, reviewKey);
    if (correspondence.isEmpty()) {
        return;
    }
    comment.correspondence = correspondence;
    comment.pdfLocations = compatiblePdfLocations(correspondence);
    const QString state = correspondence.value(QStringLiteral("state")).toString();
    if (comment.agentProposal) {
        comment.anchorStatus = state == QLatin1String("exact_text") || state == QLatin1String("exact_point")
            ? QStringLiteral("anchored")
            : state == QLatin1String("needs_attention") || state == QLatin1String("linking")
            ? QStringLiteral("needs_attention")
            : QStringLiteral("source_only");
    } else {
        comment.anchorStatus = state == QLatin1String("exact_text") || state == QLatin1String("exact_point") || state == QLatin1String("source_only")
            ? QStringLiteral("source_linked")
            : state == QLatin1String("needs_attention")
            ? QStringLiteral("needs_attention")
            : QStringLiteral("pdf_only");
    }
    comment.sourceOnly = state == QLatin1String("source_only");
    const QJsonArray targets = (correspondence.value(QStringLiteral("current_source_targets")).toArray().isEmpty() ? correspondence.value(QStringLiteral("source_targets")).toArray() : correspondence.value(QStringLiteral("current_source_targets")).toArray());
    if (!targets.isEmpty()) {
        QJsonObject target = targets.first().toObject();
        for (const QJsonValue &value : targets) {
            if (value.toObject().value(QStringLiteral("role")).toString() == (comment.agentProposal ? QLatin1String("after") : QLatin1String("target"))) {
                target = value.toObject();
                break;
            }
        }
        comment.sourceFile = target.value(QStringLiteral("source_file")).toString(comment.sourceFile);
        comment.sourceLine = target.value(QStringLiteral("line_start")).toInt(comment.sourceLine);
    }
    const QJsonObject location = firstPrimaryPdfLocation(comment.pdfLocations, [&artifact](const QJsonObject &candidate) {
        return candidate.value(QStringLiteral("pdf_filename")).toString().compare(artifact.filename, Qt::CaseInsensitive) == 0
            && candidate.value(QStringLiteral("pdf_sha256")).toString() == artifact.pdfSha256;
    });
    if (!location.isEmpty()) {
        comment.page = location.value(QStringLiteral("page")).toInt(comment.page);
    }
}

void mergeLogicalComments(Thoughts::TrackedCommentDocument &document, const AgentProposalArtifact &artifact)
{
    if (!artifact.logicalCommentsAvailable) {
        return;
    }
    document.comments.removeIf([](const Thoughts::TrackedComment &comment) { return !comment.agentProposal; });
    QHash<QString, QJsonObject> originsById;
    for (const QJsonValue &value : artifact.logicalComments) {
        const auto payload = value.toObject();
        originsById.insert(payload.value(QStringLiteral("comment_id")).toString(), payload.value(QStringLiteral("origin")).toObject());
    }
    for (const QJsonValue &value : artifact.logicalComments) {
        const QJsonObject payload = value.toObject();
        const QString id = payload.value(QStringLiteral("comment_id")).toString().trimmed();
        if (id.isEmpty() || payload.value(QStringLiteral("schema_version")).toInt() != 1 || payload.value(QStringLiteral("deleted")).toBool()) {
            continue;
        }
        const QString rootId = payload.value(QStringLiteral("thread_root_id")).toString(id);
        QJsonObject correspondence;
        for (const QJsonValue &candidate : artifact.reviewCorrespondences) {
            if (candidate.toObject().value(QStringLiteral("review_key")).toString() == QStringLiteral("comment:") + rootId) {
                correspondence = candidate.toObject();
                break;
            }
        }
        QJsonArray matchingLocations;
        for (const QJsonValue &value : correspondence.value(QStringLiteral("locations")).toArray()) {
            QJsonObject location = value.toObject();
            if (location.value(QStringLiteral("pdf_filename")).toString().compare(artifact.filename, Qt::CaseInsensitive) == 0
                && location.value(QStringLiteral("pdf_sha256")).toString() == artifact.pdfSha256) {
                location.insert(QStringLiteral("status"), QStringLiteral("exact"));
                matchingLocations.append(location);
            }
        }
        const QJsonObject origin = originsById.value(rootId, payload.value(QStringLiteral("origin")).toObject());
        const bool originDocument = origin.value(QStringLiteral("collection")).toString().compare(document.stem, Qt::CaseInsensitive) == 0;
        if (matchingLocations.isEmpty() && !originDocument) {
            continue;
        }
        auto existing = std::find_if(document.comments.begin(), document.comments.end(), [&id](const Thoughts::TrackedComment &comment) {
            return !comment.agentProposal && comment.id == id;
        });
        if (existing == document.comments.end()) {
            Thoughts::TrackedComment comment;
            comment.id = id;
            document.comments.append(comment);
            existing = std::prev(document.comments.end());
        }
        Thoughts::TrackedComment &comment = *existing;
        comment.documentStem = document.stem;
        comment.pdfPath = document.pdfPath;
        comment.pdfAvailable = document.pdfAvailable;
        comment.parentId = payload.value(QStringLiteral("parent_id")).toString();
        comment.threadRootId = payload.value(QStringLiteral("thread_root_id")).toString(id);
        comment.contents = payload.value(QStringLiteral("contents")).toString();
        comment.selectedText = payload.value(QStringLiteral("selected_text")).toString();
        comment.author = payload.value(QStringLiteral("author")).toString();
        comment.sourceFile = payload.value(QStringLiteral("source_file")).toString(payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_file")).toString(comment.sourceFile));
        comment.sourceLine = payload.value(QStringLiteral("source_line")).toInt(payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_line")).toInt(comment.sourceLine));
        comment.priorityRaw = payload.value(QStringLiteral("priority")).toString(QStringLiteral("normal"));
        comment.chatGptUrl = payload.value(QStringLiteral("chatgpt_url")).toString();
        comment.logicalPayload = payload;
        comment.correspondence = correspondence;
        const QJsonArray sourceTargets = correspondence.value(QStringLiteral("current_source_targets")).toArray().isEmpty()
            ? correspondence.value(QStringLiteral("source_targets")).toArray()
            : correspondence.value(QStringLiteral("current_source_targets")).toArray();
        if (!sourceTargets.isEmpty() && payload.value(QStringLiteral("source_authority")).toString() != QLatin1String("client-v1")) {
            comment.sourceFile = sourceTargets.first().toObject().value(QStringLiteral("source_file")).toString();
            comment.sourceLine = sourceTargets.first().toObject().value(QStringLiteral("line_start")).toInt();
        }
        comment.pdfLocations = matchingLocations;
        comment.logicalComment = true;
        const QString scope = payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString();
        comment.anchorStatus = Thoughts::isAnchorRemovedScope(payload.value(QStringLiteral("scope")).toObject()) ? QStringLiteral("anchor_removed")
                             : scope == QLatin1String("source_linked") ? QStringLiteral("source_linked")
                                                                     : scope == QLatin1String("needs_attention") ? QStringLiteral("needs_attention")
                                                                                                                    : QStringLiteral("pdf_only");
        if (!matchingLocations.isEmpty()) {
            comment.page = matchingLocations.at(0).toObject().value(QStringLiteral("page")).toInt();
        } else {
            comment.page = 0;
        }
        comment.displayDate = commentDisplayDate(payload);
        comment.rootSortDate = rootCommentSortDate(payload);
    }
}

struct LogicalConsolidationTiming {
    qint64 pdfHashMs = 0;
    qint64 pdfBytes = 0;
    int pdfHashCount = 0;
    int projectionCount = 0;
};

void consolidateLogicalComments(QList<Thoughts::TrackedCommentDocument> &documents, const QJsonObject &fedoraProjections,
                                LogicalConsolidationTiming *timing = nullptr)
{
    QHash<QString, QJsonObject> projectionById;
    const QString prefix = QStringLiteral("logical_comment\ncomments\n");
    for (auto iterator = fedoraProjections.constBegin(); iterator != fedoraProjections.constEnd(); ++iterator) {
        if (!iterator.key().startsWith(prefix) || !iterator.value().isObject()) {
            continue;
        }
        const QString id = iterator.key().mid(prefix.size());
        const QJsonObject payload = iterator.value().toObject();
        if (payload.value(QStringLiteral("comment_id")).toString() == id) {
            projectionById.insert(id, payload);
        }
    }
    if (timing) timing->projectionCount = projectionById.size();

    QHash<QString, Thoughts::TrackedComment> logicalById;
    QHash<QString, QString> fallbackStem;
    for (Thoughts::TrackedCommentDocument &document : documents) {
        for (const Thoughts::TrackedComment &candidate : std::as_const(document.comments)) {
            if (!candidate.logicalComment) {
                continue;
            }
            Thoughts::TrackedComment merged = logicalById.value(candidate.id, candidate);
            if (!logicalById.contains(candidate.id)) {
                fallbackStem.insert(candidate.id, document.stem);
            }
            QSet<QString> locationKeys;
            QJsonArray locations;
            const auto appendLocations = [&locationKeys, &locations](const QJsonArray &values) {
                for (const QJsonValue &value : values) {
                    const QJsonObject location = value.toObject();
                    const QString key = QStringLiteral("%1\n%2\n%3")
                                            .arg(location.value(QStringLiteral("pdf_filename")).toString(),
                                                 location.value(QStringLiteral("pdf_sha256")).toString())
                                            .arg(location.value(QStringLiteral("page")).toInt());
                    if (!locationKeys.contains(key)) {
                        locationKeys.insert(key);
                        locations.append(location);
                    }
                }
            };
            appendLocations(merged.pdfLocations);
            appendLocations(candidate.pdfLocations);
            merged.pdfLocations = locations;
            logicalById.insert(candidate.id, merged);
        }
        document.comments.removeIf([&projectionById](const Thoughts::TrackedComment &comment) { return comment.logicalComment || (!comment.agentProposal && projectionById.contains(comment.id)); });
    }

    for (auto iterator = projectionById.constBegin(); iterator != projectionById.constEnd(); ++iterator) {
        const QJsonObject payload = iterator.value();
        const QString rootId = payload.value(QStringLiteral("thread_root_id")).toString(iterator.key());
        if (payload.value(QStringLiteral("deleted")).toBool() || projectionById.value(rootId).value(QStringLiteral("deleted")).toBool()) {
            logicalById.remove(iterator.key());
            continue;
        }
        Thoughts::TrackedComment comment = logicalById.value(iterator.key());
        if (comment.id.isEmpty()) {
            comment.id = iterator.key();
            comment.logicalComment = true;
            const QJsonObject origin = payload.value(QStringLiteral("origin")).toObject();
            fallbackStem.insert(iterator.key(), origin.value(QStringLiteral("collection")).toString());
            comment.page = 0;
        }
        comment.logicalPayload = payload;
        comment.parentId = payload.value(QStringLiteral("parent_id")).toString();
        comment.threadRootId = payload.value(QStringLiteral("thread_root_id")).toString(iterator.key());
        comment.contents = payload.value(QStringLiteral("contents")).toString();
        comment.selectedText = payload.value(QStringLiteral("selected_text")).toString();
        comment.author = payload.value(QStringLiteral("author")).toString();
        comment.sourceFile = payload.value(QStringLiteral("source_file"))
                                 .toString(payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_file")).toString(comment.sourceFile));
        comment.sourceLine = payload.value(QStringLiteral("source_line"))
                                 .toInt(payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_line")).toInt(comment.sourceLine));
        comment.priorityRaw = payload.value(QStringLiteral("priority")).toString(QStringLiteral("normal"));
        comment.chatGptUrl = payload.value(QStringLiteral("chatgpt_url")).toString();
        const QString scope = payload.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString();
        comment.anchorStatus = Thoughts::isAnchorRemovedScope(payload.value(QStringLiteral("scope")).toObject()) ? QStringLiteral("anchor_removed")
                             : scope == QLatin1String("source_linked") ? QStringLiteral("source_linked")
                                                                     : scope == QLatin1String("needs_attention") ? QStringLiteral("needs_attention")
                                                                                                                    : QStringLiteral("pdf_only");
        comment.displayDate = commentDisplayDate(payload);
        comment.rootSortDate = rootCommentSortDate(payload);
        if (payload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note")) {
            comment.anchorStatus = QStringLiteral("standalone");
            comment.page = 0;
            comment.pdfLocations = {};
        }
        logicalById.insert(iterator.key(), comment);
    }

    const bool hasNotes = std::any_of(logicalById.cbegin(), logicalById.cend(), [](const Thoughts::TrackedComment &comment) {
        return comment.logicalPayload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note");
    });
    if (hasNotes) {
        Thoughts::TrackedCommentDocument notes;
        notes.displayName = i18n("No project");
        documents.append(notes);
    }

    QHash<QString, int> documentByStem;
    QHash<QString, int> documentByFilename;
    for (int index = 0; index < documents.size(); ++index) {
        documentByStem.insert(documents.at(index).stem.toCaseFolded(), index);
        documentByFilename.insert(QFileInfo(documents.at(index).pdfPath).fileName().toCaseFolded(), index);
    }
    QHash<QString, QByteArray> originPdfHashes;
    for (auto iterator = logicalById.begin(); iterator != logicalById.end(); ++iterator) {
        Thoughts::TrackedComment &comment = iterator.value();
        const auto root = logicalById.constFind(comment.threadRootId);
        const QJsonObject origin = (root == logicalById.cend() ? comment.logicalPayload : root->logicalPayload)
                                       .value(QStringLiteral("origin")).toObject();
        int documentIndex = documentByStem.value(origin.value(QStringLiteral("collection")).toString().toCaseFolded(), -1);
        if (documentIndex < 0 && !comment.pdfLocations.isEmpty()) {
            documentIndex = documentByFilename.value(comment.pdfLocations.at(0).toObject().value(QStringLiteral("pdf_filename")).toString().toCaseFolded(), -1);
        }
        if (documentIndex < 0) {
            documentIndex = documentByStem.value(fallbackStem.value(comment.id).toCaseFolded(), -1);
        }
        if (documentIndex < 0) {
            continue;
        }
        Thoughts::TrackedCommentDocument &document = documents[documentIndex];
        comment.documentStem = document.stem;
        comment.pdfPath = document.pdfPath;
        comment.pdfAvailable = document.pdfAvailable;
        if (comment.pdfLocations.isEmpty()) {
            // PDF-only comments need no published source correspondence. Their
            // captured page is still usable on the identical PDF, including
            // replies, which share their thread root's placement.
            comment.page = 0;
            const QString originHash = origin.value(QStringLiteral("pdf_sha256")).toString();
            if (document.pdfAvailable && !originHash.isEmpty()
                && origin.value(QStringLiteral("collection")).toString().compare(document.stem, Qt::CaseInsensitive) == 0
                && comment.logicalPayload.value(QStringLiteral("subtype")).toString() != QLatin1String("Note")) {
                if (!originPdfHashes.contains(document.pdfPath)) {
                    QElapsedTimer hashElapsed;
                    hashElapsed.start();
                    originPdfHashes.insert(document.pdfPath, fileContentSha256(document.pdfPath).toHex());
                    if (timing) {
                        timing->pdfHashMs += hashElapsed.elapsed();
                        timing->pdfBytes += QFileInfo(document.pdfPath).size();
                        ++timing->pdfHashCount;
                    }
                }
                if (originPdfHashes.value(document.pdfPath) == originHash.toLatin1()) {
                    const int pageIndex = origin.value(QStringLiteral("page_index")).toInt(-1);
                    if (pageIndex >= 0 && pageIndex < std::numeric_limits<int>::max()) {
                        comment.page = pageIndex + 1;
                    }
                }
            }
        }
        document.comments.append(comment);
    }
}

// Markers whose comment no longer exists (repository overlay, docs/comment-missing.md):
// one Issues row each, on the document its source builds.
void appendMissingCommentMarkers(QList<Thoughts::TrackedCommentDocument> &documents, const QJsonObject &fedoraProjections)
{
    const QJsonArray markers = fedoraProjections.value(Thoughts::commentMissingProjectionKey()).toObject().value(QStringLiteral("markers")).toArray();
    for (const QJsonValue &value : markers) {
        const QJsonObject marker = value.toObject();
        const QString anchorId = marker.value(QStringLiteral("anchor_id")).toString();
        const QString sourceFile = marker.value(QStringLiteral("source_file")).toString();
        const int line = marker.value(QStringLiteral("line")).toInt();
        if (anchorId.isEmpty() || sourceFile.isEmpty()) continue;
        int documentIndex = -1;
        for (const QJsonValue &target : marker.value(QStringLiteral("targets")).toArray()) {
            const QString filename = target.toString();
            for (int index = 0; index < documents.size() && documentIndex < 0; ++index) {
                const auto &document = documents.at(index);
                if (document.libraryFilename.compare(filename, Qt::CaseInsensitive) == 0
                    || (!document.pdfPath.isEmpty() && QFileInfo(document.pdfPath).fileName().compare(filename, Qt::CaseInsensitive) == 0)
                    || (!document.stem.isEmpty() && document.stem.compare(QFileInfo(filename).completeBaseName(), Qt::CaseInsensitive) == 0)) documentIndex = index;
            }
            if (documentIndex >= 0) break;
        }
        // A source whose document is not in the Library: listed with "No project" rows (by file:line).
        for (int index = 0; index < documents.size() && documentIndex < 0; ++index) {
            if (documents.at(index).stem.isEmpty() && documents.at(index).pdfPath.isEmpty()) documentIndex = index;
        }
        if (documentIndex < 0) {
            Thoughts::TrackedCommentDocument local;
            local.displayName = i18n("No project");
            documents.append(local);
            documentIndex = documents.size() - 1;
        }
        Thoughts::TrackedCommentDocument &document = documents[documentIndex];
        Thoughts::TrackedComment comment;
        comment.id = QStringLiteral("comment-missing:%1:%2:%3").arg(sourceFile).arg(line).arg(anchorId);
        comment.threadRootId = comment.id;
        comment.documentStem = document.stem;
        comment.pdfPath = document.pdfPath;
        comment.pdfAvailable = false;
        comment.contents = marker.value(QStringLiteral("contents")).toString();
        comment.selectedText = marker.value(QStringLiteral("selected_text")).toString();
        comment.sourceFile = sourceFile;
        comment.sourceLine = line;
        comment.anchorStatus = QStringLiteral("comment_missing");
        comment.priorityRaw = QStringLiteral("normal");
        comment.sourceOnly = true;
        comment.missingMarker = marker;
        document.comments.append(comment);
    }
}

QString canonicalTexSourcePath(const QString &docsRoot, const QString &storedSource)
{
    const QFileInfo storedInfo(storedSource);
    const QString candidatePath = QDir::cleanPath(storedInfo.isAbsolute() ? storedSource : QDir(docsRoot).absoluteFilePath(storedSource));
    const QFileInfo candidateInfo(candidatePath);
    if (!candidateInfo.isFile() || !Thoughts::isAgentProposalSourcePath(candidatePath)) {
        return {};
    }
    QString canonicalRoot = QFileInfo(docsRoot).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        canonicalRoot = QDir(docsRoot).absolutePath();
    }
    canonicalRoot = QDir::cleanPath(canonicalRoot);
    const QString canonicalSource = candidateInfo.canonicalFilePath();
    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    if (canonicalSource.isEmpty() || !canonicalSource.startsWith(rootPrefix)) return {};
    const QString relative = QDir(canonicalRoot).relativeFilePath(canonicalSource);
    const QString owner = Thoughts::SourceOwner::of(relative);
    if (!owner.isEmpty()) {
        const Thoughts::ThoughtIgnore ignored(QDir(canonicalRoot).filePath(owner));
        if (!ignored.error().isEmpty() || ignored.excludes(relative.mid(owner.size() + 1))) return {};
    }
    return canonicalSource;
}

struct LiveProjectProposals {
    QVector<Thoughts::AgentProposedEdit> proposals;
    // Broken wrappers: each is listed with its issue; the other proposals stay decidable.
    QVector<Thoughts::AgentProposalIssue> issues;
    QString sha256;
    QString error;
    bool valid = false;
};

// A broken wrapper as a row of its own. Its row ID is local to the source
// position: the wrapper's own ID may be missing, invalid or duplicated.
Thoughts::TrackedComment proposalIssueComment(const Thoughts::AgentProposalIssue &issue, const QString &canonicalSource, const QString &sourceFile,
                                    const QString &sourceSha256)
{
    Thoughts::TrackedComment comment;
    comment.id = QStringLiteral("proposal-issue:%1:%2").arg(canonicalSource).arg(issue.offset);
    comment.threadRootId = comment.id;
    comment.contents = i18n("This proposal cannot be accepted or rejected until its source is fixed: %1", Thoughts::agentProposalIssueMessage(issue));
    comment.proposalKind = QStringLiteral("edit");
    comment.proposalIssue = issue.code;
    comment.proposalSourceSha256 = sourceSha256;
    comment.selectedText = issue.hasId && !issue.id.isEmpty() ? i18n("Broken proposal %1 (%2)", issue.id, issue.code) : i18n("Broken proposal (%1)", issue.code);
    comment.author = QStringLiteral("Agent");
    comment.sourceFile = sourceFile;
    comment.sourceLine = issue.line;
    comment.anchorStatus = QStringLiteral("source_only");
    comment.priorityRaw = QStringLiteral("normal");
    comment.agentProposal = true;
    comment.liveSourceProposal = true;
    return comment;
}

LiveProjectProposals readLiveProjectProposals(const QString &path)
{
    static QMutex mutex;
    static QCache<QString, LiveProjectProposals> cache(16 * 1024);
    const QString key = Thoughts::artifactFileIdentity(path);
    {
        QMutexLocker lock(&mutex);
        if (!key.isEmpty()) {
            if (const auto *cached = cache.object(key)) { return *cached; }
        }
    }
    // Parse outside the shared cache lock: another project's reader can proceed.
    QFile file(path);
    LiveProjectProposals result;
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    const QByteArray contents = file.readAll();
    const bool tex = QFileInfo(path).suffix().compare(QLatin1String("tex"), Qt::CaseInsensitive) == 0;
    const QByteArray marker = tex ? QByteArrayLiteral("\\agentProposedEdit") : QByteArrayLiteral("<!-- thoughts:agent-proposal:");
    if (!contents.contains(marker)) {
        result.valid = true;
        if (!key.isEmpty() && Thoughts::artifactFileIdentity(path) == key) {
            QMutexLocker lock(&mutex);
            cache.insert(key, new LiveProjectProposals(result), 1);
        }
        return result;
    }
    result.sha256 = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const Thoughts::AgentProposalParse parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(contents));
    result.proposals = parsed.proposals;
    result.issues = parsed.issues;
    result.valid = true;
    if (!key.isEmpty() && Thoughts::artifactFileIdentity(path) == key) {
        QMutexLocker lock(&mutex);
        cache.insert(key, new LiveProjectProposals(result), qMax(1, int(contents.size() / 1024) * 3));
    }
    return result;
}

struct ConnectedProposalSource {
    QString sourceFile;
    QString projectId;
    QString projectName;
    QString target;
};

struct DiscoveredProposalSource {
    ConnectedProposalSource owner;
    LiveProjectProposals live;
};

QHash<QString, DiscoveredProposalSource> cachedProposalDiscovery(const QString &docsRoot,
                                                                  const QHash<QString, DiscoveredProposalSource> *replacement = nullptr)
{
    static QMutex mutex;
    static QHash<QString, QHash<QString, DiscoveredProposalSource>> cache;
    QMutexLocker guard(&mutex);
    if (replacement) {
        if (cache.size() >= 8 && !cache.contains(docsRoot)) cache.clear();
        cache.insert(docsRoot, *replacement);
    }
    return cache.value(docsRoot);
}

QHash<QString, ConnectedProposalSource> connectedProposalSources(const QString &docsRoot)
{
    QHash<QString, ConnectedProposalSource> connected;
    QSet<QString> ambiguous;
    const QJsonArray projects = Thoughts::LibraryProjectStore(docsRoot).projects(false);
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject project = projectValue.toObject();
        const QString projectId = project.value(QStringLiteral("id")).toString();
        if (projectId.isEmpty()) continue;
        QSet<QString> owners;
        for (const QJsonValue &folderValue : project.value(QStringLiteral("source_folders")).toArray()) {
            owners.insert(folderValue.toObject().value(QStringLiteral("project")).toString());
        }
        for (const auto &folder : project.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("source_folders")).toArray())
            owners.insert(folder.toObject().value(QStringLiteral("path")).toString());
        owners.remove(QString());
        for (const QString &owner : std::as_const(owners)) {
            if (QDir::isAbsolutePath(owner) || owner.split(QLatin1Char('/')).contains(QStringLiteral(".."))) continue;
            const QDir sourceRoot(QDir(docsRoot).filePath(owner));
            const QFileInfo rootInfo(sourceRoot.absolutePath());
            if (!rootInfo.isDir() || rootInfo.isSymLink()) continue;
            const Thoughts::ThoughtIgnore ignored(sourceRoot.absolutePath());
            if (!ignored.error().isEmpty()) continue;
            QStringList recordedSources;
            QStringList directories {sourceRoot.absolutePath()};
            while (!directories.isEmpty()) {
                const QDir directory(directories.takeLast());
                for (const auto &entry : directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
                    if (entry.isSymLink() || entry.fileName() == QLatin1String(".git") || entry.fileName() == QLatin1String(".runtime")) continue;
                    const QString relative = sourceRoot.relativeFilePath(entry.absoluteFilePath());
                    if (ignored.excludes(relative, entry.isDir())) continue;
                    if (entry.isDir()) directories.append(entry.absoluteFilePath());
                    else if (Thoughts::isAgentProposalSourcePath(entry.absoluteFilePath()))
                        recordedSources.append(QDir(docsRoot).relativeFilePath(entry.absoluteFilePath()));
                }
            }
            for (const QString &sourceFile : std::as_const(recordedSources)) {
                    const QFileInfo file(QDir(docsRoot).filePath(sourceFile));
                    const QString relative = sourceRoot.relativeFilePath(file.absoluteFilePath());
                    if (!file.isFile() || file.isSymLink() || ignored.excludes(relative, false)
                        || !Thoughts::isAgentProposalSourcePath(file.absoluteFilePath())) continue;
                    const QString path = canonicalTexSourcePath(docsRoot, sourceFile);
                    if (path.isEmpty() || ambiguous.contains(path)) continue;
                    const auto previous = connected.constFind(path);
                    if (previous != connected.cend() && previous->projectId != projectId) {
                        connected.insert(path, {sourceFile, {}, {}, {}});
                        ambiguous.insert(path);
                        continue;
                    }
                    connected.insert(path, {sourceFile, projectId, project.value(QStringLiteral("name")).toString(),
                                            project.value(QStringLiteral("target")).toString()});
            }
        }
    }
    return connected;
}

// Identity of everything the Library catalogue lists. Project sources are not
// part of it: an edit reloads only the documents built from that source (see
// documentsBySource, filled from the published source manifests).
QByteArray commentCatalogSignature(const QString &docsRoot, const QSet<QString> &ignoredProposalSources = {},
                                   QHash<QString, QStringList> *documentsBySource = nullptr)
{
    const Thoughts::DeveloperActivity activity(QStringLiteral("library.catalog_signature"));
    struct ManifestSourceCacheEntry {
        qint64 size = -1;
        qint64 modifiedMs = -1;
        qint64 metadataChangedMs = -1;
        QStringList sources;
    };
    static QMutex manifestSourceCacheMutex;
    static QHash<QString, ManifestSourceCacheEntry> manifestSourceCache;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addText = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayLiteral(":"));
        hash.addData(value);
        hash.addData(QByteArrayLiteral("\n"));
    };
    const auto addFile = [&addText, &ignoredProposalSources](const QFileInfo &file) {
        if (!ignoredProposalSources.isEmpty() && ignoredProposalSources.contains(file.absoluteFilePath())) return;
        addText(file.absoluteFilePath().toUtf8());
        addText(file.exists() ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
        addText(QByteArray::number(file.size()));
        addText(QByteArray::number(file.lastModified().toMSecsSinceEpoch()));
        addText(QByteArray::number(file.metadataChangeTime().toMSecsSinceEpoch()));
    };
    const auto addDirectory = [&addText, &addFile](const QString &path, const QStringList &filters) {
        const QDir directory(path);
        addText(directory.absolutePath().toUtf8());
        const QFileInfoList entries = directory.entryInfoList(filters, QDir::Files | QDir::Hidden, QDir::Name | QDir::IgnoreCase);
        addText(QByteArray::number(entries.size()));
        for (const QFileInfo &entry : entries) {
            addFile(entry);
        }
    };

    const QDir root(docsRoot);
    const QFileInfo currentPdfs(root.filePath(QStringLiteral("current_pdfs")));
    addText(Thoughts::Platform::directoryLinkTarget(currentPdfs).toUtf8());
    addDirectory(currentPdfs.absoluteFilePath(), {QStringLiteral("*")});
    addDirectory(root.filePath(QStringLiteral("comments")), {QStringLiteral("*_comments.jsonl"), QStringLiteral("review_resolutions.jsonl")});
    addDirectory(root.filePath(QStringLiteral("comments/.layout")), {QStringLiteral("*.json")});
    addFile(QFileInfo(Thoughts::LibraryProjectStore(docsRoot).path()));
    const auto settings = Thoughts::ProjectConfiguration::snapshotIdentity(docsRoot);
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        addText(it.key().toUtf8());
        addText(it.value().toUtf8());
    }
    const QDir bundleRoot = sourceBundleRoot(docsRoot);
    const QDir documentArtifacts(bundleRoot.filePath(QStringLiteral("documents")));
    QHash<QString, QString> canonicalSources;
    for (const QFileInfo &pointerInfo : documentArtifacts.entryInfoList({QStringLiteral("*.pdf.json")}, QDir::Files, QDir::Name)) {
        addFile(pointerInfo);
        QFile pointerFile(pointerInfo.absoluteFilePath());
        if (!pointerFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString artifactSha256 = QJsonDocument::fromJson(pointerFile.readAll()).object().value(QStringLiteral("artifact_sha256")).toString();
        const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
        if (sha256.match(artifactSha256).hasMatch()) {
            const QFileInfo manifestInfo(bundleRoot.filePath(QStringLiteral("objects/%1").arg(artifactSha256)));
            addFile(manifestInfo);
            const qint64 manifestSize = manifestInfo.exists() ? manifestInfo.size() : -1;
            const qint64 manifestModifiedMs = manifestInfo.exists() ? manifestInfo.lastModified().toMSecsSinceEpoch() : -1;
            const qint64 manifestMetadataChangedMs = manifestInfo.exists() ? manifestInfo.metadataChangeTime().toMSecsSinceEpoch() : -1;
            QStringList storedSources;
            bool cacheHit = false;
            {
                QMutexLocker locker(&manifestSourceCacheMutex);
                const auto cached = manifestSourceCache.constFind(manifestInfo.absoluteFilePath());
                if (cached != manifestSourceCache.cend() && cached->size == manifestSize && cached->modifiedMs == manifestModifiedMs
                    && cached->metadataChangedMs == manifestMetadataChangedMs) {
                    storedSources = cached->sources;
                    cacheHit = true;
                }
            }
            if (!cacheHit) {
                QFile manifestFile(manifestInfo.absoluteFilePath());
                QJsonParseError parseError;
                if (!manifestFile.open(QIODevice::ReadOnly)) {
                    continue;
                }
                const QJsonDocument manifest = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !manifest.isObject()) {
                    continue;
                }
                const QJsonArray sources = manifest.object().value(QStringLiteral("sources")).toArray();
                storedSources.reserve(sources.size());
                for (const QJsonValue &source : sources) {
                    storedSources.append(source.toObject().value(QStringLiteral("path")).toString());
                }
                QMutexLocker locker(&manifestSourceCacheMutex);
                if (manifestSourceCache.size() >= 512 && !manifestSourceCache.contains(manifestInfo.absoluteFilePath())) {
                    manifestSourceCache.clear();
                }
                manifestSourceCache.insert(manifestInfo.absoluteFilePath(),
                                           ManifestSourceCacheEntry {manifestSize, manifestModifiedMs, manifestMetadataChangedMs, storedSources});
            }
            if (!documentsBySource) {
                continue;
            }
            const QString pdfFilename = pointerInfo.fileName().chopped(int(qstrlen(".json")));
            for (const QString &storedSource : std::as_const(storedSources)) {
                auto canonical = canonicalSources.constFind(storedSource);
                if (canonical == canonicalSources.cend()) {
                    canonical = canonicalSources.insert(storedSource, canonicalTexSourcePath(docsRoot, storedSource));
                }
                if (!canonical->isEmpty() && !(*documentsBySource)[*canonical].contains(pdfFilename)) {
                    (*documentsBySource)[*canonical].append(pdfFilename);
                }
            }
        }
    }
    return hash.result();
}

class CommentsFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit CommentsFilterProxyModel(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
        setRecursiveFilteringEnabled(true);
        setAutoAcceptChildRows(true);
    }

    void setDocumentStem(const QString &stem)
    {
        if (m_documentStem == stem) {
            return;
        }
        m_documentStem = stem;
        filtersChanged();
    }

    void setProjectDocuments(bool active, const QSet<QString> &stems)
    {
        if (m_projectDocumentsActive == active && m_projectDocuments == stems) { return; }
        m_projectDocumentsActive = active;
        m_projectDocuments = stems;
        filtersChanged();
    }

    void setProjectScope(int scope)
    {
        if (m_projectScope != scope) {
            m_projectScope = scope;
            filtersChanged();
        }
    }

    void setThreadIds(bool active, const QStringList &ids)
    {
        const QSet<QString> selected(ids.cbegin(), ids.cend());
        if (m_threadFilterActive == active && m_threadIds == selected) {
            return;
        }
        m_threadFilterActive = active;
        m_threadIds = selected;
        filtersChanged();
    }

    void setSearchText(const QString &text)
    {
        const QString normalized = text.trimmed();
        if (m_searchText == normalized) {
            return;
        }
        m_searchText = normalized;
        filtersChanged();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (m_notesOnly && orientation == Qt::Horizontal && section == CommentColumn && role == Qt::DisplayRole) {
            return i18n("To-dos");
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        // A to-do row shows its first line only, so one long to-do does not make
        // every row (the view has uniform row heights) as tall as itself.
        if (m_notesOnly && index.column() == CommentColumn && role == Qt::DisplayRole) {
            const QString contents = QSortFilterProxyModel::data(index, ContentsRole).toString();
            const QStringList lines = contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            QString text = lines.isEmpty() ? QSortFilterProxyModel::data(index, role).toString() : lines.first().trimmed();
            if (lines.size() > 1) text += QStringLiteral(" …");
            const QDate from = todoVisibleFrom(QSortFilterProxyModel::data(index, LogicalPayloadRole).value<QJsonObject>());
            if (from.isValid() && from > QDate::currentDate()) {
                text += QStringLiteral("  ·  ") + i18n("hidden until %1", QLocale().toString(from, QLocale::ShortFormat));
            }
            return text;
        }
        if (m_notesOnly && index.column() == CommentColumn && role == Qt::ToolTipRole) {
            return QSortFilterProxyModel::data(index, ContentsRole);
        }
        if (index.column() == CommentColumn && role == Qt::DecorationRole) {
            const QString kind = QSortFilterProxyModel::data(index, FeedbackKindRole).toString();
            if (!kind.isEmpty()) return Thoughts::FeedbackIcon::icon(kind);
        }
        return QSortFilterProxyModel::data(index, role);
    }

    void setShowHiddenTodos(bool show)
    {
        if (m_showHiddenTodos == show) { return; }
        m_showHiddenTodos = show;
        filtersChanged();
    }

    void setNotesOnly(bool notesOnly)
    {
        if (m_notesOnly == notesOnly) { return; }
        m_notesOnly = notesOnly;
        filtersChanged();
    }

    void setItemType(const QString &itemType)
    {
        if (m_itemType == itemType) {
            return;
        }
        m_itemType = itemType;
        filtersChanged();
    }

    void beginUpdate() { m_batching = true; }
    void endUpdate()
    {
        m_batching = false;
        if (std::exchange(m_filterChanged, false)) { invalidateRowsFilter(); }
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        const QModelIndex index = sourceModel()->index(sourceRow, CommentColumn, sourceParent);
        QModelIndex root = index;
        while (root.parent().isValid()) { root = root.parent().siblingAtColumn(CommentColumn); }
        if ((root.data(ItemTypeRole).toString() == QLatin1String("note")) != m_notesOnly) {
            return false;
        }
        // Diary entries are notes too, but the Diary tab lists them, not To-dos.
        if (m_notesOnly && diaryEntry(root.data(LogicalPayloadRole).value<QJsonObject>())) {
            return false;
        }
        if (m_notesOnly && !m_showHiddenTodos && todoHiddenToday(root.data(LogicalPayloadRole).value<QJsonObject>())) {
            return false;
        }
        if (!m_documentStem.isEmpty() && index.data(DocumentRole).toString() != m_documentStem) {
            return false;
        }
        if (m_projectDocumentsActive && !m_projectDocuments.contains(index.data(DocumentRole).toString())) {
            return false;
        }
        const bool noProject = index.data(DocumentRole).toString().isEmpty() && index.data(PdfPathRole).toString().isEmpty();
        if ((m_projectScope == 1 && !noProject) || (m_projectScope == 2 && noProject)) {
            return false;
        }
        if (m_threadFilterActive && !m_threadIds.contains(index.data(ThreadRootIdRole).toString())) {
            return false;
        }
        // An anchor-removed thread is an issue only; it is not part of the main comment
        // flow. A proposal removed outside Thoughts is still counted as an agent
        // proposal, so All types and Agent proposals list it too (with Removed
        // proposals and Issues), and the counts match the list.
        const QString rootStatus = root.data(AnchorStatusRole).toString();
        const bool removedProposal = rootStatus == QLatin1String("proposal_removed");
        const bool anchorRemovedThread = rootStatus == QLatin1String("anchor_removed") || rootStatus == QLatin1String("comment_missing");
        if (m_itemType == QLatin1String("removed_proposals")) {
            if (!removedProposal) return false;
        } else if (m_itemType == QLatin1String("issues")) {
            if (!anchorRemovedThread && !removedProposal
                && (index.data(ItemTypeRole).toString() != QLatin1String("comment") || !commentIssueStatuses().contains(index.data(AnchorStatusRole).toString()))) { return false; }
        } else {
            if (anchorRemovedThread) return false;
            if (!m_itemType.isEmpty()) {
                // A comment of type other is what the list calls Comments.
                const QString kind = index.data(FeedbackKindRole).toString();
                const QString type = kind.isEmpty() ? index.data(ItemTypeRole).toString() : kind == QLatin1String("other") ? QStringLiteral("comment") : kind;
                if (type != m_itemType) return false;
            }
        }
        return m_searchText.isEmpty() || index.data(SearchRole).toString().contains(m_searchText, Qt::CaseInsensitive);
    }

    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        if (left.parent().isValid() && right.parent().isValid()) {
            const int leftOrder = left.siblingAtColumn(CommentColumn).data(ReplyOrderRole).toInt();
            const int rightOrder = right.siblingAtColumn(CommentColumn).data(ReplyOrderRole).toInt();
            if (leftOrder != rightOrder) {
                return sortOrder() == Qt::DescendingOrder ? leftOrder > rightOrder : leftOrder < rightOrder;
            }
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    void filtersChanged()
    {
        if (m_batching) { m_filterChanged = true; }
        else { invalidateRowsFilter(); }
    }
    bool m_batching = false;
    bool m_filterChanged = false;

    QString m_documentStem;
    QSet<QString> m_projectDocuments;
    bool m_projectDocumentsActive = false;
    int m_projectScope = 0;
    QSet<QString> m_threadIds;
    bool m_threadFilterActive = false;
    QString m_searchText;
    QString m_itemType;
    bool m_notesOnly = false;
    bool m_showHiddenTodos = false;
};

QStandardItem *makeItem(const QString &text, const QVariant &sortValue)
{
    auto *item = new QStandardItem(text);
    item->setEditable(false);
    item->setData(sortValue, SortRole);
    return item;
}

struct OrphanSidecarInfo {
    QFileInfo file;
    int records = 0;
    int liveRecords = 0;
    bool valid = true;
};

QList<OrphanSidecarInfo> orphanSidecars(const QString &docsRoot)
{
    const QDir root(docsRoot);
    const QDir pdfDir(root.filePath(QStringLiteral("current_pdfs")));
    QSet<QString> pdfStems;
    for (const QFileInfo &pdf : pdfDir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        if (pdf.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            pdfStems.insert(pdf.completeBaseName().toCaseFolded());
        }
    }

    QList<OrphanSidecarInfo> result;
    const QDir commentsDir(root.filePath(QStringLiteral("comments")));
    for (const QFileInfo &sidecar : commentsDir.entryInfoList({QStringLiteral("*_comments.jsonl")}, QDir::Files, QDir::Name)) {
        QString stem = sidecar.fileName();
        stem.chop(QStringLiteral("_comments.jsonl").size());
        if (pdfStems.contains(stem.toCaseFolded())) {
            continue;
        }

        OrphanSidecarInfo info;
        info.file = sidecar;
        QFile file(sidecar.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            info.valid = false;
            result.append(info);
            continue;
        }
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument record = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !record.isObject()) {
                info.valid = false;
                break;
            }
            ++info.records;
            if (!record.object().value(QStringLiteral("deleted")).toBool()) {
                ++info.liveRecords;
            }
        }
        result.append(info);
    }
    return result;
}

// The details panel keeps a comfortable floor width but never drops below what
// its rows need; a plain setMinimumWidth() would override the layout and clip
// buttons whenever the splitter squeezes the panel.
class DetailsPanel : public QWidget
{
public:
    QSize minimumSizeHint() const override
    {
        const QSize needed = QWidget::minimumSizeHint();
        return {qMax(360, needed.width()), needed.height()};
    }
};

}

namespace Thoughts
{

QList<ProjectAttachment> loadProjectAttachments(const QString &docsRoot, const QString &project, QString *error)
{
    if (error) {
        error->clear();
    }
    const QRegularExpression safeProject(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!safeProject.match(project).hasMatch()) {
        if (error) {
            *error = i18n("The attachment project name is invalid.");
        }
        return {};
    }
    const QString path = QDir(docsRoot).filePath(project + QStringLiteral("/tex/support/thoughts_project_attachments.txt"));
    const auto object = DocumentBuildSelection::read(path, error);
    if (object.isEmpty()) return {};
    const QJsonDocument document(object);

    const QRegularExpression safeFilename(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression validSha256(QStringLiteral("^[0-9a-f]{64}$"));
    QSet<QString> filenames;
    QList<ProjectAttachment> attachments;
    for (const QJsonValue &value : document.object().value(QStringLiteral("attachments")).toArray()) {
        const QJsonObject entry = value.toObject();
        ProjectAttachment attachment;
        attachment.filename = entry.value(QStringLiteral("filename")).toString();
        attachment.sha256 = entry.value(QStringLiteral("sha256")).toString();
        attachment.title = entry.value(QStringLiteral("title")).toString();
        attachment.placement = entry.value(QStringLiteral("placement")).toString();
        const QString folded = attachment.filename.toCaseFolded();
        const bool validTitle = attachment.title == attachment.title.trimmed() && !attachment.title.isEmpty() && attachment.title.size() <= 200
            && std::none_of(attachment.title.cbegin(), attachment.title.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; });
        if (!safeFilename.match(attachment.filename).hasMatch() || QFileInfo(attachment.filename).fileName() != attachment.filename
            || !validSha256.match(attachment.sha256).hasMatch() || !validTitle || attachment.placement != QLatin1String("append") || filenames.contains(folded)) {
            if (error) {
                *error = i18n("The project attachment manifest contains an invalid entry.");
            }
            return {};
        }
        filenames.insert(folded);
        attachments.append(attachment);
    }
    return attachments;
}

ProjectAttachmentMutationResult setProjectAttachment(const QString &docsRoot,
                                                     const QString &project,
                                                     const QString &pdfPath,
                                                     const QString &filename,
                                                     const QString &sha256,
                                                     const QString &title,
                                                     bool attached)
{
    ProjectAttachmentMutationResult result;
    const QRegularExpression safeProject(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    const QRegularExpression safeFilename(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression validSha256(QStringLiteral("^[0-9a-f]{64}$"));
    const QString normalizedTitle = title.simplified();
    const bool validTitle = !normalizedTitle.isEmpty() && normalizedTitle.size() <= 200
        && std::none_of(normalizedTitle.cbegin(), normalizedTitle.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; });
    if (!safeProject.match(project).hasMatch() || !safeFilename.match(filename).hasMatch() || QFileInfo(filename).fileName() != filename
        || !validSha256.match(sha256).hasMatch() || !validTitle) {
        result.error = i18n("The project attachment request is invalid.");
        return result;
    }

    const QFileInfo docsInfo(docsRoot);
    const QString canonicalDocsRoot = docsInfo.canonicalFilePath();
    const QFileInfo projectInfo(QDir(docsRoot).filePath(project));
    const QString canonicalProject = projectInfo.canonicalFilePath();
    if (canonicalDocsRoot.isEmpty() || canonicalProject.isEmpty() || !projectInfo.isDir()
        || !canonicalProject.startsWith(canonicalDocsRoot + QLatin1Char('/'))
        // A configured folder's entry points may live anywhere; only the legacy layout needs tex/source.
        || (!(QFileInfo(QDir(canonicalProject).filePath(QStringLiteral("tex/docs_order_manifest.txt"))).isFile()
              && QFileInfo(QDir(canonicalProject).filePath(QStringLiteral("tex/source"))).isDir())
            && !QFileInfo(QDir(canonicalProject).filePath(QStringLiteral("thoughts-project.json"))).isFile()
            && Thoughts::ProjectConfiguration::find(docsRoot, project).isEmpty())) {
        result.error = i18n("The selected source-backed project is unavailable.");
        return result;
    }

    QString manifestError;
    QList<ProjectAttachment> attachments = loadProjectAttachments(docsRoot, project, &manifestError);
    if (!manifestError.isEmpty()) {
        result.error = manifestError;
        return result;
    }
    auto existing = std::find_if(attachments.begin(), attachments.end(), [&filename](const ProjectAttachment &candidate) {
        return candidate.filename.compare(filename, Qt::CaseInsensitive) == 0;
    });
    if (existing != attachments.end() && existing->sha256 != sha256) {
        result.error = i18n("A different PDF with this filename is already attached to the project.");
        return result;
    }

    QString copiedAttachment;
    if (attached) {
        const QFileInfo source(pdfPath);
        if (!source.isFile() || !source.isReadable() || QString::fromLatin1(fileContentSha256(source.absoluteFilePath()).toHex()) != sha256) {
            result.error = i18n("The Library PDF no longer matches its recorded SHA-256.");
            return result;
        }
        const auto manifest = DocumentBuildSelection::rawRead(DocumentBuildSelection::manifestPath(docsRoot, project));
        const QString thirdPartyRoot = manifest.value(QStringLiteral("schema_version")).toInt() == 4
            ? QFileInfo(QDir(QDir(docsRoot).filePath(project)).filePath(DocumentBuildSelection::managedPdfSource(docsRoot, project, filename))).absolutePath()
            : QDir(docsRoot).filePath(QStringLiteral("third_party"));
        const QString destination = QDir(thirdPartyRoot).filePath(filename);
        if (!QDir().mkpath(thirdPartyRoot)) {
            result.error = i18n("The workspace third-party PDF directory could not be created.");
            return result;
        }
        if (QFileInfo::exists(destination)) {
            if (QString::fromLatin1(fileContentSha256(destination).toHex()) != sha256) {
                result.error = i18n("A different workspace PDF already uses this filename.");
                return result;
            }
        } else {
            QFile input(source.absoluteFilePath());
            QSaveFile output(destination);
            output.setDirectWriteFallback(false);
            if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
                result.error = i18n("The Library PDF could not be prepared as a project attachment.");
                return result;
            }
            bool copied = false;
            while (!input.atEnd()) {
                const QByteArray content = input.read(1024 * 1024);
                if (content.isEmpty() || output.write(content) != content.size()) {
                    copied = false;
                    break;
                }
                copied = true;
            }
            if (!copied || !output.commit()) {
                result.error = i18n("The Library PDF could not be prepared as a project attachment.");
                return result;
            }
            copiedAttachment = destination;
        }
        if (existing == attachments.end()) {
            attachments.append({filename, sha256, normalizedTitle, QStringLiteral("append")});
        } else if (existing->title != normalizedTitle) {
            existing->title = normalizedTitle;
        } else if (copiedAttachment.isEmpty()) {
            result.success = true;
            return result;
        }
    } else {
        if (existing == attachments.end()) {
            result.success = true;
            return result;
        }
        attachments.erase(existing);
    }

    const QString supportRoot = QDir(canonicalProject).filePath(QStringLiteral("tex/support"));
    const QString manifestPath = QDir(supportRoot).filePath(QStringLiteral("thoughts_project_attachments.txt"));
    if (!QDir().mkpath(supportRoot)) {
        if (!copiedAttachment.isEmpty()) {
            QFile::remove(copiedAttachment);
        }
        result.error = i18n("The project support directory could not be created.");
        return result;
    }
    QJsonArray encoded;
    for (const ProjectAttachment &attachment : std::as_const(attachments)) {
        encoded.append(QJsonObject {{QStringLiteral("filename"), attachment.filename},
                                    {QStringLiteral("sha256"), attachment.sha256},
                                    {QStringLiteral("title"), attachment.title},
                                    {QStringLiteral("placement"), attachment.placement}});
    }
    if (!DocumentBuildSelection::write(manifestPath, QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("attachments"), encoded}}).isEmpty()) {
        if (!copiedAttachment.isEmpty()) {
            QFile::remove(copiedAttachment);
        }
        result.error = i18n("The project attachment manifest could not be saved.");
        return result;
    }
    result.success = true;
    result.changed = true;
    return result;
}

static void rebuildTrackedCommentThreads(TrackedCommentDocument &document)
{
    QHash<QString, int> commentById;
    for (int index = 0; index < document.comments.size(); ++index) {
        TrackedComment &comment = document.comments[index];
        comment.threadRootId = comment.id;
        comment.reply = false;
        comment.replyOrder = 0;
        if (!comment.id.isEmpty()) {
            commentById.insert(comment.id, index);
        }
    }
    for (TrackedComment &comment : document.comments) {
        QString rootId = comment.id;
        QString currentId = comment.id;
        QSet<QString> visited;
        bool validThread = !currentId.isEmpty();
        while (validThread) {
            if (visited.contains(currentId)) {
                validThread = false;
                break;
            }
            visited.insert(currentId);
            const auto current = commentById.constFind(currentId);
            if (current == commentById.cend()) {
                validThread = false;
                break;
            }
            const QString parentId = document.comments.at(*current).parentId;
            if (parentId.isEmpty()) {
                rootId = currentId;
                break;
            }
            if (!commentById.contains(parentId)) {
                validThread = false;
                break;
            }
            currentId = parentId;
        }
        if (!validThread) {
            rootId = comment.id;
        }
        comment.threadRootId = rootId;
        comment.reply = rootId != comment.id;
    }

    QHash<QString, QList<int>> repliesByRoot;
    for (int index = 0; index < document.comments.size(); ++index) {
        const TrackedComment &comment = document.comments.at(index);
        if (comment.reply) {
            repliesByRoot[comment.threadRootId].append(index);
        }
    }
    for (auto replies = repliesByRoot.begin(); replies != repliesByRoot.end(); ++replies) {
        std::sort(replies->begin(), replies->end(), [&document](int left, int right) {
            const TrackedComment &leftComment = document.comments.at(left);
            const TrackedComment &rightComment = document.comments.at(right);
            if (leftComment.displayDate != rightComment.displayDate) {
                if (!leftComment.displayDate.isValid()) {
                    return false;
                }
                if (!rightComment.displayDate.isValid()) {
                    return true;
                }
                return leftComment.displayDate < rightComment.displayDate;
            }
            if (leftComment.page != rightComment.page) {
                return leftComment.page < rightComment.page;
            }
            return leftComment.id < rightComment.id;
        });
        for (int order = 0; order < replies->size(); ++order) {
            document.comments[replies->at(order)].replyOrder = order;
        }
    }
}

struct DocumentLoadTiming {
    qint64 metadataMs = 0;
    qint64 segmentsMs = 0;
    qint64 sidecarsMs = 0;
    DocumentSegmentTiming segment;
};

std::optional<TrackedCommentDocument> loadTrackedCommentDocument(const QString &docsRoot, const QString &pdfPath,
                                                                  const QJsonObject &fedoraProjections, DocumentLoadTiming *timing = nullptr)
{
    QElapsedTimer stepElapsed;
    stepElapsed.start();
    const QDir root(docsRoot);
    const QFileInfo pdfFile(pdfPath);
    if (!pdfFile.isFile() || pdfFile.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    TrackedCommentDocument document;
    document.stem = pdfFile.completeBaseName();
    document.displayName = document.stem;
    document.pdfPath = pdfFile.absoluteFilePath();
    document.pdfAvailable = true;
    document.pdfSizeBytes = pdfFile.size();
    const LibraryDocumentMetadata library = libraryDocumentMetadata(docsRoot, pdfFile.fileName(), pdfFile.absoluteFilePath());
    if (timing) timing->metadataMs += stepElapsed.restart();
    document.libraryFilename = library.filename;
    document.libraryKind = library.kind;
    document.librarySha256 = library.sha256;
    document.libraryProject = library.project;
    document.libraryDocumentId = library.documentId;
    document.libraryPublishedAt = library.publishedAt;
    document.libraryBuiltAt = library.builtAt;
    document.libraryPageCount = library.pageCount;
    document.libraryLatestBuildMessage = library.latestBuildMessage;
    document.libraryLatestBuildSource = library.latestBuildSource;
    document.verifiedBuildInputBytes = library.verifiedBuildInputBytes;
    document.verifiedBuildInputCount = library.verifiedBuildInputCount;
    document.libraryGenerationCurrent = library.generationCurrent;
    document.libraryPdfStale = library.pdfStale;
    document.verifiedBuildInputsAvailable = library.verifiedBuildInputsAvailable;
    document.librarySegments = libraryDocumentSegments(docsRoot, document, timing ? &timing->segment : nullptr);
    if (document.libraryPageCount < 0 && !document.librarySegments.isEmpty()) {
        qint64 nextPage = 1;
        for (const auto &raw : document.librarySegments) {
            const auto segment = raw.toObject();
            const qint64 pages = segment.value(QStringLiteral("page_count")).toInteger(-1);
            if (pages < 0 || segment.value(QStringLiteral("start_page")).toInteger() != nextPage) { nextPage = 0; break; }
            nextPage += pages;
        }
        if (nextPage > 0) document.libraryPageCount = nextPage - 1;
    }
    if (timing) timing->segmentsMs += stepElapsed.restart();
    if (!library.displayName.isEmpty()) {
        document.displayName = library.displayName;
    }

    const QString layoutPath = root.filePath(QStringLiteral("comments/.layout/%1.json").arg(document.stem));
    if (QFileInfo::exists(layoutPath)) {
        QJsonObject layout;
        if (readLayout(layoutPath, &layout)) {
            document.state = TrackedCommentDocument::SourceLinked;
        } else {
            document.error = i18n("The source-link layout is invalid.");
        }
    }

    QByteArray fingerprint;
    QString error;
    const QList<QJsonObject> sidecarRecords = Okular::DocsCommentSynchronizer::visibleSidecarComments(docsRoot, document.stem, &fingerprint, &error, &document.suppressedCommentIds);
    if (timing) timing->sidecarsMs += stepElapsed.restart();
    const QList<QJsonObject> records = recordsWithFedoraProjections(sidecarRecords, document.stem, fedoraProjections);
    if (!error.isEmpty()) {
        document.error = document.error.isEmpty() ? error : document.error + QLatin1Char('\n') + error;
    }
    if (error.isEmpty()) {
        document.comments.reserve(records.size());
        for (const QJsonObject &record : records) {
            if (!Okular::DocsCommentSynchronizer::isTrackedCommentRecord(record)) {
                continue;
            }
            TrackedComment comment;
            comment.documentStem = document.stem;
            comment.pdfPath = document.pdfPath;
            comment.pdfAvailable = document.pdfAvailable;
            comment.id = readStableId(record);
            comment.parentId = record.value(QStringLiteral("in_reply_to")).toString();
            if (comment.parentId.startsWith(QLatin1String("id:"))) {
                comment.parentId.remove(0, 3);
            }
            comment.contents = record.value(QStringLiteral("contents")).toString();
            comment.selectedText = record.value(QStringLiteral("selected_text")).toString();
            comment.author = record.value(QStringLiteral("author")).toString();
            comment.sourceFile = record.value(QStringLiteral("tex_file")).toString();
            comment.annotationPdfSha256 = record.value(QStringLiteral("annotation_pdf_sha256")).toString();
            comment.sourceLine = record.value(QStringLiteral("tex_line")).toInt();
            comment.anchorStatus = Thoughts::isAnchorRemovedPlacement(record) ? QStringLiteral("anchor_removed") : record.value(QStringLiteral("anchor_status")).toString();
            comment.priorityRaw = record.value(QStringLiteral("priority")).toString();
            comment.chatGptUrl = record.value(QStringLiteral("chatgpt_url")).toString();
            const QJsonValue rawLocation = record.value(QStringLiteral("location"));
            comment.locationPresent = record.contains(QStringLiteral("location")) && !rawLocation.isNull();
            if (rawLocation.isObject()) {
                comment.location = rawLocation.toObject();
                comment.latitude = comment.location.value(QStringLiteral("latitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
                comment.longitude = comment.location.value(QStringLiteral("longitude")).toDouble(std::numeric_limits<double>::quiet_NaN());
                comment.locationValid = !comment.location.value(QStringLiteral("label")).toString().trimmed().isEmpty() && std::isfinite(comment.latitude) && comment.latitude >= -90.0 && comment.latitude <= 90.0 &&
                    std::isfinite(comment.longitude) && comment.longitude >= -180.0 && comment.longitude <= 180.0;
            }
            comment.page = record.value(QStringLiteral("page")).toInt();
            comment.displayDate = commentDisplayDate(record);
            comment.rootSortDate = rootCommentSortDate(record);
            document.comments.append(comment);
        }

        rebuildTrackedCommentThreads(document);
    }

    return document;
}

QList<TrackedCommentDocument> loadTrackedCommentDocuments(const QString &docsRoot,
                                                          const QJsonObject &fedoraProjections,
                                                          bool *liveSourcesValid,
                                                          const QString &onlyFilename,
                                                          bool discoverIncludedSources)
{
    const auto settingsReadsBefore = ProjectConfiguration::diskReadCount();
    static std::atomic<quint64> nextLoadId {0};
    const qint64 loadId = qint64(++nextLoadId);
    QElapsedTimer totalElapsed;
    QElapsedTimer stageElapsed;
    totalElapsed.start();
    stageElapsed.start();
    const auto recordStage = [&](const QString &stage, int count = -1) {
        QJsonObject data {{QStringLiteral("load_id"), loadId}, {QStringLiteral("stage"), stage},
                          {QStringLiteral("duration_ms"), stageElapsed.restart()}};
        if (count >= 0) data.insert(QStringLiteral("count"), count);
        DeveloperLog::record(QStringLiteral("library.load.stage"), data);
    };
    DeveloperLog::record(QStringLiteral("library.load.started"), {{QStringLiteral("load_id"), loadId},
        {QStringLiteral("full_discovery"), discoverIncludedSources}, {QStringLiteral("single_document"), !onlyFilename.isEmpty()}});
    if (liveSourcesValid) {
        *liveSourcesValid = true;
    }
    QString historyError;
    QHash<QString, QJsonObject> latestProposalEvents;
    const bool historyIndexed = Thoughts::ProposalHistory::latest(docsRoot, &latestProposalEvents, &historyError);
    QHash<QString, Thoughts::ProposalHistory::Dates> datesById;
    recordStage(QStringLiteral("proposal current records"), latestProposalEvents.size());
    const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    QList<TrackedCommentDocument> documents;
    const QDir pdfs(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QFileInfoList files = pdfs.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    DocumentLoadTiming documentTiming;
    for (const QFileInfo &pdfFile : files) {
        if (!onlyFilename.isEmpty() && pdfFile.fileName().compare(onlyFilename, Qt::CaseInsensitive) != 0) { continue; }
        const std::optional<TrackedCommentDocument> document = loadTrackedCommentDocument(docsRoot, pdfFile.absoluteFilePath(), fedoraProjections, &documentTiming);
        if (document) {
            documents.append(*document);
        }
    }
    recordStage(QStringLiteral("document records"), documents.size());
    for (const auto &[name, duration] : {QPair<QString, qint64> {QStringLiteral("document metadata"), documentTiming.metadataMs},
                                        {QStringLiteral("document segments"), documentTiming.segmentsMs},
                                        {QStringLiteral("comment sidecars"), documentTiming.sidecarsMs}}) {
        DeveloperLog::record(QStringLiteral("library.load.detail"), {{QStringLiteral("load_id"), loadId},
            {QStringLiteral("stage"), name}, {QStringLiteral("duration_ms"), duration}});
    }
    for (const auto &[name, duration] : {QPair<QString, qint64>{QStringLiteral("segment pointers"), documentTiming.segment.pointerMs},
                                        {QStringLiteral("segment artifact reads"), documentTiming.segment.artifactReadMs},
                                        {QStringLiteral("segment artifact hashes"), documentTiming.segment.artifactHashMs},
                                        {QStringLiteral("segment PDF hashes"), documentTiming.segment.pdfHashMs},
                                        {QStringLiteral("segment JSON parsing"), documentTiming.segment.jsonParseMs},
                                        {QStringLiteral("segment validation"), documentTiming.segment.validationMs}}) {
        DeveloperLog::record(QStringLiteral("library.load.detail"), {{QStringLiteral("load_id"), loadId},
            {QStringLiteral("stage"), name}, {QStringLiteral("duration_ms"), duration},
            {QStringLiteral("verified_documents"), documentTiming.segment.verifiedDocuments},
            {QStringLiteral("artifact_bytes"), documentTiming.segment.artifactBytes},
            {QStringLiteral("pdf_bytes"), documentTiming.segment.pdfBytes}});
    }

    BundleLoadTiming bundleTiming;
    const QList<AgentProposalArtifact> artifacts = readAgentProposalArtifacts(docsRoot, onlyFilename, &bundleTiming);
    recordStage(QStringLiteral("source bundles"), artifacts.size());
    for (const auto &[name, duration] : {QPair<QString, qint64>{QStringLiteral("bundle resolution IDs"), bundleTiming.resolvedIdsMs},
                                        {QStringLiteral("bundle pointers"), bundleTiming.pointersMs},
                                        {QStringLiteral("bundle cache"), bundleTiming.cacheMs},
                                        {QStringLiteral("bundle object verification"), bundleTiming.verificationMs},
                                        {QStringLiteral("bundle JSON parsing"), bundleTiming.parsingMs},
                                        {QStringLiteral("bundle assembly"), bundleTiming.assemblyMs},
                                        {QStringLiteral("bundle filtering"), bundleTiming.filteringMs}}) {
        DeveloperLog::record(QStringLiteral("library.load.detail"), {{QStringLiteral("load_id"), loadId},
            {QStringLiteral("stage"), name}, {QStringLiteral("duration_ms"), duration},
            {QStringLiteral("cache_hits"), bundleTiming.cacheHits}, {QStringLiteral("cache_misses"), bundleTiming.cacheMisses},
            {QStringLiteral("object_bytes"), bundleTiming.objectBytes}});
    }
    QSet<QString> monitoredProposalPaths;
    for (const auto &artifact : artifacts)
        for (const auto &source : artifact.sources) {
            const QString path = canonicalTexSourcePath(docsRoot, source.toObject().value(QStringLiteral("path")).toString());
            if (!path.isEmpty()) monitoredProposalPaths.insert(path);
        }
    if (!artifacts.isEmpty()) {
        QHash<QString, int> documentsByFilename;
        QHash<QString, QByteArray> documentSha256s;
        QHash<int, QSet<QString>> scannedSources;
        for (int index = 0; index < documents.size(); ++index) {
            documentsByFilename.insert(QFileInfo(documents.at(index).pdfPath).fileName().toCaseFolded(), index);
        }
        for (const AgentProposalArtifact &artifact : artifacts) {
            const int documentIndex = documentsByFilename.value(artifact.filename.toCaseFolded(), -1);
            if (documentIndex >= 0) {
                mergeLogicalComments(documents[documentIndex], artifact);
            }
        }
        const auto documentSha256 = [&documentSha256s](const QString &path) {
            const auto found = documentSha256s.constFind(path);
            if (found != documentSha256s.cend()) {
                return *found;
            }
            const QByteArray digest = fileContentSha256(path).toHex();
            documentSha256s.insert(path, digest);
            return digest;
        };
        for (const AgentProposalArtifact &artifact : artifacts) {
            const int documentIndex = documentsByFilename.value(artifact.filename.toCaseFolded(), -1);
            if (documentIndex < 0) {
                continue;
            }
            for (const QJsonValue &sourceValue : artifact.sources) {
                const QString sourceFile = sourceValue.toObject().value(QStringLiteral("path")).toString();
                const QString canonicalSource = canonicalTexSourcePath(docsRoot, sourceFile);
                if (canonicalSource.isEmpty() || proposalSourceExcluded(docsRoot, sourceFile)) {
                    continue;
                }
                const LiveProjectProposals live = readLiveProjectProposals(canonicalSource);
                if (!live.valid) {
                    if (liveSourcesValid) { *liveSourcesValid = false; }
                    documents[documentIndex].error = live.error;
                    continue;
                }
                QFile observedSource(canonicalSource);
                QString historyError;
                if (!observedSource.open(QIODevice::ReadOnly) || !Thoughts::observeAgentProposalSource(docsRoot, canonicalSource, observedSource.readAll(), &historyError,
                                                                                                      historyIndexed ? &latestProposalEvents : nullptr)) {
                    documents[documentIndex].error = historyError.isEmpty() ? i18n("The proposal source could not be observed.") : historyError;
                }
                const auto &liveProposals = live.proposals;
                const QString &sourceSha256 = live.sha256;
                scannedSources[documentIndex].insert(canonicalSource);
                TrackedCommentDocument &document = documents[documentIndex];
                for (const Thoughts::AgentProposedEdit &proposal : liveProposals) {
                    TrackedComment comment;
                    comment.documentStem = document.stem;
                    comment.pdfPath = document.pdfPath;
                    comment.pdfAvailable = document.pdfAvailable;
                    comment.id = proposal.id;
                    comment.displayDate = Thoughts::agentProposalTimestampDateTime(proposal.createdAt);
                    comment.contents = proposal.explanation;
                    comment.proposalKind = QStringLiteral("edit");
                    comment.proposalBeforePreview = proposal.original;
                    comment.proposalAfterPreview = proposal.replacement;
                    comment.proposalSourceSha256 = sourceSha256;
                    comment.selectedText = i18n("%1 → %2", proposal.original.isEmpty() ? i18n("[empty]") : proposal.original, proposal.replacement.isEmpty() ? i18n("[delete]") : proposal.replacement);
                    comment.author = QStringLiteral("Agent");
                    comment.sourceFile = sourceFile;
                    const auto location = Thoughts::annotationSourceLocation(docsRoot, QStringLiteral("proposal"), proposal.id, Thoughts::SourceOwner::of(sourceFile));
                    comment.sourceLine = !Thoughts::sourceLocationIndex(docsRoot).contains(QStringLiteral("files")) ? proposal.line
                        : location.value(QStringLiteral("state")).toString() == QLatin1String("linked")
                        && location.value(QStringLiteral("source_sha256")).toString() == sourceSha256 ? location.value(QStringLiteral("start_line")).toInt() : 0;
                    comment.anchorStatus = QStringLiteral("source_only");
                    comment.priorityRaw = QStringLiteral("normal");
                    comment.agentProposal = true;
                    comment.liveSourceProposal = true;
                    comment.bundleId = artifact.reviewCorrespondenceAvailable ? artifact.artifactSha256 : QString();
                    applyReviewCorrespondence(comment, artifact, QStringLiteral("proposal:") + proposal.id);
                    document.comments.append(comment);
                }
                for (const Thoughts::AgentProposalIssue &issue : live.issues) {
                    TrackedComment comment = proposalIssueComment(issue, canonicalSource, sourceFile, sourceSha256);
                    comment.documentStem = document.stem;
                    comment.pdfPath = document.pdfPath;
                    document.comments.append(comment);
                }
            }
        }
        for (const AgentProposalArtifact &artifact : artifacts) {
            if (!sha256.match(artifact.pdfSha256).hasMatch()) {
                continue;
            }
            for (const QJsonValue &value : artifact.proposals) {
                const QJsonObject proposal = value.toObject();
                const QString id = proposal.value(QStringLiteral("id")).toString();
                if (proposalSourceExcluded(docsRoot, proposal.value(QStringLiteral("source_file")).toString())) { continue; }
                const QJsonObject placement = proposal.value(QStringLiteral("placement")).toObject();
                QJsonArray fragments = placement.value(QStringLiteral("quad_fragments")).toArray();
                if (fragments.isEmpty()) {
                    for (const QJsonValue &regionValue : placement.value(QStringLiteral("regions")).toArray()) {
                        const QJsonObject region = regionValue.toObject();
                        if (region.value(QStringLiteral("role")).toString() == QLatin1String("after") || fragments.isEmpty()) {
                            fragments = region.value(QStringLiteral("quad_fragments")).toArray();
                        }
                    }
                }
                const QString canonicalSource = canonicalTexSourcePath(docsRoot, proposal.value(QStringLiteral("source_file")).toString());
                if (!proposal.value(QStringLiteral("source_file")).toString().isEmpty() && canonicalSource.isEmpty()) continue;
                for (const QJsonValue &filenameValue : proposal.value(QStringLiteral("pdf_filenames")).toArray()) {
                    const int documentIndex = documentsByFilename.value(filenameValue.toString().toCaseFolded(), -1);
                    if (documentIndex < 0) {
                        continue;
                    }
                    if (!canonicalSource.isEmpty() && scannedSources.value(documentIndex).contains(canonicalSource)) {
                        continue;
                    }
                    TrackedCommentDocument &document = documents[documentIndex];
                    if (documentSha256(document.pdfPath) != artifact.pdfSha256.toLatin1()) {
                        continue;
                    }
                    TrackedComment comment;
                    comment.documentStem = document.stem;
                    comment.pdfPath = document.pdfPath;
                    comment.pdfAvailable = document.pdfAvailable;
                    comment.id = id;
                    comment.parentId = proposal.value(QStringLiteral("parent_id")).toString();
                    comment.displayDate = firstCommentDate(proposal, {"created_at"});
                    comment.contents = proposal.value(QStringLiteral("comment")).toString();
                    comment.proposalKind = proposal.value(QStringLiteral("kind")).toString(QStringLiteral("remove"));
                    const QString before = proposal.value(QStringLiteral("before")).toObject().value(QStringLiteral("preview")).toString(proposal.value(QStringLiteral("body_preview")).toString());
                    const QString after = proposal.value(QStringLiteral("after")).toObject().value(QStringLiteral("preview")).toString();
                    comment.proposalBeforePreview = before;
                    comment.proposalAfterPreview = after;
                    comment.selectedText = comment.proposalKind == QLatin1String("edit") ? i18n("%1 → %2", before, after.isEmpty() ? i18n("[delete]") : after) : before;
                    comment.author = QStringLiteral("Agent");
                    comment.sourceFile = proposal.value(QStringLiteral("source_file")).toString();
                    comment.sourceLine = proposal.value(QStringLiteral("line")).toInt();
                    comment.anchorStatus = placement.value(QStringLiteral("status")).toString() == QLatin1String("anchored") ? QStringLiteral("anchored") : QStringLiteral("source_only");
                    comment.priorityRaw = QStringLiteral("normal");
                    comment.bundleId = artifact.artifactSha256;
                    comment.agentProposal = true;
                    comment.proposalSuppressed = proposal.value(QStringLiteral("suppressed")).toBool();
                    comment.correspondence = proposal.value(QStringLiteral("review_correspondence")).toObject();
                    const QJsonObject fragment = firstPrimaryPdfLocation(fragments, [&filenameValue](const QJsonObject &candidate) {
                        return candidate.value(QStringLiteral("pdf_filename")).toString().compare(filenameValue.toString(), Qt::CaseInsensitive) == 0;
                    });
                    if (!fragment.isEmpty()) {
                        comment.page = fragment.value(QStringLiteral("page")).toInt();
                    }
                    applyReviewCorrespondence(comment, artifact, QStringLiteral("proposal:") + id);
                    document.comments.append(comment);
                }
            }
        }
        for (TrackedCommentDocument &document : documents) {
            rebuildTrackedCommentThreads(document);
        }
    }
    recordStage(QStringLiteral("bundle proposals"), monitoredProposalPaths.size());

    QSet<QString> visibleProposalIds;
    for (const TrackedCommentDocument &document : std::as_const(documents)) {
        for (const TrackedComment &comment : document.comments) {
            if (comment.agentProposal) visibleProposalIds.insert(comment.id);
        }
    }
    auto discovered = cachedProposalDiscovery(docsRoot);
    const auto connectedSources = discoverIncludedSources ? connectedProposalSources(docsRoot) : QHash<QString, ConnectedProposalSource>{};
    recordStage(QStringLiteral("proposal source inventory"), connectedSources.size());
    QStringList sourcePaths = connectedSources.keys();
    if (!discoverIncludedSources) sourcePaths = discovered.keys();
    QHash<QString, QString> registeredSourceByPath;
    if (discoverIncludedSources) {
        for (const auto &record : latestProposalEvents) {
            const QString source = record.value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("source_file")).toString();
            const QString path = canonicalTexSourcePath(docsRoot, source);
            if (path.isEmpty()) continue;
            registeredSourceByPath.insert(path, source);
            if (!sourcePaths.contains(path)) sourcePaths.append(path);
        }
    }
    std::sort(sourcePaths.begin(), sourcePaths.end());
    QHash<QString, DiscoveredProposalSource> refreshedDiscovery;
    QHash<QString, int> unpublishedByProject;
    for (const QString &path : std::as_const(sourcePaths)) {
        const bool monitored = monitoredProposalPaths.contains(path);
        LiveProjectProposals live = discoverIncludedSources || monitored ? readLiveProjectProposals(path) : discovered.value(path).live;
        if (!live.valid) {
            if (liveSourcesValid) *liveSourcesValid = false;
            if (!discoverIncludedSources && discovered.contains(path)) live = discovered.value(path).live;
            else continue;
        }
        auto owner = discoverIncludedSources ? connectedSources.value(path) : discovered.value(path).owner;
        if (owner.sourceFile.isEmpty()) owner.sourceFile = registeredSourceByPath.value(path, QDir(docsRoot).relativeFilePath(path));
        refreshedDiscovery.insert(path, {owner, live});
        QString observationError;
        if ((discoverIncludedSources || monitored) && (!live.proposals.isEmpty() || registeredSourceByPath.contains(path))) {
            QFile observedSource(path);
            if (!observedSource.open(QIODevice::ReadOnly)
                || !Thoughts::observeAgentProposalSource(docsRoot, path, observedSource.readAll(), &observationError,
                                                          historyIndexed ? &latestProposalEvents : nullptr)) {
                if (liveSourcesValid) *liveSourcesValid = false;
                if (observationError.isEmpty()) observationError = i18n("The proposal source could not be observed.");
            }
        }
        // The project's unpublished-proposals document, created on first use.
        const auto unpublishedIndex = [&]() {
            const QString projectKey = owner.projectId;
            int documentIndex = unpublishedByProject.value(projectKey, -1);
            if (documentIndex < 0 && !owner.target.isEmpty()) {
                for (int index = 0; index < documents.size(); ++index) {
                    if (documents.at(index).libraryFilename == owner.target) { documentIndex = index; break; }
                }
            }
            if (documentIndex < 0) {
                TrackedCommentDocument local;
                local.displayName = projectKey.isEmpty() ? i18n("No project") : owner.projectName;
                if (!projectKey.isEmpty()) {
                    local.stem = QStringLiteral("source-proposals:") + projectKey;
                    local.libraryProjectId = projectKey;
                    local.libraryKind = QStringLiteral("source_proposals");
                    local.sourceProposalOnly = true;
                }
                documents.append(local);
                documentIndex = documents.size() - 1;
            }
            unpublishedByProject.insert(projectKey, documentIndex);
            return documentIndex;
        };
        const QString sourceFile = owner.sourceFile.isEmpty() ? registeredSourceByPath.value(path) : owner.sourceFile;
        for (const Thoughts::AgentProposedEdit &proposal : live.proposals) {
            if (visibleProposalIds.contains(proposal.id)) {
                if (!observationError.isEmpty()) {
                    for (auto &document : documents)
                        if (std::any_of(document.comments.cbegin(), document.comments.cend(), [&](const auto &comment) { return comment.id == proposal.id; }))
                            document.error = observationError;
                }
                continue;
            }
            const int documentIndex = unpublishedIndex();
            TrackedCommentDocument &unpublished = documents[documentIndex];
            if (!observationError.isEmpty()) unpublished.error = observationError;
            TrackedComment comment;
            comment.documentStem = unpublished.stem;
            comment.pdfPath = unpublished.pdfPath;
            comment.pdfAvailable = unpublished.pdfAvailable;
            comment.id = proposal.id;
            comment.displayDate = Thoughts::agentProposalTimestampDateTime(proposal.createdAt);
            comment.contents = proposal.explanation;
            comment.proposalKind = QStringLiteral("edit");
            comment.proposalBeforePreview = proposal.original;
            comment.proposalAfterPreview = proposal.replacement;
            comment.proposalSourceSha256 = live.sha256;
            comment.selectedText = i18n("%1 → %2", proposal.original.isEmpty() ? i18n("[empty]") : proposal.original,
                                        proposal.replacement.isEmpty() ? i18n("[delete]") : proposal.replacement);
            comment.author = QStringLiteral("Agent");
            comment.sourceFile = sourceFile;
            comment.sourceLine = proposal.line;
            comment.anchorStatus = QStringLiteral("source_only");
            comment.priorityRaw = QStringLiteral("normal");
            comment.agentProposal = true;
            comment.liveSourceProposal = true;
            unpublished.comments.append(comment);
            visibleProposalIds.insert(proposal.id);
        }
        for (const Thoughts::AgentProposalIssue &issue : live.issues) {
            TrackedComment comment = proposalIssueComment(issue, path, sourceFile, live.sha256);
            if (visibleProposalIds.contains(comment.id)) continue;
            TrackedCommentDocument &unpublished = documents[unpublishedIndex()];
            comment.documentStem = unpublished.stem;
            comment.pdfPath = unpublished.pdfPath;
            unpublished.comments.append(comment);
            visibleProposalIds.insert(comment.id);
        }
    }
    recordStage(QStringLiteral("live proposal sources"), sourcePaths.size());
    // Proposals removed outside Thoughts (protocol/agent-proposals.md §10.2): server
    // rows listed under Issues until the user restores or dismisses them. One whose
    // wrapper is back in a local source is pending again; the next Sync tells the server.
    int removedProposals = 0;
    for (auto it = fedoraProjections.constBegin(); it != fedoraProjections.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("proposal\nproposals\n"))) continue;
        const QJsonObject row = it.value().toObject();
        if (!Thoughts::ProposalDecisions::isRemovedOutside(row)) continue;
        const QString id = row.value(QStringLiteral("proposal_id")).toString();
        const QJsonObject snapshot = row.value(QStringLiteral("snapshot")).toObject();
        const QString sourceFile = row.value(QStringLiteral("source_file")).toString(snapshot.value(QStringLiteral("source_file")).toString());
        if (visibleProposalIds.contains(id) || sourceFile.isEmpty() || proposalSourceExcluded(docsRoot, sourceFile)) continue;
        const QString path = canonicalTexSourcePath(docsRoot, sourceFile);
        const auto owner = path.isEmpty() ? decltype(connectedSources.value(path)){}
                                          : discoverIncludedSources ? connectedSources.value(path) : discovered.value(path).owner;
        int documentIndex = unpublishedByProject.value(owner.projectId, -1);
        if (documentIndex < 0 && !owner.target.isEmpty()) {
            for (int index = 0; index < documents.size(); ++index) {
                if (documents.at(index).libraryFilename == owner.target) { documentIndex = index; break; }
            }
        }
        if (documentIndex < 0) {
            // The document whose comments or proposals come from the same source.
            for (int index = 0; index < documents.size() && documentIndex < 0; ++index) {
                const auto &comments = documents.at(index).comments;
                if (std::any_of(comments.cbegin(), comments.cend(), [&](const TrackedComment &comment) { return comment.sourceFile == sourceFile; })) documentIndex = index;
            }
        }
        if (documentIndex < 0) {
            TrackedCommentDocument local;
            local.displayName = owner.projectId.isEmpty() ? i18n("No project") : owner.projectName;
            if (!owner.projectId.isEmpty()) {
                local.stem = QStringLiteral("source-proposals:") + owner.projectId;
                local.libraryProjectId = owner.projectId;
                local.libraryKind = QStringLiteral("source_proposals");
                local.sourceProposalOnly = true;
            }
            documents.append(local);
            documentIndex = documents.size() - 1;
            unpublishedByProject.insert(owner.projectId, documentIndex);
        }
        TrackedCommentDocument &document = documents[documentIndex];
        const QJsonObject removal = row.value(QStringLiteral("removal")).toObject();
        TrackedComment comment;
        comment.documentStem = document.stem;
        comment.pdfPath = document.pdfPath;
        comment.pdfAvailable = document.pdfAvailable;
        comment.id = id;
        comment.threadRootId = id;
        comment.displayDate = Thoughts::agentProposalTimestampDateTime(snapshot.value(QStringLiteral("created_at")).toString());
        comment.modifiedDate = QDateTime::fromString(removal.value(QStringLiteral("removed_at")).toString(), Qt::ISODateWithMs);
        comment.rootSortDate = comment.modifiedDate.isValid() ? comment.modifiedDate : comment.displayDate;
        comment.contents = snapshot.value(QStringLiteral("comment")).toString();
        comment.proposalKind = QStringLiteral("edit");
        comment.proposalBeforePreview = snapshot.value(QStringLiteral("before")).toString();
        comment.proposalAfterPreview = snapshot.value(QStringLiteral("after")).toString();
        comment.selectedText = i18n("%1 → %2", comment.proposalBeforePreview.isEmpty() ? i18n("[empty]") : comment.proposalBeforePreview,
                                    comment.proposalAfterPreview.isEmpty() ? i18n("[delete]") : comment.proposalAfterPreview);
        comment.author = QStringLiteral("Agent");
        comment.sourceFile = sourceFile;
        comment.sourceLine = removal.value(QStringLiteral("source_line")).toInt();
        comment.anchorStatus = QStringLiteral("proposal_removed");
        comment.priorityRaw = QStringLiteral("normal");
        comment.agentProposal = true;
        comment.removedProposal = row;
        document.comments.append(comment);
        visibleProposalIds.insert(id);
        ++removedProposals;
    }
    recordStage(QStringLiteral("proposals removed outside Thoughts"), removedProposals);
    if (discoverIncludedSources) cachedProposalDiscovery(docsRoot, &refreshedDiscovery);
    if (historyIndexed) Thoughts::ProposalHistory::latest(docsRoot, &latestProposalEvents, &historyError);
    for (auto it = latestProposalEvents.cbegin(); it != latestProposalEvents.cend(); ++it) {
        const auto record = it.value();
        datesById.insert(it.key(), {QDateTime::fromString(record.value(QStringLiteral("first_seen_at")).toString(), Qt::ISODateWithMs),
                                   QDateTime::fromString(record.value(QStringLiteral("modified_at")).toString(), Qt::ISODateWithMs)});
    }
    recordStage(QStringLiteral("proposal current dates"), datesById.size());
    for (auto &document : documents) {
        if (!historyError.isEmpty()) document.error = historyError;
        for (auto &comment : document.comments) {
            if (!comment.agentProposal || !comment.removedProposal.isEmpty()) continue;
            const auto dates = datesById.value(comment.id);
            if (!comment.displayDate.isValid()) {
                comment.displayDate = dates.firstSeen;
                comment.dateIsFirstSeen = comment.displayDate.isValid();
            }
            const auto current = latestProposalEvents.value(comment.id);
            const auto snapshot = current.value(QStringLiteral("snapshot")).toObject();
            if (!snapshot.isEmpty() && current.value(QStringLiteral("source_after_sha256")).toString() == comment.proposalSourceSha256) {
                comment.contents = snapshot.value(QStringLiteral("comment")).toString();
                comment.proposalBeforePreview = snapshot.value(QStringLiteral("before")).toString();
                comment.proposalAfterPreview = snapshot.value(QStringLiteral("after")).toString();
            }
            comment.modifiedDate = dates.updated;
            comment.rootSortDate = dates.updated.isValid() ? dates.updated : comment.displayDate;
            // Live source still controls whether a proposal is pending; history
            // can describe an older accepted revision of the same stable ID.
        }
    }
    recordStage(QStringLiteral("proposal dates applied"), documents.size());
    LogicalConsolidationTiming consolidationTiming;
    consolidateLogicalComments(documents, fedoraProjections, &consolidationTiming);
    appendMissingCommentMarkers(documents, fedoraProjections);
    recordStage(QStringLiteral("logical comments consolidated"), documents.size());
    DeveloperLog::record(QStringLiteral("library.load.detail"), {{QStringLiteral("load_id"), loadId},
        {QStringLiteral("stage"), QStringLiteral("logical comment PDF hashes")},
        {QStringLiteral("duration_ms"), consolidationTiming.pdfHashMs},
        {QStringLiteral("pdf_hashes"), consolidationTiming.pdfHashCount},
        {QStringLiteral("pdf_bytes"), consolidationTiming.pdfBytes},
        {QStringLiteral("logical_comments"), consolidationTiming.projectionCount}});
    if (!onlyFilename.isEmpty()) {
        documents.removeIf([&onlyFilename](const TrackedCommentDocument &document) {
            return QFileInfo(document.pdfPath).fileName().compare(onlyFilename, Qt::CaseInsensitive) != 0;
        });
    }
    for (TrackedCommentDocument &document : documents) {
        rebuildTrackedCommentThreads(document);
    }
    recordStage(QStringLiteral("comment threads assembled"), documents.size());

    // Resolve the authoritative navigation catalogue in the worker, once per load.
    LibraryProjectStore navigationStore(docsRoot);
    QHash<QString, QJsonObject> projectsByFilename, projectsById;
    for (const auto &raw : navigationStore.projects(false)) {
        const auto project = raw.toObject();
        const auto id = project.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        projectsById.insert(id, project);
        const auto bind = [&](const QString &filename) {
            if (!filename.isEmpty() && !projectsByFilename.contains(filename)) projectsByFilename.insert(filename, project);
        };
        bind(project.value(QStringLiteral("target")).toString());
        for (const auto &rawDocument : project.value(QStringLiteral("documents")).toArray()) {
            const auto d = rawDocument.toObject();
            bind(d.value(QStringLiteral("filename")).toString());
            bind(d.value(QStringLiteral("attachment_filename")).toString());
        }
    }
    // Publication metadata is historical. Resolve current build ownership once
    // in the catalogue worker, retaining old PDFs and their annotation state.
    const auto settings = ProjectConfiguration::snapshotIdentity(docsRoot);
    bool ownershipKnown = !settings.isEmpty();
    QSet<QString> activeSourceOwners;
    QList<QJsonObject> configurations;
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        QString error;
        const auto configuration = ProjectConfiguration::read(it.key(), &error);
        if (!error.isEmpty()) { ownershipKnown = false; break; }
        configurations.append(configuration);
        for (const auto &raw : configuration.value(QStringLiteral("source_folders")).toArray())
            activeSourceOwners.insert(raw.toObject().value(QStringLiteral("path")).toString());
    }
    // Publications carry their Project document's identity, so an unchecked
    // document is matched however its PDF is named (rendering.output).
    const QSet<QString> unselectedDocumentIds = ownershipKnown ? ProjectConfiguration::unselectedSourceDocumentIds(configurations) : QSet<QString>();
    QHash<QString, QJsonObject> optionsByProject;
    QHash<QString, QList<QPair<QString, QString>>> standaloneByProject;
    for (TrackedCommentDocument &document : documents) {
        document.sourceBuildDetached = ownershipKnown && document.libraryKind == QLatin1String("source_linked")
            && !activeSourceOwners.contains(document.libraryProject);
        document.sourceUnselected = document.libraryKind == QLatin1String("source_linked") && !document.libraryDocumentId.isEmpty()
            && unselectedDocumentIds.contains(document.libraryDocumentId);
        const auto project = projectsById.contains(document.libraryProjectId) ? projectsById.value(document.libraryProjectId)
                                                                                 : projectsByFilename.value(document.libraryFilename);
        document.navigationProjectId = project.value(QStringLiteral("id")).toString(QStringLiteral("unassigned"));
        document.navigationProjectName = project.value(QStringLiteral("name")).toString();
        document.navigationProjectTarget = project.value(QStringLiteral("target")).toString();
        if (!document.libraryProject.isEmpty()) {
            const QString projectRoot = QDir(docsRoot).filePath(document.libraryProject);
            if (!optionsByProject.contains(projectRoot)) optionsByProject.insert(projectRoot, documentOptions(projectRoot));
            document.navigationOptions = optionsByProject.value(projectRoot);
            if (document.state == TrackedCommentDocument::SourceLinked || document.libraryKind == QLatin1String("source_linked")) {
                if (!standaloneByProject.contains(projectRoot)) standaloneByProject.insert(projectRoot, projectStandaloneSources(projectRoot));
                document.standaloneSources = standaloneByProject.value(projectRoot);
            }
        }
        const auto resolve = [&](const QString &source) {
            if (source.isEmpty() || document.navigationSourcePaths.contains(source)) return;
            const QFileInfo file(QDir(docsRoot).absoluteFilePath(source));
            const auto canonical = file.canonicalFilePath();
            document.navigationSourcePaths.insert(source, canonical.isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : canonical);
        };
        for (const auto &comment : document.comments) resolve(comment.sourceFile);
        for (const auto &raw : document.librarySegments) {
            const auto segment = raw.toObject();
            resolve(segment.value(QStringLiteral("primary_source_file")).toString(segment.value(QStringLiteral("source_id")).toString()));
            for (const auto &input : segment.value(QStringLiteral("inputs")).toArray()) resolve(input.toObject().value(QStringLiteral("source_file")).toString());
        }
    }
    recordStage(QStringLiteral("standalone source lists"), standaloneByProject.size());
    DeveloperLog::record(QStringLiteral("library.settings.snapshot"), {{QStringLiteral("projects"), optionsByProject.size()},
        {QStringLiteral("manifest_reads"), qint64(ProjectConfiguration::diskReadCount() - settingsReadsBefore)}});

    std::sort(documents.begin(), documents.end(), [](const TrackedCommentDocument &left, const TrackedCommentDocument &right) {
        const int displayOrder = left.displayName.compare(right.displayName, Qt::CaseInsensitive);
        return displayOrder == 0 ? left.stem.compare(right.stem, Qt::CaseInsensitive) < 0 : displayOrder < 0;
    });

    recordStage(QStringLiteral("final assembly"), documents.size());
    DeveloperLog::record(QStringLiteral("library.load.finished"), {{QStringLiteral("load_id"), loadId},
        {QStringLiteral("duration_ms"), totalElapsed.elapsed()}, {QStringLiteral("documents"), documents.size()},
        {QStringLiteral("sources"), sourcePaths.size()}, {QStringLiteral("artifacts"), artifacts.size()},
        {QStringLiteral("full_discovery"), discoverIncludedSources}});

    return documents;
}

QSet<QString> pendingLibraryRemovalFilenames(const QJsonObject &pendingChanges)
{
    QSet<QString> filenames;
    for (const QJsonValue &value : pendingChanges.value(QStringLiteral("library_operations")).toArray()) {
        const QJsonObject operation = value.toObject();
        const QString state = operation.value(QStringLiteral("state")).toString();
        if (operation.value(QStringLiteral("type")).toString() != QLatin1String("remove")
            || (state != QLatin1String("pending") && state != QLatin1String("in_flight") && state != QLatin1String("pending_activation"))) {
            continue;
        }
        const QString filename = operation.value(QStringLiteral("filename")).toString().trimmed();
        if (!filename.isEmpty()) {
            filenames.insert(filename.toCaseFolded());
        }
    }
    return filenames;
}

OrphanArchiveResult archiveOrphanCommentSidecars(const QString &docsRoot)
{
    OrphanArchiveResult result;
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    const QDir root(docsRoot);

    for (const OrphanSidecarInfo &orphan : orphanSidecars(docsRoot)) {
        const QString fileName = orphan.file.fileName();
        QString stem = fileName;
        stem.chop(QStringLiteral("_comments.jsonl").size());
        const QString archiveDirPath = root.filePath(QStringLiteral("comments/.archive/%1/%2").arg(stem, timestamp));
        if (!QDir().mkpath(archiveDirPath)) {
            result.errors.append(i18n("Could not create the archive directory for %1.", fileName));
            continue;
        }

        QFile source(orphan.file.absoluteFilePath());
        const bool readable = source.open(QIODevice::ReadOnly);
        const QByteArray contents = readable ? source.readAll() : QByteArray();
        source.close();
        QJsonObject manifest {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("stem"), stem},
            {QStringLiteral("original_sidecar"), QStringLiteral("comments/%1").arg(fileName)},
            {QStringLiteral("archived_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("reason"), QStringLiteral("orphaned_comment_history")},
            {QStringLiteral("readable"), readable},
            {QStringLiteral("parse_valid"), orphan.valid},
            {QStringLiteral("records"), orphan.records},
            {QStringLiteral("live_records"), orphan.liveRecords},
        };
        if (readable) {
            manifest.insert(QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()));
        } else {
            manifest.insert(QStringLiteral("sha256"), QJsonValue::Null);
        }

        QSaveFile manifestFile(QDir(archiveDirPath).filePath(QStringLiteral("manifest.json")));
        const QByteArray serializedManifest = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(serializedManifest) != serializedManifest.size() || !manifestFile.commit()) {
            QDir().rmdir(archiveDirPath);
            result.errors.append(i18n("Could not write the archive manifest for %1.", fileName));
            continue;
        }

        const QString archivedSidecar = QDir(archiveDirPath).filePath(fileName);
        if (!QFile::rename(orphan.file.absoluteFilePath(), archivedSidecar)) {
            QFile::remove(QDir(archiveDirPath).filePath(QStringLiteral("manifest.json")));
            QDir().rmdir(archiveDirPath);
            result.errors.append(i18n("Could not archive %1.", fileName));
            continue;
        }
        ++result.archived;
    }
    return result;
}

QString reviewCorrespondenceEvidenceText(const QJsonObject &correspondence)
{
    if (correspondence.isEmpty()) {
        return {};
    }
    QStringList lines {
        i18n("State: %1", correspondence.value(QStringLiteral("state")).toString()),
        i18n("Reason: %1", correspondence.value(QStringLiteral("reason")).toString()),
        i18n("Source bundle SHA-256: %1", correspondence.value(QStringLiteral("source_bundle_sha256")).toString()),
    };
    for (const QJsonValue &value : (correspondence.value(QStringLiteral("current_source_targets")).toArray().isEmpty() ? correspondence.value(QStringLiteral("source_targets")).toArray() : correspondence.value(QStringLiteral("current_source_targets")).toArray())) {
        const QJsonObject target = value.toObject();
        lines.append(i18n("Source %1: %2:%3-%4 · bytes %5-%6 · %7",
                          target.value(QStringLiteral("role")).toString(),
                          target.value(QStringLiteral("source_file")).toString(),
                          target.value(QStringLiteral("line_start")).toInt(),
                          target.value(QStringLiteral("line_end")).toInt(),
                          target.value(QStringLiteral("byte_start")).toInteger(),
                          target.value(QStringLiteral("byte_end")).toInteger(),
                          target.value(QStringLiteral("render_intent")).toString()));
        lines.append(i18n("Source text SHA-256: %1", target.value(QStringLiteral("text_sha256")).toString()));
        const QString preview = target.value(QStringLiteral("preview")).toString();
        if (!preview.isEmpty()) {
            lines.append(i18n("Source preview: %1", preview));
        }
    }
    for (const QJsonValue &value : correspondence.value(QStringLiteral("locations")).toArray()) {
        const QJsonObject location = value.toObject();
        const QJsonObject proof = location.value(QStringLiteral("proof")).toObject();
        lines.append(i18n("PDF: %1 · page %2 · %3 · %4 region(s)",
                          location.value(QStringLiteral("pdf_filename")).toString(),
                          location.value(QStringLiteral("page")).toInt(),
                          location.value(QStringLiteral("placement_kind")).toString(),
                          location.value(QStringLiteral("regions")).toArray().size()));
        lines.append(i18n("PDF SHA-256: %1", location.value(QStringLiteral("pdf_sha256")).toString()));
        QString buildHash = location.value(QStringLiteral("build_sha256")).toString();
        if (buildHash.isEmpty()) {
            buildHash = proof.value(QStringLiteral("build_sha256")).toString();
        }
        if (!buildHash.isEmpty()) {
            lines.append(i18n("Component build SHA-256: %1", buildHash));
        }
        lines.append(i18n("Proof: %1 · candidates %2 · resolver v%3",
                          proof.value(QStringLiteral("method")).toString(),
                          proof.value(QStringLiteral("candidate_count")).toInt(),
                          proof.value(QStringLiteral("resolver_version")).toInt()));
        QString uniqueness = proof.value(QStringLiteral("uniqueness")).toString();
        if (uniqueness.isEmpty()) {
            uniqueness = proof.value(QStringLiteral("uniqueness_result")).toString();
        }
        if (!uniqueness.isEmpty()) {
            lines.append(i18n("Uniqueness: %1", uniqueness));
        }
        QString extractedHash = proof.value(QStringLiteral("extracted_text_sha256")).toString();
        if (extractedHash.isEmpty()) {
            extractedHash = proof.value(QStringLiteral("extracted_pdf_text_sha256")).toString();
        }
        if (!extractedHash.isEmpty()) {
            lines.append(i18n("Extracted PDF text SHA-256: %1", extractedHash));
        }
        if (proof.value(QStringLiteral("human_confirmation")).isObject()) {
            lines.append(i18n("Human-confirmed for this exact source/PDF revision."));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

CommentsOverview::CommentsOverview(const QString &docsRoot,
                                   QWidget *parent,
                                   ReviewCardPresenter reviewCardPresenter,
                                   ReviewCardReviewer reviewCardReviewer,
                                   ProjectionProvider projectionProvider)
    : QDialog(parent, Qt::Window)
    , m_docsRoot(QFileInfo(docsRoot).absoluteFilePath())
{
    m_librarySourceInventory = LibrarySourceInventory(m_docsRoot).cached();
    setObjectName(QStringLiteral("commentsOverview"));
    const QIcon thoughtsIcon = QIcon::fromTheme(QStringLiteral("thoughts"), QApplication::windowIcon());
    setWindowTitle(i18n("Thoughts"));
    setWindowIcon(thoughtsIcon);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1120, 760);

    auto *layout = new QVBoxLayout(this);
    auto *headingLayout = new QHBoxLayout;
    headingLayout->setSpacing(8);
    auto *heading = new QLabel(i18n("Thoughts"), this);
    heading->setObjectName(QStringLiteral("commentsOverviewHeading"));
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.35);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    heading->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int headingIconExtent = qBound(24, heading->fontMetrics().height() + 4, 32);
    auto *headingIcon = new QLabel(this);
    headingIcon->setObjectName(QStringLiteral("commentsOverviewHeadingIcon"));
    headingIcon->setFixedSize(headingIconExtent, headingIconExtent);
    headingIcon->setPixmap(thoughtsIcon.pixmap(QSize(headingIconExtent, headingIconExtent)));
    headingIcon->setAlignment(Qt::AlignCenter);
    headingIcon->setAccessibleName(i18n("Thoughts symbol"));
    headingLayout->addWidget(headingIcon);
    headingLayout->addWidget(heading);
    headingLayout->addSpacing(12);
    m_librarySearchIndexPath = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("library-search.sqlite"));
    m_librarySearch = new LibrarySearchField(m_librarySearchIndexPath, this);
    m_librarySearch->setObjectName(QStringLiteral("librarySearchField"));
    m_librarySearch->setAccessibleName(i18n("Search the Library"));
    {
        const KConfigGroup searchGroup(KSharedConfig::openConfig(), QStringLiteral("LibrarySearch"));
        m_librarySearch->setScope(LibrarySearchField::scopeFromKey(searchGroup.readEntry("Scope", QString())));
    }
    const auto updateLibrarySearchPlaceholder = [this] {
        const QString shortcut = QKeySequence(Qt::CTRL | Qt::Key_K).toString(QKeySequence::NativeText);
        switch (m_librarySearch->scope()) {
        case LibrarySearchScope::Sources:
            m_librarySearch->setPlaceholderText(i18n("Search source files and their text (%1)", shortcut));
            break;
        case LibrarySearchScope::All:
            m_librarySearch->setPlaceholderText(i18n("Search documents and sources (%1)", shortcut));
            break;
        case LibrarySearchScope::Documents:
            m_librarySearch->setPlaceholderText(i18n("Search PDFs, comments and review cards (%1)", shortcut));
            break;
        }
    };
    updateLibrarySearchPlaceholder();
    connect(m_librarySearch, &LibrarySearchField::scopeChanged, this, [this, updateLibrarySearchPlaceholder](LibrarySearchScope scope) {
        updateLibrarySearchPlaceholder();
        KConfigGroup searchGroup(KSharedConfig::openConfig(), QStringLiteral("LibrarySearch"));
        searchGroup.writeEntry("Scope", LibrarySearchField::scopeKey(scope));
        searchGroup.sync();
        DeveloperLog::record(QStringLiteral("library.search.scope"), QJsonObject{{QStringLiteral("scope"), LibrarySearchField::scopeKey(scope)}});
        if (scope != LibrarySearchScope::Documents) scheduleLibrarySourceSearchUpdate(true);
    });
    m_librarySearch->setMinimumWidth(300);
    m_librarySearch->setMaximumWidth(560);
    headingLayout->addWidget(m_librarySearch, 3);
    headingLayout->addStretch(1);
    auto *librarySearchShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    librarySearchShortcut->setContext(Qt::WindowShortcut);
    connect(librarySearchShortcut, &QShortcut::activated, this, [this] {
        m_librarySearch->setFocus(Qt::ShortcutFocusReason);
        m_librarySearch->selectAll();
    });
    connect(m_librarySearch, &LibrarySearchField::hitActivated, this, &CommentsOverview::openLibrarySearchHit);
    m_pdfSyncStatus = new QLabel(i18n("Checking sync status…"), this);
    m_pdfSyncStatus->setObjectName(QStringLiteral("commentsOverviewPdfSyncStatus"));
    m_pdfSyncStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pdfSyncTimestamp = new QLabel(this);
    m_pdfSyncTimestamp->setObjectName(QStringLiteral("commentsOverviewPdfSyncTimestamp"));
    m_pdfSyncTimestamp->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont timestampFont = m_pdfSyncTimestamp->font();
    timestampFont.setPointSizeF(timestampFont.pointSizeF() * 0.85);
    m_pdfSyncTimestamp->setFont(timestampFont);
    m_syncLogsLink = new QLabel(this);
    m_syncLogsLink->setObjectName(QStringLiteral("commentsOverviewSyncLogsLink"));
    m_syncLogsLink->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_syncLogsLink->setFont(timestampFont);
    m_syncLogsLink->setText(QStringLiteral("<a href=\"logs\">%1</a>").arg(SyncStatusLabels::showLogs().toHtmlEscaped()));
    m_syncLogsLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_syncLogsLink->setToolTip(i18n("Open logs for the failed operation"));
    connect(m_syncLogsLink, &QLabel::linkActivated, this, [this] {
        if (m_syncLinkShowsMerge) {
            Q_EMIT sourceMergeRequested();
            return;
        }
        Q_EMIT syncOperationLogsRequested(m_latestSyncOperation.value(QStringLiteral("id")).toString());
    });
    auto *pdfStatusLayout = new QVBoxLayout;
    pdfStatusLayout->setContentsMargins(0, 0, 0, 0);
    pdfStatusLayout->setSpacing(0);
    pdfStatusLayout->addWidget(m_pdfSyncStatus);
    pdfStatusLayout->addWidget(m_pdfSyncTimestamp);
    pdfStatusLayout->addWidget(m_syncLogsLink);
    headingLayout->addLayout(pdfStatusLayout);
    m_idlePdfSyncStatusText = m_pdfSyncStatus->text();
    updateIdleSyncPresentation();
    m_syncElapsedTimer = new QTimer(this);
    m_syncElapsedTimer->setInterval(1000);
    connect(m_syncElapsedTimer, &QTimer::timeout, this, &CommentsOverview::updateSyncElapsedTimestamp);
    auto *syncAgeTimer = new QTimer(this);
    syncAgeTimer->setInterval(60000);
    connect(syncAgeTimer, &QTimer::timeout, this, [this] {
        if (!m_syncOperationRunning) {
            updateIdleSyncPresentation();
        }
    });
    syncAgeTimer->start();
    m_reviewCardsButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-task")), i18n("Review cards"), this);
    m_reviewCardsButton->setObjectName(QStringLiteral("commentsOverviewReviewCards"));
    m_reviewCardsButton->setToolTip(i18n("Open Review cards"));
    m_reviewCardsButton->setAutoDefault(false);
    m_agentNotesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("mail-message")), i18n("Agent notes"), this);
    m_agentNotesButton->setObjectName(QStringLiteral("commentsOverviewAgentNotes"));
    m_agentNotesButton->setToolTip(i18n("Read the notes agents sent to your phone, with your replies"));
    m_agentNotesButton->setAutoDefault(false);
    m_syncButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Sync"), this);
    m_syncButton->setObjectName(QStringLiteral("commentsOverviewSync"));
    m_syncButton->setToolTip(i18n("Publish source edits, synchronize changes, and update only PDFs whose build inputs changed"));
    m_syncButton->setAutoDefault(false);
    headingLayout->addWidget(m_syncButton);
    headingLayout->addWidget(m_agentNotesButton);
    headingLayout->addWidget(m_reviewCardsButton);
    layout->addLayout(headingLayout);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("thoughtsOverviewTabs"));
    m_summaryPage = new QWidget(m_tabs);
    m_summaryPage->setObjectName(QStringLiteral("thoughtsOverviewSummaryPage"));
    auto *summaryGrid = new QGridLayout(m_summaryPage);
    summaryGrid->setContentsMargins(8, 12, 8, 8);
    summaryGrid->setHorizontalSpacing(12);
    summaryGrid->setVerticalSpacing(12);
    const auto summaryGroup = [this, summaryGrid](const QString &title, const QString &objectName, int row, int column) {
        auto *group = new QGroupBox(title, m_summaryPage);
        group->setObjectName(objectName + QStringLiteral("Group"));
        auto *groupLayout = new QVBoxLayout(group);
        auto *label = new QLabel(group);
        label->setObjectName(objectName);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        groupLayout->addWidget(label);
        groupLayout->addStretch(1);
        summaryGrid->addWidget(group, row, column);
        return qMakePair(group, label);
    };

    auto *libraryGroup = new QGroupBox(i18n("Library"), m_summaryPage);
    libraryGroup->setObjectName(QStringLiteral("thoughtsOverviewLibrarySummaryGroup"));
    auto *libraryLayout = new QVBoxLayout(libraryGroup);
    m_librarySummary = new QLabel(libraryGroup);
    m_librarySummary->setObjectName(QStringLiteral("thoughtsOverviewLibrarySummary"));
    m_librarySummary->setTextFormat(Qt::PlainText);
    m_librarySummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_librarySummary->setWordWrap(true);
    auto *libraryHeader = new QHBoxLayout;
    libraryHeader->addWidget(m_librarySummary, 1);
    m_trackPdfButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Add project…"), libraryGroup);
    m_trackPdfButton->setObjectName(QStringLiteral("commentsOverviewTrackPdf"));
    m_trackPdfButton->setAutoDefault(false);
    libraryHeader->addWidget(m_trackPdfButton);
    libraryLayout->addLayout(libraryHeader);
    m_libraryTree = new LibraryTree(libraryGroup);
    m_libraryTree->setObjectName(QStringLiteral("thoughtsOverviewLibraryTree"));
    m_libraryTree->setColumnCount(LibraryColumnCount);
    m_libraryTree->setHeaderLabels({i18n("Project"), i18n("Date"), i18n("Tracked sources"), i18n("Size"), i18n("Review cards"), i18n("Pages"), i18n("Files"),
        i18n("Source pages"), i18n("PDF pages"), i18n("Due cards"), i18n("Documents")});
    m_libraryTree->setRootIsDecorated(true);
    m_libraryTree->setAlternatingRowColors(false);
    m_libraryTree->setUniformRowHeights(false);
    m_libraryTree->setIndentation(18);
    m_libraryTree->setIconSize(QSize(18, 18));
    m_libraryTree->setItemDelegate(new LibraryTreeDelegate(m_libraryTree));
    m_libraryTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_libraryTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_libraryTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_libraryTree->setMinimumHeight(300);
    m_libraryTree->headerItem()->setToolTip(LibraryFilesColumn, i18n("Number of source files included in Thoughts Sync, after .ThoughtIgnore rules."));
    m_libraryTree->headerItem()->setToolTip(1, i18n("Last built for projects and combined PDFs; Modified for source files and folders."));
    m_libraryTree->headerItem()->setToolTip(LibrarySourcePagesColumn, i18n("Pages of PDFs built from linked sources (LaTeX, Markdown)."));
    m_libraryTree->headerItem()->setToolTip(LibraryPdfPagesColumn, i18n("Pages of imported PDFs, which have no linked sources."));
    m_libraryTree->headerItem()->setToolTip(LibraryDueCardsColumn, i18n("Review cards due now, as the review queue counts them."));
    m_libraryTree->headerItem()->setToolTip(LibraryDocumentsColumn, i18n("Number of documents in the project, not counting unpublished ones."));
    m_libraryTree->header()->setSectionsClickable(true);
    m_libraryTree->header()->setSortIndicatorShown(true);
    // Right-click the header to choose the columns; drag a header to move its column. Restoring
    // also sets the sizes: Project takes the free width, the other columns fit their contents.
    m_libraryTree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    restoreLibraryColumns();
    connect(m_libraryTree->header(), &QHeaderView::customContextMenuRequested, this, &CommentsOverview::showLibraryHeaderMenu);
    connect(m_libraryTree->header(), &QHeaderView::sectionMoved, this, [this] {
        if (!m_libraryColumnsUpdating) saveLibraryColumns();
    });
    m_libraryTree->setSortingEnabled(true);
    m_libraryTree->sortByColumn(0, Qt::AscendingOrder);
    libraryLayout->addWidget(m_libraryTree, 1);
    m_librarySelectionDetails = new QLabel(libraryGroup);
    m_librarySelectionDetails->setObjectName(QStringLiteral("thoughtsLibrarySelectionDetails"));
    m_librarySelectionDetails->setTextFormat(Qt::PlainText);
    m_librarySelectionDetails->setWordWrap(true);
    m_librarySelectionDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_librarySelectionDetails->hide();
    libraryLayout->addWidget(m_librarySelectionDetails);
    connect(m_libraryTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_librarySelectionDetails->setText(librarySelectionDetail(item));
        m_librarySelectionDetails->setVisible(!m_librarySelectionDetails->text().isEmpty());
    });
    summaryGrid->addWidget(libraryGroup, 0, 0, 1, 2);

    const auto syncGroup = summaryGroup(i18n("Sync & builds"), QStringLiteral("thoughtsOverviewSyncBuildSummary"), 1, 0);
    m_syncBuildSummary = syncGroup.second;
    summaryGrid->removeWidget(syncGroup.first);
    syncGroup.first->hide();

    auto *storageGroup = new QGroupBox(i18n("Server storage"), m_summaryPage);
    storageGroup->setObjectName(QStringLiteral("thoughtsOverviewFedoraStorageGroup"));
    auto *storageLayout = new QVBoxLayout(storageGroup);
    m_fedoraStorageProgress = new QProgressBar(storageGroup);
    m_fedoraStorageProgress->setObjectName(QStringLiteral("thoughtsOverviewFedoraStorageProgress"));
    m_fedoraStorageProgress->setRange(0, 1000);
    m_fedoraStorageProgress->setTextVisible(false);
    m_fedoraStorageProgress->hide();
    storageLayout->addWidget(m_fedoraStorageProgress);
    m_fedoraStorage = new QLabel(i18n("Checking server storage…"), storageGroup);
    m_fedoraStorage->setObjectName(QStringLiteral("commentsOverviewFedoraStorage"));
    m_fedoraStorage->setTextFormat(Qt::PlainText);
    m_fedoraStorage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fedoraStorage->setWordWrap(true);
    storageLayout->addWidget(m_fedoraStorage);
    m_fedoraStorageCleanup = new QLabel(i18n("Completed build workspaces are cleaned automatically."), storageGroup);
    m_fedoraStorageCleanup->setObjectName(QStringLiteral("thoughtsOverviewFedoraStorageCleanup"));
    m_fedoraStorageCleanup->setWordWrap(true);
    auto *storageFooterLayout = new QHBoxLayout;
    storageFooterLayout->addWidget(m_fedoraStorageCleanup, 1);
    m_fedoraDetailsButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Details…"), storageGroup);
    m_fedoraDetailsButton->setObjectName(QStringLiteral("thoughtsOverviewFedoraDetailsButton"));
    m_fedoraDetailsButton->setToolTip(i18n("Show a short health summary for the server"));
    m_fedoraDetailsButton->setAutoDefault(false);
    m_fedoraDetailsButton->setEnabled(false);
    storageFooterLayout->addWidget(m_fedoraDetailsButton, 0, Qt::AlignTop);
    storageLayout->addLayout(storageFooterLayout);
    storageLayout->addStretch(1);
    storageGroup->hide();
    summaryGrid->setColumnStretch(0, 1);
    summaryGrid->setColumnStretch(1, 1);
    summaryGrid->setRowStretch(0, 1);
    summaryGrid->setRowStretch(1, 0);
    connect(m_fedoraDetailsButton, &QPushButton::clicked, this, &CommentsOverview::showFedoraDetails);

    m_commentsPage = new QWidget(m_tabs);
    m_commentsPage->setObjectName(QStringLiteral("thoughtsOverviewCommentsPage"));
    m_notesPage = new QWidget(m_tabs);
    m_notesPage->setObjectName(QStringLiteral("thoughtsOverviewNotesPage"));
    auto *notesPageLayout = new QVBoxLayout(m_notesPage);
    notesPageLayout->setContentsMargins(0, 0, 0, 0);
    auto *commentsPageLayout = new QVBoxLayout(m_commentsPage);
    commentsPageLayout->setContentsMargins(0, 0, 0, 0);
    m_reviewPage = new QTabWidget(m_tabs);
    m_reviewPage->setObjectName(QStringLiteral("thoughtsOverviewReviewPage"));
    m_reviewPage->setDocumentMode(true);
    m_reviewCardsWidget = new ReviewCardsWidget(m_docsRoot,
                                                std::move(reviewCardPresenter),
                                                std::move(reviewCardReviewer),
                                                m_reviewPage,
                                                std::move(projectionProvider));
    m_reviewCardsWidget->setObjectName(QStringLiteral("thoughtsOverviewReviewCardsPage"));
    connect(m_reviewCardsWidget, &ReviewCardsWidget::countsChanged, this, &CommentsOverview::updateReviewCardsButton);
    connect(m_reviewCardsWidget, &ReviewCardsWidget::countsChanged, this, [this] { applyLibraryReviewCounts(); });
    connect(m_reviewCardsWidget, &ReviewCardsWidget::countsChanged, this, [this] {
        m_librarySearchCardsKnown = true;
        scheduleLibrarySearchUpdate();
    });
    connect(m_reviewCardsWidget, &ReviewCardsWidget::openSourceRequested, this, &CommentsOverview::openSourceRequested);
    connect(m_reviewCardsWidget, &ReviewCardsWidget::pdfVersionOpenRequested, this, &CommentsOverview::pdfVersionOpenRequested);
    updateReviewCardsButton(m_reviewCardsWidget->activeCardCount(), m_reviewCardsWidget->dueCardCount());
    m_tabs->addTab(m_summaryPage, i18n("Projects"));
    m_tabs->addTab(m_notesPage, i18n("To-dos"));
    m_diaryPage = new QWidget(m_tabs);
    m_diaryPage->setObjectName(QStringLiteral("thoughtsOverviewDiaryPage"));
    m_tabs->addTab(m_diaryPage, i18n("Diary"));
    m_tabs->addTab(m_commentsPage, i18n("Annotations"));
    m_mentalModelView = new MentalModelView(m_reviewPage);
    connect(m_mentalModelView, &MentalModelView::cardRequested, this, &CommentsOverview::showReviewCardInfo);
    // A rewritten Mental Model note goes to the server with a Sync, as a note edited here would.
    connect(m_mentalModelView, &MentalModelView::libraryNoteWritten, this, &CommentsOverview::syncRequested);
    m_reviewPage->addTab(m_reviewCardsWidget, i18n("Browse cards"));
    m_reviewPage->addTab(m_mentalModelView, i18n("Mental Model"));
    connect(m_reviewPage, &QTabWidget::currentChanged, this, [this] {
        if (m_reviewPage->currentWidget() == m_reviewCardsWidget) m_reviewCardsWidget->ensureLoaded();
        else m_mentalModelView->reload();
    });
    m_tabs->addTab(m_reviewPage, i18n("Review"));
    // Preserve the comments surface for direct callers; the application entry
    // point explicitly selects Projects before presenting Thoughts Overview.
    m_tabs->setCurrentWidget(m_commentsPage);
    layout->addWidget(m_tabs, 1);

    m_model = new QStandardItemModel(0, ColumnCount, this);
    const auto createBrowser = [this](QWidget *page, bool notes) {
        auto *itemsBrowser = new QWidget(page);
        page->layout()->addWidget(itemsBrowser);
        auto *commentsLayout = new QVBoxLayout(itemsBrowser);
        commentsLayout->setContentsMargins(0, 8, 0, 0);
        m_search = new QLineEdit(this);
        m_search->setObjectName(QStringLiteral("commentsOverviewSearch"));
        m_search->setClearButtonEnabled(true);
        m_search->setPlaceholderText(i18n("Search comments, proposals, documents, authors, locations, or source files…"));
        auto *filterLayout = new QHBoxLayout;
        filterLayout->addWidget(m_search, 1);
        m_typeFilter = new QComboBox(this);
        m_typeFilter->setObjectName(QStringLiteral("commentsOverviewTypeFilter"));
        m_typeFilter->addItem(i18n("All types"), QString());
        m_typeFilter->addItem(i18n("Like"), QStringLiteral("like"));
        m_typeFilter->addItem(i18n("Dislike"), QStringLiteral("dislike"));
        m_typeFilter->addItem(i18n("Question"), QStringLiteral("question"));
        m_typeFilter->addItem(i18n("Comments"), QStringLiteral("comment"));
        m_typeFilter->addItem(i18n("Drawings"), Thoughts::DocumentFeedback::drawing());
        m_typeFilter->addItem(i18n("Text boxes"), Thoughts::DocumentFeedback::textBox());
        m_typeFilter->addItem(i18n("Agent proposals"), QStringLiteral("agent_remove"));
        filterLayout->addWidget(m_typeFilter);
        if (!notes) {
            m_showEarlierFeedback = new QCheckBox(i18n("Earlier versions"), this);
            m_showEarlierFeedback->setObjectName(QStringLiteral("commentsOverviewShowEarlier"));
            m_showEarlierFeedback->setToolTip(i18n("Also list resolved comments, and likes and dislikes of earlier versions of a document"));
            filterLayout->addWidget(m_showEarlierFeedback);
        }
        if (notes) {
            m_showHiddenTodos = new QCheckBox(i18n("Show hidden"), this);
            m_showHiddenTodos->setObjectName(QStringLiteral("notesOverviewShowHidden"));
            m_showHiddenTodos->setToolTip(i18n("Also list to-dos hidden until a later date"));
            filterLayout->addWidget(m_showHiddenTodos);
            auto *openCalendar = new QPushButton(QIcon::fromTheme(QStringLiteral("view-calendar")), i18n("Open Calendar"), this);
            openCalendar->setObjectName(QStringLiteral("notesOverviewOpenCalendar"));
            openCalendar->setToolTip(i18n("Open today in Google Calendar"));
            openCalendar->setAutoDefault(false);
            connect(openCalendar, &QPushButton::clicked, this, [] {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://calendar.google.com/calendar/r/day")));
            });
            filterLayout->addWidget(openCalendar);
        }
        commentsLayout->addLayout(filterLayout);
        m_summary = new QLabel(m_commentsPage);
        m_summary->setObjectName(QStringLiteral("commentsOverviewSummary"));
        m_summary->setTextFormat(Qt::PlainText);
        m_summary->setWordWrap(true);
        commentsLayout->addWidget(m_summary);

        m_splitter = new QSplitter(this);
        m_splitter->setObjectName(QStringLiteral("commentsOverviewSplitter"));
        m_documentList = new QTreeWidget;
        m_documentList->setColumnCount(4);
        m_documentList->setHeaderLabels({i18n("Project / document"), i18n("Comments"), i18n("Agent proposals"), i18n("To-dos")});
        m_documentList->setRootIsDecorated(true);
        m_documentList->setUniformRowHeights(true);
        m_documentList->setIndentation(16);
        m_documentList->setExpandsOnDoubleClick(false);
        m_documentList->header()->setStretchLastSection(false);
        m_documentList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_documentList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_documentList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_documentList->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_documentList->setObjectName(QStringLiteral("commentsOverviewDocuments"));
        m_documentList->setMinimumWidth(440);
        m_documentList->setContextMenuPolicy(Qt::CustomContextMenu);

        m_table = new CommentNavigationTree;
        m_table->setObjectName(QStringLiteral("commentsOverviewTable"));
        m_table->setAlternatingRowColors(true);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_table->setContextMenuPolicy(Qt::CustomContextMenu);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSortingEnabled(true);
        m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
        m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        m_table->setWordWrap(false);
        m_table->setUniformRowHeights(true);
        m_table->setRootIsDecorated(true);
        m_table->setItemsExpandable(true);
        m_table->setExpandsOnDoubleClick(false);
        m_table->horizontalScrollBar()->setSingleStep(10);

        m_model->setHorizontalHeaderLabels({i18n("Comment"), i18n("Document"), i18n("Page"), i18n("Snapshot"), i18n("Source"), i18n("Status"), i18n("Author"), i18n("Created"), i18n("Modified"), i18n("Source doc")});
        m_model->horizontalHeaderItem(SourceDocumentColumn)->setToolTip(i18n("✓ the document is built from a LaTeX or Markdown source; ✗ it is a PDF"));
        auto *proxy = new CommentsFilterProxyModel(m_table);
        proxy->setSourceModel(m_model);
        proxy->setSortRole(SortRole);
        proxy->setNotesOnly(notes);
        m_proxy = proxy;
        if (notes) {
            connect(m_showHiddenTodos, &QCheckBox::toggled, proxy, [this, proxy](bool show) {
                proxy->setShowHiddenTodos(show);
                updateCommentSummary();
            });
        } else {
            connect(m_showEarlierFeedback, &QCheckBox::toggled, this, [this] {
                populateDocumentList(selectedDocument());
                populateComments();
                applyFilters();
            });
        }
        m_table->setModel(m_proxy);
        configureOverviewHeader(m_table);
        m_table->sortByColumn(ModifiedColumn, Qt::DescendingOrder);

        m_contentSplitter = new QSplitter(Qt::Horizontal);
        m_contentSplitter->setObjectName(QStringLiteral("commentsOverviewContentSplitter"));
        m_contentSplitter->addWidget(m_table);

        m_splitter->addWidget(m_documentList);
        m_splitter->addWidget(m_contentSplitter);
        m_documentList->setVisible(!notes);
        m_splitter->setStretchFactor(0, 0);
        m_splitter->setStretchFactor(1, 1);
        commentsLayout->addWidget(m_splitter, 1);
        if (notes) {
            auto *noteActions = new QHBoxLayout;
            m_addNoteButton = new QPushButton(QStringLiteral("+"), itemsBrowser);
            m_addNoteButton->setObjectName(QStringLiteral("notesOverviewAddNote"));
            m_addNoteButton->setToolTip(i18n("Add To-do"));
            m_addNoteButton->setAccessibleName(i18n("Add To-do"));
            m_addNoteButton->setFixedWidth(m_addNoteButton->sizeHint().height());
            noteActions->addWidget(m_addNoteButton);
            noteActions->addStretch(1);
            commentsLayout->addLayout(noteActions);
        }
        m_typeFilter->setVisible(!notes);
        m_search->setPlaceholderText(notes ? i18n("Search to-dos…") : i18n("Search comments, proposals, documents, authors, locations, or source files…"));
        BrowserState &browser = notes ? m_notesBrowser : m_commentsBrowser;
        browser.notes = notes;
        browser.widget = itemsBrowser;
        browser.search = m_search;
        browser.typeFilter = m_typeFilter;
        browser.documentList = m_documentList;
        browser.table = m_table;
        browser.proxy = m_proxy;
        browser.summary = m_summary;
        browser.splitter = m_splitter;
        browser.contentSplitter = m_contentSplitter;
        if (notes) {
            // Keep stable, unambiguous object names for accessibility and tests.
            for (QObject *object : QList<QObject *> {m_search, m_typeFilter, m_documentList, m_table, m_summary, m_splitter, m_contentSplitter}) {
                QString name = object->objectName();
                name.replace(QStringLiteral("commentsOverview"), QStringLiteral("notesOverview"));
                object->setObjectName(name);
            }
        }
    };
    createBrowser(m_notesPage, true);
    createBrowser(m_commentsPage, false);
    applyBrowserColumns(m_notesBrowser);
    applyBrowserColumns(m_commentsBrowser);

    // Diary: quick timestamped entries. They are notes like to-dos, read from
    // the same comment model, and are saved, edited and deleted the same way.
    {
        auto *diaryLayout = new QVBoxLayout(m_diaryPage);
        diaryLayout->setContentsMargins(0, 8, 0, 0);
        auto *input = new DiaryInput(m_diaryPage);
        input->setObjectName(QStringLiteral("diaryOverviewInput"));
        input->setPlaceholderText(i18n("Write a diary entry… Enter saves it, Shift+Enter starts a new line."));
        input->setAccessibleName(i18n("New diary entry"));
        input->setTabChangesFocus(true);
        input->setFixedHeight(input->fontMetrics().lineSpacing() * 3 + 2 * (input->frameWidth() + qRound(input->document()->documentMargin())));
        input->submit = [this] { createDiaryEntry(); };
        m_diaryInput = input;
        diaryLayout->addWidget(m_diaryInput);
        m_diarySummary = new QLabel(m_diaryPage);
        m_diarySummary->setObjectName(QStringLiteral("diaryOverviewSummary"));
        m_diarySummary->setTextFormat(Qt::PlainText);
        m_diarySummary->setWordWrap(true);
        diaryLayout->addWidget(m_diarySummary);
        auto *entries = new DiaryEntryTree(m_diaryPage);
        entries->setObjectName(QStringLiteral("diaryOverviewEntries"));
        entries->setColumnCount(2);
        entries->setHeaderHidden(true);
        entries->setRootIsDecorated(false);
        entries->setItemsExpandable(false);
        entries->setExpandsOnDoubleClick(false);
        entries->setIndentation(0);
        entries->setWordWrap(true);
        entries->setUniformRowHeights(false);
        entries->setSelectionMode(QAbstractItemView::ExtendedSelection);
        entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
        entries->header()->setStretchLastSection(true);
        entries->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        entries->editRequested = [this](const QString &id) { editDiaryEntry(id); };
        m_diaryList = entries;
        diaryLayout->addWidget(m_diaryList, 1);

        auto *deleteShortcut = new QShortcut(m_diaryList);
        QList<QKeySequence> deleteKeys = QKeySequence::keyBindings(QKeySequence::Delete);
        for (const QKeySequence &key : {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}) {
            if (!deleteKeys.contains(key)) deleteKeys.append(key);
        }
        deleteShortcut->setKeys(deleteKeys);
        deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(deleteShortcut, &QShortcut::activated, this, &CommentsOverview::deleteSelectedDiaryEntries);
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            auto *editShortcut = new QShortcut(QKeySequence(key), m_diaryList);
            editShortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(editShortcut, &QShortcut::activated, this, [this] {
                if (const QTreeWidgetItem *item = m_diaryList->currentItem()) editDiaryEntry(item->data(0, DiaryIdRole).toString());
            });
        }
        populateDiary();
    }

    m_details = new DetailsPanel;
    auto *details = m_details;
    details->setObjectName(QStringLiteral("commentsOverviewDetails"));
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(8, 0, 0, 0);

    m_commentMetadata = new QLabel(details);
    m_commentMetadata->setObjectName(QStringLiteral("commentsOverviewMetadata"));
    m_commentMetadata->setTextFormat(Qt::PlainText);
    m_commentMetadata->setWordWrap(true);
    // Wrapped text must not widen the panel to fit its longest unbroken word.
    m_commentMetadata->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_commentMetadata->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLayout->addWidget(m_commentMetadata);

    m_selectedText = new QLabel(details);
    m_selectedText->setObjectName(QStringLiteral("commentsOverviewSelectedText"));
    m_selectedText->setWordWrap(true);
    m_selectedText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_selectedText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLayout->addWidget(m_selectedText);
    m_proposalPreview = new QPlainTextEdit(details);
    m_proposalPreview->setObjectName(QStringLiteral("commentsOverviewProposalPreview"));
    m_proposalPreview->setReadOnly(true);
    m_proposalPreview->setMinimumHeight(120);
    m_proposalPreview->hide();
    detailsLayout->addWidget(m_proposalPreview, 1);

    m_correspondenceEvidence = new QPlainTextEdit(details);
    m_correspondenceEvidence->setObjectName(QStringLiteral("commentsOverviewCorrespondenceEvidence"));
    m_correspondenceEvidence->setReadOnly(true);
    m_correspondenceEvidence->setMinimumHeight(120);
    m_correspondenceEvidence->setMaximumHeight(210);
    m_correspondenceEvidence->setPlaceholderText(i18n("No proof-carrying correspondence is available."));
    m_correspondenceEvidence->hide();
    detailsLayout->addWidget(m_correspondenceEvidence);

    auto *correspondenceActions = new QHBoxLayout;
    m_confirmCorrespondenceButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Confirm Mapping"), details);
    m_confirmCorrespondenceButton->setObjectName(QStringLiteral("commentsOverviewConfirmCorrespondence"));
    m_flagCorrespondenceButton = new QPushButton(QIcon::fromTheme(QStringLiteral("flag-red")), i18n("Flag Incorrect"), details);
    m_flagCorrespondenceButton->setObjectName(QStringLiteral("commentsOverviewFlagCorrespondence"));
    m_recomputeCorrespondenceButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Recompute"), details);
    m_recomputeCorrespondenceButton->setObjectName(QStringLiteral("commentsOverviewRecomputeCorrespondence"));
    correspondenceActions->addWidget(m_confirmCorrespondenceButton);
    correspondenceActions->addWidget(m_flagCorrespondenceButton);
    correspondenceActions->addWidget(m_recomputeCorrespondenceButton);
    correspondenceActions->addStretch(1);
    detailsLayout->addLayout(correspondenceActions);
    m_confirmCorrespondenceButton->hide();
    m_flagCorrespondenceButton->hide();
    m_recomputeCorrespondenceButton->hide();

    auto *metadataEditorLayout = new QFormLayout;
    m_metadataEditorLayout = metadataEditorLayout;
    m_noteDateFields = new QWidget(details);
    auto *noteDateLayout = new QFormLayout(m_noteDateFields);
    noteDateLayout->setContentsMargins(0, 0, 0, 0);
    // A date field with a calendar button; empty means "not set".
    const auto dateField = [this, noteDateLayout](const QString &label, const QString &objectName, const QString &placeholder) {
        auto *row = new QWidget(m_noteDateFields);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(row);
        edit->setObjectName(objectName);
        edit->setPlaceholderText(placeholder);
        edit->setClearButtonEnabled(true);
        auto *pick = new QToolButton(row);
        pick->setIcon(QIcon::fromTheme(QStringLiteral("view-calendar")));
        pick->setText(QStringLiteral("…"));
        pick->setToolTip(i18n("Choose a date"));
        pick->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(pick);
        auto *calendar = new QCalendarWidget(menu);
        auto *action = new QWidgetAction(menu);
        action->setDefaultWidget(calendar);
        menu->addAction(action);
        pick->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, calendar, [edit, calendar] {
            const QDate current = QDate::fromString(edit->text().trimmed(), Qt::ISODate);
            calendar->setSelectedDate(current.isValid() ? current : QDate::currentDate());
        });
        connect(calendar, &QCalendarWidget::clicked, edit, [edit, menu](QDate date) {
            edit->setText(date.toString(Qt::ISODate));
            menu->close();
        });
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(pick);
        noteDateLayout->addRow(label, row);
        connect(edit, &QLineEdit::textChanged, this, [this] {
            if (!m_changingSelection) updateEditorDirtyState();
        });
        return edit;
    };
    m_noteDueDate = dateField(i18n("Due date:"), QStringLiteral("commentsOverviewNoteDueDate"), i18n("YYYY-MM-DD (optional)"));
    m_noteVisibleFrom = dateField(i18n("Hide until:"), QStringLiteral("commentsOverviewNoteVisibleFrom"),
                                  i18n("YYYY-MM-DD — hidden from To-dos until then"));
    metadataEditorLayout->addRow(m_noteDateFields);
    m_noteDateFields->hide();

    m_locationEnabled = new QCheckBox(i18n("Attach location"), details);
    m_locationEnabled->setObjectName(QStringLiteral("commentsOverviewLocationEnabled"));
    metadataEditorLayout->addRow(QString(), m_locationEnabled);
    m_locationLabel = new QLineEdit(details);
    m_locationLabel->setObjectName(QStringLiteral("commentsOverviewLocationLabel"));
    m_locationLabel->setPlaceholderText(i18n("Place name"));
    metadataEditorLayout->addRow(i18n("Place:"), m_locationLabel);
    auto *coordinates = new QWidget(details);
    auto *coordinatesLayout = new QHBoxLayout(coordinates);
    coordinatesLayout->setContentsMargins(0, 0, 0, 0);
    m_locationLatitude = new QDoubleSpinBox(coordinates);
    m_locationLatitude->setObjectName(QStringLiteral("commentsOverviewLocationLatitude"));
    m_locationLatitude->setRange(-90.0, 90.0);
    m_locationLatitude->setDecimals(6);
    m_locationLatitude->setSingleStep(0.001);
    m_locationLatitude->setPrefix(i18n("Lat "));
    m_locationLongitude = new QDoubleSpinBox(coordinates);
    m_locationLongitude->setObjectName(QStringLiteral("commentsOverviewLocationLongitude"));
    m_locationLongitude->setRange(-180.0, 180.0);
    m_locationLongitude->setDecimals(6);
    m_locationLongitude->setSingleStep(0.001);
    m_locationLongitude->setPrefix(i18n("Lon "));
    coordinatesLayout->addWidget(m_locationLatitude);
    coordinatesLayout->addWidget(m_locationLongitude);
    metadataEditorLayout->addRow(i18n("Coordinates:"), coordinates);
    m_locationCoordinates = coordinates;
    auto *assistantActions = new QWidget(details);
    m_assistantActions = assistantActions;
    auto *assistantActionsLayout = new QHBoxLayout(assistantActions);
    assistantActionsLayout->setContentsMargins(0, 0, 0, 0);
    m_openCodexButton = new QPushButton(QIcon::fromTheme(QStringLiteral("utilities-terminal")), i18n("Open Codex"), assistantActions);
    m_openCodexButton->setObjectName(QStringLiteral("commentsOverviewOpenCodex"));
    m_openCodexButton->setAutoDefault(false);
    m_chatGptUrlEditor = new QLineEdit(assistantActions);
    m_chatGptUrlEditor->setObjectName(QStringLiteral("commentsOverviewChatGptUrl"));
    m_chatGptUrlEditor->setPlaceholderText(i18n("ChatGPT link"));
    m_chatGptUrlEditor->setClearButtonEnabled(true);
    m_openChatGptButton = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18n("Open ChatGPT"), assistantActions);
    m_openChatGptButton->setObjectName(QStringLiteral("commentsOverviewOpenChatGpt"));
    m_openChatGptButton->setAutoDefault(false);
    assistantActionsLayout->addWidget(m_openCodexButton);
    assistantActionsLayout->addWidget(m_chatGptUrlEditor, 1);
    assistantActionsLayout->addWidget(m_openChatGptButton);
    metadataEditorLayout->addRow(QString(), assistantActions);
    m_openSourceButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open Source in VS Code"), details);
    m_openSourceButton->setObjectName(QStringLiteral("commentsOverviewOpenSource"));
    m_openSourceButton->setAutoDefault(false);
    metadataEditorLayout->addRow(QString(), m_openSourceButton);
    detailsLayout->addLayout(metadataEditorLayout);

    m_commentEditor = new QPlainTextEdit(details);
    m_commentEditor->setObjectName(QStringLiteral("commentsOverviewEditor"));
    m_commentEditor->setPlaceholderText(i18n("Select a comment to edit its text."));
    detailsLayout->addWidget(m_commentEditor, 1);
    m_noteAttachments = new QWidget(details);
    m_noteAttachments->setObjectName(QStringLiteral("commentsOverviewNoteAttachments"));
    m_noteAttachments->setLayout(new QVBoxLayout);
    m_noteAttachments->hide();
    detailsLayout->addWidget(m_noteAttachments);

    auto *commentActions = new QHBoxLayout;
    m_undoButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Undo"), details);
    m_undoButton->setObjectName(QStringLiteral("commentsOverviewUndo"));
    m_undoButton->setAutoDefault(false);
    m_redoButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-redo")), i18n("Redo"), details);
    m_redoButton->setObjectName(QStringLiteral("commentsOverviewRedo"));
    m_redoButton->setAutoDefault(false);
    commentActions->addWidget(m_undoButton);
    commentActions->addWidget(m_redoButton);
    commentActions->addStretch(1);
    m_deleteCommentButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete"), details);
    m_deleteCommentButton->setObjectName(QStringLiteral("commentsOverviewDelete"));
    m_deleteCommentButton->setAutoDefault(false);
    m_resolveFeedbackButton = new QPushButton(i18n("Resolve"), details);
    m_resolveFeedbackButton->setObjectName(QStringLiteral("commentsOverviewResolve"));
    m_resolveFeedbackButton->setAutoDefault(false);
    m_resolveFeedbackButton->hide();
    connect(m_resolveFeedbackButton, &QPushButton::clicked, this, &CommentsOverview::resolveSelectedFeedback);
    m_acceptProposalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Accept Proposal"), details);
    m_acceptProposalButton->setObjectName(QStringLiteral("commentsOverviewAcceptProposal"));
    m_acceptProposalButton->setAutoDefault(false);
    m_disregardProposalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18n("Reject Proposal"), details);
    m_disregardProposalButton->setObjectName(QStringLiteral("commentsOverviewDisregardProposal"));
    m_disregardProposalButton->setAutoDefault(false);
    m_saveCommentButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), i18n("Save Comment"), details);
    m_saveCommentButton->setObjectName(QStringLiteral("commentsOverviewSave"));
    m_saveCommentButton->setAutoDefault(false);
    m_saveCommentButton->hide();
    m_replyCommentButton = new QPushButton(QIcon::fromTheme(QStringLiteral("mail-reply-sender")), i18n("Reply…"), details);
    m_replyCommentButton->setObjectName(QStringLiteral("commentsOverviewReply"));
    m_replyCommentButton->setAutoDefault(false);
    m_restoreAnchorButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore Anchor"), details);
    m_restoreAnchorButton->setObjectName(QStringLiteral("commentsOverviewRestoreAnchor"));
    m_restoreAnchorButton->setAutoDefault(false);
    m_restoreAnchorButton->setToolTip(i18n("Re-insert the comment's marker into its source at the last known line."));
    m_restoreAnchorButton->hide();
    m_removeMissingMarkerButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Remove Marker"), details);
    m_removeMissingMarkerButton->setObjectName(QStringLiteral("commentsOverviewRemoveMissingMarker"));
    m_removeMissingMarkerButton->setAutoDefault(false);
    m_removeMissingMarkerButton->setToolTip(i18n("Take the marker out of its source; highlighted words stay."));
    m_removeMissingMarkerButton->hide();
    m_restoreMissingCommentButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore Comment"), details);
    m_restoreMissingCommentButton->setObjectName(QStringLiteral("commentsOverviewRestoreMissingComment"));
    m_restoreMissingCommentButton->setAutoDefault(false);
    m_restoreMissingCommentButton->setToolTip(i18n("Bring the comment back from the marker's text, or create it when it was never known."));
    m_restoreMissingCommentButton->hide();
    m_restoreProposalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Restore Proposal"), details);
    m_restoreProposalButton->setObjectName(QStringLiteral("commentsOverviewRestoreProposal"));
    m_restoreProposalButton->setAutoDefault(false);
    m_restoreProposalButton->setToolTip(i18n("Put the proposal back into its source where its original or proposed text is, so it can be accepted or rejected again."));
    m_restoreProposalButton->hide();
    m_copyProposalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Proposal Text"), details);
    m_copyProposalButton->setObjectName(QStringLiteral("commentsOverviewCopyProposal"));
    m_copyProposalButton->setAutoDefault(false);
    m_copyProposalButton->setToolTip(i18n("Copy the proposal's explanation, original and proposed text."));
    m_copyProposalButton->hide();
    m_dismissProposalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")), i18n("Dismiss"), details);
    m_dismissProposalButton->setObjectName(QStringLiteral("commentsOverviewDismissProposal"));
    m_dismissProposalButton->setAutoDefault(false);
    m_dismissProposalButton->setToolTip(i18n("Forget this proposal on every device without changing any source."));
    m_dismissProposalButton->hide();
    commentActions->addWidget(m_replyCommentButton);
    commentActions->addWidget(m_restoreAnchorButton);
    commentActions->addWidget(m_restoreMissingCommentButton);
    commentActions->addWidget(m_removeMissingMarkerButton);
    commentActions->addWidget(m_copyProposalButton);
    commentActions->addWidget(m_dismissProposalButton);
    commentActions->addWidget(m_restoreProposalButton);
    commentActions->addWidget(m_resolveFeedbackButton);
    commentActions->addWidget(m_deleteCommentButton);
    commentActions->addWidget(m_disregardProposalButton);
    commentActions->addWidget(m_acceptProposalButton);
    commentActions->addWidget(m_saveCommentButton);
    detailsLayout->addLayout(commentActions);

    m_contentSplitter->addWidget(details);
    // A collapsed details panel is just lost space: the splitter would hide it
    // whenever it is offered less than the width its rows need.
    m_contentSplitter->setCollapsible(m_contentSplitter->indexOf(details), false);
    m_contentSplitter->setStretchFactor(0, 3);
    m_contentSplitter->setStretchFactor(1, 2);

    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(750);
    // One Library tree rebuild per burst of background changes; see
    // scheduleLibraryTreeUpdate().
    m_libraryTreeTimer = new QTimer(this);
    m_libraryTreeTimer->setSingleShot(true);
    m_libraryTreeTimer->setInterval(100);
    connect(m_libraryTreeTimer, &QTimer::timeout, this, &CommentsOverview::flushLibraryTreeUpdate);
    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setSingleShot(true);
    m_autoSaveTimer->setInterval(300);
    m_persistenceRetryTimer = new QTimer(this);
    m_persistenceRetryTimer->setSingleShot(true);
    m_persistenceRetryTimer->setInterval(1000);
    // Catalog checks follow the shared change stream instead of a timer; the
    // short delay folds a burst of writes into one check.
    m_catalogPollTimer = new QTimer(this);
    m_catalogPollTimer->setSingleShot(true);
    m_catalogPollTimer->setInterval(150);
    m_persistencePool = new QThreadPool(this);
    m_persistencePool->setMaxThreadCount(1);
    m_catalogPool = new QThreadPool(this);
    m_catalogPool->setMaxThreadCount(2);
    m_librarySearchPool = new QThreadPool(this);
    m_librarySearchPool->setMaxThreadCount(1);
    m_librarySearchTimer = new QTimer(this);
    m_librarySearchTimer->setSingleShot(true);
    m_librarySearchTimer->setInterval(1500);
    connect(m_librarySearchTimer, &QTimer::timeout, this, &CommentsOverview::startLibrarySearchUpdate);
    m_librarySourceSearchTimer = new QTimer(this);
    m_librarySourceSearchTimer->setSingleShot(true);
    m_librarySourceSearchTimer->setInterval(1500);
    connect(m_librarySourceSearchTimer, &QTimer::timeout, this, &CommentsOverview::startLibrarySourceSearchUpdate);
    connect(this, &CommentsOverview::documentsChanged, this, [this] {
        m_librarySearchDocumentsKnown = true;
        scheduleLibrarySearchUpdate();
        // Projects and their folders may have changed.
        m_librarySourceSearchDirty = true;
        scheduleLibrarySourceSearchUpdate();
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        // Catalog workers use Qt parsing facilities. Let them finish before
        // QCoreApplication begins tearing down process-wide Qt state.
        m_shuttingDown = true;
        if (m_catalogPool) {
            m_catalogPool->waitForDone();
        }
        if (m_librarySearchPool) {
            m_librarySearchPool->waitForDone();
        }
    });

    for (BrowserState *browser : {&m_commentsBrowser, &m_notesBrowser}) {
        connect(browser->search, &QLineEdit::textChanged, this, [this, browser] { applyBrowserFilters(*browser); });
        connect(browser->typeFilter, &QComboBox::currentIndexChanged, this, [this, browser] {
            applyBrowserFilters(*browser);
            auto *filter = browser->typeFilter;
            if (filter->currentData().toString() != QLatin1String("issues") && filter->findData(QStringLiteral("issues")) >= 0) {
                QTimer::singleShot(0, filter, [filter] {
                    const int issues = filter->findData(QStringLiteral("issues"));
                    if (issues >= 0 && issues != filter->currentIndex()) filter->removeItem(issues);
                });
            }
        });
        connect(browser->documentList, &QTreeWidget::currentItemChanged, this, [this, browser] { applyBrowserFilters(*browser); });
        connect(browser->documentList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) { activateDocument(item); });
        connect(browser->documentList, &QWidget::customContextMenuRequested, this, &CommentsOverview::showDocumentsContextMenu);
        connect(browser->table, &QTreeView::pressed, this, [this](const QModelIndex &index) { traceInteraction(QStringLiteral("comments.pressed"), index); });
        connect(browser->table, &QTreeView::clicked, this, [this](const QModelIndex &index) { traceInteraction(QStringLiteral("comments.clicked"), index); });
        connect(browser->table, &QTreeView::doubleClicked, this, [this, browser](const QModelIndex &index) {
            if (browser->table == m_table) { activateComment(index); }
        });
        connect(browser->table, &QWidget::customContextMenuRequested, this, &CommentsOverview::showCommentsContextMenu);
        connect(browser->table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this, browser](const QModelIndex &current, const QModelIndex &previous) {
            if (browser->table == m_table) { selectedCommentChanged(current, previous); }
        });
        connect(browser->table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this, browser] {
            if (browser->table == m_table) { updateSelectionUi(); }
        });
        // Filtering changes a view, never the Projects library or its data.
        for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
            connect(browser->proxy, signal, this, [this] { updateCommentSummary(); });
        }
        connect(browser->proxy, &QAbstractItemModel::modelReset, this, &CommentsOverview::updateCommentSummary);
    }
    connect(m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &first, const QModelIndex &last) {
        if (m_updatingCommentModel) { return; }
        // Local edits bypass catalogue reconciliation; invalidate their cached
        // snapshot so a later authoritative update can also revert those edits.
        for (int row = first.row(); row <= last.row(); ++row) {
            const auto index = m_model->index(row, CommentColumn, first.parent());
            m_renderedComments.remove(index.data(PdfPathRole).toString() + QChar(0x1f) + index.data(AnnotationIdRole).toString());
        }
    });
    connect(m_reviewCardsButton, &QPushButton::clicked, this, &CommentsOverview::reviewOverviewRequested);
    connect(m_agentNotesButton, &QPushButton::clicked, this, &CommentsOverview::agentNotesRequested);
    connect(m_syncButton, &QPushButton::clicked, this, &CommentsOverview::syncRequested);
    // As in the reader; a disabled button ignores it, so a running Sync is not started again.
    auto *syncShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_S), this);
    syncShortcut->setObjectName(QStringLiteral("commentsOverviewSyncShortcut"));
    syncShortcut->setContext(Qt::WindowShortcut);
    connect(syncShortcut, &QShortcut::activated, m_syncButton, &QPushButton::click);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { traceInteraction(QStringLiteral("overview.tab_changed")); });
    connect(m_confirmCorrespondenceButton, &QPushButton::clicked, this, &CommentsOverview::confirmCurrentCorrespondence);
    connect(m_flagCorrespondenceButton, &QPushButton::clicked, this, &CommentsOverview::flagCurrentCorrespondence);
    connect(m_recomputeCorrespondenceButton, &QPushButton::clicked, this, &CommentsOverview::recomputeCurrentCorrespondence);
    connect(m_commentEditor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_changingSelection) {
            updateEditorDirtyState();
        }
    });
    connect(m_locationEnabled, &QCheckBox::toggled, this, [this](bool enabled) {
        m_locationLabel->setEnabled(enabled && !m_operationPending);
        m_locationLatitude->setEnabled(enabled && !m_operationPending);
        m_locationLongitude->setEnabled(enabled && !m_operationPending);
        if (!m_changingSelection) {
            m_locationTouched = true;
            updateEditorDirtyState();
        }
    });
    const auto locationChanged = [this] {
        if (!m_changingSelection) {
            m_locationTouched = true;
            updateEditorDirtyState();
        }
    };
    connect(m_locationLabel, &QLineEdit::textChanged, this, locationChanged);
    connect(m_locationLatitude, &QDoubleSpinBox::valueChanged, this, locationChanged);
    connect(m_locationLongitude, &QDoubleSpinBox::valueChanged, this, locationChanged);
    connect(m_saveCommentButton, &QPushButton::clicked, this, &CommentsOverview::saveSelectedComment);
    connect(m_addNoteButton, &QPushButton::clicked, this, &CommentsOverview::createStandaloneNote);
    connect(m_replyCommentButton, &QPushButton::clicked, this, &CommentsOverview::replyToSelectedComment);
    connect(m_deleteCommentButton, &QPushButton::clicked, this, &CommentsOverview::deleteSelectedComment);
    connect(m_restoreAnchorButton, &QPushButton::clicked, this, &CommentsOverview::restoreSelectedAnchors);
    connect(m_removeMissingMarkerButton, &QPushButton::clicked, this, &CommentsOverview::removeSelectedMissingMarkers);
    connect(m_restoreMissingCommentButton, &QPushButton::clicked, this, &CommentsOverview::restoreSelectedMissingComments);
    connect(m_restoreProposalButton, &QPushButton::clicked, this, &CommentsOverview::restoreSelectedRemovedProposals);
    connect(m_dismissProposalButton, &QPushButton::clicked, this, &CommentsOverview::dismissSelectedRemovedProposals);
    connect(m_copyProposalButton, &QPushButton::clicked, this, &CommentsOverview::copySelectedRemovedProposals);
    connect(m_acceptProposalButton, &QPushButton::clicked, this, &CommentsOverview::acceptSelectedAgentProposals);
    connect(m_disregardProposalButton, &QPushButton::clicked, this, &CommentsOverview::disregardSelectedAgentProposals);
    connect(m_openCodexButton, &QPushButton::clicked, this, [this] {
        if (!m_currentPdfPath.isEmpty() && !m_currentAnnotationId.isEmpty()) {
            Q_EMIT openCodexRequested(m_currentPdfPath, m_currentAnnotationId);
        }
    });
    connect(m_chatGptUrlEditor, &QLineEdit::textChanged, this, [this] {
        if (!m_changingSelection) {
            m_chatGptUrlDirty = m_chatGptUrlEditor->text().trimmed() != m_originalChatGptUrl;
            updateEditorActions();
        }
    });
    connect(m_chatGptUrlEditor, &QLineEdit::returnPressed, this, [this] { saveChatGptUrl(false); });
    connect(m_openChatGptButton, &QPushButton::clicked, this, &CommentsOverview::openChatGptUrl);
    connect(m_openSourceButton, &QPushButton::clicked, this, [this] {
        QString sourcePath;
        QString unavailableReason;
        int line = 0;
        if (!currentSourceTarget(&sourcePath, &line, &unavailableReason)) {
            m_summary->setText(unavailableReason);
            return;
        }
        Q_EMIT openSourceRequested(sourcePath, line);
    });
    connect(m_undoButton, &QPushButton::clicked, this, &CommentsOverview::undoOverviewChange);
    connect(m_redoButton, &QPushButton::clicked, this, &CommentsOverview::redoOverviewChange);
    connect(m_trackPdfButton, &QPushButton::clicked, this, &CommentsOverview::createLibraryProject);
    Thoughts::DocsChangeMonitor *docsMonitor = Thoughts::DocsChangeMonitor::forRoot(m_docsRoot);
    connect(docsMonitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, &CommentsOverview::handleDocsPathsChanged);
    connect(docsMonitor, &Thoughts::DocsChangeMonitor::rescanRequired, this, [this] {
        // Dropped events may have hidden source edits, which only a full
        // reload re-reads.
        requestCatalogCheck(true);
        requestReload(QStringLiteral("change_events_dropped"));
        m_librarySourceSearchDirty = true;
        scheduleLibrarySourceSearchUpdate();
    });
    // Fedora sync writes the source bundle below the application data folder.
    Thoughts::DocsChangeMonitor *dataMonitor = Thoughts::DocsChangeMonitor::forRoot(
        Thoughts::Platform::thoughtsDataRoot());
    connect(dataMonitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, [this, dataMonitor](const QStringList &paths) {
        const QString bundle = dataMonitor->rootPath() + QStringLiteral("/thoughts-server/source-bundle");
        if (std::any_of(paths.cbegin(), paths.cend(), [&bundle](const QString &path) { return Thoughts::DocsChangeMonitor::isWithin(path, bundle); })) {
            requestCatalogCheck(false);
        }
    });
    connect(m_reloadTimer, &QTimer::timeout, this, &CommentsOverview::processScheduledReloads);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &CommentsOverview::saveSelectedComment);
    connect(m_persistenceRetryTimer, &QTimer::timeout, this, &CommentsOverview::startNextMutation);
    connect(m_catalogPollTimer, &QTimer::timeout, this, [this] {
        if (!isVisible()) {
            return; // showEvent() checks again
        }
        if (m_catalogSignature.isEmpty() || m_editorDirty || m_chatGptUrlDirty || m_operationPending || m_catalogSignatureCheckRunning) {
            // Keep the request until the overview can compare safely.
            QTimer::singleShot(1000, this, [this] {
                if (m_catalogCheckRequested && !m_catalogPollTimer->isActive()) m_catalogPollTimer->start();
            });
            return;
        }
        m_catalogCheckRequested = false;
        m_catalogSignatureCheckRunning = true;
        const QString docsRoot = m_docsRoot;
        QPointer<CommentsOverview> guard(this);
        const bool refreshInventory = std::exchange(m_libraryInventoryDirty, false);
        m_catalogPool->start([guard, docsRoot, refreshInventory] {
            const DeveloperActivity activity(QStringLiteral("library.catalog_poll"), {{QStringLiteral("inventory"), refreshInventory}});
            QHash<QString, QJsonObject> sourceInventory;
            bool inventoryValid = false;
            LibraryProjectStore store(docsRoot);
            if (refreshInventory) {
                const DeveloperActivity inventoryActivity(QStringLiteral("library.source_sizes"));
                LibrarySourceInventory inventory(docsRoot);
                const QHash<QString, QJsonObject> empty;
                QSet<QString> owners;
                for (const auto &project : store.projects(true, &empty))
                    for (const auto &source : project.toObject().value(QStringLiteral("sources")).toArray()) {
                        const auto owner = source.toObject().value(QStringLiteral("id")).toString();
                        if (!owners.contains(owner)) sourceInventory.insert(owner, inventory.refresh(owner));
                        owners.insert(owner);
                    }
                inventoryValid = store.error().isEmpty();
                if (inventoryValid) inventory.save(owners);
                DeveloperLog::record(QStringLiteral("library.source_sizes.checked"),
                    {{QStringLiteral("folders"), owners.size()}, {QStringLiteral("directories_enumerated"), inventory.directoriesEnumerated},
                     {QStringLiteral("files_checked"), inventory.filesChecked}});
            }
            QHash<QString, QStringList> documentsBySource;
            const QByteArray signature = commentCatalogSignature(docsRoot, {}, &documentsBySource);
            const QString projectStoreIdentity = Thoughts::artifactFileIdentity(store.path());
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, signature, documentsBySource, sourceInventory, inventoryValid, projectStoreIdentity] {
                    if (!guard) {
                        return;
                    }
                    guard->m_catalogSignatureCheckRunning = false;
                    // Change events are reported below the configured root.
                    const QString root = QDir::cleanPath(guard->m_docsRoot);
                    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
                    guard->m_documentsBySource.clear();
                    for (auto it = documentsBySource.cbegin(); it != documentsBySource.cend(); ++it) {
                        const QString path = !canonicalRoot.isEmpty() && canonicalRoot != root && Thoughts::DocsChangeMonitor::isWithin(it.key(), canonicalRoot)
                            ? root + it.key().mid(canonicalRoot.size()) : it.key();
                        guard->m_documentsBySource.insert(path, it.value());
                    }
                    guard->m_documentsBySourceKnown = true;
                    if (guard->m_catalogCheckRequested) guard->m_catalogPollTimer->start();
                    if (inventoryValid && sourceInventory != guard->m_librarySourceInventory) {
                        const bool sizesOnly = sameLibrarySourceFiles(guard->m_librarySourceInventory, sourceInventory);
                        guard->m_librarySourceInventory = sourceInventory;
                        // Sizes alone update the rows on screen in place; anything else,
                        // or a tree that is off screen or about to be rebuilt, waits.
                        if (sizesOnly && !guard->m_libraryTreeDirty && guard->libraryTreeOnScreen()) guard->updateLibrarySizes();
                        else guard->scheduleLibraryTreeUpdate(sizesOnly ? QStringLiteral("source_sizes") : QStringLiteral("source_inventory"));
                    }
                    const bool projectChanged = projectStoreIdentity != guard->m_projectStoreIdentity;
                    guard->m_projectStoreIdentity = projectStoreIdentity;
                    if (signature != guard->m_catalogSignature || projectChanged) {
                        DeveloperLog::record(QStringLiteral("library.catalog_changed"),
                            {{QStringLiteral("signature_changed"), signature != guard->m_catalogSignature},
                             {QStringLiteral("project_changed"), projectChanged}});
                        guard->m_catalogSignature = signature;
                        guard->startFullReload(projectChanged, QStringLiteral("catalog_changed"));
                    }
                },
                Qt::QueuedConnection);
        });
    });

    for (BrowserState *browser : {&m_commentsBrowser, &m_notesBrowser}) {
        auto *deleteShortcut = new QShortcut(browser->table);
        QList<QKeySequence> deleteKeys;
        const auto appendDeleteKey = [&deleteKeys](const QKeySequence &key) {
            if (!deleteKeys.contains(key)) {
                deleteKeys.append(key);
            }
        };
        for (const QKeySequence &key : QKeySequence::keyBindings(QKeySequence::Delete)) {
            appendDeleteKey(key);
        }
        appendDeleteKey(QKeySequence(Qt::Key_Delete));
        appendDeleteKey(QKeySequence(Qt::Key_Backspace));
        deleteShortcut->setKeys(deleteKeys);
        deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(deleteShortcut, &QShortcut::activated, this, &CommentsOverview::deleteSelectedComment);
        auto *undoShortcut = new QShortcut(QKeySequence::Undo, browser->table);
        undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(undoShortcut, &QShortcut::activated, this, &CommentsOverview::undoOverviewChange);
        auto *redoShortcut = new QShortcut(QKeySequence::Redo, browser->table);
        redoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(redoShortcut, &QShortcut::activated, this, &CommentsOverview::redoOverviewChange);
        auto *returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), browser->table);
        returnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(returnShortcut, &QShortcut::activated, this, [this] { activateComment(m_table->currentIndex()); });
        auto *enterShortcut = new QShortcut(QKeySequence(Qt::Key_Enter), browser->table);
        enterShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(enterShortcut, &QShortcut::activated, this, [this] { activateComment(m_table->currentIndex()); });

    }

    connect(m_libraryTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item) {
            return;
        }
        if (const QVariant conversation = item->data(0, LibraryConversationRole); conversation.isValid()) {
            openAgentConversation(conversation.value<QJsonObject>());
            return;
        }
        const QString sourcePath = item->data(0, Qt::UserRole + 1).toString();
        if (!sourcePath.isEmpty()) {
            Q_EMIT openSourceRequested(QDir(m_docsRoot).filePath(sourcePath), 1);
            return;
        }
        const QString pdfPath = item->data(0, Qt::UserRole).toString();
        const auto document = item->data(0, Qt::UserRole + 141).value<QJsonObject>();
        // A checked document without a PDF yet opens its source instead of
        // doing nothing.
        if (pdfPath.isEmpty() || (!document.isEmpty() && !QFileInfo::exists(pdfPath))) {
            const QString documentSource = document.value(QStringLiteral("source_file")).toString();
            if (!documentSource.isEmpty() && QFileInfo::exists(QDir(m_docsRoot).filePath(documentSource))) {
                Q_EMIT openSourceRequested(QDir(m_docsRoot).filePath(documentSource), 1);
            }
            return;
        }
        {
            const int page = item->data(0, LibraryPageRole).toInt();
            auto *root = item;
            while (root->parent()) root = root->parent();
            const auto project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
            if (!document.isEmpty()) {
                // Without a known page (not yet in the Project reading), open the PDF itself.
                if (page > 0) Q_EMIT documentPageActivated(pdfPath, page, document.value(QStringLiteral("document_id")).toString(), document.value(QStringLiteral("name")).toString());
                else Q_EMIT documentActivated(pdfPath);
            } else if (!project.isEmpty()) {
                Q_EMIT documentPageActivated(pdfPath, page, {}, project.value(QStringLiteral("name")).toString(), project.value(QStringLiteral("id")).toString());
            } else if (page > 0) {
                Q_EMIT documentPageActivated(pdfPath, page);
            } else {
                Q_EMIT documentActivated(pdfPath);
            }
        }
    });
    connect(m_libraryTree, &QWidget::customContextMenuRequested, this, &CommentsOverview::showLibraryContextMenu);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (m_tabs->widget(index) == m_reviewPage) {
            if (m_reviewPage->currentWidget() == m_reviewCardsWidget) m_reviewCardsWidget->ensureLoaded();
            else m_mentalModelView->reload();
        } else if (m_tabs->widget(index) == m_commentsPage || m_tabs->widget(index) == m_notesPage) {
            const bool notes = m_tabs->widget(index) == m_notesPage;
            if (notes != m_notesActive) {
                if (m_operationPending || !resolveDirtyEditor()) {
                    const QSignalBlocker blocker(m_tabs);
                    m_tabs->setCurrentWidget(m_notesActive ? m_notesPage : m_commentsPage);
                    return;
                }
                activateBrowser(notes ? m_notesBrowser : m_commentsBrowser);
            }
            m_search->setFocus(Qt::OtherFocusReason);
        } else if (m_tabs->widget(index) == m_diaryPage) {
            m_diaryInput->setFocus(Qt::OtherFocusReason);
        } else if (m_tabs->widget(index) == m_summaryPage) {
            requestLibraryMonitoringInformation(true);
            if (m_libraryTreeDirty) {
                // Rebuild what changed while another tab was in front.
                m_libraryTreeDirtySince.start();
                m_libraryTreeTimer->start();
            }
        }
    });

    clearComment();
    updateDashboardSummary();
    // The first rows are ready before the window is shown.
    updateLibraryTree();
}

CommentsOverview::~CommentsOverview()
{
    m_shuttingDown = true;
    if (m_persistencePool) {
        m_persistencePool->waitForDone();
    }
    flushQueuedMutations();
    if (m_catalogPool && !m_catalogPool->waitForDone(100)) {
        // A coordinated/cloud filesystem open may remain in the kernel after
        // the window has gone away. The worker only holds a QPointer back to
        // this dialog, so detach the pool instead of freezing application exit.
        m_catalogPool->setParent(nullptr);
        m_catalogPool = nullptr;
    }
    saveWindowState();
}

int CommentsOverview::documentCount() const
{
    return m_documents.size();
}

int CommentsOverview::commentCount() const
{
    int count = 0;
    for (const TrackedCommentDocument &document : m_documents) {
        count += std::count_if(document.comments.cbegin(), document.comments.cend(), [this](const TrackedComment &comment) { return visibleThread(comment, m_queuedAgentProposalIds); });
    }
    return count;
}

QString CommentsOverview::selectedDocument() const
{
    const QTreeWidgetItem *item = m_documentList->currentItem();
    const int kind = item ? item->data(0, DocumentItemKindRole).toInt() : AllCommentsItem;
    return item && (kind == DocumentListEntry || kind == ComponentListEntry) ? item->data(0, Qt::UserRole).toString() : QString();
}

void CommentsOverview::setFedoraProjections(const QJsonObject &projections)
{
    const QJsonObject sourceLocations = Thoughts::sourceLocationIndex(m_docsRoot);
    const bool sourceLocationsChanged = sourceLocations != m_reviewSourceLocationIndex;
    m_reviewSourceLocationIndex = sourceLocations;
    if (m_fedoraProjections == projections) {
        if (sourceLocationsChanged) reloadReviewCards();
        return;
    }
    // Local autosaves already updated their rows. A durable-queue echo must not
    // reload the entire library or replace the active editor document.
    QJsonObject previous = m_fedoraProjections;
    QJsonObject incoming = projections;
    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        auto before = previous.value(it.key()).toObject();
        auto after = it.value().toObject();
        before.remove(QStringLiteral("content_updated_at"));
        after.remove(QStringLiteral("content_updated_at"));
        if (before == after) previous.insert(it.key(), it.value());
    }
    const bool alreadyDisplayed = previous == incoming;
    // Agents touch their conversation links while they work; a full reload takes seconds.
    const bool onlyConversations = !alreadyDisplayed && Thoughts::AgentConversations::onlyConversationChanges(previous, incoming);
    m_fedoraProjections = projections;
    ++m_commentGeneration;
    if (alreadyDisplayed) {
        if (sourceLocationsChanged) reloadReviewCards();
        return;
    }
    if (onlyConversations) {
        if (sourceLocationsChanged) reloadReviewCards();
        scheduleLibraryTreeUpdate(QStringLiteral("agent_conversations"));
        return;
    }
    reloadReviewCards();
    if (!m_initialProposalDiscoveryDone) return;
    requestReload(QStringLiteral("projection_changed"));
}

void CommentsOverview::showSummary()
{
    if (m_tabs && m_summaryPage) {
        if (m_tabs->currentWidget() == m_summaryPage) {
            requestLibraryMonitoringInformation(true);
        } else {
            m_tabs->setCurrentWidget(m_summaryPage);
        }
    }
}

void CommentsOverview::reloadReviewCards()
{
    if (m_reviewCardsWidget) {
        m_reviewCardsWidget->reload();
    }
}

int CommentsOverview::commentIssueCount() const
{
    QSet<QString> ids;
    for (const auto &index : modelCommentIndexes(m_model)) {
        if ((index.data(ItemTypeRole).toString() == QLatin1String("comment")
             && commentIssueStatuses().contains(index.data(AnchorStatusRole).toString()))
            || index.data(AnchorStatusRole).toString() == QLatin1String("proposal_removed")) {
            ids.insert(index.data(AnnotationIdRole).toString());
        }
    }
    return ids.size();
}

int CommentsOverview::reviewCardIssueCount() const
{
    if (m_reviewCardsWidget) m_reviewCardsWidget->ensureLoaded();
    return m_reviewCardsWidget ? m_reviewCardsWidget->issueCount() : 0;
}

void CommentsOverview::showCommentIssues()
{
    if (!resolveDirtyEditor()) return;
    m_tabs->setCurrentWidget(m_commentsPage);
    resetBrowserFilters();
    // Not one of the types: it is here while the issues are shown, and goes with the next choice.
    if (m_typeFilter->findData(QStringLiteral("issues")) < 0) m_typeFilter->addItem(i18n("Comments with issues"), QStringLiteral("issues"));
    m_typeFilter->setCurrentIndex(m_typeFilter->findData(QStringLiteral("issues")));
}

void CommentsOverview::showReviewCardsState(const QString &stateFilter)
{
    showReviewCards();
    if (m_reviewCardsWidget && !stateFilter.isEmpty()) {
        m_reviewCardsWidget->setStateFilter(stateFilter);
    }
}

void CommentsOverview::showReviewCardInfo(const QString &cardId)
{
    showReviewCards();
    if (m_reviewCardsWidget) {
        m_reviewCardsWidget->selectCard(cardId);
    }
}

void CommentsOverview::showReviewCards()
{
    if (!m_tabs || !m_reviewCardsWidget) {
        return;
    }
    if (m_tabs->currentWidget() == m_reviewPage && m_reviewPage->currentWidget() == m_reviewCardsWidget) {
        m_reviewCardsWidget->ensureLoaded();
    } else {
        // Each switch loads the cards (currentChanged) when it lands on them.
        m_tabs->setCurrentWidget(m_reviewPage);
        m_reviewPage->setCurrentWidget(m_reviewCardsWidget);
        m_reviewCardsWidget->ensureLoaded();
    }
    m_reviewCardsWidget->setFocus(Qt::OtherFocusReason);
}

void CommentsOverview::setAgentNotesUnread(int unread)
{
    if (!m_agentNotesButton) {
        return;
    }
    m_agentNotesButton->setText(unread > 0 ? i18n("Agent notes (%1)", unread) : i18n("Agent notes"));
    m_agentNotesButton->setToolTip(unread > 0 ? i18np("Agent notes: 1 unread", "Agent notes: %1 unread", unread)
                                              : i18n("Read the notes agents sent to your phone, with your replies"));
}

void CommentsOverview::updateReviewCardsButton(int activeCards, int dueCards)
{
    Q_UNUSED(activeCards)
    if (!m_reviewCardsButton) {
        return;
    }
    m_reviewCardsButton->setText(dueCards > 0 ? i18n("Review cards (%1)", dueCards) : i18n("Review cards"));
    m_reviewCardsButton->setToolTip(dueCards > 0
                                        ? i18np("Open Review cards; %1 card is due", "Open Review cards; %1 cards are due", dueCards)
                                        : i18n("Open Review cards"));
}

bool CommentsOverview::sourceMonitoringRefreshNeeded(const QString &pdfPath) const
{
    const bool summaryNeedsRefresh = isVisible() && m_tabs && m_tabs->currentWidget() == m_summaryPage
        && m_sourceMonitoringRequests.contains(pdfPath);
    if (summaryNeedsRefresh) {
        return true;
    }
    const QList<QDialog *> dialogs = findChildren<QDialog *>(QStringLiteral("documentInformationDialog"));
    return std::any_of(dialogs.cbegin(), dialogs.cend(), [&pdfPath](const QDialog *dialog) {
        return dialog->isVisible()
            && dialog->property("sourceMonitoringPdfPath").toString() == pdfPath;
    });
}

void CommentsOverview::setPdfSyncState(const QJsonObject &state)
{
    const QDateTime checkedAt = QDateTime::fromString(state.value(QStringLiteral("checked_at")).toString(), Qt::ISODateWithMs);
    if (checkedAt.isValid()) {
        m_pdfStatusCheckedAt = checkedAt;
    }
    if (state.contains(QStringLiteral("last_successful_build_at"))) {
        const QDateTime builtAt = QDateTime::fromString(state.value(QStringLiteral("last_successful_build_at")).toString(), Qt::ISODateWithMs);
        m_pdfLastSuccessfulBuildAt = builtAt;
    }
    const QJsonObject storage = state.value(QStringLiteral("storage")).toObject();
    if (state.value(QStringLiteral("server_summary")).isObject()) {
        m_fedoraServerSummary = state.value(QStringLiteral("server_summary")).toObject();
    }
    const qint64 totalBytes = storage.value(QStringLiteral("total_bytes")).toVariant().toLongLong();
    const qint64 usedBytes = storage.value(QStringLiteral("used_bytes")).toVariant().toLongLong();
    const qint64 availableBytes = storage.value(QStringLiteral("available_bytes")).toVariant().toLongLong();
    if (!storage.isEmpty() && totalBytes > 0 && usedBytes >= 0 && availableBytes >= 0) {
        m_fedoraStorageTotalBytes = totalBytes;
        m_fedoraStorageUsedBytes = usedBytes;
        m_fedoraStorageAvailableBytes = availableBytes;
        const QLocale locale;
        m_fedoraStorage->setText(i18n("%1 used of %2 · %3 free",
                                     locale.formattedDataSize(usedBytes, 1, QLocale::DataSizeIecFormat),
                                     locale.formattedDataSize(totalBytes, 1, QLocale::DataSizeIecFormat),
                                     locale.formattedDataSize(availableBytes, 1, QLocale::DataSizeIecFormat)));
        const QString details = i18n("Server storage: %1 bytes used of %2; %3 bytes available.",
                                     locale.toString(usedBytes),
                                     locale.toString(totalBytes),
                                     locale.toString(availableBytes));
        m_fedoraStorage->setToolTip(details);
        m_fedoraStorage->setAccessibleDescription(details);
        m_fedoraStorageProgress->setValue(qBound(0, int((usedBytes * 1000) / totalBytes), 1000));
        m_fedoraStorageProgress->setToolTip(details);
        m_fedoraStorageProgress->setAccessibleName(m_fedoraStorage->text());
        m_fedoraStorageProgress->show();
    }
    m_fedoraDetailsButton->setEnabled(m_fedoraStorageTotalBytes > 0 || !m_fedoraServerSummary.isEmpty());
    m_pdfStatusRefreshError = state.value(QStringLiteral("refresh_error")).toString().trimmed();
    updateIdleSyncPresentation();
    updateDashboardSummary();
}

void CommentsOverview::showServerDetails()
{
    showFedoraDetails();
}

void CommentsOverview::showFedoraDetails()
{
    if (auto *existing = findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    dialog->setWindowTitle(i18n("Server"));
    dialog->setMinimumWidth(500);

    auto *layout = new QVBoxLayout(dialog);
    const QString overallStatus = m_fedoraServerSummary.value(QStringLiteral("status")).toString();
    auto *summary = new QLabel(dialog);
    summary->setObjectName(QStringLiteral("thoughtsOverviewFedoraDetailsStatus"));
    summary->setWordWrap(true);
    if (overallStatus == QLatin1String("ok")) {
        summary->setText(i18n("The server is healthy."));
    } else if (overallStatus == QLatin1String("attention")) {
        summary->setText(i18n("The server needs attention."));
    } else {
        summary->setText(i18n("Some server health information is unavailable."));
    }
    QFont summaryFont = summary->font();
    summaryFont.setBold(true);
    summary->setFont(summaryFont);
    layout->addWidget(summary);

    // The address this computer's device token is used with. The running
    // connection keeps its address, so a new one applies after a restart.
    auto *addressRow = new QHBoxLayout;
    auto *addressLabel = new QLabel(i18n("Address:"), dialog);
    auto *address = new QLineEdit(dialog);
    address->setObjectName(QStringLiteral("thoughtsOverviewServerAddress"));
    address->setPlaceholderText(Thoughts::ServerAddress::defaultUrl());
    address->setText(Thoughts::ServerAddress::configured());
    address->setAccessibleName(i18n("Server address"));
    addressLabel->setBuddy(address);
    auto *saveAddress = new QPushButton(i18n("Save"), dialog);
    saveAddress->setObjectName(QStringLiteral("thoughtsOverviewServerAddressSave"));
    saveAddress->setAutoDefault(false);
    addressRow->addWidget(addressLabel);
    addressRow->addWidget(address, 1);
    addressRow->addWidget(saveAddress);
    layout->addLayout(addressRow);
    auto *addressStatus = new QLabel(dialog);
    addressStatus->setObjectName(QStringLiteral("thoughtsOverviewServerAddressStatus"));
    addressStatus->setWordWrap(true);
    addressStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(addressStatus);
    // The device tokens belong to the server that issued them. A new
    // installation, or an address that names another server, asks for that
    // server's two tokens here: the app's and the source publisher's. A token
    // stored for one server is not sent to another.
    auto *tokenStatus = new QLabel(dialog);
    tokenStatus->setObjectName(QStringLiteral("thoughtsOverviewServerTokenStatus"));
    tokenStatus->setWordWrap(true);
    tokenStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(tokenStatus);
    auto *tokenRow = new QWidget(dialog);
    tokenRow->setObjectName(QStringLiteral("thoughtsOverviewServerTokenRow"));
    auto *tokenLayout = new QGridLayout(tokenRow);
    tokenLayout->setContentsMargins(0, 0, 0, 0);
    auto *tokenLabel = new QLabel(i18n("App token:"), tokenRow);
    auto *token = new QLineEdit(tokenRow);
    token->setObjectName(QStringLiteral("thoughtsOverviewServerToken"));
    token->setEchoMode(QLineEdit::Password);
    token->setAccessibleName(i18n("Device token of the server"));
    tokenLabel->setBuddy(token);
    auto *sourceTokenLabel = new QLabel(i18n("Source publisher token:"), tokenRow);
    auto *sourceToken = new QLineEdit(tokenRow);
    sourceToken->setObjectName(QStringLiteral("thoughtsOverviewServerSourceToken"));
    sourceToken->setEchoMode(QLineEdit::Password);
    sourceToken->setAccessibleName(i18n("Source publisher token of the server"));
    sourceTokenLabel->setBuddy(sourceToken);
    auto *storeToken = new QPushButton(i18n("Store Tokens"), tokenRow);
    storeToken->setObjectName(QStringLiteral("thoughtsOverviewServerTokenStore"));
    storeToken->setAutoDefault(false);
    storeToken->setEnabled(false);
    tokenLayout->addWidget(tokenLabel, 0, 0);
    tokenLayout->addWidget(token, 0, 1);
    tokenLayout->addWidget(sourceTokenLabel, 1, 0);
    tokenLayout->addWidget(sourceToken, 1, 1);
    tokenLayout->addWidget(storeToken, 1, 2);
    tokenLayout->setColumnStretch(1, 1);
    layout->addWidget(tokenRow);
    const bool addressFromEnvironment = Thoughts::ServerAddress::overriddenByEnvironment();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    constexpr bool secretStore = true;
#else
    constexpr bool secretStore = false;
#endif
    const auto updateToken = [tokenStatus, tokenRow, addressFromEnvironment, secretStore] {
        const QString configured = Thoughts::ServerAddress::configured();
        const QString issuedFor = Thoughts::ServerAddress::tokenOrigin(Thoughts::Platform::desktopDeviceAccount());
        const QString target = Thoughts::ServerAddress::origin(configured);
        // No token recorded yet (a new installation) or one for another server.
        const bool needsToken = !target.isEmpty() && issuedFor != target;
        tokenStatus->setVisible(needsToken);
        // A token for a THOUGHTS_SERVER_URL address is given with THOUGHTS_TOKEN, not stored.
        // Without a secret store (Linux) a new installation is given its tokens in the environment.
        const bool environmentTokens = addressFromEnvironment || (!secretStore && issuedFor.isEmpty());
        tokenRow->setVisible(needsToken && !environmentTokens);
        if (!needsToken) {
            return;
        }
        if (environmentTokens) {
            tokenStatus->setText(issuedFor.isEmpty()
                    ? i18n("Give this computer's tokens for %1 in THOUGHTS_TOKEN and THOUGHTS_SOURCE_TOKEN.", target)
                    : i18n("This computer's device token was issued for %1 and is not sent to %2. Give the token for this session in THOUGHTS_TOKEN.", issuedFor, target));
        } else {
            tokenStatus->setText(issuedFor.isEmpty()
                    ? i18n("Enter the two tokens the owner of %1 gave you for this computer:", target)
                    : i18n("This computer's device token was issued for %1 and is not sent to %2. Enter the tokens of %2:", issuedFor, target));
        }
    };
    updateToken();
    const auto updateStore = [token, sourceToken, storeToken] {
        storeToken->setEnabled(!token->text().trimmed().isEmpty() || !sourceToken->text().trimmed().isEmpty());
    };
    connect(token, &QLineEdit::textChanged, storeToken, updateStore);
    connect(sourceToken, &QLineEdit::textChanged, storeToken, updateStore);
    connect(token, &QLineEdit::returnPressed, storeToken, &QPushButton::click);
    connect(sourceToken, &QLineEdit::returnPressed, storeToken, &QPushButton::click);
    connect(storeToken, &QPushButton::clicked, dialog, [this, saveAddress, token, sourceToken, tokenStatus, updateToken] {
        // The address comes first: each token is bound to the address saved
        // when it is stored, so an address typed but not saved is saved now.
        if (saveAddress->isEnabled()) {
            saveAddress->click();
            if (saveAddress->isEnabled()) {
                return;
            }
        }
        const QString saved = Thoughts::ServerAddress::saved();
        const bool firstConnection = Thoughts::ServerAddress::tokenOrigin(Thoughts::Platform::desktopDeviceAccount()).isEmpty();
        const QList<std::pair<QLineEdit *, QString>> fields{{token, Thoughts::Platform::desktopDeviceAccount()},
                                                            {sourceToken, QStringLiteral("thoughts-source-sync")}};
        for (const auto &[field, account] : fields) {
            const QByteArray secret = Thoughts::Platform::cleanSecret(field->text().toUtf8());
            if (secret.isEmpty()) {
                continue;
            }
            QString error;
            const bool stored = m_serverTokenWriter ? m_serverTokenWriter(account, secret, &error)
                                                    : Thoughts::Platform::writeSecret(Thoughts::ServerAddress::tokenService(), account, secret, &error);
            if (!stored || !Thoughts::ServerAddress::bindToken(account, secret, saved, &error)) {
                tokenStatus->setText(error.isEmpty() ? i18n("The token could not be stored.") : i18n("The token could not be stored: %1", error));
                return;
            }
            field->clear();
        }
        updateToken();
        tokenStatus->setVisible(true);
        if (firstConnection) {
            // Nothing has connected yet, so this Sync uses the new tokens.
            tokenStatus->setText(i18n("Stored the tokens of %1. Connecting…", Thoughts::ServerAddress::origin(saved)));
            Q_EMIT syncRequested();
        } else {
            // The running connection keeps the token it started with.
            tokenStatus->setText(i18n("Stored the tokens of %1. Quit and reopen Thoughts to connect.", Thoughts::ServerAddress::origin(saved)));
        }
    });
    if (addressFromEnvironment) {
        address->setReadOnly(true);
        saveAddress->setEnabled(false);
        addressStatus->setText(i18n("THOUGHTS_SERVER_URL sets the address for this session."));
    } else {
        const auto updateSave = [address, saveAddress] {
            saveAddress->setEnabled(Thoughts::ServerAddress::normalized(address->text()) != Thoughts::ServerAddress::saved());
        };
        updateSave();
        connect(address, &QLineEdit::textEdited, saveAddress, updateSave);
        connect(address, &QLineEdit::returnPressed, saveAddress, &QPushButton::click);
        connect(saveAddress, &QPushButton::clicked, dialog, [address, addressStatus, saveAddress, updateToken] {
            QString error;
            if (!Thoughts::ServerAddress::save(address->text(), &error)) {
                addressStatus->setText(error);
                return;
            }
            const QString saved = Thoughts::ServerAddress::saved();
            address->setText(saved);
            saveAddress->setEnabled(false);
            // A new installation connects once its tokens are stored; a
            // running connection keeps its address until a restart.
            const bool firstConnection = Thoughts::ServerAddress::tokenOrigin(Thoughts::Platform::desktopDeviceAccount()).isEmpty();
            addressStatus->setText(firstConnection ? i18n("Saved.") : i18n("Saved. Quit and reopen Thoughts to connect to %1.", saved));
            // Another server, or none yet: ask for its tokens.
            updateToken();
        });
    }

    auto *details = new QFormLayout;
    details->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto addDetail = [dialog, details](const QString &title, const QString &text, const QString &objectName) {
        auto *value = new QLabel(text, dialog);
        value->setObjectName(objectName);
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        details->addRow(title, value);
    };
    const auto byteValue = [](const QJsonObject &object, const QString &key) -> std::optional<qint64> {
        return nonNegativeJsonInteger(object.value(key));
    };
    const auto dateText = [](const QJsonValue &value) {
        const QDateTime date = publishedDate(value);
        return date.isValid() ? QLocale().toString(date.toLocalTime(), QLocale::ShortFormat) : QString();
    };
    const auto maintenanceText = [&dateText](const QJsonObject &maintenance) {
        const QString status = maintenance.value(QStringLiteral("status")).toString();
        const QString when = dateText(maintenance.value(QStringLiteral("last_run_at")));
        if (status == QLatin1String("successful")) {
            return when.isEmpty() ? i18n("Successful") : i18n("Successful · %1", when);
        }
        if (status == QLatin1String("running")) {
            return i18n("Running now");
        }
        if (status == QLatin1String("failed")) {
            return when.isEmpty() ? i18n("Failed") : i18n("Failed · %1", when);
        }
        return i18n("Unavailable");
    };

    QStringList systemParts;
    const QString operatingSystem = m_fedoraServerSummary.value(QStringLiteral("operating_system")).toString().trimmed();
    const QString architecture = m_fedoraServerSummary.value(QStringLiteral("architecture")).toString().trimmed();
    const int cpuCount = m_fedoraServerSummary.value(QStringLiteral("cpu_count")).toInt();
    if (!operatingSystem.isEmpty()) {
        systemParts.append(operatingSystem);
    }
    if (!architecture.isEmpty()) {
        systemParts.append(architecture);
    }
    if (cpuCount > 0) {
        systemParts.append(i18np("%1 processor core", "%1 processor cores", cpuCount));
    }
    addDetail(i18n("System:"), systemParts.isEmpty() ? i18n("Unavailable") : systemParts.join(QStringLiteral(" · ")), QStringLiteral("thoughtsOverviewFedoraDetailsSystem"));

    const auto uptimeSeconds = byteValue(m_fedoraServerSummary, QStringLiteral("uptime_seconds"));
    QString uptimeText = i18n("Unavailable");
    if (uptimeSeconds.has_value()) {
        const qint64 days = *uptimeSeconds / (24 * 60 * 60);
        const qint64 hours = (*uptimeSeconds / (60 * 60)) % 24;
        uptimeText = days > 0
            ? i18n("%1 · %2", i18np("1 day", "%1 days", days), i18np("1 hour", "%1 hours", hours))
            : i18np("1 hour", "%1 hours", hours);
    }
    addDetail(i18n("Uptime:"), uptimeText, QStringLiteral("thoughtsOverviewFedoraDetailsUptime"));

    const bool servicesAvailable = m_fedoraServerSummary.value(QStringLiteral("services_available")).toBool();
    const int servicesRunning = m_fedoraServerSummary.value(QStringLiteral("services_running")).toInt();
    const int servicesTotal = m_fedoraServerSummary.value(QStringLiteral("services_total")).toInt();
    addDetail(i18n("Services:"),
              servicesAvailable ? i18n("%1 of %2 running", servicesRunning, servicesTotal) : i18n("Unavailable"),
              QStringLiteral("thoughtsOverviewFedoraDetailsServices"));

    const auto memoryTotal = byteValue(m_fedoraServerSummary, QStringLiteral("memory_total_bytes"));
    const auto memoryAvailable = byteValue(m_fedoraServerSummary, QStringLiteral("memory_available_bytes"));
    const QLocale locale;
    const QString systemMemory = memoryTotal.has_value() && memoryAvailable.has_value()
        ? i18n("%1 available of %2",
               locale.formattedDataSize(*memoryAvailable, 1, QLocale::DataSizeIecFormat),
               locale.formattedDataSize(*memoryTotal, 1, QLocale::DataSizeIecFormat))
        : i18n("Unavailable");
    addDetail(i18n("System memory:"), systemMemory, QStringLiteral("thoughtsOverviewFedoraDetailsMemory"));

    QStringList thoughtsMemoryParts;
    if (const auto current = byteValue(m_fedoraServerSummary, QStringLiteral("thoughts_memory_current_bytes")); current.has_value()) {
        thoughtsMemoryParts.append(i18n("%1 now", locale.formattedDataSize(*current, 1, QLocale::DataSizeIecFormat)));
    }
    if (const auto peak = byteValue(m_fedoraServerSummary, QStringLiteral("worker_peak_memory_bytes")); peak.has_value()) {
        thoughtsMemoryParts.append(i18n("build worker peak %1 since restart", locale.formattedDataSize(*peak, 1, QLocale::DataSizeIecFormat)));
    }
    addDetail(i18n("Thoughts memory:"),
              thoughtsMemoryParts.isEmpty() ? i18n("Unavailable") : thoughtsMemoryParts.join(QStringLiteral(" · ")),
              QStringLiteral("thoughtsOverviewFedoraDetailsThoughtsMemory"));

    const int queuedBuilds = m_fedoraServerSummary.value(QStringLiteral("builds_queued")).toInt();
    const int runningBuilds = m_fedoraServerSummary.value(QStringLiteral("builds_running")).toInt();
    QStringList buildParts;
    buildParts.append(queuedBuilds == 0 ? i18n("No builds queued") : i18np("1 build queued", "%1 builds queued", queuedBuilds));
    buildParts.append(runningBuilds == 0 ? i18n("none running") : i18np("1 running", "%1 running", runningBuilds));
    const QString latestBuildStatus = m_fedoraServerSummary.value(QStringLiteral("latest_build_status")).toString();
    const QString latestBuildAt = dateText(m_fedoraServerSummary.value(QStringLiteral("latest_build_at")));
    if (!latestBuildStatus.isEmpty()) {
        QString statusText;
        if (latestBuildStatus == QLatin1String("succeeded")) {
            statusText = i18n("last build successful");
        } else if (latestBuildStatus == QLatin1String("failed")) {
            statusText = i18n("last build failed");
        } else if (latestBuildStatus == QLatin1String("attention")) {
            statusText = i18n("last build needs attention");
        } else if (latestBuildStatus == QLatin1String("cancelled")) {
            statusText = i18n("last build cancelled");
        } else {
            statusText = i18n("last build %1", latestBuildStatus);
        }
        buildParts.append(latestBuildAt.isEmpty() ? statusText : i18n("%1 · %2", statusText, latestBuildAt));
    }
    addDetail(i18n("Builds:"), buildParts.join(QStringLiteral(" · ")), QStringLiteral("thoughtsOverviewFedoraDetailsBuilds"));
    addDetail(i18n("Storage:"),
              m_fedoraStorageTotalBytes > 0 ? m_fedoraStorage->text() : i18n("Unavailable"),
              QStringLiteral("thoughtsOverviewFedoraDetailsStorage"));
    addDetail(i18n("Backup:"),
              maintenanceText(m_fedoraServerSummary.value(QStringLiteral("backup")).toObject()),
              QStringLiteral("thoughtsOverviewFedoraDetailsBackup"));
    addDetail(i18n("Integrity check:"),
              maintenanceText(m_fedoraServerSummary.value(QStringLiteral("integrity_check")).toObject()),
              QStringLiteral("thoughtsOverviewFedoraDetailsIntegrity"));
    layout->addLayout(details);

    auto *note = new QLabel(i18n("Detailed request, service, and build logs remain on the server."), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->open();
}

void CommentsOverview::updateIdleSyncPresentation()
{
    const QString documentState = m_aggregateSyncPresentation.value(QStringLiteral("state")).toString();
    const int pendingDocuments = m_aggregateSyncPresentation.value(QStringLiteral("pending_documents")).toInt(-1);
    const bool hasOperation = !m_latestSyncOperation.isEmpty();
    const SyncActivityPresentation activity = syncActivityPresentation(m_latestSyncOperation,
                                                                       m_pendingChanges,
                                                                       QDateTime::currentMSecsSinceEpoch(), pendingDocuments);
    const SyncOperationState operationState = syncOperationState(m_latestSyncOperation);
    const int mergeFiles = sourceFilesNeedingMerge(m_latestSyncOperation);
    int failedBuilds = 0;
    bool hasOtherDocumentErrors = false;
    for (const QJsonValue &value : m_documentSyncPresentations) {
        const QJsonObject document = value.toObject();
        const bool error = document.value(QStringLiteral("state")).toString() == QLatin1String("error");
        const bool buildFailure = document.value(QStringLiteral("last_build_failed")).toBool()
            || (error && document.value(QStringLiteral("requires_build")).toBool());
        if (buildFailure) ++failedBuilds;
        if (error && !buildFailure) hasOtherDocumentErrors = true;
    }
    // This reports the completed Sync outcome, not a claim that current sources
    // are up to date. Unknown inventory coverage must not erase that outcome.
    const bool syncedWithBuildFailure = m_syncSnapshotLoaded && failedBuilds > 0 && !hasOtherDocumentErrors
        && m_pendingSyncFileCount <= 0
        && (operationState == SyncOperationState::Succeeded
            || (operationState == SyncOperationState::Unknown && m_lastSuccessfulSyncAt.isValid())
            || (operationState == SyncOperationState::Failed
                && m_latestSyncOperation.value(QStringLiteral("last_error_code")).toString() == QLatin1String("pdf_build_failed")));
    // The status names the overall state; the first detail line says what
    // happened, for example whether the changes themselves arrived.
    QString stateDetail;
    if (mergeFiles > 0) {
        // Nothing else can sync until the conflicting files are merged.
        m_idlePdfSyncStatusText = SyncStatusLabels::mergeNeeded();
        stateDetail = SyncStatusLabels::sourceFilesNeedMerge(mergeFiles);
    } else if (syncedWithBuildFailure) {
        m_idlePdfSyncStatusText = SyncStatusLabels::pdfBuildFailed();
        stateDetail = SyncStatusLabels::changesSyncedWithFailedPdfs(failedBuilds);
    } else if (operationState == SyncOperationState::Failed
               && !(failedBuilds > 0 && !hasOtherDocumentErrors
                    && m_latestSyncOperation.value(QStringLiteral("last_error_code")).toString() == QLatin1String("pdf_build_failed"))) {
        m_idlePdfSyncStatusText = activity.status;
        if (m_latestSyncOperation.value(QStringLiteral("last_error_code")).toString() == QLatin1String("pdf_build_failed")) {
            stateDetail = SyncStatusLabels::changesSynced();
        }
    } else if (operationState == SyncOperationState::Interrupted || operationState == SyncOperationState::Cancelled) {
        m_idlePdfSyncStatusText = SyncStatusLabels::syncFailed();
        stateDetail = operationState == SyncOperationState::Cancelled ? i18n("Sync was cancelled before it completed.")
                                                                      : i18n("Sync was interrupted before it completed.");
    } else if (!m_syncSnapshotLoaded) {
        m_idlePdfSyncStatusText = i18n("Checking sync status…");
    } else if (documentState == QLatin1String("error") || failedBuilds > 0) {
        if (hasOtherDocumentErrors) {
            m_idlePdfSyncStatusText = SyncStatusLabels::syncFailed();
        } else if (failedBuilds > 0) {
            // Current local changes and the last PDF build are independent.
            // A pending edit must not hide a previously failed build.
            m_idlePdfSyncStatusText = SyncStatusLabels::pdfBuildFailed();
            const QString failed = i18np("1 PDF failed to build.", "%1 PDFs failed to build.", failedBuilds);
            stateDetail = m_pendingSyncFileCount > 0 ? SyncStatusLabels::changesPending() + QStringLiteral(" · ") + failed : failed;
        } else {
            m_idlePdfSyncStatusText = SyncStatusLabels::syncFailed();
        }
    } else if (documentState == QLatin1String("building")
               || documentState == QLatin1String("waiting_source_publication")
               || documentState == QLatin1String("syncing")) {
        m_idlePdfSyncStatusText = m_aggregateSyncPresentation.value(QStringLiteral("status")).toString();
    } else if (pendingDocuments > 0) {
        m_idlePdfSyncStatusText = SyncStatusLabels::changesPending();
    } else if (m_pendingSyncFileCount > 0) {
        m_idlePdfSyncStatusText = SyncStatusLabels::changesPending();
    } else if (m_pendingChanges.value(QStringLiteral("source_state")).toString() == QLatin1String("unknown")) {
        m_idlePdfSyncStatusText = i18n("Checking local changes…");
    } else if (m_pendingSyncFileCount < 0) {
        m_idlePdfSyncStatusText = i18n("Checking local changes…");
    } else if (m_pendingSyncFileCount == 0) {
        m_idlePdfSyncStatusText = activity.status;
        if (m_lastSuccessfulSyncAt.isValid() && activity.status == i18n("No pending changes")) {
            m_idlePdfSyncStatusText = SyncStatusLabels::upToDate();
        }
    }

    QStringList statusDetails;
    if (!stateDetail.isEmpty()) {
        statusDetails.append(stateDetail);
    }
    if (!m_syncSnapshotLoaded) {
        statusDetails.append(i18n("Waiting for local changes and document status."));
    }
    if (m_pendingSyncFileCount == 0) {
        statusDetails.append(i18n("No pending changes"));
    }
    if (m_pendingSyncFileCount > 0) {
        const int comments = m_pendingChanges.value(QStringLiteral("comments")).toInt();
        const int reviewCards = m_pendingChanges.value(QStringLiteral("review_cards")).toInt();
        const int reviewEvents = m_pendingChanges.value(QStringLiteral("review_events")).toInt();
        const int other = m_pendingChanges.value(QStringLiteral("other")).toInt();
        const int libraryChanges = m_pendingChanges.value(QStringLiteral("library_changes")).toInt();
        const int files = m_pendingChanges.value(QStringLiteral("files")).toInt();
        if (comments > 0) {
            statusDetails.append(i18np("%1 pending comment change", "%1 pending comment changes", comments));
        }
        if (reviewCards > 0) {
            statusDetails.append(i18np("%1 pending review card", "%1 pending review cards", reviewCards));
        }
        if (reviewEvents > 0) {
            statusDetails.append(i18np("%1 pending review event", "%1 pending review events", reviewEvents));
        }
        if (libraryChanges > 0) {
            statusDetails.append(i18np("%1 pending Library change", "%1 pending Library changes", libraryChanges));
        }
        if (other - libraryChanges > 0) {
            statusDetails.append(i18np("%1 other pending change", "%1 other pending changes", other - libraryChanges));
        }
        if (files > 0) {
            statusDetails.append(i18np("%1 changed source or build-input file", "%1 changed source or build-input files", files));
        }
    }
    if (m_pendingChanges.value(QStringLiteral("source_state")).toString() == QLatin1String("unknown")) {
        statusDetails.append(i18n("Local source tracking is incomplete; the last Sync result does not confirm that current sources are up to date."));
    }
    const QString pendingError = m_pendingChanges.value(QStringLiteral("error")).toString().trimmed();
    if (!pendingError.isEmpty()) {
        statusDetails.append(pendingError);
    }
    const QString documentStatus = m_aggregateSyncPresentation.value(QStringLiteral("reason")).toString();
    if (!documentStatus.isEmpty()) {
        statusDetails.append(i18n("PDF status: %1", documentStatus));
    }
    if (m_pdfStatusCheckedAt.isValid()) {
        statusDetails.append(i18n("Server observation time: %1", m_pdfStatusCheckedAt.toUTC().toString(Qt::ISODateWithMs)));
    }
    if (m_pdfLastSuccessfulBuildAt.isValid()) {
        statusDetails.append(i18n("Last successful server PDF build: %1", m_pdfLastSuccessfulBuildAt.toUTC().toString(Qt::ISODateWithMs)));
    }
    if (!m_pdfStatusRefreshError.isEmpty()) {
        statusDetails.append(m_pdfStatusRefreshError);
    }
    m_idlePdfSyncStatusDetails = statusDetails.join(QLatin1Char('\n'));

    if (hasOperation && !syncOperationIsRunning(m_latestSyncOperation)) {
        m_idlePdfSyncTimestampText = activity.detail;
        m_idlePdfSyncTimestampDetails = operationState == SyncOperationState::Failed ? syncOperationFailureMessage(m_latestSyncOperation) : QString();
    } else if (!m_syncSnapshotLoaded) {
        m_idlePdfSyncTimestampText = i18n("Last sync: Checking…");
        m_idlePdfSyncTimestampDetails.clear();
    } else if (m_lastSuccessfulSyncAt.isValid()) {
        m_idlePdfSyncTimestampText = i18n("Last sync: %1", compactSyncActivityAge(qMax<qint64>(0, m_lastSuccessfulSyncAt.secsTo(QDateTime::currentDateTimeUtc()))));
        m_idlePdfSyncTimestampDetails = i18n("Last successful full synchronization on this computer: %1", m_lastSuccessfulSyncAt.toString(Qt::ISODateWithMs));
    } else {
        m_idlePdfSyncTimestampText = i18n("Last sync: Never");
        m_idlePdfSyncTimestampDetails.clear();
    }
    if (!m_automaticSyncNotice.isEmpty()) {
        // Next to when the last Sync failed: when Thoughts may try again by itself.
        m_idlePdfSyncTimestampText += QStringLiteral(" · ") + m_automaticSyncNotice;
    }
    if (!m_syncOperationRunning) {
        // Merge needed offers only the merge; its log stays in Sync history.
        m_syncLinkShowsMerge = mergeFiles > 0;
        m_syncLogsLink->setText(QStringLiteral("<a href=\"%1\">%2</a>")
                                    .arg(m_syncLinkShowsMerge ? QStringLiteral("merge") : QStringLiteral("logs"),
                                         (m_syncLinkShowsMerge ? SyncStatusLabels::showMerge() : SyncStatusLabels::showLogs()).toHtmlEscaped()));
        m_syncLogsLink->setToolTip(m_syncLinkShowsMerge ? i18n("Show the source files that need a merge") : i18n("Open logs for the failed operation"));
        m_syncLogsLink->setVisible(m_syncLinkShowsMerge || operationState == SyncOperationState::Failed || failedBuilds > 0);
        restoreIdlePdfSyncPresentation();
    }
}

void CommentsOverview::restoreIdlePdfSyncPresentation()
{
    m_pdfSyncStatus->setText(m_idlePdfSyncStatusText);
    m_pdfSyncStatus->setToolTip(m_idlePdfSyncStatusDetails);
    m_pdfSyncStatus->setAccessibleDescription(m_idlePdfSyncStatusDetails);
    m_pdfSyncTimestamp->setText(m_idlePdfSyncTimestampText);
    m_pdfSyncTimestamp->setToolTip(m_idlePdfSyncTimestampDetails);
    m_pdfSyncTimestamp->setAccessibleDescription(m_idlePdfSyncTimestampDetails);
    updateDashboardSummary(false);
}

QVariantList CommentsOverview::documentSyncDescriptors() const
{
    QVariantList descriptors;
    descriptors.reserve(m_documents.size());
    const QSet<QString> pendingRemovals = pendingLibraryRemovalFilenames(m_pendingChanges);
    for (const TrackedCommentDocument &document : m_documents) {
        if (document.sourceProposalOnly || pendingRemovals.contains(document.libraryFilename.toCaseFolded())) {
            continue;
        }
        descriptors.append(QVariantMap {
            {QStringLiteral("stem"), document.stem},
            {QStringLiteral("filename"), document.libraryFilename},
            {QStringLiteral("pdf_path"), document.pdfPath},
            {QStringLiteral("source_linked"), document.libraryKind == QLatin1String("source_linked") && !document.sourceBuildDetached},
            {QStringLiteral("build_target"), !document.sourceUnselected},
            {QStringLiteral("library_pdf_stale"), document.libraryPdfStale},
            {QStringLiteral("latest_build_message"), document.libraryLatestBuildMessage},
        });
    }
    return descriptors;
}

void CommentsOverview::setSyncSnapshot(const QJsonObject &presentations,
                                       const QJsonObject &aggregate,
                                       const QJsonObject &pendingChanges,
                                       const QDateTime &lastSuccessfulSyncAt,
                                       const QJsonObject &latestSyncOperation)
{
    const bool pendingChanged = m_pendingChanges != pendingChanges;
    const bool presentationsChanged = m_documentSyncPresentations != presentations;
    const bool documentsChanged = pendingChanged || presentationsChanged;
    const bool monitoringChanged = pendingChanged || m_lastSuccessfulSyncAt != lastSuccessfulSyncAt;
    const QHash<QString, QString> previousBuildWarnings = documentsChanged ? libraryBuildWarnings() : QHash<QString, QString>();
    m_documentSyncPresentations = presentations;
    // A Sync that ends or clears a PDF build failure marks or unmarks its
    // Library rows; other presentation changes leave the tree as it is.
    if (documentsChanged && libraryBuildWarnings() != previousBuildWarnings) scheduleLibraryTreeUpdate(QStringLiteral("build_warnings"));
    m_aggregateSyncPresentation = aggregate;
    m_pendingChanges = pendingChanges;
    m_latestSyncOperation = latestSyncOperation;
    m_pendingSyncFileCount = pendingChanges.value(QStringLiteral("available")).toBool()
        ? pendingChanges.value(QStringLiteral("total")).toInt()
        : -1;
    m_lastSuccessfulSyncAt = lastSuccessfulSyncAt.isValid() ? lastSuccessfulSyncAt.toUTC() : QDateTime();
    m_syncSnapshotLoaded = true;
    updateIdleSyncPresentation();
    updateDashboardSummary(pendingChanged);
    if (monitoringChanged) { requestLibraryMonitoringInformation(true); }
    if (!m_syncOperationRunning) {
        m_syncButton->setText(i18n("Sync"));
        m_syncButton->setEnabled(aggregate.value(QStringLiteral("enabled")).toBool(true));
        m_syncButton->setToolTip(i18n("Publish source edits, synchronize changes, and update only PDFs whose build inputs changed"));
    }
    // The Library tree shows nothing of the pending changes, and of the Sync
    // states only each document's tooltip and icon.
    if (presentationsChanged) { refreshDocumentListSyncPresentations(); }
}

void CommentsOverview::setAutomaticSyncNotice(const QString &notice)
{
    if (m_automaticSyncNotice == notice) {
        return;
    }
    m_automaticSyncNotice = notice;
    updateIdleSyncPresentation();
}

void CommentsOverview::beginSyncElapsedPresentation(const QDateTime &startedAt)
{
    if (!m_syncOperationRunning) {
        m_syncStartedAt = startedAt.isValid() ? startedAt.toUTC() : QDateTime::currentDateTimeUtc();
    } else if (!m_syncStartedAt.isValid() && startedAt.isValid()) {
        m_syncStartedAt = startedAt.toUTC();
    }
    m_syncOperationRunning = true;
    m_syncLogsLink->hide();
    m_pdfSyncStatus->setText(SyncStatusLabels::syncing());
    m_pdfSyncStatus->setToolTip(QString());
    m_pdfSyncStatus->setAccessibleDescription(QString());
    updateSyncElapsedTimestamp();
    if (!m_syncElapsedTimer->isActive()) {
        m_syncElapsedTimer->start();
    }
}

void CommentsOverview::updateSyncElapsedTimestamp()
{
    if (!m_syncOperationRunning || !m_syncStartedAt.isValid()) {
        return;
    }
    const qint64 elapsedSeconds = std::max<qint64>(0, m_syncStartedAt.msecsTo(QDateTime::currentDateTimeUtc()) / 1000);
    const QString text = syncElapsedSecondsLabel(elapsedSeconds);
    m_pdfSyncTimestamp->setText(text);
    m_pdfSyncTimestamp->setToolTip(QString());
    m_pdfSyncTimestamp->setAccessibleDescription(text);
    updateSyncBuildSummary();
}

bool CommentsOverview::syncOperationRunning() const
{
    return m_syncOperationRunning;
}

void CommentsOverview::syncStarted(const QDateTime &startedAt)
{
    m_syncContentDeferred = true;
    m_obsoleteDocumentReloads.unite(m_runningDocumentReloads);
    m_reloadTimer->stop();
    if (m_reviewCardsWidget) { m_reviewCardsWidget->setSyncInProgress(true); }
    beginSyncElapsedPresentation(startedAt);
    m_syncButton->setEnabled(false);
    m_syncButton->setText(SyncStatusLabels::syncing());
    m_syncButton->setToolTip(i18n("Synchronization is in progress; use Logs to see its journal"));
}

void CommentsOverview::syncFinished(bool success, const QString &message, const QString &details, const QString &errorCode, const QString &operationId)
{
    m_syncContentDeferred = false;
    if (m_reviewCardsWidget) { m_reviewCardsWidget->setSyncInProgress(false); }
    m_syncOperationRunning = false;
    m_syncElapsedTimer->stop();
    m_syncStartedAt = QDateTime();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_latestSyncOperation = QJsonObject {{QStringLiteral("kind"), QStringLiteral("sync")},
                                         {QStringLiteral("status"), success ? QStringLiteral("succeeded") : QStringLiteral("failed")},
                                         {QStringLiteral("phase"), success ? QStringLiteral("succeeded") : QStringLiteral("failed")},
                                         {QStringLiteral("created_at_ms"), nowMs},
                                         {QStringLiteral("updated_at_ms"), nowMs}};
    m_latestSyncOperation.insert(QStringLiteral("id"), operationId);
    if (!success) {
        m_latestSyncOperation.insert(QStringLiteral("last_error_code"), errorCode);
        m_latestSyncOperation.insert(QStringLiteral("last_error_message"), syncOperationFailureJournalMessage(message, details));
    }
    updateIdleSyncPresentation();
    m_syncButton->setEnabled(m_aggregateSyncPresentation.value(QStringLiteral("enabled")).toBool(true));
    m_syncButton->setText(i18n("Sync"));
    m_syncButton->setToolTip(i18n("Publish source edits, synchronize changes, and update only PDFs whose build inputs changed"));
    if (!success) {
        const QString failureDetails = message.isEmpty() ? details : message + (details.isEmpty() ? QString() : QLatin1Char('\n') + details);
        m_pdfSyncTimestamp->setToolTip(failureDetails);
        m_pdfSyncTimestamp->setAccessibleDescription(failureDetails);
    }
    // A failed operation can still have downloaded valid changes.
    m_reloadTimer->stop();
    m_scheduledDocumentReloads.clear();
    m_reloadAgainDocuments.clear();
    m_fullReloadScheduled = false;
    m_catalogReloadScheduled = false;
    m_syncRefreshPending = true;
    requestReload();
}

void CommentsOverview::reload()
{
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_editorDirty || m_chatGptUrlDirty || m_operationPending) {
        m_reloadDeferred = true;
        return;
    }

    if (m_syncRefreshPending) { startFullReload(true); return; }

    const QModelIndex topVisibleIndex = m_table->indexAt(QPoint(1, 1));
    const QString topVisiblePdfPath = topVisibleIndex.data(PdfPathRole).toString();
    const QString topVisibleAnnotationId = topVisibleIndex.data(AnnotationIdRole).toString();
    const int previousScrollValue = m_table->verticalScrollBar()->value();
    const QString selectedStem = selectedDocument();
    const QList<TrackedCommentTarget> selectedTargets = selectedCommentTargets();
    const QModelIndex currentBeforeReload = m_table->selectionModel()->currentIndex().siblingAtColumn(CommentColumn);
    const QString currentPdfPath = currentBeforeReload.data(PdfPathRole).toString();
    const QString currentAnnotationId = currentBeforeReload.data(AnnotationIdRole).toString();
    m_changingSelection = true;
    m_documents = loadTrackedCommentDocuments(m_docsRoot, m_fedoraProjections);
    populateDocumentList(selectedStem);
    populateComments();
    refreshWatchPaths();
    updateDashboardSummary();
    applyFilters();
    m_changingSelection = false;
    if (!selectedTargets.isEmpty()) {
        restoreCommentSelection(selectedTargets, currentPdfPath, currentAnnotationId);
    } else {
        selectComment(QString(), QString());
    }
    QModelIndex topVisibleTarget;
    if (!topVisiblePdfPath.isEmpty() && !topVisibleAnnotationId.isEmpty()) {
        for (const QModelIndex &candidate : modelCommentIndexes(m_proxy)) {
            if (candidate.data(PdfPathRole).toString() == topVisiblePdfPath && candidate.data(AnnotationIdRole).toString() == topVisibleAnnotationId) {
                topVisibleTarget = candidate;
                break;
            }
        }
    }
    if (topVisibleTarget.isValid()) {
        m_table->scrollTo(topVisibleTarget, QAbstractItemView::PositionAtTop);
    } else {
        QScrollBar *scrollBar = m_table->verticalScrollBar();
        scrollBar->setValue(qBound(scrollBar->minimum(), previousScrollValue, scrollBar->maximum()));
    }
    m_reloadDeferred = false;
    refreshFileSnapshots();
    // A synchronous reload leaves no rebuild behind.
    if (m_libraryTreeDirty) updateLibraryTree();
}

void CommentsOverview::requestReload(const QString &reason)
{
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_editorDirty || m_chatGptUrlDirty || m_operationPending) {
        m_reloadDeferred = true;
        return;
    }
    startFullReload(m_syncRefreshPending, reason.isEmpty() ? QStringLiteral("request") : reason);
}

bool CommentsOverview::revealComment(const QString &pdfPath, const QString &annotationId)
{
    if (!resolveDirtyEditor()) {
        return false;
    }
    bool note = false;
    bool diary = false;
    for (const QModelIndex &candidate : modelCommentIndexes(m_model)) {
        if (candidate.data(PdfPathRole).toString() == pdfPath && candidate.data(AnnotationIdRole).toString() == annotationId) {
            QModelIndex root = candidate;
            while (root.parent().isValid()) { root = root.parent().siblingAtColumn(CommentColumn); }
            note = root.data(ItemTypeRole).toString() == QLatin1String("note");
            diary = note && diaryEntry(root.data(LogicalPayloadRole).value<QJsonObject>());
            break;
        }
    }
    if (diary) {
        for (QTreeWidgetItemIterator it(m_diaryList); *it; ++it) {
            if ((*it)->data(0, DiaryIdRole).toString() != annotationId) continue;
            m_tabs->setCurrentWidget(m_diaryPage);
            m_diaryList->setCurrentItem(*it);
            m_diaryList->scrollToItem(*it);
            m_diaryList->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    m_tabs->setCurrentWidget(note ? m_notesPage : m_commentsPage);
    resetBrowserFilters();
    selectComment(pdfPath, annotationId);
    if (m_currentComment.isValid() && m_currentPdfPath == pdfPath && m_currentAnnotationId == annotationId) {
        return true;
    }
    m_pendingRevealPdfPath = pdfPath;
    m_pendingRevealAnnotationId = annotationId;
    m_pendingRevealFallbackRow = -1;
    requestReload();
    return true;
}

void CommentsOverview::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    DeveloperLog::record(QStringLiteral("library.overview.shown"));
    if (!m_restoredWindowState) {
        restoreWindowState();
        m_restoredWindowState = true;
    }
    if (!m_initialProposalDiscoveryDone) {
        m_initialProposalDiscoveryDone = true;
        m_projectStoreIdentity = Thoughts::artifactFileIdentity(Thoughts::LibraryProjectStore(m_docsRoot).path());
        startFullReload(true, QStringLiteral("first_show"));
    } else {
        requestReload();
    }
    reloadReviewCards();
    // Changes may have arrived while the overview was hidden.
    requestCatalogCheck(true);
    if (m_libraryTreeDirty) {
        // Rebuild the tree once, after the reads started above.
        m_libraryTreeDirtySince.start();
        m_libraryTreeTimer->start();
    }
    if (m_tabs->currentWidget() == m_commentsPage) {
        m_search->setFocus(Qt::OtherFocusReason);
    }
}

void CommentsOverview::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_libraryTreeDirty && libraryTreeOnScreen()) {
        m_libraryTreeDirtySince.start();
        m_libraryTreeTimer->start();
    }
}

void CommentsOverview::closeEvent(QCloseEvent *event)
{
    if (!resolveDirtyEditor()) {
        event->ignore();
        return;
    }
    saveWindowState();
    m_search->clear();
    m_documentList->setCurrentItem(m_documentList->topLevelItem(0));
    QDialog::closeEvent(event);
}

bool CommentsOverview::event(QEvent *event)
{
    return QDialog::event(event);
}

bool CommentsOverview::resolveDirtyEditor()
{
    if (m_chatGptUrlDirty && !saveChatGptUrl(false)) {
        return false;
    }
    if (!m_editorDirty) {
        return true;
    }
    saveSelectedComment();
    return !m_editorDirty && !m_operationPending;
}

void CommentsOverview::selectedCommentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    traceInteraction(QStringLiteral("comments.current_changed"), current);
    if (m_changingSelection) {
        return;
    }
    if (m_chatGptUrlDirty) {
        const QModelIndex target = current.siblingAtColumn(CommentColumn);
        const QString targetPdfPath = target.data(PdfPathRole).toString();
        const QString targetAnnotationId = target.data(AnnotationIdRole).toString();
        if (!saveChatGptUrl(false)) {
            m_changingSelection = true;
            m_table->selectionModel()->setCurrentIndex(previous, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_changingSelection = false;
            return;
        }
        if (!m_editorDirty) {
            selectComment(targetPdfPath, targetAnnotationId);
            return;
        }
    }
    if (m_editorDirty) {
        const QList<TrackedCommentTarget> requestedSelection = selectedCommentTargets();
        if (requestedSelection.size() > 1) {
            m_pendingSelection = requestedSelection;
            const QModelIndex requestedCurrent = m_table->selectionModel()->currentIndex().siblingAtColumn(CommentColumn);
            m_pendingCurrentPdfPath = requestedCurrent.data(PdfPathRole).toString();
            m_pendingCurrentAnnotationId = requestedCurrent.data(AnnotationIdRole).toString();
            updateSelectionUi();
            saveSelectedComment();
            return;
        }
        const QModelIndex target = current.siblingAtColumn(CommentColumn);
        const QString targetPdfPath = target.data(PdfPathRole).toString();
        const QString targetAnnotationId = target.data(AnnotationIdRole).toString();
        if (!resolveDirtyEditor()) {
            m_changingSelection = true;
            m_table->selectionModel()->setCurrentIndex(previous, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_changingSelection = false;
            return;
        }
        selectComment(targetPdfPath, targetAnnotationId);
        return;
    }
    loadComment(current);
}

void CommentsOverview::loadComment(const QModelIndex &index)
{
    if (!index.isValid()) {
        clearComment();
        return;
    }

    const QModelIndex commentIndex = index.siblingAtColumn(CommentColumn);
    m_currentComment = commentIndex;
    m_currentPdfPath = commentIndex.data(PdfPathRole).toString();
    m_currentAnnotationId = commentIndex.data(AnnotationIdRole).toString();
    m_originalContents = commentIndex.data(ContentsRole).toString();
    m_originalChatGptUrl = commentIndex.data(ChatGptUrlRole).toString();

    const QString source = commentIndex.data(SourceDisplayRole).toString();
    const QString status = commentIndex.data(StatusDisplayRole).toString();
    const QString author = commentIndex.data(AuthorRole).toString();
    const bool proposal = commentIndex.data(ItemTypeRole).toString() == QLatin1String("agent_remove");
    LibraryProjectStore projectStore(m_docsRoot);
    const QJsonArray projects = projectStore.error().isEmpty() ? projectStore.projects(false) : QJsonArray();
    QJsonArray locations = commentIndex.data(PdfLocationsRole).value<QJsonArray>();
    if (locations.isEmpty()) {
        locations.append(QJsonObject {{QStringLiteral("pdf_filename"), QFileInfo(m_currentPdfPath).fileName()},
                                      {QStringLiteral("page"), commentIndex.data(PageRole).toInt()}});
    }
    QStringList metadata;
    for (const QJsonValue &value : locations) {
        const QJsonObject location = value.toObject();
        const QString filename = location.value(QStringLiteral("pdf_filename")).toString();
        const QString hash = location.value(QStringLiteral("pdf_sha256")).toString();
        const int page = location.value(QStringLiteral("status")).toString() == QLatin1String("unavailable") ? 0 : location.value(QStringLiteral("page")).toInt();
        QString documentName;
        int documentPage = page;
        int projectPage = 0;
        QStringList projectNames;
        bool requiresMapping = false;
        for (const QJsonValue &projectValue : projects) {
            const QJsonObject project = projectValue.toObject();
            QJsonObject matchedDocument;
            for (const QJsonValue &documentValue : project.value(QStringLiteral("documents")).toArray()) {
                const QJsonObject candidate = documentValue.toObject();
                if (candidate.value(QStringLiteral("filename")).toString().compare(filename, Qt::CaseInsensitive) == 0) {
                    matchedDocument = candidate;
                    break;
                }
            }
            const bool target = !filename.isEmpty() && project.value(QStringLiteral("target")).toString().compare(filename, Qt::CaseInsensitive) == 0;
            if (matchedDocument.isEmpty() && !target) { continue; }
            QString projectName = project.value(QStringLiteral("name")).toString();
            if (projectName.isEmpty()) { projectName = project.value(QStringLiteral("id")).toString(); }
            projectNames.append(projectName);
            if (documentName.isEmpty()) { documentName = matchedDocument.value(QStringLiteral("name")).toString(); }
            if (matchedDocument.isEmpty() && target) {
                requiresMapping = true;
                projectPage = page;
                documentPage = 0;
            }
            const QJsonObject composition = project.value(QStringLiteral("composition")).toObject();
            if (page <= 0 || hash.isEmpty()) { continue; }
            const bool combined = composition.value(QStringLiteral("filename")).toString() == filename
                && composition.value(QStringLiteral("sha256")).toString() == hash;
            QList<QJsonObject> matches;
            for (const QJsonValue &segmentValue : composition.value(QStringLiteral("segments")).toArray()) {
                const QJsonObject segment = segmentValue.toObject();
                const int start = segment.value(QStringLiteral("start_page")).toInt();
                const int count = segment.value(QStringLiteral("page_count")).toInt();
                if (start > 0 && count > 0 && (combined ? page >= start && page - start < count
                    : segment.value(QStringLiteral("filename")).toString() == filename
                        && segment.value(QStringLiteral("sha256")).toString() == hash && page <= count)) {
                    matches.append(segment);
                }
            }
            if (matches.size() == 1) {
                const QJsonObject segment = matches.first();
                const int start = segment.value(QStringLiteral("start_page")).toInt();
                documentName = segment.value(QStringLiteral("name")).toString();
                documentPage = combined ? page - start + 1 : page;
                projectPage = combined ? page : start + page - 1;
            }
        }
        projectNames.removeDuplicates();
        QString projectName = projectNames.isEmpty() ? i18n("Project information is unavailable") : projectNames.join(QStringLiteral(", "));
        if (projectPage > 0) { projectName += i18n(" · Page %1", projectPage); }
        if (documentName.isEmpty() && !requiresMapping) {
            const auto tracked = std::find_if(m_documents.cbegin(), m_documents.cend(), [&filename](const TrackedCommentDocument &candidate) {
                return QFileInfo(candidate.pdfPath).fileName().compare(filename, Qt::CaseInsensitive) == 0;
            });
            documentName = tracked == m_documents.cend() ? QFileInfo(filename).completeBaseName() : tracked->displayName;
        }
        if (documentName.isEmpty()) { documentName = i18n("Document information is unavailable"); }
        metadata.append(i18n("Project: %1\nDocument: %2 · %3", projectName, documentName,
                             documentPage > 0 ? i18n("Page %1", documentPage) : i18n("Page unavailable")));
    }
    metadata.removeDuplicates();
    metadata.append(i18n("Linking status: %1", status));
    m_commentMetadata->setText(metadata.join(QLatin1Char('\n')));
    m_commentMetadata->setToolTip(i18n("Source: %1\nAuthor: %2", source, author.isEmpty() ? QStringLiteral("—") : author));

    const QString selectedText = commentIndex.data(SelectedTextRole).toString().trimmed();
    m_proposalPreview->setVisible(proposal);
    if (proposal) {
        const QString newVersion = commentIndex.data(ProposalAfterPreviewRole).toString();
        const QString oldVersion = commentIndex.data(ProposalBeforePreviewRole).toString();
        m_proposalPreview->setPlainText(i18n("New version: %1\n\nOld version: %2\n\nMotivation: %3",
                                     newVersion.isEmpty() ? i18n("[delete]") : newVersion,
                                     oldVersion.isEmpty() ? i18n("[empty]") : oldVersion,
                                     m_originalContents));
        m_proposalPreview->verticalScrollBar()->setValue(0);
        m_selectedText->hide();
    } else {
        m_proposalPreview->clear();
        m_selectedText->setText(selectedText.isEmpty() ? QString() : i18n("Selected text: %1", selectedText));
        m_selectedText->setVisible(!selectedText.isEmpty());
    }
    const QJsonObject correspondence = commentIndex.data(CorrespondenceRole).value<QJsonObject>();
    m_correspondenceEvidence->setPlainText(reviewCorrespondenceEvidenceText(correspondence));
    m_correspondenceEvidence->setVisible(!correspondence.isEmpty());
    const QString correspondenceState = correspondence.value(QStringLiteral("state")).toString();
    const bool exactCorrespondence = correspondenceState == QLatin1String("exact_text") || correspondenceState == QLatin1String("exact_point");
    m_confirmCorrespondenceButton->setVisible(!correspondence.isEmpty());
    m_confirmCorrespondenceButton->setEnabled(exactCorrespondence && !m_operationPending);
    m_flagCorrespondenceButton->setVisible(!correspondence.isEmpty());
    m_flagCorrespondenceButton->setEnabled(exactCorrespondence && !m_operationPending);
    m_recomputeCorrespondenceButton->setVisible(!correspondence.isEmpty());
    m_recomputeCorrespondenceButton->setEnabled(!m_operationPending);

    m_changingSelection = true;
    const auto notePayload = commentIndex.data(LogicalPayloadRole).value<QJsonObject>();
    const bool standaloneNote = notePayload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note");
    m_noteDateFields->setVisible(standaloneNote);
    m_originalDueDate = standaloneNote ? notePayload.value(QStringLiteral("calendar_date")).toString() : QString();
    m_noteDueDate->setText(m_originalDueDate);
    m_originalVisibleFrom = standaloneNote ? notePayload.value(QStringLiteral("visible_from")).toString() : QString();
    m_noteVisibleFrom->setText(m_originalVisibleFrom);
    if (standaloneNote) {
        // Project, document and linking say nothing about a to-do.
        const QString where = notePayload.value(QStringLiteral("check_where")).toString().trimmed();
        m_commentMetadata->setText(where.isEmpty() ? QString() : i18n("Where: %1", where));
        m_commentMetadata->setVisible(!where.isEmpty());
    } else {
        m_commentMetadata->setVisible(true);
    }
    m_commentEditor->setPlainText(m_originalContents);
    m_commentEditor->setVisible(!proposal);
    while (auto *child = m_noteAttachments->layout()->takeAt(0)) { delete child->widget(); delete child; }
    const QJsonArray attachments = commentIndex.data(LogicalPayloadRole).value<QJsonObject>().value(QStringLiteral("attachments")).toArray();
    m_noteAttachments->setVisible(!proposal && !attachments.isEmpty());
    for (const auto &value : attachments) {
        const QJsonObject attachment = value.toObject();
        // The server chose the name: show it without invisible or direction-changing characters, and
        // say what a click does. Only documents, pictures and text are opened (part/noteattachment.h).
        const QString attachmentName = attachment.value(QStringLiteral("name")).toString();
        const QString shownName = Thoughts::NoteAttachment::displayName(attachmentName);
        const bool opens = Thoughts::NoteAttachment::actionFor(attachmentName) == Thoughts::NoteAttachment::Action::Open;
        auto *button = new QPushButton(opens ? i18n("Open attachment: %1", shownName) : i18n("Show attachment: %1", shownName), m_noteAttachments);
        button->setProperty("thoughtsAttachmentOpens", opens);
        button->setToolTip(opens ? i18n("Download and open this attachment. Downloaded files remain available offline.")
                                 : i18n("Download this attachment and show the file in its folder. Thoughts opens only documents, pictures and text itself."));
        // A long file name is clipped rather than widening the whole panel.
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_noteAttachments->layout()->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, attachment] {
            Q_EMIT noteAttachmentOpenRequested(attachment.toVariantMap());
        });
    }
    m_chatGptUrlEditor->setText(m_originalChatGptUrl);
    const QString rawPriority = commentIndex.data(PriorityRole).toString();
    m_originalPriority = rawPriority.isEmpty() ? QStringLiteral("normal") : rawPriority;
    m_originalLocationPresent = commentIndex.data(LocationPresentRole).toBool();
    m_originalLocation = commentIndex.data(LocationObjectRole).value<QJsonObject>();
    m_locationTouched = false;
    m_locationEnabled->setChecked(m_originalLocationPresent);
    m_locationLabel->setText(m_originalLocation.value(QStringLiteral("label")).toString());
    m_locationLatitude->setValue(commentIndex.data(LatitudeRole).toDouble());
    m_locationLongitude->setValue(commentIndex.data(LongitudeRole).toDouble());
    m_changingSelection = false;
    m_editorDirty = false;
    m_chatGptUrlDirty = false;
    updateSelectionUi();
}

void CommentsOverview::clearComment()
{
    m_noteDateFields->hide();
    m_currentComment = QPersistentModelIndex();
    m_currentPdfPath.clear();
    m_currentAnnotationId.clear();
    m_originalContents.clear();
    m_originalPriority = QStringLiteral("normal");
    m_originalChatGptUrl.clear();
    m_pendingChatGptUrl.clear();
    m_originalLocation = {};
    m_originalLocationPresent = false;
    m_locationTouched = false;
    m_commentMetadata->setVisible(true);
    m_commentMetadata->setText(i18n("Select a comment to inspect or edit it."));
    m_selectedText->clear();
    m_selectedText->hide();
    m_proposalPreview->clear();
    m_proposalPreview->hide();
    m_correspondenceEvidence->clear();
    m_correspondenceEvidence->hide();
    m_confirmCorrespondenceButton->hide();
    m_flagCorrespondenceButton->hide();
    m_recomputeCorrespondenceButton->hide();
    m_changingSelection = true;
    m_commentEditor->clear();
    m_commentEditor->show();
    m_noteAttachments->hide();
    m_chatGptUrlEditor->clear();
    m_locationEnabled->setChecked(false);
    m_locationLabel->clear();
    m_locationLatitude->setValue(0.0);
    m_locationLongitude->setValue(0.0);
    m_changingSelection = false;
    m_editorDirty = false;
    m_chatGptUrlDirty = false;
    updateSelectionUi();
}

void CommentsOverview::selectComment(const QString &pdfPath, const QString &annotationId, int fallbackRow)
{
    QModelIndex target;
    const QModelIndexList indexes = modelCommentIndexes(m_proxy);
    for (const QModelIndex &candidate : indexes) {
        if (candidate.data(PdfPathRole).toString() == pdfPath && candidate.data(AnnotationIdRole).toString() == annotationId) {
            target = candidate;
            break;
        }
    }
    if (!target.isValid() && fallbackRow >= 0 && !indexes.isEmpty()) {
        target = indexes.at(qMin(fallbackRow, indexes.size() - 1));
    }
    if (target.isValid()) {
        m_table->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_table->scrollTo(target);
        loadComment(target);
    } else {
        m_table->selectionModel()->clear();
        clearComment();
    }
}

void CommentsOverview::removeHistoryForPdf(const QString &pdfPath)
{
    removeUndoHistoryForPdf(pdfPath);
    removeRedoHistoryForPdf(pdfPath);
}

void CommentsOverview::removeUndoHistoryForPdf(const QString &pdfPath)
{
    // A proposal decision belongs to its sources, not to a PDF.
    m_undoHistory.removeIf([&pdfPath](const HistoryEntry &entry) { return entry.proposalSources.isEmpty() && entry.pdfPath == pdfPath; });
}

void CommentsOverview::removeRedoHistoryForPdf(const QString &pdfPath)
{
    m_redoHistory.removeIf([&pdfPath](const HistoryEntry &entry) { return entry.proposalSources.isEmpty() && entry.pdfPath == pdfPath; });
}

void CommentsOverview::setOperationPending(bool pending)
{
    m_operationPending = pending;
    if (pending && m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }
    setCursor(pending ? Okular::busyCursor() : QCursor(Qt::ArrowCursor));
    updateEditorActions();
    if (!pending && m_restoreEditorFocusAfterOperation) {
        m_restoreEditorFocusAfterOperation = false;
        if (m_commentEditor->isEnabled()) {
            m_commentEditor->setFocus(Qt::OtherFocusReason);
        }
    }
}

void CommentsOverview::updateEditorActions()
{
    m_addNoteButton->setEnabled(!m_operationPending);
    const bool singleSelection = m_table && m_table->selectionModel() && m_table->selectionModel()->selectedRows().size() == 1;
    const bool currentProposal = singleSelection && m_currentComment.isValid() && m_currentComment.data(ItemTypeRole).toString() == QLatin1String("agent_remove");
    const bool logicalComment = singleSelection && m_currentComment.isValid() && m_currentComment.data(LogicalCommentRole).toBool();
    const bool missingMarker = m_currentComment.isValid() && m_currentComment.data(AnchorStatusRole).toString() == QLatin1String("comment_missing");
    const bool validComment = singleSelection && !currentProposal && !missingMarker && m_currentComment.isValid() && !m_currentAnnotationId.isEmpty()
        && (logicalComment || (!m_currentPdfPath.isEmpty() && m_currentComment.data(PdfAvailableRole).toBool()));
    m_commentEditor->setEnabled(validComment && !m_operationPending);
    m_locationEnabled->setEnabled(validComment && !m_operationPending);
    const bool locationFieldsEnabled = validComment && m_locationEnabled->isChecked() && !m_operationPending;
    m_locationLabel->setEnabled(locationFieldsEnabled);
    m_locationLatitude->setEnabled(locationFieldsEnabled);
    m_locationLongitude->setEnabled(locationFieldsEnabled);
    m_openCodexButton->setEnabled(validComment && !m_operationPending);
    m_chatGptUrlEditor->setEnabled(validComment && !m_operationPending);
    QString chatGptError;
    const QString chatGptUrl = Okular::DocsCommentSynchronizer::normalizedChatGptUrl(m_chatGptUrlEditor->text(), &chatGptError);
    m_openChatGptButton->setEnabled(validComment && !m_operationPending && chatGptError.isEmpty() && !chatGptUrl.isEmpty());
    QString sourcePath;
    QString sourceUnavailableReason;
    int sourceLine = 0;
    const bool sourceAvailable = currentSourceTarget(&sourcePath, &sourceLine, &sourceUnavailableReason);
    m_openSourceButton->setEnabled(sourceAvailable && !m_operationPending);
    if (m_operationPending) {
        m_openSourceButton->setToolTip(i18n("Wait for the current comment operation to finish."));
    } else if (sourceAvailable) {
        const QString relativeSource = QDir(m_docsRoot).relativeFilePath(sourcePath);
        const QString shownSource = relativeSource.startsWith(QLatin1String("..")) ? sourcePath : relativeSource;
        m_openSourceButton->setToolTip(i18n("Open %1:%2 in Visual Studio Code.", shownSource, sourceLine));
    } else {
        m_openSourceButton->setToolTip(sourceUnavailableReason);
    }
    m_saveCommentButton->setEnabled(validComment && m_editorDirty && !m_operationPending);
    m_replyCommentButton->setEnabled(validComment && !m_operationPending
                                    && m_currentComment.data(ItemTypeRole).toString() != QLatin1String("note"));
    const QList<QJsonObject> feedbackRecords = selectedFeedbackRecords();
    m_deleteCommentButton->setEnabled((!selectedCommentTargets().isEmpty() || !feedbackRecords.isEmpty()) && !m_operationPending);
    // Written in the document, where the comment's editor is; here it can be resolved or deleted.
    const bool feedbackRow = m_currentComment.isValid() && !m_currentComment.data(FeedbackKindRole).toString().isEmpty();
    m_commentEditor->setReadOnly(feedbackRow);
    const bool resolvable = feedbackRecords.size() == 1 && Thoughts::DocumentFeedback::canResolve(feedbackRecords.constFirst());
    m_resolveFeedbackButton->setVisible(resolvable);
    if (resolvable) m_resolveFeedbackButton->setText(Thoughts::DocumentFeedback::isResolved(feedbackRecords.constFirst()) ? i18n("Reopen") : i18n("Resolve"));
    const QStringList anchorRemovedIds = selectedAnchorRemovedIds();
    m_restoreAnchorButton->setVisible(!anchorRemovedIds.isEmpty());
    m_restoreAnchorButton->setEnabled(!anchorRemovedIds.isEmpty() && !m_operationPending);
    m_restoreAnchorButton->setText(i18np("Restore Anchor", "Restore %1 Anchors", qMax(1, int(anchorRemovedIds.size()))));
    const int missingMarkers = selectedMissingCommentMarkers().size();
    m_removeMissingMarkerButton->setVisible(missingMarkers > 0);
    m_removeMissingMarkerButton->setEnabled(missingMarkers > 0 && !m_operationPending);
    m_removeMissingMarkerButton->setText(i18np("Remove Marker", "Remove %1 Markers", qMax(1, missingMarkers)));
    m_restoreMissingCommentButton->setVisible(missingMarkers > 0);
    m_restoreMissingCommentButton->setEnabled(missingMarkers > 0 && !m_operationPending);
    m_restoreMissingCommentButton->setText(i18np("Restore Comment", "Restore %1 Comments", qMax(1, missingMarkers)));
    const QStringList removedProposalIds = selectedRemovedProposalIds();
    const bool removedProposalsSelected = !removedProposalIds.isEmpty();
    for (QPushButton *button : {m_restoreProposalButton, m_dismissProposalButton, m_copyProposalButton}) {
        button->setVisible(removedProposalsSelected);
        button->setEnabled(removedProposalsSelected && !m_operationPending);
    }
    m_restoreProposalButton->setText(i18np("Restore Proposal", "Restore %1 Proposals", qMax(1, int(removedProposalIds.size()))));
    m_dismissProposalButton->setText(i18np("Dismiss", "Dismiss %1 Proposals", qMax(1, int(removedProposalIds.size()))));
    const QStringList selectedProposalIds = selectedAgentProposalIds();
    const bool proposalsSelected = !selectedProposalIds.isEmpty();
    // Broken proposals keep the proposal actions in view, disabled, with their issue.
    QString brokenProposalIssue;
    if (!proposalsSelected && m_table && m_table->selectionModel()) {
        const QModelIndexList rows = m_table->selectionModel()->selectedRows();
        const bool allProposals = !rows.isEmpty() && std::all_of(rows.cbegin(), rows.cend(), [](const QModelIndex &row) {
            return row.data(ItemTypeRole).toString() == QLatin1String("agent_remove");
        });
        for (const QModelIndex &row : rows) {
            if (allProposals && !row.data(ProposalIssueRole).toString().isEmpty()) {
                brokenProposalIssue = Thoughts::agentProposalIssueMessage(row.data(ProposalIssueRole).toString());
                break;
            }
        }
    }
    const bool brokenProposalSelected = !brokenProposalIssue.isEmpty();
    const QString brokenTip = brokenProposalSelected ? i18n("This proposal cannot be decided until its source is fixed: %1", brokenProposalIssue) : QString();
    m_acceptProposalButton->setVisible(proposalsSelected || brokenProposalSelected);
    m_disregardProposalButton->setVisible(proposalsSelected || brokenProposalSelected);
    m_acceptProposalButton->setToolTip(brokenTip);
    m_disregardProposalButton->setToolTip(brokenTip);
    m_acceptProposalButton->setEnabled(proposalsSelected && !m_operationPending);
    // The count is that of the proposals Accept and Reject decide, as in the context menu.
    if (selectedProposalIds.size() > 1) {
        m_acceptProposalButton->setText(i18np("Accept Proposal", "Accept %1 Proposals", selectedProposalIds.size()));
        m_disregardProposalButton->setText(i18np("Reject Proposal", "Reject %1 Proposals", selectedProposalIds.size()));
    } else {
        if (proposalsSelected) {
            const QModelIndexList rows = decidableSelectedProposalRows();
            const bool allEdits = std::all_of(rows.cbegin(), rows.cend(), [](const QModelIndex &row) { return row.data(ProposalKindRole).toString() == QLatin1String("edit"); });
            const bool allRemovals = std::all_of(rows.cbegin(), rows.cend(), [](const QModelIndex &row) { return row.data(ProposalKindRole).toString() != QLatin1String("edit"); });
            m_acceptProposalButton->setText(allEdits ? i18n("Accept Edit") : allRemovals ? i18n("Accept Removal") : i18n("Accept Proposal"));
        }
        m_disregardProposalButton->setText(i18n("Reject Proposal"));
    }
    m_disregardProposalButton->setEnabled(proposalsSelected && !m_operationPending);
    // Priority, location and assistant links belong to comments; an agent
    // proposal has none of them, so hide the rows instead of greying them out.
    const bool showCommentFields = !currentProposal && !proposalsSelected && !brokenProposalSelected && !feedbackRow;
    // A to-do has no place unless one was attached earlier (the phone uses "Where" text instead).
    const bool showLocation = showCommentFields && (!m_notesActive || m_originalLocationPresent);
    for (QWidget *field : {static_cast<QWidget *>(m_locationEnabled), static_cast<QWidget *>(m_locationLabel), m_locationCoordinates}) {
        m_metadataEditorLayout->setRowVisible(field, showLocation);
    }
    // Notes are not tied to a source, a Codex task or a ChatGPT chat.
    m_metadataEditorLayout->setRowVisible(m_assistantActions, showCommentFields && !m_notesActive);
    m_metadataEditorLayout->setRowVisible(m_openSourceButton, !m_notesActive);
    m_deleteCommentButton->setVisible(!proposalsSelected && !brokenProposalSelected && !removedProposalsSelected);
    m_replyCommentButton->setVisible(!proposalsSelected && !brokenProposalSelected && !removedProposalsSelected && !feedbackRow);
    m_saveCommentButton->setVisible(false);
    m_undoButton->setEnabled(!m_undoHistory.isEmpty() && !m_operationPending);
    m_redoButton->setEnabled(!m_redoHistory.isEmpty() && !m_operationPending);
    const QJsonObject correspondence = m_currentComment.isValid()
        ? m_currentComment.data(CorrespondenceRole).value<QJsonObject>()
        : QJsonObject();
    const QString correspondenceState = correspondence.value(QStringLiteral("state")).toString();
    const bool exactCorrespondence = correspondenceState == QLatin1String("exact_text") || correspondenceState == QLatin1String("exact_point");
    m_confirmCorrespondenceButton->setEnabled(exactCorrespondence && !m_operationPending);
    m_flagCorrespondenceButton->setEnabled(exactCorrespondence && !m_operationPending);
    m_recomputeCorrespondenceButton->setEnabled(!correspondence.isEmpty() && !m_operationPending);
}

void CommentsOverview::submitCurrentCorrespondenceReview(const QString &action)
{
    if (!m_currentComment.isValid() || !QStringList {QStringLiteral("confirm"), QStringLiteral("flag"), QStringLiteral("recompute")}.contains(action)) {
        return;
    }
    const QJsonObject correspondence = m_currentComment.data(CorrespondenceRole).value<QJsonObject>();
    const QString reviewKey = correspondence.value(QStringLiteral("review_key")).toString();
    const QString sourceBundleSha256 = correspondence.value(QStringLiteral("source_bundle_sha256")).toString();
    if (reviewKey.isEmpty() || sourceBundleSha256.size() != 64) {
        return;
    }
    QString pdfSha256;
    const QString filename = QFileInfo(m_currentPdfPath).fileName();
    for (const QJsonValue &value : correspondence.value(QStringLiteral("locations")).toArray()) {
        const QJsonObject location = value.toObject();
        if (location.value(QStringLiteral("pdf_filename")).toString().compare(filename, Qt::CaseInsensitive) == 0) {
            pdfSha256 = location.value(QStringLiteral("pdf_sha256")).toString();
            break;
        }
    }
    const QString projectionKey = QStringLiteral("review_correspondence_review\nreview-correspondence\n") + reviewKey;
    const QJsonObject previous = m_fedoraProjections.value(projectionKey).toObject();
    const qint64 revision = qMax<qint64>(1, previous.value(QStringLiteral("revision")).toInteger() + 1);
    const QJsonObject payload {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("review_key"), reviewKey},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("origin_id"), QStringLiteral("thoughts-desktop")},
        {QStringLiteral("action"), action},
        {QStringLiteral("source_bundle_sha256"), sourceBundleSha256},
        {QStringLiteral("pdf_sha256"), pdfSha256},
        {QStringLiteral("note"), QString()},
        {QStringLiteral("deleted"), false},
    };
    Q_EMIT reviewCorrespondenceReviewRequested(payload.toVariantMap());
}

void CommentsOverview::confirmCurrentCorrespondence()
{
    submitCurrentCorrespondenceReview(QStringLiteral("confirm"));
}

void CommentsOverview::flagCurrentCorrespondence()
{
    submitCurrentCorrespondenceReview(QStringLiteral("flag"));
}

void CommentsOverview::recomputeCurrentCorrespondence()
{
    submitCurrentCorrespondenceReview(QStringLiteral("recompute"));
}

bool CommentsOverview::currentSourceTarget(QString *sourcePath, int *line, QString *unavailableReason) const
{
    const auto unavailable = [unavailableReason](const QString &reason) {
        if (unavailableReason) {
            *unavailableReason = reason;
        }
        return false;
    };

    const bool singleSelection = m_table && m_table->selectionModel() && m_table->selectionModel()->selectedRows().size() == 1;
    if (!singleSelection || !m_currentComment.isValid()) {
        return unavailable(i18n("Select one comment to open its source."));
    }
    const bool proposal = m_currentComment.data(ItemTypeRole).toString() == QLatin1String("agent_remove");
    const QString anchorStatus = m_currentComment.data(AnchorStatusRole).toString();
    if (!proposal && anchorStatus != QLatin1String("anchored") && anchorStatus != QLatin1String("source_linked") && anchorStatus != QLatin1String("comment_missing")) {
        return unavailable(i18n("This comment does not have a trustworthy source anchor."));
    }

    const QString storedSource = m_currentComment.data(SourceFileRole).toString().trimmed();
    const int storedLine = m_currentComment.data(SourceLineRole).toInt();
    if (storedSource.isEmpty() || storedLine <= 0) {
        return unavailable(i18n("This comment has no source file and line."));
    }

    const QFileInfo storedInfo(storedSource);
    const QString candidatePath = QDir::cleanPath(storedInfo.isAbsolute() ? storedSource : QDir(m_docsRoot).absoluteFilePath(storedSource));
    const QFileInfo candidateInfo(candidatePath);
    if (!candidateInfo.isFile() || candidateInfo.suffix().compare(QLatin1String("tex"), Qt::CaseInsensitive) != 0) {
        return unavailable(i18n("The recorded TeX source file is missing."));
    }

    QString canonicalRoot = QFileInfo(m_docsRoot).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        canonicalRoot = QDir(m_docsRoot).absolutePath();
    }
    canonicalRoot = QDir::cleanPath(canonicalRoot);
    const QString canonicalSource = candidateInfo.canonicalFilePath();
    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    if (canonicalSource.isEmpty() || (canonicalSource != canonicalRoot && !canonicalSource.startsWith(rootPrefix))) {
        return unavailable(i18n("The recorded TeX source is outside the documentation folder."));
    }

    if (sourcePath) {
        *sourcePath = canonicalSource;
    }
    if (line) {
        *line = storedLine;
    }
    if (unavailableReason) {
        unavailableReason->clear();
    }
    return true;
}

QByteArray fileEventSignature(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return QByteArrayLiteral("missing");
    }
    return QByteArray::number(info.size()) + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
}

QVariantMap CommentsOverview::currentMetadataPatch(QString *error) const
{
    QVariantMap patch;
    if (!m_currentComment.isValid()) {
        return patch;
    }
    if (m_currentComment.data(LogicalPayloadRole).value<QJsonObject>().value(QStringLiteral("subtype")).toString() == QLatin1String("Note")) {
        const QString date = m_noteDueDate->text().trimmed();
        if (!date.isEmpty() && (!QDate::fromString(date, Qt::ISODate).isValid() || QDate::fromString(date, Qt::ISODate).toString(Qt::ISODate) != date)) {
            if (error) *error = i18n("Enter a due date as YYYY-MM-DD.");
            return patch;
        }
        if (date != m_originalDueDate) patch.insert(QStringLiteral("calendar_date"), date);
        const QString visibleFrom = m_noteVisibleFrom->text().trimmed();
        if (!visibleFrom.isEmpty() && (!QDate::fromString(visibleFrom, Qt::ISODate).isValid()
                || QDate::fromString(visibleFrom, Qt::ISODate).toString(Qt::ISODate) != visibleFrom)) {
            if (error) *error = i18n("Enter a hide-until date as YYYY-MM-DD.");
            return patch;
        }
        if (visibleFrom != m_originalVisibleFrom) patch.insert(QStringLiteral("visible_from"), visibleFrom);
    }
    if (!m_locationTouched) {
        return patch;
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
        return patch;
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

void CommentsOverview::updateEditorDirtyState()
{
    QString error;
    const QVariantMap metadataPatch = currentMetadataPatch(&error);
    m_editorDirty = m_commentEditor->toPlainText() != m_originalContents || !metadataPatch.isEmpty() || !error.isEmpty();
    if (m_autoSaveTimer) {
        if (m_editorDirty && error.isEmpty() && !m_operationPending) {
            m_autoSaveTimer->start();
        } else {
            m_autoSaveTimer->stop();
        }
    }
    updateEditorActions();
}

CommentsOverview::CommentState CommentsOverview::currentEditorState() const
{
    CommentState state;
    state.dueDate = m_noteDueDate->text().trimmed();
    state.visibleFrom = m_noteVisibleFrom->text().trimmed();
    if (!state.dueDate.isEmpty()) {
        const auto payload = m_currentComment.data(LogicalPayloadRole).value<QJsonObject>();
        state.dueTime = payload.value(QStringLiteral("calendar_time")).toString();
        state.dueZone = payload.value(QStringLiteral("calendar_zone_id")).toString();
    }
    state.contents = m_commentEditor->toPlainText();
    state.priority = m_originalPriority;
    state.locationPresent = m_locationEnabled->isChecked();
    if (state.locationPresent) {
        state.location = m_originalLocationPresent ? m_originalLocation : QJsonObject();
        state.location.insert(QStringLiteral("label"), m_locationLabel->text().trimmed());
        state.location.insert(QStringLiteral("latitude"), m_locationLatitude->value());
        state.location.insert(QStringLiteral("longitude"), m_locationLongitude->value());
        if (!state.location.contains(QStringLiteral("source"))) {
            state.location.insert(QStringLiteral("source"), QStringLiteral("manual"));
        }
    }
    return state;
}

QJsonObject CommentsOverview::logicalMutationPayload(QJsonObject payload, const CommentState &state, bool deleted) const
{
    if (payload.value(QStringLiteral("schema_version")).toInt() != 1
        || payload.value(QStringLiteral("comment_id")).toString().isEmpty()) {
        return {};
    }
    // Undo/Redo carries historical content, but must extend the current revision.
    const auto latest = m_fedoraProjections.value(QStringLiteral("logical_comment\ncomments\n") + payload.value(QStringLiteral("comment_id")).toString()).toObject();
    if (latest.value(QStringLiteral("revision")).toInteger() > payload.value(QStringLiteral("revision")).toInteger()) payload = latest;
    payload.insert(QStringLiteral("revision"), payload.value(QStringLiteral("revision")).toInteger() + 1);
    payload.insert(QStringLiteral("updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    payload.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
    payload.insert(QStringLiteral("deleted"), deleted);
    payload.insert(QStringLiteral("contents"), state.contents);
    if (payload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note")) {
        payload.insert(QStringLiteral("project"), QJsonValue(QJsonValue::Null));
        payload.remove(QStringLiteral("project_name"));
        payload.insert(QStringLiteral("calendar_date"), state.dueDate);
        payload.insert(QStringLiteral("calendar_time"), state.dueTime);
        payload.insert(QStringLiteral("calendar_zone_id"), state.dueZone);
        payload.insert(QStringLiteral("visible_from"), state.visibleFrom);
    }
    payload.insert(QStringLiteral("priority"), state.priority);
    payload.insert(QStringLiteral("location"), state.locationPresent ? QJsonValue(state.location) : QJsonValue(QJsonValue::Null));
    payload.remove(QStringLiteral("locations"));
    payload.remove(QStringLiteral("source_file"));
    payload.remove(QStringLiteral("source_line"));
    payload.remove(QStringLiteral("source_sha256"));
    return payload;
}

QJsonObject CommentsOverview::logicalReplyPayload(const QJsonObject &parentPayload, const QString &contents) const
{
    const QString parentId = parentPayload.value(QStringLiteral("comment_id")).toString();
    const QString threadRootId = parentPayload.value(QStringLiteral("thread_root_id")).toString(parentId);
    const QJsonObject origin = parentPayload.value(QStringLiteral("origin")).toObject();
    QJsonObject scope = parentPayload.value(QStringLiteral("scope")).toObject();
    if (parentPayload.value(QStringLiteral("schema_version")).toInt() != 1 || parentId.isEmpty() || threadRootId.isEmpty() || origin.isEmpty() || scope.isEmpty() || contents.trimmed().isEmpty()) {
        return {};
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString replyId = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const KConfigGroup identity(KSharedConfig::openConfig(), QStringLiteral("Identity"));
    const QString author = identity.readEntry(QStringLiteral("IdentityAuthor"), qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME")));
    scope.insert(QStringLiteral("reason"), QStringLiteral("inherits_thread_locations"));
    return {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("comment_id"), replyId},
        {QStringLiteral("thread_root_id"), threadRootId},
        {QStringLiteral("parent_id"), parentId},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("created_at"), now},
        {QStringLiteral("updated_at"), now},
        {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()},
        {QStringLiteral("deleted"), false},
        {QStringLiteral("contents"), contents.trimmed()},
        {QStringLiteral("author"), author},
        {QStringLiteral("subtype"), QStringLiteral("Text")},
        {QStringLiteral("selected_text"), QString()},
        {QStringLiteral("color"), parentPayload.value(QStringLiteral("color"))},
        {QStringLiteral("priority"), QStringLiteral("normal")},
        {QStringLiteral("location"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("chatgpt_url"), QString()},
        {QStringLiteral("origin"), origin},
        {QStringLiteral("scope"), scope},
    };
}

void CommentsOverview::applyCommentState(const QString &pdfPath, const QString &annotationId, const CommentState &state, bool loadEditor)
{
    selectComment(pdfPath, annotationId);
    if (!m_currentComment.isValid() || m_currentPdfPath != pdfPath || m_currentAnnotationId != annotationId) {
        return;
    }
    m_changingSelection = true;
    m_commentEditor->setPlainText(state.contents);
    m_originalPriority = state.priority;
    m_noteDueDate->setText(state.dueDate);
    m_noteVisibleFrom->setText(state.visibleFrom);
    m_locationEnabled->setChecked(state.locationPresent);
    m_locationLabel->setText(state.location.value(QStringLiteral("label")).toString());
    m_locationLatitude->setValue(state.location.value(QStringLiteral("latitude")).toDouble());
    m_locationLongitude->setValue(state.location.value(QStringLiteral("longitude")).toDouble());
    m_locationTouched = true;
    m_changingSelection = false;
    applyEditorToCurrentRow();
    m_editorDirty = false;
    if (!loadEditor) {
        m_commentEditor->clearFocus();
    }
}

void CommentsOverview::applyEditorToCurrentRow()
{
    if (!m_currentComment.isValid()) {
        return;
    }
    const QString pdfPath = m_currentPdfPath;
    const QString annotationId = m_currentAnnotationId;
    const QString contents = m_commentEditor->toPlainText();
    QJsonObject location;
    const bool locationPresent = m_locationEnabled->isChecked();
    if (locationPresent) {
        location = m_originalLocationPresent ? m_originalLocation : QJsonObject();
        location.insert(QStringLiteral("label"), m_locationLabel->text().trimmed());
        location.insert(QStringLiteral("latitude"), m_locationLatitude->value());
        location.insert(QStringLiteral("longitude"), m_locationLongitude->value());
        if (!location.contains(QStringLiteral("source"))) {
            location.insert(QStringLiteral("source"), QStringLiteral("manual"));
        }
    }

    QString displayContents = contents.trimmed();
    if (displayContents.isEmpty()) {
        displayContents = i18nc("@item:inlistbox annotation without comment text", "(No comment text)");
    }
    const QString locationDisplay = locationPresent ? location.value(QStringLiteral("label")).toString() : QStringLiteral("—");
    const QString coordinates = locationPresent ? QStringLiteral("%1, %2").arg(location.value(QStringLiteral("latitude")).toDouble(), 0, 'f', 6).arg(location.value(QStringLiteral("longitude")).toDouble(), 0, 'f', 6) : QString();
    const QString searchText = QStringList {contents,
                                            m_currentComment.data(SelectedTextRole).toString(),
                                            m_currentComment.data(DocumentRole).toString(),
                                            m_currentComment.data(AuthorRole).toString(),
                                            m_currentComment.data(SourceDisplayRole).toString(),
                                            m_currentComment.data(StatusDisplayRole).toString(),
                                            locationDisplay,
                                            coordinates}
                                   .join(QLatin1Char('\n'));

    const bool wasChangingSelection = m_changingSelection;
    m_changingSelection = true;
    m_proxy->setDynamicSortFilter(false);
    for (int column = 0; column < ColumnCount; ++column) {
        const QModelIndex source = m_proxy->mapToSource(QModelIndex(m_currentComment).siblingAtColumn(column));
        QStandardItem *item = source.isValid() ? m_model->itemFromIndex(source) : nullptr;
        if (!item) {
            continue;
        }
        item->setData(contents, ContentsRole);
        item->setData(searchText, SearchRole);
        item->setData(locationPresent, LocationPresentRole);
        item->setData(location.value(QStringLiteral("label")).toString(), LocationLabelRole);
        item->setData(locationPresent ? location.value(QStringLiteral("latitude")).toDouble() : 0.0, LatitudeRole);
        item->setData(locationPresent ? location.value(QStringLiteral("longitude")).toDouble() : 0.0, LongitudeRole);
        item->setData(QVariant::fromValue(location), LocationObjectRole);
        item->setToolTip(contents);
        if (column == CommentColumn) {
            item->setText(displayContents);
            item->setData(displayContents, SortRole);
        }
    }
    m_proxy->setDynamicSortFilter(true);
    m_changingSelection = wasChangingSelection;
    for (TrackedCommentDocument &document : m_documents) {
        if (document.pdfPath != pdfPath) {
            continue;
        }
        for (TrackedComment &comment : document.comments) {
            if (comment.id != annotationId) {
                continue;
            }
            comment.contents = contents;
            comment.locationPresent = locationPresent;
            comment.locationValid = locationPresent;
            comment.location = location;
            comment.latitude = location.value(QStringLiteral("latitude")).toDouble();
            comment.longitude = location.value(QStringLiteral("longitude")).toDouble();
            break;
        }
        break;
    }

    m_originalDueDate = m_noteDueDate->text().trimmed();
    m_originalVisibleFrom = m_noteVisibleFrom->text().trimmed();
    m_originalContents = contents;
    m_originalLocationPresent = locationPresent;
    m_originalLocation = location;
    m_locationTouched = false;
}

void CommentsOverview::applyLogicalPayloadToCurrentRow(const QJsonObject &payload)
{
    if (!m_currentComment.isValid() || payload.isEmpty()) {
        return;
    }
    ++m_commentGeneration;
    m_fedoraProjections.insert(QStringLiteral("logical_comment\ncomments\n") + payload.value(QStringLiteral("comment_id")).toString(), payload);
    for (int column = 0; column < ColumnCount; ++column) {
        const QModelIndex source = m_proxy->mapToSource(QModelIndex(m_currentComment).siblingAtColumn(column));
        if (QStandardItem *item = source.isValid() ? m_model->itemFromIndex(source) : nullptr) {
            item->setData(QVariant::fromValue(payload), LogicalPayloadRole);
        }
    }
    for (TrackedCommentDocument &document : m_documents) {
        for (TrackedComment &comment : document.comments) {
            if (comment.logicalComment && comment.id == m_currentAnnotationId) {
                comment.logicalPayload = payload;
                return;
            }
        }
    }
}

void CommentsOverview::applyChatGptUrlToCurrentRow(const QString &url)
{
    if (!m_currentComment.isValid()) {
        return;
    }
    const QString pdfPath = m_currentPdfPath;
    const QString annotationId = m_currentAnnotationId;
    for (int column = 0; column < ColumnCount; ++column) {
        const QModelIndex source = m_proxy->mapToSource(QModelIndex(m_currentComment).siblingAtColumn(column));
        if (QStandardItem *item = source.isValid() ? m_model->itemFromIndex(source) : nullptr) {
            item->setData(url, ChatGptUrlRole);
        }
    }
    for (TrackedCommentDocument &document : m_documents) {
        if (document.pdfPath != pdfPath) {
            continue;
        }
        for (TrackedComment &comment : document.comments) {
            if (comment.id == annotationId) {
                comment.chatGptUrl = url;
                return;
            }
        }
    }
}

bool CommentsOverview::saveChatGptUrl(bool openAfterSave)
{
    if (!m_currentComment.isValid() || m_currentPdfPath.isEmpty() || m_currentAnnotationId.isEmpty()) {
        return false;
    }
    QString error;
    const QString normalized = Okular::DocsCommentSynchronizer::normalizedChatGptUrl(m_chatGptUrlEditor->text(), &error);
    if (!error.isEmpty()) {
        m_summary->setText(i18n("Enter a valid HTTPS chatgpt.com link."));
        m_chatGptUrlEditor->setFocus(Qt::OtherFocusReason);
        m_chatGptUrlEditor->selectAll();
        updateEditorActions();
        return false;
    }
    m_changingSelection = true;
    m_chatGptUrlEditor->setText(normalized);
    m_changingSelection = false;
    m_chatGptUrlDirty = normalized != m_originalChatGptUrl;
    if (!m_chatGptUrlDirty) {
        updateEditorActions();
        if (openAfterSave && !normalized.isEmpty()) {
            if (!QDesktopServices::openUrl(QUrl(normalized))) {
                KMessageBox::error(this, i18n("The ChatGPT link could not be opened."), i18n("Open ChatGPT"));
            }
        }
        return true;
    }
    const QString pdfPath = m_currentPdfPath;
    const QString annotationId = m_currentAnnotationId;
    m_pendingChatGptUrl = normalized;
    m_openChatGptAfterSave = openAfterSave;
    m_chatGptUrlDirty = false;
    applyChatGptUrlToCurrentRow(normalized);
    if (m_currentComment.data(LogicalCommentRole).toBool()) {
        QJsonObject payload = logicalMutationPayload(m_currentComment.data(LogicalPayloadRole).value<QJsonObject>(), currentEditorState(), false);
        payload.insert(QStringLiteral("chatgpt_url"), normalized);
        applyLogicalPayloadToCurrentRow(payload);
        Q_EMIT logicalCommentUrlMutationRequested(payload.toVariantMap(), pdfPath, annotationId);
    } else {
        Q_EMIT commentChatGptUrlUpdateRequested(pdfPath, annotationId, normalized);
    }
    updateEditorActions();
    return true;
}

void CommentsOverview::openChatGptUrl()
{
    if (m_chatGptUrlDirty) {
        saveChatGptUrl(true);
        return;
    }
    QString error;
    const QString normalized = Okular::DocsCommentSynchronizer::normalizedChatGptUrl(m_originalChatGptUrl, &error);
    if (normalized.isEmpty() || !error.isEmpty()) {
        return;
    }
    if (!QDesktopServices::openUrl(QUrl(normalized))) {
        KMessageBox::error(this, i18n("The ChatGPT link could not be opened."), i18n("Open ChatGPT"));
    }
}

void CommentsOverview::enqueueMutation(PendingMutation mutation)
{
    const QString key = QString::number(static_cast<int>(mutation.kind)) + QChar(0x1f) + mutation.pdfPath + QChar(0x1f) + mutation.annotationId;
    const auto existing = m_queuedMutations.constFind(key);
    if (existing == m_queuedMutations.cend()) {
        m_mutationOrder.append(key);
    } else if (mutation.kind == PendingMutation::Comment) {
        QVariantMap merged = existing->metadata;
        for (auto iterator = mutation.metadata.cbegin(); iterator != mutation.metadata.cend(); ++iterator) {
            merged.insert(iterator.key(), iterator.value());
        }
        mutation.metadata = merged;
    }
    m_queuedMutations.insert(key, std::move(mutation));
    startNextMutation();
}

void CommentsOverview::startNextMutation()
{
    if (m_shuttingDown || m_mutationRunning || m_mutationOrder.isEmpty()) {
        return;
    }
    const QString key = m_mutationOrder.takeFirst();
    const auto found = m_queuedMutations.find(key);
    if (found == m_queuedMutations.end()) {
        startNextMutation();
        return;
    }
    const PendingMutation mutation = found.value();
    m_queuedMutations.erase(found);
    m_runningMutation = mutation;
    m_mutationRunning = true;

    const QString docsRoot = m_docsRoot;
    QPointer<CommentsOverview> guard(this);
    m_persistencePool->start([guard, docsRoot, mutation] {
        Okular::DocsCommentSynchronizer::CommentMetadataUpdateResult update;
        if (mutation.kind == PendingMutation::ChatGptUrl) {
            update = Okular::DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(docsRoot, QFileInfo(mutation.pdfPath).completeBaseName(), mutation.annotationId, mutation.chatGptUrl);
        } else {
            update = Okular::DocsCommentSynchronizer::updateSidecarComment(docsRoot, QFileInfo(mutation.pdfPath).completeBaseName(), mutation.annotationId, mutation.contents, QJsonObject::fromVariantMap(mutation.metadata));
        }
        const QString sidecarPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(QFileInfo(mutation.pdfPath).completeBaseName()));
        const QVariantMap result {
            {QStringLiteral("success"), update.success},
            {QStringLiteral("changed"), update.changed},
            {QStringLiteral("error"), update.failureReason},
            {QStringLiteral("sidecarPath"), sidecarPath},
            {QStringLiteral("sidecarSha256"), update.success ? fileContentSha256(sidecarPath) : QByteArray()},
            {QStringLiteral("totalMs"), update.totalMs},
        };
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, mutation, result] {
                if (guard) {
                    guard->finishMutation(mutation, result);
                }
            },
            Qt::QueuedConnection);
    });
}

void CommentsOverview::finishMutation(const PendingMutation &mutation, const QVariantMap &result)
{
    m_mutationRunning = false;
    m_runningMutation.reset();
    const QString sidecarPath = result.value(QStringLiteral("sidecarPath")).toString();
    if (result.value(QStringLiteral("success")).toBool()) {
        const QByteArray hash = result.value(QStringLiteral("sidecarSha256")).toByteArray();
        if (!hash.isEmpty()) {
            m_ignoredLocalHashes.insert(QFileInfo(sidecarPath).absoluteFilePath(), hash);
        }
        if (m_summary->text().startsWith(i18n("Not saved yet"))) {
            updateCommentSummary();
        }
        startNextMutation();
        return;
    }

    const QString key = QString::number(static_cast<int>(mutation.kind)) + QChar(0x1f) + mutation.pdfPath + QChar(0x1f) + mutation.annotationId;
    const auto newer = m_queuedMutations.find(key);
    if (newer == m_queuedMutations.end()) {
        m_queuedMutations.insert(key, mutation);
        m_mutationOrder.append(key);
    } else if (mutation.kind == PendingMutation::Comment) {
        QVariantMap merged = mutation.metadata;
        for (auto iterator = newer->metadata.cbegin(); iterator != newer->metadata.cend(); ++iterator) {
            merged.insert(iterator.key(), iterator.value());
        }
        newer->metadata = merged;
    }
    m_summary->setText(i18n("Not saved yet — retrying in the background: %1", result.value(QStringLiteral("error")).toString()));
    m_persistenceRetryTimer->start();
}

bool CommentsOverview::hasPendingMutationForPath(const QString &path) const
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    const auto matches = [this, &absolutePath](const PendingMutation &mutation) {
        const QString candidate = QDir(m_docsRoot).absoluteFilePath(QStringLiteral("comments/%1_comments.jsonl").arg(QFileInfo(mutation.pdfPath).completeBaseName()));
        return QFileInfo(candidate).absoluteFilePath() == absolutePath;
    };
    if (m_runningMutation && matches(*m_runningMutation)) {
        return true;
    }
    return std::any_of(m_queuedMutations.cbegin(), m_queuedMutations.cend(), matches);
}

bool CommentsOverview::shouldIgnoreLocalWrite(const QString &path)
{
    if (hasPendingMutationForPath(path)) {
        return true;
    }
    const auto expected = m_ignoredLocalHashes.constFind(QFileInfo(path).absoluteFilePath());
    if (expected == m_ignoredLocalHashes.cend()) {
        return false;
    }
    if (fileContentSha256(path) == expected.value()) {
        m_ignoredLocalHashes.erase(expected);
        return true;
    }
    m_ignoredLocalHashes.erase(expected);
    return false;
}

void CommentsOverview::flushQueuedMutations()
{
    for (const QString &key : std::as_const(m_mutationOrder)) {
        const auto found = m_queuedMutations.constFind(key);
        if (found == m_queuedMutations.cend()) {
            continue;
        }
        const PendingMutation &mutation = found.value();
        if (mutation.kind == PendingMutation::ChatGptUrl) {
            Okular::DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(m_docsRoot, QFileInfo(mutation.pdfPath).completeBaseName(), mutation.annotationId, mutation.chatGptUrl);
        } else {
            Okular::DocsCommentSynchronizer::updateSidecarComment(m_docsRoot, QFileInfo(mutation.pdfPath).completeBaseName(), mutation.annotationId, mutation.contents, QJsonObject::fromVariantMap(mutation.metadata));
        }
    }
    m_mutationOrder.clear();
    m_queuedMutations.clear();
}

void CommentsOverview::removeTargetsFromModel(const QList<TrackedCommentTarget> &targets)
{
    QHash<QString, QSet<QString>> idsByPdf;
    for (const TrackedCommentTarget &target : targets) {
        idsByPdf[target.pdfPath].insert(target.annotationId);
    }

    // Removing a reply or a root with replies can change thread ownership and
    // reply ordering. Keep the full reconciliation for those cases.
    bool changesThreads = false;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        const auto ids = idsByPdf.constFind(document.pdfPath);
        if (ids == idsByPdf.cend()) {
            continue;
        }
        QSet<QString> affectedRoots;
        for (const TrackedComment &comment : document.comments) {
            if (ids->contains(comment.id)) { affectedRoots.insert(comment.threadRootId); }
        }
        for (const TrackedComment &comment : document.comments) {
            if (!ids->contains(comment.id) && affectedRoots.contains(comment.threadRootId)) {
                changesThreads = true;
                break;
            }
        }
    }

    QHash<QString, QSet<QString>> removedByPdf;
    QHash<QString, QHash<QString, int>> removedColumnsByPdf;
    {
        // Row notifications still reach the view, but must not rebuild the
        // Library tree on each removal or filter invalidation.
        const QScopedValueRollback<bool> updatingModel(m_updatingCommentModel, true);
        for (TrackedCommentDocument &document : m_documents) {
            const auto ids = idsByPdf.constFind(document.pdfPath);
            if (ids == idsByPdf.cend()) {
                continue;
            }
            document.comments.removeIf([&](const TrackedComment &comment) {
                if (!ids->contains(comment.id)) {
                    return false;
                }
                removedByPdf[document.pdfPath].insert(comment.id);
                if (visibleThread(comment, m_queuedAgentProposalIds) && threadCountColumn(comment) > 0) {
                    removedColumnsByPdf[document.pdfPath].insert(comment.id, threadCountColumn(comment));
                }
                return true;
            });
            if (changesThreads) {
                rebuildTrackedCommentThreads(document);
            }
        }
        if (changesThreads) {
            populateComments();
            populateDocumentList(selectedDocument());
            applyFilters();
        } else {
            for (int row = m_model->rowCount() - 1; row >= 0; --row) {
                const QModelIndex index = m_model->index(row, CommentColumn);
                const auto removed = removedByPdf.constFind(index.data(PdfPathRole).toString());
                if (removed != removedByPdf.cend() && removed->contains(index.data(AnnotationIdRole).toString())) {
                    m_model->removeRow(row);
                }
            }
            // Document membership did not change. Update the existing counts
            // and component filters without rescanning sources or losing tree state.
            for (BrowserState *browser : {&m_commentsBrowser, &m_notesBrowser}) {
                const QSignalBlocker blocker(browser->documentList);
                for (QTreeWidgetItemIterator it(browser->documentList); *it; ++it) {
                    QTreeWidgetItem *item = *it;
                    const int kind = item->data(0, DocumentItemKindRole).toInt();
                    QStringList threadIds = item->data(0, DocumentThreadIdsRole).toStringList();
                    const bool threadGroup = kind == ComponentListEntry || kind == ThreadGroupListEntry;
                    const int scope = item->data(0, DocumentScopeRole).toInt();
                    for (auto pdf = removedColumnsByPdf.cbegin(); pdf != removedColumnsByPdf.cend(); ++pdf) {
                        if ((scope == 1 && !pdf.key().isEmpty()) || (scope == 2 && pdf.key().isEmpty())) { continue; }
                        if ((kind == DocumentListEntry || kind == ComponentListEntry) && item->data(0, DocumentPathRole).toString() != pdf.key()) { continue; }
                        if (kind != AllCommentsItem && kind != DocumentListEntry && !threadGroup) { continue; }
                        for (auto removed = pdf->cbegin(); removed != pdf->cend(); ++removed) {
                            if (threadGroup && scope != 2 && !threadIds.contains(removed.key())) { continue; }
                            const int column = removed.value();
                            const int count = qMax(0, item->text(column).toInt() - 1);
                            item->setText(column, QString::number(count));
                            item->setToolTip(column, threadCountTooltip(column, count));
                        }
                        if (threadGroup) {
                            threadIds.removeIf([&](const QString &id) { return pdf->contains(id); });
                        }
                    }
                    if (threadGroup) { item->setData(0, DocumentThreadIdsRole, threadIds); }
                }
                const auto *current = browser->documentList->currentItem();
                if (current && (current->data(0, DocumentItemKindRole).toInt() == ComponentListEntry || current->data(0, DocumentItemKindRole).toInt() == ThreadGroupListEntry)) {
                    static_cast<CommentsFilterProxyModel *>(browser->proxy)->setThreadIds(current->data(0, DocumentScopeRole).toInt() != 2, current->data(0, DocumentThreadIdsRole).toStringList());
                }
            }

        }
    }
    updateCommentSummary();
    populateDiary();
}

void CommentsOverview::saveSelectedComment()
{
    if (!m_currentComment.isValid() || !m_editorDirty) {
        return;
    }
    QString error;
    const QVariantMap metadataPatch = currentMetadataPatch(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, i18n("Invalid Comment Metadata"), error);
        return;
    }
    CommentState previousState;
    previousState.dueDate = m_originalDueDate;
    previousState.visibleFrom = m_originalVisibleFrom;
    const auto originalPayload = m_currentComment.data(LogicalPayloadRole).value<QJsonObject>();
    previousState.dueTime = originalPayload.value(QStringLiteral("calendar_time")).toString();
    previousState.dueZone = originalPayload.value(QStringLiteral("calendar_zone_id")).toString();
    previousState.contents = m_originalContents;
    previousState.priority = m_originalPriority;
    previousState.locationPresent = m_originalLocationPresent;
    previousState.location = m_originalLocation;
    const CommentState currentState = currentEditorState();
    const QString pdfPath = m_currentPdfPath;
    const QString annotationId = m_currentAnnotationId;
    const int page = m_currentComment.data(PageRole).toInt();
    const int fallbackRow = m_currentComment.row();
    const bool logicalComment = m_currentComment.data(LogicalCommentRole).toBool();
    const QJsonObject previousLogicalPayload = m_currentComment.data(LogicalPayloadRole).value<QJsonObject>();
    const bool editorHadFocus = m_commentEditor->hasFocus();
    const QTextCursor editorCursorBeforeApply = m_commentEditor->textCursor();
    const int editorVerticalScrollBeforeApply = m_commentEditor->verticalScrollBar()->value();
    const int editorHorizontalScrollBeforeApply = m_commentEditor->horizontalScrollBar()->value();

    applyEditorToCurrentRow();
    const int textLength = m_commentEditor->toPlainText().size();
    QTextCursor restoredCursor = m_commentEditor->textCursor();
    restoredCursor.setPosition(qBound(0, editorCursorBeforeApply.anchor(), textLength));
    restoredCursor.setPosition(qBound(0, editorCursorBeforeApply.position(), textLength), QTextCursor::KeepAnchor);
    m_commentEditor->setTextCursor(restoredCursor);
    m_commentEditor->verticalScrollBar()->setValue(qBound(m_commentEditor->verticalScrollBar()->minimum(), editorVerticalScrollBeforeApply, m_commentEditor->verticalScrollBar()->maximum()));
    m_commentEditor->horizontalScrollBar()->setValue(qBound(m_commentEditor->horizontalScrollBar()->minimum(), editorHorizontalScrollBeforeApply, m_commentEditor->horizontalScrollBar()->maximum()));
    if (editorHadFocus && !m_commentEditor->hasFocus()) {
        m_commentEditor->setFocus(Qt::OtherFocusReason);
    }
    QTimer::singleShot(0, this, [this, pdfPath, annotationId, currentState, editorCursorBeforeApply, editorVerticalScrollBeforeApply, editorHorizontalScrollBeforeApply] {
        if (m_currentPdfPath != pdfPath || m_currentAnnotationId != annotationId || m_commentEditor->toPlainText() != currentState.contents) {
            return;
        }
        const QTextCursor currentCursor = m_commentEditor->textCursor();
        if (currentCursor.position() != editorCursorBeforeApply.position() || currentCursor.anchor() != editorCursorBeforeApply.anchor()) {
            return;
        }
        m_commentEditor->verticalScrollBar()->setValue(qBound(m_commentEditor->verticalScrollBar()->minimum(), editorVerticalScrollBeforeApply, m_commentEditor->verticalScrollBar()->maximum()));
        m_commentEditor->horizontalScrollBar()->setValue(qBound(m_commentEditor->horizontalScrollBar()->minimum(), editorHorizontalScrollBeforeApply, m_commentEditor->horizontalScrollBar()->maximum()));
    });
    m_editorDirty = false;
    const bool hadDeferredReload = m_reloadDeferred;
    m_reloadDeferred = false;

    HistoryEntry historyEntry;
    historyEntry.pdfPath = pdfPath;
    historyEntry.annotationId = annotationId;
    historyEntry.operation = EditOperation;
    historyEntry.page = page;
    historyEntry.fallbackRow = fallbackRow;
    historyEntry.previousState = previousState;
    historyEntry.currentState = currentState;
    if (logicalComment) {
        historyEntry.logicalPayload = logicalMutationPayload(previousLogicalPayload, currentState, false);
        if (historyEntry.logicalPayload.isEmpty()) {
            m_summary->setText(i18n("The logical comment payload is incomplete and was not queued."));
            return;
        }
        applyLogicalPayloadToCurrentRow(historyEntry.logicalPayload);
    }
    m_undoHistory.append(historyEntry);
    m_redoHistory.clear();

    if (logicalComment) {
        Q_EMIT logicalCommentMutationRequested(historyEntry.logicalPayload.toVariantMap(), EditOperation, pdfPath, annotationId);
    } else {
        Q_EMIT commentEditRequested(pdfPath, annotationId, page, currentState.contents, metadataPatch);
    }

    if (!m_pendingSelection.isEmpty()) {
        const QList<TrackedCommentTarget> pendingSelection = m_pendingSelection;
        const QString pendingCurrentPdfPath = m_pendingCurrentPdfPath;
        const QString pendingCurrentAnnotationId = m_pendingCurrentAnnotationId;
        m_pendingSelection.clear();
        m_pendingCurrentPdfPath.clear();
        m_pendingCurrentAnnotationId.clear();
        restoreCommentSelection(pendingSelection, pendingCurrentPdfPath, pendingCurrentAnnotationId);
    }
    if (hadDeferredReload) {
        m_reloadTimer->start();
    }
    updateEditorActions();
}

void CommentsOverview::createStandaloneNote()
{
    if (m_operationPending || !resolveDirtyEditor()) {
        return;
    }
    bool accepted = false;
    const QString contents = QInputDialog::getMultiLineText(this, i18n("Add Note"), i18n("Note:"), QString(), &accepted).trimmed();
    if (!accepted || contents.isEmpty()) {
        return;
    }

    const QJsonObject payload = standaloneNotePayload(contents, QDateTime::currentDateTime());
    const QString id = payload.value(QStringLiteral("comment_id")).toString();
    m_pendingRevealPdfPath.clear();
    m_pendingRevealAnnotationId = id;
    m_pendingRevealFallbackRow = -1;
    setOperationPending(true);
    Q_EMIT logicalCommentMutationRequested(payload.toVariantMap(), CreateOperation, QString(), id);
}

QJsonObject CommentsOverview::standaloneNotePayload(const QString &contents, const QDateTime &createdAt)
{
    const QString now = createdAt.toUTC().toString(Qt::ISODateWithMs);
    const QString id = QStringLiteral("thoughts-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const KConfigGroup identity(KSharedConfig::openConfig(), QStringLiteral("Identity"));
    return QJsonObject {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("comment_id"), id},
        {QStringLiteral("thread_root_id"), id},
        {QStringLiteral("parent_id"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("created_at"), now},
        {QStringLiteral("updated_at"), now},
        {QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId()},
        {QStringLiteral("deleted"), false},
        {QStringLiteral("contents"), contents},
        {QStringLiteral("author"), identity.readEntry(QStringLiteral("IdentityAuthor"), qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME")))},
        {QStringLiteral("subtype"), QStringLiteral("Note")},
        {QStringLiteral("priority"), QStringLiteral("normal")},
        {QStringLiteral("origin"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("project"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}},
    };
}

void CommentsOverview::createDiaryEntry()
{
    const QString contents = m_diaryInput->toPlainText().trimmed();
    if (contents.isEmpty() || m_operationPending || !resolveDirtyEditor()) {
        return;
    }
    // A diary entry is a note created like a to-do, marked as a diary entry
    // and stamped with the local time and zone it was written in.
    const QDateTime now = QDateTime::currentDateTime();
    QJsonObject payload = standaloneNotePayload(contents, now);
    payload.insert(QStringLiteral("entry_kind"), QStringLiteral("diary"));
    payload.insert(QStringLiteral("diary_at"), diaryTimestamp(now));
    payload.insert(QStringLiteral("diary_zone_id"), QString::fromUtf8(QTimeZone::systemTimeZoneId()));
    const QString id = payload.value(QStringLiteral("comment_id")).toString();
    m_pendingDiaryCreateId = id;
    setOperationPending(true);
    Q_EMIT logicalCommentMutationRequested(payload.toVariantMap(), CreateOperation, QString(), id);
}

void CommentsOverview::populateDiary()
{
    if (!m_diaryList) {
        return;
    }
    struct Entry {
        QString id;
        QDateTime at;
        QString contents;
        QString pdfPath;
    };
    QList<Entry> entries;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, CommentColumn);
        if (index.data(ItemTypeRole).toString() != QLatin1String("note")) continue;
        const QJsonObject payload = index.data(LogicalPayloadRole).value<QJsonObject>();
        if (!diaryEntry(payload)) continue;
        entries.append({index.data(AnnotationIdRole).toString(), diaryEntryTime(payload), index.data(ContentsRole).toString(), index.data(PdfPathRole).toString()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        if (left.at != right.at) return left.at > right.at;
        return left.id > right.id;
    });

    const QTreeWidgetItem *current = m_diaryList->currentItem();
    const QString currentId = current ? current->data(0, DiaryIdRole).toString() : QString();
    QSet<QString> selectedIds;
    for (const QTreeWidgetItem *item : m_diaryList->selectedItems()) selectedIds.insert(item->data(0, DiaryIdRole).toString());
    const int scroll = m_diaryList->verticalScrollBar()->value();
    const QSignalBlocker blocker(m_diaryList);
    m_diaryList->clear();
    QTreeWidgetItem *day = nullptr;
    QDate dayDate;
    QTreeWidgetItem *restoredCurrent = nullptr;
    for (const Entry &entry : std::as_const(entries)) {
        if (!day || entry.at.date() != dayDate) {
            dayDate = entry.at.date();
            day = new QTreeWidgetItem(m_diaryList, {diaryDayTitle(dayDate)});
            day->setFlags(Qt::ItemIsEnabled);
            QFont font = day->font(0);
            font.setBold(true);
            day->setFont(0, font);
            day->setFirstColumnSpanned(true);
            day->setExpanded(true);
        }
        auto *item = new QTreeWidgetItem(day, {entry.at.time().toString(QStringLiteral("HH:mm")), entry.contents});
        item->setData(0, DiaryIdRole, entry.id);
        item->setData(0, DiaryContentsRole, entry.contents);
        item->setData(0, DiaryPdfPathRole, entry.pdfPath);
        item->setTextAlignment(0, Qt::AlignTop | Qt::AlignLeft);
        item->setToolTip(0, QLocale().toString(entry.at, QLocale::LongFormat));
        if (entry.id == currentId) restoredCurrent = item;
        if (selectedIds.contains(entry.id)) item->setSelected(true);
    }
    if (restoredCurrent) m_diaryList->setCurrentItem(restoredCurrent, 0, QItemSelectionModel::NoUpdate);
    m_diaryList->verticalScrollBar()->setValue(scroll);
    m_diarySummary->setText(entries.isEmpty() ? i18n("No diary entries yet.") : i18np("%1 diary entry", "%1 diary entries", entries.size()));
}

void CommentsOverview::editDiaryEntry(const QString &id)
{
    QTreeWidgetItem *item = nullptr;
    for (QTreeWidgetItemIterator it(m_diaryList); *it; ++it) {
        if ((*it)->data(0, DiaryIdRole).toString() == id) item = *it;
    }
    if (!item || id.isEmpty() || m_operationPending || !resolveDirtyEditor()) {
        return;
    }
    const QString previous = item->data(0, DiaryContentsRole).toString();
    bool accepted = false;
    const QString contents = QInputDialog::getMultiLineText(this, i18n("Edit Diary Entry"), i18n("Entry (%1, %2):", item->parent()->text(0), item->text(0)), previous, &accepted).trimmed();
    if (!accepted || contents.isEmpty() || contents == previous) {
        return;
    }
    saveDiaryEntryText(id, contents);
}

bool CommentsOverview::saveDiaryEntryText(const QString &id, const QString &contents)
{
    QModelIndex index;
    for (int row = 0; row < m_model->rowCount() && !index.isValid(); ++row) {
        const QModelIndex candidate = m_model->index(row, CommentColumn);
        if (candidate.data(AnnotationIdRole).toString() == id && diaryEntry(candidate.data(LogicalPayloadRole).value<QJsonObject>())) index = candidate;
    }
    if (!index.isValid()) {
        return false;
    }
    // Only the text changes: diary_at, its zone and every other key stay as they are.
    const QJsonObject previous = index.data(LogicalPayloadRole).value<QJsonObject>();
    CommentState state;
    state.contents = contents;
    state.priority = previous.value(QStringLiteral("priority")).toString(QStringLiteral("normal"));
    state.dueDate = previous.value(QStringLiteral("calendar_date")).toString();
    state.dueTime = previous.value(QStringLiteral("calendar_time")).toString();
    state.dueZone = previous.value(QStringLiteral("calendar_zone_id")).toString();
    state.visibleFrom = previous.value(QStringLiteral("visible_from")).toString();
    state.locationPresent = previous.value(QStringLiteral("location")).isObject();
    state.location = previous.value(QStringLiteral("location")).toObject();
    const QJsonObject payload = logicalMutationPayload(previous, state, false);
    if (payload.isEmpty()) {
        m_diarySummary->setText(i18n("The diary entry is incomplete and was not saved."));
        return false;
    }
    // Shown at once, as a to-do edit is; the durable queue publishes it at Sync.
    ++m_commentGeneration;
    m_fedoraProjections.insert(QStringLiteral("logical_comment\ncomments\n") + id, payload);
    for (TrackedCommentDocument &document : m_documents) {
        for (TrackedComment &comment : document.comments) {
            if (comment.logicalComment && comment.id == id) {
                comment.contents = contents;
                comment.logicalPayload = payload;
            }
        }
    }
    populateComments();
    m_pendingDiaryEditIds.insert(id);
    Q_EMIT logicalCommentMutationRequested(payload.toVariantMap(), EditOperation, index.data(PdfPathRole).toString(), id);
    return true;
}

void CommentsOverview::deleteSelectedDiaryEntries()
{
    QList<TrackedCommentTarget> targets;
    for (QTreeWidgetItemIterator it(m_diaryList, QTreeWidgetItemIterator::Selected); *it; ++it) {
        const QString id = (*it)->data(0, DiaryIdRole).toString();
        if (!id.isEmpty()) targets.append({(*it)->data(0, DiaryPdfPathRole).toString(), id, 0, -1});
    }
    if (targets.isEmpty() || m_operationPending || ((m_editorDirty || m_chatGptUrlDirty) && !resolveDirtyEditor())) return;
    if (targets.size() > 1) {
        QMessageBox confirmation(QMessageBox::Warning, i18n("Delete Diary Entries"), i18np("Delete the selected diary entry?", "Delete the %1 selected diary entries?", targets.size()), QMessageBox::Cancel, this);
        auto *button = confirmation.addButton(i18n("Delete"), QMessageBox::DestructiveRole);
        confirmation.exec();
        if (confirmation.clickedButton() != button) return;
    }
    for (const TrackedCommentTarget &target : std::as_const(targets)) m_pendingDiaryDeleteIds.insert(target.annotationId);
    setOperationPending(true);
    Q_EMIT commentsDeleteRequested(targets);
}

void CommentsOverview::replyToSelectedComment()
{
    const bool singleSelection = m_table && m_table->selectionModel() && m_table->selectionModel()->selectedRows().size() == 1;
    if (!singleSelection || !m_currentComment.isValid() || m_currentAnnotationId.isEmpty() || m_operationPending || !resolveDirtyEditor()) {
        return;
    }
    bool accepted = false;
    const QString contents = QInputDialog::getMultiLineText(this, i18n("Reply to Comment"), i18n("Reply:"), QString(), &accepted).trimmed();
    if (!accepted || contents.isEmpty()) {
        return;
    }

    QVariantMap logicalPayload;
    if (m_currentComment.data(LogicalCommentRole).toBool()) {
        const QJsonObject payload = logicalReplyPayload(m_currentComment.data(LogicalPayloadRole).value<QJsonObject>(), contents);
        if (payload.isEmpty()) {
            m_summary->setText(i18n("The logical comment payload is incomplete and the reply was not queued."));
            return;
        }
        logicalPayload = payload.toVariantMap();
    }
    const QString pdfPath = m_currentPdfPath;
    const QString parentAnnotationId = m_currentAnnotationId;
    const int page = m_currentComment.data(PageRole).toInt();
    setOperationPending(true);
    Q_EMIT commentReplyRequested(pdfPath, parentAnnotationId, page, contents, logicalPayload);
}

QList<QJsonObject> CommentsOverview::selectedFeedbackRecords() const
{
    QList<QJsonObject> records;
    if (!m_table || !m_table->selectionModel()) return records;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        if (row.data(FeedbackKindRole).toString().isEmpty()) return {};
        records.append(row.data(FeedbackRecordRole).value<QJsonObject>());
    }
    return records;
}

void CommentsOverview::resolveSelectedFeedback()
{
    const QList<QJsonObject> records = selectedFeedbackRecords();
    if (records.size() != 1) return;
    const QJsonObject &record = records.constFirst();
    Q_EMIT documentFeedbackWriteRequested(Thoughts::DocumentFeedback::resolve(record, !Thoughts::DocumentFeedback::isResolved(record), Thoughts::Platform::reviewOriginId()).toVariantMap());
}

void CommentsOverview::deleteSelectedComment()
{
    const QList<QJsonObject> feedbackRecords = selectedFeedbackRecords();
    if (!feedbackRecords.isEmpty()) {
        QMessageBox confirmation(QMessageBox::Warning, i18n("Delete Comments"), i18np("Delete the selected comment?", "Delete the %1 selected comments?", feedbackRecords.size()), QMessageBox::Cancel, this);
        auto *button = confirmation.addButton(i18n("Delete"), QMessageBox::DestructiveRole);
        confirmation.exec();
        if (confirmation.clickedButton() != button) return;
        for (const QJsonObject &record : feedbackRecords) {
            Q_EMIT documentFeedbackWriteRequested(Thoughts::DocumentFeedback::remove(record, Thoughts::Platform::reviewOriginId()).toVariantMap());
        }
        return;
    }
    const auto targets = selectedCommentTargets();
    if (targets.isEmpty() || m_operationPending || ((m_editorDirty || m_chatGptUrlDirty) && !resolveDirtyEditor())) return;
    if (targets.size() > 1) {
        QMessageBox confirmation(QMessageBox::Warning, i18n("Delete Comments"), i18np("Delete the selected comment?", "Delete the %1 selected comments?", targets.size()), QMessageBox::Cancel, this);
        auto *button = confirmation.addButton(i18n("Delete"), QMessageBox::DestructiveRole);
        confirmation.exec();
        if (confirmation.clickedButton() != button) return;
    }
    setOperationPending(true);
    Q_EMIT commentsDeleteRequested(targets);
}

QStringList CommentsOverview::selectedAnchorRemovedIds() const
{
    QStringList ids;
    if (!m_table || !m_table->selectionModel()) return ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        if (row.data(AnchorStatusRole).toString() != QLatin1String("anchor_removed") || !row.data(LogicalCommentRole).toBool()) return {};
        const QString id = row.data(AnnotationIdRole).toString();
        if (id.isEmpty()) return {};
        if (!ids.contains(id)) ids.append(id);
    }
    return ids;
}

void CommentsOverview::restoreSelectedAnchors()
{
    const QStringList ids = selectedAnchorRemovedIds();
    if (ids.isEmpty() || m_operationPending) return;
    setOperationPending(true);
    Q_EMIT commentsRestoreAnchorRequested(ids);
}

QVariantList CommentsOverview::selectedMissingCommentMarkers() const
{
    QVariantList markers;
    if (!m_table || !m_table->selectionModel()) return markers;
    for (const QModelIndex &row : m_table->selectionModel()->selectedRows()) {
        const QJsonObject marker = row.data(MissingMarkerRole).value<QJsonObject>();
        if (row.data(AnchorStatusRole).toString() != QLatin1String("comment_missing") || marker.isEmpty()) return {};
        markers.append(marker.toVariantMap());
    }
    return markers;
}

void CommentsOverview::removeSelectedMissingMarkers()
{
    const QVariantList markers = selectedMissingCommentMarkers();
    if (markers.isEmpty() || m_operationPending) return;
    setOperationPending(true);
    Q_EMIT missingCommentMarkersResolveRequested(QStringLiteral("remove"), markers);
}

void CommentsOverview::restoreSelectedMissingComments()
{
    const QVariantList markers = selectedMissingCommentMarkers();
    if (markers.isEmpty() || m_operationPending) return;
    setOperationPending(true);
    Q_EMIT missingCommentMarkersResolveRequested(QStringLiteral("restore"), markers);
}

void CommentsOverview::completeMissingCommentResolution(const QVariantMap &result)
{
    setOperationPending(false);
    const QString message = result.value(QStringLiteral("message")).toString();
    m_summary->setText(message.isEmpty() ? (result.value(QStringLiteral("success")).toBool() ? i18n("Done.") : i18n("The marker could not be resolved."))
                                         : QString(message).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    m_summary->setToolTip(message);
}

void CommentsOverview::completeAnchorRestore(const QVariantMap &result)
{
    setOperationPending(false);
    const QString message = result.value(QStringLiteral("message")).toString();
    m_summary->setText(message.isEmpty() ? (result.value(QStringLiteral("success")).toBool() ? i18n("Anchor restored.") : i18n("The anchor could not be restored."))
                                         : QString(message).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    m_summary->setToolTip(message);
}

QStringList CommentsOverview::selectedRemovedProposalIds() const
{
    QStringList ids;
    if (!m_table || !m_table->selectionModel()) return ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    // The removed proposals among the selected rows: Restore and Dismiss apply to
    // those even when the selection also holds comments or live proposals.
    for (const QModelIndex &row : rows) {
        if (row.data(AnchorStatusRole).toString() != QLatin1String("proposal_removed")) continue;
        const QString id = row.data(AnnotationIdRole).toString();
        if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
    }
    return ids;
}

void CommentsOverview::restoreSelectedRemovedProposals()
{
    const QStringList ids = selectedRemovedProposalIds();
    if (ids.isEmpty() || m_operationPending) return;
    setOperationPending(true);
    Q_EMIT proposalsRestoreRequested(ids);
}

void CommentsOverview::dismissSelectedRemovedProposals()
{
    const QStringList ids = selectedRemovedProposalIds();
    if (ids.isEmpty() || m_operationPending) return;
    setOperationPending(true);
    Q_EMIT proposalsDismissRequested(ids);
}

void CommentsOverview::copySelectedRemovedProposals()
{
    if (!m_table || !m_table->selectionModel()) return;
    QStringList texts;
    QSet<QString> seen;
    for (const QModelIndex &row : m_table->selectionModel()->selectedRows()) {
        const QJsonObject removed = row.data(RemovedProposalRole).value<QJsonObject>();
        if (removed.isEmpty() || seen.contains(row.data(AnnotationIdRole).toString())) continue;
        seen.insert(row.data(AnnotationIdRole).toString());
        texts.append(Thoughts::ProposalDecisions::copyableText(removed));
    }
    if (texts.isEmpty()) return;
    QGuiApplication::clipboard()->setText(texts.join(QStringLiteral("\n\n")));
    m_summary->setText(i18np("Copied the proposal's text.", "Copied the texts of %1 proposals.", texts.size()));
}

void CommentsOverview::completeRemovedProposalRestore(const QVariantMap &result)
{
    setOperationPending(false);
    QString message = result.value(QStringLiteral("message")).toString();
    bool offerCopy = false;
    for (const QVariant &value : result.value(QStringLiteral("results")).toList()) {
        const QString code = value.toMap().value(QStringLiteral("code")).toString();
        offerCopy = offerCopy || code == QLatin1String("text_changed") || code == QLatin1String("ambiguous");
    }
    if (offerCopy) message += i18n(" Use Copy Proposal Text to keep its text, or Dismiss.");
    if (message.isEmpty()) message = result.value(QStringLiteral("success")).toBool() ? i18n("Proposal restored.") : i18n("The proposal could not be restored.");
    m_summary->setText(QString(message).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    m_summary->setToolTip(message);
    if (result.value(QStringLiteral("success")).toBool()) scheduleReload();
}

void CommentsOverview::completeRemovedProposalDismiss(const QVariantMap &result)
{
    setOperationPending(false);
    const QString message = result.value(QStringLiteral("message")).toString();
    m_summary->setText(message.isEmpty() ? (result.value(QStringLiteral("success")).toBool() ? i18n("Proposal dismissed.") : i18n("The proposal could not be dismissed."))
                                         : QString(message).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    m_summary->setToolTip(message);
}

QList<TrackedCommentTarget> CommentsOverview::selectedCommentTargets() const
{
    QList<TrackedCommentTarget> targets;
    if (!m_table || !m_table->selectionModel()) {
        return targets;
    }
    QModelIndexList rows = m_table->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &left, const QModelIndex &right) { return left.row() < right.row(); });
    for (const QModelIndex &row : std::as_const(rows)) {
        // Comments on passages are not annotations of the PDF: selectedFeedbackRecords() has them.
        if (!row.data(FeedbackKindRole).toString().isEmpty()) return {};
        if (row.data(ItemTypeRole).toString() == QLatin1String("agent_remove") || row.data(AnchorStatusRole).toString() == QLatin1String("comment_missing")) {
            return {};
        }
        const QString pdfPath = row.data(PdfPathRole).toString();
        const QString annotationId = row.data(AnnotationIdRole).toString();
        // Global comments, including standalone notes, are identified without a PDF.
        if (!annotationId.isEmpty() && (row.data(LogicalCommentRole).toBool() || !pdfPath.isEmpty())) {
            targets.append({pdfPath, annotationId, row.data(PageRole).toInt(), row.row()});
        }
    }
    return targets;
}

QModelIndexList CommentsOverview::decidableSelectedProposalRows(QString *bundleId) const
{
    if (bundleId) bundleId->clear();
    if (!m_table || !m_table->selectionModel()) return {};
    QModelIndexList live;
    QModelIndexList artifacts;
    for (const QModelIndex &row : m_table->selectionModel()->selectedRows()) {
        // A broken proposal cannot be decided until its source is fixed; a removed one is no longer in it.
        if (row.data(ItemTypeRole).toString() != QLatin1String("agent_remove") || !row.data(ProposalIssueRole).toString().isEmpty()
            || row.data(AnchorStatusRole).toString() == QLatin1String("proposal_removed") || row.data(AnnotationIdRole).toString().isEmpty()) {
            continue;
        }
        if (row.data(LiveSourceProposalRole).toBool()) live.append(row);
        else if (!row.data(BundleIdRole).toString().isEmpty()) artifacts.append(row);
    }
    if (!live.isEmpty() || artifacts.isEmpty()) return live;
    // Artifact-only proposals are decided through the Sync queue one bundle at a time.
    QString bundle = artifacts.constFirst().data(BundleIdRole).toString();
    const QModelIndex current = m_table->selectionModel()->currentIndex();
    if (current.isValid() && artifacts.contains(current.siblingAtColumn(0))) bundle = current.data(BundleIdRole).toString();
    artifacts.removeIf([&bundle](const QModelIndex &row) { return row.data(BundleIdRole).toString() != bundle; });
    if (bundleId) *bundleId = bundle;
    return artifacts;
}

QStringList CommentsOverview::selectedAgentProposalIds(QString *bundleId) const
{
    QStringList ids;
    QSet<QString> seen;
    for (const QModelIndex &row : decidableSelectedProposalRows(bundleId)) {
        const QString id = row.data(AnnotationIdRole).toString();
        if (!seen.contains(id)) {
            seen.insert(id);
            ids.append(id);
        }
    }
    return ids;
}

bool CommentsOverview::selectedAgentProposalsAreLive() const
{
    const QModelIndexList rows = decidableSelectedProposalRows();
    return !rows.isEmpty() && rows.constFirst().data(LiveSourceProposalRole).toBool();
}

bool CommentsOverview::resolveLiveAgentProposals(const QString &action, QString *error)
{
    const QModelIndexList rows = decidableSelectedProposalRows();
    if (rows.isEmpty() || !rows.constFirst().data(LiveSourceProposalRole).toBool()) {
        if (error) {
            *error = i18n("Select proposals in local sources.");
        }
        return false;
    }
    // Proposals of several documents and Projects are decided file by file,
    // each against the source the user saw.
    struct SourceDecision {
        QString path;
        QString sha256;
        QStringList sourceFiles;
        QStringList ids;
        bool publishedPdf = false;
    };
    QList<SourceDecision> sources;
    QHash<QString, int> sourceIndexByPath;
    QHash<QString, QString> pathBySourceFile;
    QSet<QString> listed;
    for (const QModelIndex &row : rows) {
        const QString sourceFile = row.data(SourceFileRole).toString();
        auto known = pathBySourceFile.constFind(sourceFile);
        if (known == pathBySourceFile.cend()) known = pathBySourceFile.insert(sourceFile, canonicalTexSourcePath(m_docsRoot, sourceFile));
        const QString &sourcePath = *known;
        if (sourcePath.isEmpty()) {
            if (error) {
                *error = i18n("The local proposal source is missing or outside the documentation folder.");
            }
            return false;
        }
        const QString sourceSha256 = row.data(ProposalSourceSha256Role).toString();
        auto index = sourceIndexByPath.constFind(sourcePath);
        if (index == sourceIndexByPath.cend()) {
            index = sourceIndexByPath.insert(sourcePath, int(sources.size()));
            sources.append({sourcePath, sourceSha256, {}, {}, false});
        }
        SourceDecision &source = sources[*index];
        if (sourceSha256.isEmpty() || sourceSha256 != source.sha256) {
            if (error) *error = i18n("The source changed before this proposal could be applied.");
            return false;
        }
        if (!source.sourceFiles.contains(sourceFile)) source.sourceFiles.append(sourceFile);
        // One row per PDF built from the source: decide each proposal once.
        const QString id = row.data(AnnotationIdRole).toString();
        if (const QString key = sourcePath + QLatin1Char('\n') + id; !listed.contains(key)) {
            listed.insert(key);
            source.ids.append(id);
        }
        source.publishedPdf = source.publishedPdf || row.data(PdfAvailableRole).toBool();
    }
    int count = 0;
    for (const SourceDecision &source : std::as_const(sources)) count += source.ids.size();
    const QString docsRoot = m_docsRoot;
    const QByteArray catalogBaseline = m_catalogSignature;
    ++m_commentGeneration;
    m_obsoleteDocumentReloads.unite(m_runningDocumentReloads);
    setOperationPending(true);
    m_summary->setText(i18n("Saving proposal decision…"));
    QPointer<CommentsOverview> guard(this);
    // Use the persistence pool: shutdown waits for durable source writes, while
    // the worker never reads a widget or captures a QModelIndex.
    m_persistencePool->start([guard, docsRoot, sources, count, action, catalogBaseline] {
        const DeveloperActivity activity(QStringLiteral("proposal.resolve.worker"), {{QStringLiteral("count"), count}, {QStringLiteral("sources"), int(sources.size())}});
        QSet<QString> sourcePaths;
        for (const SourceDecision &source : sources) sourcePaths.insert(source.path);
        const auto before = commentCatalogSignature(docsRoot);
        const auto unrelatedBefore = commentCatalogSignature(docsRoot, sourcePaths);
        // A file the decision wrote: a decided source, or the other half of a move.
        struct WrittenSource {
            QString path;
            QStringList sourceFiles;
            LiveProjectProposals live;
        };
        QList<WrittenSource> written;
        QList<HistoryEntry::ProposalSource> undo;
        QStringList decidedIds;
        QSet<QString> decided;
        // Files an earlier decision of this batch wrote as the other half of a
        // move: the SHA-256 the user saw, and the one written.
        QHash<QString, std::pair<QString, QString>> rewritten;
        bool publishedPdf = true;
        int failed = 0;
        QString failure;
        const QString canonicalRoot = QFileInfo(docsRoot).canonicalFilePath();
        const auto noteWritten = [&written, &canonicalRoot](const QString &path, const QStringList &sourceFiles) {
            auto found = std::find_if(written.begin(), written.end(), [&path](const WrittenSource &file) { return file.path == path; });
            if (found == written.end()) {
                written.append(WrittenSource{path, {QDir(canonicalRoot).relativeFilePath(path)}, {}});
                found = std::prev(written.end());
            }
            for (const QString &sourceFile : sourceFiles)
                if (!found->sourceFiles.contains(sourceFile)) found->sourceFiles.append(sourceFile);
        };
        for (const SourceDecision &source : sources) {
            // A half decided with its partner in another file is already done.
            QStringList ids;
            for (const QString &id : source.ids)
                if (!decided.contains(id)) ids.append(id);
            if (ids.isEmpty()) continue;
            QString expected = source.sha256;
            if (const auto earlier = rewritten.constFind(source.path); earlier != rewritten.cend() && earlier->first == expected) expected = earlier->second;
            QString message;
            QByteArray previousContents;
            QString updatedSha256;
            QList<AgentProposalSourceChange> others;
            if (!Thoughts::resolveLiveAgentProposedEdits(docsRoot, source.path, expected, ids, action, &message, &previousContents, &updatedSha256, nullptr, {}, &others)) {
                failed += int(ids.size());
                if (failure.isEmpty()) failure = message;
                continue;
            }
            for (const QString &id : std::as_const(ids)) {
                decided.insert(id);
                if (const QString partner = AgentProposalMove::partner(id); !partner.isEmpty()) decided.insert(partner);
            }
            decidedIds.append(ids);
            publishedPdf = publishedPdf && source.publishedPdf;
            noteWritten(source.path, source.sourceFiles);
            for (const AgentProposalSourceChange &other : std::as_const(others)) {
                rewritten.insert(other.path, {ProposalHistory::digest(other.before), other.afterSha256});
                noteWritten(other.path, {});
            }
            undo.append({source.path, updatedSha256, previousContents, others});
        }
        QByteArray catalogAfter;
        if (!written.isEmpty()) {
            auto discovered = cachedProposalDiscovery(docsRoot);
            for (WrittenSource &file : written) {
                file.live = readLiveProjectProposals(file.path);
                if (auto it = discovered.find(file.path); it != discovered.end()) it->live = file.live;
            }
            cachedProposalDiscovery(docsRoot, &discovered);
            // Acknowledge only our write. Concurrent changes elsewhere must
            // still be detected by the normal catalog poll.
            if (before == catalogBaseline && unrelatedBefore == commentCatalogSignature(docsRoot, sourcePaths))
                catalogAfter = commentCatalogSignature(docsRoot);
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, written, undo, decidedIds, decided, failed, failure, action, publishedPdf, catalogBaseline, catalogAfter] {
            if (!guard) return;
            const DeveloperActivity activity(QStringLiteral("proposal.resolve.apply"), {{QStringLiteral("count"), int(decidedIds.size())}});
            ++guard->m_commentGeneration;
            if (written.isEmpty()) {
                guard->setOperationPending(false);
                guard->m_summary->setText(failure);
                if (guard->m_reloadDeferred) guard->m_reloadTimer->start();
                return;
            }
            QHash<QString, int> fileBySourceFile;
            QList<QHash<QString, int>> remainingLines;
            for (int file = 0; file < written.size(); ++file) {
                for (const QString &sourceFile : written.at(file).sourceFiles) fileBySourceFile.insert(sourceFile, file);
                QHash<QString, int> lines;
                for (const auto &proposal : written.at(file).live.proposals) lines.insert(proposal.id, proposal.line);
                remainingLines.append(lines);
            }
            QList<TrackedCommentTarget> targets;
            for (auto &document : guard->m_documents) {
                for (auto &comment : document.comments) {
                    if (!comment.agentProposal || !comment.proposalIssue.isEmpty()) continue;
                    const auto file = fileBySourceFile.constFind(comment.sourceFile);
                    if (file == fileBySourceFile.cend()) continue;
                    if (const auto line = remainingLines.at(*file).constFind(comment.id); line != remainingLines.at(*file).cend()) {
                        comment.proposalSourceSha256 = written.at(*file).live.sha256;
                        comment.sourceLine = *line;
                    } else if (decided.contains(comment.id)) {
                        targets.append({comment.pdfPath, comment.id, comment.page, -1});
                    }
                }
            }
            guard->removeTargetsFromModel(targets);
            // Surviving proposals need the new source hash and line, so a second
            // acceptance works immediately without a whole-library reload.
            {
                const QScopedValueRollback<bool> updating(guard->m_updatingCommentModel, true);
                for (const auto &index : modelCommentIndexes(guard->m_model)) {
                    if (!index.data(ProposalIssueRole).toString().isEmpty()) continue;
                    const auto file = fileBySourceFile.constFind(index.data(SourceFileRole).toString());
                    if (file == fileBySourceFile.cend()) continue;
                    const auto line = remainingLines.at(*file).constFind(index.data(AnnotationIdRole).toString());
                    if (line == remainingLines.at(*file).cend()) continue;
                    for (int column = 0; column < guard->m_model->columnCount(); ++column) {
                        const auto cell = index.siblingAtColumn(column);
                        guard->m_model->setData(cell, written.at(*file).live.sha256, ProposalSourceSha256Role);
                        guard->m_model->setData(cell, *line, SourceLineRole);
                    }
                }
            }
            if (!catalogAfter.isEmpty() && guard->m_catalogSignature == catalogBaseline)
                guard->m_catalogSignature = catalogAfter;
            // Undo puts every decided file back (undoLiveAgentProposalDecision()).
            // Each decision keeps its files' previous contents, so only the
            // latest ones can be undone.
            HistoryEntry history;
            history.proposalSources = undo;
            history.proposalIds = decidedIds;
            history.proposalAction = action;
            guard->m_undoHistory.append(history);
            guard->m_redoHistory.clear();
            constexpr int MaximumProposalUndoSteps = 20;
            int decisions = 0;
            for (auto it = guard->m_undoHistory.end(); it != guard->m_undoHistory.begin();) {
                --it;
                if (!it->proposalSources.isEmpty() && ++decisions > MaximumProposalUndoSteps) it = guard->m_undoHistory.erase(it);
            }
            guard->setOperationPending(false);
            guard->updateDashboardSummary(false);
            const int count = int(decidedIds.size());
            QString summary = !publishedPdf
                ? (action == QLatin1String("accept")
                    ? i18np("Accepted in the local source. Publish the document to create its PDF.", "%1 proposals accepted in the local source. Publish the document to create its PDF.", count)
                    : i18np("Rejected in the local source.", "%1 proposals rejected in the local source.", count))
                : action == QLatin1String("accept")
                    ? i18np("Accepted — saved to TeX. No PDF rebuild needed.", "%1 proposals accepted — saved to TeX. No PDF rebuild needed.", count)
                    : i18np("Rejected — saved to TeX. The next Sync updates the PDF.", "%1 proposals rejected — saved to TeX. The next Sync updates the PDF.", count);
            if (failed > 0) summary += QLatin1Char(' ') + i18np("1 other proposal was left undecided: %2", "%1 other proposals were left undecided: %2", failed, failure);
            guard->m_summary->setText(summary);
            Q_EMIT guard->agentProposalsDecidedLocally();
            // The decision changed sources, so a reload follows and redraws the list's counts
            // here: it shows the outcome again when it ends.
            guard->m_summaryAfterReload = summary;
            guard->m_summaryAfterReloadDeadline = QDeadlineTimer(10000);
            if (guard->m_reloadDeferred) guard->m_reloadTimer->start();
        }, Qt::QueuedConnection);
    });
    return true;
}

void CommentsOverview::undoLiveAgentProposalDecision()
{
    if (m_undoHistory.isEmpty() || m_undoHistory.constLast().proposalSources.isEmpty() || m_operationPending) return;
    // A decision has no Redo: decide again instead.
    const HistoryEntry entry = m_undoHistory.takeLast();
    const QString docsRoot = m_docsRoot;
    ++m_commentGeneration;
    m_obsoleteDocumentReloads.unite(m_runningDocumentReloads);
    setOperationPending(true);
    m_summary->setText(i18n("Undoing the proposal decision…"));
    QPointer<CommentsOverview> guard(this);
    m_persistencePool->start([guard, docsRoot, entry] {
        const DeveloperActivity activity(QStringLiteral("proposal.undo.worker"), {{QStringLiteral("count"), int(entry.proposalIds.size())}});
        // Every file must still be as the decision left it before any is
        // written back, so Undo restores all of them or none.
        QHash<QString, QString> expected;
        for (const auto &source : entry.proposalSources) {
            expected.insert(source.path, source.updatedSha256);
            for (const auto &other : source.otherSources) expected.insert(other.path, other.afterSha256);
        }
        QString changedFile;
        for (auto it = expected.cbegin(); it != expected.cend() && changedFile.isEmpty(); ++it) {
            QFile file(it.key());
            if (!file.open(QIODevice::ReadOnly) || ProposalHistory::digest(file.readAll()) != it.value()) changedFile = QFileInfo(it.key()).fileName();
        }
        bool restored = changedFile.isEmpty();
        QString failure;
        for (auto source = entry.proposalSources.crbegin(); source != entry.proposalSources.crend() && restored; ++source)
            restored = Thoughts::restoreLiveAgentProposalSources(docsRoot, source->path, source->updatedSha256, source->previousContents, source->otherSources, &failure);
        auto discovered = cachedProposalDiscovery(docsRoot);
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            if (auto found = discovered.find(it.key()); found != discovered.end()) found->live = readLiveProjectProposals(it.key());
        }
        cachedProposalDiscovery(docsRoot, &discovered);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, entry, changedFile, restored, failure] {
            if (!guard) return;
            ++guard->m_commentGeneration;
            guard->setOperationPending(false);
            const int count = int(entry.proposalIds.size());
            const QString message = !changedFile.isEmpty() ? i18n("Undo is no longer safe because %1 changed.", changedFile)
                : !restored ? (failure.isEmpty() ? i18n("The proposal decision could not be undone safely.") : failure)
                : entry.proposalAction == QLatin1String("accept")
                    ? i18np("Acceptance undone — proposal restored.", "Acceptance undone — %1 proposals restored.", count)
                    : i18np("Rejection undone — proposal restored.", "Rejection undone — %1 proposals restored.", count);
            guard->m_summary->setText(message);
            guard->m_summaryAfterReload = message;
            guard->m_summaryAfterReloadDeadline = QDeadlineTimer(10000);
            if (restored) {
                Q_EMIT guard->agentProposalsDecidedLocally();
            }
            guard->startFullReload(false, QStringLiteral("proposal decision undone"));
        }, Qt::QueuedConnection);
    });
}

void CommentsOverview::acceptSelectedAgentProposals()
{
    resolveSelectedAgentProposals(QStringLiteral("accept"));
}

void CommentsOverview::disregardSelectedAgentProposals()
{
    resolveSelectedAgentProposals(QStringLiteral("disregard"));
}

void CommentsOverview::resolveSelectedAgentProposals(const QString &action)
{
    QString bundleId;
    const QStringList ids = selectedAgentProposalIds(&bundleId);
    if (ids.isEmpty() || m_operationPending) {
        return;
    }
    if (selectedAgentProposalsAreLive()) {
        QString error;
        if (!resolveLiveAgentProposals(action, &error)) {
            QMessageBox::warning(this, i18n("Agent Proposal"), error);
            return;
        }
        return;
    }
    setOperationPending(true);
    Q_EMIT agentProposalsResolveRequested(bundleId, action, ids);
}

void CommentsOverview::completeAgentProposalResolution(const QStringList &proposalIds, bool queued, const QString &error)
{
    if (!queued) {
        setOperationPending(false);
        QMessageBox::warning(this, i18n("Agent Proposal"), error.isEmpty() ? i18n("The proposal decision could not be queued.") : error);
        return;
    }
    m_summary->setText(i18np("Decision saved locally — the next Sync sends it to the server.", "%1 decisions saved locally — the next Sync sends them to the server.", proposalIds.size()));
}

void CommentsOverview::finishAgentProposalResolution(const QStringList &proposalIds,
                                                      const QString &action,
                                                      const QString &status,
                                                      const QString &message)
{
    if (status == QLatin1String("waiting")) {
        if (action.startsWith(QLatin1String("undo_"))) {
            setOperationPending(true);
        }
        m_summary->setText(i18np("Decision saved locally — the next Sync sends it to the server.", "%1 decisions saved locally — the next Sync sends them to the server.", proposalIds.size()));
        return;
    }
    setOperationPending(false);
    if (status == QLatin1String("failed")) {
        QMessageBox::warning(this, i18n("Agent Proposal"), message.isEmpty() ? i18n("The server could not apply the proposal decision.") : message);
        return;
    }
    if (status != QLatin1String("applied")) {
        return;
    }
    if (action.startsWith(QLatin1String("undo_"))) {
        for (const QString &id : proposalIds) {
            m_queuedAgentProposalIds.remove(id);
        }
        m_summary->setText(message);
        startFullReload();
        return;
    }
    for (const QString &id : proposalIds) {
        m_queuedAgentProposalIds.insert(id);
    }
    m_editorDirty = false;
    m_chatGptUrlDirty = false;
    populateDocumentList(selectedDocument());
    populateComments();
    selectComment(QString(), QString());
    applyFilters();
    m_summary->setText(action == QLatin1String("accept")
                           ? i18np("Accepted — saved to TeX. No PDF rebuild needed.", "%1 proposals accepted — saved to TeX. No PDF rebuild needed.", proposalIds.size())
                           : i18np("Rejected — saved to TeX. PDF update queued.", "%1 proposals rejected — saved to TeX. PDF update queued.", proposalIds.size()));
}

void CommentsOverview::restoreCommentSelection(const QList<TrackedCommentTarget> &targets, const QString &currentPdfPath, const QString &currentAnnotationId, bool loadCurrentComment)
{
    if (!m_table || !m_table->selectionModel()) {
        return;
    }
    QItemSelection selection;
    QModelIndex current;
    for (const TrackedCommentTarget &target : targets) {
        for (const QModelIndex &candidate : modelCommentIndexes(m_proxy)) {
            if (candidate.data(PdfPathRole).toString() != target.pdfPath || candidate.data(AnnotationIdRole).toString() != target.annotationId) {
                continue;
            }
            selection.select(candidate, candidate);
            if (!current.isValid() || (target.pdfPath == currentPdfPath && target.annotationId == currentAnnotationId)) {
                current = candidate;
            }
            break;
        }
    }
    m_changingSelection = true;
    m_table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (current.isValid()) {
        m_table->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        m_table->scrollTo(current);
    } else {
        m_table->selectionModel()->clearCurrentIndex();
    }
    m_changingSelection = false;
    if (loadCurrentComment && current.isValid()) {
        loadComment(current);
    }
    updateSelectionUi();
}

void CommentsOverview::updateSelectionUi()
{
    traceInteraction(QStringLiteral("comments.selection.before"));
    const int selectionCount = m_table && m_table->selectionModel() ? m_table->selectionModel()->selectedRows().size() : 0;
    // Several proposals keep the panel for their actions (Accept, Reject, or Restore and Dismiss).
    const bool proposalSelection = !selectedAgentProposalIds().isEmpty() || !selectedRemovedProposalIds().isEmpty();
    if (m_details) {
        m_details->setVisible(selectionCount == 1 || proposalSelection);
    }
    traceInteraction(QStringLiteral("comments.selection.after"));
    updateEditorActions();
    // The panel and its rows may just have appeared; give it its width now
    // rather than on the next layout pass, so it is never shown squeezed.
    if (m_details && m_details->isVisible()) {
        if (auto *splitter = qobject_cast<QSplitter *>(m_details->parentWidget())) {
            splitter->refresh();
        }
    }
}

void CommentsOverview::showCommentsContextMenu(const QPoint &position)
{
    const QModelIndex clicked = m_table->indexAt(position);
    if (!clicked.isValid()) {
        return;
    }
    if (!m_table->selectionModel()->isRowSelected(clicked.row(), clicked.parent())) {
        m_table->selectionModel()->setCurrentIndex(clicked, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    QMenu menu(m_table);
    QString sourcePath;
    QString unavailableReason;
    int sourceLine = 0;
    const bool sourceAvailable = currentSourceTarget(&sourcePath, &sourceLine, &unavailableReason);
    auto *openSourceAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open with VS Code"));
    openSourceAction->setObjectName(QStringLiteral("commentsOverviewOpenSourceAction"));
    openSourceAction->setEnabled(sourceAvailable && !m_operationPending && clicked.siblingAtColumn(CommentColumn) == m_currentComment);
    openSourceAction->setToolTip(sourceAvailable ? i18n("Open %1:%2 in Visual Studio Code.", sourcePath, sourceLine) : unavailableReason);
    connect(openSourceAction, &QAction::triggered, this, [this, sourcePath, sourceLine] { Q_EMIT openSourceRequested(sourcePath, sourceLine); });
    menu.addSeparator();
    // A mixed selection offers each group of actions for the rows it applies
    // to: Accept and Reject for the decidable proposals, Restore, Copy and
    // Dismiss for the removed ones. The other rows are left out.
    const QStringList proposalIds = selectedAgentProposalIds();
    const QStringList removedIds = selectedRemovedProposalIds();
    if (!proposalIds.isEmpty() || !removedIds.isEmpty()) {
        if (!proposalIds.isEmpty()) {
            QAction *acceptAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18np("Accept Proposal", "Accept %1 Proposals", proposalIds.size()));
            acceptAction->setObjectName(QStringLiteral("commentsOverviewAcceptProposalAction"));
            acceptAction->setEnabled(!m_operationPending);
            QAction *disregardAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18np("Reject Proposal", "Reject %1 Proposals", proposalIds.size()));
            disregardAction->setObjectName(QStringLiteral("commentsOverviewRejectProposalAction"));
            disregardAction->setEnabled(!m_operationPending);
            connect(acceptAction, &QAction::triggered, this, &CommentsOverview::acceptSelectedAgentProposals);
            connect(disregardAction, &QAction::triggered, this, &CommentsOverview::disregardSelectedAgentProposals);
        }
        if (!removedIds.isEmpty()) {
            if (!proposalIds.isEmpty()) menu.addSeparator();
            QAction *restoreAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18np("Restore Proposal", "Restore %1 Proposals", removedIds.size()));
            restoreAction->setObjectName(QStringLiteral("commentsOverviewRestoreProposalAction"));
            restoreAction->setEnabled(!m_operationPending);
            connect(restoreAction, &QAction::triggered, this, &CommentsOverview::restoreSelectedRemovedProposals);
            QAction *copyAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Proposal Text"));
            copyAction->setObjectName(QStringLiteral("commentsOverviewCopyProposalAction"));
            connect(copyAction, &QAction::triggered, this, &CommentsOverview::copySelectedRemovedProposals);
            QAction *dismissAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), i18n("Dismiss"));
            dismissAction->setObjectName(QStringLiteral("commentsOverviewDismissProposalAction"));
            dismissAction->setEnabled(!m_operationPending);
            connect(dismissAction, &QAction::triggered, this, &CommentsOverview::dismissSelectedRemovedProposals);
        }
    } else if (const QVariantList missingMarkers = selectedMissingCommentMarkers(); !missingMarkers.isEmpty()) {
        QAction *restoreAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18np("Restore Comment", "Restore %1 Comments", missingMarkers.size()));
        restoreAction->setObjectName(QStringLiteral("commentsOverviewRestoreMissingCommentAction"));
        restoreAction->setEnabled(!m_operationPending);
        connect(restoreAction, &QAction::triggered, this, &CommentsOverview::restoreSelectedMissingComments);
        QAction *removeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18np("Remove Marker", "Remove %1 Markers", missingMarkers.size()));
        removeAction->setObjectName(QStringLiteral("commentsOverviewRemoveMissingMarkerAction"));
        removeAction->setEnabled(!m_operationPending);
        connect(removeAction, &QAction::triggered, this, &CommentsOverview::removeSelectedMissingMarkers);
    } else {
        const int count = selectedCommentTargets().size();
        if (count == 0) {
            return;
        }
        if (count == 1) {
            QAction *replyAction = menu.addAction(QIcon::fromTheme(QStringLiteral("mail-reply-sender")), i18n("Reply…"));
            replyAction->setObjectName(QStringLiteral("commentsOverviewReplyAction"));
            connect(replyAction, &QAction::triggered, this, &CommentsOverview::replyToSelectedComment);
        }
        const QStringList anchorRemovedIds = selectedAnchorRemovedIds();
        if (!anchorRemovedIds.isEmpty()) {
            QAction *restoreAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18np("Restore Anchor", "Restore %1 Anchors", anchorRemovedIds.size()));
            restoreAction->setObjectName(QStringLiteral("commentsOverviewRestoreAnchorAction"));
            restoreAction->setEnabled(!m_operationPending);
            connect(restoreAction, &QAction::triggered, this, &CommentsOverview::restoreSelectedAnchors);
        }
        QAction *deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18np("Delete Comment", "Delete %1 Comments", count));
        connect(deleteAction, &QAction::triggered, this, &CommentsOverview::deleteSelectedComment);
    }
    menu.exec(m_table->viewport()->mapToGlobal(position));
}

void CommentsOverview::showDocumentsContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_documentList->itemAt(position);
    if (item && item->data(0, DocumentItemKindRole).toInt() == ComponentListEntry) {
        m_documentList->setCurrentItem(item);
        const QString id = item->data(0, DocumentCompositionIdRole).toString();
        const QString path = item->data(0, DocumentPathRole).toString();
        if (id.isEmpty()) { return; }
        for (QTreeWidgetItemIterator it(m_libraryTree); *it; ++it) {
            if ((*it)->data(0, LibraryCompositionIdRole).toString() == id && (*it)->data(0, Qt::UserRole).toString() == path) {
                showLibraryItemContextMenu(*it, m_documentList, position, false);
                return;
            }
        }
        return;
    }
    if (!item || item->data(0, DocumentItemKindRole).toInt() != DocumentListEntry) { return; }
    const QString pdfPath = item->data(0, DocumentPathRole).toString();
    if (pdfPath.isEmpty()) {
        return;
    }
    const QString filename = item->data(0, DocumentFilenameRole).toString();
    const QString sha256 = item->data(0, DocumentSha256Role).toString();
    m_documentList->setCurrentItem(item);
    QMenu menu(m_documentList);
    auto *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open PDF"));
    connect(open, &QAction::triggered, this, [this, pdfPath] { Q_EMIT documentActivated(pdfPath); });
    QAction *informationAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Information…"));
    informationAction->setObjectName(QStringLiteral("documentInformationAction"));
    connect(informationAction, &QAction::triggered, this, [this, pdfPath] {
        showDocumentInformation(pdfPath);
    });
    if (filename.isEmpty() || sha256.isEmpty()) {
        menu.exec(m_documentList->viewport()->mapToGlobal(position));
        return;
    }
    menu.addSeparator();
    QAction *renameAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
    connect(renameAction, &QAction::triggered, this, [this, item, filename, sha256] {
        bool accepted = false;
        const QString currentName = item->data(0, DocumentDisplayNameRole).toString();
        const QString displayName = QInputDialog::getText(this, i18n("Rename Document"), i18n("Display name:"), QLineEdit::Normal, currentName, &accepted).trimmed();
        if (!accepted) {
            return;
        }
        if (displayName.isEmpty() || displayName.size() > 200 || std::any_of(displayName.cbegin(), displayName.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; })) {
            KMessageBox::error(this, i18n("Enter a name between 1 and 200 characters without line breaks."), i18n("Rename Document"));
            return;
        }
        if (displayName != currentName) {
            Q_EMIT libraryDisplayNameRequested(filename, sha256, displayName);
        }
    });
    menu.exec(m_documentList->viewport()->mapToGlobal(position));
}

void CommentsOverview::manageDocuments(const QJsonObject &project)
{
    if (m_syncOperationRunning) return;
    DocumentManagementDialog dialog(m_docsRoot, project, this);
    if (dialog.exec() != QDialog::Accepted) return;
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally(project.value(QStringLiteral("target")).toString());
}

void CommentsOverview::showDocumentOrder(const QString &pdfPath, const QString &selectedId)
{
    QTreeWidgetItem *project = nullptr;
    for (int i = 0; i < m_libraryTree->topLevelItemCount(); ++i) {
        auto *candidate = m_libraryTree->topLevelItem(i);
        if (candidate->data(0, Qt::UserRole).toString() == pdfPath) { project = candidate; break; }
    }
    if (!project || !project->child(0)) { return; }
    Q_UNUSED(selectedId);
    const auto configuration = project->data(0, Qt::UserRole + 140).value<QJsonObject>();
    if (!configuration.isEmpty()) manageDocuments(configuration);
}

void CommentsOverview::libraryProjectsChangedByCommand()
{
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally({});
}

void CommentsOverview::createLibraryProject()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, i18n("Add project"), i18n("Project name:"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (name.isEmpty() || name.size() > 200 || name.contains(QLatin1Char('\n'))) {
        KMessageBox::error(this, i18n("Enter a project name between 1 and 200 characters.")); return;
    }
    for (int i = 0; i < m_libraryTree->topLevelItemCount(); ++i) if (m_libraryTree->topLevelItem(i)->text(0).compare(name, Qt::CaseInsensitive) == 0) {
        KMessageBox::error(this, i18n("A project with this name already exists.")); return;
    }
    const QJsonObject project {{QStringLiteral("id"), QString(QStringLiteral("new:") + QUuid::createUuid().toString(QUuid::WithoutBraces))}, {QStringLiteral("revision"), 0}};
    const QString error = LibraryProjectStore(m_docsRoot).enqueue(QStringLiteral("create"), project, {{QStringLiteral("name"), name}});
    if (!error.isEmpty()) { KMessageBox::error(this, error); return; }
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally({});
}

void CommentsOverview::editLibraryProject(const QJsonObject &project, const QString &action, const QJsonObject &document)
{
    QJsonObject values;
    if (action == QLatin1String("rename")) {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, i18n("Rename project"), i18n("Project name:"), QLineEdit::Normal, project.value(QStringLiteral("name")).toString(), &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        values.insert(QStringLiteral("name"), name);
    } else if (action == QLatin1String("rename_document")) {
        bool accepted = false;
        const QString current = document.value(QStringLiteral("name")).toString();
        const QString name = QInputDialog::getText(this, i18n("Rename Document"), i18n("Display name:"), QLineEdit::Normal, current, &accepted).trimmed();
        if (!accepted || name == current) return;
        if (name.isEmpty() || name.size() > 200 || std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; })) {
            KMessageBox::error(this, i18n("Enter a name between 1 and 200 characters without line breaks.")); return;
        }
        values.insert(QStringLiteral("document_id"), document.value(QStringLiteral("id")));
        values.insert(QStringLiteral("name"), name);
    } else if (action == QLatin1String("publish_document") || action == QLatin1String("unpublish_document")) {
        // Reversible, so it asks nothing (protocol/project-document-publication.md).
        values.insert(QStringLiteral("document_id"), document.value(QStringLiteral("id")));
        values.insert(QStringLiteral("published"), action == QLatin1String("publish_document"));
        const QString error = LibraryProjectStore(m_docsRoot).enqueue(QStringLiteral("set_document_published"), project, values);
        if (!error.isEmpty()) { KMessageBox::error(this, error); return; }
        updateLibraryTree(); Q_EMIT projectAttachmentChangedLocally(project.value(QStringLiteral("target")).toString());
        return;
    } else if (action == QLatin1String("rename_source")) {
        const QString owner = document.value(QStringLiteral("project")).toString();
        const QString current = document.value(QStringLiteral("folder")).toString(owner);
        bool accepted = false;
        const QString name = QInputDialog::getText(this, i18n("Rename source folder"),
            i18n("Name shown in Sources (the folder %1 stays where it is):", owner), QLineEdit::Normal, current, &accepted).trimmed();
        if (!accepted || name == current) return;
        if (name.isEmpty() || name.size() > 200 || std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; })) {
            KMessageBox::error(this, i18n("Enter a name between 1 and 200 characters without line breaks.")); return;
        }
        bool taken = name.compare(QStringLiteral("Default source folder"), Qt::CaseInsensitive) == 0;
        for (const auto &entry : project.value(QStringLiteral("source_folders")).toArray()) {
            const auto other = entry.toObject();
            if (other.value(QStringLiteral("project")).toString() != owner && other.value(QStringLiteral("folder")).toString().compare(name, Qt::CaseInsensitive) == 0) taken = true;
        }
        if (taken) { KMessageBox::error(this, i18n("Another source folder in this project already has this name.")); return; }
        values.insert(QStringLiteral("source_project"), owner);
        values.insert(QStringLiteral("name"), name);
    } else if (action == QLatin1String("remove_source")) {
        const QString owner = document.value(QStringLiteral("project")).toString();
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("removeSourceDialog"));
        dialog.setWindowTitle(i18n("Remove source"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel(i18n("Remove “%1” from this project?\n\nIts documents will be removed from the project's build selection. Local files, the Git repository, and existing comments and review cards will be preserved.\n\nClick Sync to apply the change on other devices.", document.value(QStringLiteral("folder")).toString()), &dialog);
        label->setWordWrap(true); layout->addWidget(label);
        auto *list = new QListWidget(&dialog);
        list->setObjectName(QStringLiteral("removeSourceDocuments"));
        for (const auto &entry : project.value(QStringLiteral("documents")).toArray()) {
            const auto doc = entry.toObject();
            if (doc.value(QStringLiteral("source_project")).toString() == owner || doc.value(QStringLiteral("id")).toString().startsWith(owner + QLatin1Char('/')))
                list->addItem(doc.value(QStringLiteral("name")).toString(doc.value(QStringLiteral("id")).toString()));
        }
        layout->addWidget(new QLabel(list->count() ? i18n("Affected documents:") : i18n("No documents are currently selected from this source."), &dialog));
        list->setVisible(list->count() > 0); layout->addWidget(list);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(i18n("Remove source"));
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons); dialog.resize(520, 360);
        if (dialog.exec() != QDialog::Accepted) return;
        values.insert(QStringLiteral("source_project"), owner);
    } else if (action == QLatin1String("remove") || action == QLatin1String("remove_document")) {
        const bool whole = action == QLatin1String("remove");
        if (KMessageBox::questionTwoActions(this,
            whole ? i18n("Remove this project, its documents, comments and review cards from your Library?\n\nOriginal files remain. The change reaches other devices when you click Sync.")
                  : i18n("Remove this document from the project?\n\nIts comments are removed and its review cards are deleted permanently; Unpublish keeps them instead. Original files remain. Click Sync to apply the change on other devices."),
            whole ? i18n("Remove project") : i18n("Remove document"), KGuiItem(i18n("Remove")), KStandardGuiItem::cancel()) != KMessageBox::PrimaryAction) return;
        values.insert(QStringLiteral("document_id"), document.value(QStringLiteral("id")));
    } else if (action == QLatin1String("import_folder")) {
        values = Thoughts::prepareSourcesImport(m_docsRoot, this);
        if (values.isEmpty()) return;
    } else if (action == QLatin1String("import")) {
        const QStringList paths = QFileDialog::getOpenFileNames(this, i18n("Import document"), {}, i18n("PDF documents (*.pdf)"));
        QJsonObject current = project;
        for (const QString &path : paths) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.read(5) != QByteArrayLiteral("%PDF-")) { KMessageBox::error(this, i18n("Choose a readable PDF document.")); continue; }
            file.close();
            const QString error = LibraryProjectStore(m_docsRoot).importPdf(current, path, QFileInfo(path).completeBaseName().left(150));
            if (!error.isEmpty()) { KMessageBox::error(this, error); break; }
            current.insert(QStringLiteral("revision"), current.value(QStringLiteral("revision")).toInt() + 1);
        }
        updateLibraryTree(); Q_EMIT projectAttachmentChangedLocally({}); return;
    } else if (action == QLatin1String("order")) {
        manageDocuments(project);
        return;
    }

    const QString error = LibraryProjectStore(m_docsRoot).enqueue(action, project, values);
    if (!error.isEmpty()) { KMessageBox::error(this, error); return; }
    updateLibraryTree(); Q_EMIT projectAttachmentChangedLocally(project.value(QStringLiteral("target")).toString());
}

QString CommentsOverview::saveWebsiteImport(const QJsonObject &project, const QString &projectName, const QString &target, const QString &path, const QString &name, const QUrl &link)
{
    if (project.isEmpty()) {
        const QString hash = QString::fromLatin1(fileContentSha256(path).toHex());
        if (hash.isEmpty()) return i18n("The downloaded PDF could not be read.");
        QString safe = name;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        if (safe.isEmpty() || !safe.at(0).isLetterOrNumber()) safe.prepend(QStringLiteral("document_"));
        const QString projectSuffix = QString::fromLatin1(QCryptographicHash::hash(QString(QStringLiteral("source:") + projectName).toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
        const QString filename = safe + QLatin1Char('_') + hash.left(12) + QLatin1Char('_') + projectSuffix + QStringLiteral(".pdf");
        const auto mutation = setProjectAttachment(m_docsRoot, projectName, path, filename, hash, name, true);
        if (!mutation.success) return mutation.error;
        if (!link.isEmpty()) LibraryProjectStore::recordSourceLink(m_docsRoot, filename, link);
        if (mutation.changed) Q_EMIT projectAttachmentChangedLocally(target);
    } else {
        LibraryProjectStore store(m_docsRoot);
        // The catalogue may refresh while the download is in flight.
        QJsonObject current;
        for (const auto &value : store.projects(false)) {
            if (value.toObject().value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) current = value.toObject();
        }
        if (current.isEmpty()) return i18n("The destination project is no longer available.");
        const QString error = store.importPdf(current, path, name, link);
        if (!error.isEmpty()) return error;
        Q_EMIT projectAttachmentChangedLocally(current.value(QStringLiteral("target")).toString());
    }
    updateLibraryTree();
    return {};
}

void CommentsOverview::importLibraryWebsite(const QJsonObject &project, const QString &projectName, const QString &target, const QString &link)
{
    const QString destination = project.isEmpty() ? projectName : project.value(QStringLiteral("name")).toString();
    WebsiteImportDialog dialog(destination, [this, project, projectName, target](const QString &path, const QString &name, const QUrl &link) {
        return saveWebsiteImport(project, projectName, target, path, name, link);
    }, this, nullptr, link);
    dialog.exec();
}

void CommentsOverview::moveLibraryDocument(const QJsonObject &project, const QJsonObject &document)
{
    if (m_syncOperationRunning) return;
    LibraryProjectStore store(m_docsRoot);
    const auto projects = store.projects(false);
    if (!store.error().isEmpty()) { KMessageBox::error(this, store.error()); return; }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("moveDocumentDialog"));
    dialog.setWindowTitle(i18n("Move PDF to different project"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Move “%1” and all its annotations to:", document.value(QStringLiteral("name")).toString()), &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto *destination = new QComboBox(&dialog);
    destination->setObjectName(QStringLiteral("moveDocumentDestination"));
    for (const auto &value : projects) {
        const auto candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) continue;
        destination->addItem(candidate.value(QStringLiteral("name")).toString(), candidate);
    }
    layout->addWidget(destination);
    auto *status = new QLabel(&dialog); status->setWordWrap(true); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Move"));
    layout->addWidget(buttons);
    const auto validate = [=] {
        const auto target = destination->currentData().toJsonObject();
        const auto filename = document.value(QStringLiteral("attachment_filename")).toString(document.value(QStringLiteral("filename")).toString());
        bool duplicate = false;
        for (const auto &value : target.value(QStringLiteral("documents")).toArray()) {
            const auto d = value.toObject();
            duplicate |= d.value(QStringLiteral("attachment_filename")).toString(d.value(QStringLiteral("filename")).toString()) == filename;
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!target.isEmpty() && !duplicate);
        status->setText(target.isEmpty() ? i18n("Create another project before moving this PDF.") : duplicate ? i18n("This project already contains the PDF.") : i18n("The PDF and all its annotations will move together when you click Sync."));
    };
    connect(destination, &QComboBox::currentIndexChanged, &dialog, validate); validate();
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto target = destination->currentData().toJsonObject();
        const auto error = store.enqueue(QStringLiteral("move_document"), project,
            {{QStringLiteral("document_id"), document.value(QStringLiteral("id"))},
             {QStringLiteral("destination_project_id"), target.value(QStringLiteral("id"))},
             {QStringLiteral("destination_revision"), target.value(QStringLiteral("revision")).toInt()}});
        if (!error.isEmpty()) { status->setText(error); return; }
        dialog.accept();
    });
    dialog.resize(460, 200);
    if (dialog.exec() != QDialog::Accepted) return;
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally({});
}

void CommentsOverview::requestSourceMovePlan(const QJsonObject &request, QObject *context,
                                             std::function<void(const QJsonObject &plan, const QString &error)> reply)
{
    if (m_sourceMovePlanner) { m_sourceMovePlanner(request, context, std::move(reply)); return; }
    QByteArray token = qgetenv("THOUGHTS_TOKEN").trimmed();
    const QString address = Thoughts::ServerAddress::configured();
    if (token.isEmpty()) token = Thoughts::ServerAddress::storedToken(Thoughts::Platform::desktopDeviceAccount(), address);
    const QUrl url(Thoughts::ServerAddress::normalized(address));
    if (token.isEmpty() || url.isEmpty()) { reply({}, i18n("Connect this computer to the server to plan the move.")); return; }
    auto *client = new Thoughts::SyncClient(url, token, context);
    auto *network = client->editLibraryProject(request);
    connect(network, &QNetworkReply::finished, context, [network, client, reply] {
        const QByteArray body = network->readAll();
        const bool failed = network->error() != QNetworkReply::NoError;
        const QJsonObject value = QJsonDocument::fromJson(body).object();
        QString error;
        if (failed) {
            error = value.value(QStringLiteral("detail")).toString();
            if (error.isEmpty()) error = network->errorString();
        }
        network->deleteLater(); client->deleteLater();
        if (!error.isEmpty()) reply({}, error);
        else if (!value.value(QStringLiteral("plan")).isObject()) reply({}, i18n("The server does not support moving source documents yet. Install the server update."));
        else reply(value.value(QStringLiteral("plan")).toObject(), {});
    });
}

void CommentsOverview::moveLibrarySourceDocument(const QJsonObject &project, const QJsonObject &document)
{
    if (m_syncOperationRunning) return;
    LibraryProjectStore store(m_docsRoot);
    const auto projects = store.projects(false);
    if (!store.error().isEmpty()) { KMessageBox::error(this, store.error()); return; }
    const QString sourceFile = document.value(QStringLiteral("source_file")).toString();
    const QString owner = document.value(QStringLiteral("source_project")).toString();
    const QString relative = sourceFile.startsWith(owner + QLatin1Char('/')) ? sourceFile.mid(owner.size() + 1) : sourceFile.section(QLatin1Char('/'), 1);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("moveSourceDocumentDialog"));
    dialog.setWindowTitle(i18n("Move document to project"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Move “%1” and the files it uses to:", document.value(QStringLiteral("name")).toString()), &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto *destination = new QComboBox(&dialog);
    destination->setObjectName(QStringLiteral("moveSourceDocumentDestination"));
    for (const auto &value : projects) {
        const auto candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) continue;
        destination->addItem(candidate.value(QStringLiteral("name")).toString(), candidate);
    }
    layout->addWidget(destination);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(i18n("Path in its new folder:"), &dialog));
    auto *path = new QLineEdit(relative, &dialog);
    path->setObjectName(QStringLiteral("moveSourceDocumentPath"));
    pathRow->addWidget(path, 1);
    layout->addLayout(pathRow);
    auto *carry = new QCheckBox(i18n("Add the preamble definitions it needs to the document"), &dialog);
    carry->setObjectName(QStringLiteral("moveSourceDocumentCarryPreamble"));
    carry->setChecked(true); carry->setVisible(false);
    layout->addWidget(carry);
    auto *review = new QTextBrowser(&dialog);
    review->setObjectName(QStringLiteral("moveSourceDocumentReview"));
    review->setOpenLinks(false);
    layout->addWidget(review, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Move"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    auto plan = std::make_shared<QJsonObject>();
    auto generation = std::make_shared<int>(0);
    auto *debounce = new QTimer(&dialog);
    debounce->setSingleShot(true); debounce->setInterval(250);
    const QString documentId = document.value(QStringLiteral("document_id")).toString();
    const auto refresh = [=, this, &dialog] {
        *plan = {};
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        const auto target = destination->currentData().toJsonObject();
        if (target.isEmpty()) { review->setHtml(i18n("Create another project before moving this document.")); return; }
        review->setHtml(i18n("Checking what the move needs…"));
        const int requestNumber = ++*generation;
        requestSourceMovePlan(Thoughts::SourceDocumentMove::planRequest(project, documentId, target.value(QStringLiteral("id")).toString(),
                                                                        path->text().trimmed(), carry->isChecked()),
                              &dialog, [=](const QJsonObject &value, const QString &error) {
            if (requestNumber != *generation) return;
            if (!error.isEmpty()) { review->setHtml(QStringLiteral("<p style='color:#b00020'>%1</p>").arg(error.toHtmlEscaped())); return; }
            *plan = value;
            const QJsonObject preamble = value.value(QStringLiteral("preamble")).toObject();
            bool needsPreamble = !preamble.value(QStringLiteral("missing")).toArray().isEmpty();
            for (const auto &package : preamble.value(QStringLiteral("packages")).toArray())
                needsPreamble = needsPreamble || package.toObject().value(QStringLiteral("required")).toBool();
            carry->setVisible(needsPreamble);
            QHash<QString, QStringList> generators;
            const QString originFolder = value.value(QStringLiteral("source")).toObject().value(QStringLiteral("folder")).toString();
            for (const auto &entry : value.value(QStringLiteral("files")).toArray())
                if (entry.toObject().value(QStringLiteral("generated")).toBool()) {
                    const QString relativeInput = entry.toObject().value(QStringLiteral("source")).toString();
                    generators.insert(relativeInput, Thoughts::SourceDocumentMove::generatorScripts(m_docsRoot, originFolder, relativeInput));
                }
            review->setHtml(Thoughts::SourceDocumentMove::describe(value, generators));
            buttons->button(QDialogButtonBox::Ok)->setEnabled(!value.value(QStringLiteral("destination")).toObject().value(QStringLiteral("collision")).toBool()
                                                              && !value.value(QStringLiteral("plan_digest")).toString().isEmpty());
        });
    };
    connect(debounce, &QTimer::timeout, &dialog, refresh);
    connect(destination, &QComboBox::currentIndexChanged, &dialog, [debounce] { debounce->start(); });
    connect(path, &QLineEdit::textEdited, &dialog, [debounce] { debounce->start(); });
    connect(carry, &QCheckBox::toggled, &dialog, [debounce] { debounce->start(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (plan->isEmpty()) return;
        // The plan is the server's: it may only name files of the document's folder and of the
        // chosen project's folders (any plain folder name for a project that has none yet).
        const auto foldersOf = [](const QJsonObject &candidate) {
            QStringList names{candidate.value(QStringLiteral("source_project")).toString()};
            for (const auto &entry : candidate.value(QStringLiteral("source_folders")).toArray()) names << entry.toObject().value(QStringLiteral("project")).toString();
            for (const auto &entry : candidate.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("source_folders")).toArray())
                names << entry.toObject().value(QStringLiteral("path")).toString();
            names.removeAll(QString());
            return names;
        };
        const QJsonObject chosen = destination->currentData().toJsonObject();
        const QStringList destinationFolders = foldersOf(chosen);
        QStringList foreignFolders;
        for (const auto &value : projects)
            if (value.toObject().value(QStringLiteral("id")) != chosen.value(QStringLiteral("id"))) foreignFolders << foldersOf(value.toObject());
        QString unbound;
        if (!Thoughts::SourceDocumentMove::planBound(*plan, owner, destinationFolders, foreignFolders, &unbound)) {
            review->append(QStringLiteral("<p style='color:#b00020'>%1</p>").arg(unbound.toHtmlEscaped()));
            return;
        }
        const auto values = Thoughts::SourceDocumentMove::operationValues(*plan, carry->isChecked());
        // Figures and generated inputs are copied from this computer; they must be the reviewed bytes.
        for (const auto &value : values.value(QStringLiteral("desktop_transfers")).toArray()) {
            const auto transfer = value.toObject();
            const QString local = Thoughts::SourceDocumentMove::boundPath(m_docsRoot, values.value(QStringLiteral("source_folder")).toString(),
                                                                          transfer.value(QStringLiteral("source")).toString());
            if (local.isEmpty() || Thoughts::SourceDocumentMove::fileSha256(local) != transfer.value(QStringLiteral("sha256")).toString()) {
                review->append(QStringLiteral("<p style='color:#b00020'>%1</p>").arg(
                    i18n("This computer's copy of %1 differs from the one the server knows. Sync, then move the document.", transfer.value(QStringLiteral("source")).toString()).toHtmlEscaped()));
                return;
            }
        }
        QJsonObject source;
        for (const auto &value : projects)
            if (value.toObject().value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) source = value.toObject();
        source.insert(QStringLiteral("revision"), plan->value(QStringLiteral("source")).toObject().value(QStringLiteral("revision")).toInt());
        const auto error = store.enqueue(QString(Thoughts::SourceDocumentMove::action), source.isEmpty() ? project : source, values);
        if (!error.isEmpty()) { review->append(QStringLiteral("<p style='color:#b00020'>%1</p>").arg(error.toHtmlEscaped())); return; }
        dialog.accept();
    });
    dialog.resize(620, 520);
    refresh();
    if (dialog.exec() != QDialog::Accepted) return;
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally({});
}

void CommentsOverview::moveLibrarySourceFolder(const QJsonObject &project, const QString &owner, const QString &name)
{
    if (m_syncOperationRunning) return;
    LibraryProjectStore store(m_docsRoot);
    const auto projects = store.projects(false);
    if (!store.error().isEmpty()) { KMessageBox::error(this, store.error()); return; }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("moveSourceDialog"));
    dialog.setWindowTitle(i18n("Move source folder to project"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(i18n("Move “%1”, its documents, and their annotations to:", name), &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto *destination = new QComboBox(&dialog);
    destination->setObjectName(QStringLiteral("moveSourceDestination"));
    for (const auto &value : projects) {
        const auto candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) continue;
        destination->addItem(candidate.value(QStringLiteral("name")).toString(), candidate);
    }
    layout->addWidget(destination);
    auto *status = new QLabel(&dialog); status->setWordWrap(true); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Move")); layout->addWidget(buttons);
    const auto validate = [=] {
        const auto target = destination->currentData().toJsonObject();
        bool duplicate = LibraryProjectStore::ownsSourceFolder(target, owner);
        for (const auto &value : target.value(QStringLiteral("source_folders")).toArray())
            duplicate |= value.toObject().value(QStringLiteral("project")).toString() == owner;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!target.isEmpty() && !duplicate);
        status->setText(target.isEmpty() ? i18n("Create another project before moving this folder.")
            : duplicate ? i18n("This project already contains the source folder.")
            : i18n("The folder and its documents will move when you click Sync."));
    };
    connect(destination, &QComboBox::currentIndexChanged, &dialog, validate); validate();
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto target = destination->currentData().toJsonObject();
        const auto error = store.enqueue(QStringLiteral("move_source"), project,
            {{QStringLiteral("source_project"), owner},
             {QStringLiteral("destination_project_id"), target.value(QStringLiteral("id"))},
             {QStringLiteral("destination_revision"), target.value(QStringLiteral("revision")).toInt()}});
        if (!error.isEmpty()) { status->setText(error); return; }
        dialog.accept();
    });
    dialog.resize(460, 200);
    if (dialog.exec() != QDialog::Accepted) return;
    updateLibraryTree();
    Q_EMIT projectAttachmentChangedLocally({});
}

// Renames a source file in place; Sync then sends the change like any edit.
// Returns why it could not, or an empty string.
QString CommentsOverview::renameSourceFile(const QString &path, const QString &requestedName)
{
    const QFileInfo info(path);
    const QString name = requestedName.trimmed();
    if (name.isEmpty()) return i18n("Enter a file name.");
    if (name == info.fileName()) return {};
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name == QLatin1String(".") || name == QLatin1String(".."))
        return i18n("“%1” is not a valid file name.", name);
    const QString target = info.dir().filePath(name);
    if (QFileInfo::exists(target)) return i18n("“%1” already exists in this folder.", name);
    if (!QFile::rename(path, target)) return i18n("“%1” could not be renamed.", info.fileName());
    requestCatalogCheck(true);
    return {};
}

// Creates a new .md or .tex source in a Library folder; Sync then sends it
// like any edit. A LaTeX file starts like the other .tex files there, up to
// \begin{document}, so it builds the same way. Returns why it could not, or
// an empty string, and the new file's path in created.
QString CommentsOverview::createSourceFile(const QString &folder, const QString &requestedName, const QString &extension, QString *created)
{
    QString name = requestedName.trimmed();
    if (name.isEmpty()) return i18n("Enter a file name.");
    if (!name.endsWith(QLatin1Char('.') + extension, Qt::CaseInsensitive)) name += QLatin1Char('.') + extension;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
        return i18n("“%1” is not a valid file name.", name);
    const QDir dir(folder);
    const QString target = dir.filePath(name);
    if (!dir.exists()) return i18n("The folder “%1” no longer exists.", folder);
    if (QFileInfo::exists(target)) return i18n("“%1” already exists in this folder.", name);
    QString title = QFileInfo(name).completeBaseName();
    title.replace(QRegularExpression(QStringLiteral("[_-]+")), QStringLiteral(" "));
    title = title.trimmed();
    if (!title.isEmpty()) title[0] = title[0].toUpper();
    QString content;
    if (extension == QLatin1String("md")) {
        content = QStringLiteral("# %1\n\n").arg(title);
    } else {
        for (const auto &sibling : dir.entryInfoList({QStringLiteral("*.tex")}, QDir::Files, QDir::Name)) {
            QFile file(sibling.filePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QString text = QString::fromUtf8(file.read(64 * 1024));
            const auto begin = text.indexOf(QStringLiteral("\\begin{document}"));
            if (begin < 0) continue;
            content = text.left(begin) + QStringLiteral("\\begin{document}\n\n\\end{document}\n");
            break;
        }
    }
    QFile file(target);
    if (!file.open(QIODevice::NewOnly | QIODevice::WriteOnly) || file.write(content.toUtf8()) != content.toUtf8().size())
        return i18n("“%1” could not be created.", name);
    file.close();
    if (created) *created = target;
    requestCatalogCheck(true);
    return {};
}

// Moves a source file to the Trash, so it can be restored. Returns why it
// could not, or an empty string.
QString CommentsOverview::deleteSourceFile(const QString &path)
{
    if (!QFile::moveToTrash(path)) return i18n("“%1” could not be moved to the Trash.", QFileInfo(path).fileName());
    requestCatalogCheck(true);
    return {};
}

// A source folder's recorded build error stands only while one of its
// documents still lacks a built PDF; a failed rebuild of a built PDF is
// reported for that document instead. Older imports left such notes behind
// ("Sources recovered from an interrupted import ...").
QString CommentsOverview::currentFolderBuildError(const QJsonObject &project, const QString &owner, const QString &error)
{
    if (error.isEmpty() || owner.isEmpty()) return error;
    bool hasDocuments = false;
    for (const auto &value : project.value(QStringLiteral("documents")).toArray()) {
        const auto document = value.toObject();
        if (!document.value(QStringLiteral("source_file")).toString().startsWith(owner + QLatin1Char('/'))) continue;
        hasDocuments = true;
        // Built means published. The project's record says so, so that the answer does not depend
        // on whether the Library's documents have been read yet.
        if (!document.value(QStringLiteral("published")).toBool() || document.value(QStringLiteral("sha256")).toString().isEmpty()) return error;
    }
    return hasDocuments ? QString() : error;
}

void CommentsOverview::openAgentConversation(const QJsonObject &record)
{
    const QString url = record.value(QStringLiteral("conversation_url")).toString();
    if (url.isEmpty()) return;
    if (!Thoughts::openAgentConversationUrl(url) && m_librarySelectionDetails) {
        m_librarySelectionDetails->setText(url.startsWith(QLatin1String("codex:")) ? i18n("Could not open the conversation in Codex. Check that Codex is installed.")
                                                                                 : i18n("Could not open the conversation."));
        m_librarySelectionDetails->show();
    }
}

void CommentsOverview::removeAgentConversation(const QJsonObject &record)
{
    // Removal is a flag on the record: the row disappears once the queued revision is shown.
    Q_EMIT agentConversationRemoveRequested(Thoughts::AgentConversations::remove(record, Thoughts::Platform::desktopOriginId()).toVariantMap());
}

void CommentsOverview::showLibraryContextMenu(const QPoint &position)
{
    showLibraryItemContextMenu(m_libraryTree ? m_libraryTree->itemAt(position) : nullptr, m_libraryTree, position, true);
}

void CommentsOverview::showLibraryItemContextMenu(QTreeWidgetItem *item, QTreeWidget *view, const QPoint &position, bool allowOrder)
{
    if (const QVariant conversation = item ? item->data(0, LibraryConversationRole) : QVariant(); conversation.isValid()) {
        const QJsonObject record = conversation.value<QJsonObject>();
        const QString url = record.value(QStringLiteral("conversation_url")).toString();
        const bool codex = record.value(QStringLiteral("agent")).toString() == QLatin1String("codex");
        QMenu menu(view);
        auto *open = menu.addAction(QIcon::fromTheme(QStringLiteral("internet-services")), codex ? i18n("Open in Codex") : i18n("Open in Claude"));
        open->setObjectName(QStringLiteral("libraryOpenAgentConversationAction"));
        open->setEnabled(!url.isEmpty());
        connect(open, &QAction::triggered, this, [this, record] { openAgentConversation(record); });
        auto *copyLink = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Link"));
        copyLink->setObjectName(QStringLiteral("libraryCopyAgentConversationLinkAction"));
        copyLink->setEnabled(!url.isEmpty());
        connect(copyLink, &QAction::triggered, this, [url] { QApplication::clipboard()->setText(url); });
        auto *copyId = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Conversation ID"));
        copyId->setObjectName(QStringLiteral("libraryCopyAgentConversationIdAction"));
        connect(copyId, &QAction::triggered, this, [record] { QApplication::clipboard()->setText(record.value(QStringLiteral("conversation_id")).toString()); });
        menu.addSeparator();
        auto *remove = menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from Project"));
        remove->setObjectName(QStringLiteral("libraryRemoveAgentConversationAction"));
        connect(remove, &QAction::triggered, this, [this, record] { removeAgentConversation(record); });
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    const QString sourcePath = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    // A row marked for a failed PDF build offers the failure's details first.
    const auto addBuildFailureAction = [this, item](QMenu &menu) {
        const QString failure = item ? item->data(0, LibraryBuildFailureRole).toString() : QString();
        if (failure.isEmpty()) return;
        auto *details = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-warning")), i18n("Show Build Error…"));
        details->setObjectName(QStringLiteral("libraryShowBuildErrorAction"));
        const QString name = item->text(0).section(QLatin1Char('\n'), 0, 0);
        // The item can be deleted by a refresh while the menu is open; keep its text.
        connect(details, &QAction::triggered, this, [this, name, failure] { showLibraryBuildFailure(name, failure); });
        menu.addSeparator();
    };
    const auto addSourceOpenWith = [this, item, sourcePath, &addBuildFailureAction](QMenu &menu) {
        if (sourcePath.isEmpty()) return;
        addBuildFailureAction(menu);
        const QString absolute = QDir(m_docsRoot).absoluteFilePath(sourcePath);
        const QString relative = QDir(m_docsRoot).relativeFilePath(absolute);
        auto *openWith = menu.addMenu(i18n("Open With…"));
        openWith->setObjectName(QStringLiteral("libraryOpenWithMenu"));
        auto *code = openWith->addAction(i18n("VS Code"));
        code->setEnabled(QFileInfo(absolute).isFile());
        connect(code, &QAction::triggered, this, [this, absolute] { Q_EMIT openSourceRequested(absolute, 1); });
        if (Thoughts::isHtmlSourcePath(relative)) {
            auto *browser = openWith->addAction(i18n("Browser"));
            browser->setObjectName(QStringLiteral("libraryOpenHtmlBrowserAction"));
            auto *root = item; while (root->parent()) root = root->parent();
            const auto pdf = QUrl::fromLocalFile(root->data(0, Qt::UserRole).toString());
            connect(browser, &QAction::triggered, this, [this, absolute, relative, pdf] {
                if (QFileInfo(absolute).isFile()) {
                    const auto url = Thoughts::serveHtmlAttachmentFile(absolute);
                    if (!url.isValid() || url.isEmpty() || !QDesktopServices::openUrl(url))
                        KMessageBox::error(this, i18n("Could not open this HTML page. Choose a UTF-8 HTML file no larger than 100 MB."));
                } else {
                    QString error;
                    if (!Thoughts::openHtmlAttachment(QUrl(QStringLiteral("thoughts-html:") + relative), pdf, &error)) KMessageBox::error(this, error);
                }
            });
        }
        auto *other = openWith->addAction(i18n("Other application…"));
        other->setEnabled(QFileInfo(absolute).isFile());
        connect(other, &QAction::triggered, this, [this, absolute] {
#ifdef Q_OS_WIN
            // Windows programs are found by path, not by application name.
            const QString application = QFileDialog::getOpenFileName(this, i18n("Open With…"), qEnvironmentVariable("ProgramFiles"), i18n("Programs (*.exe)"));
            if (application.isEmpty()) return;
            const bool started = Thoughts::Platform::openWithApplication(application, absolute);
#else
            bool ok = false;
            const auto application = QInputDialog::getText(this, i18n("Open With…"), i18n("Application name or path:"), QLineEdit::Normal, {}, &ok).trimmed();
            if (!ok || application.isEmpty()) return;
#ifdef Q_OS_MACOS
            const bool started = QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), application, absolute});
#else
            const bool started = QProcess::startDetached(application, {absolute});
#endif
#endif
            if (!started) KMessageBox::error(this, i18n("Could not open the selected application."));
        });

        menu.addSeparator();
        auto *copyPath = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy path"));
        copyPath->setObjectName(QStringLiteral("librarySourceCopyPathAction"));
        connect(copyPath, &QAction::triggered, this, [absolute] {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(absolute));
        });
        const bool exists = QFileInfo(absolute).isFile();
        auto *rename = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
        rename->setObjectName(QStringLiteral("librarySourceRenameAction"));
        rename->setEnabled(exists);
        connect(rename, &QAction::triggered, this, [this, absolute] {
            const QString current = QFileInfo(absolute).fileName();
            bool ok = false;
            const QString name = QInputDialog::getText(this, i18n("Rename"), i18n("New name for “%1”:", current), QLineEdit::Normal, current, &ok);
            if (!ok) return;
            const QString error = renameSourceFile(absolute, name);
            if (!error.isEmpty()) KMessageBox::error(this, error, i18n("Rename"));
        });
        auto *remove = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete"));
        remove->setObjectName(QStringLiteral("librarySourceDeleteAction"));
        remove->setEnabled(exists);
        connect(remove, &QAction::triggered, this, [this, absolute] {
            const QString name = QFileInfo(absolute).fileName();
            if (KMessageBox::warningContinueCancel(this,
                                                   i18n("Move “%1” to the Trash?\n\nDocuments that use it may stop building. Sync removes it from the server too.", name),
                                                   i18n("Delete Source File"), KStandardGuiItem::del())
                != KMessageBox::Continue) return;
            const QString error = deleteSourceFile(absolute);
            if (!error.isEmpty()) KMessageBox::error(this, error, i18n("Delete Source File"));
        });
    };
    const QString folderPath = item ? item->data(0, LibraryFolderPathRole).toString() : QString();
    if (!folderPath.isEmpty()) {
        QMenu menu(view);
        addBuildFailureAction(menu);
        auto *newFile = menu.addMenu(QIcon::fromTheme(QStringLiteral("document-new")), i18n("New"));
        newFile->setObjectName(QStringLiteral("libraryNewSourceMenu"));
        newFile->setEnabled(QFileInfo(folderPath).isDir());
        for (const auto &[extension, label] : {std::pair(QStringLiteral("md"), i18n("Markdown file…")), std::pair(QStringLiteral("tex"), i18n("LaTeX file…"))}) {
            auto *action = newFile->addAction(label);
            action->setObjectName(QStringLiteral("libraryNewSourceAction_") + extension);
            connect(action, &QAction::triggered, this, [this, folderPath, extension, label] {
                QString name = QStringLiteral("untitled.") + extension;
                while (true) {
                    bool ok = false;
                    name = QInputDialog::getText(this, i18n("New %1", QString(label).remove(QStringLiteral("…"))),
                                                 i18n("Name of the new file in “%1”:", QFileInfo(folderPath).fileName()), QLineEdit::Normal, name, &ok);
                    if (!ok) return;
                    const QString error = createSourceFile(folderPath, name, extension);
                    if (error.isEmpty()) return;
                    KMessageBox::error(this, error, i18n("New File"));
                }
            });
        }
        menu.addSeparator();
        auto *openInCode = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Open in VS Code"));
        openInCode->setObjectName(QStringLiteral("libraryOpenInVSCodeAction"));
        openInCode->setEnabled(QFileInfo(folderPath).isDir());
        connect(openInCode, &QAction::triggered, this, [this, folderPath] {
            Q_EMIT openSourceRequested(folderPath, 0);
        });
#ifdef Q_OS_MACOS
        auto *reveal = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Reveal in Finder"));
        reveal->setObjectName(QStringLiteral("libraryRevealInFinderAction"));
        reveal->setEnabled(QFileInfo(folderPath).isDir());
        connect(reveal, &QAction::triggered, this, [this, folderPath] {
            if (!QFileInfo(folderPath).isDir() || !QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), folderPath})) {
                KMessageBox::error(this, i18n("Finder could not reveal “%1”.", folderPath), i18n("Reveal in Finder Failed"));
            }
        });
#elif defined(Q_OS_WIN)
        auto *reveal = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Show in Explorer"));
        reveal->setObjectName(QStringLiteral("libraryRevealInFinderAction"));
        reveal->setEnabled(QFileInfo(folderPath).isDir());
        connect(reveal, &QAction::triggered, this, [this, folderPath] {
            if (!QFileInfo(folderPath).isDir() || !Thoughts::Platform::revealInFileManager(folderPath)) {
                KMessageBox::error(this, i18n("Explorer could not show “%1”.", folderPath), i18n("Show in Explorer Failed"));
            }
        });
#endif
        auto *copyPath = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy path"));
        copyPath->setObjectName(QStringLiteral("libraryCopyPathAction"));
        connect(copyPath, &QAction::triggered, this, [folderPath] {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(folderPath));
        });
        const auto location = item->data(0, Qt::UserRole + 142).value<QJsonObject>();
        auto *root = item;
        while (root->parent()) root = root->parent();
        const auto project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
        const QString owner = Thoughts::SourceOwner::of(location.value(QStringLiteral("id")).toString());
        const QString ownerRoot = QDir(m_docsRoot).filePath(owner);
        const auto sourceFolders = project.value(QStringLiteral("source_folders")).toArray();
        if (!owner.isEmpty() && (LibraryProjectStore::ownsSourceFolder(project, owner)
            || std::any_of(sourceFolders.cbegin(), sourceFolders.cend(),
                           [&](const QJsonValue &entry) { return entry.toObject().value(QStringLiteral("project")).toString() == owner; }))
            && (folderPath == ownerRoot || folderPath == ownerRoot + QStringLiteral("/tex")
                || folderPath == ownerRoot + QStringLiteral("/markdown"))) {
            menu.addSeparator();
            auto *move = menu.addAction(QIcon::fromTheme(QStringLiteral("document-send")), i18n("Move source to project…"));
            move->setObjectName(QStringLiteral("libraryMoveSourceAction"));
            move->setEnabled(!m_syncOperationRunning);
            connect(move, &QAction::triggered, this, [this, project, owner, item] {
                moveLibrarySourceFolder(project, owner, item->text(0).section(QLatin1Char('\n'), 0, 0));
            });
        }
        for (const auto &entry : project.value(QStringLiteral("source_folders")).toArray()) {
            const auto folder = entry.toObject();
            if (owner.isEmpty() || folder.value(QStringLiteral("project")).toString() != owner || folder.value(QStringLiteral("pending_import")).toBool()) continue;
            menu.addSeparator();
            auto *rename = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
            rename->setObjectName(QStringLiteral("libraryRenameSourceAction"));
            rename->setEnabled(!m_syncOperationRunning);
            connect(rename, &QAction::triggered, this, [this, project, folder] { editLibraryProject(project, QStringLiteral("rename_source"), folder); });
            auto *remove = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Remove source…"));
            remove->setObjectName(QStringLiteral("libraryRemoveSourceAction"));
            remove->setEnabled(!m_syncOperationRunning);
            connect(remove, &QAction::triggered, this, [this, project, folder] { editLibraryProject(project, QStringLiteral("remove_source"), folder); });
            break;
        }
        const QUrl overleafUrl(item->data(0, LibraryOverleafUrlRole).toString());
        if (overleafUrl.isValid() && !overleafUrl.isEmpty()) {
            auto *open = menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18n("Open Overleaf"));
            connect(open, &QAction::triggered, this, [overleafUrl] { QDesktopServices::openUrl(overleafUrl); });
        }
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    const auto addHtmlImport = [this](QMenu &menu, const QString &projectName, const QString &target) {
        if (!QRegularExpression(QStringLiteral("^docs-[A-Za-z0-9_.-]+$")).match(projectName).hasMatch()) return;
        auto *action = menu.addAction(i18n("Import HTML…"));
        action->setObjectName(QStringLiteral("libraryImportHtmlAction"));
        action->setEnabled(!m_syncOperationRunning);
        connect(action, &QAction::triggered, this, [this, projectName, target] {
            const QString source = QFileDialog::getOpenFileName(this, i18n("Import self-contained HTML"), {}, i18n("HTML files (*.html)"));
            if (source.isEmpty()) return;
            QFile input(source);
            if (!input.open(QIODevice::ReadOnly) || input.size() < 1 || input.size() > 100000000) {
                KMessageBox::error(this, i18n("Choose a readable, self-contained HTML file up to 100 MB.")); return;
            }
            QString name = QFileInfo(source).completeBaseName();
            name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
            if (name.isEmpty() || !name.front().isLetterOrNumber()) name.prepend(QStringLiteral("page_"));
            name += QStringLiteral(".html");
            const QString relative = projectName + QStringLiteral("/tex/support/html/") + name;
            LibraryProjectStore store(m_docsRoot);
            QJsonObject project;
            for (const auto &value : store.projects(false)) {
                if (LibraryProjectStore::ownsSourceFolder(value.toObject(), projectName)) { project = value.toObject(); break; }
            }
            if (project.isEmpty()) { KMessageBox::error(this, i18n("Sync the Library before importing an attachment.")); return; }
            const QString directory = QDir(m_docsRoot).filePath(QStringLiteral(".thoughts/html-imports"));
            if (!QDir().mkpath(directory)) { KMessageBox::error(this, i18n("Could not create the attachment store.")); return; }
            const QString saved = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
            QFile output(saved);
            if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { KMessageBox::error(this, i18n("Could not save the attachment.")); return; }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            qint64 total = 0;
            while (!input.atEnd()) {
                const auto chunk = input.read(1024 * 1024);
                total += chunk.size();
                if (chunk.isEmpty() || total > 100000000 || output.write(chunk) != chunk.size()) {
                    output.close(); output.remove(); KMessageBox::error(this, i18n("Could not copy the attachment.")); return;
                }
                hash.addData(chunk);
            }
            if (!output.flush() || total < 1) { output.close(); output.remove(); return; }
            output.close();
            const auto error = store.enqueue(QStringLiteral("import_html"), project,
                {{QStringLiteral("local_path"), saved}, {QStringLiteral("filename"), name},
                 {QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex())}, {QStringLiteral("size_bytes"), total}});
            if (!error.isEmpty()) { QFile::remove(saved); KMessageBox::error(this, error); return; }
            const QString link = QStringLiteral("thoughts-html:") + relative;
            QApplication::clipboard()->setText(link);
            Q_EMIT projectAttachmentChangedLocally(target);
            updateLibraryTree();
            KMessageBox::information(this, i18n("HTML imported. Its link has been copied to the clipboard. Add it as a hyperlink in your document, then Sync to publish the PDF and attachment."));
        });
    };

    const auto groupImports = [this](QMenu &menu, const QString &name) {
        QList<QAction *> imports;
        for (auto *entry : menu.actions()) {
            if (entry->text() == i18n("Import PDF…") || entry->text() == i18n("Import document…")
                || entry->objectName() == QLatin1String("libraryImportHtmlAction")
                || entry->objectName() == QLatin1String("libraryImportWebsiteAction")) {
                imports.append(entry);
                menu.removeAction(entry);
            }
        }
        auto *import = menu.addAction(i18n("Import…"));
        import->setObjectName(QStringLiteral("libraryImportAction"));
        import->setEnabled(!m_syncOperationRunning);
        connect(import, &QAction::triggered, this, [this, imports, name] {
            QAction *pdfAction = nullptr, *htmlAction = nullptr, *linkAction = nullptr;
            for (auto *entry : imports) {
                if (entry->objectName() == QLatin1String("libraryImportWebsiteAction")) linkAction = entry;
                else if (entry->objectName() == QLatin1String("libraryImportHtmlAction")) htmlAction = entry;
                else pdfAction = entry;
            }
            // One place for every kind of import: paste a link, or pick a file.
            QDialog dialog(this);
            dialog.setObjectName(QStringLiteral("documentImportDialog"));
            dialog.setWindowTitle(i18n("Import into %1", name));
            dialog.setMinimumWidth(460);
            auto *layout = new QVBoxLayout(&dialog);
            auto *linkLabel = new QLabel(i18n("Paste a link to a paper or a PDF (arXiv, OpenReview, or any PDF link):"), &dialog);
            linkLabel->setWordWrap(true);
            layout->addWidget(linkLabel);
            auto *linkRow = new QHBoxLayout;
            auto *link = new QLineEdit(&dialog);
            link->setObjectName(QStringLiteral("documentImportLink"));
            link->setPlaceholderText(i18n("https://arxiv.org/abs/2609.37633"));
            link->setClearButtonEnabled(true);
            auto *importLink = new QPushButton(i18n("Import link"), &dialog);
            importLink->setObjectName(QStringLiteral("documentImportLinkButton"));
            importLink->setEnabled(false);
            importLink->setDefault(true);
            linkRow->addWidget(link, 1);
            linkRow->addWidget(importLink);
            layout->addLayout(linkRow);
            if (!linkAction) { link->setEnabled(false); linkLabel->setEnabled(false); }
            if (const QString clip = QApplication::clipboard()->text().trimmed(); linkAction && clip.startsWith(QLatin1String("http")) && !clip.contains(QLatin1Char('\n')))
                link->setText(clip);
            importLink->setEnabled(!link->text().trimmed().isEmpty());
            connect(link, &QLineEdit::textChanged, &dialog, [importLink](const QString &text) { importLink->setEnabled(!text.trimmed().isEmpty()); });
            layout->addSpacing(8);
            layout->addWidget(new QLabel(i18n("Or choose a file on this computer:"), &dialog));
            auto *fileRow = new QHBoxLayout;
            QPushButton *choosePdf = nullptr;
            if (pdfAction) {
                choosePdf = new QPushButton(QIcon::fromTheme(QStringLiteral("application-pdf")), i18n("Choose PDF file…"), &dialog);
                choosePdf->setObjectName(QStringLiteral("documentImportPdfButton"));
                fileRow->addWidget(choosePdf);
            }
            QPushButton *chooseHtml = nullptr;
            if (htmlAction) {
                chooseHtml = new QPushButton(QIcon::fromTheme(QStringLiteral("text-html")), i18n("Choose HTML file…"), &dialog);
                chooseHtml->setObjectName(QStringLiteral("documentImportHtmlButton"));
                fileRow->addWidget(chooseHtml);
            }
            fileRow->addStretch(1);
            layout->addLayout(fileRow);
            if (!htmlAction) {
                auto *note = new QLabel(i18n("HTML files can be imported into projects with their own source folder."), &dialog);
                note->setWordWrap(true);
                note->setEnabled(false);
                layout->addWidget(note);
            }
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            QAction *chosen = nullptr;
            QString chosenLink;
            connect(importLink, &QPushButton::clicked, &dialog, [&] { chosen = linkAction; chosenLink = link->text().trimmed(); dialog.accept(); });
            connect(link, &QLineEdit::returnPressed, &dialog, [&] { if (importLink->isEnabled()) importLink->click(); });
            if (choosePdf) connect(choosePdf, &QPushButton::clicked, &dialog, [&] { chosen = pdfAction; dialog.accept(); });
            if (chooseHtml) connect(chooseHtml, &QPushButton::clicked, &dialog, [&] { chosen = htmlAction; dialog.accept(); });
            if (dialog.exec() != QDialog::Accepted || !chosen) return;
            chosen->setProperty("thoughtsImportLink", chosenLink);
            chosen->trigger();
        });
    };

    if (item && item->data(0, LibraryIdentityRole).toString().endsWith(QLatin1String("/sources"))) {
        auto *root = item;
        while (root->parent()) root = root->parent();
        QJsonObject project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
        if (project.isEmpty()) {
            const auto owner = root->toolTip(0);
            for (const auto &value : LibraryProjectStore(m_docsRoot).projects(false)) {
                const auto candidate = value.toObject();
                if (!owner.isEmpty() && LibraryProjectStore::ownsSourceFolder(candidate, owner)) {
                    project = candidate;
                    break;
                }
            }
        }
        QMenu menu(view);
        auto *import = menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Import sources…"));
        import->setObjectName(QStringLiteral("libraryImportSourcesAction"));
        import->setEnabled(!m_syncOperationRunning && !project.isEmpty());
        if (project.isEmpty()) import->setToolTip(i18n("Sync the Library before importing sources into this project."));
        connect(import, &QAction::triggered, this, [this, project] {
            editLibraryProject(project, QStringLiteral("import_folder"), {});
        });
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    if (item) {
        auto *root = item; while (root->parent()) root = root->parent();
        QJsonObject project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
        if (project.isEmpty()) {
            const QString identity = root->data(0, LibraryIdentityRole).toString();
            for (const auto &value : LibraryProjectStore(m_docsRoot).projects(false)) {
                const QJsonObject candidate = value.toObject();
                if (candidate.value(QStringLiteral("id")).toString() == identity) {
                    project = candidate;
                    break;
                }
            }
        }
        const bool group = item->data(0, LibraryIdentityRole).toString().endsWith(QLatin1String("/documents"));
        if (!project.isEmpty() && (item == root || group || !item->data(0, Qt::UserRole + 141).isNull() || item->data(0, Qt::UserRole + 142).value<QJsonObject>().value(QStringLiteral("pending_import")).toBool())) {
            // Refreshes can delete the tree item while the menu's event loop runs.
            const auto location = item->data(0, Qt::UserRole + 142).value<QJsonObject>();
            if (location.value(QStringLiteral("pending_import")).toBool()) {
                QMenu menu(view); auto *cancel = menu.addAction(i18n("Cancel folder import")); cancel->setEnabled(!m_syncOperationRunning);
                connect(cancel, &QAction::triggered, this, [this, location] {
                    const auto error = LibraryProjectStore(m_docsRoot).cancelFolderImport(location.value(QStringLiteral("import_operation_id")).toString());
                    if (!error.isEmpty()) KMessageBox::error(this, error);
                    updateLibraryTree(); Q_EMIT projectAttachmentChangedLocally({});
                });
                menu.exec(view->viewport()->mapToGlobal(position)); return;
            }
            const QJsonObject document = item->data(0, Qt::UserRole + 141).value<QJsonObject>();
            QMenu menu(view);
            addBuildFailureAction(menu);
            const auto action = [&](const QString &label, const QString &kind) {
                auto *entry = menu.addAction(label);
                connect(entry, &QAction::triggered, this, [this, project, kind, document] {
                    editLibraryProject(project, kind, document);
                });
            };
            if (item == root || group) {
                const auto folders = project.value(QStringLiteral("source_folders")).toArray();
                if (folders.size() == 1) addHtmlImport(menu, folders.first().toObject().value(QStringLiteral("project")).toString(), root->data(0, LibraryFilenameRole).toString());
            }
            if (item == root) {
                auto *exportPdf = menu.addAction(i18n("Export combined PDF…"));
                exportPdf->setEnabled(!project.value(QStringLiteral("reading_revision")).toString().isEmpty());
                connect(exportPdf, &QAction::triggered, this, [this, project] {
                    QJsonObject request = project;
                    const auto documents = project.value(QStringLiteral("documents")).toArray();
                    if (documents.size() > 1) {
                        // Every document starts ticked; unticked ones are left out of the export.
                        QDialog chooser(this);
                        chooser.setWindowTitle(i18n("Export combined PDF"));
                        auto *layout = new QVBoxLayout(&chooser);
                        layout->addWidget(new QLabel(i18n("Documents to include:"), &chooser));
                        auto *list = new QListWidget(&chooser);
                        list->setObjectName(QStringLiteral("projectExportDocuments"));
                        for (const auto &value : documents) {
                            const auto document = value.toObject();
                            QString identity = document.value(QStringLiteral("document_id")).toString();
                            if (identity.isEmpty()) identity = document.value(QStringLiteral("id")).toString();
                            auto *entry = new QListWidgetItem(document.value(QStringLiteral("name")).toString(identity), list);
                            entry->setData(Qt::UserRole, identity);
                            entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
                            entry->setCheckState(Qt::Checked);
                        }
                        layout->addWidget(list);
                        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
                        buttons->button(QDialogButtonBox::Ok)->setText(i18n("Export…"));
                        layout->addWidget(buttons);
                        connect(buttons, &QDialogButtonBox::accepted, &chooser, &QDialog::accept);
                        connect(buttons, &QDialogButtonBox::rejected, &chooser, &QDialog::reject);
                        connect(list, &QListWidget::itemChanged, &chooser, [list, buttons] {
                            bool any = false;
                            for (int row = 0; row < list->count(); ++row) any = any || list->item(row)->checkState() == Qt::Checked;
                            buttons->button(QDialogButtonBox::Ok)->setEnabled(any);
                        });
                        if (chooser.exec() != QDialog::Accepted) return;
                        QJsonArray chosen;
                        for (int row = 0; row < list->count(); ++row)
                            if (list->item(row)->checkState() == Qt::Checked) chosen.append(list->item(row)->data(Qt::UserRole).toString());
                        if (chosen.size() < list->count()) request.insert(QStringLiteral("export_document_ids"), chosen);
                    }
                    const auto destination = QFileDialog::getSaveFileName(this, i18n("Export combined PDF"), project.value(QStringLiteral("name")).toString() + QStringLiteral(".pdf"), i18n("PDF files (*.pdf)"));
                    if (destination.isEmpty()) return;
                    Q_EMIT projectExportRequested(request, destination);
                });
            }
            if (item == root) {
                action(i18n("Import sources…"), QStringLiteral("import_folder"));
                action(i18n("Rename…"), QStringLiteral("rename"));
                auto *copyName = menu.addAction(i18n("Copy project name"));
                copyName->setObjectName(QStringLiteral("libraryCopyProjectNameAction"));
                const QString name = project.value(QStringLiteral("name")).toString();
                copyName->setEnabled(!name.isEmpty());
                connect(copyName, &QAction::triggered, this, [name] { QGuiApplication::clipboard()->setText(name); });
                action(i18n("Remove project…"), QStringLiteral("remove"));
            }
            else if (group) {
                action(i18n("Import PDF…"), QStringLiteral("import"));
                auto *website = menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Import from website…"));
                website->setObjectName(QStringLiteral("libraryImportWebsiteAction"));
                connect(website, &QAction::triggered, this, [this, project, website] { importLibraryWebsite(project, {}, {}, website->property("thoughtsImportLink").toString()); });
                auto *select = menu.addAction(i18n("Manage documents…"));
                select->setObjectName(QStringLiteral("libraryManageDocumentsAction"));
                const auto folders = project.value(QStringLiteral("source_folders")).toArray();
                select->setEnabled(!m_syncOperationRunning
                    && std::none_of(folders.cbegin(), folders.cend(), [](const QJsonValue &v) { return v.toObject().value(QStringLiteral("pending_import")).toBool(); }));
                connect(select, &QAction::triggered, this, [this, project] { manageDocuments(project); });
                groupImports(menu, project.value(QStringLiteral("name")).toString());
            }
            else {
                const QString pdfPath = item->data(0, Qt::UserRole).toString();
                const int page = item->data(0, LibraryPageRole).toInt();
                auto *open = menu.addAction(i18n("Open PDF"));
                open->setEnabled(page > 0 && !pdfPath.isEmpty());
                connect(open, &QAction::triggered, this, [this, pdfPath, page, document] {
                    Q_EMIT documentPageActivated(pdfPath, page, document.value(QStringLiteral("document_id")).toString(), document.value(QStringLiteral("name")).toString());
                });
                const QString sourcePath = document.value(QStringLiteral("source_file")).toString();
                if (sourcePath.endsWith(QLatin1String(".md")) || sourcePath.endsWith(QLatin1String(".markdown"))) {
                    auto *edit = menu.addAction(i18n("Edit source"));
                    const QString absolute = QDir(m_docsRoot).filePath(sourcePath);
                    edit->setEnabled(QFileInfo::exists(absolute));
                    connect(edit, &QAction::triggered, this, [this, absolute] { Q_EMIT openSourceRequested(absolute, 1); });
                }
                action(i18n("Rename…"), QStringLiteral("rename_document"));
                // A server that can publish and unpublish lists `unpublished_documents`.
                if (project.contains(QStringLiteral("unpublished_documents"))) {
                    const bool unpublished = item->data(0, LibraryUnpublishedRole).toBool();
                    auto *publication = menu.addAction(unpublished ? i18n("Publish") : i18n("Unpublish"));
                    publication->setObjectName(QStringLiteral("libraryDocumentPublicationAction"));
                    publication->setToolTip(unpublished ? i18n("Build it again and offer its review cards for review")
                                                        : i18n("Keep it in the project, but stop building it and stop offering its review cards"));
                    publication->setEnabled(!m_syncOperationRunning);
                    connect(publication, &QAction::triggered, this, [this, project, document, unpublished] {
                        editLibraryProject(project, unpublished ? QStringLiteral("publish_document") : QStringLiteral("unpublish_document"), document);
                    });
                }
                action(item->data(0, LibraryMissingSourcesRole).toStringList().isEmpty()
                           ? i18n("Remove…") : i18n("Remove missing document…"),
                       QStringLiteral("remove_document"));
                const bool unlinked = document.value(QStringLiteral("imported")).toBool()
                    || (sourcePath.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
                    || (sourcePath.isEmpty() && document.value(QStringLiteral("source_project")).toString().isEmpty());
                const bool editableSource = sourcePath.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)
                    || sourcePath.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || sourcePath.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
                if (!unlinked && editableSource && !document.value(QStringLiteral("document_id")).toString().isEmpty()) {
                    auto *move = menu.addAction(i18n("Move to project…"));
                    move->setObjectName(QStringLiteral("libraryMoveSourceDocumentAction"));
                    move->setEnabled(!m_syncOperationRunning && !document.value(QStringLiteral("pending_move")).toBool());
                    connect(move, &QAction::triggered, this, [this, project, document] { moveLibrarySourceDocument(project, document); });
                }
                if (unlinked) {
                    auto *move = menu.addAction(i18n("Move PDF to different project"));
                    move->setObjectName(QStringLiteral("libraryMoveDocumentAction"));
                    move->setEnabled(!m_syncOperationRunning && !document.value(QStringLiteral("pending_move")).toBool());
                    connect(move, &QAction::triggered, this, [this, project, document] { moveLibraryDocument(project, document); });
                }
            }
            menu.exec(view->viewport()->mapToGlobal(position)); return;
        }
    }
    if (item && item->data(0, LibraryIdentityRole).toString().endsWith(QLatin1String("/documents"))) {
        auto *project = item->parent();
        const QString projectName = project->toolTip(0);
        const QString target = project->data(0, LibraryFilenameRole).toString();
        const QString pdfPath = project->data(0, Qt::UserRole).toString();
        if (project->data(0, LibraryKindRole).toString() == QLatin1String("source_linked")) {
            QMenu menu(view);
            addHtmlImport(menu, projectName, target);
            auto *import = menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Import document…"));
            import->setObjectName(QStringLiteral("libraryImportDocumentAction"));
            connect(import, &QAction::triggered, this, [this, projectName, target] {
                const QStringList paths = QFileDialog::getOpenFileNames(this, i18n("Import document"), {}, i18n("PDF documents (*.pdf)"));
                for (const QString &path : paths) {
                    QFile input(path);
                    if (!input.open(QIODevice::ReadOnly) || input.read(5) != QByteArrayLiteral("%PDF-")) {
                        KMessageBox::error(this, i18n("%1 is not a readable PDF.", QFileInfo(path).fileName()), i18n("Import document"));
                        continue;
                    }
                    input.close();
                    const QString digest = QString::fromLatin1(fileContentSha256(path).toHex());
                    QString filename = QFileInfo(path).fileName();
                    filename.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
                    if (filename.isEmpty() || !filename.at(0).isLetterOrNumber()) filename.prepend(QStringLiteral("document_"));
                    filename = QFileInfo(filename).completeBaseName() + QStringLiteral(".pdf");
                    const auto mutation = setProjectAttachment(m_docsRoot, projectName, path, filename, digest, QFileInfo(path).completeBaseName(), true);
                    if (!mutation.success) {
                        KMessageBox::error(this, mutation.error, i18n("Import document"));
                        continue;
                    }
                    if (mutation.changed) Q_EMIT projectAttachmentChangedLocally(target);
                }
                updateLibraryTree();
            });
            auto *website = menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Import from website…"));
            website->setObjectName(QStringLiteral("libraryImportWebsiteAction"));
            connect(website, &QAction::triggered, this, [this, projectName, target, website] { importLibraryWebsite({}, projectName, target, website->property("thoughtsImportLink").toString()); });
            auto *select = menu.addAction(i18n("Manage documents…"));
            select->setObjectName(QStringLiteral("libraryManageDocumentsAction"));
            select->setEnabled(!m_syncOperationRunning);
            connect(select, &QAction::triggered, this, [this, pdfPath] { showDocumentOrder(pdfPath, {}); });
            groupImports(menu, projectName);
            menu.exec(view->viewport()->mapToGlobal(position));
            return;
        }
    }
    QTreeWidgetItem *documentsGroup = item && item->data(0, LibraryIdentityRole).toString().endsWith(QLatin1String("/documents")) ? item : item ? item->parent() : nullptr;
    const bool canOrder = allowOrder && documentsGroup && documentsGroup->data(0, LibraryIdentityRole).toString().endsWith(QLatin1String("/documents"))
        && documentsGroup->parent()->data(0, LibraryKindRole).toString() == QLatin1String("source_linked");
    const QString orderPdf = canOrder ? documentsGroup->data(0, Qt::UserRole).toString() : QString();
    const QString selectedId = item ? item->data(0, LibraryCompositionIdRole).toString() : QString();
    const auto addOrderAction = [this, canOrder, orderPdf, selectedId](QMenu &menu) {
        if (!canOrder) { return; }
        auto *action = menu.addAction(i18n("Manage documents…"));
        action->setObjectName(QStringLiteral("libraryManageDocumentsAction"));
        action->setEnabled(!m_syncOperationRunning);
        connect(action, &QAction::triggered, this, [this, orderPdf, selectedId] { showDocumentOrder(orderPdf, selectedId); });
    };
    if (item && sourcePath.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) {
        m_libraryTree->setCurrentItem(item);
        auto *project = item;
        while (project->parent()) { project = project->parent(); }
        const QString projectName = project->toolTip(0);
        const QString projectRoot = QDir(m_docsRoot).filePath(projectName);
        if (project->data(0, LibraryKindRole).toString() != QLatin1String("source_linked") || projectName.isEmpty() || QFileInfo(projectName).fileName() != projectName) {
            QMenu menu(view);
            addSourceOpenWith(menu);
            menu.exec(view->viewport()->mapToGlobal(position));
            return;
        }
        const QString canonical = QFileInfo(sourcePath).canonicalFilePath();
        const QString projectPdf = project->data(0, Qt::UserRole).toString();
        const QString target = project->data(0, LibraryFilenameRole).toString();
        const auto findDocument = [&, canonical, projectPdf]() -> QTreeWidgetItem * {
            QTreeWidgetItem *documents = nullptr;
            for (int i = 0; i < m_libraryTree->topLevelItemCount(); ++i) {
                auto *current = m_libraryTree->topLevelItem(i);
                if (current->data(0, Qt::UserRole).toString() == projectPdf) { documents = current->child(0); break; }
            }
            for (int i = 0; documents && i < documents->childCount(); ++i) {
                auto *entry = documents->child(i);
                if (!canonical.isEmpty() && entry->data(0, LibraryDocumentSourceRole).toString() == canonical) { return entry; }
            }
            return nullptr;
        };
        const bool included = findDocument() != nullptr;
        QMenu menu(view);
        addSourceOpenWith(menu);
        addOrderAction(menu);
        auto *action = menu.addAction(included ? i18n("Show in Documents") : i18n("Add to final PDF"));
        action->setObjectName(included ? QStringLiteral("libraryShowInDocumentsAction") : QStringLiteral("libraryAddToFinalPdfAction"));
        connect(action, &QAction::triggered, this, [&, included, projectRoot, sourcePath] {
            if (included) {
                if (auto *entry = findDocument()) {
                    entry->parent()->setExpanded(true);
                    m_libraryTree->setCurrentItem(entry);
                    m_libraryTree->scrollToItem(entry);
                }
                return;
            }
            const QString error = addStandaloneDocument(projectRoot, sourcePath);
            if (!error.isEmpty()) { KMessageBox::error(this, error, i18n("Add to final PDF")); return; }
            const auto standalone = projectStandaloneSources(projectRoot);
            for (TrackedCommentDocument &document : m_documents) {
                if (QDir(m_docsRoot).filePath(document.libraryProject) == projectRoot) document.standaloneSources = standalone;
            }
            updateLibraryTree();
            if (auto *entry = findDocument()) {
                entry->parent()->setExpanded(true);
                m_libraryTree->setCurrentItem(entry);
                m_libraryTree->scrollToItem(entry);
            }
            Q_EMIT projectAttachmentChangedLocally(target);
        });
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    if (!sourcePath.isEmpty()) {
        QMenu menu(view);
        addSourceOpenWith(menu);
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    const QUrl overleafUrl(item ? item->data(0, LibraryOverleafUrlRole).toString() : QString());
    if (overleafUrl.isValid() && !overleafUrl.isEmpty()) {
        QMenu menu(view);
        addOrderAction(menu);
        QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18n("Open Overleaf"));
        connect(open, &QAction::triggered, this, [overleafUrl] { QDesktopServices::openUrl(overleafUrl); });
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    const QString attachmentProject = item ? item->data(0, LibraryAttachmentProjectRole).toString() : QString();
    if (item && !attachmentProject.isEmpty()) {
        const QString pdfPath = item->data(0, Qt::UserRole).toString();
        const QString filename = item->data(0, LibraryFilenameRole).toString();
        const QString sha256 = item->data(0, LibrarySha256Role).toString();
        const QString title = item->data(0, LibraryDisplayNameRole).toString();
        const QString target = item->data(0, LibraryAttachmentTargetRole).toString();
        m_libraryTree->setCurrentItem(item);
        QMenu menu(view);
        addOrderAction(menu);
        QAction *openAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open PDF"));
        openAction->setEnabled(!pdfPath.isEmpty());
        connect(openAction, &QAction::triggered, this, [this, pdfPath] { Q_EMIT documentActivated(pdfPath); });
        QAction *detachAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Detach from Project…"));
        detachAction->setObjectName(QStringLiteral("libraryDetachProjectAction"));
        connect(detachAction, &QAction::triggered, this, [this, attachmentProject, target, pdfPath, filename, sha256, title] {
            if (KMessageBox::questionTwoActions(this,
                                                i18n("Detach %1 from %2?\n\nThoughts will save this change locally and preserve the standalone Library PDF and its annotations. Press Sync when you want to publish the change to the server; the server will then perform the required rebuild.", title, attachmentProject),
                                                i18n("Detach from Project"),
                                                KGuiItem(i18n("Detach"), QStringLiteral("edit-delete")),
                                                KStandardGuiItem::cancel())
                != KMessageBox::PrimaryAction) {
                return;
            }
            const ProjectAttachmentMutationResult mutation = setProjectAttachment(m_docsRoot, attachmentProject, pdfPath, filename, sha256, title, false);
            if (!mutation.success) {
                KMessageBox::error(this, mutation.error, i18n("Detach from Project"));
                return;
            }
            if (!mutation.changed) {
                KMessageBox::information(this, i18n("This PDF is no longer attached to the selected project."), i18n("Detach from Project"));
                return;
            }
            updateLibraryTree();
            Q_EMIT projectAttachmentChangedLocally(target);
        });
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    if (item && !selectedId.isEmpty()) {
        auto *project = item;
        while (project->parent()) { project = project->parent(); }
        const QString projectName = project->toolTip(0);
        const QString projectRoot = QDir(m_docsRoot).filePath(projectName);
        const QString pdfPath = project->data(0, Qt::UserRole).toString();
        const QString target = project->data(0, LibraryFilenameRole).toString();
        const QString title = item->text(0);
        const int page = item->data(0, LibraryPageRole).toInt();
        QMenu menu(view);
        addOrderAction(menu);
        auto *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open PDF"));
        open->setEnabled(page > 0 && !pdfPath.isEmpty());
        connect(open, &QAction::triggered, this, [this, pdfPath, page] { Q_EMIT documentPageActivated(pdfPath, page); });
        const auto document = std::find_if(m_documents.cbegin(), m_documents.cend(), [&](const auto &entry) { return entry.pdfPath == pdfPath; });
        const auto catalogueProject = project->data(0, Qt::UserRole + 140).value<QJsonObject>();
        if (!catalogueProject.isEmpty()) {
            auto *remove = menu.addAction(i18n("Remove from final PDF…"));
            connect(remove, &QAction::triggered, this, [this, catalogueProject, selectedId] {
                editLibraryProject(catalogueProject, QStringLiteral("remove_document"), {{QStringLiteral("id"), selectedId}});
            });
        } else if (document != m_documents.cend()) {
            addComponentManagementActions(&menu, *document, selectedId, title, page);
        }
        menu.exec(view->viewport()->mapToGlobal(position));
        return;
    }
    while (item && item->parent()) {
        item = item->parent();
    }
    if (!item) {
        return;
    }
    const QString pdfPath = item->data(0, Qt::UserRole).toString();
    const QString filename = item->data(0, LibraryFilenameRole).toString();
    const QString sha256 = item->data(0, LibrarySha256Role).toString();
    const QString kind = item->data(0, LibraryKindRole).toString();
    const QString displayName = item->data(0, LibraryDisplayNameRole).toString();
    if (pdfPath.isEmpty()) {
        return;
    }

    m_libraryTree->setCurrentItem(item);
    QMenu menu(view);
        addOrderAction(menu);
    QAction *openAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open PDF"));
    connect(openAction, &QAction::triggered, this, [this, pdfPath] { Q_EMIT documentActivated(pdfPath); });
    QAction *informationAction = menu.addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Information…"));
    connect(informationAction, &QAction::triggered, this, [this, pdfPath] { showDocumentInformation(pdfPath); });
    if (!filename.isEmpty() && !sha256.isEmpty() && (kind == QLatin1String("source_linked") || kind == QLatin1String("unlinked"))) {
        menu.addSeparator();
        QAction *renameAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
        renameAction->setObjectName(QStringLiteral("libraryRenameAction"));
        connect(renameAction, &QAction::triggered, this, [this, filename, sha256, displayName] {
            bool accepted = false;
            const QString renamedDisplayName = QInputDialog::getText(this, i18n("Rename Document"), i18n("Display name:"), QLineEdit::Normal, displayName, &accepted).trimmed();
            if (!accepted) {
                return;
            }
            if (renamedDisplayName.isEmpty() || renamedDisplayName.size() > 200 || std::any_of(renamedDisplayName.cbegin(), renamedDisplayName.cend(), [](QChar character) { return character.unicode() < 32 || character.unicode() == 127; })) {
                KMessageBox::error(this, i18n("Enter a name between 1 and 200 characters without line breaks."), i18n("Rename Document"));
                return;
            }
            if (renamedDisplayName != displayName) {
                Q_EMIT libraryDisplayNameRequested(filename, sha256, renamedDisplayName);
            }
        });
        if (kind == QLatin1String("unlinked")) {
            QAction *attachAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-send")), i18n("Attach to Project…"));
            attachAction->setObjectName(QStringLiteral("libraryAttachProjectAction"));
            connect(attachAction, &QAction::triggered, this, [this, pdfPath, filename, sha256, displayName] {
                struct Candidate {
                    QString label;
                    QString project;
                    QString target;
                };
                QList<Candidate> candidates;
                for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
                    // Project attachments exist only in docs-* folders.
                    if (document.libraryKind != QLatin1String("source_linked") || !Thoughts::SourceOwner::isConnectedFolder(document.libraryProject) || document.libraryFilename.isEmpty()) {
                        continue;
                    }
                    candidates.append({i18n("%1 — %2", document.displayName, document.libraryProject), document.libraryProject, document.libraryFilename});
                }
                std::sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
                    return left.label.localeAwareCompare(right.label) < 0;
                });
                if (candidates.isEmpty()) {
                    KMessageBox::information(this, i18n("No source-backed Thoughts project is available."), i18n("Attach to Project"));
                    return;
                }
                QStringList labels;
                for (const Candidate &candidate : std::as_const(candidates)) {
                    labels.append(candidate.label);
                }
                bool accepted = false;
                const QString selected = QInputDialog::getItem(this, i18n("Attach to Project"), i18n("Project:"), labels, 0, false, &accepted);
                if (!accepted) {
                    return;
                }
                const auto candidate = std::find_if(candidates.cbegin(), candidates.cend(), [&selected](const Candidate &value) { return value.label == selected; });
                if (candidate == candidates.cend()) {
                    return;
                }
                const QString title = displayName.trimmed().isEmpty() ? QFileInfo(filename).completeBaseName() : displayName.trimmed();
                if (KMessageBox::questionTwoActions(this,
                                                    i18n("Attach %1 to %2?\n\nThoughts will save this PDF as supplementary material locally. Press Sync when you want to publish the change to the server; the server will then perform the required rebuild of %3.", title, candidate->project, candidate->target),
                                                    i18n("Attach to Project"),
                                                    KGuiItem(i18n("Attach"), QStringLiteral("document-send")),
                                                    KStandardGuiItem::cancel())
                    != KMessageBox::PrimaryAction) {
                    return;
                }
                const ProjectAttachmentMutationResult mutation = setProjectAttachment(m_docsRoot, candidate->project, pdfPath, filename, sha256, title, true);
                if (!mutation.success) {
                    KMessageBox::error(this, mutation.error, i18n("Attach to Project"));
                    return;
                }
                if (!mutation.changed) {
                    KMessageBox::information(this, i18n("This PDF is already attached to the selected project."), i18n("Attach to Project"));
                    return;
                }
                updateLibraryTree();
                Q_EMIT projectAttachmentChangedLocally(candidate->target);
            });
            QAction *removeAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Remove from Library…"));
            removeAction->setObjectName(QStringLiteral("libraryRemoveAction"));
            connect(removeAction, &QAction::triggered, this, [this, pdfPath, filename, sha256, displayName] {
                Q_EMIT libraryRemovalRequested(pdfPath, filename, sha256, displayName);
            });
        }
    }
    menu.exec(view->viewport()->mapToGlobal(position));
}

void CommentsOverview::addComponentManagementActions(QMenu *menu, const TrackedCommentDocument &document, const QString &selectedId, const QString &title, int page)
{
    const QString projectName = document.libraryProject;
    const QString projectRoot = QDir(m_docsRoot).filePath(projectName);
    const QString pdfPath = document.pdfPath;
    const QString target = document.libraryFilename;
    auto *info = menu->addAction(QIcon::fromTheme(QStringLiteral("dialog-information")), i18n("Information…"));
    info->setObjectName(QStringLiteral("libraryDocumentInformationAction"));
    connect(info, &QAction::triggered, this, [this, title, selectedId, pdfPath, page] {
        KMessageBox::information(this, i18n("Document: %1\nSource: %2\nFinal PDF: %3\nFirst page: %4", title, selectedId, pdfPath, page > 0 ? QString::number(page) : i18n("Not built yet")), i18n("Document Information"));
    });
    if (document.libraryKind == QLatin1String("source_linked") && !projectName.isEmpty() && QFileInfo(projectName).fileName() == projectName) {
        menu->addSeparator();
        auto *rename = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
        rename->setObjectName(QStringLiteral("libraryRenameAction"));
        connect(rename, &QAction::triggered, this, [this, projectRoot, selectedId, title, target] {
            bool accepted = false;
            const QString name = QInputDialog::getText(this, i18n("Rename Document"), i18n("Display name:"), QLineEdit::Normal, title, &accepted).trimmed();
            if (!accepted || name == title) { return; }
            if (name.isEmpty() || name.size() > 200 || std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; })) {
                KMessageBox::error(this, i18n("Enter a name between 1 and 200 characters without line breaks.")); return;
            }
            QJsonObject options = documentOptions(projectRoot);
            if (options.isEmpty()) { KMessageBox::error(this, i18n("The document settings could not be read.")); return; }
            QJsonObject titles = options.value(QStringLiteral("titles")).toObject();
            titles.insert(selectedId, name); options.insert(QStringLiteral("titles"), titles);
            const QString error = saveDocumentOptions(projectRoot, options);
            if (!error.isEmpty()) { KMessageBox::error(this, error); return; }
            refreshDocumentNavigation();
            Q_EMIT projectAttachmentChangedLocally(target);
        });
        auto *remove = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete…"));
        remove->setObjectName(QStringLiteral("libraryDeleteDocumentAction"));
        connect(remove, &QAction::triggered, this, [this, pdfPath, selectedId, title] {
            deleteDocumentComponent(pdfPath, selectedId, title);
        });
    }
}

bool CommentsOverview::addOutlineDocumentActions(QMenu *menu, const QString &pdfPath, int page, const QString &outlineTitle)
{
    if (!menu || page < 1) return false;
    const QString canonical = QFileInfo(pdfPath).canonicalFilePath();
    if (!canonical.isEmpty() && std::none_of(m_documents.cbegin(), m_documents.cend(), [&](const auto &entry) {
            return QFileInfo(entry.pdfPath).canonicalFilePath() == canonical;
        })) {
        // A PDF can be opened directly before the Overview's asynchronous catalogue has loaded.
        if (const auto document = loadTrackedCommentDocument(m_docsRoot, pdfPath, m_fedoraProjections)) m_documents.append(*document);
    }
    const auto document = std::find_if(m_documents.cbegin(), m_documents.cend(), [&](const auto &entry) {
        return !canonical.isEmpty() && QFileInfo(entry.pdfPath).canonicalFilePath() == canonical;
    });
    if (document == m_documents.cend()) return false;
    const auto normalized = [](QString text) { return text.replace(QLatin1Char('_'), QLatin1Char(' ')).simplified().toCaseFolded(); };
    const QString projectRoot = QDir(m_docsRoot).filePath(document->libraryProject);
    QJsonObject matched;
    // Refresh and verify the component map against this exact PDF before exposing document mutations.
    for (const auto &value : libraryDocumentSegments(m_docsRoot, *document)) {
        const auto segment = value.toObject();
        const QString id = segment.value(QStringLiteral("source_id")).toString();
        if (id.isEmpty() || segment.value(QStringLiteral("start_page")).toInt() != page || documentExcluded(projectRoot, id)) continue;
        const QString originalTitle = librarySegmentTitle(segment);
        const QString currentTitle = documentTitle(projectRoot, id, originalTitle);
        if (normalized(outlineTitle) != normalized(originalTitle) && normalized(outlineTitle) != normalized(currentTitle)) continue;
        if (!matched.isEmpty()) return false;
        matched = segment;
    }
    if (matched.isEmpty()) return false;
    const QString id = matched.value(QStringLiteral("source_id")).toString();
    menu->addSeparator();
    addComponentManagementActions(menu, *document, id, documentTitle(projectRoot, id, librarySegmentTitle(matched)), page);
    return true;
}

void CommentsOverview::refreshDocumentNavigation()
{
    // Settings edits need a new worker snapshot, never a synchronous manifest
    // reload from the navigation painter.
    startFullReload(true, QStringLiteral("project_settings_changed"));
}

void CommentsOverview::deleteDocumentComponent(const QString &pdfPath, const QString &componentId, const QString &title)
{
    if (m_operationPending || m_pendingDocumentRemoval || ((m_editorDirty || m_chatGptUrlDirty) && !resolveDirtyEditor())) { return; }
    const auto document = std::find_if(m_documents.cbegin(), m_documents.cend(), [&](const TrackedCommentDocument &entry) { return entry.pdfPath == pdfPath; });
    if (document == m_documents.cend() || document->libraryKind != QLatin1String("source_linked") || document->libraryProject.isEmpty()) { return; }
    const QString projectRoot = QDir(m_docsRoot).filePath(document->libraryProject);
    if (documentOptions(projectRoot).isEmpty()) { KMessageBox::error(this, i18n("The document settings could not be read.")); return; }
    const auto absoluteSource = [this](const QString &path) {
        if (path.isEmpty()) { return QString(); }
        const QFileInfo info(QDir(m_docsRoot).absoluteFilePath(path));
        const QString canonical = info.canonicalFilePath();
        return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
    };
    QSet<QString> ownedSources {absoluteSource(componentId)};
    QSet<QString> sharedSources;
    int startPage = 0;
    int pageCount = 0;
    for (const auto &value : document->librarySegments) {
        const QJsonObject segment = value.toObject();
        const bool selected = segment.value(QStringLiteral("source_id")).toString() == componentId;
        QSet<QString> sources {absoluteSource(segment.value(QStringLiteral("primary_source_file")).toString(segment.value(QStringLiteral("source_id")).toString()))};
        for (const auto &input : segment.value(QStringLiteral("inputs")).toArray()) {
            sources.insert(absoluteSource(input.toObject().value(QStringLiteral("source_file")).toString()));
        }
        sources.remove(QString());
        if (selected) {
            ownedSources.unite(sources);
            startPage = segment.value(QStringLiteral("start_page")).toInt();
            pageCount = segment.value(QStringLiteral("page_count")).toInt();
        } else { sharedSources.unite(sources); }
    }
    // A shared source belongs to more than this component; preserve its comments.
    ownedSources.subtract(sharedSources);
    const QFileInfo sourceInfo(QDir(m_docsRoot).absoluteFilePath(componentId));
    const QString canonicalProject = QFileInfo(projectRoot).canonicalFilePath();
    if (sourceInfo.isSymLink() || !sourceInfo.isFile() || canonicalProject.isEmpty()
        || !sourceInfo.canonicalFilePath().startsWith(canonicalProject + QLatin1Char('/'))
        || !ownedSources.contains(sourceInfo.canonicalFilePath())) {
        KMessageBox::error(this, i18n("The document source is missing, shared, or outside this project. The document was kept."));
        return;
    }
    QSet<QString> ids;
    for (const TrackedComment &comment : document->comments) {
        if (comment.agentProposal) { continue; }
        bool belongs = !comment.sourceFile.isEmpty() && ownedSources.contains(absoluteSource(comment.sourceFile));
        // Page-only comments require a location verified against this publication.
        if (comment.sourceFile.isEmpty() && startPage > 0) {
            belongs = !comment.annotationPdfSha256.isEmpty() && comment.annotationPdfSha256 == document->librarySha256
                && comment.page >= startPage && comment.page - startPage < pageCount;
            for (const auto &value : comment.pdfLocations) {
                const QJsonObject location = value.toObject();
                const int page = location.value(QStringLiteral("page")).toInt();
                belongs |= location.value(QStringLiteral("pdf_filename")).toString() == document->libraryFilename
                    && location.value(QStringLiteral("pdf_sha256")).toString() == document->librarySha256
                    && page >= startPage && page - startPage < pageCount;
            }
        }
        if (belongs) { ids.insert(comment.id); }
    }
    bool expanded;
    do {
        expanded = false;
        for (const TrackedComment &comment : document->comments) {
            if (!comment.agentProposal && !ids.contains(comment.id) && (ids.contains(comment.parentId) || ids.contains(comment.threadRootId))) {
                ids.insert(comment.id); expanded = true;
            }
        }
    } while (expanded);
    QList<TrackedCommentTarget> targets;
    PendingDocumentRemoval pending {projectRoot, componentId, document->libraryFilename, {}};
    for (const TrackedComment &comment : document->comments) {
        if (ids.contains(comment.id)) targets.append({pdfPath, comment.id, comment.page, -1});
    }
    if (KMessageBox::questionTwoActions(this,
            i18np("Delete “%2” and its %1 associated comment?\n\nThe source file and all its agent proposals will be moved to Trash. Press Sync to publish the deletion and rebuild the PDF.",
                  "Delete “%2” and its %1 associated comments?\n\nThe source file and all its agent proposals will be moved to Trash. Press Sync to publish the deletion and rebuild the PDF.", ids.size(), title),
            i18n("Delete Document"), KGuiItem(i18n("Delete"), QStringLiteral("edit-delete")), KStandardGuiItem::cancel()) != KMessageBox::PrimaryAction) { return; }
    m_pendingDocumentRemoval = pending;
    if (!targets.isEmpty()) {
        setOperationPending(true);
        Q_EMIT commentsDeleteRequested(targets);
    } else { finishDocumentRemoval(); }
}

void CommentsOverview::finishDocumentRemoval()
{
    if (!m_pendingDocumentRemoval) { return; }
    const auto pending = *m_pendingDocumentRemoval;
    m_pendingDocumentRemoval.reset();
    QJsonObject options = documentOptions(pending.projectRoot);
    if (options.isEmpty()) { KMessageBox::error(this, i18n("The document settings could not be read.")); return; }
    QJsonArray excluded = options.value(QStringLiteral("excluded_documents")).toArray();
    if (!excluded.contains(pending.componentId)) { excluded.append(pending.componentId); }
    options.insert(QStringLiteral("excluded_documents"), excluded);
    const QString sourcePath = QDir(m_docsRoot).absoluteFilePath(pending.componentId);
    const QString orderPath = QDir(pending.projectRoot).filePath(QStringLiteral("tex/docs_order_manifest.txt"));
    QFile orderInput(orderPath);
    QByteArray previousOrder;
    QByteArray updatedOrder;
    const bool hasOrder = orderInput.exists() && Thoughts::DocumentBuildSelection::rawRead(QDir(pending.projectRoot).filePath(QStringLiteral("thoughts-project.json"))).value(QStringLiteral("schema_version")).toInt() != 2;
    if (hasOrder) {
        if (!orderInput.open(QIODevice::ReadOnly)) {
            KMessageBox::error(this, i18n("The document order could not be read. The document was kept."));
            return;
        }
        previousOrder = orderInput.readAll();
        orderInput.close();
        const bool standalone = QDir(pending.projectRoot).relativeFilePath(sourcePath)
            == QStringLiteral("tex/source/") + QFileInfo(sourcePath).fileName();
        QList<QByteArray> keptLines;
        for (const auto &line : previousOrder.split('\n')) {
            if (standalone && line.trimmed() == QFileInfo(sourcePath).fileName().toUtf8()) { continue; }
            keptLines.append(line);
        }
        updatedOrder = keptLines.join('\n');
    }
    const auto writeOrder = [&orderPath](const QByteArray &bytes) {
        QSaveFile file(orderPath);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    QString trashedPath;
    if (!QFile::moveToTrash(sourcePath, &trashedPath)) {
        KMessageBox::error(this, i18n("The document source could not be moved to Trash. The document was kept."));
        return;
    }
    if (hasOrder && !writeOrder(updatedOrder)) {
        const bool restored = QFile::rename(trashedPath, sourcePath);
        KMessageBox::error(this, restored ? i18n("The document order could not be saved. The document was kept.")
                                         : i18n("The document order could not be saved. Its source is in Trash at %1.", trashedPath));
        return;
    }
    const QString error = saveDocumentOptions(pending.projectRoot, options);
    if (!error.isEmpty()) {
        const bool orderRestored = !hasOrder || writeOrder(previousOrder);
        const bool restored = QFile::rename(trashedPath, sourcePath) && orderRestored;
        KMessageBox::error(this, restored ? error : i18n("The document settings could not be saved. Its source is in Trash at %1.", trashedPath));
        return;
    }
    QList<TrackedCommentTarget> proposals;
    for (const auto &document : std::as_const(m_documents)) {
        for (const auto &comment : document.comments) {
            if (comment.agentProposal && proposalSourceExcluded(m_docsRoot, comment.sourceFile)) {
                proposals.append({document.pdfPath, comment.id, comment.page, -1});
            }
        }
    }
    removeTargetsFromModel(proposals);
    refreshDocumentNavigation();
    Q_EMIT projectAttachmentChangedLocally(pending.target);
}

void CommentsOverview::showDocumentInformation(const QString &pdfPath)
{
    const auto found = std::find_if(m_documents.cbegin(), m_documents.cend(), [&pdfPath](const TrackedCommentDocument &document) { return document.pdfPath == pdfPath; });
    if (found == m_documents.cend()) {
        return;
    }
    const TrackedCommentDocument &document = *found;
    const bool sourceLinked = document.libraryKind == QLatin1String("source_linked");

    auto *dialog = new QDialog(this, Qt::Window);
    dialog->setObjectName(QStringLiteral("documentInformationDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowTitle(i18n("Document Information"));
    dialog->setWindowIcon(QIcon::fromTheme(QStringLiteral("dialog-information")));
    dialog->setMinimumWidth(660);
    dialog->setProperty("sourceMonitoringPdfPath", pdfPath);

    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto addValue = [dialog, form](const QString &title, const QString &value, const QString &objectName) {
        auto *label = new QLabel(value, dialog);
        label->setObjectName(objectName);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        form->addRow(title, label);
    };
    addValue(i18n("Document:"), document.displayName, QStringLiteral("documentInformationName"));
    addValue(i18n("Filename:"), QFileInfo(document.pdfPath).fileName(), QStringLiteral("documentInformationFilename"));
    addValue(i18n("Project:"), document.libraryProject.isEmpty() ? i18n("No build project") : document.libraryProject, QStringLiteral("documentInformationProject"));
    addValue(i18n("Final PDF:"), formattedDocumentBytes(document.pdfSizeBytes), QStringLiteral("documentInformationPdfSize"));

    QString buildInputs;
    if (!sourceLinked) {
        buildInputs = i18n("Not applicable — no docs-* build project.");
    } else if (!document.verifiedBuildInputsAvailable) {
        buildInputs = i18n("Unavailable — this publication has no verified build-closure record.");
    } else {
        buildInputs = i18np("%1 file · %2", "%1 files · %2", document.verifiedBuildInputCount, formattedDocumentBytes(document.verifiedBuildInputBytes));
    }
    if (!sourceLinked) {
        addValue(i18n("Sources and monitoring:"), buildInputs, QStringLiteral("documentInformationBuildInputs"));
    } else {
        auto *monitoringRow = new QWidget(dialog);
        auto *monitoringLayout = new QHBoxLayout(monitoringRow);
        monitoringLayout->setContentsMargins(0, 0, 0, 0);
        monitoringLayout->setSpacing(8);
        auto *monitoringSummary = new QLabel(i18n("Checking local monitoring…"), monitoringRow);
        monitoringSummary->setObjectName(QStringLiteral("documentInformationBuildInputs"));
        monitoringSummary->setTextFormat(Qt::PlainText);
        monitoringSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        monitoringSummary->setWordWrap(true);
        monitoringLayout->addWidget(monitoringSummary, 1);
        auto *viewMonitoring = new QPushButton(i18n("View monitored sources…"), monitoringRow);
        viewMonitoring->setObjectName(QStringLiteral("documentInformationViewMonitoring"));
        viewMonitoring->setAutoDefault(false);
        viewMonitoring->setEnabled(false);
        connect(viewMonitoring, &QPushButton::clicked, dialog, [this, dialog] {
            const QVariantMap information = dialog->property("sourceMonitoringInformation").toMap();
            if (information.value(QStringLiteral("schema_version")).toInt() != 1) {
                return;
            }
            auto *details = new QDialog(dialog, Qt::Window);
            details->setObjectName(QStringLiteral("sourceMonitoringDetailsDialog"));
            details->setAttribute(Qt::WA_DeleteOnClose);
            details->setWindowModality(Qt::WindowModal);
            details->setWindowTitle(i18n("Monitored Sources"));
            details->resize(840, 580);
            auto *detailsLayout = new QVBoxLayout(details);
            auto *explanation = new QLabel(
                i18n("Thoughts watches these folders only to detect changes to the listed server inputs and eligible new source files. Folder contents are not uploaded automatically."),
                details);
            explanation->setWordWrap(true);
            explanation->setTextFormat(Qt::PlainText);
            detailsLayout->addWidget(explanation);
            auto *tree = new QTreeWidget(details);
            tree->setObjectName(QStringLiteral("sourceMonitoringTree"));
            tree->setColumnCount(3);
            tree->setHeaderLabels({i18n("Source or monitored folder"), i18n("Monitoring"), i18n("State")});
            tree->setRootIsDecorated(true);
            tree->setUniformRowHeights(true);
            for (const QVariant &folderValue : information.value(QStringLiteral("folders")).toList()) {
                const QVariantMap folder = folderValue.toMap();
                const QString folderPath = folder.value(QStringLiteral("path")).toString();
                const QString mode = folder.value(QStringLiteral("mode")).toString();
                const QString folderKind = folder.value(QStringLiteral("shared")).toBool()
                    ? i18n("Shared authoring root")
                    : (mode == QLatin1String("discovery")
                           ? i18n("Authoring root")
                           : i18n("Atomic-save sensor"));
                const QString folderState = folder.value(QStringLiteral("exists")).toBool()
                    ? i18n("Watching")
                    : i18n("Waiting for folder");
                auto *folderItem = new QTreeWidgetItem(tree, {folderPath, folderKind, folderState});
                folderItem->setIcon(0, QIcon::fromTheme(QStringLiteral("folder")));
                for (const QVariant &inputValue : folder.value(QStringLiteral("inputs")).toList()) {
                    const QVariantMap input = inputValue.toMap();
                    const QString inputPath = input.value(QStringLiteral("path")).toString();
                    const QString relative = inputPath.startsWith(folderPath + QLatin1Char('/'))
                        ? inputPath.mid(folderPath.size() + 1)
                        : inputPath;
                    QString kind = input.value(QStringLiteral("role")).toString();
                    if (kind.isEmpty()) {
                        kind = input.value(QStringLiteral("source_kind")).toString();
                    }
                    QString state;
                    if (input.value(QStringLiteral("pending")).toBool()) {
                        state = input.value(QStringLiteral("action")).toString()
                                == QLatin1String("delete")
                            ? i18n("Pending deletion")
                            : i18n("Pending local change");
                    } else if (input.value(QStringLiteral("source_kind")).toString()
                               == QLatin1String("build_input")) {
                        state = i18n("Server build input");
                    } else {
                        state = i18n("Server source");
                    }
                    auto *inputItem = new QTreeWidgetItem(folderItem, {relative, kind, state});
                    inputItem->setIcon(0, QIcon::fromTheme(QStringLiteral("text-x-generic")));
                    inputItem->setData(0, Qt::UserRole, inputPath);
                }
                folderItem->setExpanded(true);
            }
            tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
            tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
            detailsLayout->addWidget(tree, 1);
            auto *detailsButtons = new QDialogButtonBox(QDialogButtonBox::Close, details);
            auto *open = detailsButtons->addButton(i18n("Open in VS Code"), QDialogButtonBox::ActionRole);
            open->setObjectName(QStringLiteral("sourceMonitoringOpenSource"));
            open->setEnabled(false);
            const auto updateOpen = [tree, open] {
                open->setEnabled(tree->currentItem()
                                 && !tree->currentItem()->data(0, Qt::UserRole).toString().isEmpty());
            };
            connect(tree, &QTreeWidget::currentItemChanged, details, [updateOpen](QTreeWidgetItem *, QTreeWidgetItem *) { updateOpen(); });
            const auto openCurrent = [this, tree] {
                if (!tree->currentItem()) {
                    return;
                }
                const QString relative = tree->currentItem()->data(0, Qt::UserRole).toString();
                if (!relative.isEmpty()) {
                    Q_EMIT openSourceRequested(QDir(m_docsRoot).filePath(relative), 1);
                }
            };
            connect(open, &QPushButton::clicked, details, openCurrent);
            connect(tree, &QTreeWidget::itemDoubleClicked, details, [openCurrent](QTreeWidgetItem *, int) { openCurrent(); });
            connect(detailsButtons, &QDialogButtonBox::rejected, details, &QDialog::close);
            detailsLayout->addWidget(detailsButtons);
            details->open();
        });
        monitoringLayout->addWidget(viewMonitoring);
        form->addRow(i18n("Sources and monitoring:"), monitoringRow);
    }

    if (sourceLinked && Thoughts::SourceOwner::isDefaultFolder(document.libraryProject)) {
        // Shared project styles exist only in docs-* folders.
        addValue(i18n("Project style:"), i18n("None (default source folder)"), QStringLiteral("documentInformationProjectStyle"));
    } else if (sourceLinked && !document.libraryProject.isEmpty()) {
        const DocumentStyleInformation styleInformation = inspectDocumentStyleInformation(
            m_docsRoot,
            QFileInfo(document.pdfPath).fileName(),
            document.librarySha256,
            document.libraryProject);
        const auto showPublishedSnapshot = [dialog](const DocumentStyleResourceStatus &resource,
                                                    const QString &title,
                                                    const QString &objectName) {
            QByteArray contents;
            QString error;
            if (!readFrozenDocumentStyleSnapshot(resource, &contents, &error)) {
                KMessageBox::error(dialog, error, i18n("View Published Style"));
                return;
            }
            auto *snapshot = new QDialog(dialog, Qt::Window);
            snapshot->setObjectName(objectName);
            snapshot->setAttribute(Qt::WA_DeleteOnClose);
            snapshot->setWindowModality(Qt::WindowModal);
            snapshot->setWindowTitle(title);
            snapshot->resize(800, 560);
            auto *snapshotLayout = new QVBoxLayout(snapshot);
            auto *identity = new QLabel(
                i18n("Read-only snapshot used by the published PDF.\n%1\nSHA-256: %2",
                     resource.relativePath,
                     resource.publishedSha256),
                snapshot);
            identity->setTextFormat(Qt::PlainText);
            identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
            identity->setWordWrap(true);
            snapshotLayout->addWidget(identity);
            auto *viewer = new QPlainTextEdit(snapshot);
            viewer->setObjectName(objectName + QStringLiteral("Text"));
            viewer->setReadOnly(true);
            viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
            viewer->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            viewer->setPlainText(QString::fromUtf8(contents));
            snapshotLayout->addWidget(viewer, 1);
            auto *snapshotButtons = new QDialogButtonBox(QDialogButtonBox::Close, snapshot);
            connect(snapshotButtons, &QDialogButtonBox::rejected, snapshot, &QDialog::close);
            snapshotLayout->addWidget(snapshotButtons);
            snapshot->open();
        };
        const auto addStyleRow = [this,
                                  dialog,
                                  form,
                                  pdfPath,
                                  &styleInformation,
                                  &showPublishedSnapshot](const QString &title,
                                                          const DocumentStyleResourceStatus &resource,
                                                          bool projectOverride) {
            auto *row = new QWidget(dialog);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            auto *description = new QWidget(row);
            auto *descriptionLayout = new QVBoxLayout(description);
            descriptionLayout->setContentsMargins(0, 0, 0, 0);
            descriptionLayout->setSpacing(2);
            auto *path = new QLabel(resource.relativePath, description);
            path->setObjectName(projectOverride ? QStringLiteral("documentInformationProjectOverridePath")
                                                : QStringLiteral("documentInformationSharedStylePath"));
            path->setTextFormat(Qt::PlainText);
            path->setTextInteractionFlags(Qt::TextSelectableByMouse);
            path->setWordWrap(true);
            descriptionLayout->addWidget(path);
            auto *status = new QLabel(documentStyleStatusText(
                                          resource,
                                          styleInformation.publishedResourcesAvailable,
                                          projectOverride),
                                      description);
            status->setObjectName(projectOverride ? QStringLiteral("documentInformationProjectOverrideStatus")
                                                  : QStringLiteral("documentInformationSharedStyleStatus"));
            status->setTextFormat(Qt::PlainText);
            status->setTextInteractionFlags(Qt::TextSelectableByMouse);
            status->setWordWrap(true);
            QStringList identityDetails;
            if (!resource.localSha256.isEmpty()) {
                identityDetails.append(i18n("Current SHA-256: %1", resource.localSha256));
            }
            if (!resource.publishedSha256.isEmpty()) {
                identityDetails.append(i18n("Published SHA-256: %1", resource.publishedSha256));
            }
            if (!resource.fedoraSha256.isEmpty()) {
                identityDetails.append(i18n("Server SHA-256: %1", resource.fedoraSha256));
            }
            status->setToolTip(identityDetails.join(QLatin1Char('\n')));
            descriptionLayout->addWidget(status);
            rowLayout->addWidget(description, 1);

            auto *actions = new QWidget(row);
            auto *actionsLayout = new QHBoxLayout(actions);
            actionsLayout->setContentsMargins(0, 0, 0, 0);
            actionsLayout->setSpacing(6);
            if (resource.liveAvailable) {
                auto *open = new QPushButton(
                    projectOverride ? i18n("Open override") : i18n("Open shared style"),
                    actions);
                open->setObjectName(projectOverride ? QStringLiteral("documentInformationOpenProjectOverride")
                                                    : QStringLiteral("documentInformationOpenSharedStyle"));
                open->setAutoDefault(false);
                open->setToolTip(projectOverride
                                     ? i18n("Open the project-only style override in Visual Studio Code.")
                                     : i18n("Open the canonical shared style in Visual Studio Code."));
                connect(open, &QPushButton::clicked, this, [this, resource] {
                    Q_EMIT openSourceRequested(resource.absolutePath, 1);
                });
                actionsLayout->addWidget(open);
            }
            if (resource.frozenSnapshotAvailable
                && (!resource.liveAvailable || !resource.localMatchesPublished())) {
                auto *viewPublished = new QPushButton(i18n("View published"), actions);
                viewPublished->setObjectName(projectOverride ? QStringLiteral("documentInformationViewPublishedProjectOverride")
                                                             : QStringLiteral("documentInformationViewPublishedSharedStyle"));
                viewPublished->setAutoDefault(false);
                viewPublished->setToolTip(i18n("View the verified read-only style snapshot used by the published PDF."));
                connect(viewPublished, &QPushButton::clicked, dialog, [resource,
                                                                      projectOverride,
                                                                      showPublishedSnapshot] {
                    showPublishedSnapshot(
                        resource,
                        projectOverride ? i18n("Published Project Style Override")
                                        : i18n("Published Shared Style"),
                        projectOverride ? QStringLiteral("documentInformationPublishedProjectOverride")
                                        : QStringLiteral("documentInformationPublishedSharedStyle"));
                });
                actionsLayout->addWidget(viewPublished);
            }
            if (projectOverride && !resource.liveAvailable && !resource.publishedIdentityAvailable) {
                auto *create = new QPushButton(i18n("Create override…"), actions);
                create->setObjectName(QStringLiteral("documentInformationCreateProjectOverride"));
                create->setAutoDefault(false);
                create->setToolTip(i18n("Create a minimal project-only style file. Shared commands must not be redefined here."));
                connect(create, &QPushButton::clicked, dialog, [this, dialog, pdfPath] {
                    const auto documentIt = std::find_if(
                        m_documents.cbegin(),
                        m_documents.cend(),
                        [&pdfPath](const TrackedCommentDocument &candidate) {
                            return candidate.pdfPath == pdfPath;
                        });
                    if (documentIt == m_documents.cend() || documentIt->libraryProject.isEmpty()) {
                        return;
                    }
                    const QString detail = i18n(
                        "Create an empty project-specific style override for %1?\n\n"
                        "Use it only for project-specific packages and conventions. "
                        "It must not redefine commands from this project’s manual_style.sty.",
                        documentIt->libraryProject);
                    if (KMessageBox::questionTwoActions(
                            dialog,
                            detail,
                            i18n("Create Project Style Override"),
                            KGuiItem(i18n("Create override"), QStringLiteral("document-new")),
                            KStandardGuiItem::cancel())
                        != KMessageBox::PrimaryAction) {
                        return;
                    }
                    QString createdPath;
                    QString error;
                    if (!createProjectStyleOverride(
                            m_docsRoot,
                            documentIt->libraryProject,
                            &createdPath,
                            &error)) {
                        KMessageBox::error(dialog, error, i18n("Create Project Style Override"));
                        return;
                    }
                    Q_EMIT openSourceRequested(createdPath, 1);
                    dialog->close();
                    QTimer::singleShot(0, this, [this, pdfPath] {
                        showDocumentInformation(pdfPath);
                    });
                });
                actionsLayout->addWidget(create);
            }
            rowLayout->addWidget(actions);
            form->addRow(title, row);
        };
        addStyleRow(styleInformation.sharedStyle.kind == QLatin1String("project_style") ? i18n("Project style:") : i18n("Shared style:"), styleInformation.sharedStyle, false);
        addStyleRow(i18n("Project override:"), styleInformation.projectOverride, true);
    }
    layout->addLayout(form);

    auto *explanation = new QLabel(
        i18n("Server inputs are the exact files consumed by the published PDF. Monitored folders are local sensors for changes to those inputs and eligible new sources; Thoughts never uploads an entire folder automatically."),
        dialog);
    explanation->setObjectName(QStringLiteral("documentInformationExplanation"));
    explanation->setTextFormat(Qt::PlainText);
    explanation->setWordWrap(true);
    explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(explanation);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->setObjectName(QStringLiteral("documentInformationButtons"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->open();
    if (sourceLinked) {
        Q_EMIT sourceMonitoringInformationRequested(pdfPath);
    }
}

void CommentsOverview::applySourceMonitoringInformation(const QString &pdfPath,
                                                        const QVariantMap &information)
{
    if (!m_applyingLibraryMonitoringBatch && m_libraryMonitoringRunning) { m_libraryMonitoringRefreshAgain = true; }
    const QString status = information.value(QStringLiteral("status")).toString();
    m_sourceMonitoringInformation.insert(pdfPath, information);
    if (status == QLatin1String("reconciling")) {
        m_sourceMonitoringRequests.insert(pdfPath);
    } else {
        m_sourceMonitoringRequests.remove(pdfPath);
    }
    // The Library tree shows no monitoring information; only the document
    // information dialogs below do.

    const QList<QDialog *> dialogs = findChildren<QDialog *>(QStringLiteral("documentInformationDialog"));
    for (QDialog *dialog : dialogs) {
        if (dialog->property("sourceMonitoringPdfPath").toString() != pdfPath) {
            continue;
        }
        auto *summary = dialog->findChild<QLabel *>(QStringLiteral("documentInformationBuildInputs"));
        auto *button = dialog->findChild<QPushButton *>(QStringLiteral("documentInformationViewMonitoring"));
        if (!summary || !button) {
            continue;
        }
        const int folderCount = information.value(QStringLiteral("folders")).toList().size();
        const int pendingCount = information.value(QStringLiteral("pending_count")).toInt();
        const auto found = std::find_if(m_documents.cbegin(), m_documents.cend(), [&pdfPath](const TrackedCommentDocument &document) {
            return document.pdfPath == pdfPath;
        });
        if (status == QLatin1String("current") && found != m_documents.cend()) {
            const qint64 trackedFileCount =
                information.value(QStringLiteral("tracked_file_count"),
                                  information.value(QStringLiteral("input_count")))
                    .toLongLong();
            const QString fedoraInputs = found->verifiedBuildInputsAvailable
                ? i18np("Server build inputs: %1 file · %2",
                        "Server build inputs: %1 files · %2",
                        found->verifiedBuildInputCount,
                        formattedDocumentBytes(found->verifiedBuildInputBytes))
                : i18n("Server build inputs: unavailable for this publication");
            const QString trackedFiles = i18np("%1 tracked file",
                                               "%1 tracked files",
                                               trackedFileCount);
            const QString monitoredFolders = i18np("%1 monitored folder",
                                                    "%1 monitored folders",
                                                    folderCount);
            QString text = fedoraInputs + QLatin1Char('\n')
                + i18n("Local monitoring: %1 across %2",
                       trackedFiles,
                       monitoredFolders);
            if (pendingCount > 0) {
                text += i18np(" · %1 pending local file", " · %1 pending local files", pendingCount);
            }
            summary->setText(text);
            button->setEnabled(true);
            dialog->setProperty("sourceMonitoringInformation", information);
        } else if (status == QLatin1String("reconciling")) {
            summary->setText(i18n("Checking local monitoring…"));
            button->setEnabled(!information.value(QStringLiteral("folders")).toList().isEmpty());
            dialog->setProperty("sourceMonitoringInformation", information);
        } else {
            const QString error = information.value(QStringLiteral("error")).toString();
            summary->setText(error.isEmpty()
                                 ? i18n("Unavailable — local source monitoring has not completed.")
                                 : i18n("Unavailable — %1", error));
            button->setEnabled(false);
        }
    }
}

void CommentsOverview::undoOverviewChange()
{
    if (m_undoHistory.isEmpty() || m_operationPending || ((m_editorDirty || m_chatGptUrlDirty) && !resolveDirtyEditor())) {
        return;
    }
    const HistoryEntry entry = m_undoHistory.constLast();
    if (!entry.proposalSources.isEmpty()) {
        undoLiveAgentProposalDecision();
        return;
    }
    if (!entry.logicalPayload.isEmpty()) {
        CommentState targetState;
        if (entry.operation == EditOperation && entry.previousState) {
            targetState = *entry.previousState;
            applyCommentState(entry.pdfPath, entry.annotationId, targetState, true);
        } else {
            targetState.dueDate = entry.logicalPayload.value(QStringLiteral("calendar_date")).toString();
            targetState.dueTime = entry.logicalPayload.value(QStringLiteral("calendar_time")).toString();
            targetState.dueZone = entry.logicalPayload.value(QStringLiteral("calendar_zone_id")).toString();
            targetState.visibleFrom = entry.logicalPayload.value(QStringLiteral("visible_from")).toString();
            targetState.contents = entry.logicalPayload.value(QStringLiteral("contents")).toString();
            targetState.priority = entry.logicalPayload.value(QStringLiteral("priority")).toString(QStringLiteral("normal"));
            targetState.locationPresent = entry.logicalPayload.value(QStringLiteral("location")).isObject();
            targetState.location = entry.logicalPayload.value(QStringLiteral("location")).toObject();
        }
        const QJsonObject payload = logicalMutationPayload(entry.logicalPayload, targetState, false);
        m_undoHistory.last().logicalPayload = payload;
        if (m_currentComment.isValid() && entry.operation == EditOperation) {
            applyLogicalPayloadToCurrentRow(payload);
        }
        setOperationPending(true);
        Q_EMIT logicalCommentMutationRequested(payload.toVariantMap(), UndoOperation, entry.pdfPath, entry.annotationId);
        return;
    }
    if (entry.operation == EditOperation && entry.previousState && entry.currentState) {
        m_redoHistory.append(m_undoHistory.takeLast());
        applyCommentState(entry.pdfPath, entry.annotationId, *entry.previousState, true);
        QVariantMap exactMetadata;
        exactMetadata.insert(QStringLiteral("priority"), entry.previousState->priority);
        exactMetadata.insert(QStringLiteral("location"), entry.previousState->locationPresent ? entry.previousState->location.toVariantMap() : QVariant());
        Q_EMIT commentEditRequested(entry.pdfPath, entry.annotationId, entry.page, entry.previousState->contents, exactMetadata);
        updateEditorActions();
        return;
    }
    setOperationPending(true);
    if (entry.batchTargets.isEmpty()) {
        Q_EMIT commentUndoRequested(entry.pdfPath);
    } else {
        Q_EMIT commentsUndoRequested(entry.batchTargets);
    }
}

void CommentsOverview::redoOverviewChange()
{
    if (m_redoHistory.isEmpty() || m_operationPending || ((m_editorDirty || m_chatGptUrlDirty) && !resolveDirtyEditor())) {
        return;
    }
    const HistoryEntry entry = m_redoHistory.constLast();
    if (!entry.logicalPayload.isEmpty()) {
        CommentState targetState;
        if (entry.operation == EditOperation && entry.currentState) {
            targetState = *entry.currentState;
            applyCommentState(entry.pdfPath, entry.annotationId, targetState, true);
        } else {
            targetState.dueDate = entry.logicalPayload.value(QStringLiteral("calendar_date")).toString();
            targetState.dueTime = entry.logicalPayload.value(QStringLiteral("calendar_time")).toString();
            targetState.dueZone = entry.logicalPayload.value(QStringLiteral("calendar_zone_id")).toString();
            targetState.visibleFrom = entry.logicalPayload.value(QStringLiteral("visible_from")).toString();
            targetState.contents = entry.logicalPayload.value(QStringLiteral("contents")).toString();
            targetState.priority = entry.logicalPayload.value(QStringLiteral("priority")).toString(QStringLiteral("normal"));
            targetState.locationPresent = entry.logicalPayload.value(QStringLiteral("location")).isObject();
            targetState.location = entry.logicalPayload.value(QStringLiteral("location")).toObject();
        }
        const QJsonObject payload = logicalMutationPayload(entry.logicalPayload, targetState, false);
        m_redoHistory.last().logicalPayload = payload;
        if (m_currentComment.isValid() && entry.operation == EditOperation) {
            applyLogicalPayloadToCurrentRow(payload);
        }
        setOperationPending(true);
        Q_EMIT logicalCommentMutationRequested(payload.toVariantMap(), RedoOperation, entry.pdfPath, entry.annotationId);
        return;
    }
    if (entry.operation == EditOperation && entry.previousState && entry.currentState) {
        m_undoHistory.append(m_redoHistory.takeLast());
        applyCommentState(entry.pdfPath, entry.annotationId, *entry.currentState, true);
        QVariantMap exactMetadata;
        exactMetadata.insert(QStringLiteral("priority"), entry.currentState->priority);
        exactMetadata.insert(QStringLiteral("location"), entry.currentState->locationPresent ? entry.currentState->location.toVariantMap() : QVariant());
        Q_EMIT commentEditRequested(entry.pdfPath, entry.annotationId, entry.page, entry.currentState->contents, exactMetadata);
        updateEditorActions();
        return;
    }
    setOperationPending(true);
    if (entry.batchTargets.isEmpty()) {
        Q_EMIT commentRedoRequested(entry.pdfPath);
    } else {
        Q_EMIT commentsRedoRequested(entry.batchTargets);
    }
}

void CommentsOverview::completeBatchDelete(const QList<TrackedCommentTarget> &targets, const QVariantMap &result)
{
    setOperationPending(false);
    // Entries deleted from the Diary tab leave the comment browsers' selection alone.
    bool diaryDelete = !targets.isEmpty();
    for (const TrackedCommentTarget &target : targets) diaryDelete = m_pendingDiaryDeleteIds.remove(target.annotationId) && diaryDelete;
    const bool applied = result.value(QStringLiteral("success")).toBool() || result.value(QStringLiteral("partial")).toBool();
    const bool changed = result.value(QStringLiteral("changed"), applied).toBool();
    if (applied && changed && !targets.isEmpty()) {
        HistoryEntry entry;
        entry.pdfPath = targets.constFirst().pdfPath;
        entry.annotationId = targets.constFirst().annotationId;
        entry.operation = DeleteOperation;
        entry.page = targets.constFirst().page;
        entry.fallbackRow = targets.constFirst().fallbackRow;
        entry.batchTargets = targets;
        m_undoHistory.append(entry);
        m_redoHistory.clear();
        m_editorDirty = false;
        removeTargetsFromModel(targets);
        m_reloadDeferred = false;
        if (!diaryDelete) selectComment(QString(), QString(), entry.fallbackRow);
    }
    if (m_pendingDocumentRemoval) {
        if (result.value(QStringLiteral("success")).toBool()) {
            const auto legacyTargets = m_pendingDocumentRemoval->legacyTargets;
            if (!legacyTargets.isEmpty()) {
                m_pendingDocumentRemoval->legacyTargets.clear();
                setOperationPending(true);
                Q_EMIT commentsDeleteRequested(legacyTargets);
                return;
            }
            finishDocumentRemoval();
        } else {
            m_pendingDocumentRemoval.reset();
            refreshDocumentNavigation();
        }
    }
    const QString error = result.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, i18n("Delete Comments"), error);
    }
    updateEditorActions();
}

void CommentsOverview::completeBatchHistoryOperation(Operation operation, const QList<TrackedCommentTarget> &targets, const QVariantMap &result)
{
    setOperationPending(false);
    const bool success = result.value(QStringLiteral("success")).toBool();
    if (success && operation == UndoOperation && !m_undoHistory.isEmpty()) {
        m_redoHistory.append(m_undoHistory.takeLast());
    } else if (success && operation == RedoOperation && !m_redoHistory.isEmpty()) {
        m_undoHistory.append(m_redoHistory.takeLast());
    } else if (!success) {
        QSet<QString> affectedPdfs;
        for (const TrackedCommentTarget &target : targets) {
            affectedPdfs.insert(target.pdfPath);
        }
        const auto referencesAffectedPdf = [&affectedPdfs](const HistoryEntry &entry) {
            if (!entry.proposalSources.isEmpty()) {
                return false;
            }
            if (affectedPdfs.contains(entry.pdfPath)) {
                return true;
            }
            return std::any_of(entry.batchTargets.cbegin(), entry.batchTargets.cend(), [&affectedPdfs](const TrackedCommentTarget &target) { return affectedPdfs.contains(target.pdfPath); });
        };
        m_undoHistory.erase(std::remove_if(m_undoHistory.begin(), m_undoHistory.end(), referencesAffectedPdf), m_undoHistory.end());
        m_redoHistory.erase(std::remove_if(m_redoHistory.begin(), m_redoHistory.end(), referencesAffectedPdf), m_redoHistory.end());
    }
    if (success && operation == UndoOperation && !targets.isEmpty()) {
        m_pendingRevealPdfPath = targets.constFirst().pdfPath;
        m_pendingRevealAnnotationId = targets.constFirst().annotationId;
        m_pendingRevealFallbackRow = targets.constFirst().fallbackRow;
    }
    requestReload();
    if (success && operation == UndoOperation && !targets.isEmpty()) {
        selectComment(targets.constFirst().pdfPath, targets.constFirst().annotationId, targets.constFirst().fallbackRow);
    } else if (!targets.isEmpty()) {
        selectComment(QString(), QString(), targets.constFirst().fallbackRow);
    }
    const QString error = result.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, i18n("Comment History"), error);
    }
    updateEditorActions();
}

void CommentsOverview::completeReply(const QString &pdfPath, const QString &parentAnnotationId, const QVariantMap &result)
{
    setOperationPending(false);
    const bool applied = result.value(QStringLiteral("success")).toBool();
    const QString error = result.value(QStringLiteral("error")).toString();
    if (!applied) {
        m_summary->setText(error.isEmpty() ? i18n("The reply could not be added.") : error);
        return;
    }

    if (result.value(QStringLiteral("historyReset")).toBool()) {
        removeHistoryForPdf(pdfPath);
    }
    m_pendingRevealPdfPath = pdfPath;
    m_pendingRevealAnnotationId = result.value(QStringLiteral("replyId")).toString();
    if (m_pendingRevealAnnotationId.isEmpty()) {
        m_pendingRevealAnnotationId = parentAnnotationId;
    }
    m_pendingRevealFallbackRow = m_currentComment.isValid() ? m_currentComment.row() : -1;
    m_summary->setText(i18n("Reply added."));
    requestReload();
}

void CommentsOverview::completeOperation(Operation operation, const QString &pdfPath, const QString &annotationId, const QVariantMap &result)
{
    // Diary entries are saved from the Diary tab; the comment browsers' selection
    // and editor are not involved.
    if (operation == CreateOperation && !annotationId.isEmpty() && annotationId == m_pendingDiaryCreateId) {
        m_pendingDiaryCreateId.clear();
        setOperationPending(false);
        const QString error = result.value(QStringLiteral("error")).toString();
        if (result.value(QStringLiteral("success")).toBool()) {
            m_diaryInput->clear();
            requestReload();
        } else {
            // The text stays in the field, so it can be saved again.
            m_diarySummary->setText(error.isEmpty() ? i18n("The diary entry could not be saved.") : error);
        }
        return;
    }
    if (operation == EditOperation && m_pendingDiaryEditIds.remove(annotationId)) {
        if (!result.value(QStringLiteral("success")).toBool()) {
            const QString error = result.value(QStringLiteral("error")).toString();
            m_diarySummary->setText(error.isEmpty() ? i18n("The diary entry could not be saved.") : error);
            requestReload();
        }
        return;
    }
    const bool optimisticEdit = operation == EditOperation && std::any_of(m_undoHistory.crbegin(), m_undoHistory.crend(), [&pdfPath, &annotationId](const HistoryEntry &entry) {
                                    return entry.operation == EditOperation && entry.previousState && entry.pdfPath == pdfPath && entry.annotationId == annotationId;
                                });
    if (operation == EditOperation && !m_operationPending && optimisticEdit) {
        const QString error = result.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            m_summary->setText(i18n("Not saved yet — retrying in the background: %1", error));
        }
        updateEditorActions();
        return;
    }
    const bool hadPendingSelection = operation == EditOperation && !m_pendingSelection.isEmpty();
    setOperationPending(false);
    const bool applied = result.value(QStringLiteral("success")).toBool() || result.value(QStringLiteral("partial")).toBool();
    const bool changed = result.value(QStringLiteral("changed"), applied).toBool();
    HistoryEntry historyEntry;
    bool hasHistoryEntry = false;
    if (operation == UndoOperation && !m_undoHistory.isEmpty() && m_undoHistory.constLast().pdfPath == pdfPath) {
        historyEntry = m_undoHistory.constLast();
        hasHistoryEntry = true;
    } else if (operation == RedoOperation && !m_redoHistory.isEmpty() && m_redoHistory.constLast().pdfPath == pdfPath) {
        historyEntry = m_redoHistory.constLast();
        hasHistoryEntry = true;
    }
    if (result.value(QStringLiteral("historyReset")).toBool()) {
        removeHistoryForPdf(pdfPath);
    }
    const QString error = result.value(QStringLiteral("error")).toString();

    if (applied) {
        if (operation == CreateOperation) {
            m_summary->setText(i18n("Note added."));
        }
        if ((operation == EditOperation || operation == DeleteOperation) && changed) {
            historyEntry.pdfPath = pdfPath;
            historyEntry.annotationId = annotationId;
            historyEntry.operation = operation;
            historyEntry.page = m_currentComment.data(PageRole).toInt();
            historyEntry.fallbackRow = m_currentComment.isValid() ? m_currentComment.row() : -1;

            hasHistoryEntry = true;
            m_undoHistory.append(historyEntry);
            m_redoHistory.clear();
        } else if (operation == UndoOperation && hasHistoryEntry && !m_undoHistory.isEmpty()) {
            m_redoHistory.append(m_undoHistory.takeLast());
        } else if (operation == RedoOperation && hasHistoryEntry && !m_redoHistory.isEmpty()) {
            m_undoHistory.append(m_redoHistory.takeLast());
        }

        if (result.contains(QStringLiteral("canUndo")) && !result.value(QStringLiteral("canUndo")).toBool()) {
            removeUndoHistoryForPdf(pdfPath);
        }
        if (result.contains(QStringLiteral("canRedo")) && !result.value(QStringLiteral("canRedo")).toBool()) {
            removeRedoHistoryForPdf(pdfPath);
        }

        if (operation == EditOperation) {
            applyEditorToCurrentRow();
            m_reloadDeferred = false;
        } else if (operation == DeleteOperation && hasHistoryEntry) {
            removeTargetsFromModel({TrackedCommentTarget {historyEntry.pdfPath, historyEntry.annotationId, historyEntry.page, historyEntry.fallbackRow}});
            m_reloadDeferred = false;
        } else {
            if (operation == UndoOperation && hasHistoryEntry) {
                m_pendingRevealPdfPath = historyEntry.pdfPath;
                m_pendingRevealAnnotationId = historyEntry.annotationId;
                m_pendingRevealFallbackRow = historyEntry.fallbackRow;
            } else if (operation == RedoOperation && hasHistoryEntry && historyEntry.operation == DeleteOperation) {
                m_pendingRevealPdfPath.clear();
                m_pendingRevealAnnotationId.clear();
                m_pendingRevealFallbackRow = historyEntry.fallbackRow;
            }
            requestReload();
        }
        m_editorDirty = false;
        m_originalContents = m_commentEditor->toPlainText();
        if (operation == EditOperation && !m_pendingSelection.isEmpty()) {
            const QList<TrackedCommentTarget> pendingSelection = m_pendingSelection;
            const QString pendingCurrentPdfPath = m_pendingCurrentPdfPath;
            const QString pendingCurrentAnnotationId = m_pendingCurrentAnnotationId;
            m_pendingSelection.clear();
            m_pendingCurrentPdfPath.clear();
            m_pendingCurrentAnnotationId.clear();
            restoreCommentSelection(pendingSelection, pendingCurrentPdfPath, pendingCurrentAnnotationId);
        } else if (operation == EditOperation) {
            // applyEditorToCurrentRow() already updated the selected comment. Reloading it
            // would replace the editor document and reset its cursor after every autosave.
        } else if (hasHistoryEntry && ((operation == DeleteOperation) || (operation == RedoOperation && historyEntry.operation == DeleteOperation))) {
            selectComment(QString(), QString(), historyEntry.fallbackRow);
        } else if (hasHistoryEntry) {
            selectComment(historyEntry.pdfPath, historyEntry.annotationId, historyEntry.fallbackRow);
        } else if (!annotationId.isEmpty()) {
            selectComment(pdfPath, annotationId);
        } else {
            selectComment(m_currentPdfPath, m_currentAnnotationId);
        }
    }

    if (!applied && operation == EditOperation && !m_pendingSelection.isEmpty()) {
        const TrackedCommentTarget editedComment = {
            m_currentPdfPath,
            m_currentAnnotationId,
            m_currentComment.isValid() ? m_currentComment.data(PageRole).toInt() : 0,
            m_currentComment.isValid() ? m_currentComment.row() : -1,
        };
        m_pendingSelection.clear();
        m_pendingCurrentPdfPath.clear();
        m_pendingCurrentAnnotationId.clear();
        restoreCommentSelection({editedComment}, editedComment.pdfPath, editedComment.annotationId, false);
        if (m_details) {
            m_details->show();
        }
        m_editorDirty = true;
    }
    if (!applied && operation == CreateOperation) {
        m_pendingRevealPdfPath.clear();
        m_pendingRevealAnnotationId.clear();
        m_pendingRevealFallbackRow = -1;
    }

    if (!error.isEmpty()) {
        m_summary->setText(error);
    } else if (!applied) {
        m_summary->setText(i18n("The comment operation did not complete."));
    }
    if (operation == DeleteOperation) {
    }
    if (operation == EditOperation && m_restoreEditorStateAfterOperation) {
        if (!applied || !hadPendingSelection) {
            const int textLength = m_commentEditor->toPlainText().size();
            QTextCursor editorCursor = m_commentEditor->textCursor();
            editorCursor.setPosition(qBound(0, m_editorCursorAnchorBeforeOperation, textLength));
            editorCursor.setPosition(qBound(0, m_editorCursorPositionBeforeOperation, textLength), QTextCursor::KeepAnchor);
            m_commentEditor->setTextCursor(editorCursor);
            QScrollBar *verticalScrollBar = m_commentEditor->verticalScrollBar();
            verticalScrollBar->setValue(qBound(verticalScrollBar->minimum(), m_editorVerticalScrollBeforeOperation, verticalScrollBar->maximum()));
            QScrollBar *horizontalScrollBar = m_commentEditor->horizontalScrollBar();
            horizontalScrollBar->setValue(qBound(horizontalScrollBar->minimum(), m_editorHorizontalScrollBeforeOperation, horizontalScrollBar->maximum()));
        }
        m_restoreEditorStateAfterOperation = false;
    }
    updateEditorActions();
    if (m_reloadDeferred && !m_editorDirty && operation != EditOperation && operation != DeleteOperation) {
        requestReload();
    }
}

void CommentsOverview::completeChatGptUrlUpdate(const QString &pdfPath, const QString &annotationId, const QVariantMap &result)
{
    const bool applied = result.value(QStringLiteral("success")).toBool();
    const QString error = result.value(QStringLiteral("error")).toString();
    const bool openAfterSave = m_openChatGptAfterSave;
    m_openChatGptAfterSave = false;
    setOperationPending(false);
    if (applied && pdfPath == m_currentPdfPath && annotationId == m_currentAnnotationId) {
        m_originalChatGptUrl = m_pendingChatGptUrl;
        m_pendingChatGptUrl.clear();
        m_chatGptUrlDirty = false;
        m_changingSelection = true;
        m_chatGptUrlEditor->setText(m_originalChatGptUrl);
        m_changingSelection = false;
        applyChatGptUrlToCurrentRow(m_originalChatGptUrl);
        m_reloadDeferred = false;
        if (openAfterSave && !m_originalChatGptUrl.isEmpty() && !QDesktopServices::openUrl(QUrl(m_originalChatGptUrl))) {
            KMessageBox::error(this, i18n("The ChatGPT link could not be opened."), i18n("Open ChatGPT"));
        }
    } else {
        m_pendingChatGptUrl.clear();
        if (!error.isEmpty()) {
            m_summary->setText(error);
        } else {
            m_summary->setText(i18n("The ChatGPT link could not be saved."));
        }
    }
    updateEditorActions();
}

void CommentsOverview::applyBrowserColumns(BrowserState &browser)
{
    // The two browsers share one saved header layout. Notes are not assigned to a
    // project, so they have no document, page, snapshot or source.
    if (!browser.table) return;
    for (const int column : {DocumentColumn, PageColumn, SnapshotColumn, SourceColumn, SourceDocumentColumn}) {
        browser.table->setColumnHidden(column, browser.notes);
    }
}

void CommentsOverview::activateBrowser(BrowserState &browser)
{
    const DeveloperActivity activity(QStringLiteral("comments.tab_switch"), {{QStringLiteral("notes"), browser.notes}});
    BrowserState &previous = m_notesActive ? m_notesBrowser : m_commentsBrowser;
    const auto previousSizes = m_contentSplitter->sizes();
    if (previousSizes.size() == 2 && previousSizes[1] > 0) { previous.contentSizes = previousSizes; }
    m_notesActive = browser.notes;
    m_search = browser.search;
    m_typeFilter = browser.typeFilter;
    m_documentList = browser.documentList;
    m_table = browser.table;
    m_proxy = browser.proxy;
    m_summary = browser.summary;
    m_splitter = browser.splitter;
    m_contentSplitter = browser.contentSplitter;
    // The editor is shared; the two browsers and their selection models stay put.
    m_contentSplitter->addWidget(m_details);
    m_contentSplitter->setCollapsible(m_contentSplitter->indexOf(m_details), false);
    m_contentSplitter->setStretchFactor(0, 3);
    m_contentSplitter->setStretchFactor(1, 2);
    loadComment(m_table->currentIndex());
    updateSelectionUi();
    m_contentSplitter->setSizes(browser.contentSizes.isEmpty() ? QList<int>{700, 450} : browser.contentSizes);
}

void CommentsOverview::mirrorDocumentNavigation()
{
    // Build the navigation catalogue once, then project it into the other tab.
    // This runs on catalogue changes, never on tab activation.
    auto *other = m_notesActive ? m_commentsBrowser.documentList : m_notesBrowser.documentList;
    QString selectedKey;
    QSet<QString> expanded;
    const int scroll = other->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(other); *it; ++it) {
        const QString key = (*it)->data(0, DocumentNavigationKeyRole).toString();
        if ((*it)->isExpanded()) { expanded.insert(key); }
        if (*it == other->currentItem()) { selectedKey = key; }
    }
    const QSignalBlocker blocker(other);
    other->clear();
    for (int row = 0; row < m_documentList->topLevelItemCount(); ++row) {
        other->addTopLevelItem(m_documentList->topLevelItem(row)->clone());
    }
    auto *selected = other->topLevelItem(0);
    for (QTreeWidgetItemIterator it(other); *it; ++it) {
        const QString key = (*it)->data(0, DocumentNavigationKeyRole).toString();
        (*it)->setExpanded(expanded.contains(key));
        if (key == selectedKey) { selected = *it; }
    }
    other->setCurrentItem(selected);
    other->verticalScrollBar()->setValue(scroll);
}

void CommentsOverview::resetBrowserFilters()
{
    const QSignalBlocker search(m_search), type(m_typeFilter), documents(m_documentList);
    m_search->clear();
    m_typeFilter->setCurrentIndex(0);
    m_documentList->setCurrentItem(m_documentList->topLevelItem(0));
    applyFilters();
}

void CommentsOverview::applyFilters()
{
    applyBrowserFilters(m_notesActive ? m_notesBrowser : m_commentsBrowser);
}

void CommentsOverview::applyBrowserFilters(BrowserState &browser)
{
    const DeveloperActivity activity(QStringLiteral("comments.filter"), {{QStringLiteral("notes"), browser.notes}});
    auto *proxy = static_cast<CommentsFilterProxyModel *>(browser.proxy);
    proxy->beginUpdate();
    const auto *selection = browser.documentList->currentItem();
    proxy->setDocumentStem(selection ? selection->data(0, Qt::UserRole).toString() : QString());
    const auto *documentItem = browser.documentList->currentItem();
    proxy->setProjectScope(documentItem ? documentItem->data(0, DocumentScopeRole).toInt() : 0);
    const int kind = documentItem ? documentItem->data(0, DocumentItemKindRole).toInt() : AllCommentsItem;
    QSet<QString> projectDocuments;
    if (documentItem && kind == ProjectListEntry) {
        for (int row = 0; row < documentItem->childCount(); ++row) {
            projectDocuments.insert(documentItem->child(row)->data(0, Qt::UserRole).toString());
        }
    }
    proxy->setProjectDocuments(kind == ProjectListEntry, projectDocuments);
    proxy->setThreadIds(documentItem && (kind == ComponentListEntry || (kind == ThreadGroupListEntry && documentItem->data(0, DocumentScopeRole).toInt() != 2)),
                        documentItem ? documentItem->data(0, DocumentThreadIdsRole).toStringList() : QStringList());
    proxy->setSearchText(browser.search->text());
    proxy->setNotesOnly(browser.notes);
    proxy->setItemType(browser.notes ? QString() : browser.typeFilter->currentData().toString());
    browser.documentList->setColumnHidden(1, browser.notes);
    browser.documentList->setColumnHidden(2, browser.notes);
    browser.documentList->setColumnHidden(3, !browser.notes);
    applyBrowserColumns(browser);
    for (QTreeWidgetItemIterator it(browser.documentList); *it; ++it) {
        const QString key = (*it)->data(0, DocumentNavigationKeyRole).toString();
        const bool emptyNoProject = key == QLatin1String("no-project")
            && (browser.notes ? (*it)->text(3).toInt() == 0 : (*it)->text(1).toInt() + (*it)->text(2).toInt() == 0);
        (*it)->setHidden(emptyNoProject || (key == QLatin1String("no-project/notes") ? !browser.notes
                            : (key == QLatin1String("no-project/comments") || key == QLatin1String("no-project/proposals")) && browser.notes));
    }
    proxy->endUpdate();
    // Navigation changes only the comment view; the Library data is unchanged.
    updateCommentSummary();

}

void CommentsOverview::traceInteraction(const QString &event, const QModelIndex &index)
{
    if (!m_table || !m_table->selectionModel() || !m_proxy || !m_details || !m_tabs) { return; }
    const auto current = (index.isValid() ? index : m_table->currentIndex()).siblingAtColumn(CommentColumn);
    const auto rows = m_table->selectionModel()->selectedRows();
    QJsonArray selected;
    for (const auto &row : rows.mid(0, 30)) {
        selected.append(QJsonObject {{QStringLiteral("row"), row.row()}, {QStringLiteral("parent_row"), row.parent().row()},
            {QStringLiteral("comment_id"), DeveloperLog::documentId(row.data(AnnotationIdRole).toString())}});
    }
    DeveloperLog::record(event, {{QStringLiteral("document_id"), DeveloperLog::documentId(current.data(PdfPathRole).toString())},
        {QStringLiteral("comment_id"), DeveloperLog::documentId(current.data(AnnotationIdRole).toString())},
        {QStringLiteral("filter_id"), DeveloperLog::documentId(selectedDocument())},
        {QStringLiteral("row"), current.row()}, {QStringLiteral("page"), current.data(PageRole).toInt()},
        {QStringLiteral("selected_count"), rows.size()}, {QStringLiteral("selected_rows"), selected},
        {QStringLiteral("visible_rows"), m_proxy->rowCount()}, {QStringLiteral("details_visible"), m_details->isVisible()},
        {QStringLiteral("details_hidden"), m_details->isHidden()}, {QStringLiteral("editor_dirty"), m_editorDirty},
        {QStringLiteral("changing_selection"), m_changingSelection}, {QStringLiteral("tab"), m_tabs->currentIndex()}});
}

void CommentsOverview::activateComment(const QModelIndex &index)
{
    traceInteraction(QStringLiteral("comments.activate.requested"), index);
    if (!index.isValid()) {
        return;
    }
    if (!resolveDirtyEditor()) {
        return;
    }
    const QModelIndex firstColumn = index.siblingAtColumn(CommentColumn);
    if (!firstColumn.data(FeedbackKindRole).toString().isEmpty()) {
        Q_EMIT documentFeedbackActivated(firstColumn.data(PdfPathRole).toString(), firstColumn.data(AnnotationIdRole).toString());
        return;
    }
    const bool proposal = firstColumn.data(ItemTypeRole).toString() == QLatin1String("agent_remove");
    if (proposal) {
        m_table->selectionModel()->setCurrentIndex(firstColumn, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        loadComment(firstColumn);
        QString sourcePath;
        QString error;
        int line = 0;
        const bool sourceAvailable = currentSourceTarget(&sourcePath, &line, &error);
        const QString id = firstColumn.data(AnnotationIdRole).toString();
        if (firstColumn.data(AnchorStatusRole).toString() == QLatin1String("proposal_removed")) {
            m_summary->setText(i18n("This proposal was removed outside Thoughts. Use Restore Proposal to put it back, Copy Proposal Text to keep its text, or Dismiss."));
        } else if (const QString issue = firstColumn.data(ProposalIssueRole).toString(); !issue.isEmpty()) {
            m_summary->setText(sourceAvailable ? i18n("This proposal cannot be decided until its source is fixed: %1 Use Open Source in VS Code to fix it.", Thoughts::agentProposalIssueMessage(issue))
                                               : i18n("This proposal cannot be decided until its source is fixed: %1", Thoughts::agentProposalIssueMessage(issue)));
        } else if (firstColumn.data(PdfAvailableRole).toBool() && !id.isEmpty()) {
            Q_EMIT agentProposalActivated(firstColumn.data(PdfPathRole).toString(), id, sourceAvailable ? sourcePath : QString(), sourceAvailable ? line : 0);
        } else {
            m_summary->setText(firstColumn.data(PdfAvailableRole).toBool() ? i18n("This proposal has no stable identifier.")
                                                                          : sourceAvailable ? i18n("This proposal has no published PDF yet. Use Open Source in VS Code to inspect it.")
                                                                                            : error.isEmpty() ? i18n("The canonical PDF for this proposal is missing.") : error);
        }
        return;
    }
    if (firstColumn.data(LogicalCommentRole).toBool()) {
        const QJsonArray encodedLocations = firstColumn.data(PdfLocationsRole).value<QJsonArray>();
        if (encodedLocations.isEmpty()) {
            const QString pdfPath = firstColumn.data(PdfPathRole).toString();
            const QString id = firstColumn.data(AnnotationIdRole).toString();
            if (firstColumn.data(PdfAvailableRole).toBool() && !pdfPath.isEmpty() && !id.isEmpty()) {
                Q_EMIT commentActivated(pdfPath, id, firstColumn.data(PageRole).toInt());
                return;
            }
            m_summary->setText(firstColumn.data(AnchorStatusRole).toString() == QLatin1String("needs_attention")
                                   ? i18n("This comment needs source-link attention. Use Open Source when a source location is available.")
                                   : i18n("This comment has no location for an available PDF revision. Use Open Source when available."));
            return;
        }
        QJsonObject selectedLocation;
        if (encodedLocations.size() == 1) {
            selectedLocation = encodedLocations.first().toObject();
        } else {
            QMenu locationMenu(this);
            for (const QJsonValue &encoded : encodedLocations) {
                const QJsonObject location = encoded.toObject();
                const QString filename = location.value(QStringLiteral("pdf_filename")).toString();
                const int page = location.value(QStringLiteral("page")).toInt();
                const auto tracked = std::find_if(m_documents.cbegin(), m_documents.cend(), [&filename](const TrackedCommentDocument &document) {
                    return QFileInfo(document.pdfPath).fileName().compare(filename, Qt::CaseInsensitive) == 0;
                });
                const QString documentName = tracked == m_documents.cend() ? QFileInfo(filename).completeBaseName() : tracked->displayName;
                QAction *action = locationMenu.addAction(page > 0 ? i18n("%1 · p. %2", documentName, page) : documentName);
                action->setData(QVariant::fromValue(location));
            }
            QAction *selectedAction = locationMenu.exec(m_table->viewport()->mapToGlobal(m_table->visualRect(index).bottomLeft()));
            if (!selectedAction) {
                return;
            }
            selectedLocation = selectedAction->data().value<QJsonObject>();
        }
        const QString filename = selectedLocation.value(QStringLiteral("pdf_filename")).toString();
        const QString pdfPath = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename));
        const QString expectedSha256 = selectedLocation.value(QStringLiteral("pdf_sha256")).toString().toLower();
        const QString actualSha256 = QString::fromLatin1(fileContentSha256(pdfPath).toHex());
        const QString placementStatus = selectedLocation.value(QStringLiteral("status")).toString();
        if (QFileInfo(pdfPath).fileName() != filename || !QFileInfo::exists(pdfPath) || expectedSha256.isEmpty() || actualSha256 != expectedSha256
            || (placementStatus != QLatin1String("exact") && placementStatus != QLatin1String("page_only"))) {
            m_summary->setText(i18n("That PDF revision is unavailable. Use Open Source instead of opening stale placement geometry."));
            return;
        }
        Q_EMIT logicalCommentLocationActivated(pdfPath,
                                               firstColumn.data(AnnotationIdRole).toString(),
                                               selectedLocation.value(QStringLiteral("page")).toInt(),
                                               selectedLocation.toVariantMap());
        return;
    }
    if (!firstColumn.data(PdfAvailableRole).toBool()) {
        m_summary->setText(i18n("The canonical PDF for this comment is missing."));
        return;
    }
    const QString id = firstColumn.data(AnnotationIdRole).toString();
    if (id.isEmpty()) {
        m_summary->setText(i18n("This comment has no stable annotation identifier."));
        return;
    }
    Q_EMIT commentActivated(firstColumn.data(PdfPathRole).toString(), id, firstColumn.data(PageRole).toInt());
}

void CommentsOverview::activateDocument(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    const int kind = item->data(0, DocumentItemKindRole).toInt();
    if (kind == ProjectListEntry) {
        // A project container follows the Library: activating it opens the
        // project's own published PDF when the catalogue names one.
        const QString target = item->data(0, DocumentPathRole).toString();
        const int page = item->data(0, DocumentStartPageRole).toInt();
        if (!target.isEmpty() && page > 0) {
            Q_EMIT documentPageActivated(target, page);
        } else if (!target.isEmpty()) {
            Q_EMIT documentActivated(target);
        }
        return;
    }
    if (kind != DocumentListEntry && kind != ComponentListEntry) {
        return;
    }
    const QString pdfPath = item->data(0, DocumentPathRole).toString();
    const int startPage = item->data(0, DocumentStartPageRole).toInt();
    if (!pdfPath.isEmpty() && kind == ComponentListEntry && startPage > 0) {
        Q_EMIT documentPageActivated(pdfPath, startPage, item->data(0, DocumentCompositionIdRole).toString(), item->text(0));
    } else if (!pdfPath.isEmpty() && kind == DocumentListEntry) {
        Q_EMIT documentActivated(pdfPath);
    }
}

void CommentsOverview::scheduleReload(const QString &path)
{
    if (!isVisible()) {
        return;
    }
    const QDir root(m_docsRoot);
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    const QString commentsDirectory = QFileInfo(root.filePath(QStringLiteral("comments"))).absoluteFilePath();
    const QString layoutsDirectory = QFileInfo(root.filePath(QStringLiteral("comments/.layout"))).absoluteFilePath();
    const QString pdfDirectory = QFileInfo(root.filePath(QStringLiteral("current_pdfs"))).absoluteFilePath();
    const QString libraryIndexPath = QFileInfo(root.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"))).absoluteFilePath();
    const QString resolutionsPath = QFileInfo(root.filePath(QStringLiteral("comments/review_resolutions.jsonl"))).absoluteFilePath();

    const auto scheduleStem = [this](const QString &stem) {
        if (!stem.isEmpty()) {
            m_scheduledDocumentReloads.insert(stem);
        }
    };
    const auto scanChangedFiles = [this, &scheduleStem](const QString &directoryPath, const QStringList &filters, bool layoutFiles) {
        const QDir directory(directoryPath);
        QSet<QString> present;
        for (const QFileInfo &file : directory.entryInfoList(filters, QDir::Files, QDir::Name)) {
            const QString candidate = file.absoluteFilePath();
            present.insert(candidate);
            const QByteArray signature = fileEventSignature(candidate);
            if (m_knownFileSignatures.value(candidate) == signature) {
                continue;
            }
            m_knownFileSignatures.insert(candidate, signature);
            if (!shouldIgnoreLocalWrite(candidate)) {
                QString stem = file.completeBaseName();
                if (!layoutFiles) {
                    stem.remove(QStringLiteral("_comments"));
                }
                scheduleStem(stem);
            }
        }
        const QString prefix = QFileInfo(directoryPath).absoluteFilePath() + QLatin1Char('/');
        const QStringList known = m_knownFileSignatures.keys();
        for (const QString &knownPath : known) {
            if (!knownPath.startsWith(prefix) || present.contains(knownPath)) {
                continue;
            }
            const QFileInfo knownInfo(knownPath);
            if (!filters.isEmpty() && !QDir::match(filters, knownInfo.fileName())) {
                continue;
            }
            m_knownFileSignatures.remove(knownPath);
            QString stem = knownInfo.completeBaseName();
            if (!layoutFiles) {
                stem.remove(QStringLiteral("_comments"));
            }
            scheduleStem(stem);
        }
    };

    if (absolutePath == pdfDirectory) {
        const QStringList pdfNames = QDir(pdfDirectory).entryList({QStringLiteral("*.pdf"), QStringLiteral("*.PDF")}, QDir::Files, QDir::Name);
        const QByteArray membership = pdfNames.join(QChar(0x1f)).toUtf8();
        if (m_knownFileSignatures.value(pdfDirectory) != membership) {
            m_knownFileSignatures.insert(pdfDirectory, membership);
            m_fullReloadScheduled = true;
        } else {
            return;
        }
    } else if (absolutePath == commentsDirectory) {
        scanChangedFiles(commentsDirectory, {QStringLiteral("*_comments.jsonl")}, false);
    } else if (absolutePath == layoutsDirectory) {
        scanChangedFiles(layoutsDirectory, {QStringLiteral("*.json")}, true);
    } else if (absolutePath == libraryIndexPath) {
        m_knownFileSignatures.insert(absolutePath, fileEventSignature(absolutePath));
        m_fullReloadScheduled = true;
    } else if (absolutePath == resolutionsPath) {
        m_knownFileSignatures.insert(absolutePath, fileEventSignature(absolutePath));
        m_fullReloadScheduled = true;
    } else if (absolutePath.endsWith(QStringLiteral("_comments.jsonl"))) {
        m_knownFileSignatures.insert(absolutePath, fileEventSignature(absolutePath));
        if (!shouldIgnoreLocalWrite(absolutePath)) {
            QString stem = QFileInfo(absolutePath).completeBaseName();
            stem.remove(QStringLiteral("_comments"));
            scheduleStem(stem);
        }
    } else if (absolutePath.endsWith(QStringLiteral(".json")) && QFileInfo(absolutePath).absolutePath() == layoutsDirectory) {
        m_knownFileSignatures.insert(absolutePath, fileEventSignature(absolutePath));
        scheduleStem(QFileInfo(absolutePath).completeBaseName());
    } else if (path.isEmpty()) {
        m_fullReloadScheduled = true;
    } else {
        return;
    }

    refreshWatchPaths();
    if (m_editorDirty || m_chatGptUrlDirty || m_operationPending) {
        m_reloadDeferred = true;
    } else {
        m_reloadTimer->start();
    }
}

void CommentsOverview::processScheduledReloads()
{
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_editorDirty || m_chatGptUrlDirty || m_operationPending) {
        m_reloadDeferred = true;
        return;
    }
    m_reloadDeferred = false;
    if (m_fullReloadScheduled) {
        m_fullReloadScheduled = false;
        m_scheduledDocumentReloads.clear();
        startFullReload(std::exchange(m_discoveryReloadScheduled, false) || m_syncRefreshPending, QStringLiteral("scheduled_reload"));
        return;
    }
    const QSet<QString> stems = std::exchange(m_scheduledDocumentReloads, {});
    for (const QString &stem : stems) {
        startDocumentReload(stem);
    }
}

void CommentsOverview::startDocumentReload(const QString &stem)
{
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_runningDocumentReloads.contains(stem)) {
        m_reloadAgainDocuments.insert(stem);
        return;
    }
    m_runningDocumentReloads.insert(stem);
    const QString docsRoot = m_docsRoot;
    const QString pdfPath = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(stem));
    const QJsonObject fedoraProjections = m_fedoraProjections;
    QPointer<CommentsOverview> guard(this);
    m_catalogPool->start([guard, docsRoot, pdfPath, stem, fedoraProjections] {
        const auto settingsIdentity = ProjectConfiguration::snapshotIdentity(docsRoot);
        const auto loaded = loadTrackedCommentDocuments(docsRoot, fedoraProjections, nullptr, QFileInfo(pdfPath).fileName(), false);
        const std::optional<TrackedCommentDocument> document = loaded.isEmpty() ? std::nullopt : std::optional<TrackedCommentDocument>(loaded.constFirst());
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, stem, document, settingsIdentity] {
                if (guard && settingsIdentity != ProjectConfiguration::snapshotIdentity(guard->m_docsRoot)) {
                    guard->m_runningDocumentReloads.remove(stem);
                    guard->startFullReload(true, QStringLiteral("settings_changed_during_load"));
                    return;
                }
                if (guard) {
                    guard->finishDocumentReload(stem, document);
                }
            },
            Qt::QueuedConnection);
    });
}

// The publication fields the Library tree, its summary and source monitoring
// read (updateLibraryTree() and its passes, updateDashboardSummary()). Comments
// and proposals are not shown there: a document read that changes only those
// leaves the tree as it is.
static bool sameLibraryTreeFields(const TrackedCommentDocument &left, const TrackedCommentDocument &right)
{
    return left.libraryFilename == right.libraryFilename && left.pdfPath == right.pdfPath && left.libraryKind == right.libraryKind
        && left.librarySha256 == right.librarySha256 && left.libraryProject == right.libraryProject
        && left.libraryPublishedAt == right.libraryPublishedAt && left.libraryBuiltAt == right.libraryBuiltAt
        && left.libraryPageCount == right.libraryPageCount && left.libraryPdfStale == right.libraryPdfStale
        && left.libraryLatestBuildMessage == right.libraryLatestBuildMessage && left.libraryLatestBuildSource == right.libraryLatestBuildSource
        && left.librarySegments == right.librarySegments
        && left.pdfSizeBytes == right.pdfSizeBytes;
}

void CommentsOverview::finishDocumentReload(const QString &stem, const std::optional<TrackedCommentDocument> &document)
{
    m_runningDocumentReloads.remove(stem);
    if (m_obsoleteDocumentReloads.remove(stem) || m_syncContentDeferred || m_syncRefreshPending) { m_reloadDeferred = true; return; }
    const QString sidecarPath = QDir(m_docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(stem));
    if (hasPendingMutationForPath(sidecarPath)) {
        m_reloadAgainDocuments.insert(stem);
    } else {
        const QModelIndex topVisibleIndex = m_table->indexAt(QPoint(1, 1));
        const QString topVisiblePdfPath = topVisibleIndex.data(PdfPathRole).toString();
        const QString topVisibleAnnotationId = topVisibleIndex.data(AnnotationIdRole).toString();
        const int previousScrollValue = m_table->verticalScrollBar()->value();
        const QString selectedStem = selectedDocument();
        const QList<TrackedCommentTarget> selectedTargets = selectedCommentTargets();
        const QString currentPdfPath = m_currentPdfPath;
        const QString currentAnnotationId = m_currentAnnotationId;
        bool libraryChanged = document.has_value();
        {
            const QScopedValueRollback<bool> updatingModel(m_updatingCommentModel, true);
            const auto existing = std::find_if(m_documents.begin(), m_documents.end(), [&stem](const TrackedCommentDocument &candidate) { return candidate.stem == stem; });
            if (existing != m_documents.end()) {
                libraryChanged = !document || !sameLibraryTreeFields(*existing, *document);
                if (document) {
                    *existing = *document;
                } else {
                    m_documents.erase(existing);
                }
            } else if (document) {
                m_documents.append(*document);
                std::sort(m_documents.begin(), m_documents.end(), [](const TrackedCommentDocument &left, const TrackedCommentDocument &right) {
                    const int displayOrder = left.displayName.compare(right.displayName, Qt::CaseInsensitive);
                    return displayOrder == 0 ? left.stem.compare(right.stem, Qt::CaseInsensitive) < 0 : displayOrder < 0;
                });
            }
            populateComments();
        }
        populateDocumentList(selectedStem);
        Q_EMIT documentsChanged();
        updateDashboardSummary(libraryChanged);
        applyFilters();
        restoreCommentSelection(selectedTargets, currentPdfPath, currentAnnotationId, !m_editorDirty);
        QModelIndex topVisibleTarget;
        if (!topVisiblePdfPath.isEmpty() && !topVisibleAnnotationId.isEmpty()) {
            for (const QModelIndex &candidate : modelCommentIndexes(m_proxy)) {
                if (candidate.data(PdfPathRole).toString() == topVisiblePdfPath && candidate.data(AnnotationIdRole).toString() == topVisibleAnnotationId) {
                    topVisibleTarget = candidate;
                    break;
                }
            }
        }
        if (topVisibleTarget.isValid()) {
            m_table->scrollTo(topVisibleTarget, QAbstractItemView::PositionAtTop);
        } else {
            QScrollBar *scrollBar = m_table->verticalScrollBar();
            scrollBar->setValue(qBound(scrollBar->minimum(), previousScrollValue, scrollBar->maximum()));
        }
    }
    if (m_reloadAgainDocuments.remove(stem)) {
        startDocumentReload(stem);
    }
}

void CommentsOverview::startFullReload(bool discoverIncludedSources, const QString &reason)
{
    DeveloperLog::record(QStringLiteral("library.reload.requested"),
        {{QStringLiteral("reason"), reason}, {QStringLiteral("full_discovery"), discoverIncludedSources},
         {QStringLiteral("already_running"), m_fullReloadRunning}});
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_fullReloadRunning) {
        if (reason == QLatin1String("catalog_changed") && !discoverIncludedSources) {
            // Decided when the running reload finishes: it may have read this catalogue already.
            m_catalogReloadScheduled = true;
            m_scheduledCatalogSignature = m_catalogSignature;
            return;
        }
        m_fullReloadScheduled = true;
        m_discoveryReloadScheduled |= discoverIncludedSources;
        return;
    }
    m_fullReloadRunning = true;
    m_fullReloadElapsed.start();
    DeveloperLog::record(QStringLiteral("library.reload.queued"),
        {{QStringLiteral("reason"), reason}, {QStringLiteral("full_discovery"), discoverIncludedSources}});
    const QString docsRoot = m_docsRoot;
    const QJsonObject fedoraProjections = m_fedoraProjections;
    QPointer<CommentsOverview> guard(this);
    const quint64 generation = m_commentGeneration;
    m_catalogPool->start([guard, docsRoot, fedoraProjections, generation, discoverIncludedSources] {
        const auto settingsIdentity = ProjectConfiguration::snapshotIdentity(docsRoot);
        // The catalogue this reload reads, taken before reading it: a change seen later with the
        // same signature is already read (finishFullReload), and the first baseline.
        const QByteArray catalogBaseline = commentCatalogSignature(docsRoot);
        bool liveSourcesValid = true;
        const QList<TrackedCommentDocument> documents = loadTrackedCommentDocuments(docsRoot, fedoraProjections, &liveSourcesValid, {}, discoverIncludedSources);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, documents, liveSourcesValid, generation, catalogBaseline, settingsIdentity] {
                if (guard) {
                    guard->finishFullReload(documents, liveSourcesValid, generation, catalogBaseline, settingsIdentity);
                }
            },
            Qt::QueuedConnection);
    });
}

void CommentsOverview::finishFullReload(const QList<TrackedCommentDocument> &documents, bool liveSourcesValid, std::optional<quint64> generation,
                                        const QByteArray &catalogBaseline, const QMap<QString, QString> &settingsIdentity)
{
    DeveloperLog::record(QStringLiteral("library.reload.worker_finished"), {{QStringLiteral("duration_ms"), m_fullReloadElapsed.elapsed()},
        {QStringLiteral("documents"), documents.size()}});
    const DeveloperActivity activity(QStringLiteral("comments.refresh"), {{QStringLiteral("documents"), documents.size()}});
    m_fullReloadRunning = false;
    if (m_catalogSignature.isEmpty()) m_catalogSignature = catalogBaseline;
    if (std::exchange(m_catalogReloadScheduled, false)) {
        const QByteArray seen = std::exchange(m_scheduledCatalogSignature, {});
        if (!catalogBaseline.isEmpty() && seen == catalogBaseline) {
            DeveloperLog::record(QStringLiteral("library.reload.covered"), {{QStringLiteral("reason"), QStringLiteral("catalog_changed")}});
        } else {
            m_fullReloadScheduled = true;
        }
    }
    if (generation && (*generation != m_commentGeneration || settingsIdentity != ProjectConfiguration::snapshotIdentity(m_docsRoot))) {
        // Autosave can clear editorDirty while this older snapshot is in flight.
        // Never publish it, even if the editor currently has no unsaved text.
        m_fullReloadScheduled = true;
        if (m_editorDirty || m_chatGptUrlDirty || m_operationPending || m_syncContentDeferred) m_reloadDeferred = true;
        else m_reloadTimer->start();
        return;
    }
    if (m_syncContentDeferred) { m_reloadDeferred = true; return; }
    if (m_fullReloadScheduled && m_syncRefreshPending) {
        m_fullReloadScheduled = false;
        m_discoveryReloadScheduled = false;
        startFullReload(true);
        return;
    }
    // A malformed source is reported on its owning document. It must not hold
    // every healthy project's refreshed rows behind a library-wide retry loop.
    Q_UNUSED(liveSourcesValid)
    m_fullReloadTransientRetries = 0;
    if (m_editorDirty || m_chatGptUrlDirty || m_operationPending) {
        m_fullReloadScheduled = true;
        m_reloadDeferred = true;
    } else {
        const QModelIndex topVisibleIndex = m_table->indexAt(QPoint(1, 1));
        const QString topVisiblePdfPath = topVisibleIndex.data(PdfPathRole).toString();
        const QString topVisibleAnnotationId = topVisibleIndex.data(AnnotationIdRole).toString();
        const int previousScrollValue = m_table->verticalScrollBar()->value();
        const QString selectedStem = selectedDocument();
        const QList<TrackedCommentTarget> selectedTargets = selectedCommentTargets();
        const QString currentPdfPath = m_currentPdfPath;
        const QString currentAnnotationId = m_currentAnnotationId;
        if (m_documents != documents || m_syncRefreshPending) {
            if (m_syncRefreshPending && !m_syncLibraryTree) {
                m_syncLibraryTree = new QTreeWidget(this);
                m_syncLibraryTree->hide();
                m_syncLibraryTree->setColumnCount(m_libraryTree->columnCount());
            }
            m_syncRefreshPending = false;
            m_reloadDeferred = false;
            m_documents = documents;
            populateDocumentList(selectedStem);
            populateComments();
            updateDashboardSummary();
            if (m_syncLibraryTree) { QTimer::singleShot(0, this, &CommentsOverview::publishSyncLibraryRefresh); }
            applyFilters();
            restoreCommentSelection(selectedTargets, currentPdfPath, currentAnnotationId);
            QModelIndex topVisibleTarget;
            if (!topVisiblePdfPath.isEmpty() && !topVisibleAnnotationId.isEmpty()) {
                for (const QModelIndex &candidate : modelCommentIndexes(m_proxy)) {
                    if (candidate.data(PdfPathRole).toString() == topVisiblePdfPath && candidate.data(AnnotationIdRole).toString() == topVisibleAnnotationId) {
                        topVisibleTarget = candidate;
                        break;
                    }
                }
            }
            if (topVisibleTarget.isValid()) {
                m_table->scrollTo(topVisibleTarget, QAbstractItemView::PositionAtTop);
            } else {
                QScrollBar *scrollBar = m_table->verticalScrollBar();
                scrollBar->setValue(qBound(scrollBar->minimum(), previousScrollValue, scrollBar->maximum()));
            }
            Q_EMIT documentsChanged();
        }
        if (!std::exchange(m_documentsLoaded, true)) updateDashboardSummary();
        if (!m_pendingRevealAnnotationId.isEmpty() || m_pendingRevealFallbackRow >= 0) {
            const QString pdfPath = std::exchange(m_pendingRevealPdfPath, {});
            const QString annotationId = std::exchange(m_pendingRevealAnnotationId, {});
            const int fallbackRow = std::exchange(m_pendingRevealFallbackRow, -1);
            resetBrowserFilters();
            selectComment(pdfPath, annotationId, fallbackRow);
            if (!m_currentComment.isValid()) {
                m_summary->setText(i18n("The requested comment could not be found."));
            }
        }
        if (!m_summaryAfterReload.isEmpty()) {
            const QString outcome = std::exchange(m_summaryAfterReload, {});
            if (!m_summaryAfterReloadDeadline.hasExpired()) m_summary->setText(outcome);
        }
    }
    // The sources were just rescanned: finish marker removals left pending.
    if (m_reviewCardsWidget && !m_editorDirty) m_reviewCardsWidget->retryPendingMarkerRemovals();
    if (m_fullReloadScheduled && !m_reloadDeferred) {
        m_reloadTimer->start();
    }
}

// What a document's row says about the document besides its name and counts:
// its Library and Sync state as a tooltip, and a warning icon when either needs
// attention. The only part of the Library tree that depends on the Sync state.
CommentsOverview::DocumentRowPresentation CommentsOverview::documentRowPresentation(const TrackedCommentDocument &document, bool hasComponents) const
{
    const bool sourceLinked = document.state == TrackedCommentDocument::SourceLinked || document.libraryKind == QLatin1String("source_linked");
    bool warning = false;
    const QString syncKey = document.libraryFilename.isEmpty() ? QFileInfo(document.pdfPath).fileName() : document.libraryFilename;
    const QJsonObject syncPresentation = m_documentSyncPresentations.value(syncKey).toObject();
    const QString syncStatus = syncPresentation.value(QStringLiteral("status")).toString(i18n("Not checked"));
    QStringList status {i18n("Sync: %1", syncStatus)};
    const QString syncReason = syncPresentation.value(QStringLiteral("reason")).toString().trimmed();
    if (!syncReason.isEmpty()) {
        status.append(syncReason);
    }
    if (sourceLinked) {
        status.append(i18n("Source layout: linked"));
    } else {
        status.append(i18n("Source layout: not linked"));
        status.append(i18n("PDF only: comments are tracked, but source anchoring and targeted refresh are unavailable."));
    }
    if (document.libraryFilename.isEmpty()) {
        status.prepend(i18n("Library: not listed"));
    } else if (!document.libraryGenerationCurrent) {
        status.prepend(i18n("Library: listed, but the active publication identity is not verified"));
        warning = true;
    } else if (document.libraryPublishedAt.isValid()) {
        status.prepend(i18n("Library: current · published %1", QLocale().toString(document.libraryPublishedAt.toLocalTime(), QLocale::ShortFormat)));
    }
    if (document.libraryPdfStale) {
        status.prepend(i18n("Library: last-good publication · latest server build failed: %1",
                           document.libraryLatestBuildMessage.isEmpty() ? i18n("unknown error") : document.libraryLatestBuildMessage));
        warning = true;
    }
    if (!document.error.isEmpty()) {
        status.append(document.error);
        warning = true;
    }
    const QString syncState = syncPresentation.value(QStringLiteral("state")).toString();
    if (syncState == QLatin1String("error") || syncState == QLatin1String("attention") || syncState == QLatin1String("conflict")) {
        warning = true;
    }
    if (sourceLinked && !hasComponents) {
        status.append(i18n("The verified document map is unavailable. All comments remain available at project level."));
    }
    return {status.join(QLatin1Char('\n')), warning ? QStringLiteral("dialog-warning") : sourceLinked ? QStringLiteral("folder-documents") : QStringLiteral("application-pdf")};
}

// The documents' Sync states changed and nothing else: every document's row
// gets its tooltip and icon in place, in both tabs. Rebuilding the whole tree
// for this, as a Sync that starts did for every document at once, kept the
// window from answering.
void CommentsOverview::refreshDocumentListSyncPresentations()
{
    if (m_syncContentDeferred) {
        return;
    }
    QHash<QString, const TrackedCommentDocument *> documents;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (!document.sourceProposalOnly && !document.sourceUnselected) {
            documents.insert(document.stem, &document);
        }
    }
    for (QTreeWidget *tree : {m_commentsBrowser.documentList, m_notesBrowser.documentList}) {
        if (!tree) {
            continue;
        }
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            QTreeWidgetItem *item = *it;
            if (item->data(0, DocumentItemKindRole).toInt() != DocumentListEntry) {
                continue;
            }
            const TrackedCommentDocument *document = documents.value(item->data(0, Qt::UserRole).toString());
            if (!document) {
                continue;
            }
            const DocumentRowPresentation row = documentRowPresentation(*document, item->childCount() > 0);
            if (item->toolTip(0) != row.toolTip) {
                item->setToolTip(0, row.toolTip);
            }
            if (item->data(0, DocumentRowIconRole).toString() != row.iconName) {
                item->setIcon(0, QIcon::fromTheme(row.iconName));
                item->setData(0, DocumentRowIconRole, row.iconName);
            }
        }
    }
}

void CommentsOverview::populateDocumentList(const QString &selectedStem)
{
    mergeDocumentFeedback();
    if (m_syncContentDeferred) { return; }
    const DeveloperActivity activity(QStringLiteral("library.navigation"));
    QString selectedKey;
    QSet<QString> expanded;
    const int scroll = m_documentList->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(m_documentList); *it; ++it) {
        const QString key = (*it)->data(0, DocumentNavigationKeyRole).toString();
        if ((*it)->isExpanded()) {
            expanded.insert(key);
        }
        if (*it == m_documentList->currentItem()) {
            selectedKey = key;
        }
    }
    const QSignalBlocker blocker(m_documentList);
    m_documentList->clear();
    auto *allItem = new QTreeWidgetItem(m_documentList, {i18n("All items"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")});
    allItem->setData(0, DocumentItemKindRole, AllCommentsItem);
    allItem->setData(0, DocumentNavigationKeyRole, QStringLiteral("all"));
    QTreeWidgetItem *selectedItem = allItem;
    int allComments = 0;
    int allProposals = 0;
    int allNotes = 0;
    const auto setCounts = [](QTreeWidgetItem *item, int comments, int proposals, int notes) {
        item->setText(1, QString::number(comments));
        item->setText(2, QString::number(proposals));
        item->setText(3, QString::number(notes));
        for (int column = 1; column <= 3; ++column) {
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(column, threadCountTooltip(column, item->text(column).toInt()));
        }
    };
    const auto appendHeading = [this](const QString &text) {
        auto *item = new QTreeWidgetItem(m_documentList, {text});
        item->setFlags(Qt::NoItemFlags);
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        item->setFirstColumnSpanned(true);
        item->setData(0, DocumentItemKindRole, SectionHeadingItem);
    };
    struct Component {
        QString key;
        QString title;
        QString source;
        QSet<QString> sources;
        int startPage = 0;
        int pageCount = 0;
        QStringList threadIds;
        int comments = 0;
        int proposals = 0;
        int notes = 0;
    };
    const auto appendDocuments = [&] {
        // Projects are containers in the navigation tree exactly as they are in
        // the Library. A project keeps its documents whether or not each one has
        // editable source, so an imported PDF never leaves its project.
        struct ProjectNode {
            QTreeWidgetItem *item = nullptr;
            QString key;
            QString title;
            int comments = 0;
            int proposals = 0;
            int notes = 0;
        };
        // Values are heap-allocated so that a later insert cannot invalidate the
        // node a caller is still holding.
        QHash<QString, ProjectNode *> projectNodes;
        QStringList projectOrder;
        const auto ensureProject = [&](const TrackedCommentDocument &document) -> ProjectNode * {
            const QString identity = document.navigationProjectId.isEmpty() ? QStringLiteral("unassigned") : document.navigationProjectId;
            const auto found = projectNodes.constFind(identity);
            if (found != projectNodes.cend()) { return *found; }
            auto *node = new ProjectNode;
            node->key = QStringLiteral("library/active/") + identity;
            // The Library names the container after the project, so take that
            // name whenever the catalogue knows this document.
            node->title = document.navigationProjectName.isEmpty() ? i18n("No project") : document.navigationProjectName;
            node->item = new QTreeWidgetItem(m_documentList, {node->title});
            node->item->setData(0, DocumentItemKindRole, ProjectListEntry);
            node->item->setData(0, DocumentNavigationKeyRole, node->key);
            node->item->setData(0, DocumentLibraryProjectRole, identity);
            node->item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder-documents")));
            if (!document.navigationProjectTarget.isEmpty()) {
                node->item->setData(0, DocumentPathRole, QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/") + document.navigationProjectTarget));
            } else {
                node->item->setData(0, DocumentPathRole, document.pdfPath);
            }
            if (!document.libraryProject.isEmpty()) { node->item->setToolTip(0, document.libraryProject); }
            projectOrder.append(identity);
            projectNodes.insert(identity, node);
            return node;
        };
        for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
            const bool sourceLinked = document.state == TrackedCommentDocument::SourceLinked || document.libraryKind == QLatin1String("source_linked");
            // An unchecked document's kept PDF is not part of the Library.
            if ((document.stem.isEmpty() && document.pdfPath.isEmpty()) || document.sourceUnselected) {
                continue;
            }
            auto project = ensureProject(document);
            if (document.sourceProposalOnly) {
                QStringList proposalIds;
                for (const TrackedComment &comment : document.comments) {
                    if (visibleThread(comment, m_queuedAgentProposalIds)) proposalIds.append(comment.id);
                }
                auto *item = new QTreeWidgetItem(project->item, {i18n("Source proposals")});
                item->setData(0, DocumentItemKindRole, ThreadGroupListEntry);
                item->setData(0, DocumentNavigationKeyRole, QString(project->key + QStringLiteral("/source-proposals")));
                item->setData(0, DocumentScopeRole, 2);
                item->setData(0, DocumentThreadIdsRole, proposalIds);
                item->setIcon(0, QIcon::fromTheme(QStringLiteral("text-x-tex")));
                setCounts(item, 0, proposalIds.size(), 0);
                project->proposals += proposalIds.size();
                allProposals += proposalIds.size();
                if (item->data(0, DocumentNavigationKeyRole).toString() == selectedKey) selectedItem = item;
                continue;
            }
            auto *item = new QTreeWidgetItem(project->item, {document.displayName});
            item->setData(0, Qt::UserRole, document.stem);
            item->setData(0, DocumentPathRole, document.pdfPath);
            item->setData(0, DocumentItemKindRole, DocumentListEntry);
            item->setData(0, DocumentFilenameRole, document.libraryFilename);
            item->setData(0, DocumentKindRole, document.libraryKind);
            item->setData(0, DocumentSha256Role, document.librarySha256);
            item->setData(0, DocumentDisplayNameRole, document.displayName);
            const QString documentKey = project->key + QStringLiteral("/document/") + document.stem;
            item->setData(0, DocumentNavigationKeyRole, documentKey);
            item->setData(0, DocumentLibraryProjectRole, project->key);

            const auto absoluteSource = [&](const QString &source) {
                return source.isEmpty() ? QString() : document.navigationSourcePaths.value(source, QDir::cleanPath(QDir(m_docsRoot).absoluteFilePath(source)));
            };
            // The user's titles are read once per document, and the title a
            // component gets without one only where none was given.
            const QJsonObject titles = document.navigationOptions.value(QStringLiteral("titles")).toObject();
            const auto titleOf = [&titles](const QString &id, const QJsonObject &segment) {
                const QJsonValue given = titles.value(id);
                return given.isString() ? given.toString() : librarySegmentTitle(segment);
            };
            QList<Component> components;
            QSet<QString> componentKeys;
            for (const QJsonValue &value : document.librarySegments) {
                const QJsonObject segment = value.toObject();
                const QString sourceId = segment.value(QStringLiteral("source_id")).toString();
                if (documentExcluded(document.navigationOptions, QDir(m_docsRoot).filePath(document.libraryProject), sourceId)) { continue; }
                QString key = sourceId;
                if (key.isEmpty() || componentKeys.contains(key)) {
                    key += QStringLiteral("/occurrence/%1").arg(components.size());
                }
                componentKeys.insert(key);
                Component component;
                component.key = key;
                component.title = titleOf(sourceId, segment);
                component.source = absoluteSource(segment.value(QStringLiteral("primary_source_file")).toString(sourceId));
                component.sources.insert(component.source);
                for (const QJsonValue &input : segment.value(QStringLiteral("inputs")).toArray()) {
                    const QString source = absoluteSource(input.toObject().value(QStringLiteral("source_file")).toString());
                    if (!source.isEmpty()) {
                        component.sources.insert(source);
                    }
                }
                component.startPage = segment.value(QStringLiteral("start_page")).toInt();
                component.pageCount = segment.value(QStringLiteral("page_count")).toInt();
                components.append(component);
            }
            // Include documents added locally before their next published build.
            if (sourceLinked && !document.libraryProject.isEmpty()) {
                for (const auto &[entry, source] : document.standaloneSources) {
                    const QString entryId = document.libraryProject + QStringLiteral("/tex/source/") + entry;
                    if (source.isEmpty() || componentKeys.contains(entryId)
                        || std::any_of(components.cbegin(), components.cend(), [&](const Component &component) { return component.source == source; })) {
                        continue;
                    }
                    Component component;
                    component.key = entryId;
                    component.title = titleOf(entryId, QJsonObject {{QStringLiteral("source_id"), entryId}});
                    component.source = source;
                    component.sources.insert(source);
                    components.append(component);
                    componentKeys.insert(entryId);
                }
            }
            Component unassigned;
            unassigned.key = QStringLiteral("unassigned");
            unassigned.title = i18n("Unassigned to a document");
            int comments = 0;
            int proposals = 0;
            int notes = 0;
            for (const TrackedComment &comment : document.comments) {
                if (!visibleThread(comment, m_queuedAgentProposalIds)) {
                    continue;
                }
                const int column = threadCountColumn(comment);
                if (column == 0) {
                    continue;
                }
                column == 2 ? ++proposals : column == 3 ? ++notes : ++comments;
                QSet<int> matches;
                const auto matchPage = [&](int page) {
                    for (int i = 0; i < components.size(); ++i) {
                        const Component &component = components.at(i);
                        if (component.startPage > 0 && page >= component.startPage && page - component.startPage < component.pageCount) {
                            matches.insert(i);
                        }
                    }
                };
                if (!comment.sourceOnly && comment.anchorStatus != QLatin1String("needs_anchor") && comment.anchorStatus != QLatin1String("needs_attention")) {
                    for (const QJsonValue &value : comment.pdfLocations) {
                        const QJsonObject location = value.toObject();
                        const QString sha256 = location.value(QStringLiteral("pdf_sha256")).toString();
                        if (location.value(QStringLiteral("pdf_filename")).toString() == QFileInfo(document.pdfPath).fileName()
                            && (sha256.isEmpty() || sha256 == document.librarySha256)) {
                            matchPage(location.value(QStringLiteral("page")).toInt());
                        }
                    }
                    if (comment.pdfLocations.isEmpty()) {
                        matchPage(comment.page);
                    }
                }
                if (!comment.sourceFile.isEmpty()) {
                    // Source ownership wins over stale page numbers after composition changes.
                    matches.clear();
                    const QString source = absoluteSource(comment.sourceFile);
                    for (int i = 0; i < components.size(); ++i) {
                        if (components.at(i).sources.contains(source)) {
                            matches.insert(i);
                        }
                    }
                }
                const auto countThread = [&](Component &component) {
                    component.threadIds.append(comment.id);
                    threadCountColumn(comment) == 2 ? ++component.proposals : threadCountColumn(comment) == 3 ? ++component.notes : ++component.comments;
                };
                if (matches.isEmpty()) {
                    countThread(unassigned);
                } else {
                    for (int i : std::as_const(matches)) {
                        countThread(components[i]);
                    }
                }
            }
            setCounts(item, comments, proposals, notes);
            project->comments += comments;
            project->proposals += proposals;
            project->notes += notes;
            allComments += comments;
            allProposals += proposals;
            allNotes += notes;
            const DocumentRowPresentation row = documentRowPresentation(document, !components.isEmpty());
            item->setIcon(0, QIcon::fromTheme(row.iconName));
            item->setData(0, DocumentRowIconRole, row.iconName);
            item->setToolTip(0, row.toolTip);
            if (!components.isEmpty() && !unassigned.threadIds.isEmpty()) {
                components.append(unassigned);
            }
            for (const Component &component : std::as_const(components)) {
                auto *child = new QTreeWidgetItem(item, {component.title});
                child->setData(0, Qt::UserRole, document.stem);
                child->setData(0, DocumentPathRole, document.pdfPath);
                child->setData(0, DocumentItemKindRole, ComponentListEntry);
                child->setData(0, DocumentThreadIdsRole, component.threadIds);
                child->setData(0, DocumentStartPageRole, component.startPage);
                child->setData(0, DocumentCompositionIdRole, component.key == QLatin1String("unassigned") ? QString() : component.key);
                const QString key = project->key + QStringLiteral("/document/") + document.stem + QStringLiteral("/component/") + component.key;
                child->setData(0, DocumentNavigationKeyRole, key);
                child->setIcon(0, QIcon::fromTheme(component.key == QLatin1String("unassigned") ? QStringLiteral("dialog-information") : QStringLiteral("text-x-tex")));
                child->setToolTip(0, component.key == QLatin1String("unassigned")
                                       ? i18n("These threads have no verified document location. They remain included in the project totals.")
                                       : component.startPage > 0 ? component.source : i18n("%1\nNot in the published document map yet.", component.source));
                setCounts(child, component.comments, component.proposals, component.notes);
                if (key == selectedKey) {
                    selectedItem = child;
                }
            }
            item->setExpanded(expanded.contains(documentKey));
            if (documentKey == selectedKey) {
                // The document itself was selected, so it wins over any
                // component that matched the same stem.
                selectedItem = item;
            }
            if (document.stem == selectedStem && selectedItem == allItem) {
                selectedItem = item;
            }
        }
        for (const QString &identity : std::as_const(projectOrder)) {
            const ProjectNode *node = projectNodes.value(identity);
            if (!node) { continue; }
            setCounts(node->item, node->comments, node->proposals, node->notes);
            node->item->setExpanded(expanded.contains(node->key));
            if (node->key == selectedKey) {
                selectedItem = node->item;
            }
        }
        // Delete the node records, not the hash iterators qDeleteAll would take.
        for (ProjectNode *node : std::as_const(projectNodes)) {
            delete node;
        }
        projectNodes.clear();
    };
    const auto addGroup = [&](QTreeWidgetItem *parent, const QString &title, const QString &key, int scope, const QStringList &ids, int comments, int proposals, int notes) {
        auto *item = parent ? new QTreeWidgetItem(parent, {title}) : new QTreeWidgetItem(m_documentList, {title});
        item->setData(0, DocumentItemKindRole, ThreadGroupListEntry);
        item->setData(0, DocumentNavigationKeyRole, key);
        item->setData(0, DocumentScopeRole, scope);
        item->setData(0, DocumentThreadIdsRole, ids);
        setCounts(item, comments, proposals, notes);
        if (selectedKey == key) { selectedItem = item; }
        return item;
    };
    QStringList noteIds, commentIds, proposalIds, projectIds;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        const bool noProject = document.stem.isEmpty() && document.pdfPath.isEmpty();
        for (const TrackedComment &comment : document.comments) {
            if (!visibleThread(comment, m_queuedAgentProposalIds) || threadCountColumn(comment) == 0) { continue; }
            if (!noProject) { projectIds.append(comment.id); continue; }
            (threadCountColumn(comment) == 3 ? noteIds : threadCountColumn(comment) == 2 ? proposalIds : commentIds).append(comment.id);
        }
    }
    auto *noProject = addGroup(nullptr, i18n("No project"), QStringLiteral("no-project"), 1, noteIds + commentIds + proposalIds, commentIds.size(), proposalIds.size(), noteIds.size());
    addGroup(noProject, i18n("To-dos"), QStringLiteral("no-project/notes"), 1, noteIds, 0, 0, noteIds.size());
    addGroup(noProject, i18n("Unassigned comments"), QStringLiteral("no-project/comments"), 1, commentIds, commentIds.size(), 0, 0);
    if (!proposalIds.isEmpty()) {
        addGroup(noProject, i18n("Agent proposals"), QStringLiteral("no-project/proposals"), 1, proposalIds, 0, proposalIds.size(), 0);
    }
    noProject->setExpanded(selectedKey.isEmpty() || expanded.contains(QStringLiteral("no-project")));
    auto *projects = addGroup(nullptr, i18n("Projects and PDFs"), QStringLiteral("projects-and-pdfs"), 2, projectIds, 0, 0, 0);
    // Every document appears under the project and folder the Library places it
    // in. Imported PDFs without editable source are documents like any other,
    // so they are listed here instead of in a separate PDF-only section.
    appendHeading(i18n("Projects"));
    appendDocuments();
    setCounts(projects, allComments, allProposals, allNotes);
    setCounts(allItem, allComments + commentIds.size(), allProposals + proposalIds.size(), allNotes + noteIds.size());
    QStringList staleDocumentNames;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (document.libraryPdfStale) {
            staleDocumentNames.append(document.displayName);
        }
    }
    if (!staleDocumentNames.isEmpty()) {
        allItem->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
        allItem->setToolTip(0, i18np("Library updated with errors: %1 document is using its last-good PDF.",
                                  "Library updated with errors: %1 documents are using their last-good PDFs.",
                                  staleDocumentNames.size())
                            + QLatin1Char('\n') + i18n("Affected documents: %1", staleDocumentNames.join(QStringLiteral(", "))));
    }

    m_documentList->setCurrentItem(selectedItem);
    // Reveal the selection through its project container. A component needs its
    // own document expanded, but a selected document must keep the collapsed
    // state the user chose; only its ancestors are opened.
    m_documentList->verticalScrollBar()->setValue(scroll);
    mirrorDocumentNavigation();
    applyBrowserFilters(m_commentsBrowser);
    applyBrowserFilters(m_notesBrowser);
}

QList<QStandardItem *> CommentsOverview::makeCommentRow(const TrackedCommentDocument &document, const TrackedComment &comment) const
{
    QString commentText = comment.contents.trimmed();
    if (commentText.isEmpty()) {
        commentText = i18nc("@item:inlistbox annotation without comment text", "(No comment text)");
    }

    const bool note = comment.logicalPayload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note");
    const bool noProject = document.stem.isEmpty() && document.pdfPath.isEmpty();
    const QString attachedPage = comment.feedbackRecord.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString();
    QString source;
    if (noProject && (comment.agentProposal || !comment.missingMarker.isEmpty()) && !comment.sourceFile.isEmpty()) {
        source = comment.sourceLine > 0 ? i18nc("@item source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
    } else if (noProject || note) {
        source = i18n("No project");
    } else if ((comment.agentProposal || comment.logicalComment || !comment.missingMarker.isEmpty()) && !comment.sourceFile.isEmpty()) {
        source = comment.sourceLine > 0 ? i18nc("@item source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
    } else if (!attachedPage.isEmpty()) {
        source = attachedPage;
    } else if (document.state == TrackedCommentDocument::PdfOnly) {
        source = i18nc("@item comment without a TeX source location", "PDF only");
    } else if (!comment.sourceFile.isEmpty()) {
        source = comment.sourceLine > 0 ? i18nc("@item source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
    } else if (!comment.feedbackKind.isEmpty()) {
        // A comment on a passage is made on the PDF, not on a line of its source.
        source = QStringLiteral("—");
    } else {
        source = i18nc("@item comment without a TeX source location", "PDF only");
    }

    QString status;
    int statusRank = 2;
    const QString correspondenceState = comment.correspondence.value(QStringLiteral("state")).toString();
    if (note) {
        status = i18n("Note");
        statusRank = 0;
    } else if (comment.anchorStatus == QLatin1String("comment_missing")) {
        status = i18n("Comment missing");
        statusRank = 1;
    } else if (comment.agentProposal && comment.anchorStatus == QLatin1String("proposal_removed")) {
        status = i18n("Removed outside Thoughts");
        statusRank = 1;
    } else if (noProject) {
        status = i18n("No project");
    } else if (comment.agentProposal) {
        const bool edit = comment.proposalKind == QLatin1String("edit");
        if (correspondenceState == QLatin1String("needs_attention") || correspondenceState == QLatin1String("linking")) {
            status = edit ? i18n("Edit proposal · Needs placement review") : i18n("Removal proposal · Needs placement review");
            statusRank = 1;
        } else if (correspondenceState == QLatin1String("source_only") || comment.anchorStatus != QLatin1String("anchored")) {
            status = edit ? i18n("Edit proposal · Source-only") : i18n("Removal proposal · Source-only");
            statusRank = 1;
        } else {
            status = edit ? i18n("Edit proposal · Linked to source") : i18n("Removal proposal · Linked to source");
            statusRank = 0;
        }
    } else if (comment.logicalComment && comment.anchorStatus == QLatin1String("source_linked")) {
        const bool placed = comment.pdfAvailable && std::any_of(comment.pdfLocations.cbegin(), comment.pdfLocations.cend(), [](const QJsonValue &value) {
            const auto location = value.toObject();
            if (location.value(QStringLiteral("status")).toString() != QLatin1String("exact")
                || location.value(QStringLiteral("page")).toInt() < 1) return false;
            const auto regions = location.value(QStringLiteral("regions")).toArray();
            if (!regions.isEmpty()) return std::all_of(regions.cbegin(), regions.cend(), [](const QJsonValue &region) {
                return Thoughts::validNormalizedReviewCoordinates(region.toArray(), 4);
            });
            return Thoughts::validNormalizedReviewCoordinates(location.value(QStringLiteral("point")).toArray(), 2);
        });
        status = placed ? i18n("Linked") : i18n("Linked but PDF-placement failed");
        statusRank = placed ? 0 : 1;
    } else if (comment.anchorStatus == QLatin1String("anchor_removed")) {
        status = i18n("Anchor removed");
        statusRank = 1;
    } else if (comment.anchorStatus == QLatin1String("source_unavailable")) {
        status = i18n("Source location unavailable"); statusRank = 1;
    } else if (comment.logicalPayload.value(QStringLiteral("source_authority")).toString() == QLatin1String("client-v1")) {
        status = comment.anchorStatus == QLatin1String("pdf_only") ? i18n("PDF without source location") : i18n("Linking failed");
        statusRank = comment.anchorStatus == QLatin1String("pdf_only") ? 0 : 1;
    } else if (correspondenceState == QLatin1String("source_only")) {
        status = i18n("Source-only");
        statusRank = 0;
    } else if (correspondenceState == QLatin1String("linking")) {
        status = i18n("Linking to source…");
        statusRank = 1;
    } else if (correspondenceState == QLatin1String("no_source")) {
        status = i18n("Unlinked PDF");
    } else if (comment.logicalComment && comment.anchorStatus == QLatin1String("source_linked")) {
        status = i18np("Linked to source · %1 PDF location", "Linked to source · %1 PDF locations", comment.pdfLocations.size());
        statusRank = 0;
    } else if (comment.logicalComment && comment.anchorStatus == QLatin1String("needs_attention")) {
        status = i18n("Lost location");
        statusRank = 1;
    } else if (comment.logicalComment || document.state == TrackedCommentDocument::PdfOnly) {
        status = i18n("Unlinked PDF");
    } else if (comment.anchorStatus == QLatin1String("anchored")) {
        status = i18n("Anchored");
        statusRank = 0;
    } else if (comment.anchorStatus == QLatin1String("source_projected")) {
        status = i18n("Linked to source");
        statusRank = 0;
    } else if (comment.anchorStatus == QLatin1String("needs_anchor")) {
        status = i18n("Needs anchor");
        statusRank = 1;
    } else if (comment.anchorStatus == QLatin1String("orphaned")) {
        status = i18n("Unlinked from source");
        statusRank = 1;
    } else {
        status = i18n("Unlinked PDF");
    }

    const QString date = comment.displayDate.isValid() ? comment.displayDate.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("—");
    const QString effectivePriority = comment.priorityRaw.isEmpty() ? QStringLiteral("normal") : comment.priorityRaw;
    if (!comment.feedbackKind.isEmpty()) {
        QStringList parts {feedbackKindLabel(comment.feedbackKind)};
        if (Thoughts::DocumentFeedback::isResolved(comment.feedbackRecord)) parts.append(i18n("Resolved"));
        else if (comment.feedbackEarlier) parts.append(i18n("Earlier version"));
        status = parts.join(QStringLiteral(" · "));
        statusRank = 0;
    }
    // Which version the comment was made on, where a document has versions at all.
    const QString snapshot = document.state != TrackedCommentDocument::SourceLinked ? QString()
        : !comment.feedbackKind.isEmpty() ? comment.feedbackSnapshot
        : !comment.logicalPayload.isEmpty() ? Thoughts::PdfVersions::recordedSha256(comment.logicalPayload) : comment.annotationPdfSha256;
    const QString currentVersion = comment.feedbackKind.isEmpty() ? document.librarySha256 : comment.feedbackCurrentSnapshot;
    const bool currentSnapshot = !snapshot.isEmpty() && snapshot.compare(currentVersion, Qt::CaseInsensitive) == 0;
    const QString snapshotDisplay = snapshot.isEmpty() ? QStringLiteral("—") : currentSnapshot ? i18n("Current") : i18n("Earlier · %1", snapshot.left(8));
    const QString locationDisplay = comment.locationValid ? comment.location.value(QStringLiteral("label")).toString() : comment.locationPresent ? i18n("Invalid location") : QStringLiteral("—");
    QStringList pdfLocationLabels;
    QStringList pdfLocationPages;
    QSet<QPair<QString, int>> displayedPdfPages;
    for (const QJsonValue &value : comment.pdfLocations) {
        const QJsonObject placement = value.toObject();
        const QString filename = placement.value(QStringLiteral("pdf_filename")).toString();
        const int page = placement.value(QStringLiteral("page")).toInt();
        const auto pageKey = qMakePair(filename.toCaseFolded(), page);
        if (displayedPdfPages.contains(pageKey)) {
            continue;
        }
        displayedPdfPages.insert(pageKey);
        const auto tracked = std::find_if(m_documents.cbegin(), m_documents.cend(), [&filename](const TrackedCommentDocument &candidate) {
            return QFileInfo(candidate.pdfPath).fileName().compare(filename, Qt::CaseInsensitive) == 0;
        });
        const QString name = tracked == m_documents.cend() ? QFileInfo(filename).completeBaseName() : tracked->displayName;
        pdfLocationLabels.append(page > 0 ? i18n("%1 · p. %2", name, page) : name);
        if (page > 0) {
            pdfLocationPages.append(QString::number(page));
        }
    }
    const QString documentDisplay = pdfLocationLabels.isEmpty() ? document.displayName : pdfLocationLabels.join(QStringLiteral("; "));
    const QString pageDisplay = pdfLocationPages.isEmpty() ? (comment.page > 0 ? QString::number(comment.page) : QStringLiteral("—")) : pdfLocationPages.join(QStringLiteral(", "));
    const QDateTime modifiedDate = comment.agentProposal ? comment.modifiedDate : comment.rootSortDate;
    QList<QStandardItem *> row {
        makeItem(commentText, commentText),
        makeItem(documentDisplay, documentDisplay),
        makeItem(pageDisplay, comment.page),
        makeItem(snapshotDisplay, snapshot.isEmpty() ? 2 : currentSnapshot ? 0 : 1),
        makeItem(source, source),
        makeItem(status, statusRank),
        makeItem(comment.author, comment.author),
        makeItem(date + (comment.dateIsFirstSeen ? i18n(" (first seen)") : QString()), comment.displayDate),
        makeItem(modifiedDate.isValid() ? modifiedDate.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("—"), comment.rootSortDate),
        makeItem(document.state == TrackedCommentDocument::SourceLinked ? QStringLiteral("✓") : QStringLiteral("✗"), document.state == TrackedCommentDocument::SourceLinked ? 0 : 1),
    };
    row.at(SourceDocumentColumn)->setTextAlignment(Qt::AlignCenter);
    row.at(SourceDocumentColumn)->setToolTip(document.state == TrackedCommentDocument::SourceLinked ? i18n("Source document (LaTeX or Markdown)") : i18n("PDF"));

    const QString coordinates = comment.locationValid ? QStringLiteral("%1, %2").arg(comment.latitude, 0, 'f', 6).arg(comment.longitude, 0, 'f', 6) : QString();
    const QString searchText =
        QStringList {comment.contents, comment.selectedText, comment.documentStem, document.displayName, comment.author, comment.sourceFile, status, snapshotDisplay, effectivePriority, locationDisplay, coordinates,
                     comment.agentProposal ? (comment.proposalKind == QLatin1String("edit") ? i18n("edit proposal") : i18n("removal proposal")) : comment.feedbackKind.isEmpty() ? i18n("comment") : feedbackKindLabel(comment.feedbackKind)}
            .join(QLatin1Char('\n'));
    // Keep nested placement/proof data in its implicitly shared JSON form.
    // Expanding it into QVariant containers per column duplicates every nested
    // record and stalls the UI when a proposal spans thousands of PDF regions.
    const QVariant location = QVariant::fromValue(comment.location);
    const QVariant logicalPayload = QVariant::fromValue(comment.logicalPayload);
    const QVariant pdfLocations = QVariant::fromValue(comment.pdfLocations);
    const QVariant correspondence = QVariant::fromValue(comment.correspondence);
    for (QStandardItem *item : std::as_const(row)) {
        item->setData(comment.documentStem, DocumentRole);
        item->setData(document.displayName, DocumentDisplayRole);
        item->setData(comment.pdfPath, PdfPathRole);
        item->setData(comment.id, AnnotationIdRole);
        item->setData(comment.threadRootId, ThreadRootIdRole);
        item->setData(comment.page, PageRole);
        item->setData(comment.pdfAvailable, PdfAvailableRole);
        item->setData(searchText, SearchRole);
        item->setData(comment.contents, ContentsRole);
        item->setData(comment.selectedText, SelectedTextRole);
        item->setData(source, SourceDisplayRole);
        item->setData(comment.sourceFile, SourceFileRole);
        item->setData(comment.sourceLine, SourceLineRole);
        item->setData(comment.anchorStatus, AnchorStatusRole);
        item->setData(status, StatusDisplayRole);
        item->setData(comment.author, AuthorRole);
        item->setData(effectivePriority, PriorityRole);
        item->setData(comment.locationPresent, LocationPresentRole);
        item->setData(comment.location.value(QStringLiteral("label")).toString(), LocationLabelRole);
        item->setData(comment.locationValid ? comment.latitude : 0.0, LatitudeRole);
        item->setData(comment.locationValid ? comment.longitude : 0.0, LongitudeRole);
        item->setData(location, LocationObjectRole);
        item->setData(comment.chatGptUrl, ChatGptUrlRole);
        item->setData(comment.replyOrder, ReplyOrderRole);
        item->setData(note ? QStringLiteral("note") : comment.agentProposal ? QStringLiteral("agent_remove") : !comment.feedbackKind.isEmpty() ? QStringLiteral("feedback") : QStringLiteral("comment"), ItemTypeRole);
        item->setData(comment.feedbackKind, FeedbackKindRole);
        item->setData(QVariant::fromValue(comment.feedbackRecord), FeedbackRecordRole);
        item->setData(comment.bundleId, BundleIdRole);
        item->setData(comment.proposalSuppressed, ProposalSuppressedRole);
        item->setData(comment.proposalKind, ProposalKindRole);
        item->setData(comment.proposalBeforePreview, ProposalBeforePreviewRole);
        item->setData(comment.proposalAfterPreview, ProposalAfterPreviewRole);
        item->setData(comment.proposalSourceSha256, ProposalSourceSha256Role);
        item->setData(comment.liveSourceProposal, LiveSourceProposalRole);
        item->setData(comment.proposalIssue, ProposalIssueRole);
        item->setData(QVariant::fromValue(comment.removedProposal), RemovedProposalRole);
        item->setData(comment.logicalComment, LogicalCommentRole);
        item->setData(logicalPayload, LogicalPayloadRole);
        item->setData(pdfLocations, PdfLocationsRole);
        item->setData(correspondence, CorrespondenceRole);
        item->setData(QVariant::fromValue(comment.missingMarker), MissingMarkerRole);
        item->setToolTip(comment.contents.isEmpty() ? comment.selectedText : comment.contents);
        if ((!comment.pdfAvailable && !comment.liveSourceProposal && !note) || comment.id.isEmpty()) {
            item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        }
        if (comment.locationPresent && !comment.locationValid) {
            item->setToolTip(i18n("This comment contains metadata that Thoughts will preserve until you replace or clear it."));
        }
    }
    if (!snapshot.isEmpty()) row.at(SnapshotColumn)->setToolTip(i18n("Version %1", snapshot));
    if (comment.anchorStatus == QLatin1String("proposal_removed")) {
        QStandardItem *statusItem = row.at(StatusColumn);
        statusItem->setIcon(QIcon::fromTheme(QStringLiteral("link-broken"), QIcon::fromTheme(QStringLiteral("dialog-warning"))));
        const QString location = comment.sourceLine > 0 ? i18nc("@info source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
        const QString looksLike = comment.removedProposal.value(QStringLiteral("removal")).toObject().value(QStringLiteral("looks_like")).toString();
        const QString hint = looksLike == QLatin1String("accept") ? i18n(" The source now reads as if it was accepted.")
                           : looksLike == QLatin1String("reject") ? i18n(" The source now reads as if it was rejected.")
                           : looksLike == QLatin1String("file_deleted") ? i18n(" Its file was deleted.") : QString();
        statusItem->setToolTip(i18n("Its markup was removed from %1 without Accept or Reject in Thoughts.%2 Restore Proposal puts it back; Dismiss forgets it.", location, hint));
    }
    if (comment.anchorStatus == QLatin1String("anchor_removed")) {
        QStandardItem *statusItem = row.at(StatusColumn);
        statusItem->setIcon(QIcon::fromTheme(QStringLiteral("link-broken"), QIcon::fromTheme(QStringLiteral("dialog-warning"))));
        const QString location = comment.sourceFile.isEmpty() ? i18n("its source") : comment.sourceLine > 0 ? i18nc("@info source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
        statusItem->setToolTip(i18n("Its marker was removed from %1. Restore re-inserts it; Delete removes the comment.", location));
    }
    if (comment.anchorStatus == QLatin1String("comment_missing")) {
        QStandardItem *statusItem = row.at(StatusColumn);
        statusItem->setIcon(QIcon::fromTheme(QStringLiteral("link-broken"), QIcon::fromTheme(QStringLiteral("dialog-warning"))));
        const QString location = comment.sourceLine > 0 ? i18nc("@info source file and line", "%1:%2", comment.sourceFile, comment.sourceLine) : comment.sourceFile;
        const bool deleted = comment.missingMarker.value(QStringLiteral("state")).toString() == QLatin1String("deleted");
        statusItem->setToolTip(deleted ? i18n("The marker at %1 belongs to a comment that was deleted. Remove Marker takes it out of the source; Restore Comment brings the comment back.", location)
                                       : i18n("The marker at %1 belongs to no known comment. Remove Marker takes it out of the source; Restore Comment creates the comment from it.", location));
    }
    return row;
}

void CommentsOverview::mergeDocumentFeedback()
{
    const bool showEarlier = m_showEarlierFeedback && m_showEarlierFeedback->isChecked();
    QHash<QString, QList<QJsonObject>> byDocument;
    const QString prefix = Thoughts::DocumentFeedback::entityType() + QLatin1Char('\n') + Thoughts::DocumentFeedback::collection() + QLatin1Char('\n');
    for (auto it = m_fedoraProjections.constBegin(); it != m_fedoraProjections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix)) continue;
        const QJsonObject record = it.value().toObject();
        if (!record.value(QStringLiteral("deleted")).toBool()) byDocument[Thoughts::DocumentFeedback::documentIdOf(record)].append(record);
    }
    for (TrackedCommentDocument &document : m_documents) {
        document.comments.removeIf([](const TrackedComment &comment) { return !comment.feedbackKind.isEmpty(); });
        if (document.libraryDocumentId.isEmpty()) continue;
        QHash<QString, QString> attachmentVersions;
        const QList<QJsonObject> records = byDocument.value(document.libraryDocumentId);
        for (const QJsonObject &record : records) {
            const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
            const QString quote = anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString();
            const QString written = record.value(QStringLiteral("comment")).toString();
            const QString page = anchor.value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString();
            if (!page.isEmpty() && !attachmentVersions.contains(page)) {
                QString error;
                attachmentVersions.insert(page, Thoughts::resolveHtmlAttachment(page, document.pdfPath, &error).sha256);
            }
            const auto regions = Thoughts::ReviewCardFeedback::regionsOf(record);
            TrackedComment comment;
            comment.id = record.value(QStringLiteral("feedback_id")).toString();
            comment.threadRootId = comment.id;
            comment.documentStem = document.stem;
            comment.pdfPath = document.pdfPath;
            comment.pdfAvailable = true;
            comment.contents = written.isEmpty() ? quote : written;
            comment.selectedText = quote;
            comment.priorityRaw = QStringLiteral("normal");
            comment.page = regions.isEmpty() ? 0 : regions.constFirst().first;
            comment.displayDate = QDateTime::fromString(record.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
            comment.modifiedDate = QDateTime::fromString(record.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
            comment.rootSortDate = comment.modifiedDate;
            comment.feedbackKind = feedbackKindOf(record);
            comment.feedbackRecord = record;
            // Without the current PDF's identity nothing can be called earlier.
            comment.feedbackEarlier = !document.librarySha256.isEmpty() && !Thoughts::DocumentFeedback::shownOn(record, document.librarySha256, attachmentVersions.value(page));
            // Left out here, so the counts, the tree and the table agree on what is listed.
            if (comment.feedbackEarlier && !showEarlier) continue;
            comment.feedbackSnapshot = page.isEmpty() ? Thoughts::PdfVersions::recordedSha256(record) : anchor.value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("sha256")).toString();
            comment.feedbackCurrentSnapshot = page.isEmpty() ? document.librarySha256 : attachmentVersions.value(page);
            document.comments.append(comment);
        }
    }
}

void CommentsOverview::populateComments()
{
    mergeDocumentFeedback();
    // Stable rows let both tabs retain selection, expansion and scroll position.
    // Catalogue scans may cover every record; notifications cover only changes.
    const QScopedValueRollback<bool> updatingModel(m_updatingCommentModel, true);
    const QScopedValueRollback<bool> changingSelection(m_changingSelection, true);
    const auto keyFor = [](const QString &pdf, const QString &id) { return pdf + QChar(0x1f) + id; };
    QHash<QString, QString> parents;
    QHash<QString, TrackedCommentDocument> metadata;
    for (const auto &document : std::as_const(m_documents)) {
        auto descriptor = document;
        descriptor.comments.clear();
        metadata.insert(document.pdfPath, descriptor);
        QSet<QString> roots;
        for (const auto &comment : document.comments) {
            if (comment.agentProposal && m_queuedAgentProposalIds.contains(comment.id)) { continue; }
            if (comment.threadRootId == comment.id) { roots.insert(comment.id); }
        }
        for (const auto &comment : document.comments) {
            if (comment.agentProposal && m_queuedAgentProposalIds.contains(comment.id)) { continue; }
            if (comment.threadRootId != comment.id && !comment.reply) { continue; }
            const QString key = keyFor(document.pdfPath, comment.id);
            parents.insert(key, comment.reply && roots.contains(comment.threadRootId) && comment.threadRootId != comment.id
                ? keyFor(document.pdfPath, comment.threadRootId) : QString());
        }
    }
    QHash<QString, QStandardItem *> existing;
    const auto prune = [&](auto &&self, QStandardItem *parent, const QString &parentKey) -> void {
        for (int row = parent->rowCount() - 1; row >= 0; --row) {
            auto *item = parent->child(row, CommentColumn);
            const QString key = keyFor(item->data(PdfPathRole).toString(), item->data(AnnotationIdRole).toString());
            if (!parents.contains(key) || parents.value(key) != parentKey) {
                parent->removeRow(row);
            } else {
                existing.insert(key, item);
                self(self, item, key);
            }
        }
    };
    prune(prune, m_model->invisibleRootItem(), QString());
    QHash<QString, TrackedComment> rendered;
    const auto upsert = [&](const TrackedCommentDocument &document, const TrackedComment &comment) {
        const QString key = keyFor(document.pdfPath, comment.id);
        if (!parents.contains(key)) { return; }
        const QString parentKey = parents.value(key);
        auto *parent = parentKey.isEmpty() ? m_model->invisibleRootItem() : existing.value(parentKey);
        if (!parent) { return; }
        auto *item = existing.value(key);
        const bool changed = !item || !m_renderedComments.contains(key) || m_renderedComments.value(key) != comment
            || !m_renderedDocumentMetadata.contains(document.pdfPath)
            || m_renderedDocumentMetadata.value(document.pdfPath) != metadata.value(document.pdfPath);
        if (!item) {
            const auto row = makeCommentRow(document, comment);
            parent->appendRow(row);
            existing.insert(key, row.first());
        } else if (changed) {
            QStandardItemModel replacement;
            replacement.appendRow(makeCommentRow(document, comment));
            for (int column = 0; column < ColumnCount; ++column) {
                const auto index = m_model->index(item->row(), column, parent->index());
                const auto oldData = m_model->itemData(index);
                auto newData = replacement.itemData(replacement.index(0, column));
                if (oldData == newData) { continue; }
                for (auto it = oldData.cbegin(); it != oldData.cend(); ++it) {
                    if (!newData.contains(it.key())) { newData.insert(it.key(), QVariant()); }
                }
                m_model->setItemData(index, newData);
            }
        }
        rendered.insert(key, comment);
    };
    for (const auto &document : std::as_const(m_documents)) {
        for (const auto &comment : document.comments) {
            if (parents.contains(keyFor(document.pdfPath, comment.id)) && parents.value(keyFor(document.pdfPath, comment.id)).isEmpty()) {
                upsert(document, comment);
            }
        }
        for (const auto &comment : document.comments) {
            if (!parents.value(keyFor(document.pdfPath, comment.id)).isEmpty()) { upsert(document, comment); }
        }
    }
    m_renderedComments = std::move(rendered);
    m_renderedDocumentMetadata = std::move(metadata);
    populateDiary();
}

void CommentsOverview::requestCatalogCheck(bool inventory)
{
    m_libraryInventoryDirty = m_libraryInventoryDirty || inventory;
    m_catalogCheckRequested = true;
    if (!m_catalogPollTimer->isActive()) {
        m_catalogPollTimer->start();
    }
}

void CommentsOverview::handleDocsPathsChanged(const QStringList &paths)
{
    // Everything the catalogue signature and the Library inventory read:
    // comment sidecars, layouts and proposal history, the published Library,
    // Library project settings, and project sources.
    const QString root = QDir::cleanPath(m_docsRoot);
    const QString comments = root + QStringLiteral("/comments");
    const QString settings = root + QStringLiteral("/.thoughts");
    const QString inventory = settings + QStringLiteral("/library-source-inventory.json");
    bool catalog = false;
    bool sources = false;
    QSet<QString> sourceDocuments;
    bool unmappedSource = false;
    // The Sources tree lists every file its folder's .ThoughtIgnore keeps, so
    // exactly that policy decides which changes rescan it. Source folders are
    // the docs-* folders and each project's default folder under .thoughts.
    QHash<QString, std::optional<Thoughts::ThoughtIgnore>> ignores;
    const auto isSourcePath = [&](const QString &path, const QString &owner, const QString &relative) {
        const QString inside = relative.mid(owner.size() + 1);
        if (inside.isEmpty() || inside == QLatin1String(".ThoughtIgnore")) {
            return true;
        }
        auto &ignore = ignores[owner];
        if (!ignore) {
            ignore.emplace(root + QLatin1Char('/') + owner);
        }
        // An unreadable policy fails the rescan, which reports it.
        return !ignore->error().isEmpty() || !ignore->excludes(inside, QFileInfo(path).isDir());
    };
    for (const QString &path : paths) {
        if (path == inventory) {
            continue; // written by the check itself
        }
        const QString relative = path.mid(root.size() + 1);
        const QString topLevel = relative.section(QLatin1Char('/'), 0, 0);
        const QString owner = Thoughts::SourceOwner::of(relative);
        if (Thoughts::DocsChangeMonitor::isWithin(path, comments) || Thoughts::DocsChangeMonitor::isWithin(path, settings)
            || topLevel == QLatin1String("current_pdfs") || topLevel.startsWith(QLatin1String(".thoughts-library-stage-"))) {
            catalog = true;
        }
        if (!owner.isEmpty() && isSourcePath(path, owner, relative)) {
            sources = true;
            // Sources carry agent proposals; re-read only the documents
            // published from this source.
            if (!Thoughts::isAgentProposalSourcePath(path)) {
                continue;
            }
            if (!m_documentsBySourceKnown) {
                unmappedSource = true;
                continue;
            }
            for (const QString &pdfFilename : m_documentsBySource.value(path)) {
                sourceDocuments.insert(QFileInfo(pdfFilename).completeBaseName());
            }
        }
    }
    if (catalog || sources) {
        requestCatalogCheck(sources);
    }
    if (sources) {
        m_librarySourceSearchDirty = true;
        scheduleLibrarySourceSearchUpdate();
    }
    if (unmappedSource) {
        requestReload(QStringLiteral("source_changed"));
    } else if (!sourceDocuments.isEmpty()) {
        DeveloperLog::record(QStringLiteral("library.source_documents_changed"), {{QStringLiteral("documents"), sourceDocuments.size()}});
        m_scheduledDocumentReloads.unite(sourceDocuments);
        if (m_editorDirty || m_chatGptUrlDirty || m_operationPending || m_syncContentDeferred) {
            m_reloadDeferred = true;
        } else {
            m_reloadTimer->start();
        }
    }
}

void CommentsOverview::refreshWatchPaths()
{
    // Nothing to register: the shared recursive change stream covers the docs
    // root (see handleDocsPathsChanged()), and the catalogue comparison runs
    // on the background pool.
}

void CommentsOverview::refreshFileSnapshots()
{
    // Kept as a compatibility hook for synchronous test reloads. Production
    // refreshes compare complete background catalog snapshots.
}

void CommentsOverview::restoreWindowState()
{
    const auto config = KSharedConfig::openConfig();
    const QString groupName = config->hasGroup(QStringLiteral("CommentsOverview")) ? QStringLiteral("CommentsOverview") : QStringLiteral("AllCommentsDashboard");
    const KConfigGroup group(config, groupName);
    const QByteArray geometry = group.readEntry("Geometry", QByteArray());
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray splitterState = group.readEntry("SplitterState", QByteArray());
    if (!splitterState.isEmpty()) {
        m_splitter->restoreState(splitterState);
    }
    const QByteArray contentSplitterState = group.readEntry("ContentSplitterStateV2", QByteArray());
    if (!contentSplitterState.isEmpty()) {
        m_contentSplitter->restoreState(contentSplitterState);
    } else {
        m_contentSplitter->setSizes({700, 450});
    }
    const QByteArray headerState = group.readEntry("HeaderStateV5", QByteArray());
    if (!headerState.isEmpty()) {
        m_table->header()->restoreState(headerState);
    } else {
        m_table->setColumnWidth(CommentColumn, 420);
        m_table->setColumnWidth(DocumentColumn, 180);
        m_table->setColumnWidth(PageColumn, 70);
        m_table->setColumnWidth(SnapshotColumn, 130);
        m_table->setColumnWidth(SourceColumn, 320);
        m_table->setColumnWidth(StatusColumn, 110);
        m_table->setColumnWidth(AuthorColumn, 180);
        m_table->setColumnWidth(ActivityColumn, 160);
        m_table->header()->moveSection(m_table->header()->visualIndex(ActivityColumn), 0);
    }
    // A layout saved before the Source doc column existed gets it once, right after Comment.
    if (!group.readEntry("SourceDocumentColumnPlaced", false)) {
        QHeaderView *header = m_table->header();
        const int target = header->visualIndex(CommentColumn) + 1;
        if (header->visualIndex(SourceDocumentColumn) != target) header->moveSection(header->visualIndex(SourceDocumentColumn), target > header->visualIndex(SourceDocumentColumn) ? target - 1 : target);
        m_table->setColumnWidth(SourceDocumentColumn, 80);
        KConfigGroup(config, QStringLiteral("CommentsOverview")).writeEntry("SourceDocumentColumnPlaced", true);
    }
    auto *otherTable = m_notesActive ? m_commentsBrowser.table : m_notesBrowser.table;
    otherTable->header()->restoreState(m_table->header()->saveState());
    applyBrowserColumns(m_commentsBrowser);
    applyBrowserColumns(m_notesBrowser);
    configureOverviewHeader(m_table);
}

void CommentsOverview::saveWindowState()
{
    if (!m_splitter || !m_table) {
        return;
    }
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview"));
    group.writeEntry("Geometry", saveGeometry());
    group.writeEntry("SplitterState", m_splitter->saveState());
    group.writeEntry("ContentSplitterStateV2", m_contentSplitter->saveState());
    group.writeEntry("HeaderStateV5", m_table->header()->saveState());
    group.deleteEntry("PriorityFilter"); // The filter was removed: a saved value must not hide rows.
    // Drop the settings of the removed location filter.
    for (const char *key : {"LocationFilterActive", "LocationFilterLabel", "LocationFilterLatitude", "LocationFilterLongitude", "LocationFilterRadiusKm"}) {
        group.deleteEntry(key);
    }
    group.sync();
}

void CommentsOverview::applyLibraryColumnDefaults()
{
    QHeaderView *header = m_libraryTree->header();
    const QScopedValueRollback<bool> updating(m_libraryColumnsUpdating, true);
    int visual = 0;
    for (const auto &entry : LibraryColumnDefaults) {
        header->moveSection(header->visualIndex(entry.column), visual++);
        header->setSectionHidden(entry.column, !entry.visible);
    }
    header->setSectionHidden(LibraryTrackedSourcesColumn, true);
    if (header->isSectionHidden(header->sortIndicatorSection())) m_libraryTree->sortByColumn(0, Qt::AscendingOrder);
}

void CommentsOverview::restoreLibraryColumns()
{
    QHeaderView *header = m_libraryTree->header();
    const QScopedValueRollback<bool> updating(m_libraryColumnsUpdating, true);
    const QByteArray state = KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview")).readEntry(LibraryHeaderStateKey, QByteArray());
    if (state.isEmpty() || !header->restoreState(state)) applyLibraryColumnDefaults();
    // Whatever was saved: Project stays first and shown, Tracked sources stays hidden, the
    // Project column takes the free width and the others fit their contents.
    header->setSectionHidden(0, false);
    if (header->visualIndex(0) != 0) header->moveSection(header->visualIndex(0), 0);
    header->setSectionHidden(LibraryTrackedSourcesColumn, true);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < LibraryColumnCount; ++column) header->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(false);
}

void CommentsOverview::setLibraryColumnVisible(int column, bool visible)
{
    if (column <= 0 || column >= LibraryColumnCount || column == LibraryTrackedSourcesColumn) return;
    QHeaderView *header = m_libraryTree->header();
    header->setSectionHidden(column, !visible);
    // Rows stay sorted by a column only while it is shown.
    if (!visible && header->sortIndicatorSection() == column) m_libraryTree->sortByColumn(0, Qt::AscendingOrder);
    saveLibraryColumns();
}

void CommentsOverview::saveLibraryColumns()
{
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview"));
    group.writeEntry(LibraryHeaderStateKey, m_libraryTree->header()->saveState());
    group.sync();
}

void CommentsOverview::showLibraryHeaderMenu(const QPoint &position)
{
    QHeaderView *header = m_libraryTree->header();
    QMenu menu(header);
    menu.setObjectName(QStringLiteral("libraryColumnsMenu"));
    menu.setToolTipsVisible(true);
    for (const auto &entry : LibraryColumnDefaults) {
        const int column = entry.column;
        QAction *action = menu.addAction(m_libraryTree->headerItem()->text(column));
        action->setData(column);
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(column));
        action->setToolTip(m_libraryTree->headerItem()->toolTip(column));
        // The Project column holds the tree, so it is always shown.
        if (column == 0) action->setEnabled(false);
        else connect(action, &QAction::toggled, this, [this, column](bool checked) { setLibraryColumnVisible(column, checked); });
    }
    menu.addSeparator();
    QAction *restore = menu.addAction(i18n("Restore default columns"));
    restore->setObjectName(QStringLiteral("libraryColumnsRestoreDefaults"));
    connect(restore, &QAction::triggered, this, [this] {
        applyLibraryColumnDefaults();
        // Nothing saved: the defaults of later versions apply too.
        KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview"));
        group.deleteEntry(LibraryHeaderStateKey);
        group.sync();
    });
    menu.exec(header->viewport()->mapToGlobal(position));
}

void CommentsOverview::updateCommentSummary()
{
    if (m_updatingCommentModel) { return; }
    const auto *all = m_documentList->topLevelItem(0);
    const int comments = all ? all->text(1).toInt() : 0;
    const int proposals = all ? all->text(2).toInt() : 0;
    const int notes = all ? all->text(3).toInt() : 0;
    for (BrowserState *browser : {&m_commentsBrowser, &m_notesBrowser}) {
        const int visible = browser->proxy->rowCount();
        const int total = browser->notes ? notes : comments + proposals;
        if (browser->notes) {
            int hidden = 0;
            for (int row = 0; row < m_model->rowCount(); ++row) {
                const QModelIndex index = m_model->index(row, CommentColumn);
                const QJsonObject payload = index.data(LogicalPayloadRole).value<QJsonObject>();
                if (index.data(ItemTypeRole).toString() == QLatin1String("note") && !diaryEntry(payload) && todoHiddenToday(payload)) ++hidden;
            }
            const bool showingHidden = m_showHiddenTodos && m_showHiddenTodos->isChecked();
            const int listed = showingHidden ? notes : notes - hidden;
            QString text = visible == listed ? i18np("%1 to-do", "%1 to-dos", listed)
                                             : i18n("%1 of %2 to-dos shown", visible, listed);
            if (hidden > 0) text += QStringLiteral(" · ") + (showingHidden ? i18np("%1 hidden until later is shown", "%1 hidden until later are shown", hidden)
                                                                            : i18np("%1 hidden until later", "%1 hidden until later", hidden));
            browser->summary->setText(text);
            continue;
        }
        const QString breakdown = i18n("%1 · %2", i18np("%1 comment", "%1 comments", comments), i18np("%1 agent proposal", "%1 agent proposals", proposals));
        browser->summary->setText(visible == total ? i18n("%1 items — %2", total, breakdown)
            : i18n("%1 of %2 items shown — %3", visible, total, breakdown));
    }
}

void CommentsOverview::updateLibraryTree()
{
    if (m_syncContentDeferred || (m_syncRefreshPending && !m_syncLibraryTree)) { return; }
    if (!m_libraryTree) {
        return;
    }
    // This rebuild covers every change scheduled so far.
    if (m_libraryTreeTimer) m_libraryTreeTimer->stop();
    m_libraryTreeDirty = false;
    const QStringList reasons = std::exchange(m_libraryTreeReasons, {});
    const DeveloperActivity activity(QStringLiteral("library.tree"),
        reasons.isEmpty() ? QJsonObject() : QJsonObject {{QStringLiteral("reasons"), QJsonArray::fromStringList(reasons)}});
    QSet<QString> expanded;
    QSet<QString> known;
    QString selected;
    const int scroll = m_libraryTree->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(m_libraryTree); *it; ++it) {
        const QString key = (*it)->data(0, LibraryIdentityRole).toString();
        known.insert(key);
        if ((*it)->isExpanded()) {
            expanded.insert(key);
        }
        if (*it == m_libraryTree->currentItem()) {
            selected = key;
        }
    }
    const int sortColumn = m_libraryTree->header()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_libraryTree->header()->sortIndicatorOrder();
    QTreeWidget *tree = libraryRefreshTree();
    // Fill detached items and hand them to the view at once. Items in a view
    // report every change to it, and once the view has laid out its rows (a
    // current item does that) each report re-measures rows: seconds per
    // rebuild for ~1300 rows on 2026-10-13.
    QElapsedTimer stage;
    stage.start();
    QJsonObject stages;
    const auto endStage = [&stage, &stages](const char *name) { stages.insert(QLatin1String(name), stage.restart()); };
    QList<QTreeWidgetItem *> topLevel;
    QHash<QString, QIcon> themeIcons;
    const auto add = [&](QTreeWidgetItem *parent, const QString &name, const QString &identity, qint64 bytes, const QString &icon) {
        auto *item = new LibraryTreeItem(QStringList {name, {}, {}, formattedDocumentBytes(bytes)});
        if (parent) parent->addChild(item);
        else topLevel.append(item);
        item->setData(0, LibraryIdentityRole, identity);
        item->setData(3, LibrarySortValueRole, bytes);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        auto themeIcon = themeIcons.constFind(icon);
        if (themeIcon == themeIcons.cend()) themeIcon = themeIcons.insert(icon, QIcon::fromTheme(icon));
        item->setIcon(0, *themeIcon);
        item->setSizeHint(0, QSize(0, name.contains(QLatin1Char('\n')) ? 48 : 28));
        return item;
    };
    const auto setSize = [](QTreeWidgetItem *item, qint64 bytes) {
        item->setText(3, formattedDocumentBytes(bytes));
        item->setData(3, LibrarySortValueRole, bytes);
    };
    const auto groupStyle = [tree](QTreeWidgetItem *item, bool) {
        QFont font = tree->font();
        font.setWeight(QFont::Medium);
        for (int column : {0, 3, LibraryFilesColumn}) { item->setFont(column, font); }
        item->setSizeHint(0, QSize(0, 32));
    };
    const auto setCount = [](QTreeWidgetItem *item, int column, qint64 count) {
        item->setText(column, count < 0 ? QString::fromUtf8("—") : QString::number(count));
        item->setData(column, LibrarySortValueRole, count);
        item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    };
    QHash<QString, const TrackedCommentDocument *> publications;
    for (const auto &document : std::as_const(m_documents)) {
        publications.insert(document.libraryFilename.isEmpty() ? QFileInfo(document.pdfPath).fileName() : document.libraryFilename, &document);
    }
    QHash<QString, qint64> publishedSegmentSizes;
    for (const auto &document : std::as_const(m_documents)) {
        for (const auto &value : document.librarySegments) {
            const auto segment = value.toObject();
            publishedSegmentSizes.insert(librarySegmentKey(document, segment),
                document.librarySegments.size() == 1 ? document.pdfSizeBytes : -1);
        }
    }
    // PDFs whose latest build failed or needs attention, as the Sync status
    // reads them: the Library index keeps the last-good PDF, and Fedora's
    // render status says the latest build needs attention. The failing
    // document, its failing source file and its project are marked.
    const QHash<QString, QString> buildWarnings = libraryBuildWarnings();
    QHash<QString, QString> sourceBuildWarnings;
    for (const auto &document : std::as_const(m_documents)) {
        if (!document.libraryLatestBuildSource.isEmpty() && buildWarnings.contains(document.libraryFilename))
            sourceBuildWarnings.insert(document.libraryLatestBuildSource, buildWarnings.value(document.libraryFilename));
    }
    const QIcon warningIcon = QIcon::fromTheme(QStringLiteral("dialog-warning"));
    const auto buildFailureText = [](const QString &message) {
        return message.isEmpty() ? SyncStatusLabels::pdfBuildFailed()
                                 : i18nc("@info:tooltip PDF build failed: error", "%1: %2", SyncStatusLabels::pdfBuildFailed(), message);
    };
    const auto markRowFailed = [&warningIcon, &buildFailureText](QTreeWidgetItem *item, const QString &message, const QString &detail) {
        item->setIcon(0, warningIcon);
        item->setToolTip(0, detail.isEmpty() ? buildFailureText(message) : buildFailureText(message) + QLatin1Char('\n') + detail);
        item->setData(0, LibraryBuildFailureRole, message.isEmpty() ? SyncStatusLabels::pdfBuildFailed() : message);
    };
    QHash<QString, QTreeWidgetItem *> roots;
    {
        LibraryProjectStore store(m_docsRoot);
        const auto projects = store.projects(true, &m_librarySourceInventory);
        endStage("project_store_ms");
        const auto populateSources = [&](QTreeWidgetItem *root, const QJsonObject &project, const QString &id) {
            for (int i = root->childCount() - 1; i >= 0; --i)
                if (root->child(i)->text(0) == i18n("Sources")) delete root->takeChild(i);
            auto *sources = add(root, i18n("Sources"), id + QStringLiteral("/sources"), 0, QStringLiteral("folder")); sources->setData(0, LibraryOrderRole, 1); groupStyle(sources, true);
            qint64 sourceBytes = 0;
            for (const auto &entry : project.value(QStringLiteral("sources")).toArray()) {
                const auto location = entry.toObject();
                const QString locationId = location.value(QStringLiteral("id")).toString();
                auto *group = add(sources, location.value(QStringLiteral("name")).toString(), id + QStringLiteral("/sources/") + locationId, 0, QStringLiteral("folder"));
                group->setData(0, Qt::UserRole + 142, QVariant::fromValue(location));
                if (!location.value(QStringLiteral("pending_import")).toBool() && !locationId.isEmpty()) {
                    group->setData(0, LibraryFolderPathRole, QDir(m_docsRoot).filePath(locationId));
                }
                if (location.value(QStringLiteral("pending_import")).toBool()) {
                    group->setIcon(0, librarySymbol(LibrarySymbol::PendingImport, palette()));
                    group->setToolTip(0, i18n("Pending import. Sync to finish importing these sources and enable source selection."));
                    group->setData(0, Qt::AccessibleTextRole, i18n("%1 — Pending import", group->text(0)));
                } else if (const QString buildError = currentFolderBuildError(project, locationId, location.value(QStringLiteral("build_error")).toString()); !buildError.isEmpty()) {
                    markRowFailed(group, buildError, QString());
                }
                const QString locationPath = QDir(m_docsRoot).filePath(locationId);
                QMap<QString, QTreeWidgetItem *> directories;
                qint64 total = 0;
                for (const auto &value : location.value(QStringLiteral("files")).toArray()) {
                    const auto file = value.toObject();
                    QTreeWidgetItem *parent = group;
                    QString relative = file.value(QStringLiteral("relative_path")).toString();
                    if (relative.isEmpty()) {
                        const QString prefix = location.value(QStringLiteral("id")).toString() + QLatin1Char('/');
                        const auto source = file.value(QStringLiteral("id")).toString();
                        relative = source.startsWith(prefix) ? source.mid(prefix.size()) : file.value(QStringLiteral("name")).toString();
                    }
                    const auto parts = relative.split(QLatin1Char('/')); QString key;
                    for (int i = 0; i < parts.size() - 1; ++i) {
                        key += (key.isEmpty() ? QString() : QStringLiteral("/")) + parts[i];
                        if (!directories.contains(key)) directories.insert(key, add(parent, parts[i], id + QStringLiteral("/source-folder/") + location.value(QStringLiteral("id")).toString() + QLatin1Char('/') + key, 0, QStringLiteral("folder")));
                        parent = directories.value(key);
                        parent->setData(0, LibraryFolderPathRole, QDir(locationPath).filePath(key));
                    }
                    const auto size = file.value(QStringLiteral("size_bytes")).toInteger(); total += size;
                    for (auto *ancestor = parent; ancestor && ancestor != group; ancestor = ancestor->parent()) setSize(ancestor, ancestor->data(3, LibrarySortValueRole).toLongLong() + size);
                    auto *child = add(parent, file.value(QStringLiteral("name")).toString(), id + QStringLiteral("/source/") + file.value(QStringLiteral("id")).toString(), size, MarkdownFolderImport::image(relative) ? QStringLiteral("image-x-generic") : QStringLiteral("text-markdown"));
                    child->setToolTip(0, file.value(QStringLiteral("id")).toString());
                    if (const auto failure = sourceBuildWarnings.constFind(file.value(QStringLiteral("id")).toString()); failure != sourceBuildWarnings.cend())
                        markRowFailed(child, *failure, file.value(QStringLiteral("id")).toString());
                    child->setData(0, Qt::UserRole + 1, file.value(QStringLiteral("id")).toString());
                    child->setData(0, LibraryModifiedPathRole, QDir(m_docsRoot).filePath(file.value(QStringLiteral("id")).toString()));
                    child->setData(0, LibraryModifiedPdfRole, false);
                }
                setSize(group, group->data(3, LibrarySortValueRole).toLongLong() + total); sourceBytes += total;
            }
            setSize(sources, sourceBytes);
        };
        // Agent conversations: the Claude Code and Codex conversations linked to the Project.
        // The group is appended last, so child(0) and child(1) stay Documents and Sources.
        QColor conversationSecondary = palette().color(QPalette::Text);
        {
            const QColor base = palette().color(QPalette::Base);
            conversationSecondary = QColor((conversationSecondary.red() * 3 + base.red()) / 4, (conversationSecondary.green() * 3 + base.green()) / 4,
                                           (conversationSecondary.blue() * 3 + base.blue()) / 4);
        }
        const auto populateConversations = [&](QTreeWidgetItem *root, const QString &id) {
            const QList<QJsonObject> records = Thoughts::AgentConversations::recordsFor(m_fedoraProjections, id);
            const auto clearSize = [](QTreeWidgetItem *item) {
                item->setText(3, QString());
                item->setData(3, LibrarySortValueRole, QVariant());
            };
            auto *group = add(root, i18n("Agent conversations (%1)", records.size()), id + QStringLiteral("/agent-conversations"), 0, QStringLiteral("im-user"));
            group->setData(0, LibraryOrderRole, 2);
            groupStyle(group, true);
            clearSize(group);
            if (records.isEmpty()) {
                auto *empty = add(group, i18n("No agent conversations yet"), id + QStringLiteral("/agent-conversations/none"), 0, QString());
                empty->setFlags(Qt::ItemIsEnabled);
                empty->setForeground(0, conversationSecondary);
                empty->setData(0, LibraryOrderRole, 0);
                clearSize(empty);
                return;
            }
            const QLocale locale;
            int order = 0;
            for (const QJsonObject &record : records) {
                const bool codex = record.value(QStringLiteral("agent")).toString() == QLatin1String("codex");
                const QString title = record.value(QStringLiteral("title")).toString();
                const QString url = record.value(QStringLiteral("conversation_url")).toString();
                auto *row = add(group, title, id + QStringLiteral("/agent-conversation/") + record.value(QStringLiteral("link_id")).toString(), 0,
                                codex ? QStringLiteral("utilities-terminal") : QStringLiteral("im-user"));
                clearSize(row);
                row->setData(0, LibraryConversationRole, QVariant::fromValue(record));
                row->setData(0, LibraryOrderRole, order++);
                row->setText(2, codex ? i18n("Codex") : i18n("Claude"));
                row->setForeground(2, conversationSecondary);
                if (url.isEmpty()) row->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
                const QDateTime active = QDateTime::fromString(record.value(QStringLiteral("last_active_at")).toString(), Qt::ISODateWithMs);
                QStringList facts{codex ? i18n("Codex") : i18n("Claude")};
                if (const QString session = record.value(QStringLiteral("session_name")).toString(); !session.isEmpty()) facts.append(session);
                QString activity = i18n("last active %1", locale.toString(active.toLocalTime(), QLocale::ShortFormat));
                if (const QString device = record.value(QStringLiteral("device_name")).toString(); !device.isEmpty()) activity = i18n("%1 on %2", activity, device);
                facts.append(activity);
                const QString linkedBy = record.value(QStringLiteral("linked_by")).toString();
                const QString tool = record.value(QStringLiteral("last_tool")).toString();
                const QString linker = linkedBy == QLatin1String("agent") ? i18n("the agent")
                    : linkedBy == QLatin1String("backfill")              ? i18n("a pending proposal")
                    : tool.isEmpty()                                     ? i18n("a Thoughts tool")
                                                                         : tool;
                const QString link = !url.isEmpty() ? url
                    : codex ? i18n("No link: this Codex thread is not on this computer.")
                            : i18n("No link: this Claude session was not started with Remote Control.");
                row->setToolTip(0, title + QLatin1Char('\n') + facts.join(QStringLiteral(" · ")) + QLatin1Char('\n') + i18n("Linked by %1", linker) + QLatin1Char('\n') + link);
                row->setData(0, LibraryTypeRole, codex ? i18n("Codex conversation") : i18n("Claude conversation"));
                row->setData(0, LibraryDetailRole, url.isEmpty() ? record.value(QStringLiteral("conversation_id")).toString() : url);
            }
        };
        QSet<QString> visible;
        for (const auto &value : projects) {
            const auto project = value.toObject(); const QString id = project.value(QStringLiteral("id")).toString(); visible.insert(id);
            auto *root = roots.value(id);
            if (!root) { root = add(nullptr, project.value(QStringLiteral("name")).toString(), id, 0, QStringLiteral("folder")); roots.insert(id, root); }
            root->setData(0, Qt::UserRole + 140, QVariant::fromValue(project));
            const QString target = project.value(QStringLiteral("target")).toString();
            const auto projectDocuments = project.value(QStringLiteral("documents")).toArray();
            const auto *publication = publications.value(target, nullptr);
            QDateTime projectBuiltAt = publication ? publication->libraryBuiltAt : QDateTime();
            qint64 projectPages = publication ? publication->libraryPageCount : -1;
            if (!publication) {
                // A source project's target is its reading, not a PDF: summarize its documents' PDFs.
                // Count the built PDFs; documents not built yet must not hide the total.
                qint64 pages = 0;
                int counted = 0;
                for (const auto &entry : projectDocuments) {
                    const auto *document = publications.value(entry.toObject().value(QStringLiteral("filename")).toString(), nullptr);
                    if (document && document->libraryBuiltAt.isValid() && (!projectBuiltAt.isValid() || document->libraryBuiltAt > projectBuiltAt))
                        projectBuiltAt = document->libraryBuiltAt;
                    if (document && document->libraryPageCount >= 0) {
                        pages += document->libraryPageCount;
                        ++counted;
                    }
                }
                if (counted > 0) projectPages = pages;
                const int uncounted = int(projectDocuments.size()) - counted;
                root->setToolTip(LibraryPagesColumn, counted > 0 && uncounted > 0
                    ? i18np("Pages of the built PDFs; 1 document is not built yet.", "Pages of the built PDFs; %1 documents are not built yet.", uncounted)
                    : QString());
            }
            root->setData(0, LibraryRecordedDateRole, projectBuiltAt);
            setCount(root, LibraryPagesColumn, projectPages);
            setCount(root, LibraryDocumentsColumn, projectDocuments.size());
            QString projectPath;
            if (!target.isEmpty()) {
                projectPath = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/") + target);
            } else if (projectDocuments.size() == 1) {
                const auto doc = projectDocuments.first().toObject();
                projectPath = doc.value(QStringLiteral("local_path")).toString();
                if (projectPath.isEmpty() && !doc.value(QStringLiteral("filename")).toString().isEmpty())
                    projectPath = QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/") + doc.value(QStringLiteral("filename")).toString());
            }
            root->setData(0, Qt::UserRole, projectPath);
            qDeleteAll(root->takeChildren());
            qint64 bytes = 0;
            auto *documents = add(root, i18n("Documents"), id + QStringLiteral("/documents"), 0, QStringLiteral("application-pdf")); groupStyle(documents, false);
            documents->setData(0, LibraryOrderRole, 0);
            documents->setData(0, Qt::UserRole, projectPath);
            int order = 0;
            // Unpublished documents stay in their Project, greyed and after the
            // published ones (protocol/project-document-publication.md).
            auto projectEntries = project.value(QStringLiteral("documents")).toArray();
            for (const auto &entry : project.value(QStringLiteral("unpublished_documents")).toArray()) projectEntries.append(entry);
            for (const auto &entry : std::as_const(projectEntries)) {
                const auto doc = entry.toObject();
                const bool unpublished = !doc.value(QStringLiteral("published")).toBool(true);
                qint64 size = doc.value(QStringLiteral("size_bytes")).toInteger(-1);
                const QString sizeKey = librarySegmentKey(doc.value(QStringLiteral("sha256")).toString(), doc);
                const bool usePublishedSize = size < 0 && !doc.value(QStringLiteral("pending_build")).toBool();
                if (usePublishedSize) { size = publishedSegmentSizes.value(sizeKey, -1); }
                if (!unpublished) bytes += qMax<qint64>(size, 0);
                QTreeWidgetItem *container = documents;
                auto *child = add(container, doc.value(QStringLiteral("name")).toString(), id + QLatin1Char('/') + doc.value(QStringLiteral("id")).toString(), size, QStringLiteral("application-pdf"));
                const auto *documentPublication = publications.value(doc.value(QStringLiteral("filename")).toString(), nullptr);
                // A document of a project reading is built as its own PDF; show when that happened.
                if (doc.value(QStringLiteral("start_page")).toInt() <= 0 || (documentPublication && target.endsWith(QLatin1String(".thoughts-project"))))
                    child->setData(0, LibraryRecordedDateRole, documentPublication ? documentPublication->libraryBuiltAt : QDateTime());
                const qint64 pages = doc.value(QStringLiteral("page_count")).toInteger(-1);
                setCount(child, LibraryPagesColumn, pages >= 0 ? pages : doc.value(QStringLiteral("start_page")).toInt() <= 0 && documentPublication ? documentPublication->libraryPageCount : -1);
                child->setData(0, LibraryCompositionIdRole, doc.value(QStringLiteral("id")));
                child->setData(0, LibraryDocumentSourceRole, doc.value(QStringLiteral("source_file")));
                QStringList missing;
                for (const auto &path : doc.value(QStringLiteral("missing_sources")).toArray()) missing.append(path.toString());
                const auto source = doc.value(QStringLiteral("source_file")).toString();
                if (!source.isEmpty() && !QFileInfo::exists(QDir(m_docsRoot).filePath(source)) && !missing.contains(source)) missing.append(source);
                child->setData(0, LibraryMissingSourcesRole, missing);
                child->setData(0, LibraryOrderRole, order++); child->setData(0, Qt::UserRole + 141, QVariant::fromValue(doc));
                const QString path = doc.value(QStringLiteral("local_path")).toString();
                const auto filename = doc.value(QStringLiteral("filename")).toString();
                child->setData(0, Qt::UserRole, !path.isEmpty() ? path : filename.isEmpty() ? QString() : QDir(m_docsRoot).filePath(QStringLiteral("current_pdfs/") + filename));
                // Its PDF has left the Library, so it is not opened.
                child->setData(0, LibraryPageRole, unpublished ? 0 : doc.value(QStringLiteral("start_page")).toInt());
                child->setData(0, LibraryUnpublishedRole, unpublished);
            }
            setSize(documents, project.value(QStringLiteral("pdf_size_bytes")).toInteger(bytes)); setSize(root, project.value(QStringLiteral("pdf_size_bytes")).toInteger(bytes));
            populateSources(root, project, id);
            populateConversations(root, id);
        }
        for (auto it = roots.cbegin(); it != roots.cend(); ++it) if (!visible.contains(it.key())) { topLevel.removeOne(it.value()); delete it.value(); }
        const QString summary = !store.error().isEmpty() ? store.error()
            : !m_documentsLoaded ? i18n("Loading the Library…") : i18np("1 project", "%1 projects", projects.size());
        if (m_syncLibraryTree) m_syncLibrarySummary = summary;
        else m_librarySummary->setText(summary);
    }
    const QIcon folderIcon = librarySymbol(LibrarySymbol::Folder, palette());
    const QIcon documentsIcon = librarySymbol(LibrarySymbol::Documents, palette());
    const QIcon linkedIcon = librarySymbol(LibrarySymbol::Linked, palette());
    const QIcon unlinkedIcon = librarySymbol(LibrarySymbol::Unlinked, palette());
    const QIcon unknownIcon = librarySymbol(LibrarySymbol::LinkUnknown, palette());
    const QIcon missingSourceIcon = librarySymbol(LibrarySymbol::MissingSource, palette());
    QColor secondary = palette().color(QPalette::Text);
    const QColor base = palette().color(QPalette::Base);
    secondary = QColor((secondary.red() * 3 + base.red()) / 4, (secondary.green() * 3 + base.green()) / 4, (secondary.blue() * 3 + base.blue()) / 4);
    // A Source pages or PDF pages cell: the count, "—" when unknown, empty when not of this kind.
    const auto setPageKindCell = [](QTreeWidgetItem *item, int column, qint64 pages, bool ofThisKind) {
        item->setText(column, !ofThisKind ? QString() : pages < 0 ? QString::fromUtf8("—") : QString::number(pages));
        item->setData(column, LibrarySortValueRole, ofThisKind ? pages : qint64(-1));
        item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    };
    // The first publication of each file name and PDF path, as a scan of m_documents finds it.
    QHash<QString, const TrackedCommentDocument *> trackedByFilename, trackedByPath;
    for (const auto &tracked : std::as_const(m_documents)) {
        if (!trackedByFilename.contains(tracked.libraryFilename)) trackedByFilename.insert(tracked.libraryFilename, &tracked);
        if (!trackedByPath.contains(tracked.pdfPath)) trackedByPath.insert(tracked.pdfPath, &tracked);
    }
    for (QTreeWidgetItem *project : std::as_const(topLevel)) {
        const auto metadata = project->data(0, Qt::UserRole + 140).value<QJsonObject>();
        project->setIcon(0, QIcon());
        project->setToolTip(0, QString());
        // A failed project carries a warning sign; other projects are unaffected.
        const auto markBuildFailed = [project, &markRowFailed](const QString &error) { markRowFailed(project, error, QString()); };
        for (const auto &folder : metadata.value(QStringLiteral("source_folders")).toArray()) {
            const QString error = currentFolderBuildError(metadata, folder.toObject().value(QStringLiteral("project")).toString(),
                                                          folder.toObject().value(QStringLiteral("build_error")).toString());
            if (!error.isEmpty()) markBuildFailed(error);
        }
        project->setData(0, LibraryTypeRole, i18n("Project"));
        project->setForeground(1, secondary);
        QFont projectFont = tree->font();
        projectFont.setWeight(QFont::Medium);
        project->setFont(0, projectFont);
        auto *documents = project->child(0);
        if (!documents) { continue; }
        documents->setIcon(0, documentsIcon);
        if (documents->childCount() == 0) documents->setText(0, i18n("No documents yet"));
        if (auto *sources = project->child(1)) { sources->setIcon(0, folderIcon); }
        QList<QTreeWidgetItem *> documentRows;
        const auto collect = [&documentRows](const auto &self, QTreeWidgetItem *parent) -> void {
            for (int c = 0; c < parent->childCount(); ++c) {
                documentRows.append(parent->child(c));
                self(self, parent->child(c));
            }
        };
        collect(collect, documents);
        // The project's pages by the kind its document rows show; unpublished documents count in neither.
        qint64 sourcePages = 0;
        qint64 pdfPages = 0;
        qint64 unknownPages = 0;
        bool pagesKnown = false;
        for (QTreeWidgetItem *item : std::as_const(documentRows)) {
            const auto doc = item->data(0, Qt::UserRole + 141).value<QJsonObject>();
            if (doc.isEmpty() && item->childCount()) continue;
            const bool pending = doc.value(QStringLiteral("pending_build")).toBool() || (doc.isEmpty() && item->text(3) == i18n("Not built yet"));
            QString source = doc.value(QStringLiteral("source_file")).toString();
            if (source.isEmpty()) source = item->data(0, LibraryDocumentSourceRole).toString();
            QString kind;
            const QString filename = doc.value(QStringLiteral("filename")).toString();
            const QString *buildWarning = nullptr;
            if (const TrackedCommentDocument *tracked = !filename.isEmpty() ? trackedByFilename.value(filename)
                                                                              : trackedByPath.value(item->data(0, Qt::UserRole).toString())) {
                kind = tracked->libraryKind;
                if (const auto found = buildWarnings.constFind(tracked->libraryFilename); found != buildWarnings.cend()) buildWarning = &found.value();
            }
            const bool unlinked = doc.value(QStringLiteral("imported")).toBool() || source.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)
                || item->data(0, LibraryCompositionIdRole).toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
            // An imported PDF is not built, so no build can fail for it.
            if (unlinked || pending) buildWarning = nullptr;
            if (buildWarning) markBuildFailed(*buildWarning);
            const bool linked = !unlinked && (!source.isEmpty() || kind == QLatin1String("source_linked"));
            const bool unknown = !unlinked && !linked && kind != QLatin1String("unlinked");
            const QStringList missing = item->data(0, LibraryMissingSourcesRole).toStringList();
            const QString sourceStatus = linked && !missing.isEmpty() ? i18n("Missing source: %1", missing.join(QStringLiteral(", ")))
                : linked ? i18n("PDF with sources") : unknown ? i18n("PDF source status unavailable") : i18n("PDF without sources");
            item->setIcon(0, linked && !missing.isEmpty() ? missingSourceIcon : linked ? linkedIcon : unknown ? unknownIcon : unlinkedIcon);
            item->setData(0, LibraryTypeRole, pending ? i18n("Not built yet") : i18n("PDF"));
            item->setForeground(1, secondary);
            QString detail = doc.value(QStringLiteral("attachment_filename")).toString();
            if (detail.isEmpty()) detail = doc.value(QStringLiteral("filename")).toString();
            if (detail.isEmpty()) detail = QFileInfo(item->data(0, Qt::UserRole).toString()).fileName();
            if (doc.value(QStringLiteral("pending_move")).toBool()) detail = i18n("Move pending Sync — %1", detail);
            if (pending) {
                detail.clear();
                item->setText(3, i18n("Not built yet"));
                item->setData(0, Qt::UserRole, QString());
                item->setToolTip(0, i18n("Selected for building. Sync to create the PDF."));
            }
            item->setToolTip(0, sourceStatus + QLatin1Char('\n') + (pending ? i18n("Selected for building. Sync to create the PDF.") : detail));
            item->setData(0, LibraryDetailRole, detail);
            item->setData(0, Qt::AccessibleDescriptionRole, QString(sourceStatus + QStringLiteral(" · ") + detail));
            if (buildWarning) {
                // The warning replaces the link symbol; the tooltip still says how the PDF is linked.
                markRowFailed(item, *buildWarning, sourceStatus + QLatin1Char('\n') + detail);
                item->setData(0, Qt::AccessibleDescriptionRole, QString(buildFailureText(*buildWarning) + QStringLiteral(" · ") + sourceStatus + QStringLiteral(" · ") + detail));
            }
            const bool unpublished = item->data(0, LibraryUnpublishedRole).toBool();
            if (unpublished) {
                for (int column = 0; column < item->columnCount(); ++column) item->setForeground(column, secondary);
                item->setText(3, i18n("Unpublished"));
                item->setData(0, LibraryTypeRole, i18n("Unpublished"));
                item->setToolTip(0, i18n("Unpublished: not built and not in the Library; its review cards are kept but not offered for review. Publish brings it back.")
                    + QLatin1Char('\n') + sourceStatus);
                item->setData(0, Qt::AccessibleDescriptionRole, QString(i18n("Unpublished") + QStringLiteral(" · ") + sourceStatus));
            }
            item->setData(0, LibraryModifiedPathRole, pending || unpublished ? QString() : item->data(0, Qt::UserRole).toString());
            item->setData(0, LibraryModifiedPdfRole, true);
            item->setSizeHint(0, QSize(0, 32));
            // A document's pages go to the column of its kind; "—" where the kind or the count is not known.
            const QVariant pageValue = item->data(LibraryPagesColumn, LibrarySortValueRole);
            const qint64 pages = pageValue.isValid() ? pageValue.toLongLong() : -1;
            if (doc.isEmpty() && !pageValue.isValid()) continue;
            setPageKindCell(item, LibrarySourcePagesColumn, unknown ? -1 : pages, linked || unknown);
            setPageKindCell(item, LibraryPdfPagesColumn, unknown ? -1 : pages, !linked);
            if (unpublished || pages < 0) continue;
            pagesKnown = true;
            (linked ? sourcePages : unknown ? unknownPages : pdfPages) += pages;
        }
        // Pages whose kind is not known yet (before the Library index is read) count in neither
        // column; with no known kind at all both read "—".
        const bool kindsKnown = sourcePages + pdfPages > 0 || unknownPages == 0;
        const QString unknownPagesTip = unknownPages > 0
            ? i18np("1 page is in a PDF whose sources are not known yet.", "%1 pages are in PDFs whose sources are not known yet.", unknownPages)
            : QString();
        for (const int column : {LibrarySourcePagesColumn, LibraryPdfPagesColumn}) {
            setPageKindCell(project, column, !pagesKnown || !kindsKnown ? -1 : column == LibrarySourcePagesColumn ? sourcePages : pdfPages, true);
            project->setToolTip(column, unknownPagesTip);
        }
    }
    endStage("rows_ms");
    updateLibrarySizes(&topLevel);
    applyLibraryModificationDates(&topLevel);
    applyLibraryReviewCounts(&topLevel);
    endStage("columns_ms");
    const QSignalBlocker blocker(tree);
    tree->setUpdatesEnabled(false);
    tree->setSortingEnabled(false);
    tree->clear();
    tree->addTopLevelItems(topLevel);
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const QString identity = (*it)->data(0, LibraryIdentityRole).toString();
        // Documents and Sources start expanded; Agent conversations start collapsed.
        const bool defaultExpanded = (*it)->data(0, LibraryOrderRole).isValid() && (*it)->parent() && !(*it)->parent()->parent()
            && !identity.endsWith(QLatin1String("/agent-conversations"));
        if (expanded.contains(identity) || (!known.contains(identity) && defaultExpanded)) (*it)->setExpanded(true);
        if (identity == selected) { tree->setCurrentItem(*it); }
    }
    if (!m_syncLibraryTree) {
        m_librarySelectionDetails->setText(librarySelectionDetail(tree->currentItem()));
        m_librarySelectionDetails->setVisible(!m_librarySelectionDetails->text().isEmpty());
    }
    tree->setSortingEnabled(true);
    tree->sortItems(sortColumn, sortOrder);
    tree->setUpdatesEnabled(true);
    tree->verticalScrollBar()->setValue(scroll);
    endStage("view_ms");
    int rows = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) ++rows;
    stages.insert(QStringLiteral("rows"), rows);
    DeveloperLog::record(QStringLiteral("library.tree.stages"), stages);
    QTimer::singleShot(0, this, &CommentsOverview::requestLibraryModificationDates);
}

QList<QTreeWidgetItem *> CommentsOverview::libraryProjectItems(const QList<QTreeWidgetItem *> *detached) const
{
    if (detached) return *detached;
    QList<QTreeWidgetItem *> projects;
    if (QTreeWidget *tree = libraryRefreshTree()) {
        projects.reserve(tree->topLevelItemCount());
        for (int i = 0; i < tree->topLevelItemCount(); ++i) projects.append(tree->topLevelItem(i));
    }
    return projects;
}

void CommentsOverview::updateLibrarySizes(const QList<QTreeWidgetItem *> *detached)
{
    QTreeWidget *tree = libraryRefreshTree();
    if (!tree) return;
    const DeveloperActivity activity(QStringLiteral("library.size_rows"));
    // Detached rows are sorted when they reach the view.
    const bool sorting = tree->isSortingEnabled();
    if (!detached) tree->setSortingEnabled(false);
    const auto sum = [](qint64 a, qint64 b) -> qint64 {
        return a < 0 || b < 0 || a > std::numeric_limits<qint64>::max() - b ? -1 : a + b;
    };
    const auto set = [](QTreeWidgetItem *item, qint64 bytes, const QString &state = QString(), const QString &error = QString()) {
        item->setData(3, LibrarySortValueRole, bytes);
        QString text = formattedDocumentBytes(bytes);
        if (state == QLatin1String("checking")) text = i18n("Calculating…");
        else if (state == QLatin1String("cached") && bytes >= 0) text = i18n("%1 (cached)", text);
        item->setText(3, text);
        item->setToolTip(3, error.isEmpty() && state == QLatin1String("cached") ? i18n("Last recorded size; checking local files in the background.") : error);
    };
    const auto setFiles = [](QTreeWidgetItem *item, qint64 count) {
        item->setText(LibraryFilesColumn, count < 0 ? QString::fromUtf8("—") : QLocale().toString(count));
        item->setData(LibraryFilesColumn, LibrarySortValueRole, count);
        item->setTextAlignment(LibraryFilesColumn, Qt::AlignRight | Qt::AlignVCenter);
    };
    for (QTreeWidgetItem *root : libraryProjectItems(detached)) {
        const auto project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
        if (project.isEmpty() || root->childCount() < 2) continue;
        auto *documents = root->child(0);
        auto *sources = root->child(1);
        // Ordering can differ while sorting is enabled; identify groups by identity.
        for (int i = 0; i < root->childCount(); ++i) {
            const auto key = root->child(i)->data(0, LibraryIdentityRole).toString();
            if (key.endsWith(QLatin1String("/documents"))) documents = root->child(i);
            if (key.endsWith(QLatin1String("/sources"))) sources = root->child(i);
        }
        qint64 sourceBytes = 0;
        QString state, error;
        // Files are counted once even when connected source folders overlap.
        QSet<QString> sourceFiles;
        bool sourceFilesKnown = true;
        for (int i = 0; i < sources->childCount(); ++i) {
            auto *folder = sources->child(i);
            auto location = folder->data(0, Qt::UserRole + 142).value<QJsonObject>();
            const auto owner = location.value(QStringLiteral("id")).toString();
            if (m_librarySourceInventory.contains(owner)) location = m_librarySourceInventory.value(owner);
            QHash<QString, qint64> sizes;
            for (const auto &file : location.value(QStringLiteral("files")).toArray()) {
                const auto entry = file.toObject();
                sizes.insert(entry.value(QStringLiteral("id")).toString(), entry.value(QStringLiteral("size_bytes")).toInteger(-1));
            }
            const auto update = [&](const auto &self, QTreeWidgetItem *item) -> qint64 {
                const auto path = item->data(0, Qt::UserRole + 1).toString();
                qint64 bytes = 0;
                if (!path.isEmpty()) bytes = sizes.value(path, -1);
                else for (int j = 0; j < item->childCount(); ++j) bytes = sum(bytes, self(self, item->child(j)));
                set(item, bytes);
                return bytes;
            };
            update(update, folder);
            const auto countFiles = [&](const auto &self, QTreeWidgetItem *item) -> qint64 {
                if (!item->data(0, Qt::UserRole + 1).toString().isEmpty()) return sizes.contains(item->data(0, Qt::UserRole + 1).toString()) ? 1 : 0;
                qint64 count = 0;
                for (int j = 0; j < item->childCount(); ++j) count += self(self, item->child(j));
                setFiles(item, count);
                return count;
            };
            countFiles(countFiles, folder);
            const auto folderState = location.value(QStringLiteral("size_state")).toString();
            const auto folderError = location.value(QStringLiteral("size_error")).toString();
            const auto bytes = location.value(QStringLiteral("size_bytes")).toInteger(-1);
            set(folder, folderState == QLatin1String("unavailable") ? -1 : bytes, folderState, folderError);
            if (folderState == QLatin1String("unavailable")) {
                setFiles(folder, -1);
                sourceFilesKnown = false;
            } else {
                setFiles(folder, sizes.size());
                for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) sourceFiles.insert(it.key());
            }
            sourceBytes = sum(sourceBytes, folderState == QLatin1String("unavailable") ? -1 : bytes);
            if (folderState == QLatin1String("unavailable")) { state = folderState; error = folderError; }
            else if (state != QLatin1String("unavailable") && folderState == QLatin1String("checking")) state = folderState;
            else if (state.isEmpty() && folderState == QLatin1String("cached")) state = folderState;
        }
        set(sources, sourceBytes, state, error);
        const qint64 fileCount = sourceFilesKnown ? sourceFiles.size() : -1;
        setFiles(sources, fileCount);
        const QFileInfo pdf(root->data(0, Qt::UserRole).toString());
        qint64 pdfBytes = pdf.isFile() ? pdf.size() : -1;
        const auto entries = project.value(QStringLiteral("documents")).toArray();
        bool pendingOnly = true;
        for (const auto &entry : entries) pendingOnly &= entry.toObject().value(QStringLiteral("pending_build")).toBool();
        const bool pdfNotBuilt = !entries.isEmpty() && !pdf.isFile()
            && (pendingOnly || root->data(0, Qt::UserRole).toString().isEmpty());
        if (pdfBytes < 0 && (pendingOnly || pdfNotBuilt)) pdfBytes = 0;
        set(documents, pdfBytes);
        if (pdfNotBuilt) documents->setText(3, i18n("Not built yet"));
        const auto total = sum(pdfBytes, sourceBytes);
        set(root, total, state, error);
        setFiles(root, fileCount);
        if (pdfNotBuilt && total >= 0) root->setText(3, i18n("%1 (PDF not built)", root->text(3)));
        root->setToolTip(3, i18n("Final PDF: %1\nIncluded source files: %2\nProject total: %3",
            formattedDocumentBytes(pdfBytes), formattedDocumentBytes(sourceBytes), formattedDocumentBytes(total))
            + (pdfNotBuilt ? i18n("\nNo final concatenated PDF exists yet; this total includes source files only.") : QString())
            + (state == QLatin1String("cached") ? i18n("\nChecking cached source sizes…") : QString())
            + (error.isEmpty() ? QString() : QLatin1Char('\n') + error));
    }
    if (!detached) tree->setSortingEnabled(sorting);
}

QTreeWidget *CommentsOverview::libraryRefreshTree() const
{
    return m_syncLibraryTree ? m_syncLibraryTree : m_libraryTree;
}

void CommentsOverview::publishSyncLibraryRefresh()
{
    if (!m_syncLibraryTree) return;
    if (m_syncContentDeferred || m_syncRefreshPending || m_fullReloadRunning
        || m_libraryModificationRequestRunning
        || m_libraryMonitoringScheduled || m_libraryMonitoringRunning
        || QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
        QTimer::singleShot(50, this, &CommentsOverview::publishSyncLibraryRefresh);
        return;
    }
    // Restore the user's latest navigation, including changes made while reads ran.
    QSet<QString> expanded;
    QString selected;
    for (QTreeWidgetItemIterator it(m_libraryTree); *it; ++it) {
        const QString key = (*it)->data(0, LibraryIdentityRole).toString();
        if ((*it)->isExpanded()) expanded.insert(key);
        if (*it == m_libraryTree->currentItem()) selected = key;
    }
    const int scroll = m_libraryTree->verticalScrollBar()->value();
    const int column = m_libraryTree->header()->sortIndicatorSection();
    const auto order = m_libraryTree->header()->sortIndicatorOrder();
    const QSignalBlocker blocker(m_libraryTree);
    m_libraryTree->setUpdatesEnabled(false);
    m_libraryTree->setSortingEnabled(false);
    m_libraryTree->clear();
    while (m_syncLibraryTree->topLevelItemCount()) {
        m_libraryTree->addTopLevelItem(m_syncLibraryTree->takeTopLevelItem(0));
    }
    delete std::exchange(m_syncLibraryTree, nullptr);
    m_librarySummary->setText(std::exchange(m_syncLibrarySummary, {}));
    for (QTreeWidgetItemIterator it(m_libraryTree); *it; ++it) {
        const QString key = (*it)->data(0, LibraryIdentityRole).toString();
        (*it)->setExpanded(expanded.contains(key));
        if (key == selected) m_libraryTree->setCurrentItem(*it);
    }
    m_libraryTree->setSortingEnabled(true);
    m_libraryTree->sortItems(column, order);
    m_libraryTree->verticalScrollBar()->setValue(scroll);
    m_libraryTree->setUpdatesEnabled(true);
    m_librarySelectionDetails->setText(librarySelectionDetail(m_libraryTree->currentItem()));
    m_librarySelectionDetails->setVisible(!m_librarySelectionDetails->text().isEmpty());
}

void CommentsOverview::applyLibraryModificationDates(const QList<QTreeWidgetItem *> *detached)
{
    if (m_syncContentDeferred || (m_syncRefreshPending && !m_syncLibraryTree)) { return; }
    const DeveloperActivity activity(QStringLiteral("library.dates.apply"));
    const QLocale systemLocale;
    // An explicit locale also avoids native formatter construction for month names.
    const QLocale locale(systemLocale.name());
    const QDate today = QDate::currentDate();
    QHash<qint64, QString> formattedDates;
    const auto setDate = [&](QTreeWidgetItem *item, const QDateTime &date, LibraryDateTooltip tooltip = PlainDateTooltip) {
        const qint64 stamp = date.isValid() ? date.toMSecsSinceEpoch() : qint64(-1);
        QString formatted = QString::fromUtf8("—");
        if (date.isValid()) {
            auto found = formattedDates.constFind(stamp);
            if (found != formattedDates.cend()) formatted = *found;
            else {
                const QDateTime local = date.toLocalTime();
                if (local.date() == today) formatted = i18n("Today, %1", local.toString(QStringLiteral("HH:mm")));
                else if (local.date() == today.addDays(-1)) formatted = i18n("Yesterday");
                else formatted = locale.toString(local.date(), QStringLiteral("d MMM yyyy"));
                formattedDates.insert(stamp, formatted);
            }
        }
        item->setText(1, formatted);
        item->setData(1, LibrarySortValueRole, stamp);
        const int kind = date.isValid() ? tooltip : PlainDateTooltip;
        if (dynamic_cast<LibraryTreeItem *>(item)) item->setData(1, LibraryDateTooltipRole, kind);
        else item->setToolTip(1, libraryDateTooltip(stamp, kind));
    };
    const auto update = [&](const auto &self, QTreeWidgetItem *item) -> QDateTime {
        if (const QVariant conversation = item->data(0, LibraryConversationRole); conversation.isValid()) {
            const QDateTime active = QDateTime::fromString(conversation.value<QJsonObject>().value(QStringLiteral("last_active_at")).toString(), Qt::ISODateWithMs);
            setDate(item, active, PlainDateTooltip);
            return active;
        }
        QString path = item->data(0, LibraryModifiedPathRole).toString();
        if (path.isEmpty()) { path = item->data(0, Qt::UserRole + 1).toString(); }
        const QDateTime recorded = item->data(0, LibraryRecordedDateRole).toDateTime();
        if (!item->parent() && item->data(0, Qt::UserRole + 140).isValid()) {
            for (int i = 0; i < item->childCount(); ++i) self(self, item->child(i));
            setDate(item, recorded, BuiltDateTooltip);
            return recorded;
        }
        QDateTime latest = item->data(0, LibraryModifiedPdfRole).toBool()
            ? recorded : recorded.isValid() ? recorded : m_libraryModificationDates.value(path);
        if (item->data(0, LibraryModifiedPdfRole).toBool()) {
            setDate(item, latest, BuiltDateTooltip);
            return latest;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            const auto childDate = self(self, item->child(i));
            // A project's date follows its documents, not downloaded build inputs.
            if (!item->parent() && i != 0 && item->child(0)->childCount()) { continue; }
            if (childDate.isValid() && (!latest.isValid() || childDate > latest)) { latest = childDate; }
        }
        setDate(item, latest, ModifiedDateTooltip);
        return latest;
    };
    const bool sorting = libraryRefreshTree()->isSortingEnabled();
    if (!detached) libraryRefreshTree()->setSortingEnabled(false);
    for (QTreeWidgetItem *project : libraryProjectItems(detached)) { update(update, project); }
    if (!detached) libraryRefreshTree()->setSortingEnabled(sorting);
}

void CommentsOverview::applyLibraryReviewCounts(const QList<QTreeWidgetItem *> *detached)
{
    if (m_syncContentDeferred || (m_syncRefreshPending && !m_syncLibraryTree) || !m_reviewCardsWidget) return;
    auto *tree = libraryRefreshTree();
    if (!tree) return;
    const QList<QTreeWidgetItem *> projects = libraryProjectItems(detached);
    const bool sorting = tree->isSortingEnabled();
    if (!detached) tree->setSortingEnabled(false);
    auto byDocument = m_reviewCardsWidget->libraryCardIds();
    QSet<QString> documentIds;
    QHash<QString, QSet<QString>> sourceDocuments;
    QHash<QString, QSet<QString>> unresolvedByFilename;
    for (QTreeWidgetItem *projectItem : projects) {
        const auto project = projectItem->data(0, Qt::UserRole + 140).value<QJsonObject>();
        const auto configuration = project.value(QStringLiteral("project_configuration")).toObject();
        for (const auto &raw : configuration.value(QStringLiteral("documents")).toArray()) {
            const auto declaration = raw.toObject();
            const QString id = declaration.value(QStringLiteral("document_id")).toString();
            documentIds.insert(id);
            sourceDocuments[Thoughts::ProjectConfiguration::documentAlias(configuration, declaration)].insert(id);
        }
        for (const auto &raw : project.value(QStringLiteral("documents")).toArray()) {
            const auto doc = raw.toObject();
            const QString id = doc.value(QStringLiteral("document_id")).toString();
            if (id.isEmpty()) continue;
            documentIds.insert(id);
            const QString source = doc.value(QStringLiteral("source_file")).toString(doc.value(QStringLiteral("id")).toString());
            if (!source.isEmpty()) sourceDocuments[source].insert(id);
        }
    }
    // Older cards identify the assembled PDF. Resolve their component from the
    // already loaded dependency metadata, without changing the stored card.
    const auto primarySources = sourceDocuments;
    for (const auto &publication : std::as_const(m_documents)) {
        for (const auto &raw : publication.librarySegments) {
            const auto segment = raw.toObject();
            const auto ids = primarySources.value(segment.value(QStringLiteral("source_id")).toString());
            for (const auto &input : segment.value(QStringLiteral("inputs")).toArray())
                sourceDocuments[input.toObject().value(QStringLiteral("source_file")).toString()].unite(ids);
        }
    }
    for (const auto &item : m_reviewCardsWidget->libraryCards()) {
        const auto &card = item.card;
        const auto primary = card.record.value(QStringLiteral("primary")).toObject();
        // Only active cards count: retired ones (ReviewRetire) are left out like deleted ones.
        if (card.schedule.deleted || card.schedule.retired || card.record.value(QStringLiteral("deleted")).toBool()
            || card.status() == QLatin1String("rejected") || card.id().isEmpty()
            || documentIds.contains(primary.value(QStringLiteral("document_id")).toString())) continue;
        auto candidates = sourceDocuments.value(card.linkingSource());
        if (candidates.size() != 1) {
            QSet<QString> placed;
            for (const auto &publication : std::as_const(m_documents)) {
                if (publication.libraryFilename != card.pdfStem() + QStringLiteral(".pdf")
                    || publication.librarySha256.isEmpty() || publication.librarySha256 != card.pdfSha256()) continue;
                for (const auto &raw : publication.librarySegments) {
                    const auto segment = raw.toObject();
                    const int start = segment.value(QStringLiteral("start_page")).toInt();
                    const int pages = segment.value(QStringLiteral("page_count")).toInt();
                    if (card.page() >= start && card.page() - start < pages)
                        placed.unite(primarySources.value(segment.value(QStringLiteral("source_id")).toString()));
                }
            }
            if (placed.size() == 1) candidates = placed;
        }
        if (candidates.size() == 1) byDocument[*candidates.cbegin()].insert(card.id());
        else unresolvedByFilename[card.pdfStem() + QStringLiteral(".pdf")].insert(card.id());
    }
    const QSet<QString> &dueIds = m_reviewCardsWidget->libraryDueCardIds();
    const auto setCount = [&](QTreeWidgetItem *item, const QSet<QString> &ids, bool known = true) {
        const bool available = m_reviewCardsWidget->libraryCountsAvailable() && known;
        const qint64 count = available ? ids.size() : -1;
        qint64 due = available ? 0 : -1;
        if (available) for (const QString &id : ids) due += dueIds.contains(id);
        for (const auto &[column, value] : {std::pair{LibraryReviewCardsColumn, count}, std::pair{LibraryDueCardsColumn, due}}) {
            item->setText(column, value < 0 ? QString::fromUtf8("—") : QString::number(value));
            item->setData(column, LibrarySortValueRole, value);
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        }
    };
    for (QTreeWidgetItem *root : projects) {
        const auto project = root->data(0, Qt::UserRole + 140).value<QJsonObject>();
        if (project.isEmpty()) continue;
        QSet<QString> projectCards = unresolvedByFilename.value(project.value(QStringLiteral("target")).toString());
        // Include every manifest document, even when excluded from the assembled PDF.
        for (const auto &raw : project.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("documents")).toArray())
            projectCards.unite(byDocument.value(raw.toObject().value(QStringLiteral("document_id")).toString()));
        auto *documents = root->child(0);
        if (documents) for (int j = 0; j < documents->childCount(); ++j) {
            auto *item = documents->child(j);
            const auto document = item->data(0, Qt::UserRole + 141).value<QJsonObject>();
            if (document.isEmpty()) continue;
            const auto ids = byDocument.value(document.value(QStringLiteral("document_id")).toString());
            const auto unresolved = unresolvedByFilename.value(document.value(QStringLiteral("filename")).toString());
            setCount(item, ids, unresolved.isEmpty());
            projectCards.unite(ids);
            projectCards.unite(unresolved);
        }
        setCount(root, projectCards);
    }
    if (!detached) tree->setSortingEnabled(sorting);
}

void CommentsOverview::requestLibraryModificationDates()
{
    if (m_syncContentDeferred) { return; }
    if (!m_catalogPool) { return; }
    if (m_libraryModificationRequestRunning) { m_libraryModificationRequestPending = true; return; }
    QHash<QString, bool> paths;
    for (QTreeWidgetItemIterator it(libraryRefreshTree()); *it; ++it) {
        QString path = (*it)->data(0, LibraryModifiedPathRole).toString();
        bool pdf = (*it)->data(0, LibraryModifiedPdfRole).toBool();
        if (path.isEmpty()) {
            path = (*it)->data(0, Qt::UserRole + 1).toString();
            pdf = QFileInfo(path).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
        }
        if (!path.isEmpty() && !pdf) { paths.insert(path, false); }
    }
    m_libraryModificationRequestRunning = true;
    const QString root = m_docsRoot;
    QPointer<CommentsOverview> guard(this);
    m_catalogPool->start([guard, root, paths] {
        bool incomplete = false;
        const auto dates = libraryModificationDates(root, paths, &incomplete);
        if (!guard) { return; }
        QMetaObject::invokeMethod(guard.data(), [guard, dates, incomplete] {
            if (!guard) { return; }
            guard->m_libraryModificationRequestRunning = false;
            if (incomplete) {
                for (auto it = dates.cbegin(); it != dates.cend(); ++it) guard->m_libraryModificationDates.insert(it.key(), it.value());
            } else {
                guard->m_libraryModificationDates = dates;
            }
            guard->applyLibraryModificationDates();
            const bool pending = std::exchange(guard->m_libraryModificationRequestPending, false);
            if (incomplete) {
                QTimer::singleShot(100, guard.data(), [guard] { if (guard) guard->requestLibraryModificationDates(); });
            } else if (pending) {
                guard->requestLibraryModificationDates();
            }
        }, Qt::QueuedConnection);
    });
}

static QStringList monitoringDocumentIdentity(const TrackedCommentDocument &document)
{
    return {document.libraryFilename, document.librarySha256, document.libraryProject, document.libraryKind,
            document.libraryPublishedAt.toString(Qt::ISODateWithMs)};
}

void CommentsOverview::requestLibraryMonitoringInformation(bool force)
{
    if (m_syncContentDeferred) { m_libraryMonitoringForce |= force; return; }
    if (m_libraryMonitoringRunning) {
        m_libraryMonitoringRefreshAgain |= force;
        return;
    }
    if (!m_tabs || m_tabs->currentWidget() != m_summaryPage) { return; }
    m_libraryMonitoringForce |= force;
    if (m_libraryMonitoringScheduled) { return; }
    m_libraryMonitoringScheduled = true;
    // Switching tabs only schedules work. Keep the existing tree on screen.
    QTimer::singleShot(0, this, &CommentsOverview::startLibraryMonitoringRefresh);
}

void CommentsOverview::startLibraryMonitoringRefresh()
{
    m_libraryMonitoringScheduled = false;
    if (!m_tabs || m_tabs->currentWidget() != m_summaryPage) { return; }
    const bool force = std::exchange(m_libraryMonitoringForce, false);
    const QSet<QString> removals = pendingLibraryRemovalFilenames(m_pendingChanges);
    m_libraryMonitoringRequestIdentities.clear();
    QStringList targets;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (removals.contains(document.libraryFilename.toCaseFolded()) || document.libraryKind != QLatin1String("source_linked")) { continue; }
        const QStringList identity = monitoringDocumentIdentity(document);
        if (!force && m_sourceMonitoringInformation.contains(document.pdfPath) && m_libraryMonitoringIdentities.value(document.pdfPath) == identity) { continue; }
        m_libraryMonitoringRequestIdentities.insert(document.pdfPath, identity);
        targets.append(document.libraryFilename);
    }
    if (targets.isEmpty()) { return; }
    targets.removeDuplicates();
    m_libraryMonitoringRunning = true;
    Q_EMIT sourceMonitoringBatchRequested(++m_libraryMonitoringRequestId, targets);
}

void CommentsOverview::applySourceMonitoringBatch(quint64 requestId, const QVariantMap &information)
{
    if (!m_libraryMonitoringRunning || requestId != m_libraryMonitoringRequestId) { return; }
    m_libraryMonitoringRunning = false;
    if (std::exchange(m_libraryMonitoringRefreshAgain, false)) {
        // Local changes or another activation superseded this snapshot.
        requestLibraryMonitoringInformation(true);
        return;
    }
    const QSet<QString> removals = pendingLibraryRemovalFilenames(m_pendingChanges);
    bool retry = false;
    {
        const QScopedValueRollback<bool> batch(m_applyingLibraryMonitoringBatch, true);
        for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
            const auto requested = m_libraryMonitoringRequestIdentities.constFind(document.pdfPath);
            if (requested == m_libraryMonitoringRequestIdentities.cend() || removals.contains(document.libraryFilename.toCaseFolded())) { continue; }
            if (*requested != monitoringDocumentIdentity(document)) { retry = true; continue; }
            QVariantMap result = information.value(document.libraryFilename).toMap();
            if (result.isEmpty()) {
                result = {{QStringLiteral("status"), QStringLiteral("unavailable")},
                          {QStringLiteral("error"), i18n("Local source monitoring is unavailable.")}};
            }
            m_libraryMonitoringIdentities.insert(document.pdfPath, *requested);
            applySourceMonitoringInformation(document.pdfPath, result);
            retry |= result.value(QStringLiteral("status")).toString() == QLatin1String("reconciling");
        }
    }
    if (retry) { QTimer::singleShot(750, this, [this] { requestLibraryMonitoringInformation(true); }); }
}

bool CommentsOverview::libraryTreeOnScreen() const
{
    return m_libraryTree && isVisible() && !isMinimized() && m_tabs && m_tabs->currentWidget() == m_summaryPage;
}

bool CommentsOverview::libraryReadsPending() const
{
    // Reads under way, or about to start, that end in another Library change.
    return (m_reloadTimer->isActive() && !m_reloadDeferred) || !m_runningDocumentReloads.isEmpty() || m_fullReloadRunning
        || m_catalogSignatureCheckRunning || (m_catalogCheckRequested && m_catalogPollTimer->isActive());
}

void CommentsOverview::showLibraryBuildFailure(const QString &name, const QString &message)
{
    // The failure message, then the Sync log that reported it: it holds the
    // build output, Retry build and the source changes of the failed build.
    auto *box = new QMessageBox(QMessageBox::Warning, SyncStatusLabels::pdfBuildFailed(), i18n("“%1”: the latest PDF build failed. The Library keeps the last PDF that built.", name),
                                QMessageBox::Close, this);
    box->setObjectName(QStringLiteral("libraryBuildErrorBox"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(message);
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *logs = box->addButton(SyncStatusLabels::showLogs(), QMessageBox::ActionRole);
    logs->setObjectName(QStringLiteral("libraryBuildErrorShowLogs"));
    // The Sync that reported it: one that succeeded with the failed build as
    // its warning, or (an earlier run) one that failed with it.
    const QString operationId = (m_latestSyncOperation.value(QStringLiteral("warning_code")).toString() == QLatin1String("pdf_build_failed")
                                 || m_latestSyncOperation.value(QStringLiteral("last_error_code")).toString() == QLatin1String("pdf_build_failed"))
        ? m_latestSyncOperation.value(QStringLiteral("id")).toString() : QString();
    connect(logs, &QAbstractButton::clicked, this, [this, operationId] {
        if (operationId.isEmpty()) Q_EMIT syncOperationsRequested();
        else Q_EMIT syncOperationLogsRequested(operationId);
    });
    box->open();
}

QHash<QString, QString> CommentsOverview::libraryBuildWarnings() const
{
    QHash<QString, QString> warnings;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (document.libraryFilename.isEmpty() || document.libraryKind != QLatin1String("source_linked")) continue;
        const QJsonObject presentation = m_documentSyncPresentations.value(document.libraryFilename).toObject();
        const bool attention = presentation.value(QStringLiteral("last_build_failed")).toBool();
        if (!document.libraryPdfStale && !attention) continue;
        // The compiler's message names the failing file and line; Fedora's
        // render status may only say that the build needs attention.
        QString message = document.libraryLatestBuildMessage.trimmed();
        if (message.isEmpty() && attention) message = presentation.value(QStringLiteral("reason")).toString().trimmed();
        warnings.insert(document.libraryFilename, message);
    }
    return warnings;
}

void CommentsOverview::scheduleLibraryTreeUpdate(const QString &reason)
{
    if (!m_libraryTree) return;
    if (!m_libraryTreeReasons.contains(reason)) m_libraryTreeReasons.append(reason);
    if (m_syncLibraryTree) {
        // A Sync refresh fills its off-screen tree now and publishes it once.
        updateLibraryTree();
        return;
    }
    if (!m_libraryTreeDirty) {
        m_libraryTreeDirty = true;
        m_libraryTreeDirtySince.start();
    }
    // Off screen, showEvent() and the Projects tab start the rebuild.
    if (libraryTreeOnScreen() && !m_libraryTreeTimer->isActive()) m_libraryTreeTimer->start();
}

void CommentsOverview::flushLibraryTreeUpdate()
{
    // One source change arrives as a file event, a pending-change snapshot and
    // a document read about a second apart (developer log 2026-10-13 13:39);
    // each rebuilt the tree on the UI thread. Rebuild once, after the reads.
    constexpr qint64 coalesceLimitMs = 2000;
    if (!m_libraryTreeDirty || m_syncContentDeferred || m_syncRefreshPending) return; // the Sync refresh rebuilds it
    if (!libraryTreeOnScreen()) {
        DeveloperLog::record(QStringLiteral("library.tree.deferred"), {{QStringLiteral("reasons"), QJsonArray::fromStringList(m_libraryTreeReasons)}});
        return;
    }
    // Context-menu callbacks can retain tree items until their nested event
    // loop closes. Do not delete those items underneath an active action.
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()
        || (libraryReadsPending() && m_libraryTreeDirtySince.elapsed() < coalesceLimitMs)) {
        m_libraryTreeTimer->start();
        return;
    }
    updateLibraryTree();
}

void CommentsOverview::updateDashboardSummary(bool refreshLibrary)
{
    if (m_syncContentDeferred) { updateSyncBuildSummary(); return; }
    if (!m_librarySummary || !m_libraryTree) {
        return;
    }

    const QSet<QString> pendingRemovals = pendingLibraryRemovalFilenames(m_pendingChanges);
    const int visibleDocuments = int(std::count_if(m_documents.cbegin(), m_documents.cend(), [&pendingRemovals](const TrackedCommentDocument &document) {
        return !document.pdfPath.isEmpty() && !pendingRemovals.contains(document.libraryFilename.toCaseFolded());
    }));
    const int linkedDocuments = int(std::count_if(m_documents.cbegin(), m_documents.cend(), [&pendingRemovals](const TrackedCommentDocument &document) {
        return document.libraryKind == QLatin1String("source_linked")
            && !pendingRemovals.contains(document.libraryFilename.toCaseFolded());
    }));
    const int pdfOnlyDocuments = visibleDocuments - linkedDocuments;
    qint64 pdfBytes = 0;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (pendingRemovals.contains(document.libraryFilename.toCaseFolded())) {
            continue;
        }
        pdfBytes += qMax<qint64>(0, document.pdfSizeBytes);
    }
    const QString librarySize = QLocale().formattedDataSize(pdfBytes, 1, QLocale::DataSizeIecFormat);
    const QString summary = !m_documentsLoaded ? i18n("Loading the Library…") : i18n("%1 · %2 · %3 · PDF files use %4",
                                   i18np("%1 entry", "%1 entries", visibleDocuments),
                                   i18np("%1 project with sources", "%1 projects with sources", linkedDocuments),
                                   i18np("%1 PDF-only", "%1 PDF-only", pdfOnlyDocuments),
                                   librarySize);
    if (m_syncLibraryTree) m_syncLibrarySummary = summary;
    else m_librarySummary->setText(summary);
    if (refreshLibrary) { scheduleLibraryTreeUpdate(QStringLiteral("documents")); }
    if (refreshLibrary && m_tabs && m_tabs->currentWidget() == m_summaryPage) {
        QTimer::singleShot(0, this, [this] { requestLibraryMonitoringInformation(); });
    }

    updateSyncBuildSummary();
}

void CommentsOverview::updateSyncBuildSummary()
{
    if (!m_syncBuildSummary) {
        return;
    }

    const QString syncStatus = m_pdfSyncStatus ? m_pdfSyncStatus->text() : i18n("Checking sync status…");
    const QString syncTimestamp = m_pdfSyncTimestamp ? m_pdfSyncTimestamp->text() : i18n("Last sync: Checking…");
    const QString lastBuild = m_pdfLastSuccessfulBuildAt.isValid()
        ? QLocale().toString(m_pdfLastSuccessfulBuildAt.toLocalTime(), QLocale::ShortFormat)
        : i18n("Never");
    m_syncBuildSummary->setText(i18n("%1\n%2 · Last build: %3", syncStatus, syncTimestamp, lastBuild));
    m_syncBuildSummary->setToolTip(m_idlePdfSyncStatusDetails);
}


void CommentsOverview::scheduleLibrarySearchUpdate()
{
    if (m_librarySearchTimer && !m_shuttingDown) m_librarySearchTimer->start();
}

void CommentsOverview::startLibrarySearchUpdate()
{
    if (m_shuttingDown || !m_librarySearchDocumentsKnown) return;
    if (m_librarySearchRunning) {
        m_librarySearchPending = true;
        return;
    }
    // Snapshot on the GUI thread; the worker only reads these copies.
    QList<LibrarySearchPdf> pdfs;
    QList<LibrarySearchRecord> records;
    QHash<QString, QString> titles;
    for (const TrackedCommentDocument &document : std::as_const(m_documents)) {
        if (!document.pdfPath.isEmpty()) titles.insert(document.pdfPath, document.displayName);
        if (!document.pdfPath.isEmpty() && document.pdfAvailable) {
            const QFileInfo file(document.pdfPath);
            pdfs.append({document.pdfPath, document.displayName, document.libraryProject,
                         QStringLiteral("%1|%2|%3|%4").arg(document.librarySha256).arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch()).arg(document.libraryProject)});
        }
        for (const TrackedComment &comment : document.comments) {
            if (comment.agentProposal || (comment.contents.trimmed().isEmpty() && comment.selectedText.trimmed().isEmpty())) continue;
            const bool note = comment.logicalPayload.value(QStringLiteral("subtype")).toString() == QLatin1String("Note");
            const bool diary = diaryEntry(comment.logicalPayload);
            const QString pdfPath = comment.pdfPath.isEmpty() ? document.pdfPath : comment.pdfPath;
            records.append({diary ? QStringLiteral("diary") : note ? QStringLiteral("note") : QStringLiteral("comment"), comment.id, pdfPath, comment.page, QString(),
                            comment.contents + QLatin1Char('\n') + comment.selectedText,
                            diary ? QLocale().toString(diaryEntryTime(comment.logicalPayload), QLocale::ShortFormat) : note ? i18n("Note") : document.displayName});
        }
    }
    // A card's PDF is resolved in the worker: reviewCardPdfPath reads the Library
    // and identity files, and once per card on this thread it stalled scrolling
    // whenever a Sync update refreshed the index.
    struct SearchCard {
        QJsonObject record;
        QString id;
        QString stem;
        QString question;
        QString text;
        int page = 0;
    };
    QList<SearchCard> cards;
    if (m_librarySearchCardsKnown && m_reviewCardsWidget) {
        for (const ReviewBrowserCard &item : m_reviewCardsWidget->libraryCards()) {
            const ReviewCard &card = item.card;
            if (card.schedule.deleted) continue;
            const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
            const QString answer = primary.value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString();
            cards.append({card.record, card.id(), card.pdfStem(), card.question(),
                          QStringList{answer, primary.value(QStringLiteral("selected_text")).toString(), card.record.value(QStringLiteral("note")).toString()}
                              .join(QLatin1Char('\n')),
                          card.page()});
        }
    }
    m_librarySearchRunning = true;
    const QString path = m_librarySearchIndexPath;
    if (LibrarySearchIndex(path).isEmpty() && !pdfs.isEmpty()) {
        m_librarySearch->setIndexingStatus(i18np("Indexing one PDF…", "Indexing %1 PDFs…", pdfs.size()));
    }
    // Comment and card records are replaced as a whole, so keep the stored
    // cards until the card list has loaded once.
    const bool cardsKnown = m_librarySearchCardsKnown;
    QPointer<CommentsOverview> guard(this);
    m_librarySearchPool->start([guard, path, pdfs, records, cards, titles, cardsKnown, root = m_docsRoot]() mutable {
        LibrarySearchUpdate summary;
        QString error;
        QHash<QString, QString> pdfByDocument;
        for (const SearchCard &card : std::as_const(cards)) {
            const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
            const QString documentId = primary.value(QStringLiteral("pdf_document_id")).toString(primary.value(QStringLiteral("document_id")).toString());
            // Cards of one document share its PDF; resolve each document once.
            QString pdfPath;
            if (documentId.isEmpty()) {
                pdfPath = reviewCardPdfPath(root, card.record);
            } else {
                auto known = pdfByDocument.constFind(documentId);
                if (known == pdfByDocument.cend()) known = pdfByDocument.insert(documentId, reviewCardPdfPath(root, card.record));
                pdfPath = *known;
            }
            const QString document = titles.value(pdfPath, card.stem);
            records.append({QStringLiteral("card"), card.id, pdfPath, card.page, card.question, card.text,
                            card.page > 0 ? i18n("%1 · p. %2", document, card.page) : document});
        }
        // Name documents as reader tabs do: the Library's own name when it has one.
        const QJsonArray projects = LibraryProjectStore(root).projects(false);
        QFile index(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
        const QJsonArray publications = index.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray() : QJsonArray();
        for (LibrarySearchPdf &pdf : pdfs) {
            const QUrl url = QUrl::fromLocalFile(pdf.pdfPath);
            const QString name = readerDisplayTitle(projects, publications, root, url);
            if (name != url.fileName()) pdf.libraryTitle = name;
        }
        QStringList kinds{QStringLiteral("comment"), QStringLiteral("note"), QStringLiteral("diary")};
        if (cardsKnown) kinds.append(QStringLiteral("card"));
        const bool ok = LibrarySearchIndex(path).update(pdfs, records, kinds, &summary, &error);
        QMetaObject::invokeMethod(
            qApp,
            [guard, ok, summary, error] {
                if (!guard) return;
                guard->m_librarySearchRunning = false;
                guard->m_librarySearch->setIndexingStatus(ok ? QString() : i18n("The search index could not be updated: %1", error));
                guard->m_librarySearch->refreshResults();
                DeveloperLog::record(QStringLiteral("library.search.indexed"),
                                     QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("pdfs_indexed"), summary.indexedPdfs},
                                                 {QStringLiteral("pdfs_removed"), summary.removedPdfs}, {QStringLiteral("pages"), summary.pages},
                                                 {QStringLiteral("records"), summary.records}, {QStringLiteral("extract_ms"), summary.extractMs},
                                                 {QStringLiteral("write_ms"), summary.writeMs}, {QStringLiteral("error"), error}});
                if (guard->m_librarySearchPending) {
                    guard->m_librarySearchPending = false;
                    guard->startLibrarySearchUpdate();
                }
            },
            Qt::QueuedConnection);
    });
}

void CommentsOverview::scheduleLibrarySourceSearchUpdate(bool immediately)
{
    if (m_shuttingDown || !m_librarySourceSearchTimer || !m_librarySourceSearchDirty) return;
    // Documents alone never read the sources; choosing Sources or All catches up.
    if (!m_librarySearch || m_librarySearch->scope() == LibrarySearchScope::Documents) return;
    m_librarySourceSearchTimer->start(immediately ? 0 : 1500);
}

void CommentsOverview::startLibrarySourceSearchUpdate()
{
    // A change during a running update stays dirty and is picked up when it ends.
    if (m_shuttingDown || !m_librarySourceSearchDirty || m_librarySourceSearchRunning) return;
    m_librarySourceSearchDirty = false;
    m_librarySourceSearchRunning = true;
    const QString path = m_librarySearchIndexPath;
    if (!LibrarySearchIndex(path).hasSources()) m_librarySearch->setSourceIndexingStatus(i18n("Indexing source files…"));
    QPointer<CommentsOverview> guard(this);
    m_librarySearchPool->start([guard, path, root = m_docsRoot] {
        QElapsedTimer timer;
        timer.start();
        // Every source folder of every project, named by its project.
        QList<LibrarySearchSourceFolder> folders;
        const QHash<QString, QJsonObject> noInventory;
        const LibraryProjectStore store(root);
        for (const QJsonValue &value : store.projects(true, &noInventory)) {
            const QJsonObject project = value.toObject();
            const QString name = project.value(QStringLiteral("name")).toString();
            for (const QJsonValue &source : project.value(QStringLiteral("sources")).toArray()) {
                const QString owner = source.toObject().value(QStringLiteral("id")).toString();
                if (SourceOwner::isOwner(owner)) folders.append({owner, name});
            }
        }
        const QList<LibrarySearchSource> sources = LibrarySearchIndex::listSources(root, folders);
        const qint64 scanMs = timer.elapsed();
        LibrarySearchSourceUpdate summary;
        QString error;
        // A Library that could not be read keeps the previous index.
        const bool ok = store.error().isEmpty() ? LibrarySearchIndex(path).updateSources(sources, &summary, &error) : (error = store.error(), false);
        QMetaObject::invokeMethod(
            qApp,
            [guard, ok, summary, error, scanMs, folders = folders.size()] {
                if (!guard) return;
                guard->m_librarySourceSearchRunning = false;
                if (!ok) guard->m_librarySourceSearchDirty = true;
                guard->m_librarySearch->setSourceIndexingStatus(ok ? QString() : i18n("The source index could not be updated: %1", error));
                guard->m_librarySearch->refreshResults();
                DeveloperLog::record(QStringLiteral("library.search.sources_indexed"),
                                     QJsonObject{{QStringLiteral("ok"), ok}, {QStringLiteral("folders"), folders}, {QStringLiteral("files"), summary.files},
                                                 {QStringLiteral("files_read"), summary.read}, {QStringLiteral("files_unchanged"), summary.unchanged},
                                                 {QStringLiteral("files_removed"), summary.removed}, {QStringLiteral("blocks"), summary.chunks},
                                                 {QStringLiteral("scan_ms"), scanMs}, {QStringLiteral("read_ms"), summary.readMs},
                                                 {QStringLiteral("write_ms"), summary.writeMs}, {QStringLiteral("error"), error}});
                // A failure retries with the next change, not in a loop.
                if (ok) guard->scheduleLibrarySourceSearchUpdate();
            },
            Qt::QueuedConnection);
    });
}

void CommentsOverview::openLibrarySearchHit(const LibrarySearchHit &hit, const QString &query)
{
    DeveloperLog::record(QStringLiteral("library.search.opened"), QJsonObject{{QStringLiteral("kind"), hit.kind}});
    if (hit.kind == QLatin1String("source") || hit.kind == QLatin1String("source-line")) {
        Q_EMIT openSourceRequested(hit.sourcePath, qMax(1, hit.line));
        return;
    }
    if (hit.kind == QLatin1String("document")) {
        // A match in its name alone leaves nothing for the document search to find.
        Q_EMIT librarySearchPageActivated(hit.pdfPath, hit.page, hit.matchingPages > 0 ? query : QString());
        return;
    }
    if (hit.kind == QLatin1String("card")) {
        showReviewCardInfo(hit.ref);
        return;
    }
    // Comments and notes: select the row in their tab, then open a comment in its PDF.
    if (revealComment(hit.pdfPath, hit.ref) && hit.kind == QLatin1String("comment") && m_currentComment.isValid() && m_currentAnnotationId == hit.ref) {
        activateComment(m_table->currentIndex());
    }
}

}
