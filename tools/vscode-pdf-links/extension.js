// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const {
  mappingsFromLayout,
  mergeMappings,
  normalizedPath
} = require("./linked-pdfs");
const { openPdfInThoughts } = require("./open-thoughts");
const { findLinks, linkAt, resolveLink } = require("./source-links");

const viewType = "thoughts.externalPdf";
const openLinkedPdfCommand = "thoughts.openLinkedPdf";
const linkedTexPathsContext = "thoughts.linkedTexPaths";

class ExternalPdfDocument {
  constructor(uri) {
    this.uri = uri;
  }

  dispose() {}
}

class ExternalPdfEditorProvider {
  constructor(output) {
    this.output = output;
  }

  async openCustomDocument(uri) {
    return new ExternalPdfDocument(uri);
  }

  async resolveCustomEditor(document, webviewPanel) {
    try {
      if (document.uri.scheme !== "file") {
        throw new Error(
          `Only local PDF files are supported; received ${document.uri.toString()}`
        );
      }

      this.output.appendLine(`Opening ${document.uri.fsPath}`);
      await openPdfInThoughts(document.uri.fsPath);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.output.appendLine(`Failed: ${message}`);
      void vscode.window.showErrorMessage(
        `Could not open the PDF in Thoughts: ${message}`
      );
    } finally {
      // The custom editor is only a routing hook. Do not leave an empty tab.
      webviewPanel.dispose();
    }
  }
}

const linkDocumentSelector = [
  { scheme: "file", language: "latex" },
  { scheme: "file", language: "tex" },
  { scheme: "file", pattern: "**/*.tex" }
];

// Reads a link target, preferring unsaved editor contents.
function readLinkTarget(file) {
  const open = vscode.workspace.textDocuments.find(
    (document) =>
      document.uri.scheme === "file" &&
      normalizedPath(document.uri.fsPath) === normalizedPath(file)
  );
  return open ? open.getText() : fs.readFileSync(file, "utf8");
}

function vscodeRange(document, start, end) {
  return new vscode.Range(document.positionAt(start), document.positionAt(end));
}

// Ctrl/Cmd+click (Go to Definition) on a SourceLink or CrossSourceLink opens
// the target source at its label.
class SourceLinkDefinitionProvider {
  provideDefinition(document, position) {
    if (document.uri.scheme !== "file") {
      return undefined;
    }
    const link = linkAt(document.getText(), document.offsetAt(position));
    if (!link || link.family === "pdf") {
      return undefined;
    }
    const target = resolveLink(link, document.uri.fsPath, readLinkTarget);
    if (!target) {
      return undefined;
    }
    const range = new vscode.Range(
      target.start.line,
      target.start.character,
      target.end.line,
      target.end.character
    );
    return [
      {
        originSelectionRange: vscodeRange(document, link.start, link.end),
        targetUri: vscode.Uri.file(target.file),
        targetRange: range,
        targetSelectionRange: range
      }
    ];
  }
}

// PdfLink and CrossPdfLink open their target PDF, which the custom editor
// hands to Thoughts. Thoughts opens the PDF without the anchor.
class PdfLinkDocumentLinkProvider {
  provideDocumentLinks(document) {
    if (document.uri.scheme !== "file") {
      return [];
    }
    const links = [];
    for (const link of findLinks(document.getText())) {
      if (link.family !== "pdf") {
        continue;
      }
      const target = resolveLink(link, document.uri.fsPath, readLinkTarget);
      if (!target) {
        continue;
      }
      const documentLink = new vscode.DocumentLink(
        vscodeRange(document, link.start, link.end),
        vscode.Uri.file(target.file)
      );
      documentLink.tooltip = "Open PDF in Thoughts";
      links.push(documentLink);
    }
    return links;
  }
}

function docsRootForLayout(layoutPath) {
  return path.dirname(path.dirname(path.dirname(layoutPath)));
}

async function readLinkedPdfMappings(output) {
  const layoutUris = await vscode.workspace.findFiles(
    "comments/.layout/*.json"
  );
  const mappingGroups = [];

  for (const layoutUri of layoutUris) {
    try {
      const contents = await fs.promises.readFile(layoutUri.fsPath, "utf8");
      const docsRoot = docsRootForLayout(layoutUri.fsPath);
      mappingGroups.push(
        mappingsFromLayout(layoutUri.fsPath, contents, docsRoot)
      );
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      output.appendLine(
        `Skipping linked-PDF layout ${layoutUri.fsPath}: ${message}`
      );
    }
  }

  return mergeMappings(mappingGroups);
}

async function activate(context) {
  const output = vscode.window.createOutputChannel("Thoughts PDF Links");
  const provider = new ExternalPdfEditorProvider(output);
  let linkedPdfMappings = new Map();
  let refreshTimer;
  let refreshSequence = 0;

  const refreshMappings = async () => {
    const sequence = ++refreshSequence;
    const refreshed = await readLinkedPdfMappings(output);
    if (sequence !== refreshSequence) {
      return;
    }

    linkedPdfMappings = refreshed;
    await vscode.commands.executeCommand(
      "setContext",
      linkedTexPathsContext,
      [...linkedPdfMappings.keys()]
    );
    output.appendLine(
      `Loaded ${linkedPdfMappings.size} linked LaTeX source(s).`
    );
  };

  const scheduleRefresh = () => {
    if (refreshTimer) {
      clearTimeout(refreshTimer);
    }
    refreshTimer = setTimeout(() => {
      refreshTimer = undefined;
      void refreshMappings();
    }, 100);
  };

  const openLinkedPdf = async (resource) => {
    try {
      const uri =
        resource instanceof vscode.Uri
          ? resource
          : vscode.window.activeTextEditor?.document.uri;
      if (!uri || uri.scheme !== "file") {
        throw new Error("The selected tab is not a local file.");
      }

      const sourcePath = normalizedPath(uri.fsPath);
      let pdfPaths = linkedPdfMappings.get(sourcePath);
      if (!pdfPaths) {
        await refreshMappings();
        pdfPaths = linkedPdfMappings.get(sourcePath);
      }
      if (!pdfPaths || pdfPaths.length === 0) {
        throw new Error(
          "This LaTeX source is not linked to an available canonical PDF."
        );
      }

      let pdfPath = pdfPaths[0];
      if (pdfPaths.length > 1) {
        const choice = await vscode.window.showQuickPick(
          pdfPaths.map((candidate) => ({
            label: path.basename(candidate),
            description: candidate,
            pdfPath: candidate
          })),
          {
            placeHolder: "Select the canonical PDF to open in Thoughts"
          }
        );
        if (!choice) {
          return;
        }
        pdfPath = choice.pdfPath;
      }

      await fs.promises.access(pdfPath, fs.constants.R_OK);
      output.appendLine(`Opening linked PDF ${pdfPath} for ${sourcePath}`);
      await openPdfInThoughts(pdfPath);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      output.appendLine(`Failed: ${message}`);
      void vscode.window.showErrorMessage(
        `Could not open the linked PDF in Thoughts: ${message}`
      );
    }
  };

  const layoutWatcher = vscode.workspace.createFileSystemWatcher(
    "comments/.layout/*.json"
  );
  const currentPdfWatcher = vscode.workspace.createFileSystemWatcher(
    "current_pdfs/*.pdf"
  );
  for (const watcher of [layoutWatcher, currentPdfWatcher]) {
    watcher.onDidCreate(scheduleRefresh);
    watcher.onDidChange(scheduleRefresh);
    watcher.onDidDelete(scheduleRefresh);
  }

  context.subscriptions.push(
    output,
    layoutWatcher,
    currentPdfWatcher,
    vscode.window.registerCustomEditorProvider(viewType, provider, {
      supportsMultipleEditorsPerDocument: true
    }),
    vscode.commands.registerCommand(openLinkedPdfCommand, openLinkedPdf),
    vscode.languages.registerDefinitionProvider(
      linkDocumentSelector,
      new SourceLinkDefinitionProvider()
    ),
    vscode.languages.registerDocumentLinkProvider(
      linkDocumentSelector,
      new PdfLinkDocumentLinkProvider()
    ),
    vscode.workspace.onDidChangeWorkspaceFolders(scheduleRefresh),
    {
      dispose() {
        if (refreshTimer) {
          clearTimeout(refreshTimer);
        }
        void vscode.commands.executeCommand(
          "setContext",
          linkedTexPathsContext,
          []
        );
      }
    }
  );

  await refreshMappings();
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
