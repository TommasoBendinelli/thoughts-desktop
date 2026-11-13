#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Stage 4: the desktop's own Sync with edits on the Mac and on Android.

The Mac is the desktop Sync engine (autotests/syncdriver.cpp) with a private
home; Android is the real publisher CLI working on its own copy. Every check
compares what each side has against Fedora's source heads and the Mac's
active Library generation.
"""
import hashlib
import json
import shutil
import sys
import tempfile
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import harness  # noqa: E402

# Any real TrueType font; the build must embed it.
FONT = Path("/System/Library/Fonts/Supplemental/Arial.ttf")
PROPOSAL = ["# Harness proposal", "", "Aims line.", "", "Method line.", "", "Budget line.", "", "Risks line.", ""]
failures: list[str] = []


def check(condition: bool, description: str, detail: str = "") -> bool:
    print(("PASS " if condition else "FAIL ") + description + (f"  [{detail}]" if detail and not condition else ""))
    if not condition:
        failures.append(description + (f": {detail}" if detail else ""))
    return condition


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def summary(report: dict) -> str:
    details = report.get("details", "")
    # A publisher traceback ends in its exception; keep that, not the frames.
    errors = [line.strip() for line in details.splitlines() if "Error" in line or "error" in line.lower()[:40]]
    return json.dumps({k: report.get(k) for k in ("state", "message", "error_code", "generation")}) + " " + (" | ".join(errors[-3:]) or details[-400:])


credentials = harness.credentials(Path(sys.argv[1]))
keep = len(sys.argv) > 2 and sys.argv[2] == "--keep"
harness.control("reset")
fedora = harness.Api(credentials["base_url"], credentials["devices"][harness.ANDROID])
workspace = Path(tempfile.mkdtemp(prefix="thoughts-e2e-"))
try:
    mac = harness.MacDesktop(workspace, credentials["base_url"], credentials["devices"][harness.MAC], credentials["devices"][harness.MAC_SOURCE])
    project = harness.Project(mac.docs, "new:" + str(uuid.uuid4()))
    project.write("\n".join(PROPOSAL))
    source = project.owner + "/" + project.source
    stylesheet = project.owner + "/markdown/support/document.css"

    # 1. The Mac creates the Project; its Sync publishes it and builds the PDF.
    first = mac.sync()
    check(first["state"] == "succeeded", "Mac publishes a new Project", summary(first))
    heads = fedora.sources()
    check(heads.get(source, {}).get("sha256") == digest(mac.path(source).read_bytes()), "The server has the Mac's source")
    built = harness.wait_for("the first PDF", lambda: next((p for p in fedora.library().get("publications", []) if p["filename"] == project.output), None), timeout=900)
    initial = mac.sync()
    check(initial["pdfs"].get(project.output) == built["sha256"], "Mac Library shows the built PDF", summary(initial))
    check(bool(initial["readings"]), "Mac Library has the Project reading")

    # Android starts from a copy of the same Project.
    android_root = workspace / "android-docs"
    shutil.copytree(mac.docs / project.owner, android_root / project.owner)
    shutil.copytree(mac.docs / ".thoughts/projects", android_root / ".thoughts/projects")
    android = harness.Publisher(android_root, workspace / "android-state", credentials["base_url"], credentials["devices"][harness.ANDROID])
    android.generate_manifest()
    adopted = android.publish()
    check(adopted.returncode == 0, "Android adopts the published Project", (adopted.stdout + adopted.stderr)[-800:])

    def edit(root: Path, relative: str, line: int, text: str) -> None:
        path = root / relative
        lines = path.read_text().split("\n")
        lines[line] = text
        path.write_text("\n".join(lines))

    def android_publish(description: str) -> None:
        android.generate_manifest()
        result = android.publish()
        check(result.returncode == 0, description, (result.stdout + result.stderr)[-800:])

    # 2. Different lines of one file: Fedora merges; the Mac installs the merge.
    edit(android_root, source, 4, "Method line edited on Android.")
    android_publish("Android publishes a Method edit")
    edit(mac.docs, source, 2, "Aims line edited on the Mac.")
    merged = mac.sync()
    text = mac.path(source).read_text()
    check(merged["state"] == "succeeded", "Mac Sync merges edits to different lines", summary(merged))
    check("edited on Android" in text and "edited on the Mac" in text, "Mac file has both edits", text[:300])
    check(fedora.sources()[source]["sha256"] == digest(mac.path(source).read_bytes()), "The server and the Mac agree after the merge")
    rebuilt = harness.wait_for("the rebuilt PDF", lambda: (lambda r: r if r["pdfs"].get(project.output) not in (None, built["sha256"]) else None)(mac.sync()), timeout=900, interval=5)
    check(rebuilt["pdfs"][project.output] != built["sha256"], "Mac picks up the rebuilt PDF")
    reading = next(iter(rebuilt["readings"].values()))
    check(reading and reading[0]["pdf_sha256"] == rebuilt["pdfs"][project.output], "Mac's Project reading shows the rebuilt PDF", json.dumps(rebuilt["readings"]))

    # 3. The same line of two files: two conflicts, resolved one by one.
    shutil.copy2(mac.path(source), android_root / source)
    edit(android_root, source, 6, "Budget from Android.")
    edit(android_root, stylesheet, 0, "body { font-family: sans-serif; }")
    android_publish("Android publishes Budget and stylesheet edits")
    edit(mac.docs, source, 6, "Budget from the Mac.")
    edit(mac.docs, stylesheet, 0, "body { font-family: monospace; }")
    # Publication stops at the first conflict, so each round offers one
    # file; resolving it and syncing surfaces the next.
    latest = mac.sync()
    check(bool(latest.get("conflicts")), "Mac records a conflict", summary(latest))
    choices = {source: "mac", stylesheet: "fedora"}
    resolutions = {}
    import base64
    for _ in range(len(choices) + 2):
        if set(resolutions) == set(choices):
            break
        if not latest.get("conflicts"):
            # The next conflict appears when Sync publishes again.
            latest = mac.sync()
            print("  Sync after a resolution:", summary(latest), json.dumps(sorted(latest.get("conflicts") or {})))
            if not latest.get("conflicts"):
                break
        bundle = mac.conflict()
        if not check(bundle.get("path") in choices and bundle["path"] not in resolutions,
                     "Mac prepares the next conflicting file", bundle.get("error", "") + json.dumps(bundle.get("path"))):
            break
        choice = choices[bundle["path"]]
        contents = base64.b64decode(bundle[f"{choice}_content_base64"])
        resolutions[bundle["path"]] = contents
        latest = mac.resolve(contents)
        print("  Resolution Sync:", summary(latest), json.dumps(sorted(latest.get("conflicts") or {})))
        check(latest["state"] == "succeeded" or bool(latest.get("conflicts")),
              f"Keep {choice} for {bundle['path']} is submitted", summary(latest))
    check(set(resolutions) == set(choices), "Both conflicting files were offered for resolution", json.dumps(sorted(resolutions)))
    heads = fedora.sources()
    for path, contents in resolutions.items():
        check(heads[path]["sha256"] == digest(contents), f"The server has the chosen version of {path}")
        check(mac.path(path).read_bytes() == contents, f"Mac has the chosen version of {path}")
    after = mac.sync()
    check(after["state"] == "succeeded" and not after.get("conflicts"), "No conflicts remain", summary(after))

    # 4. A saved hand merge, as "Submit saved merge" sends it.
    shutil.copy2(mac.path(source), android_root / source)
    shutil.copy2(mac.path(stylesheet), android_root / stylesheet)
    android.generate_manifest()
    android.publish()
    edit(android_root, source, 8, "Risks from Android.")
    android_publish("Android publishes a Risks edit")
    edit(mac.docs, source, 8, "Risks from the Mac.")
    conflicted = mac.sync()
    check(source in (conflicted.get("conflicts") or {}), "Mac records the Risks conflict", summary(conflicted))
    bundle = mac.conflict()
    if check(bundle.get("path") == source and "mac_content_base64" in bundle, "Mac prepares the Risks conflict", bundle.get("error", "")):
        merged_text = base64.b64decode(bundle["mac_content_base64"]).decode().replace("Risks from the Mac.", "Risks from the Mac and from Android.")
    else:
        merged_text = mac.path(source).read_text()
    resolved = mac.resolve(merged_text.encode())
    check(resolved["state"] == "succeeded", "Saved hand merge is submitted", summary(resolved))
    check(fedora.sources()[source]["sha256"] == digest(merged_text.encode()), "The server has the hand merge")

    # 5. Added, deleted, and renamed files on the Mac.
    extra = project.owner + "/markdown/support/extra.css"
    mac.path(extra).write_text("p { margin: 0; }\n")
    added = mac.sync()
    heads = fedora.sources()
    check(added["state"] == "succeeded" and extra in heads and not heads[extra].get("deleted"), "A file added on the Mac reaches the server", summary(added))
    renamed = project.owner + "/markdown/support/renamed.css"
    mac.path(extra).rename(mac.path(renamed))
    moved = mac.sync()
    heads = fedora.sources()
    check(moved["state"] == "succeeded" and heads.get(extra, {}).get("deleted") and renamed in heads and not heads[renamed].get("deleted"),
          "A file renamed on the Mac is deleted and added on the server", summary(moved) + json.dumps({p: heads.get(p) for p in (extra, renamed)}))
    mac.path(renamed).unlink()
    removed = mac.sync()
    check(removed["state"] == "succeeded" and fedora.sources().get(renamed, {}).get("deleted"), "A file deleted on the Mac is deleted on the server", summary(removed))

    # 6. A file deleted on Android is removed on the Mac.
    shutil.copy2(mac.path(source), android_root / source)
    (android_root / renamed).unlink(missing_ok=True)
    mac.path(extra).unlink(missing_ok=True)
    bonus = project.owner + "/markdown/support/bonus.css"
    mac.path(bonus).write_text("h1 { color: black; }\n")
    check(mac.sync()["state"] == "succeeded", "Mac publishes another file")
    shutil.copy2(mac.path(bonus), android_root / bonus)
    android.generate_manifest()
    android.publish()
    (android_root / bonus).unlink()
    android_publish("Android deletes that file")
    pulled = mac.sync()
    check(pulled["state"] == "succeeded" and not mac.path(bonus).exists(), "Mac removes a file deleted on Android", summary(pulled))

    # 7. A file added on Android is installed on the Mac.
    shutil.copy2(mac.path(source), android_root / source)
    added_remotely = project.owner + "/markdown/support/android.css"
    (android_root / added_remotely).write_text("em { font-style: italic; }\n")
    android_publish("Android adds a file")
    installed = mac.sync()
    check(installed["state"] == "succeeded" and mac.path(added_remotely).is_file()
          and mac.path(added_remotely).read_bytes() == (android_root / added_remotely).read_bytes(),
          "Mac installs a file added on Android", summary(installed))

    # 8. A font the stylesheet loads with url() reaches Fedora's build.
    font = project.owner + "/markdown/support/fonts/harness-sans.ttf"
    mac.path(font).parent.mkdir(parents=True, exist_ok=True)
    mac.path(font).write_bytes(FONT.read_bytes())
    css = mac.path(stylesheet)
    # Appended, so this body rule wins over the stylesheet's earlier one.
    css.write_text(css.read_text() + '\n@font-face { font-family: "Harness Sans"; src: url("fonts/harness-sans.ttf") format("truetype"); }\n'
                   'body { font-family: "Harness Sans"; }\n')
    before_font = fedora.library()
    before_pdf = next(p["sha256"] for p in before_font["publications"] if p["filename"] == project.output)
    with_font = mac.sync()
    check(with_font["state"] == "succeeded", "Mac Sync with a stylesheet font", summary(with_font))
    fonted = with_font
    for _ in range(20):
        if fonted["pdfs"].get(project.output) not in (None, before_pdf) or "failed" in (fonted.get("details") or ""):
            break
        time.sleep(5)
        fonted = mac.sync()
    check(fonted["pdfs"].get(project.output) not in (None, before_pdf), "The server rebuilds the PDF with the stylesheet font", summary(fonted))
    # The body may use only the family loaded from fonts/; WeasyPrint names
    # the embedded font after it ("Harness-Sans").
    from pypdf import PdfReader
    fonts = set()
    for page in PdfReader(mac.docs / "current_pdfs" / project.output).pages:
        for font in (page.get("/Resources", {}).get("/Font") or {}).values():
            fonts.add(str(font.get_object().get("/BaseFont", "")))
    check(any("Harness" in name for name in fonts), "The rebuilt PDF embeds the stylesheet font", ", ".join(sorted(fonts)))

    # 9. Repeated Syncs change nothing.
    settled = mac.sync()
    for attempt in range(2):
        started = time.monotonic()
        again = mac.sync()
        check(again["state"] == "succeeded" and again["generation"] == settled["generation"], f"Repeated Sync {attempt + 1} is a no-op", summary(again))
        check(time.monotonic() - started < 60, f"Repeated Sync {attempt + 1} finishes promptly", f"{time.monotonic() - started:.0f}s")
finally:
    (workspace / "mac-log.json").write_text(json.dumps(mac.log, indent=1))
    print("Mac Sync log:", workspace / "mac-log.json" if keep else "(removed)")
    if not keep:
        harness.remove_tree(workspace)

if failures:
    print(f"\n{len(failures)} FAILED:\n  " + "\n  ".join(failures))
    sys.exit(1)
print("\nOK: desktop Sync with edits on both sides")
