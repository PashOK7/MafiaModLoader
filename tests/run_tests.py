"""run_tests.py: the loader's offline tests, through the real bridge (Python standard library only).

Starts build/x86/harness/mml_harness.exe (a stand-in game that loads MafiaModLoader.asi and pumps its frames), with
the test mods (hook_a, hook_b, kit) and the shipped ones (core, hello_c) in its mml/mods, and checks over the bridge:
hello and the token, ping, the mods' list, hook chains in every load and unload order, services, memory read and
write (a write-protected page), a mod faulting in a command (unloaded, the game goes on), state kept across a hot
reload, the core mod with no game, the log; and the mod author's kit (kit_tests: the drawing queue, keys and
hotkeys through injected keys, a mod's settings changed by the host, by the bridge and on disk, its data folder).
Exit code 0: all passed.

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


def kit_tests(b):
    """The mod author's kit with no game: the test mod `kit` uses each part, `kit.status` tells what it saw."""
    status = lambda: b.call('kit.status')['result']
    s = status()
    check('kit: a loader with the kit (MML_HAS), key names known', s['has_kit'] == 1 and s['key_f5'] == 0x3F and s['key_bad'] == -1, s)

    # drawing: queued every frame, never drawn here (no game's 2D pass), dropped at the next frame
    r = b.call('draw.state')
    check('draw: the mod\'s 4 items a frame are queued; no 2D pass to draw them in',
          r['ok'] and r['result']['queued_last_frame'] == 4 and r['result']['hooked'] is False and r['result']['passes'] == 0, r)
    check('draw: a text\'s width is 0 with no game', s['text_width'] == 0, s)
    r = b.call('draw.text', {'x': 400, 'y': 300, 'text': 'hello', 'size': 30, 'color': '#80FF0000', 'align': 'center', 'ms': 60000})
    check('draw.text from the bridge: held, and it says nothing will be drawn', r['ok'] and r['result']['held'] == 1 and 'note' in r['result'], r)
    b.call('draw.rect', {'x': 1, 'y': 1, 'w': 5, 'h': 5})
    b.call('draw.line', {'x0': 0, 'y0': 0, 'x1': 800, 'y1': 600, 'ms': 100})
    time.sleep(0.3)
    r = b.call('draw.state')
    check('draw: the line\'s time ran out, the two others are held', r['ok'] and r['result']['held'] == 2, r)
    r = b.call('draw.text', {'x': float('1e30'), 'y': 0, 'text': 'x'})
    check('draw.text: a coordinate that is no usable number is refused', not r['ok'], r)
    r = b.call('draw.clear')
    check('draw.clear', r['ok'] and r['result']['cleared'] == 2 and b.call('draw.state')['result']['held'] == 0, r)

    # keys: with no game the keyboard is what input.key holds; mods see it as they would the real one
    r = b.call('input.key', {'key': 'F5', 'frames': 5})
    check('input.key with no game: the key is held for mods', r['ok'], r)
    time.sleep(0.3)
    s2 = status()
    check('key_down for the 5 frames it was held, key_pressed once', s2['f5_down'] - s['f5_down'] == 5 and s2['f5_pressed'] - s['f5_pressed'] == 1, (s, s2))
    b.call('input.key', {'key': 'F5', 'frames': 2})
    time.sleep(0.2)
    check('pressed again: once more', status()['f5_pressed'] - s['f5_pressed'] == 2)

    # hotkeys: the combination's modifiers, exactly
    b.call('input.key', {'key': 'F6', 'frames': 2})
    time.sleep(0.2)
    check('hotkey Ctrl+F6: F6 alone does not fire it', status()['hot_ctrl_f6'] == 0, status())
    b.call('input.key', {'key': 'Ctrl', 'frames': 12})
    b.call('input.key', {'key': 'F6', 'frames': 2, 'after_frames': 2})
    time.sleep(0.4)
    check('Ctrl held, F6 pressed: fired once', status()['hot_ctrl_f6'] == 1, status())
    b.call('input.key', {'key': 'Ctrl', 'frames': 14})
    b.call('input.key', {'key': 'Shift', 'frames': 14})
    b.call('input.key', {'key': 'F6', 'frames': 2, 'after_frames': 3})
    time.sleep(0.4)
    check('Ctrl+Shift+F6 is another combination: not fired', status()['hot_ctrl_f6'] == 1, status())
    b.call('input.key', {'key': 'F7', 'frames': 2})
    time.sleep(0.2)
    check('the hotkey from the settings ("hotkey": "F7") fires', status()['hot_setting'] == 1, status())
    r = b.call('input.state')
    check('input.state lists the mods\' hotkeys', r['ok'] and {'mod': 'kit', 'combo': 'Ctrl+F6'} in r['result']['hotkeys'], r)

    # settings: the defaults shipped with the mod, changes in its data folder, the mod told
    s = status()
    check('config: the defaults (a number, a string, a path, a boolean as 1, a missing key\'s default)',
          s['speed'] == 12.5 and s['name'] == 'kit' and s['overlay_x'] == 10 and s['on'] == 1 and s['missing'] == -7
          and s['overlay'] == {'x': 10, 'on': True}, s)
    r = b.call('config.get', {'mod': 'kit'})
    check('config.get: the whole config, nothing changed yet', r['ok'] and r['result']['value']['speed'] == 12.5 and r['result']['changes'] == {}, r)
    r = b.call('config.set', {'mod': 'kit', 'key': 'overlay.x', 'value': 55})
    check('config.set of a path', r['ok'] and r['result']['value'] == 55 and r['result']['default'] == 10, r)
    time.sleep(0.2)
    s2 = status()
    check('the mod got MML_EVENT_CONFIG_CHANGED (its own id) and reads the new value; the rest stays',
          s2['config_events'] == s['config_events'] + 1 and s2['config_event_name'] == 'kit' and s2['overlay'] == {'x': 55, 'on': True}, s2)
    changes_path = os.path.join(HARNESS, 'mml', 'data', 'kit', 'config.json')
    check('the change is in the data folder\'s config.json, and only the change',
          os.path.exists(changes_path) and json.load(open(changes_path)) == {'overlay': {'x': 55}}, changes_path)
    check('config_set from the mod itself', b.call('kit.set_speed', {'value': 30}).get('result') is True)
    time.sleep(0.2)
    check('... is read back and told', status()['speed'] == 30 and status()['config_events'] == s['config_events'] + 2, status())
    # edited by hand on disk (as a person or an agent would): read again within a second
    time.sleep(0.05)
    with open(changes_path, 'w') as f:
        f.write('{ "speed": 31, "hotkey": "F8" }  // by hand\n')
    time.sleep(1.2)
    s3 = status()
    check('the file edited on disk is read again: new values, the mod told, a removed change back to its default',
          s3['speed'] == 31 and s3['hotkey'] == 'F8' and s3['overlay_x'] == 10 and s3['config_events'] == s['config_events'] + 3, s3)
    b.call('input.key', {'key': 'F7', 'frames': 2})
    b.call('input.key', {'key': 'F8', 'frames': 2, 'after_frames': 4})
    time.sleep(0.4)
    check('the hotkey bound again from the new setting: F8 fires, F7 no more', status()['hot_setting'] == 2, status())
    with open(changes_path, 'w') as f:
        f.write('{ "speed": ')
    time.sleep(1.2)
    check('a file that does not parse: the values in use stay', status()['speed'] == 31, status())
    r = b.call('config.set', {'mod': 'kit', 'key': 'speed', 'value': None})
    check('config.set null: back to the default', r['ok'] and r['result']['value'] == 12.5, r)
    r = b.call('config.set', {'mod': 'kit', 'key': 'speed', 'value': 77})
    b.call('mod.reload', {'id': 'kit'})
    check('settings survive the mod\'s reload', status()['speed'] == 77, status())
    r = b.call('config.get', {'mod': 'nobody'})
    check('config.get of an unknown mod: an error', not r['ok'], r)

    # the data folder
    r = b.call('kit.write')
    want = os.path.join(HARNESS, 'mml', 'data', 'kit', 'note.txt')
    check('mod_data_dir: <game>\\mml\\data\\<id>\\, writable', r['ok'] and os.path.normcase(r['result']) == os.path.normcase(want)
          and os.path.exists(want), r)

    # the mission: none here
    s = status()
    check('no game: no mission, no mission events, no actions', s['mission'] == '' and s['mission_state'] == 0
          and s['mission_events'] == 0 and s['slot_accelerate'] == -1, s)
    # the game's menus: none here. An item asked for is kept (shown once the menus are known); a page is refused
    s = status()
    check('menu_item: a handle, kept with no menus to show it in', s['menu_item'] > 0, s)
    r = b.call('menu.items')
    kits = [i for i in r['result']['items'] if i['owner'] == 'kit'] if r['ok'] else []
    check('menu.items: the menus not known, the kit\'s item listed once after its reloads',
          r['ok'] and r['result']['known'] is False and len(kits) == 1 and kits[0]['menu'] == 'gamemenu' and kits[0]['after'] == 108, r)
    r = b.call('kit.menu')
    check('menu_open with no menus: refused; menu_set_value, menu_value, menu_set_text: no page, 0',
          r['ok'] and r['result'] == {'open': 0, 'set_value': 0, 'value': 0, 'set_text': 0}, r)
    r = b.call('kit.script')
    check('script_run with no game: refused (0); script_state of no such program: -1',
          r['ok'] and r['result'] == {'run': 0, 'state': -1}, r)
    r = b.call('script.run', {'code': 'end'})
    check('script.run with no game: an error, saying why', not r['ok'] and 'not known' in str(r), r)
    r = b.call('script.list')
    check('script.list with no game: known false, none', r['ok'] and r['result'] == {'known': False, 'scripts': []}, r)
    r = b.call('kit.world')
    check('actor_spawn, actor_remove, actor_teleport, item_id, give_item, actor_seat with no game: refused',
          r['ok'] and r['result'] == {'spawn': 0, 'remove': 0, 'teleport': 0, 'item': -1, 'give': 0, 'seat': 0}, r)
    r = b.call('world.spawn', {'car': 'Bolt Ace Tudor'})
    check('world.spawn with no game: an error, saying why', not r['ok'] and 'no car' in str(r), r)
    r = b.call('world.spawned')
    check('world.spawned with no game: known false, none', r['ok'] and r['result'] == {'known': False, 'spawned': []}, r)
    r = b.call('world.cars')
    check('world.cars with no game: none', r['ok'] and r['result']['count'] == 0, r)
    r = b.call('world.teleport', {'to': [0, 0, 0]})
    check('world.teleport with no game: an error', not r['ok'], r)
    r = b.call('menu.page', {'name': 'x', 'controls': [{'id': 1, 'kind': 'wind', 'x': 0, 'y': 0, 'w': 100, 'h': 100}, {'kind': 'end'}]})
    check('menu.page with no menus: refused, saying why', not r['ok'] and 'not known' in str(r), r)
    r = b.call('menu.item', {'menu': 'gamemenu', 'after': 108, 'text': 'Bridge'})
    check('menu.item from the bridge: kept, and it says it will not be shown', r['ok'] and r['result']['handle'] > 0 and 'not known' in r['result']['note'], r)
    r = b.call('menu.item.remove', {'handle': r['result']['handle']})
    check('menu.item.remove', r['ok'], r)
    r = b.call('menu.def', {'name': 'gamemenu'})
    check('menu.def with no game\'s files: an error, saying why', not r['ok'] and 'rw_data' in str(r), r)
    r = b.call('menu.click', {'id': 102})
    check('menu.click with no menus: an error', not r['ok'], r)
    r = b.call('input.mouse', {'mouse': [100, 100]})
    check('input.mouse with no game: the mouse is not hooked, said plainly', not r['ok'] and 'not hooked' in str(r), r)
    r = b.call('input.text', {'text': 'Ab 1!'})
    check('input.text with no game: the keys held for mods (Shift for A and !)', r['ok'] and r['result']['keys'] == 7, r)
    r = b.call('input.text', {'text': 'é'})
    check('input.text: a character the US layout has no key for is refused', not r['ok'], r)
    time.sleep(0.8)
    names = {c['name'] for c in b.call('commands.list')['result']}
    check('commands.list has the kit\'s methods', {'draw.text', 'draw.rect', 'draw.line', 'draw.clear', 'draw.state',
                                                   'config.get', 'config.set'} <= names, names)


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
    shutil.rmtree(os.path.join(HARNESS, 'mml', 'data'), ignore_errors=True)   # the mods' data folders: a clean start
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
        check('mods loaded: core, hello_c, hook_a, hook_b, kit',
              all(loaded.get(m) for m in ('core', 'hello_c', 'hook_a', 'hook_b', 'kit')), loaded)

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
        check('commands.list has the input, screenshot and menu methods',
              {'input.actions', 'input.act', 'input.key', 'input.seq', 'input.release', 'screenshot', 'menu.value'} <= names, names)
        # eyes and hands with no game: plain errors, nothing touched
        r = b.call('input.seq', {'steps': [{'action': 'Forwards', 'ms': 100}]})
        check('input: says plainly the build is not known', not r['ok'] and 'not known' in str(r), r)
        r = b.call('screenshot')
        check('screenshot: says plainly the device is not known', not r['ok'] and 'device' in str(r), r)
        r = b.call('menu.value', {'id': 21})
        check('menu.value: says plainly the menus are not known', not r['ok'] and 'not known' in str(r), r)
        kit_tests(b)
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
