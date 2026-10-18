#!/usr/bin/env python3
"""Mafia (2002) .MNU menu layout files  <->  editable JSON.

Reversed from Game.exe 1.2 (loader 0x005EA370, control base ctor 0x005E9BE0).

File layout (little-endian):
  header, 16 bytes : char[4] "Menu" | u32 version (1) | u32 count | u32 0
  count records, 32 bytes each:
    +0x00 u32    id      - control id; buttons send it to the screen's
                           click handler (main menu: 0x005E06E0)
    +0x04 char4  type    - widget class tag, stored reversed ("dniw" = wind)
    +0x08 f32    x, y    - position (window: on screen, child: inside parent)
    +0x10 f32    w, h    - size
    +0x18 u32    text    - textdb id of the label (0 = none)
    +0x1C u32    flags   - low nibble = font, 0x80 = centred, 0x40/0x80000000
                           = text alignment bits, 0x100 = highlighted colour,
                           0x00FF0000 always set in shipped files (keep it)

Nesting: a container (wind, winc, wins, comp, ...) becomes the parent of every
record after it. A record of type "upup" closes the scope and returns to the
top level. The game ignores the header magic and only uses `count`.

usage:
  mnu.py dump  file.mnu [file.json] [--textdb textdb_cz.def]
  mnu.py build file.json file.mnu
  mnu.py check file.mnu            (dump + build must give the same bytes)
  mnu.py text  textdb_cz.def ID ["new text"]     read / set one string
"""
import json, struct, sys

HDR = struct.Struct('<4sIII')
REC = struct.Struct('<I4sffffII')

KNOWN_IDS = {   # main menu click handler 0x005E06E0 (switch on id - 0xF1B95)
    0xF1B95: 'intro', 0xF1B96: 'training', 0xF1B97: 'new game',
    0xF1B98: 'load game', 0xF1B99: 'free ride', 0xF1B9A: 'free ride extreme',
    0xF1B9B: 'racing (cut, not in file)', 0xF1B9C: 'multiplayer (cut, not in file)',
    0xF1B9D: 'carcyclopedia', 0xF1B9E: 'options', 0xF1B9F: 'credits',
    0xF1BA0: 'change profile', 0xF1BA1: 'exit',
}


def read_textdb(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    out = {}
    for i in range(n):
        tid, off = struct.unpack_from('<II', d, 8 + 8 * i)
        out[tid] = d[off:d.index(b'\0', off)].decode('cp1250', 'replace')
    return out


def write_textdb(path, texts, head2=0):
    ids = sorted(texts)
    table = 8 + 8 * len(ids)
    blob, offs = bytearray(), []
    for t in ids:
        offs.append(table + len(blob))
        blob += texts[t].encode('cp1250') + b'\0'
    out = struct.pack('<II', len(ids), head2)
    out += b''.join(struct.pack('<II', t, o) for t, o in zip(ids, offs))
    open(path, 'wb').write(out + bytes(blob))


def read_mnu(path):
    d = open(path, 'rb').read()
    magic, ver, count, zero = HDR.unpack_from(d, 0)
    if len(d) < HDR.size + count * REC.size:
        raise ValueError('%s: file is shorter than its %d records' % (path, count))
    ctrls = []
    for i in range(count):
        cid, tag, x, y, w, h, text, flags = REC.unpack_from(d, HDR.size + i * REC.size)
        ctrls.append({'id': cid, 'type': tag[::-1].decode('latin1'),
                      'x': x, 'y': y, 'w': w, 'h': h, 'text': text, 'flags': flags})
    tail = d[HDR.size + count * REC.size:]
    return {'magic': magic.decode('latin1'), 'version': ver, 'reserved': zero,
            'tail': tail.hex(), 'controls': ctrls}


def build_mnu(m):
    ctrls = m['controls']
    out = HDR.pack(m.get('magic', 'Menu').encode('latin1'), m.get('version', 1),
                   len(ctrls), m.get('reserved', 0))
    for c in ctrls:
        tag = c['type'].encode('latin1')
        if len(tag) != 4:
            raise ValueError('type must be 4 characters: %r' % c['type'])
        out += REC.pack(num(c['id']), tag[::-1], c['x'], c['y'], c['w'], c['h'],
                        num(c.get('text', 0)), num(c.get('flags', 0x00FF00B1)))
    return out + bytes.fromhex(m.get('tail', ''))


def num(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def to_json(m, texts=None):
    """Readable JSON: ids/flags as hex strings, comments as '_' keys."""
    j = dict(m)
    j['controls'] = []
    depth = 0
    for c in m['controls']:
        e = {'id': '0x%X' % c['id'], 'type': c['type'],
             'x': c['x'], 'y': c['y'], 'w': c['w'], 'h': c['h'],
             'text': '0x%X' % c['text'], 'flags': '0x%08X' % c['flags']}
        if texts and c['text'] in texts:
            e['_label'] = texts[c['text']]
        if c['id'] in KNOWN_IDS:
            e['_action'] = KNOWN_IDS[c['id']]
        if c['type'] == 'upup':
            depth = 0
        e['_depth'] = depth
        if c['type'] in ('wind', 'winc', 'wins', 'comp'):
            depth = 1
        j['controls'].append(e)
    return j


def main(a):
    if len(a) >= 2 and a[0] == 'dump':
        texts = None
        if '--textdb' in a:
            i = a.index('--textdb'); texts = read_textdb(a[i + 1]); a = a[:i] + a[i + 2:]
        j = json.dumps(to_json(read_mnu(a[1]), texts), indent=1, ensure_ascii=False)
        if len(a) > 2:
            open(a[2], 'w', encoding='utf-8').write(j + '\n'); print('wrote', a[2])
        else:
            print(j)
    elif len(a) == 3 and a[0] == 'build':
        m = json.load(open(a[1], encoding='utf-8'))
        data = build_mnu(m)
        open(a[2], 'wb').write(data)
        print('wrote %s: %d controls, %d bytes' % (a[2], len(m['controls']), len(data)))
    elif len(a) == 2 and a[0] == 'check':
        src = open(a[1], 'rb').read()
        again = build_mnu(json.loads(json.dumps(to_json(read_mnu(a[1])))))
        print('identical' if src == again else 'DIFFERENT (%d vs %d bytes)' % (len(src), len(again)))
        return 0 if src == again else 1
    elif len(a) in (3, 4) and a[0] == 'text':
        texts = read_textdb(a[1]); tid = num(a[2])
        if len(a) == 3:
            print(repr(texts.get(tid)))
        else:
            head2 = struct.unpack_from('<I', open(a[1], 'rb').read(), 4)[0]
            texts[tid] = a[3]; write_textdb(a[1], texts, head2)
            print('set 0x%X = %r in %s' % (tid, a[3], a[1]))
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
