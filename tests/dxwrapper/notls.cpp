// dxwrapper_notls: dxwrapper's hooks called from a thread without TLS (run_tests.py).
//
// It loads the d3d8.dll stub beside it (which loads dxwrapper.dll, D3d8to9 on in dxwrapper.ini: DDrawCompat's Win32
// hooks go in), then makes a thread as injectors and overlays make theirs (NtCreateThreadEx with SkipThreadAttach: no
// TLS vector) and calls every function those hooks take on it. The release v1.8.8600.25 dies at the first one (a
// player's game did so: its busy-waiting mitigation reads a thread_local); the build with
// third_party/dxwrapper/patch.py calls the originals there. Prints what it did; exits 0 when the thread came back.
#include <windows.h>
#include <cstdio>
#include <string>

namespace {
LONG NTAPI Calls(void *) {
    printf("notls: the thread's TLS vector (fs:[0x2C]) = %08lX\n", __readfsdword(0x2C));
    fflush(stdout);
    HANDLE a = CreateEventA(nullptr, TRUE, TRUE, nullptr), b = CreateEventA(nullptr, TRUE, TRUE, nullptr);
    MSG msg;
    printf("notls: GetTickCount %lu\n", GetTickCount());
    auto timeGetTimeFn = reinterpret_cast<DWORD(WINAPI *)()>(GetProcAddress(LoadLibraryA("winmm.dll"), "timeGetTime"));
    printf("notls: timeGetTime %lu\n", timeGetTimeFn ? timeGetTimeFn() : 0);
    printf("notls: PeekMessageA %d, PeekMessageW %d\n", PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE),
           PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE));
    printf("notls: WaitForSingleObjectEx %lu\n", WaitForSingleObjectEx(a, 0, FALSE));
    HANDLE both[2] = {a, b};
    printf("notls: WaitForMultipleObjectsEx %lu\n", WaitForMultipleObjectsEx(2, both, TRUE, 0, FALSE));
    printf("notls: MsgWaitForMultipleObjects %lu, Ex %lu\n", MsgWaitForMultipleObjects(1, &a, FALSE, 0, QS_ALLINPUT),
           MsgWaitForMultipleObjectsEx(1, &a, 0, QS_ALLINPUT, 0));
    printf("notls: SignalObjectAndWait %lu\n", SignalObjectAndWait(a, b, 0, FALSE));
    OSVERSIONINFOA va = {sizeof va};
    OSVERSIONINFOW vw = {sizeof vw};
#pragma warning(suppress : 4996)
    BOOL gva = GetVersionExA(&va);
#pragma warning(suppress : 4996)
    BOOL gvw = GetVersionExW(&vw);
#pragma warning(suppress : 4996)
    DWORD gv = GetVersion();
    printf("notls: GetVersion %08lX, GetVersionExA %d, GetVersionExW %d\n", gv, gva, gvw);
    DEVMODEA dma = {};
    dma.dmSize = sizeof dma;
    DEVMODEW dmw = {};
    dmw.dmSize = sizeof dmw;
    printf("notls: EnumDisplaySettingsExA %d, ExW %d\n", EnumDisplaySettingsExA(nullptr, ENUM_CURRENT_SETTINGS, &dma, 0),
           EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &dmw, 0));
    HDC dc = GetDC(nullptr);
    printf("notls: GetDeviceCaps %d\n", GetDeviceCaps(dc, BITSPIXEL));
    ReleaseDC(nullptr, dc);
    MEMORYSTATUS ms = {sizeof ms};
    GlobalMemoryStatus(&ms);
    printf("notls: GlobalMemoryStatus %lu MB\n", static_cast<unsigned long>(ms.dwTotalPhys >> 20));
    NONCLIENTMETRICSA ncma = {sizeof ncma};
    NONCLIENTMETRICSW ncmw = {sizeof ncmw};
    printf("notls: SystemParametersInfoA %d, W %d\n", SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncma, &ncma, 0),
           SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncmw, &ncmw, 0));
    char value[256];
    DWORD size = sizeof value, type = 0;
    HKEY key = nullptr;
    LONG q = -1;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key) == 0) {
        q = RegQueryValueExA(key, "ProductName", nullptr, &type, reinterpret_cast<BYTE *>(value), &size);
        RegCloseKey(key);
    }
    wchar_t wvalue[256];
    DWORD wsize = sizeof wvalue;
    LONG g = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName",
                          RRF_RT_REG_SZ, nullptr, wvalue, &wsize);
    printf("notls: RegQueryValueExA %ld, RegGetValueW %ld\n", q, g);
    CloseHandle(a);
    CloseHandle(b);
    printf("notls: the thread came back\n");
    fflush(stdout);
    return 0;
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // a crash ends it, no Windows dialog
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE d3d8 = LoadLibraryW((dir + L"d3d8.dll").c_str());
    HMODULE dxw = GetModuleHandleW((dir + L"dxwrapper.dll").c_str());
    printf("notls: d3d8.dll stub %s, dxwrapper.dll %s\n", d3d8 ? "loaded" : "not loaded", dxw ? "loaded" : "not loaded");
    if (!d3d8 || !dxw) return 2;
    using NtCreateThreadExFn = LONG(NTAPI *)(HANDLE *, ACCESS_MASK, void *, HANDLE, void *, void *, ULONG, SIZE_T,
                                             SIZE_T, SIZE_T, void *);
    auto create = reinterpret_cast<NtCreateThreadExFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx"));
    HANDLE t = nullptr;
    const ULONG kSkipThreadAttach = 0x2;
    if (!create || create(&t, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(), reinterpret_cast<void *>(Calls), nullptr,
                          kSkipThreadAttach, 0, 0, 0, nullptr) < 0) {
        printf("notls: NtCreateThreadEx failed\n");
        return 3;
    }
    DWORD w = WaitForSingleObject(t, 20000);
    CloseHandle(t);
    printf("notls: %s\n", w == WAIT_OBJECT_0 ? "done" : "the thread did not end");
    return w == WAIT_OBJECT_0 ? 0 : 4;
}
