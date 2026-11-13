/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWCARDMERGE_H
#define THOUGHTS_REVIEWCARDMERGE_H

#include "reviewstore.h"

#include <QList>
#include <QString>

class QWidget;

namespace Thoughts
{
// The menu entry, in Review → Browse and in the reader's Cards list.
QString mergeReviewCardsActionText();

// Merge cards…: asks which card to keep (by default the one reviewed most) and the merged
// question (the cards' questions joined, editable), then merges @p cards with
// ReviewStore::mergeCards. When they cannot be merged (cards of several documents, a passage
// outside the source, a source not synced yet, ...) a message says why and nothing changes.
// True once merged; @p result then says what was done.
bool mergeReviewCards(QWidget *parent, ReviewStore &store, const QList<ReviewCard> &cards, const QString &bundleRoot,
                      ReviewStore::CardMerge *result = nullptr);
}

#endif
