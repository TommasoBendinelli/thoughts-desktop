# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Agent notes (protocol/agent-notes.md in thoughts-server): client, folder, rendering and MCP tools.

Runs against a fake Thoughts server on 127.0.0.1 and temporary folders only: never the real
server, Keychain, ~/Thoughts/agent-notes or Chrome profile, and never the network (the
Markdown page's files come from stand-ins). The real Chrome tests run only with
THOUGHTS_TEST_CHROME=1; of those only test_real_chrome downloads KaTeX and marked.
"""
import asyncio
import base64
import contextlib
import datetime as dt
import hashlib
import html
import http.client
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
import urllib.parse
import uuid
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parent))
import agent_notes  # noqa: E402

TOKEN = 'test-token-0123456789'


def png(width=8, height=6, color=(200, 30, 30)):
    raw = b''.join(b'\x00' + bytes(color) * width for _ in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def png_pixels(data):
    """(width, height, rows of (r, g, b) tuples) of an 8-bit RGB or RGBA PNG without interlacing."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    position, chunks, header = 8, [], None
    while position < len(data):
        length, kind = struct.unpack('>I4s', data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            chunks.append(body)
        position += 12 + length
    width, height, depth, colour, _, _, interlace = header
    assert depth == 8 and colour in (2, 6) and interlace == 0, header
    size = 3 if colour == 2 else 4
    raw, stride, rows, previous = zlib.decompress(b''.join(chunks)), width * size, [], bytearray(width * size)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - size] if x >= size else 0
            up, corner = previous[x], previous[x - size] if x >= size else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                estimate = left + up - corner
                nearest = min((abs(estimate - left), 0, left), (abs(estimate - up), 1, up), (abs(estimate - corner), 2, corner))[2]
                line[x] = (line[x] + nearest) & 255
        rows.append([tuple(line[x:x + 3]) for x in range(0, stride, size)])
        previous = line
    return width, height, rows


def through_proxy(proxy, url, method='GET'):
    """(status, headers, body) of a request sent to a render proxy the way Chrome sends it."""
    host, port = proxy.rsplit(':', 1)
    connection = http.client.HTTPConnection(host.replace('http://', ''), int(port), timeout=10)
    try:
        connection.request(method, url)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def jpeg_header(width, height):
    app0 = b'JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00'
    sof = struct.pack('>BHHB', 8, height, width, 3) + b'\x01\x11\x00\x02\x11\x01\x03\x11\x01'
    return (b'\xff\xd8' + b'\xff\xe0' + struct.pack('>H', len(app0) + 2) + app0
            + b'\xff\xc0' + struct.pack('>H', len(sof) + 2) + sof + b'\xff\xd9')


def now_iso(offset_seconds=0):
    return (dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=offset_seconds)).isoformat(timespec='seconds').replace('+00:00', 'Z')


class FakeThoughts:
    """The subset of the agent-notes HTTP API the Mac uses, recording every request."""

    def __init__(self):
        self.notes, self.replies, self.blobs, self.requests = {}, [], {}, []
        self.sequence = self.reply_sequence = 0
        self.live = False
        self.reply_listening = None   # the create answer's reply_listening (None: an older server sends none)
        self.fail = {}            # path -> (status, body bytes), one shot
        self.etags = True         # the note list's ETag and 304 answers (False: a server from before them)
        self.held = self.most_held = 0   # folder-sync long polls (reply feed, wait > 0, not listening): now, at most
        self.condition = threading.Condition()
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send(self, status, body=None, content_type='application/json', headers=None):
                data = b'' if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
                self.send_response(status)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', str(len(data)))
                for name, value in (headers or {}).items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(data)

            def handle_any(self, method):
                parts = urllib.parse.urlsplit(self.path)
                query = dict(urllib.parse.parse_qsl(parts.query))
                length = int(self.headers.get('Content-Length') or 0)
                body = json.loads(self.rfile.read(length)) if length else None
                with fake.condition:
                    fake.requests.append(dict(method=method, path=parts.path, query=query, body=body,
                                              authorization=self.headers.get('Authorization'),
                                              if_none_match=self.headers.get('If-None-Match'), at=time.monotonic()))
                    failure = fake.fail.pop(parts.path, None)
                if failure:
                    return self.send(failure[0], failure[1])
                if self.headers.get('Authorization') != 'Bearer ' + TOKEN:
                    return self.send(401, {'detail': 'Invalid device token'})
                headers = None
                if method == 'GET' and parts.path == '/v1/agent-notes' and fake.etags:
                    # As the server does since list ETags: 304 while nothing about any note changed.
                    headers = {'ETag': fake.list_etag(query)}
                    if self.headers.get('If-None-Match') == headers['ETag']:
                        return self.send(304, None, headers=headers)
                status, payload, content_type = fake.route(method, parts.path, query, body)
                self.send(status, payload, content_type, headers if status == 200 else None)

            def do_GET(self):
                self.handle_any('GET')

            def do_POST(self):
                self.handle_any('POST')

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}'
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def create_answer(self, note, listeners):
        answer = {'note': self.note_view(note), 'delivery': {'live': self.live, 'listeners': listeners}}
        if self.reply_listening is not None:
            answer['reply_listening'] = self.reply_listening
        return answer

    def feed_requests(self):
        with self.condition:
            return [r for r in self.requests if r['path'] == '/v1/agent-notes/replies']

    def sync_requests(self):
        """The plain folder sync's requests: the reply feed without listen, and the note list."""
        with self.condition:
            return [r for r in self.requests if r['method'] == 'GET' and (
                (r['path'] == '/v1/agent-notes/replies' and not r['query'].get('listen')) or r['path'] == '/v1/agent-notes')]

    def list_etag(self, query):
        """Like the server's: from what any note shows (counts and stamps), whatever the limit asks for."""
        with self.condition:
            state = (len(self.notes), self.sequence, len(self.replies), self.reply_sequence,
                     sorted((n['note_id'], n.get('delivered_at'), n.get('opened_at'), n.get('deleted')) for n in self.notes.values()),
                     sorted(query.items()))
        return '"' + hashlib.sha256(repr(state).encode()).hexdigest() + '"'

    def note_view(self, note, with_replies=True):
        view = dict(note)
        if with_replies:
            view['replies'] = [r for r in self.replies if r['note_id'] == note['note_id']]
        return view

    def add_reply(self, note_id, image=None, strokes=True, message=None, photo=None, capture=None):
        image = image or png(12, 9, (20, 20, 220))
        with self.condition:
            self.reply_sequence += 1
            reply_id = str(uuid.uuid4())
            self.blobs[f'/v1/agent-notes/replies/{reply_id}/image'] = (image, 'image/png')
            photo_record = None
            if photo is not None:
                url = f'/v1/agent-notes/replies/{reply_id}/photo'
                self.blobs[url] = (photo, 'image/jpeg')
                photo_record = {'url': url, 'media_type': 'image/jpeg', 'width': 640, 'height': 480, 'bytes': len(photo),
                                'sha256': 'p', 'capture': capture}
            document = None
            if strokes:
                document = {'schema_version': 1, 'image_width': 12, 'image_height': 9, 'source_width': 12, 'source_height': 9,
                            'strokes': [{'tool': 'pen', 'color': '#ff0000', 'opacity': 1.0, 'width': 6.0, 'points': [[1, 1], [2, 2]]},
                                        {'tool': 'highlighter', 'color': '#00AA00', 'opacity': 0.35, 'width': 24.0, 'points': [[3, 3]]},
                                        {'tool': 'pen', 'color': '#FF0000', 'opacity': 1.0, 'width': 6.0, 'points': [[4, 4], [5, 5], [6, 6]]}]}
                self.blobs[f'/v1/agent-notes/replies/{reply_id}/strokes'] = (json.dumps(document).encode(), 'application/json')
            note = self.notes[note_id]
            reply = {'reply_id': reply_id, 'sequence': self.reply_sequence, 'note_id': note_id, 'device_id': 'phone',
                     'created_at': now_iso(), 'media_type': 'image/png', 'image_width': 12, 'image_height': 9,
                     'image_bytes': len(image), 'image_sha256': 'x', 'image_url': f'/v1/agent-notes/replies/{reply_id}/image',
                     'strokes_url': f'/v1/agent-notes/replies/{reply_id}/strokes' if strokes else None,
                     'stroke_count': len(document['strokes']) if document else 0, 'message': message,
                     'photo': photo_record}
            self.replies.append(reply)
            self.condition.notify_all()
            return reply

    def add_note(self, title='Older note', created_at=None, sender='thoughts-mac', origin=None, note_id=None):
        """A note sent earlier (e.g. by another Mac process, or the phone), not in the local folder."""
        note_id = note_id or str(uuid.uuid4())
        with self.condition:
            self.sequence += 1
            self.blobs[f'/v1/agent-notes/{note_id}/image'] = (png(4, 4), 'image/png')
            self.notes[note_id] = {'note_id': note_id, 'sequence': self.sequence, 'created_at': created_at or now_iso(-3 * 86400),
                                   'sender_device_id': sender, 'title': title, 'media_type': 'image/png',
                                   'image_width': 4, 'image_height': 4, 'image_url': f'/v1/agent-notes/{note_id}/image',
                                   'review_card_id': None, 'document_id': None, 'origin': origin, 'deleted': False}
        return note_id

    def route(self, method, path, query, body):
        with self.condition:
            if method == 'POST' and path == '/v1/agent-notes':
                data = base64.b64decode(body['image']['data_base64'])
                if not (data.startswith(b'\x89PNG') or data.startswith(b'\xff\xd8\xff')):
                    return 422, {'detail': 'Not a PNG or JPEG', 'error_code': 'unsupported_image'}, 'application/json'
                existing = self.notes.get(body['note_id'])
                if existing:
                    return 200, self.create_answer(existing, []), 'application/json'
                self.sequence += 1
                width, height = agent_notes.image_info(data)[1:]
                note = {'note_id': body['note_id'], 'sequence': self.sequence, 'created_at': now_iso(), 'sender_device_id': 'thoughts-mac',
                        'title': body['title'], 'message': body.get('message'), 'media_type': body['image']['media_type'],
                        'image_width': width, 'image_height': height, 'image_bytes': len(data),
                        'image_url': f"/v1/agent-notes/{body['note_id']}/image", 'document_id': body.get('document_id'),
                        'document_filename': body.get('document_filename'), 'review_card_id': body.get('review_card_id'),
                        'review_question': 'What is X?' if body.get('review_card_id') else None, 'origin': body.get('origin'),
                        'delivered_at': None, 'opened_at': None, 'deleted': False}
                if getattr(self, 'supports_links', True):
                    note['links'] = body.get('links') or []
                self.notes[note['note_id']] = note
                self.blobs[note['image_url']] = (data, body['image']['media_type'])
                listeners = [{'device_id': 'phone', 'last_seen_at': now_iso(), 'context': {}}] if self.live else []
                return 201, self.create_answer(note, listeners), 'application/json'
            if method == 'GET' and path == '/v1/agent-notes':
                # sent_by_me as the server reads it: this client is thoughts-mac.
                mine = {'true': True, 'false': False}.get(query.get('sent_by_me'))
                notes = [n for n in self.notes.values()
                         if all(not query.get(k) or n.get(k) == query[k] for k in ('document_id', 'review_card_id'))
                         and (mine is None or (n['sender_device_id'] == 'thoughts-mac') == mine)]
                notes.sort(key=lambda n: -n['sequence'])
                return 200, {'notes': [self.note_view(n) for n in notes[:int(query.get('limit', 50))]]}, 'application/json'
            if method == 'GET' and path == '/v1/agent-notes/replies':
                after, note_id, conversation = int(query.get('after') or 0), query.get('note_id'), query.get('conversation_id')

                def matching():
                    return [r for r in self.replies if r['sequence'] > after and (not note_id or r['note_id'] == note_id)
                            and (not conversation or (self.notes[r['note_id']].get('origin') or {}).get('conversation_id') == conversation)]
                deadline = time.monotonic() + int(query.get('wait') or 0)
                folder_sync = int(query.get('wait') or 0) > 0 and not query.get('listen')
                if folder_sync:
                    self.held += 1
                    self.most_held = max(self.most_held, self.held)
                try:
                    while not matching() and time.monotonic() < deadline:
                        self.condition.wait(max(0.0, deadline - time.monotonic()))
                finally:
                    if folder_sync:
                        self.held -= 1
                found = matching()[:50]
                replies = [{**r, 'note': {k: self.notes[r['note_id']].get(k) for k in ('note_id', 'title', 'document_id', 'review_card_id')}}
                           for r in found]
                return 200, {'replies': replies, 'cursor': max([r['sequence'] for r in found] + [after])}, 'application/json'
            if method == 'GET' and path in self.blobs:
                data, content_type = self.blobs[path]
                return 200, data, content_type
            match = re.fullmatch(r'/v1/agent-notes/([0-9a-f-]{36})', path)
            if method == 'GET' and match:
                note = self.notes.get(match.group(1))
                if not note:
                    return 404, {'detail': 'No such note', 'error_code': 'agent_note_not_found'}, 'application/json'
                return 200, self.note_view(note), 'application/json'
            return 404, {'detail': 'Not Found'}, 'application/json'


class FakeServerCase(unittest.TestCase):
    def setUp(self):
        self.fake = FakeThoughts()
        self.addCleanup(self.fake.close)
        self.directory = Path(tempfile.mkdtemp(prefix='thoughts-agent-notes-test-'))
        self.addCleanup(lambda: __import__('shutil').rmtree(self.directory, True))
        self.root = self.directory / 'agent-notes'
        self.client = agent_notes.Client(agent_notes.Config(self.fake.url, TOKEN))

    def send(self, **kwargs):
        image = self.directory / 'picture.png'
        image.write_bytes(png())
        args = dict(image_path=str(image), client=self.client, root=self.root)
        args.update(kwargs)
        return agent_notes.send_note(args.pop('title', 'Group mean'), **args)


class ConfigTest(unittest.TestCase):
    def test_environment_overrides_and_keychain(self):
        calls = []

        def keychain():
            calls.append(1)
            return 'keychain-token'
        with tempfile.TemporaryDirectory() as home:
            config = agent_notes.load_config({'THOUGHTS_SERVER_URL': 'http://127.0.0.1:9/', 'THOUGHTS_TOKEN': 'env-token'}, keychain, Path(home))
            self.assertEqual((config.server_url, config.token), ('http://127.0.0.1:9', 'env-token'))
            self.assertEqual(calls, [], 'the Keychain is not read when THOUGHTS_TOKEN is set')
            self.assertNotIn('env-token', repr(config))
            settings = Path(home) / 'Library/Application Support/Thoughts'
            settings.mkdir(parents=True)
            (settings / 'server.json').write_text(json.dumps({'schema_version': 1, 'server_url': 'https://thoughts.example'}))
            config = agent_notes.load_config({}, keychain, Path(home))
            self.assertEqual((config.server_url, config.token), ('https://thoughts.example', 'keychain-token'))
            self.assertEqual(len(calls), 1)
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.load_config({}, lambda: None, Path(home))
            self.assertEqual(caught.exception.error_code, 'no_credentials')
            (settings / 'server.json').unlink()
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.load_config({'THOUGHTS_TOKEN': 'x'}, keychain, Path(home))
            self.assertEqual(caught.exception.error_code, 'no_server_config')

    def test_plain_http_only_for_this_computer(self):
        for url in ('http://127.0.0.1:8080', 'http://localhost:1/', 'http://[::1]:9', 'http://dev.localhost', 'https://thoughts.example'):
            self.assertEqual(agent_notes.load_config({'THOUGHTS_SERVER_URL': url, 'THOUGHTS_TOKEN': 't'}, lambda: None).server_url, url.rstrip('/'))
        for url in ('http://thoughts.example', 'http://192.168.1.10:8000', 'http://localhost.example.org', 'http://127.0.0.1.example.org',
                    'http://10.0.0.5', 'http://0.0.0.0:8000', 'ftp://127.0.0.1'):
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.load_config({'THOUGHTS_SERVER_URL': url, 'THOUGHTS_TOKEN': 't'}, lambda: None)
            self.assertEqual(caught.exception.error_code, 'no_server_config', url)
        with tempfile.TemporaryDirectory() as home:
            folder = Path(home) / 'Library/Application Support/Thoughts'
            folder.mkdir(parents=True)
            (folder / 'server.json').write_text(json.dumps({'server_url': 'http://thoughts.example'}))
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.load_config({'THOUGHTS_TOKEN': 't'}, lambda: None, Path(home))
            self.assertIn('https://', caught.exception.detail)

    def test_keychain_token_goes_only_to_the_server_that_issued_it(self):
        token = 'keychain-token'

        def config(settings, environ=None):
            with tempfile.TemporaryDirectory() as home:
                folder = Path(home) / 'Library/Application Support/Thoughts'
                folder.mkdir(parents=True)
                (folder / 'server.json').write_text(json.dumps(settings))
                return agent_notes.load_config(environ or {}, lambda: token, Path(home))

        def refused(settings, environ=None):
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                config(settings, environ)
            self.assertEqual(caught.exception.error_code, 'no_credentials')
            self.assertNotIn(token, caught.exception.detail)
            return caught.exception.detail
        first, second = 'https://first.example.org', 'https://second.example.org'
        fingerprint = agent_notes.token_fingerprint(token)
        # The same value Thoughts writes (autotests/thoughtsserveraddresstest.cpp).
        self.assertEqual(agent_notes.token_fingerprint('token-one'), '5c8fb37591dc58fc3ab5fd7f8bbc15c4')
        # The record Thoughts keeps: the token was issued by the first server.
        bound = {'thoughts-mac': {'origin': first, 'token_fingerprint': fingerprint}}
        self.assertEqual(config({'server_url': first + '/prefix', 'token_origins': bound}).token, token)
        # The address was changed to another server: the old token stays here.
        detail = refused({'server_url': second, 'token_origins': bound})
        self.assertIn(f'was issued for {first} and is not sent to {second}', detail)
        self.assertIn('thoughtsctl store-secret --account thoughts-mac', detail)
        # So it does under a THOUGHTS_SERVER_URL override, unless that names the same server.
        refused({'server_url': first, 'token_origins': bound}, {'THOUGHTS_SERVER_URL': second})
        refused({'server_url': first, 'token_origins': bound}, {'THOUGHTS_SERVER_URL': 'https://first.example.org:8443'})
        self.assertEqual(config({'server_url': first, 'token_origins': bound}, {'THOUGHTS_SERVER_URL': 'HTTPS://First.Example.org:443/'}).token, token)
        # A token given for the process goes where the process goes.
        self.assertEqual(config({'server_url': first, 'token_origins': bound}, {'THOUGHTS_SERVER_URL': second, 'THOUGHTS_TOKEN': 'given'}).token, 'given')
        # Before Thoughts recorded anything the token belongs to the saved address,
        self.assertEqual(config({'server_url': first}).token, token)
        refused({'server_url': first}, {'THOUGHTS_SERVER_URL': second})
        # and after a change of address to the server used until then.
        refused({'server_url': second, 'token_origins': {'*': {'origin': first}}})
        # Another token was stored since the record: it is the saved server's.
        other = {'thoughts-mac': {'origin': first, 'token_fingerprint': 'f' * 32}}
        self.assertEqual(config({'server_url': second, 'token_origins': other}).token, token)
        refused({'server_url': second, 'token_origins': other}, {'THOUGHTS_SERVER_URL': first})
        # A damaged record is no record.
        self.assertEqual(config({'server_url': first, 'token_origins': ['x']}).token, token)
        self.assertEqual(agent_notes.url_origin('https://Example.org:443/a?b'), 'https://example.org')
        self.assertEqual(agent_notes.url_origin('http://127.0.0.1:8080/'), 'http://127.0.0.1:8080')
        self.assertEqual(agent_notes.url_origin('http://[::1]:80/'), 'http://[::1]')
        self.assertEqual(agent_notes.url_origin('file:///etc/passwd'), '')

    def test_folder_override(self):
        self.assertEqual(agent_notes.notes_root({'THOUGHTS_AGENT_NOTES_DIR': '/tmp/x-notes'}), Path('/tmp/x-notes'))
        self.assertEqual(agent_notes.notes_root({}), Path.home() / 'Thoughts/agent-notes')


class PathTest(unittest.TestCase):
    """Files an agent names (image_path, html_path, a PDF to import): never from private locations."""

    def setUp(self):
        directory = tempfile.mkdtemp(prefix='thoughts-paths-')
        self.addCleanup(lambda: __import__('shutil').rmtree(directory, True))
        self.home = Path(os.path.realpath(directory)) / 'home'
        self.other = Path(os.path.realpath(directory)) / 'elsewhere'
        for name in ('.ssh/id_rsa', '.aws/credentials', '.config/gcloud/credentials.db', '.gnupg/private-keys-v1.d/key', '.netrc',
                     '.claude/projects/x/memory.md', 'Library/Keychains/login.keychain-db', 'Library/Application Support/Thoughts/server.json',
                     'Library/Cookies/Cookies.binarycookies', 'Library/Mobile Documents/notes/picture.png', 'Documents/plot.png',
                     'Documents/sub/deep.png', 'Desktop/page.html', 'Thoughts/agent-notes/note/note.png', 'Pictures/old.keychain/item.png',
                     'Documents/copy.keychain-db', 'notes.png'):
            path = self.home / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(png(2, 2))
        (self.other / 'pictures').mkdir(parents=True)
        (self.other / 'pictures/free.png').write_bytes(png(2, 2))

    def refused(self, value, homes=True):
        with self.assertRaises(agent_notes.AgentNoteError, msg=value) as caught:
            agent_notes.readable_path(str(value), 'image_path', [self.home] if homes else None)
        self.assertEqual(caught.exception.error_code, 'path_refused', value)
        return caught.exception.detail

    def allowed(self, value):
        return agent_notes.readable_path(str(value), 'image_path', [self.home])

    def test_hidden_folders_library_and_keychains_are_refused(self):
        for name in ('.ssh/id_rsa', '.aws/credentials', '.config/gcloud/credentials.db', '.gnupg/private-keys-v1.d/key', '.netrc',
                     '.claude/projects/x/memory.md', '.ssh', '.ssh/missing'):
            self.assertIn('a hidden folder of the home directory', self.refused(self.home / name))
        for name in ('Library/Keychains/login.keychain-db', 'Library/Application Support/Thoughts/server.json', 'Library/Cookies/Cookies.binarycookies',
                     'Library/Mobile Documents/notes/picture.png', 'Library'):
            self.refused(self.home / name)
        self.assertIn('a keychain', self.refused(self.home / 'Pictures/old.keychain/item.png'))
        self.assertIn('a keychain', self.refused(self.home / 'Documents/copy.keychain-db'))
        self.assertIn('a keychain', self.refused(self.other / 'Backup.KEYCHAIN-DB'))
        self.assertIn(str(self.home / '.ssh/id_rsa'), self.refused(self.home / '.ssh/id_rsa'))

    def test_other_places_are_allowed(self):
        for name in ('Documents/plot.png', 'Documents/sub/deep.png', 'Desktop/page.html', 'Thoughts/agent-notes/note/note.png', 'notes.png',
                     'Documents/not-there-yet.png'):
            self.assertEqual(self.allowed(self.home / name), self.home / name)
        self.assertEqual(self.allowed(self.other / 'pictures/free.png'), self.other / 'pictures/free.png')
        # A hidden folder that is not directly in the home directory is an ordinary folder.
        (self.home / 'Documents/.cache').mkdir()
        self.assertEqual(self.allowed(self.home / 'Documents/.cache/x.png'), self.home / 'Documents/.cache/x.png')
        # The temporary folders agents render into.
        with tempfile.NamedTemporaryFile(suffix='.png') as scratch:
            self.assertEqual(agent_notes.readable_path(scratch.name, 'image_path'), Path(os.path.realpath(scratch.name)))

    def test_links_are_followed_before_the_check(self):
        os.symlink(self.home / '.ssh/id_rsa', self.home / 'Documents/key.png')
        os.symlink(self.home / 'Library', self.home / 'Documents/lib')
        os.symlink(self.home / '.aws', self.other / 'cloud')
        os.symlink(self.home / 'Documents/key.png', self.other / 'twice.png')
        os.symlink(self.other / 'pictures', self.home / 'Documents/shared')
        self.refused(self.home / 'Documents/key.png')
        self.refused(self.home / 'Documents/lib/Cookies/Cookies.binarycookies')
        self.refused(self.other / 'cloud/credentials')
        self.refused(self.other / 'twice.png')
        self.refused(self.home / 'Documents/../.ssh/id_rsa')
        self.refused(self.home / 'Documents/sub/../../.netrc')
        # A link out of the home directory to an ordinary place is fine, and reported resolved.
        self.assertEqual(self.allowed(self.home / 'Documents/shared/free.png'), self.other / 'pictures/free.png')
        with self.assertRaises(agent_notes.AgentNoteError):
            agent_notes.readable_path('/tmp/a\x00b.png', 'image_path', [self.home])

    def test_letter_case_does_not_hide_a_private_folder(self):
        if not (self.home / 'library').exists():
            self.skipTest('this file system distinguishes letter case')
        self.refused(self.home / 'library/Cookies/Cookies.binarycookies')
        self.refused(self.home / 'LIBRARY/Keychains/LOGIN.KEYCHAIN-DB')
        self.refused(self.home / '.SSH/id_rsa')
        self.refused(Path(str(self.home).upper()) / '.ssh/id_rsa')
        self.refused(Path(str(self.home).upper()) / 'Library/Cookies/Cookies.binarycookies')

    def test_the_home_directory_is_the_accounts_and_the_environments(self):
        with unittest.mock.patch.dict(os.environ, {'HOME': str(self.home)}):
            homes = [Path(os.path.realpath(home)) for home in agent_notes._home_directories()]
            self.assertIn(self.home, homes)
            self.assertIn('~/.ssh', self.refused('~/.ssh/id_rsa', homes=False))
            self.refused(self.home / 'Library/Cookies/Cookies.binarycookies', homes=False)
        if hasattr(os, 'getuid'):
            import pwd
            self.assertIn(Path(os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir)), [Path(os.path.realpath(h)) for h in agent_notes._home_directories()])

    def test_system_credential_folders_are_refused(self):
        for name in ('/etc/hosts', '/private/etc/hosts', '/etc/ssh/ssh_host_rsa_key', '/var/db/anything', '/private/var/db/dslocal/nodes/Default/users/root.plist',
                     '/var/root/.ssh/id_rsa', '/Library/Keychains/System.keychain', '/dev/null', '/proc/self/environ', '/run/secrets/token'):
            if os.path.exists(os.path.dirname(os.path.realpath(name))) or name.startswith(('/Library/Keychains', '/etc/')):
                with self.assertRaises(agent_notes.AgentNoteError, msg=name) as caught:
                    agent_notes.readable_path(name, 'image_path', [self.home])
                self.assertEqual(caught.exception.error_code, 'path_refused', name)

    def test_send_note_reads_nothing_private(self):
        calls = []

        class NoClient:
            def __getattr__(self, name):
                calls.append(name)
                raise AssertionError('nothing may be sent')
        with unittest.mock.patch.object(agent_notes, '_home_directories', lambda: [self.home]):
            for kwargs in ({'image_path': str(self.home / '.ssh/id_rsa')}, {'image_path': str(self.home / 'Library/Mobile Documents/notes/picture.png')},
                           {'html_path': str(self.home / '.config/gcloud/credentials.db')}, {'html_path': '/etc/hosts'}):
                with self.assertRaises(agent_notes.AgentNoteError) as caught:
                    agent_notes.send_note('Title', client=NoClient(), root=self.other / 'notes', **kwargs)
                self.assertEqual(caught.exception.error_code, 'path_refused', kwargs)
        self.assertEqual(calls, [])
        self.assertFalse((self.other / 'notes').exists())


class ImageTest(unittest.TestCase):
    def test_png_and_jpeg_by_signature(self):
        self.assertEqual(agent_notes.check_image(png(30, 20)), ('image/png', 30, 20))
        self.assertEqual(agent_notes.check_image(jpeg_header(640, 480)), ('image/jpeg', 640, 480))
        for data in (b'GIF89a....', b'', b'\x89PNG\r\n\x1a\n' + b'\0' * 20, b'\xff\xd8\xff\xd9'):
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.check_image(data)
            self.assertEqual(caught.exception.error_code, 'unsupported_image')
        for width, height in ((9000, 10), (8000, 8000)):
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                agent_notes.check_image(jpeg_header(width, height))
            self.assertEqual(caught.exception.error_code, 'agent_note_too_large')


class ClientTest(FakeServerCase):
    def test_send_note_origin_names_the_conversation(self):
        # The calling conversation (server.current_conversation) adds its link and title.
        self.send(title='Study', conversation={'agent': 'claude', 'conversation_id': '5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139',
                                               'conversation_url': 'https://claude.ai/code/session_01ExampleSessionAbcdEfgh',
                                               'title': 'Study GRPO'})
        body = [r for r in self.fake.requests if r['method'] == 'POST'][-1]['body']
        self.assertEqual(body['origin'], {'agent': 'mcp', 'conversation_agent': 'claude', 'conversation_id': '5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139',
                                          'conversation_url': 'https://claude.ai/code/session_01ExampleSessionAbcdEfgh',
                                          'conversation_title': 'Study GRPO'})
        self.send(title='Plain', conversation={'agent': 'claude', 'conversation_id': '5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139',
                                               'conversation_url': '', 'title': ''})
        body = [r for r in self.fake.requests if r['method'] == 'POST'][-1]['body']
        self.assertEqual(body['origin'], {'agent': 'mcp', 'conversation_agent': 'claude', 'conversation_id': '5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139'})

    def test_send_note_request_and_folder(self):
        card = str(uuid.uuid4())
        result = self.send(title='GRPO: group mean!', message='Circle the baseline.', review_card_id=card,
                           conversation_id='conv-1', document_filename='rl.pdf')
        request = [r for r in self.fake.requests if r['method'] == 'POST'][0]
        body = request['body']
        self.assertEqual(request['authorization'], 'Bearer ' + TOKEN)
        self.assertEqual(body['note_id'], result['note_id'])
        self.assertTrue(agent_notes.UUID_RE.match(body['note_id']))
        self.assertEqual(base64.b64decode(body['image']['data_base64']), png())
        self.assertEqual(body['image']['media_type'], 'image/png')
        self.assertEqual(body['origin'], {'agent': 'mcp', 'conversation_id': 'conv-1'})
        self.assertEqual((body['title'], body['message'], body['review_card_id'], body['document_filename']),
                         ('GRPO: group mean!', 'Circle the baseline.', card, 'rl.pdf'))
        self.assertNotIn('document_id', body)
        self.assertFalse(result['delivery']['live'])
        self.assertIn('Ask the user to open Thoughts on the phone', result['next_step'])
        self.assertEqual(result['image'], {'media_type': 'image/png', 'width': 8, 'height': 6, 'bytes': len(png())})
        self.assertEqual(result['review_question'], 'What is X?')
        folder = Path(result['folder'])
        self.assertEqual(folder.parent, self.root)
        self.assertRegex(folder.name, r'^\d{8}-\d{6}-grpo-group-mean-' + result['note_id'][:8] + '$')
        self.assertEqual(Path(result['image_path']), folder / 'note.png')
        self.assertEqual((folder / 'note.png').read_bytes(), png())
        self.assertEqual(json.loads((folder / 'note.json').read_text())['note_id'], result['note_id'])
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ['note.json', 'note.png'])
        self.assertEqual(result['deliveries'], [])  # this fake is a server without per-device delivery
        # Same note_id again: the server's stored note, the same folder.
        self.fake.live = True
        again = self.send(title='GRPO: group mean!', note_id=result['note_id'])
        self.assertEqual(again['folder'], result['folder'])
        self.assertTrue(again['delivery']['live'])
        self.assertIn('The note is open on the phone', again['next_step'])
        self.assertEqual(len([p for p in self.root.iterdir() if p.is_dir()]), 1)

    def test_pages_from_the_phone(self):
        """Study mode: pages the phone sends are notes from it; the watcher finds them, get_page downloads one."""
        self.send(title='From the Mac')
        older = self.fake.add_note('Agent note from elsewhere')
        from_phone = self.fake.add_note('Paper · page 1', created_at=now_iso(), sender='example-phone', origin={'kind': 'phone_page', 'page': 1})
        # The Thoughts app on this Mac sends pages as this Mac (thoughts-mac): found too.
        page = self.client.create_note({'note_id': str(uuid.uuid4()), 'title': 'AgentsVsNetwork · page 3', 'message': 'Is this right?',
                                        'document_id': 'doc-1', 'document_filename': 'main.pdf',
                                        'origin': {'kind': 'phone_page', 'device': 'mac', 'page': 3, 'pages': 12, 'document_name': 'AgentsVsNetwork',
                                                   'stroke_count': 2},
                                        'image': {'media_type': 'image/png', 'data_base64': base64.b64encode(png(10, 14)).decode()}})['note']
        found = agent_notes.new_phone_pages(self.client, 0)
        self.assertEqual([n['note_id'] for n in found], [from_phone, page['note_id']])
        self.assertEqual(agent_notes.new_phone_pages(self.client, page['sequence']), [])
        self.assertTrue(agent_notes.is_phone_page(page) and not agent_notes.is_phone_page(self.fake.notes[older]))
        entry = agent_notes.get_page(page['note_id'], client=self.client, root=self.root)
        self.assertEqual((entry['title'], entry['message'], entry['page'], entry['pages'], entry['document_name'], entry['stroke_count']),
                         ('AgentsVsNetwork · page 3', 'Is this right?', 3, 12, 'AgentsVsNetwork', 2))
        self.assertTrue(entry['phone_page'])
        self.assertEqual(Path(entry['image_path']).read_bytes(), png(10, 14))
        # Asked again: the same file, no second download.
        downloads = len([r for r in self.fake.requests if r['path'].endswith('/image')])
        self.assertEqual(agent_notes.get_page(page['note_id'], client=self.client, root=self.root)['image_path'], entry['image_path'])
        self.assertEqual(len([r for r in self.fake.requests if r['path'].endswith('/image')]), downloads)

    def test_inputs_are_checked_before_sending(self):
        for kwargs in ({'html': '<p>x</p>'}, {'image_path': None}, {'note_id': 'not-a-uuid'}, {'title': ' '}):
            with self.assertRaises(agent_notes.AgentNoteError) as caught:
                self.send(**kwargs)
            self.assertEqual(caught.exception.error_code, 'invalid_agent_note', kwargs)
        gif = self.directory / 'x.gif'
        gif.write_bytes(b'GIF89a' + b'\0' * 20)
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.send(image_path=str(gif))
        self.assertEqual(caught.exception.error_code, 'unsupported_image')
        self.assertEqual(self.fake.requests, [])

    def test_error_mapping(self):
        self.fake.fail['/v1/agent-notes'] = (409, json.dumps({'detail': 'Different content', 'error_code': 'agent_note_conflict'}).encode())
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.send()
        self.assertEqual(caught.exception.as_dict(), {'error_code': 'agent_note_conflict', 'detail': 'Different content', 'http_status': 409})
        self.fake.fail['/v1/agent-notes'] = (500, b'<html>Internal error</html>')
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.send()
        self.assertEqual(caught.exception.error_code, 'server_error')
        self.fake.fail['/v1/agent-notes'] = (422, json.dumps({'detail': [{'msg': 'field required'}]}).encode())
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.send()
        self.assertEqual((caught.exception.error_code, caught.exception.detail), ('invalid_agent_note', 'field required'))
        wrong = agent_notes.Client(agent_notes.Config(self.fake.url, 'wrong-token'))
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            wrong.list_notes()
        self.assertEqual(caught.exception.error_code, 'unauthorized')
        self.assertNotIn('wrong-token', str(caught.exception.as_dict()))
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.client.request('GET', '/v1/agent-notes/unknown-endpoint')
        self.assertEqual(caught.exception.error_code, 'not_found')
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            self.client.get_note(str(uuid.uuid4()))
        self.assertEqual(caught.exception.error_code, 'agent_note_not_found')
        port = self.fake.server.server_address[1]
        self.fake.close()
        closed = agent_notes.Client(agent_notes.Config(f'http://127.0.0.1:{port}', TOKEN), connect_timeout=2)
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            closed.list_notes()
        self.assertEqual(caught.exception.error_code, 'server_unreachable')
        self.assertNotIn(TOKEN, caught.exception.detail)

    def test_list_notes_marks_local_folders(self):
        sent = self.send(review_card_id='card-1')
        older = self.fake.add_note()
        listed = agent_notes.list_notes(client=self.client, root=self.root)
        self.assertEqual([n['note_id'] for n in listed['notes']], [older, sent['note_id']])
        self.assertEqual(listed['notes'][1]['local_folder'], sent['folder'])
        self.assertIsNone(listed['notes'][0]['local_folder'])
        only = agent_notes.list_notes(review_card_id='card-1', client=self.client, root=self.root)
        self.assertEqual([n['note_id'] for n in only['notes']], [sent['note_id']])
        self.assertEqual(self.fake.requests[-1]['query'], {'review_card_id': 'card-1', 'limit': '20'})


    def test_per_device_deliveries_pass_through(self):
        sent = self.send(review_card_id='card-1')
        deliveries = [
            {'device_id': 'thoughts-android-moto-g34', 'delivered_at': now_iso(), 'opened_at': now_iso()},
            {'device_id': 'example-phone', 'delivered_at': now_iso(), 'opened_at': None},
        ]
        self.fake.notes[sent['note_id']]['deliveries'] = deliveries
        listed = agent_notes.list_notes(client=self.client, root=self.root)
        self.assertEqual(listed['notes'][0]['deliveries'], deliveries)
        again = self.send(review_card_id='card-1', note_id=sent['note_id'])
        self.assertEqual(again['deliveries'], deliveries)


class FolderTest(FakeServerCase):
    def test_replies_are_written_once_numbered_and_journaled(self):
        first = self.send(title='First')
        second = self.send(title='Second')
        a1 = self.fake.add_reply(first['note_id'], message='Here.')
        b1 = self.fake.add_reply(second['note_id'], strokes=False)
        a2 = self.fake.add_reply(first['note_id'])
        result = agent_notes.sync_replies(client=self.client, root=self.root)
        self.assertEqual([e['reply_id'] for e in result['replies']], [a1['reply_id'], b1['reply_id'], a2['reply_id']])
        self.assertTrue(all(e['new'] for e in result['replies']))
        self.assertEqual(result['cursor'], 3)
        self.assertEqual(agent_notes.read_cursor(self.root), 3)
        folder_a, folder_b = Path(first['folder']), Path(second['folder'])
        self.assertEqual(sorted(p.name for p in folder_a.iterdir()),
                         ['note.json', 'note.png', 'reply-1.json', 'reply-1.png', 'reply-1.strokes.json',
                          'reply-2.json', 'reply-2.png', 'reply-2.strokes.json'])
        self.assertEqual(sorted(p.name for p in folder_b.iterdir()), ['note.json', 'note.png', 'reply-1.json', 'reply-1.png'])
        record = json.loads((folder_a / 'reply-1.json').read_text())
        self.assertEqual((record['reply_id'], record['message'], record['note']['title']), (a1['reply_id'], 'Here.', 'First'))
        self.assertEqual(record['local'], {'image': 'reply-1.png', 'json': 'reply-1.json', 'strokes': 'reply-1.strokes.json'})
        self.assertEqual(json.loads((folder_a / 'reply-2.strokes.json').read_text())['schema_version'], 1)
        journal = [json.loads(line) for line in (self.root / 'replies.jsonl').read_text().splitlines()]
        self.assertEqual([j['reply_id'] for j in journal], [a1['reply_id'], b1['reply_id'], a2['reply_id']])
        self.assertEqual(journal[2]['image'], f'{folder_a.name}/reply-2.png')
        # Another process (a second call, from an older cursor too) writes nothing twice.
        (self.root / '.state.json').unlink()
        again = agent_notes.sync_replies(client=self.client, root=self.root)
        self.assertEqual([e['new'] for e in again['replies']], [False, False, False])
        self.assertEqual(len((self.root / 'replies.jsonl').read_text().splitlines()), 3)
        self.assertEqual(len(list(folder_a.glob('reply-*.png'))), 2)
        self.assertEqual(list(self.root.rglob('*.tmp')), [])
        # The summary of what was drawn.
        summary = agent_notes.stroke_summary(str(folder_a / 'reply-1.strokes.json'))
        self.assertEqual((summary['count'], summary['tools'], summary['colors'], summary['points']),
                         (3, {'pen': 2, 'highlighter': 1}, ['#FF0000', '#00AA00'], 6))
        listed = agent_notes.list_replies(client=self.client, root=self.root)
        self.assertEqual([e['reply_id'] for e in listed['replies']], [a1['reply_id'], b1['reply_id'], a2['reply_id']])
        self.assertTrue(Path(listed['replies'][0]['image_path']).is_file())

    def test_cursor_is_monotonic_and_filtered_reads_leave_it(self):
        note = self.send()
        self.fake.add_reply(note['note_id'])
        agent_notes.advance_cursor(self.root, 10)
        self.assertEqual(agent_notes.advance_cursor(self.root, 5), 10)
        self.assertEqual(agent_notes.read_cursor(self.root), 10)
        (self.root / '.state.json').unlink()
        filtered = agent_notes.sync_replies(note_id=note['note_id'], client=self.client, root=self.root)
        self.assertEqual(len(filtered['replies']), 1)
        self.assertIsNone(agent_notes.read_cursor(self.root), 'a note-filtered read does not move the feed cursor')
        agent_notes.sync_replies(client=self.client, root=self.root)
        self.assertEqual(agent_notes.read_cursor(self.root), 1)

    def test_concurrent_syncs_do_not_duplicate(self):
        note = self.send()
        for _ in range(4):
            self.fake.add_reply(note['note_id'])
        errors = []

        def run():
            try:
                agent_notes.sync_replies(after=0, client=agent_notes.Client(agent_notes.Config(self.fake.url, TOKEN)), root=self.root)
            except Exception as error:  # surfaced below
                errors.append(error)
        threads = [threading.Thread(target=run) for _ in range(3)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(20)
        self.assertEqual(errors, [])
        folder = Path(note['folder'])
        self.assertEqual(sorted(p.name for p in folder.glob('reply-*.png')), [f'reply-{n}.png' for n in range(1, 5)])
        ids = [json.loads((folder / f'reply-{n}.json').read_text())['reply_id'] for n in range(1, 5)]
        self.assertEqual(ids, [r['reply_id'] for r in self.fake.replies], 'numbered in reply sequence order')
        self.assertEqual(len((self.root / 'replies.jsonl').read_text().splitlines()), 4)

    def test_reply_to_a_note_not_in_the_folder(self):
        older = self.fake.add_note('Sent last week')
        reply = self.fake.add_reply(older)
        listed = agent_notes.list_replies(note_id=older, client=self.client, root=self.root)
        self.assertEqual([e['reply_id'] for e in listed['replies']], [reply['reply_id']])
        folder = Path(listed['replies'][0]['folder'])
        self.assertTrue(folder.name.endswith('-sent-last-week-' + older[:8]))
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ['note.json', 'note.png', 'reply-1.json', 'reply-1.png', 'reply-1.strokes.json'])
        self.assertFalse(agent_notes.has_recent_note(self.root), 'a three-day-old note does not keep the watcher running')
        self.send()
        self.assertTrue(agent_notes.has_recent_note(self.root))

    def test_wait_for_reply_returns_when_one_arrives(self):
        note = self.send()
        timer = threading.Timer(0.8, self.fake.add_reply, args=(note['note_id'],), kwargs={'message': 'Done'})
        timer.start()
        self.addCleanup(timer.cancel)
        started = time.monotonic()
        entry = agent_notes.wait_for_reply(note['note_id'], 20, client=self.client, root=self.root)
        elapsed = time.monotonic() - started
        self.assertLess(elapsed, 5)
        self.assertGreater(elapsed, 0.5)
        self.assertEqual(entry['message'], 'Done')
        self.assertTrue(Path(entry['image_path']).is_file())
        waits = [int(r['query'].get('wait', 0)) for r in self.fake.requests if r['path'] == '/v1/agent-notes/replies']
        self.assertTrue(waits and 0 < waits[0] <= 60, waits)
        # Newest of several, at once.
        newer = self.fake.add_reply(note['note_id'], message='Second')
        self.assertEqual(agent_notes.wait_for_reply(note['note_id'], 0, client=self.client, root=self.root)['reply_id'], newer['reply_id'])

    def test_no_reply_yet(self):
        note = self.send()
        started = time.monotonic()
        self.assertIsNone(agent_notes.wait_for_reply(note['note_id'], 1, client=self.client, root=self.root))
        self.assertGreaterEqual(time.monotonic() - started, 0.9)
        self.assertIsNone(agent_notes.wait_for_reply(note['note_id'], 0, client=self.client, root=self.root))

    def test_watch_loop_writes_and_stops(self):
        note = self.send()
        stop = threading.Event()
        seen = []
        thread = threading.Thread(target=agent_notes.watch_loop, args=(stop,),
                                  kwargs=dict(client_factory=lambda: self.client, root=self.root, wait=2, on_reply=seen.append))
        thread.start()
        self.fake.add_reply(note['note_id'])
        deadline = time.monotonic() + 5
        while not seen and time.monotonic() < deadline:
            time.sleep(0.05)
        stop.set()
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(seen), 1)
        self.assertTrue((Path(note['folder']) / 'reply-1.png').is_file())

        # A folder sync never says an agent listens: it wakes nobody.
        self.assertFalse(any(r['query'].get('listen') for r in self.fake.feed_requests()))


CONVERSATION = '3a7f52ca-b651-44dd-816e-2ac08f52f2c6'
WATCHER = Path(__file__).resolve().parent / 'thoughts-watch-replies'


class ReplyListeningTest(FakeServerCase):
    """protocol/agent-notes.md "Who hears a reply": which polls tell the server an agent listens."""

    def test_waiting_for_a_reply_listens_for_that_note_and_checking_once_does_not(self):
        note = self.send()
        self.assertIsNone(agent_notes.wait_for_reply(note['note_id'], 1, client=self.client, root=self.root))
        waited = [r['query'] for r in self.fake.feed_requests()]
        self.assertTrue(waited and all(q.get('listen') == 'true' and q.get('note_id') == note['note_id'] for q in waited
                                       if int(q.get('wait', 0)) > 0), waited)
        self.fake.requests.clear()
        agent_notes.wait_for_reply(note['note_id'], 0, client=self.client, root=self.root)
        agent_notes.list_replies(client=self.client, root=self.root)
        self.assertEqual([r['query'].get('listen') for r in self.fake.feed_requests()], [None, None])

    def test_send_note_reports_who_hears_the_reply(self):
        self.fake.reply_listening = {'state': 'not_listening', 'listeners': []}
        result = self.send()
        self.assertEqual(result['reply_listening'], {'state': 'not_listening', 'listeners': []})
        self.assertIn('thoughts-watch-replies', result['next_step'])
        self.assertIn('before you end your turn', result['next_step'].lower())

        listener = {'device_id': 'thoughts-mac', 'conversation_id': CONVERSATION, 'note_id': None, 'last_seen_at': now_iso()}
        self.fake.reply_listening = {'state': 'listening', 'listeners': [listener]}
        result = self.send()
        self.assertEqual(result['reply_listening']['state'], 'listening')
        self.assertEqual(result['reply_listening']['listeners'], [listener])
        self.assertNotIn('thoughts-watch-replies', result['next_step'])

        self.fake.reply_listening = None  # a server before agent_note_listening_v1
        result = self.send()
        self.assertEqual(result['reply_listening'], {'state': 'unknown', 'listeners': []})
        self.assertIn('thoughts-watch-replies', result['next_step'])

        # Only the reply agent (or a topic owner) listens: this conversation still starts its watcher.
        responder = {'device_id': 'server', 'conversation_id': None, 'note_id': None, 'last_seen_at': now_iso(),
                     'responder': True, 'role': 'responder'}
        self.fake.reply_listening = {'state': 'listening', 'listeners': [responder]}
        result = self.send()
        self.assertIn('start thoughts-watch-replies', result['next_step'])
        self.assertIn("answering rules", result['next_step'])

    def test_feed_cursor_is_the_newest_reply_not_the_first_page(self):
        ours = self.fake.add_note(title='Ours', origin={'agent': 'mcp', 'conversation_id': CONVERSATION})
        other = self.fake.add_note(title='Other')
        for _ in range(60):
            self.fake.add_reply(ours, strokes=False)
        newest_other = self.fake.add_reply(other, strokes=False)
        self.assertEqual(agent_notes.feed_cursor(self.client), newest_other['sequence'])
        self.assertEqual(agent_notes.feed_cursor(self.client, CONVERSATION), 60)
        self.assertEqual(agent_notes.feed_cursor(self.client, 'no-such-conversation'), 0)

    def watcher(self, *args):
        environment = {**os.environ, 'THOUGHTS_SERVER_URL': self.fake.url, 'THOUGHTS_TOKEN': TOKEN,
                       'THOUGHTS_AGENT_NOTES_DIR': str(self.root), 'CLAUDE_CONFIG_DIR': str(self.directory / 'claude')}
        process = subprocess.Popen([sys.executable, str(WATCHER), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=environment)
        self.addCleanup(lambda: (process.kill(), process.wait(5), process.stdout.close(), process.stderr.close()))
        lines = []

        def read():
            for line in process.stdout:
                lines.append(line.rstrip('\n'))
        threading.Thread(target=read, daemon=True).start()
        return process, lines

    def wait_until(self, condition, seconds=10):
        deadline = time.monotonic() + seconds
        while not condition() and time.monotonic() < deadline:
            time.sleep(0.05)
        return condition()

    def test_watcher_listens_for_its_conversation_and_starts_after_the_newest_reply(self):
        ours = self.fake.add_note(title='New drafter', origin={'agent': 'mcp', 'conversation_id': CONVERSATION})
        other = self.fake.add_note(title='Another session')
        for _ in range(60):  # more than one feed page of old replies: none of them is announced
            self.fake.add_reply(ours, strokes=False, message='old')
        process, lines = self.watcher('--conversation', CONVERSATION)
        self.assertTrue(self.wait_until(lambda: any(
            r['query'].get('listen') == 'true' and int(r['query'].get('wait', 0)) > 0 for r in self.fake.feed_requests())),
            (lines, process.stderr.read() if process.poll() is not None else ''))
        self.assertEqual(lines[0], f'LISTENING for replies to the notes of conversation {CONVERSATION}')
        self.fake.add_reply(other, strokes=False, message='not ours')
        self.fake.add_reply(ours, strokes=False, message='Give me a new version', photo=jpeg_header(64, 48),
                            capture='IMG_20261111_170138_123_ab12')
        self.assertTrue(self.wait_until(lambda: any(line.startswith('REPLY') for line in lines)), lines)
        time.sleep(0.3)
        replies = [line for line in lines if line.startswith('REPLY')]
        self.assertEqual(len(replies), 1, lines)
        self.assertIn(f'note={ours}', replies[0])
        self.assertIn("title='New drafter'", replies[0])
        self.assertIn("message='Give me a new version'", replies[0])
        self.assertIn('photo=IMG_20261111_170138_123_ab12', replies[0])
        polls = [r['query'] for r in self.fake.feed_requests() if r['query'].get('listen')]
        self.assertTrue(polls and all(q.get('conversation_id') == CONVERSATION for q in polls), polls)

    def test_watcher_finds_its_conversation_or_listens_for_the_whole_mac(self):
        process, lines = self.watcher()  # no Claude session above it in this test
        self.assertTrue(self.wait_until(lambda: lines), process.stderr.read() if process.poll() is not None else '')
        self.assertTrue(lines[0].startswith('LISTENING for replies to every note this Mac sent'), lines)
        self.assertTrue(self.wait_until(lambda: any(r['query'].get('listen') for r in self.fake.feed_requests())))
        self.assertFalse(any(r['query'].get('conversation_id') for r in self.fake.feed_requests()))

    def test_session_of_ancestors(self):
        sys.path.insert(0, str(WATCHER.parent))
        import conversation_identity
        sessions = self.directory / 'claude' / 'sessions'
        sessions.mkdir(parents=True)
        (sessions / '300.json').write_text(json.dumps({'sessionId': CONVERSATION, 'name': 'Data preprocessing'}))
        (sessions / '200.json').write_text(json.dumps({'sessionId': 'not-a-uuid'}))
        parents = {100: 200, 200: 300, 300: 1}
        with unittest.mock.patch.dict(os.environ, {'CLAUDE_CONFIG_DIR': str(self.directory / 'claude')}):
            found = conversation_identity.claude_session_of_ancestors(100, parent=parents.get)
            self.assertEqual(found['sessionId'], CONVERSATION)
            self.assertIsNone(conversation_identity.claude_session_of_ancestors(100, depth=2, parent=parents.get))
            self.assertIsNone(conversation_identity.claude_session_of_ancestors(400, parent=parents.get))


class WhoAnswersFake:
    """Claims, rules, owners and pages on top of FakeThoughts (protocol "Who answers")."""

    def __init__(self, fake):
        self.fake, self.claims, self.owners, self.pages = fake, {}, {}, []
        self.rules = {'text': '# Answering rules\nAnswer in plain words.\n', 'sha256': 'r1',
                      'source_path': 'docs-notes/agent-note-rules.md', 'revision': 3, 'updated_at': now_iso()}
        self.covering = set()  # item ids this Mac's conversation covers
        original = fake.route

        def route(method, path, query, body):
            if path == '/v1/health':
                return 200, {'capabilities': ['agent_notes_v1', 'agent_note_claims_v1', 'agent_note_owners_v1',
                                              'agent_note_rules_v1']}, 'application/json'
            if path == '/v1/agent-notes/rules':
                if self.rules is None:
                    return 404, {'detail': 'none', 'error_code': 'agent_note_not_found'}, 'application/json'
                return 200, {**self.rules, 'others': []}, 'application/json'
            claim = re.fullmatch(r'/v1/agent-notes/handling/([0-9a-f-]{36})/claim', path)
            if claim and method == 'POST':
                item = claim.group(1)
                held = self.claims.get(item)
                me = {'device_id': 'thoughts-mac', 'conversation_id': body.get('conversation_id'), 'role': 'sender',
                      'claimed_at': now_iso(), 'answer_note_id': None}
                if held and held['conversation_id'] != body.get('conversation_id'):
                    return 409, {'claimed': False, 'error_code': 'agent_note_claimed', 'claim': held}, 'application/json'
                if not held and item not in self.covering:
                    return 409, {'claimed': False, 'error_code': 'agent_note_not_covering', 'claim': None}, 'application/json'
                self.claims[item] = held or me
                return (200 if held else 201), {'claimed': True, 'claim': self.claims[item]}, 'application/json'
            if path == '/v1/agent-notes/owners' and method == 'POST':
                owner = {'owner_id': 'a' * 24, 'live': True, **body}
                self.owners[owner['owner_id']] = owner
                return 201, owner, 'application/json'
            if path == '/v1/agent-notes/owners' and method == 'GET':
                return 200, {'owners': list(self.owners.values())}, 'application/json'
            if path == '/v1/agent-notes/pages':
                if 'after' not in query:
                    return 200, {'notes': [], 'cursor': 0}, 'application/json'
                found = [n for n in self.pages if n['sequence'] > int(query['after'])
                         and (not query.get('conversation_id') or n.get('owned_by') == query['conversation_id'])]
                if not found:
                    time.sleep(0.2)
                return 200, {'notes': found, 'cursor': max([n['sequence'] for n in found] + [int(query['after'])])}, \
                    'application/json'
            return original(method, path, query, body)

        fake.route = route


class WhoAnswersTest(FakeServerCase):
    def setUp(self):
        super().setUp()
        self.extra = WhoAnswersFake(self.fake)

    def test_a_covered_reply_is_claimed_for_this_conversation_with_the_rules(self):
        note = self.fake.add_note(title='Group mean', origin={'agent': 'mcp', 'conversation_id': CONVERSATION})
        reply = self.fake.add_reply(note, strokes=False, message='Why 3?')
        self.extra.covering.add(reply['reply_id'])
        handling = agent_notes.claim_item(reply['reply_id'], CONVERSATION, client=self.client)
        self.assertEqual(handling['state'], 'yours')
        self.assertIn('answering rules', handling['next_step'])
        self.assertEqual(agent_notes.claim_item(reply['reply_id'], CONVERSATION, client=self.client)['state'], 'yours')
        rules = agent_notes.answering_rules(self.client)
        self.assertEqual(rules['text'], '# Answering rules\nAnswer in plain words.\n')
        self.assertEqual(rules['source_path'], 'docs-notes/agent-note-rules.md')

    def test_a_reply_another_agent_has_or_nobody_here_covers_is_left_alone(self):
        note = self.fake.add_note(title='Group mean')
        taken = self.fake.add_reply(note, strokes=False)
        self.extra.claims[taken['reply_id']] = {'device_id': 'server', 'conversation_id': 'x', 'role': 'responder',
                                                'claimed_at': now_iso(), 'answer_note_id': None}
        handling = agent_notes.claim_item(taken['reply_id'], CONVERSATION, client=self.client)
        self.assertEqual(handling['state'], 'taken')
        self.assertIn('Thoughts reply agent answers it', handling['next_step'])
        other = self.fake.add_reply(note, strokes=False)
        handling = agent_notes.claim_item(other['reply_id'], CONVERSATION, client=self.client)
        self.assertEqual(handling['state'], 'not_yours')
        self.assertNotIn(other['reply_id'], self.extra.claims)

    def test_missing_rules_and_older_servers_are_said(self):
        self.extra.rules = None
        self.assertIn('agent-note-rules.md', agent_notes.answering_rules(self.client)['missing'])
        older = agent_notes.Client(agent_notes.Config(self.fake.url, TOKEN))
        older._capabilities = (time.monotonic(), set())
        self.assertEqual(agent_notes.claim_item(str(uuid.uuid4()), CONVERSATION, client=older)['state'], 'unknown')
        self.assertIn('does not serve', agent_notes.answering_rules(older)['missing'])

    def test_an_existing_reply_to_another_devices_note_comes_from_the_note(self):
        note = self.fake.add_note(title='[Reading group] Variant B', sender='server')
        reply = self.fake.add_reply(note, strokes=False, message='Redo it')
        entry = agent_notes.wait_for_reply(note, 0, client=self.client, root=self.root)
        self.assertEqual(entry['reply_id'], reply['reply_id'])
        self.assertEqual(entry['message'], 'Redo it')
        self.assertTrue(Path(entry['image_path']).is_file())

    def test_own_and_release_a_topic(self):
        owner = agent_notes.own_topic(CONVERSATION, 'Reading group', title_tags=['[Reading group]'], captures=True,
                                      client=self.client)
        self.assertEqual(owner['owner_id'], 'a' * 24)
        posted = [r for r in self.fake.requests if r['path'] == '/v1/agent-notes/owners' and r['method'] == 'POST']
        self.assertEqual(posted[0]['body'], {'conversation_id': CONVERSATION, 'label': 'Reading group',
                                             'document_ids': [], 'review_card_ids': [],
                                             'title_tags': ['[Reading group]'], 'captures': True})

    def test_the_watcher_prints_the_rules_and_only_its_topics_pages(self):
        process, lines = ReplyListeningTest.watcher(self, '--conversation', CONVERSATION)
        self.assertTrue(ReplyListeningTest.wait_until(self, lambda: any(
            r['path'] == '/v1/agent-notes/pages' and r['query'].get('conversation_id') == CONVERSATION
            for r in self.fake.requests)), lines)
        self.assertIn('RULES| Answer in plain words.', lines)
        self.extra.pages.append({'note_id': str(uuid.uuid4()), 'sequence': 5, 'title': 'Not mine', 'origin': {},
                                 'owned_by': 'another-conversation'})
        mine = str(uuid.uuid4())
        self.extra.pages.append({'note_id': mine, 'sequence': 6, 'title': '[Reading group] page 2',
                                 'origin': {'page': 2}, 'message': 'What next?', 'owned_by': CONVERSATION})
        self.assertTrue(ReplyListeningTest.wait_until(self, lambda: any(line.startswith('PAGE') for line in lines)), lines)
        pages = [line for line in lines if line.startswith('PAGE')]
        self.assertEqual(len(pages), 1, lines)
        self.assertIn(f'note={mine}', pages[0])


# ---------------------------------------------------------------- one folder sync for the whole Mac

HERE = Path(__file__).resolve().parent
# The commit before the folder sync had one holder: the code that sessions started earlier keep running.
BASE_COMMIT = '32b727779'
SYNCED_NOTE = '0190a000-0000-7000-8000-00000000000a'
# A process that does what the MCP server's background thread does (agent_notes.watch_loop), and nothing else.
SYNC_WORKER = '''
import json, sys, threading
from pathlib import Path
settings = json.loads(sys.argv[1])
sys.path.insert(0, settings['modules'])
import agent_notes
client = agent_notes.Client(agent_notes.Config(settings['url'], settings['token']))
agent_notes.watch_loop(threading.Event(), client_factory=lambda: client, root=Path(settings['root']), **settings['options'])
'''


def base_code(directory):
    """agent_notes.py, server.py, conversation_identity.py and thoughts-watch-replies as of BASE_COMMIT, written
    into @p directory/base; None without that history (a clone without it)."""
    target = Path(directory) / 'base'
    target.mkdir(exist_ok=True)
    for name in ('agent_notes.py', 'server.py', 'conversation_identity.py', 'thoughts-watch-replies'):
        try:
            done = subprocess.run(['git', '-C', str(HERE), 'show', f'{BASE_COMMIT}:./{name}'], capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if done.returncode != 0 or not done.stdout:
            return None
        (target / name).write_bytes(done.stdout)
    return target


def wait_until(condition, seconds=10.0):
    deadline = time.monotonic() + seconds
    while not condition() and time.monotonic() < deadline:
        time.sleep(0.05)
    return condition()


async def until(condition, seconds=10.0):
    deadline = time.monotonic() + seconds
    while not condition() and time.monotonic() < deadline:
        await asyncio.sleep(0.05)
    return condition()


def holder_pid(root):
    """The process ID the folder's sync holder wrote into .sync.lock, or None."""
    try:
        return json.loads((Path(root) / agent_notes.SYNC_LOCK).read_text(encoding='utf-8'))['pid']
    except (OSError, ValueError, KeyError, TypeError):
        return None


def read_lines(stream):
    """The lines a child process prints, collected by a thread as they come."""
    lines = []

    def read():
        for line in stream:
            lines.append(line.rstrip('\n'))
    threading.Thread(target=read, daemon=True).start()
    return lines


def child_pids():
    """This test process's own child processes."""
    done = subprocess.run(['pgrep', '-P', str(os.getpid())], capture_output=True, text=True, timeout=30)
    return {int(pid) for pid in done.stdout.split()}


def alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def end_process(process):
    if process.poll() is None:
        process.kill()
    process.wait(10)
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream:
            stream.close()


def sync_workers(test, fake, root, count, modules=HERE, **options):
    """@p count worker processes on one notes folder, each running watch_loop like an MCP server's thread."""
    settings = json.dumps(dict(modules=str(modules), url=fake.url, token=TOKEN, root=str(root), options=options))
    processes = [subprocess.Popen([sys.executable, '-c', SYNC_WORKER, settings], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for _ in range(count)]
    for process in processes:
        test.addCleanup(end_process, process)
    return processes


def sent_note(fake, root, title='Group mean', directory=None):
    """A note sent from this Mac a moment ago: it keeps the folder sync running (a note < 24 h old)."""
    picture = Path(directory or root.parent) / f'picture-{uuid.uuid4().hex[:8]}.png'
    picture.write_bytes(png())
    return agent_notes.send_note(title, image_path=str(picture), client=agent_notes.Client(agent_notes.Config(fake.url, TOKEN)),
                                 root=root)


def check_folder(test, fake, root, expected):
    """Every reply of @p expected on disk exactly once, complete, numbered per note in sequence order, journaled once."""
    found = {}
    for path in root.glob('*/reply-*.json'):
        if re.fullmatch(r'reply-\d+\.json', path.name):
            record = json.loads(path.read_text(encoding='utf-8'))
            test.assertNotIn(record['reply_id'], found, f'{path} repeats a reply')
            found[record['reply_id']] = (path, record)
    test.assertEqual(set(found), {r['reply_id'] for r in expected})
    for reply in expected:
        path, record = found[reply['reply_id']]
        image = path.parent / record['local']['image']
        test.assertEqual(image.read_bytes(), fake.blobs[reply['image_url']][0], image)
    for folder in {path.parent for path, _ in found.values()}:
        numbered = sorted((int(re.fullmatch(r'reply-(\d+)\.json', p.name).group(1)), json.loads(p.read_text())['sequence'])
                          for p in folder.glob('reply-*.json') if re.fullmatch(r'reply-\d+\.json', p.name))
        test.assertEqual([n for n, _ in numbered], list(range(1, len(numbered) + 1)), folder)
        test.assertEqual([s for _, s in numbered], sorted(s for _, s in numbered), f'{folder}: numbered in sequence order')
    journal = [json.loads(line) for line in (root / 'replies.jsonl').read_text(encoding='utf-8').splitlines()]
    test.assertEqual(sorted(j['reply_id'] for j in journal), sorted(r['reply_id'] for r in expected))
    test.assertEqual(agent_notes.read_cursor(root), max(r['sequence'] for r in expected))
    test.assertEqual(list(root.rglob('*.tmp')), [])


class FolderSyncTest(FakeServerCase):
    """One process on the Mac keeps the notes folder in step with the server; the others wait for its lock."""

    def watch(self, **options):
        stop = threading.Event()
        thread = threading.Thread(target=agent_notes.watch_loop, args=(stop,), daemon=True,
                                  kwargs=dict(client_factory=lambda: self.client, root=self.root, **options))
        thread.start()
        self.addCleanup(lambda: (stop.set(), thread.join(10)))
        return stop, thread

    def lists(self):
        return [r for r in self.fake.sync_requests() if r['path'] == '/v1/agent-notes']

    def long_polls(self):
        return [r for r in self.fake.sync_requests() if r['path'] == '/v1/agent-notes/replies']

    def test_the_holder_asks_whether_anything_changed_and_pauses_longer_while_nothing_does(self):
        note = self.send()
        self.watch(wait=1, pause_min=0.1, pause_max=0.8, tick=0.02)
        # The first look pulls the feed (one long poll); after that, while nothing changes, only the
        # note list is asked, with If-None-Match (304), and the pauses double up to pause_max.
        self.assertTrue(wait_until(lambda: len(self.lists()) >= 7, 8), self.lists())
        lists = self.lists()
        self.assertIsNone(lists[0]['if_none_match'])
        self.assertTrue(all(r['if_none_match'] for r in lists[1:]))
        self.assertEqual(lists[0]['query'], {'limit': '1'})
        self.assertEqual([r['query'] for r in self.long_polls()], [{'after': '0', 'wait': '1'}])
        gaps = [b['at'] - a['at'] for a, b in zip(lists[1:], lists[2:])]   # nominally 0.2, 0.4, 0.8, 0.8, ...
        self.assertLess(max(gaps[:2]), min(gaps[2:]), gaps)
        self.assertGreater(min(gaps[2:]), 0.7, gaps)
        self.assertLess(max(gaps), 1.5, gaps)
        # A reply is a change: it lands in the folder at the next look, and the next looks come quickly again.
        added_at = time.monotonic()
        reply = self.fake.add_reply(note['note_id'], message='Here')
        landed = Path(note['folder']) / 'reply-1.png'
        self.assertTrue(wait_until(landed.exists, 3))
        self.assertTrue(wait_until(lambda: len([r for r in self.lists() if r['at'] > added_at]) >= 2, 3))
        after = [r for r in self.lists() if r['at'] > added_at]   # the look that saw the reply, and the next
        self.assertLess(after[1]['at'] - after[0]['at'], 0.6, 'the pause starts short again after a change')
        self.assertEqual(json.loads((Path(note['folder']) / 'reply-1.json').read_text())['reply_id'], reply['reply_id'])
        # A note sent from this Mac (the folder changes) ends a pause at once: a reply may follow.
        time.sleep(2.5)
        sent_at = time.monotonic()
        self.send(title='Second')
        self.assertTrue(wait_until(lambda: any(r['at'] > sent_at for r in self.long_polls()), 3))
        self.assertLess(min(r['at'] for r in self.lists() if r['at'] > sent_at) - sent_at, 0.5)
        # A folder sync never says an agent listens.
        self.assertFalse(any(r['query'].get('listen') for r in self.fake.feed_requests()))

    def test_a_server_without_list_etags_is_long_polled_with_growing_pauses(self):
        self.fake.etags = False
        note = self.send()
        self.watch(wait=1, pause_min=0.1, pause_max=0.4, tick=0.02)
        self.assertTrue(wait_until(lambda: len(self.long_polls()) >= 4, 10), self.long_polls())
        self.assertEqual(len(self.lists()), 1, 'without an ETag the list is not asked again')
        starts = [r['at'] for r in self.long_polls()]
        self.assertGreater(starts[3] - starts[2], 1.3)   # 1 s long poll + a pause grown to 0.4 s
        self.fake.add_reply(note['note_id'])
        self.assertTrue(wait_until((Path(note['folder']) / 'reply-1.png').exists, 3))

    def test_the_list_failing_does_not_stop_the_folder_sync(self):
        note = self.send()
        self.fake.fail['/v1/agent-notes'] = (503, b'{"detail": "busy"}')
        self.watch(wait=1, pause_min=0.1, pause_max=0.4, tick=0.02)
        self.fake.add_reply(note['note_id'])
        self.assertTrue(wait_until((Path(note['folder']) / 'reply-1.png').exists, 4))

    def test_a_change_is_pulled_even_when_its_first_pull_failed(self):
        note = self.send()
        self.fake.add_reply(note['note_id'])
        self.fake.fail['/v1/agent-notes/replies'] = (503, b'{"detail": "busy"}')
        self.watch(wait=1, pause_min=0.1, pause_max=0.4, tick=0.02)
        # The list said "changed", the feed failed: the next look must not take the list's 304 for "nothing new".
        self.assertTrue(wait_until((Path(note['folder']) / 'reply-1.png').exists, 6))

    def test_after_replies_the_feed_is_pulled_once_more(self):
        note = self.send()
        self.watch(wait=1, pause_min=0.1, pause_max=0.4, tick=0.02)
        self.assertTrue(wait_until(lambda: len(self.lists()) >= 3, 5))
        self.fake.add_reply(note['note_id'])
        self.assertTrue(wait_until((Path(note['folder']) / 'reply-1.png').exists, 3))
        pulled = len(self.long_polls())
        # A second reply right after the first is caught by a long poll, not left for a later look.
        self.assertTrue(wait_until(lambda: len(self.long_polls()) > pulled, 3))
        self.fake.add_reply(note['note_id'])
        self.assertTrue(wait_until((Path(note['folder']) / 'reply-2.png').exists, 2))

    def test_five_processes_send_the_requests_of_one_and_a_killed_holder_hands_over(self):
        options = dict(wait=2, pause_min=0.2, pause_max=2.0, takeover=0.2, tick=0.05)
        alone = FakeThoughts()
        self.addCleanup(alone.close)
        alone_root = self.directory / 'alone' / 'agent-notes'
        sent_note(alone, alone_root, directory=self.directory)
        note = self.send()
        sync_workers(self, alone, alone_root, 1, **options)
        workers = sync_workers(self, self.fake, self.root, 5, **options)
        self.assertTrue(wait_until(lambda: alone.sync_requests() and self.fake.sync_requests(), 10))
        time.sleep(6)
        one, five = len(alone.sync_requests()), len(self.fake.sync_requests())
        self.assertGreaterEqual(one, 4)
        self.assertLessEqual(abs(five - one), 2, (one, five))
        self.assertEqual(self.fake.most_held, 1, 'one folder-sync long poll at a time')
        holder = holder_pid(self.root)
        self.assertIn(holder, [w.pid for w in workers])
        # The holder dies: another process takes the lock within a few seconds and syncs.
        next(w for w in workers if w.pid == holder).kill()
        self.assertTrue(wait_until(lambda: holder_pid(self.root) not in (None, holder) and alive(holder_pid(self.root)), 5),
                        holder_pid(self.root))
        self.assertIn(holder_pid(self.root), [w.pid for w in workers])
        reply = self.fake.add_reply(note['note_id'])
        landed = Path(note['folder']) / 'reply-1.png'
        self.assertTrue(wait_until(landed.exists, 5))
        check_folder(self, self.fake, self.root, [reply])

    def test_watch_prints_the_replies_another_process_writes(self):
        note = self.send()
        worker = sync_workers(self, self.fake, self.root, 1)[0]
        self.assertTrue(wait_until(lambda: holder_pid(self.root) == worker.pid, 10))
        environment = {**os.environ, 'THOUGHTS_SERVER_URL': self.fake.url, 'THOUGHTS_TOKEN': TOKEN,
                       'THOUGHTS_AGENT_NOTES_DIR': str(self.root)}
        process = subprocess.Popen([sys.executable, str(HERE / 'agent_notes.py'), 'watch'], stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, text=True, env=environment)
        self.addCleanup(end_process, process)
        lines = read_lines(process.stdout)
        time.sleep(1.5)
        before = len(self.fake.sync_requests())
        reply = self.fake.add_reply(note['note_id'], message='Printed')
        self.assertTrue(wait_until(lambda: lines, 5))
        self.assertEqual(json.loads(lines[0])['reply_id'], reply['reply_id'])
        self.assertTrue(Path(json.loads(lines[0])['image_path']).is_file())
        self.assertEqual(holder_pid(self.root), worker.pid, 'watch did not take over a running holder')
        self.assertLessEqual(len(self.fake.sync_requests()) - before, 2, 'watch sent no requests of its own')

    def test_the_newest_note_is_read_again_when_the_folder_changes(self):
        (self.root / '20261001-120000-old-0190a000').mkdir(parents=True)
        note_json = self.root / '20261001-120000-old-0190a000' / 'note.json'
        note_json.write_text(json.dumps({'note_id': SYNCED_NOTE, 'created_at': now_iso(-3 * 86400)}))
        settled = time.time() - 30
        os.utime(self.root, (settled, settled))
        self.assertFalse(agent_notes.has_recent_note(self.root))
        # A note.json changed in place does not change the folder: the answer kept in memory stands...
        note_json.write_text(json.dumps({'note_id': SYNCED_NOTE, 'created_at': now_iso()}))
        os.utime(self.root, (settled, settled))
        self.assertFalse(agent_notes.has_recent_note(self.root))
        # ...and any note folder added or removed (the folder's modification time) makes it read again.
        (self.root / '20261114-090000-new-0190a000').mkdir()
        self.assertTrue(agent_notes.has_recent_note(self.root))


class ListeningAsBeforeTest(FakeServerCase):
    """Listening polls (listen=true) are per conversation, not the folder sync: the same requests as the base code."""

    def setUp(self):
        super().setUp()
        self.base = base_code(self.directory)
        if self.base is None:
            self.skipTest(f'needs the history of commit {BASE_COMMIT}')

    def base_module(self):
        spec = importlib.util.spec_from_file_location('agent_notes_base', self.base / 'agent_notes.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module   # dataclasses look their module up there
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(module)
        return module

    def test_waiting_for_a_reply_polls_as_the_base_code_does(self):
        lines = {}
        for name, module in (('base', self.base_module()), ('new', agent_notes)):
            fake = FakeThoughts()
            self.addCleanup(fake.close)
            fake.add_note('Group mean', created_at=now_iso(), note_id=SYNCED_NOTE)
            client, root = module.Client(module.Config(fake.url, TOKEN)), self.directory / name
            self.assertIsNone(module.wait_for_reply(SYNCED_NOTE, 1, client=client, root=root))
            timer = threading.Timer(0.5, fake.add_reply, args=(SYNCED_NOTE,))
            timer.start()
            self.addCleanup(timer.cancel)
            self.assertIsNotNone(module.wait_for_reply(SYNCED_NOTE, 5, client=client, root=root))
            self.assertIsNotNone(module.wait_for_reply(SYNCED_NOTE, 0, client=client, root=root))
            lines[name] = [(r['method'], r['path'], sorted(r['query'].items())) for r in fake.requests
                           if r['path'] in ('/v1/agent-notes/replies', f'/v1/agent-notes/{SYNCED_NOTE}')]
        self.assertEqual(lines['new'], lines['base'])
        self.assertIn(('GET', '/v1/agent-notes/replies', [('after', '0'), ('listen', 'true'), ('note_id', SYNCED_NOTE), ('wait', '1')]),
                      lines['new'])

    def test_the_watcher_listens_as_the_base_code_does(self):
        polls = {}
        for name, script in (('base', self.base / 'thoughts-watch-replies'), ('new', HERE / 'thoughts-watch-replies')):
            fake = FakeThoughts()
            self.addCleanup(fake.close)
            WhoAnswersFake(fake)
            ours = fake.add_note('New drafter', origin={'agent': 'mcp', 'conversation_id': CONVERSATION}, note_id=SYNCED_NOTE)
            for _ in range(3):
                fake.add_reply(ours, strokes=False)
            environment = {**os.environ, 'THOUGHTS_SERVER_URL': fake.url, 'THOUGHTS_TOKEN': TOKEN,
                           'THOUGHTS_AGENT_NOTES_DIR': str(self.directory / name), 'CLAUDE_CONFIG_DIR': str(self.directory / 'claude')}
            process = subprocess.Popen([sys.executable, str(script), '--conversation', CONVERSATION], stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, text=True, env=environment)
            self.addCleanup(end_process, process)
            lines = read_lines(process.stdout)

            def listening(count):
                return len([r for r in fake.feed_requests() if r['query'].get('listen')]) >= count
            self.assertTrue(wait_until(lambda: listening(1), 10), lines)
            fake.add_reply(ours, strokes=False, message='Again')
            self.assertTrue(wait_until(lambda: any(line.startswith('REPLY') for line in lines), 10), lines)
            self.assertTrue(wait_until(lambda: listening(2), 10), lines)
            end_process(process)
            polls[name] = [(r['method'], r['path'], sorted(r['query'].items())) for r in fake.requests if r['query'].get('listen')]
        self.assertEqual(polls['new'], polls['base'])
        self.assertEqual(polls['new'][0], ('GET', '/v1/agent-notes/replies', [
            ('after', '3'), ('conversation_id', CONVERSATION), ('listen', 'true'), ('wait', '50')]))


class ToolProcessesTest(unittest.IsolatedAsyncioTestCase):
    """Several MCP servers (one per agent session) on one notes folder against one counting fake server."""

    async def asyncSetUp(self):
        self.fake = FakeThoughts()
        self.addCleanup(self.fake.close)
        self.directory = Path(tempfile.mkdtemp(prefix='thoughts-tool-processes-'))
        self.addCleanup(shutil.rmtree, self.directory, True)
        self.root = self.directory / 'agent-notes'
        home = self.directory / 'home'
        home.mkdir()
        self.env = {**os.environ, 'HOME': str(home), 'CFFIXED_USER_HOME': str(home), 'CLAUDE_CONFIG_DIR': str(home / '.claude'),
                    'CODEX_HOME': str(home / '.codex'), 'THOUGHTS_SERVER_URL': self.fake.url, 'THOUGHTS_TOKEN': TOKEN,
                    'THOUGHTS_AGENT_NOTES_DIR': str(self.root), 'THOUGHTS_CHROME': str(self.directory / 'no-chrome'),
                    'THOUGHTSCTL': '/nonexistent/thoughtsctl'}
        self.errors = open(self.directory / 'servers.log', 'w')
        self.addCleanup(self.errors.close)

    async def tool_process(self, stack, server=HERE / 'server.py'):
        """An MCP server as an agent session starts it, and its process ID (a child of this process)."""
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        params = StdioServerParameters(command=sys.executable, args=[str(server)], env=self.env)
        before = await asyncio.to_thread(child_pids)
        read, write = await stack.enter_async_context(stdio_client(params, errlog=self.errors))
        session = await stack.enter_async_context(ClientSession(read, write))
        await session.initialize()
        started = await asyncio.to_thread(child_pids) - before
        self.assertEqual(len(started), 1, started)
        return session, started.pop()

    async def test_one_holder_every_process_sees_a_reply_and_a_killed_holder_hands_over(self):
        note = sent_note(self.fake, self.root)
        async with contextlib.AsyncExitStack() as stack:
            processes = dict([await self.tool_process(stack) for _ in range(4)])   # session -> process ID
            sessions = list(processes)
            self.assertTrue(await until(lambda: holder_pid(self.root) in processes.values(), 10), holder_pid(self.root))
            first = holder_pid(self.root)
            await asyncio.sleep(2)   # every process has tried the lock by now
            self.assertEqual(self.fake.most_held, 1, 'one folder-sync long poll for four processes')
            self.assertEqual(len([r for r in self.fake.sync_requests() if r['path'] == '/v1/agent-notes']), 1)

            # A reply lands in the folder within a few seconds, once, and every process's tools show it.
            reply = self.fake.add_reply(note['note_id'], message='See the circle')
            landed = Path(note['folder']) / 'reply-1.png'
            self.assertTrue(await until(landed.exists, 5))
            for session in sessions:
                listed = await session.call_tool('list_note_replies', {})
                self.assertEqual([r['reply_id'] for r in listed.structuredContent['replies']], [reply['reply_id']])
                got = await session.call_tool('get_note_reply', {'note_id': note['note_id']})
                self.assertEqual((got.structuredContent['status'], got.structuredContent['reply_id']), ('replied', reply['reply_id']))
            check_folder(self, self.fake, self.root, [reply])

            # The holder dies: another process takes over within a few seconds, and replies keep landing.
            os.kill(first, signal.SIGKILL)   # one of this test's own servers (it holds this test's folder)
            living = [s for s in sessions if processes[s] != first]
            self.assertTrue(await until(lambda: holder_pid(self.root) in [processes[s] for s in living], 5), holder_pid(self.root))
            second = self.fake.add_reply(note['note_id'], message='And this')
            self.assertTrue(await until((Path(note['folder']) / 'reply-2.png').exists, 5))
            check_folder(self, self.fake, self.root, [reply, second])

            # Each listening conversation sends its own listening polls, as before.
            self.assertEqual(len(living), 3)
            notes = [sent_note(self.fake, self.root, title=f'Question {n}')['note_id'] for n in range(len(living))]
            waited = await asyncio.gather(*(s.call_tool('get_note_reply', {'note_id': n, 'wait_seconds': 2})
                                            for s, n in zip(living, notes)))
            self.assertEqual([w.structuredContent['status'] for w in waited], ['no_reply_yet'] * 3)
            for note_id in notes:
                polls = [r['query'] for r in self.fake.feed_requests() if r['query'].get('note_id') == note_id and r['query'].get('listen')]
                self.assertEqual(polls, [{'after': '0', 'wait': '2', 'note_id': note_id, 'listen': 'true'}])

    async def test_an_old_copy_beside_new_ones_loses_nothing_and_corrupts_nothing(self):
        base = await asyncio.to_thread(base_code, self.directory)
        if base is None:
            self.skipTest(f'needs the history of commit {BASE_COMMIT}')
        here = sent_note(self.fake, self.root)
        elsewhere = [self.fake.add_note(f'Sent elsewhere {n}') for n in range(2)]   # not in the folder yet
        async with contextlib.AsyncExitStack() as stack:
            old = (await self.tool_process(stack, base / 'server.py'))[0]
            new = [(await self.tool_process(stack))[0] for _ in range(2)]
            self.assertTrue(await until(lambda: alive(holder_pid(self.root)), 10))
            await asyncio.sleep(1)
            expected = []
            for round_ in range(4):
                for note_id in [here['note_id'], *elsewhere]:
                    expected.append(self.fake.add_reply(note_id, message=f'Round {round_}',
                                                        photo=jpeg_header(64, 48) if round_ == 2 else None, capture='IMG_0001'))
                await asyncio.sleep(0.3)
            self.assertTrue(await until(lambda: len(list(self.root.glob('*/reply-*.png'))) == len(expected), 20),
                            sorted(p.name for p in self.root.glob('*/reply-*')))
            await asyncio.sleep(1.5)   # a late or second write would show now
            check_folder(self, self.fake, self.root, expected)
            for session in [old, *new]:
                listed = await session.call_tool('list_note_replies', {})
                self.assertEqual([r['reply_id'] for r in listed.structuredContent['replies']], [r['reply_id'] for r in expected])
            self.assertLessEqual(self.fake.most_held, 2, 'the old copy\'s long poll and the holder\'s')
            # Both kinds polled the feed while the replies came: the old copy (25 s) and the holder (50 s).
            waits = {r['query'].get('wait') for r in self.fake.sync_requests() if r['path'] == '/v1/agent-notes/replies'}
            self.assertLessEqual({'25', '50'}, waits)


@unittest.skipUnless(os.environ.get('THOUGHTS_TEST_LOAD'), 'Set THOUGHTS_TEST_LOAD=1 to measure two minutes of folder sync')
class FolderSyncLoadTest(unittest.TestCase):
    """Requests of the folder sync per minute, for 1, 5 and 20 processes, base code against this code, with the
    defaults the MCP servers use (no tool calls, nothing changing)."""

    def test_requests_per_minute(self):
        directory = Path(tempfile.mkdtemp(prefix='thoughts-sync-load-'))
        self.addCleanup(shutil.rmtree, directory, True)
        base = base_code(directory)
        if base is None:
            self.skipTest(f'needs the history of commit {BASE_COMMIT}')
        rows = []
        for count in (1, 5, 20):
            runs = {}
            for name, modules in (('base', base), ('new', HERE)):
                fake = FakeThoughts()
                self.addCleanup(fake.close)
                root = directory / f'{name}-{count}' / 'agent-notes'
                sent_note(fake, root, directory=directory)
                started = time.monotonic()
                runs[name] = (fake, started, sync_workers(self, fake, root, count, modules))
            time.sleep(120)
            row = [count]
            for name, (fake, started, processes) in runs.items():
                for process in processes:
                    end_process(process)
                times = [r['at'] - started for r in fake.sync_requests()]
                row += [len([t for t in times if t < 60]), len([t for t in times if 60 <= t < 120]), fake.most_held]
            rows.append(row)
            print(f'{count:>3} processes: base {row[1]:>3} + {row[2]:>3} requests (minute 1 + 2), {row[3]:>2} long polls held; '
                  f'new {row[4]:>3} + {row[5]:>3}, {row[6]:>2} held', file=sys.stderr, flush=True)
        for row in rows:
            self.assertEqual(row[6], 1)
            self.assertLessEqual(abs(row[4] - rows[0][4]) + abs(row[5] - rows[0][5]), 2, rows)


def stub_assets(name):
    """Stand-ins for the Markdown page's marked and KaTeX files, so no test downloads them."""
    if not agent_notes.ASSET_NAME.fullmatch(name):
        return None
    if name.endswith('marked.min.js'):
        return b"window.marked={parse:function(s){return '<p class=\"from-marked\">'+s+'</p>';}};", 'text/javascript; charset=utf-8'
    if name.endswith('auto-render.min.js'):
        return (b"window.renderMathInElement=function(e){e.setAttribute('data-math','typeset');"
                b"document.documentElement.setAttribute('data-thoughts-warning','stub KaTeX ran');};", 'text/javascript; charset=utf-8')
    if name.endswith('.css'):
        return b'.from-marked{color:#008000}', 'text/css; charset=utf-8'
    return b'/* stub */', agent_notes.ASSET_TYPES[Path(name).suffix]


class RenderTest(unittest.TestCase):
    def setUp(self):
        self.chrome = Path(tempfile.mkdtemp(prefix='thoughts-chrome-stub-')) / 'Google Chrome'
        self.chrome.write_text('#!/bin/sh\nexit 0\n')
        self.chrome.chmod(0o755)
        self.addCleanup(lambda: __import__('shutil').rmtree(self.chrome.parent, True))
        # No test may download: the CDN is reached only by test_real_chrome, which asks for it.
        self.fetches = []

        def no_network(url):
            self.fetches.append(url)
            raise OSError('tests do not download')
        patcher = unittest.mock.patch.object(agent_notes, 'fetch_asset', no_network)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.assets = Path(tempfile.mkdtemp(prefix='thoughts-assets-'))
        self.addCleanup(lambda: __import__('shutil').rmtree(self.assets, True))
        environment = unittest.mock.patch.dict(os.environ, {'THOUGHTS_AGENT_NOTES_ASSETS': str(self.assets)})
        environment.start()
        self.addCleanup(environment.stop)

    def fake_runner(self, height, extra_attributes='', code=0):
        """Stands in for Chrome: asks the render proxy for the page, as Chrome would, and answers like Chrome."""
        calls = []

        def runner(argv, timeout):
            proxy = next(a.split('=', 1)[1] for a in argv if a.startswith('--proxy-server='))
            status, headers, body = through_proxy(proxy, argv[-1])
            calls.append(dict(argv=argv, page=body.decode('utf-8'), status=status, headers=headers, proxy=proxy, timeout=timeout))
            shot = next((a.split('=', 1)[1] for a in argv if a.startswith('--screenshot=')), None)
            if shot is None:
                return code, f'<html lang="en" data-thoughts-height="{height}" data-thoughts-width="480"{extra_attributes}><body></body></html>'.encode(), b''
            size = next(a for a in argv if a.startswith('--window-size=')).split('=')[1].split(',')
            Path(shot).write_bytes(png(int(int(size[0]) * 2.5), int(int(size[1]) * 2.5)))
            return 0, b'', b''
        return runner, calls

    def test_command_line(self):
        argv = agent_notes.chrome_command('/x/Chrome', Path('/tmp/profile'), 'file:///tmp/p.html', 700, screenshot=Path('/tmp/s.png'),
                                          platform='darwin')
        self.assertEqual(argv[:4], ['/usr/bin/nice', '-n', '10', '/x/Chrome'])
        for flag in ('--headless=new', '--user-data-dir=/tmp/profile', '--hide-scrollbars', '--force-device-scale-factor=2.5',
                     '--window-size=480,700', '--screenshot=/tmp/s.png', '--use-mock-keychain'):
            self.assertIn(flag, argv)
        self.assertTrue(any(a.startswith('--virtual-time-budget=') for a in argv))
        self.assertEqual(argv[-1], 'file:///tmp/p.html')
        dump = agent_notes.chrome_command('/x/Chrome', Path('/tmp/profile'), 'file:///tmp/p.html', 100, platform='darwin')
        self.assertIn('--dump-dom', dump)
        self.assertFalse(any(a.startswith('--screenshot') for a in dump))
        # Chrome has no network: every request goes to the render proxy, loopback addresses too,
        # and no name resolves. Without a proxy it is pointed at a closed port.
        proxied = agent_notes.chrome_command('/x/Chrome', Path('/tmp/profile'), 'http://thoughts-note.invalid/x/page.html', 100,
                                             proxy='127.0.0.1:4321', platform='darwin')
        for flag in ('--proxy-server=http://127.0.0.1:4321', '--proxy-bypass-list=<-loopback>',
                     '--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1', '--disable-background-networking'):
            self.assertIn(flag, proxied)
        self.assertIn('--proxy-server=http://127.0.0.1:9', dump)
        self.assertIn('--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1', dump)
        self.assertFalse(any('allow-file-access' in a or 'disable-web-security' in a for a in proxied))
        self.assertNotIn('--password-store=basic', proxied)

    def test_command_line_on_linux(self):
        """Chromium on Linux: the cookie key stays out of the desktop keyring, and no host rule cuts short notes."""
        proxied = agent_notes.chrome_command('/usr/bin/chromium-browser', Path('/tmp/profile'), 'http://thoughts-note.invalid/x/page.html',
                                             100, screenshot=Path('/tmp/s.png'), proxy='127.0.0.1:4321', platform='linux')
        for flag in ('--proxy-server=http://127.0.0.1:4321', '--proxy-bypass-list=<-loopback>', '--password-store=basic',
                     '--disable-background-networking', '--window-size=480,100', '--screenshot=/tmp/s.png'):
            self.assertIn(flag, proxied)
        self.assertFalse(any(a.startswith(('--host-resolver-rules', '--host-rules')) for a in proxied))
        self.assertEqual(proxied[-1], 'http://thoughts-note.invalid/x/page.html')
        dump = agent_notes.chrome_command('/usr/bin/chromium-browser', Path('/tmp/profile'), 'http://thoughts-note.invalid/x/page.html',
                                          100, platform='linux')
        self.assertIn('--proxy-server=http://127.0.0.1:9', dump)
        self.assertIn('--password-store=basic', dump)
        self.assertEqual(dump[-2:], ['--dump-dom', 'http://thoughts-note.invalid/x/page.html'])
        self.assertFalse(any('allow-file-access' in a or 'disable-web-security' in a for a in proxied))

    def test_render_proxy_keeps_quiet_when_chrome_hangs_up(self):
        """A connection Chrome closes before the answer is no traceback on stderr; other errors still are."""
        with agent_notes.RenderServer('<p>note</p>', 'N0nce') as server:
            printed = []
            with unittest.mock.patch.object(ThreadingHTTPServer, 'handle_error', lambda *args: printed.append(args)):
                for error in (BrokenPipeError(), ConnectionResetError(), ValueError('a bug')):
                    try:
                        raise error
                    except Exception:
                        server._server.handle_error(None, ('127.0.0.1', 1))
            self.assertEqual(len(printed), 1)

    def test_two_passes_with_measured_height_and_clipping(self):
        runner, calls = self.fake_runner(900)
        rendered = agent_notes.render(markdown='# Title\n\n$x^2$', runner=runner, chrome=str(self.chrome))
        self.assertEqual(len(calls), 2)
        self.assertIn('--dump-dom', calls[0]['argv'])
        self.assertIn('--window-size=480,900', calls[1]['argv'])
        profiles = {a for c in calls for a in c['argv'] if a.startswith('--user-data-dir=')}
        self.assertEqual(len(profiles), 2)
        self.assertFalse(any(str(Path.home() / 'Library/Application Support/Google') in p for p in profiles))
        self.assertTrue(all(c['timeout'] == agent_notes.CHROME_TIMEOUT for c in calls))
        self.assertEqual(agent_notes.image_info(rendered.png), ('image/png', 1200, 2250))
        self.assertEqual(rendered.warnings, [])
        page = calls[0]['page']
        for needle in ('marked@12.0.2', 'katex@0.16.11/dist/katex.min.js', 'auto-render.min.js', 'width=480', 'data-thoughts-height'):
            self.assertIn(needle, page)
        self.assertIn('"# Title\\n\\n$x^2$"', page)
        # Both passes got the same page, from an origin that is not a file and exists nowhere.
        self.assertEqual(calls[0]['page'], calls[1]['page'])
        self.assertTrue(all(c['status'] == 200 and c['argv'][-1].startswith('http://thoughts-note.invalid/') for c in calls))
        self.assertFalse(any(a.startswith('file:') for c in calls for a in c['argv']))
        self.assertNotIn('file:', page)
        self.assertNotIn('cdn.jsdelivr.net/npm', page.replace("'marked did not load from cdn.jsdelivr.net (offline?)'", ''))
        # Nothing is left behind: the profiles are removed and the proxy has stopped.
        self.assertFalse(any(Path(p.split('=', 1)[1]).exists() for p in profiles), 'temporary files removed')
        with self.assertRaises(OSError):
            through_proxy(calls[0]['proxy'], calls[0]['argv'][-1])
        self.assertEqual(self.fetches, [], 'the page alone needs no download')
        runner, calls = self.fake_runner(5000)
        rendered = agent_notes.render(html='<p>tall</p>', runner=runner, chrome=str(self.chrome))
        self.assertIn(f'--window-size=480,{agent_notes.MAX_CSS_HEIGHT}', calls[1]['argv'])
        self.assertLessEqual(agent_notes.image_info(rendered.png)[2], 8192)
        self.assertTrue(any('clipped' in w for w in rendered.warnings))

    def test_html_injection(self):
        page = agent_notes.html_page('<!doctype html><html><head><title>x</title></head><body>y</body></html>', 'N0nce')
        self.assertRegex(page, r'<head><meta name="viewport" content="width=480"><script nonce="N0nce">')
        kept = agent_notes.html_page('<html><head><meta name="viewport" content="width=device-width"></head><body></body></html>')
        self.assertEqual(kept.count('name="viewport"'), 1)
        self.assertIn('data-thoughts-height', kept)
        bare = agent_notes.html_page('<p>fragment</p>')
        self.assertTrue(bare.endswith('<p>fragment</p>'))
        # The page gets no base URL, least of all a file one: it has no folder to read from.
        self.assertNotIn('<base', bare)
        self.assertNotIn('file:', bare)
        # Each render has its own nonce, which only the measuring script carries.
        self.assertNotEqual(agent_notes.html_page('<p>x</p>'), agent_notes.html_page('<p>x</p>'))
        agent = agent_notes.html_page('<html><head></head><body><script>alert(1)</script><script nonce="guess">x()</script></body></html>', 'N0nce')
        self.assertEqual(agent.count('nonce="N0nce"'), 1)
        self.assertIn('<script>alert(1)</script>', agent)

    def test_policy_allows_inline_styles_and_data_images_only(self):
        policy = agent_notes.content_security_policy('N0nce')
        directives = dict(part.strip().split(' ', 1) for part in policy.split(';'))
        self.assertEqual(directives['default-src'], "'none'")
        self.assertEqual(directives['style-src'], "'unsafe-inline'")
        self.assertEqual(directives['img-src'], 'data:')
        self.assertEqual(directives['script-src'], "'nonce-N0nce'")
        self.assertEqual(directives['base-uri'], "'none'")
        self.assertEqual(directives['form-action'], "'none'")
        for missing in ('connect-src', 'frame-src', 'font-src', 'media-src', 'object-src'):
            self.assertNotIn(missing, directives)     # default-src 'none' covers them
        for word in ('file:', 'http:', 'https:', '*', "'unsafe-eval'", "'self'"):
            self.assertNotIn(word, policy)
        # A Markdown page adds this program's own style sheet and fonts, by exact prefix.
        assets = 'http://thoughts-note.invalid/abc/assets/'
        markdown = dict(part.strip().split(' ', 1) for part in agent_notes.content_security_policy('N0nce', assets).split(';'))
        self.assertEqual(markdown['style-src'], f"'unsafe-inline' {assets}")
        self.assertEqual(markdown['font-src'], assets)
        self.assertEqual(markdown['script-src'], "'nonce-N0nce'")
        self.assertEqual(markdown['img-src'], 'data:')

    def test_render_proxy_serves_the_page_and_refuses_everything_else(self):
        with agent_notes.RenderServer('<p>note</p>', 'N0nce') as server:
            self.assertRegex(server.url, r'^http://thoughts-note\.invalid/[A-Za-z0-9_-]{16,}/page\.html$')
            self.assertTrue(server.proxy.startswith('127.0.0.1:'))
            status, headers, body = through_proxy(server.proxy, server.url)
            self.assertEqual((status, body), (200, b'<p>note</p>'))
            self.assertEqual(headers['Content-Security-Policy'], agent_notes.content_security_policy('N0nce'))
            self.assertEqual(headers['Content-Type'], 'text/html; charset=utf-8')
            self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
            prefix = server.url.rsplit('/', 1)[0]
            refused = ['http://example.org/', 'http://127.0.0.1:8795/', 'http://localhost/', 'http://169.254.169.254/latest/meta-data/',
                       'http://thoughts-note.invalid/', 'http://thoughts-note.invalid/other/page.html', prefix + '/page.html?x=1',
                       prefix + '/../../etc/passwd', prefix + '/assets/marked@12.0.2/marked.min.js', prefix + '/note.html',
                       'http://thoughts-note.invalid.example.org' + server.url[len('http://thoughts-note.invalid'):],
                       'https://thoughts-note.invalid' + server.url[len('http://thoughts-note.invalid'):],
                       server.url[len('http://thoughts-note.invalid'):], '/etc/passwd', 'file:///etc/passwd']
            for url in refused:
                self.assertEqual(through_proxy(server.proxy, url)[0], 403, url)
            for method, url in (('CONNECT', 'example.org:443'), ('CONNECT', 'cdn.jsdelivr.net:443'), ('POST', server.url), ('PUT', server.url),
                                ('HEAD', 'http://example.org/'), ('OPTIONS', server.url), ('DELETE', server.url)):
                self.assertEqual(through_proxy(server.proxy, url, method)[0], 403, (method, url))
            self.assertEqual(len(server.refused), len(refused) + 7)
            self.assertIn('CONNECT example.org:443', server.refused)
        # A Markdown page's own files, and only those, are served next to it.
        asked = []

        def assets(name):
            asked.append(name)
            return stub_assets(name)
        with agent_notes.RenderServer('<p>note</p>', 'N0nce', assets) as server:
            prefix = server.url.rsplit('/', 1)[0]
            self.assertIn(f"style-src 'unsafe-inline' {prefix}/assets/;", through_proxy(server.proxy, server.url)[1]['Content-Security-Policy'])
            status, headers, body = through_proxy(server.proxy, prefix + '/assets/marked@12.0.2/marked.min.js')
            self.assertEqual((status, headers['Content-Type']), (200, 'text/javascript; charset=utf-8'))
            self.assertIn(b'window.marked', body)
            self.assertEqual(through_proxy(server.proxy, prefix + '/assets/katex@0.16.11/dist/fonts/KaTeX_Main-Regular.woff2')[1]['Content-Type'], 'font/woff2')
            for name in ('marked@13.0.0/marked.min.js', 'katex@0.16.11/dist/../../../etc/passwd', 'lodash@4/lodash.min.js', '', 'page.html',
                         'katex@0.16.11/dist/fonts/x.exe', 'https://example.org/x.js'):
                self.assertEqual(through_proxy(server.proxy, prefix + '/assets/' + name)[0], 404, name)
            self.assertEqual(through_proxy(server.proxy, 'http://example.org/assets/marked@12.0.2/marked.min.js')[0], 403)
            self.assertEqual(asked[:2], ['marked@12.0.2/marked.min.js', 'katex@0.16.11/dist/fonts/KaTeX_Main-Regular.woff2'])

    def test_markdown_files_come_from_the_cache_or_the_pinned_cdn(self):
        fetched = []

        def fetch(url):
            fetched.append(url)
            return b'/* downloaded */'
        cache = self.assets / 'cache'
        self.assertEqual(agent_notes.asset_cache_dir(), self.assets)
        self.assertEqual(agent_notes.load_asset('marked@12.0.2/marked.min.js', cache, fetch), (b'/* downloaded */', 'text/javascript; charset=utf-8'))
        self.assertEqual(fetched, ['https://cdn.jsdelivr.net/npm/marked@12.0.2/marked.min.js'])
        self.assertEqual((cache / 'marked@12.0.2/marked.min.js').read_bytes(), b'/* downloaded */')
        # The second render reads the cache: no download, also without a network.
        self.assertEqual(agent_notes.load_asset('marked@12.0.2/marked.min.js', cache, agent_notes.fetch_asset)[0], b'/* downloaded */')
        self.assertEqual(len(fetched), 1)
        for name, media_type in (('katex@0.16.11/dist/katex.min.css', 'text/css; charset=utf-8'), ('katex@0.16.11/dist/katex.min.js', 'text/javascript; charset=utf-8'),
                                 ('katex@0.16.11/dist/contrib/auto-render.min.js', 'text/javascript; charset=utf-8'),
                                 ('katex@0.16.11/dist/fonts/KaTeX_Math-Italic.woff2', 'font/woff2'), ('katex@0.16.11/dist/fonts/KaTeX_Size1-Regular.ttf', 'font/ttf')):
            self.assertEqual(agent_notes.load_asset(name, cache, fetch)[1], media_type)
            self.assertEqual(fetched[-1], 'https://cdn.jsdelivr.net/npm/' + name)
        # Nothing else is ever downloaded or read, whatever a page asks for.
        count = len(fetched)
        (cache / 'secret.txt').write_text('secret')
        for name in ('secret.txt', '../secret.txt', 'marked@12.0.2/../secret.txt', 'marked@12.0.3/marked.min.js', 'marked@12.0.2/marked.min.js?x=1',
                     'katex@0.16.11/dist/fonts/../../../../secret.txt', 'katex@0.16.11/dist/fonts/KaTeX_Main-Regular.exe', 'react@18/index.js',
                     '/etc/passwd', 'katex@0.16.11/dist/katex.min.js\n', ''):
            self.assertIsNone(agent_notes.load_asset(name, cache, fetch), name)
        self.assertEqual(len(fetched), count)
        # Offline and not cached: the page says that marked did not load.
        self.assertIsNone(agent_notes.load_asset('katex@0.16.11/dist/fonts/KaTeX_Fraktur-Bold.woff2', cache, agent_notes.fetch_asset))
        self.assertEqual(self.fetches, ['https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/fonts/KaTeX_Fraktur-Bold.woff2'])

    def test_html_file_takes_pictures_and_styles_from_its_own_folder(self):
        directory = Path(tempfile.mkdtemp(prefix='thoughts-html-'))
        self.addCleanup(lambda: __import__('shutil').rmtree(directory, True))
        directory = Path(os.path.realpath(directory))
        page, outside = directory / 'page', directory / 'outside'
        (page / 'img').mkdir(parents=True)
        outside.mkdir()
        (page / 'img/plot one.png').write_bytes(png(3, 2))
        (page / 'draw.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg"><rect width="1" height="1"/></svg>')
        (page / 'note.css').write_text('p{color:red} /* </style><script>x()</script> */')
        (page / 'notes.txt').write_text('not a picture')
        (outside / 'private.png').write_bytes(png(1, 1))
        os.symlink(outside / 'private.png', page / 'link.png')
        html = ('<link rel="stylesheet" href="note.css"><link rel="icon" href="note.css">'
                '<img src="img/plot%20one.png" alt="a"><img alt=b src=\'draw.svg\'><IMG SRC=img/plot%20one.png>'
                '<img src="../outside/private.png"><img src="link.png"><img src="/etc/passwd"><img src="file:///etc/passwd">'
                '<img src="notes.txt"><img src="missing.png"><img src="data:image/png;base64,AAAA"><img src="https://example.org/x.png">')
        warnings = []
        result = agent_notes.inline_local_files(html, page, warnings)
        encoded = __import__('base64').b64encode(png(3, 2)).decode()
        self.assertEqual(result.count(f'"data:image/png;base64,{encoded}"'), 2)
        self.assertIn(f'<IMG SRC="data:image/png;base64,{encoded}">', result)
        self.assertIn('<img alt=b src="data:image/svg+xml;base64,', result)
        self.assertIn('<style>p{color:red} /* <\\/style><script>x()<\\/script> */</style><link rel="icon" href="note.css">', result)
        # Left as written, so they do not load: outside the folder (also through a link), absolute, not a picture, missing.
        for kept in ('<img src="../outside/private.png">', '<img src="link.png">', '<img src="/etc/passwd">', '<img src="file:///etc/passwd">',
                     '<img src="notes.txt">', '<img src="missing.png">', '<img src="data:image/png;base64,AAAA">', '<img src="https://example.org/x.png">'):
            self.assertIn(kept, result)
        self.assertNotIn(__import__('base64').b64encode(png(1, 1)).decode(), result)
        self.assertEqual(len(warnings), 4, warnings)
        self.assertTrue(any('../outside/private.png' in w and "inside the HTML file's folder" in w for w in warnings))
        self.assertTrue(any(w.startswith('link.png') for w in warnings))
        self.assertTrue(any('notes.txt' in w and 'not a PNG' in w for w in warnings))
        self.assertTrue(any('missing.png was not found' in w for w in warnings))
        # A folder that is itself private gives nothing, whatever the file is called.
        home = directory / 'home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/key.png').write_bytes(png(1, 1))
        warnings = []
        self.assertEqual(agent_notes.inline_local_files('<img src="key.png">', home / '.ssh', warnings, homes=[home]), '<img src="key.png">')
        self.assertEqual(len(warnings), 1)
        # The rendered page carries the picture and no path.
        runner, calls = self.fake_runner(100)
        (page / 'index.html').write_text('<html><body><img src="img/plot one.png"></body></html>')
        rendered = agent_notes.render(html_path=str(page / 'index.html'), runner=runner, chrome=str(self.chrome))
        self.assertIn(f'<img src="data:image/png;base64,{encoded}">', calls[0]['page'])
        self.assertNotIn(str(page), calls[0]['page'])
        self.assertEqual(rendered.warnings, [])

    def test_warnings_for_what_a_page_cannot_load(self):
        self.assertEqual(agent_notes.unloaded_references('<p style="color:red">x</p><img src="data:image/png;base64,AA"><a href="https://example.org">l</a>'
                                                         '<svg><rect width="1" height="1"/></svg>'), [])
        warnings = agent_notes.unloaded_references('<script src="https://cdn.example/mathjax.js"></script><img src="//example.org/a.png">'
                                                   '<style>@import url("https://fonts.example/css");</style>')
        self.assertEqual(len(warnings), 2)
        self.assertIn('Not loaded: https://cdn.example/mathjax.js and 2 more.', warnings[0])
        self.assertIn('without network access', warnings[0])
        self.assertIn('scripts did not run', warnings[1])
        self.assertIn('scripts did not run', agent_notes.unloaded_references('<img src="data:image/png;base64,AA" onerror="x()">')[0])
        self.assertIn('Not loaded: https://example.org/p.png.', agent_notes.unloaded_references('![plot](https://example.org/p.png)', markdown=True)[0])
        self.assertEqual(agent_notes.unloaded_references('Costs $5 < $6 and [a link](https://example.org)', markdown=True), [])
        runner, _ = self.fake_runner(100)
        rendered = agent_notes.render(html='<script>draw()</script><p>x</p>', runner=runner, chrome=str(self.chrome))
        self.assertEqual(len(rendered.warnings), 1)
        self.assertIn('scripts did not run', rendered.warnings[0])

    def test_failures(self):
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            agent_notes.render(html='<p>x</p>', chrome='/nonexistent/Google Chrome')
        self.assertEqual(caught.exception.error_code, 'renderer_unavailable')
        self.assertIn('image_path', caught.exception.detail)
        runner, _ = self.fake_runner(100, code=1)
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            agent_notes.render(html='<p>x</p>', runner=runner, chrome=str(self.chrome))
        self.assertEqual(caught.exception.error_code, 'render_failed')
        runner, _ = self.fake_runner(100, extra_attributes=' data-thoughts-error="marked did not load"')
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            agent_notes.render(markdown='x', runner=runner, chrome=str(self.chrome))
        self.assertIn('marked did not load', caught.exception.detail)
        started = time.monotonic()
        with self.assertRaises(agent_notes.AgentNoteError) as caught:
            agent_notes.run_process(['/bin/sh', '-c', 'sleep 30 & sleep 30'], timeout=0.5)
        self.assertEqual(caught.exception.error_code, 'render_failed')
        self.assertLess(time.monotonic() - started, 5)

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CHROME') == '1', 'Set THOUGHTS_TEST_CHROME=1 to render with the real Google Chrome')
    def test_real_chrome_reaches_no_file_and_no_network(self):
        """What a hostile page tries, in the real browser: nothing may load or run, and the note still renders."""
        hits = []

        class Trap(BaseHTTPRequestHandler):
            def do_GET(self):
                hits.append(self.path)
                body = png(40, 40, (255, 0, 0))
                self.send_response(200)
                self.send_header('Content-Type', 'image/png')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass
        trap = ThreadingHTTPServer(('127.0.0.1', 0), Trap)
        threading.Thread(target=trap.serve_forever, daemon=True).start()
        self.addCleanup(trap.shutdown)
        port = trap.server_address[1]
        directory = Path(os.path.realpath(tempfile.mkdtemp(prefix='thoughts-secret-')))
        self.addCleanup(lambda: __import__('shutil').rmtree(directory, True))
        (directory / 'secret.png').write_bytes(png(40, 40, (255, 0, 0)))
        (directory / 'secret.html').write_text('<body style="background:#f00">secret</body>')
        secret = (directory / 'secret.png').as_uri()
        green = base64.b64encode(png(40, 40, (0, 255, 0))).decode()
        mark = "document.documentElement.setAttribute('data-thoughts-error','a script of the page ran')"
        html = f"""<html><head><style>body{{margin:0;background:#fff}} .band{{height:100px;background:#00f}}
            @import url("http://127.0.0.1:{port}/import.css"); .bg{{background:url("http://127.0.0.1:{port}/bg.png")}}</style>
            <link rel="stylesheet" href="http://127.0.0.1:{port}/style.css"><link rel="dns-prefetch" href="//example.org">
            <base href="file://{directory}/"><script>{mark}</script><script src="http://127.0.0.1:{port}/s.js"></script></head><body>
            <div class="band"></div><img src="data:image/png;base64,{green}" width="100" height="100" style="display:block">
            <img src="{secret}" width="100" height="100" style="display:block"><img src="secret.png" width="100" height="100">
            <img src="http://127.0.0.1:{port}/a.png" width="100" height="100"><img src="http://localhost:{port}/b.png">
            <img src="x" onerror="{mark}"><iframe src="{(directory / 'secret.html').as_uri()}" width="200" height="100"></iframe>
            <iframe src="http://127.0.0.1:{port}/frame.html"></iframe><object data="{secret}"></object><embed src="{secret}">
            <video poster="{secret}" src="http://127.0.0.1:{port}/v.mp4"></video><div class="bg" style="height:50px"></div>
            <svg width="100" height="100"><image href="{secret}" width="100" height="100"/></svg>
            <form action="http://127.0.0.1:{port}/form"><input type="image" src="http://127.0.0.1:{port}/i.png"></form>
            </body></html>"""
        rendered = agent_notes.render(html=html)     # raises render_failed if a script of the page ran
        width, height, rows = png_pixels(rendered.png)
        self.assertEqual(width, 1200)
        colours = {pixel for row in rows[::5] for pixel in row[::5]}
        self.assertIn((0, 0, 255), colours, 'inline styles apply')
        self.assertIn((0, 255, 0), colours, 'data: images show')
        self.assertFalse(any(r > 200 and g < 80 and b < 80 for r, g, b in colours), 'nothing of the local or the fetched pictures shows')
        self.assertEqual(hits, [], 'no request left the browser')
        self.assertEqual(len(rendered.warnings), 2)
        if os.environ.get('THOUGHTS_TEST_CHROME_OUT'):
            Path(os.environ['THOUGHTS_TEST_CHROME_OUT'], 'hostile.png').write_bytes(rendered.png)
        # An HTML file gets the pictures of its folder, and still nothing else.
        (directory / 'page.html').write_text(f'<body style="margin:0"><img src="secret.png" width="100" height="100" style="display:block">'
                                             f'<img src="http://127.0.0.1:{port}/c.png"></body>')
        _, _, rows = png_pixels(agent_notes.render(html_path=str(directory / 'page.html')).png)
        self.assertEqual(rows[100][100], (255, 0, 0))
        self.assertEqual(hits, [])
        # The Markdown page's own scripts, styles and fonts load from the render proxy and run.
        rendered = agent_notes.render(markdown='hello $x$ <img src=x onerror="' + mark + '">', assets=stub_assets)
        self.assertEqual(rendered.warnings[-1], 'stub KaTeX ran')
        _, _, rows = png_pixels(rendered.png)
        self.assertTrue(any(g > 100 and r < 60 and b < 60 for row in rows[::2] for r, g, b in row[::2]), 'the style sheet from the proxy applies')
        self.assertEqual(hits, [])
        self.assertEqual(self.fetches, [])

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CHROME') == '1', 'Set THOUGHTS_TEST_CHROME=1 to render with the real Google Chrome')
    def test_real_chrome_paints_a_short_note_to_its_last_line(self):
        """A note lower than 150 CSS px (Chromium on Linux painted only its first 128 device px under a host rule)."""
        html = ('<html><body style="margin:0;padding:8px 12px;font:20px/30px sans-serif;color:#000;background:#fff">'
                'first line<br>second line<br><a href="https://example.org/">third line</a></body></html>')
        rendered = agent_notes.render(html=html)
        width, height, rows = png_pixels(rendered.png)
        self.assertEqual((width, rendered.css_height), (1200, 106))
        inked = [y for y, row in enumerate(rows) if any(sum(pixel) < 600 for pixel in row[::3])]   # text on white
        self.assertGreater(max(inked), height * 0.7, 'the third line is painted')
        self.assertEqual([href for href, _rect, _label in rendered.links], ['https://example.org/'])
        self.assertTrue(68 <= rendered.links[0][1][1] < 98, rendered.links)   # on the third line (68 to 98 CSS px)

    @unittest.skipUnless(os.environ.get('THOUGHTS_TEST_CHROME') == '1', 'Set THOUGHTS_TEST_CHROME=1 to render with the real Google Chrome')
    def test_real_chrome(self):
        markdown = ('# The GRPO advantage\n\nFor a group of $G$ answers with rewards $r_1,\\dots,r_G$:\n\n'
                    '$$A_i = \\frac{r_i - \\operatorname{mean}(r)}{\\operatorname{std}(r)}$$\n\n'
                    '- no value network, the **group mean** is the baseline\n- inline \\(\\epsilon\\) and display \\[\\sum_i A_i = 0\\]\n\n'
                    '```python\nadv = (r - r.mean()) / r.std()  # $not math$\n```\n')
        for kind, kwargs in (('markdown', {'markdown': markdown}),
                             ('html', {'html': '<html><body style="margin:0"><div style="height:1500px;background:linear-gradient(red,blue)">'
                                               'tall</div></body></html>'})):
            started = time.monotonic()
            rendered = agent_notes.render(**kwargs)
            media_type, width, height = agent_notes.image_info(rendered.png)
            print(f'\n{kind}: {width}x{height} px, {len(rendered.png)} bytes, {time.monotonic() - started:.1f} s, warnings {rendered.warnings}',
                  file=sys.stderr)
            self.assertEqual((media_type, width), ('image/png', 1200))
            self.assertEqual(rendered.warnings, [])
            if kind == 'html':
                self.assertEqual(height, 3750)
            else:
                self.assertGreater(height, 600)
                self.assertLess(height, 3000)
            if os.environ.get('THOUGHTS_TEST_CHROME_OUT'):
                Path(os.environ['THOUGHTS_TEST_CHROME_OUT'], f'{kind}.png').write_bytes(rendered.png)


class McpToolsTest(unittest.IsolatedAsyncioTestCase):
    async def test_tools_over_stdio(self):
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        fake = FakeThoughts()
        self.addCleanup(fake.close)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'agent-notes'
            home = Path(directory) / 'home'
            home.mkdir()
            picture = Path(directory) / 'picture.png'
            picture.write_bytes(png(40, 30))
            env = {**os.environ, 'HOME': str(home), 'CFFIXED_USER_HOME': str(home), 'THOUGHTS_SERVER_URL': fake.url,
                   'THOUGHTS_TOKEN': TOKEN, 'THOUGHTS_AGENT_NOTES_DIR': str(root), 'THOUGHTS_CHROME': str(Path(directory) / 'no-chrome'),
                   'THOUGHTSCTL': '/nonexistent/thoughtsctl'}
            params = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name('server.py'))], env=env)
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    initialized = await session.initialize()
                    self.assertIn('send_note', initialized.instructions)
                    self.assertIn('get_note_reply', initialized.instructions)
                    tools = {t.name: t for t in (await session.list_tools()).tools}
                    self.assertTrue({'send_note', 'list_notes', 'list_note_replies', 'get_note_reply'} <= set(tools))
                    self.assertTrue(tools['list_notes'].annotations.readOnlyHint)
                    self.assertIn('exactly one of image_path', tools['send_note'].description)
                    self.assertIn('no_reply_yet', tools['get_note_reply'].description)

                    renderless = await session.call_tool('send_note', {'title': 'Math', 'markdown': '$x$'})
                    self.assertTrue(renderless.isError)
                    self.assertEqual(renderless.structuredContent['error_code'], 'renderer_unavailable')
                    for invalid in ({'title': 'x', 'image_path': 'relative.png'}, {'title': 'x'},
                                    {'title': 'x', 'image_path': str(picture), 'markdown': 'y'}, {'title': '', 'image_path': str(picture)}):
                        self.assertTrue((await session.call_tool('send_note', invalid)).isError, invalid)
                    self.assertEqual([r for r in fake.requests if r['method'] == 'POST'], [])

                    sent = await session.call_tool('send_note', {'title': 'Baseline', 'image_path': str(picture), 'review_card_id': 'card-7',
                                                                 'conversation_id': 'conv-9', 'message': 'Mark the baseline.'})
                    self.assertFalse(sent.isError, sent.content)
                    note = sent.structuredContent
                    self.assertEqual(note['image'], {'media_type': 'image/png', 'width': 40, 'height': 30, 'bytes': picture.stat().st_size})
                    self.assertFalse(note['delivery']['live'])
                    self.assertTrue(Path(note['image_path']).is_file())
                    self.assertTrue(Path(note['folder']).parent == root)
                    body = [r for r in fake.requests if r['method'] == 'POST'][0]['body']
                    self.assertEqual(body['origin'], {'agent': 'mcp', 'conversation_id': 'conv-9'})

                    listed = await session.call_tool('list_notes', {'review_card_id': 'card-7'})
                    self.assertFalse(listed.isError, listed.content)
                    self.assertEqual([n['note_id'] for n in listed.structuredContent['notes']], [note['note_id']])

                    none = await session.call_tool('get_note_reply', {'note_id': note['note_id']})
                    self.assertFalse(none.isError, none.content)
                    self.assertEqual(none.structuredContent['status'], 'no_reply_yet')
                    self.assertEqual(none.structuredContent['error_code'], 'no_reply_yet')

                    # The background watcher (started by send_note: a note < 24 h old) writes the
                    # reply into the folder without any tool call.
                    reply = fake.add_reply(note['note_id'], message='See the circle')
                    reply_png = Path(note['folder']) / 'reply-1.png'
                    deadline = time.monotonic() + 10
                    while not reply_png.exists() and time.monotonic() < deadline:
                        await __import__('asyncio').sleep(0.1)
                    self.assertTrue(reply_png.exists(), 'background watcher wrote the reply')

                    got = await session.call_tool('get_note_reply', {'note_id': note['note_id'], 'wait_seconds': 5})
                    self.assertFalse(got.isError, got.content)
                    result = got.structuredContent
                    self.assertEqual((result['status'], result['reply_id'], result['message']), ('replied', reply['reply_id'], 'See the circle'))
                    self.assertEqual(result['image_path'], str(reply_png))
                    self.assertEqual((result['strokes']['count'], result['strokes']['tools']), (3, {'pen': 2, 'highlighter': 1}))
                    self.assertTrue(result['image_included'])
                    images = [c for c in got.content if c.type == 'image']
                    self.assertEqual(len(images), 1)
                    self.assertEqual(images[0].mimeType, 'image/png')
                    self.assertEqual(base64.b64decode(images[0].data), reply_png.read_bytes())
                    self.assertEqual(len((root / 'replies.jsonl').read_text().splitlines()), 1)

                    # "Send with picture": the glasses photo comes with the words, as a second image.
                    photo = jpeg_header(640, 480)
                    with_photo = fake.add_reply(note['note_id'], strokes=False, message='A new version from this',
                                                photo=photo, capture='IMG_20261111_170138_123_ab12')
                    got = await session.call_tool('get_note_reply', {'note_id': note['note_id'], 'wait_seconds': 5})
                    result = got.structuredContent
                    self.assertEqual((result['reply_id'], result['photo_capture']), (with_photo['reply_id'], 'IMG_20261111_170138_123_ab12'))
                    self.assertEqual(Path(result['photo_path']).read_bytes(), photo)
                    self.assertTrue(result['photo_path'].endswith('reply-2.photo.jpg'), result['photo_path'])
                    self.assertTrue(result['photo_included'])
                    images = [c for c in got.content if c.type == 'image']
                    self.assertEqual([i.mimeType for i in images], ['image/png', 'image/jpeg'])
                    self.assertEqual(base64.b64decode(images[1].data), photo)
                    journal = [json.loads(line) for line in (root / 'replies.jsonl').read_text().splitlines()]
                    self.assertEqual((journal[-1]['photo'], journal[-1]['photo_capture']),
                                     (f"{Path(note['folder']).name}/reply-2.photo.jpg", 'IMG_20261111_170138_123_ab12'))
                    self.assertIsNone(journal[0]['photo'])

                    replies = await session.call_tool('list_note_replies', {})
                    self.assertFalse(replies.isError, replies.content)
                    self.assertEqual([r['reply_id'] for r in replies.structuredContent['replies']], [reply['reply_id'], with_photo['reply_id']])
                    bad = await session.call_tool('get_note_reply', {'note_id': note['note_id'], 'wait_seconds': 601})
                    self.assertTrue(bad.isError)
                started = time.monotonic()
            self.assertLess(time.monotonic() - started, 10, 'the server exits promptly despite a pending long-poll')
            self.assertTrue(all(r['authorization'] == 'Bearer ' + TOKEN for r in fake.requests))




PRIMER = 'Sample_primer__example.org__0123456789ab_cdef0123.pdf'


def links_runner(height, links):
    """Chrome as the two render passes see it, with the measuring pass reporting @p links."""
    attribute = html.escape(json.dumps(links), quote=True)

    def runner(argv, timeout):
        shot = next((a.split('=', 1)[1] for a in argv if a.startswith('--screenshot=')), None)
        if shot is None:
            return 0, (f'<html data-thoughts-height="{height}" data-thoughts-width="480" '
                       f'data-thoughts-links="{attribute}"><body></body></html>').encode(), b''
        Path(shot).write_bytes(png(1200, int(height * 2.5)))
        return 0, b'', b''
    return runner


class LinksTest(FakeServerCase):
    def setUp(self):
        super().setUp()
        self.pdfs = self.directory / 'current_pdfs'
        self.pdfs.mkdir()
        (self.pdfs / PRIMER).write_bytes(b'%PDF-1.4')
        self.chrome = self.directory / 'Google Chrome'
        self.chrome.write_text('#!/bin/sh\nexit 0\n')
        self.chrome.chmod(0o755)
        # patch.dict puts back what was there: the real-Chrome tests after this class still find THOUGHTS_CHROME.
        environment = unittest.mock.patch.dict(os.environ, {'THOUGHTS_CHROME': str(self.chrome), 'THOUGHTS_DOCS': str(self.directory)})
        environment.start()
        self.addCleanup(environment.stop)

    def test_hrefs_become_what_the_phone_opens(self):
        resolve = lambda href, doc=None: agent_notes.resolve_link(href, doc, self.pdfs)
        self.assertEqual(resolve('https://example.org/abs/1234.56789'), ('https://example.org/abs/1234.56789', None))
        self.assertEqual(resolve(f'{PRIMER}#page=53'), (f'thoughts-pdf-link:{PRIMER}?page=52', None))
        self.assertEqual(resolve(f'../../docs-concepts/{PRIMER}#page=1'), (f'thoughts-pdf-link:{PRIMER}?page=0', None))
        self.assertEqual(resolve(PRIMER), (f'thoughts-pdf-link:{PRIMER}?page=0', None))
        self.assertEqual(resolve('#page=9', PRIMER), (f'thoughts-pdf-link:{PRIMER}?page=8', None))
        self.assertEqual(resolve(f'thoughts-pdf-link:{PRIMER}?page=3&x=10&y=20'), (f'thoughts-pdf-link:{PRIMER}?page=3&x=10&y=20', None))
        target, warning = resolve('other.pdf#page=2')
        self.assertEqual(target, 'thoughts-pdf-link:other.pdf?page=1')
        self.assertIn('not in this Mac', warning)
        for dropped in ('#page=9', 'mailto:x@y', 'javascript:alert(1)', 'notes.md#source-label:x',
                        'thoughts-pdf-link:../x.pdf?page=1', '#intro', 'https://'):
            target, warning = resolve(dropped)
            self.assertIsNone(target, dropped)
            self.assertIn('dropped', warning)

    def test_rectangles_become_fractions_of_the_image(self):
        measured = [('https://a.org', [48, 100, 96, 20], 'A'), (f'{PRIMER}#page=2', [400, 190, 200, 30], 'B'),
                    ('https://gone.org', [10, 250, 50, 20], 'below the page'), ('mailto:x@y', [0, 0, 10, 10], 'm'),
                    ('mailto:x@y', [0, 20, 10, 10], 'm')]
        links, warnings = agent_notes.note_links(measured, 200, None, self.pdfs)
        self.assertEqual(links, [
            {'rect': [0.1, 0.5, 0.2, 0.1], 'href': 'https://a.org', 'label': 'A'},
            # Clipped to the page: 480 px wide, 200 px tall.
            {'rect': [0.833333, 0.95, 0.166667, 0.05], 'href': f'thoughts-pdf-link:{PRIMER}?page=1', 'label': 'B'},
        ])
        self.assertEqual(len(warnings), 1)  # one warning per href, not per rectangle
        many, warnings = agent_notes.note_links([('https://a.org', [0, i, 10, 1], '') for i in range(70)], 100, None, self.pdfs)
        self.assertEqual(len(many), 64)
        self.assertIn('only the first 64', warnings[0])

    def test_the_measuring_pass_reports_links(self):
        self.assertIn('data-thoughts-links', agent_notes.MEASURE_SCRIPT)
        self.assertIn('getClientRects', agent_notes.MEASURE_SCRIPT)
        dom = '<html data-thoughts-links="[[&quot;https://a.org/?q=1&amp;r=2&quot;,1,2,3,4,&quot;A &amp; B&quot;]]"><body></body></html>'
        self.assertEqual(agent_notes.measured_links(dom), [('https://a.org/?q=1&r=2', [1.0, 2.0, 3.0, 4.0], 'A & B')])
        self.assertEqual(agent_notes.measured_links('<html data-thoughts-links="not json"></html>'), [])
        self.assertEqual(agent_notes.measured_links('<html></html>'), [])

    def test_send_note_sends_the_links_of_an_html_note(self):
        runner = links_runner(200, [[f'{PRIMER}#page=53', 240, 100, 48, 20, 'p. 53'], ['mailto:x@y', 0, 0, 10, 10, 'm']])
        result = agent_notes.send_note('Two questions', html='<p><a href="x">p. 53</a></p>', client=self.client,
                                       root=self.root, runner=runner)
        body = [r for r in self.fake.requests if r['method'] == 'POST'][0]['body']
        self.assertEqual(body['links'], [{'rect': [0.5, 0.5, 0.1, 0.1], 'href': f'thoughts-pdf-link:{PRIMER}?page=52', 'label': 'p. 53'}])
        self.assertEqual(result['links'], [{'href': f'thoughts-pdf-link:{PRIMER}?page=52', 'label': 'p. 53'}])
        self.assertTrue(any('mailto:x@y' in w for w in result['warnings']))
        folder = Path(result['folder'])
        self.assertEqual(json.loads((folder / 'note.json').read_text())['links'], body['links'])

    def test_notes_without_links_send_none(self):
        result = agent_notes.send_note('Plain', html='<p>no links</p>', client=self.client, root=self.root,
                                       runner=links_runner(100, []))
        body = [r for r in self.fake.requests if r['method'] == 'POST'][0]['body']
        self.assertNotIn('links', body)
        self.assertEqual(result['links'], [])

    def test_a_server_without_links_is_reported(self):
        self.fake.supports_links = False
        result = agent_notes.send_note('Old server', html='<p>x</p>', client=self.client, root=self.root,
                                       runner=links_runner(100, [['https://a.org', 0, 0, 50, 20, 'a']]))
        self.assertTrue(any('does not support links' in w for w in result['warnings']))


if __name__ == '__main__':
    unittest.main()
