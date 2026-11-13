# Installing Thoughts on another Mac

`Thoughts.dmg` holds the complete app: Qt, KDE Frameworks, Poppler and the
document plugins are inside `Thoughts.app`, so nothing else needs to be installed.

You need three things:

- A Mac with Apple silicon (M1 or later) and **macOS 13.3 (Ventura) or newer**.
  Intel Macs cannot run this build.
- The **address of a Thoughts server** (it starts with `https://`).
- **Device tokens** for this Mac from that server (see **Connect to a server**).

Thoughts is **not signed with an Apple Developer ID** and is not notarized by Apple.
macOS therefore refuses to open it the first time, and you have to allow it once
under **System Settings › Privacy & Security** (see **Open it the first time**).

## Download

New versions are published on the public release page,
<https://github.com/TommasoBendinelli/thoughts-desktop/releases>, as releases named
**Thoughts … for Mac** (tag `mac-…`). Download `Thoughts-<version>.dmg` from the
newest one. No GitHub account is needed.

Each release's notes give the SHA-256 of its disk image. To compare it with your
download, run this command in Terminal:

```sh
shasum -a 256 ~/Downloads/Thoughts-<version>.dmg
```

## Install

1. Open the downloaded `Thoughts-<version>.dmg`.
2. Drag **Thoughts** onto **Applications**.
3. Eject the disk image.

## Open it the first time

The app is signed with a certificate its builder made, not with an Apple Developer
ID, and it is not notarized by Apple. macOS therefore blocks the first start:

1. Open **Thoughts** from Applications. macOS says it could not verify that
   “Thoughts” is free of malware. Click **Done** (not **Move to Trash**).
2. Open **System Settings › Privacy & Security** and scroll down to **Security**.
   It says that “Thoughts” was blocked. Click **Open Anyway** and confirm with your
   password or Touch ID.
3. Click **Open** in the dialog that follows.

After that, Thoughts opens normally. You have to do this again when you install a
newer disk image yourself; an update that Thoughts installs itself (see **Updates**)
should not need it.

## Connect to a server

Thoughts keeps your PDFs and notes in `~/Thoughts/docs` and synchronizes them with a
Thoughts server. Each computer needs **two device tokens** of its own from that
server: one for the app, and one for the source publisher, which uploads the sources
you edit. They are kept in your login Keychain, and you never type them into Thoughts
itself.

Set the address first, then store the tokens: Thoughts connects to its server as soon
as it starts, and it sends the tokens to whatever address is saved.

1. **Server address.** A new installation starts with a placeholder address
   (`https://thoughts.example.org`), which is no real server. Choose
   **Thoughts › Server…**, enter your server's address (it must start with
   `https://`), click **Save**, then quit Thoughts.
   From Terminal you can do the same with this command:

   ```sh
   /Applications/Thoughts.app/Contents/MacOS/thoughtsctl set-server https://thoughts.example.org
   ```

   (with your server's address in place of the example).

2. **Device tokens.** Ask whoever runs the server for the two tokens for this Mac.
   With Thoughts closed, run these two commands in Terminal:

   ```sh
   security add-generic-password -U -s thoughts-server -a thoughts-mac -w
   security add-generic-password -U -s thoughts-server -a thoughts-source-sync -w
   ```

   Each command asks for its token twice: paste the app's token into the first and the
   source publisher's into the second. The tokens do not show on screen and are not
   saved in your shell history. To replace a token, run its command again.

3. Open Thoughts and click **Sync**.

Until both tokens are stored, Sync reports that the server is not configured or that
its credentials are unavailable. If it reports that the server refused the device
token, store that token again (step 2).

Running your own server means deploying `thoughts-server` behind HTTPS and creating
the two devices there with `thoughts-admin create-device`. Both are described in
thoughts-server's README ("Device provisioning"), not here.

## Updates

Thoughts checks for a newer `mac-…` release 30 seconds after it starts, at most once a
day, and asks before downloading anything. **Help › Check for Updates…** checks at
once.

- The check reads the public release page's list from GitHub and needs no account
  and no token. Set `THOUGHTS_NO_UPDATE_CHECK=1` to stop the daily check.
- The download is checked against the release's SHA-256. Thoughts then quits, replaces
  itself and opens again. The new version must be signed with the same certificate as
  the installed one; otherwise nothing changes.
- If anything goes wrong, the previous version stays and opens again. An alert names
  the log, `~/Library/Logs/Thoughts/install-update.log`.
- Replacing the app needs permission to change the folder it is in. In
  `/Applications` that means an administrator account. Otherwise Thoughts asks you to
  install the downloaded image yourself, as below.
- Thoughts does not mark its downloads as coming from the internet, so an update it
  installs should open without **Open Anyway**.

## Install a newer version yourself

1. Quit Thoughts (**Thoughts › Quit Thoughts**).
2. Open the new `Thoughts-<version>.dmg` and drag **Thoughts** onto **Applications**.
   When Finder asks, choose **Replace**.
3. Open Thoughts. macOS blocks it once more: use **Open Anyway** as above.

Your Library (`~/Thoughts/docs`), your settings, the server address and the device
tokens are kept outside the app, so replacing the app keeps all of them. To see which
build you have, run this command:

```sh
defaults read /Applications/Thoughts.app/Contents/Info ThoughtsSourceCommit
```

## Remove Thoughts

Move `/Applications/Thoughts.app` to the Trash. Your Library stays in `~/Thoughts`.
Its state is in `~/Library/Application Support/Thoughts`, `…/okular` and `…/kxmlgui5`.
The settings are in `~/Library/Preferences`: `okularrc`, `okularpartrc`,
`okular-generator-popplerrc` and `org.tbend.thoughts.plist`. Delete those as well to
remove everything. To remove the tokens, run these commands:

```sh
security delete-generic-password -s thoughts-server -a thoughts-mac
security delete-generic-password -s thoughts-server -a thoughts-source-sync
```
