"""deps.py: download the pinned third-party archives, check their SHA-256, unpack them into third_party/_deps/.

    python tools/deps.py            # everything missing (but the optional ones)
    python tools/deps.py jolt       # only these (the build asks for its libraries so: CMakeLists.txt)
    python tools/deps.py ghidra     # an optional one: Ghidra and its Python bridge (tools/ghidra_cli.py)
    python tools/deps.py --force    # again, from scratch

Only standard library. A hash that does not match stops everything: the archive is not used.
"""
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'third_party', '_deps')

PINNED = {
    # Ultimate ASI Loader v9.7.4 (x86, with PDB), MIT; the hash is the one GitHub publishes for the asset
    'ual': ('https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip',
            '952cebfc30d525afc2bdbaca954329d405ded3aa688a83027354dae14dfd5c5f'),
    # Jolt Physics v5.6.0, commit e77f175595e64cb44218cc9d9d56fc365ad0e36a (2026-07-11), MIT: the rigid bodies for
    # mods, built for x86 by CMakeLists.txt (target Jolt). The archive's 1250 files were compared with a clone of
    # the tag (mafia-ragdoll's, whose 577 unit tests pass on the 32-bit build): all identical.
    'jolt': ('https://github.com/jrouwe/JoltPhysics/archive/e77f175595e64cb44218cc9d9d56fc365ad0e36a.zip',
             'ff13202d21a7a09efea72534a0a041cd4971cb9ac66b53262275881387cc6871'),
    # Ghidra 12.1.4 (2026-09-21), Apache 2.0: the reverse-engineering projects of both builds behind tools/ghidra_cli.py
    # and the MCP server's ghidra_* tools. 570 MB (about 900 unpacked); needs a JDK 21 or later. The hash is the one
    # the release publishes (its notes and the asset's digest). Its PyGhidra and JPype wheels are in the archive: the
    # venv below is made from them, nothing else downloaded.
    'ghidra': ('https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/'
               'ghidra_12.1.4_PUBLIC_20260921.zip',
               'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'),
}
# fetched only when named: not every build needs them
OPTIONAL = {'ghidra'}


def fetch(name, url, sha, force):
    dest = os.path.join(DEPS, name)
    mark = os.path.join(dest, '.pinned')   # the hash it was unpacked from: a changed pin or a broken unpack is fetched anew
    if not force and os.path.exists(mark) and open(mark).read().strip() == sha:
        print('%-7s present' % name)
        return
    os.makedirs(DEPS, exist_ok=True)
    archive = os.path.join(DEPS, name + '.zip')
    print('%-7s %s' % (name, url))
    with urllib.request.urlopen(url) as r, open(archive, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    h = hashlib.sha256()
    with open(archive, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    got = h.hexdigest()
    if got != sha:
        os.remove(archive)
        sys.exit('%s: SHA-256 %s, expected %s: not used' % (name, got, sha))
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    os.remove(archive)
    # a GitHub source archive holds one top folder: its content goes up one level
    entries = os.listdir(dest)
    if len(entries) == 1 and os.path.isdir(os.path.join(dest, entries[0])):
        top = os.path.join(dest, entries[0])
        for e in os.listdir(top):
            shutil.move(os.path.join(top, e), dest)
        os.rmdir(top)
    with open(mark, 'w') as f:
        f.write(sha + '\n')
    print('%-7s ok (sha256 %s)' % (name, sha[:16]))


def pyghidra_venv(force):
    """third_party/_deps/pyghidra: a venv of the Python running this, with Ghidra's own PyGhidra and JPype wheels
    (no index: only the archive's). tools/ghidra_cli.py runs its server in it."""
    venv = os.path.join(DEPS, 'pyghidra')
    dist = os.path.join(DEPS, 'ghidra', 'Ghidra', 'Features', 'PyGhidra', 'pypkg', 'dist')
    py = os.path.join(venv, 'Scripts', 'python.exe')
    mark = os.path.join(venv, '.pinned')
    want = PINNED['ghidra'][1] + ' ' + sys.version.split()[0]
    if not force and os.path.exists(mark) and open(mark).read().strip() == want:
        print('%-7s present' % 'pyghidra')
        return
    import subprocess
    shutil.rmtree(venv, ignore_errors=True)
    subprocess.run([sys.executable, '-m', 'venv', venv], check=True)
    subprocess.run([py, '-m', 'pip', 'install', '--no-index', '--find-links', dist, 'pyghidra'], check=True)
    with open(mark, 'w') as f:
        f.write(want + '\n')
    print('%-7s ok (%s)' % ('pyghidra', venv))


def main():
    force = '--force' in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith('--')] or [n for n in PINNED if n not in OPTIONAL]
    unknown = [n for n in names if n not in PINNED]
    if unknown:
        sys.exit('not pinned: %s (pinned: %s)' % (', '.join(unknown), ', '.join(PINNED)))
    for name in names:
        url, sha = PINNED[name]
        fetch(name, url, sha, force)
        if name == 'ghidra':
            pyghidra_venv(force)


if __name__ == '__main__':
    main()
