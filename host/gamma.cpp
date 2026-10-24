// The game's brightness in a window on DXVK. The game sets its brightness (Options' slider) as the device's gamma ramp
// (Direct3D 8 SetGammaRamp, through a d3d8to9 to Direct3D 9's). Direct3D 9 applies a gamma ramp in an exclusive full
// screen only, and so does DXVK (D3D9SwapChainEx::SetGammaRamp: its blitter takes the ramp only when not windowed);
// dxwrapper drew it itself in a window (WindowModeGammaShader). Where the game's Direct3D 9 is DXVK's (Ultimate ASI
// Loader's d3d8to9 over DXVK's d3d9.dll, the dlss_nr pack's layout and Mafia Fusion Fix's, no dxwrapper) the loader
// keeps the ramp the game sets and, in a window, draws it over the back buffer at each Present: the frame copied, then
// each channel looked up in a 256-texel table of the ramp (bridge9.hlsl PsGamma), before the screenshots and the
// showing take the frame, so they see what the player sees. Nothing when the ramp is the identity, in a full screen,
// or under another Direct3D 9 (dxwrapper's has its own way).
#include "host.h"
#include <d3d9.h>
#include <cmath>
#include "gfx_vk.h"
#include "shaders/VsQuad.h"
#include "shaders/PsGamma.h"

namespace host {
namespace {

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

using SetGammaRampFn = void(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, const D3DGAMMARAMP *);
constexpr int kVtSetGammaRamp = 21;   // (IDirect3DDevice9's vtable, d3d9.h's order)
SetGammaRampFn oSetGammaRamp;
bool g_hooked;
IDirect3DDevice9 *g_dev;              // DXVK's device the ramp is drawn on (not held: the game's)
D3DGAMMARAMP g_ramp;
bool g_set, g_identity = true, g_lutStale;
std::string g_state = "not DXVK's Direct3D 9 (or no device yet)";
unsigned g_frames;

IDirect3DVertexShader9 *g_vs;
IDirect3DPixelShader9 *g_ps;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DVertexBuffer9 *g_quad;
IDirect3DTexture9 *g_lut;             // 256 x 1, A16B16G16R16 (managed): the ramp
IDirect3DTexture9 *g_copy;            // the back buffer's copy (render target)
IDirect3DSurface9 *g_copySurf;
UINT g_cw, g_ch;
D3DFORMAT g_cf;
IDirect3DStateBlock9 *g_block;        // the device's whole state around the draw
bool g_failed;

void STDMETHODCALLTYPE hkSetGammaRamp(IDirect3DDevice9 *d, UINT swapChain, DWORD flags, const D3DGAMMARAMP *r) {
    if (r && d == g_dev && swapChain == 0) {
        g_ramp = *r;
        g_set = true;
        g_identity = true;
        for (int i = 0; i < 256 && g_identity; i++) {
            const int want = i * 257;
            g_identity = std::abs(r->red[i] - want) <= 256 && std::abs(r->green[i] - want) <= 256 && std::abs(r->blue[i] - want) <= 256;
        }
        g_lutStale = true;
    }
    oSetGammaRamp(d, swapChain, flags, r);
}

void ReleaseSized() {
    SafeRelease(g_copySurf);
    SafeRelease(g_copy);
    SafeRelease(g_block);
    SafeRelease(g_quad);
    g_cw = g_ch = 0;
}

void ReleaseAll() {
    ReleaseSized();
    SafeRelease(g_lut);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_decl);
}

bool Fail(const std::string &why) {
    g_state = "off: " + why;
    HWARN("gamma: %s", g_state.c_str());
    g_failed = true;
    ReleaseAll();
    return false;
}

bool Make(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f) {
    if (!g_vs) {
        if (FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsQuad), &g_vs)) ||
            FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsGamma), &g_ps)))
            return Fail("its shaders");
        const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        if (FAILED(d->CreateVertexDeclaration(el, &g_decl))) return Fail("its vertex declaration");
        if (FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_MANAGED, &g_lut, nullptr))) return Fail("its table");
        g_lutStale = true;
    }
    if (g_copy && g_cw == w && g_ch == h && g_cf == f) return true;
    ReleaseSized();
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &g_copy, nullptr)) ||
        FAILED(g_copy->GetSurfaceLevel(0, &g_copySurf)))
        return Fail("the frame's copy");
    if (FAILED(d->CreateVertexBuffer(4 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_quad, nullptr))) return Fail("the quad");
    float *v = nullptr;
    if (FAILED(g_quad->Lock(0, 0, reinterpret_cast<void **>(&v), 0))) return Fail("the quad's lock");
    const float q[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};   // a triangle strip, clip space
    memcpy(v, q, sizeof q);
    g_quad->Unlock();
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_block))) return Fail("its state block");
    g_cw = w, g_ch = h, g_cf = f;
    return true;
}

void FillLut() {
    D3DLOCKED_RECT lr{};
    if (FAILED(g_lut->LockRect(0, &lr, nullptr, 0))) return;
    auto *p = static_cast<uint16_t *>(lr.pBits);   // (A16B16G16R16: red first in memory)
    for (int i = 0; i < 256; i++) {
        p[i * 4 + 0] = g_ramp.red[i];
        p[i * 4 + 1] = g_ramp.green[i];
        p[i * 4 + 2] = g_ramp.blue[i];
        p[i * 4 + 3] = 0xFFFF;
    }
    g_lut->UnlockRect(0);
    g_lutStale = false;
}

bool Windowed(IDirect3DDevice9 *d) {
    IDirect3DSwapChain9 *sc = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    bool w = SUCCEEDED(d->GetSwapChain(0, &sc)) && sc && SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.Windowed;
    SafeRelease(sc);
    return w;
}

void Draw(IDirect3DDevice9 *d) {
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || !Make(d, desc.Width, desc.Height, desc.Format)) { SafeRelease(bb); return; }
    if (g_lutStale) FillLut();
    GfxQuiet(true);   // (the loader's own drawing: not the game's to the graphics bridge's hooks)
    if (SUCCEEDED(d->StretchRect(bb, nullptr, g_copySurf, nullptr, D3DTEXF_NONE))) {
        IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
        d->GetRenderTarget(0, &rt);
        d->GetDepthStencilSurface(&ds);
        g_block->Capture();
        d->SetRenderTarget(0, bb);
        d->SetDepthStencilSurface(nullptr);
        const D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0, 1};
        d->SetViewport(&vp);
        const D3DRENDERSTATETYPE off[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE,
                                          D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
                                          D3DRS_SEPARATEALPHABLENDENABLE};
        for (D3DRENDERSTATETYPE s : off) d->SetRenderState(s, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        d->SetVertexShader(g_vs);
        d->SetPixelShader(g_ps);
        d->SetVertexDeclaration(g_decl);
        d->SetStreamSource(0, g_quad, 0, 16);
        d->SetStreamSourceFreq(0, 1);
        d->SetTexture(0, g_copy);
        d->SetTexture(1, g_lut);
        for (DWORD s = 0; s < 2; s++) {
            d->SetSamplerState(s, D3DSAMP_MINFILTER, s ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, s ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        const float c[4] = {static_cast<float>(desc.Width), static_cast<float>(desc.Height), 1.0f / desc.Width, 1.0f / desc.Height};
        d->SetPixelShaderConstantF(0, c, 1);
        d->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        g_block->Apply();
        d->SetRenderTarget(0, rt);
        d->SetDepthStencilSurface(ds);
        SafeRelease(rt);
        SafeRelease(ds);
        g_frames++;
    }
    GfxQuiet(false);
    SafeRelease(bb);
}
} // namespace

void GammaDevice(void *dev8) {
    g_dev = nullptr;
    g_failed = false;
    ReleaseAll();
    if (!dev8) return;
    IDirect3DDevice9 *d9 = nullptr;
    auto qi = (*reinterpret_cast<HRESULT(__stdcall ***)(void *, REFIID, void **)>(dev8))[0];
    if (FAILED(qi(dev8, __uuidof(IDirect3DDevice9), reinterpret_cast<void **>(&d9))) || !d9) return;
    d9->Release();   // (the game's device holds it)
    if (!gfxvk::IsDxvk(d9)) { g_state = "not DXVK's Direct3D 9 (the wrapper applies the ramp, or the device does)"; return; }
    if (GetModuleHandleW(L"dxwrapper.dll")) { g_state = "dxwrapper's (its WindowModeGammaShader draws a window's ramp)"; return; }
    if (!g_hooked) {
        std::string err;
        void *at = (*reinterpret_cast<void ***>(d9))[kVtSetGammaRamp];
        if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(at), reinterpret_cast<void *>(&hkSetGammaRamp), reinterpret_cast<void **>(&oSetGammaRamp), &err)) {
            g_state = "off: SetGammaRamp not hooked (" + err + ")";
            HWARN("gamma: %s", g_state.c_str());
            return;
        }
        g_hooked = true;
    }
    g_dev = d9;
    g_set = false, g_identity = true;
    g_state = "on (DXVK: the game's gamma ramp drawn over the frame in a window)";
    HLOG("gamma: the game's brightness drawn over the frame while it is in a window (DXVK applies a gamma ramp in a full screen only)");
}

void GammaReset() { ReleaseSized(); }   // (before the device's Reset: the copy and the quad are its default pool's)

void GammaPresent(void *dev8) {
    if (!g_dev || g_failed || !g_set || g_identity || !dev8) return;
    IDirect3DDevice9 *d9 = nullptr;
    auto qi = (*reinterpret_cast<HRESULT(__stdcall ***)(void *, REFIID, void **)>(dev8))[0];
    if (FAILED(qi(dev8, __uuidof(IDirect3DDevice9), reinterpret_cast<void **>(&d9))) || !d9) return;
    d9->Release();
    if (d9 != g_dev || !Windowed(d9)) return;
    Draw(d9);
}

std::string GammaState() {
    char b[64];
    snprintf(b, sizeof b, "; the ramp %s, %u frames drawn", !g_set ? "not set" : g_identity ? "the identity" : "the game's", g_frames);
    return g_state + (g_dev ? b : "");
}

} // namespace host
