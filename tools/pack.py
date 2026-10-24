"""pack.py: a player's archive: the loader, the mods it names and what they need, laid out as the game's folder.

    python tools/pack.py dlss_nr      # build/dist/Mafia-DLSS5-NR-RTX50.zip (and the folder beside it)
    python tools/pack.py dlss_nr --since <an older archive of it>
                                      # also <name>-update.zip: only what changed since that archive (a player
                                      # unpacks it over the old version), with UPDATE.txt saying what and what to delete
    python tools/pack.py dlss_nr --intro <a video> [--intro-note "<who made it>"]
                                      # a mod of files alone in the pack, upscaled_intro: the video as the game's
                                      # intro (files\intro.<its extension>, played by the loader's FFmpeg), stored
    ... --since <old> --include mml/ffmpeg
                                      # the update carries everything under that path, changed or not (a player whose
                                      # install lacks it); several --include may be given
    ... --since <old> --split 19      # the update also as 7-Zip volumes of 19 MB each, <name>-update.7z.001, .002...
                                      # (a chat's upload limit; 7-Zip from Program Files)

From the builds' outputs (cmake --build --preset x86-release and x64-release: they must be current) and the pinned
third-party files (tools/deps.py): Ultimate ASI Loader as dinput8.dll, MafiaModLoader.asi, our dxwrapper build
(d3d8.dll, dxwrapper.dll, dxwrapper.ini: third_party/dxwrapper/patch.py), the mods (their 64-bit halves and the
sidecar when one has a half; switched on in the archive's manifests: a graphics mod must be on when the game starts),
the pack's own files, FFmpeg (the loader's video player: mml/ffmpeg, its BUILD.txt and licences), its README
(tools/pack/<pack>.txt) and every license. No PDBs (players send logs and dumps).
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lab import loose_json   # (a manifest as the loader reads it: // comments allowed)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT86 = os.path.join(ROOT, 'build', 'x86', 'out')
OUT64 = os.path.join(ROOT, 'build', 'x64', 'out')
DEPS = os.path.join(ROOT, 'third_party', '_deps')
DIST = os.path.join(ROOT, 'build', 'dist')

PACKS = {
    'dlss_nr': {
        'name': 'Mafia-DLSS5-NR-RTX50',
        'mods': ['core', 'dlss_nr', 'fps_unlock'],
        # NVIDIA's DLL (tools/deps.py ngx): beside the mod's mod.json, where the half looks for it
        'files': {'mml/mods/dlss_nr/nvngx_dlssnr.dll': os.path.join(DEPS, 'ngx', 'bin', 'nvngx_dlssnr.dll'),
                  'mml/mods/dlss_nr/nvngx_dlss.dll': os.path.join(DEPS, 'ngx', 'bin', 'nvngx_dlss.dll'),
                  'mml/mods/dlss_nr/nvngx_dlssg.dll': os.path.join(DEPS, 'ngx', 'bin', 'nvngx_dlssg.dll'),
                  'licenses/NVIDIA-RTX-SDKs-LICENSE.txt': os.path.join(DEPS, 'ngx', 'bin', 'LICENSE.txt')},
        'inside': ['NVIDIA nvngx_dlssnr.dll, nvngx_dlss.dll and nvngx_dlssg.dll 310.9.16.0 (NVIDIA RTX SDKs license).'],
    },
}


def need(path):
    if not os.path.exists(path):
        sys.exit('pack: %s is missing (build both presets; python tools/deps.py ...)' % os.path.relpath(path, ROOT))
    return path


def put(dest, rel, src):
    to = os.path.join(dest, rel.replace('/', os.sep))
    os.makedirs(os.path.dirname(to), exist_ok=True)
    shutil.copy2(need(src), to)


def manifest_on(src, dst):
    """The build's manifest with "enabled": true (its text otherwise as it is)."""
    text = open(need(src), encoding='utf-8').read()
    if re.search(r'"enabled"\s*:\s*false', text):
        text = re.sub(r'"enabled"\s*:\s*false', '"enabled": true', text, count=1)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'w', encoding='utf-8', newline='').write(text)


def file_version(path):
    """A PE file's version (its version resource's VS_FIXEDFILEINFO: signature 0xFEEF04BD), or ''."""
    data = open(path, 'rb').read()
    i = data.rfind(b'\xbd\x04\xef\xfe')   # (the resources come last)
    if i < 0:
        return ''
    ms, ls = int.from_bytes(data[i + 8:i + 12], 'little'), int.from_bytes(data[i + 12:i + 16], 'little')
    return '%d.%d.%d.%d' % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def options(name):
    """Every value given after `name` on the command line."""
    return [sys.argv[i + 1] for i, a in enumerate(sys.argv[:-1]) if a == name]


def add_intro(dest, video, note):
    """A mod of files alone, upscaled_intro: the video as the game's intro (both builds ask for "intro")."""
    ext = os.path.splitext(video)[1].lower()
    if ext not in ('.mp4', '.mkv', '.webm', '.mov'):
        sys.exit('pack: --intro takes an .mp4, .mkv, .webm or .mov (the extensions the loader tries before the game\'s own)')
    put(dest, 'mml/mods/upscaled_intro/files/intro' + ext, video)
    manifest = ('{\n'
                '  // A mod of files alone: the game\'s intro replaced, played by the loader with FFmpeg (mml\\ffmpeg).\n'
                '  "id": "upscaled_intro",\n'
                '  "name": "Upscaled intro%s",\n'
                '  "version": "1.0.0",\n'
                '  "type": "files",\n'
                '  "enabled": true,\n'
                '  "builds": ["1.2", "1.0"],\n'
                '  "load_order": 100\n'
                '}\n') % ((' (' + note + ')') if note else '')
    path = os.path.join(dest, 'mml', 'mods', 'upscaled_intro', 'mod.json')
    open(path, 'w', encoding='utf-8', newline='\r\n').write(manifest)
    return 'mml\\mods\\upscaled_intro\\files\\intro%s: the intro%s.' % (ext, (', ' + note) if note else '')


STORED = ('.mp4', '.mkv', '.webm', '.mov')   # (compressed already: kept as they are in the archives)


def zip_write(z, path, rel):
    z.write(path, rel, zipfile.ZIP_STORED if path.lower().endswith(STORED) else zipfile.ZIP_DEFLATED)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in PACKS:
        sys.exit(__doc__ + '\npacks: ' + ', '.join(PACKS))
    pack = dict(PACKS[sys.argv[1]])
    dest = os.path.join(DIST, pack['name'])
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    # the loader, Ultimate ASI Loader, dxwrapper
    put(dest, 'dinput8.dll', os.path.join(DEPS, 'ual', 'dinput8.dll'))
    put(dest, 'MafiaModLoader.asi', os.path.join(OUT86, 'MafiaModLoader.asi'))
    for f in ('d3d8.dll', 'dxwrapper.dll', 'dxwrapper.ini'):
        put(dest, f, os.path.join(OUT86, 'dxwrapper', f))
    # the mods: the 32-bit half, its 64-bit half and the sidecar when it has one
    sidecar = False
    for mod in pack['mods']:
        src = need(os.path.join(OUT86, 'mml', 'mods', mod))
        for name in os.listdir(src):
            if name.lower().endswith(('.pdb', '.ilk', '.exp', '.lib', '.mmltmp')) or os.path.isdir(os.path.join(src, name)):
                continue
            if name == 'mod.json':
                manifest_on(os.path.join(src, name), os.path.join(dest, 'mml', 'mods', mod, name))
            else:
                put(dest, 'mml/mods/%s/%s' % (mod, name), os.path.join(src, name))
        files_dir = os.path.join(src, 'files')   # (a mod's own game files)
        if os.path.isdir(files_dir):
            shutil.copytree(files_dir, os.path.join(dest, 'mml', 'mods', mod, 'files'))
        if 'entry64' in loose_json(open(os.path.join(src, 'mod.json'), encoding='utf-8').read()):
            half = need(os.path.join(OUT64, 'mml', 'mods', mod, 'x64'))
            for name in os.listdir(half):
                if not name.lower().endswith(('.pdb', '.ilk', '.exp', '.lib')):
                    put(dest, 'mml/mods/%s/x64/%s' % (mod, name), os.path.join(half, name))
            sidecar = True
    if sidecar:
        put(dest, 'mml/sidecar/mml-sidecar64.exe', os.path.join(OUT64, 'mml', 'sidecar', 'mml-sidecar64.exe'))
    for rel, src in pack['files'].items():
        put(dest, rel, src)
    intro = options('--intro')
    if intro:
        note = (options('--intro-note') or [''])[0]
        pack['inside'] = pack['inside'] + [add_intro(dest, intro[0], note)]
    # FFmpeg, the loader's video player: its DLLs, how it was built and from what (the LGPL's terms), its licences
    ff = need(os.path.join(OUT86, 'mml', 'ffmpeg'))
    ff_version = open(os.path.join(ff, 'BUILD.txt'), encoding='utf-8').readline().split(' (')[0]
    for name in os.listdir(ff):
        if name.lower().endswith(('.dll', '.txt')):
            put(dest, 'mml/ffmpeg/' + name, os.path.join(ff, name))
    for name in os.listdir(os.path.join(ff, 'licenses')):
        put(dest, 'licenses/' + name, os.path.join(ff, 'licenses', name))
    # the licenses
    put(dest, 'licenses/MafiaModLoader-LICENSE.txt', os.path.join(ROOT, 'LICENSE'))
    put(dest, 'licenses/UltimateASILoader-LICENSE.txt', os.path.join(DEPS, 'ual', 'license'))
    put(dest, 'licenses/dxwrapper-License.txt', os.path.join(OUT86, 'dxwrapper', 'License.txt'))
    # the README, with what is inside and its versions
    inside = ['MafiaModLoader with its %s mod%s (MIT),' % (', '.join(m for m in pack['mods'] if m != 'core'),
                                                       's' if len(pack['mods']) > 2 else ''),
              'Ultimate ASI Loader v9.7.4 (dinput8.dll, MIT),',
              'dxwrapper %s built with MafiaModLoader\'s patch: its hooks safe on' % file_version(os.path.join(dest, 'dxwrapper.dll')),
              'threads without TLS (d3d8.dll, dxwrapper.dll; zlib),',
              '%s with dav1d, the loader\'s video player (mml\\ffmpeg; LGPL 2.1 or later and' % ff_version,
              'BSD 2-Clause: its sources and how it was built in mml\\ffmpeg\\BUILD.txt),'] + pack['inside']
    readme = open(os.path.join(ROOT, 'tools', 'pack', sys.argv[1] + '.txt'), encoding='utf-8').read()
    open(os.path.join(dest, 'README.txt'), 'w', encoding='utf-8', newline='\r\n').write(readme.replace('{inside}', '\n'.join(inside)))
    # the archive
    archive = dest + '.zip'
    if os.path.exists(archive):
        os.remove(archive)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for folder, _, files in os.walk(dest):
            for f in sorted(files):
                path = os.path.join(folder, f)
                zip_write(z, path, os.path.relpath(path, dest))
    print('pack: %s (%.1f MB)' % (os.path.relpath(archive, ROOT), os.path.getsize(archive) / 1e6))
    if '--since' in sys.argv:
        update(dest, options('--since')[0], [p.replace('\\', '/').strip('/') + '/' for p in options('--include')])


def update(dest, old_archive, include=()):
    """<dest>-update.zip: the files of the new pack that the old archive lacks or holds otherwise (and every file under
    the paths of `include`), and UPDATE.txt."""
    with zipfile.ZipFile(need(old_archive)) as z:
        old = {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}
    new = {}
    for folder, _, files in os.walk(dest):
        for f in files:
            path = os.path.join(folder, f)
            new[os.path.relpath(path, dest).replace(os.sep, '/')] = path
    changed = sorted(rel for rel, path in new.items()
                     if any(rel.startswith(p) for p in include) or old.get(rel) != open(path, 'rb').read())
    gone = sorted(rel for rel in old if rel not in new)
    note = ['Update for %s' % os.path.basename(dest), '',
            'Unpack into the game\'s folder over the previous version (replace the files).', '',
            'Changed or new:'] + ['  ' + rel for rel in changed]
    if gone:
        note += ['', 'Delete (no longer part of it):'] + ['  ' + rel for rel in gone]
    archive = dest + '-update.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in changed:
            zip_write(z, new[rel], rel)
        z.writestr('UPDATE.txt', '\r\n'.join(note) + '\r\n')
    print('update: %s (%.1f MB, %d files changed, %d to delete)' % (os.path.relpath(archive, ROOT),
                                                                     os.path.getsize(archive) / 1e6, len(changed), len(gone)))
    if '--split' in sys.argv:
        split(dest, changed, new, note, float(options('--split')[0]))


def split(dest, changed, new, note, mb):
    """The update as 7-Zip volumes of `mb` MB each (LZMA2 at its best): <dest>-update.7z.001, .002..."""
    seven = next((p for p in (shutil.which('7z'), os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), '7-Zip', '7z.exe'))
                  if p and os.path.exists(p)), None)
    if not seven:
        sys.exit('pack: --split needs 7-Zip (7z.exe on PATH or in Program Files)')
    stage = dest + '-update'
    shutil.rmtree(stage, ignore_errors=True)
    for rel in changed:
        put(stage, rel, new[rel])
    note = note[:3] + ['All the parts (.7z.001, .7z.002...) in one folder: open the .001 with 7-Zip or WinRAR.', ''] + note[3:]
    open(os.path.join(stage, 'UPDATE.txt'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(note) + '\n')
    base = dest + '-update.7z'
    for old in glob.glob(base + '.*'):
        os.remove(old)
    subprocess.run([seven, 'a', '-t7z', '-mx=9', '-v%db' % int(mb * 1e6), base, '.'], cwd=stage, check=True,
                   stdout=subprocess.DEVNULL)
    parts = sorted(glob.glob(base + '.*'))
    print('split: %s' % ', '.join('%s (%.1f MB)' % (os.path.basename(p), os.path.getsize(p) / 1e6) for p in parts))


if __name__ == '__main__':
    main()
