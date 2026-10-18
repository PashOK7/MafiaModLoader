"""The measuring mods' cases by the river, in the whole city (ROADMAP item 23): a car made over the river (shader
water, a body for the ragdoll), an exhaust under a roof (realistic smoke: under the elevated railway).

The places, the same in both builds: the road under the elevated railway beside Little Italy's embankment, and the
open river east of it (world.water: open, deep, its surface at -9.99). The player is taken there first."""
from mml_game import dist

START = 'city'
UNDER_EL = [-1614.0, -6.6, -24.0]   # the road under the el (its deck 5.5 m up)
RIVER = [-1570.0, -6.0, -30.0]      # open water, 45 m east of the road


def to_the_river(g):
    g.tool('teleport', actor='car', to=UNDER_EL, dir=[0, 0, 1])
    g.sleep(1500)


def test_spawn_over_water(g):
    """A car made over the open river stays where it was asked (the water is no ground for spawn, and no calming
    drops it through the river in one call). That it then falls in is not checked: the game keeps it asleep (act
    state 1; ROADMAP Known gaps)."""
    to_the_river(g)
    w = g.call('world.water', at=RIVER)
    g.check('world.water: the open river (%.2f)' % w.get('height', 0),
            w.get('water') and not w.get('covered') and w.get('deep') and abs(w.get('height', 0) + 9.99) < 0.05, w)
    above = [RIVER[0], -4.0, RIVER[2]]
    car = g.tool('spawn', car='Bolt-Thrower', at=above)
    g.made.append(car['actor'])
    g.check('spawn over the water: where it was asked (y %.1f)' % car['pos'][1], abs(car['pos'][1] + 4.0) < 0.5, car)
    g.sleep(1000)
    y = g.pos(car['actor'])[1]
    g.check('and not flung away after it (y %.1f)' % y, -12.0 < y < 0.0, y)


def test_exhaust_under_roof(g):
    """The car's exhaust: behind it, pointing back, emitting while the engine runs; a ray up from the pipe meets
    nothing in the open street and the railway's deck under the el."""
    def pipe():
        ex = g.call('exhaust.state')['car']
        return ex, ex['pipes'][0] if ex.get('pipes') else None

    ex, p = pipe()
    for _ in range(10):   # (right after the start the engine has not begun to smoke: seen, both builds)
        if ex.get('emitting'):
            break
        g.sleep(500)
        ex, p = pipe()
    g.check('the exhaust: a pipe, emitting', p is not None and ex.get('emitting'), ex)
    if not p:
        return
    up = g.call('world.ray', **{'from': p['pos']}, dir=[0, 1, 0], length=8, what='static')
    g.check('the open street: nothing over the pipe within 8 m', not up.get('hit'), up)
    to_the_river(g)
    ex, p = pipe()
    car = g.pos('car')   # (turned by the teleport to face +z)
    back = [p['pos'][i] - car[i] for i in range(3)]
    g.check('the pipe is behind the car (z %.2f)' % back[2], back[2] < -0.5, (p, car))
    g.check('it points back (dir z %.2f)' % p['dir'][2], p['dir'][2] < -0.9, p)
    up = g.call('world.ray', **{'from': p['pos']}, dir=[0, 1, 0], length=8, what='static')
    g.check('under the el: the deck over the pipe (%.1f m)' % up.get('distance', -1), up.get('hit') and up['distance'] < 7.0, up)
    g.check('still emitting there', ex.get('emitting'), ex)
    g.check('the car is where it was put (%.1f m)' % dist(car, UNDER_EL), dist(car, UNDER_EL) < 2.0, car)
