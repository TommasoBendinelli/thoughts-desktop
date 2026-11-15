# Contributor notes

Thoughts is a fork of [Okular](https://okular.kde.org). Files that came from Okular keep their own copyright lines and
licences; see `THIRD-PARTY-NOTICES.md` for code from elsewhere.

If `AGENTS.local.md` exists at the repository root, read it too: it holds the maintainer's machine-specific instructions.

## Build and test

Thoughts builds with CMake and Ninja against Qt 6 and KDE Frameworks 6 (on macOS and Windows most easily from
[KDE Craft](https://community.kde.org/Craft)). It also needs the C++ client SDK of the Thoughts server: a checkout of
thoughts-server next to this repository (`../thoughts-server/sdk/cpp`, the default), or `THOUGHTS_SERVER_SDK_ROOT`
pointing at another checkout's `sdk/cpp`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
    -DCMAKE_PREFIX_PATH="$HOME/CraftRoot" \
    -DTHOUGHTS_SERVER_SDK_ROOT=/path/to/thoughts-server/sdk/cpp
cmake --build build
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

- If the documentation build fails for want of kdoctools catalogs, add `-DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON`.
- Run one test function with `QT_QPA_PLATFORM=offscreen build/bin/parttest testName`. `parttest` is long; never run two
  `parttest` processes at once.
- Every autotest runs with a temporary `HOME`, Library and application data, and an unreachable server
  (`autotests/testhome.cpp`); it refuses to start if it could reach real user data or the build's own servers.
- The Python tools have their own tests: `tools/mcp/test_server.py` and `tools/mcp/test_agent_notes.py` (in a virtual
  environment with `tools/mcp/requirements.txt`), and `tools/hooks/test_thoughts_card_link.py` and
  `tools/test_release_scan.py` (standard library only). `sh tools/macos/test-install-update.sh` tests the Mac updater
  on throwaway bundles.
  `tools/vscode-pdf-links` runs `npm test`.
- Windows: `tools/windows/README.md`. macOS signing (optional), the disk image and releases: `tools/macos/README.md`.

Build options specific to Thoughts:

| Option | Default | Meaning |
| --- | --- | --- |
| `THOUGHTS_SERVER_SDK_ROOT` | `../thoughts-server/sdk/cpp` | The thoughts-server C++ client SDK to build against. |
| `THOUGHTS_DEFAULT_SERVER_URL` | `https://thoughts.example.org` | The server a new installation starts with. An installation keeps the address it saved (File › Server…, `thoughtsctl set-server`); `THOUGHTS_SERVER_URL` overrides it for one process. |
| `THOUGHTS_LEGACY_SERVER_URL` | empty | An earlier fixed server address that installations which synchronized before saving an address keep. Empty: none. |
| `THOUGHTS_HTML_VIEW` | on when Qt WebView is found | The HTML view of documents. |
| `THOUGHTS_MACOS_PERSISTENT_SIGNING` | on when the identity of `tools/macos/setup-signing.py` exists | Sign the macOS app bundle on build and install. |
| `THOUGHTS_RELEASE_VERSION`, `THOUGHTS_UPDATE_REPOSITORY` | empty | Version and GitHub repository of a published Windows or Mac release; Help › Check for Updates exists only with them. The release tools (`tools/windows/release.py`, `tools/macos/release.py`) set both, the repository to `TommasoBendinelli/thoughts-desktop` unless told otherwise. |

On macOS, quit Thoughts normally before replacing or signing an installed copy, and install matching core, part,
generator and shell components with the generated CMake installers (`tools/macos/install-local.py` does both), so the
Craft dependencies and RPATHs stay consistent.

## Commit discipline

- Commit completed, coherent changes regularly and before handing off a finished coding task.
- During longer work, create clearly labeled checkpoint commits at useful milestones rather than letting changes accumulate across tasks.
- Before committing, inspect the diff, preserve unrelated work, and exclude credentials, local configuration, signing material, and generated build outputs.
- Run checks appropriate to the change and record any incomplete validation in the commit or handoff.
- Report the commit hash, whether it was pushed, and whether the running application or server uses that code as separate facts.
