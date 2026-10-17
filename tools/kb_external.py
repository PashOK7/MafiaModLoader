"""kb_external.py: what other projects found in Mafia's code, checked against the game's own files, into knowledge/.

    python tools/kb_external.py --game12 <1.2 Game.exe> --ls3d12 <1.2 LS3DF.dll> --game10 <1.0 Game.exe> [--ext re/ext]

The sources are cloned into re/ext (ignored by git: their code never enters this repository; only facts do, each
with its source): see knowledge/notes/external.md for what each one holds and its terms.

  MafiaOnline (MafiaHub)        1.2 (the Steam Game.exe)   docs/native_contract.md (address, original bytes,
                                                           convention), the SDK's address constants
  rc1-oakwood (MafiaHub)        1.2                        Vendors/mafia-sdk: the FunctionsAddresses enums
  reM (DavoSK)                  1.2                        projects/: hooked and called addresses; idbs/LS3DF.idc
  mafia-re (Marvisak)           1.2                        reccmp annotations (// FUNCTION: LS3DF 0x...)
  mafia-reverse-engineering-    1.0                        ghidra/Game-ghidra.exe.functions.md: names and the
  export (st0rm94)                                         PS2 build's symbol for each function, carried to 1.2
                                                           through the functions' code (twins_10.csv)

Every address is checked in the 1.2 files: a function's must start a function (our function list, from the
decompile), a contract row's original bytes must be there; what does not hold is left out and counted.

Writes:
  knowledge/<module>/external.csv         address,kind,name,source,note: the sources in PUBLISHED (committed)
  knowledge/<module>/external.local.csv   the same for every other source (ignored by git: this machine's)
  knowledge/game.exe/twins_10.csv     va12,va10,how: the 1.0 twin of each 1.2 function that could be paired
  knowledge/game.exe/script_commands.csv   opcode,command,handler,opcode_10,handler_10: the mission scripts'
      commands of both builds, read from the binaries themselves (the keyword table DecodeInstruction builds,
      emulated; the dispatcher's jump table)
"""
import argparse
import csv
import glob
import hashlib
import os
import re
import struct
import sys
from collections import Counter, defaultdict

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KB = os.path.join(ROOT, 'knowledge')
# The sources whose names the repository carries (external.csv): st0rm94 shared his export with everyone and agreed
# to its use. The others' terms do not allow it or say nothing: their names go to external.local.csv, made on the
# machine that has their clones, never committed (the MCP server reads both).
PUBLISHED = {'st0rm94'}
HEX_ADDR = re.compile(r'0x[0-9a-fA-F]{6,8}\b')

# The script machine of each build (checked when used: the code must be what was read).
# 1.2: C_program::DecodeInstruction, C_program::Process (the knowledge base); 1.0: st0rm94's program_decode_instruction,
# program_process_command (opcode-table.md), which are the same code in our 1.0 file.
SCRIPT = {'1.2': (0x463960, 0x46D3B0), '1.0': (0x5B1B60, 0x5BB2D0)}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytes(self.pe.get_memory_mapped_image())
        self.text, self.data = [], []   # (start, end) of the code sections, of the others
        for s in self.pe.sections:
            lo = self.base + s.VirtualAddress
            hi = lo + max(s.Misc_VirtualSize, s.SizeOfRawData)
            (self.text if s.Characteristics & 0x20000000 else self.data).append((lo, hi))

    def at(self, va, n):
        o = va - self.base
        return self.mem[o:o + n] if 0 <= o < len(self.mem) else b''

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.text)

    def is_data(self, va):
        return any(lo <= va < hi for lo, hi in self.data)

    def inside(self, va):
        return self.base <= va < self.base + len(self.mem)


def read_functions(module):
    path = os.path.join(KB, module, 'functions.csv')
    return {int(r['address'], 16): r['name'] for r in csv.DictReader(open(path, encoding='utf-8'))}


# ---- the facts of each source: (address, name, note)
def mafiaonline(ext, img):
    rows, bad = [], Counter()
    contract = os.path.join(ext, 'MafiaOnline', 'docs', 'native_contract.md')
    if os.path.exists(contract):
        for line in open(contract, encoding='utf-8'):
            m = re.match(r'\|\s*`([^`]+)`\s*`(0x[0-9a-fA-F]+)`\s*\|\s*`([0-9A-Fa-f ?]+)`\s*\|\s*([^|]*)\|', line)
            if not m:
                continue
            name, va, want, conv = m.group(1), int(m.group(2), 16), m.group(3).split(), m.group(4)
            got = img.at(va, len(want))
            if len(got) != len(want) or any(w != '??' and int(w, 16) != g for w, g in zip(want, got)):
                bad['contract bytes differ'] += 1
                continue
            first = re.split(r'(?<=[.;])\s', conv.strip())[0].strip().rstrip('.;')
            rows.append((va, name, 'contract (bytes checked): ' + first.replace('`', '')[:160]))
    for p in glob.glob(os.path.join(ext, 'MafiaOnline', 'code', 'sdk', '**', '*.h'), recursive=True):
        rel = os.path.relpath(p, os.path.join(ext, 'MafiaOnline')).replace('\\', '/')
        for m in re.finditer(r'constexpr\s+(?:uintptr_t|uint32_t)\s+k([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]{6,8})\s*;', open(p, encoding='utf-8').read()):
            rows.append((int(m.group(2), 16), m.group(1), rel))
    return rows, bad


def oakwood(ext):
    rows = []
    sdk = os.path.join(ext, 'rc1-oakwood', 'Vendors', 'mafia-sdk', 'MafiaSDK')
    for p in glob.glob(os.path.join(sdk, '**', '*.hpp'), recursive=True):
        src = open(p, encoding='utf-8', errors='replace').read()
        cls = os.path.splitext(os.path.basename(p))[0]
        for e in re.finditer(r'enum\s+FunctionsAddresses[^{]*\{([^}]*)\}', src):
            for m in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]{6,8})', e.group(1)):
                rows.append((int(m.group(2), 16), cls + '::' + m.group(1), 'mafia-sdk ' + os.path.relpath(p, sdk).replace('\\', '/')))
    return rows


def rem(ext):
    rows = []
    proj = os.path.join(ext, 'reM', 'projects')
    for p in glob.glob(os.path.join(proj, '**', '*.cpp'), recursive=True):
        src = open(p, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'ReversibleHooks::Install\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]{6,8})', src):
            rows.append((int(m.group(3), 16), m.group(1) + '::' + m.group(2), 'reM ' + os.path.relpath(p, proj).replace('\\', '/')))
    return rows


def demangle(n):
    """Enough of MSVC's decoration for a name: ??0X@@ ctor, ??1X@@ dtor, ?m@C@@ C::m."""
    m = re.match(r'\?\?([01])([A-Za-z0-9_]+)@@', n)
    if m:
        return m.group(2) + '::' + ('~' if m.group(1) == '1' else '') + m.group(2)
    m = re.match(r'\?([A-Za-z0-9_]+)@([A-Za-z0-9_]+)@@', n)
    if m:
        return m.group(2) + '::' + m.group(1)
    m = re.match(r'\?([A-Za-z0-9_]+)@@', n)
    return m.group(1) if m else n


def ls3d_names(ext):
    rows = []
    idc = os.path.join(ext, 'reM', 'idbs', 'LS3DF.idc')
    if os.path.exists(idc):
        auto = ('nullsub_', 'sub_', 'loc_', 'unk_', 'off_', 'dword_', 'byte_', 'word_', 'flt_', 'dbl_', 'stru_', 'asc_',
                'j_', 'locret_', 'qword_', 'xmmword_', 'algn_', 'unknown_libname', '__')
        for a, n in re.findall(r'MakeName\s*\(\s*0[xX]([0-9A-Fa-f]+)\s*,\s*"([^"]+)"', open(idc, encoding='latin1').read()):
            if n.startswith(auto) or re.match(r'a[A-Z0-9]', n):
                continue
            rows.append((int(a, 16), demangle(n), 'reM LS3DF.idc' + (' (' + n + ')' if n != demangle(n) else '')))
    for p in glob.glob(os.path.join(ext, 'mafia-re', 'ls3df', '**', '*.*'), recursive=True):
        if not p.endswith(('.cpp', '.h', '.c')):
            continue
        lines = open(p, encoding='utf-8', errors='replace').read().split('\n')
        for i, l in enumerate(lines):
            m = re.match(r'//\s*(FUNCTION|GLOBAL):\s*LS3DF\s+(0x[0-9a-fA-F]+)', l)
            if not m:
                continue
            nxt = next((x for x in lines[i + 1:i + 4] if x.strip() and not x.startswith('//')), '')
            name = re.findall(r'([A-Za-z_][A-Za-z0-9_:~]*)\s*\(', nxt) if m.group(1) == 'FUNCTION' else re.findall(r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[|=|;)', nxt)
            if name:
                rows.append((int(m.group(2), 16), name[0], 'mafia-re ' + os.path.relpath(p, os.path.join(ext, 'mafia-re')).replace('\\', '/')))
    return rows


IGRAPH_VTABLE_12 = 0x1009D500   # IGraph's vtable in the 1.2 LS3DF (knowledge/notes/interface.md)


def igraph_methods(ext, img):
    """IGraph's methods by slot from mEdit's LS3D header, put on the 1.2 vtable's functions; checked by two slots
    whose functions we read ourselves (38 the keyboard's read 0x100717F0, 55 the mouse's 0x10071E20)."""
    p = os.path.join(ext, 'mEdit', 'Vendors', 'LS3D', 'Include', 'IGraph', 'IGraph.h')
    if not os.path.exists(p):
        return []
    src = open(p, encoding='utf-8', errors='replace').read()
    body = src[src.index('class IGraph'):]
    body = body[:body.index('};')]
    methods = re.findall(r'virtual\s+[^;(]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', body)
    slot = lambda k: struct.unpack('<I', img.at(IGRAPH_VTABLE_12 + 4 * k, 4))[0]
    if len(methods) < 60 or methods[38] != 'UpdateKeyboardData' or slot(38) != 0x100717F0 or slot(55) != 0x10071E20:
        print('mEdit IGraph.h: the slots do not fit the 1.2 vtable: left out')
        return []
    return [(slot(k), 'IGraph::' + m, 'mEdit IGraph.h: vtable slot %d' % k) for k, m in enumerate(methods)]


def st0rm94_functions(ext):
    out = {}
    p = os.path.join(ext, 'mafia-reverse-engineering-export', 'ghidra', 'Game-ghidra.exe.functions.md')
    for l in open(p, encoding='utf-8'):
        m = re.match(r'\|\s*\d+\s*\|\s*0x([0-9a-f]{8})\s*\|\s*([^|]+?)\s*\|\s*([^|]*?)\s*\|', l)
        if m:
            out[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return out


# ---- 1.0 twins of 1.2 functions, by their code
class Shape:
    """A function's instructions with what moves between builds taken out (image addresses, call and jump targets,
    the fields' offsets in the loose form), the strings it uses, the functions it calls."""
    __slots__ = ('exact', 'loose', 'mnem', 'calls', 'strings', 'n')


def string_at(img, va):
    s = img.at(va, 80)
    end = s.find(b'\0')
    if end < 4 or not all(32 <= c < 127 for c in s[:end]):
        return None
    return s[:end].decode()


def shape(img, md, start, end):
    toks, loose, mnem, calls, strings = [], [], [], [], []
    code = img.at(start, min(end - start, 0x6000))
    for i in md.disasm(code, start):
        if i.address >= end:
            break
        ops = i.op_str
        if i.mnemonic == 'call' and ops.startswith('0x'):
            calls.append(int(ops, 16))
            toks.append('call C')
        elif (i.mnemonic.startswith('j') or i.mnemonic.startswith('loop')) and ops.startswith('0x'):
            toks.append(i.mnemonic + (' L' if start <= int(ops, 16) < end else ' X'))
        else:
            def addr(m):
                v = int(m.group(), 16)
                if not img.inside(v):
                    return m.group()
                s = string_at(img, v) if img.is_data(v) else None
                if s:
                    strings.append(s)
                return '$'
            toks.append(i.mnemonic + ' ' + HEX_ADDR.sub(addr, ops))
        mnem.append(i.mnemonic)
        loose.append(i.mnemonic + ' ' + re.sub(r'0x[0-9a-f]+', 'N', toks[-1].split(' ', 1)[1] if ' ' in toks[-1] else ''))
    h = lambda t: hashlib.md5('\n'.join(t).encode()).hexdigest()
    s = Shape()
    s.exact, s.loose, s.mnem, s.calls, s.strings, s.n = h(toks), h(loose), mnem, calls, tuple(strings), len(toks)
    return s


def similarity(a, b):
    import difflib
    if not a.n or not b.n or min(a.n, b.n) < 0.6 * max(a.n, b.n):
        return 0.0
    return difflib.SequenceMatcher(None, a.mnem[:600], b.mnem[:600], autojunk=False).ratio()


def twins(img12, f12, img10, f10, seeds):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def shapes(img, starts):
        s = sorted(starts)
        return {a: shape(img, md, a, b) for a, b in zip(s, s[1:] + [s[-1] + 0x400])}
    s12, s10 = shapes(img12, f12), shapes(img10, f10)
    callers12, callers10 = defaultdict(list), defaultdict(list)
    for a, s in s12.items():
        for c in s.calls:
            callers12[c].append(a)
    for a, s in s10.items():
        for c in s.calls:
            callers10[c].append(a)
    pair, how = {}, {}   # va10 -> va12
    taken = set()

    def accept(a10, a12, label):
        if a10 in pair or a12 in taken or a10 not in s10 or a12 not in s12:
            return False
        pair[a10], how[a10] = a12, label
        taken.add(a12)
        return True

    def unique_pairs(key, label):
        by12, by10 = defaultdict(list), defaultdict(list)
        for a in s12:
            if a not in taken:
                k = key(s12[a])
                if k is not None:
                    by12[k].append(a)
        for a in s10:
            if a not in pair:
                k = key(s10[a])
                if k is not None:
                    by10[k].append(a)
        n = 0
        for k, l10 in by10.items():
            l12 = by12.get(k, [])
            if len(l10) == 1 and len(l12) == 1:
                n += accept(l10[0], l12[0], label)
        return n

    def best_of(a10, cands, label, floor):
        """The candidate most like a10, when it is clearly the best."""
        scored = sorted(((similarity(s10[a10], s12[c]), c) for c in set(cands) if c not in taken and c in s12), reverse=True)
        if scored and scored[0][0] >= floor and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.08):
            return accept(a10, scored[0][1], label)
        return False

    for a10, a12 in seeds.items():   # pairs checked by hand (game_build.h's tables, the script machine)
        accept(a10, a12, 'known')
    unique_pairs(lambda s: s.exact if s.n >= 6 else None, 'exact')
    unique_pairs(lambda s: s.strings if sum(map(len, s.strings)) >= 8 else None, 'strings')
    for _ in range(20):
        added = 0
        for a10, a12 in list(pair.items()):
            c10, c12 = s10[a10].calls, s12[a12].calls
            # the calls in order: aligned by the callees' loose shapes, the rest by place when the counts agree
            k10 = [s10[c].loose if c in s10 else c for c in c10]
            k12 = [s12[c].loose if c in s12 else c for c in c12]
            import difflib
            for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, k10, k12, autojunk=False).get_opcodes():
                if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
                    for b10, b12 in zip(c10[i1:i2], c12[j1:j2]):
                        if b10 in pair or b12 in taken or b10 not in s10 or b12 not in s12:
                            continue
                        if tag == 'equal' or similarity(s10[b10], s12[b12]) >= 0.6:
                            added += accept(b10, b12, 'calls')
            # callers of a pair: among the callers of its twin
            for b10 in callers10.get(a10, []):
                if b10 not in pair:
                    added += best_of(b10, callers12.get(a12, []), 'callers', 0.75)
        added += unique_pairs(lambda s: s.exact if s.n >= 4 else None, 'exact')
        added += unique_pairs(lambda s: s.loose if s.n >= 10 else None, 'loose')
        added += unique_pairs(lambda s: s.strings if sum(map(len, s.strings)) >= 8 else None, 'strings')
        if not added:
            break
    return {a10: (a12, how[a10]) for a10, a12 in pair.items()}


# ---- the script commands of a build, read from its own code
def script_table(path, build):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
    from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX
    decode, process = SCRIPT[build]
    img = Image(path)
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(img.base, (len(img.mem) + 0xFFFF) & ~0xFFFF)
    mu.mem_write(img.base, img.mem)
    stack, size, text, ret = 0x10000000, 0x200000, 0x20000000, 0x30000000
    if img.base <= stack < img.base + len(img.mem):
        raise SystemExit('the emulator\'s stack overlaps the image')
    for a, n in ((stack, size), (text, 0x1000), (ret, 0x1000)):
        mu.mem_map(a, n)
    mu.mem_write(text, b'ZZZZ_NOT_A_COMMAND\0')
    esp = stack + size - 0x1000 - 12
    mu.mem_write(esp, struct.pack('<III', ret, text, text + 0x100))   # thiscall(this, text, S_instruction*)
    mu.reg_write(UC_X86_REG_ESP, esp)
    mu.reg_write(UC_X86_REG_ECX, text + 0x200)
    mu.emu_start(decode, ret, count=5_000_000)
    mem = bytes(mu.mem_read(stack, size))
    names = {}
    # {char name[0x20]; int opcode}: the names are upper case, digits, '_', '[' ']', and the block's '{' and '}' alone
    # (left out once by a pattern without them: their numbers had no name)
    for m in re.finditer(rb'(?:[A-Z_\[\]][A-Z0-9_\[\]]{1,30}|(?<![^\x00])[{}]) ?\x00', mem):
        o = m.start()
        if o + 0x24 <= len(mem):
            op = struct.unpack_from('<i', mem, o + 0x20)[0]
            name = m.group()[:-1].decode().strip()
            if 0 <= op < 0x400 and name not in names:
                names[name] = op
    # the dispatcher: dec eax; cmp eax, n; ja default; jmp [eax*4 + table]
    seg = img.at(process, 0x200)
    m = re.search(rb'\x48\x3d(....)\x0f\x87(....)\xff\x24\x85(....)', seg, re.S)
    if not m:
        raise SystemExit(build + ': the dispatcher\'s jump table is not where it was read')
    n, table = struct.unpack('<I', m.group(1))[0], struct.unpack('<I', m.group(3))[0]
    at_ja = process + m.start() + 6
    default = at_ja + 6 + struct.unpack('<i', m.group(2))[0]
    arms = {k + 1: struct.unpack('<I', img.at(table + 4 * k, 4))[0] for k in range(n + 1)}
    if len(names) < 300:
        raise SystemExit(build + ': the keyword table was not found (%d names)' % len(names))
    return names, arms, default


def known_pairs(f12, f10):
    """1.0 twins already known: game_build.h's two Game.exe tables (each 1.0 address was found by the code and is
    checked by tools/check_builds.py), and the script machine's functions."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    from check_builds import struct_fields, table
    src = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game_build.h'), encoding='utf-8').read()
    fields = struct_fields(src, 'ExeBuild')
    e12, e10 = table(src, 'kExe12', fields), table(src, 'kExe10', fields)
    out = {}
    for k in fields:
        a, b = e10.get(k), e12.get(k)
        if isinstance(a, int) and isinstance(b, int) and a in f10 and b in f12:
            out[a] = b
    out[SCRIPT['1.0'][0]] = SCRIPT['1.2'][0]
    out[SCRIPT['1.0'][1]] = SCRIPT['1.2'][1]
    return out


def write_csv(path, header, rows):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(header)
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game12', required=True)
    ap.add_argument('--ls3d12', required=True)
    ap.add_argument('--game10', required=True)
    ap.add_argument('--ext', default=os.path.join(ROOT, 're', 'ext'))
    a = ap.parse_args()
    g12, l12, g10 = Image(a.game12), Image(a.ls3d12), Image(a.game10)
    f12 = read_functions('game.exe')
    lf12 = read_functions('ls3df.dll')

    def classify(img, funcs, va):
        if va in funcs:
            return 'function'
        if img.is_code(va):
            return 'site'
        if img.is_data(va):
            return 'data'
        return None

    def check(img, funcs, rows, source, out, bad):
        seen = set()
        for va, name, note in rows:
            kind = classify(img, funcs, va)
            if not kind:
                bad[source + ': not in the image'] += 1
                continue
            if (va, name) in seen:
                continue
            seen.add((va, name))
            out.append(['%08X' % va, kind, name, source, note])

    game_rows, ls3d_rows, bad = [], [], Counter()
    mo, mo_bad = mafiaonline(a.ext, g12)
    bad.update(mo_bad)
    mo_game = [r for r in mo if r[0] < 0x10000000]
    check(g12, f12, mo_game, 'MafiaOnline', game_rows, bad)
    check(g12, f12, [r for r in oakwood(a.ext) if r[0] < 0x10000000], 'Oakwood', game_rows, bad)
    check(l12, lf12, [r for r in oakwood(a.ext) if r[0] >= 0x10000000], 'Oakwood', ls3d_rows, bad)
    check(g12, f12, rem(a.ext), 'reM', game_rows, bad)
    ls = ls3d_names(a.ext)
    check(l12, lf12, [r for r in ls if r[2].startswith('reM')], 'reM', ls3d_rows, bad)
    check(l12, lf12, [r for r in ls if r[2].startswith('mafia-re')], 'mafia-re', ls3d_rows, bad)
    check(l12, lf12, igraph_methods(a.ext, l12), 'mEdit', ls3d_rows, bad)

    # 1.0 names carried to 1.2 through the twins
    st = st0rm94_functions(a.ext)
    tw = twins(g12, list(f12), g10, list(st), known_pairs(f12, st))
    for va10, (va12, how) in sorted(tw.items(), key=lambda kv: kv[1][0]):
        name, ps2 = st[va10]
        note = '1.0 0x%08X %s (%s)' % (va10, name, how) if ps2 else '1.0 0x%08X (%s)' % (va10, how)
        game_rows.append(['%08X' % va12, 'function', ps2 or name, 'st0rm94', note])
    write_csv(os.path.join(KB, 'game.exe', 'twins_10.csv'), ['va12', 'va10', 'how'],
              [['%08X' % va12, '%08X' % va10, how] for va10, (va12, how) in sorted(tw.items(), key=lambda kv: kv[1][0])])

    # the script commands
    n12, arms12, def12 = script_table(a.game12, '1.2')
    n10, arms10, def10 = script_table(a.game10, '1.0')
    rows = []
    for name, op in sorted(n12.items(), key=lambda kv: kv[1]):
        h12 = arms12.get(op)
        op10 = n10.get(name)
        h10 = arms10.get(op10) if op10 is not None else None
        rows.append([op, name, '' if h12 is None else ('default' if h12 == def12 else '%08X' % h12),
                     '' if op10 is None else op10, '' if h10 is None else ('default' if h10 == def10 else '%08X' % h10)])
    write_csv(os.path.join(KB, 'game.exe', 'script_commands.csv'), ['opcode', 'command', 'handler', 'opcode_10', 'handler_10'], rows)

    game_rows.sort(key=lambda r: (int(r[0], 16), r[3]))
    ls3d_rows.sort(key=lambda r: (int(r[0], 16), r[3]))
    # what the repository may carry (PUBLISHED) and what stays on this machine (external.local.csv, ignored by git)
    for module, rows in (('game.exe', game_rows), ('ls3df.dll', ls3d_rows)):
        head = ['address', 'kind', 'name', 'source', 'note']
        write_csv(os.path.join(KB, module, 'external.csv'), head, [r for r in rows if r[3] in PUBLISHED])
        write_csv(os.path.join(KB, module, 'external.local.csv'), head, [r for r in rows if r[3] not in PUBLISHED])
    per = Counter(r[3] for r in game_rows)
    print('game.exe external: %d (%s)' % (len(game_rows), ', '.join('%s %d' % kv for kv in per.most_common())))
    print('ls3df.dll external: %d (%s)' % (len(ls3d_rows), ', '.join('%s %d' % kv for kv in Counter(r[3] for r in ls3d_rows).most_common())))
    print('1.0 twins: %d of %d st0rm94 functions (%s); 1.2 functions: %d' % (len(tw), len(st), dict(Counter(h for _, h in tw.values())), len(f12)))
    print('script commands: 1.2 %d, 1.0 %d, only in 1.2: %s' % (len(n12), len(n10), sorted(set(n12) - set(n10))))
    if bad:
        print('left out:', dict(bad))


if __name__ == '__main__':
    sys.exit(main())
