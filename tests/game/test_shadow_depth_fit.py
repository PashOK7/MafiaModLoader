"""Current opaque-depth fitting, including moving camera and restored HUD's viewport/cache."""
START = 'city'


def test_current_depth_shadows(g):
    old = g.call('shader.state')['shadows']['depth_fit']['enabled']
    poses = [([-1985,-2,28],[1,.03,0]),
             ([-1407.48035,7.42366,-798.74927],[.70615917,.12939537,.69612938]),
             ([-1490,30,20],[-100,-40,-45])]
    try:
        g.call('shader.shadow_depth', on=True)
        for eye,direction in poses:
            g.call('camera.set', eye=eye, dir=direction, ms=60000)
            for _ in range(40):
                g.tool('screenshot', max_width=160)
                ready = g.call('shader.state')['shadows']['depth_fit']
                if ready['used'] and not ready['pending']:
                    break
            s = g.call('shader.state')['shadows']
            g.check('current depth used after camera change', s['depth_fit']['used'] and not s['depth_fit']['pending'], s)
            g.check('no stale bounds or fallback after camera change', s['depth_fit']['fallbacks']==0, s)
            g.check('current fitted map preserves receiver density', not s['drawn'] or
                    (s['coverage']['texel_m']<=240/2048+1e-5 and not s['coverage']['density_limited']), s)
        gfx = g.call('gfx.state')
        g.check('late shadows preserve graphics bridge', gfx['state']=='on', gfx)
        g.check('late shadows preserve main viewport', g.call('render.state')['camera']['viewport']==gfx['size'])
        dlss = g.call('dlss_nr.state')
        g.check('DLSS still draws after late shadow cameras', dlss['drawn']>0 and not dlss['last_error'], dlss)
    finally:
        g.call('shader.shadow_depth', on=old)
        g.call('camera.release')
