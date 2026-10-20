// People's skinning extended (mml.h skin_set, skin_get; docs/MODDING.md "People's skinning"; knowledge/notes/skinning.md).
//
// LS3D skins a single mesh ('SNGM') or a single morph ('SMMR': the people, their faces morphed) on the CPU:
// I3D_singlemesh::PoseLod (game_build.h smUpdateLod) runs when the bones moved, each of the LOD's bone records
// (0x68 bytes: +0x00 the mesh's space at rest to the joint's, +0x40 the vertices wholly its own, +0x44 those blended
// with one other bone, +0x48 that bone 1-based (0: the mesh's own space), +0x64 their weights) taking its vertices
// through the record's matrix times the joint's matrix in the mesh's space: two bones a vertex at most, in the data
// itself. The vertices weighted by no bone (the LOD's last ones) are copied once. The result, a position and a normal a
// vertex (POS_NORM), goes to the LOD's cache; m_AddFGroups copies the positions into the drawn buffer (CopyRigidPositions
// the rigid ones from the mesh's own buffer, CopySkinnedPositions the skinned ones from the cache) and lights the vertices
// from the cache's normals (LightVertices).
//
// A model whose skin a mod gives (a file `models\<name>.skin.json` among its files, or skin_set) is skinned here
// instead: up to 8 bones a vertex, any joint of the mesh's subtree (not only the 64 LS3D binds), each bone with its own
// matrix at rest; the same cache, so LS3D's lighting, its drawing and everything else that reads the cache go on as
// before. Every other model keeps LS3D's own code. The joints' matrices are LS3D's own (the one cached at +0x1C4 while
// +0x204 bit 0x10 is set, else built as PoseLod builds it, the frames' UpdateMatrix called on the way), each
// normal's matrix its matrix's columns normalised (S_matrix::SetRot).
//
// Bridge methods: skin.state, skin.export, skin.set, skin.reload.
#include "host.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// ---- LS3D's layouts (both builds)
constexpr int kSmFlags = 0x1D8, kSmMesh = 0x1E0, kSmLods = 0x1E4, kSmBoneCount = 0x21C, kSmBones = 0x220, kSmStamp = 0x224;
constexpr uint32_t kSmInstance = 0x10000;   // (an instance: its +0x08 the mesh whose LODs it shares)
constexpr int kLodLight = 0x04, kLodLight2 = 0x08, kLodSkin = 0x30, kLodCache = 0x34, kLodCacheFirst = 0x3C, kLodStamp = 0x40;
constexpr int kSkinRecords = 0x08, kSkinRigid = 0x0C, kRecSize = 0x68;
constexpr int kMeshLevels = 0x0C, kLevelVerts = 0x08, kLevelVb = 0x0C, kLevelFirst = 0x14;
constexpr int kJointFlags = 0x204, kJointModel = 0x1C4, kFrameLocal = 0x50;
constexpr int kMaxLods = 8, kMaxInfluences = 8;
constexpr int kSrcStride = 0x20;   // (the mesh's own vertices: position, normal, uv)

template <class T> T &F(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// "Models\\Tommy.i3d", "tommy", "models/Tommy.4ds" -> models\tommy.4ds (the key of a model)
std::string ModelKey(std::string f) {
    for (char &c : f) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t slash = f.rfind('\\'), dot = f.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) f += ".4ds";
    else if (f.substr(dot) == ".i3d") f = f.substr(0, dot) + ".4ds";
    if (f.find('\\') == std::string::npos) f = "models\\" + f;
    return f;
}
std::string SkinFileOf(const std::string &modelKey) { return modelKey.substr(0, modelKey.size() - 4) + ".skin.json"; }

// ---- matrices: LS3D's (row vectors, a point times the matrix)
struct M44 { float m[16]; };
// S_matrix::operator*: the 3x3 parts multiplied, the translation of a through b plus b's own
M44 Mul(const float *a, const float *b) {
    M44 o{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.m[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
    for (int j = 0; j < 3; j++) o.m[12 + j] = a[12] * b[j] + a[13] * b[4 + j] + a[14] * b[8 + j] + b[12 + j];
    o.m[15] = 1;
    return o;
}
// S_matrix::SetRot: the 3x3 part, each column normalised unless its length is 1 within 1e-8
M44 SetRot(const M44 &s) {
    M44 o{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.m[i * 4 + j] = s.m[i * 4 + j];
    for (int j = 0; j < 3; j++) {
        float l = o.m[j] * o.m[j] + o.m[4 + j] * o.m[4 + j] + o.m[8 + j] * o.m[8 + j];
        if (std::fabs(l - 1.0f) > 1e-8f && l > 0) {
            float k = 1.0f / std::sqrt(l);
            o.m[j] *= k, o.m[4 + j] *= k, o.m[8 + j] *= k;
        }
    }
    o.m[15] = 1;
    return o;
}
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

using FrameFn = void(__fastcall *)(void *frame);   // (thiscall with no arguments: the same)
void UpdateMatrix(void *frame) {
    reinterpret_cast<FrameFn>(ModuleBase("LS3DF.dll") + game::Ls3d().frameUpdateMatrix)(frame);
}
// A joint's matrix in its single mesh's space, as PoseLod builds it: kept at +0x1C4 while +0x204 bit 0x10 is set;
// else its local matrix times its parents' up to a visual, a sector or a model (types 1, 5, 9), or up to a joint
// whose own is kept.
const float *JointMatrix(void *j) {
    if (!(F<uint32_t>(j, kJointFlags) & 0x10)) {
        F<uint32_t>(j, kJointFlags) |= 0x10;
        UpdateMatrix(j);
        float *m = &F<float>(j, kJointModel);
        memcpy(m, &F<float>(j, kFrameLocal), 64);
        for (void *p = F<void *>(j, game::kFrameParent); p; p = F<void *>(p, game::kFrameParent)) {
            int t = F<int>(p, game::kFrameType);
            if (t == 1 || t == 5 || t == 9) break;
            if (t == 10 && (F<uint32_t>(p, kJointFlags) & 0x10)) {
                M44 r = Mul(m, &F<float>(p, kJointModel));
                memcpy(m, r.m, 64);
                break;
            }
            UpdateMatrix(p);
            M44 r = Mul(m, &F<float>(p, kFrameLocal));
            memcpy(m, r.m, 64);
        }
    }
    return &F<float>(j, kJointModel);
}

// LS3D's vertex buffers (its own wrapper: +0 the locked data or null; 1.2 +0x04 its flags (0x10: locked another way,
// unlocked first), +0x18 the stride, +0x1C the IDirect3DVertexBuffer8, +0x48 its place in the locked list; 1.0 +0x14,
// +0x18, no flags, no list) locked as LS3D locks them, so LS3D unlocks them as its own before drawing.
uint8_t *Locked(void *w) {
    const game::Ls3dBuild &l = game::Ls3d();
    uint8_t *data = F<uint8_t *>(w, 0);
    void *d3d = F<void *>(w, l.vbD3d);
    using LockFn = HRESULT(__stdcall *)(void *, UINT, UINT, BYTE **, DWORD);
    using UnlockFn = HRESULT(__stdcall *)(void *);
    auto vt = *reinterpret_cast<void ***>(d3d);
    if (l.lockList) {
        if (data && !(F<uint32_t>(w, 4) & 0x10)) return data;
        if (data) reinterpret_cast<UnlockFn>(vt[12])(d3d);
        F<uint32_t>(w, 4) = 0;
        reinterpret_cast<LockFn>(vt[11])(d3d, 0, 0, &F<BYTE *>(w, 0), 0);
        if (F<int>(w, 0x48) < 0) {
            uintptr_t list = ModuleBase("LS3DF.dll") + l.lockList;
            F<int>(w, 0x48) = *reinterpret_cast<int *>(list + 8);
            using AppendFn = void(__fastcall *)(void *, void *, void **);
            void *item = w;
            reinterpret_cast<AppendFn>(ModuleBase("LS3DF.dll") + l.lockAppend)(reinterpret_cast<void *>(list), nullptr, &item);
        }
    } else if (!data) {
        reinterpret_cast<LockFn>(vt[11])(d3d, 0, 0, &F<BYTE *>(w, 0), 0x800);   // (D3DLOCK_NOSYSLOCK)
    }
    return F<uint8_t *>(w, 0);
}
int StrideOf(void *w) { return F<int>(w, game::Ls3d().vbStride); }

// ---- a skin given by a mod: its bones and each vertex's influences, a LOD each
struct Bone { std::string joint; float bind[16]; };
struct Lod {
    uint32_t vertices = 0;
    std::vector<Bone> bones;
    std::vector<uint8_t> n;       // influences a vertex (0: left as LS3D has it: the vertices weighted by no bone)
    std::vector<uint16_t> idx;    // 8 a vertex
    std::vector<float> w;         // 8 a vertex, summing to 1
};
struct Def {
    std::string model, mesh;      // the model's key (models\tommy.4ds), the mesh's name in it (lower case)
    std::vector<Lod> lods;
    MmlMod *mod = nullptr;        // (null: a file's or the bridge's)
    std::string source;           // the file, the mod's id, "bridge"
    unsigned gen = 0;
};
// by "model|mesh": the skins given for it, the last one given the one used (a mod's over a file's, the bridge's over
// both while it holds one); one a giver (a mod, a file, the bridge)
std::map<std::string, std::vector<std::shared_ptr<Def>>> g_defs;
std::map<std::string, std::string> g_files;          // the model's key -> its skin file's state ("" none, else what it gave)
unsigned g_gen = 1;
unsigned g_defsGen = 1;   // (a skin added or removed, a file forgotten: every mesh looks again)

// A skin's LODs from JSON (the file's format, docs/MODDING.md): "lods": [{"vertices", "bones": [{"joint", "bind"}],
// "influences": [[bone, weight, ...], ...]}].
bool ParseLods(const json &j, std::vector<Lod> *out, std::string *err) {
    if (!j.is_array() || j.empty()) { *err = "lods: a list of LODs"; return false; }
    for (size_t l = 0; l < j.size(); l++) {
        const json &jl = j[l];
        Lod lod;
        std::string at = "lods[" + std::to_string(l) + "]";
        lod.vertices = jl.value("vertices", 0u);
        const json &bones = jl.contains("bones") ? jl["bones"] : json();
        if (!bones.is_array() || bones.empty() || bones.size() > 0xFFFF) { *err = at + ".bones: a list of bones"; return false; }
        for (const json &b : bones) {
            Bone bo{};
            bo.joint = b.contains("joint") && b["joint"].is_string() ? b["joint"].get<std::string>() : std::string();
            memcpy(bo.bind, kIdentity, sizeof bo.bind);
            if (b.contains("bind")) {
                const json &m = b["bind"];
                if (!m.is_array() || m.size() != 16) { *err = at + ".bones: bind is 16 numbers (a 4x4 matrix, row vectors)"; return false; }
                for (int i = 0; i < 16; i++) bo.bind[i] = m[i].get<float>();
            }
            lod.bones.push_back(std::move(bo));
        }
        const json &inf = jl.contains("influences") ? jl["influences"] : json();
        if (!inf.is_array()) { *err = at + ".influences: a list a vertex"; return false; }
        if (!lod.vertices) lod.vertices = static_cast<uint32_t>(inf.size());
        if (inf.size() != lod.vertices) { *err = at + ": " + std::to_string(inf.size()) + " influences for " + std::to_string(lod.vertices) + " vertices"; return false; }
        lod.n.assign(lod.vertices, 0);
        lod.idx.assign(size_t(lod.vertices) * kMaxInfluences, 0);
        lod.w.assign(size_t(lod.vertices) * kMaxInfluences, 0.0f);
        for (uint32_t v = 0; v < lod.vertices; v++) {
            const json &p = inf[v];
            if (!p.is_array() || p.size() % 2 || p.size() > 2 * kMaxInfluences) {
                *err = at + ".influences[" + std::to_string(v) + "]: bone, weight pairs, 8 at most";
                return false;
            }
            float sum = 0;
            int n = 0;
            for (size_t k = 0; k < p.size(); k += 2) {
                int b = p[k].get<int>();
                float w = p[k + 1].get<float>();
                if (b < 0 || b >= static_cast<int>(lod.bones.size())) { *err = at + ".influences[" + std::to_string(v) + "]: no bone " + std::to_string(b); return false; }
                if (!(w > 0)) continue;
                lod.idx[v * kMaxInfluences + n] = static_cast<uint16_t>(b);
                lod.w[v * kMaxInfluences + n] = w;
                sum += w;
                n++;
            }
            if (n && !(sum > 1e-6f)) { *err = at + ".influences[" + std::to_string(v) + "]: weights sum to 0"; return false; }
            for (int k = 0; k < n; k++) lod.w[v * kMaxInfluences + k] /= sum;
            lod.n[v] = static_cast<uint8_t>(n);
        }
        out->push_back(std::move(lod));
    }
    return true;
}

bool AddDef(const std::string &model, const std::string &mesh, std::vector<Lod> lods, MmlMod *mod, const std::string &source, std::string *err) {
    if (mesh.empty()) { *err = "no mesh name"; return false; }
    auto d = std::make_shared<Def>();
    d->model = ModelKey(model);
    d->mesh = Lower(mesh);
    d->lods = std::move(lods);
    d->mod = mod;
    d->source = source;
    d->gen = g_gen++;
    auto &stack = g_defs[d->model + "|" + d->mesh];
    stack.erase(std::remove_if(stack.begin(), stack.end(), [&](const std::shared_ptr<Def> &o) { return o->mod == mod && o->source == source; }),
                stack.end());
    stack.push_back(d);
    g_defsGen++;
    return true;
}
// A giver's skins taken away (keep: whether one stays).
void DropDefs(const std::function<bool(const Def &)> &drop) {
    for (auto it = g_defs.begin(); it != g_defs.end();) {
        auto &s = it->second;
        s.erase(std::remove_if(s.begin(), s.end(), [&](const std::shared_ptr<Def> &d) { return drop(*d); }), s.end());
        it = s.empty() ? g_defs.erase(it) : std::next(it);
    }
    g_defsGen++;
}

// The skin file of a model, read once (skin.reload forgets them): `models\<name>.skin.json` as the game's file layer
// answers it (a mod's files\ first).
void ReadFileOf(const std::string &modelKey) {
    if (g_files.count(modelKey)) return;
    std::string path = SkinFileOf(modelKey);
    int64_t size = FilesRead(path.c_str(), nullptr, 0, 0);
    if (size <= 0) { g_files[modelKey] = ""; return; }
    std::string text(static_cast<size_t>(size), '\0');
    if (FilesRead(path.c_str(), text.data(), text.size(), 0) != size) { g_files[modelKey] = "unreadable"; return; }
    json j = json::parse(text, nullptr, false);
    std::string err, state;
    if (!j.is_object() || !j.contains("meshes") || !j["meshes"].is_array()) err = "not a skin file (\"meshes\": [...])";
    else {
        int n = 0;
        for (const json &m : j["meshes"]) {
            std::vector<Lod> lods;
            std::string e;
            try {
                if (!ParseLods(m.contains("lods") ? m["lods"] : json(), &lods, &e) || !AddDef(modelKey, m.value("mesh", ""), std::move(lods), nullptr, path, &e)) {
                    err = m.value("mesh", "?") + ": " + e;
                    break;
                }
            } catch (const json::exception &x) {
                err = m.value("mesh", "?") + ": " + x.what();
                break;
            }
            n++;
        }
        state = "meshes: " + std::to_string(n);
    }
    g_files[modelKey] = err.empty() ? state : "error: " + err;
    if (!err.empty()) HWARN("skin: %s: %s", path.c_str(), err.c_str());
    else HLOG("skin: %s read (%s)", path.c_str(), state.c_str());
}

// ---- the single meshes seen, each bound to its skin
struct Vis {
    void *vtable = nullptr, *mesh = nullptr, *bones = nullptr;   // (what it was when bound: another object at the address differs)
    std::string model, name;
    std::shared_ptr<Def> def;
    unsigned gen = 0;
    std::vector<std::vector<void *>> joints;   // a LOD's bones' joints (null: the mesh's own space)
    std::string error;
    unsigned skinned = 0;
    unsigned looked = 0;   // (g_defsGen when its skin was looked up: none is looked for again until a skin comes or goes)
};
std::unordered_map<void *, Vis> g_vis;
unsigned g_calls, g_ours, g_faults;
double g_ms;
LARGE_INTEGER g_freq;
double NowMs() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * t.QuadPart / g_freq.QuadPart;
}

// A frame's name is its model's frame's name, a dot, its own in the .4ds ("Tommy.r_thigh", "p_traff_elem.base"): the
// .4ds's name is what follows the first dot.
const char *NodeName(const char *n) {
    const char *dot = n ? strchr(n, '.') : nullptr;
    return dot ? dot + 1 : n;
}
void *FindJoint(void *root, const std::string &name, int depth = 0) {
    if (!root || depth > 64) return nullptr;
    for (void *c = F<void *>(root, 0x124); c; c = F<void *>(c, 0x12C)) {
        const char *n = F<const char *>(c, game::kFrameName);
        if (n && F<int>(c, game::kFrameType) == 10 && (_stricmp(NodeName(n), name.c_str()) == 0 || _stricmp(n, name.c_str()) == 0)) return c;
        if (void *f = FindJoint(c, name, depth + 1)) return f;
    }
    return nullptr;
}

// The skin this mesh takes, bound to its joints (null: LS3D's own).
Vis *Bind(void *mesh) {
    auto it = g_vis.find(mesh);
    void *vt = *reinterpret_cast<void **>(mesh), *m = F<void *>(mesh, kSmMesh), *b = F<void *>(mesh, kSmBones);
    if (it != g_vis.end() && (it->second.vtable != vt || it->second.mesh != m || it->second.bones != b)) { g_vis.erase(it); it = g_vis.end(); }
    if (it == g_vis.end()) {
        Vis v;
        v.vtable = vt, v.mesh = m, v.bones = b;
        std::string file;
        ModelFile(mesh, &file);
        v.model = file.empty() ? std::string() : ModelKey(file);
        if (const char *n = F<const char *>(mesh, game::kFrameName)) v.name = NodeName(n);
        it = g_vis.emplace(mesh, std::move(v)).first;
    }
    Vis &v = it->second;
    if (v.looked == g_defsGen) return v.def && v.error.empty() ? &v : nullptr;   // (LS3D's own models: this alone a call)
    v.looked = g_defsGen;
    if (v.model.empty()) return nullptr;
    ReadFileOf(v.model);
    v.looked = g_defsGen;   // (a file just read added its skins)
    auto d = g_defs.find(v.model + "|" + Lower(v.name));
    if (d == g_defs.end() || d->second.empty()) { v.def.reset(); return nullptr; }
    const std::shared_ptr<Def> &use = d->second.back();
    if (v.def != use || v.gen != use->gen) {   // (bound again: a new skin)
        v.def = use;
        v.gen = v.def->gen;
        v.error.clear();
        v.joints.clear();
        for (const Lod &l : v.def->lods) {
            std::vector<void *> js;
            for (const Bone &bo : l.bones) {
                void *j = bo.joint.empty() ? nullptr : FindJoint(mesh, bo.joint);
                if (!bo.joint.empty() && !j && v.error.empty()) v.error = "no joint \"" + bo.joint + "\" under the mesh";
                js.push_back(j);
            }
            v.joints.push_back(std::move(js));
        }
        if (!v.error.empty()) HWARN("skin: %s %s: %s (LS3D's own skin kept)", v.model.c_str(), v.name.c_str(), v.error.c_str());
    }
    return v.error.empty() ? &v : nullptr;
}

using UpdateLodFn = void(__fastcall *)(void *mesh, void *edx, int lod);
UpdateLodFn oUpdateLod;
using VbPosFn = void *(__fastcall *)(void *mesh, void *edx, uint8_t *dest, int stride, int lod);
VbPosFn oVbPos0;

// The LOD skinned into its cache with the mod's influences. False: not done (LS3D's own then).
bool SkinLod(void *mesh, Vis &v, int lod) {
    if (lod < 0 || lod >= static_cast<int>(v.def->lods.size()) || lod >= kMaxLods) return false;
    const Lod &L = v.def->lods[lod];
    void *rec = F<void *>(mesh, kSmLods + 4 * lod), *data = F<void *>(mesh, kSmMesh);
    void *level = data ? F<void *>(data, kMeshLevels + 4 * lod) : nullptr;
    if (!rec || !level || !F<void *>(rec, kLodSkin)) return false;
    if (F<uint32_t>(level, kLevelVerts) != L.vertices) {
        if (v.error.empty()) {
            v.error = "LOD " + std::to_string(lod) + ": " + std::to_string(L.vertices) + " vertices given, the mesh has " + std::to_string(F<uint32_t>(level, kLevelVerts));
            HWARN("skin: %s %s: %s (LS3D's own skin kept)", v.model.c_str(), v.name.c_str(), v.error.c_str());
        }
        return false;
    }
    if (!F<void *>(rec, kLodCache) || !F<void *>(mesh, kSmBones)) return false;   // (made by LS3D's first call)
    // what PoseLod marks: the lighting and the stamp
    F<int>(rec, kLodLight) = -700;
    F<int>(rec, kLodLight2) = -600;
    F<uint32_t>(rec, kLodStamp) = F<uint32_t>(mesh, kSmStamp);
    uint8_t *cache = Locked(F<void *>(rec, kLodCache)), *src = Locked(F<void *>(level, kLevelVb));
    if (!cache || !src) return false;
    int cs = StrideOf(F<void *>(rec, kLodCache)), ss = StrideOf(F<void *>(level, kLevelVb));
    cache += size_t(F<uint32_t>(rec, kLodCacheFirst)) * cs;
    src += size_t(F<uint32_t>(level, kLevelFirst)) * ss;
    // the bones' matrices now
    static std::vector<M44> pos, nrm;   // (PoseLod runs on the game thread only)
    pos.resize(L.bones.size());
    nrm.resize(L.bones.size());
    const std::vector<void *> &js = v.joints[lod];
    for (size_t b = 0; b < L.bones.size(); b++) {
        if (!js[b]) { memcpy(pos[b].m, kIdentity, 64); memcpy(nrm[b].m, kIdentity, 64); continue; }
        pos[b] = Mul(L.bones[b].bind, JointMatrix(js[b]));
        nrm[b] = SetRot(pos[b]);
    }
    void *skin = F<void *>(rec, kLodSkin);
    const uint32_t firstRigid = L.vertices - std::min(F<uint32_t>(skin, kSkinRigid), L.vertices);
    for (uint32_t i = 0; i < L.vertices; i++) {
        int n = L.n[i];
        if (!n) {
            if (i >= firstRigid) continue;
            v.error = "LOD " + std::to_string(lod) + ": vertex " + std::to_string(i) + " has no bone (only the last " +
                      std::to_string(L.vertices - firstRigid) + ", the ones the .4ds weights by none, may have none)";
            HWARN("skin: %s %s: %s (LS3D's own skin kept)", v.model.c_str(), v.name.c_str(), v.error.c_str());
            return false;
        }
        const float *p = reinterpret_cast<const float *>(src + size_t(i) * ss), *q = p + 3;
        float o[6] = {};
        for (int k = 0; k < n; k++) {
            const float *a = pos[L.idx[i * kMaxInfluences + k]].m, *r = nrm[L.idx[i * kMaxInfluences + k]].m;
            float w = L.w[i * kMaxInfluences + k];
            o[0] += w * (p[0] * a[0] + p[1] * a[4] + p[2] * a[8] + a[12]);
            o[1] += w * (p[0] * a[1] + p[1] * a[5] + p[2] * a[9] + a[13]);
            o[2] += w * (p[0] * a[2] + p[1] * a[6] + p[2] * a[10] + a[14]);
            o[3] += w * (q[0] * r[0] + q[1] * r[4] + q[2] * r[8]);
            o[4] += w * (q[0] * r[1] + q[1] * r[5] + q[2] * r[9]);
            o[5] += w * (q[0] * r[2] + q[1] * r[6] + q[2] * r[10]);
        }
        memcpy(cache + size_t(i) * cs, o, sizeof o);
    }
    return true;
}

void __fastcall hkUpdateLod(void *mesh, void *edx, int lod) {
    g_calls++;
    Vis *v = nullptr;
    bool instance = true;
    __try {
        instance = (F<uint32_t>(mesh, kSmFlags) & kSmInstance) != 0;
        if (!instance) v = Bind(mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        v = nullptr;
    }
    // (an instance's PoseLod calls its mesh's through the vtable: here again)
    if (!v) { oUpdateLod(mesh, edx, lod); return; }
    double t0 = NowMs();
    bool done = false;
    __try {
        void *rec = lod >= 0 && lod < kMaxLods ? F<void *>(mesh, kSmLods + 4 * lod) : nullptr;
        // (the first time LS3D binds the joints and makes the cache: its own PoseLod does it)
        if (rec && (!F<void *>(mesh, kSmBones) || !F<void *>(rec, kLodCache))) oUpdateLod(mesh, edx, lod);
        done = SkinLod(mesh, *v, lod);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        done = false;
    }
    if (!done) { oUpdateLod(mesh, edx, lod); return; }
    v->skinned++;
    g_ours++;
    g_ms += NowMs() - t0;
}

// The rigid vertices' positions into the drawn buffer: from the cache for a mesh whose skin moves them too.
void *__fastcall hkVbPos0(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    void *r = oVbPos0(mesh, edx, dest, stride, lod);
    __try {
        void *m = mesh;
        for (int i = 0; i < 8 && (F<uint32_t>(m, kSmFlags) & kSmInstance); i++) m = F<void *>(m, 0x08);
        auto it = g_vis.find(m);
        if (it == g_vis.end() || !it->second.def || !it->second.error.empty() || lod < 0 || lod >= static_cast<int>(it->second.def->lods.size()))
            return r;
        const Lod &L = it->second.def->lods[lod];
        void *rec = F<void *>(m, kSmLods + 4 * lod);
        void *skin = rec ? F<void *>(rec, kLodSkin) : nullptr;
        if (!skin || !F<void *>(rec, kLodCache)) return r;
        uint32_t rigid = F<uint32_t>(skin, kSkinRigid);
        if (!rigid || rigid > L.vertices) return r;
        uint8_t *cache = Locked(F<void *>(rec, kLodCache));
        int cs = StrideOf(F<void *>(rec, kLodCache));
        if (!cache) return r;
        cache += size_t(F<uint32_t>(rec, kLodCacheFirst)) * cs;
        for (uint32_t i = L.vertices - rigid; i < L.vertices; i++)
            if (L.n[i]) memcpy(dest + size_t(i) * stride, cache + size_t(i) * cs, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
    }
    return r;
}

bool g_hookTried, g_hooked;
std::string g_hookWhy;
bool EnsureHooks() {
    if (g_hookTried) return g_hooked;
    if (!game::g_ls3d) { g_hookWhy = "no game known"; return false; }
    g_hookTried = true;
    const game::Ls3dBuild &l = game::Ls3d();
    if (!l.smUpdateLod || !l.smUpdateVBPos0 || !l.frameUpdateMatrix) { g_hookWhy = "not known for this LS3DF.dll"; return false; }
    uintptr_t base = ModuleBase("LS3DF.dll");
    std::string err;
    if (!HookAdd(nullptr, base + l.smUpdateLod, reinterpret_cast<void *>(&hkUpdateLod), reinterpret_cast<void **>(&oUpdateLod), &err)) {
        g_hookWhy = "PoseLod not hooked: " + err;
        return false;
    }
    if (!HookAdd(nullptr, base + l.smUpdateVBPos0, reinterpret_cast<void *>(&hkVbPos0), reinterpret_cast<void **>(&oVbPos0), &err)) {
        HookRemoveLink(nullptr, base + l.smUpdateLod, reinterpret_cast<void *>(&hkUpdateLod));
        g_hookWhy = "CopyRigidPositions not hooked: " + err;
        return false;
    }
    g_hooked = true;
    HLOG("skin: I3D_singlemesh::PoseLod and CopyRigidPositions hooked (models with a skin of a mod's)");
    return true;
}

// ---- LS3D's own skin of a model's mesh, as skin_set takes one: bone 0 the mesh's own space, then the .4ds's records
struct Got {
    ModelSkinLod rest;
    std::string mesh;
    int lods = 0;
    std::vector<MmlSkinBone> bones;
    std::vector<uint8_t> n;
    std::vector<uint16_t> idx;
    std::vector<float> w;
};
std::map<MmlMod *, Got> g_got;   // (each mod's last skin_get: its arrays live until the next)

bool FromLs3d(const ModelSkinLod &l, Got *g, std::string *err) {
    g->bones.clear();
    MmlSkinBone root{};
    root.joint = "";
    memcpy(root.bind, kIdentity, sizeof root.bind);
    g->bones.push_back(root);
    for (const auto &b : l.bones) {
        MmlSkinBone o{};
        o.joint = b.joint.c_str();
        memcpy(o.bind, b.bind, sizeof o.bind);
        g->bones.push_back(o);
    }
    g->n.assign(l.vertices, 0);
    g->idx.assign(size_t(l.vertices) * kMaxInfluences, 0);
    g->w.assign(size_t(l.vertices) * kMaxInfluences, 0.0f);
    uint32_t v = 0;
    for (size_t b = 0; b < l.bones.size(); b++) {
        for (uint32_t k = 0; k < l.bones[b].one && v < l.vertices; k++, v++) {
            g->n[v] = 1, g->idx[v * kMaxInfluences] = static_cast<uint16_t>(b + 1), g->w[v * kMaxInfluences] = 1;
        }
        for (uint32_t k = 0; k < l.bones[b].blended && v < l.vertices; k++, v++) {
            float w = k < l.bones[b].weights.size() ? l.bones[b].weights[k] : 1.0f;
            g->idx[v * kMaxInfluences] = static_cast<uint16_t>(b + 1), g->w[v * kMaxInfluences] = w;
            g->n[v] = 1;
            if (w < 1.0f) {   // (the other bone, 1-based among the records: 0 the mesh's own space, our bone 0)
                g->idx[v * kMaxInfluences + 1] = static_cast<uint16_t>(l.bones[b].other), g->w[v * kMaxInfluences + 1] = 1.0f - w;
                g->n[v] = 2;
            }
        }
    }
    if (v + l.rigid != l.vertices) {
        *err = "the .4ds's records cover " + std::to_string(v) + " and " + std::to_string(l.rigid) + " rigid of " + std::to_string(l.vertices) + " vertices";
        return false;
    }
    return true;
}

bool ToLod(const MmlSkinLod &in, Lod *out, std::string *err) {
    if (!in.vertices || !in.bones || !in.bone || !in.count || !in.index || !in.weight) { *err = "a LOD needs vertices, bones and the arrays"; return false; }
    if (in.bones > 0xFFFF) { *err = "65535 bones at most"; return false; }
    out->vertices = in.vertices;
    out->bones.clear();
    for (uint32_t b = 0; b < in.bones; b++) {
        Bone bo;
        bo.joint = in.bone[b].joint ? in.bone[b].joint : "";
        memcpy(bo.bind, in.bone[b].bind, sizeof bo.bind);
        out->bones.push_back(std::move(bo));
    }
    out->n.assign(in.vertices, 0);
    out->idx.assign(size_t(in.vertices) * kMaxInfluences, 0);
    out->w.assign(size_t(in.vertices) * kMaxInfluences, 0.0f);
    for (uint32_t v = 0; v < in.vertices; v++) {
        int c = std::min<int>(in.count[v], kMaxInfluences), n = 0;
        float sum = 0;
        for (int k = 0; k < c; k++) {
            uint16_t b = in.index[size_t(v) * kMaxInfluences + k];
            float w = in.weight[size_t(v) * kMaxInfluences + k];
            if (b >= in.bones) { *err = "vertex " + std::to_string(v) + ": no bone " + std::to_string(b); return false; }
            if (!(w > 0)) continue;
            out->idx[size_t(v) * kMaxInfluences + n] = b, out->w[size_t(v) * kMaxInfluences + n] = w;
            sum += w;
            n++;
        }
        for (int k = 0; k < n; k++) out->w[size_t(v) * kMaxInfluences + k] /= sum;
        out->n[v] = static_cast<uint8_t>(n);
    }
    return true;
}

// ---- LS3D's own skin of a model's mesh, in the skin file's form (skin.export, skin_get)
bool Export(const std::string &model, const std::string &mesh, bool rest, json *out, std::string *err) {
    std::vector<ModelSkinLod> lods;
    std::string name;
    std::string key = ModelKey(model);
    if (!ModelSkin(key.c_str(), mesh.c_str(), &lods, &name, err)) return false;
    json jl = json::array();
    for (const ModelSkinLod &l : lods) {
        json bones = json::array();
        bones.push_back({{"joint", ""}, {"bind", std::vector<float>(kIdentity, kIdentity + 16)}});   // 0: the mesh's own space
        for (const auto &b : l.bones) bones.push_back({{"joint", b.joint}, {"bind", std::vector<float>(b.bind, b.bind + 16)}});
        json inf = json::array();
        uint32_t v = 0;
        for (size_t b = 0; b < l.bones.size(); b++) {
            for (uint32_t k = 0; k < l.bones[b].one; k++, v++) inf.push_back({b + 1, 1.0});
            for (uint32_t k = 0; k < l.bones[b].blended; k++, v++) {
                float w = k < l.bones[b].weights.size() ? l.bones[b].weights[k] : 1.0f;
                json p = {b + 1, w};
                if (w < 1.0f) { p.push_back(l.bones[b].other); p.push_back(1.0f - w); }
                inf.push_back(p);
            }
        }
        for (; v < l.vertices; v++) inf.push_back(json::array());   // (weighted by no bone: left as they are)
        if (inf.size() != l.vertices) { *err = name + ": the .4ds's records cover " + std::to_string(inf.size()) + " of " + std::to_string(l.vertices) + " vertices"; return false; }
        json j = {{"vertices", l.vertices}, {"rigid", l.rigid}, {"bones", bones}, {"influences", inf}};
        if (rest) j["rest"] = {{"positions", l.pos}, {"normals", l.nrm}, {"uvs", l.uv}, {"triangles", l.idx}};
        jl.push_back(j);
    }
    *out = {{"format", "mml-skin"}, {"version", 1}, {"model", key}, {"meshes", json::array({{{"mesh", name}, {"lods", jl}}})}};
    return true;
}

} // namespace

void SkinMissionClose() { g_vis.clear(); }

void SkinModGone(MmlMod *mod) {
    if (mod) DropDefs([&](const Def &d) { return d.mod == mod; });
    auto it = g_got.find(mod);
    if (it != g_got.end()) g_got.erase(it);
}

int SkinSet(MmlMod *mod, const char *model, const char *mesh, const MmlSkinLod *lods, int count, std::string *err) {
    if (!model || !*model || !mesh || !*mesh) { *err = "model and mesh: a model (\"Tommy\") and its single mesh's name (\"base\")"; return 0; }
    std::string key = ModelKey(model) + "|" + Lower(mesh), source = mod ? ModIdOf(mod) : "bridge";
    if (count <= 0) {
        DropDefs([&](const Def &d) { return d.mod == mod && d.model + "|" + d.mesh == key; });
        return 1;
    }
    if (!lods || count > kMaxLods) { *err = "lods: 1 to 8 LODs"; return 0; }
    std::vector<Lod> out(count);
    for (int i = 0; i < count; i++)
        if (!ToLod(lods[i], &out[i], err)) { *err = "LOD " + std::to_string(i) + ": " + *err; return 0; }
    if (!AddDef(model, mesh, std::move(out), mod, source, err)) return 0;
    if (!EnsureHooks()) { *err = g_hookWhy; return 0; }
    return 1;
}

int SkinGet(MmlMod *mod, const char *model, const char *mesh, int lod, MmlSkinMesh *out, std::string *err) {
    if (!out || !model) { *err = "no model or no out"; return 0; }
    std::vector<ModelSkinLod> lods;
    std::string name;
    if (!ModelSkin(ModelKey(model).c_str(), mesh ? mesh : "", &lods, &name, err)) return 0;
    if (lod < 0 || lod >= static_cast<int>(lods.size())) { *err = "no LOD " + std::to_string(lod); return 0; }
    Got &g = g_got[mod];
    g.rest = std::move(lods[lod]);
    g.mesh = name;
    g.lods = static_cast<int>(lods.size());
    if (!FromLs3d(g.rest, &g, err)) return 0;
    memset(out, 0, sizeof *out);
    strncpy_s(out->mesh, g.mesh.c_str(), _TRUNCATE);
    out->lods = g.lods;
    out->skin = MmlSkinLod{g.rest.vertices, static_cast<uint32_t>(g.bones.size()), g.bones.data(), g.n.data(), g.idx.data(), g.w.data()};
    out->rigid = g.rest.rigid;
    out->pos = g.rest.pos.data();
    out->normal = g.rest.nrm.data();
    out->uv = g.rest.uv.data();
    out->index = g.rest.idx.data();
    out->triangles = static_cast<int>(g.rest.idx.size() / 3);
    return 1;
}

// (game_hooks.cpp, at the game's start once the builds are known) hooked only when a skin is there to use: a file is
// looked for at a mesh's first skinning, so the hook goes in at once
void SkinStart() {
    if (game::g_ls3d) EnsureHooks();
}

bool SkinMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.rfind("skin.", 0) != 0) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "skin.state") {
            json vis = json::array();
            for (auto &kv : g_vis) {
                if (!kv.second.def && p.value("all", false) == false) continue;
                char a[16];
                snprintf(a, sizeof a, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(kv.first)));
                json j = {{"mesh", a}, {"model", kv.second.model}, {"name", kv.second.name}, {"skin", kv.second.def ? kv.second.def->source : "LS3D's"},
                          {"skinned", kv.second.skinned}};
                if (!kv.second.error.empty()) j["error"] = kv.second.error;
                vis.push_back(j);
            }
            json defs = json::array();
            for (auto &kv : g_defs)
                for (size_t i = 0; i < kv.second.size(); i++) {
                    const Def &d = *kv.second[i];
                    json lods = json::array();
                    for (const Lod &l : d.lods) lods.push_back({{"vertices", l.vertices}, {"bones", l.bones.size()}});
                    defs.push_back({{"model", d.model}, {"mesh", d.mesh}, {"source", d.mod ? std::string(ModIdOf(d.mod)) : d.source}, {"lods", lods},
                                    {"used", i + 1 == kv.second.size()}});
                }
            json files = json::object();
            for (auto &kv : g_files) if (!kv.second.empty()) files[SkinFileOf(kv.first)] = kv.second;
            *reply = dump({{"hooked", g_hooked ? json("on") : json(g_hookWhy)}, {"skins", defs}, {"files", files}, {"meshes", vis},
                           {"meshes_seen", g_vis.size()}, {"calls", g_calls}, {"ours", g_ours}, {"ms_ours", g_ms}, {"faults", g_faults}});
        } else if (method == "skin.export") {
            json out;
            if (!Export(p.value("model", "Tommy"), p.value("mesh", ""), p.value("rest", true), &out, &err)) { *reply = "skin.export: " + err; return true; }
            std::string dir = MmlDir() + "skin\\";
            MakeDir(dir);
            std::string name = out["model"].get<std::string>();
            name = name.substr(name.rfind('\\') + 1);
            std::string path = dir + name.substr(0, name.size() - 4) + ".skin.json";
            if (p.contains("path")) path = p["path"].get<std::string>();
            FILE *f = FileOpen(path, "wb");
            if (!f) { *reply = "skin.export: cannot write " + path; return true; }
            std::string text = out.dump(1, ' ', false, json::error_handler_t::replace);
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            json lods = json::array();
            for (const json &l : out["meshes"][0]["lods"]) lods.push_back({{"vertices", l["vertices"]}, {"bones", l["bones"].size()}, {"rigid", l["rigid"]}});
            *reply = dump({{"path", path}, {"model", out["model"]}, {"mesh", out["meshes"][0]["mesh"]}, {"lods", lods}});
        } else if (method == "skin.set") {
            std::vector<Lod> lods;
            if (p.contains("file")) {
                FILE *f = FileOpen(p["file"].get<std::string>(), "rb");
                if (!f) { *reply = "skin.set: cannot read " + p["file"].get<std::string>(); return true; }
                std::string text;
                char b[65536];
                for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) text.append(b, n);
                fclose(f);
                json j = json::parse(text, nullptr, false);
                if (!j.is_object() || !j.contains("meshes")) { *reply = "skin.set: not a skin file"; return true; }
                int n = 0;
                for (const json &m : j["meshes"]) {
                    lods.clear();
                    if (!ParseLods(m["lods"], &lods, &err) || !AddDef(j.value("model", p.value("model", "")), m.value("mesh", ""), std::move(lods), nullptr, "bridge", &err)) {
                        *reply = "skin.set: " + m.value("mesh", "?") + ": " + err;
                        return true;
                    }
                    n++;
                }
                *reply = dump({{"meshes", n}});
            } else {
                if (!ParseLods(p.contains("lods") ? p["lods"] : json(), &lods, &err) ||
                    !AddDef(p.value("model", ""), p.value("mesh", ""), std::move(lods), nullptr, "bridge", &err)) {
                    *reply = "skin.set: " + err;
                    return true;
                }
                *reply = dump({{"meshes", 1}});
            }
            EnsureHooks();
        } else if (method == "skin.check") {
            // the LOD skinned by LS3D's own PoseLod and by the mod's skin, the cache compared (an exported LS3D skin
            // given back must come out the same)
            void *mesh = reinterpret_cast<void *>(ParseAddress(p.value("mesh", "")));
            int lod = p.value("lod", 0);
            auto it = g_vis.find(mesh);
            if (it == g_vis.end() || !it->second.def) { *reply = "skin.check: mesh: a mesh skin.state lists with a skin"; return true; }
            Vis &v = it->second;
            void *rec = lod >= 0 && lod < kMaxLods ? F<void *>(mesh, kSmLods + 4 * lod) : nullptr;
            if (!rec || !F<void *>(rec, kLodCache)) { *reply = "skin.check: that LOD has no cache yet (draw it first)"; return true; }
            uint32_t verts = F<uint32_t>(F<void *>(F<void *>(mesh, kSmMesh), kMeshLevels + 4 * lod), kLevelVerts);
            uint8_t *cache = Locked(F<void *>(rec, kLodCache));
            int cs = StrideOf(F<void *>(rec, kLodCache));
            cache += size_t(F<uint32_t>(rec, kLodCacheFirst)) * cs;
            std::vector<float> theirs(size_t(verts) * 6);
            double t0 = NowMs();
            oUpdateLod(mesh, nullptr, lod);
            double tLs3d = NowMs() - t0;
            for (uint32_t i = 0; i < verts; i++) memcpy(&theirs[i * 6], cache + size_t(i) * cs, 24);
            t0 = NowMs();
            bool done = SkinLod(mesh, v, lod);
            double tOurs = NowMs() - t0;
            if (!done) { *reply = "skin.check: not skinned: " + (v.error.empty() ? std::string("(see skin.state)") : v.error); return true; }
            float dp = 0, dn = 0;
            uint32_t worst = 0;
            for (uint32_t i = 0; i < verts; i++) {
                const float *o = reinterpret_cast<const float *>(cache + size_t(i) * cs), *t = &theirs[i * 6];
                float a = std::max({std::fabs(o[0] - t[0]), std::fabs(o[1] - t[1]), std::fabs(o[2] - t[2])});
                float b = std::max({std::fabs(o[3] - t[3]), std::fabs(o[4] - t[4]), std::fabs(o[5] - t[5])});
                if (a > dp) dp = a, worst = i;
                dn = std::max(dn, b);
            }
            *reply = dump({{"vertices", verts}, {"most_position_m", dp}, {"most_normal", dn}, {"worst_vertex", worst},
                           {"ms_ls3d", tLs3d}, {"ms_ours", tOurs}});
        } else if (method == "skin.reload") {
            unsigned n = 0;
            DropDefs([&](const Def &d) { return !d.mod && ++n; });
            g_files.clear();
            g_vis.clear();
            *reply = dump({{"forgotten", n}});
        } else {
            *reply = "unknown skin method (skin.state, skin.export, skin.set, skin.reload)";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
