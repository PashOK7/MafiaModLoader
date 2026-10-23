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
// A person's limbs: LS3D skins a single mesh on the CPU and copies the positions into the LOD's drawn vertex buffer
// (game_build.h smCopySkinned, hooked here): the copy just made is kept for each mesh and LOD, and so is the one the
// frame's world drew the frame before. A skinned draw that moved goes again from a ring of our own, each vertex with
// both positions (VsMotionSkin), its BaseVertexIndex moved to the LOD's place in the ring (LS3D's draw gives the LOD's
// first vertex as its base: that is how the draw and the copy are matched).
//
// A compromise, said plainly: billboards (sprites that turn to the camera) move with the camera's motion. Only the
// frame's world drawing: scene views get none.
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <intrin.h>
#include <smmintrin.h>
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "shaders/VsMotion.h"
#include "shaders/VsMotionSkin.h"
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
bool g_shadow;   // the group is a shadow's (kind 4): its picture moves with its shadow projector, not with its faces
constexpr int kShadowOwner = 0x1E0;   // the shadow visual: its projector (game_build.h fgRenderShadow)
constexpr float kShadowSeen = 0.2f;   // a shadow's alpha from which its own motion is taken
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

// A single mesh's LOD as LS3D last copied its skinned positions into the drawn buffer (cur), and as the frame's world
// drew it the last time (drawn: cur until a copy comes after that draw). Its vertices keep their order whatever place
// the LOD has in the pooled buffer (first: the draw's BaseVertexIndex).
struct SkinKey {
    const void *mesh;
    int lod;
    bool operator==(const SkinKey &o) const { return mesh == o.mesh && lod == o.lod; }
};
struct SkinKeyHash {
    size_t operator()(const SkinKey &k) const { return reinterpret_cast<size_t>(k.mesh) * 0x9E3779B1u ^ static_cast<size_t>(k.lod); }
};
struct Skin {
    unsigned first = 0, count = 0;
    std::vector<float> cur, drawn;     // xyz each
    unsigned curVersion = 0, drawnVersion = 0;
    unsigned drawnFrame = 0;           // the frame (g_frame) the world drew it last
    unsigned ringFrame = 0;            // this frame's copy in the ring: where, and whether its limbs moved
    UINT ringAt = 0;
    bool ringed = false, limbs = false;
    unsigned seen = 0;                 // copied or drawn last (g_frame): an entry not seen for a while goes
};
std::unordered_map<SkinKey, Skin, SkinKeyHash> g_skins;
constexpr int kSmFlags = 0x1D8, kSmMesh = 0x1E0, kSmLods = 0x1E4;
constexpr int kLodFirst = 0x18, kLodRigid = 0x30, kLodCache = 0x34, kLodCacheFirst = 0x3C;   // a LOD's record
constexpr int kMeshLods = 0x0C, kMeshLodCount = 0x08, kMeshLodBuffer = 0x0C, kMeshLodFirst = 0x14;   // the I3D_mesh's
constexpr int kMaxLods = 8;
using CopyFn = void(__fastcall *)(void *mesh, void *edx, uint8_t *dest, int stride, int lod);
CopyFn oCopySkinned;
bool g_skinHookTried, g_skinHooked;
std::string g_skinHookWhy;

// Direct3D 9 objects
IDirect3DVertexShader9 *g_vs, *g_vsSkin;
IDirect3DPixelShader9 *g_ps;
IDirect3DVertexDeclaration9 *g_declSkin;
IDirect3DStateBlock9 *g_block;
IDirect3DDevice9 *g_madeOn;
// the skinned draws' vertices, this frame's position and the last frame's (24 bytes), appended through the frames
IDirect3DVertexBuffer9 *g_ring;
constexpr UINT kRingVerts = 1u << 17, kRingStride = 24;
UINT g_ringAt;
bool g_ringFresh = true;

// statistics
unsigned g_visualsMoving, g_drawsAgain, g_skipped, g_skinnedAgain;
unsigned g_lastVisuals, g_lastDraws, g_lastSkipped, g_lastSkinned;
unsigned g_shadowsSeen, g_shadowsAgain, g_lastShadowsSeen, g_lastShadowsAgain;   // shadows' draws: met, drawn again
unsigned g_copiesStreamed;          // copies read from the drawn buffer (a source not locked)
double g_msCopies, g_msCopiesLast; // the skinned copies kept since the last frame began
unsigned g_limbLods, g_lastLimbLods;  // skinned LODs whose limbs moved
float g_limbMax, g_lastLimbMax;      // the most a vertex moved in its mesh (metres; every 8th vertex looked at)
double g_msNow, g_msLast;
LARGE_INTEGER g_freq;
double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * t.QuadPart / g_freq.QuadPart; }

void Release() {
    SafeRelease(g_vs); SafeRelease(g_vsSkin); SafeRelease(g_ps); SafeRelease(g_declSkin);
    SafeRelease(g_block); SafeRelease(g_ring);
    g_madeOn = nullptr;
    g_ringFresh = true;
}

bool Make(IDirect3DDevice9 *d) {
    if (g_madeOn == d && g_vs) return true;
    Release();
    const D3DVERTEXELEMENT9 skin[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 1}, D3DDECL_END()};
    if (FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsMotion), &g_vs)) ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsMotionSkin), &g_vsSkin)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsMotion), &g_ps)) ||
        FAILED(d->CreateVertexDeclaration(skin, &g_declSkin)) ||
        FAILED(d->CreateVertexBuffer(kRingVerts * kRingStride, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_ring, nullptr))) {
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

// an affine matrix's inverse (row vectors: rotation and scale in the 3x3, the translation in the last row)
D3DMATRIX AffineInverse(const D3DMATRIX &m) {
    const float a = m._11, b = m._12, c = m._13, d = m._21, e = m._22, f = m._23, g = m._31, h = m._32, i = m._33;
    const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    float det = a * A + b * B + c * C;
    det = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
    D3DMATRIX r{};
    r._11 = A * det, r._12 = (c * h - b * i) * det, r._13 = (b * f - c * e) * det;
    r._21 = B * det, r._22 = (a * i - c * g) * det, r._23 = (c * d - a * f) * det;
    r._31 = C * det, r._32 = (b * g - a * h) * det, r._33 = (a * e - b * d) * det;
    r._41 = -(m._41 * r._11 + m._42 * r._21 + m._43 * r._31);
    r._42 = -(m._41 * r._12 + m._42 * r._22 + m._43 * r._32);
    r._43 = -(m._41 * r._13 + m._42 * r._23 + m._43 * r._33);
    r._44 = 1;
    return r;
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

// The draw's alpha test as PsMotion repeats it (its reference 0..1, or -1): the stage-0 texture's alpha against the
// reference, where the test passes greater (LS3D's cut-out textures: a wheel's spokes, a fence; without it the
// quad's clear parts gave the objects' target their depth over what is seen through them). The diffuse alpha the
// stage may modulate it by is left out.
float AlphaCut(IDirect3DDevice9 *d) {
    DWORD on = FALSE, ref = 0, func = 0, op = 0, a1 = 0, a2 = 0, index = 0;
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &on);
    if (!on) return -1;
    d->GetRenderState(D3DRS_ALPHAREF, &ref);
    d->GetRenderState(D3DRS_ALPHAFUNC, &func);
    d->GetTextureStageState(0, D3DTSS_ALPHAOP, &op);
    d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &a1);
    d->GetTextureStageState(0, D3DTSS_ALPHAARG2, &a2);
    d->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &index);
    const bool tex1 = (a1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE, tex2 = (a2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    const bool fromTex = op == D3DTOP_SELECTARG1 ? tex1 : op == D3DTOP_SELECTARG2 ? tex2 : op != D3DTOP_DISABLE && (tex1 || tex2);
    if (!fromTex || index != 0 || (func != D3DCMP_GREATER && func != D3DCMP_GREATEREQUAL)) return -1;
    IDirect3DBaseTexture9 *t = nullptr;
    d->GetTexture(0, &t);
    if (!t) return -1;
    t->Release();
    return ((ref & 0xFF) + (func == D3DCMP_GREATER ? 0.5f : 0.0f)) / 255.0f;   // (clip keeps what is not below it)
}

template <class T> T At(const void *p, int off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off); }

// The drawn buffer, read only when a copy's sources are not locked: it is video memory the CPU reads uncached (a
// person's LOD took 1.5 to 1.8 ms). SSE4.1's streaming loads read it whole lines at once where the memory is
// write-combined (they did not help on the lab's RTX 3070); the range is read 16-byte aligned (an aligned block
// never crosses a page) into a buffer of our own.
std::vector<__m128i> g_wc;
int g_sse41 = -1;
const uint8_t *ReadLocked(const uint8_t *src, size_t n) {
    if (g_sse41 < 0) {
        int r[4];
        __cpuid(r, 1);
        g_sse41 = (r[2] >> 19) & 1;
    }
    uintptr_t from = reinterpret_cast<uintptr_t>(src) & ~uintptr_t(15), to = (reinterpret_cast<uintptr_t>(src) + n + 15) & ~uintptr_t(15);
    size_t blocks = (to - from) / 16;
    if (g_wc.size() < blocks) g_wc.resize(blocks);
    auto *in = reinterpret_cast<__m128i *>(from);
    if (g_sse41)
        for (size_t i = 0; i < blocks; i++) g_wc[i] = _mm_stream_load_si128(in + i);
    else
        memcpy(g_wc.data(), in, blocks * 16);
    return reinterpret_cast<const uint8_t *>(g_wc.data()) + (reinterpret_cast<uintptr_t>(src) - from);
}

// smCopySkinned has just copied a LOD's skinned positions (its rigid ones were copied just before): the LOD's whole
// range in the drawn buffer is what will be drawn, kept here (the cur before it, if the world drew that one, as drawn).
void KeepCopy(const void *mesh, const uint8_t *dest, int stride, int lod) {
    if (lod < 0 || lod >= kMaxLods || stride < 12 || !dest) return;
    const void *m = mesh;   // (an instance shares its mesh's LODs: walked up as the copy does)
    for (int i = 0; i < 8 && (At<uint32_t>(m, kSmFlags) & 0x10000); i++) m = At<const void *>(m, 0x08);
    const void *rec = At<const void *>(m, kSmLods + 4 * lod), *data = At<const void *>(m, kSmMesh);
    const void *lodData = data ? At<const void *>(data, kMeshLods + 4 * lod) : nullptr;
    if (!rec || !lodData) return;
    unsigned count = At<uint32_t>(lodData, kMeshLodCount);
    if (!count || count > kRingVerts) return;
    Skin &s = g_skins[SkinKey{mesh, lod}];
    if (s.count != count) s = Skin{};   // (another mesh at a freed address)
    s.count = count;
    s.first = At<uint32_t>(rec, kLodFirst);
    if (s.curVersion == s.drawnVersion && s.drawnFrame) std::swap(s.cur, s.drawn);   // (what the world drew stays)
    s.cur.resize(count * 3);
    // read where the copies read (the cache the CPU skinned into, the mesh's own buffer: both system memory the game
    // reads itself), not the drawn buffer (video memory the CPU reads uncached: 1.8 ms a person)
    const int vbStride = game::Ls3d().vbStride;
    const void *rigidPart = At<const void *>(rec, kLodRigid), *cache = At<const void *>(rec, kLodCache);
    const void *own = At<const void *>(lodData, kMeshLodBuffer);
    unsigned rigid = rigidPart ? At<uint32_t>(rigidPart, 0x0C) : 0, skinned = count - std::min(rigid, count);
    const uint8_t *cacheAt = cache ? At<const uint8_t *>(cache, 0) : nullptr, *ownAt = own ? At<const uint8_t *>(own, 0) : nullptr;
    int cacheStride = cache ? At<int>(cache, vbStride) : 0, ownStride = own ? At<int>(own, vbStride) : 0;
    if (rigid <= count && (!skinned || (cacheAt && cacheStride >= 12)) && (!rigid || (ownAt && ownStride >= 12))) {
        const uint8_t *from = skinned ? cacheAt + size_t(At<uint32_t>(rec, kLodCacheFirst)) * cacheStride : nullptr;
        for (unsigned i = 0; i < skinned; i++) memcpy(&s.cur[i * 3], from + size_t(i) * cacheStride, 12);
        from = rigid ? ownAt + size_t(At<uint32_t>(lodData, kMeshLodFirst) + skinned) * ownStride : nullptr;
        for (unsigned i = 0; i < rigid; i++) memcpy(&s.cur[(skinned + i) * 3], from + size_t(i) * ownStride, 12);
    } else {   // (a source not locked: the drawn copy, streamed)
        const uint8_t *v = ReadLocked(dest, size_t(count - 1) * stride + 12);
        for (unsigned i = 0; i < count; i++) memcpy(&s.cur[i * 3], v + size_t(i) * stride, 12);
        g_copiesStreamed++;
    }
    s.curVersion++;
    s.seen = g_frame;
}

void __fastcall hkCopySkinned(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    oCopySkinned(mesh, edx, dest, stride, lod);
    if (g_asks.empty() || SkinGpuDrawsLod(mesh, lod)) return;   // (a LOD the GPU skins: its draw has both palettes)
    double t0 = NowMs();
    __try {
        KeepCopy(mesh, dest, stride, lod);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_skipped++;
    }
    g_msCopies += NowMs() - t0;
}

bool EnsureSkinHook() {
    if (g_skinHookTried) return g_skinHooked;
    if (!game::g_ls3d) { g_skinHookWhy = "no game known"; return false; }
    g_skinHookTried = true;
    if (!game::Ls3d().smCopySkinned) { g_skinHookWhy = "not known for this LS3DF.dll"; return false; }
    std::string err;
    if (!HookAdd(nullptr, ModuleBase("LS3DF.dll") + game::Ls3d().smCopySkinned, reinterpret_cast<void *>(&hkCopySkinned),
                 reinterpret_cast<void **>(&oCopySkinned), &err)) {
        g_skinHookWhy = "smCopySkinned not hooked: " + err;
        HLOG("motion: %s (people's limbs move with them as wholes)", g_skinHookWhy.c_str());
        return false;
    }
    g_skinHooked = true;
    return true;
}

// The skinned LOD a draw of the visual is (LS3D's draw gives the LOD's first vertex as its base), its positions now and
// as drawn the frame before put in the ring once a frame when the draw moved (`moved`: its transform did). Null: not one.
Skin *SkinOf(INT base, bool moved) {
    if (g_skins.empty()) return nullptr;   // (only single meshes are there: 'SNGM', and 'SMMR' the people, morphed faces)
    Skin *s = nullptr;
    for (int lod = 0; lod < kMaxLods && !s; lod++) {
        auto it = g_skins.find(SkinKey{g_visual, lod});
        if (it != g_skins.end() && it->second.first == static_cast<unsigned>(base) && it->second.cur.size()) s = &it->second;
    }
    if (!s) return nullptr;
    if (s->ringFrame != g_frame) {   // (its first draw this frame)
        s->ringFrame = g_frame;
        s->ringed = false;
        bool before = s->drawnFrame && s->drawnFrame + 1 == g_frame && s->curVersion != s->drawnVersion;
        const std::vector<float> &now = s->cur, &was = before && s->drawn.size() == s->cur.size() ? s->drawn : s->cur;
        s->limbs = &was != &now;
        if (s->limbs) {
            g_limbLods++;
            for (unsigned i = 0; i < s->count; i += 8) {
                float dx = now[i * 3] - was[i * 3], dy = now[i * 3 + 1] - was[i * 3 + 1], dz = now[i * 3 + 2] - was[i * 3 + 2];
                g_limbMax = std::max(g_limbMax, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        s->drawnFrame = g_frame;
        s->drawnVersion = s->curVersion;
        s->seen = g_frame;
        if ((s->limbs || moved) && g_ring) {
            DWORD flags = D3DLOCK_NOOVERWRITE;
            if (g_ringFresh || g_ringAt + s->count > kRingVerts) g_ringAt = 0, flags = D3DLOCK_DISCARD, g_ringFresh = false;
            float *p = nullptr;
            if (SUCCEEDED(g_ring->Lock(g_ringAt * kRingStride, s->count * kRingStride, reinterpret_cast<void **>(&p), flags)) && p) {
                for (unsigned i = 0; i < s->count; i++) memcpy(p + i * 6, &now[i * 3], 12), memcpy(p + i * 6 + 3, &was[i * 3], 12);
                g_ring->Unlock();
                s->ringAt = g_ringAt;
                s->ringed = true;
                g_ringAt += s->count;
            }
        }
    }
    return s;
}

void DrawAgain(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip) {
    D3DMATRIX world{};
    if (FAILED(d->GetTransform(D3DTS_WORLD, &world))) return;
    // what the draw moves with: its world matrix, or a shadow's projector's (the faces under it are the road's, its
    // picture is the car's)
    D3DMATRIX track = world;
    if (g_shadow) {
        g_shadowsSeen++;
        const uint8_t *owner = At<const uint8_t *>(g_visual, kShadowOwner);
        if (!owner) return;
        std::memcpy(&track, owner + game::kFrameWorld, sizeof track);
    }
    const Key key{g_visual, g_group, g_occurrence << 8 | (g_drawInGroup & 0xFF)};
    auto it = g_seen.find(key);
    Seen *s = it == g_seen.end() ? nullptr : &it->second;
    if (!s || s->frame != g_frame) {   // (this draw's first time this frame: its transform taken)
        if (!s) s = &g_seen[key];
        s->haveBefore = s->frame && s->frame + 1 == g_frame;
        s->before = s->now;
        s->now = track;
        s->frame = g_frame;
        if (s->haveBefore && !Same(s->now, s->before)) g_visualsMoving++;   // (draws that moved, counted once a frame)
    }
    if (!s->haveBefore) return;   // new: its motion is not known
    bool moved = !Same(track, s->before);
    SkinGpuDraw gpu;
    const bool gpuSkin = !g_shadow && SkinGpuFor(d, g_visual, base, &gpu);   // (a person the GPU skins: both palettes in his shader)
    Skin *skin = gpuSkin || g_shadow ? nullptr : SkinOf(base, moved);   // (a person: his limbs' own motion)
    if (skin && !skin->ringed) skin = nullptr;
    if (!moved && !skin && !gpuSkin) return;   // still
    // what writes no depth (a car's shadow on the road, its glass) is drawn too, under the frame's own depth test:
    // PsMotionAll keeps it where its depth is what is seen (the shadow on the road, not glass over the street far
    // behind it). Its own depth function: it was not written by it
    DWORD zwrite = TRUE, zfunc = D3DCMP_LESSEQUAL;
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
    d->GetRenderState(D3DRS_ZFUNC, &zfunc);
    if (!PositionFirst(d)) { g_skipped++; return; }
    // (a shadow's picture is blended by its alpha over the whole box of faces under it: its own motion where it
    // darkens the road enough to be seen, the road's elsewhere)
    const float cut[4] = {skin ? -1.0f : g_shadow ? (std::max)(AlphaCut(d), kShadowSeen) : AlphaCut(d), 0, 0, 0};
    D3DMATRIX view{}, proj{}, now, before;
    d->GetTransform(D3DTS_VIEW, &view);
    d->GetTransform(D3DTS_PROJECTION, &proj);
    Mul(world, view, now);
    Mul(now, proj, now);
    if (g_shadow) {   // (a point of the faces where the picture's place on them was: through its shadow's move back)
        Mul(world, AffineInverse(track), before);
        Mul(before, s->before, before);
    } else {
        before = s->before;
    }
    Mul(before, g_viewBefore, before);
    Mul(before, g_projBefore, before);
    // the device's state kept: the target, the viewport, the shaders, the vertex layout and the states the block holds
    IDirect3DSurface9 *rt = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DVertexBuffer9 *vb = nullptr;
    UINT vbOff = 0, vbStride = 0;
    DWORD fvf = 0;
    D3DVIEWPORT9 vp{};
    if (skin) d->GetStreamSource(0, &vb, &vbOff, &vbStride);
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
    d->SetRenderState(D3DRS_ZFUNC, zwrite ? D3DCMP_LESSEQUAL : zfunc);   // (the object's own depth, just written: what of it is in front)
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
    d->SetVertexShader(skin ? g_vsSkin : g_vs);
    d->SetPixelShader(g_ps);
    if (skin) d->SetVertexDeclaration(g_declSkin);   // (else the draw's own FVF: its position and its texture's place)
    if (skin) d->SetStreamSource(0, g_ring, 0, kRingStride);
    D3DSURFACE_DESC td{};
    g_target->GetDesc(&td);
    const float size[4] = {float(td.Width), float(td.Height), 1.0f / td.Width, 1.0f / td.Height};
    d->SetVertexShaderConstantF(4, &now._11, 4);
    d->SetVertexShaderConstantF(8, &before._11, 4);
    d->SetPixelShaderConstantF(0, size, 1);
    d->SetPixelShaderConstantF(2, cut, 1);
    if (gpuSkin) {
        SkinGpuSaved k{};
        SkinGpuBind(d, gpu, &k);
        d->SetVertexShader(static_cast<IDirect3DVertexShader9 *>(SkinGpuShader(3)));
        // (now: the world, view and projection one by one at c0..c11, as VsSkin drew them; c4 and c8 above are VsMotion's)
        d->SetVertexShaderConstantF(0, &world._11, 4);
        d->SetVertexShaderConstantF(4, &view._11, 4);
        d->SetVertexShaderConstantF(8, &proj._11, 4);
        d->SetVertexShaderConstantF(164, &before._11, 4);
        dip(d, t, 0, minIndex, verts, start, n);   // (the LOD's vertices from 0 in the skin's buffers)
        SkinGpuUnbind(d, k);
        g_skinnedAgain++;
    } else {
        dip(d, t, skin ? static_cast<INT>(skin->ringAt) : base, minIndex, verts, start, n);   // (the LOD's vertices in the ring)
    }
    g_drawsAgain++;
    if (g_shadow) g_shadowsAgain++;
    if (skin) g_skinnedAgain++;
    d->SetRenderTarget(0, rt);
    d->SetViewport(&vp);
    g_block->Apply();
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    if (fvf) d->SetFVF(fvf);
    else d->SetVertexDeclaration(decl);
    if (skin) d->SetStreamSource(0, vb, vbOff, vbStride);
    SafeRelease(rt); SafeRelease(vs); SafeRelease(ps); SafeRelease(decl); SafeRelease(vb);
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
    if (on) EnsureSkinHook();
    return on && !(GeomEnsureHooks(&why) && GeomEnsureDip(&why)) ? 0 : 1;
}

void MotionRemove(MmlMod *mod) { MotionAsk(mod, false); }

// gfx.cpp, before the frame's world: the objects' target (cleared by gfx.cpp), the last frame's camera.
void MotionFrameBegin(void *dev9, void *target, const float *viewBefore, const float *projBefore) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_lastVisuals = g_visualsMoving, g_lastDraws = g_drawsAgain, g_lastSkipped = g_skipped, g_lastSkinned = g_skinnedAgain;
    g_lastShadowsSeen = g_shadowsSeen, g_lastShadowsAgain = g_shadowsAgain;
    g_shadowsSeen = g_shadowsAgain = 0;
    g_msLast = g_msNow + g_msCopies, g_msCopiesLast = g_msCopies;
    g_lastLimbLods = g_limbLods, g_lastLimbMax = g_limbMax, g_limbLods = 0, g_limbMax = 0;
    g_visualsMoving = g_drawsAgain = g_skipped = g_skinnedAgain = 0;
    g_msNow = g_msCopies = 0;
    g_frame++;
    if (!g_skinHookTried) EnsureSkinHook();   // (a mod that asked before the game was known)
    if (g_frame % 120 == 0)   // (the visuals gone: not seen for two seconds or so)
        for (auto it = g_seen.begin(); it != g_seen.end();) it = g_frame - it->second.frame > 120 ? g_seen.erase(it) : std::next(it);
    if (g_frame % 120 == 0)
        for (auto it = g_counts.begin(); it != g_counts.end();) it = g_frame - it->second.frame > 120 ? g_counts.erase(it) : std::next(it);
    if (g_frame % 120 == 0)
        for (auto it = g_skins.begin(); it != g_skins.end();) it = g_frame - it->second.seen > 120 ? g_skins.erase(it) : std::next(it);
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

void MotionDeviceGone() { Release(); g_seen.clear(); g_counts.clear(); g_skins.clear(); g_on = false; }

// geom.cpp: the face group LS3D draws now (kind 1 an object, 2 lightmapped, 3 a billboard), in the frame or not.
void MotionGroupBegin(int kind, const void *visual, const void *group, bool inFrame) {
    g_visual = g_on && inFrame && (kind == 1 || kind == 2 || kind == 4) ? visual : nullptr;
    g_shadow = kind == 4;
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
    char b[512];
    std::string mods;
    for (MmlMod *m : g_asks) if (ModAlive(m)) mods += std::string(mods.empty() ? "" : ", ") + ModIdOf(m);
    snprintf(b, sizeof b,
             "{\"asked_by\": \"%s\", \"visuals_known\": %u, \"moving\": %u, \"drawn_again\": %u, \"skinned_again\": %u, "
             "\"skinned_known\": %u, \"limbs_moving\": %u, \"limbs_most_m\": %.3f, \"skin_hook\": \"%s\", \"left_out\": %u, "
             "\"ms\": %.3f, \"ms_copies\": %.3f, \"copies_streamed\": %u, \"shadows\": %u, \"shadows_again\": %u}",
             mods.c_str(), static_cast<unsigned>(g_seen.size()), g_lastVisuals, g_lastDraws, g_lastSkinned,
             static_cast<unsigned>(g_skins.size()), g_lastLimbLods, g_lastLimbMax, g_skinHooked ? "on" : g_skinHookWhy.c_str(),
             g_lastSkipped, g_msLast, g_msCopiesLast, g_copiesStreamed, g_lastShadowsSeen, g_lastShadowsAgain);
    return b;
}

} // namespace host
