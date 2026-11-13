# Thoughts PDF Links

This local VS Code extension connects VS Code to the custom macOS application
with bundle identifier `org.tbend.thoughts`.

It lives in `tools/vscode-pdf-links` of the thoughts-desktop repository and is
installed by hand as `local.thoughts-pdf-links`. Unlike
`tools/vscode-source-selection`, the desktop shell does not bundle or install it.

VS Code uses the custom editor only as an opening hook. The extension launches:

```text
/usr/bin/open -b org.tbend.thoughts <absolute-pdf-path>
```

and immediately closes the temporary editor tab. Arguments are passed without a
shell, so spaces and other ordinary path characters are preserved.

The corresponding VS Code user setting is:

```json
"workbench.editorAssociations": {
  "*.pdf": "thoughts.externalPdf"
}
```

Only local `file:` resources are supported. Remote-workspace PDFs must first be
copied to the Mac.

## Linked LaTeX tabs

For the docs workspace, a linked `.tex` tab has an **Open PDF in Thoughts**
action in its tab context menu. The action opens the canonical annotated PDF
from `current_pdfs/` and preserves Thoughts' normal remembered position
and tab behavior.

Visibility comes from the renderer's authoritative
`comments/.layout/*.json` files. The action is absent for a source that is not
listed in a current layout, or when either the source or canonical PDF is
missing. Layout and PDF changes are detected automatically.

## Following document links in LaTeX sources

In a `.tex` source of the docs workspace, Ctrl/Cmd+click (or F12, Go to
Definition) anywhere on a Thoughts document link macro opens its target. The
targets resolve as the publisher resolves them:

| Macro | Target argument | Label argument |
| --- | --- | --- |
| `\SourceLink{path}{label}{text}` | path inside the linking document's source folder (`docs-<name>` or a Project's `.thoughts/projects/<hash>/default`, `.../documents` before the 2026-10-17 rename) | `\SourceLabel{label}` in LaTeX, `{#source-label:label}` heading identifier in Markdown |
| `\CrossSourceLink{docs-x/path}{label}{text}` | docs-root relative path | as for `\SourceLink` |
| `\PdfLink{key}{anchor}{text}` | `key.pdf` selected in the linking folder's Project, else `third_party/key.pdf` | ignored |
| `\CrossPdfLink{docs-x/key}{anchor}{text}` | `key.pdf` selected in the Project of `docs-x` | ignored |

A target path that is a single `.tex` file name is also looked up as
`tex/source/<name>`, matching the publisher's alias. The retired `\TexLink` and
`\CrossTexLink` are no links: the build leaves them inactive, so they have no
definition here either; rewrite them as `\SourceLink` / `\CrossSourceLink`
and mark the target with `\SourceLabel`. Labels are searched
in the target file and the files it includes with `\input`, `\include` or
`\subfile` (resolved against the source folder), skipping TeX comments; unsaved
editor contents are used when the file is open. When the label is not found the
target file opens at its first line; when the file does not exist the link has
no definition.

PDF links are document links: Ctrl/Cmd+click opens the PDF through the
**Open in Thoughts** custom editor. Thoughts opens the PDF at its remembered
position; the anchor is not passed on. Source links deliberately have no
document link so that Ctrl/Cmd+click navigates only once, to the label.

## Testing, building and installing

Run these commands from `tools/vscode-pdf-links`.

Run `npm test` for the resolver tests. `test/vscode-integration.js` checks the
registered providers inside VS Code; run it with an isolated profile (keep the
profile path short, VS Code's socket path must fit the Unix limit):

```sh
code --user-data-dir "$(mktemp -d)" --extensions-dir "$(mktemp -d)" \
  --extensionDevelopmentPath "$PWD" \
  --extensionTestsPath "$PWD/test/vscode-integration.js"
```

Build and install the VSIX (built files in `dist/` are not committed):

```sh
npx @vscode/vsce package --no-dependencies --allow-missing-repository \
  -o dist/thoughts-pdf-links-<version>.vsix
code --install-extension dist/thoughts-pdf-links-<version>.vsix --force
```
