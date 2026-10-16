// exhaust_fx: the game's car exhaust read and replaced (exhaust_hook, car_exhaust, particle_def; docs/MODDING.md "The
// game's effects").
//
// Every tick the host tells the mod each car whose exhaust the game is about to make: its pipes, the jet out of
// them, the car's velocity at each pipe. The mod leaves the game's puffs out (`replace`) and makes its own, puffed at
// the game's own rate for the car's particle type (its particles.def record), moving as the game's puffs start
// (the jet, the car's motion, the emitter's addition) and rising; they are drawn over the frame as coloured squares
// sized by their distance (the screen's point from camera_project), fading over their life. With `game_tint` the
// game's own puffs are tinted instead, through particles.def at each mission's opening (the emitters take their
// records when the mission's load makes them).
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
struct Puff { float pos[3], vel[3], age, life, size; };
std::vector<Puff> g_puffs;
struct Pipe { const void *car; int pipe; float due; };
std::vector<Pipe> g_pipes;
bool g_replace = true;
int g_gameTint = -1;   // RRGGBB for the game's own exhaust (types 12 and 15), -1: as the game has it
uint32_t g_color = 0x40FF60;
float g_rise = 0.6f;
DWORD g_last;
unsigned g_cars, g_made;
MmlExhaust g_lastTold;   // the last car the host told of (for the status)

void Settings() {
    g_replace = H->config_number(M, "replace", 1) != 0;
    g_color = static_cast<uint32_t>(H->config_number(M, "color", 0x40FF60)) & 0xFFFFFF;
    g_rise = static_cast<float>(H->config_number(M, "rise", 0.6));
    g_gameTint = static_cast<int>(H->config_number(M, "game_tint", -1));
}

float Interval(int type) {
    MmlParticleDef d;
    if (type > 0 && H->particle_def(M, type, &d) && d.interval_ms > 0) return d.interval_ms / 1000.0f;
    return 0.05f;
}

int OnExhaust(const MmlExhaust *e, void *) {
    g_cars++;
    g_lastTold = *e;
    if (!e->emitting) return 0;
    float every = Interval(e->type), dt = e->dt_ms / 1000.0f;
    for (int i = 0; i < e->pipes; i++) {
        Pipe *p = nullptr;
        for (Pipe &x : g_pipes) if (x.car == e->car && x.pipe == i) { p = &x; break; }
        if (!p) { g_pipes.push_back({e->car, i, 0}); p = &g_pipes.back(); }
        p->due -= dt;
        if (p->due > 0 || g_puffs.size() > 3000) continue;
        p->due += every;
        Puff f;
        for (int k = 0; k < 3; k++) {
            f.pos[k] = e->pos[i][k];
            f.vel[k] = e->dir[i][k] * e->jet_speed + e->carry[i][k] + e->add_birth[k];
        }
        f.age = 0;
        f.life = 1.4f;
        f.size = 0.04f;
        g_puffs.push_back(f);
        g_made++;
    }
    return g_replace ? 1 : 0;
}

void Step(float dt) {
    size_t w = 0;
    for (Puff &p : g_puffs) {
        p.age += dt;
        if (p.age >= p.life) continue;
        float drag = std::exp(-2.5f * dt);
        for (int k = 0; k < 3; k++) { p.vel[k] *= drag; p.pos[k] += p.vel[k] * dt; }
        p.pos[1] += g_rise * dt;
        p.size += 0.2f * dt;
        g_puffs[w++] = p;
    }
    g_puffs.resize(w);
}

void Draw() {
    for (const Puff &p : g_puffs) {
        float s[3];
        if (!H->camera_project(M, p.pos, s) || s[2] < 0.3f) continue;
        float half = p.size * 300.0f / s[2];   // (about the frame's height: 600 units for a 90-degree view)
        if (half > 80) half = 80;
        float a = 1.0f - p.age / p.life;
        uint32_t alpha = static_cast<uint32_t>(a * a * 120);
        H->draw_rect(M, s[0] - half, s[1] - half, 2 * half, 2 * half, alpha << 24 | g_color, 1);
    }
}

// The game's exhaust records tinted (before the mission's load makes its emitters from them).
void TintGame() {
    if (g_gameTint < 0) return;
    for (int type : {12, 15}) {
        MmlParticleDef d;
        if (!H->particle_def(M, type, &d)) continue;
        float c[3] = {((g_gameTint >> 16) & 255) / 255.0f, ((g_gameTint >> 8) & 255) / 255.0f, (g_gameTint & 255) / 255.0f};
        for (int k = 0; k < 3; k++) { d.tint_birth[k] = c[k]; d.tint_death[k] = c[k]; }
        H->particle_def_set(M, type, &d);
    }
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { Settings(); return; }
    if (ev->type == MML_EVENT_MISSION_OPEN) { TintGame(); return; }
    if (ev->type == MML_EVENT_MISSION_CLOSE) { g_puffs.clear(); g_pipes.clear(); return; }
    if (ev->type != MML_EVENT_FRAME) return;
    DWORD now = GetTickCount();
    float dt = (now - g_last) / 1000.0f;
    g_last = now;
    if (H->mission_state() != MML_MISSION_RUNNING) return;
    if (dt > 0 && dt < 0.25f) Step(dt);
    Draw();
}

int Status(const char *, MmlReply *r, void *) {
    MmlExhaust e;
    int have = g_lastTold.car && H->car_exhaust(M, g_lastTold.car, &e);   // (read again now)
    char out[320];
    snprintf(out, sizeof out, "{\"replace\":%s,\"puffs\":%u,\"made\":%u,\"told\":%u,\"car\":%s,\"type\":%d,\"emitting\":%d,\"pipes\":%d,\"jet\":%.2f}",
             g_replace ? "true" : "false", static_cast<unsigned>(g_puffs.size()), g_made, g_cars, have ? "true" : "false", have ? e.type : 0,
             have ? e.emitting : 0, have ? e.pipes : 0, have ? e.jet_speed : 0.0f);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, particle_def_set)) return 1;   // a loader without the game's effects
    Settings();
    g_last = GetTickCount();
    if (!H->exhaust_hook(M, OnExhaust, nullptr)) return 1;
    TintGame();
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_CLOSE, MML_EVENT_MISSION_OPEN}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "replacing or not, the puffs alive and made, the player's car's exhaust", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
