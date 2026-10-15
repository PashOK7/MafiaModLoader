// hud_panel: the frame's 2D pass taken by a mod (the graphics bridge's gfx_ui; docs/MODDING.md "Effects over the
// frame"): the HUD and the menus shown on a panel in the 3D world instead of over the screen, as a VR mod shows them.
//
// What it does: gfx_ui(MML_GFX_UI_HIDE) takes the 2D pass off the frame; the mod's pass (hud_panel.hlsl) draws it on a
// panel that floats ahead and to the right of the camera, turned towards it, hidden by what stands in front of it.
// `hud_panel.place` leaves the panel where it is in the world (walk around it); `hud_panel.follow` brings it back to
// the camera; `hud_panel.mode {"mode": "take"}` keeps the HUD on the screen too ("hide" again: only on the panel).
// The 2D pass is drawn after the passes, so the panel shows the last frame's.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mml/mml.h"
#include "shaders/PsPanel.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_pass, g_mode = MML_GFX_UI_HIDE;
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
unsigned g_madeFor, g_frames, g_noUi;
bool g_fixed;
float g_panel[16];   // centre, u, v, n (as the shader's cbuffer)

struct Settings { float distance = 2.6f, side = 0.9f, width = 1.6f, turn = 25.0f, backing = 0.55f; } S;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

void ReadSettings() {
    S.distance = static_cast<float>(H->config_number(M, "distance", 2.6));
    S.side = static_cast<float>(H->config_number(M, "side", 0.9));
    S.width = static_cast<float>(H->config_number(M, "width", 1.6));
    S.turn = static_cast<float>(H->config_number(M, "turn", 25));
    S.backing = static_cast<float>(H->config_number(M, "backing", 0.55));
}

// The panel ahead of the camera, to its right, turned towards it by `turn` degrees about the world's up.
void Follow(const MmlGfxConstants &k, float aspect) {
    const float *e = k.eye, *r = k.right, *f = k.fwd;
    float c[3], fl[3] = {f[0], 0, f[2]}, len = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (len < 1e-4f) { fl[0] = 0; fl[2] = 1; len = 1; }
    for (int i = 0; i < 3; i++) c[i] = e[i] + f[i] * S.distance + r[i] * S.side;
    float a = S.turn * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
    float n[3] = {-(fl[0] * ca - fl[2] * sa) / len, 0, -(fl[0] * sa + fl[2] * ca) / len};   // towards the camera, turned
    float u[3] = {n[2], 0, -n[0]};   // its right: up x n... (y up): perpendicular in the ground plane
    float hw = S.width * 0.5f, hh = hw / aspect;
    // u must point to the viewer's right: flip it when it points away from the camera's right
    if (u[0] * r[0] + u[2] * r[2] < 0) { u[0] = -u[0]; u[2] = -u[2]; }
    float panel[16] = {c[0], c[1], c[2], S.backing, u[0] * hw, 0, u[2] * hw, 0, 0, hh, 0, 0, n[0], n[1], n[2], 0};
    memcpy(g_panel, panel, sizeof panel);
}

void Pass(const MmlGfxFrame *f, void *) {
    if (!MML_GFX_HAS(f, ui_frames)) return;
    if (!f->ui) { g_noUi++; return; }
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        SafeRelease(g_ps);
        SafeRelease(g_cb);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof g_panel;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(d->CreatePixelShader(kPsPanel, sizeof kPsPanel, nullptr, &g_ps)) || FAILED(d->CreateBuffer(&bd, nullptr, &g_cb))) return;
        g_madeFor = f->generation;
    }
    if (!g_fixed) Follow(*f->k, static_cast<float>(f->width) / static_cast<float>(f->height));
    g_panel[3] = S.backing;
    ctx->UpdateSubresource(g_cb, 0, nullptr, g_panel, 0, 0);
    ID3D11ShaderResourceView *ui = static_cast<ID3D11ShaderResourceView *>(f->ui);
    ctx->PSSetShaderResources(2, 1, &ui);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);
    g_frames++;
}

void OnEvent(const MmlEvent *ev, void *) { if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings(); }

int Reply(MmlReply *r) {
    char out[256];
    snprintf(out, sizeof out, "{\"frames\":%u,\"no_ui\":%u,\"mode\":\"%s\",\"fixed\":%s,\"center\":[%.2f,%.2f,%.2f]}", g_frames, g_noUi,
             g_mode == MML_GFX_UI_HIDE ? "hide" : "take", g_fixed ? "true" : "false", g_panel[0], g_panel[1], g_panel[2]);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Status(const char *, MmlReply *r, void *) { return Reply(r); }
int Place(const char *, MmlReply *r, void *) { g_fixed = true; return Reply(r); }
int FollowCmd(const char *, MmlReply *r, void *) { g_fixed = false; return Reply(r); }
int Mode(const char *args, MmlReply *r, void *) {
    g_mode = args && strstr(args, "take") ? MML_GFX_UI_TAKE : MML_GFX_UI_HIDE;
    H->gfx_ui(M, g_mode);
    return Reply(r);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, gfx_ui)) return 1;   // a loader without the 2D pass for mods
    ReadSettings();
    g_pass = H->gfx_pass(M, 200, Pass, nullptr);   // (after other effects: the panel over them)
    H->gfx_ui(M, g_mode);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->command(M, "status", "frames drawn, the mode, where the panel is", Status, nullptr);
    H->command(M, "place", "the panel stays where it is in the world", Place, nullptr);
    H->command(M, "follow", "the panel follows the camera again", FollowCmd, nullptr);
    H->command(M, "mode", "{\"mode\": \"hide\"}: the HUD only on the panel; \"take\": on the screen too", Mode, nullptr);
    return g_pass ? 0 : 2;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
}
