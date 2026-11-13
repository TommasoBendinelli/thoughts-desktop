# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Builds Thoughts with KDE Craft's MSVC toolchain and installs it into the
# Craft root, as tools/macos does on the Mac. Run from any PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\windows\build.ps1
#   ... build.ps1 -ServerUrl https://thoughts.example.org   # the server new installations start with
# Expects the thoughts-server checkout beside this repository (for the C++
# client SDK) and dependencies from setup-craft.ps1.
param(
    [string]$CraftRoot = 'C:\CraftRoot',
    [ValidateSet('RelWithDebInfo', 'Release', 'Debug')]
    [string]$BuildType = 'RelWithDebInfo',
    [string]$ServerCheckout = '',
    [string]$ServerUrl = '',
    [switch]$Tests,
    [switch]$NoInstall
)
$ErrorActionPreference = 'Stop'

$Repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $ServerCheckout) {
    $ServerCheckout = Join-Path (Split-Path $Repository -Parent) 'thoughts-server'
}
$Sdk = Join-Path $ServerCheckout 'sdk\cpp'
if (-not (Test-Path (Join-Path $Sdk 'CMakeLists.txt'))) {
    throw "The Thoughts client SDK is missing at $Sdk. Clone thoughts-server beside this repository or pass -ServerCheckout."
}
$CraftEnvironment = Join-Path $CraftRoot 'craft\craftenv.ps1'
if (-not (Test-Path $CraftEnvironment)) {
    throw "KDE Craft is not installed at $CraftRoot. Run tools\windows\setup-craft.ps1 first."
}

# Craft's environment adds its tools, Qt and the MSVC compiler to this session.
. $CraftEnvironment

$Build = Join-Path $Repository 'build-windows'
$Testing = if ($Tests) { 'ON' } else { 'OFF' }
# Without -ServerUrl the build keeps its configured (or the default) server address.
$ServerOption = if ($ServerUrl) { @("-DTHOUGHTS_DEFAULT_SERVER_URL=$ServerUrl") } else { @() }
cmake -S $Repository -B $Build -G Ninja @ServerOption `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DCMAKE_INSTALL_PREFIX=$CraftRoot" `
    "-DCMAKE_PREFIX_PATH=$CraftRoot" `
    "-DBUILD_TESTING=$Testing" `
    -DBUILD_DESKTOP=ON `
    "-DTHOUGHTS_SERVER_SDK_ROOT=$Sdk" `
    -DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed (exit $LASTEXITCODE)." }

cmake --build $Build --parallel
if ($LASTEXITCODE -ne 0) { throw "The build failed (exit $LASTEXITCODE)." }

if ($NoInstall) {
    Write-Host "Built in $Build (not installed)."
    return
}
# Replacing loaded DLLs fails on Windows and would leave a mixed installation.
if (Get-Process -Name thoughts, thoughtsctl, thoughts-publish-sources, thoughts-watch-build -ErrorAction SilentlyContinue) {
    throw 'Quit Thoughts (and any running thoughtsctl, Sync or build viewer) before installing.'
}
cmake --install $Build
if ($LASTEXITCODE -ne 0) { throw "Installing failed (exit $LASTEXITCODE)." }

foreach ($Program in 'thoughts.exe', 'thoughtsctl.exe', 'thoughts-comment-sync.exe', 'thoughts-publish-sources.exe', 'thoughts-watch-build.exe') {
    if (-not (Test-Path (Join-Path $CraftRoot "bin\$Program"))) {
        throw "Installed, but $Program is missing from $CraftRoot\bin."
    }
}
Write-Host "Installed Thoughts into $CraftRoot. Start it with $CraftRoot\bin\thoughts.exe, or run tools\windows\register.ps1 once for the Start menu and thoughts:// links."
