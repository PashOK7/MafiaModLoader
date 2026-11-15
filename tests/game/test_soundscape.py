"""Source telemetry against an actual native sound; drawing budget, pause and unload behavior."""
import time
import math
import struct
import wave
from pathlib import Path


def test_soundscape_sources_and_lifecycle(g):
    mods = g.tool('game_status')['mods']
    if not any(m['id'] == 'soundscape' and m['loaded'] for m in mods):
        g.call('mod.load', id='soundscape')
    g.later('mod.unload', id='soundscape')
    g.call('config.set', mod='soundscape', key='show', value=True)
    original = g.call('sound.compat')
    camera = g.call('camera.state')['frame_camera']
    point = [a + b * 7 for a, b in zip(camera['eye'], camera['fwd'])]
    root = Path(g.tool('game_status')['game_dir']).resolve()
    tone = root / 'mml' / 'tests' / 'soundscape_tone.wav'
    if not tone.resolve().is_relative_to(root) or tone.exists():
        raise RuntimeError('refusing to overwrite an existing/non-lab tone')
    tone.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(tone), 'wb') as out:
        out.setparams((1, 2, 8000, 16000, 'NONE', 'not compressed'))
        out.writeframes(b''.join(struct.pack('<h', round(13000*math.sin(i*2*math.pi*200/8000))) for i in range(16000)))
    sound = g.call('sound.play', file='mml\\tests\\soundscape_tone.wav', at=point, loop=True, volume=.35, range=[3, 22])
    try:
        g.sleep(850)
        snapshot = g.call('soundscape.status')
        own = next((s for s in snapshot['sources'] if s['frame'] == int(sound['frame'], 16)), None)
        g.check('soundscape knows this game build', snapshot['known'], snapshot)
        g.check('actual native source found in engine list', own is not None, snapshot['registered'])
        if own:
            g.check('native source filename decoded', own['name'].lower().endswith('soundscape_tone.wav'), own)
            g.check('source world location follows native placement', max(abs(a-b) for a,b in zip(own['pos'], point)) < .02, own)
            g.check('native authored range and gain read accurately',
                    abs(own['volume']-.35)<.001 and abs(own['near']-3)<.001 and abs(own['far']-22)<.001, own)
        g.check('real sources emit bounded artistic waves', 0 < snapshot['waves'] <= 16 and snapshot['emitted'] > 0, snapshot)
        g.check('render queue leaves reserve for game and other mods', snapshot['lines_last'] <= 1702, snapshot)
        g.check('actual source PCM measured', own and own['pcm_measured'] and .27 < own['rms'] < .30, own)
        g.check('geometry and wet-output measurement distinguished',
                snapshot['measured_waveform'] is True and snapshot['traced_reflections'] is True and
                snapshot['measured_wet_output'] is False)
        g.check('world rendering submits 3D triangles and restores native state',
                snapshot['primitives'] > 0 and snapshot['state_restored'] and snapshot['render_hresult'] == 0, snapshot)
        g.check('room scan includes a 3D surface mesh',
                snapshot['room_samples'] == 264 and snapshot['room_triangles'] > 0, snapshot)
        g.check('room mesh uses actual native collision triangles',
                snapshot['native_surface_triangles'] > 0, snapshot)
        def source_now():
            return next(s for s in g.call('soundscape.status')['sources'] if s['frame'] == int(sound['frame'], 16))
        g.call('sound.set', sound=sound['sound'], volume=.8)
        g.sleep(180)
        loud = source_now()
        g.call('sound.set', sound=sound['sound'], volume=.1)
        g.sleep(180)
        quiet = source_now()
        g.check('native backend gain follows eightfold source volume change',
                quiet['backend_gain'] > 0 and 7.9 < loud['backend_gain']/quiet['backend_gain'] < 8.1, (loud, quiet))
        g.check('measured emission amplitude changes eightfold, not just alpha',
                quiet['emission_level'] > 0 and 7.9 < loud['emission_level']/quiet['emission_level'] < 8.1, (loud, quiet))
        g.call('sound.set', sound=sound['sound'], volume=0)
        g.sleep(180)
        muted = source_now()
        g.check('mute has exactly zero measured emission and listener energy',
                muted['emission_level'] == 0 and muted['listener_level'] == 0, muted)
        g.call('sound.set', sound=sound['sound'], volume=.35)
        g.sleep(100)
        # Reuse the same native sound frame without an observable stopped interval.
        # Gunshots can restart a frame this way; only watching a logical false->true edge misses them.
        frame = int(sound['frame'], 16)
        vt = g.tool('mem_read', addr=hex(frame), size=4, **{'as': 'u32'})['value'][0]
        setter = g.tool('mem_read', addr=hex(vt + 0x24), size=4, **{'as': 'u32'})['value'][0]
        before_restart = g.call('soundscape.status')['emitted']
        g.call('native.call', at=hex(setter), conv='stdcall', args=[frame, 1], ret='void')
        g.sleep(80)
        g.check('restarting an existing native sound emits another wave',
                g.call('soundscape.status')['emitted'] > before_restart)
        g.call('config.set', mod='soundscape', key='show', value=False)
        off = g.call('soundscape.status')
        still = next(s for s in off['sources'] if s['frame'] == int(sound['frame'], 16))
        g.check('hide clears the visual simulation without stopping sound', not off['shown'] and off['waves'] == 0 and still['logical'], off)
        g.check('hide leaves native gain unchanged', abs(still['volume']-.35)<.001, still)
        g.call('config.set', mod='soundscape', key='show', value=True)
        g.tool('input', steps=[{'key': 'Esc'}, {'wait_ms': 120}])
        try:
            first = g.call('soundscape.status')
            time.sleep(.12)
            second = g.call('soundscape.status')
            g.check('artistic waves freeze under native pause', first['time'] == second['time'], (first['time'], second['time']))
        finally:
            g.tool('input', steps=[{'key': 'Esc'}, {'wait_ms': 120}])
        after = g.call('sound.compat')
        g.check('native audio allocation policy unchanged', after['native_flags'] == original['native_flags'] and after['changed'] == original['changed'])
        g.call('mod.unload', id='soundscape')
        state = g.call('sounds.list')
        remaining = next(s for s in state['sounds'] if s['sound'] == sound['sound'])
        g.check('unload does not stop another owner native source', remaining['playing'], remaining)
        g.call('mod.load', id='soundscape')
        g.check('soundscape hot reload succeeds', g.call('soundscape.status')['known'])
    finally:
        g.call('sound.stop', sound=sound['sound'])
        if tone.resolve().is_relative_to(root):
            tone.unlink(missing_ok=True)
