// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Finds Visual Studio Code for Open Source in VS Code and Resolve Source Conflict
// (shell/shell.cpp). Windows has its own lookup there, for code.cmd.
namespace Thoughts::VisualStudioCode
{
// How to open a file in VS Code. cli is its `code` command line, which can go to a
// line and wait for the editor to close. bundleId names the app to macOS, which can
// still open a file in it when there is no command line or it does not start; the
// line is lost then.
struct Launcher {
    QString cli;
    QString bundleId;
    bool found() const
    {
        return !cli.isEmpty() || !bundleId.isEmpty();
    }
};

// Where the lookup searches, so a test can stand in for this computer.
struct Places {
    // `code` on PATH. An app opened from the Finder or the Dock gets the system's
    // minimal PATH, so on macOS this is usually empty unless Thoughts was started
    // from a terminal.
    QString pathCommand;
    // Command lines at fixed places, in order (where VS Code's "Install 'code'
    // command in PATH" puts it).
    QStringList commands;
    // macOS: the app LaunchServices knows for a bundle identifier, empty when none.
    // It finds VS Code wherever it is: ~/Applications, Downloads, another disk.
    std::function<QString(const QString &bundleId)> applicationForBundleId;
    // Folders that may hold the app when LaunchServices does not know it.
    QStringList applicationFolders;
};

// This computer's places.
Places systemPlaces();

// The first command line that can run, in this order: PATH, the fixed commands, the
// apps LaunchServices knows (VS Code, then Insiders, then VSCodium), the apps in the
// folders. bundleId is the first of those apps that LaunchServices knows.
Launcher find(const Places &places);
Launcher find();

// macOS: the app LaunchServices would open for bundleId; empty when it knows none,
// and always empty on other systems.
QString applicationForBundleId(const QString &bundleId);
// Asks macOS to open path in the app bundleId (`open -b`), without waiting.
bool openWithApplication(const QString &bundleId, const QString &path);
}
