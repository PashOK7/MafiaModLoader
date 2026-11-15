"""Effective projections, rather than FOV fields, agree across native/mod cameras and water."""
import math
import time

from test_window import settled, picture

START = 'city'


def test_fusion_projection_interop(g):
    loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
    if not {'fusion_fix', 'devtools', 'river_water', 'kit'} <= loaded:
        from mml_game import Skip
        raise Skip('load fusion_fix, devtools, river_water and the kit projection observer')
    original = g.call('window.state')['render']
    old = g.call('config.get', mod='devtools')['changes']
    eye = [-1336.8252, -4.9427, 198.3002]
    direction = [-.75505, -.17512, -.63186]

    def config(key, value):
        g.call('config.set', mod='devtools', key=key, value=value)

    def frames(view=False):
        before = g.call('kit.projection')
        until = time.monotonic()+20
        while time.monotonic() < until:
            sample = g.call('kit.projection')
            if (sample['main']['frames'] > before['main']['frames']+2 and
                    (not view or sample['view']['frames'] > before['view']['frames']+2)):
                return sample
            time.sleep(.05)
        raise RuntimeError('no fresh main/reflection projection samples')

    def close(a, b):
        return abs(a-b) < max(abs(b)*2e-4, 2e-4)

    def reflection(label):
        sample = frames(True)
        water = g.call('river_water.status')
        advertised = water['reflection_matrices'][16:]
        actual = sample['view']['matrix']
        main = sample['main']['matrix']
        g.check(label+' actual reflection matches its advertised matrix',
                all(close(actual[i], advertised[i]) for i in (0,5,8,9,10,11,14)),
                {'actual': actual, 'advertised': advertised})
        g.check(label+' reflection inherits the main frustum',
                all(close(actual[i], main[i]) for i in (0,5,8,9)), sample)
        g.check(label+' reflection owns its exact projection', sample['view']['ownership'] >= 0 and sample['view']['ownership'] & 2, sample)
        g.check(label+' no reflection fault',
                all(not v['faults'] for v in g.call('view.state')['views']))
        return sample

    try:
        config('freecam', False)
        g.call('camera.release')
        for w,h in [(1280,960), (1440,810), (2400,900)]:
            g.call('window.resize', width=w, height=h)
            settled(g, (w,h))
            for fov in (60,80,110):
                config('fov', fov)
                sample = frames()
                p = sample['main']['matrix']
                x = 1/math.tan(math.radians(fov)/2)
                g.check('native effective FOV %s at %sx%s' % (fov,w,h),
                        close(p[0],x) and close(p[5],x*w/h), sample)
                g.check('native absolute FOV ownership', sample['main']['ownership']==1, sample)
            config('fov', 0)
            sample = frames()
            native = g.call('camera.game')['fov']
            expected = 1/(math.tan(native/2)*(w/h)/(4/3))
            g.check('native Hor+ restored at %sx%s' % (w,h), close(sample['main']['matrix'][0],expected), sample)
            config('fov', 80)
            g.call('camera.set', eye=eye, dir=direction, ms=120000)
            reflection('inherited %sx%s' % (w,h))
            g.call('camera.set', eye=eye, dir=direction, fov=40, ms=120000)
            sample = reflection('explicit %sx%s' % (w,h))
            g.check('explicit 40 degree vertical FOV is preserved',
                    close(sample['main']['matrix'][5],1/math.tan(math.radians(20))), sample)
            picture(g,w,h,'projection interop %sx%s' % (w,h))
            g.call('camera.release')

        g.call('camera.set', eye=eye, dir=direction, fov=40, ms=120000)
        g.call('mod.reload', id='fusion_fix')
        reflection('Fusion Fix reloaded')
        g.call('mod.unload', id='fusion_fix')
        sample = reflection('Fusion Fix unloaded')
        g.check('explicit FOV survives Fusion Fix unload',
                close(sample['main']['matrix'][5],1/math.tan(math.radians(20))), sample)
        g.call('mod.load', id='fusion_fix')
        reflection('Fusion Fix loaded again')
        g.call('camera.release')
        g.tool('restore', save='projection-interop')
        g.tool('restore', load='projection-interop')
        sample = frames()
        g.check('absolute FOV survives a native save/load',
                close(sample['main']['matrix'][0],1/math.tan(math.radians(40))) and
                sample['main']['ownership']==1, sample)
        g.call('mod.unload', id='devtools')
        sample = frames()
        g.check('unloading FOV owner restores native projection ownership', sample['main']['ownership']==0, sample)
        native = g.call('camera.game')['fov']
        size = g.call('window.state')['render']
        expected = 1/(math.tan(native/2)*(size[0]/size[1])/(4/3))
        g.check('unloading FOV owner restores effective Hor+',close(sample['main']['matrix'][0],expected),sample)
        g.call('mod.load', id='devtools')
        config('freecam', True)
        sample = frames()
        g.check('devtools freecam owns its exact frustum', sample['main']['ownership'] >= 0 and sample['main']['ownership'] & 2, sample)
    finally:
        g.call('camera.release')
        for key in ('fov', 'freecam'):
            config(key, old.get(key))
        g.call('window.resize', width=original[0],height=original[1])
        settled(g, original)
