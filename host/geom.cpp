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
#include "d3dmirror.h"
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// The layouts (the same in 1.2 and 1.0: game_build.h fgRender*).
constexpr int kEntryVisual = 0x00, kEntryGroup = 0x10;
constexpr int kGroupMaterial = 0x04, kGroupFaces = 0x08;
constexpr int kMaterialFlags = 0x08, kMaterialTexture = 0x0C;
constexpr int kTextureName = 0x1C;
constexpr int kVisualType = 0x1D4;
constexpr int kFrameName = 0x100;   // (game.h: an I3D_frame's name, a visual's too)
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
    std::string pattern;   // lower case, without its "frame:"
    int where;             // MML_GEOM_*
    bool byFrame;          // "frame:<pattern>": the visual's frame's name, not its texture's (any group of it, a texture
                           // or none: the water's model's flat shadows under the bridges have none)
    unsigned groupsNow = 0, facesNow = 0, groupsLast = 0, facesLast = 0;
};
std::vector<Rule> g_rules;
int g_nextHandle = 1;

// What each texture, and each visual by its frame's name, matched this frame (a bit a rule); cleared at each frame and
// when the rules change.
std::unordered_map<const void *, uint64_t> g_match, g_frameMatch;
uint64_t g_textureRules, g_frameRules, g_mainRules;

void RefreshRuleMatches() {
    g_textureRules = g_frameRules = g_mainRules = 0;
    for (size_t i = 0; i < g_rules.size(); ++i) {
        const uint64_t bit = 1ull << i;
        if (g_rules[i].byFrame) g_frameRules |= bit;
        else g_textureRules |= bit;
        if (g_rules[i].where & MML_GEOM_FRAME) g_mainRules |= bit;
    }
    g_match.clear();
    g_frameMatch.clear();
}

// ---- one frame's state
bool g_inFrame;        // the frame's own Render (not a scene view's) is drawing
bool g_frameOpen;      // between GeomFrameBegin and GeomFrameEnd (the views, the frame, the passes)
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
    bool armed = true;     // waiting for its frame (false: asked during a frame, armed by the next one's beginning)
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
struct DrawObserver { MmlMod *mod;MmlSceneDrawFn fn;void *user; };
std::vector<DrawObserver> g_drawObservers;
uintptr_t g_drawLs3dBase;
struct DrawSource { void *visual;const void *group;unsigned kind,pass=0; };
DrawSource *g_drawSource;
const MmlSceneDraw *g_activeDraw;
MmlMod *g_activeObserver;
IDirect3DDevice9 *g_activeDevice;
const MmlSceneMaterialDetail *g_activeMaterial;
std::vector<MmlSceneVertex> g_sourceVertices;
std::vector<uint32_t> g_sourceIndices;
std::vector<uint32_t> g_sourceVertexOffsets;
alignas(64) uint8_t g_sourceVertexStage[64*1024];
struct SourceBytes {
    std::vector<uint8_t> indexBytes, vertexBytes;
    std::vector<MmlSceneVertex> vertices;
    std::vector<uint32_t> indices,vertexOffsets;
    uint32_t fvf=0,stride=0,indexStride=0,minimum=0,maximum=0,vertexSpan=0;
    uint32_t indexOffset=0,vertexOffset=0;
    uint64_t indexRevision=0,vertexRevision=0;
    uint64_t revision=0,seen=0;
    size_t Bytes() const {
        return sizeof(SourceBytes)+indexBytes.capacity()+vertexBytes.capacity()+
            vertices.capacity()*sizeof(MmlSceneVertex)+(indices.capacity()+vertexOffsets.capacity())*sizeof(uint32_t);
    }
};
struct SourceKey {
    uintptr_t visual,group;
    uint32_t pass;
    bool operator==(const SourceKey &other) const {
        return visual==other.visual&&group==other.group&&pass==other.pass;
    }
};
struct SourceKeyHash {
    size_t operator()(const SourceKey &key) const {
        return (key.visual>>4)*2654435761u^(key.group>>2)*2246822519u^key.pass;
    }
};
std::unordered_map<SourceKey,SourceBytes,SourceKeyHash> g_sourceCache;
size_t SourceCacheBudget() {
    static const size_t budget=[] {
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof memory;
        const bool large=GlobalMemoryStatusEx(&memory)&&memory.ullTotalVirtual>=3ull*1024*1024*1024;
        return (large?96u:32u)*1024u*1024u;
    }();
    return budget;
}
size_t g_sourceCacheBytes;
uint64_t g_sourceCacheHits,g_sourceCacheNew,g_sourceLastCollect,g_sourceShadowReads;
uint64_t g_sourceIndexProofs,g_sourceNativeIndexProofs,g_sourceVertexProofs;
uint64_t g_sourceSparseDraws,g_sourceSkippedVertices,g_sourceDecodedVertices,g_sourceShadowVertices;
uint64_t g_sourceBindingReads;
uint64_t g_sourceRevision;
uint64_t g_sourceDiskSkipped;
const SourceBytes *g_sourceCached;
bool g_sourceAttempted,g_sourceValid;
uint32_t g_sourceFvf;
unsigned g_sourceStep,g_sourceBadIndex,g_sourceStride,g_sourceVbBytes,g_sourceIbBytes,g_sourceFailureLog;
HRESULT g_sourceReadHr;
bool g_collectStage;
MmlMod *g_collectOwner;
unsigned g_collectDraws;
uint32_t g_collectFlags;
std::map<MmlMod *,std::vector<MmlSceneMissing>> g_missingSources;
unsigned g_dipFails;
std::string g_dipWhy;

struct SourceTimingRow {
    uint64_t selected=0,groups=0,draws=0,copies=0;
    uint64_t cachedCopies=0,vertexProofs=0,nativeIndexProofs=0;
    uint64_t prepare=0,replay=0,observe=0,callback=0,geometry=0;
};
bool g_sourceTiming;
int g_sourceTimingRow=-1;
std::array<SourceTimingRow,6> g_sourceTimings;
uint64_t g_sourceTimingCollections;
uint64_t g_sourceTimingPrune,g_sourceTimingSetup,g_sourceTimingWalk,g_sourceTimingRestore;
uint64_t SourceCounter(){LARGE_INTEGER now{};QueryPerformanceCounter(&now);return uint64_t(now.QuadPart);}
int SourceTypeRow(unsigned type){
    switch(type){
    case 0x5F4A424Fu:return 0; // OBJ_
    case 0x50414D4Cu:return 1; // LMAP
    case 0x4850524Du:return 2; // MRPH
    case 0x4D474E53u:return 3; // SNGM
    case 0x524D4D53u:return 4; // SMMR
    default:return 5;         // BBRD
    }
}
class SourceTimer {
    uint64_t *ticks_=nullptr,start_=0;
    SourceTimingRow *geometry_=nullptr;
    uint64_t hits_=0,vertices_=0,indices_=0;
public:
    explicit SourceTimer(uint64_t &ticks){if(g_sourceTiming){ticks_=&ticks;start_=SourceCounter();}}
    explicit SourceTimer(uint64_t SourceTimingRow::*field){
        if(g_sourceTiming&&g_sourceTimingRow>=0){
            auto &row=g_sourceTimings[g_sourceTimingRow];ticks_=&(row.*field);start_=SourceCounter();
            if(field==&SourceTimingRow::geometry){
                geometry_=&row;hits_=g_sourceCacheHits;vertices_=g_sourceVertexProofs;indices_=g_sourceNativeIndexProofs;
            }
        }
    }
    ~SourceTimer(){
        if(ticks_)*ticks_+=SourceCounter()-start_;
        if(geometry_){
            geometry_->cachedCopies+=g_sourceCacheHits-hits_;
            geometry_->vertexProofs+=g_sourceVertexProofs-vertices_;
            geometry_->nativeIndexProofs+=g_sourceNativeIndexProofs-indices_;
        }
    }
};

std::string Lower(const char *s) {
    std::string o;
    for (; s && *s; s++) o += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return o;
}

// '*' any run, '?' one character. Fold borrowed names only as they are compared.
template<size_t limit = SIZE_MAX, bool fold = false>
bool Glob(const char *p, const char *s) {
    const char *star = nullptr;
    size_t at = 0, after = 0;
    while (at < limit && s[at]) {
        const char c = fold ? static_cast<char>(std::tolower(static_cast<unsigned char>(s[at]))) : s[at];
        if (*p == '?' || *p == c) { ++p; ++at; continue; }
        if (*p == '*') { star = p++; after = at; continue; }
        if (star) { p = star + 1; at = ++after; continue; }
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
    if (!tex || !g_textureRules) return 0;
    auto it = g_match.find(tex);
    if (it != g_match.end()) return it->second;
    uint64_t m = 0;
    const char *name = TextureName(tex);
    if (name)
        for (size_t i = 0; i < g_rules.size() && i < kMaxRules; i++)
            if (!g_rules[i].byFrame && Glob<SIZE_MAX, true>(g_rules[i].pattern.c_str(), name)) m |= 1ull << i;
    g_match[tex] = m;
    return m;
}

uint64_t FrameMatchOf(const void *visual) {
    if (!g_frameRules) return 0;
    auto it = g_frameMatch.find(visual);
    if (it != g_frameMatch.end()) return it->second;
    uint64_t m = 0;
    const char *name = At<const char *>(visual, kFrameName);
    if (name && *name)
        for (size_t i = 0; i < g_rules.size() && i < kMaxRules; i++)
            if (g_rules[i].byFrame && Glob<128, true>(g_rules[i].pattern.c_str(), name)) m |= 1ull << i;
    g_frameMatch[visual] = m;
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

bool DrawBackdrop() {
    if(!game::g_ls3d)return false;const auto &l=game::Ls3d();
    const auto *camera=*reinterpret_cast<const uint8_t *const *>(g_drawLs3dBase+l.currentCamera);
    // Native Render selects this backdrop matrix pointer in both builds; numerical equality at origin is ambiguous.
    return camera&&*reinterpret_cast<const void *const *>(g_drawLs3dBase+l.viewProj)==camera+0x324;
}
struct ObserverCall { DrawObserver observer;const MmlSceneDraw *draw; };
void CallObserver(void *p){auto &call=*static_cast<ObserverCall *>(p);call.observer.fn(call.draw,call.observer.user);}
void ObserveDraw(IDirect3DDevice9 *device,D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT vertices,UINT start,UINT primitives){
    if(g_activeDraw||g_drawObservers.empty()||!g_drawSource||ShadowViewNow()||(!g_inFrame&&!ViewDrawingNow()))return;
    SourceTimer timer(&SourceTimingRow::observe);
    if(g_sourceTiming&&g_sourceTimingRow>=0)g_sourceTimings[g_sourceTimingRow].draws++;
    const D3dMirror *mirror=MirrorOf(device);if(!mirror)return;
    MmlSceneDraw draw{};draw.size=sizeof draw;draw.frame=Frames();draw.visual=g_drawSource->visual;draw.group=g_drawSource->group;
    draw.view=ViewDrawingNow();draw.kind=g_drawSource->kind;draw.pass=g_drawSource->pass++;
    if(g_collectOwner){draw.flags|=MML_SCENE_DRAW_COLLECTION;g_collectDraws++;}
    draw.primitive_type=type;draw.primitive_count=primitives;draw.base_vertex=base;draw.min_vertex=minimum;draw.vertex_count=vertices;draw.start_index=start;
    memcpy(draw.world,&mirror->world,sizeof draw.world);
    if(DrawBackdrop())draw.flags|=MML_SCENE_DRAW_BACKDROP;
    if(ShadersWanted()||mirror->vs)draw.flags|=MML_SCENE_DRAW_SHADER_RULES;
    if(mirror->rs[D3DRS_LIGHTING])draw.flags|=MML_SCENE_DRAW_FIXED_LIGHTING;
    SkinGpuDraw skin{};if(SkinGpuFor(device,draw.visual,base,&skin))draw.flags|=MML_SCENE_DRAW_GPU_SKIN;
    draw.alpha_test=mirror->rs[D3DRS_ALPHATESTENABLE];draw.alpha_reference=mirror->rs[D3DRS_ALPHAREF];draw.alpha_function=mirror->rs[D3DRS_ALPHAFUNC];
    draw.cull=mirror->rs[D3DRS_CULLMODE];draw.blend=mirror->rs[D3DRS_ALPHABLENDENABLE];draw.source_blend=mirror->rs[D3DRS_SRCBLEND];draw.destination_blend=mirror->rs[D3DRS_DESTBLEND];
    draw.texture_factor=mirror->rs[D3DRS_TEXTUREFACTOR];draw.stage_stride=sizeof(MmlSceneTextureStage);
    MmlSceneTextureStage stages[2]{};draw.stages=stages;
    MmlSceneMaterialDetail detail{};
    if(mirror->nativeMaterialKnown){
        detail.stage2_color_op=mirror->tss[2][D3DTSS_COLOROP];
        detail.blend_op=mirror->rs[D3DRS_BLENDOP];detail.color_write=mirror->rs[D3DRS_COLORWRITEENABLE];
        g_activeMaterial=&detail;
    }
    for(unsigned stage=0;stage<2;stage++){
        auto &s=stages[stage];const auto *t=mirror->tss[stage],*a=mirror->samp[stage];s.texture=mirror->tex[stage];
        s.color_op=t[D3DTSS_COLOROP];s.color_arg0=t[D3DTSS_COLORARG0];s.color_arg1=t[D3DTSS_COLORARG1];s.color_arg2=t[D3DTSS_COLORARG2];
        s.alpha_op=t[D3DTSS_ALPHAOP];s.alpha_arg0=t[D3DTSS_ALPHAARG0];s.alpha_arg1=t[D3DTSS_ALPHAARG1];s.alpha_arg2=t[D3DTSS_ALPHAARG2];
        s.coordinate=t[D3DTSS_TEXCOORDINDEX];s.transform_flags=t[D3DTSS_TEXTURETRANSFORMFLAGS];
        s.address_u=a[D3DSAMP_ADDRESSU];s.address_v=a[D3DSAMP_ADDRESSV];s.min_filter=a[D3DSAMP_MINFILTER];s.mag_filter=a[D3DSAMP_MAGFILTER];s.mip_filter=a[D3DSAMP_MIPFILTER];
        if(g_activeMaterial)detail.stages[stage]={a[D3DSAMP_SRGBTEXTURE],t[D3DTSS_RESULTARG],a[D3DSAMP_MAXANISOTROPY],
            a[D3DSAMP_MAXMIPLEVEL],a[D3DSAMP_MIPMAPLODBIAS]};
    }
    // Snapshot registrations: a callback may release/replace its observer without invalidating this iteration.
    DrawObserver observers[64];const size_t count=(std::min)(g_drawObservers.size(),size_t(64));
    std::copy_n(g_drawObservers.begin(),count,observers);g_activeDraw=&draw;g_activeDevice=device;
    g_sourceAttempted=g_sourceValid=false;
    for(size_t i=0;i<count;i++){
        if(g_collectOwner&&observers[i].mod!=g_collectOwner)continue;
        g_activeObserver=observers[i].mod;ObserverCall call{observers[i],&draw};
        SourceTimer callback(&SourceTimingRow::callback);
        ModCall(observers[i].mod,CallObserver,&call,kSceneDrawObserverSite);
    }
    g_activeObserver=nullptr;g_activeDraw=nullptr;g_activeDevice=nullptr;g_activeMaterial=nullptr;g_sourceCached=nullptr;
    if(g_drawObservers.empty()){
        std::vector<MmlSceneVertex>().swap(g_sourceVertices);std::vector<uint32_t>().swap(g_sourceIndices);
        std::vector<uint32_t>().swap(g_sourceVertexOffsets);
    }
}
HRESULT DrawOn(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n) {
    HRESULT hr;
    ObserveDraw(d,t,base,minIndex,verts,start,n);
    if(g_collectOwner)return D3D_OK; // source collection never writes color/depth/motion or invokes shader rules
    BatchDip(d, t, base, minIndex, verts, start, n);   // (batch.cpp: a capture's draw read; the batches before a blended one)
    if (ShadersDraw(d, t, base, minIndex, verts, start, n, reinterpret_cast<void *>(oDip), &hr)) return hr;   // (shaders.cpp)
    MirrorGiveBack(d);   // (a draw no rule takes: the game's shaders, if a rule's were lent)
    return oDip(d, t, base, minIndex, verts, start, n);
}

HRESULT STDMETHODCALLTYPE hkDip(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n) {
    if(g_collectOwner)return DrawOn(d,t,base,minIndex,verts,start,n);
    if (BatchDrawing()) return oDip(d, t, base, minIndex, verts, start, n);   // (batch.cpp's own: no group's)
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
    if (!tex && !g_frameRules) return false;
    int view = ViewDrawingNow();
    // A shadow camera is an auxiliary view too. Otherwise hidden replacement water and the game's old flat
    // bridge-shade meshes reappear as casters, projecting a second, baked-looking shadow onto the new water.
    int bit = (view || ShadowViewNow()) ? MML_GEOM_VIEWS : (g_inFrame ? MML_GEOM_FRAME : 0);
    if (!bit) return false;   // (a menu's scene, or a drawing not the world's)
    uint64_t m = g_rules.empty() ? 0 : (tex ? MatchOf(tex) : 0) | FrameMatchOf(visual);
    bool hide = false;
    unsigned faces = At<uint32_t>(group, kGroupFaces);
    for (size_t i = 0; m && i < g_rules.size() && i < kMaxRules; i++)
        if ((m >> i & 1) && (g_rules[i].where & bit)) {
            hide = true;
            g_rules[i].groupsNow++;
            g_rules[i].facesNow += faces;
        }
    if (!tex) return hide;   // (the survey and the captures are by texture)
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
    const bool shaders = !g_collectOwner&&ShadersWanted(), motion = !g_collectOwner&&MotionWanted();
    const bool observe=!g_drawObservers.empty();
    if (!shaders && !motion && !observe) { (*orig)(visual, edx, entry); return; }
    DrawSource source{visual,At<const void *>(entry,kEntryGroup),kind,0};
    DrawSource *previous=g_drawSource;if(observe)g_drawSource=&source;
    int view = ViewDrawingNow();
    if (motion) MotionGroupBegin(kind, visual, At<uint8_t *>(entry, kEntryGroup), !view && g_inFrame);
    if (shaders) ShadersGroupBegin(kind, visual, entry, view ? MML_GEOM_VIEWS : (g_inFrame ? MML_GEOM_FRAME : 0));
    {
        SourceTimer replay(&SourceTimingRow::replay);
        if(g_sourceTiming&&g_sourceTimingRow>=0)g_sourceTimings[g_sourceTimingRow].groups++;
        (*orig)(visual, edx, entry);
    }
    g_drawSource=previous;
    if (shaders) ShadersGroupEnd();
    if (motion) MotionGroupEnd();
}

template <RenderFgFn *orig, int kind>
void HkRenderFg(void *visual, void *edx, uint8_t *entry);
template <RenderFgFn *orig, int kind>
void __fastcall hkRenderFg(void *visual, void *edx, uint8_t *entry) {
    BatchEntry(entry, g_inFrame);   // (batch.cpp: its place in LS3D's order, for the batches asked for before)
    if constexpr (kind == 1) {   // (an object's group: batch.cpp takes it, or captures it while LS3D draws it)
        const int b = BatchGroup(visual, entry, g_inFrame);
        if (b == 1) return;
        if (b == 2) { HkRenderFg<orig, kind>(visual, edx, entry); BatchCaptured(); return; }
    }
    HkRenderFg<orig, kind>(visual, edx, entry);
}
template <RenderFgFn *orig, int kind>
void HkRenderFg(void *visual, void *edx, uint8_t *entry) {
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
    if (ShadowViewNow()) return;   // an already projected game shadow is not geometry that blocks the sun
    BatchEntry(entry, g_inFrame);
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
    bool byFrame = p.compare(0, 6, "frame:") == 0;
    if (byFrame) p.erase(0, 6);
    if (p.empty()) { *err = "an empty pattern"; return 0; }
    if (g_rules.size() >= kMaxRules) { *err = "64 rules already"; return 0; }
    if (!EnsureHooks()) { *err = g_hookWhy; return 0; }
    if (!(where & (MML_GEOM_FRAME | MML_GEOM_VIEWS))) where = MML_GEOM_FRAME | MML_GEOM_VIEWS;
    Rule r{mod, g_nextHandle++, p, where & (MML_GEOM_FRAME | MML_GEOM_VIEWS), byFrame};
    g_rules.push_back(r);
    RefreshRuleMatches();
    return r.handle;
}

void RemoveRules(MmlMod *mod, int handle) {
    size_t before = g_rules.size();
    g_rules.erase(std::remove_if(g_rules.begin(), g_rules.end(), [&](const Rule &r) { return r.mod == mod && (handle == 0 || r.handle == handle); }),
                  g_rules.end());
    if (g_rules.size() != before) RefreshRuleMatches();
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

struct VertexLock { IDirect3DVertexBuffer9 *buffer;bool locked=false;~VertexLock(){if(locked)buffer->Unlock();} };
struct IndexLock { IDirect3DIndexBuffer9 *buffer;bool locked=false;~IndexLock(){if(locked)buffer->Unlock();} };
bool CopySceneGeometry(const MmlSceneDraw &draw){
    SourceTimer timer(&SourceTimingRow::geometry);
    if(g_sourceTiming&&g_sourceTimingRow>=0)g_sourceTimings[g_sourceTimingRow].copies++;
    using Microsoft::WRL::ComPtr;
    g_sourceCached=nullptr;
    g_sourceStep=0;g_sourceBadIndex=g_sourceStride=g_sourceVbBytes=g_sourceIbBytes=0;g_sourceReadHr=S_OK;
    if(draw.primitive_type!=D3DPT_TRIANGLELIST||!draw.primitive_count||draw.primitive_count>1000000||
       (draw.flags&(MML_SCENE_DRAW_GPU_SKIN|MML_SCENE_DRAW_SHADER_RULES)))return false;
    IDirect3DDevice9 *d=g_activeDevice;DWORD fvf=0;UINT offset=0,stride=0;
    ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;
    g_sourceStep=1;
    const HRESULT format=g_collectOwner?MirrorSourceFvf(d,&fvf):d->GetFVF(&fvf);
    if(FAILED(format)||(fvf&D3DFVF_POSITION_MASK)!=D3DFVF_XYZ)return false;
    VbSourceStreams streams{};
    if(g_collectOwner)VbRenameSourceStreams(d,nullptr,nullptr,&streams);
    if(streams.vertices||streams.indices)g_sourceBindingReads++;
    if(streams.vertices){offset=streams.vertexOffset;stride=streams.stride;}
    else if(FAILED(d->GetStreamSource(0,&vb,&offset,&stride))||!vb)return false;
    g_sourceFvf=fvf;g_sourceStride=stride;g_sourceStep=2;
    const unsigned textures=(fvf&D3DFVF_TEXCOUNT_MASK)>>D3DFVF_TEXCOUNT_SHIFT;
    if(textures>2)return false;
    for(unsigned stage=0;stage<textures;stage++)if(((fvf>>(16+stage*2))&3)!=0)return false;
    unsigned attribute=12;if(fvf&D3DFVF_NORMAL)attribute+=12;if(fvf&D3DFVF_PSIZE)attribute+=4;
    const unsigned colorOffset=attribute;if(fvf&D3DFVF_DIFFUSE)attribute+=4;
    if(fvf&D3DFVF_SPECULAR)attribute+=4;
    g_sourceStep=3;const unsigned uvOffset=attribute;if(stride<attribute+textures*8)return false;

    const SourceKey key{reinterpret_cast<uintptr_t>(draw.visual),reinterpret_cast<uintptr_t>(draw.group),draw.pass};
    auto found=g_collectOwner?g_sourceCache.find(key):g_sourceCache.end();
    SourceBytes *prior=found==g_sourceCache.end()?nullptr:&found->second;
    const uint32_t count=draw.primitive_count*3;
    const auto sameRange=[&](uint32_t indexStride){
        return prior&&prior->indexStride==indexStride&&prior->indices.size()==count&&
            prior->minimum>=draw.min_vertex&&uint64_t(prior->maximum)<uint64_t(draw.min_vertex)+draw.vertex_count;
    };
    const bool nativeProof=streams.nativeIndexRevision&&sameRange(streams.indexStride)&&
        prior->indexRevision==streams.nativeIndexRevision&&prior->indexOffset==uint64_t(draw.start_index)*streams.indexStride;
    if(!streams.indices&&!nativeProof){
        streams.nativeIndexRevision=0;
        if(FAILED(d->GetIndices(&ib))||!ib)return false;
    }
    if(g_collectOwner&&(vb||ib))VbRenameSourceStreams(d,vb.Get(),ib.Get(),&streams);
    if(streams.vertices||streams.indices)g_sourceShadowReads++;

    D3DINDEXBUFFER_DESC id{};D3DVERTEXBUFFER_DESC vd{};
    g_sourceStep=4;
    if(streams.vertices)vd.Size=streams.vertexBytes;
    else if(FAILED(vb->GetDesc(&vd)))return false;
    if(streams.indices||streams.nativeIndexRevision){id.Size=streams.indexBytes;id.Format=streams.indexStride==2?D3DFMT_INDEX16:D3DFMT_INDEX32;}
    else if(FAILED(ib->GetDesc(&id))||(id.Format!=D3DFMT_INDEX16&&id.Format!=D3DFMT_INDEX32))return false;
    g_sourceVbBytes=vd.Size;g_sourceIbBytes=id.Size;g_sourceStep=5;
    const uint32_t indexBytes=id.Format==D3DFMT_INDEX16?2:4;
    if((uint64_t(draw.start_index)+count)*indexBytes>id.Size)return false;
    const uint32_t indexOffset=draw.start_index*indexBytes;
    const uint8_t *indices=nullptr;IndexLock indexLock{ib.Get()};
    if(streams.indices)indices=streams.indices+indexOffset;
    else if(!nativeProof){
        void *locked=nullptr;g_sourceStep=6;g_sourceReadHr=ib->Lock(indexOffset,count*indexBytes,&locked,D3DLOCK_READONLY);
        if(FAILED(g_sourceReadHr)||!locked)return false;
        indexLock.locked=true;indices=static_cast<const uint8_t *>(locked);
    }
    const size_t indexSize=size_t(count)*indexBytes;
    const uint64_t indexRevision=streams.nativeIndexRevision?streams.nativeIndexRevision:
        VbRenameSourceRevision(streams.indexRevisions,indexOffset,indexSize);
    const bool indexProof=prior&&indexRevision&&prior->indexRevision==indexRevision&&prior->indexOffset==indexOffset;
    const bool sameIndices=sameRange(indexBytes)&&
        (indexProof||(prior->indexBytes.size()==indexSize&&memcmp(prior->indexBytes.data(),indices,indexSize)==0));
    if(sameIndices&&indexProof)g_sourceIndexProofs++;
    if(nativeProof)g_sourceNativeIndexProofs++;
    uint32_t minimum=UINT32_MAX,maximum=0;
    g_sourceStep=7;
    if(sameIndices){minimum=prior->minimum;maximum=prior->maximum;}
    else{
      g_sourceIndices.resize(count);
      for(uint32_t i=0;i<count;i++){
        uint32_t index=indexBytes==2?reinterpret_cast<const uint16_t *>(indices)[i]:reinterpret_cast<const uint32_t *>(indices)[i];
        g_sourceBadIndex=index;
        if(index<draw.min_vertex||uint64_t(index)>=uint64_t(draw.min_vertex)+draw.vertex_count)return false;
        minimum=std::min(minimum,index);maximum=std::max(maximum,index);g_sourceIndices[i]=index;
      }
    }
    g_sourceStep=8;const int64_t first=int64_t(draw.base_vertex)+minimum;const uint64_t vertexCount=uint64_t(maximum)-minimum+1;
    if(first<0||vertexCount>1000000||uint64_t(offset)+(uint64_t(first)+vertexCount)*stride>vd.Size)return false;
    const uint8_t *vertices=nullptr;VertexLock vertexLock{vb.Get()};
    const UINT begin=offset+static_cast<UINT>(first)*stride;
    if(streams.vertices)vertices=streams.vertices+begin;
    else{
        void *locked=nullptr;g_sourceStep=9;g_sourceReadHr=vb->Lock(begin,static_cast<UINT>(vertexCount)*stride,&locked,D3DLOCK_READONLY);
        if(FAILED(g_sourceReadHr)||!locked)return false;
        vertexLock.locked=true;vertices=static_cast<const uint8_t *>(locked);
    }
    const size_t vertexSize=size_t(vertexCount)*stride;
    const bool sparse=g_collectOwner&&vertexCount>uint64_t(count)*2;
    // A native read-only lock may expose write-combined driver memory. Read
    // it in bounded contiguous runs before inspecting individual attributes.
    const bool stage=g_collectOwner&&!streams.vertices&&stride<=sizeof g_sourceVertexStage;
    const bool stageWhole=stage&&!sparse&&vertexSize<=sizeof g_sourceVertexStage;
    if(stageWhole){memcpy(g_sourceVertexStage,vertices,vertexSize);vertices=g_sourceVertexStage;}
    const uint64_t vertexRevision=VbRenameSourceRevision(streams.vertexRevisions,begin,vertexSize);
    const bool vertexProof=prior&&vertexRevision&&prior->vertexRevision==vertexRevision&&prior->vertexOffset==begin;
    const bool sameLayout=sameIndices&&prior->fvf==fvf&&prior->stride==stride&&prior->vertexSpan==vertexCount;
    const auto reuse=[&]{
        if(vertexProof)g_sourceVertexProofs++;
        // Static ranges that have already passed a producer-revision check
        // need no duplicate raw bytes. A changed revision decodes them anew.
        if(indexProof&&!prior->indexBytes.empty()){
            g_sourceCacheBytes-=prior->indexBytes.capacity();
            std::vector<uint8_t>().swap(prior->indexBytes);
        }
        if(vertexProof&&!prior->vertexBytes.empty()){
            g_sourceCacheBytes-=prior->vertexBytes.capacity();
            std::vector<uint8_t>().swap(prior->vertexBytes);
        }
        prior->indexRevision=indexRevision;prior->indexOffset=indexOffset;
        prior->vertexRevision=vertexRevision;prior->vertexOffset=begin;
        prior->seen=draw.frame;g_sourceCached=prior;g_sourceCacheHits++;
    };
    if(sameLayout&&(vertexProof||(prior->vertexBytes.size()==vertexSize&&memcmp(prior->vertexBytes.data(),vertices,vertexSize)==0))){
        reuse();return true;
    }
    const std::vector<uint32_t> *offsets=nullptr;
    // Native groups can reference a handful of vertices scattered across a
    // pooled buffer. Keep index order and decode only the vertices they use.
    if(sparse){
        if(sameIndices)offsets=&prior->vertexOffsets;
        else{
            g_sourceVertexOffsets=g_sourceIndices;
            std::sort(g_sourceVertexOffsets.begin(),g_sourceVertexOffsets.end());
            g_sourceVertexOffsets.erase(std::unique(g_sourceVertexOffsets.begin(),g_sourceVertexOffsets.end()),g_sourceVertexOffsets.end());
            for(auto &index:g_sourceIndices)index=uint32_t(std::lower_bound(g_sourceVertexOffsets.begin(),g_sourceVertexOffsets.end(),index)-g_sourceVertexOffsets.begin());
            for(auto &index:g_sourceVertexOffsets)index-=minimum;
            offsets=&g_sourceVertexOffsets;
        }
        g_sourceSparseDraws++;g_sourceSkippedVertices+=vertexCount-offsets->size();
    }
    const size_t decodedCount=offsets?offsets->size():size_t(vertexCount);
    if(g_collectOwner){g_sourceDecodedVertices+=decodedCount;if(streams.vertices)g_sourceShadowVertices+=decodedCount;}
    g_sourceVertices.resize(decodedCount);
    size_t stagedFirst=0,stagedEnd=0;
    g_sourceStep=10;for(size_t i=0;i<decodedCount;i++){
        const size_t index=offsets?(*offsets)[i]:i;
        const auto *source=vertices+index*stride;
        if(stage&&!stageWhole){
            if(index>=stagedEnd){
                stagedFirst=index;stagedEnd=std::min(size_t(vertexCount),index+sizeof g_sourceVertexStage/stride);
                if(offsets){
                    size_t last=i+1;
                    while(last<decodedCount&&(*offsets)[last]<stagedEnd&&(*offsets)[last]-(*offsets)[last-1]<=2)last++;
                    stagedEnd=size_t((*offsets)[last-1])+1;
                }
                memcpy(g_sourceVertexStage,source,(stagedEnd-stagedFirst)*stride);
            }
            source=g_sourceVertexStage+(index-stagedFirst)*stride;
        }
        // Inspect the IEEE float exponent without taking a trip through x87
        // and the CRT per coordinate. Preserve the original float bytes.
        uint32_t position[3];memcpy(position,source,sizeof position);
        constexpr uint32_t exponent=0x7f800000;
        if((position[0]&exponent)==exponent||(position[1]&exponent)==exponent||(position[2]&exponent)==exponent)return false;
        auto &vertex=g_sourceVertices[i];memcpy(vertex.pos,position,sizeof position);
        vertex.color=0xffffffff;if(fvf&D3DFVF_DIFFUSE)memcpy(&vertex.color,source+colorOffset,4);
        if(textures)memcpy(vertex.uv0,source+uvOffset,8);else memset(vertex.uv0,0,8);
        if(textures>1)memcpy(vertex.uv1,source+uvOffset+8,8);else memcpy(vertex.uv1,vertex.uv0,8);
    }
    if(offsets&&sameLayout&&prior->vertices.size()==decodedCount&&
       memcmp(prior->vertices.data(),g_sourceVertices.data(),decodedCount*sizeof(MmlSceneVertex))==0){
        reuse();return true;
    }
    if(sameIndices)g_sourceIndices=prior->indices;
    else if(!offsets)for(auto &index:g_sourceIndices)index-=minimum;
    if(g_collectOwner){
        const size_t previous=prior?prior->Bytes():0;
        const size_t required=sizeof(SourceBytes)+(indices?indexSize:0)+(offsets?offsets->size()*sizeof(uint32_t):vertexSize)+g_sourceVertices.size()*sizeof(MmlSceneVertex)+
            g_sourceIndices.size()*sizeof(uint32_t);
        const size_t budget=SourceCacheBudget();
        if(required<=budget-(g_sourceCacheBytes-previous))try{
            SourceBytes next;
            if(indices)next.indexBytes.assign(indices,indices+indexSize);
            if(offsets)next.vertexOffsets=*offsets;else next.vertexBytes.assign(vertices,vertices+vertexSize);
            next.vertices=g_sourceVertices;next.indices=g_sourceIndices;
            next.fvf=fvf;next.stride=stride;next.indexStride=indexBytes;next.minimum=minimum;next.maximum=maximum;next.vertexSpan=uint32_t(vertexCount);
            next.indexRevision=indexRevision;next.indexOffset=indexOffset;
            next.vertexRevision=vertexRevision;next.vertexOffset=begin;
            next.revision=++g_sourceRevision;next.seen=draw.frame;
            if(next.Bytes()<=budget-(g_sourceCacheBytes-previous)){
                g_sourceCached=&g_sourceCache.insert_or_assign(key,std::move(next)).first->second;
                g_sourceCacheBytes=g_sourceCacheBytes-previous+g_sourceCached->Bytes();
                g_sourceCacheNew++;
            }
        }catch(const std::bad_alloc &){
            // The current normalized arrays remain usable without a cache entry.
        }
    }
    return true;
}
bool SafeCopySceneGeometry(const MmlSceneDraw &draw){
    __try{return CopySceneGeometry(draw);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

} // namespace

bool SceneDrawsWanted(){return !g_drawObservers.empty();}
bool SceneCollecting(){return g_collectOwner!=nullptr&&GetCurrentThreadId()==MainThread();}
void SceneCollectStage(bool on){g_collectStage=on;}

namespace {
using BoundUpdate=void(__stdcall *)(void *);
bool SafeChildren(void *frame,std::vector<void *> *children){
    __try{game::EnumFrames(frame,children);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool SafeBounds(void *visual,BoundUpdate update,float lo[3],float hi[3]){
    __try{
        if(!(At<uint32_t>(visual,0x1D8)&0x40000u))update(visual);
        memcpy(lo,static_cast<uint8_t *>(visual)+0x184,12);memcpy(hi,static_cast<uint8_t *>(visual)+0x190,12);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool SourceHidden(void *visual,uint8_t *entry){
    if((g_frameRules&g_mainRules) && (FrameMatchOf(visual)&g_mainRules))return true;
    if(!(g_textureRules&g_mainRules))return false;
    const void *group=At<const void *>(entry,kEntryGroup);
    const void *material=group?At<const void *>(group,kGroupMaterial):nullptr;
    const void *texture=material?At<const void *>(material,kMaterialTexture):nullptr;
    return (MatchOf(texture)&g_mainRules)!=0;
}
void PrepareSource(void *visual){
    using Add=void(__fastcall *)(void *,void *);
    auto *vtable=*reinterpret_cast<Add **>(visual);
    vtable[0x68/4](visual,nullptr);
}
void PublishSourcePools(){
    // The same boundary as native Render, including the renamed buffer writes.
    using Unlock=void(__thiscall *)(void *);const auto &ls=game::Ls3d();
    reinterpret_cast<Unlock>(g_drawLs3dBase+ls.sourceVbUnlock)(reinterpret_cast<void *>(g_drawLs3dBase+ls.sourceVbManager));
    reinterpret_cast<Unlock>(g_drawLs3dBase+ls.sourceIbUnlock)(reinterpret_cast<void *>(g_drawLs3dBase+ls.sourceIbManager));
}
bool SafePublishSourcePools(){
    __try{PublishSourcePools();return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReplaySource(void *visual,unsigned kind,uint8_t *helper,int first,int end){
    bool eligible=false;
    DrawSource *previous=g_drawSource;
    __try{
        auto *entries=At<uint8_t *>(helper,0x1C);
        for(int i=first;i<end;i++){
            uint8_t *entry=entries+i*32;
            if(SourceHidden(visual,entry))continue;
            eligible=true;
            if(kind==3)DrawGroup<&oBillboard,3>(visual,nullptr,entry);
            else if(kind==2)DrawGroup<&oLit,2>(visual,nullptr,entry);
            else DrawGroup<&oObject,1>(visual,nullptr,entry);
        }
    }__finally{g_drawSource=previous;}
    return eligible;
}
bool SafePrepareSource(void *visual,unsigned kind,uint8_t *helper,int first,bool *eligible){
    __try{
        PrepareSource(visual);
        PublishSourcePools();
        const int end=At<int>(helper,0x24);
        if(end<first||end>first+100000)return false;
        *eligible=ReplaySource(visual,kind,helper,first,end);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct SourceNativeState {
    uint8_t *helper;uint8_t prefix[0x1C];int used,opaque,transparent;
    uintptr_t properties[19];DWORD cached[19];
    Microsoft::WRL::ComPtr<IDirect3DStateBlock9> state;
    uint16_t fpu;
    bool ready=false,poolWritesPending=false;
    bool Begin(IDirect3DDevice9 *device){
      __try{
        const auto &l=game::Ls3d();const uintptr_t offsets[]={l.propCull,l.propCullMode,l.propFog,l.propFogType,l.propFogEnd,l.propFogStart,l.propFogColor,
            l.propClip,l.propAlpha,l.propBlend,l.propColorOp,l.propAlphaOp,l.propColorOp1,l.propColorOp2,l.propZBias,l.propZCompare,l.propAddressU,l.propAddressV,l.propVertexFormat};
        for(unsigned i=0;i<19;i++){properties[i]=g_drawLs3dBase+offsets[i];cached[i]=*reinterpret_cast<DWORD *>(properties[i]);}
        memcpy(prefix,helper,sizeof prefix);used=At<int>(helper,0x24);opaque=At<int>(helper,0x38);transparent=At<int>(helper,0x3C);
        return SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&state));
      }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    SourceNativeState(IDirect3DDevice9 *device,uint8_t *h):helper(h){
        // Install the mirror's state-block and UP-draw invalidation hooks before tracking bindings.
        ready=MirrorOf(device)&&Begin(device);fpu=FpuWord();
        if(ready){FpuSet(FpuGameWord());RenderSourceLodBegin();RenderSourceRadianceBegin();VbRenameBeginSourceWrites(device);}
    }
    void ResetQueue(){At<int>(helper,0x24)=used;At<int>(helper,0x38)=opaque;At<int>(helper,0x3C)=transparent;}
    bool RestoreNative(){
      __try{
        RenderSourceLodEnd();ResetQueue();memcpy(helper,prefix,sizeof prefix);
        const bool ok=!state||SUCCEEDED(state->Apply());for(unsigned i=0;i<19;i++)*reinterpret_cast<DWORD *>(properties[i])=cached[i];
        return ok;
      }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool Finish(){
        if(!ready)return true;
        const bool published=!poolWritesPending||SafePublishSourcePools();
        g_collectOwner=nullptr;VbRenameFinishSourceWrites();
        const bool restored=RestoreNative();
        RenderSourceRadianceEnd();MirrorFrame();FpuRestore(fpu);ready=false;
        return published&&restored;
    }
    ~SourceNativeState(){Finish();}
};
bool Overlap(const float *lo,const float *hi,const float *a,const float *b){
    for(unsigned axis=0;axis<3;axis++)if(hi[axis]<a[axis]||lo[axis]>b[axis])return false;return true;
}
struct SourceDisk {
    double x,z,radiusSquared;
    bool Overlap(const float *lo,const float *hi)const{
        const double dx=(std::max)({double(lo[0])-x,x-double(hi[0]),0.0});
        const double dz=(std::max)({double(lo[2])-z,z-double(hi[2]),0.0});
        return dx*dx+dz*dz<=radiusSquared;
    }
};
void MissingBox(std::vector<MmlSceneMissing> &missing,const void *visual,const float *lo,const float *hi,unsigned reason){
    MmlSceneMissing box{};box.visual=visual;memcpy(box.lo,lo,12);memcpy(box.hi,hi,12);box.reason=reason;missing.push_back(box);
}
bool BillboardMetadata(const void *visual,MmlSceneBillboard *out){
    __try{
        if(At<uint32_t>(visual,kVisualType)!=0x44524242u||(At<uint32_t>(visual,0x1D8)&0x40u))return false;
        const void *source=visual;if(At<uint32_t>(visual,0x1D8)&0x10000u)source=At<const void *>(visual,8);
        if(!source||At<uint32_t>(source,kVisualType)!=0x44524242u)return false;
        out->axis=At<uint32_t>(source,0x21C);
        out->mode=!strcmp(game::Ls3dName(),"1.0")?At<uint8_t>(source,0x220):At<uint32_t>(source,0x220);
        if(!((out->axis==2&&out->mode==0)||(out->axis==1&&out->mode==1)))return false;
        memcpy(out->world,static_cast<const uint8_t *>(visual)+0x10,64);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct CollectionScratch {
    std::vector<void *> stack, children, visited;
    size_t count = 0;

    void Clear() {
        stack.clear();children.clear();count=0;
        std::fill(visited.begin(),visited.end(),nullptr);
    }

    bool Visit(void *frame) {
        if(count*2>=visited.size()){
            std::vector<void *> larger(visited.empty()?2048:visited.size()*2);
            for(void *old:visited)if(old)Slot(larger,old)=old;
            visited.swap(larger);
        }
        void *&slot=Slot(visited,frame);
        if(slot)return false;
        slot=frame;++count;return true;
    }

private:
    static void *&Slot(std::vector<void *> &table, void *frame) {
        const uintptr_t address=reinterpret_cast<uintptr_t>(frame);
        const size_t mask=table.size()-1;
        size_t i=((address>>3)^(address>>15))*2654435761u&mask;
        while(table[i]&&table[i]!=frame)i=(i+1)&mask;
        return table[i];
    }
};
CollectionScratch g_sourceScratch;

// 1.0 UnlockAll traverses every pool buffer even when none were locked.
// Prepare consecutive native rigid visuals together, like the main renderer,
// and publish once. The queue's entries and callbacks keep source order.
struct SourcePreparationBatch {
    struct Row {
        void *visual,*sector;
        unsigned kind,type;
        int first,end;
        float lo[3],hi[3];
    };
    std::array<Row,256> rows;
    size_t count=0;

    bool Queue(SourceNativeState &native,void *visual,void *sector,unsigned kind,unsigned type,const float *lo,const float *hi){
        auto *helper=native.helper;
        const int first=At<int>(helper,0x24);
        native.poolWritesPending=true;
        At<void *>(helper,0x14)=sector;
        PrepareSource(visual);
        const int end=At<int>(helper,0x24);
        if(end<first||end>200000||static_cast<unsigned>(end)>At<unsigned>(helper,0x20))return false;
        auto &row=rows[count++];
        row={visual,sector,kind,type,first,end,{lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]}};
        return true;
    }

    bool Replay(SourceNativeState &native,std::vector<MmlSceneMissing> &missing){
        if(!count)return true;
        PublishSourcePools();
        native.poolWritesPending=false;
        for(size_t i=0;i<count;i++){
            if(!ModAlive(g_collectOwner))return false;
            const auto &row=rows[i];
            At<void *>(native.helper,0x14)=row.sector;
            if(g_sourceTiming)g_sourceTimingRow=SourceTypeRow(row.type);
            const unsigned before=g_collectDraws;
            if(ReplaySource(row.visual,row.kind,native.helper,row.first,row.end)&&g_collectDraws==before)
                MissingBox(missing,row.visual,row.lo,row.hi,MML_SCENE_MISSING_PREPARE);
        }
        g_sourceTimingRow=-1;
        count=0;
        native.ResetQueue();
        return true;
    }
};

bool CollectSources(MmlMod *mod,const float *lo,const float *hi,MmlSceneCollection &result,SourceNativeState &kept,CollectionScratch &scratch,BoundUpdate update){
    const auto &l=game::Ls3d();void *scene=*reinterpret_cast<void **>(g_drawLs3dBase+l.renderScene);if(!scene)return false;
    auto *helper=reinterpret_cast<uint8_t *>(g_drawLs3dBase+l.addFgCamera);
    if(At<int>(helper,0x24)<0||At<int>(helper,0x24)>200000)return false;
    const bool useDisk=(g_collectFlags&MML_SCENE_COLLECT_XZ_DISK)!=0;
    const bool batchPrepare=game::g_ls3d==&game::kLs3d10;
    // Round the culling boundary out, never toward the declared query domain.
    const double radius=(std::max)(double(hi[0])-lo[0],double(hi[2])-lo[2])*.5+.125;
    const SourceDisk disk{(double(hi[0])+lo[0])*.5,(double(hi[2])+lo[2])*.5,radius*radius};
    g_collectOwner=mod;g_collectDraws=0;g_sourceFailureLog=0;auto &missing=g_missingSources[mod];missing.clear();
    SourcePreparationBatch batch;
    auto &stack=scratch.stack;auto &children=scratch.children;
    // Native world sectors only. Backdrop (+214) and first-person near sector (+21C) are not world reflectors.
    for(unsigned offset:{0x210u,0x218u})if(void *root=At<void *>(scene,offset))stack.push_back(root);
    while(!stack.empty()){
        void *frame=stack.back();stack.pop_back();if(!frame)return false;
        if(!scratch.Visit(frame))continue;
        if(scratch.count>200000||!ModAlive(mod))return false;
        result.visited++;
        if(!(At<uint32_t>(frame,game::kFrameFlags)&1u))continue;
        children.clear();if(!SafeChildren(frame,&children))return false;stack.insert(stack.end(),children.begin(),children.end());
        if(At<uint32_t>(frame,game::kFrameType)!=1)continue;
        const unsigned type=At<uint32_t>(frame,kVisualType);
        // SMMR (people with morphed faces) inherits the singlemesh preparation/posed-buffer path too.
        unsigned kind=type==0x50414D4Cu?2:(type==0x5F4A424Fu||type==0x4850524Du||type==0x4D474E53u||type==0x524D4D53u?1:0);
        const bool billboard=type==0x44524242u;if(!kind&&!billboard)continue;
        if(kind&&!At<void *>(frame,0x1E0))continue;
        float a[3],b[3];if(!SafeBounds(frame,update,a,b)){MissingBox(missing,frame,lo,hi,MML_SCENE_MISSING_PREPARE);continue;}
        bool valid=true;for(unsigned axis=0;axis<3;axis++)valid=valid&&std::isfinite(a[axis])&&std::isfinite(b[axis])&&a[axis]<=b[axis];
        if(!valid){MissingBox(missing,frame,lo,hi,MML_SCENE_MISSING_PREPARE);continue;}
        if(!Overlap(lo,hi,a,b))continue;
        if(useDisk&&!disk.Overlap(a,b)){++g_sourceDiskSkipped;continue;}
        result.selected++;
        if(billboard){MmlSceneBillboard metadata{};metadata.size=sizeof metadata;
            if(!(g_collectFlags&MML_SCENE_COLLECT_BILLBOARDS)||!BillboardMetadata(frame,&metadata)){
                MissingBox(missing,frame,a,b,MML_SCENE_MISSING_DEFORMATION);continue;
            }kind=3;
        }
        void *sector=nullptr;unsigned ancestors=0;
        for(void *parent=At<void *>(frame,game::kFrameParent);parent;parent=At<void *>(parent,game::kFrameParent)){
            if(++ancestors>256)return false;
            if(At<unsigned>(parent,game::kFrameType)==5){sector=parent;break;}
        }
        if(!sector)sector=At<void *>(scene,0x210);
        const uintptr_t add=type==0x5F4A424Fu?l.addObject:type==0x50414D4Cu?l.addLitObject:0;
        if(batchPrepare&&add&&At<const uintptr_t *>(frame,0)[0x68/4]==g_drawLs3dBase+add){
            if(g_sourceTiming){g_sourceTimingRow=SourceTypeRow(type);g_sourceTimings[g_sourceTimingRow].selected++;}
            {
                SourceTimer timer(&SourceTimingRow::prepare);
                if(!batch.Queue(kept,frame,sector,kind,type,a,b))return false;
            }
            g_sourceTimingRow=-1;
            if(batch.count==batch.rows.size()&&!batch.Replay(kept,missing))return false;
            continue;
        }
        if(!batch.Replay(kept,missing))return false;
        At<void *>(helper,0x14)=sector;
        const unsigned before=g_collectDraws;
        bool eligible=false,prepared=false;
        if(g_sourceTiming){g_sourceTimingRow=SourceTypeRow(type);g_sourceTimings[g_sourceTimingRow].selected++;}
        {
            SourceTimer timer(&SourceTimingRow::prepare);
            prepared=SafePrepareSource(frame,kind,helper,kept.used,&eligible);
        }
        g_sourceTimingRow=-1;kept.ResetQueue();
        // A geom_hide rule intentionally removes a reflector; it must not become an enormous unknown water box.
        if(!prepared||(eligible&&g_collectDraws==before))MissingBox(missing,frame,a,b,MML_SCENE_MISSING_PREPARE);
    }
    if(!batch.Replay(kept,missing))return false;
    // Streaming gaps remain conservative missing geometry, including behind the eye. Blocks store an enclosing
    // XZ circle; use the request's full Y range rather than inventing an unloaded building height.
    const uintptr_t cache=game::Exe().cityCache;
    if(cache&&At<uint8_t>(reinterpret_cast<void *>(cache),0x24)){
        const auto *begin=At<const uint8_t *>(reinterpret_cast<void *>(cache),4),*end=At<const uint8_t *>(reinterpret_cast<void *>(cache),8);
        if(!begin||end<begin||size_t(end-begin)%0x74||size_t(end-begin)>100000*0x74u)return false;
        for(const uint8_t *block=begin;block<end;block+=0x74)if(!At<uint8_t>(block,4)){
            const float x=At<float>(block,0x18),z=At<float>(block,0x1C),r=At<float>(block,0x20);
            if(!std::isfinite(x)||!std::isfinite(z)||!std::isfinite(r)||r<0)return false;
            float a[3]={x-r,lo[1],z-r},b[3]={x+r,hi[1],z+r};
            if(Overlap(lo,hi,a,b))MissingBox(missing,nullptr,a,b,MML_SCENE_MISSING_STREAM);
        }
    }
    if(g_sourceTiming)g_sourceTimingCollections++;
    result.frame=Frames();result.draws=g_collectDraws;result.missing_count=static_cast<uint32_t>(missing.size());result.missing=missing.data();return true;
}
bool SafeCollectSources(MmlMod *mod,const float *lo,const float *hi,MmlSceneCollection *result,SourceNativeState *kept,CollectionScratch *scratch,BoundUpdate update){
    __try{return CollectSources(mod,lo,hi,*result,*kept,*scratch,update);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool SceneDrawCollect(MmlMod *mod,const float lo[3],const float hi[3],MmlSceneCollection *out,uint32_t flags){
    if(flags&~(MML_SCENE_COLLECT_BILLBOARDS|MML_SCENE_COLLECT_XZ_DISK))return false;
    if(GetCurrentThreadId()!=MainThread())return false;
    if(!lo||!hi||!out||out->size<sizeof(MmlSceneCollection)||!g_collectStage||!g_inFrame||g_collectOwner||g_activeDraw||ViewDrawingNow()||ShadowViewNow())return false;
    if(std::none_of(g_drawObservers.begin(),g_drawObservers.end(),[&](const DrawObserver &o){return o.mod==mod;}))return false;
    for(unsigned axis=0;axis<3;axis++)if(!std::isfinite(lo[axis])||!std::isfinite(hi[axis])||lo[axis]>hi[axis]||hi[axis]-lo[axis]>20000)return false;
    const auto &ls=game::Ls3d();
    if(!ls.sourceVbUnlock||!ls.sourceVbManager||!ls.sourceIbUnlock||!ls.sourceIbManager)return false;
    std::string why;if(!EnsureHooks()||!EnsureDip()||!RenderSourceLod(&why)||!RenderSourceRadiance(&why))return false;
    try{
        const uint64_t frame=Frames();g_sourceLastCollect=frame;
        {
            SourceTimer timer(g_sourceTimingPrune);
            for(auto i=g_sourceCache.begin();i!=g_sourceCache.end();){
                if(frame-i->second.seen<=2){++i;continue;}
                g_sourceCacheBytes-=i->second.Bytes();
                i=g_sourceCache.erase(i);
            }
        }
        auto update=reinterpret_cast<BoundUpdate>(GetProcAddress(reinterpret_cast<HMODULE>(g_drawLs3dBase),"?UpdateWBoundProc@I3D_visual@@AAGXXZ"));
        auto *device=static_cast<IDirect3DDevice9 *>(RenderDevice(9));if(!update||!device)return false;
        const uint64_t setup=g_sourceTiming?SourceCounter():0;
        SourceNativeState kept(device,reinterpret_cast<uint8_t *>(g_drawLs3dBase+game::Ls3d().addFgCamera));
        if(g_sourceTiming)g_sourceTimingSetup+=SourceCounter()-setup;
        if(!kept.ready)return false;
        g_sourceScratch.Clear();MmlSceneCollection result{};result.size=sizeof result;
        g_collectFlags=flags;
        bool collected=false;
        {SourceTimer timer(g_sourceTimingWalk);collected=SafeCollectSources(mod,lo,hi,&result,&kept,&g_sourceScratch,update);}
        g_sourceTimingRow=-1;
        bool restored=false;
        {SourceTimer timer(g_sourceTimingRestore);restored=kept.Finish();}
        g_collectFlags=0;
        if(!collected||!restored)return false;memcpy(out,&result,sizeof result);return true;
    }
    catch(...){g_sourceTimingRow=-1;return false;}
}
bool SceneDrawObserve(MmlMod *mod,MmlSceneDrawFn fn,void *user){
    auto existing=std::find_if(g_drawObservers.begin(),g_drawObservers.end(),[&](const DrawObserver &o){return o.mod==mod;});
    if(!fn){
        const bool removed=existing!=g_drawObservers.end();
        if(removed)g_drawObservers.erase(existing);
        if(g_drawObservers.empty()){
            if(g_activeDraw==nullptr){std::vector<MmlSceneVertex>().swap(g_sourceVertices);std::vector<uint32_t>().swap(g_sourceIndices);}
            if(removed)MirrorUse(false);
        }
        return true;
    }
    if(existing!=g_drawObservers.end()){*existing={mod,fn,user};return true;}
    if(g_drawObservers.size()>=64||!EnsureHooks())return false;
    if(g_drawObservers.empty()){g_drawLs3dBase=ModuleBase("LS3DF.dll");MirrorUse(true);}
    g_drawObservers.push_back({mod,fn,user});return true;
}
bool SceneDrawGeometry(MmlMod *mod,const MmlSceneDraw *draw,MmlSceneGeometry *out){
    constexpr size_t minimum=offsetof(MmlSceneGeometry,source_fvf)+sizeof(uint32_t);
    constexpr size_t revision=offsetof(MmlSceneGeometry,content_revision)+sizeof(uint64_t);
    constexpr size_t detail=offsetof(MmlSceneGeometry,material_detail)+sizeof(void *);
    if(!out||out->size<minimum||!g_activeDraw||g_activeDraw!=draw||g_activeObserver!=mod)return false;
    if(!g_sourceAttempted){g_sourceAttempted=true;try{g_sourceValid=SafeCopySceneGeometry(*draw);}catch(...){g_sourceValid=false;}}
    if(!g_sourceValid){
        if(g_collectOwner&&!(g_sourceFailureLog&(1u<<g_sourceStep))){
            g_sourceFailureLog|=1u<<g_sourceStep;
            Log(MML_LOG_DEBUG,"scene","source copy refused step=%u fvf=%X visual=%p base=%d min=%u vertices=%u start=%u prims=%u index=%u stride=%u vb=%u ib=%u hr=%08X",
                g_sourceStep,g_sourceFvf,draw->visual,draw->base_vertex,draw->min_vertex,draw->vertex_count,draw->start_index,draw->primitive_count,g_sourceBadIndex,g_sourceStride,g_sourceVbBytes,g_sourceIbBytes,unsigned(g_sourceReadHr));
        }
        return false;
    }
    const auto &vertices=g_sourceCached?g_sourceCached->vertices:g_sourceVertices;
    const auto &indices=g_sourceCached?g_sourceCached->indices:g_sourceIndices;
    MmlSceneGeometry geometry{};
    geometry.size=out->size>=sizeof geometry?sizeof geometry:out->size>=detail?detail:out->size>=revision?revision:minimum;
    geometry.vertices=static_cast<uint32_t>(vertices.size());geometry.vertex_stride=sizeof(MmlSceneVertex);geometry.vertex=vertices.data();
    geometry.triangles=static_cast<uint32_t>(indices.size()/3);geometry.index=indices.data();geometry.source_fvf=g_sourceFvf;
    if(g_sourceCached)geometry.content_revision=g_sourceCached->revision;
    geometry.material_detail=g_activeMaterial;
    memcpy(out,&geometry,geometry.size);return true;
}
bool SceneDrawBillboard(MmlMod *mod,const MmlSceneDraw *draw,MmlSceneBillboard *out){
    if(!out||out->size<sizeof(MmlSceneBillboard)||!draw||draw!=g_activeDraw||g_activeObserver!=mod||draw->kind!=3)return false;
    MmlSceneBillboard result{};result.size=sizeof result;
    if(!BillboardMetadata(draw->visual,&result))return false;*out=result;return true;
}

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
    // asked during a frame (a graphics pass, after the world was drawn): the whole next frame, not this one's rest
    g_captures.back()->armed = !g_frameOpen;
    return true;
}

void GeomRemove(MmlMod *mod) {
    SceneDrawObserve(mod,nullptr,nullptr);
    g_missingSources.erase(mod);
    RemoveRules(mod, 0);
    g_captures.erase(std::remove_if(g_captures.begin(), g_captures.end(), [&](const std::unique_ptr<Capture> &c) { return c->mod == mod; }),
                     g_captures.end());
}

// (render.cpp) a frame of the mission's world begins (before the scene views), its own drawing, its end.
void GeomFrameBegin() {
    if(Frames()-g_sourceLastCollect>2){
        if(!g_sourceCache.empty()){g_sourceCache.clear();g_sourceCacheBytes=0;}
        if(!g_sourceScratch.visited.empty())g_sourceScratch=CollectionScratch{};
    }
    g_match.clear();
    g_frameMatch.clear();
    g_frameOpen = true;
    for (auto &c : g_captures)
        if (!c->done) c->armed = true;
    for (Rule &r : g_rules) { r.groupsNow = r.facesNow = 0; }
    if (!g_captures.empty() || ShadersWanted() || !g_drawObservers.empty()) EnsureDip();
}
bool GeomInFrame() { return g_inFrame; }
void GeomFrameDrawing(bool on) {
    if (!on && g_inFrame) BatchFrameEnd(RenderDevice(9));   // (batch.cpp: what is asked for and not drawn yet)
    g_inFrame = on;
}
void GeomFrameEnd() {
    g_frames++;
    g_frameOpen = false;
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
        if(p.is_object()){
            if(p.value("timing_reset",false)){
                g_sourceTimings={};g_sourceTimingCollections=0;
                g_sourceTimingPrune=g_sourceTimingSetup=g_sourceTimingWalk=g_sourceTimingRestore=0;
            }
            if(p.contains("timing_sources"))g_sourceTiming=p.value("timing_sources",false);
        }
        json rules = json::array();
        for (const Rule &r : g_rules)
            rules.push_back({{"owner", ModIdOf(r.mod)}, {"handle", r.handle}, {"pattern", r.pattern},
                             {"where", r.where == MML_GEOM_FRAME ? "frame" : r.where == MML_GEOM_VIEWS ? "views" : "all"},
                             {"groups_last_frame", r.groupsLast}, {"faces_last_frame", r.facesLast}});
        json out = {{"hooked", g_hooked}, {"capture_hooked", g_dipHooked}, {"frames", g_frames}, {"rules", rules},
                    {"faults", g_faults}, {"capture_fails", g_dipFails}};
        out["source_cache"]={{"entries",g_sourceCache.size()},{"bytes",g_sourceCacheBytes},{"budget",SourceCacheBudget()},
            {"hits",g_sourceCacheHits},{"cached",g_sourceCacheNew},{"shadow_reads",g_sourceShadowReads},
            {"binding_reads",g_sourceBindingReads},
            {"index_proofs",g_sourceIndexProofs},{"native_index_proofs",g_sourceNativeIndexProofs},{"vertex_proofs",g_sourceVertexProofs},
            {"sparse_draws",g_sourceSparseDraws},{"skipped_vertices",g_sourceSkippedVertices},
            {"decoded_vertices",g_sourceDecodedVertices},{"shadow_vertices",g_sourceShadowVertices},
            {"outside_disk",g_sourceDiskSkipped}};
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        const double milliseconds=1000.0/double(frequency.QuadPart);
        json timings=json::array();const char *types[]={"OBJ_","LMAP","MRPH","SNGM","SMMR","BBRD"};
        for(unsigned i=0;i<g_sourceTimings.size();i++){
            const auto &t=g_sourceTimings[i];
            timings.push_back({{"type",types[i]},{"selected",t.selected},{"groups",t.groups},{"draws",t.draws},{"copies",t.copies},
                {"cached_copies",t.cachedCopies},{"vertex_proofs",t.vertexProofs},{"native_index_proofs",t.nativeIndexProofs},
                {"prepare_ms",t.prepare*milliseconds},{"replay_ms",t.replay*milliseconds},
                {"observe_ms",t.observe*milliseconds},{"callback_ms",t.callback*milliseconds},{"geometry_ms",t.geometry*milliseconds}});
        }
        out["source_timing"]={{"enabled",g_sourceTiming},{"collections",g_sourceTimingCollections},{"nested",true},{"types",timings},
            {"prune_ms",g_sourceTimingPrune*milliseconds},{"setup_ms",g_sourceTimingSetup*milliseconds},
            {"walk_ms",g_sourceTimingWalk*milliseconds},{"restore_ms",g_sourceTimingRestore*milliseconds}};
        if (!g_hooked && g_hookTried) out["why"] = g_hookWhy;
        if (!g_dipWhy.empty()) out["capture_why"] = g_dipWhy;
        *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    return fail("unknown geom method (geom.hide, geom.show, geom.state, geom.survey, geom.capture)");
}

} // namespace host
