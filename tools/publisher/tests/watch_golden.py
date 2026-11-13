#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Golden test of the native thoughts-watch-build, following a scripted fake
Fedora server. Exit codes, the stamped output (timestamps normalized) and the
requests the server received are compared against
tools/publisher/tests/golden/watch/<scenario>.json.

  python3 watch_golden.py --native <build>/bin/thoughts-watch-build [--only REGEX] [--record]

--record rewrites the expected files from the native watcher; review the diff
before committing it.
"""
from __future__ import annotations

import argparse
import json
import os
import pty
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Script:
    """Responses for one run: per build, a list of records (one per GET,
    the last repeats) and output chunks (likewise)."""

    def __init__(self, spec):
        self.spec = spec
        self.counters = {}
        self.requests = []

    def next(self, key, items):
        index = self.counters.get(key, 0)
        self.counters[key] = index + 1
        return items[min(index, len(items) - 1)]


def serve(script: Script):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def respond(self, status, value):
            body = value if isinstance(value, bytes) else json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def handle_any(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
            script.requests.append((self.command, self.path, body.decode()))
            path = self.path
            spec = script.spec
            if path.startswith("/v1/render-status"):
                status, value = script.next("render", spec.get("render", [[200, {}]]))
                return self.respond(status, value)
            if self.command == "POST" and path == "/v1/builds":
                # The watcher only reads; a build request here is a failure the recorded requests show.
                return self.respond(422, {"detail": "unexpected build request"})
            match = re.fullmatch(r"/v1/builds/([^/?]+)(/output\?offset=(\d+))?", path)
            if not match:
                return self.respond(404, {"detail": "Not Found"})
            build = spec["builds"].get(match.group(1))
            if build is None:
                return self.respond(404, {"detail": "Build not found"})
            if match.group(2):
                status, value = script.next(match.group(1) + ":out", build["outputs"])
            else:
                status, value = script.next(match.group(1), build["records"])
            return self.respond(status, value)

        do_GET = do_POST = do_HEAD = handle_any

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def record(status, *, detail="", command=None, result=None, error=None, target="m.pdf", repair_mode="none"):
    value = {"id": "b1", "status": status, "progress": {"detail": detail} if detail is not None else "x",
             "target": target, "repair_mode": repair_mode}
    if command is not None:
        value["command"] = command
    if result is not None:
        value["result"] = result
    if error is not None:
        value["error"] = error
    return [200, value]


def chunk(text, next_offset, complete=False):
    return [200, {"offset": 0, "next_offset": next_offset, "text": text, "complete": complete}]


FAILED_RESULT = {"log_sha256": "ab" * 32,
                 "timings_ms": {"renderer": 59950, "total": 61000, "cleanup": True, "publication": "x"},
                 "failed_documents": [
                     {"pdf_filename": "m.pdf", "source": "docs-m/tex/main.tex", "message": "docs-m/tex/main.tex:3: Undefined\nsecond line"},
                     {"filename": "n.pdf", "error": " boom "},
                     {"pdf_filename": "m.pdf", "source": "docs-m/tex/main.tex", "message": "docs-m/tex/main.tex:3: Undefined\nsecond line"},
                     "not an object", {"pdf_filename": 5, "source": "s.tex", "message": "other"}]}

SCENARIOS = {
    "succeeded": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("queued", detail=None), record("running", detail="Compiling", command=["/srv/py", "-m", "x", "--a b", "it's", "é", ""]),
                                      record("running", detail="Compiling"), record("succeeded", result={"timings_ms": {"renderer": 297718, "total": 302283}})],
                          "outputs": [chunk("", 0), chunk("line one\nprog 50%\rprog 100%\npart", 30), [502, b""],
                                      chunk("ial\r\n\nfinal \u2028 sep\x0cff\n", 60), chunk("tail without newline", 80, True)]}},
    },
    "attention-no-tty": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("attention", detail="Compiling", result=FAILED_RESULT)], "outputs": [chunk("x\n", 2, True)]}},
    },
    "failed-error-only": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("failed", command="nope", error="  renderer crashed  ")], "outputs": [chunk("", 0, True)]}},
    },
    "failed-status-only": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [[200, {"status": "cancelled", "progress": [], "result": []}]], "outputs": [chunk("", 0, True)]}},
    },
    "not-found": {"args": ["--build-id", "missing", "--target", "m.pdf"], "builds": {}},
    "output-unavailable": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("running")], "outputs": [chunk("partial line", 12), [403, {"detail": "nope"}]]}},
    },
    "bad-offset": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("running")], "outputs": [[200, {"next_offset": "x"}]]}},
    },
    # On a terminal a failed build ends like anywhere else: no question, no further request.
    "failed-terminal": {
        "args": ["--build-id", "b1", "--target", "m.pdf"], "tty": "y\n",
        "builds": {"b1": {"records": [record("failed", result=FAILED_RESULT)], "outputs": [chunk("", 0, True)]}},
    },
    "attention-terminal": {
        "args": ["--build-id", "b1", "--target", "m.pdf"], "tty": "Yes\n",
        "builds": {"b1": {"records": [record("attention", result=FAILED_RESULT, target="all")], "outputs": [chunk("", 0, True)]}},
    },
    # Old build records carry other repair_mode values; they read like any other record.
    "old-record-repair-mode": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("failed", result=FAILED_RESULT, repair_mode="codex")], "outputs": [chunk("", 0, True)]}},
    },
    "old-record-repair-mode-succeeded": {
        "args": ["--build-id", "b1", "--target", "m.pdf"],
        "builds": {"b1": {"records": [record("succeeded", repair_mode="review_cards")], "outputs": [chunk("ok\n", 3, True)]}},
    },
    "run-directory": {
        "args": ["--run-directory", "run"],
        "files": {"run/request.json": json.dumps({"schema_version": 1.0, "target": "m.pdf", "submission_command": ["/Users/x y/pub", "--target", "m.pdf", "--root", "C:\\a", "it's"]}),
                  "run/submission-status.json": json.dumps({"schema_version": 1, "state": "publishing", "message": "Uploading 3 objects"}),
                  "run/build-id": " b1 \n"},
        "builds": {"b1": {"records": [record("succeeded")], "outputs": [chunk("done\n", 5, True)]}},
    },
    "run-directory-failed": {
        "args": ["--run-directory", "run"],
        "files": {"run/request.json": json.dumps({"schema_version": 1, "target": "m.pdf", "submission_command": []}),
                  "run/submission-status.json": json.dumps({"schema_version": 1, "state": "failed", "message": "network unavailable"})},
        "builds": {},
    },
    "run-directory-missing": {"args": ["--run-directory", "nowhere"], "builds": {}},
    "run-directory-bad-schema": {"args": ["--run-directory", "run"], "files": {"run/request.json": '{"schema_version": 2}'}, "builds": {}},
    "run-directory-bad-command": {"args": ["--run-directory", "run"], "files": {"run/request.json": '{"schema_version": 1, "target": "m.pdf", "submission_command": [1]}'}, "builds": {}},
    "run-directory-bom": {"args": ["--run-directory", "run"], "files": {"run/request.json": '\ufeff{}'}, "builds": {}},
    "cli-no-token": {"args": ["--build-id", "b1", "--target", "m.pdf"], "env": {"THOUGHTS_TOKEN": ""}, "builds": {}},
    "cli-bad-target": {"args": ["--build-id", "b1", "--target", "m.tex"], "builds": {}},
    "cli-no-source": {"args": [], "builds": {}},
    "cli-both": {"args": ["--build-id", "b1", "--run-directory", "r"], "builds": {}},
    "cli-poll": {"args": ["--build-id", "b1", "--target", "m.pdf", "--poll-interval", "0"], "builds": {}},
    "cli-poll-text": {"args": ["--build-id", "b1", "--target", "m.pdf", "--poll-interval", "abc"], "builds": {}},
    "cli-help": {"args": ["-h"], "builds": {}},
    "cli-abbrev": {"args": ["--build", "b1", "--tar", "m.pdf", "--t", "x"], "builds": {}},
}

STAMP = re.compile(r"\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} [^\]]*\]")


def run(command, scenario, directory, args):
    script = Script(scenario)
    server = serve(script)
    url = f"http://127.0.0.1:{server.server_address[1]}"
    environment = dict(os.environ, THOUGHTS_TOKEN="secret")
    environment.pop("THOUGHTS_TRACE_ID", None)
    environment.pop("COLUMNS", None)
    environment.update(scenario.get("env", {}))
    if scenario["args"] == ["-h"]:
        arguments = [*command, "-h"]
    elif "--poll-interval" in scenario["args"]:
        arguments = [*command, "--server", url, *scenario["args"]]
    else:
        arguments = [*command, "--server", url, *scenario["args"], "--poll-interval", "0.01"]
    try:
        if "tty" in scenario:
            master, slave = pty.openpty()
            process = subprocess.Popen(arguments, cwd=directory, env=environment, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            os.close(slave)
            os.write(master, scenario["tty"].encode())
            try:
                stdout, stderr = process.communicate(timeout=60)
                code = process.returncode
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                code = "timeout"
            os.close(master)
        else:
            # Windows' isatty() is true for NUL (Python's too), and Wine maps /dev/null to NUL, so use an empty pipe.
            stdin = {"input": b""} if args.windows else {"stdin": subprocess.DEVNULL}
            result = subprocess.run(arguments, cwd=directory, env=environment, capture_output=True, timeout=60, **stdin)
            stdout, stderr, code = result.stdout, result.stderr, result.returncode
    finally:
        server.shutdown()
    out = STAMP.sub("[T]", stdout.decode("utf-8", "replace"))
    err = STAMP.sub("[T]", stderr.decode("utf-8", "replace"))
    if args.windows:
        out = out.replace("\r\n", "\n")
        err = "".join(line for line in err.replace("\r\n", "\n").splitlines(keepends=True) if "MoltenVK" not in line)
    out, err = out.replace(url, "URL"), err.replace(url, "URL")
    requests = [[method, path, body] for method, path, body in script.requests]
    for root in (str(directory.resolve()), str(directory)):
        out, err = out.replace(root, "<DIR>"), err.replace(root, "<DIR>")
    return {"exit": code, "stdout": out, "stderr": err, "requests": requests}


GOLDEN = Path(__file__).resolve().parent / "golden" / "watch"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--only")
    parser.add_argument("--record", action="store_true", help="rewrite the expected outcomes from the native watcher")
    parser.add_argument("--windows", action="store_true", help="native is a Windows build under Wine: normalize CRLF and Wine noise")
    args = parser.parse_args()
    native = [os.path.abspath(args.native)]
    work = Path(tempfile.mkdtemp(prefix="watch-golden-"))
    selected = [name for name in SCENARIOS if not args.only or re.fullmatch(args.only, name)]
    failures = 0
    for name in selected:
        scenario = SCENARIOS[name]
        directory = work / name
        directory.mkdir(parents=True)
        for relative, text in scenario.get("files", {}).items():
            (directory / relative).parent.mkdir(parents=True, exist_ok=True)
            (directory / relative).write_text(text, encoding="utf-8")
        got = run(native, scenario, directory, args)
        expected_path = GOLDEN / f"{name}.json"
        problems = []
        if args.record:
            GOLDEN.mkdir(parents=True, exist_ok=True)
            expected_path.write_text(json.dumps(got, indent=1, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
        elif not expected_path.is_file():
            problems.append(f"no expected outcome {expected_path}; run with --record")
        else:
            want = json.loads(expected_path.read_text(encoding="utf-8"))
            for label in ("exit", "stdout", "stderr", "requests"):
                if want[label] != got[label]:
                    problems.append(f"{label}\n    expected: {want[label]!r}\n    native:   {got[label]!r}")
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for problem in problems:
                print("  " + problem[:2500])
        else:
            print(f"ok   {name} (exit {got['exit']})")
    if args.record and not args.only:
        for path in GOLDEN.glob("*.json"):
            if path.stem not in SCENARIOS:
                path.unlink()
    shutil.rmtree(work, ignore_errors=True)
    print(f"{len(selected) - failures} scenarios {'recorded' if args.record else 'match'}, {failures} differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
