"""Real-device regression: the shadow camera must not disable the main city's depth writes.

Needs the graphics bridge (a gfx mod enabled before launch). Uses a fixed river view in both builds;
settings are restored even after failure. No synthetic device can reproduce LS3D's cached-state reset.
"""
from mml_game import Skip

START = 'city'


def test_shadow_depth_and_cutouts(g):
    if g.call('gfx.state').get('state') != 'on':
        raise Skip('enable river_water before launching the graphics device')
    mods = g.call('mods.list')
    was_loaded = any(m['id'] == 'river_water' and m.get('loaded') for m in mods)
    if not was_loaded:
        g.call('mod.load', id='river_water')
    config = g.call('config.get', mod='river_water')
    # Restore overrides themselves, so running the test does not create new saved preferences.
    old = {k: config['changes'].get(k) for k in ['on', 'shadows']}
    try:
        g.call('camera.set', eye=[-1490, 30, 20], at=[-1590, -10, -25], ms=60000)
        g.call('config.set', mod='river_water', key='on', value=True)

        def survey(shadows=False):
            request = g.call('shader.survey', shadows=shadows)
            g.tool('screenshot', max_width=320)
            return g.call('shader.survey', id=request['id'])

        for enabled in [False, True, False, True]:
            g.call('config.set', mod='river_water', key='shadows', value=enabled)
            main = survey()
            opaque = [c for c in main.get('combinations', []) if not c['blend'] and c['faces']]
            g.check('opaque city still writes depth, shadows=%s' % enabled,
                    bool(opaque) and all(c['zwrite'] for c in opaque), opaque)

        state = g.call('shader.state')['shadows']
        g.check('LS3D draws the shadow casters', state.get('drawn') and state.get('casters_by') == 'game', state)
        shadow = survey(True)
        combos = shadow.get('combinations', [])
        g.check('shadow survey records actual draws', shadow.get('draws', 0) > 100, shadow)
        g.check('cut-outs use textures and alpha test',
                any(c['alpha_test'] and any(s['texture'] for s in c['stages']) for c in combos), combos)
        g.check('every recorded caster writes depth', bool(combos) and all(c['zwrite'] for c in combos), combos)
        names = [v['name'].lower() for c in combos for v in c.get('visuals', [])]
        g.check('hidden baked water shades never become casters',
                not any(n.startswith('watercity.stin') for n in names), names)
        mirror = g.call('shader.mirror', check=2000)
        g.tool('screenshot', max_width=320)
        mirror = g.call('shader.mirror')
        g.check('shadow path uses a checked device mirror', mirror['users'] > 0 and mirror['hooked'] and
                mirror['mismatch_draws'] == 0, mirror)
        audio = g.call('view.state')['audio']
        g.check('auxiliary audio skipped while the world still updates',
                audio['aux_skipped'] > 0 and audio['main_updates'] > 0, audio)
    finally:
        for key, value in old.items():
            g.call('config.set', mod='river_water', key=key, value=value)
        g.call('camera.release')
        if not was_loaded:
            g.call('mod.unload', id='river_water')
