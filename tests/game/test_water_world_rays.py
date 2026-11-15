"""Native CPU/GPU world-ray checkpoint; deliberately independent of normal water visibility/materials."""
import time
from mml_game import Skip

START = 'city'


def test_water_native_world_rays(g):
    if not any(m['id'] == 'river_water' and m.get('loaded') for m in g.call('mods.list')):
        raise Skip('load current river_water geometry diagnostic')
    old_camera = g.call('camera.state')['frame_camera']
    old_time = g.call('state.time')['scale']
    props = []
    samples = []
    box = [-4520, 100, -4520, -4460, 150, -4440]

    def capture(ray):
        before = g.call('river_water.scene_probe')['captures']
        g.call('river_water.scene_probe', capture=True, box=box, ray=ray)
        end = time.monotonic()+30
        while time.monotonic() < end:
            value = g.call('river_water.scene_probe')
            if not value['pending'] and value['captures'] > before:
                samples.append(value)
                g.check('fresh native snapshot uploads and agrees with independent CPU reference',
                        value['success'] and value['cpu_reference']['agrees'], value)
                if value['hit']['status'] == 1:
                    g.check('GPU original UV0/UV1 and interpolated native prelit colors match the hit primitive',
                            value['attributes']['agrees'], value)
                return value
            time.sleep(.05)
        raise RuntimeError('native GPU ray did not finish: '+str(value))

    try:
        g.call('state.time', scale=0)
        # All source instances are kilometres outside the active game camera and old reflected view.
        for z in (-4500, -4490, -4480):
            props.append(g.call('prop.create', model='2bbat1', at=[-4500, 120, z], scale=10)['prop'])
        ray = [-4500.03, 120.13, -4510, 0, 0, 1, 50]
        first = capture(ray)
        g.check('off-camera native foreground produces a real triangle hit', first['hit']['status'] == 1, first)
        for direction, fov in (([1, .4, 0], 110), ([-1, -.6, .3], 45)):
            g.call('camera.set', eye=old_camera['eye'], dir=direction, fov=fov, ms=600000)
            changed = capture(ray)
            g.check('pitch/yaw/FOV cannot change the same world-ray hit',
                    changed['hit'] == first['hit'], changed)
        g.call('prop.place', prop=props[0], pos=[-4470, 120, -4500])
        second = capture(ray)
        g.check('moving off-camera foreground reveals a deeper native layer',
                second['hit']['status'] == 1 and second['hit']['distance'] > first['hit']['distance']+5, second)
        g.call('prop.remove', prop=props.pop(1))
        third = capture(ray)
        g.check('removing the middle instance reveals the final layer in the next snapshot',
                third['hit']['status'] == 1 and third['hit']['distance'] > second['hit']['distance']+5, third)
        for prop in props:
            g.call('prop.remove', prop=prop)
        props.clear()
        empty = capture(ray)
        g.check('removed instances leave a complete bounded miss with no ghost',
                empty['hit']['status'] == 0 and empty['triangles'] == 0, empty)
        outside = capture([-4500, 120, -4510, 0, 0, -1, 50])
        g.check('leaving declared coverage returns unknown, never false sky', outside['hit']['status'] == 2, outside)
        print('native_world_ray_samples:', samples, flush=True)
    finally:
        for prop in props:
            g.call('prop.remove', prop=prop)
        g.call('river_water.scene_probe', release=True)
        g.call('state.time', scale=old_time)
        g.call('camera.release')


def test_water_native_current_instances(g):
    """Same-frame transform/lifetime gate, using an asymmetric native model outside both capture views."""
    old_time = g.call('state.time')['scale']
    prop = None
    samples = []
    box = [-4520, 100, -4520, -4480, 150, -4480]
    ray = [-4500, 120.6, -4510, 0, 0, 1, 25]

    def capture():
        mutation_frame = g.call('ping')['frames']
        before = g.call('river_water.scene_probe')['captures']
        g.call('river_water.scene_probe', capture=True, box=box, ray=ray)
        end = time.monotonic()+30
        while time.monotonic() < end:
            value = g.call('river_water.scene_probe')
            if not value['pending'] and value['captures'] > before:
                samples.append(value)
                g.check('snapshot uses current post-mutation frame and agrees with independent geometry oracle',
                        value['success'] and value['source_frame'] >= mutation_frame
                        and value['cpu_reference']['agrees'], value)
                return value
            time.sleep(.05)
        raise RuntimeError('current instance query timeout')

    try:
        g.call('state.time', scale=0)
        prop = g.call('prop.create', model='taxi00.i3d', at=[-4500, 120, -4500])['prop']
        initial = capture()
        g.check('asymmetric off-camera native model is hit', initial['hit']['status'] == 1, initial)
        g.call('prop.place', prop=prop, dir=[1, 0, 0])
        rotated = capture()
        g.check('rotation changes the same world-ray surface depth immediately',
                rotated['hit']['status'] == 1
                and abs(rotated['hit']['distance']-initial['hit']['distance']) > .25, rotated)
        g.call('prop.place', prop=prop, pos=[-4470, 120, -4500])
        moved = capture()
        g.check('moving the off-camera instance leaves no previous-position hit', moved['hit']['status'] == 0, moved)
        g.call('prop.place', prop=prop, pos=[-4500, 120, -4500], dir=[0, 0, 1])
        returned = capture()
        g.check('restoring its pose restores the original native hit depth',
                returned['hit']['status'] == 1
                and abs(returned['hit']['distance']-initial['hit']['distance']) < .002, returned)
        g.call('prop.remove', prop=prop)
        prop = None
        removed = capture()
        g.check('deletion removes the instance in the next source snapshot',
                removed['hit']['status'] == 0 and removed['triangles'] == 0, removed)
        print('native_current_instance_samples:', samples, flush=True)
    finally:
        if prop:
            g.call('prop.remove', prop=prop)
        g.call('river_water.scene_probe', release=True)
        g.call('state.time', scale=old_time)


def test_water_native_singlemorph_pose(g):
    """SMMR people must reach native preparation with current off-camera posed vertices."""
    old_time = g.call('state.time')['scale']
    actor = None
    box = [-4520, 100, -4520, -4480, 150, -4480]

    def capture(x):
        before = g.call('river_water.scene_probe')['captures']
        g.call('river_water.scene_probe', capture=True, box=box, ray=[x, 121.4, -4510, 0, 0, 1, 20])
        deadline = time.monotonic()+30
        while time.monotonic() < deadline:
            result = g.call('river_water.scene_probe')
            if not result['pending'] and result['captures'] > before:
                return result
            time.sleep(.03)
        raise RuntimeError('off-camera singlemorph query timeout')

    try:
        g.call('state.time', scale=0)
        actor = g.call('world.spawn', human='Paulie', at=[-4500, 120, -4500], ground=False)['actor']
        bone = next(b for b in g.call('human.skeleton', actor=actor)['bones'] if b['name'] == 'pelvis')
        pose = {'rot': bone['rot'], 'pos': list(bone['pos'])}
        g.call('human.pose', actor=actor, bones={'pelvis': pose}, ms=600000)
        initial = capture(-4500)
        g.check('off-camera SMMR person yields actual posed triangles and a CPU/GPU matching hit',
                initial['success'] and initial['triangles'] > 0 and initial['hit']['status'] == 1
                and initial['cpu_reference']['agrees'], initial)
        pose['pos'][0] += 3
        g.call('human.pose', actor=actor, bones={'pelvis': pose}, ms=600000)
        old_place, new_place = capture(-4500), capture(-4497)
        g.check('changing an off-camera skeleton removes old vertices and publishes the new body',
                old_place['hit']['status'] == 0 and new_place['hit']['status'] == 1
                and old_place['cpu_reference']['agrees'] and new_place['cpu_reference']['agrees']
                and new_place['triangles'] == initial['triangles'], (old_place, new_place))
        pose['pos'] = bone['pos']
        g.call('human.pose', actor=actor, bones={'pelvis': pose}, ms=600000)
        restored = capture(-4500)
        g.check('restoring pose restores the original hit without a rest-pose substitute or ghost',
                restored['hit']['status'] == 1 and restored['cpu_reference']['agrees']
                and abs(restored['hit']['distance']-initial['hit']['distance']) < .002, restored)
    finally:
        if actor:
            g.call('human.pose', actor=actor, clear=True)
            g.call('world.remove', actor=actor)
        g.call('river_water.scene_probe', release=True)
        g.call('state.time', scale=old_time)
