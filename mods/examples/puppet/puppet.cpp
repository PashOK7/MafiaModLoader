// puppet: a person's bones posed by a mod (humans_list, human_skeleton, bone_set; docs/MODDING.md "People,
// skeletons, poses").
//
// Every frame, in MML_EVENT_RENDER_BEGIN (after the game's animation set the bones, before LS3D draws them), the
// target (the player by default) is posed over his animation:
// - "wave": the right upper arm raised sideways and swung up and down, the forearm bent, at `speed` waves a second;
// - "tpose": both arms straight out (the bones' local rotations the identity: each one along its parent);
// - "off": the game's animation alone.
// Only the drawing: the game's logic, collision and aim are the animation's (`puppet.mode {"mode"}`, `puppet.target
// {"actor": "0x..." | "player"}`; `humans.list` on the bridge lists the people).
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
std::string g_mode = "wave";
void *g_target;          // null: the player
float g_speed = 1.2f;
unsigned g_posed, g_noSkeleton;


// A quaternion (w x y z) of `angle` radians about the axis (x, y, z) (unit).
void AxisAngle(float x, float y, float z, float angle, float q[4]) {
    float s = std::sin(angle * 0.5f);
    q[0] = std::cos(angle * 0.5f);
    q[1] = x * s;
    q[2] = y * s;
    q[3] = z * s;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) {
        g_speed = static_cast<float>(H->config_number(M, "speed", 1.2));
        g_mode = H->config_string(M, "mode", "wave");
        return;
    }
    if (ev->type != MML_EVENT_RENDER_BEGIN || g_mode == "off") return;
    if (!MML_EVENT_HAS(ev, world) || !ev->world) return;
    void *who = g_target ? g_target : H->human_player(M);
    if (!who) return;
    MmlBone b[MML_BONE_COUNT];
    if (!H->human_skeleton(M, who, b, MML_BONE_COUNT)) { g_noSkeleton++; return; }
    const float id[4] = {1, 0, 0, 0};
    if (g_mode == "tpose") {
        for (int k : {MML_BONE_R_ARM, MML_BONE_L_ARM, MML_BONE_R_ELBOW, MML_BONE_L_ELBOW})
            if (b[k].frame) H->bone_set(M, b[k].frame, id, nullptr);
    } else {   // wave: out sideways (the identity), swung up about the arm's local z between 10 and 60 degrees
        float t = static_cast<float>(GetTickCount() % 100000) / 1000.0f;
        float up = -(0.6f + 0.45f * std::sin(t * 6.2831853f * g_speed));   // (negative about z: up, seen in the 1.2 lab)
        float arm[4], elbow[4];
        AxisAngle(0, 0, 1, up, arm);
        AxisAngle(0, 0, 1, 1.2f, elbow);
        if (b[MML_BONE_R_ARM].frame) H->bone_set(M, b[MML_BONE_R_ARM].frame, arm, nullptr);
        if (b[MML_BONE_R_ELBOW].frame) H->bone_set(M, b[MML_BONE_R_ELBOW].frame, elbow, nullptr);
    }
    g_posed++;
}

int Reply(MmlReply *r) {
    char out[256];
    snprintf(out, sizeof out, "{\"mode\":\"%s\",\"target\":\"%s\",\"posed\":%u,\"no_skeleton\":%u}", g_mode.c_str(),
             g_target ? "an actor" : "player", g_posed, g_noSkeleton);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Status(const char *, MmlReply *r, void *) { return Reply(r); }
int Mode(const char *args, MmlReply *r, void *) {
    std::string a = args ? args : "";
    for (const char *m : {"wave", "tpose", "off"})
        if (a.find(std::string("\"") + m + "\"") != std::string::npos) { g_mode = m; return Reply(r); }
    H->reply_error(r, "{\"mode\": \"wave\" | \"tpose\" | \"off\"}");
    return 1;
}
int Target(const char *args, MmlReply *r, void *) {
    std::string a = args ? args : "";
    size_t at = a.find("0x");
    g_target = at == std::string::npos ? nullptr : reinterpret_cast<void *>(strtoul(a.c_str() + at, nullptr, 16));
    return Reply(r);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, human_player)) return 1;   // a loader without people's skeletons
    g_speed = static_cast<float>(H->config_number(M, "speed", 1.2));
    g_mode = H->config_string(M, "mode", "wave");
    H->subscribe(M, MML_EVENT_RENDER_BEGIN, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->command(M, "status", "the mode, the target, frames posed", Status, nullptr);
    H->command(M, "mode", "{\"mode\": \"wave\" | \"tpose\" | \"off\"}", Mode, nullptr);
    H->command(M, "target", "{\"actor\": \"0x...\"}: that person (humans.list); {}: the player", Target, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
