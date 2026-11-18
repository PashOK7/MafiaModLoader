"""Check the opening 15 seconds of Clark's Motel in both stopped labs with a 04-03 save."""
import argparse
import time
from concurrent.futures import ThreadPoolExecutor

from mml_game import Game


DOUBLES = {'FMVSam2', 'FMVPaul_raneny', 'FMVPaul2', 'FMVTom2'}


def check(build, profile, save):
    g = Game(build, profile)
    started = False
    try:
        if g.tool('game_status')['running']:
            raise RuntimeError(f'{build} lab already running')
        g.tool('game_launch', profile=profile, go='load', save=save, wait_s=60)
        started = True
        if g.call('game')['mission_name'] != 'mise04-motorest':
            raise AssertionError(f'{build}: select the opening 04-03 save')
        if not any(m['id'] == 'ragdoll' and m['loaded'] for m in g.call('mods.list')):
            g.call('mod.load', id='ragdoll')
        deadline = time.monotonic() + 15
        seen = set()
        while time.monotonic() < deadline:
            humans = g.call('humans.list')['humans']
            actors = {h['actor']: h for h in humans if h['model'] in DOUBLES}
            seen.update(h['model'] for h in actors.values())
            state = g.call('ragdoll.state')
            if not state['enabled']:
                raise AssertionError(f'{build}: ragdolls disabled')
            for body in state['ragdolls']:
                if body['actor'] in actors:
                    raise AssertionError(f"{build}: unexpected ragdoll {actors[body['actor']]['model']}")
            if any(not h['alive'] for h in actors.values()):
                raise AssertionError(f'{build}: cinematic double died')
            time.sleep(.12)
        if seen != DOUBLES:
            raise AssertionError(f'{build}: missing cinematic doubles {DOUBLES-seen}')
        print(f'{build}: four living cinematic doubles, zero ragdolls in 15 seconds', flush=True)
    finally:
        try:
            if started:
                g.tool('game_close')
        finally:
            g.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=int, default=0)
    parser.add_argument('--save', type=int, default=60)
    args = parser.parse_args()
    with ThreadPoolExecutor(2) as pool:
        jobs = [pool.submit(check, b, args.profile, args.save) for b in ('1.2', '1.0')]
        for job in jobs:
            job.result()
