// fusion_fix: Mafia Fusion Fix (ThirteenAG, WidescreenFixesPack source/Mafia.FusionFix at d35057d, 2026-10-04; MIT:
// LICENSE beside this file) as a mod of the loader, in both builds: the HUD, the menus, the map, the texts, the field of view and the
// videos at the screen's aspect, the cutscenes' borders, the draw distance, the shadows' z-bias, the HUD's lines
// and the cursor at the screen's scale, the borderless window, the eyes' animations, the cars' scratches and the
// radar's map restored, the free camera around the car, the sticks' dead zones.
//
// Its modules are ported in widescreen.cpp, ls3df.cpp, cutscene.cpp, ui.cpp, camera.cpp, restored.cpp, ijoy.cpp,
// as they are written there, against ff.h (patterns, patches and hooks given by the host). What is not here: its
// settings file in place of the registry (WriteSettingsToFile), and its post-processing (ConsoleGamma, SMAA: shaders
// on d3d8to9's device, a graphics pass of the loader's to be). The settings are its ini's, live: a change takes
// every patch and hook back and makes them again (Apply), and the HUD is laid out again from its constants.
#include "ff.h"
#include <cstdio>
#include <string>
#include "mml/game/game_build.h"

namespace ff {
namespace {

struct PartState {
    const char *name;
    bool on;
};
std::vector<PartState> g_parts;
bool g_applied;
std::string g_refused;   // why nothing was applied (another Fusion Fix in the game, an unknown build)

void ReadSettings() {
    auto b = [](const char *k, bool d) { return H->config_number(M, k, d ? 1 : 0) != 0; };
    auto f = [](const char *k, float d) { return static_cast<float>(H->config_number(M, k, d)); };
    Settings s;
    s.widescreenHud = b("widescreen_hud", s.widescreenHud);
    s.widescreenMap = b("widescreen_map", s.widescreenMap);
    s.zoomFmvs = b("zoom_fmvs", s.zoomFmvs);
    s.cutsceneBorders = static_cast<int>(f("cutscene_borders", static_cast<float>(s.cutsceneBorders)));
    if (s.cutsceneBorders < 0 || s.cutsceneBorders > 3) s.cutsceneBorders = 3;
    s.noCutsceneBorderAnimation = b("no_cutscene_border_animation", s.noCutsceneBorderAnimation);
    s.drawDistance = b("draw_distance", s.drawDistance);
    s.drawDistanceValue = f("draw_distance_value", s.drawDistanceValue);
    s.drawDistanceMin = f("draw_distance_skip_min", s.drawDistanceMin);
    s.drawDistanceMax = f("draw_distance_skip_max", s.drawDistanceMax);
    s.fpsLimit = static_cast<int>(f("fps_limit", static_cast<float>(s.fpsLimit)));
    s.borderlessWindowed = b("borderless_windowed", s.borderlessWindowed);
    s.leftStickDeadzone = f("left_stick_deadzone", s.leftStickDeadzone);
    s.rightStickDeadzone = f("right_stick_deadzone", s.rightStickDeadzone);
    s.restoreEyeAnimations = b("restore_eye_animations", s.restoreEyeAnimations);
    s.restoreCarScratches = b("restore_car_scratches", s.restoreCarScratches);
    s.radarMap = b("radar_map", s.radarMap);
    s.freeCamera = b("free_camera", s.freeCamera);
    s.cameraReturnTimeout = f("camera_return_timeout", s.cameraReturnTimeout);
    s.cameraReturnSpeed = f("camera_return_speed", s.cameraReturnSpeed);
    S = s;
}

// The HUD's layout made again from its constants as they are now (game_build.h hudResolutionChanged): the game made
// it once in G_LoadSaveClass::Init, before the loader's mods loaded, and Fusion Fix's patches of those constants
// would otherwise only show at the next start.
void RelayoutHud() {
    const game::ExeBuild &e = game::Exe();
    if (!e.hudResolutionChanged || !e.hud) return;
    reinterpret_cast<void(__thiscall *)(uintptr_t)>(e.hudResolutionChanged)(e.hud);
}

// One part of the port: on when it made at least one patch or hook.
template <class F> void Step(const char *name, F init) {
    size_t before = PatchCount() + HookCount();
    init();
    bool on = PatchCount() + HookCount() > before;
    g_parts.push_back({name, on});
    if (!on) Log(MML_LOG_INFO, "%s: off (its settings, or its code not found in this game)", name);
}

void Undo() {
    UndoPatches();
    ResetWidescreen();
    ResetCutscenes();
    ResetUI();
    ResetLS3DF();
    ResetCamera();
    g_parts.clear();
    if (g_applied) {
        RelayoutHud();
        ResetFOV();
    }
    g_applied = false;
}

void Apply() {
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    Undo();
    g_scanMs = g_hookMs = g_patchMs = 0;
    g_refused.clear();
    for (const wchar_t *other : {L"Mafia.FusionFix.asi", L"Mafia.WidescreenFix.asi"})
        if (GetModuleHandleW(other)) {
            g_refused = "another Fusion Fix is in the game (" + std::string(other == std::wstring(L"Mafia.FusionFix.asi") ? "Mafia.FusionFix.asi" : "Mafia.WidescreenFix.asi") + "): take it out of the game's folder";
            Log(MML_LOG_WARN, "%s; nothing done", g_refused.c_str());
            return;
        }
    if (!InitWidescreenFix()) {
        g_refused = "the game's resolution was not found (an unknown Game.exe)";
        Log(MML_LOG_WARN, "%s; nothing done", g_refused.c_str());
        return;
    }
    // Fusion Fix's order: Init's SetHooks (at the resolution), its camera and radar; then LS3DF's, rw_data's, IJoy's
    Step("cutscenes", InitCutscenes);
    Step("fov", InitFOV);
    Step("hud", InitHUD);
    Step("map", InitMap);
    Step("text", InitText);
    Step("menu", InitMenu);
    Step("movie", InitMovie);
    if (S.drawDistance) Step("draw_distance", InitDrawDistance);
    Step("ui_scaling", InitUIScaling);
    Step("others", InitOthers);
    Step("camera", InitCamera);
    Step("radar", InitRadar);
    Step("eye_animations", InitEyeAnimations);
    Step("ls3df", InitLS3DF);
    Step("ijoy", InitIJoy);
    g_applied = true;
    RelayoutHud();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    Log(MML_LOG_INFO, "applied at %.0fx%.0f (game %s): %zu patches, %zu hooks, in %.0f ms (%.0f looking, %.0f patching, %.0f hooking)",
        Screen.fWidth, Screen.fHeight, nGameVersion == MAFIA_1_0_ENG ? "1.0" : nGameVersion == MAFIA_1_2_ENG ? "1.2" : "1.1",
        PatchCount(), HookCount(), (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart, g_scanMs, g_patchMs, g_hookMs);
}

void OnConfig(const MmlEvent *, void *) {
    ReadSettings();
    Apply();
}

int Status(const char *, MmlReply *reply, void *) {
    std::string out = "{\"applied\":" + std::string(g_applied ? "true" : "false");
    if (!g_refused.empty()) out += ",\"refused\":\"" + g_refused + "\"";
    char line[256];
    snprintf(line, sizeof line, ",\"screen\":[%.0f,%.0f],\"base_width\":%.2f,\"menu_offset\":%.1f,\"patches\":%zu,\"hooks\":%zu,\"parts\":{",
             Screen.fWidth, Screen.fHeight, Screen.fBaseWidth, fMenuOffset, PatchCount(), HookCount());
    out += line;
    for (size_t i = 0; i < g_parts.size(); i++)
        out += std::string(i ? "," : "") + "\"" + g_parts[i].name + "\":" + (g_parts[i].on ? "true" : "false");
    out += "}}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}

} // namespace
} // namespace ff

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    ff::H = host;
    ff::M = mod;
    if (!MML_HAS(host, code_patch)) return 1;   // a loader without hook_mid and code_patch
    if (!game::Detect(host->module_base("LS3DF.dll"))) {
        host->log(mod, MML_LOG_WARN, "fusion_fix: this game's build is not known; nothing done");
        return 1;
    }
    ff::ReadSettings();
    ff::Apply();
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, ff::OnConfig, nullptr);
    host->command(mod, "status", "what is applied: the screen, the parts on, the patches and hooks", ff::Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    ff::Undo();   // (the patches first, then the HUD laid out from the game's own constants again)
}
