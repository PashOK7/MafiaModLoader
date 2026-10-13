"""run_tests.py: the loader's offline tests, through the real bridge (Python standard library only).

Starts build/x86/harness/mml_harness.exe (a stand-in game that loads MafiaModLoader.asi and pumps its frames), with
the test mods (hook_a, hook_b) and the shipped ones (core, hello_c) in its mml/mods, and checks over the bridge:
hello and the token, ping, the mods' list, hook chains in every load and unload order, services, memory read and
write (a write-protected page), a mod faulting in a command (unloaded, the game goes on), state kept across a hot
reload, the core mod with no game, the log. Exit code 0: all passed.

    cmake --build --preset x86-release && python tests/run_tests.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, 'build', 'x86', 'harness')
OUT = os.path.join(ROOT, 'build', 'x86', 'out')


class Bridge:
    def __init__(self, info):
        self.s = socket.create_connection(('127.0.0.1', info['port']), timeout=10)
        self.f = self.s.makefile('rwb')
        self.n = 0
        r = self.call('hello', {'token': info['token']})
        assert r['ok'], r

    def call(self, method, params=None):
        self.n += 1
        self.f.write((json.dumps({'id': self.n, 'method': method, 'params': params or {}}) + '\n').encode())
        self.f.flush()
        reply = json.loads(self.f.readline())
        assert reply['id'] == self.n, reply
        if 'error' in reply:
            return {'ok': False, 'error': reply['error']['message']}
        return {'ok': True, 'result': reply['result']}


failures = []


def check(name, cond, detail=''):
    print(('  ok    ' if cond else '  FAIL  ') + name + ('' if cond else '  -- %s' % detail))
    if not cond:
        failures.append(name)


def main():
    # the harness's game folder: the loader, the test mods and the shipped ones
    mods = os.path.join(HARNESS, 'mml', 'mods')
    for mod in ('core', 'hello_c'):
        shutil.rmtree(os.path.join(mods, mod), ignore_errors=True)
        shutil.copytree(os.path.join(OUT, 'mml', 'mods', mod), os.path.join(mods, mod))
    for stale in ('bridge.json',):
        p = os.path.join(HARNESS, 'mml', stale)
        if os.path.exists(p):
            os.remove(p)
    proc = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '90'], cwd=HARNESS,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    info_path = os.path.join(HARNESS, 'mml', 'bridge.json')
    for _ in range(100):
        if os.path.exists(info_path):
            break
        time.sleep(0.05)
    time.sleep(0.3)
    info = json.load(open(info_path))
    b = Bridge(info)
    try:
        r = b.call('ping')
        check('ping: no Mafia here, the loader runs anyway', r['ok'] and r['result']['build'] == 'unknown', r)
        time.sleep(0.2)
        r = b.call('mods.list')
        loaded = {m['id']: m['loaded'] for m in r['result']}
        check('mods loaded: core, hello_c, hook_a, hook_b', all(loaded.get(m) for m in ('core', 'hello_c', 'hook_a', 'hook_b')), loaded)

        add = lambda: b.call('hook_a.call', {'a': 2, 'b': 3})
        r = add()
        check('chain b(a(add)): (2+3+10)*2 = 30', r.get('result') == 30, r)
        r = b.call('hooks.list')
        chain = r['result'][0]['links'] if r['ok'] and r['result'] else []
        check('hooks.list: one function, two links, hook_b outermost',
              [l['owner'] for l in chain] == ['hook_b', 'hook_a'], r)
        r = b.call('hook_a.math')
        check('service test.math found and called: 42', r.get('result') == 42, r)

        b.call('mod.unload', {'id': 'hook_b'})
        r = add()
        check('hook_b unloaded: a(add) = 15', r.get('result') == 15, r)
        r = b.call('hook_a.math')
        check('its service is gone with it', not r['ok'], r)
        b.call('mod.load', {'id': 'hook_b'})
        check('hook_b back (outermost again): 30', add().get('result') == 30)
        b.call('mod.unload', {'id': 'hook_a'})
        r = b.call('hook_b.call', {'a': 2, 'b': 3})
        check('the inner link removed: b(add) = 10', r.get('result') == 10, r)
        b.call('mod.load', {'id': 'hook_a'})
        r = b.call('hook_b.call', {'a': 2, 'b': 3})
        check('hook_a loaded again, now outermost: a(b(add)) = 20', r.get('result') == 20, r)

        # memory
        base = next(m['base'] for m in b.call('modules')['result'] if m['name'].lower() == 'mml_harness.exe')
        r = b.call('mem.read', {'addr': 'mml_harness.exe+0', 'size': 2, 'as': 'u8'})
        check('mem.read the harness\'s PE header: "MZ"', r['ok'] and r['result']['hex'] == '4d5a', r)
        r = b.call('mem.write', {'addr': hex(base), 'hex': '4d5a'})
        check('mem.write into a read-only page (the same bytes back)', r['ok'], r)
        r = b.call('mem.read', {'addr': '0x10', 'size': 4})
        check('mem.read of an unmapped address: an error, no crash', not r['ok'], r)

        # tap: a link of the same chain, outermost
        r = b.call('tap', {'cmd': 'mml_harness.exe+0 n=2 ret'})
        check('tap: refuses what is not code', r['ok'] and r['result'].startswith('error'), r)
        r = b.call('tap', {'cmd': 'harness_add n=2 ret'})
        check('tap: refuses a name it cannot resolve', r['ok'] and r['result'].startswith('error'), r)
        r = b.call('hooks.list')
        target = r['result'][0]['target']
        before = add().get('result')
        r = b.call('tap', {'cmd': target + ' n=2 ret ptr=c:4'})
        check('tap on the chained function', r['ok'] and r['result'].startswith('tap 0 on'), r)
        check('the chain under the tap gives what it gave', add().get('result') == before, before)
        r = b.call('tap', {'cmd': 'show 0'})
        text = r.get('result', '')
        check('tap show: the call, its arguments 2 and 3, the result',
              '1 calls' in text and 'a1: 2 x1' in text and 'a2: 3 x1' in text and 'eax: %X x1' % before in text, text)
        r = b.call('tap', {'cmd': 'off all'})
        check('tap off: the chain without it the same', r['ok'] and add().get('result') == before, r)

        # a mod that faults
        r = b.call('hook_b.crash')
        check('a fault inside a mod\'s command: an error reply', not r['ok'], r)
        time.sleep(0.2)
        loaded = {m['id']: m['loaded'] for m in b.call('mods.list')['result']}
        check('the faulted mod is unloaded at the next frame', loaded.get('hook_b') is False, loaded)
        check('the game goes on (ping)', b.call('ping')['ok'])
        check('the chain without it: a(add) = 15', add().get('result') == 15)
        b.call('mod.load', {'id': 'hook_b'})

        # state across a hot reload (hello_c keeps its counts)
        s1 = b.call('hello_c.status')['result']
        time.sleep(0.3)
        r = b.call('mod.reload', {'id': 'hello_c'})
        check('mod.reload hello_c: generation 2', r['ok'] and r['result']['generation'] == 2, r)
        s2 = b.call('hello_c.status')['result']
        check('its counts survived the reload', s2['loads'] == 2 and s2['frames'] >= s1['frames'], (s1, s2))

        # the core mod with no game
        r = b.call('game')
        check('core: `game` answers with no game', r['ok'] and r['result']['build'] == 'unknown', r)
        r = b.call('player')
        check('core: `player` says plainly there is no game', not r['ok'], r)

        r = b.call('commands.list')
        names = {c['name'] for c in r['result']}
        check('commands.list has the host\'s and the mods\'', {'mod.reload', 'player', 'hello_c.status'} <= names, names)
        r = b.call('nonexistent.method')
        check('an unknown method: an error', not r['ok'])
        tail = '\n'.join(b.call('log.tail', {'lines': 400})['result'])
        check('the log tells of the fault', 'faulted' in tail)
    finally:
        open(os.path.join(HARNESS, 'quit'), 'w').close()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    check('the harness exits cleanly', proc.returncode == 0, out.decode(errors='replace'))
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
