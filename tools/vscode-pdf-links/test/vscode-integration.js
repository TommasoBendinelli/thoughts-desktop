// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

// Run with Code's --extensionTestsPath and this folder as --extensionDevelopmentPath.
const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const vscode = require("vscode");

exports.run = async function run() {
  const docsRoot = fs.realpathSync(
    fs.mkdtempSync(path.join(os.tmpdir(), "thoughts-source-links-vscode-"))
  );
  const write = (relative, contents) => {
    const file = path.join(docsRoot, ...relative.split("/"));
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, contents);
    return file;
  };
  try {
    fs.mkdirSync(path.join(docsRoot, ".thoughts", "projects"), { recursive: true });
    write("third_party/paper.pdf", "%PDF-1.4\n");
    write(
      "docs-template/tex/source/thoughts_manual.tex",
      "\\input{tex/fragments/manual/02_library.tex}\n"
    );
    const library = write(
      "docs-template/tex/fragments/manual/02_library.tex",
      "\\subsection{Links}\n\\SourceLabel{links-between-documents}\n"
    );
    const source = write(
      "docs-template/tex/source/internals.tex",
      "See \\SourceLink{tex/source/thoughts_manual.tex}{links-between-documents}{Links}.\n" +
        "Also \\PdfLink{paper}{paper-title}{the paper}.\n"
    );

    const extension = vscode.extensions.getExtension("local.thoughts-pdf-links");
    assert.ok(extension, "extension is loaded");
    await extension.activate();

    const document = await vscode.workspace.openTextDocument(source);
    for (const character of [4, 20, 55, 78]) {
      const definitions = await vscode.commands.executeCommand(
        "vscode.executeDefinitionProvider",
        document.uri,
        new vscode.Position(0, character)
      );
      const ours = definitions.filter(
        (definition) => (definition.targetUri || definition.uri).fsPath === library
      );
      assert.strictEqual(ours.length, 1, `definition at character ${character}`);
      const range = ours[0].targetSelectionRange || ours[0].range;
      assert.strictEqual(range.start.line, 1);
      assert.strictEqual(range.start.character, 13);
      assert.strictEqual(ours[0].originSelectionRange.start.character, 4);
    }
    const outside = await vscode.commands.executeCommand(
      "vscode.executeDefinitionProvider",
      document.uri,
      new vscode.Position(0, 1)
    );
    assert.ok(
      !outside.some((definition) => (definition.targetUri || definition.uri).fsPath === library)
    );

    const links = await vscode.commands.executeCommand(
      "vscode.executeLinkProvider",
      document.uri
    );
    const pdfLinks = links.filter((link) => link.target && link.target.fsPath.endsWith("paper.pdf"));
    assert.strictEqual(pdfLinks.length, 1);
    assert.strictEqual(pdfLinks[0].range.start.line, 1);
    assert.ok(
      !links.some((link) => link.target && link.target.fsPath.endsWith(".tex")),
      "source links have no document link"
    );
  } finally {
    fs.rmSync(docsRoot, { recursive: true, force: true });
  }
};
