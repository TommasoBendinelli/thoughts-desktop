# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
# Cross-compiles Thoughts for x86_64 Windows (MSVC ABI) on Linux, with clang-cl,
# lld-link, the xwin-splatted Windows SDK/CRT and KDE Craft's Windows binary cache.
# WINCROSS holds xwin-sdk/, craft-win/ (Windows libraries) and craft-host/ (Linux
# build tools of the same Craft release, so moc and kconfig_compiler match Qt and KF).
if(DEFINED ENV{WINCROSS})
    set(WINCROSS "$ENV{WINCROSS}")
else()
    set(WINCROSS "$ENV{HOME}/thoughts-builder/wincross")
endif()
set(XWIN "${WINCROSS}/xwin-sdk")
set(CRAFTWIN "${WINCROSS}/craft-win")
set(CRAFTHOST "${WINCROSS}/craft-host")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
# Craft's libraries use the release CRT (/MD); the SDK has no debug CRT libraries.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(_flags "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40 /EHsc -Wno-error=undef -Wno-unknown-argument")
foreach(dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
    string(APPEND _flags " /imsvc \"${XWIN}/${dir}\"")
endforeach()
# build-in-container.sh writes this overlay: Craft's headers need case-insensitive lookups.
string(APPEND _flags " -Xclang -ivfsoverlay -Xclang \"${WINCROSS}/craft-include-overlay.json\"")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_RC_FLAGS_INIT "-I \"${XWIN}/sdk/include/um\" -I \"${XWIN}/sdk/include/shared\"")

set(_link "/libpath:\"${XWIN}/crt/lib/x86_64\" /libpath:\"${XWIN}/sdk/lib/um/x86_64\" /libpath:\"${XWIN}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")

set(CMAKE_FIND_ROOT_PATH "${CRAFTWIN}")
set(CMAKE_PREFIX_PATH "${CRAFTWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(QT_HOST_PATH "${CRAFTHOST}" CACHE PATH "")
set(QT_HOST_PATH_CMAKE_DIR "${CRAFTHOST}/lib/cmake" CACHE PATH "")
set(KF6_HOST_TOOLING "${CRAFTHOST}/lib/cmake" CACHE PATH "")
