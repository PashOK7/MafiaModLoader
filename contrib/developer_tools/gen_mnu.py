#!/usr/bin/env python3
"""Builds devtools.mnu, gamemenu.mnu, textdb_cz.def and devtools_en.def.

Usage: python gen_mnu.py <game>/tables/menu/gamemenu.mnu <game>/tables/textdb_cz.def
  (the game's ORIGINAL files: they are read, never written; the outputs land next to this script.
   gamemenu.mnu and textdb_cz.def are the game's files plus our item/texts, so they are not in the repository.)


Control ids must match developer_tools.cpp:
  row i of kRows      -> control 0xDF100 + i   (chck for MR_BOOL/MR_INT, slid for MR_FLOAT/MR_FOV)
  edit X / Y / Z      -> 0xDF200 / 0xDF201 / 0xDF202
  button Teleport     -> 0xDF210, button "Moje pozice" -> 0xDF211, button Zpet -> 0xDF2FF
Texts: 0xDF0000 title, 0xDF0100 + i row labels, 0xDF0200+ the rest.
"""
import json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
if len(sys.argv) != 3:
    sys.exit(__doc__)
ORIG_GAMEMENU, BASE_TEXTDB = sys.argv[1], sys.argv[2]
sys.path.insert(0, HERE)   # mnu.py
import mnu

# ini key -> label (the order is taken from kRows in developer_tools.cpp)
LABELS = {
    'ShowOnStart':         'Triggery zapnuté',
    'ShowCollision':       'Kolize (statické)',
    'CollisionSeeThrough': 'Kolize přes zdi (vidět všechny)',
    'CollisionDepthTest':  'Kolize jen viditelné (ne přes zdi)',
    'ShowInrange':         'Koule inrange',
    'ShowActorsDist':      'Getactorsdist',
    'ShowVolumes':         'Objemy detektorů',
    'ShowLabels':          'Popisky skriptů',
    'ShowName':            'Názvy skriptů',
    'ShowLines':           'Čáry mezi postavami',
    'OnlyActive':          'Jen aktivní trigger',
    'MaxDistance':         'Vzdálenost vykreslování (10-500 m)',
    'FillAlpha':           'Výplň koulí',
    'LineWidth':           'Tloušťka čar',
    'TextScale':           'Velikost textu',
    'CollisionDistance':   'Vzdálenost kolizí (5-200 m)',
    'CollisionLineWidth':  'Tloušťka kolizí',
    'CollisionFillAlpha':  'Výplň kolizí',
    'CollisionPrimitives': 'Tvary z kolizí (box, koule, válec)',
    'HideHud':             'Skrýt HUD (zůstane jen menu)',
    'Wireframe':           'Wireframe (drátěný model)',
    'Freecam':             'Free kamera (myš + WSAD)',
    'ShowCheck':           'Check.bin (cesty postav)',
    'ShowRoad':            'Road.bin (silnice, křižovatky)',
    'Flying':              'Létání',
    'GodMode':             'God mode (nesmrtelnost)',
    'InfiniteAmmo':        'Nekonečné náboje',
    'ShowDebugText':       'Debug text (debug_text ze skriptů)',
    'CameraFov':           'FOV kamery (vlevo = hra)',
    'Hud':                 'Stavový řádek',
    'ShowAllObjects':      'Názvy všech objektů',
    'AllObjectsDistance':  'Vzdálenost názvů objektů (5-200 m)',
    'CollisionLabels':     'Popisky kolizí (materiály)',
    'CollisionCullBack':   'Skrýt odvrácené stěny kolizí',
    'LanguageEN':          'Jazyk: English (po znovuotevření menu)',
}
LABELS_EN = {
    'ShowOnStart':         'Triggers on',
    'ShowAllObjects':      'Names of all objects',
    'ShowCollision':       'Collision (static)',
    'HideHud':             'Hide HUD (menus stay)',
    'Wireframe':           'Wireframe',
    'Freecam':             'Free camera (mouse + WSAD)',
    'ShowCheck':           'Check.bin (pedestrian paths)',
    'ShowRoad':            'Road.bin (roads, crossings)',
    'CollisionSeeThrough': 'Collision through walls (all)',
    'CollisionLabels':     'Collision labels (materials)',
    'Flying':              'Flying',
    'GodMode':             'God mode',
    'InfiniteAmmo':        'Infinite ammo',
    'ShowDebugText':       'Debug text (debug_text from scripts)',
    'LanguageEN':          'Language: English (reopen the menu)',
    'CameraFov':           'Camera FOV (left = game)',
}
EN_FIXED = {
    0xDF0000: 'Developer Tools - settings', 0xDF0200: 'Teleport', 0xDF0201: 'My position', 0xDF0202: 'Back',
    0xDF0203: 'Teleport (coordinates X  Y  Z):', 0xDF0204: '0', 0xDF0205: 'Display:', 0xDF0206: 'Values:',
    0xDF0207: 'X', 0xDF0208: 'Y', 0xDF0209: 'Z', 0xDF0220: 'Developer Tools',
}


def rows_from_source():
    src = open(os.path.join(HERE, 'developer_tools.cpp'), encoding='latin-1').read()
    body = src[src.index('static const MRow kRows[] = {'):]
    body = body[:body.index('};')]
    rows = []
    for m in re.finditer(r'\{\s*"[^"]*"(?:\s*"[^"]*")*\s*,\s*(MR_\w+)\s*,[^}]*?(?:"(\w+)"|NULL)\s*,\s*\d+\s*\}', body):
        rows.append((m.group(1), m.group(2)))
    return rows


def main():
    rows = rows_from_source()
    assert rows and rows[-1][0] == 'MR_CLOSE', rows
    texts = mnu.read_textdb(BASE_TEXTDB)
    head2 = __import__('struct').unpack_from('<I', open(BASE_TEXTDB, 'rb').read(), 4)[0]
    T = {}
    E = dict(EN_FIXED)
    T[0xDF0000] = 'Developer Tools - nastavení'
    T[0xDF0200] = 'Teleport'
    T[0xDF0201] = 'Moje pozice'
    T[0xDF0202] = 'Zpět'
    T[0xDF0203] = 'Teleport (souřadnice X  Y  Z):'
    T[0xDF0204] = '0'
    T[0xDF0205] = 'Zobrazení:'
    T[0xDF0206] = 'Hodnoty:'
    T[0xDF0207] = 'X'
    T[0xDF0208] = 'Y'
    T[0xDF0209] = 'Z'

    C = [{'id': 1, 'type': 'wind', 'x': 20, 'y': 12, 'w': 760, 'h': 576, 'text': 0xDF0000, 'flags': 0x00FF0032}]
    C.append({'id': 0xDF300, 'type': 'stxt', 'x': 20, 'y': 12, 'w': 20, 'h': 22, 'text': 0xDF0205, 'flags': 0x001F0031})
    C.append({'id': 0xDF301, 'type': 'stxt', 'x': 390, 'y': 12, 'w': 20, 'h': 22, 'text': 0xDF0206, 'flags': 0x001F0031})
    yl, yr = 40, 40
    for i, (kind, key) in enumerate(rows):
        if kind == 'MR_CLOSE':
            continue
        T[0xDF0100 + i] = LABELS[key]
        E[0xDF0100 + i] = LABELS_EN[key]
        cid = 0xDF100 + i
        if kind in ('MR_BOOL', 'MR_INT'):
            C.append({'id': cid, 'type': 'chck', 'x': 20, 'y': yl, 'w': 340, 'h': 20, 'text': 0xDF0100 + i, 'flags': 0x00FF0030})
            yl += 23
        else:
            C.append({'id': 0xDF400 + i, 'type': 'stxt', 'x': 390, 'y': yr, 'w': 20, 'h': 18, 'text': 0xDF0100 + i, 'flags': 0x00FF0030})
            C.append({'id': cid, 'type': 'slid', 'x': 400, 'y': yr + 20, 'w': 300, 'h': 16, 'text': 0, 'flags': 0x00FF0030})
            yr += 44
    y = max(yl, yr) + 8
    C.append({'id': 0xDF302, 'type': 'line', 'x': 10, 'y': y, 'w': 740, 'h': 0, 'text': 0, 'flags': 0x00FF0030})
    y += 8
    C.append({'id': 0xDF303, 'type': 'stxt', 'x': 20, 'y': y, 'w': 20, 'h': 22, 'text': 0xDF0203, 'flags': 0x001F0031})
    y += 28
    for k in range(3):
        x = 20 + k * 170
        C.append({'id': 0xDF310 + k, 'type': 'stxt', 'x': x, 'y': y + 3, 'w': 20, 'h': 18, 'text': 0xDF0207 + k, 'flags': 0x00FF0030})
        C.append({'id': 0xDF200 + k, 'type': 'edit', 'x': x + 22, 'y': y, 'w': 130, 'h': 24, 'text': 0xDF0204, 'flags': 0x00FF0030})
    C.append({'id': 0xDF210, 'type': 'butt', 'x': 540, 'y': y - 1, 'w': 100, 'h': 25, 'text': 0xDF0200, 'flags': 0x001F00B1})
    C.append({'id': 0xDF211, 'type': 'butt', 'x': 640, 'y': y - 1, 'w': 110, 'h': 25, 'text': 0xDF0201, 'flags': 0x001F00B1})
    y += 34
    assert y + 25 <= 576, y
    C.append({'id': 0xDF2FF, 'type': 'butt', 'x': 640, 'y': 576 - 34, 'w': 110, 'h': 25, 'text': 0xDF0202, 'flags': 0x001F0071})

    m = {'magic': 'Menu', 'version': 1, 'reserved': 0, 'tail': '', 'controls': C}
    data = mnu.build_mnu(json.loads(json.dumps(m)))
    open(os.path.join(HERE, 'devtools.mnu'), 'wb').write(data)
    # ESC game menu: original gamemenu.mnu + item 0x75 "Developer Tools" under "Nahrat hru"
    g = mnu.to_json(mnu.read_mnu(ORIG_GAMEMENU))
    G = []
    for c in g['controls']:
        if c['type'] == 'wind': c['h'] = float(c['h']) + 30
        elif float(c['y']) >= 70: c['y'] = float(c['y']) + 30
        G.append(c)
        if c['id'] in ('0x67', 0x67):
            G.append({'id': 0x75, 'type': 'item', 'x': 0.0, 'y': 70.0, 'w': 217.0, 'h': 30.0, 'text': 0xDF0220, 'flags': 0x0F0000B1, '_depth': 1})
    g['controls'] = G
    open(os.path.join(HERE, 'gamemenu.mnu'), 'wb').write(mnu.build_mnu(json.loads(json.dumps(g))))
    T[0xDF0220] = 'Developer Tools'
    texts.update(T)
    mnu.write_textdb(os.path.join(HERE, 'textdb_cz.def'), texts, head2)
    mnu.write_textdb(os.path.join(HERE, 'devtools_en.def'), E, 0)   # same format as textdb_*.def, only our texts
    print('devtools.mnu: %d controls, %d bytes; %d rows; textdb +%d strings' % (len(C), len(data), len(rows), len(T)))


if __name__ == '__main__':
    main()
