// probe: questions to the world, shown (world_ray, world_sweep, world_near, world_shapes; docs/MODDING.md
// "Questions to the world").
//
// Each frame: what the crosshair looks at (a ray from the camera through the screen's middle, the player passed:
// how far, the static world's surface or a person's or a car's object), the ground under the player, a sphere swept
// a few metres ahead of his chest (what would stop him), the people near him; and the static world's collision
// around him drawn as lines over the frame (its triangles' edges, boxes as crosses), so what the game collides with
// can be seen against what it draws.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_on = true, g_wire = true;
float g_wireRadius = 4;
char g_look[160], g_ground[96], g_ahead[96], g_people[64];
MmlShape g_shapes[400];
int g_shapeCount;

const char *What(const MmlHit &h) {
    if (!h.actor) return "the static world";
    if (h.classes & 0x100) return "a person";
    if (h.classes & 0x200) return "a car";
    return "a moving object";
}

bool PlayerPos(void *p, float out[3]) {
    MmlBone b[MML_BONE_COUNT];
    if (!p || !H->human_skeleton(M, p, b, MML_BONE_COUNT) || !b[MML_BONE_PELVIS].frame) return false;
    for (int i = 0; i < 3; i++) out[i] = b[MML_BONE_PELVIS].world[12 + i];
    return true;
}

void Ask() {
    void *player = H->human_player(M);
    g_look[0] = g_ground[0] = g_ahead[0] = g_people[0] = 0;
    g_shapeCount = 0;
    // the crosshair: the camera's ray through the screen's middle
    float o[3], d[3];
    if (H->camera_ray(M, 400, 300, o, d)) {
        float to[3] = {o[0] + d[0] * 150, o[1] + d[1] * 150, o[2] + d[2] * 150};
        MmlHit h;
        if (H->world_ray(M, o, to, MML_WORLD_ALL, player, &h))
            snprintf(g_look, sizeof g_look, "looking at %s, %.1f m (kind 0x%02X, surface %u)", What(h), h.fraction * 150,
                     h.flags & 0xFF, h.actor ? 0 : h.flags >> 24);
        else
            snprintf(g_look, sizeof g_look, "looking at nothing within 150 m");
    }
    float pos[3];
    if (!PlayerPos(player, pos)) return;
    // the ground under him (the static world only)
    float down[3] = {pos[0], pos[1] - 50, pos[2]};
    MmlHit g;
    if (H->world_ray(M, pos, down, MML_WORLD_STATIC, nullptr, &g))
        snprintf(g_ground, sizeof g_ground, "ground %.2f m below the pelvis, at height %.2f", pos[1] - g.pos[1], g.pos[1]);
    // a 0.3 m sphere swept 4 m ahead of his chest, along the camera's look flattened
    if (H->camera_ray(M, 400, 300, o, d)) {
        float fl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        if (fl > 0.01f) {
            float from[3] = {pos[0], pos[1] + 0.4f, pos[2]}, to[3] = {from[0] + d[0] / fl * 4, from[1], from[2] + d[2] / fl * 4};
            MmlHit s;
            if (H->world_sweep(M, from, to, 0.3f, MML_WORLD_ALL, player, &s))
                snprintf(g_ahead, sizeof g_ahead, "ahead: %s stops a 0.3 m sphere after %.2f m", What(s), s.fraction * 4);
            else
                snprintf(g_ahead, sizeof g_ahead, "ahead: 4 m clear");
        }
    }
    void *nearby[64];
    int n = H->world_near(M, pos, 20, 2, nearby, 64);
    snprintf(g_people, sizeof g_people, "people within 20 m: %d", n > 0 ? n - 1 : 0);   // (him among them)
    if (g_wire) {
        float lo[3] = {pos[0] - g_wireRadius, pos[1] - 2, pos[2] - g_wireRadius}, hi[3] = {pos[0] + g_wireRadius, pos[1] + 3, pos[2] + g_wireRadius};
        g_shapeCount = H->world_shapes(M, lo, hi, MML_WORLD_ALL, g_shapes, 400);
        if (g_shapeCount > 400) g_shapeCount = 400;
    }
}

void Line(const float a[3], const float b[3], uint32_t c) {
    float sa[3], sb[3];
    if (H->camera_project(M, a, sa) && H->camera_project(M, b, sb)) H->draw_line(M, sa[0], sa[1], sb[0], sb[1], c);
}

void DrawShapes() {
    for (int i = 0; i < g_shapeCount; i++) {
        const MmlShape &s = g_shapes[i];
        uint32_t c = s.actor ? 0xC0FF6040u : 0x9040E0FFu;
        if (s.kind == MML_SHAPE_TRIANGLE) {
            for (int k = 0; k < 3; k++) Line(s.corner[k], s.corner[(k + 1) % 3], c);
        } else if (s.kind == MML_SHAPE_BOX) {
            for (int a = 0; a < 3; a++) {
                float p0[3], p1[3];
                for (int k = 0; k < 3; k++) {
                    p0[k] = s.center[k] - s.axes[a * 3 + k] * s.half[a];
                    p1[k] = s.center[k] + s.axes[a * 3 + k] * s.half[a];
                }
                Line(p0, p1, c);
            }
        } else {
            float r = s.half[0];
            for (int a = 0; a < 3; a++) {
                float p0[3] = {s.center[0], s.center[1], s.center[2]}, p1[3] = {s.center[0], s.center[1], s.center[2]};
                p0[a] -= r;
                p1[a] += r;
                Line(p0, p1, c);
            }
        }
    }
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) {
        g_wire = H->config_number(M, "wire", 1) != 0;
        g_wireRadius = static_cast<float>(H->config_number(M, "wire_radius", 4));
        return;
    }
    if (ev->type != MML_EVENT_FRAME || !g_on || H->mission_state() != MML_MISSION_RUNNING) return;
    Ask();
    if (g_wire) DrawShapes();
    float y = 420;
    for (const char *t : {g_look, g_ground, g_ahead, g_people}) {
        if (*t) H->draw_text(M, 12, y, 14, 0xFFE0F0FFu, MML_FONT_SMALL, MML_DRAW_LEFT, t);
        y += 16;
    }
    H->draw_line(M, 396, 300, 404, 300, 0xFFFFFFFFu);
    H->draw_line(M, 400, 296, 400, 304, 0xFFFFFFFFu);
}

int Status(const char *, MmlReply *r, void *) {
    char out[700];
    snprintf(out, sizeof out, "{\"on\":%s,\"look\":\"%s\",\"ground\":\"%s\",\"ahead\":\"%s\",\"people\":\"%s\",\"shapes\":%d}",
             g_on ? "true" : "false", g_look, g_ground, g_ahead, g_people, g_shapeCount);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int On(const char *a, MmlReply *r, void *u) {
    g_on = a && strstr(a, "true") != nullptr;
    return Status(a, r, u);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, world_shapes)) return 1;   // a loader without the world's questions
    g_wire = H->config_number(M, "wire", 1) != 0;
    g_wireRadius = static_cast<float>(H->config_number(M, "wire_radius", 4));
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_CONFIG_CHANGED}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "what the last frame's questions answered", Status, nullptr);
    H->command(M, "on", "{\"on\": true | false}", On, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
