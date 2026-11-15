"""External support must reach the native usability query without leaking into tyre physics."""
import struct

from mml_game import Skip, ToolError


def test_native_support_query_and_lifetime(g):
    mods = g.call('mods.list')
    if not any(mod['id'] == 'kit' for mod in mods):
        raise Skip('install the kit test mod')
    ragdoll_loaded = any(mod['id'] == 'ragdoll' and mod['loaded'] for mod in mods)
    kit_loaded = any(mod['id'] == 'kit' and mod['loaded'] for mod in mods)
    if ragdoll_loaded:
        g.call('mod.unload', id='ragdoll')
    if not kit_loaded:
        g.call('mod.load', id='kit')
    car = g.call('car')
    wheels = [int(wheel['wheel'], 16) + 0x120 for wheel in car['wheels']]
    assert len(wheels) == 4, 'The fixture needs the four-wheel starter car'
    native = {'1.2': '0x429850', '1.0': '0x470D30'}[g.build]

    def read():
        return [g.call('mem.read', addr=hex(address), size=4)['hex'] for address in wheels]

    def query(checks=255):
        result = g.call('native.call', at=native, conv='thiscall',
                        **{'this': car['car']}, args=[checks], ret='int')
        assert result['stack']['ok']
        return result['ret'] & 255  # Native bool returns AL. Upper EAX bytes are unspecified.

    g.call('state.time', scale=0)
    original = read()
    expected = list(original)

    def write(values):
        nonlocal expected
        assert read() == expected, 'The paused fixture flags changed unexpectedly'
        for address, value in zip(wheels, values):
            g.call('mem.write', addr=hex(address), hex=value)
        expected = list(values)

    try:
        g.check('road car starts usable', query() == 1)
        airborne = [struct.pack('<I', int.from_bytes(bytes.fromhex(value), 'little') & ~8).hex()
                    for value in original]
        airborne[0] = struct.pack('<I', int.from_bytes(bytes.fromhex(airborne[0]), 'little') | 0x20000000).hex()
        write(airborne)
        g.call('kit.vehicle_support', mode='off')
        g.check('native rejects four unsupported wheels', query() == 0)
        g.check('native bool reader ignores unspecified upper EAX bytes', not g.call('kit.vehicle_usable')['usable'])
        g.check('support provider accepted', g.call('kit.vehicle_support', mask=7)['accepted'])
        g.check('three supported wheels make the intact car usable', query() == 1)
        g.check('query restores every native wheel flag', read() == airborne)
        g.call('kit.vehicle_support', mask=5)
        g.check('one supported side still fails the native side check', query(4) == 0)
        g.call('kit.vehicle_support', mask=3)
        g.check('two supported wheels fail the native low-speed count', query() == 0)
        g.call('kit.vehicle_support', mask=15)
        detached = list(airborne)
        detached[0] = struct.pack('<I', int.from_bytes(bytes.fromhex(detached[0]), 'little') | 0x40000000).hex()
        write(detached)
        g.check('support preserves native rejection of a detached wheel', query() == 0)
        g.check('detached wheel flags also restored', read() == detached)
        write(airborne)
        g.call('kit.vehicle_support', mode='off')
        g.check('unregister removes external support immediately', query() == 0)
        g.call('kit.vehicle_support', mask=15, mode='fault')
        g.check('a provider fault falls back to native rejection', query() == 0)
        g.check('provider fault leaves native wheel flags intact', read() == airborne)
        g.call('mod.reload', id='kit')
        g.call('kit.vehicle_support', mask=15)
        g.check('provider can register after reloading', query() == 1)
        g.call('mod.unload', id='kit')
        g.check('owner unload removes external support', query() == 0)
    finally:
        write(original)
        g.call('state.time', scale=1)
        if not kit_loaded:
            try:
                g.call('mod.unload', id='kit')
            except ToolError:
                pass
        else:
            g.call('mod.load', id='kit')
        if ragdoll_loaded:
            g.call('mod.load', id='ragdoll')
