"""Only explicit mod state belongs in .mml files. Statistics consumers own persistence policy."""
import json
from pathlib import Path
import struct
from uuid import uuid4


def test_save_files_require_mod_state(g):
    folder = Path(g.tool('game_status')['game_dir'])
    name = 'state_owner_' + uuid4().hex[:10]
    state = g.call('stats.state')
    g.check('diagnostic stats read leaves collection off', not state['collecting'], state)
    g.tool('restore', save=name)
    point = folder / 'mml' / 'restore' / (name + '.mml')
    g.check('save without mod state has no mml', not point.exists(), str(point))
    # Model a save from the former automatic statistics writer. The normal load path imports it.
    data = json.dumps({'shots': 7}).encode()
    legacy = b'MMLS' + struct.pack('<IIH', 1, 1, 3) + b'mml' + struct.pack('<H', 5) + b'stats' + struct.pack('<I', len(data)) + data
    with point.open('xb') as output:
        output.write(legacy)
    g.tool('restore', load=name)
    clean = name + '_clean'
    g.tool('restore', save=clean)
    g.check('legacy host counters are not propagated to new saves',
            not (point.parent / (clean + '.mml')).exists())

    mods = g.call('mods.list')
    if not any(m['id'] == 'kit' and m['loaded'] for m in mods):
        g.call('mod.load', id='kit')
    g.call('kit.stats')
    g.check('a requesting mod activates in-memory collection', g.call('stats.state')['collecting'])
    g.sleep(250)
    active = name + '_active'
    g.tool('restore', save=active)
    g.check('collection alone creates no save file', not (point.parent / (active + '.mml')).exists())
    g.call('mod.unload', id='kit')
    g.check('last consumer unload stops collection', not g.call('stats.state')['collecting'])

    g.call('save.put', key='ownership_test', value={'value': 17})
    explicit = name + '_mod'
    g.tool('restore', save=explicit)
    g.check('explicit mod state creates a companion file', (point.parent / (explicit + '.mml')).exists())
    g.tool('restore', load=explicit)
    record = g.call('save.get', key='ownership_test')
    g.check('explicit mod state survives a native reload', '17' in str(record), record)
    for saved in (explicit, active, clean, name):
        g.tool('restore', delete=saved)
