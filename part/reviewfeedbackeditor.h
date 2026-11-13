// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "okularpart_export.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QJsonObject>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace Thoughts
{
// The answer feedback editor (review-card-feedback.md §6.3), anchored to a highlight: the quote
// (at most 3 lines), Like / Dislike, the comment, when it was created and edited, and Delete (not
// for a new record) and Cancel. There is no Save: what was written is kept when the editor goes
// away, however it goes, and only Cancel throws it away. It only reports the choice; the caller
// writes. With @p kinds it also offers Question and Comment (document-feedback.md §9), which need
// text.
class OKULARPART_EXPORT ReviewFeedbackEditor : public QFrame
{
    Q_OBJECT
public:
    // @p record is the stored record, or for a new one a record whose feedback_id is empty
    // (quote and polarity are read from it).
    ReviewFeedbackEditor(const QJsonObject &record, bool isNew, QWidget *parent = nullptr, bool kinds = false);

    QString polarity() const;
    // DocumentFeedback::question() or other(), or empty for Like and Dislike.
    QString kind() const;
    QString comment() const;
    QString feedbackId() const;
    bool isNew() const;
    // Whether closing would change the stored record (always true for a new one).
    bool changed() const;
    // Offers Change passage (hidden until then): the caller does so only for a highlight drawn on
    // the displayed PDF that this card or document owns.
    void setChangePassageAvailable(bool available);
    // Shows the editor next to @p anchor (global coordinates), on screen.
    void popupAt(const QRect &anchor);

    // The quote cut to @p lines lines of @p width pixels in @p font, with "…" when cut.
    static QString elidedLines(const QString &text, const QFont &font, int width, int lines);

Q_SIGNALS:
    void saveRequested(const QString &polarity, const QString &comment);
    void deleteRequested();
    void resolveRequested();
    // Change passage: the editor closes without saving; the polarity, kind and comment as they
    // stand go with the new passage.
    void changePassageRequested(const QString &polarity, const QString &kind, const QString &comment);

protected:
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void keep();

    QJsonObject m_record;
    bool m_new = false;
    bool m_finished = false;
    QElapsedTimer m_shown;
    QLabel *m_quote = nullptr;
    QToolButton *m_like = nullptr;
    QToolButton *m_dislike = nullptr;
    QToolButton *m_question = nullptr;
    QToolButton *m_other = nullptr;
    QPlainTextEdit *m_comment = nullptr;
    QPushButton *m_changePassage = nullptr;
};
}
