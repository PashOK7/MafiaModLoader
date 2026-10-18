"""run.py: the tests in the real game (tests/game/test_*.py), in the labs of mml.config.json, each lab's game driven
by its own MCP server, the labs at the same time. Pass or fail; nothing is sent to the user's own game.

    python tests/game/run.py                                  every case, every lab (the games launched as needed)
    python tests/game/run.py orders --game 1.0                the cases whose name holds "orders", in 1.0
    python tests/game/run.py --profile 1.2=4 --profile 1.0=Run   the profiles the freeride starts from
    python tests/game/run.py --reuse                          a running freeride used as it is (faster, not the same start)
    python tests/game/run.py --list                           the cases

A case is a function `test_<name>(g)` of a module tests/game/test_<topic>.py, `g` a mml_game.Game: it drives the game
by its tools and says what it found with g.check(name, cond, detail). A case starts in the city's small freeride
without police, pedestrians or traffic, the player in his car at its start (the same place in both builds): the games are launched anew
(unless --reuse), that moment kept as a restore point and loaded before each case; what a case spawns it lists in
g.made (spawn_human does), removed after it. `BUILDS = ("1.2",)` in a module keeps its cases to those builds.
The profile a lab starts from: --profile, else mml.config.json's "test_profiles" ({"1.2": 4, "1.0": "Run"}): one
that has opened the freeride's settings once (another ignores the police slider).
"""
import argparse
import glob
import importlib.util
import json
import os
import sys
import threading
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
START = 'tests_game_start'   # the restore point (the lab's mml\restore, the profile's saves untouched)
sys.path.insert(0, HERE)
import mml_game  # noqa: E402


def load_cases():
    cases = []
    for path in sorted(glob.glob(os.path.join(HERE, 'test_*.py'))):
        name = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        builds = getattr(mod, 'BUILDS', ('1.2', '1.0'))
        for attr in dir(mod):
            if attr.startswith('test_') and callable(getattr(mod, attr)):
                cases.append((name[len('test_'):] + '.' + attr[len('test_'):], getattr(mod, attr), builds))
    return cases


def run_lab(build, profile, cases, reuse, results):
    g = None
    try:
        g = mml_game.Game(build, profile)
        g.case = 'start'
        g.start_freeride(fresh=not reuse)
        g.tool('restore', save=START)   # the freeride's start, whole: each case begins there
        for i, (name, fn, _) in enumerate(cases):
            g.case = name
            g.log('case %s' % name)
            if i:
                g.tool('restore', load=START)
            try:
                fn(g)
            except mml_game.Skip as e:
                g.log('  skip  %s: %s' % (name, e))
            except Exception as e:   # a tool's error, a broken case: the case fails, the next one runs
                g.fails.append('%s: %s' % (name, e))
                g.log('  FAIL  %s: %s\n%s' % (name, e, traceback.format_exc()))
            finally:
                g.cleanup()
        results[build] = (g.passes, g.fails)
    except Exception as e:
        results[build] = (g.passes if g else 0, (g.fails if g else []) + ['%s: %s' % (build, e)])
        print('[%s] FAIL  %s' % (build, e), flush=True)
    finally:
        if g:
            g.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('query', nargs='*', help='only the cases whose name holds one of these')
    ap.add_argument('--game', action='append', help='a lab\'s build (default every lab of mml.config.json)')
    ap.add_argument('--profile', action='append', default=[], help='BUILD=PROFILE (its number or name)')
    ap.add_argument('--reuse', action='store_true', help='use a running freeride as it is (default: launch anew)')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()

    cases = [c for c in load_cases() if not a.query or any(q in c[0] for q in a.query)]
    if a.list:
        for name, _, builds in cases:
            print('%-40s %s' % (name, ', '.join(builds)))
        return 0
    with open(os.path.join(mml_game.ROOT, 'mml.config.json'), encoding='utf-8') as f:
        cfg = json.load(f)
    labs = a.game or sorted(cfg.get('labs', {}), reverse=True)
    profiles = dict(cfg.get('test_profiles', {}))
    for p in a.profile:
        build, _, value = p.partition('=')
        profiles[build] = int(value) if value.isdigit() else value
    if not cases:
        print('no case matches')
        return 1

    results, threads = {}, []
    for build in labs:
        mine = [c for c in cases if build in c[2]]
        t = threading.Thread(target=run_lab, args=(build, profiles.get(build), mine, a.reuse, results))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    fails = [f for b in labs for f in ['%s %s' % (b, x) for x in results.get(b, (0, ['no result']))[1]]]
    for b in labs:
        passes, fl = results.get(b, (0, ['no result']))
        print('%s: %d passed, %d failed' % (b, passes, len(fl)))
    for f in fails:
        print('  FAIL ' + f)
    print('ALL PASSED' if not fails else '%d FAILED' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
