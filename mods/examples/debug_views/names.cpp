// debug_views, names: the name and the kind of every actor near the camera, over it (a debug view for mission and mod
// authors: what a script's FINDACTOR and the scene know it by; after pet1kk's developer tools, PR 2).
//
// Each actor of the mission within `range` of the camera (game::ListActors: the game's own list), nearest first, at
// most `max`: a label over its model with its frame's name and its kind (the class the game made it as:
// game::ActorTypeName, read from C_mission::CreateActor in both builds), coloured by the kind. Its key (Shift+F8)
// shows and hides it; Options > Mods > Debug views > Names has the rest. Command: debug_views.names.list.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "lines.h"
#include "views.h"

#define SECTION "names."

namespace {
const MmlHost *H;
MmlMod *M;
// The mission's world is there to draw over (in play, or under the pause menu: the menus are drawn over the views).
bool Shown() { const int st = H->mission_state(); return st == MML_MISSION_RUNNING || st == MML_MISSION_PAUSED; }
bool g_on, g_game;

struct Named { uintptr_t actor; int type; std::string name; float pos[3]; float dist; };
std::vector<Named> g_named;

template <class T> T R(uintptr_t a) {
    T v{};
    if (a) H->mem_read(a, &v, sizeof v);
    return v;
}
std::string Str(uintptr_t p) {
    char b[64] = {};
    if (p) H->mem_read(p, b, sizeof b - 1);
    return b;
}
bool SafeListActors(std::vector<void *> *out) {
    __try { game::ListActors(out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Read(const float eye[3]) {
    g_named.clear();
    std::vector<void *> actors;
    if (!g_game || !SafeListActors(&actors)) return;
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 40));
    for (void *p : actors) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p), frame = R<uintptr_t>(a + game::kActorFrame);
        float w[16];
        if (!frame || !H->frame_world(M, reinterpret_cast<void *>(frame), w)) continue;
        const float dx = w[12] - eye[0], dy = w[13] - eye[1], dz = w[14] - eye[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > range) continue;
        g_named.push_back({a, R<int>(a + game::kActorType), game::TextUtf8(Str(R<uintptr_t>(frame + game::kFrameName)).c_str()), {w[12], w[13], w[14]}, d});
    }
    std::sort(g_named.begin(), g_named.end(), [](const Named &x, const Named &y) { return x.dist < y.dist; });
}

void OnFrame(const MmlEvent *, void *) {
    if (!g_on || !Shown()) return;
    float o[3], d[3];
    if (!H->camera_ray(M, 400, 300, o, d)) return;
    Read(o);
    const int max = static_cast<int>(H->config_number(M, SECTION "max", 60));
    int shown = 0;
    for (const Named &n : g_named) {
        if (shown >= max) break;
        const char *kind = game::ActorTypeName(n.type);
        char b[48];
        snprintf(b, sizeof b, "%s", kind ? kind : "");
        if (!kind) snprintf(b, sizeof b, "type %d", n.type);
        const float at[3] = {n.pos[0], n.pos[1] + (n.type == game::kActorTypeCar ? 1.8f : 1.0f), n.pos[2]};
        shown += lines::Label(H, M, at, {n.name.empty() ? std::string("(no name)") : n.name, b}, lines::Palette(static_cast<uint32_t>(n.type)));
    }
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 0) != 0;
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "Shift+F8"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

// debug_views.names.list: the actors near the camera as data
int List(const char *, MmlReply *reply, void *) {
    float o[3], d[3];
    if (!H->camera_ray(M, 400, 300, o, d)) { H->reply_append(reply, "no frame drawn yet", static_cast<size_t>(-1)); return 1; }
    Read(o);
    std::string out = "{\"actors\":[";
    char b[256];
    for (size_t k = 0; k < g_named.size(); k++) {
        const Named &n = g_named[k];
        const char *kind = game::ActorTypeName(n.type);
        std::string name = n.name;
        for (char &ch : name) if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20) ch = '\'';
        snprintf(b, sizeof b, "%s{\"actor\":\"0x%08X\",\"type\":%d,\"kind\":\"%s\",\"name\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"distance\":%.1f}", k ? "," : "",
                 static_cast<unsigned>(n.actor), n.type, kind ? kind : "", name.c_str(), n.pos[0], n.pos[1], n.pos[2], n.dist);
        out += b;
    }
    out += "]}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int NamesLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, camera_ray) || !MML_HAS(host, frame_world)) return 1;   // a loader older than this view
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    if (!g_game) host->log(mod, MML_LOG_WARN, "names: not a known build of the game: nothing is read");
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, [](const MmlEvent *, void *) { ReadSettings(); }, nullptr);
    host->command(mod, "names.list", "the actors near the camera: their names, kinds, places and distances", List, nullptr);
    host->log(mod, MML_LOG_INFO, "names loaded (game %s)", host->game_build());
    return 0;
}

void NamesUnload() {}
