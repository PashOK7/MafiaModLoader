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
     '\t\t\t<< " (MafiaModLoader\'s build: hooks safe on threads without TLS, third_party/dxwrapper/patch.py)";\n'),
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
    print('dxwrapper patch: %d of %d files written (%d changes)' % (written, len(names) + 2, len(EDITS)))


main()
