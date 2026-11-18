"""Passenger handover on real city railways, including a replay of the stale local coordinates."""
import math
import struct

START = 'city'


def run_railway(g, prefix, open_air=False):
    if not any(m['id'] == 'ragdoll' and m['loaded'] for m in g.call('mods.list')):
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    assert g.on_foot()
    g.sleep(3200)  # Let the car-exit animation release its world-space target.
    player = g.player()
    human, model = int(player['actor'], 16), int(player['frame'], 16)

    def u32(addr):
        return struct.unpack('<I', bytes.fromhex(g.call('mem.read', addr=hex(addr), size=4)['hex']))[0]

    def write(addr, fmt, *values):
        g.call('mem.write', addr=hex(addr), hex=struct.pack('<'+fmt, *values).hex())

    def place_in_carriage(world, local):
        # The frame setter accepts a world point, while C_human keeps its feet in carriage space.
        vt = u32(model)
        write(human+0x24, '3f', *world)
        g.call('native.call', at=hex(u32(vt+4)), conv='stdcall', args=[model, human+0x24], ret='void')
        write(human+0x24, '3f', *local)
        g.call('native.call', at=hex(u32(vt+0x18)), conv='stdcall', args=[model], ret='void')

    def visible():
        return bool(int(g.call('frame', addr=hex(model))['flags'], 16) & 1)

    g.later('state.time', scale=1)
    g.call('state.time', scale=0)
    trains = g.call('world.near', type=8, radius=3000)['actors']
    tram = next(t for t in trains if t['name'].lower().startswith(prefix))
    vehicle = int(tram['actor'], 16)
    # Exercise the trailing carriage's separate frame and collision grid on the elevated train.
    car = 1 if open_air else 0
    if car:
        assert u32(vehicle+0x1a8) >= car
    carriage_frame = u32(u32(vehicle+0xc4) + 4*(car-1)) if car else u32(vehicle+0x68)
    frame = g.call('frame', addr=hex(carriage_frame))
    # Settle on the street first. After restoring the city save the player can still have
    # a car-exit animation rooted at spawn; boarding during that track pulls him back there.
    street = [v + 4 * side + 2 * up for v, side, up in zip(frame['pos'], *frame['axes'][:2])]
    street = g.call('world.ground', at=street)['pos']
    g.call('state.time', scale=1)
    g.call('world.teleport', actor='player', to=street)
    g.sleep(1100)
    settled = g.player()
    assert math.dist(settled['pos'], street) < 2., (settled, street, g.call('human.anim_state'))
    g.call('state.time', scale=0)
    frame = g.call('frame', addr=hex(carriage_frame))
    at = [v + .7 * axis for v, axis in zip(frame['pos'], frame['axes'][1])]
    g.call('world.teleport', actor='player', to=at)
    # Board without waiting for the traffic timetable. The native function parents Tommy and sets his camera.
    write(human+0x9c, 'I', vehicle)
    write(human+0x584, 'i', car)
    platform = '0x571b90' if g.build == '1.2' else '0x492000'
    g.call('native.call', at=platform, conv='thiscall', **{'this':hex(human)}, args=[1], ret='void')
    place_in_carriage(at, (0., .7, 0.))
    g.call('state.time', scale=.35)
    g.sleep(300)
    before = g.player()
    g.check('fixture starts inside the intended carriage', (before.get('vehicle') or {}).get('actor') == tram['actor']
            and math.dist(before['pos_local'], [0, .7, 0]) < 1., (before, at))
    g.call('ragdoll.start', actor=player['actor'])
    g.check('fall returns Tommy to a visible physical body', visible() and not g.player().get('vehicle') and
            any(r['actor'] == player['actor'] for r in g.call('ragdoll.state')['ragdolls']))
    g.call('ragdoll.drop', actor=player['actor'])
    g.call('state.time', scale=1)
    g.sleep(1400)
    rider = g.player()
    g.check('supported passenger remains in the same carriage', (rider.get('vehicle') or {}).get('actor') == tram['actor']
            and u32(human+0x584) == car, rider)
    g.check('railway first-person camera hides Tommy', g.call('camera.game')['mode'] == 17 and not visible())
    if not rider.get('vehicle'):
        return
    # Recreate the real incident while time stands still. Reload removes all mod-side boarding history.
    g.sleep(3000)
    g.call('state.time', scale=0)
    g.call('mod.unload', id='ragdoll')
    wagon = g.call('frame', addr=hex(carriage_frame))

    def world_point(local):
        return [wagon['pos'][i] + sum(local[j]*wagon['axes'][j][i] for j in range(3)) for i in range(3)]

    def ground_between(feet, deck_y):
        top = [feet[0], deck_y, feet[2]]
        while top[1] > feet[1]:
            hit = g.call('world.ray', **{'from': top}, to=feet, what='static')
            if not hit['hit'] or hit['surface'] in (31, 44):
                return None
            if hit['normal'][1] > .5:
                return hit['pos']
            top[1] = hit['pos'][1] - .02

    deck_height = rider['pos_local'][1]
    local = (1.683, -17.804, 2.677)
    if open_air:
        # Pick clear air at the side of the elevated track, away from its beams.
        options = ((x, .0, z) for z in (0., 3., -3., 6., -6., 12., -12., 24., -24.)
                   for x in (2.5, -2.5, 4., -4., 7., -7., 12., -12., 20., -20.))
        local = next(p for p in options if ground_between(world_point(p),
                     world_point((p[0], deck_height, p[2]))[1]) is None)
    world = world_point(local)
    ground = ground_between(world, world_point((local[0], deck_height, local[2]))[1])
    place_in_carriage(world, local)
    write(human+0x40c, 'i', 3)
    write(human+0x418, 'f', -1.)
    g.call('mod.load', id='ragdoll')
    g.check('ascending passenger keeps the native railway link', bool(g.player().get('vehicle')))
    stuck = g.player()
    g.check('fixture has the intended surface crossing and local transform',
            (ground is None) == open_air and math.dist(stuck['pos'], world) < .05, (world, stuck, ground))
    write(human+0x418, 'f', 17.)
    g.call('mod.reload', id='ragdoll')
    free = g.player()
    g.check('broken passenger returns to world space and third person', not free.get('vehicle') and
            g.call('camera.game')['mode'] == 1 and visible(), free)
    if open_air:
        speed = struct.unpack('<f', struct.pack('<I', u32(human+0x418)))[0]
        g.check('open-air exit keeps position and downward speed',
                math.dist(free['pos'], stuck['pos']) < .05 and abs(speed-17.) < .01, (free, speed))
        # Keep the induced high-speed fall out of the next city's restore/cleanup.
        write(human+0x418, 'f', 0.)
        g.call('world.teleport', actor='player', to=street)
    else:
        g.check('places feet on the crossed surface', ground is not None and
                math.dist(free['pos'], ground) < .1, (free['pos'], ground))
        g.check('surface recovery cancels underground fall speed and preserves health',
                u32(human+0x418) == 0 and free['health'] == stuck['health'] and free['alive'])
        if prefix == 'metro':
            g.call('state.time', scale=1)
            g.sleep(450)
            landed = g.player()
            g.check('landing preserves health and world position', ground is not None and landed['alive'] and
                    landed['health'] >= stuck['health']-.01 and landed['pos'][1] >= ground[1]-.2, (landed, ground))

    if prefix == 'salina':
        g.call('state.time', scale=0)
        wagon = g.call('frame', addr=hex(carriage_frame))
        beside = world_point((2.7, 1., 2.677))
        street = g.call('world.ground', at=beside)
        assert street['hit']
        g.call('world.teleport', actor='player', to=street['pos'])
        g.call('state.time', scale=.1)
        g.sleep(300)
        g.call('ragdoll.start', actor=player['actor'])
        g.check('ragdoll starts beside the streetcar', g.player()['alive'] and
                any(r['actor'] == player['actor'] for r in g.call('ragdoll.state')['ragdolls']))
        g.call('ragdoll.drop', actor=player['actor'])
        outside = g.player()
        g.check('getting up beside the doorway stays in world space', not outside.get('vehicle') and
                not g.call('ragdoll.state')['ragdolls'] and visible() and g.call('camera.game')['mode'] != 17,
                outside)


def test_tram(g):
    run_railway(g, 'salina')


def test_train(g):
    run_railway(g, 'metro')


def test_train_open_air(g):
    run_railway(g, 'metro', open_air=True)
