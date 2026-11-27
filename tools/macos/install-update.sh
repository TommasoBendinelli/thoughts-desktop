#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Installs a downloaded Thoughts disk image over the app once Thoughts has quit.
#
#     install-update.sh PID DMG APP
#
# Thoughts (shell/updatecheck.cpp) checks the image's SHA-256, copies this script
# out of Contents/Resources, starts it detached and quits. The script waits for
# process PID to exit, mounts DMG and accepts its Thoughts.app only if it satisfies
# the designated requirement of the installed APP (same identifier, same signing
# certificate). It copies the new app beside APP under a hidden name, renames APP
# away and the copy into its place, and opens it. When this account may not change
# APP's folder but owns APP itself (/Applications on a managed Mac without admin
# rights), it replaces APP's Contents in place instead, through a verified copy in a
# temporary folder. On any failure the old app stays (or is put back), opens again,
# and an alert names the log:
# ~/Library/Logs/Thoughts/install-update.log.
# THOUGHTS_UPDATE_NO_ALERT=1 leaves out the alert (for tests).
set -u
if [ $# -ne 3 ]; then
    echo "usage: install-update.sh PID DMG APP" >&2
    exit 2
fi
pid=$1
dmg=$2
app=${3%/}
log_dir="$HOME/Library/Logs/Thoughts"
log="$log_dir/install-update.log"
mkdir -p "$log_dir"
exec >>"$log" 2>&1
echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) installing $dmg over $app"

parent=$(dirname "$app")
name=$(basename "$app")
staged="$parent/.$name.update-$$"
old="$parent/.$name.old-$$"
work=$(mktemp -d "${TMPDIR:-/tmp}/thoughts-update.XXXXXX") || exit 1
mount="$work/mount"
mounted=0

cleanup() {
    if [ "$mounted" = 1 ]; then
        /usr/bin/hdiutil detach -quiet "$mount" || /usr/bin/hdiutil detach -quiet -force "$mount"
        mounted=0
    fi
    rm -rf "$staged" "$work"
}

fail() {
    echo "failed: $1"
    # Put the old app back if it was already moved away.
    if [ ! -e "$app" ] && [ -d "$old" ]; then
        mv "$old" "$app" && echo "restored $app"
    fi
    # In place: put the old Contents back if they were already moved away.
    if [ -d "$work/old-Contents" ]; then
        rm -rf "$app/Contents" && mv "$work/old-Contents" "$app/Contents" && echo "restored $app/Contents"
    fi
    cleanup
    if [ "${THOUGHTS_UPDATE_NO_ALERT:-}" != 1 ]; then
        /usr/bin/osascript -e 'on run argv' \
            -e 'display alert "Thoughts was not updated" message (item 1 of argv) as warning' \
            -e 'end run' "$1 The previous version opens again. Details: $log" >/dev/null 2>&1 &
    fi
    [ -d "$app" ] && /usr/bin/open "$app"
    exit 1
}

# Thoughts quits after starting this script; a running sync may take a while.
waited=0
while kill -0 "$pid" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 1500 ] && fail "Thoughts did not quit within five minutes."
    sleep 0.2
done

[ -d "$app/Contents" ] || fail "$app is not an app bundle."
in_place=0
if [ ! -w "$parent" ]; then
    [ -w "$app" ] && [ -w "$app/Contents" ] || fail "Thoughts cannot write to $parent or to $app."
    in_place=1
fi
requirement=$(/usr/bin/codesign -d -r- "$app" 2>/dev/null | sed -n 's/^designated => //p')
[ -n "$requirement" ] || fail "The signature of the installed Thoughts could not be read."
echo "requirement: $requirement"

mkdir "$mount" || fail "Could not create $mount."
/usr/bin/hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$mount" "$dmg" >/dev/null \
    || fail "The disk image could not be opened."
mounted=1
new="$mount/Thoughts.app"
[ -d "$new" ] || fail "The disk image contains no Thoughts.app."
/usr/bin/codesign --verify --deep --strict -R "=$requirement" "$new" \
    || fail "The new Thoughts is not signed like the installed one."

if [ "$in_place" = 1 ]; then
    # The bundle folder stays; its Contents are swapped for the verified copy's.
    /usr/bin/ditto "$new" "$work/new.app" || fail "Could not copy the new Thoughts to $work."
    /usr/bin/codesign --verify --deep --strict -R "=$requirement" "$work/new.app" \
        || fail "The copied Thoughts does not verify."
    mv "$app/Contents" "$work/old-Contents" || fail "Could not move the installed Thoughts aside."
    mv "$work/new.app/Contents" "$app/Contents" || fail "Could not move the new Thoughts into place."
    /usr/bin/codesign --verify --deep --strict -R "=$requirement" "$app" \
        || fail "The installed Thoughts does not verify."
    echo "replaced the contents of $app"
    rm -rf "$work/old-Contents"
    cleanup
    rm -f "$dmg"
    /usr/bin/open "$app"
    echo "done"
    exit 0
fi

/usr/bin/ditto "$new" "$staged" || fail "Could not copy the new Thoughts to $parent."
/usr/bin/codesign --verify --deep --strict -R "=$requirement" "$staged" \
    || fail "The copied Thoughts does not verify."

mv "$app" "$old" || fail "Could not move the installed Thoughts aside."
if ! mv "$staged" "$app"; then
    fail "Could not move the new Thoughts into place."
fi
echo "replaced $app"
rm -rf "$old"
cleanup
rm -f "$dmg"
/usr/bin/open "$app"
echo "done"
exit 0
