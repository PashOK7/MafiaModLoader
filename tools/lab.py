"""lab.py: the game folder used for development (game_dir in mml.config.json): which mode it is in, switching modes,
installing the build.

    python tools/lab.py status
    python tools/lab.py mode loader     # MafiaModLoader.asi in, the ragdoll's host out (kept)
    python tools/lab.py mode ragdoll    # back as it was: the ragdoll's host IJoy.dll, the loader out (kept)
    python tools/lab.py install         # build/x86/out -> the game folder (the loader, the mods)

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


def game_dir():
    if os.environ.get('MML_GAME_DIR'):
        return os.environ['MML_GAME_DIR']
    cfg = os.path.join(ROOT, 'mml.config.json')
    if not os.path.exists(cfg):
        sys.exit('no mml.config.json: copy mml.config.example.json and set game_dir')
    return json.load(open(cfg))['game_dir']


def running(game):
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    exe = os.path.join(game, 'Game.exe').lower()
    # tasklist has no paths: a Game.exe running is enough to refuse (the check is conservative)
    return any(line.lower().startswith('"game.exe"') for line in out.splitlines()) and os.path.exists(exe)


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None


def status(game):
    ijoy, orig = os.path.join(game, 'IJoy.dll'), os.path.join(game, 'IJoy_orig.dll')
    host_in = os.path.exists(orig) and os.path.exists(ijoy) and not filecmp.cmp(ijoy, orig, shallow=False)
    asi_in = os.path.exists(os.path.join(game, 'MafiaModLoader.asi'))
    ual = sha(os.path.join(game, 'dinput8.dll'))
    return {
        'game_dir': game,
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
        dst = os.path.join(game, 'mml', 'mods', mod)
        os.makedirs(dst, exist_ok=True)
        for f in os.listdir(os.path.join(mods, mod)):
            if f.lower().endswith(('.ilk', '.exp', '.lib')):
                continue
            shutil.copy2(os.path.join(mods, mod, f), os.path.join(dst, f))
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
