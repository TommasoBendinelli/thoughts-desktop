#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""thoughts-reply-waker: wake the Claude Code session that should answer the owner's reply, on its own machine.

One waker runs on each machine (on a Linux server as a systemd user service, on a Mac as a LaunchAgent; the same
code). On Windows the Thoughts desktop app runs this copy itself (tools/waker, shell/wakersupervisor.cpp) with the
Python it installs: the device token comes from Windows Credential Manager through the Thoughts tools (agent_notes),
and the app stops it by closing its standard input (stop_on_stdin_eof). This copy is vendored from the Thoughts
connectors' reply-agent/thoughts_reply_waker.py (43acadb) with the Windows parts added (see "this computer" below).

Events (from every agent and every machine):
  reply    the owner's reply to an agent note
  page     a page the owner sent to the agents (origin.kind phone_page); one with origin.start_session true asks
           for a NEW session (the phone's "Start a new study session"): the waker that starts sessions (the fallback's,
           where the glasses captures arrive) starts one in the background (claude --bg) with the page's text as its
           prompt, claims the page for it and tells the owner with a note; only for origin.where "computer" (or
           none): "server" is the Thoughts server's, and "one_computer" (Send to Agent's "New session on this
           computer") is for the Thoughts app of the computer origin.computer names, never for a waker
  capture  a new glasses photo or video (only where the capture folders are, i.e. the server)

Who answers is decided by the Thoughts server (protocol/agent-notes.md "Wakers", agent_note_wakers_v1), from what
every waker registers: each waker beats every 30 s with the sessions of its machine it can wake. For each reply or
page the server names a wake_target in GET /v1/agent-notes/handling/<id>: the owner's per-note choice (answer_by)
when set; else the note's own session; else a topic owner's session; else the fallback session (a session on the
server named for it), whose waker answers as the responder. A waker acts only on targets on its own machine:

  1. claim the item on the server for the target session (the first claim wins, so nobody answers twice);
  2. if the session is live (a running process holds it): wait a little while it is busy (idle includes "shell", a
     finished turn with background shells such as a Monitor watcher), then send it a pointer with a one-tool
     messenger (`claude -p --tools SendMessage`), and confirm delivery by watching its transcript grow; one attempt
     takes at most attempt_seconds (3 minutes), and a pointer an earlier attempt sent is never sent again;
  3. if it is not live: take a per-session lock, check again, trim it with /compact when it has been idle for 2 hours
     or more, then resume it (`claude -p --resume <id>`) in its own folder with its last permission mode, model and
     effort, its own settings, and a spending cap, with the same pointer;
  4. if it cannot be woken: hand the item over (POST .../handover) and decide again, so the next session the server
     names (a topic owner, then the fallback session) answers it;
  5. after a delivery, check back after answer_wait_minutes (30): answered (a note of that session after the item
     arrived) ends it; a session that never took it up (no tool call about it in its transcript) is handed over; one
     that took it up gets taken_up_hours (4) more.
When nobody can take an item (the fallback session itself failed or did not answer, claude cannot sign in), the
waker tells the owner once with a short agent note (origin.kind "waker_notice"), instead of starting a new session.
Every outcome is a line in logs/wakes.jsonl.

The pointer only points (note id, reply id): the woken session reads the owner's words itself with its Thoughts tool
(get_note_reply / get_note), the trusted path, and that tool hands it the answering rules. A live session is never
resumed headlessly (its answer would become a branch the live process never sees), and a server Remote Control
session that has ended is not revived: it is registered as not wakeable, so its replies go to the fallback session.

While a computer is asleep its waker does not beat; the server keeps that computer's sessions' items from the
fallback for fallback_after_hours (8), then the fallback session answers them.

This computer: Claude Code keeps one file per running session in <claude_dir>/sessions/<pid>.json (claude_dir:
CLAUDE_CONFIG_DIR, else ~/.claude, the .claude folder of the user profile on Windows) and the folders it trusts in
~/.claude.json. On Windows a process is looked up with OpenProcess (os.kill(pid, 0) would end it there), files are
locked with msvcrt, a process tree is stopped with taskkill, children start without a console window, and text to
and from claude is UTF-8.

Commands:
  run                 the service
  status              registration, queue, the last wakes
  sessions [--json]   the sessions this waker would register now (nothing is sent)
  decide ITEM_ID      what the waker would do for a reply or page (reads only; claims nothing)
  pause | unpause     hold deliveries (events keep queueing)
  forget              remove this machine's registration from the server (after uninstalling)

Home: REPLY_WAKER_HOME (default ~/thoughts-reply-waker): config.json, state.db, logs/wakes.jsonl, messenger/, PAUSE,
waker.lock (held while `run` runs: a second one in the same home exits with code 3).
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import datetime as dt
import hashlib
import importlib.util
import json
import logging
import logging.handlers
import ntpath
import os
import posixpath
import re
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from zoneinfo import ZoneInfo

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None
try:
    import msvcrt
except ImportError:  # everywhere else
    msvcrt = None

WINDOWS = os.name == "nt"
# Windows process creation flags (subprocess has the names only on Windows).
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_NO_WINDOW = 0x08000000
# Windows: the byte a lock file's lock covers, far past what the file holds, so its text stays readable.
LOCK_OFFSET = 1 << 30
# The programs a running Claude Code session is on Windows: the native installer's, or node or bun for npm's.
CLAUDE_PROGRAMS = ("claude.exe", "node.exe", "bun.exe")


# --- this computer -----------------------------------------------------------------------------------------------


def text_options() -> dict:
    """Text to and from the programs this waker runs is UTF-8 (on Windows the default would be the ANSI code page,
    which cannot hold every title or pointer)."""
    return {"encoding": "utf-8", "errors": "replace"}


def no_window() -> dict:
    """On Windows: a console program started from here gets no window of its own."""
    return {"creationflags": CREATE_NO_WINDOW} if WINDOWS else {}


def own_group() -> dict:
    """Popen options that put a child in a group of its own, which terminate() stops with everything it started."""
    if WINDOWS:
        return {"creationflags": CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP}
    return {"start_new_session": True}


def path_module():
    return ntpath if WINDOWS else posixpath


def folder_key(path) -> str:
    """A folder as this waker compares folders: normalized, and on Windows without case and with \\ throughout
    (Claude Code may write C:/Users/... in ~/.claude.json and C:\\Users\\... in its session files)."""
    paths = path_module()
    return paths.normcase(paths.normpath(str(path)))


def inside(folder, parent) -> bool:
    """Whether folder is parent or a folder below it."""
    if not folder or not parent:
        return False
    paths = path_module()
    a, b = folder_key(folder), folder_key(parent)
    return a == b or a.startswith(b.rstrip(paths.sep) + paths.sep)


def folder_and_parents(folder) -> list[str]:
    paths = path_module()
    current, found = folder_key(folder), []
    while current not in found:
        found.append(current)
        current = paths.dirname(current)
    return found


def windows_process(pid: int) -> tuple[bool, str | None]:
    """Windows: whether a process with this id runs, and its program's file name (lower case) when Windows says."""
    import ctypes  # noqa: PLC0415 (Windows only)
    from ctypes import wintypes  # noqa: PLC0415

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    query_limited_information, synchronize = 0x1000, 0x00100000
    access_denied, wait_timeout = 5, 0x102
    handle = kernel32.OpenProcess(query_limited_information | synchronize, False, pid)
    if not handle:
        # Access denied: it exists (another user's, or elevated). Anything else (an id nobody has): it does not.
        return ctypes.get_last_error() == access_denied, None
    try:
        if kernel32.WaitForSingleObject(handle, 0) != wait_timeout:
            return False, None  # it has ended (its handle is signalled)
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return True, ntpath.basename(buffer.value).lower()
        return True, None
    finally:
        kernel32.CloseHandle(handle)


def lock_file(handle, blocking: bool = True) -> bool:
    """An exclusive lock on an open file (flock; on Windows msvcrt's lock of one byte at LOCK_OFFSET). False when
    blocking is False and another process holds it."""
    if fcntl is not None:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
            return True
        except BlockingIOError:
            return False
    if msvcrt is not None:
        descriptor = handle.fileno()
        while True:
            os.lseek(descriptor, LOCK_OFFSET, os.SEEK_SET)
            try:
                msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
                return True
            except OSError:
                if not blocking:
                    return False
                time.sleep(0.2)
    return True


def unlock_file(handle) -> None:
    if fcntl is not None:
        fcntl.flock(handle, fcntl.LOCK_UN)
    elif msvcrt is not None:
        os.lseek(handle.fileno(), LOCK_OFFSET, os.SEEK_SET)
        with contextlib.suppress(OSError):
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)

VERSION = "1"
# The exit code of `run` when another waker runs in the same home.
ALREADY_RUNNING = 3
HOME = Path(os.environ.get("REPLY_WAKER_HOME", Path.home() / "thoughts-reply-waker")).expanduser()
WAKERS_CAPABILITY = "agent_note_wakers_v1"
PHONE_PAGE = "phone_page"
WAKER_NOTICE = "waker_notice"
CAPTURE_NAME = re.compile(r"^(IMG|VID)_(\d{8})_(\d{6})(?:_[^/]*)?\.(jpg|jpeg|mp4)$", re.IGNORECASE)
# What claude prints when this machine's login can no longer be used: nothing it runs works until the owner signs in.
AUTH_FAILURE = re.compile(
    r"failed to authenticate|claude auth login|not logged in|please run /login|invalid api key|"
    r"oauth token (?:has )?expired|authentication_error",
    re.IGNORECASE,
)
UUID_TEXT = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
# Environment of the process that started the waker which must not reach the claude it starts (an agent shell).
DROPPED_ENV_PREFIXES = ("CLAUDE_CODE_", "CLAUDECODE", "CLAUDE_AGENT_SDK", "CLAUDE_PID", "CLAUDE_EFFORT", "ANTHROPIC_")
# Modes in which a resumed session does not need its folder's permission rules (they would be ignored in a folder
# Claude Code does not trust): bypassPermissions allows every tool, auto decides with its classifier.
TRUST_FREE_MODES = ("bypassPermissions", "auto")
log = logging.getLogger("reply-waker")

DEFAULTS = {
    # This machine's name on the server ("server", "laptop"); default: the short host name.
    "machine": None,
    # The Thoughts server. On the server: its local URL and the connector's token file. On a computer with the
    # Thoughts app: leave both empty and set agent_notes (the Thoughts tools' agent_notes.py; mac_agent_notes is
    # its older name), which finds the server URL and the device token as the computer's note tools do (macOS:
    # the Keychain; Windows: Credential Manager).
    "thoughts_url": None,
    "token_file": None,
    "agent_notes": None,
    "mac_agent_notes": None,
    # claude: a path, or "auto" (macOS: the newest claude bundled with the Claude app; Windows: the native
    # installer's %USERPROFILE%\.local\bin\claude.exe; else claude on PATH).
    "claude": "auto",
    # null: CLAUDE_CONFIG_DIR, else ~/.claude (and ~/.claude.json beside it).
    "claude_dir": None,
    "claude_json": None,
    "desktop_sessions_dir": (
        "%APPDATA%/Claude/claude-code-sessions" if WINDOWS else "~/Library/Application Support/Claude/claude-code-sessions"
    ),
    # The session that answers what has no other home (on the server, a session named for it). Only one waker
    # names one.
    "fallback_session_id": None,
    "fallback_title": None,
    # While this waker is offline (the machine asleep), the server keeps this machine's items this long.
    "fallback_after_hours": 8,
    # Closed sessions of these kinds may be resumed headlessly ("unknown": never seen live by this waker).
    "resume_closed_kinds": ["desktop", "terminal", "headless", "unknown"],
    # Never wake these sessions (e.g. the old generic reply agent's), nor sessions in these folders.
    "exclude_sessions": [],
    "exclude_folders": ["~/thoughts-reply-agent/work"],
    # Tests: wake only these sessions (empty: every session).
    "only_sessions": [],
    # A session the owner archived in the Claude app is left alone: its replies go to the fallback.
    "wake_archived": False,
    "budget_usd": 3.0,
    "idle_trim_hours": 2,
    "trim_instructions": (
        "Keep only what you need to answer the owner's replies to your notes: the owner's answering rules, how to "
        "work in this project (folders, commands, tools, conventions) and the open tasks and promises. Drop the rest."
    ),
    "messenger_model": "haiku",
    "messenger_budget_usd": 0.1,
    # One message attempt (waiting while busy, the messenger, the transcript check) ends within attempt_seconds. A
    # busy session is messaged after busy_wait_seconds anyway: the message waits in its queue for the turn to end.
    "attempt_seconds": 180,
    "busy_wait_seconds": 60,
    "messenger_timeout_seconds": 90,
    "confirm_seconds": 60,
    "resume_timeout_seconds": 1800,
    "max_parallel": 3,
    "max_attempts": 3,
    # After a delivery: a session that has not taken the item up within answer_wait_minutes is handed over; one that
    # took it up (a tool call about it) but sent no answer gets taken_up_hours, then the owner is told.
    "answer_wait_minutes": 30,
    "taken_up_hours": 4,
    "recheck_minutes": 10,
    # The fallback waker watches another waker's claim this long past those limits before it tells the owner.
    "handover_grace_minutes": 15,
    # Tell the owner (one short agent note per item, and one per sign-in outage) when nobody could take an item.
    "notify_owner": True,
    # New sessions the owner starts from the phone (a page with origin.start_session): started by this waker when
    # start_sessions is true (null: the fallback's waker), in start_session_folder (Claude Code must trust it: run
    # claude there once and accept), as background sessions (claude --bg), with this permission mode and model.
    # auto, as Remote Control gives the sessions the owner starts from the Claude app: nobody is there to answer
    # permission prompts, and the classifier still checks each action.
    "start_sessions": None,
    "start_session_folder": "~/study-sessions",
    "start_session_permission_mode": "auto",
    "start_session_model": None,
    "start_session_name": "Study session",
    "start_timeout_seconds": 90,
    # While claude cannot sign in on this machine, items wait and are tried again this often.
    "auth_retry_minutes": 10,
    "heartbeat_seconds": 30,
    "full_registration_seconds": 300,
    "notes_refresh_seconds": 300,
    "poll_wait_seconds": 50,
    "max_event_age_hours": 24,
    "max_sessions": 400,
    # Extra arguments for every resume (tests: --strict-mcp-config --mcp-config <file>).
    "resume_extra_args": [],
    # Folders put before PATH for claude (os.pathsep between them; a list works too).
    "path": (
        "~/.local/bin;%APPDATA%/npm"
        if WINDOWS
        else "/opt/homebrew/bin:/usr/local/bin:~/.local/bin:~/.npm-global/bin:/usr/bin:/bin:/usr/sbin:/sbin"
    ),
    # The time zone of the times in notes and logs; null: this computer's own.
    "local_zone": None,
    # The service's log: this file (rotated at 1 MB, three kept) instead of standard error.
    "log_file": None,
    # The service stops when its standard input ends: the Thoughts app that started it closed it, or ended.
    "stop_on_stdin_eof": False,
    # Glasses captures (only where the capture folders exist).
    "photos_dir": None,
    "videos_dir": None,
    "transcribe": "~/.local/bin/transcribe",
    "ffmpeg": "ffmpeg",
    "capture_grace_seconds": 30,
    "capture_scan_seconds": 5,
    "max_capture_age_hours": 6,
    "max_video_seconds": 300,
    "max_video_frames": 40,
}


def expand(value) -> Path | None:
    return None if value in (None, "") else Path(os.path.expandvars(str(value))).expanduser()


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def iso(value: float | dt.datetime | None) -> str | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        value = dt.datetime.fromtimestamp(value, dt.UTC)
    return value.astimezone(dt.UTC).isoformat().replace("+00:00", "Z")


def parse_time(value) -> dt.datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.UTC)


@dataclass
class Config:
    home: Path
    values: dict = field(default_factory=dict)

    @classmethod
    def load(cls, home: Path = HOME) -> "Config":
        values = dict(DEFAULTS)
        path = home / "config.json"
        if path.is_file():
            values.update(json.loads(path.read_text(encoding="utf-8")))
        return cls(home, values)

    def __getitem__(self, key):
        return self.values[key]

    def get(self, key, default=None):
        return self.values.get(key, default)

    def path(self, key: str) -> Path | None:
        value = expand(self.values.get(key))
        if value is not None and not value.is_absolute():
            value = self.home / value
        return value

    @property
    def machine(self) -> str:
        name = self.values.get("machine") or socket.gethostname().split(".")[0]
        return re.sub(r"[^A-Za-z0-9_.-]", "-", name)[:64] or "machine"

    @property
    def is_fallback(self) -> bool:
        return bool(self.values.get("fallback_session_id") or self.values.get("fallback_title"))

    @property
    def starts_sessions(self) -> bool:
        chosen = self.values.get("start_sessions")
        return self.is_fallback if chosen is None else bool(chosen)

    @property
    def state_file(self) -> Path:
        return self.home / "state.db"

    @property
    def pause_file(self) -> Path:
        return self.home / "PAUSE"

    @property
    def wakes_log(self) -> Path:
        return self.home / "logs" / "wakes.jsonl"

    @property
    def messenger_dir(self) -> Path:
        return self.home / "messenger"

    @property
    def locks_dir(self) -> Path:
        return self.home / "locks"

    @property
    def claude_dir(self) -> Path:
        """Claude Code's own folder: as configured, else CLAUDE_CONFIG_DIR, else ~/.claude."""
        configured = self.path("claude_dir")
        if configured is not None:
            return configured
        return expand(os.environ.get("CLAUDE_CONFIG_DIR")) or Path.home() / ".claude"

    @property
    def claude_json(self) -> Path:
        """Claude Code's global settings, with the folders it trusts: as configured, else .claude.json in
        CLAUDE_CONFIG_DIR when that is set, else ~/.claude.json."""
        configured = self.path("claude_json")
        if configured is not None:
            return configured
        folder = expand(os.environ.get("CLAUDE_CONFIG_DIR"))
        return folder / ".claude.json" if folder is not None else Path.home() / ".claude.json"

    def zone(self) -> dt.tzinfo:
        """local_zone, else this computer's own zone (Windows has no time zone database for ZoneInfo)."""
        name = self.values.get("local_zone")
        if name:
            try:
                return ZoneInfo(name)
            except (KeyError, ValueError, OSError):
                log.warning("local_zone %r is unknown here; using this computer's time zone", name)
        return dt.datetime.now().astimezone().tzinfo or dt.UTC


# --- the Thoughts server ------------------------------------------------------------------------------


class ThoughtsError(RuntimeError):
    pass


class Thoughts:
    """JSON client of the Thoughts server. The token is read when needed and never logged."""

    def __init__(self, url: str, token: callable, ssl_context=None):
        self.url = url.rstrip("/")
        self._token = token
        # On a computer: the note tool's context (python.org builds have no CA certificates of their own).
        self._ssl_context = ssl_context

    @classmethod
    def from_config(cls, config: Config) -> "Thoughts":
        url = (os.environ.get("THOUGHTS_SERVER_URL") or config["thoughts_url"] or "").strip()
        token_file = config.path("token_file")
        if token_file is not None:
            if not url:
                raise SystemExit("config: thoughts_url is required with token_file")
            return cls(url, lambda: token_file.read_text(encoding="utf-8").strip())
        module_path = config.path("agent_notes") or config.path("mac_agent_notes")
        if module_path is None:
            raise SystemExit("config: set token_file (the server) or agent_notes (a computer with the Thoughts app)")
        spec = importlib.util.spec_from_file_location("waker_mac_agent_notes", module_path)
        module = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(module_path.parent))
        # Registered before it runs: a dataclass in the module looks its module up in sys.modules.
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        # THOUGHTS_SERVER_URL / THOUGHTS_TOKEN, else server.json + the Keychain or Credential Manager.
        try:
            settings = module.load_config()
        except Exception as error:  # noqa: BLE001 (its AgentNoteError: no server, not signed in; never the token)
            raise SystemExit(f"no Thoughts sign-in: {getattr(error, 'detail', None) or error}") from None
        context = module.ssl_context() if hasattr(module, "ssl_context") else None
        return cls(url or settings.server_url, lambda: settings.token, context)

    def request(
        self,
        method: str,
        path: str,
        params: dict | None = None,
        body: dict | None = None,
        timeout: float = 30,
        allow: tuple = (),
    ) -> tuple[int, dict]:
        query = {key: value for key, value in (params or {}).items() if value is not None}
        url = self.url + path + (("?" + urllib.parse.urlencode(query)) if query else "")
        headers = {"Authorization": "Bearer " + self._token(), "Accept": "application/json"}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout, context=self._ssl_context) as response:  # noqa: S310 (configured URL)
                raw = response.read().decode("utf-8")
                return response.status, (json.loads(raw) if raw.strip() else {})
        except urllib.error.HTTPError as error:
            payload = error.read()
            if error.code in allow:
                try:
                    return error.code, json.loads(payload.decode("utf-8"))
                except ValueError:
                    return error.code, {}
            raise ThoughtsError(
                f"{method} {path}: HTTP {error.code} {payload[:300].decode('utf-8', 'replace')}"
            ) from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise ThoughtsError(f"{method} {path}: {error}") from None

    def get(self, path, params=None, timeout=30) -> dict:
        return self.request("GET", path, params, timeout=timeout)[1]

    def capabilities(self) -> set[str]:
        return set(self.get("/v1/health").get("capabilities") or [])

    def register(self, machine: str, body: dict) -> dict:
        return self.request("PUT", f"/v1/agent-notes/wakers/{machine}", body=body)[1]

    def forget(self, machine: str) -> int:
        return self.request("DELETE", f"/v1/agent-notes/wakers/{machine}", allow=(404,))[0]

    def handling(self, item_id: str) -> dict:
        return self.get(f"/v1/agent-notes/handling/{item_id}")

    def claim(self, item_id: str, body: dict) -> tuple[int, dict]:
        return self.request("POST", f"/v1/agent-notes/handling/{item_id}/claim", body=body, allow=(409,))

    def handover(self, item_id: str, machine: str, reason: str) -> dict:
        return self.request(
            "POST", f"/v1/agent-notes/handling/{item_id}/handover", body={"machine": machine, "reason": reason[:200]}
        )[1]

    def replies(self, after: int, wait: float = 0, listen: bool = False) -> dict:
        params = {"after": after, "wait": wait, "responder": "true"}
        if listen:
            params["listen"] = "true"
        return self.get("/v1/agent-notes/replies", params, timeout=wait + 30)

    def pages(self, after: int | None, wait: float = 0) -> dict:
        return self.get("/v1/agent-notes/pages", {"after": after, "wait": wait}, timeout=wait + 30)

    def notes(self, sent_by_me: bool | None = None, limit: int = 200) -> list[dict]:
        params = {"limit": limit}
        if sent_by_me is not None:
            params["sent_by_me"] = "true" if sent_by_me else "false"
        return self.get("/v1/agent-notes", params).get("notes") or []

    def note(self, note_id: str) -> dict:
        return self.get(f"/v1/agent-notes/{note_id}")

    def owners(self) -> list[dict]:
        return self.get("/v1/agent-notes/owners").get("owners") or []

    def send_note(self, body: dict) -> dict:
        return self.request("POST", "/v1/agent-notes", body=body, timeout=60)[1]


# --- the sessions of this machine -----------------------------------------------------------------------


@dataclass
class LiveSession:
    session_id: str
    pid: int
    cwd: str
    name: str | None
    status: str  # idle | busy (busy also covers "waiting": a dialog is open)
    kind: str
    started_at: float | None
    updated_at: float | None
    version: str | None
    raw_status: str | None = None  # as Claude Code wrote it: idle | shell | busy | waiting


def live_status(raw) -> str:
    """idle or busy. Claude Code writes "shell" for an idle session with background shells running (a Monitor watcher,
    say): its turn is over, so a message reaches it at once. "busy" (a turn runs) and "waiting" (a dialog is open)
    are busy."""
    return "idle" if raw in ("idle", "shell") else "busy"


@dataclass
class SessionInfo:
    session_id: str
    title: str | None
    folder: str | None
    kind: str
    live: LiveSession | None
    transcript: Path | None
    last_active: float | None
    permission_mode: str | None = None
    model: str | None = None
    effort: str | None = None
    archived: bool = False
    wakeable: bool = False
    why_not: str | None = None

    @property
    def state(self) -> str:
        return "closed" if self.live is None else self.live.status

    def record(self) -> dict:
        return {
            "session_id": self.session_id,
            "title": self.title,
            "folder": self.folder,
            "state": self.state,
            "kind": self.kind,
            "last_active_at": iso(self.last_active),
            "wakeable": self.wakeable,
        }


def pid_alive(pid, proc_start=None) -> bool:
    """The process runs (and is the one that wrote the registry entry, when its start time can be compared)."""
    if not isinstance(pid, int) or pid <= 0:
        return False
    if WINDOWS:
        # Never os.kill(pid, 0) here: on Windows it ends the process. Its start time is not compared (Claude Code's
        # procStart there is not ps' lstart); a reused id is told apart by its program instead.
        running, program = windows_process(pid)
        return running and (program is None or program in CLAUDE_PROGRAMS)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    if proc_start is None:
        return True
    if sys.platform.startswith("linux"):
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            return str(fields[19]) == str(proc_start)
        except (OSError, IndexError):
            return True
    key = (pid, str(proc_start))
    if key in _STARTED_AS_RECORDED:  # a running process keeps its start time: ask ps once per process
        return True
    try:
        started = subprocess.run(
            ["ps", "-o", "lstart=", "-p", str(pid)],
            capture_output=True,
            text=True,
            timeout=10,
            env={"LC_ALL": "C", "TZ": "UTC", "PATH": "/bin:/usr/bin"},
            **text_options(),
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return True
    same = not started or " ".join(started.split()) == " ".join(str(proc_start).split())
    if same and started:
        _STARTED_AS_RECORDED.add(key)
    return same


_STARTED_AS_RECORDED: set[tuple[int, str]] = set()


def session_kind(entry: dict) -> str:
    """desktop (the Claude app), remote_control (a Remote Control child), terminal (claude in a terminal), headless."""
    entrypoint = entry.get("entrypoint")
    if entrypoint == "claude-desktop":
        return "desktop"
    if entrypoint == "cli":
        return "terminal"
    if entry.get("bridgeSessionId") or "claude-rc" in str(entry.get("tmux") or ""):
        return "remote_control"
    return "headless"


def scan_live(
    sessions_dir: Path, exclude_pids: set[int] = frozenset(), exclude_cwds: tuple = ()
) -> dict[str, LiveSession]:
    """The sessions a running process holds: ~/.claude/sessions/<pid>.json whose pid runs."""
    found: dict[str, LiveSession] = {}
    if not sessions_dir.is_dir():
        return found
    for path in sessions_dir.glob("*.json"):
        try:
            entry = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        session_id, pid = entry.get("sessionId"), entry.get("pid")
        if not isinstance(session_id, str) or pid in exclude_pids or not pid_alive(pid, entry.get("procStart")):
            continue
        cwd = str(entry.get("cwd") or "")
        if any(inside(cwd, folder) for folder in exclude_cwds):
            continue
        live = LiveSession(
            session_id,
            pid,
            cwd,
            entry.get("name") or None,
            live_status(entry.get("status")),
            session_kind(entry),
            (entry.get("startedAt") or 0) / 1000 or None,
            (entry.get("statusUpdatedAt") or entry.get("updatedAt") or 0) / 1000 or None,
            entry.get("version"),
            entry.get("status") if isinstance(entry.get("status"), str) else None,
        )
        previous = found.get(session_id)
        if previous is None or (live.updated_at or 0) > (previous.updated_at or 0):
            found[session_id] = live
    return found


def project_folder(cwd: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


def find_transcript(projects_dir: Path, session_id: str, cwd: str | None = None) -> Path | None:
    if cwd:
        path = projects_dir / project_folder(cwd) / f"{session_id}.jsonl"
        if path.is_file():
            return path
    return next(iter(sorted(projects_dir.glob(f"*/{session_id}.jsonl"))), None)


def transcript_facts(path: Path, max_bytes: int = 8 * 1024 * 1024) -> dict:
    """From the end of a transcript: the last permission mode, model, effort, title and folder."""
    facts: dict = {}
    try:
        size = path.stat().st_size
        facts["mtime"] = path.stat().st_mtime
    except OSError:
        return facts
    chunk = 512 * 1024
    while True:
        start = max(0, size - chunk)
        with path.open("rb") as handle:
            handle.seek(start)
            data = handle.read(size - start)
        lines = data.split(b"\n")
        if start > 0:
            lines = lines[1:]  # the first line is cut
        for raw in reversed(lines):
            if not raw.strip():
                continue
            try:
                entry = json.loads(raw)
            except ValueError:
                continue
            if not isinstance(entry, dict):
                continue
            kind = entry.get("type")
            if "permission_mode" not in facts and isinstance(entry.get("permissionMode"), str):
                facts["permission_mode"] = entry["permissionMode"]
            if kind == "assistant" and "model" not in facts:
                model = entry.get("requestedModel") or (entry.get("message") or {}).get("model")
                if isinstance(model, str) and model and not model.startswith("<"):
                    facts["model"] = model
                if isinstance(entry.get("effort"), str):
                    facts["effort"] = entry["effort"]
            if kind == "custom-title" and "title" not in facts and isinstance(entry.get("customTitle"), str):
                facts["title"] = entry["customTitle"]
            if kind == "agent-name" and "agent_name" not in facts and isinstance(entry.get("agentName"), str):
                facts["agent_name"] = entry["agentName"]
            if "cwd" not in facts and isinstance(entry.get("cwd"), str):
                facts["cwd"] = entry["cwd"]
            if "entrypoint" not in facts and isinstance(entry.get("entrypoint"), str):
                facts["entrypoint"] = entry["entrypoint"]
        done = all(key in facts for key in ("permission_mode", "model", "cwd"))
        if done or start == 0 or chunk >= max_bytes:
            return facts
        chunk *= 4


class Sessions:
    """What this waker knows about the sessions of its machine: the registry, transcripts, the Claude app's store, and
    what it saw itself (the kind of a session that has ended)."""

    def __init__(self, config: Config, state: "State | None" = None):
        self.config = config
        self.state = state
        claude_dir = config.claude_dir
        self.sessions_dir = claude_dir / "sessions"
        self.projects_dir = claude_dir / "projects"
        self.own_pids: set[int] = set()
        self._facts: dict[str, tuple[float, int, dict]] = {}
        self._desktop: dict[str, tuple[float, dict]] = {}
        self._trust: tuple[float, set[str]] | None = None
        self.cache_lock = threading.Lock()

    def facts(self, transcript: Path) -> dict:
        """transcript_facts, read again only when the transcript changed."""
        try:
            stat = transcript.stat()
        except OSError:
            return {}
        key = str(transcript)
        with self.cache_lock:
            cached = self._facts.get(key)
        if cached and cached[0] == stat.st_mtime and cached[1] == stat.st_size:
            return cached[2]
        facts = transcript_facts(transcript)
        with self.cache_lock:
            self._facts[key] = (stat.st_mtime, stat.st_size, facts)
        return facts

    def desktop(self) -> dict[str, dict]:
        """desktop_sessions, each file read again only when it changed."""
        store = self.config.path("desktop_sessions_dir")
        found: dict[str, dict] = {}
        if store is None or not store.is_dir():
            return found
        for path in store.glob("*/*/local_*.json"):
            try:
                mtime = path.stat().st_mtime
            except OSError:
                continue
            with self.cache_lock:
                cached = self._desktop.get(str(path))
            if cached is None or cached[0] != mtime:
                try:
                    entry = json.loads(path.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    continue
                cached = (
                    mtime,
                    {
                        key: entry.get(key)
                        for key in (
                            "cliSessionId",
                            "title",
                            "cwd",
                            "permissionMode",
                            "model",
                            "effort",
                            "isArchived",
                            "lastActivityAt",
                        )
                    },
                )
                with self.cache_lock:
                    self._desktop[str(path)] = cached
            if isinstance(cached[1].get("cliSessionId"), str):
                found[cached[1]["cliSessionId"]] = cached[1]
        return found

    def live(self) -> dict[str, LiveSession]:
        excluded = tuple(str(path) for path in [self.config.messenger_dir, *self.excluded_folders()])
        found = scan_live(self.sessions_dir, set(self.own_pids), excluded)
        if self.state is not None:
            for live in found.values():
                self.state.remember_session(live)
        return found

    def excluded_folders(self) -> list[Path]:
        return [path for path in (expand(value) for value in self.config["exclude_folders"] or []) if path]

    def allowed(self, session_id: str) -> bool:
        only = self.config["only_sessions"] or []
        return session_id not in (self.config["exclude_sessions"] or []) and (not only or session_id in only)

    def info(
        self, session_id: str, live: dict[str, LiveSession] | None = None, desktop: dict[str, dict] | None = None
    ) -> SessionInfo | None:
        live = self.live() if live is None else live
        desktop = self.desktop() if desktop is None else desktop
        running = live.get(session_id)
        app = desktop.get(session_id) or {}
        remembered = self.state.remembered(session_id) if self.state is not None else None
        cwd = (running.cwd if running else None) or app.get("cwd") or (remembered or {}).get("cwd")
        transcript = find_transcript(self.projects_dir, session_id, cwd)
        if running is None and transcript is None:
            return None
        facts = self.facts(transcript) if transcript is not None else {}
        cwd = cwd or facts.get("cwd")
        kind = running.kind if running else (remembered or {}).get("kind") or ("desktop" if app else "unknown")
        last_active = facts.get("mtime")
        if running is not None and running.updated_at:
            last_active = max(last_active or 0, running.updated_at)
        info = SessionInfo(
            session_id=session_id,
            title=(running.name if running else None)
            or app.get("title")
            or facts.get("title")
            or facts.get("agent_name")
            or (remembered or {}).get("title"),
            folder=cwd,
            kind=kind,
            live=running,
            transcript=transcript,
            last_active=last_active,
            permission_mode=app.get("permissionMode") or facts.get("permission_mode"),
            model=app.get("model") or facts.get("model"),
            effort=app.get("effort") or facts.get("effort"),
            archived=bool(app.get("isArchived")),
        )
        info.wakeable, info.why_not = self.wakeable(info)
        return info

    def wakeable(self, info: SessionInfo) -> tuple[bool, str | None]:
        if not self.allowed(info.session_id):
            return False, "excluded by the waker's configuration"
        folder = info.folder or ""
        if any(inside(folder, path) for path in self.excluded_folders()):
            return False, "in an excluded folder"
        if info.live is not None:
            return (True, None) if info.live.name else (False, "a running session without a name cannot be messaged")
        if info.archived and not self.config["wake_archived"]:
            return False, "archived in the Claude app"
        if info.kind not in (self.config["resume_closed_kinds"] or []):
            return False, f"a closed {info.kind} session is not resumed here"
        if info.transcript is None or not info.folder or not Path(info.folder).is_dir():
            return False, "its transcript or folder is gone"
        if info.permission_mode not in TRUST_FREE_MODES and not self.trusted(info.folder):
            # claude -p in a folder nobody trusted ignores the folder's permission rules: the session would run
            # with fewer permissions than it had (e.g. its Thoughts tools refused), so it is not resumed.
            return (
                False,
                "its folder is not trusted, so its own permission rules would not apply in "
                f"{info.permission_mode or 'default'} mode",
            )
        return True, None

    def trusted(self, folder: str) -> bool:
        """Whether Claude Code trusts the folder or a folder above it (~/.claude.json, projects[...] with
        hasTrustDialogAccepted)."""
        path = self.config.claude_json
        try:
            mtime = path.stat().st_mtime
        except OSError:
            return False
        with self.cache_lock:
            cached = self._trust
        if cached is None or cached[0] != mtime:
            try:
                projects = json.loads(path.read_text(encoding="utf-8")).get("projects") or {}
            except (OSError, ValueError, AttributeError):
                projects = {}
            accepted = {
                folder_key(key)
                for key, value in projects.items()
                if isinstance(value, dict) and value.get("hasTrustDialogAccepted")
            }
            cached = (mtime, accepted)
            with self.cache_lock:
                self._trust = cached
        return any(candidate in cached[1] for candidate in folder_and_parents(folder))


# --- state -------------------------------------------------------------------------------------------------


class State:
    """state.db: events (each once), cursors, the sessions seen live, captures. Thread-safe."""

    SCHEMA = """
    CREATE TABLE IF NOT EXISTS events (
        id TEXT PRIMARY KEY, kind TEXT NOT NULL, payload TEXT NOT NULL, event_at TEXT, status TEXT NOT NULL,
        attempts INTEGER NOT NULL DEFAULT 0, created_at REAL NOT NULL, not_before REAL, finished_at REAL,
        target TEXT, detail TEXT);
    CREATE INDEX IF NOT EXISTS events_status ON events (status, not_before, created_at);
    CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS sessions (session_id TEXT PRIMARY KEY, kind TEXT NOT NULL, cwd TEXT, title TEXT,
        first_seen REAL NOT NULL, last_live REAL NOT NULL);
    CREATE TABLE IF NOT EXISTS reply_captures (capture TEXT PRIMARY KEY, reply_id TEXT NOT NULL, seen_at REAL NOT NULL);
    CREATE TABLE IF NOT EXISTS captures (name TEXT PRIMARY KEY, path TEXT NOT NULL, first_seen REAL NOT NULL,
        size INTEGER NOT NULL, stable_since REAL, status TEXT NOT NULL);
    """

    def __init__(self, path: Path | str):
        self.lock = threading.RLock()
        self.db = sqlite3.connect(str(path), check_same_thread=False, isolation_level=None, timeout=30)
        self.db.row_factory = sqlite3.Row
        if str(path) != ":memory:":
            self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript(self.SCHEMA)

    @contextlib.contextmanager
    def transaction(self):
        with self.lock:
            self.db.execute("BEGIN IMMEDIATE")
            try:
                yield self.db
            except BaseException:
                self.db.execute("ROLLBACK")
                raise
            self.db.execute("COMMIT")

    def get(self, key: str, default=None):
        with self.lock:
            row = self.db.execute("SELECT value FROM kv WHERE key = ?", (key,)).fetchone()
        return json.loads(row["value"]) if row else default

    def put(self, key: str, value, db=None) -> None:
        statement = "INSERT INTO kv (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"
        with self.lock:
            (db or self.db).execute(statement, (key, json.dumps(value)))

    def add_event(self, event: dict, status: str = "queued", detail: str | None = None, db=None) -> bool:
        statement = (
            "INSERT OR IGNORE INTO events (id, kind, payload, event_at, status, created_at, detail) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"
        )
        values = (
            event["id"],
            event["kind"],
            json.dumps(event["payload"]),
            event.get("at"),
            status,
            time.time(),
            detail,
        )
        with self.lock:
            return (db or self.db).execute(statement, values).rowcount == 1

    def ready(self, busy: set[str], now: float | None = None) -> sqlite3.Row | None:
        """The oldest event that may be handled now (not one whose session has a delivery running)."""
        now = time.time() if now is None else now
        with self.lock:
            rows = self.db.execute(
                "SELECT * FROM events WHERE status IN ('queued', 'waiting', 'watching', 'awaiting_answer') "
                "AND (not_before IS NULL OR not_before <= ?) ORDER BY created_at, rowid LIMIT 50",
                (now,),
            ).fetchall()
        return next((row for row in rows if row["target"] not in busy), None)

    def delivering(self) -> list[sqlite3.Row]:
        with self.lock:
            return self.db.execute("SELECT * FROM events WHERE status = 'delivering'").fetchall()

    def delete(self, key: str) -> None:
        with self.lock:
            self.db.execute("DELETE FROM kv WHERE key = ?", (key,))

    def delete_prefix(self, prefix: str) -> None:
        with self.lock:
            self.db.execute("DELETE FROM kv WHERE substr(key, 1, ?) = ?", (len(prefix), prefix))

    def update(self, event_id: str, **values) -> None:
        if not values:
            return
        columns = ", ".join(f"{key} = ?" for key in values)
        with self.lock:
            self.db.execute(f"UPDATE events SET {columns} WHERE id = ?", (*values.values(), event_id))

    def event(self, event_id: str) -> sqlite3.Row | None:
        with self.lock:
            return self.db.execute("SELECT * FROM events WHERE id = ?", (event_id,)).fetchone()

    def requeue_interrupted(self) -> list[str]:
        with self.transaction() as db:
            rows = db.execute("SELECT id FROM events WHERE status = 'delivering'").fetchall()
            db.execute(
                "UPDATE events SET status = 'queued', not_before = NULL, detail = 'interrupted' "
                "WHERE status = 'delivering'"
            )
            return [row["id"] for row in rows]

    def remember_session(self, live: LiveSession) -> None:
        now = time.time()
        with self.lock:
            self.db.execute(
                "INSERT INTO sessions (session_id, kind, cwd, title, first_seen, last_live) VALUES (?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(session_id) DO UPDATE SET kind = excluded.kind, cwd = excluded.cwd, "
                "title = COALESCE(excluded.title, sessions.title), last_live = excluded.last_live",
                (live.session_id, live.kind, live.cwd, live.name, now, now),
            )

    def remembered(self, session_id: str) -> dict | None:
        with self.lock:
            row = self.db.execute("SELECT * FROM sessions WHERE session_id = ?", (session_id,)).fetchone()
        return dict(row) if row else None

    def remembered_since(self, seconds: float) -> list[str]:
        with self.lock:
            return [
                row["session_id"]
                for row in self.db.execute(
                    "SELECT session_id FROM sessions WHERE last_live >= ? ORDER BY last_live DESC",
                    (time.time() - seconds,),
                )
            ]

    def counts(self) -> dict:
        with self.lock:
            return {
                row["status"]: row["n"]
                for row in self.db.execute("SELECT status, count(*) AS n FROM events GROUP BY status")
            }

    def recent(self, limit: int = 15) -> list[sqlite3.Row]:
        with self.lock:
            return self.db.execute(
                "SELECT * FROM events ORDER BY created_at DESC, rowid DESC LIMIT ?", (limit,)
            ).fetchall()

    def record_reply_capture(self, capture: str, reply_id: str) -> None:
        with self.lock:
            self.db.execute(
                "INSERT OR IGNORE INTO reply_captures (capture, reply_id, seen_at) VALUES (?, ?, ?)",
                (capture_key(capture), reply_id, time.time()),
            )

    def reply_for_capture(self, name: str) -> str | None:
        with self.lock:
            rows = self.db.execute("SELECT * FROM reply_captures ORDER BY seen_at DESC LIMIT 500").fetchall()
        return next((row["reply_id"] for row in rows if same_capture(row["capture"], name)), None)


# --- events ------------------------------------------------------------------------------------------------


def capture_time(name: str) -> dt.datetime | None:
    """IMG_YYYYMMDD_HHMMSS_* / VID_...: the glasses name captures in UTC."""
    match = CAPTURE_NAME.match(name)
    if not match:
        return None
    try:
        return dt.datetime.strptime(match.group(2) + match.group(3), "%Y%m%d%H%M%S").replace(tzinfo=dt.UTC)
    except ValueError:
        return None


def capture_key(value: str | None) -> str:
    name = Path(str(value or "")).name
    return re.sub(r"\.(jpe?g|mp4|png)$", "", name, flags=re.IGNORECASE).lower()


def same_capture(a: str, b: str) -> bool:
    a, b = capture_key(a), capture_key(b)
    return bool(a and b) and (a == b or a.startswith(b + "_") or b.startswith(a + "_"))


def reply_event(reply: dict) -> dict:
    note = reply.get("note") or {}
    return {
        "id": f"reply:{reply['reply_id']}",
        "kind": "reply",
        "at": reply.get("created_at"),
        "payload": {
            "reply_id": reply["reply_id"],
            "note_id": reply.get("note_id"),
            "note_title": note.get("title"),
            "created_at": reply.get("created_at"),
            "has_photo": bool(reply.get("photo")),
        },
    }


def page_event(note: dict) -> dict:
    origin = note.get("origin") if isinstance(note.get("origin"), dict) else {}
    payload = {"note_id": note["note_id"], "title": note.get("title"), "created_at": note.get("created_at")}
    if origin.get("start_session") is True:
        # The phone's "Start a new study session": the page's text is the new session's prompt; agent "claude"
        # (or none) is this waker's, "codex" is for the owner's Thoughts app on a computer.
        payload["start_session"] = True
        payload["agent"] = str(origin.get("agent") or "claude")
        payload["where"] = str(origin.get("where") or "computer")
    elif origin.get("copy_for_agent") is True:
        # Thoughts' Send to Agent: Copy for agent's text for one conversation (origin.conversation_id), as if pasted.
        payload["copy_for_agent"] = True
        payload["conversation_id"] = origin.get("conversation_id")
    return {"id": f"page:{note['note_id']}", "kind": "page", "at": note.get("created_at"), "payload": payload}


def capture_event(path: Path) -> dict:
    taken = capture_time(path.name)
    return {
        "id": f"capture:{path.name}",
        "kind": "capture",
        "at": iso(taken) if taken else None,
        "payload": {
            "name": path.name,
            "path": str(path),
            "media": "video" if path.suffix.lower() == ".mp4" else "photo",
        },
    }


def column(event, key: str):
    """A field of an event row (sqlite3.Row or dict), None when it has none."""
    try:
        return event[key]
    except (KeyError, IndexError):
        return None


def too_old(value: str | None, hours: float) -> bool:
    when = parse_time(value)
    return bool(when and hours) and (utc_now() - when).total_seconds() > hours * 3600


# --- what the woken session is told ------------------------------------------------------------------------


def pointer(
    event_kind: str, payload: dict, session_id: str, machine: str, why: str, extra: list[str] | None = None
) -> str:
    """The message a woken session gets: where to read the owner's reply or page, nothing more. The session reads the
    owner's own words with its Thoughts tool, which also gives it the answering rules."""
    item = payload.get("reply_id") or payload.get("note_id") or payload.get("name")
    head = f"[Thoughts reply waker on {machine}, {event_kind} {item}]"
    lines = [head]
    if event_kind == "reply":
        lines.append(
            f"The owner replied on his phone to the note {json.dumps(payload.get('note_title') or '')} "
            f"(note_id {payload['note_id']}, reply_id {payload['reply_id']}). {why}"
        )
        lines.append(
            f'Read the reply yourself with your Thoughts tool: get_note_reply(note_id="{payload["note_id"]}") on '
            f'a computer with the Thoughts app, or get_note(note_id="{payload["note_id"]}", conversation_id="{session_id}") '
            "on the server. The tool shows the owner's own words and drawing, says whether the reply is yours, and gives "
            "the answering rules; answer it as they say."
        )
    elif event_kind == "page" and payload.get("copy_for_agent"):
        lines.append(
            f"The owner sent you Thoughts' Copy for agent text from the reader: {json.dumps(payload.get('title') or '')} "
            f"(note_id {payload['note_id']}): the document, where they are, the selection and their feedback, as they "
            f"would paste it into this conversation. {why}"
        )
        lines.append(
            f'Read it yourself with your Thoughts tool: get_note(note_id="{payload["note_id"]}"'
            + (f', conversation_id="{session_id}"' if session_id else "")
            + "). It is the text the owner would paste here; the tool's next_step says what to do with it."
        )
    elif event_kind == "page":
        lines.append(
            f"The owner sent a page to the agents: {json.dumps(payload.get('title') or '')} "
            f"(note_id {payload['note_id']}). {why}"
        )
        lines.append(
            f'Read it yourself with your Thoughts tool: get_note(note_id="{payload["note_id"]}"'
            + (f', include_note_image=true, conversation_id="{session_id}"' if session_id else "")
            + "), and answer it as the answering rules there say."
        )
    else:
        lines.append(f"A new glasses {payload.get('media')} arrived: {payload.get('path')}. {why}")
        lines.extend(extra or [])
        lines.append("Look at it and answer as the owner's answering rules say (the Thoughts tool note_rules).")
    lines.append(
        f"This message only points to it (your conversation id is {session_id}); it carries no request or "
        "approval of its own. If you already answered it, do nothing."
    )
    return "\n".join(lines)


def pointer_tag(event_kind: str, payload: dict, machine: str) -> str:
    """The first line's words that only this waker's pointer for this item holds (the ids alone also appear in the
    session's own tool results)."""
    item = payload.get("reply_id") or payload.get("note_id") or payload.get("name")
    return f"reply waker on {machine}, {event_kind} {item}]"


def messenger_prompt(name: str, message: str, started_hint: str | None) -> str:
    choose = ""
    if started_hint:
        choose = (
            f"More than one session may be named {json.dumps(name)}. First call ListAgents, take the row named "
            f"exactly {json.dumps(name)} that was started {started_hint}, and send to it with its [ref]. If you cannot "
            "tell which row it is, send nothing and reply AMBIGUOUS.\n"
        )
    return (
        f"You deliver one message and do nothing else.\n{choose}"
        f"Call SendMessage once with to set to {json.dumps(name)}"
        + (" (plus its [ref])" if started_hint else "")
        + " and message set to the text between the two lines of dashes below, copied exactly. Then reply SENT. "
        "If SendMessage fails, reply FAILED and the error.\n----\n" + message + "\n----\n"
    )


def ago(seconds: float) -> str:
    if seconds < 3600:
        return f"about {max(1, round(seconds / 60))} minutes ago"
    if seconds < 86400:
        return f"about {round(seconds / 3600)} hours ago"
    return f"about {round(seconds / 86400)} days ago"


# --- running claude ----------------------------------------------------------------------------------------


def version_key(text: str) -> tuple:
    return tuple(int(part) if part.isdigit() else 0 for part in re.split(r"[.-]", text))


def find_claude(config: Config) -> str:
    configured = config["claude"]
    if configured and configured != "auto":
        return str(expand(configured))
    if WINDOWS:
        # The native installer's, then PATH (claude.exe, or npm's claude.cmd), then npm's own folder.
        native = Path.home() / ".local" / "bin" / "claude.exe"
        if native.is_file():
            return str(native)
        found = shutil.which("claude")
        if found:
            return found
        npm = expand("%APPDATA%/npm/claude.cmd")
        return str(npm) if npm is not None and npm.is_file() else "claude"
    bundles = sorted(
        Path.home().glob("Library/Application Support/Claude/claude-code/*/*/claude.app/Contents/MacOS/claude"),
        key=lambda path: version_key(path.parents[4].name),
    )
    if bundles:
        return str(bundles[-1])
    for candidate in (Path.home() / ".local/bin/claude", Path("/usr/local/bin/claude")):
        if candidate.exists():
            return str(candidate)
    return "claude"


def claude_env(config: Config) -> dict:
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith(DROPPED_ENV_PREFIXES)}
    configured = config["path"]
    listed = configured if isinstance(configured, list) else str(configured or "").split(os.pathsep)
    parts = [str(expand(part)) for part in listed if part]
    parts += [part for part in os.environ.get("PATH", "").split(os.pathsep) if part and part not in parts]
    env["PATH"] = os.pathsep.join(parts)
    env["DISABLE_AUTOUPDATER"] = "1"  # never let a wake update Claude Code
    if not WINDOWS:  # Claude Code on Windows finds the home folder in USERPROFILE; HOME would only confuse Git's tools
        env.setdefault("HOME", str(Path.home()))
    return env


def claude_version(binary: str, config: Config) -> str:
    try:
        out = subprocess.run(
            [binary, "--version"],
            capture_output=True,
            text=True,
            timeout=60,
            env=claude_env(config),
            stdin=subprocess.DEVNULL,
            **text_options(),
            **no_window(),
        ).stdout.strip()
        return out.split()[0] if out else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def permission_args(mode: str | None) -> list[str]:
    mode = mode if mode in ("default", "acceptEdits", "plan", "auto", "dontAsk", "bypassPermissions") else "default"
    return ["--permission-mode", mode]


@dataclass
class Run:
    code: int | None
    answer: dict
    stderr: str
    seconds: float

    @property
    def ok(self) -> bool:
        return self.code == 0 and not self.answer.get("is_error")

    @property
    def cost(self) -> float | None:
        value = self.answer.get("total_cost_usd")
        return round(value, 6) if isinstance(value, (int, float)) else None

    def problem(self) -> str:
        return str(self.answer.get("result") or self.stderr or f"exit {self.code}").strip()[-400:]

    @property
    def auth_failed(self) -> bool:
        """claude could not sign in (an expired login): no run on this machine works until the owner signs in."""
        return bool(AUTH_FAILURE.search(f"{self.answer.get('result') or ''}\n{self.stderr or ''}"))


class Claude:
    def __init__(self, config: Config, sessions: Sessions, stop: threading.Event):
        self.config = config
        self.sessions = sessions
        self.stop = stop
        self.binary = find_claude(config)
        self.version = claude_version(self.binary, config)

    def run(self, argv: list[str], message: str, cwd: Path | str, timeout: float) -> Run:
        started = time.time()
        child = subprocess.Popen(
            [self.binary, *argv],
            cwd=str(cwd),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=claude_env(self.config),
            **text_options(),
            **own_group(),
        )
        self.sessions.own_pids.add(child.pid)
        try:
            try:
                out, err = child.communicate(message, timeout=timeout)
            except subprocess.TimeoutExpired:
                terminate(child)
                out, err = child.communicate()
                err = (err or "") + f"\n(stopped after {timeout:.0f} s)"
        finally:
            self.sessions.own_pids.discard(child.pid)
        try:
            answer = json.loads(out) if out.strip() else {}
        except ValueError:
            answer = {"result": out[-2000:]}
        return Run(
            child.returncode, answer if isinstance(answer, dict) else {}, err or "", round(time.time() - started, 1)
        )

    def message(self, name: str, mode: str | None, text: str, started_hint: str | None, timeout: float = 180) -> Run:
        tools = "SendMessage,ListAgents" if started_hint else "SendMessage"
        argv = [
            "-p",
            "--no-session-persistence",
            "--model",
            self.config["messenger_model"],
            "--tools",
            tools,
            "--strict-mcp-config",
            "--output-format",
            "json",
            "--max-budget-usd",
            str(self.config["messenger_budget_usd"]),
            *permission_args(mode),
        ]
        folder = self.config.messenger_dir
        folder.mkdir(parents=True, exist_ok=True)
        return self.run(argv, messenger_prompt(name, text, started_hint), folder, timeout)

    def resume(self, info: SessionInfo, text: str, budget: float) -> Run:
        argv = [
            "-p",
            "--resume",
            info.session_id,
            "--output-format",
            "json",
            *permission_args(info.permission_mode),
            "--max-budget-usd",
            str(budget),
        ]
        if info.model:
            argv += ["--model", info.model]
        if info.effort:
            argv += ["--effort", info.effort]
        argv += [str(arg) for arg in self.config["resume_extra_args"] or []]
        return self.run(argv, text, info.folder, self.config["resume_timeout_seconds"])


@dataclass
class Started:
    ok: bool
    short_id: str = ""
    session_id: str = ""
    output: str = ""


BACKGROUNDED = re.compile(r"backgrounded\s*·\s*([0-9a-f]{6,})")


def start_background(claude: "Claude", name: str, folder: Path, prompt: str) -> Started:
    """A new background session (claude --bg) in folder with prompt; its id from claude agents --json."""
    config = claude.config
    argv = [claude.binary, "--bg", "-n", name, *permission_args(config["start_session_permission_mode"])]
    if config["start_session_model"]:
        argv += ["--model", str(config["start_session_model"])]
    argv.append(prompt)
    try:
        done = subprocess.run(argv, cwd=str(folder), capture_output=True, text=True, env=claude_env(config),
                              timeout=float(config["start_timeout_seconds"]), stdin=subprocess.DEVNULL,
                              **text_options(), **no_window())
    except (OSError, subprocess.TimeoutExpired) as error:
        return Started(False, output=f"{type(error).__name__}: {error}")
    output = ((done.stdout or "") + (done.stderr or "")).strip()
    match = BACKGROUNDED.search(output)
    if done.returncode != 0 or not match:
        return Started(False, output=output[-600:])
    short_id = match.group(1)
    session_id = ""
    for _ in range(10):
        with contextlib.suppress(OSError, subprocess.TimeoutExpired, ValueError):
            listed = subprocess.run([claude.binary, "agents", "--json"], capture_output=True, text=True, timeout=30,
                                    env=claude_env(config), stdin=subprocess.DEVNULL, **text_options(), **no_window())
            for row in json.loads(listed.stdout or "[]"):
                if isinstance(row, dict) and (row.get("id") == short_id or str(row.get("sessionId", "")).startswith(short_id)):
                    session_id = str(row.get("sessionId") or "")
        if session_id:
            break
        time.sleep(1)
    return Started(True, short_id, session_id, output[-600:])


def stop_background(claude: "Claude", short_id: str) -> None:
    """Stops and removes a background session this waker started (claude stop, claude rm)."""
    for verb in ("stop", "rm"):
        with contextlib.suppress(OSError, subprocess.TimeoutExpired):
            subprocess.run([claude.binary, verb, short_id], capture_output=True, text=True, timeout=30,
                           env=claude_env(claude.config), stdin=subprocess.DEVNULL, **text_options(), **no_window())


def terminate(child: subprocess.Popen) -> None:
    if WINDOWS:
        # The process and everything it started (taskkill /T); Windows has no SIGTERM for a windowless console.
        with contextlib.suppress(OSError, subprocess.SubprocessError):
            subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], capture_output=True, timeout=30,
                           stdin=subprocess.DEVNULL, **no_window())
        with contextlib.suppress(subprocess.TimeoutExpired):
            child.wait(20)
        return
    with contextlib.suppress(ProcessLookupError):
        os.killpg(child.pid, signal.SIGTERM)
    try:
        child.wait(20)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(child.pid, signal.SIGKILL)


@contextlib.contextmanager
def session_lock(folder: Path, session_id: str):
    """Held while a session is woken: never two wakes of one session at once, from any process."""
    folder.mkdir(parents=True, exist_ok=True)
    with (folder / f"{session_id}.lock").open("a") as handle:
        lock_file(handle)
        try:
            yield
        finally:
            unlock_file(handle)


def grew_with(path: Path | None, offset: int, needle: str) -> str | None:
    """Whether the transcript got the pointer after offset: "read" (a user turn holds it), "queued" (enqueued)."""
    if path is None or not path.is_file():
        return None
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            data = handle.read()
    except OSError:
        return None
    found = None
    for raw in data.split(b"\n"):
        if needle.encode() not in raw:
            continue
        try:
            entry = json.loads(raw)
        except ValueError:
            continue
        if entry.get("type") == "user":
            return "read"
        if entry.get("type") == "queue-operation":
            found = "queued"
    return found


def taken_up(path: Path | None, offset: int, ids: list[str]) -> bool:
    """Whether the session, after offset, made a tool call about the item (get_note_reply / get_note with its note id,
    say): it is working on it. The pointer itself (a user turn) names the ids too, so only the assistant's tool calls
    count."""
    ids = [value for value in ids if value]
    if path is None or not ids or not path.is_file():
        return False
    try:
        with path.open("rb") as handle:
            handle.seek(offset)
            data = handle.read()
    except OSError:
        return False
    for raw in data.split(b"\n"):
        if not any(value.encode() in raw for value in ids):
            continue
        try:
            entry = json.loads(raw)
        except ValueError:
            continue
        if not isinstance(entry, dict) or entry.get("type") != "assistant":
            continue
        for part in (entry.get("message") or {}).get("content") or []:
            if isinstance(part, dict) and part.get("type") == "tool_use":
                text = json.dumps(part.get("input"))
                if any(value in text for value in ids):
                    return True
    return False


def answer_note(handling: dict, note: dict | None, notes: list[dict]) -> str | None:
    """The note that answered the item, or None: the server's own word (claim role "answered", an inferred answer),
    else a note of the claimant (same device and conversation) sent after the item arrived. A claim for a
    conversation is answered by that conversation's next note on any document, as the server infers for a
    conversation that covers an item: a session often answers on the document the answer is about (replies on
    one paper answered on a note about another were once reported to the owner as unanswered). A claim without a
    conversation needs a note on the same document or card as the note it replies to (or on none). The server only
    infers answers for unclaimed items, so a woken session's answer to an item claimed for it is seen here. The
    owner's pages and the waker's notices never count; nor do responder notes, unless the claim is the responder's."""
    claim = handling.get("claim") or {}
    if claim.get("role") == "answered":
        return claim.get("answer_note_id") or "answered"
    if handling.get("inferred_answer_note_id"):
        return handling["inferred_answer_note_id"]
    if not claim.get("device_id"):
        return None
    arrived = parse_time(handling.get("arrived_at"))
    if arrived is None:
        return None
    places = {value for value in ((note or {}).get("document_id"), (note or {}).get("review_card_id")) if value}
    # The first such note after the item arrived (the feed is newest first).
    for candidate in sorted(notes, key=lambda row: str(row.get("created_at") or "")):
        origin = candidate.get("origin") if isinstance(candidate.get("origin"), dict) else {}
        if origin.get("kind") in (PHONE_PAGE, WAKER_NOTICE):
            continue
        if origin.get("responder") is True and claim.get("role") != "responder":
            continue
        if candidate.get("note_id") in (handling.get("note_id"), handling.get("item_id")):
            continue
        if candidate.get("sender_device_id") != claim.get("device_id"):
            continue
        if (origin.get("conversation_id") or None) != (claim.get("conversation_id") or None):
            continue
        created = parse_time(candidate.get("created_at"))
        if created is None or created <= arrived:
            continue
        own = {value for value in (candidate.get("document_id"), candidate.get("review_card_id")) if value}
        if not claim.get("conversation_id") and places and own and not places & own:
            continue  # a note about something else
        return candidate.get("note_id") or "answered"
    return None


def fallback_reason(handling: dict, fallback: str | None) -> str:
    """Why an item is the fallback session's, in the words the pointer gives it."""
    covered = [entry for entry in handling.get("covered_by") or [] if isinstance(entry, dict)]
    own = [entry for entry in covered if fallback and entry.get("conversation_id") == fallback]
    if any(entry.get("role") == "sender" for entry in own):
        return "It is yours: you sent the note."
    if any(entry.get("role") == "owner" for entry in own):
        label = next((entry.get("label") for entry in own if entry.get("role") == "owner"), None)
        return f"It is yours: you own the topic {json.dumps(label or '')}."
    others = [entry for entry in covered if entry not in own]
    if any(entry.get("woken") and entry.get("handed_over_at") for entry in others):
        return "The session it belongs to could not be woken (its waker handed it over), so it is yours."
    if any(entry.get("woken") for entry in others):
        return (
            "The session it belongs to could not be woken in time (its machine was offline past the fallback "
            "time), so it is yours."
        )
    if others:
        return "The agent that covered it did not answer within 10 minutes, so it is yours."
    return "Nobody else covers it, so it is yours as the session that answers what has no other home."


def follow_up_action(answered: bool, took_up: bool, fallback: bool, waited: float, config: "Config") -> str:
    """What to do with a delivered item when the waker checks back: answered (done), wait, hand_over (to the next
    session), or tell (the owner: nobody else can take it)."""
    if answered:
        return "answered"
    if waited < float(config["answer_wait_minutes"]) * 60:
        return "wait"
    if took_up:
        return "wait" if waited < float(config["taken_up_hours"]) * 3600 else "tell"
    return "tell" if fallback else "hand_over"


def notice_text(kind: str, payload: dict, why: str, zone: dt.tzinfo) -> tuple[str, str]:
    """The title and message of the note that tells the owner an item has no answer."""
    when = parse_time(payload.get("created_at"))
    at = f" of {when.astimezone(zone).strftime('%d %b %H:%M')}" if when else ""
    if kind == "reply":
        name = payload.get("note_title") or payload.get("note_id")
        title = f"Unanswered reply: {name}"
        what = f"Your reply{at} to the note “{name}” has no answer yet."
    elif kind == "page":
        name = payload.get("title") or payload.get("note_id")
        title = f"Unanswered page: {name}"
        what = f"The page{at} you sent (“{name}”) has no answer yet."
    else:
        name = payload.get("name")
        title = f"Glasses {payload.get('media') or 'capture'} not answered: {name}"
        what = f"Nobody answered the glasses {payload.get('media') or 'capture'} {name}."
    message = f"{what} {why.rstrip('.')}. The reply waker does not try again; nothing else was started."
    return title[:200], message[:4000]


def notice_image(title: str, message: str) -> bytes:
    """A plain PNG with the notice's words (Pillow when there is one, else a blank card: the message says it all)."""
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: PLC0415 (optional)

        try:
            font = ImageFont.load_default(size=30)
            bold = ImageFont.load_default(size=36)
        except TypeError:  # Pillow before 10.1
            font = bold = ImageFont.load_default()
        lines = []
        for text, face, width in ((title, bold, 58), (message, font, 74)):
            words, line = text.split(), ""
            for word in words:
                if line and len(line) + 1 + len(word) > width:
                    lines.append((line, face))
                    line = word
                else:
                    line = f"{line} {word}".strip()
            lines.append((line, face))
            lines.append(("", font))
        height = 60 + 44 * len(lines)
        image = Image.new("RGB", (1200, height), "white")
        draw = ImageDraw.Draw(image)
        y = 40
        for text, face in lines:
            draw.text((48, y), text, fill="black", font=face)
            y += 44
        from io import BytesIO  # noqa: PLC0415

        out = BytesIO()
        image.save(out, "PNG")
        return out.getvalue()
    except Exception:  # noqa: BLE001 (no Pillow, or no font: a blank card)
        import zlib  # noqa: PLC0415

        width, height = 1200, 120
        row = b"\x00" + b"\xff" * (width * 3)
        raw = zlib.compress(row * height)

        def chunk(kind: bytes, data: bytes) -> bytes:
            return len(data).to_bytes(4, "big") + kind + data + zlib.crc32(kind + data).to_bytes(4, "big")

        header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")


# --- the service -------------------------------------------------------------------------------------------


@dataclass
class Decision:
    action: str  # deliver | wait | watch | skip | retry | finish (answered) | tell (the owner: nobody takes it)
    session_id: str | None = None
    why: str = ""
    seconds: float = 0
    fallback: bool = False


class Waker:
    def __init__(self, config: Config, thoughts: Thoughts | None = None, state: State | None = None):
        self.config = config
        self.state = state or State(config.state_file)
        self.thoughts = thoughts or Thoughts.from_config(config)
        self.stop = threading.Event()
        self.sessions = Sessions(config, self.state)
        self.claude: Claude | None = None
        self.device_id: str | None = None
        self.busy: dict[str, threading.Thread] = {}
        self.busy_lock = threading.Lock()
        self.registered: list[dict] = []
        self.note_conversations: set[str] = set()
        self.owner_conversations: set[str] = set()

    # registration

    def fallback_id(self, live: dict[str, LiveSession] | None = None) -> str | None:
        if self.config["fallback_session_id"]:
            return self.config["fallback_session_id"]
        title = self.config["fallback_title"]
        if not title:
            return None
        live = self.sessions.live() if live is None else live
        named = sorted((s for s in live.values() if s.name == title), key=lambda s: -(s.updated_at or 0))
        if named:
            return named[0].session_id
        remembered = self.state.get("fallback_session_id")
        return remembered

    def candidate_ids(self, live: dict[str, LiveSession]) -> list[str]:
        """The sessions worth registering: live ones, the senders of this device's recent notes, this device's topic
        owners, and the sessions seen live in the last 30 days."""
        ids = list(live)
        ids += sorted(self.note_conversations) + sorted(self.owner_conversations)
        ids += self.state.remembered_since(30 * 86400)
        fallback = self.fallback_id(live)
        if fallback:
            ids.insert(0, fallback)
        seen, out = set(), []
        for session_id in ids:
            if session_id and session_id not in seen and UUID_TEXT.match(session_id):
                seen.add(session_id)
                out.append(session_id)
        return out[: self.config["max_sessions"]]

    def session_records(self) -> list[SessionInfo]:
        live = self.sessions.live()
        desktop = self.sessions.desktop()
        infos = []
        for session_id in self.candidate_ids(live):
            if not self.sessions.allowed(session_id):
                continue
            info = self.sessions.info(session_id, live, desktop)
            if info is not None:
                infos.append(info)
        fallback = self.fallback_id(live)
        if fallback:
            self.state.put("fallback_session_id", fallback)
        return infos

    def registration(self) -> dict:
        infos = self.session_records()
        body = {
            "sessions": [info.record() for info in infos],
            "heartbeat_seconds": self.config["heartbeat_seconds"],
            "fallback_after_seconds": int(float(self.config["fallback_after_hours"]) * 3600),
            "version": VERSION,
            "claude_version": self.claude.version if self.claude else None,
        }
        fallback = self.fallback_id()
        if fallback:
            title = next((info.title for info in infos if info.session_id == fallback), None)
            body["fallback"] = {"session_id": fallback, "title": title or self.config["fallback_title"]}
        return body

    def register(self, full: bool = True) -> dict:
        body = self.registration() if full else {}
        answer = self.thoughts.register(self.config.machine, body)
        self.device_id = answer.get("device_id") or self.device_id
        if full:
            self.registered = body["sessions"]
        return answer

    def heartbeat_loop(self) -> None:
        last_full, last_hash, last_notes = 0.0, None, 0.0
        while not self.stop.is_set():
            try:
                now = time.time()
                if now - last_notes >= self.config["notes_refresh_seconds"]:
                    self.refresh_conversations()
                    last_notes = now
                body = self.registration()
                digest = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
                full = digest != last_hash or now - last_full >= self.config["full_registration_seconds"]
                answer = self.thoughts.register(self.config.machine, body if full else {})
                self.device_id = answer.get("device_id") or self.device_id
                if full:
                    self.registered, last_hash, last_full = body["sessions"], digest, now
            except ThoughtsError as error:
                log.warning("heartbeat: %s", error)
            except Exception:  # noqa: BLE001 (keep beating; the error is logged)
                log.exception("heartbeat")
            self.stop.wait(self.config["heartbeat_seconds"])

    def refresh_conversations(self) -> None:
        """The sessions that sent this device's recent notes and own its topics: worth registering when closed."""
        try:
            notes = self.thoughts.notes(sent_by_me=True, limit=200)
            if self.device_id is None and notes:
                self.device_id = notes[0].get("sender_device_id")
            self.note_conversations = {
                note["origin"]["conversation_id"]
                for note in notes
                if isinstance(note.get("origin"), dict)
                and isinstance(note["origin"].get("conversation_id"), str)
                and not note["origin"].get("responder")
            }
            owners = self.thoughts.owners()
            self.owner_conversations = {
                row["conversation_id"]
                for row in owners
                if row.get("device_id") == self.device_id and row.get("conversation_id")
            }
        except ThoughtsError as error:
            log.warning("notes: %s", error)

    # pollers

    def reply_poller(self) -> None:
        wait = self.config["poll_wait_seconds"]
        while not self.stop.is_set():
            try:
                after = self.state.get("reply_cursor")
                if after is None:
                    after = self.newest_reply()
                    self.state.put("reply_cursor", after)
                    log.info("replies: starting after reply %s", after)
                # The fallback waker listens as the responder, so older phones still show an agent as listening.
                feed = self.thoughts.replies(after, wait=wait, listen=self.config.is_fallback)
                with self.state.transaction() as db:
                    for reply in feed.get("replies") or []:
                        event = reply_event(reply)
                        photo = reply.get("photo") if isinstance(reply.get("photo"), dict) else None
                        if photo and photo.get("capture"):
                            self.state.record_reply_capture(photo["capture"], reply["reply_id"])
                        if too_old(reply.get("created_at"), self.config["max_event_age_hours"]):
                            self.state.add_event(event, status="stale", db=db)
                        elif self.state.add_event(event, db=db):
                            log.info("queued %s (note %r)", event["id"], event["payload"].get("note_title"))
                        after = max(after, reply.get("sequence") or after)
                    self.state.put("reply_cursor", max(after, feed.get("cursor") or after), db)
            except ThoughtsError as error:
                log.warning("replies: %s", error)
                self.stop.wait(10)
            except Exception:  # noqa: BLE001
                log.exception("replies")
                self.stop.wait(30)

    def newest_reply(self) -> int:
        cursor = 0
        while True:
            feed = self.thoughts.replies(cursor)
            replies = feed.get("replies") or []
            if not replies:
                return max(cursor, feed.get("cursor") or 0)
            cursor = max(reply.get("sequence") or 0 for reply in replies)

    def page_poller(self) -> None:
        wait = self.config["poll_wait_seconds"]
        while not self.stop.is_set():
            try:
                after = self.state.get("page_cursor")
                if after is None:
                    after = self.thoughts.pages(None).get("cursor") or 0
                    self.state.put("page_cursor", after)
                feed = self.thoughts.pages(after, wait=wait)
                with self.state.transaction() as db:
                    for note in feed.get("notes") or []:
                        event = page_event(note)
                        if too_old(note.get("created_at"), self.config["max_event_age_hours"]):
                            self.state.add_event(event, status="stale", db=db)
                        elif self.state.add_event(event, db=db):
                            log.info("queued %s (%r)", event["id"], note.get("title"))
                        after = max(after, note.get("sequence") or after)
                    self.state.put("page_cursor", max(after, feed.get("cursor") or after), db)
            except ThoughtsError as error:
                log.warning("pages: %s", error)
                self.stop.wait(10)
            except Exception:  # noqa: BLE001
                log.exception("pages")
                self.stop.wait(30)

    def capture_files(self) -> list[Path]:
        files = []
        for key in ("photos_dir", "videos_dir"):
            folder = self.config.path(key)
            if folder is not None and folder.is_dir():
                files += [path for path in folder.iterdir() if path.is_file() and CAPTURE_NAME.match(path.name)]
        return sorted(files, key=lambda path: path.name)

    def capture_watcher(self) -> None:
        if not self.state.get("captures_baseline"):
            with self.state.transaction() as db:
                for path in self.capture_files():
                    db.execute(
                        "INSERT OR IGNORE INTO captures (name, path, first_seen, size, status) VALUES (?, ?, ?, ?, "
                        "'baseline')",
                        (path.name, str(path), time.time(), path.stat().st_size),
                    )
                self.state.put("captures_baseline", True, db)
        while not self.stop.is_set():
            try:
                self.scan_captures()
            except Exception:  # noqa: BLE001
                log.exception("captures")
            self.stop.wait(self.config["capture_scan_seconds"])

    def scan_captures(self, now: float | None = None) -> None:
        now = time.time() if now is None else now
        grace = self.config["capture_grace_seconds"]
        with self.state.transaction() as db:
            pending = {row["name"]: row for row in db.execute("SELECT * FROM captures WHERE status = 'pending'")}
            names = {row["name"] for row in db.execute("SELECT name FROM captures")}
            for path in self.capture_files():
                try:
                    size = path.stat().st_size
                except OSError:
                    continue
                if path.name not in names:
                    db.execute(
                        "INSERT INTO captures (name, path, first_seen, size, status) VALUES (?, ?, ?, ?, 'pending')",
                        (path.name, str(path), now, size),
                    )
                    continue
                row = pending.get(path.name)
                if row is None:
                    continue
                if size != row["size"] or size == 0:
                    db.execute("UPDATE captures SET size = ?, stable_since = NULL WHERE name = ?", (size, path.name))
                    continue
                stable_since = row["stable_since"] or now
                if row["stable_since"] is None:
                    db.execute("UPDATE captures SET stable_since = ? WHERE name = ?", (now, path.name))
                if now - row["first_seen"] < grace or now - stable_since < min(grace, 3):
                    continue
                event = capture_event(path)
                owner = self.state.reply_for_capture(path.name)
                if owner is not None:
                    status = "belongs_to_reply"
                    self.state.add_event(event, status="skipped", detail=f"belongs to reply {owner}", db=db)
                elif too_old(event["at"], self.config["max_capture_age_hours"]):
                    status = "stale"
                    self.state.add_event(event, status="stale", db=db)
                else:
                    status = "queued"
                    self.state.add_event(event, db=db)
                db.execute("UPDATE captures SET status = ? WHERE name = ?", (status, path.name))

    # deciding

    def mine(self, target: dict | None) -> bool:
        return (
            bool(target)
            and target.get("machine") == self.config.machine
            and (self.device_id is None or target.get("device_id") == self.device_id)
        )

    def decide(self, event: sqlite3.Row | dict, claim: bool = True) -> Decision:
        """Who on this machine answers the event, after claiming it for that session (claim=False: only say)."""
        kind = event["kind"]
        payload = json.loads(event["payload"]) if isinstance(event["payload"], str) else event["payload"]
        if kind == "capture":
            return self.decide_capture(payload)
        item = payload.get("reply_id") if kind == "reply" else payload.get("note_id")
        if kind == "page" and payload.get("start_session"):
            # A new session, never an existing one: only the waker that starts sessions takes it, once.
            if not self.config.starts_sessions:
                return Decision("skip", why="a new session is started by the waker that starts sessions")
            if payload.get("agent", "claude") != "claude" or payload.get("where", "computer") != "computer":
                return Decision("skip", why=f"a {payload.get('agent')} session ({payload.get('where')}) is not this waker's")
            taken = self.thoughts.handling(item).get("claim")
            if taken:
                return Decision("skip", why=f"already started: {taken.get('conversation_id')} on {taken.get('device_id')}")
            return Decision("start", why="the owner started a new session from the phone")
        handling = self.thoughts.handling(item)
        taken = handling.get("claim")
        target = handling.get("wake_target")
        fallback = self.fallback_id() if self.config.is_fallback else None
        if taken and taken.get("role") != "answered":
            # Never answer twice: a claimed item whose claimant already sent its answer is done.
            answered = self.answered(handling, payload)
            if answered:
                return Decision(
                    "finish", why=f"answered: {taken.get('conversation_id')} sent note {answered} after it arrived"
                )
        if (
            taken
            and taken.get("role") != "answered"
            and taken.get("device_id") == self.device_id
            and (
                (self.mine(target) and taken.get("conversation_id") == target.get("conversation_id"))
                or (fallback and taken.get("conversation_id") == fallback)
            )
        ):
            # Claimed by this waker before (a wake that is tried again).
            session_id = taken["conversation_id"]
            return Decision(
                "deliver",
                session_id,
                fallback_reason(handling, fallback) if session_id == fallback else "It is yours (claimed for you).",
                fallback=bool(fallback) and session_id == fallback,
            )
        if taken:
            woken_claim = any(
                entry.get("woken")
                and entry.get("conversation_id") == taken.get("conversation_id")
                and entry.get("device_id") == taken.get("device_id")
                for entry in handling.get("covered_by") or []
            )
            if woken_claim and payload.get("copy_for_agent"):
                # Copy for agent's text is context pasted into its conversation, answered in that chat: no note
                # answers it, so its delivery is the end (23 Nov: 8 false "Unanswered page" notices in one day).
                return Decision(
                    "skip",
                    why=f"Copy for agent text delivered to {taken.get('conversation_id')} on {taken.get('device_id')}",
                )
            if self.config.is_fallback and woken_claim and taken.get("role") != "answered":
                # Another waker claimed it for its session. That waker hands it over when the session does not take
                # it up; watch for that, for a bounded time, then tell the owner.
                return self.watch_claim(taken)
            return Decision(
                "skip",
                why=f"{taken.get('role')} by {taken.get('device_id')} (conversation {taken.get('conversation_id')})",
            )
        if self.mine(target) and target.get("role") != "responder":
            session_id = target["conversation_id"]
            why = {
                "sender": "It is yours: you sent the note.",
                "owner": f"It is yours: you own the topic {json.dumps(target.get('label') or '')}.",
            }.get(target.get("role"), "It is yours.")
            if handling.get("answer_by"):
                why = "The owner chose you to answer the replies to this note."
            is_fallback = bool(fallback) and session_id == fallback
            if not claim:
                return Decision("deliver", session_id, why, fallback=is_fallback)
            status, answer = self.thoughts.claim(item, {"conversation_id": session_id})
            if status in (200, 201):
                return Decision("deliver", session_id, why, fallback=is_fallback)
            return Decision("skip", why=f"claim for {session_id} refused: {answer.get('error_code')}")
        if fallback:
            why = fallback_reason(handling, fallback)
            # Copy for agent's text is for one conversation: when nobody else covers it any more (the responder's
            # claim succeeds), the owner is told instead of the fallback session taking it.
            elsewhere = bool(payload.get("copy_for_agent")) and payload.get("conversation_id") != fallback
            unreachable = "the conversation it was sent to could not be woken"
            if not claim:
                return Decision("tell", why=unreachable) if elsewhere else Decision("deliver", fallback, why, fallback=True)
            status, answer = self.thoughts.claim(item, {"conversation_id": fallback, "responder": True})
            if status in (200, 201):
                return Decision("tell", why=unreachable) if elsewhere else Decision("deliver", fallback, why, fallback=True)
            code = answer.get("error_code")
            if code == "agent_note_covered":
                return Decision(
                    "wait",
                    why=answer.get("detail") or code,
                    seconds=max(30.0, min(600.0, float(answer.get("retry_after") or 60))),
                )
            return Decision("skip", why=f"not the fallback's: {code}")
        return Decision(
            "skip", why="not this machine's" + (f" (wake target on {target.get('machine')})" if target else "")
        )

    def start_session(self, event) -> None:
        """The phone's "Start a new study session": a background session with the page's text as its prompt, the page
        claimed for it, and a note to the owner saying where it runs (or, when it cannot start, why)."""
        payload = json.loads(event["payload"]) if isinstance(event["payload"], str) else event["payload"]
        note_id = payload["note_id"]
        try:
            prompt = str(self.thoughts.note(note_id).get("message") or "").strip()
        except ThoughtsError as error:
            self.state.update(event["id"], status="waiting", not_before=time.time() + 60, detail=str(error)[:300])
            return
        if not prompt:
            self.tell_owner(event, "the page asking for a new session held no prompt")
            return
        if self.claude is None:
            self.tell_owner(event, f"claude is not available on {self.config.machine}")
            return
        folder = Path(os.path.expanduser(str(self.config["start_session_folder"])))
        folder.mkdir(parents=True, exist_ok=True)
        now = dt.datetime.now(self.config.zone())
        name = f"{self.config['start_session_name']} · {now:%d %b %H:%M}"
        header = (f"[Thoughts reply waker on {self.config.machine}: the owner started this session from their phone at "
                  f"{now:%H:%M} (page {note_id}). Their prompt for it follows.]\n\n")
        started = start_background(self.claude, name, folder, header + prompt)
        if not started.ok:
            why = (f"{folder} on {self.config.machine} is not trusted by Claude Code yet: run claude there once and "
                   "accept, then start the session again" if "not trusted" in started.output
                   else f"claude could not start a session on {self.config.machine}: {started.output[-300:]}")
            self.tell_owner(event, why)
            return
        if started.session_id:
            status, answer = self.thoughts.claim(note_id, {"conversation_id": started.session_id})
            if status not in (200, 201):
                # Another computer (the owner's Thoughts app) started it first: this one goes, quietly.
                log.info("page %s was claimed elsewhere (%s): stopping %s", note_id, answer.get("error_code"), started.short_id)
                stop_background(self.claude, started.short_id)
                self.finish(event, "skipped", f"started elsewhere first; stopped {started.short_id}")
                return
        title = "Study session started"
        message = (f"“{name}” started on {self.config.machine} at {now:%H:%M}. It works from your prompt and answers "
                   f"with notes. On {self.config.machine}: claude attach {started.short_id}")
        body = {
            "note_id": str(uuid.uuid4()),
            "title": title,
            "message": message,
            "image": {"media_type": "image/png",
                      "data_base64": base64.b64encode(notice_image(title, message)).decode("ascii")},
            "origin": {"agent": "reply-waker", "kind": "session_started", "machine": self.config.machine,
                       "item_id": note_id, "conversation_id": started.session_id or None},
        }
        try:
            self.thoughts.send_note(body)
        except ThoughtsError as error:
            log.warning("started-session note for %s: %s", note_id, error)
        self.finish(event, "started", f"{name}: {started.session_id or started.short_id}")

    def answered(self, handling: dict, payload: dict) -> str | None:
        """answer_note for this item, from the newest notes of every device."""
        claim = handling.get("claim") or {}
        if claim.get("role") == "answered" or handling.get("inferred_answer_note_id"):
            return answer_note(handling, None, [])
        try:
            notes = self.thoughts.notes(limit=200)
        except ThoughtsError as error:
            log.warning("notes for %s: %s", handling.get("item_id"), error)
            return None
        note_id = handling.get("note_id") or payload.get("note_id")
        note = next((row for row in notes if row.get("note_id") == note_id), None)
        if note is None and note_id:
            with contextlib.suppress(ThoughtsError):
                note = self.thoughts.note(note_id)
        return answer_note(handling, note, notes)

    def watch_claim(self, taken: dict) -> Decision:
        """Another waker's claim for its session: watched until that waker hands it over or the session answers, at most
        taken_up_hours plus a grace time after the claim; then the owner is told."""
        claimed = parse_time(taken.get("claimed_at"))
        age = (utc_now() - claimed).total_seconds() if claimed else 0.0
        grace = float(self.config["handover_grace_minutes"]) * 60
        who = f"claimed for {taken.get('conversation_id')} on {taken.get('device_id')}"
        if age < float(self.config["taken_up_hours"]) * 3600 + grace:
            early = age < float(self.config["answer_wait_minutes"]) * 60 + grace
            return Decision("watch", why=who, seconds=300 if early else float(self.config["recheck_minutes"]) * 60)
        return Decision(
            "tell",
            why=(
                f"It was claimed {ago(age)} for the session {taken.get('conversation_id')} on "
                f"{taken.get('device_id')}, which has sent no answer, and its waker did not hand it over"
            ),
        )

    def decide_capture(self, payload: dict) -> Decision:
        if self.state.reply_for_capture(payload["name"]):
            return Decision("skip", why="belongs to a reply")
        try:
            owners = [row for row in self.thoughts.owners() if row.get("captures")]
        except ThoughtsError as error:
            return Decision("retry", why=f"owners: {error}", seconds=30)
        registered = {record["session_id"] for record in self.registered if record.get("wakeable")}
        tried = set(payload.get("tried") or [])  # sessions a delivery of this capture already failed for
        for owner in owners:  # most recently seen first
            if owner.get("conversation_id") in tried:
                continue
            if owner.get("live"):
                return Decision("skip", why=f"left to the capture owner {owner.get('label')} (it listens)")
            if owner.get("device_id") == self.device_id and owner.get("conversation_id") in registered:
                return Decision(
                    "deliver",
                    owner["conversation_id"],
                    f"It is yours: you own the glasses captures ({owner.get('label')}).",
                )
        fallback = self.fallback_id() if self.config.is_fallback else None
        if fallback and fallback not in tried:
            why = "Nobody owns the glasses captures, so it is yours."
            if tried:
                why = "The session that owns the glasses captures could not be woken, so it is yours."
            return Decision("deliver", fallback, why, fallback=True)
        return Decision("skip", why="no capture owner here")

    # the worker

    def worker(self) -> None:
        while not self.stop.is_set():
            if self.config.pause_file.exists():
                self.stop.wait(5)
                continue
            with self.busy_lock:
                running = {key for key, thread in self.busy.items() if thread.is_alive()}
                self.busy = {key: thread for key, thread in self.busy.items() if thread.is_alive()}
            self.reclaim_lost(running)
            if len(running) >= self.config["max_parallel"]:
                self.stop.wait(2)
                continue
            event = self.state.ready(running)
            if event is None:
                self.stop.wait(2)
                continue
            try:
                self.handle(event, running)
            except ThoughtsError as error:
                log.warning("%s: %s", event["id"], error)
                # A delivered item keeps waiting for its answer (asking again would deliver it twice).
                status = "awaiting_answer" if column(event, "status") == "awaiting_answer" else "waiting"
                self.state.update(event["id"], status=status, not_before=time.time() + 30, detail=str(error)[:300])
            except Exception as error:  # noqa: BLE001 (the event fails, the queue goes on)
                log.exception("event %s", event["id"])
                self.state.update(event["id"], status="failed", finished_at=time.time(), detail=f"error: {error}"[:300])
                self.log_outcome(event, "failed", f"error: {error}")

    def reclaim_lost(self, running: set[str]) -> None:
        """A 'delivering' event without a running delivery (its thread died) is offered again, never left stuck."""
        for row in self.state.delivering():
            if row["target"] in running:
                continue
            with self.busy_lock:
                thread = self.busy.get(row["target"])
            if thread is not None and thread.is_alive():
                continue
            log.warning("delivery of %s to %s was lost; offered again", row["id"], row["target"])
            self.state.update(row["id"], status="queued", not_before=time.time() + 5, detail="delivery lost")
            self.log_outcome(row, "lost", "the delivery stopped without an outcome; offered again")

    def handle(self, event: sqlite3.Row, running: set[str]) -> None:
        if column(event, "status") == "awaiting_answer":
            self.follow_up(event)
            return
        decision = self.decide(event)
        event_id = event["id"]
        if decision.action == "skip":
            log.info("skipped %s: %s", event_id, decision.why)
            self.state.update(event_id, status="skipped", finished_at=time.time(), detail=decision.why[:300])
            self.log_outcome(event, "skipped", decision.why)
            return
        if decision.action == "finish":
            log.info("answered %s: %s", event_id, decision.why)
            self.finish(event, "answered", decision.why)
            return
        if decision.action == "tell":
            self.tell_owner(event, decision.why)
            return
        if decision.action == "start":
            self.start_session(event)
            return
        if decision.action in ("wait", "watch", "retry"):
            log.info("%s %s for %ss: %s", decision.action, event_id, round(decision.seconds), decision.why)
            status = "watching" if decision.action == "watch" else "waiting"
            if column(event, "status") != status or column(event, "detail") != decision.why[:300]:
                self.log_outcome(event, status, decision.why, seconds=round(decision.seconds))
            self.state.update(
                event_id,
                status=status,
                not_before=time.time() + decision.seconds,
                detail=decision.why[:300],
            )
            return
        session_id = decision.session_id
        self.state.update(event_id, status="delivering", target=session_id, detail=decision.why[:300])
        if session_id in running:
            self.state.update(event_id, status="waiting", not_before=time.time() + 15)
            return
        thread = threading.Thread(
            target=self.deliver, args=(dict(event), decision), daemon=True, name=f"wake-{session_id[:8]}"
        )
        with self.busy_lock:
            self.busy[session_id] = thread
        thread.start()

    # waking a session

    def deliver(self, event: dict, decision: Decision) -> None:
        event_id, session_id = event["id"], decision.session_id
        payload = json.loads(event["payload"])
        attempts = (event.get("attempts") or 0) + 1
        self.state.update(event_id, attempts=attempts)
        entry = {
            "event": event_id,
            "kind": event["kind"],
            "session": session_id,
            "machine": self.config.machine,
            "fallback": decision.fallback,
            "claude_version": self.claude.version if self.claude else None,
            "started": iso(time.time()),
        }
        started = time.time()
        try:
            with session_lock(self.config.locks_dir, session_id):
                status, detail = self.wake(event, payload, decision, entry)
        except Exception as error:  # noqa: BLE001
            log.exception("waking %s for %s", session_id, event_id)
            status, detail = "failed", f"error: {error}"
        entry.update({"status": status, "detail": detail, "seconds": round(time.time() - started, 1)})
        self.log_wake(entry)
        self.after_wake(event, payload, decision, status, detail, attempts, entry)

    def after_wake(
        self, event: dict, payload: dict, decision: Decision, status: str, detail: str, attempts: int, entry: dict
    ) -> None:
        """Every outcome moves the event on: delivered (then wait for the answer), tried again later, handed over to
        the next session, or the owner told. Nothing stays 'delivering'."""
        event_id, session_id = event["id"], decision.session_id
        server_item = event["kind"] in ("reply", "page")
        if status == "done":
            self.state.delete("auth_outage")
            self.state.delete_prefix(f"offset:{event_id}:")
            if not server_item or payload.get("copy_for_agent"):
                # A capture, or Copy for agent's text (context, which no note answers): delivered is done.
                self.state.update(event_id, status="done", finished_at=time.time(), detail=detail)
                return
            self.state.put(
                f"delivered:{event_id}",
                {
                    "at": time.time(),
                    "session": session_id,
                    "offset": entry.get("offset"),
                    "fallback": decision.fallback,
                },
            )
            self.state.update(
                event_id,
                status="awaiting_answer",
                not_before=time.time() + float(self.config["answer_wait_minutes"]) * 60,
                detail=detail,
            )
            return
        if status == "interrupted" or self.stop.is_set():
            self.state.update(event_id, status="queued", not_before=None, detail="interrupted")
            return
        if status == "auth":
            self.auth_outage(detail)
            if server_item and not decision.fallback:
                # Another machine's claude may still work: the next session (in the end the fallback) answers it.
                self.hand_over(event, payload, detail)
                return
            if too_old(event.get("event_at"), self.config["max_event_age_hours"]):
                self.tell_owner(event, f"Claude on {self.config.machine} could not sign in until the item was too old")
                return
            self.state.update(
                event_id,
                status="waiting",
                not_before=time.time() + float(self.config["auth_retry_minutes"]) * 60,
                detail=detail,
            )
            return
        if status == "retry" and attempts < self.config["max_attempts"]:
            pause = 60 * attempts if decision.fallback else 30 * attempts
            self.state.update(event_id, status="waiting", not_before=time.time() + pause, detail=detail)
            return
        if decision.fallback:
            # The fallback session is the last one: the owner hears of it instead of a new session being started.
            title = entry.get("title") or session_id
            self.tell_owner(event, f"It could not be delivered to {title}: {detail}")
            return
        if server_item:
            self.hand_over(event, payload, detail)
            return
        # A capture whose owner could not be woken goes to the fallback session.
        payload["tried"] = sorted({*(payload.get("tried") or []), session_id})
        self.state.update(event_id, status="queued", not_before=time.time() + 5, payload=json.dumps(payload))
        self.log_outcome(event, "next", f"{detail}; trying the next session")

    def hand_over(self, event, payload: dict, detail: str) -> None:
        """The server passes the item on (the topic owner's session, then the fallback); this waker decides again,
        so a next session on this machine is woken here."""
        event_id = event["id"]
        item = payload.get("reply_id") or payload.get("note_id")
        try:
            answer = self.thoughts.handover(item, self.config.machine, detail or "could not be woken")
            log.info("handed over %s (released: %s): %s", event_id, answer.get("released"), detail)
        except ThoughtsError as error:
            log.warning("handover %s: %s", event_id, error)
            self.state.update(event_id, status="waiting", not_before=time.time() + 60, detail=f"handover: {error}")
            return
        self.state.delete(f"delivered:{event_id}")
        self.state.delete_prefix(f"offset:{event_id}:")
        self.state.update(
            event_id, status="queued", not_before=time.time() + 5, target=None, detail=f"handed over: {detail}"[:300]
        )
        self.log_outcome(event, "handed_over", detail, released=answer.get("released"))

    def follow_up(self, event) -> None:
        """Checks back on a delivered reply or page: answered, still being worked on, or handed on."""
        event_id = event["id"]
        payload = json.loads(event["payload"]) if isinstance(event["payload"], str) else event["payload"]
        item = payload.get("reply_id") or payload.get("note_id")
        delivered = self.state.get(f"delivered:{event_id}") or {}
        session_id = delivered.get("session") or column(event, "target")
        handling = self.thoughts.handling(item)
        claim = handling.get("claim") or {}
        answered = self.answered(handling, payload)
        if answered:
            self.finish(event, "answered", f"answered (note {answered})")
            return
        if claim and claim.get("conversation_id") != session_id:
            self.finish(event, "skipped", f"now claimed by {claim.get('conversation_id')} on {claim.get('device_id')}")
            return
        info = self.sessions.info(session_id) if session_id else None
        offset = int(delivered.get("offset") or 0)
        took_up = bool(info) and taken_up(info.transcript, offset, [item, payload.get("note_id")])
        waited = time.time() - float(delivered.get("at") or time.time())
        span = ago(waited).replace(" ago", "")
        fallback = bool(delivered.get("fallback"))
        action = follow_up_action(False, took_up, fallback, waited, self.config)
        title = (info.title if info else None) or session_id
        if action == "wait":
            self.state.update(
                event_id,
                status="awaiting_answer",
                not_before=time.time() + float(self.config["recheck_minutes"]) * 60,
                detail=f"{'taken up' if took_up else 'delivered'}; no answer yet after {span}",
            )
            return
        if action == "hand_over":
            self.hand_over(event, payload, f"{title} did not take it up within {span}")
            return
        why = (
            f"{title} read it {span} ago but has sent no answer"
            if took_up
            else f"{title} did not take it up within {span}"
        )
        self.tell_owner(event, why)

    def finish(self, event, status: str, detail: str) -> None:
        self.state.delete(f"delivered:{event['id']}")
        self.state.delete_prefix(f"offset:{event['id']}:")
        self.state.update(event["id"], status=status, finished_at=time.time(), detail=detail[:300])
        self.log_outcome(event, status, detail)

    # telling the owner

    def tell_owner(self, event, why: str) -> None:
        """One short agent note per item saying it has no answer and why (never a second one, from any machine), then
        the event ends as 'unanswered'."""
        payload = json.loads(event["payload"]) if isinstance(event["payload"], str) else event["payload"]
        item = payload.get("reply_id") or payload.get("note_id") or payload.get("name")
        told = self.state.get(f"told:{item}") or self.notice_for(item)
        if not told and self.config["notify_owner"]:
            title, message = notice_text(event["kind"], payload, why, self.config.zone())
            told = self.send_notice(title, message, item, event["kind"])
            if told:
                self.state.put(f"told:{item}", told)
        self.finish(event, "unanswered", f"{why}; owner told (note {told})" if told else f"{why}; owner not told")

    def notice_for(self, item: str) -> str | None:
        """A notice any waker already sent about this item (the server's notes are shared by every machine)."""
        try:
            notes = self.thoughts.notes(limit=200)
        except ThoughtsError:
            return None
        for note in notes:
            origin = note.get("origin") if isinstance(note.get("origin"), dict) else {}
            if origin.get("kind") == WAKER_NOTICE and origin.get("item_id") == item:
                return note.get("note_id")
        return None

    def send_notice(self, title: str, message: str, item: str, kind: str) -> str | None:
        note_id = str(uuid.uuid4())
        body = {
            "note_id": note_id,
            "title": title,
            "message": message,
            "image": {
                "media_type": "image/png",
                "data_base64": base64.b64encode(notice_image(title, message)).decode("ascii"),
            },
            # responder: true, so no tool takes this note for anybody's answer; replies to it go to the fallback.
            "origin": {
                "agent": "reply-waker",
                "kind": WAKER_NOTICE,
                "responder": True,
                "machine": self.config.machine,
                "item_id": item,
                "item_kind": kind,
            },
        }
        try:
            self.thoughts.send_note(body)
        except ThoughtsError as error:
            log.warning("notice for %s: %s", item, error)
            return None
        log.info("told the owner about %s (note %s): %s", item, note_id, message)
        self.log_outcome({"id": f"{kind}:{item}", "kind": kind}, "told_owner", message, note=note_id)
        return note_id

    def auth_outage(self, detail: str) -> None:
        """claude cannot sign in on this machine: the owner is told once per outage (it ends with the next good run)."""
        outage = self.state.get("auth_outage")
        if outage:
            return
        self.state.put("auth_outage", {"since": time.time(), "detail": detail[:300]})
        log.error("claude cannot sign in on %s: %s", self.config.machine, detail)
        if not self.config["notify_owner"]:
            return
        message = (
            f"Claude Code on {self.config.machine} cannot sign in, so the reply waker there cannot reach any session: "
            f"{detail[:300]} Run `claude auth login` on {self.config.machine}; the waiting replies are delivered "
            "after that."
        )
        self.send_notice(f"Reply waker: Claude on {self.config.machine} needs signing in", message,
                         f"auth:{self.config.machine}:{int(time.time())}", "auth")

    def wake(self, event: dict, payload: dict, decision: Decision, entry: dict) -> tuple[str, str]:
        session_id = decision.session_id
        info = self.sessions.info(session_id)
        if info is None:
            return "failed", "the session is not on this machine"
        entry["title"] = info.title
        extra = capture_lines(payload, self.config) if event["kind"] == "capture" else None
        text = pointer(event["kind"], payload, session_id, self.config.machine, decision.why, extra)
        nonce = pointer_tag(event["kind"], payload, self.config.machine)
        if info.live is not None:
            return self.message(info, text, nonce, entry)
        return self.resume(info, text, nonce, entry)

    def message(self, info: SessionInfo, text: str, nonce: str, entry: dict) -> tuple[str, str]:
        """One attempt, at most attempt_seconds: wait a little while the session is busy, send the pointer once, and
        see it in the session's transcript. A pointer an earlier attempt sent that arrived late is not sent again."""
        entry["method"] = "message"
        live = info.live
        if not live.name:
            return "failed", "the running session has no name, so it cannot be messaged"
        started = time.time()
        deadline = started + float(self.config["attempt_seconds"])
        busy_until = min(started + float(self.config["busy_wait_seconds"]), deadline - 30)
        while live.status != "idle" and time.time() < busy_until:
            if self.stop.wait(5):
                return "interrupted", "the waker stopped before the message was sent"
            fresh = self.sessions.live().get(info.session_id)
            if fresh is None:  # it ended while we waited: resume it instead (if it may be)
                info = self.sessions.info(info.session_id)
                if info is None or not info.wakeable:
                    return "failed", (info.why_not if info else "the session is gone")
                return self.resume(info, text, nonce, entry)
            live = fresh
        if self.stop.is_set():
            return "interrupted", "the waker stopped before the message was sent"
        # Busy still: send anyway. The message waits in the session's queue and is read when its turn ends.
        entry["waited_busy"] = live.status != "idle"
        entry["session_status"] = live.raw_status
        others = [s for s in self.sessions.live().values() if s.name == live.name and s.session_id != live.session_id]
        hint = ago(time.time() - live.started_at) if others and live.started_at else None
        transcript = info.transcript or find_transcript(self.sessions.projects_dir, info.session_id, live.cwd)
        offset = transcript.stat().st_size if transcript and transcript.is_file() else 0
        # Where the transcript ended before the first attempt to this session (an earlier pointer is looked for there).
        key = f"offset:{entry.get('event')}:{info.session_id}" if entry.get("event") else None
        first = self.state.get(key) if key else None
        if first is not None:
            seen = grew_with(transcript, int(first), nonce)
            if seen:
                entry.update({"confirmed": seen, "offset": int(first)})
                return "done", f"messaged {json.dumps(live.name)} ({seen}, by an earlier attempt)"
        elif key:
            self.state.put(key, offset)
        entry["offset"] = int(first) if first is not None else offset
        remaining = deadline - time.time()
        run = self.claude.message(
            live.name,
            info.permission_mode,
            text,
            hint,
            timeout=max(20.0, min(float(self.config["messenger_timeout_seconds"]), remaining - 10)),
        )
        entry["messenger_cost_usd"] = round(run.cost or 0, 6)
        result = str(run.answer.get("result") or "").strip()
        sent = run.ok and "SENT" in result.upper()
        if not sent:
            log.warning("messenger for %s: %s", info.session_id, run.problem())
            if run.auth_failed:
                return "auth", f"claude on {self.config.machine} cannot sign in: {run.problem()[:200]}"
        confirm_until = min(deadline, time.time() + float(self.config["confirm_seconds"]))
        while True:
            transcript = transcript or find_transcript(self.sessions.projects_dir, info.session_id, live.cwd)
            seen = grew_with(transcript, entry["offset"], nonce)
            if seen:
                entry["confirmed"] = seen
                return "done", f"messaged {json.dumps(live.name)} ({seen})"
            if not sent or time.time() >= confirm_until or self.stop.wait(2):
                break
        if not sent:
            return "retry", f"the messenger failed: {run.problem()[:200]}"
        seconds = round(time.time() - started)
        return "retry", f"the messenger sent it, but it did not reach the session's transcript within {seconds} s"

    def resume(self, info: SessionInfo, text: str, nonce: str, entry: dict) -> tuple[str, str]:
        entry["method"] = "resume"
        # Again, under the lock: a session opened meanwhile is messaged, never resumed beside its live process.
        fresh = self.sessions.info(info.session_id)
        if fresh is None:
            return "failed", "the session is gone"
        if fresh.live is not None:
            return self.message(fresh, text, nonce, entry)
        if not fresh.wakeable:
            return "failed", fresh.why_not or "not wakeable"
        info = fresh
        entry.update({"folder": info.folder, "permission_mode": info.permission_mode, "model": info.model})
        with contextlib.suppress(OSError, AttributeError):
            entry.setdefault("offset", info.transcript.stat().st_size)
        idle_hours = (time.time() - (info.last_active or time.time())) / 3600
        entry["idle_hours"] = round(idle_hours, 2)
        budget = float(self.config["budget_usd"])
        if idle_hours >= float(self.config["idle_trim_hours"]):
            trim = self.claude.resume(info, f"/compact {self.config['trim_instructions']}", budget)
            entry["trim_cost_usd"] = trim.cost
            entry["trimmed"] = trim.ok
            if not trim.ok:
                log.warning("trimming %s: %s", info.session_id, trim.problem())
            budget = max(0.05, budget - (trim.cost or 0))
            if self.sessions.live().get(info.session_id) is not None:
                return "retry", "the session was opened while it was trimmed"
        run = self.claude.resume(info, text, budget)
        entry["cost_usd"] = run.cost
        entry["num_turns"] = run.answer.get("num_turns")
        entry["result"] = str(run.answer.get("result") or "")[:1000]
        refused = sorted(
            {
                str(denial.get("tool_name"))
                for denial in run.answer.get("permission_denials") or []
                if isinstance(denial, dict) and "thoughts" in str(denial.get("tool_name"))
            }
        )
        if refused:
            # Woken, but it could not read the owner's reply: its permissions here do not allow its Thoughts tools.
            entry["refused_tools"] = refused
            return "failed", f"resumed, but its Thoughts tools were refused ({', '.join(refused)})"
        if run.ok:
            return "done", f"resumed in {info.folder} ({info.permission_mode or 'default'} mode)"
        if run.auth_failed:
            return "auth", f"claude on {self.config.machine} cannot sign in: {run.problem()[:200]}"
        return "retry", f"resume failed: {run.problem()}"

    def log_outcome(self, event, outcome: str, detail: str, **extra) -> None:
        """A wakes.jsonl line for an outcome that is not a wake attempt (skipped, watching, answered, handed over,
        told the owner, lost): every event this waker acts on leaves a trace there."""
        entry = {
            "event": event["id"],
            "kind": column(event, "kind"),
            "machine": self.config.machine,
            "outcome": outcome,
            "detail": (detail or "")[:500],
            "at": iso(time.time()),
            **extra,
        }
        try:
            self.config.wakes_log.parent.mkdir(parents=True, exist_ok=True)
            with self.config.wakes_log.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
        except OSError as error:
            log.warning("wakes log: %s", error)

    def log_wake(self, entry: dict) -> None:
        self.config.wakes_log.parent.mkdir(parents=True, exist_ok=True)
        with self.config.wakes_log.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
        log.info(
            "wake %s -> %s (%s): %s, %s USD%s",
            entry["event"],
            entry["session"],
            entry.get("method"),
            entry["status"],
            round(
                (entry.get("cost_usd") or 0)
                + (entry.get("trim_cost_usd") or 0)
                + (entry.get("messenger_cost_usd") or 0),
                4,
            ),
            f" ({entry['detail']})",
        )

    # running

    def run(self) -> None:
        self.config.home.mkdir(parents=True, exist_ok=True)
        # One service per home: a second one (two apps, an old one still stopping) leaves at once.
        self.home_lock = (self.config.home / "waker.lock").open("a")
        if not lock_file(self.home_lock, blocking=False):
            log.error("another waker runs in %s", self.config.home)
            raise SystemExit(ALREADY_RUNNING)
        if WAKERS_CAPABILITY not in self.thoughts.capabilities():
            raise SystemExit(f"the Thoughts server has no {WAKERS_CAPABILITY}: deploy it first")
        self.claude = Claude(self.config, self.sessions, self.stop)
        log.info(
            "machine %s, claude %s (%s), fallback %s",
            self.config.machine,
            self.claude.version,
            self.claude.binary,
            self.fallback_id() or "none",
        )
        interrupted = self.state.requeue_interrupted()
        if interrupted:
            log.warning("interrupted wakes offered again: %s", ", ".join(interrupted))
            for event_id in interrupted:
                self.log_outcome({"id": event_id, "kind": event_id.split(":", 1)[0]}, "interrupted", "offered again")
        self.refresh_conversations()
        self.register()
        log.info("registered %d session(s) as device %s", len(self.registered), self.device_id)
        targets = [self.heartbeat_loop, self.reply_poller, self.page_poller, self.worker]
        if self.config.is_fallback and (self.config.path("photos_dir") or self.config.path("videos_dir")):
            targets.append(self.capture_watcher)
        threads = [threading.Thread(target=target, name=target.__name__, daemon=True) for target in targets]
        for thread in threads:
            thread.start()
        if self.config["stop_on_stdin_eof"]:
            threading.Thread(target=watch_stdin, args=(self.stop,), name="stdin", daemon=True).start()

        def stop(_signum, _frame):
            self.stop.set()

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        failed = False
        while not self.stop.is_set():
            self.stop.wait(1)
            dead = [thread.name for thread in threads if not thread.is_alive()]
            if dead and not self.stop.is_set():
                log.error("thread(s) stopped: %s; exiting so the service restarts", ", ".join(dead))
                failed = True
                self.stop.set()
        log.info("stopping")
        for thread in list(self.busy.values()):
            thread.join(60)
        if failed:
            raise SystemExit(1)


def watch_stdin(stop: threading.Event, stream=None) -> None:
    """Sets stop when standard input ends: the app that started this waker closed it (to stop it), or ended."""
    if stream is None:
        stream = getattr(sys.stdin, "buffer", None)
    if stream is None:
        log.warning("no standard input to watch: stopping on a signal only")
        return
    try:
        while stream.read(1024):
            pass
    except (OSError, ValueError):
        pass
    log.info("standard input ended: stopping")
    stop.set()


# --- glasses videos ------------------------------------------------------------------------------------------


def capture_lines(payload: dict, config: Config) -> list[str]:
    """For a video: frames and the transcript, made before the wake (the same as the old reply agent did)."""
    path = Path(payload["path"])
    if payload.get("media") != "video":
        return [f"Read the photo at full resolution: {path}."]
    seconds = probe_seconds(path)
    long_video = seconds is not None and seconds > config["max_video_seconds"]
    folder = config.home / "captures" / path.stem
    frames = extract_frames(path, folder, config, 3 if long_video else None, seconds)
    lines = [
        f"Frames ({len(frames)}): {', '.join(str(frame) for frame in frames)}. Read the sharpest."
        if frames
        else "No frames could be taken from it (ffmpeg failed)."
    ]
    if long_video:
        lines.append(f"It is longer than {config['max_video_seconds']} s, so it was not transcribed.")
    else:
        transcript = transcribe(path, folder, config)
        lines.append(
            f"What the owner says in it (transcript): {json.dumps(transcript, ensure_ascii=False)}"
            if transcript
            else "No speech in it (or the transcription failed)."
        )
    return lines


def probe_seconds(path: Path) -> float | None:
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout.strip()
        return float(out) if out else None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def extract_frames(path: Path, folder: Path, config: Config, count: int | None, seconds: float | None) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob("frame-*.jpg"):
        old.unlink()
    ffmpeg = config["ffmpeg"]
    try:
        if count:
            points = [0.0, (seconds or 0) / 2, max(0.0, (seconds or 0) - 0.5)]
            for index, point in enumerate(points, 1):
                subprocess.run(
                    [
                        ffmpeg,
                        "-v",
                        "error",
                        "-y",
                        "-ss",
                        f"{point:.2f}",
                        "-i",
                        str(path),
                        "-frames:v",
                        "1",
                        str(folder / f"frame-{index:03d}.jpg"),
                    ],
                    timeout=120,
                    check=False,
                )
        else:
            subprocess.run(
                [
                    ffmpeg,
                    "-v",
                    "error",
                    "-y",
                    "-i",
                    str(path),
                    "-vf",
                    "fps=1",
                    "-frames:v",
                    str(config["max_video_frames"]),
                    str(folder / "frame-%03d.jpg"),
                ],
                timeout=300,
                check=False,
            )
            subprocess.run(
                [
                    ffmpeg,
                    "-v",
                    "error",
                    "-y",
                    "-sseof",
                    "-0.3",
                    "-i",
                    str(path),
                    "-frames:v",
                    "1",
                    str(folder / "frame-last.jpg"),
                ],
                timeout=120,
                check=False,
            )
    except (OSError, subprocess.SubprocessError) as error:
        log.warning("frames of %s: %s", path.name, error)
    return sorted(folder.glob("frame-*.jpg"))


def transcribe(path: Path, folder: Path, config: Config) -> str:
    done = folder / "transcript.txt"
    if done.is_file():
        return done.read_text(encoding="utf-8").strip()
    tool = config.path("transcribe")
    if tool is None or not tool.exists():
        return ""
    try:
        result = subprocess.run([str(tool), str(path)], capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.SubprocessError):
        return ""
    text = result.stdout.strip()
    if result.returncode != 0 or text == "(no audio)":
        text = ""
    folder.mkdir(parents=True, exist_ok=True)
    done.write_text(text, encoding="utf-8")
    return text


# --- the other commands ----------------------------------------------------------------------------------------


def show_sessions(config: Config, as_json: bool) -> None:
    state = State(config.state_file) if config.state_file.is_file() else State(":memory:")
    try:
        thoughts = Thoughts.from_config(config)
    except SystemExit as error:
        print(f"(no Thoughts server: {error}; only local sessions)", file=sys.stderr)
        thoughts = None
    waker = Waker(config, thoughts=thoughts or Thoughts("http://127.0.0.1:9", lambda: ""), state=state)
    if thoughts is not None:
        waker.refresh_conversations()
    infos = waker.session_records()
    if as_json:
        print(
            json.dumps(
                [{**info.record(), "why_not": info.why_not, "permission_mode": info.permission_mode} for info in infos],
                indent=1,
            )
        )
        return
    for info in infos:
        print(
            f"{'wake' if info.wakeable else 'no  '} {info.state:<6} {info.kind:<14} {info.session_id} "
            f"{json.dumps(info.title or '')} {info.permission_mode or '-'}"
            + (f"  ({info.why_not})" if info.why_not else "")
        )


def status(config: Config) -> None:
    if not config.state_file.is_file():
        print(f"no state yet in {config.home}")
        return
    state = State(config.state_file)
    print(
        f"machine {config.machine}; paused: {'yes' if config.pause_file.exists() else 'no'}; "
        f"fallback session: {state.get('fallback_session_id') or 'none'}"
    )
    print(f"cursors: replies {state.get('reply_cursor')}, pages {state.get('page_cursor')}; events: {state.counts()}")
    for row in state.recent(15):
        when = dt.datetime.fromtimestamp(row["created_at"], config.zone()).strftime("%d %b %H:%M")
        print(
            f"  {when} {row['status']:<11} {row['id']}"
            + (f" -> {row['target']}" if row["target"] else "")
            + (f"  ({row['detail']})" if row["detail"] else "")
        )
    if config.wakes_log.is_file():
        for line in config.wakes_log.read_text(encoding="utf-8").strip().splitlines()[-8:]:
            entry = json.loads(line)
            cost = (
                (entry.get("cost_usd") or 0)
                + (entry.get("trim_cost_usd") or 0)
                + (entry.get("messenger_cost_usd") or 0)
            )
            if "outcome" in entry:
                print(f"{entry.get('outcome'):<5} {entry.get('at')} {entry.get('event')}: {entry.get('detail')}")
                continue
            print(
                f"wake {entry.get('started')} {entry.get('event')} -> {entry.get('title')!r} {entry.get('method')} "
                f"{entry.get('status')} {round(cost, 4)} USD"
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("run")
    sub.add_parser("status")
    shown = sub.add_parser("sessions")
    shown.add_argument("--json", action="store_true")
    decide = sub.add_parser("decide")
    decide.add_argument("item_id")
    sub.add_parser("pause")
    sub.add_parser("unpause")
    sub.add_parser("forget")
    args = parser.parse_args(argv)
    config = Config.load()
    log_format = "%(asctime)s %(levelname)s %(threadName)s: %(message)s"
    log_file = config.path("log_file") if args.command == "run" else None
    if log_file is not None:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(log_file, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
        logging.basicConfig(level=logging.INFO, format=log_format, handlers=[handler])
    else:
        logging.basicConfig(level=logging.INFO, format=log_format, stream=sys.stderr)
    if args.command == "run":
        Waker(config).run()
    elif args.command == "status":
        status(config)
    elif args.command == "sessions":
        show_sessions(config, args.json)
    elif args.command == "decide":
        waker = Waker(config, state=State(config.state_file) if config.state_file.is_file() else State(":memory:"))
        mine = [
            row
            for row in waker.thoughts.get("/v1/agent-notes/wakers").get("wakers") or []
            if row.get("machine") == config.machine
        ]
        waker.device_id = mine[0]["device_id"] if mine else None
        if waker.device_id is None:
            waker.refresh_conversations()
        handling = waker.thoughts.handling(args.item_id)
        kind = "page" if handling.get("kind") == "page" else "reply"
        payload = (
            {"reply_id": args.item_id, "note_id": handling.get("note_id")}
            if kind == "reply"
            else {"note_id": args.item_id}
        )
        decision = waker.decide({"kind": kind, "payload": payload}, claim=False)
        print(
            json.dumps(
                {
                    "wake_target": handling.get("wake_target"),
                    "claim": handling.get("claim"),
                    "decision": decision.__dict__,
                },
                indent=1,
            )
        )
    elif args.command == "pause":
        config.pause_file.write_text("paused\n", encoding="utf-8")
        print("paused: no wakes until `unpause`")
    elif args.command == "unpause":
        config.pause_file.unlink(missing_ok=True)
        print("unpaused")
    elif args.command == "forget":
        print(f"DELETE registration of {config.machine}: HTTP {Thoughts.from_config(config).forget(config.machine)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
