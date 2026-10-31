// river_water: the game's own water replaced (geom_hide, geom_capture, scene_view, shader_shadows; docs/MODDING.md
// "The game's own geometry hidden or replaced"), drawn by the SDK's water: mml/water.h (the game's water's names, its
// planes, the ripples, the reflection's copy), mml/gfx/water.hlsli (the shading), mml/gfx/water_waves.hlsli (the
// ripples' kernels). knowledge/notes/water.md has the findings behind it.
//
// - The game's water (00TEST01..20) and the shade it lays on it under the bridges (the frames watercity.stin*) are
//   hidden from every drawing (the frame and the scene views);
// - the water's triangles are captured from what LS3D draws, every few seconds (a capture holds only what the frame
//   drew), kept by group and sorted into planes by their level (the river at -9.99, a fountain at 2.41);
// - the reflection is a scene view of the camera mirrored in the plane the screen showed most of (an occlusion query
//   a plane counts its pixels; none on the screen: no reflection drawn, the second drawing of the scene saved);
// - the wind's ripples are made each frame on the GPU (a JONSWAP spectrum, three cascades, an inverse FFT, the slopes'
//   mipmaps), only while water is on the screen;
// - a graphics pass draws the water on the captured triangles (river_water.hlsl): the reflection, the body lit and
//   shadowed from the visible sun by default (or the scene light, selected in settings), the sun scattered in the water
//   and its glitter towards the sun the sky shows (scene_sun_seen: its lens flare, elsewhere than that light).
// `river_water.status`; settings (mod.json: the game's Options > Mods): on, reflection, wind, wind_from, waves,
// clarity, depth, scattering, crests, glitter, foam; config.json also: texture, shade, fetch, colour, tint,
// phase, shadows, shadow_source (0 visible sun, 1 scene light), debug.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "mml/mml.h"
#include "mml/water.h"
#include "mml/water_mesh.h"
#include "shaders/VsWater.h"
#include "shaders/HsWater.h"
#include "shaders/DsWater.h"
#include "shaders/PsWater.h"
#include "shaders/CsPaintedSky.h"
#include "shaders/CsSpectrum.h"
#include "shaders/CsFftRows.h"
#include "shaders/CsFftCols.h"
#include "shaders/CsResolve.h"

namespace {
namespace mw = mml::water;
const MmlHost *H;
MmlMod *M;
int g_pass, g_view, g_hide, g_hideShade;

constexpr int kMaxPlanes = 16;
constexpr int kRing = 4;   // occlusion queries in flight a plane

struct Settings {
    bool on = true, reflection = true, shadows = true;
    int shadowSource = 0;       // 0: visible sky sun (default); 1: scene's directional light
    float shadowSoftness = 1;   // solar angular radius multiplier; penumbra grows with blocker/receiver separation
    bool geometry = true;
    float waveHeight = 1;
    std::string texture = mw::kGameWater, shade = mw::kGameShade;
    float wind = 1, windFrom = 200, fetch = 800, waves = 1, clarity = 2.5f, depth = 10, glitter = 1, foam = 0.5f;
    float scattering = 0.6f, phase = 0.75f, crests = 1, chop = 1, sunRadius = 18;
    int debug = 0;              // the shader's debug view (river_water.hlsl g_light.z)
    float colour[3], tint[3];   // linear: the light the body scatters of white light; white light after `clarity` m
} S;

mw::Surfaces g_water;
std::vector<float> g_patches;
std::vector<mw::Plane> g_patchPlanes;
unsigned long long g_pixels[kMaxPlanes];   // each plane's pixels at its last count (~0: not counted yet)
unsigned g_captures;
DWORD g_nextAsk;
bool g_asked;
int g_reflPlane = -1;                      // the plane the reflection was placed for this frame
float g_reflLevel;

mw::Spectrum g_spectrum;
unsigned g_specVersion = 1;

struct Gpu {
    ID3D11VertexShader *vs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11PixelShader *ps;
    ID3D11ComputeShader *paintedSky;
    ID3D11Texture2D *skyTexture;
    ID3D11ShaderResourceView *skyView;
    ID3D11UnorderedAccessView *skyOut;
    unsigned skyW, skyH;
    bool skyReady;
    ID3D11InputLayout *layout;
    ID3D11Buffer *cb, *vb;
    ID3D11RasterizerState *rs, *wire;
    ID3D11Texture2D *depth;
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilState *depthState;
    unsigned depthW, depthH;
    ID3D11Query *stats[kRing];
    bool statsIssued[kRing];
    D3D11_QUERY_DATA_PIPELINE_STATISTICS geometryStats;
    ID3D11Query *disjoint[kRing], *stamp[kRing][3];
    bool timingIssued[kRing];
    float fftMs, surfaceMs;
    ID3D11SamplerState *wrap, *clamp;
    ID3D11Query *query[kRing][kMaxPlanes];
    bool issued[kRing][kMaxPlanes];
    unsigned queryEpoch[kRing][kMaxPlanes];
    unsigned madeFor, vbVersion, frame;
    bool made;
} G;
mw::Waves g_waves;
mw::ReflectionCopy g_refl;
unsigned g_drawn, g_reflDrawn;
unsigned g_skyDrawn;
unsigned g_offscreen, g_reflectionRejected;
float g_visibilityPose[16];
bool g_haveVisibilityPose;
unsigned g_visibilityEpoch;

bool VisibleWaterPlane(const mw::Plane &plane, const float vp[16], float projX, float projY);
void WaterViewProjection(const MmlViewCamera &camera, float vp[16]);
float g_rview[16], g_rproj[16];   // the last reflection's (status)
LARGE_INTEGER g_t0, g_freq;
float g_previousVP[16], g_previousEye[4];
bool g_haveHistory;
unsigned g_temporalDrawn;

using mw::SafeRelease;

void Release() {
    SafeRelease(G.paintedSky); SafeRelease(G.skyOut); SafeRelease(G.skyView); SafeRelease(G.skyTexture);
    G.skyW = G.skyH = 0; G.skyReady = false;
    g_haveHistory = false;
    SafeRelease(G.vs); SafeRelease(G.hs); SafeRelease(G.ds); SafeRelease(G.ps);
    SafeRelease(G.layout); SafeRelease(G.cb); SafeRelease(G.vb);
    SafeRelease(G.rs); SafeRelease(G.wire); SafeRelease(G.wrap); SafeRelease(G.clamp);
    SafeRelease(G.dsv); SafeRelease(G.depth); SafeRelease(G.depthState);
    G.depthW = G.depthH = 0;
    for (auto &q : G.stats) SafeRelease(q);
    for (auto &q : G.disjoint) SafeRelease(q);
    for (auto &row : G.stamp) for (auto &q : row) SafeRelease(q);
    memset(G.timingIssued, 0, sizeof G.timingIssued);
    memset(G.statsIssued, 0, sizeof G.statsIssued);
    for (auto &ring : G.query)
        for (auto &q : ring) SafeRelease(q);
    memset(G.issued, 0, sizeof G.issued);
    G.made = false;
    G.vbVersion = 0;
    g_waves.Release();
    g_refl.Release();
}

float Lin(float c) { return std::pow(c, 2.2f); }

// "#rrggbb" into linear light
void Colour(const char *key, const char *def, float out[3]) {
    const char *c = H->config_string(M, key, def);
    if (*c == '#') c++;
    unsigned v = static_cast<unsigned>(strtoul(c, nullptr, 16));
    out[0] = Lin(((v >> 16) & 255) / 255.0f);
    out[1] = Lin(((v >> 8) & 255) / 255.0f);
    out[2] = Lin((v & 255) / 255.0f);
}

void UpdateShadowRequest();
void Forget() {
    g_haveHistory = false;
    g_haveVisibilityPose = false;
    g_water.Clear();
    for (auto &p : g_pixels) p = ~0ull;
    g_asked = false;
    g_nextAsk = 0;
    UpdateShadowRequest();
}

// The game's water shown (and its shade) or hidden from every drawing.
void ShowWater(bool show) {
    if (show && g_hide) { H->geom_show(M, g_hide); g_hide = 0; }
    if (show && g_hideShade) { H->geom_show(M, g_hideShade); g_hideShade = 0; }
    if (!show && !g_hide) {
        g_hide = H->geom_hide(M, S.texture.c_str(), MML_GEOM_FRAME | MML_GEOM_VIEWS);
        if (!S.shade.empty()) g_hideShade = H->geom_hide(M, S.shade.c_str(), MML_GEOM_FRAME | MML_GEOM_VIEWS);
        Forget();
    }
}

void UpdateShadowRequest() {
    if (!MML_HAS(H, shader_shadows)) return;
    MmlShadowSettings sh{2048, 120.0f, 0.0f};
    const auto *settings = S.on && S.shadows ? &sh : nullptr;
    const int source = S.shadowSource == 0 ? MML_SHADOW_SUN : MML_SHADOW_SCENE;
    if (MML_HAS(H, shader_shadows_fit) && !g_water.planes.empty()) {
        float minY = g_water.planes[0].level, maxY = minY;
        for (const auto &p : g_water.planes) { minY = std::min(minY, p.level); maxY = std::max(maxY, p.level); }
        const float bound = std::max(0.05f, 6 * std::sqrt(g_spectrum.heightVar) * std::fabs(S.waves * S.waveHeight) * (1 + S.chop));
        if (MML_HAS(H, shader_shadows_surface))
            H->shader_shadows_surface(M, settings, source, g_water.tris.data(), static_cast<unsigned>(g_water.tris.size()/9), bound);
        else H->shader_shadows_fit(M, settings, source, minY - bound, maxY + bound);
    } else if (MML_HAS(H, shader_shadows_from)) H->shader_shadows_from(M, settings, source);
    else H->shader_shadows(M, settings);
}

void ReadSettings() {
    g_haveHistory = false;   // height/material changes are not the preceding surface
    g_haveVisibilityPose = false; // changed displacement can expose water hidden from the preceding queries
    float wind = S.wind, from = S.windFrom, fetch = S.fetch;
    std::string tex = H->config_string(M, "texture", mw::kGameWater);
    std::string shade = H->config_string(M, "shade", mw::kGameShade);
    S.on = H->config_number(M, "on", 1) != 0;
    S.geometry = H->config_number(M, "geometry", 1) != 0;
    S.waveHeight = static_cast<float>(H->config_number(M, "wave_height", 1));
    if (!(S.waveHeight >= 0)) S.waveHeight = 0;
    if (S.waveHeight > 8) S.waveHeight = 8;
    S.reflection = H->config_number(M, "reflection", 1) != 0;
    S.shadows = H->config_number(M, "shadows", 1) != 0;
    S.shadowSource = H->config_number(M, "shadow_source", 0) == 1 ? 1 : 0;
    S.shadowSoftness = static_cast<float>(H->config_number(M, "shadow_softness", 1));
    if (!(S.shadowSoftness >= 0)) S.shadowSoftness = 0;
    if (S.shadowSoftness > 8) S.shadowSoftness = 8;
    S.wind = static_cast<float>(H->config_number(M, "wind", 1));
    S.windFrom = static_cast<float>(H->config_number(M, "wind_from", 200));
    S.fetch = static_cast<float>(H->config_number(M, "fetch", 800));
    S.waves = static_cast<float>(H->config_number(M, "waves", 1));
    S.clarity = static_cast<float>(H->config_number(M, "clarity", 2.5));
    S.depth = static_cast<float>(H->config_number(M, "depth", 10));
    if (!(S.depth >= 0.1f)) S.depth = 0.1f;
    if (S.depth > 1000) S.depth = 1000;
    S.glitter = static_cast<float>(H->config_number(M, "glitter", 1));
    S.sunRadius = static_cast<float>(H->config_number(M, "sun_radius", 18));
    if (!(S.sunRadius >= 0.5f)) S.sunRadius = 0.5f;
    if (S.sunRadius > 60) S.sunRadius = 60;
    S.foam = std::clamp(static_cast<float>(H->config_number(M, "foam", 0.5)), 0.0f, 2.0f);
    S.scattering = static_cast<float>(H->config_number(M, "scattering", 0.6));
    S.phase = static_cast<float>(H->config_number(M, "phase", 0.75));
    if (S.phase < 0) S.phase = 0;
    if (S.phase > 0.95f) S.phase = 0.95f;
    S.crests = static_cast<float>(H->config_number(M, "crests", 1));
    S.chop = static_cast<float>(H->config_number(M, "chop", 1));
    S.debug = static_cast<int>(H->config_number(M, "debug", 0));
    Colour("colour", "#2c4740", S.colour);
    Colour("tint", "#6e9c80", S.tint);
    if (wind != S.wind || from != S.windFrom || fetch != S.fetch || g_spectrum.h0.empty()) {
        g_spectrum = mw::MakeSpectrum(S.wind, S.windFrom, S.fetch);
        g_specVersion++;
    }
    if (tex != S.texture || shade != S.shade) { ShowWater(true); S.texture = tex; S.shade = shade; }
    ShowWater(!S.on);
    UpdateShadowRequest();
}

void OnCapture(const MmlGeomCapture *c, void *) {
    g_asked = false;
    g_captures++;
    bool changed = false;
    for (unsigned i = 0; i < c->group_count; i++) {
        const MmlGeomGroup &gr = c->groups[i];
        changed = g_water.Add(gr.visual, gr.group, c->tris + static_cast<size_t>(gr.first) * 9, gr.count) || changed;
    }
    if (!changed) return;
    g_water.Sort();
    UpdateShadowRequest();
    for (auto &p : g_pixels) p = ~0ull;   // (the planes changed: their counts start again)
}

void Ask() {
    if (g_asked || !g_hide) return;
    g_asked = H->geom_capture(M, S.texture.c_str(), OnCapture, nullptr) != 0;
    g_nextAsk = GetTickCount() + (g_water.planes.empty() ? 1000 : 3000);
}

// The reflection: the camera mirrored in the plane the screen showed most of at the last count (none on the screen:
// not drawn). Only what is above the water is drawn (the sky is not cut: the loader).
int PlaceReflection(MmlViewCamera *c, void *) {
    g_reflPlane = -1;
    if (!S.on) return 0;
    float vp[16]; WaterViewProjection(*c, vp);
    bool moved = !g_haveVisibilityPose || memcmp(g_visibilityPose, c->world, sizeof g_visibilityPose) != 0;
    memcpy(g_visibilityPose, c->world, sizeof g_visibilityPose); g_haveVisibilityPose = true;
    if (moved) { ++g_visibilityEpoch; for (auto &pixels : g_pixels) pixels = ~0ull; }
    if (!S.reflection) return 0;
    int best = -1;
    unsigned long long most = 0;
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        if (c->world[13] < g_water.planes[p].level + 0.05f ||
            !VisibleWaterPlane(g_water.planes[p], vp, 2 / (c->tan_left + c->tan_right), 2 / (c->tan_up + c->tan_down))) continue;
        // Old zero samples are not evidence about a new camera. The conservative geometry test permits recovery
        // immediately, while an unchanged view can still use the asynchronous occlusion result.
        if (!moved && !g_pixels[p]) continue;
        auto pixels = std::max<unsigned long long>(g_pixels[p], moved ? 1 : 0);
        if (best < 0 || pixels > most) best = static_cast<int>(p), most = pixels;
    }
    if (best < 0) { g_reflectionRejected++; return 0; }
    g_reflLevel = g_water.planes[best].level;
    mw::MirrorCamera(c->world, c->clip, g_reflLevel);
    c->flags |= MML_VIEW_CLIP;
    g_reflPlane = best;
    g_reflDrawn++;
    return 1;
}

void Mul44(const float *a, const float *b, float *out) {   // out = a * b (row vectors)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

// out = m^-1 (4x4, Gauss-Jordan with partial pivoting); false: singular
bool Inverse44(const float *m, float *out) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(a[c][j], a[p][j]);
        double inv = 1 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<float>(a[i][j + 4]);
    return true;
}

void WaterViewProjection(const MmlViewCamera &c, float vp[16]) {
    const float *w = c.world;
    float view[16] = {w[0],w[4],w[8],0, w[1],w[5],w[9],0, w[2],w[6],w[10],0,
        -(w[12]*w[0]+w[13]*w[1]+w[14]*w[2]), -(w[12]*w[4]+w[13]*w[5]+w[14]*w[6]),
        -(w[12]*w[8]+w[13]*w[9]+w[14]*w[10]), 1};
    float lr = c.tan_left + c.tan_right, ud = c.tan_up + c.tan_down;
    float z = c.far_z / (c.far_z - c.near_z);
    float proj[16] = {2/lr,0,0,0, 0,2/ud,0,0, (c.tan_left-c.tan_right)/lr,(c.tan_down-c.tan_up)/ud,z,1,
        0,0,-c.near_z*z,0};
    Mul44(view, proj, vp);
}

// The same expanded homogeneous side/eye tests as WaterFactors, applied to the original triangles. Every clipped
// 16 m patch is inside one of them, so rejecting an entire original triangle cannot remove a surviving patch.
// No far-plane test: the GPU path deliberately keeps that case conservative too.
bool VisibleWaterPlane(const mw::Plane &plane, const float vp[16], float projX, float projY) {
    float strength = S.wind >= 0.1f ? std::fabs(S.waves) : 0;
    float bound = 6 * std::sqrt(g_spectrum.heightVar) * strength * S.waveHeight * (1 + S.chop);
    float pad = std::max(bound, 0.1f) * std::max(projX, projY) * 2 + 0.01f;
    for (unsigned n = 0; n < plane.count; n++) {
        bool outside[5] = {true,true,true,true,true};
        const float *t = g_water.tris.data() + size_t(plane.first + n) * 9;
        for (int p = 0; p < 3; p++) {
            float x=t[p*3], y=t[p*3+1], z=t[p*3+2];
            float cx=x*vp[0]+y*vp[4]+z*vp[8]+vp[12], cy=x*vp[1]+y*vp[5]+z*vp[9]+vp[13];
            float cw=x*vp[3]+y*vp[7]+z*vp[11]+vp[15];
            const float d[5] = {cw,cx+cw,cw-cx,cy+cw,cw-cy};
            for (int side=0; side<5; side++) outside[side] = outside[side] && d[side] < -pad;
        }
        if (!outside[0] && !outside[1] && !outside[2] && !outside[3] && !outside[4]) return true;
    }
    return false;
}

bool Ok(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    H->log(M, MML_LOG_ERROR, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return false;
}

bool Make(ID3D11Device *d) {
    Release();
    D3D11_INPUT_ELEMENT_DESC el{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 448;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // (the captured triangles' winding is the game's)
    rd.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 16;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC cd = sd;
    cd.AddressU = cd.AddressV = cd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    bool ok = Ok(d->CreateVertexShader(kVsWater, sizeof kVsWater, nullptr, &G.vs), "VsWater") &&
              Ok(d->CreateHullShader(kHsWater, sizeof kHsWater, nullptr, &G.hs), "HsWater") &&
              Ok(d->CreateDomainShader(kDsWater, sizeof kDsWater, nullptr, &G.ds), "DsWater") &&
              Ok(d->CreatePixelShader(kPsWater, sizeof kPsWater, nullptr, &G.ps), "PsWater") &&
              Ok(d->CreateInputLayout(&el, 1, kVsWater, sizeof kVsWater, &G.layout), "the layout") &&
              Ok(d->CreateBuffer(&bd, nullptr, &G.cb), "the constants") &&
              Ok(d->CreateRasterizerState(&rd, &G.rs), "the rasterizer state") &&
              Ok(d->CreateSamplerState(&sd, &G.wrap), "a sampler") && Ok(d->CreateSamplerState(&cd, &G.clamp), "a sampler") &&
              Ok(d->CreateComputeShader(kCsPaintedSky, sizeof kCsPaintedSky, nullptr, &G.paintedSky), "the painted sky shader");
    rd.FillMode = D3D11_FILL_WIREFRAME;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && Ok(d->CreateRasterizerState(&rd, &G.wire), "the wireframe state") &&
         Ok(d->CreateDepthStencilState(&depth, &G.depthState), "the water's depth state");
    if (ok && !g_waves.Make(d, {kCsSpectrum, sizeof kCsSpectrum}, {kCsFftRows, sizeof kCsFftRows},
                            {kCsFftCols, sizeof kCsFftCols}, {kCsResolve, sizeof kCsResolve})) {
        H->log(M, MML_LOG_ERROR, "the ripples' kernels and textures could not be made");
        ok = false;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    for (auto &ring : G.query)
        for (auto &q : ring) ok = ok && Ok(d->CreateQuery(&qd, &q), "a query");
    qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    for (auto &q : G.stats) ok = ok && Ok(d->CreateQuery(&qd, &q), "the geometry statistics");
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    for (auto &q : G.disjoint) ok = ok && Ok(d->CreateQuery(&qd, &q), "the water GPU clock");
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (auto &row : G.stamp) for (auto &q : row) ok = ok && Ok(d->CreateQuery(&qd, &q), "the water GPU timestamp");
    G.made = ok;
    return ok;
}

// Restore the painted sky's radiance before filtering it. Ordinary scene geometry is copied unchanged; clouds
// modulate the source, rather than an independent analytic disc being added after the reflection was filtered.
bool PaintSky(ID3D11Device *d, ID3D11DeviceContext *ctx, const MmlGfxFrame *f, const MmlGfxView *refl) {
    G.skyReady = false;
    if (!refl || S.glitter <= 0) return false;
    const unsigned w = g_refl.Width(), h = g_refl.Height();
    if (!G.skyTexture || G.skyW != w || G.skyH != h) {
        SafeRelease(G.skyOut); SafeRelease(G.skyView); SafeRelease(G.skyTexture);
        G.skyW = G.skyH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = td.Format; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (!Ok(d->CreateTexture2D(&td, nullptr, &G.skyTexture), "the HDR sky reflection") ||
            !Ok(d->CreateShaderResourceView(G.skyTexture, nullptr, &G.skyView), "the HDR sky view") ||
            !Ok(d->CreateUnorderedAccessView(G.skyTexture, &ud, &G.skyOut), "the HDR sky output")) return false;
        G.skyW = w; G.skyH = h;
    }
    ID3D11Buffer *cbs[] = {static_cast<ID3D11Buffer *>(f->constants), G.cb};
    ID3D11ShaderResourceView *src[] = {g_refl.View(), static_cast<ID3D11ShaderResourceView *>(refl->depth)};
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShaderResources(4, 2, src);
    ctx->CSSetUnorderedAccessViews(0, 1, &G.skyOut, nullptr);
    ctx->CSSetShader(G.paintedSky, nullptr, 0);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView *none[2] = {}; ID3D11UnorderedAccessView *noUav = nullptr;
    ctx->CSSetShaderResources(4, 2, none);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->GenerateMips(G.skyView);
    G.skyReady = true; g_skyDrawn++;
    return true;
}

// Each plane's pixels on the screen from its query issued kRing frames ago (if the GPU has it: never waited for).
void Count(ID3D11DeviceContext *ctx, unsigned slot) {
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        if (!G.issued[slot][p]) continue;
        UINT64 n;
        if (ctx->GetData(G.query[slot][p], &n, sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (G.queryEpoch[slot][p] == g_visibilityEpoch) g_pixels[p] = n;
            G.issued[slot][p] = false;
        }
    }
}

void Pass(const MmlGfxFrame *f, void *) {
    if (MML_GFX_HAS(f, in_view) && f->in_view) return;   // (the frame's only)
    if (!S.on) return;
    if (!g_asked && GetTickCount() >= g_nextAsk) Ask();
    if (g_water.planes.empty()) return;
    float visibilityVp[16]; Mul44(f->view, f->proj, visibilityVp);
    bool possible = false;
    for (const auto &plane : g_water.planes)
        possible = possible || VisibleWaterPlane(plane, visibilityVp, f->proj[0], f->proj[5]);
    if (!possible) {
        g_offscreen++; g_haveHistory = false; G.skyReady = false;
        // Full draw and FFT skipped; moving back makes the conservative test positive without awaiting a query.
        return;
    }
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (G.madeFor != f->generation || !G.made) {
        if (!Make(d)) return;
        G.madeFor = f->generation;
    }
    if (G.vbVersion != g_water.version) {
        SafeRelease(G.vb);
        memset(G.issued, 0, sizeof G.issued);
        g_patches.clear(); g_patchPlanes.clear();
        for (const auto &p : g_water.planes) {
            unsigned first = static_cast<unsigned>(g_patches.size() / 9);
            mw::WaterPatches(g_water.tris.data() + size_t(p.first) * 9, p.count, g_patches);
            g_patchPlanes.push_back({p.level, first, static_cast<unsigned>(g_patches.size() / 9) - first});
        }
        if (g_patches.empty()) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(g_patches.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{g_patches.data(), 0, 0};
        if (!Ok(d->CreateBuffer(&bd, &init, &G.vb), "the water's triangles")) return;
        G.vbVersion = g_water.version;
    }
    unsigned slot = G.frame++ % kRing;
    Count(ctx, slot);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    if (G.statsIssued[slot] && ctx->GetData(G.stats[slot], &stats, sizeof stats,
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        G.geometryStats = stats; G.statsIssued[slot] = false;
    }
    if (G.depthW != f->width || G.depthH != f->height) {
        SafeRelease(G.dsv); SafeRelease(G.depth);
        G.depthW = G.depthH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = f->width; td.Height = f->height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        // In Quality/Performance the viewport is smaller than the actual effects/MRT texture. D3D11 requires the
        // depth target to match the attachment dimensions, not the render-size viewport.
        ID3D11Resource *targetResource = nullptr;
        static_cast<ID3D11RenderTargetView *>(f->target)->GetResource(&targetResource);
        if (targetResource) {
            D3D11_TEXTURE2D_DESC color{};
            static_cast<ID3D11Texture2D *>(targetResource)->GetDesc(&color);
            td.Width = color.Width; td.Height = color.Height;
            targetResource->Release();
        }
        td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (!Ok(d->CreateTexture2D(&td, nullptr, &G.depth), "the water's depth buffer") ||
            !Ok(d->CreateDepthStencilView(G.depth, nullptr, &G.dsv), "the water's depth view")) return;
        G.depthW = f->width; G.depthH = f->height;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float time = static_cast<float>(static_cast<double>(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart);
    bool seen = false;
    for (size_t p = 0; p < g_water.planes.size(); p++) seen = seen || g_pixels[p];
    if (G.timingIssued[slot]) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        UINT64 stamp[3] = {};
        if (ctx->GetData(G.disjoint[slot], &clock, sizeof clock, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(G.stamp[slot][0], &stamp[0], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(G.stamp[slot][1], &stamp[1], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(G.stamp[slot][2], &stamp[2], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (!clock.Disjoint && clock.Frequency) {
                G.fftMs = float(double(stamp[1] - stamp[0]) * 1000 / clock.Frequency);
                G.surfaceMs = float(double(stamp[2] - stamp[1]) * 1000 / clock.Frequency);
            }
            G.timingIssued[slot] = false;
        }
    }
    const bool timing = !G.timingIssued[slot];
    if (timing) { ctx->Begin(G.disjoint[slot]); ctx->End(G.stamp[slot][0]); }
    if (seen) g_waves.Update(d, ctx, g_spectrum, g_specVersion, time);
    if (timing) ctx->End(G.stamp[slot][1]);

    // the reflection drawn for this frame
    const MmlGfxView *refl = nullptr;
    if (MML_GFX_HAS(f, views) && g_reflPlane >= 0)
        for (unsigned i = 0; i < f->view_count; i++)
            if (f->views[i].handle == g_view && f->views[i].fresh) refl = &f->views[i];
    if (refl && !g_refl.Update(d, ctx, static_cast<ID3D11Texture2D *>(refl->color_texture))) refl = nullptr;

    float cb[112] = {};
    Mul44(f->view, f->proj, cb);
    if (refl) {
        Mul44(refl->view, refl->proj, cb + 16);
        if (!Inverse44(cb + 16, cb + 64)) refl = nullptr;
    }
    if (refl) {
        cb[32] = refl->view[2], cb[33] = refl->view[6], cb[34] = refl->view[10], cb[35] = refl->view[14];
        memcpy(g_rview, refl->view, sizeof g_rview);
        memcpy(g_rproj, refl->proj, sizeof g_rproj);
    }
    cb[36] = g_reflLevel;
    cb[37] = S.wind >= 0.1f ? S.waves : 0;
    cb[38] = refl ? 1.0f : 0.0f;
    cb[39] = time;
    for (int c = 0; c < mw::kCascades; c++) cb[40 + c] = 1 / mw::kPatch[c];
    cb[43] = g_spectrum.rest;
    float clarity = S.clarity > 0.05f ? S.clarity : 0.05f;
    for (int c = 0; c < 3; c++) {
        cb[44 + c] = -std::log(S.tint[c] > 1e-4f ? S.tint[c] : 1e-4f) / clarity;
        cb[48 + c] = S.colour[c];
    }
    cb[47] = S.depth;
    cb[51] = S.foam;
    cb[52] = S.glitter;
    cb[53] = refl && g_refl.Encoded() ? 1.0f : 0.0f;
    cb[54] = static_cast<float>(S.debug);
    cb[55] = S.chop;
    cb[56] = static_cast<float>(g_refl.Width() ? g_refl.Width() : f->width);
    cb[57] = static_cast<float>(g_refl.Height() ? g_refl.Height() : f->height);
    cb[58] = 1 / cb[56];
    cb[59] = 1 / cb[57];
    float strength = S.wind >= 0.1f ? S.waves : 0;
    cb[60] = S.scattering > 0 ? S.scattering : 0;
    cb[61] = S.phase;
    cb[62] = S.crests;
    cb[63] = 2 * std::sqrt(g_spectrum.heightVar) * strength;   // the crests' height: 2 deviations of the heights
    cb[80] = 0.00465f * S.shadowSoftness;   // tan of the sun's angular radius (0.266 degrees), scaled by the setting
    cb[81] = std::cos(S.sunRadius * 0.01745329252f); // outer angular support of the painted sun's glow
    cb[84] = S.waveHeight; cb[85] = S.geometry ? 1.0f : 0.0f;
    cb[86] = 6 * std::sqrt(g_spectrum.heightVar) * strength * S.waveHeight * (1 + S.chop);
    const bool temporal = MML_GFX_HAS(f, temporal_target) && f->temporal_target;
    memcpy(cb + 88, g_previousVP, sizeof g_previousVP);
    memcpy(cb + 104, g_previousEye, sizeof g_previousEye);
    if (MML_GFX_HAS(f, jitter)) { cb[108] = f->jitter[0]; cb[109] = f->jitter[1]; }
    cb[110] = g_haveHistory && seen ? 1.0f : 0.0f; cb[111] = temporal ? 1.0f : 0.0f;
    ctx->UpdateSubresource(G.cb, 0, nullptr, cb, 0, 0);
    const bool paintedSky = PaintSky(d, ctx, f, refl);
    // A missing HDR target keeps the ordinary painted reflection, never an independent analytic sun.
    cb[87] = paintedSky ? 1.0f : 0.0f;
    ctx->UpdateSubresource(G.cb, 0, nullptr, cb, 0, 0);

    UINT stride = 12, offset = 0;
    ctx->IASetInputLayout(G.layout);
    ctx->IASetVertexBuffers(0, 1, &G.vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11Buffer *cbs[2] = {static_cast<ID3D11Buffer *>(f->constants), G.cb};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->HSSetConstantBuffers(0, 2, cbs);
    ctx->DSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetShader(G.vs, nullptr, 0);
    ctx->HSSetShader(G.hs, nullptr, 0);
    ctx->DSSetShader(G.ds, nullptr, 0);
    ctx->PSSetShader(G.ps, nullptr, 0);
    ID3D11ShaderResourceView *srv[7] = {refl ? g_refl.View() : nullptr,
                                        refl ? static_cast<ID3D11ShaderResourceView *>(refl->depth) : nullptr,
                                        g_waves.Slopes(), g_waves.Shape(), g_waves.Displacement(), g_waves.PreviousDisplacement(),
                                        paintedSky ? G.skyView : (refl ? g_refl.View() : nullptr)};
    ctx->PSSetShaderResources(4, 7, srv);
    ctx->DSSetShaderResources(4, 7, srv);
    ID3D11SamplerState *samplers[2] = {G.wrap, G.clamp};
    ctx->PSSetSamplers(2, 2, samplers);
    ctx->DSSetSamplers(2, 2, samplers);
    ctx->RSSetState(S.debug == 14 ? G.wire : G.rs);
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *oldDepth = nullptr;
    ID3D11DepthStencilState *oldState = nullptr; UINT oldRef = 0;
    ctx->OMGetRenderTargets(1, &target, &oldDepth);
    ctx->OMGetDepthStencilState(&oldState, &oldRef);
    ID3D11RenderTargetView *targets[2] = {target, temporal ? static_cast<ID3D11RenderTargetView *>(f->temporal_target) : nullptr};
    ctx->OMSetRenderTargets(temporal ? 2 : 1, targets, G.dsv);
    ctx->OMSetDepthStencilState(G.depthState, 0);
    ctx->ClearDepthStencilView(G.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    bool record = !G.statsIssued[slot];
    if (record) ctx->Begin(G.stats[slot]);
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        bool query = !G.issued[slot][p];
        if (query) ctx->Begin(G.query[slot][p]);
        ctx->Draw(g_patchPlanes[p].count * 3, g_patchPlanes[p].first * 3);
        if (query) {
            ctx->End(G.query[slot][p]);
            G.issued[slot][p] = true; G.queryEpoch[slot][p] = g_visibilityEpoch;
        }
    }
    if (record) { ctx->End(G.stats[slot]); G.statsIssued[slot] = true; }
    if (timing) { ctx->End(G.stamp[slot][2]); ctx->End(G.disjoint[slot]); G.timingIssued[slot] = true; }
    ctx->OMSetRenderTargets(1, &target, oldDepth);
    ctx->OMSetDepthStencilState(oldState, oldRef);
    SafeRelease(target); SafeRelease(oldDepth); SafeRelease(oldState);
    ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView *none[7] = {};
    ctx->PSSetShaderResources(4, 7, none);
    ctx->DSSetShaderResources(4, 7, none);
    memcpy(g_previousVP, cb, sizeof g_previousVP);
    g_previousEye[0] = f->k->eye[0]; g_previousEye[1] = f->k->eye[1]; g_previousEye[2] = f->k->eye[2];
    g_haveHistory = seen;
    if (temporal) g_temporalDrawn++;
    g_drawn++;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { ReadSettings(); return; }
    if (ev->type == MML_EVENT_MISSION_LOADED) Forget();
}

int Status(const char *, MmlReply *r, void *) {
    MmlFrameStats fs{};
    fs.size = sizeof fs;
    if (!MML_HAS(H, frame_stats) || !H->frame_stats(M, &fs)) fs.frames = 0;
    std::string planes;
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        char one[128];
        snprintf(one, sizeof one, "%s{\"level\":%.3f,\"triangles\":%u,\"pixels\":%lld}", p ? "," : "",
                 g_water.planes[p].level, g_water.planes[p].count,
                 g_pixels[p] == ~0ull ? -1ll : static_cast<long long>(g_pixels[p]));
        planes += one;
    }
    std::string seen = "null";
    float to[3];
    if (MML_HAS(H, scene_sun_seen) && H->scene_sun_seen(M, to)) {
        char one[96];
        snprintf(one, sizeof one, "[%.3f,%.3f,%.3f]", to[0], to[1], to[2]);
        seen = one;
    }
    std::string mats = "[";   // the last reflection's view, then its projection (row-major, row vectors)
    for (int i = 0; i < 32; i++) {
        char one[32];
        snprintf(one, sizeof one, "%s%.4f", i ? "," : "", i < 16 ? g_rview[i] : g_rproj[i - 16]);
        mats += one;
    }
    mats += "]";
    char out[2800];
    snprintf(out, sizeof out,
             "{\"on\":%s,\"texture\":\"%s\",\"shade\":\"%s\",\"hidden\":[%d,%d],\"groups\":%u,\"captures\":%u,"
             "\"planes\":[%s],\"reflection\":{\"plane\":%d,\"level\":%.3f,\"drawn\":%u,\"mips\":%u,\"srgb\":%s},"
             "\"painted_sky\":{\"ready\":%s,\"frames\":%u},"
             "\"visibility\":{\"surface_skipped\":%u,\"reflection_skipped\":%u},"
             "\"waves\":{\"wind\":%.1f,\"from\":%.0f,\"fetch\":%.0f,\"peak_m\":%.2f,\"slope_var_drawn\":%.4f,"
             "\"slope_var_rest\":%.4f,\"height_dev_m\":%.4f},\"optics\":{\"scattering\":%.2f,\"phase\":%.2f,\"crests\":%.2f,\"depth\":%.2f},"
             "\"geometry\":{\"on\":%s,\"height\":%.2f,\"patches\":%u,\"vertices\":%llu,\"triangles\":%llu},"
             "\"temporal\":{\"drawn\":%u,\"history\":%s},\"gpu\":{\"fft_ms\":%.3f,\"surface_ms\":%.3f},"
             "\"sun_seen\":%s,\"shadow_source\":\"%s\",\"reflection_matrices\":%s,\"drawn\":%u,"
             "\"frame\":{\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"mod_ms\":%.3f,\"views_ms\":%.2f,\"bridge_ms\":%.2f}}",
             S.on ? "true" : "false", S.texture.c_str(), S.shade.c_str(), g_hide, g_hideShade,
             static_cast<unsigned>(g_water.groups.size()), g_captures, planes.c_str(), g_reflPlane, g_reflLevel,
             g_reflDrawn, g_refl.Mips(), g_refl.Encoded() ? "false" : "true", G.skyReady ? "true" : "false", g_skyDrawn,
             g_offscreen, g_reflectionRejected, S.wind, S.windFrom, S.fetch,
             g_spectrum.peak, g_spectrum.drawn, 2 * g_spectrum.rest, std::sqrt(g_spectrum.heightVar), S.scattering, S.phase,
             S.crests, S.depth, S.geometry ? "true" : "false", S.waveHeight, static_cast<unsigned>(g_patches.size() / 9),
             G.geometryStats.DSInvocations, G.geometryStats.CPrimitives,
             g_temporalDrawn, g_haveHistory ? "true" : "false",
             G.fftMs, G.surfaceMs,
             seen.c_str(), S.shadowSource == 0 ? "sun" : "scene", mats.c_str(), g_drawn, fs.avg_ms, fs.p95_ms, fs.mod_ms,
             fs.views_ms, fs.bridge_ms);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, geom_capture)) return 1;   // a loader without the game's geometry
    QueryPerformanceCounter(&g_t0);
    QueryPerformanceFrequency(&g_freq);
    g_water.maxPlanes = kMaxPlanes;
    ReadSettings();
    g_pass = H->gfx_pass(M, -120, Pass, nullptr);   // before the upscaler: the water and its mask enter DLSS with their frame
    g_view = H->scene_view(M, 0, 0, PlaceReflection, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_MISSION_LOADED, OnEvent, nullptr);
    H->command(M, "status", "the water: its planes and their pixels, the reflection, the waves, the frame's cost",
               Status, nullptr);
    return g_pass ? 0 : 3;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { Release(); }
