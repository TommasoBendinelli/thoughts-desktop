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

## Installing

Ready-made Thoughts apps for Windows and macOS are on the
[Releases page](https://github.com/TommasoBendinelli/thoughts-desktop/releases). No GitHub account is needed. To build
Thoughts yourself, see [Building](#building).

Thoughts keeps your Library in sync with a Thoughts server. Before you start, get these from whoever runs the server:

- the **server address**, which starts with `https://`;
- **two device tokens** for this computer: one for the app, and one for the source publisher, which uploads the
  sources you edit. Each computer needs its own two tokens.

<!-- TODO(owner): how a new user gets the server address and the two device tokens (whom to ask, how they are sent). -->

Whoever runs the server creates the tokens with `thoughts-admin create-device`; see "Device provisioning" in the
thoughts-server README.

### Windows

Thoughts runs on 64-bit Windows 10 and 11.

1. On the Releases page, open the newest release named **Thoughts … for Windows** and download
   `Thoughts-<version>-setup.exe`.
2. Run it. The installer is not code-signed, so Windows SmartScreen may say "Windows protected your PC". Choose
   **More info**, then **Run anyway**.
3. The installer needs no administrator rights. It installs Thoughts for your user account into
   `%LOCALAPPDATA%\Programs\Thoughts` and adds it to the Start menu and to **Open with** for PDFs.

To remove Thoughts, use **Settings › Apps**. Your Library (`%USERPROFILE%\Thoughts`) and settings
(`%LOCALAPPDATA%\Thoughts`) stay.

### macOS

Thoughts needs a Mac with Apple silicon (M1 or later) and macOS 13.3 or newer. Intel Macs cannot run it.

1. On the Releases page, open the newest release named **Thoughts … for Mac** and download `Thoughts-<version>.dmg`.
   Mac releases are not marked "Latest", so look further down the page when the first release is for Windows.
2. Open the disk image, drag **Thoughts** onto **Applications**, then eject the disk image.
3. Open Thoughts from Applications. Thoughts is not signed with an Apple Developer ID and not notarized by Apple, so
   macOS blocks the first start and says it could not verify that "Thoughts" is free of malware. Click **Done**, not
   **Move to Trash**.
4. Open **System Settings › Privacy & Security** and scroll down to **Security**. It says that "Thoughts" was blocked.
   Click **Open Anyway** and confirm with your password or Touch ID.
5. Click **Open** in the dialog that follows.

You do this once for each disk image you install yourself. [Installing Thoughts on another
Mac](tools/macos/INSTALL-ON-ANOTHER-MAC.md) has more detail, including how to remove Thoughts completely.

### Linux

There is no ready-made Linux package. Build Thoughts from source as described in [Building](#building).

<!-- TODO(owner): Linux builds have no secret store for device tokens (part/thoughtsplatform.cpp); say how a Linux build gets its two tokens. -->

### Connecting to your server

Do these steps in this order. A token is sent only to the server whose address was saved when the token was stored.

1. **Save the server address.** Open Thoughts and choose **Server…** in the **File** menu (Windows) or the
   **Thoughts** menu (macOS). Enter the address, click **Save**, then quit Thoughts.
2. **Store the two tokens**, with Thoughts closed.

   On Windows, open PowerShell from the Start menu and run these two commands. Each asks for one token: paste the
   app's token into the first and the source publisher's token into the second.

   ```powershell
   Read-Host 'App token' | & "$env:LOCALAPPDATA\Programs\Thoughts\bin\thoughtsctl.exe" store-secret --account thoughts-windows
   Read-Host 'Source publisher token' | & "$env:LOCALAPPDATA\Programs\Thoughts\bin\thoughtsctl.exe" store-secret --account thoughts-source-sync
   ```

   Each command names the server the token is now stored for. Check that it is your server. The tokens are kept in
   Windows Credential Manager.

   On macOS, open Terminal and run these two commands. Each asks for its token twice: paste the app's token into the
   first command and the source publisher's token into the second. The tokens do not show on screen.

   ```sh
   security add-generic-password -U -s thoughts-server -a thoughts-mac -w
   security add-generic-password -U -s thoughts-server -a thoughts-source-sync -w
   ```

   The tokens are kept in your login Keychain. To replace a token, run its command again.
3. **Open Thoughts and click Sync.**

Step 1 can also be done from the command line, with Thoughts closed:

```sh
# Windows (PowerShell)
& "$env:LOCALAPPDATA\Programs\Thoughts\bin\thoughtsctl.exe" set-server https://your-thoughts-server.example
# macOS (Terminal)
/Applications/Thoughts.app/Contents/MacOS/thoughtsctl set-server https://your-thoughts-server.example
```

### First Sync

The first Sync downloads your Library: PDFs, comments, review cards and Project settings. The Library is kept in
`%USERPROFILE%\Thoughts\docs` on Windows and `~/Thoughts/docs` on macOS.

On a new server the Library is empty. Create a Project with **Add project…** in the Library, then click Sync again.

### Phone and agents (optional)

- **Thoughts for Android** is a separate app that syncs with the same server.
  <!-- TODO(owner): where to get Thoughts for Android and how it connects to the server. -->
- **AI agents** (Claude Code, Codex and others) reach the Library through `thoughtsctl` and the MCP server in
  `tools/mcp`. Setting it up needs Python and a copy of this repository; see [tools/mcp/README.md](tools/mcp/README.md),
  and on Windows also "Agents (MCP)" in [tools/windows/README.md](tools/windows/README.md). The Windows installer
  already sets `THOUGHTSCTL`, so the MCP server finds `thoughtsctl.exe`.

### Updating

Thoughts checks for a newer release 30 seconds after it starts, at most once a day, and asks before downloading it.
**Help › Check for Updates…** checks at once. The download is checked against the release's SHA-256; Thoughts then
quits, installs the new version and opens again. Set `THOUGHTS_NO_UPDATE_CHECK=1` to turn off the daily check.

Your Library, settings, server address and tokens are kept outside the app, so an update keeps them.

On macOS, Thoughts can replace itself only if your account may change the folder it is in; in `/Applications` that
means an administrator account. Otherwise it asks you to install the new disk image yourself: quit Thoughts, drag the
new **Thoughts** onto **Applications**, choose **Replace**, and allow it once more with **Open Anyway**.

### Troubleshooting

- **"The server is not configured or its credentials are unavailable."** A token is missing. Store both tokens again
  (step 2 of [Connecting to your server](#connecting-to-your-server)).
- **"This computer's device token belongs to another server."** The tokens were stored while another address was
  saved. Check the address under **Server…**, then store both tokens again.
- **Sync says the server refused the device token.** Store that token again.
- **"The server address is invalid."** Thoughts connects only over `https://`. Correct the address under
  **Server…**. On Windows, Windows must also trust the server's certificate.
- **"Library projects are not configured."** The server is missing a setting (`THOUGHTS_DOCS_ROOT`). Tell whoever runs
  it.
- **"current_pdfs is in use."** Close the PDFs opened from the Library folder and Sync again.
- **A macOS update failed.** The previous version stays and opens again. The log is
  `~/Library/Logs/Thoughts/install-update.log`.
- **Logs:** **Logs…** and **Developer logs…** are next to **Server…** in the menu. On Windows the developer logs are in
  `%LOCALAPPDATA%\thoughts\developer-logs`, and the Sync state and logs in `%LOCALAPPDATA%\Thoughts\thoughts-server`.

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
