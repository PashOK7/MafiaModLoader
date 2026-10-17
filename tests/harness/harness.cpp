// mml_harness: a stand-in game for the offline tests (tests/run_tests.py).
//
// It loads MafiaModLoader.asi from its own folder (the loader sees no Mafia: no game hooks, everything else runs) and
// pumps the loader's frames at 60 Hz, as the game's frame limiter would; a stand-in rw_data.dll (rw_data.cpp) is
// loaded before it, as Game.exe imports the game's file layer. It exports harness_add, a function the test
// mods hook (two of them on the same function: the hook chains). It quits when a file named `quit` appears next to
// it, or after `--seconds N` (default 120); a file named `crash` makes it crash, an exception nobody handles (the
// loader's crash report is tested so).
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <float.h>
#include <string>

extern "C" __declspec(dllexport) __declspec(noinline) int harness_add(int a, int b) {
    volatile int x = a;   // (long enough for a hook: more than five bytes before its return)
    volatile int y = b;
    return x + y;
}

extern "C" __declspec(dllexport) __declspec(noinline) void harness_crash(volatile int *p) {
    *p = 1;
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
        frames++;
        _controlfp_s(&cw, 0, 0);
        if ((cw & _MCW_PC) != _PC_24) { changed++; _controlfp_s(&cw, _PC_24, _MCW_PC); }
        if (GetFileAttributesW((dir + L"quit").c_str()) != INVALID_FILE_ATTRIBUTES) break;
        if (GetFileAttributesW((dir + L"crash").c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((dir + L"crash").c_str());
            harness_crash(nullptr);
        }
        Sleep(16);
    }
    DeleteFileW((dir + L"quit").c_str());
    printf("harness: the x87 at 24 bits after %u frames, %u given back otherwise\n", frames, changed);
    printf("harness: done (harness_add(2, 3) = %d)\n", harness_add(2, 3));
    return 0;
}
