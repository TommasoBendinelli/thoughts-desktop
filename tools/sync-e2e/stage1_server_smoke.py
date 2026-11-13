#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Stage 1: a project published by one device builds its PDF and Project reading."""
import sys
import tempfile
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import harness  # noqa: E402

credentials = harness.credentials(Path(sys.argv[1]))
print("reset:", harness.control("reset")["services"])
api = harness.Api(credentials["base_url"], credentials["devices"][harness.ANDROID])
with tempfile.TemporaryDirectory(prefix="thoughts-e2e-") as temporary:
    root = Path(temporary) / "android-docs"
    project = harness.Project(root, "new:" + str(uuid.uuid4()))
    project.write("# Harness proposal\n\nFirst version.\n")
    android = harness.Publisher(root, Path(temporary) / "android-state", credentials["base_url"], credentials["devices"][harness.ANDROID])
    android.generate_manifest()
    result = android.publish(target=project.output)
    print("publish exit:", result.returncode)
    print((result.stdout + result.stderr)[-1500:])
    library = harness.wait_for("the published PDF and Project reading", lambda: (
        (lib := api.library()) and any(p.get("filename") == project.output for p in lib.get("publications", []))
        and lib.get("project_readings") and lib), timeout=900)
    pdf = next(p for p in library["publications"] if p["filename"] == project.output)
    print("publication:", pdf["filename"], pdf["sha256"][:12])
    print("readings:", [(r["filename"], r["sha256"][:12]) for r in library["project_readings"]])
