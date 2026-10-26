// writer.asi: an ASI mod of the old kind for the offline tests (tests/run_tests.py asi_compat_checks), loaded by the
// stand-in game from its scripts\ as Ultimate ASI Loader loads them. It writes the stand-in's constants and code
// without VirtualProtect, as libMafiaCountrysideMap 2.0 writes Game.exe's: harness_ro_a and harness_eight's `mov eax,
// 8` in its DllMain, harness_ro_b from a thread made without TLS (NtCreateThreadEx's SkipThreadAttach). The loader's
// handler (host/asi_compat.cpp) lets each write through.
#include <windows.h>

namespace {
LONG NTAPI NoTlsWriter(void *) {
    auto b = reinterpret_cast<volatile unsigned *>(GetProcAddress(GetModuleHandleW(nullptr), "harness_ro_b"));
    if (b) b[0] = 0xB2;
    return 0;
}

void StartNoTlsWriter() {
    using NtCreateThreadExFn = LONG(NTAPI *)(HANDLE *, ACCESS_MASK, void *, HANDLE, void *, void *, ULONG, SIZE_T,
                                             SIZE_T, SIZE_T, void *);
    auto create = reinterpret_cast<NtCreateThreadExFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx"));
    HANDLE t = nullptr;
    const ULONG kSkipThreadAttach = 0x2;
    if (create && create(&t, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(), reinterpret_cast<void *>(NoTlsWriter),
                         nullptr, kSkipThreadAttach, 0, 0, 0, nullptr) >= 0)
        CloseHandle(t);
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    HMODULE game = GetModuleHandleW(nullptr);
    auto a = reinterpret_cast<volatile unsigned *>(GetProcAddress(game, "harness_ro_a"));
    auto code = reinterpret_cast<volatile unsigned char *>(GetProcAddress(game, "harness_eight"));
    if (a) a[0] = 0xA2;
    if (code) code[1] = 9;   // (B8 08 00 00 00: mov eax, 9)
    StartNoTlsWriter();
    return TRUE;
}
