"""Run with the labs in city freeride with pedestrians=1 and the kit and ragdoll installed."""
from mml_game import Skip, ToolError


START = 'city'


def find_dodge(g, placement):
    peds = g.call('pedestrians.list')['pedestrians']
    for ped in peds:
        if not ped['alive'] or not ped['spawned']:
            continue
        try:
            result = g.call('kit.pedestrian_dodge', pedestrian=ped['element'], placement=placement)
        except ToolError as exc:
            if 'requires a living traffic pedestrian' in str(exc):
                continue
            raise
        if not result['eligible'] or not result['hit_car'] or result['animation'] != 0x143:
            continue
        if placement == 'behind' and result['stop'] < .5:
            return result
        if placement == 'ahead' and 0 < result['stop'] < .06:
            return result
    raise AssertionError(f'could not place a car {placement} on an open native dodge path')


def test_dodge_only_sweeps_untravelled_distance(g):
    if not any(p['alive'] and p['spawned'] for p in g.call('pedestrians.list')['pedestrians']):
        raise Skip('needs city freeride pedestrians=1')
    loaded = {m['id'] for m in g.call('mods.list') if m['loaded']}
    for mod in ('kit', 'ragdoll'):
        if mod not in loaded:
            g.call('mod.load', id=mod)
            g.later('mod.unload', id=mod)

    g.call('ragdoll.dodge', mode='native')
    try:
        native = find_dodge(g, 'behind')
        g.check('native sweep moves the model back to the old path', native['advance'] < -1.5 and
                native['model_error'] < .005, native)
    finally:
        g.call('ragdoll.dodge', mode='fixed')

    before = g.call('ragdoll.state')['dodge_step_sweeps']
    fixed = find_dodge(g, 'behind')
    g.check('car behind the pedestrian no longer rewinds the dodge', 0 < fixed['advance'] < .10 and
            fixed['step'] < .10 and fixed['model_error'] < .005, fixed)
    g.check('the replacement sweep ran in the game', g.call('ragdoll.state')['dodge_step_sweeps'] > before)

    ahead = find_dodge(g, 'ahead')
    g.check('the native collider ahead still stops the dodge', abs(ahead['advance'] - ahead['stop']) < .025 and
            ahead['step'] < .12 and ahead['model_error'] < .005, ahead)
