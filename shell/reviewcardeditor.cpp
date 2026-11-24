/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "reviewcardeditor.h"
#include "../part/reviewmaskgeometry.h"
#include "../part/reviewplacement.h"

#include "../part/thoughtsplatform.h"

#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <poppler-qt6.h>

namespace Thoughts
{
namespace
{
struct Region { int page; QRectF rect; };

class RegionCanvas : public QWidget
{
public:
    explicit RegionCanvas(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("reviewCardRegionCanvas"));
        setMouseTracking(true);
        setMinimumSize(340, 360);
        setAccessibleName(i18n("Answer areas in the PDF"));
    }
    QList<Region> areas;
    int page = 1;
    int selected = -1;
    bool edited = false;
    QImage image;
    std::function<void()> changed;

    QRectF pageBounds() const {
        QSizeF size = image.size();
        size.scale(QSizeF(width() - 24, height() - 24), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }
    QRectF displayRect(const QRectF &rect) const {
        const auto bounds = pageBounds();
        return QRectF(bounds.left() + rect.left() * bounds.width(), bounds.top() + rect.top() * bounds.height(), rect.width() * bounds.width(), rect.height() * bounds.height());
    }
    void removeSelected() {
        if (selected < 0 || selected >= areas.size()) return;
        areas.removeAt(selected);
        selected = -1;
        edited = true;
        update();
        if (changed) changed();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Mid));
        if (image.isNull()) return;
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(pageBounds(), image);
        const QColor accent = palette().color(QPalette::Highlight);
        for (int i = 0; i < areas.size(); ++i) {
            if (areas[i].page != page) continue;
            const QRectF box = displayRect(areas[i].rect);
            painter.setPen(QPen(accent, i == selected ? 2 : 1));
            painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 24));
            painter.drawRect(box);
            if (i == selected) {
                painter.setBrush(palette().brush(QPalette::Base));
                for (const auto point : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}) painter.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
            }
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || image.isNull() || !pageBounds().contains(event->position())) return;
        start = normalized(event->position());
        mode = 0;
        if (selected >= 0 && areas[selected].page == page) {
            const auto box = displayRect(areas[selected].rect);
            const QList<QPointF> corners{box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()};
            for (int corner = 0; corner < corners.size(); ++corner) {
                if (QRectF(corners[corner] - QPointF(9, 9), QSizeF(18, 18)).contains(event->position())) mode = corner + 2;
            }
        }
        if (!mode) {
            selected = -1;
            for (int i = areas.size() - 1; i >= 0; --i) {
                if (areas[i].page == page && areas[i].rect.contains(start)) { selected = i; mode = 1; break; }
            }
        }
        adding = selected < 0;
        if (adding) { areas.append({page, QRectF(start, start)}); selected = areas.size() - 1; mode = 6; }
        original = areas[selected].rect;
        dragging = true;
        update();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!dragging) return;
        const QPointF point = normalized(event->position());
        QRectF box = original;
        if (mode == 1) {
            box.moveTopLeft(QPointF(qBound(0.0, original.left() + point.x() - start.x(), 1.0 - original.width()),
                                   qBound(0.0, original.top() + point.y() - start.y(), 1.0 - original.height())));
        } else if (mode == 6) box = QRectF(start, point).normalized();
        else {
            const QList<QPointF> fixed{original.bottomRight(), original.bottomLeft(), original.topRight(), original.topLeft()};
            box = QRectF(fixed.at(mode - 2), point).normalized();
        }
        areas[selected].rect = box;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!dragging || event->button() != Qt::LeftButton) return;
        mouseMoveEvent(event);
        dragging = false;
        const auto box = areas[selected].rect;
        if (box.width() < .003 || box.height() < .003) {
            if (adding) { areas.removeAt(selected); selected = -1; }
            else areas[selected].rect = original;
        } else if (adding || box != original) edited = true;
        update();
        if (changed) changed();
    }
private:
    QPointF normalized(QPointF point) const {
        const auto box = pageBounds();
        return {qBound(0.0, (point.x() - box.left()) / box.width(), 1.0), qBound(0.0, (point.y() - box.top()) / box.height(), 1.0)};
    }
    bool dragging = false;
    bool adding = false;
    int mode = 0;
    QPointF start;
    QRectF original;
};

class PreviewScrollArea : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;
    double zoom = 1;
    void fitCanvas() {
        if (widget()) widget()->resize(qRound(viewport()->width() * zoom), qRound(viewport()->height() * zoom));
    }
protected:
    void resizeEvent(QResizeEvent *event) override { QScrollArea::resizeEvent(event); fitCanvas(); }
};
}

void fillReviewCardKindChoice(QComboBox *choice, const ReviewCard &card)
{
    if (!choice) return;
    const QSignalBlocker blocker(choice);
    choice->clear();
    choice->addItem(i18n("Knowledge card"), ReviewKinds::knowledge());
    choice->addItem(i18n("Project card"), ReviewKinds::project());
    if (!card.kindProjectIds.isEmpty())
        choice->addItem(card.projectsKind == ReviewKinds::project() ? i18n("As its Project (Project card)") : i18n("As its Project (Knowledge card)"),
                        ReviewKinds::followProject());
    choice->setToolTip(i18n("A knowledge card is material you study. A project card is about work in progress: it comes back at most a week after "
                            "its latest answer, and at once when its passage changes."));
    const QString initial = card.reviewKindSource == ReviewKinds::sourceProject() && !card.kindProjectIds.isEmpty() ? ReviewKinds::followProject()
        : card.isProjectCard() ? ReviewKinds::project() : ReviewKinds::knowledge();
    choice->setProperty("thoughtsInitialKind", initial);
    choice->setCurrentIndex(qMax(0, choice->findData(initial)));
}

QString chosenReviewCardKind(const QComboBox *choice)
{
    if (!choice) return {};
    const QString chosen = choice->currentData().toString();
    return chosen != choice->property("thoughtsInitialKind").toString() ? chosen : QString();
}

struct ReviewCardEditor::Private {
    std::unique_ptr<Poppler::Document> pdf;
    QVariantList originalRegions;
    RegionCanvas *canvas = nullptr;
    QSpinBox *page = nullptr;
    QPlainTextEdit *question = nullptr;
    QComboBox *kind = nullptr;
    QLabel *error = nullptr;
    QPushButton *save = nullptr;
};

ReviewCardEditor::ReviewCardEditor(const ReviewCard &card, const QString &pdfPath, SaveHandler save, QWidget *parent)
    : ReviewCardEditor(card, pdfPath, std::move(save), ReviewCardEditorOptions(), parent)
{
}

ReviewCardEditor::ReviewCardEditor(const ReviewCard &card, const QString &pdfPath, SaveHandler save, const ReviewCardEditorOptions &options, QWidget *parent)
    : QDialog(parent), d(std::make_unique<Private>())
{
    setObjectName(QStringLiteral("reviewCardEditor"));
    setWindowTitle(options.title.isEmpty() ? i18n("Edit Review Card") : options.title);
    resize(1000, 850);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18n("Question"), this));
    d->question = new QPlainTextEdit(card.question(), this);
    d->question->setObjectName(QStringLiteral("reviewCardQuestion"));
    d->question->setMaximumHeight(90);
    layout->addWidget(d->question);
    if (options.offerKind) {
        // The card's kind, as in the review's Edit (the Cards list's menu no longer has it, 23 Nov 2026).
        auto *kindRow = new QHBoxLayout;
        kindRow->addWidget(new QLabel(i18n("Kind"), this));
        d->kind = new QComboBox(this);
        d->kind->setObjectName(QStringLiteral("reviewCardKind"));
        fillReviewCardKindChoice(d->kind, card);
        kindRow->addWidget(d->kind);
        kindRow->addStretch();
        layout->addLayout(kindRow);
    }
    auto *hint = new QLabel(i18n("Drag an area to move it, drag a corner to resize it, or drag on the page to add an area."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(i18n("Page"), this));
    auto *page = new QSpinBox(this);
    page->setObjectName(QStringLiteral("reviewCardPage"));
    row->addWidget(page);
    row->addWidget(new QLabel(i18n("Zoom"), this));
    auto *zoom = new QComboBox(this);
    zoom->setObjectName(QStringLiteral("reviewCardZoom"));
    zoom->addItem(i18n("Fit page"), 1.0);
    zoom->addItem(QStringLiteral("150%"), 1.5);
    zoom->addItem(QStringLiteral("200%"), 2.0);
    row->addWidget(zoom);
    row->addStretch();
    auto *wholeDocument = new QPushButton(i18n("Whole document"), this);
    wholeDocument->setObjectName(QStringLiteral("reviewCardWholeDocument"));
    wholeDocument->setToolTip(i18n("One area on each page around its text, from below the title to the end of the document."));
    row->addWidget(wholeDocument);
    auto *remove = new QPushButton(i18n("Remove selected area"), this);
    remove->setObjectName(QStringLiteral("reviewCardRemoveArea"));
    row->addWidget(remove);
    layout->addLayout(row);
    d->canvas = new RegionCanvas(this);
    auto *preview = new PreviewScrollArea(this);
    preview->setWidget(d->canvas);
    preview->setFrameShape(QFrame::NoFrame);
    layout->addWidget(preview, 1);
    connect(zoom, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, preview, zoom] {
        preview->zoom = zoom->currentData().toDouble();
        preview->fitCanvas();
        if (d->canvas->selected >= 0) {
            const QPoint center = d->canvas->displayRect(d->canvas->areas[d->canvas->selected].rect).center().toPoint();
            preview->ensureVisible(center.x(), center.y(), 80, 80);
        }
    });
    auto *scope = new QLabel(options.scopeText.isEmpty() ? i18n("Area edits apply to this PDF version.") : options.scopeText, this);
    scope->setObjectName(QStringLiteral("reviewCardEditScope"));
    scope->setWordWrap(true);
    layout->addWidget(scope);
    d->error = new QLabel(this);
    d->error->setObjectName(QStringLiteral("reviewCardEditError"));
    d->error->setWordWrap(true);
    layout->addWidget(d->error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    d->save = buttons->button(QDialogButtonBox::Save);
    if (!options.saveText.isEmpty()) d->save->setText(options.saveText);
    d->save->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QFile file(pdfPath);
    QByteArray data;
    if (file.open(QIODevice::ReadOnly)) data = file.readAll();
    const QString expectedPdf = options.pdfSha256.isEmpty() ? card.pdfSha256() : options.pdfSha256;
    if (data.isEmpty() || QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != expectedPdf) {
        d->error->setText(i18n("The PDF changed or is unavailable. Reopen this card after updating its location."));
        return;
    }
    d->pdf = Poppler::Document::loadFromData(data);
    if (!d->pdf || d->pdf->isLocked()) { d->error->setText(i18n("This PDF cannot be opened for editing.")); return; }
    d->pdf->setRenderHint(Poppler::Document::Antialiasing);
    d->pdf->setRenderHint(Poppler::Document::TextAntialiasing);
    d->originalRegions = options.useAreas ? options.areas : card.regions();
    if (options.useAreas) {
        for (const auto &value : std::as_const(d->originalRegions)) {
            const auto region = value.toMap();
            const int number = region.value(QStringLiteral("page")).toInt();
            const auto rect = QJsonArray::fromVariantList(region.value(QStringLiteral("rect")).toList());
            if (number < 1 || number > d->pdf->numPages() || !validNormalizedReviewCoordinates(rect, 4)) {
                d->error->setText(i18n("The answer location needs updating before it can be edited.")); return;
            }
            d->canvas->areas.append({number, QRectF(QPointF(rect[0].toDouble(), rect[1].toDouble()), QPointF(rect[2].toDouble(), rect[3].toDouble()))});
        }
    }
    QMap<int, QList<Okular::NormalizedRect>> pages;
    for (const auto &value : options.useAreas ? QVariantList() : d->originalRegions) {
        const auto region = value.toMap();
        const int number = region.value(QStringLiteral("page")).toInt();
        const auto rect = QJsonArray::fromVariantList(region.value(QStringLiteral("rect")).toList());
        if (number < 1 || number > d->pdf->numPages() || !validNormalizedReviewCoordinates(rect, 4)) {
            d->error->setText(i18n("The answer location needs updating before it can be edited.")); return;
        }
        pages[number].append(Okular::NormalizedRect(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble()));
    }
    for (auto it = pages.cbegin(); it != pages.cend(); ++it) {
        const auto pdfPage = d->pdf->page(it.key() - 1);
        const auto size = pdfPage->pageSizeF();
        for (const auto &box : continuousReviewMaskBlocks(it.value(), size.height() / size.width()))
            d->canvas->areas.append({it.key(), QRectF(QPointF(box.left, box.top), QPointF(box.right, box.bottom))});
    }
    const auto validate = [this, remove] {
        const auto text = question();
        d->save->setEnabled(!d->canvas->image.isNull() && !text.isEmpty() && text.size() <= 500 && !regions().isEmpty());
        remove->setEnabled(d->canvas->selected >= 0);
    };
    const auto showPage = [this, validate](int number) {
        d->canvas->page = number;
        d->canvas->selected = -1;
        for (int i = 0; i < d->canvas->areas.size(); ++i) if (d->canvas->areas[i].page == number) { d->canvas->selected = i; break; }
        d->canvas->image = d->pdf->page(number - 1)->renderToImage(144, 144);
        d->canvas->update();
        validate();
    };
    page->setRange(1, d->pdf->numPages());
    page->setSuffix(i18n(" of %1", d->pdf->numPages()));
    page->setValue(qBound(1, options.page > 0 ? options.page : card.page(), d->pdf->numPages()));
    d->page = page;
    connect(page, qOverload<int>(&QSpinBox::valueChanged), this, showPage);
    connect(d->question, &QPlainTextEdit::textChanged, this, validate);
    d->canvas->changed = validate;
    connect(remove, &QPushButton::clicked, this, [this] { d->canvas->removeSelected(); });
    // The whole document: one area per page around its text, the title left visible.
    connect(wholeDocument, &QPushButton::clicked, this, [this, page, showPage] {
        if (!d->pdf) return;
        const auto areas = wholeDocumentReviewAreas(d->pdf.get());
        if (areas.isEmpty()) {
            d->error->setText(i18n("No text was found in this PDF to cover."));
            return;
        }
        d->canvas->areas.clear();
        for (const auto &area : areas) d->canvas->areas.append({area.page, area.rect});
        if (page->value() == areas.first().page) showPage(areas.first().page);
        else page->setValue(areas.first().page);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this, save] {
        QString error;
        if (!save || !save(regions(), question(), d->pdf->numPages(), &error)) {
            d->error->setText(error.isEmpty() ? i18n("The changes could not be saved.") : error);
            return;
        }
        accept();
    });
    showPage(page->value());
}

ReviewCardEditor::~ReviewCardEditor() = default;
QVariantList ReviewCardEditor::regions() const {
    if (!d->canvas->edited) return d->originalRegions;
    return areas();
}
QVariantList ReviewCardEditor::areas() const {
    QVariantList result;
    for (const auto &area : d->canvas->areas) result.append(QVariantMap{{QStringLiteral("page"), area.page},
        {QStringLiteral("rect"), QVariantList{area.rect.left(), area.rect.top(), area.rect.right(), area.rect.bottom()}}});
    return result;
}
QString ReviewCardEditor::question() const { return d->question->toPlainText().trimmed(); }
QString ReviewCardEditor::kind() const { return chosenReviewCardKind(d->kind); }
void ReviewCardEditor::showArea(int index) {
    if (index < 0 || index >= d->canvas->areas.size() || !d->page) return;
    d->page->setValue(d->canvas->areas[index].page);
    d->canvas->selected = index;
    d->canvas->update();
    if (d->canvas->changed) d->canvas->changed();
}
void ReviewCardEditor::setError(const QString &error) { d->error->setText(error); }

QList<ReviewArea> reviewAreasFromRegions(const QVariantList &regions, const QList<ReviewArea> &textSelections) {
    QList<ReviewArea> areas;
    for (const auto &area : reviewAreasFromRegions(regions, ReviewArea{})) {
        ReviewArea kept = area;
        for (const auto &selection : textSelections) {
            if (kept.textSelection) break;
            kept.textSelection = reviewAreasFromRegions(QVariantList{QVariantMap{{QStringLiteral("page"), area.page},
                {QStringLiteral("rect"), QVariantList{area.rect.left(), area.rect.top(), area.rect.right(), area.rect.bottom()}}}}, selection).constFirst().textSelection;
        }
        areas.append(kept);
    }
    return areas;
}

QList<ReviewArea> reviewAreasFromRegions(const QVariantList &regions, const ReviewArea &textSelection) {
    QList<ReviewArea> areas;
    for (const auto &value : regions) {
        const auto region = value.toMap();
        const auto rect = region.value(QStringLiteral("rect")).toList();
        if (rect.size() != 4) continue;
        ReviewArea area{region.value(QStringLiteral("page")).toInt(),
            QRectF(QPointF(rect[0].toDouble(), rect[1].toDouble()), QPointF(rect[2].toDouble(), rect[3].toDouble())).normalized()};
        const auto close = [](const QRectF &a, const QRectF &b) {
            return qAbs(a.left() - b.left()) < 1e-6 && qAbs(a.top() - b.top()) < 1e-6 && qAbs(a.right() - b.right()) < 1e-6 && qAbs(a.bottom() - b.bottom()) < 1e-6;
        };
        area.textSelection = textSelection.textSelection && textSelection.page == area.page && close(textSelection.rect.normalized(), area.rect);
        areas.append(area);
    }
    return areas;
}

QString reviewAreaFailureText(const QList<ReviewArea> &areas, int failedArea, const QString &reason) {
    if (failedArea < 0 || failedArea >= areas.size())
        return i18n("The card can't be linked to source text: %1", reason);
    if (areas.size() == 1)
        return i18n("The area on page %1 can't be linked to source text: %2", areas[failedArea].page, reason);
    return i18n("Area %1 of %2 (page %3) can't be linked to source text: %4", failedArea + 1, areas.size(), areas[failedArea].page, reason);
}

ReviewAreaFailureChoice askAboutUnlinkedArea(QWidget *parent, const QString &text, bool allowByHand, bool allowServerLink) {
    QMessageBox box(QMessageBox::Warning, i18n("Can't Link Review Card"), text, QMessageBox::NoButton, parent);
    box.setObjectName(QStringLiteral("reviewAreaFailure"));
    box.setInformativeText(allowByHand
        ? i18n("Adjust the areas to cover complete passages, or save the card and place its source markers by hand.")
        : i18n("Adjust the areas to cover complete passages of the source."));
    auto *adjust = box.addButton(i18n("Adjust Areas"), QMessageBox::AcceptRole);
    adjust->setObjectName(QStringLiteral("reviewAreaAdjust"));
    QPushButton *byHand = nullptr;
    if (allowByHand) {
        byHand = box.addButton(i18n("Save and Place Markers by Hand"), QMessageBox::DestructiveRole);
        byHand->setObjectName(QStringLiteral("reviewAreaPlaceByHand"));
    }
    QPushButton *serverLink = nullptr;
    if (allowServerLink) {
        serverLink = box.addButton(i18n("Save; Link at Next Sync"), QMessageBox::ActionRole);
        serverLink->setObjectName(QStringLiteral("reviewAreaLinkAtSync"));
        serverLink->setToolTip(i18n("This computer has no source map for this PDF yet. The server links the card at the next Sync; it stays out of reviews until then."));
    }
    auto *cancel = box.addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("reviewAreaCancel"));
    box.setDefaultButton(adjust);
    box.exec();
    if (box.clickedButton() == adjust) return ReviewAreaFailureChoice::AdjustAreas;
    if (byHand && box.clickedButton() == byHand) return ReviewAreaFailureChoice::PlaceByHand;
    if (serverLink && box.clickedButton() == serverLink) return ReviewAreaFailureChoice::LinkAtNextSync;
    return ReviewAreaFailureChoice::Cancel;
}

bool planReviewAreasWithProgress(QWidget *parent, const std::function<ReviewAnchorPlan()> &work, ReviewAnchorPlan *plan) {
    auto result = std::make_shared<ReviewAnchorPlan>();
    QThread *thread = QThread::create([work, result] { *result = work(); });
    QProgressDialog progress(i18n("Linking the areas to the source…"), i18n("Cancel"), 0, 0, parent);
    progress.setObjectName(QStringLiteral("reviewAreaProgress"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    progress.setAutoClose(false);
    QEventLoop loop;
    bool cancelled = false;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] { cancelled = true; loop.quit(); });
    thread->start();
    if (!thread->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    // The work cannot be interrupted: wait for it, but forget its result after Cancel.
    thread->wait();
    delete thread;
    // Closing a progress dialog reports a cancel; only the user's counts.
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    if (cancelled) return false;
    *plan = *result;
    return true;
}

namespace {
bool sameRegions(const QVariantList &a, const QVariantList &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        const auto x = a[i].toMap(), y = b[i].toMap();
        if (x.value(QStringLiteral("page")).toInt() != y.value(QStringLiteral("page")).toInt()) return false;
        const auto r = x.value(QStringLiteral("rect")).toList(), s = y.value(QStringLiteral("rect")).toList();
        if (r.size() != 4 || s.size() != 4) return false;
        for (int j = 0; j < 4; ++j) if (qAbs(r[j].toDouble() - s[j].toDouble()) > 1e-9) return false;
    }
    return true;
}
QString fileSha256(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}
}

QString defaultReviewSourceBundleRoot() {
    return QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle"));
}

bool canPlaceReviewCardByAreas(const ReviewCard &card) {
    return !card.isWholeDocument() && !isBasicReviewCard(card.record);
}

bool reviewCardNeedsPlacingAgain(const ReviewCard &card) {
    if (!canPlaceReviewCardByAreas(card)) return false;
    if (card.linkingState() == QLatin1String("linking_failed")) return true;
    const auto reanchor = card.record.value(QStringLiteral("reanchor")).toObject();
    static const QStringList codes{QStringLiteral("review_card_place_again"), QStringLiteral("review_source_link_unresolved"),
        QStringLiteral("review_source_marker_missing_or_ambiguous")};
    return reanchor.value(QStringLiteral("state")).toString() == QLatin1String("needs_attention")
        && (codes.contains(reanchor.value(QStringLiteral("error_code")).toString())
            || reanchor.value(QStringLiteral("strategy")).toString() == QLatin1String("manual_source_link_v1"));
}

ReviewAnchorPlan reviewPlanForServerLink(ReviewAnchorPlan plan, const QList<ReviewArea> &areas, const QString &pdfPath, const QString &pdfSha256) {
    plan.linkOnServer = true;
    plan.regions = {};
    for (const auto &area : areas) {
        const QRectF r = area.rect.normalized();
        plan.regions.append(QJsonObject{{QStringLiteral("page"), area.page}, {QStringLiteral("rect"), QJsonArray{r.left(), r.top(), r.right(), r.bottom()}}});
    }
    if (!areas.isEmpty())
        plan.pageVisualSha256 = fingerprintReviewPage(pdfPath, areas.first().page, pdfSha256).value(QStringLiteral("page_visual_sha256")).toString();
    return plan;
}

bool chooseNewReviewCardAreas(QWidget *parent, const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                              const QString &pdfSha256, const QString &cardId, const ReviewArea &first, const QString &question,
                              NewReviewCardAreas *result) {
    return chooseNewReviewCardAreas(parent, docsRoot, bundleRoot, pdfPath, pdfSha256, cardId, QList<ReviewArea>{first}, question, result);
}

bool chooseNewReviewCardAreas(QWidget *parent, const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                              const QString &pdfSha256, const QString &cardId, const QList<ReviewArea> &initial, const QString &question,
                              NewReviewCardAreas *result) {
    if (initial.isEmpty()) return false;
    const ReviewArea &first = initial.constFirst();
    const QRectF rect = first.rect.normalized();
    const QJsonArray firstRect{rect.left(), rect.top(), rect.right(), rect.bottom()};
    const QJsonObject primary{{QStringLiteral("pdf_sha256"), pdfSha256}, {QStringLiteral("page"), first.page},
        {QStringLiteral("rect"), firstRect}, {QStringLiteral("question"), question},
        {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), first.page}, {QStringLiteral("rect"), firstRect}}}}};
    const ReviewCard seed{QJsonObject{{QStringLiteral("schema_version"), 4}, {QStringLiteral("card_id"), cardId},
        {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("primary"), primary}}, {}};
    ReviewCardEditorOptions options;
    options.title = i18n("Add Review Card");
    options.saveText = i18n("Add Card");
    options.scopeText = i18n("Each area is linked to its own passage of the source. Drag on a page to add an area; choose other pages with Page.");
    options.pdfSha256 = pdfSha256;
    options.useAreas = true;
    options.areas = {};
    for (const auto &area : initial) {
        const QRectF r = area.rect.normalized();
        options.areas.append(QVariantMap{{QStringLiteral("page"), area.page}, {QStringLiteral("rect"), QVariantList{r.left(), r.top(), r.right(), r.bottom()}}});
    }
    options.page = first.page;
    ReviewCardEditor *self = nullptr;
    NewReviewCardAreas chosen;
    ReviewCardEditor editor(seed, pdfPath, [&](const QVariantList &, const QString &text, int, QString *error) {
        const auto areas = reviewAreasFromRegions(self->areas(), initial);
        ReviewAnchorPlan plan;
        if (!planReviewAreasWithProgress(self, [=] { return planReviewSourceAnchors(docsRoot, bundleRoot, pdfPath, pdfSha256, areas, cardId); }, &plan)) {
            *error = i18n("Linking was cancelled.");
            return false;
        }
        if (plan.invalidPdf) { *error = plan.error; return false; }
        if (!plan.error.isEmpty()) {
            const QString message = reviewAreaFailureText(areas, plan.failedArea, plan.error);
            const auto choice = askAboutUnlinkedArea(self, message, areas.size() == 1, plan.sourceMapUnavailable);
            if (choice == ReviewAreaFailureChoice::PlaceByHand) { chosen = {text, plan, areas, true}; return true; }
            if (choice == ReviewAreaFailureChoice::LinkAtNextSync) {
                chosen = {text, reviewPlanForServerLink(plan, areas, pdfPath, pdfSha256), areas, false};
                return true;
            }
            if (choice == ReviewAreaFailureChoice::Cancel) self->reject();
            else self->showArea(plan.failedArea);
            *error = message;
            return false;
        }
        chosen = {text, plan, areas, false};
        return true;
    }, options, parent);
    self = &editor;
    if (editor.exec() != QDialog::Accepted) return false;
    *result = chosen;
    return true;
}

bool editReviewCardAreas(QWidget *parent, ReviewStore &store, const QString &docsRoot, const ReviewCard &card,
                         const QString &pdfPath, bool placeAgain, bool wholeDocument, bool offerKind) {
    const QString pdfSha256 = fileSha256(pdfPath);
    if (pdfSha256.isEmpty()) {
        QMessageBox::warning(parent, i18n("Cannot Edit Review Card"), i18n("The card's PDF is unavailable: %1", pdfPath));
        return false;
    }
    const bool samePdf = pdfSha256 == card.pdfSha256();
    ReviewCardEditorOptions options;
    options.pdfSha256 = pdfSha256;
    if (placeAgain) {
        options.title = i18n("Place Review Card Again");
        options.saveText = i18n("Place Card");
        options.scopeText = i18n("Draw the card's answer areas on the current PDF. Each area marks its own passage of the source.");
    }
    options.offerKind = offerKind;
    if (!samePdf) {
        // Regions of another build would point at the wrong text: start from an empty page.
        options.useAreas = true;
        options.page = card.page();
    }
    if (wholeDocument) {
        const auto areas = wholeDocumentReviewAreas(pdfPath);
        if (areas.isEmpty()) {
            QMessageBox::warning(parent, i18n("Cannot Cover the Whole Document"), i18n("No text was found in the card's PDF."));
            return false;
        }
        options.title = i18n("Cover the Whole Document");
        options.scopeText = i18n("Every page's text below the title is the answer. Adjust an area if needed; the card's markers in the source move with them, and it keeps its reviews.");
        options.useAreas = true;
        options.areas.clear();
        for (const auto &area : areas)
            options.areas.append(QVariantMap{{QStringLiteral("page"), area.page},
                                             {QStringLiteral("rect"), QVariantList{area.rect.left(), area.rect.top(), area.rect.right(), area.rect.bottom()}}});
        options.page = areas.first().page;
    }
    const QString bundleRoot = defaultReviewSourceBundleRoot();
    ReviewCardEditor *self = nullptr;
    ReviewCardEditor editor(card, pdfPath, [&](const QVariantList &regions, const QString &question, int pageCount, QString *error) {
        if (!placeAgain && samePdf && sameRegions(regions, card.regions()))
            return store.updateCardAnswer(card, regions, question, pageCount, error);
        const auto areas = reviewAreasFromRegions(self->areas());
        ReviewAnchorPlan plan;
        if (!planReviewAreasWithProgress(self, [=, id = card.id()] { return planReviewSourceAnchors(docsRoot, bundleRoot, pdfPath, pdfSha256, areas, id, true); }, &plan)) {
            *error = i18n("Linking was cancelled.");
            return false;
        }
        if (plan.invalidPdf) { *error = plan.error; return false; }
        if (plan.withoutSource) {
            // A PDF without editable source keeps its areas on this PDF version.
            if (!samePdf) { *error = i18n("This card belongs to another version of the PDF. Update its location before editing its areas."); return false; }
            return store.updateCardAnswer(card, self->areas(), question, pageCount, error);
        }
        if (!plan.error.isEmpty()) {
            const QString message = reviewAreaFailureText(areas, plan.failedArea, plan.error);
            if (askAboutUnlinkedArea(self, message, false) == ReviewAreaFailureChoice::Cancel) self->reject();
            else self->showArea(plan.failedArea);
            *error = message;
            return false;
        }
        return store.replaceCardAreas(card, pdfSha256, question, plan, error);
    }, options, parent);
    self = &editor;
    if (editor.exec() != QDialog::Accepted) return false;
    if (const QString kind = editor.kind(); !kind.isEmpty()) {
        QString error;
        if (!store.setCardKind(card.id(), kind, &error)) QMessageBox::warning(parent, i18n("Cannot Change the Card's Kind"), error);
    }
    return true;
}
}
