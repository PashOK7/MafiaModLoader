"""The native HUD's borrowed car must not survive the actual car's destruction.

Use native removal, not world.remove (which already clears its own spawned cars).
The stale pointer used to make world_near call a matrix writer on recycled memory.
CHANGE_MISSION may preserve actors; test_hud_car_carry covers that distinct lifetime.
"""
import struct
import time


def pointer(g, at):
    return struct.unpack('<I', bytes.fromhex(g.call('mem.read', addr=hex(at), size=4)['hex']))[0]


def hud(g):
    # RadarSetPlayerCar: matched native setter bodies, both assign exactly one field.
    return {'1.2': (0x6bf980, 0x44a8, 0x5fa500),
            '1.0': (0x658330, 0x94a0, 0x54c620)}[g.build]


def set_car(g, car):
    base, _, setter = hud(g)
    result = g.call('native.call', at=hex(setter), conv='thiscall',
                    **{'this': hex(base)}, args=[car], ret='ptr')
    assert result['stack']['ok'], result


def test_native_car_destruction(g):
    base, off, _ = hud(g)
    previous = pointer(g, base + off)
    car = g.tool('spawn', car='Bolt-Thrower', at='player', ahead=15)['actor']
    other = g.tool('spawn', car='Bolt-Thrower', at='player', ahead=25)['actor']
    g.made.extend([car, other])
    mission = int(g.call('game')['mission'], 16)
    game = pointer(g, mission + 0x24)  # game::kMissionGame, shared by both builds
    remove = {'1.2': 0x5a79a0, '1.0': 0x5e3400}[g.build]

    def remove_native(actor):
        seq = g.latest()
        result = g.call('native.call', at=hex(remove), conv='thiscall',
                        **{'this': hex(game)}, args=[int(actor, 16)], ret='void')
        assert result['stack']['ok'], result
        event = g.wait('ACTOR_DESTROYED', {'actor': actor}, since=seq, timeout=10)
        assert event, 'native removal did not destroy the fixture actor'
        g.made.remove(actor)

    try:
        set_car(g, int(car, 16))
        remove_native(other)
        g.check('destroying another actor preserves the live last car',
                pointer(g, base + off) == int(car, 16))
        remove_native(car)
        g.check('native destruction clears the HUD car', pointer(g, base + off) == 0)
        # Clear a failed pre-fix fixture before asking world_near to avoid touching freed storage.
        if pointer(g, base + off):
            set_car(g, 0)
        near = g.call('world.near', type=2, radius=10)
        g.check('nearby actors remain readable after native removal', near['count'] == 1, near)
    finally:
        set_car(g, previous)


def test_mission_close_forgets_car(g):
    base, off, _ = hud(g)
    # Deliberately keep a live car in the HUD until native mission shutdown owns its destruction.
    car = g.tool('spawn', car='Bolt-Thrower', at='player', ahead=15)['actor']
    g.made.append(car)
    set_car(g, int(car, 16))
    g.tool('input', steps=[{'key': 'Esc'}, {'wait_ms': 300}])
    g.call('menu.command', command=102)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if '138' in str(g.call('menu.state')):
            break
        time.sleep(.05)
    seq = g.latest()
    g.call('menu.command', command=138)
    loaded = g.wait('MISSION_LOADED', since=seq, timeout=20)
    assert loaded, 'main menu did not load'
    g.made.clear()
    try:
        g.check('closing the mission clears the HUD car', pointer(g, base + off) == 0)
    finally:
        set_car(g, 0)
        # The suite restores its fixture before the next case; restore.load needs a live mission.
        g.start_freeride(fresh=True)
