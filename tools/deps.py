"""deps.py: download the pinned third-party archives, check their SHA-256, unpack them into third_party/_deps/.

    python tools/deps.py            # everything missing
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
}


def fetch(name, url, sha, force):
    dest = os.path.join(DEPS, name)
    if os.path.isdir(dest) and not force:
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
    print('%-7s ok (sha256 %s)' % (name, sha[:16]))


def main():
    force = '--force' in sys.argv
    for name, (url, sha) in PINNED.items():
        fetch(name, url, sha, force)


if __name__ == '__main__':
    main()
