#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Prompt hook for Claude Code and Codex: shows the conversation on the Thoughts review card it asks about.

Registered as a UserPromptSubmit hook (Claude Code: ~/.claude/settings.json; Codex:
~/.codex/hooks.json). It reads the hook's JSON on stdin. A prompt with the Copy for agent line
"Review card: <uuid>" links this conversation to that card (and its Project) with
`thoughtsctl link-conversation`, in a detached process, so the prompt never waits for it
(card-agent-conversations spec section 3.1). It prints nothing: hook output would be added
to the conversation. Thoughts is never launched for it; errors go to
~/Library/Logs/thoughts-card-link.log only.

Installed next to the Thoughts MCP (~/plugins/thoughts/scripts/), with conversation_identity.py.
"""
from __future__ import annotations

import os
import sys

MARKER = "Review card:"
PROMPT_LIMIT = 60000  # characters handed to the detached process (the title needs the first lines)


def log_path():
    from pathlib import Path
    configured = os.environ.get("THOUGHTS_CARD_LINK_LOG")
    if configured:
        return Path(configured)
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Logs" / "thoughts-card-link.log"
    return Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state") / "thoughts-card-link.log"


def log(message: str) -> None:
    import time
    try:
        path = log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.stat().st_size > 1024 * 1024:
            os.replace(path, path.with_name(path.name + ".1"))
        with path.open("a", encoding="utf-8") as out:
            out.write(time.strftime("%Y-%m-%dT%H:%M:%S%z ") + message.replace("\n", " ") + "\n")
    except OSError:
        pass


def main() -> int:
    """The hook itself: returns at once; only a prompt naming a card starts the detached link."""
    try:
        data = sys.stdin.buffer.read()
    except OSError:
        return 0
    if MARKER.encode() not in data:
        return 0
    try:
        import json
        import subprocess
        event = json.loads(data)
        prompt = event.get("prompt") if isinstance(event, dict) else None
        if not isinstance(prompt, str) or MARKER not in prompt:
            return 0
        job = {key: event.get(key) for key in ("session_id", "transcript_path", "cwd", "hook_event_name")}
        # Codex's hook input carries turn_id; Claude Code's does not.
        job["agent"] = os.environ.get("THOUGHTS_CARD_LINK_AGENT") or ("codex" if "turn_id" in event else "claude")
        job["prompt"] = prompt[:PROMPT_LIMIT]
        child = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--link"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True)
        try:
            child.stdin.write(json.dumps(job).encode("utf-8"))
        finally:
            child.stdin.close()
    except Exception as error:  # never disturb the prompt
        log(f"hook failed: {error!r}")
    return 0


def identity_module():
    """conversation_identity.py: next to this script (installed), else in the repository's tools/mcp."""
    from pathlib import Path
    here = Path(__file__).resolve().parent
    for folder in (here, here.parent / "mcp"):
        if (folder / "conversation_identity.py").is_file():
            sys.path.insert(0, str(folder))
            break
    import conversation_identity
    return conversation_identity


def thoughtsctl() -> str:
    import shutil
    configured = os.environ.get("THOUGHTSCTL")
    if configured:
        return configured
    if sys.platform == "win32":
        installed = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Thoughts", "bin", "thoughtsctl.exe")
        return installed if os.path.exists(installed) else (shutil.which("thoughtsctl") or r"C:\CraftRoot\bin\thoughtsctl.exe")
    return "/Applications/Thoughts.app/Contents/MacOS/thoughtsctl"


def conversation_of(job: dict, identity) -> dict | None:
    """agent, conversation_id, conversation_url, session_name, title and title_source of the hook's conversation."""
    import time
    agent = job.get("agent") if job.get("agent") in ("claude", "codex") else "claude"
    conversation_id = ""
    if agent == "codex" and isinstance(job.get("transcript_path"), str):
        # The rollout file is named after the thread.
        match = identity.ROLLOUT.search(job["transcript_path"])
        conversation_id = match.group(1) if match else ""
    if not conversation_id and isinstance(job.get("session_id"), str):
        conversation_id = job["session_id"].lower()
    if not identity.UUID.fullmatch(conversation_id):
        log(f"no conversation id in the hook input (agent {agent})")
        return None
    record, url = None, ""
    if agent == "claude":
        # The sessions file may appear a moment after the first prompt: wait up to ~10 s for its link.
        wait = float(os.environ.get("THOUGHTS_CARD_LINK_SESSION_WAIT", "10"))
        started = time.monotonic()
        while True:
            record = identity.claude_session_by_id(conversation_id)
            url = identity.conversation_url(conversation_id) or ""
            elapsed = time.monotonic() - started
            if url or elapsed >= wait or (record is not None and elapsed >= min(2.0, wait)):
                break
            time.sleep(0.25)
        if not url.startswith("https://claude.ai/"):
            url = ""
    else:
        url = "codex://threads/" + conversation_id if identity.codex_rollout_exists(conversation_id) else ""
    session_name = record.get("name") if record and isinstance(record.get("name"), str) else ""
    title, source = "", "fallback"
    if agent == "claude" and session_name and record.get("nameSource") != "derived":
        title, source = session_name, "app"
    elif agent == "codex" and (name := identity.codex_thread_name(conversation_id)):
        title, source = name, "app"
    elif prompt := identity.prompt_title(job.get("prompt") or ""):
        title, source = prompt, "prompt"
    if not title:
        title = session_name or f"{'Codex' if agent == 'codex' else 'Claude'} conversation {conversation_id[:8]}"
    return {"agent": agent, "conversation_id": conversation_id, "conversation_url": url, "session_name": session_name[:100],
            "title": identity.short_title(title, 200), "title_source": source}


def link() -> int:
    """The detached process: one link-conversation per card the prompt names."""
    import json
    import subprocess
    try:
        job = json.loads(sys.stdin.buffer.read())
        identity = identity_module()
        cards = identity.review_card_ids(job.get("prompt") or "")
        if not cards:
            return 0
        conversation = conversation_of(job, identity)
        if conversation is None:
            return 0
        for card in cards:
            payload = {"review_card_id": card, **conversation, "linked_by": "tool", "card_linked_by": "prompt", "tool": "prompt"}
            try:
                done = subprocess.run([thoughtsctl(), "--json", "link-conversation"], input=json.dumps(payload).encode("utf-8"),
                                      capture_output=True, timeout=30)
            except (OSError, subprocess.SubprocessError) as error:
                log(f"card {card}: thoughtsctl failed: {error!r}")
                continue
            try:
                result = json.loads(done.stdout)
            except ValueError:
                result = {}
            if isinstance(result, dict) and result.get("accepted"):
                log(f"card {card}: {conversation['agent']} {conversation['conversation_id']} linked to {result.get('project_name')!r}"
                    f" (card_written {result.get('card_written')}, written {result.get('written')})")
            else:
                detail = (result.get("error_code"), result.get("message")) if isinstance(result, dict) and result else done.stderr.decode(errors="replace").strip()[:300]
                log(f"card {card}: not linked: {detail}")
    except Exception as error:
        log(f"link failed: {error!r}")
    return 0


if __name__ == "__main__":
    sys.exit(link() if sys.argv[1:] == ["--link"] else main())
