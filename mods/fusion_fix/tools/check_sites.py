"""check_sites.py: what each byte pattern of the mod (or of Fusion Fix's sources) finds in both builds' files.

    python mods/fusion_fix/tools/check_sites.py [<source folder>] [<1.2 game folder> <1.0 game folder>]

Fusion Fix (ThirteenAG, WidescreenFixesPack source/Mafia.FusionFix, MIT) finds the places it changes by byte
patterns at run time, and so does the mod (ff.cpp, in the modules' files as the game ships them). This lists every
pattern literal (hex bytes and ?) of the sources (the mod's folder by default; Fusion Fix's own, to see what a new
version of it asks), the module it is found in and its matches' RVAs in each build: a pattern a part needs exactly
once and finds otherwise leaves that part off in that build. The game folders default to mml.config.json's labs.
"""
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
MODULES = {'game': ['Game.exe'], 'ls3d': ['LS3DF.dll'], 'rw_data': ['rw_data.dll'], 'ijoy': ['IJoy_orig.dll', 'IJoy.dll']}


def image(path):
    """The file laid out as it is in memory (its sections at their RVAs)."""
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    size = struct.unpack_from('<I', d, pe + 24 + 56)[0]
    hdr = struct.unpack_from('<I', d, pe + 24 + 60)[0]
    img = bytearray(size)
    img[:hdr] = d[:hdr]
    sec = pe + 24 + optsz
    for i in range(nsec):
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, sec + i * 40 + 8)
        n = min(rsize, vsize or rsize)
        img[va:va + n] = d[rptr:rptr + n]
    return bytes(img)


def regex(pat):
    return re.compile(b''.join(b'.' if t == '?' else re.escape(bytes([int(t, 16)])) for t in pat.split()), re.S)


def main():
    src = sys.argv[1] if len(sys.argv) >= 2 else os.path.join(ROOT, 'mods', 'fusion_fix')
    if len(sys.argv) >= 4:
        dirs = {'1.2': sys.argv[2], '1.0': sys.argv[3]}
    else:
        dirs = json.load(open(os.path.join(ROOT, 'mml.config.json'), encoding='utf-8'))['labs']
    images = {}
    for build, d in dirs.items():
        for mod, names in MODULES.items():
            for n in names:
                p = os.path.join(d, n)
                if os.path.exists(p):
                    images[(build, mod)] = image(p)
                    break
    pats = set()
    for fn in sorted(os.listdir(src)):
        if fn.endswith(('.ixx', '.cpp', '.h')):
            text = open(os.path.join(src, fn), encoding='utf-8').read()
            for p in re.findall(r'"((?:[0-9A-Fa-f]{2}|\?)(?: (?:[0-9A-Fa-f]{2}|\?))+)"', text):
                pats.add(' '.join(p.upper().split()))
    found = set()
    for p in sorted(pats):
        rx = regex(p)
        for mod in MODULES:
            hits = {b: [m.start() for m in rx.finditer(images[(b, mod)])] if (b, mod) in images else [] for b in dirs}
            if any(hits.values()):
                found.add(p)
                print('%-7s %s' % (mod, p))
                print('        ' + '  '.join('%s %s' % (b, ', '.join('0x%X' % h for h in hits[b]) or '-') for b in dirs))
    print('%d patterns, %d found' % (len(pats), len(found)))
    for p in sorted(pats - found):
        print('  in neither build: ' + p)


if __name__ == '__main__':
    main()
