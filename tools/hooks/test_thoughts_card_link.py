# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""The review-card prompt hook (thoughts_card_link.py) with a thoughtsctl stub, never the real app."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

HOOK = Path(__file__).with_name("thoughts_card_link.py")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "mcp"))
import conversation_identity  # noqa: E402

CARD = "0fc5768d-a56c-53d0-9f37-7af64c3cf8a9"
OTHER_CARD = "c1e62ecc-2dfb-4259-97cc-e319b1186848"
CLAUDE = "9037f64f-185a-564e-a957-c7f4089267a6"
CODEX = "01900000-0000-7000-8000-000000000002"

BLOCK = f'''Thoughts document: "GRPO notes"
Project: Concepts
Source: /Users/alex/Thoughts/docs/docs-concepts/grpo.tex  (LaTeX)
To change grpo.tex, create proposals with the Thoughts create_proposals tool (several edits at once) or `thoughtsctl create-proposals`; don't edit the source directly unless asked, and never edit the PDF.
Built PDF: /Users/alex/Thoughts/docs/current_pdfs/grpo.pdf  (12 pages)
Project file: /Users/alex/Thoughts/docs/docs-concepts/thoughts-project.json
Document ID: 4f1c
Review card: {CARD}
Question: "Why does GRPO normalise the advantage?"
Answer pages: 6
Answer source: grpo.tex lines 40-52
Answer feedback:
- Liked: "the group mean" — clear
Current review (being given now, not saved yet):
- Recall: Partly
- Again in: 3 days
- Notes:
  - mixed up the std
'''


class PromptTitleTest(unittest.TestCase):
    def test_plain_prompt_is_its_first_line(self):
        self.assertEqual(conversation_identity.prompt_title("\n  Make   the search\nwork on the phone"), "Make the search")
        self.assertEqual(conversation_identity.prompt_title(""), "")

    def test_card_block_takes_the_users_own_words(self):
        self.assertEqual(conversation_identity.prompt_title(BLOCK + "\nExplain the std part again, slowly"), "Explain the std part again, slowly")
        self.assertEqual(conversation_identity.prompt_title("Quiz me on this one\n\n" + BLOCK), "Quiz me on this one")

    def test_card_block_alone_is_named_after_the_question(self):
        self.assertEqual(conversation_identity.prompt_title(BLOCK), "Card: Why does GRPO normalise the advantage?")
        physical = BLOCK + "\nPhysical studying: I answer on paper and photograph the page.\n- Watch that folder\n- Read every new photo\n"
        self.assertEqual(conversation_identity.prompt_title(physical), "Card: Why does GRPO normalise the advantage?")

    def test_card_ids(self):
        text = f"Review card: {CARD}\nReview card:{OTHER_CARD.upper()}\nReview card: {CARD}\nReview card: not-a-card"
        self.assertEqual(conversation_identity.review_card_ids(text), [CARD, OTHER_CARD])


class HookTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.calls = root / "calls.jsonl"
        self.log = root / "card-link.log"
        stub = root / "thoughtsctl"
        stub.write_text("#!" + sys.executable + "\n" + """import json, os, sys
from pathlib import Path
payload = json.load(sys.stdin)
with Path(os.environ['CALL_LOG']).open('a') as out:
    out.write(json.dumps({'args': sys.argv[1:], 'payload': payload}) + '\\n')
print(json.dumps({'schema_version': 1, 'accepted': True, 'project_name': 'Concepts', 'written': True, 'card_written': True,
                  'review_card_id': payload.get('review_card_id')}))
""")
        stub.chmod(0o755)
        self.home = root / "home"
        (self.home / ".claude" / "sessions").mkdir(parents=True)
        self.env = {**os.environ, "THOUGHTSCTL": str(stub), "CALL_LOG": str(self.calls), "THOUGHTS_CARD_LINK_LOG": str(self.log),
                    "HOME": str(self.home), "CLAUDE_CONFIG_DIR": str(self.home / ".claude"), "CODEX_HOME": str(self.home / ".codex"),
                    "THOUGHTS_CARD_LINK_SESSION_WAIT": "0.5"}
        self.env.pop("THOUGHTS_CARD_LINK_AGENT", None)

    def tearDown(self):
        self.directory.cleanup()

    def run_hook(self, event):
        started = time.monotonic()
        done = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(event).encode(), capture_output=True, env=self.env, timeout=30)
        return done, time.monotonic() - started

    def wait_calls(self, count, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.calls.exists():
                lines = self.calls.read_text().splitlines()
                if len(lines) >= count:
                    time.sleep(0.2)
                    return [json.loads(line) for line in self.calls.read_text().splitlines()]
            time.sleep(0.05)
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_prompt_without_a_card_does_nothing(self):
        done, seconds = self.run_hook({"session_id": CLAUDE, "transcript_path": "/x.jsonl", "cwd": "/", "hook_event_name": "UserPromptSubmit",
                                       "prompt": "Explain GRPO"})
        self.assertEqual((done.returncode, done.stdout), (0, b""))
        time.sleep(0.5)
        self.assertFalse(self.calls.exists())
        self.assertLess(seconds, 5)

    def test_claude_prompt_with_a_card_links_it_in_the_background(self):
        (self.home / ".claude" / "sessions" / "26661.json").write_text(json.dumps(
            {"pid": 26661, "sessionId": CLAUDE, "name": "alex-61", "nameSource": "derived", "bridgeSessionId": "session_01BcDeFgHjKmNpQrStUvWxYz"}))
        done, _ = self.run_hook({"session_id": CLAUDE, "transcript_path": "/x.jsonl", "cwd": "/Users/alex", "hook_event_name": "UserPromptSubmit",
                                 "prompt": BLOCK + "\nWhy the std?"})
        self.assertEqual((done.returncode, done.stdout, done.stderr), (0, b"", b""))
        calls = self.wait_calls(1)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["args"], ["--json", "link-conversation"])
        self.assertEqual(calls[0]["payload"], {
            "review_card_id": CARD, "agent": "claude", "conversation_id": CLAUDE,
            "conversation_url": "https://claude.ai/code/session_01BcDeFgHjKmNpQrStUvWxYz", "session_name": "alex-61",
            "title": "Why the std?", "title_source": "prompt", "linked_by": "tool", "card_linked_by": "prompt", "tool": "prompt"})
        self.assertIn("linked to 'Concepts'", self.log.read_text())

    def test_claude_session_without_a_link_and_two_cards(self):
        done, _ = self.run_hook({"session_id": CLAUDE, "transcript_path": None, "cwd": "/", "hook_event_name": "UserPromptSubmit",
                                 "prompt": f"Compare\nReview card: {CARD}\nReview card: {OTHER_CARD}"})
        self.assertEqual(done.stdout, b"")
        calls = self.wait_calls(2)
        self.assertEqual([call["payload"]["review_card_id"] for call in calls], [CARD, OTHER_CARD])
        self.assertEqual({call["payload"]["conversation_url"] for call in calls}, {""})
        self.assertEqual(calls[0]["payload"]["title"], "Compare")

    def test_codex_thread_comes_from_its_rollout(self):
        day = self.home / ".codex" / "sessions" / "2026" / "10" / "26"
        day.mkdir(parents=True)
        rollout = day / f"rollout-2026-10-26T18-00-00-{CODEX}.jsonl"
        rollout.write_text("{}\n")
        (self.home / ".codex" / "session_index.jsonl").write_text(json.dumps({"id": CODEX, "thread_name": "GRPO card"}) + "\n")
        done, _ = self.run_hook({"session_id": CODEX, "turn_id": "t1", "transcript_path": str(rollout), "cwd": "/", "model": "gpt",
                                 "permission_mode": "default", "hook_event_name": "UserPromptSubmit", "prompt": BLOCK})
        self.assertEqual(done.stdout, b"")
        calls = self.wait_calls(1)
        self.assertEqual(len(calls), 1)
        payload = calls[0]["payload"]
        self.assertEqual((payload["agent"], payload["conversation_id"], payload["conversation_url"], payload["title"], payload["title_source"]),
                         ("codex", CODEX, "codex://threads/" + CODEX, "GRPO card", "app"))

    def test_bad_input_is_only_logged(self):
        done = subprocess.run([sys.executable, str(HOOK)], input=b"Review card: {not json", capture_output=True, env=self.env, timeout=30)
        self.assertEqual((done.returncode, done.stdout), (0, b""))
        self.assertIn("hook failed", self.log.read_text())


if __name__ == "__main__":
    unittest.main()
