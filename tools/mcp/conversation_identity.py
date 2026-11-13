# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Which Claude Code or Codex conversation this is, its link and title (standard library only).

Shared by the Thoughts MCP (server.py) and the prompt hook (tools/hooks/thoughts_card_link.py),
which runs on every prompt and must not import the MCP's dependencies. Installed next to
server.py (the plugin's scripts directory).
"""
from __future__ import annotations

import contextlib
import json
import os
import re
from pathlib import Path
from typing import Callable

SESSION = re.compile(r"session_[A-Za-z0-9]{1,64}")
UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
ROLLOUT = re.compile(r"rollout-.*-(" + UUID.pattern + r")\.jsonl$")
PROMPT_READ_BYTES = 512 * 1024
# The line Thoughts' "Copy for agent" text names its review card with.
REVIEW_CARD = re.compile(r"Review card:[ \t]*([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})")


def claude_home() -> Path:
    return Path(os.environ.get("CLAUDE_CONFIG_DIR") or Path.home() / ".claude")


def codex_home() -> Path:
    return Path(os.environ.get("CODEX_HOME") or Path.home() / ".codex")


def read_json(path: Path) -> dict | None:
    with contextlib.suppress(OSError, ValueError):
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else None
    return None


def conversation_url(conversation_id: str) -> str | None:
    """The link that opens this conversation (agent-proposals spec section 3.6), when this computer knows it.

    A Claude Code session started through Remote Control records its claude.ai session next
    to its local UUID in ~/.claude/sessions/<pid>.json while it runs; a Codex thread is a
    rollout file named after its UUID. Anything else (a plain CLI session) has no link.
    """
    with contextlib.suppress(OSError):
        for path in (claude_home() / "sessions").glob("*.json"):
            with contextlib.suppress(OSError, ValueError, AttributeError):
                record = json.loads(path.read_text(encoding="utf-8"))
                bridge = record.get("bridgeSessionId")
                if record.get("sessionId") == conversation_id and isinstance(bridge, str) and SESSION.fullmatch(bridge):
                    return "https://claude.ai/code/" + bridge
    if codex_rollout_exists(conversation_id):
        return "codex://threads/" + conversation_id
    return None


def claude_session_by_id(conversation_id: str) -> dict | None:
    with contextlib.suppress(OSError):
        for path in (claude_home() / "sessions").glob("*.json"):
            record = read_json(path)
            if record and record.get("sessionId") == conversation_id:
                return record
    return None


def claude_session_of_ancestors(pid: int | None = None, depth: int = 6,
                                parent: Callable[[int], int | None] | None = None) -> dict | None:
    """The sessions/<pid>.json of the Claude Code process above `pid` (default: this process's parent).

    For scripts an agent runs in its shell, e.g. thoughts-watch-replies as a background monitor:
    claude -> shell -> script. `parent` maps a pid to its parent (default: ps).
    """
    parent = parent or _parent_pid
    sessions = claude_home() / "sessions"
    pid = os.getppid() if pid is None else pid
    for _ in range(depth):
        if not pid or pid <= 1:
            break
        record = read_json(sessions / f"{pid}.json")
        if record and isinstance(record.get("sessionId"), str) and UUID.fullmatch(record["sessionId"]):
            return record
        pid = parent(pid)
    return None


def _parent_pid(pid: int) -> int | None:
    import subprocess  # here: the prompt hook imports this module on every prompt and never needs it

    with contextlib.suppress(OSError, ValueError, subprocess.SubprocessError):
        output = subprocess.run(["ps", "-o", "ppid=", "-p", str(pid)], capture_output=True, text=True, timeout=5).stdout.strip()
        return int(output) if output else None
    return None


def codex_rollout_exists(conversation_id: str) -> bool:
    codex = codex_home()
    for folder, pattern in (("sessions", "*/*/*/rollout-*-"), ("archived_sessions", "rollout-*-")):
        with contextlib.suppress(OSError):
            if next((codex / folder).glob(pattern + conversation_id + ".jsonl"), None):
                return True
    return False


def claude_transcript(conversation_id: str, cwd: str | None = None) -> Path | None:
    projects = claude_home() / "projects"
    if cwd:
        path = projects / re.sub(r"[^A-Za-z0-9]", "-", cwd) / f"{conversation_id}.jsonl"
        if path.is_file():
            return path
    with contextlib.suppress(OSError):
        return next(projects.glob(f"*/{conversation_id}.jsonl"), None)
    return None


def codex_thread_name(conversation_id: str) -> str:
    name = ""
    with contextlib.suppress(OSError):
        with (codex_home() / "session_index.jsonl").open(encoding="utf-8") as index:
            for line in index:
                if conversation_id not in line:
                    continue
                with contextlib.suppress(ValueError):
                    entry = json.loads(line)
                    if isinstance(entry, dict) and entry.get("id") == conversation_id and isinstance(entry.get("thread_name"), str) and entry["thread_name"].strip():
                        name = entry["thread_name"]
    return name


def short_title(text: str, limit: int = 120) -> str:
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    cut = text[:limit - 1]
    space = cut.rfind(" ")
    return (cut[:space] if space > limit // 2 else cut).rstrip() + "…"


def review_card_ids(text: str) -> list[str]:
    """The review cards a prompt names with "Review card: <uuid>", lowercase, in order, without repeats."""
    ids: list[str] = []
    for match in REVIEW_CARD.finditer(text or ""):
        card = match.group(1).lower()
        if card not in ids:
            ids.append(card)
    return ids


# The lines of the Copy for agent / physical studying texts (Thoughts part/agentprompts.h and
# the phone's AgentPromptTemplates.kt): "Key: value" lines, and the sections whose "- " items follow.
_BLOCK_LINE = re.compile(
    r"^(Thoughts document|Project|Source|Built PDF|Project file|Document ID|Review card|Question|Note|Answer pages|Answer source|Answer text|Answer):")
_BLOCK_SECTION = re.compile(r"^(Answer feedback:|Current review \(|Physical studying:)")
_QUESTION = re.compile(r'^Question:\s*"?(.*?)"?\s*$')


def prompt_title(text: str) -> str:
    """The title a prompt gives its conversation: its first line, or for a prompt carrying a
    review card's Copy for agent text, the user's own words around it (else "Card: <question>")."""
    stripped = (text or "").strip()
    if not stripped:
        return ""
    lines = stripped.splitlines()
    if not review_card_ids(stripped):
        line = next((line for line in lines if line.strip()), "")
        return short_title(line) if line else ""
    own: list[str] = []
    question = ""
    in_section = False
    for raw in lines:
        line = raw.strip()
        if not line:
            in_section = False
            continue
        if _BLOCK_SECTION.match(line):
            in_section = True
            continue
        if in_section and (line.startswith("- ") or raw.startswith((" ", "\t"))):
            continue
        in_section = False
        if _BLOCK_LINE.match(line) or line.startswith("To change ") and "create_proposals" in line:
            if line.startswith("Question:") and not question:
                match = _QUESTION.match(line)
                question = match.group(1).strip() if match else ""
            continue
        own.append(line)
    if own:
        return short_title(own[0])
    return short_title("Card: " + question) if question else ""
