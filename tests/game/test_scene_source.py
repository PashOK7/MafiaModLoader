"""Native geometry/material source, C/C++/Rust borrows and guarded owner lifecycle. Both builds."""
import time
from mml_game import Skip

START = 'city'


def test_scene_draw_source(g):
    loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
    if not {'kit', 'kit_rs'} <= loaded:
        raise Skip('load C/C++ and Rust kit probes')

    def until(method, predicate):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            value = g.call(method)
            if predicate(value):
                return value
            time.sleep(.05)
        raise RuntimeError('native source did not settle: '+str(value))

    def advance():
        frame = g.call('ping')['frames']
        until('ping', lambda s: s['frames'] >= frame+3)

    try:
        initial = g.call('kit.scene_source', on=True)
        g.check('native source ABI is supported', initial['supported'])
        g.call('kit_rs.scene_source', on=True)
        source = until('kit.scene_source', lambda s: s['copies'] >= 64 and s['max_triangles'] > 2)
        rust = until('kit_rs.scene_source', lambda s: s['copies'] > 0)
        g.check('C/C++ copy real triangles with finite positions/UV and valid indices',
                source['invalid'] == source['c_fail'] == 0 and source['c_ok'] >= source['copies'], source)
        g.check('source materials and world transforms match actual bound D3D9 state',
                source['state_checks'] > 0 and source['detail_checks'] > 0 and source['state_errors'] == 0, source)
        g.check('native source carries textured geometry and backdrop classification',
                source['textured'] > 0 and source['uv'] > 0 and source['backdrop'] > 0, source)
        textures = until('kit.scene_source', lambda s: s['texture_gpu'] >= 8 and s['texture_gpu'] == s['texture_ok'])
        g.check('native material texels and every mip survive exact D3D9 to D3D11 transfer',
                textures['texture_errors'] == 0 and textures['texture_bytes'] > 0 and
                textures['texture_mips'] > textures['texture_gpu'], textures)
        g.check('undersized and expired callback borrows are rejected',
                source['refused_size'] >= source['copies'] and source['stale_refused'], source)
        rust_loaded = next(m for m in g.call('mods.list') if m['id'] == 'kit_rs')['loaded']
        g.check('safe Rust owns valid native vertex/index copies (guarded assertions)',
                rust['copies'] > 0 and rust_loaded, rust)
        stopped = g.call('kit.scene_source', on=False)
        advance()
        g.check('unregister stops callbacks while another owner remains',
                g.call('kit.scene_source')['callbacks'] == stopped['callbacks'])
        g.call('kit_rs.scene_source', on=False)
        g.call('kit.scene_source', on=False)
        g.call('kit.scene_source', on=True, self_remove=True)
        removed = until('kit.scene_source', lambda s: not s['on'])
        advance()
        g.check('observer can unregister itself and duplicate removal is harmless',
                g.call('kit.scene_source')['callbacks'] == removed['callbacks'])
        g.call('kit.scene_source', on=True)
        until('kit.scene_source', lambda s: s['copies'] > 0)
        g.call('mod.reload', id='kit')
        advance()
        g.check('reload removes callback and borrowed geometry of preceding generation',
                g.call('kit.scene_source')['callbacks'] == 0)
        g.call('kit.scene_source', on=True, fault=True)
        until('mods.list', lambda rows: not next(m for m in rows if m['id'] == 'kit')['loaded'])
        advance()
        g.check('observer fault unloads its owner and game keeps rendering', g.call('ping')['frames'] > 0)
        g.call('mod.load', id='kit')
        g.call('kit.scene_source', on=True)
        recovered = until('kit.scene_source', lambda s: s['copies'] > 0)
        g.check('source can be reacquired after fault cleanup', recovered['invalid'] == 0)
    finally:
        loaded = {m['id'] for m in g.call('mods.list') if m.get('loaded')}
        if 'kit' in loaded:
            g.call('kit.scene_source', on=False)
        if 'kit_rs' in loaded:
            g.call('kit_rs.scene_source', on=False)
