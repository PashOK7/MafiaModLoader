"""The ragdoll's toolkit (ROADMAP item 32): a person's hit given by the game's own Hit, taken before the game, and the
bullets meeting a body a mod moved (a corpse posed away from where the game thinks it lies), both builds."""
import math
import struct

from mml_game import Skip, dist


def bones(g, actor):
    return {b['name']: b['world_pos'] for b in g.call('human.skeleton', actor=actor)['bones']}


def fire_at(g, point, direction=(0, -.8, .6), distance=1.8):
    """Queue a real game bullet with a measured trajectory and no weapon spread or camera aiming error."""
    build = g.tool('game_status')['ping']['build']
    mission_ptr, new_shoot = {'1.0': (0x65115C, 0x5E3DF0), '1.2': (0x63788C, 0x5A84A0)}[build]

    def pointer(address):
        data = g.call('mem.read', addr=hex(address), size=4)['hex']
        return struct.unpack('<I', bytes.fromhex(data))[0]

    game = pointer(pointer(mission_ptr) + 0x24)
    shooter = g.call('human.skeleton', actor='player')['actor']
    origin = [p - d * distance for p, d in zip(point, direction)]
    seq = g.latest()
    result = g.call('native.call', at=hex(new_shoot), conv='thiscall', ret='int', **{'this': hex(game)},
                    args=[shooter, *[{'float': v} for v in origin], *[{'float': d * 40} for d in direction],
                          {'float': 50}, 0, 0, 1])
    assert result['stack']['ok'], result
    g.sleep(120)
    return [e for e in g.call('events.since', seq=seq, limit=25)['events'] if e['type'] in ('SHOT', 'HUMAN_HIT', 'BULLET_HIT')]


def test_world_bullet_event_with_capsules(g):
    ground = g.call('world.ground', at=[-1981, 0, 75])['height']
    actor = g.spawn_human('Paulie', [-1977, ground + .02, 75])
    g.call('hits.shape', actor=actor, capsules=[[-1977, ground + .5, 75, -1977, ground + 1.5, 75, .3]])
    events = fire_at(g, [-1981, ground, 75], direction=(0, -1, 0))
    impacts = [e['data'] for e in events if e['type'] == 'BULLET_HIT']
    g.check('capsule ray still reports the native world impact exactly once',
            len(impacts) == 1 and dist(impacts[0]['pos'], [-1981, ground, 75]) < .1, events)


def test_living_bullet_with_corpse_capsules(g):
    ground = g.call('world.ground', at=[-1981, 0, 75])['height']
    corpse = g.spawn_human('Paulie', [-1977, ground + .02, 75])
    g.call('human.hit', actor=corpse, type=0, damage=1000, dir=[0, 0, 0])
    g.call('hits.shape', actor=corpse, capsules=[[-1977, ground + .5, 75, -1977, ground + 1.5, 75, .3]])
    g.later('hits.shape', actor=corpse, clear=True)
    living = g.spawn_human('civil15', [-1981, ground + .02, 75])
    g.sleep(300)
    health = g.call('human.health', actor=living)['health']
    events = fire_at(g, bones(g, living)['back3'])
    hits = [e['data'] for e in events if e['type'] == 'HUMAN_HIT' and e['data']['actor'] == living]
    g.check('native bullet damages a living target while an unrelated corpse has capsules',
            len(hits) == 1 and hits[0]['was_alive'] and not hits[0].get('shape') and not hits[0].get('taken') and
            g.call('human.health', actor=living)['health'] < health, events)


def test_ragdoll_limb_bullets(g):
    """An actual bullet into either palm reaches its matching forearm, even close to the ground."""
    mod = next((m for m in g.call('mods.list') if m['id'] == 'ragdoll'), None)
    if mod is None:
        raise Skip('ragdoll port is not installed')
    if not mod['loaded']:
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    ground = g.call('world.ground', at=[-1981, 0, 75])['height']
    for name, sign, shape, part in (('r_hand', 1, 15, 6), ('l_hand', -1, 16, 8)):
        actor = g.spawn_human('civil15', [-1981, ground + .02, 75])
        g.call('human.anim', actor=actor, play='smrt pad end A')
        g.sleep(250)
        g.call('human.hit', actor=actor, type=0, dir=[0, 0, 0], damage=1000)
        g.sleep(2300)
        hand = next(b for b in g.call('human.skeleton', actor=actor)['bones'] if b['name'] == name)
        target = [p + axis * sign * .075 for p, axis in zip(hand['world_pos'], hand['x_axis'])]
        events = fire_at(g, target)
        hits = [e['data'] for e in events if e['type'] == 'HUMAN_HIT' and e['data']['actor'] == actor]
        g.check(name + ': actual projectile reaches exactly the palm capsule',
                len(hits) == 1 and hits[0].get('shape') == shape and hits[0].get('taken') and
                dist(hits[0]['pos'], target) < .085, (events, g.call('hits.state')['bullets']))
        contact = g.call('ragdoll.state')['last_bullet']
        g.check(name + ': local impulse reaches the corresponding physical forearm',
                contact and contact['actor'] == actor and contact['shape'] == shape and
                contact['part'] == part and contact['local_dv'] > 0, contact)
        g.call('world.remove', actor=actor)
        g.made.remove(actor)


def test_knock_down(g):
    """human.hit with a car's hit (type 8) faster than 3 m/s and no damage: the game knocks him down, alive."""
    g.on_foot()
    paulie = g.spawn_human('Paulie', 'player', ahead=4)
    g.sleep(500)
    r = g.call('human.hit', actor=paulie, type=8, dir=[5, 0, 0], damage=0)
    g.check('the game took the hit, he lives', r.get('was_alive') and r.get('alive'), r)
    s = g.call('human.anim_state', actor=paulie)
    g.check('he falls (a "smrt pad" animation plays)', 'smrt pad' in str(s.get('name', '')).lower(), s)


def test_hit_taken(g):
    """hits.take: a lethal hit taken before the game leaves him alive and is told as taken; without it, he dies."""
    paulie = g.spawn_human('Paulie', 'player', ahead=4)
    g.sleep(500)
    seq = g.latest()
    g.call('hits.take', actor=paulie, ms=20000)
    g.later('hits.take', clear=True)
    r = g.call('human.hit', actor=paulie, type=0, dir=[0, 0, -1], damage=1000, part=1)
    g.check('taken: the lethal hit leaves him alive', r.get('alive'), r)
    ev = g.wait('HUMAN_HIT', {'actor': paulie}, since=seq, timeout=5)
    g.check('HUMAN_HIT tells it taken', ev is not None and ev['data'].get('taken'), ev)
    g.call('hits.take', clear=True)
    r = g.call('human.hit', actor=paulie, type=0, dir=[0, 0, -1], damage=1000, part=1)
    g.check('not taken: the same hit kills him', r.get('was_alive') and not r.get('alive'), r)


def test_bullets_on_moved_body(g):
    """A corpse's pelvis posed 2.5 m from where he fell (the game's own collision stays there): his bones go with it
    (the frame position marked changed, as LS3D marks it), and the player's bullets, given his body as capsules,
    strike him there (HUMAN_HIT with `shape`)."""
    g.on_foot()
    here = g.pos('player')
    # on the pavement 7 m behind the car he got out of (a player pressed against a car does not fire)
    paulie = g.spawn_human('Paulie', [here[0] + 3, here[1], here[2] + 6.5])
    g.sleep(300)
    g.call('human.hit', actor=paulie, type=0, dir=[0, 0, -1], damage=1000, part=1)
    g.sleep(4000)   # the death animation played out
    before = bones(g, paulie)
    local = [b for b in g.call('human.skeleton', actor=paulie)['bones'] if b['name'] == 'pelvis'][0]['pos']
    g.call('human.pose', actor=paulie, bones={'pelvis': {'pos': [local[0] + 2.5, local[1], local[2]]}}, ms=60000)
    g.later('human.pose', actor=paulie, clear=True)
    g.sleep(300)
    after = bones(g, paulie)
    moved = {n: dist(before[n], after[n]) for n in ('pelvis', 'back3', 'neck', 'r_foot', 'l_shin')}
    g.check('the posed pelvis takes his back, neck and legs with it (2.5 m)', all(abs(d - 2.5) < 0.3 for d in moved.values()), moved)
    pairs = [('pelvis', 'back3', 0.22), ('back3', 'neck', 0.2), ('r_thigh', 'r_shin', 0.12), ('r_shin', 'r_foot', 0.1),
             ('l_thigh', 'l_shin', 0.12), ('l_shin', 'l_foot', 0.1)]
    caps = [after[a] + after[b] + [r] for a, b, r in pairs]
    g.call('hits.shape', actor=paulie, capsules=caps)
    g.later('hits.shape', actor=paulie, clear=True)
    p = after['pelvis']
    g.tool('teleport', to=[p[0] + 3.5, here[1], p[2]], dir=[-1, 0, 0])
    g.sleep(500)
    # the crosshair onto the pelvis: the camera's ray pitched step by step (Aim Y: down positive)
    for _ in range(40):
        ray = g.call('world.ray', **{'from': 'camera'})
        eye, to = ray['from'], ray['to']
        d = [to[i] - eye[i] for i in range(3)]
        now = math.atan2(-d[1], math.hypot(d[0], d[2]))
        want = math.atan2(eye[1] - p[1], math.hypot(p[0] - eye[0], p[2] - eye[2]))
        err = want - now
        if abs(err) < math.radians(0.6):
            break
        g.tool('input', steps=[{'action': 'Aim Y', 'value': max(-0.004, min(0.004, err * 0.15)), 'ms': 30}, {'wait_ms': 60}])
    g.check('the crosshair on the moved body', abs(err) < math.radians(1.5), math.degrees(err))
    seq = g.latest()
    rounds = lambda: [i['loaded'] for i in g.call('human.items')['items'] if i['where'] == 'hand'][0]
    before_fire, shapes = rounds(), g.call('hits.state')['shapes']
    g.tool('input', steps=[{'action': 'Fire', 'ms': 200}, {'wait_ms': 800}, {'action': 'Fire', 'ms': 500}, {'wait_ms': 600}])
    g.check('the player fired (rounds gone)', rounds() < before_fire, (before_fire, rounds(), g.player()))
    ev = g.wait('HUMAN_HIT', {'actor': paulie}, since=seq, timeout=5)
    st = g.call('hits.state')['bullets']
    # A dead C_human's own collision follows his bones (seen in both builds): the game's test finds the moved body
    # itself, nearer than or as near as his capsules, and wins; the capsules strike where it does not (a pedestrian's
    # corpse, out of the collision grid since pedHit's death).
    g.check('a bullet strikes the moved body (by the game\'s test or his capsules)',
            ev is not None and dist(ev['data']['pos'], p) < 1.2, (ev, st, shapes))
    g.check('the bullets\' stretches were tested against his capsules', st.get('stretches', 0) > 0, st)
