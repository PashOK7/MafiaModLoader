"""A pedestrian's retained LS3D model name resolves to its archived skeleton."""


def test_pedestrian_model_alias(g):
    expected = g.call('model.info', file='Models/civil31.4ds', meshes=False)
    actual = g.call('model.info', file='Models/civil31.I3d!', meshes=False)
    g.check('marked LS3D name opens the same nonempty rest skeleton as the .4ds',
            bool(expected['nodes']) and actual == expected)
