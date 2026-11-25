#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Prepares the Linux builder for tools/windows/release.py. Run on the builder after
# copying the Mac's ~/tools/wincross/{pkgs,xwin-cache} to $W (see README.md):
#   bash setup-builder.sh
# Idempotent; re-running rebuilds the trees from the archives.
set -euo pipefail
RB=${RB:-$HOME/thoughts-builder}
W=$RB/wincross
PODMAN=$RB/bin/thoughts-desktop-podman
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$W"

# 1. Containers: clang-cl/lld-link on top of the desktop build image; NSIS from Debian.
mkdir -p image nsis-image
cat > image/Containerfile <<'EOF'
FROM localhost/thoughts-desktop-build:20261015
RUN pacman -Sy --noconfirm --needed llvm 7zip && { yes | pacman -Scc >/dev/null || true; } && rm -rf /var/cache/pacman/pkg/*
EOF
printf 'FROM docker.io/library/debian:trixie-slim\nRUN apt-get update && apt-get install -y --no-install-recommends nsis && rm -rf /var/lib/apt/lists/*\n' > nsis-image/Containerfile
$PODMAN build -t localhost/thoughts-wincross:20261015 image
$PODMAN build -t localhost/thoughts-nsis:20261015 nsis-image
# Wine on a virtual display, for wine-smoke-test.sh.
mkdir -p wine-image
cat > wine-image/Containerfile <<'EOF'
FROM docker.io/library/archlinux:latest
RUN printf "\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n" >> /etc/pacman.conf \
 && pacman -Syu --noconfirm --needed wine xorg-server-xvfb xorg-xwininfo xdotool imagemagick python ttf-dejavu \
 && { yes | pacman -Scc >/dev/null || true; } && rm -rf /var/cache/pacman/pkg/*
EOF
$PODMAN build -t localhost/thoughts-winetest:20261015 wine-image

# 2. Windows SDK and CRT. xwin splats on Linux with the lower-case aliases that a
# case-sensitive file system needs (kernel32.lib for Kernel32.Lib).
if [ ! -x xwin ]; then
    v=0.10.0
    curl -sSfL "https://github.com/Jake-Shadle/xwin/releases/download/$v/xwin-$v-x86_64-unknown-linux-musl.tar.gz" | tar xz
    mv "xwin-$v-x86_64-unknown-linux-musl/xwin" . && rm -rf "xwin-$v-x86_64-unknown-linux-musl"
fi
rm -rf xwin-sdk
./xwin --accept-license --cache-dir xwin-cache --manifest-version 17 --sdk-version 10.0.26100 --crt-version 14.44.17.14 splat --output xwin-sdk

# The MSVC runtime DLLs, deployed next to the programs (Microsoft's app-local
# deployment), from the same Visual Studio manifest xwin used.
mkdir -p vcredist && python3 - <<'EOF'
import hashlib, io, json, os, shutil, urllib.request, zipfile
channel = json.load(open("xwin-cache/dl/manifest_17.json"))
url = next(p["url"] for i in channel["channelItems"] if i.get("type") == "Manifest" for p in i["payloads"])
packages = json.load(urllib.request.urlopen(url))["packages"]
payload = next(p for p in packages if p["id"] == "Microsoft.VC.14.44.17.14.CRT.Redist.X64.base")["payloads"][0]
data = urllib.request.urlopen(payload["url"]).read()
assert hashlib.sha256(data).hexdigest() == payload["sha256"]
shutil.rmtree("vcredist/x64", ignore_errors=True)
os.makedirs("vcredist/x64")
with zipfile.ZipFile(io.BytesIO(data)) as z:
    for name in z.namelist():
        if ("/x64/Microsoft.VC143.CRT/" in name or "/x64/Microsoft.VC143.OpenMP/" in name) and "debug_nonredist" not in name and name.endswith(".dll"):
            open(os.path.join("vcredist/x64", os.path.basename(name)), "wb").write(z.read(name))
EOF

# 3. KDE Craft 26.05 Windows libraries (MSVC 2022), plus gpgme from Craft's MinGW
# cache, unpacked on Linux so folders differing only in case stay apart.
rm -rf craft-win && mkdir craft-win
$PODMAN run --rm --network none -v "$W:$W" -w "$W/craft-win" localhost/thoughts-wincross:20261015 \
    bash -c 'for f in ../pkgs/*.7z; do 7z x -y -bso0 -bsp0 "$f"; done; for f in ../pkgs/*.zip; do 7z x -y -bso0 -bsp0 "$f"; done'
# Craft's pkg-config files name its own build prefix; CMake checks this include path exists.
sed -i "s#C:/Craft/windows-msvc2022_64-cl#$W/craft-win#g" craft-win/lib/pkgconfig/*.pc
ln -sfn KExiv2 craft-win/include/KExiv2Qt6/kexiv2

# 4. Linux build tools (moc, rcc, uic, kconfig_compiler, msgfmt) of the same Craft release.
base=https://files.kde.org/craft/Qt6/26.05/linux/gcc/x86_64/RelWithDebInfo
mkdir -p host-pkgs && curl -sSfL -o host-manifest.json "$base/manifest.json"
python3 - "$base" <<'EOF'
import hashlib, json, os, subprocess, sys
base = sys.argv[1]
(_, packages), = json.load(open("host-manifest.json"))["packages"].items()
by = {p["name"]: p for p in packages}
want = """libs/qt6/qtbase libs/qt6/qttools libs/qt6/qtdeclarative libs/qt6/qtshadertools libs/qt6/qtsvg
kde/frameworks/extra-cmake-modules kde/frameworks/tier1/kconfig kde/frameworks/tier1/kcoreaddons kde/frameworks/tier1/ki18n
libs/gettext libs/icu libs/zlib libs/libzstd libs/pcre2 libs/openssl libs/libb2 libs/dbus libs/glib libs/libffi libs/iconv
libs/libxml2 libs/expat libs/liblzma libs/libbzip2 libs/brotli libs/freetype libs/libpng libs/harfbuzz libs/fontconfig libs/libjpeg-turbo""".split()
out = []
for name in want:
    binary = by[name]["build"][-1]["files"]["Binary"]
    path = "host-pkgs/" + os.path.basename(binary["fileName"])
    if not os.path.exists(path):
        subprocess.run(["curl", "-sSfL", "--retry", "3", "-o", path, f"{base}/{binary['fileName']}"], check=True)
    assert hashlib.sha256(open(path, "rb").read()).hexdigest() == binary["checksum"], name
    out.append(path)
open("host-pkgs/list", "w").write("\n".join(out))
EOF
rm -rf craft-host && mkdir craft-host
$PODMAN run --rm --network none -v "$W:$W" -w "$W/craft-host" localhost/thoughts-wincross:20261015 \
    bash -c 'for p in $(cat ../host-pkgs/list); do 7z x -so "../$p" | tar x; done'
rm -f craft-include-overlay.json

# 5. Microsoft's WebView2 SDK, for live HTML pages (part/htmllivewebview_win.cpp): the
# Microsoft.Web.WebView2 NuGet package from nuget.org, unpacked. Its loader is linked in
# statically (build/native/x64/WebView2LoaderStatic.lib), so no WebView2Loader.dll ships.
wv=1.0.4314.35
wv_sha256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
mkdir -p pkgs
nupkg=pkgs/microsoft.web.webview2.$wv.nupkg
[ -f "$nupkg" ] || curl -sSfL --retry 3 -o "$nupkg" "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$wv/microsoft.web.webview2.$wv.nupkg"
echo "$wv_sha256  $nupkg" | sha256sum -c --quiet -
rm -rf "webview2-sdk/$wv" && mkdir -p "webview2-sdk/$wv"
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$nupkg" "webview2-sdk/$wv"
echo "Builder ready in $W"
