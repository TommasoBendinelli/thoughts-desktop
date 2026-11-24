// SPDX-License-Identifier: GPL-2.0-or-later
// Take Snapshot of a live HTML page (thoughts-server protocol/html-live-pages.md §3).
//
// The body of an async function that Thoughts runs in the live page's isolated script world
// (WKContentWorld.defaultClientWorld: the page's own scripts share the DOM but cannot change the
// functions used here). Its one argument, argsJson, is {"limit": bytes for inlined resources}.
//
// It serializes what the page shows now into one self-contained HTML document: scripts removed,
// every <canvas> replaced by an <img> of its pixels (data: PNG), stylesheets inlined from their
// rules (so rules added by scripts are kept), images, fonts and other CSS resources inlined as
// data: URLs where this page can read them, form fields with their current values (never a
// password), links made absolute. Frames are left out (an embedded page shows none). It returns
// JSON {html, title, inlined, skipped, canvases}; html is the root element's outer HTML, to which
// Thoughts adds the snapshot's meta elements and its visible line (Thoughts::LivePage::finishSnapshot).
const args = JSON.parse(argsJson || '{}');
let budget = Number(args.limit) > 0 ? Number(args.limit) : 90 * 1000 * 1000;
const stats = {inlined: 0, skipped: 0, canvases: 0};
const loaded = new Map();
const root = document.documentElement;
const pick = (element, selector) => Array.from(element.querySelectorAll(selector));
const absolute = (url, base) => {
    try {
        return new URL(url, base || document.baseURI).href;
    } catch (e) {
        return null;
    }
};

// --- What the page shows now, read at once (the page keeps running while resources load). ---
const live = {
    canvases: pick(root, 'canvas'),
    images: pick(root, 'img'),
    videos: pick(root, 'video'),
    fields: pick(root, 'input, textarea, select'),
    styles: pick(root, 'link, style'),
    frames: pick(root, 'iframe, frame, object, embed'),
};
// Its border box as laid out (offsetWidth ignores transforms), and its computed style.
const boxOf = (element) => {
    const rect = element.getBoundingClientRect();
    const style = getComputedStyle(element);
    const width = typeof element.offsetWidth === 'number' ? element.offsetWidth : rect.width;
    const height = typeof element.offsetHeight === 'number' ? element.offsetHeight : rect.height;
    return {width, height, style};
};
const pixels = live.canvases.map((canvas) => {
    try {
        return {url: canvas.toDataURL('image/png'), box: boxOf(canvas)};
    } catch (e) {
        // A canvas that drew another site's image cannot be read.
        return {url: null, box: boxOf(canvas)};
    }
});
const frames = live.videos.map((video) => {
    try {
        if (video.readyState < 2 || !video.videoWidth) return {url: null, box: boxOf(video)};
        const canvas = document.createElement('canvas');
        canvas.width = video.videoWidth;
        canvas.height = video.videoHeight;
        canvas.getContext('2d').drawImage(video, 0, 0);
        return {url: canvas.toDataURL('image/png'), box: boxOf(video)};
    } catch (e) {
        return {url: null, box: boxOf(video)};
    }
});
const imageSources = live.images.map((image) => image.currentSrc || image.src || '');
const values = live.fields.map((field) => {
    if (field.localName === 'select') return Array.from(field.options).map((option) => option.selected);
    if (field.type === 'checkbox' || field.type === 'radio') return field.checked;
    if (field.type === 'password' || field.type === 'file') return null;
    return field.value;
});
const sheets = live.styles.map((node) => node.sheet || null);
const frameBoxes = live.frames.map(boxOf);
const adopted = Array.from(document.adoptedStyleSheets || []);
const title = document.title;
const clone = root.cloneNode(true);
const copies = {
    canvases: pick(clone, 'canvas'),
    images: pick(clone, 'img'),
    videos: pick(clone, 'video'),
    fields: pick(clone, 'input, textarea, select'),
    styles: pick(clone, 'link, style'),
    frames: pick(clone, 'iframe, frame, object, embed'),
};

// --- Resources as data: URLs. ---
const readAsDataUrl = (blob) =>
    new Promise((resolve) => {
        const reader = new FileReader();
        reader.onload = () => resolve(typeof reader.result === 'string' ? reader.result : null);
        reader.onerror = () => resolve(null);
        reader.readAsDataURL(blob);
    });
// The body of `url` as a blob or text within `seconds` (its download included), else null.
const fetchBody = async (url, seconds, asText) => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), seconds * 1000);
    try {
        const response = await fetch(url, {signal: controller.signal, credentials: 'same-origin'});
        if (!response.ok) return null;
        return asText ? await response.text() : await response.blob();
    } catch (e) {
        return null;
    } finally {
        clearTimeout(timer);
    }
};
const dataUrl = (url) => {
    if (!url) return Promise.resolve(null);
    if (/^data:/i.test(url)) return Promise.resolve(url);
    if (!/^(https?|blob):/i.test(url)) return Promise.resolve(null);
    if (!loaded.has(url)) {
        loaded.set(
            url,
            (async () => {
                const blob = await fetchBody(url, 20, false);
                const size = blob ? Math.ceil((blob.size * 4) / 3) + 64 : 0;
                // Taken from the budget before reading, as other resources load at the same time.
                const fits = blob !== null && size <= budget;
                if (fits) budget -= size;
                const result = fits ? await readAsDataUrl(blob) : null;
                if (result) {
                    stats.inlined++;
                } else {
                    if (fits) budget += size;
                    stats.skipped++;
                }
                return result;
            })()
        );
    }
    return loaded.get(url);
};
const cssUrl = /url\(\s*(?:"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'|([^)"'\s]*))\s*\)/g;
const inlineCss = async (text, base) => {
    const found = new Map();
    for (const match of text.matchAll(cssUrl)) {
        const raw = (match[1] ?? match[2] ?? match[3] ?? '').replace(/\\(.)/g, '$1').trim();
        if (!raw || raw.startsWith('#') || /^data:/i.test(raw)) continue;
        const url = absolute(raw, base);
        if (url) found.set(match[0], url);
    }
    const replaced = new Map();
    await Promise.all(
        Array.from(found, async ([token, url]) => {
            const data = await dataUrl(url);
            replaced.set(token, `url("${data || url}")`);
        })
    );
    return text.replace(cssUrl, (token) => replaced.get(token) ?? token);
};
// A stylesheet from its rules: @import expanded, rules added by scripts included.
const sheetText = async (sheet, depth) => {
    let rules = null;
    try {
        rules = sheet.cssRules;
    } catch (e) {
        rules = null;
    }
    const base = sheet.href || document.baseURI;
    if (!rules) {
        // Another site's sheet without CORS: read it again, else it is left out.
        const text = sheet.href ? await fetchBody(sheet.href, 20, true) : null;
        if (text === null) {
            stats.skipped++;
            return '';
        }
        return inlineCss(text, sheet.href);
    }
    let out = '';
    for (const rule of Array.from(rules)) {
        if (rule.type === CSSRule.IMPORT_RULE) {
            if (rule.styleSheet && depth < 8) {
                const inner = await sheetText(rule.styleSheet, depth + 1);
                const media = rule.media ? rule.media.mediaText : '';
                out += media && media !== 'all' ? `@media ${media} {\n${inner}\n}\n` : `${inner}\n`;
            }
            continue;
        }
        out += `${rule.cssText}\n`;
    }
    return inlineCss(out, base);
};
const styleElement = (text, media) => {
    const style = document.createElement('style');
    if (media) style.setAttribute('media', media);
    style.textContent = text;
    return style;
};
// An element that stands where another was, at the size and place it had on screen.
const sized = (element, box, original) => {
    for (const name of ['id', 'class', 'style', 'title']) {
        const value = original.getAttribute(name);
        if (value !== null) element.setAttribute(name, value);
    }
    const style = box.style;
    const keep = ['display', 'position', 'top', 'left', 'right', 'bottom', 'z-index', 'vertical-align', 'float', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left'];
    let css = keep.map((name) => {
        const value = style.getPropertyValue(name);
        // A box in place of an inline frame keeps its size only as an inline block.
        return name === 'display' && value === 'inline' && element.localName !== 'img' ? 'display:inline-block' : `${name}:${value}`;
    }).join(';');
    // The size it had on screen, its border and padding included.
    css += `;box-sizing:border-box;width:${box.width}px;height:${box.height}px`;
    const own = element.getAttribute('style');
    element.setAttribute('style', own ? `${own};${css}` : css);
    return element;
};

const work = [];

// Scripts and what only a running page or the network can use.
for (const element of pick(clone, 'script, noscript, base')) element.remove();
for (const element of pick(clone, 'meta[http-equiv], meta[charset]')) element.remove();
for (const element of pick(clone, '*')) {
    for (const attribute of Array.from(element.attributes)) {
        const name = attribute.name.toLowerCase();
        if (name.startsWith('on')) element.removeAttribute(attribute.name);
        else if (/^\s*javascript:/i.test(attribute.value) && ['href', 'src', 'action', 'formaction', 'xlink:href'].includes(name)) element.removeAttribute(attribute.name);
    }
}

// Stylesheets.
copies.styles.forEach((copy, index) => {
    const node = live.styles[index];
    if (!node || node.localName !== copy.localName) return;
    if (copy.localName === 'style') {
        const sheet = sheets[index];
        if (sheet) work.push(sheetText(sheet, 0).then((text) => (copy.textContent = text)));
        return;
    }
    const rel = (copy.getAttribute('rel') || '').toLowerCase().split(/\s+/);
    const sheet = sheets[index];
    if (rel.includes('stylesheet') && sheet && !sheet.disabled) {
        const media = copy.getAttribute('media');
        work.push(sheetText(sheet, 0).then((text) => copy.replaceWith(styleElement(text, media))));
    } else {
        // Icons, preloads, manifests, disabled or alternate sheets: nothing for an embedded page.
        copy.remove();
    }
});
if (adopted.length) {
    const head = clone.querySelector('head') || clone;
    for (const sheet of adopted) {
        const style = styleElement('', null);
        head.appendChild(style);
        work.push(sheetText(sheet, 0).then((text) => (style.textContent = text)));
    }
}
for (const element of pick(clone, '[style*="url("]')) {
    work.push(inlineCss(element.getAttribute('style'), document.baseURI).then((text) => element.setAttribute('style', text)));
}

// Canvases and videos as pictures of what they show.
copies.canvases.forEach((copy, index) => {
    const shot = pixels[index];
    if (!shot) return;
    const image = sized(document.createElement('img'), shot.box, copy);
    if (shot.url) {
        image.setAttribute('src', shot.url);
        budget -= shot.url.length;
        stats.canvases++;
    }
    image.setAttribute('alt', '');
    copy.replaceWith(image);
});
copies.videos.forEach((copy, index) => {
    const shot = frames[index];
    const poster = copy.getAttribute('poster');
    if (!shot || (!shot.url && !poster)) {
        for (const source of pick(copy, 'source, track')) source.remove();
        copy.removeAttribute('src');
        return;
    }
    const image = sized(document.createElement('img'), shot.box, copy);
    image.setAttribute('alt', '');
    copy.replaceWith(image);
    if (shot.url) image.setAttribute('src', shot.url);
    else work.push(dataUrl(absolute(poster)).then((data) => data && image.setAttribute('src', data)));
});

// Images.
for (const source of pick(clone, 'picture > source')) source.remove();
copies.images.forEach((copy, index) => {
    const url = absolute(imageSources[index] || copy.getAttribute('src') || '');
    copy.removeAttribute('srcset');
    copy.removeAttribute('sizes');
    copy.removeAttribute('loading');
    if (!url) return;
    copy.setAttribute('src', url);
    work.push(dataUrl(url).then((data) => data && copy.setAttribute('src', data)));
});
for (const element of pick(clone, 'image, feImage')) {
    const name = element.hasAttribute('href') ? 'href' : 'xlink:href';
    const url = absolute(element.getAttribute(name) || '');
    if (url && !url.startsWith('#')) work.push(dataUrl(url).then((data) => data && element.setAttribute(name, data)));
}
for (const element of pick(clone, 'input[type="image" i][src]')) {
    const url = absolute(element.getAttribute('src'));
    work.push(dataUrl(url).then((data) => data && element.setAttribute('src', data)));
}

// Frames: an embedded page shows none, so their place stays empty at their size. An <object> or
// <embed> of a picture (an SVG plot) becomes that picture.
const pictureType = /^image\//i;
const pictureFile = /\.(svg|png|jpe?g|gif|webp)([?#]|$)/i;
copies.frames.forEach((copy, index) => {
    const box = frameBoxes[index];
    if (!box) return copy.remove();
    const source = copy.localName === 'object' ? copy.getAttribute('data') : copy.localName === 'embed' ? copy.getAttribute('src') : null;
    const url = source ? absolute(source) : null;
    if (url && (pictureType.test(copy.getAttribute('type') || '') || pictureFile.test(url))) {
        const image = sized(document.createElement('img'), box, copy);
        image.setAttribute('alt', '');
        image.setAttribute('src', url);
        copy.replaceWith(image);
        work.push(dataUrl(url).then((data) => data && image.setAttribute('src', data)));
        return;
    }
    copy.replaceWith(sized(document.createElement('div'), box, copy));
});

// Form fields as they are filled in now.
copies.fields.forEach((copy, index) => {
    const value = values[index];
    if (copy.localName === 'select' && Array.isArray(value)) {
        Array.from(copy.options).forEach((option, at) => (value[at] ? option.setAttribute('selected', '') : option.removeAttribute('selected')));
    } else if (typeof value === 'boolean') {
        if (value) copy.setAttribute('checked', '');
        else copy.removeAttribute('checked');
    } else if (copy.localName === 'textarea') {
        copy.textContent = value || '';
    } else if (value === null) {
        copy.removeAttribute('value');
    } else {
        copy.setAttribute('value', value);
    }
});

// Links to the live page's site stay links to it (an embedded page opens them in the browser).
for (const element of pick(clone, 'a[href], area[href]')) {
    const href = element.getAttribute('href');
    if (href && !href.startsWith('#')) {
        const url = absolute(href);
        if (url) element.setAttribute('href', url);
    }
}

await Promise.all(work);
return JSON.stringify({html: clone.outerHTML, title, inlined: stats.inlined, skipped: stats.skipped, canvases: stats.canvases});
