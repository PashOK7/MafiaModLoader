"""deps.py: download the pinned third-party archives, check their SHA-256, unpack them into third_party/_deps/.

    python tools/deps.py            # everything missing
    python tools/deps.py jolt       # only these (the build asks for its libraries so: CMakeLists.txt)
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
    # LuaJIT, branch v2.1 at commit 634daf2453559fd5e0ec282f3b7aedaa09e9f344 (2026-10-13), MIT
    'luajit': ('https://github.com/LuaJIT/LuaJIT/archive/634daf2453559fd5e0ec282f3b7aedaa09e9f344.zip',
               '48f9fabc6c58d610044df1b460083fbf5842f642e3a7c367545990f94da4d002'),
    # Jolt Physics v5.6.0, commit e77f175595e64cb44218cc9d9d56fc365ad0e36a (2026-07-11), MIT: the rigid bodies for
    # mods, built for x86 by CMakeLists.txt (target Jolt). The archive's 1250 files were compared with a clone of
    # the tag (mafia-ragdoll's, whose 577 unit tests pass on the 32-bit build): all identical.
    'jolt': ('https://github.com/jrouwe/JoltPhysics/archive/e77f175595e64cb44218cc9d9d56fc365ad0e36a.zip',
             'ff13202d21a7a09efea72534a0a041cd4971cb9ac66b53262275881387cc6871'),
}


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
        shutil.copyfileobj(r, f)
    got = hashlib.sha256(open(archive, 'rb').read()).hexdigest()
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


def main():
    force = '--force' in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith('--')] or list(PINNED)
    unknown = [n for n in names if n not in PINNED]
    if unknown:
        sys.exit('not pinned: %s (pinned: %s)' % (', '.join(unknown), ', '.join(PINNED)))
    for name in names:
        url, sha = PINNED[name]
        fetch(name, url, sha, force)


if __name__ == '__main__':
    main()
