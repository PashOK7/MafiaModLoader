"""Orders for people (host/orders.cpp; docs/MODDING.md "Orders for people"): a person made by spawn sent somewhere,
again (an order after an order), after an order with its own `end`, to follow the player, to stop, to shoot
another, to flee, into the player's car; a drive started with the car facing away from its way."""
from mml_game import Skip, dist


def done(g, order, since, timeout=25.0):
    ev = g.wait('ORDER_ENDED', {'id': order['id']}, since=since, timeout=timeout)
    return ev is not None


def test_orders(g):
    g.check('the player gets out of his car', g.on_foot(), g.player())
    me = g.pos('player')
    # the street beside the freeride's start: his car ahead (+z), the pavement behind
    start = [me[0] - 1.5, me[1], me[2] - 7.0]
    paulie = g.spawn_human('Paulie', start)

    # go, twice (the second from the first's end: he is ticked after an order)
    for n, to in enumerate(([me[0] - 1.5, me[1], me[2] - 17.0], [me[0] - 1.5, me[1], me[2] - 12.0])):
        seq = g.latest()
        o = g.call('human.go', actor=paulie, to=to, run=True)
        g.check('go %d: his script drives him' % n, o.get('script_drives') and o.get('running'), o)
        g.check('go %d: ORDER_ENDED' % n, done(g, o, seq), o)
        d = dist(g.pos(paulie), to)
        g.check('go %d: he stands within the radius of the place (%.2f m)' % (n, d), d < 1.5, d)

    # an order with an `end` of its own (made a jump to the loader's wait): the next order still moves him
    seq = g.latest()
    o = g.call('human.order', actor=paulie, code='enemy_stop\nend')
    g.check('an order with its own end: ORDER_ENDED', done(g, o, seq), o)
    to = [me[0] - 1.5, me[1], me[2] - 6.0]
    seq = g.latest()
    o = g.call('human.go', actor=paulie, to=to)
    g.check('after an end: ORDER_ENDED', done(g, o, seq), o)
    d = dist(g.pos(paulie), to)
    g.check('after an end: he walked there (%.2f m)' % d, d < 1.5, d)

    # follow the player, who walks away; then stop
    seq = g.latest()
    o = g.call('human.follow', actor=paulie, target='player', distance=2)
    g.sleep(6000)
    d = dist(g.pos(paulie), g.pos('player'))
    g.check('follow: he comes to the player (%.2f m)' % d, d < 3.0, d)
    g.tool('input', steps=[{'turn': 180}, {'action': 'Forwards', 'ms': 3000}, {'wait_ms': 3000}])
    d = dist(g.pos(paulie), g.pos('player'))
    g.check('follow: he keeps up with the player (%.2f m)' % d, d < 3.5, d)
    g.check('follow: it lasts', g.call('human.order_state', actor=paulie).get('running'), '')
    seq = g.latest()
    o = g.call('human.stop', actor=paulie)
    g.check('stop: ORDER_ENDED', done(g, o, seq, 10.0), o)

    # attack another person: Sam on the far side of Paulie from the player (a miss once struck the player)
    here, me = g.pos(paulie), g.pos('player')
    away = [here[0] - me[0], 0.0, here[2] - me[2]]
    n = max(1e-3, (away[0] ** 2 + away[2] ** 2) ** 0.5)
    sam = g.spawn_human('Sam', [here[0] + 8.0 * away[0] / n, here[1], here[2] + 8.0 * away[2] / n])
    g.tool('give', actor=paulie, item='colt 1911')
    seq = g.latest()
    g.call('human.attack', actor=paulie, target=sam)
    shot = g.wait('SHOT', {'actor': paulie}, since=seq, timeout=15)
    g.check('attack: he shoots', shot is not None, shot)
    # (his aim is the game's: in the labs the first hit came after 5 to 30 s, the target running)
    hit = g.wait('HUMAN_HIT', {'attacker': paulie}, since=seq, timeout=45)
    g.check('attack: he hits the target', hit is not None and hit['data']['actor'].lower() == sam.lower(), hit)

    # flee from the player
    before = dist(g.pos(paulie), g.pos('player'))
    g.call('human.flee', actor=paulie, **{'from': 'player'}, distance=15)
    g.sleep(8000)
    after = dist(g.pos(paulie), g.pos('player'))
    g.check('flee: he runs from the player (%.1f -> %.1f m)' % (before, after), after > before + 5.0, (before, after))

    # into the player's car, beside the driver's seat
    seq = g.latest()
    o = g.call('human.enter', actor=paulie, car='car', seat=1, run=True)
    sat = g.wait('VEHICLE_ENTERED', {'actor': paulie}, since=seq, timeout=40)
    g.check('enter: he gets into the car', sat is not None, o)
    g.check('enter: ORDER_ENDED', done(g, o, seq, 10.0), o)


def drive_state(g, man, timeout_ms=10000):
    """human.order_state once the game has planned the drive (its `start` is there), or the last state."""
    s = {}
    for _ in range(timeout_ms // 250):
        s = g.call('human.order_state', actor=man) or {}
        if s.get('start'):
            return s
        g.sleep(250)
    return s


def test_drive_facing_away(g):
    """A drive whose car faces away from its way's first leg: the game's reverse turns it first (`start` "turn"),
    then it goes at the road's speeds; no hard knock in its first 15 s, and it arrives. The place: a street of
    Little Italy north of the freeride's start (z 126.5, running east-west, the same in both builds), the car
    facing east, its way west to the next crossing, then north."""
    to = g.road([-2000.9, -2.7, 300.0])
    if not to:
        raise Skip('no road node north of the start')
    at = [-1962.0, -4.1, 126.5]
    car = g.tool('spawn', car='Bolt-Thrower', at=at, dir=[1, 0, 0])['actor']
    g.made.append(car)
    man = g.spawn_human('Paulie', at, **{'in': car, 'seat': 0})
    g.sleep(1500)
    seq = g.latest()
    o = g.call('human.drive', actor=man, car=car, to=to, retries=1)
    s = drive_state(g, man)
    g.check('drive: facing away from its way (%s degrees)' % s.get('start_angle'), (s.get('start_angle') or 0) > 90, s)
    g.check('drive: turned first (%s)' % s.get('start'), str(s.get('start', '')).startswith('turn'), s)
    g.sleep(15000)
    ev = g.call('events.since', seq=seq, limit=200)
    knocks = [e['data'].get('speed', 0) for e in ev.get('events', [])
              if e['type'] == 'CAR_CRASH' and str(e['data'].get('actor', '')).lower() == car.lower()]
    g.check('drive: no knock above 3 m/s in its first 15 s (%s)' % ['%.1f' % k for k in knocks],
            all(k <= 3.0 for k in knocks), knocks)
    end = g.order_end(o, seq, timeout=120)
    g.check('drive: ORDER_ENDED, arrived (%s)' % (end and end.get('start')), end is not None and end.get('arrived'), end)
