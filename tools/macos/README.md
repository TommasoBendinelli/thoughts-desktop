# Persistent signing for local Thoughts builds

Optional. Without it, macOS treats every rebuilt Thoughts as a new app, so privacy permissions (for example access to Documents) and Keychain access are asked for again after updates.

Run `python3 tools/macos/setup-signing.py` once on this Mac.
It creates a ten-year local code-signing identity in the user's login keychain.
Only `/usr/bin/codesign` gets automatic private-key access, and certificate trust is limited to the user's code-signing policy.
The private key is imported as non-extractable; temporary key material is removed after import.
The public certificate and fingerprint are stored under `~/Library/Application Support/Thoughts/signing`.
Preserve that keychain identity across rebuilds; do not delete and regenerate it to fix a build failure.
This is local development signing, not Developer ID signing or notarization for public distribution.

The CMake option `THOUGHTS_MACOS_PERSISTENT_SIGNING` (on by default when `identity.txt` exists at configure time) makes macOS builds and shell component installs call `sign-app.py`.
With it on, signing deliberately fails if the permanent identity is missing or inaccessible; it never falls back to ad hoc signing.
The app identifier remains `org.tbend.thoughts`, and its designated requirement pins the persistent certificate rather than the changing executable hash.
The bundled CLI helpers also receive stable identifiers and the same signer.

`tools/macos/install-local.py` installs a stopped build from `build-thoughts` into `~/CraftRoot` and `/Applications/Thoughts.app` with the generated installers, keeps a backup of the replaced files, and signs the result.
To sign an installed `/Applications/Thoughts.app` bundle yourself, quit Thoughts normally, preserve the matching CraftRoot runtime installation procedure, then run:

```sh
python3 tools/macos/sign-app.py /Applications/Thoughts.app
```

Do not append `codesign --sign -` to deployment commands: it would replace the persistent identity.
The script refuses to modify an app bundle whose main executable is running.
Sign a staging copy first if Thoughts must remain open.
After the first transition from ad hoc signing, macOS may require one more Documents permission approval while unlocked.
Permission persistence across later updates still needs a real user-session check; signature verification alone does not prove that.

Apple's explanation: https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements

## Libraries

The installed app loads its libraries from `~/CraftRoot/lib` (the executables' RPATH); none is copied into the bundle, so `sign-app.py` signs only Thoughts' own executables.
The disk image below is the exception: it carries its own copies.
Import sources › From Git repository needs libgit2 (GPL-2.0 with the linking exception) in CraftRoot: `craft libgit2` installs Craft's prebuilt 1.7.1, which uses macOS' Security framework for HTTPS and CraftRoot's libssh2 for SSH.
No installed Git is needed.

## Disk image for another Mac

```sh
python3 tools/macos/make-dmg.py
```

This writes `dist/Thoughts.dmg` (plus its `.sha256`), a `Thoughts.app` that runs without `~/CraftRoot`.
Run it again for each new version: it configures and builds `build-dmg` from the checked-out commit, so commit first.
`--skip-build` packages `build-dmg` as it is.
The other Mac's owner follows `INSTALL-ON-ANOTHER-MAC.md`, which is also copied into the image as `Install Thoughts.md`.

What the script needs, and where each value comes from (none is written in the repository):

| Value | Given by | Default |
| --- | --- | --- |
| KDE Craft root with Qt 6, KDE Frameworks 6, Poppler and libgit2 | `THOUGHTS_CRAFT_ROOT` | `~/CraftRoot` |
| thoughts-server C++ SDK | `--server-sdk`, `THOUGHTS_SERVER_SDK_ROOT` | `../thoughts-server/sdk/cpp` |
| Signing identity (SHA-1 fingerprint of the certificate) | `~/Library/Application Support/Thoughts/signing/identity.txt`, written by `setup-signing.py` | none: the script stops without it |
| Server a new installation starts with | `--server-url` | the build's `https://thoughts.example.org` |
| Oldest macOS | `--macos-min` | 13.3 |

Craft's own packager (`craft --package okular`) does not fit, for three reasons. Thoughts is built outside Craft, so Craft has no okular image directory to package. Its dylib bundler ad hoc signs (`codesign -s -`) every binary it changes. And it writes into `~/CraftRoot/build`.
`make-dmg.py` takes the same approach as Craft's packager, with the persistent identity.

The script never writes to `~/CraftRoot` or `/Applications`.
It installs `build-dmg` with `DESTDIR` into `build-dmg/dmg-stage` and fails if any installed path lands outside it.
Then it assembles `build-dmg/dmg-package/Thoughts.app`:

- `Contents/PlugIns`: okularpart and the Okular generators from this build, and from `~/CraftRoot/plugins` the Qt and KDE plugins Thoughts loads (the `PLUGINS` list in the script).
- `Contents/Frameworks`: every library and framework those and the executables reference, found by following their load commands.
- `Contents/Resources`: Okular's data (`okular/`, `icons/hicolor`) and a `qt.conf`. The kxmlgui `.rc` files are compiled in, the icon theme comes from `libKF6BreezeIcons`, and MIME types come from Qt's built-in database, as in the installed app. Translations are left out, as the installed app has none either.

Every install name and reference becomes `@executable_path/../Frameworks/…` (or `../PlugIns/…`), and every RPATH becomes `@executable_path/../Frameworks`.
Every library, framework and plugin is then signed with the persistent identity, and the helpers and the app with `sign-app.py`.
The script fails unless `otool -L` and `otool -l` show no `/Users/` path in any Mach-O, every reference resolves inside the bundle or to the system, no Mach-O needs a newer macOS than `--macos-min` (the `minos` of `LC_BUILD_VERSION`), and `codesign --verify --deep --strict` passes, both on the bundle and inside the mounted image.
The results are in `build-dmg/dmg-report.txt`.

The minimum macOS is 13.3 (`--macos-min`), the oldest the bundled CraftRoot libraries support (libcrypto needs 13.3, the others 13.0); libc++'s `std::to_chars` for doubles, which `tools/publisher/pyjson.cpp` writes numbers with, needs 13.3 too.
For that, `tools/publisher/pyjson.cpp` reads floating-point JSON numbers with `strtod_l` in the C locale on macOS instead of `std::from_chars`, which Apple's libc++ has only from macOS 26.
Sync and publication hashes depend on those doubles, so `autotests/pyjsonfloattest.cpp` and `tools/publisher/tests/pyjson_differential.py` check that they are the ones Python's `json.loads` reads.
The app is arm64 only.

### Trying the image without risking the real server

Never open the packaged app from your own account, not even with another `HOME`: it would use your Keychain's device token against the live server.
Use a separate macOS account instead:

1. In **System Settings › Users & Groups**, add a **Standard** (not Administrator) user, for example `example-tester`.
2. Copy `dist/Thoughts.dmg` to `/Users/Shared`.
3. Log in as that user (**Control Center › Fast User Switching**, or log out first).
4. Open `/Users/Shared/Thoughts.dmg` and drag **Thoughts** into **`~/Applications`** in that account (create the folder if needed). Do not drag it into `/Applications`: that would replace your own installed app.
5. Open it from `~/Applications` and follow **Open it the first time** in `INSTALL-ON-ANOTHER-MAC.md`.
6. Open a PDF, an HTML page and the Library window. Without a device token, Sync should report that the server is not configured or that its credentials are unavailable. Store no token in that account.
7. In Terminal, in that account, check that nothing is loaded from your CraftRoot. Standard users can read your home folder, so a leak would not fail by itself. This command, with your own account's name in place of `alex`, should print nothing:

   ```sh
   lsof -p "$(pgrep -f 'Thoughts.app/Contents/MacOS/thoughts$')" | grep -E '/Users/alex|CraftRoot'
   ```

8. Quit Thoughts and log out. Delete the account afterwards if you like.

### Releases and updates

```sh
python3 tools/macos/release.py              # builds and publishes mac-YYYY.MM.DD.N
python3 tools/macos/release.py --no-publish # only builds the disk image
```

A published release is built from a clean clone of the public repository whose `HEAD` is on its `main`; the script refuses anything else, and a checkout with a private remote.
It runs `make-dmg.py` with `THOUGHTS_RELEASE_VERSION=YYYY.MM.DD.N` and `THOUGHTS_UPDATE_REPOSITORY` set to the release's repository, which turns on **Help › Check for Updates…** and the daily check (`shell/updatecheck.cpp`, shared with Windows).
The repository is `TommasoBendinelli/thoughts-desktop` unless `--repository` or `THOUGHTS_RELEASE_REPOSITORY` names another; `gh` must be signed in with an account that may create releases there.
It keeps `Thoughts-<version>.dmg` in `~/.cache/thoughts-mac-releases` and publishes it as release `mac-<version>`.
The release notes default to the commit subjects since the previous `mac-` release.
Mac releases are not marked Latest, so the Windows installer stays the release GitHub shows first.

Before the upload, the mounted image is scanned for personal or private strings (`tools/release_scan.py`): home folders, tailnet hosts and addresses, private keys, tokens, and your own markers, one regular expression per line, from `~/.config/thoughts/release-private-patterns.txt` (or the file `THOUGHTS_RELEASE_PATTERNS` names).
A hit stops the release; with `--no-publish` it only warns.
Compilers and Craft write the paths of the source, of the build folder and of the Craft root into the binaries, so a build made inside a personal home folder (`/Users/<name>/…`) is refused.
Build releases where those paths are not personal: with the clone, the server SDK and the Craft root (`THOUGHTS_CRAFT_ROOT`) under `/Users/Shared`, or from a macOS account with a neutral name such as `builder`; `release_scan.py` lists the account names it lets pass.

On the Mac, the update check takes the newest `mac-*` release with a `Thoughts-*.dmg` asset.
It installs the image only if GitHub gives the asset's SHA-256 and the download matches it.
Thoughts then copies `Contents/Resources/install-update.sh` to the temporary folder, starts it and quits.
The script waits for Thoughts to exit and mounts the image.
It accepts the `Thoughts.app` inside only if that satisfies the installed app's designated requirement, so the same identifier and the same certificate.
Every release must therefore be signed with the same identity: keep the keychain identity of the Mac that publishes them.
It copies the new app beside the old one under a hidden name, verifies the copy, renames the old app away and the copy into its place, unmounts the image and opens the app.
On any failure the old app stays or is put back, opens again, and an alert names `~/Library/Logs/Thoughts/install-update.log`.
`sh tools/macos/test-install-update.sh` tests this on throwaway bundles in a temporary folder; it signs them ad hoc and needs no certificate.
Because of the requirement check, switching to a Developer ID certificate needs one manual install on every Mac.

### Developer ID and notarization

Without an Apple Developer ID, the other Mac must allow the app once per version under **Privacy & Security › Open Anyway**.
With one, signing and notarization would change as follows. This has not been tried:

- Sign everything with the `Developer ID Application: …` identity instead of the local one, with `--options runtime --timestamp`. `sign-app.py` currently pins the local certificate in the designated requirement and uses `--timestamp=none`, so it would need a Developer ID mode.
- Hardened runtime with library validation accepts the bundled libraries only when they all carry the same Team ID, which re-signing every library already gives.
- Submit and staple the image: `xcrun notarytool submit dist/Thoughts.dmg --keychain-profile <profile> --wait`, then `xcrun stapler staple dist/Thoughts.dmg`.
