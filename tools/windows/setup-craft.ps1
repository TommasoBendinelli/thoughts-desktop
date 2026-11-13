# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Installs KDE Craft and Thoughts' build dependencies (Okular's, from KDE's
# prebuilt Windows packages). Run once from a normal (not administrator)
# PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\windows\setup-craft.ps1
# Needs Visual Studio 2022 or its Build Tools with "Desktop development with
# C++", Python 3 and Git for Windows. Craft's installer asks a few questions;
# choose the install folder C:\CraftRoot, the MSVC 2022 x86_64 compiler, Qt 6,
# and the prebuilt binary cache.
param(
    [string]$CraftRoot = 'C:\CraftRoot'
)
$ErrorActionPreference = 'Stop'

foreach ($Program in 'git', 'python') {
    if (-not (Get-Command $Program -ErrorAction SilentlyContinue)) {
        throw "$Program is not on PATH. Install it first (Git for Windows; Python 3 from python.org with 'Add to PATH')."
    }
}

$CraftEnvironment = Join-Path $CraftRoot 'craft\craftenv.ps1'
if (-not (Test-Path $CraftEnvironment)) {
    Write-Host 'Installing KDE Craft. Answer its questions as described at the top of this script.'
    $Installer = Join-Path $env:TEMP 'install_craft.ps1'
    Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/KDE/craft/master/setup/install_craft.ps1' -OutFile $Installer
    & $Installer
    if (-not (Test-Path $CraftEnvironment)) {
        throw "Craft did not finish installing into $CraftRoot."
    }
}

. $CraftEnvironment
# Okular's dependencies are Thoughts' dependencies. Install them only:
# Craft's own Okular would install plugins with the same names as Thoughts'.
craft --install-deps okular
if ($LASTEXITCODE -ne 0) { throw "Craft could not install Okular's dependencies (exit $LASTEXITCODE)." }
# Thoughts also links OpenSSL (required), zstd (PDF delta downloads) and libgit2
# (required: Import sources › From Git repository).
craft libs/openssl libs/libzstd libs/libgit2
if ($LASTEXITCODE -ne 0) { throw "Craft could not install OpenSSL, zstd and libgit2 (exit $LASTEXITCODE)." }
Write-Host 'Dependencies are installed. Next: tools\windows\build.ps1'
