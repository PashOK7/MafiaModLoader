"""Physical-pose handover: two sides, displaced pelvis, pause and re-knockdown cancellation.

The pelvis starts three metres away from the actor's stored position. Testing a standing actor at its old
origin would miss the original bug. No ragdoll mod is needed for the shared animation service cases.
"""
import math
import struct
import time
from uuid import uuid4

from mml_game import Skip


def pose_body(g, actor, side):
    q = math.sqrt(0.5)
    g.call('human.pose', actor=actor, bones={'pelvis': {'rot': [q, side * q, 0, 0], 'pos': [3, 0.22, 1]}}, ms=2000)
    g.sleep(80)
    g.call('human.pose', actor=actor, clear=True)


def native_blast(g, point):
    mission_slot, explosion = {'1.0': (0x65115C, 0x5E6760), '1.2': (0x63788C, 0x5AAE10)}[g.build]

    def pointer(addr):
        return struct.unpack('<I', bytes.fromhex(g.call('mem.read', addr=hex(addr), size=4)['hex']))[0]

    result = g.call('native.call', at=hex(explosion), conv='thiscall', ret='void',
                    **{'this': hex(pointer(pointer(mission_slot) + 0x24))},
                    args=[0, {'buf': 12, 'hex': struct.pack('<3f', *point).hex()},
                          {'float': 10}, {'float': 200}, 1, 0, 1, 0x12])
    assert result['stack']['ok'], result


def test_carried_player_blast_getup(g):
    """The native mission transfer keeps Tommy's C_human alive without calling its constructor again."""
    mod = next((m for m in g.call('mods.list') if m['id'] == 'ragdoll'), None)
    if mod is None:
        raise Skip('ragdoll port is not installed')
    if not mod['loaded']:
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    assert g.on_foot(), 'Tommy did not exit the starting car'
    player = g.call('human.skeleton', actor='player')['actor']
    at = g.pos('player')
    old_actor = g.spawn_human('Paulie', [at[0] + 1.5, at[1], at[2]])
    seq = g.latest()
    g.call('script.run', owner=old_actor, code='change_mission "freeitaly", "start", 0\nend')
    assert g.wait('MISSION_LOADED', {'name': 'freeitaly'}, since=seq, timeout=20), 'mission did not reopen'
    g.made.remove(old_actor)
    humans = g.call('humans.list')['humans']
    g.check('carried Tommy is still registered exactly once',
            [h['actor'] for h in humans if h['player']] == [player],
            [h for h in humans if h['player']])
    events = g.call('events.since', seq=seq, limit=1000, types='HUMAN_CREATED,ACTOR_DESTROYED')['events']
    g.check('the mission destroyed the old NPC and did not recreate Tommy',
            any(e['type'] == 'ACTOR_DESTROYED' and e['data']['actor'] == old_actor for e in events) and
            not any(e['data']['actor'] == player for e in events), (old_actor, player))
    g.check('skeleton resolves for the same player after the scene change',
            g.call('human.skeleton', actor='player')['actor'] == player)
    at = g.pos('player')
    facing = g.player()['facing']
    point = [at[0] + 4 * facing[0], at[1] + .85, at[2] + 4 * facing[2]]
    bystander = g.spawn_human('Paulie', [point[0] + 7, at[1], point[2]])
    g.sleep(120)
    seq = g.latest()
    native_blast(g, point)
    assert g.wait('EXPLOSION', since=seq, timeout=5), 'native explosion did not fire'
    initial = g.call('ragdoll.state')['ragdolls']
    g.check('the native blast knocks Tommy down alive',
            any(b['actor'] == player and b['knockdown'] for b in initial), initial)
    event = g.wait('HUMAN_HIT', {'actor': bystander}, since=seq, timeout=3)
    g.check('a surviving NPC takes the native blast damage without a new physical knockdown',
            event and event['data']['alive'] and event['data']['damage'] > 0 and
            not any(b['actor'] == bystander for b in initial), (event, initial))
    played = fitted = False
    trace = []
    deadline = time.monotonic() + 13
    while time.monotonic() < deadline:
        state = g.call('human.anim_state', actor='player')
        played |= bool(state.get('blending') and state.get('playing') in (111, 338))
        fitted |= bool((state.get('handover') or {}).get('fitted'))
        marker = (state.get('state'), state.get('playing'), state.get('blending'), fitted)
        if not trace or trace[-1] != marker:
            trace.append(marker)
        if played and fitted and not state.get('blending') and state.get('state') == 1:
            break
        time.sleep(.05)
    g.check('the real get-up track plays and fits after the blast', played and fitted, trace)
    g.check('the completed get-up returns Tommy to the native controller',
            played and not state.get('blending') and state.get('state') == 1, trace)
    g.log('blast get-up transitions: %s' % trace)


def test_weak_blast_keeps_native_player_reaction(g):
    mod = next((m for m in g.call('mods.list') if m['id'] == 'ragdoll'), None)
    if mod is None:
        raise Skip('ragdoll port is not installed')
    if not mod['loaded']:
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    assert g.on_foot(), 'Tommy did not exit the starting car'
    for distance, low, high in [(8.6, 0, 50), (6.7, 50, 100)]:
        before = g.player()
        x, y, z = before['pos']
        seq = g.latest()
        native_blast(g, [x, y + .85, z + distance])
        event = g.wait('HUMAN_HIT', {'actor': before['actor'], 'hit_type': 2}, since=seq, timeout=3)
        assert event, 'no native explosion hit'
        damage = event['data']['damage']
        after = g.player()
        anim = g.call('human.anim_state', actor='player')
        bodies = g.call('ragdoll.state')['ragdolls']
        g.check('blast at %.1f m keeps native damage without activating physics' % distance,
                low < damage < high and abs(before['health'] - after['health'] - damage) < .01 and
                not any(b['actor'] == before['actor'] for b in bodies), (event, after, bodies))
        if low == 0:
            g.check('the distant blast leaves Tommy standing', anim['state'] == 1, anim)
        g.sleep(350)


def test_displaced_body_both_sides(g):
    for side in (-1, 1):
        actor = g.spawn_human('Paulie', [-1981, -4.7, 28 + side * 3])
        g.sleep(100)
        pose_body(g, actor, side)
        result = g.call('human.get_up', actor=actor, blend_ms=600)
        g.check('native get-up queued', result.get('from') in ('back', 'front'), result)
        g.sleep(450)
        state = g.call('human.anim_state', actor=actor)
        handover = state.get('handover') or {}
        g.check('actual animation fitted onto displaced physical pelvis',
                handover.get('fitted') and handover.get('fit_error_m', 100) < 0.015, state)
        g.sleep(2600)
        state = g.call('human.anim_state', actor=actor)
        g.check('native actor resumes, handover releases', not state.get('blending') and state.get('state') == 1, state)


def test_same_mission_load_releases_ragdoll(g):
    mod = next((m for m in g.call('mods.list') if m['id'] == 'ragdoll'), None)
    if mod is None:
        raise Skip('ragdoll port is not installed')
    if not mod['loaded']:
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    assert g.on_foot(), 'Tommy did not exit the starting car'
    before = g.player()
    name = 'ragdoll_load_' + uuid4().hex
    g.tool('restore', save=name)
    g.later('restore.delete', name=name)
    x, y, z = before['pos']
    g.call('world.teleport', actor='player', to=[x + 4, y, z])
    g.sleep(140)
    g.call('ragdoll.start', actor=before['actor'])
    g.sleep(140)
    bodies = g.call('ragdoll.state')['ragdolls']
    assert any(b['actor'] == before['actor'] for b in bodies), bodies
    assert math.dist(g.player()['pos'], before['pos']) > 2, 'the physical pose did not leave the saved position'
    seq = g.latest()
    g.tool('restore', load=name)
    g.sleep(250)
    after = g.player()
    events = g.call('events.since', seq=seq, types='MISSION_CLOSE,MISSION_OPEN,GAME_LOADED')['events']
    g.check('same-mission load reuses the live actor and restores the saved position',
            after['actor'] == before['actor'] and math.dist(after['pos'], before['pos']) < .2 and
            [e['type'] for e in events] == ['GAME_LOADED'], (before, after, events))
    bodies = g.call('ragdoll.state')['ragdolls']
    g.check('pre-load ragdoll and camera release', not bodies and
            not g.call('ragdoll.camera')['overriding'], bodies)


def test_pause_does_not_finish_handover(g):
    actor = g.spawn_human('Paulie', [-1981, -4.7, 35])
    g.sleep(100)
    pose_body(g, actor, 1)
    g.call('human.get_up', actor=actor, blend_ms=600)
    g.call('state.time', scale=0)
    try:
        before = g.call('human.anim_state', actor=actor)
        # Wall-clock delay while mission time is held: input.wait would itself wait on mission time.
        import time
        time.sleep(0.35)
        after = g.call('human.anim_state', actor=actor)
        g.check('pause preserves the staged physical pose', after.get('blending') and
                after.get('handover', {}).get('elapsed_ms') == before.get('handover', {}).get('elapsed_ms'), after)
    finally:
        g.call('state.time', scale=1)


def test_ragdoll_sdk_handover_and_cancel(g):
    """The real mod supplies a current physics pose, cancels on re-knockdown, releases on unload."""
    g.call('mod.load', id='ragdoll')
    g.later('mod.unload', id='ragdoll')
    actor = g.spawn_human('Paulie', [-1981, -4.7, 30])
    g.sleep(100)
    g.call('ragdoll.start', actor=actor)
    g.sleep(1700)
    g.call('ragdoll.drop', actor=actor)
    g.sleep(400)
    state = g.call('human.anim_state', actor=actor)
    handover = state.get('handover') or {}
    g.check('current Jolt pose is accepted and fitted', handover.get('fitted') and
            handover.get('fit_error_m', 100) < 0.015, state)
    g.call('ragdoll.start', actor=actor)
    state = g.call('human.anim_state', actor=actor)
    g.check('another knockdown cancels the old host handover', not state.get('blending'), state)
    g.sleep(500)
    g.call('ragdoll.drop', actor=actor)
    g.sleep(100)
    g.call('mod.unload', id='ragdoll')
    state = g.call('human.anim_state', actor=actor)
    g.check('owner unload releases the handover without stopping the native animation',
            not state.get('blending') and state.get('playing') in (111, 338), state)


def test_pedestrian_physical_handover(g):
    import struct
    peds = g.call('pedestrians.list')['pedestrians']
    peds = [p for p in peds if p.get('alive') and p.get('spawned')]
    if not peds:
        from mml_game import Skip
        raise Skip('this case needs a live traffic pedestrian')
    ped = peds[0]['element']
    g.call('mod.load', id='ragdoll')
    g.later('mod.unload', id='ragdoll')
    g.call('ragdoll.start', pedestrian=ped)
    g.sleep(1700)
    g.call('ragdoll.drop', pedestrian=ped)
    g.sleep(450)
    state = g.call('human.anim_state', pedestrian=ped)
    handover = state.get('handover') or {}
    g.check('traffic pedestrian accepts current physics pose', handover.get('fitted') and
            handover.get('fit_error_m', 100) < 0.015, state)
    raw = g.tool('mem_read', addr=ped, size=0x200)
    data = bytes.fromhex(raw['hex'])
    cell = struct.unpack_from('<I', data, 0xD0)[0]
    g.check('getting-up pedestrian is in the collision grid', data[0x150] != 0 and cell != 0, hex(cell))
    g.sleep(3000)
    state = g.call('human.anim_state', pedestrian=ped)
    g.check('pedestrian returns to walking', not state.get('blending') and state.get('state') == 0, state)


def test_car_touch_respects_native_knockdown_threshold(g):
    g.call('mod.load', id='ragdoll')
    g.later('mod.unload', id='ragdoll')
    actor = g.spawn_human('Paulie', [-1981, -4.7, 31])
    g.sleep(100)
    g.call('human.hit', actor=actor, type=8, dir=[0.5, 0, 0], damage=0, attacker='car')
    state = g.call('ragdoll.state')
    g.check('gentle car touch does not invent a ragdoll fall', not any(b['actor'] == actor for b in state['ragdolls']), state)
    g.call('human.hit', actor=actor, type=8, dir=[5, 0, 0], damage=0, attacker='car')
    state = g.call('ragdoll.state')
    g.check('a real car knockdown still becomes physical', any(b['actor'] == actor for b in state['ragdolls']), state)


def test_native_vehicle_contact_service(g):
    from mml_game import ToolError
    # An occupied car must not hit its own driver through the SDK's fallback path.
    refused = False
    try:
        g.call('human.vehicle_contact', actor='player', car='car', velocity=[6, 0, 0], pos=g.pos('player'))
    except ToolError:
        refused = True
    g.check('vehicle contact refuses its seated driver', refused)
    actor = g.spawn_human('Paulie', [-1981, -4.7, 31])
    g.sleep(100)
    result = g.call('human.vehicle_contact', actor=actor, car='car', velocity=[6, 0, 0], pos=g.pos(actor))
    state = g.call('human.anim_state', actor=actor)
    g.check('measured C_human contact goes through native knockdown', result.get('accepted') and
            'smrt pad' in str(state.get('name', '')).lower(), state)
    g.call('human.hit', actor=actor, type=0, dir=[0,0,-1], damage=1000)
    refused = False
    try:
        g.call('human.vehicle_contact', actor=actor, car='car', velocity=[6,0,0], pos=g.pos(actor))
    except ToolError:
        refused = True
    g.check('vehicle contact does not re-hit a dead target', refused)


def test_pedestrian_getup_root_motion_continuity(g):
    """A real native fall leaves a TCK trajectory whose old endpoint must not pull the get-up away."""
    import time
    if not any(m['id'] == 'ragdoll' and m['loaded'] for m in g.call('mods.list')):
        g.call('mod.load', id='ragdoll')
        g.later('mod.unload', id='ragdoll')
    peds = [p for p in g.call('pedestrians.list')['pedestrians']
            if p.get('alive') and p.get('spawned') and p['health'] > 50]
    if not peds:
        from mml_game import Skip
        raise Skip('this case needs a traffic pedestrian healthy enough to survive the test hit')
    ped = peds[0]['element']
    model = peds[0]['model']
    g.call('human.hit', pedestrian=ped, type=5, dir=[4,0,1], damage=25, attacker='car')
    g.sleep(1700)
    current = next((p for p in g.call('pedestrians.list')['pedestrians'] if p['element'] == ped), {})
    body = next((b for b in g.call('ragdoll.state')['ragdolls'] if b['actor'] == ped), {})
    if not g.check('get-up target survived as the same physical knockdown', current.get('alive') and
                   current.get('model') == model and body.get('knockdown'), {'pedestrian': current, 'body': body}):
        return
    def pelvis():
        return next(b['world_pos'] for b in g.call('human.skeleton', pedestrian=ped)['bones'] if b['name'] == 'pelvis')
    initial = pelvis()
    g.call('ragdoll.drop', pedestrian=ped)
    previous = initial
    jump = travel = 0.0
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        current = pelvis()
        jump = max(jump, math.hypot(current[0]-previous[0], current[2]-previous[2]))
        travel = max(travel, math.hypot(current[0]-initial[0], current[2]-initial[2]))
        previous = current
        time.sleep(0.025)
    g.check('native fall/get-up has no horizontal teleport', jump < 0.35, {'largest_step_m':jump})
    g.check('get-up stays by the physical body through native track progression', travel < 1.4, {'largest_travel_m':travel})
