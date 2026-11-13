#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Run proposaldecisionstest's server cases against a real local thoughts-server.

Starts the server of --server-src (uvicorn on sqlite, 127.0.0.1 only, in a
temporary directory) in compatible mode, or with
THOUGHTS_PROPOSAL_DECISIONS_STRICT on (--strict), then runs
proposaldecisionstest testClaimsAgainstServer: the native publisher code
and the desktop's decision claims against the server's claim matching
(protocol/agent-proposals.md section 10). The server checkout is only read.

    tools/e2e/run-proposal-decisions-server.py --server-src <thoughts-server>/src \\
        --python <venv with the server's dependencies>/bin/python [--strict] [--build-dir build-thoughts]
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-src", required=True)
    parser.add_argument("--python", required=True)
    parser.add_argument("--build-dir", default="build-thoughts")
    parser.add_argument("--strict", action="store_true")
    options = parser.parse_args()
    workspace = Path(tempfile.mkdtemp(prefix="thoughts-decisions-"))
    for directory in ("state", "objects", "library", "home"):
        (workspace / directory).mkdir()
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    token = secrets.token_hex(16)
    environment = {
        "PATH": os.environ["PATH"],
        "HOME": str(workspace / "home"),
        "PYTHONPATH": options.server_src,
        "PYTHONDONTWRITEBYTECODE": "1",
        "THOUGHTS_DATABASE_URL": f"sqlite:///{workspace / 'state/thoughts.db'}",
        "THOUGHTS_STATE_ROOT": str(workspace / "state"),
        "THOUGHTS_OBJECT_ROOT": str(workspace / "objects"),
        "THOUGHTS_DOCS_ROOT": str(workspace / "library"),
        "THOUGHTS_WORKTREE_ROOT": str(workspace / "library"),
        "THOUGHTS_API_HOST": "127.0.0.1",
        "THOUGHTS_API_PORT": str(port),
        "THOUGHTS_BOOTSTRAP_TOKENS": json.dumps({"publisher": token}),
        "THOUGHTS_PROVIDER_SYNC_ENABLED": "false",
        "THOUGHTS_PROPOSAL_DECISIONS_STRICT": "true" if options.strict else "false",
    }
    log = open(workspace / "server.log", "wb")
    server = subprocess.Popen([options.python, "-c", "from thoughts_server.cli import server_main; server_main()"],
                              cwd=workspace, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.monotonic() + 60
        while True:
            if server.poll() is not None or time.monotonic() > deadline:
                print((workspace / "server.log").read_text()[-3000:], file=sys.stderr)
                return 1
            try:
                request = urllib.request.Request(url + "/v1/sources", headers={"Authorization": "Bearer " + token})
                urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5).read()
                break
            except (urllib.error.URLError, ConnectionError, OSError):
                time.sleep(0.2)
        test = dict(os.environ, THOUGHTS_DECISIONS_SERVER=url, THOUGHTS_DECISIONS_SERVER_TOKEN=token,
                    THOUGHTS_DECISIONS_SERVER_STRICT="1" if options.strict else "0", QT_QPA_PLATFORM="offscreen")
        binary = REPO / options.build_dir / "bin/proposaldecisionstest"
        return subprocess.run([str(binary), "testClaimsAgainstServer"], env=test).returncode
    finally:
        os.killpg(server.pid, signal.SIGTERM)
        server.wait(timeout=30)
        log.close()
        shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
