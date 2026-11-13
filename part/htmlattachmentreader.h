// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// HTML attachments inside Thoughts (thoughts-html-attachment-cards-SPEC.md, "Desktop"): the page a
// thoughts-html: link of a PDF names, shown in an HtmlDocumentView with the shared viewer in
// attachment mode. The page keeps its own scripts, tabs and #hash routes. Cards made in it are
// anchored by their remembered text (text_quote) and page state (location.hash), never by source
// markers: this class highlights the owner's cards of the page, turns a selection into a new
// card's placement, and presents a card's review (page state, mask, reveal, "not found").
#include "core/htmlattachment.h"
#include "okularpart_export.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace Thoughts
{
class HtmlDocumentView;

class OKULARPART_EXPORT HtmlAttachmentReader : public QObject
{
    Q_OBJECT
public:
    // The document whose PDF link opened the page: it owns the cards made in it.
    struct Owner {
        QString pdfPath;
        QString pdfStem;
        QString pdfSha256;
        QString documentId;
    };

    explicit HtmlAttachmentReader(HtmlDocumentView *view, QObject *parent = nullptr);

    HtmlDocumentView *view() const
    {
        return m_view;
    }

    // Shows the page of `target` (served on a fresh loopback origin that lives as long as the page
    // is shown). The same object already shown is kept as it is (its scripts' state too).
    bool open(const HtmlAttachmentTarget &target, const Owner &owner, QString *error);
    void close();
    bool isOpen() const
    {
        return m_target.isValid();
    }
    bool isReady() const
    {
        return m_ready;
    }
    const HtmlAttachmentTarget &target() const
    {
        return m_target;
    }
    const Owner &owner() const
    {
        return m_owner;
    }
    QUrl pageUrl() const
    {
        return m_url;
    }
    // The page state last reported by the viewer ({hash}).
    QJsonObject pageState() const
    {
        return m_pageState;
    }

    // The owner's cards (the reader sidebar's list); those made in this page are highlighted.
    void setCards(const QJsonArray &cards);
    QList<QJsonObject> pageCards() const;
    // The last placement report of the viewer (cards found / not found in the page).
    QJsonObject placement() const
    {
        return m_placement;
    }

    // The document's feedback records; those made in this page are highlighted.
    void setFeedback(const QList<QJsonObject> &records);
    QList<QJsonObject> pageFeedback() const;

    // Whether `card` was made in the attachment at `path`.
    static bool isCardOf(const QJsonObject &card, const QString &path);
    // setItems() cards: by quote, the card under review selected.
    static QJsonArray viewerCards(const QList<QJsonObject> &cards, const QString &reviewCardId);
    // A new card's placement from a viewer selection payload: created_in, html_attachment
    // {path, sha256}, text_quote, page_state {hash}. Empty when the selection has no text.
    static QJsonObject newCardFields(const QJsonObject &selection, const HtmlAttachmentTarget &target);
    // The answer text a card remembers (shown when the page no longer has it).
    static QString storedAnswer(const QJsonObject &card);

    QJsonObject lastSelection() const
    {
        return m_lastSelection;
    }

    // Review presenter: restores the card's page state and masks its text once it is in the page.
    // "pending" until then; "not_found" when the page settled without it (answerNotFound()).
    bool presentReview(const QJsonObject &card, bool inverted);
    void reveal();
    void setReviewInverted(bool inverted);
    void clearReview();
    QString reviewCardId() const
    {
        return m_reviewCardId;
    }
    QString reviewState() const
    {
        return m_reviewState;
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
    // Shows a card's place without masking it: its page state, then its text scrolled into view.
    void showCard(const QJsonObject &card);
    // The same for a feedback record made in this page.
    void showFeedback(const QJsonObject &record);

    // Options for load() (attachment mode); exposed for tests.
    QJsonObject loadOptions() const;

    // The app's shortcuts the page hands back when it does not use them itself (the viewer's
    // forwardKeys; in attachment mode decided after the page's own key listeners). Asked at
    // every load. A match arrives as keyForwarded.
    void setForwardedKeysProvider(std::function<QStringList()> provider)
    {
        m_forwardedKeys = std::move(provider);
    }

Q_SIGNALS:
    void ready();
    void loadFailed(const QString &error);
    void selectionChanged(const QJsonObject &selection);
    void itemTapped(const QJsonObject &message);
    // A right click on selected text or feedback, where the app shows its menu (app_menu).
    void contextMenuRequested(const QJsonObject &message);
    void reviewMaskChanged();
    void answerNotFound(const QString &cardId);
    void placementChanged(const QJsonObject &report);
    // A forwardKeys match: {key, code, meta, ctrl, alt, shift, repeat} as the page saw it.
    void keyForwarded(const QJsonObject &message);

private:
    void onViewerMessage(const QJsonObject &message);
    void pushItems();
    void showPlace(const QString &hash, const QString &id, const QString &kind);
    void presentNow();
    void whenReady(std::function<void()> run);

    QPointer<HtmlDocumentView> m_view;
    QObject *m_serverOwner = nullptr;
    QString m_loadError;
    HtmlAttachmentTarget m_target;
    Owner m_owner;
    QUrl m_url;
    bool m_ready = false;
    int m_generation = 0;
    QList<QJsonObject> m_cards;
    QList<QJsonObject> m_feedback;
    QJsonObject m_placement;
    QJsonObject m_lastSelection;
    QJsonObject m_pageState;
    QJsonObject m_reviewCard;
    QString m_reviewCardId;
    QString m_reviewState;
    bool m_reviewInverted = false;
    bool m_reviewRevealed = false;
    int m_reviewBlocks = 0;
    QList<std::function<void()>> m_afterReady;
    std::function<QStringList()> m_forwardedKeys;
};
}
