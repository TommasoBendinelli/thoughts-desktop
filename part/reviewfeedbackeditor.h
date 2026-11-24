// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "okularpart_export.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QSet>

#include <functional>

class QHBoxLayout;
class QImage;
class QLabel;
class QMimeData;
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

    // Where the pictures of a comment are kept on this computer (document-feedback.md §12): keep
    // stores bytes by their SHA-256 and returns it (empty with an error), load gives the bytes when
    // they are here, fetch downloads them from the server, then done(found).
    struct PictureStore {
        std::function<QString(const QByteArray &bytes, QString *error)> keep;
        std::function<QByteArray(const QString &sha256)> load;
        std::function<void(const QString &sha256, QObject *context, std::function<void(bool found)> done)> fetch;
    };
    // Offers pictures (20 November): a row of them under the comment, Add Picture…, paste
    // and drop. Only for document feedback, never on a drawing. pictures() is what the record
    // should carry when it is saved.
    void enablePictures(const PictureStore &store);
    QJsonArray pictures() const;
    // A picture as it is kept: at most DocumentFeedback::maximumPictureSide on its longer side, PNG
    // when @p png or when it has transparency, else JPEG; @p mediaType says which. Empty when it
    // cannot be written.
    static QByteArray encodedPicture(const QImage &image, bool png, QString *mediaType);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    // Adds pictures, up to the limit; says why one could not be added.
    void addPictureFiles(const QStringList &paths);
    void addPictureImage(const QImage &image, bool png);
    bool addPictureBytes(const QByteArray &bytes, const QString &mediaType, const QSize &size, QString *error);
    // Whether @p data holds a picture this editor can take (an image, or image files).
    static bool hasPicture(const QMimeData *data);
    void addPicturesFrom(const QMimeData *data);
    void choosePictureFiles();
    void rebuildPictures();
    void openPicture(const QJsonObject &picture);

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
    // Pictures (§12): the store, the record's pictures as edited, the row that shows them.
    PictureStore m_store;
    bool m_picturesEnabled = false;
    QJsonArray m_pictures;
    QWidget *m_picturesRow = nullptr;
    QHBoxLayout *m_picturesLayout = nullptr;
    QLabel *m_pictureMessage = nullptr;
    QSet<QString> m_fetching;
    // While the file dialog is open the editor is hidden, not finished.
    bool m_choosingPicture = false;
    QRect m_anchor;
};
}
