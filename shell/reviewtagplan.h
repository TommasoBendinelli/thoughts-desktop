// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "reviewstore.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

/**
 * A scripted reorganization of the review tags (`thoughtsctl review-tags plan|apply FILE`):
 * merges of several tags into one and the complete group layout, written as the tag picker and
 * Compact tags write them (ReviewStore::recordTagMerge and ReviewStore::saveTagGroups), so they
 * sync like edits made by hand.
 *
 * The plan file (schema_version 1):
 *   {"schema_version": 1,
 *    "groups": [{"name": "Understanding",
 *                "previous_names": ["Effort / status"],   // optional: keep that group's ID
 *                "group_id": "...",                       // optional: keep this group's ID
 *                "section": "Work left",                  // optional: see below
 *                "phase": "before",                       // optional: see below
 *                "scale": [{"tag": "Partly", "from": ["I kind of get it", ...]}, ...],
 *                "tags":  ["Asked AI", {"tag": "Skimmed", "from": ["Read it fast"]}]}],
 *    "knowledge_levels": {"Partly": "shaky", ...}}         // optional
 * Each "from" becomes one permanent merge into its tag. Groups are written in the order given;
 * a group of the current layout the plan does not name is removed (its tags show under Other).
 * A group is matched to the current one with the same group_id, name or a previous name.
 * "section" puts the group under a section header (ReviewTags::TagGroup::section): adjacent
 * groups with the same section show together. Left out, a matched group keeps its current
 * section and a new one has none; "" or null takes the group out of its section.
 * "phase" says when the group is asked (ReviewTags::TagGroup::phase): "before" the answer is
 * shown, "before_and_after", or "after" ("", null) only. Left out, a matched group keeps its
 * current phase and a new one is asked after.
 */
namespace Thoughts::ReviewTagPlan
{
struct Merge {
    QString to;
    QStringList from;
};
struct Group {
    QString name;
    QString groupId;
    QStringList previousNames;
    // Unset: keep the matched group's section; empty: no section.
    std::optional<QString> section;
    // Unset: keep the matched group's phase; empty: asked after the answer.
    std::optional<QString> phase;
    QStringList scale;
    QStringList tags; // Free tags, not steps.
};
struct Plan {
    QList<Merge> merges;
    QList<Group> groups;
    QList<QPair<QString, QString>> levels; // In file order; empty when the plan has none.
};

// False (and @p error) when @p document is not a valid plan.
bool parse(const QJsonObject &document, Plan *plan, QString *error);

// What applying @p plan would do with these @p cards (active, not deleted), the current merges
// and the current @p groups (ReviewStore::tagGroups()). The report (schema_version 1) lists
// every merge with the cards and answers it affects, the resulting groups, groups removed, tags
// in the data no group of the plan covers, and problems (merge sources no answer or group has).
// @p mergesToRecord and @p groupsToSave receive the writes; empty when already applied.
// A plan that cannot be applied gives accepted false with error_code and message.
QJsonObject evaluate(const Plan &plan, const QList<ReviewCard> &cards, const QHash<QString, QString> &merges,
                     const QList<ReviewTags::TagGroup> &groups, QList<Merge> *mergesToRecord = nullptr,
                     QList<ReviewTags::TagGroup> *groupsToSave = nullptr);

// Records the merges and saves the layout through @p store, then reports what was written
// (the evaluate() report with "applied": {...}). Running it again writes nothing.
QJsonObject apply(const Plan &plan, ReviewStore &store);

// The report as text for a terminal.
QString formatReport(const QJsonObject &report);
}
