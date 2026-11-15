# Thoughts MCP actions

The local stdio MCP server exposes sixteen tools: the twelve below, which go through `thoughtsctl`, and four agent-note tools (see [Agent notes to the phone](#agent-notes-to-the-phone)).

- `create_review_card(source, expected_source_sha256, start_line, end_line, question, due_at, request_id)` saves an approved source-linked card locally with PDF placement pending.
- `move_review_card(card_id, source, expected_source_sha256, start_line, end_line, request_id, explanation=None, conversation_id=None)` moves a review card; it keeps its ID, question, review history and schedule. A PDF-only card (for example on an imported paper; `start_line < end_line`) moves at once into a source document of any Project, the selected range marked as its answer, PDF placement pending. A card linked to source markers (`start_line == end_line`, the line before which its passage goes; `explanation` and `conversation_id` required) moves through `thoughtsctl move-card-passage`: Thoughts writes two linked pending proposals, `agent-<uuid>.cut` removing the marked passage with its markers and `agent-<uuid>.paste` inserting it at the destination (same source, another source, another folder or Project), and the user accepts or rejects them as one (thoughts-server `protocol/agent-proposals.md` §12). It Syncs before and after like `create_proposal` and needs a server with `review_card_move_pairs_v1`.
- `merge_review_cards(card_ids, source, expected_source_sha256, request_id, keep_card_id=None, question=None)` merges two or more review cards of one document, whose passages are marked in `source`, into one (`thoughtsctl merge-review-cards`, the same merge as Merge cards… in the app): the kept card (by default the one with the most reviews) keeps its ID, schedule and review history, takes `question` (by default the cards' questions joined) and every card's passage as its own answer area, in reading order; the others are deleted with their markers. One change, made at once, with no proposals; the next Sync carries all of it (each other card's deletion removing its markers, then the kept card's new markers, in one outbox batch). Refused with `merge_refused` and the reason when the cards span several documents or source files, a card is not linked to a source passage, the cards are on different PDF versions, or the source has edits the server does not have yet.
- `list_review_cards(source=None, document=None, start_line=None, end_line=None)` lists cards and their schedule, mark, source link and PDF placement state for a source or exact Library filename.
- `search_review_cards(query=None, tags=None, tag_groups=None, project=None, document=None, state=None, sort=None, limit=20, include_answer=True, include_history=True)` finds cards across the Library by words in their question, answer, note, tags, document or project, ranked by relevance, with their current tags, schedule, answer excerpt and answer history. Read-only.
- `knowledge_profile(topic, project=None, document=None, limit=40)` classifies the cards matching a topic as solid, shaky, unknown or unreviewed and returns a short summary, so an agent can calibrate an explanation or edit to what the user knows. Read-only; see `tools/review-query/README.md` for the rules and the editable tag-to-level mapping.
- `open_document(document, page=1)` opens an absolute local PDF path or an exact Library filename at a one-based PDF page, reuses an existing tab where possible, and returns the actual page and page count.
- `start_sync(target=None)` starts the existing Thoughts Sync workflow, optionally scoped to an exact Library filename, and returns its operation ID immediately.
- `create_proposal(...)` captures a selected source range and writes a pending `agentProposedEdit` with a generated ID and UTC creation timestamp.
- `create_proposals(source, expected_source_sha256, proposals, conversation_id, request_id)` creates several pending proposals in one source in a single transaction; every `{start_line, end_line, replacement, explanation}` range refers to the version you read.
- `revise_proposal(...)` changes a pending proposal's replacement text and explanation, preserving its ID, creation timestamp, original-text argument, and the surrounding source.

- `link_conversation(project=None, source=None, document=None, review_card=None, title=None, conversation_id=None)` lists the calling Claude Code or Codex conversation under a Project's Agent conversations in the Library (`thoughtsctl link-conversation`; protocol: thoughts-server `protocol/agent-conversations.md`). The conversation is found from `conversation_id`, else from the Claude Code process that started the MCP (`~/.claude/sessions/<pid>.json`) or a parent `codex` process with exactly one open rollout. After a successful `create_proposal`, `create_proposals`, `revise_proposal`, `remove_proposal`, `create_review_card`, `move_review_card` or `open_document`, the adapter links the conversation to that Project by itself in the background (at most once per target every 10 minutes); a failed link never changes the tool's result, and an automatic link never launches Thoughts. With `review_card` (or automatically after `create_review_card` and `move_review_card`) the conversation is also linked to that review card: the card's bar shows its newest conversation above Flip | Reveal | Set knowledge, and the card's ⋮ menu lists all of them (`review_cards` entries of the record).

- `remove_proposal(...)` withdraws a pending proposal, removes its wrapper, and restores its captured original text; the next Sync records it as withdrawn by the agent, not rejected by the user.

The `thoughts://sync/{operation_id}` resource reads durable Sync status without starting another operation.
An accepted or queued response does not mean synchronization has completed.
A timed-out start must not be retried automatically because the app may already have accepted it.

MCP provides tool discovery, argument schemas, tool results, and resource discovery around the existing local command interface.
The adapter uses the official Python MCP SDK and launches `thoughtsctl` with argument arrays, without a shell or an HTTP listener.
The app's local command socket is restricted to the current user.
Proposal authoring runs locally in `thoughtsctl`, sharing the desktop proposal parser and transactional source/history writer.
Each proposal command first runs a Sync through the app and waits for it (`--timeout`, default 30 minutes; the MCP adapter uses 480 seconds), waiting out another active Sync or build.
It skips this Sync when a proposal Sync (before or delivery) succeeded in the last minute, so a burst of proposals syncs once; Syncs started in the app are not counted.
If the Sync does not succeed, nothing is written.
After writing, it returns `sync_after: "scheduled"` and leaves delivery to a detached `thoughtsctl deliver-proposals`: one at a time, it waits until no proposal has been written for 10 seconds, runs one Sync, and repeats if more proposals arrived meanwhile.
Delivery never launches Thoughts; if the app is not running, the next normal Sync delivers the proposals.
The timestamps live in `~/Library/Application Support/thoughtsctl/proposal-sync.json`.
`THOUGHTS_PROPOSAL_SYNC=0` skips both Syncs, so isolated tests need neither the app nor a server.
Opening a missing PDF or an invalid page returns an error; an out-of-range page can leave the requested document open but does not navigate it to a different page.

## Runtime and installation

Install `requirements.txt` in a Python virtual environment and register `server.py` as a stdio MCP server, using that environment's Python executable.
The default command helper is `/Applications/Thoughts.app/Contents/MacOS/thoughtsctl`.
Set `THOUGHTSCTL` when running against another installation or an isolated test helper.
The helper launches Thoughts if necessary.

To ship the adapter as a Codex or Claude Code plugin, copy `server.py`, `agent_notes.py` and `conversation_identity.py` into the plugin's `scripts/` directory (the adapter imports both from its own directory), together with the prompt hook `tools/hooks/thoughts_card_link.py`.
Point the plugin's `.mcp.json` at absolute interpreter and adapter paths (for example a virtual environment in `~/Library/Application Support/Thoughts/mcp-venv`), so startup depends neither on the working directory nor on downloading dependencies.
For Codex, also set `"tool_timeout_sec": 600`: proposal calls wait for a Sync, and Codex otherwise ends tool calls after its default timeout.
Start a new agent task after an update to discover the updated tool catalog.

The underlying CLI also works independently of MCP:

```sh
thoughtsctl open /absolute/path/document.pdf --page 3 --json
thoughtsctl open document.pdf --page 3 --json
thoughtsctl sync --json
thoughtsctl status OPERATION_ID --json
thoughtsctl create-proposal --json < request.json
thoughtsctl create-proposals --json < request.json
thoughtsctl revise-proposal --json < request.json
thoughtsctl remove-proposal --json < request.json
thoughtsctl search-review-cards --json < request.json
thoughtsctl knowledge-profile --json < request.json
thoughtsctl knowledge-levels [--init] --json
thoughtsctl list-todos --json < request.json
thoughtsctl add-todo --json < request.json
thoughtsctl edit-todo --json < request.json
```

`search-review-cards` and `knowledge-profile` run inside `thoughtsctl` against the review index (`thoughts-server/review-query.sqlite`) and the Library files, never through the app and never writing; they work when Thoughts is not running.
Retired cards (taken out of review by the user, history kept; thoughts-server `protocol/review-card-retire.md`) are returned with `status: "retired"` and `retired_at`, never as due or overdue; `state` filters `retired` and `active` (not retired) select them, and `knowledge_profile` still counts their history. `list-review-cards` marks them with `retired: true` and `retired_at`. Deleted cards are never returned.
The server instructions ask agents to call `knowledge_profile` once per topic before explaining or editing something the user may have studied, and to calibrate to it without changing cards.

## To-dos

- `list_todos(include_hidden=False)` lists the To-dos tab, newest first. Each to-do has `todo_id`, `revision`, `text`, `priority`, `due_date`, `due_time`, `hidden_until`, `hidden`, `where`, `created_at` and `updated_at`; `hidden_count` says how many are hidden until a later date. Diary entries and document comments are not listed.
- `add_todo(text, request_id, hidden_until=None, due_date=None, due_time=None, priority=None, where=None)` adds one. The to-do's ID comes from `request_id`, so retrying an uncertain call with the same arguments adds nothing (`replayed`); the same `request_id` with other content fails with `request_id_reused`.
- `edit_todo(todo_id, expected_revision, text=None, hidden_until=None, due_date=None, due_time=None, priority=None, where=None)` changes only the fields given; empty text removes a date, time or place. A to-do that changed since it was read fails with `todo_changed` and returns the current one in `todo`. An edit that is already in place returns `state: "unchanged"`. Other errors are `todo_unavailable`, `not_a_todo`, `queue_failed`, `invalid_request` and, for `list_todos`, `not_configured`.

These go through the running app (`thoughtsctl` starts it if it is closed) and queue the change as the Add To-do button and the to-do editor do, so it shows at once and the next Sync publishes it. They do not start Sync and they do not delete: a to-do has no done state, and finishing one means deleting it.

## Agent notes to the phone

While the user studies a review card with an agent, the agent can send a picture to the phone; the user draws on it in Thoughts and sends it back.
The contract is `protocol/agent-notes.md` in thoughts-server (branch `feature/agent-notes`).
These tools do not use `thoughtsctl` or the app: `agent_notes.py` (standard library only) talks to the Thoughts server directly with this Mac's device credential, `server_url` from `~/Library/Application Support/Thoughts/server.json` and the token from the login Keychain item `thoughts-server` / `thoughts-mac` (`THOUGHTS_SERVER_URL` and `THOUGHTS_TOKEN` override both; the token is never logged or returned).

- `send_note(title, image_path | html | html_path | markdown, message=None, review_card_id=None, document_id=None, document_filename=None, conversation_id=None, note_id=None)` sends exactly one picture: a PNG/JPEG file as is, or HTML/Markdown rendered on the Mac. It returns `note_id`, `delivery.live` (a phone is listening now) with `listeners`, the local `folder` and `image_path`, the image size, `warnings` and `next_step`, which says to wait with `get_note_reply` or, when no phone listens, to ask the user to open Thoughts on the phone (a phone that is not listening receives the note within about 15 minutes). Retrying with the same `note_id` and content does not send twice. The note's `origin` records the calling conversation (`conversation_agent`, `conversation_id`, `conversation_url`, `conversation_title`, found like `link_conversation`'s), which the phone's Agent notes open; `send_note` does not list the conversation under the card's or document's Project.
- `list_notes(review_card_id=None, document_id=None, limit=20)` lists notes, newest first, with their replies and `local_folder`. Read-only.
- `list_note_replies(note_id=None, after=None)` downloads new replies into the folder and lists them with local paths; with no arguments it lists the newest replies in the folder.
- `get_note_reply(note_id, wait_seconds=0)` returns the newest reply to a note, waiting up to 600 s (long-polls of at most 60 s) for one. The result has the reply's paths, a stroke summary (count, tools, colours) and the annotated image itself as MCP image content when it is at most 4 MB. When nothing arrived in time, `status` and `error_code` are `no_reply_yet`, which is not an error result.
- Who answers (server capabilities `agent_note_claims_v1`, `agent_note_owners_v1`, `agent_note_rules_v1`): `get_note_reply` and `get_note` claim the reply or page for the calling conversation when it covers it (its own note, or a topic it owns) and add `handling` (`state` `yours`, `taken`, `not_yours` or `unknown`, the claim, `next_step`) and, when it is theirs to answer, `answering_rules`: the user's rules, the Library note `agent-note-rules.md` as the server serves it, the same text on every computer. `note_rules()` shows them. `own_topic(label, document_ids, review_card_ids, title_tags, captures)` makes the conversation responsible for a topic while thoughts-watch-replies runs in it (its feed then holds the topic's replies, and PAGE lines are the topic's pages only); `release_topic(owner_id?)` ends it. The Thoughts reply agent on the server answers what no running agent covers, and after 10 minutes what a covering agent left unanswered.

Rendering uses headless Google Chrome (`/Applications/Google Chrome.app`, `THOUGHTS_CHROME` overrides) with a fresh temporary profile, never the user's: a 480 CSS px wide page at device scale 2.5, so the PNG is 1200 px wide.
A first pass (`--dump-dom` after `--virtual-time-budget`) lets scripts finish and reads the content height that an injected script writes into `<html data-thoughts-height>`; the second pass takes the screenshot at that height, clipped (with a warning) to 3276 CSS px so the image stays within 8192 px.
Markdown is wrapped in a page with pinned marked 12.0.2 and KaTeX 0.16.11 (auto-render for `$…$`, `$$…$$`, `\(…\)`, `\[…\]`). `agent_notes.py` downloads those files itself from cdn.jsdelivr.net the first time and keeps them (`~/Library/Caches/Thoughts/agent-note-assets`, `THOUGHTS_AGENT_NOTES_ASSETS` overrides), so only the first Markdown note needs the network.
Raw HTML is used as is, with a `width=480` viewport added when it has none; for `html_path`, the pictures and style sheets the file names inside its own folder are put into the page (`data:` URIs, `<style>`).
What an agent sends is not trusted (a page it read may have told it what to send), so the page is rendered where it can reach nothing. Chrome gets it from a proxy inside `agent_notes.py` under an origin that exists nowhere else (`http://thoughts-note.invalid/…`, never a `file://` page), and that proxy is Chrome's only route: it serves the page and, for Markdown, the KaTeX and marked files, and refuses every other request, loopback addresses included. The page's Content-Security-Policy allows inline styles and `data:` pictures only; scripts run only with a nonce made for that render, which the height script and the Markdown page's own scripts carry. So scripts in a note's HTML do not run and nothing loads from the network or from files; `warnings` names what was left out.
`image_path`, `html_path` and `import_pdf`'s `path` are resolved through symbolic links and refused (`path_refused`) when they lead into a hidden entry of the home directory (`~/.ssh`, `~/.aws`, `~/.config`, `~/.gnupg`, …), `~/Library`, a keychain or a system credential folder; every other path is read. `import_pdf`'s `url` must be https and resolve to public addresses only, also after redirects. The server address must be https; `http://` is accepted for this computer itself (localhost, 127.0.0.1, ::1). The Keychain token is sent only to the server that issued it, as `server.json` records (`token_origins`).
Chrome runs under `nice -n 10` and is killed after 45 s per pass. Errors are `renderer_unavailable` (no Chrome) or `render_failed`; both suggest sending a PNG via `image_path` instead.
Server errors keep the server's `error_code` (`invalid_agent_note`, `agent_note_too_large`, `unsupported_image`, `agent_note_not_found`, `agent_note_conflict`); local ones are `no_credentials`, `no_server_config`, `unauthorized`, `server_unreachable`, `server_timeout`, `server_error`, `not_found` (the server lacks the endpoint) and `invalid_response`.

Notes and replies are kept in `~/Thoughts/agent-notes/` (`THOUGHTS_AGENT_NOTES_DIR` overrides):

```
<YYYYMMDD-HHMMSS>-<slug>-<note8>/
  note.png | note.jpg   note.json   [note.html | note.md]
  reply-1.png  reply-1.strokes.json  reply-1.json
replies.jsonl     # one line per reply as it lands
.state.json       # reply-feed cursor (only ever increases)
.sync.lock        # flock of the one process that syncs the folder; holds its process ID
```

Every file is written to a temporary name and renamed; a reply's PNG is written last, so a `reply-*.png` means its set is complete.
Writers (several MCP servers, `watch`) serialize on `.lock`, and a reply already on disk (by `reply_id`) is never written twice; replies are numbered per note in the order they arrive.
A note sent from elsewhere gets its folder (with `note.json` and the image from the server) when its first reply is fetched.
While the MCP server runs and a note in the folder is younger than 24 hours, a background thread keeps the folder in step with the server, so an agent can also just watch the folder for `reply-*.png`.
It starts with the server when such a note exists and after any agent-note tool call.
Every agent session runs its own MCP server, but only one process syncs the folder at a time: the one holding the `flock` on `.sync.lock`.
The others make no requests for it; they try the lock every second, and since the operating system releases it when its holder exits, however it exits, another process takes over within about a second.
The holder asks first whether anything changed: `GET /v1/agent-notes?limit=1` with `If-None-Match`, which the server answers 304 while no note was added, delivered, opened or deleted and no reply was added.
Only after a change (and once more after replies came) does it pull the reply feed, as one long poll (`wait=50`) that returns as soon as a reply is there; the ETag is kept only once that pull succeeded.
Between looks it pauses 2 s, doubled each time nothing changed, up to 60 s; a change, or a note sent from this Mac (the folder changes), starts it at 2 s again. A server without the list ETag is long-polled at every look. Errors back off 2 s to 60 s and are logged on stderr only.
So the folder can be up to about a minute behind while nothing has happened for a while; tool calls never are: `list_notes`, `list_note_replies`, `get_note` and `get_note_reply` ask the server themselves (one request each, no loop).
Listening polls (`listen=true`: `get_note_reply` while it waits, `thoughts-watch-replies`) belong to their conversation, not to the folder sync, and every listening conversation sends its own.
MCP servers started before this change go on polling every 25 s beside the holder until their sessions end; both write the folder safely together.
Outside MCP:

```sh
python3 tools/mcp/agent_notes.py watch [--once]
python3 tools/mcp/agent_notes.py send --title "GRPO advantage" --markdown-file note.md --card CARD_ID
python3 tools/mcp/agent_notes.py replies [--note NOTE_ID]
```

`watch` prints one line per reply that lands in the folder, whichever process wrote it, and syncs the folder itself only while no other process does.

## Proposal authoring contract

All proposal tools require `source`, `expected_source_sha256`, `explanation`, `conversation_id`, and `request_id`.
Creation and revision also require `replacement`; removal takes `proposal_id` and does not accept replacement text.
`source` is an existing UTF-8 `.tex`, `.md`, or `.markdown` file inside the documentation workspace; the expected SHA-256 is calculated from its exact bytes before preparing the request.
`explanation` is plain text, escaped by Thoughts when written into a proposal wrapper.
Creation stores the conversation UUID in metadata; revision and removal preserve the original provenance and record the acting conversation in history.
`replacement` is source text without an outer proposal wrapper; use one sentence per physical prose line.
An empty replacement proposes deletion.

Creation also takes one-based `start_line` and exclusive `end_line` boundaries.
For example, `start_line=2, end_line=4` captures lines 2 and 3 exactly, including their line endings; equal boundaries insert before a line without replacing existing text.
The final EOF boundary is available whether or not the last line ends with a newline.
`create_proposals` takes the same source, hash, conversation and request fields, plus `proposals`: 1–500 objects with exactly `start_line`, `end_line`, `replacement` and `explanation`.
All their line boundaries refer to the one source version named by the hash, so an agent need not renumber lines after earlier edits or reread the hash between them.
Ranges may be given in any order and may touch, but must not overlap each other or a pending proposal, and two insertions at the same boundary are rejected.
Either every proposal is written, in one source/history transaction, or none is; each gets its own ID and is accepted or rejected separately.
The result's `proposals` lists each proposal's ID, line, original text, replacement and explanation in request order, with one `source_sha256` for the new version.
Revision instead takes `proposal_id` and replaces only the explanation and replacement argument contents, leaving all other proposal bytes intact.
For TeX, include a newline after a trailing comment so it cannot consume the argument delimiter; unmatched braces and nested proposal commands are rejected.
For Markdown, Thoughts stores the captured original and provenance in an encoded HTML comment marker, shows the proposed text between markers, and checks that accept and reject decisions restore the exact selected bytes.
Legacy proposals without creation metadata must be migrated before revision.
Removal also supports legacy proposals and restores the captured original text exactly, including an empty original for additions.
It preserves neighbouring source and proposals, keeps a retry receipt, and records a `withdraw` decision claim (origin `mcp`) that the next Sync sends with the source change, so the server retires the proposal as withdrawn.
This is distinct from a user rejection. Agents never accept or reject proposals: `thoughtsctl decide-proposal` is for editor integrations acting on the user's click and is not an MCP tool.

The service rejects stale source versions, overlapping selections, invalid argument structure, and duplicate proposal IDs elsewhere in the source workspace, including snapshots.
Generated files take no new or revised proposals (`generated_source`): a path with a `tex/generated/` segment, or a first non-empty line starting with `% Generated artifact` or `% Generated by ` (Markdown: `<!-- Generated artifact` or `<!-- Generated by `). Change the generator or its inputs instead; existing proposals there can still be removed.

Links a proposal adds must resolve as the PDF build resolves them (`invalid_link`, nothing written): `\PdfLink{pdfKey}{anchor}{text}` reaches only a PDF of the document's own Project; another Project's PDF needs `\CrossPdfLink{docs-<folder>/pdfKey}{anchor}{text}`; the anchor is `document-start` or a `doc-anchor:` annotation of that PDF; `\SourceLink`/`\CrossSourceLink` need a selected target document that defines the Source label; the retired `\TexLink`/`\CrossTexLink` are refused (reason `retired_link`, with the `\SourceLink`/`\CrossSourceLink` that replaces it in `suggestion` when the target is known). Markdown writes the same four links as relative paths and they are checked the same way: `[text](other.md#source-label:label)` (a `.md`, `.markdown` or `.tex` document of the same folder; without a fragment its start), `[text](../../docs-<folder>/path.md#source-label:label)` (a document of another folder), `[text](pdfKey.pdf#anchor)` (a bare filename always names a PDF of the same Project; no fragment is `document-start`) and `[text](../../docs-<folder>/pdfKey.pdf#anchor)` (a PDF of another Project, through the root of one of its docs-* folders); suggestions for Markdown sources are Markdown links. A fragment that is no Source label is reported (`fragment_not_source_label`); other relative Markdown links and images must exist in the folder. Each entry of `links` gives `reason`, `message` and, when the target exists elsewhere, the corrected command in `suggestion`. Links already in the replaced text are not checked. A link that still cannot resolve (for example written by hand) stays inactive in its PDF and is reported in that document's server status; it never blocks Sync.

IDs are generated as UUIDs and checked against `.tex`, `.md`, and `.markdown` sources; directory symlinks are not followed and targets outside the workspace are rejected.
These structural checks do not judge the factual correctness of the proposed text or enforce every editorial convention.
Direct filesystem writes remain possible outside this API.

Use a fresh UUID `request_id` for each logical operation.
After an uncertain result, retry only with the same ID and identical arguments; the durable history returns the original result with `replayed=true` instead of writing again.
A replay's source hash describes the original completed operation, so reread the source before making a subsequent edit.
Reusing the ID with different arguments is rejected.
Creation and revision success mean **saved locally as a pending proposal**.
Removal success means **withdrawn locally with the original text restored**.
None of these operations accepts a proposal.
If the Sync before writing changes the source, the request fails as stale; reread the source and send a new request.

The Thoughts documents root is `$THOUGHTS_HOME`, defaulting to `~/Thoughts/docs` (`%USERPROFILE%\Thoughts\docs` on Windows); the app and the CLI read the same variable. It is a trusted process-level setting for an alternate workspace or isolated tests, not a tool argument.

To install only the rebuilt command helper into a stopped macOS app without replacing its shell, core, part or generators:

```sh
cmake --build build-thoughts --target thoughtsctl
cmake -DCMAKE_INSTALL_COMPONENT=thoughts-agent-cli -DCMAKE_INSTALL_LOCAL_ONLY=1 \
  -DTHOUGHTS_AGENT_CLI_BUNDLE=/Applications/Thoughts.app \
  -P build-thoughts/shell/cmake_install.cmake
```

This generated installer verifies the bundle and stopped state, installs the helper, and uses the existing persistent signer.
It exists only in builds configured with `THOUGHTS_MACOS_PERSISTENT_SIGNING` on (see `tools/macos/README.md`).

## Validation

Run `python tools/mcp/test_server.py` and `python tools/mcp/test_agent_notes.py` using the MCP environment, and `python3 tools/hooks/test_thoughts_card_link.py` (standard library only).
The folder-sync tests compare with the code before the sync had one holder (commit `32b727779`, read with `git show`) and are skipped in a clone without that history; `THOUGHTS_TEST_LOAD=1` adds a measurement of six minutes: the sync's requests per minute for 1, 5 and 20 processes, before and after.

## Review card prompt hook

Most conversations about a review card start with its pasted Copy for agent text and may never call a Thoughts tool. `tools/hooks/thoughts_card_link.py` is a `UserPromptSubmit` hook for Claude Code (`~/.claude/settings.json`) and Codex (`~/.codex/hooks.json`, trusted once in `/hooks`): a prompt with `Review card: <uuid>` links the conversation to that card with `thoughtsctl link-conversation` (`card_linked_by: "prompt"`) in a detached process, so the prompt never waits. Any other prompt costs one Python start-up (~20–30 ms). It prints nothing (hook output would join the conversation), never launches Thoughts, and logs to `~/Library/Logs/thoughts-card-link.log` (rotated at 1 MB). It needs `conversation_identity.py` next to it (installed) or in `../mcp` (repository). Register it as a command such as `/usr/bin/python3 ~/plugins/thoughts/scripts/thoughts_card_link.py` (wherever the plugin installs it); Codex is detected from its `turn_id` input field.
The agent-note tests use a fake Thoughts server on 127.0.0.1, a temporary notes folder and a private HOME, and inject the Keychain lookup and the Chrome runner; `THOUGHTS_TEST_CHROME=1` additionally runs the real Chrome: `test_real_chrome_reaches_no_file_and_no_network` gives it a page that tries local files, loopback and remote addresses and scripts (no download), and `test_real_chrome` renders Markdown with math and HTML (it downloads KaTeX and marked once). On Linux point `THOUGHTS_CHROME` at Chromium (e.g. `/usr/bin/chromium-browser`); `test_real_chrome_paints_a_short_note_to_its_last_line` checks the Linux command line (no host rule, `--password-store=basic`).
It exercises real MCP initialization, discovery, tool calls and resource reads against a temporary command stub, including strict page validation, filenames containing shell syntax, asynchronous Sync status and application errors.
It does not open the real app or perform synchronization.
Set `THOUGHTS_TEST_CLI` to the built or installed helper to additionally exercise real MCP creation/revision/removal in a temporary documentation root, including exact original preservation, retries, stale reads, and malformed proposals.
The same variable enables the proposal Sync tests, which run `thoughtsctl` against a fake app socket in a private HOME: one Sync before and one after a burst, waiting out a busy app, and no write when the first Sync fails.
Run `proposalauthoringtest` for the C++ mutation and parser tests.
The shell regression `testOpenDocumentCommand` verifies real PDF navigation, tab reuse, page bounds and invalid document inputs with isolated application settings.

## Conversation provenance

New proposals use `[created=<timestamp>,conversation_id=<UUID>]` in the existing optional TeX argument.
The four required arguments and PDF layout are unchanged.
The conversation ID identifies the creator and is copied into proposal history snapshots and published metadata.
Revisions preserve the complete metadata argument and record the acting conversation as `agent_conversation_id` in the immutable history event and command receipt.
The command result's `conversation_id` identifies the proposal origin, while `agent_conversation_id` identifies the current operation.
Legacy motivation provenance remains readable and is preserved when revising an older proposal.
When the MCP server can find a link that reopens the conversation, creation also writes `,conversation_url=<link>` after the conversation ID (agent-proposals spec §3.6; Markdown: a `conversation_url:` header line).
A Claude Code session started through Remote Control gets `https://claude.ai/code/session_…`, read from its running `~/.claude/sessions/<pid>.json` (`bridgeSessionId`); a Codex thread with a rollout file under `~/.codex/sessions` or `archived_sessions` gets `codex://threads/<UUID>`; anything else gets no link.
thoughtsctl accepts `conversation_url` only on create; revisions keep the stored link.
The desktop proposal window and the phone open the link with Open in Claude / Open in Codex (the phone opens Claude links only).
Deploy readers (server, desktop, Android and the VS Code review extension) before enabling the updated authoring CLI, since older parsers reject the new optional field.
Existing notes and immutable history events do not require migration.

## Source card creation

Select a nonempty, continuous range of complete source blocks using one-based start and exclusive end lines.
The current source hash must match, and the source must belong to one registered Library document.
The question is 1–500 characters and `due_at` is an ISO timestamp with timezone for now or a future review.
Creation preserves the selected prose and inserts checked review markers through the existing recoverable source/card transaction.
It refuses overlap with cards or pending proposals.
A saved card appears immediately, but is excluded from the review queue until an explicit Sync and successful build verify PDF placement.
Creation never starts Sync or a build.
Retry uncertain requests with the same UUID and identical arguments.

```sh
thoughtsctl create-review-card --json < request.json
thoughtsctl list-review-cards --json < query.json
```
