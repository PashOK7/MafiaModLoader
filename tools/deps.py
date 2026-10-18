"""deps.py: download the pinned third-party archives, check their SHA-256, unpack them into third_party/_deps/.

    python tools/deps.py            # everything missing (but the optional ones)
    python tools/deps.py jolt       # only these (the build asks for its libraries so: CMakeLists.txt)
    python tools/deps.py physx      # PhysX 5 (the x64 preset's: the mods' 64-bit halves)
    python tools/deps.py cuda       # the parts of NVIDIA's CUDA Toolkit that build PhysX's GPU part (no installation)
    python tools/deps.py flow       # NVIDIA Flow 2 with its shader compiler and premake (the x64 preset's)
    python tools/deps.py ghidra     # an optional one: Ghidra and its Python bridge (tools/ghidra_cli.py)
    python tools/deps.py --force    # again, from scratch

Only standard library. A hash that does not match stops everything: the archive is not used.
"""
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'third_party', '_deps')
CUDA_REDIST = 'https://developer.download.nvidia.com/compute/cuda/redist/'
PHYSX_URL = 'https://github.com/NVIDIA-Omniverse/PhysX/archive/517a0073715120e114ee055b63b26c95e00d9039.zip'
PHYSX_SHA = 'b2f713bac94e2655614b1c8c34ba18750673d5d780fba19e0a23a21bda5695bb'

PINNED = {
    # Ultimate ASI Loader v9.7.4 (x86, with PDB), MIT; the hash is the one GitHub publishes for the asset
    'ual': ('https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip',
            '952cebfc30d525afc2bdbaca954329d405ded3aa688a83027354dae14dfd5c5f'),
    # Jolt Physics v5.6.0, commit e77f175595e64cb44218cc9d9d56fc365ad0e36a (2026-07-11), MIT: the rigid bodies for
    # mods, built for x86 by CMakeLists.txt (target Jolt). The archive's 1250 files were compared with a clone of
    # the tag (mafia-ragdoll's, whose 577 unit tests pass on the 32-bit build): all identical.
    'jolt': ('https://github.com/jrouwe/JoltPhysics/archive/e77f175595e64cb44218cc9d9d56fc365ad0e36a.zip',
             'ff13202d21a7a09efea72534a0a041cd4971cb9ac66b53262275881387cc6871'),
    # NVIDIA PhysX SDK 5.9.0 (tag 110.1-omni-and-physx-5.9.0, commit 517a0073715120e114ee055b63b26c95e00d9039,
    # 2026-07-14), BSD 3-Clause: rigid bodies for the mods' 64-bit halves (PhysX 5 exists only for 64 bits), built by
    # the x64 preset (CMakeLists.txt, PhysX's own CMake as an external project: static, /MT, CPU only). The archive
    # is the whole repository (Omniverse's, Blast, Flow); only its physx/ folder is kept, and its LICENSE.md. The hash
    # is the archive's as GitHub served it on 2026-10-18 (the release publishes none for its source).
    'physx': (PHYSX_URL, PHYSX_SHA, 'physx'),
    # NVIDIA Flow 2.2.0 (2025-03-10, the newest Flow: the repository's flow/ changed only in its package scripts
    # since), BSD 3-Clause: smoke and fire on a sparse grid, simulated and ray-marched on the GPU through Vulkan, for
    # the mods' 64-bit halves (Flow 2 exists only for 64 bits). The flow/ folder of the same archive as PhysX's, with
    # what its own build would fetch through NVIDIA's packman given from their releases instead: the Slang shader
    # compiler (MIT/Apache 2.0, its SPIR-V), premake (BSD 3-Clause, the Visual Studio projects) and DXC (the LLVM
    # license; the DXIL Slang makes through it), laid out where flow/premake5.lua looks for them. Flow's own pins are
    # Slang 2025.6.1, premake 5.0.0-beta2 and DXC 1.7.2308; the newest releases (2026-10-17, 2026-10-05, 2026-09-29)
    # build it through third_party/flow/premake5.lua. The hashes are the ones GitHub publishes for the assets.
    'flow': [(PHYSX_URL, PHYSX_SHA, '', 'flow'),
             ('https://github.com/shader-slang/slang/releases/download/v2026.20/slang-2026.20-windows-x86_64.zip',
              'e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174', 'external/slang'),
             ('https://github.com/premake/premake-core/releases/download/v5.0.0/premake-5.0.0-windows.zip',
              '15e63506eed5dfd526b96c8e957a7feff2be6cfe70f46e353bfe4813b82e2fdc', 'external/premake/windows-x86_64'),
             ('https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip',
              'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1', 'external/dxc/bin', 'bin/x64')],
    # NVIDIA's CUDA Toolkit 12.8.1 (2025-03-06; PhysX 5.9's GPU part asks for 12.8), only what builds PhysX's GPU
    # projects: nvcc, the runtime and the driver API's headers and import library, CCCL, NVTX, the profiler API and
    # the Visual Studio integration (MSBuild's CUDA rules, given to the build as its CUDA toolset: `-T cuda=<path>`).
    # NVIDIA's redistributable archives (https://developer.download.nvidia.com/compute/cuda/redist/redistrib_12.8.1.json,
    # the hashes are the ones it publishes), merged into one folder laid out as the toolkit's (the Visual Studio
    # integration in extras/, where CMake looks for it): nothing installed, no administrator, about 370 MB. The CUDA Toolkit EULA (NVIDIA's): for building, not redistributed here.
    'cuda': [(CUDA_REDIST + 'cuda_nvcc/windows-x86_64/cuda_nvcc-windows-x86_64-12.8.93-archive.zip',
              '9fdc70b4271ed9aad4d64cd7076a7d96ec36512d074b9995fe638de669197391'),
             (CUDA_REDIST + 'cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.8.90-archive.zip',
              '4a39058fd8519444a81cfc7ae055d136f48d1a31ffa41ae255b35b2edd61e13b'),
             (CUDA_REDIST + 'cuda_cccl/windows-x86_64/cuda_cccl-windows-x86_64-12.8.90-archive.zip',
              'bd8548fa1ae82f92910bebc3079e14bd58c5a92aa64596d46bd610a478cb39d7'),
             (CUDA_REDIST + 'cuda_nvtx/windows-x86_64/cuda_nvtx-windows-x86_64-12.8.90-archive.zip',
              '38dbd5941537fcb359137f26fe31b4e3528c504cc2a55fa10a9baaf0ff006187'),
             (CUDA_REDIST + 'cuda_profiler_api/windows-x86_64/cuda_profiler_api-windows-x86_64-12.8.90-archive.zip',
              '0a9d628d2582bd58c78ba92827cd9613d62790924cde1178719f677fc03b6ee2'),
             (CUDA_REDIST + 'visual_studio_integration/windows-x86_64/'
              'visual_studio_integration-windows-x86_64-12.8.90-archive.zip',
              'f41d12a0e49b7848ed35e8a15b58926b83f635c723cab7e9952bc633e3c1f200', 'extras')],
    # Ghidra 12.1.4 (2026-09-21), Apache 2.0: the reverse-engineering projects of both builds behind tools/ghidra_cli.py
    # and the MCP server's ghidra_* tools. 570 MB (about 900 unpacked); needs a JDK 21 or later. The hash is the one
    # the release publishes (its notes and the asset's digest). Its PyGhidra and JPype wheels are in the archive: the
    # venv below is made from them, nothing else downloaded.
    'ghidra': ('https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/'
               'ghidra_12.1.4_PUBLIC_20260921.zip',
               'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'),
}
# fetched only when named: not every build needs them
OPTIONAL = {'ghidra', 'cuda', 'flow'}


def download(name, url, sha, archive):
    print('%-7s %s' % (name, url))
    with urllib.request.urlopen(url) as r, open(archive, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    h = hashlib.sha256()
    with open(archive, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    got = h.hexdigest()
    if got != sha:
        os.remove(archive)
        sys.exit('%s: SHA-256 %s, expected %s: not used' % (name, got, sha))


def fetch_parts(name, parts, force):
    """Several archives (each one top folder) merged into third_party/_deps/<name>/ (a part's third element: the
    subfolder its content goes to; a fourth: the one folder of the archive taken, with the archive's LICENSE* files)."""
    dest = os.path.join(DEPS, name)
    mark = os.path.join(dest, '.pinned')
    want = hashlib.sha256(' '.join('/'.join(p[1:]) for p in parts).encode()).hexdigest()
    if not force and os.path.exists(mark) and open(mark).read().strip() == want:
        print('%-7s present' % name)
        return
    os.makedirs(DEPS, exist_ok=True)
    shutil.rmtree(dest, ignore_errors=True)
    archive, tmp = os.path.join(DEPS, name + '.zip'), os.path.join(DEPS, name + '.part')
    for url, sha, *rest in parts:
        sub, pick = (rest + ['', ''])[:2]
        download(name, url, sha, archive)
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
        os.remove(archive)
        entries = os.listdir(tmp)
        top = os.path.join(tmp, entries[0]) if len(entries) == 1 and os.path.isdir(os.path.join(tmp, entries[0])) else tmp
        to = os.path.join(dest, sub) if sub else dest
        if pick:
            if not os.path.isdir(os.path.join(top, pick)):
                sys.exit('%s: the archive has no %s/ folder' % (name, pick))
            shutil.copytree(os.path.join(top, pick), to, dirs_exist_ok=True)
            for e in os.listdir(top):
                if e.upper().startswith('LICENSE') and os.path.isfile(os.path.join(top, e)):
                    shutil.copy2(os.path.join(top, e), to)
        else:
            shutil.copytree(top, to, dirs_exist_ok=True)
        shutil.rmtree(tmp)
    with open(mark, 'w') as f:
        f.write(want + '\n')
    print('%-7s ok (%d archives)' % (name, len(parts)))


def fetch(name, url, sha, force, keep=None):
    dest = os.path.join(DEPS, name)
    mark = os.path.join(dest, '.pinned')   # the hash it was unpacked from: a changed pin or a broken unpack is fetched anew
    if not force and os.path.exists(mark) and open(mark).read().strip() == sha:
        print('%-7s present' % name)
        return
    os.makedirs(DEPS, exist_ok=True)
    archive = os.path.join(DEPS, name + '.zip')
    download(name, url, sha, archive)
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    os.remove(archive)
    # a GitHub source archive holds one top folder: its content goes up one level
    entries = os.listdir(dest)
    if len(entries) == 1 and os.path.isdir(os.path.join(dest, entries[0])):
        top = os.path.join(dest, entries[0])
        for e in os.listdir(top):
            shutil.move(os.path.join(top, e), dest)
        os.rmdir(top)
    if keep:   # one folder of the archive is the library: it alone is kept, as the folder's content (with the
               # archive's license files beside it: LICENSE*)
        inner = os.path.join(dest, keep)
        if not os.path.isdir(inner):
            sys.exit('%s: the archive has no %s/ folder' % (name, keep))
        for e in os.listdir(dest):
            path = os.path.join(dest, e)
            if e.upper().startswith('LICENSE') and os.path.isfile(path) and not os.path.exists(os.path.join(inner, e)):
                shutil.move(path, inner)
            elif e != keep:
                shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
        os.rename(inner, inner + '.kept')
        for e in os.listdir(inner + '.kept'):
            shutil.move(os.path.join(inner + '.kept', e), dest)
        os.rmdir(inner + '.kept')
    with open(mark, 'w') as f:
        f.write(sha + '\n')
    print('%-7s ok (sha256 %s)' % (name, sha[:16]))


def pyghidra_venv(force):
    """third_party/_deps/pyghidra: a venv of the Python running this, with Ghidra's own PyGhidra and JPype wheels
    (no index: only the archive's). tools/ghidra_cli.py runs its server in it."""
    venv = os.path.join(DEPS, 'pyghidra')
    dist = os.path.join(DEPS, 'ghidra', 'Ghidra', 'Features', 'PyGhidra', 'pypkg', 'dist')
    py = os.path.join(venv, 'Scripts', 'python.exe')
    mark = os.path.join(venv, '.pinned')
    want = PINNED['ghidra'][1] + ' ' + sys.version.split()[0]
    if not force and os.path.exists(mark) and open(mark).read().strip() == want:
        print('%-7s present' % 'pyghidra')
        return
    import subprocess
    shutil.rmtree(venv, ignore_errors=True)
    subprocess.run([sys.executable, '-m', 'venv', venv], check=True)
    subprocess.run([py, '-m', 'pip', 'install', '--no-index', '--find-links', dist, 'pyghidra'], check=True)
    with open(mark, 'w') as f:
        f.write(want + '\n')
    print('%-7s ok (%s)' % ('pyghidra', venv))


def main():
    force = '--force' in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith('--')] or [n for n in PINNED if n not in OPTIONAL]
    unknown = [n for n in names if n not in PINNED]
    if unknown:
        sys.exit('not pinned: %s (pinned: %s)' % (', '.join(unknown), ', '.join(PINNED)))
    for name in names:
        if isinstance(PINNED[name], list):
            fetch_parts(name, PINNED[name], force)
            continue
        url, sha = PINNED[name][:2]
        fetch(name, url, sha, force, *PINNED[name][2:])
        if name == 'ghidra':
            pyghidra_venv(force)


if __name__ == '__main__':
    main()
