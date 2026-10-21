"""The ragdoll's toolkit (ROADMAP item 32): a person's hit given by the game's own Hit, taken before the game, and the
bullets meeting a body a mod moved (a corpse posed away from where the game thinks it lies), both builds."""
import math

from mml_game import dist


def bones(g, actor):
    return {b['name']: b['world_pos'] for b in g.call('human.skeleton', actor=actor)['bones']}


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
