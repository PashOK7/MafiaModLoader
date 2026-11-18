"""Live settings and moving car geometry in the ragdoll port."""
import re


def load_ragdoll(g):
    if not any(m['id'] == 'ragdoll' and m['loaded'] for m in g.call('mods.list')):
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')


def test_release_input(g):
    load_ragdoll(g)
    assert g.on_foot()
    cfg = g.call('config.get', mod='ragdoll')
    g.later('config.set', mod='ragdoll', key='player_hotkey', value=cfg['changes'].get('player_hotkey'))
    # An override left behind by an older installation must not reserve either key.
    g.call('config.set', mod='ragdoll', key='player_hotkey', value='F12')
    g.tool('input', steps=[{'key': 'J', 'ms': 100}, {'key': 'F12', 'ms': 100}])
    actor = g.player()['actor']
    g.check('release leaves manual ragdoll keys free',
            all(b['actor'] != actor for b in g.call('ragdoll.state')['ragdolls']))
    g.check('ragdoll registers no hotkeys', not any(h.get('mod') == 'ragdoll'
            for h in g.call('input.state')['hotkeys']))


def test_live_settings(g):
    load_ragdoll(g)
    original = g.call('config.get', mod='ragdoll')['changes']
    for key in ('enabled', 'bullets'):
        g.later('config.set', mod='ragdoll', key=key, value=original.get(key))
    g.call('config.set', mod='ragdoll', key='enabled', value=True)
    g.call('config.set', mod='ragdoll', key='bullets', value=False)
    p = g.pos('player')
    actor = g.spawn_human('civil15', [p[0]+5, p[1]+.1, p[2]])
    g.call('human.hit', actor=actor, type=0, damage=1000, dir=[0,0,0])
    g.sleep(500)
    g.later('state.time', scale=1)
    g.call('state.time', scale=0)

    def body():
        return next(b for b in g.call('ragdoll.state')['ragdolls'] if b['actor'] == actor)

    before = body()
    g.call('human.hit', actor=actor, type=0, damage=80, dir=[1,0,0], pos=before['chest'])
    after = body()
    g.check('disabled bullet push leaves frozen body velocity unchanged',
            after['age'] == before['age'] and after['fastest_part'] == before['fastest_part'], after)
    g.call('state.time', scale=1)
    living = g.spawn_human('Paulie', [p[0]+8, p[1]+.1, p[2]])
    g.call('ragdoll.start', actor=living)
    g.call('config.set', mod='ragdoll', key='enabled', value=False)
    g.sleep(150)
    s = g.call('ragdoll.state')
    g.check('switching off drops bodies and stops the solver', not s['world'] and not s['ragdolls'], s)
    g.check('living puppet survives disabling physics', g.call('human.health', actor=living)['health'] > 0)
    g.call('config.set', mod='ragdoll', key='enabled', value=True)
    g.sleep(250)
    s = g.call('ragdoll.state')
    g.check('enabling resumes corpse adoption without reclaiming the living actor',
            s['world'] and actor in {r['actor'] for r in s['ragdolls']} and
            living not in {r['actor'] for r in s['ragdolls']}, s)


def test_car_door(g):
    load_ragdoll(g)
    car = g.player()['vehicle']['actor']
    def mesh():
        items = g.call('actor.meshes', actor=car, hidden_root=True)['list']
        return next(i for i in items if i['visual'].upper().endswith('.DOOR0'))
    def hulls():
        lines = str(g.call('ragdoll.vehicle', car=car)).splitlines()
        return {int(i): float(x) for line in lines if ' wheel' not in line
                for i, x in re.findall(r'part\s+(\d+): x\s*(-?\d+\.\d+)', line)}
    def door(percent):
        g.tool('script', code=f'car_setdooropen 0,0,{percent}\nend', act={'0': car})
        g.sleep(350)
    door(0)
    closed, shapes = mesh(), hulls()
    try:
        door(100)
        opened, updated = mesh(), hulls()
        g.check('native driver door actually opens', max(abs(a-b) for edge in ('lo', 'hi')
                    for a,b in zip(closed[edge], opened[edge])) > .35, opened)
        g.check('local collision hull follows opening door',
                any(k in updated and abs(x-updated[k]) > .35 for k,x in shapes.items()), updated)
    finally:
        door(0)
    final = hulls()
    g.check('closing door restores original hull bounds',
            bool(shapes) and shapes.keys() == final.keys() and all(abs(final[k]-x)<.03 for k,x in shapes.items()), final)
