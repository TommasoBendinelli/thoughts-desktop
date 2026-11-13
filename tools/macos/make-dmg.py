#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Build dist/Thoughts.dmg: a Thoughts.app that runs on a Mac without ~/CraftRoot.

    python3 tools/macos/make-dmg.py            # configure/build build-dmg, then package
    python3 tools/macos/make-dmg.py --skip-build

Run it again for every new version. Steps:

1. Configure and build a separate build directory (build-dmg by default).
2. Install it with DESTDIR into build-dmg/dmg-stage. The prefix stays ~/CraftRoot,
   so the staged tree mirrors what install-local.py would install, but nothing is
   written to ~/CraftRoot or /Applications; every installed path is checked.
3. Copy the staged thoughts.app, the Qt/KDE plugins Thoughts loads (okularpart and
   the Okular generators from this build, the rest from ~/CraftRoot), Okular's data
   files and every library those need into the bundle.
4. Point every library reference and install name at @executable_path/../Frameworks
   (plugins at ../PlugIns) and replace each RPATH with @executable_path/../Frameworks.
5. Sign every library, framework and plugin with the persistent local identity
   (tools/macos/README.md), then the helpers and the app with sign-app.py. Ad hoc
   signing is never used.
6. Check every Mach-O statically (no /Users path in otool -L or otool -l, every
   reference resolves inside the bundle or to the system), verify the signature,
   make the disk image with hdiutil and verify the app inside the mounted image.

The packaged app is never launched: on this Mac it could reach the live server with
this account's Keychain token. tools/macos/README.md explains a launch test in a
separate macOS user account.

~/CraftRoot above is the KDE Craft root; THOUGHTS_CRAFT_ROOT names another. The
server SDK is --server-sdk, THOUGHTS_SERVER_SDK_ROOT or ../thoughts-server/sdk/cpp.
"""
import argparse
import fnmatch
import functools
import hashlib
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOME = Path.home()
CRAFT = Path(os.environ.get('THOUGHTS_CRAFT_ROOT') or HOME / 'CraftRoot').expanduser().resolve()
IDENTITY_FILE = HOME / 'Library/Application Support/Thoughts/signing/identity.txt'
DEFAULT_SDK = Path(os.environ.get('THOUGHTS_SERVER_SDK_ROOT') or ROOT.parent / 'thoughts-server/sdk/cpp')
BUNDLE_ID = 'org.tbend.thoughts'
FRAMEWORKS_REF = '@executable_path/../Frameworks'
# The oldest macOS the bundled CraftRoot libraries allow (libcrypto's minos is 13.3,
# the rest 13.0); check() fails if any Mach-O in the bundle needs a newer one.
DEFAULT_MACOS_MIN = '13.3'

# Plugins, relative to ~/CraftRoot/plugins (or the staged build's plugins, which win).
# The list is what the running app loads (lsof) plus the ones Qt and KDE load on
# demand: the HTML viewer (webview), speech, media, spell checking and local files.
PLUGINS = [
    'platforms/libqcocoa.dylib',
    'styles/libqmacstyle.dylib',
    'styles/breeze6.so',  # main.cpp sets the Breeze style
    'imageformats/*',
    'iconengines/libqsvgicon.dylib',
    'kiconthemes6/iconengines/KIconEnginePlugin.so',
    'sqldrivers/libqsqlite.dylib',
    'tls/*',
    'networkinformation/libqapplenetworkinformation.dylib',
    'webview/libqtwebview_darwin.dylib',
    'texttospeech/libqtexttospeech_speechdarwin.dylib',
    'multimedia/libdarwinmediaplugin.dylib',
    'kf6/sonnet/sonnet_nsspellchecker.dylib',
    'kf6/kio/kio_file.so',
    'kf6/kio/kio_http.so',
    'kf6/parts/okularpart.dylib',
    'okular_generators/*',
]
# The build's own plugins: always from the staged build, never from ~/CraftRoot,
# whose copies belong to the installed app.
OWN_PLUGINS = ['kf6/parts/okularpart.dylib', 'okular_generators/*']
# Data from the staged build into Contents/Resources, which Craft's Qt searches as a
# generic data location. The kxmlgui .rc files are compiled into the binaries, icons
# come from libKF6BreezeIcons, and MIME types from Qt's built-in database, as in the
# installed app; translations are left out, as the installed app has none either.
DATA = [('share/okular', 'okular'), ('share/icons/hicolor', 'icons/hicolor')]
HELPERS = ['thoughtsctl', 'thoughts-comment-sync', 'thoughts-publish-sources', 'thoughts-watch-build']
# Help › Check for Updates installs a new disk image with this script (shell/CMakeLists.txt).
UPDATE_HELPER = 'Contents/Resources/install-update.sh'
DYLIB_COMMANDS = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB', 'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB'}
MACHO_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


def fail(message):
    raise SystemExit('make-dmg: ' + message)


def run(*args, cwd=None, env=None, quiet=False):
    args = [str(a) for a in args]
    result = subprocess.run(args, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE if quiet else None,
                            stderr=subprocess.STDOUT if quiet else None)
    if result.returncode != 0:
        if quiet:
            sys.stderr.write(result.stdout)
        fail('command failed: ' + ' '.join(args))
    return result.stdout if quiet else ''


def output(*args, cwd=None):
    return subprocess.run([str(a) for a in args], cwd=cwd, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def is_macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACHO_MAGICS


def load_commands(path):
    """[(cmd, argument)] for the dylib, id and rpath load commands of path."""
    commands = []
    current = None
    for line in output('/usr/bin/otool', '-l', path).splitlines():
        text = line.strip()
        if text.startswith('Load command'):
            current = None
        elif text.startswith('cmd '):
            current = text.split()[1]
        elif current and (text.startswith('name ') or text.startswith('path ')):
            commands.append((current, re.sub(r' \(offset \d+\)$', '', text.split(' ', 1)[1])))
            current = None
    return commands


def version_tuple(text):
    return tuple(int(part) for part in text.split('.'))


def minimum_macos(otool_l):
    """The newest minimum macOS that `otool -l` output declares (LC_BUILD_VERSION
    minos, or LC_VERSION_MIN_MACOSX version), or None if it declares none."""
    found = []
    current = None
    for line in otool_l.splitlines():
        text = line.strip()
        if text.startswith('cmd '):
            current = text.split()[1]
        elif (current == 'LC_BUILD_VERSION' and text.startswith('minos ')) or \
                (current == 'LC_VERSION_MIN_MACOSX' and text.startswith('version ')):
            found.append(text.split()[1])
    return max(found, key=version_tuple) if found else None


@functools.lru_cache(maxsize=None)
def source_commands(path):
    """load_commands of a file outside the bundle, which never changes during a run."""
    return load_commands(path)


def inside(path, root):
    path, root = Path(os.path.abspath(path)), Path(os.path.abspath(root))
    return path == root or root in path.parents


# --- build and stage -------------------------------------------------------------

def build_environment():
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join([str(CRAFT / 'bin'), str(CRAFT / 'dev-utils/bin'), env.get('PATH', '')])
    return env


def configure_and_build(build_dir, sdk, macos_min, release_version, update_repository, server_url):
    env = build_environment()
    cmake = CRAFT / 'dev-utils/bin/cmake'
    ccache = shutil.which('ccache', path=env['PATH'])
    # The prefix stays ~/CraftRoot so the build matches the installed one (Qt plugin
    # paths are absolute under it anyway); install() below always uses DESTDIR.
    args = [cmake, '-S', ROOT, '-B', build_dir, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo', f'-DCMAKE_INSTALL_PREFIX={CRAFT}', f'-DCMAKE_PREFIX_PATH={CRAFT}',
            f'-DCMAKE_MAKE_PROGRAM={CRAFT}/dev-utils/bin/ninja', '-DBUILD_TESTING=OFF',
            '-DFORCE_NOT_REQUIRED_DEPENDENCIES=LibSpectre;KF6DocTools', '-DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON',
            f'-DKDE_INSTALL_BUNDLEDIR={CRAFT}/Applications/KDE', f'-DTHOUGHTS_SERVER_SDK_ROOT={sdk}',
            f'-DCMAKE_OSX_DEPLOYMENT_TARGET={macos_min}',
            # Always given, so a reused build directory never keeps an old release's values.
            # With both set, Help › Check for Updates looks for mac-* releases there.
            f'-DTHOUGHTS_RELEASE_VERSION={release_version}', f'-DTHOUGHTS_UPDATE_REPOSITORY={update_repository}',
            # The server a new installation starts with: the one named, else the build's neutral default.
            f'-DTHOUGHTS_DEFAULT_SERVER_URL={server_url}' if server_url else '-UTHOUGHTS_DEFAULT_SERVER_URL']
    if ccache:
        args += [f'-DCMAKE_C_COMPILER_LAUNCHER={ccache}', f'-DCMAKE_CXX_COMPILER_LAUNCHER={ccache}']
    print('Configuring', build_dir, flush=True)
    run(*args, cwd=ROOT, env=env, quiet=True)
    print('Building (this also signs build-dmg/bin/thoughts.app with sign-app.py)…', flush=True)
    run(cmake, '--build', build_dir, '-j8', cwd=ROOT, env=env, quiet=True)


def install_into_stage(build_dir, stage):
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    env = build_environment()
    env['DESTDIR'] = str(stage)
    print('Installing into', stage, flush=True)
    log = run(CRAFT / 'dev-utils/bin/cmake', '--install', build_dir, cwd=ROOT, env=env, quiet=True)
    for line in log.splitlines():
        match = re.match(r'-- (Installing|Up-to-date|Set runtime path of): "?(.*?)"?$', line)
        if match and not inside(match.group(2), stage):
            fail(f'the install wrote outside the staging folder: {match.group(2)}')
    return stage / str(CRAFT).lstrip('/')


# --- bundle ----------------------------------------------------------------------

class Bundler:
    def __init__(self, app, staged_craft):
        self.app = app
        self.contents = app / 'Contents'
        self.frameworks = self.contents / 'Frameworks'
        self.staged_craft = staged_craft
        self.source = {}        # bundle file -> the file it was copied from
        self.copied = {}        # real source path -> bundle file
        self.names = {}         # bundle file name -> real source path
        self.queue = []

    def bundle_ref(self, path):
        return '@executable_path/../' + str(path.relative_to(self.contents))

    def staged(self, path):
        """The staged build's copy of a ~/CraftRoot path, if the build installs one."""
        path = Path(path)
        if inside(path, CRAFT):
            candidate = self.staged_craft / path.relative_to(CRAFT)
            if candidate.exists():
                return candidate
        return path

    def add(self, bundle_file, source):
        self.source[bundle_file] = Path(source)
        if is_macho(bundle_file):
            self.queue.append(bundle_file)

    def copy_plugins(self):
        plugins = self.contents / 'PlugIns'
        for pattern in PLUGINS:
            own = pattern in OWN_PLUGINS
            roots = [self.staged_craft / 'plugins'] if own else [self.staged_craft / 'plugins', CRAFT / 'plugins']
            for root in roots:
                directory = (root / pattern).parent
                if not directory.is_dir():
                    continue
                matches = sorted(p for p in directory.iterdir()
                                 if fnmatch.fnmatch(p.name, Path(pattern).name) and p.is_file() and not p.is_symlink()
                                 and p.suffix in ('.dylib', '.so'))
                if matches:
                    break
            else:
                fail(f'no plugin matches {pattern}' + (' in the staged build' if own else ''))
            for match in matches:
                target = plugins / match.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(match, target)
                target.chmod(0o755)
                self.add(target, match)

    def copy_data(self):
        resources = self.contents / 'Resources'
        for source, target in DATA:
            source = self.staged_craft / source
            if not source.is_dir():
                fail(f'{source} is missing from the staged build')
            shutil.copytree(source, resources / target, symlinks=False, dirs_exist_ok=True)
        # Qt reads qt.conf from Contents/Resources; relative paths start at Contents.
        (resources / 'qt.conf').write_text('[Paths]\nPlugins = PlugIns\nTranslations = Resources/translations\n')

    def resolve(self, reference, binary):
        """The file a load command of binary means, or None for a system library."""
        if reference.startswith(('/usr/lib/', '/System/Library/')):
            return None
        source = self.source[binary]
        if reference.startswith('@rpath/'):
            rest = reference[len('@rpath/'):]
            # The build's own libraries come from the staged install. The helpers in
            # Contents/MacOS are copies of build-tree binaries whose RPATH still lists
            # the build directory, which is never packaged.
            if (self.staged_craft / 'lib' / rest).exists():
                return self.staged_craft / 'lib' / rest
            for kind, rpath in source_commands(source):
                if kind != 'LC_RPATH':
                    continue
                rpath = rpath.replace('@loader_path', str(source.parent)).replace('@executable_path', str(source.parent))
                if inside(rpath, ROOT):
                    continue
                candidate = self.staged(Path(rpath) / rest)
                if candidate.exists():
                    return candidate
            candidate = self.staged(CRAFT / 'lib' / rest)
            if candidate.exists():
                return candidate
            fail(f'{binary.relative_to(self.app)}: cannot resolve {reference}')
        if reference.startswith('@loader_path/'):
            candidate = self.staged(source.parent / reference[len('@loader_path/'):])
        elif reference.startswith('@executable_path/'):
            fail(f'{binary.relative_to(self.app)}: unexpected {reference} before bundling')
        elif reference.startswith('/'):
            candidate = self.staged(reference)
        else:
            fail(f'{binary.relative_to(self.app)}: relative reference {reference}')
        if not inside(candidate, CRAFT) and not inside(candidate, self.staged_craft):
            fail(f'{binary.relative_to(self.app)}: {reference} is neither a system nor a CraftRoot library')
        if not candidate.exists():
            fail(f'{binary.relative_to(self.app)}: {reference} does not exist')
        return candidate

    def bundle_library(self, path):
        """Copies the library at path (and its framework) into Frameworks once; returns its bundle file."""
        real = Path(os.path.realpath(path))
        if real in self.copied:
            return self.copied[real]
        # Frameworks are found from the real file (Versions/A/QtCore), so the copy
        # rewritten below is the binary itself, not the QtCore symlink beside it.
        parts = real.parts
        framework = next((i for i, part in enumerate(parts) if part.endswith('.framework')), None)
        if framework is not None:
            root = Path(*parts[:framework + 1])
            target_root = self.frameworks / root.name
            if not target_root.exists():
                shutil.copytree(root, target_root, symlinks=True,
                                ignore=shutil.ignore_patterns('Headers', '*.prl', '*.dSYM', '_CodeSignature'))
                for link in [p for p in target_root.rglob('*') if p.is_symlink()]:
                    if not inside(link.resolve(), target_root):
                        fail(f'{link} points outside its framework')
            target = target_root / Path(*parts[framework + 1:])
        else:
            install_id = next((arg for kind, arg in source_commands(real) if kind == 'LC_ID_DYLIB'), '')
            name = os.path.basename(install_id) or Path(path).name
            if name in self.names and self.names[name] != real:
                fail(f'two different libraries are both called {name}: {self.names[name]} and {real}')
            self.names[name] = real
            target = self.frameworks / name
            shutil.copyfile(real, target)
            target.chmod(0o755)
        self.copied[real] = target
        self.add(target, real)
        return target

    def fix(self, binary):
        commands = load_commands(binary)
        changes = []
        for kind, reference in commands:
            if kind in DYLIB_COMMANDS:
                dependency = self.resolve(reference, binary)
                if dependency is not None:
                    new = self.bundle_ref(self.bundle_library(dependency))
                    if new != reference:
                        changes += ['-change', reference, new]
        args = []
        if any(kind == 'LC_ID_DYLIB' for kind, _ in commands):
            args += ['-id', self.bundle_ref(binary)]
        args += changes
        self.modify(binary, args)
        rpaths = [arg for kind, arg in commands if kind == 'LC_RPATH']
        for rpath in rpaths:
            self.modify(binary, ['-delete_rpath', rpath])
        if rpaths:
            self.modify(binary, ['-add_rpath', FRAMEWORKS_REF])

    def modify(self, binary, args):
        if not args:
            return
        if not inside(binary, self.app):
            fail(f'refusing to change {binary}: it is outside the bundle being built')
        result = subprocess.run(['/usr/bin/install_name_tool', *args, str(binary)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        problems = [line for line in result.stdout.splitlines() if 'will invalidate the code signature' not in line]
        if result.returncode != 0 or problems:
            fail(f'install_name_tool {" ".join(args)} {binary}:\n' + '\n'.join(problems))

    def run(self):
        for helper in [self.contents / 'MacOS' / 'thoughts'] + [self.contents / 'MacOS' / name for name in HELPERS]:
            if not helper.is_file():
                fail(f'{helper.name} is missing from the staged app')
            self.add(helper, self.staged_bundle_source(helper))
        unexpected = sorted(p.name for p in (self.contents / 'MacOS').iterdir() if p not in self.source)
        if unexpected:
            fail('unexpected files in Contents/MacOS: ' + ', '.join(unexpected))
        self.frameworks.mkdir(exist_ok=True)
        self.copy_plugins()
        self.copy_data()
        done = set()
        while self.queue:
            binary = self.queue.pop(0)
            if binary not in done:
                done.add(binary)
                self.fix(binary)
        return sorted(done)

    def staged_bundle_source(self, helper):
        return self.staged_craft / 'Applications/KDE/thoughts.app/Contents/MacOS' / helper.name


def prepare_info_plist(app, macos_min, commit, release_version):
    path = app / 'Contents/Info.plist'
    with path.open('rb') as stream:
        info = plistlib.load(stream)
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        fail(f'the staged app is not {BUNDLE_ID}')
    info['LSMinimumSystemVersion'] = macos_min
    # Which source a copy was built from (Finder › Get Info does not show it;
    # `defaults read /Applications/Thoughts.app/Contents/Info ThoughtsSourceCommit` does).
    info['ThoughtsSourceCommit'] = commit
    if release_version:
        # Finder › Get Info shows this; updates compare the compiled-in version.
        info['CFBundleShortVersionString'] = release_version
    with path.open('wb') as stream:
        plistlib.dump(info, stream)
    return info


# --- signing and checks ----------------------------------------------------------

def sign(app, identity):
    contents = app / 'Contents'
    nested = sorted(p for p in (contents / 'Frameworks').iterdir() if p.suffix != '.framework' and is_macho(p))
    nested += sorted(p for p in (contents / 'PlugIns').rglob('*') if is_macho(p))
    frameworks = sorted(p for p in (contents / 'Frameworks').iterdir() if p.suffix == '.framework')
    print(f'Signing {len(nested)} libraries and plugins and {len(frameworks)} frameworks…', flush=True)
    for start in range(0, len(nested), 50):
        run('/usr/bin/codesign', '--force', '--sign', identity, '--timestamp=none', *nested[start:start + 50], quiet=True)
    for framework in frameworks:
        run('/usr/bin/codesign', '--force', '--sign', identity, '--timestamp=none', framework, quiet=True)
    # The helpers and the app itself, with the designated requirements Thoughts
    # keeps across builds, then codesign --verify --deep --strict.
    print(run(sys.executable, ROOT / 'tools/macos/sign-app.py', app, quiet=True), end='')


def check(app, macos_min):
    """Static checks; returns the Mach-O files, the codesign output and the newest
    minimum macOS any of them declares. Fails on any problem."""
    contents = app / 'Contents'
    machos = sorted(p for p in app.rglob('*') if is_macho(p))
    problems = []
    newest = ('0', None)
    for binary in machos:
        relative = binary.relative_to(app)
        for tool in ('-L', '-l'):
            text = output('/usr/bin/otool', tool, relative, cwd=app)
            body = text.split('\n', 1)[1] if text.startswith(f'{relative}:') else text
            if '/Users/' in body:
                for line in body.splitlines():
                    if '/Users/' in line:
                        problems.append(f'{relative}: otool {tool}: {line.strip()}')
            if tool == '-l':
                # dyld loads a library built for a newer macOS, which then fails on a
                # missing symbol or crashes; LSMinimumSystemVersion alone does not show it.
                minimum = minimum_macos(body)
                if minimum is None:
                    problems.append(f'{relative}: no minimum macOS (LC_BUILD_VERSION) in otool -l')
                elif version_tuple(minimum) > version_tuple(macos_min):
                    problems.append(f'{relative}: needs macOS {minimum}, newer than --macos-min {macos_min}')
                elif version_tuple(minimum) > version_tuple(newest[0]):
                    newest = (minimum, relative)
        for kind, reference in load_commands(binary):
            if kind == 'LC_RPATH' and reference != FRAMEWORKS_REF:
                problems.append(f'{relative}: RPATH {reference}')
            if kind in DYLIB_COMMANDS | {'LC_ID_DYLIB'}:
                # System libraries live in the dyld shared cache, not as files.
                if reference.startswith(('/usr/lib/', '/System/Library/')):
                    continue
                if not reference.startswith('@executable_path/../'):
                    problems.append(f'{relative}: {kind} {reference}')
                elif not (contents / reference[len('@executable_path/../'):]).exists():
                    problems.append(f'{relative}: {kind} {reference} is not in the bundle')
    strict = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', str(app)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if strict.returncode != 0:
        problems.append('codesign --verify --deep --strict failed:\n' + strict.stdout)
    if problems:
        fail('static checks failed:\n' + '\n'.join(problems[:200]))
    return machos, strict.stdout, newest


def make_dmg(app, output_path, volume_name, notes):
    work = Path(tempfile.mkdtemp(prefix='thoughts-dmg-', dir=app.parent))
    try:
        root = work / 'root'
        root.mkdir()
        run('/usr/bin/ditto', app, root / 'Thoughts.app')
        (root / 'Applications').symlink_to('/Applications')
        shutil.copyfile(notes, root / 'Install Thoughts.md')
        output_path.parent.mkdir(parents=True, exist_ok=True)
        if output_path.exists():
            output_path.unlink()
        print('Creating', output_path, flush=True)
        run('/usr/bin/hdiutil', 'create', '-volname', volume_name, '-srcfolder', root, '-fs', 'HFS+',
            '-format', 'UDZO', '-imagekey', 'zlib-level=9', '-ov', output_path, quiet=True)
        run('/usr/bin/hdiutil', 'verify', output_path, quiet=True)
        # Check the app as the other Mac will see it: from the mounted image.
        mount = work / 'mount'
        mount.mkdir()
        run('/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', mount, output_path, quiet=True)
        try:
            mounted = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', str(mount / 'Thoughts.app')],
                                     text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        finally:
            run('/usr/bin/hdiutil', 'detach', mount, quiet=True)
        if mounted.returncode != 0:
            fail('the app inside the disk image does not verify:\n' + mounted.stdout)
        return mounted.stdout
    finally:
        shutil.rmtree(work, ignore_errors=True)


def verdict(text):
    """codesign --verbose=2 output without its per-file --prepared/--validated lines."""
    return '\n'.join(line for line in text.splitlines() if not line.startswith(('--prepared:', '--validated:')))


def size(path):
    total = path.stat().st_size if path.is_file() else sum(p.lstat().st_size for p in path.rglob('*'))
    return f'{total / 1024 / 1024:.0f} MB'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('--build-dir', default='build-dmg', help='separate build directory (default build-dmg)')
    parser.add_argument('--output', default='dist/Thoughts.dmg', help='disk image to write (default dist/Thoughts.dmg)')
    parser.add_argument('--server-sdk', default=str(DEFAULT_SDK),
                        help='sdk/cpp of a thoughts-server checkout (default: THOUGHTS_SERVER_SDK_ROOT, else ../thoughts-server/sdk/cpp)')
    parser.add_argument('--macos-min', default=DEFAULT_MACOS_MIN, help=f'oldest macOS the app supports (default {DEFAULT_MACOS_MIN})')
    parser.add_argument('--skip-build', action='store_true', help='package the existing build directory as it is')
    parser.add_argument('--release-version', default='', help='THOUGHTS_RELEASE_VERSION, e.g. 2026.11.10.1 (tools/macos/release.py sets it)')
    parser.add_argument('--update-repository', default='', help='THOUGHTS_UPDATE_REPOSITORY whose mac-* releases update this build')
    parser.add_argument('--server-url', default='', help='THOUGHTS_DEFAULT_SERVER_URL, the https:// address a new installation starts with (default: the build\'s neutral one)')
    args = parser.parse_args()

    if sys.platform != 'darwin':
        fail('run this on macOS')
    if not (CRAFT / 'lib').is_dir():
        fail(f'{CRAFT} is missing; the bundle is assembled from it')
    if not IDENTITY_FILE.exists():
        fail('the signing identity is missing; run tools/macos/setup-signing.py once (ad hoc signing is not a fallback)')
    identity = IDENTITY_FILE.read_text().strip()
    if not re.fullmatch(r'[0-9A-F]{40}', identity):
        fail('invalid signing identity fingerprint')
    build_dir = (ROOT / args.build_dir).resolve()
    output_path = (ROOT / args.output).resolve()
    for path in (build_dir, output_path):
        if inside(path, CRAFT) or inside(path, '/Applications'):
            fail(f'{path} must not be inside ~/CraftRoot or /Applications')

    commit = output('git', 'rev-parse', '--short=9', 'HEAD', cwd=ROOT).strip()
    if output('git', 'status', '--porcelain', '--untracked-files=no', cwd=ROOT).strip():
        commit += '-modified'
        print('Warning: the worktree has uncommitted changes; they are packaged too.', flush=True)

    if bool(args.release_version) != bool(args.update_repository):
        fail('--release-version and --update-repository go together')
    if not args.skip_build:
        configure_and_build(build_dir, args.server_sdk, args.macos_min, args.release_version, args.update_repository, args.server_url)
    elif not (build_dir / 'CMakeCache.txt').exists():
        fail(f'{build_dir} is not configured; run without --skip-build')
    staged_craft = install_into_stage(build_dir, build_dir / 'dmg-stage')

    package = build_dir / 'dmg-package'
    if package.exists():
        shutil.rmtree(package)
    package.mkdir()
    app = package / 'Thoughts.app'
    run('/usr/bin/ditto', staged_craft / 'Applications/KDE/thoughts.app', app)
    shutil.rmtree(app / 'Contents/_CodeSignature', ignore_errors=True)
    if not (app / UPDATE_HELPER).is_file():
        fail(f'{UPDATE_HELPER} is missing from the staged app')
    info = prepare_info_plist(app, args.macos_min, commit, args.release_version)

    print('Bundling libraries and plugins…', flush=True)
    fixed = Bundler(app, staged_craft).run()
    print(f'Rewrote {len(fixed)} Mach-O files', flush=True)
    sign(app, identity)
    machos, verification, newest = check(app, args.macos_min)
    version = args.release_version or info.get('CFBundleVersion', '')
    mounted = make_dmg(app, output_path, f'Thoughts {version}', ROOT / 'tools/macos/INSTALL-ON-ANOTHER-MAC.md')
    digest = hashlib.sha256(output_path.read_bytes()).hexdigest()
    (output_path.parent / (output_path.name + '.sha256')).write_text(f'{digest}  {output_path.name}\n')

    report = build_dir / 'dmg-report.txt'
    report.write_text('\n'.join([
        f'Thoughts {version} ({commit}), macOS {args.macos_min} or later',
        f'app: {app} ({size(app)}), {len(machos)} Mach-O files',
        f'dmg: {output_path} ({size(output_path)}), sha256 {digest}',
        'otool -L / otool -l: no /Users path in any Mach-O; every reference is @executable_path/../ or the system',
        f'minimum macOS: every Mach-O allows {args.macos_min}; the newest any needs is {newest[0]} ({newest[1]})',
        'codesign --verify --deep --strict (bundle):', verdict(verification).strip(),
        'codesign --verify --deep --strict (inside the mounted dmg):', verdict(mounted).strip(), '']))
    print(report.read_text(), end='')
    print('Report:', report)


if __name__ == '__main__':
    main()
