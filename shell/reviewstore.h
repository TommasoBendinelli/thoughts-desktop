/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_REVIEWSTORE_H
#define THOUGHTS_REVIEWSTORE_H

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

#include "reviewinterval.h"

namespace Thoughts
{

class DesktopSyncOutbox;
struct ReviewAnchorPlan;

struct ReviewSchedule {
    QDateTime dueAt;
    int intervalDays = 0;
    bool deleted = false;
    bool retained = false;
    bool fixedInterval = false;
    int learningStep = -1;
    bool relearning = false;
    int previousIntervalDays = 0;
    bool reviewed = false;
    int reviewCount = 0;
    int lapseCount = 0;
    QString lastRating;
    QDateTime lastReviewedAt;
    QString deleteEventId;
    // The latest answer's interval; twice it is suggested next, whatever the answer was tagged.
    QDateTime suggestionReviewedAt, suggestionDueAt;
    // Retired (ReviewRetire::fold, set by ReviewStore::cards()): never offered for review, its
    // schedule and history kept. retiredAt and retireEventId are those of the retire in effect.
    bool retired = false;
    QDateTime retiredAt;
    QString retireEventId;
    // Its Project document is unpublished (protocol/project-document-publication.md): like a
    // retired card it is offered for nothing, and its schedule is kept. Set where cards are
    // read for agents (ReviewKnowledge); ReviewStore::activeCards() applies the rule itself.
    bool unpublished = false;
};

/**
 * Retiring a card takes it out of review but keeps its history; "Restore to review" brings it
 * back with its old schedule (thoughts-server protocol/review-card-retire.md). Retire, restore and
 * the undo of either are review events with a queue_action and no rating, so the answer fold and
 * older clients ignore them.
 */
namespace ReviewRetire
{
QString retireAction();  // "retire"
QString restoreAction(); // "restore"
QString undoAction();    // "undo_retire", naming the undone retire or restore in undoneField()
QString undoneField();   // "undone_event_id"
// Whether @p event is a retire, a restore or an undo of one.
bool isEvent(const QJsonObject &event);
struct State {
    bool deleted = false; // A deliberate delete event: deletion wins.
    bool retired = false;
    QDateTime retiredAt;
    QString retiredAtText; // reviewed_at of the retire in effect, as recorded.
    QString retireEventId;
};
// The spec's section 4 for every card with events (card_id -> state); a card without any is
// active. A pure function of the set of events: their order does not matter.
QHash<QString, State> fold(const QList<QJsonObject> &events);
}

/**
 * Knowledge cards and project cards (thoughts-server protocol/review-card-kinds.md). Knowledge cards
 * keep today's schedule; project cards come back at least a week after their latest answer, and
 * at once when their passage changed. A card follows its Library Projects unless the user chose
 * its kind (a set_review_kind queue action, or the kind it was made with in its record).
 */
namespace ReviewKinds
{
QString capability();     // "review_card_kinds_v1"
QString action();         // "set_review_kind": the review event queue action and the Library-project action
QString field();          // "review_kind": of kind events, card records and catalogue entries
QString knowledge();      // "knowledge"
QString project();        // "project"
QString followProject();  // "follow_project": the card follows its Projects again
QString sourceCard();     // "card": the user chose the card's kind
QString sourceProject();  // "project": a Project holds the card
QString sourceDefault();  // "default": no Project holds it
QString passageField();   // "passage": the passage an answer was given on
int projectIntervalDays(); // 7: at most this many days between reviews of a project card
// Whether @p event is a kind event (queue_action set_review_kind), valid or not.
bool isEvent(const QJsonObject &event);
// The kind a catalogue entry gives its cards: knowledge unless it says project.
QString projectKind(const QJsonObject &project);
// Section 4: the catalogue entries that hold the card, in catalogue order.
QList<QJsonObject> cardProjects(const QJsonObject &cardRecord, const QJsonArray &projects);
struct Kind {
    QString kind;           // knowledge() or project()
    QString source;         // sourceCard(), sourceProject() or sourceDefault()
    QStringList projectIds; // The Projects that hold the card, in catalogue order.
    QString projectsKind;   // The kind the card would follow: project() when any of them is a project Project.
    bool isProject() const { return kind == project(); }
};
// Section 3: a pure function of the card's events, its record and the catalogue; the order of the
// events does not matter.
Kind fold(const QJsonObject &cardRecord, const QList<QJsonObject> &events, const QJsonArray &projects);
// The card's latest valid kind event (section 3 step 1), or an empty object.
QJsonObject latestEvent(const QString &cardId, const QList<QJsonObject> &events);
// Section 6.1: the card's latest answer that no retraction took back, or an empty object.
QJsonObject latestAnswer(const QString &cardId, const QList<QJsonObject> &events);
// Section 6.3: what an answer given now carries as "passage" ({"segments": [{segment_id,
// content_sha256}…]}), or an empty object when the card has no complete version-2 segments.
QJsonObject passageStamp(const QJsonObject &cardRecord);
// Section 6.3: the passage differs from the one the latest answer was given on.
bool passageChanged(const QJsonObject &cardRecord, const QList<QJsonObject> &events);
// Sections 6.2 and 6.3: a project card's due time from the knowledge schedule's @p scheduledDueAt
// (invalid: not scheduled). A card never answered keeps it.
QDateTime projectDueAt(const QJsonObject &cardRecord, const QList<QJsonObject> &events, const QDateTime &scheduledDueAt);
// An ISO 8601 time with a zone, else invalid (kind events and answers need one).
QDateTime parseTime(const QString &value);
// The catalogue the fold reads: the Library-project records as this computer last received them
// (GET /v1/library/projects, kept in .thoughts/library-projects.json), with this computer's queued
// set_review_kind and remove actions applied. Read again only when the file changed.
QJsonArray catalogue(const QString &docsRoot);
// The catalogue entry @p projectId of @p projects, or an empty object.
QJsonObject catalogueProject(const QJsonArray &projects, const QString &projectId);
}

/**
 * Free-form review labels, shared with the phone. Tags only describe an answer; the
 * chosen interval alone schedules the card. Built-ins keep stable wire IDs.
 */
namespace ReviewTags
{
QString correct();
QString wrong();
QString skip();
QStringList builtIn();
// Trims and collapses whitespace; built-in IDs and names match case-insensitively.
// Up to 1000 characters are kept, as on Android.
QString normalize(const QString &text, const QHash<QString, QString> &builtInLabels = {});
// Tags are stored whole; buttons and chips show the first few words.
QString shortLabel(const QString &label);
// Most-used first; unused built-ins keep their familiar order ahead of other tags.
// Built-ins merged into another tag (@p merges) are left out: their answers count under the new name.
QStringList byPopularity(const QHash<QString, int> &usage, const QStringList &extra = {}, const QHash<QString, QString> &merges = {});
// The tag picker's order inside a group, as on the phone: most-used first, ties by the label
// shown (case-insensitive). @p usage is reviewTagUsage(); a tag never used counts 0.
QStringList byUsage(QStringList tags, const QHash<QString, int> &usage, const std::function<QString(const QString &)> &label);
// A tag with the number of answers tagged with it, e.g. "Correct (42)". On a chip only the
// label is shortened (shortLabel), never the count; tooltips and lists show the whole label.
QString withCount(const QString &label, int count);
QString chipText(const QString &label, int count);
// Older clients read `review_action`; keep writing it while they are in use.
QString legacyReviewAction(const QStringList &tags);
// Events written before tagging existed are mapped from the action and rating they recorded;
// an answer recorded with an empty `review_tags` list has no tags.
// These are the recorded tags; readers show effective ones (ReviewCard::tagsOfEvent).
QStringList ofEvent(const QJsonObject &event);

// Permanent merges ("Compact tags") are made on the phone and arrive through Sync as
// `review_tag_merge` entities (collection `review-tags`). Answers keep the tags they were
// recorded with; every reader shows a merged tag under its new name.
QString mergeEntityType();
QString mergeCollection();
// Old tag -> final name. Merges apply in (created_at, merge_id) order and values are never
// keys, so chains and re-merges cannot loop. Must match Android's ReviewTagMerges.mappingOf.
QHash<QString, QString> mergeMapping(const QList<QJsonObject> &merges);
// The name a tag is shown under, e.g. a typed old name becomes the tag it was merged into.
QString resolve(const QString &tag, const QHash<QString, QString> &mapping);
// Maps each tag and drops repeats, keeping the first occurrence.
QStringList applyMerges(const QStringList &tags, const QHash<QString, QString> &mapping);

// Named groups only organize the tag list; any tags from any groups can still be chosen.
// One mutable `review_tag_groups` record per account (entity_id "groups", collection
// `review-tags`), shared with the phone through the server; the newest write wins. Every
// known tag in no group is shown under the implicit "Other" group, always last.
//
// A group may be shown as a slider: `scale` lists its ordered steps (weakest first), each also
// listed in `tags` so clients without sliders show them as chips. Picking a step replaces the
// group's other step; the group's other tags ("free tags") stay multi-select. Answers store plain
// tags. As on the phone: a step missing from `tags` is added to them; a step already in an
// earlier group's tags is not a step of this one; moving a step out of its group ends it.
//
// A group may name a `section` (display only): groups with the same section, exactly and
// case-sensitively, that are adjacent in the layout show under one collapsible header named
// after it ("Work left" over Structure, Style, Content). The same name further down starts
// another header; nothing is reordered. Every edit keeps a group's section.
//
// A group may be asked before the answer is shown (review-phases spec): `phase` "before" asks it
// only then (Feeling), "before_and_after" asks it then as a prediction and again after the answer,
// starting at the prediction (Recall). Absent, the group is asked after the answer. Another value
// (from a future client) stays in `extra`, unchanged, and the group is asked after.
struct TagGroup {
    QString id;
    QString name;
    QStringList tags;
    QStringList scale = {};
    // Trimmed, at most sectionMaxLength() characters; empty when the group is in no section.
    QString section = {};
    // Fields of the stored group this client does not know; written back unchanged.
    QJsonObject extra = {};
    // beforePhase(), beforeAndAfterPhase(), or empty: asked after the answer only.
    QString phase = {};
    bool operator==(const TagGroup &other) const = default;
};
QString groupsEntityType();
QString groupsId();
// Servers that accept `review_tag_groups`; until one does, local edits stay queued.
QString groupsCapability();
QString otherGroupName();
// The groups of a record exactly as stored, or false (and @p error) when the record does not
// follow the shared schema. A tag in more than one group keeps its first occurrence.
bool parseGroups(const QJsonObject &record, QList<TagGroup> *groups, QString *error = nullptr);
// The groups as shown: tag names normalized and mapped through the merges, Skip dropped, and
// every tag shown once: in the first group listing its final name, else the first listing an
// old name of it.
QList<TagGroup> effectiveGroups(const QList<TagGroup> &groups, const QHash<QString, QString> &mapping);
// The tags of @p tags that are (or were merged into) a slider step. Free tags are no longer
// offered, so a new answer keeps only these.
QStringList onlySteps(const QStringList &tags, const QList<TagGroup> &groups, const QHash<QString, QString> &mapping);
// Empty when @p groups can be saved as a record; otherwise why not.
QString groupsError(const QList<TagGroup> &groups);
// The whole record for @p groups, as uploaded. @p extra holds record fields this client does not
// know (from the record being replaced); they are kept unchanged.
QJsonObject groupsRecord(const QList<TagGroup> &groups, const QString &updatedAt, const QString &originId, const QJsonObject &extra = {});
// The fields of a stored record (or group) that groupsRecord() does not write itself.
QJsonObject unknownRecordFields(const QJsonObject &record);
// Servers that accept groups with a `scale`; until one does, such records stay queued.
QString scaleCapability();
// Whether a stored record has a slider group, which needs scaleCapability().
bool recordHasScale(const QJsonObject &record);
// Servers that accept groups with a `section`; until one does, such records stay queued.
QString sectionCapability();
// Whether a stored record has a group with a section, which needs sectionCapability().
bool recordHasSection(const QJsonObject &record);
// In characters (code points), as the server counts them.
int sectionMaxLength();
// Puts @p groupId in the section @p section (trimmed), or in none when it is empty. False (and
// @p error) when the name is too long or the group no longer exists.
bool setSection(QList<TagGroup> &groups, const QString &groupId, const QString &section, QString *error = nullptr);
// The section names of @p groups, each once, in layout order.
QStringList sectionNames(const QList<TagGroup> &groups);
// The two phases a group may name; servers accept exactly these.
QString beforePhase();          // "before"
QString beforeAndAfterPhase();  // "before_and_after"
// Servers that accept groups with a `phase`; until one does, such records stay queued.
QString phaseCapability();
// Whether a stored record has a group with a phase, which needs phaseCapability().
bool recordHasPhase(const QJsonObject &record);
// Asks @p groupId before the answer (beforePhase()), before and after (beforeAndAfterPhase()), or
// after only (empty). False (and @p error) for another value or a group that no longer exists.
bool setPhase(QList<TagGroup> &groups, const QString &groupId, const QString &phase, QString *error = nullptr);
// Whether the group is asked before the answer (as a prediction for "before_and_after").
bool askedBefore(const TagGroup &group);
// Whether the group is asked after the answer (not "before" only).
bool askedAfter(const TagGroup &group);
// The group the agent predicts recall for: the first "before_and_after" group with a scale; null
// when there is none (then no prediction is asked for).
const TagGroup *recallGroup(const QList<TagGroup> &groups);
// Shows @p groupId as a slider with @p steps (tags of the group, at least two), or as chips when
// @p steps is empty. False (and @p error) when the steps are not the group's tags.
bool setScale(QList<TagGroup> &groups, const QString &groupId, const QStringList &steps, QString *error = nullptr);
// The group ID of @p tag, or an empty string when it is in Other.
QString groupOf(const QList<TagGroup> &groups, const QString &tag);
// The sections shown in a tag picker: each group with its tags in @p knownTags order (tags only
// a group lists come last), then Other with the remaining known tags when there are any.
// Other has an empty id.
QList<TagGroup> sections(const QList<TagGroup> &groups, const QStringList &knownTags);
// Edits; each returns the complete new list of groups, or false with @p error.
bool addGroup(QList<TagGroup> &groups, const QString &name, QString *error = nullptr, QString *createdId = nullptr);
bool renameGroup(QList<TagGroup> &groups, const QString &groupId, const QString &name, QString *error = nullptr);
bool moveGroup(QList<TagGroup> &groups, const QString &groupId, int offset);
bool deleteGroup(QList<TagGroup> &groups, const QString &groupId);
// Moves @p tag into @p groupId, or into Other when @p groupId is empty.
bool moveTag(QList<TagGroup> &groups, const QString &tag, const QString &groupId, QString *error = nullptr);

// An answer may carry a free-text note per group, recorded with its tags in the review event
// as `review_group_notes`: group_id -> note, with the key "other" for the implicit Other group.
// The answer's own free-form note (the Note box at the end of the tag sheet, not tied to a
// group) is recorded there too, under the reserved key "note". The note typed before the answer
// was shown (review-phases spec) is recorded under the reserved key "before_note". The field is
// left out when every note is empty.
QString groupNotesField();
QString otherGroupNoteKey();
QString answerNoteKey();
QString beforeNoteKey();
// The label of the answer's free-form note where notes are listed.
QString answerNoteName();
// The label of the note typed before the answer was shown.
QString beforeNoteName();
// In characters (code points), as for the other limits the server checks.
int groupNoteMaxLength();
// @p text cut to groupNoteMaxLength() characters, as typed.
QString cutGroupNote(const QString &text);
// The note @p text as recorded: trimmed and cut to groupNoteMaxLength() characters.
QString cleanGroupNote(const QString &text);
// The notes as recorded: each cleaned, empty ones dropped.
QHash<QString, QString> cleanGroupNotes(const QHash<QString, QString> &notes);
QHash<QString, QString> groupNotesOfEvent(const QJsonObject &event);
// "<Group>: <note>" in the order of @p groups, then Other, then notes of groups that no longer
// exist (shown under their ID), then the note typed before the answer as "<beforeNoteLabel>:
// <note>", then the answer's own note as "<noteLabel>: <note>".
QStringList groupNoteLines(const QHash<QString, QString> &notes, const QList<TagGroup> &groups, const QString &otherLabel = otherGroupName(),
                           const QString &noteLabel = answerNoteName(), const QString &beforeNoteLabel = beforeNoteName());
}

/**
 * A review in two steps (review-phases spec): before the answer is shown the person picks the
 * groups asked before (ReviewTags::askedBefore) and may type a before note; after it, the other
 * groups, and again the "before_and_after" ones. An answer given so records, next to its
 * `review_tags` (the before-only groups' tags and the after step's tags):
 *
 *   "review_phases": {"schema_version": 1,
 *                     "before": {"tags": [...], "answer_shown_at": "<ISO>"},
 *                     "after": {"tags": [...]},
 *                     "agent_prediction": {"tag", "group_id", "reason", "model", "prediction_id",
 *                                          "requested_at", "made_at", "arrived_before_answer"}}
 *
 * answer_shown_at is left out when the answer was never shown, agent_prediction when none arrived
 * before the answer was saved. The notes are in review_group_notes (ReviewTags::beforeNoteKey()).
 */
/**
 * How the review that gave an answer was started, recorded with every answer (never with a
 * deletion) as "review_entry": "due_queue" when the card came up in a session of due cards,
 * "review_now" when the person picked this one card to review (Review now), due or not.
 * Events without the field come from older clients.
 */
namespace ReviewEntry
{
QString field(); // "review_entry"
QString dueQueue(); // "due_queue"
QString reviewNow(); // "review_now"
}

namespace ReviewPhases
{
QString field(); // "review_phases"
struct Recorded {
    bool present = false;
    QStringList beforeTags;
    QStringList afterTags;
    QDateTime answerShownAt; // Invalid when not recorded.
    QJsonObject agentPrediction; // Empty when none arrived.
};
// What @p event records; present is false for an event without the field (older clients).
Recorded ofEvent(const QJsonObject &event);
// The event field. @p agentPrediction is the service's answer completed by the client (left out
// when empty).
QJsonObject toField(const QStringList &beforeTags, const QStringList &afterTags, const QDateTime &answerShownAt,
                    const QJsonObject &agentPrediction = {});
}

struct ReviewCard {
    QJsonObject record;
    ReviewSchedule schedule;
    QDateTime skippedUntil;
    QList<QJsonObject> history;
    QJsonObject sourceLocation; // Disposable local location, never written into the card record.
    // The record as stored, before this computer's view of its sources changed it (cards() lines a
    // card up with the source as it is now). An edit checks that this, not the view, is unchanged.
    QJsonObject savedRecord;
    const QJsonObject &stored() const { return savedRecord.isEmpty() ? record : savedRecord; }

    QString id() const;
    QString status() const;
    QString pdfStem() const;
    QString pdfSha256() const;
    QString pageVisualSha256() const;
    QString question() const;
    int page() const;
    QVariantList rect() const;
    QVariantList regions() const;
    QString pagesLabel() const;
    // The answer is the whole PDF: every page is hidden and the source has no markers.
    bool isWholeDocument() const;
    bool isApproved() const;
    QString linkingState() const;
    QString linkingReason() const;
    QString linkingSource() const;
    QString currentMark() const;
    // Tags of the latest answer still in effect (skips, undone answers, undos, deletions and
    // interval changes are not answers), under their merged names; empty when the card was never
    // answered (see wasAnswered()) or that answer had no tag. Each card's review bar starts with them.
    QStringList currentTags() const;
    // Whether an answer is still in effect: false for a new card, or one whose answers were all undone.
    bool wasAnswered() const;
    // The answers still in effect, oldest first: the events currentTags() and wasAnswered() read
    // (not undone, and not skips, interval changes or deletions).
    QList<QJsonObject> answers() const;
    // An event's tags as shown and counted: recorded tags mapped through the tag merges.
    QStringList tagsOfEvent(const QJsonObject &event) const;

    QHash<QString, QString> tagMerges; // ReviewTags::mergeMapping of the synced merges.

    // Its kind (ReviewKinds::fold), as ReviewStore::cards() reads it. A project card's
    // schedule.dueAt is the capped due time (ReviewKinds::projectDueAt); scheduledDueAt is the
    // knowledge schedule's (invalid when it gives none).
    QString reviewKind = QStringLiteral("knowledge");
    QString reviewKindSource = QStringLiteral("default");
    QStringList kindProjectIds;
    QString projectsKind = QStringLiteral("knowledge");
    bool passageChanged = false; // Project cards only: "Changed since you last saw it".
    QDateTime scheduledDueAt;
    bool isProjectCard() const { return reviewKind == QLatin1String("project"); }
};

// The Project document a card belongs to (protocol/project-document-publication.md §4).
QString cardDocumentId(const ReviewCard &card, const QString &docsRoot);

/**
 * Cards Thoughts retired itself (server review_card_auto_retire_v1): when a card's whole passage
 * was removed from its note, the server writes an ordinary retire event (ReviewRetire) with
 * origin_id "thoughts-passage-check", reason "passage_removed" and
 * "passage_removed": {"source_files": [...], "pdf": "<Library PDF>"}; when the passage comes back,
 * a restore with reason "passage_returned". The Library tells the user once about each such
 * retirement still in effect; this Mac remembers which ones it told (shownNoticesPath()).
 */
namespace ReviewAutoRetire
{
QString originId();              // "thoughts-passage-check"
QString passageRemovedField();   // "passage_removed": the reason, and the field naming the note
int noticeDays();                // 7: an older retirement is not announced (a fresh Mac syncing history).
// Whether Thoughts wrote @p event (its origin_id is originId()).
bool isAutomatic(const QJsonObject &event);
struct Notice {
    QString eventId; // The retire event.
    QString cardId;
    QString question;
    QDateTime retiredAt;
    QString pdf;             // passage_removed.pdf, else the card's Library PDF.
    QStringList sourceFiles; // passage_removed.source_files (Library paths).
};
// The retirement by Thoughts in effect for @p card: its retire event is the one in effect
// (schedule.retireEventId) and came from originId(). Nothing for a card the user retired again
// since, or that was restored or deleted.
std::optional<Notice> retirementOf(const ReviewCard &card);
// The retirements of @p cards the user was not told about yet: in effect (retirementOf), not
// in @p shown (event_id -> when it was shown) and made at most noticeDays() before @p now.
// Oldest first.
QList<Notice> pendingNotices(const QList<ReviewCard> &cards, const QHash<QString, QDateTime> &shown, const QDateTime &now);
// The note a notice names: the name of the Library Project document of its PDF (@p projects as
// LibraryProjectStore::projects gives them), else the file name of its first source, else its PDF.
QString noteName(const Notice &notice, const QJsonArray &projects);
// This Mac's record of the notices shown: a JSON file of the application data, never synced.
QString shownNoticesPath();
// event_id -> when its notice was shown; empty when the file is missing or unreadable.
QHash<QString, QDateTime> shownNotices(const QString &path = shownNoticesPath());
// Adds @p notices to the record as shown at @p now; entries shown more than 30 days before @p now
// are dropped (their retirements are older than noticeDays() by then).
bool rememberShown(const QList<Notice> &notices, const QDateTime &now, const QString &path = shownNoticesPath());
}

// What a reader tab's "Copy for agent" adds about the PDF version on screen (@p pdfSha256): the
// cards of @p cards answered on that version (their answers given on it, with tags by @p groups
// and notes, and their answer feedback given on it, from @p answerFeedback), then the document
// feedback of @p textFeedback given on it. "Given on" a version: the record's review_version, or
// for feedback the PDF its passage was marked on, is that version (the user's rule: likes,
// dislikes and questions belong to their version). Comments never go to agents. Feedback and
// answers given on other versions are left out; deleted records and undone answers too. Empty when
// there is none.
QString agentVersionFeedback(const QList<ReviewCard> &cards, const QList<QJsonObject> &answerFeedback, const QList<QJsonObject> &textFeedback,
                             const QList<ReviewTags::TagGroup> &groups, const QString &pdfSha256);
// "Copy for agent" on an HTML page's tab: the document feedback of @p textFeedback given in the page
// @p path (its Library path) on its version @p pageSha256, without comments, then what each kind
// asks of the agent. Empty when there is none.
QString agentHtmlPageFeedback(const QList<QJsonObject> &textFeedback, const QString &path, const QString &pageSha256);


/**
 * Context notes: one shared free-form note per section, page or whole document, written from the
 * Tags window of any card of that document and shown on every other card of it. They replace
 * whole-document cards. Shared with the phone (thoughts-context-notes-SPEC.md):
 *
 *   "review_context_notes": {"section": {"key": "3 Method › 3.2 Loss", "label": "3.2 Loss", "text": "…"},
 *                            "page": {"key": "7", "label": "Page 7", "text": "…"},
 *                            "document": {"key": "", "label": "<display name>", "text": "…"}}
 *
 * The field rides in a review_event; a scope is present only when that answer changed it, and an
 * empty text clears the note. The note of (document, scope, key) now in effect is the text of the
 * newest event (reviewed_at, then event_id) of any card of that document that carries the scope
 * with that key, undone (retracted) and deleted events aside.
 */
namespace ReviewContextNotes
{
QString field();
QString sectionScope();
QString pageScope();
QString documentScope();
// Section, page, document: the order of the selector.
QStringList scopes();
// Between the titles of a section key, e.g. "3 Method › 3.2 Loss".
QString sectionSeparator();

// One PDF outline (bookmarks) entry: its title, its destination (1-based page, 0 when it has none;
// top as a fraction of the page height from the top) and its entries.
struct OutlineItem {
    QString title;
    int page = 0;
    double top = 0;
    QList<OutlineItem> children;
};
// From maps {title, page, top, children} (Part::reviewDocumentOutline()).
QList<OutlineItem> outlineFromVariant(const QVariantList &items);

struct Note {
    QString key;
    QString label;
    QString text;
    bool operator==(const Note &other) const = default;
};
// The section enclosing (page, top): the outline path, from the top level down to the deepest
// entry whose destination is at or before that position, its titles whitespace-collapsed and
// joined by sectionSeparator(); the label is the deepest title. False when no entry is.
bool sectionAt(const QList<OutlineItem> &outline, int page, double top, Note *section);
// The document a card's notes belong to: its document_id, else (only without one) its pdf_stem.
QString documentKey(const QJsonObject &cardRecord);
// The top of the card's primary region: the first page of its answer, and the highest top of its
// areas on that page.
bool primaryPosition(const ReviewCard &card, int *page, double *top);
// The page note's key and label for @p page.
Note pageNote(int page);
// The notes an event records, by scope; malformed entries are left out.
QHash<QString, Note> ofEvent(const QJsonObject &event);
// The event field for @p changes (by scope), texts cleaned like answer notes; empty when there is
// nothing to record.
QJsonObject toField(const QHash<QString, Note> &changes);
// "scope\nkey", the key of current()'s result.
QString noteId(const QString &scope, const QString &key);
// The notes in effect for the document @p documentKey: noteId -> text (cleared notes left out).
QHash<QString, QString> current(const QList<QJsonObject> &cardRecords, const QList<QJsonObject> &events, const QString &documentKey);

// What the Tags window edits for one answer: the card's section (when its PDF has one), page and
// document, each with the note now in effect and the text as typed.
struct Draft {
    struct Entry {
        QString scope;
        Note note; // text: the note in effect when the draft was made.
        QString text; // As typed.
        bool available = false;
    };
    QString cardId;
    QList<Entry> entries; // In scopes() order.
    int selected = 0;
    bool isEmpty() const { return entries.isEmpty(); }
    // The scopes whose text differs from the note in effect, with their cleaned text.
    QHash<QString, Note> changes() const;
};
}

struct ReviewImportResult {
    QList<ReviewCard> cards;
    QStringList errors;
};

enum class ReviewBrowserState {
    New,
    Learning,
    Review,
    Future,
    Skipped,
    Repairing,
    NeedsAttention,
    Stale,
    Excluded,
    Deleted,
    Retained,
    Candidate,
    Rejected,
    Retired,
    // Kept with its schedule, but its document is unpublished in its Project: not reviewed, and
    // not a problem of the card (listed with the retired cards, not as unavailable).
    Unpublished,
};

struct ReviewBrowserCard {
    ReviewCard card;
    ReviewBrowserState state = ReviewBrowserState::New;
    bool hasIssues = false;
    bool matchesFilter(const QString &filter) const;
    bool canReview() const;
};

enum class ReviewSkipScope { Card, Document, Project };

struct ReviewSkipBatch {
    QString id;
    QStringList cardIds;
    QDateTime until;
};

class ReviewStore
{
public:
    using ProjectionProvider = std::function<QJsonObject()>;

    explicit ReviewStore(QString docsRoot, ProjectionProvider projectionProvider = {}, QString syncStateRoot = {}, bool readOnly = false);
    ~ReviewStore();

    QString rootPath() const;
    QList<ReviewCard> cards(QStringList *errors = nullptr, const QString &pdfStem = {}) const;
    // The cards of @p cardIds only (deleted ones too, as cards() has them), read as cards() reads
    // them but without building every other card: for one card's refresh or its feedback's homes.
    QList<ReviewCard> cardsWithIds(const QSet<QString> &cardIds, QStringList *errors = nullptr) const;
    // How many times every card was built on the GUI thread (cards() without a document or IDs):
    // the work a refresh must keep off it (a Library of a few hundred cards takes about a second).
    static quint64 guiThreadFullLoads();
    QList<ReviewCard> activeCards(QStringList *errors = nullptr, const QString &pdfStem = {}) const;
    // The cards of @p cards that activeCards() returns, without reading the cards again. With
    // @p compareLibraryPdfs false a card made on another PDF than the Library's current one is
    // kept (that check reads the whole Library index); a reader tab compares the cards with the
    // PDF it shows instead.
    QList<ReviewCard> activeOf(const QList<ReviewCard> &cards, QStringList *errors = nullptr, bool compareLibraryPdfs = true) const;
    QList<ReviewCard> candidateCards(QStringList *errors = nullptr) const;
    QList<ReviewBrowserCard> browserCards(const QDateTime &now = QDateTime::currentDateTimeUtc(), QStringList *errors = nullptr) const;
    // browserCards() of @p cards, as cards() gave them, without reading them again.
    QList<ReviewBrowserCard> browserCardsOf(const QList<ReviewCard> &cards, const QDateTime &now = QDateTime::currentDateTimeUtc(), QStringList *errors = nullptr) const;
    QList<ReviewCard> dueCards(const QDateTime &now = QDateTime::currentDateTimeUtc(), const QString &pdfStemFilter = QString(), QStringList *errors = nullptr) const;
    // dueCards() of @p cards, as cards() gave them, without reading them again.
    QList<ReviewCard> dueCardsOf(const QList<ReviewCard> &cards, const QDateTime &now, const QString &pdfStemFilter = QString(), QStringList *errors = nullptr) const;
    // Whether an active card belongs in the review queue at @p now (the queue's own rule).
    static bool isDueNow(const ReviewCard &card, const QDateTime &now);
    // From when isDueNow() holds for @p card (UTC): isDueNow(card, now) is now >= dueFrom(card).
    // Invalid when never (a retired card); the Unix epoch when always (a new card without a date).
    static QDateTime dueFrom(const ReviewCard &card);
    // Rebuildable, read-only-to-consumers SQL projection of cards and their complete history.
    bool refreshQueryDatabase(const QString &databasePath, QString *error = nullptr) const;
    QJsonObject sourceCardCommand(const QJsonObject &request, const QString &bundleRoot);
    ReviewImportResult importAgentOutput(const QByteArray &output, const QVariantMap &document, int maxCandidates = 6);
    ReviewImportResult createBasicCards(const QVariantMap &document, int *skipped = nullptr);
    bool createManualCard(const QVariantMap &document,
                          int page,
                          const QVariantList &rect,
                          const QString &question,
                          const QString &pageVisualSha256,
                          ReviewCard *createdCard = nullptr,
                          QString *error = nullptr,
                          int firstDueDays = 0,
                          const QDateTime &customFirstDueAt = {},
                          const ReviewAnchorPlan *preparedPlan = nullptr, const QString &preparedCardId = {});
    // A card made from a selection in the HTML view (thoughts-html-view-SPEC.md): no page, rect or
    // regions; @p html carries created_in "html", html_edition_sha256, text_quote and optionally
    // source_range. Saved with source_link.state = pending_server_link: the server places its
    // markers at the next Sync and the next build gives it PDF regions.
    bool createHtmlCard(const QVariantMap &document, const QJsonObject &html, const QString &question, ReviewCard *createdCard = nullptr,
                        QString *error = nullptr, int firstDueDays = 0, const QDateTime &customFirstDueAt = {});
    // A card made from a selection in an HTML attachment page (thoughts-html-attachment-cards-SPEC.md):
    // @p attachment carries created_in "html_attachment", html_attachment {path, sha256},
    // text_quote and page_state {hash}; @p document names the owner (the PDF whose link opened the
    // page). No PDF geometry; source_link.state "html_attachment" (never linked to markers).
    bool createHtmlAttachmentCard(const QVariantMap &document, const QJsonObject &attachment, const QString &question, ReviewCard *createdCard = nullptr,
                                  QString *error = nullptr, int firstDueDays = 0, const QDateTime &customFirstDueAt = {});
    bool updateCardLocation(const QString &cardId, const QString &pdfSha256, int page, QString *error = nullptr);
    bool checkManualSourceLink(const ReviewCard &expected, const QString &sourcePath, QString *error = nullptr);
    bool markNeedsReanchor(const QString &cardId, QString *error = nullptr);
    // Removes the markers of deleted cards whose removal was left pending, once that is safe. Returns how many.
    int retryPendingMarkerRemovals(QStringList *errors = nullptr) const;
    // Except for a deletion, the event records the PDF on screen (reviewVersion()) and the passage the
    // answer is given on (ReviewKinds::passageStamp): from @p cardRecord when given (the caller's copy
    // of the card), else from the card read again. @p reviewPhases
    // (ReviewPhases::toField) is recorded with an answer given in two steps.
    bool recordRating(const QString &cardId, const QString &rating, const QDateTime &reviewedAt = QDateTime::currentDateTimeUtc(), QString *error = nullptr, const QDateTime &scheduledDueAt = {}, QString *recordedEventId = nullptr, const QString &reviewAction = {}, const QStringList &reviewTags = {},
                      const QHash<QString, QString> &groupNotes = {}, const QHash<QString, ReviewContextNotes::Note> &contextNotes = {},
                      const QJsonObject *cardRecord = nullptr, const QJsonObject &reviewPhases = {}, const QString &reviewEntry = {});
    // The version stamp of a new review event of the card (thoughts-context-notes-SPEC.md, addendum):
    // {"pdf_sha256": SHA-256 of the card's current PDF file, "source_built": built from a .tex/.md
    // source}. Empty when that PDF is not here. The hash is kept per file identity (path, size,
    // times, inode), so answering hashes a PDF at most once per version.
    QJsonObject reviewVersion(const QJsonObject &cardRecord) const;
    static QString reviewVersionField();
    // The context notes in effect for the document @p documentKey (ReviewContextNotes::current()).
    QHash<QString, QString> contextNotes(const QString &documentKey) const;
    // The section, page and document notes of @p card for its Tags window. @p outline is its PDF's
    // outline (none: no section), @p documentLabel the document's display name.
    ReviewContextNotes::Draft contextDraft(const ReviewCard &card, const QList<ReviewContextNotes::OutlineItem> &outline, const QString &documentLabel) const;
    // Whole-document cards (primary.scope "document") become document notes, once: each card not
    // deleted gets an event with the document note (its question, then its latest answer Note),
    // appended to the note already in effect, then is deleted like thoughtsctl delete-review-card.
    // Both events have IDs derived from the card ID, so a second run, or another device, does
    // nothing new. Returns the converted cards.
    struct ContextNoteConversion {
        QString cardId;
        QString documentKey;
        QString noteEventId;
        QString deleteEventId;
    };
    // @p documentLabel names a card's document in its note (default: the PDF's file name).
    QList<ContextNoteConversion> convertWholeDocumentCards(const std::function<QString(const ReviewCard &)> &documentLabel = {}, QStringList *errors = nullptr,
                                                           const QDateTime &now = QDateTime::currentDateTimeUtc());
    // How many cards, deleted ones aside, currently have each tag (ReviewCard::currentTags());
    // tags of earlier answers still in effect that no card has now are listed with 0.
    QHash<QString, int> reviewTagUsage() const;
    // The tag merges received through Sync, as ReviewTags::mergeMapping.
    QHash<QString, QString> tagMerges(QStringList *errors = nullptr) const;
    // Records that `from` are now shown as `to` on every device ("Compact tags"). Permanent:
    // it syncs as a review_tag_merge entity, and recorded answers keep their tags.
    bool recordTagMerge(const QStringList &from, const QString &to, QString *error = nullptr) const;
    // The tag groups as shown (ReviewTags::effectiveGroups with this store's merges); empty
    // when no valid record exists yet, which puts every tag in Other.
    QList<ReviewTags::TagGroup> tagGroups(QStringList *errors = nullptr) const;
    // Saves the whole record with a fresh updated_at and queues it for the server.
    bool saveTagGroups(const QList<ReviewTags::TagGroup> &groups, QString *error = nullptr) const;
    bool setInterval(const QString &cardId, const ReviewInterval &interval, bool never,
                     const QDateTime &selectedAt = QDateTime::currentDateTimeUtc(), QString *error = nullptr);
    bool retractRating(const QString &cardId, const QString &eventId, const QDateTime &retractedAt, QString *error) const;
    // Whether @p eventId is an answer of @p card still in effect, which its History can take back
    // with retractRating as Undo does: a rating (not a deletion, skip, interval or retire) that no
    // retraction of the card names, of a card that is not deleted. The fold reads it the same way.
    static bool answerInEffect(const ReviewCard &card, const QString &eventId);
    bool updateCardContent(const ReviewCard &expected, const QVariantList &rect, const QString &question, QString *error = nullptr);
    bool updateCardAnswer(const ReviewCard &expected, const QVariantList &regions, const QString &question, int pageCount, QString *error = nullptr);
    // Moves a card to new answer areas of the PDF @p pdfSha256: removes its markers from the
    // source, marks the passages of @p plan (planReviewSourceAnchors with replaceCardMarkers),
    // and saves the new page, regions and source anchor in one revision. The source change
    // carries a fresh transaction ID, so the server applies it as a replacement
    // (client_review_source_replacement_v1).
    bool replaceCardAreas(const ReviewCard &expected, const QString &pdfSha256, const QString &question,
                          const ReviewAnchorPlan &plan, QString *error = nullptr);
    // Merging review cards (Merge cards…, thoughtsctl merge-review-cards). @p cards are the
    // caller's copies of two or more cards of one document whose passages are marked in one
    // source file; a card changed since is refused. The kept card keeps its ID, schedule and
    // history, takes @p question and every card's passages as its own areas, in reading order;
    // text between the passages stays out. The other cards are deleted with their markers.
    // Their deletions (each removing its card's markers) and then the kept card's new areas are
    // queued in one batch, so one Sync carries the whole merge and no passage is ever marked by
    // two of the cards. The source must be a version the server has: the one its current PDF
    // was built from (found in @p bundleRoot), or the one the cards were placed on.
    struct CardMerge {
        QString keptId;
        QStringList deletedIds;      // In the order their deletions are queued.
        QStringList deleteEventIds;
        QStringList order;           // Every card, by its first passage in the source.
        QString sourceFile;          // Library path of the source.
        QString sourceSha256;        // The source after the merge.
        int passages = 0;            // The kept card's passages afterwards (its answer areas, for cards of a PDF).
        bool pdf = false;            // Cards of an imported PDF: no source; their areas become the kept card's.
    };
    // The checks mergeCards makes before writing anything: empty when @p cards can be merged,
    // else why not. @p preview gets the cards' reading order and their source. Cards marked in a
    // note's source merge there; cards with no source (an imported PDF) merge on the PDF alone.
    // @p syncFirst: set when the only obstacle is that the server lacks this Mac's latest source
    // (unsent edits, or the source check after a Sync): a Sync, then the merge again.
    QString cardMergeProblem(const QList<ReviewCard> &cards, const QString &bundleRoot, CardMerge *preview = nullptr, bool *syncFirst = nullptr) const;
    bool mergeCards(const QList<ReviewCard> &cards, const QString &keptId, const QString &question, const QString &bundleRoot,
                    QString *error = nullptr, CardMerge *result = nullptr);
    // The card that keeps its schedule unless the user chooses another: the one with the most
    // reviews, then the first in @p cards.
    static QString defaultMergeKeptCard(const QList<ReviewCard> &cards);
    // The merged card's question: each card's own, in the order of @p cards, without repeats.
    static QString joinedMergeQuestion(const QList<ReviewCard> &cards);
    bool updateQuestion(const QString &cardId, const QString &question, QString *error = nullptr);
    bool updateNote(const ReviewCard &expected, const QString &note, QString *error = nullptr);
    QHash<QString, QString> projectsByPdfStem() const;
    bool skip(const QString &cardId, ReviewSkipScope scope, const QDateTime &until, ReviewSkipBatch *batch,
              QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc(), bool dueOnly = false);
    bool undoSkip(const QString &skipId, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc());
    // Retire, "Restore to review" and their undo (ReviewRetire): one queue action each, written and
    // queued like a skip; the schedule is never changed. A card's action is recorded after its
    // earlier ones (1 ms later when the clock says otherwise). @p recordedEventId: the event,
    // for undoRetire(), which undoes a retire or a restore of this store's cards.
    bool retireCard(const QString &cardId, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc(), QString *recordedEventId = nullptr);
    bool restoreRetiredCard(const QString &cardId, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc(),
                            QString *recordedEventId = nullptr);
    bool undoRetire(const QString &eventId, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc());
    // Make project card, Make knowledge card, Follow the Project (ReviewKinds): one set_review_kind
    // queue action with @p kind (knowledge, project or follow_project), written and queued like a
    // retire, after the card's earlier kind events (1 ms later when the clock says otherwise).
    bool setCardKind(const QString &cardId, const QString &kind, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc(),
                     QString *recordedEventId = nullptr);
    // The active (not retired, not deleted) project cards that the Project @p projectId holds, of @p cards.
    static QList<ReviewCard> activeProjectCards(const QList<ReviewCard> &cards, const QString &projectId);
    // "Retire project cards…" of a Project: one retire for each of its active project cards, all with
    // the same reviewed_at, queued in one batch. @p retiredIds: the cards retired.
    bool retireProjectCards(const QString &projectId, QString *error = nullptr, const QDateTime &now = QDateTime::currentDateTimeUtc(),
                            QStringList *retiredIds = nullptr);
    bool setCardsDeleted(const QStringList &cardIds, bool deleted, const QDateTime &now, QString *error = nullptr);
    bool deleteCard(const QString &cardId, const QDateTime &deletedAt = QDateTime::currentDateTimeUtc(), QString *error = nullptr);
    bool restoreCard(const QString &cardId, const QDateTime &restoredAt = QDateTime::currentDateTimeUtc(), QString *error = nullptr);
    bool excludePage(const QString &pageVisualSha256, int page, const QString &pdfStem, QString *error = nullptr);
    // Answer feedback (review_card_feedback, part/reviewcardfeedback.h) of @p cardId, or of every
    // card when it is empty, in created_at order; queued writes are merged as the server merges them.
    QList<QJsonObject> cardFeedback(const QString &cardId = {}, bool includeDeleted = false, QStringList *errors = nullptr) const;
    // Document feedback (document_feedback, part/documentfeedback.h) of @p documentId, or of every
    // document when it is empty, in created_at order; queued writes are merged as the server merges them.
    QList<QJsonObject> documentFeedback(const QString &documentId = {}, bool includeDeleted = false, QStringList *errors = nullptr) const;
    // Feedback is shared by the cards of a document (ReviewCardFeedback::recordsForCard): the
    // cards other than @p cardId that answer feedback was made on and that exist (not deleted, no
    // candidate), card_id -> card record. With @p documentIds, only for feedback anchored in one
    // of them (or without a valid anchor document). Empty without such feedback (the cards are
    // read only then).
    QHash<QString, QJsonObject> feedbackHomeCards(const QString &cardId = {}, const QSet<QString> &documentIds = {}) const;
    // Whether @p card exists for its feedback: not deleted, and no candidate or rejected card.
    static bool feedbackCardExists(const ReviewCard &card);
    bool isExcluded(const ReviewCard &card) const;
    QJsonObject agentRequest(const QVariantMap &document,
                             const QString &agentModel = QString(),
                             const QString &agentReasoningEffort = QString(),
                             int maxCandidates = 6,
                             const QString &agentInstruction = QString()) const;

    static ReviewSchedule nextSchedule(const ReviewSchedule &previous,
                                       const QString &rating,
                                       const QDateTime &reviewedAt,
                                       bool hasPrevious = true,
                                       int schedulerVersion = 2,
                                       const QDateTime &scheduledDueAt = {});
    static QHash<QString, ReviewSchedule> foldEvents(const QList<QJsonObject> &events, QStringList *errors = nullptr);
private:
    // cards(), of @p pdfStem's cards or of @p cardIds' when given.
    QList<ReviewCard> loadCards(QStringList *errors, const QString &pdfStem, const QSet<QString> *cardIds) const;
    void migrateLegacyStore();
    void recoverSourceTransactions();
    bool saveQueueActions(const QList<QJsonObject> &actions, QString *error);
    bool recordRetireAction(const QString &cardId, const QString &action, const QString &undoneEventId, const QDateTime &now, QString *error,
                            QString *recordedEventId);
    void flushQueueActions() const;
    QJsonObject cardRecord(const QString &cardId, QString *error = nullptr) const;
    QJsonObject prepareCardUpdate(QJsonObject card) const;
    // @p transactionId identifies the source change; by default the card or event ID (a creation
    // or deletion). A later change of an existing card needs a fresh one.
    bool commitSourceChange(const QString &path, const QByteArray &before, const QByteArray &after,
                            const QString &relativePath, QJsonObject &object, QString *error, const QString &baseSha256 = {},
                            const QString &transactionId = {}) const;
    bool writeCard(const QJsonObject &card, QString *error) const;
    bool writeServerJson(const QString &relativePath, const QJsonObject &object, QString *error) const;
    // Card and event records ({relative_path, record}) written here and queued for the server
    // in one outbox batch, in this order.
    bool writeServerRecords(const QJsonArray &records, QString *error) const;
    struct CardMergePlan;
    QString prepareCardMerge(const QList<ReviewCard> &cards, QString keptId, const QString &bundleRoot, CardMergePlan *plan, bool *syncFirst = nullptr) const;
    bool mergePdfCards(const CardMergePlan &plan, const QJsonArray &regions, const QStringList &answers, const QString &question, QString *error, CardMerge *result);
    // @p projectionSnapshot, when given, is read instead of calling the projection provider again.
    QList<QJsonObject> records(const QString &entityType, const QString &collection, const QString &idField, QStringList *errors = nullptr, const QString &pdfStem = {},
                               const QJsonObject *projectionSnapshot = nullptr) const;
    QList<QJsonObject> events(QStringList *errors) const;
    QList<QJsonObject> exclusions(QStringList *errors = nullptr) const;
    QHash<QString, QString> canonicalPdfHashes(QStringList *errors = nullptr) const;

    bool m_readOnly = false;
    QString m_docsRoot;
    ProjectionProvider m_projectionProvider;
    std::unique_ptr<DesktopSyncOutbox> m_outbox;
};

}

#endif
