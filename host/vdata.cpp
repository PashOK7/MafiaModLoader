// A rule's vertex data (mml.h MML_SHADE_VERTEX_DATA, shader_vertex_data; docs/MODDING.md "Shaders on the game's own
// drawing"; game.hlsli MmlVertexData): what the game's draws do not carry, given to a rule's vertex shader in a second
// stream: each vertex's normal (the mesh's own, at rest), its MikkTSpace tangent and a float4 of the mod's.
//
// LS3D lights its objects on the CPU and copies their vertices into pooled, dynamic vertex buffers (a LOD's record:
// +0x10 the pool, +0x18 its first vertex there, the draw's BaseVertexIndex); a LOD keeps its place in the pool while
// it lives. Direct3D 9 adds BaseVertexIndex to every stream's index, so the second stream is a mirror of the pool: a
// buffer of the pool's count of vertices, 48 bytes each, each LOD's vertices at the same place as in the pool, written
// when the place's owner changes (a LOD drawn there for the first time). The vertex layout is the draw's FVF with the
// second stream's three elements after it (TEXCOORD5..7).
//
// The normals and the uvs are the LOD's own vertex buffer's (MeshLevel +0x0C), the triangles its face groups'
// index buffers (as meshes.cpp reads them); the tangents MikkTSpace's (the bakers' convention).
#include "host.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mikktspace.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace host {

// ---- MikkTSpace (skin.cpp's people and this file's objects): a tangent a vertex, xyz and the bitangent's sign; the
// face corners of one vertex averaged (LS3D's vertices are already split where the uv map is)
namespace {
struct Mikk {
    const float *pos, *nrm, *uv;
    int ps, ns, us;
    const uint32_t *idx;
    int faces;
    float *tan;
    int *count;
};
int MkFaces(const SMikkTSpaceContext *c) { return static_cast<Mikk *>(c->m_pUserData)->faces; }
int MkVerts(const SMikkTSpaceContext *, const int) { return 3; }
void MkPos(const SMikkTSpaceContext *c, float o[], const int f, const int v) {
    auto *m = static_cast<Mikk *>(c->m_pUserData);
    memcpy(o, m->pos + size_t(m->idx[f * 3 + v]) * m->ps, 12);
}
void MkNrm(const SMikkTSpaceContext *c, float o[], const int f, const int v) {
    auto *m = static_cast<Mikk *>(c->m_pUserData);
    memcpy(o, m->nrm + size_t(m->idx[f * 3 + v]) * m->ns, 12);
}
void MkUv(const SMikkTSpaceContext *c, float o[], const int f, const int v) {
    auto *m = static_cast<Mikk *>(c->m_pUserData);
    memcpy(o, m->uv + size_t(m->idx[f * 3 + v]) * m->us, 8);
}
void MkSet(const SMikkTSpaceContext *c, const float t[], const float sign, const int f, const int v) {
    auto *m = static_cast<Mikk *>(c->m_pUserData);
    uint32_t i = m->idx[f * 3 + v];
    float *o = m->tan + size_t(i) * 4;
    o[0] += t[0], o[1] += t[1], o[2] += t[2], o[3] += sign;
    m->count[i]++;
}
} // namespace

void MikkTangents(uint32_t verts, const float *pos, int posStride, const float *nrm, int nrmStride, const float *uv, int uvStride,
                  const uint32_t *idx, uint32_t faces, float *tan) {
    std::vector<float> acc(size_t(verts) * 4, 0.0f);
    std::vector<int> count(verts, 0);
    if (faces && idx) {
        Mikk m{pos, nrm, uv, posStride, nrmStride, uvStride, idx, static_cast<int>(faces), acc.data(), count.data()};
        SMikkTSpaceInterface mi{};
        mi.m_getNumFaces = MkFaces;
        mi.m_getNumVerticesOfFace = MkVerts;
        mi.m_getPosition = MkPos;
        mi.m_getNormal = MkNrm;
        mi.m_getTexCoord = MkUv;
        mi.m_setTSpaceBasic = MkSet;
        SMikkTSpaceContext mc{&mi, &m};
        genTangSpaceDefault(&mc);
    }
    for (uint32_t i = 0; i < verts; i++) {
        float *o = tan + size_t(i) * 4, *s = &acc[size_t(i) * 4];
        o[0] = 1, o[1] = 0, o[2] = 0, o[3] = 1;
        if (!count[i]) continue;
        float l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (l > 1e-12f) o[0] = s[0] / l, o[1] = s[1] / l, o[2] = s[2] / l;
        o[3] = s[3] < 0 ? -1.0f : 1.0f;
    }
}

namespace {

template <class T> T At(const void *p, int off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off); }
void *Vt(void *obj, int slot) { return (*reinterpret_cast<void ***>(obj))[slot]; }
template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

constexpr int kVisMesh = 0x1E0, kVisLods = 0x1E4, kMeshLevels = 0x08, kMeshLevel0 = 0x0C;
constexpr int kLevelVerts = 0x08, kLevelVb = 0x0C, kLevelFirst = 0x14, kLevelGroups = 0x18, kLevelFirstGroup = 0x1C;
constexpr int kGroupFaces = 0x08, kGroupNext = 0x0C, kGroupD3dIb = 0x14, kGroupFirstIndex = 0x18;
constexpr int kLodDrawFirst = 0x18;
constexpr int kMaxLods = 8;
constexpr UINT kStride = 48;   // normal (12), tangent (16), the mod's (16), 4 left
constexpr uint32_t kSingleMesh = 0x4D474E53, kSingleMorph = 0x524D4D53;   // 'SNGM', 'SMMR' (+0x1D4): people

// ---- the mods' own attributes: a float4 a vertex of a visual's LOD, by the visual's frame name
struct Attr { MmlMod *mod; std::string name; int lod; std::vector<float> v; unsigned gen; };
std::vector<Attr> g_attrs;
unsigned g_attrGen = 1;

std::string Lower(const char *s) {
    std::string o;
    for (; s && *s; s++) o += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return o;
}
const Attr *AttrOf(const void *visual, int lod) {
    if (g_attrs.empty()) return nullptr;
    const char *n = At<const char *>(visual, game::kFrameName);
    if (!n) return nullptr;
    std::string full = Lower(n), node = full.substr(full.find('.') == std::string::npos ? 0 : full.find('.') + 1);
    for (auto it = g_attrs.rbegin(); it != g_attrs.rend(); ++it)   // (the last given wins)
        if (it->lod == lod && (it->name == full || it->name == node)) return &*it;
    return nullptr;
}

// ---- a LOD's data (normals, tangents) read once, by its level
struct Level { uint32_t verts = 0, first = 0; std::vector<float> nrm, tan; bool ok = false; std::string why; };
std::unordered_map<const void *, Level> g_levels;

bool ReadVb8(void *vb, uint32_t offset, uint32_t size, std::vector<uint8_t> *out) {
    using LockFn = HRESULT(__stdcall *)(void *, UINT, UINT, BYTE **, DWORD);
    using UnlockFn = HRESULT(__stdcall *)(void *);
    using DescFn = HRESULT(__stdcall *)(void *, D3DVERTEXBUFFER_DESC *);
    D3DVERTEXBUFFER_DESC d{};
    if (FAILED(reinterpret_cast<DescFn>(Vt(vb, 13))(vb, &d)) || static_cast<uint64_t>(offset) + size > d.Size) return false;
    BYTE *p = nullptr;
    if (FAILED(reinterpret_cast<LockFn>(Vt(vb, 11))(vb, offset, size, &p, D3DLOCK_READONLY)) || !p) return false;
    out->assign(p, p + size);
    reinterpret_cast<UnlockFn>(Vt(vb, 12))(vb);
    return true;
}
bool ReadIb8(void *ib, uint32_t first, uint32_t count, std::vector<uint32_t> *out) {
    using LockFn = HRESULT(__stdcall *)(void *, UINT, UINT, BYTE **, DWORD);
    using UnlockFn = HRESULT(__stdcall *)(void *);
    using DescFn = HRESULT(__stdcall *)(void *, D3DINDEXBUFFER_DESC *);
    D3DINDEXBUFFER_DESC d{};
    if (FAILED(reinterpret_cast<DescFn>(Vt(ib, 13))(ib, &d))) return false;
    uint32_t isz = d.Format == D3DFMT_INDEX32 ? 4 : 2;
    if (static_cast<uint64_t>(first + count) * isz > d.Size) return false;
    BYTE *p = nullptr;
    if (FAILED(reinterpret_cast<LockFn>(Vt(ib, 11))(ib, first * isz, count * isz, &p, D3DLOCK_READONLY)) || !p) return false;
    size_t at = out->size();
    out->resize(at + count);
    for (uint32_t i = 0; i < count; i++)
        (*out)[at + i] = isz == 4 ? reinterpret_cast<const uint32_t *>(p)[i] : reinterpret_cast<const uint16_t *>(p)[i];
    reinterpret_cast<UnlockFn>(Vt(ib, 12))(ib);
    return true;
}

bool ReadLevel(const void *level, Level *o) {
    const game::Ls3dBuild &l = game::Ls3d();
    void *w = At<void *>(level, kLevelVb);
    o->verts = At<uint32_t>(level, kLevelVerts);
    o->first = At<uint32_t>(level, kLevelFirst);
    if (!w || !o->verts) { o->why = "no vertices"; return false; }
    uint32_t fvf = At<uint32_t>(w, l.vbFvf), stride = At<uint32_t>(w, l.vbStride);
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !(fvf & D3DFVF_NORMAL) || stride < 32) { o->why = "no normals in its vertices"; return false; }
    std::vector<uint8_t> v;
    const uint8_t *src = At<const uint8_t *>(w, 0);   // (LS3D keeps some buffers locked: read there)
    if (src) src += size_t(o->first) * stride;
    else if (ReadVb8(At<void *>(w, l.vbD3d), o->first * stride, o->verts * stride, &v)) src = v.data();
    if (!src) { o->why = "its vertex buffer could not be read"; return false; }
    std::vector<float> pos(size_t(o->verts) * 3), uv(size_t(o->verts) * 2);
    o->nrm.resize(size_t(o->verts) * 3);
    for (uint32_t i = 0; i < o->verts; i++) {
        const float *f = reinterpret_cast<const float *>(src + size_t(i) * stride);
        memcpy(&pos[i * 3], f, 12);
        memcpy(&o->nrm[i * 3], f + 3, 12);
        memcpy(&uv[i * 2], f + 6, 8);
    }
    std::vector<uint32_t> idx;
    uint32_t ng = At<uint32_t>(level, kLevelGroups);
    const void *fg = At<const void *>(level, kLevelFirstGroup);
    for (uint32_t g = 0; g < ng && fg; g++, fg = At<const void *>(fg, kGroupNext)) {
        void *ib = At<void *>(fg, kGroupD3dIb);
        uint32_t faces = At<uint32_t>(fg, kGroupFaces), start = At<uint32_t>(fg, kGroupFirstIndex);
        if (ib && faces && !ReadIb8(ib, start, faces * 3, &idx)) { o->why = "an index buffer could not be read"; return false; }
    }
    idx.erase(std::remove_if(idx.begin(), idx.end(), [&](uint32_t i) { return i >= o->verts; }), idx.end());
    idx.resize(idx.size() / 3 * 3);
    o->tan.resize(size_t(o->verts) * 4);
    MikkTangents(o->verts, pos.data(), 3, o->nrm.data(), 3, uv.data(), 2, idx.data(), static_cast<uint32_t>(idx.size() / 3), o->tan.data());
    return true;
}
const Level *LevelOf(const void *level) {
    auto it = g_levels.find(level);
    uint32_t verts = At<uint32_t>(level, kLevelVerts), first = At<uint32_t>(level, kLevelFirst);
    if (it != g_levels.end() && it->second.verts == verts && it->second.first == first) return it->second.ok ? &it->second : nullptr;
    Level &lv = g_levels[level];
    lv = Level{};
    lv.ok = ReadLevel(level, &lv);
    return lv.ok ? &lv : nullptr;
}

// ---- the mirrors: one a pool (the draw's stream 0), its places' owners
struct Owner { const void *level = nullptr; uint32_t verts = 0; unsigned attrGen = 0; const void *attr = nullptr; };
struct Mirror {
    IDirect3DDevice9 *dev = nullptr;
    IDirect3DVertexBuffer9 *game = nullptr, *vb = nullptr;
    UINT verts = 0;
    std::map<UINT, Owner> owners;   // by the place's first vertex
};
std::unordered_map<IDirect3DVertexBuffer9 *, Mirror> g_mirrors;
std::map<DWORD, IDirect3DVertexDeclaration9 *> g_decls;   // the FVF's elements and the second stream's
IDirect3DDevice9 *g_dev;
unsigned g_binds, g_bindsLast, g_writes, g_writesLast, g_misses, g_missesLast;
std::map<std::string, unsigned> g_whyNow, g_whyLast;   // (the draws left without data: why)
std::string g_why;

void ReleaseAll() {
    for (auto &kv : g_mirrors) SafeRelease(kv.second.vb);
    g_mirrors.clear();
    for (auto &kv : g_decls) SafeRelease(kv.second);
    g_decls.clear();
    g_dev = nullptr;
}

// The draw's FVF as declaration elements (stream 0), then the second stream's: TEXCOORD5 the normal, TEXCOORD6 the
// tangent, TEXCOORD7 the mod's
IDirect3DVertexDeclaration9 *DeclOf(IDirect3DDevice9 *d, DWORD fvf) {
    if (g_dev != d) ReleaseAll(), g_dev = d;
    auto it = g_decls.find(fvf);
    if (it != g_decls.end()) return it->second;
    std::vector<D3DVERTEXELEMENT9> el;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) { el.push_back({0, off, type, D3DDECLMETHOD_DEFAULT, usage, index}); off += size; };
    DWORD p = fvf & D3DFVF_POSITION_MASK;
    if (p == D3DFVF_XYZRHW) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
    else if (p >= D3DFVF_XYZ) {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        int blends = p == D3DFVF_XYZ ? 0 : static_cast<int>((p - D3DFVF_XYZB1) / 2 + 1);
        if (blends) add(static_cast<BYTE>(D3DDECLTYPE_FLOAT1 + blends - 1), D3DDECLUSAGE_BLENDWEIGHT, 0, static_cast<WORD>(4 * blends));
    }
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    int tex = static_cast<int>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (int t = 0; t < tex && t < 5; t++) {
        int fmt = static_cast<int>((fvf >> (16 + t * 2)) & 3);   // (D3DFVF_TEXTUREFORMAT2 0, 1 3, 2 4, 3 1)
        int n = fmt == 0 ? 2 : fmt == 1 ? 3 : fmt == 2 ? 4 : 1;
        add(static_cast<BYTE>(D3DDECLTYPE_FLOAT1 + n - 1), D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(t), static_cast<WORD>(4 * n));
    }
    el.push_back({1, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5});
    el.push_back({1, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6});
    el.push_back({1, 28, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7});
    el.push_back(D3DDECL_END());
    IDirect3DVertexDeclaration9 *decl = nullptr;
    if (FAILED(d->CreateVertexDeclaration(el.data(), &decl))) decl = nullptr;
    g_decls[fvf] = decl;
    return decl;
}

Mirror *MirrorOf(IDirect3DDevice9 *d, IDirect3DVertexBuffer9 *game, UINT gameStride) {
    auto it = g_mirrors.find(game);
    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(game->GetDesc(&desc)) || !gameStride) return nullptr;
    UINT verts = desc.Size / gameStride;
    if (it != g_mirrors.end() && it->second.dev == d && it->second.verts == verts && it->second.vb) return &it->second;
    Mirror &m = g_mirrors[game];
    SafeRelease(m.vb);
    m = Mirror{};
    m.dev = d, m.game = game, m.verts = verts;
    if (FAILED(d->CreateVertexBuffer(verts * kStride, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m.vb, nullptr))) return nullptr;
    void *p = nullptr;
    if (SUCCEEDED(m.vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p) { memset(p, 0, verts * kStride); m.vb->Unlock(); }   // (zeros: no data)
    return &m;
}

// The level a draw of the visual is: the LOD whose place in its pool is the draw's base (LS3D's objects), else the
// LOD whose own vertices start at it (drawn from its own buffer)
const void *LevelOfDraw(const void *visual, INT base, UINT verts, int *lod) {
    const void *mesh = At<const void *>(visual, kVisMesh);
    if (!mesh) return nullptr;
    uint32_t n = std::min<uint32_t>(At<uint32_t>(mesh, kMeshLevels), kMaxLods);
    for (uint32_t i = 0; i < n; i++) {
        const void *rec = At<const void *>(visual, kVisLods + 4 * i);
        if (rec && At<int>(rec, kLodDrawFirst) == base) { *lod = static_cast<int>(i); return At<const void *>(mesh, kMeshLevel0 + 4 * i); }
    }
    for (uint32_t i = 0; i < n; i++) {
        const void *level = At<const void *>(mesh, kMeshLevel0 + 4 * i);
        if (level && At<int>(level, kLevelFirst) == base) { *lod = static_cast<int>(i); return level; }
    }
    // (a lightmapped object's LOD records are not the visual's +0x1E4: the LOD whose count of vertices is the draw's)
    for (uint32_t i = 0; i < n; i++) {
        const void *level = At<const void *>(mesh, kMeshLevel0 + 4 * i);
        if (level && At<uint32_t>(level, kLevelVerts) == verts) { *lod = static_cast<int>(i); return level; }
    }
    // (else a lightmapped object drawn from a vertex set of its own, split for its lightmap: one more vertex here and
    // there, in another order, at another place; seen in the city: no LOD record, base 387 against the mesh's 1992,
    // 134 vertices against 133. Its order is the lit object's, not known yet: left with zeros, ROADMAP Known gaps)
    return nullptr;
}

void Why(const void *level, const void *visual) {
    if (level) { g_whyNow[g_levels[level].why]++; return; }
    uint32_t t = At<uint32_t>(visual, 0x1D4);
    char k[5] = {static_cast<char>(t), static_cast<char>(t >> 8), static_cast<char>(t >> 16), static_cast<char>(t >> 24), 0};
    for (char &c : k) if (c && (c < 0x20 || c > 0x7E)) c = '?';
    g_whyNow[std::string("no LOD of the visual starts at the draw's base (") + k + ")"]++;
}

bool Write(Mirror &m, UINT at, const Level &lv, const Attr *attr) {
    if (static_cast<uint64_t>(at) + lv.verts > m.verts) return false;
    void *p = nullptr;
    if (FAILED(m.vb->Lock(at * kStride, lv.verts * kStride, &p, D3DLOCK_NOOVERWRITE)) || !p) return false;
    auto *o = static_cast<uint8_t *>(p);
    for (uint32_t i = 0; i < lv.verts; i++, o += kStride) {
        memcpy(o, &lv.nrm[i * 3], 12);
        memcpy(o + 12, &lv.tan[i * 4], 16);
        if (attr && attr->v.size() >= size_t(i + 1) * 4) memcpy(o + 28, &attr->v[i * 4], 16);
        else memset(o + 28, 0, 16);
    }
    m.vb->Unlock();
    g_writes++;
    return true;
}

} // namespace

// (shaders.cpp) a rule's draw with MML_SHADE_VERTEX_DATA: the declaration and the second stream set (the FVF and
// stream 1 kept in `saved` for VdataUnbind). False: the draw is not one this can serve (it goes without).
bool VdataBind(void *dev9, const void *visual, int base, unsigned verts, VdataSaved *saved) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    __try {
        uint32_t type = At<uint32_t>(visual, 0x1D4);
        if (type == kSingleMesh || type == kSingleMorph) { g_misses++; return false; }   // (people: skin_gpu's normals)
        DWORD fvf = 0;
        if (FAILED(d->GetFVF(&fvf)) || !fvf) { g_misses++; return false; }
        IDirect3DVertexBuffer9 *vb = nullptr;
        UINT off = 0, stride = 0;
        d->GetStreamSource(0, &vb, &off, &stride);
        if (!vb) { g_misses++; return false; }
        Mirror *m = MirrorOf(d, vb, stride);
        vb->Release();
        IDirect3DVertexDeclaration9 *decl = DeclOf(d, fvf);
        if (!m || !decl) { g_misses++; return false; }
        int lod = -1;
        const void *level = LevelOfDraw(visual, base, verts, &lod);
        const Level *lv = level ? LevelOf(level) : nullptr;
        if (!lv) Why(level, visual);
        if (lv && base >= 0) {
            const Attr *attr = AttrOf(visual, lod);
            Owner &o = m->owners[static_cast<UINT>(base)];
            if (o.level != level || o.verts != lv->verts || o.attr != attr || (attr && o.attrGen != attr->gen)) {
                if (Write(*m, static_cast<UINT>(base), *lv, attr)) o = Owner{level, lv->verts, attr ? attr->gen : 0, attr};
            }
        } else {
            g_misses++;   // (zeros: the mirror's place stays as it was)
        }
        saved->fvf = fvf;
        IDirect3DVertexBuffer9 *old1 = nullptr;
        d->GetStreamSource(1, &old1, &saved->off1, &saved->stride1);
        saved->vb1 = old1;
        d->SetVertexDeclaration(decl);
        d->SetStreamSource(1, m->vb, 0, kStride);
        g_binds++;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void VdataUnbind(void *dev9, VdataSaved &saved) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    d->SetStreamSource(1, static_cast<IDirect3DVertexBuffer9 *>(saved.vb1), saved.off1, saved.stride1);
    if (saved.vb1) static_cast<IDirect3DVertexBuffer9 *>(saved.vb1)->Release();
    d->SetFVF(saved.fvf);
}

int VdataSet(MmlMod *mod, const char *visual, int lod, const float *attr, int vertices, std::string *err) {
    if (!visual || !*visual) { *err = "visual: a frame's name (\"Tommy.base\", or the .4ds's \"base\")"; return 0; }
    std::string name = Lower(visual);
    g_attrs.erase(std::remove_if(g_attrs.begin(), g_attrs.end(), [&](const Attr &a) { return a.mod == mod && a.name == name && a.lod == lod; }),
                  g_attrs.end());
    if (attr && vertices > 0) g_attrs.push_back(Attr{mod, name, lod, std::vector<float>(attr, attr + size_t(vertices) * 4), g_attrGen++});
    return 1;
}
void VdataModGone(MmlMod *mod) {
    g_attrs.erase(std::remove_if(g_attrs.begin(), g_attrs.end(), [&](const Attr &a) { return a.mod == mod; }), g_attrs.end());
}
void VdataMissionClose() {
    g_levels.clear();
    for (auto &kv : g_mirrors) kv.second.owners.clear();   // (the pools' places are given anew)
}
void VdataFrameEnd() {
    g_bindsLast = g_binds, g_writesLast = g_writes, g_missesLast = g_misses;
    g_binds = g_writes = g_misses = 0;
    g_whyLast.swap(g_whyNow);
    g_whyNow.clear();
}
std::string VdataState() {
    nlohmann::json why = nlohmann::json::object();
    for (auto &kv : g_whyLast) why[kv.first] = kv.second;
    nlohmann::json j = {{"draws_last_frame", g_bindsLast}, {"places_written_last_frame", g_writesLast}, {"left_out_last_frame", g_missesLast},
                        {"why_left_out", why}, {"mirrors", g_mirrors.size()}, {"levels", g_levels.size()}, {"attributes", g_attrs.size()}};
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace host
