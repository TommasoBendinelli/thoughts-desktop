// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

'use strict';
const vscode = require('vscode');
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const { findSelection } = require('./matcher');

function activate(context) {
    const handler = {
        async handleUri(uri) {
            if (uri.path !== '/open') return;
            try {
                const requestPath = new URLSearchParams(uri.query).get('request');
                if (!requestPath || !path.isAbsolute(requestPath)
                    || !/^thoughts-source-selection-[a-f0-9-]+\.json$/.test(path.basename(requestPath))) {
                    throw new Error('Invalid source selection request.');
                }
                const stat = await fs.stat(requestPath);
                if (stat.size > 4 * 1024 * 1024) throw new Error('Source selection is too large.');
                const request = JSON.parse(await fs.readFile(requestPath, 'utf8'));
                if (typeof request.path !== 'string' || !path.isAbsolute(request.path)
                    || typeof request.text !== 'string' || !Number.isInteger(request.line) || request.line < 1) {
                    throw new Error('Invalid source selection.');
                }
                await fs.unlink(requestPath);
                const document = await vscode.workspace.openTextDocument(vscode.Uri.file(request.path));
                if (request.original_path) {
                    if (typeof request.original_path !== 'string' || !path.isAbsolute(request.original_path)) {
                        throw new Error('Invalid original source path.');
                    }
                    const line = Math.min(request.line - 1, document.lineCount - 1);
                    const selection = new vscode.Range(line, 0, line, 0);
                    const current = request.current_source === true;
                    const changed = current && crypto.createHash('sha256').update(document.getText(), 'utf8').digest('hex') !== request.failed_sha256;
                    const rightLabel = current ? 'Current source' : 'Selected build';
                    const suffix = changed ? ' — Modified since failed build' : '';
                    await vscode.commands.executeCommand('vscode.diff', vscode.Uri.file(request.original_path),
                        vscode.Uri.file(request.path), `${path.basename(request.path)} — Last successful PDF ↔ ${rightLabel}${suffix}`,
                        { preview: false, selection });
                    if (changed) vscode.window.showInformationMessage('Modified since failed build: your current source, including any unsaved edits, has been preserved.');
                    return;
                }
                const kind = /\.md(?:own|markdown)?$/i.test(request.path) ? 'markdown'
                    : /\.tex$/i.test(request.path) ? 'latex' : '';
                const match = findSelection(document.getText(), request.text, request.line, kind);
                const start = match ? document.positionAt(match.start)
                    : new vscode.Position(Math.min(request.line - 1, document.lineCount - 1), 0);
                const end = match ? document.positionAt(match.end) : start;
                const selection = new vscode.Selection(start, end);
                const editor = await vscode.window.showTextDocument(document, { preview: false, selection });
                editor.selection = selection;
                editor.revealRange(selection, vscode.TextEditorRevealType.InCenterIfOutsideViewport);
                if (!match) vscode.window.showInformationMessage('Thoughts opened the source location, but could not match the full selected passage.');
            } catch (error) {
                vscode.window.showErrorMessage(`Thoughts: ${error.message}`);
            }
        }
    };
    context.subscriptions.push(vscode.window.registerUriHandler(handler));
    return { open: (uri) => handler.handleUri(uri) };
}
module.exports = { activate };
