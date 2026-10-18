"""The measuring mods' first cases in the small city (ROADMAP item 23): a car hits a person (the ragdoll's start), a
camera turned (VR's, the airplane's), a prop held in a hand (fishing's rod, the skateboard)."""
import math

from mml_game import dist


def unit(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / n for c in v]


def test_car_hits_person(g):
    """The player's car driven into a person standing in front of it: the game's own hit, told as HUMAN_HIT."""
    g.check('the player starts in his car', g.player().get('vehicle'), g.player())
    car = g.pos('car')
    victim = g.tool('spawn', human='Paulie', at='car', ahead=12)
    g.made.append(victim['actor'])
    # his back to the car: facing it, the game's people jump out of a car's way (ai_action type 0x25), and once did
    away = unit([victim['pos'][0] - car[0], 0.0, victim['pos'][2] - car[2]])
    g.tool('teleport', actor=victim['actor'], to=victim['pos'], dir=away)
    victim = victim['actor']
    seq = g.latest()
    g.tool('input', steps=[{'action': 'Accelerate', 'ms': 3500}])
    hit = g.wait('HUMAN_HIT', {'actor': victim}, since=seq, timeout=8)
    g.check('the car hits him (HUMAN_HIT)', hit is not None, hit)
    if hit:
        g.check('the hit is the car\'s (hit type 8, the attacker the car)', hit['data'].get('hit_type') == 8, hit['data'])
        g.check('the hit has its strength and place', hit['data'].get('damage', 0) > 0 and len(hit['data'].get('pos', [])) == 3,
                hit['data'])
    g.tool('input', steps=[{'action': 'Brake', 'ms': 2500}])


def test_camera_turned(g):
    """The frame drawn from a camera of the agent's (camera.set), then given back."""
    me = g.pos('player')
    eye = [me[0] + 6.0, me[1] + 4.0, me[2] + 6.0]
    at = [me[0], me[1] + 1.0, me[2]]
    before = g.shot()
    g.call('camera.set', eye=eye, at=at, ms=10000)
    g.later('camera.release')
    g.sleep(500)
    st = g.call('camera.state')
    g.check('camera.state: the bridge holds it', 'camera.set' in str(st.get('override', '')), st)
    fc = st.get('frame_camera', {})
    g.check('the frame drawn from the eye asked (%.2f m)' % dist(fc.get('eye', [0, 0, 0]), eye),
            dist(fc.get('eye', [0, 0, 0]), eye) < 0.05, fc)
    want = unit([at[i] - eye[i] for i in range(3)])
    d = sum(a * b for a, b in zip(unit(fc.get('fwd', [0, 0, 1])), want))
    g.check('looking where asked (cos %.4f)' % d, d > 0.999, fc)
    turned = g.shot()
    g.check('the picture changes', turned != before, (len(before), len(turned)))
    g.call('camera.release')
    g.undo = []
    g.sleep(500)
    st = g.call('camera.state')
    g.check('given back: no override', 'camera.set' not in str(st.get('override', '')), st)


def test_prop_in_hand(g):
    """A model of the game's put into the player's right hand: it stays there when he turns."""
    g.check('the player gets out of his car', g.on_foot(), g.player())
    prop = g.call('prop.create', model='2bbat1', parent='player', bone='r_hand')
    pid = prop.get('prop')
    g.check('prop.create: made on the hand bone', pid and str(prop.get('parent_name', '')).endswith('r_hand'), prop)
    if not pid:
        return
    g.later('prop.remove', prop=pid)

    def gap():
        hand = [b for b in g.call('human.skeleton')['bones'] if b['name'] == 'r_hand'][0]['world_pos']
        mine = [p for p in g.call('props.list')['props'] if p['prop'] == pid][0]['pos']
        return dist(hand, mine)

    g.sleep(300)
    d = gap()
    g.check('it is at the hand (%.3f m)' % d, d < 0.05, d)
    g.tool('input', steps=[{'turn': 120, 'ms': 600}, {'action': 'Forwards', 'ms': 1500}])
    d = gap()
    g.check('it stays at the hand when he turns and walks (%.3f m)' % d, d < 0.05, d)
