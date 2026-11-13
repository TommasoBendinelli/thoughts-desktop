#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Run every end-to-end Sync stage against the Fedora test instance.

    tools/sync-e2e/run_all.py [--server-src PATH] [--stage N ...]

It fetches the test credentials into a private temporary file (never
printed), builds the desktop Sync driver, and runs each stage in order.
Every stage resets the test instance first. Production is never contacted.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import harness  # noqa: E402

STAGES = {
    1: "stage1_server_smoke.py",
    2: "stage2_rebuild_reading.py",
    3: "stage3_not_ready_documents.py",
    4: "stage4_desktop_sync.py",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", type=int, action="append", choices=sorted(STAGES))
    args = parser.parse_args()
    python = sys.executable
    ninja = os.environ.get("NINJA") or shutil.which("ninja") or str(Path.home() / "CraftRoot/dev-utils/bin/ninja")
    subprocess.run([ninja, "thoughts-sync-driver", "thoughts-publish-sources"], cwd=harness.DESKTOP_BUILD, check=True,
                   stdout=subprocess.DEVNULL)
    if not harness.TEST_HOST:
        raise SystemExit("Set THOUGHTS_E2E_TEST_HOST to the SSH destination (user@host) of the test instance")
    environment = dict(os.environ)
    results = []
    with tempfile.TemporaryDirectory(prefix="thoughts-e2e-credentials-") as private:
        credentials = Path(private) / "credentials.json"
        output = subprocess.check_output(["ssh", "-o", "BatchMode=yes", harness.TEST_HOST,
                                          f"sudo -n {harness.TEST_CONTROL} credentials"], text=True)
        credentials.touch(mode=0o600)
        credentials.write_text(json.dumps(json.loads(output)))
        harness.credentials(credentials)
        for number in args.stage or sorted(STAGES):
            started = time.monotonic()
            print(f"\n=== Stage {number}: {STAGES[number]}", flush=True)
            code = subprocess.run([python, "-u", str(HERE / STAGES[number]), str(credentials)], env=environment).returncode
            results.append((number, code, time.monotonic() - started))
        harness.control("reset")
    print("\n=== Summary")
    for number, code, seconds in results:
        print(f"Stage {number} {'passed' if code == 0 else 'FAILED'} in {seconds:.0f}s  ({STAGES[number]})")
    return 0 if all(code == 0 for _, code, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
