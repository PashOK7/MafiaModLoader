"""Focused x86 hook-chain regression. Uses an isolated stand-in game, no installed games or desktop input.

Build MafiaModLoader, mml_harness, hook_a, hook_b and kit first; python tests/test_hook_chain_thunk.py.
"""
import itertools
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

from run_tests import Bridge

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / 'build/x86/harness'


def main():
    folder = Path(tempfile.mkdtemp(prefix='hook-chain-', dir=ROOT / 'build'))
    for name in ('mml_harness.exe', 'rw_data.dll'):
        shutil.copyfile(HARNESS / name, folder / name)
    for name in ('MafiaModLoader.asi', 'MafiaModLoader.pdb'):
        shutil.copyfile(ROOT / 'build/x86/out' / name, folder / name)
    for mod in ('hook_a', 'hook_b', 'kit'):
        shutil.copytree(HARNESS / 'mml/mods' / mod, folder / 'mml/mods' / mod)
    proc = subprocess.Popen([str(folder / 'mml_harness.exe'), '--seconds', '40'], cwd=folder,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    checks = 0
    def check(name, condition, detail=None):
        nonlocal checks
        assert condition, (name, detail)
        checks += 1
        print('PASS:', name, flush=True)
    try:
        info = folder / 'mml/bridge.json'
        deadline = time.monotonic() + 8
        while not info.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        bridge = Bridge(json.loads(info.read_text(encoding='utf-8')))
        def call(method, **params):
            reply = bridge.call(method, params)
            assert reply['ok'], (method, reply)
            return reply['result']
        def read(address, size):
            return bytes.fromhex(call('mem.read', addr=address, size=size)['hex'])
        target = next(h['target'] for h in call('hooks.list') if h['target'].startswith('mml_harness.exe'))
        target = target.split(' ')[0]
        def thunk():
            entry = call('mem.read', addr=target, size=5)
            data = bytes.fromhex(entry['hex'])
            check('function enters its chain through a relative jump', data[0] == 0xE9)
            address = (entry['abs'] + 5 + struct.unpack_from('<i', data, 1)[0]) & 0xffffffff
            code = read(hex(address), 7)
            check('absolute SIB jump avoids RTSS RIP-relative recognizer', code[:3] == bytes.fromhex('ff2425'), code.hex())
            head = struct.unpack_from('<I', code, 3)[0]
            check('chain head points to executable detour code', len(read(hex(struct.unpack('<I', read(hex(head), 4))[0]), 1)) == 1)
            return address, code
        original = thunk()
        check('both hooks execute through the emitted thunk', call('hook_a.call', a=2, b=3) == 30)
        for order in itertools.permutations(('hook_a','hook_b')):
            for mod in order:
                call('mod.unload', id=mod)
            for mod in order:
                call('mod.load', id=mod)
            expected = 30 if order[-1] == 'hook_b' else 20
            check('recreated chain executes in '+str(order), call('hook_a.call', a=2, b=3) == expected)
            current = thunk()
            call('mod.unload', id=order[-1])
            check('removing outer hook preserves immutable thunk instructions', thunk() == current)
            remaining = order[0]
            expected = 15 if remaining == 'hook_a' else 10
            check('remaining hook still executes', call(remaining+'.call', a=2, b=3) == expected)
            call('mod.load', id=order[-1])
        mid = call('kit.midhook')
        check('mid-hook preserves argument, live x87 and continuation', mid['before']==6 and mid['with']==106
              and mid['arg']==2 and mid['after']==6 and mid['skipped']==5, mid)
        check('mid-hook preserves incoming zero and nonzero flags', mid['flags_kept'], mid)
        print('Evidence:', folder, checks, 'checks', flush=True)
    finally:
        with (folder / 'quit').open('x'):
            pass
        try:
            output, _ = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, _ = proc.communicate()
        assert proc.returncode == 0, output.decode(errors='replace')


if __name__ == '__main__':
    main()
