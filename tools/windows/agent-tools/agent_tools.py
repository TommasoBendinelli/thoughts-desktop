#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""The Python, the Thoughts tools and the reply waker the Windows installer brings.

Agents on a person's own Windows PC need a Python for the Thoughts tools (tools/mcp, which Claude
Code starts) and for the reply waker (tools/waker, which Thoughts runs). The installer brings its
own: the official CPython embeddable package for Windows amd64 from python.org, and the win_amd64
wheels of what the tools need from PyPI, every file pinned by its SHA-256 in lock.json. Installed:

    <installation>/python-<id>/            python.exe, pythonw.exe, python313.zip, Lib/site-packages
    <installation>/tools/mcp/              server.py, agent_notes.py, conversation_identity.py
    <installation>/tools/waker/            thoughts_reply_waker.py
    <installation>/tools/agent-tools.json  where they are (ClaudeCode::bundledTools reads it)

<id> is made from lock.json, so another Python or other wheels go to another folder: an update
never writes over the Python of a Claude Code session that runs (Windows keeps its files locked),
and Thoughts registers the tools again when the folder changed.

    agent_tools.py lock WHEELS_DIR --python-version 3.13.17   write lock.json (network: python.org, pypi.org)
    agent_tools.py fetch CACHE                                  download what lock.json names (network)
    agent_tools.py stage CACHE SRC OUT                          build the folders above in OUT (offline)

Downloads come only from https://www.python.org/ftp/python/ and PyPI's file host
(https://files.pythonhosted.org/packages/), and a file whose SHA-256 is not the pinned one is
refused. `lock` takes the wheels `pip download --only-binary=:all: --platform win_amd64
--python-version 3.13 ... -r requirements-win.txt` left in WHEELS_DIR and records, for each, the
URL PyPI gives for exactly that SHA-256 (so every pinned file is PyPI's own). Standard library only.
"""
from __future__ import annotations

import argparse
import calendar
import hashlib
import json
import os
import re
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
LOCK = HERE / "lock.json"
PYTHON_URL = "https://www.python.org/ftp/python/"
WHEEL_URL = "https://files.pythonhosted.org/packages/"
# The Thoughts tools' files and the waker, from the source tree.
TOOL_FILES = {"tools/mcp": ["server.py", "agent_notes.py", "conversation_identity.py"], "tools/waker": ["thoughts_reply_waker.py"]}
# What the tools never use from the wheels: pywin32's IDE, help file, demos and tests, and its
# ADO and ISAPI modules (mcp uses win32api, win32con, win32job and pywintypes).
PRUNE = ["pythonwin", "PyWin32.chm", "adodbapi", "isapi", "win32/Demos", "win32/test", "win32com/demos", "win32com/test",
         "win32comext/axdebug", "win32comext/axscript/demos", "win32comext/axscript/test"]
# Every installed file gets this time, so two stages of one lock are the same bytes and times.
FIXED_TIME = calendar.timegm((2026, 1, 1, 0, 0, 0, 0, 0, 0))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_lock(path: Path = LOCK) -> dict:
    lock = json.loads(path.read_text(encoding="utf-8"))
    check_lock(lock)
    return lock


def check_lock(lock: dict) -> None:
    """Every file comes from python.org or PyPI's file host, has a name of its own and a SHA-256."""
    python = lock.get("python") or {}
    entries = [python, *(lock.get("wheels") or [])]
    if not python or len(entries) < 2:
        raise SystemExit("lock.json: no Python or no wheels")
    if not re.fullmatch(r"3\.\d+\.\d+", str(python.get("version") or "")):
        raise SystemExit("lock.json: python.version must be like 3.13.17")
    for entry in entries:
        url, name, digest = str(entry.get("url") or ""), str(entry.get("file") or ""), str(entry.get("sha256") or "")
        allowed = PYTHON_URL if entry is python else WHEEL_URL
        if not url.startswith(allowed) or not url.endswith("/" + name):
            raise SystemExit(f"lock.json: {name or url}: only {allowed}... may be downloaded, and the URL must end with the file name")
        if not re.fullmatch(r"[A-Za-z0-9._+-]+\.(?:zip|whl)", name) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise SystemExit(f"lock.json: {name}: a file name and a SHA-256 are needed")
        if entry is not python and not re.search(r"-(?:py3-none-any|cp\d+-(?:cp\d+|abi3)-win_amd64)\.whl$", name):
            raise SystemExit(f"lock.json: {name} is not a wheel for Windows amd64")


def tools_id(lock: dict) -> str:
    """The Python folder's id: the same lock, the same folder."""
    pinned = [lock["python"]["sha256"], *sorted(wheel["sha256"] for wheel in lock["wheels"])]
    return hashlib.sha256("\n".join(pinned).encode()).hexdigest()[:12]


def python_tag(version: str) -> str:
    major, minor = version.split(".")[:2]
    return f"python{major}{minor}"


# ---------------------------------------------------------------- lock (network)


def pypi_files(name: str, version: str) -> list[dict]:
    with urllib.request.urlopen(f"https://pypi.org/pypi/{name}/{version}/json", timeout=60) as response:  # noqa: S310 (fixed host)
        return json.load(response).get("urls") or []


def python_release_file(version: str) -> dict:
    """python.org's own record of the embeddable package (its SHA-256 among them)."""
    base = "https://www.python.org/api/v2/downloads/"
    with urllib.request.urlopen(f"{base}release/?name=Python%20{version}", timeout=60) as response:  # noqa: S310
        releases = json.load(response)
    if not releases:
        raise SystemExit(f"python.org has no Python {version}")
    release = releases[0]["resource_uri"].rstrip("/").rsplit("/", 1)[1]
    with urllib.request.urlopen(f"{base}release_file/?release={release}", timeout=60) as response:  # noqa: S310
        files = json.load(response)
    name = f"python-{version}-embed-amd64.zip"
    for entry in files:
        if str(entry.get("url") or "").endswith("/" + name):
            return {"version": version, "file": name, "url": entry["url"], "sha256": entry["sha256_sum"]}
    raise SystemExit(f"python.org lists no {name}")


def write_lock(wheels_dir: Path, version: str, output: Path = LOCK) -> dict:
    wheels = []
    for path in sorted(wheels_dir.glob("*.whl")):
        name, release = path.name.split("-")[:2]
        digest = sha256(path)
        match = next((entry for entry in pypi_files(name, release) if entry.get("digests", {}).get("sha256") == digest), None)
        if match is None:
            raise SystemExit(f"{path.name}: PyPI has no file with this SHA-256")
        wheels.append({"name": name, "version": release, "file": path.name, "url": match["url"], "sha256": digest})
    lock = {
        "about": "Pinned by tools/windows/agent-tools/agent_tools.py lock; see its docstring.",
        "python": python_release_file(version),
        "wheels": wheels,
    }
    check_lock(lock)
    output.write_text(json.dumps(lock, indent=1) + "\n", encoding="utf-8")
    return lock


# ---------------------------------------------------------------- fetch (network)


def fetch(cache: Path, lock: dict, opener=urllib.request.urlopen) -> list[Path]:
    """Downloads what the lock names into cache, keeping what is there already with the right SHA-256."""
    cache.mkdir(parents=True, exist_ok=True)
    found = []
    for entry in [lock["python"], *lock["wheels"]]:
        target = cache / entry["file"]
        if target.is_file() and sha256(target) == entry["sha256"]:
            found.append(target)
            continue
        partial = target.with_name(target.name + ".part")
        with opener(entry["url"], timeout=120) as response, partial.open("wb") as handle:
            shutil.copyfileobj(response, handle)
        if sha256(partial) != entry["sha256"]:
            partial.unlink()
            raise SystemExit(f"{entry['file']}: the download's SHA-256 is not the pinned one; nothing is used")
        os.replace(partial, target)
        found.append(target)
    return found


# ---------------------------------------------------------------- stage (offline)


def safe_member(name: str) -> PurePosixPath | None:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not path.parts or "\\" in name or ":" in path.parts[0]:
        return None
    return path


def unpack(archive: Path, folder: Path, place=lambda path: path) -> None:
    """The archive's files into folder (place maps a member's path to where it goes, None: left out)."""
    with zipfile.ZipFile(archive) as opened:
        for info in opened.infolist():
            if info.is_dir():
                continue
            member = safe_member(info.filename)
            if member is None:
                raise SystemExit(f"{archive.name}: {info.filename} would land outside the folder")
            relative = place(member)
            if relative is None:
                continue
            target = folder.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with opened.open(info) as source, target.open("wb") as handle:
                shutil.copyfileobj(source, handle)


def wheel_place(member: PurePosixPath) -> PurePosixPath | None:
    """A wheel's file in site-packages: <name>.data/purelib and platlib go there too; its scripts,
    headers and data are not needed."""
    if member.parts[0].endswith(".data"):
        if len(member.parts) > 2 and member.parts[1] in ("purelib", "platlib"):
            return PurePosixPath(*member.parts[2:])
        return None
    return member


def check_file(path: Path, entry: dict) -> None:
    if not path.is_file() or sha256(path) != entry["sha256"]:
        raise SystemExit(f"{entry['file']}: missing in the cache or not the pinned file; run fetch first")


def stage(cache: Path, source: Path, out: Path, lock: dict) -> dict:
    """Builds python-<id>/ and tools/ in out. Returns agent-tools.json's contents."""
    python = lock["python"]
    folder_name = f"python-{tools_id(lock)}"
    python_dir = out / folder_name
    if python_dir.exists():
        shutil.rmtree(python_dir)
    check_file(cache / python["file"], python)
    unpack(cache / python["file"], python_dir)
    tag = python_tag(python["version"])
    if not (python_dir / "python.exe").is_file() or not (python_dir / f"{tag}.zip").is_file():
        raise SystemExit(f"{python['file']} is not the embeddable package of {python['version']}")
    # The package's own ._pth leaves out site-packages and site: the wheels need both (pywin32's .pth).
    # A ._pth also puts Python in isolated mode, so no PYTHON* variable or user site of the PC's own
    # Python changes what these tools run with.
    (python_dir / f"{tag}._pth").write_text(f"{tag}.zip\n.\nLib\\site-packages\nimport site\n", encoding="utf-8")
    site = python_dir / "Lib" / "site-packages"
    site.mkdir(parents=True)
    for wheel in lock["wheels"]:
        check_file(cache / wheel["file"], wheel)
        unpack(cache / wheel["file"], site, wheel_place)
    for relative in PRUNE:
        target = site.joinpath(*relative.split("/"))
        if target.is_dir():
            shutil.rmtree(target)
        elif target.is_file():
            target.unlink()
    for cached in list(site.rglob("__pycache__")):
        shutil.rmtree(cached, ignore_errors=True)
    for folder, names in TOOL_FILES.items():
        (out / folder).mkdir(parents=True, exist_ok=True)
        for name in names:
            shutil.copy2(source / folder / name, out / folder / name)
    described = {
        "python": f"{folder_name}/python.exe",
        "pythonw": f"{folder_name}/pythonw.exe",
        "mcp_server": "tools/mcp/server.py",
        "waker": "tools/waker/thoughts_reply_waker.py",
        "python_version": python["version"],
    }
    (out / "tools" / "agent-tools.json").write_text(json.dumps(described, indent=1) + "\n", encoding="utf-8")
    for path in [python_dir, *python_dir.rglob("*"), out / "tools", *(out / "tools").rglob("*")]:
        os.utime(path, (FIXED_TIME, FIXED_TIME))
    return described


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lock", type=Path, default=LOCK)
    commands = parser.add_subparsers(dest="command", required=True)
    locking = commands.add_parser("lock")
    locking.add_argument("wheels", type=Path)
    locking.add_argument("--python-version", required=True)
    fetching = commands.add_parser("fetch")
    fetching.add_argument("cache", type=Path)
    staging = commands.add_parser("stage")
    staging.add_argument("cache", type=Path)
    staging.add_argument("source", type=Path)
    staging.add_argument("out", type=Path)
    commands.add_parser("id")
    args = parser.parse_args(argv)
    if args.command == "lock":
        lock = write_lock(args.wheels, args.python_version, args.lock)
        print(f"{args.lock}: Python {lock['python']['version']} and {len(lock['wheels'])} wheels")
    elif args.command == "fetch":
        files = fetch(args.cache, read_lock(args.lock))
        print(f"{len(files)} pinned files in {args.cache}")
    elif args.command == "stage":
        described = stage(args.cache, args.source, args.out, read_lock(args.lock))
        print(f"staged {described['python']} and the tools in {args.out}")
    else:
        print(tools_id(read_lock(args.lock)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
