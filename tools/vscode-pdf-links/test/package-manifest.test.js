// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const assert = require("assert");
const manifest = require("../package.json");

const command = manifest.contributes.commands.find(
  (candidate) => candidate.command === "thoughts.openLinkedPdf"
);
assert.deepStrictEqual(command, {
  command: "thoughts.openLinkedPdf",
  title: "Open PDF in Thoughts"
});

const tabMenu = manifest.contributes.menus["editor/title/context"].find(
  (candidate) => candidate.command === "thoughts.openLinkedPdf"
);
assert.deepStrictEqual(tabMenu, {
  command: "thoughts.openLinkedPdf",
  when: "resourceScheme == file && resourceExtname == .tex && resourcePath in thoughts.linkedTexPaths",
  group: "2_open@1"
});

assert.ok(manifest.activationEvents.includes("onStartupFinished"));
assert.deepStrictEqual(
  manifest.contributes.menus.commandPalette.find(
    (candidate) => candidate.command === "thoughts.openLinkedPdf"
  ),
  {
    command: "thoughts.openLinkedPdf",
    when: "false"
  }
);

process.stdout.write("Extension manifest tests passed\n");
