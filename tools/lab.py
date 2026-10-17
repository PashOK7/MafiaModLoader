"""lab.py: the game folder used for development (game_dir in mml.config.json): which build and mode it is in,
switching labs and modes, installing the build.

    python tools/lab.py status
    python tools/lab.py use 1.0         # game_dir to the 1.0 lab (mml.config.json's "labs"); `use 1.2` back
    python tools/lab.py mode loader     # MafiaModLoader.asi in, the ragdoll's host out (kept)
    python tools/lab.py mode ragdoll    # back as it was: the ragdoll's host IJoy.dll, the loader out (kept)
    python tools/lab.py install         # build/x86/out -> the game folder (the loader, the mods)

The labs are the user's folders, one a build, listed in mml.config.json: {"game_dir": ..., "labs": {"1.2": ...,
"1.0": ...}}. `use` points game_dir at one of them (the MCP server and every tool read game_dir at each call); which
build a piece of work is tested on: AGENTS.md "Builds". The build of a folder is told by its LS3DF.dll.

The lab may be shared with mafia-ragdoll, whose developer host is the game's IJoy.dll (the original renamed to
IJoy_orig.dll). Both hook the same functions of the game, so only one is in at a time; nothing is deleted:
what is taken out is kept in <game>\\mml_lab\\. Saves are copied to <game>-backup\\savegame-<date> before a switch.
Nothing here runs while the game is running.
"""
import datetime
import filecmp
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'x86', 'out')
UAL = os.path.join(ROOT, 'third_party', '_deps', 'ual', 'dinput8.dll')


CFG = os.path.join(ROOT, 'mml.config.json')
# LS3DF.dll's link time (its PE header) -> the build (game_build.h knows these two)
BUILDS = {0x3EF851B2: '1.2', 0x3D4BEDFE: '1.0'}


def config():
    if not os.path.exists(CFG):
        sys.exit('no mml.config.json: copy mml.config.example.json and set game_dir')
    with open(CFG) as f:
        return json.load(f)


def game_dir():
    if os.environ.get('MML_GAME_DIR'):
        return os.environ['MML_GAME_DIR']
    return config()['game_dir']


def build_of(game):
    """'1.2', '1.0', 'unknown' or 'missing', by LS3DF.dll's link time."""
    path = os.path.join(game, 'LS3DF.dll')
    if not os.path.exists(path):
        return 'missing'
    with open(path, 'rb') as f:
        head = f.read(4096)
    pe = int.from_bytes(head[0x3C:0x40], 'little')
    return BUILDS.get(int.from_bytes(head[pe + 8:pe + 12], 'little'), 'unknown')


def same_dir(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def use(build):
    cfg = config()
    labs = cfg.get('labs') or {}
    if build not in labs:
        sys.exit('mml.config.json has no lab for %s: "labs": {"1.2": "<folder>", "1.0": "<folder>"} (the user\'s '
                 'folders; never search the disk for one)' % build)
    game = labs[build]
    found = build_of(game)
    if found != build:
        sys.exit('%s is %s, not %s (its LS3DF.dll)' % (game, found, build))
    # (only the default game changes: a game running in either lab goes on, reached by the MCP tools' `game`)
    if not same_dir(cfg['game_dir'], game):
        cfg['game_dir'] = game
        with open(CFG, 'w') as f:
            json.dump(cfg, f, indent=2)
            f.write('\n')
    st = status(game)
    print(json.dumps(st, indent=2))
    if st['mode'] != 'loader':
        print('next: python tools/lab.py mode loader')


def process_paths():
    """Every running process's executable (the ones this user may ask: the game runs as the user)."""
    import ctypes
    from ctypes import wintypes
    psapi, k32 = ctypes.WinDLL('psapi'), ctypes.WinDLL('kernel32')
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    k32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    k32.CloseHandle.argtypes = (wintypes.HANDLE,)
    pids = (wintypes.DWORD * 8192)()
    got = wintypes.DWORD()
    psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
    paths = []
    for pid in pids[:got.value // ctypes.sizeof(wintypes.DWORD)]:
        h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        buf, n = ctypes.create_unicode_buffer(1024), wintypes.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            paths.append(buf.value)
        k32.CloseHandle(h)
    return paths


def running(game):
    """A game of this folder runs (its Game.exe, the folder compared through junctions): the player's own game in
    another folder, or the other lab's, does not count (the loader lets games of different folders run together)."""
    exe = os.path.normcase(os.path.realpath(os.path.join(game, 'Game.exe')))
    return any(os.path.normcase(os.path.realpath(p)) == exe for p in process_paths())


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None


def status(game):
    ijoy, orig = os.path.join(game, 'IJoy.dll'), os.path.join(game, 'IJoy_orig.dll')
    host_in = os.path.exists(orig) and os.path.exists(ijoy) and not filecmp.cmp(ijoy, orig, shallow=False)
    asi_in = os.path.exists(os.path.join(game, 'MafiaModLoader.asi'))
    ual = sha(os.path.join(game, 'dinput8.dll'))
    labs = {}
    if os.path.exists(CFG):
        labs = {b: {'dir': d, 'in_use': same_dir(d, game)} for b, d in (config().get('labs') or {}).items()}
    return {
        'game_dir': game,
        'build': build_of(game),
        'labs': labs,
        'mode': 'loader' if asi_in and not host_in else 'ragdoll' if host_in and not asi_in else
                'BOTH (they fight over the same hooks)' if host_in else 'vanilla',
        'ragdoll_host_in': host_in,
        'loader_in': asi_in,
        'ual': 'pinned v9.7.4' if ual and ual == sha(UAL) else ('other' if ual else 'missing'),
        'game_running': running(game),
    }


def backup_saves(game):
    src = os.path.join(game, 'savegame')
    if not os.path.isdir(src):
        return
    dest = game.rstrip('\\/') + '-backup\\savegame-' + datetime.date.today().isoformat()
    if not os.path.exists(dest):
        shutil.copytree(src, dest)
        print('saves copied to', dest)


def keep(game, rel):
    os.makedirs(os.path.join(game, 'mml_lab'), exist_ok=True)
    return os.path.join(game, 'mml_lab', rel)


def install(game):
    if not os.path.exists(os.path.join(OUT, 'MafiaModLoader.asi')):
        sys.exit('nothing built: cmake --build --preset x86-release')
    for f in ('MafiaModLoader.asi', 'MafiaModLoader.pdb'):
        if os.path.exists(os.path.join(OUT, f)):
            shutil.copy2(os.path.join(OUT, f), os.path.join(game, f))
    mods = os.path.join(OUT, 'mml', 'mods')
    for mod in os.listdir(mods):
        src = os.path.join(mods, mod)
        for folder, _, files in os.walk(src):   # with its subfolders (files\: the game files it ships)
            dst = os.path.join(game, 'mml', 'mods', mod, os.path.relpath(folder, src))
            os.makedirs(dst, exist_ok=True)
            for f in files:
                if f.lower().endswith(('.ilk', '.exp', '.lib', '.mmltmp')):
                    continue
                shutil.copy2(os.path.join(folder, f), os.path.join(dst, f))
    print('installed: MafiaModLoader.asi and mods', ', '.join(os.listdir(mods)))


def mode(game, which):
    st = status(game)
    if st['game_running']:
        sys.exit('the game is running: close it first')
    backup_saves(game)
    ijoy, orig = os.path.join(game, 'IJoy.dll'), os.path.join(game, 'IJoy_orig.dll')
    asi = os.path.join(game, 'MafiaModLoader.asi')
    if which == 'loader':
        if st['ragdoll_host_in']:
            shutil.copy2(ijoy, keep(game, 'IJoy.ragdoll-host.dll'))
            shutil.copy2(orig, ijoy)   # the game's own joystick DLL back in place
            print('ragdoll host kept in mml_lab\\IJoy.ragdoll-host.dll; IJoy.dll is the game\'s own again')
        if st['ual'] != 'pinned v9.7.4':
            if not os.path.exists(UAL):
                sys.exit('no pinned Ultimate ASI Loader: python tools/deps.py')
            d8 = os.path.join(game, 'dinput8.dll')
            if os.path.exists(d8):
                shutil.copy2(d8, keep(game, 'dinput8.prev.dll'))
            shutil.copy2(UAL, d8)
            print('Ultimate ASI Loader v9.7.4 in as dinput8.dll (the previous kept in mml_lab)')
        install(game)
    elif which == 'ragdoll':
        if os.path.exists(asi):
            shutil.move(asi, keep(game, 'MafiaModLoader.asi.off'))
            print('loader out (kept in mml_lab\\MafiaModLoader.asi.off)')
        host = keep(game, 'IJoy.ragdoll-host.dll')
        if os.path.exists(host):
            shutil.copy2(host, ijoy)
            print('ragdoll host back as IJoy.dll')
    else:
        sys.exit('mode loader | ragdoll')
    print(json.dumps(status(game), indent=2))


def main():
    game = game_dir()
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    if cmd == 'status':
        print(json.dumps(status(game), indent=2))
    elif cmd == 'use' and len(sys.argv) > 2:
        use(sys.argv[2])
    elif cmd == 'mode' and len(sys.argv) > 2:
        mode(game, sys.argv[2])
    elif cmd == 'install':
        if status(game)['game_running']:
            sys.exit('the game is running (Windows keeps the loader locked): close it, or use the MCP tool `deploy` for mods')
        install(game)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
