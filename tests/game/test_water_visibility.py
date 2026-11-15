"""Native regression for the lost radiance on wave slopes, using an identical frozen wave field."""
import struct
import time
from pathlib import Path

START = 'city'


def test_wave_visibility(g):
    water = g.call('config.get', mod='river_water')['changes']
    dev = g.call('config.get', mod='devtools')['changes']
    has_dlss = any(m['id']=='dlss_nr' and m.get('loaded') for m in g.call('mods.list'))
    dlss = g.call('config.get', mod='dlss_nr')['changes'] if has_dlss else {}
    scale = g.call('state.time')['scale']
    touched = dict(wind=2.5, waves=1, wave_height=8, wave_width=6.5, wave_speed=0,
                   chop=.900554, foam=0, reflection_softness=.25, debug=0)

    def put(key, value):
        g.call('config.set', mod='river_water', key=key, value=value)

    def ready():
        frame = g.call('river_water.status')['drawn']
        until = time.monotonic()+30
        while time.monotonic()<until:
            state = g.call('river_water.status')
            if state['drawn']>=frame+6:
                return state
            time.sleep(.05)
        raise RuntimeError('water frame did not advance')

    def pixels():
        shot = g.call('screenshot')
        until = time.monotonic()+20
        while time.monotonic()<until:
            info = g.call('screenshot.state', id=shot['id'])
            if info.get('done'):
                break
            time.sleep(.05)
        assert info.get('ok'), info
        raw = Path(info['path']).read_bytes()
        magic, width, height, _ = struct.unpack_from('<4I', raw)
        assert magic == 0x534c4d4d
        return [tuple(raw[16+4*(y*width+x):19+4*(y*width+x)])
                for y in range(height//2, height*7//10)
                for x in range(width//10, width*9//10)]

    def dark(ps):
        return sum(max(p)<24 for p in ps)

    try:
        g.call('state.time', scale=0)
        g.call('config.set', mod='devtools', key='freecam', value=False)
        if has_dlss:
            g.call('config.set', mod='dlss_nr', key='on', value=False)
        for key, value in touched.items():
            put(key, value)
        # Reload with zero speed supplies a deterministic phase (t=0) in both builds.
        g.call('mod.reload', id='river_water')
        g.call('camera.set', eye=[-1488.6678, -9.6745, -56.7638],
               dir=[-.4623, -.04, .85134], fov=54.5, ms=600000)
        put('debug', 21)  # developer-only previous single-scattering path, same final display transform
        old = ready()
        before = pixels()
        put('debug', 0)
        new = ready()
        after = pixels()
        counts = {'before': dark(before), 'after': dark(after), 'pixels': len(after)}
        print(f'[{g.build}] black-band pixel counts: {counts}', flush=True)
        g.check('frozen native wave geometry/phase unchanged by transport',
                old['wave_controls']==new['wave_controls'] and old['waves']==new['waves'])
        g.check('old path reproduces black wave bands', counts['before']>len(before)*.002, counts)
        g.check('wave transport removes most black-band pixels', counts['after']<counts['before']*.25, counts)
        # Empty geometry/depth/shadow assertions cannot establish appearance; compare actual native pixels.
        g.check('fix does not globally brighten the river',
                sum(sum(p) for p in after)<sum(sum(p) for p in before)*1.35, counts)
        put('waves', 0)
        put('debug', 21)
        ready()
        flat_old = pixels()
        put('debug', 0)
        ready()
        flat_new = pixels()
        maximum = max(abs(a-b) for p,q in zip(flat_old,flat_new) for a,b in zip(p,q))
        g.check('flat-water radiance retained without extra sky fill', maximum<=1, maximum)
    finally:
        for key in touched:
            put(key, water.get(key))
        if has_dlss:
            g.call('config.set', mod='dlss_nr', key='on', value=dlss.get('on'))
        g.call('state.time', scale=scale)
        # Return control to the previous mod/game camera. camera_ray reads the native game camera;
        # a temporary bridge camera does not seed devtools' stored free-camera position.
        g.call('config.set', mod='devtools', key='freecam', value=dev.get('freecam'))
        g.call('camera.release')
