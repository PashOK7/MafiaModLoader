"""Move the old loader Atmosphere policy to the SDK example, with the game closed.

    python tools/migrate_atmosphere.py --game 1.2          # review, no writes
    python tools/migrate_atmosphere.py --game 1.2 --apply  # hash-guarded JSON migration + backups

Only configured lab folders are accepted. Run after deploying the new atmosphere example. Existing destination
values win, unrelated settings remain, and a completed migration is a no-op. No loader code knows the example id.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path

import lab


def moved(loader, atmosphere):
    """Pure migration, used by offline tests. Inputs are not mutated."""
    loader = json.loads(json.dumps(loader))
    atmosphere = json.loads(json.dumps(atmosphere))
    render = loader.get('render', {})
    legacy = render.get('fog_mode') == 3
    mapping = {'fog_start':'start', 'fog_span':'distance', 'fog_density':'density', 'fog_height':'height',
               'fog_base':'base', 'fog_sun':'sun', 'fog_tint':'tint'}
    changed = False
    for source, dest in mapping.items():
        if legacy and source in render and dest not in atmosphere:
            atmosphere[dest] = render[source]
    if legacy:
        atmosphere.setdefault('on', True)
        render['fog_mode'] = 2  # native linear fallback; owner-scoped atmosphere overrides it while ready
        changed = True
    for source in ['fog_density','fog_height','fog_base','fog_sun','fog_tint']:
        if source in render:
            del render[source]
            changed = True
    return loader, atmosphere, changed, legacy


def digest(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def guarded_write(path, original, replacement):
    """Re-read immediately before replacing; a changed destination is refused, never silently overwritten."""
    current = path.read_bytes() if path.exists() else None
    if digest(current) != digest(original):
        raise RuntimeError('changed during migration: ' + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.atmosphere-migrate.tmp')
    with temporary.open('xb') as f:
        f.write(replacement)
    try:
        current = path.read_bytes() if path.exists() else None
        if digest(current) != digest(original):
            raise RuntimeError('changed before replace: ' + str(path))
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', required=True, choices=['1.2','1.0'])
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    cfg = json.loads(Path(lab.CFG).read_text(encoding='utf-8-sig'))
    if args.game not in cfg.get('labs', {}):
        raise RuntimeError('no configured lab for build ' + args.game)
    game = Path(cfg['labs'][args.game]).resolve()
    if any(lab.same_dir(str(game / 'Game.exe'), p) for p in lab.process_paths()):
        raise RuntimeError('close this lab before migrating: ' + str(game))
    paths = [game/'mml/data/loader/config.json', game/'mml/data/atmosphere/config.json', game/'mml/mods/atmosphere/mod.json']
    original = [p.read_bytes() if p.exists() else None for p in paths]
    if original[2] is None:
        raise RuntimeError('deploy atmosphere before migrating: ' + str(paths[2]))
    values = [json.loads(b.decode('utf-8-sig')) if b else {} for b in original]
    loader, atmosphere, changed, enable = moved(values[0],values[1])
    if not changed:
        print('No legacy atmosphere settings; nothing to migrate.')
        return
    manifest = values[2]
    if enable:
        manifest['enabled'] = True
    planned = [loader, atmosphere, manifest]
    for path, value in zip(paths,planned):
        print(str(path) + '\n' + json.dumps(value,indent=2))
    if not args.apply:
        print('Dry run. Add --apply to write these changes with backups.')
        return
    backup = game/'mml_lab'/('atmosphere-sdk-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    backup.mkdir(parents=True, exist_ok=False)
    # All targets are revalidated before any mutation; exact preimages are backed up first.
    for path, data in zip(paths,original):
        if digest(path.read_bytes() if path.exists() else None) != digest(data):
            raise RuntimeError('changed before backup: ' + str(path))
    for path, data in zip(paths,original):
        if data is not None:
            dest = backup / path.relative_to(game)
            dest.parent.mkdir(parents=True, exist_ok=True)
            with dest.open('xb') as f:
                f.write(data)
    for path, before, value in zip(paths,original,planned):
        guarded_write(path,before,(json.dumps(value,indent=2)+'\n').encode('utf-8'))
    print('Migrated. Backups: ' + str(backup))


if __name__ == '__main__':
    main()
