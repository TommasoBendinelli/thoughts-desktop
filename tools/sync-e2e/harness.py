#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""End-to-end Sync harness against the disposable Fedora test instance.

Only the test instance is used (thoughts-test on port 8443). Every run
resets it first. Credentials are read from a private file and never
printed.
"""
from __future__ import annotations

import hashlib
import json
import os
import ssl
import subprocess
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path

DESKTOP_BUILD = Path(os.environ.get("THOUGHTS_DESKTOP_BUILD", Path(__file__).resolve().parents[2] / "build-thoughts"))
# The native source publisher of the desktop build, as Thoughts bundles it.
PUBLISH = DESKTOP_BUILD / "bin/thoughts-publish-sources"
# SSH destination (user@host) of the machine that runs the test instance and its
# password-free thoughts-test-environment actions (thoughts-server deploy/fedora/TESTING.md).
TEST_HOST = os.environ.get("THOUGHTS_E2E_TEST_HOST", "")
TEST_CONTROL = "/usr/local/sbin/thoughts-test-environment"
ANDROID = "thoughts-android-test"
MAC = "thoughts-mac-test"
MAC_SOURCE = "thoughts-mac-source-test"


def credentials(path: Path) -> dict:
    data = json.loads(path.read_text())
    if "8443" not in data["base_url"]:
        raise SystemExit("Refusing credentials that are not for the test instance")
    if MAC_SOURCE not in data.get("devices", {}):
        raise SystemExit(f"The test instance has no {MAC_SOURCE} device; re-run its install and refresh the credentials")
    return data


def control(action: str) -> dict:
    """Run a password-free test-instance action over SSH."""
    if not TEST_HOST:
        raise SystemExit("Set THOUGHTS_E2E_TEST_HOST to the SSH destination (user@host) of the test instance")
    output = subprocess.check_output(["ssh", "-o", "BatchMode=yes", TEST_HOST, f"sudo -n {TEST_CONTROL} {action}"],
                                     text=True, stderr=subprocess.DEVNULL)
    return json.loads(output.strip().splitlines()[-1])


def _tls() -> ssl.SSLContext:
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


class Api:
    def __init__(self, base_url: str, token: str):
        self.base_url = base_url.rstrip("/")
        self.token = token

    def request(self, method: str, path: str, body: dict | None = None) -> dict:
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base_url + path, data=data, method=method,
                                         headers={"Authorization": "Bearer " + self.token, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=60, context=_tls()) as response:
                return json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            raise RuntimeError(f"{method} {path} -> {error.code}: {error.read()[:400]!r}") from None

    def upload(self, content: bytes, media_type: str = "application/octet-stream") -> str:
        digest = hashlib.sha256(content).hexdigest()
        request = urllib.request.Request(self.base_url + "/v1/objects", data=content, method="POST",
                                         headers={"Authorization": "Bearer " + self.token, "Content-Type": media_type,
                                                  "X-Content-SHA256": digest})
        with urllib.request.urlopen(request, timeout=60, context=_tls()) as response:
            response.read()
        return digest

    def import_pdf(self, filename: str, content: bytes) -> dict:
        """Add a standalone Library PDF, as Import PDF does."""
        digest = self.upload(content, "application/pdf")
        return self.request("POST", "/v1/library/imports",
                            {"document_id": str(uuid.uuid4()), "filename": filename, "object_sha256": digest})

    def library(self) -> dict:
        return self.request("GET", "/v2/library")

    def sources(self) -> dict[str, dict]:
        """Fedora's current source heads by path, deleted ones included."""
        return {item["path"]: item for item in self.request("GET", "/v1/sources").get("sources", [])}

    def projects(self) -> dict:
        return self.request("GET", "/v1/library/projects")


@dataclass
class Project:
    """A Library project with one Markdown source folder, shaped like "Applications"."""
    root: Path
    project_id: str
    owner: str = "docs-harness"
    source: str = "markdown/source/Proposal .md"
    output: str = "harness_proposal.pdf"
    document_id: str = ""

    @property
    def hash(self) -> str:
        return hashlib.sha256(self.project_id.encode()).hexdigest()

    @property
    def default_folder(self) -> str:
        """The Project's default folder: default/ (canonical) once it exists,
        else legacy documents/ (what Fedora creates before the rename)."""
        base = f".thoughts/projects/{self.hash}"
        return f"{base}/default" if (self.root / base / "default").is_dir() else f"{base}/documents"

    @property
    def manifest_path(self) -> Path:
        return self.root / self.default_folder / "thoughts-project.json"

    @property
    def source_path(self) -> Path:
        return self.root / self.owner / self.source

    def __post_init__(self):
        # Like the app: default-folder identities are derived, every document
        # identity (including a source folder's aggregate) is random.
        self.document_id = self.document_id or str(uuid.uuid4())
        self.source_folder_document_id = str(uuid.uuid4())

    def write(self, text: str) -> None:
        default_folder = str(uuid.uuid5(uuid.NAMESPACE_URL, self.project_id + "/default"))
        source_folder = str(uuid.uuid5(uuid.NAMESPACE_URL, self.project_id + "/" + self.owner))
        manifest = {
            "schema_version": 5, "kind": "thoughts-project", "id": self.project_id, "name": "Harness Applications",
            "default_folder_id": default_folder,
            "source_folders": [
                {"folder_id": default_folder, "path": self.default_folder,
                 "document_id": str(uuid.uuid5(uuid.NAMESPACE_URL, default_folder))},
                {"folder_id": source_folder, "path": self.owner,
                 "document_id": self.source_folder_document_id}],
            "documents": [{"document_id": self.document_id, "source": {"folder_id": source_folder, "path": self.source},
                           "format": "markdown", "included": True}],
            "rendering": {self.document_id: {"stylesheet": "markdown/support/document.css", "output": self.output}},
            "document_names": {self.document_id: "Harness proposal"},
        }
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        self.manifest_path.write_text(json.dumps(manifest, indent=1) + "\n")
        stylesheet = self.root / self.owner / "markdown/support/document.css"
        stylesheet.parent.mkdir(parents=True, exist_ok=True)
        stylesheet.write_text("body { font-family: serif; }\n")
        self.edit(text)

    def declare_markdown(self, source: str, name: str) -> str:
        """Declare another included Markdown document of the source folder."""
        manifest = json.loads(self.manifest_path.read_text())
        folder = manifest["source_folders"][1]["folder_id"]
        document_id = str(uuid.uuid4())
        manifest["documents"].append({"document_id": document_id, "source": {"folder_id": folder, "path": source},
                                      "format": "markdown", "included": True})
        manifest["rendering"][document_id] = {"stylesheet": "markdown/support/document.css"}
        manifest["document_names"][document_id] = name
        self.manifest_path.write_text(json.dumps(manifest, indent=1) + "\n")
        return document_id

    def attach_pdf(self, imported: dict, managed_copy: bytes, name: str) -> str:
        """Attach an imported Library PDF to the default folder.

        Like "Applications", the Project keeps and pins its own copy, which
        can differ from the document's current Library publication."""
        pinned_sha256 = hashlib.sha256(managed_copy).hexdigest()
        manifest = json.loads(self.manifest_path.read_text())
        filename = imported["filename"]
        document_id = imported["document_id"]
        manifest["documents"].append({"document_id": document_id,
                                      "source": {"folder_id": manifest["default_folder_id"], "path": filename},
                                      "format": "pdf", "included": True})
        manifest["rendering"][document_id] = {"sha256": pinned_sha256}
        manifest["document_names"][document_id] = name
        self.manifest_path.write_text(json.dumps(manifest, indent=1) + "\n")
        (self.manifest_path.parent / filename).write_bytes(managed_copy)
        return document_id

    def edit(self, text: str) -> None:
        self.source_path.parent.mkdir(parents=True, exist_ok=True)
        self.source_path.write_text(text)


class Publisher:
    """Another device publishing from its own folder with the real publisher CLI."""

    def __init__(self, root: Path, state: Path, base_url: str, token: str):
        self.root, self.state, self.base_url, self.token = root, state, base_url, token
        self.manifest = state / "source-manifest.json"
        state.mkdir(parents=True, exist_ok=True)

    def _run(self, executable: Path, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([str(executable), *args], capture_output=True, text=True, timeout=900,
                              env={**os.environ, "THOUGHTS_TOKEN": self.token, "THOUGHTS_SOURCE_TOKEN": self.token})

    def generate_manifest(self) -> None:
        result = self._run(PUBLISH, "generate-manifest", "--docs-root", str(self.root), "--output", str(self.manifest))
        if result.returncode:
            raise RuntimeError("generate-manifest failed: " + (result.stderr or result.stdout)[-2000:])

    def publish(self, target: str | None = None) -> subprocess.CompletedProcess:
        args = ["--root", str(self.root), "--manifest", str(self.manifest), "--state", str(self.state / "source-publication.json"),
                "--bootstrap-state", str(self.state / "source-sync-state.json"), "--server", self.base_url]
        if target:
            args += ["--target", target, "--force-build"]
        return self._run(PUBLISH, *args)


class MacDesktop:
    """The desktop's own Sync engine (autotests/syncdriver.cpp) as the Mac test device.

    It runs with a private HOME, so its state, source manifest, and docs root
    are all disposable and never the real Library."""

    def __init__(self, workspace: Path, base_url: str, token: str, source_token: str):
        self.home = workspace / "thoughts-e2e-home"
        self.docs = self.home / "docs"
        self.docs.mkdir(parents=True, exist_ok=True)
        # Like production, Sync and the source publisher are separate devices.
        self.base_url, self.token, self.source_token = base_url, token, source_token
        self.driver = DESKTOP_BUILD / "bin/thoughts-sync-driver"
        self.log: list[dict] = []

    def _run(self, *args: str, timeout: int = 1800) -> dict:
        environment = {**os.environ, "HOME": str(self.home), "CFFIXED_USER_HOME": str(self.home),
                       "THOUGHTS_SERVER_URL": self.base_url, "THOUGHTS_TOKEN": self.token,
                       "THOUGHTS_SOURCE_TOKEN": self.source_token, "QT_QPA_PLATFORM": "offscreen"}
        result = subprocess.run([str(self.driver), *args, "--docs", str(self.docs), "--timeout", str(timeout)],
                                capture_output=True, text=True, timeout=timeout + 120, env=environment)
        lines = [line for line in result.stdout.splitlines() if line.startswith("{")]
        if result.returncode or not lines:
            raise RuntimeError(f"driver {args} exited {result.returncode}: {(result.stderr or result.stdout)[-3000:]}")
        report = json.loads(lines[-1])
        report["stderr_tail"] = result.stderr[-3000:]
        self.log.append({"command": list(args), **report})
        return report

    def sync(self) -> dict:
        return self._run("sync")

    def conflict(self) -> dict:
        return self._run("conflict")

    def resolve(self, contents: bytes) -> dict:
        path = self.home / "resolution.txt"
        path.write_bytes(contents)
        return self._run("resolve", "--contents", str(path))

    def path(self, relative: str) -> Path:
        return self.docs / relative


def remove_tree(path: Path) -> None:
    """Remove a workspace, including the Library's read-only generations."""
    for directory, _subdirectories, _files in os.walk(path):
        os.chmod(directory, 0o755)
    import shutil
    shutil.rmtree(path, ignore_errors=True)


def blank_pdf(pages: int, width: float = 612) -> bytes:
    import io
    from pypdf import PdfWriter
    writer = PdfWriter()
    for _ in range(pages):
        writer.add_blank_page(width=width, height=792)
    output = io.BytesIO()
    writer.write(output)
    return output.getvalue()


def wait_for(description: str, predicate, timeout: float = 600, interval: float = 3):
    started = time.monotonic()
    last = None
    while time.monotonic() - started < timeout:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise TimeoutError(f"Timed out after {timeout:.0f}s waiting for {description}")
