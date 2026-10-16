// storm: the weather changed (weather_get, weather_set; docs/MODDING.md "The game's effects").
//
// Rain over the city through the scene's own weather system, as the missions' scripts start it: on, many drops,
// falling fast and slanted by a wind that turns; what the scene had is kept and put back when the storm is turned
// off (`storm.on {"on": false}`) or the mod unloads.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
double g_kept[MML_WEATHER_PARAMS];
bool g_haveKept, g_on;
float g_count = 20000, g_speed = 30, g_slant = 0.4f;
DWORD g_last;

void Settings() {
    g_count = static_cast<float>(H->config_number(M, "drops", 20000));
    g_speed = static_cast<float>(H->config_number(M, "speed", 30));
    g_slant = static_cast<float>(H->config_number(M, "slant", 0.4));
}

void Start() {
    if (H->mission_state() == MML_MISSION_NONE) return;
    if (!g_haveKept) {
        for (int i = 0; i < MML_WEATHER_PARAMS; i++)
            if (!H->weather_get(M, i, &g_kept[i])) return;   // (no mission's scene)
        g_haveKept = true;
    }
    H->weather_set(M, MML_WEATHER_MAX_COUNT, g_count);
    H->weather_set(M, MML_WEATHER_SPEED, g_speed);
    H->weather_set(M, MML_WEATHER_ON, 1);
    g_on = true;
}
void Stop() {
    if (g_haveKept)
        for (int i : {MML_WEATHER_SPEED, MML_WEATHER_MAX_COUNT, MML_WEATHER_DIR_X, MML_WEATHER_DIR_Y, MML_WEATHER_DIR_Z, MML_WEATHER_ON})
            H->weather_set(M, i, g_kept[i]);
    g_haveKept = false;
    g_on = false;
}

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_CONFIG_CHANGED: Settings(); if (g_on) Start(); break;
    case MML_EVENT_MISSION_CLOSE: g_haveKept = false; break;   // (the scene goes with its weather)
    case MML_EVENT_MISSION_LOADED: if (g_on) { g_on = false; Start(); } break;
    case MML_EVENT_MISSION_TICK: {
        if (!g_on) break;
        DWORD now = GetTickCount();
        if (now - g_last < 100) break;
        g_last = now;
        double t = now / 1000.0, a = t * 0.15;   // a wind that turns slowly
        H->weather_set(M, MML_WEATHER_DIR_X, g_slant * std::cos(a));
        H->weather_set(M, MML_WEATHER_DIR_Y, -1.0);
        H->weather_set(M, MML_WEATHER_DIR_Z, g_slant * std::sin(a));
        break;
    }
    default: break;
    }
}

int Status(const char *, MmlReply *r, void *) {
    double v[MML_WEATHER_PARAMS] = {};
    for (int i = 0; i < MML_WEATHER_PARAMS; i++) H->weather_get(M, i, &v[i]);
    char out[256];
    snprintf(out, sizeof out, "{\"on\":%s,\"rain\":%d,\"drops\":%.0f,\"speed\":%.2f,\"dir\":[%.2f,%.2f,%.2f]}", g_on ? "true" : "false",
             static_cast<int>(v[MML_WEATHER_ON]), v[MML_WEATHER_MAX_COUNT], v[MML_WEATHER_SPEED], v[MML_WEATHER_DIR_X], v[MML_WEATHER_DIR_Y],
             v[MML_WEATHER_DIR_Z]);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int On(const char *a, MmlReply *r, void *u) {
    if (a && strstr(a, "false")) Stop();
    else Start();
    return Status(a, r, u);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, weather_reset)) return 1;   // a loader without the weather
    Settings();
    for (MmlEventType t : {MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_CLOSE, MML_EVENT_MISSION_LOADED, MML_EVENT_MISSION_TICK})
        H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "the storm on or off, the scene's rain now", Status, nullptr);
    H->command(M, "on", "{\"on\": true | false}", On, nullptr);
    Start();   // (loaded into a running mission: the storm starts)
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { Stop(); }
