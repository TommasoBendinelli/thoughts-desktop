# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""thoughts-reply-waker with a fake Thoughts server, a fake claude and a fake ~/.claude (made-up sessions and notes)."""

import datetime as dt
import json
import os
import sys
import threading
import time
from pathlib import Path

import pytest

APP = Path(__file__).resolve().parent
sys.path.insert(0, str(APP))
import thoughts_reply_waker as waker_module  # noqa: E402
from thoughts_reply_waker import (  # noqa: E402
    Claude,
    Config,
    Decision,
    Run,
    State,
    Thoughts,
    Waker,
    grew_with,
    messenger_prompt,
    page_event,
    pointer,
    scan_live,
    session_lock,
    transcript_facts,
)

NOTE = "11111111-1111-4111-8111-111111111111"
REPLY = "22222222-2222-4222-8222-222222222222"
PAGE = "33333333-3333-4333-8333-333333333333"
STUDY = "44444444-4444-4444-8444-444444444444"  # a project session on this machine
OWNER = "55555555-5555-4555-8555-555555555555"  # a topic owner's session
DESK = "66666666-6666-4666-8666-666666666666"  # the fallback session
ENDED = "77777777-7777-4777-8777-777777777777"  # a Remote Control session that has ended
DEVICE = "laptop-device"


class FakeThoughts(Thoughts):
    """The calls the waker makes, answered from dictionaries; every call is recorded."""

    def __init__(self):
        self.calls = []
        self.handlings = {}
        self.claims = {}  # item -> list of (status, answer), used in order; default 201
        self.owners_list = []
        self.notes_list = []
        self.registrations = []
        self.sent = []  # notes the waker sent (notices to the owner)

    def handling(self, item_id):
        self.calls.append(("handling", item_id))
        return self.handlings[item_id]

    def claim(self, item_id, body):
        self.calls.append(("claim", item_id, body))
        answers = self.claims.get(item_id) or []
        return answers.pop(0) if answers else (201, {"claimed": True})

    def handover(self, item_id, machine, reason):
        self.calls.append(("handover", item_id, machine, reason))
        return {"handed_over": 1, "released": True}

    def register(self, machine, body):
        self.registrations.append((machine, body))
        return {"device_id": DEVICE, "machine": machine}

    def owners(self):
        return self.owners_list

    def notes(self, sent_by_me=None, limit=200):
        return self.notes_list

    def note(self, note_id):
        return next((note for note in self.notes_list if note.get("note_id") == note_id), {"note_id": note_id})

    def send_note(self, body):
        self.sent.append(body)
        self.notes_list.insert(0, {"note_id": body["note_id"], "title": body["title"], "origin": body["origin"]})
        return {"note": {"note_id": body["note_id"]}}

    def capabilities(self):
        return {"agent_note_wakers_v1"}

    change_feed: list = []  # change feed records, server_sequence ascending

    def changes(self, after, limit=1000):
        self.calls.append(("changes", after))
        rows = [row for row in self.change_feed if row["server_sequence"] > after]
        page = rows[:limit]
        return {
            "changes": page,
            "next_cursor": page[-1]["server_sequence"] if page else after,
            "has_more": len(rows) > limit,
        }


class FakeClaude(Claude):
    """Records each run; a message run appends the pointer to the target's transcript, as a real delivery does."""

    def __init__(self, config, sessions, deliver_to=None, results=None):
        self.config, self.sessions, self.stop = config, sessions, threading.Event()
        self.binary, self.version = "claude", "9.9.9"
        self.runs = []
        self.deliver_to = deliver_to  # transcript path the "messenger" writes into
        self.results = list(results or [])

    def run(self, argv, message, cwd, timeout):
        self.runs.append({"argv": argv, "message": message, "cwd": str(cwd)})
        if self.results:
            return self.results.pop(0)
        if "--no-session-persistence" in argv:
            if self.deliver_to is not None:
                text = message.split("----\n")[1]
                with self.deliver_to.open("a") as handle:
                    handle.write(json.dumps({"type": "user", "message": {"content": text}}) + "\n")
            return Run(0, {"result": "SENT", "total_cost_usd": 0.0009}, "", 1.0)
        return Run(0, {"result": "Sent note 'x'", "total_cost_usd": 0.12, "num_turns": 3}, "", 2.0)


@pytest.fixture
def home(tmp_path):
    claude = tmp_path / "claude"
    (claude / "sessions").mkdir(parents=True)
    (claude / "projects").mkdir()
    (tmp_path / "work").mkdir()
    return tmp_path


def make_config(home, **values):
    base = {
        "machine": "laptop",
        "claude_dir": str(home / "claude"),
        "desktop_sessions_dir": str(home / "desktop"),
        "thoughts_url": "http://127.0.0.1:9",
        "token_file": str(home / "token"),
        "exclude_folders": [],
        "confirm_seconds": 3,
        "busy_wait_seconds": 5,
        "claude_json": str(home / "claude.json"),
    }
    base.update(values)
    (home / "waker").mkdir(exist_ok=True)
    return Config(home / "waker", {**waker_module.DEFAULTS, **base})


def live_session(home, session_id, name="Eigenvalue study", status="idle", entrypoint="claude-desktop", **extra):
    entry = {
        "pid": os.getpid(),
        "sessionId": session_id,
        "cwd": str(home / "work"),
        "name": name,
        "status": status,
        "entrypoint": entrypoint,
        "startedAt": int((time.time() - 3600) * 1000),
        "updatedAt": int(time.time() * 1000),
        **extra,
    }
    path = home / "claude" / "sessions" / f"{abs(hash(session_id)) % 100000}.json"
    path.write_text(json.dumps(entry))
    return path


def transcript(home, session_id, mode="bypassPermissions", model="claude-test-model", age_hours=0.0, lines=None):
    folder = home / "claude" / "projects" / waker_module.project_folder(str(home / "work"))
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{session_id}.jsonl"
    entries = lines or [
        {"type": "user", "permissionMode": mode, "cwd": str(home / "work"), "message": {"content": "hi"}},
        {"type": "assistant", "requestedModel": model, "effort": "high", "message": {"model": model}},
        {"type": "custom-title", "customTitle": "Eigenvalue study"},
    ]
    path.write_text("".join(json.dumps(entry) + "\n" for entry in entries))
    when = time.time() - age_hours * 3600
    os.utime(path, (when, when))
    return path


def make_waker(home, thoughts=None, **values):
    config = make_config(home, **values)
    waker = Waker(config, thoughts=thoughts or FakeThoughts(), state=State(":memory:"))
    waker.device_id = DEVICE
    return waker


def reply_row(reply_id=REPLY, note_id=NOTE, title="Eigenvalues"):
    return {
        "id": f"reply:{reply_id}",
        "kind": "reply",
        "attempts": 0,
        "event_at": None,
        "payload": json.dumps({"reply_id": reply_id, "note_id": note_id, "note_title": title}),
    }


def target(session_id=STUDY, machine="laptop", role="sender", **extra):
    return {
        "state": "will_be_woken",
        "role": role,
        "device_id": DEVICE,
        "machine": machine,
        "conversation_id": session_id,
        "title": "Eigenvalue study",
        **extra,
    }


# --- sessions ----------------------------------------------------------------------------------------------


def test_scan_live_reads_the_registry_and_drops_dead_processes(home):
    live_session(home, STUDY, status="busy")
    live_session(home, OWNER, name=None, entrypoint="cli", status="waiting")
    dead = home / "claude" / "sessions" / "1.json"
    dead.write_text(json.dumps({"pid": 999999, "sessionId": DESK, "cwd": "/x", "status": "idle"}))
    found = scan_live(home / "claude" / "sessions")
    assert set(found) == {STUDY, OWNER}
    assert found[STUDY].kind == "desktop" and found[STUDY].status == "busy"
    assert found[OWNER].kind == "terminal" and found[OWNER].status == "busy"  # a dialog waits: not idle
    assert scan_live(home / "claude" / "sessions", exclude_cwds=(str(home / "work"),)) == {}


@pytest.mark.parametrize(
    "entry, kind",
    [
        ({"entrypoint": "claude-desktop", "bridgeSessionId": "b"}, "desktop"),
        ({"entrypoint": "sdk-cli", "bridgeSessionId": "b", "tmux": "claude-rc:@0.%0"}, "remote_control"),
        ({"entrypoint": "sdk-cli"}, "headless"),
        ({"entrypoint": "cli"}, "terminal"),
    ],
)
def test_session_kinds(entry, kind):
    assert waker_module.session_kind(entry) == kind


def test_transcript_facts_come_from_the_end(home, tmp_path):
    lines = (
        [{"type": "user", "permissionMode": "auto"}]
        + [{"type": "x", "pad": "y" * 1000}] * 2000
        + [
            {"type": "user", "permissionMode": "bypassPermissions", "cwd": "/work"},
            {"type": "assistant", "requestedModel": "claude-test-model", "effort": "xhigh"},
            {"type": "custom-title", "customTitle": "Late title"},
        ]
    )
    path = transcript(home, STUDY, lines=lines)
    facts = transcript_facts(path)
    assert facts["permission_mode"] == "bypassPermissions" and facts["model"] == "claude-test-model"
    assert facts["effort"] == "xhigh" and facts["title"] == "Late title" and facts["cwd"] == "/work"
    early = transcript(
        home,
        OWNER,
        lines=[{"type": "user", "permissionMode": "auto", "cwd": "/w"}] + [{"type": "x", "pad": "y" * 1000}] * 2000,
    )
    assert transcript_facts(early)["permission_mode"] == "auto"  # read further back when the end lacks it


def test_what_can_be_woken(home):
    waker = make_waker(home, resume_closed_kinds=["desktop", "terminal", "headless"])
    live_session(home, STUDY)
    live_session(home, OWNER, name=None, entrypoint="cli")
    transcript(home, DESK)
    transcript(home, ENDED)
    waker.state.remember_session(
        waker_module.LiveSession(ENDED, 1, str(home / "work"), "Old desk", "idle", "remote_control", None, None, None)
    )
    sessions = waker.sessions
    assert sessions.info(STUDY).wakeable is True
    unnamed = sessions.info(OWNER)
    assert unnamed.wakeable is False and "name" in unnamed.why_not
    closed = sessions.info(DESK)
    assert closed.state == "closed" and closed.kind == "unknown" and closed.wakeable is False  # never seen live
    ended = sessions.info(ENDED)
    assert ended.kind == "remote_control" and ended.wakeable is False and "remote_control" in ended.why_not
    assert make_waker(home).sessions.info(DESK).wakeable is True  # the Mac's default resumes unknown ones too
    assert sessions.info("88888888-8888-4888-8888-888888888888") is None


def test_a_closed_session_in_a_folder_nobody_trusted_is_resumed_only_where_its_rules_do_not_matter(home):
    transcript(home, STUDY, mode="acceptEdits")
    waker = make_waker(home)
    info = waker.sessions.info(STUDY)
    assert info.wakeable is False and "not trusted" in info.why_not
    (home / "claude.json").write_text(json.dumps({"projects": {str(home): {"hasTrustDialogAccepted": True}}}))
    assert waker.sessions.info(STUDY).wakeable is True  # a folder above it is trusted
    transcript(home, OWNER, mode="bypassPermissions")
    (home / "claude.json").write_text(json.dumps({"projects": {}}))
    assert waker.sessions.info(OWNER).wakeable is True


def test_excluded_and_test_only_sessions(home):
    live_session(home, STUDY)
    assert make_waker(home, exclude_sessions=[STUDY]).sessions.info(STUDY).wakeable is False
    assert make_waker(home, exclude_folders=[str(home / "work")]).sessions.info(STUDY) is None  # not even live
    assert make_waker(home, only_sessions=[OWNER]).sessions.info(STUDY).wakeable is False


def test_the_claude_apps_record_names_the_mode_of_a_closed_session(home):
    store = home / "desktop" / "a" / "b"
    store.mkdir(parents=True)
    (store / "local_1.json").write_text(
        json.dumps(
            {
                "cliSessionId": STUDY,
                "title": "From the app",
                "cwd": str(home / "work"),
                "permissionMode": "auto",
                "model": "claude-app-model",
                "effort": "medium",
                "isArchived": True,
            }
        )
    )
    transcript(home, STUDY, mode="bypassPermissions")
    info = make_waker(home).sessions.info(STUDY)
    assert (info.kind, info.title, info.permission_mode, info.model, info.archived) == (
        "desktop",
        "From the app",
        "auto",
        "claude-app-model",
        True,
    )
    # Archived sessions are left alone by default; the setting wakes them.
    assert info.wakeable is False and info.why_not == "archived in the Claude app"
    assert make_waker(home, wake_archived=True).sessions.info(STUDY).wakeable is True


def test_registration_lists_live_sessions_note_senders_and_the_fallback(home):
    thoughts = FakeThoughts()
    thoughts.notes_list = [
        {"sender_device_id": DEVICE, "origin": {"conversation_id": DESK}},
        {"origin": {"conversation_id": OWNER, "responder": True}},
    ]  # the old reply agent's
    waker = make_waker(home, thoughts, fallback_title="Study desk", fallback_after_hours=8)
    live_session(home, STUDY, name="Study desk")
    transcript(home, DESK)
    waker.refresh_conversations()
    body = waker.registration()
    assert [record["session_id"] for record in body["sessions"]] == [STUDY, DESK]
    assert body["fallback"] == {"session_id": STUDY, "title": "Study desk"}
    assert body["fallback_after_seconds"] == 8 * 3600 and body["heartbeat_seconds"] == 30
    assert body["sessions"][0]["state"] == "idle" and body["sessions"][1]["state"] == "closed"


# --- deciding ------------------------------------------------------------------------------------------------


def test_a_target_on_this_machine_is_claimed_for_its_session(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {"claim": None, "wake_target": target(), "covered_by": []}
    waker = make_waker(home, thoughts)
    decision = waker.decide(reply_row())
    assert (decision.action, decision.session_id) == ("deliver", STUDY) and "you sent the note" in decision.why
    assert thoughts.calls[-1] == ("claim", REPLY, {"conversation_id": STUDY})


def test_a_target_elsewhere_is_left_alone_or_taken_by_the_fallback_waker(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {"claim": None, "wake_target": target(machine="server"), "covered_by": []}
    assert make_waker(home, thoughts).decide(reply_row()).action == "skip"
    assert not [call for call in thoughts.calls if call[0] == "claim"]

    thoughts.claims[REPLY] = [(409, {"error_code": "agent_note_covered", "retry_after": 28000})]
    fallback = make_waker(home, thoughts, fallback_session_id=DESK)
    decision = fallback.decide(reply_row())
    assert decision.action == "wait" and decision.seconds == 600  # asks again every 10 minutes at most
    assert thoughts.calls[-1] == ("claim", REPLY, {"conversation_id": DESK, "responder": True})
    decision = fallback.decide(reply_row())
    assert decision.action == "deliver" and decision.session_id == DESK and decision.fallback is True


def test_the_fallback_waker_watches_a_reply_another_waker_claimed(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {
        "claim": {"device_id": "mac", "conversation_id": STUDY, "role": "sender"},
        "covered_by": [{"woken": True, "device_id": "mac", "conversation_id": STUDY}],
        "arrived_at": None,
    }
    assert make_waker(home, thoughts, fallback_session_id=DESK).decide(reply_row()).action == "watch"
    assert make_waker(home, thoughts).decide(reply_row()).action == "skip"
    thoughts.handlings[REPLY]["covered_by"] = []
    assert make_waker(home, thoughts, fallback_session_id=DESK).decide(reply_row()).action == "skip"


def test_a_wake_tried_again_finds_its_own_claim(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {
        "claim": {"device_id": DEVICE, "conversation_id": STUDY, "role": "sender"},
        "wake_target": target(),
        "covered_by": [],
    }
    decision = make_waker(home, thoughts).decide(reply_row())
    assert (decision.action, decision.session_id) == ("deliver", STUDY)
    assert not [call for call in thoughts.calls if call[0] == "claim"]
    thoughts.handlings[REPLY]["claim"]["role"] = "answered"  # the session answered meanwhile
    assert make_waker(home, thoughts).decide(reply_row()).action == "skip"


def test_the_fallback_sessions_own_note_is_claimed_as_its_sender(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {"claim": None, "wake_target": target(DESK), "covered_by": []}
    decision = make_waker(home, thoughts, fallback_session_id=DESK).decide(reply_row())
    assert decision.session_id == DESK and thoughts.calls[-1] == ("claim", REPLY, {"conversation_id": DESK})
    assert decision.fallback is True  # the last session there is: never handed over to itself


def test_a_users_choice_is_named_to_the_woken_session(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {
        "claim": None,
        "wake_target": target(OWNER, role="owner", label="Weights"),
        "covered_by": [],
        "answer_by": {"role": "owner"},
    }
    assert "chose you" in make_waker(home, thoughts).decide(reply_row()).why


def test_captures_go_to_their_owner_or_the_fallback(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    waker.registered = [{"session_id": OWNER, "wakeable": True}]
    capture = {"kind": "capture", "payload": {"name": "IMG_20260105_100000_1.jpg", "path": "/p", "media": "photo"}}
    assert waker.decide(capture).session_id == DESK
    thoughts.owners_list = [
        {"captures": True, "live": False, "device_id": DEVICE, "conversation_id": OWNER, "label": "Glasses"}
    ]
    assert waker.decide(capture).session_id == OWNER
    thoughts.owners_list[0]["live"] = True
    assert waker.decide(capture).action == "skip"
    waker.state.record_reply_capture("IMG_20260105_100000_1", REPLY)
    assert "reply" in waker.decide(capture).why


# --- waking ---------------------------------------------------------------------------------------------------


def test_a_live_session_gets_the_pointer_from_the_messenger(home):
    waker = make_waker(home)
    live_session(home, STUDY)
    path = transcript(home, STUDY, mode="bypassPermissions")
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(row, Decision("deliver", STUDY, "It is yours: you sent the note."))
    run = waker.claude.runs[0]
    assert "--no-session-persistence" in run["argv"] and run["argv"][run["argv"].index("--tools") + 1] == "SendMessage"
    assert run["argv"][run["argv"].index("--permission-mode") + 1] == "bypassPermissions"  # the session's own mode
    assert '"Eigenvalue study"' in run["message"] and REPLY in run["message"] and NOTE in run["message"]
    event = waker.state.event(row["id"])
    assert event["status"] == "awaiting_answer"  # delivered: the waker checks back for the answer
    assert event["not_before"] == pytest.approx(time.time() + 30 * 60, abs=5)
    logged = json.loads(waker.config.wakes_log.read_text().splitlines()[-1])
    assert logged["method"] == "message" and logged["confirmed"] == "read" and logged["messenger_cost_usd"] == 0.0009
    assert logged["claude_version"] == "9.9.9"


def test_a_busy_session_is_messaged_once_it_is_idle(home):
    waker = make_waker(home)
    registry = live_session(home, STUDY, status="busy")
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    waker.stop.wait = lambda seconds: (registry.write_text(registry.read_text().replace('"busy"', '"idle"')), False)[1]
    status, _detail = waker.wake(
        json.loads(json.dumps(reply_row())),
        json.loads(reply_row()["payload"]),
        Decision("deliver", STUDY, "x"),
        entry := {},
    )
    assert status == "done" and entry["waited_busy"] is False


def test_a_message_that_never_arrives_is_tried_again(home):
    waker = make_waker(home)
    live_session(home, STUDY)
    transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=None)
    waker.stop.wait = lambda seconds: time.sleep(0.01) or False
    status, detail = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), {})
    assert status == "retry" and "did not reach" in detail
    assert len(waker.claude.runs) == 1  # one message per attempt: a second one could reach the session twice


def test_two_sessions_with_one_name_are_told_apart_by_their_start(home):
    waker = make_waker(home)
    live_session(home, STUDY, name="Same name")
    other = home / "claude" / "sessions" / "other.json"
    other.write_text(
        json.dumps(
            {
                "pid": os.getpid(),
                "sessionId": OWNER,
                "cwd": str(home / "work"),
                "name": "Same name",
                "status": "idle",
                "entrypoint": "claude-desktop",
            }
        )
    )
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    assert waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), {})[0] == "done"
    run = waker.claude.runs[0]
    assert run["argv"][run["argv"].index("--tools") + 1] == "SendMessage,ListAgents"
    assert "about 1 hours ago" in run["message"] and "AMBIGUOUS" in run["message"]


def test_a_closed_session_idle_for_hours_is_trimmed_then_resumed_in_its_folder(home):
    waker = make_waker(home, budget_usd=3)
    transcript(home, STUDY, mode="bypassPermissions", model="claude-test-model", age_hours=5)
    waker.claude = FakeClaude(waker.config, waker.sessions)
    entry = {}
    status, detail = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), entry)
    assert status == "done" and "bypassPermissions" in detail
    trim, answer = waker.claude.runs
    assert trim["message"].startswith("/compact ") and "answering rules" in trim["message"]
    for run in (trim, answer):
        argv = run["argv"]
        assert run["cwd"] == str(home / "work")
        assert argv[argv.index("--resume") + 1] == STUDY
        assert argv[argv.index("--permission-mode") + 1] == "bypassPermissions"
        assert argv[argv.index("--model") + 1] == "claude-test-model" and argv[argv.index("--effort") + 1] == "high"
    assert float(trim["argv"][trim["argv"].index("--max-budget-usd") + 1]) == 3.0
    assert float(answer["argv"][answer["argv"].index("--max-budget-usd") + 1]) == pytest.approx(2.88)
    assert REPLY in answer["message"]
    assert entry["trimmed"] is True and entry["trim_cost_usd"] == 0.12 and entry["cost_usd"] == 0.12


def test_a_recently_active_closed_session_is_resumed_without_trimming(home):
    waker = make_waker(home, resume_extra_args=["--strict-mcp-config"])
    transcript(home, STUDY, age_hours=0.5)
    waker.claude = FakeClaude(waker.config, waker.sessions)
    assert waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), {})[0] == "done"
    assert len(waker.claude.runs) == 1 and waker.claude.runs[0]["argv"][-1] == "--strict-mcp-config"


def test_a_resumed_session_that_cannot_use_its_thoughts_tools_is_handed_over(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts)
    transcript(home, STUDY, age_hours=0.1)
    refused = Run(
        0,
        {
            "result": "I could not read it",
            "total_cost_usd": 0.01,
            "permission_denials": [{"tool_name": "mcp__thoughts__get_note_reply"}],
        },
        "",
        1,
    )
    waker.claude = FakeClaude(waker.config, waker.sessions, results=[refused])
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(row, Decision("deliver", STUDY, "x"))
    event = waker.state.event(row["id"])
    assert event["status"] == "queued" and event["detail"].startswith("handed over")  # decided again: the next one
    assert "mcp__thoughts__get_note_reply" in thoughts.calls[-1][3]


def test_a_session_opened_meanwhile_is_messaged_not_resumed(home):
    waker = make_waker(home)
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    info = waker.sessions.info(STUDY)
    assert info.live is None
    live_session(home, STUDY)  # the owner opened it after the waker looked
    status, detail = waker.resume(info, "pointer " + REPLY, REPLY, entry := {})
    assert status == "done" and entry["method"] == "message"
    assert all("--resume" not in run["argv"] for run in waker.claude.runs)


def test_a_session_that_cannot_be_woken_is_handed_over(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, resume_closed_kinds=["desktop"])
    transcript(home, STUDY)  # closed and never seen live: "unknown", not resumable here
    waker.claude = FakeClaude(waker.config, waker.sessions)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(row, Decision("deliver", STUDY, "x"))
    assert thoughts.calls[-1][:3] == ("handover", REPLY, "laptop")
    assert waker.state.event(row["id"])["status"] == "queued" and waker.state.event(row["id"])["target"] is None
    assert waker.claude.runs == []
    outcomes = [json.loads(line) for line in waker.config.wakes_log.read_text().splitlines()]
    assert [entry.get("outcome") or entry.get("status") for entry in outcomes] == ["failed", "handed_over"]


def test_a_failing_resume_is_tried_again_then_handed_over(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, max_attempts=2)
    transcript(home, STUDY, age_hours=0.1)
    waker.claude = FakeClaude(
        waker.config, waker.sessions, results=[Run(1, {"is_error": True, "result": "API Error"}, "", 1)] * 2
    )
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(dict(row), Decision("deliver", STUDY, "x"))
    assert waker.state.event(row["id"])["status"] == "waiting" and not [c for c in thoughts.calls if c[0] == "handover"]
    waker.deliver(dict(row, attempts=1), Decision("deliver", STUDY, "x"))
    assert waker.state.event(row["id"])["status"] == "queued" and thoughts.calls[-1][0] == "handover"


def test_the_fallback_session_is_tried_again_then_the_owner_is_told_never_handed_over(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, fallback_session_id=DESK, max_attempts=2)
    transcript(home, DESK, age_hours=0.1)
    waker.claude = FakeClaude(
        waker.config, waker.sessions, results=[Run(1, {"is_error": True, "result": "API Error"}, "", 1)] * 2
    )
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(dict(row), Decision("deliver", DESK, "x", fallback=True))
    assert waker.state.event(row["id"])["status"] == "waiting" and thoughts.sent == []
    waker.deliver(dict(row, attempts=1), Decision("deliver", DESK, "x", fallback=True))
    event = waker.state.event(row["id"])
    assert event["status"] == "unanswered" and "owner told" in event["detail"]
    assert len(thoughts.sent) == 1 and "Eigenvalues" in thoughts.sent[0]["title"]
    assert not [call for call in thoughts.calls if call[0] == "handover"]


def test_a_fallback_session_that_is_not_here_is_told_at_once(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.claude = FakeClaude(waker.config, waker.sessions)
    waker.deliver(row, Decision("deliver", DESK, "x", fallback=True))  # not on this machine at all
    assert waker.state.event(row["id"])["status"] == "unanswered" and len(thoughts.sent) == 1
    assert "not on this machine" in thoughts.sent[0]["message"]


def test_a_session_lock_lets_one_wake_at_a_time(tmp_path):
    order = []

    def hold(name, seconds):
        with session_lock(tmp_path, STUDY):
            order.append(f"{name} in")
            time.sleep(seconds)
            order.append(f"{name} out")

    first = threading.Thread(target=hold, args=("first", 0.3))
    first.start()
    time.sleep(0.05)
    hold("second", 0)
    first.join()
    assert order == ["first in", "first out", "second in", "second out"]


def test_the_worker_starts_one_delivery_per_session(home):
    thoughts = FakeThoughts()
    for reply_id in (REPLY, PAGE):
        thoughts.handlings[reply_id] = {"claim": None, "wake_target": target(), "covered_by": []}
    waker = make_waker(home, thoughts)
    started = []
    waker.deliver = lambda event, decision: (started.append(event["id"]), time.sleep(0.3))
    for reply_id in (REPLY, PAGE):
        row = reply_row(reply_id)
        waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    first = waker.state.ready(set())
    waker.handle(first, set())
    running = {STUDY}
    assert waker.state.ready(running)["id"] == f"reply:{PAGE}"
    waker.handle(waker.state.ready(running), running)
    assert waker.state.event(f"reply:{PAGE}")["status"] == "waiting"  # its session has a delivery running
    assert started == [f"reply:{REPLY}"]


# --- what is said ------------------------------------------------------------------------------------------------


def test_the_pointer_only_points(home):
    text = pointer(
        "reply",
        {"reply_id": REPLY, "note_id": NOTE, "note_title": "Eigenvalues"},
        STUDY,
        "laptop",
        "It is yours: you sent the note.",
    )
    assert REPLY in text and NOTE in text and STUDY in text
    assert "get_note_reply" in text and "get_note(" in text and "no request or approval" in text
    page = pointer("page", {"note_id": PAGE, "title": "Linear algebra, page 3"}, OWNER, "server", "x")
    assert "include_note_image=true" in page
    capture = pointer(
        "capture",
        {"name": "IMG_1.jpg", "path": "/g/IMG_1.jpg", "media": "photo"},
        DESK,
        "server",
        "x",
        ["Read the photo at full resolution: /g/IMG_1.jpg."],
    )
    assert "/g/IMG_1.jpg" in capture and "note_rules" in capture


def test_the_messenger_copies_the_message_exactly():
    prompt = messenger_prompt("Eigenvalue study", "line one\nline two", None)
    assert prompt.split("----\n")[1] == "line one\nline two\n"
    assert "ListAgents" not in prompt
    assert "ListAgents" in messenger_prompt("x", "y", "about 2 hours ago")


def test_delivery_is_seen_in_the_transcript(tmp_path):
    path = tmp_path / "t.jsonl"
    path.write_text(json.dumps({"type": "user", "message": {"content": "old " + REPLY}}) + "\n")
    offset = path.stat().st_size
    assert grew_with(path, offset, REPLY) is None
    with path.open("a") as handle:
        handle.write(json.dumps({"type": "queue-operation", "content": "pointer " + REPLY}) + "\n")
    assert grew_with(path, offset, REPLY) == "queued"
    with path.open("a") as handle:
        handle.write(json.dumps({"type": "user", "message": {"content": "pointer " + REPLY}}) + "\n")
    assert grew_with(path, offset, REPLY) == "read"


def test_runs_report_cost_and_problems():
    assert Run(0, {"total_cost_usd": 0.0123456789}, "", 1).cost == 0.012346
    assert Run(0, {"is_error": True, "result": "Budget exceeded"}, "", 1).ok is False
    assert Run(1, {}, "boom", 1).problem() == "boom"


def test_claude_runs_never_update_and_never_inherit_an_agent_shell(home, monkeypatch):
    monkeypatch.setenv("CLAUDE_CODE_ENTRYPOINT", "claude-desktop")
    monkeypatch.setenv("CLAUDECODE", "1")
    env = waker_module.claude_env(make_config(home))
    assert env["DISABLE_AUTOUPDATER"] == "1" and "CLAUDE_CODE_ENTRYPOINT" not in env and "CLAUDECODE" not in env
    assert waker_module.permission_args("bypassPermissions") == ["--permission-mode", "bypassPermissions"]
    assert waker_module.permission_args("something else") == ["--permission-mode", "default"]


def test_status_and_sessions_commands_run_without_a_server(home, monkeypatch, capsys):
    config = make_config(home, token_file=None)
    live_session(home, STUDY)
    monkeypatch.setattr(waker_module.Config, "load", classmethod(lambda cls, home=None: config))
    waker_module.main(["sessions"])
    assert STUDY in capsys.readouterr().out
    waker_module.main(["status"])
    assert "no state yet" in capsys.readouterr().out


def test_the_mac_note_tool_with_a_dataclass_loads_and_lends_its_tls_context(tmp_path, monkeypatch):
    # 14 Nov 2026: the Mac's agent_notes.py defines dataclasses; loaded without being registered in
    # sys.modules it failed at import, and python.org builds need its CA context to reach the server.
    tool = tmp_path / "agent_notes.py"
    tool.write_text(
        "import ssl\n"
        "from dataclasses import dataclass\n\n"
        "@dataclass\n"
        "class Settings:\n"
        "    server_url: str\n"
        "    token: str\n\n"
        "def load_config():\n"
        "    return Settings('https://example-server.test', 'not-a-real-token')\n\n"
        "CONTEXT = ssl.create_default_context()\n\n"
        "def ssl_context():\n"
        "    return CONTEXT\n",
        encoding="utf-8",
    )
    home = tmp_path / "home"
    home.mkdir()
    config = make_config(home, mac_agent_notes=str(tool), token_file=None, thoughts_url=None)
    thoughts = waker_module.Thoughts.from_config(config)
    assert thoughts.url == "https://example-server.test"
    assert thoughts._token() == "not-a-real-token"
    assert thoughts._ssl_context is sys.modules["waker_mac_agent_notes"].CONTEXT


# --- 19 Nov: shell status, bounded attempts, sign-in failures, answers, hand-over after 30 minutes, notices ---------

from thoughts_reply_waker import (  # noqa: E402
    answer_note,
    answer_words,
    work_rows,
    fallback_reason,
    follow_up_action,
    live_status,
    notice_image,
    notice_text,
    taken_up,
)


def iso_ago(minutes):
    return waker_module.iso(time.time() - minutes * 60)


def answer(note_id="88888888-8888-4888-8888-888888888888", device="mac", conversation=STUDY, minutes_ago=5, **extra):
    return {
        "note_id": note_id,
        "sender_device_id": device,
        "created_at": iso_ago(minutes_ago),
        "origin": {"agent": "mcp", "conversation_id": conversation},
        **extra,
    }


def claimed(device="mac", conversation=STUDY, role="sender", minutes_ago=40, arrived_minutes_ago=41):
    return {
        "item_id": REPLY,
        "note_id": NOTE,
        "arrived_at": iso_ago(arrived_minutes_ago),
        "claim": {"device_id": device, "conversation_id": conversation, "role": role, "claimed_at": iso_ago(minutes_ago)},
        "covered_by": [{"woken": True, "device_id": device, "conversation_id": conversation, "role": "sender"}],
    }


@pytest.mark.parametrize(
    "raw, status", [("idle", "idle"), ("shell", "idle"), ("busy", "busy"), ("waiting", "busy"), (None, "busy")]
)
def test_a_finished_turn_with_background_shells_is_idle(raw, status):
    # Claude Code writes "shell" for an idle session whose Monitor watchers run (Inbox on 19 Nov).
    assert live_status(raw) == status


def test_a_session_whose_watchers_run_is_messaged_at_once(home):
    waker = make_waker(home, busy_wait_seconds=1800)
    live_session(home, STUDY, status="shell")
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    waits = []
    waker.stop.wait = lambda seconds: waits.append(seconds) or False
    entry = {"event": "reply:" + REPLY}
    status, _ = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), entry)
    assert status == "done" and entry["waited_busy"] is False and entry["session_status"] == "shell"
    assert waits == []  # no waiting for "idle"
    assert waker.registration()["sessions"][0]["state"] == "idle"


def test_a_busy_session_is_messaged_after_a_short_wait_and_the_message_queues(home):
    waker = make_waker(home, busy_wait_seconds=0.05)
    live_session(home, STUDY, status="busy")
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    waker.stop.wait = lambda seconds: time.sleep(0.02) or False
    entry = {}
    status, _ = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), entry)
    assert status == "done" and entry["waited_busy"] is True and len(waker.claude.runs) == 1


def test_stopping_the_waker_never_sends_a_message(home):
    # 19 Nov: stopping the service ended the busy wait and the old code went on to message the session.
    waker = make_waker(home, busy_wait_seconds=60)
    live_session(home, STUDY, status="busy")
    transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions)
    waker.stop.set()
    status, _ = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), {})
    assert status == "interrupted" and waker.claude.runs == []


def test_one_attempt_ends_within_its_time(home):
    waker = make_waker(home, attempt_seconds=40, confirm_seconds=60, busy_wait_seconds=60)
    live_session(home, STUDY, status="busy")
    transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=None)
    clock = [time.time()]
    waker_module.time.time, real = (lambda: clock[0]), waker_module.time.time
    try:
        waker.stop.wait = lambda seconds: clock.__setitem__(0, clock[0] + seconds) or False
        started = clock[0]
        status, detail = waker.wake(reply_row(), json.loads(reply_row()["payload"]), Decision("deliver", STUDY, "x"), {})
        assert status == "retry" and clock[0] - started <= 40 + 2
        timeout = waker.claude.runs[0]
        assert "did not reach" in detail
    finally:
        waker_module.time.time = real
    assert timeout  # the messenger ran once


def test_a_pointer_that_arrived_late_is_not_sent_again(home):
    waker = make_waker(home)
    live_session(home, STUDY)
    path = transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=None)
    waker.stop.wait = lambda seconds: time.sleep(0.01) or False
    row = reply_row()
    payload = json.loads(row["payload"])
    first = {"event": row["id"]}
    assert waker.wake(row, payload, Decision("deliver", STUDY, "x"), first)[0] == "retry"
    sent = waker.claude.runs[0]["message"].split("----\n")[1]
    with path.open("a") as handle:  # it reached the session after the attempt had given up
        handle.write(json.dumps({"type": "queue-operation", "operation": "enqueue", "content": sent}) + "\n")
    second = {"event": row["id"]}
    status, detail = waker.wake(row, payload, Decision("deliver", STUDY, "x"), second)
    assert status == "done" and "earlier attempt" in detail and len(waker.claude.runs) == 1


EXPIRED = (
    "Failed to authenticate: this login reached its time limit and cannot be renewed. Run `claude auth login` to "
    "sign in again"
)


def test_an_expired_login_holds_the_fallback_item_and_tells_the_owner_once(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    live_session(home, DESK, name="Inbox", status="shell")
    transcript(home, DESK)
    waker.claude = FakeClaude(waker.config, waker.sessions, results=[Run(1, {"is_error": True, "result": EXPIRED}, "", 1)] * 4)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(dict(row), Decision("deliver", DESK, "x", fallback=True))
    event = waker.state.event(row["id"])
    assert event["status"] == "waiting" and event["not_before"] == pytest.approx(time.time() + 600, abs=5)
    assert "cannot sign in" in event["detail"] and len(waker.claude.runs) == 1
    waker.deliver(dict(row, attempts=1), Decision("deliver", DESK, "x", fallback=True))
    assert waker.state.event(row["id"])["status"] == "waiting"  # never 'failed' while the login is the problem
    assert len(thoughts.sent) == 1 and "claude auth login" in thoughts.sent[0]["message"]  # once per outage
    assert thoughts.sent[0]["origin"]["kind"] == "waker_notice" and thoughts.sent[0]["origin"]["responder"] is True


def test_an_expired_login_on_a_project_machine_hands_the_item_on(home):
    thoughts = FakeThoughts()
    waker = make_waker(home, thoughts)
    live_session(home, STUDY)
    transcript(home, STUDY)
    waker.claude = FakeClaude(waker.config, waker.sessions, results=[Run(1, {"is_error": True, "result": EXPIRED}, "", 1)])
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.deliver(dict(row), Decision("deliver", STUDY, "x"))
    assert thoughts.calls[-1][:3] == ("handover", REPLY, "laptop")
    assert waker.state.event(row["id"])["status"] == "queued"


def test_answers_are_seen_from_the_server_or_the_claimants_later_note():
    handling = claimed()
    note = {"note_id": NOTE, "document_id": "doc-1"}
    assert answer_note({**handling, "claim": {**handling["claim"], "role": "answered", "answer_note_id": "n1"}}, note, []) == "n1"
    assert answer_note({**handling, "inferred_answer_note_id": "n2"}, note, []) == "n2"
    later = answer(document_id="doc-1")
    assert answer_note(handling, note, [later]) == later["note_id"]
    assert answer_note(handling, note, [answer()]) == answer()["note_id"]  # a note on no document counts too
    assert answer_note(handling, note, [answer(minutes_ago=50)]) is None  # sent before the reply arrived
    first, second = answer(note_id="first", minutes_ago=30), answer(note_id="second", minutes_ago=2)
    assert answer_note(handling, note, [second, first]) == "first"  # the first note after the reply
    assert answer_note(handling, note, [answer(conversation=OWNER)]) is None  # another session
    assert answer_note(handling, note, [answer(device="phone")]) is None
    on_another = answer(document_id="doc-2")
    assert answer_note(handling, note, [on_another]) == on_another["note_id"]  # the session answered on another document
    deviceless = {**handling, "claim": {**handling["claim"], "conversation_id": None}}
    other_device_note = answer(document_id="doc-2", origin={})
    assert answer_note(deviceless, note, [other_device_note]) is None  # no conversation: a note about something else
    notice = answer(origin={"kind": "waker_notice", "responder": True, "conversation_id": STUDY})
    assert answer_note(handling, note, [notice]) is None
    responder = answer(origin={"conversation_id": STUDY, "responder": True})
    assert answer_note(handling, note, [responder]) is None
    as_responder = claimed(role="responder")
    assert answer_note(as_responder, note, [responder]) == responder["note_id"]
    assert answer_note({**handling, "claim": None}, note, [later]) is None  # unclaimed: the server's own inference


def proposal_change(sequence, conversation=STUDY, minutes_ago=5, proposal_id="p1", status="pending"):
    created = (dt.datetime.now(dt.UTC) - dt.timedelta(minutes=minutes_ago)).isoformat().replace("+00:00", "Z")
    snapshot = {"proposal_id": proposal_id, "source_file": "docs-x/tex/source/a.tex", "conversation_id": conversation,
                "created_at": created}
    return {"server_sequence": sequence, "entity_type": "proposal", "entity_id": proposal_id,
            "payload": {"proposal_id": proposal_id, "snapshot": snapshot, "status": status,
                        "deleted": status != "pending", "first_seen_at": created}}


def test_a_proposal_or_overlay_of_the_claimed_session_answers_the_item():
    # A reply asking for a citation, answered with a proposal the owner accepted at once, was reported hours later as
    # having no answer.
    handling = claimed()
    note = {"note_id": NOTE, "document_id": "doc-1"}
    later = {"key": "proposal p1", "created_at": answer()["created_at"]}
    assert answer_note(handling, note, [], [later]) == "proposal p1"
    earlier = {"key": "proposal p0", "created_at": answer(minutes_ago=50)["created_at"]}
    assert answer_note(handling, note, [], [earlier]) is None  # made before the reply arrived
    deviceless = {**handling, "claim": {**handling["claim"], "conversation_id": None}}
    assert answer_note(deviceless, note, [], [later]) is None
    feed = [
        proposal_change(10, minutes_ago=50, proposal_id="old"),
        {"server_sequence": 11, "entity_type": "review_event", "entity_id": "r", "payload": {"conversation_id": STUDY}},
        proposal_change(12, conversation=OWNER, proposal_id="other"),
        proposal_change(13, proposal_id="p1", status="accepted"),  # accepted already: it left the projections
        {"server_sequence": 14, "entity_type": "reading_overlay", "entity_id": "o1",
         "payload": {"author": {"conversation_id": STUDY}, "created_at": answer()["created_at"]}},
    ]
    rows = work_rows(feed)
    assert [row[0] for row in rows] == ["proposal old", "proposal other", "proposal p1", "reading overlay o1"]
    assert answer_words("proposal p1") == "proposal p1" and answer_words("n1") == "note n1"


def test_the_waker_reads_the_change_feed_for_what_the_session_made(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(minutes_ago=300, arrived_minutes_ago=301)
    thoughts.change_feed = [proposal_change(5000 + n, minutes_ago=400, proposal_id=f"old{n}") for n in range(3)]
    thoughts.change_feed += [proposal_change(5003, minutes_ago=290, proposal_id="p1", status="accepted")]
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    event = waker.state.event(row["id"])
    assert event["status"] == "answered" and "proposal p1" in event["detail"] and thoughts.sent == []
    assert waker.state.get("work_cursor") == 5003
    thoughts.calls.clear()
    waker.scan_work()
    assert [call for call in thoughts.calls if call[0] == "changes"] == [("changes", 5003)]  # goes on from there


def test_the_fallback_is_told_why_an_item_is_its_own():
    covered = lambda *entries: {"covered_by": list(entries)}  # noqa: E731
    sender = {"conversation_id": DESK, "role": "sender", "device_id": "server"}
    assert "you sent the note" in fallback_reason(covered(sender), DESK)
    owner = {"conversation_id": DESK, "role": "owner", "label": "OPD", "woken": True}
    assert "own the topic" in fallback_reason(covered(owner), DESK)
    handed = {"conversation_id": STUDY, "woken": True, "handed_over_at": "x"}
    assert "handed it over" in fallback_reason(covered(handed), DESK)
    assert "offline" in fallback_reason(covered({"conversation_id": STUDY, "woken": True}), DESK)
    assert "10 minutes" in fallback_reason(covered({"conversation_id": STUDY}), DESK)
    assert "no other home" in fallback_reason(covered(), DESK)


@pytest.mark.parametrize(
    "answered, took_up, fallback, minutes, action",
    [
        (True, False, False, 5, "answered"),
        (False, False, False, 10, "wait"),
        (False, False, False, 31, "hand_over"),
        (False, False, True, 31, "tell"),  # the fallback hands over to nobody: the owner hears of it
        (False, True, False, 31, "wait"),  # it read the reply and works on it
        (False, True, False, 4 * 60 + 1, "tell"),
    ],
)
def test_what_happens_when_the_waker_checks_back(home, answered, took_up, fallback, minutes, action):
    assert follow_up_action(answered, took_up, fallback, minutes * 60, make_config(home)) == action


def test_taking_an_item_up_is_a_tool_call_about_it(tmp_path):
    path = tmp_path / "t.jsonl"
    pointer_line = {"type": "user", "message": {"content": f"[Thoughts reply waker on mac, reply {REPLY}] note {NOTE}"}}
    path.write_text(json.dumps(pointer_line) + "\n")
    assert taken_up(path, 0, [REPLY, NOTE]) is False  # the pointer itself names the ids
    offset = path.stat().st_size
    call = {"type": "tool_use", "name": "mcp__thoughts__get_note_reply", "input": {"note_id": NOTE}}
    with path.open("a") as handle:
        handle.write(json.dumps({"type": "assistant", "message": {"content": [call]}}) + "\n")
    assert taken_up(path, 0, [REPLY, NOTE]) is True
    assert taken_up(path, path.stat().st_size, [REPLY, NOTE]) is False
    assert taken_up(path, offset, [REPLY, NOTE]) is True


def test_the_fallback_stops_watching_a_claim_that_was_answered(home):
    # 19 Nov: two replies claimed for a Mac session that answered them with new notes were watched every 5 minutes.
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(minutes_ago=300, arrived_minutes_ago=301)
    thoughts.notes_list = [answer(minutes_ago=290)]
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    event = waker.state.event(row["id"])
    assert event["status"] == "answered" and "88888888" in event["detail"] and thoughts.sent == []
    logged = json.loads(waker.config.wakes_log.read_text().splitlines()[-1])
    assert logged["outcome"] == "answered" and logged["event"] == row["id"]


def test_the_fallback_watches_a_fresh_claim_then_tells_the_owner_once(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(minutes_ago=20)
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    assert (waker.decide(reply_row()).action, waker.decide(reply_row()).seconds) == ("watch", 300)
    thoughts.handlings[REPLY] = claimed(minutes_ago=90)
    assert waker.decide(reply_row()).seconds == 600
    thoughts.handlings[REPLY] = claimed(minutes_ago=4 * 60 + 16)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "unanswered" and len(thoughts.sent) == 1
    assert thoughts.sent[0]["origin"]["item_id"] == REPLY and "did not hand it over" in thoughts.sent[0]["message"]
    # Another waker (or this one after a restart) finds the notice on the server and sends none.
    other = make_waker(home, thoughts, fallback_session_id=DESK)
    other.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    other.handle(other.state.ready(set()), set())
    assert len(thoughts.sent) == 1


def deliver_and_age(home, thoughts, minutes, session=STUDY, **values):
    waker = make_waker(home, thoughts, **values)
    live_session(home, session)
    path = transcript(home, session)
    waker.claude = FakeClaude(waker.config, waker.sessions, deliver_to=path)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    fallback = session == values.get("fallback_session_id")
    waker.deliver(dict(row), Decision("deliver", session, "x", fallback=fallback))
    delivered = waker.state.get(f"delivered:{row['id']}")
    waker.state.put(f"delivered:{row['id']}", {**delivered, "at": time.time() - minutes * 60})
    waker.state.update(row["id"], not_before=0)
    return waker, row, path


def test_a_woken_session_that_does_not_take_the_reply_up_is_handed_over_after_30_minutes(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(device=DEVICE, minutes_ago=31)
    waker, row, _ = deliver_and_age(home, thoughts, 31)
    waker.handle(waker.state.ready(set()), set())
    assert thoughts.calls[-1][:3] == ("handover", REPLY, "laptop") and "did not take it up" in thoughts.calls[-1][3]
    event = waker.state.event(row["id"])
    assert event["status"] == "queued" and event["target"] is None  # decided again: the next session
    thoughts.handlings[REPLY] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"),
                                 "covered_by": []}
    waker.state.update(row["id"], not_before=0)
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "skipped"  # the fallback on the server takes it


def test_a_woken_session_that_read_the_reply_gets_more_time(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(device=DEVICE, minutes_ago=31)
    waker, row, path = deliver_and_age(home, thoughts, 31)
    call = {"type": "tool_use", "name": "mcp__thoughts__get_note_reply", "input": {"note_id": NOTE}}
    with path.open("a") as handle:
        handle.write(json.dumps({"type": "assistant", "message": {"content": [call]}}) + "\n")
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "awaiting_answer"
    assert not [c for c in thoughts.calls if c[0] == "handover"] and thoughts.sent == []
    thoughts.notes_list = [answer(device=DEVICE, minutes_ago=1)]
    waker.state.update(row["id"], not_before=0)
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "answered"


def test_the_fallback_session_that_does_not_answer_is_reported_not_handed_over(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = claimed(device=DEVICE, conversation=DESK, role="responder", minutes_ago=31)
    waker, row, _ = deliver_and_age(home, thoughts, 31, session=DESK, fallback_session_id=DESK)
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "unanswered" and len(thoughts.sent) == 1
    assert not [c for c in thoughts.calls if c[0] == "handover"]


def test_a_retried_wake_of_an_answered_item_is_not_delivered_again(home):
    thoughts = FakeThoughts()
    thoughts.handlings[REPLY] = {**claimed(device=DEVICE, minutes_ago=3, arrived_minutes_ago=4), "wake_target": target()}
    thoughts.notes_list = [answer(device=DEVICE, minutes_ago=1)]
    assert make_waker(home, thoughts).decide(reply_row()).action == "finish"


def test_a_lost_delivery_is_offered_again(home):
    waker = make_waker(home)
    row = reply_row()
    waker.state.add_event({"id": row["id"], "kind": "reply", "payload": json.loads(row["payload"])})
    waker.state.update(row["id"], status="delivering", target=STUDY)
    waker.reclaim_lost(set())
    assert waker.state.event(row["id"])["status"] == "queued"
    assert json.loads(waker.config.wakes_log.read_text().splitlines()[-1])["outcome"] == "lost"


def test_a_capture_its_owner_could_not_take_goes_to_the_fallback(home):
    thoughts = FakeThoughts()
    thoughts.owners_list = [{"captures": True, "live": False, "device_id": DEVICE, "conversation_id": OWNER}]
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    waker.registered = [{"session_id": OWNER, "wakeable": True}]
    capture = {"kind": "capture", "payload": {"name": "IMG_20260105_100000_1.jpg", "path": "/p", "tried": [OWNER]}}
    decision = waker.decide(capture)
    assert decision.session_id == DESK and "could not be woken" in decision.why


def test_the_notice_names_the_reply_and_why(home):
    payload = {"reply_id": REPLY, "note_id": NOTE, "note_title": "OPD blog 1/6", "created_at": "2026-11-19T16:05:23Z"}
    title, message = notice_text("reply", payload, "Inbox did not take it up within 31 minutes.",
                                 make_config(home, local_zone="Europe/Berlin").zone())
    assert title == "Unanswered reply: OPD blog 1/6"
    assert "19 Nov 17:05" in message and "Inbox did not take it up" in message and "nothing else was started" in message
    image = notice_image(title, message)
    assert image.startswith(b"\x89PNG\r\n\x1a\n")


# --- Thoughts' Send to Agent: Copy for agent's text for one conversation -----------------------------------------


def copy_row(conversation_id=STUDY):
    event = page_event({"note_id": PAGE, "title": "Copy for agent: main · page 3", "created_at": None,
                        "origin": {"kind": "phone_page", "copy_for_agent": True, "conversation_id": conversation_id}})
    return {**event, "attempts": 0, "event_at": None, "payload": json.dumps(event["payload"])}


def test_copy_for_agent_text_is_pointed_to_as_pasted_context(home):
    payload = json.loads(copy_row()["payload"])
    assert payload == {"note_id": PAGE, "title": "Copy for agent: main · page 3", "created_at": None,
                       "copy_for_agent": True, "conversation_id": STUDY}
    text = pointer("page", payload, STUDY, "laptop", "It is yours.")
    assert "Copy for agent text" in text and f'get_note(note_id="{PAGE}", conversation_id="{STUDY}")' in text
    assert "the tool's next_step says what to do with it" in text and "no request or approval" in text
    # Any other page keeps its wording.
    plain = pointer("page", {"note_id": PAGE, "title": "Linear algebra, page 3"}, OWNER, "server", "x")
    assert "Copy for agent" not in plain and "include_note_image=true" in plain


def test_copy_for_agent_text_goes_to_its_conversation_and_never_to_the_fallback(home):
    thoughts = FakeThoughts()
    # Its own conversation, here: delivered.
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(), "covered_by": []}
    decision = make_waker(home, thoughts).decide(copy_row())
    assert decision.action == "deliver" and decision.session_id == STUDY
    # Elsewhere: the fallback waker waits while that conversation's waker covers it...
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(machine="server"), "covered_by": []}
    thoughts.claims[PAGE] = [(409, {"error_code": "agent_note_covered", "retry_after": 600})]
    fallback = make_waker(home, thoughts, fallback_session_id=DESK)
    assert fallback.decide(copy_row()).action == "wait"
    # ...and when nobody covers it any more, tells the owner instead of taking it.
    decision = fallback.decide(copy_row())
    assert decision.action == "tell" and "could not be woken" in decision.why
    # Sent to the fallback conversation itself: delivered there.
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    decision = fallback.decide(copy_row(DESK))
    assert decision.action == "deliver" and decision.session_id == DESK


def test_the_fallback_does_not_watch_copy_for_agent_text_another_waker_delivered(home):
    # 23 Nov: Copy for agent pages a Mac session read and answered in its chat were reported as unanswered after 4 h.
    thoughts = FakeThoughts()
    thoughts.handlings[PAGE] = {**claimed(minutes_ago=4 * 60 + 16), "item_id": PAGE, "note_id": PAGE}
    waker = make_waker(home, thoughts, fallback_session_id=DESK)
    decision = waker.decide(copy_row())
    assert decision.action == "skip" and "Copy for agent text delivered" in decision.why
    row = copy_row()
    waker.state.add_event({"id": row["id"], "kind": "page", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    assert waker.state.event(row["id"])["status"] == "skipped" and thoughts.sent == []
    # Any other page claimed by another waker is still watched.
    thoughts.handlings[PAGE] = {**claimed(minutes_ago=20), "item_id": PAGE, "note_id": PAGE}
    plain = page_event({"note_id": PAGE, "title": "Linear algebra, page 3", "created_at": None, "origin": {"kind": "phone_page"}})
    plain_row = {**plain, "attempts": 0, "event_at": None, "payload": json.dumps(plain["payload"])}
    assert waker.decide(plain_row).action == "watch"


# --- a new session from the phone (origin.start_session) ---------------------------------------------------


def start_row():
    event = page_event({"note_id": PAGE, "title": "Start a study session", "created_at": None,
                        "origin": {"kind": "phone_page", "start_session": True, "copy_for_agent": True}})
    return {**event, "attempts": 0, "event_at": None, "payload": json.dumps(event["payload"])}


def test_a_start_session_page_is_started_once_by_the_waker_that_starts_sessions(home):
    assert json.loads(start_row()["payload"]) == {"note_id": PAGE, "title": "Start a study session", "created_at": None,
                                                  "start_session": True, "agent": "claude", "where": "computer"}
    thoughts = FakeThoughts()
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    # Not the starting machine (the Mac): never a session of its own for it.
    assert make_waker(home, thoughts).decide(start_row()).action == "skip"
    starter = make_waker(home, thoughts, fallback_session_id=DESK)
    decision = starter.decide(start_row())
    assert decision.action == "start" and decision.session_id is None
    assert make_waker(home, thoughts, start_sessions=True).decide(start_row()).action == "start"
    # Started already (claimed): not twice.
    thoughts.handlings[PAGE] = {"claim": {"conversation_id": STUDY, "device_id": DEVICE, "role": "woken"}}
    assert starter.decide(start_row()).action == "skip"


def test_starting_a_session_claims_the_page_and_tells_the_owner(home, monkeypatch):
    thoughts = FakeThoughts()
    thoughts.notes_list = [{"note_id": PAGE, "title": "Start a study session", "message": "Agent study mode is on.\n- Watch."}]
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    waker = make_waker(home, thoughts, fallback_session_id=DESK, start_session_folder=str(home / "study"))
    waker.claude = FakeClaude(waker.config, waker.sessions)
    calls = []

    def started(claude, name, folder, prompt):
        calls.append((name, folder, prompt))
        return waker_module.Started(True, "c3d0acd8", "c3d0acd8-0147-4cc0-aff3-012a22bd0e62", "backgrounded · c3d0acd8")

    monkeypatch.setattr(waker_module, "start_background", started)
    row = start_row()
    waker.state.add_event({"id": row["id"], "kind": "page", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    name, folder, prompt = calls[0]
    assert name.startswith("Study session · ") and folder == home / "study" and folder.is_dir()
    assert prompt.startswith("[Thoughts reply waker on laptop: the owner started this session from their phone at ")
    assert prompt.endswith("Their prompt for it follows.]\n\nAgent study mode is on.\n- Watch.")
    assert ("claim", PAGE, {"conversation_id": "c3d0acd8-0147-4cc0-aff3-012a22bd0e62"}) in thoughts.calls
    note = thoughts.sent[-1]
    assert note["title"] == "Study session started" and "claude attach c3d0acd8" in note["message"]
    assert note["origin"]["kind"] == "session_started" and note["origin"]["conversation_id"].startswith("c3d0acd8")
    assert waker.state.event(row["id"])["status"] == "started"


def test_a_session_that_cannot_start_is_told_with_the_reason(home, monkeypatch):
    thoughts = FakeThoughts()
    thoughts.notes_list = [{"note_id": PAGE, "title": "Start a study session", "message": "Prompt"}]
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    waker = make_waker(home, thoughts, fallback_session_id=DESK, start_session_folder=str(home / "study"))
    waker.claude = FakeClaude(waker.config, waker.sessions)
    monkeypatch.setattr(waker_module, "start_background", lambda *a: waker_module.Started(
        False, output="Workspace not trusted. Run `claude` in /x once and accept the trust prompt, then retry."))
    row = start_row()
    waker.state.add_event({"id": row["id"], "kind": "page", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    assert "is not trusted by Claude Code yet: run claude there once" in thoughts.sent[-1]["message"]
    assert waker.state.event(row["id"])["status"] == "unanswered"


def test_start_background_reads_the_new_session_id(home, monkeypatch):
    config = make_config(home)
    claude = FakeClaude(config, None)
    outputs = [
        waker_module.subprocess.CompletedProcess([], 0, "Starting background service…\nbackgrounded · c3d0acd8 · Study session\n", ""),
        waker_module.subprocess.CompletedProcess([], 0, json.dumps([{"id": "c3d0acd8", "sessionId": "c3d0acd8-0147-4cc0-aff3-012a22bd0e62"}]), ""),
    ]
    argvs = []
    monkeypatch.setattr(waker_module.subprocess, "run", lambda argv, **kw: argvs.append(argv) or outputs.pop(0))
    started = waker_module.start_background(claude, "Study session · 26 Nov 15:52", home, "the prompt")
    assert started.ok and started.short_id == "c3d0acd8" and started.session_id.startswith("c3d0acd8-0147")
    assert argvs[0][:4] == ["claude", "--bg", "-n", "Study session · 26 Nov 15:52"] and argvs[0][-1] == "the prompt"
    assert "--permission-mode auto" in " ".join(argvs[0])


def test_a_session_started_elsewhere_first_is_stopped_quietly(home, monkeypatch):
    thoughts = FakeThoughts()
    thoughts.notes_list = [{"note_id": PAGE, "title": "Start a study session", "message": "Prompt"}]
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    thoughts.claims[PAGE] = [(409, {"error_code": "agent_note_claimed"})]
    waker = make_waker(home, thoughts, fallback_session_id=DESK, start_session_folder=str(home / "study"))
    waker.claude = FakeClaude(waker.config, waker.sessions)
    monkeypatch.setattr(waker_module, "start_background", lambda *a: waker_module.Started(True, "c3d0acd8", "c3d0acd8-0147-4cc0-aff3-012a22bd0e62", ""))
    stopped = []
    monkeypatch.setattr(waker_module, "stop_background", lambda claude, short_id: stopped.append(short_id))
    row = start_row()
    waker.state.add_event({"id": row["id"], "kind": "page", "payload": json.loads(row["payload"])})
    waker.handle(waker.state.ready(set()), set())
    assert stopped == ["c3d0acd8"] and thoughts.sent == []
    assert waker.state.event(row["id"])["status"] == "skipped"


def test_codex_and_server_sessions_are_left_to_others(home):
    thoughts = FakeThoughts()
    thoughts.handlings[PAGE] = {"claim": None, "wake_target": target(DESK, machine="server", role="responder"), "covered_by": []}
    starter = make_waker(home, thoughts, fallback_session_id=DESK)
    for origin in ({"agent": "codex"}, {"where": "server"}):
        event = page_event({"note_id": PAGE, "title": "Start a study session", "created_at": None,
                            "origin": {"kind": "phone_page", "start_session": True, "copy_for_agent": True, **origin}})
        row = {**event, "attempts": 0, "event_at": None, "payload": json.dumps(event["payload"])}
        assert starter.decide(row).action == "skip"


# --- Windows (the Thoughts app runs this copy there); simulated on any system with fakes -----------------------


class FakeMsvcrt:
    """msvcrt.locking: a lock of one byte at the position the descriptor is at, refused while another holds it."""

    LK_NBLCK, LK_UNLCK = 2, 0

    def __init__(self):
        self.held: dict[tuple, int] = {}
        self.calls = []

    def locking(self, descriptor, mode, count):
        where = (os.fstat(descriptor).st_ino, os.lseek(descriptor, 0, os.SEEK_CUR))
        self.calls.append((mode, where[1], count))
        if mode == self.LK_NBLCK:
            if self.held.get(where, descriptor) != descriptor:
                raise OSError(36, "Resource deadlock avoided")
            self.held[where] = descriptor
        else:
            self.held.pop(where, None)


@pytest.fixture
def windows(monkeypatch):
    """This waker as on Windows: no fcntl, msvcrt's locks, Windows paths, and processes as windows_process says."""
    fake = FakeMsvcrt()
    monkeypatch.setattr(waker_module, "WINDOWS", True)
    monkeypatch.setattr(waker_module, "fcntl", None)
    monkeypatch.setattr(waker_module, "msvcrt", fake)
    return fake


def test_on_windows_a_process_is_looked_up_never_signalled(windows, monkeypatch):
    def refuse(*_args):
        raise AssertionError("os.kill(pid, 0) ends the process on Windows")

    monkeypatch.setattr(waker_module.os, "kill", refuse)
    answers = {11: (True, "claude.exe"), 12: (True, "node.exe"), 13: (True, "notepad.exe"), 14: (False, None), 15: (True, None)}
    monkeypatch.setattr(waker_module, "windows_process", lambda pid: answers[pid])
    assert waker_module.pid_alive(11, "Fri Nov 27 17:31:27 2026")  # its start time is not compared there
    assert waker_module.pid_alive(12)
    assert not waker_module.pid_alive(13)  # the id now belongs to another program
    assert not waker_module.pid_alive(14)
    assert waker_module.pid_alive(15)  # running, program unknown (another user's): counted as running
    assert not waker_module.pid_alive(0) and not waker_module.pid_alive("12")


def test_on_windows_live_sessions_are_found_with_windows_paths(home, windows, monkeypatch):
    monkeypatch.setattr(waker_module, "windows_process", lambda pid: (True, "claude.exe"))
    live_session(home, STUDY, cwd="C:\\Users\\alex\\Thoughts Sessions")
    live_session(home, OWNER, cwd="C:\\Users\\alex\\AppData\\Local\\Thoughts\\reply-waker\\messenger")
    found = scan_live(home / "claude" / "sessions", exclude_cwds=("c:/users/ALEX/appdata/local/thoughts/reply-waker/messenger",))
    assert set(found) == {STUDY}


def test_on_windows_trust_is_read_whatever_the_slashes_and_case(home, windows):
    (home / "claude.json").write_text(json.dumps({"projects": {
        "C:/Users/alex/Thoughts Sessions": {"hasTrustDialogAccepted": True},
        "C:/Users/alex/Downloads": {"hasTrustDialogAccepted": False},
    }}))
    sessions = waker_module.Sessions(make_config(home))
    assert sessions.trusted("C:\\Users\\alex\\Thoughts Sessions")
    assert sessions.trusted("c:\\users\\alex\\thoughts sessions\\notes")  # a folder below
    assert not sessions.trusted("C:\\Users\\alex\\Downloads")
    assert not sessions.trusted("C:\\Users\\alex")
    assert waker_module.inside("C:\\A\\b", "c:/a") and not waker_module.inside("C:\\Ab", "C:\\A")


def test_on_windows_locks_are_msvcrt_locks_past_the_text(tmp_path, windows):
    path = tmp_path / "waker.lock"
    first, second = path.open("a"), path.open("a")
    try:
        assert waker_module.lock_file(first, blocking=False)
        assert not waker_module.lock_file(second, blocking=False)  # held by the first
        assert windows.calls[0] == (FakeMsvcrt.LK_NBLCK, waker_module.LOCK_OFFSET, 1)
        waker_module.unlock_file(first)
        assert waker_module.lock_file(second, blocking=False)
    finally:
        first.close()
        second.close()
    with session_lock(tmp_path / "locks", STUDY):
        assert (tmp_path / "locks" / f"{STUDY}.lock").is_file()


def test_a_second_waker_in_the_same_home_leaves(home):
    config = make_config(home)
    config.home.mkdir(parents=True, exist_ok=True)
    holder = (config.home / "waker.lock").open("a")
    try:
        assert waker_module.lock_file(holder, blocking=False)
        thoughts = FakeThoughts()
        with pytest.raises(SystemExit) as stopped:
            Waker(config, thoughts=thoughts, state=State(":memory:")).run()
        assert stopped.value.code == waker_module.ALREADY_RUNNING and thoughts.calls == []
    finally:
        holder.close()


def test_on_windows_claude_gets_the_path_without_home(home, windows, monkeypatch):
    monkeypatch.setattr(os, "pathsep", ";")
    monkeypatch.setenv("PATH", "C:\\Windows\\System32;C:\\Tools")
    monkeypatch.delenv("HOME", raising=False)
    monkeypatch.setenv("ClaudeCode_Entrypoint", "x")  # Windows names have any case
    monkeypatch.setenv("CLAUDECODE", "1")
    env = waker_module.claude_env(make_config(home, path=["C:\\Users\\alex\\.local\\bin"]))
    assert env["PATH"].split(";") == ["C:\\Users\\alex\\.local\\bin", "C:\\Windows\\System32", "C:\\Tools"]
    assert "HOME" not in env and "CLAUDECODE" not in env and "ClaudeCode_Entrypoint" not in env
    assert env["DISABLE_AUTOUPDATER"] == "1"


def test_on_windows_claude_is_the_native_installers(home, windows, monkeypatch):
    monkeypatch.setattr(waker_module.Path, "home", classmethod(lambda cls: home))
    native = home / ".local" / "bin" / "claude.exe"
    native.parent.mkdir(parents=True)
    native.write_text("")
    assert waker_module.find_claude(make_config(home, claude="auto")) == str(native)
    native.unlink()
    monkeypatch.setattr(waker_module.shutil, "which", lambda name: "C:\\npm\\claude.cmd")
    assert waker_module.find_claude(make_config(home, claude="auto")) == "C:\\npm\\claude.cmd"
    assert waker_module.find_claude(make_config(home, claude=str(home / "c.exe"))) == str(home / "c.exe")


def test_on_windows_children_have_no_window_and_utf8(home, windows, monkeypatch):
    seen = []

    class Child:
        pid, returncode = 4242, 0

        def communicate(self, message=None, timeout=None):
            return json.dumps({"result": "SENT \u2713"}), ""

    def popen(argv, **options):
        seen.append(options)
        return Child()

    monkeypatch.setattr(waker_module.subprocess, "Popen", popen)
    config = make_config(home)
    claude = Claude.__new__(Claude)
    claude.config, claude.sessions, claude.stop, claude.binary = config, waker_module.Sessions(config), threading.Event(), "claude.exe"
    run = claude.run(["-p"], "“Eigenvalues” — reply", home, 5)
    assert run.ok and run.answer["result"] == "SENT \u2713"
    options = seen[0]
    assert options["encoding"] == "utf-8" and "start_new_session" not in options
    assert options["creationflags"] == waker_module.CREATE_NO_WINDOW | waker_module.CREATE_NEW_PROCESS_GROUP


def test_on_windows_a_stuck_child_is_stopped_with_its_tree(windows, monkeypatch):
    ran = []
    monkeypatch.setattr(waker_module.subprocess, "run", lambda argv, **options: ran.append((argv, options)))

    class Child:
        pid = 777

        def wait(self, timeout=None):
            return 0

    waker_module.terminate(Child())
    assert ran[0][0] == ["taskkill", "/PID", "777", "/T", "/F"]
    assert ran[0][1]["creationflags"] == waker_module.CREATE_NO_WINDOW


def test_the_end_of_standard_input_stops_the_waker():
    import io

    stop = threading.Event()
    waker_module.watch_stdin(stop, io.BytesIO(b"anything\n"))
    assert stop.is_set()


def test_the_zone_is_this_computers_without_local_zone(home, monkeypatch):
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    config = make_config(home, local_zone=None, claude_dir=None, claude_json=None)
    assert config.zone().utcoffset(None) == dt_now().astimezone().utcoffset()
    assert make_config(home, local_zone="Not/AZone").zone() is not None
    assert config.claude_dir == Path.home() / ".claude" and config.claude_json == Path.home() / ".claude.json"
    monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(home / "cc"))
    assert config.claude_dir == home / "cc" and config.claude_json == home / "cc" / ".claude.json"


def dt_now():
    import datetime

    return datetime.datetime.now()


def test_the_sign_in_comes_from_the_thoughts_tools(home, monkeypatch):
    module = home / "agent_notes.py"
    module.write_text(
        "from dataclasses import dataclass\n"
        "@dataclass\n"
        "class Settings:\n"
        "    server_url: str\n"
        "    token: str\n"
        "def load_config():\n"
        "    return Settings('https://example-server.test', 'device-token')\n"
    )
    monkeypatch.delenv("THOUGHTS_SERVER_URL", raising=False)
    thoughts = Thoughts.from_config(make_config(home, token_file=None, thoughts_url=None, agent_notes=str(module)))
    assert thoughts.url == "https://example-server.test" and thoughts._token() == "device-token"
    module.write_text(
        "class AgentNoteError(Exception):\n"
        "    def __init__(self, code, detail):\n"
        "        super().__init__(detail)\n"
        "        self.detail = detail\n"
        "def load_config():\n"
        "    raise AgentNoteError('no_credentials', 'Not signed in to Thoughts on this computer.')\n"
    )
    with pytest.raises(SystemExit) as stopped:
        Thoughts.from_config(make_config(home, token_file=None, thoughts_url=None, agent_notes=str(module)))
    assert "Not signed in" in str(stopped.value.code)


def test_a_new_session_for_one_computer_is_left_to_that_computers_app(home):
    # Send to Agent's "New session on this computer" (origin.where "one_computer"): the Thoughts app of the computer
    # it names starts it, never a waker, not even one that starts the phone's sessions.
    note = {"note_id": PAGE, "title": "Copy for agent: Notes · page 2", "created_at": "2026-11-27T10:00:00Z",
            "origin": {"kind": "phone_page", "start_session": True, "copy_for_agent": True, "agent": "claude",
                       "where": "one_computer", "computer": "0b5c9a1e-computer"}}
    event = page_event(note)
    assert event["payload"]["start_session"] and event["payload"]["where"] == "one_computer"
    for starts in (False, True):
        thoughts = FakeThoughts()
        waker = make_waker(home, thoughts, start_sessions=starts)
        decision = waker.decide(event)
        assert decision.action == "skip" and not any(call[0] == "claim" for call in thoughts.calls)
