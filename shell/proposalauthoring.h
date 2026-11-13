/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_PROPOSALAUTHORING_H
#define THOUGHTS_PROPOSALAUTHORING_H
#include <QJsonObject>
#include <QString>
namespace Thoughts {
// Local source mutation only: never accepts a proposal or starts publication/Sync.
// remove-proposal records a `withdraw` decision claim (§10.4.2) with `origin`
// (thoughtsctl or mcp) in `claimDatabase`, by default the Library's sync database.
QJsonObject authorProposal(const QString &docsRoot, const QJsonObject &request, const QString &origin = QStringLiteral("thoughtsctl"),
                           const QString &claimDatabase = {});
// The proposal commands' gate after the Sync before writing. @p before is that
// Sync's terminal command result, or empty when it was skipped because one
// succeeded recently. Only the sources gate the write: when they are not
// synchronized nothing is written (error_code sync_before_failed); a PDF build
// that failed after they synced is returned in "warnings", since the proposal
// may be what fixes the build.
struct CardMoveContext;
QJsonObject authorProposalAfterSync(const QString &docsRoot, const QJsonObject &request, const QString &origin,
                                    const QJsonObject &before, const QString &syncError = {}, const CardMoveContext *card = nullptr);

// move-card-passage (protocol/agent-proposals.md §12): what the caller knows of
// the card, from the review index and the app's server capabilities.
struct CardMoveContext {
    // The server decides both halves of a move pair together (review_card_move_pairs_v1).
    bool movePairsSupported = false;
    // "" when the card can move by its markers; else the refusal code and message.
    QString refusal;
    QString refusalMessage;
    // Library-relative source holding the card's markers.
    QString sourceFile;
    QString question;
};
// Moves a review card linked to source markers: two pending proposals, one
// removing the marked passage with its markers ("<base>.cut") and one inserting
// it before `line` of `source` ("<base>.paste"), in one write. Nothing moves
// until the user accepts; either decision decides both halves.
QJsonObject authorCardMove(const QString &docsRoot, const QJsonObject &request, const QString &origin, const CardMoveContext &card,
                           const QString &claimDatabase = {});
}
#endif
