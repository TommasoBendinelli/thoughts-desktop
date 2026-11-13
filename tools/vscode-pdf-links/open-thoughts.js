// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const { spawn } = require("child_process");

const openExecutable = "/usr/bin/open";
const thoughtsBundleIdentifier = "org.tbend.thoughts";

function openPdfInThoughts(filePath, spawnImplementation = spawn) {
  return new Promise((resolve, reject) => {
    const child = spawnImplementation(
      openExecutable,
      ["-b", thoughtsBundleIdentifier, filePath],
      {
        detached: true,
        shell: false,
        stdio: "ignore"
      }
    );

    child.once("error", reject);
    child.once("spawn", () => {
      child.unref();
      resolve();
    });
  });
}

module.exports = {
  thoughtsBundleIdentifier,
  openExecutable,
  openPdfInThoughts
};
