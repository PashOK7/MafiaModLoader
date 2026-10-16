// sun_shafts: the sun's light in the air, where the sun reaches it (volumetric light: shafts between the buildings,
// the shadows of cars and trees standing in the haze), an effect over the frame through the graphics bridge
// (docs/MODDING.md "Effects over the frame", "The sun's shadow and the game's lights").
//
// How: the mod asks the host for the shadow map from the sun (shader_shadows), which the bridge hands every pass
// (MmlGfxFrame::sun_shadow, frame.hlsli SunLit). Its pass marches each pixel's ray from the eye to what the pixel
// shows (or to `distance` metres for the sky), asking the map at each step whether the sun reaches that point of the
// air: the light scattered towards the eye there (single scattering: Henyey-Greenstein's phase, Beer-Lambert's
// dimming) adds up. At half the frame's size (a quarter of the pixels), then a blur that does not cross depth edges
// and an upsample that takes each pixel's own depth (sun_shafts.hlsl), so the cost is one march per four pixels and
// the edges of buildings stay sharp. By night there is no sun above the horizon, no map: the pass draws nothing.
//
// The Direct3D 11 objects are the mod's, made on the bridge's device when the pass first runs and again when the
// device or the frame's size changes; released at the unload.
//
// Commands: sun_shafts.status. Settings (config.json): on, the hotkey, the light's intensity and tint, the air's
// density, the anisotropy, the steps, the distance, how much the haze dims what is behind it, the map's size.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "mml/mml.h"
#include "shaders/PsMarch.h"
#include "shaders/PsBlurX.h"
#include "shaders/PsBlurY.h"
#include "shaders/PsComposite.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_pass;

struct Settings {
    bool on = true;
    float intensity = 3.0f, tint[3] = {1, 1, 1}, density = 0.012f, anisotropy = 0.5f, distance = 100.0f, dimming = 0.4f;
    int steps = 32, mapSize = 2048;
    int debug = 0;
} S;

ID3D11PixelShader *g_march, *g_blurX, *g_blurY, *g_composite;
ID3D11Buffer *g_cb;
ID3D11BlendState *g_over;   // premultiplied over the frame, as the bridge composites its target
ID3D11Texture2D *g_lightTex[2], *g_depthTex;
ID3D11RenderTargetView *g_lightRTV[2], *g_depthRTV;
ID3D11ShaderResourceView *g_lightSRV[2], *g_depthSRV;
unsigned g_madeFor, g_w, g_h;   // the device generation and the half size they were made for
unsigned g_frames, g_withMap;
double g_lastMs;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

void ReleaseTargets() {
    for (int i = 0; i < 2; i++) { SafeRelease(g_lightSRV[i]); SafeRelease(g_lightRTV[i]); SafeRelease(g_lightTex[i]); }
    SafeRelease(g_depthSRV); SafeRelease(g_depthRTV); SafeRelease(g_depthTex);
    g_w = g_h = 0;
}
void ReleaseAll() {
    ReleaseTargets();
    SafeRelease(g_march); SafeRelease(g_blurX); SafeRelease(g_blurY); SafeRelease(g_composite);
    SafeRelease(g_cb); SafeRelease(g_over);
}

void Color(const char *key, const char *def, float out[3]) {
    const char *c = H->config_string(M, key, def);
    if (*c == '#') c++;
    unsigned v = static_cast<unsigned>(strtoul(c, nullptr, 16));
    out[0] = ((v >> 16) & 255) / 255.0f;
    out[1] = ((v >> 8) & 255) / 255.0f;
    out[2] = (v & 255) / 255.0f;
}

// The shadow map asked for while the effect is on, as large as its rays go.
void AskShadows() {
    if (!S.on) { H->shader_shadows(M, nullptr); return; }
    MmlShadowSettings s{S.mapSize, S.distance, 0.0f};   // (bias 0: the host's own)
    H->shader_shadows(M, &s);
}

void ReadSettings() {
    S.on = H->config_number(M, "on", 1) != 0;
    S.intensity = static_cast<float>(H->config_number(M, "intensity", 3.0));
    Color("tint", "#FFFFFF", S.tint);
    S.density = static_cast<float>(H->config_number(M, "density", 0.012));
    S.anisotropy = static_cast<float>(H->config_number(M, "anisotropy", 0.5));
    if (S.anisotropy > 0.95f) S.anisotropy = 0.95f;
    if (S.anisotropy < -0.95f) S.anisotropy = -0.95f;
    S.steps = static_cast<int>(H->config_number(M, "steps", 32));
    if (S.steps < 4) S.steps = 4;
    if (S.steps > 128) S.steps = 128;
    S.distance = static_cast<float>(H->config_number(M, "distance", 100));
    if (S.distance < 10) S.distance = 10;
    S.dimming = static_cast<float>(H->config_number(M, "dimming", 0.4));
    S.mapSize = static_cast<int>(H->config_number(M, "shadow_map", 2048));
    S.debug = static_cast<int>(H->config_number(M, "debug", 0));
    AskShadows();
}

bool MakeShaders(ID3D11Device *d) {
    ReleaseAll();
    if (FAILED(d->CreatePixelShader(kPsMarch, sizeof kPsMarch, nullptr, &g_march)) ||
        FAILED(d->CreatePixelShader(kPsBlurX, sizeof kPsBlurX, nullptr, &g_blurX)) ||
        FAILED(d->CreatePixelShader(kPsBlurY, sizeof kPsBlurY, nullptr, &g_blurY)) ||
        FAILED(d->CreatePixelShader(kPsComposite, sizeof kPsComposite, nullptr, &g_composite)))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &g_cb))) return false;
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(d->CreateBlendState(&b, &g_over));
}

bool MakeTarget(ID3D11Device *d, DXGI_FORMAT f, ID3D11Texture2D **t, ID3D11RenderTargetView **rtv, ID3D11ShaderResourceView **srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_w;
    td.Height = g_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(d->CreateTexture2D(&td, nullptr, t)) && SUCCEEDED(d->CreateRenderTargetView(*t, nullptr, rtv)) &&
           SUCCEEDED(d->CreateShaderResourceView(*t, nullptr, srv));
}

bool MakeTargets(ID3D11Device *d, unsigned w, unsigned h) {
    ReleaseTargets();
    g_w = w;
    g_h = h;
    bool ok = MakeTarget(d, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_lightTex[0], &g_lightRTV[0], &g_lightSRV[0]) &&
              MakeTarget(d, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_lightTex[1], &g_lightRTV[1], &g_lightSRV[1]) &&
              MakeTarget(d, DXGI_FORMAT_R32_FLOAT, &g_depthTex, &g_depthRTV, &g_depthSRV);
    if (!ok) ReleaseTargets();
    return ok;
}

// One full-screen draw into `rtv` (w x h) with `ps`, reading `scatter` at t3 and the half depth at t4.
void Draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, unsigned w, unsigned h, ID3D11ShaderResourceView *scatter) {
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(3, 2, none);   // (a target is never bound as a texture at once)
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView *in[2] = {scatter, g_depthSRV};
    ctx->PSSetShaderResources(3, 2, in);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
}

void Pass(const MmlGfxFrame *f, void *) {
    if (!S.on || !MML_GFX_HAS(f, sun_shadow) || !f->sun_shadow) return;   // no map this frame: nothing to do
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_march) {
        if (!MakeShaders(d)) { ReleaseAll(); return; }
        g_madeFor = f->generation;
    }
    unsigned w = (f->width + 1) / 2, h = (f->height + 1) / 2;
    if ((w != g_w || h != g_h || !g_lightTex[0]) && !MakeTargets(d, w, h)) return;
    const MmlGfxConstants *k = f->k;
    const float cb[16] = {k->sun_color[0] * S.tint[0] * S.intensity, k->sun_color[1] * S.tint[1] * S.intensity,
                          k->sun_color[2] * S.tint[2] * S.intensity, S.density,
                          static_cast<float>(S.steps), S.distance, S.anisotropy, S.dimming,
                          static_cast<float>(w), static_cast<float>(h), 1.0f / w, 1.0f / h,
                          static_cast<float>(S.debug), 0, 0, 0};
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    // 1. the march: light and transmittance into [0], the depth it used into the depth target
    {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(3, 2, none);
        ID3D11RenderTargetView *mrt[2] = {g_lightRTV[0], g_depthRTV};
        ctx->OMSetRenderTargets(2, mrt, nullptr);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(g_march, nullptr, 0);
        ctx->Draw(3, 0);
    }
    // 2. the blur across, then down
    Draw(ctx, g_blurX, g_lightRTV[1], w, h, g_lightSRV[0]);
    Draw(ctx, g_blurY, g_lightRTV[0], w, h, g_lightSRV[1]);
    // 3. over the frame, at its size, into the bridge's target as the bridge blends
    ctx->OMSetBlendState(g_over, nullptr, 0xFFFFFFFFu);
    Draw(ctx, g_composite, static_cast<ID3D11RenderTargetView *>(f->target), f->width, f->height, g_lightSRV[0]);
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(3, 2, none);
    g_frames++;
    g_withMap++;
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    g_lastMs = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
}
void OnHotkey(void *) { H->config_set(M, "on", S.on ? "false" : "true"); }   // (CONFIG_CHANGED reads it back)

int Status(const char *, MmlReply *r, void *) {
    char out[320];
    snprintf(out, sizeof out,
             "{\"on\":%s,\"pass\":%d,\"frames\":%u,\"half\":[%u,%u],\"cpu_ms\":%.3f,\"generation\":%u,\"intensity\":%.2f,\"density\":%.4f,"
             "\"anisotropy\":%.2f,\"steps\":%d,\"distance\":%.0f}",
             S.on ? "true" : "false", g_pass, g_frames, g_w, g_h, g_lastMs, g_madeFor, S.intensity, S.density, S.anisotropy, S.steps,
             S.distance);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, shader_shadows)) return 1;   // a loader without the sun's shadow map
    g_pass = H->gfx_pass(M, 50, Pass, nullptr);   // (early among the passes: the light is in the air, under what is laid on the frame)
    if (!g_pass) return 3;
    ReadSettings();
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->hotkey(M, H->config_string(M, "hotkey", "F7"), OnHotkey, nullptr);
    H->command(M, "status", "on or off, frames drawn, the half size, the pass's CPU time, the settings", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    ReleaseAll();   // (the pass and the shadow map's wish are taken back by the loader)
}
