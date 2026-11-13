; SPDX-License-Identifier: GPL-2.0-or-later
; SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
; Thoughts for Windows: a per-user installer (no administrator rights needed).
; Built by tools/windows/release.py with makensis:
;   makensis -DVERSION=2026.10.15.1 -DSTAGE=<runtime tree> -DOUTFILE=<setup.exe> thoughts.nsi
; Options: /S installs silently; /RELAUNCH starts Thoughts afterwards (the in-app updater
; passes both). User data lives in %LOCALAPPDATA%\Thoughts and is never touched.

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define APP "Thoughts"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Thoughts"
!define CLASSES "Software\Classes"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Thoughts"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later (based on Okular)"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\thoughts.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Thoughts"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Waits up to a minute for Thoughts and its helpers to exit (an update is started
; just before Thoughts quits). A running program keeps its .exe locked.
!macro WaitForExit un
Function ${un}WaitForExit
    StrCpy $1 0
    retry:
        StrCpy $2 ""
        ${If} ${FileExists} "$INSTDIR\bin\*.exe"
            ClearErrors
            FileOpen $0 "$INSTDIR\bin\thoughts.exe" a
            ${If} ${Errors}
                StrCpy $2 "thoughts.exe"
            ${Else}
                FileClose $0
            ${EndIf}
            ${If} $2 == ""
                ClearErrors
                FileOpen $0 "$INSTDIR\bin\thoughts-publish-sources.exe" a
                ${If} ${Errors}
                    StrCpy $2 "thoughts-publish-sources.exe"
                ${Else}
                    FileClose $0
                ${EndIf}
            ${EndIf}
            ${If} $2 == ""
                ClearErrors
                FileOpen $0 "$INSTDIR\bin\thoughtsctl.exe" a
                ${If} ${Errors}
                    StrCpy $2 "thoughtsctl.exe"
                ${Else}
                    FileClose $0
                ${EndIf}
            ${EndIf}
        ${EndIf}
        ${If} $2 != ""
            IntOp $1 $1 + 1
            ${If} $1 < 60
                Sleep 1000
                Goto retry
            ${EndIf}
            MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Thoughts is still running ($2). Quit it, then choose Retry." /SD IDCANCEL IDRETRY again
            Abort "Thoughts is still running ($2)."
            again:
                StrCpy $1 0
                Goto retry
        ${EndIf}
FunctionEnd
!macroend
!insertmacro WaitForExit ""
!insertmacro WaitForExit "un."

Section "Thoughts"
    Call WaitForExit
    ; Replace the program wholesale: files dropped from a release must not linger.
    RMDir /r "$INSTDIR\bin"
    RMDir /r "$INSTDIR\plugins"
    RMDir /r "$INSTDIR\share"
    RMDir /r "$INSTDIR\etc"
    RMDir /r "$INSTDIR\qml"
    RMDir /r "$INSTDIR\translations"
    SetOutPath "$INSTDIR"
    File /r "${STAGE}\*.*"

    WriteUninstaller "$INSTDIR\uninstall.exe"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Tommaso Bendinelli"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\thoughts.exe,0"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0

    ; The same registration as tools/windows/register.ps1: thoughts:// links,
    ; Open with › Thoughts for PDFs, and the Start menu.
    WriteRegStr HKCU "${CLASSES}\thoughts" "" "URL:Thoughts"
    WriteRegStr HKCU "${CLASSES}\thoughts" "URL Protocol" ""
    WriteRegStr HKCU "${CLASSES}\thoughts\DefaultIcon" "" '"$INSTDIR\bin\thoughts.exe",0'
    WriteRegStr HKCU "${CLASSES}\thoughts\shell\open\command" "" '"$INSTDIR\bin\thoughts.exe" "%1"'
    WriteRegStr HKCU "${CLASSES}\Thoughts.PDF" "" "PDF in Thoughts"
    WriteRegStr HKCU "${CLASSES}\Thoughts.PDF\DefaultIcon" "" '"$INSTDIR\bin\thoughts.exe",0'
    WriteRegStr HKCU "${CLASSES}\Thoughts.PDF\shell\open\command" "" '"$INSTDIR\bin\thoughts.exe" "%1"'
    WriteRegStr HKCU "${CLASSES}\.pdf\OpenWithProgids" "Thoughts.PDF" ""
    CreateShortcut "$SMPROGRAMS\Thoughts.lnk" "$INSTDIR\bin\thoughts.exe" "" "$INSTDIR\bin\thoughts.exe" 0
    ; Lets the agents' MCP server (tools/mcp/server.py) find thoughtsctl.exe.
    WriteRegExpandStr HKCU "Environment" "THOUGHTSCTL" "$INSTDIR\bin\thoughtsctl.exe"
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=2000

    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/RELAUNCH" $1
    ${IfNot} ${Errors}
        Exec '"$INSTDIR\bin\thoughts.exe"'
    ${EndIf}
SectionEnd

Section "Uninstall"
    Call un.WaitForExit
    Delete "$SMPROGRAMS\Thoughts.lnk"
    DeleteRegKey HKCU "${CLASSES}\thoughts"
    DeleteRegKey HKCU "${CLASSES}\Thoughts.PDF"
    DeleteRegValue HKCU "${CLASSES}\.pdf\OpenWithProgids" "Thoughts.PDF"
    DeleteRegValue HKCU "Environment" "THOUGHTSCTL"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    RMDir /r "$INSTDIR\bin"
    RMDir /r "$INSTDIR\plugins"
    RMDir /r "$INSTDIR\share"
    RMDir /r "$INSTDIR\etc"
    RMDir /r "$INSTDIR\qml"
    RMDir /r "$INSTDIR\translations"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
