# xdiff from git v2.56.0

Sync merges source files three ways as the Thoughts server does with `git merge-file`, so both produce the same bytes. Instead of running an installed Git, the publisher compiles Git's own merge code. These files come from git **v2.56.0** (tag `v2.56.0`, the git-2.56.0 release tarball, `xdiff/`). Fedora runs git 2.56.0.

License: the xdiff library is LGPL-2.1-or-later (`COPYING.LGPL-2.1`, Git's `LGPL-2.1`), which may be combined with the rest of Thoughts (GPL-2.0-or-later). The two glue files `git-compat-util.h` and `thoughts-xdiff.c` follow Git's own GPL-2.0-only sources and are GPL-2.0-only (`LICENSES/GPL-2.0-only.txt`); Okular already contains GPL-2.0-only files, so the combined program is distributed under version 2 of the GPL as before.

## Files

- Unmodified from git v2.56.0: `xdiff.h`, `xdiffi.c`, `xdiffi.h`, `xemit.c`, `xemit.h`, `xhistogram.c`, `xinclude.h`, `xmacros.h`, `xmerge.c`, `xpatience.c`, `xprepare.c`, `xprepare.h`, `xtypes.h`, `xutils.c`, `xutils.h`.
- Ours:
  - `git-compat-util.h`: what xdiff takes from Git's own `git-compat-util.h`: `BUG()`, `signed_add_overflows`, `UNUSED`, `xmalloc`/`xcalloc`/`xrealloc`, Git's locale-free ctype (`isalnum` decides how conflicts are coalesced), and a `regex_t` stub (merges never use `diff -I`). It builds with clang and gcc on macOS and Linux, and with clang-cl on Windows.
  - `thoughts-xdiff.c` / `thoughts-xdiff.h`: `thoughts_xdiff_merge_file()`, the `xdl_merge()` call of `builtin/merge-file.c` for `git merge-file -p -L … -L … -L …`. The settings are Git's defaults: level `XDL_MERGE_ZEALOUS_ALNUM`, style 0 (no `merge.conflictstyle`), favor 0, the Myers diff (`xpp.flags = 0`) and conflict markers of 7 characters. It also does merge-file's binary check: a NUL in the first 8000 bytes, or more than 1023 MiB. It returns the exit status Git would return (the conflict count, capped at 127).

## Local changes

None to Git's files. Where Git dies (`BUG()`, out of memory), `thoughts-xdiff.c` jumps back out of the merge (`longjmp`, through C frames only) and reports Git's message instead. What xdiff allocated up to that point leaks.

## Updating

Copy `xdiff/` of the new Git release over these files. Compare `builtin/merge-file.c` and `xdiff-interface.c` (`buffer_is_binary`, `MAX_XDIFF_SIZE`) with `thoughts-xdiff.c`, and add anything new that xdiff needs from `git-compat-util.h`. Then run `tools/publisher/tests/merge_differential.py` against the Git that Fedora runs.
