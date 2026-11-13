// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const {
  findLinks,
  linkAt,
  maskTexComments,
  ownerOf,
  resolveLink,
  sourceContext
} = require("../source-links");

const docsRoot = fs.realpathSync(
  fs.mkdtempSync(path.join(os.tmpdir(), "thoughts-source-links-"))
);
const projectHash = "a".repeat(64);

function write(relative, contents) {
  const file = path.join(docsRoot, ...relative.split("/"));
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, contents);
  return file;
}

try {
  write(".thoughts/projects/placeholder/.keep", "");
  write(
    `.thoughts/projects/${projectHash}/documents/thoughts-project.json`,
    JSON.stringify({
      source_folders: [
        { folder_id: "default", path: `.thoughts/projects/${projectHash}/documents` },
        { folder_id: "template", path: "docs-template" }
      ],
      documents: [
        {
          source: { folder_id: "default", path: "papers/paper-key.pdf" },
          format: "pdf",
          included: true
        },
        {
          source: { folder_id: "template", path: "tex/source/thoughts_manual.tex" },
          format: "latex",
          included: true
        }
      ]
    })
  );
  write(`.thoughts/projects/${projectHash}/documents/papers/paper-key.pdf`, "%PDF-1.4\n");
  write("third_party/shared-paper.pdf", "%PDF-1.4\n");

  const manual = write(
    "docs-template/tex/source/thoughts_manual.tex",
    [
      "\\documentclass{article}",
      "\\begin{document}",
      "% \\input{tex/fragments/manual/00_commented.tex}",
      "\\input{tex/fragments/manual/01_intro}",
      "\\input{tex/fragments/manual/02_library.tex}",
      "\\end{document}",
      ""
    ].join("\n")
  );
  write(
    "docs-template/tex/fragments/manual/01_intro.tex",
    "\\section{Intro}\n\\label{sec:intro}\n"
  );
  const library = write(
    "docs-template/tex/fragments/manual/02_library.tex",
    [
      "\\section{Library}",
      "% \\SourceLabel{links-between-documents}",
      "\\subsection{Links between documents}",
      "  \\SourceLabel{links-between-documents}%",
      ""
    ].join("\n")
  );
  const internals = write(
    "docs-template/tex/fragments/internals/04_projects_rendering.tex",
    "See \\SourceLink{tex/source/thoughts_manual.tex}{links-between-documents}{Links\nbetween {documents}}.\n"
  );
  const notes = write(
    "docs-concepts/notes/grpo.md",
    "# GRPO\n\n## Objective {#source-label:doc:grpo .unnumbered}\n"
  );
  const aliased = write(
    "docs-concepts/tex/source/olmo3.tex",
    "\\section{OLMo 3}\\SourceLabel{doc:olmo3}\n"
  );
  const models = write(
    "docs-models/tex/source/olmo3.tex",
    [
      "\\CrossSourceLink{docs-concepts/notes/grpo.md}{doc:grpo}{GRPO}",
      "\\CrossSourceLink{docs-concepts/olmo3.tex}{doc:olmo3}{OLMo}",
      "\\TexLink{docs-concepts/olmo3.tex}{doc:olmo3}{OLMo} \\CrossTexLink{docs-concepts/olmo3.tex}{document-start}{OLMo}",
      "\\SourceLink{tex/source/missing.tex}{x}{missing}",
      "\\SourceLink{tex/source/olmo3.tex}{no-such-label}{self}",
      "\\CrossPdfLink{docs-template/paper-key}{document-start}{paper}",
      "\\PdfLink{shared-paper}{paper-title}{shared}",
      "% \\SourceLink{tex/source/olmo3.tex}{commented}{ignored}",
      "\\code{\\SourceLink{sourcePath}{name}{visibleText}}",
      ""
    ].join("\n")
  );

  // Owners and contexts.
  assert.strictEqual(ownerOf("docs-concepts/tex/source/a.tex"), "docs-concepts");
  assert.strictEqual(
    ownerOf(`.thoughts/projects/${projectHash}/documents/a.tex`),
    `.thoughts/projects/${projectHash}/documents`
  );
  assert.strictEqual(ownerOf("other/a.tex"), null);
  assert.deepStrictEqual(sourceContext(internals), {
    docsRoot,
    owner: "docs-template"
  });

  // Comments are masked without moving offsets.
  const masked = maskTexComments("a % b\n\\% c");
  assert.strictEqual(masked, "a    \n\\% c");

  // Parsing: multi-line text argument and click positions.
  const internalsText = fs.readFileSync(internals, "utf8");
  const [sourceLink] = findLinks(internalsText);
  assert.strictEqual(sourceLink.macro, "SourceLink");
  assert.strictEqual(sourceLink.target, "tex/source/thoughts_manual.tex");
  assert.strictEqual(sourceLink.label, "links-between-documents");
  assert.strictEqual(internalsText.slice(sourceLink.end - 2, sourceLink.end), "}}");
  assert.deepStrictEqual(linkAt(internalsText, sourceLink.start), sourceLink);
  assert.deepStrictEqual(linkAt(internalsText, internalsText.indexOf("between {")), sourceLink);
  assert.strictEqual(linkAt(internalsText, 0), null);
  assert.strictEqual(linkAt(internalsText, sourceLink.end + 1), null);

  const modelLinks = findLinks(fs.readFileSync(models, "utf8"));
  assert.deepStrictEqual(
    modelLinks.map((link) => link.macro),
    [
      "CrossSourceLink",
      "CrossSourceLink",
      "SourceLink",
      "SourceLink",
      "CrossPdfLink",
      "PdfLink",
      "SourceLink"
    ],
    "commented links and the retired TexLink/CrossTexLink are skipped"
  );

  // SourceLink: owner-relative path, label found through \input.
  assert.deepStrictEqual(resolveLink(sourceLink, internals), {
    kind: "source",
    file: library,
    labelFound: true,
    start: { line: 3, character: 15 },
    end: { line: 3, character: 38 }
  });

  // CrossSourceLink to a Markdown heading identifier.
  assert.deepStrictEqual(resolveLink(modelLinks[0], models), {
    kind: "source",
    file: notes,
    labelFound: true,
    start: { line: 2, character: 15 },
    end: { line: 2, character: 36 }
  });

  // A single file name aliases tex/source/<name>.
  assert.deepStrictEqual(resolveLink(modelLinks[1], models), {
    kind: "source",
    file: aliased,
    labelFound: true,
    start: { line: 0, character: 29 },
    end: { line: 0, character: 38 }
  });

  // An ordinary \label is no Source label: the file opens at its top.
  const [texLabel] = findLinks("\\SourceLink{tex/source/thoughts_manual.tex}{sec:intro}{x}");
  assert.deepStrictEqual(resolveLink(texLabel, internals), {
    kind: "source",
    file: manual,
    labelFound: false,
    start: { line: 0, character: 0 },
    end: { line: 0, character: 0 }
  });

  // Missing file: no target. Missing label: the file itself.
  assert.strictEqual(resolveLink(modelLinks[2], models), null);
  assert.deepStrictEqual(resolveLink(modelLinks[3], models), {
    kind: "source",
    file: models,
    labelFound: false,
    start: { line: 0, character: 0 },
    end: { line: 0, character: 0 }
  });

  // PDF links: Project-selected PDF of the owner, then third_party.
  assert.deepStrictEqual(resolveLink(modelLinks[4], models), {
    kind: "pdf",
    file: path.join(docsRoot, ".thoughts", "projects", projectHash, "documents", "papers", "paper-key.pdf")
  });
  assert.deepStrictEqual(resolveLink(modelLinks[5], models), {
    kind: "pdf",
    file: path.join(docsRoot, "third_party", "shared-paper.pdf")
  });

  // Placeholder arguments and unsafe paths do not resolve.
  assert.strictEqual(resolveLink(modelLinks[6], models), null);
  const [unsafe] = findLinks("\\SourceLink{../docs-concepts/notes/grpo.md}{doc:grpo}{x}");
  assert.strictEqual(resolveLink(unsafe, models), null);

  // Unsaved editor text is used when provided.
  const unsaved = resolveLink(sourceLink, internals, (file) =>
    file === manual ? "\\SourceLabel{links-between-documents}\n" : fs.readFileSync(file, "utf8")
  );
  assert.strictEqual(unsaved.file, manual);
  assert.deepStrictEqual(unsaved.start, { line: 0, character: 13 });

  // Default Project folders are source owners too.
  const defaultSource = write(
    `.thoughts/projects/${projectHash}/documents/notes.tex`,
    "\\SourceLink{target.tex}{here}{x}\n"
  );
  const defaultTarget = write(
    `.thoughts/projects/${projectHash}/documents/target.tex`,
    "\\SourceLabel{here}\n"
  );
  const [defaultLink] = findLinks(fs.readFileSync(defaultSource, "utf8"));
  assert.strictEqual(resolveLink(defaultLink, defaultSource).file, defaultTarget);

  // The canonical default/ spelling (2026-10-17 rename) works the same way.
  const canonicalHash = "b".repeat(64);
  const canonical = `.thoughts/projects/${canonicalHash}/default`;
  assert.strictEqual(ownerOf(`${canonical}/a.tex`), canonical);
  assert.strictEqual(ownerOf(`.thoughts/projects/${canonicalHash}/defaults/a.tex`), null);
  write(
    `${canonical}/thoughts-project.json`,
    JSON.stringify({
      source_folders: [{ folder_id: "default", path: canonical }],
      documents: [
        { source: { folder_id: "default", path: "canonical-paper.pdf" }, format: "pdf", included: true }
      ]
    })
  );
  write(`${canonical}/canonical-paper.pdf`, "%PDF-1.4\n");
  const canonicalSource = write(
    `${canonical}/notes.tex`,
    "\\SourceLink{target.tex}{here}{x} \\PdfLink{canonical-paper}{document-start}{p}\n"
  );
  const canonicalTarget = write(`${canonical}/target.tex`, "\\SourceLabel{here}\n");
  assert.deepStrictEqual(sourceContext(canonicalSource), { docsRoot, owner: canonical });
  const [canonicalLink, canonicalPdf] = findLinks(fs.readFileSync(canonicalSource, "utf8"));
  assert.strictEqual(resolveLink(canonicalLink, canonicalSource).file, canonicalTarget);
  assert.deepStrictEqual(resolveLink(canonicalPdf, canonicalSource), {
    kind: "pdf",
    file: path.join(docsRoot, ".thoughts", "projects", canonicalHash, "default", "canonical-paper.pdf")
  });
} finally {
  fs.rmSync(docsRoot, { recursive: true, force: true });
}

process.stdout.write("Source link tests passed\n");
