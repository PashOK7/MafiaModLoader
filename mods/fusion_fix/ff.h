// ff.h: Fusion Fix (ThirteenAG, WidescreenFixesPack source/Mafia.FusionFix, MIT: LICENSE beside this file) on the
// loader. Its modules are kept as they are written there, against the few things they use, given here by the host:
//   hook::pattern, find_pattern      byte patterns looked for in the game's modules at run time (as Fusion Fix does:
//                                    any Game.exe of a build, not only the labs' files; tools/check_sites.py lists
//                                    what each pattern finds in the labs' files)
//   injector::WriteMemory            code_patch (what was there given back at the unload or when the settings change)
//   safetyhook::create_mid           hook_mid (the registers as MmlMidContext; eip set: the game goes on there)
//   safetyhook::create_inline        hook (a link of the loader's chains)
// Every patch and hook is the mod's: Undo() takes them all back, Apply() makes them again from the settings.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "mml/mml.h"

namespace ff {

extern const MmlHost *H;
extern MmlMod *M;

// ---- the settings (config.json; Fusion Fix's Mafia.FusionFix.ini by the same names)
struct Settings {
    bool widescreenHud = true, widescreenMap = true, zoomFmvs = true;
    int cutsceneBorders = 3;                 // 0 off, 1 letterbox, 2 pillarbox, 3 both
    bool noCutsceneBorderAnimation = false;
    bool drawDistance = true;
    float drawDistanceValue = 600, drawDistanceMin = 20, drawDistanceMax = 450;
    int fpsLimit = 0;
    bool borderlessWindowed = true;
    float leftStickDeadzone = 10, rightStickDeadzone = 10;
    bool restoreEyeAnimations = true, restoreCarScratches = true;
    bool radarMap = true;
    bool freeCamera = true;
    float cameraReturnTimeout = 3, cameraReturnSpeed = 2;
};
extern Settings S;

// ---- ComVars
struct ScreenT {
    float fWidth = 800.0f;
    float fHeight = 600.0f;
    float fAspectRatio = 4.0f / 3.0f;
    float fBaseWidth = 800.0f;
    float fInvBaseWidth = 1.0f / 800.0f;
};
extern ScreenT Screen;
extern float fCreditsTextX, fMapPlayerWidth, fMenuOffset, fMovieAspect;
enum { MAFIA_1_0_ENG, MAFIA_1_1_ENG, MAFIA_1_2_ENG };
extern int nGameVersion;
extern int32_t *pResolutionWidth, *pResolutionHeight;

template <class T> T &Field(uintptr_t object, size_t offset) { return *reinterpret_cast<T *>(object + offset); }
template <class Fn> Fn Virtual(void *object, size_t offset) {
    return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t **>(object))[offset / sizeof(uintptr_t)]);
}

uintptr_t ExeBase();
uintptr_t Ls3dBase();   // LS3DF.dll (0: not loaded)

// ---- patterns: "8B 35 ? ? ? ?" (hex bytes, ? any byte); every match in the range, in address order
struct Pattern {
    std::vector<uintptr_t> m;
    size_t size() const { return m.size(); }
    bool empty() const { return m.empty(); }
    template <class T = void> T *get_first(ptrdiff_t off = 0) const { return reinterpret_cast<T *>(m[0] + off); }
    struct Match {
        uintptr_t a;
        template <class T = void> T *get(ptrdiff_t off = 0) const { return reinterpret_cast<T *>(a + off); }
    };
    Match get(size_t i) const { return Match{m[i]}; }
};
Pattern ScanRange(uintptr_t lo, uintptr_t hi, const char *pattern);
Pattern ScanModule(uintptr_t base, const char *pattern);   // the module's whole image
// The first of the patterns that matches anywhere (Fusion Fix's find_pattern: one pattern a build, tried in order;
// `count` is Hooking.Patterns' hint, every match is found whatever it says)
template <size_t count = 1, class... A> Pattern find_module_pattern(uintptr_t base, A... alts) {
    Pattern p;
    if (base) ((p = ScanModule(base, alts), !p.empty()) || ...);
    return p;
}
template <size_t count = 1, class... A> Pattern find_pattern(A... alts) { return find_module_pattern(ExeBase(), alts...); }
inline Pattern range_pattern(uintptr_t lo, uintptr_t hi, const char *pattern) { return ScanRange(lo, hi, pattern); }

// ---- patches and hooks, each the mod's until Undo
bool Write(uintptr_t at, const void *bytes, size_t n);
template <class P, class T> bool WriteMemory(P at, T value) {
    return Write((uintptr_t)(at), &value, sizeof value);
}
using MidFn = void (*)(MmlMidContext &regs);
bool Mid(uintptr_t at, MidFn fn);
template <class P> bool Mid(P at, MidFn fn) { return Mid((uintptr_t)(at), fn); }
bool InlineRaw(uintptr_t at, void *detour, void **original);
template <class P, class F> bool Inline(P at, F detour, F *original) {
    return InlineRaw((uintptr_t)(at), reinterpret_cast<void *>(detour), reinterpret_cast<void **>(original));
}
// An import of `module` (its IAT slot for `dll`!`name`) pointed at `fn`; the address the slot held in *original.
bool ImportReplace(uintptr_t module, const char *dll, const char *name, void *fn, void **original);

void UndoPatches();              // every patch and hook of the mod taken back
size_t PatchCount();
extern double g_scanMs, g_hookMs, g_patchMs;   // the last Apply's time looking for patterns, and patching and hooking
size_t HookCount();

void Log(MmlLogLevel level, const char *fmt, ...);
// A module of the port told as on or off in the status (a part whose patterns were not found stays off, said once)
void Part(const char *name, bool on, const char *why = nullptr);

// ---- the modules (each Init* as in Fusion Fix; Reset* puts a module's own state back before the next Apply)
bool InitWidescreenFix();        // WidescreenFix.ixx: the resolution (false: not known), then the parts below
void ResetWidescreen();
HCURSOR WINAPI LoadCursorHook(HINSTANCE instance, LPCSTR name);
void InitCutscenes();            // Cutscene.ixx
float GetCameraFovScale();
void InitFOV();                  // LS3DF.ixx
void ResetFOV();
void InitMovie();
void InitHUD();                  // HUD.ixx
void InitMap();
void InitText();
void InitOthers();
void InitMenu();                 // Menu.ixx
void InitDrawDistance();         // DrawDistance.ixx
void InitUIScaling();            // UI.ixx
void ResetUI();
void InitCamera();               // Camera.ixx
void InitRadar();                // RestoredFeatures.ixx
void InitEyeAnimations();
void InitProjectorOpacity();
void InitLS3DF();                // dllmain.cpp: the borderless window, the shadows' z-bias
void ResetLS3DF();
void InitIJoy();                 // dllmain.cpp: the sticks' dead zones
void ResetCutscenes();
void ResetCamera();

} // namespace ff
