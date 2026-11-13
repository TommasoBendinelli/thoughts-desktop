// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { findSelection } = require('./extension/matcher');
test('full Markdown passage across paragraphs and bold markers', () => {
    const passage = 'Markdown distilled from a set of PDF and HTML reports. Built for quick reading:\neach page leads with the finding.\n\n**This knowledge base is self-contained.** Every link points inside it.\n\nThe sources are listed under **Sources** on each section page.';
    const source = '---\ntitle: Example\n---\n\n# Knowledge base\n\n' + passage + '\n\n## Sections';
    const selected = passage.replaceAll('**', '').replaceAll('\n', ' ');
    const range = findSelection(source, selected, 8, 'markdown');
    assert.equal(source.slice(range.start, range.end), passage);
});
test('partial lines retain exact boundaries and UTF-16 offsets', () => {
    const source = 'Before 😀 café, selected words, after.';
    const range = findSelection(source, '😀 café, selected words', 1, 'markdown');
    assert.equal(source.slice(range.start, range.end), '😀 café, selected words');
});
test('whitespace, smart apostrophes, and PDF ligatures', () => {
    const source = "A file's\r\ncontents are fine.";
    const range = findSelection(source, 'A ﬁle’s contents are fine.', 1, 'markdown');
    assert.deepEqual(range, { start: 0, end: source.length });
});
test('Markdown link label and TeX formatting', () => {
    assert.deepEqual(findSelection('A [link](https://example.com) here.', 'A link here.', 1, 'markdown'), { start: 0, end: 35 });
    const source = 'An \\emph{important} passage.\nAnother sentence.';
    assert.deepEqual(findSelection(source, 'An important passage. Another sentence.', 1, 'latex'), { start: 0, end: source.length });
});
test('duplicates choose nearest mapped line and reject ties', () => {
    const source = 'same words\n\n\nsame words';
    assert.deepEqual(findSelection(source, 'same words', 4, ''), { start: 13, end: 23 });
    assert.equal(findSelection('same words\n\nsame words', 'same words', 2, ''), null);
});
test('missing, distant, empty, and incomplete passages do not invent ranges', () => {
    assert.equal(findSelection('some text', 'some missing text', 1, ''), null);
    assert.equal(findSelection('some text', '', 1, ''), null);
    assert.equal(findSelection('some text', 'some text', 20, ''), null);
});
