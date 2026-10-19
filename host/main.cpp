// MafiaModLoader.asi: the entry (see host.h).
//
// Ultimate ASI Loader loads this DLL early, on the game's main thread, before the game's own code runs. Init does
// only what is safe under the loader lock: paths, the log, the crash filter, the game's hooks (MinHook writes code,
// it loads nothing), the mods' manifests and their files\ folders (files.cpp: read before the game's first file),
// the bridge's thread. Mods are loaded at the first frame (the game's frame limiter), on the game thread, where
// LoadLibrary is safe.
#include "host.h"

namespace host {
namespace {
std::string g_gameDir, g_mmlDir;

// ---- other games beside this one. Game.exe makes the mutex "Mafia Launcher Super Mutex Shit" first thing
// (MafiaEntryPoint: 1.2 0x5BEBE0, 1.0 0x5F99B0) and quits at once, with no word, when it is there already: one Mafia
// at a time on the machine. Its import is given a mutex of this game's folder instead (the folder itself: its
// volume and file index, so a junction to it is the same folder), so games in other folders run together (the 1.2
// and the 1.0 labs side by side, a player's own game, with or without the loader), while the same folder twice is
// still refused as the game refuses it (one folder's saves, its mml\ files). The loader makes that mutex itself
// before anything else (Ultimate ASI Loader loads it before the game's code): a second game of a running one's folder
// leaves that folder's log, bridge.json and hooks alone and only lets the game quit.
constexpr char kGameMutex[] = "Mafia Launcher Super Mutex Shit";
using CreateMutexAFn = HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexAFn oCreateMutexA;
std::wstring g_mutexName;   // this folder's
HANDLE g_folderMutex;       // held for the process's life
bool g_second;              // it was there already: another game of this folder runs

std::wstring FolderMutexName() {
    wchar_t name[96];
    HANDLE dir = CreateFileW(Wide(g_gameDir).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    bool known = dir != INVALID_HANDLE_VALUE && GetFileInformationByHandle(dir, &info);
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    if (known) {
        swprintf(name, 96, L"%hs %08lX-%08lX%08lX", kGameMutex, info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow);
    } else {   // (its path, lower case)
        std::wstring w = Wide(g_gameDir);
        if (!w.empty()) CharLowerBuffW(&w[0], static_cast<DWORD>(w.size()));
        uint32_t h = 2166136261u;
        for (wchar_t c : w) { h ^= c; h *= 16777619u; }
        swprintf(name, 96, L"%hs path %08X", kGameMutex, h);
    }
    return name;
}

// The game's mutex: this folder's (the loader holds it already), and what the game reads next told as it would be
// told of its own: ERROR_ALREADY_EXISTS when another game of this folder runs, and it quits.
HANDLE WINAPI hkCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
    if (!name || strcmp(name, kGameMutex) != 0) return oCreateMutexA(sa, owner, name);
    HANDLE m = CreateMutexW(sa, owner, g_mutexName.c_str());
    SetLastError(g_second ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return m;
}

// False: a game of this folder runs already.
bool InstanceInit() {
    g_mutexName = FolderMutexName();
    g_folderMutex = CreateMutexW(nullptr, FALSE, g_mutexName.c_str());
    g_second = GetLastError() == ERROR_ALREADY_EXISTS;
    void *old = nullptr;
    if (PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", "CreateMutexA", reinterpret_cast<void *>(&hkCreateMutexA), &old))
        oCreateMutexA = reinterpret_cast<CreateMutexAFn>(old);
    return !g_second;
}
}
const std::string &GameDir() { return g_gameDir; }
const std::string &MmlDir() { return g_mmlDir; }

void Init() {
    std::string exe = ModulePath(nullptr);
    g_gameDir = exe.substr(0, exe.find_last_of("\\/") + 1);
    g_mmlDir = g_gameDir + "mml\\";
    if (!InstanceInit()) {   // (first: before the log, the bridge's file, anything of this folder's)
        LogQuiet();          // a game of this folder runs: its files are its own; the game quits at its entry
        return;
    }
    MakeDir(g_mmlDir);
    SetMainThread(GetCurrentThreadId());
    HLOG("MafiaModLoader (API %u) loaded into %s", MML_API_VERSION, exe.c_str());
    HLOG("instance: the game's one-at-a-time mutex is this folder's (%ls): games of other folders run beside it%s",
         g_mutexName.c_str(), oCreateMutexA ? "" : " (Game.exe's CreateMutexA import not found: not linked)");
    {
        AddressSpace a = AddressSpaceNow();
        HLOG("address space: %u MB, %u MB free (the largest piece %u MB)%s", a.totalMb, a.freeMb, a.largestMb,
             a.largeAddressAware ? " (Game.exe is Large Address Aware)" :
             " (Game.exe is not Large Address Aware: the 4 GB patch would double it, for texture packs, DXVK, ReShade)");
    }
    EnsureCrashFilter();
    if (GameHooksInstall()) {
        HLOG("game: Mafia %s", GameBuild());
        StartInit();
        InputInit();
        DrawInit();
        MenusInit();
        PanelInit();    // (the mods' settings in Options > Mods)
        RenderInit();
        SavesInit();
        AnimsInit();
    }
    FilesInit();        // (the game reads its first tables before the first frame: the mods' files are there already;
    ModsEarlyFiles();   // after the build is known: a mod made for another build gives none)
    GfxInit();          // (the game's device is made after this: the bridge's mods are known from their manifests)
    BridgeStart();
}
} // namespace host

// For the offline tests (tests/harness): no game frame limiter there, the harness pumps the loader's frames itself.
extern "C" __declspec(dllexport) void mml_host_pump() { host::Frame(); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) host::Init();
    else if (reason == DLL_PROCESS_DETACH) HLOG("process detaching");
    return TRUE;
}
