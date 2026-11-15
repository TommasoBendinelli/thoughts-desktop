/*
    SPDX-FileCopyrightText: 2006 Pino Toscano <pino@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annotationmodel.h"
#include "commentfragmentgroup.h"
#include "commentfragmentids.h"
#include "documentfeedback.h"
#include "feedbackicon.h"
#include "reviewcardfeedback.h"

#include <QJsonArray>
#include <QList>
#include <QLocale>
#include <QHash>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

#include <KLocalizedString>
#include <QIcon>

#include "core/annotations.h"
#include "core/document.h"
#include "core/observer.h"
#include "core/page.h"
#include "gui/guiutils.h"
#include "gui/commenticon.h"

struct AnnItem {
    enum class Type {
        Root,
        Page,
        Annotation,
        AgentProposalGroup,
        AgentProposal,
        DetachedCommentGroup,
        DetachedComment,
        ReviewCardGroup,
        ReviewCard,
        FeedbackGroup,
        Feedback,
    };

    AnnItem();
    AnnItem(AnnItem *parent, Okular::Annotation *ann);
    AnnItem(AnnItem *parent, int page);
    AnnItem(AnnItem *parent, Type type);
    AnnItem(AnnItem *parent, const AgentProposalSidebarEntry &proposal);
    ~AnnItem();

    AnnItem(const AnnItem &) = delete;
    AnnItem &operator=(const AnnItem &) = delete;

    AnnItem *parent;
    QList<AnnItem *> children;

    Type type;
    Okular::Annotation *annotation;
    int page;
    AgentProposalSidebarEntry proposal;
    DetachedCommentSidebarEntry comment;
    ReviewCardSidebarEntry card;
    FeedbackSidebarEntry feedback;
};

static QList<Okular::Annotation *> filterOutWidgetAndContinuationAnnotations(
    const QList<Okular::Annotation *> &annotations,
    const QList<Okular::Page *> &pages,
    int pageNumber)
{
    QList<Okular::Annotation *> result;

    for (Okular::Annotation *annotation : annotations) {
        if (annotation->subType() == Okular::Annotation::AWidget) {
            continue;
        }
        if (Thoughts::isRecognizedCommentContinuation(pages, annotation, pageNumber)) {
            continue;
        }

        result.append(annotation);
    }

    return result;
}

// A time of a card's "sidebar_schedule", in UTC; invalid when it has none.
static QDateTime scheduleTime(const QJsonObject &schedule, const QString &key)
{
    const QString text = schedule.value(key).toString();
    const QDateTime at = QDateTime::fromString(text, Qt::ISODateWithMs);
    return (at.isValid() ? at : QDateTime::fromString(text, Qt::ISODate)).toUTC();
}

// As the Review tab writes times.
static QString scheduleWhen(const QDateTime &at)
{
    return at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// How long until @p at: "in 25 minutes", "in 3 hours" (today), "tomorrow", "in 3 days", "in 2 weeks",
// "in 4 months", "in 2 years"; days are local calendar days.
static QString scheduleIn(const QDateTime &at, const QDateTime &now)
{
    const qint64 seconds = std::max<qint64>(0, now.secsTo(at));
    const qint64 days = now.toLocalTime().date().daysTo(at.toLocalTime().date());
    if (seconds < 3600) return i18np("in 1 minute", "in %1 minutes", std::max<qint64>(1, (seconds + 59) / 60));
    if (days <= 0) return i18np("in 1 hour", "in %1 hours", seconds / 3600);
    if (days == 1) return i18nc("a review card is due", "tomorrow");
    if (days < 14) return i18np("in 1 day", "in %1 days", days);
    if (days < 63) return i18np("in 1 week", "in %1 weeks", qRound(days / 7.0));
    if (days < 730) return i18np("in 1 month", "in %1 months", qRound(days / 30.44));
    return i18np("in 1 year", "in %1 years", qRound(days / 365.25));
}

// Later today by the clock, another day by its date.
static QString scheduleUntil(const QDateTime &at, const QDateTime &now)
{
    const QDateTime local = at.toLocalTime();
    return local.toString(local.date() == now.toLocalTime().date() ? QStringLiteral("HH:mm") : QStringLiteral("yyyy-MM-dd"));
}

// When a card comes due, in local time, for "Due …": "in 25 min" and "in 3 h" when that close,
// "today 21:40", "tomorrow 08:15", else its date, "29 Nov" (with the year when it is another one).
static QString scheduleDue(const QDateTime &at, const QDateTime &now)
{
    const qint64 seconds = std::max<qint64>(0, now.secsTo(at));
    const QDateTime local = at.toLocalTime();
    const QDate today = now.toLocalTime().date();
    const qint64 days = today.daysTo(local.date());
    const QString time = local.toString(QStringLiteral("HH:mm"));
    if (seconds < 3600) return i18nc("a review card is due in some minutes", "in %1 min", std::max<qint64>(1, (seconds + 59) / 60));
    if (seconds < 4 * 3600) return i18nc("a review card is due in some hours", "in %1 h", seconds / 3600);
    if (days <= 0) return i18nc("a review card is due today at a time", "today %1", time);
    if (days == 1) return i18nc("a review card is due tomorrow at a time", "tomorrow %1", time);
    return QLocale::c().toString(local.date(), local.date().year() == today.year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy"));
}

// How a card the review queue does not take stands against its due date: "Overdue 15 days",
// "Due now" (earlier today), "Due 29 Nov"; nothing for a card that was never due.
static QString scheduleDueState(const QDateTime &dueAt, const QDateTime &now)
{
    if (!dueAt.isValid()) return {};
    if (dueAt > now) return i18nc("a review card", "Due %1", scheduleDue(dueAt, now));
    const qint64 days = std::max<qint64>(0, dueAt.toLocalTime().date().daysTo(now.toLocalTime().date()));
    return days > 0 ? i18np("Overdue 1 day", "Overdue %1 days", days) : i18n("Due now");
}

// What the page column shows.
static QString feedbackPlace(const FeedbackSidebarEntry &entry)
{
    if (!entry.attachment.isEmpty()) return i18nc("feedback given in an HTML page, shown in place of a PDF page number", "HTML");
    return AnnotationModel::reviewCardPagesLabel(entry.page < 0 ? QList<int>() : QList<int>{entry.page}, false);
}

// A dot in the page highlight's colour with +, -, ? or ..., so they differ without colour too.
static QIcon feedbackIcon(const FeedbackSidebarEntry &entry)
{
    return Thoughts::FeedbackIcon::icon(!entry.kind.isEmpty() ? entry.kind : entry.liked ? QStringLiteral("like") : QStringLiteral("dislike"));
}

static QList<Okular::Page *> pagesForOpenDocument(const QPointer<Okular::Document> &document)
{
    QList<Okular::Page *> pages;
    if (!document || !document->isOpened()) {
        return pages;
    }

    pages.reserve(document->pages());
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        pages.append(const_cast<Okular::Page *>(document->page(pageNumber)));
    }
    return pages;
}

class AnnotationModelPrivate : public Okular::DocumentObserver
{
public:
    explicit AnnotationModelPrivate(AnnotationModel *qq);
    ~AnnotationModelPrivate() override;

    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;
    void notifyPageChanged(int page, int flags) override;

    QModelIndex indexForItem(AnnItem *item) const;
    void rebuildTree(const QList<Okular::Page *> &pages);
    void appendAgentProposalGroup();
    void refreshDetachedComments();
    AnnItem *findItem(int page, int *index) const;

    AnnotationModel *q;
    AnnItem *root;
    QPointer<Okular::Document> document;
    QList<AgentProposalSidebarEntry> agentProposals;
    QHash<QString, SourceCommentSidebarEntry> sourceComments;
    QMap<QString, QString> unplacedComments;
    QList<ReviewCardSidebarEntry> reviewCards;
    QHash<QString, QString> reviewCardStatuses;
    QStringList reviewCardPageLabels;
    QList<FeedbackSidebarEntry> feedback;
    QStringList feedbackPageLabels;
    bool agentProposalsVisible = true;
};

AnnItem::AnnItem()
    : parent(nullptr)
    , type(Type::Root)
    , annotation(nullptr)
    , page(-1)
{
}

AnnItem::AnnItem(AnnItem *_parent, Okular::Annotation *ann)
    : parent(_parent)
    , type(Type::Annotation)
    , annotation(ann)
    , page(_parent->page)
{
    Q_ASSERT(parent->type == Type::Page);
    parent->children.append(this);
}

AnnItem::AnnItem(AnnItem *_parent, int _page)
    : parent(_parent)
    , type(Type::Page)
    , annotation(nullptr)
    , page(_page)
{
    Q_ASSERT(parent->type == Type::Root);
    parent->children.append(this);
}

AnnItem::AnnItem(AnnItem *_parent, Type _type)
    : parent(_parent)
    , type(_type)
    , annotation(nullptr)
    , page(-1)
{
    Q_ASSERT((parent->type == Type::Root && (type == Type::AgentProposalGroup || type == Type::DetachedCommentGroup || type == Type::ReviewCardGroup || type == Type::FeedbackGroup))
             || (parent->type == Type::ReviewCardGroup && type == Type::ReviewCard) || (parent->type == Type::FeedbackGroup && type == Type::Feedback));
    parent->children.append(this);
}

AnnItem::AnnItem(AnnItem *_parent, const AgentProposalSidebarEntry &_proposal)
    : parent(_parent)
    , type(Type::AgentProposal)
    , annotation(nullptr)
    , page(-1)
    , proposal(_proposal)
{
    Q_ASSERT(parent->type == Type::AgentProposalGroup);
    parent->children.append(this);
}

AnnItem::~AnnItem()
{
    qDeleteAll(children);
}

AnnotationModelPrivate::AnnotationModelPrivate(AnnotationModel *qq)
    : q(qq)
    , root(new AnnItem)
{
}

AnnotationModelPrivate::~AnnotationModelPrivate()
{
    delete root;
}

static void updateAnnotationPointer(AnnItem *item, const QList<Okular::Page *> &pages)
{
    if (item->annotation) {
        item->annotation = pages[item->page]->annotation(item->annotation->uniqueName());
        if (!item->annotation) {
            qWarning() << "Lost annotation on document save, something went wrong";
        }
    }

    for (AnnItem *child : std::as_const(item->children)) {
        updateAnnotationPointer(child, pages);
    }
}

void AnnotationModelPrivate::notifySetup(const QList<Okular::Page *> &pages, int setupFlags)
{
    if (!(setupFlags & Okular::DocumentObserver::DocumentChanged)) {
        if (setupFlags & Okular::DocumentObserver::UrlChanged) {
            // Here with UrlChanged and no document changed it means we
            // need to update all the Annotation* otherwise
            // they still point to the old document ones, luckily the old ones are still
            // around so we can look for the new ones using unique ids, etc
            updateAnnotationPointer(root, pages);
        }
        return;
    }

    q->beginResetModel();
    qDeleteAll(root->children);
    root->children.clear();

    rebuildTree(pages);
    q->endResetModel();
    refreshDetachedComments();
}

void AnnotationModelPrivate::notifyPageChanged(int page, int flags)
{
    // we are strictly interested in annotations
    if (!(flags & Okular::DocumentObserver::Annotations)) {
        return;
    }
    const auto refreshDetached = qScopeGuard([this] { refreshDetachedComments(); });

    QList<Okular::Page *> pages;
    pages.reserve(document->pages());
    for (uint pageNumber = 0; pageNumber < document->pages(); ++pageNumber) {
        pages.append(const_cast<Okular::Page *>(document->page(pageNumber)));
    }
    const QList<Okular::Annotation *> annots =
        filterOutWidgetAndContinuationAnnotations(document->page(page)->annotations(), pages, page);
    int annItemIndex = -1;
    AnnItem *annItem = findItem(page, &annItemIndex);
    // case 1: the page has no more annotations
    //         => remove the branch, if any
    if (annots.isEmpty()) {
        if (annItem) {
            q->beginRemoveRows(indexForItem(root), annItemIndex, annItemIndex);
            delete root->children.at(annItemIndex);
            root->children.removeAt(annItemIndex);
            q->endRemoveRows();
        }
        return;
    }
    // case 2: no existing branch
    //         => add a new branch, and add the annotations for the page
    if (!annItem) {
        int i = 0;
        while (i < root->children.count() && root->children.at(i)->page < page) {
            ++i;
        }

        AnnItem *newAnnItem = new AnnItem();
        newAnnItem->type = AnnItem::Type::Page;
        newAnnItem->page = page;
        newAnnItem->parent = root;
        q->beginInsertRows(indexForItem(root), i, i);
        newAnnItem->parent->children.insert(i, newAnnItem);
        for (Okular::Annotation *annot : annots) {
            new AnnItem(newAnnItem, annot);
        }
        q->endInsertRows();
        return;
    }
    // case 3: existing branch, less annotations than items
    //         => lookup and remove the annotations
    if (annItem->children.count() > annots.count()) {
        for (int i = annItem->children.count(); i > 0; --i) {
            const Okular::Annotation *ref = annItem->children.at(i - 1)->annotation;
            bool found = false;
            for (const Okular::Annotation *annot : annots) {
                if (annot == ref) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                q->beginRemoveRows(indexForItem(annItem), i - 1, i - 1);
                delete annItem->children.at(i - 1);
                annItem->children.removeAt(i - 1);
                q->endRemoveRows();
            }
        }
        return;
    }
    // case 4: existing branch, less items than annotations
    //         => lookup and add annotations if not in the branch
    if (annots.count() > annItem->children.count()) {
        for (Okular::Annotation *ref : annots) {
            bool found = false;
            int count = annItem->children.count();
            for (int i = 0; !found && i < count; ++i) {
                if (ref == annItem->children.at(i)->annotation) {
                    found = true;
                }
            }
            if (!found) {
                q->beginInsertRows(indexForItem(annItem), count, count);
                new AnnItem(annItem, ref);
                q->endInsertRows();
            }
        }
        return;
    }
    // case 5: the data of some annotation changed
    // TODO: what do we do in this case?
    // FIXME: for now, update ALL the annotations for that page
    for (int i = 0; i < annItem->children.count(); ++i) {
        QModelIndex index = indexForItem(annItem->children.at(i));
        Q_EMIT q->dataChanged(index, index);
    }
}

QModelIndex AnnotationModelPrivate::indexForItem(AnnItem *item) const
{
    if (item->parent) {
        int id = item->parent->children.indexOf(item);
        if (id >= 0 && id < item->parent->children.count()) {
            return q->createIndex(id, 0, item);
        }
    }
    return QModelIndex();
}

void AnnotationModelPrivate::rebuildTree(const QList<Okular::Page *> &pages)
{
    for (int i = 0; i < pages.count(); ++i) {
        const QList<Okular::Annotation *> annots =
            filterOutWidgetAndContinuationAnnotations(pages.at(i)->annotations(), pages, i);
        if (annots.isEmpty()) {
            continue;
        }

        AnnItem *annItem = new AnnItem(root, i);
        for (Okular::Annotation *annot : annots) {
            new AnnItem(annItem, annot);
        }
    }
    appendAgentProposalGroup();
    if (!reviewCards.isEmpty()) {
        auto *group = new AnnItem(root, AnnItem::Type::ReviewCardGroup);
        for (const auto &card : std::as_const(reviewCards)) {
            auto *item = new AnnItem(group, AnnItem::Type::ReviewCard);
            item->card = card;
        }
    }
    if (!feedback.isEmpty()) {
        auto *group = new AnnItem(root, AnnItem::Type::FeedbackGroup);
        for (const auto &entry : std::as_const(feedback)) {
            auto *item = new AnnItem(group, AnnItem::Type::Feedback);
            item->feedback = entry;
        }
    }
}

void AnnotationModelPrivate::appendAgentProposalGroup()
{
    if (!agentProposalsVisible || agentProposals.isEmpty()) {
        return;
    }
    AnnItem *group = new AnnItem(root, AnnItem::Type::AgentProposalGroup);
    for (const AgentProposalSidebarEntry &proposal : std::as_const(agentProposals)) {
        new AnnItem(group, proposal);
    }
}

void AnnotationModelPrivate::refreshDetachedComments()
{
    QSet<QString> represented;
    AnnItem *group = nullptr;
    QList<DetachedCommentSidebarEntry> previous;
    for (AnnItem *parent : std::as_const(root->children)) {
        if (parent->type == AnnItem::Type::DetachedCommentGroup) {
            group = parent;
            for (AnnItem *child : std::as_const(parent->children)) previous.append(child->comment);
        } else {
            for (AnnItem *child : std::as_const(parent->children)) {
                if (child->annotation) represented.insert(Thoughts::logicalCommentId(child->annotation->uniqueName()));
            }
        }
    }
    QList<DetachedCommentSidebarEntry> entries;
    for (auto it = unplacedComments.cbegin(); it != unplacedComments.cend(); ++it) {
        if (!represented.contains(it.key())) {
            entries.append({it.key(), it.value(), {}});
            represented.insert(it.key());
        }
    }
    if (entries == previous) return;
    if (group) {
        const int row = root->children.indexOf(group);
        q->beginRemoveRows({}, row, row);
        root->children.removeAt(row);
        delete group;
        q->endRemoveRows();
    }
    if (!entries.isEmpty()) {
        const int row = root->children.size();
        q->beginInsertRows({}, row, row);
        group = new AnnItem(root, AnnItem::Type::DetachedCommentGroup);
        for (const auto &entry : std::as_const(entries)) {
            auto *item = new AnnItem();
            item->parent = group;
            item->type = AnnItem::Type::DetachedComment;
            item->comment = entry;
            group->children.append(item);
        }
        q->endInsertRows();
    }
}

AnnItem *AnnotationModelPrivate::findItem(int page, int *index) const
{
    for (int i = 0; i < root->children.count(); ++i) {
        AnnItem *tmp = root->children.at(i);
        if (tmp->type == AnnItem::Type::Page && tmp->page == page) {
            if (index) {
                *index = i;
            }
            return tmp;
        }
    }
    if (index) {
        *index = -1;
    }
    return nullptr;
}

AnnotationModel::AnnotationModel(Okular::Document *document, QObject *parent)
    : QAbstractItemModel(parent)
    , d(new AnnotationModelPrivate(this))
{
    d->document = document;

    d->document->addObserver(d);
}

AnnotationModel::~AnnotationModel()
{
    if (d->document) {
        d->document->removeObserver(d);
    }

    delete d;
}

int AnnotationModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

QVariant AnnotationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    AnnItem *item = static_cast<AnnItem *>(index.internalPointer());
    if (role == ThreadRootRole && item->annotation) {
        auto *root = item->annotation;
        QSet<Okular::Annotation *> visited;
        while (root->previousAnnotation()) {
            if (visited.contains(root)) return {};
            visited.insert(root);
            auto *previous = root->previousAnnotation();
            bool present = false;
            for (const auto *page : std::as_const(d->root->children)) {
                for (const auto *child : std::as_const(page->children)) {
                    if (child->annotation == previous) present = true;
                }
            }
            if (!present) return {};
            root = previous;
        }
        return Thoughts::logicalCommentId(root->uniqueName());
    }
    if (role == CommentCreatedRole && item->annotation) return item->annotation->creationDate();
    if (role == ItemTypeRole) {
        if (item->type == AnnItem::Type::Annotation) return Comment;
        if (item->type == AnnItem::Type::AgentProposal) return AgentProposal;
        if (item->type == AnnItem::Type::ReviewCard) return ReviewCard;
        // A comment follows its text and lists with the comments; the rest belongs to its version.
        if (item->type == AnnItem::Type::Feedback) return item->feedback.kind == Thoughts::DocumentFeedback::other() ? Comment : Feedback;
        if (item->type == AnnItem::Type::DetachedComment) return Comment;
        return {};
    }
    if (role == ItemIdRole) {
        if (item->annotation) return Thoughts::logicalCommentId(item->annotation->uniqueName());
        if (item->type == AnnItem::Type::AgentProposal) return item->proposal.id;
        if (item->type == AnnItem::Type::ReviewCard) return item->card.id;
        if (item->type == AnnItem::Type::Feedback) return item->feedback.id;
        if (item->type == AnnItem::Type::DetachedComment) return item->comment.id;
        return {};
    }
    if (role == DocumentFeedbackRole) return item->type == AnnItem::Type::Feedback;
    if (role == UnplacedRole) {
        if (item->type == AnnItem::Type::ReviewCard) return item->card.page < 0;
        if (item->type == AnnItem::Type::DetachedComment) return true;
        return item->annotation && d->unplacedComments.contains(Thoughts::logicalCommentId(item->annotation->uniqueName()));
    }
    if (item->type == AnnItem::Type::ReviewCardGroup) {
        if (role == Qt::DisplayRole) return i18n("Review cards");
        if (role == PageRole) return -1;
        return {};
    }
    if (item->type == AnnItem::Type::ReviewCard) {
        if (role == Qt::DisplayRole) return item->card.question;
        if (role == Qt::DecorationRole) return QIcon::fromTheme(QStringLiteral("view-card"), QIcon::fromTheme(QStringLiteral("view-list-details")));
        // Worded against the clock when asked, so "Due in 2 hours" turns into "Due now" by itself.
        if (role == ReviewCardScheduleRole || role == ReviewCardScheduleTagsRole || role == ReviewCardSchedulePartsRole || role == ReviewCardDueRole
            || role == ReviewCardDueOrderRole || role == Qt::ToolTipRole) {
            const ReviewCardScheduleText schedule = reviewCardSchedule(item->card.schedule, QDateTime::currentDateTimeUtc());
            if (role == ReviewCardScheduleRole) return schedule.line;
            if (role == ReviewCardScheduleTagsRole) return schedule.tags;
            if (role == ReviewCardSchedulePartsRole) return schedule.tags.isEmpty() ? schedule.parts : schedule.parts + QStringList{schedule.tags};
            if (role == ReviewCardDueRole) return schedule.due;
            if (role == ReviewCardDueOrderRole) return QVariantList{schedule.dueGroup, schedule.dueOrderAt.isValid() ? schedule.dueOrderAt.toMSecsSinceEpoch() : qint64(0)};
            const QString details = schedule.details.isEmpty() ? QString() : schedule.details.toHtmlEscaped() + QLatin1Char('\n');
            if (item->card.retired && item->card.page < 0)
                return i18n("Review card: %1\n%2It stays on the PDF version it was on when it was retired (%3): open that version from its History to see it there.",
                            item->card.question.toHtmlEscaped(), details, item->card.pdfSha256.left(12));
            return item->card.page < 0
                ? i18n("Review card: %1\n%2The answer’s PDF location needs updating.", item->card.question.toHtmlEscaped(), details)
                : i18n("Review card: %1\n%2Select to highlight the answer in the PDF. Double-click or press Enter to jump to its location.", item->card.question.toHtmlEscaped(), details);
        }
        if (role == PageRole) return item->card.page;
        if (role == PagesRole) { QVariantList pages; for (int page : item->card.pages) pages.append(page); return pages; }
        if (role == ReviewCardStatusRole) {
            // A retired card is never placed again: it stays on its own version, which is not this one.
            if (item->card.retired && item->card.page < 0) return i18n("On the PDF version it was retired on");
            return item->card.page < 0 ? i18n("PDF location needs updating") : d->reviewCardStatuses.value(item->card.id);
        }
        if (role == CandidateRole) return item->card.candidate;
        if (role == ReviewCardPagesLabelRole) return item->card.page < 0 ? QString() : AnnotationModel::reviewCardPagesLabel(item->card.pages, item->card.wholeDocument);
        if (role == ReviewCardPagesLabelsRole) return d->reviewCardPageLabels;
        return {};
    }
    if (item->type == AnnItem::Type::FeedbackGroup) {
        if (role == Qt::DisplayRole) return i18n("Comments");
        if (role == PageRole) return -1;
        return {};
    }
    if (item->type == AnnItem::Type::Feedback) {
        const FeedbackSidebarEntry &entry = item->feedback;
        if (role == Qt::DisplayRole) return entry.text;
        if (role == Qt::DecorationRole) return feedbackIcon(entry);
        if (role == Qt::ToolTipRole) {
            const QString text = entry.passage.isEmpty() ? entry.text : i18n("%1\n%2", entry.text, entry.passage);
            const QString what = entry.kind == Thoughts::DocumentFeedback::question() ? i18n("Question")
                : entry.kind == Thoughts::DocumentFeedback::drawing() ? i18n("Drawing") : entry.kind == Thoughts::DocumentFeedback::textBox() ? i18n("Text box")
                : !entry.kind.isEmpty() ? i18n("Comment")
                : entry.liked ? i18n("Liked") : i18n("Disliked");
            return i18n("%1: %2\nClick to open it.", what, text.toHtmlEscaped());
        }
        if (role == PageRole) return entry.page;
        // Drawn like a review card: the page left of the text, the passage under a comment.
        if (role == ReviewCardStatusRole) {
            QStringList lines;
            if (!entry.passage.isEmpty()) lines.append(entry.passage);
            if (!entry.attachment.isEmpty()) lines.append(entry.attachment);
            return lines.join(QStringLiteral(" · "));
        }
        if (role == ReviewCardPagesLabelRole) return feedbackPlace(entry);
        if (role == ReviewCardPagesLabelsRole) return d->feedbackPageLabels;
        return {};
    }
    if (item->type == AnnItem::Type::DetachedCommentGroup) {
        if (role == Qt::DisplayRole) return i18n("No page");
        if (role == PageRole) return -1;
        return {};
    }
    if (item->type == AnnItem::Type::DetachedComment) {
        const auto &comment = item->comment;
        if (role == Qt::DisplayRole) return comment.contents.simplified().isEmpty() ? i18n("Empty comment") : comment.contents.simplified();
        if (role == Qt::DecorationRole) return Thoughts::standaloneCommentIcon();
        if (role == Qt::ToolTipRole) return i18n("%1\n\nPDF location unresolved. Open to view the comment and its source.", comment.contents.toHtmlEscaped());
        if (role == AuthorRole) return comment.author;
        if (role == PageRole) return -1;
        return {};
    }
    if (item->type == AnnItem::Type::AgentProposalGroup) {
        if (role == Qt::DisplayRole) {
            return i18n("Agent edits");
        }
        if (role == Qt::DecorationRole) {
            return QIcon::fromTheme(QStringLiteral("document-edit"));
        }
        if (role == PageRole) {
            return -1;
        }
        return QVariant();
    }
    if (item->type == AnnItem::Type::AgentProposal) {
        if (role == Qt::DisplayRole) {
            return item->proposal.motivation.isEmpty() ? i18n("Agent edit") : item->proposal.motivation.simplified();
        }
        if (role == Qt::DecorationRole) {
            return QIcon::fromTheme(QStringLiteral("document-edit"));
        }
        if (role == Qt::ToolTipRole) {
            const QString newVersion = item->proposal.after.isEmpty() ? i18n("[delete]") : item->proposal.after;
            const QString oldVersion = item->proposal.before.isEmpty() ? i18n("[empty]") : item->proposal.before;
            return i18n("New version: %1\n\nOld version: %2\n\nMotivation: %3", newVersion, oldVersion, item->proposal.motivation);
        }
        if (role == AuthorRole) {
            return i18n("Agent");
        }
        if (role == PageRole) {
            return -1;
        }
        return QVariant();
    }
    if (!d->document || !d->document->isOpened() || item->page < 0 || static_cast<uint>(item->page) >= d->document->pages()) {
        return QVariant();
    }
    if (!item->annotation) {
        if (role == Qt::DisplayRole) {
            auto *page = d->document->page(item->page);
            if (page && !page->label().isEmpty() && page->label().toInt() != item->page + 1) {
                return i18nc("Page label (number)", "Page %1 (%2)", page->label(), item->page + 1);
            } else {
                return i18n("Page %1", item->page + 1);
            }
        } else if (role == Qt::DecorationRole) {
            return QIcon::fromTheme(QStringLiteral("text-plain"));
        } else if (role == PageRole) {
            return item->page;
        }

        return QVariant();
    }
    const Okular::Page *page = d->document->page(item->page);
    if (!page || !page->annotations().contains(item->annotation)) {
        return QVariant();
    }
    switch (role) {
    case Qt::DisplayRole: {
        const auto unresolved = d->unplacedComments.constFind(Thoughts::logicalCommentId(item->annotation->uniqueName()));
        if (unresolved != d->unplacedComments.cend()) return unresolved->simplified().isEmpty() ? i18n("Empty comment") : unresolved->simplified();
        const QString contents = item->annotation->contents().simplified();
        return contents.isEmpty() ? GuiUtils::captionForAnnotation(item->annotation) : contents;
        break;
    }
    case Qt::DecorationRole: {
        const GuiUtils::AnnotationInfo info = GuiUtils::getAnnotationInfo(item->annotation);
        // Annotation colors belong on the PDF. Tiny sidebar glyphs need the
        // theme's foreground contrast, especially for yellow markup.
        return GuiUtils::iconForAnnotationInfo(info);
    } break;
    case Qt::ToolTipRole: {
        if (d->unplacedComments.contains(Thoughts::logicalCommentId(item->annotation->uniqueName()))) {
            return i18n("%1\n\nPDF location unresolved. Open to view the comment and its source.", data(index, Qt::DisplayRole).toString().toHtmlEscaped());
        }
        const auto sourceComment = d->sourceComments.constFind(Thoughts::logicalCommentId(item->annotation->uniqueName()));
        if (sourceComment != d->sourceComments.cend()) {
            const QString source = sourceComment->sourceLine > 0 ? i18n("Source: %1:%2", sourceComment->sourcePath, sourceComment->sourceLine) : i18n("Source: %1", sourceComment->sourcePath);
            return i18n("%1\n%2", GuiUtils::prettyToolTip(item->annotation), source);
        }
        return GuiUtils::prettyToolTip(item->annotation);
    }
    case AuthorRole:
        return item->annotation->author();
        break;
    case PageRole:
        return item->page;
        break;
    }
    return QVariant();
}

bool AnnotationModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return true;
    }

    AnnItem *item = static_cast<AnnItem *>(parent.internalPointer());
    return !item->children.isEmpty();
}

QVariant AnnotationModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) {
        return QVariant();
    }

    if (section == 0 && role == Qt::DisplayRole) {
        return QString::fromLocal8Bit("Annotations");
    }

    return QVariant();
}

QModelIndex AnnotationModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column != 0) {
        return QModelIndex();
    }

    AnnItem *item = parent.isValid() ? static_cast<AnnItem *>(parent.internalPointer()) : d->root;
    if (row < item->children.count()) {
        return createIndex(row, column, item->children.at(row));
    }

    return QModelIndex();
}

QModelIndex AnnotationModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    AnnItem *item = static_cast<AnnItem *>(index.internalPointer());
    return d->indexForItem(item->parent);
}

int AnnotationModel::rowCount(const QModelIndex &parent) const
{
    AnnItem *item = parent.isValid() ? static_cast<AnnItem *>(parent.internalPointer()) : d->root;
    return item->children.count();
}

bool AnnotationModel::isAnnotation(const QModelIndex &index) const
{
    return annotationForIndex(index);
}

Okular::Annotation *AnnotationModel::annotationForIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return nullptr;
    }

    const AnnItem *item = static_cast<AnnItem *>(index.internalPointer());
    return item->annotation;
}

QModelIndex AnnotationModel::indexForAnnotation(const Okular::Annotation *annotation, int pageNumber) const
{
    if (!annotation) {
        return QModelIndex();
    }

    const Thoughts::CommentFragmentGroup group =
        Thoughts::commentFragmentGroup(d->document, const_cast<Okular::Annotation *>(annotation), pageNumber);
    const Thoughts::CommentFragment root = group.root();
    if (root.annotation) {
        annotation = root.annotation;
        pageNumber = root.page;
    }

    AnnItem *pageItem = d->findItem(pageNumber, nullptr);
    if (!pageItem) {
        return QModelIndex();
    }

    const QString uniqueName = annotation->uniqueName();
    for (AnnItem *item : std::as_const(pageItem->children)) {
        if (item->annotation == annotation || (!uniqueName.isEmpty() && item->annotation && item->annotation->uniqueName() == uniqueName)) {
            return d->indexForItem(item);
        }
    }

    return QModelIndex();
}

bool AnnotationModel::isAgentProposal(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return false;
    }
    const AnnItem *item = static_cast<const AnnItem *>(index.internalPointer());
    return item && item->type == AnnItem::Type::AgentProposal;
}

AgentProposalSidebarEntry AnnotationModel::agentProposalForIndex(const QModelIndex &index) const
{
    if (!isAgentProposal(index)) {
        return {};
    }
    return static_cast<const AnnItem *>(index.internalPointer())->proposal;
}

AgentProposalSidebarEntry AnnotationModel::agentProposal(const QString &proposalId) const
{
    const auto found = std::find_if(d->agentProposals.cbegin(), d->agentProposals.cend(), [&proposalId](const AgentProposalSidebarEntry &proposal) { return proposal.id == proposalId; });
    return found == d->agentProposals.cend() ? AgentProposalSidebarEntry {} : *found;
}

QList<AgentProposalSidebarEntry> AnnotationModel::agentProposals() const
{
    return d->agentProposals;
}

void AnnotationModel::setAgentProposals(const QList<AgentProposalSidebarEntry> &proposals)
{
    d->agentProposals = proposals;
    beginResetModel();
    qDeleteAll(d->root->children);
    d->root->children.clear();
    d->rebuildTree(pagesForOpenDocument(d->document));
    endResetModel();
    d->refreshDetachedComments();
}

void AnnotationModel::removeAgentProposal(const QString &proposalId)
{
    QList<AgentProposalSidebarEntry> proposals = d->agentProposals;
    proposals.removeIf([&proposalId](const AgentProposalSidebarEntry &proposal) { return proposal.id == proposalId; });
    if (proposals.size() != d->agentProposals.size()) {
        setAgentProposals(proposals);
    }
}

void AnnotationModel::setAgentProposalsVisible(bool visible)
{
    if (d->agentProposalsVisible == visible) {
        return;
    }
    d->agentProposalsVisible = visible;
    setAgentProposals(d->agentProposals);
}

void AnnotationModel::setSourceComments(const QList<SourceCommentSidebarEntry> &comments)
{
    d->sourceComments.clear();
    for (const SourceCommentSidebarEntry &comment : comments) {
        if (!comment.id.isEmpty()) {
            d->sourceComments.insert(comment.id, comment);
        }
    }
    beginResetModel();
    qDeleteAll(d->root->children);
    d->root->children.clear();
    d->rebuildTree(pagesForOpenDocument(d->document));
    endResetModel();
    d->refreshDetachedComments();
}

void AnnotationModel::setUnplacedComments(const QMap<QString, QString> &comments)
{
    if (d->unplacedComments == comments) return;
    d->unplacedComments = comments;
    for (AnnItem *parent : std::as_const(d->root->children)) {
        if (parent->type == AnnItem::Type::Page && !parent->children.isEmpty()) {
            Q_EMIT dataChanged(d->indexForItem(parent->children.first()), d->indexForItem(parent->children.last()));
        }
    }
    d->refreshDetachedComments();
}

void AnnotationModel::setReviewCards(const QList<ReviewCardSidebarEntry> &cards)
{
    if (d->reviewCards == cards) return;
    d->reviewCards = cards;
    d->reviewCardStatuses.clear();
    d->reviewCardPageLabels.clear();
    for (const auto &card : cards) {
        const QString label = card.page < 0 ? QString() : reviewCardPagesLabel(card.pages, card.wholeDocument);
        if (!label.isEmpty() && !d->reviewCardPageLabels.contains(label)) d->reviewCardPageLabels.append(label);
    }
    beginResetModel();
    qDeleteAll(d->root->children);
    d->root->children.clear();
    d->rebuildTree(pagesForOpenDocument(d->document));
    endResetModel();
    d->refreshDetachedComments();
}

void AnnotationModel::setFeedback(const QList<FeedbackSidebarEntry> &feedback)
{
    if (d->feedback == feedback) return;
    d->feedback = feedback;
    d->feedbackPageLabels.clear();
    for (const auto &entry : feedback) {
        const QString label = feedbackPlace(entry);
        if (!label.isEmpty() && !d->feedbackPageLabels.contains(label)) d->feedbackPageLabels.append(label);
    }
    beginResetModel();
    qDeleteAll(d->root->children);
    d->root->children.clear();
    d->rebuildTree(pagesForOpenDocument(d->document));
    endResetModel();
    d->refreshDetachedComments();
}

QModelIndex AnnotationModel::indexForFeedback(const QString &feedbackId) const
{
    for (auto *group : std::as_const(d->root->children)) {
        for (auto *item : std::as_const(group->children)) {
            if (item->type == AnnItem::Type::Feedback && item->feedback.id == feedbackId) return d->indexForItem(item);
        }
    }
    return {};
}

QString AnnotationModel::reviewCardPagesLabel(const QList<int> &pages, bool wholeDocument)
{
    if (pages.isEmpty()) return {};
    if (wholeDocument) return i18nc("review card answer covers the whole document", "all");
    QList<int> sorted = pages;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.size() == 1) return i18nc("page of a review card answer", "p. %1", QString::number(sorted.first() + 1));
    // Consecutive pages collapse into a range: 2, 3, 4, 7 -> "2–4, 7".
    QStringList parts;
    for (qsizetype start = 0; start < sorted.size();) {
        qsizetype end = start;
        while (end + 1 < sorted.size() && sorted.at(end + 1) == sorted.at(end) + 1) ++end;
        parts.append(end == start ? QString::number(sorted.at(start) + 1) : QStringLiteral("%1\u2013%2").arg(sorted.at(start) + 1).arg(sorted.at(end) + 1));
        start = end + 1;
    }
    return i18nc("pages of a review card answer", "pp. %1", parts.join(QStringLiteral(", ")));
}

ReviewCardScheduleText AnnotationModel::reviewCardSchedule(const QJsonObject &schedule, const QDateTime &now)
{
    ReviewCardScheduleText text;
    if (schedule.isEmpty()) return text;
    const QDateTime utcNow = now.toUTC();
    const QString state = schedule.value(QStringLiteral("state")).toString();
    const QDateTime dueAt = scheduleTime(schedule, QStringLiteral("due_at"));
    const QDateTime dueFrom = scheduleTime(schedule, QStringLiteral("due_from"));
    const QDateTime skippedUntil = scheduleTime(schedule, QStringLiteral("skipped_until"));
    const QDateTime lastReviewedAt = scheduleTime(schedule, QStringLiteral("last_reviewed_at"));
    const QString interval = schedule.value(QStringLiteral("interval")).toString();
    const int reviews = schedule.value(QStringLiteral("reviews")).toInt();
    QStringList tags;
    for (const QJsonValue &tag : schedule.value(QStringLiteral("tags")).toArray()) tags.append(tag.toString());
    const bool isNew = state == QLatin1String("new");
    // Only a card the review queue takes has a due_from: from then on it is due (ReviewStore::isDueNow).
    const bool queued = (isNew || state == QLatin1String("learning") || state == QLatin1String("review")) && dueFrom.isValid();
    text.due = queued && dueFrom <= utcNow;
    const bool skipped = queued && !text.due && skippedUntil.isValid() && skippedUntil > utcNow;
    // Overdue days are local calendar days since the later of its due date and a skip
    // (the knowledge profile's count); never-answered cards are just new.
    const QDateTime counted = skippedUntil.isValid() && skippedUntil > dueAt ? skippedUntil : dueAt;
    const qint64 overdueDays = text.due && !isNew && counted.isValid() ? std::max<qint64>(0, counted.toLocalTime().date().daysTo(utcNow.toLocalTime().date())) : 0;

    QStringList line;
    QStringList details;
    if (state == QLatin1String("retired")) {
        const QDateTime retiredAt = scheduleTime(schedule, QStringLiteral("retired_at"));
        line.append(i18n("Retired"));
        details.append(retiredAt.isValid() ? i18n("Retired %1: not reviewed; its history and schedule are kept.", scheduleWhen(retiredAt))
                                           : i18n("Retired: not reviewed; its history and schedule are kept."));
    } else if (state == QLatin1String("never")) {
        line.append(i18n("Never due"));
        details.append(i18n("Due: never (interval Never)"));
    } else if (state == QLatin1String("unpublished") || !queued) {
        // When it is due, and why it is not offered.
        const QString when = scheduleDueState(dueAt, utcNow);
        const bool placement = schedule.value(QStringLiteral("not_offered")).toString() == QLatin1String("placement");
        const bool source = schedule.value(QStringLiteral("not_offered")).toString() == QLatin1String("source");
        if (state == QLatin1String("unpublished")) {
            line.append(when.isEmpty() ? i18n("Not offered: document unpublished") : when);
            if (!when.isEmpty()) line.append(i18nc("why a review card is not offered", "not offered: document unpublished"));
            details.append(i18n("Not reviewed: its document is unpublished in its Project. Its schedule is kept."));
        } else if (placement) {
            line.append(when.isEmpty() ? (isNew ? i18n("New") : i18n("Not offered")) : when);
            line.append(i18nc("why a review card is not offered", "waiting for its place in the PDF"));
            details.append(i18n("Not offered for review until its answer is placed in this version of the PDF."));
        } else if (source) {
            line.append(when.isEmpty() ? (isNew ? i18n("New") : i18n("Not offered")) : when);
            line.append(i18nc("why a review card is not offered", "its passage is not found in the source"));
            details.append(i18n("Not offered for review until its marked passage is found in the source again."));
        } else {
            line.append(when.isEmpty() ? (isNew ? i18n("New") : i18n("Not offered")) : when);
            line.append(i18nc("why a review card is not offered", "not offered now"));
            details.append(i18n("Not offered for review now."));
        }
        if (dueAt.isValid()) details.append(i18n("Due date: %1", scheduleWhen(dueAt)));
    } else if (skipped) {
        line.append(i18n("Skipped until %1", scheduleUntil(skippedUntil, utcNow)));
        if (dueAt.isValid()) details.append(i18n("Due: %1", scheduleWhen(dueAt)));
        details.append(i18n("Skipped until: %1", scheduleWhen(skippedUntil)));
    } else if (text.due) {
        line.append(isNew ? i18n("New") : overdueDays > 0 ? i18np("Overdue 1 day", "Overdue %1 days", overdueDays) : i18n("Due now"));
        details.append(!dueAt.isValid() ? i18n("Due: now")
                       : overdueDays > 0 ? i18np("Due: %2 (overdue 1 day)", "Due: %2 (overdue %1 days)", overdueDays, scheduleWhen(dueAt))
                                         : i18n("Due: %1 (due now)", scheduleWhen(dueAt)));
    } else {
        const QString in = scheduleIn(dueFrom, utcNow);
        const QString due = scheduleDue(dueFrom, utcNow);
        line.append(isNew ? i18nc("a new review card, first due", "New · due %1", due) : i18nc("a review card", "Due %1", due));
        // "Due 28 Nov · in 2 weeks": how long until then, when the date alone does not say it
        // (later today and tomorrow already do).
        if (utcNow.toLocalTime().date().daysTo(dueFrom.toLocalTime().date()) >= 2) line.append(in);
        details.append(i18n("Due: %1 (%2)", scheduleWhen(dueAt.isValid() ? dueAt : dueFrom), in));
    }
    if (isNew) details.append(i18n("New: not answered yet"));
    // The interval ("every 8 weeks") is in the details: on the line it read as when the card comes due.
    if (queued && state == QLatin1String("learning")) line.append(i18nc("a review card in its learning steps", "learning"));
    if (reviews > 0) line.append(i18np("1 review", "%1 reviews", reviews));
    if (!interval.isEmpty()) details.append(i18n("Interval: %1", interval));
    details.append(i18n("Reviews: %1 · Lapses: %2", reviews, schedule.value(QStringLiteral("lapses")).toInt()));
    if (lastReviewedAt.isValid()) details.append(i18n("Last reviewed: %1", scheduleWhen(lastReviewedAt)));
    if (!tags.isEmpty()) {
        text.tags = i18nc("the tags of a review card's last answer", "last: %1", tags.join(QStringLiteral(", ")));
        details.append(i18n("Last tags: %1", tags.join(QStringLiteral(", "))));
    } else if (schedule.value(QStringLiteral("answered")).toBool()) {
        details.append(i18n("Last tags: none"));
    }
    text.parts = line;
    text.line = line.join(QStringLiteral(" · "));
    text.details = details.join(QLatin1Char('\n'));
    // Due first, in the review queue's order (ReviewStore::dueCards), then the next to come due.
    if (text.due) {
        text.dueGroup = isNew ? 1 : dueAt.isValid() && dueAt > utcNow ? 2 : 0;
        text.dueOrderAt = dueAt.isValid() ? dueAt : QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC);
    } else if (queued) {
        text.dueGroup = 3;
        text.dueOrderAt = dueFrom;
    }
    return text;
}

void AnnotationModel::setReviewCardStatus(const QString &id, const QString &status)
{
    if (d->reviewCardStatuses.value(id) == status) return;
    if (status.isEmpty()) d->reviewCardStatuses.remove(id);
    else d->reviewCardStatuses.insert(id, status);
    for (auto *group : std::as_const(d->root->children)) {
        for (auto *item : std::as_const(group->children)) {
            if (item->type == AnnItem::Type::ReviewCard && item->card.id == id) {
                const QModelIndex index = d->indexForItem(item);
                Q_EMIT dataChanged(index, index, {ReviewCardStatusRole, Qt::SizeHintRole});
                return;
            }
        }
    }
}

#include "moc_annotationmodel.cpp"
