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
#include "d3dmirror.h"
#include "host.h"
#include <algorithm>
#include <cstring>
#include <emmintrin.h>
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
};
constexpr int kVtSbApply = 5;

D3dMirror g_m;
IDirect3DDevice9 *g_dev;        // the device the mirror holds (null: nothing held)
bool g_valid, g_vpStale, g_recording, g_hooked, g_hookFailed;
int g_users;
std::string g_hookWhy;
uint64_t g_reads, g_why[5];     // reads from the device; why: frame, apply, reset, thread, device
enum { kWhyFrame, kWhyApply, kWhyReset, kWhyThread, kWhyDevice };
int g_checkLeft;
uint64_t g_checked, g_mismatchDraws;
std::vector<std::string> g_mismatches;

inline void Invalidate(int why) { if (g_valid) g_why[why]++; g_valid = false; }
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
    if (d == g_dev) Invalidate(kWhyReset);
    return oReset(d, p);
}
HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    HRESULT hr = oSetRenderTarget(d, i, s);
    if (i == 0 && Keep(d)) g_vpStale = true;   // (the viewport made the new target's whole)
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    HRESULT hr = oSetTransform(d, t, m);
    if (SUCCEEDED(hr) && m && Keep(d))
        if (D3DMATRIX *o = TransformOf(t)) *o = *m;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkMultiplyTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    HRESULT hr = oMultiplyTransform(d, t, m);
    if (SUCCEEDED(hr) && Keep(d))
        if (D3DMATRIX *o = TransformOf(t)) d->GetTransform(t, o);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    HRESULT hr = oSetViewport(d, v);
    if (SUCCEEDED(hr) && v && Keep(d)) { g_m.viewport = *v; g_vpStale = false; }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetMaterial(IDirect3DDevice9 *d, const D3DMATERIAL9 *m) {
    HRESULT hr = oSetMaterial(d, m);
    if (SUCCEEDED(hr) && m && Keep(d)) g_m.material = *m;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetLight(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) {
    HRESULT hr = oSetLight(d, i, l);
    if (SUCCEEDED(hr) && l && i < 8 && Keep(d)) g_m.light[i] = *l;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkLightEnable(IDirect3DDevice9 *d, DWORD i, BOOL on) {
    HRESULT hr = oLightEnable(d, i, on);
    if (SUCCEEDED(hr) && i < 8 && Keep(d)) g_m.lightOn[i] = on ? TRUE : FALSE;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    HRESULT hr = oSetRenderState(d, s, v);
    if (SUCCEEDED(hr) && static_cast<unsigned>(s) < 256 && Keep(d)) g_m.rs[s] = v;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9 *d, DWORD i, IDirect3DBaseTexture9 *t) {
    HRESULT hr = oSetTexture(d, i, t);
    if (SUCCEEDED(hr) && i < 8 && Keep(d)) g_m.tex[i] = t;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetTextureStageState(IDirect3DDevice9 *d, DWORD i, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    HRESULT hr = oSetTextureStageState(d, i, s, v);
    if (SUCCEEDED(hr) && i < 8 && static_cast<unsigned>(s) < 33 && Keep(d)) g_m.tss[i][s] = v;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetSamplerState(IDirect3DDevice9 *d, DWORD i, D3DSAMPLERSTATETYPE s, DWORD v) {
    HRESULT hr = oSetSamplerState(d, i, s, v);
    if (SUCCEEDED(hr) && i < 4 && static_cast<unsigned>(s) < 14 && Keep(d)) g_m.samp[i][s] = v;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    HRESULT hr = oSetVertexShader(d, s);
    if (SUCCEEDED(hr) && Keep(d)) g_m.vs = s;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    HRESULT hr = oSetPixelShader(d, s);
    if (SUCCEEDED(hr) && Keep(d)) g_m.ps = s;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9 *d, UINT start, const float *v, UINT n) {
    HRESULT hr = oSetVertexShaderConstantF(d, start, v, n);
    if (SUCCEEDED(hr) && v && start < 256 && Keep(d)) memcpy(g_m.vsc[start], v, std::min(n, 256 - start) * 16);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetPixelShaderConstantF(IDirect3DDevice9 *d, UINT start, const float *v, UINT n) {
    HRESULT hr = oSetPixelShaderConstantF(d, start, v, n);
    if (SUCCEEDED(hr) && v && start < 224 && Keep(d)) memcpy(g_m.psc[start], v, std::min(n, 224 - start) * 16);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkBeginStateBlock(IDirect3DDevice9 *d) {
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
    HRESULT hr = oSbApply(sb);
    Invalidate(kWhyApply);   // (whichever device's: the next use reads ours again)
    return hr;
}

// Everything read from the device.
void Read(IDirect3DDevice9 *d) {
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
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    m.vs = vs;
    m.ps = ps;
    if (vs) vs->Release();
    if (ps) ps->Release();
    d->GetVertexShaderConstantF(0, &m.vsc[0][0], 256);
    d->GetPixelShaderConstantF(0, &m.psc[0][0], 224);
    g_dev = d;
    g_valid = true;
    g_vpStale = false;
    g_reads++;
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
        if (!HookAdd(nullptr, at, reinterpret_cast<void *>(&hkSbApply), reinterpret_cast<void **>(&oSbApply), &err)) {
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

} // namespace

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
    else if (g_vpStale) { d->GetViewport(&g_m.viewport); g_vpStale = false; }
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
    // a register's 16 bytes compared as bits (a -0 written over a 0 is written)
    auto same = [&](UINT k) {
        const UINT r = start + k;
        if (reads && !(reads[r >> 5] >> (r & 31) & 1)) return true;   // (a register the shader does not read: left as it is)
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(held[start + k]));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + k * 4));
        return _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xFFFF;
    };
    UINT i = 0;
    while (i < count) {
        if (same(i)) { i++; continue; }
        UINT j = i + 1;
        while (j < count && !same(j)) j++;
        if (pixel) d->SetPixelShaderConstantF(start + i, v + i * 4, j - i);   // (the hook keeps what is written)
        else d->SetVertexShaderConstantF(start + i, v + i * 4, j - i);
        i = j;
    }
}

void MirrorFrame() { Invalidate(kWhyFrame); }

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
                {"mismatches", g_mismatches}};
    if (g_hookFailed) out["why"] = g_hookWhy;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
