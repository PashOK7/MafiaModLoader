"""Unculled native geometry preparation: source invariance, same-frame instances and restoration. Both builds."""
import time
from mml_game import Skip

START = 'city'
BOX = [-2000, -50, -500, -1000, 300, 700]
PROP_BOX = [-4510, 100, -4510, -4460, 150, -4460]


def test_native_scene_collection(g):
    loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
    if not {'kit', 'kit_rs'} <= loaded:
        raise Skip('load current native and Rust source probes')
    old_time = g.call('state.time')['scale']
    old_camera = g.call('camera.state')['frame_camera']
    devtools = g.call('config.get', mod='devtools') if 'devtools' in loaded else None
    freecam = devtools and devtools['value']['freecam']
    prop = None
    samples = []

    def until(method, predicate):
        deadline = time.monotonic()+30
        while time.monotonic() < deadline:
            value = g.call(method)
            if predicate(value):
                return value
            time.sleep(.05)
        raise RuntimeError('native collection did not settle: '+str(value))

    def advance():
        frame = g.call('ping')['frames']
        until('ping', lambda s: s['frames'] >= frame+3)

    def snapshot(**params):
        before = g.call('kit.scene_collect')['captures']
        g.call('kit.scene_collect', on=True, **params)
        sample = until('kit.scene_collect', lambda s: s['captures'] > before and not s['pending'])
        samples.append(sample)
        return sample

    try:
        g.call('state.time', scale=0)
        if freecam:
            g.call('config.get', mod='devtools')
            g.call('config.set', mod='devtools', key='freecam', value=False)
        g.call('camera.set', eye=old_camera['eye'], dir=old_camera['fwd'], fov=60, ms=600000)
        advance()
        g.call('kit.scene_source', on=True)
        snapshot(box=BOX)  # prepare previously unseen sources before taking a stable baseline
        baseline = snapshot(box=BOX)
        g.check('native C/C++ collection includes valid geometry absent from ordinary camera draws',
                baseline['success'] and baseline['copies'] > 0 and baseline['unseen'] > 0 and
                baseline['triangles'] > 1000 and baseline['invalid'] == 0, baseline)
        g.check('collection C size guard and command-context refusal work',
                baseline['c_checks'] == 3 and baseline['outside_refused'], baseline)
        if not g.call('skin.state')['gpu']['wanted']:
            g.check('CPU mesh packets have matching prepared LOD buffers and index ranges',
                    baseline['unavailable'] == 0 and baseline['copies'] == baseline['native_draws'], baseline)
        for label, direction in [('pitch down', [0, -.8, .6]), ('yaw away', [-1, -.1, 0])]:
            g.call('camera.set', eye=old_camera['eye'], dir=direction, fov=60, ms=600000)
            advance()
            changed = snapshot()
            g.check(label+': source topology and world positions remain identical',
                    changed['success'] and changed['world_hash'] == baseline['world_hash'] and
                    changed['triangles'] == baseline['triangles'], changed)
        meta = snapshot(metadata=True)
        g.check('metadata-only collection keeps source coverage without copying geometry',
                meta['success'] and meta['native_draws'] == baseline['native_draws'] and
                meta['copies'] == 0, meta)
        print('native collection full/metadata ms:', baseline['ms'], meta['ms'], flush=True)

        prop = g.call('prop.create', model='2bbat1', at=[-4500, 120, -4500])['prop']
        isolated = snapshot(box=PROP_BOX)
        visual = isolated['first_visual']
        position = g.call('kit.scene_collect', visual=visual)
        g.check('isolated off-camera native prop is present with current transform',
                isolated['success'] and isolated['unique'] == 1 and position['present'] and
                abs(position['position'][0]+4500) < .001, position)
        g.call('camera.set', eye=old_camera['eye'], dir=[1, .4, 0], fov=100, ms=600000)
        advance()
        fov = snapshot()
        g.check('FOV/frustum changes do not change the isolated world geometry',
                fov['success'] and fov['world_hash'] == isolated['world_hash'], fov)
        g.call('prop.place', prop=prop, pos=[-4475, 120, -4500])
        moved = snapshot()
        position = g.call('kit.scene_collect', visual=visual)
        g.check('moved instance is published in the next collected frame',
                moved['success'] and moved['frame'] > fov['frame'] and position['present'] and
                abs(position['position'][0]+4475) < .001, position)
        g.call('prop.remove', prop=prop)
        prop = None
        removed = snapshot()
        g.check('removed instance leaves no stale geometry in the current snapshot',
                removed['success'] and removed['unique'] == 0 and
                not g.call('kit.scene_collect', visual=visual)['present'], removed)
        g.check('every successful collection restores actual device states and texture bindings',
                all(s['state_equal'] for s in samples if s['success']), samples)
        invalid = snapshot(box=[1, 1, 1, -1, -1, -1])
        g.check('inverted bounds refuse a current snapshot', not invalid['success'])
        g.call('kit_rs.scene_source', on=True)
        g.call('kit_rs.scene_collect', on=True)
        rust = until('kit_rs.scene_collect', lambda s: not s['pending'])
        g.check('safe Rust owns collection metadata and conservative missing boxes',
                rust['success'] and rust['draws'] > 0 and rust['outside_refused'], rust)
        g.call('kit_rs.scene_source', on=False)
        g.call('kit.scene_source', on=False)
        absent = snapshot(box=BOX)
        g.check('collection without its owner observer is refused', not absent['success'])
        g.call('kit.scene_source', on=True)
        g.call('kit.scene_collect', on=True, box=BOX, fault=True)
        until('mods.list', lambda rows: not next(m for m in rows if m['id'] == 'kit')['loaded'])
        advance()
        g.check('fault during native collection releases owner and game continues', g.call('ping')['frames'] > 0)
        g.call('mod.load', id='kit')
        g.call('kit.scene_source', on=True)
        recovered = snapshot(box=PROP_BOX)
        g.check('collection reacquires native state after nested callback fault',
                recovered['success'] and recovered['state_equal'] and recovered['unique'] == 0, recovered)
    finally:
        if prop:
            g.call('prop.remove', prop=prop)
        loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
        if 'kit' in loaded:
            g.call('kit.scene_source', on=False)
        if 'kit_rs' in loaded:
            g.call('kit_rs.scene_source', on=False)
        # Seed the developer camera from the held pose before returning its ownership, as in the native test loop.
        g.call('camera.set', eye=old_camera['eye'], dir=old_camera['fwd'], ms=600000)
        advance()
        if freecam:
            g.call('config.get', mod='devtools')
            g.call('config.set', mod='devtools', key='freecam', value=True)
            advance()
        g.call('camera.release')
        g.call('state.time', scale=old_time)
