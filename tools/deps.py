"""deps.py: download the pinned third-party archives, check their SHA-256, unpack them into third_party/_deps/.

    python tools/deps.py            # everything missing (but the optional ones)
    python tools/deps.py jolt       # only these (the build asks for its libraries so: CMakeLists.txt)
    python tools/deps.py physx      # PhysX 5 (the x64 preset's: the mods' 64-bit halves)
    python tools/deps.py cuda       # the parts of NVIDIA's CUDA Toolkit that build PhysX's GPU part (no installation)
    python tools/deps.py flow       # NVIDIA Flow 2 with its shader compiler and premake (the x64 preset's)
    python tools/deps.py ngx vulkan # NVIDIA NGX (DLSS SDK) with DLSS 5 Neural Rendering, the Vulkan headers (x64)
    python tools/deps.py dxwrapper  # dxwrapper's sources (Direct3D 8 on 9Ex), built by the x86 preset with our patch
    python tools/deps.py dxvk       # DXVK's 32-bit d3d9.dll: the graphics bridge's Vulkan path tested in a lab
    python tools/deps.py ual10      # Ultimate ASI Loader 10 (its d3d8.dll with d3d8to9: the dlss_nr pack, Fusion Fix's way)
    python tools/deps.py ffmpeg     # FFmpeg's and dav1d's sources, built by the x86 preset (the loader's video player)
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
DLSS_RAW = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/'
DLSSNR_URL = 'https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z'
DLSSNR_SHA = '4bb8651b3ac2180f23df1f39422c489810a13d445a94dc836c876c18c93ecefe'

PINNED = {
    # Ultimate ASI Loader v9.7.4 (x86, with PDB), MIT; the hash is the one GitHub publishes for the asset. Its license
    # (the tag's `license`, as GitHub served it on 2026-10-19) beside it: a player's package carries it (tools/pack.py).
    'ual': [('https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip',
             '952cebfc30d525afc2bdbaca954329d405ded3aa688a83027354dae14dfd5c5f'),
            ('https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/v9.7.4/license',
             'ceca73c504e39084b0b80d2a437aea9988313d1915418d3c629c3e4a8313f5a1', '')],
    # Ultimate ASI Loader v10.0.0 (2026-10-21, x86, with PDB), MIT: its d3d8.dll form carries crosire's d3d8to9
    # ([GlobalSets] UseD3D8to9=1 in d3d8.ini), as Mafia Fusion Fix ships it; the dlss_nr pack puts it there over DXVK
    # (tools/pack.py "loader": "d3d8"). The hash is GitHub's published digest; the license is the same file as v9.7.4's.
    'ual10': [('https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v10.0.0/Ultimate-ASI-Loader.zip',
               '41bc535561a001f7164a9513c7b05fcecf08be780cac896ec84566cd6d534fb5'),
              ('https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/v10.0.0/license',
               'ceca73c504e39084b0b80d2a437aea9988313d1915418d3c629c3e4a8313f5a1', '')],
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
    # NVIDIA NGX for the mods' 64-bit halves (mods/examples/dlss_nr: DLSS 5 Neural Rendering over the game's frame),
    # under the NVIDIA RTX SDKs license (LICENSE.txt beside them: for building; nothing of it is committed or shipped by
    # this repository). The NGX API's headers and its static library (/MT) from the DLSS SDK 310.9.16 (NVIDIA/DLSS, tag
    # v310.9.16, 2026-10-16; the repository is 880 MB, so its files one by one, from that tag). DLSS 5 Neural
    # Rendering is not in the public DLSS SDK: its headers (a Vulkan API) and its runtime nvngx_dlssnr.dll 310.9.16
    # (signed by NVIDIA, the same file the RTX Remix runtime of 2026-10-16 ships) come from NVIDIA's own package for
    # RTX Remix, rtx-remix-ngx_sdk_dlnr version 2 (the package NVIDIAGameWorks/dxvk-remix's packman-external.xml
    # names, from the CDN its config.packman.xml names; 351 MB), a 7z archive unpacked by 7-Zip's own console 7zr.exe
    # 26.04 (ip7z/7zip's release; Python has no LZMA2 7z reader, nor Windows' tar). As Remix builds: the common NGX
    # headers from the DLSS SDK, only the DLSSNR ones from the package. The hashes: GitHub's published digest for
    # 7zr.exe, the files as GitHub and NVIDIA's CDN served them on 2026-10-19.
    'ngx': [(DLSS_RAW + 'include/nvsdk_ngx_defs.h', 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_defs_vk.h', '0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_params.h', '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_vk.h', '2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_helpers_vk.h', 'c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623', 'include'),
            (DLSS_RAW + 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', '7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765', 'lib'),
            # DLSS Super Resolution (DLAA to Ultra Performance) and Frame Generation of the same SDK: their runtimes
            # (signed by NVIDIA) and Frame Generation's headers (its Vulkan helpers; nvsdk_ngx.h and its loader header
            # are what nvsdk_ngx_params_dlssg.h includes). The hashes: the files as GitHub served them on 2026-10-23.
            (DLSS_RAW + 'include/nvsdk_ngx.h', 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_loader.h', 'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_defs_dlssg.h', 'c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_params_dlssg.h', 'c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad', 'include'),
            (DLSS_RAW + 'include/nvsdk_ngx_helpers_dlssg_vk.h', 'b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316', 'include'),
            (DLSS_RAW + 'lib/Windows_x86_64/rel/nvngx_dlss.dll', 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e', 'bin'),
            (DLSS_RAW + 'lib/Windows_x86_64/rel/nvngx_dlssg.dll', '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7', 'bin'),
            (DLSS_RAW + 'LICENSE.txt', 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0', ''),
            ('https://github.com/ip7z/7zip/releases/download/26.04/7zr.exe',
             '256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282', 'tools'),
            (DLSSNR_URL, DLSSNR_SHA, '', {'include/nvsdk_ngx_defs_dlssnr.h': 'include',
                                          'include/nvsdk_ngx_helpers_dlssnr_vk.h': 'include',
                                          'lib/Windows_x86_64/rel/nvngx_dlssnr.dll': 'bin',
                                          'LICENSE.txt': 'bin'})],
    # The Vulkan headers (KhronosGroup/Vulkan-Headers, tag vulkan-sdk-1.4.363.0, the newest), Apache 2.0 / MIT: a
    # 64-bit half that draws through Vulkan itself (dlss_nr) loads vulkan-1.dll at run time (VK_NO_PROTOTYPES), so only
    # the headers. The hash is the archive's as GitHub served it on 2026-10-19.
    'vulkan': ('https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip',
               'f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81', 'include'),
    # dxwrapper v1.8.8600.25 (elishacloud/dxwrapper, commit 2607aa3d73e4c063ecbcbab9b2395e9011d5cdd3, 2026-08-22), zlib:
    # the game's Direct3D 8 on Direct3D 9Ex (d3d8.dll stub and dxwrapper.dll: the release both labs run), built from
    # its sources by the x86 preset with third_party/dxwrapper/patch.py's changes (hooks that any thread may call keep
    # off thread_local and magic statics on a thread without TLS: one crashed a player's game). GitHub's archives have
    # no submodules: each submodule's archive at the commit the tag points to (all but DirectX, documentation), unpacked
    # where the submodule lives. The hashes are the archives' as GitHub served them on 2026-10-19.
    'dxwrapper': [
        ('https://github.com/elishacloud/dxwrapper/archive/2607aa3d73e4c063ecbcbab9b2395e9011d5cdd3.zip',
         '7be2c4c99607d30a60d81df7c7110a9efe888ad80efe219684a913063ad02db9', ''),
        ('https://github.com/microsoft/DirectXMath/archive/d33ba2f150aeb6d3cf62d10f454652ee83672200.zip',
         'c45263119b4b0310ad69f953574ea49c2ce84555bbd8584846abb3baf76e4a21', 'External/DirectXMath'),
        ('https://github.com/elishacloud/Hooking/archive/4fec9831ae7a4e99f58a9c837d37f76a88bf88da.zip',
         '5b39e8d4d8705f2beb2850586055355631588216710fd5b481420287b9bf8106', 'External/Hooking'),
        ('https://github.com/ThirteenAG/Hooking.Patterns/archive/460a47bc813085b66c79768531caab1ed5ea96fc.zip',
         '37a812b0ee905ccd00f85a18dc9e4ad336df2ad71fcb74e283bc22696458cee0', 'External/Hooking.Patterns'),
        ('https://github.com/elishacloud/Logging/archive/d352bb946ae25d2af1cc91227192f581baefbe92.zip',
         'eebf7de2455e064cffe7fe58e1e9c825422db510e38d964da4669b22299b2d7f', 'External/Logging'),
        ('https://github.com/fancycode/MemoryModule/archive/5f83e41c3a3e7c6e8284a5c1afa5a38790809461.zip',
         '4aec529595663ef5baa8bc30305de550d28d8832481720cfbbd58524dca3747f', 'External/MemoryModule'),
        ('https://github.com/crosire/d3d8to9/archive/6cdb8a82184898f1b9371e4c8412c2d33ebb7b51.zip',
         'b37d5e399e2f9c238d118cbe7502c441238544167310db300b0d373886e1ec60', 'External/d3d8to9'),
        ('https://github.com/microsoft/detours/archive/734ac64899c44933151c1335f6ef54a590219221.zip',
         'a42df660f6130b3272da7692fe24696ee032b6df58c035a33a2018b2f6c67370', 'External/detours'),
        ('https://github.com/elishacloud/dinputto8/archive/f18224808c58c84e46c003e100804bd3184d8168.zip',
         'cb4bac7802ec442fcce4824db9942f8f0473479682b222bdd04a4e8fcc090b69', 'External/dinputto8'),
        ('https://github.com/ocornut/imgui/archive/f1cc2ae15e53a861a874c3034aae6798fde194ab.zip',
         '008e1f3398e1c6e4d317ae4a05d2dec597bd360f4f3717b1d96e9edb66dba34f', 'External/imgui')],
    # FFmpeg 9.0.2 (the newest release, ffmpeg.org; its PGP signature checked on 2026-10-21 against the release key
    # FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8), LGPL 2.1 or later as built here (no --enable-gpl, no
    # nonfree), and dav1d 1.5.4 (VideoLAN, 2026-07-14; the hash VideoLAN publishes beside it), BSD 2-Clause: the
    # loader plays the game's videos with them (host/video.cpp). Built for x86 by third_party/ffmpeg/build.py (the x86
    # preset runs it once: MSVC, MSYS2's make, NASM, Meson), only the decoders, demuxers and parsers a video needs.
    'ffmpeg': [('https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz',
                '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e', 'ffmpeg', '*'),
               ('https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz',
                '686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5', 'dav1d', '*')],
    # DXVK 3.1.1 (doitsujin/dxvk, 2026-09-15), zlib: Direct3D 9 on Vulkan, as players put its d3d9.dll beside the game
    # (under dxwrapper, which loads the game folder's d3d9.dll first; ReShade on Vulkan then). The labs test the
    # graphics bridge's Vulkan path with it (host/gfx_vk.cpp: DXVK's shared handles are Vulkan's own, which Direct3D 11
    # cannot open). Its 32-bit d3d9.dll, and dxgi.dll and d3d11.dll (players put them beside the game too: the bridge
    # must still take Windows' own), and its license from the tag. The hashes: GitHub's published digest for
    # the release's archive, the license as GitHub served it on 2026-10-19.
    'dxvk': [('https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz',
              '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608', '',
              {'dxvk-3.1.1/x32/d3d9.dll': 'x32', 'dxvk-3.1.1/x32/dxgi.dll': 'x32', 'dxvk-3.1.1/x32/d3d11.dll': 'x32'}),
             ('https://raw.githubusercontent.com/doitsujin/dxvk/v3.1.1/LICENSE',
              'a5cb1a6ded7d2d7e92d550ba28edd21be2d1d4044662b399887351023e30ce64', '')],
    # Ghidra 12.1.4 (2026-09-21), Apache 2.0: the reverse-engineering projects of both builds behind tools/ghidra_cli.py
    # and the MCP server's ghidra_* tools. 570 MB (about 900 unpacked); needs a JDK 21 or later. The hash is the one
    # the release publishes (its notes and the asset's digest). Its PyGhidra and JPype wheels are in the archive: the
    # venv below is made from them, nothing else downloaded.
    'ghidra': ('https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/'
               'ghidra_12.1.4_PUBLIC_20260921.zip',
               'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'),
}
# fetched only when named: not every build needs them
OPTIONAL = {'ghidra', 'cuda', 'flow', 'ngx', 'vulkan', 'dxwrapper', 'dxvk', 'ffmpeg', 'ual10'}


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
    subfolder its content goes to; a fourth: the one folder of the archive taken, with the archive's LICENSE* files).
    A part that is not a .zip is a file put into that subfolder as it is; a .7z is unpacked by the 7zr.exe an earlier
    part put into tools/, and its fourth element names the files taken ({path in the archive: subfolder})."""
    dest = os.path.join(DEPS, name)
    mark = os.path.join(dest, '.pinned')
    want = hashlib.sha256(' '.join('/'.join(map(str, p[1:])) for p in parts).encode()).hexdigest()
    if not force and os.path.exists(mark) and open(mark).read().strip() == want:
        print('%-7s present' % name)
        return
    os.makedirs(DEPS, exist_ok=True)
    shutil.rmtree(dest, ignore_errors=True)
    tmp = os.path.join(DEPS, name + '.part')
    for url, sha, *rest in parts:
        sub, pick = (rest + ['', ''])[:2]
        kind = next((k for k in ('.tar.gz', '.tar.xz') if url.lower().endswith(k)), os.path.splitext(url)[1].lower())
        archive = os.path.join(DEPS, name + (kind if kind in ('.zip', '.7z', '.tar.gz', '.tar.xz') else '.file'))
        download(name, url, sha, archive)
        if kind == '.tar.xz' and pick == '*':   # the whole archive, its one top folder's content into the subfolder
            import tarfile
            shutil.rmtree(tmp, ignore_errors=True)
            with tarfile.open(archive, 'r:xz') as t:
                members = t.getmembers()
                for m in members:   # (Python before 3.10.12 has no extraction filter: the same checks by hand)
                    if os.path.isabs(m.name) or '..' in m.name.replace('\\', '/').split('/') or not (m.isfile() or m.isdir()):
                        sys.exit('%s: the archive holds %s: not used' % (name, m.name))
                t.extractall(tmp, members)
            os.remove(archive)
            entries = os.listdir(tmp)
            top = os.path.join(tmp, entries[0]) if len(entries) == 1 and os.path.isdir(os.path.join(tmp, entries[0])) else tmp
            shutil.copytree(top, os.path.join(dest, sub), dirs_exist_ok=True)
            shutil.rmtree(tmp)
            continue
        if kind == '.tar.gz':   # (the files its fourth element names: {path in the archive: subfolder})
            import tarfile
            with tarfile.open(archive, 'r:gz') as t:
                for path, to in pick.items():
                    os.makedirs(os.path.join(dest, to), exist_ok=True)
                    with t.extractfile(path) as src, open(os.path.join(dest, to, os.path.basename(path)), 'wb') as out:
                        shutil.copyfileobj(src, out)
            os.remove(archive)
            continue
        if kind not in ('.zip', '.7z'):
            os.makedirs(os.path.join(dest, sub), exist_ok=True)
            shutil.move(archive, os.path.join(dest, sub, url.rsplit('/', 1)[1]))
            continue
        if kind == '.7z':
            import subprocess
            shutil.rmtree(tmp, ignore_errors=True)
            seven = os.path.join(dest, 'tools', '7zr.exe')
            subprocess.run([seven, 'x', '-y', '-bso0', '-bsp0', '-o' + tmp, archive] + [p.replace('/', os.sep) for p in pick],
                           check=True)
            os.remove(archive)
            for path, to in pick.items():
                os.makedirs(os.path.join(dest, to), exist_ok=True)
                shutil.move(os.path.join(tmp, path.replace('/', os.sep)), os.path.join(dest, to, os.path.basename(path)))
            shutil.rmtree(tmp)
            continue
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
