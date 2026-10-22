"""patch.py: MafiaModLoader's changes to dxwrapper's sources (third_party/_deps/dxwrapper), applied before it is built
(CMakeLists.txt runs it after tools/deps.py dxwrapper). Each change replaces one exact piece of dxwrapper's text; the
files are made again from dxwrapper's own text each time (kept beside them as <file>.mml-orig), so the script may run
at every configure. dxwrapper's license (zlib) allows them; altered versions must be marked so: the log's first line
says this build is ours.

    python third_party/dxwrapper/patch.py [dxwrapper folder]

1. Hooks that any thread may call keep off thread-local storage on a thread that has none. With D3d8to9 on, dxwrapper
   installs DDrawCompat's Win32 hooks (GetTickCount, timeGetTime, PeekMessage, GetMessage, the waits,
   EnumDisplaySettings, GetVersionEx, RegQueryValueExA, RegGetValueW...) in every thread of the process, and some of
   them read a thread_local or a function's static (MSVC's thread-safe statics check an epoch kept in TLS). A thread made with
   NtCreateThreadEx's SkipThreadAttach (injectors, overlays, protections make theirs so) has no TLS vector: the read is
   at null + 4 * the module's TLS index, and the game dies (seen 2026-10-19 in a player's Mafia 1.2 on Windows 11 24H2:
   EIP in mitigateBusyWaiting, EAX 0, ECX 0xC). On such a thread every HOOK_FUNCTION and HOOK_SHIM_FUNCTION hook now
   calls the original function, and the busy-waiting mitigation is skipped. dxwrapper's own logging works there (it
   logged that crash).
2. The build number is fixed (8600: the release v1.8.8600.25 the labs ran) and d3d8to9's version written here: the
   project's pre-build step counts the build number up at every build and makes d3d8\\BuildNo.rc through a batch file
   that failed on this machine; the build passes PreBuildEventUseInBuild=false instead.
3. Managed textures stay in video memory on Direct3D 9Ex. 9Ex has no managed pool, and D3d9to9Ex made each managed
   texture DYNAMIC, which the driver and DXVK keep where the CPU reaches it: a second copy of every texture inside the
   game's 4 GB (a player's Mafia with an 8.4 GB texture pack held 3.2 GB of them and died loading the freeride,
   2026-10-22; with D3d9to9Ex off the same load took 578 MB). MmlManaged.h (beside this script, copied into d3d9\\ and
   included by d3d9.h) makes a managed 2D texture a default-pool one and gives the game system-memory copies of its
   levels while they are locked, kept least recently used within a budget and read back from the video memory when
   one let go is locked again; the device's writes into such a texture (UpdateSurface, UpdateTexture, StretchRect,
   ColorFill) mark its copies out of date.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DXW = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', '_deps', 'dxwrapper')
COMPAT = 'DDrawCompat/v0.3.2/'

EDITS = [
    # the check and the guard, beside the hooks' original pointers
    (COMPAT + 'Common/Hook.h',
     '#define HOOK_FUNCTION(module, func, newFunc) \\\n'
     '\tCompat32::hookFunction<&func>(#module, #func, &newFunc)\n',
     '// (MML: third_party/dxwrapper/patch.py) on a thread without a TLS vector the original function is called\n'
     '#define HOOK_FUNCTION(module, func, newFunc) \\\n'
     '\tCompat32::hookFunction<&func>(#module, #func, &Compat32::tlsGuardedFunc<&func, &newFunc>)\n'),
    (COMPAT + 'Common/Hook.h',
     '#define HOOK_SHIM_FUNCTION(func, newFunc) \\\n'
     '\tCompat32::hookFunction(reinterpret_cast<void*&>(Compat32::g_origFuncPtr<&func>), newFunc, #func)\n',
     '#define HOOK_SHIM_FUNCTION(func, newFunc) \\\n'
     '\tCompat32::hookFunction(reinterpret_cast<void*&>(Compat32::g_origFuncPtr<&func>), \\\n'
     '\t\treinterpret_cast<void*>(static_cast<decltype(&func)>(&Compat32::tlsGuardedFunc<&func, &newFunc>)), #func)\n'),
    (COMPAT + 'Common/Hook.h',
     '\ttemplate <auto origFunc>\n'
     '\tstd::string g_origFuncName;\n',
     '\ttemplate <auto origFunc>\n'
     '\tstd::string g_origFuncName;\n'
     '\n'
     '\t// (MML: third_party/dxwrapper/patch.py) Whether this thread has a TLS vector (the TEB\'s\n'
     '\t// ThreadLocalStoragePointer): a thread made with NtCreateThreadEx\'s SkipThreadAttach has none, and a\n'
     '\t// thread_local or a function\'s static (its thread-safe initialization checks an epoch kept in TLS) read on\n'
     '\t// it faults at null. Hooks any thread may call ask first.\n'
     '\tinline bool hasTlsVector()\n'
     '\t{\n'
     '#ifdef _M_IX86\n'
     '\t\treturn __readfsdword(0x2C) != 0;\n'
     '#else\n'
     '\t\treturn __readgsqword(0x58) != 0;\n'
     '#endif\n'
     '\t}\n'
     '\n'
     '\ttemplate <auto origFunc, auto newFunc, typename Result, typename... Params>\n'
     '\tResult WINAPI tlsGuardedFunc(Params... params)\n'
     '\t{\n'
     '\t\treturn hasTlsVector() ? newFunc(params...) : g_origFuncPtr<origFunc>(params...);\n'
     '\t}\n'),
    (COMPAT + 'Win32/WaitFunctions.cpp',
     '\t\tmitigateBusyWaiting();\n'
     '\t\treturn Compat32::g_origFuncPtr<func>(params...);\n',
     '\t\tif (Compat32::hasTlsVector())   // (MML: its thread_local faults on a thread without TLS)\n'
     '\t\t{\n'
     '\t\t\tmitigateBusyWaiting();\n'
     '\t\t}\n'
     '\t\treturn Compat32::g_origFuncPtr<func>(params...);\n'),
    # the log's first line says the build is ours (the zlib license: altered versions plainly marked)
    ('Dllmain/Dllmain.cpp',
     '\t\tLogging::Log() << "Starting DxWrapper v" << APP_VERSION;\n',
     '\t\tLogging::Log() << "Starting DxWrapper v" << APP_VERSION\n'
     '\t\t\t<< " (MafiaModLoader\'s build: hooks safe on threads without TLS, managed textures in video memory, '
     'third_party/dxwrapper/patch.py)";\n'),
    # 3. managed textures on 9Ex in video memory only (MmlManaged.h, written beside these files)
    ('d3d9/d3d9.h',
     '#include "IDirect3DVolumeTexture9.h"\n',
     '#include "IDirect3DVolumeTexture9.h"\n'
     '#include "MmlManaged.h"   // (MML: third_party/dxwrapper/patch.py)\n'),
    ('d3d9/IDirect3DDevice9Ex.cpp',
     '\tif (IsForcingD3d9to9Ex() && Pool == D3DPOOL_MANAGED && !(Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))\n'
     '\t{\n'
     '\t\tPool = D3DPOOL_DEFAULT;\n'
     '\t\tUsage |= D3DUSAGE_DYNAMIC;\n'
     '\t}\n'
     '\n'
     '\tHRESULT hr = ProxyInterface->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);\n'
     '\n'
     '\tif (SUCCEEDED(hr) && ppTexture)\n'
     '\t{\n',
     '\t// (MML: patch.py) a managed texture in video memory only, its copies kept by MmlManaged.h while they are used;\n'
     '\t// DYNAMIC (kept where the CPU reaches it, inside the game\'s address space) only for what that does not take\n'
     '\tbool MmlEmulated = false;\n'
     '\tif (IsForcingD3d9to9Ex() && Pool == D3DPOOL_MANAGED && !(Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))\n'
     '\t{\n'
     '\t\tPool = D3DPOOL_DEFAULT;\n'
     '\t\tMmlEmulated = MmlManaged::Wanted(Usage);\n'
     '\t\tif (!MmlEmulated)\n'
     '\t\t{\n'
     '\t\t\tUsage |= D3DUSAGE_DYNAMIC;\n'
     '\t\t}\n'
     '\t}\n'
     '\n'
     '\tHRESULT hr = ProxyInterface->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);\n'
     '\n'
     '\tif (SUCCEEDED(hr) && ppTexture)\n'
     '\t{\n'
     '\t\tif (MmlEmulated)\n'
     '\t\t{\n'
     '\t\t\tMmlManaged::Register(*ppTexture, Format);\n'
     '\t\t}\n'),
    # the GPU's writes into a texture: its copy out of date
    ('d3d9/IDirect3DDevice9Ex.cpp',
     '\tHRESULT hr = ProxyInterface->UpdateSurface(pSourceSurface, pSourceRect, pDestSurface, pDestPoint);\n',
     '\tHRESULT hr = ProxyInterface->UpdateSurface(pSourceSurface, pSourceRect, pDestSurface, pDestPoint);\n'
     '\tif (SUCCEEDED(hr))\n'
     '\t{\n'
     '\t\tMmlManaged::GpuWroteSurface(pDestSurface);   // (MML: patch.py)\n'
     '\t}\n'),
    ('d3d9/IDirect3DDevice9Ex.cpp',
     '\treturn ProxyInterface->UpdateTexture(pSourceTexture, pDestinationTexture);\n',
     '\tHRESULT hr = ProxyInterface->UpdateTexture(pSourceTexture, pDestinationTexture);\n'
     '\tif (SUCCEEDED(hr))\n'
     '\t{\n'
     '\t\tMmlManaged::GpuWroteTexture(pDestinationTexture);   // (MML: patch.py)\n'
     '\t}\n'
     '\treturn hr;\n'),
    ('d3d9/IDirect3DDevice9Ex.cpp',
     '\tHRESULT hr = ProxyInterface->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);\n',
     '\tHRESULT hr = ProxyInterface->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);\n'
     '\tif (SUCCEEDED(hr))\n'
     '\t{\n'
     '\t\tMmlManaged::GpuWroteSurface(pDestSurface);   // (MML: patch.py)\n'
     '\t}\n'),
    ('d3d9/IDirect3DDevice9Ex.cpp',
     '\tHRESULT hr = ProxyInterface->ColorFill(pSurface, pRect, color);\n',
     '\tHRESULT hr = ProxyInterface->ColorFill(pSurface, pRect, color);\n'
     '\tif (SUCCEEDED(hr))\n'
     '\t{\n'
     '\t\tMmlManaged::GpuWroteSurface(pSurface);   // (MML: patch.py)\n'
     '\t}\n'),
    # a texture's levels locked, unlocked; its end
    ('d3d9/IDirect3DTexture9.cpp',
     '\tHRESULT hr = ProxyInterface->LockRect(Level, pLockedRect, pRect, Flags);\n',
     '\t// (MML: patch.py) a managed texture emulated by MmlManaged.h: the level\'s copy in system memory\n'
     '\tHRESULT hr = MmlManaged::Has(ProxyInterface) ? MmlManaged::LockLevel(ProxyInterface, Level, pLockedRect, pRect, Flags) :\n'
     '\t\tProxyInterface->LockRect(Level, pLockedRect, pRect, Flags);\n'),
    ('d3d9/IDirect3DTexture9.cpp',
     '\treturn ProxyInterface->UnlockRect(Level);\n',
     '\treturn MmlManaged::Has(ProxyInterface) ? MmlManaged::UnlockLevel(ProxyInterface, Level) :   // (MML: patch.py)\n'
     '\t\tProxyInterface->UnlockRect(Level);\n'),
    ('d3d9/IDirect3DTexture9.cpp',
     '\tULONG ref = ProxyInterface->Release();\n'
     '\n'
     '\tif (ref == 0)\n'
     '\t{\n',
     '\tULONG ref = ProxyInterface->Release();\n'
     '\n'
     '\tif (ref == 0)\n'
     '\t{\n'
     '\t\tMmlManaged::Forget(ProxyInterface);   // (MML: patch.py) its copies with it\n'),
    # a level's surface locked, unlocked, its device context
    ('d3d9/IDirect3DSurface9.cpp',
     '\treturn GetNonMultiSampledSurface(Flags)->LockRect(pLockedRect, pRect, Flags);\n',
     '\t// (MML: patch.py) a level of a managed texture emulated by MmlManaged.h\n'
     '\tIDirect3DTexture9* MmlTexture = nullptr;\n'
     '\tUINT MmlLevel = 0;\n'
     '\tif (MmlManaged::LevelOf(ProxyInterface, &MmlTexture, &MmlLevel))\n'
     '\t{\n'
     '\t\treturn MmlManaged::LockLevel(MmlTexture, MmlLevel, pLockedRect, pRect, Flags);\n'
     '\t}\n'
     '\n'
     '\treturn GetNonMultiSampledSurface(Flags)->LockRect(pLockedRect, pRect, Flags);\n'),
    ('d3d9/IDirect3DSurface9.cpp',
     '\tHRESULT hr = D3DERR_INVALIDCALL;\n'
     '\n'
     '\t// Copy data back from emulated surface\n'
     '\tif (Emu.pSurface)\n'
     '\t{\n'
     '\t\thr = Emu.pSurface->UnlockRect();\n',
     '\tIDirect3DTexture9* MmlTexture = nullptr;   // (MML: patch.py)\n'
     '\tUINT MmlLevel = 0;\n'
     '\tif (MmlManaged::LevelOf(ProxyInterface, &MmlTexture, &MmlLevel))\n'
     '\t{\n'
     '\t\treturn MmlManaged::UnlockLevel(MmlTexture, MmlLevel);\n'
     '\t}\n'
     '\n'
     '\tHRESULT hr = D3DERR_INVALIDCALL;\n'
     '\n'
     '\t// Copy data back from emulated surface\n'
     '\tif (Emu.pSurface)\n'
     '\t{\n'
     '\t\thr = Emu.pSurface->UnlockRect();\n'),
    ('d3d9/IDirect3DSurface9.cpp',
     '\treturn GetNonMultiSampledSurface(0)->GetDC(phdc);\n',
     '\tIDirect3DTexture9* MmlTexture = nullptr;   // (MML: patch.py)\n'
     '\tUINT MmlLevel = 0;\n'
     '\tif (MmlManaged::LevelOf(ProxyInterface, &MmlTexture, &MmlLevel))\n'
     '\t{\n'
     '\t\treturn MmlManaged::GetLevelDC(MmlTexture, MmlLevel, phdc);\n'
     '\t}\n'
     '\n'
     '\treturn GetNonMultiSampledSurface(0)->GetDC(phdc);\n'),
    ('d3d9/IDirect3DSurface9.cpp',
     '\tHRESULT hr = D3DERR_INVALIDCALL;\n'
     '\n'
     '\t// Copy data back from emulated surface\n'
     '\tif (Emu.pSurface)\n'
     '\t{\n'
     '\t\thr = Emu.pSurface->ReleaseDC(hdc);\n',
     '\tIDirect3DTexture9* MmlTexture = nullptr;   // (MML: patch.py)\n'
     '\tUINT MmlLevel = 0;\n'
     '\tif (MmlManaged::LevelOf(ProxyInterface, &MmlTexture, &MmlLevel))\n'
     '\t{\n'
     '\t\treturn MmlManaged::ReleaseLevelDC(MmlTexture, MmlLevel, hdc);\n'
     '\t}\n'
     '\n'
     '\tHRESULT hr = D3DERR_INVALIDCALL;\n'
     '\n'
     '\t// Copy data back from emulated surface\n'
     '\tif (Emu.pSurface)\n'
     '\t{\n'
     '\t\thr = Emu.pSurface->ReleaseDC(hdc);\n'),
]


def patch_file(name, edits):
    path = os.path.join(DXW, name)
    orig = path + '.mml-orig'
    if not os.path.exists(orig):
        text = open(path, encoding='utf-8', newline='').read()
        if '(MML' in text:
            sys.exit('dxwrapper patch: %s holds an older patch and no %s: python tools/deps.py --force dxwrapper' % (name, orig))
        open(orig, 'w', encoding='utf-8', newline='').write(text)
    text = open(orig, encoding='utf-8', newline='').read()
    crlf = '\r\n' in text
    t = text.replace('\r\n', '\n')
    for old, new in edits:
        if t.count(old) != 1:
            sys.exit('dxwrapper patch: %s: the piece to change is not there once (%d): dxwrapper changed?' % (name, t.count(old)))
        t = t.replace(old, new)
    t = t.replace('\n', '\r\n') if crlf else t
    if open(path, encoding='utf-8', newline='').read() != t:
        open(path, 'w', encoding='utf-8', newline='').write(t)
        return True
    return False


def write_if_changed(path, text):
    if os.path.exists(path) and open(path, encoding='utf-8').read() == text:
        return False
    open(path, 'w', encoding='utf-8').write(text)
    return True


def main():
    written = 0
    names = sorted({e[0] for e in EDITS})
    for name in names:
        written += patch_file(name, [(o, n) for f, o, n in EDITS if f == name])
    # the build numbers (2.): dxwrapper's, and d3d8to9's FILEVERSION as the pre-build step would write it
    m = re.search(r'FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)', open(os.path.join(DXW, 'External', 'd3d8to9', 'res', 'd3d8to9.rc')).read())
    if not m:
        sys.exit('dxwrapper patch: no FILEVERSION in d3d8to9.rc')
    written += write_if_changed(os.path.join(DXW, 'Dllmain', 'BuildNo.rc'), '#define BUILD_NUMBER 8600\n')
    written += write_if_changed(os.path.join(DXW, 'd3d8', 'BuildNo.rc'), '#define FILEVERSION %s\n' % '.'.join(m.groups()))
    # the managed textures' emulation (3.), included by d3d9.h
    written += write_if_changed(os.path.join(DXW, 'd3d9', 'MmlManaged.h'),
                                open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'MmlManaged.h'), encoding='utf-8').read())
    print('dxwrapper patch: %d of %d files written (%d changes)' % (written, len(names) + 3, len(EDITS)))


main()
