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
#include "d3dmirror.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "shaders/VsSkin.h"
#include "shaders/PsSkin.h"
#include "shaders/VsSkinDepth.h"
#include "shaders/VsSkinMotion.h"

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

// ---- a LOD of a single mesh skinned on the GPU (skin_gpu): the influences it uses (a mod's skin, or LS3D's own read
// from its bone records), the bones' palette now and as last drawn, the rest pose (the morph's: a face's expression),
// the lights LightVertices chose for it, the Direct3D 9 objects it is drawn from
template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
struct GpuLod {
    bool ok = false;
    std::string why;                   // (not on the GPU: why)
    std::shared_ptr<Lod> own;          // LS3D's own skin as influences (a mesh without a mod's)
    const Lod *L = nullptr;            // what it uses: the mod's LOD or own
    std::vector<void *> joints;        // a bone each (null: the mesh's own space)
    uint32_t verts = 0;
    std::vector<float> pal, palDrawn;  // 24 floats a bone: the position's matrix's columns, then the normal's
    unsigned palVersion = 0, drawnFrame = 0;
    int row = -1;                      // its row of the palettes' atlas (PaletteAtlas)
    std::vector<float> rest;           // 6 floats a vertex: the rest pose's position and normal
    unsigned restVersion = 0, restUploaded = 0;
    float light[36 * 4] = {};          // c120..c155 (skin.hlsli)
    bool lit = false;
    int env = -1;                      // the environment map's mode (EnvMapCoords), -1 none
    float envScale = 1, envU = 0, envV = 0;
    IDirect3DDevice9 *dev = nullptr;
    IDirect3DVertexBuffer9 *restVb = nullptr;
    void *attrKeyMesh = nullptr;       // (the attributes' buffer is the mesh's, shared: g_attrs)
    ~GpuLod();
};

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
    std::vector<std::unique_ptr<GpuLod>> gpu;   // a LOD each, skinned on the GPU (skin_gpu)
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

bool HasSkin(const Vis &v) { return v.def && v.error.empty(); }

// The mesh tracked, its skin (a mod's) bound to its joints when it has one (HasSkin).
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
    if (v.looked == g_defsGen) return &v;   // (LS3D's own models: this alone a call)
    v.looked = g_defsGen;
    if (v.model.empty()) return &v;
    ReadFileOf(v.model);
    v.looked = g_defsGen;   // (a file just read added its skins)
    auto d = g_defs.find(v.model + "|" + Lower(v.name));
    if (d == g_defs.end() || d->second.empty()) { v.def.reset(); v.gpu.clear(); return &v; }
    const std::shared_ptr<Def> &use = d->second.back();
    if (v.def != use || v.gen != use->gen) {   // (bound again: a new skin)
        v.def = use;
        v.gen = v.def->gen;
        v.error.clear();
        v.joints.clear();
        v.gpu.clear();   // (the GPU's data made again from the new skin)
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
    return &v;
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

// ---- people skinned on the GPU (skin_gpu): every single mesh's LOD that can be, while a mod (or the bridge) asks
std::vector<MmlMod *> g_gpuAsks;
bool g_gpuBridge;
unsigned g_frame = 1;          // SkinFrameBegin's count (the frames of the mission's world)
unsigned g_gpuSkins, g_gpuLights, g_gpuDraws, g_gpuDrawsLast, g_gpuFallback;
double g_gpuMs, g_gpuMsLast;   // the CPU's part: palettes, lights, uploads
std::string g_gpuWhy;          // (off: why, once the device was looked at)

bool GpuWanted() {
    if (g_gpuBridge) return true;
    for (MmlMod *m : g_gpuAsks) if (ModAlive(m)) return true;
    return false;
}
constexpr uint32_t kSingleMorph = 0x524D4D53;   // 'SMMR' at the visual's +0x1D4 (a single mesh's 'SNGM')
constexpr int kVisualType = 0x1D4, kSmMorph = 0x22C, kSmFlat = 0x80;
constexpr int kLevelGroups = 0x1C, kLevelFlags = 0x3C, kLevelEnvScale = 0x40, kLodEnvU = 0x24, kLodEnvV = 0x28;
constexpr int kLodDrawFirst = 0x18;

// LS3D's own skin of a mesh's LOD as influences, read from its bone records (the joints by their place in +0x220)
std::shared_ptr<Lod> OwnLod(void *mesh, int lod, std::vector<void *> *joints, std::string *why) {
    void *rec = F<void *>(mesh, kSmLods + 4 * lod), *level = F<void *>(F<void *>(mesh, kSmMesh), kMeshLevels + 4 * lod);
    void *skin = F<void *>(rec, kLodSkin);
    if (!skin) { *why = "no skin"; return nullptr; }
    uint32_t verts = F<uint32_t>(level, kLevelVerts), rigid = F<uint32_t>(skin, kSkinRigid);
    int bones = F<int>(mesh, kSmBoneCount);
    uint8_t *records = F<uint8_t *>(skin, kSkinRecords);
    void **frames = F<void **>(mesh, kSmBones);
    if (bones < 0 || bones > 255 || !records || !frames) { *why = "no bones bound"; return nullptr; }
    auto L = std::make_shared<Lod>();
    L->vertices = verts;
    Bone root{};
    memcpy(root.bind, kIdentity, sizeof root.bind);
    L->bones.push_back(root);
    joints->assign(1, nullptr);
    for (int b = 0; b < bones; b++) {
        Bone bo{};
        memcpy(bo.bind, records + b * kRecSize, 64);
        L->bones.push_back(bo);
        joints->push_back(frames[b]);
    }
    L->n.assign(verts, 0);
    L->idx.assign(size_t(verts) * kMaxInfluences, 0);
    L->w.assign(size_t(verts) * kMaxInfluences, 0.0f);
    uint32_t v = 0;
    for (int b = 0; b < bones; b++) {
        const uint8_t *r = records + b * kRecSize;
        uint32_t one = *reinterpret_cast<const uint32_t *>(r + 0x40), blended = *reinterpret_cast<const uint32_t *>(r + 0x44);
        uint32_t other = *reinterpret_cast<const uint32_t *>(r + 0x48);
        const float *weights = *reinterpret_cast<float *const *>(r + 0x64);
        if (v + one + blended > verts || other > static_cast<uint32_t>(bones)) { *why = "the bone records do not fit the vertices"; return nullptr; }
        for (uint32_t k = 0; k < one; k++, v++) L->n[v] = 1, L->idx[v * kMaxInfluences] = static_cast<uint16_t>(b + 1), L->w[v * kMaxInfluences] = 1;
        for (uint32_t k = 0; k < blended; k++, v++) {
            float w = weights ? weights[k] : 1.0f;
            L->idx[v * kMaxInfluences] = static_cast<uint16_t>(b + 1), L->w[v * kMaxInfluences] = w;
            L->idx[v * kMaxInfluences + 1] = static_cast<uint16_t>(other), L->w[v * kMaxInfluences + 1] = 1.0f - w;
            L->n[v] = w < 1.0f ? 2 : 1;
        }
    }
    if (v + rigid != verts) { *why = "the bone records cover " + std::to_string(v) + " of " + std::to_string(verts - rigid) + " vertices"; return nullptr; }
    for (; v < verts; v++) L->n[v] = 1, L->idx[v * kMaxInfluences] = 0, L->w[v * kMaxInfluences] = 1;   // (the rigid ones: the mesh's space)
    return L;
}

// Whether this LOD can be drawn by the GPU: what it would need of LS3D's CPU path that the vertex shader does not do.
GpuLod *GpuFor(void *mesh, Vis &v, int lod) {
    if (lod < 0 || lod >= kMaxLods) return nullptr;
    if (v.gpu.size() <= static_cast<size_t>(lod)) v.gpu.resize(lod + 1);
    if (v.gpu[lod]) return v.gpu[lod]->ok ? v.gpu[lod].get() : nullptr;
    v.gpu[lod].reset(new GpuLod);
    GpuLod &g = *v.gpu[lod];
    void *rec = F<void *>(mesh, kSmLods + 4 * lod), *data = F<void *>(mesh, kSmMesh);
    void *level = data ? F<void *>(data, kMeshLevels + 4 * lod) : nullptr;
    if (!rec || !level) { g.why = "no LOD"; return nullptr; }
    if (!(*reinterpret_cast<uint32_t *>(ModuleBase("LS3DF.dll") + game::Ls3d().vertexPath) & 1)) { g.why = "LS3D transforms through Direct3D here"; return nullptr; }
    if (F<uint32_t>(mesh, kSmFlags) & kSmFlat) { g.why = "lit flat (one colour a LOD)"; return nullptr; }
    uint32_t flags = F<uint32_t>(level, kLevelFlags);
    if (flags & 8) {
        g.env = static_cast<int>(flags & 7);
        if (g.env > 1) { g.why = "an environment map of mode " + std::to_string(g.env); return nullptr; }
        g.envScale = F<float>(level, kLevelEnvScale);
        g.envU = F<float>(rec, kLodEnvU);
        g.envV = F<float>(rec, kLodEnvV);
    }
    g.verts = F<uint32_t>(level, kLevelVerts);
    if (HasSkin(v) && lod < static_cast<int>(v.def->lods.size())) {
        g.L = &v.def->lods[lod];
        g.joints = v.joints[lod];
        if (g.L->vertices != g.verts) { g.why = "the mod's skin has another count of vertices"; return nullptr; }
        void *skin = F<void *>(rec, kLodSkin);
        uint32_t firstRigid = g.verts - std::min(skin ? F<uint32_t>(skin, kSkinRigid) : 0u, g.verts);
        for (uint32_t i = 0; i < firstRigid; i++)
            if (!g.L->n[i]) { g.why = "the mod's skin leaves vertex " + std::to_string(i) + " without a bone"; return nullptr; }
    } else {
        g.own = OwnLod(mesh, lod, &g.joints, &g.why);
        if (!g.own) return nullptr;
        g.L = g.own.get();
    }
    g.ok = true;
    return &g;
}

// The bones' palette now: each bone's matrix (its record's at rest times the joint's in the mesh's space) as three
// columns, then its normals' (SetRot) as three.
void Palette(GpuLod &g) {
    const Lod &L = *g.L;
    g.pal.resize(L.bones.size() * 24);
    for (size_t b = 0; b < L.bones.size(); b++) {
        M44 s, r;
        if (!g.joints[b]) memcpy(s.m, kIdentity, 64), memcpy(r.m, kIdentity, 64);
        else s = Mul(L.bones[b].bind, JointMatrix(g.joints[b])), r = SetRot(s);
        float *o = &g.pal[b * 24];
        for (int j = 0; j < 3; j++) {
            o[j * 4 + 0] = s.m[0 * 4 + j], o[j * 4 + 1] = s.m[1 * 4 + j], o[j * 4 + 2] = s.m[2 * 4 + j], o[j * 4 + 3] = s.m[3 * 4 + j];
            o[12 + j * 4 + 0] = r.m[0 * 4 + j], o[12 + j * 4 + 1] = r.m[1 * 4 + j], o[12 + j * 4 + 2] = r.m[2 * 4 + j], o[12 + j * 4 + 3] = 0;
        }
    }
    g.palVersion++;
}

// The LOD's rest pose from the mesh's own vertices (a single morph's after its morph: they move with the face)
bool RestPose(void *mesh, GpuLod &g, int lod) {
    void *level = F<void *>(F<void *>(mesh, kSmMesh), kMeshLevels + 4 * lod);
    uint8_t *src = Locked(F<void *>(level, kLevelVb));
    if (!src) return false;
    int ss = StrideOf(F<void *>(level, kLevelVb));
    src += size_t(F<uint32_t>(level, kLevelFirst)) * ss;
    g.rest.resize(size_t(g.verts) * 6);
    for (uint32_t i = 0; i < g.verts; i++) memcpy(&g.rest[i * 6], src + size_t(i) * ss, 24);
    g.restVersion++;
    return true;
}

// PoseLod for a LOD the GPU skins: what LS3D marks, the palette, the rest pose when the face moved. False: not.
bool GpuUpdateLod(void *mesh, Vis &v, int lod) {
    GpuLod *g = GpuFor(mesh, v, lod);
    if (!g) return false;
    void *rec = F<void *>(mesh, kSmLods + 4 * lod);
    if (!F<void *>(rec, kLodCache) || !F<void *>(mesh, kSmBones)) return false;   // (LS3D's first call makes them)
    F<int>(rec, kLodLight) = -700;
    F<int>(rec, kLodLight2) = -600;
    F<uint32_t>(rec, kLodStamp) = F<uint32_t>(mesh, kSmStamp);
    Palette(*g);
    bool morph = F<uint32_t>(mesh, kVisualType) == kSingleMorph && (F<uint8_t>(mesh, kSmMorph) & 1);
    if ((morph || g->rest.empty()) && !RestPose(mesh, *g, lod)) return false;
    return true;
}

// The GPU's LOD of a drawn mesh (an instance's: its mesh's), null when it is not drawn by the GPU
GpuLod *GpuOfLod(void *mesh, int lod) {
    for (int i = 0; i < 8 && (F<uint32_t>(mesh, kSmFlags) & kSmInstance); i++) mesh = F<void *>(mesh, 0x08);
    auto it = g_vis.find(mesh);
    if (it == g_vis.end() || lod < 0 || static_cast<size_t>(lod) >= it->second.gpu.size() || !it->second.gpu[lod]) return nullptr;
    return it->second.gpu[lod]->ok ? it->second.gpu[lod].get() : nullptr;
}

// LS3D's own functions called from the hooks, timed for the frame's record (ProfHotScope: no __try beside it)
void LodGame(void *mesh, void *edx, int lod) {
    ProfHotScope t(kHotSkinLodGame);
    oUpdateLod(mesh, edx, lod);
}

void UpdateLodHooked(void *mesh, void *edx, int lod) {
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
    if (!v) { LodGame(mesh, edx, lod); return; }
    const bool gpu = GpuWanted() && g_gpuWhy.empty();
    if (!gpu && !HasSkin(*v)) { LodGame(mesh, edx, lod); return; }
    double t0 = NowMs();
    bool done = false, onGpu = false;
    __try {
        void *rec = lod >= 0 && lod < kMaxLods ? F<void *>(mesh, kSmLods + 4 * lod) : nullptr;
        // (the first time LS3D binds the joints and makes the cache: its own PoseLod does it)
        if (rec && (!F<void *>(mesh, kSmBones) || !F<void *>(rec, kLodCache))) LodGame(mesh, edx, lod);
        if (gpu) done = onGpu = GpuUpdateLod(mesh, *v, lod);
        if (!done && HasSkin(*v)) done = SkinLod(mesh, *v, lod);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        done = false;
    }
    if (!done) {
        if (gpu) g_gpuFallback++;
        LodGame(mesh, edx, lod);
        return;
    }
    v->skinned++;
    if (onGpu) g_gpuSkins++, g_gpuMs += NowMs() - t0;
    else g_ours++, g_ms += NowMs() - t0;
}
void __fastcall hkUpdateLod(void *mesh, void *edx, int lod) {
    ProfHotScope t(kHotSkinLod);
    UpdateLodHooked(mesh, edx, lod);
}

// The rigid vertices' positions into the drawn buffer: from the cache for a mesh whose skin moves them too; none for a
// LOD the GPU draws (its drawn buffer is not drawn).
void *VbPos0Game(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    ProfHotScope t(kHotSkinCopyGame);
    return oVbPos0(mesh, edx, dest, stride, lod);
}
void *VbPos0Hooked(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    bool gpu = false;
    __try { gpu = !g_vis.empty() && GpuOfLod(mesh, lod); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    if (gpu) return nullptr;
    void *r = VbPos0Game(mesh, edx, dest, stride, lod);
    __try {
        void *m = mesh;
        for (int i = 0; i < 8 && (F<uint32_t>(m, kSmFlags) & kSmInstance); i++) m = F<void *>(m, 0x08);
        auto it = g_vis.find(m);
        if (it == g_vis.end() || !HasSkin(it->second) || lod < 0 || lod >= static_cast<int>(it->second.def->lods.size())) return r;
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
void *__fastcall hkVbPos0(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    ProfHotScope t(kHotSkinCopy);
    return VbPos0Hooked(mesh, edx, dest, stride, lod);
}

// The skinned vertices' positions into the drawn buffer (CopySkinnedPositions: motion.cpp keeps them too): none for a LOD the
// GPU draws.
VbPosFn oVbPos;
bool GpuDrawsLod(void *mesh, int lod) {
    __try { return !g_vis.empty() && GpuOfLod(mesh, lod); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
}
void *__fastcall hkVbPos(void *mesh, void *edx, uint8_t *dest, int stride, int lod) {
    ProfHotScope t(kHotSkinCopy);
    if (GpuDrawsLod(mesh, lod)) return nullptr;
    ProfHotScope g(kHotSkinCopyGame);
    return oVbPos(mesh, edx, dest, stride, lod);
}

// LightVertices for a LOD the GPU draws: LS3D's own choice of the lights, run on one vertex of our own; what it chose
// (its globals) becomes the vertex shader's constants. LS3D lights again only when the bones or the sector's lights
// changed, and so do we.
using LightFn = uint32_t(__fastcall *)(void *dest, int destStride, void *level, void *posNorm, int stride, void *visual,
                                       void *invWorld, uint32_t mask, uint32_t changed, void *lodRec);
LightFn oLighting;
void CaptureLights(GpuLod &g) {
    const game::Ls3dBuild &l = game::Ls3d();
    const uintptr_t base = ModuleBase("LS3DF.dll");
    const float *amb = reinterpret_cast<const float *>(base + l.lightAmbient);
    int count = std::min(8, *reinterpret_cast<const int *>(base + l.lightCount));
    float *c = g.light;
    memset(c, 0, sizeof g.light);
    c[0] = amb[0], c[1] = amb[1], c[2] = amb[2], c[3] = static_cast<float>(count);   // c120
    for (int i = 0; i < count; i++) {
        const float *s = reinterpret_cast<const float *>(base + l.lightSlots + i * 0x50);
        uint8_t *light = *reinterpret_cast<uint8_t *const *>(s + 0x10);
        float *o = c + (124 - 120) * 4 + i * 16;
        int type = light ? *reinterpret_cast<const uint8_t *>(light + 0x154) : 1;
        o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = static_cast<float>(type);
        o[4] = s[4], o[5] = s[5], o[6] = s[6], o[7] = s[0xC];
        o[8] = s[8], o[9] = s[9], o[10] = s[10], o[11] = s[0xD];
        o[12] = s[0xF];
        if (light && type == 2) o[13] = *reinterpret_cast<float *>(light + 0x1A0), o[14] = *reinterpret_cast<float *>(light + 0x1B0),
                                 o[15] = *reinterpret_cast<float *>(light + 0x1C0);
    }
    g.lit = true;
}
uint32_t LightingGame(void *dest, int destStride, void *level, void *posNorm, int stride, void *visual, void *invWorld,
                      uint32_t mask, uint32_t changed, void *lodRec) {
    ProfHotScope t(kHotSkinLightGame);
    return oLighting(dest, destStride, level, posNorm, stride, visual, invWorld, mask, changed, lodRec);
}
uint32_t LightingHooked(void *dest, int destStride, void *level, void *posNorm, int stride, void *visual, void *invWorld,
                        uint32_t mask, uint32_t changed, void *lodRec) {
    GpuLod *g = nullptr;
    int lod = -1;
    __try {
        if (!g_vis.empty() && visual && lodRec)
            for (int i = 0; i < kMaxLods && !g; i++)
                if (F<void *>(visual, kSmLods + 4 * i) == lodRec) lod = i, g = GpuOfLod(visual, i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        g = nullptr;
    }
    if (!g) return LightingGame(dest, destStride, level, posNorm, stride, visual, invWorld, mask, changed, lodRec);
    // one vertex: a copy of the LOD's level with a count of 1, a vertex at the origin with no normal
    double t0 = NowMs();
    uint8_t fake[0x80];
    uint32_t color[2] = {0xA5A5A5A5u, 0};
    float pn[8] = {};
    uint32_t r = 0;
    __try {
        memcpy(fake, level, sizeof fake);
        *reinterpret_cast<uint32_t *>(fake + kLevelVerts) = 1;
        r = LightingGame(color, 4, fake, pn, 0x18, visual, invWorld, mask, changed, lodRec);
        if (color[0] != 0xA5A5A5A5u) CaptureLights(*g), g_gpuLights++;   // (lit again: else what it chose before stands)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
    }
    g_gpuMs += NowMs() - t0;
    return r;
}
uint32_t __fastcall hkLighting(void *dest, int destStride, void *level, void *posNorm, int stride, void *visual, void *invWorld,
                               uint32_t mask, uint32_t changed, void *lodRec) {
    ProfHotScope t(kHotSkinLight);
    ProfHotScope byClass(g_profVisual >= 0 ? kHotLighting + g_profVisual : kHotSkinLight);   // (the visual's class: prof.cpp)
    if (g_profVisual < 0) byClass.t0 = 0;   // (no class known: counted once, as skin.lighting)
    return LightingHooked(dest, destStride, level, posNorm, stride, visual, invWorld, mask, changed, lodRec);
}

bool g_hookTried, g_hooked;
std::string g_hookWhy;
bool EnsureHooks() {
    if (g_hookTried) return g_hooked;
    if (!game::g_ls3d) { g_hookWhy = "no game known"; return false; }
    g_hookTried = true;
    const game::Ls3dBuild &l = game::Ls3d();
    if (!l.smUpdateLod || !l.smUpdateVBPos0 || !l.smCopySkinned || !l.frameUpdateMatrix || !l.lightingProcess) {
        g_hookWhy = "not known for this LS3DF.dll";
        return false;
    }
    uintptr_t base = ModuleBase("LS3DF.dll");
    struct { uintptr_t at; void *detour; void **orig; const char *name; } hooks[] = {
        {base + l.smUpdateLod, reinterpret_cast<void *>(&hkUpdateLod), reinterpret_cast<void **>(&oUpdateLod), "PoseLod"},
        {base + l.smUpdateVBPos0, reinterpret_cast<void *>(&hkVbPos0), reinterpret_cast<void **>(&oVbPos0), "CopyRigidPositions"},
        {base + l.smCopySkinned, reinterpret_cast<void *>(&hkVbPos), reinterpret_cast<void **>(&oVbPos), "CopySkinnedPositions"},
        {base + l.lightingProcess, reinterpret_cast<void *>(&hkLighting), reinterpret_cast<void **>(&oLighting), "LightVertices"},
    };
    std::string err;
    for (auto &h : hooks) {
        if (HookAdd(nullptr, h.at, h.detour, h.orig, &err)) continue;
        for (auto &u : hooks) if (*u.orig) HookRemoveLink(nullptr, u.at, u.detour);
        g_hookWhy = std::string(h.name) + " not hooked: " + err;
        return false;
    }
    g_hooked = true;
    HLOG("skin: I3D_singlemesh::PoseLod, CopyRigidPositions, CopySkinnedPositions and LightVertices hooked (a mod's skins, the GPU's)");
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

// ---- the GPU's drawing: the device's objects (the vertex layout, the shaders, a LOD's buffers and palettes)
IDirect3DDevice9 *g_shDev;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DVertexShader9 *g_vsSkin, *g_vsDepth, *g_vsMotion;
IDirect3DPixelShader9 *g_psSkin;
bool g_gpuChecked;

// The mesh's attributes that do not move (uv, tangent, bones, weights: 72 bytes a vertex), shared by its instances
struct Attrs { IDirect3DDevice9 *dev = nullptr; IDirect3DVertexBuffer9 *vb = nullptr; uint32_t verts = 0; unsigned frame = 0; };
std::map<std::tuple<const void *, int, const void *>, Attrs> g_attrs;
std::map<std::string, std::vector<ModelSkinLod>> g_rests;   // the .4ds's LODs (their triangles), by model|mesh
constexpr UINT kRestStride = 24, kAttrStride = 72;

void AtlasRelease();
void ReleaseDevice() {
    SafeRelease(g_decl); SafeRelease(g_vsSkin); SafeRelease(g_vsDepth); SafeRelease(g_vsMotion); SafeRelease(g_psSkin);
    for (auto &kv : g_attrs) SafeRelease(kv.second.vb);
    g_attrs.clear();
    for (auto &kv : g_vis) for (auto &g : kv.second.gpu) if (g) { SafeRelease(g->restVb); g->dev = nullptr; }
    AtlasRelease();
    g_shDev = nullptr;
}

bool MakeDevice(IDirect3DDevice9 *d) {
    if (g_shDev == d && g_decl) return true;
    ReleaseDevice();
    const D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1, 8, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {1, 24, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {1, 32, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 1},
        {1, 40, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {1, 56, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 1},
        D3DDECL_END()};
    if (FAILED(d->CreateVertexDeclaration(el, &g_decl)) ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsSkin), &g_vsSkin)) ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsSkinDepth), &g_vsDepth)) ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsSkinMotion), &g_vsMotion)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsSkin), &g_psSkin))) {
        ReleaseDevice();
        return false;
    }
    g_shDev = d;
    return true;
}

// Whether the device takes what the GPU's skinning needs: shader model 3 and float textures in the vertex shader.
std::string CheckDevice(IDirect3DDevice9 *d) {
    D3DCAPS9 caps{};
    if (FAILED(d->GetDeviceCaps(&caps)) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
        return "the device has no shader model 3";
    IDirect3D9 *d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    if (FAILED(d->GetDirect3D(&d3d)) || !d3d) return "no Direct3D 9 interface";
    d->GetCreationParameters(&cp);
    d->GetDisplayMode(0, &mode);
    HRESULT hr = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format ? mode.Format : D3DFMT_X8R8G8B8,
                                        D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F);
    d3d->Release();
    if (FAILED(hr)) return "the device reads no float texture in a vertex shader";
    if (!MakeDevice(d)) return "the device refused the skinning's shaders";
    return "";
}

// The attributes' buffer of a LOD: the uvs from the mesh's own vertices, the tangents from MikkTSpace, the bones.
Attrs *AttrsFor(IDirect3DDevice9 *d, void *mesh, const Vis &v, GpuLod &g, int lod) {
    void *data = F<void *>(mesh, kSmMesh);
    auto key = std::make_tuple(static_cast<const void *>(data), lod, static_cast<const void *>(g.own ? nullptr : g.L));
    Attrs &a = g_attrs[key];
    if (a.vb && a.dev == d && a.verts == g.verts) return &a;
    SafeRelease(a.vb);
    a.dev = d;
    a.verts = g.verts;
    void *level = F<void *>(data, kMeshLevels + 4 * lod);
    uint8_t *src = Locked(F<void *>(level, kLevelVb));
    if (!src || g.rest.size() < size_t(g.verts) * 6) return nullptr;
    int ss = StrideOf(F<void *>(level, kLevelVb));
    src += size_t(F<uint32_t>(level, kLevelFirst)) * ss;
    std::vector<float> uv(size_t(g.verts) * 2), tan(size_t(g.verts) * 4, 0.0f);
    for (uint32_t i = 0; i < g.verts; i++) memcpy(&uv[i * 2], src + size_t(i) * ss + 24, 8);
    // the triangles: the .4ds's (the drawn index buffers hold the same, split by material)
    std::string rk = v.model + "|" + Lower(v.name);
    auto it = g_rests.find(rk);
    if (it == g_rests.end()) {
        std::vector<ModelSkinLod> lods;
        std::string name, err;
        if (!ModelSkin(v.model.c_str(), v.name.c_str(), &lods, &name, &err)) HWARN("skin: %s %s: no triangles for the tangents (%s)", v.model.c_str(), v.name.c_str(), err.c_str());
        it = g_rests.emplace(rk, std::move(lods)).first;
    }
    if (lod < static_cast<int>(it->second.size()) && it->second[lod].vertices == g.verts && !it->second[lod].idx.empty()) {
        const ModelSkinLod &r = it->second[lod];
        std::vector<uint32_t> idx(r.idx.begin(), r.idx.end());
        MikkTangents(g.verts, g.rest.data(), 6, g.rest.data() + 3, 6, uv.data(), 2, idx.data(), static_cast<uint32_t>(idx.size() / 3), tan.data());
    } else {
        for (uint32_t i = 0; i < g.verts; i++) tan[i * 4] = 1, tan[i * 4 + 3] = 1;
    }
    if (FAILED(d->CreateVertexBuffer(g.verts * kAttrStride, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &a.vb, nullptr))) return nullptr;
    uint8_t *p = nullptr;
    if (FAILED(a.vb->Lock(0, 0, reinterpret_cast<void **>(&p), 0)) || !p) { SafeRelease(a.vb); return nullptr; }
    const Lod &L = *g.L;
    for (uint32_t i = 0; i < g.verts; i++) {
        uint8_t *o = p + size_t(i) * kAttrStride;
        const float *t = &tan[i * 4];
        int16_t b[8] = {};
        float w[8] = {};
        for (int k = 0; k < L.n[i] && k < kMaxInfluences; k++) b[k] = static_cast<int16_t>(L.idx[i * kMaxInfluences + k]), w[k] = L.w[i * kMaxInfluences + k];
        memcpy(o, &uv[i * 2], 8);
        memcpy(o + 8, t, 16);
        memcpy(o + 24, b, 16);
        memcpy(o + 40, w, 32);
    }
    a.vb->Unlock();
    return &a;
}

// The palettes and the rest pose into the LOD's textures and buffer (the palette as last drawn becomes "before" at
// the frame's first draw: the motion vectors')
// ---- the people's palettes, every one in one texture (31d): a row each (a GpuLod's `row`, kept while it lives), a
// texel's float4 as skin.hlsli reads it; the rows drawn this frame written into the CPU's copy and the texture filled
// with one lock (D3DLOCK_DISCARD) at the frame's first skinned draw, again only if a palette changes after it (a scene
// view's). With the motion vectors asked, a second one holds each row as it was drawn the frame before (a row not drawn
// then: the one now). A lock a person a frame cost 5 to 10 us on Direct3D 9Ex (1.0's crowd, 48 people).
struct Atlas {
    IDirect3DTexture9 *tex = nullptr;
    UINT w = 0, h = 0;                 // texels a row, rows
    std::vector<float> data;           // what it holds (w * h * 4 floats)
    bool dirty = true;
};
Atlas g_atlas[2];                      // [0] now, [1] the frame before
IDirect3DDevice9 *g_atlasDev;
unsigned g_atlasFrame;                 // the frame its rows were last gathered for
// (made once, never destroyed: GpuLods held by g_vis are destroyed after these at the process's exit)
struct Rows { std::vector<int> free; std::vector<GpuLod *> owner; };   // owner: row -> its GpuLod
Rows &RowsOf() { static Rows *r = new Rows; return *r; }
int g_rows;

GpuLod::~GpuLod() {
    SafeRelease(restVb);
    if (row >= 0) { Rows &r = RowsOf(); r.free.push_back(row); if (row < static_cast<int>(r.owner.size())) r.owner[row] = nullptr; }
}
void AtlasRelease() {
    for (Atlas &a : g_atlas) { SafeRelease(a.tex); a.w = a.h = 0; a.data.clear(); a.dirty = true; }
    g_atlasDev = nullptr;
}
bool AtlasMake(IDirect3DDevice9 *d, Atlas &a, UINT w, UINT h) {
    if (a.tex && a.w == w && a.h == h) return true;
    SafeRelease(a.tex);
    std::vector<float> old = std::move(a.data);
    const UINT ow = a.w, oh = a.h;
    a.data.assign(size_t(w) * h * 4, 0.0f);
    for (UINT y = 0; y < oh && y < h; y++)   // (what it held, kept: the rows are the same people's)
        memcpy(&a.data[size_t(y) * w * 4], &old[size_t(y) * ow * 4], size_t(std::min(ow, w)) * 16);
    a.w = w, a.h = h, a.dirty = true;
    return SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &a.tex, nullptr));
}
bool AtlasFill(Atlas &a) {
    if (!a.dirty) return true;
    D3DLOCKED_RECT r{};
    if (FAILED(a.tex->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) return false;
    for (UINT y = 0; y < a.h; y++) memcpy(static_cast<uint8_t *>(r.pBits) + size_t(y) * r.Pitch, &a.data[size_t(y) * a.w * 4], size_t(a.w) * 16);
    a.tex->UnlockRect(0);
    a.dirty = false;
    return true;
}
float *AtlasRow(Atlas &a, int row) { return &a.data[size_t(row) * a.w * 4]; }
// g's palette in the atlas as this frame's (and every other person's posed this frame: one lock for all of them)
bool AtlasFor(IDirect3DDevice9 *d, GpuLod &g) {
    if (g_atlasDev != d) { AtlasRelease(); g_atlasDev = d; }
    Rows &rows = RowsOf();
    if (g.row < 0) {
        if (!rows.free.empty()) { g.row = rows.free.back(); rows.free.pop_back(); }
        else g.row = g_rows++;
        if (g.row >= static_cast<int>(rows.owner.size())) rows.owner.resize(g.row + 1);
        rows.owner[g.row] = &g;
    }
    // the size: the widest palette, every row; grown by halves (a texture made again seldom)
    UINT w = g_atlas[0].w, h = g_atlas[0].h;
    const UINT need = static_cast<UINT>(g.pal.size() / 4);
    if (need > w) w = std::max(need, w + w / 2);
    if (static_cast<UINT>(g_rows) > h) h = std::max<UINT>(g_rows, std::max<UINT>(16, h + h / 2));
    const bool motion = MotionWanted();
    if (!AtlasMake(d, g_atlas[0], w, h) || (motion && !AtlasMake(d, g_atlas[1], w, h))) return false;
    if (g_atlasFrame != g_frame) {   // (the frame's first: every row posed is gathered now)
        if (motion) {
            for (GpuLod *o : rows.owner) {
                if (!o || o->palDrawn.empty() || o->palDrawn.size() / 4 > w) continue;
                const std::vector<float> &before = o->drawnFrame + 1 == g_frame && o->palDrawn.size() == o->pal.size() ? o->palDrawn : o->pal;
                float *dst = AtlasRow(g_atlas[1], o->row);
                if (memcmp(dst, before.data(), before.size() * 4)) { memcpy(dst, before.data(), before.size() * 4); g_atlas[1].dirty = true; }
            }
        }
        for (GpuLod *o : rows.owner) {   // (the palettes now: each person's posed this frame, whether drawn yet or not)
            if (!o || o->pal.empty() || o->pal.size() / 4 > w) continue;
            float *dst = AtlasRow(g_atlas[0], o->row);
            if (memcmp(dst, o->pal.data(), o->pal.size() * 4)) { memcpy(dst, o->pal.data(), o->pal.size() * 4); g_atlas[0].dirty = true; }
        }
        g_atlasFrame = g_frame;
    } else {   // (later in the frame: this one's row alone, posed again since: a scene view's)
        float *dst = AtlasRow(g_atlas[0], g.row);
        if (memcmp(dst, g.pal.data(), g.pal.size() * 4)) { memcpy(dst, g.pal.data(), g.pal.size() * 4); g_atlas[0].dirty = true; }
    }
    if (g.drawnFrame != g_frame) { g.palDrawn = g.pal; g.drawnFrame = g_frame; }
    else g.palDrawn = g.pal;
    return AtlasFill(g_atlas[0]) && (!motion || AtlasFill(g_atlas[1]));
}

bool Upload(IDirect3DDevice9 *d, GpuLod &g) {
    const UINT width = static_cast<UINT>(g.pal.size() / 4);
    if (g.dev != d) {
        SafeRelease(g.restVb);
        g.restUploaded = 0;
        g.dev = d;
    }
    (void)width;
    if (!g.restVb && FAILED(d->CreateVertexBuffer(g.verts * kRestStride, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g.restVb, nullptr)))
        return false;
    if (!AtlasFor(d, g)) return false;
    if (g.restUploaded != g.restVersion) {
        void *p = nullptr;
        if (FAILED(g.restVb->Lock(0, 0, &p, D3DLOCK_DISCARD)) || !p) return false;
        memcpy(p, g.rest.data(), g.rest.size() * sizeof(float));
        g.restVb->Unlock();
        g.restUploaded = g.restVersion;
    }
    return true;
}

} // namespace

bool SkinGpuWanted() { return GpuWanted() && g_gpuChecked && g_gpuWhy.empty(); }

double g_gpuDrawMs, g_gpuDrawMsLast;
long long SkinGpuTimeBegin() {
    if (!g_gpuChecked || !g_gpuWhy.empty()) return 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
void SkinGpuTimeEnd(long long t0) {
    if (!t0) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_gpuDrawMs += 1000.0 * (t.QuadPart - t0) / g_freq.QuadPart;
}

// (render.cpp, a frame of the mission's world begins) the frame's count; the device looked at while a mod asks
void SkinFrameBegin() {
    g_frame++;
    g_gpuDrawsLast = g_gpuDraws, g_gpuDraws = 0;
    g_gpuMsLast = g_gpuMs, g_gpuMs = 0;
    g_gpuDrawMsLast = g_gpuDrawMs, g_gpuDrawMs = 0;
    // the device's state followed while the GPU skins (d3dmirror.cpp: its draws ask the device nothing, their shaders
    // lent from one person to the next)
    static bool mirrored;
    if (SkinGpuWanted() != mirrored) { mirrored = !mirrored; MirrorUse(mirrored); }
    if (!GpuWanted() || g_gpuChecked) return;
    auto *d = static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if (!d) { g_gpuWhy = "no Direct3D 9 device (the GPU's skinning needs a wrapper's Direct3D 9: dxwrapper, DXVK)"; g_gpuChecked = true; return; }
    std::string why;
    g_gpuWhy = !EnsureHooks() ? g_hookWhy : !GeomEnsureHooks(&why) || !GeomEnsureDip(&why) ? why : CheckDevice(d);
    g_gpuChecked = true;
    if (g_gpuWhy.empty()) HLOG("skin: people skinned on the GPU from this frame");
    else HWARN("skin: the GPU's skinning is off: %s", g_gpuWhy.c_str());
}

// What a draw of a mesh skinned on the GPU binds (geom.cpp's visual, its draw's BaseVertexIndex: its LOD).
bool SkinGpuFor(void *dev9, const void *visual, int base, SkinGpuDraw *out) {
    if (!SkinGpuWanted() || !visual || g_vis.empty()) return false;
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    __try {
        void *mesh = const_cast<void *>(visual);
        for (int i = 0; i < 8 && (F<uint32_t>(mesh, kSmFlags) & kSmInstance); i++) mesh = F<void *>(mesh, 0x08);
        auto it = g_vis.find(mesh);
        if (it == g_vis.end()) return false;
        Vis &v = it->second;
        int lod = -1;
        for (size_t i = 0; i < v.gpu.size() && lod < 0; i++) {
            void *rec = F<void *>(mesh, kSmLods + 4 * static_cast<int>(i));
            if (v.gpu[i] && v.gpu[i]->ok && rec && F<int>(rec, kLodDrawFirst) == base) lod = static_cast<int>(i);
        }
        if (lod < 0) return false;
        GpuLod &g = *v.gpu[lod];
        if (g.pal.empty() || g.rest.empty() || !MakeDevice(d) || !Upload(d, g)) return false;
        Attrs *a = AttrsFor(d, mesh, v, g, lod);
        if (!a) return false;
        a->frame = g_frame;
        out->decl = g_decl;
        out->rest = g.restVb;
        out->attrs = a->vb;
        out->palette = g_atlas[0].tex;
        out->paletteBefore = g_atlas[1].tex ? g_atlas[1].tex : g_atlas[0].tex;
        out->restStride = kRestStride;
        out->attrStride = kAttrStride;
        memcpy(out->c, g.light, sizeof g.light);
        // c121: the vertices' alpha (LightVertices's: the first material's opacity times the fading), the
        // environment map's mode, the palette's width
        void *level = F<void *>(F<void *>(mesh, kSmMesh), kMeshLevels + 4 * lod);
        void *group = F<void *>(level, kLevelGroups);
        void *mat = group ? F<void *>(group, 4) : nullptr;
        float alpha = F<float>(mesh, 0x218) * F<float>(mesh, 0x214) * (mat ? F<float>(mat, 0x38) : 1.0f);
        alpha = static_cast<float>(static_cast<int>(alpha * 255.0f + 0.5f) & 0xFF) / 255.0f;
        float width = static_cast<float>(g_atlas[0].w);
        float *c = out->c;
        c[4] = alpha, c[5] = static_cast<float>(g.env), c[6] = width, c[7] = 1.0f / width;
        c[8] = g.envScale, c[9] = g.envU, c[10] = g.envV, c[11] = (g.row + 0.5f) / static_cast<float>(g_atlas[0].h);
        g_gpuDraws++;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        return false;
    }
}

bool SkinGpuDrawsLod(void *mesh, int lod) {
    if (g_vis.empty()) return false;
    __try { return GpuOfLod(mesh, lod) != nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void *SkinGpuShader(int which) {
    switch (which) {
    case 0: return g_vsSkin;
    case 1: return g_psSkin;
    case 2: return g_vsDepth;
    case 3: return g_vsMotion;
    }
    return nullptr;
}

// The skinned draw's streams, layout, palettes (vertex samplers 0, 1) and constants (c120..c155) set. Only what LS3D
// keeps a cache of is put back after (SkinGpuUnbind: stream 0, the FVF): the vertex samplers, their textures and stream 1
// are no one else's (the game's fixed pipeline reads none of them), so they stay as set, the samplers' point filtering
// and clamping set once a frame. There are hundreds of these draws a frame: each call to the device counts.
unsigned g_samplersFrame;
void SkinGpuBind(void *dev9, const SkinGpuDraw &s, SkinGpuSaved *k) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    IDirect3DVertexBuffer9 *vb = nullptr;
    d->GetStreamSource(0, &vb, &k->off[0], &k->stride[0]);
    k->vb[0] = vb;
    k->decl = nullptr;
    d->GetFVF(&k->fvf);
    if (!k->fvf) {
        IDirect3DVertexDeclaration9 *decl = nullptr;
        d->GetVertexDeclaration(&decl);
        k->decl = decl;
    }
    if (g_samplersFrame != g_frame) {
        g_samplersFrame = g_frame;
        for (UINT i = 0; i < 2; i++) {
            d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
    }
    d->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9 *>(s.decl));
    d->SetStreamSource(0, static_cast<IDirect3DVertexBuffer9 *>(s.rest), 0, s.restStride);
    d->SetStreamSource(1, static_cast<IDirect3DVertexBuffer9 *>(s.attrs), 0, s.attrStride);
    d->SetTexture(D3DVERTEXTEXTURESAMPLER0, static_cast<IDirect3DBaseTexture9 *>(s.palette));
    d->SetTexture(D3DVERTEXTEXTURESAMPLER1, static_cast<IDirect3DBaseTexture9 *>(s.paletteBefore));
    MirrorConstants(d, false, 120, s.c, 36);   // (only what differs from the last person's: d3dmirror.cpp)
}
void SkinGpuUnbind(void *dev9, SkinGpuSaved &k) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev9);
    d->SetStreamSource(0, static_cast<IDirect3DVertexBuffer9 *>(k.vb[0]), k.off[0], k.stride[0]);
    if (k.vb[0]) static_cast<IDirect3DVertexBuffer9 *>(k.vb[0])->Release();
    if (k.fvf) d->SetFVF(k.fvf);
    else d->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9 *>(k.decl));
    if (k.decl) static_cast<IDirect3DVertexDeclaration9 *>(k.decl)->Release();
}

// skin_gpu: the mod wants every person skinned on the GPU (on) or no more; 1, 0 when the device cannot.
int SkinGpuAsk(MmlMod *mod, bool on) {
    g_gpuAsks.erase(std::remove(g_gpuAsks.begin(), g_gpuAsks.end(), mod), g_gpuAsks.end());
    if (on) g_gpuAsks.push_back(mod);
    SkinGpuChanged();
    return !g_gpuChecked || g_gpuWhy.empty() ? 1 : 0;
}

// The GPU's skinning switched: every LOD skinned and lit again by whoever does it now (the stamps made stale).
void SkinGpuChanged() {
    const bool want = GpuWanted();
    if (!want) g_gpuChecked = false, g_gpuWhy.clear();
    if (want) EnsureHooks();
    for (auto &kv : g_vis) {
        __try {
            for (int lod = 0; lod < kMaxLods; lod++) {
                void *rec = F<void *>(kv.first, kSmLods + 4 * lod);
                if (!rec) continue;
                F<uint32_t>(rec, kLodStamp) = 0xFFFFFFFFu;
                F<int>(rec, kLodLight) = -700;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults++;
        }
        if (!want) kv.second.gpu.clear();
    }
}

namespace {
} // namespace

void SkinMissionClose() {
    g_vis.clear();
    for (auto &kv : g_attrs) SafeRelease(kv.second.vb);
    g_attrs.clear();
    g_rests.clear();
}

void SkinModGone(MmlMod *mod) {
    if (mod) DropDefs([&](const Def &d) { return d.mod == mod; });
    if (std::find(g_gpuAsks.begin(), g_gpuAsks.end(), mod) != g_gpuAsks.end()) SkinGpuAsk(mod, false);
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
            unsigned onGpu = 0, offGpu = 0;
            std::map<std::string, unsigned> whyNot;
            for (auto &kv : g_vis)
                for (auto &g : kv.second.gpu)
                    if (g && g->ok) onGpu++;
                    else if (g) offGpu++, whyNot[g->why]++;
            // the palettes drawn now and before differ by this much at most (a moving person's limbs: not 0)
            float most = 0;
            if (g_atlas[1].data.size() == g_atlas[0].data.size())
                for (size_t i = 0; i < g_atlas[0].data.size(); i++) most = std::max(most, std::fabs(g_atlas[0].data[i] - g_atlas[1].data[i]));
            json gpu = {{"palette_motion_most", most}, {"frame", g_frame}, {"wanted", GpuWanted()}, {"on", SkinGpuWanted()}, {"why", g_gpuWhy}, {"lods", onGpu}, {"lods_cpu", offGpu},
                        {"why_cpu", whyNot}, {"skinned", g_gpuSkins}, {"lit", g_gpuLights}, {"fallbacks", g_gpuFallback},
                        {"draws_last_frame", g_gpuDrawsLast}, {"ms_last_frame", g_gpuMsLast}, {"draw_ms_last_frame", g_gpuDrawMsLast},
                        {"meshes_attrs", g_attrs.size()}};
            std::string asked;
            for (MmlMod *m : g_gpuAsks) if (ModAlive(m)) asked += std::string(asked.empty() ? "" : ", ") + ModIdOf(m);
            if (g_gpuBridge) asked += std::string(asked.empty() ? "" : ", ") + "bridge";
            gpu["asked_by"] = asked;
            *reply = dump({{"hooked", g_hooked ? json("on") : json(g_hookWhy)}, {"gpu", gpu}, {"skins", defs}, {"files", files}, {"meshes", vis},
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
        } else if (method == "skin.gpu") {
            g_gpuBridge = p.value("on", true);
            SkinGpuChanged();
            *reply = dump({{"wanted", GpuWanted()}, {"note", "from the next frame; skin.state's gpu tells whether the device takes it"}});
        } else if (method == "skin.reload") {
            unsigned n = 0;
            DropDefs([&](const Def &d) { return !d.mod && ++n; });
            g_files.clear();
            g_vis.clear();
            *reply = dump({{"forgotten", n}});
        } else {
            *reply = "unknown skin method (skin.state, skin.export, skin.set, skin.check, skin.gpu, skin.reload)";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
