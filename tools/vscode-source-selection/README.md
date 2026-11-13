# Thoughts Source Selection

The desktop shell bundles this VS Code companion and installs it with VS Code's CLI on the first selected-source open in each Thoughts session.
The extension opens the local source and selects the full matching passage using VS Code's editor API.
VS Code may ask once whether to allow Thoughts Source Selection to open a URI.

Matching preserves source offsets while normalizing whitespace, PDF ligatures, common Markdown formatting, and simple TeX formatting commands.
It searches near the mapped source line, rejects equally close duplicates, and never substitutes a partial match for a full passage.
Unsupported rendering, changed text, or a selection spanning multiple source files opens the mapped line with a message explaining that the full passage could not be matched.
No document text is modified.
Requests use private local files, are deleted after consumption, and stale requests expire after one day on the next source open.

Run `node --test tools/vscode-source-selection/test.js` for matching tests.
For an actual editor test, launch VS Code with isolated user-data/extensions directories, `--extensionDevelopmentPath` pointing at `extension`, and `--extensionTestsPath` pointing at `test-vscode.js`.

Build comparisons open the last successful source snapshot on the left and the editable current source file on the right by default.
Saving updates the current file directly; it does not start Sync.
The companion compares the current editor buffer with the failed source hash and labels newer content “Modified since failed build”.
The separate failed-build snapshot option keeps both sides as historical copies.
The editor test also verifies saving TeX and Markdown changes and preserving newer unsaved edits.

The separate, hand-installed `tools/vscode-pdf-links` extension opens PDFs and linked LaTeX sources in Thoughts from VS Code and follows Thoughts document links in LaTeX sources.
