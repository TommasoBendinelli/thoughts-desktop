#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Stage 3: a Project with a pinned imported PDF and an unbuilt document.

This is the "Applications" shape: one Markdown document that is rebuilt, one
imported PDF whose declaration pins an older copy than its publication, and
one declared document that has never been built. Every rebuild must still
republish the Project reading with the ready documents.
"""
import sys
import tempfile
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import harness  # noqa: E402

credentials = harness.credentials(Path(sys.argv[1]))
harness.control("reset")
api = harness.Api(credentials["base_url"], credentials["devices"][harness.ANDROID])
with tempfile.TemporaryDirectory(prefix="thoughts-e2e-") as temporary:
    project = harness.Project(Path(temporary) / "android-docs", "new:" + str(uuid.uuid4()))
    project.write("# Harness proposal\n\nFirst version.\n")
    # Declared and included, but never requested as a build target.
    project.declare_markdown("markdown/source/Draft .md", "Unbuilt draft")
    (project.root / project.owner / "markdown/source/Draft .md").write_text("# Draft\n")
    imported_pdf = harness.blank_pdf(2)
    older_copy = harness.blank_pdf(2, width=600)
    imported = api.import_pdf("harness_import.pdf", imported_pdf)
    project.attach_pdf(imported, older_copy, "Imported example")

    android = harness.Publisher(project.root, Path(temporary) / "android-state", credentials["base_url"], credentials["devices"][harness.ANDROID])
    android.generate_manifest()
    result = android.publish(target=project.output)
    assert result.returncode == 0, (result.stdout + result.stderr)[-2000:]

    def state(previous_pdf=None):
        library = api.library()
        pdf = next((p for p in library.get("publications", []) if p["filename"] == project.output), None)
        if not pdf or (previous_pdf and pdf["sha256"] == previous_pdf) or not library.get("project_readings"):
            return None
        entry = next(p for p in api.projects()["projects"] if p["id"] == project.project_id)
        if not entry.get("target"):
            return None
        return pdf, library["project_readings"][0], entry

    failures = []
    pdf, reading, entry = harness.wait_for("the first build and reading", state, timeout=900)
    documents = {d.get("name"): d for d in entry["documents"]}
    print("documents:", {name: (bool(d.get("pending_build")), (d.get("sha256") or "")[:12]) for name, d in documents.items()})
    built_pages = next(d for d in entry["documents"] if d.get("filename") == project.output)["page_count"]
    expected_pages = built_pages + 2
    print("reading pages:", entry.get("page_count"), "expected:", expected_pages)
    if entry.get("page_count") != expected_pages:
        failures.append(f"first reading has {entry.get('page_count')} pages, expected the built document plus the imported PDF ({expected_pages})")
    if documents.get("Imported example", {}).get("sha256") != imported["sha256"]:
        failures.append("the imported PDF does not use its current publication")
    if not documents.get("Unbuilt draft", {}).get("pending_build"):
        failures.append("the unbuilt draft is not reported as pending")

    project.edit("# Harness proposal\n\nSecond version.\n\n" + "More text.\n\n" * 80)
    result = android.publish(target=project.output)
    assert result.returncode == 0, (result.stdout + result.stderr)[-2000:]
    second_pdf, second_reading, second_entry = harness.wait_for("the rebuild", lambda: state(pdf["sha256"]), timeout=900)
    second_pages = next(d for d in second_entry["documents"] if d.get("filename") == project.output)["page_count"]
    print("rebuild:", second_pdf["sha256"][:12], "reading:", reading["sha256"][:12], "->", second_reading["sha256"][:12],
          "pages:", second_entry.get("page_count"))
    if second_reading["sha256"] == reading["sha256"]:
        failures.append("the Project reading was not republished after the rebuild")
    if second_entry.get("page_count") != second_pages + 2:
        failures.append(f"rebuilt reading has {second_entry.get('page_count')} pages, expected {second_pages + 2}")
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        sys.exit(1)
    print("OK: the reading follows rebuilds and keeps the imported PDF despite an unbuilt document")
