"""kb_import.py: the game knowledge base (knowledge/) rebuilt from a MafiaReverse folder.

    python tools/kb_import.py                       # reverse_dir from mml.config.json
    python tools/kb_import.py <the reverse folder>      (or reverse_dir in mml.config.json)

MafiaReverse holds full Hex-Rays decompiles of the 1.2 Game.exe and LS3DF.dll with generated indexes. The
repository takes the facts only: every function's address, name, class, signature and topics, and the call graph.
Never the decompiled code (the MCP server reads it at run time from the reverse folder, `kb_function`, which stays
on this machine), nor the game's own text: the strings column stays empty, tools/kb_strings.py fills
strings.local.csv from this machine's game.

Output (CSV, UTF-8):
  knowledge/<module>/functions.csv  address,name,class,signature,topics,strings (empty),src_start,src_end
  knowledge/<module>/calls.csv      caller,callee
where <module> is game.exe or ls3df.dll, and src_start/src_end are the function's lines in the decompile
(Game.c / LS3DF_dll.c).

knowledge/<module>/names.csv (address,kind,name,source,note) holds our own names: those found in the running game
and in other projects (mafia-ragdoll's Ghidra project, AntiCulling). It is edited by hand and never regenerated,
except its rows with source `game_build.h`: those are rebuilt here from the 1.2 tables of
sdk/include/mml/game/game_build.h (kind `site` for an address inside a function: a patch site, a return address).
"""
import csv
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'knowledge')
SETS = {'game.exe': 'mafia_gamec_docs', 'ls3df.dll': 'mafia_ls3df_docs'}
csv.field_size_limit(1 << 24)


def reverse_dir():
    if len(sys.argv) > 1:
        return sys.argv[1]
    cfg = os.path.join(ROOT, 'mml.config.json')
    d = json.load(open(cfg)).get('reverse_dir') if os.path.exists(cfg) else None
    if not d:
        sys.exit('give the MafiaReverse folder, or set reverse_dir in mml.config.json')
    return d


def rows(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        yield from csv.DictReader(f)


def name_of(r):
    """The function's name: the index's, else the one in its signature (`bool __thiscall C_actor::Init(...)`)."""
    n = r['name']
    if not n:
        head = r['signature'].split('(', 1)[0].split()
        n = head[-1].lstrip('*&') if head else ''
    return '' if not n or n.startswith(('sub_', 'nullsub_', 'j_', 'unknown_libname')) else n


def write(path, header, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(header)
        w.writerows(data)


def build_table_names():
    """{module: [(address, kind, name)]} from game_build.h's 1.2 tables (kExe12: virtual addresses; kLs3d12: RVAs)."""
    raw = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game_build.h'), encoding='utf-8').read()
    exe_body = re.search(r'struct ExeBuild \{(.*?)\};', raw, re.S).group(1)
    sites = set()
    sec = re.search(r'game_fixes patches[^\n]*\n(.*?)\n\s*//', exe_body, re.S)
    if sec:
        sites.update(re.findall(r'[A-Za-z_]\w*', re.sub(r'\buintptr_t\b', '', sec.group(1))))
    src = re.sub(r'//[^\n]*', '', raw)
    out = {}
    for module, struct, tab, base in (('game.exe', 'ExeBuild', 'kExe12', 0), ('ls3df.dll', 'Ls3dBuild', 'kLs3d12', 0x10000000)):
        body = re.search(r'struct %s \{(.*?)\};' % struct, src, re.S).group(1)
        fields = [(m.group(1).strip(), n.strip()) for m in re.finditer(r'\b(const char\s*\*|uintptr_t|int)\s*([^;]+);', body)
                  for n in m.group(2).split(',')]
        init = re.search(r'%s = \{(.*?)\};' % tab, src, re.S).group(1)
        vals = [v.strip() for v in init.split(',') if v.strip()]
        rows = []
        for (typ, name), v in zip(fields, vals):
            if typ == 'uintptr_t' and int(v, 16):
                site = name in sites or re.search(r'Return\d*$', name)
                rows.append(('%08X' % (base + int(v, 16)), 'site' if site else 'function', name))
        out[module] = rows
    return out


def merge_names(module, rows, funcs):
    """names.csv with the game_build.h rows replaced; a function entry that is not one becomes `label`."""
    path = os.path.join(OUT, module, 'names.csv')
    kept = [r for r in rows_list(path) if r[3] != 'game_build.h'] if os.path.exists(path) else []
    entries = {f[0] for f in funcs}
    have = {(r[0], r[2]) for r in kept}
    for a, kind, name in rows:
        if (a, name) in have:
            continue
        if kind == 'function' and a not in entries:
            kind = 'label'
        kept.append([a, kind, name, 'game_build.h', ''])
    kept.sort(key=lambda r: (int(r[0], 16), r[2]))
    write(path, ['address', 'kind', 'name', 'source', 'note'], kept)
    return len(kept)


def rows_list(path):
    with open(path, newline='', encoding='utf-8') as f:
        r = csv.reader(f)
        next(r, None)
        return [row for row in r if row]


def main():
    src = reverse_dir()
    table_names = build_table_names()
    for module, docs in SETS.items():
        idx = os.path.join(src, docs, 'indexes')
        funcs = []
        for r in rows(os.path.join(idx, 'functions.csv')):
            name = name_of(r)
            funcs.append([r['address'].upper(), name, r['class'], ' '.join(r['signature'].split()), r['topics'], '',
                          r['start_line'], r['end_line']])
        funcs.sort(key=lambda f: int(f[0], 16))
        write(os.path.join(OUT, module, 'functions.csv'),
              ['address', 'name', 'class', 'signature', 'topics', 'strings', 'src_start', 'src_end'], funcs)
        calls = sorted({(r['caller_address'].upper(), r['callee_address'].upper()) for r in rows(os.path.join(idx, 'calls.csv'))})
        write(os.path.join(OUT, module, 'calls.csv'), ['caller', 'callee'], calls)
        strings = []   # (the game's own text is not committed: tools/kb_strings.py reads it from this machine's game)
        named = sum(1 for f in funcs if f[1])
        ours = merge_names(module, table_names[module], funcs)
        print('%-9s %5d functions (%d named), %6d calls, %5d string sites, %d names of ours'
              % (module, len(funcs), named, len(calls), len(strings), ours))


if __name__ == '__main__':
    main()
