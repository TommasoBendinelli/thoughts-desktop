#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Compares mediatypescheck with Python's mimetypes on every known extension
and awkward names. Run with the server's Python on macOS, whose module table
the frozen one reproduces:  python3 mediatypes_differential.py <build>/bin/mediatypescheck"""
import mimetypes
import subprocess
import sys

mimetypes.init()


def expected(name):
    return mimetypes.guess_type(name)[0] or "application/octet-stream"


names = []
for extension in mimetypes._db.types_map[True]:
    names += ["f" + extension, "F" + extension.upper(), "x.y" + extension, extension, "." + extension,
              "a" + extension + ".gz", "a" + extension + ".GZ", "a" + extension + ".Z", "a" + extension + ".br"]
names += ["a.tgz", "a.TGZ", "a.svgz", "x.tar.gz", "x.tar.bz2", "noext", ".bashrc", "..png", "a..png", "a.", "a. png",
          "é.png", "😀.svg", "a:b.png", "http:x.png", "http://h/x.png?q#f", "http:x;y.png", "ftp:a/b;c.png",
          "data:image/png;base64,xx", "data:text", "data:a=b,c", "c:x.png", "C:.png", "1a:b.png", "a+b:c.pdf",
          ".tar.gz", "a.tar.Z", "a.tar.z", "a.tex.gz", "A.TEX", "x.jpeg", "x.JPG", "x.Svg", "x.mD", "x.markdown"]
output = subprocess.run([sys.argv[1]], input="\n".join(names).encode(), capture_output=True, check=True).stdout.decode().split("\n")
mismatches = [(name, expected(name), got) for name, got in zip(names, output) if expected(name) != got]
print(len(names), "names;", len(mismatches), "mismatches", mismatches[:10])
sys.exit(1 if mismatches else 0)
