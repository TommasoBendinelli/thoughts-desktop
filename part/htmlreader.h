// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The reader's HTML view (thoughts-html-view-SPEC.md, "Desktop"): which edition the open document
// has, the PDF | HTML choice per document, the items (cards, comments, answer feedback) shown in
// the edition, and the review presenter (mask, reveal, flip, blocks) on top of HtmlDocumentView.
// The Part owns one and turns its signals into the app's actions (menus, new cards, links).
#include "htmledition.h"
#include "okularpart_export.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <functional>

namespace Okular
{
class HtmlViewTest;
}
class QTimer;

namespace Thoughts
{
class HtmlDocumentView;

class OKULARPART_EXPORT HtmlReader : public QObject
{
    Q_OBJECT
public:
    HtmlReader(HtmlDocumentView *view, HtmlEdition::Store *store, QObject *parent = nullptr);

    HtmlDocumentView *view() const
    {
        return m_view;
    }
    HtmlEdition::Store *store() const
    {
        return m_store;
    }
    HtmlEdition::Fetcher *fetcher() const
    {
        return m_fetcher;
    }

    // The opened document (a Library PDF path, or empty when closed). Reads its Library entry.
    void setDocument(const QString &pdfPath);
    QString documentPath() const
    {
        return m_pdfPath;
    }
    const HtmlEdition::LibraryInfo &libraryInfo() const
    {
        return m_info;
    }
    // Re-reads the Library entry (after Sync) and, while the HTML view is shown, follows it.
    void refreshLibraryState();

    // The PDF | HTML choice of this document, remembered per document on this computer.
    bool prefersHtml() const;
    bool isActive() const
    {
        return m_active;
    }
    // Shows the HTML view (or the reason it cannot) / the PDF. `remember` stores the choice.
    void setActive(bool html, bool remember);

    // What the HTML view can show now, and the sentence for the user ("HTML is being built", …).
    HtmlEdition::Availability availability() const
    {
        return m_availability;
    }
    QString availabilityText() const;
    bool isEditionShown() const
    {
        return m_active && m_ready && !m_shownEdition.isEmpty();
    }
    QString shownEditionSha() const
    {
        return m_shownEdition;
    }
    std::optional<HtmlEdition::Manifest> shownManifest() const
    {
        return m_manifest;
    }

    // Items: card records of this PDF (the sidebar's list), logical comment records, feedback
    // records of the card under review (own and shared).
    void setCards(const QJsonArray &cards);
    void setComments(const QList<QJsonObject> &comments);
    void setFeedback(const QList<QJsonObject> &records);
    // Cards the edition cannot place (manifest has no anchor for them): "Not shown in HTML view".
    QStringList notShownCardIds() const;
    QJsonObject card(const QString &cardId) const;
    const QList<QJsonObject> &cards() const
    {
        return m_cards;
    }

    // The anchor keys of a card's answer / question in this edition.
    QStringList cardAnchors(const QJsonObject &card) const;
    QStringList cardQuestionAnchors(const QJsonObject &card) const;
    // Whether the HTML view can present this card's review (else it reviews in the PDF).
    bool canPresent(const QJsonObject &card) const;
    static bool isHtmlPending(const QJsonObject &card);

    // Review presenter. present() returns false when the card cannot be shown in HTML; a later
    // failure in the page emits reviewUnavailable().
    bool presentReview(const QJsonObject &card, bool inverted);
    void reveal();
    void setReviewInverted(bool inverted);
    void clearReview();
    QString reviewCardId() const
    {
        return m_reviewCardId;
    }
    bool reviewRevealed() const
    {
        return m_reviewRevealed;
    }
    int reviewBlockCount() const
    {
        return m_reviewBlocks;
    }
    bool focusReviewBlock(int index);

    // Position mapping: the last position the viewer reported; scroll targets.
    QJsonObject lastPosition() const
    {
        return m_lastPosition;
    }
    QJsonObject lastSelection() const
    {
        return m_lastSelection;
    }
    void scrollTo(const QJsonValue &target);

    void setTextSize(int percent);
    int textSize() const;

    // Options for load() (text size, theme); exposed for tests.
    QJsonObject loadOptions() const;

    // The app's shortcuts the page hands back instead of handling them (the viewer's
    // forwardKeys, e.g. "s", "Escape", "Meta+Shift+u"); asked at every load, so changed
    // shortcuts apply to the next edition shown. A match arrives as keyForwarded.
    void setForwardedKeysProvider(std::function<QStringList()> provider)
    {
        m_forwardedKeys = std::move(provider);
    }

Q_SIGNALS:
    void activeChanged(bool active);
    void availabilityChanged();
    void ready();
    void itemsPlaced(const QJsonObject &report);
    void contextMenuRequested(const QJsonObject &message);
    void itemTapped(const QJsonObject &message);
    void linkActivated(const QJsonObject &message);
    void selectionChanged(const QJsonObject &selection);
    void reviewMaskChanged();
    void reviewUnavailable(const QString &cardId, const QString &reason);
    void notShownChanged(const QStringList &cardIds);
    void messageForUser(const QString &text);
    // A forwardKeys match: {key, code, meta, ctrl, alt, shift, repeat} as the page saw it.
    void keyForwarded(const QJsonObject &message);

private:
    friend class Okular::HtmlViewTest;
    void updateAvailability(bool askServer);
    void showCurrent();
    void loadShownEdition(const QString &editionSha);
    void onViewerMessage(const QJsonObject &message);
    void pushItems();
    QString preferenceKey() const;
    QJsonArray viewerCards() const;
    QJsonArray viewerComments() const;
    QJsonArray viewerFeedback() const;

    QPointer<HtmlDocumentView> m_view;
    HtmlEdition::Store *m_store;
    HtmlEdition::Fetcher *m_fetcher;
    QTimer *m_pollTimer;
    int m_polls = 0; // server checks while the shown version's edition is being built
    QString m_pdfPath;
    HtmlEdition::LibraryInfo m_info;
    HtmlEdition::Availability m_availability;
    QString m_lastError;
    bool m_active = false;
    bool m_ready = false;
    bool m_checking = false;
    bool m_downloading = false;
    QString m_shownEdition;
    std::optional<HtmlEdition::Manifest> m_manifest;
    QList<QJsonObject> m_cards;
    QList<QJsonObject> m_comments;
    QList<QJsonObject> m_feedback;
    QSet<QString> m_unplacedCards;
    QJsonObject m_lastPosition;
    QJsonObject m_lastSelection;
    QString m_reviewCardId;
    QJsonObject m_reviewCard;
    bool m_reviewInverted = false;
    bool m_reviewRevealed = false;
    int m_reviewBlocks = 0;
    int m_generation = 0;
    QList<std::function<void()>> m_afterReady;
    std::function<QStringList()> m_forwardedKeys;
};
}
