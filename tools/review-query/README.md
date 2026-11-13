# Querying Thoughts review cards

Thoughts Desktop maintains a SQLite projection at `~/Library/Application Support/Thoughts/thoughts-server/review-query.sqlite`.
The file is created when Thoughts starts and refreshed after local card or review changes and after synchronized changes arrive.
It can be read by another program under the same macOS account while Thoughts is running.
Open it read-only because Thoughts owns updates and can rebuild its contents.

```sh
sqlite3 -readonly "$HOME/Library/Application Support/Thoughts/thoughts-server/review-query.sqlite"
```

`review_cards` contains one row per card identifier, including deleted cards.
Its `state` is `new`, `correct`, `skip`, `wrong`, `unavailable`, or `deleted` when that state can be derived from the stored review and placement data.
Its `review_count` is the scheduler's repetition count after retractions, and `due_at` is an ISO 8601 UTC timestamp or NULL.
Its `card_json` contains the complete stored card object, including answer-region evidence.
`review_events` contains one row per event identifier, including queue actions, interval selections, and retractions.
Its `is_retracted` flag identifies events withdrawn by a later event, while `event_json` preserves the complete event object.
An event's `origin_id` identifies the client that recorded it.
The same identifier can appear in local and server storage without becoming a second event.

For example, the following SQL finds cards with at least two unretracted ratings literally named `good`.

```sql
SELECT c.card_id, c.question, COUNT(*) AS good_ratings
FROM review_cards AS c
JOIN review_events AS e USING (card_id)
WHERE e.rating = 'good' AND e.is_retracted = 0
GROUP BY c.card_id
HAVING COUNT(*) >= 2;
```

The following SQL finds cards due before a selected UTC date with at least three repetitions.

```sql
SELECT card_id, question, due_at, review_count
FROM review_cards
WHERE is_deleted = 0 AND due_at < '2026-10-01T00:00:00.000Z' AND review_count >= 3;
```

The index reflects this Desktop installation's current local and received server state.
Other clients' pending changes appear after those clients synchronize and Desktop pulls them.
`review_meta` records when the indexed content last changed.
`review_tag_records` holds the tag merges (`review_tag_merge`, by `merge_id`) and the tag groups record (`review_tag_groups`, `groups`) exactly as synchronized, so readers can show each answer's tags under their merged names and group them.
Indexes written by an older Thoughts lack this table until Thoughts refreshes them.
`review_feedback` holds the answer feedback (`review_card_feedback`, thoughts-server `protocol/review-card-feedback.md`): one row per liked or disliked passage of a card's answer, with `polarity` (`positive` or `negative`), the quoted passage, the comment, `created_at`, `updated_at`, `deleted` (1 for a deleted record, kept for studies) and the whole synchronized record in `record_json`.
Indexes written before it lack the table until Thoughts refreshes them; readers then report no feedback.
`review_card_retirements` lists the retired cards (thoughts-server `protocol/review-card-retire.md`), one row each: `card_id`, `retired_at` (the retire's `reviewed_at`) and `retire_event_id`.
A retired card is never reviewed but keeps its row in `review_cards`, its history and its schedule; its retire, restore and `undo_retire` events are rows of `review_events` with that `queue_action` and no rating.
Indexes written before index version 5 lack the table until Thoughts refreshes them.
`document_feedback` holds the document feedback (`document_feedback`, thoughts-server `protocol/document-feedback.md`): one row per liked or disliked passage marked while reading a Library PDF, with no card.
Each row has `document_id` and `pdf_sha256` (the document and the exact PDF version it was marked on), `page` (the page of the first anchor region in that version), `polarity`, the quoted passage, the comment, `created_at`, `updated_at`, `deleted` and the whole synchronized record in `record_json`.
Indexes written before index version 6 lack the table until Thoughts refreshes them.

```sql
SELECT c.card_id, c.question
FROM review_cards AS c
WHERE c.is_deleted = 0 AND c.card_id NOT IN (SELECT card_id FROM review_card_retirements);
```
Each row keeps the card it was made on (`card_id`), but feedback is shared by the cards of a document: `thoughtsctl search-review-cards` and `list-review-cards` also list, on every other card of the same document, the feedback whose passage lies in that card's answer (made on the card's PDF with its lines inside the answer regions, else with its quote found in the answer's text), with `"shared": true` and the `card_id` it was made on.

```sql
SELECT c.question, f.polarity, f.quote, f.comment
FROM review_feedback f JOIN review_cards c USING (card_id)
WHERE f.deleted = 0 AND c.is_deleted = 0
ORDER BY f.created_at;
```

## Searching cards and profiling knowledge

`thoughtsctl search-review-cards` and `thoughtsctl knowledge-profile` (and the MCP tools of the same names) read this index without writing or creating any file.
While Thoughts is not rewriting it they read it without locks, and keep the result only if the file did not change meanwhile; when Thoughts keeps a write-ahead log open they read through it with `mode=ro&readonly_shm=1`.
They also read the Library's project catalogue, the sources between each card's mask markers, and, for cards without a source passage, the answer regions of the card's PDF, as "Copy for agent" does.
So they work while Thoughts runs and when it does not, showing the cards as of `indexed_at`.

```sh
echo '{"query": "GRPO advantage", "limit": 5}' | thoughtsctl --json search-review-cards
echo '{"topic": "SDPO"}' | thoughtsctl knowledge-profile
thoughtsctl knowledge-levels          # the tag-to-level mapping in effect
thoughtsctl knowledge-levels --init   # write it to a file you can edit
```

`search-review-cards` reports each card's `status` (`due`, `overdue`, `scheduled`, `never` or `retired`, the last with `retired_at`); its `state` filter is `new`, `due`, `overdue`, `untagged_last`, `reviewed`, `retired` or `active`, and `new`, `due` and `overdue` never include retired cards.
`knowledge-profile` classifies each matching card as solid, shaky, unknown or unreviewed.
Each answer still in effect counts at the lowest level among its tags that the mapping lists; the latest answer weighs 1, the one before 1/2, then 1/4 and so on.
The weighted mean (solid 1, shaky 0.5, unknown 0) loses 0.1 per lapse (at most 0.3), gains 0.1 for an interval of 30 days or more, and loses 0.15 when the card is overdue by more than its interval (at least 7 days).
At 0.75 or more a card is solid, from 0.4 shaky, below that unknown.
A card answered only with tags the mapping does not list counts 0.8 with an interval of 21 days or more and no lapse, else 0.5.
The mapping is `~/Library/Application Support/Thoughts/knowledge-tag-levels.json` (`{"schema_version": 1, "levels": {"<tag>": "solid" | "shaky" | "unknown" | "ignore"}}`); without that file a built-in default mapping is used and the output says `"source": "default mapping"`.
Only `knowledge-levels --init` creates the file.
