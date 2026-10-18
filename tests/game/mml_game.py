"""mml_game.py: the real game driven for a test, through the MCP server (mcp/target/release/mml-mcp.exe) over stdio,
one server and one game a lab. Python standard library only. A case is a function of a `Game`; tests/game/run.py runs
the cases in the labs (docs/MODDING.md "Tests in the real game").

    g.tool('spawn', human='Paulie', at=[x, y, z])      -> the tool's reply (json parsed when it is json)
    g.call('human.go', actor=a, to=[x, y, z])           -> a bridge method's reply
    g.wait('ORDER_ENDED', {'id': 3}, since=seq)        -> the event, or None when it did not come
    g.check('he got there', d < 1.5, d)                 -> pass or fail, the case goes on
"""
import json
import math
import os
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MCP = os.path.join(ROOT, 'mcp', 'target', 'release', 'mml-mcp.exe')
_print_lock = threading.Lock()


class ToolError(Exception):
    """A tool's error reply (its text)."""


class Skip(Exception):
    """The case cannot run here (no such thing in this build, a lab without what it needs)."""


class Client:
    """A minimal MCP client over stdio (as tests/mcp_smoke.py's)."""

    def __init__(self):
        if not os.path.exists(MCP):
            raise RuntimeError('no MCP server: cargo build --release --manifest-path mcp/Cargo.toml')
        env = dict(os.environ, MML_REPO=ROOT)
        env.pop('MML_GAME_DIR', None)   # the labs of mml.config.json, not a harness
        self.p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
        self.n = 0
        self.request('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                    'clientInfo': {'name': 'tests/game', 'version': '1'}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, msg):
        self.p.stdin.write((json.dumps(msg) + '\n').encode())
        self.p.stdin.flush()

    def request(self, method, params=None):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params or {}})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('the MCP server closed its stdout')
            msg = json.loads(line)
            if msg.get('id') == self.n:
                if 'error' in msg:
                    raise RuntimeError(msg['error'])
                return msg['result']

    def tool(self, name, args):
        r = self.request('tools/call', {'name': name, 'arguments': args})
        text = ''.join(c.get('text', '') for c in r.get('content', []) if c.get('type') == 'text')
        return (not r.get('isError', False)), text

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


class Game:
    """One lab's game, every tool call aimed at it (`game`: its build)."""

    def __init__(self, build, profile=None):
        self.build = build
        self.profile = profile
        self.c = Client()
        self.fails = []
        self.passes = 0
        self.case = ''
        self.made = []   # the actors a case made: removed at its end
        self.undo = []   # (method, params) called at its end, the last first (a prop removed, a camera given back)

    # ---- the tools
    def tool(self, name, **args):
        args = {k: v for k, v in args.items() if v is not None}
        args['game'] = self.build
        ok, text = self.c.tool(name, args)
        if not ok:
            raise ToolError(text)
        try:
            return json.loads(text)
        except ValueError:
            return text

    def call(self, method, timeout_ms=None, **params):
        return self.tool('call', method=method, params=params, timeout_ms=timeout_ms)

    def latest(self):
        """The newest event's sequence number: wait(since=it) sees only what comes after."""
        return self.call('events.since', seq=1 << 62, limit=1)['latest']

    def wait(self, types, filter=None, since=None, timeout=30.0):
        try:
            return self.tool('wait', types=types, filter=filter, since=since, timeout_s=float(timeout))['event']
        except ToolError as e:
            if ' in ' in str(e) and 'no ' in str(e):
                return None
            raise

    def order_end(self, order, since, timeout=60.0):
        """The ORDER_ENDED of an order (a human.* reply) after `since`: its data, with the named orders' outcome
        (`arrived`, `distance`, `seated`, `retried`), or None when it did not end in time."""
        ev = self.wait('ORDER_ENDED', {'id': order['id']}, since=since, timeout=timeout)
        return ev['data'] if ev else None

    def road(self, at):
        """The game's road network's node nearest to a place or an actor: its position (None: no road network)."""
        r = self.call('world.road', at=at)
        return r.get('pos') if r.get('node') is not None else None

    def sleep(self, ms):
        """The game's own time: an input sequence of nothing, so a paused game does not count."""
        self.tool('input', steps=[{'wait_ms': int(ms)}])

    # ---- the world
    def player(self):
        return self.call('player')

    def pos(self, actor):
        """Where an actor is in the world (its model frame's world position)."""
        near = self.call('world.near', at=actor, radius=0.5)
        for a in near.get('actors', []):
            if actor in ('player',) or a['actor'].lower() == str(actor).lower():
                return a['pos']
        return near['at']

    def spawn_human(self, model, at, **kw):
        a = self.tool('spawn', human=model, at=at, **kw)
        self.made.append(a['actor'])
        return a['actor']

    def later(self, method, **params):
        """A bridge call made when the case ends, whatever happened (the last asked, the first made)."""
        self.undo.append((method, params))

    def shot(self):
        """A screenshot of the game's frame: its file's bytes."""
        r = self.tool('screenshot', max_width=320)
        files = [x.get('file') for x in r if isinstance(x, dict) and x.get('file')] if isinstance(r, list) else []
        if not files:
            raise ToolError('screenshot: no file in %r' % (r,))
        with open(files[0], 'rb') as f:
            return f.read()

    def on_foot(self):
        """The player out of his car (the freeride starts him in it): the engine stopped, then out."""
        for _ in range(3):
            if not self.player().get('vehicle'):
                return True
            self.tool('input', steps=[{'action': 'Action'}, {'wait_ms': 2500}])
        return not self.player().get('vehicle')

    # ---- the verdicts
    def log(self, text):
        with _print_lock:
            print('[%s] %s' % (self.build, text), flush=True)

    def check(self, name, cond, detail=''):
        if cond:
            self.passes += 1
            self.log('  ok    %s: %s' % (self.case, name))
        else:
            self.fails.append('%s: %s' % (self.case, name))
            self.log('  FAIL  %s: %s  -- %s' % (self.case, name, str(detail)[:400]))
        return bool(cond)

    def cleanup(self):
        """What the case made, taken out; a person sitting in a car gets out first (world.remove refuses him)."""
        for method, params in reversed(self.undo):
            try:
                self.call(method, **params)
            except (ToolError, RuntimeError) as e:
                self.log('  (cleanup: %s: %s)' % (method, e))
        self.undo = []
        for a in reversed(self.made):
            try:
                self.call('world.remove', actor=a)
            except ToolError as e:
                if 'car' not in str(e):
                    continue
                try:
                    seq = self.latest()
                    self.call('human.order', actor=a, code='enemy_usecar')   # no arguments: out of his car
                    self.wait('VEHICLE_LEFT', {'actor': a}, since=seq, timeout=15)
                    self.call('world.remove', actor=a)
                except (ToolError, RuntimeError) as e2:
                    self.log('  (cleanup: %s stays: %s)' % (a, e2))
            except RuntimeError:
                pass
        self.made = []

    # ---- the game itself
    MISSIONS = {'city-small': 'freeitaly', 'city': 'freeride'}   # a freeride's kind, its mission

    def in_freeride(self, kind='city-small'):
        try:
            g = self.call('game')
        except (ToolError, RuntimeError):
            return False
        return (isinstance(g, dict) and str(g.get('mission_name', '')).lower() == self.MISSIONS.get(kind, kind)
                and g.get('mission_state') == 'running')

    def start_freeride(self, kind='city-small', fresh=False):
        """The city's small freeride without police (the player holds a Thompson: patrols would shoot), pedestrians
        or traffic (a shot among pedestrians brings the game's own reactions: a gang that shot back killed a test's
        Paulie), the same start in both builds: launched, or the running game used when it is there already."""
        st = self.tool('game_status')
        running = st.get('running') if isinstance(st, dict) else False
        if running and not fresh and self.in_freeride(kind):
            return
        if running:
            self.tool('game_close')
        if self.profile is None:
            raise RuntimeError('no profile for %s: --profile %s=<number or name> (or mml.config.json "test_profiles")'
                               % (self.build, self.build))
        self.tool('game_launch', profile=self.profile, go='freeride', kind=kind, police=0, pedestrians=0, traffic=0,
                  wait_s=120)
        log = '\n'.join(self.call('log.tail', lines=60))
        if 'will not use the police density' in log:
            raise RuntimeError('profile %s of %s never opened the freeride\'s settings: the game ignores police 0 '
                               '(another profile, or open them once by hand)' % (self.profile, self.build))

    def close(self):
        self.c.close()
