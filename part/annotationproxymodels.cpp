/*
    SPDX-FileCopyrightText: 2007 Tobias Koenig <tokoe@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annotationproxymodels.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <QDateTime>

#include <QItemSelection>
#include <QList>

#include <QIcon>

#include "annotationmodel.h"
#include "core/annotations.h"
#include "gui/debug_ui.h"

PageFilterProxyModel::PageFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , mGroupByCurrentPage(false)
    , mCurrentPage(-1)
{
    setDynamicSortFilter(true);
    sort(0, Qt::AscendingOrder);
}

void PageFilterProxyModel::groupByCurrentPage(bool value)
{
    if (mGroupByCurrentPage == value) {
        return;
    }

    mGroupByCurrentPage = value;

    invalidateFilter();
}

void PageFilterProxyModel::setCurrentPage(int page)
{
    if (mCurrentPage == page) {
        return;
    }

    mCurrentPage = page;

    // no need to invalidate when we're not showing the current page only
    if (!mGroupByCurrentPage) {
        return;
    }

    invalidateFilter();
}

bool PageFilterProxyModel::filterAcceptsRow(int row, const QModelIndex &sourceParent) const
{
    const QModelIndex index = sourceModel()->index(row, 0, sourceParent);
    if (sourceModel()->rowCount(index) > 0) {
        for (int child = 0; child < sourceModel()->rowCount(index); ++child) {
            if (filterAcceptsRow(child, index)) return true;
        }
        return false;
    }
    if (!(index.data(AnnotationModel::ItemTypeRole).toInt() & mVisibleItemTypes)) return false;
    const int page = index.data(AnnotationModel::PageRole).toInt();
    if (mGroupByCurrentPage && index.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard) {
        // A card on the newest version has no page here: it is listed whatever the page.
        return index.data(AnnotationModel::ReviewCardOnNewestVersionRole).toBool() || index.data(AnnotationModel::PagesRole).toList().contains(mCurrentPage);
    }
    if (mGroupByCurrentPage && index.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::Comment) {
        const auto threadId = [this](const QModelIndex &comment) {
            const QString id = comment.data(AnnotationModel::ItemIdRole).toString();
            const QString canonical = mSourceOrder.value(id).threadRootId;
            const QString native = comment.data(AnnotationModel::ThreadRootRole).toString();
            return !canonical.isEmpty() ? canonical : (!native.isEmpty() ? native : id);
        };
        const QString root = threadId(index);
        if (!root.isEmpty()) {
            // A page filter keeps the whole thread when any member is on that page.
            for (int group = 0; group < sourceModel()->rowCount(); ++group) {
                const auto parent = sourceModel()->index(group, 0);
                for (int row = 0; row < sourceModel()->rowCount(parent); ++row) {
                    const auto candidate = sourceModel()->index(row, 0, parent);
                    if (candidate.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::Comment
                        && candidate.data(AnnotationModel::PageRole).toInt() == mCurrentPage && threadId(candidate) == root) return true;
                }
            }
        }
    }
    if (page < 0 && index.data(AnnotationModel::DocumentFeedbackRole).toBool()) return true; // On no page.
    return !mGroupByCurrentPage || page == mCurrentPage;
}

void PageFilterProxyModel::setVisibleItemTypes(int types)
{
    if (mVisibleItemTypes == types) return;
    mVisibleItemTypes = types;
    invalidateFilter();
}

QVariant PageFilterProxyModel::data(const QModelIndex &index, int role) const
{
    if (role == AnnotationModel::ThreadRootRole) {
        const auto entry = mSourceOrder.constFind(index.data(AnnotationModel::ItemIdRole).toString());
        if (entry != mSourceOrder.cend() && !entry->threadRootId.isEmpty()) return entry->threadRootId;
    }
    return QSortFilterProxyModel::data(index, role);
}

void PageFilterProxyModel::setAnnotationSourceOrder(const QHash<QString, AnnotationSourceOrder> &sourceOrder)
{
    if (mSourceOrder == sourceOrder) {
        return;
    }
    mSourceOrder = sourceOrder;
    invalidate();
    sort(0, Qt::AscendingOrder);
}

void PageFilterProxyModel::setReviewCardsDueFirst(bool dueFirst)
{
    if (mReviewCardsDueFirst == dueFirst) return;
    mReviewCardsDueFirst = dueFirst;
    invalidate();
    sort(0, Qt::AscendingOrder);
}

bool PageFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (mReviewCardsDueFirst && left.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard
        && right.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard) {
        const auto key = [](const QModelIndex &index) {
            const QVariantList order = index.data(AnnotationModel::ReviewCardDueOrderRole).toList();
            return std::pair(order.value(0).toInt(), order.value(1).toLongLong());
        };
        // Equal ones keep their page order.
        if (key(left) != key(right)) return key(left) < key(right);
    }
    // Feedback made in an HTML attachment has no page: after the pages.
    const auto order = [](const QModelIndex &index) {
        const int page = index.data(AnnotationModel::PageRole).toInt();
        return page < 0 && index.data(AnnotationModel::DocumentFeedbackRole).toBool() ? std::numeric_limits<int>::max() : page;
    };
    const int leftPage = order(left);
    const int rightPage = order(right);
    if (leftPage != rightPage) {
        return leftPage < rightPage;
    }

    const auto *annotationModel = qobject_cast<const AnnotationModel *>(sourceModel());
    const Okular::Annotation *leftAnnotation = annotationModel ? annotationModel->annotationForIndex(left) : nullptr;
    const Okular::Annotation *rightAnnotation = annotationModel ? annotationModel->annotationForIndex(right) : nullptr;
    if (!leftAnnotation || !rightAnnotation) {
        return left.row() < right.row();
    }

    const auto leftOrder = mSourceOrder.constFind(left.data(AnnotationModel::ItemIdRole).toString());
    const auto rightOrder = mSourceOrder.constFind(right.data(AnnotationModel::ItemIdRole).toString());
    const bool leftHasSourcePosition = leftOrder != mSourceOrder.cend() && leftOrder->hasSourcePosition;
    const bool rightHasSourcePosition = rightOrder != mSourceOrder.cend() && rightOrder->hasSourcePosition;
    if (leftHasSourcePosition != rightHasSourcePosition) {
        return leftHasSourcePosition;
    }
    if (leftHasSourcePosition) {
        if (leftOrder->sourceOrder != rightOrder->sourceOrder) {
            return leftOrder->sourceOrder < rightOrder->sourceOrder;
        }
        if (leftOrder->inputOrder != rightOrder->inputOrder) {
            return leftOrder->inputOrder < rightOrder->inputOrder;
        }
        if (leftOrder->line != rightOrder->line) {
            return leftOrder->line < rightOrder->line;
        }
        if (leftOrder->column != rightOrder->column) {
            return leftOrder->column < rightOrder->column;
        }
    }

    const Okular::NormalizedRect leftRect = leftAnnotation->boundingRectangle();
    const Okular::NormalizedRect rightRect = rightAnnotation->boundingRectangle();
    const auto hasFiniteGeometry = [](const Okular::NormalizedRect &rect) { return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right) && std::isfinite(rect.bottom); };
    const bool leftHasFiniteGeometry = hasFiniteGeometry(leftRect);
    const bool rightHasFiniteGeometry = hasFiniteGeometry(rightRect);
    if (leftHasFiniteGeometry != rightHasFiniteGeometry) {
        return leftHasFiniteGeometry;
    }
    if (!leftHasFiniteGeometry) {
        return left.row() < right.row();
    }

    if (leftRect.top != rightRect.top) {
        return leftRect.top < rightRect.top;
    }
    if (leftRect.left != rightRect.left) {
        return leftRect.left < rightRect.left;
    }
    if (leftRect.bottom != rightRect.bottom) {
        return leftRect.bottom < rightRect.bottom;
    }
    if (leftRect.right != rightRect.right) {
        return leftRect.right < rightRect.right;
    }

    // Preserve the PDF's embedded order for annotations at the exact same position.
    return left.row() < right.row();
}

PageGroupProxyModel::PageGroupProxyModel(QObject *parent)
    : QAbstractProxyModel(parent)
    , mGroupByPage(false)
{
}

int PageGroupProxyModel::columnCount(const QModelIndex &parentIndex) const
{
    // For top-level and second level we have always only one column
    if (mGroupByPage) {
        if (parentIndex.isValid()) {
            if (parentIndex.parent().isValid()) {
                return 0;
            } else {
                return 1; // second-level
            }
        } else {
            return 1; // top-level
        }
    } else {
        if (!parentIndex.isValid()) { // top-level
            return 1;
        } else {
            return 0;
        }
    }
    return 1;
}

int PageGroupProxyModel::rowCount(const QModelIndex &parentIndex) const
{
    if (mGroupByPage) {
        if (parentIndex.isValid()) {
            if (parentIndex.parent().isValid()) {
                return 0;
            } else {
                return mTreeIndexes[parentIndex.row()].second.count(); // second-level
            }
        } else {
            return mTreeIndexes.count(); // top-level
        }
    } else {
        if (!parentIndex.isValid()) { // top-level
            return mIndexes.count();
        } else {
            return 0;
        }
    }
}

QModelIndex PageGroupProxyModel::index(int row, int column, const QModelIndex &parentIndex) const
{
    if (row < 0 || column != 0) {
        return QModelIndex();
    }

    if (mGroupByPage) {
        if (parentIndex.isValid()) {
            if (parentIndex.row() >= 0 && parentIndex.row() < mTreeIndexes.count() && row < mTreeIndexes[parentIndex.row()].second.count()) {
                return createIndex(row, column, qint32(parentIndex.row() + 1));
            } else {
                return QModelIndex();
            }
        } else {
            if (row < mTreeIndexes.count()) {
                return createIndex(row, column);
            } else {
                return QModelIndex();
            }
        }
    } else {
        // We have only top-level items
        if (!parentIndex.isValid() && row < mIndexes.count()) {
            return createIndex(row, column);
        } else {
            return QModelIndex();
        }
    }
}

QModelIndex PageGroupProxyModel::parent(const QModelIndex &idx) const
{
    if (mGroupByPage) {
        if (idx.internalId() == 0) { // top-level
            return QModelIndex();
        } else {
            return index(idx.internalId() - 1, idx.column());
        }
    } else {
        // We have only top-level items
        return QModelIndex();
    }
}

QModelIndex PageGroupProxyModel::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (mGroupByPage) {
        if (sourceIndex.parent().isValid()) {
            return index(sourceIndex.row(), sourceIndex.column(), sourceIndex.parent());
        } else {
            return index(sourceIndex.row(), sourceIndex.column());
        }
    } else {
        for (int i = 0; i < mIndexes.count(); ++i) {
            if (mIndexes[i] == sourceIndex) {
                return index(i, 0);
            }
        }

        return QModelIndex();
    }
}

QModelIndex PageGroupProxyModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid()) {
        return QModelIndex();
    }

    if (mGroupByPage) {
        if (proxyIndex.internalId() == 0) {
            if (proxyIndex.row() >= mTreeIndexes.count() || proxyIndex.row() < 0) {
                return QModelIndex();
            }

            return mTreeIndexes[proxyIndex.row()].first;
        } else {
            if (qint32(proxyIndex.internalId()) - 1 >= mTreeIndexes.count() || proxyIndex.row() >= mTreeIndexes[proxyIndex.internalId() - 1].second.count()) {
                return QModelIndex();
            }

            return mTreeIndexes[proxyIndex.internalId() - 1].second[proxyIndex.row()];
        }
    } else {
        if (proxyIndex.column() > 0 || proxyIndex.row() >= mIndexes.count()) {
            return QModelIndex();
        } else {
            return mIndexes[proxyIndex.row()];
        }
    }
}

void PageGroupProxyModel::setSourceModel(QAbstractItemModel *model)
{
    if (sourceModel()) {
        disconnect(sourceModel(), &QAbstractItemModel::layoutAboutToBeChanged, this, &PageGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::layoutChanged, this, &PageGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::modelAboutToBeReset, this, &PageGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::modelReset, this, &PageGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsAboutToBeInserted, this, &PageGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsInserted, this, &PageGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsAboutToBeRemoved, this, &PageGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsRemoved, this, &PageGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::dataChanged, this, &PageGroupProxyModel::sourceDataChanged);
    }

    QAbstractProxyModel::setSourceModel(model);

    connect(sourceModel(), &QAbstractItemModel::layoutAboutToBeChanged, this, &PageGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::layoutChanged, this, &PageGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::modelAboutToBeReset, this, &PageGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::modelReset, this, &PageGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsAboutToBeInserted, this, &PageGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsInserted, this, &PageGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsAboutToBeRemoved, this, &PageGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsRemoved, this, &PageGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::dataChanged, this, &PageGroupProxyModel::sourceDataChanged);

    rebuildIndexes();
}

void PageGroupProxyModel::doRebuildIndexes()
{
    if (mGroupByPage) {
        mTreeIndexes.clear();

        for (int row = 0; row < sourceModel()->rowCount(); ++row) {
            const QModelIndex pageIndex = sourceModel()->index(row, 0);

            QList<QModelIndex> itemIndexes;
            for (int subRow = 0; subRow < sourceModel()->rowCount(pageIndex); ++subRow) {
                itemIndexes.append(sourceModel()->index(subRow, 0, pageIndex));
            }

            mTreeIndexes.append(QPair<QModelIndex, QList<QModelIndex>>(pageIndex, itemIndexes));
        }
    } else {
        mIndexes.clear();

        // Flatten the source model
        for (int row = 0; row < sourceModel()->rowCount(); ++row) {
            const QModelIndex pageIndex = sourceModel()->index(row, 0);
            for (int subRow = 0; subRow < sourceModel()->rowCount(pageIndex); ++subRow) {
                mIndexes.append(sourceModel()->index(subRow, 0, pageIndex));
            }
        }
    }
}

void PageGroupProxyModel::aboutToRebuild()
{
    beginResetModel();
}

void PageGroupProxyModel::rebuild()
{
    doRebuildIndexes();
    endResetModel();
}

void PageGroupProxyModel::rebuildIndexes()
{
    aboutToRebuild();
    rebuild();
}

void PageGroupProxyModel::sourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles)
{
    Q_EMIT dataChanged(mapFromSource(topLeft), mapFromSource(bottomRight), roles);
}

void PageGroupProxyModel::groupByPage(bool value)
{
    if (mGroupByPage == value) {
        return;
    }

    mGroupByPage = value;

    rebuildIndexes();
}

class AuthorGroupItem
{
public:
    enum Type { Page, Author, Annotation };

    explicit AuthorGroupItem(AuthorGroupItem *parent, Type type = Page, const QModelIndex &index = QModelIndex())
        : mParent(parent)
        , mType(type)
        , mIndex(index)
    {
    }

    ~AuthorGroupItem()
    {
        qDeleteAll(mChilds);
    }

    AuthorGroupItem(const AuthorGroupItem &) = delete;
    AuthorGroupItem &operator=(const AuthorGroupItem &) = delete;

    void appendChild(AuthorGroupItem *child)
    {
        mChilds.append(child);
    }
    void moveTo(AuthorGroupItem *parent)
    {
        mParent->mChilds.removeAll(this);
        mParent = parent;
        parent->appendChild(this);
    }
    void pruneEmptyGroups()
    {
        for (int i = mChilds.size() - 1; i >= 0; --i) {
            auto *child = mChilds.at(i);
            child->pruneEmptyGroups();
            if (child->type() != Annotation && child->childCount() == 0) delete mChilds.takeAt(i);
        }
    }
    AuthorGroupItem *parent() const
    {
        return mParent;
    }
    AuthorGroupItem *child(int row) const
    {
        return mChilds.value(row);
    }
    int childCount() const
    {
        return mChilds.count();
    }

    void dump(int level = 0)
    {
        QString prefix;
        for (int i = 0; i < level; ++i) {
            prefix += QLatin1Char(' ');
        }

        qCDebug(OkularUiDebug, "%s%s", qPrintable(prefix), (mType == Page ? "Page" : (mType == Author ? "Author" : "Annotation")));

        for (int i = 0; i < mChilds.count(); ++i) {
            mChilds[i]->dump(level + 2);
        }
    }

    const AuthorGroupItem *findIndex(const QModelIndex &index) const
    {
        if (index == mIndex) {
            return this;
        }

        for (int i = 0; i < mChilds.count(); ++i) {
            const AuthorGroupItem *item = mChilds[i]->findIndex(index);
            if (item) {
                return item;
            }
        }

        return nullptr;
    }

    int row() const
    {
        return (mParent ? mParent->mChilds.indexOf(const_cast<AuthorGroupItem *>(this)) : 0);
    }

    Type type() const
    {
        return mType;
    }
    QModelIndex index() const
    {
        return mIndex;
    }

    void setAuthor(const QString &author)
    {
        mAuthor = author;
    }
    QString author() const
    {
        return mAuthor;
    }

private:
    AuthorGroupItem *mParent;
    Type mType;
    QModelIndex mIndex;
    QList<AuthorGroupItem *> mChilds;
    QString mAuthor;
};

class AuthorGroupProxyModel::Private
{
public:
    explicit Private(AuthorGroupProxyModel *parent)
        : mParent(parent)
        , mRoot(nullptr)
        , mGroupByAuthor(false)
    {
    }
    ~Private()
    {
        delete mRoot;
    }

    AuthorGroupProxyModel *mParent;
    AuthorGroupItem *mRoot;
    bool mGroupByAuthor;
};

AuthorGroupProxyModel::AuthorGroupProxyModel(QObject *parent)
    : QAbstractProxyModel(parent)
    , d(new Private(this))
{
}

AuthorGroupProxyModel::~AuthorGroupProxyModel()
{
    delete d;
}

int AuthorGroupProxyModel::columnCount(const QModelIndex &) const
{
    return 1;
}

bool AuthorGroupProxyModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
}

int AuthorGroupProxyModel::rowCount(const QModelIndex &parentIndex) const
{
    AuthorGroupItem *item = nullptr;
    if (!parentIndex.isValid()) {
        item = d->mRoot;
    } else {
        item = static_cast<AuthorGroupItem *>(parentIndex.internalPointer());
    }

    return item ? item->childCount() : 0;
}

QModelIndex AuthorGroupProxyModel::index(int row, int column, const QModelIndex &parentIndex) const
{
    if (!hasIndex(row, column, parentIndex)) {
        return QModelIndex();
    }

    AuthorGroupItem *parentItem = nullptr;
    if (!parentIndex.isValid()) {
        parentItem = d->mRoot;
    } else {
        parentItem = static_cast<AuthorGroupItem *>(parentIndex.internalPointer());
    }

    AuthorGroupItem *child = parentItem->child(row);
    if (child) {
        return createIndex(row, column, child);
    } else {
        return QModelIndex();
    }
}

QModelIndex AuthorGroupProxyModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    AuthorGroupItem *childItem = static_cast<AuthorGroupItem *>(index.internalPointer());
    AuthorGroupItem *parentItem = childItem->parent();

    if (parentItem == d->mRoot) {
        return QModelIndex();
    } else {
        return createIndex(parentItem->row(), 0, parentItem);
    }
}

QModelIndex AuthorGroupProxyModel::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (!sourceIndex.isValid()) {
        return QModelIndex();
    }

    const AuthorGroupItem *item = d->mRoot->findIndex(sourceIndex);
    if (!item) {
        return QModelIndex();
    }

    return createIndex(item->row(), 0, const_cast<AuthorGroupItem *>(item));
}

QModelIndex AuthorGroupProxyModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid()) {
        return QModelIndex();
    }

    const AuthorGroupItem *item = static_cast<AuthorGroupItem *>(proxyIndex.internalPointer());

    return item->index();
}

void AuthorGroupProxyModel::setSourceModel(QAbstractItemModel *model)
{
    if (sourceModel()) {
        disconnect(sourceModel(), &QAbstractItemModel::layoutAboutToBeChanged, this, &AuthorGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::layoutChanged, this, &AuthorGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::modelAboutToBeReset, this, &AuthorGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::modelReset, this, &AuthorGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsAboutToBeInserted, this, &AuthorGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsInserted, this, &AuthorGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsAboutToBeRemoved, this, &AuthorGroupProxyModel::aboutToRebuild);
        disconnect(sourceModel(), &QAbstractItemModel::rowsRemoved, this, &AuthorGroupProxyModel::rebuild);
        disconnect(sourceModel(), &QAbstractItemModel::dataChanged, this, &AuthorGroupProxyModel::sourceDataChanged);
    }

    QAbstractProxyModel::setSourceModel(model);

    connect(sourceModel(), &QAbstractItemModel::layoutAboutToBeChanged, this, &AuthorGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::layoutChanged, this, &AuthorGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::modelAboutToBeReset, this, &AuthorGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::modelReset, this, &AuthorGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsAboutToBeInserted, this, &AuthorGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsInserted, this, &AuthorGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsAboutToBeRemoved, this, &AuthorGroupProxyModel::aboutToRebuild);
    connect(sourceModel(), &QAbstractItemModel::rowsRemoved, this, &AuthorGroupProxyModel::rebuild);
    connect(sourceModel(), &QAbstractItemModel::dataChanged, this, &AuthorGroupProxyModel::sourceDataChanged);

    rebuildIndexes();
}

static bool isAuthorItem(const QModelIndex &index)
{
    if (!index.isValid()) {
        return false;
    }

    const AuthorGroupItem *item = static_cast<AuthorGroupItem *>(index.internalPointer());
    return (item->type() == AuthorGroupItem::Author);
}

QItemSelection AuthorGroupProxyModel::mapSelectionToSource(const QItemSelection &selection) const
{
    const QModelIndexList proxyIndexes = selection.indexes();
    QItemSelection sourceSelection;
    for (const QModelIndex &proxyIndex : proxyIndexes) {
        if (!isAuthorItem(proxyIndex)) {
            sourceSelection << QItemSelectionRange(mapToSource(proxyIndex));
        }
    }

    return sourceSelection;
}

QItemSelection AuthorGroupProxyModel::mapSelectionFromSource(const QItemSelection &selection) const
{
    return QAbstractProxyModel::mapSelectionFromSource(selection);
}

QVariant AuthorGroupProxyModel::data(const QModelIndex &proxyIndex, int role) const
{
    if (proxyIndex.isValid()) {
        const auto *item = static_cast<AuthorGroupItem *>(proxyIndex.internalPointer());
        if (role == AnnotationModel::ReplyCountRole) return item->type() == AuthorGroupItem::Annotation ? item->childCount() : 0;
        if (role == AnnotationModel::IsReplyRole) return item->parent() && item->parent()->type() == AuthorGroupItem::Annotation;
    }
    if (isAuthorItem(proxyIndex)) {
        const AuthorGroupItem *item = static_cast<AuthorGroupItem *>(proxyIndex.internalPointer());
        if (role == Qt::DisplayRole) {
            return item->author();
        } else if (role == Qt::DecorationRole) {
            return QIcon::fromTheme(item->author().isEmpty() ? QStringLiteral("user-away") : QStringLiteral("user-identity"));
        } else {
            return QVariant();
        }
    } else {
        return QAbstractProxyModel::data(proxyIndex, role);
    }
}

QMap<int, QVariant> AuthorGroupProxyModel::itemData(const QModelIndex &index) const
{
    if (isAuthorItem(index)) {
        return QMap<int, QVariant>();
    } else {
        return QAbstractProxyModel::itemData(index);
    }
}

Qt::ItemFlags AuthorGroupProxyModel::flags(const QModelIndex &index) const
{
    if (isAuthorItem(index)) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    } else {
        return QAbstractProxyModel::flags(index);
    }
}

void AuthorGroupProxyModel::groupByAuthor(bool value)
{
    if (d->mGroupByAuthor == value) {
        return;
    }

    d->mGroupByAuthor = value;

    rebuildIndexes();
}

void AuthorGroupProxyModel::doRebuildIndexes()
{
    delete d->mRoot;
    d->mRoot = new AuthorGroupItem(nullptr);

    if (d->mGroupByAuthor) {
        QMap<QString, AuthorGroupItem *> authorMap;

        for (int row = 0; row < sourceModel()->rowCount(); ++row) {
            const QModelIndex idx = sourceModel()->index(row, 0);
            const QString author = sourceModel()->data(idx, AnnotationModel::AuthorRole).toString();
            if (idx.data(AnnotationModel::DocumentFeedbackRole).toBool()) {
                // Always the reader's own: no author between it and its group.
                d->mRoot->appendChild(new AuthorGroupItem(d->mRoot, AuthorGroupItem::Annotation, idx));
            } else if (idx.data(AnnotationModel::ItemTypeRole).toInt() != 0) {
                // We have the annotations as top-level, so introduce authors as new
                // top-levels and append the annotations
                AuthorGroupItem *authorItem = authorMap.value(author, nullptr);
                if (!authorItem) {
                    authorItem = new AuthorGroupItem(d->mRoot, AuthorGroupItem::Author);
                    authorItem->setAuthor(author);

                    // Add item to tree
                    d->mRoot->appendChild(authorItem);

                    // Insert to lookup list
                    authorMap.insert(author, authorItem);
                }

                AuthorGroupItem *item = new AuthorGroupItem(authorItem, AuthorGroupItem::Annotation, idx);
                authorItem->appendChild(item);
            } else {
                // We have the pages as top-level, so we use them as top-level, append the
                // authors for all annotations of the page, and then the annotations themself
                AuthorGroupItem *pageItem = new AuthorGroupItem(d->mRoot, AuthorGroupItem::Page, idx);
                d->mRoot->appendChild(pageItem);

                // First collect all authors...
                QMap<QString, AuthorGroupItem *> pageAuthorMap;
                for (int subRow = 0; subRow < sourceModel()->rowCount(idx); ++subRow) {
                    const QModelIndex annIdx = sourceModel()->index(subRow, 0, idx);
                    if (annIdx.data(AnnotationModel::DocumentFeedbackRole).toBool()) {
                        pageItem->appendChild(new AuthorGroupItem(pageItem, AuthorGroupItem::Annotation, annIdx));
                        continue;
                    }
                    const QString pageAuthor = sourceModel()->data(annIdx, AnnotationModel::AuthorRole).toString();

                    AuthorGroupItem *authorItem = pageAuthorMap.value(pageAuthor, nullptr);
                    if (!authorItem) {
                        authorItem = new AuthorGroupItem(pageItem, AuthorGroupItem::Author);
                        authorItem->setAuthor(pageAuthor);

                        // Add item to tree
                        pageItem->appendChild(authorItem);

                        // Insert to lookup list
                        pageAuthorMap.insert(pageAuthor, authorItem);
                    }

                    AuthorGroupItem *item = new AuthorGroupItem(authorItem, AuthorGroupItem::Annotation, annIdx);
                    authorItem->appendChild(item);
                }
            }
        }
    } else {
        for (int row = 0; row < sourceModel()->rowCount(); ++row) {
            const QModelIndex idx = sourceModel()->index(row, 0);
            const QString author = sourceModel()->data(idx, AnnotationModel::AuthorRole).toString();
            if (idx.data(AnnotationModel::ItemTypeRole).toInt() != 0) {
                // We have the annotations as top-level items
                AuthorGroupItem *item = new AuthorGroupItem(d->mRoot, AuthorGroupItem::Annotation, idx);
                d->mRoot->appendChild(item);
            } else {
                // We have the pages as top-level items
                AuthorGroupItem *pageItem = new AuthorGroupItem(d->mRoot, AuthorGroupItem::Page, idx);
                d->mRoot->appendChild(pageItem);

                // Append all annotations as second-level
                for (int subRow = 0; subRow < sourceModel()->rowCount(idx); ++subRow) {
                    const QModelIndex subIdx = sourceModel()->index(subRow, 0, idx);
                    AuthorGroupItem *item = new AuthorGroupItem(pageItem, AuthorGroupItem::Annotation, subIdx);
                    pageItem->appendChild(item);
                }
            }
        }
    }

    // Group only explicit thread identities, across page and author groups.
    // Missing or cyclic roots remain visible as independent comments.
    QHash<QString, AuthorGroupItem *> comments;
    QList<AuthorGroupItem *> ordered;
    const auto collect = [&](auto &&self, AuthorGroupItem *parent) -> void {
        for (int row = 0; row < parent->childCount(); ++row) {
            auto *item = parent->child(row);
            if (item->type() == AuthorGroupItem::Annotation && item->index().data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::Comment) {
                const QString id = item->index().data(AnnotationModel::ItemIdRole).toString();
                if (!id.isEmpty()) comments.insert(id, item);
                ordered.append(item);
            } else self(self, item);
        }
    };
    collect(collect, d->mRoot);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
        const auto a = left->index().data(AnnotationModel::CommentCreatedRole).toDateTime();
        const auto b = right->index().data(AnnotationModel::CommentCreatedRole).toDateTime();
        if (a.isValid() != b.isValid()) return a.isValid();
        if (a != b) return a < b;
        return left->index().data(AnnotationModel::ItemIdRole).toString() < right->index().data(AnnotationModel::ItemIdRole).toString();
    });
    for (auto *item : std::as_const(ordered)) {
        const QString rootId = item->index().data(AnnotationModel::ThreadRootRole).toString();
        auto *root = comments.value(rootId);
        if (!root || root == item) continue;
        const QString rootParent = root->index().data(AnnotationModel::ThreadRootRole).toString();
        if (!rootParent.isEmpty() && rootParent != rootId) continue;
        item->moveTo(root);
    }
    d->mRoot->pruneEmptyGroups();
}

void AuthorGroupProxyModel::refreshCommentThreads()
{
    rebuildIndexes();
}

void AuthorGroupProxyModel::aboutToRebuild()
{
    beginResetModel();
}

void AuthorGroupProxyModel::rebuild()
{
    doRebuildIndexes();
    endResetModel();
}

void AuthorGroupProxyModel::rebuildIndexes()
{
    aboutToRebuild();
    rebuild();
}

void AuthorGroupProxyModel::sourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles)
{
    for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
        const auto mapped = mapFromSource(sourceModel()->index(row, 0, topLeft.parent()));
        if (mapped.isValid()) Q_EMIT dataChanged(mapped, mapped, roles);
    }
}

#include "moc_annotationproxymodels.cpp"
