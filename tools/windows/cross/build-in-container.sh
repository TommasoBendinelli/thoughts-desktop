#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Runs inside the thoughts-wincross container: configures, builds and installs the
# Windows build of Thoughts into $STAGE.
# Usage: build-in-container.sh SRC SDK BUILD STAGE VERSION REPOSITORY SERVER_URL [LEGACY_SERVER_URL]
# SERVER_URL is the server new installations start with; LEGACY_SERVER_URL (may be
# empty) the earlier fixed address that already synchronized installations keep.
set -euo pipefail
SRC=$1 SDK=$2 BUILD=$3 STAGE=$4 VERSION=$5 REPOSITORY=$6 SERVER_URL=$7 LEGACY_SERVER_URL=${8:-} JOBS=${JOBS:-16}
: "${WINCROSS:?}"
export LD_LIBRARY_PATH="$WINCROSS/craft-host/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Only the Windows libraries' pkg-config files, never the container's own.
export PKG_CONFIG_LIBDIR="$WINCROSS/craft-win/lib/pkgconfig" PKG_CONFIG_PATH=
export CCACHE_DIR="$WINCROSS/ccache" CCACHE_BASEDIR="$(dirname "$SRC")" CCACHE_MAXSIZE=20G
# The programs carry no builder paths: __FILE__ (and debug info) name src\... for the
# sources and wincross\... for everything else in the builder's tree (the longest prefix
# wins). clang-cl needs the /clang: spelling; it would drop a bare -ffile-prefix-map
# silently, since the toolchain sets -Wno-unknown-argument. Passed on every configure, so
# an existing build folder picks it up too.
RELEASE_FLAGS="/O2 /Ob2 /DNDEBUG -Wno-error=undef /clang:-ffile-prefix-map=$WINCROSS=wincross /clang:-ffile-prefix-map=$SRC=src"
overlay="$WINCROSS/craft-include-overlay.json"
if [ ! -f "$overlay" ] || [ -n "$(find "$WINCROSS/craft-win/include" -newer "$overlay" -print -quit)" ]; then
    python3 "$(dirname "$0")/case-insensitive-overlay.py" "$overlay" "$WINCROSS/craft-win/include"
fi
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/tools/windows/cross/clangcl-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_C_FLAGS_RELEASE=$RELEASE_FLAGS" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$RELEASE_FLAGS" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_INSTALL_PREFIX="$STAGE" \
        -DBUILD_TESTING=OFF -DBUILD_DESKTOP=ON \
        -DKDE_INSTALL_PLUGINDIR=plugins -DKDE_INSTALL_QTPLUGINDIR=plugins \
        -DTHOUGHTS_SERVER_SDK_ROOT="$SDK" \
        -DFORCE_NOT_REQUIRED_DEPENDENCIES=Discount \
        -DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON \
        -DTHOUGHTS_RELEASE_VERSION="$VERSION" \
        -DTHOUGHTS_UPDATE_REPOSITORY="$REPOSITORY" \
        -DTHOUGHTS_DEFAULT_SERVER_URL="$SERVER_URL" \
        -DTHOUGHTS_LEGACY_SERVER_URL="$LEGACY_SERVER_URL"
else
    cmake "$BUILD" "-DCMAKE_C_FLAGS_RELEASE=$RELEASE_FLAGS" "-DCMAKE_CXX_FLAGS_RELEASE=$RELEASE_FLAGS" \
        -DTHOUGHTS_RELEASE_VERSION="$VERSION" -DTHOUGHTS_UPDATE_REPOSITORY="$REPOSITORY" \
        -DTHOUGHTS_DEFAULT_SERVER_URL="$SERVER_URL" -DTHOUGHTS_LEGACY_SERVER_URL="$LEGACY_SERVER_URL" >/dev/null
fi
nice -n 10 ninja -C "$BUILD" -j "$JOBS"
rm -rf "$STAGE"
cmake --install "$BUILD" >/dev/null
for program in thoughts.exe thoughtsctl.exe thoughts-comment-sync.exe thoughts-publish-sources.exe thoughts-watch-build.exe; do
    [ -f "$STAGE/bin/$program" ] || { echo "missing $program in the install" >&2; exit 3; }
done
echo "installed into $STAGE"
