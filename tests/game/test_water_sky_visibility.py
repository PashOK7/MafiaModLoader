"""Real water/sky resources and conservative offscreen rejection, without changing material settings."""
from mml_game import Skip

START = 'city'


def test_painted_sky_visibility(g):
    cfg = g.call('config.get', mod='river_water')['value']
    if not (cfg['on'] and cfg['reflection'] and cfg['glitter'] > 0):
        raise Skip('needs enabled water/reflection and nonzero sky glitter')
    if g.call('gfx.state')['state'] != 'on':
        raise Skip('needs the graphics bridge')

    def barrier(n=1):
        for _ in range(n):
            g.tool('screenshot', max_width=160)

    def visible():
        g.call('camera.set', eye=[-1490,30,20], at=[-1590,-10,-25], ms=60000)

    try:
        visible()
        barrier(5)
        a = g.call('river_water.status')
        g.check('painted sky HDR resources active', a['painted_sky']['ready'] and a['painted_sky']['frames'] > 0, a)
        g.check('visible water has a fresh reflection', a['reflection']['plane'] >= 0 and a['drawn'] > 0, a)
        g.call('camera.set', eye=[-1490,30,20], dir=[0,.99,.1], ms=60000)
        barrier(3)
        a = g.call('river_water.status')
        barrier(3)
        b = g.call('river_water.status')
        g.check('offscreen water skips the complete surface pass',
                b['drawn'] == a['drawn'] and b['visibility']['surface_skipped'] > a['visibility']['surface_skipped'], b)
        g.check('offscreen water skips scene reflection',
                b['reflection']['drawn'] == a['reflection']['drawn'] and b['reflection']['plane'] == -1, b)
        visible()
        barrier()
        c = g.call('river_water.status')
        g.check('reflection returns immediately after camera movement',
                c['reflection']['drawn'] > b['reflection']['drawn'] and c['reflection']['plane'] >= 0, c)
        barrier(3)
        d = g.call('river_water.status')
        g.check('surface and painted sky recover after rejection',
                d['drawn'] > b['drawn'] and d['painted_sky']['ready'], d)
        dlss = next((m for m in g.call('mods.list') if m['id'] == 'dlss_nr' and m.get('loaded')), None)
        if dlss and g.call('dlss_nr.state')['on']:
            s = g.call('dlss_nr.state')
            g.check('DLSS retains valid inputs after offscreen recovery',
                    not s['last_error'] and s['drawn'] > 0 and d['temporal']['history'], s)
    finally:
        g.call('camera.release')
