// props_demo: models in the world that are not actors (prop_create, prop_place, prop_attach, prop_remove,
// actor_frame, frame_world; docs/MODDING.md "Props").
//
// A baseball bat rides on the player's back (a prop attached to his `back3` bone: it follows every move of his
// animation), stones circle his head (props in the world, placed every frame around where his neck is), and
// `props_demo.drop` takes the bat off his back and leaves it in the air where it was (attached to the world, its
// place kept). `props_demo.wear` puts it back.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
void *g_bat;
void *g_stones[8];
int g_count = 6;
float g_radius = 0.9f;
void *g_player;   // the person they were made for (a new mission: a new player)

void *Bone(void *human, int which) {
    MmlBone b[MML_BONE_COUNT];
    return human && H->human_skeleton(M, human, b, MML_BONE_COUNT) ? b[which].frame : nullptr;
}

void Wear() {
    void *back = Bone(g_player, MML_BONE_BACK3);
    if (!back) return;
    // In the bone's space (y up his spine, z out of his chest): behind his back, the bat's length (its y) along the
    // spine, turned 20 degrees about z to lie across it (as the labs showed it).
    const float pos[3] = {0, -0.05f, -0.2f};
    const float rot[4] = {0.9848078f, 0, 0, 0.1736482f};
    if (g_bat) H->prop_attach(M, g_bat, back, 0), H->prop_place(M, g_bat, pos, rot, 1);
    else g_bat = H->prop_create(M, "2bbat1.i3d", back, pos, rot, 1);
}

void Make() {
    g_player = H->human_player(M);
    if (!g_player) return;
    Wear();
    for (int i = 0; i < g_count && i < 8; i++)
        if (!g_stones[i]) g_stones[i] = H->prop_create(M, "2stone.i3d", nullptr, nullptr, nullptr, 0.25f);   // (a boulder: made small)
}
void Forget() {   // (the mission took them: every prop goes with it)
    g_bat = nullptr;
    for (void *&s : g_stones) s = nullptr;
    g_player = nullptr;
}

void Orbit() {
    void *neck = Bone(g_player, MML_BONE_NECK);
    float w[16];
    if (!neck || !H->frame_world(M, neck, w)) return;
    double t = GetTickCount() / 1000.0;
    for (int i = 0; i < g_count && i < 8; i++) {
        if (!g_stones[i]) continue;
        double a = t * 1.5 + i * 6.2831853 / g_count;
        const float pos[3] = {w[12] + g_radius * static_cast<float>(std::cos(a)), w[13] + 0.35f + 0.1f * static_cast<float>(std::sin(t * 3 + i)),
                              w[14] + g_radius * static_cast<float>(std::sin(a))};
        H->prop_place(M, g_stones[i], pos, nullptr, 0);
    }
}

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_MISSION_CLOSE: Forget(); break;
    case MML_EVENT_MISSION_LOADED: Make(); break;
    case MML_EVENT_RENDER_BEGIN:
        if (!g_player && H->mission_state() == MML_MISSION_RUNNING) Make();
        if (g_player) Orbit();
        break;
    default: break;
    }
}

int Status(const char *, MmlReply *r, void *) {
    int stones = 0;
    for (void *s : g_stones) stones += s != nullptr;
    char out[128];
    snprintf(out, sizeof out, "{\"bat\":%s,\"stones\":%d,\"player\":%s}", g_bat ? "true" : "false", stones, g_player ? "true" : "false");
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Drop(const char *a, MmlReply *r, void *u) {
    if (g_bat) H->prop_attach(M, g_bat, nullptr, 1);   // into the world, where it is now
    return Status(a, r, u);
}
int WearCmd(const char *a, MmlReply *r, void *u) {
    Wear();
    return Status(a, r, u);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, frame_world)) return 1;   // a loader without props
    g_count = static_cast<int>(H->config_number(M, "stones", 6));
    g_radius = static_cast<float>(H->config_number(M, "radius", 0.9));
    for (MmlEventType t : {MML_EVENT_MISSION_CLOSE, MML_EVENT_MISSION_LOADED, MML_EVENT_RENDER_BEGIN}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "the bat and the stones made", Status, nullptr);
    H->command(M, "drop", "the bat off his back, left in the air where it is", Drop, nullptr);
    H->command(M, "wear", "the bat on his back again", WearCmd, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}   // (the host removes a mod's props at its unload)
