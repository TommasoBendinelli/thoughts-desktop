/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_REVIEWFEEDBACKPANEL_H
#define THOUGHTS_REVIEWFEEDBACKPANEL_H

#include <QDateTime>
#include <QFrame>
#include <QList>
#include <QPair>
#include <QRectF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QLabel;
class QScrollArea;
class QVBoxLayout;
class MainShellTest;

namespace Thoughts
{
// A document's History by PDF version (documentHistoryVersions, Sidebar › History), where "Show
// history" of a card under review also leads; one block per PDF version, newest first: the version
// shown now, earlier ones by the date of their first entry, and last the entries whose version was
// not recorded. A block lists the comments made on that version's passages (likes, dislikes,
// questions, comments and drawings), then the cards reviewed on it, each with its number of reviews
// and of feedback (the comments in its tooltip); a click opens the comment's passage or the card's
// answer in that version: in this tab for the version it shows, else in that version's tab. Nothing
// of a card whose answer is masked in a review opens or shows its comments.
class ReviewFeedbackPanel : public QFrame
{
    Q_OBJECT

    friend class ::MainShellTest;

public:
    // One entry of a version: a review answer (@p html, as the card's History words it) or answer
    // feedback (@p feedback: feedback_id, polarity, comment, quote, state as the reader finds it
    // now (exact, relocated, orphaned; empty when unknown or deleted), created_at, updated_at,
    // deleted, deleted_at, revision, anchor_pdf_sha256 and anchor_regions, where it was given;
    // card_id, the card it was made on, shared, true for another card's feedback found in this
    // card's answer, labelled "From another card · “<question>”", and home_question, that card's
    // question, also the label's tooltip).
    // In a document's History (documentHistoryVersions) each entry also names its card: a card's
    // entries follow one another, the first with @p cardAnswer, its answer in that version when
    // known. An entry with neither html nor feedback only names its card.
    struct Entry {
        QDateTime at;
        QString html;
        QVariantMap feedback;
        QString cardId;
        QString cardQuestion;
        QList<QPair<int, QRectF>> cardAnswer;
        bool operator==(const Entry &other) const = default;
    };
    struct Version {
        QString sha256;   // Empty: the entries whose version was not recorded.
        bool current = false; // The Library's PDF now.
        bool shown = false;   // The PDF of the tab the History is in.
        QString pdfPath;  // That PDF here, for the version read now.
        QDateTime firstAt; // Its first entry.
        // The card's answer in this version (1-based page, normalized), when known.
        QList<QPair<int, QRectF>> answerRegions;
        QList<Entry> entries; // Oldest first.
        // The comments on its passages (document-feedback.md), oldest first: feedback_id, kind (like,
        // dislike, question, other or drawing), comment, quote, regions (as anchor regions), resolved,
        // html_page (for one made in an HTML page) and at.
        QList<QVariantMap> comments;
        bool operator==(const Version &other) const = default;
    };

    explicit ReviewFeedbackPanel(QWidget *parent = nullptr);

    // Called each time the panel is shown, before it is drawn, to bring its History up to date.
    void setRefresh(std::function<void()> refresh);
    // The History, newest version first, under "@p summary". Blocks are rebuilt only when
    // something changed.
    void setHistory(const QList<Version> &versions, const QString &summary);
    QList<Version> versions() const
    {
        return m_versions;
    }
    // Scrolls to @p cardId under its newest version and marks it briefly, once it is laid out.
    void scrollToCard(const QString &cardId);
    // The card whose answer is masked in a review: it is listed, but does not open.
    void setMaskedCard(const QString &cardId);

    QSize sizeHint() const override;

Q_SIGNALS:
    // Another version to open ("Open ›", or one of its cards): its SHA-256, the date of its first
    // entry and the passages to mark there (regions JSON: the card's, else none).
    void versionRequested(const QString &sha256, const QDateTime &firstAt, const QVariantList &regions);
    // A card of the version this tab shows: its answer (regions JSON) to show in the PDF.
    void shownPassageRequested(const QVariantList &regions);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void rebuild();
    void applyCardScroll();

    QList<Version> m_versions;
    QString m_summary;
    QString m_maskedCardId;
    QString m_scrollCardId; // Asked for by scrollToCard, until it is shown.
    QLabel *m_title = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_list = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    bool m_built = false;
    std::function<void()> m_refresh;
};
}

#endif
