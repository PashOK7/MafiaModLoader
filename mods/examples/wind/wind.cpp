// wind: the game's own particles moved (particle_hook; docs/MODDING.md "The game's effects").
//
// Every particle the game shows (the exhaust, dust, grass, fire, steam) is told to the mod each time the game moves
// it; the mod adds a wind's drift for the time since its last move, and the particle is drawn there and goes on
// from there (the game moves it from where it was put). Gusts come and go.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
float g_wind[3] = {4, 0, 0}, g_gust = 0.5f;
unsigned long long g_seen[3], g_pushed;

void Settings() {
    g_wind[0] = static_cast<float>(H->config_number(M, "wind.x", 4));
    g_wind[1] = static_cast<float>(H->config_number(M, "wind.y", 0));
    g_wind[2] = static_cast<float>(H->config_number(M, "wind.z", 0));
    g_gust = static_cast<float>(H->config_number(M, "gust", 0.5));
}

int OnParticle(MmlParticle *p, void *) {
    if (p->kind >= 1 && p->kind <= 2) g_seen[p->kind]++;
    if (p->first || p->dt_ms <= 0 || p->dt_ms > 500) return 0;
    double t = GetTickCount() / 1000.0;
    float k = 1.0f + g_gust * static_cast<float>(std::sin(t * 0.7) * std::sin(t * 1.9));   // gusts
    float dt = p->dt_ms / 1000.0f;
    for (int i = 0; i < 3; i++) p->pos[i] += g_wind[i] * k * dt;
    g_pushed++;
    return 1;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) Settings();
}

int Status(const char *, MmlReply *r, void *) {
    char out[200];
    snprintf(out, sizeof out, "{\"wind\":[%.1f,%.1f,%.1f],\"gust\":%.2f,\"pooled_moves\":%llu,\"emitted_moves\":%llu,\"pushed\":%llu}",
             g_wind[0], g_wind[1], g_wind[2], g_gust, g_seen[MML_PARTICLE_POOLED], g_seen[MML_PARTICLE_EMITTED], g_pushed);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, particle_hook)) return 1;   // a loader without the game's particles
    Settings();
    if (!H->particle_hook(M, OnParticle, nullptr)) return 1;
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->command(M, "status", "the wind, the particles seen and pushed", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
