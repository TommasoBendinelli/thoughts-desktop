/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_PROPOSALDECISIONS_H
#define THOUGHTS_PROPOSALDECISIONS_H

#include "agentproposalparser.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

// Decisions and removed proposals, protocol/agent-proposals.md §10.
//
// A local Accept, Reject or withdrawal records a decision claim (§10.4.2) in
// the desktop sync database before the source is written. The publisher
// attaches the claims of the paths it uploads and forgets them once the
// server has answered; a claim whose wrapper is back in the uploaded text
// (a local Undo) is dropped instead. Nothing else keeps decisions.
namespace Thoughts::ProposalDecisions
{

// The claim table in desktop-sync.sqlite (shared with tools/publisher).
inline constexpr char TableName[] = "desktop_proposal_decisions";
// At most this many claims travel with one upload (§10.4.2).
inline constexpr int MaximumClaimsPerUpload = 500;
// Server capability that reads claims (§10.7.1).
inline constexpr char Capability[] = "proposal_decisions_v1";

// The desktop sync database of the Thoughts Library `docsRoot`, or "" when
// `docsRoot` is another folder (nothing synchronizes it, so no claims), or
// a THOUGHTS_HOME Library other than the default one while the data folder is
// the account's own (Platform::isAccountDataRoot): a temporary Library never
// writes the real database. Works in thoughtsctl too, whose own application
// data folder differs.
QString databasePath(const QString &docsRoot);

// Whether a decision in `sourceFile` (relative to `docsRoot`) gets a claim:
// only sources Sync manages are uploaded, so only their claims can ever be
// matched (tools/publisher/managedsource.h). A file under tex/generated/, one
// .ThoughtIgnore excludes or one outside a Project's folders gets none; the
// decision itself proceeds. True when the Project configurations cannot be
// read: the publisher drops the claim later if it does not apply.
bool claimApplies(const QString &docsRoot, const QString &sourceFile);

// §10.3: SHA-256 of the UTF-8 bytes of the wrapper's span.
QString wrapperSha256(const QString &source, const AgentProposedEdit &proposal);

// A new claim: action accept, reject, partial or withdraw; origin desktop,
// vscode, thoughtsctl, mcp or phone_editor; `sourceFile` is relative to the
// Library root.
QJsonObject newClaim(const QString &proposalId, const QString &action, const QString &wrapperSha256,
                     const QString &sourceFile, const QString &origin);
bool isValidClaim(const QJsonObject &claim);

// Durable claims. recordClaims replaces any older claim for the same proposal
// (deciding again means the earlier decision was undone).
bool recordClaims(const QString &database, const QList<QJsonObject> &claims, QString *error = nullptr);
bool forgetClaims(const QString &database, const QStringList &decisionIds, QString *error = nullptr);
// In recording order; every claim when `paths` is empty. A missing database
// or table has no claims.
QList<QJsonObject> storedClaims(const QString &database, QString *error = nullptr, const QSet<QString> &paths = {});

// §10.4.2 attach rule for one upload. `uploads` maps a Library-relative path
// to its uploaded text, or to nullopt for a deletion. Claims of other paths
// wait; a claim whose ID is in the uploaded text (a valid wrapper or an
// issue carrying it) is dropped; the others are sent, at most 500 with
// unique proposal IDs (the rest wait for the next upload).
struct ClaimSelection {
    QList<QJsonObject> sent;
    QStringList dropped; // decision IDs
};
ClaimSelection selectClaimsForUpload(const QList<QJsonObject> &claims, const QHash<QString, std::optional<QString>> &uploads);
QStringList decisionIds(const QList<QJsonObject> &claims);
// The stored claims a source-conflict resolution of `path` with `contents`
// carries (§10.8, "the resolving upload"); claims undone in `contents` are
// forgotten. The resolution's answer expires them (forgetClaims).
QList<QJsonObject> claimsForConflictResolution(const QString &database, const QString &path, const QString &contents, QString *error = nullptr);

// §10.6 Restore Proposal: the edit that puts a removed proposal back.
// `code` is "" with `output` set, "already_present", or a refusal:
// source_missing, id_in_source, ambiguous, text_changed, not_restorable.
struct RestoreEdit {
    QString code;
    QString output;
    int line = 0; // 1-based line of the restored wrapper
};
RestoreEdit restoreEdit(const QString &path, const std::optional<QString> &current, const QJsonObject &snapshot, int line);
QString restoreRefusalMessage(const QString &code);

// Server `proposal` rows (§10.2, §10.3).
bool isRemovedOutside(const QJsonObject &row);
// The Dismiss operation payload of a removed_outside row (§10.6).
QJsonObject dismissedPayload(const QJsonObject &row);
// "Proposal <id> in <file>:<line>" with its explanation, original and proposed texts.
QString copyableText(const QJsonObject &row);

// Restore Proposal on the local source: a verified write through the
// proposal journal, rolled back when the result does not read back as
// exactly this proposal. {success, code, message, source_file, line}.
QJsonObject restoreRemovedProposal(const QString &docsRoot, const QJsonObject &row);

// `thoughtsctl decide-proposal` (§10.7.3), executed by the app. `claimDatabase`
// empty uses databasePath(docsRoot). The answer's `claim` is "recorded",
// "not_applicable" (claimApplies is false) or "not_recorded" (no database).
QJsonObject decideProposal(const QString &docsRoot, const QJsonObject &request, const QString &claimDatabase = {});

}

#endif
