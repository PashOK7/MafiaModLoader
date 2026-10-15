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


def main():
    c = Client()
    check('initialize: the server tells agents how to use it', 'game_status' in (c.info.get('instructions') or ''), c.info)
    tools = {t['name'] for t in c.request('tools/list')['tools']}
    want = {'game_status', 'call', 'commands_list', 'mem_read', 'mem_write', 'log_tail', 'mods_list', 'mod_reload',
            'where', 'game_launch', 'game_close', 'crash_list', 'crash_inspect', 'deploy', 'kb_search', 'kb_function', 'tap',
            'screenshot', 'input', 'script', 'script_commands', 'spawn', 'teleport', 'give'}
    check('tools/list has every tool', want <= tools, want - tools)

    ok, text = c.tool('kb_search', {'query': 'scene.i3d', 'module': 'game.exe'})
    check('kb_search: by a string the function uses', ok and 'C_mission::Open' in text, text)
    ok, text = c.tool('kb_search', {'query': 'pedCarHit'})
    check('kb_search: our own names (names.csv)', ok and '0044CA50' in text, text)
    ok, text = c.tool('kb_search', {'query': 'ISD0'})
    check('kb_search: the notes', ok and 'notes/rw_data.md' in text, text)
    ok, text = c.tool('kb_function', {'key': 'game+0x1409D0', 'max_lines': 30})
    check('kb_function: by an address inside, with callers', ok and 'C_mission::Open' in text and '"callers"' in text, text)
    if 'code_unavailable' not in text:
        check('kb_function: the decompiled code from the reverse folder', 'missions' in text, text[-400:])
    ok, text = c.tool('kb_function', {'key': 'C_human::Hit', 'code': False})
    check('kb_function: by name', ok and '005762A0' in text, text)

    harness = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '60'], cwd=HARNESS,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        ok, text = c.tool('game_status')
        check('game_status: running, with the mods', ok and '"running": true' in text and 'hook_a' in text, text)
        ok, text = c.tool('call', {'method': 'hook_a.call', 'params': {'a': 2, 'b': 3}})
        check('call: a mod command through the bridge (30)', ok and text.strip() == '30', text)
        ok, text = c.tool('mem_read', {'addr': 'mml_harness.exe+0', 'size': 2})
        check('mem_read', ok and '4d5a' in text, text)
        ok, text = c.tool('mod_reload', {'id': 'hello_c'})
        check('mod_reload', ok and '"generation"' in text, text)
        ok, text = c.tool('log_tail', {'lines': 50})
        check('log_tail (live)', ok and 'hello_c' in text, text)
        ok, text = c.tool('call', {'method': 'no.such.method'})
        check('an unknown method is an error result', not ok, text)
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
        check('the instructions tell of the kit', all(w in c.info.get('instructions', '') for w in ('draw.text', 'config.set', 'new_mod.py')))
    finally:
        open(os.path.join(HARNESS, 'quit'), 'w').close()
        harness.wait(timeout=10)
    time.sleep(0.3)
    ok, text = c.tool('game_status')
    check('game_status after the game is gone: not running', ok and '"running": false' in text, text)
    ok, text = c.tool('log_tail', {'lines': 20})
    check('log_tail from the file when the game is gone', ok and 'process detaching' in text, text)
    # deploy: a mod's folder with its subfolders (a mod of files alone: its files\), what did not change left alone
    pic = os.path.join(HARNESS, 'mml', 'mods', 'freeride_picture')
    shutil.rmtree(pic, ignore_errors=True)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    check('deploy: a mod\'s files\\ folder goes with it', ok and 'freeride0.bmp' in text
          and os.path.exists(os.path.join(pic, 'files', 'Maps', 'freeride0.bmp')), text)
    ok, text = c.tool('deploy', {'mods': ['freeride_picture']})
    check('deploy again: unchanged files are left alone', ok and 'freeride0.bmp' not in text and 'mod.json' not in text, text)
    shutil.rmtree(pic, ignore_errors=True)
    c.close()
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
