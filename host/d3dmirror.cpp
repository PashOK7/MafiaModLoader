// The Direct3D 9 device's state followed, not asked (d3dmirror.h; ROADMAP item 31b).
//
// While a user wants it (the shader rules, the people the GPU skins: shaders.cpp), the device's Set* calls are hooked
// in the host's chains (hooks.cpp) on the vtable of the device the game draws with (a wrapper's Direct3D 9: dxwrapper's,
// DXVK's): each value set is kept. What a Set* does not tell is handled so: MultiplyTransform's product read back once;
// SetRenderTarget (the viewport made the target's whole) the viewport read back at the next use; a state block's Apply,
// a Reset, a call from another thread than the game's, or the device not the one followed: everything read again from
// the device at the next use (some 700 Get* calls, once). Set* calls between BeginStateBlock and EndStateBlock record
// and change nothing: not kept. And once a frame (Present) everything is read again, so nothing a wrapper does behind
// the vtable (its own state set on the device it wraps) outlives a frame; `shader.mirror {check}` compares the mirror
// with the device's own answers draw by draw.
//
// The shaders lent (MirrorLend; ROADMAP item 31c): a rule's shaders set for a draw stay bound after it, the shaders
// the game had (its fixed pipeline: none) kept aside and given back only when something else would see them: a draw
// that is not a rule's (DrawPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP; a DrawIndexedPrimitive no rule takes:
// shaders.cpp), a shader asked (Get*Shader: whoever asks sets it back after, the loader's passes too), a state block
// made, begun or captured (it would keep the lent ones), Present, the frame's end. A shader the game sets meanwhile ends the loan of that stage (its own is the game's).
// So consecutive draws of rules set nothing: the switches between the fixed pipeline and shaders fall to the frame's
// few changes of kind. While a rule's vertex shader is lent the fixed pipeline's transforms are not used: the game's
// SetTransform of the world, the view, the projection is kept here and sent to the device only when it would matter
// (the shaders given back, a transform asked or multiplied, a state block applied).
#include "d3dmirror.h"
#include "host.h"
#include <algorithm>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

// IDirect3DDevice9's slots; IDirect3DStateBlock9's Apply
enum : int {
    kVtReset = 16, kVtSetRenderTarget = 37, kVtSetTransform = 44, kVtMultiplyTransform = 46, kVtSetViewport = 47,
    kVtSetMaterial = 49, kVtSetLight = 51, kVtLightEnable = 53, kVtSetRenderState = 57, kVtBeginStateBlock = 60,
    kVtEndStateBlock = 61, kVtSetTexture = 65, kVtSetTextureStageState = 67, kVtSetSamplerState = 69,
    kVtSetVertexShader = 92, kVtSetVertexShaderConstantF = 94, kVtSetPixelShader = 107, kVtSetPixelShaderConstantF = 109,
    kVtPresent = 17, kVtCreateStateBlock = 59, kVtDrawPrimitive = 81, kVtDrawPrimitiveUP = 83, kVtDrawIndexedPrimitiveUP = 84,
    kVtGetVertexShader = 93, kVtGetPixelShader = 108, kVtGetTransform = 45,
};
constexpr int kVtSbCapture = 4, kVtSbApply = 5;

D3dMirror g_m;
uint32_t g_serial = 1;
uint32_t g_constSerial = 1;   // (MirrorConstSerial)
bool g_ownWrite;              // (MirrorConstants writing)
inline void Changed() { g_m.serial = ++g_serial; }
IDirect3DDevice9 *g_dev;        // the device the mirror holds (null: nothing held)
bool g_valid, g_vpStale, g_recording, g_hooked, g_hookFailed;
int g_users;
std::string g_hookWhy;
uint64_t g_reads, g_why[5];     // reads from the device; why: frame, apply, reset, thread, device
enum { kWhyFrame, kWhyApply, kWhyReset, kWhyThread, kWhyDevice };
int g_checkLeft;
uint64_t g_checked, g_mismatchDraws;
std::vector<std::string> g_mismatches;

// the loan: the stages lent, the shaders the game has there, the ones lent
bool g_lentVs, g_lentPs, g_lending;
IDirect3DVertexShader9 *g_gameVs, *g_lendVs;
IDirect3DPixelShader9 *g_gamePs, *g_lendPs;
uint64_t g_lends, g_returns, g_shaderSets, g_xfKept;
unsigned g_xfPending;   // the transforms kept, not yet on the device: 1 the world, 2 the view, 4 the projection

inline void Invalidate(int why) { if (g_valid) g_why[why]++; g_valid = false; }
// a shader set on the device held, by its game thread, not being recorded (the loan follows these even while the
// mirror is not valid)
inline bool Live(void *self) { return self == g_dev && !g_recording && GetCurrentThreadId() == MainThread(); }
void GiveBack(IDirect3DDevice9 *d);
void SendKept(IDirect3DDevice9 *d);
// a Set* to keep: the game thread's, on the device held, not being recorded
inline bool Keep(void *self) {
    if (!g_valid) return false;
    if (self != g_dev) return false;   // (another device of the same class: nothing of ours)
    if (GetCurrentThreadId() != MainThread()) { Invalidate(kWhyThread); return false; }
    return !g_recording;
}

#define ORIG(name, ...) using name##Fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, __VA_ARGS__); name##Fn o##name;
ORIG(Reset, D3DPRESENT_PARAMETERS *)
ORIG(SetRenderTarget, DWORD, IDirect3DSurface9 *)
ORIG(SetTransform, D3DTRANSFORMSTATETYPE, const D3DMATRIX *)
ORIG(MultiplyTransform, D3DTRANSFORMSTATETYPE, const D3DMATRIX *)
ORIG(SetViewport, const D3DVIEWPORT9 *)
ORIG(SetMaterial, const D3DMATERIAL9 *)
ORIG(SetLight, DWORD, const D3DLIGHT9 *)
ORIG(LightEnable, DWORD, BOOL)
ORIG(SetRenderState, D3DRENDERSTATETYPE, DWORD)
ORIG(SetTexture, DWORD, IDirect3DBaseTexture9 *)
ORIG(SetTextureStageState, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)
ORIG(SetSamplerState, DWORD, D3DSAMPLERSTATETYPE, DWORD)
ORIG(SetVertexShader, IDirect3DVertexShader9 *)
ORIG(SetPixelShader, IDirect3DPixelShader9 *)
ORIG(SetVertexShaderConstantF, UINT, const float *, UINT)
ORIG(SetPixelShaderConstantF, UINT, const float *, UINT)
#undef ORIG
using NoArgFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *);
NoArgFn oBeginStateBlock;
using EndSbFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DStateBlock9 **);
EndSbFn oEndStateBlock;
using SbApplyFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DStateBlock9 *);
SbApplyFn oSbApply;

D3DMATRIX *TransformOf(D3DTRANSFORMSTATETYPE t) {
    if (t == D3DTS_VIEW) return &g_m.view;
    if (t == D3DTS_PROJECTION) return &g_m.proj;
    if (t == D3DTS_WORLD) return &g_m.world;
    return nullptr;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p) {
    if (d == g_dev) {
        Invalidate(kWhyReset);
        g_xfPending = 0;
        g_lentVs = g_lentPs = false;   // (a Reset sets every state to its default: no shaders)
        g_gameVs = nullptr;
        g_gamePs = nullptr;
    }
    return oReset(d, p);
}
HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    HRESULT hr = oSetRenderTarget(d, i, s);
    if (i == 0 && Keep(d)) g_vpStale = true;   // (the viewport made the new target's whole)
    return hr;
}
unsigned XfBit(D3DTRANSFORMSTATETYPE t) { return t == D3DTS_WORLD ? 1 : t == D3DTS_VIEW ? 2 : t == D3DTS_PROJECTION ? 4 : 0; }
HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (g_lentVs && m && Keep(d)) {
        if (const unsigned bit = XfBit(t)) {   // (kept: the lent shader reads it from its constants)
            *TransformOf(t) = *m;
            if (bit != 1) Changed();
            g_xfPending |= bit;
            g_xfKept++;
            return D3D_OK;
        }
    }
    HRESULT hr = oSetTransform(d, t, m);
    if (SUCCEEDED(hr) && m && Keep(d))
        if (D3DMATRIX *o = TransformOf(t)) { *o = *m; if (t != D3DTS_WORLD) Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkMultiplyTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (g_xfPending && d == g_dev) SendKept(d);
    HRESULT hr = oMultiplyTransform(d, t, m);
    if (SUCCEEDED(hr) && Keep(d))
        if (D3DMATRIX *o = TransformOf(t)) { d->GetTransform(t, o); Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    HRESULT hr = oSetViewport(d, v);
    if (SUCCEEDED(hr) && v && Keep(d)) { g_m.viewport = *v; g_vpStale = false; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetMaterial(IDirect3DDevice9 *d, const D3DMATERIAL9 *m) {
    HRESULT hr = oSetMaterial(d, m);
    if (SUCCEEDED(hr) && m && Keep(d) && memcmp(&g_m.material, m, sizeof *m)) { g_m.material = *m; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetLight(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) {
    HRESULT hr = oSetLight(d, i, l);
    if (SUCCEEDED(hr) && l && i < 8 && Keep(d)) { g_m.light[i] = *l; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkLightEnable(IDirect3DDevice9 *d, DWORD i, BOOL on) {
    HRESULT hr = oLightEnable(d, i, on);
    if (SUCCEEDED(hr) && i < 8 && Keep(d)) { g_m.lightOn[i] = on ? TRUE : FALSE; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    HRESULT hr = oSetRenderState(d, s, v);
    if (SUCCEEDED(hr) && static_cast<unsigned>(s) < 256 && Keep(d) && g_m.rs[s] != v) { g_m.rs[s] = v; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9 *d, DWORD i, IDirect3DBaseTexture9 *t) {
    HRESULT hr = oSetTexture(d, i, t);
    if (SUCCEEDED(hr) && i < 8 && Keep(d)) g_m.tex[i] = t;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetTextureStageState(IDirect3DDevice9 *d, DWORD i, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    HRESULT hr = oSetTextureStageState(d, i, s, v);
    if (SUCCEEDED(hr) && i < 8 && static_cast<unsigned>(s) < 33 && Keep(d) && g_m.tss[i][s] != v) { g_m.tss[i][s] = v; Changed(); }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetSamplerState(IDirect3DDevice9 *d, DWORD i, D3DSAMPLERSTATETYPE s, DWORD v) {
    HRESULT hr = oSetSamplerState(d, i, s, v);
    if (SUCCEEDED(hr) && i < 4 && static_cast<unsigned>(s) < 14 && Keep(d)) g_m.samp[i][s] = v;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    HRESULT hr = oSetVertexShader(d, s);
    if (SUCCEEDED(hr) && Live(d)) {
        g_m.vs = s;
        if (!g_lending) { g_gameVs = s; g_lentVs = false; }   // (someone else's: the game's now)
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    HRESULT hr = oSetPixelShader(d, s);
    if (SUCCEEDED(hr) && Live(d)) {
        g_m.ps = s;
        if (!g_lending) { g_gamePs = s; g_lentPs = false; }
    }
    return hr;
}
// where the lent shaders are given back first
using DpFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DpUpFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
using DipUpFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using CreateSbFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **);
using GetVsFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DVertexShader9 **);
using GetPsFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DPixelShader9 **);
using SbCaptureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DStateBlock9 *);
using GetXfFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, D3DMATRIX *);
GetXfFn oGetXf;
DpFn oDp;
DpUpFn oDpUp;
DipUpFn oDipUp;
PresentFn oPresent;
CreateSbFn oCreateSb;
GetVsFn oGetVs;
GetPsFn oGetPs;
SbCaptureFn oSbCapture;
inline void BackFirst(IDirect3DDevice9 *d) { if ((g_lentVs || g_lentPs) && Live(d)) GiveBack(d); }
HRESULT STDMETHODCALLTYPE hkDp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b) { BackFirst(d); return oDp(d, t, a, b); }
HRESULT STDMETHODCALLTYPE hkDpUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *v, UINT s) { BackFirst(d); return oDpUp(d, t, a, v, s); }
HRESULT STDMETHODCALLTYPE hkDipUp(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *i, D3DFORMAT f, const void *v, UINT s) {
    BackFirst(d);
    return oDipUp(d, t, a, b, c, i, f, v, s);
}
HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) { BackFirst(d); return oPresent(d, a, b, w, r); }
HRESULT STDMETHODCALLTYPE hkCreateSb(IDirect3DDevice9 *d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **sb) { BackFirst(d); return oCreateSb(d, t, sb); }
HRESULT STDMETHODCALLTYPE hkGetVs(IDirect3DDevice9 *d, IDirect3DVertexShader9 **s) { BackFirst(d); return oGetVs(d, s); }
HRESULT STDMETHODCALLTYPE hkGetPs(IDirect3DDevice9 *d, IDirect3DPixelShader9 **s) { BackFirst(d); return oGetPs(d, s); }
HRESULT STDMETHODCALLTYPE hkGetXf(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) {
    if (g_xfPending && d == g_dev) SendKept(d);
    return oGetXf(d, t, m);
}
HRESULT STDMETHODCALLTYPE hkSbCapture(IDirect3DStateBlock9 *sb) {
    IDirect3DDevice9 *d = nullptr;
    if (g_dev && (g_lentVs || g_lentPs) && SUCCEEDED(sb->GetDevice(&d)) && d) { BackFirst(d); d->Release(); }
    return oSbCapture(sb);
}
HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9 *d, UINT start, const float *v, UINT n) {
    HRESULT hr = oSetVertexShaderConstantF(d, start, v, n);
    if (SUCCEEDED(hr) && v && start < 256 && Keep(d)) {
        memcpy(g_m.vsc[start], v, std::min(n, 256 - start) * 16);
        if (!g_ownWrite) g_constSerial++;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetPixelShaderConstantF(IDirect3DDevice9 *d, UINT start, const float *v, UINT n) {
    HRESULT hr = oSetPixelShaderConstantF(d, start, v, n);
    if (SUCCEEDED(hr) && v && start < 224 && Keep(d)) {
        memcpy(g_m.psc[start], v, std::min(n, 224 - start) * 16);
        if (!g_ownWrite) g_constSerial++;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkBeginStateBlock(IDirect3DDevice9 *d) {
    BackFirst(d);
    HRESULT hr = oBeginStateBlock(d);
    if (SUCCEEDED(hr) && d == g_dev) g_recording = true;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkEndStateBlock(IDirect3DDevice9 *d, IDirect3DStateBlock9 **sb) {
    HRESULT hr = oEndStateBlock(d, sb);
    if (d == g_dev) g_recording = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSbApply(IDirect3DStateBlock9 *sb) {
    if (g_xfPending && g_dev && GetCurrentThreadId() == MainThread()) SendKept(g_dev);   // (before what the block sets)
    HRESULT hr = oSbApply(sb);
    Invalidate(kWhyApply);   // (whichever device's: the next use reads ours again)
    // a block of the shaders' state put the game's own back: the loan of that stage ends (no block holds a lent one)
    if ((g_lentVs || g_lentPs) && g_dev && GetCurrentThreadId() == MainThread()) {
        IDirect3DVertexShader9 *vs = nullptr;
        IDirect3DPixelShader9 *ps = nullptr;
        oGetVs(g_dev, &vs);
        oGetPs(g_dev, &ps);
        if (g_lentVs && vs != g_lendVs) { g_lentVs = false; g_gameVs = vs; }
        if (g_lentPs && ps != g_lendPs) { g_lentPs = false; g_gamePs = ps; }
        if (vs) vs->Release();
        if (ps) ps->Release();
    }
    return hr;
}

// Everything read from the device.
void Read(IDirect3DDevice9 *d) {
    if (g_xfPending && d == g_dev) SendKept(d);
    D3dMirror &m = g_m;
    memset(&m, 0, sizeof m);
    d->GetTransform(D3DTS_WORLD, &m.world);
    d->GetTransform(D3DTS_VIEW, &m.view);
    d->GetTransform(D3DTS_PROJECTION, &m.proj);
    for (DWORD s = 1; s < 256; s++) d->GetRenderState(static_cast<D3DRENDERSTATETYPE>(s), &m.rs[s]);   // (an unknown one stays 0)
    for (DWORD i = 0; i < 8; i++) {
        for (DWORD s = 1; s < 33; s++) d->GetTextureStageState(i, static_cast<D3DTEXTURESTAGESTATETYPE>(s), &m.tss[i][s]);
        IDirect3DBaseTexture9 *t = nullptr;
        d->GetTexture(i, &t);
        m.tex[i] = t;
        if (t) t->Release();   // (the device keeps it: compared and set back only)
    }
    for (DWORD i = 0; i < 4; i++)
        for (DWORD s = 1; s < 14; s++) d->GetSamplerState(i, static_cast<D3DSAMPLERSTATETYPE>(s), &m.samp[i][s]);
    d->GetMaterial(&m.material);
    for (DWORD i = 0; i < 8; i++) {
        d->GetLightEnable(i, &m.lightOn[i]);
        if (m.lightOn[i]) d->GetLight(i, &m.light[i]);
        m.lightOn[i] = m.lightOn[i] ? TRUE : FALSE;
    }
    d->GetViewport(&m.viewport);
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    if (oGetVs) oGetVs(d, &vs); else d->GetVertexShader(&vs);   // (the device's own: no give-back from inside)
    if (oGetPs) oGetPs(d, &ps); else d->GetPixelShader(&ps);
    m.vs = vs;
    m.ps = ps;
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (!g_lentVs || d != g_dev) { g_gameVs = vs; g_lentVs = false; }   // (while lent the game's stay those kept aside)
    if (!g_lentPs || d != g_dev) { g_gamePs = ps; g_lentPs = false; }
    d->GetVertexShaderConstantF(0, &m.vsc[0][0], 256);
    d->GetPixelShaderConstantF(0, &m.psc[0][0], 224);
    g_dev = d;
    g_valid = true;
    g_vpStale = false;
    g_reads++;
    Changed();
    g_constSerial++;
}

bool Hook(IDirect3DDevice9 *d) {
    void **vt = *reinterpret_cast<void ***>(d);
    struct S { int slot; void *detour; void **orig; };
    const S sites[] = {
        {kVtReset, reinterpret_cast<void *>(&hkReset), reinterpret_cast<void **>(&oReset)},
        {kVtSetRenderTarget, reinterpret_cast<void *>(&hkSetRenderTarget), reinterpret_cast<void **>(&oSetRenderTarget)},
        {kVtSetTransform, reinterpret_cast<void *>(&hkSetTransform), reinterpret_cast<void **>(&oSetTransform)},
        {kVtMultiplyTransform, reinterpret_cast<void *>(&hkMultiplyTransform), reinterpret_cast<void **>(&oMultiplyTransform)},
        {kVtSetViewport, reinterpret_cast<void *>(&hkSetViewport), reinterpret_cast<void **>(&oSetViewport)},
        {kVtSetMaterial, reinterpret_cast<void *>(&hkSetMaterial), reinterpret_cast<void **>(&oSetMaterial)},
        {kVtSetLight, reinterpret_cast<void *>(&hkSetLight), reinterpret_cast<void **>(&oSetLight)},
        {kVtLightEnable, reinterpret_cast<void *>(&hkLightEnable), reinterpret_cast<void **>(&oLightEnable)},
        {kVtSetRenderState, reinterpret_cast<void *>(&hkSetRenderState), reinterpret_cast<void **>(&oSetRenderState)},
        {kVtBeginStateBlock, reinterpret_cast<void *>(&hkBeginStateBlock), reinterpret_cast<void **>(&oBeginStateBlock)},
        {kVtEndStateBlock, reinterpret_cast<void *>(&hkEndStateBlock), reinterpret_cast<void **>(&oEndStateBlock)},
        {kVtSetTexture, reinterpret_cast<void *>(&hkSetTexture), reinterpret_cast<void **>(&oSetTexture)},
        {kVtSetTextureStageState, reinterpret_cast<void *>(&hkSetTextureStageState), reinterpret_cast<void **>(&oSetTextureStageState)},
        {kVtSetSamplerState, reinterpret_cast<void *>(&hkSetSamplerState), reinterpret_cast<void **>(&oSetSamplerState)},
        {kVtSetVertexShader, reinterpret_cast<void *>(&hkSetVertexShader), reinterpret_cast<void **>(&oSetVertexShader)},
        {kVtSetVertexShaderConstantF, reinterpret_cast<void *>(&hkSetVertexShaderConstantF), reinterpret_cast<void **>(&oSetVertexShaderConstantF)},
        {kVtSetPixelShader, reinterpret_cast<void *>(&hkSetPixelShader), reinterpret_cast<void **>(&oSetPixelShader)},
        {kVtSetPixelShaderConstantF, reinterpret_cast<void *>(&hkSetPixelShaderConstantF), reinterpret_cast<void **>(&oSetPixelShaderConstantF)},
        {kVtDrawPrimitive, reinterpret_cast<void *>(&hkDp), reinterpret_cast<void **>(&oDp)},
        {kVtDrawPrimitiveUP, reinterpret_cast<void *>(&hkDpUp), reinterpret_cast<void **>(&oDpUp)},
        {kVtDrawIndexedPrimitiveUP, reinterpret_cast<void *>(&hkDipUp), reinterpret_cast<void **>(&oDipUp)},
        {kVtPresent, reinterpret_cast<void *>(&hkPresent), reinterpret_cast<void **>(&oPresent)},
        {kVtCreateStateBlock, reinterpret_cast<void *>(&hkCreateSb), reinterpret_cast<void **>(&oCreateSb)},
        {kVtGetVertexShader, reinterpret_cast<void *>(&hkGetVs), reinterpret_cast<void **>(&oGetVs)},
        {kVtGetPixelShader, reinterpret_cast<void *>(&hkGetPs), reinterpret_cast<void **>(&oGetPs)},
        {kVtGetTransform, reinterpret_cast<void *>(&hkGetXf), reinterpret_cast<void **>(&oGetXf)},
    };
    std::vector<std::pair<uintptr_t, void *>> done;   // (taken away again, our links alone, if one fails)
    auto undo = [&] { for (auto &[a, h] : done) HookRemoveLink(nullptr, a, h); };
    for (const S &s : sites) {
        const auto at = reinterpret_cast<uintptr_t>(vt[s.slot]);
        std::string err;
        if (!HookAdd(nullptr, at, s.detour, s.orig, &err)) {
            g_hookWhy = "slot " + std::to_string(s.slot) + ": " + err;
            undo();
            return false;
        }
        done.push_back({at, s.detour});
    }
    // a state block's Apply: the class's, from one made for the purpose (the game's are of the same class)
    IDirect3DStateBlock9 *sb = nullptr;
    if (SUCCEEDED(d->CreateStateBlock(D3DSBT_PIXELSTATE, &sb)) && sb) {
        std::string err;
        const auto at = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(sb))[kVtSbApply]);
        const auto cap = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(sb))[kVtSbCapture]);
        if (!HookAdd(nullptr, at, reinterpret_cast<void *>(&hkSbApply), reinterpret_cast<void **>(&oSbApply), &err) ||
            !HookAdd(nullptr, cap, reinterpret_cast<void *>(&hkSbCapture), reinterpret_cast<void **>(&oSbCapture), &err)) {
            g_hookWhy = "a state block's Apply: " + err;
            sb->Release();
            undo();
            return false;
        }
        sb->Release();
    } else {
        g_hookWhy = "no state block made: a state block's Apply not followed";
        undo();
        return false;
    }
    return true;
}

const char *kWhyNames[5] = {"frame", "state_block", "reset", "other_thread", "other_device"};

void SendKept(IDirect3DDevice9 *d) {
    const unsigned p = g_xfPending;
    g_xfPending = 0;
    if (p & 1) oSetTransform(d, D3DTS_WORLD, &g_m.world);
    if (p & 2) oSetTransform(d, D3DTS_VIEW, &g_m.view);
    if (p & 4) oSetTransform(d, D3DTS_PROJECTION, &g_m.proj);
}

void GiveBack(IDirect3DDevice9 *d) {
    if (g_xfPending) SendKept(d);   // (the fixed pipeline's transforms first)
    g_lending = true;
    if (g_lentVs) { d->SetVertexShader(g_gameVs); g_shaderSets++; }   // (the whole chain: prof.counters counts them)
    if (g_lentPs) { d->SetPixelShader(g_gamePs); g_shaderSets++; }
    g_lending = false;
    g_m.vs = g_gameVs;
    g_m.ps = g_gamePs;
    g_lentVs = g_lentPs = false;
    g_returns++;
}

} // namespace

bool MirrorLend(IDirect3DDevice9 *d, IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps) {
    if (!MirrorOf(d)) return false;
    IDirect3DVertexShader9 *const wantVs = vs ? vs : g_gameVs;   // (a stage the rule leaves: the game's)
    IDirect3DPixelShader9 *const wantPs = ps ? ps : g_gamePs;
    g_lending = true;
    if (g_m.vs != wantVs) { d->SetVertexShader(wantVs); g_m.vs = wantVs; g_shaderSets++; }
    if (g_m.ps != wantPs) { d->SetPixelShader(wantPs); g_m.ps = wantPs; g_shaderSets++; }
    g_lending = false;
    g_lentVs = wantVs != g_gameVs;
    g_lentPs = wantPs != g_gamePs;
    g_lendVs = wantVs;
    g_lendPs = wantPs;
    g_lends++;
    return true;
}

void MirrorGiveBack(IDirect3DDevice9 *d) {
    if (!d) d = g_dev;
    if (d && (g_lentVs || g_lentPs) && Live(d)) GiveBack(d);
}

void MirrorUse(bool on) {
    g_users += on ? 1 : -1;
    if (g_users < 0) g_users = 0;
    if (!g_users) { g_valid = false; g_dev = nullptr; }
}

const D3dMirror *MirrorOf(IDirect3DDevice9 *d) {
    if (!g_users || !d || g_hookFailed) return nullptr;
    if (GetCurrentThreadId() != MainThread()) return nullptr;
    if (!g_hooked) {
        if (!Hook(d)) { g_hookFailed = true; HWARN("d3d mirror: not made (%s): the device asked at each draw", g_hookWhy.c_str()); return nullptr; }
        g_hooked = true;
        HLOG("d3d mirror: the device's Set* calls followed");
    }
    if (g_valid && d != g_dev) Invalidate(kWhyDevice);
    if (!g_valid) Read(d);
    else if (g_vpStale) { d->GetViewport(&g_m.viewport); g_vpStale = false; Changed(); }
    return &g_m;
}

void MirrorConstants(IDirect3DDevice9 *d, bool pixel, UINT start, const float *v, UINT count, const uint32_t *reads) {
    const D3dMirror *m = MirrorOf(d);
    const UINT regs = pixel ? 224 : 256;
    if (!m || start + count > regs) {
        if (pixel) d->SetPixelShaderConstantF(start, v, count);
        else d->SetVertexShaderConstantF(start, v, count);
        return;
    }
    const float(*held)[4] = pixel ? m->psc : m->vsc;
    // the registers read (their bits walked), each compared as its 16 bytes (a -0 written over a 0 is written); a run
    // of changed ones written in one call
    const UINT end = start + count;
    UINT runFrom = 0, runTo = 0;   // ([from, to) of the run being gathered; empty when equal)
    auto flush = [&] {
        if (runTo > runFrom) {
            g_ownWrite = true;
            if (pixel) d->SetPixelShaderConstantF(runFrom, v + (runFrom - start) * 4, runTo - runFrom);   // (the hook keeps it)
            else d->SetVertexShaderConstantF(runFrom, v + (runFrom - start) * 4, runTo - runFrom);
            g_ownWrite = false;
        }
        runFrom = runTo = 0;
    };
    for (UINT w = start >> 5; w <= (end - 1) >> 5; w++) {
        uint32_t bits = reads ? reads[w] : ~0u;
        if (w == start >> 5) bits &= ~0u << (start & 31);
        if (w == (end - 1) >> 5 && (end & 31)) bits &= ~0u >> (32 - (end & 31));
        while (bits) {
            unsigned long b;
            _BitScanForward(&b, bits);
            bits &= bits - 1;
            const UINT r = w * 32 + b;
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(held[r]));
            const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + (r - start) * 4));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)) == 0xFFFF) continue;
            if (runTo && r <= runTo + 8) runTo = r + 1;   // (a gap of a few registers written along: one call, not two)
            else { flush(); runFrom = r; runTo = r + 1; }
        }
    }
    flush();
}

void MirrorFrame() { Invalidate(kWhyFrame); }
uint32_t MirrorConstSerial() { return g_constSerial; }

void MirrorCheckNext(int draws) {
    g_checkLeft = draws;
    g_checked = g_mismatchDraws = 0;
    g_mismatches.clear();
}

void MirrorCheckDraw(IDirect3DDevice9 *d) {
    if (g_checkLeft <= 0) return;
    const D3dMirror *m = MirrorOf(d);
    if (!m) return;
    g_checkLeft--;
    g_checked++;
    bool any = false;
    auto miss = [&](const std::string &what) {
        any = true;
        if (g_mismatches.size() < 40 && std::find(g_mismatches.begin(), g_mismatches.end(), what) == g_mismatches.end())
            g_mismatches.push_back(what);
    };
    D3DMATRIX t{};
    d->GetTransform(D3DTS_WORLD, &t); if (memcmp(&t, &m->world, sizeof t)) miss("world");
    d->GetTransform(D3DTS_VIEW, &t); if (memcmp(&t, &m->view, sizeof t)) miss("view");
    d->GetTransform(D3DTS_PROJECTION, &t); if (memcmp(&t, &m->proj, sizeof t)) miss("projection");
    for (DWORD s = 1; s < 256; s++) {
        DWORD v = 0;
        if (SUCCEEDED(d->GetRenderState(static_cast<D3DRENDERSTATETYPE>(s), &v)) && v != m->rs[s]) miss("render state " + std::to_string(s));
    }
    for (DWORD i = 0; i < 8; i++) {
        for (DWORD s = 1; s < 33; s++) {
            DWORD v = 0;
            if (SUCCEEDED(d->GetTextureStageState(i, static_cast<D3DTEXTURESTAGESTATETYPE>(s), &v)) && v != m->tss[i][s])
                miss("stage " + std::to_string(i) + " state " + std::to_string(s));
        }
        IDirect3DBaseTexture9 *tex = nullptr;
        d->GetTexture(i, &tex);
        if (tex != m->tex[i]) miss("texture " + std::to_string(i));
        if (tex) tex->Release();
    }
    for (DWORD i = 0; i < 4; i++)
        for (DWORD s = 1; s < 14; s++) {
            DWORD v = 0;
            if (SUCCEEDED(d->GetSamplerState(i, static_cast<D3DSAMPLERSTATETYPE>(s), &v)) && v != m->samp[i][s])
                miss("sampler " + std::to_string(i) + " state " + std::to_string(s));
        }
    D3DMATERIAL9 mat{};
    d->GetMaterial(&mat);
    if (memcmp(&mat, &m->material, sizeof mat)) miss("material");
    for (DWORD i = 0; i < 8; i++) {
        BOOL on = FALSE;
        d->GetLightEnable(i, &on);
        if ((on ? TRUE : FALSE) != m->lightOn[i]) miss("light " + std::to_string(i) + " enable");
        D3DLIGHT9 l{};
        if (on && SUCCEEDED(d->GetLight(i, &l)) && memcmp(&l, &m->light[i], sizeof l)) miss("light " + std::to_string(i));
    }
    D3DVIEWPORT9 vp{};
    d->GetViewport(&vp);
    if (memcmp(&vp, &m->viewport, sizeof vp)) miss("viewport");
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    if (vs != m->vs) miss("vertex shader");
    if (ps != m->ps) miss("pixel shader");
    if (vs) vs->Release();
    if (ps) ps->Release();
    static float c[256][4];
    d->GetVertexShaderConstantF(0, &c[0][0], 256);
    for (int r = 0; r < 256; r++) if (memcmp(c[r], m->vsc[r], 16)) { miss("vertex constant c" + std::to_string(r)); break; }
    d->GetPixelShaderConstantF(0, &c[0][0], 224);
    for (int r = 0; r < 224; r++) if (memcmp(c[r], m->psc[r], 16)) { miss("pixel constant c" + std::to_string(r)); break; }
    if (any) g_mismatchDraws++;
}

std::string MirrorStateJson() {
    json why = json::object();
    for (int i = 0; i < 5; i++) why[kWhyNames[i]] = g_why[i];
    json out = {{"users", g_users}, {"hooked", g_hooked}, {"valid", g_valid}, {"reads", g_reads}, {"invalidated", why},
                {"checked", g_checked}, {"check_left", g_checkLeft}, {"mismatch_draws", g_mismatchDraws},
                {"mismatches", g_mismatches},
                {"lent", {{"draws", g_lends}, {"given_back", g_returns}, {"shaders_set", g_shaderSets}, {"transforms_kept", g_xfKept},
                          {"now", g_lentVs || g_lentPs}}}};
    if (g_hookFailed) out["why"] = g_hookWhy;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
