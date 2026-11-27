# The Windows installer's Python for the Thoughts tools

`agent_tools.py` (see its docstring) brings the Python that runs the Thoughts tools (`tools/mcp`) and the reply waker (`tools/waker`) on a Windows PC: the CPython embeddable package for Windows amd64 from python.org and the win_amd64 wheels of `requirements-win.txt` from PyPI, every file pinned by its SHA-256 in `lock.json`. `tools/windows/release.py` runs `fetch` (with a network) and `stage` (without) on the builder.

To change what is pinned (another Python, another `mcp`), on the builder:

```sh
python3 -m pip download --only-binary=:all: --platform win_amd64 --python-version 3.13 --implementation cp \
    --abi cp313 --abi abi3 --abi none -d wheels -r tools/windows/agent-tools/requirements-win.txt
python3 -I tools/windows/agent-tools/agent_tools.py lock wheels --python-version 3.13.17
```

`lock` takes the SHA-256 of the embeddable package from python.org's own release record and, for each wheel, the URL PyPI gives for exactly that file's SHA-256. pip resolves with the builder's platform markers, so a dependency only Windows has (pywin32 for `mcp`) is named in `requirements-win.txt` itself. Keep `mcp` the same as in `tools/mcp/requirements.txt` (`test_agent_tools.py` checks it).
