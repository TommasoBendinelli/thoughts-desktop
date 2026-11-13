// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const fs = require("fs");
const path = require("path");

const layoutVersion = 1;

function normalizedPath(filePath) {
  return path.normalize(path.resolve(filePath));
}

function isPathInside(parentPath, candidatePath) {
  const relative = path.relative(parentPath, candidatePath);
  return (
    relative === "" ||
    (!relative.startsWith(`..${path.sep}`) && relative !== ".." && !path.isAbsolute(relative))
  );
}

function mappingsFromLayout(
  layoutPath,
  contents,
  docsRoot,
  fileExists = fs.existsSync
) {
  const layout = JSON.parse(contents);
  if (
    layout.version !== layoutVersion ||
    !Array.isArray(layout.sources)
  ) {
    return [];
  }

  const normalizedDocsRoot = normalizedPath(docsRoot);
  const pdfStem = path.basename(layoutPath, path.extname(layoutPath));
  const pdfPath = normalizedPath(
    path.join(normalizedDocsRoot, "current_pdfs", `${pdfStem}.pdf`)
  );
  if (!fileExists(pdfPath)) {
    return [];
  }

  const mappings = [];
  for (const source of layout.sources) {
    if (
      !source ||
      typeof source.source_id !== "string" ||
      path.extname(source.source_id).toLowerCase() !== ".tex"
    ) {
      continue;
    }

    const sourcePath = normalizedPath(
      path.resolve(normalizedDocsRoot, source.source_id)
    );
    if (
      !isPathInside(normalizedDocsRoot, sourcePath) ||
      !fileExists(sourcePath)
    ) {
      continue;
    }

    mappings.push({ sourcePath, pdfPath });
  }
  return mappings;
}

function mergeMappings(mappingGroups) {
  const merged = new Map();
  for (const mappings of mappingGroups) {
    for (const { sourcePath, pdfPath } of mappings) {
      const sourceKey = normalizedPath(sourcePath);
      const targets = merged.get(sourceKey) || new Set();
      targets.add(normalizedPath(pdfPath));
      merged.set(sourceKey, targets);
    }
  }

  return new Map(
    [...merged.entries()].map(([sourcePath, targets]) => [
      sourcePath,
      [...targets].sort()
    ])
  );
}

module.exports = {
  isPathInside,
  layoutVersion,
  mappingsFromLayout,
  mergeMappings,
  normalizedPath
};
