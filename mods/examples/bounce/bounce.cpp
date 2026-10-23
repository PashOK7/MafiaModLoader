// bounce: everything around the player pulsing in size to a beat (actor_scale; docs/MODDING.md "The size things are
// drawn at"). The "Bounce" of the racing games' and GTA's mods: cars, people, the traffic's walkers, trams and trains
// drawn tall and narrow on each beat and low and wide between, eased both ways (MML_PULSE_SQUASH: never longer),
// their physics and collision their own (the host scales their drawing alone; the player in his car goes with it).
//
// Every half second the mod looks for what is near the player and gives each new one the pulse (the host keeps it
// pulsing; all at one bpm beat together): the cars and the people by the collision's moving objects in a box around
// him (world_shapes: parked and traffic cars are there, though not among the game's actors), the traffic's walkers
// (pedestrians_list), the trams and the elevated trains (world_near, the game's type 8), the player himself. A hotkey
// turns it on and off, another starts the beat at once (tap it on the music's beat). Its settings: the game's
// Options > Mods > Bounce, or `config.set {"mod": "bounce", "key": "bpm", "value": 128}`.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_set>
#include <vector>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;

struct Settings {
    bool on, cars, people, pedestrians, trains, player;
    float bpm, amount, size, radius;
    int shape;
} S;
std::unordered_set<void *> g_pulsing;   // what has the pulse now
DWORD g_lastScan;

constexpr uint32_t kClassCarBody = 0x201, kClassCarHull = 0x202, kClassPersonMask = 0x100;
constexpr int kTypeRailway = 8;   // C_railway: the trams (salina*), the elevated trains (metro*)

void ReadSettings() {
    S.on = H->config_number(M, "on", 1) != 0;
    S.bpm = static_cast<float>(H->config_number(M, "bpm", 120));
    S.amount = static_cast<float>(H->config_number(M, "amount", 0.2));
    S.size = static_cast<float>(H->config_number(M, "size", 1));
    S.shape = static_cast<int>(H->config_number(M, "shape", MML_PULSE_SQUASH));
    S.radius = static_cast<float>(H->config_number(M, "radius", 80));
    S.cars = H->config_number(M, "cars", 1) != 0;
    S.people = H->config_number(M, "people", 1) != 0;
    S.pedestrians = H->config_number(M, "pedestrians", 1) != 0;
    S.trains = H->config_number(M, "trains", 1) != 0;
    S.player = H->config_number(M, "player", 1) != 0;
}

MmlActorScale Pulse() {
    MmlActorScale s{};
    s.size = sizeof s;
    s.scale[0] = s.scale[1] = s.scale[2] = S.size > 0 ? S.size : 1;
    s.pivot = MML_SCALE_PIVOT_BOTTOM;
    s.bpm = S.bpm;
    s.amount = S.amount;
    s.shape = S.shape;
    return s;
}

void StopAll() {
    for (void *a : g_pulsing) H->actor_scale(M, a, nullptr);
    g_pulsing.clear();
}

void Give(void *who, const MmlActorScale &s) {
    if (!who || g_pulsing.count(who)) return;
    if (H->actor_scale(M, who, &s)) g_pulsing.insert(who);
}

void Scan() {
    if (!S.on || H->mission_state() == MML_MISSION_NONE) return;
    void *player = H->human_player(M);
    if (!player) return;
    float w[16], at[3];
    void *frame = H->actor_frame(M, player);
    if (!frame || !H->frame_world(M, frame, w)) return;
    at[0] = w[12]; at[1] = w[13]; at[2] = w[14];
    MmlActorScale s = Pulse();
    if (S.player) Give(player, s);
    if (S.cars || S.people) {
        const float lo[3] = {at[0] - S.radius, at[1] - 40, at[2] - S.radius}, hi[3] = {at[0] + S.radius, at[1] + 40, at[2] + S.radius};
        static MmlShape shapes[2048];
        int n = H->world_shapes(M, lo, hi, MML_WORLD_MOVING, shapes, 2048);
        for (int i = 0; i < n && i < 2048; i++) {
            const MmlShape &sh = shapes[i];
            bool car = sh.classes == kClassCarBody || sh.classes == kClassCarHull;
            // (a person's own spheres; the traffic's walkers share their system's owner: pedestrians_list below)
            bool person = (sh.classes & kClassPersonMask) && sh.kind == MML_SHAPE_SPHERE;
            if ((car && S.cars) || (person && S.people)) Give(sh.actor, s);
        }
    }
    if (S.pedestrians) {
        static MmlPedestrian peds[256];
        int n = H->pedestrians_list(M, peds, 256);
        for (int i = 0; i < n && i < 256; i++) if (peds[i].spawned) Give(peds[i].element, s);
    }
    if (S.trains) {
        void *rail[64];
        int n = H->world_near(M, at, S.radius * 4, kTypeRailway, rail, 64);
        for (int i = 0; i < n && i < 64; i++) Give(rail[i], s);
    }
}

void Apply() {   // (the settings changed: every one given the new pulse)
    StopAll();
    Scan();
}

void BindKeys();
void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_CONFIG_CHANGED: ReadSettings(); BindKeys(); Apply(); break;
    case MML_EVENT_MISSION_TICK:
        if (GetTickCount() - g_lastScan >= 500) { g_lastScan = GetTickCount(); Scan(); }
        break;
    case MML_EVENT_MISSION_CLOSE: g_pulsing.clear(); break;   // (the host drops every one at the close)
    case MML_EVENT_ACTOR_DESTROYED: g_pulsing.erase(ev->actor); break;
    default: break;
    }
}

void BindKeys() {
    H->hotkey(M, H->config_string(M, "hotkey", "Shift+F7"), [](void *) { H->config_set(M, "on", S.on ? "false" : "true"); }, nullptr);
    H->hotkey(M, H->config_string(M, "beat_hotkey", "Shift+F5"), [](void *) {
        MmlActorScale s = Pulse();
        s.flags = MML_SCALE_BEAT_NOW;
        H->actor_scale(M, nullptr, &s);   // (no actor: the shared beat alone)
    }, nullptr);
}

int Status(const char *, MmlReply *r, void *) {
    char out[200];
    snprintf(out, sizeof out, "{\"on\":%s,\"pulsing\":%d,\"bpm\":%.1f,\"amount\":%.2f,\"shape\":%d,\"radius\":%.0f}", S.on ? "true" : "false",
             static_cast<int>(g_pulsing.size()), S.bpm, S.amount, S.shape, S.radius);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, actor_scale)) return 1;   // a loader without actor_scale
    ReadSettings();
    BindKeys();
    for (MmlEventType t : {MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_TICK, MML_EVENT_MISSION_CLOSE, MML_EVENT_ACTOR_DESTROYED})
        H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "on or off, how many pulse, the bpm, the amount, the shape, the radius", Status, nullptr);
    Scan();
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}   // (the host gives every one its own size back at the unload)
