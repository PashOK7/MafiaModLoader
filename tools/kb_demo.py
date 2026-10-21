"""kb_demo.py: the Mafia demo's own names for LS3D's code, carried onto LS3DF.dll of both builds.

    python tools/kb_demo.py --fetch     # a machine without the demo: it is downloaded (the Internet Archive, its
                                        # SHA-256 pinned) into re/demo/, read with 7-Zip, never installed; then as below
    python tools/kb_demo.py [--demo <demo LS3D.dll>] [--ls3d10 <1.0 LS3DF.dll>] [--ls3d12 <1.2 LS3DF.dll>]
    python tools/ghidra_cli.py sync && python tools/ghidra_cli.py --build 1.0 sync   # the names into the projects

Whoever runs it accepts the demo's own licence (re/demo/license.txt) for their copy; the repository carries nothing
of the demo, its names included (tools/check_demo_names.py, in the offline tests, refuses one that leaked).

The demo (2002-11, one mission of the full game) ships beside its LS3DF.dll (byte for byte the 1.0 lab's) an LS3D.dll
linked 2002-07-04 that still carries the linker's CodeView NB11 block: 9892 public symbols, among them 3109 functions
with their mangled C++ signatures, the globals and the vtables. It is the same engine (3.76 there, 3.84 in 1.0) built
four weeks earlier, and links more beside it (qhull, DirectShow's base classes), so its functions are found in 1.0's
LS3DF by their code, and 1.0's in 1.2's the same way:

  1. the publics of the demo's LS3D.dll (sstGlobalPub's S_PUB32 records), demangled by dbghelp
  2. the functions of each DLL: its exports, its entry, the code reached from them (calls, branches, switch tables),
     the code its relocated pointers name (vtables, callbacks), the demo's publics, 1.2's functions.csv
  3. each function's shape: its instructions with what moves between builds taken out (image addresses, branch
     targets; the fields' offsets too in the loose form), the strings it uses, the functions it calls
  4. pairs, demo -> 1.0 and 1.0 -> 1.2 (and demo -> 1.2 straight, a check of the two steps): the exports by name, the
     SDK's tables (game_build.h kLs3d10 / kLs3d12) and the functions we named alike in both builds (names.csv,
     names_10.csv), shapes found once on each side, the strings used, the calls of a pair aligned, the callers of a
     pair, the slots of paired vtables beside slots paired in the same place, then the link order (both DLLs keep
     their objects' order) between pairs; a pair whose writable data moves against the other pairs' is refuted and
     the pairing made again without it; every pair scored by how alike its two instruction lists are. What the
     linker folded into one function (the same code: /OPT:ICF, which the demo was linked without) is a second name
  5. the data: the image addresses the paired instructions use, voted (a global's symbol, a vtable's)

A pair's confidence: high (an export, the SDK's, ours, the same instructions, the same long strings, 90 % alike, or
found alike both ways: demo -> 1.0 -> 1.2 and demo -> 1.2), medium (70 %, the one function between two pairs, the
straight pair where the two ways differ), low (less). The names files take high and medium; the match file has
every symbol.

Writes (knowledge/ls3df.dll/):
  twins_10.csv           va12,va10,how,score: the 1.0 twin of each 1.2 function (committed: addresses of our own
                         files only)
  demo_10.local.csv      the demo's names at 1.0 addresses (address,kind,name,source,note)
  demo.local.csv         the same at 1.2 addresses, through the twins
  demo_matches.local.csv every demo symbol paired: its 1.0 and 1.2 addresses, how, score, confidence
  demo_conflicts.local.csv  where another name (ours, the SDK's, MafiaReverse's, other projects') says otherwise
The demo's names stay on this machine (*.local.csv, ignored by git): the demo's licence forbids copying any part of
it, and the repository commits no text of the game's files.
"""
import argparse
import csv
import ctypes
import difflib
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
from bisect import bisect_right
from collections import Counter, defaultdict

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KB = os.path.join(ROOT, 'knowledge', 'ls3df.dll')
HEX = re.compile(r'0x[0-9a-f]+')
DIRECT = re.compile(r'0x[0-9a-f]+')   # a branch's own target (not a far one, seg:off)
SOURCE = 'demo LS3D.dll'
BRANCH_TERMINATORS = ('ret', 'retf', 'iret', 'iretd', 'int3', 'hlt', 'ud2')


# ---------------------------------------------------------------- the demo's publics

def demangle(name, flags):
    if not name.startswith('?'):
        # C names: _f (cdecl), _f@8 (stdcall), @f@8 (fastcall)
        m = re.fullmatch(r'[_@](.+?)(@\d+)?', name)
        return m.group(1) if m else name
    try:
        buf = ctypes.create_string_buffer(2048)
        if ctypes.windll.dbghelp.UnDecorateSymbolName(name.encode('latin-1'), buf, 2048, flags):
            return buf.value.decode('latin-1')
    except (AttributeError, OSError):
        pass
    return name


def clean_name(n):
    """A demangled name as a Ghidra symbol: `vftable' -> vftable, {for `B'} -> _for_B, the rest kept."""
    n = re.sub(r"`vftable'\{for `([^']+)'\}", lambda m: 'vftable_for_' + m.group(1).replace('::', '_'), n)
    n = n.replace("`vftable'", 'vftable').replace("`vbtable'", 'vbtable')
    return n.strip()


def publics(path):
    """[(va, section, mangled, signature, name)] of the CodeView NB11 block's global publics."""
    data = open(path, 'rb').read()
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DEBUG']])
    base = -1
    for d in getattr(pe, 'DIRECTORY_ENTRY_DEBUG', []):
        if d.struct.Type == 2 and data[d.struct.PointerToRawData:d.struct.PointerToRawData + 4] == b'NB11':
            base = d.struct.PointerToRawData
    if base < 0:
        sys.exit('kb_demo.py: %s has no CodeView NB11 block' % path)
    d = base + struct.unpack_from('<I', data, base + 4)[0]
    cb_hdr, cb_ent, n_dir = struct.unpack_from('<HHI', data, d)
    secs = pe.sections
    out = []
    for i in range(n_dir):
        sst, _, off, _ = struct.unpack_from('<HHII', data, d + cb_hdr + i * cb_ent)
        if sst != 0x12a:   # sstGlobalPub
            continue
        a = base + off
        cb_sym = struct.unpack_from('<I', data, a + 4)[0]
        p, end = a + 16, a + 16 + cb_sym
        while p + 4 <= end:
            ln, ty = struct.unpack_from('<HH', data, p)
            if ty == 0x1009:   # S_PUB32: type, offset, segment, name
                _, o, seg = struct.unpack_from('<IIH', data, p + 4)
                name = data[p + 15:p + 15 + data[p + 14]].decode('latin-1')
                if 0 < seg <= len(secs):
                    s = secs[seg - 1]
                    out.append((pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress + o, s.Name.rstrip(b'\0').decode(),
                                name, demangle(name, 0), clean_name(demangle(name, 0x1000))))
            p += ln + 2
    return sorted(out)


def worth_naming(mangled, section):
    """Not the compiler's own: string literals, float constants, import thunks, GUIDs, the CRT's section markers."""
    if mangled.startswith(('??_C@', '__real@', '__imp_', '__IMPORT_', '___xc_', '___xi_', '___xp_', '___xt_', '__GUID_')):
        return False
    if section != '.text' and mangled.startswith(('_IID_', '_CLSID_', '_GUID_', '_MEDIA', '_TID_', '_DIID_', '_LIBID_',
                                                   '_FORMAT_', '_PIN_CATEGORY_', '_AM_KS', '_LOOK_')):
        return False
    return True


# ---------------------------------------------------------------- a DLL's functions

class Bin:
    def __init__(self, path, label):
        self.path, self.label = path, label
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'],
                                               pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytes(pe.get_memory_mapped_image())
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.code, self.data, self.writable = [], [], []
        for s in pe.sections:
            lo = self.base + s.VirtualAddress
            span = (lo, lo + max(s.Misc_VirtualSize, s.SizeOfRawData))
            (self.code if s.Characteristics & 0x20000000 else self.data).append(span)
            if s.Characteristics & 0x80000000:
                self.writable.append(span)
        self.exports = {}
        exports = pe.DIRECTORY_ENTRY_EXPORT.symbols if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else []
        for e in exports:
            if e.name:
                self.exports[e.name.decode('latin-1')] = self.base + e.address
        self.ptrs = {}   # va of a relocated dword -> its value
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 3:
                    va = self.base + e.rva
                    self.ptrs[va] = self.dword(va)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.insns = {}   # va -> (size, mnemonic, operands)
        self.tables = set()   # the dwords of switch tables
        self.funcs = []

    def dword(self, va):
        o = va - self.base
        return struct.unpack_from('<I', self.mem, o)[0] if 0 <= o <= len(self.mem) - 4 else 0

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)

    def is_data(self, va):
        return any(lo <= va < hi for lo, hi in self.data)

    def is_writable(self, va):
        return any(lo <= va < hi for lo, hi in self.writable)

    def inside(self, va):
        return self.base <= va < self.base + len(self.mem)

    def string_at(self, va):
        o = va - self.base
        s = self.mem[o:o + 80] if 0 <= o < len(self.mem) else b''
        end = s.find(b'\0')
        if end < 4 or not all(32 <= c < 127 for c in s[:end]):
            return None
        return s[:end].decode()

    def decode(self, va):
        i = self.insns.get(va)
        if i is None:
            o = va - self.base
            for _, size, m, ops in self.md.disasm_lite(self.mem[o:o + 16], va, 1):
                i = (size, m, ops)
            if i is None:
                i = (0, '', '')
            self.insns[va] = i
        return i

    def walk(self, start, found):
        """The code of a function from its entry: its blocks followed, the calls' targets into `found`."""
        blocks, seen = [start], set()
        while blocks:
            va = blocks.pop()
            while self.is_code(va) and va not in seen:
                seen.add(va)
                size, m, ops = self.decode(va)
                if not size:
                    break
                nxt = va + size
                target = int(ops, 16) if DIRECT.fullmatch(ops) else None
                if m == 'call':
                    if target is not None and self.is_code(target):
                        found.add(target)
                elif m == 'jmp':
                    if target is not None:
                        blocks.append(target)
                    else:
                        t = re.fullmatch(r'dword ptr \[(?:e\w\w)\*4 \+ (0x[0-9a-f]+)\]', ops)
                        if t:   # a switch: the relocated dwords of its table
                            tab = int(t.group(1), 16)
                            k = tab
                            while k in self.ptrs and self.is_code(self.ptrs[k]):
                                self.tables.add(k)
                                blocks.append(self.ptrs[k])
                                k += 4
                    break
                elif (m.startswith('j') or m.startswith('loop')) and target is not None:
                    blocks.append(target)
                elif m in BRANCH_TERMINATORS:
                    break
                va = nxt

    def find_functions(self, seeds=()):
        starts = set(v for v in self.exports.values() if self.is_code(v)) | {self.entry} | set(s for s in seeds if self.is_code(s))
        done = set()
        while True:
            todo = starts - done
            if not todo:
                break
            while todo:
                f = todo.pop()
                if f in done:
                    continue
                done.add(f)
                more = set()
                self.walk(f, more)
                todo |= more - done
                starts |= more
            # the code relocated pointers name and no walk reached: vtables' slots, callbacks
            covered = sorted(a for a, i in self.insns.items() if i[0])
            for loc, val in self.ptrs.items():
                if val in starts or not self.is_code(val) or (val in self.insns and self.insns[val][0]):
                    continue
                if self.is_code(loc):
                    if loc in self.tables:
                        continue
                    k = bisect_right(covered, loc) - 1
                    if k < 0 or not (covered[k] < loc < covered[k] + self.insns[covered[k]][0]):
                        continue   # not an instruction's operand
                starts.add(val)
        self.funcs = sorted(starts)
        return self.funcs

    def vtables(self, funcs):
        """{va: [slot functions]}: runs of relocated pointers to functions in the data sections that code names."""
        fs = set(funcs)
        named = set(v for v in self.ptrs.values() if self.is_data(v))
        out = {}
        for loc in sorted(self.ptrs):
            if loc in named and self.is_data(loc) and self.ptrs[loc] in fs:
                slots, k = [], loc
                while k in self.ptrs and self.ptrs[k] in fs and (k == loc or k not in named):
                    slots.append(self.ptrs[k])
                    k += 4
                if len(slots) >= 2:
                    out[loc] = slots
        return out


# ---------------------------------------------------------------- shapes

class Shape:
    __slots__ = ('va', 'exact', 'loose', 'toks', 'calls', 'strings', 'refs', 'n')


def shapes(b):
    out = {}
    fs = b.funcs
    fset = set(fs)
    covered = sorted(a for a, i in b.insns.items() if i[0])
    for k, start in enumerate(fs):
        end = fs[k + 1] if k + 1 < len(fs) else start + 0x2000
        lo, hi = bisect_right(covered, start - 1), bisect_right(covered, end - 1)
        exact, loose, calls, strings, refs = [], [], [], [], []
        for va in covered[lo:hi][:1500]:
            _, m, ops = b.insns[va]
            target = int(ops, 16) if DIRECT.fullmatch(ops) else None
            r = []
            if m == 'call' and target is not None:
                calls.append(target)
                tok = 'call C'
            elif (m.startswith('j') or m.startswith('loop')) and target is not None:
                inside = start <= target < end
                if not inside and target in fset:
                    calls.append(target)   # a tail call
                tok = m + (' L' if inside else ' X')
            else:
                def addr(mm):
                    v = int(mm.group(), 16)
                    if not b.inside(v) or v < b.base + 0x1000:
                        return mm.group()
                    r.append(v)
                    if b.is_data(v):
                        s = b.string_at(v)
                        if s:
                            strings.append(s)
                    return '$'
                tok = m + ' ' + HEX.sub(addr, ops)
            exact.append(tok)
            loose.append(HEX.sub('N', tok))
            refs.append(r)
        s = Shape()
        h = lambda t: hashlib.md5('\n'.join(t).encode()).hexdigest()
        s.va, s.exact, s.loose, s.toks, s.calls, s.strings, s.refs, s.n = start, h(exact), h(loose), loose, calls, tuple(strings), refs, len(exact)
        out[start] = s
    return out


def similarity(a, b, floor=0.0):
    if not a.n or not b.n or min(a.n, b.n) < 0.5 * max(a.n, b.n):
        return 0.0
    if a.exact == b.exact or a.loose == b.loose:
        return 1.0
    sm = difflib.SequenceMatcher(None, a.toks[:800], b.toks[:800], autojunk=False)
    if sm.real_quick_ratio() < floor or sm.quick_ratio() < floor:
        return 0.0
    return sm.ratio()


def resemblance(a, b):
    """A pair's score: its similarity, or where one side is much longer (a function start missed on one side, a
    false one on the other: the ranges end at the next start), the shorter one against as much of the longer's
    beginning (a little less sure). Only scores pairs made: it never makes one."""
    s = similarity(a, b)
    m = min(a.n, b.n)
    if s >= 0.9 or m < 12 or m >= 0.5 * max(a.n, b.n):
        return s
    x, y = (a.toks, b.toks) if a.n <= b.n else (b.toks, a.toks)
    return max(s, 0.95 * difflib.SequenceMatcher(None, x[:800], y[:min(800, int(m * 1.25))], autojunk=False).ratio())


# ---------------------------------------------------------------- pairs

class Pairs:
    """Functions of A paired with functions of B: pair {a: b}, how {a: label}, score {a: 0..1}."""

    def __init__(self, A, B, a_bin, b_bin):
        self.A, self.B, self.a_bin, self.b_bin = A, B, a_bin, b_bin
        self.pair, self.how, self.score, self.taken = {}, {}, {}, {}
        self.callers_a, self.callers_b = defaultdict(list), defaultdict(list)
        for a, s in A.items():
            for c in s.calls:
                self.callers_a[c].append(a)
        for b, s in B.items():
            for c in s.calls:
                self.callers_b[c].append(b)
        self.vt_pairs = {}
        self.banned = set()   # (a, b) the data refuted (check_data)
        self.folded = {}

    def accept(self, a, b, how, score=None):
        if a in self.pair or b in self.taken or a not in self.A or b not in self.B or (a, b) in self.banned:
            return False
        self.pair[a], self.how[a], self.taken[b] = b, how, a
        s = similarity(self.A[a], self.B[b]) if score is None else score
        self.score[a] = s if s >= 0.9 else max(s, resemblance(self.A[a], self.B[b]))
        return True

    def unique(self, key, how):
        ka, kb = defaultdict(list), defaultdict(list)
        for a, s in self.A.items():
            if a not in self.pair:
                k = key(s)
                if k is not None:
                    ka[k].append(a)
        for b, s in self.B.items():
            if b not in self.taken:
                k = key(s)
                if k is not None:
                    kb[k].append(b)
        n = 0
        for k, la in ka.items():
            lb = kb.get(k, [])
            if len(la) == 1 and len(lb) == 1:
                n += self.accept(la[0], lb[0], how)
        return n

    def best_of(self, a, cands, how, floor, margin=0.08):
        scored = sorted(((similarity(self.A[a], self.B[c], floor), c) for c in set(cands)
                         if c not in self.taken and c in self.B), reverse=True)
        if scored and scored[0][0] >= floor and (len(scored) == 1 or scored[0][0] - scored[1][0] >= margin):
            return self.accept(a, scored[0][1], how, scored[0][0])
        return False

    def propagate(self):
        added = 0
        for a, b in list(self.pair.items()):
            ca, cb = self.A[a].calls, self.B[b].calls
            ka = [self.A[c].loose if c in self.A else c for c in ca]
            kb = [self.B[c].loose if c in self.B else c for c in cb]
            for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ka, kb, autojunk=False).get_opcodes():
                if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
                    for x, y in zip(ca[i1:i2], cb[j1:j2]):
                        if x in self.pair or y in self.taken or x not in self.A or y not in self.B:
                            continue
                        s = similarity(self.A[x], self.B[y])
                        if (tag == 'equal' and s >= 0.5) or s >= 0.6:
                            added += self.accept(x, y, 'calls', s)
            for x in self.callers_a.get(a, []):
                if x not in self.pair:
                    added += self.best_of(x, self.callers_b.get(b, []), 'callers', 0.7)
        return added

    def order(self):
        """Between two pairs that keep the link order, what is left on both sides, paired in order where alike."""
        anchors = sorted(self.pair.items())
        # the longest run of pairs increasing on both sides (the others crossed: kept, but no anchors)
        bs = [b for _, b in anchors]
        tails, prev, idx = [], [-1] * len(bs), []
        for i, b in enumerate(bs):
            k = bisect_right([bs[j] for j in idx], b)
            if k == len(idx):
                idx.append(i)
            else:
                idx[k] = i
            prev[i] = idx[k - 1] if k else -1
        chain, i = [], idx[-1] if idx else -1
        while i >= 0:
            chain.append(anchors[i])
            i = prev[i]
        chain.reverse()
        fa, fb = sorted(self.A), sorted(self.B)
        added = 0
        for (a1, b1), (a2, b2) in zip(chain, chain[1:]):
            ga = [a for a in fa[bisect_right(fa, a1):bisect_right(fa, a2 - 1)] if a not in self.pair]
            gb = [b for b in fb[bisect_right(fb, b1):bisect_right(fb, b2 - 1)] if b not in self.taken]
            if not ga or not gb or len(ga) > 60 or len(gb) > 60:
                continue
            if len(ga) == len(gb) == 1:
                # the one function between two pairs on both sides: the same one, rewritten maybe (1.2's
                # I3D_singlemesh::PoseLod: 47 % like 1.0's)
                s = similarity(self.A[ga[0]], self.B[gb[0]])
                if s >= 0.35:
                    added += self.accept(ga[0], gb[0], 'between' if s < 0.6 else 'order', s)
                continue
            if len(ga) == len(gb):
                for x, y in zip(ga, gb):
                    s = similarity(self.A[x], self.B[y])
                    if s >= 0.6:
                        added += self.accept(x, y, 'order', s)
                continue
            for x in ga:
                added += self.best_of(x, gb, 'order', 0.7, 0.1)
        return added

    def vtables(self, vt_a, vt_b):
        """Vtables paired by their paired slots; their other slots paired by place."""
        at_b = defaultdict(list)   # a B function -> (vtable, slot)
        for v, slots in vt_b.items():
            for k, f in enumerate(slots):
                at_b[f].append((v, k))
        added = 0
        used = set(self.vt_pairs.values())
        for va, slots in vt_a.items():
            if va in self.vt_pairs:
                continue
            votes = Counter()
            mapped = 0
            for k, f in enumerate(slots):
                if f in self.pair:
                    mapped += 1
                    for v, kk in at_b.get(self.pair[f], []):
                        if kk == k:
                            votes[v] += 1
            if not votes:
                continue
            (vb, n), *rest = votes.most_common(2) + [(None, 0)]
            if vb in used or n < max(1, (mapped + 1) // 2) or (rest[0][1] == n):
                continue
            self.vt_pairs[va] = vb
            used.add(vb)
            sb = vt_b[vb]
            # a slot by its place only where a paired slot beside it is in the same place (methods are added to
            # interfaces between the builds: the slots after one move)
            same = lambda k: 0 <= k < len(slots) and k < len(sb) and self.pair.get(slots[k]) == sb[k]
            for k, f in enumerate(slots):
                if k < len(sb) and f not in self.pair and sb[k] not in self.taken and (same(k - 1) or same(k + 1)):
                    s = similarity(self.A[f], self.B[sb[k]]) if f in self.A and sb[k] in self.B else 0
                    if s >= 0.4:
                        added += self.accept(f, sb[k], 'vtable', s)
        return added

    def run(self, seeds, vt_a=None, vt_b=None, rounds=4):
        """The pairs; then those the data refutes banned and the pairing made again, until none is."""
        for k in range(rounds):
            if k:
                self.pair, self.how, self.score, self.taken, self.vt_pairs = {}, {}, {}, {}, {}
            self.pass_(seeds, vt_a, vt_b)
            bad = self.check_data()
            print('  %s -> %s: %d pairs, %d refuted by the data' % (self.a_bin.label, self.b_bin.label, len(self.pair),
                                                                   len(bad)), flush=True)
            if not bad:
                break
            self.banned.update(bad)
        for a, _ in bad:   # the last pass's refuted: out
            self.taken.pop(self.pair.pop(a), None)
            self.how.pop(a)
            self.score.pop(a)
        if vt_a is not None:
            self.fold(vt_a, vt_b)
        return self

    def fold(self, vt_a, vt_b):
        """folded {a: b}: functions of A that B's linker folded into another one of the same code (/OPT:ICF: the
        AddRefs, the empty destructors; the demo's LS3D.dll was linked without it): an A function left over whose
        B slot in a paired vtable is a function paired already, or whose instructions are, to the byte but for the
        addresses, those of one B function only, paired already."""
        self.folded = {}
        for va, vb in self.vt_pairs.items():
            sa, sb = vt_a[va], vt_b[vb]
            same = lambda k: 0 <= k < len(sa) and k < len(sb) and self.pair.get(sa[k]) == sb[k]
            for k, f in enumerate(sa):
                if (k < len(sb) and f not in self.pair and f in self.A and sb[k] in self.taken
                        and (same(k - 1) or same(k + 1)) and similarity(self.A[f], self.B[sb[k]]) >= 0.8):
                    self.folded.setdefault(f, sb[k])
        by_b = defaultdict(list)
        for b, s in self.B.items():
            by_b[s.exact].append(b)
        for a, s in self.A.items():
            if a not in self.pair and a not in self.folded and s.n >= 2:
                bs = by_b.get(s.exact, [])
                if len(bs) == 1 and bs[0] in self.taken and self.A[self.taken[bs[0]]].exact == s.exact:
                    self.folded[a] = bs[0]

    def strings(self):
        """Functions using the same strings, found once on each side: a long one (24 characters) is sure by itself
        (LS3D's messages name the method they come from), a short one only by its score."""
        long_ = lambda s: sum(map(len, s.strings))
        return (self.unique(lambda s: s.strings if long_(s) >= 24 else None, 'strings')
                + self.unique(lambda s: s.strings if 8 <= long_(s) < 24 else None, 'short strings'))

    def pass_(self, seeds, vt_a=None, vt_b=None):
        for a, (b, how) in seeds.items():
            self.accept(a, b, how, 1.0)
        self.unique(lambda s: s.exact if s.n >= 6 else None, 'exact')
        self.strings()
        for _ in range(30):
            added = self.propagate()
            added += self.unique(lambda s: s.exact if s.n >= 4 else None, 'exact')
            added += self.unique(lambda s: s.loose if s.n >= 8 else None, 'loose')
            added += self.strings()
            if vt_a is not None:
                added += self.vtables(vt_a, vt_b)
            if not added:
                added += self.order()
            if not added:
                break
        return self

    def refs(self, a):
        """[(a address, b address)] the data addresses a pair's aligned instructions use."""
        sa, sb = self.A[a], self.B[self.pair[a]]
        out = []
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, sa.toks, sb.toks, autojunk=False).get_opcodes():
            if tag != 'equal':
                continue
            for ra, rb in zip(sa.refs[i1:i2], sb.refs[j1:j2]):
                if len(ra) == len(rb):
                    out += [(x, y) for x, y in zip(ra, rb) if self.a_bin.is_data(x) and self.b_bin.is_data(y)]
        return out

    def data(self):
        """{a address: Counter(b address)}: the image addresses that paired instructions use."""
        votes = defaultdict(Counter)
        for a in self.pair:
            if self.score[a] >= 0.7:
                for x, y in self.refs(a):
                    votes[x][y] += 1
        for va, vb in self.vt_pairs.items():
            votes[va][vb] += 3
        return votes

    def check_data(self):
        """Pairs whose writable data says otherwise: an address the other pairs map elsewhere (two of them at least,
        three of four votes), or one moved far from where its page's globals moved (more than 0x200 bytes: within a
        page globals move by a few bytes each between builds). Only the writable data: the read-only's literals
        (floats, strings) are placed by the linker in no order. The pairs refuted, banned for the next pass."""
        own = {a: [(x, y) for x, y in self.refs(a) if self.a_bin.is_writable(x) and self.b_bin.is_writable(y)]
               for a in self.pair}
        at, pages = defaultdict(Counter), defaultdict(Counter)   # a address -> b addresses; 256-byte page -> shifts
        for rs in own.values():
            for x, y in set(rs):
                at[x][y] += 1
                pages[x >> 8][y - x] += 1
        bad = []
        for a, rs in own.items():
            yes = no = 0
            for x, y in set(rs):
                others = at[x].copy()
                others[y] -= 1
                others = +others
                if others:
                    t, n = others.most_common(1)[0]
                    if t == y:
                        yes += 1
                        continue
                    if n >= 2 and n * 4 >= sum(others.values()) * 3 and others[y] == 0:
                        no += 1
                        continue
                c = Counter()
                for pg in ((x >> 8) - 1, x >> 8, (x >> 8) + 1):
                    c.update(pages.get(pg, {}))
                c[y - x] -= 1
                c = +c
                if not c:
                    continue
                d, n = c.most_common(1)[0]
                if n >= 2 and n * 10 >= sum(c.values()) * 6:
                    if abs(y - x - d) > 0x200:
                        no += 1
                    elif abs(y - x - d) <= 0x40:
                        yes += 1
            if no > yes and self.how[a] not in ('export', 'known'):
                bad.append((a, self.pair[a]))
        return bad


def confidence(how, score):
    if how in ('export', 'known', 'exact', 'strings') or score >= 0.9:
        return 'high'
    return 'medium' if score >= 0.7 or how == 'between' else 'low'


def map_symbols(addrs, votes, extent=0x400):
    """{symbol a: (b, votes, confidence)}: each symbol takes the shift most of its extent's votes agree on."""
    out = {}
    addrs = sorted(set(addrs))
    voted = sorted(votes)
    for k, s in enumerate(addrs):
        end = min(addrs[k + 1] if k + 1 < len(addrs) else s + extent, s + extent)
        shifts = Counter()
        for x in voted[bisect_right(voted, s - 1):bisect_right(voted, end - 1)]:
            for y, n in votes[x].items():
                shifts[y - x] += n
        if not shifts:
            continue
        (d, n), *rest = shifts.most_common(2) + [(None, 0)]
        total = sum(shifts.values())
        if n * 4 < total * 3:
            continue
        out[s] = (s + d, n, 'high' if n >= 2 else 'medium')
    return out


# ---------------------------------------------------------------- the repository's other names

def rows(p):
    if not os.path.exists(p):
        return []
    with open(p, encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f))


def ls3d_tables():
    """(1.0 va, 1.2 va, field) of game_build.h's Ls3dBuild tables."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    from check_builds import struct_fields, table
    src = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game_build.h'), encoding='utf-8').read()
    fields = struct_fields(src, 'Ls3dBuild')
    t12, t10 = table(src, 'kLs3d12', fields), table(src, 'kLs3d10', fields)
    out = []
    for k in fields:
        a, b = t10.get(k), t12.get(k)
        if isinstance(a, int) and isinstance(b, int) and a > 0x100 and b > 0x100:
            out.append((0x10000000 + a, 0x10000000 + b, k))
    return out


def our_pairs():
    """(1.0 va, 1.2 va) of the functions we named alike in both builds (names_10.csv, names.csv), each name once
    in each: found in both games by hand (I3D_singlemesh::PoseLod, 47 % like its twin)."""
    def by_name(f):
        out = defaultdict(list)
        for r in rows(os.path.join(KB, f)):
            if r['kind'] == 'function' and r['source'] != 'game_build.h':
                out[r['name']].append(int(r['address'], 16))
        return out
    n10, n12 = by_name('names_10.csv'), by_name('names.csv')
    return [(n10[n][0], n12[n][0]) for n in n10 if len(n10[n]) == 1 and len(n12.get(n, [])) == 1]


OURS = set()   # the sources of names.csv's and names_10.csv's rows (mml, mafia-ragdoll, AntiCulling...)


def other_names(build):
    """{address: [(name, source)]} the repository already gives LS3DF of a build."""
    out = defaultdict(list)
    files = ['names.csv', 'external.csv', 'external.local.csv'] if build == '1.2' else ['names_10.csv']
    for f in files:
        for r in rows(os.path.join(KB, f)):
            src = r['source'] or f
            if f.startswith('names') and src != 'game_build.h':
                OURS.add(src)
            out[int(r['address'], 16)].append((r['name'], src))
    if build == '1.2':
        for r in rows(os.path.join(KB, 'functions.csv')):
            if r['name'] and r['name'] not in ('int', 'void', 'char'):
                out[int(r['address'], 16)].append((r['name'], 'MafiaReverse'))
    for a10, a12, field in ls3d_tables():
        out[a12 if build == '1.2' else a10].append((field, 'game_build.h'))
    return out


def agrees(ours, demo):
    """Whether two names say the same: equal, or one's last part is the other's (case and underscores aside)."""
    norm = lambda n: re.sub(r'[^a-z0-9]', '', n.lower())
    a, b = norm(ours.split('::')[-1]), norm(demo.split('::')[-1])
    return bool(a) and (a == b or a in norm(demo) or b in norm(ours))


# ---------------------------------------------------------------- main

def config_lab(build):
    p = os.path.join(ROOT, 'mml.config.json')
    labs = (json.load(open(p, encoding='utf-8')).get('labs') or {}) if os.path.exists(p) else {}
    return os.path.join(labs[build], 'LS3DF.dll') if build in labs else None


def write_csv(path, header, data):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(header)
        w.writerows(data)


DEMO_URL = 'https://archive.org/download/Mafia_201405/Mafia.exe'
DEMO_SHA = '54cacf5deb5883f9511fee64c5d47a4f853dc5301c1d6ac8b71474dc1d7eafd7'   # 196686681 bytes, a RAR self-extractor
DEMO_FILES = ('LS3D.dll', 'license.txt', 'readme.txt')


def seven_zip():
    for p in (os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), '7-Zip', '7z.exe'),
              os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), '7-Zip', '7z.exe')):
        if os.path.exists(p):
            return p
    return shutil.which('7z')


def fetch_demo(dest):
    """The demo's installer from the Internet Archive into re/demo/ (git ignores it), its SHA-256 checked, and the
    three files read from it (LS3D.dll, its licence, its readme) taken out with 7-Zip (a RAR: 7-Zip reads it; the
    installer itself is never run). On this machine only: nothing of it goes into the repository."""
    os.makedirs(dest, exist_ok=True)
    setup = os.path.join(dest, 'Mafia_demo_setup.rar.exe')
    if not (os.path.exists(setup) and hashlib.sha256(open(setup, 'rb').read()).hexdigest() == DEMO_SHA):
        print('the demo: %s (188 MB)' % DEMO_URL, flush=True)
        part = setup + '.part'
        with urllib.request.urlopen(DEMO_URL) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        got = hashlib.sha256(open(part, 'rb').read()).hexdigest()
        if got != DEMO_SHA:
            os.remove(part)
            sys.exit('the demo: SHA-256 %s, expected %s: not used' % (got, DEMO_SHA))
        os.replace(part, setup)
    z = seven_zip()
    if not z:
        sys.exit('the demo: 7-Zip is needed to read its installer (https://www.7-zip.org; then run this again)')
    subprocess.run([z, 'e', '-y', '-o' + dest, setup] + list(DEMO_FILES), check=True, stdout=subprocess.DEVNULL)
    print('the demo: %s in %s (its licence: license.txt there)' % (', '.join(DEMO_FILES), dest), flush=True)
    return os.path.join(dest, 'LS3D.dll')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--demo', default=os.path.join(ROOT, 're', 'demo', 'LS3D.dll'))
    ap.add_argument('--ls3d10', default=config_lab('1.0'))
    ap.add_argument('--ls3d12', default=config_lab('1.2'))
    ap.add_argument('--out', default=KB, help='where the tables go (knowledge/ls3df.dll)')
    ap.add_argument('--fetch', action='store_true', help="the demo downloaded first (re/demo/, this machine's only)")
    a = ap.parse_args()
    if a.fetch:
        a.demo = fetch_demo(os.path.dirname(a.demo))
    os.makedirs(a.out, exist_ok=True)
    for k in ('demo', 'ls3d10', 'ls3d12'):
        if not getattr(a, k) or not os.path.exists(getattr(a, k)):
            sys.exit('kb_demo.py: --%s: no file (%s)' % (k, getattr(a, k)))

    pubs = publics(a.demo)
    dm, b10, b12 = Bin(a.demo, 'demo'), Bin(a.ls3d10, '1.0'), Bin(a.ls3d12, '1.2')
    code_pubs = {va: p for va, p in ((p[0], p) for p in pubs) if dm.is_code(va)}
    dm.find_functions(code_pubs)
    b10.find_functions()
    b12.find_functions(int(r['address'], 16) for r in rows(os.path.join(KB, 'functions.csv')))
    for b in (dm, b10, b12):
        print('%s: %d functions found' % (b.label, len(b.funcs)), flush=True)
    S = {b.label: shapes(b) for b in (dm, b10, b12)}
    VT = {b.label: b.vtables(b.funcs) for b in (dm, b10, b12)}

    def export_seeds(x, y):
        return {x.exports[n]: (y.exports[n], 'export') for n in x.exports if n in y.exports
                and x.exports[n] in S[x.label] and y.exports[n] in S[y.label]}

    # demo -> 1.0
    p10 = Pairs(S['demo'], S['1.0'], dm, b10).run(export_seeds(dm, b10), VT['demo'], VT['1.0'])
    print('demo -> 1.0: %d pairs %s' % (len(p10.pair), dict(Counter(p10.how.values()))), flush=True)
    # 1.0 -> 1.2 (the twins)
    seeds = export_seeds(b10, b12)
    for a10, a12, _ in ls3d_tables():
        if a10 in S['1.0'] and a12 in S['1.2']:
            seeds.setdefault(a10, (a12, 'known'))
    for a10, a12 in our_pairs():
        if a10 in S['1.0'] and a12 in S['1.2']:
            seeds.setdefault(a10, (a12, 'known'))
    tw = Pairs(S['1.0'], S['1.2'], b10, b12).run(seeds, VT['1.0'], VT['1.2'])
    print('1.0 -> 1.2: %d twins %s' % (len(tw.pair), dict(Counter(tw.how.values()))), flush=True)
    # demo -> 1.2 straight: the check of the two steps
    p12 = Pairs(S['demo'], S['1.2'], dm, b12).run(export_seeds(dm, b12), VT['demo'], VT['1.2'])
    print('demo -> 1.2 (check): %d pairs' % len(p12.pair), flush=True)

    write_csv(os.path.join(a.out, 'twins_10.csv'), ['va12', 'va10', 'how', 'score'],
              sorted(('%08X' % b, '%08X' % x, tw.how[x], '%.2f' % tw.score[x]) for x, b in tw.pair.items()))

    # the data: the demo's symbols placed in 1.0 by the votes, 1.0's in 1.2 the same way
    dsyms = [p[0] for p in pubs if dm.is_data(p[0])]
    d10 = map_symbols(dsyms, p10.data())
    d10_12 = map_symbols([v[0] for v in d10.values()], tw.data())

    names10, names12, matches = {}, {}, []
    folded10, folded12 = defaultdict(list), defaultdict(list)
    level = {'low': 0, 'medium': 1, 'high': 2}
    for va, sec, mangled, sig, name in pubs:
        if not worth_naming(mangled, sec):
            continue
        v10 = v12 = None
        how10 = how12 = check = c10 = c12 = ''
        sc10 = sc12 = 0
        if dm.is_code(va):
            kind = 'function'
            if va in p10.pair:
                v10, how10, sc10 = p10.pair[va], p10.how[va], p10.score[va]
                c10 = confidence(how10, sc10)
                if v10 in tw.pair:
                    v12, how12, sc12 = tw.pair[v10], tw.how[v10], tw.score[v10]
                    c12 = min(c10, confidence(how12, sc12), key=level.get)
            straight = p12.pair.get(va)
            if straight is not None:
                if v12 is None or straight != v12:
                    # no way through 1.0, or another: 1.0's function may be two the linker folded into one (the
                    # same code: two classes' alike getters) that 1.2 keeps apart; the demo's own
                    # function found in 1.2 straight is taken, medium at most
                    check = 'straight' if v12 is None else 'differs: through 1.0 %08X' % v12
                    v12, how12, sc12 = straight, 'straight ' + p12.how[va], p12.score[va]
                    c12 = min(confidence(p12.how[va], sc12), 'medium', key=level.get)
                else:
                    # the three pairings close: demo -> 1.0 -> 1.2 is demo -> 1.2, each made on its own
                    check, c10, c12 = 'agrees', 'high', 'high'
            if v10 is None and v12 is None:
                # folded by the linker into a function paired with another of the demo's: a second name there
                f10 = p10.folded.get(va)
                f12 = p12.folded.get(va) or (tw.pair.get(f10) if f10 else None)
                if f10 or f12:
                    matches.append(('%08X' % va, kind, name, sig, '%08X' % f10 if f10 else '', 'folded' if f10 else '',
                                    '', '%08X' % f12 if f12 else '', 'folded' if f12 else '', '', '', 'folded'))
                    if f10:
                        folded10[f10].append(name)
                    if f12:
                        folded12[f12].append(name)
                    continue
                matches.append(('%08X' % va, kind, name, sig, '', '', '', '', '', '', '', 'unmatched'))
                continue
        else:
            kind = 'label'
            if va not in d10:
                continue
            v10, n10, c10 = d10[va]
            how10, sc10 = 'data %d' % n10, 1.0 if c10 == 'high' else 0.8
            if v10 in d10_12:
                v12, n12, c = d10_12[v10]
                how12, sc12, c12 = 'data %d' % n12, 1.0 if c == 'high' else 0.8, min(c10, c, key=level.get)
        matches.append(('%08X' % va, kind, name, sig, '%08X' % v10 if v10 else '', how10, '%.2f' % sc10 if v10 else '',
                        '%08X' % v12 if v12 else '', how12, '%.2f' % sc12 if v12 else '', check,
                        (c10 or '-') + '/' + (c12 or '-')))
        note = lambda c, how, sc, extra: '%s; %s %.2f; demo %08X%s; %s' % (c, how, sc, va, extra, sig)
        if v10 and level[c10] >= 1:
            names10.setdefault(v10, (kind, name, note(c10, how10, sc10, '')))
        if v12 and level[c12] >= 1:
            names12.setdefault(v12, (kind, name, note(c12, how12, sc12, ' via 1.0 %08X' % v10 if v10 and not how12.startswith('straight') else '')))

    for names, folded in ((names10, folded10), (names12, folded12)):
        for v, more in folded.items():
            if v in names:
                k, n, t = names[v]
                more = [m for m in more if m != n]
                if more:
                    names[v] = (k, n, t + '; the same code, folded by the linker: ' + ', '.join(sorted(set(more))))

    hdr = ['address', 'kind', 'name', 'source', 'note']
    write_csv(os.path.join(a.out, 'demo_10.local.csv'), hdr, [('%08X' % v, k, n, SOURCE, t) for v, (k, n, t) in sorted(names10.items())])
    write_csv(os.path.join(a.out, 'demo.local.csv'), hdr, [('%08X' % v, k, n, SOURCE, t) for v, (k, n, t) in sorted(names12.items())])
    write_csv(os.path.join(a.out, 'demo_matches.local.csv'),
              ['demo', 'kind', 'name', 'signature', 'va10', 'how10', 'score10', 'va12', 'how12', 'score12', 'demo_to_12',
               'confidence'], matches)

    # conflicts: another name at the address that says something else
    # (ours, names.csv and names_10.csv, stay the primary name: tools/ghidra_names.py ranks them over the demo's;
    # the SDK's become aliases beside it; MafiaReverse's and other projects' give way)
    conflicts = set()
    for build, ns in (('1.0', names10), ('1.2', names12)):
        others = other_names(build)
        for v, (k, n, t) in sorted(ns.items()):
            for o, src in others.get(v, []):
                if not agrees(o, n):
                    kept = ('SDK alias beside the demo name' if src == 'game_build.h' else
                            'ours kept' if src in OURS else 'demo name')
                    conflicts.add((build, '%08X' % v, n, o, src, t.split(';')[0], kept))
    write_csv(os.path.join(a.out, 'demo_conflicts.local.csv'),
              ['build', 'address', 'demo', 'other', 'other_source', 'confidence', 'primary'], sorted(conflicts))

    fn = [m for m in matches if m[1] == 'function']
    by = Counter(m[-1] for m in fn)
    print('demo functions: %d; in 1.0 %d, in 1.2 %d; confidence 1.0/1.2: %s' % (
        len(fn), sum(1 for m in fn if m[4]), sum(1 for m in fn if m[7]), dict(sorted(by.items()))))
    print('demo -> 1.2 straight and through 1.0: %s' % dict(Counter(m[10].split(':')[0] for m in fn if m[10])))
    dl = [m for m in matches if m[1] == 'label']
    print('demo data symbols placed: 1.0 %d, 1.2 %d' % (len(dl), sum(1 for m in dl if m[7])))
    print('names written: 1.0 %d, 1.2 %d (high and medium); conflicts with other names: %d (demo_conflicts.local.csv)'
          % (len(names10), len(names12), len(conflicts)))


if __name__ == '__main__':
    main()
