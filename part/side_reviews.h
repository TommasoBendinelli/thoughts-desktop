/*
    SPDX-FileCopyrightText: 2005 Enrico Ros <eros.kde@email.it>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _OKULAR_SIDE_REVIEWS_H_
#define _OKULAR_SIDE_REVIEWS_H_

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QModelIndexList>
#include <QSet>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include "core/observer.h"
#include "okularpart_export.h"

class QModelIndex;
template<typename T> class QFutureWatcher;
class QTimer;
class QPushButton;
class QJsonObject;

struct AnnotationSourceOrder;
struct AgentProposalSidebarEntry;

namespace Okular
{
class Annotation;
class Document;
class PartTest;
}

class AnnotationModel;
class AuthorGroupProxyModel;
class PageFilterProxyModel;
class PageGroupProxyModel;
class QLabel;
class KTreeViewSearchLine;
class TreeView;

/**
 * @short ...
 */
class OKULARPART_EXPORT Reviews : public QWidget, public Okular::DocumentObserver
{
    Q_OBJECT
public:
    enum class DocsSourceLinkState {
        NoDocument,
        NotLinked,
        NoSource, // a Library PDF without LaTeX sources (an imported paper), or sources still being looked up
        Linked,
    };
    Q_ENUM(DocsSourceLinkState)

    Reviews(QWidget *parent, Okular::Document *document, const QString &docsRoot = QString());
    ~Reviews() override;

    // [INHERITED] from DocumentObserver
    void notifySetup(const QList<Okular::Page *> &pages, int setupFlags) override;
    void notifyCurrentPageChanged(int previous, int current) override;

    void reparseConfig();
    void setCommentSidebarState(const QHash<QString, AnnotationSourceOrder> &state);
    void refreshCommentThreads();
    void setUnplacedComments(const QMap<QString, QString> &comments);
    void setReviewCards(const QJsonArray &cards);
    // The document feedback of the open PDF, deleted records included (they are left out).
    void setDocumentFeedback(const QList<QJsonObject> &records);
    // @p earlier of them (ids) are from earlier versions, which show with the panel's own line;
    // @p earlierCount says how many there are and @p showing whether they are shown.
    void setEarlierFeedback(const QSet<QString> &earlier, int earlierCount, bool showing);
    // Shows the row of @p feedbackId, without taking the focus from the page.
    void selectFeedback(const QString &feedbackId);
    void setDocsSourceLinkStatus(DocsSourceLinkState state, int sourceFileCount = 0, const QStringList &projects = {});
    bool selectAndOpenAnnotation(Okular::Annotation *annotation, int pageNumber);
    AgentProposalSidebarEntry agentProposal(const QString &proposalId) const;
    QString loadAgentProposalSource(const QString &proposalId, const QString &sourcePath);
    bool agentProposalRefreshPending() const;
    // Proposals of the open PDF that its local sources no longer contain.
    QSet<QString> decidedAgentProposalIds() const;
    void suppressAgentProposal(const QString &proposalId);
    // @p reload: read the sources again (they changed, e.g. Undo); a refused
    // decision changed nothing and shows the proposal as it was.
    void restoreAgentProposal(const QString &proposalId, bool reload = true);

    QAbstractItemModel *annotationsModel() const;
    bool selectReviewCard(const QString &id);

public Q_SLOTS:
    void editActiveReviewCard();
    void slotPageEnabled(bool);
    void slotAuthorEnabled(bool);
    void slotCurrentPageOnly(bool);
    void slotExpandAll();
    void slotCollapseAll();
    void selectAnnotation(Okular::Annotation *annotation, int pageNumber);

Q_SIGNALS:
    void openAnnotationWindow(Okular::Annotation *annotation, int pageNumber);
    void replyToAnnotation(Okular::Annotation *annotation, int pageNumber);
    void closeReviewAnnotationWindow();
    void openReviewAnnotationWindow(Okular::Annotation *annotation, int pageNumber);
    void reviewSelectionChanged(Okular::Annotation *annotation, int pageNumber);
    void agentProposalReviewRequested(const QString &proposalId);
    // Accept ("accept") or Reject ("disregard") from the list's context menu.
    void agentProposalsResolutionRequested(const QStringList &proposalIds, const QString &action);
    void agentProposalsChanged();
    // Single opening interface for comments. Placement travels with the request
    // as data (page + unplaced flag) instead of selecting a different gesture.
    void openCommentRequested(const QString &id, int page, bool unplaced);
    void commentsDeleteRequested(const QStringList &ids);
    void reviewCardHighlightsChanged(const QVariantList &cards);
    void reviewCardPresentationRequested(const QVariantList &regions, bool navigate);
    void clearReviewCardPresentationRequested();
    // The Cards list was left (Cards filter off, or the panel hidden): only the selected card's
    // highlight goes; a review's answer mask stays.
    void reviewCardHighlightHidden();
    void reviewCardEditRequested(const QString &id);
    void reviewCardOpenRequested(const QString &id);
    // Review now: this card alone in the Review window.
    void reviewCardReviewRequested(const QString &id);
    // Merge cards…: the review cards selected in the list (two or more), in list order.
    void reviewCardsMergeRequested(const QStringList &ids);
    // Make project card, Make knowledge card or Follow the Project: @p kind is knowledge, project
    // or follow_project (Shell::setSidebarReviewCardKind).
    void reviewCardKindRequested(const QString &id, const QString &kind);
    // Retire card / Delete card…: the right-clicked card, or every selected card when it is one of them
    // (Shell::retireSidebarReviewCards, Shell::deleteSidebarReviewCards).
    void reviewCardsRetireRequested(const QStringList &ids);
    void reviewCardsDeleteRequested(const QStringList &ids);
    // The user switched an Annotations type filter (AnnotationModel::ItemType) on
    // or off, so the matching page markup should be shown or hidden too.
    void typeFilterClicked(int itemType, bool shown);
    // A feedback row was clicked (@p edit false) or opened to change it (@p edit true).
    void feedbackRequested(const QString &feedbackId, bool edit);
    void feedbackDeleteRequested(const QString &feedbackId);
    void feedbackResolveRequested(const QString &feedbackId, bool resolved);
    void earlierFeedbackToggled(bool shown);

private Q_SLOTS:
    void currentChanged(const QModelIndex &);
    void deleteAnnotation(const QModelIndex &);
    void moveReview(bool forward);
    void openReview(const QModelIndex &);
    void contextMenuRequested(const QPoint);
    void agentProposalContextMenu(const QModelIndex &row, const QPoint pos);
    void saveSearchOptions();
    void saveTreeState();
    void restoreTreeState();
    void finishAgentProposalRefresh();

private:
    friend class Okular::PartTest;

protected:
    void hideEvent(QHideEvent *event) override;
    // Notes which card was highlighted when a click on the list began (see m_activeCardAtPress).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:

    QModelIndex annotationModelIndex(const QModelIndex &index) const;
    QModelIndex adjacentAnnotationIndex(const QModelIndex &index, bool forward) const;
    QModelIndexList retrieveAnnotations(const QModelIndex &idx) const;
    void refreshAgentProposals();
    void startAgentProposalRefresh();
    void activateAgentProposal(const QModelIndex &index);
    void openCommentWindow(const QModelIndex &index);
    bool activateReviewCard(const QModelIndex &index, bool navigate = false);
    void clearActiveReviewCard();
    void hideActiveReviewCardHighlight();
    void updateTypeFilter();
    void revealRow(const QModelIndex &viewIndex);
    // The Cards list and highlights from m_reviewCardRecords.
    void rebuildReviewCards();

    // data fields (GUI)
    QLabel *m_sourceLinkIndicator;
    KTreeViewSearchLine *m_searchLine = nullptr;
    TreeView *m_view;
    QPushButton *m_commentsFilter;
    QPushButton *m_feedbackFilter;
    QPushButton *m_earlierFeedback = nullptr;
    QSet<QString> m_earlierFeedbackIds;
    QHash<QString, QJsonObject> m_feedbackRecords;
    QPushButton *m_proposalsFilter;
    QPushButton *m_reviewCardsFilter;
    // Lists the document's retired cards, which stay on the PDF version they were retired on.
    QPushButton *m_retiredCardsFilter = nullptr;
    QHash<QString, QJsonObject> m_reviewCards;
    QJsonArray m_reviewCardRecords;
    QString m_reviewPdfSha256;
    QString m_activeReviewCardId;
    // The card highlighted when the last click on the list began: a click on that same card takes
    // its highlight off again (selecting it happens on the press, before the click arrives).
    QString m_activeCardAtPress;
    QStringList m_expansionKeys;
    QString m_docsRoot;
    DocsSourceLinkState m_docsSourceLinkState = DocsSourceLinkState::NoDocument;
    int m_docsSourceLinkFileCount = 0;
    QStringList m_docsSourceLinkProjects;
    QFutureWatcher<QVariant> *m_agentProposalWatcher = nullptr;
    QSet<QString> m_agentProposalSourcePaths;
    QTimer *m_agentProposalRefreshTimer = nullptr;
    QUrl m_agentProposalRequestedUrl;
    QUrl m_agentProposalRunningUrl;
    // Includes the interval between worker completion and applying its UI result.
    bool m_agentProposalRefreshInFlight = false;
    bool m_agentProposalRefreshPending = false;
    QSet<QString> m_decidedAgentProposalIds;
    int m_agentProposalTransientRetries = 0;
    QSet<QString> m_suppressedAgentProposalIds;
    QStringList m_additionalAgentProposalSources;
    QHash<QString, AgentProposalSidebarEntry> m_suppressedAgentProposals;
    // internal storage
    Okular::Document *m_document;
    AnnotationModel *m_model;
    AuthorGroupProxyModel *m_authorProxy;
    PageFilterProxyModel *m_filterProxy;
    PageGroupProxyModel *m_groupProxy;
};

#endif
