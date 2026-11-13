// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewtagplan.h"

#include <QJsonArray>
#include <QSet>
#include <QTextStream>
#include <QUuid>

#include <algorithm>

namespace Thoughts::ReviewTagPlan
{
namespace
{
const QStringList &levelNames()
{
    static const QStringList names{QStringLiteral("solid"), QStringLiteral("shaky"), QStringLiteral("unknown"), QStringLiteral("ignore")};
    return names;
}

bool onlyKeys(const QJsonObject &object, const QStringList &keys, const QString &where, QString *error)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) {
            *error = QStringLiteral("%1: unknown field \"%2\" (expected %3).").arg(where, it.key(), keys.join(QStringLiteral(", ")));
            return false;
        }
    }
    return true;
}

bool strings(const QJsonValue &value, const QString &where, QStringList *result, QString *error)
{
    if (value.isUndefined()) return true;
    if (!value.isArray()) {
        *error = QStringLiteral("%1 must be a list of strings.").arg(where);
        return false;
    }
    for (const auto &item : value.toArray()) {
        const QString text = item.isString() ? ReviewTags::normalize(item.toString()) : QString();
        if (text.isEmpty()) {
            *error = QStringLiteral("%1 must be a list of non-empty strings.").arg(where);
            return false;
        }
        result->append(text);
    }
    return true;
}

// The merge mapping once @p planned are recorded after the merges already in @p mapping, as
// ReviewTags::mergeMapping reads them (later merges last).
QHash<QString, QString> withMerges(QHash<QString, QString> mapping, const QList<Merge> &planned)
{
    for (const Merge &merge : planned) {
        const QString target = mapping.value(merge.to, merge.to);
        for (const QString &source : merge.from) {
            if (source == target) continue;
            mapping.insert(source, target);
            for (auto it = mapping.begin(); it != mapping.end(); ++it) {
                if (it.value() == source) it.value() = target;
            }
        }
    }
    return mapping;
}

struct Usage {
    QHash<QString, int> cards;   // Cards whose latest answer has the tag.
    QHash<QString, int> answers; // Answers in effect with the tag.
    int answerCount = 0;
};

Usage usageOf(const QList<ReviewCard> &cards, const QHash<QString, QString> &mapping)
{
    Usage usage;
    for (ReviewCard card : cards) {
        card.tagMerges = mapping;
        for (const QString &tag : card.currentTags()) ++usage.cards[tag];
        for (const auto &answer : card.answers()) {
            ++usage.answerCount;
            for (const QString &tag : card.tagsOfEvent(answer)) ++usage.answers[tag];
        }
    }
    return usage;
}

QJsonObject failure(const QString &code, const QString &message)
{
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), false}, {QStringLiteral("terminal"), true},
            {QStringLiteral("error_code"), code}, {QStringLiteral("message"), message}};
}

QString plural(int count, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(count).arg(QString::fromLatin1(count == 1 ? one : many));
}
}

bool parse(const QJsonObject &document, Plan *plan, QString *error)
{
    Plan result;
    if (!onlyKeys(document, {QStringLiteral("schema_version"), QStringLiteral("about"), QStringLiteral("groups"), QStringLiteral("knowledge_levels")},
                  QStringLiteral("plan"), error))
        return false;
    if (document.value(QStringLiteral("schema_version")).toInt() != 1 || !document.value(QStringLiteral("schema_version")).isDouble()) {
        *error = QStringLiteral("The plan needs \"schema_version\": 1.");
        return false;
    }
    if (!document.value(QStringLiteral("groups")).isArray() || document.value(QStringLiteral("groups")).toArray().isEmpty()) {
        *error = QStringLiteral("The plan needs a non-empty \"groups\" list.");
        return false;
    }
    QSet<QString> named, merged;
    for (const auto &value : document.value(QStringLiteral("groups")).toArray()) {
        if (!value.isObject()) {
            *error = QStringLiteral("Each group must be an object.");
            return false;
        }
        const QJsonObject object = value.toObject();
        Group group;
        group.name = object.value(QStringLiteral("name")).toString().simplified();
        const QString where = QStringLiteral("group \"%1\"").arg(group.name);
        if (group.name.isEmpty()) {
            *error = QStringLiteral("Each group needs a \"name\".");
            return false;
        }
        if (!onlyKeys(object, {QStringLiteral("name"), QStringLiteral("about"), QStringLiteral("group_id"), QStringLiteral("previous_names"),
                               QStringLiteral("section"), QStringLiteral("phase"), QStringLiteral("scale"), QStringLiteral("tags")}, where, error))
            return false;
        group.groupId = object.value(QStringLiteral("group_id")).toString();
        if (const QJsonValue section = object.value(QStringLiteral("section")); !section.isUndefined()) {
            if (!section.isString() && !section.isNull()) {
                *error = QStringLiteral("%1: \"section\" must be a string (or null for none).").arg(where);
                return false;
            }
            group.section = section.toString().trimmed();
            if (group.section->toUcs4().size() > ReviewTags::sectionMaxLength()) {
                *error = QStringLiteral("%1: a section name can be at most %2 characters long.").arg(where).arg(ReviewTags::sectionMaxLength());
                return false;
            }
        }
        if (const QJsonValue phase = object.value(QStringLiteral("phase")); !phase.isUndefined()) {
            const QString value = phase.toString();
            if (!(phase.isNull() || (phase.isString() && (value.isEmpty() || value == QLatin1String("after") || value == ReviewTags::beforePhase()
                                                          || value == ReviewTags::beforeAndAfterPhase())))) {
                *error = QStringLiteral("%1: \"phase\" must be \"before\", \"before_and_after\" or \"after\".").arg(where);
                return false;
            }
            group.phase = value == QLatin1String("after") ? QString() : value;
        }
        if (!strings(object.value(QStringLiteral("previous_names")), where + QStringLiteral(" previous_names"), &group.previousNames, error)) return false;
        for (const QString &list : {QStringLiteral("scale"), QStringLiteral("tags")}) {
            const QJsonValue entries = object.value(list);
            if (entries.isUndefined()) continue;
            if (!entries.isArray()) {
                *error = QStringLiteral("%1: \"%2\" must be a list.").arg(where, list);
                return false;
            }
            for (const auto &entry : entries.toArray()) {
                QString tag;
                QStringList from;
                if (entry.isString()) {
                    tag = ReviewTags::normalize(entry.toString());
                } else if (entry.isObject()) {
                    const QJsonObject item = entry.toObject();
                    if (!onlyKeys(item, {QStringLiteral("tag"), QStringLiteral("from"), QStringLiteral("about")}, where, error)) return false;
                    tag = ReviewTags::normalize(item.value(QStringLiteral("tag")).toString());
                    if (!strings(item.value(QStringLiteral("from")), QStringLiteral("%1, tag \"%2\": from").arg(where, tag), &from, error)) return false;
                }
                if (tag.isEmpty()) {
                    *error = QStringLiteral("%1: every %2 entry needs a tag name.").arg(where, list);
                    return false;
                }
                if (named.contains(tag)) {
                    *error = QStringLiteral("The tag \"%1\" is listed twice.").arg(tag);
                    return false;
                }
                named.insert(tag);
                (list == QLatin1String("scale") ? group.scale : group.tags).append(tag);
                from.removeAll(tag);
                from.removeDuplicates();
                for (const QString &source : std::as_const(from)) {
                    if (merged.contains(source)) {
                        *error = QStringLiteral("\"%1\" is merged into two tags.").arg(source);
                        return false;
                    }
                    merged.insert(source);
                }
                if (!from.isEmpty()) result.merges.append({tag, from});
            }
        }
        if (group.scale.size() == 1) {
            *error = QStringLiteral("%1: a slider needs at least two steps.").arg(where);
            return false;
        }
        result.groups.append(group);
    }
    for (const QString &source : std::as_const(merged)) {
        if (named.contains(source)) {
            *error = QStringLiteral("\"%1\" is both a tag of the plan and merged into another; a merged tag cannot stay a tag.").arg(source);
            return false;
        }
    }
    const QJsonValue levels = document.value(QStringLiteral("knowledge_levels"));
    if (!levels.isUndefined()) {
        if (!levels.isObject()) {
            *error = QStringLiteral("\"knowledge_levels\" must map tags to solid, shaky, unknown or ignore.");
            return false;
        }
        const QJsonObject object = levels.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!levelNames().contains(it.value().toString()) || ReviewTags::normalize(it.key()).isEmpty()) {
                *error = QStringLiteral("The knowledge level of \"%1\" must be one of %2.").arg(it.key(), levelNames().join(QStringLiteral(", ")));
                return false;
            }
            result.levels.append({it.key(), it.value().toString()});
        }
    }
    *plan = result;
    return true;
}

QJsonObject evaluate(const Plan &plan, const QList<ReviewCard> &cards, const QHash<QString, QString> &merges,
                     const QList<ReviewTags::TagGroup> &groups, QList<Merge> *mergesToRecord, QList<ReviewTags::TagGroup> *groupsToSave)
{
    if (mergesToRecord) mergesToRecord->clear();
    if (groupsToSave) groupsToSave->clear();
    const Usage before = usageOf(cards, merges);
    QSet<QString> grouped;
    for (const auto &group : groups) {
        for (const QString &tag : group.tags) grouped.insert(tag);
    }
    const auto known = [&](const QString &tag) { return before.answers.contains(tag) || grouped.contains(tag); };

    // Merges, by the names tags are shown under now.
    QJsonArray mergeReports;
    QStringList problems;
    QList<Merge> recorded;
    for (const Merge &merge : plan.merges) {
        const QString target = ReviewTags::resolve(merge.to, merges);
        if (target != merge.to)
            return failure(QStringLiteral("invalid_plan"),
                           QStringLiteral("\"%1\" was merged into \"%2\" earlier; it cannot become a tag again. Use \"%2\".").arg(merge.to, target));
        Merge write{merge.to, {}};
        QJsonArray sources;
        for (const QString &source : merge.from) {
            const QString shown = ReviewTags::resolve(source, merges);
            QJsonObject item{{QStringLiteral("tag"), source}, {QStringLiteral("cards"), before.cards.value(shown)},
                             {QStringLiteral("answers"), before.answers.value(shown)}};
            if (shown != source) item.insert(QStringLiteral("shown_as"), shown);
            if (shown == merge.to) {
                item.insert(QStringLiteral("state"), QStringLiteral("already_merged"));
            } else if (!known(shown)) {
                item.insert(QStringLiteral("state"), QStringLiteral("not_found"));
                problems.append(QStringLiteral("No answer or group has \"%1\" (to merge into \"%2\"); check the spelling.").arg(source, merge.to));
            } else {
                item.insert(QStringLiteral("state"), QStringLiteral("merge"));
                if (!write.from.contains(shown)) write.from.append(shown);
            }
            sources.append(item);
        }
        if (!write.from.isEmpty()) recorded.append(write);
        mergeReports.append(QJsonObject{{QStringLiteral("to"), merge.to}, {QStringLiteral("from"), sources},
                                        {QStringLiteral("records"), !write.from.isEmpty()}});
    }
    const QHash<QString, QString> mapping = withMerges(merges, recorded);
    const Usage after = usageOf(cards, mapping);

    // The layout: each group keeps the ID of the current group it matches.
    const QList<ReviewTags::TagGroup> current = ReviewTags::effectiveGroups(groups, mapping);
    QList<ReviewTags::TagGroup> layout;
    QJsonArray groupReports;
    QSet<QString> matched;
    for (const Group &group : plan.groups) {
        const ReviewTags::TagGroup *match = nullptr;
        QString state = QStringLiteral("new");
        for (const auto &candidate : current) {
            if (matched.contains(candidate.id)) continue;
            if (!group.groupId.isEmpty() ? candidate.id == group.groupId : candidate.name.compare(group.name, Qt::CaseInsensitive) == 0) {
                match = &candidate;
                state = candidate.name == group.name ? QStringLiteral("kept") : QStringLiteral("renamed");
                break;
            }
        }
        for (const QString &previous : group.previousNames) {
            for (const auto &candidate : current) {
                if (match || matched.contains(candidate.id) || candidate.name.compare(previous, Qt::CaseInsensitive) != 0) continue;
                match = &candidate;
                state = QStringLiteral("renamed");
            }
        }
        ReviewTags::TagGroup result;
        result.id = match ? match->id : !group.groupId.isEmpty() ? group.groupId : QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.name = group.name;
        result.scale = group.scale;
        result.tags = group.scale + group.tags;
        if (match) {
            result.extra = match->extra;
            result.section = match->section;
            result.phase = match->phase;
            matched.insert(match->id);
        }
        if (group.section) result.section = *group.section;
        if (group.phase) {
            result.phase = *group.phase;
            // A newer client's value gives way to the plan's.
            result.extra.remove(QStringLiteral("phase"));
        }
        layout.append(result);
        QJsonObject report{{QStringLiteral("name"), group.name}, {QStringLiteral("group_id"), result.id}, {QStringLiteral("state"), state},
                           {QStringLiteral("slider"), !group.scale.isEmpty()}};
        if (!result.section.isEmpty()) report.insert(QStringLiteral("section"), result.section);
        if (match && match->section != result.section) report.insert(QStringLiteral("previous_section"), match->section);
        if (!result.phase.isEmpty()) report.insert(QStringLiteral("phase"), result.phase);
        if (match && match->phase != result.phase) report.insert(QStringLiteral("previous_phase"), match->phase);
        if (match && match->name != group.name) report.insert(QStringLiteral("previous_name"), match->name);
        for (const QString &list : {QStringLiteral("scale"), QStringLiteral("tags")}) {
            QJsonArray items;
            for (const QString &tag : list == QLatin1String("scale") ? group.scale : group.tags)
                items.append(QJsonObject{{QStringLiteral("tag"), tag}, {QStringLiteral("cards"), after.cards.value(tag)},
                                         {QStringLiteral("answers"), after.answers.value(tag)}});
            report.insert(list, items);
        }
        groupReports.append(report);
    }
    if (const QString problem = ReviewTags::groupsError(layout); !problem.isEmpty())
        return failure(QStringLiteral("invalid_plan"), QStringLiteral("The groups cannot be saved: %1").arg(problem));
    QJsonArray removed;
    for (const auto &group : current) {
        if (!matched.contains(group.id))
            removed.append(QJsonObject{{QStringLiteral("name"), group.name}, {QStringLiteral("group_id"), group.id},
                                       {QStringLiteral("tags"), QJsonArray::fromStringList(group.tags)}});
    }

    // Tags the data has that no group of the plan lists: they would show under Other.
    QSet<QString> planned;
    for (const auto &group : std::as_const(layout)) {
        for (const QString &tag : group.tags) planned.insert(tag);
    }
    QStringList uncovered;
    for (auto it = after.answers.cbegin(); it != after.answers.cend(); ++it) {
        if (!planned.contains(it.key()) && !uncovered.contains(it.key())) uncovered.append(it.key());
    }
    for (const auto &group : current) {
        for (const QString &tag : group.tags) {
            if (!planned.contains(tag) && !uncovered.contains(tag)) uncovered.append(tag);
        }
    }
    std::sort(uncovered.begin(), uncovered.end(), [&after](const QString &a, const QString &b) {
        if (after.answers.value(a) != after.answers.value(b)) return after.answers.value(a) > after.answers.value(b);
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    QJsonArray notCovered;
    for (const QString &tag : std::as_const(uncovered)) {
        QJsonObject item{{QStringLiteral("tag"), tag}, {QStringLiteral("cards"), after.cards.value(tag)}, {QStringLiteral("answers"), after.answers.value(tag)}};
        const QString owner = ReviewTags::groupOf(current, tag);
        for (const auto &group : current) {
            if (group.id == owner && !owner.isEmpty()) item.insert(QStringLiteral("current_group"), group.name);
        }
        notCovered.append(item);
    }

    const bool layoutChanged = layout != current;
    if (mergesToRecord) *mergesToRecord = recorded;
    if (groupsToSave && layoutChanged) *groupsToSave = layout;
    QJsonObject levels;
    for (const auto &[tag, level] : plan.levels) levels.insert(tag, level);
    QJsonObject report{{QStringLiteral("schema_version"), 1},
                       {QStringLiteral("accepted"), true},
                       {QStringLiteral("terminal"), true},
                       {QStringLiteral("cards"), int(cards.size())},
                       {QStringLiteral("answers"), before.answerCount},
                       {QStringLiteral("merges"), mergeReports},
                       {QStringLiteral("groups"), groupReports},
                       {QStringLiteral("removed_groups"), removed},
                       {QStringLiteral("not_covered"), notCovered},
                       {QStringLiteral("problems"), QJsonArray::fromStringList(problems)},
                       {QStringLiteral("writes"), QJsonObject{{QStringLiteral("merges"), int(recorded.size())},
                                                              {QStringLiteral("groups"), layoutChanged}}}};
    if (!plan.levels.isEmpty()) report.insert(QStringLiteral("knowledge_levels"), levels);
    if (std::any_of(layout.cbegin(), layout.cend(), [](const ReviewTags::TagGroup &group) { return !group.scale.isEmpty(); }))
        report.insert(QStringLiteral("notes"), QJsonArray{QStringLiteral("The layout has sliders: Sync uploads it only to a server that advertises %1. "
                                                                         "Until then it stays queued and shows on this computer only; the merges sync at once.")
                                                              .arg(ReviewTags::scaleCapability())});
    if (std::any_of(layout.cbegin(), layout.cend(), [](const ReviewTags::TagGroup &group) { return !group.section.isEmpty(); })) {
        QJsonArray notes = report.value(QStringLiteral("notes")).toArray();
        notes.append(QStringLiteral("The layout has sections: Sync uploads it only to a server that advertises %1. "
                                    "Until then it stays queued and shows on this computer only; the merges sync at once.")
                         .arg(ReviewTags::sectionCapability()));
        report.insert(QStringLiteral("notes"), notes);
    }
    if (std::any_of(layout.cbegin(), layout.cend(), [](const ReviewTags::TagGroup &group) { return !group.phase.isEmpty(); })) {
        QJsonArray notes = report.value(QStringLiteral("notes")).toArray();
        notes.append(QStringLiteral("The layout asks groups before the answer: Sync uploads it only to a server that advertises %1. "
                                    "Until then it stays queued and shows on this computer only; the merges sync at once.")
                         .arg(ReviewTags::phaseCapability()));
        report.insert(QStringLiteral("notes"), notes);
    }
    return report;
}

QJsonObject apply(const Plan &plan, ReviewStore &store)
{
    const auto activeCards = [&store] {
        QList<ReviewCard> result;
        for (const auto &card : store.cards()) {
            const QString status = card.status();
            if (card.schedule.deleted || card.record.value(QStringLiteral("deleted")).toBool() || status == QLatin1String("candidate")
                || status == QLatin1String("rejected")) continue;
            result.append(card);
        }
        return result;
    };
    QList<Merge> merges;
    QList<ReviewTags::TagGroup> layout;
    const QList<ReviewCard> cards = activeCards();
    QJsonObject report = evaluate(plan, cards, store.tagMerges(), store.tagGroups(), &merges, &layout);
    if (!report.value(QStringLiteral("accepted")).toBool()) return report;
    int recorded = 0;
    for (const Merge &merge : std::as_const(merges)) {
        QString error;
        if (!store.recordTagMerge(merge.from, merge.to, &error)) {
            QJsonObject result = failure(QStringLiteral("write_failed"),
                                         QStringLiteral("Recorded %1 of %2 merges; merging into \"%3\" failed: %4").arg(recorded).arg(merges.size()).arg(merge.to, error));
            result.insert(QStringLiteral("report"), report);
            return result;
        }
        ++recorded;
    }
    // The layout as it reads with the merges now recorded (a merged name in a group moves).
    const QJsonObject after = evaluate(plan, cards, store.tagMerges(), store.tagGroups(), nullptr, &layout);
    bool saved = false;
    if (!layout.isEmpty()) {
        QString error;
        if (!store.saveTagGroups(layout, &error)) {
            QJsonObject result = failure(QStringLiteral("write_failed"),
                                         QStringLiteral("Recorded %1 merges, but the groups could not be saved: %2").arg(recorded).arg(error));
            result.insert(QStringLiteral("report"), report);
            return result;
        }
        saved = true;
    }
    report.insert(QStringLiteral("groups"), after.value(QStringLiteral("groups")));
    report.insert(QStringLiteral("applied"), QJsonObject{{QStringLiteral("merges_recorded"), recorded}, {QStringLiteral("groups_saved"), saved}});
    report.insert(QStringLiteral("message"), recorded || saved
        ? QStringLiteral("Recorded %1 and %2. Sync to share them with your other devices.")
              .arg(plural(recorded, "merge", "merges"), saved ? QStringLiteral("saved the groups") : QStringLiteral("left the groups unchanged"))
        : QStringLiteral("Nothing to change: the plan is already applied."));
    return report;
}

QString formatReport(const QJsonObject &report)
{
    QString text;
    QTextStream out(&text);
    if (!report.value(QStringLiteral("accepted")).toBool()) {
        out << "error (" << report.value(QStringLiteral("error_code")).toString() << "): " << report.value(QStringLiteral("message")).toString() << '\n';
        return text;
    }
    const auto count = [](const QJsonObject &item) {
        return QStringLiteral("%1, %2").arg(plural(item.value(QStringLiteral("cards")).toInt(), "card", "cards"),
                                            plural(item.value(QStringLiteral("answers")).toInt(), "answer", "answers"));
    };
    out << "Review tags: " << plural(report.value(QStringLiteral("cards")).toInt(), "card", "cards") << ", "
        << plural(report.value(QStringLiteral("answers")).toInt(), "answer", "answers") << " in effect.\n";
    out << "Counts: cards = cards whose latest answer has the tag; answers = answers in effect with it.\n\n";
    const QJsonObject writes = report.value(QStringLiteral("writes")).toObject();
    out << "Merges (" << writes.value(QStringLiteral("merges")).toInt() << " to record):\n";
    for (const auto &value : report.value(QStringLiteral("merges")).toArray()) {
        const QJsonObject merge = value.toObject();
        out << "  -> \"" << merge.value(QStringLiteral("to")).toString() << "\"\n";
        for (const auto &source : merge.value(QStringLiteral("from")).toArray()) {
            const QJsonObject item = source.toObject();
            const QString state = item.value(QStringLiteral("state")).toString();
            out << "       <- \"" << item.value(QStringLiteral("tag")).toString() << "\"";
            if (item.contains(QStringLiteral("shown_as"))) out << " (shown as \"" << item.value(QStringLiteral("shown_as")).toString() << "\")";
            out << "  " << count(item);
            if (state == QLatin1String("already_merged")) out << "  [already merged]";
            if (state == QLatin1String("not_found")) out << "  [NOT FOUND]";
            out << '\n';
        }
    }
    out << "\nGroups, in order" << (writes.value(QStringLiteral("groups")).toBool() ? " (layout changes)" : " (unchanged)") << ":\n";
    int index = 0;
    QString section;
    for (const auto &value : report.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = value.toObject();
        // Adjacent groups of one section show under its header, as in the tag picker.
        const QString groupSection = group.value(QStringLiteral("section")).toString();
        if (!groupSection.isEmpty() && groupSection != section) out << "  [section \"" << groupSection << "\"]\n";
        section = groupSection;
        const QString phase = group.value(QStringLiteral("phase")).toString();
        out << "  " << (groupSection.isEmpty() ? "" : "  ") << ++index << ". " << group.value(QStringLiteral("name")).toString()
            << (group.value(QStringLiteral("slider")).toBool() ? "  [slider]" : "")
            << (phase.isEmpty() ? QString() : QStringLiteral("  [asked %1]").arg(phase == ReviewTags::beforePhase() ? QStringLiteral("before") : QStringLiteral("before and after")))
            << "  (" << group.value(QStringLiteral("state")).toString();
        if (group.contains(QStringLiteral("previous_name"))) out << " from \"" << group.value(QStringLiteral("previous_name")).toString() << "\"";
        if (group.contains(QStringLiteral("previous_section"))) {
            const QString previous = group.value(QStringLiteral("previous_section")).toString();
            out << (previous.isEmpty() ? QStringLiteral(", section added") : QStringLiteral(", section was \"%1\"").arg(previous));
        }
        if (group.contains(QStringLiteral("previous_phase"))) {
            const QString previous = group.value(QStringLiteral("previous_phase")).toString();
            out << QStringLiteral(", was asked %1").arg(previous.isEmpty() ? QStringLiteral("after") : previous == ReviewTags::beforePhase() ? QStringLiteral("before")
                                                                                                                              : QStringLiteral("before and after"));
        }
        out << ", id " << group.value(QStringLiteral("group_id")).toString() << ")\n";
        for (const QString &list : {QStringLiteral("scale"), QStringLiteral("tags")}) {
            const QJsonArray items = group.value(list).toArray();
            if (items.isEmpty()) continue;
            QStringList shown;
            for (const auto &item : items)
                shown.append(QStringLiteral("%1 (%2)").arg(item.toObject().value(QStringLiteral("tag")).toString()).arg(item.toObject().value(QStringLiteral("cards")).toInt()));
            out << "       " << (list == QLatin1String("scale") ? "steps: " : "tags:  ") << shown.join(QStringLiteral(" | ")) << '\n';
        }
    }
    out << "  -  Other (tags in no group)\n";
    const QJsonArray removed = report.value(QStringLiteral("removed_groups")).toArray();
    if (!removed.isEmpty()) {
        out << "\nGroups removed (their remaining tags show under Other):\n";
        for (const auto &value : removed) out << "  " << value.toObject().value(QStringLiteral("name")).toString() << '\n';
    }
    const QJsonArray uncovered = report.value(QStringLiteral("not_covered")).toArray();
    out << "\nTags not covered by the plan (" << uncovered.size() << ", shown under Other):\n";
    for (const auto &value : uncovered) {
        const QJsonObject item = value.toObject();
        out << "  \"" << item.value(QStringLiteral("tag")).toString() << "\"  " << count(item);
        if (item.contains(QStringLiteral("current_group"))) out << "  (now in " << item.value(QStringLiteral("current_group")).toString() << ")";
        out << '\n';
    }
    if (uncovered.isEmpty()) out << "  (none)\n";
    const QJsonArray problems = report.value(QStringLiteral("problems")).toArray();
    if (!problems.isEmpty()) {
        out << "\nProblems:\n";
        for (const auto &value : problems) out << "  " << value.toString() << '\n';
    }
    for (const auto &value : report.value(QStringLiteral("notes")).toArray()) out << "\nNote: " << value.toString() << '\n';
    if (report.contains(QStringLiteral("knowledge_levels_file")))
        out << "\nKnowledge levels: " << report.value(QStringLiteral("knowledge_levels_file")).toString() << '\n';
    if (report.contains(QStringLiteral("message"))) out << '\n' << report.value(QStringLiteral("message")).toString() << '\n';
    return text;
}
}
