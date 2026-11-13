// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

// Resolves the Thoughts cross-document link macros of manual_style.sty for
// editor navigation. The rules follow the publisher's resolver
// (thoughts_server/renderer_tools/doc_links.py):
//
//   \SourceLink{path}{label}{text}            path relative to the linking
//                                             document's source folder
//   \CrossSourceLink{owner/path}{label}{text} path relative to the docs root
//   \PdfLink{key}{anchor}{text}               key.pdf selected in this Project
//   \CrossPdfLink{owner/key}{anchor}{text}    key.pdf selected in owner's Project
//
// A source folder ("owner") is a docs-root folder docs-<name> or a Project's
// default folder .thoughts/projects/<64 hex>/default (.../documents before the
// 2026-10-17 rename; both spellings are accepted). A source path naming a
// single .tex file is also an alias of tex/source/<file>.
// Source labels are \SourceLabel{label} in LaTeX and a {#source-label:label}
// heading identifier in Markdown. The retired \TexLink and \CrossTexLink are
// not links any more: the build leaves them inactive.

const fs = require("fs");
const path = require("path");

const MACRO_PATTERN = /\\(Cross)?(Source|Pdf)Link(?![A-Za-z@])/g;
const CONNECTED_FOLDER = /^docs-[A-Za-z0-9_.-]+$/;
const DEFAULT_FOLDER = /^\.thoughts\/projects\/[0-9a-f]{64}\/(default|documents)$/;
const SOURCE_DOCUMENT_EXTENSIONS = new Set([".tex", ".md", ".markdown"]);
const MAX_INPUT_DEPTH = 16;
const MAX_ARGUMENT_LENGTH = 20000;

function ownerOf(relativePath) {
  if (typeof relativePath !== "string" || !relativePath) {
    return null;
  }
  const parts = relativePath.split("/");
  if (CONNECTED_FOLDER.test(parts[0])) {
    return parts[0];
  }
  if (parts.length >= 4) {
    const candidate = parts.slice(0, 4).join("/");
    if (DEFAULT_FOLDER.test(candidate)) {
      return candidate;
    }
  }
  return null;
}

function relativeToOwner(relativePath) {
  const owner = ownerOf(relativePath);
  if (!owner || !relativePath.startsWith(`${owner}/`)) {
    return null;
  }
  return relativePath.slice(owner.length + 1);
}

function isDirectory(candidate) {
  try {
    return fs.statSync(candidate).isDirectory();
  } catch {
    return false;
  }
}

function isFile(candidate) {
  try {
    return fs.statSync(candidate).isFile();
  } catch {
    return false;
  }
}

// The docs root and source folder of an absolute source path, or null.
function sourceContext(filePath) {
  const parts = path.resolve(filePath).split(path.sep);
  const candidates = [];
  // Stop before the file name itself: an owner is always a folder.
  for (let index = 1; index < parts.length - 1; index += 1) {
    if (CONNECTED_FOLDER.test(parts[index])) {
      candidates.push({
        docsRoot: parts.slice(0, index).join(path.sep) || path.sep,
        owner: parts[index]
      });
    }
    if (
      index + 3 < parts.length - 1 &&
      DEFAULT_FOLDER.test(parts.slice(index, index + 4).join("/"))
    ) {
      candidates.push({
        docsRoot: parts.slice(0, index).join(path.sep) || path.sep,
        owner: parts.slice(index, index + 4).join("/")
      });
    }
  }
  if (candidates.length === 0) {
    return null;
  }
  // Prefer the folder whose parent is a Thoughts docs root.
  const rooted = candidates.filter((candidate) =>
    isDirectory(path.join(candidate.docsRoot, ".thoughts"))
  );
  const chosen = (rooted.length ? rooted : candidates).at(-1);
  return chosen;
}

function lineStartOffsets(text) {
  const starts = [0];
  for (let index = 0; index < text.length; index += 1) {
    if (text[index] === "\n") {
      starts.push(index + 1);
    }
  }
  return starts;
}

function positionAt(text, offset, starts = lineStartOffsets(text)) {
  let low = 0;
  let high = starts.length - 1;
  while (low < high) {
    const middle = (low + high + 1) >> 1;
    if (starts[middle] <= offset) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return { line: low, character: offset - starts[low] };
}

// Replaces TeX comments with spaces so offsets stay valid.
function maskTexComments(text) {
  let result = "";
  let index = 0;
  while (index < text.length) {
    const character = text[index];
    if (character === "\\") {
      result += text.slice(index, index + 2);
      index += 2;
      continue;
    }
    if (character === "%") {
      const end = text.indexOf("\n", index);
      const stop = end === -1 ? text.length : end;
      result += " ".repeat(stop - index);
      index = stop;
      continue;
    }
    result += character;
    index += 1;
  }
  return result;
}

function skipSpaces(text, index) {
  let newlines = 0;
  while (index < text.length && /[ \t\r\n]/.test(text[index])) {
    if (text[index] === "\n") {
      newlines += 1;
      // A blank line ends a TeX argument list.
      if (newlines > 1) {
        return -1;
      }
    }
    index += 1;
  }
  return index;
}

// A balanced {...} group starting at index: {value, start, end} where start
// and end delimit the content without the braces.
function bracedGroup(text, index) {
  if (text[index] !== "{") {
    return null;
  }
  let depth = 0;
  for (let cursor = index; cursor < text.length; cursor += 1) {
    if (cursor - index > MAX_ARGUMENT_LENGTH) {
      return null;
    }
    const character = text[cursor];
    if (character === "\\") {
      cursor += 1;
    } else if (character === "{") {
      depth += 1;
    } else if (character === "}") {
      depth -= 1;
      if (depth === 0) {
        return {
          value: text.slice(index + 1, cursor),
          start: index + 1,
          end: cursor
        };
      }
    }
  }
  return null;
}

// Every link macro with at least its target and label arguments.
function findLinks(text) {
  const masked = maskTexComments(text);
  const links = [];
  MACRO_PATTERN.lastIndex = 0;
  let match;
  while ((match = MACRO_PATTERN.exec(masked)) !== null) {
    const args = [];
    let cursor = match.index + match[0].length;
    while (args.length < 3) {
      const next = skipSpaces(masked, cursor);
      const group = next === -1 ? null : bracedGroup(masked, next);
      if (!group) {
        break;
      }
      args.push(group);
      cursor = group.end + 1;
    }
    if (args.length < 2) {
      continue;
    }
    links.push({
      cross: Boolean(match[1]),
      family: match[2].toLowerCase(),
      macro: match[0].slice(1),
      target: args[0].value.trim(),
      label: args[1].value.trim(),
      start: match.index,
      end: args.at(-1).end + 1,
      targetStart: args[0].start,
      targetEnd: args[0].end
    });
  }
  return links;
}

function linkAt(text, offset) {
  return (
    findLinks(text).find(
      (link) => link.start <= offset && offset <= link.end
    ) || null
  );
}

// The docs-root relative selector a source link names, or null when invalid.
function sourceSelector(link, context) {
  let selector = link.target.replace(/\\/g, "/");
  if (!link.cross) {
    selector = `${context.owner}/${selector}`;
  }
  const parts = selector.split("/");
  if (
    selector.startsWith("/") ||
    parts.some((part) => part === "" || part === "." || part === "..")
  ) {
    return null;
  }
  const relative = relativeToOwner(selector);
  if (!relative) {
    return null;
  }
  const extension = path.extname(relative).toLowerCase();
  if (!SOURCE_DOCUMENT_EXTENSIONS.has(extension)) {
    return null;
  }
  return selector;
}

function sourceTargetPath(selector, docsRoot) {
  const direct = path.join(docsRoot, ...selector.split("/"));
  if (isFile(direct)) {
    return direct;
  }
  const owner = ownerOf(selector);
  const relative = relativeToOwner(selector);
  // A single file name aliases tex/source/<name>.
  if (relative && !relative.includes("/")) {
    const alias = path.join(docsRoot, ...owner.split("/"), "tex", "source", relative);
    if (isFile(alias)) {
      return alias;
    }
  }
  return null;
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function labelPattern(label, markdown) {
  const name = escapeRegExp(label);
  return markdown
    ? new RegExp(`(?:\\{[^}\\n]*#|id=["'])(source-label:${name})(?=[\\s}"'])`)
    : new RegExp(`\\\\SourceLabel\\s*\\{\\s*(${name})\\s*\\}`);
}

const INPUT_PATTERN = /\\(?:input|include|subfile|InputIfFileExists)\s*\{([^{}]+)\}/g;

function inputCandidates(argument, currentFile, rootFile, ownerDir) {
  const name = argument.trim();
  if (!name || path.isAbsolute(name)) {
    return [];
  }
  const names = path.extname(name) ? [name] : [`${name}.tex`, name];
  const bases = [ownerDir, path.dirname(rootFile), path.dirname(currentFile)];
  const candidates = [];
  for (const base of bases) {
    for (const candidateName of names) {
      candidates.push(path.resolve(base, candidateName));
    }
  }
  return candidates;
}

// Finds the label in the target file or, for LaTeX, the files it inputs.
function findLabel(rootFile, label, ownerDir, readFile) {
  const visited = new Set();
  const search = (file, depth) => {
    if (depth > MAX_INPUT_DEPTH || visited.has(file)) {
      return null;
    }
    visited.add(file);
    let text;
    try {
      text = readFile(file);
    } catch {
      return null;
    }
    const markdown = /\.(md|markdown)$/i.test(file);
    const searchable = markdown ? text : maskTexComments(text);
    const match = labelPattern(label, markdown).exec(searchable);
    if (match) {
      const start = match.index + match[0].indexOf(match[1]);
      const starts = lineStartOffsets(text);
      return {
        file,
        start: positionAt(text, start, starts),
        end: positionAt(text, start + match[1].length, starts)
      };
    }
    if (markdown) {
      return null;
    }
    INPUT_PATTERN.lastIndex = 0;
    const inputs = [...searchable.matchAll(INPUT_PATTERN)];
    for (const input of inputs) {
      const child = inputCandidates(input[1], file, rootFile, ownerDir).find(isFile);
      if (child) {
        const found = search(child, depth + 1);
        if (found) {
          return found;
        }
      }
    }
    return null;
  };
  return search(rootFile, 0);
}

function readProjectConfigurations(docsRoot) {
  const projectsDir = path.join(docsRoot, ".thoughts", "projects");
  let entries;
  try {
    entries = fs.readdirSync(projectsDir);
  } catch {
    return [];
  }
  const configurations = [];
  for (const entry of entries) {
    // A Project has one default folder: default/ first, then legacy documents/.
    for (const segment of ["default", "documents"]) {
      const file = path.join(projectsDir, entry, segment, "thoughts-project.json");
      if (!isFile(file)) {
        continue;
      }
      try {
        configurations.push(JSON.parse(fs.readFileSync(file, "utf8")));
      } catch {
        // Not a Project, or one that cannot be read.
      }
      break;
    }
  }
  return configurations;
}

// The PDF a PdfLink key names in owner's Project, or the shared third_party PDF.
function pdfTargetPath(owner, key, docsRoot, allowThirdParty) {
  if (!/^[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(key)) {
    return null;
  }
  const filename = `${key}.pdf`;
  for (const configuration of readProjectConfigurations(docsRoot)) {
    const folders = Array.isArray(configuration.source_folders)
      ? configuration.source_folders
      : [];
    if (!folders.some((folder) => folder && folder.path === owner)) {
      continue;
    }
    for (const document of configuration.documents || []) {
      const source = document && document.source;
      if (
        !document ||
        document.format !== "pdf" ||
        !source ||
        typeof source.path !== "string" ||
        path.posix.basename(source.path) !== filename
      ) {
        continue;
      }
      const folder = folders.find((candidate) => candidate && candidate.folder_id === source.folder_id);
      if (!folder || typeof folder.path !== "string") {
        continue;
      }
      const candidate = path.join(docsRoot, ...folder.path.split("/"), ...source.path.split("/"));
      if (isFile(candidate)) {
        return candidate;
      }
    }
  }
  if (allowThirdParty) {
    const shared = path.join(docsRoot, "third_party", filename);
    if (isFile(shared)) {
      return shared;
    }
  }
  return null;
}

// Resolves a link to {kind: "source", file, labelFound, start, end} or
// {kind: "pdf", file}, or null when its target does not exist.
function resolveLink(link, documentPath, readFile = (file) => fs.readFileSync(file, "utf8")) {
  const context = sourceContext(documentPath);
  if (!context) {
    return null;
  }
  const { docsRoot } = context;

  if (link.family === "pdf") {
    if (link.cross) {
      const parts = link.target.split("/");
      if (parts.length !== 2 || !CONNECTED_FOLDER.test(parts[0])) {
        return null;
      }
      const file = pdfTargetPath(parts[0], parts[1], docsRoot, false);
      return file ? { kind: "pdf", file } : null;
    }
    const file = pdfTargetPath(context.owner, link.target, docsRoot, true);
    return file ? { kind: "pdf", file } : null;
  }

  const selector = sourceSelector(link, context);
  if (!selector) {
    return null;
  }
  const file = sourceTargetPath(selector, docsRoot);
  if (!file) {
    return null;
  }
  const top = { line: 0, character: 0 };
  const ownerDir = path.join(docsRoot, ...ownerOf(selector).split("/"));
  const found = link.label
    ? findLabel(file, link.label, ownerDir, readFile)
    : null;
  if (!found) {
    return { kind: "source", file, labelFound: false, start: top, end: top };
  }
  return {
    kind: "source",
    file: found.file,
    labelFound: true,
    start: found.start,
    end: found.end
  };
}

module.exports = {
  findLinks,
  linkAt,
  maskTexComments,
  ownerOf,
  positionAt,
  resolveLink,
  sourceContext
};
