// The meshes the game draws (mml.h scene_focus, scene_pieces, actor_meshes; docs/MODDING.md "The meshes the game
// draws"), read from LS3D's own vertex and index buffers: every visual the game has loaded, its triangles as it holds
// them, without files or names to match.
//
// The scene: its frames walked a little each frame (the city's 27 000 take 5 to 10 ms at once: that made the game
// hitch in mafia-ragdoll), the last complete walk kept; every 0.25 s the visuals of a plain or lightmapped object
// (`OBJ_`, `LMAP` at the visual's +0x1D4) whose bounding sphere (+0x19C, its radius +0x1A8) touches a box a mod or
// the bridge asked for, shown (each frame above it on), not a part of an actor near it, are the pieces. A mesh is
// read once (the meshes are shared by the instances of a model) within a time budget a scan (3 ms; 200 ms at the
// first scan of a mission), the rest at the next scans. Its vertices are in its frame's space as a rule, but some
// (the terrain, large lightmapped pieces) hold the world's or their parent's: the space that puts the mesh where the
// engine's own bounding sphere says the visual is wins (mafia-ragdoll's scenery.cpp, the reference of this file).
//
// LS3D's mesh (both builds): the visual +0x1E0 its mesh; mesh +0x08 the LODs' count, +0x0C LOD 0; LOD +0x08 the
// vertices, +0x0C the vertex buffer (game_build.h vbFvf, vbStride, vbD3d: its FVF, stride, IDirect3DVertexBuffer8),
// +0x14 its first vertex, +0x18 the face groups' count, +0x1C the first face group; a face group +0x04 its material
// (+0x08 the .4ds flags), +0x08 its faces, +0x0C the next, +0x14 IDirect3DIndexBuffer8, +0x18 its first index. The
// buffers are locked read-only through their Direct3D 8 vtables (Lock 11, Unlock 12, GetDesc 13): a wrapper's
// (dxwrapper's) objects answer them as Direct3D 8's.
//
// A box is ready (scene_ready) once a scan that left nothing for later (no mesh deferred by its budget), over a
// complete walk of the scene, had a focus of the mod's holding it, and its focus holds it still (the pieces that
// touch a focus are kept): a body put there finds every visual of it among the pieces (mafia-ragdoll's scenery
// Covers: a body dropped before the street was there fell through it).
//
// Bridge methods: scene.focus, scene.pieces, actor.meshes.
#include "host.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kVisualType = 0x1D4, kVisualMesh = 0x1E0, kBoundCentre = 0x19C, kBoundRadius = 0x1A8;
constexpr uint32_t kTypeObject = 0x5F4A424F /*OBJ_*/, kTypeLightmapped = 0x50414D4C /*LMAP*/;
constexpr int kMeshLods = 0x08, kMeshLod0 = 0x0C;
constexpr int kLodVerts = 0x08, kLodVb = 0x0C, kLodFirstVert = 0x14, kLodGroups = 0x18, kLodFirstGroup = 0x1C;
constexpr int kGroupMat = 0x04, kGroupFaces = 0x08, kGroupNext = 0x0C, kGroupD3dIb = 0x14, kGroupFirstIndex = 0x18;
constexpr int kMaterialFlags = 0x08;
constexpr uint64_t kScanEveryMs = 250;
constexpr unsigned kKeepScans = 120;   // a mesh no piece used for 30 s goes

double NowMs() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// ---- a mesh read from its buffers
struct MeshData {
    std::vector<float> pos, nrm;
    std::vector<uint32_t> idx, mat;
    bool ok = false;
    std::string err;
    uint32_t nv = 0, first = 0;   // what the LOD said when it was read (another mesh at a reused address differs)
    unsigned used = 0;
};
std::unordered_map<const void *, MeshData> g_meshes;

template <class T> T At(const void *p, int off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off); }
void *Vt(void *obj, int slot) { return (*reinterpret_cast<void ***>(obj))[slot]; }

using LockFn = HRESULT(__stdcall *)(void *self, UINT offset, UINT size, BYTE **data, DWORD flags);
using UnlockFn = HRESULT(__stdcall *)(void *self);
using VbDescFn = HRESULT(__stdcall *)(void *self, D3DVERTEXBUFFER_DESC *d);
using IbDescFn = HRESULT(__stdcall *)(void *self, D3DINDEXBUFFER_DESC *d);

bool ReadVb(void *vb, uint32_t offset, uint32_t size, std::vector<uint8_t> *out) {
    D3DVERTEXBUFFER_DESC d{};
    if (FAILED(reinterpret_cast<VbDescFn>(Vt(vb, 13))(vb, &d)) || static_cast<uint64_t>(offset) + size > d.Size) return false;
    BYTE *p = nullptr;
    if (FAILED(reinterpret_cast<LockFn>(Vt(vb, 11))(vb, offset, size, &p, D3DLOCK_READONLY)) || !p) return false;
    out->assign(p, p + size);
    reinterpret_cast<UnlockFn>(Vt(vb, 12))(vb);
    return true;
}
bool ReadIb(void *ib, uint32_t first, uint32_t count, std::vector<uint32_t> *out) {
    D3DINDEXBUFFER_DESC d{};
    if (FAILED(reinterpret_cast<IbDescFn>(Vt(ib, 13))(ib, &d))) return false;
    uint32_t isz = d.Format == D3DFMT_INDEX32 ? 4 : 2;
    if (static_cast<uint64_t>(first + count) * isz > d.Size) return false;
    BYTE *p = nullptr;
    if (FAILED(reinterpret_cast<LockFn>(Vt(ib, 11))(ib, first * isz, count * isz, &p, D3DLOCK_READONLY)) || !p) return false;
    out->resize(count);
    for (uint32_t i = 0; i < count; i++)
        (*out)[i] = isz == 4 ? reinterpret_cast<const uint32_t *>(p)[i] : reinterpret_cast<const uint16_t *>(p)[i];
    reinterpret_cast<UnlockFn>(Vt(ib, 12))(ib);
    return true;
}

bool ReadMesh(const void *mesh, MeshData *out) {
    if (!At<uint32_t>(mesh, kMeshLods)) { out->err = "no LOD"; return false; }
    const void *lod = At<const void *>(mesh, kMeshLod0);
    if (!lod) { out->err = "no LOD 0"; return false; }
    uint32_t nv = At<uint32_t>(lod, kLodVerts), first = At<uint32_t>(lod, kLodFirstVert);
    void *vbw = At<void *>(lod, kLodVb);
    if (!vbw || !nv) { out->err = "no vertices"; return false; }
    const game::Ls3dBuild &l = game::Ls3d();
    uint32_t fvf = At<uint32_t>(vbw, l.vbFvf), stride = At<uint32_t>(vbw, l.vbStride);
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || stride < 12) { out->err = "a vertex format of its own"; return false; }
    bool normals = (fvf & D3DFVF_NORMAL) != 0 && stride >= 24;   // (XYZ then NORMAL: the normal at +12)
    void *vb = At<void *>(vbw, l.vbD3d);
    std::vector<uint8_t> v;
    if (!vb || !ReadVb(vb, first * stride, nv * stride, &v)) { out->err = "its vertex buffer could not be read"; return false; }
    out->pos.resize(nv * 3);
    if (normals) out->nrm.resize(nv * 3);
    for (uint32_t i = 0; i < nv; i++) {
        const float *f = reinterpret_cast<const float *>(&v[static_cast<size_t>(i) * stride]);
        memcpy(&out->pos[i * 3], f, 12);
        if (normals) memcpy(&out->nrm[i * 3], f + 3, 12);
    }
    uint32_t ng = At<uint32_t>(lod, kLodGroups);
    const void *fg = At<const void *>(lod, kLodFirstGroup);
    std::vector<uint32_t> ind;
    for (uint32_t g = 0; g < ng && fg; g++, fg = At<const void *>(fg, kGroupNext)) {
        void *ib = At<void *>(fg, kGroupD3dIb);
        uint32_t faces = At<uint32_t>(fg, kGroupFaces), start = At<uint32_t>(fg, kGroupFirstIndex);
        if (!ib || !faces) continue;
        if (!ReadIb(ib, start, faces * 3, &ind)) { out->err = "an index buffer could not be read"; return false; }
        const void *m = At<const void *>(fg, kGroupMat);
        uint32_t flags = m ? At<uint32_t>(m, kMaterialFlags) : 0;
        for (uint32_t t = 0; t < faces; t++) {
            const uint32_t *x = &ind[t * 3];
            if (x[0] >= nv || x[1] >= nv || x[2] >= nv) { out->err = "an index past its vertices"; return false; }
            out->idx.insert(out->idx.end(), x, x + 3);
            out->mat.push_back(flags);
        }
    }
    if (out->idx.empty()) { out->err = "no faces"; return false; }
    return true;
}

bool SafeReadMesh(const void *mesh, MeshData *out) {
    __try { return ReadMesh(mesh, out); } __except (EXCEPTION_EXECUTE_HANDLER) { out->err = "reading it faulted"; return false; }
}

unsigned g_scans;

const MeshData *MeshFor(const void *mesh) {
    const void *lod = At<uint32_t>(mesh, kMeshLods) ? At<const void *>(mesh, kMeshLod0) : nullptr;
    uint32_t nv = lod ? At<uint32_t>(lod, kLodVerts) : 0, first = lod ? At<uint32_t>(lod, kLodFirstVert) : 0;
    auto it = g_meshes.find(mesh);
    if (it != g_meshes.end() && it->second.nv == nv && it->second.first == first) { it->second.used = g_scans; return &it->second; }
    MeshData &d = g_meshes[mesh];
    d = MeshData{};
    d.nv = nv;
    d.first = first;
    d.used = g_scans;
    d.ok = SafeReadMesh(mesh, &d);
    return &d;
}

// ---- where a mesh's vertices are: its frame's space (0), the world's (1), its parent's (2)
void Identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
bool SpaceOf(void *visual, const MeshData &md, int *space, float out[16]) {
    const float *bc = reinterpret_cast<const float *>(static_cast<const uint8_t *>(visual) + kBoundCentre);
    float br = At<float>(visual, kBoundRadius);
    if (!(br >= 0)) return false;
    float best = 1e30f;
    *space = -1;
    for (int cand = 0; cand < 3; cand++) {
        float m[16];
        if (cand == 0) memcpy(m, game::WorldMatrix(visual), 64);
        else if (cand == 1) Identity(m);
        else {
            void *parent = At<void *>(visual, game::kFrameParent);
            if (!parent) break;
            memcpy(m, game::WorldMatrix(parent), 64);
        }
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i + 2 < md.pos.size(); i += 3) {
            const float *p = &md.pos[i];
            for (int k = 0; k < 3; k++) {
                float x = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
                lo[k] = std::min(lo[k], x);
                hi[k] = std::max(hi[k], x);
            }
        }
        float cc[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
        float off = std::sqrt((cc[0] - bc[0]) * (cc[0] - bc[0]) + (cc[1] - bc[1]) * (cc[1] - bc[1]) + (cc[2] - bc[2]) * (cc[2] - bc[2]));
        float half = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) / 2;
        if (off <= br + 0.25f && half <= br * 1.05f + 0.25f && off < best - 0.01f) {
            best = off;
            *space = cand;
            memcpy(out, m, 64);
        }
    }
    return *space >= 0;
}
void MatrixOf(void *visual, int space, float out[16]) {
    if (space == 1) { Identity(out); return; }
    void *f = space == 2 ? At<void *>(visual, game::kFrameParent) : visual;
    memcpy(out, game::WorldMatrix(f), 64);
}

// ---- the scene's frames, walked a little each frame
std::vector<void *> g_walked, g_next, g_stack;
bool g_walking;
unsigned g_walks;
double g_walkMs, g_lastWalkMs;

void *SceneRoot() {
    void *pl = game::Player();
    void *top = pl ? At<void *>(pl, game::kActorFrame) : nullptr;
    while (top && At<void *>(top, game::kFrameParent)) top = At<void *>(top, game::kFrameParent);
    return top;
}
std::vector<void *> g_kids;
void WalkSteps(double until, bool *finished) {
    while (!g_stack.empty()) {
        if (NowMs() > until) { *finished = false; return; }
        for (int i = 0; i < 64 && !g_stack.empty(); i++) {
            void *f = g_stack.back();
            g_stack.pop_back();
            g_next.push_back(f);
            g_kids.clear();
            game::EnumFrames(f, &g_kids);
            for (size_t k = g_kids.size(); k-- > 0;) g_stack.push_back(g_kids[k]);
        }
    }
    *finished = true;
}
bool WalkStep(double until, bool *finished) {
    __try { WalkSteps(until, finished); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Walk(double budgetMs) {
    double t0 = NowMs();
    if (!g_walking) {
        void *root = SceneRoot();
        if (!root) return;
        g_next.clear();
        g_stack.clear();
        std::vector<void *> kids;
        game::EnumFrames(root, &kids);
        for (size_t k = kids.size(); k-- > 0;) g_stack.push_back(kids[k]);
        g_walking = true;
        g_walkMs = 0;
    }
    bool finished = false;
    if (!WalkStep(t0 + budgetMs, &finished)) { g_walking = false; return; }   // (a frame went under the walk)
    g_walkMs += NowMs() - t0;
    if (!finished) return;
    g_walked.swap(g_next);
    g_walking = false;
    g_walks++;
    g_lastWalkMs = g_walkMs;
}

// ---- the pieces
struct Focus { V3 lo, hi; };
std::map<MmlMod *, Focus> g_focus;   // (null: the bridge's)
std::map<MmlMod *, Focus> g_ready;   // each mod's focus at the last scan that left nothing for later
struct Piece {
    const void *mesh = nullptr;
    int space = 0;
    float world[16];
    uint32_t version = 1;
    double movedAt = -1e9;
    unsigned seen = 0;
    uintptr_t vtable = 0;
};
std::unordered_map<void *, Piece> g_pieces;
uint64_t g_nextScan;
bool g_first = true;
unsigned g_deferred, g_unreadable, g_misplaced, g_hidden, g_ofActors;
std::string g_lastErr;
double g_scanMs;
std::map<uint32_t, unsigned> g_types;   // the visuals in the boxes by their kind (the scan's diagnostics)

bool Touches(const float *c, float r, const Focus &f) {
    for (int k = 0; k < 3; k++) {
        float lo = (&f.lo.x)[k] - 2, hi = (&f.hi.x)[k] + 2;
        if (c[k] + r < lo || c[k] - r > hi) return false;
    }
    return true;
}

void Scan() {
    double t0 = NowMs(), budget = g_first ? 200.0 : 3.0;
    g_scans++;
    g_deferred = g_unreadable = g_misplaced = g_hidden = g_ofActors = 0;
    // the actors near the boxes: their models' frames are not the scene's
    std::unordered_set<void *> roots;
    for (auto &kv : g_focus) {
        V3 mid = (kv.second.lo + kv.second.hi) * 0.5f;
        float at[3] = {mid.x, mid.y, mid.z};
        void *nearby[256];
        int n = WorldNear(at, Len(kv.second.hi - mid) + 10.0f, 0, nearby, 256);
        for (int i = 0; i < n && i < 256; i++) if (void *m = ActorFrame(nearby[i])) roots.insert(m);
    }
    g_types.clear();
    for (void *f : g_walked) {
        if (At<uint32_t>(f, game::kFrameType) != 1) continue;
        uint32_t type = At<uint32_t>(f, kVisualType);
        const float *bc = reinterpret_cast<const float *>(static_cast<const uint8_t *>(f) + kBoundCentre);
        float br = At<float>(f, kBoundRadius);
        bool in = false;
        for (auto &kv : g_focus) in = in || Touches(bc, br, kv.second);
        if (!in || !(br >= 0)) continue;
        g_types[type]++;
        if (type != kTypeObject && type != kTypeLightmapped) continue;
        bool hidden = false, actor = false;
        for (void *a = f; a; a = At<void *>(a, game::kFrameParent)) {
            if (!(At<uint32_t>(a, game::kFrameFlags) & 1u)) hidden = true;
            if (roots.count(a)) actor = true;
        }
        if (hidden) { g_hidden++; continue; }
        if (actor) { g_ofActors++; continue; }
        const void *mesh = At<const void *>(f, kVisualMesh);
        if (!mesh) continue;
        auto known = g_pieces.find(f);
        bool fresh = known == g_pieces.end() || known->second.mesh != mesh;
        if (fresh && NowMs() - t0 > budget) { g_deferred++; continue; }
        const MeshData *md = MeshFor(mesh);
        if (!md->ok) { g_unreadable++; g_lastErr = md->err; continue; }
        Piece &p = g_pieces[f];
        if (fresh) {
            float m[16];
            int space;
            if (!SpaceOf(f, *md, &space, m)) { g_misplaced++; g_pieces.erase(f); continue; }
            p.mesh = mesh;
            p.space = space;
            memcpy(p.world, m, 64);
            p.version++;
            p.vtable = At<uintptr_t>(f, 0);
        } else {
            float m[16];
            MatrixOf(f, p.space, m);
            bool moved = false;
            for (int i = 0; i < 16 && !moved; i++) moved = std::fabs(m[i] - p.world[i]) > (i >= 12 ? 1e-3f : 1e-4f);
            if (moved) { memcpy(p.world, m, 64); p.version++; p.movedAt = NowMs(); }
        }
        p.seen = g_scans;
    }
    for (auto it = g_pieces.begin(); it != g_pieces.end();) it = it->second.seen == g_scans ? std::next(it) : g_pieces.erase(it);
    if (g_scans % 40 == 0)
        for (auto it = g_meshes.begin(); it != g_meshes.end();) it = it->second.used + kKeepScans >= g_scans ? std::next(it) : g_meshes.erase(it);
    if (!g_deferred && g_walks) g_ready = g_focus;   // (everything of the walked scene in the boxes is a piece)
    if (g_first) HLOG("scene meshes: %zu pieces at the first scan (%u hidden, %u of actors, %u unreadable, %u out of place)", g_pieces.size(),
                      g_hidden, g_ofActors, g_unreadable, g_misplaced);
    g_first = false;
    g_scanMs = NowMs() - t0;
}
bool SafeScan() {
    __try { Scan(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Fill(void *visual, const Piece &p, const MeshData &md, MmlMeshPiece *o) {
    o->visual = visual;
    memcpy(o->world, p.world, 64);
    o->pos = md.pos.data();
    o->normal = md.nrm.empty() ? nullptr : md.nrm.data();
    o->vertices = static_cast<int>(md.pos.size() / 3);
    o->index = md.idx.data();
    o->material = md.mat.data();
    o->triangles = static_cast<int>(md.idx.size() / 3);
    o->version = p.version;
    o->moving = NowMs() - p.movedAt < 1000.0 ? 1 : 0;
}

template <class F> bool Guarded(F &f) {
    __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string NameOf(void *frame) {
    std::string name;
    auto g = [&] { const char *s = At<const char *>(frame, game::kFrameName); if (s) name.assign(s, strnlen(s, 64)); };
    Guarded(g);
    return name;
}

} // namespace

bool SceneFocus(MmlMod *mod, const float *lo, const float *hi) {
    if (!lo || !hi) { g_focus.erase(mod); return true; }
    Focus f{{std::min(lo[0], hi[0]), std::min(lo[1], hi[1]), std::min(lo[2], hi[2])}, {std::max(lo[0], hi[0]), std::max(lo[1], hi[1]), std::max(lo[2], hi[2])}};
    // a scan at once only for a box that reaches somewhere the last one did not (by 5 m: a new place, not a box moving
    // with the player, which the scans every 0.25 s follow): a mod giving its box
    // every tick (a box round a moving player) kept the scans from waiting their 0.25 s
    auto old = g_focus.find(mod);
    bool grew = old == g_focus.end();
    for (int k = 0; k < 3 && !grew; k++)
        grew = (&f.lo.x)[k] < (&old->second.lo.x)[k] - 5.0f || (&f.hi.x)[k] > (&old->second.hi.x)[k] + 5.0f;
    g_focus[mod] = f;
    if (grew) g_nextScan = 0;
    return true;
}

int ScenePieces(MmlMod *mod, MmlMeshPiece *out, int max) {
    auto fo = g_focus.find(mod);
    if (fo == g_focus.end()) return 0;
    int n = 0;
    for (auto &kv : g_pieces) {
        const float *bc = nullptr;
        float br = 0;
        auto g = [&] { bc = reinterpret_cast<const float *>(static_cast<const uint8_t *>(kv.first) + kBoundCentre); br = At<float>(kv.first, kBoundRadius); };
        if (!Guarded(g)) continue;
        if (!Touches(bc, br, fo->second)) continue;
        auto md = g_meshes.find(kv.second.mesh);
        if (md == g_meshes.end() || !md->second.ok) continue;
        if (out && n < max) Fill(kv.first, kv.second, md->second, &out[n]);
        n++;
    }
    return n;
}

int ActorMeshes(void *actor, MmlMeshPiece *out, int max) {
    void *model = ActorFrame(actor);
    if (!model) return -1;
    std::vector<void *> frames;
    // (its own frames that are shown: not into another model, a person sitting in a car, nor below a hidden frame)
    auto w = [&] {
        std::vector<void *> stack{model}, kids;
        while (!stack.empty() && frames.size() < 4096) {
            void *f = stack.back();
            stack.pop_back();
            if (!(At<uint32_t>(f, game::kFrameFlags) & 1u)) continue;
            if (f != model && At<uint32_t>(f, game::kFrameType) == 9) continue;
            frames.push_back(f);
            kids.clear();
            game::EnumFrames(f, &kids);
            stack.insert(stack.end(), kids.begin(), kids.end());
        }
    };
    if (!Guarded(w)) return -1;
    static std::unordered_map<void *, Piece> s_actorPieces;   // (their spaces, kept)
    int n = 0;
    for (void *f : frames) {
        bool got = false;
        auto g = [&] {
            if (At<uint32_t>(f, game::kFrameType) != 1) return;
            uint32_t type = At<uint32_t>(f, kVisualType);
            if (type != kTypeObject && type != kTypeLightmapped) return;
            const void *mesh = At<const void *>(f, kVisualMesh);
            if (!mesh) return;
            const MeshData *md = MeshFor(mesh);
            if (!md->ok) return;
            Piece &p = s_actorPieces[f];
            if (p.mesh != mesh || p.vtable != At<uintptr_t>(f, 0)) {
                float m[16];
                int space;
                if (!SpaceOf(f, *md, &space, m)) space = 0;
                p.mesh = mesh;
                p.space = space;
                p.vtable = At<uintptr_t>(f, 0);
            }
            MatrixOf(f, p.space, p.world);
            if (out && n < max) Fill(f, p, *md, &out[n]);
            got = true;
        };
        if (Guarded(g) && got) n++;
    }
    return n;
}

void MeshesFrame() {
    if (g_focus.empty()) return;
    // Only the scans read a walk: the next walk starts when a scan is due, and that scan waits for it (a city's
    // 25 000 frames are 2 ms of walking over a few frames, once a scan instead of without pause: 0.6 ms a frame).
    // A focus that grew is scanned at once, from the walk before.
    uint64_t now = GetTickCount64();
    bool due = now >= g_nextScan, urgent = g_nextScan == 0;
    if (g_walking || g_walked.empty() || due) Walk(0.75);
    if (g_walked.empty() || !due || (g_walking && !urgent)) return;
    g_nextScan = now + kScanEveryMs;
    if (!SafeScan()) g_pieces.clear();   // (a frame went: the next scan starts over)
}

void MeshesMissionClose() {
    g_walked.clear();
    g_next.clear();
    g_stack.clear();
    g_walking = false;
    g_pieces.clear();
    g_meshes.clear();
    g_ready.clear();
    g_first = true;
}

bool SceneReady(MmlMod *mod, const float *lo, const float *hi) {
    auto in = [&](const std::map<MmlMod *, Focus> &m) {
        auto it = m.find(mod);
        if (it == m.end()) return false;
        for (int k = 0; k < 3; k++)
            if (std::min(lo[k], hi[k]) < (&it->second.lo.x)[k] || std::max(lo[k], hi[k]) > (&it->second.hi.x)[k]) return false;
        return true;
    };
    return lo && hi && in(g_ready) && in(g_focus);
}

void MeshesModGone(MmlMod *mod) { g_focus.erase(mod); g_ready.erase(mod); }

bool MeshesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "scene.focus" && method != "scene.pieces" && method != "actor.meshes") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto r3 = [](const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); };
    std::string err;
    try {
        if (method == "scene.focus") {
            if (p.value("off", false)) { SceneFocus(nullptr, nullptr, nullptr); *reply = dump({{"focus", nullptr}}); return *ok = true; }
            float lo[3], hi[3];
            if (p.contains("lo") && p.contains("hi")) {
                for (int i = 0; i < 3; i++) { lo[i] = p["lo"][i].get<float>(); hi[i] = p["hi"][i].get<float>(); }
            } else {
                float at[3];
                if (p.contains("at") && p["at"].is_array()) for (int i = 0; i < 3; i++) at[i] = p["at"][i].get<float>();
                else {
                    void *a = nullptr;
                    float w[16];
                    if (!ParseActor(p.value("at", "player"), &a, &err) || !FrameWorld(ActorFrame(a), w)) { *reply = "scene.focus: at: " + (err.empty() ? std::string("no place") : err); return true; }
                    for (int i = 0; i < 3; i++) at[i] = w[12 + i];
                }
                float r = p.value("radius", 20.0f);
                for (int i = 0; i < 3; i++) { lo[i] = at[i] - r; hi[i] = at[i] + r; }
            }
            SceneFocus(nullptr, lo, hi);
            *reply = dump({{"focus", {{"lo", r3(lo)}, {"hi", r3(hi)}}}, {"note", "scanned within a second; scene.pieces"}});
        } else if (method == "scene.pieces") {
            int n = ScenePieces(nullptr, nullptr, 0);
            std::vector<MmlMeshPiece> pieces(static_cast<size_t>(n));
            if (n) ScenePieces(nullptr, pieces.data(), n);
            size_t tris = 0;
            json list = json::array();
            int limit = p.value("limit", 20);
            for (const MmlMeshPiece &m : pieces) {
                tris += static_cast<size_t>(m.triangles);
                if (static_cast<int>(list.size()) >= limit) continue;
                std::string name = NameOf(m.visual);
                list.push_back({{"visual", name}, {"triangles", m.triangles}, {"vertices", m.vertices}, {"at", r3(m.world + 12)},
                                {"moving", m.moving != 0}, {"version", m.version}});
            }
            json out = {{"pieces", n}, {"triangles", tris}, {"list", list}, {"walks", g_walks}, {"walk_ms", std::round(g_lastWalkMs * 100) / 100},
                        {"walked_frames", g_walked.size()}, {"scans", g_scans}, {"scan_ms", std::round(g_scanMs * 100) / 100},
                        {"left_for_later", g_deferred}, {"hidden", g_hidden}, {"of_actors", g_ofActors}, {"unreadable", g_unreadable},
                        {"out_of_place", g_misplaced}, {"meshes_cached", g_meshes.size()}};
            if (!g_lastErr.empty()) out["last_error"] = g_lastErr;
            json types = json::object();
            for (auto &kv : g_types) {
                char t[5] = {static_cast<char>(kv.first & 0xFF), static_cast<char>(kv.first >> 8 & 0xFF), static_cast<char>(kv.first >> 16 & 0xFF),
                             static_cast<char>(kv.first >> 24 & 0xFF), 0};
                for (char &c : t) if (c && (c < 32 || c > 126)) c = '?';
                types[t] = kv.second;
            }
            out["visuals_in_box"] = types;
            if (!g_focus.count(nullptr)) out["note"] = "no focus of the bridge's: scene.focus first";
            *reply = dump(out);
        } else {
            void *actor = nullptr;
            if (!ParseActor(p.value("actor", "car"), &actor, &err)) { *reply = "actor.meshes: actor: " + err; return true; }
            int n = ActorMeshes(actor, nullptr, 0);
            if (n < 0) { *reply = "actor.meshes: the actor has no model"; return true; }
            std::vector<MmlMeshPiece> pieces(static_cast<size_t>(n));
            if (n) ActorMeshes(actor, pieces.data(), n);
            json list = json::array();
            size_t tris = 0;
            for (const MmlMeshPiece &m : pieces) {
                tris += static_cast<size_t>(m.triangles);
                std::string name = NameOf(m.visual);
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (int i = 0; i < m.vertices; i++) {
                    const float *v = m.pos + i * 3;
                    for (int k = 0; k < 3; k++) {
                        float x = v[0] * m.world[k] + v[1] * m.world[4 + k] + v[2] * m.world[8 + k] + m.world[12 + k];
                        lo[k] = std::min(lo[k], x);
                        hi[k] = std::max(hi[k], x);
                    }
                }
                list.push_back({{"visual", name}, {"triangles", m.triangles}, {"lo", r3(lo)}, {"hi", r3(hi)}});
            }
            *reply = dump({{"pieces", n}, {"triangles", tris}, {"list", list}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
