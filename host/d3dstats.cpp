// The Direct3D 9 device's calls counted and timed a frame (prof.counters; each frame's in prof.frames' `d3d`), and the
// GPU's time of each frame: what the game's drawing costs, and what the host's own work on it adds.
//
// Every call of the device the game draws with (a wrapper's Direct3D 9: dxwrapper's, DXVK's) is hooked twice while
// counting is on, in the host's hook chains (hooks.cpp):
//   - an outer link, kept outermost: the caller's own return address tells whose call it is (the host's: in
//     MafiaModLoader.asi; the game's: everything else, its Direct3D 8 through the wrapper), counted by kind and timed
//     whole. A game's draw's time then holds the hooks' work in its chain (the host's shaders, the motion pass...);
//   - for the draws, an inner link, kept innermost: the time past every hook, the driver's own (the runtime's and the
//     wrapper's too), given to whoever's draw it is (the outer links' callers, innermost last); and whether the draw
//     reaches the driver through shaders or the fixed pipeline, a switch between the two counted.
// So the host's own part in the game's draws is the game's draws' time less the driver's under them. Only the game
// thread's calls are counted (another thread's go straight through). The counting's own cost, measured with MCP
// `bench` (prof.counters off against on, 1.2's freeride, some 2400 calls a frame): 0.05 ms of a 2.8 ms frame, some
// 35 ns a call; a comparison counts on both sides alike.
//
// The GPU's time of a frame: a timestamp after the game's Present returned and one before its next Present, in one
// disjoint query, read back without waiting a few frames later (prof.frames' gpu_ms). It is the span on the GPU's
// clock between the two marks: the frame's own work when the GPU is the bottleneck (Present then blocks), its work and
// its waits for the CPU's commands when the CPU is.
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <utility>

namespace host {
namespace {

// IDirect3DDevice9's slots
enum : int {
    kVtStretchRect = 34, kVtSetRenderTarget = 37, kVtGetRenderTarget = 38, kVtSetDepthStencil = 39, kVtGetDepthStencil = 40,
    kVtBeginScene = 41, kVtEndScene = 42, kVtClear = 43, kVtSetTransform = 44, kVtGetTransform = 45,
    kVtMultiplyTransform = 46, kVtSetViewport = 47, kVtGetViewport = 48, kVtSetMaterial = 49, kVtGetMaterial = 50,
    kVtSetLight = 51, kVtGetLight = 52, kVtLightEnable = 53, kVtGetLightEnable = 54, kVtSetRenderState = 57,
    kVtGetRenderState = 58, kVtGetTexture = 64, kVtSetTexture = 65, kVtGetTextureStageState = 66,
    kVtSetTextureStageState = 67, kVtGetSamplerState = 68, kVtSetSamplerState = 69, kVtDrawPrimitive = 81,
    kVtDrawIndexedPrimitive = 82, kVtDrawPrimitiveUP = 83, kVtDrawIndexedPrimitiveUP = 84, kVtSetVertexDeclaration = 87,
    kVtGetVertexDeclaration = 88, kVtSetFVF = 89, kVtGetFVF = 90, kVtSetVertexShader = 92, kVtGetVertexShader = 93,
    kVtSetVertexShaderConstantF = 94, kVtGetVertexShaderConstantF = 95, kVtSetStreamSource = 100,
    kVtGetStreamSource = 101, kVtSetIndices = 104, kVtGetIndices = 105, kVtSetPixelShader = 107, kVtGetPixelShader = 108,
    kVtSetPixelShaderConstantF = 109, kVtGetPixelShaderConstantF = 110,
};
// IDirect3DVertexBuffer9's and IDirect3DIndexBuffer9's Lock and Unlock; IDirect3DStateBlock9's Capture and Apply
enum : int { kVtLock = 11, kVtUnlock = 12, kVtCapture = 4, kVtApply = 5 };
// what is counted besides the kinds: a draw's primitives, a vertex or pixel shader set
enum Extra { kNone, kPrimsArg2, kPrimsArg3, kPrimsArg4, kPrimsArg6, kVs, kPs, kUncounted };

const char *kKindNames[kKindCount] = {"draw_primitive", "draw_indexed", "draw_up", "render_state", "texture_stage",
                                      "sampler", "texture", "transform", "light", "material", "vertex_format", "stream",
                                      "indices", "shader", "shader_constants", "viewport", "targets", "clear", "scene",
                                      "state_block", "get", "lock"};

bool g_on, g_hooked;
uintptr_t g_hostBase, g_hostEnd;
D3dFrameCounts g_now;
void *g_vs, *g_ps;              // the shaders set now (the fixed pipeline: both null)
int g_lastDriverProg = -1;
int g_who[32];                  // the outer links' callers being run, innermost last
int g_whoDepth;
DWORD g_tid;

inline int WhoOf(void *ra) {
    const auto a = reinterpret_cast<uintptr_t>(ra);
    return a >= g_hostBase && a < g_hostEnd ? kWhoHost : kWhoGame;
}
inline bool Counting() { return g_on && GetCurrentThreadId() == g_tid; }

template <size_t> using U = uint32_t;   // (every argument of these methods is 4 bytes: forwarded as its bits)

template <int Id, int Kind, int What, typename Seq> struct Hk;
template <int Id, int Kind, int What, size_t... I> struct Hk<Id, Kind, What, std::index_sequence<I...>> {
    using Fn = HRESULT(__stdcall *)(void *, U<I>...);
    static inline Fn outer, inner;
    static HRESULT __stdcall Outer(void *self, U<I>... a) {
        if (!Counting()) return outer(self, a...);
        const int who = WhoOf(_ReturnAddress());
        const uint32_t args[] = {a..., 0};
        if constexpr (What != kUncounted) g_now.calls[Kind][who]++;
        if constexpr (What == kPrimsArg2) g_now.prims[who] += args[1];
        if constexpr (What == kPrimsArg3) g_now.prims[who] += args[2];
        if constexpr (What == kPrimsArg4) g_now.prims[who] += args[3];
        if constexpr (What == kPrimsArg6) g_now.prims[who] += args[5];
        if constexpr (What == kVs) g_vs = reinterpret_cast<void *>(static_cast<uintptr_t>(args[0]));
        if constexpr (What == kPs) g_ps = reinterpret_cast<void *>(static_cast<uintptr_t>(args[0]));
        if (g_whoDepth < 32) g_who[g_whoDepth] = who;
        g_whoDepth++;
        const uint64_t t0 = __rdtsc();
        HRESULT hr = outer(self, a...);
        g_now.ticks[Kind][who] += __rdtsc() - t0;
        g_whoDepth--;
        return hr;
    }
    // (the draws') innermost: the driver's time, the pipeline the draw takes
    static HRESULT __stdcall Inner(void *self, U<I>... a) {
        if (!Counting()) return inner(self, a...);
        const int who = g_whoDepth > 0 && g_whoDepth <= 32 ? g_who[g_whoDepth - 1] : kWhoGame;
        const int prog = g_vs || g_ps ? 1 : 0;
        g_now.driverDraws[prog]++;
        if (g_lastDriverProg >= 0 && prog != g_lastDriverProg) g_now.switches++;
        g_lastDriverProg = prog;
        const uint64_t t0 = __rdtsc();
        HRESULT hr = inner(self, a...);
        g_now.driverTicks[who] += __rdtsc() - t0;
        return hr;
    }
};

struct Site {
    int vt;                       // which object's vtable: 0 the device, 1 a vertex buffer, 2 an index buffer, 3 a state block
    int slot;
    void *outer;
    void **outerOrig;
    void *inner;
    void **innerOrig;
    uintptr_t at;                 // where hooked
};

#define HK(vt, slot, kind, what, n)                                                                                    \
    Site{vt, slot, reinterpret_cast<void *>(&Hk<vt * 1000 + slot, kind, what, std::make_index_sequence<n>>::Outer),     \
         reinterpret_cast<void **>(&Hk<vt * 1000 + slot, kind, what, std::make_index_sequence<n>>::outer), nullptr,     \
         nullptr, 0}
#define HKDRAW(slot, kind, what, n)                                                                                     \
    Site{0, slot, reinterpret_cast<void *>(&Hk<slot, kind, what, std::make_index_sequence<n>>::Outer),                  \
         reinterpret_cast<void **>(&Hk<slot, kind, what, std::make_index_sequence<n>>::outer),                          \
         reinterpret_cast<void *>(&Hk<slot, kind, what, std::make_index_sequence<n>>::Inner),                           \
         reinterpret_cast<void **>(&Hk<slot, kind, what, std::make_index_sequence<n>>::inner), 0}

Site g_sites[] = {
    HKDRAW(kVtDrawPrimitive, kKindDrawPrimitive, kPrimsArg3, 3),
    HKDRAW(kVtDrawIndexedPrimitive, kKindDrawIndexed, kPrimsArg6, 6),
    HKDRAW(kVtDrawPrimitiveUP, kKindDrawUp, kPrimsArg2, 4),
    HKDRAW(kVtDrawIndexedPrimitiveUP, kKindDrawUp, kPrimsArg4, 8),
    HK(0, kVtSetRenderState, kKindRenderState, kNone, 2),
    HK(0, kVtSetTextureStageState, kKindTextureStage, kNone, 3),
    HK(0, kVtSetSamplerState, kKindSampler, kNone, 3),
    HK(0, kVtSetTexture, kKindTexture, kNone, 2),
    HK(0, kVtSetTransform, kKindTransform, kNone, 2),
    HK(0, kVtMultiplyTransform, kKindTransform, kNone, 2),
    HK(0, kVtSetLight, kKindLight, kNone, 2),
    HK(0, kVtLightEnable, kKindLight, kNone, 2),
    HK(0, kVtSetMaterial, kKindMaterial, kNone, 1),
    HK(0, kVtSetFVF, kKindVertexFormat, kNone, 1),
    HK(0, kVtSetVertexDeclaration, kKindVertexFormat, kNone, 1),
    HK(0, kVtSetStreamSource, kKindStream, kNone, 4),
    HK(0, kVtSetIndices, kKindIndices, kNone, 1),
    HK(0, kVtSetVertexShader, kKindShader, kVs, 1),
    HK(0, kVtSetPixelShader, kKindShader, kPs, 1),
    HK(0, kVtSetVertexShaderConstantF, kKindShaderConstants, kNone, 3),
    HK(0, kVtSetPixelShaderConstantF, kKindShaderConstants, kNone, 3),
    HK(0, kVtSetViewport, kKindViewport, kNone, 1),
    HK(0, kVtSetRenderTarget, kKindTargets, kNone, 2),
    HK(0, kVtSetDepthStencil, kKindTargets, kNone, 1),
    HK(0, kVtStretchRect, kKindTargets, kNone, 5),
    HK(0, kVtClear, kKindClear, kNone, 6),
    HK(0, kVtBeginScene, kKindScene, kNone, 0),
    HK(0, kVtEndScene, kKindScene, kNone, 0),
    HK(0, kVtGetTransform, kKindGet, kNone, 2),
    HK(0, kVtGetViewport, kKindGet, kNone, 1),
    HK(0, kVtGetMaterial, kKindGet, kNone, 1),
    HK(0, kVtGetLight, kKindGet, kNone, 2),
    HK(0, kVtGetLightEnable, kKindGet, kNone, 2),
    HK(0, kVtGetRenderState, kKindGet, kNone, 2),
    HK(0, kVtGetTexture, kKindGet, kNone, 2),
    HK(0, kVtGetTextureStageState, kKindGet, kNone, 3),
    HK(0, kVtGetSamplerState, kKindGet, kNone, 3),
    HK(0, kVtGetVertexDeclaration, kKindGet, kNone, 1),
    HK(0, kVtGetFVF, kKindGet, kNone, 1),
    HK(0, kVtGetVertexShader, kKindGet, kNone, 1),
    HK(0, kVtGetVertexShaderConstantF, kKindGet, kNone, 3),
    HK(0, kVtGetStreamSource, kKindGet, kNone, 4),
    HK(0, kVtGetIndices, kKindGet, kNone, 1),
    HK(0, kVtGetPixelShader, kKindGet, kNone, 1),
    HK(0, kVtGetPixelShaderConstantF, kKindGet, kNone, 3),
    HK(0, kVtGetRenderTarget, kKindGet, kNone, 2),
    HK(0, kVtGetDepthStencil, kKindGet, kNone, 1),
    HK(1, kVtLock, kKindLock, kNone, 4),        // (a Lock counted; its Unlock timed with it, not counted)
    HK(1, kVtUnlock, kKindLock, kUncounted, 0),
    HK(2, kVtLock, kKindLock, kNone, 4),
    HK(2, kVtUnlock, kKindLock, kUncounted, 0),
    HK(3, kVtCapture, kKindStateBlock, kNone, 0),
    HK(3, kVtApply, kKindStateBlock, kNone, 0),
};
#undef HK
#undef HKDRAW

void Unhook() {
    for (Site &s : g_sites) {
        if (!s.at) continue;
        HookRemoveLink(nullptr, s.at, s.outer);
        if (s.inner) HookRemoveLink(nullptr, s.at, s.inner);
        s.at = 0;
    }
    g_hooked = false;
}

// The vtables: the device's, and a buffer's, an index buffer's and a state block's made for the purpose (the game's
// own are of the same classes: the wrapper's, or the runtime's).
bool Hook(IDirect3DDevice9 *d, std::string *why) {
    if (!g_hostBase) {
        HMODULE me = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char *>(&WhoOf), &me);
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(me);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(me) + dos->e_lfanew);
        g_hostBase = reinterpret_cast<uintptr_t>(me);
        g_hostEnd = g_hostBase + nt->OptionalHeader.SizeOfImage;
    }
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    IDirect3DStateBlock9 *sb = nullptr;
    d->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    d->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr);
    d->CreateStateBlock(D3DSBT_PIXELSTATE, &sb);
    void **vts[4] = {*reinterpret_cast<void ***>(d), vb ? *reinterpret_cast<void ***>(vb) : nullptr,
                     ib ? *reinterpret_cast<void ***>(ib) : nullptr, sb ? *reinterpret_cast<void ***>(sb) : nullptr};
    std::vector<uintptr_t> done;
    bool ok = true;
    for (Site &s : g_sites) {
        if (!vts[s.vt]) continue;   // (a buffer the device would not make: its locks not counted)
        uintptr_t at = reinterpret_cast<uintptr_t>(vts[s.vt][s.slot]);
        if (std::find(done.begin(), done.end(), at) != done.end()) continue;   // (one function behind two slots)
        done.push_back(at);
        std::string err;
        if (!HookAddOuter(at, s.outer, s.outerOrig, &err) || (s.inner && !HookAddInner(at, s.inner, s.innerOrig, &err))) {
            *why = "slot " + std::to_string(s.slot) + " not hooked: " + err;
            HookRemoveLink(nullptr, at, s.outer);
            ok = false;
            break;
        }
        s.at = at;
    }
    if (vb) vb->Release();
    if (ib) ib->Release();
    if (sb) sb->Release();
    if (!ok) { Unhook(); return false; }
    g_hooked = true;
    return true;
}

// ---- the GPU's time of each frame
constexpr int kGpuSets = 6;
struct GpuSet { IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr, *t0 = nullptr, *t1 = nullptr; uint32_t seq = 0; bool open = false, pending = false; };
GpuSet g_gpu[kGpuSets];
int g_gpuNext;
bool g_gpuOn;
std::string g_gpuWhy = "off (prof.counters {gpu: true})";
IDirect3DDevice9 *g_gpuDev;
GpuSet *g_gpuOpen;

void GpuRelease() {
    for (GpuSet &s : g_gpu) {
        for (IDirect3DQuery9 **q : {&s.disjoint, &s.freq, &s.t0, &s.t1}) if (*q) { (*q)->Release(); *q = nullptr; }
        s.open = s.pending = false;
    }
    g_gpuOpen = nullptr;
    g_gpuDev = nullptr;
}
bool GpuMake(IDirect3DDevice9 *d, std::string *why) {
    for (GpuSet &s : g_gpu) {
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq)) ||
            FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.t0)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.t1))) {
            GpuRelease();
            *why = "the device makes no timestamp queries";
            return false;
        }
    }
    g_gpuDev = d;
    return true;
}
// the frames whose marks the GPU has passed: their time given to their records
void GpuRead() {
    for (GpuSet &s : g_gpu) {
        if (!s.pending) continue;
        BOOL disjoint = TRUE;
        UINT64 freq = 0, t0 = 0, t1 = 0;
        HRESULT h = s.disjoint->GetData(&disjoint, sizeof disjoint, 0);
        if (h == D3DERR_DEVICELOST) { GpuRelease(); g_gpuOn = false; g_gpuWhy = "the device was lost"; return; }
        if (h != S_OK || s.freq->GetData(&freq, sizeof freq, 0) != S_OK || s.t0->GetData(&t0, sizeof t0, 0) != S_OK ||
            s.t1->GetData(&t1, sizeof t1, 0) != S_OK)
            continue;
        s.pending = false;
        if (!disjoint && freq && t1 >= t0) ProfGpuOf(s.seq, static_cast<float>(1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(freq)));
    }
}
} // namespace

const char *D3dKindName(int kind) { return kind >= 0 && kind < kKindCount ? kKindNames[kind] : "?"; }
bool D3dStatsOn() { return g_on; }
std::string D3dStatsWhyGpu() { return g_gpuOn ? std::string() : g_gpuWhy; }

bool D3dStatsSet(bool on, bool gpu, std::string *why) {
    auto *d = static_cast<IDirect3DDevice9 *>(RenderDevice(9));
    if ((on || gpu) && !d) {
        *why = "no Direct3D 9 device (counting and the GPU's time need a wrapper's Direct3D 9: dxwrapper, DXVK)";
        if (gpu) g_gpuWhy = *why;
        return false;
    }
    g_tid = MainThread();
    if (on && !g_hooked) {
        if (!Hook(d, why)) return false;
        IDirect3DVertexShader9 *vs = nullptr;
        IDirect3DPixelShader9 *ps = nullptr;
        d->GetVertexShader(&vs);
        d->GetPixelShader(&ps);
        g_vs = vs, g_ps = ps;
        if (vs) vs->Release();
        if (ps) ps->Release();
        HLOG("prof: the device's calls counted (%zu methods hooked)", sizeof g_sites / sizeof g_sites[0]);
    }
    if (!on && g_hooked) {
        g_on = false;
        Unhook();
        HLOG("prof: the device's calls no longer counted");
    }
    g_on = on;
    g_now = D3dFrameCounts{};
    g_whoDepth = 0;
    g_lastDriverProg = -1;
    if (gpu && !g_gpuOn) {
        if (g_gpuDev != d) GpuRelease();
        if (!g_gpuDev && !GpuMake(d, &g_gpuWhy)) return true;   // (counting stays; gpu_ms stays null, and why)
        g_gpuOn = true;
    }
    if (!gpu && g_gpuOn) {
        g_gpuOn = false;
        GpuRelease();
        g_gpuWhy = "off (prof.counters {gpu: true})";
    }
    return true;
}

void D3dStatsTake(D3dFrameCounts *out) {
    *out = g_now;
    g_now = D3dFrameCounts{};
}

void D3dStatsFrameEnd(uint32_t seq) {
    if (!g_gpuOn || !g_gpuOpen) return;
    g_gpuOpen->t1->Issue(D3DISSUE_END);
    g_gpuOpen->disjoint->Issue(D3DISSUE_END);
    g_gpuOpen->freq->Issue(D3DISSUE_END);
    g_gpuOpen->seq = seq;
    g_gpuOpen->open = false;
    g_gpuOpen->pending = true;
    g_gpuOpen = nullptr;
}

void D3dStatsFrameStart() {
    if (!g_gpuOn) return;
    if (RenderDevice(9) != g_gpuDev) { GpuRelease(); g_gpuOn = false; g_gpuWhy = "the device changed (prof.counters again)"; return; }
    GpuRead();
    if (!g_gpuOn) return;
    GpuSet &s = g_gpu[g_gpuNext];
    if (s.pending) return;   // (the GPU is more than six frames behind: this frame is not timed)
    g_gpuNext = (g_gpuNext + 1) % kGpuSets;
    s.disjoint->Issue(D3DISSUE_BEGIN);
    s.t0->Issue(D3DISSUE_END);
    s.open = true;
    g_gpuOpen = &s;
}

} // namespace host
