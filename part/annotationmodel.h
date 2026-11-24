/*
    SPDX-FileCopyrightText: 2006 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef ANNOTATIONMODEL_H
#define ANNOTATIONMODEL_H

#include <QAbstractItemModel>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "okularpart_export.h"

namespace Okular
{
class Annotation;
class Document;
}

class AnnotationModelPrivate;

struct AgentProposalSidebarEntry
{
    QString id;
    QString motivation;
    QString conversationId;
    QString conversationUrl; // spec §3.6, empty when the proposal has no link
    QString before;
    QString after;
    QString kind;
    QString sourcePath;
    int sourceLine = 0;
    QString sourceSha256;
    QString bundleId;
    bool liveSource = false;
    bool localSourcePresent = false;
    QString resolutionError;
    // Its place in the text (sortAgentProposalRowsInReadingOrder): the list is in this order.
    QList<int> readingOrder;
};

struct SourceCommentSidebarEntry
{
    QString id;
    QString contents;
    QString sourcePath;
    int sourceLine = 0;
};

struct DetachedCommentSidebarEntry
{
    QString id;
    QString contents;
    QString author;

    bool operator==(const DetachedCommentSidebarEntry &) const = default;
};

struct ReviewCardSidebarEntry
{
    QString id;
    QString question;
    int page = -1;
    QList<int> pages;
    QByteArray contentHash;
    bool candidate = false;
    bool wholeDocument = false;
    QJsonObject schedule; // "sidebar_schedule" of its record (Thoughts::sidebarReviewSchedule in the shell).
    // Retired: it stays on the PDF version it was on when it was retired (pdfSha256), and is
    // drawn only there; on another version it is listed (the Retired filter) but not placed.
    bool retired = false;
    QString pdfSha256;
    // Placed on the newest version of the PDF, which is ready while the tab keeps an earlier one
    // on screen: listed, not drawn; choosing it shows that version.
    bool onNewestVersion = false;
    bool operator==(const ReviewCardSidebarEntry &) const = default;
};

// A review card's schedule as the Cards list words it at a given time (AnnotationModel::reviewCardSchedule).
struct ReviewCardScheduleText
{
    QString line; // Under the question: "Due in 3 days · every 2 weeks · 3 reviews", "Overdue 8 days", "New", "Retired".
    QStringList parts; // The line's parts, most important first; a narrow row shows the first ones.
    QString tags; // The last answer's tags, "last: Easy, Recalled": after the line where it fits, else only in the tooltip.
    QString details; // The whole schedule for the tooltip, one fact per line.
    bool due = false; // In the review queue now: due or overdue (ReviewStore::dueCards).
    // Due first: the due ones in the queue's order (overdue reviews oldest first, new cards, learning
    // steps taken early), then the next to come due, soonest first, then the cards not in review.
    int dueGroup = 4;
    QDateTime dueOrderAt;
};

// Document feedback (documentfeedback.h): a comment on a passage: liked, disliked, a question or other.
struct FeedbackSidebarEntry
{
    QString id;
    bool liked = true;
    QString text; // Its comment, or the passage when it has none.
    QString passage; // The passage under a comment; empty when text is the passage.
    QString attachment; // The HTML attachment it was made in, by name; such feedback has no page.
    int page = -1; // Zero-based: where it was made, which an earlier PDF version may place elsewhere.
    QString kind; // DocumentFeedback::question() or other(), or empty for a like or a dislike.
    bool operator==(const FeedbackSidebarEntry &) const = default;
};

class OKULARPART_EXPORT AnnotationModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum { AuthorRole = Qt::UserRole + 1000, PageRole, ItemTypeRole, ItemIdRole, UnplacedRole, ReviewCardStatusRole, PagesRole, CandidateRole, ThreadRootRole, ReplyCountRole, IsReplyRole, CommentCreatedRole,
           ReviewCardPagesLabelRole, ReviewCardPagesLabelsRole,
           // A review card's schedule now (reviewCardSchedule): its line, the tags that may follow
           // it, the parts of both in order (a row shows those that fit), whether it is due, and
           // its Due first order (a list: group, then time).
           ReviewCardScheduleRole, ReviewCardScheduleTagsRole, ReviewCardSchedulePartsRole, ReviewCardDueRole, ReviewCardDueOrderRole,
           // Whether the row is document feedback, whatever its type: a comment-kind feedback lists
           // with the comments (ItemTypeRole Comment) but opens, edits and deletes as feedback.
           DocumentFeedbackRole,
           // A review card's "sidebar_schedule" as a map (its state, review_kind, review_kind_source and
           // projects_kind, review-card-kinds.md §5).
           ReviewCardKindRole,
           // A review card placed on the newest version of the PDF while the tab keeps an earlier
           // one on screen (ReviewCardSidebarEntry::onNewestVersion).
           ReviewCardOnNewestVersionRole };
    // Notes are not annotations: they never appear in the Annotations list (value 2 stays unused).
    enum ItemType { Comment = 1, Feedback = 2, AgentProposal = 4, ReviewCard = 8 };

    explicit AnnotationModel(Okular::Document *document, QObject *parent = nullptr);
    ~AnnotationModel() override;

    // reimplementations from QAbstractItemModel
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    bool isAnnotation(const QModelIndex &index) const;
    Okular::Annotation *annotationForIndex(const QModelIndex &index) const;
    QModelIndex indexForAnnotation(const Okular::Annotation *annotation, int pageNumber) const;
    bool isAgentProposal(const QModelIndex &index) const;
    AgentProposalSidebarEntry agentProposalForIndex(const QModelIndex &index) const;
    AgentProposalSidebarEntry agentProposal(const QString &proposalId) const;
    QList<AgentProposalSidebarEntry> agentProposals() const;
    void setAgentProposals(const QList<AgentProposalSidebarEntry> &proposals);
    void removeAgentProposal(const QString &proposalId);
    void setAgentProposalsVisible(bool visible);
    void setSourceComments(const QList<SourceCommentSidebarEntry> &comments);
    void setUnplacedComments(const QMap<QString, QString> &comments);
    void setReviewCards(const QList<ReviewCardSidebarEntry> &cards);
    void setReviewCardStatus(const QString &id, const QString &status);
    void setFeedback(const QList<FeedbackSidebarEntry> &feedback);
    QModelIndex indexForFeedback(const QString &feedbackId) const;

    // Short name of the pages that hold a review card's answer, shown left of its question:
    // "p. 3", "pp. 3–4", "pp. 2, 5", "all" for a whole-document card, empty without pages.
    // pages are zero-based PDF page numbers.
    static QString reviewCardPagesLabel(const QList<int> &pages, bool wholeDocument);
    // A card's @p schedule ("sidebar_schedule") as the Cards list words it at @p now: due now and
    // overdue as the review queue counts them (from its due_from), "Overdue N days" by local
    // calendar days as the knowledge profile counts them, other times relative to @p now.
    static ReviewCardScheduleText reviewCardSchedule(const QJsonObject &schedule, const QDateTime &now);

private:
    // storage
    friend class AnnotationModelPrivate;
    AnnotationModelPrivate *const d;
};

#endif
