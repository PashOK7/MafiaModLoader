"""kb_strings.py: the strings each function of the game uses, read from this machine's own game files.

    python tools/kb_strings.py [--game12 <1.2 Game.exe>] [--ls3d12 <1.2 LS3DF.dll>]

The repository carries nothing of the game's own text: the knowledge base's functions (knowledge/<module>/
functions.csv) come without their strings, and this tool reads them from your copy of the game (by default the 1.2
lab of mml.config.json, else its game_dir). Each function's code, from its address to the next function's, is
disassembled; every operand that points into the image's data at a NUL-ended run of printable characters is a string
it uses. The MCP server (kb_search, kb_function) reads the result beside functions.csv.

Writes (ignored by git):
  knowledge/game.exe/strings.local.csv, knowledge/ls3df.dll/strings.local.csv   address,strings
"""
import argparse
import csv
import json
import os
import sys

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KB = os.path.join(ROOT, 'knowledge')
MIN_LEN, MAX_LEN, MAX_FUNC = 3, 240, 0x10000


def config_dir():
    path = os.path.join(ROOT, 'mml.config.json')
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8') as f:
        cfg = json.load(f)
    return (cfg.get('labs') or {}).get('1.2') or cfg.get('game_dir')


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.__data__
        self.code, self.rest = [], []
        for s in self.pe.sections:
            lo = self.base + s.VirtualAddress
            span = (lo, lo + max(s.Misc_VirtualSize, s.SizeOfRawData), s)
            (self.code if s.Characteristics & 0x20000000 else self.rest).append(span)

    def bytes_at(self, va, n):
        for lo, hi, s in self.code + self.rest:
            if lo <= va < hi:
                off = s.PointerToRawData + (va - lo)
                end = min(off + n, s.PointerToRawData + s.SizeOfRawData)
                return self.data[off:end] if off < end else b''
        return b''

    def in_data(self, va):
        return any(lo <= va < hi for lo, hi, _ in self.rest)

    def string_at(self, va):
        if not self.in_data(va):
            return None
        raw = self.bytes_at(va, MAX_LEN + 1)
        end = raw.find(b'\0')
        if end < MIN_LEN:
            return None
        s = raw[:end]
        if not all(32 <= c < 127 or c in (9, 10, 13) or c >= 0xA0 for c in s):
            return None
        if sum(chr(c).isalpha() for c in s) < 2:
            return None
        return s.decode('latin-1')


def function_strings(img, starts):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    out = {}
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else start + 0x400
        code = img.bytes_at(start, min(end - start, MAX_FUNC))
        found = []
        for ins in md.disasm(code, start):
            if ins.id == 0:   # (bytes skipped as data: no operands)
                continue
            for op in ins.operands:
                va = op.imm if op.type == capstone.x86.X86_OP_IMM else op.mem.disp if op.type == capstone.x86.X86_OP_MEM else None
                if va is None:
                    continue
                s = img.string_at(va & 0xFFFFFFFF)
                if s is not None and s not in found:
                    found.append(s)
        if found:
            out[start] = ', '.join(json.dumps(s, ensure_ascii=False) for s in found)
    return out


def run(module, path):
    with open(os.path.join(KB, module, 'functions.csv'), encoding='utf-8') as f:
        starts = sorted(int(r['address'], 16) for r in csv.DictReader(f))
    img = Image(path)
    found = function_strings(img, starts)
    dst = os.path.join(KB, module, 'strings.local.csv')
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['address', 'strings'])
        for va in sorted(found):
            w.writerow(['%08X' % va, found[va]])
    print('%s: %d of %d functions use strings -> %s' % (module, len(found), len(starts), os.path.relpath(dst, ROOT)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game12', help='the 1.2 Game.exe (default: the 1.2 lab in mml.config.json)')
    ap.add_argument('--ls3d12', help='the 1.2 LS3DF.dll (default: beside that Game.exe)')
    a = ap.parse_args()
    game = a.game12
    if not game:
        d = config_dir()
        game = os.path.join(d, 'Game.exe') if d else None
    if not game or not os.path.exists(game):
        sys.exit('no 1.2 Game.exe: give --game12 (the knowledge base is the 1.2 build\'s)')
    ls3d = a.ls3d12 or os.path.join(os.path.dirname(game), 'LS3DF.dll')
    run('game.exe', game)
    if os.path.exists(ls3d):
        run('ls3df.dll', ls3d)
    else:
        print('no LS3DF.dll beside it: give --ls3d12')


if __name__ == '__main__':
    main()
