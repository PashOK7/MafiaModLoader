"""The real D3D11 tessellation path, including off/on transitions and its state handed back to DLSS."""
from mml_game import Skip

START = 'city'


def test_tessellated_water(g):
    if g.call('gfx.state').get('state') != 'on':
        raise Skip('enable river_water before launching the graphics device')
    mods = g.call('mods.list')
    was_loaded = any(m['id'] == 'river_water' and m.get('loaded') for m in mods)
    if not was_loaded:
        g.call('mod.load', id='river_water')
    cfg = g.call('config.get', mod='river_water')
    overrides = cfg['changes']
    settings = dict(on=True, geometry=True, wave_height=2, wind=10, waves=1, debug=0, shadows=True)
    try:
        for key, value in settings.items():
            g.call('config.set', mod='river_water', key=key, value=value)
        g.call('camera.set', eye=[-1470, -9.7, -394], at=[-1490, -9.5, -345], ms=60000)
        for _ in range(12):
            g.tool('screenshot', max_width=320)
            state = g.call('river_water.status')
            if state.get('geometry', {}).get('triangles', 0) > 100 and state['reflection']['plane'] >= 0:
                break
        mesh = state.get('geometry', {})
        g.check('actual GPU tessellated vertices', mesh.get('vertices', 0) > mesh.get('patches', 0) > 54, mesh)
        g.check('actual rasterized wave triangles', mesh.get('triangles', 0) > 100, mesh)
        g.check('fresh reflection and nonzero physical height', state['reflection']['plane'] >= 0 and
                state['waves']['height_dev_m'] > 0.01, state)
        for enabled in [False, True]:
            g.call('config.set', mod='river_water', key='geometry', value=enabled)
            g.tool('screenshot', max_width=320)
            g.check('3D mode toggles live: %s' % enabled,
                    g.call('river_water.status')['geometry']['on'] == enabled)
        gfx = g.call('gfx.state')
        g.check('graphics bridge remains active', gfx['state'] == 'on', gfx)
        if any(m['id'] == 'dlss_nr' and m.get('loaded') for m in mods):
            dlss = g.call('dlss_nr.state')
            g.check('DLSS continues after tessellated water', dlss['on'] and dlss['drawn'] > 0 and not dlss['last_error'], dlss)
            temporal = g.call('river_water.status')['temporal']
            g.check('water emits its depth and moving-wave history', temporal['drawn'] > 0 and temporal['history'], temporal)
            g.check('host merges replacement temporal inputs', g.call('gfx.state')['surface_temporal']['frames'] > 0)
        shadow = g.call('shader.state')['shadows']
        g.check('visible receiver coverage has no fixed-radius cutoff', shadow.get('coverage', {}).get('fitted'), shadow)
        g.check('receiver fit retains world texel density', shadow['coverage']['texel_m'] <= 240 / 2048 + 1e-5 and
                not shadow['coverage']['density_limited'], shadow)
    finally:
        for key in settings:
            g.call('config.set', mod='river_water', key=key, value=overrides.get(key))
        g.call('camera.release')
        if not was_loaded:
            g.call('mod.unload', id='river_water')
