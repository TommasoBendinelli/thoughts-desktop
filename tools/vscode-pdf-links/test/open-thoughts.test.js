// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

"use strict";

const assert = require("assert");
const { EventEmitter } = require("events");
const {
  thoughtsBundleIdentifier,
  openExecutable,
  openPdfInThoughts
} = require("../open-thoughts");

async function testOpenArguments() {
  const calls = [];
  const child = new EventEmitter();
  child.unrefCalled = false;
  child.unref = () => {
    child.unrefCalled = true;
  };

  const result = openPdfInThoughts(
    "/tmp/PDF with spaces.pdf",
    (executable, args, options) => {
      calls.push({ executable, args, options });
      process.nextTick(() => child.emit("spawn"));
      return child;
    }
  );

  await result;
  assert.deepStrictEqual(calls, [
    {
      executable: openExecutable,
      args: ["-b", thoughtsBundleIdentifier, "/tmp/PDF with spaces.pdf"],
      options: {
        detached: true,
        shell: false,
        stdio: "ignore"
      }
    }
  ]);
  assert.strictEqual(child.unrefCalled, true);
}

async function testSpawnFailure() {
  const child = new EventEmitter();
  child.unref = () => {};
  const expectedError = new Error("open failed");

  await assert.rejects(
    openPdfInThoughts("/tmp/test.pdf", () => {
      process.nextTick(() => child.emit("error", expectedError));
      return child;
    }),
    expectedError
  );
}

async function main() {
  await testOpenArguments();
  await testSpawnFailure();
  process.stdout.write("Thoughts launcher tests passed\n");
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
