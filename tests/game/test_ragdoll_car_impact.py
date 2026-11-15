"""A native car hit supplies no canned velocity and uses its real collider."""
import math
import time


def test_car_hit_uses_collision_only(g):
    loaded = {m['id'] for m in g.call('mods.list') if m['loaded']}
    for mod in ('ragdoll', 'kit'):
        if mod not in loaded:
            g.call('mod.load', id=mod)
            g.later('mod.unload', id=mod)
    player = g.pos('player')
    actor = g.spawn_human('civil15', [player[0]+7, player[1]+.1, player[2]+5])
    g.call('human.anim', actor=actor, play='stand')
    g.sleep(250)
    detached = g.call('kit.detached_player_car', target=actor)
    g.check('occupied vehicle remains discoverable outside the general lists', detached['listed'] == 1, detached)
    g.check('native contact accepts the occupied vehicle outside the lists', detached['contact'], detached)
    bodies = g.call('ragdoll.state')['ragdolls']
    body = next((b for b in bodies if b['actor'] == actor), {})
    g.check('native vehicle hit creates a physical vehicle-impact body', body.get('vehicle_impact'), body)
    g.check('remote car contact never supplies the old launch velocity', body.get('fastest_part', 100) < 5, body)
    car = g.call('ragdoll.vehicle')
    g.check('striking vehicle has its actual physical shape', 'has no body' not in str(car) and 'car' in str(car), car)


def test_hidden_truck_physically_hits_body(g):
    mods = g.call('mods.list')
    if any(m['id'] == 'ragdoll' and m['loaded'] for m in mods):
        g.call('mod.unload', id='ragdoll')
    previous_car = g.player()['vehicle']['actor']
    ground = g.call('world.ground', at=[-1981, 0, 75])['height']
    car = g.call('world.spawn', car=61, at=[-1981, ground+.1, 75], dir=[0,0,1])['actor']
    g.made.append(car)
    g.call('world.seat', actor='player', car=car, seat=0)
    g.later('world.seat', actor='player', car=previous_car, seat=0)
    g.call('camera.game', mode=12)
    g.later('camera.game', mode=7)
    g.sleep(150)
    hidden = g.call('actor.meshes', actor=car)
    physical = g.call('actor.meshes', actor=car, hidden_root=True)
    g.check('bumper camera hides the root but keeps actual truck geometry',
            hidden['pieces'] == 0 and physical['triangles'] > 1000, physical['triangles'])
    g.call('mod.load', id='ragdoll')
    g.later('mod.unload', id='ragdoll')
    actor = g.spawn_human('civil15', [-1981, ground+.02, 82])
    g.call('ragdoll.start', actor=actor)
    g.call('human.hit', actor=actor, type=0, dir=[0,0,0], damage=1000)
    g.sleep(2500)
    body = next(b for b in g.call('ragdoll.state')['ragdolls'] if b['actor'] == actor)
    g.tool('teleport', actor=car, to=[body['pelvis'][0],ground+.1,body['pelvis'][2]-5], dir=[0,0,1])
    g.sleep(150)
    g.call('vehicle.move', car=car, velocity=[0,0,6], ms=1500)
    contact = False
    distance = fastest = 0
    for _ in range(24):
        time.sleep(.045)
        report = g.call('ragdoll.inspect', actor=actor)
        current = next(b for b in g.call('ragdoll.state')['ragdolls'] if b['actor'] == actor)
        contact |= ('car ' + car[2:].lstrip('0').lower()) in str(report).lower()
        distance = max(distance, math.dist(current['pelvis'], body['pelvis']))
        fastest = max(fastest, current['fastest_part'])
    g.check('true truck/body contacts supply the physical impact', contact, {'distance':distance,'speed':fastest})
    g.check('the mesh collision moves the body without a native living-hit event', distance > .5 and fastest > 1,
            {'distance':distance,'speed':fastest})
