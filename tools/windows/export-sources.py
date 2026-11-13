#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Pack the Library's source folders for a new Thoughts computer.

Sync downloads PDFs, comments, reviews and Project settings, but not the
editable docs-* source folders: a new computer needs a copy first. This packs
each docs-* folder as the source publisher sees it, leaving out what it never
publishes (build outputs, tmp, .git, caches, .ThoughtIgnore matches), so the
archive holds sources and build inputs only. Bytes are stored unchanged.

Sync on this Mac first, so the copy matches Fedora. Never copy Thoughts'
application data (~/Library/Application Support/Thoughts) to the new
computer: its recorded bases would let the new computer publish deletions.

  python3 tools/windows/export-sources.py ~/Desktop/thoughts-sources.zip
"""

from __future__ import annotations

import argparse
import os
import sys
import zipfile
from pathlib import Path

# The native publisher's rules (tools/publisher/buildinputs.cpp): folders it
# never publishes, and files it never uploads.
BUILD_INPUT_EXCLUDED_PARTS = {".cache", ".git", ".pytest_cache", ".runtime", ".venv", "__pycache__", "build",
                              "current_pdfs", "node_modules", "pdf", "tmp", "venv"}
SENSITIVE_BUILD_INPUT_NAMES = {".ds_store", "desktop.ini", "ehthumbs.db", "thumbs.db", ".env", ".env.local",
                               "credentials.json", "id_ed25519", "id_rsa", "secrets.json"}
SENSITIVE_BUILD_INPUT_SUFFIXES = {".key", ".p12", ".pem"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="zip file to write")
    parser.add_argument("--docs", type=Path, default=Path(os.environ.get("THOUGHTS_HOME") or Path.home() / "Thoughts/docs"))
    parser.add_argument("--server-checkout", type=Path, default=Path.home() / "repos/thoughts-server",
                        help="thoughts-server checkout, for its .ThoughtIgnore rules")
    arguments = parser.parse_args()

    sys.path.insert(0, str(arguments.server_checkout / "src"))
    try:
        from thoughts_server.thought_ignore import ThoughtIgnore
    except ImportError as error:
        print(f"Cannot load the .ThoughtIgnore rules from {arguments.server_checkout}: {error}", file=sys.stderr)
        return 2

    docs = arguments.docs.expanduser().resolve()
    projects = sorted(path for path in docs.iterdir() if path.is_dir() and not path.is_symlink() and path.name.startswith("docs-"))
    if not projects:
        print(f"No docs-* folders in {docs}.", file=sys.stderr)
        return 2

    files = 0
    total = 0
    with zipfile.ZipFile(arguments.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for project in projects:
            ignore = ThoughtIgnore(project)
            for directory, subdirectories, names in os.walk(project):
                relative_directory = Path(directory).relative_to(project)
                subdirectories[:] = sorted(
                    name for name in subdirectories
                    if name not in BUILD_INPUT_EXCLUDED_PARTS
                    and not (Path(directory) / name).is_symlink()
                    and not ignore.excludes((relative_directory / name).as_posix(), is_dir=True)
                )
                for name in sorted(names):
                    path = Path(directory) / name
                    relative = (relative_directory / name).as_posix()
                    if path.is_symlink() or not path.is_file() or ignore.excludes(relative):
                        continue
                    lowered = name.casefold()
                    # Secrets never leave this Mac, as the publisher never uploads them.
                    if lowered in SENSITIVE_BUILD_INPUT_NAMES or lowered.startswith(".env.") or Path(lowered).suffix in SENSITIVE_BUILD_INPUT_SUFFIXES:
                        continue
                    archive.write(path, f"{project.name}/{relative}")
                    files += 1
                    total += path.stat().st_size
    print(f"Packed {files} files ({total / 1048576:.1f} MB) from {len(projects)} source folders into {arguments.output}.")
    print("On the PC, extract the archive into %USERPROFILE%\\Thoughts\\docs before the first Sync.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
