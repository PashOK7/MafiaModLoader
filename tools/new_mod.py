"""new_mod.py <id>: a new native mod, ready to build and to load.

    python tools/new_mod.py lap_ghost
    python tools/new_mod.py lap_ghost --name "Lap ghost" --dir mods/examples/lap_ghost

Makes mods/<id>/ (or --dir) with <id>.cpp (a small working mod: an overlay line, a hotkey from its settings, a
command; every part of the mod kit touched once: cut what you do not need), mod.json (the manifest) and config.json
(the defaults of its settings), and adds its line to CMakeLists.txt. Then:

    cmake --build --preset x86-release      # build/x86/out/mml/mods/<id>/
    (MCP) deploy {"mods": ["<id>"]}         # into the running game, loaded at once; again after every build

The whole way, and what the host gives a mod: docs/MODDING.md. A complete example: mods/examples/telemetry.

    python tools/new_mod.py night_signs --files

A mod of files alone (no DLL, no code): mod.json ("type": "files") and an empty files/ folder, where every file is
a game file by its path there (files/maps/sign.bmp answers the game's "maps\\sign.bmp"); its CMake line is
mml_add_files_mod. docs/MODDING.md "A mod's own files in the game"; mods/examples/freeride_picture.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = '    # (tools/new_mod.py adds a mod\'s line above this one)'

SOURCE = r'''// @ID@: @NAME@.
//
// Made by tools/new_mod.py: a working frame with one of each thing a mod usually needs. docs/MODDING.md tells what
// the host gives a mod; mods/examples/telemetry is a complete example; sdk/include/mml/mml.h is the contract.
#include <windows.h>
#include <cstdio>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_game;             // a build of the game whose addresses are known (mml/game/game_build.h)
unsigned g_presses;

// A hotkey: called on the game thread when the combination goes down.
void OnKey(void *) { g_presses++; }

// The settings: config.json next to the DLL holds the defaults; changes are kept in the mod's data folder and made
// with `config.set {"mod": "@ID@", "key": ..., "value": ...}` (or by editing that file). Read them here.
void ReadSettings() {
    H->hotkey(M, H->config_string(M, "hotkey", "F9"), OnKey, nullptr);   // (the same function again: bound anew)
}

// Every frame, in every state of the game: draw here (800x600 units, colours 0xAARRGGBB).
void OnFrame(const MmlEvent *, void *) {
    if (H->config_number(M, "show", 1) == 0) return;
    char line[128];
    snprintf(line, sizeof line, "@ID@: %s, %u presses", H->mission_name(), g_presses);
    H->draw_rect(M, 8, 8, H->draw_text_width(MML_FONT_SMALL, 14, line) + 12, 20, 0xA0000000u, 1);
    H->draw_text(M, 14, 10, 14, 0xFFFFFFFFu, MML_FONT_SMALL, MML_DRAW_LEFT, line);
}

// The mission's world moved by ev->dt seconds (not while the game is paused or loads): the game's objects here.
void OnTick(const MmlEvent *ev, void *) {
    (void)ev;
    if (!g_game) return;
    // game::Car car(game::PlayerCar());  if (car) { car.Speed(); car.Position(); ... }   (mml/game/game.h)
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    // MML_EVENT_MISSION_OPEN (ev->name), MML_EVENT_MISSION_LOADED, MML_EVENT_MISSION_CLOSE,
    // MML_EVENT_GAME_PAUSED, MML_EVENT_GAME_RESUMED: subscribe to what you need
}

// A command: agents call it over MCP as "@ID@.status". The reply is JSON text.
int Status(const char *args_json, MmlReply *reply, void *) {
    (void)args_json;
    char out[256];
    snprintf(out, sizeof out, "{\"presses\":%u,\"mission\":\"%s\",\"game\":\"%s\"}", g_presses, H->mission_name(), H->game_build());
    H->reply_append(reply, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, config_set)) return 1;                          // a loader older than the mod kit
    g_game = game::Detect(host->module_base("LS3DF.dll"));             // before any game:: call
    host->state_get(mod, "presses", &g_presses, sizeof g_presses);     // kept across this mod's own reloads
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->command(mod, "status", "what this mod has seen", Status, nullptr);
    host->log(mod, MML_LOG_INFO, "@ID@ loaded (game %s); its files go to %s", host->game_build(), host->mod_data_dir(mod));
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) {
    H->state_put(mod, "presses", &g_presses, sizeof g_presses);
}
'''

MANIFEST = '''{
  "id": "@ID@",
  "name": "@NAME@",
  "version": "0.1.0",
  "type": "native",
  "entry": "@ID@.dll",
  "enabled": true,
  "builds": ["1.2", "1.0"],
  "load_order": 100,
  "depends": ["core"]
}
'''

FILES_MANIFEST = '''{
  // A mod of files alone: every file under files/ is given to the game by its path there (docs/MODDING.md
  // "A mod's own files in the game").
  "id": "@ID@",
  "name": "@NAME@",
  "version": "0.1.0",
  "type": "files",
  "enabled": true,
  "builds": ["1.2", "1.0"],
  "load_order": 100
}
'''

CONFIG = '''{
  // The defaults of @ID@'s settings (shipped with the mod; a deploy replaces this file in the game's folder).
  // Changes are kept in <game>\\mml\\data\\@ID@\\config.json: `config.set {"mod": "@ID@", "key": "show", "value": false}`.
  "show": true,
  "hotkey": "F9"
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('id', help='the mod\'s id: its folder, its DLL, its commands\' prefix (lower case, digits, _)')
    ap.add_argument('--name', help='its name for people (default: the id)')
    ap.add_argument('--dir', help='where its sources go, relative to the repository (default: mods/<id>)')
    ap.add_argument('--files', action='store_true', help='a mod of files alone: no DLL, a files/ folder of game files')
    a = ap.parse_args()
    if not re.fullmatch(r'[a-z][a-z0-9_]{1,31}', a.id):
        sys.exit('id: a lower-case letter, then lower-case letters, digits or _ (2..32 characters)')
    if a.id in ('core', 'host'):
        sys.exit('id: "%s" is taken' % a.id)
    rel = (a.dir or 'mods/' + a.id).replace('\\', '/').strip('/')
    folder = os.path.join(ROOT, *rel.split('/'))
    cmake_path = os.path.join(ROOT, 'CMakeLists.txt')
    cmake = open(cmake_path, encoding='utf-8', newline='').read()
    if re.search(r'mml_add_(files_)?mod\(%s\s' % re.escape(a.id), cmake):
        sys.exit('CMakeLists.txt already has a mod "%s"' % a.id)
    if os.path.exists(folder) and os.listdir(folder):
        sys.exit('%s exists and is not empty' % rel)
    if MARK not in cmake:
        sys.exit('CMakeLists.txt: the line "%s" is gone; add mml_add_mod(...) by hand' % MARK.strip())
    name = a.name or a.id
    os.makedirs(folder, exist_ok=True)
    files = {'mod.json': FILES_MANIFEST} if a.files else {a.id + '.cpp': SOURCE, 'mod.json': MANIFEST, 'config.json': CONFIG}
    for fname, text in files.items():
        text = text.replace('@ID@', a.id).replace('@NAME@', name.replace('"', '\\"') if fname != a.id + '.cpp' else name)
        with open(os.path.join(folder, fname), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    nl = '\r\n' if '\r\n' in cmake else '\n'
    if a.files:
        os.makedirs(os.path.join(folder, 'files'), exist_ok=True)
        line = '    mml_add_files_mod(%s ${CMAKE_SOURCE_DIR}/%s)' % (a.id, rel)
    else:
        line = '    mml_add_mod(%s ${CMAKE_SOURCE_DIR}/%s %s/%s.cpp)' % (a.id, rel, rel, a.id)
    cmake = cmake.replace(MARK, line + nl + MARK, 1)
    with open(cmake_path, 'w', encoding='utf-8', newline='') as f:
        f.write(cmake)
    if a.files:
        print('%s: mod.json and files/ (put game files there by their paths); CMakeLists.txt has its line.' % rel)
        print('Next: cmake --preset x86 (a new target), cmake --build --preset x86-release, then (MCP) deploy {"mods": ["%s"]}' % a.id)
        print('The way from here: docs/MODDING.md "A mod\'s own files in the game"')
        return
    print('%s: %s, mod.json, config.json; CMakeLists.txt has its line.' % (rel, a.id + '.cpp'))
    print('Next: cmake --build --preset x86-release   then (MCP) deploy {"mods": ["%s"]}   then call %s.status' % (a.id, a.id))
    print('The way from here: docs/MODDING.md')


if __name__ == '__main__':
    main()
