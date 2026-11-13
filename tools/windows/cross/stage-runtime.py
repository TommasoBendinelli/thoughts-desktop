#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Assembles the files the Windows installer ships: Thoughts' install tree plus the
parts of the KDE Craft tree it needs at run time. DLLs are chosen by following the
PE import tables of every shipped program and plugin, so development tools, headers
and import libraries stay out. A plugin whose libraries are neither in Craft's tree
nor part of Windows is left out. The MSVC runtime comes from Microsoft's
redistributable package (VCREDIST), deployed next to the programs.
Usage: stage-runtime.py INSTALL CRAFT VCREDIST WINSDK OUT"""
import os
import shutil
import struct
import sys

INSTALL, CRAFT, VCREDIST, WINSDK, OUT = (os.path.abspath(p) for p in sys.argv[1:6])

# Craft programs Thoughts (through KDE Frameworks) starts at run time.
CRAFT_PROGRAMS = ["kioworker.exe", "kbuildsycoca6.exe", "dbus-daemon.exe", "gpgme-w32spawn.exe", "update-mime-database.exe"]
# Qt/KF plugin folders that only serve development tools.
SKIP_PLUGINS = {"designer", "qmllint", "qmlls", "qmltooling", "help"}
# bin/data (XDG data on Windows) folders worth shipping; translations are left out.
DATA = ["icons", "mime", "kf6", "kxmlgui5", "glib-2.0", "qlogging-categories6"]
SHARE = ["poppler"]


def pe_imports(path):
    """Names of the DLLs a PE file imports, including delay-loaded ones."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return []
    sections_count, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    sections = []
    for i in range(sections_count):
        s = optional + optional_size + 40 * i
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, max(vsize, rsize), raddr))

    def offset(rva):
        for vaddr, size, raddr in sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + raddr
        return None

    def name_at(rva):
        o = offset(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace") if o is not None else None

    names = []
    for index, entry_size, name_field in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from("<I", data, directories + 8 * index)[0]
        o = offset(rva) if rva else None
        while o is not None and o + entry_size <= len(data):
            entry = data[o:o + entry_size]
            if not any(entry):
                break
            name = name_at(struct.unpack_from("<I", entry, name_field)[0])
            if name:
                names.append(name)
            o += entry_size
    return names


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst):
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=False,
                        ignore=shutil.ignore_patterns("*.pdb", "*.lib", "*.cmake", "*.pc"))


shutil.rmtree(OUT, ignore_errors=True)
copytree(INSTALL, OUT)
# Headers and CMake files of okularcore are for developers.
for folder in ("include", os.path.join("lib", "cmake")):
    shutil.rmtree(os.path.join(OUT, folder), ignore_errors=True)
for program in CRAFT_PROGRAMS:
    if os.path.exists(os.path.join(CRAFT, "bin", program)):
        copy(os.path.join(CRAFT, "bin", program), os.path.join(OUT, "bin", program))
for folder in sorted(os.listdir(os.path.join(CRAFT, "plugins"))):
    if folder not in SKIP_PLUGINS:
        copytree(os.path.join(CRAFT, "plugins", folder), os.path.join(OUT, "plugins", folder))
for folder in DATA:
    copytree(os.path.join(CRAFT, "bin", "data", folder), os.path.join(OUT, "bin", "data", folder))
for folder in SHARE:
    copytree(os.path.join(CRAFT, "share", folder), os.path.join(OUT, "share", folder))
copytree(os.path.join(CRAFT, "etc"), os.path.join(OUT, "etc"))

# A DLL Windows provides has an import library in the Windows SDK.
system_dlls = {"winspool.drv"}
for directory in ("sdk/lib/um/x86_64", "sdk/lib/ucrt/x86_64"):
    for name in os.listdir(os.path.join(WINSDK, directory)):
        if name.lower().endswith(".lib"):
            system_dlls.add(name[:-4].lower() + ".dll")
available = {}
for directory in (os.path.join(CRAFT, "bin"), VCREDIST):
    for name in os.listdir(directory):
        if name.lower().endswith(".dll"):
            available.setdefault(name.lower(), os.path.join(directory, name))


def closure(path, seen, missing):
    """DLLs path needs beyond Windows, resolved in available; unresolvable ones go to missing."""
    for dependency in pe_imports(path):
        key = dependency.lower()
        if key in seen or key in system_dlls or key.startswith(("api-ms-win-", "ext-ms-")):
            continue
        seen.add(key)
        if key in available:
            closure(available[key], seen, missing)
        elif key not in shipped:
            missing.add(key)


shipped = {}
for root, _, files in os.walk(OUT):
    for name in files:
        if name.lower().endswith((".exe", ".dll")):
            shipped[name.lower()] = os.path.join(root, name)
needed, dropped = set(), []
for key, path in sorted(shipped.items()):
    seen, missing = set(), set()
    closure(path, seen, missing)
    if missing:
        if os.sep + "plugins" + os.sep not in path:
            sys.exit(f"{os.path.relpath(path, OUT)} needs {', '.join(sorted(missing))}, which nothing provides")
        dropped.append(f"{os.path.relpath(path, OUT)} ({', '.join(sorted(missing))})")
        os.remove(path)
        continue
    needed |= seen
for key in sorted(needed - set(shipped)):
    copy(available[key], os.path.join(OUT, "bin", os.path.basename(available[key])))

# Craft's Qt finds its plugins relative to a qt.conf next to the programs; without
# it Thoughts stops with "no Qt platform plugin could be initialized".
with open(os.path.join(OUT, "bin", "qt.conf"), "w") as f:
    f.write("[Paths]\nPrefix=..\n")

size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(OUT) for f in fs)
print(f"staged {OUT}: {len(shipped) - len(dropped) + len(needed - set(shipped))} programs and libraries, {size / 1e6:.0f} MB")
for line in dropped:
    print("left out", line)
