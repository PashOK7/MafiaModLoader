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
import glob
import json
import os
import re
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


def native_tests(b):
    """native.call: each convention, floats, a string, an out-buffer, a wrong convention seen in the stack, a fault."""
    a = b.call('kit.natives')['result']
    call = lambda **p: b.call('native.call', p)
    r = call(at=a['add'], conv='cdecl', args=[2, 40])
    check('native.call cdecl: 2 + 40, the caller pops', r['ok'] and r['result']['ret'] == 42 and r['result']['stack']['ok'], r)
    r = call(at=a['mul'], conv='stdcall', args=[6, -7])
    check('native.call stdcall: 6 * -7, the callee popped 8', r['ok'] and r['result']['ret'] == -42 and r['result']['stack']['popped_by_callee'] == 8, r)
    r = call(at=a['plus'], conv='thiscall', this=a['obj'], args=[2])
    check('native.call thiscall: this->x (40) + 2', r['ok'] and r['result']['ret'] == 42 and r['result']['stack']['ok'], r)
    r = call(at=a['sum3'], conv='fastcall', args=[1, 2, 3])
    check('native.call fastcall: ECX, EDX and one on the stack', r['ok'] and r['result']['ret'] == 123 and r['result']['stack']['ok'], r)
    r = call(at=a['half'], conv='stdcall', args=[{'float': 5.0}], ret='float')
    check('native.call: a float argument, a float back (ST0)', r['ok'] and r['result']['ret'] == 2.5, r)
    r = call(at=a['twice'], conv='cdecl', args=[{'double': 1.25}], ret='int')
    check('native.call: a double argument; ST0 popped though `ret` said int', r['ok'] and r['result']['st0'] == 2.5 and r['result']['stack']['ok'], r)
    r = call(at=a['fill'], conv='stdcall', args=[{'buf': 16}, 4], ret='void')
    words = r['result']['buffers'][0]['words'] if r['ok'] else []
    check('native.call: an out-buffer read back', words == ['0x00000000', '0x00000003', '0x00000006', '0x00000009'], r)
    r = call(at=a['len'], conv='cdecl', args=[{'str': 'Lost Heaven'}])
    check('native.call: a string argument', r['ok'] and r['result']['ret'] == 11, r)
    r = call(at=a['add'], conv='stdcall', args=[1, 2])
    check('native.call: a cdecl function called as stdcall: the stack says so, and is put back', r['ok'] and r['result']['ret'] == 3
          and r['result']['stack']['ok'] is False and 'note' in r['result']['stack'], r)
    r = call(at=a['fault'], conv='stdcall', args=[0])
    check('native.call: a fault in the function: an error with where, the game goes on', not r['ok'] and 'faulted' in str(r)
          and b.call('ping')['ok'], r)
    r = call(at='0x00000010', args=[])
    check('native.call: an address that is not code: refused', not r['ok'] and 'not executable' in str(r), r)


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

    # the settings panel (Options > Mods): the manifest's "settings" read into controls
    r = b.call('panel.state')
    kit = [m for m in r.get('result', {}).get('mods', []) if m['mod'] == 'kit']
    c = {k['key']: k for k in kit[0]['controls']} if kit else {}
    check('panel.state: the kit\'s page titled from its settings (no Mods row with no game: the menus are the game\'s)',
          r['ok'] and r['result']['item'] == 0 and kit and kit[0]['title'] == 'Kit', r)
    check('panel.state: a slider (its value as % of its range), one with a unit, a check box from a boolean default, a choice',
          c.get('speed', {}).get('kind') == 'slider' and c['speed']['shown'] == '77%' and
          c.get('overlay.x', {}).get('shown') == '10 px' and c.get('overlay.on', {}).get('kind') == 'check' and
          c.get('name', {}).get('kind') == 'choice' and c['name']['shown'] == 'kit', c)
    check('panel.state: a slider with no max is left out (the log says why)', 'broken' not in c, c)
    check('panel.state: a "key" row shows the hotkey setting', c.get('hotkey', {}).get('kind') == 'key' and
          c['hotkey']['shown'] == c['hotkey']['value'] and c['hotkey']['shown'], c)
    r = b.call('panel.open', {'mod': 'kit'})
    check('panel.open with no game: refused, saying why', not r['ok'] and 'page' in str(r), r)

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
    # scene views: refusals, at most eight, taken back; with no game none is drawn and view.state says why
    r = b.call('kit.views')
    res = r.get('result') or {}
    check('scene_view: no function, a size over 8192, one side 0 refused; eight made, the ninth and tenth refused',
          r['ok'] and res['no_fn'] == 0 and res['too_big'] == 0 and res['half'] == 0 and res['made'] == 8 and res['first'] > 0, r)
    r = b.call('view.state')
    views = r['result']['views'] if r['ok'] else []
    check('view.state: the kit\'s one view kept (the frame\'s size), not drawn, and why',
          len(views) == 1 and views[0]['mod'] == 'kit' and views[0]['handle'] == res.get('first') and views[0]['size'] == [0, 0]
          and views[0]['drawn'] == 0 and 'no game' in r['result'].get('note', ''), r)
    r = b.call('view.show', {'handle': 99999, 'rect': [0, 0, 100, 100]})
    check('view.show of no such view: an error', not r['ok'], r)
    b.call('mod.reload', {'id': 'kit'})
    r = b.call('view.state')
    check('a mod\'s views go with its unload', r['ok'] and r['result']['views'] == [], r)
    r = b.call('kit.passviews')
    res = r.get('result') or {}
    check('gfx_pass_views: 1 for the mod\'s pass, 0 for no such pass', r['ok'] and res['pass'] > 0 and res['on'] == 1 and res['none'] == 0, r)
    r = b.call('gfx.state')
    mine = [p for p in r['result']['passes'] if p['mod'] == 'kit'] if r['ok'] else []
    check('gfx.state: the pass marked in_views', len(mine) == 1 and mine[0]['in_views'] is True, r)
    b.call('kit.passviews_clear')
    # the game's own geometry with no game: refused, and the bridge says why
    r = b.call('kit.geom')
    check('geom_hide, geom_capture with no game (and an empty pattern, no function): refused',
          r['ok'] and r['result'] == {'hide': 0, 'capture': 0, 'empty': 0, 'no_fn': 0}, r)
    r = b.call('geom.state')
    check('geom.state with no game: not hooked, no rules', r['ok'] and r['result']['hooked'] is False and r['result']['rules'] == [], r)
    r = b.call('geom.hide', {'pattern': '00test*'})
    check('geom.hide with no game: an error, saying why', not r['ok'] and 'no game' in str(r), r)
    r = b.call('geom.survey')
    check('geom.survey with no game: an error, saying why', not r['ok'] and 'no game' in str(r), r)
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


# The host's methods as agents find them: host/methods.inc is what docs/PROTOCOL.md gives, commands.list lists every
# documented method with its params, result and thread, and each of them is answered (called with no params: an
# answer or an error of its own, never "unknown method"). Methods that change what the later tests see are not
# called (they are answered elsewhere in these tests).
NOT_CALLED = {'mods.rescan', 'mod.reload', 'mod.load', 'mod.unload', 'gfx.reset', 'prof.reset', 'prof.sample', 'watch.clear',
              'input.release', 'draw.clear', 'mem.write', 'geom.show', 'camera.release'}


def methods_tests(b):
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen_methods.py'), '--check'], capture_output=True, text=True)
    check('host/methods.inc is what docs/PROTOCOL.md gives (tools/gen_methods.py --check)', gen.returncode == 0, gen.stdout)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import gen_methods
    documented = {name: thread for name, thread, _, _ in gen_methods.rows()}
    listed = {c['name']: c for c in b.call('commands.list')['result'] if c.get('owner') == 'host'}
    missing = sorted(set(documented) - set(listed))
    bare = sorted(n for n, c in listed.items() if 'params' not in c or not c.get('result') or c.get('thread') not in ('bridge', 'game'))
    check('commands.list: every documented method of the host, each with its params, result and thread',
          not missing and not bare and len(listed) == len(documented), {'missing': missing, 'bare': bare})
    unknown = []
    for name in sorted(documented):
        if name in NOT_CALLED or name == 'hello':
            continue
        r = b.call(name, {})
        if not r['ok'] and 'unknown method' in str(r.get('error', '')).lower():
            unknown.append(name)
    check('every documented method is answered (none is an unknown method)', not unknown, unknown)
    # and the other way: each method the host's code compares a name with is documented (`hello` is in the prose)
    handled = set()
    for f in os.listdir(os.path.join(ROOT, 'host')):
        if f.endswith('.cpp'):
            text = open(os.path.join(ROOT, 'host', f), encoding='utf-8').read()
            handled |= set(re.findall(r'method\s*[=!]=\s*"([a-z0-9_.]+)"', text))
    undocumented = sorted(handled - set(documented) - {'hello'})
    check('every method the host answers is documented in docs/PROTOCOL.md', not undocumented, undocumented)


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


def crash_tests():
    """A crash nobody handles (the harness's `crash` file): the dump and, beside it, the report of what dies with the
    process (the stack, the log, the taps' calls, the events, the mods)."""
    crash_dir = os.path.join(HARNESS, 'mml', 'crash')
    shutil.rmtree(crash_dir, ignore_errors=True)
    info_path = os.path.join(HARNESS, 'mml', 'bridge.json')
    if os.path.exists(info_path):
        os.remove(info_path)
    proc = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '30'], cwd=HARNESS,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        if os.path.exists(info_path):
            break
        time.sleep(0.05)
    time.sleep(0.5)
    b = Bridge(json.load(open(info_path)))
    target = next((h['target'] for h in b.call('hooks.list')['result'] if h['target'].startswith('mml_harness.exe')), None)
    if target:
        b.call('tap', {'cmd': target + ' n=2'})
    b.call('hook_a.call', {'a': 2, 'b': 3})
    open(os.path.join(HARNESS, 'crash'), 'w').close()
    try:
        proc.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
    check('a crash: the harness ended by it', proc.returncode not in (0, None), proc.returncode)
    dumps = sorted(glob.glob(os.path.join(crash_dir, '*.dmp')))
    reports = sorted(glob.glob(os.path.join(crash_dir, '*.json')))
    check('a crash: its dump and the report beside it', len(dumps) == 1 and len(reports) == 1 and
          reports[0][:-5] == dumps[0][:-4], (dumps, reports))
    if not reports:
        return
    r = json.load(open(reports[0]))
    check('the report: the exception, a write to address 0, on the main thread',
          r.get('exception') == 'C0000005' and r.get('access', {}).get('kind') == 'write' and r['access'].get('address') == '0x00000000'
          and r.get('main_thread') is True, {k: r.get(k) for k in ('exception', 'access', 'main_thread')})
    st = r.get('stack', [])
    check('the report: the stack from the crashing function into the harness\'s frame loop',
          len(st) >= 2 and all(s.startswith('mml_harness.exe+0x') for s in st[:2]), st)
    check('the report: the log\'s last lines, the crash in them', any('exception C0000005' in l for l in r.get('log', [])), r.get('log', [])[-5:])
    check('the report: the taps\' last calls', any('1 calls' in t for t in r.get('taps', [])), r.get('taps'))
    check('the report: the last events and the mods', r.get('events') and any(m.get('id') == 'hook_a' for m in r.get('mods', [])),
          (r.get('events', [])[-2:], r.get('mods')))


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
    # the examples ship disabled; the harness loads hello_c and boxes from the start (boxes' world kept across the
    # loads boxes_tests makes)
    for mod in ('hello_c', 'boxes'):
        manifest = os.path.join(mods, mod, 'mod.json')
        with open(manifest) as f:
            text = f.read()
        with open(manifest, 'w') as f:
            f.write(text.replace('"enabled": false', '"enabled": true'))
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
        # what happened, kept for agents: the fault, the unload it caused, the loads before (events.since)
        r = b.call('events.since', {'types': 'MOD_FAULTED', 'filter': {'name': 'HOOK_B'}})
        ev = r['result']['events'] if r['ok'] else []
        check('events.since: the fault kept, found by type and by a field (any case)', len(ev) == 1 and ev[0]['data']['where'] == 'hook_b.crash'
              and ev[0]['seq'] > 0, r)
        stack = ev[0]['data'].get('stack', []) if ev else []
        check('the fault\'s call stack: the mod\'s code first, named', stack and '(mod hook_b)' in stack[0] and len(stack) >= 2, stack)
        r = b.call('events.since', {'types': ['MOD_UNLOADED', 'MOD_LOADED'], 'filter': {'name': 'hook_b'}})
        kinds = [e['type'] for e in r['result']['events']] if r['ok'] else []
        check('events.since: hook_b loaded, then unloaded after its fault', kinds[:1] == ['MOD_LOADED'] and 'MOD_UNLOADED' in kinds, r)
        latest = r['result']['latest'] if r['ok'] else 0
        r = b.call('events.since', {'seq': latest})
        check('events.since after the latest: nothing, every frame\'s events left out', r['ok'] and r['result']['events'] == []
              and r['result']['latest'] == latest, r)
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
        r = b.call('gfx.state')
        check('gfx.state with no game: idle, no device, no frame', r['ok'] and r['result']['frames'] == 0
              and r['result']['generation'] == 0 and 'idle' in r['result']['state'], r)
        # measuring the frame: each entry into a mod by its site; the game thread sampled
        b.call('prof.reset')
        time.sleep(0.3)
        b.call('kit.status')
        time.sleep(0.2)
        r = b.call('prof.state')
        mods = {m['mod']: {s['site'] for s in m['sites']} for m in r['result']['mods']} if r['ok'] else {}
        check('prof.state: the frames counted, each mod by site (an event, a command)',
              r['ok'] and 'FRAME' in mods.get('hello_c', set()) and 'command kit.status' in mods.get('kit', set()), r)
        r = b.call('prof.sample', {'seconds': 0.5})
        sid = r['result']['id'] if r['ok'] else 0
        time.sleep(1.0)
        r = b.call('prof.sample', {'id': sid})
        res = r.get('result') or {}
        check('prof.sample: the game thread sampled (samples, modules, phases)',
              r['ok'] and res.get('done') and res.get('samples', 0) > 100 and res['by_module']   # (the harness's thread sleeps between frames: ntdll)
              and 'game' in res['phases'], {k: res.get(k) for k in ('done', 'samples', 'by_module')})
        kit_tests(b)
        native_tests(b)
        # people with no game: none, refused; the bridge says so
        r = b.call('kit.humans')
        check('humans_list, human_skeleton, bone_set, human_player with no game: none, 0, 0, null',
              r['ok'] and r['result'] == {'count': 0, 'skeleton': 0, 'bone_set': 0, 'player': 0}, r)
        r = b.call('humans.list')
        check('humans.list with no game: none', r['ok'] and r['result']['count'] == 0, r)
        r = b.call('human.skeleton', {'actor': 'player'})
        check('human.skeleton with no game: an error, saying why', not r['ok'] and 'no such person' in str(r), r)
        # the camera with no game: an override taken and given back, nothing to project; the bridge's camera held
        r = b.call('kit.camera')
        check('camera_override taken and given back; camera_project, camera_ray with no frame: 0',
              r['ok'] and r['result'] == {'override': 1, 'project': 0, 'ray': 0, 'back': 1}, r)
        r = b.call('camera.set', {'eye': [0, 5, 0], 'at': [0, 0, 10], 'ms': 5000})
        check('camera.set: held', r['ok'] and r['result']['held'], r)
        r = b.call('camera.state')
        check('camera.state: the bridge holds it, no frame drawn from it (no game), no frame camera',
              r['ok'] and r['result']['override'].startswith('the bridge') and r['result']['frames_overridden'] == 0
              and 'frame_camera' not in r['result'], r)
        r = b.call('camera.release')
        check('camera.release: nothing held', r['ok'] and not r['result']['held'] and b.call('camera.state')['result']['override'] == 'none', r)
        r = b.call('camera.set', {'eye': [1, 1, 1], 'at': [1, 1, 1]})
        check('camera.set with the point at the eye: refused', not r['ok'], r)
        r = b.call('camera.set', {'at': [1, 1, 1]})
        check('camera.set with no eye: refused, saying what it takes', not r['ok'] and 'eye' in str(r), r)
        # questions to the world with no game: nothing hit, near or there; the bridge says why
        r = b.call('kit.ask')
        check('world_ray, world_sweep, world_near, world_shapes with no game: 0',
              r['ok'] and r['result'] == {'ray': 0, 'sweep': 0, 'near': 0, 'shapes': 0}, r)
        r = b.call('world.ray', {'from': [0, 10, 0], 'to': [0, -10, 0]})
        check('world.ray with no game: an error, saying why', not r['ok'] and ('not known' in str(r) or 'no mission' in str(r)), r)
        r = b.call('world.ray', {'from': 'camera'})
        check('world.ray from the camera with no frame: an error, saying why', not r['ok'] and 'no frame' in str(r), r)
        r = b.call('world.ray', {'from': [0, 0, 0]})
        check('world.ray with no end: refused, saying what it takes', not r['ok'] and 'dir' in str(r), r)
        r = b.call('world.shapes', {'lo': [-5, -5, -5], 'hi': [5, 5, 5]})
        check('world.shapes with no game: none', r['ok'] and r['result']['count'] == 0, r)
        # the game's state with no game: no money, no person; the bridge says why
        r = b.call('kit.state')
        check('money, health, items, ammo, take with no game: none, refused',
              r['ok'] and r['result'] == {'money': -1, 'money_set': 0, 'health': -1, 'health_set': 0, 'items': 0, 'ammo': 0, 'take': 0}, r)
        r = b.call('state.money')
        check('state.money with no game: null', r['ok'] and r['result']['money'] is None, r)
        r = b.call('state.money', {'set': 100})
        check('state.money set with no game: refused', not r['ok'], r)
        r = b.call('human.items')
        check('human.items with no game: an error, saying why', not r['ok'] and 'no actors' in str(r), r)
        # the game's effects with no game: nothing read, the hook refused; the bridge says why
        r = b.call('kit.effects')
        check('car_exhaust, exhaust_hook, particle_def, particle_def_set, particle_hook with no game: 0',
              r['ok'] and r['result'] == {'exhaust': 0, 'hook': 0, 'defs': 0, 'def': 0, 'set': 0, 'particles': 0}, r)
        r = b.call('particles.defs')
        check('particles.defs with no game: an error, saying why', not r['ok'] and 'not known' in str(r), r)
        r = b.call('kit.props')
        check('prop_create, prop_place, prop_attach, prop_show, prop_remove, actor_frame, frame_world with no game: none',
              r['ok'] and r['result'] == {'create': 0, 'place': 0, 'attach': 0, 'show': 0, 'remove': 0, 'actor_frame': 0, 'frame_world': 0}, r)
        r = b.call('kit.interact')
        check('zone_add and use_point_add with no game: refused; a timer kept, removed once',
              r['ok'] and r['result'] == {'zone': 0, 'use': 0, 'timer': True, 'removed': 1, 'again': 0}, r)
        r = b.call('interact.list')
        check('interact.list with no game: nothing', r['ok'] and r['result']['items'] == [], r)
        r = b.call('zone.add', {'at': [0, 0, 0]})
        check('zone.add with no game: an error, saying why', not r['ok'] and 'no game' in str(r), r)
        r = b.call('kit.hud')
        check('picture_load, picture_size, hud_marker_add, _set, _remove, hud_console, hud_help with no game: none (drawing nothing: nothing)',
              r['ok'] and r['result'] == {'picture': 0, 'size': 0, 'marker': 0, 'set': 0, 'removed': 0, 'console': 0, 'help': 0}, r)
        r = b.call('kit.saves')
        check('save_put, save_get with no game: the slot\'s state kept, read, removed; the profile\'s refused',
              r['ok'] and r['result'] == {'put': 1, 'got': 5, 'value': 'trout', 'profile': 0, 'removed': 1, 'after': 0, 'bad_scope': 0}, r)
        r = b.call('save.put', {'key': 'note', 'value': 'kept'})
        r2 = b.call('save.get', {'key': 'note'})
        r3 = b.call('save.state')
        check('save.put, save.get, save.state with no game: the bridge\'s key kept, not hooked',
              r['ok'] and r2['ok'] and r2['result']['value'] == 'kept' and r3['ok'] and r3['result']['hooked'] is False
              and {'mod': 'bridge', 'key': 'note', 'size': 4} in r3['result']['slot_keys'], (r, r2, r3))
        r = b.call('restore.list')
        check('restore.list: none yet, the folder told', r['ok'] and r['result']['points'] == [] and r['result']['dir'].endswith('restore\\'), r)
        r = b.call('restore.save', {'name': 'start'})
        check('restore.save with no game: refused, saying so', not r['ok'] and 'not a known build' in r.get('error', ''), r)
        r = b.call('restore.save', {'name': '../x'})
        check('restore.save: a name that is not one refused', not r['ok'] and 'name:' in r.get('error', ''), r)
        r = b.call('restore.load', {'name': 'start'})
        check('restore.load with no game: refused', not r['ok'], r)
        r = b.call('kit.anims')
        check('human_anim_play, _stop, _replace, _state, human_get_up with no game: refused, nothing faults',
              r['ok'] and r['result'] == {'play': -1, 'stop': 0, 'replace': 0, 'state': 0, 'untouched': 1, 'get_up': 0}, r)
        r = b.call('kit.items')
        check('item_define, item_find with no game: refused, none found',
              r['ok'] and r['result'] == {'define': 0, 'bad_name': 0, 'null': 0, 'find': 0, 'find_other': 0}, r)
        r = b.call('items.define', {'name': 'rod'})
        r2 = b.call('items.state')
        check('items.define with no game: an error, saying why; items.state: not installed, nothing defined',
              not r['ok'] and 'not known' in str(r) and r2['ok'] and r2['result']['installed'] is False and r2['result']['items'] == [], (r, r2))
        r = b.call('kit.stats')
        check('stats_get, profile_progress, profile_saves with no game: the game\'s counts at 0, no profile, no saves',
              r['ok'] and r['result'] == {'game': 1, 'kills': 0, 'profile': 0, 'bad_scope': 0, 'progress': 0, 'profile_number': -1, 'saves': 0}, r)
        r = b.call('stats.state')
        r2 = b.call('profile.progress')
        check('stats.state with no game: the game\'s counts, no profile; profile.progress: an error, saying why',
              r['ok'] and r['result']['game']['kills'] == 0 and r['result']['profile_number'] == -1 and not r2['ok'] and 'no profile' in str(r2), (r, r2))
        r = b.call('kit.water')
        check('world_water, zone_watch with no game: none, refused', r['ok'] and r['result'] == {'water': 0, 'untouched': 1, 'watch': 0}, r)
        r = b.call('world.water', {'at': [0, 0, 0]})
        check('world.water with no game: an error, saying why', not r['ok'] and 'no mission' in str(r), r)
        r = b.call('kit.prop_more')
        check('prop_anim_*, prop_physics, prop_push, prop_body with no game: not a prop, refused',
              r['ok'] and r['result'] == {'play': -1, 'stop': 0, 'state': 0, 'untouched': 1, 'physics': 0, 'push': 0, 'body': 0}, r)
        r = b.call('kit.models')
        m = r.get('result', {})
        check('game_files with no game: the mods\' files only, each told; model_open, _nodes, _mesh, _materials, _file: none',
              r['ok'] and m['files'] == m['told'] and m['files'] > 0 and m['open'] == 0 and m['nodes'] == 0 and m['mesh'] == 0 and
              m['materials'] == 0 and m['file'] == 0 and m['untouched'] == 1, r)
        check('scene_focus with no game: taken (and let go); scene_pieces none; actor_meshes of no actor: -1',
              r['ok'] and m['focus'] == 1 and m['pieces'] == 0 and m['actor'] == -1 and m['off'] == 1, r)
        check('shader_shadows from a mod with no game: taken and let go', r['ok'] and m['shadows'] == 2, r)
        ref = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'shaders', 'reference.hlsl')).read()
        r = b.call('shader.add', {'source': ref.replace('mml_ps(', 'mml_ps_broken(', 1), 'vs': 'mml_vs', 'ps': 'mml_ps'})
        check('shader.add of HLSL that does not compile: refused with the compiler\'s message',
              not r['ok'] and 'pixel shader' in str(r) and 'mml_ps' in str(r), r)
        r = b.call('shader.add', {'source': ref, 'vs': 'mml_vs', 'ps': 'mml_ps'})
        check('shader.add of the reference shaders with no game: compiled (game.hlsli included), refused for want of the game',
              not r['ok'] and 'no game' in str(r), r)
        lit = ('#include "mml/game.hlsli"\n'
               'float4 ps(float3 w : TEXCOORD0, float3 n : TEXCOORD1) : COLOR0 {\n'
               '    float3 c = mml_SunAmbient.rgb + mml_SunColor.rgb * mml_Shadow(w);\n'
               '    for (int i = 0; i < 8; i++) if (i < mml_GameLightInfo.x) c += mml_GameLight(i, w, n);\n'
               '    return float4(c, 1);\n'
               '}\n')
        r = b.call('shader.add', {'source': lit, 'ps': 'ps'})
        check('shader.add of a pixel shader sampling the sun\'s shadow and the game\'s lights: compiled, refused for want of the game',
              not r['ok'] and 'no game' in str(r), r)
        r = b.call('shader.shadows', {'size': 1024, 'extent': 50})
        s = r.get('result', {})
        check('shader.shadows with no game: wanted, not drawn, saying why', r['ok'] and s['wanted'] and not s['drawn'] and s['why'], r)
        r = b.call('shader.state')
        check('shader.state shows the shadows\' state', r['ok'] and r['result']['shadows']['wanted'], r)
        r = b.call('shader.shadows', {'on': False})
        check('shader.shadows off: no longer wanted', r['ok'] and not r['result']['wanted'], r)
        r = b.call('shader.survey')
        check('shader.survey with no game: an error, saying why', not r['ok'] and 'no game' in str(r), r)
        r = b.call('kit.gamecam')
        check('game_camera, game_camera_mode, game_camera_fov with no game: none, refused',
              r['ok'] and r['result'] == {'state': 0, 'mode': 0, 'fov': 0}, r)
        r = b.call('camera.game')
        check('camera.game with no game: an error, saying why', not r['ok'] and 'no mission' in str(r), r)
        # the x87: the stand-in game keeps it at 24 bits, as the game's Direct3D does
        r = b.call('kit.fpu')
        check('a mod\'s callback runs with the x87 at 53 bits: 1/3 on the x87 is the double nearest a third',
              r['ok'] and r['result']['bits'] == 53 and r['result']['third'] == '0.33333333333333331', r)
        r = b.call('fpu.state')
        f = r.get('result', {})
        check('fpu.state: the game\'s word at 24 bits as its frame came in and went back, 53 for the mods, scopes raised',
              r['ok'] and f['game']['bits'] == 24 and f['frame_exit']['bits'] == 24 and f['mods']['bits'] == 53 and f['raised'] > 0, r)
        r = b.call('scene.pieces')
        check('scene.pieces with no game: none, saying there is no focus', r['ok'] and r['result']['pieces'] == 0 and 'scene.focus' in str(r), r)
        r = b.call('files.game', {'match': '*', 'limit': 3})
        check('files.game with no game: the mods\' files, from their mods', r['ok'] and r['result']['count'] > 0 and
              all(f['from'] not in ('folder',) and not f['from'].endswith('.dta') for f in r['result']['files']), r)
        r = b.call('model.info', {'file': 'nothing_here'})
        check('model.info of no file: an error, saying why', not r['ok'] and 'no file' in str(r), r)
        for method, params in (('prop.anim', {'prop': '0x1000', 'play': 'walk1'}), ('prop.physics', {'prop': '0x1000'}),
                               ('prop.push', {'prop': '0x1000', 'impulse': [0, 1, 0]})):
            r = b.call(method, params)
            check(f'{method} with no game: an error, saying why', not r['ok'] and 'not a prop' in str(r), r)
        for method in ('human.anim', 'human.anim_state', 'human.get_up'):
            r = b.call(method, {'actor': '0x1000', 'play': 'walk'})
            check(f'{method} with no game: an error, saying why', not r['ok'] and ('not a person' in str(r) or 'no actors' in str(r)), r)
        r = b.call('save.put', {'key': 'note', 'value': 'x', 'scope': 'profile'})
        check('save.put to the profile with no game: an error, saying why', not r['ok'] and 'no profile' in str(r), r)
        r = b.call('hud.list')
        check('hud.list with no game: nothing', r['ok'] and r['result']['markers'] == [] and r['result']['pictures'] == [], r)
        for method, params in (('hud.marker', {'at': [0, 0, 0]}), ('hud.console', {'text': 'hello'}), ('draw.picture', {'file': '2sight.bmp'})):
            r = b.call(method, params)
            check(f'{method} with no game: an error, saying why', not r['ok'] and 'no game' in str(r), r)
        r = b.call('kit.control')
        check('key_capture, action_capture, player_lock, vehicle_hook, vehicle_body, vehicle_kinds with no game: none (letting a hook go: done)',
              r['ok'] and r['result'] == {'key': 0, 'action': 0, 'lock': 0, 'hook': 0, 'unhook': 1, 'body': 0, 'kinds': 0}, r)
        r = b.call('control.state')
        check('control.state with no game: nothing locked, no hook, no airplanes, nothing moving',
              r['ok'] and r['result']['locked'] is False and r['result']['vehicle_hook'] == 'none' and r['result']['airplanes'] is False
              and r['result']['moving'] == 0 and r['result']['flown'] == 0, r)
        r = b.call('vehicle.move', {'car': '0x1000', 'velocity': [0, 1, 0]})
        check('vehicle.move with no game: an error, saying why', not r['ok'] and 'no actors' in str(r), r)
        r = b.call('player.lock', {'on': True})
        check('player.lock with no game: an error, saying why', not r['ok'] and 'not known' in str(r), r)
        r = b.call('input.capture', {'action': 'Forwards'})
        check('input.capture of an action with no game: an error, saying why', not r['ok'] and 'actions' in str(r), r)
        r = b.call('kit.sound')
        check('sound_play (2D and 3D), sound_set, sound_move, sound_stop, sound_state, music_* with no game: none, music_game -1',
              r['ok'] and r['result'] == {'play2d': 0, 'play3d': 0, 'set': 0, 'move': 0, 'stop': 0, 'state': 0, 'music': 0, 'music_set': 0,
                                          'music_stop': 0, 'music_game': -1}, r)
        r = b.call('sounds.list')
        check('sounds.list with no game: not known, none', r['ok'] and r['result']['known'] is False and r['result']['sounds'] == [] and r['result']['music'] == [], r)
        r = b.call('sound.play', {'file': 'menuopen.wav'})
        check('sound.play with no game: an error, saying why', not r['ok'] and 'not known' in str(r), r)
        r = b.call('music.game', {'on': False})
        check('music.game with no game: an error, saying why', not r['ok'] and 'no mission' in str(r), r)
        r = b.call('props.list')
        check('props.list with no game: not known, none', r['ok'] and r['result'] == {'known': False, 'props': []}, r)
        r = b.call('prop.create', {'model': '2tommy.i3d', 'at': [0, 0, 0]})
        check('prop.create with no game: an error, saying why', not r['ok'] and 'not known' in str(r), r)
        r = b.call('weather.state')
        check('weather.state with no game: an error, saying why', not r['ok'] and 'no mission' in str(r), r)
        r = b.call('weather.set', {})
        check('weather.set with nothing to set: refused, naming the parameters', not r['ok'] and 'max_count' in str(r), r)
        # the scene's lights with no game: none; light.set refused
        r = b.call('kit.lights')
        check('scene_lights, light_set, scene_sun with no game: none, refused', r['ok'] and r['result'] == {'lights': 0, 'set': 0, 'sun': 0}, r)
        r = b.call('lights.list')
        check('lights.list with no game: none, no sun', r['ok'] and r['result']['count'] == 0 and 'sun' not in r['result'], r)
        r = b.call('light.set', {'light': '0x12345678', 'power': 2})
        check('light.set of no light: refused, saying why', not r['ok'] and 'lights.list' in str(r), r)
        # a hardware watchpoint: the field the kit's command writes on the game thread, found with the code that wrote it
        addr = b.call('kit.poke', {'addr': 1})['result']
        r = b.call('watch.set', {'addr': addr, 'mode': 'write'})
        wid = r['result']['id'] if r['ok'] else 0
        check('watch.set: a write watch on the field', r['ok'] and wid > 0, r)
        b.call('kit.poke')
        b.call('kit.poke')
        r = b.call('watch.show', {'id': wid})
        sites = r['result']['sites'] if r['ok'] else []
        check('watch.show: the kit\'s code found, twice, the value before and after',
              len(sites) == 1 and 'mod_kit' in sites[0]['at'] and sites[0]['count'] == 2 and sites[0]['before'] != sites[0]['after'], r)
        r = b.call('watch.set', {'addr': addr, 'mode': 'access', 'len': 4})
        aid = r['result']['id'] if r['ok'] else 0
        b.call('kit.poke')
        r = b.call('watch.show', {'id': aid})
        check('watch.set access: the read and the write of the field found (the handler reads nothing)',
              r['ok'] and r['result']['hits'] >= 2 and r['result']['sites'], r)
        r = b.call('watch.clear', {'all': True})
        check('watch.clear: every watch off, the game goes on', r['ok'] and r['result']['cleared'] == 2 and b.call('ping')['ok'], r)
        r = b.call('watch.set', {'addr': '0x00000010'})
        check('watch.set on memory that is not readable: refused', not r['ok'], r)
        files_tests(b)
        heavy_tests(b)
        boxes_tests(b)
        methods_tests(b)
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
    text = out.decode(errors='replace')
    check('the x87 given back to the game at 24 bits after every frame (the mods\' callbacks ran at 53)',
          re.search(r'x87 at 24 bits after [1-9][0-9]* frames, 0 given back otherwise', text) is not None, text[-400:])
    check('files: a mod\'s file answers before the first frame (the game\'s first tables)',
          "before the first frame, tables\\test.txt is 'files_b'" in out.decode(errors='replace'), out.decode(errors='replace'))
    crash_tests()
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
