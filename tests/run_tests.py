"""run_tests.py: the loader's offline tests, through the real bridge (Python standard library only).

Starts build/x86/harness/mml_harness.exe (a stand-in game that loads MafiaModLoader.asi and pumps its frames), with
the test mods (hook_a, hook_b, kit, heavy, files_a, files_b) and the shipped ones (core, hello_c, boxes) in its mml/mods, and checks over the bridge:
hello and the token, ping, the mods' list, hook chains in every load and unload order, services, memory read and
write (a write-protected page), a mod faulting in a command (unloaded, the game goes on), state kept across a hot
reload, the core mod with no game, the log; the mod author's kit (kit_tests: the drawing queue, keys and
hotkeys through injected keys, a mod's settings changed by the host, by the bridge and on disk, its data folder);
a mod's own files in the game (files_tests: a stand-in rw_data.dll's names answered from the mods' `files` folders,
the order, changes on disk, file_add, a file held open replaced, loads and unloads);
and a heavy mod's lifecycle (heavy_tests: its libraries in each load's folder, threads left in its code, leaks; boxes_tests:
the heavy example, Jolt and its own shaders on a Direct3D 11 device, reloaded 20 times).
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
    print(('  ok    ' if cond else '  FAIL  ') + name + ('' if cond else '  -- %s' % (detail,)))
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
    check('menu.def with no game\'s menus: an error, saying why', not r['ok'] and 'gamemenu.mnu' in str(r), r)
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


def heavy_tests(b):
    """A heavy mod's lifecycle (host/lifecycle.cpp): its libraries found in each load's own folder, its folder never
    locked, threads still in its code after the unload, what the process did not get back."""
    live = os.path.normcase(os.path.join(HARNESS, 'mml', 'live'))
    norm = lambda p: os.path.normcase(os.path.normpath(p))
    mod_dir = os.path.join(HARNESS, 'mml', 'mods', 'heavy')
    info = lambda: next(m for m in b.call('mods.list')['result'] if m['id'] == 'heavy')

    s = b.call('heavy.status')['result']
    load_dir = norm(info()['live_dir'])
    check('heavy: the library it imports is found and runs (4242)', s['lib'] == 4242, s)
    check('heavy: its DLL and both libraries run from the load\'s own folder in mml\\live',
          load_dir.startswith(live) and all(norm(os.path.dirname(s[k])) == load_dir for k in ('self', 'lib_path', 'rt_path'))
          and os.path.basename(s['self']).startswith('mod_heavy_'), (s, load_dir))
    check('heavy: the library loaded by its bare name in mml_mod_load runs (77)', s['rt'] == 77, s)
    try:
        with open(os.path.join(mod_dir, 'heavylib.dll'), 'r+b'):
            unlocked = True
    except OSError:
        unlocked = False
    check('heavy: the mod\'s own folder is not locked (a build can replace its libraries)', unlocked)

    r = b.call('mod.reload', {'id': 'heavy'})
    u = r['result'].get('last_unload', {}) if r['ok'] else {}
    check('heavy: a clean reload: nothing kept, nothing left loaded, the old folder deleted',
          r['ok'] and 'kept' not in u and 'stays_loaded' not in u and not os.path.exists(load_dir), (r, load_dir))

    b.call('heavy.leak', {'mb': 16, 'handles': 5})
    r = b.call('mod.reload', {'id': 'heavy'})
    since = r['result']['last_unload']['since_load'] if r['ok'] else {}
    check('heavy: 16 MB and 5 handles never given back are measured at the unload',
          since.get('free_mb', 0) <= -15.5 and since.get('handles', 0) >= 5, r)

    r = b.call('mod.reload', {'id': 'heavy', 'times': 5})
    res = r.get('result', {})
    check('mod.reload times 5: five rounds measured, a clean mod loses no address space per reload',
          r['ok'] and len(res['rounds']) == 5 and abs(res['per_reload']['free_mb']) < 1, r)
    check('mod.reload times out of range: refused', not b.call('mod.reload', {'id': 'heavy', 'times': 0})['ok'])

    # a thread the mod stopped in mml_mod_unload: freed at once
    b.call('heavy.thread', {'ms': 60000})
    r = b.call('mod.reload', {'id': 'heavy'})
    check('heavy: its thread stopped in mml_mod_unload: freed, nothing kept',
          r['ok'] and 'kept' not in r['result']['last_unload'] and 'kept_generations' not in r['result'], r)

    # a thread left running in its code: the DLL is kept until the thread leaves it
    gen = info()['generation']
    b.call('heavy.thread', {'ms': 2500, 'leave': True})
    r = b.call('mod.reload', {'id': 'heavy'})
    u = r['result']['last_unload'] if r['ok'] else {}
    check('heavy: a thread still in its code: the old DLL kept, the thread named in it',
          u.get('kept') is True and u['threads'] and u['threads'][0]['where'].startswith('mod_heavy_')
          and r['result'].get('kept_generations') == [gen], r)
    check('heavy: ... the game goes on, the new load answers', b.call('heavy.status').get('result', {}).get('lib') == 4242)
    time.sleep(3.5)
    check('heavy: once the thread has ended, the kept DLL is freed', 'kept_generations' not in info(), info())
    tail = '\n'.join(b.call('log.tail', {'lines': 200})['result'])
    check('heavy: the log tells of the kept load and its freeing', 'still in its code' in tail and 'freed now' in tail)

    # a thread of the system's pool in its code: begun elsewhere, seen through the frames it will return through
    gen = info()['generation']
    b.call('heavy.pool', {'ms': 2500})
    time.sleep(0.1)
    r = b.call('mod.reload', {'id': 'heavy'})
    u = r['result']['last_unload'] if r['ok'] else {}
    check('heavy: a pool thread sleeping in its code: found by its frames ("returns to"), the old DLL kept',
          u.get('kept') is True and any(t['how'] == 'returns to' for t in u['threads'])
          and r['result'].get('kept_generations') == [gen], r)
    time.sleep(3.5)
    check('heavy: ... freed once the work item has returned', 'kept_generations' not in info(), info())

    # a library it loaded and never freed: said at the unload, and the next load is told it uses the old copy
    b.call('heavy.leak_rt')
    r = b.call('mod.reload', {'id': 'heavy'})
    check('heavy: a library it never freed is named at the unload',
          r['ok'] and r['result']['last_unload'].get('stays_loaded') == ['heavyrt.dll'], r)
    tail = '\n'.join(b.call('log.tail', {'lines': 50})['result'])
    check('heavy: the next load is warned that heavyrt.dll is not its own copy', 'its library heavyrt.dll is' in tail, tail[-600:])

    # a library it imports missing from its folder: the load fails, saying why
    b.call('mod.unload', {'id': 'heavy'})
    os.replace(os.path.join(mod_dir, 'heavylib.dll'), os.path.join(mod_dir, 'heavylib.off'))
    try:
        r = b.call('mod.load', {'id': 'heavy'})
        check('heavy: an imported library missing: the load fails and says a DLL was not found',
              not r['ok'] and 'neither in its folder' in r['error'], r)
    finally:
        os.replace(os.path.join(mod_dir, 'heavylib.off'), os.path.join(mod_dir, 'heavylib.dll'))
    r = b.call('mod.load', {'id': 'heavy'})
    check('heavy: the library back: it loads', r['ok'] and b.call('heavy.status')['result']['lib'] == 4242, r)


def boxes_tests(b):
    """The heavy example (mods/examples/boxes): a pinned library (Jolt) and shaders built with it, a Direct3D 11 device
    of its own (the GPU, or WARP where there is none), all of it given back at each reload."""
    status = lambda: b.call('boxes.status')['result']
    r = b.call('mod.load', {'id': 'boxes'})
    check('boxes: loaded again (its world kept from the first load)', r['ok'], r)
    time.sleep(0.3)
    s = status()
    check('boxes: Jolt\'s world holds its 48 boxes and steps them with its job threads',
          s['boxes'] == 48 and s['threads'] == 2 and s['steps'] > 0, s)
    check('boxes: the Direct3D 11 device was made and runs the mod\'s shaders (frames drawn, read back)',
          s['gpu']['device'] and s['gpu']['frames'] > 0 and s['gpu']['removed_reason'] == '0x00000000', s)
    time.sleep(0.5)
    s2 = status()
    check('boxes: the boxes fall (awake, steps go on) and the GPU sees them (pixels covered, counted by its compute shader)',
          s2['steps'] > s['steps'] and s2['awake'] > 0 and s2['gpu']['read_frame'] > 0 and s2['gpu']['covered_px'] > 0, s2)

    r = b.call('boxes.snapshot')
    path = r['result']['path'] if r['ok'] else ''
    data = open(path, 'rb').read() if path and os.path.exists(path) else b''
    side = int.from_bytes(data[18:22], 'little') if len(data) > 54 else 0
    pixels = data[54:]
    background = bytes(pixels[0:4]) if pixels else b''
    lit = sum(1 for i in range(0, len(pixels), 4) if pixels[i:i + 4] != background)
    check('boxes.snapshot: the GPU\'s picture as a BMP in the data folder, the boxes on it',
          r['ok'] and data[:2] == b'BM' and side == 256 and lit > 500, (r, side, lit))

    # a reload: the device released to its last reference, the job threads joined, the world kept
    before = status()
    r = b.call('mod.reload', {'id': 'boxes'})
    u = r['result'].get('last_unload', {}) if r['ok'] else {}
    s = status()
    check('boxes: a reload keeps nothing mapped, no library of its folder stays loaded',
          r['ok'] and 'kept' not in u and 'stays_loaded' not in u, r)
    check('boxes: ... the world goes on from its bodies (restored, the same pile), a new device made, the old one released whole',
          s['restored'] and s['boxes'] == before['boxes'] and s['drops'] == before['drops'] and s['load'] == before['load'] + 1
          and s['history']['devices_made'] == before['history']['devices_made'] + 1 and s['history']['last_unload_refs_left'] == 0
          and s['gpu']['device'], (before, s))
    # (that load's life held the tests above, and the harness's start the process's first device: their numbers are
    # theirs. The next load lives only for this reload: its own cost. A GPU's driver may keep a little per device:
    # NVIDIA's a section and a mutex, measured; WARP nothing)
    time.sleep(0.5)
    r = b.call('mod.reload', {'id': 'boxes'})
    since = r['result']['last_unload'].get('since_load', {}) if r['ok'] else {}
    check('boxes: one load\'s life: no thread, no address space, at most a driver\'s few handles not given back',
          r['ok'] and since.get('threads') == 0 and since.get('free_mb', -9) > -1 and since.get('handles', 9) <= 4, r)
    s = status()

    r = b.call('mod.reload', {'id': 'boxes', 'times': 20, 'timeout_ms': 120000})
    res = r.get('result', {})
    per = res.get('per_reload', {})
    check('boxes: 20 reloads in a row: no thread, no address space and few handles lost a reload',
          r['ok'] and len(res['rounds']) == 20 and per['threads'] == 0 and per['free_mb'] > -1 and per['handles'] <= 4, r)
    time.sleep(0.3)
    s2 = status()
    check('boxes: ... after them the device is the 23rd and it draws (frames, pixels), none lost',
          s2['history']['devices_made'] == s['history']['devices_made'] + 20 and s2['history']['devices_lost'] == 0
          and s2['gpu']['device'] and s2['gpu']['frames'] > 0 and s2['gpu']['covered_px'] > 0
          and s2['history']['last_unload_refs_left'] == 0, s2)

    # settings: the pool made again with another count, the device switched off and on
    b.call('config.set', {'mod': 'boxes', 'key': 'threads', 'value': 0})
    time.sleep(0.4)
    s3 = status()
    time.sleep(0.3)
    check('boxes: threads 0: the game thread steps alone', s3['threads'] == 0 and status()['steps'] > s3['steps'], s3)
    b.call('config.set', {'mod': 'boxes', 'key': 'gpu', 'value': False})
    time.sleep(0.3)
    check('boxes: gpu false: the device released', not status()['gpu']['device'], status())
    b.call('config.set', {'mod': 'boxes', 'key': 'gpu', 'value': None})
    b.call('config.set', {'mod': 'boxes', 'key': 'threads', 'value': None})
    time.sleep(0.4)
    s4 = status()
    check('boxes: back to the defaults: a device again, two threads', s4['gpu']['device'] and s4['threads'] == 2, s4)


def files_tests(b):
    """A mod's own files in the game: rw_data's dtaOpen (the stand-in's) answered from the mods' files\\ folders.
    files_a ships files alone (no DLL, load order 40); files_b (50) ships two and opens names as the game does."""
    mods = os.path.join(HARNESS, 'mml', 'mods')
    fa, fb = os.path.join(mods, 'files_a', 'files'), os.path.join(mods, 'files_b', 'files')
    opn = lambda name: b.call('files_b.open', {'name': name})['result']
    st = lambda: b.call('files_b.status')['result']

    ms = {m['id']: m for m in b.call('mods.list')['result']}
    check('files: a mod of files alone is loaded (no DLL), its 3 files counted; files_b\'s 2',
          ms['files_a']['loaded'] and ms['files_a']['type'] == 'files' and ms['files_a'].get('files') == 3
          and ms['files_b'].get('files') == 2 and 'live_dir' not in ms['files_a'], (ms.get('files_a'), ms.get('files_b')))
    check('mods.list: a mod\'s builds ("builds" in mod.json; with no known game every mod loads)',
          ms['kit'].get('builds') == ['1.2', '1.0'] and ms['kit']['loaded'] and 'builds' not in ms['files_a'], ms.get('kit'))
    check('files: one name in two mods: the later in load order answers', opn('tables\\test.txt') == {'ok': True, 'text': 'files_b'})
    check('files: names as the archives compare them (case, "/")', opn('TABLES/Test.TXT').get('text') == 'files_b'
          and opn('maps\\crate.bmp').get('text') == 'a crate')
    check('files: a name only the lower mod ships', opn('tables\\a_only.txt').get('text') == 'a only')
    check('files: a mod\'s file over the archive\'s', opn('archived.txt').get('text') == 'b over the archive')
    check('files: a name no mod ships: the archive\'s', opn('only_archive.txt').get('text') == 'the archive\'s own')
    check('files: a name nobody has: dtaOpen fails as before', opn('nothing.txt') == {'ok': False})
    check('files: the game\'s presaved copies of a mod\'s picture are not given (LS3D makes them from the picture)',
          opn('Maps\\crate_1.565') == {'ok': False} and opn('Maps\\CRATE_2L.DX1') == {'ok': False})
    check('files: a presaved copy of a picture no mod ships is given', opn('Maps\\other_1.565').get('text') == 'presaved')
    r = b.call('files.which', {'name': 'maps\\crate_1.565'})
    check('files.which: a presaved copy hidden, for which picture', r['ok'] and r['result'].get('hidden_for') == 'maps\\crate.bmp', r)

    r = b.call('files.which', {'name': 'Tables/TEST.txt'})
    check('files.which: who answers, over whom, the game has none',
          r['ok'] and r['result']['name'] == 'tables\\test.txt' and r['result']['mod'] == 'files_b'
          and [o['mod'] for o in r['result']['over']] == ['files_a'] and r['result']['game_has'] is False, r)
    r = b.call('files.which', {'name': 'only_archive.txt'})
    check('files.which: a name only the game has', r['ok'] and 'mod' not in r['result'] and r['result']['game_has'] is True
          and r['result']['game_size'] == len("the archive's own"), r)
    r = b.call('files.which', {'name': 'C:\\Windows\\win.ini'})
    check('files.which: a name outside the game\'s folder is refused', not r['ok'], r)
    r = b.call('files.read', {'name': 'archived.txt'})
    check('files.read: what the game reads (the mod\'s)', r['ok'] and r['result']['source'] == 'mod' and r['result']['text'] == 'b over the archive', r)
    r = b.call('files.read', {'name': 'archived.txt', 'game': True})
    check('files.read game: the game\'s own under the mod\'s', r['ok'] and r['result']['source'] == 'game' and r['result']['text'] == 'the archive\'s own', r)
    r = b.call('files.read', {'name': 'archived.txt', 'offset': 2, 'size': 4})
    check('files.read: offset and size', r['ok'] and r['result']['text'] == 'over' and r['result']['size'] == 18, r)
    r = b.call('files_b.read', {'name': 'archived.txt'})
    check('file_read: the size and what the game reads', r['result'] == {'size': 18, 'text': 'b over the archive'}, r)
    r = b.call('files_b.read', {'name': 'archived.txt', 'game': True})
    check('file_read MML_FILE_GAME: the game\'s own', r['result']['text'] == 'the archive\'s own', r)
    check('file_read: no such file: -1', b.call('files_b.read', {'name': 'nothing.txt'})['result']['size'] == -1)
    r = b.call('files_b.source', {'name': 'tables/test.txt'})['result']
    check('file_source: a mod\'s, its full path', r['rc'] == 1 and r['path'].lower() == os.path.join(fb, 'tables', 'test.txt').lower(), r)
    check('file_source: the game\'s own: 0', b.call('files_b.source', {'name': 'only_archive.txt'})['result']['rc'] == 0)
    r = b.call('files.list', {'mod': 'files_a'})
    rows = {f['name']: f for f in r['result']['files']} if r['ok'] else {}
    check('files.list: a mod\'s names, who answers each, how often the game opened it',
          r['ok'] and rows['tables\\test.txt']['mod'] == 'files_b' and rows['tables\\test.txt']['over'] == ['files_a']
          and rows['tables\\a_only.txt']['opens'] >= 1 and r['result']['mods'][0]['watched'] is True, r)

    # changes on disk: the next open reads the new file, every mod is told the name
    e0 = st()['events']
    with open(os.path.join(fa, 'tables', 'a_only.txt'), 'w') as f:
        f.write('a changed')
    time.sleep(0.8)
    s = st()
    check('files: a file changed on disk: the mods told its name, the game reads the new one',
          s['events'] == e0 + 1 and s['names'][-1] == 'tables\\a_only.txt' and opn('tables\\a_only.txt').get('text') == 'a changed', s)
    with open(os.path.join(fa, 'tables', 'test.txt'), 'w') as f:
        f.write('files_a changed')
    time.sleep(0.8)
    check('files: a change under another mod\'s file is no change of what the game reads: nobody told',
          st()['events'] == e0 + 1, st())
    with open(os.path.join(fa, 'tables', 'new.txt'), 'w') as f:
        f.write('new')
    time.sleep(0.8)
    check('files: a file come on disk: told, opened', st()['names'][-1] == 'tables\\new.txt' and opn('tables\\new.txt').get('text') == 'new', st())
    os.remove(os.path.join(fa, 'tables', 'new.txt'))
    time.sleep(0.8)
    check('files: a file gone: told, the name is nobody\'s again', st()['names'][-1] == 'tables\\new.txt' and opn('tables\\new.txt') == {'ok': False}, st())

    # a file the game holds open is replaced (a deploy while a sound streams): the game reads the old one to its close.
    # As the MCP server's deploy replaces it: a copy beside it, the old one deleted (a rename over a file open, even
    # sharing deletion, is refused by Windows), the copy renamed.
    check('files: a mod\'s file held open', b.call('files_b.hold', {'name': 'archived.txt'})['result']['ok'])
    tmp, dst = os.path.join(fb, 'archived.txt.mmltmp'), os.path.join(fb, 'archived.txt')
    with open(tmp, 'w') as f:
        f.write('b replaced')
    replaced = True
    try:
        os.remove(dst)
        os.rename(tmp, dst)
    except OSError as ex:
        replaced = str(ex)
    check('files: a file the game holds open can be replaced', replaced is True, replaced)
    r = b.call('files_b.release')['result']
    check('files: the held file reads the old one to its close', r == {'ok': True, 'text': 'b over the archive'}, r)
    time.sleep(0.8)
    check('files: then the new one is opened', opn('archived.txt').get('text') == 'b replaced')
    check('files: a deploy\'s copy on its way (*.mmltmp) is never a game file', 'archived.txt.mmltmp' not in
          {f['name'] for f in b.call('files.list', {'mod': 'files_b'})['result']['files']})

    # file_add: a game name answered by a file of the mod's choosing (a patched copy in its data folder)
    path = b.call('files_b.write', {'file': 'patched.txt', 'text': 'patched'})['result']['path']
    e1 = st()['events']
    check('file_add: a file of the data folder for a name', b.call('files_b.add', {'name': 'tables\\test.txt', 'path': path})['result']['rc'] == 1)
    check('file_add: answered at once', opn('tables\\test.txt').get('text') == 'patched')
    time.sleep(0.2)
    check('file_add: the mods told', st()['events'] == e1 + 1, st())
    b.call('files_b.write', {'file': 'patched.txt', 'text': 'patched again'})
    time.sleep(1.5)
    check('file_add: its file changed on disk: seen (looked at once a second)', opn('tables\\test.txt').get('text') == 'patched again'
          and st()['events'] == e1 + 2, st())
    check('file_add: taken back ("")', b.call('files_b.add', {'name': 'tables\\test.txt', 'path': ''})['result']['rc'] == 1
          and opn('tables\\test.txt').get('text') == 'files_b')
    check('file_add: a name outside the game\'s folder is refused', b.call('files_b.add', {'name': '..\\x.txt', 'path': path})['result']['rc'] == 0)
    check('file_add: no such file is refused', b.call('files_b.add', {'name': 'x.txt', 'path': 'nothing\\here.txt'})['result']['rc'] == 0)

    # loads and unloads
    r = b.call('mod.reload', {'id': 'files_b'})
    time.sleep(0.3)
    check('files: a reload keeps its files and tells nobody (nothing changed)', r['ok'] and st()['events'] == 0
          and opn('archived.txt').get('text') == 'b replaced', st())
    b.call('mod.unload', {'id': 'files_a'})
    time.sleep(0.3)
    s = st()
    check('files: a mod of files unloaded: its names are the game\'s again, the mods told',
          opn('tables\\a_only.txt') == {'ok': False} and opn('maps\\crate.bmp') == {'ok': False}
          and set(s['names']) == {'tables\\a_only.txt', 'maps\\crate.bmp'}, s)
    check('files: the name both shipped stays files_b\'s', opn('tables\\test.txt').get('text') == 'files_b')
    b.call('mod.load', {'id': 'files_a'})
    check('files: loaded again', opn('tables\\a_only.txt').get('text') == 'a changed')
    b.call('mod.unload', {'id': 'files_b'})
    check('files: files_b unloaded: files_a\'s file answers the name', b.call('files.read', {'name': 'tables\\test.txt'})['result']['text'] == 'files_a changed')
    check('files: and the archive\'s its own', b.call('files.read', {'name': 'archived.txt'})['result']['text'] == 'the archive\'s own')
    b.call('mod.load', {'id': 'files_b'})
    check('files: files_b back over files_a', opn('tables\\test.txt').get('text') == 'files_b')


def main():
    # the harness's game folder: the loader, the test mods and the shipped ones
    mods = os.path.join(HARNESS, 'mml', 'mods')
    for mod in ('core', 'hello_c', 'boxes'):
        shutil.rmtree(os.path.join(mods, mod), ignore_errors=True)
        shutil.copytree(os.path.join(OUT, 'mml', 'mods', mod), os.path.join(mods, mod))
    # a mod's own files: files_a (files alone), files_b's files\ folder, the stand-in rw_data's "archive"
    src = os.path.join(ROOT, 'tests', 'mods')
    shutil.rmtree(os.path.join(mods, 'files_a'), ignore_errors=True)
    shutil.copytree(os.path.join(src, 'files_a'), os.path.join(mods, 'files_a'))
    shutil.rmtree(os.path.join(mods, 'files_b', 'files'), ignore_errors=True)
    shutil.copytree(os.path.join(src, 'files_b', 'files'), os.path.join(mods, 'files_b', 'files'))
    archive = os.path.join(HARNESS, 'archive')
    shutil.rmtree(archive, ignore_errors=True)
    os.makedirs(archive)
    os.makedirs(os.path.join(archive, 'Maps'))
    for name, text in (('archived.txt', "the archive's own"), ('only_archive.txt', "the archive's own"),
                       ('Maps/crate_1.565', 'presaved'), ('Maps/CRATE_2L.DX1', 'presaved'), ('Maps/other_1.565', 'presaved')):
        with open(os.path.join(archive, name), 'w') as f:
            f.write(text)
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
        check('mods loaded: core, hello_c, hook_a, hook_b, kit, heavy, boxes, files_a, files_b',
              all(loaded.get(m) for m in ('core', 'hello_c', 'hook_a', 'hook_b', 'kit', 'heavy', 'boxes', 'files_a', 'files_b')), loaded)
        b.call('mod.unload', {'id': 'boxes'})   # (its overlay's 500 items a frame would count in the kit's; boxes_tests loads it)

        add = lambda: b.call('hook_a.call', {'a': 2, 'b': 3})
        r = add()
        check('chain b(a(add)): (2+3+10)*2 = 30', r.get('result') == 30, r)
        r = b.call('hooks.list')
        harness_fn = [h for h in r['result'] if h['target'].startswith('mml_harness.exe')] if r['ok'] else []
        chain = harness_fn[0]['links'] if harness_fn else []
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
        target = next(h['target'] for h in r['result'] if h['target'].startswith('mml_harness.exe'))
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
        r = b.call('render.state')
        check('render.state with no game: not hooked, nothing drawn, it says so',
              r['ok'] and r['result']['hooked'] is False and r['result']['renders'] == 0 and 'note' in r['result'], r)
        kit_tests(b)
        files_tests(b)
        heavy_tests(b)
        boxes_tests(b)
        r = b.call('nonexistent.method')
        check('an unknown method: an error', not r['ok'])
        tail = '\n'.join(b.call('log.tail', {'lines': 400})['result'])
        check('the log tells of the fault, where in the mod and what it touched',
              'faulted' in tail and 'at mod_hook_b_' in tail and 'touching 0x' in tail, tail[-600:])
    finally:
        open(os.path.join(HARNESS, 'quit'), 'w').close()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    check('the harness exits cleanly', proc.returncode == 0, out.decode(errors='replace'))
    check('files: a mod\'s file answers before the first frame (the game\'s first tables)',
          "before the first frame, tables\\test.txt is 'files_b'" in out.decode(errors='replace'), out.decode(errors='replace'))
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
