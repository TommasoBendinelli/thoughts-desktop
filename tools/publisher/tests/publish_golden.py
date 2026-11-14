#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Golden test of the publish subcommand of the native thoughts-publish-sources,
talking to a fresh local thoughts-server.

Every scenario is a list of steps (local edits, edits made on Fedora by
another device, publishes, state changes). Each publish (exit code, stdout,
the THOUGHTS_SYNC_EVENT lines and the readable progress lines) and the final
state (state file, manifest, local files with their modes, and Fedora's
source tree) are compared against tools/publisher/tests/golden/publish/<scenario>.json.
Identifiers and times are normalized.

  python3 publish_golden.py --native <build>/bin/thoughts-publish-sources \\
      --server-python <thoughts-server>/.venv/bin/python --server-src <thoughts-server>/src \\
      [--only NAME] [--record] [--keep]

--server-python and --server-src run the server only. --record rewrites the
expected files from the native publisher; review the diff before committing it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from publisher_golden import Ids, configuration, project_hash, write, write_configuration  # noqa: E402

TRACE = "7e1c6a4b-0d5c-4b53-9c2d-3f2f7d6f1a11"
EMPTY = hashlib.sha256(b"").hexdigest()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    def __init__(self, args, workspace: Path):
        self.workspace = workspace
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.publisher_token = secrets.token_hex(16)
        self.phone_token = secrets.token_hex(16)
        for directory in ("state", "objects", "library", "home"):
            (workspace / directory).mkdir(parents=True, exist_ok=True)
        environment = {
            "PATH": os.environ["PATH"],
            "HOME": str(workspace / "home"),
            "PYTHONPATH": args.server_src,
            "PYTHONDONTWRITEBYTECODE": "1",
            "THOUGHTS_DATABASE_URL": f"sqlite:///{workspace / 'state/thoughts.db'}",
            "THOUGHTS_STATE_ROOT": str(workspace / "state"),
            "THOUGHTS_OBJECT_ROOT": str(workspace / "objects"),
            "THOUGHTS_DOCS_ROOT": str(workspace / "library"),
            "THOUGHTS_WORKTREE_ROOT": str(workspace / "library"),
            "THOUGHTS_API_HOST": "127.0.0.1",
            "THOUGHTS_API_PORT": str(self.port),
            "THOUGHTS_BOOTSTRAP_TOKENS": json.dumps({"publisher": self.publisher_token, "phone": self.phone_token}),
            "THOUGHTS_PROVIDER_SYNC_ENABLED": "false",
        }
        self.log = open(workspace / "server.log", "wb")
        self.process = subprocess.Popen([args.server_python, "-c", "from thoughts_server.cli import server_main; server_main()"],
                                        cwd=workspace, env=environment, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + (workspace / "server.log").read_text()[-2000:])
            try:
                self.request("GET", "/v1/sources")
                return
            except (urllib.error.URLError, ConnectionError, OSError):
                time.sleep(0.2)
        raise RuntimeError("server did not start")

    def request(self, method, path, *, payload=None, content=None, headers=None, token=None):
        headers = {"Authorization": "Bearer " + (token or self.phone_token), "Accept": "application/json", **(headers or {})}
        body = content
        if payload is not None:
            body = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.url + path, data=body, method=method, headers=headers)
        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=30) as response:
            return response.read()

    def remote_edit(self, path, content, sequence):
        """A change made on Fedora by another device (the phone)."""
        tree = json.loads(self.request("GET", "/v1/sources"))
        current = {item["path"]: item for item in tree["sources"]}.get(path)
        digest = hashlib.sha256(content).hexdigest()
        self.request("POST", "/v1/objects", content=content,
                     headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": digest})
        self.request("POST", "/v1/objects", content=b"", headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": EMPTY})
        mutation = {
            "operation_id": str(uuid.UUID(int=sequence)),
            "device_sequence": sequence,
            "path": path,
            "base_revision": current["revision"] if current else 0,
            "base_sha256": current["sha256"] if current else EMPTY,
            "base_object_sha256": current["object_sha256"] if current else EMPTY,
            "client_timestamp": "2026-10-03T00:00:00Z",
            "content_sha256": digest,
        }
        return json.loads(self.request("POST", "/v1/sources/apply", payload=mutation))

    def remote_resolve(self, path, content, sequence):
        """Another device (the phone) resolves the open conflict of path, as POST /v1/conflicts/{id}/resolve."""
        conflict = next(item for item in json.loads(self.request("GET", "/v1/conflicts?status=unresolved")) if item["path"] == path)
        digest = hashlib.sha256(content).hexdigest()
        self.request("POST", "/v1/objects", content=content,
                     headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": digest})
        resolution = {
            "operation_id": str(uuid.UUID(int=sequence)),
            "device_sequence": sequence,
            "expected_revision": conflict["expected_revision"],
            "expected_sha256": conflict["current_sha256"],
            "content_sha256": digest,
            "client_timestamp": "2026-11-09T00:00:00Z",
        }
        return json.loads(self.request("POST", f"/v1/conflicts/{conflict['id']}/resolve", payload=resolution))

    def publisher_apply(self, path, content):
        """An edit sent by another tool with the publisher's own token.

        Like a script on 2026-10-10, it takes the device's next sequence from
        Fedora, so the publisher's local counter now names an assigned sequence.
        """
        token = self.publisher_token
        tree = json.loads(self.request("GET", "/v1/sources", token=token))
        current = {item["path"]: item for item in tree["sources"]}.get(path)
        digest = hashlib.sha256(content).hexdigest()
        self.request("POST", "/v1/objects", content=content, token=token,
                     headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": digest})
        self.request("POST", "/v1/objects", content=b"", token=token,
                     headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": EMPTY})
        sequence = json.loads(self.request("GET", "/v1/device-sequence", token=token))["next_device_sequence"]
        mutation = {
            "operation_id": str(uuid.uuid4()),
            "device_sequence": sequence,
            "path": path,
            "base_revision": current["revision"] if current else 0,
            "base_sha256": current["sha256"] if current else EMPTY,
            "base_object_sha256": current["object_sha256"] if current else EMPTY,
            "client_timestamp": "2026-10-10T14:18:07.813276+00:00",
            "content_sha256": digest,
        }
        return json.loads(self.request("POST", "/v1/sources/apply", payload=mutation, token=token))

    def publish_as_publisher(self, publication):
        """Applies a publication whose answer the publisher never received."""
        return json.loads(self.request("POST", "/v1/source-publications", payload=publication, token=self.publisher_token))

    def remote_delete(self, path, sequence):
        """A deletion published on Fedora by another device."""
        current = {item["path"]: item for item in json.loads(self.request("GET", "/v1/sources"))["sources"]}[path]
        self.request("POST", "/v1/objects", content=b"", headers={"Content-Type": "text/plain; charset=utf-8", "X-Content-SHA256": EMPTY})
        mutation = {
            "action": "delete",
            "operation_id": str(uuid.UUID(int=sequence)),
            "device_sequence": sequence,
            "path": path,
            "base_revision": current["revision"],
            "base_sha256": current["sha256"],
            "base_object_sha256": current["object_sha256"],
            "client_timestamp": "2026-10-03T00:00:00Z",
        }
        return json.loads(self.request("POST", "/v1/source-publications",
                                       payload={"publication_id": str(uuid.UUID(int=sequence)), "mutations": [mutation]}))

    def remote_page(self, path, content):
        """An HTML page added on Fedora by another device (the connector's add_html_page, protocol/html-pages.md)."""
        listed = {page["path"]: page["sha256"] for page in json.loads(self.request("GET", "/v1/html-pages"))["pages"]}
        digest = hashlib.sha256(content).hexdigest()
        self.request("POST", "/v1/objects", content=content, headers={"Content-Type": "text/html", "X-Content-SHA256": digest})
        return json.loads(self.request("POST", "/v1/html-pages", payload={
            "operation_id": str(uuid.uuid5(uuid.NAMESPACE_URL, path + digest)), "folder": path.split("/")[0],
            "changes": [{"path": path, "sha256": digest, "base_sha256": listed.get(path)}]}))

    def pages(self):
        return json.loads(self.request("GET", "/v1/html-pages"))["pages"]

    def tree(self):
        tree = json.loads(self.request("GET", "/v1/sources"))
        return [{key: item.get(key) for key in ("path", "revision", "sha256", "deleted")} for item in tree["sources"]]

    def stop(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
        self.log.close()


UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
TIME_RE = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})?")


class Normalizer:
    """Replaces UUIDs (by first appearance) and timestamps."""

    def __init__(self):
        self.ids = {TRACE: "TRACE"}

    def __call__(self, text: str) -> str:
        def identifier(match):
            value = match.group(0)
            if value not in self.ids:
                self.ids[value] = f"ID{len(self.ids)}"
            return self.ids[value]
        text = UUID_RE.sub(identifier, text)
        text = TIME_RE.sub("TIME", text)
        return re.sub(r"^\[[^\]]*\] ", "[STAMP] ", text, flags=re.M)


IGNORE_MODES = False


def snapshot_files(directory: Path):
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_file() and not path.is_symlink():
            files[path.relative_to(directory).as_posix()] = (hashlib.sha256(path.read_bytes()).hexdigest()[:16],
                                                              None if IGNORE_MODES else oct(stat.S_IMODE(path.stat().st_mode)))
    return files


# ---------------------------------------------------------------- scenarios

SCENARIOS = {}


def scenario(name):
    def register(function):
        SCENARIOS[name] = function
        return function
    return register


def project(ids, root, owner="docs-paper", sources=(("tex/source/main.tex", "latex"),), rendering=None):
    value = configuration(ids, "new:" + owner, {owner: [(source, kind, True) for source, kind in sources]}, rendering=rendering)
    write_configuration(root, value)
    return value


PUBLISH = ["--root", "docs", "--manifest", "state/source-manifest.json", "--state", "state/source-publication.json",
           "--bootstrap-state", "state/source-sync-state.json"]


def publish(*extra, env=None):
    return ("publish", [*PUBLISH, *extra], env or {})


@scenario("first-publication")
def _(ids):
    def setup(d):
        root = d / "docs"
        project(ids, root, sources=(("tex/source/main.tex", "latex"), ("markdown/source/notes.md", "markdown")))
        write(root / "docs-paper/tex/source/main.tex", "\\documentclass{article}\n")
        write(root / "docs-paper/markdown/source/notes.md", "# Notes \u00e9\n")
        write(root / "docs-paper/tex/support/s.sty", "% sty\n")
    return [("setup", setup), publish(), publish(), ("write", "docs-paper/tex/source/main.tex", "changed\n"), publish(),
            ("delete", "docs-paper/tex/support/s.sty"), publish(), publish()]


@scenario("remote-fast-forward")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "one\n")
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "two from phone\n"), publish(), publish()]


@scenario("both-changed-merge")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "a\nb\nc\nd\ne\n")
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "A\nb\nc\nd\ne\n"),
            ("write", "docs-paper/tex/source/main.tex", "a\nb\nc\nd\nE\n"), publish(), publish()]


@scenario("both-changed-conflict")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "a\n")
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "phone\n"),
            ("write", "docs-paper/tex/source/main.tex", "mac\n"), publish(), publish()]


def _superseded_conflict(ids, settle):
    # 2026-11-09: a resolution that met a newer Fedora version came back as a
    # new conflict, and Fedora marked the recorded one "superseded", which
    # never resolves. The record follows the successor, and once that is
    # resolved the Mac's file (still its edit from the first conflict, not the
    # successor's incoming merge) takes Fedora's version.
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "a\nb\nc\n")
    path = "docs-paper/tex/source/main.tex"
    frozen = ["--manifest", "state/frozen/c/source-manifest.json", "--frozen-inputs", "state/frozen/c/inputs.json"]
    return [("setup", setup), publish(), ("remote", path, "a\nphone\nc\n"), ("write", path, "a\nmac\nc\n"),
            ("call", frozen_bundle("c", [path])), ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}),
            ("remote", path, "a\nphone again\nc\n"), ("remote_resolve", path, "a\nmerged\nc\n", "conflict"), settle,
            ("remote_resolve", path, "a\nfinal\nc\n", "applied"), settle, publish()]


@scenario("superseded-conflict-followed")
def _(ids):
    return _superseded_conflict(ids, publish())


@scenario("reconcile-superseded-conflict")
def _(ids):
    return _superseded_conflict(ids, ("reconcile", 0))


@scenario("no-base-differs")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "local\n")
    return [("setup", setup), ("remote", "docs-paper/tex/source/main.tex", "remote first\n"), publish()]


@scenario("delete-after-remote-change")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/tex/source/other.tex", "y\n")
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/other.tex", "phone\n"),
            ("delete", "docs-paper/tex/source/other.tex"), publish()]


@scenario("html-pages")
def _(ids):
    # A folder's HTML pages (protocol/html-pages.md 4): a page made on this computer joins Fedora's list with the
    # next publication; a page another device adds on Fedora is installed by the source installer.
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "\\documentclass{article}\n")
        write(d / "docs/docs-paper/tex/support/html/local.html", "<!doctype html><p>made on the Mac</p>\n")
    return [("setup", setup), publish(), ("remote_page", "docs-paper/tex/support/html/agent.html", "<!doctype html><p>made by the agent</p>\n"),
            ("reconcile", 0), publish(),
            ("write", "docs-paper/tex/support/html/local.html", "<!doctype html><p>changed on the Mac</p>\n"), publish()]


@scenario("target-build")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "\\PdfLink{ext}\n")
        write(d / "docs/docs-paper/tex/source/fig.png", b"\x89PNG")
        write(d / "docs/docs-paper/tex/generated/plot.pdf", b"%PDF-1")
        write(d / "docs/third_party/ext.pdf", b"%PDF-ext")
    return [("setup", setup), publish("--target", "paper_main.pdf", "--force-build", "--project", "docs-paper"),
            publish("--target", "paper_main.pdf", "--force-build"), publish("--target", "paper_main.pdf"),
            publish("--scope-target", "paper_main.pdf"), publish()]


@scenario("cli-errors")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
    return [("setup", setup), publish("--project", "docs-paper"), publish("--target", "a.pdf", "--scope-target", "b.pdf"),
            publish("--force-build"), publish(env={"THOUGHTS_TOKEN": ""}), publish("--target", "nothing.pdf"),
            publish("--target", "a/b.pdf", "--force-build"), publish("--scope-target", "paper_main.pdf", "--project", "docs-x"),
            publish("--path", "docs-paper/tex/source/missing.tex"), publish("--path", "../x"),
            publish("--frozen-inputs", "missing/inputs.json"), publish("--adopt-remote-base", "docs-paper/tex/source/main.tex")]


@scenario("state-files")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "state/source-sync-state.json", json.dumps({"schema_version": 1, "next_device_sequence": "7",
                                                             "files": {"docs-paper/tex/source/main.tex": {"revision": 3, "sha256": "0" * 64, "object_sha256": "0" * 64},
                                                                       "bad": "x"}}))
    def bad_state(d):
        write(d / "state/source-publication.json", '{"schema_version": 2}')
    def foreign_keys(d):
        path = d / "state/source-publication.json"
        value = json.loads(path.read_text())
        value["materialized_sources"] = {"x": {"revision": 1.0, "big": 10 ** 22}}
        value["next_device_sequence"] = 40
        path.write_text(json.dumps(value))
    return [("setup", setup), publish(), ("call", foreign_keys), publish(), ("call", bad_state), publish()]


@scenario("adopt-remote-base")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "same\n")
    return [("setup", setup), ("remote", "docs-paper/tex/source/main.tex", "same\n"),
            publish("--adopt-remote-base", "docs-paper/tex/source/main.tex"), publish()]


@scenario("retired-paths")
def _(ids):
    def setup(d):
        project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("markdown/source/m.md", "markdown")))
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/markdown/source/m.md", "m\n")
        write(d / "docs/docs-paper/notes.md", "n\n")
    def ignore(d):
        write(d / "docs/docs-paper/.ThoughtIgnore", "notes.md\n.git/\n")
    return [("setup", setup), publish(), ("call", ignore), publish(), publish("--path", "docs-paper/notes.md")]


def frozen_bundle(name, sources, targets=None, base_from_state=True, bases_behind=None):
    """A desktop-style frozen bundle of the given local paths.

    bases_behind names, by path, the base Desktop's own record still holds
    when it has not caught up with the publisher's state.
    """
    def build(d):
        bundle = d / "state/frozen" / name
        objects = bundle / "objects"
        objects.mkdir(parents=True, exist_ok=True)
        state_path = d / "state/source-publication.json"
        bases = json.loads(state_path.read_text()).get("bases", {}) if state_path.is_file() else {}
        entries = []
        for path in sources:
            local = d / "docs" / path
            base = bases.get(path, {}).get("sha256", EMPTY) if base_from_state else EMPTY
            if bases_behind and path in bases_behind:
                base = bases_behind[path]
            if local.is_file():
                data = local.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                (objects / digest).write_bytes(data)
                entries.append({"path": path, "action": "upsert", "sha256": digest, "size_bytes": len(data), "base_sha256": base})
            else:
                entries.append({"path": path, "action": "delete", "base_sha256": base})
        build_inputs = {}
        for target, items in (targets or {}).items():
            build_inputs[target] = []
            for path, role, media in items:
                data = (d / "docs" / path).read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                (objects / digest).write_bytes(data)
                build_inputs[target].append({"path": path, "action": "upsert", "sha256": digest, "size_bytes": len(data),
                                             "media_type": media, "role": role})
        (bundle / "inputs.json").write_text(json.dumps({"schema_version": 2, "source_root": str((d / "docs").resolve()),
                                                        "objects_dir": "objects", "sources": entries, "build_inputs": build_inputs}))
        shutil.copy(d / "state/source-manifest.json", bundle / "source-manifest.json")
    return build


@scenario("frozen-global")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/tex/source/b.tex", "b\n")
    frozen = ["--manifest", "state/frozen/one/source-manifest.json", "--frozen-inputs", "state/frozen/one/inputs.json"]
    return [("setup", setup), publish(), ("write", "docs-paper/tex/source/main.tex", "frozen edit\n"),
            ("call", frozen_bundle("one", ["docs-paper/tex/source/main.tex", "docs-paper/tex/source/b.tex"])),
            ("write", "docs-paper/tex/source/main.tex", "later live edit\n"),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})]


@scenario("frozen-scoped-build-inputs")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/tex/generated/plot.pdf", b"%PDF")
    frozen = ["--manifest", "state/frozen/s/source-manifest.json", "--frozen-inputs", "state/frozen/s/inputs.json",
              "--scope-target", "paper_main.pdf"]
    return [("setup", setup), publish(), ("write", "docs-paper/tex/source/main.tex", "scoped\n"),
            ("call", frozen_bundle("s", ["docs-paper/tex/source/main.tex"],
                                   {"paper_main.pdf": [("docs-paper/tex/generated/plot.pdf", "generated", "application/pdf")]})),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})]


@scenario("frozen-phone-fast-forward")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "base\n")
    frozen = ["--manifest", "state/frozen/f/source-manifest.json", "--frozen-inputs", "state/frozen/f/inputs.json"]
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "phone edit\n"),
            ("call", frozen_bundle("f", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})]


@scenario("frozen-three-way-conflict")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "base\n")
    frozen = ["--manifest", "state/frozen/c/source-manifest.json", "--frozen-inputs", "state/frozen/c/inputs.json"]
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "phone edit\n"),
            ("write", "docs-paper/tex/source/main.tex", "mac edit\n"),
            ("call", frozen_bundle("c", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}), publish()]


@scenario("frozen-fedora-merges-adjacent")
def _(ids):
    # Edits on adjacent lines conflict in git merge-file, so the frozen source
    # goes to Fedora alone, and Fedora's merge joins them. Its answer "merged"
    # is installed and becomes the base, and the next Syncs send nothing
    # (2026-11-13: it was refused as inconsistent and sent again every Sync).
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "a\nb\nc\n")
    frozen = ["--manifest", "state/frozen/m/source-manifest.json", "--frozen-inputs", "state/frozen/m/inputs.json"]
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "A\nb\nc\n"),
            ("write", "docs-paper/tex/source/main.tex", "a\nB\nc\n"),
            ("call", frozen_bundle("m", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}), publish(),
            ("call", frozen_bundle("m", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})]


def _desktop_base_behind(ids, edit_after_install, remote_later=None, local_before=None):
    # 13 Nov: the installer moved the publisher's base to Fedora's revision (an
    # install, or a merge with a local edit) and Desktop kept its older base: it
    # could not record the new one (database is locked) or the file changed
    # before it could. Every frozen Sync was then refused with "frozen Desktop
    # source has no matching authoring or Fedora base".
    path = "docs-paper/tex/source/main.tex"
    first = "a\nb\nc\nd\ne\nf\ng\nh\n"
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs" / path, first)
    behind = {path: hashlib.sha256(first.encode()).hexdigest()}
    frozen = ["--manifest", "state/frozen/b/source-manifest.json", "--frozen-inputs", "state/frozen/b/inputs.json"]
    steps = [("setup", setup), publish(), ("remote", path, "A\nb\nc\nd\ne\nf\ng\nh\n")]
    if local_before:
        steps.append(("write", path, local_before))
    steps += [("reconcile", 0), ("write", path, edit_after_install)]
    if remote_later:
        steps.append(("remote", path, remote_later))
    return [*steps, ("call", frozen_bundle("b", [path], bases_behind=behind)),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}), publish()]


@scenario("frozen-desktop-base-behind-merge")
def _(ids):
    # Merged with a local edit, then edited again (an agent's proposal) while
    # Fedora moved on: the file holds the merge, so it is published against
    # the new base and Fedora's later change merges in.
    return _desktop_base_behind(ids, "A\nb\nc\nD\ne\nf\ng\nH\n", remote_later="A\nb\nc\nd\ne\nF\ng\nh\n",
                                local_before="a\nb\nc\nd\ne\nf\ng\nH\n")


@scenario("frozen-desktop-base-behind-install")
def _(ids):
    # Installed, Desktop did not record it, then edited.
    return _desktop_base_behind(ids, "A\nb\nc\nD\ne\nf\ng\nh\n")


@scenario("frozen-desktop-base-behind-stale-copy")
def _(ids):
    # An editor saved its copy from before the merge over it: Fedora's change
    # ("A") is missing from the file, so it is merged against Desktop's older
    # base and Fedora keeps the change.
    return _desktop_base_behind(ids, "a\nb\nc\nD\ne\nf\ng\nH\n", local_before="a\nb\nc\nd\ne\nf\ng\nH\n")


@scenario("frozen-errors")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
    def corrupt(d):
        bundle = d / "state/frozen/e"
        value = json.loads((bundle / "inputs.json").read_text())
        value["sources"][0]["sha256"] = "f" * 64
        (bundle / "inputs.json").write_text(json.dumps(value))
    def duplicate(d):
        bundle = d / "state/frozen/e"
        text = (bundle / "inputs.json").read_text()
        (bundle / "inputs.json").write_text(text[:-1] + ', "objects_dir": "objects"}')
    frozen = ["--manifest", "state/frozen/e/source-manifest.json", "--frozen-inputs", "state/frozen/e/inputs.json"]
    step = ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})
    return [("setup", setup), publish(), ("call", frozen_bundle("e", ["docs-paper/tex/source/main.tex", "docs-paper/unmanaged.tex"])),
            step, ("call", frozen_bundle("e", ["docs-paper/tex/source/main.tex"])), ("call", corrupt), step,
            ("call", frozen_bundle("e", ["docs-paper/tex/source/main.tex"])), ("call", duplicate), step]


@scenario("pending-retry")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
    def pending(d):
        path = d / "state/source-publication.json"
        state = json.loads(path.read_text())
        data = b"pending\n"
        state["pending_publication"] = {"publication_id": "00000000-0000-4000-8000-000000000001", "mutations": [{
            "action": "upsert", "operation_id": "00000000-0000-4000-8000-000000000002", "device_sequence": 90,
            "path": "docs-paper/tex/source/extra.tex", "base_revision": 0, "base_sha256": EMPTY, "base_object_sha256": EMPTY,
            "client_timestamp": "2026-10-03T00:00:00Z", "content_sha256": hashlib.sha256(data).hexdigest()}]}
        path.write_text(json.dumps(state))
        write(d / "docs/docs-paper/tex/source/extra.tex", data)
    def upload(d):
        pass
    return [("setup", setup), publish(), ("call", pending), ("upload", "pending\n"), publish(), publish()]


def _pending_at_next_sequence(d, data=b"pending\n"):
    """Stores a pending publication that uses the publisher's next sequence."""
    path = d / "state/source-publication.json"
    state = json.loads(path.read_text())
    sequence = state["next_device_sequence"]
    state["next_device_sequence"] = sequence + 1
    state["pending_publication"] = {"publication_id": "00000000-0000-4000-8000-000000000001", "mutations": [{
        "action": "upsert", "operation_id": "00000000-0000-4000-8000-000000000002", "device_sequence": sequence,
        "path": "docs-paper/tex/source/extra.tex", "base_revision": 0, "base_sha256": EMPTY, "base_object_sha256": EMPTY,
        "client_timestamp": "2026-10-03T00:00:00Z", "content_sha256": hashlib.sha256(data).hexdigest()}]}
    path.write_text(json.dumps(state))
    write(d / "docs/docs-paper/tex/source/extra.tex", data)


@scenario("foreign-sequence-collision")
def _(ids):
    # Another tool used the publisher's token and took its next sequence from
    # Fedora. The publisher renumbers the refused publication once.
    def setup(d):
        project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("tex/source/other.tex", "latex")))
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/tex/source/other.tex", "o\n")
    return [("setup", setup), publish(), ("publisher_apply", "docs-paper/tex/source/other.tex", "tool\n"),
            ("write", "docs-paper/tex/source/main.tex", "changed\n"), publish(), publish()]


@scenario("pending-sequence-collision")
def _(ids):
    # The incident of 2026-10-10: the stored pending publication's sequence was
    # taken by another writer, and an identical retry was refused forever.
    def setup(d):
        project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("tex/source/other.tex", "latex")))
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
        write(d / "docs/docs-paper/tex/source/other.tex", "o\n")
    return [("setup", setup), publish(), ("call", _pending_at_next_sequence), ("upload", "pending\n"),
            ("publisher_apply", "docs-paper/tex/source/other.tex", "tool\n"), publish(), publish()]


@scenario("frozen-conflict-sequence-collision")
def _(ids):
    # A preserved conflict is sent alone through /v1/sources/apply; its sequence
    # was taken by another writer, so it is renumbered before Fedora records it.
    def setup(d):
        project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("tex/source/other.tex", "latex")))
        write(d / "docs/docs-paper/tex/source/main.tex", "base\n")
        write(d / "docs/docs-paper/tex/source/other.tex", "o\n")
    frozen = ["--manifest", "state/frozen/c/source-manifest.json", "--frozen-inputs", "state/frozen/c/inputs.json"]
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "phone edit\n"),
            ("publisher_apply", "docs-paper/tex/source/other.tex", "tool\n"),
            ("write", "docs-paper/tex/source/main.tex", "mac edit\n"),
            ("call", frozen_bundle("c", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {})]


@scenario("pending-lost-acknowledgement")
def _(ids):
    # Fedora applied the pending publication but the publisher lost the answer:
    # the identical retry is answered from Fedora's record, never renumbered.
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "x\n")
    return [("setup", setup), publish(), ("call", _pending_at_next_sequence), ("upload", "pending\n"),
            ("publish_pending_as_publisher",), publish(), publish()]


@scenario("reconcile-install")
def _(ids):
    def setup(d):
        project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("markdown/source/m.md", "markdown")))
        write(d / "docs/docs-paper/tex/source/main.tex", "a\nb\nc\nd\ne\n")
        write(d / "docs/docs-paper/markdown/source/m.md", "m\n")
        write(d / "docs/docs-paper/tex/support/s.sty", "s\n")
    return [("setup", setup), publish(), ("reconcile", 0), ("reconcile", 0),
            ("remote", "docs-paper/tex/source/main.tex", "A\nb\nc\nd\ne\n"), ("reconcile", 0),
            ("remote", "docs-paper/tex/source/main.tex", "A\nb\nc\nd\nX\n"), ("write", "docs-paper/tex/source/main.tex", "Y\nb\nc\nd\ne\n"),
            ("reconcile", 0), ("remote", "docs-paper/markdown/source/m.md", "phone\n"), ("write", "docs-paper/markdown/source/m.md", "mac\n"),
            ("reconcile", 0), ("remote", "docs-paper/tex/source/new.tex", "added elsewhere\n"),
            ("remote", "docs-paper/tex/source/.hidden/x.tex", "hidden\n"), ("remote", "docs-other/tex/source/z.tex", "other folder\n"),
            ("write", "docs-paper/tex/support/s.sty", "local only\n"), ("reconcile", 0), ("reconcile", 5), ("reconcile", 0, {"THOUGHTS_TOKEN": ""})]


def _central_project_file(ids, d):
    value = project(ids, d / "docs")
    return value, d / "docs/.thoughts/projects" / project_hash(value["id"]) / "documents/thoughts-project.json"


def _project_manifest_deletion(ids, change):
    # Fedora deletes a Project file the desktop has rewritten locally.
    configured = {}
    def setup(d):
        configured["value"], configured["path"] = _central_project_file(ids, d)
        write(d / "docs/docs-paper/tex/source/main.tex", "base\n")
    def rewrite(d):
        value = json.loads(configured["path"].read_text())
        change(value)
        configured["path"].write_text(json.dumps(value, indent=4, sort_keys=True))
    def relative(d):
        return configured["path"].relative_to(d / "docs").as_posix()
    return [("setup", setup), publish(), ("call", rewrite), ("remote_delete", relative), ("reconcile", 0), ("reconcile", 0)]


def _project_documents_changed_on_both_sides(ids, remote_change, local_change):
    # Fedora and this computer both edit the Project file's documents.
    configured = {}
    def setup(d):
        configured["value"] = project(ids, d / "docs", sources=(("tex/source/main.tex", "latex"), ("tex/source/b.tex", "latex")))
        configured["path"] = d / "docs/.thoughts/projects" / project_hash(configured["value"]["id"]) / "documents/thoughts-project.json"
        configured["pdf"] = {"document_id": ids(), "format": "pdf", "included": True,
                             "source": {"folder_id": configured["value"]["default_folder_id"], "path": "submitted.pdf"}}
        write(d / "docs/docs-paper/tex/source/main.tex", "main\n")
        write(d / "docs/docs-paper/tex/source/b.tex", "b\n")
    def changed(change):
        value = json.loads(json.dumps(configured["value"]))
        change(value, configured["pdf"])
        return value
    def remote(d):
        return json.dumps(changed(remote_change), ensure_ascii=False, indent=2) + "\n"
    def local(d):
        configured["path"].write_text(json.dumps(changed(local_change), indent=4, sort_keys=True))
    def relative(d):
        return configured["path"].relative_to(d / "docs").as_posix()
    return [("setup", setup), publish(), ("remote", relative, remote), ("call", local), ("reconcile", 0), ("reconcile", 0), publish()]


def _add_pdf(position):
    def change(value, pdf):
        value["documents"].insert(len(value["documents"]) if position is None else position, pdf)
        value.setdefault("rendering", {})[pdf["document_id"]] = {"sha256": "a" * 64}
        value.setdefault("document_names", {})[pdf["document_id"]] = "submitted"
    return change


@scenario("frozen-desktop-base-behind-project-file")
def _(ids):
    # The 13 Nov Project file: the installer merged Fedora's rename with a PDF
    # added here and Desktop could not record it (database is locked); the
    # next change to the file was refused until the publisher merged it.
    configured = {}
    behind = {}
    def setup(d):
        configured["value"] = project(ids, d / "docs")
        configured["path"] = d / "docs/.thoughts/projects" / project_hash(configured["value"]["id"]) / "documents/thoughts-project.json"
        configured["pdf"] = {"document_id": ids(), "format": "pdf", "included": True,
                             "source": {"folder_id": configured["value"]["default_folder_id"], "path": "submitted.pdf"}}
        write(d / "docs/docs-paper/tex/source/main.tex", "main\n")
    def relative(d):
        return configured["path"].relative_to(d / "docs").as_posix()
    def remember(d):
        behind[relative(d)] = hashlib.sha256(configured["path"].read_bytes()).hexdigest()
    def remote(d):
        value = json.loads(json.dumps(configured["value"]))
        value["name"] = "Renamed on Fedora"
        return json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    def add(d):
        value = json.loads(configured["path"].read_text())
        _add_pdf(None)(value, configured["pdf"])
        configured["path"].write_text(json.dumps(value, indent=4, sort_keys=True))
    def rename_pdf(d):
        value = json.loads(configured["path"].read_text())
        value["document_names"][configured["pdf"]["document_id"]] = "submitted, revised"
        configured["path"].write_text(json.dumps(value, indent=4, sort_keys=True))
    def bundle(d):
        frozen_bundle("p", [relative(d)], bases_behind=behind)(d)
    frozen = ["--manifest", "state/frozen/p/source-manifest.json", "--frozen-inputs", "state/frozen/p/inputs.json"]
    return [("setup", setup), publish(), ("call", remember), ("remote", relative, remote), ("call", add), ("reconcile", 0),
            ("call", rename_pdf), ("call", bundle), ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}), publish()]


@scenario("reconcile-project-same-document-added")
def _(ids):
    # Both added the same PDF; this computer placed it first. No conflict, this computer's order.
    return _project_documents_changed_on_both_sides(ids, _add_pdf(None), _add_pdf(0))


@scenario("reconcile-project-added-while-reordered")
def _(ids):
    # Fedora added a PDF while this computer reordered the documents: both changes survive.
    return _project_documents_changed_on_both_sides(ids, _add_pdf(None), lambda value, pdf: value["documents"].reverse())


@scenario("reconcile-project-renamed-on-both-sides")
def _(ids):
    # Both renamed the Project differently: still a conflict.
    return _project_documents_changed_on_both_sides(ids, lambda value, pdf: value.update(name="renamed on Fedora"),
                                                    lambda value, pdf: value.update(name="renamed here"))


@scenario("reconcile-layout-only-project")
def _(ids):
    # Only Qt's layout changed, so the deletion wins instead of blocking every Sync.
    return _project_manifest_deletion(ids, lambda value: None)


@scenario("reconcile-edited-project-deleted")
def _(ids):
    # A real local edit still waits for publication against the deletion.
    return _project_manifest_deletion(ids, lambda value: value.update(name="edited on the Mac"))


@scenario("reconcile-conflicts")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "base\n")
    frozen = ["--manifest", "state/frozen/c/source-manifest.json", "--frozen-inputs", "state/frozen/c/inputs.json"]
    return [("setup", setup), publish(), ("remote", "docs-paper/tex/source/main.tex", "phone edit\n"),
            ("write", "docs-paper/tex/source/main.tex", "mac edit\n"),
            ("call", frozen_bundle("c", ["docs-paper/tex/source/main.tex"])),
            ("publish", [*PUBLISH[:2], *frozen, *PUBLISH[4:]], {}), ("reconcile", 0), ("reconcile", 0)]


@scenario("reconcile-missing-base")
def _(ids):
    def setup(d):
        project(ids, d / "docs")
        write(d / "docs/docs-paper/tex/source/main.tex", "local\n")
        write(d / "docs/docs-paper/tex/source/second.tex", "second\n")
        write(d / "state/source-manifest.json", "")
    def manifest(d):
        subprocess.run([sys.executable, "-c", "pass"])
    return [("setup", setup), ("remote", "docs-paper/tex/source/main.tex", "remote first\n"),
            ("remote", "docs-paper/tex/source/second.tex", "second\n"), publish("--path", "docs-paper/tex/source/second.tex"),
            ("reconcile", 0)]


def _unbased_project_file(ids, remote_change, frozen):
    # Fedora has a later revision of a Project file this computer has no base
    # for (a Project created here whose folder was moved in, 2026-10-12): a
    # frozen global publication or the installer meets it.
    configured = {}
    def setup(d):
        configured["value"], configured["path"] = _central_project_file(ids, d)
        configured["path"].write_text(json.dumps(configured["value"], indent=4, sort_keys=True))
        write(d / "docs/docs-paper/tex/source/main.tex", "main\n")
    def relative(d):
        return configured["path"].relative_to(d / "docs").as_posix()
    def remote(d):
        value = json.loads(json.dumps(configured["value"]))
        remote_change(value)
        return json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    steps = [("setup", setup), ("remote", relative, remote), publish("--path", "docs-paper/tex/source/main.tex")]
    if frozen:
        arguments = ["--manifest", "state/frozen/p/source-manifest.json", "--frozen-inputs", "state/frozen/p/inputs.json"]
        def bundle(d):
            frozen_bundle("p", [relative(d)])(d)
        return [*steps, ("call", bundle), ("publish", [*PUBLISH[:2], *arguments, *PUBLISH[4:]], {}), publish()]
    return [*steps, ("reconcile", 0), ("reconcile", 0), publish()]


@scenario("frozen-unbased-project-differs")
def _(ids):
    # This computer's older copy must not replace Fedora's: a conflict, not an upload.
    return _unbased_project_file(ids, lambda value: value.update(name="moved in on Fedora"), True)


@scenario("frozen-unbased-project-equivalent")
def _(ids):
    # Only the layout differs: Fedora's bytes are installed and become the base.
    return _unbased_project_file(ids, lambda value: None, True)


@scenario("reconcile-unbased-project-equivalent")
def _(ids):
    return _unbased_project_file(ids, lambda value: None, False)


@scenario("reconcile-unbased-project-differs")
def _(ids):
    return _unbased_project_file(ids, lambda value: value.update(name="moved in on Fedora"), False)


# ---------------------------------------------------------------- running

def run_scenario(name, build, args, base):
    workspace = base
    workspace.mkdir(parents=True)
    server = Server(args, workspace / "server")
    normalize = Normalizer()
    results = []
    try:
        directory = workspace / "client"
        (directory / "state").mkdir(parents=True)
        (directory / "docs").mkdir()
        sequence = 1
        for step in build(Ids(11)):
            kind = step[0]
            if kind == "setup" or kind == "call":
                step[1](directory)
            elif kind == "write":
                write(directory / "docs" / step[1], step[2])
            elif kind == "delete":
                (directory / "docs" / step[1]).unlink()
            elif kind == "remote":
                sequence += 1
                path = step[1](directory) if callable(step[1]) else step[1]
                content = step[2](directory) if callable(step[2]) else step[2]
                server.remote_edit(path, content.encode(), sequence)
            elif kind == "remote_resolve":
                sequence += 1
                outcome = server.remote_resolve(step[1], step[2].encode(), sequence)["outcome"]
                if outcome != step[3]:
                    raise RuntimeError(f"the phone's resolution of {step[1]} answered {outcome}, not {step[3]}")
            elif kind == "remote_page":
                server.remote_page(step[1], step[2].encode())
            elif kind == "remote_delete":
                sequence += 1
                server.remote_delete(step[1](directory) if callable(step[1]) else step[1], sequence)
            elif kind == "publisher_apply":
                server.publisher_apply(step[1], step[2].encode())
            elif kind == "publish_pending_as_publisher":
                pending = json.loads((directory / "state/source-publication.json").read_text())["pending_publication"]
                server.publish_as_publisher(pending)
            elif kind == "upload":
                data = step[1].encode()
                server.request("POST", "/v1/objects", content=data, headers={"Content-Type": "text/plain; charset=utf-8",
                                                                             "X-Content-SHA256": hashlib.sha256(data).hexdigest()})
            elif kind == "reconcile":
                frontier = json.loads(server.request("GET", "/v1/sources"))["source_frontier"]
                through = str(frontier + step[1])
                arguments = ["--root", "docs", "--manifest", "state/source-manifest.json", "--state", "state/source-publication.json",
                             "--bootstrap-state", "state/source-sync-state.json", "--server", server.url, "--through", through]
                environment = dict(os.environ, THOUGHTS_TOKEN=server.publisher_token, THOUGHTS_TRACE_ID=TRACE, HOME=str(workspace / "home"))
                environment.update(step[2] if len(step) > 2 else {})
                command = [args.native, "-m", "thoughts_server.source_reconciliation"]
                result = subprocess.run([*command, *arguments], cwd=directory, capture_output=True, env=environment, timeout=300)
                stderr = result.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
                lines = [line for line in stderr.splitlines() if not (args.windows and "MoltenVK" in line)]
                stdout = re.sub(r'"duration_ms": \d+', '"duration_ms": N', result.stdout.decode())
                stdout = re.sub(r'-\d+\.json"', '-NS.json"', stdout).replace(str(directory), "<ROOT>")
                if args.windows:
                    # Python on Windows also prints str(Path) with backslashes, and print() writes CRLF.
                    stdout = stdout.replace("\r\n", "\n").replace('state\\\\source-reconciliation-reports\\\\', "state/source-reconciliation-reports/")
                results.append(("reconcile", result.returncode, normalize(stdout), normalize("\n".join(lines))))
            elif kind == "publish":
                arguments = [*step[1], "--server", server.url]
                environment = dict(os.environ, THOUGHTS_TOKEN=server.publisher_token, THOUGHTS_TRACE_ID=TRACE, TZ="Europe/Zurich",
                                   HOME=str(workspace / "home"))
                environment.update(step[2])
                for variable in ("COLUMNS", "FORCE_COLOR", "NO_COLOR"):
                    environment.pop(variable, None)
                command = [args.native]
                result = subprocess.run([*command, *arguments], cwd=directory, capture_output=True, env=environment, timeout=300)
                stderr = result.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
                lines = [line for line in stderr.splitlines() if not (args.windows and "MoltenVK" in line)]
                results.append(("publish " + " ".join(step[1][8:]), result.returncode, normalize(result.stdout.decode().replace("\r\n", "\n")),
                                normalize("\n".join(lines).replace(str(directory.resolve()), "<ROOT>").replace(str(directory), "<ROOT>"))))
        state_path = directory / "state/source-publication.json"
        final = {
            "state": normalize(state_path.read_text()) if state_path.is_file() else None,
            "manifest": (directory / "state/source-manifest.json").read_text() if (directory / "state/source-manifest.json").is_file() else None,
            "docs": snapshot_files(directory / "docs"),
            # A resolved conflict's preserved copy is named by its conflict ID.
            "state_files": {normalize(k): v for k, v in snapshot_files(directory / "state").items()
                            if not k.startswith("frozen/") and not k.endswith(".lock") and k != "source-publication.json"
                            and not k.startswith("source-reconciliation-reports/") and k != "authoring-reconciliation.json"
                            # The server's source tree, kept between runs: a copy of "tree" below.
                            and k != "source-tree-cache.json"},
            "reports": sorted(normalize(re.sub(r'"duration_ms": \d+', '"duration_ms": N',
                                               re.sub(r'-\d+\.json"', '-NS.json"', p.read_text())).replace(str(directory), "<ROOT>")
                                        .replace('state\\\\source-reconciliation-reports\\\\', "state/source-reconciliation-reports/"))
                              for p in (directory / "state").glob("source-reconciliation-reports/*.json")),
            "tree": server.tree(),
        }
        if name.startswith("html-pages"):
            final["html_pages"] = server.pages()
    finally:
        server.stop()
    return results, final


GOLDEN = Path(__file__).resolve().parent / "golden" / "publish"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--server-python", required=True, help="interpreter with the server's dependencies")
    parser.add_argument("--server-src", required=True, help="thoughts_server package the local server runs")
    parser.add_argument("--only")
    parser.add_argument("--record", action="store_true", help="rewrite the expected outcomes from the native publisher")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--windows", action="store_true",
                        help="the native side is a Windows build (e.g. under Wine): ignore POSIX file modes and Wine's own log lines")
    args = parser.parse_args()
    args.native = os.path.abspath(args.native)
    global IGNORE_MODES
    IGNORE_MODES = args.windows
    work = Path(tempfile.mkdtemp(prefix="publish-golden-")).resolve()
    failures = 0
    selected = {k: v for k, v in SCENARIOS.items() if not args.only or re.fullmatch(args.only, k)}
    for name, build in selected.items():
        base = work / name
        steps, final = run_scenario(name, build, args, base)
        # One JSON shape for recorded and observed outcomes (tuples become lists).
        got = json.loads(json.dumps({"steps": steps, "final": final}))
        expected_path = GOLDEN / f"{name}.json"
        problems = []
        if args.record:
            GOLDEN.mkdir(parents=True, exist_ok=True)
            expected_path.write_text(json.dumps(got, indent=1, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
        elif not expected_path.is_file():
            problems.append(f"no expected outcome {expected_path}; run with --record")
        else:
            want = json.loads(expected_path.read_text(encoding="utf-8"))
            if len(want["steps"]) != len(got["steps"]):
                problems.append(f"{len(got['steps'])} steps, expected {len(want['steps'])}")
            for expected, actual in zip(want["steps"], got["steps"]):
                label = expected[0]
                if expected[1] != actual[1]:
                    problems.append(f"{label}: exit {expected[1]} != {actual[1]}")
                if expected[2] != actual[2]:
                    problems.append(f"{label}: stdout\n    expected: {expected[2][:600]!r}\n    native:   {actual[2][:600]!r}")
                if expected[3] != actual[3]:
                    left, right = expected[3].splitlines(), actual[3].splitlines()
                    for index in range(max(len(left), len(right))):
                        a = left[index] if index < len(left) else None
                        b = right[index] if index < len(right) else None
                        if a != b:
                            problems.append(f"{label}: stderr line {index}\n    expected: {a!r}\n    native:   {b!r}")
                            break
            for key in want["final"]:
                if want["final"][key] != got["final"].get(key):
                    problems.append(f"final {key}\n    expected: {str(want['final'][key])[:1500]}\n    native:   {str(got['final'].get(key))[:1500]}")
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for problem in problems[:8]:
                print("  " + problem)
        else:
            print(f"ok   {name} ({len(steps)} publishes)")
            if not args.keep:
                shutil.rmtree(base, ignore_errors=True)
    if args.record and not args.only:
        for path in GOLDEN.glob("*.json"):
            if path.stem not in SCENARIOS:
                path.unlink()
    print(f"{len(selected) - failures}/{len(selected)} scenarios {'recorded' if args.record else 'match'}; work in {work}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
