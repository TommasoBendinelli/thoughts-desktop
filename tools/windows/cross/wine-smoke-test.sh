#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Smoke test of a Windows installer under Wine, inside the thoughts-winetest container:
# installs it silently, runs the command-line programs, starts Thoughts on a virtual
# display and saves a screenshot, then uninstalls. Usage: wine-smoke-test.sh SETUP OUTDIR
set -uo pipefail
SETUP=$1 OUT=$2
mkdir -p "$OUT"
# Wine must create the prefix folder itself; without Mono and Gecko it asks nothing.
export WINEPREFIX=/tmp/wine-prefix-$$ WINEDEBUG=-all THOUGHTS_NO_UPDATE_CHECK=1 WINEDLLOVERRIDES="mscoree,mshtml="
Xvfb :9 -screen 0 1600x1000x24 >/dev/null 2>&1 &
export DISPLAY=:9
wineboot -i >/dev/null 2>&1; wineserver -w
status=0

wine "$SETUP" /S
# Again over the installed copy, as an update does.
wine "$SETUP" /S || { echo "FAIL: installing over the installed copy (exit $?)"; status=1; }
app="$WINEPREFIX/drive_c/users/$(whoami)/AppData/Local/Programs/Thoughts"
if [ ! -f "$app/bin/thoughts.exe" ]; then
    echo "FAIL: the installer put no thoughts.exe in $app"
    exit 1
fi
echo "installed: $(du -sh "$app" | cut -f1) in $app"
wine reg query 'HKCU\Software\Classes\thoughts\shell\open\command' 2>/dev/null | grep -q thoughts.exe && echo "ok: thoughts:// registered" || { echo "FAIL: thoughts:// not registered"; status=1; }

# --help would open a message box: under Wine these programs have no console.
for command in "thoughtsctl.exe knowledge-levels" "thoughts-publish-sources.exe --help" "thoughts-watch-build.exe --help"; do
    program=${command%% *}
    timeout 60 wine "$app/bin/$program" ${command#* } > "$OUT/$program.txt" 2>&1
    code=$?
    if [ "$code" -eq 124 ] || grep -qiE 'could not load|not found|0xc0000135|err:module' "$OUT/$program.txt"; then
        echo "FAIL: $command (exit $code):"; head -5 "$OUT/$program.txt"; status=1
    else
        echo "ok: $command (exit $code): $(head -c 80 "$OUT/$program.txt" | tr '\n' ' ')"
    fi
done

wine "$app/bin/thoughts.exe" > "$OUT/thoughts.log" 2>&1 &
for _ in $(seq 60); do
    sleep 2
    xdotool search --onlyvisible --name 'Thoughts|Okular|Library' >/dev/null 2>&1 && break
done
sleep 8
import -window root "$OUT/screenshot.png" 2>/dev/null
xwininfo -root -tree 2>/dev/null | grep -E '^\s+0x' | grep -vE '"(Default IME|wine|explorer)' | head -15 > "$OUT/windows.txt"
if pgrep -f 'thoughts.exe' >/dev/null && xdotool search --onlyvisible --name '.' >/dev/null 2>&1; then
    echo "ok: thoughts.exe is running with a window:"; sed 's/^/    /' "$OUT/windows.txt" | head -5
else
    echo "FAIL: thoughts.exe did not stay up"; tail -20 "$OUT/thoughts.log"; status=1
fi
wineserver -k 2>/dev/null; sleep 2

wine "$app/uninstall.exe" /S _?="$(winepath -w "$app")"
echo "uninstaller exit $?"
[ ! -e "$app/bin" ] && echo "ok: uninstalled" || { echo "FAIL: uninstall left $app/bin"; status=1; }
exit $status
