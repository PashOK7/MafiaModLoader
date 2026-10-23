// The game's own geometry, left out of LS3D's drawing, surveyed and captured, by its textures' names (mml.h geom_hide,
// geom_show, geom_capture; docs/MODDING.md "The game's own geometry hidden or replaced").
//
// LS3D's Render draws a mesh face group by face group: each visual queues a 32-byte entry per face group of its LOD
// (m_AddFGroups, vtable +0x68), Render sorts them and calls the visual's m_RenderFGroup (vtable +0x6C) with the entry
// (game_build.h fgRender*: three functions draw every mesh). The loader hooks those three: a face group whose
// material's texture (its animation's frame now) matches a mod's pattern is not drawn in the drawings the rule names
// (the frame, the scene views); nothing in the game's memory is changed and no pointer is kept from one frame to the
// next. A capture lets the group's own m_RenderFGroup run and reads what it hands Direct3D: its DrawIndexedPrimitive's
// vertex and index buffers, base vertex and the world transform, so the triangles are exactly what LS3D draws, for any
// kind of visual (LOD, instancing, morphs); a hidden group is captured with the draw itself left out.
//
// Bridge methods (game thread): geom.hide {pattern, where}, geom.show {handle}, geom.state, geom.survey (what the
// frame draws, by texture: {} starts one, {id} reads it), geom.capture ({pattern} starts one, {id} reads it).
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

// The layouts (the same in 1.2 and 1.0: game_build.h fgRender*).
constexpr int kEntryVisual = 0x00, kEntryGroup = 0x10;
constexpr int kGroupMaterial = 0x04, kGroupFaces = 0x08;
constexpr int kMaterialFlags = 0x08, kMaterialTexture = 0x0C;
constexpr int kTextureName = 0x1C;
constexpr int kVisualType = 0x1D4;
constexpr int kVtDrawIndexedPrimitive = 82;   // IDirect3DDevice9's
constexpr size_t kMaxRules = 64;

template <class T> T &At(const void *p, int off) { return *reinterpret_cast<T *>(const_cast<uint8_t *>(static_cast<const uint8_t *>(p)) + off); }

using RenderFgFn = void(__fastcall *)(void *visual, void *edx, uint8_t *entry);
using DipFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
RenderFgFn oObject, oLit, oBillboard, oShadow;
DipFn oDip;
bool g_hookTried, g_hooked, g_dipHooked;
std::string g_hookWhy;
uintptr_t g_dipAt;

struct Rule {
    MmlMod *mod;
    int handle;
    std::string pattern;   // lower case
    int where;             // MML_GEOM_*
    unsigned groupsNow = 0, facesNow = 0, groupsLast = 0, facesLast = 0;
};
std::vector<Rule> g_rules;
int g_nextHandle = 1;

// What each texture matched this frame (a bit a rule); cleared at each frame and when the rules change.
std::unordered_map<const void *, uint64_t> g_match;

// ---- one frame's state
bool g_inFrame;        // the frame's own Render (not a scene view's) is drawing
unsigned g_frames;

// the survey: what the frame draws, by texture
struct Seen { std::string name; unsigned groups = 0, faces = 0; uint32_t flags = 0; std::vector<uint32_t> types; bool hidden = false; };
int g_surveyWanted;    // its id: the next frame is surveyed
std::map<const void *, Seen> g_seen;
struct SurveyDone { int id; json result; };
std::vector<SurveyDone> g_surveys;
int g_nextId = 1;

// captures
struct Capture {
    MmlMod *mod;           // null: the bridge's
    int id;
    std::string pattern;
    MmlGeomFn fn;
    void *user;
    bool armed = true;     // waiting for its frame
    std::vector<float> tris;
    std::vector<MmlGeomGroup> groups;
    std::vector<std::string> names;
    unsigned unreadable = 0;
    json result;           // the bridge's, once done
    bool done = false;
};
std::vector<std::unique_ptr<Capture>> g_captures;   // (stable: a mod's function may ask for another while one is told)
// the group being drawn while a capture wants it (read by the DrawIndexedPrimitive hook)
struct Now { std::vector<Capture *> caps; const void *visual, *group; const char *texture; bool suppress, read; };
Now *g_now;
unsigned g_dipFails;
std::string g_dipWhy;

std::string Lower(const char *s) {
    std::string o;
    for (; s && *s; s++) o += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return o;
}

// '*' any run, '?' one character (both lower case).
bool Glob(const char *p, const char *s) {
    const char *star = nullptr, *after = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; continue; }
        if (*p == '*') { star = p++; after = s; continue; }
        if (star) { p = star + 1; s = ++after; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

const char *TextureName(const void *tex) {
    const char *n = tex ? At<const char *>(tex, kTextureName) : nullptr;
    return n && *n ? n : nullptr;
}

uint64_t MatchOf(const void *tex) {
    auto it = g_match.find(tex);
    if (it != g_match.end()) return it->second;
    uint64_t m = 0;
    std::string name = Lower(TextureName(tex));
    if (!name.empty())
        for (size_t i = 0; i < g_rules.size() && i < kMaxRules; i++)
            if (Glob(g_rules[i].pattern.c_str(), name.c_str())) m |= 1ull << i;
    g_match[tex] = m;
    return m;
}

// The DrawIndexedPrimitive LS3D makes for a captured group: its triangles read back from what the device has bound.
bool ReadDraw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT base, UINT start, UINT prims, std::vector<float> *out, std::string *why) {
    if (type != D3DPT_TRIANGLELIST) { *why = "not a triangle list"; return false; }
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    UINT off = 0, stride = 0;
    D3DMATRIX w{};
    if (FAILED(d->GetStreamSource(0, &vb, &off, &stride)) || !vb || FAILED(d->GetIndices(&ib)) || !ib ||
        FAILED(d->GetTransform(D3DTS_WORLD, &w)) || stride < 12) {
        if (vb) vb->Release();
        if (ib) ib->Release();
        *why = "no stream, indices or world transform";
        return false;
    }
    D3DVERTEXBUFFER_DESC vd{};
    D3DINDEXBUFFER_DESC id{};
    vb->GetDesc(&vd);
    ib->GetDesc(&id);
    UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
    bool ok = false;
    void *ip = nullptr, *vp = nullptr;
    if (SUCCEEDED(ib->Lock(start * isz, prims * 3 * isz, &ip, D3DLOCK_READONLY)) && ip) {
        if (SUCCEEDED(vb->Lock(0, 0, &vp, D3DLOCK_READONLY)) && vp) {
            ok = true;
            for (UINT i = 0; i < prims * 3; i++) {
                UINT idx = isz == 4 ? static_cast<const uint32_t *>(ip)[i] : static_cast<const uint16_t *>(ip)[i];
                size_t at = off + static_cast<size_t>(static_cast<INT>(idx) + base) * stride;
                if (at + 12 > vd.Size) { ok = false; *why = "an index past the vertex buffer"; break; }
                const float *p = reinterpret_cast<const float *>(static_cast<const uint8_t *>(vp) + at);
                out->push_back(p[0] * w._11 + p[1] * w._21 + p[2] * w._31 + w._41);
                out->push_back(p[0] * w._12 + p[1] * w._22 + p[2] * w._32 + w._42);
                out->push_back(p[0] * w._13 + p[1] * w._23 + p[2] * w._33 + w._43);
            }
            vb->Unlock();
        } else {
            *why = "the vertex buffer could not be locked";
        }
        ib->Unlock();
    } else {
        *why = "the index buffer could not be locked";
    }
    vb->Release();
    ib->Release();
    return ok;
}

HRESULT DrawOn(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n) {
    HRESULT hr;
    if (ShadersDraw(d, t, base, minIndex, verts, start, n, reinterpret_cast<void *>(oDip), &hr)) return hr;   // (shaders.cpp)
    return oDip(d, t, base, minIndex, verts, start, n);
}

HRESULT STDMETHODCALLTYPE hkDip(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n) {
    Now *now = g_now;
    if (!now) {
        HRESULT hr = DrawOn(d, t, base, minIndex, verts, start, n);
        MotionAfterDraw(d, t, base, minIndex, verts, start, n, reinterpret_cast<void *>(oDip));   // (motion.cpp: a moving object's)
        return hr;
    }
    if (!now->read) {   // (its first draw: a group's further passes draw the same triangles)
        now->read = true;
        std::vector<float> tris;
        std::string why;
        bool ok = ReadDraw(d, t, base, start, n, &tris, &why);
        if (!ok) { g_dipFails++; g_dipWhy = why; }
        for (Capture *c : now->caps) {
            if (!ok) { c->unreadable++; continue; }
            c->names.push_back(now->texture ? now->texture : "");
            MmlGeomGroup g{now->visual, now->group, nullptr, static_cast<unsigned>(c->tris.size() / 9), static_cast<unsigned>(tris.size() / 9)};
            c->groups.push_back(g);
            c->tris.insert(c->tris.end(), tris.begin(), tris.end());
        }
    }
    if (now->suppress) return D3D_OK;
    HRESULT hr = DrawOn(d, t, base, minIndex, verts, start, n);
    MotionAfterDraw(d, t, base, minIndex, verts, start, n, reinterpret_cast<void *>(oDip));
    return hr;
}

bool EnsureDip() {
    if (g_dipHooked) return true;
    auto *d = static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if (!d) { g_dipWhy = "no Direct3D 9 device (captures need a wrapper's Direct3D 9)"; return false; }
    std::string err;
    g_dipAt = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(d))[kVtDrawIndexedPrimitive]);
    if (!HookAdd(nullptr, g_dipAt, reinterpret_cast<void *>(&hkDip), reinterpret_cast<void **>(&oDip), &err)) {
        g_dipWhy = "DrawIndexedPrimitive not hooked: " + err;
        return false;
    }
    g_dipHooked = true;
    return true;
}

// What to do with a face group about to be drawn: false, draw it as LS3D would; true, leave it out (the caller
// returns). Sets g_now when a capture wants it (the caller draws it through LS3D with the capture listening).
bool Decide(void *visual, uint8_t *entry, Now *now) {
    const uint8_t *group = At<uint8_t *>(entry, kEntryGroup);
    const void *mat = group ? At<void *>(group, kGroupMaterial) : nullptr;
    const void *tex = mat ? At<void *>(mat, kMaterialTexture) : nullptr;
    if (!tex) return false;
    int view = ViewDrawingNow();
    int bit = view ? MML_GEOM_VIEWS : (g_inFrame ? MML_GEOM_FRAME : 0);
    if (!bit) return false;   // (a menu's scene, or a drawing not the world's)
    uint64_t m = g_rules.empty() ? 0 : MatchOf(tex);
    bool hide = false;
    unsigned faces = At<uint32_t>(group, kGroupFaces);
    for (size_t i = 0; m && i < g_rules.size() && i < kMaxRules; i++)
        if ((m >> i & 1) && (g_rules[i].where & bit)) {
            hide = true;
            g_rules[i].groupsNow++;
            g_rules[i].facesNow += faces;
        }
    if (bit == MML_GEOM_FRAME && g_surveyWanted) {
        Seen &s = g_seen[tex];
        if (!s.groups) {
            const char *n = TextureName(tex);
            s.name = n ? n : "(no name)";
            s.flags = At<uint32_t>(mat, kMaterialFlags);
        }
        s.groups++;
        s.faces += faces;
        s.hidden = s.hidden || hide;
        uint32_t type = At<uint32_t>(visual, kVisualType);
        if (std::find(s.types.begin(), s.types.end(), type) == s.types.end() && s.types.size() < 8) s.types.push_back(type);
    }
    if (bit == MML_GEOM_FRAME && !g_captures.empty()) {
        std::string name;
        for (auto &c : g_captures) {
            if (!c->armed) continue;
            if (name.empty()) name = Lower(TextureName(tex));
            if (!name.empty() && Glob(c->pattern.c_str(), name.c_str())) now->caps.push_back(c.get());
        }
        if (!now->caps.empty() && g_dipHooked) {
            now->visual = visual;
            now->group = group;
            now->texture = TextureName(tex);
            now->suppress = hide;
            now->read = false;
            return false;   // drawn (or not) by LS3D, the capture listening
        }
        now->caps.clear();
    }
    return hide;
}
DWORD SafeDecide(void *visual, uint8_t *entry, Now *now, bool *hide) {
    __try { *hide = Decide(visual, entry, now); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *hide = false; now->caps.clear(); return GetExceptionCode(); }
}

unsigned g_faults;
// The group drawn through LS3D, the mods' shaders told which it is (shaders.cpp), and the motion vectors (motion.cpp):
// kind 1 an object (objects, morphs, single meshes), 2 a lightmapped object, 3 a billboard.
template <RenderFgFn *orig, int kind>
void DrawGroup(void *visual, void *edx, uint8_t *entry) {
    const bool shaders = ShadersWanted(), motion = MotionWanted();
    if (!shaders && !motion) { (*orig)(visual, edx, entry); return; }
    int view = ViewDrawingNow();
    if (motion) MotionGroupBegin(kind, visual, At<uint8_t *>(entry, kEntryGroup), !view && g_inFrame);
    if (shaders) ShadersGroupBegin(kind, visual, entry, view ? MML_GEOM_VIEWS : (g_inFrame ? MML_GEOM_FRAME : 0));
    (*orig)(visual, edx, entry);
    if (shaders) ShadersGroupEnd();
    if (motion) MotionGroupEnd();
}

template <RenderFgFn *orig, int kind>
void __fastcall hkRenderFg(void *visual, void *edx, uint8_t *entry) {
    if (g_rules.empty() && !g_surveyWanted && g_captures.empty()) { DrawGroup<orig, kind>(visual, edx, entry); return; }
    Now now{};
    bool hide = false;
    if (SafeDecide(visual, entry, &now, &hide)) g_faults++;
    if (hide) return;
    if (now.caps.empty()) { DrawGroup<orig, kind>(visual, edx, entry); return; }
    g_now = &now;
    DrawGroup<orig, kind>(visual, edx, entry);
    g_now = nullptr;
}

// A shadow's faces on what receives it (the shadow visual: a car's shadow on the road), for the motion vectors alone: its
// entry is not a face group's, so no rules and no survey (game_build.h fgRenderShadow). The entry is a copy in
// Render's list, elsewhere each frame: the group is told by its sort key (+0x04), one of the visual's two halves
void __fastcall hkRenderShadow(void *visual, void *edx, uint8_t *entry) {
    if (!MotionWanted()) { oShadow(visual, edx, entry); return; }
    MotionGroupBegin(4, visual, reinterpret_cast<const void *>(static_cast<uintptr_t>(At<uint32_t>(entry, 0x04))),
                     !ViewDrawingNow() && g_inFrame);
    oShadow(visual, edx, entry);
    MotionGroupEnd();
}

bool EnsureHooks() {
    if (g_hookTried) return g_hooked;
    g_hookTried = true;
    if (!game::g_ls3d) { g_hookWhy = "no game known"; g_hookTried = false; return false; }
    const game::Ls3dBuild &l = game::Ls3d();
    if (!l.fgRenderObject || !l.fgRenderLit || !l.fgRenderBillboard || !l.fgRenderShadow) { g_hookWhy = "not known for this LS3DF.dll"; return false; }
    uintptr_t base = ModuleBase("LS3DF.dll");
    struct { uintptr_t at; void *detour; RenderFgFn *orig; } hooks[] = {
        {base + l.fgRenderObject, reinterpret_cast<void *>(&hkRenderFg<&oObject, 1>), &oObject},
        {base + l.fgRenderLit, reinterpret_cast<void *>(&hkRenderFg<&oLit, 2>), &oLit},
        {base + l.fgRenderBillboard, reinterpret_cast<void *>(&hkRenderFg<&oBillboard, 3>), &oBillboard},
        {base + l.fgRenderShadow, reinterpret_cast<void *>(&hkRenderShadow), &oShadow},
    };
    std::string err;
    for (auto &h : hooks) {
        if (HookAdd(nullptr, h.at, h.detour, reinterpret_cast<void **>(h.orig), &err)) continue;
        for (auto &u : hooks) if (*u.orig) HookRemoveLink(nullptr, u.at, u.detour);
        g_hookWhy = "m_RenderFGroup not hooked: " + err;
        return false;
    }
    g_hooked = true;
    HLOG("geom: the face groups' m_RenderFGroup hooked (objects, lit objects, billboards, shadows)");
    return true;
}

std::string FourCC(uint32_t t) {
    std::string s;
    for (int i = 0; i < 4; i++) {
        char c = static_cast<char>(t >> (i * 8));
        s += c >= 0x20 && c < 0x7F ? c : '?';
    }
    return s;
}

int AddRule(MmlMod *mod, const char *pattern, int where, std::string *err) {
    std::string p = Lower(pattern);
    if (p.empty()) { *err = "an empty pattern"; return 0; }
    if (g_rules.size() >= kMaxRules) { *err = "64 rules already"; return 0; }
    if (!EnsureHooks()) { *err = g_hookWhy; return 0; }
    if (!(where & (MML_GEOM_FRAME | MML_GEOM_VIEWS))) where = MML_GEOM_FRAME | MML_GEOM_VIEWS;
    Rule r{mod, g_nextHandle++, p, where & (MML_GEOM_FRAME | MML_GEOM_VIEWS)};
    g_rules.push_back(r);
    g_match.clear();
    return r.handle;
}

void RemoveRules(MmlMod *mod, int handle) {
    size_t before = g_rules.size();
    g_rules.erase(std::remove_if(g_rules.begin(), g_rules.end(), [&](const Rule &r) { return r.mod == mod && (handle == 0 || r.handle == handle); }),
                  g_rules.end());
    if (g_rules.size() != before) g_match.clear();
}

json Bounds(const std::vector<float> &t) {
    if (t.empty()) return nullptr;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < t.size(); i++) { lo[i % 3] = std::min(lo[i % 3], t[i]); hi[i % 3] = std::max(hi[i % 3], t[i]); }
    auto r2 = [](float v) { return std::round(v * 100) / 100; };
    return {{r2(lo[0]), r2(lo[1]), r2(lo[2])}, {r2(hi[0]), r2(hi[1]), r2(hi[2])}};
}

struct DeliverCall { Capture *c; MmlGeomCapture *cap; };
void CallDeliver(void *p) {
    auto *d = static_cast<DeliverCall *>(p);
    d->c->fn(d->cap, d->c->user);
}

} // namespace

bool GeomEnsureHooks(std::string *why) {
    if (EnsureHooks()) return true;
    *why = g_hookWhy;
    return false;
}
bool GeomEnsureDip(std::string *why) {
    if (EnsureDip()) return true;
    *why = g_dipWhy;
    return false;
}

int GeomHide(MmlMod *mod, const char *pattern, int where, std::string *err) { return AddRule(mod, pattern, where, err); }
void GeomShow(MmlMod *mod, int handle) { RemoveRules(mod, handle); }

bool GeomCapture(MmlMod *mod, const char *pattern, MmlGeomFn fn, void *user, std::string *err) {
    std::string p = Lower(pattern);
    if (!fn || p.empty()) { *err = !fn ? "no function" : "an empty pattern"; return false; }
    if (!EnsureHooks()) { *err = g_hookWhy; return false; }
    g_captures.push_back(std::unique_ptr<Capture>(new Capture{mod, g_nextId++, p, fn, user}));
    return true;
}

void GeomRemove(MmlMod *mod) {
    RemoveRules(mod, 0);
    g_captures.erase(std::remove_if(g_captures.begin(), g_captures.end(), [&](const std::unique_ptr<Capture> &c) { return c->mod == mod; }),
                     g_captures.end());
}

// (render.cpp) a frame of the mission's world begins (before the scene views), its own drawing, its end.
void GeomFrameBegin() {
    g_match.clear();
    for (Rule &r : g_rules) { r.groupsNow = r.facesNow = 0; }
    if (!g_captures.empty() || ShadersWanted()) EnsureDip();
}
void GeomFrameDrawing(bool on) { g_inFrame = on; }
void GeomFrameEnd() {
    g_frames++;
    for (Rule &r : g_rules) { r.groupsLast = r.groupsNow; r.facesLast = r.facesNow; }
    if (g_surveyWanted) {
        std::vector<const Seen *> list;
        for (auto &kv : g_seen) list.push_back(&kv.second);
        std::sort(list.begin(), list.end(), [](const Seen *a, const Seen *b) { return a->faces > b->faces; });
        json out = json::array();
        unsigned groups = 0, faces = 0;
        for (const Seen *s : list) {
            json types = json::array();
            for (uint32_t t : s->types) types.push_back(FourCC(t));
            char flags[16];
            snprintf(flags, sizeof flags, "0x%08X", s->flags);
            out.push_back({{"texture", s->name}, {"groups", s->groups}, {"faces", s->faces}, {"visuals", types},
                           {"material_flags", flags}, {"hidden", s->hidden}});
            groups += s->groups;
            faces += s->faces;
        }
        g_surveys.push_back({g_surveyWanted, {{"textures", out}, {"groups", groups}, {"faces", faces}}});
        if (g_surveys.size() > 4) g_surveys.erase(g_surveys.begin());
        g_seen.clear();
        g_surveyWanted = 0;
    }
    // the captures armed this frame are done: the mods' told, the bridge's kept for geom.capture {id}
    size_t count = g_captures.size();   // (a mod's function may ask for more: they wait for the next frame)
    for (size_t k = 0; k < count; k++) {
        Capture &c = *g_captures[k];
        if (!c.armed) continue;
        if (!g_dipHooked) c.unreadable = 1;
        c.armed = false;
        c.done = true;
        for (size_t i = 0; i < c.groups.size(); i++) c.groups[i].texture = c.names[i].c_str();
        if (c.mod) {
            MmlGeomCapture cap{sizeof cap, c.tris.empty() ? nullptr : c.tris.data(), static_cast<unsigned>(c.tris.size() / 9),
                               c.groups.empty() ? nullptr : c.groups.data(), static_cast<unsigned>(c.groups.size()), c.unreadable};
            DeliverCall d{&c, &cap};
            ModCall(c.mod, CallDeliver, &d, "its geometry capture's function");
        } else {
            json sample = json::array();
            for (size_t i = 0; i < c.tris.size() && i < 9 * 4; i += 9) {
                json tri = json::array();
                for (int k = 0; k < 9; k += 3) tri.push_back({c.tris[i + k], c.tris[i + k + 1], c.tris[i + k + 2]});
                sample.push_back(tri);
            }
            std::map<std::string, unsigned> per;
            for (size_t i = 0; i < c.groups.size(); i++) per[c.names[i]] += c.groups[i].count;
            c.result = {{"id", c.id}, {"done", true}, {"pattern", c.pattern}, {"groups", c.groups.size()}, {"triangles", c.tris.size() / 9},
                        {"unreadable", c.unreadable}, {"bounds", Bounds(c.tris)}, {"by_texture", per}, {"first", sample}};
            if (c.unreadable && !g_dipWhy.empty()) c.result["why"] = g_dipWhy;
        }
    }
    // the mods' done ones go; the bridge's last four done are kept
    std::vector<std::unique_ptr<Capture>> kept;
    unsigned bridgeDone = 0;
    for (auto it = g_captures.rbegin(); it != g_captures.rend(); ++it) {
        Capture &c = **it;
        if (c.done && c.mod) continue;
        if (c.done && !c.mod && ++bridgeDone > 4) continue;
        kept.insert(kept.begin(), std::move(*it));
    }
    g_captures.swap(kept);
}

bool GeomMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.rfind("geom.", 0) != 0) return false;
    *ok = true;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto fail = [&](const std::string &why) { *ok = false; *reply = why; return true; };
    if (method == "geom.hide") {
        std::string where = p.value("where", "all"), err;
        int w = where == "frame" ? MML_GEOM_FRAME : where == "views" ? MML_GEOM_VIEWS : MML_GEOM_FRAME | MML_GEOM_VIEWS;
        int h = AddRule(nullptr, p.value("pattern", "").c_str(), w, &err);
        if (!h) return fail("geom.hide: " + err);
        *reply = json({{"handle", h}, {"pattern", Lower(p.value("pattern", "").c_str())}, {"where", where}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "geom.show") {
        RemoveRules(nullptr, p.value("handle", 0));
        *reply = json({{"rules", g_rules.size()}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "geom.survey") {
        int id = p.value("id", 0);
        if (!id) {
            if (!EnsureHooks()) return fail("geom.survey: " + g_hookWhy);
            if (!g_surveyWanted) { g_surveyWanted = g_nextId++; g_seen.clear(); }
            *reply = json({{"id", g_surveyWanted}, {"done", false}, {"next", "geom.survey {id} after a frame of the mission's world"}}).dump(-1, ' ', false, json::error_handler_t::replace);
            return true;
        }
        for (const SurveyDone &s : g_surveys)
            if (s.id == id) {
                json r = s.result;
                std::string filter = Lower(p.value("filter", "").c_str());
                if (!filter.empty()) {
                    json kept = json::array();
                    for (auto &t : r["textures"]) if (Glob(filter.c_str(), Lower(t["texture"].get<std::string>().c_str()).c_str())) kept.push_back(t);
                    r["textures"] = kept;
                }
                size_t limit = p.value("limit", 60);
                if (r["textures"].size() > limit) { r["more"] = r["textures"].size() - limit; r["textures"].erase(r["textures"].begin() + limit, r["textures"].end()); }
                r["id"] = id;
                r["done"] = true;
                *reply = r.dump(-1, ' ', false, json::error_handler_t::replace);
                return true;
            }
        *reply = json({{"id", id}, {"done", false}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "geom.capture") {
        int id = p.value("id", 0);
        if (!id) {
            std::string err;
            std::string pat = p.value("pattern", "");
            if (Lower(pat.c_str()).empty()) return fail("geom.capture: an empty pattern");
            if (!EnsureHooks()) return fail("geom.capture: " + g_hookWhy);
            int cid = g_nextId++;
            g_captures.push_back(std::unique_ptr<Capture>(new Capture{nullptr, cid, Lower(pat.c_str()), nullptr, nullptr}));
            *reply = json({{"id", cid}, {"done", false}, {"next", "geom.capture {id} after a frame of the mission's world"}}).dump(-1, ' ', false, json::error_handler_t::replace);
            return true;
        }
        for (const auto &c : g_captures)
            if (!c->mod && c->id == id) {
                *reply = c->done ? c->result.dump(-1, ' ', false, json::error_handler_t::replace) : json({{"id", id}, {"done", false}}).dump(-1, ' ', false, json::error_handler_t::replace);
                return true;
            }
        return fail("geom.capture: no such capture");
    }
    if (method == "geom.state") {
        json rules = json::array();
        for (const Rule &r : g_rules)
            rules.push_back({{"owner", ModIdOf(r.mod)}, {"handle", r.handle}, {"pattern", r.pattern},
                             {"where", r.where == MML_GEOM_FRAME ? "frame" : r.where == MML_GEOM_VIEWS ? "views" : "all"},
                             {"groups_last_frame", r.groupsLast}, {"faces_last_frame", r.facesLast}});
        json out = {{"hooked", g_hooked}, {"capture_hooked", g_dipHooked}, {"frames", g_frames}, {"rules", rules},
                    {"faults", g_faults}, {"capture_fails", g_dipFails}};
        if (!g_hooked && g_hookTried) out["why"] = g_hookWhy;
        if (!g_dipWhy.empty()) out["capture_why"] = g_dipWhy;
        *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    return fail("unknown geom method (geom.hide, geom.show, geom.state, geom.survey, geom.capture)");
}

} // namespace host
