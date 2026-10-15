// world_marker: drawing in the game's 3D world with the game's own Direct3D device (the render events' example).
//
// What it does: a turning diamond and a beam of light over the player (or over a point a command fixes), drawn into
// the scene at MML_EVENT_RENDER_WORLD: the world is drawn then, the HUD not yet, so the marker is hidden by what
// stands in front of it, is lit by nothing and lies under the HUD. The device's view and projection are the scene's
// camera at that moment: the mod only sets its own world matrix. The loader keeps the game's device state around
// the event (every state, the render target, the depth buffer), so the mod sets what it needs and leaves it.
//
// Direct3D 9 (render_device(9)): the game on Direct3D 9 through a wrapper (dxwrapper's d3d8to9, as in the labs). On
// plain Direct3D 8 the mod says so and draws nothing.
//
// Commands: world_marker.status (frames drawn, the camera, where the marker is), world_marker.at {x, y, z} (a fixed
// point; {} follows the player again). Settings (config.json): the colour, the height over the player, the size.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace {
const MmlHost *H;
MmlMod *M;

struct Settings {
    uint32_t color = 0xFFFFC040u;
    float height = 2.6f, size = 0.35f, beam = 40.0f;
} S;
bool g_fixed;
V3 g_at;
unsigned g_drawn, g_noD3d9;
V3 g_lastPos;
MmlRenderCamera g_cam;

uint32_t ParseColor(const char *s, uint32_t def) {
    if (!s || !*s) return def;
    if (*s == '#') s++;
    uint32_t v = static_cast<uint32_t>(strtoul(s, nullptr, 16));
    return strlen(s) <= 6 ? v | 0xFF000000u : v;
}
void ReadSettings() {
    S.color = ParseColor(H->config_string(M, "color", "#FFC040"), 0xFFFFC040u);
    S.height = static_cast<float>(H->config_number(M, "height", 2.6));
    S.size = static_cast<float>(H->config_number(M, "size", 0.35));
    S.beam = static_cast<float>(H->config_number(M, "beam", 40));
}

struct Vertex { float x, y, z; uint32_t color; };   // D3DFVF_XYZ | D3DFVF_DIFFUSE

// A diamond (an octahedron) of radius r, its faces shaded by their direction, and a thin beam up from it.
int Build(Vertex *v, float r, float beam, uint32_t argb) {
    auto shade = [&](float k) {
        uint32_t a = argb >> 24, c = argb & 0xFFFFFF;
        uint32_t rr = static_cast<uint32_t>(((c >> 16) & 255) * k), gg = static_cast<uint32_t>(((c >> 8) & 255) * k),
                 bb = static_cast<uint32_t>((c & 255) * k);
        return (a << 24) | (rr << 16) | (gg << 8) | bb;
    };
    const float p[6][3] = {{r, 0, 0}, {-r, 0, 0}, {0, r * 1.6f, 0}, {0, -r * 1.6f, 0}, {0, 0, r}, {0, 0, -r}};
    const int f[8][3] = {{2, 0, 4}, {2, 4, 1}, {2, 1, 5}, {2, 5, 0}, {3, 4, 0}, {3, 1, 4}, {3, 5, 1}, {3, 0, 5}};
    int n = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t c = shade(i < 4 ? (i % 2 ? 0.75f : 1.0f) : (i % 2 ? 0.45f : 0.6f));
        for (int k = 0; k < 3; k++) v[n++] = {p[f[i][k]][0], p[f[i][k]][1], p[f[i][k]][2], c};
    }
    uint32_t top = (argb & 0x00FFFFFFu), low = (argb & 0x00FFFFFFu) | 0x90000000u;   // the beam fades out upwards
    const float w = r * 0.12f, y0 = r * 1.6f;
    const float q[2][2] = {{w, 0}, {0, w}};
    for (int i = 0; i < 2; i++) {   // two crossed quads
        float dx = q[i][0], dz = q[i][1];
        Vertex a = {-dx, y0, -dz, low}, b = {dx, y0, dz, low}, c = {dx, y0 + beam, dz, top}, d = {-dx, y0 + beam, -dz, top};
        v[n++] = a; v[n++] = b; v[n++] = c;
        v[n++] = a; v[n++] = c; v[n++] = d;
    }
    return n;
}

void Draw(IDirect3DDevice9 *d, const V3 &pos, float angle) {
    D3DMATRIX world = {};
    float c = std::cos(angle), s = std::sin(angle);
    world._11 = c; world._13 = -s; world._22 = 1; world._31 = s; world._33 = c; world._44 = 1;
    world._41 = pos.x; world._42 = pos.y; world._43 = pos.z;
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    Vertex v[36];
    int n = Build(v, S.size, S.beam, S.color);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 8, v, sizeof(Vertex));             // the diamond: in the depth buffer
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (n - 24) / 3, v + 24, sizeof(Vertex));   // the beam: see-through
}

bool Where(V3 *pos) {
    if (g_fixed) { *pos = g_at; return true; }
    // a mission's world with the player in it: not while it loads, nor the main menu's (its scene is a world too)
    int state = H->mission_state();
    if ((state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) || !strcmp(H->mission_name(), "00menu")) return false;
    void *p = game::Player();
    if (!p) return false;
    void *at = game::PlayerCar() ? game::PlayerCar() : p;
    *pos = game::ActorWorldPos(at) + V3{0, S.height + (at != p ? 0.6f : 0.0f), 0};
    return true;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { ReadSettings(); return; }
    if (ev->type != MML_EVENT_RENDER_WORLD || !MML_EVENT_HAS(ev, world) || !ev->world) return;
    auto *d = static_cast<IDirect3DDevice9 *>(H->render_device(9));
    if (!d) { g_noD3d9++; return; }
    V3 pos;
    if (!Where(&pos)) return;
    g_cam.size = sizeof g_cam;
    H->render_camera(&g_cam);
    Draw(d, pos, static_cast<float>(GetTickCount() % 4000) / 4000.0f * 6.2831853f);
    g_lastPos = pos;
    g_drawn++;
}

int Status(const char *, MmlReply *r, void *) {
    char out[512];
    snprintf(out, sizeof out,
             "{\"drawn\":%u,\"no_direct3d9\":%u,\"fixed\":%s,\"at\":[%.2f,%.2f,%.2f],\"eye\":[%.2f,%.2f,%.2f],"
             "\"fwd\":[%.3f,%.3f,%.3f],\"fov_y_deg\":%.1f,\"viewport\":[%d,%d]}",
             g_drawn, g_noD3d9, g_fixed ? "true" : "false", g_lastPos.x, g_lastPos.y, g_lastPos.z, g_cam.eye.x, g_cam.eye.y,
             g_cam.eye.z, g_cam.fwd.x, g_cam.fwd.y, g_cam.fwd.z, g_cam.fov_y * 57.29578f, g_cam.width, g_cam.height);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// {"x":..,"y":..,"z":..}: a fixed point; anything else follows the player.
int At(const char *args, MmlReply *r, void *) {
    float v[3];
    const char *keys[3] = {"\"x\"", "\"y\"", "\"z\""};
    bool all = args != nullptr;
    for (int i = 0; i < 3 && all; i++) {
        const char *p = strstr(args, keys[i]);
        all = p && (p = strchr(p, ':')) && sscanf(p + 1, "%f", &v[i]) == 1;
    }
    g_fixed = all;
    if (all) g_at = {v[0], v[1], v[2]};
    return Status(nullptr, r, nullptr);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, render_camera)) return 1;   // a loader without the render events
    // game.h's helpers (the player) read the build's addresses: found once here (both builds)
    if (!game::Detect(H->module_base("LS3DF.dll"))) {
        H->log(M, MML_LOG_ERROR, "not a build of the game whose addresses are known");
        return 2;
    }
    ReadSettings();
    H->subscribe(M, MML_EVENT_RENDER_WORLD, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->command(M, "status", "frames drawn, the camera, where the marker is", Status, nullptr);
    H->command(M, "at", "{x, y, z}: the marker at a fixed point; {}: over the player again", At, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
