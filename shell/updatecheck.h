// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>

class QWidget;

// Updates for builds published as GitHub Releases. A Windows release is tagged
// windows-<version> and carries one Thoughts-<version>-setup.exe asset; a Mac release
// is tagged mac-<version> and carries one Thoughts-<version>.dmg asset
// (tools/macos/release.py). A public repository is read without a token. When GitHub
// does not show the releases (a private repository), Thoughts asks for a GitHub token
// and keeps it with the other secrets (Credential Manager, the macOS Keychain) as
// thoughts-github/releases.
namespace Thoughts::Updates
{
// True when this build was made with a release version and an update repository.
bool available();
QString currentVersion();

// Check for Updates… (Help menu; the Thoughts menu on the Mac): reports; asks for a token
// only when GitHub refuses the check.
void checkInteractively(QWidget *parent);
// Shortly after startup, at most once a day; silent unless an update exists.
void checkInBackground(QWidget *parent);

// Numeric comparison of dotted versions ("2026.10.15.2" > "2026.10.15.1").
int compareVersions(const QString &a, const QString &b);

// Whether the downloaded bytes are the installer the release names: their
// SHA-256 equals the release's digest (64 hexadecimal digits). A release
// without a digest matches nothing, so its installer is never run.
bool installerMatches(const QByteArray &expectedSha256, const QByteArray &bytes);
// The name the installer is saved under: the asset's name if it is a plain
// Thoughts-<version>-setup.exe or Thoughts-<version>.dmg, else a fixed name.
// Never a path.
QString installerFileName(const QString &assetName);
// Saves the installer in a new folder with an unpredictable name, which only
// this user can write to, and returns the file's path; empty on failure.
// Another program therefore cannot put a file of its own where Thoughts
// would start the installer from.
QString saveInstaller(const QByteArray &bytes, const QString &assetName);
// Background downloads (27 Nov 2026): the background check downloads a new release's
// installer at once, so Install and Restart only installs. The installer is remembered with
// its version and digest; it is run only while the file still has that digest.
// The remembered installer for exactly @p version with digest @p sha256, else empty (a
// remembered one that is missing or changed is forgotten).
QString readyInstaller(const QString &version, const QByteArray &sha256);
void rememberReadyInstaller(const QString &path, const QString &version, const QByteArray &sha256);
// Forgets (and deletes) a remembered installer that is not newer than this build.
void forgetInstalledInstaller();
// Whether the file at @p path has the digest @p sha256.
bool installerFileMatches(const QString &path, const QByteArray &sha256);
// Whether a download may continue at `next` after a redirect from `from`:
// only https, and *sendToken tells whether it is still the same origin (the
// token goes nowhere else).
bool redirectAllowed(const QUrl &from, const QUrl &next, bool *sendToken);
}
