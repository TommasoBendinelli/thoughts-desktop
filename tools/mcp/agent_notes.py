#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Agent notes to the phone, Mac side (thoughts-server protocol/agent-notes.md).

Sends a PNG/JPEG, or HTML/Markdown rendered to a PNG with headless Google Chrome, to the
Thoughts server, and writes the phone's annotated replies into a folder
(~/Thoughts/agent-notes, THOUGHTS_AGENT_NOTES_DIR overrides). Standard library only, so the
MCP adapter (server.py, next to this file) and the command line share it. What an agent
supplies is not trusted: its HTML is rendered without access to files, the network or its own
scripts (see "rendering"), and the files it names are not read from private locations
(readable_path).

    python3 agent_notes.py watch [--once]
    python3 agent_notes.py send --title T (--image P | --html-file P | --markdown-file P) [--card ID] [--document-id ID] [--message M]
    python3 agent_notes.py replies [--note ID]

Nothing here prints to stdout except the command line; the MCP adapter's stdout is the
protocol stream. The device token is never logged or returned.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import datetime as dt
import hashlib
import html as html_escape
import html as html_lib
import http.client
import json
import os
import re
import ipaddress
import secrets
import shutil
import signal
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable

try:
    import fcntl
except ImportError:  # Windows: one process at a time is assumed.
    fcntl = None

PAGE_WIDTH = 480                      # CSS px, phone width
SCALE = 2.5                           # device pixels per CSS px
MAX_IMAGE_SIDE = 8192                 # server limit per side
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_CSS_HEIGHT = int(MAX_IMAGE_SIDE // SCALE)   # 3276 CSS px -> 8190 px
CHROME_TIMEOUT = 45.0
CHROME_DEFAULT = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
CONNECT_TIMEOUT = 10.0
FEED_PAGE = 50
RECENT_SECONDS = 24 * 3600
# The Markdown page's own scripts, styles and fonts: fixed versions from the npm CDN. This program
# downloads them (once, into a cache); the browser that renders a note has no network at all.
ASSET_ORIGIN = "https://cdn.jsdelivr.net/npm/"
MARKED_ASSET = "marked@12.0.2/marked.min.js"
KATEX_ASSETS = "katex@0.16.11/dist"
ASSET_NAME = re.compile(r"^(?:marked@12\.0\.2/marked\.min\.js"
                        r"|katex@0\.16\.11/dist/(?:katex\.min\.css|katex\.min\.js|contrib/auto-render\.min\.js"
                        r"|fonts/KaTeX_[A-Za-z0-9_-]+\.(?:woff2|woff|ttf)))$")
ASSET_TYPES = {".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8", ".woff2": "font/woff2",
               ".woff": "font/woff", ".ttf": "font/ttf"}
MAX_ASSET_BYTES = 5 * 1024 * 1024
ASSET_TIMEOUT = 20.0
RENDER_HOST = "thoughts-note.invalid"   # the page's only origin; it exists nowhere but in the render proxy
MAX_INLINE_FILE = 20 * 1024 * 1024
UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
RENDER_HINT = "Render the picture yourself and send it as a PNG with image_path instead."
KEYCHAIN_ACCOUNT = "thoughts-mac"     # the desktop app's device token in the login Keychain


class AgentNoteError(Exception):
    """An error with a stable error_code, returned to agents as {"error_code", "detail"}."""

    def __init__(self, error_code: str, detail: str, http_status: int | None = None):
        super().__init__(f"{error_code}: {detail}")
        self.error_code, self.detail, self.http_status = error_code, detail, http_status

    def as_dict(self) -> dict:
        result = {"error_code": self.error_code, "detail": self.detail}
        if self.http_status is not None:
            result["http_status"] = self.http_status
        return result


def log(message: str) -> None:
    print(f"agent-notes: {message}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------- configuration

@dataclass
class Config:
    server_url: str
    token: str = field(repr=False)


def keychain_token() -> str | None:
    """The token the desktop app stores in the login Keychain (created by /usr/bin/security, so no prompt)."""
    if sys.platform != "darwin":
        return None
    try:
        done = subprocess.run(["/usr/bin/security", "find-generic-password", "-s", "thoughts-server", "-a", KEYCHAIN_ACCOUNT, "-w"],
                              capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        return None
    token = done.stdout.strip()
    return token if done.returncode == 0 and token else None


def is_loopback_host(host: str) -> bool:
    """Whether a URL's host is this computer itself: localhost, a name under .localhost, or a loopback address."""
    host = host.strip().strip("[]").rstrip(".").lower()
    if host == "localhost" or host.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


# Where files an agent names are never read from, whatever links lead there.
SYSTEM_PRIVATE = ("/etc", "/private/etc", "/var/db", "/private/var/db", "/var/root", "/private/var/root", "/root", "/var/run",
                  "/private/var/run", "/run", "/var/lib", "/proc", "/sys", "/dev", "/Library/Keychains",
                  "/System/Library/Keychains", "/Network/Library/Keychains")


def _home_directories() -> list[Path]:
    homes = [Path.home()]
    try:
        import pwd
        homes.append(Path(pwd.getpwuid(os.getuid()).pw_dir))   # the account's home, whatever HOME says
    except (ImportError, KeyError, OSError):
        pass
    return homes


def _below(path: Path, directory: Path) -> tuple | None:
    """The parts of `path` below `directory` if it lies in it, else None. Directories are compared by
    identity (device and inode), so letter case and Unicode form cannot hide where a path leads."""
    try:
        wanted = os.stat(directory)
    except OSError:
        return None
    below, current = [], path
    while True:
        try:
            found = os.stat(current)
            if (found.st_dev, found.st_ino) == (wanted.st_dev, wanted.st_ino):
                return tuple(reversed(below))
        except OSError:
            pass
        if current.parent == current:
            return None
        below.append(current.name)
        current = current.parent


def private_location(path: Path, homes: list | None = None) -> str | None:
    """Why the file at `path` (symlinks already resolved) is not read for an agent, or None if it may be.

    Refused: hidden entries of the home directory (.ssh, .aws, .config, .gnupg, .netrc, ...), ~/Library
    (application data, Keychains, cookies, mail), keychain files anywhere and the system's credential
    folders. Everything else, such as Documents, Desktop, Downloads, projects and temporary folders, is allowed.
    """
    if any(part.casefold().endswith((".keychain", ".keychain-db")) for part in path.parts):
        return "a keychain"
    for home in (_home_directories() if homes is None else homes):
        below = _below(path, home)
        if not below:
            continue
        first = below[0]
        if first.startswith("."):
            return f"a hidden folder of the home directory (~/{first})"
        if first.casefold() == "library" or (os.name == "nt" and first.casefold() == "appdata"):
            return f"~/{first}"
    for directory in SYSTEM_PRIVATE:
        if _below(path, Path(directory)) is not None:
            return f"the system folder {directory}"
    return None


def readable_path(value: str, name: str, homes: list | None = None) -> Path:
    """The file an agent named (image_path, html_path, a PDF to import) with symlinks resolved.

    An agent can be talked into naming any path (a prompt injection in a page it read), and what
    it sends leaves this computer: private locations are refused with path_refused.
    """
    if "\x00" in value:
        raise AgentNoteError("path_refused", f"{name} is not a file path.")
    resolved = Path(os.path.realpath(Path(value).expanduser()))
    reason = private_location(resolved, homes)
    if reason:
        raise AgentNoteError("path_refused", f"{name} leads into {reason}, where files are never read for an agent: {resolved}")
    return resolved


def url_origin(url: str) -> str:
    """scheme://host[:port] of a URL, lower case and without the scheme's default port; "" if it has no host."""
    try:
        parts = urllib.parse.urlsplit(url.strip())
        host, port = (parts.hostname or "").lower(), parts.port
    except ValueError:
        return ""
    if not host or parts.scheme not in ("http", "https"):
        return ""
    if ":" in host:
        host = f"[{host}]"
    default = 443 if parts.scheme == "https" else 80
    return f"{parts.scheme}://{host}" + (f":{port}" if port and port != default else "")


def token_fingerprint(token: str) -> str:
    """What Thoughts records of a stored token to notice that another one was stored (thoughtsserveraddress.cpp)."""
    return hashlib.sha256(b"thoughts-token-binding-v1\0" + token.encode("utf-8")).hexdigest()[:32]


def stored_token_origin(settings: dict, token: str) -> str:
    """The origin of the server that issued the Keychain token, as Thoughts records it in server.json.

    The token goes only to that server. Without a record (Thoughts has not bound it yet), and for a
    token stored after the record was made, it belongs to the saved address. "" when nothing is known.
    """
    origins = settings.get("token_origins")
    origins = origins if isinstance(origins, dict) else {}
    saved = url_origin(str(settings.get("server_url") or ""))

    def recorded(account: str) -> tuple[str, str]:
        entry = origins.get(account)
        entry = entry if isinstance(entry, dict) else {}
        return url_origin(str(entry.get("origin") or "")), str(entry.get("token_fingerprint") or "")
    own, fingerprint = recorded(KEYCHAIN_ACCOUNT)
    if own:
        return own if fingerprint == token_fingerprint(token) else saved
    return recorded("*")[0] or saved


def load_config(environ=None, keychain: Callable[[], str | None] = keychain_token, home: Path | None = None) -> Config:
    environ = os.environ if environ is None else environ
    path = (home or Path.home()) / "Library/Application Support/Thoughts/server.json"
    try:
        settings = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        settings = {}
    settings = settings if isinstance(settings, dict) else {}
    url = (environ.get("THOUGHTS_SERVER_URL") or "").strip()
    if not url:
        url = str(settings.get("server_url") or "").strip()
        if not url:
            raise AgentNoteError("no_server_config", f"No Thoughts server configured: set THOUGHTS_SERVER_URL or server_url in {path}.")
    scheme = urllib.parse.urlsplit(url).scheme
    if scheme not in ("http", "https"):
        raise AgentNoteError("no_server_config", "The Thoughts server URL must start with http:// or https://.")
    if scheme == "http" and not is_loopback_host(urllib.parse.urlsplit(url).hostname or ""):
        # The device token and the notes would cross the network unencrypted.
        raise AgentNoteError("no_server_config", "The Thoughts server URL must start with https://; http:// is accepted only for "
                                                 "this computer itself (localhost, 127.0.0.1, ::1).")
    token = (environ.get("THOUGHTS_TOKEN") or "").strip()
    if not token:
        token = keychain() or ""
        # The stored token belongs to the server that issued it: it is not sent to another one
        # (a changed server.json, or a THOUGHTS_SERVER_URL override).
        issued_for = stored_token_origin(settings, token) if token else ""
        if issued_for and issued_for != url_origin(url):
            raise AgentNoteError("no_credentials", f"The device token stored on this Mac was issued for {issued_for} and is not sent to "
                                                   f"{url_origin(url) or url}. Set THOUGHTS_TOKEN for that server, or store its token with "
                                                   f"thoughtsctl store-secret --account {KEYCHAIN_ACCOUNT}.")
    if not token:
        raise AgentNoteError("no_credentials", "No Thoughts device token: set THOUGHTS_TOKEN, or pair this Mac in Thoughts "
                                               "(Keychain item thoughts-server / thoughts-mac).")
    return Config(url.rstrip("/"), token)


def notes_root(environ=None) -> Path:
    environ = os.environ if environ is None else environ
    configured = environ.get("THOUGHTS_AGENT_NOTES_DIR")
    return Path(configured).expanduser() if configured else Path.home() / "Thoughts" / "agent-notes"


def ssl_context() -> ssl.SSLContext:
    context = ssl.create_default_context()
    if not context.cert_store_stats().get("x509_ca"):
        # python.org builds ship without CA certificates unless "Install Certificates" ran.
        try:
            import certifi  # present in the MCP venv (httpx dependency)
            context.load_verify_locations(certifi.where())
        except (ImportError, OSError):
            if os.path.exists("/etc/ssl/cert.pem"):
                context.load_verify_locations("/etc/ssl/cert.pem")
    return context


# ---------------------------------------------------------------- HTTP client

class Client:
    """The agent-notes endpoints of the Thoughts server, with the Mac's device token."""

    def __init__(self, config: Config, connect_timeout: float = CONNECT_TIMEOUT):
        self.config, self.connect_timeout = config, connect_timeout
        parts = urllib.parse.urlsplit(config.server_url)
        self.https = parts.scheme == "https"
        self.host, self.port = parts.hostname or "", parts.port
        self.base_path = parts.path.rstrip("/")
        self._ssl = ssl_context() if self.https else None

    def request(self, method: str, path: str, query: dict | None = None, body: dict | None = None,
                read_timeout: float = 30.0, raw: bool = False):
        target = self.base_path + path
        query = {k: v for k, v in (query or {}).items() if v is not None}
        if query:
            target += "?" + urllib.parse.urlencode(query)
        if self.https:
            connection = http.client.HTTPSConnection(self.host, self.port, timeout=self.connect_timeout, context=self._ssl)
        else:
            connection = http.client.HTTPConnection(self.host, self.port, timeout=self.connect_timeout)
        where = f"the Thoughts server ({self.config.server_url})"
        try:
            try:
                connection.connect()
            except (OSError, http.client.HTTPException) as error:
                raise AgentNoteError("server_unreachable", f"Cannot connect to {where}: {error}") from None
            connection.sock.settimeout(read_timeout)
            headers = {"Authorization": "Bearer " + self.config.token, "Accept": "*/*" if raw else "application/json",
                       "User-Agent": "thoughts-mcp-agent-notes/1"}
            data = None
            if body is not None:
                data = json.dumps(body, ensure_ascii=False).encode("utf-8")
                headers["Content-Type"] = "application/json"
            connection.request(method, target, body=data, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            status = response.status
        except AgentNoteError:
            raise
        except TimeoutError:
            raise AgentNoteError("server_timeout", f"{where} did not answer within {read_timeout:.0f} s.") from None
        except (OSError, http.client.HTTPException) as error:
            raise AgentNoteError("server_unreachable", f"Request to {where} failed: {error}") from None
        finally:
            connection.close()
        if status >= 400:
            raise map_error(status, payload)
        if raw:
            return payload
        if not payload:
            return {}
        try:
            return json.loads(payload)
        except ValueError:
            raise AgentNoteError("invalid_response", f"The server answered {method} {path} with non-JSON content.", status) from None

    # Endpoints (protocol/agent-notes.md, "HTTP API").
    def create_note(self, body: dict) -> dict:
        return self.request("POST", "/v1/agent-notes", body=body, read_timeout=120)

    def list_notes(self, document_id=None, review_card_id=None, sent_by_me=None, limit=None) -> dict:
        return self.request("GET", "/v1/agent-notes", query=dict(
            document_id=document_id, review_card_id=review_card_id,
            sent_by_me=None if sent_by_me is None else str(bool(sent_by_me)).lower(), limit=limit))

    def get_note(self, note_id: str) -> dict:
        return self.request("GET", f"/v1/agent-notes/{urllib.parse.quote(note_id)}")

    def replies(self, after: int | None = None, wait: int = 0, note_id: str | None = None,
                conversation_id: str | None = None, listen: bool = False) -> dict:
        """The reply feed. listen=True only while an agent waits to act on what it returns: the server
        then tells the phone that the user's reply will be heard (protocol "Who hears a reply")."""
        wait = max(0, min(60, int(wait)))
        return self.request("GET", "/v1/agent-notes/replies",
                            query=dict(after=after, wait=wait, note_id=note_id, conversation_id=conversation_id,
                                       listen="true" if listen else None),
                            read_timeout=wait + 20)

    def download(self, url: str) -> bytes:
        """image_url / strokes_url from a record: a path on this server."""
        parts = urllib.parse.urlsplit(url)
        path = parts.path
        if self.base_path and path.startswith(self.base_path + "/v1/"):
            path = path[len(self.base_path):]
        if parts.scheme or parts.netloc or not path.startswith("/v1/agent-notes/"):
            raise AgentNoteError("invalid_response", f"Unexpected download URL from the server: {url[:200]}")
        return self.request("GET", path + (("?" + parts.query) if parts.query else ""), read_timeout=120, raw=True)


def map_error(status: int, payload: bytes) -> AgentNoteError:
    try:
        body = json.loads(payload)
    except ValueError:
        body = None
    code = detail = None
    if isinstance(body, dict):
        code = body.get("error_code")
        detail = body.get("detail")
        if isinstance(detail, dict):  # {"detail": {"error_code", "detail"|"message"}}
            code = code or detail.get("error_code")
            detail = detail.get("detail") or detail.get("message") or json.dumps(detail)
        elif isinstance(detail, list):  # request validation errors
            detail = "; ".join(str(item.get("msg", item)) if isinstance(item, dict) else str(item) for item in detail)
    if not code:
        code = {401: "unauthorized", 403: "unauthorized", 404: "not_found", 409: "agent_note_conflict",
                413: "agent_note_too_large", 422: "invalid_agent_note"}.get(status, "server_error")
        if status == 404:
            detail = f"{detail or 'Not Found'} (does this Thoughts server support agent notes, capability agent_notes_v1?)"
    if not detail:
        detail = payload.decode("utf-8", "replace").strip()[:500] or f"HTTP {status}"
    return AgentNoteError(code, str(detail)[:2000], status)


_client_cache: dict = {}


def default_client() -> Client:
    """One client per (URL, token source) for this process, so the Keychain is read once."""
    key = (os.environ.get("THOUGHTS_SERVER_URL"), os.environ.get("THOUGHTS_TOKEN"))
    if key not in _client_cache:
        _client_cache.clear()
        _client_cache[key] = Client(load_config())
    return _client_cache[key]


# ---------------------------------------------------------------- images

def image_info(data: bytes) -> tuple[str, int, int]:
    """(media_type, width, height) from a PNG or JPEG header; unsupported_image otherwise."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        if len(data) < 24 or data[12:16] != b"IHDR":
            raise AgentNoteError("unsupported_image", "The PNG has no IHDR header.")
        width, height = struct.unpack(">II", data[16:24])
        return "image/png", width, height
    if data[:3] == b"\xff\xd8\xff":
        offset = 2
        while offset + 4 <= len(data):
            if data[offset] != 0xFF:
                offset += 1
                continue
            marker = data[offset + 1]
            if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7 or marker == 0xFF:
                offset += 1 if marker == 0xFF else 2
                continue
            length = struct.unpack(">H", data[offset + 2:offset + 4])[0]
            if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
                if offset + 9 > len(data):
                    break
                height, width = struct.unpack(">HH", data[offset + 5:offset + 9])
                return "image/jpeg", width, height
            if marker == 0xDA or length < 2:
                break
            offset += 2 + length
        raise AgentNoteError("unsupported_image", "The JPEG has no frame header with its size.")
    raise AgentNoteError("unsupported_image", "Only PNG and JPEG images can be sent (checked by file signature).")


def check_image(data: bytes) -> tuple[str, int, int]:
    media_type, width, height = image_info(data)
    if not width or not height:
        raise AgentNoteError("unsupported_image", "The image has no size.")
    if len(data) > MAX_IMAGE_BYTES or max(width, height) > MAX_IMAGE_SIDE or width * height > MAX_IMAGE_PIXELS:
        raise AgentNoteError("agent_note_too_large", f"The image is {width}x{height} px, {len(data)} bytes; the limits are "
                                                     f"{MAX_IMAGE_SIDE} px per side, 40 megapixels and 20 MiB.")
    return media_type, width, height


def extension(media_type: str) -> str:
    return ".jpg" if media_type == "image/jpeg" else ".png"


# ---------------------------------------------------------------- rendering
#
# A note's HTML or Markdown comes from an agent, and an agent can be talked into anything by what
# it read. The page is therefore rendered where it can reach nothing:
#   - It is served to Chrome by a proxy inside this process, under an origin that exists nowhere
#     else (http://thoughts-note.invalid). It is not a file:// page, so it cannot show local files.
#   - Chrome sends every request to that proxy, which answers for the page (and, for Markdown,
#     this program's own KaTeX and marked files) and refuses everything else: no network.
#   - The page's Content-Security-Policy allows inline styles and data: images. Scripts run only
#     with a nonce that changes with every render, which this program's own scripts carry.

# Writes the content height (and width, with any overflow) into <html data-thoughts-*>.
# Headless Chrome keeps windows at least 500 px wide, so the --dump-dom pass would lay the
# page out wider than the screenshot pass (which honours 480 exactly): when the viewport is
# wider than 480, <html> is pinned to 480 px so both passes wrap text the same way.
MEASURE_SCRIPT = """<script nonce="{nonce}">(function(){var W=%d,d=document.documentElement;
function pin(){if(innerWidth>W&&d.style.width!==W+'px')d.style.width=W+'px';}
function m(){pin();var b=document.body,over=b?Math.max(0,b.scrollWidth-b.clientWidth):0;
d.setAttribute('data-thoughts-height',String(Math.ceil(Math.max(d.scrollHeight,b?b.scrollHeight:0))));
d.setAttribute('data-thoughts-width',String(W+over));links();}
function links(){var out=[],as=document.querySelectorAll('a');
for(var i=0;i<as.length&&out.length<200;i++){var a=as[i],h=a.getAttribute('href')||a.getAttributeNS('http://www.w3.org/1999/xlink','href');
if(!h)continue;var t=(a.textContent||'').replace(/\\s+/g,' ').trim().slice(0,200),rs=a.getClientRects();
for(var j=0;j<rs.length;j++){var r=rs[j];if(r.width>=1&&r.height>=1)out.push([h,r.left+scrollX,r.top+scrollY,r.width,r.height,t]);}}
d.setAttribute('data-thoughts-links',JSON.stringify(out));}
pin();setInterval(m,100);window.addEventListener('load',m);if(document.fonts&&document.fonts.ready)document.fonts.ready.then(m);})();</script>""" % PAGE_WIDTH

MARKDOWN_TEMPLATE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=480">
<link rel="stylesheet" href="assets/{katex}/katex.min.css">
<script nonce="{nonce}" src="assets/{marked}"></script>
<script nonce="{nonce}" src="assets/{katex}/katex.min.js"></script>
<script nonce="{nonce}" src="assets/{katex}/contrib/auto-render.min.js"></script>
<style>
html{{background:#fbfaf6}}
body{{margin:0;padding:22px 20px 28px;font:18px/1.55 -apple-system,"Helvetica Neue",Helvetica,Arial,sans-serif;color:#1c1c1c;word-wrap:break-word}}
h1,h2,h3,h4{{line-height:1.25;margin:1.1em 0 .45em}} h1{{font-size:1.55em}} h2{{font-size:1.32em}} h3{{font-size:1.15em}}
#content>:first-child{{margin-top:0}} p,ul,ol,blockquote,pre,table{{margin:.6em 0}} ul,ol{{padding-left:1.4em}}
code{{font:.88em/1.4 Menlo,Consolas,monospace;background:#efede6;padding:.1em .3em;border-radius:4px}}
pre{{background:#efede6;padding:10px 12px;border-radius:6px;white-space:pre-wrap}} pre code{{background:none;padding:0}}
blockquote{{border-left:4px solid #d4cfc2;margin-left:0;padding-left:12px;color:#4a4a4a}}
table{{border-collapse:collapse}} th,td{{border:1px solid #cfcabd;padding:4px 8px}} img{{max-width:100%}}
.katex{{font-size:1.08em}} .katex-display{{margin:.8em 0}} hr{{border:0;border-top:1px solid #d4cfc2}}
</style></head><body><main id="content"></main>
<script type="application/json" id="thoughts-markdown">{source}</script>
<script nonce="{nonce}">(function(){{
var d=document.documentElement,out=document.getElementById('content');
var src=JSON.parse(document.getElementById('thoughts-markdown').textContent);
if(typeof marked==='undefined'){{d.setAttribute('data-thoughts-error','marked did not load from cdn.jsdelivr.net (offline?)');out.textContent=src;return;}}
var math=[],parts=src.split(/(```[\\s\\S]*?```|~~~[\\s\\S]*?~~~|`[^`\\n]*`)/);
for(var i=0;i<parts.length;i+=2){{parts[i]=parts[i].replace(/\\$\\$[\\s\\S]+?\\$\\$|\\\\\\[[\\s\\S]+?\\\\\\]|\\\\\\([\\s\\S]+?\\\\\\)|\\$(?!\\s)[^$\\n]+?(?<!\\s)\\$(?!\\d)/g,function(x){{math.push(x);return '@@THOUGHTSMATH'+(math.length-1)+'@@';}});}}
function esc(s){{return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}}
out.innerHTML=marked.parse(parts.join('')).replace(/@@THOUGHTSMATH(\\d+)@@/g,function(_,n){{return esc(math[+n]);}});
if(typeof renderMathInElement==='undefined'){{if(math.length)d.setAttribute('data-thoughts-warning','KaTeX did not load; math is shown as source');return;}}
renderMathInElement(out,{{delimiters:[{{left:'$$',right:'$$',display:true}},{{left:'\\\\[',right:'\\\\]',display:true}},{{left:'\\\\(',right:'\\\\)',display:false}},{{left:'$',right:'$',display:false}}],throwOnError:false}});
}})();</script>
{measure}
</body></html>
"""


def new_nonce() -> str:
    return secrets.token_urlsafe(18)


def markdown_page(markdown: str, nonce: str | None = None) -> str:
    nonce = nonce or new_nonce()
    source = json.dumps(markdown, ensure_ascii=False).replace("<", "\\u003c")  # nothing can close the <script>
    return MARKDOWN_TEMPLATE.format(katex=KATEX_ASSETS, marked=MARKED_ASSET, source=source, nonce=nonce,
                                    measure=MEASURE_SCRIPT.replace("{nonce}", nonce))


def html_page(html: str, nonce: str | None = None) -> str:
    """The agent's HTML as is, plus the height-measuring script and, when missing, a viewport.

    Only that script carries the render's nonce, so it is the only script that runs.
    """
    head = MEASURE_SCRIPT.replace("{nonce}", nonce or new_nonce())
    if not re.search(r"<meta[^>]+name\s*=\s*[\"']?viewport", html, re.I):
        head = f'<meta name="viewport" content="width={PAGE_WIDTH}">' + head
    for pattern in (r"<head\b[^>]*>", r"<html\b[^>]*>"):
        match = re.search(pattern, html, re.I)
        if match:
            return html[:match.end()] + head + html[match.end():]
    return head + html


def content_security_policy(nonce: str, assets_url: str | None = None) -> str:
    """What a note's page may load: inline styles and data: images, and scripts with this render's nonce.

    @p assets_url (Markdown pages) adds this program's own style sheet and fonts under that URL.
    """
    style = "style-src 'unsafe-inline'" + (f" {assets_url}" if assets_url else "")
    font = f"font-src {assets_url}; " if assets_url else ""
    return (f"default-src 'none'; script-src 'nonce-{nonce}'; {style}; img-src data:; {font}"
            "base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


_IMAGE_TYPES = ((b"\x89PNG\r\n\x1a\n", "image/png"), (b"\xff\xd8\xff", "image/jpeg"), (b"GIF87a", "image/gif"), (b"GIF89a", "image/gif"))


def _inline_image_type(path: Path, data: bytes) -> str | None:
    for signature, media_type in _IMAGE_TYPES:
        if data.startswith(signature):
            return media_type
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    if path.suffix.lower() == ".svg" and b"<svg" in data[:4096].lower():
        return "image/svg+xml"
    return None


def inline_local_files(html: str, base_dir: Path, warnings: list, homes: list | None = None) -> str:
    """Put the pictures and style sheets an HTML file names next to itself into the page.

    The page is rendered without access to files, so <img src="plot.png"> becomes a data: URI
    and <link rel="stylesheet" href="note.css"> a <style> element. Only files inside the HTML
    file's own folder are read, by the rules of readable_path; anything else stays as written
    (and does not load), with a warning.
    """
    base = Path(os.path.realpath(base_dir))

    def local_file(reference: str, limit: int) -> tuple[Path, bytes] | None:
        reference = html_escape.unescape(reference).strip()
        parts = urllib.parse.urlsplit(reference)
        if not reference or parts.scheme or parts.netloc or reference.startswith(("/", "\\", "#")):
            return None     # a data: URI, an external URL (reported by unloaded_references) or an absolute path
        path = Path(os.path.realpath(base / urllib.parse.unquote(parts.path)))
        if _below(path, base) is None or private_location(path, homes):
            warnings.append(f"{reference} was not put into the page: only files inside the HTML file's folder are read.")
            return None
        try:
            if path.stat().st_size > limit:
                warnings.append(f"{reference} was not put into the page: it is larger than {limit // (1024 * 1024)} MiB.")
                return None
            return path, path.read_bytes()
        except OSError:
            warnings.append(f"{reference} was not found next to the HTML file.")
            return None

    def image(match: re.Match) -> str:
        found = local_file(match.group("src"), MAX_INLINE_FILE)
        if found is None:
            return match.group(0)
        media_type = _inline_image_type(*found)
        if media_type is None:
            warnings.append(f"{match.group('src')} was not put into the page: it is not a PNG, JPEG, GIF, WebP or SVG picture.")
            return match.group(0)
        return f'{match.group("before")}"data:{media_type};base64,{base64.b64encode(found[1]).decode("ascii")}"'

    def stylesheet(match: re.Match) -> str:
        tag = match.group(0)
        if not re.search(r"\brel\s*=\s*[\"']?[^\"'>]*\bstylesheet\b", tag, re.I):
            return tag
        href = re.search(r"\bhref\s*=\s*(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))", tag, re.I)
        found = local_file(next(g for g in href.groups() if g is not None), 2 * 1024 * 1024) if href else None
        if found is None:
            return tag
        css = found[1].decode("utf-8", "replace").replace("</", "<\\/")     # nothing can close the <style>
        return f"<style>{css}</style>"

    html = re.sub(r"(?P<before><img\b[^>]*?\bsrc\s*=\s*)(?:\"(?P<a>[^\"]*)\"|'(?P<b>[^']*)'|(?P<c>[^\s>]+))",
                  lambda m: image(_Source(m)), html, flags=re.I)
    return re.sub(r"<link\b[^>]*>", stylesheet, html, flags=re.I)


class _Source:
    """An <img> match with its src value under one name, whichever quotes were used."""

    def __init__(self, match: re.Match):
        self.match = match

    def group(self, name):
        if name == "src":
            return next(self.match.group(key) for key in ("a", "b", "c") if self.match.group(key) is not None)
        return self.match.group(name)


def unloaded_references(source: str, markdown: bool = False) -> list:
    """Warnings for what an agent's page asks for and a note's renderer never gives: scripts and the network."""
    warnings = []
    external = re.findall(r"<(?:img|script|link|iframe|frame|source|video|audio|object|embed|image|use|input)\b[^>]*?"
                          r"\b(?:src|href|data|xlink:href|poster)\s*=\s*[\"']?\s*((?:https?:)?//[^\s\"'>]+)", source, re.I)
    external += re.findall(r"(?:@import\s+(?:url\()?|url\()\s*[\"']?((?:https?:)?//[^\s\"')]+)", source, re.I)
    if markdown:
        external += re.findall(r"!\[[^\]]*\]\(\s*<?((?:https?:)?//[^\s)>]+)", source)
    if external:
        more = f" and {len(external) - 1} more" if len(external) > 1 else ""
        warnings.append(f"Not loaded: {external[0][:200]}{more}. Notes are rendered without network access; put pictures "
                        "into the page as data: URIs or inline <svg>.")
    if re.search(r"<script\b", source, re.I) or re.search(r"<[a-z][^>]*\son[a-z]+\s*=", source, re.I):
        warnings.append("The page's scripts did not run: notes are rendered without scripts. Use markdown for math "
                        "($...$ is typeset with KaTeX) and inline <svg> for drawings.")
    return warnings


def asset_cache_dir(environ=None) -> Path:
    """Where the Markdown page's KaTeX and marked files are kept after their first download."""
    environ = os.environ if environ is None else environ
    if environ.get("THOUGHTS_AGENT_NOTES_ASSETS"):
        return Path(environ["THOUGHTS_AGENT_NOTES_ASSETS"]).expanduser()
    if sys.platform == "darwin":
        return Path.home() / "Library/Caches/Thoughts/agent-note-assets"
    if os.name == "nt" and environ.get("LOCALAPPDATA"):
        return Path(environ["LOCALAPPDATA"]) / "Thoughts/agent-note-assets"
    return Path(environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "thoughts/agent-note-assets"


def fetch_asset(url: str) -> bytes:
    """Download one of the pinned files from the npm CDN (https, certificate checked)."""
    request = urllib.request.Request(url, headers={"User-Agent": "thoughts-mcp-agent-notes/1"})
    with urllib.request.urlopen(request, timeout=ASSET_TIMEOUT, context=ssl_context()) as response:
        if urllib.parse.urlsplit(response.geturl()).scheme != "https":
            raise OSError("redirected away from https")
        data = response.read(MAX_ASSET_BYTES + 1)
    if not data or len(data) > MAX_ASSET_BYTES:
        raise OSError("unexpected size")
    return data


def load_asset(name: str, cache: Path | None = None, fetch: Callable[[str], bytes] = fetch_asset) -> tuple[bytes, str] | None:
    """One of the Markdown page's own files, as (bytes, media type): from the cache, else downloaded
    into it. None for any other name, and when the file cannot be had (the page then says so)."""
    if not ASSET_NAME.fullmatch(name):
        return None
    media_type = ASSET_TYPES[Path(name).suffix]
    path = (cache or asset_cache_dir()) / name
    try:
        if 0 < path.stat().st_size <= MAX_ASSET_BYTES:
            return path.read_bytes(), media_type
    except OSError:
        pass
    try:
        data = fetch(ASSET_ORIGIN + name)
    except (OSError, ValueError, http.client.HTTPException) as error:
        log(f"could not download {ASSET_ORIGIN + name}: {error}")
        return None
    with contextlib.suppress(OSError):     # an unwritable cache only costs the next download
        path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(path, data)
    return data, media_type


class RenderServer:
    """The only thing a rendering Chrome can reach: a proxy on 127.0.0.1 that serves one page.

    Chrome is started with this proxy for every request (also for loopback addresses), so the
    page's own URL, http://thoughts-note.invalid/<random>/page.html, and whatever the page asks
    for arrive here. The page and, when @p assets is given, the files it returns under
    <random>/assets/ are served; every other request, to any host, is refused.
    """

    def __init__(self, page: str, nonce: str, assets: Callable[[str], tuple | None] | None = None):
        self.page = page.encode("utf-8")
        self.assets = assets
        self.prefix = "/" + secrets.token_urlsafe(16) + "/"
        self.url = f"http://{RENDER_HOST}{self.prefix}page.html"
        self.policy = content_security_policy(nonce, f"http://{RENDER_HOST}{self.prefix}assets/" if assets else None)
        self.served: list = []       # what was answered: "page.html", "assets/<name>"
        self.refused: list = []      # what was not: "<method> <target>"
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def answer(self, status: int, body: bytes = b"", media_type: str = "text/plain; charset=utf-8", policy: bool = False):
                self.send_response(status)
                self.send_header("Content-Type", media_type)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Referrer-Policy", "no-referrer")
                self.send_header("Connection", "close")
                if policy:
                    self.send_header("Content-Security-Policy", owner.policy)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)
                self.close_connection = True

            def refuse(self):
                owner.refused.append(f"{self.command} {self.path[:300]}")
                self.answer(403, b"Notes are rendered without network access.\n")

            def do_GET(self):
                parts = urllib.parse.urlsplit(self.path)
                if parts.scheme != "http" or parts.netloc != RENDER_HOST or not parts.path.startswith(owner.prefix) or parts.query:
                    return self.refuse()
                name = parts.path[len(owner.prefix):]
                if name == "page.html":
                    owner.served.append(name)
                    return self.answer(200, owner.page, "text/html; charset=utf-8", policy=True)
                if owner.assets is not None and name.startswith("assets/"):
                    found = owner.assets(name[len("assets/"):])
                    if found is not None:
                        owner.served.append(name)
                        return self.answer(200, found[0], found[1])
                    return self.answer(404, b"Not available.\n")
                return self.refuse()

            do_HEAD = do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = do_CONNECT = refuse

            def log_message(self, *args):   # stdout belongs to the MCP protocol; stay silent
                pass

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        self.proxy = f"127.0.0.1:{self._server.server_address[1]}"
        self._thread = threading.Thread(target=self._server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True,
                                        name="agent-note-render")
        self._thread.start()

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def chrome_path(environ=None) -> str:
    environ = os.environ if environ is None else environ
    return environ.get("THOUGHTS_CHROME") or CHROME_DEFAULT


def chrome_command(chrome: str, profile: Path, url: str, height: int, screenshot: Path | None = None,
                   budget_ms: int = 8000, proxy: str | None = None) -> list[str]:
    """One headless pass: --dump-dom (screenshot None) or --screenshot=<file>.

    Chrome gets no network: every request goes to @p proxy (a RenderServer, also for loopback
    addresses, which Chrome would otherwise reach directly) and no host name resolves (the
    proxy's own address is the one exception the resolver rule needs). Without a proxy it is
    pointed at a closed port.
    """
    nice = "/usr/bin/nice" if os.path.exists("/usr/bin/nice") else "nice"
    argv = [nice, "-n", "10", chrome, "--headless=new", f"--user-data-dir={profile}", "--hide-scrollbars",
            f"--force-device-scale-factor={SCALE}", f"--window-size={PAGE_WIDTH},{height}",
            f"--virtual-time-budget={budget_ms}", "--use-mock-keychain", "--no-first-run", "--no-default-browser-check",
            "--disable-extensions", "--disable-sync", "--disable-background-networking", "--disable-component-update",
            "--disable-default-apps", "--mute-audio", "--run-all-compositor-stages-before-draw",
            f"--proxy-server=http://{proxy or '127.0.0.1:9'}", "--proxy-bypass-list=<-loopback>",
            "--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1", "--no-pings", "--disable-quic",
            "--disable-domain-reliability", "--disable-client-side-phishing-detection",
            # The page's address is http on purpose; Chrome must not try https for it first.
            "--disable-features=HttpsUpgrades"]
    argv.append(f"--screenshot={screenshot}" if screenshot is not None else "--dump-dom")
    argv.append(url)
    return argv


def run_process(argv: list[str], timeout: float = CHROME_TIMEOUT) -> tuple[int, bytes, bytes]:
    """Run Chrome in its own process group and kill the whole group on timeout."""
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(OSError):
            os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise AgentNoteError("render_failed", f"Chrome did not finish rendering within {timeout:.0f} s. {RENDER_HINT}") from None
    return process.returncode, stdout, stderr


def _html_attribute(dom: str, name: str) -> str | None:
    match = re.search(r"<html\b[^>]*\b" + re.escape(name) + r"=\"([^\"]*)\"", dom, re.I)
    return match.group(1) if match else None


MAX_LINKS = 64
# PDF links in the phone's reader (thoughts-pdf-link:<Library filename>.pdf?page=<0-based>), as the server checks them.
REMOTE_PDF_LINK_RE = re.compile(r"thoughts-pdf-link:[A-Za-z0-9][A-Za-z0-9_.-]*\.pdf\?page=[0-9]{1,6}"
                                r"(?:&x=-?[0-9]+(?:\.[0-9]+)?)?(?:&y=-?[0-9]+(?:\.[0-9]+)?)?")
# A Library PDF by filename, as Thoughts links write it (optionally behind ../docs-<folder>/), with a 1-based page.
LIBRARY_PDF_HREF_RE = re.compile(r"(?:\.\./)*(?:[^/?#]+/)*([A-Za-z0-9][A-Za-z0-9_.-]*\.pdf)(?:#page=([0-9]{1,6}))?", re.I)
SAME_DOCUMENT_PAGE_RE = re.compile(r"#page=([0-9]{1,6})", re.I)


def measured_links(dom: str) -> list:
    """The links the measuring pass wrote into <html data-thoughts-links>."""
    raw = _html_attribute(dom, "data-thoughts-links")
    if not raw:
        return []
    try:
        values = json.loads(html_lib.unescape(raw))
    except ValueError:
        return []
    links = []
    for value in values if isinstance(values, list) else []:
        if (isinstance(value, list) and len(value) == 6 and isinstance(value[0], str)
                and all(isinstance(v, (int, float)) for v in value[1:5])):
            links.append((value[0], [float(v) for v in value[1:5]], value[5] if isinstance(value[5], str) else ""))
    return links


def library_pdf_dir(environ=None) -> Path:
    environ = os.environ if environ is None else environ
    return Path(environ.get("THOUGHTS_DOCS") or Path.home() / "Thoughts/docs") / "current_pdfs"


def resolve_link(href: str, document_filename: str | None = None, pdf_dir: Path | None = None) -> tuple[str | None, str | None]:
    """The href the phone opens for a link as the agent wrote it, and a warning when it is dropped or doubtful.

    https/http stay as they are. A Library PDF, written <filename>.pdf#page=N (N as printed, 1-based)
    or #page=N for the note's own document, becomes thoughts-pdf-link:<filename>.pdf?page=N-1."""
    href = (href or "").strip()
    if re.match(r"https?://[^\s/?#]+", href, re.I):
        return (href, None) if len(href) <= 2048 else (None, f"Link {href[:60]}… is longer than 2048 characters; dropped.")
    if href.lower().startswith("thoughts-pdf-link:"):
        return (href, None) if REMOTE_PDF_LINK_RE.fullmatch(href) else (None, f"Link {href} is not thoughts-pdf-link:<file>.pdf?page=<n>; dropped.")
    filename, page = None, None
    match = SAME_DOCUMENT_PAGE_RE.fullmatch(href)
    if match:
        if not document_filename:
            return None, f"Link {href} needs document_filename (the note's PDF); dropped."
        filename, page = Path(document_filename).name, match.group(1)
    else:
        match = LIBRARY_PDF_HREF_RE.fullmatch(href)
        if match:
            filename, page = match.group(1), match.group(2)
    if filename is None:
        return None, (f"Link {href} is neither a web address (https://…) nor a Library PDF "
                      "(<Library filename>.pdf#page=N); dropped.")
    target = f"thoughts-pdf-link:{filename}?page={max(0, int(page or 1) - 1)}"
    pdf_dir = pdf_dir or library_pdf_dir()
    warning = None if (pdf_dir / filename).is_file() else (
        f"Link {href}: {filename} is not in this Mac's Library (current_pdfs); the phone shows it as unavailable unless it has it.")
    return target, warning


def note_links(measured: list, css_height: int, document_filename: str | None = None,
               pdf_dir: Path | None = None) -> tuple[list, list]:
    """The note's links: each measured rectangle as fractions of the image, with its resolved href; and warnings."""
    links, warnings, dropped = [], [], set()
    width, height = float(PAGE_WIDTH), float(max(1, css_height))
    for href, (x, y, w, h), label in measured:
        target, warning = resolve_link(href, document_filename, pdf_dir)
        if warning and href not in dropped:
            warnings.append(warning)
            dropped.add(href)
        if target is None:
            continue
        left, top = max(0.0, x), max(0.0, y)
        right, bottom = min(width, x + w), min(height, y + h)
        if right - left < 1 or bottom - top < 1:
            continue  # cut off by the page's width or height
        link = {"rect": [round(left / width, 6), round(top / height, 6), round((right - left) / width, 6),
                         round((bottom - top) / height, 6)], "href": target}
        if label:
            link["label"] = label[:200]
        links.append(link)
    if len(links) > MAX_LINKS:
        warnings.append(f"The note has {len(links)} link rectangles; only the first {MAX_LINKS} are kept.")
        links = links[:MAX_LINKS]
    return links, warnings


@dataclass
class Rendered:
    png: bytes
    css_height: int
    warnings: list
    seconds: float
    # [(href as written, [x, y, width, height] in CSS px of the page, linked text)], from <a href>.
    links: list = field(default_factory=list)


def render(*, html: str | None = None, html_path: str | None = None, markdown: str | None = None,
           runner: Callable = run_process, chrome: str | None = None, assets: Callable[[str], tuple | None] | None = None) -> Rendered:
    """Render HTML or Markdown to a 1200 px wide PNG (480 CSS px at 2.5x), at most 8192 px tall.

    The page is rendered without access to files or the network and without its own scripts
    (see the top of this section). @p assets supplies the Markdown page's KaTeX and marked
    files (load_asset by default).
    """
    chrome = chrome or chrome_path()
    if not (os.path.isfile(chrome) and os.access(chrome, os.X_OK)):
        raise AgentNoteError("renderer_unavailable", f"Google Chrome was not found at {chrome} (set THOUGHTS_CHROME). {RENDER_HINT}")
    started = time.monotonic()
    warnings: list = []
    nonce = new_nonce()
    if markdown is not None:
        page = markdown_page(markdown, nonce)
        warnings.extend(unloaded_references(markdown, markdown=True))
        assets = assets or load_asset
    else:
        if html_path is not None:
            path = readable_path(html_path, "html_path")
            try:
                html = inline_local_files(path.read_text(encoding="utf-8", errors="replace"), path.parent, warnings)
            except OSError as error:
                raise AgentNoteError("invalid_agent_note", f"Cannot read html_path: {error}") from None
        html = html or ""
        warnings.extend(unloaded_references(html))
        page = html_page(html, nonce)
        assets = None
    work = Path(tempfile.mkdtemp(prefix="thoughts-note-render-"))
    try:
        with RenderServer(page, nonce, assets) as server:
            # Pass 1: let scripts settle, read the content height the page wrote into <html>.
            # A short window, so the measured height is the content's, not the viewport's.
            code, stdout, stderr = runner(chrome_command(chrome, work / "profile-1", server.url, 100, proxy=server.proxy), CHROME_TIMEOUT)
            dom = stdout.decode("utf-8", "replace")
            if code != 0 or "<html" not in dom.lower():
                raise AgentNoteError("render_failed", f"Chrome failed to load the page (exit {code}): "
                                                      f"{stderr.decode('utf-8', 'replace').strip()[-400:]} {RENDER_HINT}")
            error = _html_attribute(dom, "data-thoughts-error")
            if error:
                raise AgentNoteError("render_failed", f"{error}. {RENDER_HINT}")
            warning = _html_attribute(dom, "data-thoughts-warning")
            if warning:
                warnings.append(warning)
            measured = _html_attribute(dom, "data-thoughts-height")
            if measured and measured.isdigit() and int(measured) > 0:
                height = int(measured)
            else:
                height = 1600
                warnings.append("The page height could not be measured; rendered 1600 CSS px.")
            width = _html_attribute(dom, "data-thoughts-width")
            if width and width.isdigit() and int(width) > PAGE_WIDTH + 1:
                warnings.append(f"The content is {width} CSS px wide; everything right of {PAGE_WIDTH} px was cut off.")
            height = max(40, height)
            if height > MAX_CSS_HEIGHT:
                warnings.append(f"The page is {height} CSS px tall; it was clipped to the first {MAX_CSS_HEIGHT} px "
                                f"({int(MAX_CSS_HEIGHT * SCALE)} px image). Split it into several notes to show the rest.")
                height = MAX_CSS_HEIGHT
            # Pass 2: the screenshot at the measured height (fresh profile, same page).
            shot = work / "shot.png"
            code, stdout, stderr = runner(chrome_command(chrome, work / "profile-2", server.url, height, screenshot=shot, proxy=server.proxy),
                                          CHROME_TIMEOUT)
            if code != 0 or not shot.is_file():
                raise AgentNoteError("render_failed", f"Chrome did not write the screenshot (exit {code}): "
                                                      f"{stderr.decode('utf-8', 'replace').strip()[-400:]} {RENDER_HINT}")
            png = shot.read_bytes()
        try:
            check_image(png)
        except AgentNoteError as error:
            raise AgentNoteError("render_failed", f"Chrome wrote an unusable screenshot: {error.detail} {RENDER_HINT}") from None
        return Rendered(png, height, warnings, round(time.monotonic() - started, 2), measured_links(dom))
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------- folder

def atomic_write(path: Path, data: bytes | str) -> None:
    """Write via a temporary file in the same directory and rename, so readers never see a partial file."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
        os.replace(temporary, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(temporary)
        raise


def write_json(path: Path, value) -> None:
    atomic_write(path, json.dumps(value, ensure_ascii=False, indent=2) + "\n")


@contextlib.contextmanager
def folder_lock(root: Path):
    """Serializes writers (MCP servers, `watch`) on one folder; held only for local writes."""
    root.mkdir(parents=True, exist_ok=True)
    if fcntl is None:
        yield
        return
    with open(root / ".lock", "a+") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def slug(title: str) -> str:
    text = re.sub(r"[^a-z0-9]+", "-", (title or "").lower()).strip("-")
    return text[:40].rstrip("-") or "note"


def parse_time(value: str | None) -> dt.datetime | None:
    if not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.timezone.utc)


def note_dir_name(note: dict) -> str:
    created = parse_time(note.get("created_at")) or dt.datetime.now(dt.timezone.utc)
    return f"{created.astimezone():%Y%m%d-%H%M%S}-{slug(note.get('title', ''))}-{note['note_id'][:8]}"


def find_note_dir(root: Path, note_id: str) -> Path | None:
    if not root.is_dir():
        return None
    for candidate in sorted(root.glob(f"*-{note_id[:8]}")):
        try:
            if json.loads((candidate / "note.json").read_text(encoding="utf-8")).get("note_id") == note_id:
                return candidate
        except (OSError, ValueError, AttributeError):
            continue
    return None


def write_note(root: Path, note: dict, image: bytes | None, source: tuple[str, str] | None = None) -> Path:
    """note.png|.jpg, note.json (the server record without replies) and note.html|note.md. Call under folder_lock."""
    directory = find_note_dir(root, note["note_id"])
    if directory is None:
        directory = root / note_dir_name(note)
        directory.mkdir(parents=True, exist_ok=True)
    if image is not None:
        atomic_write(directory / ("note" + extension(note.get("media_type", "image/png"))), image)
    if source is not None:
        atomic_write(directory / source[0], source[1])
    write_json(directory / "note.json", {k: v for k, v in note.items() if k != "replies"})
    return directory


def ensure_note_dir(client: Client, root: Path, note_id: str, summary: dict | None = None) -> Path:
    """The note's folder, created from the server's record for a note not sent from this folder."""
    directory = find_note_dir(root, note_id)
    if directory is not None:
        return directory
    try:
        note = client.get_note(note_id)
        note = note.get("note", note)
    except AgentNoteError as error:
        if error.http_status != 404:
            raise
        note = {**(summary or {}), "note_id": note_id}   # deleted or gone: file the reply anyway
    image = None
    if note.get("image_url"):
        with contextlib.suppress(AgentNoteError):
            image = client.download(note["image_url"])
    with folder_lock(root):
        return write_note(root, note, image)


# A page the user sends to the agent, a note and not a reply: from the phone's reader in study
# mode, or from the Thoughts app on this Mac (sent as this Mac, origin.device "mac").
PHONE_PAGE = "phone_page"


def is_phone_page(note: dict) -> bool:
    return isinstance(note.get("origin"), dict) and note["origin"].get("kind") == PHONE_PAGE


def new_phone_pages(client: Client, after: int) -> list[dict]:
    """Pages the user sent with a sequence above ``after``, oldest first: from the phone, and from the
    Thoughts app on this Mac, which sends as this Mac (so every sender counts; agents never send a
    phone_page). The list endpoint, so the Mac never counts as a listening phone."""
    notes = client.list_notes(limit=50).get("notes", [])
    return sorted((n for n in notes if is_phone_page(n) and n.get("sequence", 0) > after), key=lambda n: n.get("sequence", 0))


def get_page(note_id: str, client: Client | None = None, root: Path | None = None) -> dict:
    """A page the phone sent: downloaded into the notes folder once; its metadata and image_path."""
    client = client or default_client()
    root = root or notes_root()
    note = client.get_note(note_id)
    note = note.get("note", note)
    directory = find_note_dir(root, note_id)
    image = next(directory.glob("note.*g"), None) if directory else None
    if image is None:
        data = client.download(note["image_url"]) if note.get("image_url") else None
        with folder_lock(root):
            directory = write_note(root, note, data)
        image = next(directory.glob("note.*g"), None)
    origin = note.get("origin") if isinstance(note.get("origin"), dict) else {}
    return {"note_id": note_id, "title": note.get("title"), "message": note.get("message"), "created_at": note.get("created_at"),
            "phone_page": is_phone_page(note), "document_id": note.get("document_id"), "document_filename": note.get("document_filename"),
            "document_name": origin.get("document_name"), "page": origin.get("page"), "pages": origin.get("pages"),
            "stroke_count": origin.get("stroke_count"), "review_card_id": note.get("review_card_id"),
            "review_question": note.get("review_question"), "media_type": note.get("media_type", "image/png"),
            "image_path": str(image) if image else None}


def reply_files(directory: Path) -> dict:
    """{reply_id: (number, record)} of the replies on disk whose image is complete."""
    found = {}
    for path in directory.glob("reply-*.json"):
        match = re.fullmatch(r"reply-(\d+)\.json", path.name)
        if not match:
            continue
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        local = record.get("local") or {}
        if record.get("reply_id") and (directory / local.get("image", "")).is_file():
            found[record["reply_id"]] = (int(match.group(1)), record)
    return found


def next_reply_number(directory: Path) -> int:
    numbers = [int(m.group(1)) for p in directory.iterdir() if (m := re.fullmatch(r"reply-(\d+)\..*", p.name))]
    return max(numbers, default=0) + 1


def local_entry(root: Path, directory: Path, record: dict, new: bool) -> dict:
    local = record.get("local") or {}
    note = record.get("note") or {}
    entry = {key: record.get(key) for key in ("reply_id", "note_id", "sequence", "created_at", "device_id", "media_type",
                                               "image_width", "image_height", "image_bytes", "stroke_count", "message")}
    photo = record.get("photo") if isinstance(record.get("photo"), dict) else None
    entry.update(note_title=note.get("title"), review_card_id=note.get("review_card_id"), document_id=note.get("document_id"),
                 folder=str(directory), image_path=str(directory / local["image"]),
                 strokes_path=str(directory / local["strokes"]) if local.get("strokes") else None,
                 # A picture sent with the reply ("Send with picture" on the phone: the glasses camera).
                 photo_path=str(directory / local["photo"]) if local.get("photo") else None,
                 photo_media_type=photo.get("media_type") if photo else None,
                 photo_capture=photo.get("capture") if photo else None,
                 json_path=str(directory / local["json"]), new=new)
    return entry


def store_reply(client: Client, root: Path, reply: dict) -> dict:
    """Download one reply into its note's folder unless already there; returns its local entry."""
    note_id = reply["note_id"]
    directory = ensure_note_dir(client, root, note_id, reply.get("note"))
    existing = reply_files(directory).get(reply["reply_id"])
    if existing:
        return local_entry(root, directory, existing[1], new=False)
    image = client.download(reply["image_url"])
    media_type = image_info(image)[0]
    strokes = client.download(reply["strokes_url"]) if reply.get("strokes_url") else None
    photo_record = reply.get("photo") if isinstance(reply.get("photo"), dict) else None
    photo = client.download(photo_record["url"]) if photo_record and photo_record.get("url") else None
    with folder_lock(root):
        existing = reply_files(directory).get(reply["reply_id"])
        if existing:
            return local_entry(root, directory, existing[1], new=False)
        # A record without its image (interrupted write) is completed under the same number.
        number = next((int(m.group(1)) for p in directory.glob("reply-*.json")
                       if (m := re.fullmatch(r"reply-(\d+)\.json", p.name)) and _reply_id(p) == reply["reply_id"]), None)
        number = number or next_reply_number(directory)
        local = {"image": f"reply-{number}{extension(media_type)}", "json": f"reply-{number}.json",
                 "strokes": f"reply-{number}.strokes.json" if strokes is not None else None}
        if photo is not None:
            local["photo"] = f"reply-{number}.photo{extension(image_info(photo)[0])}"
        record = {**reply, "local": local}
        record.setdefault("note", {"note_id": note_id})
        if strokes is not None:
            atomic_write(directory / local["strokes"], strokes)
        if photo is not None:
            atomic_write(directory / local["photo"], photo)
        write_json(directory / local["json"], record)
        atomic_write(directory / local["image"], image)       # last: a reply-*.png means the set is complete
        line = {"reply_id": reply["reply_id"], "note_id": note_id, "sequence": reply.get("sequence"),
                "created_at": reply.get("created_at"), "note_title": record["note"].get("title"),
                "stroke_count": reply.get("stroke_count"), "message": reply.get("message"),
                "image": f"{directory.name}/{local['image']}",
                "strokes": f"{directory.name}/{local['strokes']}" if local["strokes"] else None,
                "photo": f"{directory.name}/{local['photo']}" if local.get("photo") else None,
                "photo_capture": (photo_record or {}).get("capture") if local.get("photo") else None,
                "json": f"{directory.name}/{local['json']}",
                "written_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")}
        journal = root / "replies.jsonl"
        previous = journal.read_bytes() if journal.exists() else b""
        if previous and not previous.endswith(b"\n"):
            previous += b"\n"
        atomic_write(journal, previous + (json.dumps(line, ensure_ascii=False) + "\n").encode("utf-8"))
    return local_entry(root, directory, record, new=True)


def _reply_id(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("reply_id")
    except (OSError, ValueError, AttributeError):
        return None


def read_cursor(root: Path) -> int | None:
    try:
        value = json.loads((root / ".state.json").read_text(encoding="utf-8")).get("cursor")
    except (OSError, ValueError, AttributeError):
        return None
    return value if isinstance(value, int) else None


def advance_cursor(root: Path, cursor: int) -> int:
    """Monotonic: re-read under the lock and keep the larger value (another process may be ahead)."""
    with folder_lock(root):
        current = read_cursor(root)
        if current is not None and current >= cursor:
            return current
        write_json(root / ".state.json", {"cursor": cursor, "updated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")})
        return cursor


def sync_replies(after: int | None = None, wait: float = 0, note_id: str | None = None,
                 client: Client | None = None, root: Path | None = None, max_pages: int = 20,
                 listen: bool = False) -> dict:
    """Pull the reply feed and write every new reply into the folder.

    Without after and note_id it continues from .state.json and advances it; with note_id
    it reads that note's replies after `after` (default 0). Returns {"replies": [entries],
    "cursor": n} where each entry has local paths and new=True when written now. listen=True
    only when an agent waits for these replies (get_note_reply), never for a folder sync.
    """
    client = client or default_client()
    root = root or notes_root()
    stored = read_cursor(root)
    # Only an unfiltered read that starts at or before the stored cursor may advance it.
    uses_global = note_id is None and (after is None or after <= (stored or 0))
    position = after if after is not None else (stored if note_id is None else None)
    position = position or 0
    entries = []
    for _ in range(max_pages):
        page = client.replies(after=position, wait=wait, note_id=note_id, listen=listen and wait > 0)
        replies = sorted(page.get("replies") or [], key=lambda r: r.get("sequence") or 0)
        for reply in replies:
            entries.append(store_reply(client, root, reply))
        cursor = page.get("cursor")
        cursor = cursor if isinstance(cursor, int) else max([r.get("sequence") or 0 for r in replies] + [position])
        if uses_global and cursor > (stored or 0):
            stored = advance_cursor(root, cursor)
        position, wait = max(position, cursor), 0
        if len(replies) < FEED_PAGE:
            break
    return {"replies": entries, "cursor": position}


def feed_cursor(client: Client, conversation_id: str | None = None, max_pages: int = 1000) -> int:
    """The newest sequence of the reply feed (for that conversation): where a watcher starts.

    The feed answers oldest first, 50 at a time, so the first page's cursor is not the newest;
    page until a short page.
    """
    position = 0
    for _ in range(max_pages):
        page = client.replies(after=position, wait=0, conversation_id=conversation_id)
        replies = page.get("replies") or []
        cursor = page.get("cursor")
        cursor = cursor if isinstance(cursor, int) else max([r.get("sequence") or 0 for r in replies] + [position])
        position = max(position, cursor)
        if len(replies) < FEED_PAGE:
            break
    return position


def recent_replies(root: Path, note_id: str | None = None, limit: int = 50) -> list:
    """The newest replies.jsonl entries (deduplicated), as local entries, oldest first."""
    journal = root / "replies.jsonl"
    try:
        lines = journal.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    seen, result = set(), []
    for line in reversed(lines):
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if item.get("reply_id") in seen or (note_id and item.get("note_id") != note_id):
            continue
        seen.add(item.get("reply_id"))
        try:
            directory = root / item["json"].split("/")[0]
            record = json.loads((root / item["json"]).read_text(encoding="utf-8"))
        except (OSError, ValueError, KeyError, AttributeError):
            continue
        result.append(local_entry(root, directory, record, new=False))
        if len(result) >= limit:
            break
    return list(reversed(result))


def stroke_summary(path: str | None, stroke_count: int | None = None) -> dict:
    summary = {"count": stroke_count or 0, "tools": {}, "colors": [], "points": 0}
    if not path:
        return summary
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
        strokes = document.get("strokes") or []
    except (OSError, ValueError, AttributeError):
        return summary
    summary["count"] = len(strokes)
    for stroke in strokes:
        if not isinstance(stroke, dict):
            continue
        tool = str(stroke.get("tool") or "pen")
        summary["tools"][tool] = summary["tools"].get(tool, 0) + 1
        color = str(stroke.get("color") or "").upper()
        if color and color not in summary["colors"]:
            summary["colors"].append(color)
        summary["points"] += len(stroke.get("points") or [])
    for key in ("image_width", "image_height", "source_width", "source_height"):
        if key in document:
            summary[key] = document[key]
    return summary


TRANSIENT = {"server_unreachable", "server_timeout", "server_error", "invalid_response"}


def wait_for_reply(note_id: str, wait_seconds: float = 0, client: Client | None = None, root: Path | None = None,
                   clock: Callable[[], float] = time.monotonic, sleep: Callable[[float], None] = time.sleep) -> dict | None:
    """The newest reply to note_id (local entry), long-polling in steps of <= 60 s until the deadline; None if none."""
    client = client or default_client()
    root = root or notes_root()
    deadline = clock() + max(0.0, wait_seconds)
    position = 0
    while True:
        remaining = deadline - clock()
        wait = max(0, min(60, int(remaining + 0.999)))
        try:
            # Waiting is listening: while it lasts, the phone tells the user this agent will see the reply.
            result = sync_replies(after=position, wait=wait, note_id=note_id, client=client, root=root, listen=True)
        except AgentNoteError as error:
            if error.error_code not in TRANSIENT or deadline - clock() < 3:
                raise
            log(f"reply wait: {error.error_code}; retrying")
            sleep(2)
            continue
        if result["replies"]:
            return max(result["replies"], key=lambda e: e.get("sequence") or 0)
        position = max(position, result["cursor"])
        if deadline - clock() <= 0.05:
            return None


def has_recent_note(root: Path, seconds: float = RECENT_SECONDS) -> bool:
    """True when the folder has a note created (by the server clock in note.json) in the last `seconds`."""
    if not root.is_dir():
        return False
    now = dt.datetime.now(dt.timezone.utc)
    for note_json in root.glob("*/note.json"):
        try:
            created = parse_time(json.loads(note_json.read_text(encoding="utf-8")).get("created_at"))
        except (OSError, ValueError, AttributeError):
            created = None
        if created is None:
            with contextlib.suppress(OSError):
                created = dt.datetime.fromtimestamp(note_json.stat().st_mtime, dt.timezone.utc)
        if created is not None and (now - created).total_seconds() < seconds:
            return True
    return False


FATAL = {"no_credentials", "no_server_config", "unauthorized"}


def watch_loop(stop: threading.Event, client_factory: Callable[[], Client] = default_client, root: Path | None = None,
               only_while_recent: bool = True, wait: int = 25, on_reply: Callable[[dict], None] | None = None) -> None:
    """Long-poll the reply feed and write replies until stopped (or, when only_while_recent, until no note is < 24 h old)."""
    root = root or notes_root()
    backoff = 2.0
    while not stop.is_set():
        if only_while_recent and not has_recent_note(root):
            return
        try:
            result = sync_replies(wait=wait, client=client_factory(), root=root)
            backoff = 2.0
            for entry in result["replies"]:
                if entry.get("new") and on_reply:
                    on_reply(entry)
        except AgentNoteError as error:
            log(f"reply watcher: {error.error_code}: {error.detail}")
            if error.error_code in FATAL:
                return
            stop.wait(backoff)
            backoff = min(60.0, backoff * 2)
        except Exception as error:  # never let the watcher die silently on an unexpected bug
            log(f"reply watcher: {type(error).__name__}: {error}")
            stop.wait(backoff)
            backoff = min(60.0, backoff * 2)


# ---------------------------------------------------------------- sending

def send_note(title: str, *, image_path: str | None = None, html: str | None = None, html_path: str | None = None,
              markdown: str | None = None, message: str | None = None, review_card_id: str | None = None,
              document_id: str | None = None, document_filename: str | None = None, conversation_id: str | None = None,
              note_id: str | None = None, agent: str = "mcp", conversation: dict | None = None,
              client: Client | None = None, root: Path | None = None, runner: Callable = run_process) -> dict:
    """Render (when needed), upload and file one note. Returns the result agents see.

    @p conversation is the calling Claude Code or Codex conversation (server.current_conversation):
    its id, link and title go into the note's origin, where the phone's Agent notes open it.
    """
    inputs = {name: value for name, value in dict(image_path=image_path, html=html, html_path=html_path, markdown=markdown).items()
              if value is not None}
    if len(inputs) != 1:
        raise AgentNoteError("invalid_agent_note", "Give exactly one of image_path, html, html_path or markdown.")
    title = (title or "").strip()
    if not 1 <= len(title) <= 200:
        raise AgentNoteError("invalid_agent_note", "title must be 1 to 200 characters.")
    if message is not None and len(message) > 4000:
        raise AgentNoteError("invalid_agent_note", "message must be at most 4000 characters.")
    note_id = (note_id or str(uuid.uuid4())).lower()
    if not UUID_RE.match(note_id):
        raise AgentNoteError("invalid_agent_note", "note_id must be a UUID.")
    warnings, source, render_seconds, links = [], None, None, []
    if image_path is not None:
        # What is sent leaves this computer: private locations are refused (readable_path).
        path = readable_path(image_path, "image_path")
        try:
            image = path.read_bytes()
        except OSError as error:
            raise AgentNoteError("invalid_agent_note", f"Cannot read image_path: {error}") from None
    else:
        if html_path is not None:
            path = readable_path(html_path, "html_path")
            try:
                source = ("note.html", path.read_text(encoding="utf-8", errors="replace"))
            except OSError as error:
                raise AgentNoteError("invalid_agent_note", f"Cannot read html_path: {error}") from None
        elif html is not None:
            source = ("note.html", html)
        else:
            source = ("note.md", markdown)
        rendered = render(html=html, html_path=html_path, markdown=markdown, runner=runner)
        image, render_seconds = rendered.png, rendered.seconds
        warnings.extend(rendered.warnings)
        links, link_warnings = note_links(rendered.links, rendered.css_height, document_filename)
        warnings.extend(link_warnings)
    media_type, width, height = check_image(image)
    origin = {"agent": agent}
    if conversation:
        conversation_id = conversation.get("conversation_id") or conversation_id
        for key, value in (("conversation_agent", conversation.get("agent")), ("conversation_url", conversation.get("conversation_url")),
                           ("conversation_title", conversation.get("title"))):
            if isinstance(value, str) and value:
                origin[key] = value
    if conversation_id:
        origin["conversation_id"] = conversation_id
    body = {"note_id": note_id, "title": title, "image": {"media_type": media_type, "data_base64": base64.b64encode(image).decode("ascii")},
            "origin": origin}
    for key, value in dict(message=message, document_id=document_id, document_filename=document_filename,
                           review_card_id=review_card_id).items():
        if value is not None:
            body[key] = value
    if links:
        body["links"] = links
    client = client or default_client()
    root = root or notes_root()
    response = client.create_note(body)
    note = response.get("note") or {}
    if links and "links" not in note:
        warnings.append("The Thoughts server does not support links yet (agent_note_links_v1); the note was sent without them.")
    note.setdefault("note_id", note_id)
    note.setdefault("title", title)
    note.setdefault("media_type", media_type)
    delivery = response.get("delivery") or {"live": False, "listeners": []}
    # Whether an agent of this conversation waits for the reply (servers before agent_note_listening_v1: unknown).
    reply_listening = response.get("reply_listening") or {"state": "unknown", "listeners": []}
    with folder_lock(root):
        directory = write_note(root, note, image, source)
    image_file = directory / ("note" + extension(media_type))
    live = bool(delivery.get("live"))
    if live:
        next_step = (f"The note is open on the phone; call get_note_reply(note_id=\"{note_id}\", wait_seconds=300) "
                     f"or watch {directory} for reply-*.png.")
    else:
        next_step = ("No phone is listening right now. Ask the user to open Thoughts on the phone (or tap the "
                     "\"Agent note\" notification); a phone that is not listening receives the note within about 15 minutes. "
                     f"Then call get_note_reply(note_id=\"{note_id}\", wait_seconds=300) or watch {directory} for reply-*.png.")
    if reply_listening.get("state") != "listening":
        next_step += (" Nothing in this conversation is listening for the user's reply yet, and the phone shows the user "
                      "that no agent is listening. Before you end your turn, start thoughts-watch-replies as a background "
                      "monitor (one line per reply or page; it listens for this conversation's notes), so a reply that "
                      "comes after your turn still reaches you.")
    result = {"note_id": note_id, "title": note.get("title", title), "created_at": note.get("created_at"),
              "review_card_id": note.get("review_card_id", review_card_id), "document_id": note.get("document_id", document_id),
              "review_question": note.get("review_question"),
              "delivery": {"live": live, "listeners": delivery.get("listeners") or []},
              # An agent of this conversation that will hear the user's reply (thoughts-watch-replies, get_note_reply).
              "reply_listening": {"state": reply_listening.get("state") or "unknown",
                                  "listeners": reply_listening.get("listeners") or []},
              # Per phone that stored the note (servers before per-device delivery send none).
              "deliveries": note.get("deliveries") or [],
              "folder": str(directory), "image_path": str(image_file),
              "image": {"media_type": media_type, "width": width, "height": height, "bytes": len(image)},
              "links": [{"href": link["href"], "label": link.get("label", "")} for link in (note.get("links") or [])],
              "warnings": warnings, "next_step": next_step}
    if render_seconds is not None:
        result["render_seconds"] = render_seconds
    return result


def list_notes(review_card_id: str | None = None, document_id: str | None = None, limit: int = 20,
               client: Client | None = None, root: Path | None = None) -> dict:
    client = client or default_client()
    root = root or notes_root()
    notes = client.list_notes(document_id=document_id, review_card_id=review_card_id, limit=limit).get("notes") or []
    for note in notes:
        directory = find_note_dir(root, note.get("note_id", "")) if note.get("note_id") else None
        note["local_folder"] = str(directory) if directory else None
    return {"notes": notes, "count": len(notes), "folder": str(root)}


def list_replies(note_id: str | None = None, after: int | None = None, client: Client | None = None,
                 root: Path | None = None) -> dict:
    """Pull new replies into the folder; with neither argument, also list the newest replies already there."""
    client = client or default_client()
    root = root or notes_root()
    result = sync_replies(after=after, wait=0, note_id=note_id, client=client, root=root)
    replies = result["replies"]
    if note_id is None and after is None:
        new = {entry["reply_id"] for entry in replies if entry.get("new")}
        replies = [{**entry, "new": entry["reply_id"] in new} for entry in recent_replies(root)]
    return {"replies": replies, "cursor": result["cursor"], "folder": str(root)}


# ---------------------------------------------------------------- command line

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Thoughts agent notes: send pictures to the phone, collect annotated replies.")
    commands = parser.add_subparsers(dest="command", required=True)
    watch = commands.add_parser("watch", help="write replies into the folder as they arrive")
    watch.add_argument("--once", action="store_true", help="fetch what is new once and exit")
    send = commands.add_parser("send", help="send a note")
    send.add_argument("--title", required=True)
    content = send.add_mutually_exclusive_group(required=True)
    content.add_argument("--image")
    content.add_argument("--html-file")
    content.add_argument("--markdown-file")
    send.add_argument("--message")
    send.add_argument("--card", help="review card ID")
    send.add_argument("--document-id")
    replies = commands.add_parser("replies", help="fetch and list replies")
    replies.add_argument("--note")
    args = parser.parse_args(argv)
    try:
        if args.command == "watch":
            if args.once:
                result = sync_replies()
                for entry in result["replies"]:
                    print(json.dumps(entry, ensure_ascii=False), flush=True)
                return 0
            log(f"watching for replies into {notes_root()} (Ctrl-C stops)")
            stop = threading.Event()
            try:
                watch_loop(stop, only_while_recent=False, on_reply=lambda e: print(json.dumps(e, ensure_ascii=False), flush=True))
            except KeyboardInterrupt:
                stop.set()
            return 0
        if args.command == "send":
            markdown = readable_path(args.markdown_file, "--markdown-file").read_text(encoding="utf-8") if args.markdown_file else None
            result = send_note(args.title, image_path=args.image, html_path=args.html_file, markdown=markdown, message=args.message,
                               review_card_id=args.card, document_id=args.document_id, agent="cli")
        else:
            result = list_replies(note_id=args.note)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except AgentNoteError as error:
        print(json.dumps(error.as_dict(), ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
