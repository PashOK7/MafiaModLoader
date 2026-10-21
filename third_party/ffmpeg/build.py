"""build.py: FFmpeg and dav1d built for x86 (the loader's video player, host/video.cpp), from the sources tools/deps.py
ffmpeg unpacks into third_party/_deps/ffmpeg/. The x86 preset runs it (CMakeLists.txt, an external project); it does
nothing when its output was made from the same sources and settings (out/.built holds their hash).

    python third_party/ffmpeg/build.py [--force]

What it needs on the machine (none of it ships): Visual Studio's C++ x86 tools (found by vswhere), MSYS2 in C:\\msys64
(or MSYS2_ROOT) with make, diffutils and pkgconf (`pacman -S make diffutils pkgconf`), NASM and Ninja on PATH, Meson
(`pip install meson`). About ten minutes on eight cores.

What it makes, in third_party/_deps/ffmpeg_build/out/: bin/ (avutil, swresample, swscale, avcodec, avformat: DLLs and
PDBs; the C runtime linked statically, /MT), lib/ (their import libraries), include/, licenses/ and BUILD.txt (the
versions, where the sources come from and the configure line: what the LGPL asks a distributor to tell).

The licence: FFmpeg is configured without --enable-gpl and --enable-nonfree, so the build is LGPL 2.1 or later; dav1d
(BSD 2-Clause) is linked statically into avcodec. Only what a video needs is built: the decoders of the codecs games
and modders use (H.264, HEVC, VP8, VP9, AV1 through dav1d, MPEG-1/2/4, MJPEG, Theora, ProRes, Bink 1 of every
revision FFmpeg reads, Smacker, Indeo 3/4/5, Cinepak, the old Windows codecs, WMV; AAC, MP3, Vorbis, Opus, FLAC, AC-3,
E-AC-3, Bink audio, WMA, PCM and ADPCM), the demuxers of their containers (MP4/MOV, Matroska/WebM, AVI, Bink,
Smacker, Ogg, MPEG-PS/TS, FLV, IVF, ASF, WAV, MP3, AAC), their parsers, files only (no network), and swscale and
swresample to turn the frames into 32-bit pixels and the sound into 16-bit stereo. The GPU's decoders through
Direct3D 11 (D3D11VA) and DXVA2 for H.264, HEVC, VP9, AV1 (FFmpeg's own AV1 decoder, which decodes only on a GPU:
dav1d decodes in software), MPEG-2 and VC-1/WMV3 (Windows' own d3d11.dll and dxva2.dll, loaded by FFmpeg at run time).
"""
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'third_party', '_deps', 'ffmpeg')
WORK = os.path.join(ROOT, 'third_party', '_deps', 'ffmpeg_build')
OUT = os.path.join(WORK, 'out')
MSYS = os.environ.get('MSYS2_ROOT', r'C:\msys64')

DECODERS = [
    'h264', 'hevc', 'vp8', 'vp9', 'libdav1d', 'mpeg1video', 'mpeg2video', 'mpeg4', 'msmpeg4v1', 'msmpeg4v2',
    'msmpeg4v3', 'mjpeg', 'theora', 'prores', 'dnxhd', 'huffyuv', 'ffvhuff', 'utvideo', 'ffv1', 'rawvideo',
    'bink', 'binkaudio_dct', 'binkaudio_rdft', 'smacker', 'smackaud', 'indeo3', 'indeo4', 'indeo5', 'cinepak',
    'msvideo1', 'msrle', 'wmv1', 'wmv2', 'wmv3', 'vc1', 'av1',
    'aac', 'mp3float', 'mp2float', 'vorbis', 'opus', 'flac', 'ac3', 'eac3', 'alac', 'wmav1', 'wmav2',
    'pcm_s16le', 'pcm_s16be', 'pcm_s24le', 'pcm_s32le', 'pcm_f32le', 'pcm_u8', 'pcm_alaw', 'pcm_mulaw',
    'adpcm_ms', 'adpcm_ima_wav', 'adpcm_ima_qt']
DEMUXERS = ['mov', 'matroska', 'avi', 'bink', 'smacker', 'ogg', 'mpegps', 'mpegts', 'mpegvideo', 'flv', 'ivf', 'asf',
            'wav', 'mp3', 'aac', 'h264', 'hevc']
PARSERS = ['h264', 'hevc', 'vp8', 'vp9', 'av1', 'mpeg4video', 'mpegvideo', 'mpegaudio', 'aac', 'ac3', 'vorbis', 'opus',
           'flac', 'mjpeg', 'vc1']

HWACCELS = [c + '_' + api for c in ('h264', 'hevc', 'vp9', 'av1', 'mpeg2', 'vc1', 'wmv3')
            for api in ('d3d11va', 'd3d11va2', 'dxva2')]

FFMPEG_FLAGS = [
    '--toolchain=msvc', '--arch=x86', '--target-os=win32', '--enable-shared', '--disable-static',
    '--disable-programs', '--disable-doc', '--disable-network', '--disable-autodetect', '--enable-w32threads',
    '--disable-avdevice', '--disable-avfilter', '--disable-everything', '--enable-swscale', '--enable-swresample',
    '--enable-libdav1d', '--enable-protocol=file', '--extra-cflags=-MT', '--extra-ldflags=-DEBUG',
    '--enable-debug', '--disable-stripping', '--enable-d3d11va', '--enable-dxva2',
] + ['--enable-decoder=' + d for d in DECODERS] + ['--enable-demuxer=' + d for d in DEMUXERS] \
  + ['--enable-parser=' + p for p in PARSERS] + ['--enable-hwaccel=' + h for h in HWACCELS]
DAV1D_FLAGS = ['--buildtype=release', '--default-library=static', '-Denable_tools=false', '-Denable_tests=false',
               '-Denable_examples=false', '-Db_vscrt=mt', '-Db_ndebug=true']


def msvc_env():
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    vs = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                         'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                        capture_output=True, text=True, check=True).stdout.strip()
    if not vs:
        sys.exit('build.py: no Visual Studio with the C++ x86 tools (vswhere)')
    bat = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat')
    out = subprocess.run('call "%s" x86 >nul && set' % bat, shell=True, capture_output=True, text=True, check=True).stdout
    env = {}
    for line in out.splitlines():
        k, sep, v = line.partition('=')
        if sep:
            env[k.upper()] = v
    return env


def msys_path(p):
    p = os.path.abspath(p).replace('\\', '/')
    return '/' + p[0].lower() + p[2:] if p[1] == ':' else p


def stamp():
    h = hashlib.sha256()
    for name in ('ffmpeg', 'dav1d'):
        h.update(open(os.path.join(SRC, name, 'VERSION' if name == 'ffmpeg' else 'meson.build'), 'rb').read())
    h.update(open(os.path.join(SRC, '.pinned'), 'rb').read())
    h.update(' '.join(FFMPEG_FLAGS + DAV1D_FLAGS).encode())
    return h.hexdigest()


def write_notes():
    """out/BUILD.txt: what was built, from which sources, how (what the LGPL asks a distributor to tell)."""
    pins = open(os.path.join(ROOT, 'tools', 'deps.py'), encoding='utf-8').read()
    urls = [u for u in (w.strip("[(',") for w in pins.split()) if u.startswith('https://') and u.endswith('.tar.xz')
            and ('/ffmpeg-' in u or '/dav1d-' in u)]
    version = open(os.path.join(SRC, 'ffmpeg', 'VERSION')).read().strip()
    with open(os.path.join(OUT, 'BUILD.txt'), 'w', encoding='utf-8') as f:
        f.write('FFmpeg %s (LGPL 2.1 or later) with dav1d (BSD 2-Clause), built for x86 by MafiaModLoader\n' % version)
        f.write('(third_party/ffmpeg/build.py, https://github.com/PashOK7/MafiaModLoader). Their licences: licenses/.\n\n')
        f.write('Sources, unmodified (their SHA-256 in tools/deps.py):\n')
        for u in urls:
            f.write('  %s\n' % u)
        f.write('\nFFmpeg configure:\n  %s\n' % ' '.join(FFMPEG_FLAGS))
        f.write('\ndav1d meson setup:\n  %s\n' % ' '.join(DAV1D_FLAGS))


def run(cmd, env, cwd=None):
    print('build.py:', cmd if isinstance(cmd, str) else ' '.join(cmd), flush=True)
    subprocess.run(cmd, env=env, cwd=cwd, check=True)


def main():
    force = '--force' in sys.argv
    want = stamp()
    mark = os.path.join(OUT, '.built')
    if not force and os.path.exists(mark) and open(mark).read().strip() == want:
        write_notes()
        print('build.py: FFmpeg present (%s)' % OUT)
        return
    for tool in ('nasm', 'ninja'):
        if not shutil.which(tool):
            sys.exit('build.py: %s is not on PATH' % tool)
    for tool in ('make.exe', 'pkgconf.exe', 'diff.exe', 'bash.exe'):
        if not os.path.exists(os.path.join(MSYS, 'usr', 'bin', tool)):
            sys.exit('build.py: MSYS2 has no %s (%s; pacman -S make diffutils pkgconf)' % (tool, MSYS))
    env = msvc_env()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(OUT, exist_ok=True)
    jobs = str(os.cpu_count() or 4)

    # dav1d: Meson with MSVC (the compiler decides the CPU: x86), a static library into out/
    dav1d_build = os.path.join(WORK, 'dav1d')
    run([sys.executable, '-m', 'mesonbuild.mesonmain', 'setup', dav1d_build, os.path.join(SRC, 'dav1d'),
         '--prefix=' + OUT, '--libdir=lib'] + DAV1D_FLAGS, env)
    run([sys.executable, '-m', 'mesonbuild.mesonmain', 'install', '-C', dav1d_build], env)
    lib = os.path.join(OUT, 'lib')
    if os.path.exists(os.path.join(lib, 'libdav1d.a')):   # Meson's name for MSVC's static library; FFmpeg links dav1d.lib
        os.replace(os.path.join(lib, 'libdav1d.a'), os.path.join(lib, 'dav1d.lib'))

    # FFmpeg: its configure and make in MSYS2's shell. On PATH: MSVC's own folders first (MSYS2 has a link.exe of its
    # own), then MSYS2's tools, then the rest (another MSYS runtime's awk there, Git's, loses the backslashes of the
    # commands make hands it: the dependency files' awk failed so)
    ff_build = os.path.join(WORK, 'ffmpeg')
    os.makedirs(ff_build, exist_ok=True)
    sh = dict(env)
    base = [p.lower().rstrip('\\') for p in os.environ.get('PATH', '').split(os.pathsep)]
    parts = env['PATH'].split(os.pathsep)
    vc = [p for p in parts if p.lower().rstrip('\\') not in base]
    rest = [p for p in parts if p.lower().rstrip('\\') in base]
    sh['PATH'] = os.pathsep.join(vc + [os.path.join(MSYS, 'usr', 'bin')] + rest)
    sh['MSYSTEM'] = 'MSYS'
    sh['CHERE_INVOKING'] = '1'
    sh['PKG_CONFIG_PATH'] = os.path.join(lib, 'pkgconfig')
    configure = msys_path(os.path.join(SRC, 'ffmpeg', 'configure'))
    flags = FFMPEG_FLAGS + ['--prefix=' + msys_path(OUT), '--pkg-config=pkgconf']
    script = '%s %s && make -j%s && make install' % (configure, ' '.join("'%s'" % f for f in flags), jobs)
    run([os.path.join(MSYS, 'usr', 'bin', 'bash.exe'), '--noprofile', '--norc', '-c', script], sh, cwd=ff_build)

    # the DLLs' PDBs beside them, the licences, what was built and how
    for d in ('libavutil', 'libswresample', 'libswscale', 'libavcodec', 'libavformat'):
        for f in os.listdir(os.path.join(ff_build, d)):
            if f.endswith('.pdb'):
                shutil.copy2(os.path.join(ff_build, d, f), os.path.join(OUT, 'bin'))
    lic = os.path.join(OUT, 'licenses')
    os.makedirs(lic)
    shutil.copy2(os.path.join(SRC, 'ffmpeg', 'COPYING.LGPLv2.1'), os.path.join(lic, 'FFmpeg-COPYING.LGPLv2.1'))
    shutil.copy2(os.path.join(SRC, 'ffmpeg', 'LICENSE.md'), os.path.join(lic, 'FFmpeg-LICENSE.md'))
    shutil.copy2(os.path.join(SRC, 'dav1d', 'COPYING'), os.path.join(lic, 'dav1d-COPYING'))
    write_notes()
    with open(mark, 'w') as f:
        f.write(want + '\n')
    print('build.py: FFmpeg built (%s)' % OUT)


if __name__ == '__main__':
    main()
