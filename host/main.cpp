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
}
const std::string &GameDir() { return g_gameDir; }
const std::string &MmlDir() { return g_mmlDir; }

void Init() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_gameDir = path;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of("\\/") + 1);
    g_mmlDir = g_gameDir + "mml\\";
    CreateDirectoryA(g_mmlDir.c_str(), nullptr);
    SetMainThread(GetCurrentThreadId());
    HLOG("MafiaModLoader (API %u) loaded into %s", MML_API_VERSION, path);
    EnsureCrashFilter();
    if (GameHooksInstall()) {
        HLOG("game: Mafia %s", GameBuild());
        StartInit();
        InputInit();
        DrawInit();
        MenusInit();
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
