# Thoughts on Windows

Thoughts builds and runs natively on 64-bit Windows 10 and 11. It is built with KDE Craft and Microsoft's C++ compiler (MSVC 2022), like Okular's own Windows releases.

What works on Windows is the same as on the Mac:

- the reader;
- Sync, including the source publisher, which is native and needs no Python;
- comments, reviews and proposals;
- `thoughtsctl` and the MCP server;
- "Open in VS Code";
- the build output, Sync log and comment conversation windows, which use PowerShell instead of Terminal.app.

The Windows-specific pieces are in `part/thoughtsplatform.{h,cpp}`:

- **Tokens** are stored in Windows Credential Manager, not the macOS Keychain.
- **`current_pdfs`** is a directory junction instead of a symlink. Open PDFs stay valid when Sync switches to a new generation.
- **File watching** uses `ReadDirectoryChangesW` instead of FSEvents.
- **State** is kept in `%LOCALAPPDATA%\Thoughts` instead of `~/Library/Application Support/Thoughts`.
- **Single instance:** a second launch hands its files and `thoughts://` links to the running Thoughts over a named pipe.

Sync runs two command-line helpers that are built and installed with Thoughts, next to `thoughts.exe`:

- `thoughts-publish-sources.exe` publishes local sources and build inputs and installs changes made on other devices.
- `thoughts-watch-build.exe` is the build output viewer.

Both are built from `tools/publisher`. Three-way merges of sources edited on two devices use Git's own merge code, compiled into `thoughts-publish-sources.exe` (`tools/publisher/xdiff`), so they match the server's `git merge-file`.

Thoughts does not need Git for anything. **Import sources › From Git repository…** clones with libgit2, which the installer ships as `git2.dll` (libgit2 1.7.1 from KDE Craft, GPL-2.0 with the linking exception; HTTPS through Windows' WinHTTP, SSH through `libssh2.dll`). The result is an ordinary Git repository, `.git` included, with `origin` set to the address you entered.

- **Public HTTPS repositories** clone without questions.
- **Private HTTPS repositories:** Thoughts asks once per clone for a user name and a password or access token, keeps them in memory for that clone only and stores nothing. For GitHub use a fine-grained personal access token (GitHub › Settings › Developer settings › Fine-grained tokens) with read-only access to the repository's Contents. Thoughts never sends them over `http://`, and a server whose TLS certificate Windows does not trust is refused.
- **SSH addresses** (`git@github.com:owner/repo.git`, `ssh://…`): Thoughts tries the keys in your SSH agent first (the Windows "OpenSSH Authentication Agent" service, or Pageant), then `id_ed25519`, `id_ecdsa` and `id_rsa` in `%USERPROFILE%\.ssh`, and asks for the passphrase of an encrypted key. Host keys are checked against `%USERPROFILE%\.ssh\known_hosts`: for a host not listed there Thoughts shows its SHA256 fingerprint and adds it only if you trust it; a host whose key changed is refused with a warning.

## Install from a release (no build needed)

Each Windows release is a GitHub Release of the desktop repository it was built from, tagged `windows-<version>`, with one installer, `Thoughts-<version>-setup.exe`. To publish a new one, see "Publishing a release" below.

1. **Get access.** If that repository is private, only its collaborators can see the releases; its owner invites each person under the repository's Settings › Collaborators (Read access is enough).
2. **Download the installer** from the repository's Releases page (`https://github.com/<owner>/<repository>/releases`), signed in to GitHub if it is private.
3. **Run it.**
   - The installer is not code-signed. If Windows SmartScreen says "Windows protected your PC", choose **More info › Run anyway**.
   - It installs for the current user into `%LOCALAPPDATA%\Programs\Thoughts` and needs no administrator rights.
   - It registers the Start menu entry, `thoughts://` links and PDF **Open with › Thoughts**, like `register.ps1`. It also sets `THOUGHTSCTL` for the agents' MCP server.
   - It can be removed under Settings › Apps; your Library and settings in `%LOCALAPPDATA%\Thoughts` stay.
4. **Connect to the server**, with `%LOCALAPPDATA%\Programs\Thoughts\bin\thoughtsctl.exe` instead of `C:\CraftRoot\bin\thoughtsctl.exe` in the steps below:
   - Create two devices for the PC on the server (step 1.3).
   - Store their tokens and, unless the installer was built for your server, save your server's address **before you start Thoughts the first time** (step 3.5).
   - Only if you are moving an existing Library from a Mac: also do steps 1.1, 1.2 and 3.6.
   - Start Thoughts and press Sync (step 3.7).

### Updates

Thoughts checks for a newer release 30 seconds after it starts, at most once a day, and asks before downloading. **Help › Check for Updates…** checks at once.

- The installer it downloads is checked against the release's SHA-256.
- Thoughts closes, the installer replaces the program files, and Thoughts opens again.

Releases come from the repository the installer was built for (`THOUGHTS_UPDATE_REPOSITORY`; the published installers use the public `TommasoBendinelli/thoughts-desktop`). A public repository needs no token. When the installer was built for a private repository, GitHub refuses the check; **Check for Updates…** then asks for a GitHub token and stores it in Credential Manager as `thoughts-github/releases`. To make the token:

1. Go to GitHub › Settings › Developer settings › **Fine-grained tokens** › Generate new token.
2. Under **Repository access**, choose *Only select repositories* › the desktop repository.
3. Under **Repository permissions**, set **Contents** to *Read-only*.

Set `THOUGHTS_NO_UPDATE_CHECK=1` to stop the daily check.

### Publishing a release

On the Mac, from a clean clone of the public repository whose `HEAD` is on its `main` (the script refuses a published release from anything else, and from a checkout with a private remote):

```sh
export THOUGHTS_WINDOWS_BUILDERS=builder.example.org   # SSH host(s) of the Linux builder
python3 tools/windows/release.py            # builds and publishes windows-YYYY.MM.DD.N
python3 tools/windows/release.py --no-publish   # only builds the installer
```

- The build runs on a Linux builder, not on the Mac. It is cross-compiled with clang-cl against KDE Craft's Windows libraries; see `tools/windows/cross/README.md`.
- The release goes to `TommasoBendinelli/thoughts-desktop` unless `--repository` or `THOUGHTS_RELEASE_REPOSITORY` names another; the installer checks that repository for updates.
- Before the installer is packed, its files are scanned for personal or private strings (`tools/release_scan.py`: home folders, tailnet hosts, keys, tokens, and your own markers from `~/.config/thoughts/release-private-patterns.txt`). A hit stops a published release. The scan runs on the builder with your markers and the generic hits you accept (`~/.config/thoughts/release-accepted.txt`), which the script copies there; a marker is never accepted. The programs are built with neutral source paths (`src\…`, `wincross\…`), so the builder's folder does not show up.
- The release notes default to the commit subjects since the previous release.
- The installer is kept in `~/.cache/thoughts-windows-releases`.

The rest of this guide builds Thoughts from source on the PC instead.

## 1. Before you start: on the server (and on the Mac, if you move a Library)

"Fedora" below is the Thoughts server. If you start with your own, new server, skip steps 1 and 2: they only copy an existing Library's sources from a Mac.

1. **Sync on the Mac** (only when moving an existing Library). The copy of your sources you make next must match Fedora.

2. **Export the source folders** (only when moving an existing Library). Sync downloads PDFs, comments, reviews and Project settings, but not the editable `docs-*` folders:
   ```sh
   python3 tools/windows/export-sources.py ~/Desktop/thoughts-sources.zip
   ```
   This packs about 140 MB: sources and build inputs, without build outputs, `tmp`, `.git` or secrets.
   - Copy the archive to the PC.
   - Do **not** copy `~/Library/Application Support/Thoughts`. Its recorded bases would let the PC publish deletions for files it lacks.

3. **Create two devices for the PC on Fedora.** Each machine needs its own, because operation sequence numbers are per device:
   ```sh
   sudo -u thoughts bash -c 'set -a; . /etc/thoughts/server.env; set +a
     /srv/thoughts/venv/bin/thoughts-admin create-device thoughts-windows --name "Thoughts on Windows"
     /srv/thoughts/venv/bin/thoughts-admin create-device thoughts-windows-source-sync --name "Thoughts Windows Source Publisher"'
   ```
   - Each command prints a token once. Keep both for step 3.5.
   - If the `thoughts` user cannot read `server.env`, pass `--database-url` with the value from that file instead.
   - These paths are those of a server installed with thoughts-server's `deploy/fedora`. On another installation, run `thoughts-admin create-device` the same way, with that server's environment (see "Device provisioning" in thoughts-server's README).
   - Thoughts only connects over `https://`, and Windows must trust the server's certificate.
   - The server needs `THOUGHTS_DOCS_ROOT` (`deploy/fedora/server.env.example` sets it). Without it, Sync fails with "Library projects are not configured".

## 2. Install the prerequisites on the PC

Install these first; none needs anything special:

- **Visual Studio 2022** (Community or Build Tools) with the "Desktop development with C++" workload.
- **Git for Windows**, only to clone the sources below; Thoughts itself does not use it. Before cloning anything, run `git config --global core.autocrlf false`. Thoughts hashes sources byte for byte, so Git must not convert line endings.
- **Network access to the Thoughts server.** New installations use the build's default server (`THOUGHTS_DEFAULT_SERVER_URL`, `https://thoughts.example.org` unless the build names another); to use your own, save its address in step 3.5.

Optional:

- **Python 3.12 or newer**, only for the agents' MCP server (see "Agents" below).
- **VS Code**, for "Open source".
- **MiKTeX or TeX Live**, for `synctex`, which anchors comments in sources.
- **The Codex CLI** (`npm install -g @openai/codex`), for comment conversations.

## 3. Build, install and connect

All commands run in a normal (not administrator) PowerShell.

1. **Clone both repositories side by side.** The build uses the C++ client SDK from `thoughts-server`:
   ```powershell
   mkdir $env:USERPROFILE\repos; cd $env:USERPROFILE\repos
   git clone <thoughts-desktop repository URL> thoughts-desktop
   git clone <thoughts-server repository URL> thoughts-server
   ```

2. **Install KDE Craft and Thoughts' dependencies.** This takes a while and downloads a few GB of prebuilt packages:
   ```powershell
   powershell -ExecutionPolicy Bypass -File thoughts-desktop\tools\windows\setup-craft.ps1
   ```
   When Craft's installer asks, choose:
   - the folder `C:\CraftRoot`;
   - the MSVC 2022 x86_64 compiler;
   - Qt 6;
   - the prebuilt binary cache.

3. **Build Thoughts and install it into `C:\CraftRoot`:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File thoughts-desktop\tools\windows\build.ps1
   ```
   Add `-Tests` to also build the unit tests (see "Checking the build" below).
   Add `-ServerUrl https://…` to make new installations start with your server instead of the build's default (`THOUGHTS_DEFAULT_SERVER_URL`).

4. **Register Thoughts** for the Start menu, `thoughts://` links and "Open with" for PDFs:
   ```powershell
   powershell -ExecutionPolicy Bypass -File thoughts-desktop\tools\windows\register.ps1
   ```

5. **Connect to your server.** With a Thoughts account (a username and password, or an invitation link), skip this step: when Thoughts first starts without a token it opens **Sign In**, and signing in stores the tokens. Otherwise save your server's address, then store the two tokens from step 1.3. The address comes first: `store-secret` ties each token to the address saved at that moment, and the token is sent only there. Skip the address only if this build was made for your server (`-ServerUrl`).
   ```powershell
   C:\CraftRoot\bin\thoughtsctl.exe set-server https://thoughts.example.org
   ```
   Each token is read from standard input, so it doesn't end up in your history:
   ```powershell
   Read-Host 'Desktop token (thoughts-windows)' | C:\CraftRoot\bin\thoughtsctl.exe store-secret --account thoughts-windows
   Read-Host 'Publisher token (thoughts-windows-source-sync)' | C:\CraftRoot\bin\thoughtsctl.exe store-secret --account thoughts-source-sync
   ```
   Each command names the server the token is now stored for. They appear in Credential Manager as `thoughts-server/thoughts-windows` and `thoughts-server/thoughts-source-sync`.
   Do this before Thoughts first starts: Thoughts contacts its server as soon as it starts. `THOUGHTS_SERVER_URL` overrides the saved address.

6. **Unpack the sources** from step 1.2 (only when moving an existing Library) into the docs folder, which is `%USERPROFILE%\Thoughts\docs` unless `THOUGHTS_HOME` says otherwise:
   ```powershell
   Expand-Archive thoughts-sources.zip "$env:USERPROFILE\Thoughts\docs"
   ```

7. **Start Thoughts** from the Start menu and press Sync.
   - The first Sync downloads the Library: PDFs, comments, reviews and Project settings.
   - If you unpacked sources in step 6, it adopts them; they match Fedora.
   - On a new server the Library is empty. Create a Project with **Add project…** in the Library and press Sync again.

## Agents on this PC (Claude Code)

The installer brings what a person needs to use their own Claude Code with Thoughts on this PC, as the owner does on a Mac, with nothing else to install but Claude Code:

- **A Python of its own** (`python-<id>\`, 57 MB installed; with it the installer grew from 56 MB to 73 MB): the official CPython embeddable package for Windows amd64 from python.org and the win_amd64 wheels the tools need from PyPI, every file pinned by its SHA-256 in `agent-tools/lock.json` (`agent-tools/agent_tools.py`; `<id>` changes with the pinned files, so an update never writes over the Python a running Claude Code session holds).
- **The Thoughts tools** (`tools\mcp\`) and **the reply waker** (`tools\waker\`), with `tools\agent-tools.json` naming them.
- **Settings › Agent prompts › Claude Code on this computer**: *Set up Claude Code* finds `claude` (the native installer's `%USERPROFILE%\.local\bin\claude.exe`, then `PATH`, then npm's `claude.cmd`) and runs `claude mcp add --scope user thoughts -e THOUGHTSCTL=<bin\thoughtsctl.exe> -- <python-<id>\python.exe> -X utf8 -B <tools\mcp\server.py>`, replacing an earlier `thoughts` entry. The tools read this PC's Thoughts sign-in from Windows Credential Manager (`thoughts-server/thoughts-windows`) and the server address from `%LOCALAPPDATA%\Thoughts\server.json`. After an update that brings another Python, Thoughts registers them again by itself.
- *Let Claude Code sessions on this computer answer* (on once Claude Code is set up): while Thoughts runs and is signed in, it runs the reply waker with `pythonw.exe` (home `%LOCALAPPDATA%\Thoughts\reply-waker`: `config.json`, `state.db`, `logs\waker.log`). The waker registers this PC's Claude Code sessions (from `%USERPROFILE%\.claude\sessions`), so they show in Send to Agent and Ask Agent, and brings them the pages and replies that are theirs: a running one by a message, a closed one by resuming it in its folder. Thoughts starts it again when it ends, stops it when Thoughts quits (it ends with Thoughts even after a crash), and never lets it start sessions: Send to Agent's *New session on this computer* and the phone's *Start a new study session* are started by Thoughts itself, in `%USERPROFILE%\Thoughts Sessions`.

Once on the PC: install Claude Code and sign in (`claude` in a terminal), choose *Set up Claude Code*, and run `claude` once in `%USERPROFILE%\Thoughts Sessions` to trust that folder (Thoughts offers *Open Terminal Here* when a start finds it untrusted). Uninstalling Thoughts leaves Claude Code's `thoughts` entry; `claude mcp remove --scope user thoughts` removes it.

### Your own Python instead

`tools/mcp/server.py` works on Windows. It finds `thoughtsctl.exe` on `PATH`, or at `C:\CraftRoot\bin\thoughtsctl.exe`; `THOUGHTSCTL` overrides both. It starts `thoughtsctl.exe` without flashing a console window. Set it up as on the Mac (`tools/mcp/README.md`), with a Windows virtual environment:

```powershell
py -3 -m venv $env:LOCALAPPDATA\Thoughts\mcp-venv
& $env:LOCALAPPDATA\Thoughts\mcp-venv\Scripts\pip install -r thoughts-desktop\tools\mcp\requirements.txt
```

Then register `...\mcp-venv\Scripts\python.exe thoughts-desktop\tools\mcp\server.py` as a stdio MCP server.

## Checking the build

`build.ps1 -Tests` builds the unit tests into `build-windows\bin`. Run them from Craft's environment (`. C:\CraftRoot\craft\craftenv.ps1`):

```powershell
cd thoughts-desktop\build-windows
ctest -R "thoughtsplatform|desktopsyncstore|docscommentmutationstore|frozenpublicationbundle|projectreadinggeneration|thoughtscommandprotocol|markdownfolderimport" --output-on-failure
```

These Windows builds have already passed on a Mac, cross-compiled with clang-cl and run under Wine:

- The native publisher matched the (since retired) Python publisher on the same files:
  - the source manifest, the progress lines, the candidates, and every build-input entry and snapshot hash;
  - publications and editable-source installs against a local thoughts-server, compared step by step: output, events, state, and the files written on both sides. 19 of 20 scenarios matched, including three-way merges through Git for Windows.
  - `thoughts-watch-build`: 26 of 27 scenarios matched.
  - Where they differed, the message came from Windows itself: a missing path is reported as `[WinError 3] … 'C:\\…'`.
- `tools/publisher/tests` holds golden tests (`publisher_golden.py`, `publish_golden.py`, `watch_golden.py`) with expected outcomes recorded on the Mac. Pass `--windows` to `publish_golden.py` and `watch_golden.py` for a Windows build; messages that come from Windows itself still differ.

- `thoughtsplatformtest`: 11 of 11.
- `parttest`, one function per run: 162 of 244 pass and 19 skip. The skipped ones need POSIX shell helpers, a real synctex, or opt-in setups.
- The journal, identity, publication bundle, reading generation, command protocol, transfer ledger, configuration cache, HTML attachment, proposal authoring, review tools, developer log and style tests: all pass.
- `thoughts.exe` opened its Library window. `thoughtsctl.exe` opened a PDF through it and started a Sync over the named pipe.

What still needs real Windows:

- **Six `desktopsyncstoretest` cases** need change notifications from subfolders, which Wine on macOS does not deliver.
- **`markdownfolderimporttest`** needs no Git; with WinHTTP its HTTPS sign-in case skips itself, because only libgit2's OpenSSL backend can trust the test's own certificate.
- **57 `parttest` functions hang under Wine.** They open PDFs from a watched `current_pdfs`, and the hang happens while the test removes its temporary folder.
- **Five `parttest` functions fail under Wine's offscreen display:** text selection (`testSelectText`, `testCopyTextSelectionModes`), the cursor shape (`testTabletProximityBehavior`), the proposal window (`testAnnotationSidebarAgentProposals`) and the review mask (`testFlippedReviewMaskHidesOnlyTheContext`).
- `testDeviceSequenceConflictRecovery` also fails on the Mac, on main.

To run all of `parttest` on the PC (it takes a while): `ctest -R parttest --output-on-failure`, or `bin\parttest.exe <function>` for one function.

## Troubleshooting

- **"This computer is not signed in to the server":** choose **Sign In…** in the Sync menu (or File → Server…) and use your Thoughts username and password. Without sign-in on the server, repeat step 3.5 for a device token.
- **Sync says the source publisher is unavailable:** `thoughts-publish-sources.exe` is missing next to `thoughts.exe`. Run `build.ps1` again, which checks for it.
- **"current_pdfs is in use":** `current_pdfs` existed as an ordinary folder before the first Sync. Close the PDFs opened from it and Sync again. Thoughts sets the old folder aside as `.thoughts-library-legacy-*`; it deletes nothing.
- **Logs:**
  - Developer logs are in `%LOCALAPPDATA%\thoughts\developer-logs`.
  - Sync state and logs are in `%LOCALAPPDATA%\Thoughts\thoughts-server`.
