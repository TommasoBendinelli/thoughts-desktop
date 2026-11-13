#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Builds the self-contained Mac disk image and publishes it as a GitHub Release, which
Mac copies of Thoughts made by this script offer as an update (Help › Check for
Updates). A published release is built from a clean clone of the public repository
whose HEAD is on its main, never from a private checkout, and the disk image is
scanned for personal or private strings before upload (tools/release_scan.py); a
hit stops the release. Run:

    python3 tools/macos/release.py                  # build + publish mac-YYYY.MM.DD.N
    python3 tools/macos/release.py --no-publish     # build only, keep the dmg locally

The image is built by make-dmg.py (in build-dmg, signed with the persistent local
identity of tools/macos/setup-signing.py) with THOUGHTS_RELEASE_VERSION and
THOUGHTS_UPDATE_REPOSITORY set. Mac releases are never marked Latest, so the Windows
release stays the one GitHub shows first; both kinds of clients pick their releases
by tag prefix. gh must be signed in with an account that may create releases there.

Settings (each also has an option, see --help):
    THOUGHTS_RELEASE_REPOSITORY    GitHub owner/repository for the release (default: TommasoBendinelli/thoughts-desktop)
    THOUGHTS_SERVER_SDK_ROOT       thoughts-server sdk/cpp to build against (default: ../thoughts-server/sdk/cpp)
    THOUGHTS_DEFAULT_SERVER_URL    the server new installations start with (default: the build's neutral one)
    THOUGHTS_CRAFT_ROOT            the KDE Craft root the app is built against and bundled from (default: ~/CraftRoot)
    THOUGHTS_RELEASE_PATTERNS      this computer's own markers for the scan (default: ~/.config/thoughts/release-private-patterns.txt)"""
import argparse
import datetime
import hashlib
import json
import os
import shlex
import subprocess
import sys

PREFIX = "mac-"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.expanduser("~/.cache/thoughts-mac-releases")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release_scan  # noqa: E402
DEFAULT_SDK = os.environ.get("THOUGHTS_SERVER_SDK_ROOT") or os.path.join(os.path.dirname(ROOT), "thoughts-server", "sdk", "cpp")
# Passed to make-dmg.py and named in the release notes, so the two always agree.
MACOS_MIN = "13.3"


def run(cmd, **kw):
    print("+", " ".join(shlex.quote(str(c)) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout.strip()


def releases(repository):
    out = subprocess.run(["gh", "release", "list", "-R", repository, "--limit", "200", "--json", "tagName"],
                         check=True, capture_output=True, text=True).stdout
    return [r["tagName"] for r in json.loads(out)]


def next_version(tags):
    today = datetime.date.today().strftime("%Y.%m.%d")
    used = [int(t.rsplit(".", 1)[1]) for t in tags if t.startswith(f"{PREFIX}{today}.")]
    return f"{today}.{max(used, default=0) + 1}"


def notes(tags, head):
    previous = sorted((t for t in tags if t.startswith(PREFIX)), key=lambda t: [int(x) for x in t[len(PREFIX):].split(".")])
    base = previous[-1] if previous else None
    # The previous release's commit, from the remote tag, without fetching tags.
    base_commit = None
    if base:
        line = subprocess.run(["git", "-C", ROOT, "ls-remote", "origin", f"refs/tags/{base}^{{}}", f"refs/tags/{base}"],
                              capture_output=True, text=True).stdout.split()
        candidate = line[0] if line else None
        if candidate and subprocess.run(["git", "-C", ROOT, "cat-file", "-e", f"{candidate}^{{commit}}"], capture_output=True).returncode == 0:
            base_commit = candidate
    log = git("log", "--no-merges", "--format=- %s", f"{base_commit}..{head}" if base_commit else "-30", "--", ".")
    lines = log.splitlines()[:40]
    header = f"Changes since {base[len(PREFIX):]}:" if base_commit else "Recent changes:"
    return "\n".join([header, *lines]) if lines else "No changes to the desktop app."


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="release version (default: YYYY.MM.DD.N)")
    parser.add_argument("--sdk", default=DEFAULT_SDK, help="thoughts-server sdk/cpp to build against")
    parser.add_argument("--notes", help="release notes (default: commit subjects since the last mac- release)")
    parser.add_argument("--no-publish", action="store_true", help="build the disk image but publish nothing")
    parser.add_argument("--allow-unpushed", action="store_true", help="with --no-publish: build a dirty or unpushed tree")
    parser.add_argument("--repository", default=os.environ.get("THOUGHTS_RELEASE_REPOSITORY") or release_scan.PUBLIC_REPOSITORY,
                        help=f"GitHub owner/repository of the release, which the app also checks for updates "
                             f"(default: THOUGHTS_RELEASE_REPOSITORY, else {release_scan.PUBLIC_REPOSITORY})")
    parser.add_argument("--server-url", default=os.environ.get("THOUGHTS_DEFAULT_SERVER_URL", ""),
                        help="https:// address new installations start with (default: THOUGHTS_DEFAULT_SERVER_URL, else the build's neutral one)")
    args = parser.parse_args()
    repository = args.repository

    head = git("rev-parse", "HEAD")
    if args.allow_unpushed and not args.no_publish:
        sys.exit("--allow-unpushed only builds; a published release must match a pushed commit.")
    if not args.allow_unpushed:
        if git("status", "--porcelain", "--untracked-files=no"):
            sys.exit("The checkout has uncommitted changes; a release must match a commit.")
        if not git("branch", "-r", "--contains", head):
            sys.exit(f"HEAD {head[:9]} is not pushed; push it first so the release points at published source.")
    if not args.no_publish:
        problem = release_scan.source_problem(ROOT, repository)
        if problem:
            sys.exit(problem)
    if not os.path.exists(os.path.join(args.sdk, "CMakeLists.txt")):
        sys.exit(f"No Thoughts server SDK at {args.sdk}; pass --sdk.")
    tags = releases(repository)
    version = args.version or next_version(tags)
    if f"{PREFIX}{version}" in tags:
        sys.exit(f"Release {PREFIX}{version} exists already.")
    name = f"Thoughts-{version}.dmg"
    local = os.path.join(OUT, name)
    os.makedirs(OUT, exist_ok=True)
    print(f"Building Thoughts {version} ({head[:9]}) for the Mac, SDK {args.sdk}", flush=True)
    run([sys.executable, os.path.join(ROOT, "tools/macos/make-dmg.py"), "--server-sdk", args.sdk, "--macos-min", MACOS_MIN,
         "--release-version", version, "--update-repository", repository, "--server-url", args.server_url, "--output", local])
    digest = hashlib.sha256(open(local, "rb").read()).hexdigest()
    print(f"Disk image: {local} ({os.path.getsize(local) / 1e6:.0f} MB, sha256 {digest})")
    # The image may carry nothing personal: build paths, hosts, keys, this computer's markers.
    hits = release_scan.scan([local])
    if hits:
        print(release_scan.report(hits), file=sys.stderr)
        if not args.no_publish:
            sys.exit("Not published.")
        print("These strings must be gone before this image may be published.", file=sys.stderr)
    if args.no_publish:
        return
    body = (args.notes or notes(tags, head)) + (
        f"\n\nBuilt from {head[:9]}, for Apple silicon with macOS {MACOS_MIN} or later. "
        f"It is not signed with an Apple Developer ID: allow it once under System Settings › Privacy & Security, as "
        f"\"Install Thoughts.md\" inside the image describes. SHA-256 of {name}: `{digest}`")
    run(["gh", "release", "create", f"{PREFIX}{version}", local, "-R", repository, "--target", head,
         "--title", f"Thoughts {version} for Mac", "--notes", body, "--latest=false"])


if __name__ == "__main__":
    main()
