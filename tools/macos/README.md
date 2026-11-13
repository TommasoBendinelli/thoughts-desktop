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

The app loads its libraries from `~/CraftRoot/lib` (the executables' RPATH); none is copied into the bundle, so `sign-app.py` signs only Thoughts' own executables.
Import sources › From Git repository needs libgit2 (GPL-2.0 with the linking exception) in CraftRoot: `craft libgit2` installs Craft's prebuilt 1.7.1, which uses macOS' Security framework for HTTPS and CraftRoot's libssh2 for SSH.
No installed Git is needed.
