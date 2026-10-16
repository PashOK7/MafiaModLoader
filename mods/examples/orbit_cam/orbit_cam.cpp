// orbit_cam: the frame drawn from a camera of the mod's (camera_override; camera_project; docs/MODDING.md "The
// camera").
//
// While on (F8, or `orbit_cam.on {"on": true}`), the camera circles the player at `radius` metres and `height` above
// him, `speed` degrees a second, looking at his chest; the game's own camera, his aim and the controls are as they
// were (only the drawing moves). Over his head a label is drawn where camera_project puts his head on the screen,
// on or off: a world point on the screen, whatever camera drew the frame.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_on;
float g_radius = 6, g_height = 2.5f, g_speed = 25;
double g_angle;
DWORD g_last;
float g_head[3];
bool g_haveHead;

bool PlayerHead(float out[3]) {
    MmlBone b[MML_BONE_COUNT];
    void *p = H->human_player(M);
    if (!p || !H->human_skeleton(M, p, b, MML_BONE_COUNT) || !b[MML_BONE_NECK].frame) return false;
    out[0] = b[MML_BONE_NECK].world[12];
    out[1] = b[MML_BONE_NECK].world[13] + 0.25f;
    out[2] = b[MML_BONE_NECK].world[14];
    return true;
}

int PlaceCamera(MmlViewCamera *c, void *) {
    DWORD now = GetTickCount();
    g_angle += (now - g_last) / 1000.0 * g_speed * 0.0174533;
    g_last = now;
    if (!g_on || !g_haveHead) return 0;
    float target[3] = {g_head[0], g_head[1] - 0.6f, g_head[2]};
    float eye[3] = {target[0] + g_radius * static_cast<float>(std::cos(g_angle)), target[1] + g_height,
                    target[2] + g_radius * static_cast<float>(std::sin(g_angle))};
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    // forward and a world up: the host makes right and up orthonormal from them
    for (int i = 0; i < 3; i++) { c->world[8 + i] = f[i]; c->world[12 + i] = eye[i]; }
    c->world[4] = 0; c->world[5] = 1; c->world[6] = 0;
    c->flags |= MML_VIEW_OWN_SECTOR;   // (the room is looked up at the new eye)
    return 1;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) {
        g_radius = static_cast<float>(H->config_number(M, "radius", 6));
        g_height = static_cast<float>(H->config_number(M, "height", 2.5));
        g_speed = static_cast<float>(H->config_number(M, "speed", 25));
        return;
    }
    if (ev->type == MML_EVENT_RENDER_BEGIN) { g_haveHead = PlayerHead(g_head); return; }
    if (ev->type == MML_EVENT_FRAME && g_haveHead && H->mission_state() == MML_MISSION_RUNNING) {
        float s[3];
        if (H->camera_project(M, g_head, s)) {
            const char *t = g_on ? "orbit camera (F8)" : "F8: orbit";
            H->draw_text(M, s[0], s[1] - 18, 14, 0xFFFFE070u, MML_FONT_SMALL, MML_DRAW_CENTER, t);
        }
    }
}

void Toggle(void *) { g_on = !g_on; }

int Reply(MmlReply *r) {
    char out[160];
    snprintf(out, sizeof out, "{\"on\":%s,\"radius\":%.1f,\"height\":%.1f,\"speed\":%.0f,\"head\":[%.2f,%.2f,%.2f]}", g_on ? "true" : "false",
             g_radius, g_height, g_speed, g_head[0], g_head[1], g_head[2]);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Status(const char *, MmlReply *r, void *) { return Reply(r); }
int On(const char *a, MmlReply *r, void *) {
    g_on = a && strstr(a, "true") != nullptr;
    return Reply(r);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, camera_ray)) return 1;   // a loader without the camera's functions
    g_last = GetTickCount();
    g_radius = static_cast<float>(H->config_number(M, "radius", 6));
    g_height = static_cast<float>(H->config_number(M, "height", 2.5));
    g_speed = static_cast<float>(H->config_number(M, "speed", 25));
    H->camera_override(M, PlaceCamera, nullptr);
    H->hotkey(M, "F8", Toggle, nullptr);
    for (MmlEventType t : {MML_EVENT_RENDER_BEGIN, MML_EVENT_FRAME, MML_EVENT_CONFIG_CHANGED}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "on or off, the orbit, the player's head", Status, nullptr);
    H->command(M, "on", "{\"on\": true | false}", On, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
