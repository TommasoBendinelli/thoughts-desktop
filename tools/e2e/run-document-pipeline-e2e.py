#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Hermetic end-to-end test of the Thoughts document pipeline.

One command:

    tools/e2e/run-document-pipeline-e2e.py

It creates a temporary directory, starts a local thoughts-server (uvicorn on
sqlite, 127.0.0.1 only), its build worker and its review worker with fresh
device tokens, then
runs autotests/documentpipelinee2etest.cpp: the desktop's real Sync engine
(FedoraRepository) and the real source publisher CLI, on a private HOME and a
temporary Library. It covers new Markdown and LaTeX documents, PDF imports
from a file and a link, the Project file changed on both sides, edits,
deletions, the included flag, and two Projects at once; every step is
followed by a Sync that must be a quick no-op with nothing pending.

The server, worker, and test are always stopped, and a summary is printed.
Nothing contacts another host, the real Library, or the user's Thoughts
state; the test refuses to run unless every location is inside its HOME.

Options:
    --build-dir DIR    desktop build with the test binary (default: build-e2e)
    --server-src DIR   thoughts_server package to run, for the server and
                       worker (default: ../thoughts-server/src)
    --python PATH      interpreter whose environment has the server's
                       dependencies (default: ../thoughts-server/.venv)
    --build            build okularpart, thoughts-comment-sync, the source
                       publisher and the test first

The desktop runs the source publisher of the build dir
(bin/thoughts-publish-sources), as an installed Thoughts runs its bundled one.
    --keep             keep the temporary directory
    --test-prefix CMD  run the test binary through CMD ({cwd} is replaced by
                       the workspace), e.g. a container on a Linux builder
    --comment-sync P   the thoughts-comment-sync the server runs (default:
                       the build dir's)
    --extra-path DIR   prepended to the server's PATH (repeatable)
    --no-html-editions run the server with THOUGHTS_HTML_EDITIONS=false
    -- ARGS            passed to the test binary (e.g. a single test function)

testDefaultFolderRename asks the harness (THOUGHTS_E2E_CONTROL) to stop the
server and its workers, run thoughts_server.migrate_default_folder --apply
--canonical on the server Library, and start them again;
testSourceFolderRename does the same with
thoughts_server.migrate_source_folder_names.

Requirements: TeX Live in /Library/TeX/texbin, qpdf, Homebrew's pango for
WeasyPrint, and a desktop build dir configured with -DBUILD_TESTING=ON, e.g.

    cmake -S . -B build-e2e -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
        -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=$HOME/CraftRoot ...

The server sources must accept the trace echo in any header case (uvicorn
sends lowercase names), as thoughts-server does from "Accept the
source-publication trace echo in any header case" on. testProposalAfterFailedBuild
also expects LaTeX errors to name the \\input fragment, as thoughts-server does
from "Renderer LaTeX errors name the file TeX was reading" (69dfbd6) on.

A full run takes about a minute. Known product bugs are reported as
"KNOWN BUG" (QEXPECT_FAIL) and do not fail the run; once one is fixed the
run fails with XPASS until its expectation is removed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REAL_HOME = Path.home()


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def remove_tree(path: Path) -> None:
    # Library generations are read-only.
    for directory, subdirectories, files in os.walk(path):
        os.chmod(directory, 0o755)
        for name in files:
            try:
                os.chmod(os.path.join(directory, name), 0o644, follow_symlinks=False)
            except (OSError, NotImplementedError):
                pass
    shutil.rmtree(path, ignore_errors=True)


def wait_for_server(url: str, token: str, process: subprocess.Popen, timeout: float = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("the server exited during startup")
        request = urllib.request.Request(url + "/v2/sync/state", headers={"Authorization": "Bearer " + token})
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=2) as response:
                if response.status == 200:
                    return
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(0.3)
    raise RuntimeError("the server did not start within %ds" % timeout)


def scratch_interpreter(python: Path, server_src: Path, root: Path) -> Path:
    """A throwaway virtual environment that imports thoughts_server from
    server_src and everything else from python's environment. The worker
    starts its renderer with its own interpreter and a cleared environment,
    so PYTHONPATH alone would render with the installed package."""
    subprocess.run([str(python), "-m", "venv", "--without-pip", str(root)], check=True)
    dependencies = subprocess.check_output([str(python), "-c", "import site; print(site.getsitepackages()[0])"], text=True).strip()
    (site_packages,) = root.glob("lib/python*/site-packages")
    # The renderer runs without PYTHONDONTWRITEBYTECODE; keep the checkout clean.
    (site_packages / "thoughts-e2e.pth").write_text(f"{server_src}\n{dependencies}\nimport sys; sys.dont_write_bytecode = True\n")
    interpreter = root / "bin/python"
    located = subprocess.check_output([str(interpreter), "-c", "import thoughts_server; print(thoughts_server.__file__)"], text=True).strip()
    if not Path(located).resolve().is_relative_to(server_src):
        raise RuntimeError(f"the scratch interpreter imports {located}")
    return interpreter


def stop(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def tail(path: Path, lines: int = 40) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])
    except OSError:
        return ""


class ServerControl:
    """Answers the test's requests to act on the server (THOUGHTS_E2E_CONTROL):
    the test writes control/request.json and waits for control/response.json.

    migrate-default-folder / migrate-source-folder-names: stops the server
    and its workers, runs thoughts_server.migrate_default_folder or
    migrate_source_folder_names (--apply --canonical) on the server Library,
    and starts the services again, as the production rollouts do with their
    services stopped."""

    def __init__(self, directory: Path, start_services, stop_services, migration_command, environment, logs: Path):
        self.directory = directory
        self.start_services = start_services
        self.stop_services = stop_services
        self.migration_command = migration_command
        self.environment = environment
        self.logs = logs
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self) -> None:
        request_path = self.directory / "request.json"
        while not self.stopped.wait(0.2):
            if not request_path.is_file():
                continue
            try:
                request = json.loads(request_path.read_text())
            except (OSError, ValueError):
                continue
            request_path.unlink(missing_ok=True)
            response = self.handle(request.get("action", ""))
            partial = self.directory / "response.json.part"
            partial.write_text(json.dumps(response))
            partial.replace(self.directory / "response.json")

    # Harness actions and the server migration module each one runs.
    MIGRATIONS = {"migrate-default-folder": "migrate_default_folder",
                  "migrate-source-folder-names": "migrate_source_folder_names"}

    def handle(self, action: str) -> dict:
        module = self.MIGRATIONS.get(action)
        if module is None:
            return {"exit": 2, "output": f"unknown action {action!r}"}
        command = self.migration_command(module)
        if command is None:
            return {"exit": 127, "missing": True, "output": f"no thoughts_server.{module} in the server sources"}
        print(f"E2E harness: stopping the server to run {module}", flush=True)
        self.stop_services()
        try:
            with open(self.logs / f"{action}.log", "wb") as log:
                result = subprocess.run(command, env=self.environment, stdout=log, stderr=subprocess.STDOUT, timeout=600)
            output = (self.logs / f"{action}.log").read_text(errors="replace")
            exit_code = result.returncode
        except (OSError, subprocess.SubprocessError) as error:
            output, exit_code = str(error), 1
        finally:
            self.start_services()
        print(f"E2E harness: {module} exited {exit_code}", flush=True)
        return {"exit": exit_code, "output": output[-20000:]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, default=REPO / "build-e2e")
    parser.add_argument("--server-src", type=Path, default=REPO.parent / "thoughts-server/src")
    parser.add_argument("--python", type=Path, default=REPO.parent / "thoughts-server/.venv/bin/python")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--test-prefix", default="")
    parser.add_argument("--comment-sync", type=Path)
    parser.add_argument("--extra-path", action="append", default=[])
    parser.add_argument("--no-html-editions", action="store_true",
                        help="run the server with THOUGHTS_HTML_EDITIONS=false (no HTML editions of PDFs)")
    parser.add_argument("test_args", nargs="*")
    options = parser.parse_args()

    build_dir = options.build_dir.resolve()
    server_src = options.server_src.resolve()
    test_binary = build_dir / "bin/documentpipelinee2etest"
    if not (server_src / "thoughts_server/__init__.py").is_file():
        print(f"error: no thoughts_server package in {server_src}", file=sys.stderr)
        return 2
    if not options.python.is_file():
        print(f"error: {options.python} is missing", file=sys.stderr)
        return 2
    if options.build:
        ninja = shutil.which("ninja") or str(Path.home() / "CraftRoot/dev-utils/bin/ninja")
        subprocess.run([ninja, "-C", str(build_dir), "okularpart", "thoughts-comment-sync", "thoughts-publish-sources",
                        "documentpipelinee2etest"], check=True)
    # THOUGHTS_E2E_SOURCE_PUBLISHER runs another publisher build, e.g. an older
    # one to show that a test catches the bug it guards against.
    publisher = Path(os.environ.get("THOUGHTS_E2E_SOURCE_PUBLISHER") or build_dir / "bin/thoughts-publish-sources")
    for required in (test_binary, publisher):
        if not required.is_file():
            print(f"error: {required} is missing; build it or pass --build", file=sys.stderr)
            return 2

    workspace = Path(tempfile.mkdtemp(prefix="thoughts-e2e-"))
    # macOS reports temporary paths as /var/... and /private/var/...
    workspace = workspace.resolve()
    home = workspace / "desktop-home"
    server_home = workspace / "server-home"
    library = workspace / "server-library"
    logs = workspace / "logs"
    control = workspace / "control"
    for directory in (home, server_home, library, logs, control, workspace / "state/render-home", workspace / "objects"):
        directory.mkdir(parents=True)
    # The renderer runs with HOME=state/render-home; dyld falls back to
    # $HOME/lib, where WeasyPrint finds Homebrew's pango.
    (workspace / "state/render-home/lib").symlink_to("/opt/homebrew/lib")

    port = free_port()
    url = f"http://127.0.0.1:{port}"
    desktop_token, source_token, phone_token = secrets.token_hex(24), secrets.token_hex(24), secrets.token_hex(24)
    comment_sync = options.comment_sync or build_dir / "bin/thoughts-comment-sync"
    path = ":".join([*options.extra_path, "/Library/TeX/texbin", str(REAL_HOME / ".local/bin"), "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"])
    base = {
        "PATH": path,
        "LANG": "en_US.UTF-8",
        "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    server_environment = {
        **base,
        "HOME": str(server_home),
        "CFFIXED_USER_HOME": str(server_home),
        "THOUGHTS_DATABASE_URL": f"sqlite:///{workspace / 'state/thoughts.db'}",
        "THOUGHTS_STATE_ROOT": str(workspace / "state"),
        "THOUGHTS_OBJECT_ROOT": str(workspace / "objects"),
        # Builds require the Library and the worktree to be one directory.
        "THOUGHTS_DOCS_ROOT": str(library),
        "THOUGHTS_WORKTREE_ROOT": str(library),
        "THOUGHTS_API_HOST": "127.0.0.1",
        "THOUGHTS_API_PORT": str(port),
        "THOUGHTS_WORKER_POLL_SECONDS": "0.2",
        "THOUGHTS_BUILD_TIMEOUT_SECONDS": "600",
        "THOUGHTS_BUILD_PARALLEL_JOBS": "2",
        "THOUGHTS_BOOTSTRAP_TOKENS": json.dumps({"thoughts-mac-e2e": desktop_token, "thoughts-mac-source-e2e": source_token,
                                                 # A second device: creates review cards and reads the catalogue as the phone does.
                                                 "thoughts-phone-e2e": phone_token}),
        "THOUGHTS_PROVIDER_SYNC_ENABLED": "false",
        "THOUGHTS_TOOLCHAIN_KEY": "document-pipeline-e2e",
    }
    if comment_sync.is_file():
        server_environment["THOUGHTS_COMMENT_SYNC_BIN"] = str(comment_sync)
    test_environment = {
        **base,
        "HOME": str(home),
        "CFFIXED_USER_HOME": str(home),
        "QT_QPA_PLATFORM": "offscreen",
        "THOUGHTS_E2E_SERVER": url,
        "THOUGHTS_E2E_DOCS": str(home / "docs"),
        "THOUGHTS_E2E_JOURNAL": str(logs / "desktop-journal.jsonl"),
        "THOUGHTS_SERVER_URL": url,
        "THOUGHTS_TOKEN": desktop_token,
        "THOUGHTS_SOURCE_TOKEN": source_token,
        "THOUGHTS_E2E_PHONE_TOKEN": phone_token,
        "THOUGHTS_SOURCE_PUBLISHER": str(publisher),
        "THOUGHTS_E2E_CONTROL": str(control),
    }

    # Measurement and comparison switches reach the test: THOUGHTS_E2E_PERF*
    # runs testSyncSpeedRealisticLibrary, THOUGHTS_SOURCE_PUBLICATION_BATCH=0
    # publishes one PDF per publisher run as before 2026-10-14.
    for key, value in os.environ.items():
        if key.startswith("THOUGHTS_E2E_PERF") or key == "THOUGHTS_SOURCE_PUBLICATION_BATCH":
            test_environment[key] = value

    if options.no_html_editions:
        server_environment["THOUGHTS_HTML_EDITIONS"] = "false"

    # Proposal decisions in strict mode (protocol/agent-proposals.md 10.7.1) when asked.
    if "THOUGHTS_PROPOSAL_DECISIONS_STRICT" in os.environ:
        server_environment["THOUGHTS_PROPOSAL_DECISIONS_STRICT"] = os.environ["THOUGHTS_PROPOSAL_DECISIONS_STRICT"]
        test_environment["THOUGHTS_E2E_PROPOSAL_DECISIONS_STRICT"] = os.environ["THOUGHTS_PROPOSAL_DECISIONS_STRICT"]

    services: dict[str, subprocess.Popen | None] = {"server": None, "worker": None, "review_worker": None}
    server_control = None
    started = time.monotonic()
    result = 1
    output = ""
    try:
        python = scratch_interpreter(options.python, server_src, workspace / "python")

        def start_services() -> None:
            with open(logs / "server.log", "ab") as server_log:
                services["server"] = subprocess.Popen([str(python), "-c", "from thoughts_server.cli import server_main; server_main()"],
                                                      cwd=workspace, env=server_environment, stdout=server_log, stderr=subprocess.STDOUT,
                                                      start_new_session=True)
            wait_for_server(url, desktop_token, services["server"])
            with open(logs / "worker.log", "ab") as worker_log:
                services["worker"] = subprocess.Popen([str(python), "-c", "from thoughts_server.cli import worker_main; worker_main()"],
                                                      cwd=workspace, env=server_environment, stdout=worker_log, stderr=subprocess.STDOUT,
                                                      start_new_session=True)
            # The review worker links and places review cards (thoughts-review-worker in production).
            with open(logs / "review-worker.log", "ab") as review_log:
                services["review_worker"] = subprocess.Popen([str(python), "-c", "from thoughts_server.cli import review_worker_main; review_worker_main()"],
                                                             cwd=workspace, env=server_environment, stdout=review_log, stderr=subprocess.STDOUT,
                                                             start_new_session=True)

        def stop_services() -> None:
            for name in ("review_worker", "worker", "server"):
                stop(services[name])

        def migration_command(module: str) -> list[str] | None:
            if not (server_src / f"thoughts_server/{module}.py").is_file():
                return None
            return [str(python), "-m", f"thoughts_server.{module}", "--root", str(library), "--apply", "--canonical"]

        start_services()
        server_control = ServerControl(control, start_services, stop_services, migration_command, server_environment, logs)
        server_control.thread.start()
        print(f"server {url} (sources: {server_src}), workspace {workspace}", flush=True)
        prefix = [part.replace("{cwd}", str(workspace)) for part in shlex.split(options.test_prefix)]
        test = subprocess.Popen([*prefix, str(test_binary), *options.test_args], cwd=workspace, env=test_environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        lines = []
        assert test.stdout is not None
        for line in test.stdout:
            lines.append(line)
            if re.match(r"^(PASS|FAIL!|SKIP|XFAIL|XPASS|Totals|QWARN|QDEBUG   : \S+ E2E)", line) or line.startswith("   Loc:") or "E2E " in line:
                print(line, end="", flush=True)
        result = test.wait()
        output = "".join(lines)
        (logs / "test.log").write_text(output)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
    finally:
        if server_control is not None:
            server_control.stopped.set()
            server_control.thread.join(timeout=700)
        for name in ("review_worker", "worker", "server"):
            stop(services[name])

    elapsed = time.monotonic() - started
    passed = re.findall(r"^PASS   : \S+::(\w+)\(\)", output, re.M)
    failed = re.findall(r"^FAIL!  : \S+::(\w+)\(\)", output, re.M)
    skipped = re.findall(r"^SKIP   : \S+::(\w+)\(\)", output, re.M)
    known = re.findall(r"^XFAIL  : \S+::(\w+)\(\) (.*)$", output, re.M)
    print("\n=== Document pipeline end-to-end summary ===")
    for name in passed:
        if name not in ("initTestCase", "cleanupTestCase"):
            print(f"  PASS  {name}")
    for name in failed:
        print(f"  FAIL  {name}")
    for name in skipped:
        print(f"  SKIP  {name}")
    for name, reason in known:
        print(f"  KNOWN BUG in {name}: {reason}")
    print(f"  {'OK' if result == 0 and output else 'FAILED'} in {elapsed:.0f}s (test exit {result})")
    if result != 0 or not output:
        print("\n--- server log (tail) ---\n" + tail(logs / "server.log"))
        print("\n--- worker log (tail) ---\n" + tail(logs / "worker.log"))
        print("\n--- review worker log (tail) ---\n" + tail(logs / "review-worker.log"))
        print(f"\nFull test output: {logs / 'test.log'}")
    if options.keep or result != 0 or not output:
        print(f"Workspace kept: {workspace}")
    else:
        remove_tree(workspace)
    return 0 if result == 0 and output else 1


if __name__ == "__main__":
    sys.exit(main())
