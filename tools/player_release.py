"""Package the shared MafiaModLoader player runtime, its base and its optional video files."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = '0.1'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_zip(path, files):
    """Check every source and replace the destination only after verifying the complete archive."""
    path.parent.mkdir(parents=True, exist_ok=True)
    previous = digest(path) if path.exists() else None
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        staged = Path(temp.name)
    try:
        with zipfile.ZipFile(staged, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name, source in sorted(files.items()):
                if isinstance(source, bytes):
                    member = zipfile.ZipInfo(name)  # Stable metadata for generated version text.
                    z.writestr(member, source, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
                else:
                    z.write(source, name, zipfile.ZIP_STORED if source.suffix == '.zip' else zipfile.ZIP_DEFLATED)
        with zipfile.ZipFile(staged) as z:
            for name, source in files.items():
                expected = source if isinstance(source, bytes) else source.read_bytes()
                if z.read(name) != expected:
                    raise RuntimeError(f'Source changed while packaging: {name}')
        if (digest(path) if path.exists() else None) != previous:
            raise RuntimeError(f'Archive changed while packaging: {path}')
        os.replace(staged, path)
    finally:
        staged.unlink(missing_ok=True)
    return path


def base_files(root):
    host = root / 'build/x86/out'
    deps = root / 'third_party'
    return {
        'dinput8.dll': deps / '_deps/ual/dinput8.dll',
        'MafiaModLoader.asi': host / 'MafiaModLoader.asi',
        'mml/mods/core/core.dll': host / 'mml/mods/core/core.dll',
        'mml/mods/core/mod.json': host / 'mml/mods/core/mod.json',
        'licenses/MafiaModLoader.txt': root / 'LICENSE',
        'licenses/UltimateASILoader.txt': deps / '_deps/ual/license',
        'licenses/MinHook.txt': deps / 'minhook/LICENSE.txt',
        'licenses/nlohmann-json.txt': deps / 'json/LICENSE.MIT',
        'licenses/MikkTSpace.h': deps / 'mikktspace/mikktspace.h',
    }


def base_archive(root, expected_content=None):
    files = base_files(root)
    sha = digest(files['MafiaModLoader.asi'])
    hashes = ''.join(f'{digest(p)}  {n}\n' for n, p in sorted(files.items()))
    content = hashlib.sha256(hashes.encode()).hexdigest()
    if expected_content and content != expected_content:
        raise ValueError(f'Tested MML base content {expected_content} differs from build {content}')
    files['mml/base-version.txt'] = f'MafiaModLoader {RELEASE}\r\nMafiaModLoader.asi SHA-256 {sha}\r\n'.encode()
    return root / 'build/dist' / f'MafiaModLoader-{RELEASE}-base.zip', files


def wrapper_files(root):
    folder = root / 'build/x86/out/dxwrapper'
    files = {name: folder / name for name in ('d3d8.dll', 'dxwrapper.dll', 'dxwrapper.ini')}
    files['licenses/dxwrapper.txt'] = folder / 'License.txt'
    return files


def video_files(root):
    folder = root / 'build/x86/out/mml/ffmpeg'
    names = ('BUILD.txt', 'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll',
             'swresample-7.dll', 'swscale-10.dll', 'licenses/FFmpeg-COPYING.LGPLv2.1',
             'licenses/FFmpeg-LICENSE.md', 'licenses/dav1d-COPYING')
    return {'mml/ffmpeg/' + name: folder / name for name in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('variant', choices=('base', 'full', 'video', 'all'), nargs='?', default='all')
    variant = parser.parse_args().variant
    dist = ROOT / 'build/dist'
    if variant in ('base', 'full', 'all'):
        path, base = base_archive(ROOT)
        if variant in ('base', 'all'):
            print(write_zip(path, base))
        if variant in ('full', 'all'):
            full = {**base, **wrapper_files(ROOT), **video_files(ROOT)}
            full['mml/sidecar/mml-sidecar64.exe'] = ROOT / 'build/x64/out/mml/sidecar/mml-sidecar64.exe'
            full['MML-INSTALL.txt'] = ROOT / 'docs/PLAYER_INSTALL.txt'
            print(write_zip(dist / path.name.replace('-base.zip', '-full.zip'), full))
    if variant in ('video', 'all'):
        video = video_files(ROOT)
        # Players with a graphics wrapper of their own can keep it when adding the codecs.
        video.update({'optional-d3d8to9/' + n: p for n, p in wrapper_files(ROOT).items()})
        video['mml/ffmpeg/INSTALL.txt'] = ROOT / 'docs/VIDEO_INSTALL.txt'
        version = video['mml/ffmpeg/BUILD.txt'].read_text(encoding='utf-8').split()[1]
        print(write_zip(dist / f'MafiaModLoader-FFmpeg-{version}-video.zip', video))


if __name__ == '__main__':
    main()
