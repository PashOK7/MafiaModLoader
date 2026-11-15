"""The native sound policy must reach actual source buffers, retaining spatial controls."""

def test_native_sound_allocation(g):
    state = g.call('sound.compat')
    g.check('sound allocation initialization hook ran', state['hooked'] and state['initialized'], state)
    g.check('retired gain workaround is absent', state['gain_workaround'] is False, state)
    if state['caps_hresult'] != 0 or state['max_hw_3d'] != 0 or state['native_flags'] & 0x100000:
        # Hardware/EAX providers keep their native policy. The real zero-capacity branch is checked below.
        return
    g.check('zero hardware 3D capacity selects the native software path', state['software_buffers'], state)
    sound = g.call('sound.play', file='th_2.wav', on='player', loop=True, volume=0)
    def pointer(at):
        return g.tool('mem_read', addr=hex(at), size=4, **{'as': 'u32'})['value'][0]
    try:
        frame = int(sound['frame'], 16)
        voice = pointer(frame + 0x15c)
        buffer = pointer(voice + 8)
        r = g.call('native.call', at=hex(pointer(pointer(buffer) + 0x0c)), conv='stdcall',
                   args=[buffer, {'buf': 20, 'hex': '14000000'}], ret='int')
        g.check('actual source buffer capabilities are readable', r['ret'] == 0 and r['stack']['ok'], r)
        flags = int(r['buffers'][0]['words'][1], 16)
        g.check('native spatial buffer is software allocated without deferred allocation',
                flags & 0x18 == 0x18 and flags & 0x40000 == 0, hex(flags))
    finally:
        g.call('sound.stop', sound=sound['sound'])
