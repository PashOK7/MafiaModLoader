// The game's files and models (mml.h game_files, model_file, model_open, model_nodes, model_mesh, model_materials,
// model_close; docs/MODDING.md "The game's files and models").
//
// The archives: rw_data opens a8, b8, a2, a6, a0, a1, a3, ac, a4, aa, a5, a7, a9, ab (.dta) in that order, each with
// two keys the game gives it (knowledge/notes/rw_data.md); the same keys open both builds' archives. Each is read here
// with its own handle (never rw_data's): the header and the entry table after the magic `ISD0`, then each entry's
// metadata (0x20 bytes: +0x10 the size, +0x18 the name's length) and its full name, every block of 8 bytes XORed
// with the keys. A name the game finds in the game's folder is opened from there first (dtaOpen's order), and a mod's
// before both (files.cpp): game_files tells each name once, with what answers it. Read once, at the first question.
//
// A model frame (type 9) keeps the name it was opened by at +0x154 (LS3D's I3D_model::Open copies it there, a
// counted string a duplicate shares): every model the game makes goes through it (the scene's own frames do not:
// they are the scene file's, under no model).
//
// .4ds models (version 29, the same files in both builds) are read through the game's file layer (FilesRead: a mod's
// own file answers too) and parsed after the format's public description (RoadTrain/mafia-formats, 4ds.bt);
// mafia-ragdoll's reader (its model4ds.cpp) is the reference this one follows.
//
// Bridge methods: files.game, model.file, model.info.
#include "host.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// ---- the archives
struct ArchiveKey { const char *name; uint32_t k1, k2; };
// (Game.exe's archive table, in the order rw_data looks in them: the keys the game passes, before rw_data's XOR)
const ArchiveKey kArchives[] = {
    {"a8.dta", 0x6A63FA71, 0xEC45D8CE}, {"b8.dta", 0x79E21CDB, 0xB60F823F}, {"a2.dta", 0x1417D340, 0xB6399E19},
    {"a6.dta", 0x728E2DB9, 0x5055DA68}, {"a0.dta", 0x7F3D9B74, 0xEC48FE17}, {"a1.dta", 0xE7375F59, 0x0900210E},
    {"a3.dta", 0xA94B8D3C, 0x771F3888}, {"ac.dta", 0xA94B8D3C, 0x771F3888}, {"a4.dta", 0xA94B8D3C, 0x771F3888},
    {"aa.dta", 0xD4AD90C6, 0x67DA216E}, {"a5.dta", 0x4F4BB0C6, 0xEA340420}, {"a7.dta", 0xF4F03A72, 0xE266FE62},
    {"a9.dta", 0x959D1117, 0x5B763446}, {"ab.dta", 0x7F3D9B74, 0xEC48FE17},
};
constexpr uint32_t kKeyXor1 = 0x39475694, kKeyXor2 = 0x34985762;

struct GameFile { std::string name, source; uint32_t size = 0; };
std::map<std::string, GameFile> g_files;   // by the lower-case name: the archives' and the game's folder's
std::mutex g_filesMx;
bool g_scanned = false;
std::string g_scanNote;

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void Decrypt(uint8_t *b, size_t n, uint32_t k1, uint32_t k2) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint32_t *p = reinterpret_cast<uint32_t *>(b + i);
        p[0] ^= k1;
        p[1] ^= k2;
    }
}

bool ReadAt(HANDLE f, uint64_t off, void *buf, uint32_t n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    DWORD got = 0;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf, n, &got, nullptr) && got == n;
}

int ScanArchive(const ArchiveKey &a, std::map<std::string, GameFile> *out) {
    HANDLE f = PathCreateFile(GameDir() + a.name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS);
    if (f == INVALID_HANDLE_VALUE) return -1;
    uint32_t k1 = a.k1 ^ kKeyXor1, k2 = a.k2 ^ kKeyXor2;
    int n = 0;
    uint8_t magic[4], hdr[16];
    if (ReadAt(f, 0, magic, 4) && memcmp(magic, "ISD0", 4) == 0 && ReadAt(f, 4, hdr, 16)) {
        Decrypt(hdr, 16, k1, k2);
        uint32_t count, tableOff, tableSize;
        memcpy(&count, hdr, 4);
        memcpy(&tableOff, hdr + 4, 4);
        memcpy(&tableSize, hdr + 8, 4);
        if (count > 0 && count < 200000 && tableSize == count * 0x1C) {
            std::vector<uint8_t> table(tableSize);
            if (ReadAt(f, tableOff, table.data(), tableSize)) {
                Decrypt(table.data(), table.size(), k1, k2);
                uint8_t meta[0x20 + 264];
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t metaOff;
                    memcpy(&metaOff, table.data() + i * 0x1C + 4, 4);
                    if (!ReadAt(f, metaOff, meta, 0x20)) continue;
                    Decrypt(meta, 0x20, k1, k2);
                    uint32_t size, len;
                    memcpy(&size, meta + 0x10, 4);
                    memcpy(&len, meta + 0x18, 4);
                    len &= 0x7FFF;
                    if (len == 0 || len > 260) continue;
                    uint32_t padded = (len + 7) & ~7u;
                    if (!ReadAt(f, metaOff + 0x20, meta + 0x20, padded)) continue;
                    Decrypt(meta + 0x20, padded, k1, k2);
                    std::string name(reinterpret_cast<const char *>(meta + 0x20), len);
                    std::string key = Lower(name);
                    if (out->count(key)) continue;   // (an archive the game looks in before has it)
                    (*out)[key] = {name, a.name, size};
                    n++;
                }
            }
        }
    }
    CloseHandle(f);
    return n;
}

// The game's folder's own files under the folders the archives hold (the game opens them first). rel is UTF-8; the
// names are kept as the archives' are, in the system's code page (the game's names).
void ScanFolder(const std::string &rel, int depth, std::map<std::string, GameFile> *out) {
    if (depth > 6) return;
    for (const FoundFile &fd : ListDir(GameDir() + rel + "*")) {
        if (fd.dir()) {
            ScanFolder(rel + fd.name + "\\", depth + 1, out);
        } else {
            std::string name = AcpOf(rel + fd.name);
            (*out)[Lower(name)] = {name, "folder", static_cast<uint32_t>(fd.size)};
        }
    }
}

void Scan() {
    std::lock_guard<std::mutex> lk(g_filesMx);
    if (g_scanned) return;
    g_scanned = true;
    std::map<std::string, GameFile> archives;
    int archivesRead = 0;
    for (const ArchiveKey &a : kArchives) if (ScanArchive(a, &archives) >= 0) archivesRead++;
    std::map<std::string, bool> tops;   // the archives' first folders
    for (auto &kv : archives) {
        size_t s = kv.first.find('\\');
        if (s != std::string::npos) tops[kv.first.substr(0, s)] = true;
    }
    std::map<std::string, GameFile> loose;
    for (auto &kv : tops) {
        std::vector<FoundFile> top = ListDir(GameDir() + AcpToUtf8(kv.first.c_str()));
        if (!top.empty() && top[0].dir()) ScanFolder(top[0].name + "\\", 1, &loose);
    }
    g_files.swap(archives);
    for (auto &kv : loose) g_files[kv.first] = kv.second;   // (the folder's before the archives')
    char note[96];
    snprintf(note, sizeof note, "%d archives, %zu files in the folders", archivesRead, loose.size());
    g_scanNote = note;
    HLOG("files: %zu names (%s)", g_files.size(), note);
}

// A glob: * any run, ? one character, without regard to case (both lower case here).
bool Match(const char *p, const char *s) {
    const char *star = nullptr, *back = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; continue; }
        if (*p == '*') { star = p++; back = s; continue; }
        if (star) { p = star + 1; s = ++back; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

// ---- .4ds
struct Mesh {
    std::vector<float> pos, nrm, uv;
    std::vector<uint16_t> idx, mat;
};
// A single mesh's LOD as the .4ds keeps its skin: the vertices weighted by no bone (the last ones), then a record per
// bone in the order of the joints' ids (the vertices wholly the bone's, then those blended with `other`, 1-based, 0
// the mesh's own space, by a weight each), the vertices sorted so (skin.cpp reads it).
struct SkinRecord {
    float bind[16];
    uint32_t one = 0, blended = 0, other = 0;
    std::vector<float> weights;
};
struct SkinData {
    uint32_t rigid = 0;
    std::vector<SkinRecord> bones;
};
struct Node {
    MmlModelNode info{};
    std::vector<Mesh> lods;
    std::vector<SkinData> skin;   // (single meshes and single morphs: one a LOD)
    int jointId = -1;             // (a joint: its id, the record of a single mesh's LOD it moves)
};
struct Model {
    MmlMod *mod = nullptr;
    std::string file;
    std::vector<Node> nodes;
    std::vector<MmlModelMaterial> materials;
};
std::map<int, std::unique_ptr<Model>> g_models;
int g_nextModel = 1;

struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    bool Need(size_t n) { if (!ok || static_cast<size_t>(end - p) < n) { ok = false; return false; } return true; }
    template <class T> T Get() { T v{}; if (Need(sizeof(T))) { memcpy(&v, p, sizeof(T)); p += sizeof(T); } return v; }
    void Skip(size_t n) { if (Need(n)) p += n; }
    std::string Str() {
        uint8_t n = Get<uint8_t>();
        std::string s;
        if (n && Need(n)) { s.assign(reinterpret_cast<const char *>(p), n); p += n; }
        return s;
    }
    void Vec(float *v) { for (int i = 0; i < 3; i++) v[i] = Get<float>(); }
};

enum : uint32_t {
    kMtlDiffuseTex = 1u << 18, kMtlReflectionTex = 1u << 19, kMtlImageAlpha = 1u << 24, kMtlOpacityTexAnim = 1u << 25,
    kMtlDiffuseTexAnim = 1u << 26, kMtlOpacityTex = 1u << 30,
};
enum : uint8_t { kVisual = 1, kLight = 2, kSector = 5, kDummy = 6, kTarget = 7, kJoint = 10, kOccluder = 12 };
enum : uint8_t {
    kObject = 0, kLitObject = 1, kSingleMesh = 2, kSingleMorph = 3, kBillboard = 4, kMorph = 5, kLens = 6, kProjector = 7,
    kMirror = 8, kEmitor = 9,
};

void Copy(char *dst, size_t cap, const std::string &s) {
    size_t n = std::min(s.size(), cap - 1);
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}

void ReadMaterial(Reader &r, MmlModelMaterial &m) {
    uint32_t flags = r.Get<uint32_t>();
    m.flags = flags;
    r.Vec(m.ambient);
    r.Vec(m.diffuse);
    r.Vec(m.emissive);
    m.opacity = r.Get<float>();
    bool hasTex = false;
    if (flags & kMtlReflectionTex) { r.Skip(4); r.Str(); }
    if (flags & kMtlDiffuseTex) { hasTex = true; Copy(m.texture, sizeof m.texture, r.Str()); }
    if ((flags & kMtlOpacityTex) && !(flags & kMtlImageAlpha)) { hasTex = true; Copy(m.opacity_texture, sizeof m.opacity_texture, r.Str()); }
    if (!hasTex) r.Skip(1);
    if (flags & (kMtlOpacityTexAnim | kMtlDiffuseTexAnim)) r.Skip(4 + 2 + 4 + 4 + 4);
}

void ReadObject(Reader &r, Node &n) {
    int inst = r.Get<uint16_t>();
    n.info.instance_of = inst ? inst - 1 : -1;
    if (inst) return;
    int lods = r.Get<uint8_t>();
    n.info.lods = lods;
    n.lods.resize(lods);
    for (int l = 0; l < lods && r.ok; l++) {
        Mesh &m = n.lods[l];
        r.Skip(4);   // (the LOD's clipping range)
        uint16_t nv = r.Get<uint16_t>();
        m.pos.resize(nv * 3);
        m.nrm.resize(nv * 3);
        m.uv.resize(nv * 2);
        for (uint16_t i = 0; i < nv && r.ok; i++) {
            r.Vec(&m.pos[i * 3]);
            r.Vec(&m.nrm[i * 3]);
            m.uv[i * 2] = r.Get<float>();
            m.uv[i * 2 + 1] = r.Get<float>();
        }
        uint8_t groups = r.Get<uint8_t>();
        for (uint8_t g = 0; g < groups && r.ok; g++) {
            uint16_t nf = r.Get<uint16_t>();
            size_t first = m.idx.size() / 3;
            for (uint16_t f = 0; f < nf && r.ok; f++) {
                uint16_t a = r.Get<uint16_t>(), b = r.Get<uint16_t>(), c = r.Get<uint16_t>();
                if (a < nv && b < nv && c < nv) { m.idx.push_back(a); m.idx.push_back(b); m.idx.push_back(c); }
            }
            uint16_t material = r.Get<uint16_t>();
            m.mat.resize(m.idx.size() / 3, 0);
            for (size_t t = first; t < m.mat.size(); t++) m.mat[t] = material;
        }
    }
}

void ReadSingleMesh(Reader &r, Node &n) {
    n.skin.resize(n.info.lods);
    for (int l = 0; l < n.info.lods && r.ok; l++) {
        SkinData &s = n.skin[l];
        uint8_t bones = r.Get<uint8_t>();
        s.rigid = r.Get<uint32_t>();
        r.Skip(24);   // (the bounds)
        s.bones.resize(bones);
        for (uint8_t b = 0; b < bones && r.ok; b++) {
            SkinRecord &rec = s.bones[b];
            for (float &v : rec.bind) v = r.Get<float>();
            rec.one = r.Get<uint32_t>();
            rec.blended = r.Get<uint32_t>();
            rec.other = r.Get<uint32_t>();
            r.Skip(24);   // (the bone's bounds)
            if (!r.Need(static_cast<size_t>(rec.blended) * 4)) break;
            rec.weights.resize(rec.blended);
            for (float &w : rec.weights) w = r.Get<float>();
        }
    }
}

void ReadMorph(Reader &r) {
    uint8_t targets = r.Get<uint8_t>();
    if (!targets) return;
    uint8_t channels = r.Get<uint8_t>(), lods = r.Get<uint8_t>();
    for (uint8_t l = 0; l < lods && r.ok; l++)
        for (uint8_t c = 0; c < channels && r.ok; c++) {
            uint16_t nv = r.Get<uint16_t>();
            if (!nv) continue;
            r.Skip(static_cast<size_t>(nv) * targets * 24);
            if (r.Get<uint8_t>()) r.Skip(static_cast<size_t>(nv) * 2);
        }
    r.Skip(24 + 12 + 4);   // (bounds, centre, radius)
}

bool Parse(const uint8_t *data, size_t size, Model *m, std::string *err) {
    Reader r{data, data + size};
    if (size < 4 || memcmp(data, "4DS", 3) != 0) { *err = "not a 4DS file"; return false; }
    r.Skip(4);
    uint16_t version = r.Get<uint16_t>();
    if (version != 29) { *err = "a 4DS of version " + std::to_string(version) + " (29 known)"; return false; }
    r.Skip(8);   // (its time)
    uint16_t nmat = r.Get<uint16_t>();
    m->materials.assign(nmat, MmlModelMaterial{});
    for (uint16_t i = 0; i < nmat && r.ok; i++) ReadMaterial(r, m->materials[i]);
    uint16_t count = r.Get<uint16_t>();
    for (uint16_t i = 0; i < count && r.ok; i++) {
        Node n;
        n.info.frame_type = r.Get<uint8_t>();
        n.info.visual_type = -1;
        n.info.instance_of = -1;
        if (n.info.frame_type == kVisual) { n.info.visual_type = r.Get<uint8_t>(); r.Skip(2); }
        n.info.parent = static_cast<int>(r.Get<uint16_t>()) - 1;
        r.Vec(n.info.pos);
        r.Vec(n.info.scale);
        for (float &q : n.info.rot) q = r.Get<float>();
        r.Skip(1);   // (culling flags)
        Copy(n.info.name, sizeof n.info.name, r.Str());
        r.Str();     // (user properties)
        switch (n.info.frame_type) {
        case kVisual:
            switch (n.info.visual_type) {
            case kObject: case kLitObject: ReadObject(r, n); break;
            case kSingleMesh: ReadObject(r, n); ReadSingleMesh(r, n); break;
            case kSingleMorph: ReadObject(r, n); ReadSingleMesh(r, n); ReadMorph(r); break;
            case kBillboard: ReadObject(r, n); r.Skip(5); break;
            case kMorph: ReadObject(r, n); ReadMorph(r); break;
            case kLens: { uint8_t g = r.Get<uint8_t>(); r.Skip(static_cast<size_t>(g) * 6); break; }
            case kProjector: r.Skip(4); break;
            case kMirror: {
                r.Skip(24 + 16 + 64 + 12 + 4);
                uint32_t nv = r.Get<uint32_t>(), nf = r.Get<uint32_t>();
                r.Skip(static_cast<size_t>(nv) * 12 + static_cast<size_t>(nf) * 6);
                break;
            }
            case kEmitor: r.Skip(5); break;
            default: *err = "an unknown visual type " + std::to_string(n.info.visual_type) + " in " + n.info.name; return false;
            }
            break;
        case kLight: r.Skip(84); break;
        case kSector: {
            r.Skip(8);
            uint32_t nv = r.Get<uint32_t>(), nf = r.Get<uint32_t>();
            r.Skip(static_cast<size_t>(nv) * 12 + static_cast<size_t>(nf) * 6 + 24);
            uint8_t portals = r.Get<uint8_t>();
            for (uint8_t p = 0; p < portals && r.ok; p++) {
                uint8_t pv = r.Get<uint8_t>();
                r.Skip(16 + 4 + 4 + 4 + static_cast<size_t>(pv) * 12);
            }
            break;
        }
        case kDummy: r.Skip(24); break;
        case kTarget: { r.Skip(2); uint8_t l = r.Get<uint8_t>(); r.Skip(static_cast<size_t>(l) * 2); break; }
        case kJoint: r.Skip(64); n.jointId = static_cast<int>(r.Get<uint32_t>()); break;
        case kOccluder: {
            uint32_t nv = r.Get<uint32_t>(), nf = r.Get<uint32_t>();
            r.Skip(static_cast<size_t>(nv) * 12 + static_cast<size_t>(nf) * 6);
            break;
        }
        default: *err = "an unknown frame type " + std::to_string(n.info.frame_type) + " in " + n.info.name; return false;
        }
        m->nodes.push_back(std::move(n));
    }
    if (!r.ok) { *err = "the file ends early"; return false; }
    return true;
}

// "Tommy", "Tommy.i3d", "models\\Tommy.4ds" -> models\Tommy.4ds (LS3D names its models .i3d; their files are .4ds).
std::string ModelPath(std::string f) {
    for (char &c : f) if (c == '/') c = '\\';
    size_t slash = f.rfind('\\'), dot = f.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) f += ".4ds";
    else if (Lower(f.substr(dot)) == ".i3d") f = f.substr(0, dot) + ".4ds";
    if (f.find('\\') == std::string::npos) f = "models\\" + f;
    return f;
}

std::string HexAddr(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

const Mesh *MeshOf(const Model &m, int node, int lod) {
    if (node < 0 || node >= static_cast<int>(m.nodes.size())) return nullptr;
    const Node *src = &m.nodes[node];
    for (int hops = 0; src->info.instance_of >= 0 && hops < 8; hops++) {
        if (src->info.instance_of >= static_cast<int>(m.nodes.size())) return nullptr;
        src = &m.nodes[src->info.instance_of];
    }
    if (src->lods.empty()) return nullptr;
    if (lod < 0 || lod >= static_cast<int>(src->lods.size())) lod = static_cast<int>(src->lods.size()) - 1;
    return &src->lods[lod];
}

} // namespace

bool ModelSkin(const char *file, const char *mesh, std::vector<ModelSkinLod> *out, std::string *meshName, std::string *err) {
    out->clear();
    int id = ModelOpen(nullptr, file, err);
    if (!id) return false;
    std::unique_ptr<Model> m = std::move(g_models[id]);
    g_models.erase(id);
    const std::string want = Lower(mesh ? mesh : "");
    int at = -1;
    for (size_t i = 0; i < m->nodes.size() && at < 0; i++) {
        const Node &n = m->nodes[i];
        bool skinned = n.info.frame_type == kVisual && (n.info.visual_type == kSingleMesh || n.info.visual_type == kSingleMorph);
        if (skinned && (want.empty() || Lower(n.info.name) == want)) at = static_cast<int>(i);
    }
    if (at < 0) { *err = m->file + ": no single mesh" + (want.empty() ? std::string() : " \"" + std::string(mesh) + "\""); return false; }
    const Node &sm = m->nodes[at];
    *meshName = sm.info.name;
    // the joints under it (LS3D binds a single mesh to the joints in its subtree: a record's bone is the joint whose id
    // is the record's place)
    auto under = [&](int i) {
        for (int hops = 0, p = m->nodes[i].info.parent; p >= 0 && hops < 256; hops++, p = m->nodes[p].info.parent)
            if (p == at) return true;
        return false;
    };
    std::map<int, std::string> joints;
    for (size_t i = 0; i < m->nodes.size(); i++)
        if (m->nodes[i].info.frame_type == kJoint && m->nodes[i].jointId >= 0 && under(static_cast<int>(i)))
            joints.emplace(m->nodes[i].jointId, m->nodes[i].info.name);
    for (int l = 0; l < sm.info.lods; l++) {
        const Mesh *me = MeshOf(*m, at, l);
        if (!me || l >= static_cast<int>(sm.skin.size())) break;
        ModelSkinLod o;
        o.vertices = static_cast<uint32_t>(me->pos.size() / 3);
        o.rigid = sm.skin[l].rigid;
        o.pos = me->pos;
        o.nrm = me->nrm;
        o.uv = me->uv;
        o.idx = me->idx;
        for (size_t b = 0; b < sm.skin[l].bones.size(); b++) {
            const SkinRecord &r = sm.skin[l].bones[b];
            ModelSkinLod::Bone bo;
            auto j = joints.find(static_cast<int>(b));
            bo.joint = j != joints.end() ? j->second : std::string();
            memcpy(bo.bind, r.bind, sizeof bo.bind);
            bo.one = r.one;
            bo.blended = r.blended;
            bo.other = r.other;
            bo.weights = r.weights;
            o.bones.push_back(std::move(bo));
        }
        out->push_back(std::move(o));
    }
    if (out->empty()) { *err = m->file + ": " + *meshName + " has no LODs"; return false; }
    return true;
}

int GameFiles(const char *pattern, MmlFileFn fn, void *user) {
    Scan();
    std::string p = Lower(pattern && *pattern ? pattern : "*");
    for (char &c : p) if (c == '/') c = '\\';
    std::map<std::string, std::pair<std::string, std::string>> all;   // lower -> name, source
    {
        std::lock_guard<std::mutex> lk(g_filesMx);
        for (auto &kv : g_files) if (Match(p.c_str(), kv.first.c_str())) all[kv.first] = {kv.second.name, kv.second.source};
    }
    std::vector<std::pair<std::string, std::string>> mods;
    FilesNames(&mods);
    for (auto &m : mods) if (Match(p.c_str(), m.first.c_str())) all[m.first] = {m.first, m.second};   // (the mods' before all)
    int n = 0;
    for (auto &kv : all) {
        n++;
        if (fn && fn(kv.second.first.c_str(), kv.second.second.c_str(), user)) break;
    }
    return n;
}

int ModelFile(void *frame, std::string *out) {
    out->clear();
    auto f = [&] {
        void *fr = frame;
        for (int i = 0; fr && i < 64; i++, fr = game::at<void *>(fr, game::kFrameParent)) {
            if (game::at<int>(fr, game::kFrameType) != 9) continue;
            const char *name = game::at<const char *>(fr, game::kModelFile);
            if (name) { out->assign(name, strnlen(name, 260)); return; }
        }
    };
    __try { f(); } __except (EXCEPTION_EXECUTE_HANDLER) { out->clear(); }
    return static_cast<int>(out->size());
}

int ModelOpen(MmlMod *mod, const char *file, std::string *err) {
    if (!file || !*file) { *err = "file: a model (\"Tommy\", \"models\\\\Tommy.4ds\")"; return 0; }
    std::string path = ModelPath(file);
    int64_t size = FilesRead(path.c_str(), nullptr, 0, 0);
    if (size <= 0) { *err = "no file \"" + path + "\""; return 0; }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (FilesRead(path.c_str(), data.data(), data.size(), 0) != size) { *err = "\"" + path + "\" could not be read"; return 0; }
    auto m = std::make_unique<Model>();
    m->mod = mod;
    m->file = path;
    if (!Parse(data.data(), data.size(), m.get(), err)) { *err = path + ": " + *err; return 0; }
    int id = g_nextModel++;
    g_models[id] = std::move(m);
    return id;
}

int ModelNodes(int model, MmlModelNode *out, int max) {
    auto it = g_models.find(model);
    if (it == g_models.end()) return 0;
    const auto &nodes = it->second->nodes;
    for (int i = 0; out && i < max && i < static_cast<int>(nodes.size()); i++) out[i] = nodes[i].info;
    return static_cast<int>(nodes.size());
}

int ModelMesh(int model, int node, int lod, MmlModelMesh *out) {
    auto it = g_models.find(model);
    if (it == g_models.end() || !out) return 0;
    const Mesh *m = MeshOf(*it->second, node, lod);
    if (!m) return 0;
    out->pos = m->pos.data();
    out->normal = m->nrm.data();
    out->uv = m->uv.data();
    out->vertices = static_cast<int>(m->pos.size() / 3);
    out->index = m->idx.data();
    out->material = m->mat.data();
    out->triangles = static_cast<int>(m->idx.size() / 3);
    return 1;
}

int ModelMaterials(int model, MmlModelMaterial *out, int max) {
    auto it = g_models.find(model);
    if (it == g_models.end()) return 0;
    const auto &mats = it->second->materials;
    for (int i = 0; out && i < max && i < static_cast<int>(mats.size()); i++) out[i] = mats[i];
    return static_cast<int>(mats.size());
}

void ModelClose(MmlMod *mod, int model) {
    auto it = g_models.find(model);
    if (it != g_models.end() && it->second->mod == mod) g_models.erase(it);
}

void ModelsModGone(MmlMod *mod) {
    for (auto it = g_models.begin(); it != g_models.end();) {
        if (it->second->mod == mod) it = g_models.erase(it);
        else ++it;
    }
}

bool ModelsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "files.game" && method != "model.file" && method != "model.info") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "files.game") {
            struct Acc { json list = json::array(); size_t limit; std::map<std::string, int> by; } acc;
            acc.limit = p.value("limit", 500u);
            int n = GameFiles(p.value("match", "*").c_str(), [](const char *name, const char *src, void *u) {
                Acc *a = static_cast<Acc *>(u);
                a->by[src]++;
                if (a->list.size() < a->limit) a->list.push_back({{"name", name}, {"from", src}});
                return 0;
            }, &acc);
            json out = {{"count", n}, {"files", acc.list}, {"from", acc.by}};
            {
                std::lock_guard<std::mutex> lk(g_filesMx);
                out["scanned"] = g_scanNote;
            }
            if (static_cast<size_t>(n) > acc.limit) out["note"] = "the first " + std::to_string(acc.limit) + " shown (limit)";
            *reply = dump(out);
        } else if (method == "model.file") {
            void *frame = nullptr;
            if (p.contains("frame")) frame = reinterpret_cast<void *>(ParseAddress(p.value("frame", "")));
            else if (p.contains("prop")) frame = reinterpret_cast<void *>(ParseAddress(p.value("prop", "")));
            else {
                void *actor = nullptr;
                if (!ParseActor(p.value("actor", "player"), &actor, &err)) { *reply = "model.file: actor: " + err; return true; }
                frame = ActorFrame(actor);
            }
            if (!frame) { *reply = "model.file: no frame (frame, prop or actor)"; return true; }
            std::string f;
            ModelFile(frame, &f);
            json out = {{"frame", frame ? json(HexAddr(frame)) : json(nullptr)}, {"file", f}};
            if (f.empty()) out["note"] = "no model above it (a frame of the mission's scene itself?)";
            else out["4ds"] = ModelPath(f);
            *reply = dump(out);
        } else {
            int id = ModelOpen(nullptr, p.value("file", "").c_str(), &err);
            if (!id) { *reply = "model.info: " + err; return true; }
            Model &m = *g_models[id];
            json nodes = json::array();
            bool meshes = p.value("meshes", true);
            for (size_t i = 0; i < m.nodes.size(); i++) {
                const MmlModelNode &n = m.nodes[i].info;
                json j = {{"index", i}, {"name", n.name}, {"type", n.frame_type}, {"parent", n.parent}};
                if (n.visual_type >= 0) j["visual"] = n.visual_type;
                if (n.instance_of >= 0) j["instance_of"] = n.instance_of;
                if (meshes) {
                    const Mesh *mesh = MeshOf(m, static_cast<int>(i), 0);
                    if (mesh) j["lod0"] = {{"vertices", mesh->pos.size() / 3}, {"triangles", mesh->idx.size() / 3}};
                    if (n.lods > 1) j["lods"] = n.lods;
                }
                nodes.push_back(j);
            }
            json mats = json::array();
            for (const MmlModelMaterial &mt : m.materials) {
                json j = {{"flags", mt.flags}};
                if (mt.texture[0]) j["texture"] = mt.texture;
                if (mt.opacity_texture[0]) j["opacity_texture"] = mt.opacity_texture;
                mats.push_back(j);
            }
            *reply = dump({{"file", m.file}, {"nodes", nodes}, {"materials", mats}});
            g_models.erase(id);
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
