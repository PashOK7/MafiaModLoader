// mml_harness: a stand-in game for the offline tests (tests/run_tests.py).
//
// It loads MafiaModLoader.asi from its own folder (the loader sees no Mafia: no game hooks, everything else runs) and
// pumps the loader's frames at 60 Hz, as the game's frame limiter would; a stand-in rw_data.dll (rw_data.cpp) is
// loaded before it, as Game.exe imports the game's file layer. It exports harness_add, a function the test
// mods hook (two of them on the same function: the hook chains). It quits when a file named `quit` appears next to
// it, or after `--seconds N` (default 120).
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" __declspec(dllexport) __declspec(noinline) int harness_add(int a, int b) {
    volatile int x = a;   // (long enough for a hook: more than five bytes before its return)
    volatile int y = b;
    return x + y;
}

int main(int argc, char **argv) {
    int seconds = 120;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    DeleteFileA((dir + "quit").c_str());
    LoadLibraryA((dir + "rw_data.dll").c_str());   // (the game's file layer, there before the loader as Game.exe imports it)
    HMODULE loader = LoadLibraryA((dir + "MafiaModLoader.asi").c_str());
    if (!loader) { printf("harness: cannot load MafiaModLoader.asi (%lu)\n", GetLastError()); return 1; }
    auto pump = reinterpret_cast<void (*)()>(GetProcAddress(loader, "mml_host_pump"));
    if (!pump) { printf("harness: no mml_host_pump\n"); return 1; }
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
    DWORD end = GetTickCount() + seconds * 1000u;
    while (GetTickCount() < end) {
        pump();
        if (GetFileAttributesA((dir + "quit").c_str()) != INVALID_FILE_ATTRIBUTES) break;
        Sleep(16);
    }
    DeleteFileA((dir + "quit").c_str());
    printf("harness: done (harness_add(2, 3) = %d)\n", harness_add(2, 3));
    return 0;
}
