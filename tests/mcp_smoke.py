"""mcp_smoke.py: the MCP server (mcp/target/release/mml-mcp.exe) driven by a minimal MCP client over stdio, against
the test harness standing in for the game (MML_GAME_DIR = build/x86/harness). Python standard library only.

    cmake --build --preset x86-release && cargo build --release --manifest-path mcp/Cargo.toml && python tests/mcp_smoke.py
"""
import json
import shutil
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, 'build', 'x86', 'harness')
MCP = os.path.join(ROOT, 'mcp', 'target', 'release', 'mml-mcp.exe')
failures = []


def check(name, cond, detail=''):
    print(('  ok    ' if cond else '  FAIL  ') + name + ('' if cond else '  -- %s' % str(detail)[:400]))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self):
        env = dict(os.environ, MML_GAME_DIR=HARNESS, MML_REPO=ROOT)
        self.p = subprocess.Popen([MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.n = 0
        r = self.request('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                        'clientInfo': {'name': 'mcp_smoke', 'version': '1'}})
        self.info = r
        self.notify('notifications/initialized')

    def send(self, msg):
        self.p.stdin.write((json.dumps(msg) + '\n').encode())
        self.p.stdin.flush()

    def notify(self, method, params=None):
        self.send({'jsonrpc': '2.0', 'method': method, **({'params': params} if params else {})})

    def request(self, method, params=None):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params or {}})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('the server closed stdout: ' + self.p.stderr.read().decode(errors='replace'))
            msg = json.loads(line)
            if msg.get('id') == self.n:
                if 'error' in msg:
                    raise RuntimeError(msg['error'])
                return msg['result']

    def tool(self, name, args=None):
        r = self.request('tools/call', {'name': name, 'arguments': args or {}})
        text = ''.join(c.get('text', '') for c in r.get('content', []))
        return (not r.get('isError', False)), text

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)


def two_games(c):
    """Games side by side (the loader gives each folder its own one-at-a-time mutex): a second stand-in game in
    another folder, reached by the tools' `game` (a folder; a lab's build in the labs), each its own bridge."""
    second = os.path.join(ROOT, 'build', 'x86', 'harness_second')
    shutil.rmtree(second, ignore_errors=True)
    shutil.copytree(HARNESS, second, ignore=shutil.ignore_patterns('live', 'logs', 'crash', 'data', 'shots', 'restore',
                                                                    'boxes', 'heavy', 'hello_c', 'bridge.json', 'quit'))
    shutil.copy(os.path.join(HARNESS, 'mml_harness.exe'), os.path.join(second, 'Game.exe'))   # (a folder `game` takes)
    p = subprocess.Popen([os.path.join(second, 'mml_harness.exe'), '--seconds', '30'], cwd=second,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        listed = {t['name']: t for t in c.request('tools/list')['tools']}
        check('tools/list: `game` on the tools that touch a game, not on the knowledge base\'s',
              'game' in listed['call']['inputSchema'].get('properties', {}) and 'game' in listed['game_status']['inputSchema'].get('properties', {})
              and 'game' not in listed['kb_search']['inputSchema'].get('properties', {}), listed['call']['inputSchema'])
        ok1, one = c.tool('call', {'method': 'ping'})
        ok2, two = c.tool('call', {'method': 'ping', 'game': second})
        a, b = (json.loads(t) if ok else {} for t, ok in ((one, ok1), (two, ok2)))
        check('`game`: two games at once, each its own bridge (another pid, its own folder)',
              ok1 and ok2 and a.get('pid') != b.get('pid')
              and os.path.normcase(b.get('game_dir', '')) == os.path.normcase(second + os.sep), (one, two))
        ok, text = c.tool('call', {'method': 'hook_a.call', 'params': {'a': 2, 'b': 3}, 'game': second})
        check('`game`: a mod command in the second game', ok and text.strip() == '30', text)
        ok, text = c.tool('log_tail', {'lines': 200, 'game': second})
        check('`game`: the second game\'s log', ok and 'harness_second' in text, text[-300:])
        ok, text = c.tool('game_status', {'game': 'no such lab'})
        check('`game`: an unknown one refused, the labs named', not ok and 'not a lab' in text, text)
        ok, text = c.tool('call', {'method': 'ping'})
        check('the default game still answers after the other\'s calls', ok and json.loads(text).get('pid') == a.get('pid'), text)
    finally:
        open(os.path.join(second, 'quit'), 'w').close()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    shutil.rmtree(second, ignore_errors=True)


def ghidra_checks(c):
    """The ghidra_* tools: with Ghidra and the projects there (tools/deps.py ghidra, tools/ghidra_cli.py setup), both
    builds asked at once; without them, an error saying what to run."""
    made = all(os.path.isdir(os.path.join(ROOT, 're', 'ghidra', p + '.rep')) for p in ('mafia12', 'mafia10'))
    if not (os.path.exists(os.path.join(ROOT, 'third_party', '_deps', 'ghidra', 'ghidraRun.bat')) and made):
        ok, text = c.tool('ghidra_read', {'op': 'function', 'at': '5762A0'})
        check('ghidra_read with no Ghidra or no projects: an error saying what to run', not ok and 'ghidra' in text, text)
        print('SKIPPED: the Ghidra projects (python tools/deps.py ghidra; python tools/ghidra_cli.py setup)')
        return
    ok, text = c.tool('ghidra_read', {'op': 'function', 'at': 'humanHit'})
    check('ghidra_read function: 1.2, by an SDK alias, the decompile\'s class name', ok and 'C_human::Hit @ 5762A0' in text, text)
    ok, text = c.tool('ghidra_read', {'op': 'decompile', 'at': 'C_mission::Tick', 'max': 5})
    check('ghidra_read decompile: by a class name', ok and 'C_mission::Tick @ 5407E0' in text, text[:300])
    ok, text = c.tool('ghidra_read', {'op': 'twin', 'at': 'C_human::Hit'})
    check('ghidra_read twin: 1.2 -> 1.0', ok and '1.0 496710' in text, text)
    ok, text = c.tool('ghidra_read', {'op': 'function', 'at': '496710', 'build': '1.0'})
    check('ghidra_read in 1.0: the twin, named through the pair', ok and 'C_human::Hit @ 496710' in text and '(1.0)' in text, text)
    ok, text = c.tool('ghidra_read', {'op': 'callers', 'at': 'C_human::Hit', 'max': 3})
    check('ghidra_read callers', ok and 'references to C_human::Hit' in text, text)
    ok, text = c.tool('ghidra_find', {'op': 'imm', 'value': '644', 'writes': True, 'max': 3})
    check('ghidra_find imm: the writes of a field\'s offset', ok and 'instructions use 0x644' in text and 'MOV' in text, text)
    ok, text = c.tool('ghidra_find', {'op': 'functions', 'regex': '^C_mission::Open$'})
    check('ghidra_find functions: by a name', ok and '1 functions' in text, text)
    ok, text = c.tool('ghidra_read', {'op': 'rename', 'at': '5762A0'})
    check('ghidra_read refuses a write op', not ok and 'op is one of' in text, text)
    ok, text = c.tool('ghidra_read', {'op': 'function', 'at': '5762A0', 'build': '2.0'})
    check('ghidra_read: an unknown build refused', not ok and '"1.2" or "1.0"' in text, text)


def main():
    c = Client()
    check('initialize: the server tells agents how to use it', 'game_status' in (c.info.get('instructions') or ''), c.info)
    tools = {t['name'] for t in c.request('tools/list')['tools']}
    want = {'game_status', 'call', 'commands_list', 'mem_read', 'mem_write', 'log_tail', 'mods_list', 'mod_reload',
            'where', 'game_launch', 'game_close', 'crash_list', 'crash_inspect', 'deploy', 'kb_search', 'kb_function', 'tap',
            'screenshot', 'input', 'script', 'script_commands', 'spawn', 'teleport', 'give', 'profile', 'wait', 'watch',
            'inspect', 'crash_triage', 'restore', 'ghidra_read', 'ghidra_find', 'ghidra_write'}
    check('tools/list has every tool', want <= tools, want - tools)

    if os.path.exists(os.path.join(ROOT, 'knowledge', 'game.exe', 'strings.local.csv')):   # (tools/kb_strings.py made it)
        ok, text = c.tool('kb_search', {'query': 'scene.i3d', 'module': 'game.exe'})
        check('kb_search: by a string the function uses (read from this machine\'s game)', ok and 'C_mission::Open' in text, text)
    ok, text = c.tool('kb_search', {'query': 'C_mission::Open', 'module': 'game.exe'})
    check('kb_search: by a name', ok and '005' in text, text)
    ok, text = c.tool('kb_search', {'query': 'pedCarHit'})
    check('kb_search: our own names (names.csv)', ok and '0044CA50' in text, text)
    ok, text = c.tool('kb_search', {'query': '00401060', 'module': 'game.exe'})
    check('kb_search: a 1.0 address, by st0rm94\'s own 1.0 names (functions_10)', ok and '"address_10": "00401060"' in text
          and 'ai_logs::Init' in text, text[-500:])
    ok, text = c.tool('kb_search', {'query': 'ISD0'})
    check('kb_search: the notes', ok and 'notes/rw_data.md' in text, text)
    ok, text = c.tool('kb_function', {'key': 'game+0x1409D0', 'max_lines': 30})
    check('kb_function: by an address inside, with callers', ok and 'C_mission::Open' in text and '"callers"' in text, text)
    if 'code_unavailable' not in text:
        check('kb_function: the decompiled code from the reverse folder', 'missions' in text, text[-400:])
    ok, text = c.tool('kb_function', {'key': 'C_human::Hit', 'code': False})
    check('kb_function: by name', ok and '005762A0' in text, text)
    # the structures (knowledge/structs, tools/kb_structs.py): a layout needs no game
    ok, text = c.tool('inspect', {'as': 'C_player', 'fields': ['health', 'kind']})
    check('inspect: a layout with its bases, both builds\' offsets and sources', ok and '"C_actor"' in text and
          '+0x644 C_human.health f32 [game.h]' in text and '+0x10 C_actor.kind i32 [game.h]' in text, text[:600])
    ok, text = c.tool('inspect', {'as': 'C_Vehicle', 'fields': ['speed']})
    check('inspect: a field of each build (C_Vehicle in 1.0 is 1.2\'s + 0x7C)', ok and '+0x59C (1.0 +0x618) speed' in text, text[:600])
    ok, text = c.tool('inspect', {'as': 'C_nothing'})
    check('inspect: an unknown structure refused', not ok and 'no struct' in text, text)
    ok, text = c.tool('kb_search', {'query': 'fuel_tank'})
    check('kb_search: the structures\' fields', ok and 'C_Vehicle.fuel_tank +0x1FA0 (1.0 +0x1F1C)' in text, text[-400:])

    harness = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '60'], cwd=HARNESS,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        ok, text = c.tool('game_status')
        check('game_status: running, with the mods', ok and '"running": true' in text and 'hook_a' in text, text)
        ok, text = c.tool('call', {'method': 'hook_a.call', 'params': {'a': 2, 'b': 3}})
        check('call: a mod command through the bridge (30)', ok and text.strip() == '30', text)
        ok, text = c.tool('profile', {'seconds': 0.5})
        check('profile: the game thread sampled, the mods\' costs by site', ok and '"samples"' in text and '"by_module"' in text
              and '"mods"' in text and 'hook_a.call' in text, text[:600])
        ok, text = c.tool('mem_read', {'addr': 'mml_harness.exe+0', 'size': 2})
        check('mem_read', ok and '4d5a' in text, text)
        ok, text = c.tool('inspect', {'at': 'mml_harness.exe+0', 'as': 'S_GameItem'})
        check('inspect: an object of no known build of the game refused', not ok and 'build is not known' in text, text)
        ok, text = c.tool('mod_reload', {'id': 'hello_c'})
        check('mod_reload', ok and '"generation"' in text, text)
        ok, text = c.tool('call', {'method': 'events.since', 'params': {'seq': 0, 'types': 'MOD_LOADED', 'filter': {'name': 'hello_c'}}})
        check('events.since through call: hello_c loaded', ok and '"MOD_LOADED"' in text, text[:300])
        ok, text = c.tool('wait', {'types': 'MOD_LOADED', 'filter': {'name': 'hello_c'}, 'since': 0, 'timeout_s': 2})
        check('wait: an event already kept, from `since`', ok and '"MOD_LOADED"' in text and '"latest"' in text, text[:300])
        ok, text = c.tool('wait', {'types': 'MISSION_LOADED', 'timeout_s': 0.5})
        check('wait: none comes in time: an error saying so', not ok and 'no MISSION_LOADED' in text, text[:300])
        ok, text = c.tool('log_tail', {'lines': 50})
        check('log_tail (live)', ok and 'hello_c' in text, text)
        ok, text = c.tool('call', {'method': 'no.such.method'})
        check('an unknown method is an error result', not ok, text)
        ok, text = c.tool('commands_list', {'query': 'world.ray'})
        listed = json.loads(text) if ok else []
        check('commands_list {query}: the matching methods, each with its params and result',
              ok and any(m.get('name') == 'world.ray' and 'camera' in m.get('params', '') and m.get('result') for m in listed)
              and len(listed) < 10, text[:400])
        ok, text = c.tool('screenshot')
        check('screenshot with no Direct3D device: an error result that says so', not ok and 'device' in text, text)
        ok, text = c.tool('input', {'steps': [{'turn': 90}, {'action': 'Forwards', 'ms': 200}]})
        check('input with no game: an error result that says so', not ok and 'not known' in text, text)
        ok, text = c.tool('input', {'steps': [{'action': 'Fire', 'key': 'Esc'}]})
        check('input: a step with both an action and a key is refused', not ok and 'not both' in text, text)
        ok, text = c.tool('where')
        check('where samples the main thread', ok and 'sample 4' in text, text)
        # the mod kit through `call`: drawing held for a frame's 2D pass, a mod's settings, the car with no game
        ok, text = c.tool('call', {'method': 'draw.text', 'params': {'x': 10, 'y': 10, 'text': 'smoke', 'ms': 500}})
        check('call draw.text: held (and no game to draw it in)', ok and '"held": 1' in text and 'note' in text, text)
        ok, text = c.tool('call', {'method': 'config.set', 'params': {'mod': 'kit', 'key': 'smoke.n', 'value': 3}})
        check('call config.set: a mod\'s setting changed', ok and '"value": 3' in text, text)
        ok, text = c.tool('call', {'method': 'config.set', 'params': {'mod': 'kit', 'key': 'smoke', 'value': None}})
        check('call config.set null: the setting gone again', ok and '"known": false' in text, text)
        ok, text = c.tool('call', {'method': 'car'})
        check('call car with no game: an error result that says so', not ok and 'not known' in text, text)
        ok, text = c.tool('restore')
        check('restore with nothing asked: the list (none here)', ok and '"points": []' in text, text)
        ok, text = c.tool('restore', {'save': 'smoke'})
        check('restore save with no game: refused, saying so', not ok and 'not a known build' in text, text)
        # bench: A against itself on the stand-in game's frames (no device: no counters, no GPU time, said so)
        ok, text = c.tool('bench', {'blocks': 2, 'frames': 30, 'settle': 5, 'counters': False})
        r = json.loads(text) if ok else {}
        d = r.get('difference', {}).get('wall_ms', {})
        check('bench A/A: each configuration\'s frames, B less A with its interval, 8 segments of 30 frames',
              ok and r.get('names') == ['A', "A'"] and 'ci95' in d and d.get('blocks') == 2 and len(r.get('segments', [])) == 8
              and all(s['frames'] == 30 for s in r['segments']), text[:600])
        check('bench: the run\'s conditions (the machine\'s load, the games), no GPU time said so',
              ok and 'machine_busy_percent' in r['conditions']['cpu'] and any('no GPU time' in w for w in r['conditions']['warnings']), text[:600])
        ok, text = c.tool('bench', {'blocks': 2, 'frames': 30, 'settle': 5})
        check('bench with the counters and no device: refused, saying why', not ok and 'Direct3D 9' in text, text)
        ok, text = c.tool('call', {'method': 'state.time', 'params': {'scale': 0.5}})
        check('state.time: the game\'s time scaled (no game: only kept)', ok and '"scale": 0.5' in text, text)
        ok, text = c.tool('call', {'method': 'state.time', 'params': {'scale': 9}})
        check('state.time out of range: refused', not ok and '0..8' in text, text)
        c.tool('call', {'method': 'state.time', 'params': {'scale': 1}})
        check('the instructions tell of the kit', all(w in c.info.get('instructions', '') for w in ('draw.text', 'config.set', 'new_mod.py')))
        check('the instructions tell of the HUD', all(w in c.info.get('instructions', '') for w in ('hud.marker', 'hud.console', 'draw.picture')))
        # deploy into the running game: a mod that is not loaded is not reloaded (mod.reload would load it)
        ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
        ok2, listed = c.tool('call', {'method': 'mods.list'})
        pic_loaded = ok2 and any(m.get('id') == 'freeride_picture' and m.get('loaded') for m in
                                 (json.loads(listed) if listed.lstrip().startswith('[') else json.loads(listed).get('mods', [])))
        check('deploy into the running game: a mod switched off stays off (not_loaded)',
              ok and '"not_loaded"' in text and 'freeride_picture' in text.split('"not_loaded"')[1] and not pic_loaded, (text, listed))
        shutil.rmtree(os.path.join(HARNESS, 'mml', 'mods', 'freeride_picture'), ignore_errors=True)
        two_games(c)
    finally:
        open(os.path.join(HARNESS, 'quit'), 'w').close()
        harness.wait(timeout=10)
    time.sleep(0.3)
    ok, text = c.tool('game_status')
    check('game_status after the game is gone: not running', ok and '"running": false' in text, text)
    ok, text = c.tool('log_tail', {'lines': 20})
    check('log_tail from the file when the game is gone', ok and 'process exiting (ExitProcess' in text, text)
    # deploy: a mod's folder with its subfolders (a mod of files alone: its files\), what did not change left alone
    pic = os.path.join(HARNESS, 'mml', 'mods', 'freeride_picture')
    shutil.rmtree(pic, ignore_errors=True)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    check('deploy: a mod\'s files\\ folder goes with it', ok and 'freeride0.bmp' in text
          and os.path.exists(os.path.join(pic, 'files', 'Maps', 'freeride0.bmp')), text)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    check('deploy again: unchanged files are left alone', ok and 'freeride0.bmp' not in text and 'mod.json' not in text, text)
    # a mod switched on in the game's folder (its mod.json there says "enabled": true; the build's says false) stays on
    man = os.path.join(pic, 'mod.json')
    def manifest(path):   # (a manifest may have comments, as the loader allows: this one's are whole lines)
        return json.loads(''.join(l for l in open(path, encoding='utf-8') if not l.lstrip().startswith('//')))
    built = manifest(os.path.join(ROOT, 'build', 'x86', 'out', 'mml', 'mods', 'freeride_picture', 'mod.json'))
    on = open(man, encoding='utf-8').read().replace('"enabled": false', '"enabled": true', 1)
    open(man, 'w', encoding='utf-8', newline='\n').write(on)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    mine = manifest(man)
    check('deploy: a mod switched on in the game\'s folder stays on (named in kept), its file left alone',
          ok and built.get('enabled') is False and mine.get('enabled') is True and '"kept"' in text
          and 'freeride_picture: enabled true' in text and 'mod.json' not in text.split('"kept"')[0], text)
    changed = on.replace('"version": "0.1.0"', '"version": "0.0.9"', 1)
    open(man, 'w', encoding='utf-8', newline='\n').write(changed)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    mine = manifest(man)
    check('deploy: the build\'s manifest put over another one, the game folder\'s "enabled" kept in it',
          ok and changed != on and mine.get('version') == built.get('version') and mine.get('enabled') is True
          and {k: v for k, v in mine.items() if k != 'enabled'} == {k: v for k, v in built.items() if k != 'enabled'}, (text, mine))
    shutil.rmtree(pic, ignore_errors=True)
    # only the mods the build defines (build/x86/out/mml/build_mods.txt) are deployed: a removed mod's old output folder
    # (CMake keeps a target's outputs when the target goes) is skipped and named; the game folder's other mods are
    # named in `others`, never touched. A whole deploy, into a game folder of its own (`game`: a folder with Game.exe)
    listed = open(os.path.join(ROOT, 'build', 'x86', 'out', 'mml', 'build_mods.txt'), encoding='utf-8').read().split()
    stale_out = os.path.join(ROOT, 'build', 'x86', 'out', 'mml', 'mods', 'smoke_removed')
    there = os.path.join(ROOT, 'build', 'x86', 'deploy_smoke')
    shutil.rmtree(there, ignore_errors=True)
    os.makedirs(os.path.join(there, 'mml', 'mods', 'players_own'))
    open(os.path.join(there, 'Game.exe'), 'w').close()
    open(os.path.join(there, 'mml', 'mods', 'players_own', 'mod.json'), 'w').write('{"id": "players_own"}\n')
    os.makedirs(stale_out, exist_ok=True)
    open(os.path.join(stale_out, 'mod.json'), 'w').write('{"id": "smoke_removed"}\n')
    try:
        ok, text = c.tool('deploy', {'game': there})
        got = sorted(os.listdir(os.path.join(there, 'mml', 'mods')))
        check('deploy: every mod the build defines, a removed mod\'s old output skipped and named',
              ok and 'smoke_removed (build/x86/out/mml/mods/smoke_removed: not a mod of this build' in text
              and got == sorted(listed + ['players_own']), (got, text))
        check('deploy: the game folder\'s mods the build does not define named in others, left as they are',
              ok and '"others"' in text and 'players_own' in text.split('"others"')[1]
              and open(os.path.join(there, 'mml', 'mods', 'players_own', 'mod.json')).read() == '{"id": "players_own"}\n', text)
        ok, text = c.tool('deploy', {'game': there, 'mods': ['smoke_removed', 'no_such_mod']})
        check('deploy of named mods the build does not define: skipped, said so, nothing copied',
              ok and 'smoke_removed (build/x86/out/mml/mods/smoke_removed: not a mod of this build' in text
              and 'no_such_mod (not a mod of this build)' in text and '"copied": []' in text
              and not os.path.exists(os.path.join(there, 'mml', 'mods', 'smoke_removed')), text)
    finally:
        shutil.rmtree(stale_out, ignore_errors=True)
        shutil.rmtree(there, ignore_errors=True)
    # crash_triage: a mod's fault in a running game, then a crash nobody handles (the harness's `crash` file)
    shutil.rmtree(os.path.join(HARNESS, 'mml', 'crash'), ignore_errors=True)
    harness = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '30'], cwd=HARNESS,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        ok, text = c.tool('crash_triage')
        check('crash_triage with no crash yet, the game running: nothing faulted, no dumps',
              ok and 'no mod faulted in this run' in text and '"crash"' in text, text[:400])
        c.tool('call', {'method': 'hook_b.crash'})
        ok, text = c.tool('crash_triage')
        check('crash_triage: a mod\'s fault in this run, with its stack', ok and '"MOD_FAULTED"' in text and '(mod hook_b)' in text, text[:600])
        # (hook_b is unloaded now: the crash under the hooks goes through hook_a's detour alone)
        open(os.path.join(HARNESS, 'crash_hooked'), 'w').close()
        harness.wait(timeout=20)
    finally:
        if harness.poll() is None:
            harness.kill()
    time.sleep(0.3)
    ok, text = c.tool('crash_triage')
    check('crash_triage after a crash: the verdict (a write of address 0 in the game\'s own code), the stack, the log, the mods',
          ok and 'an access violation (C0000005) at mml_harness.exe+0x' in text and 'a write of 0x00000000' in text
          and 'a null pointer' in text and '"stack"' in text and 'exception C0000005' in text and 'hook_b' in text, text[:900])
    try:
        crash = json.loads(text)['crash']
    except (ValueError, KeyError, TypeError):
        crash = {}
    st = crash.get('stack', [])
    want = ['(harness_crash+0x', '(harness_add+0x', '(mod hook_a: ', '(main+0x']
    check("crash_triage's stack: the true chain, checked and named by the PDBs (harness_crash, harness_add, hook_a's "
          "detour, main), the finished calls' leftovers dropped and counted",
          len(st) >= 4 and all(w in s for w, s in zip(want, st)) and 'Detour+0x' in st[2]
          and not any('MafiaModLoader.asi' in s for s in st) and isinstance(crash.get('stack_dropped'), int)
          and 'dropped' in crash.get('stack_note', ''), {k: crash.get(k) for k in ('stack', 'stack_dropped', 'stack_note')})
    ok, text = c.tool('crash_inspect')
    check('crash_inspect: the same checked stack beside the dump\'s scan', ok and '"stack_scan"' in text and '(harness_add+0x' in text
          and '"stack_dropped"' in text, text[:600])
    # a crash in a mod's hook (hook_b's detour: the game calls it, nothing guards it)
    shutil.rmtree(os.path.join(HARNESS, 'mml', 'crash'), ignore_errors=True)
    harness = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '30'], cwd=HARNESS,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        open(os.path.join(HARNESS, 'crash_mod'), 'w').close()
        harness.wait(timeout=20)
    finally:
        if harness.poll() is None:
            harness.kill()
    time.sleep(0.3)
    ok, text = c.tool('crash_triage')
    try:
        crash = json.loads(text)['crash']
    except (ValueError, KeyError, TypeError):
        crash = {}
    st = crash.get('stack', [])
    check("crash_triage of a crash in a mod's hook: the verdict names the mod and its function, main below it",
          'in mod_hook_b_' in crash.get('verdict', '') and '(mod hook_b: ' in crash.get('verdict', '') and 'Detour+0x' in crash.get('verdict', '')
          and len(st) >= 2 and '(main+0x' in st[1], {k: crash.get(k) for k in ('verdict', 'stack')})
    shutil.rmtree(os.path.join(HARNESS, 'mml', 'crash'), ignore_errors=True)
    ghidra_checks(c)   # (last: a Ghidra server's first start takes its time, the harness's games have theirs)
    c.close()
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
