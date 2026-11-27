# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""agent_tools.py with made-up packages in temporary folders: never the network."""
import hashlib
import io
import json
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import agent_tools  # noqa: E402

SOURCE = Path(__file__).resolve().parents[3]


def archive(path: Path, files: dict) -> Path:
    with zipfile.ZipFile(path, "w") as out:
        for name, data in files.items():
            out.writestr(name, data)
    return path


def entry(path: Path, url: str) -> dict:
    return {"file": path.name, "url": url + path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


@pytest.fixture
def cache(tmp_path):
    folder = tmp_path / "cache"
    folder.mkdir()
    embed = archive(folder / "python-3.13.17-embed-amd64.zip",
                    {"python.exe": b"MZ", "pythonw.exe": b"MZ", "python313.dll": b"MZ", "python313.zip": b"PK", "python313._pth": "python313.zip\n.\n#import site\n"})
    pure = archive(folder / "mcp-1.30.0-py3-none-any.whl", {"mcp/__init__.py": "", "mcp-1.30.0.dist-info/METADATA": "Name: mcp\n",
                                                             "mcp/__pycache__/x.pyc": b"cached"})
    native = archive(folder / "pywin32-312-cp313-cp313-win_amd64.whl", {
        "win32/win32api.pyd": b"MZ", "pywin32.pth": "win32\nimport pywin32_bootstrap\n", "pythonwin/win32ui.pyd": b"MZ", "PyWin32.chm": b"help",
        "win32/Demos/demo.py": "", "pywin32-312.data/scripts/pywin32_postinstall.py": "", "pywin32-312.data/platlib/pywin32_system32/pythoncom313.dll": b"MZ"})
    lock = {"python": {"version": "3.13.17", **entry(embed, "https://www.python.org/ftp/python/3.13.17/")},
            "wheels": [{"name": "mcp", "version": "1.30.0", **entry(pure, "https://files.pythonhosted.org/packages/aa/bb/")},
                       {"name": "pywin32", "version": "312", **entry(native, "https://files.pythonhosted.org/packages/cc/dd/")}]}
    agent_tools.check_lock(lock)
    return folder, lock


def test_the_lock_allows_only_pinned_files_from_python_org_and_pypi(cache):
    _, lock = cache
    for broken in (
        {**lock, "python": {**lock["python"], "url": "https://example-mirror.test/python-3.13.17-embed-amd64.zip"}},
        {**lock, "wheels": [{**lock["wheels"][0], "url": "http://files.pythonhosted.org/packages/aa/bb/mcp-1.30.0-py3-none-any.whl"}]},
        {**lock, "wheels": [{**lock["wheels"][0], "url": "https://files.pythonhosted.org/packages/aa/bb/other.whl"}]},
        {**lock, "wheels": [{**lock["wheels"][0], "sha256": "abc"}]},
        {**lock, "wheels": [{**lock["wheels"][0], "file": "x-1-cp313-cp313-manylinux_x86_64.whl",
                             "url": "https://files.pythonhosted.org/packages/x-1-cp313-cp313-manylinux_x86_64.whl"}]},
        {**lock, "python": {**lock["python"], "version": "latest"}},
        {**lock, "wheels": []},
    ):
        with pytest.raises(SystemExit):
            agent_tools.check_lock(broken)


def test_the_repositorys_lock_is_valid():
    lock = agent_tools.read_lock()
    assert lock["python"]["file"] == f"python-{lock['python']['version']}-embed-amd64.zip"
    names = {wheel["name"].replace("_", "-").lower() for wheel in lock["wheels"]}
    assert {"mcp", "pydantic-core", "pywin32"} <= names
    pinned = (Path(agent_tools.__file__).parents[3] / "tools/mcp/requirements.txt").read_text()
    mcp = next(wheel for wheel in lock["wheels"] if wheel["name"] == "mcp")
    assert f"mcp=={mcp['version']}" in pinned  # the same mcp as the tools' requirements
    assert len(agent_tools.tools_id(lock)) == 12


def test_fetch_keeps_good_files_and_refuses_a_changed_one(cache, tmp_path):
    folder, lock = cache
    payloads = {item["url"]: (folder / item["file"]).read_bytes() for item in [lock["python"], *lock["wheels"]]}
    fetched = []

    def opener(url, timeout=None):
        fetched.append(url)
        return io.BytesIO(payloads[url])

    target = tmp_path / "fetched"
    assert len(agent_tools.fetch(target, lock, opener)) == 3
    assert len(fetched) == 3
    agent_tools.fetch(target, lock, opener)
    assert len(fetched) == 3  # all there already, with the pinned hashes
    (target / lock["wheels"][0]["file"]).write_bytes(b"changed")
    payloads[lock["wheels"][0]["url"]] = b"a different file"
    with pytest.raises(SystemExit):
        agent_tools.fetch(target, lock, opener)
    assert not (target / lock["wheels"][0]["file"]).exists() or (target / lock["wheels"][0]["file"]).read_bytes() == b"changed"
    assert not list(target.glob("*.part"))


def test_stage_builds_the_python_and_the_tools(cache, tmp_path):
    folder, lock = cache
    out = tmp_path / "runtime"
    described = agent_tools.stage(folder, SOURCE, out, lock)
    python = out / f"python-{agent_tools.tools_id(lock)}"
    assert described == {"python": f"{python.name}/python.exe", "pythonw": f"{python.name}/pythonw.exe", "mcp_server": "tools/mcp/server.py",
                         "waker": "tools/waker/thoughts_reply_waker.py", "python_version": "3.13.17"}
    assert json.loads((out / "tools/agent-tools.json").read_text()) == described
    # site-packages and site (for pywin32's .pth); isolated from the PC's own Python.
    assert (python / "python313._pth").read_text() == "python313.zip\n.\nLib\\site-packages\nimport site\n"
    site = python / "Lib/site-packages"
    assert (site / "mcp/__init__.py").is_file() and (site / "mcp-1.30.0.dist-info/METADATA").is_file()
    assert (site / "win32/win32api.pyd").is_file() and (site / "pywin32.pth").is_file()
    assert (site / "pywin32_system32/pythoncom313.dll").is_file()  # .data/platlib goes to site-packages
    assert not (site / "pywin32-312.data").exists()  # its scripts are not needed
    for pruned in ("pythonwin", "PyWin32.chm", "win32/Demos", "mcp/__pycache__"):
        assert not (site / pruned).exists(), pruned
    for tool in ("tools/mcp/server.py", "tools/mcp/agent_notes.py", "tools/mcp/conversation_identity.py", "tools/waker/thoughts_reply_waker.py"):
        assert (out / tool).read_bytes() == (SOURCE / tool).read_bytes()
    assert {path.stat().st_mtime for path in out.rglob("*")} == {agent_tools.FIXED_TIME}
    # Again: the same folder, made afresh.
    (python / "stray.txt").write_text("left from before")
    agent_tools.stage(folder, SOURCE, out, lock)
    assert not (python / "stray.txt").exists()


def test_stage_refuses_files_that_would_land_outside(cache, tmp_path):
    folder, lock = cache
    bad = archive(folder / "evil-1-py3-none-any.whl", {"../../outside.py": "x"})
    lock = {**lock, "wheels": [*lock["wheels"], {"name": "evil", "version": "1", **entry(bad, "https://files.pythonhosted.org/packages/ee/")}]}
    with pytest.raises(SystemExit):
        agent_tools.stage(folder, SOURCE, tmp_path / "runtime", lock)
    assert not (tmp_path / "outside.py").exists()


def test_stage_refuses_a_cache_that_is_not_the_lock(cache, tmp_path):
    folder, lock = cache
    (folder / lock["wheels"][0]["file"]).write_bytes(b"something else")
    with pytest.raises(SystemExit):
        agent_tools.stage(folder, SOURCE, tmp_path / "runtime", lock)
