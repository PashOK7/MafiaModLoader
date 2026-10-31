"""Far-view finite water coverage; native shadows stay before the main Render and preserve live cut-outs."""
START = 'city'


def test_shadow_surface_fit(g):
    old = g.call('shader.state')['shadows']['coverage']['surface_fit']
    try:
        g.call('camera.set', eye=[-1407.48035,7.42366,-798.74927],
               dir=[.70615917,.12939537,.69612938], ms=60000)
        g.call('shader.shadow_surface', on=False)
        for _ in range(3):
            g.tool('screenshot', max_width=160)
        a = g.call('shader.state')['shadows']
        g.call('shader.shadow_surface', on=True)
        for _ in range(3):
            g.tool('screenshot', max_width=160)
        b = g.call('shader.state')['shadows']
        g.check('finite surface uses copied receiver geometry', b['coverage']['receiver_triangles'] > 0, b)
        g.check('far-view box removes empty city area',
                all(x <= y for x,y in zip(b['coverage']['half'], a['coverage']['half'])), [a,b])
        g.check('far-view map retains or improves density', b['coverage']['texel_m'] <= a['coverage']['texel_m'], [a,b])
        g.check('current alpha-aware game casters retained', b['drawn'] and b['casters_by']=='game' and b['cut_out']>0, b)
        g.check('infinite draw distance retained', g.call('render.state')['camera']['far']>4900)
    finally:
        g.call('shader.shadow_surface', on=old)
        g.call('camera.release')
