# "Comment missing" issue

A comment marker in a managed source whose comment no longer exists. Example: the comment
was deleted by the pre-2026-10-07 rule that deleted a comment when its marker vanished, and
the marker later came back. The reader shows nothing for such a marker, so it is dead text.
Thoughts lists it under Issues as **Comment missing**. It is the reverse of **Anchor removed**,
where the comment exists and its marker is gone.

Desktop reference: `part/commentmissingstate.h`, `FedoraRepository::refreshSourceComments`,
`removeMissingCommentMarkers` and `restoreMissingComments`.

## Detection

Run detection on each source refresh, and only when all of the following hold:

- **The comment store is loaded.** At least one Sync has completed: the store is bootstrapped,
  the cursor is above 0, or there are projections. Before that, nothing is reported. Otherwise
  comments that haven't been downloaded yet would look missing.
- **The file is a managed source.** It is a `.tex`, `.md` or `.markdown` file of a Project that
  builds at least one PDF. Skip ignored files, anything under `tex/generated/`, and unmanaged
  files.
- **The file's Project was scanned completely.** Every file in it was read, and its sources are
  settled.

### Which markers count

Use the same lexer as comment linking:

- **TeX:** `\CommentAnchor{id}{text}{…}`, `\CommentHighlight{id}{text}{words}` and
  `\CommentStrikethrough{id}{text}{words}`. Markers inside `%` comments, `verbatim`-like
  environments, or the old text of a pending agent proposal don't count. `\ReviewAnchor` is
  never reported.
- **Markdown:** a `<!-- thoughts:v1:meta {"key":"comment:<id>:…", "contents":…} -->` line with
  its point token, or its start and end tokens. Markers inside fenced code, indented code, or
  inline code don't count.

### When a marker's comment is missing

Compare ids by **identity**: strip a leading `thoughts-`, then strip surrounding `{}`, then
lowercase. `X`, `thoughts-X` and `thoughts-{X}` are the same comment.

A comment is **live** if either of these is true:

- A non-deleted `logical_comment` (root or reply) has this identity as its `comment_id` or its
  `scope.anchor_id`.
- A non-deleted legacy `annotation` record has this identity, and no `logical_comment` with the
  same id exists.

Count comments from the server snapshot, the outbox, and unsent local source journals.

A marker is **missing** when no live comment has its identity. Each occurrence is one Issues
row:

| Field | Value |
| --- | --- |
| `anchor_id` | The marker's id, exactly as written. |
| `source_file` | Path relative to the docs root. |
| `line` | Line of the marker. |
| `contents` | The marker's text (its second argument). |
| `selected_text` | The highlighted words. Highlight and strikethrough only. |
| `kind` | `anchor`, `highlight`, `strikethrough` or `markdown`. |
| `targets` | The PDFs the source builds. |
| `state` | `deleted` if a deleted root `logical_comment` matches, otherwise `unknown`. |
| `comment_id`, `deletion_reason` | Taken from the deleted row, when there is one. |

To find the deleted row, try an exact `comment_id` or `scope.anchor_id` match first, then a
match by identity.

On the desktop, this list is a local-only overlay entry, `local_issue\ncomment_missing\nmarkers`.
It is never queued or synced.

### Issues row

- **Status:** "Comment missing", with a broken-link icon.
- **Text:** the marker's text.
- **Source:** `file:line`.
- **Placement:** listed on the document the source builds, or under "No project" if that
  document isn't in the Library.
- **Visibility and count:** the row appears only under Issues and counts in the Issues badge.
- **Actions:** Edit, Reply and Delete aren't offered. Open source works.

## Actions

Re-check before every action. If the comment has become live in the meantime, refuse with
"…exists again". Refuse if the file is no longer a managed source.

### Remove marker (source edit only)

Remove the marker's single occurrence from that file:

- A highlight or strikethrough with words is replaced by its words.
- A marker with an empty or absent third argument is removed. If nothing but whitespace was
  before it, and nothing or a single `%` was after it, the whole line is removed. Otherwise
  only the command is removed, so text before it keeps its trailing `%`.

  Examples:
  - `Text \CommentAnchor{x}{t}{}%` becomes `Text %`.
  - A marker that is alone on its line removes the line.
- **Markdown:** remove the meta line and its tokens. A token line that becomes empty is removed.

The write is verified and rolled back on failure:

1. Keep the original bytes.
2. Refuse if the id occurs 0 times, or more than once, in the file.
3. Edit the file.
4. Re-scan the file. The id must be gone, and every other marker id must have the same number of
   occurrences. If either check fails, write back the original bytes and report "could not be
   verified".

No logical operation is queued. The source change is published with the next Sync.

### Restore comment

Recreate the comment from the marker as it is now. Read the marker again from the file, and
require exactly one occurrence. Then:

- `contents` is the marker's text.
- `selected_text` is the highlighted words (highlight and strikethrough only).
- `scope` is `{state: "source_linked", anchor_id: <marker id>, source_file, source_line: <line>,
  reason: "unique_source_marker"}`.

The source is not edited.

**Undelete** (`state: deleted`). Send one `logical_comment` operation for the deleted row:

- `entity_id`: the row's `comment_id`.
- `collection`: `comments`.
- `base_revision`: the row's current `revision`.
- `payload`: the row with these changes:
  - `deleted: false`
  - `revision`: base + 1
  - `updated_at`: now
  - `origin_id`: this client
  - `contents`, `selected_text` and `scope` as above
  - `deletion_reason`, `document_deletion` and `source_deletion` removed

Keep `comment_id`, `thread_root_id`, `parent_id` and `origin` unchanged; the server refuses any
change to them. Replies the server cascaded are **not** restored.

**Refuse locally** if the row carries `document_deletion`, has `deletion_reason ==
"document_deleted"`, or has a `source_deletion.sources[].deleted == true` entry. The server
answers 409 "The owning document was deleted…", and a 409 rejects the whole batch, which blocks
Sync. Show "The comment's document was deleted, so the server does not restore it. Remove the
marker instead."

**Create** (`state: unknown`). Send a new root with:

- `entity_id` = `comment_id` = `thread_root_id`: the marker id
- `base_revision: 0`
- `revision: 1`
- `parent_id: null`
- `deleted: false`
- `created_at` = `updated_at`: now
- `origin_id`: this client
- `author`: the user's identity name
- `subtype`: `Text` for an anchor or Markdown, `Highlight` for a highlight, `StrikeOut` for a
  strikethrough
- `priority: "normal"`
- `source_authority: "client-v1"`. Required: otherwise the server refuses a client-created
  `source_linked` comment.
- `contents`, `selected_text` and `scope` as above
- `origin`:
  - `collection`: the stem of the first target PDF (sorted) that has a document identity
  - `document_id`: a canonical UUIDv4 from the Library index
  - `pdf_sha256`: that PDF's current sha256
  - `page_index: 0`
  - `rect: []`
  - `fragments: []`

If no target PDF has an identity, refuse with "…not in the Library yet; Sync before restoring
this comment."

The server rejects a create (`base_revision 0`) for an id it already has as a tombstone. That is
why the deleted row, when known, is always undeleted rather than created.

Show the per-marker result message: success, or the reason for a refusal. After a successful
action, run the source refresh again so the row disappears.
