#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Keeps personal and private data out of public releases.

A public release (tools/windows/release.py, tools/macos/release.py) is built from a clone of
the public repository and scanned before anything is uploaded:

    release_scan.py scan PATH...      files, folders, .zip archives or a .dmg (mounted read-only)
    release_scan.py check-source DIR OWNER/REPO

scan prints every hit and exits 1 when there is one. It looks for home folders (the build
paths a compiler leaves in binaries), tailnet hosts and Tailscale addresses, and private keys
and tokens. Hosts and paths with "example" in them are fixtures and pass. Markers that belong
to one person (an email address, a tailnet name, Library names) are not written here: they
are read, one regular expression per line, from ~/.config/thoughts/release-private-patterns.txt
or the file named by THOUGHTS_RELEASE_PATTERNS, which is never part of a repository.

A generic hit the owner decides to publish anyway (typically their home folder in the binaries,
when the build libraries live there) is listed, as a regular expression matching the whole hit,
in ~/.config/thoughts/release-accepted.txt or the file named by THOUGHTS_RELEASE_ACCEPTED.
Private markers cannot be accepted.

check-source exits 1 unless DIR is a clean clone of github.com/OWNER/REPO, has no private
remote, and its HEAD is on that repository's main as GitHub has it.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

# Accounts that build machines use (other projects' CI paths in third-party libraries); a
# person's home folder is anything else.
_BUILD_ACCOUNTS = rb"(?:Shared|Public|runner|qt|build|builder|buildbot|ci|user|username|vagrant|ubuntu)"
GENERIC_PATTERNS = {
    "home folder": rb"/Users/(?!" + _BUILD_ACCOUNTS + rb"/)[A-Za-z0-9._-]+/|/home/(?!" + _BUILD_ACCOUNTS
                   + rb"/)[a-z_][a-z0-9_-]*/|[A-Za-z]:\\\\?Users\\\\?(?!" + _BUILD_ACCOUNTS + rb"\\)[A-Za-z0-9._ -]+\\",
    "tailnet host": rb"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.ts\.net",
    "Tailscale address": rb"(?<![0-9.])100\.(?:6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.[0-9]{1,3}\.[0-9]{1,3}(?![0-9.])",
    # Key material, not the bare label that key-handling code carries as a constant.
    "private key": rb"-----BEGIN (?:[A-Z]+ )*PRIVATE KEY-----\s*[A-Za-z0-9+/=]{40,}",
    "token": rb"\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{60,}|sk-(?:ant|or)-[A-Za-z0-9_-]{32,})",
}
# Where the release tools publish, and what release builds check for updates, by default.
PUBLIC_REPOSITORY = "TommasoBendinelli/thoughts-desktop"
PRIVATE_PATTERNS_FILE = Path.home() / ".config" / "thoughts" / "release-private-patterns.txt"
# Generic hits this computer's owner has decided to publish anyway, for example the home folder
# that the compiler left in the binaries: one regular expression per line, matched against the
# whole hit. A marker from the private patterns file is never accepted this way.
ACCEPTED_FILE = Path.home() / ".config" / "thoughts" / "release-accepted.txt"
MAX_FILE_BYTES = 1 << 30
SHOWN_PER_PATTERN = 5


@dataclass(frozen=True)
class Hit:
    path: str
    kind: str
    text: str


def private_patterns(path: str | os.PathLike | None = None) -> dict[str, bytes]:
    """This computer's own markers: one regular expression per line, # starts a comment."""
    file = Path(path or os.environ.get("THOUGHTS_RELEASE_PATTERNS") or PRIVATE_PATTERNS_FILE)
    if not file.is_file():
        return {}
    patterns = {}
    for number, line in enumerate(file.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if line and not line.startswith("#"):
            re.compile(line.encode())  # A broken line stops the release rather than scanning less.
            patterns[f"private marker (line {number})"] = line.encode()
    return patterns


def accepted_lines(path: str | os.PathLike | None = None) -> list[str]:
    """Generic hits the owner accepts: one regular expression per line, # starts a comment.
    A release that scans on a builder copies these lines there (THOUGHTS_RELEASE_ACCEPTED)."""
    file = Path(path or os.environ.get("THOUGHTS_RELEASE_ACCEPTED") or ACCEPTED_FILE)
    if not file.is_file():
        return []
    lines = (line.strip() for line in file.read_text(encoding="utf-8").splitlines())
    return [line for line in lines if line and not line.startswith("#")]


def accepted_patterns(path: str | os.PathLike | None = None) -> list[re.Pattern[bytes]]:
    """Generic hits the owner accepts, compiled (see accepted_lines)."""
    return [re.compile(line.encode()) for line in accepted_lines(path)]


def scan_bytes(data: bytes, label: str, patterns: dict[str, bytes]) -> list[Hit]:
    hits = []
    accepted = accepted_patterns()
    for kind, pattern in patterns.items():
        marker = kind.startswith("private marker")
        seen = set()
        for match in re.finditer(pattern, data, re.IGNORECASE if marker else 0):
            text = match.group(0)
            # Fixture hosts and paths pass; a marker of this computer never does.
            if (not marker and b"example" in text.lower()) or text in seen:
                continue
            if not marker and any(pattern.fullmatch(text) for pattern in accepted):
                continue
            seen.add(text)
            hits.append(Hit(label, kind, text.decode("utf-8", "replace")[:120]))
            if len(seen) >= SHOWN_PER_PATTERN:
                break
    return hits


def scan_file(path: Path, label: str, patterns: dict[str, bytes]) -> list[Hit]:
    if path.is_symlink() or path.stat().st_size > MAX_FILE_BYTES:
        return []
    if path.suffix.lower() == ".zip" and zipfile.is_zipfile(path):
        hits = []
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                if not member.is_dir() and member.file_size <= MAX_FILE_BYTES:
                    hits += scan_bytes(archive.read(member), f"{label}!{member.filename}", patterns)
        return hits
    return scan_bytes(path.read_bytes(), label, patterns)


def scan_tree(root: Path, patterns: dict[str, bytes]) -> list[Hit]:
    if root.is_file():
        return scan_file(root, str(root), patterns)
    hits = []
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            path = Path(directory, name)
            hits += scan_file(path, str(path.relative_to(root.parent)), patterns)
    return hits


def scan_dmg(image: Path, patterns: dict[str, bytes]) -> list[Hit]:
    with tempfile.TemporaryDirectory(prefix="thoughts-release-scan-") as mountpoint:
        subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-noverify", "-mountpoint", mountpoint, str(image)],
                       check=True, capture_output=True)
        try:
            return [Hit(f"{image.name}:{hit.path}", hit.kind, hit.text) for hit in scan_tree(Path(mountpoint), patterns)]
        finally:
            subprocess.run(["hdiutil", "detach", mountpoint, "-force"], check=False, capture_output=True)


def scan(paths: list[str], patterns_file: str | None = None) -> list[Hit]:
    patterns = {**GENERIC_PATTERNS, **private_patterns(patterns_file)}
    hits = []
    for name in paths:
        path = Path(name)
        if not path.exists():
            raise FileNotFoundError(name)
        hits += scan_dmg(path, patterns) if path.suffix.lower() == ".dmg" else scan_tree(path, patterns)
    return hits


def report(hits: list[Hit]) -> str:
    lines = [f"{len(hits)} personal or private string(s) found; nothing may be published:"]
    lines += [f"  {hit.kind}: {hit.text!r} in {hit.path}" for hit in hits[:200]]
    if len(hits) > 200:
        lines.append(f"  … and {len(hits) - 200} more")
    return "\n".join(lines)


def is_public_url(url: str, repository: str) -> bool:
    url = url.strip().removesuffix("/")
    return url.removesuffix(".git").lower() in {f"git@github.com:{repository}".lower(),
                                                 f"https://github.com/{repository}".lower(),
                                                 f"ssh://git@github.com/{repository}".lower()}


def source_problem(root: str, repository: str, fetch: bool = True) -> str | None:
    """Why ROOT may not build a public release of REPOSITORY, or None when it may."""
    def git(*args: str) -> str:
        return subprocess.run(["git", "-C", root, *args], check=True, capture_output=True, text=True).stdout.strip()

    remotes = {}
    for line in git("remote", "-v").splitlines():
        name, url, *_ = line.split()
        remotes[name] = url
    private = sorted(name for name, url in remotes.items() if "-private" in url)
    if private:
        return (f"This checkout has a private remote ({', '.join(private)}). Public releases are built from a clone "
                f"of github.com/{repository} only, never from a private checkout.")
    if not is_public_url(remotes.get("origin", ""), repository):
        return f"origin is {remotes.get('origin') or 'missing'}; build public releases from a clone of github.com/{repository}."
    if git("status", "--porcelain", "--untracked-files=no"):
        return "The checkout has uncommitted changes; a release must match a published commit."
    head = git("rev-parse", "HEAD")
    if fetch:
        git("fetch", "--quiet", "origin", "main")
        main = git("rev-parse", "FETCH_HEAD")
    else:
        main = git("rev-parse", "origin/main")
    if subprocess.run(["git", "-C", root, "merge-base", "--is-ancestor", head, main], capture_output=True).returncode != 0:
        return f"HEAD {head[:9]} is not on {repository}'s main; publish the source first, then release from it."
    return None


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[0] == "scan":
        args = argv[1:]
        patterns_file = None
        if args[:1] == ["--patterns"]:
            patterns_file, args = args[1], args[2:]
        hits = scan(args, patterns_file)
        if hits:
            print(report(hits), file=sys.stderr)
            return 1
        print(f"No personal or private strings in {', '.join(args)}.")
        return 0
    if len(argv) == 3 and argv[0] == "check-source":
        problem = source_problem(argv[1], argv[2])
        if problem:
            print(problem, file=sys.stderr)
            return 1
        print(f"{argv[1]} is a clean clone of {argv[2]} on its main.")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
