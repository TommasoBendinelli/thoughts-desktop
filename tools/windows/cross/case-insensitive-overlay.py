#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Writes a clang VFS overlay that makes lookups under the given directories
case-insensitive, as they are on Windows. Craft's headers include each other with
the wrong case (threadweaver/queue.h for ThreadWeaver/queue.h), which only works on
a case-insensitive file system. Usage: case-insensitive-overlay.py OUT DIR..."""
import json
import os
import sys


def tree(path):
    entries = []
    with os.scandir(path) as it:
        for entry in sorted(it, key=lambda e: e.name):
            if entry.is_dir():
                entries.append({"name": entry.name, "type": "directory", "contents": tree(entry.path)})
            else:
                entries.append({"name": entry.name, "type": "file", "external-contents": os.path.realpath(entry.path)})
    return entries


out, *dirs = sys.argv[1:]
roots = [{"name": os.path.abspath(d), "type": "directory", "contents": tree(d)} for d in dirs]
with open(out, "w") as f:
    json.dump({"version": 0, "case-sensitive": "false", "roots": roots}, f)
