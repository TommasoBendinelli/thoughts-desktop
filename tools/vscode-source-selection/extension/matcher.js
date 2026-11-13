// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

'use strict';

// Preserve UTF-16 offsets, which are also the offsets used by VS Code.
function normalized(text, kind) {
    const hidden = new Uint8Array(text.length);
    const hide = (regex) => {
        for (const match of text.matchAll(regex)) hidden.fill(1, match.index, match.index + match[0].length);
    };
    if (kind === 'markdown') {
        hide(/^[ \t]{0,3}#{1,6}[ \t]+/gm);
        hide(/\*\*|__|~~|`+/g);
        // Link labels remain visible, destinations do not.
        hide(/\]\([^\n]*?\)/g);
        hide(/!?(?=\[[^\]\n]+\]\()[[]/g);
        for (const match of text.matchAll(/(^|\W)([*_])([^\n]+?)\2(?=\W|$)/g)) {
            hidden[match.index + match[1].length] = 1;
            hidden[match.index + match[0].length - 1] = 1;
        }
    } else if (kind === 'latex') {
        hide(/\\(?:textbf|textit|emph|textrm|textsf|texttt|underline|section|subsection|subsubsection|chapter|paragraph)\*?(?=\{)/g);
        hide(/[{}]/g);
        hide(/(?<!\\)%[^\n]*/g);
    }
    let value = '';
    const starts = [], ends = [];
    for (let i = 0; i < text.length;) {
        const start = i;
        const cp = String.fromCodePoint(text.codePointAt(i));
        i += cp.length;
        if (hidden[start] || cp === '\u00ad') continue;
        let rendered = cp.normalize('NFKC').replace(/[‘’]/g, "'").replace(/[“”]/g, '"');
        if (/\s/u.test(rendered)) rendered = ' ';
        if (rendered === ' ' && (!value || value.endsWith(' '))) {
            if (value) ends[ends.length - 1] = i;
            continue;
        }
        for (const char of rendered) {
            value += char;
            for (let n = 0; n < char.length; ++n) { starts.push(start); ends.push(i); }
        }
    }
    if (value.endsWith(' ')) { value = value.slice(0, -1); starts.pop(); ends.pop(); }
    return { value, starts, ends };
}

function findSelection(source, selected, line, kind) {
    const needle = normalized(selected, '').value;
    if (!needle) return null;
    // Prefer literal text; only interpret markup if the literal passage is absent.
    for (const mode of ['', kind]) {
        const haystack = normalized(source, mode);
        const candidates = [];
        for (let at = haystack.value.indexOf(needle); at >= 0; at = haystack.value.indexOf(needle, at + 1)) {
            const start = haystack.starts[at], end = haystack.ends[at + needle.length - 1];
            const first = source.slice(0, start).split('\n').length;
            const last = first + source.slice(start, end).split('\n').length - 1;
            const distance = Math.max(first - line, line - last, 0);
            if (distance <= 8) candidates.push({ start, end, distance });
        }
        candidates.sort((a, b) => a.distance - b.distance);
        if (candidates.length) {
            if (candidates.length > 1 && candidates[0].distance === candidates[1].distance) return null;
            return { start: candidates[0].start, end: candidates[0].end };
        }
    }
    return null;
}
module.exports = { findSelection };
