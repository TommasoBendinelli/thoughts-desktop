#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Sign a stopped/staged Thoughts bundle with the persistent local identity."""
from pathlib import Path
import plistlib
import re
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: sign-app.py /path/to/Thoughts.app")
app = Path(sys.argv[1]).resolve()
state = Path.home() / "Library/Application Support/Thoughts/signing"
identity_file = state / "identity.txt"
if not identity_file.exists():
    raise SystemExit("Signing identity missing. Run tools/macos/setup-signing.py once; ad hoc signing is not a fallback.")
identity = identity_file.read_text().strip()
if not re.fullmatch(r"[0-9A-F]{40}", identity):
    raise SystemExit("Invalid signing identity fingerprint")
with (app / "Contents/Info.plist").open("rb") as stream:
    info = plistlib.load(stream)
if info.get("CFBundleIdentifier") != "org.tbend.thoughts":
    raise SystemExit("Refusing to sign an app other than org.tbend.thoughts")
main = app / "Contents/MacOS" / info["CFBundleExecutable"]
processes = subprocess.check_output(["/bin/ps", "-axo", "pid=,comm="], text=True)
if any(line.strip().split(None, 1)[-1] == str(main) for line in processes.splitlines()):
    raise SystemExit("Thoughts is running from this bundle. Quit it normally before signing, or sign a staging copy.")


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def sign(path, identifier):
    requirement = f'=designated => identifier "{identifier}" and certificate leaf = H"{identity}"'
    run("/usr/bin/codesign", "--force", "--sign", identity, "--timestamp=none",
        "--preserve-metadata=entitlements,flags", "--identifier", identifier,
        "--requirements", requirement, path)


# This local bundle contains five executables and external CraftRoot libraries.
# Do not recursively re-sign external libraries or follow symlinks out of it.
for path in sorted((app / "Contents/MacOS").iterdir()):
    if path == main:
        continue
    if path.is_symlink() or not path.is_file() or path.name not in {"thoughtsctl", "thoughts-comment-sync", "thoughts-publish-sources", "thoughts-watch-build"}:
        raise SystemExit(f"Unrecognized nested code; review packaging before signing: {path}")
    sign(path, "org.tbend.thoughts." + path.name)
sign(app, "org.tbend.thoughts")
run("/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", app)
run("/usr/bin/codesign", "--verify", "-R",
    f'=identifier "org.tbend.thoughts" and certificate leaf = H"{identity}"', app)
run("/usr/bin/codesign", "-d", "-r-", app)
