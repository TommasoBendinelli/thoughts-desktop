// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewfeedbackeditor.h"

#include "documentfeedback.h"
#include "reviewcardfeedback.h"
#include "reviewfeedbackoverlay.h"

#include <KLocalizedString>

#include <QCursor>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QColor>
#include <QDateTime>
#include <QFontMetrics>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace Thoughts
{
namespace
{
QString dateText(const QString &value)
{
    QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::fromString(value, Qt::ISODate);
    return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : value;
}
}

QString ReviewFeedbackEditor::elidedLines(const QString &text, const QFont &font, int width, int lines)
{
    const QString simplified = text.simplified();
    if (width <= 0 || lines <= 0) return simplified;
    QTextLayout layout(simplified, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QList<int> starts;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        starts.append(line.textStart());
    }
    layout.endLayout();
    if (starts.size() <= lines) return simplified;
    // The first lines as they are; the last one holds the rest, cut with "…".
    const QString head = simplified.left(starts.at(lines - 1));
    return head + QFontMetrics(font).elidedText(simplified.mid(starts.at(lines - 1)), Qt::ElideRight, width);
}

ReviewFeedbackEditor::ReviewFeedbackEditor(const QJsonObject &record, bool isNew, QWidget *parent, bool kinds)
    : QFrame(parent, Qt::Popup)
    , m_record(record)
    , m_new(isNew)
{
    setObjectName(QStringLiteral("reviewFeedbackEditor"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(340);
    setMaximumWidth(460);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    const QString quote = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString();
    m_quote = new QLabel(this);
    m_quote->setObjectName(QStringLiteral("reviewFeedbackQuote"));
    m_quote->setWordWrap(true);
    m_quote->setToolTip(quote.simplified());
    QFont quoteFont = m_quote->font();
    quoteFont.setItalic(true);
    m_quote->setFont(quoteFont);
    m_quote->setText(QStringLiteral("“%1”").arg(elidedLines(quote, quoteFont, 420, 3)));
    layout->addWidget(m_quote);

    auto *polarityRow = new QHBoxLayout;
    polarityRow->setSpacing(6);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    const auto addChoice = [&](QToolButton *&slot, const QString &name, const QString &text, const QString &choice) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setText(text);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setMinimumWidth(kinds ? 70 : 84);
        // The chosen one in its highlight colour (review-card-feedback.md §6.3).
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor accent = ReviewFeedbackOverlay::colour(choice, dark);
        button->setStyleSheet(QStringLiteral("QToolButton { padding: 3px 10px; border: 1px solid palette(mid); border-radius: 4px; }"
                                             "QToolButton:checked { border: 2px solid %1; background: rgba(%2, %3, %4, %5); font-weight: 600; }")
                                  .arg(accent.name()).arg(accent.red()).arg(accent.green()).arg(accent.blue()).arg(dark ? 0.28 : 0.22));
        group->addButton(button);
        polarityRow->addWidget(button);
        slot = button;
    };
    addChoice(m_like, QStringLiteral("reviewFeedbackLike"), i18n("Like"), ReviewCardFeedback::positive());
    addChoice(m_dislike, QStringLiteral("reviewFeedbackDislike"), i18n("Dislike"), ReviewCardFeedback::negative());
    if (kinds) {
        addChoice(m_question, QStringLiteral("reviewFeedbackQuestion"), i18n("Question"), ReviewCardFeedback::neutral());
        addChoice(m_other, QStringLiteral("reviewFeedbackOther"), i18n("Comment"), ReviewCardFeedback::neutral());
    }
    polarityRow->addStretch(1);
    layout->addLayout(polarityRow);
    const bool negative = record.value(QStringLiteral("polarity")).toString() == ReviewCardFeedback::negative();
    const QString kind = DocumentFeedback::kindOf(record);
    (kind == DocumentFeedback::question() ? m_question : kind == DocumentFeedback::other() ? m_other : negative ? m_dislike : m_like)->setChecked(true);
    // A drawing stays a drawing and a text box a text box: there is no passage to quote and no
    // other kind to choose.
    if (DocumentFeedback::isPageMark(record)) {
        m_quote->hide();
        for (QToolButton *choice : {m_like, m_dislike, m_question, m_other}) {
            if (choice) choice->hide();
        }
    }

    m_comment = new QPlainTextEdit(this);
    m_comment->setObjectName(QStringLiteral("reviewFeedbackComment"));
    m_commentPlaceholder = DocumentFeedback::isTextBox(record) ? i18n("The text on the page") : kinds ? i18n("Your comment") : i18n("What do you like or dislike here?");
    m_comment->setPlaceholderText(m_commentPlaceholder);
    m_comment->setPlainText(record.value(QStringLiteral("comment")).toString());
    m_comment->setTabChangesFocus(true);
    m_comment->setFixedHeight(m_comment->fontMetrics().lineSpacing() * 4 + 14);
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this] {
        // At most 4000 characters, as the server accepts.
        const QString text = m_comment->toPlainText();
        if (text.toUcs4().size() > ReviewCardFeedback::maximumComment) {
            const QSignalBlocker blocker(m_comment);
            const auto ucs4 = text.toUcs4().mid(0, ReviewCardFeedback::maximumComment);
            m_comment->setPlainText(QString::fromUcs4(reinterpret_cast<const char32_t *>(ucs4.constData()), ucs4.size()));
            m_comment->moveCursor(QTextCursor::End);
        }
    });
    layout->addWidget(m_comment);

    // Pictures (§12), shown once enablePictures offers them.
    m_picturesRow = new QWidget(this);
    m_picturesRow->setObjectName(QStringLiteral("reviewFeedbackPictures"));
    m_picturesLayout = new QHBoxLayout(m_picturesRow);
    m_picturesLayout->setContentsMargins(0, 4, 0, 0);
    m_picturesLayout->setSpacing(6);
    m_picturesRow->hide();
    layout->addWidget(m_picturesRow);
    m_pictureMessage = new QLabel(this);
    m_pictureMessage->setObjectName(QStringLiteral("reviewFeedbackPictureMessage"));
    m_pictureMessage->setWordWrap(true);
    m_pictureMessage->hide();
    layout->addWidget(m_pictureMessage);

    if (!isNew) {
        const QString created = record.value(QStringLiteral("created_at")).toString();
        const QString updated = record.value(QStringLiteral("updated_at")).toString();
        QString dates = i18n("Created %1", dateText(created));
        if (!updated.isEmpty() && dateText(updated) != dateText(created)) dates += QStringLiteral(", ") + i18n("edited %1", dateText(updated));
        // The other person of a shared Project wrote it (the server's shared_author).
        const QString sharedBy = record.value(QStringLiteral("shared_author")).toObject().value(QStringLiteral("username")).toString().trimmed();
        if (!sharedBy.isEmpty()) dates = i18n("By %1 · %2", sharedBy, dates);
        auto *datesLabel = new QLabel(dates, this);
        datesLabel->setObjectName(QStringLiteral("reviewFeedbackDates"));
        QPalette palette = datesLabel->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
        datesLabel->setPalette(palette);
        layout->addWidget(datesLabel);
    }

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    if (!isNew) {
        auto *remove = new QPushButton(i18n("Delete"), this);
        remove->setObjectName(QStringLiteral("reviewFeedbackDelete"));
        remove->setAutoDefault(false);
        connect(remove, &QPushButton::clicked, this, [this] {
            m_finished = true;
            Q_EMIT deleteRequested();
            close();
        });
        buttons->addWidget(remove);
        if (DocumentFeedback::canResolve(record)) {
            auto *resolve = new QPushButton(DocumentFeedback::isResolved(record) ? i18n("Reopen") : i18n("Resolve"), this);
            resolve->setObjectName(QStringLiteral("reviewFeedbackResolve"));
            resolve->setAutoDefault(false);
            connect(resolve, &QPushButton::clicked, this, [this] {
                keep();
                Q_EMIT resolveRequested();
                close();
            });
            buttons->addWidget(resolve);
        }
    }
    if (!isNew) {
        m_changePassage = new QPushButton(i18n("Change passage"), this);
        m_changePassage->setObjectName(QStringLiteral("reviewFeedbackChangePassage"));
        m_changePassage->setAutoDefault(false);
        m_changePassage->hide();
        connect(m_changePassage, &QPushButton::clicked, this, [this] {
            m_finished = true;
            const QString chosenPolarity = polarity(), chosenKind = this->kind(), chosenComment = comment();
            close();
            Q_EMIT changePassageRequested(chosenPolarity, chosenKind, chosenComment);
        });
        buttons->addWidget(m_changePassage);
    }
    // No Cancel: nothing written is ever thrown away. Clicking elsewhere and Esc keep the text,
    // and only Delete removes a comment. A new one has no buttons at all, but Ask Agent for a
    // question (hidden until setAskAvailable; an empty row takes no room).
    buttons->addStretch(1);
    m_ask = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), i18n("Ask Agent"), this);
    m_ask->setObjectName(QStringLiteral("reviewFeedbackAsk"));
    m_ask->setToolTip(i18n("Sends the passage, your question and the page to the agent you sent to last. Nothing is saved in the document."));
    m_ask->setAutoDefault(false);
    m_ask->hide();
    connect(m_ask, &QPushButton::clicked, this, [this] {
        const QString question = comment();
        if (question.isEmpty() || !asking()) return;
        m_finished = true;
        Q_EMIT askRequested(question);
        close();
    });
    buttons->addWidget(m_ask);
    layout->addLayout(buttons);
    connect(group, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool checked) {
        if (checked) updateAskMode();
    });
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this] {
        if (m_ask) m_ask->setEnabled(!comment().isEmpty());
    });
    m_comment->setFocus();
}

void ReviewFeedbackEditor::setAskAvailable(bool available)
{
    m_askAvailable = available && !DocumentFeedback::isPageMark(m_record);
    // Only a new passage or a question kept before can be a question now: nothing becomes one
    // that would be saved.
    if (m_askAvailable && m_question && !m_new && DocumentFeedback::kindOf(m_record) != DocumentFeedback::question()) m_question->hide();
    updateAskMode();
}

bool ReviewFeedbackEditor::asking() const
{
    return m_askAvailable && kind() == DocumentFeedback::question();
}

void ReviewFeedbackEditor::updateAskMode()
{
    if (!m_ask) return;
    const bool ask = asking();
    m_ask->setVisible(ask);
    m_ask->setEnabled(!comment().isEmpty());
    m_comment->setPlaceholderText(ask ? i18n("Your question for the agent") : m_commentPlaceholder);
    // A question goes to the agent as words and the page: its pictures would not go with it.
    if (m_picturesEnabled && m_new) {
        m_picturesRow->setVisible(!ask);
        if (ask) m_pictureMessage->hide();
    }
    if (isVisible()) adjustSize();
}

void ReviewFeedbackEditor::setChangePassageAvailable(bool available)
{
    // A drawing or a text box has no passage.
    if (m_changePassage) m_changePassage->setVisible(available && !DocumentFeedback::isPageMark(m_record));
}

QString ReviewFeedbackEditor::kind() const
{
    if (DocumentFeedback::isPageMark(m_record)) return DocumentFeedback::kindOf(m_record);
    if (m_question && m_question->isChecked()) return DocumentFeedback::question();
    return m_other && m_other->isChecked() ? DocumentFeedback::other() : QString();
}

QString ReviewFeedbackEditor::polarity() const
{
    if (!kind().isEmpty()) return ReviewCardFeedback::neutral();
    return m_dislike->isChecked() ? ReviewCardFeedback::negative() : ReviewCardFeedback::positive();
}

QString ReviewFeedbackEditor::comment() const
{
    return ReviewCardFeedback::cleanComment(m_comment->toPlainText());
}

QString ReviewFeedbackEditor::feedbackId() const
{
    return m_record.value(QStringLiteral("feedback_id")).toString();
}

bool ReviewFeedbackEditor::isNew() const
{
    return m_new;
}

bool ReviewFeedbackEditor::changed() const
{
    return m_new || kind() != DocumentFeedback::kindOf(m_record) || polarity() != m_record.value(QStringLiteral("polarity")).toString() || comment() != m_record.value(QStringLiteral("comment")).toString()
        || (m_picturesEnabled && m_pictures != DocumentFeedback::picturesOf(m_record));
}

void ReviewFeedbackEditor::keep()
{
    if (std::exchange(m_finished, true)) return;
    // A new question is asked or nothing: never kept in the document.
    if (m_new && asking()) {
        if (!comment().isEmpty()) Q_EMIT unsentQuestion(comment());
        return;
    }
    // A new one with nothing written is nothing to keep, and a question or a comment is its text,
    // or its pictures; a text box is its text alone. A new one's own pictures (a live page's
    // picture of what was shown) are not something the user wrote.
    const QStringList given = m_new ? DocumentFeedback::pictureSha256s(m_record) : QStringList();
    const bool pictures = m_picturesEnabled && std::any_of(m_pictures.cbegin(), m_pictures.cend(), [&given](const QJsonValue &picture) {
                              return !given.contains(picture.toObject().value(QStringLiteral("object_sha256")).toString());
                          });
    if (comment().isEmpty() && (DocumentFeedback::isTextBox(m_record) || (!pictures && (m_new || (!kind().isEmpty() && !DocumentFeedback::isDrawing(m_record))))))
        return;
    if (changed()) Q_EMIT saveRequested(polarity(), comment());
}

void ReviewFeedbackEditor::hideEvent(QHideEvent *event)
{
    // However it goes away: a click elsewhere, Esc, another application. Not while the user
    // chooses pictures: it comes back after.
    if (!m_choosingPicture) keep();
    QFrame::hideEvent(event);
}

void ReviewFeedbackEditor::enablePictures(const PictureStore &store)
{
    if (DocumentFeedback::isDrawing(m_record) || !store.keep || !store.load) return;
    m_store = store;
    m_picturesEnabled = true;
    m_pictures = DocumentFeedback::picturesOf(m_record);
    m_comment->installEventFilter(this);
    m_comment->viewport()->installEventFilter(this);
    // A picture can be dropped on the comment or on the row of pictures.
    m_picturesRow->setAcceptDrops(true);
    m_picturesRow->installEventFilter(this);
    m_picturesRow->show();
    rebuildPictures();
    updateAskMode();
}

QJsonArray ReviewFeedbackEditor::pictures() const
{
    return m_picturesEnabled ? m_pictures : DocumentFeedback::picturesOf(m_record);
}

QByteArray ReviewFeedbackEditor::encodedPicture(const QImage &source, bool png, QString *mediaType)
{
    if (source.isNull()) return {};
    QImage image = source;
    const int side = DocumentFeedback::maximumPictureSide;
    if (image.width() > side || image.height() > side) image = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // JPEG has no transparency: a picture with some stays PNG.
    const bool asPng = png || image.hasAlphaChannel();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool written = asPng ? image.save(&buffer, "PNG") : image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 88);
    if (!written) return {};
    if (mediaType) *mediaType = asPng ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    return bytes;
}

bool ReviewFeedbackEditor::addPictureBytes(const QByteArray &bytes, const QString &mediaType, const QSize &size, QString *error)
{
    if (m_pictures.size() >= DocumentFeedback::maximumPictures) {
        if (error) *error = i18n("A comment holds at most %1 pictures.", DocumentFeedback::maximumPictures);
        return false;
    }
    const QString sha256 = m_store.keep(bytes, error);
    if (sha256.isEmpty()) return false;
    // The same picture twice is one picture.
    for (const QJsonValue &picture : std::as_const(m_pictures)) {
        if (picture.toObject().value(QStringLiteral("object_sha256")).toString() == sha256) return true;
    }
    m_pictures.append(DocumentFeedback::snapshotJson(sha256, mediaType, size.width(), size.height()));
    return true;
}

void ReviewFeedbackEditor::addPictureImage(const QImage &image, bool png)
{
    QString mediaType, error;
    const QByteArray bytes = encodedPicture(image, png, &mediaType);
    QImage kept;
    kept.loadFromData(bytes);
    if (bytes.isEmpty() || kept.isNull()) error = i18n("The picture could not be read.");
    else addPictureBytes(bytes, mediaType, kept.size(), &error);
    m_pictureMessage->setText(error);
    m_pictureMessage->setVisible(!error.isEmpty());
    rebuildPictures();
}

void ReviewFeedbackEditor::addPictureFiles(const QStringList &paths)
{
    QStringList problems;
    for (const QString &path : paths) {
        QImageReader reader(path);
        // As the camera held it.
        reader.setAutoTransform(true);
        const QByteArray format = reader.format().toLower();
        const QImage image = reader.read();
        if (image.isNull()) {
            problems.append(i18n("%1 is not a picture Thoughts can read.", QFileInfo(path).fileName()));
            continue;
        }
        // A screenshot or a drawing stays sharp as PNG; a photo is JPEG.
        const bool png = format == "png" || format == "gif" || format == "bmp";
        QString mediaType, error;
        const QByteArray bytes = encodedPicture(image, png, &mediaType);
        QImage kept;
        kept.loadFromData(bytes);
        if (bytes.isEmpty() || kept.isNull() || !addPictureBytes(bytes, mediaType, kept.size(), &error))
            problems.append(error.isEmpty() ? i18n("%1 could not be added.", QFileInfo(path).fileName()) : error);
        if (m_pictures.size() >= DocumentFeedback::maximumPictures) break;
    }
    problems.removeDuplicates();
    m_pictureMessage->setText(problems.join(QLatin1Char('\n')));
    m_pictureMessage->setVisible(!problems.isEmpty());
    rebuildPictures();
}

bool ReviewFeedbackEditor::hasPicture(const QMimeData *data)
{
    if (!data) return false;
    if (data->hasImage()) return true;
    for (const QUrl &url : data->urls()) {
        if (url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty()) return true;
    }
    return false;
}

void ReviewFeedbackEditor::addPicturesFrom(const QMimeData *data)
{
    QStringList files;
    for (const QUrl &url : data->urls()) {
        if (url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty()) files.append(url.toLocalFile());
    }
    if (!files.isEmpty()) {
        addPictureFiles(files);
        return;
    }
    // A copied screenshot: PNG.
    addPictureImage(qvariant_cast<QImage>(data->imageData()), true);
}

void ReviewFeedbackEditor::choosePictureFiles()
{
    // The editor is a popup: it steps aside while the file dialog is open, then comes back.
    m_choosingPicture = true;
    hide();
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QStringList files = QFileDialog::getOpenFileNames(parentWidget(), i18n("Add Pictures"), folder,
                                                            i18n("Pictures (*.png *.jpg *.jpeg *.heic *.heif *.gif *.webp *.tif *.tiff *.bmp)"));
    m_choosingPicture = false;
    if (!files.isEmpty()) addPictureFiles(files);
    popupAt(m_anchor);
}

void ReviewFeedbackEditor::rebuildPictures()
{
    while (QLayoutItem *item = m_picturesLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    const int thumb = 56;
    for (int index = 0; index < m_pictures.size(); ++index) {
        const QJsonObject picture = m_pictures.at(index).toObject();
        const QString sha256 = picture.value(QStringLiteral("object_sha256")).toString();
        auto *button = new QToolButton(m_picturesRow);
        button->setObjectName(QStringLiteral("reviewFeedbackPicture"));
        button->setFixedSize(thumb + 6, thumb + 6);
        button->setIconSize(QSize(thumb, thumb));
        button->setAutoRaise(true);
        QImage image;
        image.loadFromData(m_store.load(sha256));
        if (image.isNull()) {
            // Made on another device and not here yet: fetched, then shown.
            button->setText(QStringLiteral("…"));
            button->setToolTip(i18n("This picture is not on this computer yet."));
            button->setStyleSheet(QStringLiteral("QToolButton { border: 1px dashed palette(mid); border-radius: 4px; }"));
            if (m_store.fetch && !m_fetching.contains(sha256)) {
                m_fetching.insert(sha256);
                QPointer<ReviewFeedbackEditor> self(this);
                m_store.fetch(sha256, this, [self](bool found) {
                    if (self && found) self->rebuildPictures();
                });
            }
        } else {
            // A square of its middle, so every thumbnail is the same size and its × sits on its corner.
            const QImage filled = image.scaled(thumb * 2, thumb * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            const QImage square = filled.copy((filled.width() - thumb * 2) / 2, (filled.height() - thumb * 2) / 2, thumb * 2, thumb * 2);
            button->setIcon(QPixmap::fromImage(square));
            button->setToolTip(i18n("Open the picture"));
            connect(button, &QToolButton::clicked, this, [this, picture] { openPicture(picture); });
        }
        auto *remove = new QToolButton(button);
        remove->setObjectName(QStringLiteral("reviewFeedbackRemovePicture"));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(i18n("Remove the picture"));
        remove->setFixedSize(18, 18);
        remove->setStyleSheet(QStringLiteral("QToolButton { border-radius: 9px; background: rgba(0, 0, 0, 0.55); color: white; font-weight: 600; padding: 0; }"));
        // On the picture's top right corner (the icon is centred in the button).
        remove->move(button->width() - (button->width() - thumb) / 2 - remove->width() + 4, (button->height() - thumb) / 2 - 4);
        connect(remove, &QToolButton::clicked, this, [this, sha256] {
            for (int i = 0; i < m_pictures.size(); ++i) {
                if (m_pictures.at(i).toObject().value(QStringLiteral("object_sha256")).toString() == sha256) {
                    m_pictures.removeAt(i);
                    break;
                }
            }
            m_pictureMessage->hide();
            rebuildPictures();
        });
        m_picturesLayout->addWidget(button);
    }
    if (m_pictures.size() < DocumentFeedback::maximumPictures) {
        auto *add = new QToolButton(m_picturesRow);
        add->setObjectName(QStringLiteral("reviewFeedbackAddPicture"));
        add->setText(i18n("Add Picture…"));
        add->setToolTip(i18n("Add a picture to this comment. You can also paste one (%1) or drop one here.", QKeySequence(QKeySequence::Paste).toString(QKeySequence::NativeText)));
        add->setAutoRaise(true);
        connect(add, &QToolButton::clicked, this, &ReviewFeedbackEditor::choosePictureFiles);
        m_picturesLayout->addWidget(add);
    }
    m_picturesLayout->addStretch(1);
    if (isVisible()) adjustSize();
}

void ReviewFeedbackEditor::openPicture(const QJsonObject &picture)
{
    const QString sha256 = picture.value(QStringLiteral("object_sha256")).toString();
    const QByteArray bytes = m_store.load(sha256);
    if (bytes.isEmpty()) return;
    // A copy with its type's extension, for the picture viewer.
    const QString folder = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("thoughts-pictures"));
    const QString path = QDir(folder).filePath(sha256 + (picture.value(QStringLiteral("media_type")).toString() == QLatin1String("image/png") ? QStringLiteral(".png") : QStringLiteral(".jpg")));
    if (!QFileInfo::exists(path)) {
        QSaveFile file(path);
        if (!QDir().mkpath(folder) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool ReviewFeedbackEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_picturesEnabled) return QFrame::eventFilter(watched, event);
    // ⌘V with a picture on the clipboard adds it; text pastes as text.
    if (watched == m_comment && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->matches(QKeySequence::Paste)) {
        const QMimeData *data = QApplication::clipboard()->mimeData();
        if (hasPicture(data)) {
            addPicturesFrom(data);
            return true;
        }
    }
    // A picture dropped on the comment is added to it.
    if ((watched == m_comment->viewport() || watched == m_picturesRow)
        && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        if (hasPicture(drop->mimeData())) {
            drop->acceptProposedAction();
            if (event->type() == QEvent::Drop) addPicturesFrom(drop->mimeData());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ReviewFeedbackEditor::mousePressEvent(QMouseEvent *event)
{
    // One click opens it, so the second click of a double click must not close it again.
    if (!rect().contains(event->position().toPoint()) && m_shown.isValid() && m_shown.elapsed() < QApplication::doubleClickInterval()) {
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ReviewFeedbackEditor::popupAt(const QRect &anchor)
{
    m_anchor = anchor;
    adjustSize();
    QPoint position(anchor.left(), anchor.bottom() + 6);
    // A passage taller than the screen has its centre below it (24 Nov 2026: a Dislike of a paragraph, its
    // figure and caption): then the screen of its top, of the pointer, of the window, so the editor never
    // opens out of sight.
    const QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (!screen) screen = QGuiApplication::screenAt(anchor.topLeft());
    if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen && parentWidget()) screen = parentWidget()->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (position.y() + height() > available.bottom()) position.setY(anchor.top() - height() - 6);
        position.setX(std::clamp(position.x(), available.left(), std::max(available.left(), available.right() - width())));
        position.setY(std::clamp(position.y(), available.top(), std::max(available.top(), available.bottom() - height())));
    }
    move(position);
    show();
    m_shown.start();
    m_comment->setFocus();
}
}
