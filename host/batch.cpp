// The world's static geometry drawn in batches (render.set's batch; ROADMAP items 31f, 31g): fewer draws, the vertices
// kept in the host's own buffers.
//
// LS3D draws a frame's world a face group at a time (I3D_object::m_RenderFGroup: its states through LS3D's own cache of
// them, the LOD's vertices copied into a pooled vertex buffer, one DrawIndexedPrimitive): some 35 000 draws a frame for
// the whole city, each costing LS3D, the runtime and the driver. A static object's group lit by LS3D (its colours made
// on the CPU when the lights change, not each frame) is the same from frame to frame, so:
//   - capture: the first time LS3D draws a group of an object (kind 1: I3D_object, not blended), its vertices are read
//     at its draw (the pooled buffer, the draw's index range), put into the world's space by the object's world
//     matrix, and appended to the batch of its state (the textures, the stages' operations, the alpha test, culling,
//     depth, the samplers: `Sig`). A group drawn in more than one draw, or not as a triangle list, is left to LS3D;
//   - each frame after: LS3D's call for a captured group (its world matrix, its LOD's record and the lights' stamp
//     there, its texture of this frame (an animated material's), its being opaque the same) is not made: the group is
//     marked asked for. Anything else differing: LS3D draws it, and it is captured again;
//   - before the first blended draw of the frame (the translucent ones are drawn last, over what is opaque) and at the
//     end of the frame's world, every batch asked for is drawn in one DrawIndexedPrimitive, its index buffer made of
//     the groups asked for this frame (LS3D's own visibility: what it would not have drawn is not drawn), the batches
//     in the order LS3D asked for them (its sort: a decal after the wall it lies on). The world matrix is the
//     identity (the vertices are the world's), the fixed pipeline does the rest as for LS3D's draw (its vertex fog
//     from the view's depth, the stages, the alpha test); every state changed is put back after (LS3D's cache of the
//     states must stay true).
// Only the frame's own world (not a scene view's, not the shadow map's), only while no shader rule draws (the fixed
// pipeline's: the rules' batches are later work). Everything is dropped when the mission closes or the device changes.
#include "host.h"
#include "d3dmirror.h"
#include <d3d9.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// (+0x08: Render's setNewClipState, D3DRS_CLIPPING on for an entry crossing the frustum's sides; the batches are drawn
// with it on, always right)
constexpr int kEntryGroup = 0x10, kEntryTexture = 0x18;
constexpr int kGroupMaterial = 0x04, kMaterialFlags = 0x08, kMaterialAlpha = 0x38;
constexpr int kVisFlags = 0x1D8, kVisSource = 0x08, kVisLods = 0x1E4, kVisLod = 0x20C, kVisFade = 0x214, kVisFade2 = 0x218;
constexpr uint32_t kVisInstance = 0x10000;
// a visual's class (+0x1D4): only I3D_object's own ('OBJ_' in memory) are batched; the people's single meshes, the
// morphs, the billboards come through the same m_RenderFGroup or move each frame
constexpr int kVisType = 0x1D4;
constexpr uint32_t kTypeObject = 0x5F4A424Fu;
// a material's blending (I3D_object::m_RenderFGroup): additive (the sign bit), blended (0x40000000)
constexpr uint32_t kMatAdditive = 0x80000000u, kMatBlend = 0x40000000u;

template <class T> T &At(const void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(const_cast<void *>(p)) + off); }

// What a batch's draws share: the device's state at the captured draw that the drawing reads
constexpr D3DRENDERSTATETYPE kSigRs[] = {
    D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_CULLMODE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
    D3DRS_ZFUNC, D3DRS_FOGENABLE, D3DRS_LIGHTING, D3DRS_TEXTUREFACTOR, D3DRS_COLORVERTEX, D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_COLORWRITEENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND, D3DRS_DITHERENABLE, D3DRS_STENCILENABLE,
    // the fog: LS3D sets it a render list entry from the visual's sector (Render's setNewFogState: its colour, start,
    // end, density, kind); kFogFirst on, compared at each request too
    D3DRS_FOGCOLOR, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_FOGVERTEXMODE, D3DRS_FOGTABLEMODE,
    D3DRS_RANGEFOGENABLE,
};
constexpr int kFogFirst = 20;   // (kSigRs' index of D3DRS_FOGCOLOR)
constexpr D3DTEXTURESTAGESTATETYPE kSigTss[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
};
constexpr D3DSAMPLERSTATETYPE kSigSamp[] = {
    D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS,
    D3DSAMP_MAXANISOTROPY, D3DSAMP_MAXMIPLEVEL,
};
constexpr int kNRs = sizeof kSigRs / sizeof *kSigRs, kNTss = sizeof kSigTss / sizeof *kSigTss, kNSamp = sizeof kSigSamp / sizeof *kSigSamp;
struct Sig {
    IDirect3DBaseTexture9 *tex[2];
    DWORD rs[kNRs], tss[2][kNTss], samp[2][kNSamp];
    bool operator==(const Sig &o) const { return !memcmp(this, &o, sizeof *this); }
};
struct SigHash {
    size_t operator()(const Sig &s) const {
        const auto *p = reinterpret_cast<const uint32_t *>(&s);
        size_t h = 2166136261u;
        for (size_t i = 0; i < sizeof s / 4; i++) h = (h ^ p[i]) * 16777619u;
        return h;
    }
};
Sig SigOf(const D3dMirror &m) {
    Sig s;
    memset(&s, 0, sizeof s);
    s.tex[0] = m.tex[0];
    s.tex[1] = m.tex[1];
    for (int i = 0; i < kNRs; i++) s.rs[i] = m.rs[kSigRs[i]];
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < kNTss; i++) s.tss[t][i] = m.tss[t][kSigTss[i]];
        for (int i = 0; i < kNSamp; i++) s.samp[t][i] = m.samp[t][kSigSamp[i]];
    }
    return s;
}

struct Vtx { float pos[3]; DWORD color; float uv0[2], uv1[2]; };   // (FVF XYZ | DIFFUSE | TEX2)
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;

struct Batch;
// A face group of an object, captured
struct Member {
    const void *visual, *group;
    Batch *batch = nullptr;
    uint32_t firstIdx = 0, nIdx = 0;   // its indices in the batch's (already the batch's vertex numbers)
    float world[12];                   // the object's world matrix's three axes and its place when captured
    const void *lodRec = nullptr;      // its LOD's record, and the lights' stamps there (LightVertices: relit)
    uint32_t stamp[2] = {};
    const void *texture = nullptr;     // the entry's texture (an animated material's frame), and its Direct3D 8 one (+8,
    const void *texture8 = nullptr;    // what m_RenderFGroup binds): both compared (an address taken again by another)
    unsigned askedFrame = 0;
    bool dead = false;
};
struct Batch {
    Sig sig;
    uint32_t fogKey = 0;               // (FogKey of its state: compared at each request)
    std::vector<Vtx> verts;
    std::vector<uint32_t> idx;
    IDirect3DVertexBuffer9 *vb = nullptr;
    UINT vbVerts = 0;                  // what vb holds (made again when the batch grew)
    IDirect3DIndexBuffer9 *ib = nullptr;
    UINT ibCap = 0, ibCount = 0;       // its capacity and this frame's indices
    std::vector<Member *> asked, askedLast;   // the groups asked for this frame, and the last frame's drawn
    unsigned askedFrame = 0, order = 0;
    uint32_t deadIdx = 0;
};

struct Key { const void *visual, *group; bool operator==(const Key &o) const { return visual == o.visual && group == o.group; } };
struct KeyHash { size_t operator()(const Key &k) const { return std::hash<const void *>()(k.visual) * 31 ^ std::hash<const void *>()(k.group); } };

// The fog of a state as one number (FNV of D3DRS_FOGENABLE and the fog's own: Render sets them an entry)
template <class Rs> uint32_t FogKey(const Rs &rs, bool fromSig) {
    uint32_t h = 2166136261u;
    for (int i = 7; i < kNRs; i++) {
        if (i != 7 && i < kFogFirst) continue;
        h = (h ^ (fromSig ? rs[i] : rs[kSigRs[i]])) * 16777619u;
    }
    return h;
}

bool g_on;
IDirect3DDevice9 *g_dev;
std::unordered_map<Key, std::unique_ptr<Member>, KeyHash> g_members;   // (they own the members)
// the members found by their key at each request (some 30 000 a frame): open addressing, a power of two, half full at
// most, made again when it fills or members are forgotten
struct Slot { const void *visual, *group; Member *m; };
std::vector<Slot> g_flat;
size_t g_flatUsed;
inline size_t SlotOf(const void *v, const void *g, size_t mask) {
    uint32_t x = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v)) * 0x9E3779B1u ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g) >> 2) * 0x85EBCA6Bu;
    return (x ^ (x >> 15)) & mask;
}
void FlatRebuild() {
    size_t n = 1024;
    while (n < g_members.size() * 2 + 2) n *= 2;
    g_flat.assign(n, Slot{nullptr, nullptr, nullptr});
    g_flatUsed = 0;
    for (auto &kv : g_members) {
        size_t i = SlotOf(kv.first.visual, kv.first.group, n - 1);
        while (g_flat[i].m) i = (i + 1) & (n - 1);
        g_flat[i] = {kv.first.visual, kv.first.group, kv.second.get()};
        g_flatUsed++;
    }
}
Member *FlatFind(const void *v, const void *g) {
    if (g_flat.empty()) return nullptr;
    const size_t mask = g_flat.size() - 1;
    for (size_t i = SlotOf(v, g, mask);; i = (i + 1) & mask) {
        const Slot &s = g_flat[i];
        if (!s.m) return nullptr;
        if (s.visual == v && s.group == g) return s.m;
    }
}
void FlatPut(const void *v, const void *g, Member *m) {
    if (g_flat.empty() || (g_flatUsed + 1) * 2 > g_flat.size()) { FlatRebuild(); return; }   // (it is in g_members already)
    const size_t mask = g_flat.size() - 1;
    size_t i = SlotOf(v, g, mask);
    while (g_flat[i].m && !(g_flat[i].visual == v && g_flat[i].group == g)) i = (i + 1) & mask;
    if (!g_flat[i].m) g_flatUsed++;
    g_flat[i] = {v, g, m};
}
std::unordered_map<Sig, std::unique_ptr<Batch>, SigHash> g_batches;
std::unordered_map<Key, unsigned, KeyHash> g_refused;   // groups left to LS3D for good (why: their count of refusals)
// objects left to LS3D whole: one group of theirs is LS3D's (an environment map's, a clipped one), so all of them are,
// in LS3D's order (the layers of one object lie on each other: a facade's glass and its frame; drawn apart, the batch's
// part before or after LS3D's, the wrong one won)
std::unordered_map<const void *, unsigned> g_refusedVisuals;
// an object's last capture undone by its moving (the frame): two within a second make it LS3D's (a moving object); a
// city block's pooled instance put elsewhere by the streaming moves once, rarely
std::unordered_map<const void *, unsigned> g_moves;
std::vector<Batch *> g_asked;                           // the batches asked for this frame, in LS3D's order
unsigned g_frame;
bool g_drawing;                                         // (our own draws, through the device's chain)
// the capture under way: the group LS3D draws now, its draws seen
struct Capture { bool on; const void *visual, *group, *texture, *texture8; int draws; bool ok; std::unique_ptr<Member> m; Sig sig; };
Capture g_cap;
// counted
uint64_t g_taken, g_captured, g_recaptured, g_refusedCount, g_flushes;
// why groups were LS3D's this frame and the last (BatchGroup's answers 0): render.state's batch.left
enum { kLeftNotFrame, kLeftView, kLeftShaders, kLeftType, kLeftRefused, kLeftBlended, kLeftBudget, kLeftMoved, kLeftCount };
unsigned g_left[kLeftCount], g_leftLast[kLeftCount];
// why captured groups were found changed (captured again), all frames: render.state's batch.changed
enum { kDiedFog, kDiedWorld, kDiedLod, kDiedStamp1, kDiedStamp2, kDiedTexture, kDiedCount };
uint64_t g_died[kDiedCount];
const char *kDiedNames[kDiedCount] = {"fog", "moved", "another_lod", "lights_stamp", "second_stamp", "texture"};
const char *kLeftNames[kLeftCount] = {"not_the_frame", "a_view", "shaders_or_motion", "not_an_object", "refused", "blended", "budget", "moved"};
unsigned g_drawsLast, g_drawsNow, g_takenLast, g_takenNow;
// the captures' time this frame: past kCaptureBudget the groups not captured yet are LS3D's for this frame (reading the
// pooled buffers back is slow: the whole city at once held the game thread for seconds)
constexpr double kCaptureBudgetMs = 3.0;
double g_captureMs;
LARGE_INTEGER g_qpf;
double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}
double g_capStart;

const void *LodRecord(const void *visual) {
    const void *v = visual;
    if (At<uint32_t>(v, kVisFlags) & kVisInstance) v = At<const void *>(v, kVisSource);
    const int lod = At<int>(v, kVisLod);
    if (lod < 0 || lod > 8) return nullptr;
    return At<const void *>(v, kVisLods + 4 * lod);
}
// the group drawn blended by LS3D now (m_RenderFGroup's own test: the material's flags, its alpha times the fading)
bool Blended(const void *visual, const void *group) {
    const void *mat = At<const void *>(group, kGroupMaterial);
    if (!mat) return false;
    const uint32_t f = At<uint32_t>(mat, kMaterialFlags);
    if (f & (kMatAdditive | kMatBlend)) return true;
    return At<float>(visual, kVisFade2) * At<float>(visual, kVisFade) * At<float>(mat, kMaterialAlpha) < 1.0f;
}
void WorldOf(const void *visual, float *out) {
    const float *w = &At<float>(visual, game::kFrameWorld);
    for (int i = 0; i < 3; i++) memcpy(out + i * 4, w + i * 4, 12), out[i * 4 + 3] = w[12 + i];
}

void Release(Batch &b) {
    if (b.vb) b.vb->Release(), b.vb = nullptr;
    if (b.ib) b.ib->Release(), b.ib = nullptr;
    b.vbVerts = b.ibCap = 0;
    // its textures: held while it lives (LS3D releases a texture and another takes its address: a batch drawing with
    // the address alone drew the sky on a tower's wall)
    for (IDirect3DBaseTexture9 *&t : b.sig.tex) if (t) t->Release(), t = nullptr;
}
void DropAll() {
    for (auto &kv : g_batches) Release(*kv.second);
    g_batches.clear();
    g_members.clear();
    g_flat.clear();
    g_flatUsed = 0;
    g_refused.clear();
    g_refusedVisuals.clear();
    g_moves.clear();
    g_asked.clear();
    g_cap = Capture{};
}

void Refuse(const Key &k) { g_refused[k]++; g_refusedCount++; }
void RefuseVisual(const void *visual) {
    if (g_refusedVisuals[visual]++) return;
    for (auto &kv : g_members)
        if (kv.first.visual == visual && !kv.second->dead) { kv.second->dead = true; if (kv.second->batch) kv.second->batch->deadIdx += kv.second->nIdx; }
}

// The capture's one draw: the vertices read from the pooled buffer, the indices from the draw's
bool ReadDraw(IDirect3DDevice9 *d, INT base, UINT minIndex, UINT verts, UINT start, UINT prims, const float *world, Member &m,
              std::vector<Vtx> *vout, std::vector<uint32_t> *iout) {
    IDirect3DVertexBuffer9 *vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(d->GetStreamSource(0, &vb, &off, &stride)) || !vb) return false;
    IDirect3DIndexBuffer9 *ib = nullptr;
    d->GetIndices(&ib);
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    bool ok = false;
    if (ib && (fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1) || fvf == kFvf) && stride >= (fvf == kFvf ? 32u : 24u)) {
        D3DINDEXBUFFER_DESC id{};
        ib->GetDesc(&id);
        const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
        void *ip = nullptr, *vp = nullptr;
        if (SUCCEEDED(ib->Lock(start * isz, prims * 3 * isz, &ip, D3DLOCK_READONLY)) && ip) {
            // (NOOVERWRITE: what LS3D wrote is read without waiting for the GPU's draws from the pool: a READONLY lock alone
            // waited some 0.5 ms a group)
            if (SUCCEEDED(vb->Lock(off + (base + minIndex) * stride, verts * stride, &vp, D3DLOCK_READONLY | D3DLOCK_NOOVERWRITE)) && vp) {
                ok = true;
                const auto *src = static_cast<const uint8_t *>(vp);
                const size_t first = vout->size();
                for (UINT v = 0; v < verts; v++) {
                    const auto *p = reinterpret_cast<const float *>(src + size_t(v) * stride);
                    Vtx o;
                    for (int j = 0; j < 3; j++) o.pos[j] = p[0] * world[j] + p[1] * world[4 + j] + p[2] * world[8 + j] + world[3 + 4 * j];
                    o.color = *reinterpret_cast<const DWORD *>(p + 3);
                    memcpy(o.uv0, p + 4, 8);
                    if (fvf == kFvf) memcpy(o.uv1, p + 6, 8); else memcpy(o.uv1, p + 4, 8);
                    vout->push_back(o);
                }
                m.firstIdx = static_cast<uint32_t>(iout->size());
                for (UINT i = 0; i < prims * 3 && ok; i++) {
                    const UINT x = isz == 4 ? static_cast<const uint32_t *>(ip)[i] : static_cast<const uint16_t *>(ip)[i];
                    if (x < minIndex || x >= minIndex + verts) { ok = false; break; }
                    iout->push_back(static_cast<uint32_t>(first + (x - minIndex)));
                }
                m.nIdx = prims * 3;
                if (!ok) { vout->resize(first); iout->resize(m.firstIdx); }
                vb->Unlock();
            }
            ib->Unlock();
        }
    }
    vb->Release();
    if (ib) ib->Release();
    return ok;
}

// A batch's vertices and indices made again of its live groups alone, when the dead ones (moved, relit, another
// frame of a texture: captured again elsewhere) hold more than half of them
void Compact(Batch &b) {
    std::vector<Vtx> verts;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> map(b.verts.size(), UINT32_MAX);
    for (auto &kv : g_members) {
        Member &m = *kv.second;
        if (m.batch != &b || m.dead) continue;
        const uint32_t first = static_cast<uint32_t>(idx.size());
        for (uint32_t i = 0; i < m.nIdx; i++) {
            const uint32_t v = b.idx[m.firstIdx + i];
            if (map[v] == UINT32_MAX) { map[v] = static_cast<uint32_t>(verts.size()); verts.push_back(b.verts[v]); }
            idx.push_back(map[v]);
        }
        m.firstIdx = first;
    }
    for (auto it = g_members.begin(); it != g_members.end();)   // (the dead ones of this batch forgotten)
        it = it->second->batch == &b && it->second->dead ? g_members.erase(it) : std::next(it);
    FlatRebuild();
    b.verts.swap(verts);
    b.idx.swap(idx);
    b.deadIdx = 0;
    b.vbVerts = 0;   // (the vertex buffer made again)
    b.askedLast.clear();
}

// A batch's buffers: the vertices made again when it grew, the indices of this frame's groups when they changed
bool Upload(IDirect3DDevice9 *d, Batch &b) {
    if (b.deadIdx > 4096 && b.deadIdx * 2 > b.idx.size()) Compact(b);
    if (!b.vb || b.vbVerts != b.verts.size()) {
        if (b.vb) b.vb->Release(), b.vb = nullptr;
        const UINT bytes = static_cast<UINT>(b.verts.size() * sizeof(Vtx));
        if (FAILED(d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, &b.vb, nullptr))) return false;
        void *p = nullptr;
        if (FAILED(b.vb->Lock(0, bytes, &p, 0)) || !p) return false;
        memcpy(p, b.verts.data(), bytes);
        b.vb->Unlock();
        b.vbVerts = static_cast<UINT>(b.verts.size());
        b.askedLast.clear();   // (the indices written again)
    }
    if (b.asked != b.askedLast || !b.ib) {
        UINT n = 0;
        for (Member *m : b.asked) n += m->nIdx;
        if (!b.ib || b.ibCap < n) {
            if (b.ib) b.ib->Release(), b.ib = nullptr;
            b.ibCap = std::max<UINT>(n, b.ibCap + b.ibCap / 2);
            if (FAILED(d->CreateIndexBuffer(b.ibCap * 4, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &b.ib, nullptr))) {
                b.ibCap = 0;
                return false;
            }
        }
        void *p = nullptr;
        if (n && (FAILED(b.ib->Lock(0, n * 4, &p, D3DLOCK_DISCARD)) || !p)) return false;
        auto *o = static_cast<uint32_t *>(p);
        for (Member *m : b.asked) { memcpy(o, &b.idx[m->firstIdx], m->nIdx * 4); o += m->nIdx; }
        if (n) b.ib->Unlock();
        b.ibCount = n;
        b.askedLast = b.asked;
    }
    return true;
}

// Every batch asked for, drawn; the states put back as they were
void Flush(IDirect3DDevice9 *d) {
    if (g_asked.empty() || g_drawing) return;
    MirrorGiveBack(d);   // (the fixed pipeline's)
    const D3dMirror *m = MirrorOf(d);
    if (!m) { g_asked.clear(); return; }
    // what LS3D set: put back after (the states a batch sets, the world, clipping)
    const Sig was = SigOf(*m);
    const D3DMATRIX wasWorld = m->world;
    const DWORD wasClip = m->rs[D3DRS_CLIPPING];
    IDirect3DVertexBuffer9 *vb0 = nullptr;
    UINT off0 = 0, stride0 = 0;
    d->GetStreamSource(0, &vb0, &off0, &stride0);
    IDirect3DIndexBuffer9 *ib0 = nullptr;
    d->GetIndices(&ib0);
    DWORD fvf0 = 0;
    d->GetFVF(&fvf0);
    IDirect3DVertexDeclaration9 *decl0 = nullptr;
    if (!fvf0) d->GetVertexDeclaration(&decl0);
    g_drawing = true;
    static const D3DMATRIX kIdentity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    d->SetTransform(D3DTS_WORLD, &kIdentity);
    d->SetFVF(kFvf);
    if (!wasClip) d->SetRenderState(D3DRS_CLIPPING, TRUE);   // (a batch reaches past the frustum)
    const D3dMirror *cur = MirrorOf(d);
    for (Batch *b : g_asked) {
        if (!Upload(d, *b) || !b->ibCount) { b->asked.clear(); continue; }
        // its state, set where it differs from the device's now
        const Sig &s = b->sig;
        for (int t = 0; t < 2; t++) if (cur->tex[t] != s.tex[t]) d->SetTexture(t, s.tex[t]);
        for (int i = 0; i < kNRs; i++) if (cur->rs[kSigRs[i]] != s.rs[i]) d->SetRenderState(kSigRs[i], s.rs[i]);
        for (int t = 0; t < 2; t++) {
            for (int i = 0; i < kNTss; i++) if (cur->tss[t][kSigTss[i]] != s.tss[t][i]) d->SetTextureStageState(t, kSigTss[i], s.tss[t][i]);
            for (int i = 0; i < kNSamp; i++) if (cur->samp[t][kSigSamp[i]] != s.samp[t][i]) d->SetSamplerState(t, kSigSamp[i], s.samp[t][i]);
        }
        d->SetStreamSource(0, b->vb, 0, sizeof(Vtx));
        d->SetIndices(b->ib);
        d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, b->vbVerts, 0, b->ibCount / 3);
        g_drawsNow++;
        b->asked.clear();
        cur = MirrorOf(d);
    }
    // put back: LS3D's cache of the states holds what it set
    cur = MirrorOf(d);
    for (int t = 0; t < 2; t++) if (cur->tex[t] != was.tex[t]) d->SetTexture(t, was.tex[t]);
    for (int i = 0; i < kNRs; i++) if (cur->rs[kSigRs[i]] != was.rs[i]) d->SetRenderState(kSigRs[i], was.rs[i]);
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < kNTss; i++) if (cur->tss[t][kSigTss[i]] != was.tss[t][i]) d->SetTextureStageState(t, kSigTss[i], was.tss[t][i]);
        for (int i = 0; i < kNSamp; i++) if (cur->samp[t][kSigSamp[i]] != was.samp[t][i]) d->SetSamplerState(t, kSigSamp[i], was.samp[t][i]);
    }
    d->SetTransform(D3DTS_WORLD, &wasWorld);
    if (!wasClip) d->SetRenderState(D3DRS_CLIPPING, FALSE);
    if (fvf0) d->SetFVF(fvf0); else d->SetVertexDeclaration(decl0);
    if (decl0) decl0->Release();
    d->SetStreamSource(0, vb0, off0, stride0);
    if (vb0) vb0->Release();
    d->SetIndices(ib0);
    if (ib0) ib0->Release();
    g_drawing = false;
    g_asked.clear();
    g_flushes++;
}

bool SafeCapture(IDirect3DDevice9 *d, const D3dMirror &m, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned prims);

} // namespace

// ---- geom.cpp's hooks

// m_RenderFGroup of an object (kind 1) about to be called: 1 taken (LS3D's call not made), 2 LS3D draws it with the
// capture listening (BatchCaptured after), 0 nothing of ours.
namespace {
int Group(const void *visual, const uint8_t *entry) {
    {
        const void *group = At<const void *>(entry, kEntryGroup);
        if (!group) return 0;
        const Key k{visual, group};
        if (At<uint32_t>(visual, kVisType) != kTypeObject) { g_left[kLeftType]++; return 0; }
        if (g_refusedVisuals.count(visual)) { g_left[kLeftRefused]++; return 0; }
        if (Blended(visual, group)) { g_left[kLeftBlended]++; return 0; }   // (drawn after everything opaque: its order kept)
        const void *tex = At<const void *>(entry, kEntryTexture);
        const void *tex8 = tex ? At<const void *>(tex, 8) : nullptr;
        Member *found = FlatFind(visual, group);
        if (found && !found->dead) {
            Member &m = *found;
            float w[12];
            WorldOf(visual, w);
            const void *rec = LodRecord(visual);
            // the fog Render set for this entry (its sector's) as the batch's: the state's key made again only when the
            // mirror's state changed
            static uint32_t fogSerial, fogNow;
            bool fog = true;
            if (const D3dMirror *mi = MirrorOf(static_cast<IDirect3DDevice9 *>(g_dev))) {
                if (mi->serial != fogSerial || !fogSerial) { fogSerial = mi->serial; fogNow = FogKey(mi->rs, false); }
                fog = fogNow == m.batch->fogKey;
            }
            if (fog && !memcmp(w, m.world, sizeof w) && rec == m.lodRec && rec && At<uint32_t>(rec, 4) == m.stamp[0] &&
                At<uint32_t>(rec, 8) == m.stamp[1] && tex == m.texture && tex8 == m.texture8) {
                Batch &b = *m.batch;
                if (m.askedFrame != g_frame) {
                    m.askedFrame = g_frame;
                    if (b.askedFrame != g_frame) { b.askedFrame = g_frame; b.order = static_cast<unsigned>(g_asked.size()); g_asked.push_back(&b); }
                    b.asked.push_back(&m);
                }
                g_takenNow++;
                g_taken++;
                return 1;
            }
            const bool moved = memcmp(w, m.world, sizeof w) != 0;
            g_died[!fog ? kDiedFog : moved ? kDiedWorld : rec != m.lodRec || !rec ? kDiedLod
                   : At<uint32_t>(rec, 4) != m.stamp[0] ? kDiedStamp1 : At<uint32_t>(rec, 8) != m.stamp[1] ? kDiedStamp2 : kDiedTexture]++;
            m.dead = true;   // (moved, relit, another LOD or frame of its texture: captured again)
            m.batch->deadIdx += m.nIdx;
            g_recaptured++;
            if (moved) {
                unsigned &last = g_moves[visual];
                const bool again = last && g_frame - last < 60;
                last = g_frame ? g_frame : 1;
                if (again) { g_left[kLeftMoved]++; RefuseVisual(visual); return 0; }   // (a moving one: LS3D's)
            }
        }
        if (g_refused.count(k) && g_refused[k] > 3) return 0;
        if (g_captureMs >= kCaptureBudgetMs) { g_left[kLeftBudget]++; return 0; }   // (this frame's time for captures spent)
        g_capStart = NowMs();
        g_cap = Capture{};
        g_cap.on = true;
        g_cap.visual = visual;
        g_cap.group = group;
        g_cap.texture = tex;
        g_cap.texture8 = tex8;
        return 2;
    }
}
int SafeGroup(const void *visual, const uint8_t *entry) {
    __try { return Group(visual, entry); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace
int BatchGroup(const void *visual, const uint8_t *entry, bool inFrame) {
    if (!g_on) return 0;
    if (!inFrame) { g_left[kLeftNotFrame]++; return 0; }
    if (ViewDrawingNow() || ShadowViewNow()) { g_left[kLeftView]++; return 0; }
    if (ShadersWanted() || MotionWanted()) { g_left[kLeftShaders]++; return 0; }
    return SafeGroup(visual, entry);
}
bool BatchDrawing() { return g_drawing; }

// a DrawIndexedPrimitive about to reach the device (geom.cpp's DrawOn): the capture's draw read; the batches drawn
// before the first blended one
void BatchDip(void *dev, int type, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned prims) {
    if (!g_on || g_drawing) return;
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    if (g_dev != d) { DropAll(); g_dev = d; }
    const D3dMirror *m = MirrorOf(d);
    if (!m) return;
    // the batches asked for so far drawn before anything LS3D draws itself: their place in LS3D's order kept (a road's
    // marks, a shadow, a decal lie on what was drawn before them: the ground batched and drawn later covered them)
    if (!g_asked.empty()) Flush(d);
    if (!g_cap.on) return;
    // left to LS3D: more than one draw, not a triangle list, blended, shaded, a third stage, lit by Direct3D, drawn
    // without writing or testing the depth (its place in the order matters: the sky first, decals), and the backdrop
    // (the sky: drawn with the view's rotation alone, no place: not the world's)
    const D3DMATRIX &v = m->view;
    const bool backdrop = v.m[3][0] * v.m[3][0] + v.m[3][1] * v.m[3][1] + v.m[3][2] * v.m[3][2] < 1e-8f;
    if (++g_cap.draws > 1 || type != D3DPT_TRIANGLELIST || m->rs[D3DRS_ALPHABLENDENABLE] || m->vs || m->ps ||
        m->tss[1][D3DTSS_COLOROP] != D3DTOP_DISABLE ||   // (an object's second stage: its environment map, the view's)
        (m->tss[0][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000u) || m->tss[0][D3DTSS_TEXTURETRANSFORMFLAGS] ||
        m->tss[2][D3DTSS_COLOROP] != D3DTOP_DISABLE || m->rs[D3DRS_LIGHTING] || !m->rs[D3DRS_ZWRITEENABLE] ||
        !m->rs[D3DRS_ZENABLE] || backdrop) {
        g_cap.ok = false;
        return;
    }
    if (!SafeCapture(d, *m, base, minIndex, verts, start, prims)) g_cap.ok = false;
}

namespace {
bool Capture1(IDirect3DDevice9 *d, const D3dMirror &mi, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned prims) {
    const D3dMirror *m = &mi;
    {
        auto mem = std::make_unique<Member>();
        mem->visual = g_cap.visual;
        mem->group = g_cap.group;
        mem->texture = g_cap.texture;
        mem->texture8 = g_cap.texture8;
        WorldOf(g_cap.visual, mem->world);
        mem->lodRec = LodRecord(g_cap.visual);
        if (mem->lodRec) { mem->stamp[0] = At<uint32_t>(mem->lodRec, 4); mem->stamp[1] = At<uint32_t>(mem->lodRec, 8); }
        // the vertices into the world by the device's world matrix at the draw (LS3D's for this group): ReadDraw's
        // rows x, y, z, each followed by the place's coordinate
        const D3DMATRIX &dw = m->world;
        float wr[12] = {dw.m[0][0], dw.m[0][1], dw.m[0][2], dw.m[3][0], dw.m[1][0], dw.m[1][1], dw.m[1][2], dw.m[3][1],
                        dw.m[2][0], dw.m[2][1], dw.m[2][2], dw.m[3][2]};
        g_cap.sig = SigOf(*m);
        g_cap.m = std::move(mem);
        std::vector<Vtx> vtx;
        std::vector<uint32_t> idx;
        Member &mm = *g_cap.m;
        if (!ReadDraw(d, base, minIndex, verts, start, prims, wr, mm, &vtx, &idx)) return false;
        // into its batch
        auto &bp = g_batches[g_cap.sig];
        if (!bp) {
            bp = std::make_unique<Batch>();
            bp->sig = g_cap.sig;
            bp->fogKey = FogKey(bp->sig.rs, true);
            for (IDirect3DBaseTexture9 *t : bp->sig.tex) if (t) t->AddRef();   // (Release takes them)
        }
        Batch &b = *bp;
        const uint32_t vbase = static_cast<uint32_t>(b.verts.size());
        b.verts.insert(b.verts.end(), vtx.begin(), vtx.end());
        mm.firstIdx = static_cast<uint32_t>(b.idx.size());
        for (uint32_t x : idx) b.idx.push_back(vbase + x);
        mm.batch = &b;
        g_cap.ok = true;
        return true;
    }
}
bool SafeCapture(IDirect3DDevice9 *d, const D3dMirror &m, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned prims) {
    __try { return Capture1(d, m, base, minIndex, verts, start, prims); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

// LS3D's call for the group under capture returned: kept if its one draw was read
void BatchCaptured() {
    if (!g_cap.on) return;
    g_captureMs += NowMs() - g_capStart;
    const Key k{g_cap.visual, g_cap.group};
    if (g_cap.ok && g_cap.draws == 1 && g_cap.m) {
        g_captured++;
        Member *m = g_cap.m.get();
        g_members[k] = std::move(g_cap.m);
        FlatPut(k.visual, k.group, m);
    } else if (g_cap.draws) {
        if (g_cap.m && g_cap.m->batch) g_cap.m->batch->deadIdx += g_cap.m->nIdx;   // (read, then refused: its indices left)
        Refuse(k);
        RefuseVisual(g_cap.visual);
    }
    g_cap = Capture{};
}

// The frame's world drawn (render.cpp through geom.cpp): what is asked for and not yet drawn, drawn
void BatchFrameEnd(void *dev) {
    if (!g_on) return;
    if (dev && !g_asked.empty()) Flush(static_cast<IDirect3DDevice9 *>(dev));
    g_drawsLast = g_drawsNow, g_drawsNow = 0;
    g_takenLast = g_takenNow, g_takenNow = 0;
    g_captureMs = 0;
    memcpy(g_leftLast, g_left, sizeof g_left);
    memset(g_left, 0, sizeof g_left);
    g_frame++;
}
void BatchMissionClosed() { DropAll(); }

bool BatchSet(bool on, std::string *err) {
    if (on && !g_on) {
        if (!GeomEnsureHooks(err) || !GeomEnsureDip(err)) return false;
        MirrorUse(true);
    }
    if (!on && g_on) {   // (what was captured kept: on again at once)
        MirrorUse(false);
        for (auto &kv : g_batches) kv.second->asked.clear(), kv.second->askedFrame = 0;
        g_asked.clear();
    }
    if (on != g_on) HLOG("batch: the world's static groups %s", on ? "drawn in batches" : "drawn by LS3D again");
    g_on = on;
    return true;
}
// render.batch_probe: the groups captured of the objects within `radius` of `at` (what each was made of)
std::string BatchProbeJson(const float *at, float radius) {
    json out = json::array();
    for (auto &kv : g_members) {
        const Member &m = *kv.second;
        if (!m.batch) continue;
        const Batch &b = *m.batch;
        // its vertices' box (the group's own, in the world): taken when it holds `at` within `radius`
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < m.nIdx; i++) {
            const Vtx &x = b.verts[b.idx[m.firstIdx + i]];
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], x.pos[k]), hi[k] = std::max(hi[k], x.pos[k]);
        }
        bool inside = true;
        for (int k = 0; k < 3; k++) inside = inside && at[k] >= lo[k] - radius && at[k] <= hi[k] + radius;
        if (!inside) continue;
        json v = json::array();
        for (uint32_t i = 0; i < 3 && i < m.nIdx; i++) {
            const Vtx &x = b.verts[b.idx[m.firstIdx + i]];
            v.push_back({x.pos[0], x.pos[1], x.pos[2]});
        }
        char buf[32];
        snprintf(buf, sizeof buf, "%p", m.visual);
        json j = {{"visual", buf}, {"indices", m.nIdx}, {"dead", m.dead}, {"asked_frame", m.askedFrame}, {"frame", g_frame},
                  {"place", {m.world[3], m.world[7], m.world[11]}}, {"first", v}, {"lo", {lo[0], lo[1], lo[2]}},
                  {"hi", {hi[0], hi[1], hi[2]}}};
        snprintf(buf, sizeof buf, "%p", static_cast<void *>(b.sig.tex[0]));
        j["tex0"] = buf;
        j["cull"] = b.sig.rs[3];
        j["alphatest"] = b.sig.rs[0];
        j["zwrite"] = b.sig.rs[5];
        j["colorop0"] = b.sig.tss[0][0];
        out.push_back(j);
        if (out.size() >= 200) break;
    }
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}
std::string BatchStateJson() {
    size_t verts = 0, idx = 0, dead = 0;
    for (auto &kv : g_batches) verts += kv.second->verts.size(), idx += kv.second->idx.size(), dead += kv.second->deadIdx;
    json died = json::object();
    for (int i = 0; i < kDiedCount; i++) if (g_died[i]) died[kDiedNames[i]] = g_died[i];
    json left = json::object();
    for (int i = 0; i < kLeftCount; i++) if (g_leftLast[i]) left[kLeftNames[i]] = g_leftLast[i];
    return json({{"left_last_frame", left}, {"changed", died}, {"on", g_on}, {"batches", g_batches.size()}, {"members", g_members.size()}, {"vertices", verts},
                 {"indices", idx}, {"dead_indices", dead}, {"refused", g_refused.size()}, {"refused_objects", g_refusedVisuals.size()}, {"taken_last_frame", g_takenLast},
                 {"draws_last_frame", g_drawsLast}, {"captured", g_captured}, {"captured_again", g_recaptured},
                 {"flushes", g_flushes}}).dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
