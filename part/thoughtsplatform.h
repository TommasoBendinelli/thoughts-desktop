// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

class QFileInfo;

// Operating-system services that Thoughts needs beyond Qt. Windows code lives
// in thoughtsplatform.cpp so <windows.h> never reaches shared headers. The
// macOS branches keep the behaviour Thoughts always had there.
namespace Thoughts::Platform
{

// The folder holding Thoughts' state. On macOS QStandardPaths' application
// data folder (".../thoughts") and GenericDataLocation/Thoughts are the same
// folder because APFS ignores case, and both names stay as they were. Windows
// would split them between the roaming and local profiles, so there both
// return %LOCALAPPDATA%/Thoughts; its SQLite databases must not roam.
// On macOS both follow HOME when CFFIXED_USER_HOME is unset (QStandardPaths
// alone ignores HOME there), so a private HOME isolates Thoughts' state.
QString applicationDataRoot();
QString thoughtsDataRoot();
// The home folder of the user account from the password database, whatever
// HOME says; empty on Windows.
QString accountHomeDirectory();
// Whether `root` is the account's own data (inside its home folder, not a
// Qt test-mode folder): what a test or a temporary Library must never write.
// Always false on Windows.
bool isAccountDataRoot(const QString &root);

// Secrets stored by the operating system: the macOS Keychain (generic
// password: service, account) or the Windows Credential Manager (generic
// credential named "service/account"). Returns an empty array if absent.
QByteArray readSecret(const QString &service, const QString &account);
// A secret as typed or piped in: without a leading UTF-8 byte order mark, which
// PowerShell can put in front of what it pipes, and without surrounding whitespace.
QByteArray cleanSecret(QByteArray secret);
bool writeSecret(const QString &service, const QString &account, const QByteArray &secret, QString *error = nullptr);
// Removes the secret; true also when there was none.
bool deleteSecret(const QString &service, const QString &account, QString *error = nullptr);
// Command a person can run to store the secret, for error messages.
QString storeSecretCommand(const QString &service, const QString &account);
// Account that holds this computer's Fedora device token.
QString desktopDeviceAccount();
// This computer's own name as its owner knows it (the macOS computer name, e.g. "Alex's MacBook
// Pro"; elsewhere the host name), or an empty string.
QString computerName();
// origin_id recorded on comments and on review cards and events created
// here. Informational; "thoughts-mac" and "mac-okular" on macOS as always.
QString desktopOriginId();
QString reviewOriginId();

// Identity of the file itself rather than its path: device and inode on Unix,
// volume serial and file ID on Windows. Empty if unavailable.
QString physicalFileIdentity(const QString &path);
// Identity that changes whenever the file is rewritten or replaced: physical
// identity, size, and modification and status-change times at full
// resolution. Empty if path is not a regular file.
QString fileStamp(const QString &path);

// Atomically replaces path with bytes (QSaveFile). On Windows another
// reader's brief open, such as the source hasher, the search indexer or
// antivirus, refuses the replace, so it retries for about half a second.
bool replaceFileContents(const QString &path, const QByteArray &bytes);

// Flushes a file's data to stable storage (fsync or FlushFileBuffers).
bool flushToDisk(int fileDescriptor);
// Makes a directory's entries durable (fsync of the directory on Unix; NTFS
// journals metadata itself, so Windows needs nothing).
bool flushDirectory(const QString &path);

// Shares a file's bytes with a new name where the file system can (APFS
// clone, hard link); false means the caller should copy.
bool cloneOrLinkFile(const QString &source, const QString &destination);

// A directory that stands for another one: a symbolic link, or on Windows a
// junction. Thoughts uses one for current_pdfs so open PDFs stay valid while a
// new Library generation becomes visible.
bool isDirectoryLink(const QFileInfo &info);
// The absolute directory a link points to, or an empty string.
QString directoryLinkTarget(const QFileInfo &info);
// Points link at target in one step, creating it if needed. Existing PDFs
// opened through the old target stay open.
bool replaceDirectoryLink(const QString &link, const QString &target, QString *error = nullptr);

// Whether another process has a file below directory open in a way that stops
// it from being removed. Windows only; elsewhere open files can be unlinked
// and the answer is false.
bool directoryHasFilesInUse(const QString &directory);

// The source publisher and build viewer (thoughts-publish-sources,
// thoughts-watch-build) installed next to the running application, or empty.
QString nativeServerTool(const QString &name);

// Stops child processes from inheriting this process's standard streams.
// Windows hands every inheritable handle to a child started with redirected
// streams, so a detached helper would otherwise keep a caller's pipe open
// until it exits. Elsewhere redirection replaces the streams; a no-op.
void keepStandardStreamsPrivate();

// Starts a console program in its own new console window (on Windows; a GUI
// process would otherwise start it hidden), with the given environment.
bool startInNewConsole(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment);
// Whether the process with this ID still runs.
bool processIsRunning(const QString &pid);
// Brings the top-level window with exactly this title to the front.
bool focusWindowTitled(const QString &title);

// Shows path selected in Finder or Explorer.
bool revealInFileManager(const QString &path);
// Marks a file Thoughts saved from the server as downloaded, as a browser
// marks its downloads: the com.apple.quarantine attribute on macOS, so
// Gatekeeper checks it before it may run, and the Zone.Identifier stream
// (Internet zone) on Windows, for SmartScreen and Office's protected view.
// False if the mark could not be written; nothing to write elsewhere.
bool markDownloaded(const QString &path);
// Whether the file carries that mark.
bool isMarkedDownloaded(const QString &path);
// Opens path with the named application (macOS application name or a Windows
// executable path); falls back to the default application when empty.
bool openWithApplication(const QString &application, const QString &path);

}
