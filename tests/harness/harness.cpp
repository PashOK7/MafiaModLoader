// mml_harness: a stand-in game for the offline tests (tests/run_tests.py).
//
// It loads MafiaModLoader.asi from its own folder (the loader sees no Mafia: no game hooks, everything else runs) and
// pumps the loader's frames at 60 Hz, as the game's frame limiter would; a stand-in rw_data.dll (rw_data.cpp) is
// loaded before it, as Game.exe imports the game's file layer. It exports harness_add, a function the test
// mods hook (two of them on the same function: the hook chains). It quits when a file named `quit` appears next to
// it, or after `--seconds N` (default 120); a file named `crash` makes it crash, an exception nobody handles (the
// loader's crash report is tested so), `crash_notls` the same on a thread of its own made without TLS, `crash_hooked`
// under the test mods' hooks of harness_add, `crash_mod` in hook_b's detour.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <float.h>
#include <string>

extern "C" __declspec(dllexport) __declspec(noinline) void harness_crash(volatile int *p) {
    *p = 1;
}

extern "C" __declspec(dllexport) __declspec(noinline) int harness_add(int a, int b) {
    volatile int x = a;   // (long enough for a hook: more than five bytes before its return)
    volatile int y = b;
    if (x == -1) harness_crash(nullptr);   // (`crash_hooked`: a crash under the mods' detours, called by the hooks)
    return x + y;
}

// harness_mid(a, f): (a + 1) + (int)(f * 2), with a place in its middle for a hook (hook_mid at harness_mid + 8,
// its exported offset harness_mid_at): EAX holds a there and the x87 stack holds f, live across the hook (a mod's
// code that uses the x87 must not lose it). The 5 bytes from there are whole instructions (add eax, 1; nop; nop).
extern "C" __declspec(dllexport) __declspec(naked) int harness_mid(int, float) {
    __asm {
        mov eax, [esp + 4]        ; 8B 44 24 04
        fld dword ptr [esp + 8]   ; D9 44 24 08
        add eax, 1                ; 83 C0 01 (harness_mid + 8: the hook's place)
        nop
        nop
        fadd st(0), st(0)
        fistp dword ptr [esp + 8]
        add eax, [esp + 8]
        ret
    }
}
extern "C" __declspec(dllexport) const unsigned harness_mid_at = 8;
// harness_seven(): 7, its byte at harness_seven + 1 the one a test patches (mov eax, 7: B8 07 00 00 00)
extern "C" __declspec(dllexport) __declspec(naked) int harness_seven() {
    __asm {
        mov eax, 7
        ret
    }
}

// A filter of the harness's own, set after the loader's (as a wrapper or an overlay sets one): the loader's runs
// first and chains to it.
LONG WINAPI HarnessFilter(EXCEPTION_POINTERS *) {
    printf("harness: its own filter called\n");
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A thread made as injectors and overlays make theirs (NtCreateThreadEx with SkipThreadAttach): no DllMain, no TLS
// vector. One of those faulted in a player's game, and the loader's crash filter faulted again on its thread_local.
LONG NTAPI NoTlsThread(void *) {
    printf("harness: a thread without TLS (fs:[0x2C] = %08lX) crashes\n", __readfsdword(0x2C));
    fflush(stdout);
    harness_crash(nullptr);
    return 0;
}

bool StartNoTlsThread() {
    using NtCreateThreadExFn = LONG(NTAPI *)(HANDLE *, ACCESS_MASK, void *, HANDLE, void *, void *, ULONG, SIZE_T,
                                             SIZE_T, SIZE_T, void *);
    auto create = reinterpret_cast<NtCreateThreadExFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx"));
    HANDLE t = nullptr;
    const ULONG kSkipThreadAttach = 0x2;
    if (!create || create(&t, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(), reinterpret_cast<void *>(NoTlsThread),
                          nullptr, kSkipThreadAttach, 0, 0, 0, nullptr) < 0)
        return false;
    CloseHandle(t);
    return true;
}

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // a crash ends it, no Windows dialog
    int seconds = 120;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
    // (by wide paths, as Windows loads the game's imports and Ultimate ASI Loader the .asi: the folder may have a name
    // the system's code page cannot spell; run_tests.py's unicode_dir_tests)
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    DeleteFileW((dir + L"quit").c_str());
    LoadLibraryW((dir + L"rw_data.dll").c_str());   // (the game's file layer, there before the loader as Game.exe imports it)
    HMODULE loader = LoadLibraryW((dir + L"MafiaModLoader.asi").c_str());
    if (!loader) { printf("harness: cannot load MafiaModLoader.asi (%lu)\n", GetLastError()); return 1; }
    auto pump = reinterpret_cast<void (*)()>(GetProcAddress(loader, "mml_host_pump"));
    if (!pump) { printf("harness: no mml_host_pump\n"); return 1; }
    // a frame's end as the game's Present makes it (prof.frames), 1 ms blocked in it
    auto present = reinterpret_cast<void (*)(unsigned)>(GetProcAddress(loader, "mml_host_present"));
    // the game's one-at-a-time check, as its entry makes it (after the loader, as Ultimate ASI Loader loads it before
    // the game's code): the loader gives it this folder's mutex (run_tests.py's instances_tests)
    CreateMutexA(nullptr, FALSE, "Mafia Launcher Super Mutex Shit");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("harness: another game is running: quitting, as the game does\n");
        return 3;
    }
    // a file read before the first frame, as the game reads its first tables: a mod's file answers already
    using DtaOpenFn = unsigned(__stdcall *)(const char *, int);
    using DtaReadFn = unsigned(__stdcall *)(unsigned, void *, unsigned);
    using DtaCloseFn = unsigned(__stdcall *)(unsigned);
    HMODULE rw = GetModuleHandleA("rw_data.dll");
    auto dtaOpen = rw ? reinterpret_cast<DtaOpenFn>(GetProcAddress(rw, "_dtaOpen@8")) : nullptr;
    auto dtaRead = rw ? reinterpret_cast<DtaReadFn>(GetProcAddress(rw, "_dtaRead@12")) : nullptr;
    auto dtaClose = rw ? reinterpret_cast<DtaCloseFn>(GetProcAddress(rw, "_dtaClose@4")) : nullptr;
    if (dtaOpen && dtaRead && dtaClose) {
        char text[64] = {};
        unsigned h = dtaOpen("tables\\test.txt", 0);
        if (h != 0xFFFFFFFFu) {
            dtaRead(h, text, sizeof text - 1);
            dtaClose(h);
        }
        printf("harness: before the first frame, tables\\test.txt is '%s'\n", h == 0xFFFFFFFFu ? "(none)" : text);
    }
    printf("harness: running (pid %lu)\n", GetCurrentProcessId());
    fflush(stdout);
    // the x87 at 24 bits, as the game's Direct3D leaves it (no D3DCREATE_FPU_PRESERVE): the loader must give it back
    // so after each frame, whatever its mods did
    unsigned int cw = 0;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    unsigned frames = 0, changed = 0;
    DWORD end = GetTickCount() + seconds * 1000u;
    while (GetTickCount() < end) {
        pump();
        if (present) present(1);
        frames++;
        _controlfp_s(&cw, 0, 0);
        if ((cw & _MCW_PC) != _PC_24) { changed++; _controlfp_s(&cw, _PC_24, _MCW_PC); }
        if (GetFileAttributesW((dir + L"quit").c_str()) != INVALID_FILE_ATTRIBUTES) break;
        if (GetFileAttributesW((dir + L"crash").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"crash").c_str());
            harness_crash(nullptr);
        }
        // the same under the hook chain on harness_add (hook_b's detour, hook_a's, MinHook's trampoline), or in hook_b's
        // detour itself (a mod's hook runs unguarded: the game calls it)
        if (GetFileAttributesW((dir + L"crash_hooked").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"crash_hooked").c_str());
            harness_add(-1, 0);
        }
        if (GetFileAttributesW((dir + L"crash_mod").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"crash_mod").c_str());
            harness_add(-2, 0);
        }
        if (GetFileAttributesW((dir + L"setfilter").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"setfilter").c_str());
            LPTOP_LEVEL_EXCEPTION_FILTER before = SetUnhandledExceptionFilter(HarnessFilter);
            HMODULE of = nullptr;
            wchar_t name[MAX_PATH] = L"none";
            if (before && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                             reinterpret_cast<LPCWSTR>(before), &of))
                GetModuleFileNameW(of, name, MAX_PATH);
            printf("harness: its own filter set (it was given %p in %ls)\n", reinterpret_cast<void *>(before), name);
            fflush(stdout);
        }
        if (GetFileAttributesW((dir + L"crash_notls").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"crash_notls").c_str());
            if (!StartNoTlsThread()) printf("harness: NtCreateThreadEx failed\n");
        }
        Sleep(16);
    }
    DeleteFileW((dir + L"quit").c_str());
    printf("harness: the x87 at 24 bits after %u frames, %u given back otherwise\n", frames, changed);
    printf("harness: done (harness_add(2, 3) = %d)\n", harness_add(2, 3));
    return 0;
}
