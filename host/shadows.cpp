// What per-object shading needs beyond the game's own draw (mml.h shader_shadows; docs/MODDING.md "Shaders on the
// game's own drawing"): a shadow map from the sun, the game's lights near each draw.
//
// The shadow map: before the mission's world is drawn (after the frame's camera is set, before the mods' scene
// views), the host draws the depth of what is around the camera as seen from the scene's sun (lights.cpp scene_sun)
// into a texture of its own on the game's Direct3D 9 device (R32F, an orthographic box following the camera, its
// texels snapped so it does not shimmer). The casters, two ways:
//   - "game" (default): LS3D draws current posed visuals from an orthographic camera (view.cpp ViewDrawShadow),
//     each face group written as depth by shaders.cpp with its textures and alpha test. The shadow-only visibility
//     hook supplies box planes, disables screen occlusion and removes artificial camera-distance LOD fading.
//     Restore the target, depth buffer, viewport AND depth writes: LS3D's cached state reset omits ZWRITEENABLE.
//   - "meshes" (diagnostic/fallback): the host draws cached scene meshes and posed cars. No alpha cut-outs,
//     people or railway actors; not suitable for normal water shadows. Its full device state block is restored.
// The rules' shaders find the map at sampler 2 with its matrix in game.hlsli's registers.
//
// The game's lights: the scene's point and spot lights that are on (lights.cpp), up to eight whose range reaches
// the drawn visual's bounding sphere, nearest first, in game.hlsli's registers at each draw a rule takes.
#include "host.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "gfx_vk.h"
#include "d3dmirror.h"
#include "shaders/VsMeshShadow.h"
#include "shaders/PsMeshShadow.h"
#include "mml/game/game.h"
#include "receiver_fit.h"

namespace host {
namespace {

constexpr int kVisualBoundCentre = 0x19C, kVisualBoundRadius = 0x1A8;

struct Want {
    MmlMod *mod; MmlShadowSettings s; int source; bool fit = false; float minY = 0, maxY = 0;
    std::vector<float> surface; float displacement = 0;
};
std::vector<Want> g_wants;
bool g_surfaceFit = true;
const Want *SourceRequest() {
    for (auto it = g_wants.rbegin(); it != g_wants.rend(); ++it) if (it->source >= 0) return &*it;
    return nullptr;
}
int RequestedSource() { const Want *w = SourceRequest(); return w ? w->source : MML_SHADOW_SCENE; }
const char *SourceName(int source) { return source == MML_SHADOW_SUN ? "sun" : "scene"; }
float g_shadowDirection[3];
const char *g_sourceUsed = "none";

// ---- the map
IDirect3DDevice9 *g_dev;
IDirect3DTexture9 *g_map;
IDirect3DSurface9 *g_mapSurface, *g_mapDepth;
IDirect3DVertexShader9 *g_vs;
IDirect3DPixelShader9 *g_ps;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DStateBlock9 *g_block;
HANDLE g_shared;         // the map's handle for the graphics bridge's Direct3D 11 (gfx.cpp), when the device is 9Ex
unsigned g_mapMade;      // counts the maps made: a new one is a new handle
int g_size;
bool g_valid;            // drawn this frame: the rules' shaders may sample it
float g_lightVP[16];     // world to the map's clip space (row vectors)
float g_extent, g_bias;   // g_bias: in the map's depth (0..1 over kDepth metres)
float g_biasM;            // the same in metres along the sun's ray
constexpr float kBack = 400.0f, kDepth = 800.0f;   // the map's depth: casters up to 400 m towards the sun
constexpr float kDefaultBiasM = 0.05f;
unsigned g_casters, g_triangles, g_frames;
double g_passMs;
std::string g_why;
bool g_fitted, g_densityLimited;
float g_receiverBounds[6];
float g_halfX, g_halfY;
bool g_depthEnabled = true, g_depthPending, g_depthActive, g_depthUsed;
void *g_depthDriver, *g_depthScene;
RenderFn g_depthRender;
float g_depthBounds[6];
unsigned g_depthFrames, g_depthFallbacks, g_depthSamples;
double g_depthReduceMs;

struct Buffers { IDirect3DVertexBuffer9 *vb = nullptr; IDirect3DIndexBuffer9 *ib = nullptr; int verts = 0, tris = 0; unsigned used = 0; };
std::unordered_map<const float *, Buffers> g_buffers;   // by the mesh's positions (meshes.cpp keeps them while cached)

void ReleaseAll() {
    for (auto &kv : g_buffers) {
        if (kv.second.vb) kv.second.vb->Release();
        if (kv.second.ib) kv.second.ib->Release();
    }
    g_buffers.clear();
    if (g_mapSurface) g_mapSurface->Release();
    if (g_map) g_map->Release();
    if (g_mapDepth) g_mapDepth->Release();
    if (g_vs) g_vs->Release();
    if (g_ps) g_ps->Release();
    if (g_decl) g_decl->Release();
    if (g_block) g_block->Release();
    g_mapSurface = g_mapDepth = nullptr;
    g_map = nullptr;
    g_vs = nullptr;
    g_ps = nullptr;
    g_decl = nullptr;
    g_block = nullptr;
    g_dev = nullptr;
    g_size = 0;
    g_shared = nullptr;
}

bool Make(IDirect3DDevice9 *d, int size) {
    if (g_dev == d && g_size == size && g_map) return true;
    if (g_dev && g_dev != d) {   // (another device: the old one's objects went with it)
        g_buffers.clear();
        g_mapSurface = g_mapDepth = nullptr;
        g_map = nullptr;
        g_vs = nullptr;
        g_ps = nullptr;
        g_decl = nullptr;
        g_block = nullptr;
    } else {
        ReleaseAll();
    }
    g_dev = d;
    g_size = size;
    g_shared = nullptr;
    g_mapMade++;
    // shared when the device is 9Ex (the labs' dxwrapper): the graphics bridge's passes read it in Direct3D 11. Not on
    // DXVK (its handles are Vulkan's own): the bridge copies the map there (gfx_vk.cpp)
    IDirect3DDevice9Ex *ex = nullptr;
    bool canShare = SUCCEEDED(d->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&ex))) && ex &&
                    !gfxvk::IsDxvk(d);
    if (ex) ex->Release();
    HANDLE shared = nullptr;
    HRESULT hr = d->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_map, canShare ? &shared : nullptr);
    if (FAILED(hr) && canShare) {   // (a device that shares nothing: the rules' shaders still get the map)
        shared = nullptr;
        hr = d->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_map, nullptr);
    }
    if (FAILED(hr) || !g_map) {
        g_why = "the device makes no R32F render target";
        return false;
    }
    g_shared = shared;
    g_map->GetSurfaceLevel(0, &g_mapSurface);
    if (FAILED(d->CreateDepthStencilSurface(size, size, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_mapDepth, nullptr))) {
        g_why = "the device makes no depth buffer for the map";
        return false;
    }
    bool ok = SUCCEEDED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsMeshShadow), &g_vs)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsMeshShadow), &g_ps));
    if (!ok) { g_why = "the depth shaders were not made"; return false; }
    D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (FAILED(d->CreateVertexDeclaration(el, &g_decl))) { g_why = "no vertex declaration"; return false; }
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_block))) { g_why = "no state block"; return false; }
    g_why.clear();
    return true;
}

Buffers *BuffersFor(IDirect3DDevice9 *d, const MmlMeshPiece &m) {
    auto it = g_buffers.find(m.pos);
    if (it != g_buffers.end() && it->second.verts == m.vertices && it->second.tris == m.triangles) { it->second.used = g_frames; return &it->second; }
    if (it != g_buffers.end()) {
        if (it->second.vb) it->second.vb->Release();
        if (it->second.ib) it->second.ib->Release();
        g_buffers.erase(it);
    }
    Buffers b;
    b.verts = m.vertices;
    b.tris = m.triangles;
    b.used = g_frames;
    void *p = nullptr;
    if (FAILED(d->CreateVertexBuffer(m.vertices * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &b.vb, nullptr)) ||
        FAILED(b.vb->Lock(0, 0, &p, 0)))
        return nullptr;
    memcpy(p, m.pos, m.vertices * 12);
    b.vb->Unlock();
    if (FAILED(d->CreateIndexBuffer(m.triangles * 12, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &b.ib, nullptr)) ||
        FAILED(b.ib->Lock(0, 0, &p, 0))) {
        b.vb->Release();
        if (b.ib) b.ib->Release();
        return nullptr;
    }
    memcpy(p, m.index, m.triangles * 12);
    b.ib->Unlock();
    return &(g_buffers[m.pos] = b);
}

void MulM(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

// The sun's view of a box around the eye: a left-handed look along -toSun, an orthographic projection; the centre
// snapped to the map's texels in the light's space.
void LightMatrix(const float eye[3], const float toSun[3], float extent, int size, float out[16]) {
    V3 z = Norm({-toSun[0], -toSun[1], -toSun[2]});
    V3 up = std::fabs(z.y) > 0.99f ? V3{1, 0, 0} : V3{0, 1, 0};
    V3 x = Norm(Cross(up, z)), y = Cross(z, x);
    V3 c{eye[0], eye[1], eye[2]};
    float texel = 2 * extent / static_cast<float>(size);
    float cx = std::floor(Dot(c, x) / texel) * texel, cy = std::floor(Dot(c, y) / texel) * texel, cz = Dot(c, z);
    const float back = kBack, depth = kDepth;
    float view[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -cx, -cy, -(cz - back), 1};
    float proj[16] = {1 / extent, 0, 0, 0, 0, 1 / extent, 0, 0, 0, 0, 1 / depth, 0, 0, 0, 0, 1};
    MulM(view, proj, out);
}

// ---- the casters as the game draws them: LS3D's own drawing of the scene from the sun (view.cpp ViewDrawShadow, an
// orthographic camera of LS3D's), every face group written as its depth by shaders.cpp (its alpha test kept: leaves
// and fences cut out; what lets light through and the sky left out); people as posed, billboards turned to the sun.
using ClearFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
ClearFn oClear;
bool g_clearHooked, g_clearTried;
constexpr int kVtClear = 43;   // IDirect3DDevice9's
float g_sunDistance = 250.0f;   // the sun's eye this far from the camera (shader.shadows sun_distance)
constexpr float kBelow = 150.0f;   // the depth this far past the camera
// LS3D clears its target with the scene's background: the map's clear is "nothing", 1
HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if (ShadowViewNow()) color = 0xFFFFFFFF;
    return oClear(d, n, r, flags, color, z, stencil);
}
bool EnsureClear(IDirect3DDevice9 *d, std::string *why) {
    if (g_clearHooked) return true;
    if (g_clearTried) { *why = "the device's Clear is not hooked"; return false; }
    g_clearTried = true;
    uintptr_t at = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(d))[kVtClear]);
    if (!HookAdd(nullptr, at, reinterpret_cast<void *>(&hkClear), reinterpret_cast<void **>(&oClear), why)) return false;
    g_clearHooked = true;
    return true;
}

bool g_castersGame = true;     // the live renderer: textures/alpha tests and all posed actors, every frame
std::string g_mode = "none";   // "game": LS3D's drawing; "meshes": the host's own (when the game's could not be)
std::string g_gameWhy;         // why the game's drawing was not used
unsigned g_skipped, g_cut;
float g_range = kDepth;        // the map's depth, metres

bool PassGame(IDirect3DDevice9 *d, void *driver, void *scene, RenderFn render, const float eye[3], const float toSun[3], int size,
              float before = 0, float after = 0) {
    std::string why;
    if (!driver || !scene || !render) { g_gameWhy = "not inside the frame's drawing"; return false; }
    if (!GeomEnsureHooks(&why) || !GeomEnsureDip(&why) || !EnsureClear(d, &why)) { g_gameWhy = why; return false; }
    V3 z = Norm({-toSun[0], -toSun[1], -toSun[2]});
    V3 up = std::fabs(z.y) > 0.99f ? V3{1, 0, 0} : V3{0, 1, 0};
    V3 x = Norm(Cross(up, z)), y = Cross(z, x);
    float texel = 2 * g_halfX / static_cast<float>(size), texelY = 2 * g_halfY / static_cast<float>(size);
    V3 c{eye[0], eye[1], eye[2]};
    float cx = std::floor(Dot(c, x) / texel) * texel, cy = std::floor(Dot(c, y) / texelY) * texelY, cz = Dot(c, z);
    V3 focus{x.x * cx + y.x * cy + z.x * cz, x.y * cx + y.y * cy + z.y * cz, x.z * cx + y.z * cy + z.z * cz};
    const float distance = std::max(g_sunDistance, before);
    V3 sun{focus.x - z.x * distance, focus.y - z.y * distance, focus.z - z.z * distance};
    float world[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, sun.x, sun.y, sun.z, 1};
    const float nearZ = 1.0f, farZ = distance + std::max(kBelow, after);
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    D3DVIEWPORT9 oldVp{};
    DWORD oldZWrite = TRUE;
    d->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->GetViewport(&oldVp);
    d->SetRenderTarget(0, g_mapSurface);
    d->SetDepthStencilSurface(g_mapDepth);
    D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(size), static_cast<DWORD>(size), 0, 1};
    d->SetViewport(&vp);
    oClear(d, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    float viewProj[16], back[16];
    bool ok = ViewDrawShadow(driver, scene, render, world, g_halfX, g_halfY, nearZ, farZ, size, viewProj, back, &why);
    // (only what was changed here is put back: LS3D keeps a cache of the states it set, and they are on the device)
    d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(ds);
    d->SetViewport(&oldVp);
    // LS3D ends its auxiliary Render with depth writes off. Its ResetRenderProps state block does not contain this
    // state, so leaking it makes the following world draw in submission order and breaks water's depth mask too.
    d->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
    if (rt) rt->Release();
    if (ds) ds->Release();
    if (!ok) { g_gameWhy = why; return false; }
    memcpy(g_lightVP, viewProj, sizeof g_lightVP);   // (the camera's; the drawing's own below when it drew anything)
    g_range = farZ - nearZ;
    std::string err;
    ShadersShadowStats(&g_casters, &g_triangles, &g_skipped, &g_cut, g_lightVP, &err);
    if (!err.empty()) { g_gameWhy = err; return false; }
    g_gameWhy.clear();
    return true;
}

void PassMeshes(IDirect3DDevice9 *d, const float eye[3], const float toSun[3], int size) {
    // the casters: the scene around the camera (meshes.cpp scans the box of the host's own) and the cars near it
    // (the box moved only when the camera has gone a quarter of it: a new box makes meshes.cpp scan at once)
    static float s_at[3] = {1e30f, 0, 0};
    static float s_extent;
    if (std::fabs(eye[0] - s_at[0]) > g_extent / 4 || std::fabs(eye[1] - s_at[1]) > g_extent / 4 || std::fabs(eye[2] - s_at[2]) > g_extent / 4 ||
        s_extent != g_extent) {
        float lo[3] = {eye[0] - g_extent * 1.25f, eye[1] - 60, eye[2] - g_extent * 1.25f};
        float hi[3] = {eye[0] + g_extent * 1.25f, eye[1] + 60, eye[2] + g_extent * 1.25f};
        SceneFocus(kLoaderOwner, lo, hi);
        memcpy(s_at, eye, sizeof s_at);
        s_extent = g_extent;
    }
    std::vector<MmlMeshPiece> pieces(2048);
    int n = ScenePieces(kLoaderOwner, pieces.data(), static_cast<int>(pieces.size()));
    pieces.resize(static_cast<size_t>(std::min<int>(n, 2048)));
    void *nearby[64];
    int cars = WorldNear(eye, g_extent * 1.5f, MML_ACTOR_CAR, nearby, 64);
    for (int i = 0; i < cars && i < 64; i++) {
        MmlMeshPiece parts[96];
        int k = ActorMeshes(nearby[i], parts, 96);
        for (int j = 0; j < k && j < 96; j++) pieces.push_back(parts[j]);
    }
    LightMatrix(eye, toSun, g_extent, size, g_lightVP);
    g_block->Capture();
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->SetRenderTarget(0, g_mapSurface);
    d->SetDepthStencilSurface(g_mapDepth);
    D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(size), static_cast<DWORD>(size), 0, 1};
    d->SetViewport(&vp);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    d->SetVertexDeclaration(g_decl);
    d->SetVertexShader(g_vs);
    d->SetPixelShader(g_ps);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    // every channel written: with only red written (the map is R32F), Direct3D 11 saw the map as cleared and none of
    // the draws (an NVIDIA RTX 3070 under dxwrapper's 9Ex, measured), while Direct3D 9 saw them
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_casters = 0;
    g_triangles = 0;
    for (const MmlMeshPiece &m : pieces) {
        if (!m.vertices || !m.triangles) continue;
        Buffers *b = BuffersFor(d, m);
        if (!b) continue;
        float wvp[16];
        MulM(m.world, g_lightVP, wvp);
        d->SetVertexShaderConstantF(0, wvp, 4);
        d->SetStreamSource(0, b->vb, 0, 12);
        d->SetIndices(b->ib);
        d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, b->verts, 0, b->tris);
        g_casters++;
        g_triangles += static_cast<unsigned>(b->tris);
    }
    g_block->Apply();
    d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(ds);
    if (rt) rt->Release();
    if (ds) ds->Release();
    // the buffers no piece used for 10 s go
    if (g_frames % 600 == 0)
        for (auto it = g_buffers.begin(); it != g_buffers.end();) {
            if (g_frames - it->second.used > 600) {
                if (it->second.vb) it->second.vb->Release();
                if (it->second.ib) it->second.ib->Release();
                it = g_buffers.erase(it);
            } else {
                ++it;
            }
        }
    g_range = kDepth;
}

// Clip the camera frustum to each requested receiver-height slab, then fit in light space. No distance cutoff
// inside the visible water, and a growing texture keeps the requested world-space texel density unchanged.
bool FitReceivers(IDirect3DDevice9 *d, const MmlRenderCamera &cam, const float toSun[3], int &size,
                  float focus[3], float &before, float &after) {
    g_fitted = std::any_of(g_wants.begin(), g_wants.end(), [](const Want &w) { return w.fit; });
    g_densityLimited = false;
    if (!g_fitted) return true;
    if (g_depthActive && (g_depthBounds[0]>g_depthBounds[3] || g_depthBounds[1]>g_depthBounds[4] ||
                         g_depthBounds[2]>g_depthBounds[5])) { g_why="no visible shadow receivers";return false; }
    const V3 z = Norm({-toSun[0], -toSun[1], -toSun[2]});
    const V3 x = Norm(Cross(std::fabs(z.y) > 0.99f ? V3{1, 0, 0} : V3{0, 1, 0}, z)), y = Cross(z, x);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    auto add = [&](V3 p) {
        const float v[3] = {Dot(p, x), Dot(p, y), Dot(p, z)};
        for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], v[a]); hi[a] = std::max(hi[a], v[a]); }
    };
    V3 corners[8];
    for (int i = 0; i < 8; i++) {
        const float depth = i < 4 ? cam.near_z : cam.far_z;
        const float sx = ((i & 1 ? 1.0f : -1.0f) - cam.proj[8]) / cam.proj[0];
        const float sy = ((i & 2 ? 1.0f : -1.0f) - cam.proj[9]) / cam.proj[5];
        corners[i] = {cam.eye.x + depth * (cam.fwd.x + sx * cam.right.x + sy * cam.up.x),
                      cam.eye.y + depth * (cam.fwd.y + sx * cam.right.y + sy * cam.up.y),
                      cam.eye.z + depth * (cam.fwd.z + sx * cam.right.z + sy * cam.up.z)};
    }
    static const int edges[12][2] = {{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
    float texel = 1e30f;
    for (const Want &w : g_wants) {
        texel = std::min(texel, 2 * (w.s.extent > 1 ? w.s.extent : 60) / std::max<int>(w.s.size, 512));
        if (g_depthActive) {
            if(!w.surface.empty()) {
                const receiver_fit::V axes[3]={{x.x,x.y,x.z},{y.x,y.y,y.z},{z.x,z.y,z.z}};
                receiver_fit::IntersectLightBounds(w.surface.data(),static_cast<unsigned>(w.surface.size()/9),
                    w.displacement,axes,g_depthBounds,lo,hi);
            } else for(int a=0;a<3;a++){lo[a]=std::min(lo[a],g_depthBounds[a]);hi[a]=std::max(hi[a],g_depthBounds[a+3]);}
            continue;
        }
        if (g_surfaceFit && !w.surface.empty()) {
            const receiver_fit::V axes[3]={{x.x,x.y,x.z},{y.x,y.y,y.z},{z.x,z.y,z.z}};
            receiver_fit::Include(cam,w.surface.data(),static_cast<unsigned>(w.surface.size()/9),w.displacement,axes,lo,hi);
            continue;
        }
        if (!w.fit) {
            const V3 p{cam.eye.x, cam.eye.y, cam.eye.z};
            const float c[3] = {Dot(p, x), Dot(p, y), Dot(p, z)};
            for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], c[a] - w.s.extent); hi[a] = std::max(hi[a], c[a] + w.s.extent); }
            continue;
        }
        for (const V3 &p : corners) if (p.y >= w.minY && p.y <= w.maxY) add(p);
        for (const auto &edge : edges) for (float level : {w.minY, w.maxY}) {
            const V3 &a = corners[edge[0]], &b = corners[edge[1]];
            if (std::fabs(b.y - a.y) < 1e-7f) continue;
            const float t = (level - a.y) / (b.y - a.y);
            if (t >= 0 && t <= 1) add({a.x + (b.x - a.x) * t, level, a.z + (b.z - a.z) * t});
        }
    }
    if (lo[0] > hi[0]) { g_why = "no visible shadow receivers"; return false; }
    const float padding = 40 * texel + 0.05f; // maximum PCSS footprint (32 texels) plus snapping
    const float needed = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f + padding;
    D3DCAPS9 caps{}; d->GetDeviceCaps(&caps);
    const int limit = static_cast<int>(std::min(caps.MaxTextureWidth, caps.MaxTextureHeight));
    int target = 512;
    while (target < limit && target * texel * 0.5f < needed) target *= 2;
    target = std::min(target, limit);
    // A little hysteresis on shrinking, without changing the world texel size.
    if (g_size > target && g_size <= limit && needed > g_size * texel * 0.225f) target = g_size;
    size = std::max(size, target);
    g_extent = std::max(size * texel * 0.5f, needed);
    auto axisHalf = [&](float span) {
        float half = 256 * texel;
        // Texture dimensions still grow by powers of two. The world-space box need not: doubling each axis
        // can include almost four times the required city area. Smaller stable steps retain full coverage.
        while (half < span * 0.5f + padding) half *= g_surfaceFit ? 1.125f : 2.0f;
        // Do not let quantization push actual texel density below the request when the texture can hold it.
        return std::min(half, std::max(span * 0.5f + padding, size * texel * 0.5f));
    };
    g_halfX = axisHalf(hi[0] - lo[0]); g_halfY = axisHalf(hi[1] - lo[1]);
    g_densityLimited = needed > size * texel * 0.5f;
    float c[3];
    for (int a = 0; a < 3; a++) { c[a] = (lo[a] + hi[a]) * 0.5f; g_receiverBounds[a] = lo[a]; g_receiverBounds[a + 3] = hi[a]; }
    focus[0] = x.x * c[0] + y.x * c[1] + z.x * c[2];
    focus[1] = x.y * c[0] + y.y * c[1] + z.y * c[2];
    focus[2] = x.z * c[0] + y.z * c[1] + z.z * c[2];
    before = c[2] - lo[2] + std::max(g_sunDistance, cam.far_z * 2);
    after = hi[2] - c[2] + 10;
    return true;
}

void Pass(IDirect3DDevice9 *d, void *driver, void *scene, RenderFn render) {
    ProfScope scope(nullptr, "shadow map");
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    g_valid = false;
    g_frames++;
    g_passMs=0;g_casters=g_triangles=g_cut=g_skipped=0;g_mode="none";
    MmlShadowSettings s{};
    for (const Want &w : g_wants) {
        s.size = std::max(s.size, w.s.size);
        s.extent = std::max(s.extent, w.s.extent);
        s.bias = std::max(s.bias, w.s.bias);
    }
    int size = s.size >= 4096 ? 4096 : s.size >= 2048 ? 2048 : s.size >= 1024 ? 1024 : 512;
    g_extent = s.extent > 1 ? s.extent : 60.0f;
    g_halfX = g_halfY = g_extent;
    g_biasM = s.bias > 0 ? s.bias : kDefaultBiasM;
    float toSun[3], col[3], amb[3];
    g_sourceUsed = "none";
    bool haveSun = false;
    if (RequestedSource() == MML_SHADOW_SUN && SceneSunSeen(toSun)) {
        haveSun = true;
        g_sourceUsed = "sun";
    } else if (SceneSun(toSun, col, amb)) {
        haveSun = true;
        g_sourceUsed = "scene";
    }
    if (!haveSun || toSun[1] < 0.05f) { g_why = "no selected shadow light above the horizon"; return; }
    memcpy(g_shadowDirection, toSun, sizeof g_shadowDirection);
    MmlRenderCamera cam{};
    if (!RenderCamera(&cam)) { g_why = "no camera"; return; }
    float eye[3] = {cam.eye.x, cam.eye.y, cam.eye.z};
    float before = 0, after = 0;
    if (!FitReceivers(d, cam, toSun, size, eye, before, after)) return;
    if (!Make(d, size)) return;
    if (g_castersGame && PassGame(d, driver, scene, render, eye, toSun, size, before, after)) {
        g_mode = "game";
    } else {
        if (!g_castersGame) g_gameWhy.clear();
        PassMeshes(d, eye, toSun, size);
        g_mode = "meshes";
        g_skipped = g_cut = 0;
    }
    g_bias = g_biasM / g_range;
    g_valid = true;
    g_why.clear();
    QueryPerformanceCounter(&t1);
    g_passMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// ---- the game's lights, read once a frame
std::vector<MmlLight> g_lights;
unsigned g_lightsFrame = ~0u;
float g_sun[3], g_sunColor[3], g_ambient[3];
bool g_haveSun;

void LightsNow() {
    if (g_lightsFrame == g_frames) return;
    g_lightsFrame = g_frames;
    int n = SceneLights(nullptr, 0);
    g_lights.resize(static_cast<size_t>(std::max(n, 0)));
    if (n > 0) SceneLights(g_lights.data(), n);
    g_lights.erase(std::remove_if(g_lights.begin(), g_lights.end(), [](const MmlLight &l) {
        return !l.on || (l.type != MML_LIGHT_POINT && l.type != MML_LIGHT_SPOT) || l.power <= 0;
    }), g_lights.end());
    g_haveSun = SceneSun(g_sun, g_sunColor, g_ambient) != 0;
}

} // namespace

bool ShadowsWanted() { return !g_wants.empty(); }
void ShadowsTune(float sunDistance, int casters) {
    if (sunDistance >= 5 && sunDistance <= 2000) g_sunDistance = sunDistance;
    if (casters >= 0) g_castersGame = casters == 1;
}

bool ShaderShadows(MmlMod *mod, const MmlShadowSettings *s, int source, const float *receiverY) {
    if (source < -1 || source > MML_SHADOW_SUN) return false;
    if (s && receiverY && (!std::isfinite(receiverY[0]) || !std::isfinite(receiverY[1]) || receiverY[0] > receiverY[1])) return false;
    const bool had = !g_wants.empty();
    g_wants.erase(std::remove_if(g_wants.begin(), g_wants.end(), [&](const Want &w) { return w.mod == mod; }), g_wants.end());
    if (s) g_wants.push_back({mod, *s, source, receiverY != nullptr, receiverY ? receiverY[0] : 0, receiverY ? receiverY[1] : 0});
    if (had != !g_wants.empty()) MirrorUse(!g_wants.empty());   // follow Set*; never read the whole device per caster
    g_valid = false;   // the next world frame redraws the map with the current request
    if (g_wants.empty()) {
        SceneFocus(kLoaderOwner, nullptr, nullptr);
        g_valid = false;
    }
    return true;
}
bool ShaderShadowSurface(MmlMod *mod, const MmlShadowSettings *s, int source, const float *tris, unsigned count, float displacement) {
    if (source != MML_SHADOW_SCENE && source != MML_SHADOW_SUN) return false;
    if (!s) return ShaderShadows(mod,nullptr,source);
    if (!tris || !count || count>1000000 || !std::isfinite(displacement) || displacement<0) return false;
    float y[2]={1e30f,-1e30f};
    for(size_t i=0;i<size_t(count)*9;i++) {
        if(!std::isfinite(tris[i])) return false;
        if(i%3==1) {y[0]=std::min(y[0],tris[i]-displacement);y[1]=std::max(y[1],tris[i]+displacement);}
    }
    std::vector<float> copy(tris,tris+size_t(count)*9);
    if(!ShaderShadows(mod,s,source,y)) return false;
    Want &w=g_wants.back();w.surface=std::move(copy);w.displacement=displacement;
    return true;
}
void ShadowsSurfaceEnable(bool on) { g_surfaceFit=on; }
void ShadowsModGone(MmlMod *mod) {
    bool had = std::any_of(g_wants.begin(), g_wants.end(), [&](const Want &w) { return w.mod == mod; });
    if (had) ShaderShadows(mod, nullptr);
}

void ShadowsBeforeWorld(void *driver, void *scene, RenderFn render) {   // (render.cpp: before the mission's world is drawn)
    g_valid = false;
    g_depthPending = g_depthUsed = false;
    if (g_wants.empty()) return;
    MmlRenderCamera camera{};
    if (g_depthEnabled && g_castersGame && !ShadersNeedEarlyShadow() && GfxShadowDepthAvailable() && ViewAllowsLateShadow(scene) &&
        std::all_of(g_wants.begin(),g_wants.end(),[](const Want &w){return w.fit;}) &&
        RenderCamera(&camera) && camera.far_z > 600) {
        g_depthDriver=driver;g_depthScene=scene;g_depthRender=render;g_depthPending=true;
        return;
    }
    auto *d = static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if (!d) { g_why = "no Direct3D 9 device (a wrapper's)"; return; }
    __try { Pass(d, driver, scene, render); } __except (EXCEPTION_EXECUTE_HANDLER) { g_valid = false; g_why = "the pass faulted"; }
}

void ShadowsDepthEnable(bool on) { g_depthEnabled=on; }
int ShadowsDepthPlan(float boxes[32][4], float basis[3][4]) {
    if (!g_depthPending || g_wants.empty()) return 0;
    float toSun[3], col[3], amb[3];
    bool sun = RequestedSource()==MML_SHADOW_SUN && SceneSunSeen(toSun);
    if (!sun && !SceneSun(toSun,col,amb)) return 0;
    if (toSun[1]<0.05f) return 0;
    const V3 z=Norm({-toSun[0],-toSun[1],-toSun[2]});
    const V3 x=Norm(Cross(std::fabs(z.y)>.99f?V3{1,0,0}:V3{0,1,0},z)), y=Cross(z,x);
    const V3 axes[3]={x,y,z};
    for(int a=0;a<3;a++) {basis[a][0]=axes[a].x;basis[a][1]=axes[a].y;basis[a][2]=axes[a].z;basis[a][3]=0;}
    int count=0;
    for(const Want &w:g_wants) {
        if(w.surface.empty()) {
            if(count==16) return -1;
            float *lo=boxes[count*2],*hi=boxes[count*2+1];count++;
            lo[0]=lo[2]=-1e20f;hi[0]=hi[2]=1e20f;lo[1]=w.minY;hi[1]=w.maxY;lo[3]=hi[3]=0;
            continue;
        }
        const int first=count;
        float levels[16]={};
        for(size_t t=0;t<w.surface.size();t+=9) {
            const float level=w.surface[t+1];int b=first;
            while(b<count && std::fabs(levels[b]-level)>.01f) b++;
            if(b==count) {
                if(count==16) return -1;
                levels[b]=level;count++;
                for(int a=0;a<3;a++){boxes[b*2][a]=1e30f;boxes[b*2+1][a]=-1e30f;}
                boxes[b*2][3]=boxes[b*2+1][3]=0;
            }
            for(int v=0;v<3;v++)for(int a=0;a<3;a++) {
                const float q=w.surface[t+v*3+a];
                boxes[b*2][a]=std::min(boxes[b*2][a],q-w.displacement);
                boxes[b*2+1][a]=std::max(boxes[b*2+1][a],q+w.displacement);
            }
        }
    }
    return count;
}
void ShadowsDepthFinish(const float *bounds, unsigned samples, double reduceMs) {
    if (!g_depthPending) return;
    g_depthPending=false; g_depthReduceMs=reduceMs; g_depthSamples=samples;
    if(g_wants.empty()) return;
    g_depthActive=bounds!=nullptr; g_depthUsed=g_depthActive;
    if(bounds) {memcpy(g_depthBounds,bounds,sizeof g_depthBounds);g_depthFrames++;}
    else g_depthFallbacks++;
    auto *d=static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if(d) {
        __try {Pass(d,g_depthDriver,g_depthScene,g_depthRender);}
        __except(EXCEPTION_EXECUTE_HANDLER) {g_valid=false;g_why="the late shadow pass faulted";}
    }
    g_depthActive=false;
}
void ShadowsMissionClose() {   // (the meshes' buffers keyed by pointers meshes.cpp lets go)
    g_depthPending=false;g_depthDriver=g_depthScene=nullptr;g_depthRender=nullptr;
    for (auto &kv : g_buffers) {
        if (kv.second.vb) kv.second.vb->Release();
        if (kv.second.ib) kv.second.ib->Release();
    }
    g_buffers.clear();
    g_valid = false;
}

// game.hlsli's registers c76.. for a draw of `visual` (shaders.cpp writes c0..c75): the map, the sun, the lights.
void ShadowsConstants(const void *visual, float *c, bool nearLights) {
    LightsNow();
    float *o = c + 76 * 4;
    memcpy(o, g_lightVP, 64);
    o[16] = g_valid ? 1.0f : 0.0f;
    o[17] = g_size ? 1.0f / static_cast<float>(g_size) : 0.0f;
    o[18] = g_bias;
    o[19] = static_cast<float>(g_size);
    o[20] = g_sun[0]; o[21] = g_sun[1]; o[22] = g_sun[2]; o[23] = g_haveSun ? 1.0f : 0.0f;
    o[24] = g_sunColor[0]; o[25] = g_sunColor[1]; o[26] = g_sunColor[2]; o[27] = 0;
    o[28] = g_ambient[0]; o[29] = g_ambient[1]; o[30] = g_ambient[2]; o[31] = 0;
    if (!nearLights) return;   // (a shader that does not read them)
    memset(o + 32, 0, (117 - 84) * 16);   // (the slots past the lights near: zeros, as they are compared)
    // the lights whose far range reaches the visual's bounding sphere, nearest first
    V3 bc{0, 0, 0};
    float br = 0;
    if (visual) {
        const float *p = reinterpret_cast<const float *>(static_cast<const uint8_t *>(visual) + kVisualBoundCentre);
        bc = {p[0], p[1], p[2]};
        br = *reinterpret_cast<const float *>(static_cast<const uint8_t *>(visual) + kVisualBoundRadius);
    }
    struct Near { float d; const MmlLight *l; };
    Near list[64];
    int n = 0;
    for (const MmlLight &l : g_lights) {
        float dist = Len(V3{l.pos[0], l.pos[1], l.pos[2]} - bc);
        if (dist > l.range[1] + br) continue;
        if (n < 64) list[n++] = {dist, &l};
    }
    std::sort(list, list + n, [](const Near &a, const Near &b) { return a.d < b.d; });
    n = std::min(n, 8);
    o[32] = static_cast<float>(n);
    o[33] = static_cast<float>(g_lights.size());
    for (int i = 0; i < n; i++) {
        const MmlLight &l = *list[i].l;
        float *r = o + 36 + i * 16;
        r[0] = l.pos[0]; r[1] = l.pos[1]; r[2] = l.pos[2]; r[3] = static_cast<float>(l.type);
        r[4] = l.color[0] * l.power; r[5] = l.color[1] * l.power; r[6] = l.color[2] * l.power; r[7] = l.range[1];
        r[8] = l.dir[0]; r[9] = l.dir[1]; r[10] = l.dir[2]; r[11] = std::cos(l.cone[1] / 2);
        r[12] = l.range[0]; r[13] = std::cos(l.cone[0] / 2); r[14] = 0; r[15] = 0;
    }
}

IDirect3DBaseTexture9 *ShadowsTexture() { return g_valid ? g_map : nullptr; }

bool ShadowsShared(void **handle, unsigned *made, float viewProj[16], float info[4]) {
    if (!g_valid) return false;
    *handle = g_shared;   // (null: not shared; DXVK's device, whose map the bridge copies)
    *made = g_mapMade;
    memcpy(viewProj, g_lightVP, 64);
    info[0] = 1;
    info[1] = 1.0f / static_cast<float>(g_size);
    info[2] = g_bias;
    info[3] = static_cast<float>(g_size);
    return true;
}

void ShadowsState(std::string *out) {
    using json = nlohmann::json;
    json j = {{"wanted", !g_wants.empty()}, {"drawn", g_valid}, {"casters_by", g_mode}, {"size", g_size}, {"extent", g_extent},
              {"bias", g_biasM}, {"depth_m", g_range}, {"casters", g_casters}, {"triangles", g_triangles}, {"left_out", g_skipped},
              {"cut_out", g_cut}, {"pass_ms", std::round(g_passMs * 100) / 100}, {"meshes_uploaded", g_buffers.size()},
              {"lights", g_lights.size()}, {"sun", g_haveSun}, {"shared", g_shared != nullptr},
              {"why", !g_wants.empty() && !g_valid && g_why.empty() ? std::string("no frame of the mission's world drawn since") : g_why}};
    if (!g_gameWhy.empty()) j["game_why"] = g_gameWhy;   // (why LS3D's drawing was not used: the host's own meshes then)
    j["source"] = SourceName(RequestedSource());
    const Want *source = SourceRequest();
    j["source_owner"] = source ? ModIdOf(source->mod) : "default";
    j["source_used"] = g_valid ? g_sourceUsed : "none";
    j["direction"] = g_valid ? json({g_shadowDirection[0], g_shadowDirection[1], g_shadowDirection[2]}) : json(nullptr);
    j["coverage"] = {{"fitted", g_fitted}, {"texel_m", g_size ? 2 * std::max(g_halfX, g_halfY) / g_size : 0},
                     {"half", {g_halfX, g_halfY}},
                     {"density_limited", g_densityLimited}, {"light_bounds", std::vector<float>(g_receiverBounds, g_receiverBounds + 6)}};
    size_t receivers=0; for(const Want &w:g_wants) receivers+=w.surface.size()/9;
    j["coverage"]["surface_fit"]=g_surfaceFit;
    j["coverage"]["receiver_triangles"]=receivers;
    j["depth_fit"]={{"enabled",g_depthEnabled},{"used",g_depthUsed},{"pending",g_depthPending},
        {"frames",g_depthFrames},{"fallbacks",g_depthFallbacks},{"samples",g_depthSamples},{"reduce_ms",g_depthReduceMs}};
    *out = j.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
