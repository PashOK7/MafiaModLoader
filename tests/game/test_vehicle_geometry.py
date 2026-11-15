"""Physical vehicle geometry remains available while its model root is hidden by the camera."""


def test_hidden_root_meshes(g):
    loaded = {mod['id'] for mod in g.call('mods.list') if mod['loaded']}
    if 'kit' not in loaded:
        g.call('mod.load', id='kit')
        g.later('mod.unload', id='kit')
    result = g.call('kit.hidden_root_meshes')
    g.check('visible vehicle has native mesh pieces', result['shown'] > 0, result)
    g.check('ordinary mesh enumeration respects hidden root', result['hidden'] == 0, result)
    g.check('physical enumeration retains every visible child while root is hidden',
            result['physical'] == result['shown'], result)
    g.check('visibility state is restored and invalid flags refused', result['restored'] and result['invalid'] == -1, result)
