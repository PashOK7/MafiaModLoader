"""pack.py: a player's archive: the loader, the mods it names and what they need, laid out as the game's folder.

    python tools/pack.py dlss_nr      # build/dist/Mafia-DLSS5-NR-RTX50.zip (and the folder beside it)

From the builds' outputs (cmake --build --preset x86-release and x64-release: they must be current) and the pinned
third-party files (tools/deps.py): Ultimate ASI Loader as dinput8.dll, MafiaModLoader.asi, our dxwrapper build
(d3d8.dll, dxwrapper.dll, dxwrapper.ini: third_party/dxwrapper/patch.py), the mods (their 64-bit halves and the
sidecar when one has a half; switched on in the archive's manifests: a graphics mod must be on when the game starts),
the pack's own files, its README (tools/pack/<pack>.txt) and every license. No PDBs (players send logs and dumps).
"""
import os
import re
import shutil
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
        'mods': ['core', 'dlss_nr'],
        # NVIDIA's DLL (tools/deps.py ngx): beside the mod's mod.json, where the half looks for it
        'files': {'mml/mods/dlss_nr/nvngx_dlssnr.dll': os.path.join(DEPS, 'ngx', 'bin', 'nvngx_dlssnr.dll'),
                  'licenses/NVIDIA-RTX-SDKs-LICENSE.txt': os.path.join(DEPS, 'ngx', 'bin', 'LICENSE.txt')},
        'inside': ['NVIDIA nvngx_dlssnr.dll 310.9.16.0 (NVIDIA RTX SDKs license).'],
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


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in PACKS:
        sys.exit(__doc__ + '\npacks: ' + ', '.join(PACKS))
    pack = PACKS[sys.argv[1]]
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
    # the licenses
    put(dest, 'licenses/MafiaModLoader-LICENSE.txt', os.path.join(ROOT, 'LICENSE'))
    put(dest, 'licenses/UltimateASILoader-LICENSE.txt', os.path.join(DEPS, 'ual', 'license'))
    put(dest, 'licenses/dxwrapper-License.txt', os.path.join(OUT86, 'dxwrapper', 'License.txt'))
    # the README, with what is inside and its versions
    inside = ['MafiaModLoader with its %s mod%s (MIT),' % (', '.join(m for m in pack['mods'] if m != 'core'),
                                                       's' if len(pack['mods']) > 2 else ''),
              'Ultimate ASI Loader v9.7.4 (dinput8.dll, MIT),',
              'dxwrapper %s built with MafiaModLoader\'s patch: its hooks safe on' % file_version(os.path.join(dest, 'dxwrapper.dll')),
              'threads without TLS (d3d8.dll, dxwrapper.dll; zlib),'] + pack['inside']
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
                z.write(path, os.path.relpath(path, dest))
    print('pack: %s (%.1f MB)' % (os.path.relpath(archive, ROOT), os.path.getsize(archive) / 1e6))


if __name__ == '__main__':
    main()
