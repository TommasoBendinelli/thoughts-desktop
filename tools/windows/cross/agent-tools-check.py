# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Run by wine-smoke-test.sh with the installed Python of the Thoughts tools:
    python.exe -X utf8 -B agent-tools-check.py <installation folder>
Checks that this Python has what the tools need, that the tools reach Windows' own APIs as they do
on a PC (Credential Manager, processes, file locks; under Wine, Wine's), that the MCP server answers
an MCP initialize on its standard input, and that the waker runs. Prints one line per check and
"agent tools: ok" at the end; exits 1 on the first failure. Prints no secret (there is none here)."""
import json
import os
import subprocess
import sys
import tempfile
import time

here = sys.argv[1]
mcp_dir = os.path.join(here, "tools", "mcp")
waker_dir = os.path.join(here, "tools", "waker")
sys.path[:0] = [mcp_dir, waker_dir]

print("python", sys.version.split()[0], "isolated", sys.flags.isolated, "utf8", sys.flags.utf8_mode)
described = json.load(open(os.path.join(here, "tools", "agent-tools.json"), encoding="utf-8"))
assert os.path.isfile(os.path.join(here, described["python"])) and os.path.isfile(os.path.join(here, described["pythonw"])), described
import ctypes  # noqa: E402,F401
import sqlite3  # noqa: E402,F401
import ssl  # noqa: E402

import mcp.server.fastmcp  # noqa: E402,F401  (the Thoughts tools' framework, with pydantic_core)
import pydantic_core  # noqa: E402,F401
import win32api  # noqa: E402,F401  (pywin32, which mcp needs on Windows)

print("ssl", ssl.OPENSSL_VERSION, "certificates", ssl.create_default_context().cert_store_stats().get("x509_ca", 0))
import agent_notes  # noqa: E402
import thoughts_reply_waker as waker  # noqa: E402

assert agent_notes.WINDOWS and waker.WINDOWS
assert agent_notes.device_account() == "thoughts-windows"
# Nobody signed in here: Credential Manager has no thoughts-server/thoughts-windows (None, never a token).
print("credential manager:", "no device token" if agent_notes.credential_manager_token() is None else "a device token is stored")
print("server.json:", agent_notes.server_settings_path())
running, program = waker.windows_process(os.getpid())
assert running, "this process does not run, says OpenProcess"
print("this process:", program)
assert not waker.windows_process(0x7FFFFFF0)[0]
lock = os.path.join(tempfile.gettempdir(), "agent-tools-check.lock")
with open(lock, "a") as first, open(lock, "a") as second:
    assert waker.lock_file(first, blocking=False) and not waker.lock_file(second, blocking=False), "msvcrt locks"
    waker.unlock_file(first)
    assert waker.lock_file(second, blocking=False)
print("locks: ok")
print("chrome or edge:", agent_notes.chrome_path())

# The MCP server, as Claude Code starts it: an initialize request on its standard input.
python = os.path.join(here, described["python"])
server = subprocess.Popen([python, "-X", "utf8", "-B", os.path.join(here, described["mcp_server"])], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, env={**os.environ, "THOUGHTSCTL": "C:\\nowhere\\thoughtsctl.exe"},
                          creationflags=0x08000000)
request = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
           "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "agent-tools-check", "version": "1"}}}
server.stdin.write((json.dumps(request) + "\n").encode())
server.stdin.flush()
answer = server.stdout.readline()
server.stdin.close()
try:
    server.wait(30)
except subprocess.TimeoutExpired:
    server.kill()
result = json.loads(answer or b"{}").get("result") or {}
assert result.get("serverInfo"), f"the MCP server did not answer: {answer[:300]!r} {server.stderr.read()[-800:]!r}"
print("mcp server:", result["serverInfo"].get("name"), "protocol", result.get("protocolVersion"))

# The waker: its commands run (no sign-in here, so `sessions` lists only this computer's sessions).
home = tempfile.mkdtemp(prefix="waker-check-")
done = subprocess.run([python, "-X", "utf8", "-B", os.path.join(here, described["waker"]), "sessions", "--json"], capture_output=True,
                      env={**os.environ, "REPLY_WAKER_HOME": home}, timeout=120, creationflags=0x08000000)
assert done.returncode == 0, done.stderr[-800:]
print("waker sessions:", done.stdout.decode("utf-8", "replace").strip()[:200])

# The windowless Python, as Thoughts starts the waker: it sees its standard input end (how Thoughts stops it).
pythonw = os.path.join(here, described["pythonw"])
marker, script = os.path.join(home, "stdin.txt"), os.path.join(home, "stdin.py")
with open(script, "w", encoding="utf-8") as handle:
    handle.write("import sys\nopen(sys.argv[1], 'w').write(str(sys.stdin is not None))\n"
                 "sys.stdin.buffer.read() if sys.stdin is not None else None\nopen(sys.argv[1], 'a').write(' ended')\n")
child = subprocess.Popen([pythonw, "-X", "utf8", "-B", script, marker], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)
child.stdin.close()
child.wait(60)
seen = open(marker, encoding="utf-8").read() if os.path.exists(marker) else "nothing"
assert seen == "True ended", f"pythonw and its standard input: {seen}"
print("pythonw: its standard input ends when closed")
print("agent tools: ok")
