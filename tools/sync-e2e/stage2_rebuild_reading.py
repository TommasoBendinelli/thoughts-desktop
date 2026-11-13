#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Stage 2: rebuilding a document republishes its Project reading with the new PDF."""
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
    android = harness.Publisher(project.root, Path(temporary) / "android-state", credentials["base_url"], credentials["devices"][harness.ANDROID])
    android.generate_manifest()
    assert android.publish(target=project.output).returncode == 0

    def published(sha=None):
        library = api.library()
        pdf = next((p for p in library.get("publications", []) if p["filename"] == project.output), None)
        if pdf and library.get("project_readings") and (sha is None or pdf["sha256"] != sha):
            return library, pdf
        return None

    library, first = harness.wait_for("the first build", published, timeout=900)
    first_reading = library["project_readings"][0]["sha256"]
    print("first build:", first["sha256"][:12], "reading:", first_reading[:12])

    project.edit("# Harness proposal\n\nSecond version with a table.\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n")
    result = android.publish(target=project.output)
    assert result.returncode == 0, (result.stdout + result.stderr)[-1500:]
    library, second = harness.wait_for("the rebuild", lambda: published(first["sha256"]), timeout=900)
    second_reading = library["project_readings"][0]["sha256"]
    catalogue = api.projects()["projects"][0]
    print("rebuild:", second["sha256"][:12], "reading:", second_reading[:12])
    print("catalogue document PDFs:", [d.get("sha256", "")[:12] for d in catalogue["documents"]], "reading revision:", str(catalogue.get("reading_revision"))[:12])
    if second_reading == first_reading:
        print("REPRODUCED: the Project reading was not republished after the rebuild")
        sys.exit(1)
    print("OK: the Project reading follows the rebuilt PDF")
