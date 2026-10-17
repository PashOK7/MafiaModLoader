"""gen_script_commands.py: the mission scripts' commands, from knowledge/game.exe/script_commands.csv into
sdk/include/mml/game/script_commands.h.

    python tools/gen_script_commands.py           write the header
    python tools/gen_script_commands.py --check   exit 1 when the header is not what the table gives

A program's instructions hold the command's number, which is not the same in both builds (1.2 DETECTOR_INRANGE 29,
1.0 28): a mod that reads programs (a debug view, a tool) names them with game::ScriptCommandName(opcode) and finds
a number with game::ScriptOpcode("detector_inrange"), for the build running. The game builds its keyword table on
the stack each time it compiles a line, so it keeps no copy a mod could read: the table is read from the binaries
by tools/kb_external.py. tests/run_tests.py runs the check.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'knowledge', 'game.exe', 'script_commands.csv')
OUT = os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'script_commands.h')

HEAD = '''// The mission scripts' commands: each one's number in a program's instructions, in 1.2 and in 1.0 (-1: not in that
// build). Made by tools/gen_script_commands.py from knowledge/game.exe/script_commands.csv (read from the binaries by
// tools/kb_external.py): do not edit. A program's instruction is {int opcode; void *params} (docs/SCRIPTS.md).
#pragma once
#include <cctype>
#include <cstring>
#include "game_build.h"

namespace game {

struct ScriptCommand { const char *name; short op12, op10; };

inline const ScriptCommand kScriptCommands[] = {
'''

TAIL = '''};

// The command's number in the build running, -1 when it is not there (game::Detect first).
inline int ScriptOpcode(const char *name) {
    const bool v10 = g_exe && strcmp(g_exe->name, "1.0") == 0;
    for (const ScriptCommand &c : kScriptCommands) {
        if (_stricmp(c.name, name) == 0) return v10 ? c.op10 : c.op12;
    }
    return -1;
}

// The name (lower case, as the scripts write it) of a number in the build running, or null.
inline const char *ScriptCommandName(int opcode) {
    const bool v10 = g_exe && strcmp(g_exe->name, "1.0") == 0;
    for (const ScriptCommand &c : kScriptCommands) {
        if ((v10 ? c.op10 : c.op12) == opcode) return c.name;
    }
    return nullptr;
}

} // namespace game
'''


def render():
    rows = []
    for r in csv.DictReader(open(SRC, encoding='utf-8')):
        name = r['command'].strip().lower()
        if not name:
            continue
        op12 = int(r['opcode']) if r['opcode'].strip() else -1
        op10 = int(r['opcode_10']) if r['opcode_10'].strip() else -1
        rows.append('    {"%s", %d, %d},\n' % (name, op12, op10))
    return HEAD + ''.join(rows) + TAIL


def main():
    text = render()
    if '--check' in sys.argv:
        old = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else ''
        if old != text:
            print('sdk/include/mml/game/script_commands.h is not what script_commands.csv gives: run tools/gen_script_commands.py')
            sys.exit(1)
        return
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('sdk/include/mml/game/script_commands.h: %d commands' % text.count('{"'))


if __name__ == '__main__':
    main()
