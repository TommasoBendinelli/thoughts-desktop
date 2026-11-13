#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Install a stopped Thoughts app using matching generated installers and its persistent signer."""
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
craft = Path.home() / 'CraftRoot'
# Build first: the installers below only copy what build-thoughts already
# contains, so a skipped build would reinstall stale code. Thoughts may stay
# open while it builds; a failed build changes nothing.
cache = (root / 'build-thoughts/CMakeCache.txt').read_text()
cmake = next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('CMAKE_COMMAND:INTERNAL='))
print('Building Thoughts…', flush=True)
if subprocess.run([cmake, '--build', str(root / 'build-thoughts'), '-j8'], cwd=root, stdout=subprocess.DEVNULL).returncode != 0:
    raise SystemExit('The build failed; nothing was installed. Run cmake --build build-thoughts to see why.')
processes = subprocess.check_output(['/bin/ps', '-axo', 'pid=,comm='], text=True)
if any(line.strip().endswith('/Contents/MacOS/thoughts') for line in processes.splitlines()):
    raise SystemExit('Quit Thoughts normally before installing; no running files were changed.')
# The .noindex suffix keeps Spotlight, and so Launchpad, from listing every
# backed-up app bundle as another Thoughts.app.
backups = Path.home() / 'Library/Application Support/Thoughts/install-backups.noindex'
KEEP_BACKUPS = 2
backup = backups / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-before-install')
backup.mkdir(parents=True)
paths = [Path('/Applications/Thoughts.app'), craft / 'Applications/KDE/thoughts.app',
         craft / 'lib/libOkular6Core.4.0.0.dylib', craft / 'plugins/kf6/parts/okularpart.dylib',
         craft / 'plugins/okular_generators/okularGenerator_poppler.so', craft / 'bin/thoughtsctl', craft / 'bin/thoughts-comment-sync']
for path in paths:
    if path.exists():
        target = backup / str(path).lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['/usr/bin/ditto', str(path), str(target)], check=True)
log = backup / 'install.log'
try:
    with log.open('w') as output:
        for name in ['cmake_install.cmake', 'generators/poppler/cmake_install.cmake', 'shell/cmake_install.cmake']:
            subprocess.run([str(craft / 'dev-utils/bin/cmake'), '-DCMAKE_INSTALL_LOCAL_ONLY=TRUE',
                            '-DCMAKE_INSTALL_COMPONENT=okular', '-P', str(root / 'build-thoughts' / name)],
                           cwd=root, stdout=output, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['/usr/bin/ditto', str(craft / 'Applications/KDE/thoughts.app'), '/Applications/Thoughts.app'], check=True, stdout=output, stderr=subprocess.STDOUT)
        subprocess.run(['python3', str(root / 'tools/macos/sign-app.py'), '/Applications/Thoughts.app'], check=True, stdout=output, stderr=subprocess.STDOUT)
except BaseException:
    for path in paths:
        original = backup / str(path).lstrip('/')
        if original.exists():
            subprocess.run(['/usr/bin/ditto', str(original), str(path)], check=True)
    raise
# Keep only the most recent backups, including this one.
for old in sorted((path for path in backups.iterdir() if path.is_dir()), reverse=True)[KEEP_BACKUPS:]:
    shutil.rmtree(old)
print('Installed and signed /Applications/Thoughts.app')
print('Backup and installation log:', backup)
