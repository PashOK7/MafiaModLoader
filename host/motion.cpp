// Motion vectors for the graphics passes (mml.h gfx_motion, MmlGfxFrame motion; docs/MODDING.md "Motion vectors").
//
// Each pixel's motion, in pixels, to where the surface it shows was the frame before: DLSS's and TAA's convention.
// The camera's part comes from the depth (gfx.cpp's PsMotionAll reprojects each pixel through the last frame's camera);
// what moves by itself (cars, their wheels, doors, props, people as wholes) is drawn again here: LS3D draws each face
// group through m_RenderFGroup (geom.cpp hooks it), which knows the visual it draws, and the visual's world transform
// is the device's at its DrawIndexedPrimitive. A visual whose world transform is not the one it was drawn with the
// frame before has moved: right after LS3D's own draw of each of its face groups (its depth just written), the same
// draw (the same buffers and ranges: LS3D refills dynamic buffers as it goes, so a later draw could not use them) goes
// again into the objects' target (A16B16G16R16F) with VsMotion and PsMotion: every vertex through this frame's and
// the last frame's world, view and projection, each pixel's motion, its view depth and 1. PsMotionAll then takes an
// object's motion where the object is what is finally seen (its depth is the frame's) and the camera's elsewhere.
//
// One visual may draw one face group several times a frame with other transforms (a car's four wheels: one visual, one
// group): each draw is known by the visual, the group, the how-many-th time this frame that pair is drawn and the
// draw's place in that call. What writes no depth (a car's shadow decal, glass) keeps the motion of what is behind it.
//
// A compromise, said plainly: a skinned person moves as a whole (his visual's transform); his limbs' own motion
// (LS3D skins on the CPU into a cache: knowledge of item 28) is not drawn yet. Billboards (sprites that turn to the
// camera) move with the camera's motion. Only the frame's world drawing: scene views get none.
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "shaders/VsMotion.h"
#include "shaders/PsMotion.h"

namespace host {
namespace {

using DipFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

std::vector<MmlMod *> g_asks;      // the mods that want motion vectors

// Each draw's world transform, this frame's and the one before, by the visual, its face group and the draw's place
// among the group's draws (one m_RenderFGroup may draw a group several times with other transforms: a car's wheels);
// an entry not seen for a while goes.
struct Key {
    const void *visual, *group;
    unsigned draw;
    bool operator==(const Key &o) const { return visual == o.visual && group == o.group && draw == o.draw; }
};
struct KeyHash {
    size_t operator()(const Key &k) const {
        size_t h = reinterpret_cast<size_t>(k.visual) * 0x9E3779B1u;
        h ^= reinterpret_cast<size_t>(k.group) + 0x7F4A7C15u + (h << 6) + (h >> 2);
        return h ^ (k.draw * 0x85EBCA6Bu);
    }
};
struct Seen { D3DMATRIX now{}, before{}; unsigned frame = 0; bool haveBefore = false; };
std::unordered_map<Key, Seen, KeyHash> g_seen;
unsigned g_frame;                  // MotionFrameBegin's count

// this frame
bool g_on;                         // the objects' target is bound for this frame's world drawing
IDirect3DDevice9 *g_dev;
IDirect3DSurface9 *g_target;       // the objects' target (gfx.cpp's)
D3DMATRIX g_viewBefore, g_projBefore;
bool g_haveCameraBefore;

// the group LS3D draws now (geom.cpp), and its draws so far; how many times this frame the same visual's same group
// was drawn before (one visual and one group drawn again for another instance: the four wheels of a car)
const void *g_visual, *g_group;
unsigned g_drawInGroup, g_occurrence;
struct Pair {
    const void *visual, *group;
    bool operator==(const Pair &o) const { return visual == o.visual && group == o.group; }
};
struct PairHash {
    size_t operator()(const Pair &p) const { return reinterpret_cast<size_t>(p.visual) * 0x9E3779B1u ^ reinterpret_cast<size_t>(p.group); }
};
struct Count { unsigned frame = 0, n = 0; };
std::unordered_map<Pair, Count, PairHash> g_counts;

// Direct3D 9 objects
IDirect3DVertexShader9 *g_vs;
IDirect3DPixelShader9 *g_ps;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DStateBlock9 *g_block;
IDirect3DDevice9 *g_madeOn;

// statistics
unsigned g_visualsMoving, g_drawsAgain, g_skipped;
unsigned g_lastVisuals, g_lastDraws, g_lastSkipped;
double g_msNow, g_msLast;
LARGE_INTEGER g_freq;
double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * t.QuadPart / g_freq.QuadPart; }

void Release() {
    SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_decl); SafeRelease(g_block);
    g_madeOn = nullptr;
}

bool Make(IDirect3DDevice9 *d) {
    if (g_madeOn == d && g_vs) return true;
    Release();
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsMotion), &g_vs)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsMotion), &g_ps)) ||
        FAILED(d->CreateVertexDeclaration(el, &g_decl))) {
        Release();
        return false;
    }
    // the states this drawing changes (their values while recording do not matter)
    if (FAILED(d->BeginStateBlock())) { Release(); return false; }
    const D3DRENDERSTATETYPE rs[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                     D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
                                     D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE, D3DRS_SEPARATEALPHABLENDENABLE};
    for (D3DRENDERSTATETYPE s : rs) d->SetRenderState(s, 0);
    D3DVIEWPORT9 vp{0, 0, 1, 1, 0, 1};
    d->SetViewport(&vp);
    if (FAILED(d->EndStateBlock(&g_block)) || !g_block) { Release(); return false; }
    g_madeOn = d;
    return true;
}

void Mul(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &out) {
    D3DMATRIX r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    out = r;
}

bool Same(const D3DMATRIX &a, const D3DMATRIX &b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (std::fabs(a.m[i][j] - b.m[i][j]) > 1e-5f * (1 + std::fabs(a.m[i][j]))) return false;
    return true;
}

// The vertices' position is a float3 at the start of the vertex (D3DFVF_XYZ and its blend-weight kinds): every
// LS3D object vertex is so (FVF 0x142, 0x152...); pretransformed vertices (XYZRHW) are left out.
bool PositionFirst(IDirect3DDevice9 *d) {
    DWORD fvf = 0;
    if (FAILED(d->GetFVF(&fvf)) || !fvf) return false;
    DWORD p = fvf & D3DFVF_POSITION_MASK;
    return p == D3DFVF_XYZ || (p >= D3DFVF_XYZB1 && p <= D3DFVF_XYZB5);
}

void DrawAgain(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip) {
    D3DMATRIX world{};
    if (FAILED(d->GetTransform(D3DTS_WORLD, &world))) return;
    const Key key{g_visual, g_group, g_occurrence << 8 | (g_drawInGroup & 0xFF)};
    auto it = g_seen.find(key);
    Seen *s = it == g_seen.end() ? nullptr : &it->second;
    if (!s || s->frame != g_frame) {   // (this draw's first time this frame: its transform taken)
        if (!s) s = &g_seen[key];
        s->haveBefore = s->frame && s->frame + 1 == g_frame;
        s->before = s->now;
        s->now = world;
        s->frame = g_frame;
        if (s->haveBefore && !Same(s->now, s->before)) g_visualsMoving++;   // (draws that moved, counted once a frame)
    }
    if (!s->haveBefore || Same(world, s->before)) return;   // still (or new: its motion is not known)
    // what writes no depth (a car's shadow on the road, glass, smoke): translucent over what is behind it, whose motion
    // is the one to keep (a shadow's own would move the road under it with the car)
    DWORD zwrite = TRUE;
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    if (!zwrite) return;
    if (!PositionFirst(d)) { g_skipped++; return; }
    D3DMATRIX view{}, proj{}, now, before;
    d->GetTransform(D3DTS_VIEW, &view);
    d->GetTransform(D3DTS_PROJECTION, &proj);
    Mul(world, view, now);
    Mul(now, proj, now);
    Mul(s->before, g_viewBefore, before);
    Mul(before, g_projBefore, before);
    // the device's state kept: the target, the viewport, the shaders, the vertex layout and the states the block holds
    IDirect3DSurface9 *rt = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf = 0;
    D3DVIEWPORT9 vp{};
    d->GetRenderTarget(0, &rt);
    d->GetViewport(&vp);
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    d->GetVertexDeclaration(&decl);
    d->GetFVF(&fvf);
    g_block->Capture();
    d->SetRenderTarget(0, g_target);
    d->SetViewport(&vp);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);   // (the object's own depth, just written: what of it is in front)
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetVertexShader(g_vs);
    d->SetPixelShader(g_ps);
    d->SetVertexDeclaration(g_decl);
    D3DSURFACE_DESC td{};
    g_target->GetDesc(&td);
    const float size[4] = {float(td.Width), float(td.Height), 1.0f / td.Width, 1.0f / td.Height};
    d->SetVertexShaderConstantF(4, &now._11, 4);
    d->SetVertexShaderConstantF(8, &before._11, 4);
    d->SetPixelShaderConstantF(0, size, 1);
    dip(d, t, base, minIndex, verts, start, n);
    g_drawsAgain++;
    d->SetRenderTarget(0, rt);
    d->SetViewport(&vp);
    g_block->Apply();
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    if (fvf) d->SetFVF(fvf);
    else d->SetVertexDeclaration(decl);
    SafeRelease(rt); SafeRelease(vs); SafeRelease(ps); SafeRelease(decl);
}
} // namespace

bool MotionWanted() {
    for (MmlMod *m : g_asks) if (ModAlive(m)) return true;
    return false;
}

int MotionAsk(MmlMod *mod, bool on) {
    g_asks.erase(std::remove(g_asks.begin(), g_asks.end(), mod), g_asks.end());
    if (on) g_asks.push_back(mod);
    std::string why;   // (LS3D's face groups and the device's draws: geom.cpp; again at each frame while they are not)
    return on && !(GeomEnsureHooks(&why) && GeomEnsureDip(&why)) ? 0 : 1;
}

void MotionRemove(MmlMod *mod) { MotionAsk(mod, false); }

// gfx.cpp, before the frame's world: the objects' target (cleared by gfx.cpp), the last frame's camera.
void MotionFrameBegin(void *dev9, void *target, const float *viewBefore, const float *projBefore) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_lastVisuals = g_visualsMoving, g_lastDraws = g_drawsAgain, g_lastSkipped = g_skipped, g_msLast = g_msNow;
    g_visualsMoving = g_drawsAgain = g_skipped = 0;
    g_msNow = 0;
    g_frame++;
    if (g_frame % 120 == 0)   // (the visuals gone: not seen for two seconds or so)
        for (auto it = g_seen.begin(); it != g_seen.end();) it = g_frame - it->second.frame > 120 ? g_seen.erase(it) : std::next(it);
    if (g_frame % 120 == 0)
        for (auto it = g_counts.begin(); it != g_counts.end();) it = g_frame - it->second.frame > 120 ? g_counts.erase(it) : std::next(it);
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    g_on = d && target && Make(d);
    g_dev = d;
    g_target = static_cast<IDirect3DSurface9 *>(target);
    g_haveCameraBefore = viewBefore && projBefore;
    if (g_haveCameraBefore) {
        memcpy(&g_viewBefore, viewBefore, sizeof g_viewBefore);
        memcpy(&g_projBefore, projBefore, sizeof g_projBefore);
    }
    g_on = g_on && g_haveCameraBefore;
}

void MotionFrameEnd() { g_on = false; g_target = nullptr; }

void MotionDeviceGone() { Release(); g_seen.clear(); g_counts.clear(); g_on = false; }

// geom.cpp: the face group LS3D draws now (kind 1 an object, 2 lightmapped, 3 a billboard), in the frame or not.
void MotionGroupBegin(int kind, const void *visual, const void *group, bool inFrame) {
    g_visual = g_on && inFrame && (kind == 1 || kind == 2) ? visual : nullptr;
    g_group = group;
    g_drawInGroup = 0;
    if (g_visual) {
        Count &c = g_counts[Pair{visual, group}];
        if (c.frame != g_frame) c.frame = g_frame, c.n = 0;
        g_occurrence = c.n++;
    }
}
void MotionGroupEnd() { g_visual = nullptr; }

// geom.cpp's DrawIndexedPrimitive hook, after the game's draw: the same draw into the objects' target if it moved.
void MotionAfterDraw(void *dev, int type, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned n, void *dip) {
    if (!g_visual || dev != g_dev) return;
    double t0 = NowMs();
    GfxQuiet(true);   // (the bridge's own draw: not the game's, for the fog's and the sun's tallies)
    __try {
        DrawAgain(static_cast<IDirect3DDevice9 *>(dev), static_cast<D3DPRIMITIVETYPE>(type), base, minIndex, verts, start, n,
                  reinterpret_cast<DipFn>(dip));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_skipped++;
    }
    g_drawInGroup++;
    GfxQuiet(false);
    g_msNow += NowMs() - t0;
}

std::string MotionState() {
    char b[256];
    std::string mods;
    for (MmlMod *m : g_asks) if (ModAlive(m)) mods += std::string(mods.empty() ? "" : ", ") + ModIdOf(m);
    snprintf(b, sizeof b, "{\"asked_by\": \"%s\", \"visuals_known\": %u, \"moving\": %u, \"drawn_again\": %u, \"left_out\": %u, \"ms\": %.3f}",
             mods.c_str(), static_cast<unsigned>(g_seen.size()), g_lastVisuals, g_lastDraws, g_lastSkipped, g_msLast);
    return b;
}

} // namespace host
