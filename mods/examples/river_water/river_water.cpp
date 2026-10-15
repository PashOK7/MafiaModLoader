// river_water: the game's own water replaced (geom_hide, geom_capture, scene_view; docs/MODDING.md "The game's own
// geometry hidden or replaced").
//
// The city's river is one plane LS3D draws with the animated texture 00TEST01..20 (geom.survey shows it). This mod:
// - hides it from every drawing (the frame and the scene views) by its texture's name;
// - captures its triangles (the world's space) from what LS3D draws, at the load and at each mission's load, again
//   every two seconds until it has them;
// - draws a reflection of the scene from the camera mirrored in the water's plane (a scene view, the plane as its
//   clip plane);
// - draws its own water on those triangles in a graphics pass: waves, the reflection bent by them, the riverbed seen
//   through the water darker the deeper it lies, the sun's glint, the world's fog. What stands in front of the water
//   hides it (the frame's depth).
// `river_water.status`; settings: texture, waves, reflection, deep, deep_metres, shallow, bend.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "shaders/VsWater.h"
#include "shaders/PsWater.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_pass, g_view, g_hide;

struct Settings {
    std::string texture = "00test*";
    float waves = 1, deepMetres = 4, bend = 18;
    float deep[3] = {0.06f, 0.16f, 0.17f}, shallow[3] = {0.55f, 0.66f, 0.6f};
    bool reflection = true;
} S;

std::vector<float> g_tris;      // the water's triangles (the world), three points each
float g_level;                  // the water's level: its triangles' mean height
bool g_have;
unsigned g_captures, g_groups;
DWORD g_nextAsk;
bool g_asked;

ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11InputLayout *g_layout;
ID3D11Buffer *g_vb, *g_cb;
ID3D11RasterizerState *g_rs;
unsigned g_madeFor, g_vbVersion, g_trisVersion, g_drawn;
LARGE_INTEGER g_t0;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

void Colour(const char *key, float *out, const float *def) {
    // "r g b" (0..1) or nothing
    std::string s = H->config_string(M, key, "");
    float r, g, b;
    if (sscanf(s.c_str(), "%f %f %f", &r, &g, &b) == 3) { out[0] = r; out[1] = g; out[2] = b; }
    else memcpy(out, def, 3 * sizeof(float));
}

void ReadSettings() {
    std::string tex = H->config_string(M, "texture", "00test*");
    S.waves = static_cast<float>(H->config_number(M, "waves", 1));
    S.deepMetres = static_cast<float>(H->config_number(M, "deep_metres", 4));
    S.bend = static_cast<float>(H->config_number(M, "bend", 18));
    S.reflection = H->config_number(M, "reflection", 1) != 0;
    const float deep[3] = {0.06f, 0.16f, 0.17f}, shallow[3] = {0.55f, 0.66f, 0.6f};
    Colour("deep", S.deep, deep);
    Colour("shallow", S.shallow, shallow);
    if (tex != S.texture || !g_hide) {
        S.texture = tex;
        if (g_hide) H->geom_show(M, g_hide);
        g_hide = H->geom_hide(M, S.texture.c_str(), MML_GEOM_FRAME | MML_GEOM_VIEWS);
        g_have = false;
        g_asked = false;
    }
}

void OnCapture(const MmlGeomCapture *c, void *) {
    g_asked = false;
    g_captures++;
    if (!c->count) return;   // (none drawn that frame: asked again)
    g_tris.assign(c->tris, c->tris + c->count * 9);
    double y = 0;
    for (unsigned i = 0; i < c->count * 3; i++) y += c->tris[i * 3 + 1];
    g_level = static_cast<float>(y / (c->count * 3));
    g_groups = c->group_count;
    g_have = true;
    g_trisVersion++;
}

void Ask() {
    if (g_asked) return;
    g_asked = H->geom_capture(M, S.texture.c_str(), OnCapture, nullptr) != 0;
    g_nextAsk = GetTickCount() + 2000;
}

// The camera mirrored in the water's plane: only what is above the water drawn (the sky is not cut: the loader).
int PlaceReflection(MmlViewCamera *c, void *) {
    if (!g_have || !S.reflection) return 0;
    if (c->world[13] < g_level + 0.05f) return 0;   // (the camera under the water)
    c->world[13] = 2 * g_level - c->world[13];
    c->world[5] = -c->world[5];
    c->world[9] = -c->world[9];
    c->flags |= MML_VIEW_CLIP;
    c->clip[0] = 0;
    c->clip[1] = 1;
    c->clip[2] = 0;
    c->clip[3] = -(g_level + 0.02f);
    return 1;
}

void Mul44(const float *a, const float *b, float *out) {   // out = a * b (row vectors)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

bool Make(ID3D11Device *d) {
    SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_layout); SafeRelease(g_cb); SafeRelease(g_rs); SafeRelease(g_vb);
    g_vbVersion = 0;
    D3D11_INPUT_ELEMENT_DESC el{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 176;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // (the captured triangles' winding is the game's)
    rd.DepthClipEnable = TRUE;
    return SUCCEEDED(d->CreateVertexShader(kVsWater, sizeof kVsWater, nullptr, &g_vs)) &&
           SUCCEEDED(d->CreatePixelShader(kPsWater, sizeof kPsWater, nullptr, &g_ps)) &&
           SUCCEEDED(d->CreateInputLayout(&el, 1, kVsWater, sizeof kVsWater, &g_layout)) &&
           SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb)) && SUCCEEDED(d->CreateRasterizerState(&rd, &g_rs));
}

void Pass(const MmlGfxFrame *f, void *) {
    if (MML_GFX_HAS(f, in_view) && f->in_view) return;   // (the frame's only)
    if (!g_have) { if (!g_asked && GetTickCount() >= g_nextAsk) Ask(); return; }
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        if (!Make(d)) return;
        g_madeFor = f->generation;
    }
    if (g_vbVersion != g_trisVersion) {
        SafeRelease(g_vb);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(g_tris.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{g_tris.data(), 0, 0};
        if (FAILED(d->CreateBuffer(&bd, &init, &g_vb))) return;
        g_vbVersion = g_trisVersion;
    }
    // the reflection drawn for this frame
    const MmlGfxView *refl = nullptr;
    if (MML_GFX_HAS(f, views))
        for (unsigned i = 0; i < f->view_count; i++)
            if (f->views[i].handle == g_view && f->views[i].fresh) refl = &f->views[i];
    float cb[44] = {};
    Mul44(f->view, f->proj, cb);
    if (refl) Mul44(refl->view, refl->proj, cb + 16);
    LARGE_INTEGER t, fq;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&fq);
    cb[32] = g_level;
    cb[33] = S.waves;
    cb[34] = refl ? 1.0f : 0.0f;
    cb[35] = static_cast<float>(static_cast<double>(t.QuadPart - g_t0.QuadPart) / fq.QuadPart);
    memcpy(cb + 36, S.deep, sizeof S.deep);
    cb[39] = S.deepMetres;
    memcpy(cb + 40, S.shallow, sizeof S.shallow);
    cb[43] = S.bend;
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    UINT stride = 12, offset = 0;
    ctx->IASetInputLayout(g_layout);
    ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *cbs[2] = {static_cast<ID3D11Buffer *>(f->constants), g_cb};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ID3D11ShaderResourceView *srv = refl ? static_cast<ID3D11ShaderResourceView *>(refl->color) : nullptr;
    ctx->PSSetShaderResources(2, 1, &srv);
    ctx->RSSetState(g_rs);
    ctx->Draw(static_cast<UINT>(g_tris.size() / 3), 0);
    g_drawn++;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { ReadSettings(); return; }
    if (ev->type == MML_EVENT_MISSION_LOADED) { g_have = false; g_tris.clear(); g_trisVersion++; Ask(); }
}

int Status(const char *, MmlReply *r, void *) {
    MmlFrameStats fs{};
    fs.size = sizeof fs;
    if (!MML_HAS(H, frame_stats) || !H->frame_stats(M, &fs)) fs.frames = 0;
    char out[640];
    snprintf(out, sizeof out,
             "{\"texture\":\"%s\",\"hidden\":%d,\"have\":%s,\"triangles\":%u,\"groups\":%u,\"level\":%.3f,\"captures\":%u,"
             "\"reflection_view\":%d,\"drawn\":%u,\"frame\":{\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"mod_ms\":%.3f,\"views_ms\":%.2f,"
             "\"bridge_ms\":%.2f}}",
             S.texture.c_str(), g_hide, g_have ? "true" : "false", static_cast<unsigned>(g_tris.size() / 9), g_groups, g_level,
             g_captures, g_view, g_drawn, fs.avg_ms, fs.p95_ms, fs.mod_ms, fs.views_ms, fs.bridge_ms);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, geom_capture)) return 1;   // a loader without the game's geometry
    QueryPerformanceCounter(&g_t0);
    ReadSettings();
    if (!g_hide) return 2;
    g_pass = H->gfx_pass(M, 120, Pass, nullptr);   // (after depth_pulse: the water covers the riverbed and its wave)
    g_view = H->scene_view(M, 0, 0, PlaceReflection, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_MISSION_LOADED, OnEvent, nullptr);
    H->command(M, "status", "the water: hidden, its triangles, its level, the reflection, frames drawn", Status, nullptr);
    Ask();
    return g_pass ? 0 : 3;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_layout); SafeRelease(g_vb); SafeRelease(g_cb); SafeRelease(g_rs);
}
