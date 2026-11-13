# Thoughts

Thoughts is a source-linked document workspace for the desktop. It shows the PDFs of a Thoughts Library next to the
LaTeX and Markdown sources they are built from, and keeps both in sync with a Thoughts server, which builds the PDFs,
holds comments, review cards and their history, and shares them with Thoughts for Android.

By **Tommaso Bendinelli**. Built on [Okular](https://invent.kde.org/graphics/okular); see
[Acknowledgements](#acknowledgements).

## What it does

- **Library**: Projects of LaTeX and Markdown notes and imported PDFs, opened in tabs, with a Library overview.
- **Source-linked reading**: move between a passage of the PDF and its source; comments are anchored to source
  passages and follow edits.
- **Review cards**: turn a passage into a question-and-answer card and review it on a schedule, on the desktop or the
  phone.
- **Agent proposals**: AI agents (Claude, Codex and others) suggest edits that appear in the PDF and the source, and you
  accept or reject each one. Agents reach the Library through the `thoughtsctl` command line and the MCP server in
  `tools/mcp`, which also sends agent notes to the phone.
- **Sync** with a Thoughts server: sources, comments, review history and the server-built PDFs.

## Repository layout

| Path | Contents |
| --- | --- |
| `shell/`, `part/` | The Thoughts application and its document view (Okular's shell and part, extended) |
| `core/`, `generators/`, `ui/` | Okular's document model, format backends and views |
| `tools/thoughtsctl.cpp` | `thoughtsctl`, the command line the agent tools use |
| `tools/mcp/`, `tools/hooks/` | The MCP server and prompt hooks for coding agents |
| `tools/publisher/`, `tools/thoughts-comment-sync.cpp` | Source publication and comment synchronization helpers |
| `tools/vscode-pdf-links/`, `tools/vscode-source-selection/` | VS Code extensions |
| `tools/macos/`, `tools/windows/` | Packaging for macOS and Windows |
| `autotests/` | Tests |

Related repositories: the Thoughts server (with the client SDKs and `thoughts-cli`), Thoughts for Android, and the
Thoughts connector for ChatGPT and Claude.

## Building

Thoughts builds with CMake and Ninja against Qt 6 and KDE Frameworks 6, and against the C++ client SDK of a
thoughts-server checkout next to this one:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
    -DTHOUGHTS_SERVER_SDK_ROOT=/path/to/thoughts-server/sdk/cpp \
    -DTHOUGHTS_DEFAULT_SERVER_URL=https://your-thoughts-server.example
cmake --build build
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

[AGENTS.md](AGENTS.md) has the full build and test notes and every build option; `tools/windows/README.md` and
`tools/macos/README.md` cover packaging.

## Acknowledgements

Thoughts was originally built on top of [Okular](https://invent.kde.org/graphics/okular), KDE's universal document
viewer. We are very grateful to the Okular developers and the KDE community.

The files that come from Okular keep their authors' copyright notices and licences (see the SPDX headers and
[`LICENSES/`](LICENSES/)); [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists the other bundled code.

## License

Thoughts is licensed under the GNU General Public License, version 2 or (at your option) any later version, like
Okular. Each file states its licence in an SPDX header; the licence texts are in [`LICENSES/`](LICENSES/).

Copyright © 2026 Tommaso Bendinelli; copyright of the code inherited from Okular remains with its authors.
