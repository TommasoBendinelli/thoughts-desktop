#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Tests install-update.sh on throwaway bundles in a temporary folder; it never touches
# an installed Thoughts. The bundles are tiny programs named Thoughts.app with the
# identifier org.tbend.thoughts.updatetest. The successful update opens the new one,
# which only writes a line to a file in the temporary folder and exits.
#
#     sh tools/macos/test-install-update.sh
#
# The bundles are signed ad hoc, which needs no certificate and no keychain; their
# designated requirement is then the identifier alone. To test with a certificate too,
# name a code-signing identity of your keychain (its SHA-1 fingerprint, as
# tools/macos/setup-signing.py prints it):
#
#     THOUGHTS_TEST_SIGNING_IDENTITY=<40 hex digits> sh tools/macos/test-install-update.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
helper="$here/install-update.sh"
identity=${THOUGHTS_TEST_SIGNING_IDENTITY:-}
id=org.tbend.thoughts.updatetest
root=$(mktemp -d "${TMPDIR:-/tmp}/thoughts-update-test.XXXXXX")
trap 'chmod -R u+w "$root" 2>/dev/null; rm -rf "$root"' EXIT
marker="$root/launched"
failures=0

make_app() { # make_app DIR VERSION [IDENTIFIER|unsigned]
    app="$1/Thoughts.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cat >"$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>fake</string>
<key>CFBundleIdentifier</key><string>$id</string>
<key>CFBundleName</key><string>Thoughts update test</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$2</string>
<key>LSBackgroundOnly</key><true/>
</dict></plist>
EOF
    printf '#include <stdio.h>\nint main(void){FILE*f=fopen("%s","a");if(f){fputs("%s\\n",f);fclose(f);}return 0;}\n' "$marker" "$2" >"$root/fake.c"
    cc -o "$app/Contents/MacOS/fake" "$root/fake.c"
    echo "$2" >"$app/Contents/Resources/version"
    signer=${3:-$id}
    if [ "$signer" = unsigned ]; then
        :
    elif [ -n "$identity" ]; then
        codesign --force --sign "$identity" --timestamp=none --identifier "$signer" \
            --requirements "=designated => identifier \"$signer\" and certificate leaf = H\"$identity\"" "$app" 2>/dev/null
    else
        codesign --force --sign - --timestamp=none --identifier "$signer" \
            --requirements "=designated => identifier \"$signer\"" "$app" 2>/dev/null
    fi
}

make_dmg() { # make_dmg VERSION [IDENTIFIER|unsigned] -> path
    rm -rf "$root/dmgsrc"
    mkdir -p "$root/dmgsrc"
    make_app "$root/dmgsrc" "$1" "${2:-}"
    hdiutil create -quiet -volname "Thoughts $1" -srcfolder "$root/dmgsrc" -fs HFS+ -format UDZO -ov "$root/Thoughts-$1.dmg"
    echo "$root/Thoughts-$1.dmg"
}

run_case() { # run_case NAME EXPECT(0|1) EXPECTED_VERSION DMG
    rm -f "$marker"
    sleep 1 &
    waiter=$!
    if HOME="$root/home" THOUGHTS_UPDATE_NO_ALERT=1 /bin/sh "$helper" "$waiter" "$4" "$root/Applications/Thoughts.app"; then status=0; else status=1; fi
    version=$(cat "$root/Applications/Thoughts.app/Contents/Resources/version" 2>/dev/null || echo missing)
    leftovers=$(ls -A "$root/Applications" | grep -v '^Thoughts.app$' || true)
    i=0
    while [ ! -s "$marker" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    opened=$(tail -1 "$marker" 2>/dev/null || echo none)
    if [ "$status" = "$2" ] && [ "$version" = "$3" ] && [ -z "$leftovers" ] && [ "$opened" = "$3" ] \
        && codesign --verify --deep --strict "$root/Applications/Thoughts.app"; then
        echo "ok   $1"
    else
        echo "FAIL $1: exit $status (want $2), installed $version (want $3), opened $opened, leftovers [$leftovers]"
        failures=$((failures + 1))
    fi
}

mkdir -p "$root/Applications" "$root/home"
make_app "$root/Applications" 1

run_case "refuses an unsigned app" 1 1 "$(make_dmg 2 unsigned)"
run_case "refuses another identifier" 1 1 "$(make_dmg 2 org.tbend.thoughts.other)"
run_case "refuses an image without Thoughts.app" 1 1 "$(rm -rf "$root/empty"; mkdir "$root/empty"; hdiutil create -quiet -srcfolder "$root/empty" -ov "$root/empty.dmg"; echo "$root/empty.dmg")"
run_case "refuses a damaged image" 1 1 "$(head -c 4096 "$(make_dmg 2)" >"$root/broken.dmg"; echo "$root/broken.dmg")"
chmod a-w "$root/Applications"
run_case "refuses a folder it cannot change" 1 1 "$(make_dmg 2)"
chmod u+w "$root/Applications"
dmg=$(make_dmg 2)
run_case "installs a signed newer app" 0 2 "$dmg"
[ ! -e "$dmg" ] && echo "ok   removes the installed image" || { echo "FAIL the image was not removed"; failures=$((failures + 1)); }
grep -q '^failed: ' "$root/home/Library/Logs/Thoughts/install-update.log" && echo "ok   logs refusals" || { echo "FAIL no refusal in the log"; failures=$((failures + 1)); }

[ "$failures" = 0 ] && echo "All install-update.sh tests passed." || { echo "$failures failed."; exit 1; }
