// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
// Run using Code's --extensionTestsPath with this extension as --extensionDevelopmentPath.
const vscode = require('vscode');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
exports.run = async function () {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'thoughts-selection-test-'));
    try {
        const extension = vscode.extensions.getExtension('thoughts.source-selection');
        assert.ok(extension);
        const api = await extension.activate();
        const sourcePath = path.join(dir, 'source.md');
        const passage = 'First selected paragraph wraps\nonto another line.\n\n**Second paragraph is bold.** More words.\n\nThe final selected paragraph.';
        const source = '# Heading\n\n' + passage + '\n\n## Next section\n';
        await fs.writeFile(sourcePath, source);
        const requestPath = path.join(dir, `thoughts-source-selection-${crypto.randomUUID()}.json`);
        await fs.writeFile(requestPath, JSON.stringify({path: sourcePath, line: 3, text: passage.replaceAll('**', '').replaceAll('\n', ' ')}));
        await api.open(vscode.Uri.parse('vscode://thoughts.source-selection/open?' + new URLSearchParams({request: requestPath})));
        const editor = vscode.window.activeTextEditor;
        assert.equal(editor.document.uri.fsPath, sourcePath);
        assert.equal(editor.document.getText(editor.selection), passage);
        assert.equal(editor.selection.start.line, 2);
        assert.equal(editor.selection.end.line, 7);
        assert.equal(editor.document.isDirty, false);
        assert.equal(await fs.readFile(sourcePath, 'utf8'), source);
        await assert.rejects(fs.stat(requestPath), {code: 'ENOENT'});
        const originalPath = path.join(dir, 'original.md');
        await fs.writeFile(originalPath, '# Previous version\n');
        const diffRequest = path.join(dir, `thoughts-source-selection-${crypto.randomUUID()}.json`);
        await fs.writeFile(diffRequest, JSON.stringify({path: sourcePath, original_path: originalPath, line: 5, text: ''}));
        await api.open(vscode.Uri.parse('vscode://thoughts.source-selection/open?' + new URLSearchParams({request: diffRequest})));
        const tab = vscode.window.tabGroups.activeTabGroup.activeTab;
        assert.ok(tab.input instanceof vscode.TabInputTextDiff);
        assert.equal(tab.input.original.fsPath, originalPath);
        assert.equal(tab.input.modified.fsPath, sourcePath);
        assert.equal(vscode.window.activeTextEditor.selection.active.line, 4);
        assert.equal(await fs.readFile(originalPath, 'utf8'), '# Previous version\n');
        assert.equal(await fs.readFile(sourcePath, 'utf8'), source);
        await assert.rejects(fs.stat(diffRequest), {code: 'ENOENT'});
        // Current source is the actual file, and newer unsaved edits are preserved.
        const currentEditor = vscode.window.activeTextEditor;
        await currentEditor.edit(edit => edit.insert(new vscode.Position(0, 0), 'Unsaved newer change.\n'));
        assert.equal(currentEditor.document.isDirty, true);
        const editableBase = path.join(dir, 'editable-baseline.md');
        await fs.copyFile(originalPath, editableBase);
        const editRequest = path.join(dir, `thoughts-source-selection-${crypto.randomUUID()}.json`);
        await fs.writeFile(editRequest, JSON.stringify({path: sourcePath, original_path: editableBase,
            line: 5, text: '', current_source: true, failed_sha256: crypto.createHash('sha256').update(source).digest('hex')}));
        await api.open(vscode.Uri.parse('vscode://thoughts.source-selection/open?' + new URLSearchParams({request: editRequest})));
        const editableTab = vscode.window.tabGroups.activeTabGroup.activeTab;
        assert.ok(editableTab.input instanceof vscode.TabInputTextDiff);
        assert.equal(editableTab.input.modified.fsPath, sourcePath);
        assert.ok(editableTab.label.includes('Current source'));
        assert.ok(editableTab.label.includes('Modified since failed build'));
        assert.equal(vscode.window.activeTextEditor.document.getText(), 'Unsaved newer change.\n' + source);
        assert.equal(await vscode.window.activeTextEditor.document.save(), true);
        assert.equal(await fs.readFile(sourcePath, 'utf8'), 'Unsaved newer change.\n' + source);
        assert.equal(await fs.readFile(originalPath, 'utf8'), '# Previous version\n');
        // Also exercise a TeX file with no changes since the failed build.
        const texPath = path.join(dir, 'current.tex');
        const tex = '\\section{Example}\nOriginal text.\n';
        await fs.writeFile(texPath, tex);
        const texRequest = path.join(dir, `thoughts-source-selection-${crypto.randomUUID()}.json`);
        await fs.writeFile(texRequest, JSON.stringify({path: texPath, original_path: originalPath,
            line: 2, text: '', current_source: true, failed_sha256: crypto.createHash('sha256').update(tex).digest('hex')}));
        await api.open(vscode.Uri.parse('vscode://thoughts.source-selection/open?' + new URLSearchParams({request: texRequest})));
        assert.ok(vscode.window.tabGroups.activeTabGroup.activeTab.label.includes('Current source'));
        assert.ok(!vscode.window.tabGroups.activeTabGroup.activeTab.label.includes('Modified since failed build'));
        await vscode.window.activeTextEditor.edit(edit => edit.insert(new vscode.Position(1, 0), 'Edited. '));
        assert.equal(await vscode.window.activeTextEditor.document.save(), true);
        assert.equal(await fs.readFile(texPath, 'utf8'), '\\section{Example}\nEdited. Original text.\n');
        console.log('PASS: editable TeX and Markdown comparisons save to the actual current source and preserve newer unsaved edits.');
        console.log('PASS: actual VS Code diff opens the correct snapshots at the diagnostic line.');
        console.log('PASS: actual VS Code editor selects the entire multi-paragraph passage without modifying source.');
    } finally {
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        await fs.rm(dir, {recursive: true, force: true});
    }
};
