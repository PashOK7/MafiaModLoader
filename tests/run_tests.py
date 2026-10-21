"""run_tests.py: the loader's offline tests, through the real bridge (Python standard library only).

Starts build/x86/harness/mml_harness.exe (a stand-in game that loads MafiaModLoader.asi and pumps its frames), with
the test mods (hook_a, hook_b, kit, heavy, files_a, files_b) and the shipped ones (core, hello_c, boxes) in its mml/mods, and checks over the bridge:
hello and the token, ping, the mods' list, hook chains in every load and unload order, services, memory read and
write (a write-protected page), a mod faulting in a command (unloaded, the game goes on), state kept across a hot
reload, the core mod with no game, the log; the mod author's kit (kit_tests: the drawing queue, keys and
hotkeys through injected keys, a mod's settings changed by the host, by the bridge and on disk, its data folder;
config_watch_tests: how soon a change on disk is seen, a burst of writes, a held file, a deleted one);
tools/new_mod.py's own mods (new_mod_build, new_mod_tests: a C++ and a Rust mod made, built by the project's build,
loaded, asked, unloaded, then taken away: sources, CMake lines, outputs);
a mod's own files in the game (files_tests: a stand-in rw_data.dll's names answered from the mods' `files` folders,
the order, changes on disk, file_add, a file held open replaced, loads and unloads);
the game's videos played by the loader (video_tests: FFmpeg loaded from mml/ffmpeg, the search order, a Bink 2 file
refused, an H.264 clip opened);
every mod's settings pages in Options > Mods laid out by the loader's own code (ui_tests: nothing over another,
off its window or cut, no text wider than its button);
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
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, 'build', 'x86', 'harness')
OUT = os.path.join(ROOT, 'build', 'x86', 'out')
for _stream in (sys.stdout, sys.stderr):   # (a folder's name in a line: a console or a pipe in any code page)
    _stream.reconfigure(errors='backslashreplace')


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
    config_watch_tests(b, status, changes_path)
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
    parts = kit[0].get('parts', []) if kit else []
    labels = [p['label'] for p in parts]
    check('panel.state: the controls and a section of 20 rows, cut to fit the screen: three tabs (two parts of about '
          'the same height, 11 and 9 rows, the two-line label in the second)',
          labels == ['Kit', 'Many 1/2', 'Many 2/2'] and len(parts[1]['keys']) == 11 and len(parts[2]['keys']) == 9 and
          parts[2]['keys'][-1] == 'many.k19' and [s['title'] for s in kit[0]['sections']] == ['', 'Many'], parts)
    check('panel.state: the sections\' controls listed with the others (a key under the section\'s name)',
          c.get('many.k3', {}).get('kind') == 'check' and c['many.k3']['value'] is True, c.get('many.k3'))
    r = b.call('panel.open', {'mod': 'kit'})
    check('panel.open with no game: refused, saying why', not r['ok'] and 'page' in str(r), r)
    r = b.call('panel.open', {'mod': 'kit', 'part': 'Many 3/2'})
    check('panel.open of a part the page has not: refused, saying where they are listed', not r['ok'] and 'panel.state' in str(r), r)

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


def config_watch_tests(b, status, changes_path):
    """A mod's settings edited on disk (as a person or an agent would): both folders are watched, a file is read again
    a frame or two after its writes stop (it was polled twice a second: up to 500 ms)."""
    def seen(before, t0, seconds=2.0):
        # the mod's status once it has been told (a call a frame), the time since t0 and the frames gone by
        while time.perf_counter() - t0 < seconds:
            s = status()
            if s['config_events'] > before['config_events']:
                return s, round((time.perf_counter() - t0) * 1000), s['frames'] - before['frames']
        return status(), None, None

    def edit(text, path=changes_path):
        before = status()
        t0 = time.perf_counter()
        with open(path, 'w') as f:
            f.write(text)
        s, ms, frames = seen(before, t0)
        return s, ms, frames, s['config_events'] - before['config_events']

    # (the frames counted from the call before the write to the call that sees the event: the mod is told at the
    # second frame after the write, the calls add one at each end; polled, it was up to 500 ms: 16 of these frames)
    lat = [edit('{ "speed": %d }' % (40 + i)) + (40 + i,) for i in range(5)]
    check('a setting changed on disk reaches the mod a frame or two later (seen in %s ms, %s frames)'
          % (', '.join(str(x[1]) for x in lat), ', '.join(str(x[2]) for x in lat)),
          all(ms is not None and frames <= 6 and n == 1 and s['speed'] == want for s, ms, frames, n, want in lat), lat)
    s3, ms, _, n = edit('{ "speed": 31, "hotkey": "F8" }  // by hand\n')
    check('the file edited on disk is read again: new values, the mod told once, a removed change back to its default',
          s3['speed'] == 31 and s3['hotkey'] == 'F8' and s3['overlay_x'] == 10 and n == 1, s3)
    b.call('input.key', {'key': 'F7', 'frames': 2})
    b.call('input.key', {'key': 'F8', 'frames': 2, 'after_frames': 4})
    time.sleep(0.4)
    check('the hotkey bound again from the new setting: F8 fires, F7 no more', status()['hot_setting'] == 2, status())

    # a burst of writes (an editor's save, a tool writing in pieces): one change, its last values
    before = status()
    for i in range(10):
        with open(changes_path, 'w') as f:
            f.write('{ "speed": %d, "hotkey": "F8" }' % (50 + i))
        time.sleep(0.002)
    time.sleep(0.3)
    s4 = status()
    check('a burst of 10 writes in 20 ms: one event, the last values',
          s4['config_events'] == before['config_events'] + 1 and s4['speed'] == 59, (before['config_events'], s4))

    # the defaults beside the mod's DLL, edited by hand: the mod's folder is watched too
    defaults_path = os.path.join(HARNESS, 'mml', 'mods', 'kit', 'config.json')
    original = open(defaults_path, encoding='utf-8').read()
    s5, ms, _, n = edit(original.replace('"name": "kit"', '"name": "kit edited"'), defaults_path)
    check('the defaults\' file edited: read again, told (%s ms)' % ms, s5['name'] == 'kit edited' and n == 1 and s5['speed'] == 59, s5)
    s5, ms, _, n = edit(original, defaults_path)
    check('... and put back', s5['name'] == 'kit' and n == 1, s5)

    # a file that does not parse (a write cut short): the values in use stay, nothing told
    before = status()
    with open(changes_path, 'w') as f:
        f.write('{ "speed": ')
    time.sleep(0.3)
    s6 = status()
    check('a file that does not parse: the values in use stay, the mod not told',
          s6['speed'] == 59 and s6['config_events'] == before['config_events'], s6)

    # a writer that holds the file (no sharing): not read meanwhile, read once it lets go
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    before = status()
    h = k32.CreateFileW(changes_path, 0x40000000, 0, None, 2, 0x80, None)   # GENERIC_WRITE, no sharing, CREATE_ALWAYS
    check('(a file held with no sharing)', h not in (None, wintypes.HANDLE(-1).value), ctypes.get_last_error())
    data = b'{ "speed": 32, "hotkey": "F8" }'
    k32.WriteFile(h, data, len(data), ctypes.byref(wintypes.DWORD()), None)
    time.sleep(0.25)
    s7 = status()
    k32.CloseHandle(h)
    s8, ms, _ = seen(before, time.perf_counter())
    tail = '\n'.join(b.call('log.tail', {'lines': 40})['result'])
    check('a file its writer holds: not read meanwhile, the values stay, no warning; read once it lets go (%s ms)' % ms,
          s7['speed'] == 59 and s7['config_events'] == before['config_events'] and s8['speed'] == 32
          and s8['config_events'] == before['config_events'] + 1 and 'cannot be opened' not in tail, (s7, s8))

    # the changes' file deleted: the defaults
    before = status()
    t0 = time.perf_counter()
    os.remove(changes_path)
    s9, ms, _ = seen(before, t0)
    check('the changes\' file deleted: the defaults, the mod told (%s ms)' % ms,
          s9['speed'] == 12.5 and s9['hotkey'] == 'F7' and s9['config_events'] == before['config_events'] + 1, s9)


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


def frames_tests(b):
    # each frame's record (the harness's stand-in Present blocks 1 ms a frame): a label after its settling frames
    r = b.call('prof.mark', {'label': 'A', 'settle': 5})
    check('prof.mark: the label from a frame after its settling ones', r['ok'] and r['result']['from_seq'] == r['result']['seq'] + 5, r)
    seq = r['result']['seq'] if r['ok'] else 0
    time.sleep(1.0)
    r = b.call('prof.frames', {'since': seq - 1})
    res = r.get('result') or {}
    f = res.get('frames') or {}
    labels = res.get('labels') or []
    a = labels.index('A') if 'A' in labels else -1
    seqs, lab, flags = f.get('seq', []), f.get('label', []), f.get('flags', [])
    settling = [s for s, fl in zip(seqs, flags) if fl & 1]
    check('prof.frames: the 5 settling frames flagged, the rest labelled A, in order',
          r['ok'] and seqs[:1] == [seq] and settling == list(range(seq, seq + 5))
          and all(l == a for s, l in zip(seqs, lab) if s >= seq + 5) and len(seqs) > 20, {k: f.get(k) for k in ('seq', 'label', 'flags')})
    present = [x for x in f.get('present_ms', []) if x is not None]
    wall = [x for x in f.get('wall_ms', []) if x is not None]
    cpu = [x for x in f.get('cpu_ms', []) if x is not None]
    check('prof.frames: each frame its time, the time Present blocked (1 ms), the game thread\'s CPU time below the frame\'s',
          present and min(present) >= 0.9 and wall and min(wall) > min(present) and cpu and all(c < w for c, w in zip(f['cpu_ms'], f['wall_ms']) if c is not None)
          and res.get('tsc_per_ms', 0) > 100000 and 'd3d' not in f, {k: f.get(k) for k in ('wall_ms', 'present_ms', 'cpu_ms')})
    r = b.call('prof.frames', {'since': res.get('next', 0)})
    check('prof.frames since the last one read: only the newer ones', r['ok'] and all(s > res.get('next', 0) for s in r['result']['frames']['seq']), r)
    b.call('prof.mark', {'label': ''})
    r = b.call('prof.counters', {'on': True})
    check('prof.counters with no game: refused, saying why', not r['ok'] and 'Direct3D 9' in str(r), r)
    r = b.call('prof.state')
    check('prof.state: the frame\'s record\'s state (no label, not counting, the GPU off)',
          r['ok'] and r['result']['label'] == '' and r['result']['counters'] is False and r['result']['gpu'] != 'measured', r)


def methods_tests(b):
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen_methods.py'), '--check'], capture_output=True, text=True)
    check('host/methods.inc is what docs/PROTOCOL.md gives (tools/gen_methods.py --check)', gen.returncode == 0, gen.stdout)
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen_script_commands.py'), '--check'], capture_output=True, text=True)
    check('sdk script_commands.h is what script_commands.csv gives (tools/gen_script_commands.py --check)', gen.returncode == 0, gen.stdout)
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'check_paths.py')], capture_output=True, text=True)
    check('no ANSI path and no strict json dump in the sources (tools/check_paths.py)', gen.returncode == 0, gen.stdout)
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'check_demo_names.py')], capture_output=True, text=True)
    check("none of the Mafia demo's own names in the repository (tools/check_demo_names.py: its licence)", gen.returncode == 0,
          gen.stdout + gen.stderr)
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


def json_with_comments(text):
    """A mod's config.json: JSON with // comments (outside strings) taken out."""
    out, in_str, esc, i = [], False, False, 0
    while i < len(text):
        c = text[i]
        if in_str:
            out.append(c)
            esc = c == '\\' and not esc
            if c == '"' and not esc:
                in_str = False
            i += 1
        elif c == '"':
            in_str, esc = True, False
            out.append(c)
            i += 1
        elif text.startswith('//', i):
            i = text.find('\n', i) if '\n' in text[i:] else len(text)
        else:
            out.append(c)
            i += 1
    return json.loads(''.join(out))


def ui_layout_problems(page, header=44, text_h=15):
    """A laid-out page's (panel.layout) controls and texts over each other, out of their window or the screen, or a
    button's text wider than the button. Each item's `ink` is the loader's own estimate of its text's width (panel.cpp
    TextWidth, measured on the game's pages): a static text is taken as its text (`ink` wide, a line of `text_h`: the
    pages put a label's second line 15 under its first), the other controls as their boxes."""
    items = page['items']
    win = items[0]
    problems = []
    if win['x'] < 0 or win['y'] < 0 or win['x'] + win['w'] > 800 or win['y'] + win['h'] > 600:
        problems.append('the window is off the screen: %s' % win)
    if win['ink'] > win['w'] - 20:
        problems.append('the title %r (%d) is wider than its window' % (win['text'], win['ink']))
    boxes = []
    for it in items[1:]:
        if it['kind'] == 'stxt':
            x0, y0, x1, y1 = it['x'], it['y'], it['x'] + it['ink'], it['y'] + text_h
        else:
            x0, y0, x1, y1 = it['x'], it['y'], it['x'] + it['w'], it['y'] + it['h']
            if it['ink'] > it['w'] - 10:   # (a button's text keeps off its edges)
                problems.append('%s %d: %r (%d) is wider than its %d' % (it['kind'], it['id'], it.get('sample') or it['text'], it['ink'], it['w'] - 10))
        if x0 < 0 or y0 < 0 or x1 > win['w'] or y1 > win['h'] - header:
            problems.append('%s %d %r out of its window' % (it['kind'], it['id'], it['text']))
        boxes.append((it, x0, y0, x1, y1))
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i], boxes[j]
            if a[1] < b[3] and b[1] < a[3] and a[2] < b[4] and b[2] < a[4]:
                problems.append('%s %d %r over %s %d %r' % (a[0]['kind'], a[0]['id'], a[0]['text'], b[0]['kind'], b[0]['id'], b[0]['text']))
    for label in page.get('cut', []):
        problems.append('a label cut (more than two lines): %r' % label)
    return problems


def ui_tests(b):
    """Every mod's settings pages in the game's Options > Mods, laid out by the loader's own code (panel.layout, no
    game): no control or text over another, nothing off its window or the screen, no button's text wider than the
    button, no label cut; and its row in the Mods list."""
    manifests = sorted(glob.glob(os.path.join(ROOT, 'mods', '**', 'mod.json'), recursive=True))
    seen, all_problems = [], {}
    for path in manifests:
        manifest = json_with_comments(open(path, encoding='utf-8-sig').read())
        if 'settings' not in manifest:
            continue
        config = os.path.join(os.path.dirname(path), 'config.json')
        defaults = json_with_comments(open(config, encoding='utf-8-sig').read()) if os.path.exists(config) else {}
        r = b.call('panel.layout', {'settings': manifest['settings'], 'defaults': defaults, 'name': manifest['id']})
        if not r['ok']:
            all_problems[manifest['id']] = [str(r.get('error'))]
            continue
        seen.append(manifest['id'])
        for page in r['result']['mods'][0]['pages']:
            p = ui_layout_problems(page)
            if p:
                all_problems['%s: %s' % (manifest['id'], page['part'])] = p
        p = ui_layout_problems(r['result']['list'])
        if p:
            all_problems['%s: its row in the Mods list' % manifest['id']] = p
    check('the mods\' settings pages (%s): nothing over another, off its window or cut, no text wider than its button'
          % ', '.join(seen), len(seen) >= 4 and not all_problems, all_problems)
    r = b.call('panel.layout', {})
    check('panel.layout with no settings: the loaded mods\' pages and the list', r['ok'] and 'list' in r['result'], r)
    # the test sees what it should: a page made to fail is caught
    bad = {'sections': [{'title': 'A', 'controls': [
               {'key': 'a', 'label': 'A label far too long to fit into two lines of twenty characters each'},
               {'key': 'b', 'label': 'B', 'min': 0, 'max': 1, 'unit': 'metres per second squared'},
               {'key': 'c', 'label': 'C', 'options': [{'value': 0, 'label': 'An option far wider than its selector, surely'}]}]},
           {'title': 'A second section with a long name', 'controls': [{'key': 'd', 'label': 'D'}]}]}
    r = b.call('panel.layout', {'settings': bad, 'defaults': {'a': True, 'b': 0.5, 'c': 0, 'd': True}, 'name': 'bad'})
    p = ui_layout_problems(r['result']['mods'][0]['pages'][0]) if r['ok'] else ['no layout: %s' % r]
    check('ui_layout_problems catches a cut label, a value field running into the window\'s edge, an option wider than its selector',
          any('cut' in x for x in p) and any('out of its window' in x for x in p) and any('An option' in x for x in p), p)
    tabs = [i for i in r['result']['mods'][0]['pages'][0]['items'] if 150 <= i['id'] < 200] if r['ok'] else []
    check('a tab is as wide as its text needs', tabs and all(t['ink'] <= t['w'] - 10 for t in tabs), tabs)


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


def export_rva(path, name):
    """An exported function's RVA in a PE file (the tap takes `module+rva`, not a name)."""
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    opt = pe + 24
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    sec0 = opt + struct.unpack_from('<H', d, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', d, sec0 + 40 * i + 8) for i in range(nsec)]   # vsize, va, rawsize, raw

    def off(rva):
        return next(raw + rva - va for vs, va, rs, raw in secs if va <= rva < va + max(vs, rs))
    exp = off(struct.unpack_from('<I', d, opt + 96)[0])   # the export directory (PE32)
    n, funcs, names, ords = struct.unpack_from('<IIII', d, exp + 24)
    for i in range(n):
        at = off(struct.unpack_from('<I', d, off(names) + 4 * i)[0])
        if d[at:d.index(b'\0', at)].decode() == name:
            return struct.unpack_from('<I', d, off(funcs) + 4 * struct.unpack_from('<H', d, off(ords) + 2 * i)[0])[0]
    return None


def run_crash(trigger, tap_crash=False, setup=None, tap=True):
    """The harness started, a tap and a call made (and `setup(bridge)`), then crashed by its `trigger` file; (its
    output, the report or None)."""
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
    if target and tap:
        b.call('tap', {'cmd': target + ' n=2'})
    if tap_crash:
        # with its return: the tap's thunk keeps a shadow of the return in the thread's TLS, when it has one
        rva = export_rva(os.path.join(HARNESS, 'mml_harness.exe'), 'harness_crash')
        r = b.call('tap', {'cmd': 'mml_harness.exe+%X n=2 ret' % rva})
        check('a tap with its return on harness_crash', r.get('ok') and not str(r.get('result')).startswith('error'), r)
    b.call('hook_a.call', {'a': 2, 'b': 3})
    if setup:
        setup(b)
    open(os.path.join(HARNESS, trigger), 'w').close()
    try:
        out = proc.communicate(timeout=20)[0].decode(errors='replace')
    except subprocess.TimeoutExpired:
        proc.kill()
        out = proc.communicate()[0].decode(errors='replace')
    tag = 'a crash (%s): ' % trigger
    check(tag + 'the harness ended by it', proc.returncode not in (0, None), proc.returncode)
    dumps = sorted(glob.glob(os.path.join(crash_dir, '*.dmp')))
    reports = sorted(glob.glob(os.path.join(crash_dir, '*.json')))
    check(tag + 'its dump and the report beside it', len(dumps) == 1 and len(reports) == 1 and
          reports[0][:-5] == dumps[0][:-4], (dumps, reports, out[-600:]))
    return out, json.load(open(reports[0])) if reports else None


def crash_tests():
    """A crash nobody handles (the harness's `crash` file): the dump and, beside it, the report of what dies with the
    process (the stack, the log, the taps' calls, the events, the mods). Then the same on a thread made without TLS
    (`crash_notls`: NtCreateThreadEx's SkipThreadAttach, as injectors make theirs): the filter and a tap's thunk run on
    it without touching thread_local (a player's game died silently so, the filter faulting again)."""
    out, r = run_crash('crash')
    if not r:
        return
    check('the report: the exception, a write to address 0, on the main thread',
          r.get('exception') == 'C0000005' and r.get('access', {}).get('kind') == 'write' and r['access'].get('address') == '0x00000000'
          and r.get('main_thread') is True, {k: r.get(k) for k in ('exception', 'access', 'main_thread')})
    st = r.get('stack', [])
    check('the report: the stack from the crashing function into the harness\'s frame loop',
          len(st) >= 2 and all(s.startswith('mml_harness.exe+0x') for s in st[:2]), st)
    exe = os.path.join(HARNESS, 'mml_harness.exe')
    crash_at, add_at = ['mml_harness.exe+0x%X' % export_rva(exe, n) for n in ('harness_crash', 'harness_add')]
    calls = r.get('calls', [])
    check("the report's calls: each return address with its call's target, the first main's call of harness_crash",
          calls and calls[0][0].startswith('mml_harness.exe+0x') and calls[0][1] == crash_at, calls[:4])

    check('the report: the log\'s last lines, the crash in them', any('exception C0000005' in l for l in r.get('log', [])), r.get('log', [])[-5:])
    check('the report: the taps\' last calls', any('1 calls' in t for t in r.get('taps', [])), r.get('taps'))
    check('the report: the last events and the mods', r.get('events') and any(m.get('id') == 'hook_a' for m in r.get('mods', [])),
          (r.get('events', [])[-2:], r.get('mods')))
    a = r.get('address_space', {})
    check('the report: the address space (free, its largest piece, Large Address Aware or not)',
          a.get('total_mb', 0) >= 2000 and 0 < a.get('largest_mb', 0) <= a.get('free_mb', 0) < a.get('total_mb', 0)
          and isinstance(a.get('large_address_aware'), bool), a)

    # under the hooks: main -> harness_add's patch -> the chain's thunk -> hook_b's detour -> hook_a's -> MinHook's
    # trampoline -> harness_add -> harness_crash; each call's target read through the jumps and the trampoline
    out, r = run_crash('crash_hooked', tap=False)   # (a tap's thunk would stand first in the chain)
    calls = r.get('calls', []) if r else []
    to_add = [c for c in calls if c[1] == add_at]
    check("a crash under the hooks: hook_a's call of the original (MinHook's trampoline) leads to harness_add",
          any(c[0].lower().startswith('mod_hook_a_') for c in to_add), calls[:8])
    check("... and main's call of harness_add (its patch, the chain's jmp [head]) to hook_b's detour",
          any(c[0].startswith('mml_harness.exe+0x') and str(c[1]).lower().startswith('mod_hook_b_') for c in calls), calls[:8])
    check("... and hook_b's call of its original (`mov eax, [g_original]`, `call eax`) to hook_a's detour",
          any(c[0].lower().startswith('mod_hook_b_') and str(c[1]).lower().startswith('mod_hook_a_') for c in calls), calls[:8])

    def another_filter(b):
        r = b.call('crash.filter')
        check('crash.filter: SetUnhandledExceptionFilter hooked, ours not set again each frame',
              r['ok'] and r['result']['hooked'] is True and r['result']['every_frame'] is False, r)
        open(os.path.join(HARNESS, 'setfilter'), 'w').close()
        time.sleep(0.3)
        r = b.call('crash.filter')
        check("crash.filter: another's filter set later goes after ours (chained to the harness's)",
              r['ok'] and r['result']['others_set'] == 1 and r['result']['chained_to'].startswith('mml_harness.exe+0x'), r)
        r = b.call('log.tail', {'lines': 40})
        check('the log: another one was set, ours stays first', any('another one was set' in l for l in r['result']), r['result'][-5:])
        r = b.call('crash.filter', {'every_frame': True})
        check("crash.filter every_frame: ours set again each frame (a comparison's other side)", r['ok'] and r['result']['every_frame'] is True, r)
        b.call('crash.filter', {'every_frame': False})
    out, r = run_crash('crash', setup=another_filter)
    given = out.split('its own filter set (it was given')[1].split(')')[0] if 'its own filter set (it was given' in out else ''
    check("another's filter after ours: the dump written, then its filter called; it was given what came before it, not ours",
          'its own filter called' in out and given and r is not None and 'MafiaModLoader.asi' not in given, out[-800:])

    out, r = run_crash('crash_notls', tap_crash=True)
    check('a thread without TLS: the harness made it (fs:[0x2C] 0)', 'fs:[0x2C] = 00000000' in out, out[-600:])
    if not r:
        return
    check('a thread without TLS: the report, a write to address 0 in harness_crash, not the main thread',
          r.get('exception') == 'C0000005' and r.get('access', {}).get('address') == '0x00000000'
          and r.get('main_thread') is False and r.get('stack', [''])[0].startswith('mml_harness.exe+0x'),
          {k: r.get(k) for k in ('exception', 'access', 'main_thread', 'stack')})
    check('a thread without TLS: both taps saw their call (harness_crash\'s thunk ran there)',
          sum('1 calls' in t for t in r.get('taps', [])) >= 2, r.get('taps'))


def dxwrapper_tests():
    """dxwrapper built with third_party/dxwrapper/patch.py: its hooks (DDrawCompat's, in every thread) called from a
    thread made without TLS (tests/dxwrapper/notls.cpp). The release v1.8.8600.25 dies at the first one, as a player's
    game did."""
    folder = os.path.join(ROOT, 'build', 'x86', 'dxwrapper_test')
    exe = os.path.join(folder, 'dxwrapper_notls.exe')
    if not os.path.exists(exe):
        check('dxwrapper: its test built (MML_DXWRAPPER on: cmake --preset x86)', False, exe)
        return
    for stale in glob.glob(os.path.join(folder, '*.log')):
        os.remove(stale)
    proc = subprocess.run([exe], cwd=folder, capture_output=True, timeout=60)
    out = proc.stdout.decode(errors='replace')
    check('dxwrapper: its hooks called from a thread without TLS, every one came back',
          proc.returncode == 0 and 'fs:[0x2C]) = 00000000' in out and 'the thread came back' in out,
          (proc.returncode, out[-800:]))
    logs = glob.glob(os.path.join(folder, 'dxwrapper-*.log'))
    first = open(logs[0], encoding='utf-8', errors='replace').readline() if logs else ''
    check('dxwrapper: its log says the build is ours (the patch\'s mark)', "MafiaModLoader's build" in first, first)


def instances_tests():
    """Games side by side: Game.exe quits when its one-at-a-time mutex is there already (the stand-in game makes it as
    the game's entry does); the loader gives it a mutex of its folder. Two folders run together, each with its own
    bridge (the next free port); the same folder again quits, and so does a junction to it; a game without the loader
    holding the game's own mutex (a player's) does not stop ours."""
    import ctypes
    root = os.path.join(ROOT, 'build', 'x86', 'harness_instances')
    shutil.rmtree(root, ignore_errors=True)
    games = [os.path.join(root, n) for n in ('a', 'b')]
    for game in games:
        shutil.copytree(HARNESS, game, ignore=shutil.ignore_patterns('live', 'logs', 'crash', 'data', 'shots', 'restore',
                                                                      'boxes', 'heavy', 'hello_c', 'bridge.json', 'quit'))
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateMutexW.restype = ctypes.c_void_p
    k32.CreateMutexW.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p)
    player = k32.CreateMutexW(None, 0, 'Mafia Launcher Super Mutex Shit')   # a game without the loader, running

    def start(game):
        return subprocess.Popen([os.path.join(game, 'mml_harness.exe'), '--seconds', '30'], cwd=game,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def bridge_of(game, proc):
        p = os.path.join(game, 'mml', 'bridge.json')
        for _ in range(200):
            if os.path.exists(p) or proc.poll() is not None:
                break
            time.sleep(0.05)
        time.sleep(0.3)
        return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else {}

    procs = []
    try:
        procs = [start(g) for g in games]
        infos = [bridge_of(g, p) for g, p in zip(games, procs)]
        check('two folders\' games run together (the game\'s own mutex held by another game meanwhile)',
              all(p.poll() is None for p in procs) and all(infos), [p.poll() for p in procs])
        if all(infos):
            pings = [Bridge(i).call('ping') for i in infos]
            check('each its own bridge: other ports, its own folder',
                  infos[0]['port'] != infos[1]['port'] and all(r['ok'] for r in pings)
                  and [os.path.normcase(r['result']['game_dir']) for r in pings] == [os.path.normcase(g + os.sep) for g in games],
                  (infos, pings))
        again = start(games[0])
        out, _ = again.communicate(timeout=20)
        check('the same folder again quits, as the game does', again.returncode == 3 and b'another game is running' in out,
              (again.returncode, out[-300:]))
        link = os.path.join(root, 'a_link')
        subprocess.run(['cmd', '/c', 'mklink', '/J', link, games[0]], capture_output=True)
        if os.path.isdir(link):
            again = start(link)
            out, _ = again.communicate(timeout=20)
            check('the same folder through a junction quits too', again.returncode == 3, (again.returncode, out[-300:]))
            os.rmdir(link)
        log = open(os.path.join(games[0], 'mml', 'logs', 'mml.log'), encoding='utf-8').read()
        check('the running game\'s log is its own still (not turned over by the second), the folder\'s mutex in it',
              "the game's one-at-a-time mutex is this folder's" in log and log.count('MafiaModLoader (API') == 1, log[:600])
        after = bridge_of(games[0], procs[0])
        check('the running game\'s bridge.json is its own still', after.get('pid') == infos[0].get('pid')
              and Bridge(after).call('ping')['ok'], after)
    finally:
        for g, p in zip(games, procs):
            open(os.path.join(g, 'quit'), 'w').close()
        for p in procs:
            try:
                p.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
                p.communicate()
        if player:
            k32.CloseHandle(ctypes.c_void_p(player))
    check('both exit cleanly', all(p.returncode == 0 for p in procs), [p.returncode for p in procs])
    shutil.rmtree(root, ignore_errors=True)


def unicode_dir_tests():
    """The game in a folder whose name is not ASCII (a player's "C:\\Игры\\Mafia"): every path the loader keeps is
    UTF-8 and Windows is asked through its wide functions (an ANSI path once threw in json's dump at the bridge's
    start: 2026-10-17). The harness's folder copied under such names: the bridge's file and replies, the log, the
    mods loaded, a mod's data folder, and a mod's file given to the game's ANSI file layer (in the system's code page,
    or by the folder's short name when the code page has no spelling for it)."""
    import ctypes
    root = os.path.join(ROOT, 'build', 'x86', 'harness_unicode')
    # Cyrillic (the system's code page spells it where it is 1251, not where it is 1252), then a name no ANSI code
    # page spells
    for name in ('Игры Мафия', 'Mafia \u2603 \u8a66'):
        shutil.rmtree(root, ignore_errors=True)
        game = os.path.join(root, name)
        shutil.copytree(HARNESS, game)
        for sub in ('live', 'logs', 'crash', 'data', 'shots', 'restore'):
            shutil.rmtree(os.path.join(game, 'mml', sub), ignore_errors=True)
        for mod in ('boxes', 'heavy', 'hello_c'):   # (not needed here; boxes makes a Direct3D device)
            shutil.rmtree(os.path.join(game, 'mml', 'mods', mod), ignore_errors=True)
        for stale in (os.path.join(game, 'mml', 'bridge.json'), os.path.join(game, 'quit'), os.path.join(game, 'crash')):
            if os.path.exists(stale):
                os.remove(stale)
        acp = 'cp%d' % ctypes.windll.kernel32.GetACP()
        try:
            game.encode(acp)
            spelled = True
        except UnicodeEncodeError:
            buf = ctypes.create_unicode_buffer(1024)
            n = ctypes.windll.kernel32.GetShortPathNameW(game, buf, 1024)
            spelled = 0 < n < 1024 and all(ord(c) < 128 for c in buf.value)   # the volume makes short names
        tag = 'a folder named %r: ' % name
        info_path = os.path.join(game, 'mml', 'bridge.json')
        proc = subprocess.Popen([os.path.join(game, 'mml_harness.exe'), '--seconds', '30'], cwd=game,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(200):
            if os.path.exists(info_path) or proc.poll() is not None:
                break
            time.sleep(0.05)
        time.sleep(0.3)
        try:
            info = json.load(open(info_path, encoding='utf-8')) if os.path.exists(info_path) else {}
            check(tag + 'bridge.json written, UTF-8, the game\'s folder in it',
                  os.path.normcase(info.get('game_dir', '')) == os.path.normcase(game + os.sep), info)
            if info:
                b = Bridge(info)
                r = b.call('ping')
                check(tag + 'ping: the game\'s folder', r['ok'] and os.path.normcase(r['result']['game_dir']) == os.path.normcase(game + os.sep), r)
                time.sleep(0.2)
                r = b.call('mods.list')
                loaded = {m['id']: m['loaded'] for m in r['result']} if r['ok'] else {}
                check(tag + 'mods loaded: core, kit, hook_a, files_b', all(loaded.get(m) for m in ('core', 'kit', 'hook_a', 'files_b')), loaded)
                r = b.call('kit.write')
                want = os.path.join(game, 'mml', 'data', 'kit', 'note.txt')
                check(tag + 'mod_data_dir: the mod writes there (mml_fopen)', r['ok'] and os.path.normcase(r['result']) == os.path.normcase(want)
                      and os.path.exists(want), r)
                r = b.call('files.which', {'name': 'tables\\test.txt'})
                check(tag + 'files.which: the mod\'s file, its path', r['ok'] and r['result'].get('mod') == 'files_b' and
                      os.path.normcase(r['result'].get('path', '')).startswith(os.path.normcase(game)), r)
                tail = '\n'.join(b.call('log.tail', {'lines': 50})['result'])
                check(tag + 'log.tail: the folder in the loader\'s first line', name in tail, tail[:400])
        finally:
            open(os.path.join(game, 'quit'), 'w').close()
            try:
                out, _ = proc.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
        text = out.decode(errors='replace')
        check(tag + 'the harness exits cleanly (no crash)', proc.returncode == 0, (proc.returncode, text[-400:]))
        check(tag + 'a mod\'s file given to the game\'s ANSI file layer before the first frame (%s)'
              % ('by its ANSI name' if spelled else 'no ANSI name: opened by its wide path'),
              "before the first frame, tables\\test.txt is 'files_b'" in text, text[-400:])
        log = os.path.join(game, 'mml', 'logs', 'mml.log')
        try:
            body = open(log, encoding='utf-8').read()
            check(tag + 'the log file: UTF-8, the folder in it', name in body, body[:300])
        except (OSError, UnicodeDecodeError) as e:
            check(tag + 'the log file: UTF-8, the folder in it', False, e)
    shutil.rmtree(root, ignore_errors=True)


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


def video_tests(b):
    """The loader's video player (host/video.cpp) without a game: FFmpeg loaded from mml\\ffmpeg (delay-loaded), the
    search order (a mod's files before the game's folder, mp4 before bik), a Bink 2 file refused with its reason, an
    H.264 clip (tests/videos/clip.mp4: FFmpeg's testsrc, 64x48, with AAC) opened and its streams told. main() puts
    the files: video_t's files\\clip.bik (a KB2 header) and clip.mp4 in the game's folder."""
    r = b.call('video.state')
    check('video.state: built with FFmpeg, not hooked without a game, the extensions in their order',
          r['ok'] and r['result']['ffmpeg']['built'] and r['result']['hooked'] is False
          and r['result']['extensions'] == ['mp4', 'mkv', 'webm', 'mov', 'bik', 'avi'], r)
    r = b.call('video.find', {'name': 'clip.avi'})
    c = r['result']['candidates'] if r['ok'] else []
    check('video.find: the mod\'s file first, then the game folder\'s', [x['from'] for x in c] == ['mod video_t', "the game's folder"]
          and c[0]['file'].lower().endswith('clip.bik') and c[1]['file'].lower().endswith('clip.mp4'), r)
    check('video.find: a Bink 2 file refused as such', len(c) == 2 and c[0]['playable'] is False and 'Bink 2 (KB2j)' in c[0].get('why', ''), c[:1])
    check('video.find: the H.264 clip opened, its size and its sound told', len(c) == 2 and c[1]['playable'] is True
          and c[1]['width'] == 64 and c[1]['height'] == 48 and c[1]['video'].startswith('h264') and c[1]['audio'].startswith('aac'), c[1:])
    # who decodes it: the GPU when the driver says it has the decoder (the harness has no game device: the first adapter),
    # else the CPU with the reason; asked for, the CPU; a way that is no way, refused
    check('video.find: the decoder chosen and said (D3D11VA, DXVA2 or software with its reason)',
          len(c) == 2 and c[1].get('decode') in ('D3D11VA', 'DXVA2', 'software') and (c[1]['decode'] != 'software' or c[1].get('decode_why')), c[1:])
    r = b.call('video.find', {'name': 'clip', 'decode': 'software'})
    c = r['result']['candidates'] if r['ok'] else []
    check('video.find decode software: decoded by the CPU, as asked', len(c) == 2 and c[1].get('decode') == 'software'
          and c[1].get('decode_why') == 'software asked for', c[1:])
    r = b.call('video.find', {'name': 'clip', 'decode': 'gpu'})
    check('video.find decode: an unknown way refused', not r['ok'] and 'decode' in str(r.get('error', '')), r)
    r = b.call('video.state')
    check('video.state: FFmpeg loaded (LGPL), from mml\\ffmpeg', r['ok'] and r['result']['ffmpeg']['state'] == 'loaded'
          and 'LGPL' in r['result']['ffmpeg'].get('license', '') and r['result']['ffmpeg'].get('version'), r['result'].get('ffmpeg'))
    r = b.call('video.play', {'name': 'clip'})
    check('video.play: refused without a game', not r['ok'] and 'not hooked' in str(r.get('error', '')), r)
    r = b.call('video.find', {'name': 'none_such.avi'})
    check('video.find: a name with no file: no candidates', r['ok'] and r['result']['candidates'] == [], r)


def rust_tests(b):
    """The Rust mod kit (sdk/rust/mml): kit_rs (tests/mods/kit_rs) and the example hello_rust, through the bridge."""
    if not RUST:
        print('SKIPPED: the Rust mods (no kit_rs.dll: cargo or the i686-pc-windows-msvc target missing at the configure)')
        return
    gen = subprocess.run(['cargo', 'run', '--release', '--quiet', '--manifest-path',
                          os.path.join(ROOT, 'tools', 'rust_sys', 'Cargo.toml'), '--', '--check'],
                         capture_output=True, text=True)
    check('sdk/rust/mml/src/sys.rs is what mml.h gives (tools/rust_sys --check)', gen.returncode == 0, gen.stdout + gen.stderr)
    # every member of MmlHost has its safe method in the crate (a call through f! or host_fn! names it)
    rs = os.path.join(ROOT, 'sdk', 'rust', 'mml', 'src')
    sysrs = open(os.path.join(rs, 'sys.rs'), encoding='utf-8').read()
    body = sysrs[sysrs.index('pub struct MmlHost {'):]
    body = body[:body.index('\n}\n')]
    members = [m for m in re.findall(r'\n    pub (\w+):', body) if m not in ('size', 'api_version')]
    src = ''.join(open(f, encoding='utf-8').read() for f in glob.glob(os.path.join(rs, '**', '*.rs'), recursive=True)
                  if os.path.basename(f) != 'sys.rs')
    used = set(re.findall(r'(?:f!|host_fn!)\(\s*[\w.]+,\s*(\w+)\)', src))
    missing = [m for m in members if m not in used]
    check('sdk/rust/mml: a safe method for every member of MmlHost (%d)' % len(members), len(members) > 150 and not missing, missing)
    gen = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen_rust_game.py'), '--check'], capture_output=True, text=True)
    check('sdk/rust/mml/src/game/tables.rs is what the game headers give (tools/gen_rust_game.py --check)', gen.returncode == 0, gen.stdout)
    r = b.call('kit_rs.offline')
    res = r.get('result') or {}
    for name, c in sorted((res.get('checks') or {}).items()):
        check('kit_rs.offline: ' + name, c['ok'], c['info'])
    check('kit_rs.offline: answered, nothing failed', r['ok'] and res.get('failed') == [] and len(res.get('checks', {})) >= 20, r)
    s = b.call('kit_rs.status')
    r = s.get('result') or {}
    check('kit_rs: loaded, its id, folder, settings (a string, a number, a missing key\'s default), no game',
          s['ok'] and r['loads'] == 1 and r['id'] == 'kit_rs' and r['dir_ends'] and r['name'] == 'kit_rs'
          and r['speed'] == 12.5 and r['missing'] == -7 and r['build'] == 'unknown' and r['mission'] == '', s)
    time.sleep(0.2)
    check('kit_rs: its FRAME handler runs', b.call('kit_rs.status')['result']['frames'] > r['frames'])
    check('kit_rs: typed params (serde)', b.call('kit_rs.add', {'a': 2, 'b': 40}).get('result') == 42)
    r = b.call('kit_rs.add', {'a': 2})
    check('kit_rs: params missing: an error saying so', not r['ok'] and 'params' in str(r) and 'b' in str(r), r)
    r = b.call('kit_rs.fail')
    check('kit_rs: an Err is an error reply', not r['ok'] and 'refused on purpose' in str(r), r)
    r = b.call('kit_rs.timer')
    check('kit_rs: a timer added (it waits for the mission\'s time) and removed; one of 0 ms refused: None',
          r['ok'] and r['result']['id'] > 0 and r['result']['removed'] and r['result']['zero_refused'], r)
    check('kit_rs: post from another thread', b.call('kit_rs.post').get('result') is True)
    time.sleep(0.2)
    check('kit_rs: ... run on the game thread at a frame', b.call('kit_rs.status')['result']['posted'] == 1)
    before = b.call('kit_rs.status')['result']
    r = b.call('mod.reload', {'id': 'kit_rs'})
    check('kit_rs: reloaded', r['ok'] and r['result']['generation'] == 2, r)
    after = b.call('kit_rs.status')['result']
    check('kit_rs: state across the reload (bytes and JSON)', after['loads'] == 2 and after['frames'] >= before['frames'], (before, after))
    tail = '\n'.join(b.call('log.tail', {'lines': 100})['result'])
    check('kit_rs: what its closures held dropped at the unload', 'kit_rs: its closures dropped' in tail, tail[-600:])
    check('kit_rs: a call posted at the unload dropped there, never run',
          'kit_rs: a call posted at the unload dropped unrun' in tail and 'posted at the unload ran' not in tail, tail[-600:])
    # the example
    r = b.call('hello_rust.status')
    check('hello_rust: status', r['ok'] and r['result']['loads'] == 1 and r['result']['game'] == 'unknown', r)
    check('hello_rust: say', b.call('hello_rust.say', {'text': 'hi from the tests'})['ok'])
    r = b.call('hello_rust.spawn')
    check('hello_rust: spawn with no game: an error saying why', not r['ok'] and 'no player' in str(r), r)
    # a panic: logged with its message and place, then a fault of the mod's as a C mod's
    r = b.call('kit_rs.panic')
    check('kit_rs: a panic in a command: an error reply', not r['ok'] and 'faulted' in str(r), r)
    time.sleep(0.2)
    loaded = {m['id']: m['loaded'] for m in b.call('mods.list')['result']}
    check('kit_rs: unloaded at the next frame, the game goes on', loaded.get('kit_rs') is False and b.call('ping')['ok'], loaded)
    tail = '\n'.join(b.call('log.tail', {'lines': 100})['result'])
    check('kit_rs: the panic logged: its message and its place',
          re.search(r'panicked in a command: kit_rs panics on purpose at .*lib\.rs:\d+', tail) is not None, tail[-800:])
    r = b.call('events.since', {'types': 'MOD_FAULTED', 'filter': {'name': 'kit_rs'}})
    ev = r['result']['events'] if r['ok'] else []
    check('kit_rs: MOD_FAULTED with the panic\'s exception (E0525553)', len(ev) == 1 and ev[0]['data']['exception'].upper() == 'E0525553', r)
    r = b.call('mod.load', {'id': 'kit_rs'})
    check('kit_rs: loaded again after its fault', r['ok'] and b.call('kit_rs.add', {'a': 1, 'b': 1}).get('result') == 2, r)


OUT64 = os.path.join(ROOT, 'build', 'x64', 'out')
SIDECAR = os.path.join(OUT64, 'mml', 'sidecar', 'mml-sidecar64.exe')


def wait_for(fn, seconds=8.0):
    """fn() until it is true (its last value), polled every 0.1 s."""
    end = time.time() + seconds
    v = fn()
    while not v and time.time() < end:
        time.sleep(0.1)
        v = fn()
    return v


def channel_checks(b, when):
    """side_test's channels both ways: box written by the 64-bit half, read by the 32-bit one; back the other way."""
    v0 = (b.call('side_test.peek').get('result') or {}).get('version', 0)
    w = b.call('side_test.write', {'text': 'from 64 bits', 'when': when})
    r = b.call('side_test.peek')
    got = r.get('result') or {}
    check('a channel from the 64-bit half to the 32-bit one (%s): the latest version, whole' % when,
          w['ok'] and r['ok'] and json.loads(got.get('text') or 'null') == {'text': 'from 64 bits', 'when': when} and got['version'] > v0, (w, r))
    w = b.call('side_test.post', {'n': 7, 'when': when})
    r = b.call('side_test.fetch')
    got = r.get('result') or {}
    check('a channel from the 32-bit half to the 64-bit one (%s)' % when,
          w['ok'] and r['ok'] and json.loads(got.get('text') or 'null') == {'n': 7, 'when': when} and got['version'] >= 1, (w, r))


def sidecar_tests(b):
    """The 64-bit sidecar (host/sidecar.cpp, sidecar/main.cpp): side_test's 64-bit half loaded in it, its commands
    answered from 64 bits, its calls to the bridge, the frames told to it, a reload, a fault (the half unloaded, the
    game going on), the sidecar killed and started again with its halves."""
    if not os.path.exists(SIDECAR):
        check('the sidecar is built (cmake --preset x64, cmake --build --preset x64-release)', False, SIDECAR)
        return

    def half():
        r = b.call('mods.list')
        m = next((m for m in r['result'] if m['id'] == 'side_test'), {}) if r['ok'] else {}
        return m.get('x64', {})
    h = wait_for(lambda: half().get('state') == 'loaded' and half())
    check('side_test: its 64-bit half loaded in the sidecar', h and h.get('state') == 'loaded', half())
    r = b.call('side_test.echo', {'a': 1, 'b': [2, 3]})
    check('a 64-bit command answered from 64 bits, its args as they were sent',
          r['ok'] and r['result'].get('args') == {'a': 1, 'b': [2, 3]} and r['result'].get('pointer_bits') == 64, r)
    pid = b.call('ping')['result']['pid']
    r = b.call('side_test.call', {'method': 'ping'})
    check('a bridge call from the 64-bit side (ping, the bridge\'s thread): the game\'s pid', r['ok'] and r['result'].get('pid') == pid, r)
    r = b.call('side_test.call', {'method': 'mods.list'})
    check('a bridge call from the 64-bit side answered by the game thread (mods.list)',
          r['ok'] and isinstance(r['result'], list) and any(m['id'] == 'side_test' for m in r['result']), str(r)[:300])
    r = b.call('side_test.call', {'method': 'kit.status'})
    check('a 32-bit mod\'s command called from the 64-bit side', r['ok'], str(r)[:300])
    f1 = b.call('side_test.frames')['result']['frames']
    time.sleep(0.5)
    f2 = b.call('side_test.frames')['result']['frames']
    check('FRAME events reach the 64-bit half (%d -> %d)' % (f1, f2), f2 > f1 > 0, (f1, f2))
    r = b.call('commands.list')
    cmd = next((c for c in r['result'] if c['name'] == 'side_test.echo'), None) if r['ok'] else None
    check('commands.list: the 64-bit half\'s commands, by the sidecar', cmd and cmd.get('thread') == 'sidecar', cmd)
    r = b.call('sidecar.state')
    st = r['result'] if r['ok'] else {}
    check('sidecar.state: running, the half loaded and subscribed to FRAME',
          st.get('running') and st['halves']['side_test']['state'] == 'loaded' and 'FRAME' in st['halves']['side_test']['subscribed'], st)
    channel_checks(b, 'the first load')
    r = b.call('side_test.write_later', {'ms': 300})
    w = b.call('side_test.wait_box', {'wait_ms': 3000}) if r['ok'] else r
    wr = w.get('result') or {}
    check('channel_wait in the 32-bit half: woken by the 64-bit half\'s write 300 ms later, not by its timeout (%s ms)' % wr.get('ms'),
          w['ok'] and wr['version'] > wr['after'] and 200 <= wr['ms'] < 2000, (r, w))
    w = b.call('side_test.wait_box', {'wait_ms': 150})
    wr = w.get('result') or {}
    check('channel_wait: nothing written, 0 after its timeout (%s ms)' % wr.get('ms'), w['ok'] and wr['version'] == 0 and 130 <= wr['ms'] < 1000, w)
    r = b.call('side_test.post_later', {'ms': 300})
    w = b.call('side_test.wait_back', {'wait_ms': 3000}) if r['ok'] else r
    wr = w.get('result') or {}
    check('channel_wait in the 64-bit half: woken by the 32-bit half\'s write 300 ms later (%s ms)' % wr.get('ms'),
          w['ok'] and wr['version'] > wr['after'] and 200 <= wr['ms'] < 2000, (r, w))
    r = b.call('side_test.refusals')
    check('channels: another size refused, a version longer than the channel refused; gfx_shared_texture outside a pass: 0',
          r['ok'] and r['result'] == {'other_size': False, 'too_long': 0, 'texture_outside_pass': 0}, r)
    r = b.call('side_test.flood')
    t = b.call('side_test.torn') if r['ok'] else r
    tr = t.get('result') or {}
    check('a channel across the processes: read while the other half writes as fast as it can, every read one whole '
          'version, none gone back (%s reads, %s versions)' % (tr.get('reads'), tr.get('versions')),
          t['ok'] and tr['reads'] > 1000 and tr['versions'] > 100 and tr['torn'] == 0 and tr['backwards'] == 0, t)

    # a reload: both halves (here one), from a fresh copy
    gen = h.get('generation', 0) if h else 0
    b.call('mod.reload', {'id': 'side_test'})
    h = wait_for(lambda: half().get('state') == 'loaded' and half().get('generation', 0) > gen and half())
    r = b.call('side_test.frames')
    check('mod.reload: the 64-bit half loaded again, its state new', h and r['ok'] and r['result']['frames'] < f2 + 1000, (h, r))
    channel_checks(b, 'after mod.reload')

    # a fault in the 64-bit half: unloaded, told; the sidecar and the game go on
    seq = b.call('events.since', {'seq': 1 << 62, 'limit': 1})['result']['latest']
    r = b.call('side_test.fault')
    check('a 64-bit command that faults: an error', not r['ok'] and 'faulted' in r.get('error', ''), r)
    h = wait_for(lambda: half().get('state') == 'faulted' and half())
    check('the faulted half: marked faulted, where it faulted named', h and 'side_test' in h.get('where', '') + h.get('error', ''), half())
    ev = b.call('events.since', {'seq': seq, 'limit': 50})['result']['events']
    check('MOD_FAULTED told, the 64-bit half named', any(e['type'] == 'MOD_FAULTED' and e['data'].get('half') == 'x64' for e in ev), ev[-5:])
    check('the game goes on after a 64-bit fault', b.call('ping')['ok'] and b.call('kit.status')['ok'])
    b.call('mod.reload', {'id': 'side_test'})
    check('a faulted half loaded again by mod.reload', wait_for(lambda: half().get('state') == 'loaded'), half())

    # a reload whose channel changed size, in one go: the old 64-bit half holds "grow" at the old size for 300 ms
    # after it is told to go; the new 32-bit half's channel_open waits for it instead of being refused
    grow_txt = os.path.join(HARNESS, 'mml', 'data', 'side_test', 'grow.txt')
    os.makedirs(os.path.dirname(grow_txt), exist_ok=True)
    for size in (4096, 8192):
        with open(grow_txt, 'w') as f:
            f.write(str(size))
        gen = half().get('generation', 0)
        r = b.call('mod.reload', {'id': 'side_test'})
        loaded = r['ok'] and wait_for(lambda: half().get('state') == 'loaded' and half().get('generation', 0) > gen)
        g = wait_for(lambda: (lambda x: x['ok'] and x['result']['bytes'] == size and x)(b.call('side_test.grow')))
        check('a channel of %d bytes after one mod.reload: both halves loaded, the 64-bit half\'s version whole' % size,
              loaded and g and g['result']['size'] == size, (r, half(), b.call('side_test.grow')))
    os.remove(grow_txt)
    gen = half().get('generation', 0)
    b.call('mod.reload', {'id': 'side_test'})
    wait_for(lambda: half().get('state') == 'loaded' and half().get('generation', 0) > gen)

    # the sidecar killed: SIDECAR_EXITED, started again with its halves
    st = b.call('sidecar.state')['result']
    seq = b.call('events.since', {'seq': 1 << 62, 'limit': 1})['result']['latest']
    subprocess.run(['taskkill', '/F', '/PID', str(st.get('pid'))], capture_output=True)
    ev = wait_for(lambda: [e for e in b.call('events.since', {'seq': seq, 'limit': 50})['result']['events'] if e['type'] == 'SIDECAR_EXITED'])
    check('the sidecar killed: SIDECAR_EXITED, a restart promised', ev and ev[0]['data'].get('restart'), ev)
    h = wait_for(lambda: half().get('state') == 'loaded' and half(), 10)
    r = b.call('side_test.echo', {'again': True})
    st2 = b.call('sidecar.state')['result']
    check('started again: another process, the half loaded, its commands answer',
          h and r['ok'] and st2.get('pid') not in (None, st.get('pid')), (h, r, st2.get('pid')))


def ghidra_names_tests():
    """What the Ghidra projects are given (tools/ghidra_names.py), with no Ghidra: the ranks, the SDK's aliases, 1.0's
    names through the twins, the prototypes made from the decompile's signatures, a name and an annotation written
    and forgotten (in a copy of the knowledge base)."""
    import tempfile
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import ghidra_names as g
    n12 = g.names('Game.exe', '1.2')
    check('ghidra names: 1.2 C_human::Hit (the decompile\'s) over the SDK\'s humanHit, kept as an alias',
          n12.best[0x5762A0].name == 'C_human::Hit' and 'humanHit' in n12.aliases.get(0x5762A0, []), n12.best.get(0x5762A0))
    n10 = g.names('Game.exe', '1.0')
    check('ghidra names: 1.0 named through the twins (C_human::Hit at 496710), the SDK\'s 1.0 tables',
          n10.best[0x496710].name == 'C_human::Hit' and len(n10.best) > 2500, n10.best.get(0x496710))
    tw10 = set(g.twins().values())
    own = [n for at, n in n10.best.items() if at not in tw10 and n.source == 'st0rm94']
    check('ghidra names: 1.0 takes st0rm94\'s own names of functions with no 1.2 twin (external_10.csv)',
          len(own) > 2000 and len(n10.best) > 5000, (len(own), len(n10.best)))
    sig = g.c_signature
    check('ghidra prototypes: thiscall\'s first argument left out (whatever its name), an unknown class pointer void *',
          sig('C_actor *__thiscall sub_445290(C_actor *lpMem, char a2)') == 'void * __thiscall f(char a2)'
          and sig('int __thiscall f(_DWORD *this, int a2, float *a3)') == 'int __thiscall f(int a2, float * a3)', sig('C_actor *__thiscall sub_445290(C_actor *lpMem, char a2)'))
    check('ghidra prototypes: registers and values of an unknown type refused',
          sig('int __userpurge sub_430142@<eax>(int a1@<edi>, int a2)') is None and sig('int __cdecl f(S_vector v)') is None
          and sig('void sub_401030()') == 'void f(void)')
    check('ghidra names: a namespace split outside templates', g.split_name('std::vector<a::b>::push_back') == (['std', 'vector<a::b>'], 'push_back'))
    # LS3DF: 1.0 named through its own twins (tools/kb_demo.py's twins_10.csv), the SDK's pairs among them
    tw = g.twins('LS3DF.dll')
    l10 = g.names('LS3DF.dll', '1.0')
    check('ghidra names: LS3DF 1.0 twins (2000 at least, frameUpdateWMatrix 1C7D0 -> 1C570), the 1.2 name carried',
          len(tw) > 2000 and tw.get(0x1001C7D0) == 0x1001C570
          and l10.best[0x1001C570].name == g.names('LS3DF.dll', '1.2').best[0x1001C7D0].name, (len(tw), l10.best.get(0x1001C570)))
    ds = g.demo_signature
    check('ghidra prototypes: the demo\'s signatures (a stdcall method\'s this on the stack, thiscall\'s in ECX, '
          'references as pointers, a struct by value refused)',
          ds('public: virtual bool __stdcall C_x::IsOn(void)') == 'bool __stdcall f(void * this)'
          and ds('public: void __thiscall C_x::Move(struct S_vector const &)') == 'void __thiscall f(void * a1)'
          and ds('bool __fastcall Clip(char const *,int,struct T *,bool)') == 'bool __fastcall f(char * a1, int a2, void * a3, bool a4)'
          and ds('public: static void __cdecl C_x::f(unsigned int,float)') == 'void __cdecl f(unsigned int a1, float a2)'
          and ds('public: struct S_vector __thiscall S_vector::operator*(float const &)const ') is None
          and ds('enum R __fastcall Read(char const *)') == 'int __fastcall f(char * a1)',
          ds('public: virtual bool __stdcall C_x::IsOn(void)'))
    import kb_demo as kd
    check('kb_demo: C names undecorated, vtables named for Ghidra, the compiler\'s literals not named',
          kd.demangle('_c_alloc', 0) == 'c_alloc' and kd.demangle('_RandFloat@8', 0) == 'RandFloat'
          and kd.clean_name("C_x::`vftable'") == 'C_x::vftable'
          and kd.clean_name("C::`vftable'{for `B::I'}") == 'C::vftable_for_B_I'
          and not kd.worth_naming('??_C@_0BA@abc@', '.rdata') and not kd.worth_naming('__real@4@3f800000', '.rdata')
          and kd.worth_naming('?g_x@@3MA', '.data'))
    check('kb_demo: names agree by their last part (ours UpdateWMatrix, the export\'s I3D_frame::UpdateWMatrixProc)',
          kd.agrees('UpdateWMatrix', 'I3D_frame::UpdateWMatrixProc') and not kd.agrees('frameSlot18', 'C_x::Move'))
    with tempfile.TemporaryDirectory() as tmp:
        kb = g.KB
        try:
            g.KB = tmp
            g.record_name('Game.exe', '1.0', 0x401000, 'function', 'C_t::A', 'a test')
            g.record_annotation('Game.exe', '1.0', 0x401000, 'proto', 'int f(void)')
            had = [g.rows(g.names_file('Game.exe', '1.0')), g.annotations('Game.exe', '1.0')]
            gone = g.forget('Game.exe', '1.0', 0x401000)
            check('ghidra names: written (names_10.csv, annotations.csv) and forgotten, the files gone with their rows',
                  had[0][0]['name'] == 'C_t::A' and had[1][0]['kind'] == 'proto' and len(gone) == 2
                  and not os.path.exists(g.names_file('Game.exe', '1.0')), (had, gone))
        finally:
            g.KB = kb


RUST = os.path.exists(os.path.join(HARNESS, 'mml', 'mods', 'kit_rs', 'kit_rs.dll'))
NEW_MODS = (('newmod_cpp', False), ('newmod_rs', True))   # tools/new_mod.py's mods: made, built, loaded, taken away
NEW_MOD_DIR = 'build/x86/new_mod_test'                     # (their sources: --dir, in the build's ignored folder)


def new_mod_lines(text):
    """CMakeLists.txt without the lines new_mod.py added for the tests' mods."""
    pattern = re.compile(r'mml_add_(rust_)?mod\((%s)\s' % '|'.join(m for m, _ in NEW_MODS))
    return ''.join(line for line in text.splitlines(True) if not pattern.search(line))


def new_mod_build():
    """tools/new_mod.py as a mod author runs it, then the way its output says: a C++ mod and one in Rust (--rust,
    when the build has the i686 target) made in build/x86/new_mod_test/, configured and built by the project's
    build, copied into the harness (switched off: new_mod_tests loads them). Then everything it made is taken away
    before the harness starts: the sources, the CMake lines (configured again, so the build forgets their targets
    and removes their output folders). The ids it built."""
    cmake_path = os.path.join(ROOT, 'CMakeLists.txt')
    src_root = os.path.join(ROOT, *NEW_MOD_DIR.split('/'))
    mods = os.path.join(HARNESS, 'mml', 'mods')
    run = lambda cmd: subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace')

    def take_away():   # (also what an interrupted run left)
        text = open(cmake_path, encoding='utf-8', newline='').read()
        if new_mod_lines(text) != text:
            with open(cmake_path, 'w', encoding='utf-8', newline='') as f:
                f.write(new_mod_lines(text))
        shutil.rmtree(src_root, ignore_errors=True)

    take_away()
    original = open(cmake_path, encoding='utf-8', newline='').read()
    for mod, _ in NEW_MODS:
        shutil.rmtree(os.path.join(mods, mod), ignore_errors=True)
    wanted = [(mod, rust) for mod, rust in NEW_MODS if RUST or not rust]
    if not RUST:
        print('SKIPPED: new_mod.py --rust (the build has no Rust mods: rustup target add i686-pc-windows-msvc, '
              'then cmake --preset x86)')
    built = []
    try:
        for mod, rust in wanted:
            r = run([sys.executable, os.path.join(ROOT, 'tools', 'new_mod.py'), mod, '--dir', NEW_MOD_DIR + '/' + mod]
                    + (['--rust'] if rust else []))
            check('new_mod.py %s%s: its files made, its line in CMakeLists.txt' % (mod, ' --rust' if rust else ''),
                  r.returncode == 0 and 'CMakeLists.txt has its line' in r.stdout
                  and os.path.exists(os.path.join(src_root, mod, 'mod.json')), r.stdout + r.stderr)
        r = run(['cmake', '--preset', 'x86'])
        check('new_mod.py\'s mods: the build configured with them', r.returncode == 0, (r.stdout + r.stderr)[-1500:])
        targets = [('rust_' if rust else 'mod_') + mod for mod, rust in wanted]
        r = run(['cmake', '--build', '--preset', 'x86-release', '--target'] + targets)
        check('new_mod.py\'s mods: built by the project\'s build (%s)' % ', '.join(targets), r.returncode == 0,
              (r.stdout + r.stderr)[-3000:])
        for mod, _ in wanted:
            out = os.path.join(OUT, 'mml', 'mods', mod)
            if not all(os.path.exists(os.path.join(out, f)) for f in (mod + '.dll', 'mod.json', 'config.json')):
                check('new_mod.py %s: its DLL, mod.json and config.json in the build\'s output' % mod, False, out)
                continue
            shutil.copytree(out, os.path.join(mods, mod))
            manifest = os.path.join(mods, mod, 'mod.json')
            text = open(manifest, encoding='utf-8').read()
            with open(manifest, 'w', encoding='utf-8') as f:
                f.write(text.replace('"enabled": true', '"enabled": false'))
            built.append(mod)
    finally:
        take_away()
        r = run(['cmake', '--preset', 'x86'])
        listed = open(os.path.join(OUT, 'mml', 'build_mods.txt'), encoding='utf-8').read().split()
        check('new_mod.py\'s mods taken away: sources, CMakeLists.txt as it was, the build configured again without '
              'them (their output folders gone)',
              r.returncode == 0 and not os.path.exists(src_root) and open(cmake_path, encoding='utf-8', newline='').read() == original
              and not any(os.path.exists(os.path.join(OUT, 'mml', 'mods', m)) or m in listed for m, _ in NEW_MODS),
              (r.stdout + r.stderr)[-1500:])
    return built


def new_mod_tests(b, built):
    """The mods tools/new_mod.py made (new_mod_build): each loads, answers its command, unloads."""
    for mod in built:
        r = b.call('mod.load', {'id': mod})
        check('new_mod.py\'s %s: loaded' % mod, r['ok'], r)
        r = b.call(mod + '.status')
        check('new_mod.py\'s %s: %s.status answers (presses, mission, game)' % (mod, mod),
              r['ok'] and r['result'].get('presses') == 0 and 'mission' in r['result'] and 'game' in r['result'], r)
        r = b.call('mod.unload', {'id': mod})
        loaded = {m['id']: m['loaded'] for m in b.call('mods.list')['result']}
        check('new_mod.py\'s %s: unloaded, its command gone' % mod,
              r['ok'] and loaded.get(mod) is False and not b.call(mod + '.status')['ok'], (r, loaded.get(mod)))


def main():
    ghidra_names_tests()
    new_mods = new_mod_build()
    # the harness's game folder: the loader, the test mods and the shipped ones
    mods = os.path.join(HARNESS, 'mml', 'mods')
    shipped = ('core', 'hello_c', 'boxes') + (('hello_rust',) if RUST else ())
    for mod in shipped:
        shutil.rmtree(os.path.join(mods, mod), ignore_errors=True)
        shutil.copytree(os.path.join(OUT, 'mml', 'mods', mod), os.path.join(mods, mod))
    # the examples ship disabled; the harness loads hello_c and boxes from the start (boxes' world kept across the
    # loads boxes_tests makes)
    for mod in shipped[1:]:
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
    # the video player's (video_tests): FFmpeg as the build put it, a mod with a Bink 2 file, a clip in the game folder
    shutil.rmtree(os.path.join(HARNESS, 'mml', 'ffmpeg'), ignore_errors=True)
    if os.path.isdir(os.path.join(OUT, 'mml', 'ffmpeg')):
        shutil.copytree(os.path.join(OUT, 'mml', 'ffmpeg'), os.path.join(HARNESS, 'mml', 'ffmpeg'))
    vt = os.path.join(mods, 'video_t')
    shutil.rmtree(vt, ignore_errors=True)
    os.makedirs(os.path.join(vt, 'files'))
    with open(os.path.join(vt, 'mod.json'), 'w') as f:
        f.write('{ "id": "video_t", "name": "test: a video", "version": "1", "type": "files", "load_order": 60 }\n')
    with open(os.path.join(vt, 'files', 'clip.bik'), 'wb') as f:
        f.write(b'KB2j' + bytes(252))
    shutil.copy2(os.path.join(ROOT, 'tests', 'videos', 'clip.mp4'), os.path.join(HARNESS, 'clip.mp4'))
    # the sidecar and side_test's 64-bit half (the x64 preset's build) beside its 32-bit half (this build's)
    side_dir = os.path.join(HARNESS, 'mml', 'sidecar')
    shutil.rmtree(side_dir, ignore_errors=True)
    shutil.rmtree(os.path.join(mods, 'side_test', 'x64'), ignore_errors=True)
    if os.path.exists(SIDECAR):
        shutil.copytree(os.path.dirname(SIDECAR), side_dir)
        shutil.copytree(os.path.join(OUT64, 'tests', 'side_test', 'x64'), os.path.join(mods, 'side_test', 'x64'))
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
    proc = subprocess.Popen([os.path.join(HARNESS, 'mml_harness.exe'), '--seconds', '150'], cwd=HARNESS,
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
        frames_tests(b)
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
        video_tests(b)
        heavy_tests(b)
        boxes_tests(b)
        rust_tests(b)
        new_mod_tests(b, new_mods)
        sidecar_tests(b)
        methods_tests(b)
        ui_tests(b)
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
    for mod in new_mods:   # (nothing of new_mod.py's mods left in the harness either)
        shutil.rmtree(os.path.join(HARNESS, 'mml', 'mods', mod), ignore_errors=True)
        shutil.rmtree(os.path.join(HARNESS, 'mml', 'data', mod), ignore_errors=True)
    log = open(os.path.join(HARNESS, 'mml', 'logs', 'mml.log'), encoding='utf-8', errors='replace').read()
    exiting = next((l for l in log.splitlines() if 'process exiting (ExitProcess' in l), '')
    walked = exiting.split('): ')[-1].lower()
    check("the process's exit: who ended it, walked by the frame pointers from the loader's detach through the "
          "system's exit to the harness's code", 0 <= walked.find('mafiamodloader.asi+0x') < walked.find('ntdll.dll+0x')
          < walked.find('mml_harness.exe+0x'), exiting)
    text = out.decode(errors='replace')
    check('the x87 given back to the game at 24 bits after every frame (the mods\' callbacks ran at 53)',
          re.search(r'x87 at 24 bits after [1-9][0-9]* frames, 0 given back otherwise', text) is not None, text[-400:])
    check('files: a mod\'s file answers before the first frame (the game\'s first tables)',
          "before the first frame, tables\\test.txt is 'files_b'" in out.decode(errors='replace'), out.decode(errors='replace'))
    crash_tests()
    dxwrapper_tests()
    instances_tests()
    unicode_dir_tests()
    print('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), ', '.join(failures)))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
