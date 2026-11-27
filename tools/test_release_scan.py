# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Tests for release_scan.py, from tools/: python3 -m unittest test_release_scan (standard library only)."""
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_scan  # noqa: E402


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.patterns = self.root / "patterns.txt"
        self.patterns.write_text("# this computer's markers\nalex\\.example-person\\.org\nsecret-library-name\n", encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def kinds(self, data: bytes) -> list[str]:
        (self.root / "bundle").mkdir(exist_ok=True)
        (self.root / "bundle" / "lib.dylib").write_bytes(data)
        return sorted({hit.kind for hit in release_scan.scan([str(self.root / "bundle")], str(self.patterns))})

    def test_build_paths_hosts_addresses_and_keys_are_found(self):
        self.assertEqual(self.kinds(b"\0/Users/alex/repos/app/build/shell/main.cpp\0"), ["home folder"])
        self.assertEqual(self.kinds(b"\0/home/alex/wincross/wt/src/x.cpp\0"), ["home folder"])
        self.assertEqual(self.kinds(b"C:\\Users\\alex\\AppData\\x"), ["home folder"])
        # Made-up, and put together here so that this file holds no host or address of that shape.
        self.assertEqual(self.kinds(b"https://machine.tailnet-name" + b".ts" + b".net:8443/"), ["tailnet host"])
        self.assertEqual(self.kinds(b"host " + b".".join([b"100", b"101", b"102", b"103"]) + b" port"), ["Tailscale address"])
        label = b"-----BEGIN OPENSSH " + b"PRIVATE KEY-----"
        self.assertEqual(self.kinds(label + b"\n" + b"A" * 70 + b"\n"), ["private key"])
        self.assertEqual(self.kinds(label + b"\0"), [])  # The label alone is code, not a key.

    def test_fixtures_build_accounts_and_ordinary_numbers_pass(self):
        self.assertEqual(self.kinds(b"https://server.example.ts.net/ /Users/Shared/x /Users/runner/work/qt/ /home/qt/work/"), [])
        self.assertEqual(self.kinds(b"version 100.2.3.4 and 10.100.64.1 and 100.128.0.1"), [])
        # Third-party parts of the Windows installer's Python: GitHub's Windows runner account in wheels'
        # Rust extensions, and the shared address space's network in the standard library's ipaddress.
        self.assertEqual(self.kinds(b"C:\\Users\\runneradmin\\.cargo\\registry\\src\\x.rs"), [])
        self.assertEqual(self.kinds(b"IPv4Network('" + b".".join([b"100", b"64", b"0", b"0"]) + b"/10')"), [])
        self.assertEqual(self.kinds(b"host " + b".".join([b"100", b"64", b"0", b"1"]) + b" port"), ["Tailscale address"])

    def test_this_computers_markers_come_from_its_own_file(self):
        self.assertEqual(self.kinds(b"mail to Alex.Example-Person.org and SECRET-library-name"), ["private marker (line 2)", "private marker (line 3)"])
        hits = release_scan.scan([str(self.root / "bundle")], str(self.root / "missing.txt"))
        self.assertEqual(hits, [])

    def test_an_accepted_home_folder_passes_and_a_private_marker_never_does(self):
        accepted = self.root / "accepted.txt"
        accepted.write_text("# the build libraries live in this home folder\n/Users/alex/\n", encoding="utf-8")
        old = os.environ.get("THOUGHTS_RELEASE_ACCEPTED")
        os.environ["THOUGHTS_RELEASE_ACCEPTED"] = str(accepted)
        try:
            patterns = dict(release_scan.GENERIC_PATTERNS)
            self.assertEqual(release_scan.scan_bytes(b"built in /Users/alex/CraftRoot/lib", "x", patterns), [])
            # Another person's home folder is still a hit, and so is the same text as a private marker.
            self.assertEqual([hit.kind for hit in release_scan.scan_bytes(b"/Users/sam/x", "x", patterns)], ["home folder"])
            marker = {"private marker (line 1)": rb"/Users/alex/"}
            self.assertEqual(len(release_scan.scan_bytes(b"/Users/alex/CraftRoot", "x", marker)), 1)
        finally:
            if old is None:
                del os.environ["THOUGHTS_RELEASE_ACCEPTED"]
            else:
                os.environ["THOUGHTS_RELEASE_ACCEPTED"] = old

    def test_accepted_lines_copied_to_a_builder_judge_as_here(self):
        accepted = self.root / "accepted.txt"
        accepted.write_text("# upstream build paths in bundled libraries\n\n  /home/alex/  \n/Users/sam/\n", encoding="utf-8")
        lines = release_scan.accepted_lines(accepted)
        self.assertEqual(lines, ["/home/alex/", "/Users/sam/"])
        self.assertEqual(release_scan.accepted_lines(self.root / "missing.txt"), [])
        # What a release copies to its builder, which scans with THOUGHTS_RELEASE_ACCEPTED pointing at the copy.
        copy = self.root / "builder-accepted.txt"
        copy.write_text("\n".join(lines), encoding="utf-8")
        marker = self.root / "markers.txt"
        marker.write_text("/Users/sam/\n", encoding="utf-8")
        (self.root / "runtime").mkdir()
        (self.root / "runtime" / "lib.dll").write_bytes(b"\0/home/alex/src/x.c\0")
        script = str(Path(release_scan.__file__))
        env = {**os.environ, "THOUGHTS_RELEASE_ACCEPTED": str(copy)}
        run = lambda *args: subprocess.run([sys.executable, script, "scan", *args], capture_output=True, text=True, env=env)
        self.assertEqual(run("--patterns", str(self.patterns), str(self.root / "runtime")).returncode, 0)
        # An accepted line never accepts a private marker.
        (self.root / "runtime" / "app.exe").write_bytes(b"\0/Users/sam/x\0")
        result = run("--patterns", str(marker), str(self.root / "runtime"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("private marker (line 1): '/Users/sam/'", result.stderr)
        self.assertNotIn("/home/alex/", result.stderr)

    def test_zip_members_are_scanned(self):
        archive = self.root / "out.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
            out.writestr("bin/app.exe", b"\0/Users/alex/build\0/")
        hits = release_scan.scan([str(archive)], str(self.patterns))
        self.assertEqual([(hit.kind, hit.path) for hit in hits], [("home folder", f"{archive}!bin/app.exe")])

    def test_command_line_exit_codes(self):
        clean = self.root / "clean.bin"
        clean.write_bytes(b"nothing personal")
        script = str(Path(release_scan.__file__))
        self.assertEqual(subprocess.run([sys.executable, script, "scan", "--patterns", str(self.patterns), str(clean)], capture_output=True).returncode, 0)
        (self.root / "dirty.bin").write_bytes(b"/home/alex/x")
        result = subprocess.run([sys.executable, script, "scan", str(self.root / "dirty.bin")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("home folder: '/home/alex/'", result.stderr)


class SourceTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_AUTHOR_NAME": "Alex Example", "GIT_AUTHOR_EMAIL": "alex@example.org",
               "GIT_COMMITTER_NAME": "Alex Example", "GIT_COMMITTER_EMAIL": "alex@example.org"}
        self.git = lambda *args, cwd=None: subprocess.run(["git", *args], cwd=cwd or self.clone, check=True, capture_output=True, text=True, env=env).stdout.strip()
        self.clone = self.root / "clone"
        self.git("init", "-q", "-b", "main", str(self.clone), cwd=self.root)
        (self.clone / "a.txt").write_text("a\n")
        self.git("add", "a.txt")
        self.git("commit", "-q", "-m", "a")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")

    def tearDown(self):
        self.directory.cleanup()

    def problem(self):
        return release_scan.source_problem(str(self.clone), "example-owner/thoughts-desktop", fetch=False)

    def test_public_urls(self):
        for url in ("git@github.com:example-owner/thoughts-desktop.git", "https://github.com/example-owner/thoughts-desktop",
                    "ssh://git@github.com/Example-Owner/thoughts-desktop.git"):
            self.assertTrue(release_scan.is_public_url(url, "example-owner/thoughts-desktop"), url)
        for url in ("git@github.com:example-owner/thoughts-desktop-private.git", "/tmp/thoughts-desktop", ""):
            self.assertFalse(release_scan.is_public_url(url, "example-owner/thoughts-desktop"), url)

    def test_only_a_clean_public_clone_on_main_may_release(self):
        self.git("remote", "add", "origin", "git@github.com:example-owner/thoughts-desktop-private.git")
        self.assertIn("private remote", self.problem())
        self.git("remote", "set-url", "origin", "git@github.com:example-owner/thoughts-desktop.git")
        self.git("remote", "add", "private", "git@github.com:example-owner/thoughts-desktop-private.git")
        self.assertIn("private remote (private)", self.problem())
        self.git("remote", "remove", "private")
        self.assertIsNone(self.problem())
        (self.clone / "a.txt").write_text("changed\n")
        self.assertIn("uncommitted", self.problem())
        self.git("commit", "-q", "-am", "local only")
        self.assertIn("is not on example-owner/thoughts-desktop's main", self.problem())


if __name__ == "__main__":
    unittest.main()
