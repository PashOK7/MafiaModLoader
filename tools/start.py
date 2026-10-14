"""start.py: launch the game straight into a profile and a freeride, a save, a new game or the main menu, the logos skipped.

    python tools/start.py freeride --profile 4        # by the profile's number (savegame\\mafia004.sav)
    python tools/start.py freeride --profile "Name"   # or by its name
    python tools/start.py menu --profile 4            # stop in the main menu
    python tools/start.py freeride --logos            # keep the logo videos
    python tools/start.py freeride --profile 4 --police 0   # a city without police patrols (also --traffic, --pedestrians)
    python tools/start.py freeride --profile 4 --kind city-night --place "Down Town"   # the night city
    python tools/start.py load --profile 4 --save "Old Prison"   # a chapter from the profile's saves (or its number)
    python tools/start.py new --profile 4             # a new game (also: extreme)

It writes <game>\\mml\\start.json (the loader reads and deletes it at the game's start: host/start.cpp) and starts
Game.exe from the game folder of mml.config.json. Without a profile the profile menu is left to the player. The MCP
tool game_launch does the same for agents (go, profile, skip_logos).
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description='Launch Mafia into a profile and the freeride or the main menu.')
    ap.add_argument('go', choices=['freeride', 'menu', 'load', 'new', 'extreme'])
    ap.add_argument('--profile', help='the profile: its number or its name')
    ap.add_argument('--kind', choices=['city-small', 'city-night', 'countryside-night', 'city', 'countryside'],
                    help='the freeride\'s kind (default city-small)')
    ap.add_argument('--place', help='where a freeride other than city-small starts: the spawn menu\'s row (1..) or a part of its text')
    ap.add_argument('--save', help='go load: the save\'s number or a part of its name')
    ap.add_argument('--logos', action='store_true', help='play the logo videos')
    for name, what in (('police', 'police patrols'), ('traffic', 'traffic'), ('pedestrians', 'pedestrians')):
        ap.add_argument('--' + name, type=float, metavar='0..1', help='the freeride\'s %s density (the menu\'s slider)' % what)
    a = ap.parse_args()
    game = os.environ.get('MML_GAME_DIR') or json.load(open(os.path.join(ROOT, 'mml.config.json')))['game_dir']
    tasks = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    if any(line.lower().startswith('"game.exe"') for line in tasks.splitlines()):
        sys.exit('Game.exe is running: close it first')
    req = {'skip_logos': not a.logos, 'go': a.go}
    if a.profile is not None:
        req['profile'] = int(a.profile) if a.profile.isdigit() else a.profile
    for name in ('police', 'traffic', 'pedestrians', 'kind'):
        if getattr(a, name) is not None:
            req[name] = getattr(a, name)
    for name in ('place', 'save'):
        v = getattr(a, name)
        if v is not None:
            req[name] = int(v) if v.isdigit() else v
    os.makedirs(os.path.join(game, 'mml'), exist_ok=True)
    with open(os.path.join(game, 'mml', 'start.json'), 'w') as f:
        json.dump(req, f)
    subprocess.Popen([os.path.join(game, 'Game.exe')], cwd=game, creationflags=0x8 | 0x200)  # detached
    print('started:', json.dumps(req))


if __name__ == '__main__':
    main()
