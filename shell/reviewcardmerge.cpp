/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "reviewcardmerge.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace Thoughts
{
namespace
{
constexpr int questionLimit = 500;

QString cardChoiceText(const ReviewCard &card)
{
    QString question = card.question().simplified();
    if (question.size() > 80) question = question.left(77) + QStringLiteral("…");
    if (question.isEmpty()) question = i18n("Card without a question");
    return i18nc("@item:inlistbox card to keep: question, review count", "%1 — %2", question,
                 i18np("1 review", "%1 reviews", card.schedule.reviewCount));
}
}

QString mergeReviewCardsActionText()
{
    return i18n("Merge cards…");
}

bool mergeReviewCards(QWidget *parent, ReviewStore &store, const QList<ReviewCard> &cards, const QString &bundleRoot,
                      ReviewStore::CardMerge *result)
{
    ReviewStore::CardMerge preview;
    if (const QString problem = store.cardMergeProblem(cards, bundleRoot, &preview); !problem.isEmpty()) {
        KMessageBox::error(parent, problem, i18n("Cannot Merge Review Cards"));
        return false;
    }
    // The cards in reading order, as the merged card shows their passages.
    QList<ReviewCard> ordered;
    for (const QString &id : std::as_const(preview.order)) {
        const auto found = std::find_if(cards.cbegin(), cards.cend(), [&id](const ReviewCard &card) { return card.id() == id; });
        if (found != cards.cend()) ordered.append(*found);
    }

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("reviewMergeCardsDialog"));
    dialog.setWindowTitle(i18n("Merge Review Cards"));
    auto *layout = new QVBoxLayout(&dialog);
    // Cards of an imported PDF have no source: their highlighted areas are combined, the PDF stays as it is.
    auto *intro = new QLabel(i18np("These cards become one card. ", "These %1 cards become one card. ", ordered.size())
                                 + (preview.pdf ? i18n("The card you keep keeps its schedule and review history and shows every card's highlighted "
                                                       "areas as its own, in reading order. The other cards are deleted with their history. "
                                                       "The PDF does not change.")
                                                : i18n("The card you keep keeps its schedule and review history and shows every card's passage as its own "
                                                       "answer area, in reading order; text between the passages stays out. The other cards are deleted "
                                                       "with their history.")),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *kept = new QComboBox(&dialog);
    kept->setObjectName(QStringLiteral("reviewMergeKeptCard"));
    for (const ReviewCard &card : std::as_const(ordered)) kept->addItem(cardChoiceText(card), card.id());
    kept->setCurrentIndex(std::max(0, kept->findData(preview.keptId)));
    form->addRow(i18n("Keep:"), kept);
    auto *question = new QPlainTextEdit(&dialog);
    question->setObjectName(QStringLiteral("reviewMergeQuestion"));
    question->setTabChangesFocus(true);
    question->setPlainText(ReviewStore::joinedMergeQuestion(ordered));
    question->setMinimumHeight(90);
    form->addRow(i18n("Question:"), question);
    layout->addLayout(form);
    auto *passages = new QLabel(preview.pdf ? i18np("1 area on the PDF", "%1 areas on the PDF", preview.passages)
                                            : i18np("1 passage in %2", "%1 passages in %2", preview.passages, preview.sourceFile),
                                &dialog);
    passages->setObjectName(QStringLiteral("reviewMergePassages"));
    passages->setWordWrap(true);
    layout->addWidget(passages);
    auto *status = new QLabel(&dialog);
    status->setObjectName(QStringLiteral("reviewMergeStatus"));
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton *merge = buttons->button(QDialogButtonBox::Ok);
    merge->setObjectName(QStringLiteral("reviewMergeButton"));
    merge->setText(i18n("Merge"));
    layout->addWidget(buttons);

    const auto validate = [=] {
        const int length = question->toPlainText().trimmed().size();
        merge->setEnabled(length > 0 && length <= questionLimit);
        status->setText(length == 0 ? i18n("Write the merged card's question.")
                        : length > questionLimit ? i18n("The question is too long: %1 of at most %2 characters.", length, questionLimit)
                                                 : QString());
    };
    QObject::connect(question, &QPlainTextEdit::textChanged, &dialog, validate);
    validate();
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString error;
        ReviewStore::CardMerge merged;
        if (!store.mergeCards(ordered, kept->currentData().toString(), question->toPlainText().trimmed(), bundleRoot, &error, &merged)) {
            status->setText(error.isEmpty() ? i18n("The cards could not be merged.") : error);
            return;
        }
        if (result) *result = merged;
        dialog.accept();
    });
    dialog.resize(620, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted;
}
}
