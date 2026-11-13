#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Builds the Windows installer on the Linux builder and publishes it as a GitHub
Release, which installed copies of Thoughts offer as an update (Help › Check for
Updates). A published release is built from a clean clone of the public repository
whose HEAD is on its main, never from a private checkout, and the installer's files
are scanned for personal or private strings before they are packed
(tools/release_scan.py); a hit stops the release. Run:

    python3 tools/windows/release.py                  # build + publish windows-YYYY.MM.DD.N
    python3 tools/windows/release.py --no-publish     # build only, keep the setup.exe locally

The builder (tools/windows/cross/README.md) cross-compiles with clang-cl against KDE
Craft's Windows libraries in a container; nothing heavy runs on the Mac.

Settings (each also has an option, see --help):
    THOUGHTS_WINDOWS_BUILDERS      SSH hosts of the Linux builder, comma-separated; the first that answers builds
    THOUGHTS_WINDOWS_BUILDER_ROOT  its tree (default: ~/thoughts-builder on the builder)
    THOUGHTS_RELEASE_REPOSITORY    GitHub owner/repository for the release (default: TommasoBendinelli/thoughts-desktop)
    THOUGHTS_RELEASE_PATTERNS      this computer's own markers for the scan (default: ~/.config/thoughts/release-private-patterns.txt)
    THOUGHTS_DEFAULT_SERVER_URL    the server new installations start with (required)
    THOUGHTS_LEGACY_SERVER_URL     an earlier fixed address that already synchronized installations keep (optional)"""
import argparse
import datetime
import hashlib
import json
import os
import shlex
import subprocess
import sys

IMAGE = "localhost/thoughts-wincross:20261015"
NSIS_IMAGE = "localhost/thoughts-nsis:20261015"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.expanduser("~/.cache/thoughts-windows-releases")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release_scan  # noqa: E402


def run(cmd, **kw):
    print("+", " ".join(shlex.quote(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout.strip()


def builder(hosts):
    for host in hosts:
        if subprocess.run(["ssh", "-o", "ConnectTimeout=6", "-o", "BatchMode=yes", host, "true"], capture_output=True).returncode == 0:
            return host
    sys.exit("No builder answers (tried " + ", ".join(hosts) + ").")


def builder_root(host, configured):
    """The builder's tree: as configured, else ~/thoughts-builder on the builder (an absolute path,
    since the containers mount it at the same place)."""
    if configured:
        return configured.rstrip("/")
    home = subprocess.run(["ssh", "-o", "BatchMode=yes", host, 'printf %s "$HOME"'], check=True, capture_output=True, text=True).stdout.strip()
    if not home.startswith("/"):
        sys.exit(f"Cannot tell the home folder on {host}; pass --builder-root.")
    return f"{home}/thoughts-builder"


def default_sdk():
    cache = os.path.join(ROOT, "build-thoughts", "CMakeCache.txt")
    if os.path.exists(cache):
        for line in open(cache):
            if line.startswith("THOUGHTS_SERVER_SDK_ROOT:"):
                return line.split("=", 1)[1].strip()
    return os.path.join(os.path.dirname(ROOT), "thoughts-server", "sdk", "cpp")


def releases(repository):
    out = subprocess.run(["gh", "release", "list", "-R", repository, "--limit", "200", "--json", "tagName"],
                         check=True, capture_output=True, text=True).stdout
    return [r["tagName"] for r in json.loads(out)]


def next_version(tags):
    today = datetime.date.today().strftime("%Y.%m.%d")
    used = [int(t.rsplit(".", 1)[1]) for t in tags if t.startswith(f"windows-{today}.")]
    return f"{today}.{max(used, default=0) + 1}"


def notes(tags, head):
    previous = sorted((t for t in tags if t.startswith("windows-")), key=lambda t: [int(x) for x in t[8:].split(".")])
    base = previous[-1] if previous else None
    log = git("log", "--no-merges", "--format=- %s", f"{base}..{head}" if base else "-30", "--", ".")
    lines = log.splitlines()[:40]
    header = f"Changes since {base[8:]}:" if base else "Recent changes:"
    return "\n".join([header, *lines]) if lines else "No changes to the desktop app."


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", help="release version (default: YYYY.MM.DD.N)")
    parser.add_argument("--sdk", default=default_sdk(), help="thoughts-server sdk/cpp to build against")
    parser.add_argument("--notes", help="release notes (default: commit subjects since the last release)")
    parser.add_argument("--no-publish", action="store_true", help="build the installer but publish nothing")
    parser.add_argument("--allow-unpushed", action="store_true", help="with --no-publish: build a dirty or unpushed tree")
    parser.add_argument("--builder", action="append",
                        help="SSH host of the Linux builder, repeatable; the first that answers builds (default: THOUGHTS_WINDOWS_BUILDERS)")
    parser.add_argument("--builder-root", default=os.environ.get("THOUGHTS_WINDOWS_BUILDER_ROOT"),
                        help="the builder's tree (default: THOUGHTS_WINDOWS_BUILDER_ROOT, else ~/thoughts-builder on the builder)")
    parser.add_argument("--repository", default=os.environ.get("THOUGHTS_RELEASE_REPOSITORY") or release_scan.PUBLIC_REPOSITORY,
                        help=f"GitHub owner/repository of the release, which the installer also checks for updates "
                             f"(default: THOUGHTS_RELEASE_REPOSITORY, else {release_scan.PUBLIC_REPOSITORY})")
    parser.add_argument("--server-url", default=os.environ.get("THOUGHTS_DEFAULT_SERVER_URL"),
                        help="https:// address new installations start with (default: THOUGHTS_DEFAULT_SERVER_URL)")
    parser.add_argument("--legacy-server-url", default=os.environ.get("THOUGHTS_LEGACY_SERVER_URL", ""),
                        help="earlier fixed address that already synchronized installations keep (default: THOUGHTS_LEGACY_SERVER_URL, else none)")
    args = parser.parse_args()
    if not (args.server_url or "").startswith("https://"):
        sys.exit("Name the server new installations start with: --server-url or THOUGHTS_DEFAULT_SERVER_URL (an https:// address).")
    hosts = args.builder or [h.strip() for h in os.environ.get("THOUGHTS_WINDOWS_BUILDERS", "").split(",") if h.strip()]
    if not hosts:
        sys.exit("Name the Linux builder with --builder or THOUGHTS_WINDOWS_BUILDERS.")
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
    if f"windows-{version}" in tags:
        sys.exit(f"Release windows-{version} exists already.")
    setup = f"Thoughts-{version}-setup.exe"
    host = builder(hosts)
    RB = builder_root(host, args.builder_root)
    W = f"{RB}/wincross"
    print(f"Building Thoughts {version} ({head[:9]}) on {host}, SDK {args.sdk}")

    excludes = ["--exclude", ".git", "--exclude", "build*/", "--exclude", ".claude"]
    run(["ssh", host, f"mkdir -p {W}/wt/src {W}/wt/server/sdk {W}/dist {W}/logs"])
    run(["rsync", "-a", "--delete", *excludes, ROOT + "/", f"{host}:{W}/wt/src/"])
    run(["rsync", "-a", "--delete", args.sdk.rstrip("/") + "/", f"{host}:{W}/wt/server/sdk/cpp/"])
    # This computer's own markers for the scan (an empty list when it has none); never in the repository.
    markers = release_scan.private_patterns()
    run(["ssh", host, f"cat > {W}/wt/release-private-patterns.txt"], input="\n".join(p.decode() for p in markers.values()).encode())

    # Before packing: the installer may carry nothing personal. A build that is not published only warns.
    scan_outcome = " || echo 'These strings must be gone before this build may be published.'" if args.no_publish else ""
    podman = f"{RB}/bin/thoughts-desktop-podman run --rm --init --security-opt label=disable -e WINCROSS={W} -v {W}:{W}"
    script = f"""set -euo pipefail
{podman} --network none -w {W}/wt {IMAGE} bash {W}/wt/src/tools/windows/cross/build-in-container.sh \\
    {W}/wt/src {W}/wt/server/sdk/cpp {W}/wt/build {W}/wt/stage {version} {repository} \\
    {shlex.quote(args.server_url)} {shlex.quote(args.legacy_server_url)} > {W}/logs/release-{version}.log 2>&1 \\
  || {{ grep -nE 'error:|CMake Error|FAILED' {W}/logs/release-{version}.log | head -20; tail -5 {W}/logs/release-{version}.log; exit 1; }}
{podman} --network none {IMAGE} python3 {W}/wt/src/tools/windows/cross/stage-runtime.py {W}/wt/stage {W}/craft-win {W}/vcredist/x64 {W}/xwin-sdk {W}/wt/runtime
{podman} --network none {IMAGE} python3 {W}/wt/src/tools/release_scan.py scan --patterns {W}/wt/release-private-patterns.txt {W}/wt/runtime{scan_outcome}
{podman} --network none {NSIS_IMAGE} makensis -V2 -DVERSION={version} -DSTAGE={W}/wt/runtime \\
    -DICON={W}/wt/src/icons/thoughts.ico -DOUTFILE={W}/dist/{setup} {W}/wt/src/tools/windows/installer/thoughts.nsi
ls -l {W}/dist/{setup}
"""
    run(["ssh", host, "bash -s"], input=script.encode())
    os.makedirs(OUT, exist_ok=True)
    local = os.path.join(OUT, setup)
    run(["rsync", "-a", f"{host}:{W}/dist/{setup}", local])
    digest = hashlib.sha256(open(local, "rb").read()).hexdigest()
    print(f"Installer: {local} ({os.path.getsize(local) / 1e6:.0f} MB, sha256 {digest})")
    if args.no_publish:
        return
    body = (args.notes or notes(tags, head)) + f"\n\nBuilt from {head[:9]}. SHA-256 of {setup}: `{digest}`"
    run(["gh", "release", "create", f"windows-{version}", local, "-R", repository, "--target", head,
         "--title", f"Thoughts {version} for Windows", "--notes", body])


if __name__ == "__main__":
    main()
