#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Compares the publisher's native merge (git's xdiff, tools/publisher/xdiff)
with `git merge-file -p -L FEDORA_CURRENT -L COMMON_BASE -L PHONE_INCOMING
current base incoming`, which the Thoughts server runs, byte for byte: merged
output and exit status (the conflict count), or the refusal for binary input.

Python 3 standard library only, so the git half also runs on Fedora:

  merge_differential.py generate CASES [--seed 1] [--random 5000] [--corpus DIR --real 2000]
  merge_differential.py git CASES OUT [--git git]          # expected results
  merge_differential.py native CASES OUT --mergecheck BIN [--runner wine]
  merge_differential.py compare CASES EXPECTED ACTUAL
  merge_differential.py all CASES --mergecheck BIN [--git git] [generate options]

CASES/<name>/{current,base,incoming} are the inputs; OUT/<name>.out holds the
merged bytes and OUT/<name>.code the exit status, or the "error: ..." line git
prints when it refuses. --corpus takes a directory of real .tex/.md files
(copies, never the Library itself) to make realistic edit triples from.
"""
import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

SIDES = ("current", "base", "incoming")
LABELS = ("FEDORA_CURRENT", "COMMON_BASE", "PHONE_INCOMING")

# Few distinct lines, so that diffs have many equally good alignments.
VOCABULARY = [
    b"a", b"b", b"c", b"x", b"y", b"", b" ", b"}", b"{", b"\\item one", b"\\item two", b"\\section{Intro}",
    b"%", b"--", b"=", b"...", b"\\end{itemize}", b"\\begin{itemize}", b"The quick brown fox.",
    b"caf\xc3\xa9 na\xc3\xafve \xe2\x80\x94 \xf0\x9f\x98\x80", b"\xc3\xa9", b"\t\tindented", b"trailing space ",
    b"<<<<<<< FEDORA_CURRENT", b"=======", b">>>>>>> PHONE_INCOMING", b"||||||| COMMON_BASE", b"<<<<<<<", b">>>>>>>",
    b"$x^2$", b"## Heading", b"- [ ] task", b"1. first", b"```", b"\r", b"a\rb", b"\xff\xfe broken utf-8 \x80",
]


def random_line(rng):
    roll = rng.random()
    if roll < 0.75:
        return rng.choice(VOCABULARY)
    if roll < 0.85:
        return bytes(rng.choice(b"abcxyz0123 .,;{}\\%$") for _ in range(rng.randint(1, 12)))
    if roll < 0.92:
        return "".join(rng.choice("éàüß€😀中文ñ ") for _ in range(rng.randint(1, 8))).encode()
    if roll < 0.97:
        return (rng.choice(VOCABULARY) or b"z") * rng.randint(200, 4000)  # very long lines
    return bytes(rng.randint(1, 255) for _ in range(rng.randint(1, 20)))


def join(lines, rng, style):
    """Lines to bytes with the given ending style; the final newline may be missing."""
    if not lines:
        return b""
    if style == "lf":
        endings = [b"\n"] * len(lines)
    elif style == "crlf":
        endings = [b"\r\n"] * len(lines)
    else:
        endings = [rng.choice([b"\n", b"\r\n", b"\r\n", b"\n"]) for _ in lines]
    if rng.random() < 0.25:
        endings[-1] = b""
    return b"".join(line + ending for line, ending in zip(lines, endings))


def edit(lines, rng, count, make_line):
    lines = list(lines)
    for _ in range(count):
        position = rng.randint(0, len(lines))
        kind = rng.random()
        if kind < 0.35 and position < len(lines):
            lines[position] = make_line()
        elif kind < 0.6:
            lines[position:position] = [make_line() for _ in range(rng.randint(1, 4))]
        elif kind < 0.85 and lines:
            del lines[position:position + rng.randint(1, 3)]
        elif lines:  # move a block
            start = rng.randrange(len(lines))
            block = lines[start:start + rng.randint(1, 3)]
            del lines[start:start + len(block)]
            target = rng.randint(0, len(lines))
            lines[target:target] = block
    return lines


def nearby(base, lines, rng, make_line):
    """Edits close to (adjacent to or overlapping) the region the other side changed."""
    lines = list(lines)
    if not lines:
        return [make_line()]
    position = rng.randrange(len(lines))
    span = rng.randint(1, 3)
    choice = rng.random()
    if choice < 0.4:
        lines[position:position + span] = [make_line() for _ in range(span)]
    elif choice < 0.7:
        lines.insert(position, make_line())
    else:
        del lines[position:position + span]
    return lines


def random_triple(rng):
    style = rng.choice(["lf", "lf", "lf", "crlf", "mixed"])
    if rng.random() < 0.05:
        base = []
    else:
        base = [random_line(rng) for _ in range(rng.randint(1, 40))]
    make = lambda: random_line(rng)  # noqa: E731
    current = edit(base, rng, rng.randint(0, 4), make)
    roll = rng.random()
    if roll < 0.15:
        incoming = list(current)  # identical changes on both sides
        incoming = edit(incoming, rng, rng.randint(0, 1), make)
    elif roll < 0.5:
        incoming = nearby(base, current if rng.random() < 0.3 else base, rng, make)
    else:
        incoming = edit(base, rng, rng.randint(0, 4), make)
    triple = [join(side, random.Random(rng.random()), style) for side in (current, base, incoming)]
    if rng.random() < 0.03:  # NUL bytes: binary for git, wherever they are
        which = rng.randrange(3)
        data = bytearray(triple[which])
        data.insert(rng.randint(0, len(data)), 0)
        triple[which] = bytes(data)
    if rng.random() < 0.02:
        triple[rng.randrange(3)] = b""
    return triple


def word_edit(line, rng):
    words = line.split(b" ")
    position = rng.randrange(len(words))
    words[position] = rng.choice([b"changed", b"edited", words[position].upper(), b"", b"\xc3\xa9t\xc3\xa9", words[position] + b","])
    return b" ".join(words)


def real_triple(rng, text):
    lines = text.split(b"\n")
    final = lines.pop() if lines else b""
    if len(lines) > 400:
        start = rng.randrange(len(lines) - 300)
        lines = lines[start:start + rng.randint(50, 300)]
    pool = [line for line in lines if line.strip()] or [b"x"]

    def make():
        roll = rng.random()
        if roll < 0.5:
            return word_edit(rng.choice(pool), rng)
        if roll < 0.8:
            return rng.choice(pool)
        return rng.choice([b"", b"% comment", b"\\paragraph{New?}", b"New sentence.", b"}"])

    def side(source):
        result = list(source)
        for _ in range(rng.randint(1, 6)):
            if not result:
                result.append(make())
                continue
            position = rng.randrange(len(result))
            roll = rng.random()
            if roll < 0.5:
                result[position] = word_edit(result[position], rng) if result[position] else make()
            elif roll < 0.75:
                result[position:position] = [make() for _ in range(rng.randint(1, 5))]
            else:
                del result[position:position + rng.randint(1, 8)]
        return result

    current = side(lines)
    incoming = list(current) if rng.random() < 0.1 else side(lines)
    if rng.random() < 0.2:  # both sides touch the same paragraph
        position = rng.randrange(max(1, len(lines)))
        current = current[:position] + [word_edit(rng.choice(pool), rng)] + current[position:]
        incoming = incoming[:position] + [word_edit(rng.choice(pool), rng)] + incoming[position:]
    crlf = b"\r\n" if rng.random() < 0.1 else b"\n"
    return [crlf.join(side_lines + [final]) for side_lines in (current, lines, incoming)]


def generate(arguments):
    rng = random.Random(arguments.seed)
    cases = Path(arguments.cases)
    cases.mkdir(parents=True, exist_ok=True)
    triples = []
    # Fixed edge cases first.
    triples += [
        [b"", b"", b""], [b"a\n", b"", b"b\n"], [b"", b"a\n", b"b\n"], [b"a", b"", b"a"], [b"a\n", b"a", b"a\r\n"],
        [b"x\na\n", b"a\n", b"y\na\n"], [b"a\nb\nc\n", b"a\nc\n", b"a\nB\nc\n"], [b"1\n2\n3", b"1\n2\n3\n", b"1\n2\n3\r\n"],
        [b"a\0b\n", b"a\n", b"c\n"], [b"a\n", b"a\0\n", b"c\n"], [b"a\n", b"b\n", b"c\0\n"],
        [b"x" * 9000 + b"\0", b"a\n", b"b\n"], [b"a\r\nb\r\n", b"a\r\nc\r\n", b"a\r\nd\r\n"],
        [b"<<<<<<< FEDORA_CURRENT\nx\n=======\ny\n>>>>>>> PHONE_INCOMING\n", b"z\n", b"w\n"],
    ]
    triples += [random_triple(rng) for _ in range(arguments.random)]
    if arguments.corpus:
        files = sorted(path for path in Path(arguments.corpus).rglob("*") if path.is_file() and path.suffix in (".tex", ".md"))
        texts = [path.read_bytes() for path in files]
        texts = [text for text in texts if text]
        for _ in range(arguments.real if texts else 0):
            triples.append(real_triple(rng, rng.choice(texts)))
    names = []
    for index, triple in enumerate(triples):
        name = f"case{index:05d}"
        directory = cases / name
        directory.mkdir(exist_ok=True)
        for side, data in zip(SIDES, triple):
            (directory / side).write_bytes(data)
        names.append(name)
    (cases / "names.txt").write_text("\n".join(names) + "\n")
    print(len(names), "cases in", cases)


def names_of(cases):
    return [name for name in (Path(cases) / "names.txt").read_text().split("\n") if name]


def run_git(arguments):
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    git = arguments.git.split()
    for name in names_of(arguments.cases):
        command = git + ["merge-file", "-p", "-L", LABELS[0], "-L", LABELS[1], "-L", LABELS[2], *SIDES]
        completed = subprocess.run(command, cwd=Path(arguments.cases) / name, capture_output=True, check=False)
        (out / f"{name}.out").write_bytes(completed.stdout)
        if 0 <= completed.returncode <= 127:
            code = f"{completed.returncode}\n".encode()
        else:
            code = completed.stderr.strip() + b"\n" if completed.stderr.strip() else f"exit {completed.returncode}\n".encode()
        (out / f"{name}.code").write_bytes(code)
    print(subprocess.run(git + ["--version"], capture_output=True, text=True).stdout.strip(), "->", out)


def run_native(arguments):
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    command = (arguments.runner.split() if arguments.runner else []) + [arguments.mergecheck, arguments.cases, str(out)]
    subprocess.run(command, input=(Path(arguments.cases) / "names.txt").read_bytes(), check=True)


def compare(arguments):
    names = names_of(arguments.cases)
    mismatches = []
    conflicts = refusals = 0
    for name in names:
        expected = [(Path(arguments.expected) / f"{name}{suffix}").read_bytes() for suffix in (".out", ".code")]
        actual = [(Path(arguments.actual) / f"{name}{suffix}").read_bytes() for suffix in (".out", ".code")]
        if expected[1].startswith(b"error"):
            refusals += 1
        elif expected[1] != b"0\n":
            conflicts += 1
        if expected != actual:
            mismatches.append(name)
    print(f"{len(names)} cases ({conflicts} with conflicts, {refusals} refused as binary); {len(mismatches)} mismatches",
          mismatches[:20])
    return 1 if mismatches else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def generate_options(command):
        command.add_argument("--seed", type=int, default=1)
        command.add_argument("--random", type=int, default=5000)
        command.add_argument("--corpus")
        command.add_argument("--real", type=int, default=2000)

    command = commands.add_parser("generate")
    command.add_argument("cases")
    generate_options(command)
    command = commands.add_parser("git")
    command.add_argument("cases")
    command.add_argument("out")
    command.add_argument("--git", default="git")
    command = commands.add_parser("native")
    command.add_argument("cases")
    command.add_argument("out")
    command.add_argument("--mergecheck", required=True)
    command.add_argument("--runner")
    command = commands.add_parser("compare")
    command.add_argument("cases")
    command.add_argument("expected")
    command.add_argument("actual")
    command = commands.add_parser("all")
    command.add_argument("cases")
    command.add_argument("--mergecheck", required=True)
    command.add_argument("--git", default="git")
    command.add_argument("--runner")
    generate_options(command)
    arguments = parser.parse_args()
    if arguments.command == "generate":
        generate(arguments)
    elif arguments.command == "git":
        run_git(arguments)
    elif arguments.command == "native":
        run_native(arguments)
    elif arguments.command == "compare":
        return compare(arguments)
    else:
        generate(arguments)
        root = Path(arguments.cases)
        arguments.out = str(root / "git")
        run_git(arguments)
        arguments.out = str(root / "native")
        run_native(arguments)
        arguments.expected, arguments.actual = str(root / "git"), str(root / "native")
        return compare(arguments)
    return 0


if __name__ == "__main__":
    sys.exit(main())
