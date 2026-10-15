// rear_mirror: the scene drawn again from other cameras (scene_view; docs/MODDING.md "The scene from another camera").
//
// Modes (`rear_mirror.mode {"mode": "..."}`, or the setting "mode"):
// - "mirror" (the default): a rear-view mirror at the top of the screen. One view from the camera's eye looking back
//   (the camera turned half round about its up), a narrow frustum; the pass shows it flipped left to right, as a
//   mirror shows what is behind.
// - "stereo": the two eyes of a headset, side by side: each view from the camera's eye moved half the eyes' distance
//   to its side, with a headset's asymmetric frustum (wider outwards than towards the nose).
// - "check": a test of the asymmetric frusta and of the camera put back. One view of only the left half of the game
//   camera's frustum (tan_right 0), half the frame wide, compared pixel for pixel with the left half of the frame:
//   the pass shows their difference, 8 times, there (black where they are the same; lens flares, which views leave
//   out, and things that move between the two drawings show).
// - "reflect": a planar reflection, as water's: the camera mirrored in the level plane at the player's feet (his
//   car's), the frame's size, a clip plane just above it (nothing under the plane is drawn), the game camera's room
//   kept (the mirrored eye is under the ground); the pass lays it on the ground around the player as a puddle.
// - "spin": a mirror standing in the world, turning about its upright axis (`rear_mirror.spin {"at": [x, y, z]}`
//   puts its centre there; by default a few metres ahead of the camera when the mode starts). Each frame the game
//   camera is mirrored in the mirror's plane and the scene drawn from there with that plane as the clip plane (what is
//   behind the mirror left out); the pass finds where each pixel's ray meets the mirror (behind what stands in front
//   of it: the frame's depth) and takes the view's colour there through the view's own matrices. Seen from behind it
//   is a grey back, and no view is drawn.
// - "off": no view.
// The pass (rear_mirror.hlsl) copies a view's colour into a rectangle of the frame (MmlGfxFrame::views).
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "shaders/PsMirror.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_pass;
int g_views[2];
std::string g_mode = "mirror";
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
unsigned g_madeFor, g_frames, g_missing;
unsigned g_checkW, g_checkH;   // the check view's size (half the frame's)

struct Settings {
    float ipd = 0.064f, mirrorWidth = 0.42f, mirrorFov = 50.0f, puddle = 6.0f;
    float spinSpeed = 30.0f, spinWidth = 2.4f, spinHeight = 1.8f;
} S;
float g_plane[3];   // "reflect": the plane's height and the puddle's centre (x, z), as the last view was placed

// "spin": the mirror as the last view was placed: its centre, its front's normal, its right and up (unit)
struct Spin {
    bool placed, front;
    float at[3], n[3], r[3], u[3];
    double t0;
} g_spin;

double Seconds() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / f.QuadPart;
}
float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

void ReadSettings() {
    S.ipd = static_cast<float>(H->config_number(M, "ipd", 0.064));
    S.mirrorWidth = static_cast<float>(H->config_number(M, "mirror_width", 0.42));
    S.mirrorFov = static_cast<float>(H->config_number(M, "mirror_fov", 50));
    S.puddle = static_cast<float>(H->config_number(M, "puddle_radius", 6));
    S.spinSpeed = static_cast<float>(H->config_number(M, "spin_speed", 30));
    S.spinWidth = static_cast<float>(H->config_number(M, "spin_width", 2.4));
    S.spinHeight = static_cast<float>(H->config_number(M, "spin_height", 1.8));
}

// The mirror: the camera turned half round about its up (right and forward reversed: still a proper rotation).
int PlaceMirror(MmlViewCamera *c, void *) {
    for (int i = 0; i < 3; i++) { c->world[i] = -c->world[i]; c->world[8 + i] = -c->world[8 + i]; }
    float t = std::tan(S.mirrorFov * 0.5f * 0.0174533f);
    c->tan_left = c->tan_right = t;
    c->tan_down = c->tan_up = t * 200.0f / 640.0f;   // the view's shape: 640 x 200
    return 1;
}

// An eye: moved along the camera's right; its frustum wider outwards (a headset's: about 100 degrees across).
int PlaceEye(MmlViewCamera *c, void *user) {
    float side = user ? 1.0f : -1.0f;   // user: the right eye
    for (int i = 0; i < 3; i++) c->world[12 + i] += c->world[i] * side * S.ipd * 0.5f;
    float outer = 1.15f, inner = 0.95f;
    c->tan_left = side > 0 ? inner : outer;
    c->tan_right = side > 0 ? outer : inner;
    c->tan_down = c->tan_up = 1.05f;
    return 1;
}

// The check: the left half of the game camera's frustum.
int PlaceCheck(MmlViewCamera *c, void *) {
    c->tan_right = 0;
    return 1;
}

// The reflection: the eye, forward and up mirrored in the plane y = h (the host makes right from them again: a
// proper rotation, so the picture is the mirror's flipped left to right; the pass flips it back). Only what is above
// the plane is drawn. Not in a world with no player (a menu's).
int PlaceReflect(MmlViewCamera *c, void *) {
    int state = H->mission_state();
    if ((state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) || !strcmp(H->mission_name(), "00menu")) return 0;
    void *p = game::Player();
    if (!p) return 0;
    V3 at = game::ActorWorldPos(game::PlayerCar() ? game::PlayerCar() : p);
    float h = at.y;
    g_plane[0] = h;
    g_plane[1] = at.x;
    g_plane[2] = at.z;
    c->world[13] = 2 * h - c->world[13];
    c->world[5] = -c->world[5];   // up's y
    c->world[9] = -c->world[9];   // forward's y
    c->flags |= MML_VIEW_CLIP;
    c->clip[0] = 0;
    c->clip[1] = 1;
    c->clip[2] = 0;
    c->clip[3] = -(h + 0.05f);    // y >= h + 5 cm: the ground itself and what is under it left out
    return 1;
}

// The turning mirror: placed (once) ahead of the camera, turned by the time; the camera mirrored in its plane:
// p' = p - 2 ((p - at) . n) n for the eye, d' = d - 2 (d . n) n for its axes (the host makes right again from up
// and forward; the pass uses the matrices the view was drawn with, so the flip does not matter). Seen from behind:
// no view.
int PlaceSpin(MmlViewCamera *c, void *) {
    float *eye = c->world + 12;
    if (!g_spin.placed) {
        float f[3] = {c->world[8], 0, c->world[10]};
        float l = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) { f[0] = 0; f[2] = 1; l = 1; }
        for (int i : {0, 2}) g_spin.at[i] = eye[i] + f[i] / l * 12.0f;
        void *p = game::Player();   // its foot 20 cm above the ground the player (his car) stands on
        float ground = p ? game::ActorWorldPos(game::PlayerCar() ? game::PlayerCar() : p).y : eye[1] - 1.6f;
        g_spin.at[1] = ground + 0.2f + S.spinHeight * 0.5f;
        g_spin.t0 = Seconds();
        g_spin.placed = true;
    }
    // (the angle wrapped in double: a float of many turns' radians steps by hundredths, and the mirror jerks)
    float a = static_cast<float>(std::fmod((Seconds() - g_spin.t0) * S.spinSpeed * 0.017453292519943, 6.283185307179586));
    float n[3] = {std::sin(a), 0, std::cos(a)}, r[3] = {std::cos(a), 0, -std::sin(a)}, u[3] = {0, 1, 0};
    memcpy(g_spin.n, n, sizeof n);
    memcpy(g_spin.r, r, sizeof r);
    memcpy(g_spin.u, u, sizeof u);
    float rel[3] = {eye[0] - g_spin.at[0], eye[1] - g_spin.at[1], eye[2] - g_spin.at[2]};
    float side = Dot(rel, n);
    g_spin.front = side > 0.02f;
    if (!g_spin.front) return 0;   // its back is towards the camera
    for (int i = 0; i < 3; i++) eye[i] -= 2 * side * n[i];
    for (int row : {0, 4, 8}) {
        float d = Dot(c->world + row, n);
        for (int i = 0; i < 3; i++) c->world[row + i] -= 2 * d * n[i];
    }
    c->flags |= MML_VIEW_CLIP;    // only what is in front of the mirror (on the camera's side)
    for (int i = 0; i < 3; i++) c->clip[i] = n[i];
    c->clip[3] = -Dot(n, g_spin.at) - 0.01f;
    return 1;
}

void SetMode(const std::string &mode) {
    for (int &v : g_views) if (v) { H->scene_view_remove(M, v); v = 0; }
    g_mode = mode;
    g_checkW = g_checkH = 0;
    if (mode == "mirror") {
        g_views[0] = H->scene_view(M, 640, 200, PlaceMirror, nullptr);
    } else if (mode == "stereo") {
        g_views[0] = H->scene_view(M, 840, 880, PlaceEye, nullptr);
        g_views[1] = H->scene_view(M, 840, 880, PlaceEye, reinterpret_cast<void *>(1));
    } else if (mode == "reflect") {
        g_views[0] = H->scene_view(M, 0, 0, PlaceReflect, nullptr);   // the frame's size: pixel for pixel
    } else if (mode == "spin") {
        g_views[0] = H->scene_view(M, 0, 0, PlaceSpin, nullptr);      // the game camera's frustum, mirrored
    } else if (mode != "check") {
        g_mode = "off";
    }
    // "check": made by the pass, once the frame's size is known
}

const MmlGfxView *FindView(const MmlGfxFrame *f, int handle) {
    if (!MML_GFX_HAS(f, views) || !handle) return nullptr;
    for (unsigned i = 0; i < f->view_count; i++) if (f->views[i].handle == handle) return &f->views[i];
    return nullptr;
}

void Mul44(const float *a, const float *b, float *out) {   // out = a * b (row vectors)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

// One view into a rectangle of the frame (pixels), flipped left to right or not.
// how: 0 a copy, 1 the difference from the frame, 2 a puddle (g_plane), 3 the turning mirror (g_spin; v may be null:
// its back only)
void Show(ID3D11DeviceContext *ctx, const MmlGfxView *v, float x0, float y0, float x1, float y1, bool flip, float border,
          int how = 0) {
    const Spin &s = g_spin;
    bool front = how == 3 && v && v->fresh && s.front;
    float cb[44] = {x0, y0, x1, y1, flip ? 1.0f : 0.0f, border, static_cast<float>(how), 0, g_plane[0], S.puddle, g_plane[1], g_plane[2],
                    s.at[0], s.at[1], s.at[2], front ? 1.0f : 0.0f, s.n[0], s.n[1], s.n[2], S.spinWidth * 0.5f,
                    s.u[0], s.u[1], s.u[2], S.spinHeight * 0.5f, s.r[0], s.r[1], s.r[2], 0.08f};
    if (v) Mul44(v->view, v->proj, cb + 28);   // the view's view x projection: a world point to its picture
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    ID3D11ShaderResourceView *srv = v ? static_cast<ID3D11ShaderResourceView *>(v->color) : nullptr;
    ctx->PSSetShaderResources(2, 1, &srv);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);
}

void Pass(const MmlGfxFrame *f, void *) {
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        SafeRelease(g_ps);
        SafeRelease(g_cb);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 176;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(d->CreatePixelShader(kPsMirror, sizeof kPsMirror, nullptr, &g_ps)) || FAILED(d->CreateBuffer(&bd, nullptr, &g_cb))) return;
        g_madeFor = f->generation;
    }
    float W = static_cast<float>(f->width), Hh = static_cast<float>(f->height);
    if (g_mode == "check" && (g_checkW != f->width / 2 || g_checkH != f->height)) {
        if (g_views[0]) H->scene_view_remove(M, g_views[0]);
        g_checkW = f->width / 2;
        g_checkH = f->height;
        g_views[0] = H->scene_view(M, static_cast<int>(g_checkW), static_cast<int>(g_checkH), PlaceCheck, nullptr);
        return;   // (drawn from the next frame)
    }
    const MmlGfxView *a = FindView(f, g_views[0]), *b = FindView(f, g_views[1]);
    if (g_mode == "off") return;
    if (g_mode == "spin") {   // drawn even with no view yet (or seen from behind): its back
        if (g_spin.placed) Show(ctx, a, 0, 0, W, Hh, false, 0, 3);
        g_frames++;
        return;
    }
    if (!a || (g_mode == "stereo" && !b)) { g_missing++; return; }
    if (g_mode == "mirror") {
        float w = W * S.mirrorWidth, h = w * a->height / a->width, x = (W - w) * 0.5f, y = Hh * 0.03f;
        Show(ctx, a, x, y, x + w, y + h, true, 4);
    } else if (g_mode == "stereo") {
        float half = W * 0.5f, h = half * a->height / a->width;
        if (h > Hh) h = Hh;
        float w = h * a->width / a->height, y = (Hh - h) * 0.5f;
        Show(ctx, a, (half - w) * 0.5f, y, (half + w) * 0.5f, y + h, false, 0);
        Show(ctx, b, half + (half - w) * 0.5f, y, half + (half + w) * 0.5f, y + h, false, 0);
    } else if (g_mode == "check") {
        Show(ctx, a, 0, 0, static_cast<float>(a->width), static_cast<float>(a->height), false, 0, 1);
    } else if (g_mode == "reflect") {
        if (a->width != f->width || a->height != f->height) { g_missing++; return; }   // (the frame was resized)
        Show(ctx, a, 0, 0, W, Hh, true, 0, 2);
    }
    g_frames++;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) {
        ReadSettings();
        std::string mode = H->config_string(M, "mode", "mirror");
        if (mode != g_mode) SetMode(mode);
    }
}

int Reply(MmlReply *r) {
    char out[384];
    snprintf(out, sizeof out, "{\"mode\":\"%s\",\"views\":[%d,%d],\"frames\":%u,\"missing\":%u,\"spin\":{\"at\":[%.2f,%.2f,%.2f],"
             "\"normal\":[%.3f,%.3f,%.3f],\"front\":%s}}", g_mode.c_str(), g_views[0], g_views[1], g_frames, g_missing,
             g_spin.at[0], g_spin.at[1], g_spin.at[2], g_spin.n[0], g_spin.n[1], g_spin.n[2], g_spin.front ? "true" : "false");
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Status(const char *, MmlReply *r, void *) { return Reply(r); }
int Mode(const char *args, MmlReply *r, void *) {
    std::string a = args ? args : "";
    for (const char *m : {"mirror", "stereo", "check", "reflect", "spin", "off"})
        if (a.find(std::string("\"") + m + "\"") != std::string::npos) {
            if (!strcmp(m, "spin")) g_spin.placed = false;   // ahead of the camera again
            SetMode(m);
            return Reply(r);
        }
    H->reply_error(r, "{\"mode\": \"mirror\" | \"stereo\" | \"check\" | \"reflect\" | \"spin\" | \"off\"}");
    return 1;
}
// spin {"at": [x, y, z]}: the turning mirror's centre there; {}: ahead of the camera again at the next frame
int SpinAt(const char *args, MmlReply *r, void *) {
    const char *p = args ? strstr(args, "\"at\"") : nullptr;
    float x, y, z;
    if (p && (p = strchr(p, '[')) && sscanf(p, "[%f ,%f ,%f", &x, &y, &z) == 3) {
        g_spin.at[0] = x;
        g_spin.at[1] = y;
        g_spin.at[2] = z;
        g_spin.t0 = Seconds();
        g_spin.placed = true;
    } else {
        g_spin.placed = false;
    }
    if (g_mode != "spin") SetMode("spin");
    return Reply(r);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, scene_view)) return 1;   // a loader without scene views
    if (!game::Detect(H->module_base("LS3DF.dll"))) {   // (game.h's player, for "reflect")
        H->log(M, MML_LOG_ERROR, "not a build of the game whose addresses are known");
        return 2;
    }
    ReadSettings();
    g_pass = H->gfx_pass(M, 150, Pass, nullptr);
    SetMode(H->config_string(M, "mode", "mirror"));
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->command(M, "status", "the mode, its views, frames shown", Status, nullptr);
    H->command(M, "mode", "{\"mode\": \"mirror\"}: a rear-view mirror; \"stereo\": a headset's two eyes; \"check\": the "
                          "left half of the frame drawn again (it must not change); \"reflect\": a puddle; \"spin\": a "
                          "turning mirror in the world; \"off\"", Mode, nullptr);
    H->command(M, "spin", "{\"at\": [x, y, z]}: the turning mirror's centre there ({}: ahead of the camera)", SpinAt, nullptr);
    return g_pass ? 0 : 2;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
}
