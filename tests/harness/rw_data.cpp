// rw_data.dll for the stand-in game (tests/run_tests.py): the game's file layer as far as the loader uses it.
//
// The real one (the game's folder; re/ext/mafia-re/rw_data) opens a loose file of the game's folder by its name, and
// failing that an entry of its .dta archives; here the "archives" are the files of the harness's `archive\` folder,
// opened by the same names. Loose files are opened as the real one opens them, with CreateFileA from kernel32's
// imports sharing only reading (the loader adds writing and deleting for the mods' files). The harness loads this
// before the loader, as Game.exe imports the real one.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex g_mx;
std::vector<HANDLE> g_files;   // a handle is an index; INVALID_HANDLE_VALUE: free

uint32_t Add(HANDLE f) {
    for (size_t i = 0; i < g_files.size(); i++)
        if (g_files[i] == INVALID_HANDLE_VALUE) { g_files[i] = f; return static_cast<uint32_t>(i); }
    g_files.push_back(f);
    return static_cast<uint32_t>(g_files.size() - 1);
}
HANDLE Get(uint32_t h) { return h < g_files.size() ? g_files[h] : INVALID_HANDLE_VALUE; }
HANDLE Open(const std::string &path) {
    return CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,   // ansi-ok: the game's rw_data does so
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
} // namespace

extern "C" __declspec(dllexport) uint32_t __stdcall dtaOpen(const char *name, int) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!name || strlen(name) >= MAX_PATH) return 0xFFFFFFFFu;
    HANDLE f = Open(name);
    if (f == INVALID_HANDLE_VALUE && !(name[0] && name[1] == ':')) f = Open(std::string("archive\\") + name);
    return f == INVALID_HANDLE_VALUE ? 0xFFFFFFFFu : Add(f);
}

extern "C" __declspec(dllexport) uint32_t __stdcall dtaRead(uint32_t h, void *dst, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_mx);
    DWORD got = 0;
    HANDLE f = Get(h);
    if (f == INVALID_HANDLE_VALUE || !ReadFile(f, dst, size, &got, nullptr)) return 0xFFFFFFFFu;
    return got;
}

extern "C" __declspec(dllexport) uint32_t __stdcall dtaSeek(uint32_t h, int32_t offset, int origin) {
    std::lock_guard<std::mutex> lk(g_mx);
    HANDLE f = Get(h);
    if (f == INVALID_HANDLE_VALUE) return 0xFFFFFFFFu;
    return SetFilePointer(f, offset, nullptr, origin == 2 ? FILE_END : origin == 1 ? FILE_CURRENT : FILE_BEGIN);
}

extern "C" __declspec(dllexport) uint32_t __stdcall dtaClose(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_mx);
    HANDLE f = Get(h);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(f);
    g_files[h] = INVALID_HANDLE_VALUE;
    return 1;
}
