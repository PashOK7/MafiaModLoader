// dusk: the scene's lights changed (scene_lights, light_set, scene_sun; docs/MODDING.md "The scene's lights").
//
// The level's sun and ambient lights (the lights that light people, cars and objects as they move: the world itself
// is lit ahead of time) are tinted towards a dusk: their colours times `tint`, their power times `strength`. Each
// light's own values are kept and put back when the mod is turned off or unloaded; a new mission's lights are tinted
// when it has loaded.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
struct Kept { void *frame; float color[3], power; };
std::vector<Kept> g_kept;
bool g_on = true;
float g_tint[3] = {1.0f, 0.55f, 0.35f}, g_strength = 0.6f;

void Settings() {
    g_on = H->config_number(M, "on", 1) != 0;
    g_tint[0] = static_cast<float>(H->config_number(M, "tint.r", 1.0));
    g_tint[1] = static_cast<float>(H->config_number(M, "tint.g", 0.55));
    g_tint[2] = static_cast<float>(H->config_number(M, "tint.b", 0.35));
    g_strength = static_cast<float>(H->config_number(M, "strength", 0.6));
}

void Restore() {
    for (const Kept &k : g_kept) H->light_set(M, k.frame, k.color, k.power, NAN, NAN, -1);
    g_kept.clear();
}

void Apply() {
    Restore();
    if (!g_on || H->mission_state() == MML_MISSION_NONE) return;
    MmlLight lights[512];
    int n = H->scene_lights(M, lights, 512);
    for (int i = 0; i < n && i < 512; i++) {
        const MmlLight &l = lights[i];
        if (l.type != MML_LIGHT_DIRECTIONAL && l.type != MML_LIGHT_AMBIENT) continue;
        Kept k{l.frame, {l.color[0], l.color[1], l.color[2]}, l.power};
        float c[3] = {l.color[0] * g_tint[0], l.color[1] * g_tint[1], l.color[2] * g_tint[2]};
        if (H->light_set(M, l.frame, c, l.power * g_strength, NAN, NAN, -1)) g_kept.push_back(k);
    }
    H->log(M, MML_LOG_INFO, "dusk: %d of the scene's %d lights tinted", static_cast<int>(g_kept.size()), n);
}

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_CONFIG_CHANGED: Settings(); Apply(); break;
    case MML_EVENT_MISSION_LOADED: g_kept.clear(); Apply(); break;
    case MML_EVENT_MISSION_CLOSE: g_kept.clear(); break;   // (its lights go with it)
    default: break;
    }
}

int Status(const char *, MmlReply *r, void *) {
    float s[3] = {}, c[3] = {}, a[3] = {};
    int sun = H->scene_sun(M, s, c, a);
    char out[320];
    snprintf(out, sizeof out, "{\"on\":%s,\"tinted\":%d,\"sun\":%s,\"to_sun\":[%.3f,%.3f,%.3f],\"sun_color\":[%.3f,%.3f,%.3f],\"ambient\":[%.3f,%.3f,%.3f]}",
             g_on ? "true" : "false", static_cast<int>(g_kept.size()), sun ? "true" : "false", s[0], s[1], s[2], c[0], c[1], c[2], a[0], a[1], a[2]);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, scene_sun)) return 1;   // a loader without the scene's lights
    Settings();
    for (MmlEventType t : {MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_LOADED, MML_EVENT_MISSION_CLOSE}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "on or off, the lights tinted, the scene's sun and ambient light now", Status, nullptr);
    Apply();   // (loaded into a running mission)
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { Restore(); }
