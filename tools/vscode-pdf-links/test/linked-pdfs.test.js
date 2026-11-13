// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const {
  mappingsFromLayout,
  mergeMappings,
  normalizedPath
} = require("../linked-pdfs");

function writeFile(filePath, contents = "") {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, contents);
}

function withFixture(callback) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "thoughts-links-"));
  try {
    callback(root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

function testValidLayout() {
  withFixture((root) => {
    const sourcePath = path.join(
      root,
      "docs-project",
      "tex",
      "source",
      "note with spaces.tex"
    );
    const pdfPath = path.join(root, "current_pdfs", "project.pdf");
    const layoutPath = path.join(
      root,
      "comments",
      ".layout",
      "project.json"
    );
    writeFile(sourcePath);
    writeFile(pdfPath);

    const mappings = mappingsFromLayout(
      layoutPath,
      JSON.stringify({
        version: 1,
        sources: [
          {
            source_id:
              "docs-project/tex/source/note with spaces.tex"
          },
          { source_id: "docs-project/pdf/external.pdf" }
        ]
      }),
      root
    );

    assert.deepStrictEqual(mappings, [
      {
        sourcePath: normalizedPath(sourcePath),
        pdfPath: normalizedPath(pdfPath)
      }
    ]);
  });
}

function testUnavailableAndUnsafeMappingsAreIgnored() {
  withFixture((root) => {
    const layoutPath = path.join(
      root,
      "comments",
      ".layout",
      "project.json"
    );
    writeFile(path.join(root, "current_pdfs", "project.pdf"));
    writeFile(path.join(root, "outside.tex"));

    const missingSource = mappingsFromLayout(
      layoutPath,
      JSON.stringify({
        version: 1,
        sources: [
          { source_id: "docs-project/tex/source/missing.tex" },
          { source_id: "../outside.tex" }
        ]
      }),
      root
    );
    assert.deepStrictEqual(missingSource, []);

    const missingPdf = mappingsFromLayout(
      path.join(root, "comments", ".layout", "absent.json"),
      JSON.stringify({
        version: 1,
        sources: [
          { source_id: "docs-project/tex/source/missing.tex" }
        ]
      }),
      root
    );
    assert.deepStrictEqual(missingPdf, []);

    const unsupportedVersion = mappingsFromLayout(
      layoutPath,
      JSON.stringify({ version: 2, sources: [] }),
      root
    );
    assert.deepStrictEqual(unsupportedVersion, []);
  });
}

function testMultipleLayoutsAreDeduplicatedAndSorted() {
  const sourcePath = "/tmp/docs-project/tex/source/note.tex";
  const firstPdf = "/tmp/current_pdfs/first.pdf";
  const secondPdf = "/tmp/current_pdfs/second.pdf";
  const merged = mergeMappings([
    [
      { sourcePath, pdfPath: secondPdf },
      { sourcePath, pdfPath: firstPdf }
    ],
    [{ sourcePath, pdfPath: firstPdf }]
  ]);

  assert.deepStrictEqual(merged.get(normalizedPath(sourcePath)), [
    normalizedPath(firstPdf),
    normalizedPath(secondPdf)
  ]);
}

function testMalformedJsonIsReported() {
  assert.throws(
    () =>
      mappingsFromLayout(
        "/tmp/comments/.layout/project.json",
        "{not json",
        "/tmp"
      ),
    SyntaxError
  );
}

testValidLayout();
testUnavailableAndUnsafeMappingsAreIgnored();
testMultipleLayoutsAreDeduplicatedAndSorted();
testMalformedJsonIsReported();
process.stdout.write("Linked PDF mapping tests passed\n");
