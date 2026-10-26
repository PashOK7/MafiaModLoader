// What per-object shading needs beyond the game's own draw (mml.h shader_shadows; docs/MODDING.md "Shaders on the
// game's own drawing"): a shadow map from the sun, the game's lights near each draw.
//
// The shadow map: before the mission's world is drawn (after the frame's camera is set, before the mods' scene
// views), the host draws the depth of what is around the camera as seen from the scene's sun (lights.cpp scene_sun)
// into a texture of its own on the game's Direct3D 9 device (R32F, an orthographic box following the camera, its
// texels snapped so it does not shimmer). The casters, two ways:
//   - "meshes" (the default): the scene's meshes around the camera (meshes.cpp, a box of the host's own) and the cars
//     near it as they are posed now, drawn by the host (their vertices uploaded once a mesh into managed buffers),
//     every Direct3D state put back after (a state block of everything, the render target, the depth buffer). No
//     people, no billboards, alpha-tested faces whole (ROADMAP Known gaps).
//   - "game" (shader.shadows casters "game", not yet the default): LS3D itself draws the scene from an orthographic
//     camera of its own (view.cpp ViewDrawShadow), each face group written as its depth by shaders.cpp with the
//     game's alpha test kept; only the target, depth buffer and viewport put back (LS3D keeps a cache of the states
//     it set). LS3D leaves out most objects from such a camera (measured: Known gaps), so it stays an option.
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
#include "mml/game/game.h"

namespace host {
namespace {

constexpr int kVisualBoundCentre = 0x19C, kVisualBoundRadius = 0x1A8;

struct Want { MmlMod *mod; MmlShadowSettings s; };
std::vector<Want> g_wants;

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

struct Buffers { IDirect3DVertexBuffer9 *vb = nullptr; IDirect3DIndexBuffer9 *ib = nullptr; int verts = 0, tris = 0; unsigned used = 0; };
std::unordered_map<const float *, Buffers> g_buffers;   // by the mesh's positions (meshes.cpp keeps them while cached)

const char kDepthHlsl[] = R"(
row_major float4x4 m : register(c0);
struct O { float4 pos : POSITION; float2 z : TEXCOORD0; };
O vs(float3 p : POSITION) { O o; o.pos = mul(float4(p, 1), m); o.z = o.pos.zw; return o; }
float4 ps(O i) : COLOR0 {
    // a slope-scaled bias: the depth's own change across a texel added (a face the sun grazes is pushed back more,
    // one it faces square hardly), so the receivers' constant bias stays small and no shadow comes loose
    float z = i.z.x / i.z.y;
    float slope = max(abs(ddx(z)), abs(ddy(z)));
    return float4(z + slope * 1.5, 0, 0, 1);
}
)";

using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

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
    static CompileFn compile = [] {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!compile) { g_why = "no d3dcompiler_47.dll"; return false; }
    ID3DBlob *v = nullptr, *p = nullptr, *e = nullptr;
    compile(kDepthHlsl, sizeof kDepthHlsl - 1, "shadow", nullptr, nullptr, "vs", "vs_3_0", 0, 0, &v, &e);
    if (e) e->Release();
    e = nullptr;
    compile(kDepthHlsl, sizeof kDepthHlsl - 1, "shadow", nullptr, nullptr, "ps", "ps_3_0", 0, 0, &p, &e);
    if (e) e->Release();
    bool ok = v && p && SUCCEEDED(d->CreateVertexShader(static_cast<const DWORD *>(v->GetBufferPointer()), &g_vs)) &&
              SUCCEEDED(d->CreatePixelShader(static_cast<const DWORD *>(p->GetBufferPointer()), &g_ps));
    if (v) v->Release();
    if (p) p->Release();
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

bool g_castersGame;            // LS3D drawing the casters (shader.shadows casters "game"): not yet the default, see ROADMAP
std::string g_mode = "none";   // "game": LS3D's drawing; "meshes": the host's own (when the game's could not be)
std::string g_gameWhy;         // why the game's drawing was not used
unsigned g_skipped, g_cut;
float g_range = kDepth;        // the map's depth, metres

bool PassGame(IDirect3DDevice9 *d, void *driver, void *scene, RenderFn render, const float eye[3], const float toSun[3], int size) {
    std::string why;
    if (!driver || !scene || !render) { g_gameWhy = "not inside the frame's drawing"; return false; }
    if (!GeomEnsureHooks(&why) || !GeomEnsureDip(&why) || !EnsureClear(d, &why)) { g_gameWhy = why; return false; }
    V3 z = Norm({-toSun[0], -toSun[1], -toSun[2]});
    V3 up = std::fabs(z.y) > 0.99f ? V3{1, 0, 0} : V3{0, 1, 0};
    V3 x = Norm(Cross(up, z)), y = Cross(z, x);
    float texel = 2 * g_extent / static_cast<float>(size);
    V3 c{eye[0], eye[1], eye[2]};
    float cx = std::floor(Dot(c, x) / texel) * texel, cy = std::floor(Dot(c, y) / texel) * texel, cz = Dot(c, z);   // (snapped: no shimmer)
    V3 focus{x.x * cx + y.x * cy + z.x * cz, x.y * cx + y.y * cy + z.y * cz, x.z * cx + y.z * cy + z.z * cz};
    V3 sun{focus.x - z.x * g_sunDistance, focus.y - z.y * g_sunDistance, focus.z - z.z * g_sunDistance};
    float world[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, sun.x, sun.y, sun.z, 1};
    const float nearZ = 1.0f, farZ = g_sunDistance + kBelow;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    D3DVIEWPORT9 oldVp{};
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    d->GetViewport(&oldVp);
    d->SetRenderTarget(0, g_mapSurface);
    d->SetDepthStencilSurface(g_mapDepth);
    D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(size), static_cast<DWORD>(size), 0, 1};
    d->SetViewport(&vp);
    oClear(d, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    float viewProj[16], back[16];
    bool ok = ViewDrawShadow(driver, scene, render, world, g_extent, nearZ, farZ, size, viewProj, back, &why);
    // (only what was changed here is put back: LS3D keeps a cache of the states it set, and they are on the device)
    d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(ds);
    d->SetViewport(&oldVp);
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

void Pass(IDirect3DDevice9 *d, void *driver, void *scene, RenderFn render) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    g_valid = false;
    g_frames++;
    MmlShadowSettings s{};
    for (const Want &w : g_wants) {
        s.size = std::max(s.size, w.s.size);
        s.extent = std::max(s.extent, w.s.extent);
        s.bias = std::max(s.bias, w.s.bias);
    }
    int size = s.size >= 4096 ? 4096 : s.size >= 2048 ? 2048 : s.size >= 1024 ? 1024 : 512;
    g_extent = s.extent > 1 ? s.extent : 60.0f;
    g_biasM = s.bias > 0 ? s.bias : kDefaultBiasM;
    float toSun[3], col[3], amb[3];
    if (!SceneSun(toSun, col, amb) || toSun[1] < 0.05f) { g_why = "no sun above the horizon"; return; }
    MmlRenderCamera cam{};
    if (!RenderCamera(&cam)) { g_why = "no camera"; return; }
    float eye[3] = {cam.eye.x, cam.eye.y, cam.eye.z};
    if (!Make(d, size)) return;
    if (g_castersGame && PassGame(d, driver, scene, render, eye, toSun, size)) {
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

bool ShaderShadows(MmlMod *mod, const MmlShadowSettings *s) {
    g_wants.erase(std::remove_if(g_wants.begin(), g_wants.end(), [&](const Want &w) { return w.mod == mod; }), g_wants.end());
    if (s) g_wants.push_back({mod, *s});
    if (g_wants.empty()) {
        SceneFocus(kLoaderOwner, nullptr, nullptr);
        g_valid = false;
    }
    return true;
}
void ShadowsModGone(MmlMod *mod) {
    bool had = std::any_of(g_wants.begin(), g_wants.end(), [&](const Want &w) { return w.mod == mod; });
    if (had) ShaderShadows(mod, nullptr);
}

void ShadowsBeforeWorld(void *driver, void *scene, RenderFn render) {   // (render.cpp: before the mission's world is drawn)
    g_valid = false;
    if (g_wants.empty()) return;
    auto *d = static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if (!d) { g_why = "no Direct3D 9 device (a wrapper's)"; return; }
    __try { Pass(d, driver, scene, render); } __except (EXCEPTION_EXECUTE_HANDLER) { g_valid = false; g_why = "the pass faulted"; }
}

void ShadowsMissionClose() {   // (the meshes' buffers keyed by pointers meshes.cpp lets go)
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
    *out = j.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
