#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Golden test of the native thoughts-publish-sources.

Each scenario builds a docs folder and runs the publisher with fixed
arguments. Exit codes, stdout, stderr (progress lines first) and the files
the publisher writes are compared, after replacing the scenario directory
with <ROOT>, against tools/publisher/tests/golden/publisher/<scenario>.json.

  python3 publisher_golden.py --native <build>/bin/thoughts-publish-sources \\
      [--only REGEX] [--record] [--keep]

--record rewrites the expected files from the native publisher; review the
diff before committing it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unicodedata
import uuid
from pathlib import Path

PROGRESS = "THOUGHTS_LOCAL_SOURCE_PROGRESS "


# ---------------------------------------------------------------- fixtures

class Ids:
    """Deterministic version-4 UUIDs, so both runs see identical files."""

    def __init__(self, seed):
        self.random = random.Random(seed)

    def __call__(self):
        return str(uuid.UUID(int=self.random.getrandbits(128), version=4))


def project_hash(project_id):
    return hashlib.sha256(project_id.encode()).hexdigest()


# A Project's default folder is .thoughts/projects/<hash>/default since the
# 2026-10-17 rename, .../documents before it; both spellings stay valid.
LEGACY_SEGMENT = "documents"
CANONICAL_SEGMENT = "default"


def configuration(ids, project_id, folders, *, pdfs=(), rendering=None, name=None, extra=None, segment=LEGACY_SEGMENT):
    """folders: {owner: [(source, format, included), ...]}."""
    default_id = ids()
    value = {
        "schema_version": 5,
        "kind": "thoughts-project",
        "id": project_id,
        "name": name if name is not None else project_id,
        "default_folder_id": default_id,
        "source_folders": [{"folder_id": default_id,
                            "path": f".thoughts/projects/{project_hash(project_id)}/{segment}",
                            "document_id": ids()}],
        "documents": [],
    }
    documents = {}
    for owner, entries in folders.items():
        folder_id = ids()
        value["source_folders"].append({"folder_id": folder_id, "path": owner, "document_id": ids()})
        for source, kind, included in entries:
            document_id = ids()
            documents[(owner, source)] = document_id
            value["documents"].append({"document_id": document_id,
                                       "source": {"folder_id": folder_id, "path": source},
                                       "format": kind, "included": included})
    for filename in pdfs:
        document_id = ids()
        value["documents"].append({"document_id": document_id,
                                   "source": {"folder_id": default_id, "path": filename},
                                   "format": "pdf", "included": True})
        value.setdefault("rendering", {})[document_id] = {"sha256": "0" * 64}
    for (owner, source), options in (rendering or {}).items():
        value.setdefault("rendering", {})[documents[(owner, source)]] = options
    if extra:
        extra(value, documents)
    return value


def default_segment(value):
    """The segment of value's default folder (legacy when it names none)."""
    try:
        folder = next(f["path"] for f in value["source_folders"] if f["folder_id"] == value["default_folder_id"])
    except (KeyError, StopIteration, TypeError):
        return LEGACY_SEGMENT
    segment = folder.rsplit("/", 1)[-1] if isinstance(folder, str) else ""
    return segment if segment in (LEGACY_SEGMENT, CANONICAL_SEGMENT) else LEGACY_SEGMENT


def write_configuration(root, value, *, raw=None, directory=None, segment=None):
    folder = root / ".thoughts/projects" / (directory or project_hash(value["id"])) / (segment or default_segment(value))
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / "thoughts-project.json"
    path.write_bytes(raw if raw is not None else (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode())
    return path


def write(path, data=""):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data, encoding="utf-8")


def symlink(path, target):
    path.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(target, path)


def paper(root, ids, *, name="docs-paper"):
    """The verified fixture of spec A, appendix B.1."""
    project = root / name
    for relative, text in [("tex/source/main.tex", "main\n"), ("tex/source/sub/b.tex", "b\n"),
                           ("tex/fragments/f.tex", "f\n"), ("tex/support/s.sty", "s\n"),
                           ("tex/support/notes.txt", "n\n"), ("tex/support/x.json", "{}\n"),
                           ("markdown/source/doc.md", "# d\n"), ("markdown/support/style.css", "p{}\n"),
                           ("notes.md", "x\n"), ("tex/docs_order_manifest.txt", "main.tex\n"),
                           ("tex/support/thoughts_document_order.txt", "x\n"), ("README.txt", "r\n"),
                           ("tex/source/Ünï.tex", "u\n"), (".git/config", "g\n"),
                           ("tex/source/bad.tex", b"\xff\xfe bad")]:
        write(project / relative, text)
    write_configuration(root, configuration(ids, "new:" + name, {
        name: [("tex/source/main.tex", "latex", True), ("markdown/source/doc.md", "markdown", True)]}))


# ---------------------------------------------------------------- scenarios

SCENARIOS = {}


def scenario(name):
    def register(function):
        SCENARIOS[name] = function
        return function
    return register


GENERATE = ["generate-manifest", "--docs-root", "docs", "--output", "out.json"]
GENERATE_PROGRESS = GENERATE + ["--desktop-progress-jsonl"]
CANDIDATES = ["list-candidates", "--docs-root", "docs"]


def refresh(project, manifest="manifest.json"):
    return ["refresh-project-manifest", "--docs-root", "docs", "--manifest", manifest, "--project", project,
            "--output", "refreshed.json", "--desktop-progress-jsonl"]


@scenario("paper")
def _(d, ids):
    root = d / "docs"
    paper(root, ids)
    write_configuration(root, configuration(ids, "zeta", {"docs-zeta": [("tex/source/z.tex", "latex", True)]},
                                            name="Ünicode"))
    write(root / "docs-zeta/tex/source/z.tex", "z\n")
    return [GENERATE_PROGRESS, GENERATE, CANDIDATES]


@scenario("empty-root")
def _(d, ids):
    (d / "docs").mkdir()
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("missing-root")
def _(d, ids):
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("not-installed")
def _(d, ids):
    root = d / "docs"
    write_configuration(root, configuration(ids, "p", {"docs-elsewhere": [("tex/source/main.tex", "latex", True)]}))
    return [GENERATE_PROGRESS, CANDIDATES]


def broken_configuration(raw=None, mutate=None, directory=None, symlinked=False):
    def build(d, ids):
        root = d / "docs"
        value = configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]})
        if mutate:
            mutate(value)
        write(root / "docs-a/tex/source/main.tex", "x\n")
        if symlinked:
            target = d / "real.json"
            write(target, json.dumps(value))
            symlink(root / ".thoughts/projects" / project_hash("p") / "documents/thoughts-project.json", target)
        else:
            write_configuration(root, value, raw=raw(value) if raw else None, directory=directory)
        return [GENERATE_PROGRESS, CANDIDATES]
    return build


SCENARIOS["config-not-json"] = broken_configuration(raw=lambda v: b"{bad")
SCENARIOS["config-bom"] = broken_configuration(raw=lambda v: b"\xef\xbb\xbf" + json.dumps(v).encode())
SCENARIOS["config-utf16"] = broken_configuration(raw=lambda v: json.dumps(v).encode("utf-16"))
SCENARIOS["config-utf16le-nobom"] = broken_configuration(raw=lambda v: json.dumps(v).encode("utf-16-le"))
SCENARIOS["config-invalid-utf8"] = broken_configuration(raw=lambda v: json.dumps(v).encode()[:-2] + b"\xff}")
SCENARIOS["config-schema-4"] = broken_configuration(mutate=lambda v: v.update(schema_version=4))
SCENARIOS["config-schema-5.0"] = broken_configuration(raw=lambda v: json.dumps(v).replace('"schema_version": 5', '"schema_version": 5.0').encode())
SCENARIOS["config-wrong-kind"] = broken_configuration(mutate=lambda v: v.update(kind="other"))
SCENARIOS["config-extra-key"] = broken_configuration(mutate=lambda v: v.update(color="red"))
SCENARIOS["config-wrong-dir"] = broken_configuration(directory="0" * 64)
SCENARIOS["config-uppercase-dir"] = broken_configuration(directory=project_hash("p").upper())
SCENARIOS["config-symlink"] = broken_configuration(symlinked=True)
SCENARIOS["config-uuid-type"] = broken_configuration(mutate=lambda v: v.update(default_folder_id=7))
SCENARIOS["config-uuid-short"] = broken_configuration(mutate=lambda v: v.update(default_folder_id="1234"))
SCENARIOS["config-uuid-nonhex"] = broken_configuration(mutate=lambda v: v.update(default_folder_id="z" * 32))
SCENARIOS["config-uuid-upper"] = broken_configuration(mutate=lambda v: v.update(default_folder_id=v["default_folder_id"].upper()))
SCENARIOS["config-uuid-braces"] = broken_configuration(mutate=lambda v: v.update(default_folder_id="{" + v["default_folder_id"] + "}"))


def mutate_document(**changes):
    def mutate(value):
        value["documents"][0].update(changes)
    return mutate


def mutate_source(**changes):
    def mutate(value):
        value["documents"][0]["source"].update(changes)
    return mutate


def mutate_folder(index, **changes):
    def mutate(value):
        value["source_folders"][index].update(changes)
    return mutate


SCENARIOS["config-included-int"] = broken_configuration(mutate=mutate_document(included=1))
SCENARIOS["config-format-other"] = broken_configuration(mutate=mutate_document(format="html"))
SCENARIOS["config-format-list"] = broken_configuration(mutate=mutate_document(format=["latex"]))
SCENARIOS["config-source-dotdot"] = broken_configuration(mutate=mutate_source(path="../x.tex"))
SCENARIOS["config-source-slashes"] = broken_configuration(mutate=mutate_source(path="tex//x.tex"))
SCENARIOS["config-source-control"] = broken_configuration(mutate=mutate_source(path="tex/\x7fx.tex"))
SCENARIOS["config-source-suffix"] = broken_configuration(mutate=mutate_source(path="tex/source/main.md"))
SCENARIOS["config-source-upper-suffix"] = broken_configuration(mutate=mutate_source(path="tex/source/MAIN.TEX"))
SCENARIOS["config-source-folder-list"] = broken_configuration(mutate=mutate_source(folder_id=["x"]))
SCENARIOS["config-source-folder-unknown"] = broken_configuration(mutate=mutate_source(folder_id="x"))
SCENARIOS["config-folder-name"] = broken_configuration(mutate=mutate_folder(1, path="Docs-a"))
SCENARIOS["config-folder-default-path"] = broken_configuration(mutate=mutate_folder(0, path="docs-z"))
SCENARIOS["config-folder-keys"] = broken_configuration(mutate=mutate_folder(1, extra=1))
SCENARIOS["config-document-v1"] = broken_configuration(mutate=mutate_document(document_id="6ba7b810-9dad-11d1-80b4-00c04fd430c8"))
SCENARIOS["config-rendering-list"] = broken_configuration(mutate=lambda v: v.update(rendering=[]))
SCENARIOS["config-rendering-unknown"] = broken_configuration(mutate=lambda v: v.update(rendering={"x": {}}))
SCENARIOS["config-names-bad"] = broken_configuration(mutate=lambda v: v.update(document_names={"x": "y"}))
SCENARIOS["config-names-ok"] = broken_configuration(mutate=lambda v: v.update(document_names={v["documents"][0]["document_id"]: "Title"}))
SCENARIOS["config-duplicate-key"] = broken_configuration(raw=lambda v: json.dumps(v)[:-1].encode() + b', "name": "again"}')
SCENARIOS["config-is-directory"] = None  # defined below


@scenario("config-is-directory")
def _(d, ids):
    root = d / "docs"
    (root / ".thoughts/projects" / project_hash("p") / "documents/thoughts-project.json").mkdir(parents=True)
    return [GENERATE_PROGRESS]


@scenario("config-escapes")
def _(d, ids):
    root = d / "docs"
    outside = d / "outside"
    value = configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]})
    write_configuration(outside, value)
    symlink(root / ".thoughts/projects" / project_hash("p"), outside / ".thoughts/projects" / project_hash("p"))
    write(root / "docs-a/tex/source/main.tex")
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("two-projects-one-folder")
def _(d, ids):
    root = d / "docs"
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
    write_configuration(root, configuration(ids, "q", {"docs-a": [("tex/source/other.tex", "latex", True)]}))
    write(root / "docs-a/tex/source/main.tex")
    return [GENERATE_PROGRESS, CANDIDATES]


def with_ignore(contents):
    def build(d, ids):
        root = d / "docs"
        paper(root, ids, name="docs-a")
        write(root / "docs-a/.ThoughtIgnore", contents)
        for relative in ["tex/source/cache/x.tex", "tex/source/deep/cache/y.tex", "tex/support/a b.sty",
                         "markdown/source/keep.md", "markdown/source/drop.md", "tex/source/[ab].tex",
                         "tex/source/a.TEX", "x/tex/source/a.md", "tex/fragments/sub/f2.tex"]:
            write(root / "docs-a" / relative, "t\n")
        return [GENERATE_PROGRESS]
    return build


IGNORE_RULES = {
    "ignore-empty": "",
    "ignore-basename": "*.sty\n",
    "ignore-directory": "cache/\n",
    "ignore-anchored": "/tex/fragments/\n",
    "ignore-recursive": "**/cache\n",
    "ignore-trailing-double": "tex/source/**\n",
    "ignore-negation": "*\n!*/\n!*.tex\n",
    "ignore-negation-parent": "*\n!*.tex\n",
    "ignore-classes-literal": "[ab].tex\n",
    "ignore-case": "*.TEX\n",
    "ignore-spaces": " a b.sty \n  # comment\n",
    "ignore-crlf": "drop.md\r\n*.sty\r",
    "ignore-bom": "﻿*.sty\n*.css\n",
    "ignore-question": "?.TEX\n",
    "ignore-itself": ".ThoughtIgnore\n",
    "ignore-markdown": "markdown/\n",
    "ignore-invalid-slash": "!/\n",
    "ignore-invalid-dot": "..\n",
    "ignore-invalid-backslash": "a\\b\n",
    "ignore-invalid-repr": "!'quote\\\n",
    "ignore-separators": "a.sty\x1cb.sty\x85*.css drop.md\n",
}
for _name, _rules in IGNORE_RULES.items():
    SCENARIOS[_name] = with_ignore(_rules)
SCENARIOS["ignore-invalid-utf8"] = with_ignore(b"\xff*.sty\n")
SCENARIOS["ignore-overlong"] = with_ignore(b"\xc0\xaf\n")


@scenario("ignore-directory-entry")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    (root / "docs-a/.ThoughtIgnore").mkdir()
    return [GENERATE_PROGRESS]


@scenario("ignore-symlink")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    symlink(root / "docs-a/.ThoughtIgnore", "missing")
    return [GENERATE_PROGRESS]


@scenario("ignore-large")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    write(root / "docs-a/.ThoughtIgnore", b"#" * (4 * 1024 * 1024 + 1))
    return [GENERATE_PROGRESS]


@scenario("backslash-name")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    write(root / "docs-a/tex/source/a\\b.tex", "x\n")
    return [GENERATE_PROGRESS]


@scenario("backslash-directory")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    write(root / "docs-a/other/a\\b/x.md", "x\n")
    return [GENERATE_PROGRESS]


@scenario("names")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    project = root / "docs-a"
    for name in ["UP.STY", "x.ﬆy", "Makefile", "foo.", "..tex", ".hidden.tex", "a..tex", "astral\U0001F600.tex",
                 ".tex", unicodedata.normalize("NFD", "é-nfd.tex"), "é-nfc.tex", "sp ace.bib", "tab\tname.cls",
                 "nl\nname.tex", "ß.TXT", "K.Markdown"]:
        write(project / "tex/support" / name, "x\n")
    for name in ["İx.MD", "a.CSS", "b.Txt", "c.png", "d.JPEG"]:
        write(project / "deep/er" / name, "x\n")
    write(project / "tex/source/exact.tex", b"a" * (4 * 1024 * 1024))
    write(project / "tex/source/large.tex", b"a" * (4 * 1024 * 1024 + 1))
    write(project / "tex/source/surrogate.tex", b"\xed\xa0\x80")
    write(project / "tex/source/bom.tex", b"\xef\xbb\xbfx")
    write(project / "tex/source/nul.tex", b"\x00")
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("symlinks")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    project = root / "docs-a"
    write(d / "outside/o.tex", "o\n")
    symlink(project / "tex/source/linked.tex", d / "outside/o.tex")
    symlink(project / "tex/source/inside.tex", "main.tex")
    symlink(project / "tex/source/broken.tex", "missing.tex")
    symlink(project / "tex/source/dirlink", d / "outside")
    symlink(project / "markdown/linked.md", "source/doc.md")
    write(project / "real/tex/source/r.tex", "r\n")
    return [GENERATE_PROGRESS]


@scenario("symlinked-source-root")
def _(d, ids):
    root = d / "docs"
    project = root / "docs-a"
    write(d / "store/main.tex", "m\n")
    symlink(project / "tex/source", d / "store")
    write(project / "notes.md", "n\n")
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
    return [GENERATE_PROGRESS]


@scenario("symlinked-project")
def _(d, ids):
    root = d / "docs"
    write(d / "store/tex/source/main.tex", "m\n")
    write(d / "store/notes.md", "n\n")
    symlink(root / "docs-a", d / "store")
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
    return [GENERATE_PROGRESS, CANDIDATES, refresh("docs-a")]


@scenario("symlinked-project-inside")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-real")
    symlink(root / "docs-link", "docs-real")
    write_configuration(root, configuration(ids, "p", {"docs-link": [("tex/source/main.tex", "latex", True)]}))
    write(d / "manifest.json", json.dumps({"schema_version": 3, "projects": [
        {"project": "docs-link", "targets": [], "files": []}]}))
    return [GENERATE_PROGRESS, CANDIDATES, refresh("docs-link")]


@scenario("source-escapes")
def _(d, ids):
    root = d / "docs"
    write(d / "outside/main.tex", "m\n")
    symlink(root / "docs-a/tex/source/main.tex", d / "outside/main.tex")
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("working-directories")
def _(d, ids):
    root = d / "docs"
    project = root / "docs-a"
    for relative in ["paper/tex/source/main.tex", "paper/tex/support/p.sty", "tex/source/top.tex", "a/tex/source/x.tex",
                     "linked/tex/source/l.tex"]:
        write(project / relative, "x\n")
    write(d / "elsewhere/tex/source/e.tex", "e\n")
    symlink(project / "via", d / "elsewhere")
    write_configuration(root, configuration(ids, "p", {"docs-a": [
        ("paper/tex/source/main.tex", "latex", True), ("tex/source/top.tex", "latex", True),
        ("a/tex/source/x.tex", "latex", False), ("via/tex/source/e.tex", "latex", False)]},
        rendering={("docs-a", "paper/tex/source/main.tex"): {"working_directory": "paper"},
                   ("docs-a", "a/tex/source/x.tex"): {"working_directory": "a"},
                   ("docs-a", "via/tex/source/e.tex"): {"working_directory": "via"}}))
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("missing-working-directory")
def _(d, ids):
    root = d / "docs"
    write(root / "docs-a/tex/source/main.tex")
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]},
                                            rendering={("docs-a", "tex/source/main.tex"): {"working_directory": "gone"}}))
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("outputs")
def _(d, ids):
    root = d / "docs"
    project = root / "docs-a"
    sources = ["tex/source/main.tex", "tex/source/Main File.TEX", "tex/source/sub/b.tex", "tex/src2/main.tex",
               "markdown/source/x/y z.md", "notes/a.md", "tex/source/é.tex", "tex/source/main.tex.tex",
               "markdown/source/Ünï.markdown", "chapter.tex"]
    for source in sources:
        write(project / source, "x\n")
    write_configuration(root, configuration(ids, "p", {"docs-a": [
        (s, "markdown" if s.endswith(("md", "markdown")) else "latex", True) for s in sources]}))
    return [GENERATE_PROGRESS, CANDIDATES]


def rendering_error(options, kind="latex", source="tex/source/main.tex"):
    def build(d, ids):
        root = d / "docs"
        write(root / "docs-a" / source, "x\n")
        write(root / "docs-a/markdown/support/style.css", "x\n")
        write_configuration(root, configuration(ids, "p", {"docs-a": [(source, kind, True)]},
                                                rendering={("docs-a", source): options}))
        return [GENERATE_PROGRESS, CANDIDATES]
    return build


SCENARIOS["output-subdirectory"] = rendering_error({"output": "a/b.pdf"})
SCENARIOS["output-combined"] = rendering_error({"output": "a_combined.pdf"})
SCENARIOS["output-not-pdf"] = rendering_error({"output": "a.PDF"})
SCENARIOS["output-explicit"] = rendering_error({"output": "My Paper.pdf"})
SCENARIOS["stylesheet-latex"] = rendering_error({"stylesheet": "markdown/support/style.css"})
SCENARIOS["stylesheet-markdown"] = rendering_error({"stylesheet": "markdown/support/style.css"}, "markdown", "markdown/source/m.md")
SCENARIOS["stylesheet-not-css"] = rendering_error({"stylesheet": "markdown/source/m.md"}, "markdown", "markdown/source/m.md")
SCENARIOS["stylesheet-missing"] = rendering_error({"stylesheet": "markdown/support/none.css"}, "markdown", "markdown/source/m.md")
SCENARIOS["stylesheet-dotdot"] = rendering_error({"stylesheet": "../x.css"}, "markdown", "markdown/source/m.md")
SCENARIOS["working-directory-dotdot"] = rendering_error({"working_directory": "../x"})


@scenario("duplicate-outputs")
def _(d, ids):
    root = d / "docs"
    write(root / "docs-a/tex/source/a.tex")
    write(root / "docs-a/tex/source/b.tex")
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/a.tex", "latex", True),
                                                                  ("tex/source/b.tex", "latex", True)]},
                                            rendering={("docs-a", "tex/source/a.tex"): {"output": "X.pdf"},
                                                       ("docs-a", "tex/source/b.tex"): {"output": "x.pdf"}}))
    return [GENERATE_PROGRESS]


@scenario("missing-input")
def _(d, ids):
    root = d / "docs"
    (root / "docs-a").mkdir(parents=True)
    write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
    return [GENERATE_PROGRESS, CANDIDATES]


@scenario("output-is-directory")
def _(d, ids):
    root = d / "docs"
    paper(root, ids)
    (d / "out.json").mkdir()
    return [GENERATE]


@scenario("output-into-new-directories")
def _(d, ids):
    root = d / "docs"
    paper(root, ids)
    return [["generate-manifest", "--docs-root", "docs", "--output", "new/deeper/out.json"]]


@scenario("candidates")
def _(d, ids):
    root = d / "docs"
    for owner in ["docs-b", "docs-A2", "docs-c", "docs-registered", "docs-skipped", "docs-plain", "docs-ß", "docs-ss"]:
        write(root / owner / "tex/source/main.tex")
    write(root / "docs-c/tex/source/second.tex")
    write(root / "docs-file", "not a folder")
    write_configuration(root, configuration(ids, "p", {
        "docs-b": [("tex/source/main.tex", "latex", True)],
        "docs-A2": [("tex/source/main.tex", "latex", True)],
        "docs-c": [("tex/source/main.tex", "latex", True), ("tex/source/second.tex", "latex", True),
                   ("tex/source/off.tex", "latex", False)],
        "docs-registered": [("tex/source/main.tex", "latex", True)],
        "docs-skipped": [("tex/source/main.tex", "latex", True)]}))
    write(root / "current_pdfs/.thoughts-source-index.json",
          json.dumps({"schema_version": 1, "pdfs": [{"project": "docs-registered"}, "x", {"project": 3}]}))
    write(root / "library/source-project-policy.json",
          json.dumps({"schema_version": True, "excluded_projects": ["docs-skipped", "docs-"]}))
    return [CANDIDATES, GENERATE_PROGRESS]


def candidates_files(index=None, policy=None):
    def build(d, ids):
        root = d / "docs"
        write(root / "docs-a/tex/source/main.tex")
        write_configuration(root, configuration(ids, "p", {"docs-a": [("tex/source/main.tex", "latex", True)]}))
        if index is not None:
            write(root / "current_pdfs/.thoughts-source-index.json", index)
        if policy is not None:
            write(root / "library/source-project-policy.json", policy)
        return [CANDIDATES]
    return build


SCENARIOS["candidates-index-list"] = candidates_files(index="[]")
SCENARIOS["candidates-index-bom"] = candidates_files(index=b"\xef\xbb\xbf{}")
SCENARIOS["candidates-index-no-pdfs"] = candidates_files(index='{"pdfs": {}}')
SCENARIOS["candidates-policy-schema"] = candidates_files(policy='{"schema_version": 2, "excluded_projects": []}')
SCENARIOS["candidates-policy-slash"] = candidates_files(policy='{"schema_version": 1, "excluded_projects": ["docs-a/b"]}')
SCENARIOS["candidates-policy-float"] = candidates_files(policy='{"schema_version": 1.0, "excluded_projects": ["docs-a"]}')
SCENARIOS["candidates-policy-bad-json"] = candidates_files(policy='{"schema_version": 1,}')


def manifest_scenario(manifest, project="docs-a", *, raw=False, extra_setup=None):
    def build(d, ids):
        root = d / "docs"
        paper(root, ids, name="docs-a")
        write(root / "docs-other/tex/source/o.tex")
        write_configuration(root, configuration(ids, "q", {"docs-other": [("tex/source/o.tex", "latex", True)]}))
        text = manifest(root) if callable(manifest) else manifest
        write(d / "manifest.json", text if raw else json.dumps(text))
        if extra_setup:
            extra_setup(d, root)
        return [refresh(project), ["migrate-manifest", "--docs-root", "docs", "--manifest", "manifest.json"]]
    return build


def shared(root):
    return [{"path": p.relative_to(root).as_posix(), "role": "render_manifest"}
            for p in sorted((root / ".thoughts/projects").glob("*/*/thoughts-project.json"))
            if p.parent.name in (LEGACY_SEGMENT, CANONICAL_SEGMENT)]


SCENARIOS["refresh-b2"] = manifest_scenario(lambda root: (
    '{"zz":1,"schema_version":3,"projects":[{"files":[],"extra":"é","targets":["Z.PDF"],"project":"docs-a"},'
    '{"project":"docs-other","targets":["o.pdf","o.pdf"],"files":[{"path":"p","role":"r","x":1.5}]}],'
    '"shared_files":' + json.dumps(shared(root)) + ',"w":[1e20,1.0,10000000000000000000000,null,true,NaN,-Infinity,'
    '"\\ud800",1E400,-0.0,1e-7,123456789012345678]}'), raw=True)
SCENARIOS["refresh-duplicate-keys"] = manifest_scenario(
    '{"schema_version":3,"projects":[{"project":"docs-a","targets":[],"files":[]}],"a":1,"a":2}', raw=True)
SCENARIOS["refresh-bom"] = manifest_scenario(b'\xef\xbb\xbf{"schema_version":3,"projects":[]}', raw=True)
SCENARIOS["refresh-not-json"] = manifest_scenario("{", raw=True)
SCENARIOS["refresh-schema-float"] = manifest_scenario('{"schema_version":3.0,"projects":[{"project":"docs-a","targets":[],"files":[]}]}', raw=True)
SCENARIOS["refresh-schema-2"] = manifest_scenario({"schema_version": 2, "projects": []})
SCENARIOS["refresh-schema-true"] = manifest_scenario({"schema_version": True, "projects": []})
SCENARIOS["refresh-v1"] = manifest_scenario({"schema_version": 1, "files": ["a", "b"]})
SCENARIOS["refresh-v1-1.0"] = manifest_scenario('{"schema_version": 1.0, "files": []}', raw=True)
SCENARIOS["refresh-v1-bad"] = manifest_scenario({"schema_version": 1, "files": [1]})
SCENARIOS["refresh-v2"] = manifest_scenario({"schema_version": 2, "projects": [{"project": "x", "targets": [], "files": []}]})
SCENARIOS["refresh-v2-bad"] = manifest_scenario({"schema_version": 2, "projects": [{"project": "x", "targets": []}]})
SCENARIOS["refresh-list"] = manifest_scenario([])
SCENARIOS["refresh-no-project"] = manifest_scenario({"schema_version": 3, "projects": [{"project": "docs-other", "targets": [], "files": []}]})
SCENARIOS["refresh-two-entries"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}, {"project": "docs-a", "targets": [], "files": []}]})
SCENARIOS["refresh-duplicate-target"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-other", "targets": ["x.pdf"], "files": []}, {"project": "docs-a", "targets": ["x.pdf"], "files": []}]})
SCENARIOS["refresh-target-case"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": ["X.PDF", "ß.pdf", "a/b.pdf"], "files": []}]})
SCENARIOS["refresh-duplicate-paths"] = manifest_scenario(lambda root: {"schema_version": 3, "projects": [
    {"project": "docs-other", "targets": [], "files": [{"path": "docs-a/notes.md", "role": "source"}]},
    {"project": "docs-a", "targets": [], "files": []}]})
SCENARIOS["refresh-shared-bad"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}], "shared_files": [{"path": "x", "role": "render_manifest"}]})
SCENARIOS["refresh-shared-int"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}], "shared_files": [{"path": 3}]})
SCENARIOS["refresh-shared-dict"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}], "shared_files": {}})
SCENARIOS["refresh-other-files-str"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}, {"project": "docs-other", "targets": [], "files": ["x"]}]})
SCENARIOS["refresh-other-files-missing-path"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}, {"project": "docs-other", "targets": [], "files": [{"role": "x"}]}]})
SCENARIOS["refresh-other-files-list-path"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []}, {"project": "docs-other", "targets": [], "files": [{"path": []}]}]})
SCENARIOS["refresh-numeric-paths"] = manifest_scenario({"schema_version": 3, "projects": [
    {"project": "docs-a", "targets": [], "files": []},
    {"project": "docs-other", "targets": [], "files": [{"path": 1}, {"path": 1.0}, {"path": True}]}]})
SCENARIOS["migrate-projects-dict"] = manifest_scenario({"schema_version": 3, "projects": {"a": 1}})
SCENARIOS["migrate-projects-null"] = manifest_scenario({"schema_version": 3, "projects": None})
SCENARIOS["refresh-invalid-project"] = manifest_scenario({"schema_version": 3, "projects": []}, project="docs-")
SCENARIOS["refresh-project-slash"] = manifest_scenario({"schema_version": 3, "projects": []}, project="docs-a/b")
SCENARIOS["refresh-project-missing"] = manifest_scenario({"schema_version": 3, "projects": []}, project="docs-gone")
SCENARIOS["refresh-unconfigured"] = manifest_scenario({"schema_version": 3, "projects": []}, project="docs-plain",
                                                      extra_setup=lambda d, root: (root / "docs-plain").mkdir())


@scenario("refresh-missing-manifest")
def _(d, ids):
    paper(d / "docs", ids, name="docs-a")
    return [refresh("docs-a", "absent.json"), ["migrate-manifest", "--docs-root", "docs", "--manifest", "absent.json"]]


@scenario("refresh-real-roundtrip")
def _(d, ids):
    root = d / "docs"
    paper(root, ids, name="docs-a")
    write(root / "docs-other/tex/source/o.tex")
    write_configuration(root, configuration(ids, "q", {"docs-other": [("tex/source/o.tex", "latex", True)]}))
    return [GENERATE, refresh("docs-a", "out.json"), refresh("docs-other", "out.json")]


# ---------------------------------------------------------------- build inputs

LIBRARY = ["inspect-library-build-inputs", "--root", "docs", "--manifest", "out.json", "--desktop-progress-jsonl"]


def inspect(target, project=None, manifest="out.json"):
    command = ["inspect-build-inputs", "--root", "docs", "--manifest", manifest, "--target", target, "--desktop-progress-jsonl"]
    return command + (["--project", project] if project else [])


def build_project(root, ids, name="docs-paper", files=None, documents=None, ignore=None, pdfs=(), rendering=None):
    project = root / name
    for relative, data in (files or {}).items():
        target = root / relative[1:] if relative.startswith("/") else project / relative
        write(target, data)
    if ignore is not None:
        write(project / ".ThoughtIgnore", ignore)
    value = configuration(ids, "new:" + name, {name: documents or [("tex/source/main.tex", "latex", True)]},
                          pdfs=[p for p, _ in pdfs], rendering=rendering)
    for document in value["documents"]:
        if document["format"] == "pdf":
            data = dict(pdfs)[document["source"]["path"]]
            value["rendering"][document["document_id"]] = {"sha256": hashlib.sha256(data or b"").hexdigest()}
    path = write_configuration(root, value)
    for filename, data in pdfs:
        if data is not None:
            write(path.parent / filename, data)
    return value


APPENDIX_A = {
    "tex/source/main.tex": "\\PdfLink{ext-paper}{a}{b}\n% \\PdfLink{commented}{a}{b}\n\\includegraphics{fig}\n",
    "tex/source/fig.png": b"\x89PNG fake", "tex/source/sub/data.csv": "a,b\n", "tex/source/.hidden/x.png": b"hidden",
    "tex/generated/plot.pdf": b"%PDF-plot", "tex/generated/secret.pem": b"KEY", "tex/generated/build/x.png": b"excluded",
    "tex/generated/notes.log": b"ignored", "tex/generated/Thumbs.db": b"thumbs", "tex/generated/caf\u00e9 \u00fc.svg": "<svg/>",
    "tex/assets/logo.svg": "<svg/>", "tex/assets/data.tar.gz": b"tgz", "tex/source/page.html": "<html>\u00e9</html>",
    "tex/.DS_Store": b"x", "markdown/source/notes.md": "# Notes\n",
    "markdown/support/style.css": "@import \"other.css\";\n@font-face { src: url(fonts/a.ttf), url('fonts/b%20c.woff2'); }\n"
                                  "body{background:url(http://x/y.png)}\n",
    "markdown/support/other.css": "p{}\n", "markdown/support/fonts/a.ttf": b"ttf", "markdown/support/fonts/b c.woff2": b"woff2",
    "markdown/assets/pic.jpg": b"jpg", "img.PNG": b"png", "/third_party/ext-paper.pdf": b"%PDF-ext", "/current_pdfs/.keep": b"",
}


@scenario("inspect-appendix-a")
def _(d, ids):
    build_project(d / "docs", ids, files=APPENDIX_A, ignore=".git/\n*.log\n",
                  documents=[("tex/source/main.tex", "latex", True), ("markdown/source/notes.md", "markdown", True)],
                  pdfs=[("imported.pdf", b"%PDF-imported")])
    return [GENERATE, inspect("paper_main.pdf", "docs-paper"), inspect("paper_notes.pdf"), LIBRARY,
            ["inspect-library-build-inputs", "--root", "docs", "--manifest", "out.json"],
            inspect("paper_main.pdf", "docs-other"), inspect("missing.pdf"), inspect("paper_main.pdf", manifest="nowhere/m.json")]


ENTRY_POINTS = {
    "paper/main.tex": "\\documentclass{article}\n\\usepackage{m}\n\\begin{document}\n\\input{sections/a}\n\\end{document}\n",
    "paper/sections/a.tex": "A\n", "paper/figs/p.png": b"\x89PNG fake", "paper/data/s.csv": "a,b\n",
    "paper/figs/.hidden.csv": "h\n", "shared/m.sty": "\\ProvidesPackage{m}\n", "notes.txt": "n\n", "loose.TEX": "l\n",
    "build/x.tex": "x\n", "build/s.csv": "s\n", "pdf/out.pdf": b"%PDF-out", ".hidden/y.tex": "y\n", "private/z.tex": "z\n",
    "tex/generated/t.tex": "t\n", "tex/source/top.tex": "top\n", "tex/source/fig.pdf": b"%PDF-fig", "tex/odd/o.tex": "o\n",
    "/elsewhere/e.tex": "e\n",
}


@scenario("inspect-entry-points-anywhere")
def _(d, ids):
    root = d / "docs"
    build_project(root, ids, files=ENTRY_POINTS, ignore="private/\n",
                  documents=[("paper/main.tex", "latex", True), ("tex/source/top.tex", "latex", True)],
                  rendering={("docs-paper", "paper/main.tex"): {"working_directory": "paper"}})
    symlink(root / "docs-paper/linked", "../elsewhere")
    symlink(root / "docs-paper/paper/alias.tex", "main.tex")
    target = "paper_main_" + hashlib.sha256(b"paper/main.tex").hexdigest()[:12] + ".pdf"
    return [GENERATE_PROGRESS, CANDIDATES, inspect(target, "docs-paper"), inspect("paper_top.pdf"), LIBRARY]


def default_folder_project(root, ids, project_id, documents, *, files=None, pdfs=(), rendering=None, connected=None,
                           segment=LEGACY_SEGMENT):
    """A Project whose default folder declares documents: [(source, format, included)]."""
    value = configuration(ids, project_id, connected or {}, segment=segment)
    default_id = value["default_folder_id"]
    owner = value["source_folders"][0]["path"]
    for source, kind, included in documents:
        document_id = ids()
        value["documents"].append({"document_id": document_id, "source": {"folder_id": default_id, "path": source},
                                   "format": kind, "included": included})
        if source in (rendering or {}):
            value.setdefault("rendering", {})[document_id] = rendering[source]
    for filename, data in pdfs:
        document_id = ids()
        value["documents"].append({"document_id": document_id, "source": {"folder_id": default_id, "path": filename},
                                   "format": "pdf", "included": True})
        value.setdefault("rendering", {})[document_id] = {"sha256": hashlib.sha256(data).hexdigest()}
        write(root / owner / filename, data)
    for relative, data in (files or {}).items():
        write(root / relative[1:] if relative.startswith("/") else root / owner / relative, data)
    write_configuration(root, value)
    return owner


def default_output(owner, source):
    # Rename rule 4: the owner is hashed in its legacy spelling, so a PDF keeps
    # its name when documents/ becomes default/.
    if owner.endswith("/" + CANONICAL_SEGMENT):
        owner = owner[: -len(CANONICAL_SEGMENT)] + LEGACY_SEGMENT
    return "document_" + hashlib.sha256(f"{owner}/{source}".encode()).hexdigest()[:20] + ".pdf"


STANDALONE = "\\documentclass{article}\n\\begin{document}\\input{part}\\includegraphics{figure.pdf}\\end{document}\n"


def default_folder_documents(segment):
    def build(d, ids):
        root = d / "docs"
        owner = default_folder_project(root, ids, "new:alpha", [("notes.md", "markdown", True), ("chapters/paper.tex", "latex", True)],
                                       pdfs=[("imported.pdf", b"%PDF imported")], segment=segment,
                                       connected={"docs-alpha": [("tex/source/main.tex", "latex", True)]},
                                       files={"notes.md": "# Notes\n", "style.css": "p{}\n", "chapters/paper.tex": STANDALONE,
                                              "chapters/part.tex": "part\n", "chapters/refs.bib": "@misc{x}\n",
                                              "chapters/figure.pdf": b"%PDF figure", "stray.pdf": b"%PDF stray",
                                              "build/leftover.md": "# build\n", "pdf/old.md": "# old\n", ".DS_Store": b"\0",
                                              ".git/config": "g\n", "/docs-alpha/tex/source/main.tex": "main\n"})
        return [GENERATE_PROGRESS, CANDIDATES, inspect(default_output(owner, "notes.md"), owner),
                inspect(default_output(owner, "chapters/paper.tex")), LIBRARY]
    return build


def default_folder_two_projects(segments):
    def build(d, ids):
        root = d / "docs"
        for project_id, segment in zip(("new:first", "new:second"), segments):
            default_folder_project(root, ids, project_id, [("notes.md", "markdown", True)], files={"notes.md": "# Notes\n"},
                                   segment=segment)
        return [GENERATE, LIBRARY]
    return build


def default_folder_explicit_output(segment):
    def build(d, ids):
        root = d / "docs"
        owner = default_folder_project(root, ids, "new:named", [("notes.md", "markdown", True), ("draft.md", "markdown", False)],
                                       rendering={"notes.md": {"output": "my_notes.pdf"}}, segment=segment,
                                       files={"notes.md": "# Notes\n", "draft.md": "# Draft\n", ".ThoughtIgnore": "drafts/\n",
                                              "drafts/x.md": "# x\n", "build/kept.md": "# kept\n"})
        return [GENERATE, inspect("my_notes.pdf", owner), inspect(default_output(owner, "notes.md"), owner)]
    return build


def default_folder_only_pdfs(segment):
    def build(d, ids):
        root = d / "docs"
        default_folder_project(root, ids, "new:reading", [], pdfs=[("paper.pdf", b"%PDF")], segment=segment)
        return [GENERATE_PROGRESS, LIBRARY]
    return build


def default_folder_remote_note(segment):
    def build(d, ids):
        # Another device added the first note to a folder of imported PDFs:
        # the Project declares it before Sync has installed its file here.
        root = d / "docs"
        default_folder_project(root, ids, "new:reading", [("notes.md", "markdown", True)], pdfs=[("paper.pdf", b"%PDF")],
                               segment=segment)
        return [GENERATE, LIBRARY]
    return build


SCENARIOS["default-folder-documents"] = default_folder_documents(LEGACY_SEGMENT)
SCENARIOS["default-folder-two-projects"] = default_folder_two_projects((LEGACY_SEGMENT, LEGACY_SEGMENT))
SCENARIOS["default-folder-explicit-output"] = default_folder_explicit_output(LEGACY_SEGMENT)
SCENARIOS["default-folder-only-pdfs"] = default_folder_only_pdfs(LEGACY_SEGMENT)
SCENARIOS["default-folder-remote-note"] = default_folder_remote_note(LEGACY_SEGMENT)
# The same Projects after the rename: identical PDF names (rule 4).
SCENARIOS["canonical-default-folder-documents"] = default_folder_documents(CANONICAL_SEGMENT)
SCENARIOS["canonical-default-folder-two-projects"] = default_folder_two_projects((CANONICAL_SEGMENT, CANONICAL_SEGMENT))
SCENARIOS["canonical-default-folder-explicit-output"] = default_folder_explicit_output(CANONICAL_SEGMENT)
SCENARIOS["canonical-default-folder-only-pdfs"] = default_folder_only_pdfs(CANONICAL_SEGMENT)
SCENARIOS["canonical-default-folder-remote-note"] = default_folder_remote_note(CANONICAL_SEGMENT)
# One Library mid-migration: one Project renamed, the other not yet.
SCENARIOS["mixed-default-folder-two-projects"] = default_folder_two_projects((CANONICAL_SEGMENT, LEGACY_SEGMENT))


@scenario("canonical-default-folder-wrong-segment-manifest")
def _(d, ids):
    # The manifest must live inside the default folder it declares (rule 3).
    root = d / "docs"
    value = configuration(ids, "new:misplaced", {}, segment=CANONICAL_SEGMENT)
    write_configuration(root, value, segment=LEGACY_SEGMENT)
    return [GENERATE]


@scenario("canonical-default-folder-other-project-hash")
def _(d, ids):
    root = d / "docs"
    value = configuration(ids, "new:mine", {}, segment=CANONICAL_SEGMENT)
    value["source_folders"][0]["path"] = f".thoughts/projects/{project_hash('new:other')}/default"
    write_configuration(root, value)
    return [GENERATE]


@scenario("canonical-default-folder-invalid-segment")
def _(d, ids):
    root = d / "docs"
    value = configuration(ids, "new:mine", {}, segment=CANONICAL_SEGMENT)
    value["source_folders"][0]["path"] = f".thoughts/projects/{project_hash('new:mine')}/defaults"
    write_configuration(root, value, segment=CANONICAL_SEGMENT)
    return [GENERATE]


def pdflink(source, third_party=("ext",), name="pdflink"):
    def build(d, ids):
        files = {"tex/source/main.tex": source}
        for key in third_party:
            files[f"/third_party/{key}.pdf"] = b"%PDF"
        build_project(d / "docs", ids, files=files)
        return [inspect("paper_main.pdf", "docs-paper")]
    return build


SCENARIOS["pdflink-active"] = pdflink("\\PdfLink{ext}{t}{x}")
SCENARIOS["pdflink-spaces"] = pdflink("\\PdfLink  % c\n {  ext \n}")
SCENARIOS["pdflink-missing"] = pdflink("\\PdfLink{gone}")
SCENARIOS["pdflink-unsafe"] = pdflink("\\PdfLink{a/b}")
SCENARIOS["pdflink-dotdot"] = pdflink("\\PdfLink{..}")
SCENARIOS["pdflink-empty"] = pdflink("\\PdfLink{ }")
SCENARIOS["pdflink-literal"] = pdflink("\\code{\\PdfLink{gone}} \\verb|\\PdfLink{gone}| \\verb*+x+ "
                                       "\\begin{DocCode}\\PdfLink{gone}\\end{DocCode} \\string\\PdfLink{gone}"
                                       "\\begin{ verbatim }\\PdfLink{gone}\\end{verbatim} %\\PdfLink{gone}\n\\PdfLink{ext}")
SCENARIOS["pdflink-unterminated-verbatim"] = pdflink("\\begin{lstlisting}\\PdfLink{gone}")
SCENARIOS["pdflink-crlf-comment"] = pdflink("% c\r\\PdfLink{ext}\r\n%\\PdfLink{gone}\r\n")
SCENARIOS["pdflink-nested-braces"] = pdflink("\\PdfLink{e{x}t\\}}{x}", third_party=("e{x}t\\}",))
SCENARIOS["pdflink-verb-astral"] = pdflink("\\verb\U0001F600\\PdfLink{gone}\U0001F600\\PdfLink{ext}")
SCENARIOS["pdflink-unclosed"] = pdflink("\\PdfLink{ext")
SCENARIOS["pdflink-attachment-name"] = None


@scenario("pdflink-attachment-name")
def _(d, ids):
    build_project(d / "docs", ids, files={"tex/source/main.tex": "\\PdfLink{imported}"},
                  pdfs=[("imported.pdf", b"%PDF-i"), ("other.pdf", None)])
    return [inspect("paper_main.pdf", "docs-paper")]


@scenario("attachment-long-title")
def _(d, ids):
    build_project(d / "docs", ids, files={"tex/source/main.tex": "x"}, pdfs=[("a" * 210 + ".pdf", b"%PDF")])
    return [inspect("paper_main.pdf", "docs-paper")]


def html(data, name="tex/source/page.html"):
    def build(d, ids):
        build_project(d / "docs", ids, files={"tex/source/main.tex": "x", name: data})
        return [inspect("paper_main.pdf", "docs-paper")]
    return build


SCENARIOS["html-ok"] = html("<p>\u00e9</p>")
SCENARIOS["html-empty"] = html(b"")
SCENARIOS["html-invalid"] = html(b"<p>\xff</p>")
SCENARIOS["html-invalid-late"] = html(b"a" * 70000 + b"\xff")
SCENARIOS["html-split-sequence"] = html(b"a" * 65535 + "\u00e9".encode() + b"\xff")
SCENARIOS["html-hidden"] = html("x", name="tex/.hidden/page.html")
SCENARIOS["html-query-name"] = html("x", name="markdown/a#b.HTM")
SCENARIOS["html-upper"] = html("x", name="markdown/deep/A.HTML")


def css(stylesheet, extra=None):
    def build(d, ids):
        files = {"markdown/source/m.md": "# m\n", "markdown/support/style.css": stylesheet,
                 "markdown/support/fonts/a.ttf": b"a", "markdown/support/fonts/b b.woff2": b"b", "markdown/support/x.png": b"x",
                 "markdown/support/d.ttf": b"d", "markdown/support/e.ttf": b"e", "markdown/support/f": b"f", "markdown/support/g": b"g",
                 "markdown/support/other.css": "p{}", "markdown/top.otf": b"t", "fonts/root.ttf": b"r", "markdown/support/é.ttf": b"e"}
        files.update(extra or {})
        build_project(d / "docs", ids, name="docs-paper", files=files, documents=[("markdown/source/m.md", "markdown", True)])
        return [inspect("paper_m.pdf", "docs-paper")]
    return build


CSS_CASES = {
    "css-basic": "@font-face{src:url(fonts/a.ttf),url('fonts/b%20b.woff2')}",
    "css-bom": "\ufeff@import \"other.css\"; p{background:url(x.png)}",
    "css-bom-only": "\ufeff@import \"other.css\";",
    "css-unterminated-rule": "x url(fonts/a.ttf)",
    "css-unterminated-block": "p{background:url(x.png)",
    "css-bad-url": "p{a:url(x(.png)}",
    "css-mixed": "@import \"other.css\" screen; @IMPORT url(d.ttf); p{src:URL( \"e.ttf\" ) url(e\\2e ttf) url(\"f\" \"g\")}",
    "css-query": "p{src:url(fonts/a.ttf?#iefix)}",
    "css-parent": "p{src:url(../top.otf) url(../../fonts/root.ttf) url(../../../x)}",
    "css-absolute": "p{src:url(/etc/passwd) url(%2Fetc%2Fpasswd)}",
    "css-schemes": "p{src:url(https://x/a.ttf) url(data:font/ttf;base64,AA) url(C:fonts/a.ttf) url(//host/a.ttf)}",
    "css-ipv6-bad": "p{src:url(//[x)}",
    "css-ipv6-ipv4": "p{src:url(//[1.2.3.4]/a)}",
    "css-ipv6-ok": "p{src:url(//[::1]/a) url(fonts/a.ttf)}",
    "css-ipv6-junk": "p{src:url(//[zz]/a)}",
    "css-ipvfuture": "p{src:url(//[v1x]/a)}",
    "css-percent": "p{src:url(%C3%A9.ttf) url(%E9.ttf) url(%zz) url(fonts/a%2Ettf)}",
    "css-escapes": "p{src:url(fonts\\/a.ttf) url(\\66 onts/a.ttf) url(\"fonts\\\n/a.ttf\")}",
    "css-comments": "/* url(x.png) */ p{src:url(fonts/a.ttf)} /* unterminated url(d.ttf)",
    "css-strings-newline": "p{src:url(\"fonts/a.ttf\nx\") url(d.ttf)}",
    "css-nested": "@media screen { p { src: url(d.ttf) } } @supports (x: url(e.ttf)) { }",
    "css-unicode-range": "@font-face{unicode-range:U+0025-00FF, u+4??; src:url(fonts/a.ttf)}",
    "css-numbers": "p{margin:-1.5e3px +.5% 1.2.3 5e url(d.ttf)}",
    "css-hash": "#url(x.png){} p{src:url(fonts/a.ttf)}",
    "css-cdo": "<!-- p{src:url(d.ttf)} -->",
    "css-unmatched": "} ] ) p{src:url(d.ttf)}",
    "css-whitespace-url": "p{src:url(  fonts/a.ttf  ) url( d.ttf x)}",
    "css-control": "p{src:url(d\x01.ttf) url(e.ttf)}",
    "css-unicode-name": "p{src:url(é.ttf)}",
    "css-symlink": "p{src:url(linked.ttf)}",
    "css-null": "p{src:url(d\x00.ttf) url(e.ttf)}",
    "css-cr": "p{src:url(d.ttf)}\r\rq{src:url(e.ttf)}",
    "css-strip": "p{src:url(\" \u00a0e.ttf\u2003\")}",
}
for _name, _css in CSS_CASES.items():
    SCENARIOS[_name] = css(_css, {"markdown/support/linked.ttf": None} if _name == "css-symlink" else None)


@scenario("css-symlink")
def _(d, ids):
    build = css("p{src:url(linked.ttf) url(fonts/a.ttf)}")
    commands = build(d, ids)
    symlink(d / "docs/docs-paper/markdown/support/linked.ttf", "fonts/a.ttf")
    return commands


@scenario("build-object-roots")
def _(d, ids):
    files = {"tex/source/main.tex": "x"}
    for relative in ["tex/generated/a/b/c.svg", "tex/generated/a/x.pdf", "tex/generated/z/y.png", "tex/generated/top.eps",
                     "tex/generated/.hidden", "tex/assets/deep/er/f.bin", "tex/assets/.env.local", "tex/assets/.env.prod",
                     "tex/assets/id_rsa", "tex/assets/key.PEM", "tex/assets/cert.p12", "tex/assets/Desktop.ini",
                     "tex/assets/tmp/t.png", "tex/assets/node_modules/m.js", "tex/assets/venv", "paper/tex/generated/w.pdf",
                     "tex/source/a.dat", "tex/source/b.EPS", "tex/source/c.txt", "tex/source/.git/x.png", "tex/source/deep/fig.JPG"]:
        files[relative] = relative.encode()
    build_project(d / "docs", ids, files=files,
                  documents=[("tex/source/main.tex", "latex", True), ("paper/tex/source/p.tex", "latex", True)],
                  rendering={("docs-paper", "paper/tex/source/p.tex"): {"working_directory": "paper"}})
    write(d / "docs/docs-paper/paper/tex/source/p.tex", "p")
    write(d / "outside/o.png", b"o")
    symlink(d / "docs/docs-paper/tex/generated/linked.png", d / "outside/o.png")
    symlink(d / "docs/docs-paper/tex/generated/linkdir", d / "outside")
    symlink(d / "docs/docs-paper/tex/assets/inside.png", "../generated/top.eps")
    return [GENERATE, LIBRARY, inspect("paper_main.pdf", "docs-paper")]


@scenario("build-ignore")
def _(d, ids):
    build_project(d / "docs", ids, files={"tex/source/main.tex": "x", "tex/generated/a.log": b"l", "tex/generated/keep.pdf": b"k",
                                          "tex/generated/sub/b.pdf": b"b", "img/x.png": b"x"},
                  ignore="*.log\nsub/\n/img\n")
    return [inspect("paper_main.pdf", "docs-paper")]


@scenario("library-errors")
def _(d, ids):
    build_project(d / "docs", ids)
    write(d / "bad1.json", '{"schema_version": 2, "projects": []}')
    write(d / "bad2.json", '{"schema_version": 3, "projects": [{"project": "docs-paper", "targets": ["paper_main.pdf"], "files": [1]}]}')
    write(d / "bad3.json", '{"schema_version": 3, "projects": [{"project": "docs-paper", "targets": ["x.PDF"], "files": []}]}')
    write(d / "bad4.json", '{"schema_version": 3, "projects": [{"project": "docs-paper", "targets": ["other.pdf"], "files": []}]}')
    write(d / "bad5.json", '{"schema_version": 3, "projects": [{"project": "", "targets": [], "files": []}]}')
    write(d / "bad6.json", '{"schema_version": 3, "projects": [{"project": "docs-paper", "targets": [], "files": '
                           '[{"path": "a", "role": "s"}, {"path": "a", "role": "s"}]}]}')
    write(d / "ok.json", '{"schema_version": 3, "projects": [{"project": "docs-paper", "targets": ["paper_main.pdf"], "files": []}]}')
    return [["inspect-library-build-inputs", "--root", "docs", "--manifest", name] for name in
            ["bad1.json", "bad2.json", "bad3.json", "bad4.json", "bad5.json", "bad6.json", "ok.json", "missing.json"]]


@scenario("inspect-registration")
def _(d, ids):
    root = d / "docs"
    build_project(root, ids, files={"tex/source/main.tex": "p"})
    build_project(root, ids, name="docs-twin", files={"tex/source/main.tex": "t"},
                  rendering={("docs-twin", "tex/source/main.tex"): {"output": "paper_main.pdf"}})
    build_project(root, ids, name="docs-B", files={"tex/source/main.tex": "b"})
    write(root / "library/source-project-policy.json", json.dumps({"schema_version": 1, "excluded_projects": ["docs-B"]}))
    return [inspect("paper_main.pdf"), inspect("paper_main.pdf", "docs-twin"), inspect("B_main.pdf", "docs-B"), inspect("twin_main.pdf"),
            inspect("paper_main.pdf", "docs-"), inspect("../x.pdf", "docs-paper")]


# Command lines: argparse behaviour without touching files.
ARGV_CASES = [
    [], ["-h"], ["--help"], ["bogus"], ["Generate-Manifest"], ["generate-manifest"], ["generate-manifest", "-h"],
    ["refresh-project-manifest", "--help"], ["migrate-manifest", "-h"], ["list-candidates", "-h"],
    ["inspect-build-inputs", "-h"], ["inspect-library-build-inputs", "-h"],
    ["generate-manifest", "--docs-root", "x"], ["generate-manifest", "--output"],
    ["generate-manifest", "--docs-root", "-x", "--output", "o"], ["generate-manifest", "--d", "x"],
    ["generate-manifest", "--docs", "missing-dir", "--out", "o.json", "--desk"],
    ["generate-manifest", "--docs-root=missing-dir", "--output=o.json", "--desktop-progress-jsonl=1"],
    ["generate-manifest", "--docs-root=missing-dir", "--output=o.json", "--desktop-progress-jsonl="],
    ["generate-manifest", "--docs-root", "a", "--output", "o.json", "extra"],
    ["generate-manifest", "--docs-root", "a", "--output", "o.json", "--", "x"],
    ["generate-manifest", "--docs-root", "--", "--output", "o.json"],
    ["generate-manifest", "-x", "--docs-root", "a", "--output", "o.json"],
    ["generate-manifest", "-5", "--docs-root", "a", "--output", "o.json"],
    ["generate-manifest", "--docs-root", "-5", "--output", "o.json"],
    ["generate-manifest", "--docs-root", "-x y", "--output", "o.json"],
    ["generate-manifest", "-hx"], ["generate-manifest", "-h=1"], ["generate-manifest", "--he"],
    ["generate-manifest", "--docs-root=a", "--docs-root", "b", "--output", "o.json", "--bogus"],
    ["list-candidates", "--bogus"], ["list-candidates", "--docs-root", "nowhere", "--bogus"],
    ["list-candidates", "--docs-root", "nowhere", "--desktop-progress-jsonl"],
    ["list-candidates", "--docs-root", "nowhere", "-", "x y"],
    ["list-candidates", "--docs-root", ""], ["list-candidates", "--docs-root="],
    ["migrate-manifest", "--docs-root", "x"],
    ["refresh-project-manifest", "--docs-root", "x", "--manifest", "m", "--output", "o"],
    ["--root", "r"], ["--p", "x"], ["--root=r", "--manifest", "m", "--state", "s"], ["once"],
    ["generate-manifest", "--output", "o.json", "--docs-root", "a", "-h"],
    ["generate-manifest", "-h", "--d"], ["generate-manifest", "--d", "-h"],
]


@scenario("argv")
def _(d, ids):
    return ARGV_CASES


# ---------------------------------------------------------------- running

GOLDEN = Path(__file__).resolve().parent / "golden" / "publisher"


def normalise(data, roots):
    text = data.decode("utf-8", "surrogateescape")
    for root in roots:
        text = text.replace(root, "<ROOT>")
    return text


def normalise_stderr(text):
    lines = text.splitlines()
    progress = [line for line in lines if line.startswith(PROGRESS)]
    rest = [line for line in lines if not line.startswith(PROGRESS)]
    # mkstemp's random suffix.
    return [re.sub(r"(\.[^/' ]+\.json\.)[a-z0-9_]{8}", r"\1XXXXXXXX", line) for line in progress + rest]


def snapshot(directory):
    files = {}
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory).as_posix()
        if relative.startswith("docs/") and not relative.startswith("docs/.out"):
            continue
        if path.is_symlink() or not path.is_file():
            continue
        files[relative] = (path.read_bytes(), stat.S_IMODE(path.stat().st_mode))
    return files


def observe(native, build, base):
    """Run one scenario and return its normalised, JSON-serialisable outcome."""
    environment = dict(os.environ)
    for variable in ("COLUMNS", "FORCE_COLOR", "PYTHON_COLORS", "NO_COLOR"):
        environment.pop(variable, None)
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    roots = sorted({str(base), os.path.realpath(base)}, key=len, reverse=True)
    commands = build(base, Ids(7))
    before = snapshot(base)
    runs = []
    for arguments in commands:
        result = subprocess.run([native, *arguments], cwd=base, capture_output=True, env=environment)
        runs.append({"arguments": [str(argument) for argument in arguments], "exit": result.returncode,
                     "stdout": normalise(result.stdout, roots),
                     "stderr": normalise_stderr(normalise(result.stderr, roots))})
    after = snapshot(base)
    files = {key: {"content": normalise(data, roots), "mode": oct(mode)}
             for key, (data, mode) in after.items() if before.get(key) != (data, mode)}
    return {"runs": runs, "files": files}


def differences(want, got):
    problems = []
    if len(want["runs"]) != len(got["runs"]):
        problems.append(f"{len(got['runs'])} runs, expected {len(want['runs'])}")
    for expected, actual in zip(want["runs"], got["runs"]):
        label = " ".join(expected["arguments"])
        for key in ("arguments", "exit", "stdout"):
            if expected[key] != actual[key]:
                problems.append(f"{label}: {key}\n    expected: {expected[key]!r:.400}\n    native:   {actual[key]!r:.400}")
        for index in range(max(len(expected["stderr"]), len(actual["stderr"]))):
            left = expected["stderr"][index] if index < len(expected["stderr"]) else None
            right = actual["stderr"][index] if index < len(actual["stderr"]) else None
            if left != right:
                problems.append(f"{label}: stderr line {index}\n    expected: {left!r}\n    native:   {right!r}")
                break
    for key in sorted(set(want["files"]) | set(got["files"])):
        if want["files"].get(key) != got["files"].get(key):
            problems.append(f"file {key}\n    expected: {want['files'].get(key)!r:.400}\n    native:   {got['files'].get(key)!r:.400}")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--only")
    parser.add_argument("--record", action="store_true", help="rewrite the expected outcomes from the native publisher")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    native = os.path.abspath(args.native)
    scenarios = dict(SCENARIOS)
    if args.only:
        scenarios = {k: v for k, v in scenarios.items() if re.fullmatch(args.only, k)}
    work = Path(tempfile.mkdtemp(prefix="publisher-golden-"))
    failures = runs = 0
    for name, build in scenarios.items():
        outcome = observe(native, build, work / name)
        runs += len(outcome["runs"])
        expected_path = GOLDEN / f"{name}.json"
        if args.record:
            GOLDEN.mkdir(parents=True, exist_ok=True)
            expected_path.write_text(json.dumps(outcome, indent=1, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
            problems = []
        elif not expected_path.is_file():
            problems = [f"no expected outcome {expected_path}; run with --record"]
        else:
            problems = differences(json.loads(expected_path.read_text(encoding="utf-8")), outcome)
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for problem in problems[:6]:
                print("  " + problem)
        elif not args.keep:
            shutil.rmtree(work / name, ignore_errors=True)
    if args.record and not args.only:
        stale = sorted(path.name for path in GOLDEN.glob("*.json") if path.stem not in scenarios)
        for name in stale:
            (GOLDEN / name).unlink()
        if stale:
            print(f"removed {len(stale)} expected outcomes of deleted scenarios")
    verb = "recorded" if args.record else "match"
    print(f"{len(scenarios) - failures}/{len(scenarios)} scenarios {verb} ({runs} runs); work in {work}")
    if not args.keep and failures == 0:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
