# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Registers Thoughts for the current user: thoughts:// links (macOS gets them
# from the app bundle's Info.plist), a Start menu shortcut, and Thoughts in
# "Open with" for PDFs without changing the default PDF viewer. No
# administrator rights are needed. Run again after moving the installation;
# -Remove undoes it.
param(
    [string]$Executable = 'C:\CraftRoot\bin\thoughts.exe',
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'

$Classes = 'HKCU:\Software\Classes'
$Shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Thoughts.lnk'

if ($Remove) {
    foreach ($Key in "$Classes\thoughts", "$Classes\Thoughts.PDF") {
        if (Test-Path $Key) { Remove-Item -Recurse -Force $Key }
    }
    $OpenWith = "$Classes\.pdf\OpenWithProgids"
    if (Test-Path $OpenWith) { Remove-ItemProperty -Path $OpenWith -Name 'Thoughts.PDF' -ErrorAction SilentlyContinue }
    Remove-Item -Force $Shortcut -ErrorAction SilentlyContinue
    Write-Host 'Removed the Thoughts registration.'
    return
}

if (-not (Test-Path $Executable)) {
    throw "Thoughts is not installed at $Executable. Run tools\windows\build.ps1 first, or pass -Executable."
}
$Executable = (Resolve-Path $Executable).Path

function Set-Default([string]$Key, [string]$Value) {
    New-Item -Path $Key -Force | Out-Null
    Set-ItemProperty -Path $Key -Name '(default)' -Value $Value
}

# thoughts://current-pdfs/<file>.pdf and thoughts://review-card/<id>
Set-Default "$Classes\thoughts" 'URL:Thoughts'
New-ItemProperty -Path "$Classes\thoughts" -Name 'URL Protocol' -Value '' -PropertyType String -Force | Out-Null
Set-Default "$Classes\thoughts\DefaultIcon" "`"$Executable`",0"
Set-Default "$Classes\thoughts\shell\open\command" "`"$Executable`" `"%1`""

# Offered under "Open with" for PDFs; the default viewer stays as it is.
Set-Default "$Classes\Thoughts.PDF" 'PDF in Thoughts'
Set-Default "$Classes\Thoughts.PDF\DefaultIcon" "`"$Executable`",0"
Set-Default "$Classes\Thoughts.PDF\shell\open\command" "`"$Executable`" `"%1`""
New-Item -Path "$Classes\.pdf\OpenWithProgids" -Force | Out-Null
New-ItemProperty -Path "$Classes\.pdf\OpenWithProgids" -Name 'Thoughts.PDF' -Value '' -PropertyType String -Force | Out-Null

$Shell = New-Object -ComObject WScript.Shell
$Link = $Shell.CreateShortcut($Shortcut)
$Link.TargetPath = $Executable
$Link.WorkingDirectory = Split-Path $Executable
$Link.IconLocation = "$Executable,0"
$Link.Description = 'Thoughts'
$Link.Save()

Write-Host "Registered $Executable for thoughts:// links, PDF Open with, and the Start menu."
