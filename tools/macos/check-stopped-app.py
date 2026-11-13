#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Validate a stopped Thoughts app before its generated CLI-only installer runs."""
from pathlib import Path
import plistlib
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: check-stopped-app.py /path/to/Thoughts.app")
app = Path(sys.argv[1]).resolve()
with (app / "Contents/Info.plist").open("rb") as stream:
    info = plistlib.load(stream)
if info.get("CFBundleIdentifier") != "org.tbend.thoughts":
    raise SystemExit("Refusing to modify a bundle other than Thoughts")
main = app / "Contents/MacOS" / info["CFBundleExecutable"]
if not main.is_file():
    raise SystemExit("The Thoughts executable is missing")
processes = subprocess.check_output(["/bin/ps", "-axo", "pid=,comm="], text=True)
if any(line.strip().split(None, 1)[-1] == str(main) for line in processes.splitlines()):
    raise SystemExit("Thoughts is running from this bundle; quit it normally before installing the helper")
