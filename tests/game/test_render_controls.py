"""Saved distance/occluder/fog controls, native streaming and water's extended foam range."""
START = 'city'


def test_render_controls(g):
    mods=g.call('mods.list')
    if isinstance(mods,dict):
        mods=mods['mods']
    atmosphere=any(m['id']=='atmosphere' and m.get('loaded') for m in mods)
    atmosphereOn=g.call('config.get',mod='atmosphere')['changes'].get('on') if atmosphere else None
    if atmosphere:
        g.call('config.set',mod='atmosphere',key='on',value=False)
    before = g.call('config.get', mod='loader')['changes'].get('render', {})
    state = g.call('render.state')
    foamBefore = g.call('config.get', mod='river_water')['changes'].get('foam')
    keys = ['distance_mode', 'distance', 'occlusion', 'fog_mode', 'fog_start', 'fog_span']

    def setting(key, value):
        g.call('config.set', mod='loader', key='render.' + key, value=value)

    def settled():
        g.sleep(250)
        g.tool('screenshot', max_width=160)
        return g.call('render.state')

    try:
        g.call('camera.set', eye=[-1490, 40, 20], dir=[-100, -20, -45], ms=120000)
        setting('distance', 100)
        setting('distance_mode', 1)
        setting('occlusion', True)
        near = settled()
        g.check('custom distance applies to freecam projection', abs(near['camera']['far'] - 100) < 1, near)
        g.check('custom distance unloads distant city blocks', 0 < near['city']['active'] < near['city']['blocks'], near['city'])
        g.check('custom distance preserves requested occluders', near['ls3d']['occlusion'])
        setting('distance', 1000)
        far = settled()
        g.check('increasing distance loads additional blocks', far['city']['active'] > near['city']['active'], far['city'])
        g.check('custom distance follows slider live', abs(far['camera']['far'] - 1000) < 10, far['camera'])
        setting('distance_mode', 2)
        whole = settled()
        g.check('whole-city mode activates all cache blocks', whole['city']['active'] == whole['city']['blocks'], whole['city'])
        g.check('whole-city mode leaves occluders independent', whole['ls3d']['occlusion'])
        setting('occlusion', False)
        g.check('occluders switch off independently', not settled()['ls3d']['occlusion'])
        setting('distance_mode', 1)
        setting('distance', 100)
        g.check('shrinking whole city releases blocks', settled()['city']['active'] < whole['city']['active'])
        setting('distance_mode', 0)
        native = settled()
        g.check('game mode restores native camera range', abs(native['camera']['far'] - native['city']['game_far']) < 2, native)
        g.check('game mode preserves occluder choice', not native['ls3d']['occlusion'])

        setting('distance_mode', 2)
        setting('fog_start', 50)
        setting('fog_span', 200)
        setting('fog_mode', 2)
        custom = settled()
        g.check('custom fog uses start plus fade distance', custom['fog']['mode'] == 2 and
                custom['fog']['start'] == 50 and custom['fog']['end'] == 250, custom['fog'])
        g.check('native fog setter is reached', custom['fog']['hardware_calls'] > 0, custom['fog'])
        fog = g.call('gfx.state')['world']['fog']
        g.check('water bridge receives custom native fog', fog['on'] and fog['start'] == 50 and fog['end'] == 250, fog)
        setting('fog_mode', 1)
        g.check('native fog can be disabled', settled()['fog']['mode'] == 1)
        g.check('water bridge sees fog disabled', not g.call('gfx.state')['world']['fog']['on'])
        setting('fog_mode', 0)
        g.check('native fog can be restored', settled()['fog']['mode'] == 0)
        g.check('settings are stored for restart', g.call('config.get', mod='loader')['changes']['render']['distance'] == 100)
        gfx = g.call('gfx.state')
        g.check('graphics bridge remains intact', gfx['state'] == 'on')
        if gfx['upscale']['mod'] == 'dlss_nr':
            dlss = g.call('dlss_nr.state')
            g.check('DLSS still produces frames', dlss['drawn'] > 0 and not dlss['last_error'], dlss)
        water = g.call('panel.state')['mods']
        g.check('native settings expose loader and water pages', any(m['mod'] == 'loader' for m in water) and
                any(m['mod'] == 'river_water' for m in water))
        for value, shown in [(1, '50%'), (2, '100%')]:
            g.call('config.set', mod='river_water', key='foam', value=value)
            settled()
            foam = next(c for m in g.call('panel.state')['mods'] if m['mod'] == 'river_water'
                        for c in m['controls'] if c['key'] == 'foam')
            g.check('foam multiplier %s displays %s' % (value, shown), foam['max'] == 2 and foam['shown'] == shown, foam)
    finally:
        g.call('config.set', mod='river_water', key='foam', value=foamBefore)
        for key in keys:
            setting(key, before.get(key))
        settled()
        g.call('render.set', distance_mode=state['city']['distance_mode'], far=state['city']['far'],
               occlusion=state['ls3d']['occlusion'], fog_mode=state['fog']['mode'],
               fog_start=state['fog']['start'], fog_end=state['fog']['end'])
        g.call('camera.release')
        if atmosphere:
            g.call('config.set',mod='atmosphere',key='on',value=atmosphereOn)
