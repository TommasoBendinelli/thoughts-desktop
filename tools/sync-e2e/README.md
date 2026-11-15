# End-to-end Sync harness

Runs Thoughts' real Sync against a disposable Fedora test instance (a second
thoughts-server on port 8443 of the server machine, see `deploy/fedora/TESTING.md`
in thoughts-server). Every stage resets that instance first; production is never
contacted.

```bash
tools/sync-e2e/run_all.py                      # every stage
tools/sync-e2e/run_all.py --stage 4            # one stage
```

To test a server change, stage it on Fedora (`thoughts-test-environment update`) first.

| Stage | What it checks |
| --- | --- |
| 1 | A Project published from another device builds its PDF and Project reading. |
| 2 | A rebuild republishes the Project reading with the new PDF. |
| 3 | A Project with an unbuilt document and an imported PDF pinned to an older copy still republishes its reading on every rebuild. |
| 4 | The desktop's own Sync engine (`autotests/syncdriver.cpp`) against another device: new Project, merged edits, conflicts on two files resolved as Keep this computer's version and Keep the server's version, a saved hand merge, added/renamed/deleted files on both sides, the rebuilt PDF and reading reaching the Mac, and repeated Syncs that change nothing. |

The Mac side is `build-thoughts/bin/thoughts-sync-driver`, built by
`run_all.py`. It runs with a private `HOME`, refuses to start if any Thoughts
state location or the docs root is outside it, and refuses any server other
than the test instance. Android is the desktop build's native publisher
(`build-thoughts/bin/thoughts-publish-sources`, also built by `run_all.py`)
working on its own copy of the files.

Requirements: `THOUGHTS_E2E_TEST_HOST` set to the SSH destination (`user@host`)
of the server machine, whose account can run the test instance's password-free
`thoughts-test-environment` actions, and a desktop build in `build-thoughts`
(or `THOUGHTS_DESKTOP_BUILD`). `NINJA` names the ninja to build with when it is
not on `PATH`.
