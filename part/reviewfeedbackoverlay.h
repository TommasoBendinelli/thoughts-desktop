// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "okularpart_export.h"
#include "documentfeedback.h"
#include "reviewcardfeedback.h"

#include <QColor>
#include <QFont>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

class QPainter;

namespace Okular
{
class Document;
}

namespace Thoughts
{
// The answer feedback of the card under review, on the PDF that shows it (review-card-feedback.md
// §4, §6): finds each passage in the card's current answer text, paints the highlights and
// creates, edits and deletes records through a writer. The page view decides when it is visible
// (only while the card's answer is revealed).
class OKULARPART_EXPORT ReviewFeedbackOverlay : public QObject
{
    Q_OBJECT
public:
    struct Item {
        QJsonObject record;
        QString state;                       // exact, relocated or orphaned
        QList<QPair<int, QRectF>> rects;     // 1-based page, normalized; empty when orphaned
        qint64 position = 0;                 // order in the answer
    };
    // Writes one record (queues it for the server); false and @p error when it could not.
    using Writer = std::function<bool(const QJsonObject &record, QString *error)>;

    explicit ReviewFeedbackOverlay(Okular::Document *document, QObject *parent = nullptr);

    // The card under review: its record, its current answer regions ({page, rect} maps, as the
    // mask gets them), and the displayed PDF's Library document ID and SHA-256.
    void setCard(const QJsonObject &card, const QVariantList &regions, const QString &documentId, const QString &pdfSha256);
    // The review_version of feedback created or edited now (FeedbackVersion, the displayed PDF), or
    // empty when it is not known.
    void setReviewVersion(const QJsonObject &version);
    QJsonObject reviewVersion() const;
    // Every known record of the card, deleted ones included (tombstones keep Undo and sticky deletes),
    // and the records of other cards (homeCards()) made in its document (document()), which are
    // shown where their passage lies in this card's answer (shared feedback,
    // ReviewCardFeedback::resolveShared). Other records are left out.
    void setRecords(const QList<QJsonObject> &records);
    // The cards that exist, which feedback may be shared from (set before setRecords()); without
    // them no other card's feedback is shown. clear() forgets them.
    void setHomeCards(const ReviewCardFeedback::HomeCards &cards);
    ReviewCardFeedback::HomeCards homeCards() const;
    // The document of the card's answer: the shown PDF's and the card's own IDs, its PDF stem.
    ReviewCardFeedback::CardDocument document() const;
    void clear();
    bool hasCard() const;
    QString cardId() const;
    QJsonObject card() const;
    // Writes several records in one store write.
    using BatchWriter = std::function<bool(const QList<QJsonObject> &records, QString *error)>;
    void setWriter(Writer writer);

    // The card's records, not deleted, found again in the current answer: by position in the
    // answer, the ones not found (orphaned) last. Then also the records of other cards found in
    // this answer (shared); those not found in it are left out, never orphaned here.
    QList<Item> items();
    // items() for the review panel: feedback_id, polarity, comment, quote, state, created_at,
    // updated_at, revision, anchor_pdf_sha256 and anchor_regions (where it was given: the PDF and
    // its {page, rect} regions there), displayed_pdf_sha256 (the PDF shown now), card_id (the card
    // it was made on) and shared (made on another card).
    QVariantList itemMaps();
    // feedback_id -> state of every listed record, as the answer is now (also while masked).
    QVariantMap states();
    QJsonObject record(const QString &feedbackId) const;
    QList<QPair<int, QRectF>> answerRegions() const;

    // Whether a selection with these line rectangles may get feedback (§6.2).
    bool qualifies(const QList<QPair<int, QRectF>> &lines) const;
    // A new record for the selection: @p lines its line rectangles in reading order, @p text its
    // text, @p selected whether a glyph centre (1-based page, normalized point) is selected.
    QJsonObject newRecord(const QString &polarity, const QString &comment, const QList<QPair<int, QRectF>> &lines, const QString &text,
                          const std::function<bool(int, const QPointF &)> &selected, QString *error) const;
    // Writes @p record through the writer and shows it at once.
    bool write(const QJsonObject &record, QString *error = nullptr);
    // Writes @p records in one store write (the batch writer; without one, one by one through the
    // writer) and shows them at once. Nothing is shown when the write fails.
    bool writeAll(const QList<QJsonObject> &records, QString *error = nullptr);
    void setBatchWriter(BatchWriter writer);
    // While a highlight's passage is being changed it is not drawn and not hit (empty: none).
    void setHiddenFeedback(const QString &feedbackId);
    QString hiddenFeedback() const;

    // Paints the highlights, oldest first so the newest is on top. @p map gives a normalized
    // rectangle of a page in painter coordinates, or an empty one when the page is not shown.
    void paint(QPainter *painter, const std::function<QRectF(int page, const QRectF &rect)> &map, bool dark);
    // The highlight at @p point (painter coordinates), the newest on top; empty when none.
    QString hitTest(const QPointF &point, const std::function<QRectF(int page, const QRectF &rect)> &map);
    // Every highlight rectangle of @p feedbackId, as items() places it.
    QList<QPair<int, QRectF>> rectsOf(const QString &feedbackId);

    // paint() and hitTest() for any highlights (also document feedback, documentfeedbackoverlay.h):
    // the §6.3 fill, underline and comment dot, the newest on top.
    // With @p textBoxFrames (the Text tool) each text box has a frame, @p selectedTextBox a stronger one.
    static void paintItems(QPainter *painter, const QList<Item> &items, const std::function<QRectF(int page, const QRectF &rect)> &map, bool dark,
                           const QString &selectedTextBox = QString(), bool textBoxFrames = false);
    static QString hitTestItems(const QList<Item> &items, const QPointF &point, const std::function<QRectF(int page, const QRectF &rect)> &map);

    // The pencil strokes of a drawing on @p page, in normalized page coordinates.
    static void paintStrokes(QPainter *painter, int page, const QList<DocumentFeedback::Stroke> &strokes, const std::function<QRectF(int page, const QRectF &rect)> &map,
                             bool dark);

    // The text of a text box at @p box (normalized) on @p page, its font @p fontSize of the page's
    // height; @p frame 0 without a frame, 1 with a light one, 2 with the frame of the selected box.
    static void paintTextBox(QPainter *painter, int page, const QRectF &box, double fontSize, const QString &text,
                             const std::function<QRectF(int page, const QRectF &rect)> &map, bool dark, int frame = 0);
    // The size (normalized) of @p text in a text box at @p fontSize on a page @p aspect wide (width
    // over height): as wide as its longest line, at most @p maximumWidth, where its lines wrap.
    static QSizeF textBoxSize(const QString &text, double fontSize, double maximumWidth, double aspect);
    // The font of text boxes, @p pixelSize pixels high.
    static QFont textBoxFont(double pixelSize);

    static QColor colour(const QString &polarity, bool dark);
    static double fillOpacity(bool dark);

    // "2 liked · 1 disliked (1 not found)", or empty without feedback.
    static QString summary(const QList<Item> &items);

Q_SIGNALS:
    void changed();

private:
    void ensureAnswer();
    QList<ReviewCardFeedback::Glyph> glyphs(int page, const QRectF &rect) const;

    Okular::Document *m_document = nullptr;
    QJsonObject m_card;
    QString m_cardId;
    QList<QPair<int, QRectF>> m_regions;
    QString m_documentId;
    ReviewCardFeedback::CardDocument m_answerDocument;
    ReviewCardFeedback::HomeCards m_homeCards;
    QString m_pdfSha256;
    QJsonObject m_reviewVersion;
    QList<QJsonObject> m_records;
    Writer m_writer;
    BatchWriter m_batchWriter;
    QString m_hidden;
    std::optional<ReviewCardFeedback::AnswerText> m_answer;
    std::optional<QList<Item>> m_items;
};
}
