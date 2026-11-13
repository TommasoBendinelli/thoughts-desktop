# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Protocol integration tests using an isolated command stub, never the real app."""
import asyncio
import json
import hashlib
import os
from pathlib import Path
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def private_home(directory):
    """HOME and CFFIXED_USER_HOME in `directory`: thoughtsctl's state (the
    decision claims in desktop-sync.sqlite) must never reach the real one."""
    home = Path(directory) / 'home'
    home.mkdir(exist_ok=True)
    return {'HOME': str(home), 'CFFIXED_USER_HOME': str(home)}


def session_instructions(initialized):
    return initialized.instructions or ''


def claim_databases(directory):
    return sorted(Path(directory).rglob('desktop-sync.sqlite'))


class ServerTest(unittest.IsolatedAsyncioTestCase):
    async def test_tools_and_status_over_stdio(self):
        with tempfile.TemporaryDirectory() as directory:
            stub = Path(directory) / 'thoughtsctl'
            log = Path(directory) / 'calls.jsonl'
            stub.write_text('#!' + sys._base_executable + '\n' + '''import json, os, sys
from pathlib import Path
args = sys.argv[1:]
assert args[0] == '--json', args
with Path(os.environ['CALL_LOG']).open('a') as out:
    out.write(json.dumps(args) + '\\n')
result = {'schema_version': 1, 'accepted': True, 'terminal': True}
if args[1] == 'open':
    assert args[2:5] == ['--page', '2', '--'], args
    result.update(state='opened', page=2, document=args[5], page_count=3)
elif args[1] == 'sync':
    if '--target' in args:
        result.update(accepted=False, state='busy', error_code='busy')
    else:
        result.update(state='queued', terminal=False, operation_id='test-operation')
elif args[1] == 'create-proposal':
    # Proposal calls give the CLI time for the Sync it runs before writing.
    assert args[2:] == ['--timeout', '480'], args
    result.update(state='proposal_created', echo=json.load(sys.stdin))
elif args[1] in ('create-review-card', 'move-review-card', 'merge-review-cards', 'list-review-cards', 'search-review-cards', 'knowledge-profile', 'list-todos', 'add-todo', 'edit-todo', 'open-agent-notes'):
    result.update(echo=json.load(sys.stdin))
elif args[1] == 'remove-proposal':
    result.update(state='proposal_removed', origin=os.environ.get('THOUGHTS_REQUEST_ORIGIN'), echo=json.load(sys.stdin))
elif args[1] == 'status':
    assert args[2:] == ['--', 'test-operation'], args
    result.update(state='succeeded', operation_id='test-operation')
print(json.dumps(result))
''')
            stub.chmod(0o755)
            # A private notes folder: the agent-note reply watcher never reaches the real server.
            # Private Claude and Codex folders: no conversation is detected, so nothing is linked automatically.
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))], env={**os.environ, 'THOUGHTSCTL': str(stub), 'CALL_LOG': str(log),
                                                                                                                         'THOUGHTS_AGENT_NOTES_DIR': str(Path(directory) / 'agent-notes'),
                                                                                                                         'CLAUDE_CONFIG_DIR': str(Path(directory) / 'claude'),
                                                                                                                         'CODEX_HOME': str(Path(directory) / 'codex')})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    tools = (await session.list_tools()).tools
                    self.assertEqual({t.name for t in tools}, {'open_document', 'open_agent_notes', 'start_sync', 'create_proposal', 'create_proposals', 'revise_proposal', 'remove_proposal', 'create_review_card', 'move_review_card', 'merge_review_cards', 'create_project', 'import_pdf', 'list_review_cards',
                                                               'search_review_cards', 'knowledge_profile',
                                                               'send_note', 'list_notes', 'list_note_replies', 'get_note_reply', 'get_note', 'link_conversation',
                                                               'list_todos', 'add_todo', 'edit_todo'})
                    for name in ('search_review_cards', 'knowledge_profile', 'list_todos'):
                        self.assertTrue(next(t for t in tools if t.name == name).annotations.readOnlyHint)
                    instructions = session_instructions(await session.initialize())
                    self.assertIn('call knowledge_profile once for that topic', instructions)
                    self.assertIn('never create, change or review cards', instructions)
                    self.assertIn('call link_conversation once for that Project', instructions)
                    self.assertIn('call link_conversation once with review_card=<id>', instructions)
                    link_tool = next(t for t in tools if t.name == 'link_conversation')
                    self.assertFalse(link_tool.annotations.readOnlyHint)
                    self.assertTrue(link_tool.annotations.idempotentHint)
                    # Proposal links are validated; \\PdfLink finds a Library PDF in any Project (2026-11-10).
                    self.assertIn('error_code invalid_link', instructions)
                    self.assertIn('\\PdfLink{pdfKey}{anchor}{text} finds the Library PDF pdfKey.pdf in any Project', instructions)
                    self.assertIn('every note defines \\PdfLink', instructions)
                    self.assertNotIn('reaches only a PDF of the same Project', instructions)
                    for name in ('create_proposal', 'create_proposals', 'revise_proposal'):
                        self.assertIn('invalid_link', next(t for t in tools if t.name == name).description)
                    # The retired \TexLink / \CrossTexLink are not taught as link forms.
                    self.assertNotIn('TexLink', instructions + ''.join(t.description or '' for t in tools))
                    searched = await session.call_tool('search_review_cards', {'query': 'GRPO $(advantage)', 'tags': ['Correct'], 'tag_groups': ['Understanding'],
                                                                               'state': 'due', 'sort': 'last_reviewed', 'limit': 5, 'include_history': False})
                    self.assertFalse(searched.isError, searched.content)
                    self.assertEqual(searched.structuredContent['echo'], {'query': 'GRPO $(advantage)', 'tags': ['Correct'], 'tag_groups': ['Understanding'],
                                                                          'state': 'due', 'sort': 'last_reviewed', 'limit': 5, 'include_answer': True, 'include_history': False})
                    self.assertEqual((await session.call_tool('search_review_cards', {})).structuredContent['echo'],
                                     {'limit': 20, 'include_answer': True, 'include_history': True})
                    # Deleted answer feedback only on request (review-card-feedback.md §7).
                    self.assertEqual((await session.call_tool('search_review_cards', {'include_deleted': True})).structuredContent['echo'],
                                     {'limit': 20, 'include_answer': True, 'include_history': True, 'include_deleted': True})
                    # Retired cards (review-card-retire.md section 7): their own state and status.
                    for state in ('retired', 'active'):
                        self.assertEqual((await session.call_tool('search_review_cards', {'state': state})).structuredContent['echo'],
                                         {'state': state, 'limit': 20, 'include_answer': True, 'include_history': True})
                    search_tool = next(t for t in tools if t.name == 'search_review_cards')
                    self.assertIn('status (due, overdue, scheduled, never or retired)', search_tool.description)
                    self.assertIn('retired_at', search_tool.description)
                    self.assertEqual(search_tool.inputSchema['properties']['state']['anyOf'][0]['enum'],
                                     ['new', 'due', 'overdue', 'untagged_last', 'reviewed', 'retired', 'active'])
                    self.assertIn('Retired cards', next(t for t in tools if t.name == 'knowledge_profile').description)
                    self.assertIn('retired_at', next(t for t in tools if t.name == 'list_review_cards').description)
                    for name in ('search_review_cards', 'list_review_cards'):
                        self.assertIn('anchor_state', next(t for t in tools if t.name == name).description)
                        # Shared feedback: the card it was made on, and whether that is another card.
                        self.assertIn('card_id (the card the feedback was made on) and shared', next(t for t in tools if t.name == name).description)
                    profiled = await session.call_tool('knowledge_profile', {'topic': 'SDPO', 'project': 'course-notes'})
                    self.assertFalse(profiled.isError, profiled.content)
                    self.assertEqual(profiled.structuredContent['echo'], {'topic': 'SDPO', 'project': 'course-notes', 'limit': 40})
                    before = log.read_text()
                    for invalid in ({'state': 'forgotten'}, {'sort': 'random'}, {'limit': 0}, {'limit': 201}, {'limit': 1.5}, {'tags': 'Correct'}):
                        self.assertTrue((await session.call_tool('search_review_cards', invalid)).isError, invalid)
                    for invalid in ({}, {'topic': ''}, {'topic': 'x', 'limit': 0}):
                        self.assertTrue((await session.call_tool('knowledge_profile', invalid)).isError, invalid)
                    self.assertEqual(log.read_text(), before)
                    opened = await session.call_tool('open_document', {'document': '/tmp/a space;$(literal).pdf', 'page': 2})
                    self.assertFalse(opened.isError, opened.content)
                    self.assertEqual(opened.structuredContent['page'], 2)
                    before = log.read_text()
                    for page in (0, -1, 1.5, True, '2'):
                        result = await session.call_tool('open_document', {'document': '/tmp/test.pdf', 'page': page})
                        self.assertTrue(result.isError, page)
                    for name in ('../test.pdf', '-test.pdf', 'https://example.com/test.pdf'):
                        self.assertTrue((await session.call_tool('open_document', {'document': name})).isError)
                    self.assertEqual(log.read_text(), before)
                    # Agent Notes for the user: a project's notes, a note selected; never both filters.
                    shown = await session.call_tool('open_agent_notes', {'project': 'MethodsComparison', 'note_id': 'n-1'})
                    self.assertFalse(shown.isError, shown.content)
                    self.assertEqual(shown.structuredContent['echo'], {'project': 'MethodsComparison', 'note_id': 'n-1'})
                    self.assertEqual((await session.call_tool('open_agent_notes', {})).structuredContent['echo'], {})
                    before = log.read_text()
                    self.assertTrue((await session.call_tool('open_agent_notes', {'project': 'a', 'document': 'b.pdf'})).isError)
                    self.assertTrue((await session.call_tool('open_agent_notes', {'document': ''})).isError)
                    self.assertEqual(log.read_text(), before)
                    request = dict(source='/tmp/literal $(text).md', expected_source_sha256='a' * 64,
                                   start_line=2, end_line=4, question='What is $(literal)?',
                                   due_at='2099-01-01T12:00:00Z', request_id=str(uuid.uuid4()))
                    created = await session.call_tool('create_review_card', request)
                    self.assertFalse(created.isError, created.content)
                    self.assertEqual(created.structuredContent['echo'], request)
                    listed = await session.call_tool('list_review_cards', {'source': request['source'], 'start_line': 2, 'end_line': 4})
                    self.assertFalse(listed.isError, listed.content)
                    self.assertEqual(listed.structuredContent['echo']['source'], request['source'])
                    for invalid in ({}, {'start_line': 2}, {'source': request['source'], 'start_line': 4, 'end_line': 2}):
                        self.assertTrue((await session.call_tool('list_review_cards', invalid)).isError)
                    for value in (0, True, 1.5, '2'):
                        self.assertTrue((await session.call_tool('create_review_card', {**request, 'start_line': value})).isError)
                    listed = await session.call_tool('list_todos', {})
                    self.assertFalse(listed.isError, listed.content)
                    self.assertEqual(listed.structuredContent['echo'], {'include_hidden': False})
                    todo = dict(text='Call the bank', request_id=str(uuid.uuid4()))
                    added = await session.call_tool('add_todo', todo)
                    self.assertFalse(added.isError, added.content)
                    self.assertEqual(added.structuredContent['echo'], todo)
                    change = dict(todo_id='thoughts-1', expected_revision=2, due_date='', priority='high')
                    changed = await session.call_tool('edit_todo', change)
                    self.assertFalse(changed.isError, changed.content)
                    self.assertEqual(changed.structuredContent['echo'], change)
                    for invalid in ({'due_date': 'tomorrow'}, {'priority': 'urgent'}, {'expected_revision': 0}, {'expected_revision': '2'}, {'todo_id': 'a}b'}, {'hidden_until': '2026-1-1'}, {'due_time': '9am'}):
                        self.assertTrue((await session.call_tool('edit_todo', {**change, **invalid})).isError, invalid)
                    self.assertTrue((await session.call_tool('add_todo', {**todo, 'request_id': 'not-a-uuid'})).isError)
                    self.assertTrue((await session.call_tool('add_todo', {**todo, 'text': ''})).isError)
                    move = dict(card_id=str(uuid.uuid4()), source=request['source'], expected_source_sha256='a' * 64,
                                start_line=2, end_line=4, request_id=str(uuid.uuid4()))
                    moved = await session.call_tool('move_review_card', move)
                    self.assertFalse(moved.isError, moved.content)
                    self.assertEqual(moved.structuredContent['echo'], move)
                    self.assertTrue((await session.call_tool('move_review_card', {**move, 'card_id': 'not-a-card'})).isError)
                    merge = dict(card_ids=[str(uuid.uuid4()), str(uuid.uuid4())], source=request['source'], expected_source_sha256='a' * 64,
                                 request_id=str(uuid.uuid4()))
                    merged = await session.call_tool('merge_review_cards', merge)
                    self.assertFalse(merged.isError, merged.content)
                    self.assertEqual(merged.structuredContent['echo'], merge)
                    merge_tool = next(t for t in tools if t.name == 'merge_review_cards')
                    self.assertTrue(merge_tool.annotations.destructiveHint)
                    self.assertIn('never merge cards with a deletion and an edit', merge_tool.description)
                    self.assertIn('call merge_review_cards once', instructions)
                    for invalid in ({'card_ids': merge['card_ids'][:1]}, {'card_ids': [merge['card_ids'][0].upper(), merge['card_ids'][1]]},
                                    {'keep_card_id': 'not-a-card'}, {'question': ''}, {'question': 'x' * 501}):
                        self.assertTrue((await session.call_tool('merge_review_cards', {**merge, **invalid})).isError, invalid)
                    proposal = dict(source='/tmp/literal $(text).tex', expected_source_sha256='a' * 64, start_line=2, end_line=3,
                                    replacement='Proposed.\n', explanation='Test.', conversation_id='0190a000-0000-7000-8000-00000000000a',
                                    request_id=str(uuid.uuid4()))
                    proposed = await session.call_tool('create_proposal', proposal)
                    self.assertFalse(proposed.isError, proposed.content)
                    self.assertEqual(proposed.structuredContent['echo'], proposal)
                    sync = await session.call_tool('start_sync', {})
                    self.assertFalse(sync.isError)
                    self.assertFalse(sync.structuredContent['terminal'])
                    status = await session.read_resource(sync.structuredContent['status_uri'])
                    self.assertEqual(json.loads(status.contents[0].text)['state'], 'succeeded')
                    busy = await session.call_tool('start_sync', {'target': 'paper.pdf'})
                    self.assertTrue(busy.isError)
                    self.assertEqual(busy.structuredContent['error_code'], 'busy')
                    # A withdrawal is marked as the agent's own (a `withdraw` claim with origin mcp).
                    removal = dict(source=proposal['source'], expected_source_sha256='a' * 64, proposal_id='agent-1', explanation='Obsolete.',
                                   conversation_id=proposal['conversation_id'], request_id=str(uuid.uuid4()))
                    removed = await session.call_tool('remove_proposal', removal)
                    self.assertFalse(removed.isError, removed.content)
                    self.assertEqual(removed.structuredContent['origin'], 'mcp')
                    # Agents never decide proposals: decide-proposal is not a tool.
                    self.assertNotIn('decide', ' '.join(t.name for t in tools))
                    self.assertNotIn('history', next(t.description for t in tools if t.name == 'remove_proposal'))
                    for name in ('create_proposal', 'create_proposals', 'revise_proposal'):
                        self.assertIn('tex/generated/', next(t.description for t in tools if t.name == name))

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
    async def test_proposal_links_must_resolve_with_real_cli(self):
        # 2026-10-12: create_proposal accepted a \\PdfLink the build could not resolve and every Sync failed.
        # Since 2026-11-10 the build finds a Library PDF by its name in any Project, so a \\PdfLink to
        # another Project's PDF is checked against that PDF.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            counter = iter(range(1, 1000))
            # A readable one-page PDF without doc-anchor: annotations.
            objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
                       b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] >>']
            paper, offsets = b'%PDF-1.4\n', []
            for number, body in enumerate(objects, 1):
                offsets.append(len(paper))
                paper += b'%d 0 obj\n' % number + body + b'\nendobj\n'
            xref = len(paper)
            paper += b'xref\n0 4\n0000000000 65535 f \n' + b''.join(b'%010d 00000 n \n' % offset for offset in offsets)
            paper += b'trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % xref

            def project(project_id, name, folder, sources, pdfs):
                digest = hashlib.sha256(project_id.encode()).hexdigest()
                default = f'.thoughts/projects/{digest}/documents'
                ids = lambda: f'{digest[:8]}-0000-4000-8000-{next(counter):012d}'
                default_id, folder_id = ids(), ids()
                documents, rendering = [], {}
                for source in sources:
                    documents.append({'document_id': ids(), 'source': {'folder_id': folder_id, 'path': source}, 'format': 'latex', 'included': True})
                for pdf in pdfs:
                    document_id = ids()
                    documents.append({'document_id': document_id, 'source': {'folder_id': default_id, 'path': pdf}, 'format': 'pdf', 'included': True})
                    (root / default).mkdir(parents=True, exist_ok=True)
                    (root / default / pdf).write_bytes(paper)
                    rendering[document_id] = {'sha256': hashlib.sha256(paper).hexdigest()}
                (root / default).mkdir(parents=True, exist_ok=True)
                (root / default / 'thoughts-project.json').write_text(json.dumps({
                    'schema_version': 5, 'kind': 'thoughts-project', 'id': project_id, 'name': name, 'default_folder_id': default_id,
                    'source_folders': [{'folder_id': default_id, 'path': default, 'document_id': ids()},
                                       {'folder_id': folder_id, 'path': folder, 'document_id': ids()}],
                    'documents': documents, 'rendering': rendering}))

            source = root / 'docs-concepts/tex/source/grpo.tex'
            source.parent.mkdir(parents=True)
            source.write_text('Intro.\nClaim.\n')
            (root / 'docs-followup/tex/source').mkdir(parents=True)
            (root / 'docs-followup/tex/source/main.tex').write_text('Main.\n')
            project('test:concepts', 'concepts', 'docs-concepts', ['tex/source/grpo.tex'], [])
            project('test:followup', 'Benchmark-followup', 'docs-followup', ['tex/source/main.tex'], ['SFT_paper.pdf'])
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))],
                env={**os.environ, 'THOUGHTSCTL': os.environ['THOUGHTS_TEST_CLI'], 'THOUGHTS_HOME': directory,
                     'THOUGHTS_PROPOSAL_SYNC': '0', **private_home(directory)})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    request = dict(source=str(source), expected_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                   start_line=2, end_line=3, replacement='Claim, see \\PdfLink{SFT_paper}{paper-title}{Chu et al.}.\n',
                                   explanation='Cite the paper.', conversation_id='7304c669-0000-4000-8000-000000000000', request_id=str(uuid.uuid4()))
                    refused = await session.call_tool('create_proposal', request)
                    self.assertTrue(refused.isError)
                    self.assertEqual(refused.structuredContent['error_code'], 'invalid_link')
                    link = refused.structuredContent['links'][0]
                    self.assertEqual(link['reason'], 'anchor_not_found')
                    self.assertEqual(link['suggestion'], '\\PdfLink{SFT_paper}{document-start}{Chu et al.}')
                    self.assertIn('SFT_paper.pdf has no doc-anchor:paper-title annotation', refused.structuredContent['message'])
                    self.assertEqual(source.read_text(), 'Intro.\nClaim.\n')
                    missing = await session.call_tool('create_proposal', {**request, 'request_id': str(uuid.uuid4()), 'replacement': 'Claim, see \\PdfLink{SFT_paper_v2}{document-start}{Chu}.\n'})
                    self.assertTrue(missing.isError)
                    self.assertEqual(missing.structuredContent['links'][0]['reason'], 'pdf_not_in_library')
                    self.assertIn('similar PDF keys: SFT_paper', missing.structuredContent['message'])
                    self.assertEqual(source.read_text(), 'Intro.\nClaim.\n')
                    accepted = await session.call_tool('create_proposal', {**request, 'request_id': str(uuid.uuid4()), 'replacement': 'Claim, see ' + link['suggestion'] + '.\n'})
                    self.assertFalse(accepted.isError, accepted.content)
                    self.assertIn('\\PdfLink{SFT_paper}{document-start}{Chu et al.}', source.read_text())

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
    async def test_linked_card_move_over_stdio_with_real_cli(self):
        """thoughtsctl knows move-card-passage; without a server that decides move pairs it writes nothing."""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'docs-a' / 'b.tex'
            source.parent.mkdir()
            source.write_text('First.\n')
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))],
                env={**os.environ, 'THOUGHTSCTL': os.environ['THOUGHTS_TEST_CLI'], 'THOUGHTS_HOME': directory,
                     'THOUGHTS_PROPOSAL_SYNC': '0', **private_home(directory)})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    moved = await session.call_tool('move_review_card', dict(
                        card_id='6f1c2a0e-3b4d-4c5e-8f60-718293a4b5c6', source=str(source),
                        expected_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), start_line=1, end_line=1,
                        request_id=str(uuid.uuid4()), explanation='Merge.', conversation_id='0190a000-0000-7000-8000-00000000000a'))
                    self.assertTrue(moved.isError, moved.content)
                    self.assertEqual(moved.structuredContent['error_code'], 'server_unsupported')
                    self.assertEqual(source.read_text(), 'First.\n')

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
    async def test_batch_proposals_over_stdio_with_real_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'batch.tex'
            original = 'One.\nTwo.\nThree.\n'
            source.write_text(original)
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))],
                env={**os.environ, 'THOUGHTSCTL': os.environ['THOUGHTS_TEST_CLI'], 'THOUGHTS_HOME': directory,
                     'THOUGHTS_PROPOSAL_SYNC': '0', **private_home(directory)})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    request = dict(source=str(source), expected_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                   proposals=[dict(start_line=3, end_line=4, replacement='3.\n', explanation='Digits.'),
                                              dict(start_line=1, end_line=2, replacement='1.\n', explanation='Digits too.')],
                                   conversation_id='0190a000-0000-7000-8000-00000000000a', request_id=str(uuid.uuid4()))
                    extra = await session.call_tool('create_proposals', {**request, 'proposals': [{**request['proposals'][0], 'proposal_id': 'x'}]})
                    self.assertTrue(extra.isError)
                    overlap = await session.call_tool('create_proposals', {**request, 'request_id': str(uuid.uuid4()),
                                                                             'proposals': [request['proposals'][0], {**request['proposals'][0], 'replacement': 'Other.\n'}]})
                    self.assertTrue(overlap.isError, overlap.content)
                    self.assertIsNotNone(overlap.structuredContent, overlap.content)
                    self.assertEqual(overlap.structuredContent['error_code'], 'proposal_overlap')
                    self.assertEqual(source.read_text(), original)
                    response = await session.call_tool('create_proposals', request)
                    self.assertFalse(response.isError, response.content)
                    created = response.structuredContent
                    self.assertEqual(created['state'], 'proposals_created')
                    self.assertEqual([p['original_text'] for p in created['proposals']], ['Three.\n', 'One.\n'])
                    self.assertEqual(created['source_sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
                    self.assertEqual(source.read_text().count('\\agentProposedEdit['), 2)
                    replay = await session.call_tool('create_proposals', request)
                    self.assertTrue(replay.structuredContent['replayed'])

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
    async def test_proposals_over_stdio_with_real_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'note with spaces.tex'
            original = 'First sentence.\nOriginal café sentence.\nLast sentence.\n'
            source.write_text(original)
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))],
                env={**os.environ, 'THOUGHTSCTL': os.environ['THOUGHTS_TEST_CLI'], 'THOUGHTS_HOME': directory,
                     'THOUGHTS_PROPOSAL_SYNC': '0', **private_home(directory)})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    request = dict(source=str(source), expected_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                   start_line=2, end_line=3, replacement='Proposed sentence.\n',
                                   explanation='Fix 50% & {literal} syntax; $(not-a-shell-command).',
                                   conversation_id='0190a000-0000-7000-8000-00000000000a', request_id=str(uuid.uuid4()))
                    for value in (True, 1.5, '2', 0):
                        invalid = await session.call_tool('create_proposal', {**request, 'start_line': value})
                        self.assertTrue(invalid.isError)
                    self.assertEqual(source.read_text(), original)
                    response = await session.call_tool('create_proposal', request)
                    self.assertFalse(response.isError, response.content)
                    created = response.structuredContent
                    self.assertEqual(created['original_text'], 'Original café sentence.\n')
                    self.assertEqual(created['source_sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
                    self.assertEqual(created['conversation_id'], request['conversation_id'])
                    self.assertIn(',conversation_id=' + request['conversation_id'], source.read_text())
                    self.assertNotIn('Prepared in Codex conversation', source.read_text())
                    first_bytes = source.read_bytes()
                    replay = await session.call_tool('create_proposal', request)
                    self.assertTrue(replay.structuredContent['replayed'])
                    self.assertEqual(source.read_bytes(), first_bytes)
                    stale = await session.call_tool('create_proposal', {**request, 'request_id': str(uuid.uuid4())})
                    self.assertTrue(stale.isError)
                    self.assertEqual(stale.structuredContent['error_code'], 'source_conflict')
                    revision = {key: value for key, value in request.items() if key not in ('start_line', 'end_line')}
                    revision.update(proposal_id=created['proposal_id'], expected_source_sha256=created['source_sha256'],
                                    conversation_id='0190b000-0000-7000-8000-00000000000b',
                                    request_id=str(uuid.uuid4()), replacement='Revised $x$ sentence.\n', explanation='Improve the proposed wording.')
                    response = await session.call_tool('revise_proposal', revision)
                    self.assertFalse(response.isError, response.content)
                    revised = response.structuredContent
                    for field in ('proposal_id', 'created_at', 'original_text', 'conversation_id'):
                        self.assertEqual(revised[field], created[field])
                    self.assertEqual(revised['agent_conversation_id'], revision['conversation_id'])
                    self.assertEqual(revised['replacement'], 'Revised $x$ sentence.\n')
                    self.assertEqual(revised['source_sha256'], hashlib.sha256(source.read_bytes()).hexdigest())
                    final_bytes = source.read_bytes()
                    invalid = await session.call_tool('revise_proposal', {**revision, 'request_id': str(uuid.uuid4()),
                        'expected_source_sha256': revised['source_sha256'], 'replacement': r'\agentProposedEdit{nested}{Why.}{Old.}{New.}'})
                    self.assertTrue(invalid.isError)
                    self.assertEqual(source.read_bytes(), final_bytes)
                    self.assertEqual(source.read_text().count('\\agentProposedEdit'), 1)
                    self.assertIn('Original café sentence.', source.read_text())
                    removal = {key: value for key, value in revision.items() if key != 'replacement'}
                    removal.update(expected_source_sha256=revised['source_sha256'], request_id=str(uuid.uuid4()),
                                   explanation='Withdraw this obsolete proposal.')
                    response = await session.call_tool('remove_proposal', removal)
                    self.assertFalse(response.isError, response.content)
                    self.assertEqual(response.structuredContent['state'], 'proposal_removed')
                    self.assertEqual(source.read_text(), original)
                    # A file outside any Project folder is never uploaded, so the
                    # withdrawal records no decision claim, in the private home or elsewhere.
                    self.assertEqual(claim_databases(directory), [])
                    replay = await session.call_tool('remove_proposal', removal)
                    self.assertTrue(replay.structuredContent['replayed'])
                    self.assertEqual(source.read_text(), original)
                    # Generated files take no proposals.
                    generated = Path(directory) / 'tex/generated/table.tex'
                    generated.parent.mkdir(parents=True)
                    generated.write_text('One.\nTwo.\n')
                    refused = await session.call_tool('create_proposal', {**request, 'source': str(generated), 'request_id': str(uuid.uuid4()),
                        'expected_source_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(), 'start_line': 1, 'end_line': 2})
                    self.assertTrue(refused.isError)
                    self.assertEqual(refused.structuredContent['error_code'], 'generated_source')
                    self.assertEqual(generated.read_text(), 'One.\nTwo.\n')
                    # Proposal history is no longer kept; only the retry record remains.
                    self.assertFalse((Path(directory) / 'comments/.history/proposal_events.jsonl').exists())
                    requests = json.loads((Path(directory) / 'comments/.history/agent-requests.json').read_text())
                    self.assertEqual(requests[removal['request_id']]['result']['state'], 'proposal_removed')



class FakeApp(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    """Answers thoughtsctl's sync and status commands like the app's command socket."""
    daemon_threads = True

    def __init__(self, path, fail=False, busy_first=False, build_failed=False):
        self.syncs, self.fail, self.busy_first, self.lock = [], fail, busy_first, threading.Lock()
        self.build_failed = build_failed
        self.decisions = []
        self.todos = []

        class Handler(socketserver.StreamRequestHandler):
            def handle(handler):
                request = json.loads(handler.rfile.readline())
                handler.wfile.write((json.dumps(self.answer(request)) + '\n').encode())
        super().__init__(str(path), Handler)

    def answer(self, request):
        with self.lock:
            if request['command'] == 'decide-proposal':
                self.decisions.append(request)
                if request.get('expected_source_sha256') != 'a' * 64:
                    return dict(schema_version=1, accepted=False, terminal=True, state='failed', error_code='source_changed', message='The source changed.')
                return dict(schema_version=1, accepted=True, terminal=True, state='accepted' if request['action'] == 'accept' else 'rejected',
                            proposal_id=request['proposal_id'], source=request['source'], source_sha256='b' * 64, claim='recorded', sync='pending', replayed=False)
            if request['command'] in ('list-todos', 'add-todo', 'edit-todo'):
                self.todos.append(request)
                return dict(schema_version=1, accepted=True, terminal=True, state='done')
            if request['command'] == 'open-agent-notes':
                self.todos.append(request)
                return dict(schema_version=1, accepted=True, terminal=True, state='opened')
            if request['command'] == 'sync':
                if self.busy_first:
                    self.busy_first = False
                    return dict(schema_version=1, accepted=False, state='busy', terminal=False, error_code='busy')
                self.syncs.append(time.monotonic())
                return dict(schema_version=1, accepted=True, reused=False, operation_id=f'op-{len(self.syncs)}', state='syncing', terminal=False)
            if self.build_failed:
                # As the app reports a Sync whose sources synced but whose PDF build failed.
                return dict(schema_version=1, accepted=True, operation_id=request['operation_id'], state='failed', terminal=True,
                            failure_code='pdf_build_failed', sources_synced=True,
                            message='PDF build failed Your changes are synced with the server. The last successful PDF remains available. '
                                    'renderer exited with 1: paper.pdf — latex_error: Source docs-paper/tex/source/main.tex:3 failed')
            state = 'failed' if self.fail else 'succeeded'
            result = dict(schema_version=1, accepted=True, operation_id=request['operation_id'], state=state, terminal=True, message=state,
                          sources_synced=not self.fail)
            if self.fail:
                result['failure_code'] = 'sync_failed'
            return result


@unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
class ProposalSyncTest(unittest.TestCase):
    """thoughtsctl against a fake app in a private HOME, so the real app and Library are never touched."""

    def library(self, **fake):
        home = Path(tempfile.mkdtemp(dir='/tmp', prefix='thoughts-ps-'))
        self.addCleanup(shutil.rmtree, home, True)
        (home / 'docs').mkdir()
        self.source = home / 'docs/note.tex'
        self.source.write_text('One.\nTwo.\n')
        socket_dir = home / 'Library/Application Support/Thoughts'
        socket_dir.mkdir(parents=True)
        app = FakeApp(socket_dir / 'thoughts-command.sock', **fake)
        threading.Thread(target=app.serve_forever, daemon=True).start()
        self.addCleanup(app.server_close)
        self.addCleanup(app.shutdown)
        self.env = {k: v for k, v in os.environ.items() if k != 'THOUGHTS_PROPOSAL_SYNC'}
        self.env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), THOUGHTS_HOME=str(home / 'docs'))
        return app

    def propose(self, text):
        end = len(self.source.read_text().splitlines()) + 1
        request = dict(source=str(self.source), expected_source_sha256=hashlib.sha256(self.source.read_bytes()).hexdigest(),
                       start_line=end, end_line=end, replacement=text, explanation='Test.',
                       conversation_id='0190a000-0000-7000-8000-00000000000a', request_id=str(uuid.uuid4()))
        done = subprocess.run([os.environ['THOUGHTS_TEST_CLI'], '--json', 'create-proposal', '--timeout', '30'],
                              input=json.dumps(request), capture_output=True, text=True, env=self.env, timeout=60)
        return json.loads(done.stdout)

    def test_burst_shares_one_sync_before_and_one_after(self):
        app = self.library(busy_first=True)
        first = self.propose('Added.\n')
        self.assertEqual(first['state'], 'proposal_created', first)
        self.assertEqual(first['sync_before'], 'op-1')
        self.assertEqual(first['sync_after'], 'scheduled')
        second = self.propose('Also added.\n')
        self.assertEqual(second['state'], 'proposal_created', second)
        self.assertEqual(second['sync_before'], 'skipped_recent')
        deadline = time.monotonic() + 40
        while len(app.syncs) < 2 and time.monotonic() < deadline:
            time.sleep(0.5)
        time.sleep(3)
        self.assertEqual(len(app.syncs), 2, 'one Sync before and one delivery Sync after the burst')

    def test_failed_pdf_build_does_not_block_proposals(self):
        # 2026-10-14: a failed build blocked the proposals that would fix it.
        app = self.library(build_failed=True)
        first = self.propose('Added.\n')
        self.assertEqual(first['state'], 'proposal_created', first)
        self.assertNotIn('error_code', first)
        self.assertEqual(first['sync_before'], 'op-1')
        self.assertEqual([warning['code'] for warning in first['warnings']], ['pdf_build_failed'], first)
        self.assertEqual(first['warnings'][0]['sync'], 'before')
        self.assertIn('latex_error', first['warnings'][0]['message'])
        self.assertIn('Added.', self.source.read_text())
        # The Sync counted as synced: the next proposal skips its Sync before.
        second = self.propose('Also added.\n')
        self.assertEqual(second['state'], 'proposal_created', second)
        self.assertEqual(second['sync_before'], 'skipped_recent')
        self.assertNotIn('warnings', second)
        # One delivery Sync after the burst, and it counts as delivered too.
        deadline = time.monotonic() + 40
        while len(app.syncs) < 2 and time.monotonic() < deadline:
            time.sleep(0.5)
        time.sleep(3)
        self.assertEqual(len(app.syncs), 2, 'one Sync before and one delivery Sync after the burst')
        state = json.loads((Path(self.env['HOME']) / 'Library/Application Support/thoughtsctl/proposal-sync.json').read_text())
        self.assertGreater(state['delivered_through_ms'], state['last_write_ms'])

    def test_failed_sync_before_writes_nothing(self):
        app = self.library(fail=True)
        original = self.source.read_bytes()
        result = self.propose('Added.\n')
        self.assertEqual(result['error_code'], 'sync_before_failed', result)
        self.assertEqual(self.source.read_bytes(), original)
        time.sleep(1)
        self.assertEqual(len(app.syncs), 1)


@unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
class DecideProposalTest(ProposalSyncTest):
    """thoughtsctl decide-proposal hands the request to the app unchanged (protocol/agent-proposals.md 10.7.3)."""

    test_burst_shares_one_sync_before_and_one_after = None
    test_failed_pdf_build_does_not_block_proposals = None
    test_failed_sync_before_writes_nothing = None

    def decide(self, request):
        done = subprocess.run([os.environ['THOUGHTS_TEST_CLI'], '--json', 'decide-proposal'], input=json.dumps(request),
                              capture_output=True, text=True, env=self.env, timeout=60)
        return done.returncode, json.loads(done.stdout)

    def test_request_reaches_the_app(self):
        app = self.library()
        request = dict(request_id=str(uuid.uuid4()), source=str(self.source), expected_source_sha256='a' * 64, proposal_id='agent-1',
                       wrapper_sha256='c' * 64, action='accept', origin='vscode')
        code, result = self.decide(request)
        self.assertEqual(code, 0, result)
        self.assertEqual(result['state'], 'accepted')
        self.assertEqual(result['claim'], 'recorded')
        self.assertEqual(app.decisions, [{**request, 'command': 'decide-proposal', 'schema_version': 1}])
        code, result = self.decide({**request, 'expected_source_sha256': 'd' * 64})
        self.assertNotEqual(code, 0)
        self.assertEqual(result['error_code'], 'source_changed')
        # A request naming its own command is refused before reaching the app.
        code, result = self.decide({**request, 'command': 'sync'})
        self.assertEqual(code, 2)
        self.assertEqual(result['error_code'], 'invalid_request')
        self.assertEqual(len(app.decisions), 2)
        # Nothing about deciding is written locally by thoughtsctl itself.
        self.assertEqual(self.source.read_text(), 'One.\nTwo.\n')


@unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
class TodoCommandTest(ProposalSyncTest):
    """thoughtsctl hands to-do requests to the app unchanged; the app checks them (shell/todocommands.h)."""

    test_burst_shares_one_sync_before_and_one_after = None
    test_failed_pdf_build_does_not_block_proposals = None
    test_failed_sync_before_writes_nothing = None

    def test_requests_reach_the_app(self):
        app = self.library()
        requests = {'list-todos': dict(include_hidden=True), 'add-todo': dict(text='Call the bank', request_id=str(uuid.uuid4())),
                    'edit-todo': dict(todo_id='thoughts-1', expected_revision=2, text='Call the bank today')}
        for command, request in requests.items():
            done = subprocess.run([os.environ['THOUGHTS_TEST_CLI'], '--json', command], input=json.dumps(request),
                                  capture_output=True, text=True, env=self.env, timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(json.loads(done.stdout)['state'], 'done')
        self.assertEqual(app.todos, [{**request, 'command': command, 'schema_version': 1} for command, request in requests.items()])

    def test_open_agent_notes_reaches_the_app(self):
        """thoughtsctl open-agent-notes hands {document | project, note_id} to the app (Shell::openAgentNotesCommand)."""
        app = self.library()
        request = dict(project='MethodsComparison', note_id='0a595e0c-4b9f-4b21-9864-d0c2fa176b7c')
        done = subprocess.run([os.environ['THOUGHTS_TEST_CLI'], '--json', 'open-agent-notes'], input=json.dumps(request),
                              capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(json.loads(done.stdout)['state'], 'opened')
        self.assertEqual(app.todos, [{**request, 'command': 'open-agent-notes', 'schema_version': 1}])


GRPO_CARD = '00000000-0000-4000-8000-0000000000b1'
NEW_CARD = '00000000-0000-4000-8000-0000000000b2'


def feedback_record(feedback_id, polarity, quote, comment, created_at, pdf_sha256='a' * 64, deleted=False):
    record = dict(schema_version=1, feedback_id=feedback_id, card_id=GRPO_CARD, polarity=polarity, comment=comment, revision=2 if deleted else 1,
                  anchor=dict(version=1, quote=dict(exact=quote, prefix='', suffix=''),
                              pdf=dict(document_id='c1e62ecc-2dfb-4259-97cc-e319b1186848', pdf_sha256=pdf_sha256,
                                       regions=[dict(page=1, rect=[0.1, 0.2, 0.8, 0.3])]), segment_id='0', segment_content_sha256='c' * 64),
                  created_at=created_at, updated_at=created_at, created_origin_id='android-pixel', origin_id='android-pixel', deleted=deleted)
    if deleted:
        record['deleted_at'] = created_at
    return record


# Answer feedback of GRPO_CARD: liked on the current PDF, disliked on an earlier build, and one deleted.
FEEDBACK = [
    feedback_record('5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13', 'negative', 'minus the group mean', 'Divide by the std too.', '2026-10-20T10:14:03.512Z', pdf_sha256='d' * 64),
    feedback_record('0c4d1f7a-9b2e-4c5d-8e6f-1a2b3c4d5e6f', 'positive', 'The advantage is the reward', '', '2026-10-20T09:00:00.000Z'),
    feedback_record('7e8f9a0b-1c2d-4e3f-9a5b-6c7d8e9f0a1b', 'positive', 'deleted passage', '', '2026-10-20T11:00:00.000Z', deleted=True),
]


def write_review_index(path):
    """A review index as Thoughts writes it (shell/reviewstore.cpp refreshQueryDatabase)."""
    import sqlite3
    path.parent.mkdir(parents=True, exist_ok=True)
    database = sqlite3.connect(path)
    database.execute('PRAGMA journal_mode=WAL')
    database.executescript('''
        CREATE TABLE review_cards (card_id TEXT PRIMARY KEY, document_id TEXT, pdf_stem TEXT, question TEXT, status TEXT, state TEXT, current_mark TEXT, due_at TEXT, last_reviewed_at TEXT, review_count INTEGER NOT NULL, lapse_count INTEGER NOT NULL, interval_days INTEGER NOT NULL, is_deleted INTEGER NOT NULL, is_retained INTEGER NOT NULL, card_json TEXT NOT NULL);
        CREATE TABLE review_events (event_id TEXT PRIMARY KEY, card_id TEXT NOT NULL, reviewed_at TEXT, origin_id TEXT, rating TEXT, review_action TEXT, queue_action TEXT, scheduled_due_at TEXT, retracted_event_id TEXT, is_retracted INTEGER NOT NULL, event_json TEXT NOT NULL);
        CREATE TABLE review_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE review_tag_records (entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, record_json TEXT NOT NULL, PRIMARY KEY (entity_type, entity_id));
        CREATE TABLE review_feedback (feedback_id TEXT PRIMARY KEY, card_id TEXT NOT NULL, polarity TEXT NOT NULL, quote TEXT NOT NULL, comment TEXT NOT NULL, created_at TEXT, updated_at TEXT, deleted INTEGER NOT NULL, record_json TEXT NOT NULL);
    ''')

    def card(card_id, question, source=None):
        primary = dict(pdf_stem='rl_grpo', pdf_sha256='a' * 64, page=1, rect=[0.1, 0.2, 0.8, 0.6], page_visual_sha256='b' * 64, question=question,
                       pdf_document_id='c1e62ecc-2dfb-4259-97cc-e319b1186848')
        if source:
            primary['source_anchor'] = dict(version=2, segments=[dict(segment_id='0', source_file=source, line_start=3, line_end=3)])
        return dict(schema_version=3, card_type='visual_region', card_id=card_id, status='approved', revision=1, primary=primary,
                    initial_due_at='2099-01-01T00:00:00.000Z')

    for record in (card(GRPO_CARD, 'How does GRPO compute the advantage?', 'rl/grpo.tex'), card(NEW_CARD, 'Why is a baseline needed?')):
        database.execute('INSERT INTO review_cards VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                         (record['card_id'], '', 'rl_grpo', record['primary']['question'], 'approved', 'new', '', None, None, 0, 0, 0, 0, 0, json.dumps(record)))
    event = dict(event_id='e1', card_id=GRPO_CARD, origin_id='test', reviewed_at='2026-10-01T12:00:00.000Z', rating='custom_interval',
                 scheduler_version=2, schema_version=1, scheduled_due_at='2099-01-01T12:00:00.000Z', review_tags=['Memorized'])
    database.execute('INSERT INTO review_events VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                     ('e1', GRPO_CARD, event['reviewed_at'], 'test', 'custom_interval', '', '', event['scheduled_due_at'], '', 0, json.dumps(event)))
    groups = dict(layout_id='groups', schema_version=1, updated_at='2026-10-01T00:00:00.000Z', origin_id='test',
                  groups=[dict(group_id='g1', name='Understanding', tags=['correct', 'Memorized'])])
    database.execute('INSERT INTO review_tag_records VALUES (?,?,?)', ('review_tag_groups', 'groups', json.dumps(groups)))
    for feedback in FEEDBACK:
        database.execute('INSERT INTO review_feedback VALUES (?,?,?,?,?,?,?,?,?)',
                         (feedback['feedback_id'], feedback['card_id'], feedback['polarity'], feedback['anchor']['quote']['exact'], feedback['comment'],
                          feedback['created_at'], feedback['updated_at'], int(feedback['deleted']), json.dumps(feedback)))
    database.execute("INSERT INTO review_meta VALUES ('indexed_at_utc', '2026-10-12T08:00:00.000Z')")
    database.commit()
    database.close()


def snapshot(root):
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(root).rglob('*')) if p.is_file()}


@unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CLI'), 'Set THOUGHTS_TEST_CLI to the built or installed thoughtsctl')
class KnowledgeToolsTest(unittest.IsolatedAsyncioTestCase):
    """search_review_cards and knowledge_profile through the real thoughtsctl, in a private HOME, without the app."""

    def setUp(self):
        self.home = Path(tempfile.mkdtemp(dir='/tmp', prefix='thoughts-kp-'))
        self.addCleanup(shutil.rmtree, self.home, True)
        self.data = Path(private_home(self.home)['HOME']) / 'Library/Application Support/Thoughts'
        write_review_index(self.data / 'thoughts-server/review-query.sqlite')
        docs = self.home / 'docs'
        (docs / 'rl').mkdir(parents=True)
        (docs / 'rl/grpo.tex').write_text('\\section{GRPO}\n\\ReviewCardMaskStart{%s}{0}%%\nThe advantage is the reward minus the group mean.\n\\ReviewCardMaskEnd{%s}{0}%%\n'
                                          % (GRPO_CARD, GRPO_CARD))
        self.env = {**os.environ, **private_home(self.home), 'THOUGHTS_HOME': str(docs), 'THOUGHTSCTL': os.environ['THOUGHTS_TEST_CLI']}
        self.before = snapshot(self.home)

    def cli(self, *args, request=None):
        done = subprocess.run([os.environ['THOUGHTS_TEST_CLI'], '--json', *args], input=json.dumps(request) if request is not None else '',
                              capture_output=True, text=True, env=self.env, timeout=60)
        return done.returncode, json.loads(done.stdout)

    async def test_tools_read_the_index_and_write_nothing(self):
        params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))], env=self.env)
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                found = await session.call_tool('search_review_cards', {'query': 'GRPO advantages'})
                self.assertFalse(found.isError, found.content)
                cards = found.structuredContent['cards']
                self.assertEqual([c['card_id'] for c in cards], [GRPO_CARD, NEW_CARD])
                self.assertEqual(cards[0]['answer'], {'text': 'The advantage is the reward minus the group mean.', 'from': 'source', 'truncated': False})
                self.assertEqual(cards[0]['source']['lines'], [[3, 3]])
                self.assertEqual(cards[0]['current_tags'], ['Memorized'])
                self.assertEqual([(h['reviewed_at'], h['tags']) for h in cards[0]['history']], [('2026-10-01T12:00:00.000Z', ['Memorized'])])
                self.assertEqual(cards[1]['history'], [])
                # Answer feedback (review-card-feedback.md §7): not deleted, in created_at order.
                self.assertEqual(cards[0]['feedback'], [
                    dict(feedback_id='0c4d1f7a-9b2e-4c5d-8e6f-1a2b3c4d5e6f', polarity='positive', quote='The advantage is the reward', comment='',
                         created_at='2026-10-20T09:00:00.000Z', updated_at='2026-10-20T09:00:00.000Z', origin='android-pixel', anchor_state='exact',
                         page=1, segment_id='0', source_changed=None, card_id=GRPO_CARD, shared=False),
                    dict(feedback_id='5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13', polarity='negative', quote='minus the group mean', comment='Divide by the std too.',
                         created_at='2026-10-20T10:14:03.512Z', updated_at='2026-10-20T10:14:03.512Z', origin='android-pixel', anchor_state='pdf_changed',
                         page=1, segment_id='0', source_changed=None, card_id=GRPO_CARD, shared=False)])
                # Shared by the cards of the document: GRPO_CARD's passage on this PDF lies inside
                # NEW_CARD's answer regions; the one made on an earlier build is not in its (unknown) answer text.
                self.assertEqual(cards[1]['feedback'], [
                    dict(feedback_id='0c4d1f7a-9b2e-4c5d-8e6f-1a2b3c4d5e6f', polarity='positive', quote='The advantage is the reward', comment='',
                         created_at='2026-10-20T09:00:00.000Z', updated_at='2026-10-20T09:00:00.000Z', origin='android-pixel', anchor_state='exact',
                         page=1, segment_id='0', source_changed=None, card_id=GRPO_CARD, shared=True)])
                with_deleted = await session.call_tool('search_review_cards', {'query': 'GRPO advantages', 'include_deleted': True})
                deleted = [f for f in with_deleted.structuredContent['cards'][0]['feedback'] if f.get('deleted')]
                self.assertEqual([(f['feedback_id'], f['deleted_at']) for f in deleted], [('7e8f9a0b-1c2d-4e3f-9a5b-6c7d8e9f0a1b', '2026-10-20T11:00:00.000Z')])
                self.assertEqual(found.structuredContent['tag_groups'], [{'name': 'Understanding', 'tags': ['Correct', 'Memorized']}])
                profile = await session.call_tool('knowledge_profile', {'topic': 'GRPO'})
                self.assertFalse(profile.isError, profile.content)
                self.assertEqual(profile.structuredContent['counts'], {'solid': 1, 'shaky': 0, 'unknown': 0, 'unreviewed': 1})
                self.assertTrue(profile.structuredContent['summary'].startswith('"GRPO": 2 matching review cards'))
                self.assertEqual(profile.structuredContent['mapping']['source'], 'default mapping')
                unknown = await session.call_tool('knowledge_profile', {'topic': 'GRPO', 'project': 'Nope'})
                self.assertTrue(unknown.isError)
                self.assertEqual(unknown.structuredContent['error_code'], 'unknown_project')
        self.assertEqual(snapshot(self.home), self.before)
        self.assertFalse((self.data / 'knowledge-tag-levels.json').exists())

    def test_retired_cards(self):
        """A retired card (review-card-retire.md section 7): status retired with retired_at, never due, its history kept."""
        import sqlite3
        database = sqlite3.connect(self.data / 'thoughts-server/review-query.sqlite')
        retire = dict(schema_version=3, event_id='6f1c2b3a-4d5e-4f60-8a7b-9c0d1e2f3a4b', card_id=NEW_CARD, queue_action='retire',
                      reviewed_at='2026-10-27T14:02:11.204Z', origin_id='android-pixel')
        database.execute('INSERT INTO review_events VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                         (retire['event_id'], NEW_CARD, retire['reviewed_at'], 'android-pixel', '', '', 'retire', '', '', 0, json.dumps(retire)))
        database.commit()
        database.close()
        code, found = self.cli('search-review-cards', request={'query': 'baseline'})
        self.assertEqual(code, 0, found)
        card = next(c for c in found['cards'] if c['card_id'] == NEW_CARD)
        self.assertEqual(card['status'], 'retired')
        self.assertEqual(card['retired_at'], '2026-10-27T14:02:11.204Z')
        self.assertIsNone(card['due_at'])
        code, retired = self.cli('search-review-cards', request={'state': 'retired'})
        self.assertEqual([c['card_id'] for c in retired['cards']], [NEW_CARD])
        code, active = self.cli('search-review-cards', request={'state': 'active'})
        self.assertEqual([c['card_id'] for c in active['cards']], [GRPO_CARD])
        self.assertNotIn('retired_at', active['cards'][0])
        code, new = self.cli('search-review-cards', request={'state': 'new'})
        self.assertEqual(new['cards'], [])
        code, profile = self.cli('knowledge-profile', request={'topic': 'baseline'})
        self.assertEqual(code, 0, profile)
        self.assertEqual(profile['counts']['unreviewed'], 1)
        self.assertEqual(profile['cards']['unreviewed'][0]['reason'], 'never answered; retired')

    def test_levels_file_only_on_init(self):
        code, levels = self.cli('knowledge-levels')
        self.assertEqual(code, 0, levels)
        self.assertEqual(levels['source'], 'default mapping')
        self.assertEqual(levels['levels']['Correct'], 'solid')
        self.assertEqual(snapshot(self.home), self.before)
        code, created = self.cli('knowledge-levels', '--init')
        self.assertEqual(code, 0, created)
        path = self.data / 'knowledge-tag-levels.json'
        self.assertEqual(created['source'], str(path))
        self.assertEqual(json.loads(path.read_text())['levels']['Wrong'], 'unknown')
        code, again = self.cli('knowledge-levels', '--init')
        self.assertNotEqual(code, 0)
        self.assertEqual(again['error_code'], 'levels_exist')
        path.write_text(json.dumps({'schema_version': 1, 'levels': {'Memorized': 'shaky'}}))
        code, profile = self.cli('knowledge-profile', request={'topic': 'GRPO'})
        self.assertEqual(code, 0, profile)
        self.assertEqual(profile['counts']['shaky'], 1)
        self.assertEqual(profile['mapping']['source'], str(path))
        code, refused = self.cli('search-review-cards', request={'state': 'forgotten'})
        self.assertEqual(code, 2)
        self.assertEqual(refused['error_code'], 'invalid_request')
        (self.data / 'thoughts-server/review-query.sqlite').unlink()
        code, missing = self.cli('search-review-cards', request={})
        self.assertNotEqual(code, 0)
        self.assertEqual(missing['error_code'], 'index_unavailable')
        self.assertFalse((self.data / 'thoughts-server/review-query.sqlite').exists())


class ConversationLinkTest(unittest.TestCase):
    """The link written next to conversation_id (agent-proposals spec section 3.6)."""

    def setUp(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import server
        self.server = server
        self.directory = tempfile.TemporaryDirectory()
        self.home = Path(self.directory.name)
        self.saved = {key: os.environ.get(key) for key in ('HOME', 'CLAUDE_CONFIG_DIR', 'CODEX_HOME')}
        os.environ['HOME'] = str(self.home)
        os.environ.pop('CLAUDE_CONFIG_DIR', None)
        os.environ.pop('CODEX_HOME', None)

    def tearDown(self):
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self.directory.cleanup()

    def test_remote_control_claude_session(self):
        conversation = '5484d188-ea61-5e83-916b-87eb66ead7d1'
        sessions = self.home / '.claude' / 'sessions'
        sessions.mkdir(parents=True)
        (sessions / '1.json').write_text(json.dumps({'sessionId': '00000000-0000-4000-8000-000000000000', 'bridgeSessionId': 'session_01Other'}))
        (sessions / '2.json').write_text('not json')
        self.assertIsNone(self.server.conversation_url(conversation))
        (sessions / '3.json').write_text(json.dumps({'sessionId': conversation, 'bridgeSessionId': 'session_01ExampleSessionAbcdEfgh'}))
        self.assertEqual(self.server.conversation_url(conversation), 'https://claude.ai/code/session_01ExampleSessionAbcdEfgh')

    def test_claude_session_without_remote_control_or_with_a_bad_id(self):
        conversation = '5484d188-ea61-5e83-916b-87eb66ead7d1'
        sessions = self.home / '.claude' / 'sessions'
        sessions.mkdir(parents=True)
        (sessions / '1.json').write_text(json.dumps({'sessionId': conversation}))
        (sessions / '2.json').write_text(json.dumps({'sessionId': conversation, 'bridgeSessionId': 'session_01/../x'}))
        self.assertIsNone(self.server.conversation_url(conversation))

    def test_codex_thread(self):
        conversation = '01900000-0000-7000-8000-000000000001'
        day = self.home / '.codex' / 'sessions' / '2026' / '10' / '18'
        day.mkdir(parents=True)
        self.assertIsNone(self.server.conversation_url(conversation))
        (day / f'rollout-2026-10-18T14-22-27-{conversation}.jsonl').write_text('{}\n')
        self.assertEqual(self.server.conversation_url(conversation), 'codex://threads/' + conversation)
        archived = '0190e000-0000-7000-8000-00000000000e'
        (self.home / '.codex' / 'archived_sessions').mkdir()
        (self.home / '.codex' / 'archived_sessions' / f'rollout-2026-05-15T11-50-32-{archived}.jsonl').write_text('{}\n')
        self.assertEqual(self.server.conversation_url(archived), 'codex://threads/' + archived)


class AgentConversationTest(unittest.IsolatedAsyncioTestCase):
    """Agent conversations: detection, automatic links and link_conversation (spec section 5)."""

    CLAUDE = '5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139'
    CODEX = '01900000-0000-7000-8000-000000000002'

    def setUp(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import server
        self.server = server
        self.directory = tempfile.TemporaryDirectory()
        self.home = Path(self.directory.name)
        self.saved = {key: os.environ.get(key) for key in ('HOME', 'CLAUDE_CONFIG_DIR', 'CODEX_HOME')}
        os.environ['HOME'] = str(self.home)
        os.environ.pop('CLAUDE_CONFIG_DIR', None)
        os.environ.pop('CODEX_HOME', None)
        self.patches = []
        self.patch(server.os, 'getppid', lambda: 4242)
        self.patch(server, 'parent_pid', lambda pid: None)
        self.patch(server, 'codex_thread_by_process', lambda: None)
        server._claude_session.clear()
        server._auto_links.clear()
        self.calls = []

    def patch(self, owner, name, value):
        self.patches.append((owner, name, getattr(owner, name)))
        setattr(owner, name, value)

    def tearDown(self):
        for owner, name, value in reversed(self.patches):
            setattr(owner, name, value)
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self.server._claude_session.clear()
        self.server._auto_links.clear()
        self.directory.cleanup()

    def claude_session(self, **record):
        sessions = self.home / '.claude' / 'sessions'
        sessions.mkdir(parents=True, exist_ok=True)
        (sessions / '4242.json').write_text(json.dumps({'pid': 4242, 'sessionId': self.CLAUDE, 'cwd': '/Users/alex', **record}))

    def transcript(self, *entries):
        folder = self.home / '.claude' / 'projects' / '-Users-alex'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f'{self.CLAUDE}.jsonl').write_text(''.join(json.dumps(entry) + '\n' for entry in entries))

    def fake_command(self, result=None, error=None):
        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload))
            if error:
                raise error
            return result or {'schema_version': 1, 'accepted': True, 'written': True}
        self.patch(self.server, 'command', command)

    async def settle(self):
        while self.server._link_tasks:
            await asyncio.gather(*list(self.server._link_tasks))

    def test_claude_session_of_the_parent_process(self):
        self.assertIsNone(self.server.current_conversation())
        self.claude_session(bridgeSessionId='session_01AbCdEfGhJkLmNpQrStUvWx', name='Thoughts search on mobile', nameSource='user')
        conversation = self.server.current_conversation()
        self.assertEqual(conversation, {'agent': 'claude', 'conversation_id': self.CLAUDE,
                                        'conversation_url': 'https://claude.ai/code/session_01AbCdEfGhJkLmNpQrStUvWx',
                                        'session_name': 'Thoughts search on mobile', 'title': 'Thoughts search on mobile', 'title_source': 'app'})
        # A derived name is only the session name; the first real prompt is the title.
        self.claude_session(name='alex-94', nameSource='derived')
        self.transcript({'type': 'summary'}, {'type': 'user', 'message': {'content': '<command-name>/clear</command-name>'}},
                        {'type': 'user', 'message': {'content': [{'type': 'text', 'text': 'Caveat: local'}]}},
                        {'type': 'user', 'message': {'content': [{'type': 'tool_result'}, {'type': 'text', 'text': '\n  Make   the search\nwork on the phone'}]}})
        conversation = self.server.current_conversation()
        self.assertEqual((conversation['title'], conversation['title_source'], conversation['session_name'], conversation['conversation_url']),
                         ('Make the search', 'prompt', 'alex-94', ''))
        # Without a prompt the session name, else the short id.
        self.transcript({'type': 'assistant'})
        self.assertEqual(self.server.current_conversation()['title'], 'alex-94')
        self.claude_session()
        self.assertEqual((self.server.current_conversation()['title'], self.server.current_conversation()['title_source']),
                         ('Claude conversation 5ef4cd0f', 'fallback'))

    def test_long_prompt_is_cut_at_a_word(self):
        self.claude_session(name='alex-1', nameSource='derived')
        self.transcript({'type': 'user', 'message': {'content': 'word ' * 60}})
        title = self.server.current_conversation()['title']
        self.assertLessEqual(len(title), 120)
        self.assertTrue(title.endswith('\u2026') and title.startswith('word word'))

    def test_explicit_ids(self):
        self.assertIsNone(self.server.current_conversation(self.CODEX))
        self.assertIsNone(self.server.current_conversation('not-a-uuid'))
        day = self.home / '.codex' / 'sessions' / '2026' / '10' / '26'
        day.mkdir(parents=True)
        (day / f'rollout-2026-10-26T10-00-00-{self.CODEX}.jsonl').write_text('{}\n')
        (self.home / '.codex' / 'session_index.jsonl').write_text(
            json.dumps({'id': self.CODEX, 'thread_name': 'Old'}) + '\n' + json.dumps({'id': 'other', 'thread_name': 'x'}) + '\n'
            + json.dumps({'id': self.CODEX, 'thread_name': 'GRPO notes'}) + '\n')
        self.assertEqual(self.server.current_conversation(self.CODEX), {
            'agent': 'codex', 'conversation_id': self.CODEX, 'conversation_url': 'codex://threads/' + self.CODEX,
            'session_name': '', 'title': 'GRPO notes', 'title_source': 'app'})
        # A Claude id is recognised by its transcript, even without a running session.
        self.transcript({'type': 'user', 'message': {'content': 'Explain GRPO'}})
        conversation = self.server.current_conversation(self.CLAUDE)
        self.assertEqual((conversation['agent'], conversation['title'], conversation['title_source']), ('claude', 'Explain GRPO', 'prompt'))

    async def test_auto_link_after_success_only_and_throttled(self):
        self.claude_session(bridgeSessionId='session_01AbCdEfGhJkLmNpQrStUvWx', name='alex-94', nameSource='derived')
        self.fake_command()
        ok = self.server.tool_result({'schema_version': 1, 'accepted': True})
        failed = self.server.tool_result({'schema_version': 1, 'accepted': False, 'error_code': 'stale_source'})
        self.assertIs(self.server.linked(failed, 'create_proposal', {'source': 'docs-a/a.tex'}), failed)
        await self.settle()
        self.assertEqual(self.calls, [])
        self.assertIs(self.server.linked(ok, 'create_proposal', {'source': 'docs-a/a.tex'}, self.CLAUDE), ok)
        await self.settle()
        self.assertEqual(len(self.calls), 1)
        args, payload = self.calls[0]
        self.assertEqual(args, ('link-conversation',))
        self.assertEqual(payload, {'source': 'docs-a/a.tex', 'agent': 'claude', 'conversation_id': self.CLAUDE,
                                   'conversation_url': 'https://claude.ai/code/session_01AbCdEfGhJkLmNpQrStUvWx', 'session_name': 'alex-94',
                                   'title': 'alex-94', 'title_source': 'fallback', 'linked_by': 'tool', 'tool': 'create_proposal'})
        # The same target within 10 minutes is not sent again; another target is.
        self.server.linked(ok, 'revise_proposal', {'source': 'docs-a/a.tex'}, self.CLAUDE)
        self.server.linked(ok, 'open_document', {'document': 'a.pdf'})
        await self.settle()
        self.assertEqual([call[1].get('document') for call in self.calls[1:]], ['a.pdf'])
        # Without a detectable conversation nothing is linked.
        (self.home / '.claude' / 'sessions' / '4242.json').unlink()
        self.server._claude_session.clear()
        self.server.linked(ok, 'open_document', {'document': 'b.pdf'})
        await self.settle()
        self.assertEqual(len(self.calls), 2)

    async def test_failing_thoughtsctl_does_not_fail_the_tool(self):
        self.claude_session()
        self.fake_command(error=RuntimeError('Thoughts command failed: not running'))
        ok = self.server.tool_result({'schema_version': 1, 'accepted': True, 'state': 'opened'})
        self.assertIs(self.server.linked(ok, 'open_document', {'document': 'a.pdf'}), ok)
        await self.settle()
        self.assertEqual(len(self.calls), 1)
        self.assertFalse(ok.isError)
        # A refused link is only logged.
        self.fake_command(result={'schema_version': 1, 'accepted': False, 'error_code': 'not_in_library'})
        self.server.linked(ok, 'open_document', {'document': '/tmp/elsewhere.pdf'})
        await self.settle()
        self.assertEqual(len(self.calls), 2)

    async def test_send_note_records_its_conversation_without_linking(self):
        self.claude_session(bridgeSessionId='session_01ExampleSessionAbcdEfgh', name='Study GRPO', nameSource='user')
        self.fake_command()
        sent = []

        async def note_call(function, *args, **kwargs):
            sent.append(kwargs)
            return {'note_id': 'n1'}
        self.patch(self.server, 'agent_note_call', note_call)
        result = await self.server.send_note(title='Sketch', markdown='$x$', review_card_id='c1e62ecc-2dfb-4259-97cc-e319b1186848')
        self.assertFalse(result.isError)
        await self.settle()
        # A study note never lists its conversation under the card or its Project.
        self.assertEqual(self.calls, [])
        conversation = sent[0]['conversation']
        self.assertEqual((conversation['agent'], conversation['conversation_id'], conversation['conversation_url'], conversation['title']),
                         ('claude', self.CLAUDE, 'https://claude.ai/code/session_01ExampleSessionAbcdEfgh', 'Study GRPO'))
        # Without a detectable conversation the note is sent all the same.
        (self.home / '.claude' / 'sessions' / '4242.json').unlink()
        self.server._claude_session.clear()
        await self.server.send_note(title='Sketch', markdown='$x$')
        self.assertIsNone(sent[1]['conversation'])

    async def test_link_conversation(self):
        self.fake_command(result={'schema_version': 1, 'accepted': True, 'written': True, 'project_name': 'Concepts'})
        for arguments in ({}, {'project': 'A', 'source': 'docs-a/a.tex'}, {'project': ' '}):
            with self.assertRaises(ValueError):
                await self.server.link_conversation(**arguments)
        unknown = await self.server.link_conversation(project='Concepts')
        self.assertTrue(unknown.isError)
        self.assertEqual(unknown.structuredContent['error_code'], 'unknown_conversation')
        self.assertIn('Pass conversation_id', unknown.structuredContent['message'])
        self.assertEqual(self.calls, [])
        self.claude_session(name='alex-94', nameSource='derived')
        result = await self.server.link_conversation(project='Concepts', title='  Explains\nGRPO  ')
        self.assertFalse(result.isError)
        self.assertEqual(self.calls[-1][1], {'project': 'Concepts', 'agent': 'claude', 'conversation_id': self.CLAUDE, 'conversation_url': '',
                                             'session_name': 'alex-94', 'title': 'Explains GRPO', 'title_source': 'agent',
                                             'linked_by': 'agent', 'tool': 'link_conversation'})
        # Without a title the automatic one is sent, still as an explicit link.
        await self.server.link_conversation(document='grpo.pdf')
        self.assertEqual((self.calls[-1][1]['title_source'], self.calls[-1][1]['linked_by']), ('fallback', 'agent'))
        # A review card is the fourth target: it links the card too, as the agent's own link.
        await self.server.link_conversation(review_card='0FC5768D-A56C-53D0-9F37-7AF64C3CF8A9')
        payload = self.calls[-1][1]
        self.assertEqual((payload['review_card_id'], payload['card_linked_by'], payload['linked_by']),
                         ('0fc5768d-a56c-53d0-9f37-7af64c3cf8a9', 'agent', 'agent'))
        self.assertFalse({'project', 'source', 'document'} & payload.keys())
        with self.assertRaises(ValueError):
            await self.server.link_conversation(project='Concepts', review_card='0fc5768d-a56c-53d0-9f37-7af64c3cf8a9')

    async def test_card_tools_link_the_card(self):
        """card-agent-conversations spec section 3.2: automatic card links, the Project alone when the app lacks the card."""
        card = '0fc5768d-a56c-53d0-9f37-7af64c3cf8a9'
        self.claude_session(name='alex-61', nameSource='derived')
        replies = []

        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload))
            if args[0] == 'create-review-card':
                return {'schema_version': 1, 'accepted': True, 'state': 'card_created', 'card_id': card}
            if args[0] == 'move-review-card':
                return {'schema_version': 1, 'accepted': True, 'state': 'card_moved'}
            return replies.pop(0) if replies else {'schema_version': 1, 'accepted': True, 'written': True}
        self.patch(self.server, 'command', command)
        request = dict(source='docs-a/a.tex', expected_source_sha256='a' * 64, start_line=2, end_line=4, question='Q?',
                       due_at='2099-01-01T12:00:00Z', request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139')
        await self.server.create_review_card(**request)
        await self.settle()
        links = [payload for args, payload in self.calls if args == ('link-conversation',)]
        self.assertEqual(len(links), 1)
        self.assertEqual((links[0]['review_card_id'], links[0]['card_linked_by'], links[0]['linked_by'], links[0]['tool']),
                         (card, 'tool', 'tool', 'create_review_card'))
        self.assertNotIn('source', links[0])
        # The app does not know the card: the Project of the source is linked instead.
        self.calls.clear()
        self.server._auto_links.clear()
        replies.append({'schema_version': 1, 'accepted': False, 'error_code': 'card_not_found'})
        await self.server.move_review_card(card_id=card, source='docs-a/a.tex', expected_source_sha256='a' * 64, start_line=2, end_line=4,
                                           request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139')
        await self.settle()
        links = [payload for args, payload in self.calls if args == ('link-conversation',)]
        self.assertEqual([('review_card_id' in link, link.get('source'), link['tool']) for link in links],
                         [(True, None, 'move_review_card'), (False, 'docs-a/a.tex', 'move_review_card')])
        self.assertNotIn('card_linked_by', links[1])

    async def test_linked_card_move_proposes_both_halves(self):
        """agent-proposals spec section 12: a linked card moves through move-card-passage, which Syncs like a proposal."""
        card = '0fc5768d-a56c-53d0-9f37-7af64c3cf8a9'
        conversation = '0190a000-0000-7000-8000-00000000000a'

        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload, timeout))
            if args[0] == 'move-card-passage':
                return {'schema_version': 1, 'accepted': True, 'state': 'move_proposed', 'card_id': card}
            return {'schema_version': 1, 'accepted': True, 'written': True}
        self.patch(self.server, 'command', command)
        self.patch(self.server, 'conversation_url', lambda value: None)
        move = dict(card_id=card, source='docs-b/b.tex', expected_source_sha256='b' * 64, start_line=7, end_line=7,
                    request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139')
        # The insertion line alone is not enough: both proposals record why and from which conversation.
        refused = await self.server.move_review_card(**move)
        self.assertTrue(refused.isError)
        self.assertEqual(refused.structuredContent['error_code'], 'invalid_request')
        self.assertEqual(self.calls, [])
        moved = await self.server.move_review_card(**move, explanation='Merge the notes.', conversation_id=conversation)
        self.assertFalse(moved.isError, moved.content)
        args, payload, timeout = self.calls[0]
        self.assertEqual(args, ('move-card-passage', '--timeout', str(self.server.PROPOSAL_SYNC_SECONDS)))
        self.assertEqual(payload, dict(card_id=card, source='docs-b/b.tex', expected_source_sha256='b' * 64, line=7,
                                       explanation='Merge the notes.', conversation_id=conversation,
                                       request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139'))
        self.assertGreater(timeout, self.server.PROPOSAL_SYNC_SECONDS)
        # A PDF-only card still moves at once through the app.
        self.calls.clear()
        await self.server.move_review_card(**{**move, 'end_line': 9})
        self.assertEqual(self.calls[0][0], ('move-review-card',))

    async def test_merge_review_cards_is_one_change_through_the_app(self):
        """merge_review_cards: one thoughtsctl merge-review-cards request, never proposals; the conversation follows the kept card."""
        kept = '0fc5768d-a56c-53d0-9f37-7af64c3cf8a9'
        other = '6f1c2a0e-3b4d-4c5e-8f60-718293a4b5c6'
        self.claude_session(name='alex-62', nameSource='derived')
        replies = []

        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload, timeout))
            if args[0] == 'merge-review-cards':
                if replies:
                    raise replies.pop(0)
                return {'schema_version': 1, 'accepted': True, 'state': 'merged', 'kept_card_id': kept, 'deleted_card_ids': [other]}
            return {'schema_version': 1, 'accepted': True, 'written': True}
        self.patch(self.server, 'command', command)
        merge = dict(card_ids=[other, kept], source='docs-b/b.tex', expected_source_sha256='b' * 64,
                     request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139')
        merged = await self.server.merge_review_cards(**merge, keep_card_id=kept, question='Both?')
        self.assertFalse(merged.isError, merged.content)
        args, payload, timeout = self.calls[0]
        self.assertEqual(args, ('merge-review-cards',))
        self.assertEqual(payload, {**merge, 'keep_card_id': kept, 'question': 'Both?'})
        self.assertEqual(timeout, 25)
        await self.settle()
        links = [payload for args, payload, timeout in self.calls if args == ('link-conversation',)]
        self.assertEqual(len(links), 1)
        self.assertEqual((links[0]['review_card_id'], links[0]['tool']), (kept, 'merge_review_cards'))
        # Defaults stay with the app: the card reviewed most, the questions joined.
        self.calls.clear()
        await self.server.merge_review_cards(**merge)
        self.assertEqual(self.calls[0][1], merge)
        # A Thoughts build without the command says so instead of failing obscurely.
        replies.append(RuntimeError('Thoughts command failed: error: usage: thoughtsctl <open PDF [--page N]|sync|status OPERATION_ID>'))
        old = await self.server.merge_review_cards(**merge)
        self.assertTrue(old.isError)
        self.assertEqual(old.structuredContent['error_code'], 'app_update_required')
        replies.append(RuntimeError('Thoughts did not respond in time'))
        with self.assertRaises(RuntimeError):
            await self.server.merge_review_cards(**merge)

    async def test_project_tools_go_through_the_app(self):
        """create_project and import_pdf use the app's Add project and Import document, never the files."""
        project = 'new:5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139'
        self.claude_session(name='alex-62', nameSource='derived')

        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload))
            if args[0] == 'create-project':
                return {'schema_version': 1, 'accepted': True, 'state': 'created', 'project_id': project, 'project': 'Distillation'}
            if args[0] == 'import-document':
                self.assertTrue(Path(payload['path']).read_bytes().startswith(b'%PDF-'))
                return {'schema_version': 1, 'accepted': True, 'state': 'imported', 'project_id': project, 'filename': 'a.pdf'}
            return {'schema_version': 1, 'accepted': True, 'written': True}
        self.patch(self.server, 'command', command)
        await self.server.create_project(name='Distillation', request_id='5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139')
        pdf = self.home / 'paper.pdf'
        pdf.write_bytes(b'%PDF-1.7\n')
        await self.server.import_pdf(project='Distillation', name='Paper', path=str(pdf))
        self.patch(self.server, 'download_pdf', lambda url, directory: (Path(directory) / 'download.pdf').write_bytes(b'%PDF-1.7\n') and str(Path(directory) / 'download.pdf'))
        await self.server.import_pdf(project='Distillation', name='Paper', url='https://arxiv.org/pdf/2503.20083')
        await self.settle()
        requests = [(args[0], payload) for args, payload in self.calls if args[0] != 'link-conversation']
        self.assertEqual([name for name, _ in requests], ['create-project', 'import-document', 'import-document'])
        self.assertEqual(requests[0][1], {'name': 'Distillation', 'request_id': '5ef4cd0f-6a8a-4b7a-ac7b-1e9f51cd3139'})
        # The file that was checked is the file that is imported: links are resolved.
        self.assertEqual((requests[1][1]['path'], 'link' in requests[1][1]), (os.path.realpath(pdf), False))
        self.assertEqual(requests[2][1]['link'], 'https://arxiv.org/pdf/2503.20083')
        links = [payload for args, payload in self.calls if args == ('link-conversation',)]
        self.assertEqual({link['project'] for link in links}, {project})
        with self.assertRaises(ValueError):
            await self.server.import_pdf(project='Distillation', name='Paper')

    async def test_import_pdf_reads_no_private_file_and_no_private_host(self):
        """An agent chooses what import_pdf reads, and the PDF is uploaded: private files and hosts are refused."""
        async def command(*args, payload=None, timeout=25):
            self.calls.append((args, payload))
            return {'schema_version': 1, 'accepted': True, 'state': 'imported', 'project_id': 'p', 'filename': 'a.pdf'}
        self.patch(self.server, 'command', command)
        downloads = []
        self.patch(self.server, 'download_pdf', lambda url, directory: downloads.append(url))
        for url in ('http://arxiv.org/pdf/2503.20083', 'ftp://example.org/a.pdf', 'file:///etc/passwd', 'https:///a.pdf', 'https://user:pw@example.org/a.pdf',
                    'javascript:alert(1)', 'https://example.org@127.0.0.1/a.pdf', '//example.org/a.pdf'):
            with self.assertRaises(ValueError, msg=url):
                await self.server.import_pdf(project='P', name='Paper', url=url)
        self.assertEqual(downloads, [])
        # Files: hidden folders of the home directory, ~/Library and keychains, also through a link.
        home = Path(os.path.realpath(self.home))
        for folder in ('.ssh', '.aws', '.config/gh', 'Library/Application Support/App', 'Documents'):
            (home / folder).mkdir(parents=True, exist_ok=True)
            (home / folder / 'file.pdf').write_bytes(b'%PDF-1.7\n')
        (home / 'Documents/login.keychain-db').write_bytes(b'%PDF-1.7\n')
        os.symlink(home / '.ssh/file.pdf', home / 'Documents/link.pdf')
        self.patch(self.server.agent_notes, '_home_directories', lambda: [home])
        for refused in ('.ssh/file.pdf', '.aws/file.pdf', '.config/gh/file.pdf', 'Library/Application Support/App/file.pdf', 'Documents/link.pdf',
                        'Documents/login.keychain-db', 'Documents/../.ssh/file.pdf'):
            with self.assertRaises(ValueError, msg=refused) as caught:
                await self.server.import_pdf(project='P', name='Paper', path=str(home / refused))
            self.assertIn('never read for an agent', str(caught.exception))
        with self.assertRaises(ValueError):
            await self.server.import_pdf(project='P', name='Paper', path='/etc/passwd')
        self.assertEqual([args for args, _ in self.calls if args[0] == 'import-document'], [])
        await self.server.import_pdf(project='P', name='Paper', path=str(home / 'Documents/file.pdf'))
        self.assertEqual([payload['path'] for args, payload in self.calls if args[0] == 'import-document'], [str(home / 'Documents/file.pdf')])

    def test_download_only_from_public_https_hosts(self):
        server = self.server

        def resolver(addresses):
            return lambda host, port, type=None: [(None, None, None, '', (address, port)) for address in addresses]
        self.assertEqual(server.public_addresses('example.org', 443, resolver(['93.184.215.14', '2606:2800:21f:cb07:6820:80da:af6b:8b2c'])),
                         ['2606:2800:21f:cb07:6820:80da:af6b:8b2c', '93.184.215.14'])
        for address in ('127.0.0.1', '127.8.8.8', '::1', '10.1.2.3', '172.16.0.1', '192.168.1.1', '169.254.169.254', '100.64.0.1', '0.0.0.0', '::',
                        'fe80::1%en0', 'fd00::1', '::ffff:127.0.0.1', '::ffff:10.0.0.1', '224.0.0.1', '255.255.255.255', '192.0.2.1'):
            with self.assertRaises(ValueError, msg=address):
                server.public_addresses('host.example', 443, resolver([address]))
        # One private address among public ones is enough to refuse (a name that resolves both ways).
        with self.assertRaises(ValueError):
            server.public_addresses('host.example', 443, resolver(['93.184.215.14', '127.0.0.1']))
        with self.assertRaises(ValueError):
            server.public_addresses('host.example', 443, resolver([]))

        def unresolvable(host, port, type=None):
            raise OSError('no such host')
        with self.assertRaises(ValueError):
            server.public_addresses('host.example', 443, unresolvable)
        # The connection is made to the checked addresses only, and a private one is never dialled.
        dialled = []
        self.patch(server.socket, 'create_connection', lambda address, timeout=None: dialled.append(address) or (_ for _ in ()).throw(OSError('not dialled in tests')))
        self.patch(server.socket, 'getaddrinfo', resolver(['127.0.0.1']))
        for url in ('https://localhost/a.pdf', 'https://127.0.0.1:8443/a.pdf', 'https://internal.example/a.pdf'):
            with self.assertRaises(ValueError, msg=url) as caught:
                server.download_pdf(url, str(self.home))
            self.assertIn('public Internet host', str(caught.exception))
        self.assertEqual(dialled, [])
        self.patch(server.socket, 'getaddrinfo', resolver(['93.184.215.14']))
        with self.assertRaises(ValueError) as caught:
            server.download_pdf('https://example.org/a.pdf', str(self.home))
        self.assertEqual(dialled, [('93.184.215.14', 443)])
        self.assertIn('could not be downloaded', str(caught.exception))
        with self.assertRaises(ValueError):
            server.download_pdf('http://example.org/a.pdf', str(self.home))
        # A redirect to plain http is refused by the opener.
        with self.assertRaises(server.urllib.error.URLError):
            server.NoPlainHTTP().http_open(server.urllib.request.Request('http://example.org/a.pdf'))

    def test_first_prompt_of_a_card_conversation(self):
        self.claude_session(name='alex-61', nameSource='derived')
        block = ('Thoughts document: "GRPO"\nProject: Concepts\nReview card: 0fc5768d-a56c-53d0-9f37-7af64c3cf8a9\n'
                 'Question: "Why normalise?"\nAnswer pages: 6\n')
        self.transcript({'type': 'user', 'message': {'content': block + '\nExplain it with a picture'}})
        self.assertEqual(self.server.current_conversation()['title'], 'Explain it with a picture')
        self.transcript({'type': 'user', 'message': {'content': block}})
        self.assertEqual(self.server.current_conversation()['title'], 'Card: Why normalise?')


if __name__ == '__main__':
    unittest.main()
