// SPDX-License-Identifier: GPL-2.0-or-later
// The selection of a live HTML page, for text feedback (thoughts-server protocol/html-live-pages.md
// §2.1: Like, Dislike and Comment… on a live page are the reader's, not the page's).
//
// The body of an async function that Thoughts runs in the live page's isolated script world
// (WKContentWorld.defaultClientWorld), right after the viewer's text.js, which defines
// ThoughtsViewerText in that world only. The page's own scripts can neither see nor call any of
// it, and it changes nothing in the page. Its one argument, argsJson, is
// {"context": characters around the selection, "longest": characters of the selection}.
//
// It returns JSON {exact, prefix, suffix, hash}: the selected text, up to `context` characters of
// the page's text right before and after it, cut from the text as the viewer indexes it (whitespace
// collapsed, a space between blocks, so a snapshot of the page finds the quote again), and
// location.hash. exact is empty when nothing is selected that it can read (a form field's text is
// never read).
const args = JSON.parse(argsJson || '{}');
const context = Number(args.context) > 0 ? Number(args.context) : 32;
const longest = Number(args.longest) > 0 ? Number(args.longest) : 4000;
// At most `count` UTF-16 units, never half a character.
const cut = (text, count) => {
    if (text.length <= count) return text;
    const code = text.charCodeAt(count - 1);
    return text.slice(0, code >= 0xd800 && code <= 0xdbff ? count - 1 : count);
};
const result = {exact: '', prefix: '', suffix: '', hash: cut(String(location.hash || ''), 2048)};
const selection = window.getSelection();
if (selection && selection.rangeCount > 0 && !selection.isCollapsed) {
    const range = selection.getRangeAt(0);
    const T = globalThis.ThoughtsViewerText;
    const root = document.body || document.documentElement;
    let quote = null;
    if (T && root) {
        try {
            const index = new T.TextIndex(root);
            const span = index.spanOfRange(range);
            quote = T.quoteOf(index.text, span[0], span[1], context);
        } catch (e) {
            quote = null;
        }
    }
    // Text the index leaves out (an SVG label): the selection's own text, without context.
    if (!quote || !quote.exact) {
        quote = {exact: selection.toString().replace(/\s+/g, ' ').trim(), prefix: '', suffix: ''};
    }
    result.exact = cut(quote.exact, longest);
    result.prefix = quote.exact ? quote.prefix : '';
    result.suffix = quote.exact ? quote.suffix : '';
}
return JSON.stringify(result);
