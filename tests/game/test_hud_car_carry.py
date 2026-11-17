"""A carried native car keeps its radar binding; destruction must still invalidate it."""
import json
from pathlib import Path
import time
from test_last_car_lifetime import hud, pointer

START = 'city'


def test_change_mission_keeps_car(g):
    g.call('joy.hotplug', on=False)
    g.call('joy.drop')
    for mod in g.call('mods.list'):
        if mod['loaded'] and mod['id'] != 'core':
            g.call('mod.unload', id=mod['id'])
    g.call('state.time', scale=1)
    g.call('input.release')
    player = g.call('player')
    car = player['vehicle']['actor']
    base, off, _ = hud(g)
    before = dict(player=player, car=g.call('car'), radar=pointer(g, base+off))
    assert before['radar'] == int(car, 16), before
    seq = g.latest()
    request = g.tool('script', code='change_mission "freeitaly", "TomyhoAuto", 0',
                     owner='player', no_wait=True)
    event = g.wait('MISSION_LOADED', since=seq, timeout=30)
    assert event, 'native CHANGE_MISSION did not finish'
    after = dict(player=g.call('player'), radar=pointer(g, base+off), game=g.call('game'))
    g.check('native CHANGE_MISSION keeps the same player actor', after['player']['actor']==player['actor'])
    g.check('native CHANGE_MISSION keeps the same vehicle actor', after['player'].get('vehicle', {}).get('actor')==car)
    g.check('carried driver car remains bound to radar', after['radar']==int(car, 16))
    g.check('carried scene remains queryable', g.call('world.near', type=4, radius=10)['count']>0)
    folder = Path(__file__).resolve().parents[2]/'build/radar-carry-20261117'
    folder.mkdir(parents=True, exist_ok=True)
    with (folder/f'{g.build}-{time.time_ns()}.json').open('x', encoding='utf-8') as f:
        json.dump(dict(before=before, after=after, request=request, event=event, failures=g.fails), f)
