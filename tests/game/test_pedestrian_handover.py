"""Run with both labs in freeride with pedestrians=1 and ragdoll/kit installed.

The fixture changes skeleton matrices and invokes native contact in the same game-thread command.
This forces a measurable difference from the last cached pose without an intervening animation tick.
It does not show the changed pose on screen before the synthetic contact.
"""
import math
import time
from mml_game import Skip


def test_current_pose_at_car_contact(g):
    peds = [p for p in g.call('pedestrians.list')['pedestrians'] if p['alive'] and p['spawned']]
    if not peds:
        raise Skip('needs freeride pedestrians=1')
    loaded = {m['id'] for m in g.call('mods.list') if m['loaded']}
    for mod in ('ragdoll', 'kit'):
        if mod not in loaded:
            g.call('mod.load', id=mod)
            g.later('mod.unload', id=mod)
    if not g.player().get('vehicle'):
        g.call('world.seat', actor='player', car='car', seat=0)
    bodies = {b['actor'] for b in g.call('ragdoll.state')['ragdolls']}
    peds = [p for p in peds if p['element'] not in bodies]
    if not peds:
        raise Skip('needs a pedestrian that is not already physical')
    player = g.pos('player')
    peds.sort(key=lambda p: math.dist(p['pos'], player))
    ped = peds[0]
    at = ped['pos']
    # Native pedCarHit deactivates distant pedestrians instead of hitting them (20 m from camera).
    g.tool('teleport', to=[at[0]-5, at[1]+.15, at[2]], dir=[1,0,0])
    g.call('camera.set', eye=[at[0]-3, at[1]+2, at[2]-3], at=[at[0], at[1]+1, at[2]], ms=2000)
    g.later('camera.release')
    g.sleep(250)
    deadline = time.monotonic() + 5
    probe = g.call('kit.pedestrian_handover', pedestrian=ped['element'], probe=True)
    while not probe['eligible'] and time.monotonic() < deadline:
        g.sleep(100)
        probe = g.call('kit.pedestrian_handover', pedestrian=ped['element'], probe=True)
    g.check('native contact target passes its camera-distance gate',probe['eligible'],probe)
    result = g.call('kit.pedestrian_handover', pedestrian=ped['element'], shift=True)
    g.sleep(150)
    rows = [e for e in g.call('ragdoll.state')['car_handovers'] if e['actor'] == ped['element']]
    entry = rows[-1] if rows else {}
    g.check('native car contact actually started a ragdoll', result['accepted'] and entry.get('started'),
            {'contact': result, 'handover': entry})
    g.check('physical start uses the captured pre-hit skeleton', entry.get('captured') and entry.get('pre_hit_pose'), entry)
    g.check('all initial skeleton points match the pre-hit matrices',
            entry.get('started') and entry.get('pose_gap_m', 100) < .002, entry)
    g.check('fixture and diagnostic agree on the pre-hit pelvis',
            entry.get('captured') and math.dist(entry['pelvis_before'], result['pelvis_before']) < .002, entry)
    g.check('post-world snapshot is retained separately from a same-command matrix change',
            entry.get('last_drawn_captured') and entry.get('last_drawn_gap_m',0) > 1, entry)
    samples = g.call('ragdoll.handover', pedestrian=ped['element'])['samples']
    g.check('bounded pose history contains tick and render transitions',
            4 <= len(samples) <= 128 and {1,2,3,4}.issubset({p['stage'] for p in samples}), len(samples))
    drawn = [p for p in samples if p['stage'] == 4]
    g.check('rendered pelvis follows the physical pelvis after applying the pose',
            bool(drawn) and max(math.dist(p['drawn'],p['physical']) for p in drawn) < .005, drawn[-1:] )
    world = [p for p in samples if p['stage'] == 5]
    g.check('pelvis still matches after the actual world drawing',
            bool(world) and max(math.dist(p['drawn'],p['physical']) for p in world) < .005, world[-1:])
