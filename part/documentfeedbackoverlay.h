// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "documentfeedback.h"
#include "okularpart_export.h"
#include "reviewfeedbackoverlay.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QVariantMap>

#include <functional>

class QPainter;

namespace Okular
{
class Document;
}

namespace Thoughts
{
// The document feedback of the PDF being read (document-feedback.md §4, §5): finds each passage
// on the displayed PDF, paints the highlights and creates, edits and deletes records through a
// writer. The page view decides when it is visible (normal reading: not while a review card's
// answer is masked or revealed). A record made on another PDF version is placed only when it
// could show on a page that is painted (its anchor pages within pageWindow of it) or when it is
// asked for, and each placement is kept for the displayed PDF.
class OKULARPART_EXPORT DocumentFeedbackOverlay : public QObject
{
    Q_OBJECT
public:
    using Item = ReviewFeedbackOverlay::Item;
    using Writer = ReviewFeedbackOverlay::Writer;
    using BatchWriter = ReviewFeedbackOverlay::BatchWriter;

    explicit DocumentFeedbackOverlay(Okular::Document *document, QObject *parent = nullptr);

    // The displayed PDF: its Library document ID (a UUIDv4) and the SHA-256 of its bytes. Placements
    // are forgotten when either changes; without both no feedback is given or shown.
    void setDocument(const QString &documentId, const QString &pdfSha256);
    // The review_version of feedback created or edited now (FeedbackVersion), or empty when unknown.
    void setReviewVersion(const QJsonObject &version);
    QJsonObject reviewVersion() const;
    // What shows (document-feedback.md §9): @p version gives the current SHA-256 of an HTML attachment
    // by its path; with @p show also what is from earlier versions.
    void setAttachmentVersions(std::function<QString(const QString &path)> version);
    void setShowEarlier(bool show);
    bool showsEarlier() const;
    bool isCurrent(const QJsonObject &record) const;
    // The records not deleted that show now, and how many others there are.
    QList<QJsonObject> shownRecords() const;
    int earlierCount() const;

    // Every known record (deleted ones too: tombstones keep Undo and sticky deletes); those of
    // other documents are left out.
    void setRecords(const QList<QJsonObject> &records);
    // The PDF's pages were replaced (a reload): its text is read again.
    void resetPages();
    void clear();
    bool hasDocument() const;
    QString documentId() const;
    QString pdfSha256() const;
    void setWriter(Writer writer);

    // The document's records, deleted ones included.
    QList<QJsonObject> records() const;
    QJsonObject record(const QString &feedbackId) const;
    // feedback_id -> exact / relocated / orphaned for every record not deleted (places them all).
    QVariantMap states();
    // Every highlight rectangle of @p feedbackId (empty when orphaned or deleted).
    QList<QPair<int, QRectF>> rectsOf(const QString &feedbackId);

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

    // Paints the highlights that can show on @p pages (1-based, the pages painted now), oldest
    // first. @p map gives a normalized rectangle of a page in painter coordinates, or an empty one
    // when the page is not shown.
    // With @p pageMarksOnly the highlights stay hidden and drawings and text boxes still show. With
    // @p textBoxFrames (the Text tool) every text box has a frame, the selected one a stronger one.
    void paint(QPainter *painter, const QSet<int> &pages, const std::function<QRectF(int page, const QRectF &rect)> &map, bool dark, bool pageMarksOnly = false,
               bool textBoxFrames = false);
    // The highlight at @p point (painter coordinates) on @p page, the newest on top; empty when none.
    QString hitTest(int page, const QPointF &point, const std::function<QRectF(int page, const QRectF &rect)> &map, bool pageMarksOnly = false);
    // The text box at @p point on @p page, the newest on top; empty when none.
    QString textBoxAt(int page, const QPointF &point, const std::function<QRectF(int page, const QRectF &rect)> &map);
    // For the eraser: the strokes of the drawings shown on @p page that pass within @p radius of the
    // segment from @p from to @p to (painter coordinates), as feedback_id -> their indexes.
    QHash<QString, QList<int>> strokesNear(int page, const QPointF &from, const QPointF &to, double radius,
                                           const std::function<QRectF(int page, const QRectF &rect)> &map);
    // The text box the Text tool has selected (Delete removes it), drawn with a stronger frame.
    void setSelectedTextBox(const QString &feedbackId);
    QString selectedTextBox() const;
    // The text box being typed or moved: the page view draws it where it is going, so it is not
    // painted here (it is still hit).
    void setTextBoxInHand(const QString &feedbackId);

Q_SIGNALS:
    void changed();

private:
    // The placed records (not deleted, not orphaned) that may show on @p pages; every record
    // when @p pages is empty.
    QList<Item> items(const QSet<int> &pages);
    QList<Item> shownItems(const QSet<int> &pages, bool pageMarksOnly);

public:
    // Which highlights show (Visible markup); drawings and text boxes always do.
    void setShownKinds(bool comments, bool feedback);

private:
    DocumentFeedback::Placement placementOf(const QJsonObject &record);
    QList<ReviewCardFeedback::Glyph> pageGlyphs(int page) const;
    ReviewCardFeedback::AnswerText pagesText(const QList<int> &pages) const;
    int pageCount() const;

    Okular::Document *m_document = nullptr;
    QString m_documentId;
    QString m_pdfSha256;
    QJsonObject m_reviewVersion;
    QList<QJsonObject> m_records;
    Writer m_writer;
    BatchWriter m_batchWriter;
    QString m_hidden;
    QString m_selectedTextBox;
    QString m_textBoxInHand;
    std::function<QString(const QString &path)> m_attachmentVersions;
    bool m_showEarlier = false;
    // Visible markup: comments (they follow their text) and feedback (likes, dislikes and
    // questions, which belong to their version). Drawings and text boxes always show.
    bool m_showComments = true;
    bool m_showFeedback = true;
    QHash<QString, DocumentFeedback::Placement> m_placements;
    mutable QHash<int, QList<ReviewCardFeedback::Glyph>> m_pageGlyphs;
};
}
